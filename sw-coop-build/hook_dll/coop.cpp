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
#include "coop_version.h"
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
    // detour_appstate.asm (0x847EE0 c_application_state_game::update): PASSIVE auto-arm - stash the app-state
    // pointer + bump a seen-counter every frame so my_runcb resolves the editor and arms with NO local edit.
    void DetourAppState();
    unsigned long long g_cap_appstate=0, g_cap_seen=0, g_tramp_appstate=0;
    // detour_interact.asm (0x789070 input-action handler): which interact key was used.
    //   action 0x13 = interact-left (Q by default), 0x14 = interact-right (E). state 1 = pressed.
    void DetourInteract();
    unsigned long long g_interact_action=0, g_interact_state=0, g_interact_seen=0, g_tramp_interact=0;
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
static unsigned char g_orig_appstate[24];// 0x847EE0 stolen bytes (18 used; padded)
static unsigned char g_orig_interact[24];// 0x789070 stolen bytes (15 used; padded)
static const ULONGLONG INTERACT_OFF = 0x789070;  // player input-action handler (E/Q capture)
static const int STEAL_INTERACT = 15;            // mov rax,rsp + 3 arg saves, before `push rbp`
static const int ACT_INTERACT_LEFT  = 0x13;      // Q by default
static const int ACT_INTERACT_RIGHT = 0x14;      // E by default
static const ULONGLONG APPUPD_OFF = 0x847EE0;   // c_application_state_game::update (passive auto-arm capture)
static const int STEAL_APPUPD = 18;             // 8 pushes + lea rbp,[rsp-1Fh] (measured clean boundary)
static const ULONGLONG VT_APPSTATE      = 0xB02A28;  // c_application_state_game vtable (identity check)
static const ULONGLONG VT_EDITOR        = 0xB44838;  // c_game_state_vehicle_editor vtable (identity check)
static const ULONGLONG OFF_EDITOR_PTR   = 0xCE0F8;   // app_state -> workbench editor
static const ULONGLONG OFF_ACTIVE_STATE = 0x350;     // app_state -> active game_state (==editor only in BUILD mode)
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
static volatile long g_have_struct = 0;   // 1 once g_struct holds a real forge template (gates apply_place forge)
// 1 while the workbench editor is the ACTIVE game state (i.e. the player is really IN the bench, not walking
// the world / in the sim). Unlike g_armed this CLEARS on exit. wsdraw.cpp reads it (same DLL, C linkage).
extern "C" volatile long g_in_bench = 0;
static int g_peer_bench[3] = {0,0,0};     // partner's build volume in voxels (from their presence beacon)
extern "C" volatile long g_cursor_selftest;   // defined in wsdraw.cpp; F8 toggles the solo cursor self-test
static void logline(const char* fmt, ...);    // fwd (defined below; used by the interact-key watcher)
static void update_workbench_prompt();        // fwd (live prompt text: CO-OP / START CO-OP / JOIN PARTNER)
// CO-OP vs SOLO entry. The game sends the same use-workbench request for either key, so we simply watch which
// key was pressed just before the bench opened and let it choose whether THIS session syncs.
// Matches the patched prompt: [E] CO-OP, [Q] SOLO. Defaults to ENABLED so a missed key never silently
// disables sync (the safe direction - you notice unwanted sync immediately, you may not notice its absence).
extern "C" volatile long g_sync_enabled = 1;
static unsigned long long g_interact_prev = 0, g_last_key = 0;
static DWORD g_last_key_time = 0;
static long g_join_mode = 0;   // 1 = we entered as the JOINER (partner was already building -> pull theirs)
static void watch_interact_key() {
    unsigned long long seen = g_interact_seen;
    if (seen == g_interact_prev) return;
    g_interact_prev = seen;
    // DIAG: log every captured interact so we can see the real action ids / states this build uses.
    logline("[key] interact: action=0x%llX state=%llu (seen=%llu)", g_interact_action, g_interact_state, seen);
    if (g_interact_state != 1) return;                                  // key-down only
    unsigned long long a = g_interact_action;
    if (a != ACT_INTERACT_LEFT && a != ACT_INTERACT_RIGHT) return;      // not an interact key
    g_last_key = a; g_last_key_time = GetTickCount();
}
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
// Seed the 26 neighbours of a just-placed cell so a PRE-EXISTING adjacent block (e.g. the manually-placed
// origin block from before injection) gets tracked and its paint syncs. Bounded by a budget; empty neighbour
// cells that never hold a component are pruned by paint_diff, so this can't bloat the cache.
static volatile long g_seed_budget = 64;
static void pcache_seed_neighbors(int x,int y,int z){
    if(g_seed_budget<=0) return;
    InterlockedDecrement(&g_seed_budget);
    for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) for(int dz=-1;dz<=1;dz++)
        if(dx||dy||dz) pcache_seed(x+dx,y+dy,z+dz);
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
static identClear_t p_identClear=nullptr;      // promoted to file scope: auto-connect needs them too
static identSetID_t p_identSetID=nullptr;
// ---- AUTO-CONNECT: ISteamFriends (resolved by GetProcAddress like everything else) ----
typedef int      (*friendCount_t)(void*, int);
typedef uint64_t (*friendByIdx_t)(void*, int, int);
typedef bool     (*friendGame_t)(void*, uint64_t, void*);
typedef bool     (*hasFriend_t)(void*, uint64_t, int);
static void* g_friends=nullptr;
static friendCount_t p_fcount=nullptr;
static friendByIdx_t p_fbyidx=nullptr;
static friendGame_t  p_fgame=nullptr;
static hasFriend_t   p_hasfriend=nullptr;
static volatile long g_manual_peer = 0;        // coop-peer.txt supplied an id -> discovery OFF, manual wins
static volatile long g_autoconnect_on = 0;     // opt-in only: set when coop-autoconnect.txt exists
static volatile long g_probe_on = 0;           // diagnostic memory scan: only when coop-probe.txt exists
static const uint32_t SW_APPID = 573090;       // Stormworks
static const int K_FRIEND_IMMEDIATE = 0x04;    // k_EFriendFlagImmediate
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
static bool peer_away();   // fwd (presence gate: TRUE only when the peer is KNOWN to be out of the bench)
static bool bench_mismatch();   // fwd (TRUE when both bench volumes are known and differ)
static bool sync_paused();      // fwd (peer_away || bench_mismatch - blocks all edit traffic)
static void bench_size_vox(int out[3]);              // fwd (build volume in voxels, editor+0xD70)
static void bench_max_voxel(const int size[3], int out[3]);   // fwd (last legal voxel per axis)
static void apply_conn_add(const int va[3], const int vb[3], int type);   // fwd (kind=4 = connection add)
static void apply_conn_del(const int va[3], const int vb[3], int type);   // fwd (kind=5 = connection remove)
static void emit_disconn(const int va[3], const int vb[3], int type);     // fwd (send a disconnect)
static void apply_prop(const int xyz[3], int offset, uint32_t value);     // fwd (kind=6 = numeric property)
static void apply_name(const int xyz[3], const char* s, int len);         // fwd (kind=7 = name/string)
static void prop_track(int x, int y, int z, const char* def);             // fwd (watch a component's properties, gated by def)
static volatile long g_aq_wr=0, g_aq_rd=0;

static void apply_place(const PlaceMsg* m, const char* name);   // fwd

static void enqueue_apply(const PlaceMsg* m, const char* name) {
    long wr = g_aq_wr;
    if (wr - g_aq_rd >= AQ_SIZE) {                   // queue full - drop (F7 pull is the recovery)
        static DWORD s_last_warn = 0; DWORD now = GetTickCount();
        if (now - s_last_warn > 5000) { s_last_warn = now;
            logline("!!! apply queue FULL (%d) - dropping peer edits. If you were out of the bench a long time, press F7 to resync.", AQ_SIZE); }
        return;
    }
    ApplyItem* it = &g_aq[wr & AQ_MASK];
    it->kind=m->kind; it->x=m->x; it->y=m->y; it->z=m->z; memcpy(it->rot, m->rot, 36); it->aux=m->aux; it->color=m->color; it->cat=m->cat;
    uint16_t nl = m->namelen; if (nl > MAXNAME) nl = MAXNAME;
    it->namelen = nl; memcpy(it->name, name, nl);
    MemoryBarrier();                                 // publish item before advancing wr
    g_aq_wr = wr + 1;
}
// drained on the MAIN thread from the RunCallbacks hook
static void drain_apply_queue() {
    // HOLD incoming edits until we're armed AND actually in the bench - don't drop them. Two cases:
    //  (a) between (re-)inject and arming, and
    //  (b) while the player is OUT of the workbench (walking the world / in the sim). g_armed is additive and
    //      never clears, so without the g_in_bench gate we would forge blocks through a STALE g_editor while
    //      the editor is not the active state - writing into memory that may have been torn down/reused.
    // Queued edits flush the moment the player walks back in, so "reopen the bench and the partner's work is
    // already there" still holds - it just applies on re-entry instead of behind the player's back.
    if (!g_armed || !g_in_bench) return;
    if (bench_mismatch()) {   // HARD BLOCK: never apply edits from a differently-sized bench
        static DWORD s_lw=0; DWORD nowt=GetTickCount();
        if (nowt-s_lw>5000) { s_lw=nowt;
            logline("!!! SYNC BLOCKED - partner is at a different-size workbench (yours vs %dx%dx%d vox). Both players must use the SAME bench type.",
                    g_peer_bench[0],g_peer_bench[1],g_peer_bench[2]); }
        g_aq_rd = g_aq_wr;    // discard rather than accumulate a flood to replay later
        return;
    }
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
        else if (it->kind == 6) {   // numeric property: offset in rot[0], value in rot[1]
            int xyz[3]={it->x,it->y,it->z};
            apply_prop(xyz, it->rot[0], (uint32_t)it->rot[1]);
        }
        else if (it->kind == 7) {   // name/string: bytes in the name payload
            int xyz[3]={it->x,it->y,it->z};
            apply_name(xyz, it->name, it->namelen);
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
    if (m->magic != MAGIC || (m->kind < 1 || m->kind > 7)) return;
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
    if (!p_send || !g_net || !g_peerid || sync_paused()) return;   // partner away (resyncs on entry) or wrong bench
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
    if (!p_send || !g_net || !g_peerid || sync_paused()) return;   // partner away (resyncs on entry) or wrong bench
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
    if (!p_send || !g_net || !g_peerid || sync_paused()) return;   // partner away (resyncs on entry) or wrong bench
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
    if (!p_send || !g_net || !g_peerid || sync_paused()) return;   // partner away (resyncs on entry) or wrong bench
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
    if (!g_armed || !g_have_struct) { logline("<<< recv place but no forge template yet - make ONE local placement to capture it (delete/paint/conn/pull work without it)"); return; }
    // BENCH BOUNDS GUARD: a partner at a LARGER bench can legally place blocks that do not fit our volume.
    // Forging those anyway is what leaves the stuck out-of-bounds warning markers, so drop them with a clear
    // message instead. Last legal voxel per axis = (size-3)/2 (the game's own rule; matches measurements).
    { int sz[3], mx[3]; bench_size_vox(sz);
      if (sz[0]) { bench_max_voxel(sz, mx);
          int ax = m->x<0?-m->x:m->x, ay = m->y<0?-m->y:m->y, az = m->z<0?-m->z:m->z;
          if (ax>mx[0] || ay>mx[1] || az>mx[2]) {
              static DWORD s_lw=0; DWORD nowt=GetTickCount();
              if (nowt-s_lw>3000) { s_lw=nowt;
                  logline("<<< OUT OF BOUNDS (%d,%d,%d) dropped - this bench allows +/-%d,%d,%d. Partner is at a bigger bench.",
                          m->x,m->y,m->z, mx[0],mx[1],mx[2]); }
              return;
          } } }
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
            prop_track(px, py, pz, name);   // watch the applied block's properties too (bidirectional)
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
        pcache_seed_neighbors(px,py,pz);   // pick up pre-existing adjacent blocks (origin block etc.)
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
    if (p_send && g_net && g_peerid && !sync_paused()) { int rc=p_send(g_net,g_peerIdent,&m,sizeof(m),SEND_RELIABLE,CHANNEL); logline(">>> SEND delete (%d,%d,%d) EResult=%d", x,y,z, rc); }
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
        if (!comp) { if (!g_pcache[i].init) { g_pcache[i]=g_pcache[--g_npcache]; i--; } continue; }   // prune empty seeded neighbours
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
// ---- generic component-property sync (kind=6 numeric, kind=7 name/string) ----
// The component memory layout is IDENTICAL on both machines, so we replicate any changed non-pointer
// dword byte-for-byte without knowing what it means - covering every numeric slider/dropdown/keybind
// with zero per-property work. Strings (names) can't be byte-copied (heap pointer) so they get one
// special case. Echo-safe: apply updates our own snapshot so the diff never re-broadcasts an applied change.
// SAFE, TARGETED property sync. The generic raw-memory diff crashed both machines (it flooded Steam
// and wrote pointers/strings/runtime-state into the peer), so we sync only a WHITELIST of confirmed,
// value-validated offsets. Grow the list as each offset is verified with the probe. Strings/names
// need the game's own setter (a forged command, like place/paint) - NOT raw dwords.
// SAFE, PER-TYPE property sync. An offset means DIFFERENT things per part (+0x2A8 is battery charge but
// a name-string byte on other parts, which leaked as UTF-16 garbage). So each rule is gated by the part's
// definition NAME and value-validated. Grow PROP_RULES as each part's offsets are probed.
struct PropRule { const char* def; int offset; };
static const PropRule PROP_RULES[] = {
    { "battery", 0x2A8 },      // charge % (float 0..1)
    // grow after probing each part: { "linear_track", 0x??? }, { "pivot", 0x??? }, ...
};
static const int NRULES = (int)(sizeof(PROP_RULES)/sizeof(PROP_RULES[0]));
static const int PROPCELLS=512;
struct PropCell { int x,y,z; bool used, init; char def[24]; uint32_t snap[NRULES>0?NRULES:1]; int last_namelen; char last_name[64]; };
static PropCell g_prop[PROPCELLS]; static unsigned g_prop_wr=0;
static void prop_track(int x,int y,int z,const char* def){
    PropCell* e=nullptr;
    for(int i=0;i<PROPCELLS;i++) if(g_prop[i].used&&g_prop[i].x==x&&g_prop[i].y==y&&g_prop[i].z==z){ e=&g_prop[i]; break; }
    if(!e){ e=&g_prop[g_prop_wr++ % PROPCELLS]; e->x=x;e->y=y;e->z=z;e->used=true;e->init=false;e->last_namelen=-1;e->last_name[0]=0;e->def[0]=0; }
    if(def && def[0] && !e->def[0]){ int n=0; while(def[n]&&n<23){ e->def[n]=def[n]; n++; } e->def[n]=0; }   // remember the part type (once)
}
static PropCell* prop_find(int x,int y,int z){ for(int i=0;i<PROPCELLS;i++) if(g_prop[i].used&&g_prop[i].x==x&&g_prop[i].y==y&&g_prop[i].z==z) return &g_prop[i]; return nullptr; }
static bool prop_off_known(int offset){ for(int i=0;i<NRULES;i++) if(PROP_RULES[i].offset==offset) return true; return false; }
static bool prop_val_ok(int offset,uint32_t v){          // per-offset sanity so we never sync garbage
    if(offset==0x2A8){                                   // charge: exactly 0.0, or a NORMAL float in (0,1] (reject denormal junk)
        if(v==0) return true;
        if((v & 0x7F800000u)==0) return false;           // denormal exponent (e.g. UTF-16 chars) -> not charge
        float f; memcpy(&f,&v,4); return f>0.0f && f<=1.0f;
    }
    return false;
}
static void emit_prop(const int xyz[3],int offset,uint32_t value){
    BYTE buf[sizeof(PlaceMsg)]; PlaceMsg* m=(PlaceMsg*)buf; memset(buf,0,sizeof(PlaceMsg));
    m->magic=MAGIC;m->ver=1;m->kind=6;m->namelen=0; m->x=xyz[0];m->y=xyz[1];m->z=xyz[2]; m->rot[0]=offset; m->rot[1]=(int32_t)value;
    if(g_localecho){ handle_place_msg(buf,sizeof(PlaceMsg)); return; }
    if(!p_send||!g_net||!g_peerid) return;
    int rc=p_send(g_net,g_peerIdent,buf,sizeof(PlaceMsg),SEND_RELIABLE,CHANNEL);
    logline(">>> SEND prop (%d,%d,%d) +0x%X=%08X rc=%d", xyz[0],xyz[1],xyz[2], offset, value, rc);
}
static void emit_name(const int xyz[3],const char* s,int len){
    if(len<0)len=0; if(len>MAXNAME)len=MAXNAME;
    BYTE buf[sizeof(PlaceMsg)+MAXNAME]; PlaceMsg* m=(PlaceMsg*)buf; memset(buf,0,sizeof(PlaceMsg));
    m->magic=MAGIC;m->ver=1;m->kind=7;m->namelen=(uint16_t)len; m->x=xyz[0];m->y=xyz[1];m->z=xyz[2];
    memcpy(buf+sizeof(PlaceMsg),s,len);
    if(g_localecho){ handle_place_msg(buf,sizeof(PlaceMsg)+len); return; }
    if(!p_send||!g_net||!g_peerid) return;
    int rc=p_send(g_net,g_peerIdent,buf,sizeof(PlaceMsg)+len,SEND_RELIABLE,CHANNEL);
    logline(">>> SEND name (%d,%d,%d) \"%.*s\" rc=%d", xyz[0],xyz[1],xyz[2], len, s, rc);
}
static void apply_prop(const int xyz[3],int offset,uint32_t value){
    if(!g_armed||!g_editor) return;
    if(!prop_off_known(offset)||!prop_val_ok(offset,value)){ logline("  prop: reject off=0x%X val=%08X",offset,value); return; }
    int px=xyz[0]+g_echo_dy,py=xyz[1],pz=xyz[2];
    void* comp=lookup_component(g_editor,px,py,pz);
    if(!comp){ logline("  prop: no comp (%d,%d,%d)",px,py,pz); return; }
    g_suppress=1;
    __try {
        safe_copy((void*)((unsigned long long)comp+offset),&value,4);
        logline("  <<< APPLIED prop (%d,%d,%d) +0x%X=%08X",px,py,pz,offset,value);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  prop apply EXC 0x%lX",GetExceptionCode()); }
    g_suppress=0;
    PropCell* c=prop_find(px,py,pz); if(c&&c->init){ for(int i=0;i<NRULES;i++) if(PROP_RULES[i].offset==offset){ c->snap[i]=value; break; } }
}
static void apply_name(const int xyz[3],const char* s,int len){
    if(!g_armed||!g_editor) return; if(len<0)len=0; if(len>MAXNAME)len=MAXNAME;
    int px=xyz[0]+g_echo_dy,py=xyz[1],pz=xyz[2];
    void* comp=lookup_component(g_editor,px,py,pz); if(!comp){ logline("  name: no comp"); return; }
    unsigned long long c=(unsigned long long)comp;
    g_suppress=1;
    __try {
        unsigned long long capv=0; safe_copy(&capv,(void*)(c+0xB8),8);
        if((unsigned long long)len<=capv){                           // fits existing capacity -> write in place
            void* dst; unsigned long long p=0;
            if(capv<=15) dst=(void*)(c+0xA0); else { safe_copy(&p,(void*)(c+0xA0),8); dst=(void*)p; }
            if(dst){ char zero=0; safe_copy(dst,s,len); safe_copy((char*)dst+len,&zero,1);
                     unsigned long long l=(unsigned long long)len; safe_copy((void*)(c+0xB0),&l,8);
                     logline("  <<< APPLIED name (%d,%d,%d) \"%.*s\"",px,py,pz,len,s); }
        } else logline("  name: len %d > cap %llu - realloc needed, skipped (v1)",len,capv);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  name apply EXC 0x%lX",GetExceptionCode()); }
    g_suppress=0;
    PropCell* pc=prop_find(px,py,pz); if(!pc){ prop_track(px,py,pz,""); pc=prop_find(px,py,pz); }
    if(pc){ int n=len; if(n>63)n=63; memcpy(pc->last_name,s,n); pc->last_name[n]=0; pc->last_namelen=len; }
}
// detect: check ONLY the whitelisted, validated property offsets per watched component. MAIN thread.
static void prop_diff(){
    if(!g_armed||!g_editor||g_suppress) return;
    for(int ci=0;ci<PROPCELLS;ci++){
        PropCell& c=g_prop[ci]; if(!c.used||!c.def[0]) continue;
        void* comp=lookup_component(g_editor,c.x,c.y,c.z); if(!comp) continue;
        unsigned long long base=(unsigned long long)comp;
        uint32_t cur[NRULES>0?NRULES:1]; bool ok=true;
        for(int i=0;i<NRULES;i++){
            if(!strstr(c.def, PROP_RULES[i].def)){ cur[i]=c.init?c.snap[i]:0; continue; }   // rule doesn't apply to this part type
            uint32_t v=0; if(!safe_copy(&v,(void*)(base+PROP_RULES[i].offset),4)){ ok=false; break; } cur[i]=v;
        }
        if(!ok) continue;
        if(!c.init){ for(int i=0;i<NRULES;i++) c.snap[i]=cur[i]; c.init=true; continue; }
        for(int i=0;i<NRULES;i++){
            if(!strstr(c.def, PROP_RULES[i].def) || cur[i]==c.snap[i]) continue;
            if(!prop_val_ok(PROP_RULES[i].offset,cur[i])){ c.snap[i]=cur[i]; continue; }
            int xyz[3]={c.x,c.y,c.z};
            logline("[prop] (%d,%d,%d) [%s] +0x%X : %08X -> %08X", c.x,c.y,c.z, c.def, PROP_RULES[i].offset, c.snap[i], cur[i]);
            emit_prop(xyz,PROP_RULES[i].offset,cur[i]); c.snap[i]=cur[i];
        }
    }
}

// ---- DIAGNOSTIC: component property probe (solo test to find property offsets) ----
// Watches the LAST-PLACED component's memory and logs any DWORD that changes - so placing a battery
// then dragging its charge slider / renaming it reveals which offset holds that property. Read-only;
// logs to coopworkbench-log.txt with a [probe] prefix. A discovery tool, not a sync feature.
static volatile long g_probe_vx=0, g_probe_vy=0, g_probe_vz=0, g_probe_set=0;
static const int PROBE_LO=0x00, PROBE_HI=0x600, PROBE_N=(PROBE_HI-PROBE_LO)/4;
static uint32_t g_probe_snap[PROBE_N]; static bool g_probe_have_snap=false; static DWORD g_probe_last=0;
static void component_probe() {
    if (!g_armed || !g_editor || !g_probe_set) return;
    DWORD now=GetTickCount(); if (now - g_probe_last < 120) return; g_probe_last=now;   // ~8 Hz
    void* comp = lookup_component(g_editor, (int)g_probe_vx, (int)g_probe_vy, (int)g_probe_vz);
    if (!comp) { if (g_probe_have_snap) { logline("[probe] watched component gone"); g_probe_have_snap=false; } return; }
    unsigned long long base=(unsigned long long)comp;
    uint32_t cur[PROBE_N];
    for (int i=0;i<PROBE_N;i++){ uint32_t v=0; safe_copy(&v,(void*)(base+PROBE_LO+i*4),4); cur[i]=v; }
    if (!g_probe_have_snap) {
        memcpy(g_probe_snap,cur,sizeof cur); g_probe_have_snap=true;
        logline("[probe] WATCHING comp=0x%p voxel(%ld,%ld,%ld) window +0x%X..+0x%X -- now change ONE thing (drag charge slider, rename, set a value)",
                comp, g_probe_vx,g_probe_vy,g_probe_vz, PROBE_LO, PROBE_HI);
        return;
    }
    for (int i=0;i<PROBE_N;i++){
        if (cur[i]!=g_probe_snap[i]) {
            int off=PROBE_LO+i*4; float f; memcpy(&f,&cur[i],4);
            char asc[5]; for(int b=0;b<4;b++){ unsigned char c=(unsigned char)(cur[i]>>(b*8)); asc[b]=(c>=32&&c<127)?(char)c:'.'; } asc[4]=0;
            logline("[probe] +0x%03X : %08X -> %08X  (f=%.4f  i=%d  '%s')", off, g_probe_snap[i], cur[i], f, (int)cur[i], asc);
            // pointer-follow: treat the 8 bytes at this offset as a candidate pointer; if it targets
            // printable text, dump it - so a distinctive battery name shows up as a readable string.
            if (i+1 < PROBE_N) {
                unsigned long long ptr = ((unsigned long long)cur[i+1] << 32) | cur[i];
                if (ptr > 0x10000ULL && ptr < 0x7FFFFFFFFFFFULL) {
                    char s[33]; memset(s,0,sizeof s);
                    if (safe_copy(s,(void*)ptr,32) && s[0]>=32 && (unsigned char)s[0]<127) {
                        for(int b=0;b<32;b++){ if(s[b] && (s[b]<32||(unsigned char)s[b]>=127)) s[b]='.'; }
                        logline("[probe]     -> \"%.32s\"  (target of the pointer at +0x%03X - possible name/string)", s, off);
                    }
                }
            }
            g_probe_snap[i]=cur[i];
        }
    }
}

// ======================= STEAM AUTO-CONNECT (kind 12 HELLO / 13 ACK) =======================
// Find the partner automatically instead of pasting SteamID64s. We ping every Steam friend who is CURRENTLY
// PLAYING STORMWORKS with a HELLO on our own channel. Only someone else running this mod can answer, so a
// vanilla friend is never adopted. The id claimed in the payload is checked against the AUTHENTICATED
// transport identity (Steam signs the relay identity), so a forged payload cannot hijack the pairing.
// coop-peer.txt stays a HARD manual override - if it has an id, discovery never runs.
#pragma pack(push,1)
struct HelloMsg { uint32_t magic; uint8_t ver; uint8_t kind; uint16_t pad; uint64_t myid; };
#pragma pack(pop)
static void adopt_peer(uint64_t id) {
    if (!id || id==g_myid || g_peerid) return;              // already paired / nonsense
    if (!p_identClear || !p_identSetID) return;
    p_identClear(g_peerIdent); p_identSetID(g_peerIdent, id);
    MemoryBarrier();                                        // identity fully written before we publish the id
    g_peerid = id;
    if (p_accept) p_accept(g_net, g_peerIdent);
    FILE* f=nullptr; if(!fopen_s(&f,PEERP,"w") && f){ fprintf(f,"%llu\n",(unsigned long long)id); fclose(f); }
    logline("*** AUTO-CONNECTED to partner %llu (Steam friends handshake) ***", (unsigned long long)id);
}
// Read the roster of the Stormworks multiplayer session we are ALREADY in and pair with the other player.
// This is the intended path: no messaging strangers, no friend scanning - if you are playing together, the
// game already knows who is connected. Roster = deque inline at app_state+0x190 {base, cap@8, head@0xC,
// count@0x10}; each player's SteamID64 at player+0x140. We then send that ONE player a HELLO so they pair
// with us too even if their own roster read fails.
// PROBE: the roster offsets above are unverified guesses. We DO know one thing for certain - our own
// SteamID64 - so scan the app-state object for that exact 8-byte value. Wherever the game keeps connected
// players, our own entry must contain it, so the hits give us the real player-struct addresses to work from.
// Runs at most a few times, logs the offsets, then stops.
static void roster_probe() {
    // Keep probing until we actually find something: the player may inject BEFORE joining the session, and
    // a fixed 3 runs would then all land on an empty roster and give up before it mattered.
    // Scan every 5s for up to 5 minutes, not once a second for 30s: the player will often inject BEFORE
    // joining the session, and a short window would be spent before there is anything to find. A 4 MB scan
    // every 5s is negligible, and it stops permanently on the first hit.
    // Memory scanning is now a DIAGNOSTIC fallback only - friends_discover() above is the real mechanism.
    // Confirmed in-game that a partner's id IS present in the process, but the one useful hit sat at a
    // stack-like address among ~40 unrelated ids (mod authors, a x511 workshop array), i.e. transient and
    // ambiguous. Left in, off by default, for future investigation - it does not run unless enabled.
    static int s_runs = 0, s_found = 0; static DWORD s_last = 0;
    if (!g_probe_on || !g_autoconnect_on || s_found || s_runs >= 60) return;
    if (!g_cap_appstate || !g_myid) return;
    DWORD nowp = GetTickCount();
    if (s_last && nowp - s_last < 5000) return;
    s_last = nowp; s_runs++;
    __try {
        // Scan the WHOLE process for any value in the SteamID64 range, not just our own id inside app_state.
        // Rationale: searching for our own id assumed the roster lives near app_state; a range scan finds
        // OTHER players' ids too, and any 8-byte value in this range is almost certainly a real Steam id.
        // Finding the PARTNER's id is far more decisive than finding our own (ours is stored in many places).
        // DEDUPLICATE. The first run's 40-hit cap was entirely eaten by ONE id repeated at a regular 0x2960
        // stride - a workshop/mod array carrying the author's id, not a player roster. Collect DISTINCT ids
        // with a count and first address instead, so one repeated value cannot hide everything else.
        const unsigned long long SID_LO = 76561197960265728ULL, SID_HI = 76561202255233024ULL;
        struct Seen { unsigned long long id; void* first; unsigned n; };
        Seen seen[48]; int nseen = 0; size_t scanned = 0;
        const size_t SCAN_CAP = 1536u*1024u*1024u;     // runs on the recv worker, not the main thread
        MEMORY_BASIC_INFORMATION mbi;
        for (char* p = nullptr; scanned < SCAN_CAP; ) {
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            char* regEnd = (char*)mbi.BaseAddress + mbi.RegionSize;
            if (regEnd <= p) break;                                        // no forward progress
            const bool readable = (mbi.State == MEM_COMMIT) && (mbi.Type == MEM_PRIVATE)
                && (mbi.Protect & (PAGE_READONLY|PAGE_READWRITE))
                && !(mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS));
            if (readable && mbi.RegionSize <= 64u*1024u*1024u) {
                scanned += mbi.RegionSize;
                for (char* q = p; q + 8 <= regEnd; q += 8) {
                    unsigned long long v = *(unsigned long long*)q;
                    if (v < SID_LO || v >= SID_HI) continue;
                    int k = 0; for (; k < nseen; k++) if (seen[k].id == v) { seen[k].n++; break; }
                    if (k == nseen && nseen < 48) { seen[nseen].id=v; seen[nseen].first=q; seen[nseen].n=1; nseen++; }
                }
            }
            p = regEnd;
        }
        int others = 0;
        for (int k = 0; k < nseen; k++) {
            const bool mine = (seen[k].id == g_myid);
            if (!mine) others++;
            logline("[auto] PROBE: %llu  x%u  first=%p%s", (unsigned long long)seen[k].id, seen[k].n,
                    seen[k].first, mine ? "   (US)" : "   <<< OTHER");
        }
        logline("[auto] PROBE: %d distinct id(s), %d not ours (%llu MB scanned, run %d)",
                nseen, others, (unsigned long long)(scanned/(1024*1024)), s_runs);
        if (others) s_found = 1;
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[auto] PROBE faulted (run %d)", s_runs); }
}
// PRIMARY DISCOVERY: ask Steam who of your friends is in Stormworks right now. No RE, no memory scanning,
// nothing that breaks on a game patch - and we message nobody: this only reads presence Steam already
// publishes to you. In practice "friends currently in Stormworks" is 0, 1 or 2 people.
static void friends_discover() {
    if (g_peerid || !g_friends || !p_fcount || !p_fbyidx || !p_fgame) return;
    int n = p_fcount(g_friends, K_FRIEND_IMMEDIATE);
    if (n <= 0 || n > 4096) return;
    unsigned long long cand[8]; int nc = 0;
    for (int i = 0; i < n && nc < 8; i++) {
        unsigned long long fid = p_fbyidx(g_friends, i, K_FRIEND_IMMEDIATE);
        if (!fid || fid == g_myid) continue;
        BYTE fgi[64]; memset(fgi, 0, sizeof fgi);
        if (!p_fgame(g_friends, fid, fgi)) continue;        // not in any game
        unsigned app = 0; memcpy(&app, fgi, 4);             // FriendGameInfo_t: low 32 bits of gameID = AppID
        if (app != SW_APPID) continue;                      // not in Stormworks
        cand[nc++] = fid;
    }
    static int s_lastn = -1;
    if (nc == 1) {
        logline("[auto] one friend is in Stormworks -> pairing with %llu", (unsigned long long)cand[0]);
        adopt_peer(cand[0]);
    } else if (nc > 1) {
        if (nc != s_lastn) { logline("[auto] %d friends are in Stormworks - ambiguous, not auto-pairing:", nc);
            for (int i=0;i<nc;i++) logline("[auto]    %llu", (unsigned long long)cand[i]);
            logline("[auto] put the one you want in coop-peer.txt"); }
    } else if (nc != s_lastn) {
        logline("[auto] no friends currently in Stormworks - waiting");
    }
    s_lastn = nc;
}
static void roster_discover() {
    friends_discover();
    if (g_peerid) return;          // paired via Steam - no need to go digging in memory
    roster_probe();
    if (!g_cap_appstate || !p_send || !p_identClear || !p_identSetID) return;
    __try {
        char* app = (char*)g_cap_appstate;
        void** base=nullptr; unsigned cap=0, head=0, count=0;
        if (!safe_copy(&base, app+0x190, 8)) return;
        safe_copy(&cap, app+0x198, 4); safe_copy(&head, app+0x19C, 4); safe_copy(&count, app+0x1A0, 4);
        static DWORD s_lw=0; DWORD nowt=GetTickCount();
        bool logit = (nowt-s_lw>10000); if (logit) s_lw=nowt;
        if (!base || !cap || cap>4096 || !count || count>64) {
            if (logit) logline("[auto] roster: no session players yet (base=%p cap=%u head=%u count=%u)", (void*)base,cap,head,count);
            return;
        }
        for (unsigned i=0;i<count;i++) {
            void* pl=nullptr; if (!safe_copy(&pl,&base[(head+i)%cap],8) || !pl) continue;
            uint64_t sid=0; if (!safe_copy(&sid,(char*)pl+0x140,8)) continue;
            if (sid < 76561197960265728ULL) continue;         // not a plausible SteamID64
            if (sid == g_myid) { if (logit) logline("[auto] roster player[%u] = us", i); continue; }
            // PRIVACY: never record a stranger's SteamID64. On a public server the roster is full of people
            // who have nothing to do with this mod - we do not log their ids, adopt them, write them to
            // coop-peer.txt, or message them. Only a Steam FRIEND is ever named or paired with.
            bool isfriend = (g_friends && p_hasfriend) ? p_hasfriend(g_friends, sid, K_FRIEND_IMMEDIATE) : false;
            if (!isfriend) { if (logit) logline("[auto] roster player[%u] = (not a friend - ignored)", i); continue; }
            if (logit) logline("[auto] roster player[%u] = %llu (friend)", i, (unsigned long long)sid);
            adopt_peer(sid);
            if (g_peerid == sid) {                            // tell them too, so they pair without needing the roster
                BYTE id[144]; p_identClear(id); p_identSetID(id, sid);
                HelloMsg h; memset(&h,0,sizeof h); h.magic=MAGIC; h.ver=1; h.kind=12; h.myid=g_myid;
                p_send(g_net, id, &h, sizeof h, SEND_RELIABLE, CHANNEL);
            }
            return;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
static void handle_hello(void* mm, const void* data, int sz, uint8_t kd) {
    if (sz < (int)sizeof(HelloMsg)) return;
    HelloMsg h; if (!safe_copy(&h,data,sizeof h)) return;
    // SteamNetworkingMessage_t.m_identityPeer @ mm+0x10: type 16 = SteamID, id64 @ mm+0x18. Steam authenticates
    // this, the payload id is just a claim - if they disagree, drop it.
    int32_t itype=0; uint64_t tid=0;
    safe_copy(&itype,(BYTE*)mm+0x10,4); safe_copy(&tid,(BYTE*)mm+0x18,8);
    if (itype==16 && tid && h.myid && tid!=h.myid) { logline("[auto] ignored HELLO: claimed %llu but transport says %llu",
                                                             (unsigned long long)h.myid,(unsigned long long)tid); return; }
    uint64_t peer = (itype==16 && tid) ? tid : h.myid;
    if (!peer || peer==g_myid) return;
    adopt_peer(peer);
    if (kd==12 && p_send && p_identClear && p_identSetID) {   // answer a HELLO so they adopt us too
        BYTE id[144]; p_identClear(id); p_identSetID(id, peer);
        HelloMsg a; memset(&a,0,sizeof a); a.magic=MAGIC; a.ver=1; a.kind=13; a.myid=g_myid;
        p_send(g_net, id, &a, sizeof a, SEND_RELIABLE, CHANNEL);
    }
}

// ======================= PRESENCE (kind=14) =======================
// Live edits are only worth sending while the PEER is actually in the workbench. When they are away we stay
// quiet; when they come back they full-resync from the whole-craft serialise (the F7 pull) instead of us
// replaying a queue of increments that could overflow or arrive out of order. Standard "snapshot on join,
// deltas while present" model.
extern "C" volatile long g_peer_in_bench = 0;     // last reported peer state (wsdraw shows it in the HUD)
static volatile long g_peer_presence_known = 0;   // 1 once any beacon has arrived
static DWORD g_peer_presence_last = 0;
// Build-volume size in VOXELS (editor+0xD70, f32 x3 = metres*4). 0 if unknown.
static void bench_size_vox(int out[3]) {
    out[0]=out[1]=out[2]=0;
    if (!g_editor) return;
    float sz[3]={0,0,0};
    if (!safe_copy(sz,(void*)(g_editor+0xD70),12)) return;
    for (int i=0;i<3;i++) if (sz[i]>0.0f && sz[i]<100000.0f) out[i]=(int)(sz[i]+0.5f);
}
// Max placeable |voxel| per axis. The game's bounds test uses half-extent (size-2)/2 and blocks are CENTRED
// on their voxel (+/-0.5), so the last legal voxel is (size-3)/2. Verified against in-game plus-shape
// measurements at two benches (30x30x60 -> +/-13,13,28 and 146x40x140 -> +/-71,18,68 - exact on all 6 axes).
static void bench_max_voxel(const int size[3], int out[3]) {
    for (int i=0;i<3;i++) out[i] = size[i]>3 ? (size[i]-3)/2 : 0;
}
static void emit_presence(long in_bench) {
    if (!p_send || !g_net || !g_peerid || g_localecho) return;
    int sz[3]; bench_size_vox(sz);
    PlaceMsg m; memset(&m,0,sizeof m); m.magic=MAGIC; m.ver=1; m.kind=14; m.aux=(int32_t)in_bench;
    m.x=sz[0]; m.y=sz[1]; m.z=sz[2];         // carry our bench volume so the peer can compare benches
    p_send(g_net, g_peerIdent, &m, sizeof m, SEND_RELIABLE, CHANNEL);
}
// TRUE only when we POSITIVELY know the peer is out of the bench. Unknown / stale info => assume present and
// keep syncing: a wrong "away" guess would silently stop sync, which is a far worse failure than extra traffic.
static bool peer_away() {
    if (g_localecho) return false;
    if (!g_peer_presence_known) return false;
    if (GetTickCount() - g_peer_presence_last > 10000) return false;   // stale beacon -> assume present
    return !g_peer_in_bench;
}
// SAME BENCH IS A HARD REQUIREMENT. Different build volumes mean blocks that are legal on one side are
// impossible on the other, so the crafts could never actually match - we refuse to sync at all rather than
// silently produce two different vehicles. Fail-OPEN on unknown (same philosophy as peer_away): only a
// POSITIVELY known difference blocks sync, so a missing beacon can never wedge a working session.
extern "C" volatile long g_bench_mismatch = 0;    // 1 = blocked (wsdraw shows it)
static bool bench_mismatch() {
    if (g_localecho) return false;
    if (!g_peer_bench[0]) return false;                 // partner volume not known yet
    int mine[3]; bench_size_vox(mine);
    if (!mine[0]) return false;                         // our volume not known yet
    return mine[0]!=g_peer_bench[0] || mine[1]!=g_peer_bench[1] || mine[2]!=g_peer_bench[2];
}
// single gate for ALL edit traffic: partner away (they will resync on entry) or wrong bench (hard block).
static bool sync_paused() { return !g_sync_enabled || peer_away() || bench_mismatch(); }
// cheap "is our craft essentially empty" test: total render-node (occupied 16-voxel chunk) count over bodies.
// Used to decide whether a resync-on-entry can auto-pull (nothing to lose) or must ask first.
static unsigned craft_node_count() {
    if (!g_editor) return 0;
    unsigned total = 0;
    __try {
        char* V = *(char**)(g_editor+0x13C8); if (!V) return 0;
        unsigned nb=*(unsigned*)(V+0x10), bc=*(unsigned*)(V+0x08), bh=*(unsigned*)(V+0x0C);
        void** bb=*(void***)(V+0x00);
        if (bb && bc) for (unsigned i=0;i<nb && i<64;i++) { char* b=(char*)bb[(bh+i)%bc]; if (b) total += *(unsigned*)(b+0x400); }
    } __except(EXCEPTION_EXECUTE_HANDLER){ return 0; }
    return total;
}

// ---- 2-machine full-craft PULL ("load peer's craft") transport ----
// A presses F7 -> sends a REQUEST (kind=8) to the peer. B, on receiving it, serializes its craft on the MAIN
// thread and streams it back as CHUNKS (kind=9). A reassembles + loads on the MAIN thread. magic@0/ver@4/kind@5
// share PlaceMsg's layout so the recv peek at offset 5 tells them apart.
#pragma pack(push,1)
struct ChunkHdr { uint32_t magic; uint8_t ver; uint8_t kind; uint16_t pad; uint32_t total; uint32_t offset; uint32_t clen; };
#pragma pack(pop)
// 64 KB per message. 400 KB "fits" Steam's per-message cap but FILLS the connection's send buffer, so the
// next chunk came straight back as EResult 25 (LimitExceeded) and was silently lost. Smaller chunks + the
// paced pump below let the buffer drain between sends.
static const unsigned PULL_CHUNK = 65536;
static const int      PULL_CHUNKS_PER_FRAME = 4;   // ~256 KB/frame ceiling; stops early if the buffer fills
static char*    g_pull_tx = nullptr;               // craft being sent (game-allocated; freed when done)
static unsigned g_pull_tx_len = 0, g_pull_tx_off = 0;
static DWORD    g_pull_tx_last = 0;
static volatile long  g_pull_send_req = 0;    // B: set by recv(kind=8), consumed by my_runcb -> serialize+send
static char*          g_pull_rx = nullptr;    // A: reassembly buffer (VirtualAlloc)
static unsigned       g_pull_rx_total = 0, g_pull_rx_got = 0;
// A pull CLEARS the craft before rebuilding it, so with no feedback it looks like the mod deleted your work.
// wsdraw draws a centre-screen banner from these while a sync is in flight.
extern "C" {
    volatile long g_sync_busy = 0;            // 1 = waiting for / receiving the partner's craft
    volatile long g_sync_got = 0, g_sync_total = 0;
    volatile long g_sync_started = 0;         // GetTickCount when the request went out (for a timeout message)
    volatile long g_sync_err = 0;             // GetTickCount of the last refusal; banner shows why for a few s
    char          g_sync_err_msg[96] = {0};
}
static void sync_error(const char* why) {     // make a refused/failed sync VISIBLE - silence is the bug we are fixing
    strncpy_s(g_sync_err_msg, sizeof g_sync_err_msg, why, _TRUNCATE);
    InterlockedExchange(&g_sync_err, (long)GetTickCount());
    InterlockedExchange(&g_sync_busy, 0);
}
static volatile long  g_pull_rx_ready = 0;    // A: set when the last chunk lands, consumed by my_runcb -> load

// Serialize the whole craft via the game's own save format. Returns the game-allocated blob (free via
// 0x9B15A0) + its length, or nullptr. MUST run on the MAIN thread (reads the live editor/vehicle).
static char* snapshot_serialize(unsigned* out_len) {
    *out_len = 0;
    if(!g_armed || !g_editor) return nullptr;
    char* data=nullptr; unsigned len=0;
    __try {
        unsigned long long editor=g_editor;
        void* vehicle=*(void**)(editor+0x13C8);
        void* gobj   =*(void**)(editor+0x70);
        if(!vehicle||!gobj) return nullptr;
        void* ctx     =(char*)gobj+0x64C8;
        void* registry=(char*)gobj+0xBB670;
        unsigned char ob[64]; memset(ob,0,sizeof ob);
        ((void(*)(void*,void*,void*,void*))(g_base+0x4C5FE0))(vehicle, ctx, registry, ob);   // whole-craft serialize
        data=*(char**)(ob+0);
        unsigned len8=*(unsigned*)(ob+8), len16=*(unsigned*)(ob+0x10);
        len = (len8>0 && len8<0x8000000u) ? len8 : ((len16>0 && len16<0x8000000u) ? len16 : 0);
        if(!data || !len){ if(data) ((void(*)(void*))(g_base+0x9B15A0))(data); return nullptr; }
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[snap] serialize EXC 0x%lX", GetExceptionCode()); return nullptr; }
    *out_len = len;
    return data;   // caller frees via 0x9B15A0
}

// F5 solo test: serialize the local craft to a file (confirms the serializer; read-only, no craft mutation).
static void snapshot_save_to_file() {
    if(!g_armed||!g_editor){ logline("[snap] not armed - place a block first, then press F5"); return; }
    unsigned len=0; char* data=snapshot_serialize(&len);
    if(!data){ logline("[snap] serialize failed"); return; }
    logline("[snap] serialized %u bytes", len);
    char p[MAX_PATH]; DWORD n=GetModuleFileNameA(g_hmod,p,MAX_PATH);
    if(n && n<MAX_PATH){ char* s=strrchr(p,'\\'); if(s) *(s+1)=0; strncat_s(p,MAX_PATH,"coopworkbench-snapshot.xml",_TRUNCATE);
        FILE* f=nullptr; if(!fopen_s(&f,p,"wb")&&f){ fwrite(data,1,(size_t)len,f); fclose(f); logline("[snap] wrote %u bytes -> coopworkbench-snapshot.xml", len); } }
    ((void(*)(void*))(g_base+0x9B15A0))(data);   // free the serializer's buffer
}
// Load a whole-craft blob into the live editor vehicle + render it THIS FRAME. MUST run on the MAIN thread.
// blob = the game's save format (from snapshot_serialize OR a peer pull). Does NOT free blob (caller owns it).
// g_suppress gates our detect hooks so the rebuilt components don't re-broadcast (flood).
static void snapshot_load_from_buffer(char* blob, unsigned sz) {
    if(!g_armed||!g_editor){ logline("[snap] load: not armed"); return; }
    if(!blob||!sz||sz>0x8000000u){ logline("[snap] load: bad buffer"); return; }
    __try {
        unsigned char ib[64]; memset(ib,0,sizeof ib);
        *(char**)(ib+0)=blob; *(unsigned*)(ib+8)=sz; *(unsigned*)(ib+0xC)=sz;   // GStr in {data, len@8, cap@0xC}
        unsigned long long editor=g_editor;
        void* vehicle=*(void**)(editor+0x13C8);
        void* gobj   =*(void**)(editor+0x70);
        if(!vehicle||!gobj){ logline("[snap] load: no vehicle/gobj"); return; }
        void* ctx     =(char*)gobj+0x64C8;
        void* registry=(char*)gobj+0xBB670;
        logline("[snap] LOADING %u bytes via 0x4C6160 ...", sz);
        // ==== POSITION: capture OUR bench anchor BEFORE the load ====
        // vehicle+0x1F0 (f64 x3) = world position of voxel (0,0,0) - the REAL render anchor (RE-confirmed,
        // and read back correct in-game). vehicle+0x2D8 (f32 x3) = placement offset, which the blob carries
        // from the sender. NOTE: the previous attempt here deltad body+0x2B8/+0x2F8; those read 0.000 in-game
        // at every bench, so that code could only ever compute a zero delta - it was a no-op. Removed.
        double recv_org[3];  bool have_org  = safe_copy(recv_org,  (char*)vehicle+0x1F0, 24);
        float  recv_poff[3]; bool have_poff = safe_copy(recv_poff, (char*)vehicle+0x2D8, 12);
        if (have_org) logline("[snap] our anchor: voxel0=(%.2f,%.2f,%.2f)", recv_org[0],recv_org[1],recv_org[2]);
        g_suppress=1;
        typedef void (*load_t)(void*,void*,void*,void*);
        ((load_t)(g_base+0x4C6160))(vehicle, ctx, registry, ib);
        // ==== POSITION: put OUR anchor back (the blob may carry the sender's) ====
        // Logs whether the load actually changed them, so a wrong-position report is diagnosable rather than
        // guesswork. Note the craft's VOXELS are bench-independent (origin is always (0,0,0), §15.1), so with
        // both players on the same bench - now a hard requirement - geometry should line up on its own; this
        // only guards the world anchor the renderer uses.
        { double a[3]={0,0,0}; float p[3]={0,0,0};
          bool ga = safe_copy(a,(char*)vehicle+0x1F0,24), gp = safe_copy(p,(char*)vehicle+0x2D8,12);
          bool org_ch  = ga && have_org  && (a[0]!=recv_org[0]  || a[1]!=recv_org[1]  || a[2]!=recv_org[2]);
          bool poff_ch = gp && have_poff && (p[0]!=recv_poff[0] || p[1]!=recv_poff[1] || p[2]!=recv_poff[2]);
          logline("[snap] anchor after load: voxel0=(%.2f,%.2f,%.2f)%s poff=(%.3f,%.3f,%.3f)%s",
                  a[0],a[1],a[2], org_ch?"  <-- CHANGED BY LOAD":"", p[0],p[1],p[2], poff_ch?"  <-- CHANGED BY LOAD":"");
          if (have_org)  safe_copy((char*)vehicle+0x1F0, recv_org,  24);   // render at OUR bench, not the sender's
          if (have_poff) safe_copy((char*)vehicle+0x2D8, recv_poff, 12);
        }
        // ==== POST-LOAD RENDER (CONFIRMED WORKING - instant, no hang) ====
        // 0x4C6160 loads only the DATA model. 0x4C98C0 (body build) creates the render NODES into body+0x3F0
        // (one per occupied 16-voxel chunk) + the spatial hash body+0x3A8 - CONFIRMED in-game: after it,
        // body+0x3F0.count == the chunk count (40 for the test pyramid; body+0x408/+0x420 stay 0). Then
        // force_remesh (0x4A2E40 regionBuild + 0x4A31E0 remeshMerge) on each node builds+shows its chunk mesh
        // THIS FRAME - the proven pair paint/delete already drive safely from my_runcb.
        // MUST NOT call 0x4C0870 (node create - redundant; nodes already exist) or 0x4A3740 (whole-body mesher
        // ending in GPU upload 0x471D60): BOTH DEADLOCK when driven from the RunCallbacks context.
        {
            typedef void (*bodybuild_t)(void*, void*);   // 0x4C98C0(vehicle, &scratch)  build render nodes + spatial hash
            typedef void (*dtor_t)(void*);               // 0x898280(&scratch)           free scratch
            typedef void (*whole_t)(void*);              // 0x4C3410(vehicle)  component<->node linkage
            char* V=(char*)vehicle;
            if (V) {
                unsigned char scratch[64]; memset(scratch,0,sizeof scratch);
                ((bodybuild_t)(g_base+0x4C98C0))((void*)V, scratch);
                ((dtor_t)(g_base+0x898280))(scratch);
                // 0x4C3410 = topology/association pass. It does NOT mesh (that is why it was dropped when we
                // were chasing invisibility) but it DOES establish the component<->render-node links
                // (comp+0x148 -> node, node+0x150 -> comp) and prune anything left unlinked. Skipping it left
                // every loaded component half-linked, which is almost certainly why COPYING a part crashed the
                // game and why fresh warning markers appeared after a pull - a complete transfer still
                // produced a structurally incomplete craft. Safe from this context (an earlier build called
                // it here without hanging; the hangs were 0x4C0870 / 0x4A3740).
                ((whole_t)(g_base+0x4C3410))((void*)V);
                unsigned nb=*(unsigned*)(V+0x10), bc=*(unsigned*)(V+0x08), bh=*(unsigned*)(V+0x0C);
                void** bb=*(void***)(V+0x00);
                if (bb && bc && nb) {
                    void* bodies[256]; unsigned n = nb>256?256:nb;
                    for (unsigned i=0;i<n;i++) bodies[i]=bb[(bh+i)%bc];   // snapshot bodies (deque may shift)
                    unsigned total=0;
                    for (unsigned i=0;i<n;i++) {
                        char* body=(char*)bodies[i]; if(!body) continue;
                        unsigned nn=*(unsigned*)(body+0x400), nc=*(unsigned*)(body+0x3F8), nh=*(unsigned*)(body+0x3FC);
                        void** nbase=*(void***)(body+0x3F0);             // render-node deque
                        if (nbase && nc) for (unsigned k=0;k<nn;k++) { void* node=nbase[(nh+k)%nc]; if(node){ force_remesh(g_editor, node); total++; } }
                    }
                    logline("[snap] load+render DONE: %u body(s), %u node(s) remeshed", n, total);
                }
            }
        }
        g_suppress=0;
        logline("[snap] load+build done - craft should render now");
    } __except(EXCEPTION_EXECUTE_HANDLER){ g_suppress=0; logline("[snap] LOAD EXC 0x%lX", GetExceptionCode()); }
}

// F4 solo test: read the F5-saved file and load it through the shared buffer path.
static void snapshot_load_from_file() {
    char p[MAX_PATH]; DWORD nn=GetModuleFileNameA(g_hmod,p,MAX_PATH);
    if(!nn||nn>=MAX_PATH){ logline("[snap] load: no path"); return; }
    char* s=strrchr(p,'\\'); if(s)*(s+1)=0; strncat_s(p,MAX_PATH,"coopworkbench-snapshot.xml",_TRUNCATE);
    FILE* f=nullptr; if(fopen_s(&f,p,"rb")||!f){ logline("[snap] load: no snapshot file - press F5 first"); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0||sz>0x8000000){ fclose(f); logline("[snap] load: bad size %ld",sz); return; }
    char* blob=(char*)VirtualAlloc(nullptr,(SIZE_T)sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!blob){ fclose(f); logline("[snap] load: alloc failed"); return; }
    size_t rd=fread(blob,1,sz,f); fclose(f);
    if((long)rd==sz) snapshot_load_from_buffer(blob,(unsigned)sz);
    else logline("[snap] load: short read");
    VirtualFree(blob,0,MEM_RELEASE);
}

// ---- PULL transport ----
// B: serialize the local craft and stream it to the peer as kind=9 chunks. MAIN thread (serialize needs it).
static void pull_send_craft() {
    if(bench_mismatch()){ logline("[pull] not sending - partner is at a different-size bench; our craft would not fit theirs"); return; }
    unsigned len=0; char* data=snapshot_serialize(&len);
    if(!data){ logline("[pull] serialize failed - nothing to send"); return; }
    if(!p_send||!g_net||!g_peerid){ ((void(*)(void*))(g_base+0x9B15A0))(data); logline("[pull] no peer to send to"); return; }
    // Hand the blob to the paced pump instead of blasting it in one loop. Sending it all at once filled
    // Steam's connection send buffer and every chunk after the first came back EResult 25 (LimitExceeded)
    // and was LOST - the receiver stalled forever holding a partial craft.
    if (g_pull_tx) { ((void(*)(void*))(g_base+0x9B15A0))(g_pull_tx); g_pull_tx=nullptr; }
    g_pull_tx = data; g_pull_tx_len = len; g_pull_tx_off = 0; g_pull_tx_last = GetTickCount();
    logline("[pull] sending craft: %u bytes in %u chunk(s)", len, (len + PULL_CHUNK - 1)/PULL_CHUNK);
}
// Push a few chunks per frame, and STOP (without advancing) the moment Steam says its buffer is full, so the
// same chunk is retried next frame. Called from my_runcb.
static void pull_send_pump() {
    if (!g_pull_tx) return;
    if (!p_send || !g_net || !g_peerid) {
        ((void(*)(void*))(g_base+0x9B15A0))(g_pull_tx); g_pull_tx=nullptr;
        logline("[pull] send aborted - no peer"); return;
    }
    for (int n=0; n<PULL_CHUNKS_PER_FRAME && g_pull_tx_off < g_pull_tx_len; ++n) {
        unsigned clen = (g_pull_tx_len-g_pull_tx_off > PULL_CHUNK) ? PULL_CHUNK : (g_pull_tx_len-g_pull_tx_off);
        unsigned msglen = (unsigned)sizeof(ChunkHdr)+clen;
        char* buf=(char*)VirtualAlloc(nullptr,msglen,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!buf) return;                                    // out of memory - try again next frame
        ChunkHdr* h=(ChunkHdr*)buf; h->magic=MAGIC; h->ver=1; h->kind=9; h->pad=0;
        h->total=g_pull_tx_len; h->offset=g_pull_tx_off; h->clen=clen;
        memcpy(buf+sizeof(ChunkHdr), g_pull_tx+g_pull_tx_off, clen);
        int rc=p_send(g_net, g_peerIdent, buf, msglen, SEND_RELIABLE, CHANNEL);
        VirtualFree(buf,0,MEM_RELEASE);
        if (rc == 25) {                                     // k_EResultLimitExceeded: buffer full, back off
            static DWORD s_lw=0; DWORD nowt=GetTickCount();
            if (nowt-s_lw>2000){ s_lw=nowt; logline("[pull] send buffer full at %u/%u - pacing", g_pull_tx_off, g_pull_tx_len); }
            return;                                         // do NOT advance: this chunk retries next frame
        }
        if (rc != 1) { logline("[pull] send FAILED at %u/%u rc=%d - aborting", g_pull_tx_off, g_pull_tx_len, rc);
            ((void(*)(void*))(g_base+0x9B15A0))(g_pull_tx); g_pull_tx=nullptr; return; }
        g_pull_tx_off += clen; g_pull_tx_last = GetTickCount();
    }
    if (g_pull_tx_off >= g_pull_tx_len) {
        ((void(*)(void*))(g_base+0x9B15A0))(g_pull_tx); g_pull_tx=nullptr;
        logline("[pull] craft sent (%u bytes)", g_pull_tx_len);
    } else if (GetTickCount()-g_pull_tx_last > 30000) {
        ((void(*)(void*))(g_base+0x9B15A0))(g_pull_tx); g_pull_tx=nullptr;
        logline("[pull] send stalled >30s at %u/%u - aborting", g_pull_tx_off, g_pull_tx_len);
    }
}
// A: press F7 -> request the peer's craft (kind=8).
static void pull_request() {
    if(!p_send||!g_net||!g_peerid){ logline("[pull] no peer set - can't request");
        sync_error("no partner connected - set coop-peer.txt"); return; }
    if(bench_mismatch()){ logline("[pull] REFUSED - partner is at a %dx%dx%d bench, you are not. Their craft could not fit. Use the same bench type.",
                                  g_peer_bench[0],g_peer_bench[1],g_peer_bench[2]);
        sync_error("wrong bench - both must use the same type"); return; }
    ChunkHdr h; memset(&h,0,sizeof h); h.magic=MAGIC; h.ver=1; h.kind=8;
    int rc=p_send(g_net, g_peerIdent, &h, (unsigned)sizeof h, SEND_RELIABLE, CHANNEL);
    InterlockedExchange(&g_sync_busy,1); InterlockedExchange(&g_sync_got,0); InterlockedExchange(&g_sync_total,0);
    InterlockedExchange(&g_sync_started,(long)GetTickCount());
    logline("[pull] >>> requested peer craft (rc=%d)", rc);
}
// A: reassemble an incoming kind=9 chunk into g_pull_rx; set g_pull_rx_ready when complete. recv_worker thread.
static void pull_rx_chunk(const char* buf, int c) {
    if(c < (int)sizeof(ChunkHdr)) return;
    const ChunkHdr* h=(const ChunkHdr*)buf;
    if(h->total==0 || h->total>0x8000000u) return;
    unsigned clen=h->clen;
    if((unsigned)c < (unsigned)sizeof(ChunkHdr)+clen || (uint64_t)h->offset+clen > h->total) return;
    if(!g_pull_rx || g_pull_rx_total!=h->total){
        if(g_pull_rx) VirtualFree(g_pull_rx,0,MEM_RELEASE);
        g_pull_rx=(char*)VirtualAlloc(nullptr,h->total,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        g_pull_rx_total = g_pull_rx ? h->total : 0; g_pull_rx_got=0;
        if(!g_pull_rx) return;
    }
    memcpy(g_pull_rx+h->offset, buf+sizeof(ChunkHdr), clen);
    // Count CONTIGUOUS bytes from 0 - NEVER a running sum. A retransmitted chunk 0 previously pushed the
    // running total past 'total', which completed the transfer while the tail was still unwritten, and the
    // craft loaded with zeroed components (the "holes"). Duplicates are now free; gaps are visible.
    if (h->offset == g_pull_rx_got)      g_pull_rx_got += clen;      // the next expected chunk
    else if (h->offset < g_pull_rx_got)  { /* duplicate/retransmit - we already have these bytes */ }
    else { logline("[pull] !! gap: chunk at %u but only %u contiguous bytes received - waiting", h->offset, g_pull_rx_got); }
    InterlockedExchange(&g_sync_got,(long)g_pull_rx_got); InterlockedExchange(&g_sync_total,(long)g_pull_rx_total);
    InterlockedExchange(&g_sync_busy,1);
    logline("[pull] <<< chunk off=%u clen=%u (%u/%u)", h->offset, clen, g_pull_rx_got, g_pull_rx_total);
    // EXACT equality only. Loading a partially-filled buffer does not merely look wrong - the unwritten tail
    // is zeros, the game builds malformed components from it, and the next edit CRASHES the game (observed:
    // partner edited a railing on a craft loaded from a 400000/552644 transfer and the game went down).
    if(g_pull_rx_got == g_pull_rx_total) InterlockedExchange(&g_pull_rx_ready, 1);
}

// ======================= LIVE PROPERTY CODEC (Approach B) — corrected =======================
// Serialize/deserialize a component's FULL state (every property/sub-property, MC internals, names, modded
// parts) via the game's OWN atomic per-component unit 0x4AE3F0 - the exact routine the whole-craft body loop
// (0x4C4EF0) invokes per component. It does the entire lifecycle: header + descriptor lookup + factory-built
// codec DTO + bind + codec(+0x260) + finalize + merge (0x4BA440). We do NOT call comp vtable+0x260 directly:
// +0x260 is a method of the transient codec DTO, not the component - calling it on the comp CRASHED.
// SerializeComponent(rcx=comp, rdx=&ctxblk, r8=&archive, r9=&node, [rsp+0x20]=bodyId); one fn both ways,
// write vs read chosen by wflag/dir. registry = gobj+0xBB670 (same arg the whole-craft serialize passes).
static const unsigned MAX_BLOB = 0x10000;   // 64KB cap per component
typedef void (*sercomp_t)(void*,void*,void*,void*,unsigned);   // 0x4AE3F0

// serialize ONE component -> out[0..*outlen]. 1MB sink + guard page so any overrun FAULTS (no heap smash).
static bool serialize_one(void* comp, unsigned char* out, unsigned* outlen) {
    *outlen=0;
    if(!g_editor||!comp) return false;
    char* gobj=*(char**)(g_editor+0x70); if(!gobj) return false;
    void* registry=(void*)(gobj+0xBB670);
    const unsigned CAP=0x100000;
    void* buf=VirtualAlloc(nullptr, CAP+0x1000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(!buf) return false;
    DWORD old; VirtualProtect((char*)buf+CAP, 0x1000, PAGE_NOACCESS, &old);
    unsigned char ctxblk[0x18]; memset(ctxblk,0,sizeof ctxblk);
    *(void**)(ctxblk+0x00)=registry; *(void**)(ctxblk+0x08)=nullptr; *(unsigned*)(ctxblk+0x10)=5;   // mode 5: skip name block, ctx unused
    unsigned char archive[0x28]; memset(archive,0,sizeof archive);
    unsigned char cursor[0x18]; memset(cursor,0,sizeof cursor);
    *(void**)(cursor+0x00)=archive; *(void**)(cursor+0x08)=buf; *(unsigned*)(cursor+0x10)=CAP; cursor[0x14]=1;   // dir WRITE
    *(void**)(archive+0x00)=buf; *(unsigned*)(archive+0x08)=CAP; *(void**)(archive+0x10)=cursor; *(unsigned*)(archive+0x20)=1; archive[0x24]=1;   // submode binary, wflag WRITE
    unsigned char node[0x60]; memset(node,0,sizeof node); *(unsigned*)(node+0x28)=1;
    bool ok=false;
    __try {
        ((sercomp_t)(g_base+0x4AE3F0))(comp, ctxblk, archive, node, 0);
        unsigned written = CAP - *(unsigned*)(cursor+0x10);   // cap - remaining
        if(written && written<=MAX_BLOB){ memcpy(out, buf, written); *outlen=written; ok=true; }
    } __except(EXCEPTION_EXECUTE_HANDLER){ ok=false; }
    VirtualFree(buf,0,MEM_RELEASE);
    return ok;
}
// deserialize a blob back INTO an existing component (in-place). Caller must remesh after for a visual change.
static bool deserialize_one(void* comp, const unsigned char* blob, unsigned len) {
    if(!g_editor||!comp||!blob||!len||len>MAX_BLOB) return false;
    char* gobj=*(char**)(g_editor+0x70); if(!gobj) return false;
    void* registry=(void*)(gobj+0xBB670);
    unsigned char ctxblk[0x18]; memset(ctxblk,0,sizeof ctxblk);
    *(void**)(ctxblk+0x00)=registry; *(void**)(ctxblk+0x08)=nullptr; *(unsigned*)(ctxblk+0x10)=5;
    unsigned char archive[0x28]; memset(archive,0,sizeof archive);
    unsigned char cursor[0x18]; memset(cursor,0,sizeof cursor);
    *(void**)(cursor+0x00)=archive; *(void**)(cursor+0x08)=(void*)blob; *(unsigned*)(cursor+0x10)=len; cursor[0x14]=0;   // dir READ
    *(void**)(archive+0x00)=(void*)blob; *(unsigned*)(archive+0x08)=len; *(void**)(archive+0x10)=cursor; *(unsigned*)(archive+0x20)=1; archive[0x24]=0;   // wflag READ
    unsigned char node[0x60]; memset(node,0,sizeof node); *(unsigned*)(node+0x28)=1;
    bool ok=false;
    __try { ((sercomp_t)(g_base+0x4AE3F0))(comp, ctxblk, archive, node, 0); ok=true; }
    __except(EXCEPTION_EXECUTE_HANDLER){ ok=false; }
    return ok;
}
static void* pick_first_component() {
    for(int x=-16;x<=16;x++) for(int y=-8;y<=8;y++) for(int z=-16;z<=16;z++){
        void* c=lookup_component(g_editor, x,y,z); if(c) return c;
    }
    return nullptr;
}
// F8 SOLO GO/NO-GO: round-trip EXACTLY ONE component, per-step logged, with a witness field. Contained.
static void comp_selftest() {
    logline("[F8] === single-component round-trip ===");
    if(!g_armed||!g_editor){ logline("[F8] not armed"); return; }
    void* comp=pick_first_component();
    if(!comp){ logline("[F8] no component found near origin"); return; }
    unsigned typeIdx=*(unsigned*)((char*)comp+0x2AC);
    logline("[F8] comp=%p typeIdx=%u", comp, typeIdx);
    unsigned before=*(unsigned*)((char*)comp+0x60);   // witness: base colour @+0x60
    logline("[F8] step1 witness @+0x60 = 0x%08X", before);
    static unsigned char blob[0x10000]; unsigned len=0;
    logline("[F8] step2 serialize_one...");
    if(!serialize_one(comp, blob, &len)){ logline("[F8] FAIL@serialize (SEH caught) - archive/ctx setup"); return; }
    logline("[F8] step2 OK len=%u head=%02X %02X %02X %02X", len, len>0?blob[0]:0, len>1?blob[1]:0, len>2?blob[2]:0, len>3?blob[3]:0);
    if(!len){ logline("[F8] STOP: empty blob"); return; }
    *(unsigned*)((char*)comp+0x60)=0xDEADBEEF;   // mutate the live comp so we can prove restore
    logline("[F8] step3 mutated @+0x60 -> 0xDEADBEEF");
    logline("[F8] step4 deserialize_one (in place)...");
    g_suppress=1;
    bool r=deserialize_one(comp, blob, len);
    g_suppress=0;
    if(!r){ logline("[F8] FAIL@deserialize (SEH caught)"); return; }
    unsigned after=*(unsigned*)((char*)comp+0x60);
    logline("[F8] step4 OK @+0x60 = 0x%08X (want 0x%08X)", after, before);
    logline(after==before ? "[F8] PASS: real state captured + restored" : "[F8] PARTIAL: decoded but +0x60 not restored");
    __try{ force_remesh(g_editor, comp); }__except(EXCEPTION_EXECUTE_HANDLER){}
    logline("[F8] === done (ONE comp) ===");
}

// ---- PRESENCE UI: local hover-voxel sampler (wsdraw sends it + draws the partner's cursor) ----
// editor+0x12F8/+0x1300/+0x1308 = cursor position in craft-local METRES (= voxel * 0.25), three f64. Proven
// by two in-game consumers (single-delete funnel 0x804300 and the per-frame preview builder 0x801A80), both
// converting with round(metres * 4). editor+0x13B8 = preview-cell count, editor+0xE7C = current tool.
extern "C" {
    volatile long g_cur_valid = 0;
    volatile long g_cur_vx = 0, g_cur_vy = 0, g_cur_vz = 0;
    volatile long g_cur_tool = 0;
    // AUTO-CALIBRATION: world position of voxel (0,0,0) for the CURRENT bench (vehicle+0x1F0, RE-confirmed).
    // The overlay used to need hand-calibrating with the arrow keys, and that offset did not follow you to
    // another bench - which is why world-space markers drew in the wrong place (or kilometres away).
    volatile long g_bench_org_valid = 0;
    double g_bench_org[3] = {0,0,0};
    // FLOATING ORIGIN: the renderer draws in a space rebased to a whole-km tile chosen from the CAMERA
    // (game rule at 0x66E1F0: graphics_offset = 1000*floor(0.5 - cam/1000), render = world + graphics_offset).
    // editor+0x80 is a 4x4 f64 camera->world matrix; its translation row (+0xE0) is the camera in ABSOLUTE
    // world - the one value that lets the overlay derive the rebase exactly, at ANY bench.
    volatile long g_cam_world_valid = 0;
    double g_cam_world[3] = {0,0,0};
}
// Publish the camera's absolute world position, but only if editor+0x80 really is a rigid transform - a
// validated read is free here and makes a bad one impossible to act on.
static void sample_camera_world() {
    InterlockedExchange(&g_cam_world_valid, 0);
    if (!g_in_bench || !g_editor) return;
    __try {
        double m[16];
        if (!safe_copy(m, (void*)(g_editor + 0x80), sizeof m)) return;
        if (!(m[15] > 0.999999 && m[15] < 1.000001)) return;                 // +0xF8 sentinel
        for (int r=0;r<3;r++) if (!(m[r*4+3] > -1e-9 && m[r*4+3] < 1e-9)) return;   // +0x98/0xB8/0xD8 == 0
        for (int r=0;r<3;r++) {                                              // basis rows orthonormal
            double n = m[r*4+0]*m[r*4+0] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2];
            if (!(n > 0.999999 && n < 1.000001)) return;
        }
        for (int i=0;i<3;i++) if (!(m[12+i] > -1e7 && m[12+i] < 1e7)) return;
        g_cam_world[0]=m[12]; g_cam_world[1]=m[13]; g_cam_world[2]=m[14];
        InterlockedExchange(&g_cam_world_valid, 1);
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
static void sample_bench_origin() {
    if (!g_in_bench || !g_editor) return;
    __try {
        char* V = *(char**)(g_editor+0x13C8); if (!V) return;
        double o[3];
        if (safe_copy(o, (char*)V+0x1F0, 24)) {
            // sanity: a real bench origin is a finite world coordinate
            for (int i=0;i<3;i++) if (!(o[i] > -1e7 && o[i] < 1e7)) return;
            g_bench_org[0]=o[0]; g_bench_org[1]=o[1]; g_bench_org[2]=o[2];
            InterlockedExchange(&g_bench_org_valid, 1);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
static void sample_cursor() {
    if (!g_in_bench || !g_editor) { InterlockedExchange(&g_cur_valid, 0); return; }
    int v[3]={0,0,0}; bool got=false; unsigned char hit=0; unsigned tool=0; const char* src="none";
    safe_copy(&hit,  (void*)(g_editor+0x1568), 1);   // raycast hit this frame
    safe_copy(&tool, (void*)(g_editor+0xE7C),  4);   // 0 = build/place (a VALID id, not "unset")
    // PRIMARY: the ghost cell - where a block would land. int32 triple, same voxel space as our placement.
    if (hit && safe_copy(v, (void*)(g_editor+0x1440), 12)) { got=true; src="ghost"; }
    // FALLBACK: the component under the cursor. Self-gating (null when nothing is hovered), so it covers the
    // paint/erase/connect tools and the case where nothing is armed to place.
    if (!got) {
        unsigned long long comp=0;
        if (safe_copy(&comp,(void*)(g_editor+0xB80),8) && comp && safe_copy(v,(void*)(comp+0x18),12)) { got=true; src="comp"; }
    }
    if (!got) { InterlockedExchange(&g_cur_valid, 0); return; }
    for (int i=0;i<3;i++) if (v[i] < -4096 || v[i] > 4096) { InterlockedExchange(&g_cur_valid, 0); return; }
    InterlockedExchange(&g_cur_vx, v[0]);
    InterlockedExchange(&g_cur_vy, v[1]);
    InterlockedExchange(&g_cur_vz, v[2]);
    InterlockedExchange(&g_cur_tool, (long)tool);
    InterlockedExchange(&g_cur_valid, 1);
    if (g_cursor_selftest) {
        static DWORD s_lw=0; DWORD nowt=GetTickCount();
        if (nowt-s_lw>2000) { s_lw=nowt;
            logline("[cursor] voxel=(%d,%d,%d) src=%s hit=%u tool=%u", v[0],v[1],v[2], src, (unsigned)hit, tool); }
    }
}

static unsigned g_frame=0;
static void my_runcb() {
    if (g_orig_runcb) g_orig_runcb();
    // PASSIVE AUTO-ARM: the 0x847EE0 hook stamps g_cap_appstate + bumps g_cap_seen every frame the game
    // app-state updates. Resolve editor = *(app+0xCE0F8) when the workbench is the ACTIVE (build, not
    // simulate) state, validated by BOTH vtables. Additive: arm once + keep g_editor current; never disarm
    // (a stale ptr is harmless - applies safe_copy from it). apply_place still needs g_struct (g_have_struct).
    __try {
        static unsigned long long s_seen_prev=0; static int s_stale=99;
        unsigned long long seen=g_cap_seen;
        if(seen!=s_seen_prev){ s_seen_prev=seen; s_stale=0; } else if(s_stale<99) s_stale++;
        long in_bench=0;
        if(g_cap_appstate && s_stale<=3){
            unsigned long long app=g_cap_appstate, avt=0, editor=0, active=0, evt=0;
            if(safe_copy(&avt,(void*)app,8) && avt==g_base+VT_APPSTATE
             && safe_copy(&editor,(void*)(app+OFF_EDITOR_PTR),8) && editor
             && safe_copy(&active,(void*)(app+OFF_ACTIVE_STATE),8) && editor==active
             && safe_copy(&evt,(void*)editor,8) && evt==g_base+VT_EDITOR){
                g_editor=editor; g_arg3=editor+0x1588;
                in_bench=1;                    // the editor IS the active state -> the player is IN the bench
                if(!g_armed){ InterlockedExchange(&g_armed,1); logline("AUTO-ARMED (passive, bench open): editor=0x%llX", g_editor); }
            }
        }
        if(in_bench!=g_in_bench){
            if (in_bench && g_last_key && (GetTickCount()-g_last_key_time) < 4000) {
                long en = (g_last_key == ACT_INTERACT_RIGHT) ? 1 : 0;   // E = CO-OP, Q = SOLO (matches the prompt)
                InterlockedExchange(&g_sync_enabled, en);
                // SOURCE OF TRUTH = whoever got into the bench FIRST. If the partner is already building when
                // we press E, the prompt said "JOIN PARTNER", so pressing it IS consent to take their craft.
                // If they are not in yet, we pressed "START CO-OP" and OUR craft is the authoritative one.
                g_join_mode = (en && g_peer_in_bench) ? 1 : 0;
                logline("[mode] opened with %s -> sync %s%s", en?"E (CO-OP)":"Q (SOLO)", en?"ON":"OFF",
                        g_join_mode?" | JOINING partner (their craft wins)":(en?" | STARTING (our craft is the source)":""));
            } else if (in_bench) g_join_mode = 0;
            InterlockedExchange(&g_in_bench,in_bench); logline("[bench] %s workbench", in_bench?"ENTERED":"LEFT"); }
        { long mm = bench_mismatch()?1:0; if(mm!=g_bench_mismatch) InterlockedExchange(&g_bench_mismatch, mm); }   // HUD
    } __except(EXCEPTION_EXECUTE_HANDLER){ InterlockedExchange(&g_in_bench,0); }
    // RESYNC ON ENTRY (late join): walking into the bench while the partner is already building means we
    // missed their edits - so take a whole-craft snapshot instead of replaying increments. A pull REPLACES
    // the local craft, so it is automatic ONLY when there is nothing to lose; otherwise we prompt. (The Q
    // "join co-op" entry key will become the explicit consent gesture for the non-empty case.) The delay
    // lets the editor/vehicle finish initialising before we serialise/load anything.
    {
        static DWORD s_entered_at = 0; static bool s_pending = false;
        if (g_in_bench) { if (!s_entered_at) { s_entered_at = GetTickCount(); s_pending = true; } }
        else { s_entered_at = 0; s_pending = false; }
        if (s_pending && GetTickCount() - s_entered_at > 1500) {
            s_pending = false;
            // BENCH FINGERPRINT: log the craft's world anchor + size once the editor has settled. Different
            // workbenches sit at different WORLD positions, so these values identify WHICH bench we opened.
            // They are also the exact fields the pull position fix re-bases (vehicle+0x2D8 placement offset,
            // body+0x2B8/+0x2F8 transform translations), so this doubles as proof that those fields carry
            // real per-bench data rather than always reading zero.
            __try {
                unsigned long long ed = g_editor;
                char* V = ed ? *(char**)(ed+0x13C8) : nullptr;
                float  sz[3]  = {0,0,0};     // editor+0xD70  build volume SIZE in voxels (= metres * 4)
                double ctr[3] = {0,0,0};     // editor+0xD38  volume CENTRE, world metres  <- separates same-size benches
                double org[3] = {0,0,0};     // vehicle+0x1F0 world position of voxel (0,0,0) = centre + placement offset
                float  poff[3]= {0,0,0};     // vehicle+0x2D8 placement offset (f32) - usually 0, NOT the anchor
                unsigned long long grid=0, gvt=0;
                safe_copy(sz,  (void*)(ed+0xD70), 12);
                safe_copy(ctr, (void*)(ed+0xD38), 24);
                safe_copy(&grid,(void*)(ed+0x390), 8);
                if (grid) safe_copy(&gvt,(void*)grid,8);
                if (V) { safe_copy(org,(char*)V+0x1F0,24); safe_copy(poff,(char*)V+0x2D8,12); }
                logline("[bench] FINGERPRINT size=%.0fx%.0fx%.0f vox  centre=(%.2f,%.2f,%.2f)  voxel0=(%.2f,%.2f,%.2f)  poff=(%.3f,%.3f,%.3f)  grid=%p%s  nodes=%u",
                        sz[0],sz[1],sz[2], ctr[0],ctr[1],ctr[2], org[0],org[1],org[2], poff[0],poff[1],poff[2],
                        (void*)grid, (grid && gvt==g_base+0xAFFA08)?" OK":" ??", craft_node_count());
            } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[bench] fingerprint read failed"); }
            // JOIN always does a full sync. Deliberately does NOT re-check g_peer_in_bench: the prompt said
            // "JOIN PARTNER" when the key was pressed, so the intent is explicit - a presence beacon
            // flickering during the 1.5s settle window must not silently skip the sync.
            if (g_join_mode && g_peerid && !g_localecho) {
                logline("[presence] JOINING partner -> pulling their craft (they were in the bench first)");
                __try{ pull_request(); }__except(EXCEPTION_EXECUTE_HANDLER){}
            }
            else if (g_peerid && !g_localecho && g_peer_presence_known && g_peer_in_bench) {
                {
                    unsigned n = craft_node_count();
                    if (n <= 1) { logline("[presence] partner is building and our craft is empty -> AUTO-RESYNC (pull)");
                                  __try{ pull_request(); }__except(EXCEPTION_EXECUTE_HANDLER){} }
                    else logline("[presence] partner is building - press F7 to load their craft (not auto-pulling: %u chunk(s) of local work would be replaced)", n);
                }
            }
        }
    }
    __try { drain_apply_queue(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // self-gated on g_in_bench
    // The diff scanners READ the live vehicle through g_editor every frame. Out of the bench that pointer is
    // stale, so scanning could fault or - worse - see reused memory as "changes" and broadcast garbage.
    // Gate every vehicle-touching pass on actually being in the bench.
    if (g_in_bench) {
        __try { drain_connection(); } __except(EXCEPTION_EXECUTE_HANDLER){}
        __try { conn_diff(); }        __except(EXCEPTION_EXECUTE_HANDLER){}   // detect + emit wire disconnects
        __try { prop_diff(); }        __except(EXCEPTION_EXECUTE_HANDLER){}   // detect + emit property/name changes
    }
    { static SHORT s_f5=0; SHORT f5=GetAsyncKeyState(VK_F5);   // F5 = serialize the local craft to a file (snapshot test)
      if((f5&0x8000)&&!(s_f5&0x8000)) __try{ snapshot_save_to_file(); }__except(EXCEPTION_EXECUTE_HANDLER){}
      s_f5=f5; }
    { static SHORT s_f4=0; SHORT f4=GetAsyncKeyState(VK_F4);   // F4 = LOAD the saved snapshot back (local file test)
      if((f4&0x8000)&&!(s_f4&0x8000)) __try{ snapshot_load_from_file(); }__except(EXCEPTION_EXECUTE_HANDLER){}
      s_f4=f4; }
    // 2-machine PULL (all main-thread): serialize+send if the peer requested; load if a full craft arrived.
    if (InterlockedCompareExchange(&g_pull_send_req,0,0)) { InterlockedExchange(&g_pull_send_req,0);
        __try{ pull_send_craft(); }__except(EXCEPTION_EXECUTE_HANDLER){ logline("[pull] send EXC"); } }
    __try{ pull_send_pump(); }__except(EXCEPTION_EXECUTE_HANDLER){ logline("[pull] pump EXC"); }   // paced chunk sender
    if (InterlockedCompareExchange(&g_pull_rx_ready,0,0)) { InterlockedExchange(&g_pull_rx_ready,0);
        __try{
            if(g_pull_rx && g_pull_rx_total && g_pull_rx_got == g_pull_rx_total)
                snapshot_load_from_buffer(g_pull_rx,g_pull_rx_total);
            else logline("[pull] REFUSING to load an incomplete craft (%u/%u bytes) - it would corrupt the vehicle",
                         g_pull_rx_got, g_pull_rx_total);
        }__except(EXCEPTION_EXECUTE_HANDLER){ g_suppress=0; logline("[pull] load EXC"); }
        if(g_pull_rx){ VirtualFree(g_pull_rx,0,MEM_RELEASE); g_pull_rx=nullptr; g_pull_rx_total=0; g_pull_rx_got=0; }
        InterlockedExchange(&g_sync_busy,0); }
    // give up the banner if the partner never answers, rather than showing "syncing" forever
    if (g_sync_busy && !g_sync_total && (long)GetTickCount()-g_sync_started > 15000) {
        logline("[pull] no response from partner after 15s - giving up");
        sync_error("partner did not respond"); }
    { static SHORT s_f7=0; SHORT f7=GetAsyncKeyState(VK_F7);   // F7 = LOAD PEER'S CRAFT (pull request over Steam)
      if((f7&0x8000)&&!(s_f7&0x8000)) __try{ pull_request(); }__except(EXCEPTION_EXECUTE_HANDLER){}
      s_f7=f7; }
    // F8 disabled again: 0x4AE3F0 is the per-BODY serializer (writes body+0x147c/+0x288/+0x2c8/+0x718), NOT
    // per-component; the real per-component path is 4 levels deep (0x4AE3F0 -> 0x4B9230 component-list ->
    // factory-DTO lifecycle per comp). Deferred - the F7 PULL already carries all properties in bulk.
    (void)&comp_selftest;
    __try { watch_interact_key(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // remember E/Q for the next bench open
    __try { update_workbench_prompt(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // START CO-OP vs JOIN PARTNER
    __try { sample_cursor(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // publish our hover voxel for the overlay
    __try { sample_bench_origin(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // auto-calibrate the overlay to THIS bench
    __try { sample_camera_world(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // ...and the floating-origin rebase
    { static SHORT s_f8=0; SHORT f8=GetAsyncKeyState(VK_F8);   // F8 = SOLO cursor self-test (own cursor, 1s delayed)
      if((f8&0x8000)&&!(s_f8&0x8000)) {
          long v = g_cursor_selftest ? 0 : 1; InterlockedExchange(&g_cursor_selftest, v);
          logline("[cursor] SOLO self-test %s - your own cursor replays as the partner marker, ~1s behind", v?"ON":"OFF"); }
      s_f8=f8; }
    if ((++g_frame % 6)==0 && g_in_bench) __try { paint_diff(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // ~10Hz repaint scan
}

// ======================= ARM (from 0x7F7EB0) =======================
// The single-click place-cmd carries the editor object, arg3, and the transient placement
// struct [r9] our apply-forge needs. Capture them ONCE (first single-click) to bootstrap the
// apply path. Processed inside detect_worker BEFORE any send so a single click arms in time
// for its own echo (fixes the arm/send race). No send from here.
static void try_arm() {
    // NOTE: the g_struct forge template is captured here INDEPENDENTLY of the arm state - passive auto-arm
    // (my_runcb) may have already set g_armed with no g_struct, so we must still grab the template on the
    // first local placement (it gates apply_place via g_have_struct).
    // (1) single-click - 0x7F7EB0 captured editor (rcx) + arg3 (r8) + the [r9] placement struct.
    if (g_cap_flag == 1) {
        unsigned long long editor=g_cap_rcx, arg3=g_cap_r8;
        BYTE st[0x80]; memcpy(st, g_r9buf, 0x80);
        g_cap_flag = 0;
        memcpy(g_struct, st, 0x80); g_have_struct = 1;    // capture the forge template (needed by apply_place)
        if (!g_armed) {
            g_editor=editor; g_arg3=arg3;
            g_da_arm_needed = 0;                          // stop the drag/factory auto-arm captures
            InterlockedExchange(&g_armed, 1);
            logline("ARMED (single-click): editor=0x%llX arg3=0x%llX - apply path ready", editor, arg3);
        }
    }
    // (2) click-DRAG: editor from 0x7F3440, struct from the factory 0x45EB50. arg3 = editor+0x1588 (confirmed).
    // Validate the struct's +0x10 helper pointer before trusting the captured struct.
    if (g_da_arm_needed && g_da_editor && g_da_flag) {
        unsigned long long O=0; memcpy(&O, g_da_struct + 0x10, 8);
        if (O > 0x10000ULL && O < 0x7FFFFFFFFFFFULL) {
            memcpy(g_struct, g_da_struct, 0x80); g_have_struct = 1;   // capture the forge template
            if (!g_armed) {
                g_editor = g_da_editor; g_arg3 = g_da_editor + 0x1588;
                InterlockedExchange(&g_armed, 1);
                logline("AUTO-ARMED (click-drag, no single-click): editor=0x%llX arg3=0x%llX", g_editor, g_arg3);
            }
            g_da_arm_needed = 0;
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
            pcache_seed_neighbors(xyz[0],xyz[1],xyz[2]);   // pick up pre-existing adjacent blocks (origin block etc.)
            prop_track(xyz[0],xyz[1],xyz[2], name);   // watch this component's properties (rules gated by def)
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
        // AUTO-CONNECT: while we have no partner (and none was pinned in coop-peer.txt), look for one ~1s.
        // AUTO-CONNECT IS OPT-IN. It reads the session player list, so it stays OFF unless the user creates
        // coop-autoconnect.txt next to the DLL. That way simply having the mod loaded on a public server
        // never touches other players' data - you enable it deliberately, in a session with your partner.
        if (g_autoconnect_on && g_net && !g_peerid && !g_manual_peer && !g_localecho && (tick % 50)==0)
            __try { roster_discover(); } __except(EXCEPTION_EXECUTE_HANDLER){}
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
            // presence beacon: immediately on change, plus a ~1s heartbeat so a peer that connects late
            // (or missed the edge) still learns whether we are in the bench.
            { static long s_last_sent = -1;
              long now_in = g_in_bench;
              if (now_in != s_last_sent || (tick % 50) == 0) { s_last_sent = now_in; emit_presence(now_in); } }
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
                // PULL messages (kind=8 request / kind=9 craft chunk) - variable/large; peek magic + kind@offset 5
                { uint32_t mg=0; uint8_t kd=0;
                  if(safe_copy(&mg,data,4) && mg==MAGIC && safe_copy(&kd,(BYTE*)data+5,1) && (kd==8||kd==9||kd==12||kd==13||kd==14)) {
                      if(kd==12||kd==13){ handle_hello(mm, data, sz, kd); if(p_release) p_release(mm); continue; }
                      if(kd==14){   // presence beacon: is the partner in the workbench?
                          PlaceMsg pm;
                          if(sz>=(int)sizeof(PlaceMsg) && safe_copy(&pm,data,sizeof pm)){
                              long was=g_peer_in_bench, nowv=pm.aux?1:0;
                              InterlockedExchange(&g_peer_in_bench, nowv);
                              InterlockedExchange(&g_peer_presence_known, 1);
                              g_peer_presence_last=GetTickCount();
                              bool szchg = (g_peer_bench[0]!=pm.x||g_peer_bench[1]!=pm.y||g_peer_bench[2]!=pm.z);
                              g_peer_bench[0]=pm.x; g_peer_bench[1]=pm.y; g_peer_bench[2]=pm.z;
                              if(was!=nowv) logline("[presence] partner %s the workbench", nowv?"ENTERED":"LEFT");
                              if(szchg && pm.x){   // compare benches: differing volumes = blocks that fit one but not the other
                                  int mine[3]; bench_size_vox(mine);
                                  if(mine[0] && (mine[0]!=pm.x||mine[1]!=pm.y||mine[2]!=pm.z))
                                      logline("[presence] !! BENCH MISMATCH - yours %dx%dx%d vox, partner %dx%dx%d. Blocks outside the smaller box will not place. Use the same bench type.",
                                              mine[0],mine[1],mine[2], pm.x,pm.y,pm.z);
                                  else if(mine[0])
                                      logline("[presence] bench match: both %dx%dx%d vox", mine[0],mine[1],mine[2]);
                              }
                          }
                      }
                      else if(kd==8){ logline("[pull] <<< peer requested our craft"); InterlockedExchange(&g_pull_send_req,1); }
                      else if(sz>=(int)sizeof(ChunkHdr)){
                          char* cb=(char*)VirtualAlloc(nullptr,(SIZE_T)sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
                          if(cb){ if(safe_copy(cb,data,sz)) pull_rx_chunk(cb,sz); VirtualFree(cb,0,MEM_RELEASE); }
                      }
                      if (p_release) p_release(mm);
                      continue;
                  }
                }
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
    p_identClear = pClear; p_identSetID = pSetID;      // auto-connect needs these outside steam_init
    // ISteamFriends for auto-discovery. The game only imports a few Steam symbols, but steam_api64.dll
    // EXPORTS the whole flat API and the game already pumps RunCallbacks, so this just works.
    { auto pFrAcc = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamFriends_v017");
      g_friends = pFrAcc ? pFrAcc() : nullptr;
      p_fcount = (friendCount_t)GetProcAddress(s, "SteamAPI_ISteamFriends_GetFriendCount");
      p_fbyidx = (friendByIdx_t)GetProcAddress(s, "SteamAPI_ISteamFriends_GetFriendByIndex");
      p_fgame  = (friendGame_t) GetProcAddress(s, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
      p_hasfriend = (hasFriend_t)GetProcAddress(s, "SteamAPI_ISteamFriends_HasFriend");
      logline("auto-connect: friends=%p count=%p byidx=%p game=%p", g_friends,(void*)p_fcount,(void*)p_fbyidx,(void*)p_fgame); }
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
    if (g_peerid) InterlockedExchange(&g_manual_peer, 1);   // pinned by file -> never auto-discover over it
    // auto-connect is OPT-IN: it reads the session player list, so it must never run just because the mod
    // happens to be loaded (e.g. on a public server). Create coop-autoconnect.txt next to the DLL to enable.
    { char ap[MAX_PATH]; DWORD an=GetModuleFileNameA(g_hmod,ap,MAX_PATH);
      if (an && an<MAX_PATH) { char* s2=strrchr(ap,'\\'); if(s2)*(s2+1)=0;
          strncat_s(ap,MAX_PATH,"coop-autoconnect.txt",_TRUNCATE);
          if (GetFileAttributesA(ap)!=INVALID_FILE_ATTRIBUTES) { InterlockedExchange(&g_autoconnect_on,1);
              logline("auto-connect: ENABLED (coop-autoconnect.txt present) - will pair with a FRIEND in your session"); }
          else logline("auto-connect: off (create coop-autoconnect.txt next to the DLL to enable)");
          char* s3=strrchr(ap,'\\'); if(s3)*(s3+1)=0; strncat_s(ap,MAX_PATH,"coop-probe.txt",_TRUNCATE);
          if (GetFileAttributesA(ap)!=INVALID_FILE_ATTRIBUTES) { InterlockedExchange(&g_probe_on,1);
              logline("auto-connect: diagnostic memory probe ENABLED (coop-probe.txt)"); } } }
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

// ======================= WORKBENCH PROMPT TEXT =======================
// Change the "Create Vehicle" line under the workbench prompt, IN MEMORY only (no game files touched).
// It is localisation id 0x623 in the table at base+0xD04E10 (stride 0x18):
//     +0x00 const char* default literal   +0x08 char* live buffer   +0x10 u32 length
// The reader uses +0x00 when length==0, else the live buffer at +0x08 - so we zero the length FIRST and then
// swap the pointer. Ordering matters: a HUD thread reading mid-patch sees length 0 and follows +0x00, which
// is a valid NUL-terminated string both before and after the store, so there is no torn read.
// The table lives in .data (already READ|WRITE - no VirtualProtect, no code patched, nothing executable).
// Verified: id 0x623 has exactly ONE consumer in the whole binary, inside the workbench prompt branch, so
// this cannot leak into any other menu. Fully restored on unload (our string lives in THIS dll, so leaving it
// patched after FreeLibrary would dangle - unpatch is mandatory, not cosmetic).
static const ULONGLONG LOC_TABLE  = 0xD04E10;
static const unsigned  LOC_STRIDE = 0x18;
static const unsigned  LOC_CREATE_VEHICLE = 0x623;
// The prompt is live: it re-points as the partner's state changes (the swap is just a pointer store).
// No "###" in any of these - that is the game's key-glyph substitution token.
static const char PROMPT_NOPEER[] = "Create Vehicle    [E] CO-OP    [Q] SOLO";
static const char PROMPT_START[]  = "Create Vehicle    [E] START CO-OP    [Q] SOLO";
static const char PROMPT_JOIN[]   = "Create Vehicle    [E] JOIN PARTNER    [Q] SOLO";
static const char* PROMPT_TEXT = PROMPT_NOPEER;
static void*    g_loc_saved_ptr = nullptr;
static unsigned g_loc_saved_len = 0;
static bool     g_loc_patched   = false;
static void patch_workbench_prompt() {
    __try {
        char* e = (char*)(g_base + LOC_TABLE + (ULONGLONG)LOC_CREATE_VEHICLE * LOC_STRIDE);
        char* cur = *(char**)(e + 0x00);
        unsigned len = *(unsigned*)(e + 0x10);
        if (!cur) { logline("[prompt] loc entry empty - not patching"); return; }
        char probe[40]={0};
        if (!safe_copy(probe, cur, 32)) { logline("[prompt] loc entry unreadable - not patching"); return; }
        // Refuse to patch if it is not the string we expect (game update / wrong offset) rather than
        // corrupting some unrelated menu text.
        if (strncmp(probe, "Create Vehicle", 14) != 0) {
            logline("[prompt] loc 0x623 is '%.31s', not 'Create Vehicle' - NOT patching (game updated?)", probe); return; }
        g_loc_saved_ptr = cur; g_loc_saved_len = len;
        *(unsigned*)(e + 0x10) = 0;                 // force the reader onto +0x00 BEFORE we swap it
        MemoryBarrier();
        *(const char**)(e + 0x00) = PROMPT_TEXT;
        g_loc_patched = true;
        logline("[prompt] workbench prompt patched -> \"%s\"", PROMPT_TEXT);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[prompt] patch faulted - prompt unchanged"); }
}
// Re-point the prompt as the partner's state changes: nobody paired -> plain CO-OP; paired but not building
// -> START CO-OP; partner already in their bench -> JOIN PARTNER. Length is already 0 from the initial patch,
// so this is a single aligned pointer store - the reader either sees the old or the new string, never a tear.
static void update_workbench_prompt() {
    if (!g_loc_patched) return;
    const char* want = g_peer_in_bench ? PROMPT_JOIN : (g_peerid ? PROMPT_START : PROMPT_NOPEER);
    static const char* s_cur = nullptr;
    if (want == s_cur) return;
    s_cur = want;
    __try {
        char* e = (char*)(g_base + LOC_TABLE + (ULONGLONG)LOC_CREATE_VEHICLE * LOC_STRIDE);
        *(unsigned*)(e + 0x10) = 0;
        MemoryBarrier();
        *(const char**)(e + 0x00) = want;
        logline("[prompt] -> \"%s\"", want);
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}
static void unpatch_workbench_prompt() {
    if (!g_loc_patched) return;
    __try {
        char* e = (char*)(g_base + LOC_TABLE + (ULONGLONG)LOC_CREATE_VEHICLE * LOC_STRIDE);
        *(unsigned*)(e + 0x10) = 0;                 // park on +0x00 while we restore the pointer
        MemoryBarrier();
        *(void**)(e + 0x00) = g_loc_saved_ptr;
        MemoryBarrier();
        *(unsigned*)(e + 0x10) = g_loc_saved_len;
        g_loc_patched = false;
        logline("[prompt] workbench prompt restored");
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

// restore all inline hooks + the IAT slot to original (for hot unload)
static void unhook_all() {
    unpatch_workbench_prompt();   // MUST run before FreeLibrary - our string lives in this DLL
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
    g_tramp_appstate = install_hook(g_base + APPUPD_OFF, STEAL_APPUPD, (void*)&DetourAppState, g_orig_appstate);   // PASSIVE auto-arm capture
    g_tramp_interact = install_hook(g_base + INTERACT_OFF, STEAL_INTERACT, (void*)&DetourInteract, g_orig_interact); // E/Q capture
    { unsigned char* ib=(unsigned char*)(g_base+INTERACT_OFF);
      logline("  interact hook @base+0x%llX tramp=0x%llX first bytes now %02X %02X %02X (orig %02X %02X %02X)",
              (unsigned long long)INTERACT_OFF, g_tramp_interact, ib[0],ib[1],ib[2],
              g_orig_interact[0],g_orig_interact[1],g_orig_interact[2]); }
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
    patch_workbench_prompt();                // "[E] CO-OP  [Q] SOLO" on the workbench prompt
    DeleteFileA(CMDP);                       // clear any stale command
    spawn(detect_worker);                     // handles both arm + detect+emit
    spawn(del_worker);                        // delete detection
    // hot-unload watcher: launched OUTSIDE g_threads on purpose. It is the thread that runs the
    // unload's WaitForMultipleObjects(g_threads, bWaitAll), so it must NOT be in that set - a thread
    // waiting on its own never-signaled handle would spin the "never free on timeout" loop forever.
    CreateThread(nullptr, 0, cmd_watcher, nullptr, 0, nullptr);
    steam_init();
    logline("Coop Workbench by BayneBuild " COOP_VERSION " EXPERIMENTAL (built " __DATE__ " " __TIME__ ")");
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
