// coop.cpp - unified Stormworks real-time co-op editor mod (v1).
//   DETECT local placements  : inline-hook the editor place-command 0x7F7EB0, read
//                              {definition name, voxel x/y/z, 3x3 rotation}.
//   SEND / RECV              : Steam P2P via the game's steam_api64.dll (ISteamNetworkingMessages,
//                              relay/SDR - no server, no ports). Peer = peer.txt SteamID64.
//   APPLY remote placements  : name -> template (getTemplateByName 0x46F380), write rotation to
//                              editor+0x14a0, forge the place-command -> renders live.
//   ECHO SUPPRESS            : g_suppress gates the detour so our own applied forges are never
//                              re-broadcast.
// NOTE (v1 coverage): 0x7F7EB0 catches SINGLE-CLICK placements. Click-DRAG uses a separate batch
//   command (0x7F7EB0 not called); the universal drag-inclusive hook is being mapped separately
//   and will be slotted into detect_worker via report_local_placement() without touching net/apply.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

// overlay (wsdraw.cpp) - compiled into THIS same DLL; coop's DllMain drives its lifecycle.
extern "C" void overlay_start(HMODULE self);
extern "C" void overlay_stop();

// ---- detour interfaces ----
extern "C" {
    // detour_detect.asm  (hook on 0x7F7EB0 - arms the apply context)
    void DetourDetect();
    unsigned long long g_cap_rcx=0, g_cap_rdx=0, g_cap_r8=0, g_cap_r9=0, g_cap_arg5=0;
    unsigned long long g_cap_flag=0, g_tramp=0, g_r9buf[16]={0};
    // detour_add.asm  (hook on 0x4BFE50 - the UNIVERSAL detector: single+drag+paste/clone)
    // lock-free SPSC ring: producer=detour (game thread), consumer=detect_worker.
    void DetourAdd();
    unsigned long long g_ring_wr=0, g_ring_rd=0, g_tramp_add=0;
    unsigned char g_ring[1024*0x80] = {0};
    unsigned long long g_ring_ptr[1024] = {0};   // component pointers (parallel to g_ring), for deferred live re-read
    // AUTO-ARM: so a click-DRAG arms the apply path with NO prior single-click.
    //   detour_dragarm.asm (0x7F3440 drag-cmd)  -> g_da_editor  (rcx = editor)
    //   detour_factory.asm (0x45EB50 factory)   -> g_da_struct  (rdx = placement struct, both paths funnel here)
    void DetourDragArm(); void DetourFactory();
    unsigned long long g_da_editor=0, g_da_flag=0, g_da_arm_needed=1;
    unsigned long long g_tramp_dragarm=0, g_tramp_factory=0;
    unsigned char g_da_struct[0x80] = {0};
    // detour_conn_add.asm (0x8B70C0 connlist_push_back): DETECT a new connection (wire).
    void DetourConnAdd();
    unsigned long long g_conn_deque=0, g_conn_add_flag=0, g_tramp_conn_add=0;
    // detour_del.asm (hook on 0x4C0940 - universal component-remove funnel = delete detection).
    // RING-BUFFER the removed voxels so a drag-eraser BURST isn't lost (256 x {int x,y,z}).
    void DetourDel();
    unsigned long long g_tramp_del=0, g_delring_wr=0, g_delring_rd=0;
    int g_delring[256*3] = {0};   // voxels read synchronously in the detour
    // detour_delarm.asm (hook on 0x804300 single-delete funnel - gates + arms delete replay)
    void DetourDelArm();
    unsigned long long g_delarm_flag=0, g_delarm_editor=0, g_delarm_rdx=0, g_delarm_r8=0, g_tramp_delarm=0;
    unsigned long long g_user_delete=0;   // set while a user delete is in progress (gates 0x4C0940)
    // shared echo-suppress: nonzero while we apply a remote forge/delete (gates ALL detours)
    unsigned long long g_suppress=0;
}

// ---- offsets (module-relative RVAs; base captured at runtime) ----
static unsigned long long g_base=0;
static unsigned char g_orig[16];        // 0x7F7EB0 stolen bytes
static unsigned char g_orig_add[16];    // 0x4BFE50 stolen bytes
static unsigned char g_orig_del[16];    // 0x4C0940 stolen bytes
static unsigned char g_orig_delarm[16]; // 0x804300 stolen bytes
static unsigned char g_orig_dragarm[16];// 0x7F3440 stolen bytes (auto-arm)
static unsigned char g_orig_factory[16];// 0x45EB50 stolen bytes (auto-arm)
static unsigned char g_orig_conn_add[16];// 0x8B70C0 stolen bytes (connection detect)
// ---- hot-reload support (unload cleanly so we can rebuild+reinject without restarting the game) ----
static volatile long g_running = 1;
static HANDLE g_threads[8]; static int g_nthreads=0;
struct HookRec { unsigned char* fn; unsigned char* orig; int steal; };
static HookRec g_hookrecs[12]; static int g_nhookrecs=0;
static void** g_iat_slot=nullptr;
static void spawn(LPTHREAD_START_ROUTINE fn){ HANDLE h=CreateThread(nullptr,0,fn,nullptr,0,nullptr); if(h&&g_nthreads<8) g_threads[g_nthreads++]=h; }
static const ULONGLONG PLACE_OFF   = 0x7F7EB0;   // editor place-cmd (arm hook + apply forge)
static const ULONGLONG ADD_OFF     = 0x4BFE50;   // universal "register placed component" (detect hook)
static const ULONGLONG DEL_OFF     = 0x4C0940;   // universal "remove one component" funnel (delete detect)
static const ULONGLONG DELARM_OFF  = 0x804300;   // single-delete funnel (gate + arm + replay target)
static const int STEAL_PLACE = 16;               // 0x7F7EB0 prologue steal
static const int STEAL_ADD   = 15;               // 0x4BFE50 prologue steal (boundary after mov [rax+8],rcx)
static const int STEAL_DEL   = 15;               // 0x4C0940 prologue steal (same shape as 0x4BFE50)
static const int STEAL_DELARM= 15;               // 0x804300 prologue steal (mov rax,rsp + 3 arg-saves)
static const ULONGLONG DRAG_OFF    = 0x7F3440;   // drag/area-fill command (rcx=editor) - auto-arm editor source
static const ULONGLONG FACTORY_OFF = 0x45EB50;   // component factory (rdx=placement struct) - auto-arm struct source
static const int STEAL_DRAG    = 15;             // mov rax,rsp + 8 pushes
static const int STEAL_FACTORY = 15;             // mov [rsp+0x10],rbx + 5 pushes + mov rbp,rsp
static const ULONGLONG CONN_ADD_OFF = 0x8B70C0;  // connlist_push_back (append a wire) - connection detect
static const int STEAL_CONN_ADD = 16;            // push rbx+sub+mov rbx,rcx+mov ecx,[rcx+10]+mov r8d,[rbx+8]
// connection record (0x30 stride) + deque header (at vehicle+0x50 = *(editor+0x13c8)+0x50)
static const int CONN_VA=0x00, CONN_VB=0x0C, CONN_TYPE=0x28, CONN_STRIDE=0x30;
static const int CONN_ELECTRIC=4;   // electric power = type 4 (confirmed in-game; the "2" tooltip was wrong)
typedef void* (*connAdd_t)(void*);  // 0x8B70C0(&deque) -> new (empty) 0x30 record; we fill voxels+type
static const ULONGLONG CONN_ERASE_OFF = 0x8B6F00; // connlist_erase_at(&deque, logical index) - disconnect forge
typedef void  (*connErase_t)(void*, unsigned);    // 0x8B6F00(&deque, idx) removes one wire
// editor cursor world position (doubles) - set to voxel*0.25 to aim a forged delete
static const ULONGLONG CUR_X = 0x12F8, CUR_Y = 0x1300, CUR_Z = 0x1308;
static const ULONGLONG GETTMPL_OFF = 0x46F380;   // getTemplateByName(registry, &{char*,u32 len})
static unsigned long long g_place_fn=0, g_add_fn=0, g_gettmpl_fn=0, g_conn_erase_fn=0;  // resolved at runtime by signature scan
static const ULONGLONG REG_ADJ     = 0xBB670;    // registry = *(editor+0x70) + 0xBB670
static const ULONGLONG ROT_OFF     = 0x14a0;     // editor+0x14a0 = 3x3 rotation ints (9 x int32)
static const ULONGLONG TMPL_NAME_PTR = 0x288;    // template+0x288 = name char*
static const ULONGLONG TMPL_NAME_LEN = 0x290;    // template+0x290 = name length (u32)
// field offsets inside the 0x4BFE50 component struct (r8 / g_addbuf)
static const int AB_VOXEL = 0x18;   // int32 x/y/z at +0x18/+0x1c/+0x20
static const int AB_ROT   = 0x30;   // 3x3 int32 rotation, 36 bytes
static const int AB_TMPL  = 0x58;   // template back-ptr (deref -> +0x288 name)

// ---- bootstrap context (captured from the FIRST local placement; needed to apply) ----
static volatile long g_armed = 0;
static unsigned long long g_editor=0, g_arg3=0;
static BYTE g_struct[0x80];

// ---- files (resolved NEXT TO THE DLL at load, so the mod is portable to any machine) ----
static HMODULE g_hmod = nullptr;
static char LOGP[MAX_PATH]  = "coopworkbench-log.txt";     // fallback (cwd) if resolution fails
static char PEERP[MAX_PATH] = "coop-peer.txt";    // put the PEER's SteamID64 here
static char CMDP[MAX_PATH]  = "coopworkbench-cmd.txt";     // write "unload" here to hot-unload the mod
static void init_paths() {
    char mod[MAX_PATH]; DWORD n = GetModuleFileNameA(g_hmod, mod, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(mod, '\\');
    if (!slash) return;
    *(slash+1) = 0;                               // mod = DLL directory with trailing backslash
    _snprintf_s(LOGP,  MAX_PATH, _TRUNCATE, "%scoopworkbench-log.txt",  mod);
    _snprintf_s(PEERP, MAX_PATH, _TRUNCATE, "%scoop-peer.txt", mod);
    _snprintf_s(CMDP,  MAX_PATH, _TRUNCATE, "%scoopworkbench-cmd.txt",  mod);
}

// ---- wire format ----
#pragma pack(push,1)
struct PlaceMsg {
    uint32_t magic;   // 'SWCP'
    uint8_t  ver;     // 1
    uint8_t  kind;    // 1 = place, 2 = delete, 3 = paint (repaint of an existing block)
    uint16_t namelen; // definition-name byte length (name bytes follow the struct)
    int32_t  x, y, z; // voxel position
    int32_t  rot[9];  // 3x3 rotation matrix (kind=3 repurposes rot[0..3] as the 4 face colors)
    int32_t  aux;     // component+0x24 = placement-struct+0x0C (non-causal; carried for compat)
    uint32_t color;   // component+0x60 (paint; 0xFFFFFFFF = default/unpainted)
    uint8_t  cat;     // template+0x40 sub-shape byte (0=normal wedge, 7=inverse) - disambiguates same-named variants
};
static const int COMP_COLOR = 0x60;   // component color (4 identical dwords at +0x60..+0x6C)
#pragma pack(pop)
static const uint32_t MAGIC = 0x50435753;  // 'S''W''C''P' little-endian
static const int MAXNAME = 96;

// ---- paint-cell cache: per-face repaint sync. A repaint of an existing block fires NO placement
// hook, so we detect it by frame-diffing the 4 face colors (comp+0x60..0x6C). We can't cheaply
// enumerate all components, so we track cells that pass through DETECT (local place) / APPLY (peer
// place); each frame we re-look-up each by voxel and compare. Seed-on-add + update-on-emit/apply
// stop placement color and the loopback echo-copy from re-triggering. (Blocks that existed before
// the mod loaded aren't tracked yet - fine for build-from-scratch co-op.) ----
static const int MAXFACE = 24;                 // max per-surface colors we sync (cube=6, most parts <=9)
static const unsigned long long COMP_EXT = 0x70, COMP_EXTCNT = 0x78;   // per-surface color array ptr + count
struct PaintCell { int x, y, z; unsigned nface; uint32_t inl[4]; uint32_t face[MAXFACE]; bool init; };
static PaintCell g_pcache[4096];
static int g_npcache = 0;
static CRITICAL_SECTION g_pcs; static bool g_pcs_init = false;
static void pcache_lock()   { if (g_pcs_init) EnterCriticalSection(&g_pcs); }
static void pcache_unlock() { if (g_pcs_init) LeaveCriticalSection(&g_pcs); }
static int  pcache_find(int x,int y,int z){ for(int i=0;i<g_npcache;i++) if(g_pcache[i].x==x&&g_pcache[i].y==y&&g_pcache[i].z==z) return i; return -1; }
static void pcache_seed(int x,int y,int z){   // ensure a cell exists; colors are baselined lazily on the first diff pass
    pcache_lock();
    int i=pcache_find(x,y,z);
    if(i<0){ if(g_npcache>=4096){ pcache_unlock(); return; } i=g_npcache++; g_pcache[i].x=x; g_pcache[i].y=y; g_pcache[i].z=z; g_pcache[i].init=false; g_pcache[i].nface=0; }
    pcache_unlock();
}
static void pcache_set(int x,int y,int z,const uint32_t inl[4],const uint32_t face[],unsigned n){  // set known colors + mark baselined
    if(n>MAXFACE) n=MAXFACE;
    pcache_lock();
    int i=pcache_find(x,y,z);
    if(i<0){ if(g_npcache>=4096){ pcache_unlock(); return; } i=g_npcache++; g_pcache[i].x=x; g_pcache[i].y=y; g_pcache[i].z=z; }
    for(int k=0;k<4;k++) g_pcache[i].inl[k]=inl[k];
    g_pcache[i].nface=n; for(unsigned k=0;k<n;k++) g_pcache[i].face[k]=face[k];
    g_pcache[i].init=true;
    pcache_unlock();
}
static void pcache_remove(int x,int y,int z){ pcache_lock(); int i=pcache_find(x,y,z); if(i>=0) g_pcache[i]=g_pcache[--g_npcache]; pcache_unlock(); }

// ---- Steam typedefs ----
typedef void*    (*ifaceAccessor_t)();
typedef uint64_t (*getSteamID_t)(void*);
typedef int      (*sendToUser_t)(void*, const void*, const void*, uint32_t, int, int);
typedef int      (*recvOnChannel_t)(void*, int, void**, int);
typedef bool     (*acceptSession_t)(void*, const void*);
typedef void     (*identClear_t)(void*);
typedef void     (*identSetID_t)(void*, uint64_t);
typedef void     (*msgRelease_t)(void*);
static void* g_net=nullptr;
static sendToUser_t    p_send=nullptr;
static recvOnChannel_t p_recv=nullptr;
static acceptSession_t p_accept=nullptr;
static msgRelease_t    p_release=nullptr;
static BYTE g_peerIdent[144];
static uint64_t g_myid=0, g_peerid=0;
static const int SEND_RELIABLE=8, SEND_UNRELIABLE=0, CHANNEL=0;
// session establishment (ISteamNetworkingMessages needs the receiver to ACCEPT the sender's
// session; we poll-accept + keepalive until the link is live, and reset on connect-fail)
typedef void (*initRelay_t)(void*);
typedef void (*closeSession_t)(void*, const void*);
static closeSession_t p_close = nullptr;
static volatile long g_session_ok = 0;
// main-thread marshaling: game imports SteamAPI_RunCallbacks (called every frame, main thread);
// its IAT slot is at module RVA 0xA92B50. We swap the slot to run our drain there.
typedef void (*runCb_t)();
static runCb_t g_orig_runcb = nullptr;
static bool g_iat_hooked = false;
static const ULONGLONG RUNCB_IAT_OFF = 0xA92B50;
// LOOPBACK SELF-TEST: when peer.txt == our own SteamID, applied (round-tripped) blocks are
// shifted +1 X so they render adjacent (connected) instead of colliding with the original.
// This exercises the FULL pipeline (serialize->Steam->deserialize->resolve->forge->render) on
// one machine. 0 for real 2-machine sessions.
// Loopback echo is placed one layer UP (+Y) rather than +X: applied blocks must stay adjacent
// to existing structure to render (the game rejects floating blocks), and +Y sits an echo line
// directly on TOP of a horizontal drag - connected and non-overlapping - whereas +X would land
// on the next block of an X/Z line (occupied). 0 for real 2-machine sessions (no shift needed).
static int g_echo_dy = 0;
// LOCAL-ECHO test mode: peer==self. Steam refuses reliable self-messaging past the first msg
// (EResult=35), so instead of the Steam wire we round-trip each detected placement through
// serialize->deserialize->apply locally. Validates the full mod pipeline (incl. drag) on one
// machine; the Steam hop itself is proven separately. 0 for real 2-machine sessions.
static bool g_localecho = false;

// ---- game fn typedefs ----
typedef void* (*getTmpl_t)(void*, void*);
typedef void* (*placeCmd_t)(void*, void*, void*, void*);
struct NameStr { const char* ptr; unsigned long long len; };  // fn reads [+0]=char*, [+8]=u32 len

static void logline(const char* fmt, ...) {
    FILE* f=nullptr; fopen_s(&f, LOGP, "a"); if(!f) return;
    SYSTEMTIME st; GetLocalTime(&st); fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fprintf(f, "\n"); fclose(f);
}
static bool safe_copy(void* d, const void* s, size_t n) {
    __try { memcpy(d, s, n); return true; } __except(EXCEPTION_EXECUTE_HANDLER){ return false; }
}
static void rotstr(char* out, size_t n, const int32_t* r) {
    _snprintf_s(out, n, _TRUNCATE, "[%d,%d,%d, %d,%d,%d, %d,%d,%d]", r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],r[8]);
}

// voxel -> component lookup (from the delete RE): comp = 0x4C17E0(0x17b420(&editor[0x13c8]), &{x,y,z})
static const ULONGLONG GETCOLL_OFF = 0x17B420;  // resolve editor+0x13c8 handle -> spatial grid collection
static const ULONGLONG LOOKUP_OFF  = 0x4C17E0;  // (collection, &int32{x,y,z}) -> owning component or null
typedef void* (*getColl_t)(void*);
typedef void* (*lookupComp_t)(void*, void*);
static void* lookup_component(unsigned long long editor, int x, int y, int z) {
    if (!editor) return nullptr;
    getColl_t gc = (getColl_t)(g_base + GETCOLL_OFF);
    lookupComp_t lc = (lookupComp_t)(g_base + LOOKUP_OFF);
    int32_t key[3] = { x, y, z };
    __try { void* coll = gc((void*)(editor + 0x13c8)); return lc(coll, key); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ======================= APPLY QUEUE (main-thread marshaling) =======================
// Applying forges the place-cmd, which reads the rotation from the shared editor+0x14a0.
// Doing that from a background thread races the game's MAIN thread (which rewrites +0x14a0 to
// the local player's own cursor rotation) -> remote rotations get clobbered under load. So we
// ENQUEUE received placements here and drain+apply them from the SteamAPI_RunCallbacks IAT hook,
// which the game calls every frame ON THE MAIN THREAD -> no writer races us. SPSC ring.
struct ApplyItem { uint8_t kind; int32_t x,y,z,rot[9],aux; uint32_t color; uint8_t cat; uint16_t namelen; char name[MAXNAME]; };
static const int AQ_SIZE = 4096, AQ_MASK = 4095;   // big enough to hold a large area-drag burst
static ApplyItem g_aq[AQ_SIZE];
static void apply_delete(int x, int y, int z);   // fwd
static void apply_paint(int x, int y, int z, const uint32_t inl[4], const uint32_t face[], unsigned nface);   // fwd (kind=3 = repaint)
static void force_remesh(unsigned long long editor, void* comp);   // fwd
static void apply_conn_add(const int va[3], const int vb[3], int type);   // fwd (kind=4 = connection add)
static void apply_conn_del(const int va[3], const int vb[3], int type);   // fwd (kind=5 = connection remove)
static void emit_disconn(const int va[3], const int vb[3], int type);     // fwd (send a disconnect)
static volatile long g_aq_wr=0, g_aq_rd=0;

static void apply_place(const PlaceMsg* m, const char* name);   // fwd

static void enqueue_apply(const PlaceMsg* m, const char* name) {
    long wr = g_aq_wr;
    if (wr - g_aq_rd >= AQ_SIZE) return;             // queue full - drop
    ApplyItem* it = &g_aq[wr & AQ_MASK];
    it->kind=m->kind; it->x=m->x; it->y=m->y; it->z=m->z; memcpy(it->rot, m->rot, 36); it->aux=m->aux; it->color=m->color; it->cat=m->cat;
    uint16_t nl = m->namelen; if (nl > MAXNAME) nl = MAXNAME;
    it->namelen = nl; memcpy(it->name, name, nl);
    MemoryBarrier();                                 // publish item before advancing wr
    g_aq_wr = wr + 1;
}
// drained on the MAIN thread from the RunCallbacks hook
static void drain_apply_queue() {
    // HOLD incoming edits until we're armed - don't drop them. A peer's edits that arrive in the window
    // between (re-)inject and our first local edit stay queued and apply the moment we arm, so nothing
    // diverges (this is what caused the "white on his screen, black on mine" single-face mismatch).
    if (!g_armed) return;
    int place_budget = 8;   // forge at most N placements per frame - a whole drag batch in one frame
                            // could leave the editor mid-update and AV the next forge; spread them out.
    while (g_aq_rd != g_aq_wr) {
        ApplyItem* it = &g_aq[g_aq_rd & AQ_MASK];
        if (it->kind == 2) { apply_delete(it->x, it->y, it->z); }
        else if (it->kind == 3) {
            uint32_t inl[4]; for(int k=0;k<4;k++) inl[k]=(uint32_t)it->rot[k];
            unsigned n=it->namelen/4; if(n>MAXFACE) n=MAXFACE;
            uint32_t face[MAXFACE]; memcpy(face, it->name, n*4);
            apply_paint(it->x, it->y, it->z, inl, face, n);
        }
        else if (it->kind == 4) {   // connection add: voxelA in x/y/z, voxelB in rot[0..2], type in rot[3]
            int va[3]={it->x,it->y,it->z}, vb[3]={it->rot[0],it->rot[1],it->rot[2]};
            apply_conn_add(va, vb, it->rot[3]);
        }
        else if (it->kind == 5) {   // connection remove: same layout as kind=4
            int va[3]={it->x,it->y,it->z}, vb[3]={it->rot[0],it->rot[1],it->rot[2]};
            apply_conn_del(va, vb, it->rot[3]);
        }
        else {
            if (place_budget-- <= 0) break;   // rest of the batch drains on the following frames
            PlaceMsg m; m.magic=MAGIC; m.ver=1; m.kind=1; m.namelen=it->namelen;
            m.x=it->x; m.y=it->y; m.z=it->z; memcpy(m.rot, it->rot, 36); m.aux=it->aux; m.color=it->color; m.cat=it->cat;
            apply_place(&m, it->name);
        }
        g_aq_rd++;
    }
}

// validate + dispatch a raw PlaceMsg buffer (shared by Steam recv and local-echo) -> queue for main thread
static void handle_place_msg(const BYTE* buf, int c) {
    if (c < (int)sizeof(PlaceMsg)) return;
    const PlaceMsg* m = (const PlaceMsg*)buf;
    if (m->magic != MAGIC || (m->kind < 1 || m->kind > 5)) return;
    if ((int)(sizeof(PlaceMsg) + m->namelen) > c) return;
    enqueue_apply(m, (const char*)(buf + sizeof(PlaceMsg)));
}

// ======================= EMIT (send or local-echo) =======================
static void emit_place(const char* name, uint16_t nl, const int32_t* xyz, const int32_t* rot, int32_t aux, uint32_t color, uint8_t cat) {
    if (nl > MAXNAME) nl = MAXNAME;
    BYTE buf[sizeof(PlaceMsg)+MAXNAME];
    PlaceMsg* m = (PlaceMsg*)buf;
    m->magic=MAGIC; m->ver=1; m->kind=1; m->namelen=nl;
    m->x=xyz[0]; m->y=xyz[1]; m->z=xyz[2];
    memcpy(m->rot, rot, 36); m->aux=aux; m->color=color; m->cat=cat;
    memcpy(buf+sizeof(PlaceMsg), name, nl);
    uint32_t len = (uint32_t)(sizeof(PlaceMsg)+nl);
    if (g_localecho) {                       // one-machine full-pipeline test: no Steam wire
        logline(">>> LOCAL-ECHO '%s' (%d,%d,%d) -> round-trip apply", name, xyz[0],xyz[1],xyz[2]);
        handle_place_msg(buf, (int)len);
        return;
    }
    if (!p_send || !g_net || !g_peerid) return;
    int rc = p_send(g_net, g_peerIdent, buf, len, SEND_RELIABLE, CHANNEL);
    logline(">>> SEND place '%s' (%d,%d,%d) -> EResult=%d %s", name, xyz[0],xyz[1],xyz[2], rc,
            rc==1?"(OK)":(rc==35?"(ConnectFailed - link not up yet)":(rc==3?"(NoConnection)":"(err)")));
    // if the session is broken (connect-fail / no-connection), reset it so keepalives re-establish
    if ((rc==35 || rc==3) && p_close) { p_close(g_net, g_peerIdent); InterlockedExchange(&g_session_ok, 0); }
}

// PAINT (kind=3): voxel in x/y/z; the 4 inline base colors (comp+0x60..0x6C) in rot[0..3]; the
// per-surface color count in aux; the `nface` per-surface colors (comp+0x70 array) as the tail bytes.
// Sent when the frame-diff sees a block's colors change (a repaint of an already-placed block).
static void emit_paint(const int32_t* xyz, const uint32_t inl[4], const uint32_t face[], unsigned nface) {
    if (nface > MAXFACE) nface = MAXFACE;
    BYTE buf[sizeof(PlaceMsg)+MAXFACE*4]; PlaceMsg* m=(PlaceMsg*)buf; memset(buf,0,sizeof(PlaceMsg));
    m->magic=MAGIC; m->ver=1; m->kind=3; m->namelen=(uint16_t)(nface*4);
    m->x=xyz[0]; m->y=xyz[1]; m->z=xyz[2];
    for (int k=0;k<4;k++) m->rot[k]=(int32_t)inl[k];
    m->aux=(int32_t)nface;
    memcpy(buf+sizeof(PlaceMsg), face, nface*4);
    uint32_t len=(uint32_t)(sizeof(PlaceMsg)+nface*4);
    if (g_localecho) {
        logline(">>> LOCAL-ECHO paint (%d,%d,%d) inl=%08X nface=%u f0=%08X", xyz[0],xyz[1],xyz[2], inl[0], nface, nface?face[0]:0);
        handle_place_msg(buf, (int)len); return;
    }
    if (!p_send || !g_net || !g_peerid) return;
    int rc = p_send(g_net, g_peerIdent, buf, len, SEND_RELIABLE, CHANNEL);
    logline(">>> SEND paint (%d,%d,%d) nface=%u EResult=%d", xyz[0],xyz[1],xyz[2], nface, rc);
    if ((rc==35 || rc==3) && p_close) { p_close(g_net, g_peerIdent); InterlockedExchange(&g_session_ok, 0); }
}

// CONNECTION add (kind=4): voxelA in x/y/z, voxelB in rot[0..2], type in rot[3]. A connection is a
// portable {voxelA, voxelB, type} - both endpoints are voxels (the blocks are already synced).
static void emit_conn(const int va[3], const int vb[3], int type) {
    BYTE buf[sizeof(PlaceMsg)]; PlaceMsg* m=(PlaceMsg*)buf; memset(buf,0,sizeof(PlaceMsg));
    m->magic=MAGIC; m->ver=1; m->kind=4; m->namelen=0;
    m->x=va[0]; m->y=va[1]; m->z=va[2];
    m->rot[0]=vb[0]; m->rot[1]=vb[1]; m->rot[2]=vb[2]; m->rot[3]=type;
    if (g_localecho) { logline(">>> LOCAL-ECHO conn (%d,%d,%d)-(%d,%d,%d) t=%d", va[0],va[1],va[2], vb[0],vb[1],vb[2], type); handle_place_msg(buf, sizeof(PlaceMsg)); return; }
    if (!p_send || !g_net || !g_peerid) return;
    int rc = p_send(g_net, g_peerIdent, buf, sizeof(PlaceMsg), SEND_RELIABLE, CHANNEL);
    logline(">>> SEND conn (%d,%d,%d)-(%d,%d,%d) t=%d EResult=%d", va[0],va[1],va[2], vb[0],vb[1],vb[2], type, rc);
    if ((rc==35 || rc==3) && p_close) { p_close(g_net, g_peerIdent); InterlockedExchange(&g_session_ok, 0); }
}

// DISCONNECT (kind=5): same wire format as kind=4, but tells the peer to REMOVE the wire.
static void emit_disconn(const int va[3], const int vb[3], int type) {
    BYTE buf[sizeof(PlaceMsg)]; PlaceMsg* m=(PlaceMsg*)buf; memset(buf,0,sizeof(PlaceMsg));
    m->magic=MAGIC; m->ver=1; m->kind=5; m->namelen=0;
    m->x=va[0]; m->y=va[1]; m->z=va[2];
    m->rot[0]=vb[0]; m->rot[1]=vb[1]; m->rot[2]=vb[2]; m->rot[3]=type;
    if (g_localecho) { logline(">>> LOCAL-ECHO disconn (%d,%d,%d)-(%d,%d,%d) t=%d", va[0],va[1],va[2], vb[0],vb[1],vb[2], type); handle_place_msg(buf, sizeof(PlaceMsg)); return; }
    if (!p_send || !g_net || !g_peerid) return;
    int rc = p_send(g_net, g_peerIdent, buf, sizeof(PlaceMsg), SEND_RELIABLE, CHANNEL);
    logline(">>> SEND disconn (%d,%d,%d)-(%d,%d,%d) t=%d EResult=%d", va[0],va[1],va[2], vb[0],vb[1],vb[2], type, rc);
    if ((rc==35 || rc==3) && p_close) { p_close(g_net, g_peerIdent); InterlockedExchange(&g_session_ok, 0); }
}

// Resolve a definition by NAME + sub-shape byte (template+0x40). Mirrors getTemplateByName's registry
// scan (count@+0x108, modulus@+0x100, seed@+0x104, buckets@+0xF8; idx=(seed+i)%modulus) but ALSO matches
// +0x40, so same-named variants (normal wedge cat40=0 vs inverse wedge cat40=7) resolve to the right one.
static void* resolve_variant(void* registry, const char* name, int len, unsigned char cat) {
    if (!registry) return nullptr;
    unsigned count=0, modulus=0, seed=0; void** buckets=nullptr;
    if (!safe_copy(&count,(char*)registry+0x108,4) || !safe_copy(&modulus,(char*)registry+0x100,4) ||
        !safe_copy(&seed,(char*)registry+0x104,4) || !safe_copy(&buckets,(char*)registry+0xF8,8)) return nullptr;
    if (!modulus || !buckets || count>200000) return nullptr;
    for (unsigned i=0;i<count;i++) {
        unsigned idx=(seed+i)%modulus;
        void* tmpl=nullptr; if (!safe_copy(&tmpl,&buckets[idx],8) || !tmpl) continue;
        char* tn=nullptr; unsigned tl=0;
        if (!safe_copy(&tn,(char*)tmpl+0x288,8) || !safe_copy(&tl,(char*)tmpl+0x290,4)) continue;
        if ((int)tl!=len || !tn) continue;
        char buf[96]; if (tl>=sizeof(buf)) continue; if (!safe_copy(buf,tn,tl)) continue; buf[tl]=0;
        if (_strnicmp(buf,name,len)!=0) continue;
        unsigned char tc=0; safe_copy(&tc,(char*)tmpl+0x40,1);
        if (tc==cat) return tmpl;
    }
    return nullptr;
}

// ======================= APPLY =======================
static void apply_place(const PlaceMsg* m, const char* name) {
    if (!g_armed) { logline("<<< recv place but NOT ARMED - place one local block first to bootstrap editor context"); return; }
    // resolve name -> template
    unsigned long long p70=0;
    if (!safe_copy(&p70, (void*)(g_editor+0x70), 8) || !p70) { logline("  editor+0x70 unreadable"); return; }
    void* registry = (void*)(p70 + REG_ADJ);
    char nm[128]; uint16_t nl=m->namelen; if(nl>127)nl=127; memcpy(nm,name,nl); nm[nl]=0;
    NameStr ns; ns.ptr=nm; ns.len=nl;
    getTmpl_t getTmpl=(getTmpl_t)g_gettmpl_fn;
    void* tmpl=nullptr;
    __try { tmpl=getTmpl(registry, &ns); } __except(EXCEPTION_EXECUTE_HANDLER){ tmpl=nullptr; }   // canonical (cat=0)
    if (!tmpl) { logline("  UNKNOWN def '%s' - skipping (missing mod / version mismatch)", nm); return; }
    // reach the exact sub-shape variant: *(canonical+0) = variant group array; group[cat] = the variant
    // (e.g. inverse wedge at group[7]). Verified by the resolved template's own +0x40 == cat.
    if (m->cat != 0) {
        __try {
            unsigned long long group=0; safe_copy(&group,(char*)tmpl+0x0,8);
            if (group) {
                unsigned long long variant=0; safe_copy(&variant,(void*)(group+(unsigned)m->cat*8),8);
                unsigned char vc=0xFF; if (variant) safe_copy(&vc,(void*)(variant+0x40),1);
                if (variant && vc==m->cat) tmpl=(void*)variant;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER){}
    }
    unsigned rescat=0xFF; safe_copy(&rescat,(char*)tmpl+0x40,4);   // cat40 of the template we resolved
    // set rotation, forge placement struct, place under echo-suppress
    safe_copy((void*)(g_editor+ROT_OFF), m->rot, 36);
    BYTE forge[0x80]; memcpy(forge, g_struct, 0x80);
    ((int*)forge)[0]=m->x + g_echo_dy; ((int*)forge)[1]=m->y; ((int*)forge)[2]=m->z;  // loopback echo shifted +X (perpendicular to Y-Z beams -> no overlap)
    ((int*)forge)[3]=m->aux;   // placement-struct+0x0C -> component+0x24 (wedge sub-shape/variant)
    placeCmd_t place=(placeCmd_t)g_place_fn;
    char rs[96]; rotstr(rs,sizeof(rs),m->rot);
    int px = m->x + g_echo_dy, py = m->y, pz = m->z;   // the voxel we actually forged at
    // CONFLICT/IDEMPOTENCY guard: if a block already occupies the target voxel (e.g. both players placed
    // on the same cell at once), skip the forge - re-placing on an occupied cell can AV the place-cmd.
    // The existing block stays (first-placed wins); a hash/snapshot resync reconciles genuine conflicts.
    if (lookup_component(g_editor, px, py, pz)) { logline("  place (%d,%d,%d) '%s' skipped - voxel already occupied", px,py,pz, nm); return; }
    g_suppress=1;
    __try {
        place((void*)g_editor, tmpl, (void*)g_arg3, forge);
        void* placed = lookup_component(g_editor, px, py, pz);   // find the block we just forged
        if (placed) {
            unsigned long long pc=(unsigned long long)placed;
            for (int k=0;k<4;k++) safe_copy((void*)(pc+COMP_COLOR+k*4), &m->color, 4);   // +0x60 base colors
            // write the +0x70 per-surface array (the RENDER color) so the block shows the SENDER's color,
            // not this peer's cursor color; then remesh to re-render.
            unsigned long long extp=0; unsigned cnt=0;
            safe_copy(&extp,(void*)(pc+COMP_EXT),8); safe_copy(&cnt,(void*)(pc+COMP_EXTCNT),4);
            if (extp>0x10000ULL && extp<0x7FFFFFFFFFFFULL && cnt>=1 && cnt<=MAXFACE) {
                for (unsigned k=0;k<cnt;k++) safe_copy((void*)(extp+k*4), &m->color, 4);
                force_remesh(g_editor, placed);
            }
        }
        pcache_seed(px,py,pz);   // track this cell for repaint sync (colors baseline on the first diff pass)
        // verify the placed block's template sub-shape matches what we wanted
        unsigned ptcat=0xFF; if (placed) { unsigned long long pt=0; if (safe_copy(&pt,(void*)((unsigned long long)placed+0x58),8) && pt) safe_copy(&ptcat,(void*)(pt+0x40),4); }
        logline("  <<< APPLIED '%s' (%d,%d,%d) want-cat=%02X resolved-tmpl-cat=%02X placed-tmpl-cat=%02X | placed=0x%p", nm, m->x,m->y,m->z, (unsigned)m->cat, rescat&0xFF, ptcat&0xFF, placed);
    }
    __except(EXCEPTION_EXECUTE_HANDLER){ logline("  apply EXCEPTION 0x%lX at (%d,%d,%d) '%s'", GetExceptionCode(), m->x,m->y,m->z, nm); }
    g_suppress=0;
}

// ======================= DELETE APPLY (remove 0x4C0940 wrapped in the place/eraser remesh) =======================
// Same-frame render: build the dirty-chunk region from the component BEFORE removing (0x4A2E40),
// then merge it into the container's dirty set AFTER (0x4A31E0) - the exact pair PLACE(0x7F803F) and
// ERASER(0x7F49E4) use. Region = 0x14 bytes {data@0, modulus@8, base@0xC, count@0x10}; free if modulus!=0.
typedef void* (*removeCmd_t)(void*, void*, void*, void*, void*, void*, void*, void*);
typedef void  (*regionBuild_t)(void*, void*);                          // 0x4A2E40(comp, region)
typedef void  (*remeshMerge_t)(void*, void*, void*, unsigned char);    // 0x4A31E0(*(editor+8), region, grid, flag)
typedef void  (*freeFn_t)(void*);                                      // 0x9B15A0(ptr)
static void apply_delete(int x, int y, int z) {
    if (!g_armed || !g_editor) { logline("<<< recv DELETE (%d,%d,%d) but not armed (place a block first)", x,y,z); return; }
    int tx = x + g_echo_dy, ty = y, tz = z;      // loopback: target the echo copy (+X)
    void* comp = lookup_component(g_editor, tx, ty, tz);
    if (!comp) { logline("  DELETE (%d,%d,%d): no component there", tx,ty,tz); return; }
    unsigned long long grid=0, ed8=0, gobj=0;
    if (!safe_copy(&grid,(void*)(g_editor+0x13c8),8) || !safe_copy(&ed8,(void*)(g_editor+8),8) || !safe_copy(&gobj,(void*)(g_editor+0x70),8) || !gobj) { logline("  DELETE: editor read fail"); return; }
    unsigned long long e38p=0; unsigned char flag=0;
    if (safe_copy(&e38p,(void*)(g_editor+0xe38),8) && e38p) safe_copy(&flag,(void*)e38p,1);
    unsigned long long out1=0, out2=0;
    void* arg8 = (void*)((unsigned long long)comp + 0xb0);
    unsigned char region[0x14]; memset(region,0,sizeof(region));
    regionBuild_t rbuild=(regionBuild_t)(g_base+0x4A2E40);
    remeshMerge_t rmerge=(remeshMerge_t)(g_base+0x4A31E0);
    freeFn_t      ffree =(freeFn_t)(g_base+0x9B15A0);
    removeCmd_t   rem   =(removeCmd_t)(g_base+DEL_OFF);
    g_suppress=1;
    __try {
        rbuild(comp, region);                                                          // region BEFORE delete
        rem((void*)grid, (void*)ed8, (void*)(gobj+0x64c8), (void*)(gobj+0xbaed8), comp, &out1, &out2, arg8);
        rmerge((void*)ed8, region, (void*)grid, flag);                                 // merge dirty chunks AFTER
        if (*(unsigned*)(region+0x08) != 0) ffree(*(void**)region);
        logline("  <<< APPLIED DELETE (%d,%d,%d) + remesh", tx,ty,tz);
    }
    __except(EXCEPTION_EXECUTE_HANDLER){ logline("  delete apply EXC 0x%lX", GetExceptionCode()); }
    g_suppress=0;
    pcache_remove(tx,ty,tz);   // stop tracking a removed cell
}
static void emit_delete(int x, int y, int z) {
    pcache_remove(x,y,z);      // stop tracking a locally-removed cell
    PlaceMsg m; memset(&m,0,sizeof(m)); m.magic=MAGIC; m.ver=1; m.kind=2; m.x=x; m.y=y; m.z=z;
    if (g_localecho) { logline(">>> LOCAL-ECHO delete (%d,%d,%d)", x,y,z); handle_place_msg((BYTE*)&m, sizeof(m)); return; }
    if (p_send && g_net && g_peerid) { int rc=p_send(g_net,g_peerIdent,&m,sizeof(m),SEND_RELIABLE,CHANNEL); logline(">>> SEND delete (%d,%d,%d) EResult=%d", x,y,z, rc); }
}

// ======================= PAINT APPLY + DETECT =======================
// force_remesh(editor, comp): the same chunk-dirty pair used around deletes, but with no removal -
// rebuilds the chunks the component touches so an in-place color change re-renders. region=0x14 bytes.
static void force_remesh(unsigned long long editor, void* comp) {
    unsigned long long grid=0, ed8=0;
    if (!safe_copy(&grid,(void*)(editor+0x13c8),8) || !safe_copy(&ed8,(void*)(editor+8),8)) return;
    unsigned long long e38p=0; unsigned char flag=0;
    if (safe_copy(&e38p,(void*)(editor+0xe38),8) && e38p) safe_copy(&flag,(void*)e38p,1);
    unsigned char region[0x14]; memset(region,0,sizeof(region));
    regionBuild_t rbuild=(regionBuild_t)(g_base+0x4A2E40);
    remeshMerge_t rmerge=(remeshMerge_t)(g_base+0x4A31E0);
    freeFn_t      ffree =(freeFn_t)(g_base+0x9B15A0);
    __try {
        rbuild(comp, region);
        rmerge((void*)ed8, region, (void*)grid, flag);
        if (*(unsigned*)(region+0x08) != 0) ffree(*(void**)region);
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
// read a component's full color state: 4 inline base colors (comp+0x60..0x6C) + the per-surface color
// array *(comp+0x70) (count @ +0x78). Per-face paint lands in this array; whole-block also hits +0x60.
static bool read_face_colors(void* comp, uint32_t inl[4], uint32_t face[MAXFACE], unsigned* nface) {
    if (!comp) return false;
    unsigned long long cb=(unsigned long long)comp;
    if (!safe_copy(inl,(void*)(cb+COMP_COLOR),16)) return false;
    unsigned long long extp=0; unsigned cnt=0;
    safe_copy(&extp,(void*)(cb+COMP_EXT),8); safe_copy(&cnt,(void*)(cb+COMP_EXTCNT),4);
    unsigned n=0;
    if (extp>0x10000ULL && extp<0x7FFFFFFFFFFFULL && cnt>=1 && cnt<=MAXFACE)
        if (safe_copy(face,(void*)extp,cnt*4)) n=cnt;
    *nface=n; return true;
}
// apply a remote repaint: write the inline base colors AND the per-surface array, then force a remesh.
static void apply_paint(int x, int y, int z, const uint32_t inl[4], const uint32_t face[], unsigned nface) {
    if (!g_armed || !g_editor) { logline("<<< recv PAINT (%d,%d,%d) but not armed", x,y,z); return; }
    int tx=x+g_echo_dy, ty=y, tz=z;
    void* comp = lookup_component(g_editor, tx, ty, tz);
    if (!comp) { logline("  PAINT (%d,%d,%d): no component there", tx,ty,tz); return; }
    unsigned long long cb=(unsigned long long)comp;
    unsigned wrote=0;
    g_suppress=1;
    __try {
        for (int k=0;k<4;k++) safe_copy((void*)(cb+COMP_COLOR+k*4), &inl[k], 4);      // inline base colors
        unsigned long long extp=0; unsigned cnt=0;
        safe_copy(&extp,(void*)(cb+COMP_EXT),8); safe_copy(&cnt,(void*)(cb+COMP_EXTCNT),4);
        unsigned w=nface; if (cnt>=1 && cnt<=MAXFACE && w>cnt) w=cnt;                  // clamp to local surface count
        if (extp>0x10000ULL && extp<0x7FFFFFFFFFFFULL) for (unsigned k=0;k<w;k++) safe_copy((void*)(extp+k*4), &face[k], 4);
        wrote=w;
        force_remesh(g_editor, comp);
        logline("  <<< APPLIED PAINT (%d,%d,%d) inl=%08X nface=%u f0=%08X", tx,ty,tz, inl[0], wrote, wrote?face[0]:0);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  paint apply EXC 0x%lX", GetExceptionCode()); }
    g_suppress=0;
    pcache_set(tx,ty,tz,inl,face,nface);   // update cache for the painted (echo) cell so the diff doesn't bounce
}
// ---- connection-remove (disconnect) support ----
// DETECT is a frame-diff of the connection deque (like paint_diff): a wire present in the previous
// scan but gone now was disconnected locally -> emit. APPLY finds the matching wire on the peer and
// erases it via 0x8B6F00. Echo-safe: apply drops the erased wire from the snapshot (conn_prev_remove)
// so the diff never re-broadcasts our own applied erase.
static const int MAX_CONN = 512;
struct ConnKey { int va[3], vb[3], type; };
static ConnKey g_conn_prev[MAX_CONN]; static int g_nconn_prev = 0;
static ConnKey g_conn_cur[MAX_CONN];
static bool g_conn_diff_init = false;
static bool conn_key_eq(const ConnKey& a, const ConnKey& b) {
    return a.type==b.type && a.va[0]==b.va[0]&&a.va[1]==b.va[1]&&a.va[2]==b.va[2]
                          && a.vb[0]==b.vb[0]&&a.vb[1]==b.vb[1]&&a.vb[2]==b.vb[2];
}
static void conn_prev_remove(const int a[3], const int b[3], int type) {
    for (int i=0;i<g_nconn_prev;i++) {
        ConnKey& k=g_conn_prev[i];
        if (k.type==type && k.va[0]==a[0]&&k.va[1]==a[1]&&k.va[2]==a[2]
                         && k.vb[0]==b[0]&&k.vb[1]==b[1]&&k.vb[2]==b[2]) {
            g_conn_prev[i]=g_conn_prev[--g_nconn_prev]; return;
        }
    }
}

// apply a remote CONNECTION add: append a logic_node_link {voxelA, voxelB, type} to the peer's flat-store
// deque at vehicle+0x50. The wire renderer iterates this deque each frame, so the wire appears. We call
// the trampoline (not the hooked 0x8B70C0) so our own detect detour never sees it -> no re-broadcast.
static void apply_conn_add(const int va[3], const int vb[3], int type) {
    if (!g_armed || !g_editor) { logline("<<< recv CONN but not armed"); return; }
    unsigned long long vehicle=0;
    if (!safe_copy(&vehicle,(void*)(g_editor+0x13C8),8) || !vehicle) { logline("  conn: no vehicle"); return; }
    void* deque = (void*)(vehicle + 0x50);
    int A[3]={va[0]+g_echo_dy, va[1], va[2]}, B[3]={vb[0]+g_echo_dy, vb[1], vb[2]};   // loopback shift
    connAdd_t addfn = (connAdd_t)(g_tramp_conn_add ? g_tramp_conn_add : g_base + CONN_ADD_OFF);
    g_suppress=1;
    __try {
        void* rec = addfn(deque);   // returns the new (empty) 0x30 record
        if (rec) {
            unsigned long long r=(unsigned long long)rec;
            safe_copy((void*)(r+CONN_VA),   A, 12);
            safe_copy((void*)(r+CONN_VB),   B, 12);
            safe_copy((void*)(r+CONN_TYPE), &type, 4);
            logline("  <<< APPLIED CONN (%d,%d,%d)-(%d,%d,%d) type=%d", A[0],A[1],A[2], B[0],B[1],B[2], type);
        } else logline("  conn: push_back returned null");
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  conn apply EXC 0x%lX", GetExceptionCode()); }
    g_suppress=0;
}
// apply a remote CONNECTION remove: scan the peer's deque for the matching {voxelA,voxelB,type} and
// erase it at its logical index via 0x8B6F00. The wire renderer iterates the deque, so it vanishes.
static void apply_conn_del(const int va[3], const int vb[3], int type) {
    if (!g_armed || !g_editor) { logline("<<< recv DISCONN but not armed"); return; }
    unsigned long long vehicle=0;
    if (!safe_copy(&vehicle,(void*)(g_editor+0x13C8),8) || !vehicle) { logline("  disconn: no vehicle"); return; }
    unsigned long long d=vehicle+0x50, data=0; unsigned cap=0, head=0, count=0;
    if (!safe_copy(&data,(void*)(d+0),8)) return;
    safe_copy(&cap,(void*)(d+8),4); safe_copy(&head,(void*)(d+0xC),4); safe_copy(&count,(void*)(d+0x10),4);
    if (!data || !cap || !count) { logline("  disconn: empty deque"); return; }
    int A[3]={va[0]+g_echo_dy, va[1], va[2]}, B[3]={vb[0]+g_echo_dy, vb[1], vb[2]};   // loopback shift (matches add)
    int found=-1;
    for (unsigned i=0;i<count;i++) {
        unsigned slot=(head+i)%cap; unsigned long long rec=data + (unsigned long long)slot*CONN_STRIDE;
        int ra[3]={0}, rb[3]={0}; unsigned rt=0;
        if (!safe_copy(ra,(void*)(rec+CONN_VA),12)) continue;
        safe_copy(rb,(void*)(rec+CONN_VB),12); safe_copy(&rt,(void*)(rec+CONN_TYPE),4);
        if ((int)rt != type) continue;
        bool fwd = ra[0]==A[0]&&ra[1]==A[1]&&ra[2]==A[2] && rb[0]==B[0]&&rb[1]==B[1]&&rb[2]==B[2];
        bool rev = ra[0]==B[0]&&ra[1]==B[1]&&ra[2]==B[2] && rb[0]==A[0]&&rb[1]==A[1]&&rb[2]==A[2];
        if (fwd||rev) { found=(int)i; break; }
    }
    if (found<0) { logline("  disconn: no match (%d,%d,%d)-(%d,%d,%d) t=%d", A[0],A[1],A[2], B[0],B[1],B[2], type); return; }
    connErase_t erasefn = (connErase_t)(g_conn_erase_fn ? g_conn_erase_fn : g_base + CONN_ERASE_OFF);
    g_suppress=1;
    __try {
        erasefn((void*)d, (unsigned)found);
        logline("  <<< APPLIED DISCONN idx=%d (%d,%d,%d)-(%d,%d,%d) t=%d", found, A[0],A[1],A[2], B[0],B[1],B[2], type);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  disconn apply EXC 0x%lX", GetExceptionCode()); }
    g_suppress=0;
    conn_prev_remove(A, B, type);   // don't let conn_diff re-broadcast this applied erase
}
// frame-diff the tracked cells' face colors; a change = a repaint -> emit. Runs on the MAIN thread
// (from my_runcb) so the voxel lookups don't race the game mutating the grid. Update-before-emit so
// each change is sent once. g_suppress skip = don't detect our own applied paints.
static void paint_diff() {
    if (!g_armed || !g_editor || g_suppress) return;
    struct Ev { int x,y,z; unsigned n; uint32_t inl[4]; uint32_t f[MAXFACE]; } evs[32]; int ne=0;
    pcache_lock();
    for (int i=0; i<g_npcache && ne<32; i++) {
        int x=g_pcache[i].x, y=g_pcache[i].y, z=g_pcache[i].z;
        void* comp = lookup_component(g_editor, x, y, z);
        if (!comp) continue;
        uint32_t inl[4], face[MAXFACE]; unsigned nf=0;
        if (!read_face_colors(comp, inl, face, &nf)) continue;
        if (!g_pcache[i].init) {   // lazy baseline: capture current colors, don't emit
            for (int k=0;k<4;k++) g_pcache[i].inl[k]=inl[k];
            g_pcache[i].nface=nf; for (unsigned k=0;k<nf;k++) g_pcache[i].face[k]=face[k];
            g_pcache[i].init=true; continue;
        }
        bool d=false;
        for (int k=0;k<4;k++) if (inl[k]!=g_pcache[i].inl[k]) d=true;
        if (nf!=g_pcache[i].nface) d=true;
        for (unsigned k=0;k<nf;k++) if (face[k]!=g_pcache[i].face[k]) d=true;
        if (d) {   // a repaint - update cache BEFORE emit (send once) and record the event
            for (int k=0;k<4;k++) g_pcache[i].inl[k]=inl[k];
            g_pcache[i].nface=nf; for (unsigned k=0;k<nf;k++) g_pcache[i].face[k]=face[k];
            evs[ne].x=x; evs[ne].y=y; evs[ne].z=z; evs[ne].n=nf;
            for (int k=0;k<4;k++) evs[ne].inl[k]=inl[k];
            for (unsigned k=0;k<nf;k++) evs[ne].f[k]=face[k];
            ne++;
        }
    }
    pcache_unlock();
    for (int i=0;i<ne;i++) { int32_t xyz[3]={evs[i].x,evs[i].y,evs[i].z}; emit_paint(xyz, evs[i].inl, evs[i].f, evs[i].n); }
}

// CONNECTION detect (diagnostic): a wire was just added. The record was empty when 0x8B70C0 fired;
// the caller has since filled it, so read the deque's TAIL element now and log {voxelA,voxelB,type}.
static void drain_connection() {
    if (!g_conn_add_flag) return;
    g_conn_add_flag = 0;
    unsigned long long deque = g_conn_deque; if (!deque) return;
    unsigned long long data=0; unsigned cap=0, head=0, count=0;
    if (!safe_copy(&data,(void*)(deque+0),8)) return;
    safe_copy(&cap,(void*)(deque+8),4); safe_copy(&head,(void*)(deque+0xC),4); safe_copy(&count,(void*)(deque+0x10),4);
    if (!data || !cap || !count) return;
    unsigned idx = (head + count - 1) % cap;                       // logical tail -> physical slot
    unsigned long long rec = data + (unsigned long long)idx*CONN_STRIDE;
    int va[3]={0}, vb[3]={0}; unsigned type=0;
    safe_copy(va,(void*)(rec+CONN_VA),12); safe_copy(vb,(void*)(rec+CONN_VB),12); safe_copy(&type,(void*)(rec+CONN_TYPE),4);
    logline("[conn] ADD voxelA=(%d,%d,%d) voxelB=(%d,%d,%d) type=%u %s", va[0],va[1],va[2], vb[0],vb[1],vb[2], type,
            type==CONN_ELECTRIC?"(ELECTRIC)":(type==1?"(default/logic)":"(other)"));
    emit_conn(va, vb, (int)type);   // send it to the peer
}

// CONNECTION remove detect: frame-diff the deque on the MAIN thread. A wire in the previous scan but
// absent now was disconnected locally -> emit. g_suppress skip so an in-flight apply doesn't get read
// mid-mutation; applied erases are already pruned from g_conn_prev by conn_prev_remove.
static void conn_diff() {
    if (!g_armed || !g_editor || g_suppress) return;
    unsigned long long vehicle=0;
    if (!safe_copy(&vehicle,(void*)(g_editor+0x13C8),8) || !vehicle) return;
    unsigned long long d=vehicle+0x50, data=0; unsigned cap=0, head=0, count=0;
    if (!safe_copy(&data,(void*)(d+0),8)) return;
    safe_copy(&cap,(void*)(d+8),4); safe_copy(&head,(void*)(d+0xC),4); safe_copy(&count,(void*)(d+0x10),4);
    if (count > (unsigned)MAX_CONN) {   // too many wires to snapshot safely -> skip (never emit spurious removals)
        static bool warned=false; if(!warned){ warned=true; logline("[conn] diff skipped: %u wires > cap %d (disconnect-sync off for this craft)", count, MAX_CONN); }
        return;
    }
    int nc=0;
    if (data && cap) {
        for (unsigned i=0;i<count && nc<MAX_CONN;i++) {
            unsigned slot=(head+i)%cap; unsigned long long rec=data + (unsigned long long)slot*CONN_STRIDE;
            ConnKey k; memset(&k,0,sizeof k);
            if (!safe_copy(k.va,(void*)(rec+CONN_VA),12)) continue;
            safe_copy(k.vb,(void*)(rec+CONN_VB),12);
            unsigned t=0; safe_copy(&t,(void*)(rec+CONN_TYPE),4); k.type=(int)t;
            g_conn_cur[nc++]=k;
        }
    }
    if (!g_conn_diff_init) {   // first scan = baseline; never emit pre-existing wires as removals
        for (int i=0;i<nc;i++) g_conn_prev[i]=g_conn_cur[i];
        g_nconn_prev=nc; g_conn_diff_init=true; return;
    }
    for (int i=0;i<g_nconn_prev;i++) {
        bool present=false;
        for (int j=0;j<nc;j++) if (conn_key_eq(g_conn_prev[i], g_conn_cur[j])) { present=true; break; }
        if (!present) {
            ConnKey& k=g_conn_prev[i];
            logline("[conn] DEL voxelA=(%d,%d,%d) voxelB=(%d,%d,%d) type=%d", k.va[0],k.va[1],k.va[2], k.vb[0],k.vb[1],k.vb[2], k.type);
            emit_disconn(k.va, k.vb, k.type);
        }
    }
    for (int i=0;i<nc;i++) g_conn_prev[i]=g_conn_cur[i];
    g_nconn_prev=nc;
}

// RunCallbacks IAT detour: run the game's original, then apply queued placements. Called every
// frame on the MAIN thread, so setting editor+0x14a0 and forging is race-free.
static unsigned g_frame=0;
static void my_runcb() {
    if (g_orig_runcb) g_orig_runcb();
    __try { drain_apply_queue(); } __except(EXCEPTION_EXECUTE_HANDLER){}
    __try { drain_connection(); } __except(EXCEPTION_EXECUTE_HANDLER){}
    __try { conn_diff(); }        __except(EXCEPTION_EXECUTE_HANDLER){}   // detect + emit wire disconnects
    if ((++g_frame % 6)==0) __try { paint_diff(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // ~10Hz repaint scan
}

// ======================= ARM (from 0x7F7EB0) =======================
// The single-click place-cmd carries the editor object, arg3, and the transient placement
// struct [r9] our apply-forge needs. Capture them ONCE (first single-click) to bootstrap the
// apply path. Processed inside detect_worker BEFORE any send so a single click arms in time
// for its own echo (fixes the arm/send race). No send from here.
static void try_arm() {
    // (1) single-click arm - 0x7F7EB0 captured editor (rcx) + arg3 (r8) + the [r9] placement struct.
    if (g_cap_flag == 1) {
        unsigned long long editor=g_cap_rcx, arg3=g_cap_r8;
        BYTE st[0x80]; memcpy(st, g_r9buf, 0x80);
        g_cap_flag = 0;
        if (!g_armed) {
            g_editor=editor; g_arg3=arg3; memcpy(g_struct, st, 0x80);
            g_da_arm_needed = 0;                       // stop the drag/factory auto-arm captures
            InterlockedExchange(&g_armed, 1);
            logline("ARMED (single-click): editor=0x%llX arg3=0x%llX - apply path ready", editor, arg3);
        }
    }
    // (2) auto-arm from a click-DRAG (no single-click needed): editor from 0x7F3440, struct from the
    // factory 0x45EB50. arg3 is editor-derived (editor+0x1588, confirmed). Validate the struct's +0x10
    // helper pointer looks real before trusting the captured struct.
    if (!g_armed && g_da_arm_needed && g_da_editor && g_da_flag) {
        unsigned long long O=0; memcpy(&O, g_da_struct + 0x10, 8);
        if (O > 0x10000ULL && O < 0x7FFFFFFFFFFFULL) {
            g_editor = g_da_editor; g_arg3 = g_da_editor + 0x1588;
            memcpy(g_struct, g_da_struct, 0x80);
            g_da_arm_needed = 0;
            InterlockedExchange(&g_armed, 1);
            logline("AUTO-ARMED (click-drag, no single-click): editor=0x%llX arg3=0x%llX", g_editor, g_arg3);
        }
    }
}

// ======================= DETECT (0x4BFE50, universal) =======================
// Fires once per placed block for single-click, drag/batch, AND paste/clone/duplicate.
// r8 (captured into g_addbuf) is a fully-populated component struct: read {name,voxel,rotation}.
static DWORD WINAPI detect_worker(LPVOID) {
    while (g_running) {
        try_arm();                                 // drain a pending single-click / auto-arm
        while (g_ring_rd < g_ring_wr && g_running) { // drain ALL enqueued placements (whole drag line); bail on unload
            try_arm();                             // keep arm current through a burst
            long idx = (long)(g_ring_rd & 0x3FF);
            BYTE ab[0x80];
            memcpy(ab, &g_ring[idx * 0x80], 0x80);
            unsigned long long compptr = g_ring_ptr[idx];
            g_ring_rd++;
            unsigned long long tmpl = *(unsigned long long*)(ab + AB_TMPL);
            unsigned long long np=0; unsigned int nlen=0;
            safe_copy(&np, (void*)(tmpl+TMPL_NAME_PTR), 8);
            safe_copy(&nlen, (void*)(tmpl+TMPL_NAME_LEN), 4);
            char name[MAXNAME]; memset(name,0,sizeof(name));
            uint16_t nl = (nlen && nlen < MAXNAME-1) ? (uint16_t)nlen : 0;
            if (np && nl) safe_copy(name, (void*)np, nl);
            if (!nl) { logline("<detect> add with no resolvable name (tmpl=0x%llX) - skip", tmpl); continue; }
            int32_t xyz[3] = { *(int*)(ab+AB_VOXEL), *(int*)(ab+AB_VOXEL+4), *(int*)(ab+AB_VOXEL+8) };
            int32_t rot[9]; memcpy(rot, ab+AB_ROT, 36);
            int32_t aux = *(int32_t*)(ab + 0x24);          // wedge sub-shape/variant (= placement-struct+0x0C)
            // The block's RENDER color is the +0x70 per-surface array (like paint), NOT +0x60 which stays
            // white. Read the real color live from the component so a placed colored block syncs its color.
            uint32_t color = *(uint32_t*)(ab + COMP_COLOR); // +0x60 fallback (usually white)
            if (compptr) {
                unsigned long long extp=0;
                if (safe_copy(&extp,(void*)(compptr+COMP_EXT),8) && extp>0x10000ULL && extp<0x7FFFFFFFFFFFULL) {
                    uint32_t c0=0; if (safe_copy(&c0,(void*)extp,4)) color=c0;   // whole-block color = array[0]
                }
            }
            // BEAM VERIFY: log the per-cell template ptr + sub-shape byte (template+0x40) so we can see
            // whether alternating beam cells use different templates / sub-shapes under the same name.
            unsigned cat=0; safe_copy(&cat,(void*)(tmpl+0x40),4);
            char rs[96]; rotstr(rs,sizeof(rs),rot);
            logline("<detect> add '%s' (%d,%d,%d) rot%s color=%08X cat=%02X", name, xyz[0],xyz[1],xyz[2], rs, color, (unsigned)(cat&0xFF));
            emit_place(name, nl, xyz, rot, aux, color, (unsigned char)(cat & 0xFF));
            pcache_seed(xyz[0],xyz[1],xyz[2]);   // track for repaint sync (colors baseline on the first diff pass)
        }
        Sleep(8);
    }
    return 0;
}

// ======================= DELETE DETECT (0x4C0940) =======================
// Fires once per removed component (arg5). Detection-only for now (log); gating + apply next.
static DWORD WINAPI del_worker(LPVOID) {
    while (g_running) {
        while (g_delring_rd < g_delring_wr && g_running) { // drain the WHOLE eraser burst; bail on unload
            long idx = (long)(g_delring_rd & 0xFF);
            int x=g_delring[idx*3+0], y=g_delring[idx*3+1], z=g_delring[idx*3+2];
            g_delring_rd++;
            logline("<DELETE> voxel=(%d,%d,%d)", x,y,z);
            emit_delete(x, y, z);
        }
        Sleep(8);
    }
    return 0;
}

// ======================= RECV + SESSION KEEPALIVE =======================
static DWORD WINAPI recv_worker(LPVOID) {
    int tick = 0;
    while (g_running) {
        if (g_net && g_peerid && !g_localecho) {
            // (1) poll-accept: accept the peer's inbound session as soon as it's pending
            if (p_accept && p_accept(g_net, g_peerIdent)) {
                if (!InterlockedExchange(&g_session_ok, 1)) logline("*** SESSION ACCEPTED with peer %llu ***", (unsigned long long)g_peerid);
            }
            // (2) keepalive ~every 1s: warm the relay so the link is ready before the first edit. We do
            // NOT tear the session down on a keepalive failure - unreliable sends blip 35/3 transiently
            // even on a healthy link, and that was causing endless close+re-accept churn (and log spam).
            // Real breaks are handled by the reliable edit-send path (emit) + the poll-accept above.
            if (p_send && (tick % 50 == 0)) {
                PlaceMsg ka; memset(&ka,0,sizeof(ka)); ka.magic=MAGIC; ka.ver=1; ka.kind=0; // kind 0 = ignored on receipt
                p_send(g_net, g_peerIdent, &ka, sizeof(ka), SEND_UNRELIABLE, CHANNEL);
            }
        }
        // (3) receive
        if (p_recv && g_net) {
            void* msgs[16];
            int n = p_recv(g_net, CHANNEL, msgs, 16);
            for (int i=0;i<n;i++) {
                void* mm=msgs[i];
                void* data=*(void**)((BYTE*)mm+0x00);
                int   sz  =*(int*)((BYTE*)mm+0x08);
                if (!InterlockedExchange(&g_session_ok, 1)) logline("*** LINK LIVE - first inbound message from peer ***");
                BYTE buf[sizeof(PlaceMsg)+256];
                int c = sz < (int)sizeof(buf) ? sz : (int)sizeof(buf);
                if (c >= (int)sizeof(PlaceMsg) && safe_copy(buf, data, c)) handle_place_msg(buf, c);
                if (p_release) p_release(mm);
            }
        }
        if (!g_iat_hooked) drain_apply_queue();   // fallback: apply on this worker thread if the main-thread hook isn't active
        tick++;
        Sleep(20);
    }
    return 0;
}

// ======================= STEAM INIT =======================
static bool steam_init() {
    HMODULE s = GetModuleHandleA("steam_api64.dll");
    if (!s) { logline("steam_api64.dll not loaded"); return false; }
    auto pUser   = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamUser_v023");
    auto pGetID  = (getSteamID_t)GetProcAddress(s, "SteamAPI_ISteamUser_GetSteamID");
    auto pNetAcc = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    p_send    = (sendToUser_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    p_recv    = (recvOnChannel_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    p_accept  = (acceptSession_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    p_release = (msgRelease_t)GetProcAddress(s, "SteamAPI_SteamNetworkingMessage_t_Release");
    p_close   = (closeSession_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    auto pClear = (identClear_t)GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_Clear");
    auto pSetID = (identSetID_t)GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    if (!pNetAcc||!p_send||!p_recv||!p_accept||!p_release||!pClear||!pSetID||!pUser||!pGetID) { logline("missing steam exports"); return false; }
    g_net = pNetAcc();
    g_myid = pGetID(pUser());
    // kick off Steam Datagram Relay so P2P-by-SteamID can route (a few seconds to warm up)
    auto pUtils = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    if (!pUtils) pUtils = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingUtils_SteamAPI_v003");
    auto pInitRelay = (initRelay_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if (pUtils && pInitRelay) { pInitRelay(pUtils()); logline("InitRelayNetworkAccess() called (SDR warming up)"); }
    else logline("InitRelayNetworkAccess not found (relay may still auto-init on first use)");
    // peer id from peer.txt (required for a real 2-machine session)
    g_peerid = 0;
    FILE* pf=nullptr; fopen_s(&pf, PEERP, "r");
    if (pf) { char t[64]={0}; if(fgets(t,sizeof(t),pf)){ uint64_t v=_strtoui64(t,nullptr,10); if(v) g_peerid=v; } fclose(pf); }
    logline("steam: net=0x%p our=%llu peer=%llu %s", g_net, (unsigned long long)g_myid, (unsigned long long)g_peerid,
            g_peerid? "" : "(NO peer.txt - send disabled until set)");
    if (g_peerid) {
        pClear(g_peerIdent); pSetID(g_peerIdent, g_peerid);
        bool acc = p_accept(g_net, g_peerIdent);
        logline("AcceptSessionWithUser(%llu) -> %d", (unsigned long long)g_peerid, acc?1:0);
        if (g_peerid == g_myid) {
            g_localecho = true; g_echo_dy = 1;
            logline("LOCAL-ECHO SELF-TEST: peer==self -> round-trip apply locally (no Steam wire), applied blocks shifted +%d Y (one layer up)", g_echo_dy);
        }
    }
    spawn(recv_worker);
    return true;
}

// ======================= SIGNATURE SCANNING (survive game updates) =======================
// Hardcoded RVAs shift on every game patch. Instead, find each hooked function by a unique
// byte-pattern from its compiled prologue (verified unique in the loaded image).
// Fall back to the known offset if a scan ever fails.
static const unsigned char SIG_PLACE[]   = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x50,0x49,0x8B,0xE8};
static const unsigned char SIG_ADD[]     = {0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x48,0x08,0x55,0x48};
static const unsigned char SIG_GETTMPL[] = {0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x58,0x48,0x8B,0xF2};
static const unsigned char SIG_CONN_ERASE[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0x79,0x10,0x48,0x8B,0xD9};

static unsigned char* scan_sig(unsigned char* base, size_t size, const unsigned char* pat, size_t patlen) {
    if (size < patlen) return nullptr;
    for (size_t i=0; i + patlen <= size; i++) {
        size_t j=0; while (j<patlen && base[i+j]==pat[j]) j++;
        if (j==patlen) return base+i;
    }
    return nullptr;
}
static size_t module_size(unsigned long long modbase) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)modbase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(modbase + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}
static unsigned long long resolve_fn(unsigned char* mod, size_t sz, const unsigned char* pat, size_t patlen, unsigned long long fb_off, const char* name) {
    unsigned char* p = scan_sig(mod, sz, pat, patlen);
    unsigned long long fb = g_base + fb_off;
    if (p) { logline("  sig[%s] -> base+0x%llX %s", name, (unsigned long long)p - g_base, ((unsigned long long)p==fb)?"(matches known offset)":"(MOVED - game updated, scan saved us)"); return (unsigned long long)p; }
    logline("  sig[%s] NOT FOUND -> fallback base+0x%llX", name, fb_off);
    return fb;
}
// resolve an imported function's IAT slot dynamically (vs a hardcoded IAT RVA)
static void** find_iat_slot(unsigned long long modbase, const char* dll, const char* func) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)modbase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(modbase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    DWORD impRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRVA) return nullptr;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(modbase + impRVA);
    for (; imp->Name; imp++) {
        const char* dname = (const char*)(modbase + imp->Name);
        if (_stricmp(dname, dll) != 0) continue;
        DWORD oftRVA = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(modbase + oftRVA);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(modbase + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(modbase + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}

// install a 14-byte-abs-jmp inline hook over an `steal`-byte relocatable prologue; returns trampoline
static ULONGLONG install_hook(ULONGLONG fn_addr, int steal, void* detour, unsigned char* orig_save) {
    unsigned char* fn = (unsigned char*)fn_addr;
    BYTE* tr = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, fn, steal);
    tr[steal]=0xFF; tr[steal+1]=0x25; *(DWORD*)(tr+steal+2)=0; *(ULONGLONG*)(tr+steal+6)=(ULONGLONG)(fn+steal);
    memcpy(orig_save, fn, steal);
    DWORD old; VirtualProtect(fn, steal, PAGE_EXECUTE_READWRITE, &old);
    fn[0]=0xFF; fn[1]=0x25; *(DWORD*)(fn+2)=0; *(ULONGLONG*)(fn+6)=(ULONGLONG)detour;
    for (int i=14;i<steal;i++) fn[i]=0x90;
    VirtualProtect(fn, steal, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, steal);
    if (g_nhookrecs < 12) { g_hookrecs[g_nhookrecs].fn=fn; g_hookrecs[g_nhookrecs].orig=orig_save; g_hookrecs[g_nhookrecs].steal=steal; g_nhookrecs++; }
    return (ULONGLONG)tr;
}

// restore all inline hooks + the IAT slot to original (for hot unload)
static void unhook_all() {
    if (g_iat_slot && g_orig_runcb) { DWORD o; if (VirtualProtect(g_iat_slot,sizeof(void*),PAGE_READWRITE,&o)) { *g_iat_slot=(void*)g_orig_runcb; VirtualProtect(g_iat_slot,sizeof(void*),o,&o); } }
    for (int i=0;i<g_nhookrecs;i++) {
        HookRec& h=g_hookrecs[i]; DWORD o;
        if (VirtualProtect(h.fn,h.steal,PAGE_EXECUTE_READWRITE,&o)) {
            memcpy(h.fn,h.orig,h.steal); VirtualProtect(h.fn,h.steal,o,&o);
            FlushInstructionCache(GetCurrentProcess(),h.fn,h.steal);
        }
    }
}
// watch coopworkbench-cmd.txt for "unload" -> restore hooks, stop threads, free the DLL (unlocks the file)
static DWORD WINAPI cmd_watcher(LPVOID) {
    while (g_running) {
        FILE* f=nullptr; fopen_s(&f, CMDP, "r");
        if (f) { char t[32]={0}; if(!fgets(t,sizeof(t),f)) t[0]=0; fclose(f); DeleteFileA(CMDP);
            if (strncmp(t,"unload",6)==0) {
                logline("UNLOAD: restoring hooks + freeing DLL for hot reload");
                overlay_stop();               // tear down the overlay first (restore its hooks, join net worker)
                unhook_all();
                logline("UNLOAD: hooks restored; stopping workers");
                InterlockedExchange(&g_running, 0);
                Sleep(300);   // let any in-flight detour/game-call return past our code
                // Wait until the worker threads PROVABLY exit before unmapping. NEVER free on a timeout:
                // a worker still executing in this DLL when FreeLibrary unmaps it = use-after-free crash.
                // Loop the wait (hanging the unload is strictly safer than crashing the game).
                if (g_nthreads > 0) {
                    DWORD wr; int spins=0;
                    do { wr = WaitForMultipleObjects(g_nthreads, g_threads, TRUE, 1000);
                         if (wr == WAIT_TIMEOUT) logline("UNLOAD: workers still running (spin %d), waiting...", ++spins);
                    } while (wr == WAIT_TIMEOUT);
                    logline("UNLOAD: all workers exited (wait -> %lu)", wr);
                }
                Sleep(50);
                logline("UNLOAD: freeing library now");
                FreeLibraryAndExitThread(g_hmod, 0);
            }
        }
        Sleep(150);
    }
    return 0;
}

// ======================= SETUP =======================
static DWORD WINAPI setup(LPVOID) {
    init_paths();
    InitializeCriticalSection(&g_pcs); g_pcs_init=true;   // paint-cache lock
    g_base = (unsigned long long)GetModuleHandleA(nullptr);
    // resolve hooked functions by signature (survives game updates); fall back to known offsets
    size_t imgsz = module_size(g_base);
    unsigned char* mod = (unsigned char*)g_base;
    logline("signature scan (module image 0x%zX bytes):", imgsz);
    g_place_fn   = resolve_fn(mod, imgsz, SIG_PLACE,   sizeof(SIG_PLACE),   PLACE_OFF,   "place-cmd");
    g_add_fn     = resolve_fn(mod, imgsz, SIG_ADD,     sizeof(SIG_ADD),     ADD_OFF,     "add-detect");
    g_gettmpl_fn = resolve_fn(mod, imgsz, SIG_GETTMPL, sizeof(SIG_GETTMPL), GETTMPL_OFF, "getTemplateByName");
    g_conn_erase_fn = resolve_fn(mod, imgsz, SIG_CONN_ERASE, sizeof(SIG_CONN_ERASE), CONN_ERASE_OFF, "conn-erase");
    g_tramp     = install_hook(g_place_fn, STEAL_PLACE, (void*)&DetourDetect, g_orig);      // arm
    g_tramp_add = install_hook(g_add_fn,   STEAL_ADD,   (void*)&DetourAdd,    g_orig_add);  // detect
    g_tramp_del = install_hook(g_base + DEL_OFF, STEAL_DEL, (void*)&DetourDel, g_orig_del); // delete detect
    g_tramp_delarm = install_hook(g_base + DELARM_OFF, STEAL_DELARM, (void*)&DetourDelArm, g_orig_delarm); // delete gate+arm
    g_tramp_dragarm = install_hook(g_base + DRAG_OFF,    STEAL_DRAG,    (void*)&DetourDragArm, g_orig_dragarm); // auto-arm: editor
    g_tramp_factory = install_hook(g_base + FACTORY_OFF, STEAL_FACTORY, (void*)&DetourFactory, g_orig_factory); // auto-arm: struct
    g_tramp_conn_add = install_hook(g_base + CONN_ADD_OFF, STEAL_CONN_ADD, (void*)&DetourConnAdd, g_orig_conn_add); // connection detect
    // IAT-hook SteamAPI_RunCallbacks (per-frame, main thread) to apply queued placements race-free.
    // Resolve the slot dynamically via the import table (falls back to the known IAT RVA).
    HMODULE sm = GetModuleHandleA("steam_api64.dll");
    void* realRunCb = sm ? (void*)GetProcAddress(sm, "SteamAPI_RunCallbacks") : nullptr;
    void** iat = find_iat_slot(g_base, "steam_api64.dll", "SteamAPI_RunCallbacks");
    if (!iat) { iat = (void**)(g_base + RUNCB_IAT_OFF); logline("  RunCallbacks IAT not found via imports -> fallback base+0x%llX", (unsigned long long)RUNCB_IAT_OFF); }
    else logline("  RunCallbacks IAT resolved via imports -> base+0x%llX", (unsigned long long)iat - g_base);
    __try {
        if (realRunCb && *iat == realRunCb) {
            g_orig_runcb = (runCb_t)*iat;
            DWORD o; VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &o);
            *iat = (void*)&my_runcb;
            VirtualProtect(iat, sizeof(void*), o, &o);
            g_iat_hooked = true; g_iat_slot = iat;
            logline("RunCallbacks IAT hooked @0x%p - remote placements apply on the MAIN thread (race-free)", iat);
        } else {
            logline("RunCallbacks IAT slot mismatch (*iat=0x%p real=0x%p) - using worker-thread apply fallback", *iat, realRunCb);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("RunCallbacks IAT hook exception - worker-thread apply fallback"); g_orig_runcb=nullptr; g_iat_hooked=false; }
    DeleteFileA(CMDP);                       // clear any stale command
    spawn(detect_worker);                     // handles both arm + detect+emit
    spawn(del_worker);                        // delete detection
    // hot-unload watcher: launched OUTSIDE g_threads on purpose. It is the thread that runs the
    // unload's WaitForMultipleObjects(g_threads, bWaitAll), so it must NOT be in that set - a thread
    // waiting on its own never-signaled handle would spin the "never free on timeout" loop forever.
    CreateThread(nullptr, 0, cmd_watcher, nullptr, 0, nullptr);
    steam_init();
    logline("coop v4 ready (session poll-accept + relay). base=0x%llX. hooks: arm=0x%llX detect=0x%llX. Auto-arms on your first edit (click OR drag); build to sync. peer=%llu",
            g_base, PLACE_OFF, ADD_OFF, (unsigned long long)g_peerid);
    overlay_start(g_hmod);                    // start the in-world partner-camera overlay (merged in)
    logline("overlay started (merged into coopworkbench.dll; F9=overlay F10=hud)");
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { g_hmod = h; DisableThreadLibraryCalls(h); CreateThread(nullptr,0,setup,nullptr,0,nullptr); }
    return TRUE;
}
