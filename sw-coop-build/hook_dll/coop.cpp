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
    unsigned long long g_da_editor=0, g_da_flag=0, g_da_arm_needed=1, g_da_ptr=0;
    unsigned long long g_tramp_dragarm=0, g_tramp_factory=0;
    unsigned char g_da_struct[0x80] = {0};
    // detour_appstate.asm (0x847EE0 c_application_state_game::update): PASSIVE auto-arm - stash the app-state
    // pointer + bump a seen-counter every frame so my_runcb resolves the editor and arms with NO local edit.
    void DetourAppState();
    unsigned long long g_cap_appstate=0, g_cap_game=0, g_cap_seen=0, g_tramp_appstate=0;
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
    unsigned long long g_suppress=0;   // COUNTER, not a flag - see suppress_push/pop
}

// g_suppress gates every detect hook. It was a 0/1 FLAG, so a nested apply - and a splice runs a whole craft
// load inside the RunCallbacks apply context, where the place/delete/connect detours all fire - would have
// the INNER operation clear it on completion while the OUTER one was still running, re-broadcasting our own
// applied edits as if the player had made them. Counter semantics fix that. The asm detours are unchanged:
// they only ever test "!= 0".
static void suppress_push() { InterlockedIncrement64((volatile LONG64*)&g_suppress); }
static void suppress_pop()  { if (g_suppress) InterlockedDecrement64((volatile LONG64*)&g_suppress); }
static volatile long g_ps_dirty = 0;   // a craft reload happened -> the property baseline is stale

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
static volatile long g_flush_pending = 0; // set by try_arm (worker thread) -> my_runcb replays held places on MAIN
// 1 while the workbench editor is the ACTIVE game state (i.e. the player is really IN the bench, not walking
// the world / in the sim). Unlike g_armed this CLEARS on exit. wsdraw.cpp reads it (same DLL, C linkage).
extern "C" volatile long g_in_bench = 0;
static int g_peer_bench[3] = {0,0,0};     // partner's build volume in voxels (from their presence beacon)
extern "C" volatile long g_cursor_selftest;   // RETIRED (always 0). Nothing toggles it; it now only gates
                                              // some verbose cursor logging. F8 is the log viewer.
static void logline(const char* fmt, ...);    // fwd (defined below; used by the interact-key watcher)
static void crumb(const char* what);          // fwd (crash-report breadcrumb: what the mod was doing)
static void crash_filter_keepalive();         // fwd (re-asserts our crash handler if the game replaced it)
static void update_workbench_prompt();        // fwd (live prompt text: CO-OP / START CO-OP / JOIN PARTNER)
static void patch_workbench_prompt();         // fwd (retried from my_runcb when loaded at game start)
static bool g_loc_patched = false;            // 1 once the workbench prompt string has been swapped
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
// Offset of the placement struct WITHIN the editor object, if it lives there.
// Why this matters: we do not build a place-command, we replay one - 0x7F7EB0 takes r9 = a 0x80-byte
// placement struct of which we only understand ~24 bytes (voxel, rotation, template ptr, colour); the rest
// includes a live pointer at +0x10 the game validates. So we copy a real struct and rewrite our fields. That
// is why a local placement was needed first, and why the first inbound block was lost on BOTH machines.
// If r9 is simply a scratch field inside the editor (very likely - the caller has to keep it somewhere), the
// same bytes are readable at any moment for free. We learn the delta from the first placement, persist it,
// and from then on capture the template passively at arm - no placement, ever again.
static unsigned g_tmpl_off = 0;           // 0 = not learned yet
static char     g_tmploffp[MAX_PATH];     // coop-template-off.txt, beside the DLL

// ---- files (resolved NEXT TO THE DLL at load, so the mod is portable to any machine) ----
// Overlay entry points. wsdraw.cpp compiles into this same DLL; these are declared here rather than beside
// the rest because logline() - one of the earliest things to run - feeds the in-game log viewer.
extern "C" void wsdraw_log_push(const char* text);   // in-game log viewer ring (F8)
extern "C" void wsdraw_boot_step(const char* text, int status);   // startup sequence (declared early: the
                                                                 // self-test warning fires before setup)
extern "C" void wsdraw_log_init();
extern "C" void wsdraw_toast(const char* text);   // brief centred on-screen message
static unsigned pc_expect_components();           // fwd (component count across every body)

static HMODULE g_hmod = nullptr;
static char LOGP[MAX_PATH]  = "coopworkbench-log.txt";     // fallback (cwd) if resolution fails
static char PEERP[MAX_PATH] = "coop-peer.txt";    // put the PEER's SteamID64 here
static char CMDP[MAX_PATH]  = "coopworkbench-cmd.txt";     // write "unload" here to hot-unload the mod
static char SELFP[MAX_PATH] = "coop-selftest.txt";   // SOLO test mode - see selftest_init()
static void init_paths() {
    char mod[MAX_PATH]; DWORD n = GetModuleFileNameA(g_hmod, mod, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* slash = strrchr(mod, '\\');
    if (!slash) return;
    *(slash+1) = 0;                               // mod = DLL directory with trailing backslash
    // WRITABLE FALLBACK. Everything resolves next to the DLL, which under an ASI install means
    // <game>\plugins\ - typically under Program Files, where writes fail. logline() swallows that
    // (`if(!f) return;`), so a failed ASI install would produce NO LOG AT ALL, which is the worst possible
    // posture for an untested install path. Probe writability once and fall back to %LOCALAPPDATA%.
    {
        char probe[MAX_PATH];
        _snprintf_s(probe, MAX_PATH, _TRUNCATE, "%scoopworkbench-log.txt", mod);
        FILE* t=nullptr;
        if (fopen_s(&t, probe, "a") || !t) {
            char* la = nullptr; size_t lasz = 0;
            if (_dupenv_s(&la, &lasz, "LOCALAPPDATA") == 0 && la) {
                _snprintf_s(mod, MAX_PATH, _TRUNCATE, "%s\\CoopWorkbench\\", la);
                CreateDirectoryA(mod, nullptr);
                free(la);
            }
        } else fclose(t);
    }
    _snprintf_s(LOGP,  MAX_PATH, _TRUNCATE, "%scoopworkbench-log.txt",  mod);
    _snprintf_s(PEERP, MAX_PATH, _TRUNCATE, "%scoop-peer.txt", mod);
    _snprintf_s(CMDP,  MAX_PATH, _TRUNCATE, "%scoopworkbench-cmd.txt",  mod);
    _snprintf_s(SELFP, MAX_PATH, _TRUNCATE, "%scoop-selftest.txt", mod);
    _snprintf_s(g_tmploffp, MAX_PATH, _TRUNCATE, "%scoop-template-off.txt", mod);
    // Reload the learned placement-struct offset, so only the FIRST session on a machine ever needs a local
    // placement to bootstrap the forge template. Sanity-bounded: a corrupt file must not send us reading at a
    // wild offset inside the editor.
    FILE* tf=nullptr;
    if(!fopen_s(&tf,g_tmploffp,"r") && tf){
        unsigned v=0; if(fscanf_s(tf,"%u",&v)==1 && v>0 && v<0x20000u) g_tmpl_off=v;
        fclose(tf);
    }
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
    uint32_t body_uid;// SENDER's body unique_id (body+0x147c). 0 = unknown -> the receiver falls back.
                      // Body uids are serialized (0x4AE47C) and restored on load, so both machines agree
                      // after any F7 pull. Without this the receiver has no way to know which body a
                      // partner's block joined - see pick_body_for.
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
// Rich Presence: the intended auto-connect mechanism. A vanilla friend never sets our key, so this is an
// EXACT "is that person running this mod" test rather than a heuristic - and reading a friend's presence is
// a LOCAL Steam-client cache read, so nothing is sent to anybody. SteamID goes BY VALUE as uint64.
typedef bool        (*setRP_t)(void*, const char*, const char*);
typedef const char* (*getFriendRP_t)(void*, uint64_t, const char*);
static void* g_friends=nullptr;
static friendCount_t p_fcount=nullptr;
static friendByIdx_t p_fbyidx=nullptr;
static friendGame_t  p_fgame=nullptr;
static hasFriend_t   p_hasfriend=nullptr;
static setRP_t       p_setrp=nullptr;
static getFriendRP_t p_getfrp=nullptr;
static const char*   RP_KEY = "swcoop";        // ONE key; the ~20-key budget is shared with the game's own
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
// Read by the overlay so self-test mode is IMPOSSIBLE to leave on by accident. It was found still enabled in
// a live install, and with it on every send returns before the wire - a two-machine session would have
// transmitted nothing at all while both players watched phantom blocks appear beside their own. A log line
// is not enough for that; it needs to be on screen the whole time.
extern "C" volatile long g_selftest_on = 0;

// SOLO SELF-TEST. Everything the co-op path does needs two machines to observe - which is exactly why so
// much of this project sat "built, awaiting a tester". Local-echo closes that gap: the mod becomes its own
// partner, applying its own messages through the real encode/decode/apply path, with the target voxel
// shifted +X so the change lands on a DIFFERENT part and is therefore visible.
//
// Enabled by dropping `coop-selftest.txt` beside the mod, containing the shift in voxels (e.g. `2`).
// Deliberately a file rather than a key: it must be a decision made before launching, never something a
// player can trip into mid-session, since every edit gets duplicated while it is on.
static void selftest_init() {
    FILE* f = nullptr;
    if (fopen_s(&f, SELFP, "r") || !f) return;
    int dy = 0;
    if (fscanf_s(f, "%d", &dy) != 1) dy = 2;
    fclose(f);
    if (dy == 0) dy = 2;                       // 0 would echo onto the source and prove nothing
    if (dy < -64 || dy > 64) dy = 2;
    g_echo_dy = dy; g_localecho = true; InterlockedExchange(&g_selftest_on, 1);
    logline("=== SOLO SELF-TEST MODE (coop-selftest.txt) - echoing every edit to voxel +%d on X. ===", dy);
    logline("=== No Steam partner is used. Delete coop-selftest.txt for normal co-op. ===");
    wsdraw_boot_step("SELF-TEST MODE - nothing is sent to a partner", 2);
}

// ---- game fn typedefs ----
typedef void* (*getTmpl_t)(void*, void*);
typedef void* (*placeCmd_t)(void*, void*, void*, void*);
struct NameStr { const char* ptr; unsigned long long len; };  // fn reads [+0]=char*, [+8]=u32 len

static void logline(const char* fmt, ...) {
    // Format once, then fan out to the file AND the in-game viewer (F8). The file stays the artifact a bug
    // report carries; the viewer is what makes the log readable without leaving the game.
    char line[512];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(line, sizeof line, _TRUNCATE, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof line - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    wsdraw_log_push(line);
    FILE* f=nullptr; fopen_s(&f, LOGP, "a"); if(!f) return;
    fputs(line, f); fputc('\n', f); fclose(f);
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
struct ApplyItem { uint8_t kind; int32_t x,y,z,rot[9],aux; uint32_t color; uint8_t cat; uint32_t body_uid; uint16_t namelen; char name[MAXNAME]; };
static const int AQ_SIZE = 4096, AQ_MASK = 4095;   // big enough to hold a large area-drag burst
static ApplyItem g_aq[AQ_SIZE];
static void apply_delete(int x, int y, int z);   // fwd
static void apply_paint(int x, int y, int z, const uint32_t inl[4], const uint32_t face[], unsigned nface);   // fwd (kind=3 = repaint)
static void force_remesh(unsigned long long editor, void* comp);   // fwd
static bool peer_away();   // fwd (presence gate: TRUE only when the peer is KNOWN to be out of the bench)
static bool bench_mismatch();   // fwd (TRUE when both bench volumes are known and differ)
static bool sync_paused();      // fwd (peer_away || bench_mismatch - blocks all edit traffic)
static void bench_size_vox(int out[3]);              // fwd (build volume in voxels, editor+0xD70)
static void bench_centre(double out[3]);             // fwd (volume centre, editor+0xD38 - identifies WHICH bench)
static void bench_max_voxel(const int size[3], int out[3]);   // fwd (last legal voxel per axis)
static void apply_conn_add(const int va[3], const int vb[3], int type);   // fwd (kind=4 = connection add)
static void apply_conn_del(const int va[3], const int vb[3], int type);   // fwd (kind=5 = connection remove)
static void emit_disconn(const int va[3], const int vb[3], int type);     // fwd (send a disconnect)
static void apply_prop(const int xyz[3], int offset, uint32_t value);     // fwd (kind=6 = numeric property)
static void apply_move(const float want[3]);                             // fwd (kind=15 = whole-craft move)
static void apply_name(const int xyz[3], const char* s, int len);         // fwd (kind=7 = name/string)
static void prop_track(int x, int y, int z, const char* def);             // fwd (watch a component's properties, gated by def)
static volatile long g_aq_wr=0, g_aq_rd=0;

// Whole-craft move (kind 15). Writing the vehicle transform must happen on the MAIN thread like every other
// apply, so the receive worker only parks it here. The 3 floats ride in rot[0..2] as raw bit patterns - the
// same slot the wire message uses - so no queue-format change is needed.
static void queue_move(const float p[3]) {
    long wr = g_aq_wr;
    if (wr - g_aq_rd >= AQ_SIZE) return;
    ApplyItem* it = &g_aq[wr & AQ_MASK];
    memset(it, 0, sizeof *it);
    it->kind = 15;
    memcpy(&it->rot[0], p, 12);
    MemoryBarrier();
    g_aq_wr = wr + 1;
}

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
    it->body_uid=m->body_uid;
    uint16_t nl = m->namelen; if (nl > MAXNAME) nl = MAXNAME;
    it->namelen = nl; memcpy(it->name, name, nl);
    MemoryBarrier();                                 // publish item before advancing wr
    g_aq_wr = wr + 1;
}
// drained on the MAIN thread from the RunCallbacks hook
static void drain_apply_queue() {
    // SOLO MEANS SOLO. Q opens the bench in solo mode, but g_sync_enabled only ever gated OUTBOUND traffic -
    // a player who deliberately chose solo still had their craft rewritten by their partner, with no way to
    // stop it. Discard rather than queue: a solo session's whole point is that those edits never arrive.
    if (!g_sync_enabled) { g_aq_rd = g_aq_wr; return; }
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
        crumb("applying a partner edit");
        ApplyItem* it = &g_aq[g_aq_rd & AQ_MASK];
        if (it->kind == 15) { float wp[3]; memcpy(wp,&it->rot[0],12); apply_move(wp); }
        else if (it->kind == 2) { apply_delete(it->x, it->y, it->z); }
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
            PlaceMsg m; memset(&m, 0, sizeof m);
            m.magic=MAGIC; m.ver=2; m.kind=1; m.namelen=it->namelen;
            m.x=it->x; m.y=it->y; m.z=it->z; memcpy(m.rot, it->rot, 36); m.aux=it->aux; m.color=it->color; m.cat=it->cat;
            m.body_uid=it->body_uid;
            apply_place(&m, it->name);
        }
        g_aq_rd++;
    }
}

static void pc_net_prop(const BYTE* buf, int c);   // kind 16 - blob too large for the apply queue
// validate + dispatch a raw PlaceMsg buffer (shared by Steam recv and local-echo) -> queue for main thread
static void handle_place_msg(const BYTE* buf, int c) {
    if (c < (int)sizeof(PlaceMsg)) return;
    const PlaceMsg* m = (const PlaceMsg*)buf;
    if (m->magic != MAGIC) return;
    // kind 16 carries a whole serialized component (up to 32 KB), which does not fit ApplyItem - it takes
    // its own pending set rather than the ring.
    if (m->kind == 16) { pc_net_prop(buf, c); return; }
    if (m->kind < 1 || m->kind > 7) return;
    if ((int)(sizeof(PlaceMsg) + m->namelen) > c) return;
    enqueue_apply(m, (const char*)(buf + sizeof(PlaceMsg)));
}

// ======================= EMIT (send or local-echo) =======================
static void emit_place(const char* name, uint16_t nl, const int32_t* xyz, const int32_t* rot, int32_t aux, uint32_t color, uint8_t cat, uint32_t body_uid) {
    if (nl > MAXNAME) nl = MAXNAME;
    BYTE buf[sizeof(PlaceMsg)+MAXNAME];
    PlaceMsg* m = (PlaceMsg*)buf;
    memset(buf, 0, sizeof(PlaceMsg));
    m->magic=MAGIC; m->ver=2; m->kind=1; m->namelen=nl;
    m->x=xyz[0]; m->y=xyz[1]; m->z=xyz[2];
    memcpy(m->rot, rot, 36); m->aux=aux; m->color=color; m->cat=cat; m->body_uid=body_uid;
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

// ============ PLACEMENT-STRUCT ANATOMY (FINDINGS 23) ============
// The "0x80-byte placement struct" is really 0x18 LIVE BYTES:
//     struct Cell { int32 x, y, z; int32 pad; Body* body; };
// 0x7F7EB0 reads r9 exactly ONCE: 0x7F7ECD `mov rax,[r9+0x10]` then 0x7F7ED1 `mov r8d,[rax+0x147c]`.
// The factory 0x45EB50 reads it twice: 0x45F63D `movups xmm0,[r14]`  -> comp+0x18 (x,y,z,pad)
//                                      0x45F645 `movsd  xmm1,[r14+0x10]` -> comp+0x28 (the Body*)
// Nothing on this path touches +0x18..+0x7F, which is why forging from a captured template has always
// worked despite that tail holding stale stack pointers from a long-dead frame.
//
// +0x10 is a BODY, *not* the editor. That distinction is load-bearing: writing the editor there would make
// 0x7F7ED1 read a garbage dword out of the editor and pass it to the factory as the component's owning-body
// id (stored at comp+0x80), producing components parented to a body that does not exist - silent corruption
// rather than a clean fault. Confirmed live: [+0x10]+0x147c = 342 while editor+0x147c = 1, so they are
// definitively different objects and 342 has the shape of a body unique_id.
static const unsigned BODY_UID  = 0x147c;   // body unique_id (0x498B95 `mov [rsi+0x147c],edi`)
static const unsigned BODY_VEH  = 0x250;    // body's parent vehicle (0x4BB1F5 `mov [rbx+0x250],rsi`)
static const unsigned ED_HOVER_BODY = 0x1450; // editor's hover body - what the game's own callers copy in

// A body is trustworthy only if it claims OUR vehicle as parent and carries a sane unique_id. This is the
// guard that makes the difference between "constructed struct" and "corrupted craft": +0x147c is the exact
// dword 0x7F7ED1 will dereference, so prove it is readable and sane BEFORE handing the pointer over.
static bool body_ok(unsigned long long b, unsigned long long veh) {
    if (b < 0x10000ULL || b > 0x7FFFFFFFFFFFULL) return false;
    unsigned long long owner = 0; unsigned uid = 0;
    if (!safe_copy(&owner, (void*)(b + BODY_VEH), 8)) return false;
    if (!safe_copy(&uid,   (void*)(b + BODY_UID), 4)) return false;
    if (owner != veh) return false;
    return uid < 0x10000u;
}
// Source a live Body* the way the game itself does, with a deque fallback. Returns 0 if nothing is
// trustworthy yet (editor+0x1450 is NULL until the first hover - the editor ctor zeroes it at 0x7C8CEB).
// Which BODY should a peer's block join? (SS40)
//
// This used to read editor+0x1450 and call it "the hover body". It is really the Body* field of a 0x18-byte
// ghost CELL - the LOCAL player's cursor target, written every frame by the pick at 0x7EDB40 from a ray
// against our own camera. It has no relationship whatsoever to where a partner placed a block, and a run was
// observed choosing uid=342 where uid=1 was correct. A wrong body here writes both comp+0x28 (parent) and
// comp+0x80 (body uid), so the craft renders correctly and is structurally wrong - silent corruption.
//
// The game never DERIVES a body: it propagates the ray hit's body straight through (0x7F4AFA -> 0x7F4FBC,
// and 0x7F8180 copies it verbatim while recomputing only x/y/z). So there is nothing to replicate locally -
// a peer's placement has no local ray. The sender must tell us.
//
// Resolution order, most to least authoritative:
//   1. the sender's body uid, via the game's own find_body_by_uid (0x4BC010). Body uids are serialized
//      (0x4AE47C) and restored on load, so after any F7 pull both machines agree on them.
//   2. the body owning a face-neighbour of the target voxel - right whenever the block touches the craft.
//   3. the only body, when the craft has exactly one. Unambiguous, and the common case by far.
// editor+0x1450 is deliberately NOT in that list any more.
typedef void* (*findbody_t)(void*, const int*);

static unsigned long long pick_body_for(unsigned sender_uid, int vx, int vy, int vz, const char** src_out) {
    if (src_out) *src_out = "none";
    unsigned long long veh = 0;
    if (!g_editor || !safe_copy(&veh, (void*)(g_editor + 0x13C8), 8) || !veh) return 0;

    // 1. the sender's uid, resolved by the engine
    if (sender_uid) {
        unsigned long long b = 0;
        __try { int uid = (int)sender_uid;
                b = (unsigned long long)((findbody_t)(g_base + 0x4BC010))((void*)veh, &uid); }
        __except(EXCEPTION_EXECUTE_HANDLER) { b = 0; }
        if (b && body_ok(b, veh)) { if (src_out) *src_out = "sender body uid"; return b; }
    }

    // 2. a face neighbour of the target cell. The target itself is empty by definition, so probe around it.
    //    Prefer a neighbour whose body matches the sender's uid if we have one.
    {
        static const int N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
        unsigned long long first = 0;
        for (int i = 0; i < 6; i++) {
            unsigned long long comp = (unsigned long long)lookup_component(
                g_editor, vx + N[i][0], vy + N[i][1], vz + N[i][2]);
            if (!comp) continue;
            unsigned long long b = 0;
            if (!safe_copy(&b, (void*)(comp + 0x28), 8) || !body_ok(b, veh)) continue;
            unsigned u = 0; safe_copy(&u, (void*)(b + BODY_UID), 4);
            if (sender_uid && u == sender_uid) { if (src_out) *src_out = "neighbour (uid match)"; return b; }
            if (!first) first = b;
        }
        if (first) { if (src_out) *src_out = "neighbour voxel"; return first; }
    }

    // 3. the single-body case - unambiguous, and most crafts most of the time
    unsigned long long found = 0;                       // fallback: first valid body in the vehicle deque
    __try {
        unsigned nb=*(unsigned*)(veh+0x10), bc=*(unsigned*)(veh+0x08), bh=*(unsigned*)(veh+0x0C);
        void** bb=*(void***)(veh+0x00);
        if (bb && bc) for (unsigned i=0; i<nb && i<64 && !found; i++) {
            unsigned long long b=(unsigned long long)bb[(bh+i)%bc];
            if (body_ok(b, veh)) found = b;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    if (found && src_out) *src_out = "vehicle body deque";
    return found;
}
// Back-compat shim: the observe-only struct diff has no peer message behind it, so it asks with no hints.
static unsigned long long pick_body(const char** src_out) { return pick_body_for(0, 0, 0, 0, src_out); }
// OBSERVE-ONLY verification. Builds the struct we WOULD construct and diffs it against one the game really
// made, so construction can be proven against ground truth without ever being used. Solo-testable: place a
// block and read the log. "body MATCH" on every line means construction is correct end-to-end.
static void diff_place_struct(const BYTE* captured, unsigned long long editor) {
    unsigned long long veh=0; if (!safe_copy(&veh,(void*)(editor+0x13C8),8)) return;
    unsigned long long capb=0; memcpy(&capb, captured+0x10, 8);
    const char* src="none"; unsigned long long bilt = pick_body(&src);
    unsigned cu=0, bu=0;
    safe_copy(&cu,(void*)(capb+BODY_UID),4);
    if (bilt) safe_copy(&bu,(void*)(bilt+BODY_UID),4);
    logline("[fs] built body=%p uid=%u (%s) | captured body=%p uid=%u | %s, %s",
            (void*)bilt, bu, src, (void*)capb, cu,
            (bilt==capb) ? "body MATCH" : "body DIFFER",
            (bilt && bu==cu) ? "uid MATCH" : "uid DIFFER");
    // WHY did validation fail? pick_body returning 0 means body_ok() rejected every candidate, and the only
    // rejectable conditions are the parent-vehicle check and the uid sanity bound. Print the raw inputs so
    // the wrong assumption names itself instead of being guessed at. (Failing closed here is correct - it
    // declines to construct rather than forging with an unverified pointer - but it must be understood.)
    if (!bilt) {
        unsigned long long hover=0, capowner=0;
        safe_copy(&hover,(void*)(editor+ED_HOVER_BODY),8);
        bool got_owner = safe_copy(&capowner,(void*)(capb+BODY_VEH),8);
        logline("[fs]   editor+0x13C8 vehicle=%p | editor+0x1450 hover=%p", (void*)veh, (void*)hover);
        logline("[fs]   captured body+0x250 owner=%p (%s) -> %s vehicle",
                (void*)capowner, got_owner?"read ok":"UNREADABLE", (capowner==veh)?"MATCHES":"does NOT match");
        // Where does the captured body's owner value actually live in the editor, if anywhere? Same trick
        // that located struct+0x10 -> editor+0x1450 in the first place.
        if (got_owner && capowner) {
            int hits=0;
            for (unsigned o=0; o<0x8000 && hits<4; o+=8) {
                unsigned long long v=0;
                if (safe_copy(&v,(void*)(editor+o),8) && v==capowner) {
                    logline("[fs]   body owner %p found at editor+0x%X", (void*)capowner, o); hits++;
                }
            }
            if (!hits) logline("[fs]   body owner %p not found anywhere in the editor", (void*)capowner);
        }
        // And walk the vehicle deque reporting each body, so a bad deque read is distinguishable from a
        // genuine owner mismatch.
        __try {
            unsigned nb=*(unsigned*)(veh+0x10), bc=*(unsigned*)(veh+0x08), bh=*(unsigned*)(veh+0x0C);
            void** bb=*(void***)(veh+0x00);
            logline("[fs]   vehicle body deque: storage=%p cap=%u head=%u count=%u", (void*)bb, bc, bh, nb);
            if (bb && bc) for (unsigned i=0;i<nb && i<4;i++) {
                unsigned long long b=(unsigned long long)bb[(bh+i)%bc], ow=0; unsigned u=0;
                safe_copy(&ow,(void*)(b+BODY_VEH),8); safe_copy(&u,(void*)(b+BODY_UID),4);
                logline("[fs]     body[%u]=%p owner=%p uid=%u", i, (void*)b, (void*)ow, u);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[fs]   vehicle deque unreadable"); }
    }
}

// ======================= APPLY =======================
// DEFERRED PLACES. apply_place cannot forge until a local placement has captured the 0x80-byte template
// (g_have_struct). Inbound places before that used to be DROPPED, which is exactly the "the first block he
// placed never appeared on my screen, everything after it synced" symptom from the 2026-07-30 test - and it
// bit BOTH players, because both start a session with no template. Park them instead and replay in order the
// instant the template is captured. Bounded so a partner building alone against an unarmed peer cannot grow
// this without limit; oldest are dropped first and the loss is logged, never silent.
struct PendPlace { PlaceMsg m; char name[128]; };
static PendPlace g_pend[64];
static int g_pend_n = 0;
static void apply_place(const PlaceMsg* m, const char* name);
static void pend_place(const PlaceMsg* m, const char* name) {
    uint16_t nl = m->namelen; if (nl > 127) nl = 127;
    if (g_pend_n == 64) {                       // full - drop the oldest so the newest edits still land
        memmove(&g_pend[0], &g_pend[1], sizeof(PendPlace)*63); g_pend_n = 63;
        static DWORD s_lw=0; DWORD t=GetTickCount();
        if (t-s_lw>3000) { s_lw=t; logline("<<< deferred-place queue full - oldest dropped (place a block to flush)"); }
    }
    g_pend[g_pend_n].m = *m;
    memcpy(g_pend[g_pend_n].name, name, nl); g_pend[g_pend_n].name[nl] = 0;
    g_pend_n++;
    logline("<<< place held (%d queued) - no forge template yet; they replay the moment you place your first block", g_pend_n);
}
// Called right after g_have_struct goes 1. Copies the queue out first: apply_place can re-enter nothing here,
// but draining in place while indexing would be fragile if that ever changes.
static void flush_pending_places() {
    if (!g_pend_n) return;
    int n = g_pend_n; g_pend_n = 0;
    logline(">>> forge template captured - replaying %d held placement%s", n, n==1?"":"s");
    for (int i=0;i<n;i++) apply_place(&g_pend[i].m, g_pend[i].name);
}
// Is this template a microcontroller? vtable+0x120 is 0xCB6A0 (`mov rax,rcx; ret`) on the microprocessor
// class and 0x8FD00 (`xor eax,eax; ret`) on every other - the game's own class test, used at 0x7F7F0B.
// Asking the object rather than comparing a definition name means modded MC variants answer correctly too.
static bool is_microprocessor_template(void* tmpl) {
    if (!tmpl) return false;
    bool mc = false;
    __try {
        unsigned long long vt = *(unsigned long long*)tmpl;
        if (!vt) return false;
        unsigned long long fn = *(unsigned long long*)(vt + 0x120);
        mc = (fn == g_base + 0xCB6A0);
    } __except(EXCEPTION_EXECUTE_HANDLER) { mc = false; }
    return mc;
}

static void apply_place(const PlaceMsg* m, const char* name) {
    (void)&is_microprocessor_template;   // kept: it IS the right test, but only on a constructed component
    if (!g_armed) { pend_place(m, name); return; }
    // CONSTRUCT the placement struct when we have no captured one. This is the entire point of FINDINGS 23:
    // the struct is 0x18 live bytes {int32 x,y,z; int32 pad; Body* body}, and the only field we cannot get
    // from the network message is the Body*, which pick_body() sources from editor+0x1450 exactly as the
    // game's own callers do. Verified solo before being switched on: three placements logged
    // "body MATCH, uid MATCH" against structs the game really built.
    // Deliberately scoped to the !g_have_struct case ONLY - every placement that works today keeps using the
    // proven captured template, so the only behaviour this can change is behaviour that is currently broken
    // (the first inbound block of a session, which used to be dropped outright).
    if (!g_have_struct) {
        const char* src=nullptr;
        unsigned long long b = pick_body_for(m->body_uid, m->x, m->y, m->z, &src);
        if (!b) { pend_place(m, name); return; }      // no trustworthy body yet - defer, exactly as before
        memset(g_struct, 0, 0x80);                     // +0x18..+0x7F is never read; zero is honest
        memcpy(g_struct+0x10, &b, 8);
        g_have_struct = 1;
        logline(">>> CONSTRUCTED placement struct (body %p uid via %s) - no local placement was needed", (void*)b, src);
    }
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
    // TEMPLATE GUARD. The factory dispatches on template+0x2AC through a jump table bounded at 0x42
    // (0x45EB6E `movsxd rax,[rcx+0x2ac]` / 0x45EB75 `cmp eax,0x42` / 0x45EB78 `ja 0x45F6BF` -> returns NULL),
    // and 0x7F7EB0 does NOT null-check the result: 0x7F7EED `movups [rax+0x30],xmm0` faults immediately.
    // A modded or version-mismatched definition landing out of range would therefore crash rather than fail,
    // so refuse it here instead.
    { int kind=-1;
      if (!safe_copy(&kind,(char*)tmpl+0x2AC,4) || kind<0 || kind>0x42) {
          logline("  def '%s' has out-of-range kind %d (template+0x2AC) - refusing to forge (would AV)", nm, kind);
          return;
      } }
    // set rotation, forge placement struct, place under echo-suppress
    safe_copy((void*)(g_editor+ROT_OFF), m->rot, 36);
    BYTE forge[0x80]; memcpy(forge, g_struct, 0x80);
    ((int*)forge)[0]=m->x + g_echo_dy; ((int*)forge)[1]=m->y; ((int*)forge)[2]=m->z;  // loopback echo shifted +X (perpendicular to Y-Z beams -> no overlap)
    ((int*)forge)[3]=m->aux;   // placement-struct+0x0C -> component+0x24. NOTE: no reader for this field was
                               // found anywhere in the binary; sub-shapes actually come from the template
                               // variant array (template+0x40 cat byte), which is how we already resolve
                               // them. Carried verbatim only to stay byte-identical to the captured struct.
    // LATENT CRASH FIX. g_have_struct latches to 1 for the session, so the Body* baked into the captured
    // template at +0x10 goes DANGLING the moment the bodies are rebuilt - which an F7 pull does every time,
    // and an undo or body split can too. Forging then hands 0x7F7ED1 a stale pointer to dereference at
    // +0x147c. Re-validate every forge and re-source if needed; this is strictly safer than the old
    // unconditional reuse, and costs two reads.
    {
        unsigned long long veh=0, cur=0;
        memcpy(&cur, forge+0x10, 8);
        if (safe_copy(&veh,(void*)(g_editor+0x13C8),8) && veh && !body_ok(cur, veh)) {
            const char* src=nullptr; unsigned long long fresh = pick_body(&src);
            if (!fresh) { logline("<<< place deferred - cached body is stale and no valid body to re-source"); pend_place(m, name); return; }
            memcpy(forge+0x10, &fresh, 8);
            memcpy(g_struct+0x10, &fresh, 8);        // keep the template usable for the next forge too
            logline("<<< cached body was stale (%p) - re-sourced from %s -> %p", (void*)cur, src, (void*)fresh);
        }
    }
    placeCmd_t place=(placeCmd_t)g_place_fn;
    char rs[96]; rotstr(rs,sizeof(rs),m->rot);
    int px = m->x + g_echo_dy, py = m->y, pz = m->z;   // the voxel we actually forged at
    // CONFLICT/IDEMPOTENCY guard: if a block already occupies the target voxel (e.g. both players placed
    // on the same cell at once), skip the forge - re-placing on an occupied cell can AV the place-cmd.
    // The existing block stays (first-placed wins); a hash/snapshot resync reconciles genuine conflicts.
    if (lookup_component(g_editor, px, py, pz)) { logline("  place (%d,%d,%d) '%s' skipped - voxel already occupied", px,py,pz, nm); return; }
    suppress_push();
    __try {
        // ARG 3 IS A MICROPROCESSOR DEFINITION, not an opaque context (SS41). At 0x7F7F0B the place command
        // asks "am I a microcontroller?" and, if so, COPIES arg3's definition into the new component
        // (0x7F7F36 -> 0x38FCF0) before the grid insert. We pass editor+0x1588 - the LOCAL player's currently
        // selected microcontroller. So a partner's MC does not arrive empty: it arrives containing OUR
        // microcontroller. Silent, and worse than the "default size" this was believed to be.
        // Not fixed here on purpose: the fix is to hand it the sender's definition, and whether 0x7F7EB0
        // tolerates a caller-owned scratch definition is explicitly untested. Shipping an unverified write
        // into the editor is how this project has previously turned a wrong result into a crash. Make it
        // LOUD instead, so nobody mistakes it for working, and fix it behind a solo probe.
        // Match on the DEFINITION NAME, not vtable+0x120. That test is the game's own, but it is asked of the
        // constructed COMPONENT (0x7F7F0B tests the object the factory just returned) - a template is a
        // different class, so asking it there returned false every time and the warning never fired once.
        // A false negative on a corruption warning is worse than no warning at all.
        if (nm && _strnicmp(nm, "microprocessor", 14) == 0)
            logline("!!! '%s' is a MICROCONTROLLER - its contents and size will be WRONG on this machine "
                    "(a known limitation). Press F7 to pull the real one.", nm);
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
    suppress_pop();
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
    suppress_push();
    __try {
        rbuild(comp, region);                                                          // region BEFORE delete
        rem((void*)grid, (void*)ed8, (void*)(gobj+0x64c8), (void*)(gobj+0xbaed8), comp, &out1, &out2, arg8);
        rmerge((void*)ed8, region, (void*)grid, flag);                                 // merge dirty chunks AFTER
        if (*(unsigned*)(region+0x08) != 0) ffree(*(void**)region);
        logline("  <<< APPLIED DELETE (%d,%d,%d) + remesh", tx,ty,tz);
    }
    __except(EXCEPTION_EXECUTE_HANDLER){ logline("  delete apply EXC 0x%lX", GetExceptionCode()); }
    suppress_pop();
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
    suppress_push();
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
    suppress_pop();
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
    suppress_push();
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
    suppress_pop();
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
    suppress_push();
    __try {
        erasefn((void*)d, (unsigned)found);
        logline("  <<< APPLIED DISCONN idx=%d (%d,%d,%d)-(%d,%d,%d) t=%d", found, A[0],A[1],A[2], B[0],B[1],B[2], type);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  disconn apply EXC 0x%lX", GetExceptionCode()); }
    suppress_pop();
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
    suppress_push();
    __try {
        safe_copy((void*)((unsigned long long)comp+offset),&value,4);
        logline("  <<< APPLIED prop (%d,%d,%d) +0x%X=%08X",px,py,pz,offset,value);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("  prop apply EXC 0x%lX",GetExceptionCode()); }
    suppress_pop();
    PropCell* c=prop_find(px,py,pz); if(c&&c->init){ for(int i=0;i<NRULES;i++) if(PROP_RULES[i].offset==offset){ c->snap[i]=value; break; } }
}
static void apply_name(const int xyz[3],const char* s,int len){
    if(!g_armed||!g_editor) return; if(len<0)len=0; if(len>MAXNAME)len=MAXNAME;
    int px=xyz[0]+g_echo_dy,py=xyz[1],pz=xyz[2];
    void* comp=lookup_component(g_editor,px,py,pz); if(!comp){ logline("  name: no comp"); return; }
    unsigned long long c=(unsigned long long)comp;
    suppress_push();
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
    suppress_pop();
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
extern "C" void wsdraw_set_peer(unsigned long long id);   // wsdraw.cpp - same DLL, overlay's own peer/session
extern "C" void wsdraw_boot_resolve(const char* prefix, const char* text, int status);
static void adopt_peer(uint64_t id) {
    if (!id || id==g_myid || g_peerid) return;              // already paired / nonsense
    if (!p_identClear || !p_identSetID) return;
    p_identClear(g_peerIdent); p_identSetID(g_peerIdent, id);
    MemoryBarrier();                                        // identity fully written before we publish the id
    g_peerid = id;
    if (p_accept) p_accept(g_net, g_peerIdent);
    // DO NOT persist an auto-adopted peer to coop-peer.txt. That file means "I chose this partner", and any
    // id in it sets g_manual_peer, which is the flag that switches discovery OFF (see the auto-connect gate).
    // Writing here therefore made auto-connect DISABLE ITSELF after its first success: one discovery became a
    // permanent pin to that person, and the mod never looked for anyone again - even after they stopped
    // playing. Observed live as "it says connected but nobody is there", with the stale pin surviving across
    // launches and no way to guess why. Discovery takes seconds; it does not need a cache, and a cache that
    // silently outranks it is worse than none.
    logline("auto-connect: adopted a partner for THIS session only (not written to coop-peer.txt - "
            "discovery runs again next launch)");
    // Tell the overlay half of the DLL too. It reads coop-peer.txt only at init, so on an auto-connected
    // session it would otherwise sit on peer=0 for the whole run - HUD "Partner: none", no partner camera,
    // no partner cursor - even though sync itself worked. That mismatch is what made the 2026-07-30 test
    // look like a failed connection when the pairing had in fact succeeded.
    wsdraw_set_peer(id);
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
// Publish "I am running this mod" to friends who are also in Stormworks. Steam pushes rich presence only to
// friends running the SAME AppID, and only the fact that we set a key - no packets go to anyone, and a
// vanilla friend simply never has this key. NEVER call ClearRichPresence(): it wipes every key including the
// game's own; a single key is removed with SetRichPresence(key, NULL).
static void rp_publish() {
    if (!p_setrp || !g_friends || !g_myid) return;
    static bool s_done = false; static DWORD s_last = 0;
    DWORD now = GetTickCount();
    if (s_done && now - s_last < 30000) return;          // refresh occasionally; cheap and idempotent
    s_last = now;
    char val[96];
    _snprintf_s(val, sizeof val, _TRUNCATE, "1:%llu", (unsigned long long)g_myid);   // protocol ver : our id
    bool ok = p_setrp(g_friends, RP_KEY, val);
    if (!s_done) logline("[rp] published %s=\"%s\" -> %s", RP_KEY, val, ok ? "ok" : "REJECTED (key/value limit?)");
    s_done = true;
}
// Read friends' beacons. Anyone advertising the key is running this mod - that is the exact discriminator.
static void rp_discover() {
    if (g_peerid || !p_getfrp || !g_friends || !p_fcount || !p_fbyidx || !p_fgame) return;
    int n = p_fcount(g_friends, K_FRIEND_IMMEDIATE);
    if (n <= 0 || n > 4096) return;
    unsigned long long cand[8]; int nc = 0;
    for (int i = 0; i < n && nc < 8; i++) {
        unsigned long long fid = p_fbyidx(g_friends, i, K_FRIEND_IMMEDIATE);
        if (!fid || fid == g_myid) continue;
        const char* v = p_getfrp(g_friends, fid, RP_KEY);
        char buf[128]; buf[0] = 0;
        if (v) { strncpy_s(buf, sizeof buf, v, _TRUNCATE); }   // points into Steam's buffer - copy NOW
        if (!buf[0]) continue;                                  // not running the mod (or presence hidden)
        logline("[rp] friend %llu advertises %s=\"%s\"", (unsigned long long)fid, RP_KEY, buf);
        cand[nc++] = fid;
    }
    static int s_lastn = -1;
    if (nc == 1) { logline("[rp] one partner advertising the mod -> pairing"); adopt_peer(cand[0]); }
    else if (nc > 1 && nc != s_lastn) logline("[rp] %d friends are running the mod - ambiguous, set coop-peer.txt", nc);
    else if (nc == 0 && s_lastn != 0) logline("[rp] no friends advertising the mod yet");
    s_lastn = nc;
}
static void friends_discover() {
    rp_publish();
    rp_discover();
    if (g_peerid) return;          // paired via rich presence - the fallback below is not needed
    if (g_peerid || !g_friends || !p_fcount || !p_fbyidx || !p_fgame) return;
    int n = p_fcount(g_friends, K_FRIEND_IMMEDIATE);
    if (n <= 0 || n > 4096) return;
    unsigned long long cand[8]; int nc = 0;
    for (int i = 0; i < n && nc < 8; i++) {
        unsigned long long fid = p_fbyidx(g_friends, i, K_FRIEND_IMMEDIATE);
        if (!fid || fid == g_myid) continue;
        BYTE fgi[64]; memset(fgi, 0, sizeof fgi);
        if (!p_fgame(g_friends, fid, fgi)) continue;        // not in any game
        // CGameID is a PACKED UNION: the AppID is bits 0-23, not the whole low 32. Comparing the low 32 bits
        // happens to work for a plain app launch but fails for shortcut/mod game ids.
        unsigned long long gid = 0; memcpy(&gid, fgi, 8);
        if ((unsigned)(gid & 0xFFFFFF) != SW_APPID) continue;   // not in Stormworks
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
    // The REAL connected-player roster, found by reading the native behind Lua's server.getPlayers (0x59EA50)
    // - the lead came from a community contributor on issue #8. The owning object is R8 at the 0x847EE0 hook
    // we ALREADY had; we were discarding that register. Ring of c_player*:
    //   game+0x410 blocks | +0x418 capacity | +0x41C head | +0x420 count ; entry = blocks[(head+i) % capacity]
    // c_player is 0x1A8 bytes with its SteamID64 at +0x140. No Lua is called - we read the same structure the
    // Lua function walks. (The previous app_state+0x190 guess was wrong: base=0, count=garbage in-game.)
    if (!g_cap_game || !p_send || !p_identClear || !p_identSetID) return;
    // ROLE MATTERS. game+0x698 = 2 hosting, 4 connected client, 0 idle.
    //  - HOSTING: the roster below is populated and carries each joiner's SteamID64.
    //  - CLIENT:  the roster is EMPTY (session::connect clears it) and players are tracked in a different
    //    0xC0-byte c_peer struct whose wire record has no SteamID64 at all - the game never tells a client
    //    another player's SteamID. But it does keep the SERVER's, at game+0x3E8, which is all we need:
    //    host reads the joiner from the roster, joiner reads the host from +0x3E8, and both sides pair.
    __try {
        char* game = (char*)g_cap_game;
        unsigned role = 0; safe_copy(&role, game+0x698, 4);
        if (role == 4) {                                   // we joined someone else's session
            uint64_t host = 0;
            if (safe_copy(&host, game+0x3E8, 8) && host >= 76561197960265728ULL && host != g_myid) {
                logline("[auto] we are a CLIENT - host SteamID64 %llu (game+0x3E8)", (unsigned long long)host);
                adopt_peer(host);
                if (g_peerid == host && p_identClear && p_identSetID) {
                    BYTE id[144]; p_identClear(id); p_identSetID(id, host);
                    HelloMsg h; memset(&h,0,sizeof h); h.magic=MAGIC; h.ver=1; h.kind=12; h.myid=g_myid;
                    p_send(g_net, id, &h, sizeof h, SEND_RELIABLE, CHANNEL);   // so they pair with us too
                }
            }
            return;                                        // the roster below is host-only; nothing to walk
        }
        void** base=nullptr; unsigned cap=0, head=0, count=0;
        if (!safe_copy(&base, game+0x410, 8)) return;
        safe_copy(&cap, game+0x418, 4); safe_copy(&head, game+0x41C, 4); safe_copy(&count, game+0x420, 4);
        static DWORD s_lw=0; DWORD nowt=GetTickCount();
        bool logit = (nowt-s_lw>10000); if (logit) s_lw=nowt;
        if (!base || !cap || cap>4096 || !count || count>512) {   // guard the modulo: capacity is 0 when idle
            if (logit) logline("[auto] roster: no session players yet (base=%p cap=%u head=%u count=%u)", (void*)base,cap,head,count);
            return;
        }
        for (unsigned i=0;i<count;i++) {
            void* pl=nullptr; if (!safe_copy(&pl,&base[(head+i)%cap],8) || !pl) continue;
            uint64_t sid=0; if (!safe_copy(&sid,(char*)pl+0x140,8)) continue;   // c_player+0x140 = SteamID64
            if (sid < 76561197960265728ULL) continue;         // not a plausible SteamID64
            if (sid == g_myid) { if (logit) logline("[auto] roster player[%u] = us", i); continue; }
            // PRIVACY: never record a stranger's SteamID64. On a public server the roster is full of people
            // who have nothing to do with this mod - we do not log their ids, adopt them, write them to
            // coop-peer.txt, or message them. Only a Steam FRIEND is ever named or paired with.
            bool isfriend = (g_friends && p_hasfriend) ? p_hasfriend(g_friends, sid, K_FRIEND_IMMEDIATE) : false;
            if (!isfriend) { if (logit) logline("[auto] roster player[%u] = (not a friend - ignored)", i); continue; }
            // BEACON CROSS-CHECK. Being a friend in the same session is NOT enough to be your co-op partner -
            // in the 2026-07-30 test the friends path correctly refused to guess between two friends, and then
            // this loop paired with player[1] regardless. With three people in a session that pairs with an
            // arbitrary one. Rich presence is now PROVEN to propagate (§22.1), and only a machine running the
            // mod publishes `swcoop`, so require the beacon before adopting. A friend who has not injected is
            // simply not a candidate.
            if (p_getfrp && g_friends) {
                const char* rv = p_getfrp(g_friends, sid, RP_KEY);
                if (!rv || !rv[0]) {
                    if (logit) logline("[auto] roster player[%u] = %llu (friend, but not running the mod - ignored)",
                                       i, (unsigned long long)sid);
                    continue;
                }
            }
            if (logit) logline("[auto] roster player[%u] = %llu (friend, advertising the mod)", i, (unsigned long long)sid);
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
// ---- MOVE TOOL (kind 15): the craft's build-area offset -----------------------------------------------
// poff (vehicle+0x2D8, f32x3) is the whole-craft move offset and it is CONTINUOUS - not voxel-snapped
// (measured: 8.76 / 8.596 / 8.076 voxels from fine movements). voxel0 (vehicle+0x1F0, f64x3) = benchCentre
// + poff. We transmit poff only, never voxel0: bench centres differ between machines and even between
// benches, so the receiver rebases using its OWN centre by applying the DELTA to its own voxel0. That keeps
// the craft in the same place relative to each player's bench without either side needing the other's
// world coordinates.
static float g_poff_last[3] = {0,0,0};
static bool  g_poff_have = false;
static long  g_poff_dirty = 0;
static DWORD g_poff_lastsend = 0, g_poff_lastmove = 0;

static void emit_move(const float p[3]) {
    if (!p_send || !g_net || !g_peerid || g_localecho) return;
    if (!g_sync_enabled || peer_away()) return;
    PlaceMsg m; memset(&m,0,sizeof m); m.magic=MAGIC; m.ver=1; m.kind=15;
    memcpy(&m.rot[0], p, 12);            // reuse the rotation field as the payload - no wire-format change
    p_send(g_net, g_peerIdent, &m, sizeof m, SEND_RELIABLE, CHANNEL);
    logline(">>> SEND move poff=(%.3f,%.3f,%.3f)", p[0],p[1],p[2]);
}
// MAIN THREAD ONLY (called from the apply queue). Applies the partner's offset by delta so our own bench
// centre is preserved: voxel0_new = voxel0_now + (poff_them - poff_now).
static void apply_move(const float want[3]) {
    if (!g_armed || !g_editor) return;
    __try {
        unsigned long long V=0;
        if (!safe_copy(&V,(void*)(g_editor+0x13C8),8) || !V) return;
        float now[3]={0,0,0}; double o[3]={0,0,0};
        if (!safe_copy(now,(void*)(V+0x2D8),12) || !safe_copy(o,(void*)(V+0x1F0),24)) return;
        if (now[0]==want[0] && now[1]==want[1] && now[2]==want[2]) return;    // already there - no echo
        double no[3] = { o[0] + (double)(want[0]-now[0]),
                         o[1] + (double)(want[1]-now[1]),
                         o[2] + (double)(want[2]-now[2]) };
        suppress_push();
        safe_copy((void*)(V+0x2D8), want, 12);
        safe_copy((void*)(V+0x1F0), no,   24);
        suppress_pop();
        // Keep the detector baseline in step, or the next frame sees "moved" and bounces it straight back.
        g_poff_last[0]=want[0]; g_poff_last[1]=want[1]; g_poff_last[2]=want[2]; g_poff_have=true;
        g_poff_dirty=0;
        logline("<<< APPLY move poff=(%.3f,%.3f,%.3f) (was %.3f,%.3f,%.3f)", want[0],want[1],want[2], now[0],now[1],now[2]);
    } __except(EXCEPTION_EXECUTE_HANDLER){ suppress_pop(); logline("<<< move apply EXC"); }
}
static void emit_presence(long in_bench) {
    if (!p_send || !g_net || !g_peerid || g_localecho) return;
    int sz[3]; bench_size_vox(sz);
    PlaceMsg m; memset(&m,0,sizeof m); m.magic=MAGIC; m.ver=1; m.kind=14; m.aux=(int32_t)in_bench;
    m.x=sz[0]; m.y=sz[1]; m.z=sz[2];         // carry our bench volume so the peer can compare benches
    { double c[3]; bench_centre(c);          // ...and its CENTRE, so "same bench" means the same physical bench
      float cf[3]={(float)c[0],(float)c[1],(float)c[2]}; memcpy(&m.rot[0], cf, 12); }
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
// The partner's bench CENTRE (editor+0xD38), carried in the presence beacon. Size alone does not identify a
// bench: two players at different bases with the same bench type pass a size check while standing kilometres
// apart, and every voxel they exchange is then anchored to a different point in the world. Zero means the
// partner is on a build that predates this field - skip the identity check rather than false-alarm them.
static double g_peer_bench_ctr[3] = {0,0,0};
static void bench_centre(double out[3]) {
    out[0]=out[1]=out[2]=0;
    if (!g_editor) return;
    safe_copy(out, (void*)(g_editor+0xD38), 24);
}
static bool bench_mismatch() {
    if (g_localecho) return false;
    if (!g_peer_bench[0]) return false;                 // partner volume not known yet
    int mine[3]; bench_size_vox(mine);
    if (!mine[0]) return false;                         // our volume not known yet
    if (mine[0]!=g_peer_bench[0] || mine[1]!=g_peer_bench[1] || mine[2]!=g_peer_bench[2]) return true;
    // Same SIZE is not the same BENCH. Compare centres too, with a metre of slack for the f32 round-trip.
    if (g_peer_bench_ctr[0] || g_peer_bench_ctr[1] || g_peer_bench_ctr[2]) {
        double c[3]; bench_centre(c);
        if (c[0] || c[1] || c[2]) {
            double dx=c[0]-g_peer_bench_ctr[0], dy=c[1]-g_peer_bench_ctr[1], dz=c[2]-g_peer_bench_ctr[2];
            if (dx*dx+dy*dy+dz*dz > 1.0) return true;
        }
    }
    return false;
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
    // Log the craft's build-area offset at SAVE time. Moving the whole craft is UNSNAPPED (confirmed by the
    // player), so a craft can sit at an arbitrary fractional offset while its voxels stay integers - which
    // makes this a cross-machine hazard: two machines with different poff render identical voxel data
    // metres apart. Printing it on every save turns "move it around and keep saving" into a usable
    // experiment without needing to leave and re-enter the bench for a FINGERPRINT line.
    { float p[3]={0,0,0}; double o[3]={0,0,0};
      unsigned long long V=0; safe_copy(&V,(void*)(g_editor+0x13C8),8);
      if (V) { safe_copy(p,(void*)(V+0x2D8),12); safe_copy(o,(void*)(V+0x1F0),24); }
      logline("[snap] serialized %u bytes | poff=(%.3f,%.3f,%.3f) voxel0=(%.2f,%.2f,%.2f)",
              len, p[0],p[1],p[2], o[0],o[1],o[2]); }
    char p[MAX_PATH]; DWORD n=GetModuleFileNameA(g_hmod,p,MAX_PATH);
    if(n && n<MAX_PATH){ char* s=strrchr(p,'\\'); if(s) *(s+1)=0; strncat_s(p,MAX_PATH,"coopworkbench-snapshot.xml",_TRUNCATE);
        FILE* f=nullptr; if(!fopen_s(&f,p,"wb")&&f){ fwrite(data,1,(size_t)len,f); fclose(f); logline("[snap] wrote %u bytes -> coopworkbench-snapshot.xml", len); } }
    ((void(*)(void*))(g_base+0x9B15A0))(data);   // free the serializer's buffer
}
// Load a whole-craft blob into the live editor vehicle + render it THIS FRAME. MUST run on the MAIN thread.
// blob = the game's save format (from snapshot_serialize OR a peer pull). Does NOT free blob (caller owns it).
// g_suppress gates our detect hooks so the rebuilt components don't re-broadcast (flood).
static void snapshot_load_from_buffer(char* blob, unsigned sz) {
    InterlockedExchange(&g_ps_dirty, 1);   // any load invalidates the property-sync baseline
    if(!g_armed||!g_editor){ logline("[snap] load: not armed"); return; }
    if(!blob||!sz||sz>0x8000000u){ logline("[snap] load: bad buffer"); return; }
    // The handler below pops the suppression counter, but the __try opens BEFORE the push - so a fault in the
    // raw reads just below (editor+0x13C8 / +0x70) would decrement THE CALLER's count instead. That matters
    // now that a property apply wraps a whole load in its own suppression: it would be cancelled mid-apply,
    // and the loader rebuilding every component would be detected as local edits and broadcast.
    bool pushed = false;
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
        // ==== STRUCT DIFF: find where the warning markers actually live ====
        // The markers survive a game restart plus a working body teardown, so they are not orphaned bodies.
        // Rather than guess at another list, snapshot the editor and vehicle before the load and diff after:
        // a marker list has to keep a COUNT somewhere, and a count that grows by the same amount on every
        // load is unmistakable. Logs any dword that changed and looks like a small count, plus pointers that
        // appeared from nothing.
        // The markers are the game's OWN "component has an unconnected logic input" warnings - correct
        // warnings, replicated on every load without the previous set being cleared. So we are looking for a
        // LIST that grows by the same amount each load, and the count should match the number of duplicated
        // icons on screen. Scan wider than before: the editor beyond its first 64 KB, and the game object,
        // which is where a validation/UI list would plausibly live. (The earlier narrow sweep found nothing
        // growing in editor+0..0x10000 or vehicle+0..0x4000.)
        // NOTE the vehicle window is 0x408, not 0x8000: that is the WHOLE object (allocated
        // `mov ecx,0x408; call 0x957478` at 0x7C93C8, freed `mov edx,0x408; call 0x95706C` at 0x7C9459).
        // The old 0x8000 window read 28 KB of unrelated neighbouring heap, which is where the phantom
        // "vehicle+0x7134/+0x7164/+0x7674/+0x7678 move on every load" result came from - discard it.
        static unsigned char snap_ed[0x40000], snap_ve[0x408], snap_go[0x40000], snap_as[0x20000];
        unsigned long long gobj_p = 0; safe_copy(&gobj_p, (void*)(editor + 0x70), 8);
        bool have_ed = safe_copy(snap_ed, (void*)editor, sizeof snap_ed);
        bool have_ve = safe_copy(snap_ve, vehicle,       sizeof snap_ve);
        bool have_go = gobj_p && safe_copy(snap_go, (void*)gobj_p, sizeof snap_go);
        // The app-state has never been scanned, and it is the one structure that plainly outlives a craft.
        bool have_as = g_cap_appstate && safe_copy(snap_as, (void*)g_cap_appstate, sizeof snap_as);
        // PER-BODY DEQUE COUNTS BEFORE THE LOAD. A body carries TWO deques: components at +0x3F0
        // (count +0x400, settled in §32.8) and a second one at +0x408 (cap +0x410, head +0x414, count
        // +0x418) which 0x499D30 walks calling a virtual [vtable+0x2D0] on each child - the shape of a
        // render-node / scene-attachment list. If that count grows across loads while the component count
        // stays flat, it is the marker leak and it names itself.
        unsigned pre_comp = 0, pre_node = 0, pre_bodies = 0, pre_slots = 0, pre_elec = 0;
        __try {
            char* V = (char*)vehicle;
            unsigned nb=*(unsigned*)(V+0x10), bc=*(unsigned*)(V+0x08), bh=*(unsigned*)(V+0x0C);
            void** bb=*(void***)(V+0x00);
            pre_bodies = nb;
            if (bb && bc) for (unsigned i=0;i<nb && i<64;i++) {
                char* b=(char*)bb[(bh+i)%bc]; if(!b) continue;
                unsigned c=0,n2=0; safe_copy(&c,b+0x400,4); safe_copy(&n2,b+0x418,4);
                pre_comp += c; pre_node += n2;
                // PER-COMPONENT SLOT COUNTS. Nothing global grows, and these warnings are per-component
                // ("incomplete logic/fluid connection"), so the duplication should be INSIDE the components.
                // The codec audit predicted exactly this: comp+0x100 is a slot ring that GAINS AN ENTRY PER
                // DECODE and is never cleared - the clear helper 0x2EFCC0 skips comp+0xFC..0x117, which is
                // where that ring lives. A component with one unconnected input would then report two after
                // a second load. comp+0xC8 = logic slot count, comp+0x110 = electric slot count.
                unsigned cc=*(unsigned*)(b+0x400), ch2=*(unsigned*)(b+0x3FC), ccap=*(unsigned*)(b+0x3F8);
                void** cb=*(void***)(b+0x3F0);
                if (cb && ccap) for (unsigned k=0;k<cc && k<2048;k++) {
                    char* comp=(char*)cb[(ch2+k)%ccap]; if(!comp) continue;
                    unsigned sc=0, ec=0;
                    safe_copy(&sc, comp+0xC8, 4); safe_copy(&ec, comp+0x110, 4);
                    if (sc < 4096) pre_slots += sc;
                    if (ec < 4096) pre_elec  += ec;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        // ==== CLEAR THE WARNING ICONS - the third omitted step (§33) ====
        // The yellow "incomplete logic/power/fluid connection" triangles are 2D UI WIDGETS, not craft
        // geometry and not anything the mod draws. Component virtual [vtable+0x248] (0x326970) builds one per
        // warned component and APPENDS it to the child ring of the c_ui_element embedded at editor+0x3F8
        // (data +0x428, cap +0x430, head +0x434, count +0x438), caching it in comp+0x88:
        //     0x326B69  call 0x957478            ; operator new(0xA0)
        //     0x326B84  mov  [rsp+0xE0], 0xFF00EBFF   ; the yellow
        //     0x326BBF  lea  rdx, [rip+0x837BCA]      ; .rdata 0xB5E790 = "warning" (atlas_gameicons2)
        //     0x326C17  mov  [rdi+0x88], rax          ; cache on the component
        //     0x326C4C  mov  [r15+0x40], ecx          ; ring count++      (r15 = arg3 = editor+0x3F8,
        //     0x326C61  mov  [rax+rdx*8], rsi         ; store the child     bound at 0x32699C mov r15,r8)
        // The ONLY dedup guard is that per-component pointer:
        //     0x326B57  cmp qword ptr [rdi+0x88], rbp ; already have an icon?
        //     0x326B5E  jne 0x326C7B                  ; yes -> just reposition it, do not append
        // so a FRESH set of components always appends a FRESH set of icons. Nothing bulk-clears that ring.
        // The one and only remover is 0x326920(component, editor+0x3F8) - 4 xrefs in the whole image, all in
        // the editor command handler - called once per component as that component dies:
        //     0x326926  mov  r8, [rcx+0x88]      ; the icon
        //     0x326936  je   0x32695B            ; no icon -> no-op, so it is safe on every component
        //     0x32693F  call 0x1DD7E0            ; remove_child(parent, parent+0x50, icon): unlink, clear the
        //                                        ;   parent's hover/focus slots, run the child's dtor, compact
        //     0x326946  [rbx+0x88] = [rbx+0x90] = [rbx+0x98] = 0
        // The GAME calls it for every component of every body IMMEDIATELY BEFORE its load - the nested
        // iterator at 0x7FCCD4..0x7FCD4A (`lea rcx,[rax+0x3F0]` bodies->components, `lea rdx,[r14+0x3F8]`,
        // `call 0x326920` at 0x7FCD2A), twelve instructions before `call 0x4BDDD0` at 0x7FCDB7. We destroy
        // every component en masse and never make that call, so the previous craft's icons stay parented to
        // editor+0x3F8 forever with a dangling comp+0x88 backlink, and the next load appends another full set.
        // That is the whole bug, and it explains every observed symptom: load #1 clean / #2 doubled (the first
        // load has no previous set to orphan); survives leaving the bench and survives a DIFFERENT craft
        // (the ring belongs to the EDITOR, which is built at world setup by 0x7C91F0 and emptied only at world
        // teardown by 0x7CF750 -> 0x1E1C20 -> 0x1DD350); workbench-only (only the editor draws that layer,
        // gated on editor+0xE71); and scattered stale positions (0x326970 rewrites icon+0x08/+0x0C from the
        // camera every frame for LIVE components only, so an orphan freezes at the screen coords it held on
        // the frame its component died).
        //
        // ORDER: this MUST run BEFORE 0x4BE0A0. Teardown destructs and frees every body (0x4BAD00 + free size
        // 0x14F8) and the component destructor chain (0x34FEB0 -> 0x33C290 -> 0x33C330) never touches
        // comp+0x88, so after teardown the icons are permanently unreachable.
        // AND WE MUST NOT DO IT OURSELVES: zeroing editor+0x438, or draining the ring with bare 0x1DD7E0
        // calls, leaves every LIVE warned component pointing at a freed 0xA0 block - and 0x326970's
        // already-have-icon path at 0x326C7F re-reads comp+0x88 EVERY FRAME and writes through it
        // (movss [rax+8], movss [rax+0xC], call [vtable+0xF0]). That is a per-frame use-after-free, not a
        // leak. Calling the game's own 0x326920 is what makes this safe: it nulls the backlink in lockstep.
        //
        // This is PREVENTION ONLY. Orphans already in the ring from earlier loads in this process have dead
        // components, so nothing can tell them apart from live icons by inspecting the ring - measure from a
        // fresh game launch.
        {
            typedef void (*destroywarn_t)(void*, void*);
            destroywarn_t destroy_warn = (destroywarn_t)(g_base + 0x326920);
            void* ui_parent = (void*)(editor + 0x3F8);            // exactly what 0x7FCD20 passes
            unsigned ring_before = 0, ring_after = 0;
            unsigned swept = 0, had_icon = 0, nbody = 0, clamped = 0;
            safe_copy(&ring_before, (void*)(editor + 0x438), 4);
            __try {
                char* V = (char*)vehicle;
                void** bb=nullptr; unsigned bc=0, bh=0, nb=0;
                if (safe_copy(&bb,V+0x00,8) && safe_copy(&bc,V+0x08,4) &&
                    safe_copy(&bh,V+0x0C,4) && safe_copy(&nb,V+0x10,4) && bb && bc) {
                    if (nb > 4096) { nb = 4096; clamped = 1; }
                    for (unsigned i=0;i<nb;i++) {
                        char* b=nullptr;
                        if (!safe_copy(&b,&bb[(bh+i)%bc],8) || !b) continue;
                        nbody++;
                        // body+0x3F0 component deque - the SAME list the game hands to 0x326920 at 0x7FCCE6,
                        // and the same one 0x4BDE45..0x4BDE9F walks to write comp+0x28 = body.
                        void** cb=nullptr; unsigned ccap=0, ch=0, cc=0;
                        if (!safe_copy(&cb,b+0x3F0,8) || !safe_copy(&ccap,b+0x3F8,4) ||
                            !safe_copy(&ch,b+0x3FC,4) || !safe_copy(&cc,b+0x400,4)) continue;
                        if (!cb || !ccap) continue;
                        if (cc > 65536) { cc = 65536; clamped = 1; }
                        for (unsigned k=0;k<cc;k++) {
                            void* comp=nullptr;
                            if (!safe_copy(&comp,&cb[(ch+k)%ccap],8) || !comp) continue;
                            void* icon=nullptr;                                   // diagnostic only
                            if (safe_copy(&icon,(char*)comp+0x88,8) && icon) had_icon++;
                            destroy_warn(comp, ui_parent);                        // no-op when comp+0x88 == 0
                            swept++;
                        }
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logline("[warn] icon sweep EXC 0x%lX after %u component(s) - continuing (the load still works, "
                        "markers persist)", GetExceptionCode(), swept);
            }
            safe_copy(&ring_after, (void*)(editor + 0x438), 4);
            // The ring must drop by EXACTLY had_icon. It holds the editor's permanent UI children too, so the
            // absolute value is not meaningful - the delta is. A mismatch means the container is wrong and the
            // fix must not be trusted.
            logline("[warn] icon sweep: %u component(s) over %u body(s), %u carried an icon | "
                    "editor+0x438 ring %u -> %u (-%u)%s%s",
                    swept, nbody, had_icon, ring_before, ring_after, ring_before - ring_after,
                    clamped ? "  <-- CLAMPED: craft bigger than the sweep bound, some icons left behind" : "",
                    (ring_before - ring_after) != had_icon
                        ? "  <-- ring did not drop by the icon count: CONTAINER IS WRONG, do not trust this" : "");
        }
        // ==== TEAR DOWN THE EXISTING BODIES FIRST - the step we have never done ====
        // The game's own load wrapper (0x4BDDD0) calls 0x4BE0A0 BEFORE 0x4C6160, and that function destroys
        // and frees every existing body:
        //     0x4BE0D0 loop: per body -> call 0x499D30            (detach)
        //     0x4BE100 loop: per body -> call 0x4BAD00 (destruct) then 0x95706C(body, 0x14F8) (free;
        //                                                          0x14F8 is the body allocation size)
        // 0x4C6160 does NOT clear - it only adds. So every load we have ever done left the previous craft's
        // bodies resident, with their components still registered: orphans that render as out-of-bounds
        // warning markers at stale world positions and ACCUMULATE with each load. That is the stuck-marker
        // bug, and it is also a straight memory leak that has been in the F7 pull since it shipped.
        // It looked fixed by the back-pointer work (§24) only because a single-body craft leaks one body per
        // load; a 9-body craft leaks nine and the markers are unmissable.
        {
            unsigned nb_before = 0; safe_copy(&nb_before, (char*)vehicle + 0x10, 4);
            // ARG2 is editor+0x08, NOT ctx. All three callers of the game's load wrapper 0x4BDDD0 set up
            // identically - rcx=[editor+0x13C8] (vehicle), rdx=[editor+0x08], r8=gobj+0x64C8 (ctx),
            // r9=gobj+0xBB670 (registry) - and the wrapper forwards rcx/rdx UNCHANGED to 0x4BE0A0. The value
            // is handed down to a virtual [vtable+0x2D0] on every child of body+0x408 (0x499D30), so passing
            // ctx there access-violated. Verified call sites: 0x7C94A4, 0x7D023B, 0x7D0751.
            void* teardown_ctx = nullptr;
            safe_copy(&teardown_ctx, (void*)(editor + 0x08), 8);
            typedef void (*clearbodies_t)(void*, void*);
            __try {
                if (!teardown_ctx) { logline("[snap] editor+0x08 is NULL - skipping body teardown"); }
                else ((clearbodies_t)(g_base + 0x4BE0A0))(vehicle, teardown_ctx);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                logline("[snap] body teardown 0x4BE0A0 EXC 0x%lX - continuing (the load still works, markers persist)",
                        GetExceptionCode());
            }
            unsigned nb_after = 0; safe_copy(&nb_after, (char*)vehicle + 0x10, 4);
            logline("[snap] body teardown: %u bodies -> %u (0x4BE0A0, the pre-load step the game does at 0x4BDE00)",
                    nb_before, nb_after);
        }
        // DROP THE PROPERTIES-PANEL TARGET FIRST. The panel caches the selected component at editor+0x1550
        // (and the microcontroller sub-editor at editor+0x9BF0), and its widgets hold RAW pointers to that
        // component. The load frees every component and rebuilds, so anything still pointing at the old one
        // is a use-after-free the moment the player touches a slider. Nothing in the mod cleared these
        // before a load - a latent crash on every F7/F4 taken with a panel open.
        // editor+0x1420 is a THIRD cached pointer of the same kind: a Body* taken from comp+0x28
        // (0x7F68E2) and later dereferenced as an argument at 0x7F6A5D / 0x7F6A83. The game clears it after
        // its OWN load (0x8082AF, inside 0x808220, called at 0x7FCDCF straight after 0x4BDDD0) - we never
        // did, and every load of ours frees all bodies via 0x4BE0A0. This fixes F7 and F4 too, not just
        // property sync: click a component (which sets it), load, then click another, and the old path ran
        // on a freed body.
        { void* nul = nullptr;
          safe_copy((void*)(editor+0x1550), &nul, 8);
          safe_copy((void*)(editor+0x9BF0), &nul, 8);
          safe_copy((void*)(editor+0x1420), &nul, 8); }
        // ==== POSITION: capture OUR bench anchor BEFORE the load ====
        // vehicle+0x1F0 (f64 x3) = world position of voxel (0,0,0) - the REAL render anchor (RE-confirmed,
        // and read back correct in-game). vehicle+0x2D8 (f32 x3) = placement offset, which the blob carries
        // from the sender. NOTE: the previous attempt here deltad body+0x2B8/+0x2F8; those read 0.000 in-game
        // at every bench, so that code could only ever compute a zero delta - it was a no-op. Removed.
        double recv_org[3];  bool have_org  = safe_copy(recv_org,  (char*)vehicle+0x1F0, 24);
        float  recv_poff[3]; bool have_poff = safe_copy(recv_poff, (char*)vehicle+0x2D8, 12);
        if (have_org) logline("[snap] our anchor: voxel0=(%.2f,%.2f,%.2f)", recv_org[0],recv_org[1],recv_org[2]);
        suppress_push(); pushed = true;
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
          // ================= THE DISPLACED-CRAFT FIX (SS39) =================
          // The restore that used to be here was the bug. Craft position is NOT one field:
          //    vehicle+0x2D8  poff  - f32x3, the move-tool offset; the blob DOES carry it
          //    editor+0xCB8   f64x3 - the MASTER copy (translation row of the 4x4 at editor+0xC58)
          //    vehicle+0x1F0  f64x3 - the rendered anchor, produced FROM the other two
          // The game never loads +0x1F0 from a blob. It RECONSTRUCTS it, and this exact block appears
          // verbatim at four sites (e.g. bench entry 0x7D0472-0x7D04FB):
          //      if (vehicle+0x29C == 0) { editor+0xCB8 = (double)poff;
          //                                vehicle+0x1F0 += editor+0xCB8;
          //                                0x7D16C0(editor, vehicle); }   // clamp to bench + rebuild
          // We ran none of it, AND we overwrote +0x1F0 with the receiver's own pre-load anchor - which
          // already had the RECEIVER's offset baked in. So the craft rendered at benchCentre + poff_receiver
          // while carrying poff_sender: displaced by exactly (poff_r - poff_s) metres, in an arbitrary
          // direction, and identically ZERO solo because one machine only ever has one poff. That is the
          // entire reported symptom. It is the same shape of bug as SS24 (back-pointers) and SS33
          // (teardown): a step the game does inside one function that our hand-assembled sequence skipped.
          // A stale editor+0xCB8 also makes the game's own bounds test (0x807F50) evaluate against the wrong
          // offset, which is a plausible fourth source of spurious out-of-bounds warning markers.
          {
            unsigned long long ed = g_editor;
            unsigned char applied_flag = 1;
            safe_copy(&applied_flag, (void*)((char*)vehicle+0x29C), 1);
            // 0x7D0B40 is the game's post-load fixup: it resets the cursor cells, copies the build frame
            // editor+0xCD8 -> vehicle+0x190, ZEROES the editor+0xC58 placement matrix, and caches the craft
            // origin into editor+0x320. Run it FIRST - the zeroing is what makes step 2 well-defined.
            __try { ((void(*)(void*))(g_base+0x7D0B40))((void*)ed); }
            __except(EXCEPTION_EXECUTE_HANDLER) { logline("[snap] 0x7D0B40 post-load fixup EXC - continuing"); }

            // Anchor on OUR bench centre (editor+0xD38, f64x3), not on whatever we had before the load.
            double centre[3] = {0,0,0};
            if (safe_copy(centre, (void*)(ed+0xD38), 24))
                safe_copy((char*)vehicle+0x1F0, centre, 24);

            // Then apply the SENDER's poff exactly as the game does - gated on vehicle+0x29C the same way,
            // because that flag is carried in the blob and gates the game's own apply. Mirroring the game's
            // condition is safer than guessing what the flag means.
            if (applied_flag == 0) {
                float pf[3] = {0,0,0};
                if (safe_copy(pf, (char*)vehicle+0x2D8, 12)) {
                    double d[3] = { (double)pf[0], (double)pf[1], (double)pf[2] };
                    safe_copy((void*)(ed+0xCB8), d, 24);           // the master copy the game reads
                    double a2[3] = {0,0,0};
                    if (safe_copy(a2, (char*)vehicle+0x1F0, 24)) {
                        a2[0]+=d[0]; a2[1]+=d[1]; a2[2]+=d[2];
                        safe_copy((char*)vehicle+0x1F0, a2, 24);
                    }
                }
                // 0x7D16C0 clamps the anchor into OUR build volume (it derives the half-extent as
                // (size-3)*0.5*0.25 m, independently re-deriving SS15.6's "last legal voxel") and rebuilds
                // the 4x4 at editor+0xC58. This is also what stops a partner's offset pushing the craft
                // out of bounds on a smaller bench.
                __try { ((void(*)(void*,void*))(g_base+0x7D16C0))((void*)ed, vehicle); }
                __except(EXCEPTION_EXECUTE_HANDLER) { logline("[snap] 0x7D16C0 clamp EXC - position may be wrong"); }
            } else {
                logline("[snap] vehicle+0x29C=%u (non-zero) - the game would not apply poff here either, skipping",
                        applied_flag);
            }
            double fin[3]={0,0,0}; float fp[3]={0,0,0};
            safe_copy(fin,(char*)vehicle+0x1F0,24); safe_copy(fp,(char*)vehicle+0x2D8,12);
            logline("[xform] centre=(%.3f,%.3f,%.3f) poff=(%.3f,%.3f,%.3f) -> anchor=(%.3f,%.3f,%.3f) flag=%u",
                    centre[0],centre[1],centre[2], fp[0],fp[1],fp[2], fin[0],fin[1],fin[2], applied_flag);
          }
          // poff (vehicle+0x2D8) is NOT drift and must NOT be normalised. It is the "move the craft within
          // the build area" offset - a user-controlled value the game saves into the blob and restores on
          // load. Proven: the "<-- CHANGED BY LOAD" marker fired for the first time when a craft was moved
          // before saving, with the load setting poff to the value it had at SAVE time (0,-2.000,0) over a
          // live (0,-2.000,3.092). Two earlier attempts here were both wrong: restoring the pre-load value
          // discards the craft's own offset, and zeroing it discards the offset AND fights the player's
          // explicit placement. Leave the loaded value alone - the game already did the right thing.
          // SNAP poff TO THE VOXEL GRID. Measured: moving a craft to all 8 corners of a bench produces poff
          // values that are ALWAYS exact multiples of the 0.25 voxel scale (2.250=9, -2.750=-11, 4.250=17,
          // -6.750=-27 ...). The move tool snaps. So any fractional residue is not a player placement - it
          // is an artefact, and it only ever appeared after loading a craft saved at a DIFFERENT-sized
          // bench: (0,-1.750,-1.828) and (0,-2.000,3.092), where -1.828/0.25 = -7.312 and 3.092/0.25 =
          // 12.368. Bench centres are not voxel-aligned to each other (y 13.96 vs 0.55, differing by 13.41),
          // so a cross-bench load leaves that difference behind in poff and the craft sits OFF the voxel
          // grid - which is what trips the out-of-bounds warning markers and reads as a displaced craft.
          // Rounding to the nearest voxel keeps the player's own placement exactly (it is already a
          // multiple) while discarding only the cross-bench residue.
          // poff is left ALONE. Three theories have now died here: restoring the pre-load value (discards the
          // craft's own offset), zeroing it (discards it AND overrides an explicit player action), and
          // snapping it to the 0.25 voxel grid. The snap was inferred from a corner sweep whose eight
          // samples were all exact voxel multiples - but those were taken by JAMMING the craft into the
          // bench corners, which clamps it against a grid-aligned boundary. The experiment could not
          // distinguish "the tool snaps" from "the boundary is aligned", and the player reports it does not
          // snap. So a fractional poff is a legitimate placement and must be preserved.
          { float now[3]={0,0,0};
            if (safe_copy(now,(char*)vehicle+0x2D8,12) &&
                (now[0]!=recv_poff[0]||now[1]!=recv_poff[1]||now[2]!=recv_poff[2]))
                logline("[snap] poff (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f) from the blob - kept",
                        recv_poff[0],recv_poff[1],recv_poff[2], now[0],now[1],now[2]);
          }
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
                // ==== BACK-POINTER FIXUP - the step we were skipping ====
                // The game's OWN load (0x4BDDD0) does this between 0x4C98C0 and 0x4C3410, and we jumped
                // straight from one to the other, leaving every loaded body and render node without its
                // parent back-pointer:
                //     0x4BDE57  mov [rcx+0x250], rdi   ; body->vehicle = vehicle
                //     0x4BDE8C  mov [rdx+0x28],  rcx   ; node->body    = body   (for every node in the body)
                // body+0x250 is the same parent-vehicle field body_ok() validates before a forge, so after a
                // pull our own safety check would see every body as untrustworthy - and the game's validation
                // has equally little to go on, which is the likely source of the stuck out-of-bounds warning
                // markers (reproducible SOLO with F5 then F4).
                __try {
                    unsigned nb2=*(unsigned*)(V+0x10), bc2=*(unsigned*)(V+0x08), bh2=*(unsigned*)(V+0x0C);
                    void** bb2=*(void***)(V+0x00);
                    unsigned fixed_b=0, fixed_n=0;
                    if (bb2 && bc2) for (unsigned i=0;i<nb2;i++) {
                        char* body=(char*)bb2[(bh2+i)%bc2]; if(!body) continue;
                        *(void**)(body+0x250) = (void*)V; fixed_b++;          // body->vehicle
                        unsigned nn2=*(unsigned*)(body+0x400), nc2=*(unsigned*)(body+0x3F8), nh2=*(unsigned*)(body+0x3FC);
                        void** nb3=*(void***)(body+0x3F0);
                        if (!nb3 || !nc2) continue;
                        for (unsigned j=0;j<nn2;j++) {
                            char* node=(char*)nb3[(nh2+j)%nc2]; if(!node) continue;
                            *(void**)(node+0x28) = (void*)body; fixed_n++;    // node->body
                        }
                    }
                    logline("[snap] back-pointers fixed: %u bodies, %u components (0x4BDE57/0x4BDE8C)",
                            fixed_b, fixed_n);
                    // DELETED: the vehicle+0x1480 "conflict list" probe. It read 4 KB PAST THE END of the
                    // object. The vehicle is 0x408 bytes - allocated `mov ecx,0x408; call 0x957478` at
                    // 0x7C93C8, freed `mov edx,0x408; call 0x95706C` at 0x7C9459 - so +0x1480 was always a
                    // neighbouring heap block and every value it ever logged was noise. The conflict list is
                    // real but it is a BODY field: 0x4BA5D9 `lea rcx,[r15+0x1480]` with r15 = the body being
                    // inserted into, which fits because a Body is 0x14F8 bytes (0x4BE11E). It also could not
                    // have been the stuck markers - bodies are freed by 0x4BE0A0 and rebuilt every load, so
                    // nothing stored there survives a craft swap. The markers are UI widgets in the editor's
                    // own ring at editor+0x3F8; see the sweep above.
                } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[snap] back-pointer fixup EXC"); }
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
                    // ==== POSITION PROBE (diagnostic) ====
                    // A pulled craft renders in the WRONG SPOT in the bench, and vehicle+0x1F0 is provably not
                    // the culprit (the load never modifies it - the "CHANGED BY LOAD" marker has never fired).
                    // The save format carries no world coordinates either (it is pure voxel: <vp x= y= z=>), so
                    // whatever is displacing the craft is a world transform the LOAD recomputed, not something
                    // the blob transported. Rather than guess at another offset, find it: walk body[0] for f64
                    // triples that look like a world position and log where they are. On the receiving machine
                    // any triple holding the SENDER's bench coordinates instead of ours is the offending field.
                    // Compare the two machines' logs for the same pull and the answer is read straight off.
                    if (n && bodies[0]) {
                        char* b0=(char*)bodies[0]; int hits=0;
                        logline("[posprobe] body[0]=%p  scanning for world-position triples", b0);
                        for (unsigned o=0; o<0x600 && hits<12; o+=8) {
                            double v[3];
                            if (!safe_copy(v, b0+o, 24)) continue;
                            bool plausible = (v[0]>-1e6 && v[0]<1e6) && (v[2]>-1e6 && v[2]<1e6) &&
                                             (v[1]>-1e4 && v[1]<1e4) &&
                                             ((v[0]<-100.0||v[0]>100.0) || (v[2]<-100.0||v[2]>100.0));
                            if (!plausible) continue;
                            logline("[posprobe]   body+0x%03X = (%.2f, %.2f, %.2f)", o, v[0], v[1], v[2]);
                            hits++;
                        }
                        if (!hits) logline("[posprobe]   no world-position triple in body[0]+0..0x600 - transform is elsewhere (vehicle level?)");
                    }
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
        suppress_pop();
        logline("[snap] load+build done - craft should render now");
        // The component count is the number the player can actually check against their screen.
        { char t[96]; _snprintf_s(t, sizeof t, _TRUNCATE, "CRAFT RELOADED  %u parts  %u KB", pc_expect_components(), (sz + 512) / 1024);
          wsdraw_toast(t); }
        // ...and after. Components should match the craft; anything in the +0x408 deque that grows load
        // over load is leaked scene attachment, which is what the markers would be.
        __try {
            char* V = (char*)vehicle;
            unsigned nb=*(unsigned*)(V+0x10), bc=*(unsigned*)(V+0x08), bh=*(unsigned*)(V+0x0C);
            void** bb=*(void***)(V+0x00);
            unsigned post_comp=0, post_node=0, post_slots=0, post_elec=0;
            if (bb && bc) for (unsigned i=0;i<nb && i<64;i++) {
                char* b=(char*)bb[(bh+i)%bc]; if(!b) continue;
                unsigned c=0,n2=0; safe_copy(&c,b+0x400,4); safe_copy(&n2,b+0x418,4);
                post_comp += c; post_node += n2;
                unsigned cc=*(unsigned*)(b+0x400), ch2=*(unsigned*)(b+0x3FC), ccap=*(unsigned*)(b+0x3F8);
                void** cb=*(void***)(b+0x3F0);
                if (cb && ccap) for (unsigned k=0;k<cc && k<2048;k++) {
                    char* comp=(char*)cb[(ch2+k)%ccap]; if(!comp) continue;
                    unsigned sc=0, ec=0;
                    safe_copy(&sc, comp+0xC8, 4); safe_copy(&ec, comp+0x110, 4);
                    if (sc < 4096) post_slots += sc;
                    if (ec < 4096) post_elec  += ec;
                }
            }
            logline("[snap] deques: bodies %u->%u | components %u->%u | body+0x408 %u->%u",
                    pre_bodies, nb, pre_comp, post_comp, pre_node, post_node);
            logline("[snap] SLOTS: comp+0xC8 total %u->%u (%+d) | comp+0x110 electric %u->%u (%+d) %s",
                    pre_slots, post_slots, (int)post_slots-(int)pre_slots,
                    pre_elec, post_elec, (int)post_elec-(int)pre_elec,
                    (post_slots > pre_slots || post_elec > pre_elec)
                        ? "<-- SLOTS GREW while components did not: this is the duplicated warning source" : "");
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        // Diff the structures we snapshotted before the load.
        {
            struct Rgn { const char* nm; void* base; unsigned char* was; unsigned len; bool ok; };
            Rgn rgn[3] = { { "editor",  (void*)editor,   snap_ed, (unsigned)sizeof snap_ed, have_ed },
                           { "vehicle", vehicle,         snap_ve, (unsigned)sizeof snap_ve, have_ve },
                           { "gobj",    (void*)gobj_p,   snap_go, (unsigned)sizeof snap_go, have_go } };
            // FOLLOW THE POINTERS. The warnings survive leaving the bench AND loading a DIFFERENT craft,
            // so they are not per-component state - a new craft has entirely different components. And they
            // only render inside the workbench, so the editor owns them. That means a container the editor
            // POINTS AT: a heap deque whose count lives in the heap block, not in the editor struct, which
            // is why scanning the struct's own bytes found nothing. Snapshot the first 0x20 bytes behind
            // every pointer-shaped field and diff those too.
            {
                static unsigned long long ptrs[24000]; static unsigned char pre[24000][0x40];
                static unsigned nptr = 0; static bool armed = false;
                unsigned char now2[0x40];
                if (!armed) {
                    nptr = 0;
                    for (unsigned o = 0; o + 8 <= 0x40000 && nptr < 24000; o += 8) {
                        unsigned long long v = 0;
                        if (!safe_copy(&v, (void*)(editor + o), 8)) continue;
                        if (v < 0x10000ULL || v > 0x7FFFFFFFFFFFULL) continue;
                        // Only follow things that LOOK like a container: first field a pointer, and a small count
                        // somewhere in the header. That filters the field down enormously and targets exactly
                        // the shape we are hunting - a deque whose count lives in the heap block.
                        unsigned long long inner = 0;
                        if (!safe_copy(&inner, (void*)v, 8)) continue;
                        if (inner && (inner < 0x10000ULL || inner > 0x7FFFFFFFFFFFULL)) continue;
                        if (!safe_copy(pre[nptr], (void*)v, 0x40)) continue;
                        ptrs[nptr++] = v;
                    }
                    armed = true;
                    logline("[snap]   [ptr] armed on %u pointers behind the editor", nptr);
                } else {
                    int hits = 0;
                    for (unsigned i = 0; i < nptr && hits < 20; i++) {
                        if (!safe_copy(now2, (void*)ptrs[i], 0x40)) continue;
                        for (unsigned o = 0; o + 4 <= 0x40; o += 4) {
                            unsigned a2, b2;
                            memcpy(&a2, pre[i] + o, 4); memcpy(&b2, now2 + o, 4);
                            if (a2 == b2 || b2 <= a2) continue;
                            if (a2 > 100000u || b2 > 100000u) continue;
                            if (b2 - a2 < 2) continue;   // +1 is churn; a duplicated warning set jumps by dozens
                            logline("[snap]   [ptr] %p +0x%X: %u -> %u  (+%u)", (void*)ptrs[i], o, a2, b2, b2 - a2);
                            hits++;
                            break;
                        }
                    }
                    if (!hits) logline("[snap]   [ptr] nothing grew behind %u editor pointers", nptr);
                    for (unsigned i = 0; i < nptr; i++) safe_copy(pre[i], (void*)ptrs[i], 0x40);
                }
            }
            for (int r = 0; r < 3; r++) {
                if (!rgn[r].ok) continue;
                static unsigned char now[0x40000];
                if (!safe_copy(now, rgn[r].base, rgn[r].len)) continue;
                int shown = 0;
                for (unsigned o = 0; o + 4 <= rgn[r].len && shown < 20; o += 4) {
                    unsigned a, b;
                    memcpy(&a, rgn[r].was + o, 4); memcpy(&b, now + o, 4);
                    if (a == b) continue;
                    // A marker list keeps a count. Report small integers that grew, and nothing else -
                    // pointers and floats churn constantly and would bury the signal.
                    bool counted = (a < 100000u && b < 100000u && b > a);
                    if (!counted) continue;
                    logline("[snap]   [diff] %s+0x%X: %u -> %u  (+%u)", rgn[r].nm, o, a, b, b - a);
                    shown++;
                }
                if (!shown) logline("[snap]   [diff] %s: no growing counters in the first 0x%X bytes",
                                    rgn[r].nm, rgn[r].len);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){ if (pushed) suppress_pop();
        logline("[snap] LOAD EXC 0x%lX", GetExceptionCode()); }
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

// ======================= PER-COMPONENT PROPERTY CODEC (vtable+0x260) =======================
// Replaces the old dormant block, which called 0x4AE3F0 (the per-BODY serializer) with a COMPONENT as arg1
// and crashed the game twice. Its archive/stream/ctx construction was byte-for-byte CORRECT; only the callee
// and a fabricated node argument were wrong. Full RE in FINDINGS 27.
//
//   void __fastcall codec(void* this /*rcx*/, Ctx* /*rdx*/, Archive* /*r8*/, Node* /*r9*/)
//
// Four register args, NO stack args, NO usable return (no path in 0x326F60 or 0x359E20 defines eax).
// Direction and format are FIELDS OF THE ARCHIVE, not arguments. Real call sites: 0x4B951C (read),
// 0x4B975B (write).
//
// WIRE SHAPE, binary mode, WRITE clone at 0x328873 - what our blob looks like:
//     [12 bytes] voxel_position  comp+0x18, three int32        (0x32887D -> 0x8FF800 -> 0x2913F0)
//     [u16 + N]  rotation        comp+0x30, as a DECIMAL STRING (0x328AF7 -> 0x187D40)
//     [u16 + N]  bc / bc2 / bc3 / ac   hex colour strings
//     [u16 + N]  sc              surface colours, comma-joined
//     [4 bytes]  damage          gated on ctx.mask bit 2
// Field NAMES are not emitted in binary mode (0x8FF800's binary arm tail-jumps at 0x8FFA39 ignoring r9) and
// the skip-if-default comparisons are DOM-only, so the binary stream is FIXED-SHAPE and untagged - which is
// the only reason a sequential decode could ever be safe.
//
// ONLY THE WRITE DIRECTION IS IMPLEMENTED. Write is unreserved-safe: the write clone performs ZERO stores
// into this, and every primitive is bounds-checked BEFORE the direction branch, so the worst case is an
// empty blob. READ is a different animal - non-idempotent (comp+0x100 gains an entry per decode, forever),
// unrollbackable, with a silent stream-desync mode - and is deliberately NOT built yet.
//
// HARD RULES, each one paid for in analysis:
//   * NEVER call 0x9B15A0 on our buffer. That free belongs to the game-allocated whole-craft blob; it does
//     and rcx,~7 / mov rcx,[rcx-8] / free, and would free the 8 bytes BEFORE our array.
//   * NEVER hand this archive to 0x1F4AD0 / 0x1F4950 / 0x90C430 - they treat archive+0x00 / +0x10 as owned
//     heap and would delete our stack Stream.
//   * Archive and stream MUST live in the same stack frame (they point at each other) and die together.
//   * MAIN THREAD ONLY. Nothing here takes a game lock, but it reads the live editor.
static const unsigned PC_CAP = 0x10000;   // 64 KB scratch

struct PcCtx     { void* registry; void* gamectx; unsigned mask; unsigned _p; };
struct PcStream  { void* owner; unsigned char* cur; unsigned remaining; unsigned char dir; unsigned char _p[3]; };
struct PcArchive { void* buf; unsigned cap; unsigned _p0; PcStream* stream; void* xmldoc;
                   unsigned format; unsigned char dir; unsigned char _p1[3]; };
static_assert(sizeof(PcCtx)==0x18,     "PcCtx must be 0x18");
static_assert(sizeof(PcStream)==0x18,  "PcStream must be 0x18");
static_assert(sizeof(PcArchive)==0x28, "PcArchive must be 0x28");

typedef void (*pcodec_t)(void*, void*, void*, void*);   // vtable+0x260
extern "C" { extern volatile long g_cur_valid, g_cur_vx, g_cur_vy, g_cur_vz; }   // hover sampler, defined below

// A vptr or code pointer must land inside the game image or we refuse to dispatch through it.
static unsigned long long g_img_size = 0;
static bool in_game_image(const void* p) {
    if (!g_base || !p) return false;
    if (!g_img_size) {
        __try {
            IMAGE_DOS_HEADER*   d = (IMAGE_DOS_HEADER*)g_base;
            IMAGE_NT_HEADERS64* n = (IMAGE_NT_HEADERS64*)((char*)g_base + d->e_lfanew);
            g_img_size = n->OptionalHeader.SizeOfImage;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    unsigned long long a = (unsigned long long)p;
    return a >= g_base && a < g_base + g_img_size;
}

struct PcIdent {
    void*         vt;        // comp+0x00
    void*         codec;     // vt[0x260/8]
    void*         t3C0;      // vt[0x3C0/8]  read-mode teardown (garbage on a short vtable - only logged)
    void*         t3F8;      // vt[0x3F8/8]
    void*         def;       // comp+0x58    part definition
    char          name[96];  // def+0x288 (ptr) / def+0x290 (u32 len)
    unsigned      typeidx;   // def+0x2AC    factory switch value. NOTE: on the DEFINITION, not the component
    unsigned char cat;       // def+0x40     sub-shape byte
    unsigned      dataver;   // comp+0x10    dialect selector; ctor sets 3 at 0x4B97DF
    unsigned      nslots;    // comp+0xC8  slot count ONLY on classes that have slots; reads as
                             //            -3 / 7077993 on plain blocks, so it is not a general field
    unsigned      nsurf;     // comp+0x78    surface-colour count
    int           vox[3];    // comp+0x18
};
static bool pc_ident(void* comp, PcIdent* id) {
    memset(id, 0, sizeof *id);
    if (!comp) return false;
    unsigned long long c = (unsigned long long)comp;
    if (!safe_copy(&id->vt, (void*)c, 8) || !in_game_image(id->vt)) return false;
    if (!safe_copy(&id->codec, (char*)id->vt + 0x260, 8) || !in_game_image(id->codec)) return false;
    // DO NOT read vt+0x3C0 / vt+0x3F8. Three classes (component_base 0xB03C40, _basic 0xAFBF98,
    // _block 0xB5A6E0) have 88-slot vtables ending at +0x2C0, so those offsets are an OUT-OF-BOUNDS read
    // into .rdata string data that returns a plausible-looking pointer - for block, ImageBase+0x1DDA10.
    // in_game_image() cannot tell that from a real function. Never classify by probing those slots.
    // We no longer decode, so nothing needs them; left zeroed.
    id->t3C0 = nullptr; id->t3F8 = nullptr;
    safe_copy(&id->dataver, (void*)(c + 0x10),  4);
    safe_copy(&id->nslots,  (void*)(c + 0xC8),  4);
    safe_copy(&id->nsurf,   (void*)(c + 0x78),  4);
    safe_copy(id->vox,      (void*)(c + 0x18), 12);
    if (!safe_copy(&id->def, (void*)(c + 0x58), 8) || !id->def) return false;
    const char* np = nullptr; unsigned nl = 0;
    safe_copy(&np, (char*)id->def + 0x288, 8);
    safe_copy(&nl, (char*)id->def + 0x290, 4);
    if (np && nl && nl < sizeof id->name && safe_copy(id->name, np, nl)) id->name[nl] = 0;
    safe_copy(&id->typeidx, (char*)id->def + 0x2AC, 4);
    safe_copy(&id->cat,     (char*)id->def + 0x40,  1);
    return true;
}

// 64 KB + PAGE_NOACCESS guard page. A write cannot overrun (every primitive checks stream+0x10 before the
// direction branch), so the guard turns an impossible overrun into a catchable AV rather than a heap smash.
static unsigned char* pc_scratch() {
    static unsigned char* s = nullptr;
    if (!s) {
        s = (unsigned char*)VirtualAlloc(nullptr, PC_CAP + 0x1000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
        if (s) { DWORD o; VirtualProtect(s + PC_CAP, 0x1000, PAGE_NOACCESS, &o); }
    }
    return s;
}

// Serialize ONE component through its OWN vptr. READ-ONLY with respect to the component.
// Nothing is allocated and nothing is freed - the caller owns out.
static bool pc_serialize(void* comp, unsigned char* out, unsigned outcap, unsigned* outlen, PcIdent* idout) {
    *outlen = 0;
    if (!g_editor || !comp || !out || !outcap) return false;
    PcIdent id;
    if (!pc_ident(comp, &id)) { logline("[pc] serialize: unreadable component/vptr (%p)", comp); return false; }
    if (idout) *idout = id;
    char* gobj = nullptr;
    if (!safe_copy(&gobj, (void*)(g_editor + 0x70), 8) || !gobj) { logline("[pc] serialize: no gobj"); return false; }
    unsigned char* buf = pc_scratch();
    if (!buf) { logline("[pc] serialize: scratch alloc failed"); return false; }
    memset(buf, 0, PC_CAP);

    PcCtx ctx; PcStream st; PcArchive ar;
    memset(&ctx, 0, sizeof ctx); memset(&st, 0, sizeof st); memset(&ar, 0, sizeof ar);
    ctx.registry = gobj + 0xBB670;   // == arg3 of 0x4C5FE0 (0x4C5FFB)
    ctx.gamectx  = gobj + 0x64C8;    // == arg2 of 0x4C5FE0 (0x4C5FFF)
    ctx.mask     = 5;                // the field-group mask the proven whole-craft save uses (0x4C6003)
    ar.buf = buf; ar.cap = PC_CAP; ar.stream = &st;
    ar.xmldoc = nullptr;             // legal ONLY because format == 1
    ar.format = 1;                   // 1 = binary
    ar.dir    = 1;                   // 1 = WRITE
    st.owner = &ar; st.cur = buf; st.remaining = PC_CAP;
    st.dir   = 1;                    // must equal ar.dir (0x4C60B6 copies archive+0x24 into stream+0x14)

    unsigned written = 0;
    __try {
        ((pcodec_t)id.codec)(comp, &ctx, &ar, nullptr);   // node is NULL in binary; never dereferenced
        written = PC_CAP - st.remaining;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        logline("[pc] serialize EXC 0x%lX  comp=%p '%s' vt=%p codec=+0x%llX",
                GetExceptionCode(), comp, id.name, id.vt, (unsigned long long)id.codec - g_base);
        return false;
    }
    unsigned advanced = (unsigned)(st.cur - buf);
    if (advanced != written)
        logline("[pc] serialize WARN cursor/remaining disagree (adv=%u rem-delta=%u)", advanced, written);
    if (!written) { logline("[pc] serialize produced 0 BYTES ('%s' ver=%u) - wrong class or wrong mask",
                            id.name, id.dataver); return false; }
    if (st.remaining == 0) {   // 0x187E4F skips the LENGTH PREFIX but still writes the body when full
        logline("[pc] serialize TRUNCATED (filled all %u bytes, '%s') - DISCARDING, not a partial success",
                PC_CAP, id.name); return false; }
    if (written > outcap) { logline("[pc] serialize %u > caller cap %u - discarding", written, outcap); return false; }
    if (!safe_copy(out, buf, written)) return false;
    *outlen = written;
    return true;
}

// -------- SOLO PROBE: hover a component, press F11. Zero risk - never enters the read direction. --------
static void pc_hex(const unsigned char* b, unsigned n, char* o, size_t cap) {
    o[0] = 0; char t[4];
    for (unsigned i = 0; i < n; i++) { _snprintf_s(t, sizeof t, _TRUNCATE, "%02X", b[i]);
        strncat_s(o, cap, t, _TRUNCATE); if (i + 1 < n) strncat_s(o, cap, " ", _TRUNCATE); }
}
static void pc_probe_write() {
    if (!g_armed || !g_editor) { logline("[pc] not armed - open the workbench first"); return; }
    int vx = g_cur_vx, vy = g_cur_vy, vz = g_cur_vz;
    void* comp = g_cur_valid ? lookup_component(g_editor, vx, vy, vz) : nullptr;
    if (!comp) { for (int x=-16;x<=16&&!comp;x++) for (int y=-8;y<=8&&!comp;y++) for (int z=-16;z<=16&&!comp;z++)
                     { comp = lookup_component(g_editor, x,y,z); if (comp) { vx=x; vy=y; vz=z; } } }
    if (!comp) { logline("[pc] no component under the cursor and none near the origin"); return; }

    static unsigned char blob[PC_CAP]; unsigned len = 0; PcIdent id;
    logline("[pc] === WRITE PROBE (%d,%d,%d) ===", vx, vy, vz);
    if (!pc_serialize(comp, blob, sizeof blob, &len, &id)) { logline("[pc] VERDICT: FAIL - no blob"); return; }

    logline("[pc] '%s' typeidx=%u cat=%u ver=%u slots=%u surf=%u len=%u",
            id.name, id.typeidx, id.cat, id.dataver, id.nslots, id.nsurf, len);
    logline("[pc] vt=+0x%llX  codec=+0x%llX",
            (unsigned long long)id.vt - g_base, (unsigned long long)id.codec - g_base);
    { char h[128]; pc_hex(blob, len < 24 ? len : 24, h, sizeof h); logline("[pc] head: %s", h); }

    // SELF-PARSE: decides PASS/FAIL with no second machine and no second tool.
    if (len < 14) { logline("[pc] VERDICT: FAIL - %u bytes is too short for vp + a length prefix", len); return; }
    int bx, by, bz; unsigned short rl;
    memcpy(&bx, blob + 0, 4); memcpy(&by, blob + 4, 4); memcpy(&bz, blob + 8, 4); memcpy(&rl, blob + 12, 2);
    bool voxok = (bx == id.vox[0] && by == id.vox[1] && bz == id.vox[2]);
    bool rlok  = (rl >= 1 && rl <= 64 && 14u + rl <= len);
    bool asciiok = rlok;
    if (rlok) for (unsigned i = 0; i < rl; i++) { char ch = (char)blob[14 + i];
        if (!((ch >= '0' && ch <= '9') || ch == ',' || ch == '-' || ch == ' ' || ch == '.')) { asciiok = false; break; } }
    char rot[80] = {0}; if (rlok && rl < sizeof rot) memcpy(rot, blob + 14, rl);
    logline("[pc] parse: vp=(%d,%d,%d) live=(%d,%d,%d) %s | rotlen=%u \"%s\" %s",
            bx, by, bz, id.vox[0], id.vox[1], id.vox[2], voxok ? "MATCH" : "*** MISMATCH ***",
            rl, rot, asciiok ? "ASCII" : "*** NOT ASCII ***");
    if (voxok && rlok && asciiok)
        logline("[pc] VERDICT: PASS - write dialect is as decoded (vp raw first, rotation as a string).%s",
                id.dataver == 3 ? "" : "  *** BUT ver != 3 - this class is banned from the read direction. ***");
    else
        logline("[pc] VERDICT: FAIL - the write dialect is NOT what the analysis predicted. STOP; "
                "re-read 0x328873 before building anything on this.");
    logline("[pc] === end (component NOT modified) ===");
}

// -------- CENSUS: probe EVERY component in the craft, one keypress. --------
// The single-component probe depends on the hover sampler resolving, and when it does not it silently falls
// back to "first component near the origin" - which is how five consecutive presses aimed at a
// microcontroller all reported the same 01_block. Aiming is the wrong interface for this. Walk the whole
// build volume instead, de-duplicate by component pointer (multi-voxel parts occupy many cells), and
// serialize each one exactly once. Still read-only: the write direction performs no stores into the
// component, so this cannot modify the craft no matter how many parts it touches.
// -------- RECORD WALKER: address a component inside the craft blob, exactly. --------
// The splice gate proved blobs are embedded VERBATIM and byte-stable, but also that a byte SEARCH cannot
// address them: 67 identical blocks produce 67 identical blobs, so a search always returns the first.
// Addressing comes from the record structure instead:
//     <u32 count> then, per component:  <u16 nlen><def name><u8 def+0x40 cat><codec blob>
// and from the fact that a blob's FIRST 12 BYTES are its voxel position (0x45F63D movups [r14] -> comp+0x18),
// which is unique within a body. So: walk the records, read the voxel out of each record's blob, and match.
//
// The list ANCHOR is found empirically rather than by assuming a header size: build the record header for
// every live component, find each one's earliest occurrence, and take the minimum. That is the first record.
// Read-only. Nothing here writes to the craft or the blob.

struct PcRec { unsigned off; unsigned hdr; unsigned bloblen; int vox[3]; char name[64]; unsigned char cat; };

// Build <u16 nlen><name><u8 cat> for a component, into out; returns header length.
static unsigned pc_rec_header(const PcIdent* id, unsigned char* out, unsigned cap) {
    unsigned nl = (unsigned)strlen(id->name);
    if (nl + 3 > cap) return 0;
    out[0] = (unsigned char)(nl & 0xFF); out[1] = (unsigned char)(nl >> 8);
    memcpy(out + 2, id->name, nl);
    out[2 + nl] = id->cat;
    return nl + 3;
}

// Walk the component records. Returns how many were parsed; fills recs[] up to maxrecs.
// expect_n is the live component count (body+0x400), used to bound the walk.
static unsigned pc_walk_records(const unsigned char* craft, unsigned clen, unsigned anchor,
                                unsigned expect_n, PcRec* recs, unsigned maxrecs) {
    unsigned cur = anchor, n = 0;
    while (n < expect_n && n < maxrecs && cur + 3 <= clen) {
        unsigned nl = (unsigned)craft[cur] | ((unsigned)craft[cur + 1] << 8);
        if (nl == 0 || nl > 60 || cur + 2 + nl + 1 > clen) break;      // not a plausible record
        PcRec* r = &recs[n];
        r->off = cur;
        memcpy(r->name, craft + cur + 2, nl); r->name[nl] = 0;
        r->cat = craft[cur + 2 + nl];
        r->hdr = 2 + nl + 1;
        // The blob begins immediately after the header and opens with the voxel triple.
        unsigned bstart = cur + r->hdr;
        if (bstart + 12 > clen) break;
        memcpy(r->vox, craft + bstart, 12);
        // Length is not stored: find where the NEXT record starts by locating the next plausible header
        // whose name matches a definition we know exists. Simpler and safer: the caller supplies the blob
        // length from a live serialize of the matching component, so we only need the header here.
        r->bloblen = 0;
        n++;
        // Advance past this record using the caller-resolved length, filled in by pc_index_craft below.
        break;   // single-step; pc_index_craft drives the walk so it can resolve each length
    }
    return n;
}

// Build a full index of the craft blob. NO layout assumptions beyond the record header.
//
// The first attempt read the voxel out of blob[0..11] and looked the component up by position. That works
// for most classes but NOT all: a microprocessor's derived codec emits its own fields before chaining to the
// base, so its blob opens with <u16 28>"ZE Modular Engine Controller", and the walk read that as a voxel of
// (1163526172, ...) and stopped. Record lengths are not stored anywhere either - the game does not need
// them, because on read it constructs a component and lets the codec consume exactly what it wrote.
//
// So: cache every LIVE component's blob up front, then at each record match on name+cat and find the cached
// blob whose bytes equal the craft at that offset. That yields both the identity and the length, assumes
// nothing about field order, and is self-verifying - a record we cannot match is reported rather than
// guessed past.
// vox is the identity that SURVIVES A LOAD. Matching by component pointer was wrong: every load rebuilds
// the objects, so a baseline taken before a load matches nothing after it - which reported 429 changed and
// 404 new on a craft where one rotor had been edited. Voxel position comes from the live component
// (comp+0x18), not from the blob, so it is reliable for every class including the microprocessor.
struct PcLive { void* comp; char name[64]; unsigned char cat; unsigned off; unsigned len; int vox[3]; };
static PcLive  g_live[4096];
static unsigned g_nlive = 0;
static unsigned char g_arena[0x600000];   // 6 MB: a real craft measured 598 KB serialized
static unsigned g_arena_used = 0;

static void pc_cache_one(void* comp) {
    if (!comp || g_nlive >= 4096) return;
    static unsigned char blob[PC_CAP];
    unsigned len = 0; PcIdent id;
    if (!pc_serialize(comp, blob, sizeof blob, &len, &id)) return;
    if (g_arena_used + len > sizeof g_arena) return;
    PcLive* L = &g_live[g_nlive++];
    L->comp = comp; L->cat = id.cat; L->off = g_arena_used; L->len = len;
    L->vox[0] = id.vox[0]; L->vox[1] = id.vox[1]; L->vox[2] = id.vox[2];
    strncpy_s(L->name, sizeof L->name, id.name, _TRUNCATE);
    memcpy(g_arena + g_arena_used, blob, len);
    g_arena_used += len;
}
// Enumerate through the BODY deque, not the bench volume. A voxel scan only ever reached the main body, and
// a real craft has 9 - so sub-body components were never cached and their records could never be matched.
// body+0x3F0 is the COMPONENT deque (count +0x400, capacity +0x3F8, head +0x3FC), settled live in §32.8.
// How many components the craft has, across EVERY body. This was read from body[0] alone
// (`bb[bh%bc]+0x400`) while pc_cache_live walks all of them - and it is the loop bound for the record walk,
// so on any craft with a second body (a hinge is enough) the index stopped short, `bad` stayed 0, the
// completeness check passed vacuously, and every update to a sub-body component logged "no 'X' here".
static unsigned pc_expect_components() {
    unsigned total = 0;
    __try {
        unsigned long long V = 0;
        if (!safe_copy(&V, (void*)(g_editor + 0x13C8), 8) || !V) return 0;
        unsigned nb = *(unsigned*)(V + 0x10), bc = *(unsigned*)(V + 0x08), bh = *(unsigned*)(V + 0x0C);
        void** bb = *(void***)(V + 0x00);
        if (!bb || !bc) return 0;
        for (unsigned b = 0; b < nb && b < 64; b++) {
            char* body = (char*)bb[(bh + b) % bc];
            if (body) total += *(unsigned*)(body + 0x400);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return total; }
    return total;
}

static unsigned pc_cache_live() {
    g_nlive = 0; g_arena_used = 0;
    __try {
        unsigned long long V = 0;
        if (!safe_copy(&V, (void*)(g_editor + 0x13C8), 8) || !V) return 0;
        unsigned nb = *(unsigned*)(V + 0x10), bc = *(unsigned*)(V + 0x08), bh = *(unsigned*)(V + 0x0C);
        void** bb = *(void***)(V + 0x00);
        if (!bb || !bc) return 0;
        for (unsigned b = 0; b < nb && b < 64; b++) {
            char* body = (char*)bb[(bh + b) % bc];
            if (!body) continue;
            unsigned cn = *(unsigned*)(body + 0x400), cc = *(unsigned*)(body + 0x3F8), ch = *(unsigned*)(body + 0x3FC);
            void** cb = *(void***)(body + 0x3F0);
            if (!cb || !cc) continue;
            for (unsigned i = 0; i < cn && g_nlive < 4096; i++) pc_cache_one(cb[(ch + i) % cc]);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return g_nlive;
}

static unsigned pc_index_craft(const unsigned char* craft, unsigned clen, unsigned anchor,
                               unsigned expect_n, PcRec* recs, unsigned maxrecs, unsigned* bad) {
    unsigned cur = anchor, n = 0; *bad = 0;
    static bool used[4096];
    memset(used, 0, sizeof used);
    while (n < expect_n && n < maxrecs && cur + 3 <= clen) {
        unsigned nl = (unsigned)craft[cur] | ((unsigned)craft[cur + 1] << 8);
        if (nl == 0 || nl > 60 || cur + 2 + nl + 1 > clen) {
            (*bad)++; logline("[idx] STOP at %u: implausible name length %u at off=%u", n, nl, cur); break;
        }
        PcRec r; r.off = cur;
        memcpy(r.name, craft + cur + 2, nl); r.name[nl] = 0;
        r.cat = craft[cur + 2 + nl];
        r.hdr = 2 + nl + 1;
        unsigned bstart = cur + r.hdr;
        // Find the (unused) live component of this name+cat whose blob matches here.
        int hit = -1;
        for (unsigned i = 0; i < g_nlive; i++) {
            if (used[i] || g_live[i].cat != r.cat) continue;
            if (strcmp(g_live[i].name, r.name) != 0) continue;
            unsigned L = g_live[i].len;
            if (bstart + L > clen) continue;
            if (memcmp(craft + bstart, g_arena + g_live[i].off, L) != 0) continue;
            hit = (int)i; break;
        }
        if (hit < 0) {
            (*bad)++;
            logline("[idx] STOP at %u: '%s' cat=%u off=%u - no unused live component of that name/cat matches "
                    "these bytes", n, r.name, r.cat, r.off);
            break;
        }
        used[hit] = true;
        r.bloblen = g_live[hit].len;
        // The MATCHED live component's voxel, not the first 12 blob bytes. Those happened to look like a
        // position and were only ever logged; property sync matches a peer's update on this field, so it has
        // to be the real thing. Bench coordinates are bench-independent and centred (SS16), so the sender's
        // voxel and the receiver's agree.
        memcpy(r.vox, g_live[hit].vox, 12);
        if (n < maxrecs) recs[n] = r;
        n++;
        cur = bstart + r.bloblen;
    }
    return n;
}

// Find the first component record: the earliest offset at which any live component's record header + blob
// appears. Derived, not assumed, so a header-layout change cannot silently misalign us. Runs off the cached
// blob list (pc_cache_live) so it covers every body - the previous version did its own bench-volume scan and
// therefore only saw the main body.
static bool pc_find_anchor(const unsigned char* craft, unsigned clen, unsigned* out_anchor) {
    unsigned char hdr[80];
    unsigned best = 0xFFFFFFFFu;
    for (unsigned k = 0; k < g_nlive; k++) {
        PcLive* L = &g_live[k];
        unsigned nl = (unsigned)strlen(L->name);
        if (nl + 3 > sizeof hdr) continue;
        hdr[0] = (unsigned char)(nl & 0xFF); hdr[1] = (unsigned char)(nl >> 8);
        memcpy(hdr + 2, L->name, nl);
        hdr[2 + nl] = L->cat;
        unsigned hl = nl + 3;
        const unsigned char* blob = g_arena + L->off;
        for (unsigned i = 0; i + hl + L->len <= clen && i < best; i++) {
            if (memcmp(craft + i, hdr, hl) != 0) continue;
            if (memcmp(craft + i + hl, blob, L->len) != 0) continue;
            best = i;
            break;
        }
    }
    if (best == 0xFFFFFFFFu) return false;
    *out_anchor = best;
    return true;
}

// SOLO PROBE: prove we can address every component exactly. Read-only.
static void pc_probe_index() {
    if (!g_armed || !g_editor) { logline("[idx] not armed"); return; }
    unsigned clen = 0; char* craft = snapshot_serialize(&clen);
    if (!craft) { logline("[idx] serialize failed"); return; }
    unsigned expect_n = 0;
    __try {
        unsigned long long V = 0;
        if (safe_copy(&V, (void*)(g_editor + 0x13C8), 8) && V) {
            unsigned bc = *(unsigned*)(V + 0x08), bh = *(unsigned*)(V + 0x0C);
            void** bb = *(void***)(V + 0x00);
            if (bb && bc) safe_copy(&expect_n, (char*)bb[bh % bc] + 0x400, 4);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { expect_n = 0; }

    unsigned nlive = pc_cache_live();
    logline("[idx] cached %u live component blobs (%u bytes) across all bodies", nlive, g_arena_used);
    unsigned anchor = 0;
    bool got = pc_find_anchor((const unsigned char*)craft, clen, &anchor);
    logline("[idx] craft=%u bytes  components=%u  anchor=%s%u", clen, expect_n, got ? "" : "NOT FOUND ", anchor);
    if (!got) { ((void(*)(void*))(g_base + 0x9B15A0))(craft); logline("[idx] FAIL - no record anchor"); return; }

    static PcRec recs[4096]; unsigned bad = 0;
    unsigned n = pc_index_craft((const unsigned char*)craft, clen, anchor, expect_n, recs, 4096, &bad);
    logline("[idx] walked %u/%u records, %u bad", n, expect_n, bad);
    __try {
        unsigned long long V = 0;
        if (safe_copy(&V, (void*)(g_editor + 0x13C8), 8) && V) {
            unsigned nb = *(unsigned*)(V + 0x10), bc = *(unsigned*)(V + 0x08), bh = *(unsigned*)(V + 0x0C);
            void** bb = *(void***)(V + 0x00);
            logline("[idx] craft has %u bodies", nb);
            for (unsigned i = 0; i < nb && i < 6 && bb && bc; i++) {
                unsigned cnt = 0; safe_copy(&cnt, (char*)bb[(bh + i) % bc] + 0x400, 4);
                logline("[idx]   body[%u] components=%u", i, cnt);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    for (unsigned i = 0; i < n && i < 4; i++)
        logline("[idx]   [%u] %-26s cat=%u off=%-7u len=%u", i, recs[i].name, recs[i].cat,
                recs[i].off, recs[i].bloblen);
    if (n == expect_n && expect_n && !bad)
        logline("[idx] *** PASS - all %u records walked, every component addressable. Splice can target exactly. ***", n);
    else if (!bad && n < expect_n)
        logline("[idx] INCOMPLETE - walked %u of %u with ZERO bad records; ran out of index slots, not out of "
                "format. Raise the cap.", n, expect_n);
    else
        logline("[idx] *** FAIL - walked %u of %u, %u bad. Record format is not as modelled. ***", n, expect_n, bad);
    ((void(*)(void*))(g_base + 0x9B15A0))(craft);
}

// Report exactly WHERE two craft blobs diverge. A load round-trip is not byte-preserving - the craft comes
// back correct and the same size, but some bytes differ - so "byte-identical after load" was the wrong
// verification criterion. Knowing WHICH bytes move tells us what is volatile (unique ids? ordering?) and
// therefore what a real splice must actually check.
static void pc_report_diff(const unsigned char* a, const unsigned char* b, unsigned len,
                           const PcRec* recs, unsigned nrecs, const char* tag) {
    unsigned first = 0xFFFFFFFFu, ndiff = 0, runs = 0;
    bool inrun = false;
    for (unsigned i = 0; i < len; i++) {
        if (a[i] == b[i]) { inrun = false; continue; }
        if (first == 0xFFFFFFFFu) first = i;
        ndiff++;
        if (!inrun) { runs++; inrun = true; }
    }
    if (!ndiff) { logline("[diff] %s: identical", tag); return; }
    logline("[diff] %s: %u bytes differ in %u runs, first at %u of %u (%.3f%%)",
            tag, ndiff, runs, first, len, 100.0 * ndiff / (len ? len : 1));
    // Which record owns the first difference, and is it inside the blob or in the header?
    for (unsigned r = 0; r < nrecs; r++) {
        unsigned st = recs[r].off, en = recs[r].off + recs[r].hdr + recs[r].bloblen;
        if (first < st || first >= en) continue;
        unsigned bstart = st + recs[r].hdr;
        logline("[diff]   first diff is in record %u '%s' (off=%u len=%u), %s at +%u",
                r, recs[r].name, st, recs[r].bloblen,
                first < bstart ? "in the HEADER" : "inside the blob", first - (first < bstart ? st : bstart));
        break;
    }
    if (first < (nrecs ? recs[0].off : 0))
        logline("[diff]   first diff is BEFORE the first record (craft header, off=%u)", first);
    char ha[64], hb[64];
    unsigned n = (len - first) < 8 ? (len - first) : 8;
    pc_hex(a + first, n, ha, sizeof ha); pc_hex(b + first, n, hb, sizeof hb);
    logline("[diff]   expected: %s", ha);
    logline("[diff]   got:      %s", hb);
}

// ======================= THE SPLICE =======================
// Substitute ONE component's bytes inside a serialized craft and let the game's own loader rebuild. Every
// component is then constructed fresh, which is the only state the game ever decodes into (§32.1), so no
// write path ever points at a live component.
//
// Prerequisites, all proven solo before this was written:
//   §32.7  the craft blob is byte-stable across serialises, and every component's blob is embedded verbatim
//   §32.10 every record is addressable - 1689/1689 walked on a 9-body, 598 KB craft
//   §32.8  body+0x3F0 is the component deque, so enumeration covers every body
//
// SAFETY, in the order it matters:
//   1. ROLLBACK. 0x4B94A6-0x4B94BD shows the loader clears the vehicle BEFORE it can fail, so a rejected
//      splice leaves a partially rebuilt craft rather than the original. We keep the pre-splice bytes and
//      reload them on any failure. This is not a nicety - without it a bad splice destroys the craft.
//   2. EXACT-LENGTH GATE. The codec truncates silently (0x32BE7D cmp edx,4/jb). After a splice we
//      re-serialize and require the result to match what we spliced, byte for byte. A length that drifts
//      means the load did not reproduce what we sent.
//   3. SUPPRESS. A load fires the place/delete/connect detours; g_suppress is now a counter (§32) so a
//      nested apply cannot clear it early and re-broadcast our own edits.

// Replace the record at recs[i] with `newblob`, returning the spliced craft in out (caller-sized).
static bool pc_build_spliced(const unsigned char* craft, unsigned clen, const PcRec* rec,
                             const unsigned char* newblob, unsigned newlen,
                             unsigned char* out, unsigned outcap, unsigned* outlen) {
    unsigned bstart = rec->off + rec->hdr;
    unsigned tail   = bstart + rec->bloblen;
    if (tail > clen) return false;
    unsigned need = bstart + newlen + (clen - tail);
    if (need > outcap) return false;
    memcpy(out, craft, bstart);                                   // everything before the blob
    memcpy(out + bstart, newblob, newlen);                        // the substituted blob
    memcpy(out + bstart + newlen, craft + tail, clen - tail);     // everything after
    *outlen = need;
    return true;
}

// SOLO TEST: splice a component's OWN blob back over itself and reload. The craft must come back
// byte-identical. This exercises the entire pipeline - serialize, index, substitute, load, verify, roll back
// - while changing nothing, so a failure is a pipeline bug and never a lost craft.
// PROPERTY DIFF: learn where a property lives by watching the player change it.
// Binary blobs carry no field names, so blade_count cannot be located by inspection. But it can be learned:
// snapshot every component, let the player change one thing in-game, snapshot again, and the bytes that
// moved ARE that property. This is also precisely the change-detection half of live sync - "which component
// changed, and what are its new bytes" - so it is the mechanism, not just a probe.
// Press once to arm the baseline, press again after making a change.
// The most recent changed component's new bytes - the exact payload a peer would transmit.
static unsigned char g_stored[PC_CAP];
static unsigned      g_stored_len = 0;
static unsigned char g_stored_cat = 0;
static char          g_stored_name[64] = {0};

static unsigned char g_base_arena[0x600000];
static PcLive       g_base_live[4096];
static unsigned     g_base_n = 0, g_base_used = 0;
static bool         g_base_armed = false;

static void pc_probe_prop_diff() {
    if (!g_armed || !g_editor) { logline("[pdiff] not armed"); return; }
    unsigned n = pc_cache_live();
    if (!n) { logline("[pdiff] no components"); return; }

    if (!g_base_armed) {
        memcpy(g_base_live, g_live, sizeof(PcLive) * (n < 4096 ? n : 4096));
        memcpy(g_base_arena, g_arena, g_arena_used);
        g_base_n = n; g_base_used = g_arena_used; g_base_armed = true;
        logline("[pdiff] BASELINE captured: %u components, %u bytes.", n, g_arena_used);
        logline("[pdiff] Now change something in-game - rotor blade count, a name, a slider - then press again.");
        return;
    }

    logline("[pdiff] === COMPARING against the baseline (%u components) ===", g_base_n);
    unsigned changed = 0, appeared = 0;
    for (unsigned i = 0; i < n; i++) {
        // Match on voxel + name + cat, which survives a load. (Pointer matching does not: a reload rebuilds
        // every component, and a stale baseline then reports the whole craft as changed.)
        int j = -1;
        for (unsigned k = 0; k < g_base_n; k++) {
            if (g_base_live[k].vox[0] != g_live[i].vox[0]) continue;
            if (g_base_live[k].vox[1] != g_live[i].vox[1]) continue;
            if (g_base_live[k].vox[2] != g_live[i].vox[2]) continue;
            if (g_base_live[k].cat != g_live[i].cat) continue;
            if (strcmp(g_base_live[k].name, g_live[i].name) != 0) continue;
            j = (int)k; break;
        }
        if (j < 0) { appeared++; continue; }
        const unsigned char* a = g_base_arena + g_base_live[j].off;
        const unsigned char* b = g_arena + g_live[i].off;
        unsigned la = g_base_live[j].len, lb = g_live[i].len;
        if (la == lb && memcmp(a, b, la) == 0) continue;
        changed++;
        logline("[pdiff] CHANGED: %s at (%d,%d,%d) (len %u -> %u)", g_live[i].name,
                g_live[i].vox[0], g_live[i].vox[1], g_live[i].vox[2], la, lb);
        // Keep the NEW bytes. This is exactly what a peer would send, and what pc_apply_stored replays.
        if (lb <= sizeof g_stored) {
            memcpy(g_stored, b, lb); g_stored_len = lb;
            g_stored_cat = g_live[i].cat;
            strncpy_s(g_stored_name, sizeof g_stored_name, g_live[i].name, _TRUNCATE);
            logline("[pdiff]   stored %u bytes for '%s' - press numpad * to apply them back", lb, g_stored_name);
        }
        if (la == lb) {
            unsigned shown = 0;
            for (unsigned o = 0; o + 4 <= la && shown < 6; o++) {
                if (a[o] == b[o]) continue;
                // Show the differing byte both raw and as an int/float, since a property is usually one of
                // those and the interpretation is what makes it actionable.
                int ai, bi; float af, bf;
                memcpy(&ai, a + o, 4); memcpy(&bi, b + o, 4);
                memcpy(&af, a + o, 4); memcpy(&bf, b + o, 4);
                logline("[pdiff]   +0x%X: %02X->%02X | as int32 %d -> %d | as float %.4f -> %.4f",
                        o, a[o], b[o], ai, bi, af, bf);
                shown++;
                while (o + 1 < la && a[o+1] != b[o+1]) o++;   // collapse the rest of this run
            }
        }
    }
    if (changed > 20)
        logline("[pdiff] NOTE: %u changed at once - that is a reload invalidating the baseline, not %u edits",
                changed, changed);
    if (!changed && !appeared) logline("[pdiff] nothing changed since the baseline");
    else logline("[pdiff] === %u changed, %u new. Baseline re-armed. ===", changed, appeared);
    memcpy(g_base_live, g_live, sizeof(PcLive) * (n < 4096 ? n : 4096));
    memcpy(g_base_arena, g_arena, g_arena_used);
    g_base_n = n; g_base_used = g_arena_used;
}

// PLAIN RELOAD - one load per press, nothing else. The marker question is "does a load duplicate the
// game's unconnected-input warnings", so the test should be a load and NOTHING else: no splice, no stored
// blob, no property machinery to go wrong or to explain away a result. Serialize the craft, load it
// straight back. Press once for the first load, again for the second - duplicates should appear on the
// second if the list is not being cleared.

// THE DECIDING PROBE (numpad *). Three splices have each lost exactly one component, and the standing theory
// is that a blob is not position-independent - that it carries the component's own voxel, so pasting A's
// blob into B's record makes both claim one position and the loader keeps one.
//
// That is answerable by LOOKING rather than by reasoning about the loader: dump the head of every component's
// blob beside the voxel we know it lives at. If the leading int32s are the voxel, the theory is proven in one
// keypress and the fix follows immediately (preserve the receiver's identity prefix, splice only the tail).
// If they are not, the theory dies here and the drop is something else entirely.
static void pc_probe_blob_head() {
    if (!g_armed || !g_editor) { logline("[head] not armed - open the workbench first"); return; }
    unsigned n = pc_cache_live();
    logline("[head] === blob heads for %u components (voxel we know vs leading bytes) ===", n);
    for (unsigned i = 0; i < n && i < 24; i++) {
        const unsigned char* b = g_arena + g_live[i].off;
        unsigned L = g_live[i].len;
        char hex[128]; hex[0] = 0;
        for (unsigned k = 0; k < 24 && k < L; k++) {
            char t[4]; _snprintf_s(t, sizeof t, _TRUNCATE, "%02X ", b[k]);
            strcat_s(hex, sizeof hex, t);
        }
        int w[6] = {0,0,0,0,0,0};
        for (int k = 0; k < 6 && (unsigned)(k*4+4) <= L; k++) memcpy(&w[k], b + k*4, 4);
        logline("[head] '%s' vox=(%d,%d,%d) len=%u", g_live[i].name,
                g_live[i].vox[0], g_live[i].vox[1], g_live[i].vox[2], L);
        logline("[head]    hex: %s", hex);
        logline("[head]    i32: %d %d %d %d %d %d", w[0], w[1], w[2], w[3], w[4], w[5]);
    }
    // The direct comparison: two components of the SAME name differ only by position and settings, so the
    // first differing byte between their blobs is where identity lives.
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++) {
            if (g_live[i].cat != g_live[j].cat) continue;
            if (strcmp(g_live[i].name, g_live[j].name) != 0) continue;
            const unsigned char* a = g_arena + g_live[i].off;
            const unsigned char* b = g_arena + g_live[j].off;
            unsigned L = g_live[i].len < g_live[j].len ? g_live[i].len : g_live[j].len;
            unsigned d = 0; while (d < L && a[d] == b[d]) d++;
            logline("[head] PAIR '%s' (%d,%d,%d) vs (%d,%d,%d): first difference at byte %u of %u",
                    g_live[i].name, g_live[i].vox[0], g_live[i].vox[1], g_live[i].vox[2],
                    g_live[j].vox[0], g_live[j].vox[1], g_live[j].vox[2], d, L);
            if (d + 4 <= L) {
                int ai, bj; memcpy(&ai, a + d, 4); memcpy(&bj, b + d, 4);
                logline("[head]    at +%u: %d vs %d  (delta %d)", d, ai, bj, bj - ai);
            }
            goto done;                      // one representative pair is enough
        }
done:
    logline("[head] === end ===");
}

static void pc_probe_reload() {
    if (!g_armed || !g_editor) { logline("[reload] not armed - open the workbench first"); return; }
    unsigned len = 0; char* craft = snapshot_serialize(&len);
    if (!craft) { logline("[reload] serialize failed"); return; }
    static int s_n = 0; s_n++;
    logline("[reload] === RELOAD #%d (%u bytes) - plain load, nothing substituted ===", s_n, len);
    snapshot_load_from_buffer(craft, len);
    ((void(*)(void*))(g_base + 0x9B15A0))(craft);
    logline("[reload] === reload #%d done - check for DUPLICATE warning icons ===", s_n);
}

// APPLY A STORED BLOB - the receiver half of property sync, minus the network.
// Takes the bytes captured by the property diff and splices them into the live craft at the matching
// record. That is precisely what applying a partner's change would do: same substitution, same loader, same
// rollback. Only the transport is missing.
static void pc_apply_stored() {
    if (!g_armed || !g_editor) { logline("[apply] not armed"); return; }
    if (!g_stored_len) { logline("[apply] nothing stored - press numpad + to baseline, change something, press + again"); return; }
    logline("[apply] === APPLYING %u stored bytes for '%s' ===", g_stored_len, g_stored_name);

    unsigned clen = 0; char* craft = snapshot_serialize(&clen);
    if (!craft) { logline("[apply] serialize failed"); return; }
    static unsigned char orig[0x600000];
    if (clen > sizeof orig) { ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[apply] craft too large"); return; }
    memcpy(orig, craft, clen);

    unsigned expect_n = pc_expect_components();   // every body, not just body[0]
    pc_cache_live();
    unsigned anchor2 = 0;
    if (!pc_find_anchor((const unsigned char*)craft, clen, &anchor2)) {
        ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[apply] no anchor"); return; }
    static PcRec recs[4096]; unsigned bad = 0;
    unsigned n = pc_index_craft((const unsigned char*)craft, clen, anchor2, expect_n, recs, 4096, &bad);
    if (!n || bad) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[apply] index incomplete (%u/%u, %u bad)", n, expect_n, bad); return; }

    // Match on name + cat. With one rotor in the craft this is unambiguous; a real apply would carry the
    // voxel from the sender and match on that too.
    int t = -1;
    for (unsigned r = 0; r < n; r++)
        if (recs[r].cat == g_stored_cat && strcmp(recs[r].name, g_stored_name) == 0) { t = (int)r; break; }
    if (t < 0) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[apply] no '%s' record in the craft", g_stored_name); return; }

    static unsigned char spliced[0x600000]; unsigned slen = 0;
    bool built = pc_build_spliced((const unsigned char*)craft, clen, &recs[t], g_stored, g_stored_len,
                                  spliced, sizeof spliced, &slen);
    unsigned oldlen = recs[t].bloblen;
    ((void(*)(void*))(g_base+0x9B15A0))(craft);
    if (!built) { logline("[apply] build failed - craft untouched"); return; }
    logline("[apply] record %u '%s': %u -> %u bytes, craft %u -> %u. Loading ...",
            t, recs[t].name, oldlen, g_stored_len, clen, slen);

    snapshot_load_from_buffer((char*)spliced, slen);

    unsigned vlen = 0; char* v = snapshot_serialize(&vlen);
    bool ok = v && vlen == slen;
    if (v) ((void(*)(void*))(g_base+0x9B15A0))(v);
    if (ok) logline("[apply] *** PASS - applied. Look at the '%s' in-game: it should show the stored state. ***",
                    g_stored_name);
    else { logline("[apply] *** FAIL (got %u, expected %u) - rolling back ***", vlen, slen);
        crumb("psync: rolling back after a failed splice");
           snapshot_load_from_buffer((char*)orig, clen); }
    logline("[apply] === end ===");
}

// ======================= PROPERTY SYNC OVER THE WIRE (kind 16) =======================
// The last big gap the public README admits: "component settings and microcontrollers need a resync".
// Everything except transport already existed - pc_cache_live captures every component's serialized blob,
// and the splice substitutes one blob inside a craft and lets the game's own loader rebuild (SS30-SS34).
// This wires those two together.
//
// Shape, deliberately following the move-sync pattern that shipped in SS28: poll -> diff -> send -> apply on
// the main thread under g_suppress.
//
// Three things make this different from every other message kind, and each drove a decision here:
//
//  1. THE PAYLOAD DOES NOT FIT THE APPLY QUEUE. ApplyItem carries a 96-byte name; a microcontroller blob is
//     ~16 KB. So property applies get their own pending set rather than riding the ring.
//  2. AN APPLY IS A WHOLE-CRAFT RELOAD. That is expensive and visible, so pending changes COALESCE: N
//     components arriving together are spliced into one craft and loaded once, not N times. Coalescing by
//     (voxel,name,cat) also means a slider dragged through twenty values costs one apply, not twenty.
//  3. AN APPLY LOOKS EXACTLY LIKE A CRAFT-WIDE EDIT. The reload rebuilds every component, so the next scan
//     would diff the whole craft against a stale baseline and broadcast it all back. The baseline is
//     therefore re-armed from the post-load state, and a scan that sees a suspiciously large number of
//     changes re-arms instead of sending - that is a reload, not an edit (the guard SS34 already uses).

// 128 KB. A microcontroller measured 15,976 bytes (SS30), but that was a modest one - a Lua-heavy MC
// clears 32 KB easily, and the interesting case for co-op is precisely the big one someone spent an hour on.
// Whatever the cap is, exceeding it must be VISIBLE: silently dropping the update looks identical to sync
// being broken, and the partner has no way to know they are out of date.
static const unsigned PROP_MAX_BLOB = 0x20000;
static const unsigned PROP_PEND_MAX = 32;       // distinct components pending at once
static const unsigned PROP_ARENA    = 0x40000;  // 256 KB of pending blob bytes

// `cap` is the arena span originally reserved; `len` is what currently occupies it. They were one field, so
// a shrinking update overwrote the capacity and every later mid-sized one took fresh arena instead of reusing
// the span - a slow leak of the 256 KB pool under exactly the workload property sync generates.
struct PropPend { int vox[3]; char name[64]; unsigned char cat; unsigned off, len, cap; };
static PropPend      g_pp[PROP_PEND_MAX];
static unsigned      g_pp_n = 0, g_pp_used = 0;
static unsigned char g_pp_arena[PROP_ARENA];
static CRITICAL_SECTION g_pp_cs;
static bool             g_pp_cs_ok = false;

// Received off the Steam thread; drained on the main thread. Coalesces onto an existing entry for the same
// component so a stream of updates for one part cannot fill the set.
static void pc_queue_prop(const int vox[3], const char* name, unsigned char cat,
                          const unsigned char* blob, unsigned len) {
    if (!g_pp_cs_ok || !len || len > PROP_MAX_BLOB) return;
    EnterCriticalSection(&g_pp_cs);
    int slot = -1;
    for (unsigned i = 0; i < g_pp_n; i++) {
        if (g_pp[i].vox[0]!=vox[0] || g_pp[i].vox[1]!=vox[1] || g_pp[i].vox[2]!=vox[2]) continue;
        if (g_pp[i].cat != cat || strcmp(g_pp[i].name, name) != 0) continue;
        slot = (int)i; break;
    }
    // Replacing in place only works when the new blob fits the old span; otherwise take fresh arena.
    if (slot >= 0 && len <= g_pp[slot].cap) {
        memcpy(g_pp_arena + g_pp[slot].off, blob, len);
        g_pp[slot].len = len;                          // cap is unchanged - the span stays reusable
        LeaveCriticalSection(&g_pp_cs); return;
    }
    if (g_pp_used + len > PROP_ARENA || (slot < 0 && g_pp_n >= PROP_PEND_MAX)) {
        static DWORD s_warn = 0; DWORD now = GetTickCount();
        if (now - s_warn > 5000) { s_warn = now;
            logline("!!! property queue full - dropping a settings update. Press F7 to resync."); }
        LeaveCriticalSection(&g_pp_cs); return;
    }
    if (slot < 0) { slot = (int)g_pp_n++; memset(&g_pp[slot], 0, sizeof g_pp[slot]);
                    memcpy(g_pp[slot].vox, vox, 12);
                    strncpy_s(g_pp[slot].name, sizeof g_pp[slot].name, name, _TRUNCATE);
                    g_pp[slot].cat = cat; }
    g_pp[slot].off = g_pp_used; g_pp[slot].len = len; g_pp[slot].cap = len;
    memcpy(g_pp_arena + g_pp_used, blob, len);
    g_pp_used += len;
    LeaveCriticalSection(&g_pp_cs);
}

// ---- the scan baseline. Separate from the numpad-+ probe's baseline on purpose: pressing the probe key
// mid-session must not perturb what the network path considers "already sent".
static unsigned char g_ps_arena[0x600000];
static PcLive        g_ps_live[4096];
static unsigned      g_ps_n = 0, g_ps_used = 0;
static bool          g_ps_armed = false;

static void pc_prop_rearm() {          // snapshot the CURRENT craft as "already in sync"
    unsigned n = pc_cache_live();
    if (n > 4096) n = 4096;
    memcpy(g_ps_live, g_live, sizeof(PcLive) * n);
    memcpy(g_ps_arena, g_arena, g_arena_used);
    g_ps_n = n; g_ps_used = g_arena_used; g_ps_armed = true;
    InterlockedExchange(&g_ps_dirty, 0);
    // Every "why was my change not detected" question comes back to what the baseline held at the time, so
    // log the transition rather than leaving it to be inferred.
    logline("[psync] baseline := %u components, %u bytes", g_ps_n, g_ps_used);
}
// Every whole-craft reload (F7 pull, F4, a splice, our own apply) invalidates the baseline: the loader
// rebuilds every component, so a stale baseline reads the entire incoming craft as local edits and sends it
// straight back at the partner. snapshot_load_from_buffer raises g_ps_dirty for ALL of those paths at once,
// which is why there is no per-call-site invalidation to forget.

static bool emit_prop_blob(const int vox[3], const char* name, unsigned char cat,
                           const unsigned char* blob, unsigned len) {
    unsigned nl = (unsigned)strlen(name); if (nl > MAXNAME) nl = MAXNAME;
    if (!len) return false;
    if (len > PROP_MAX_BLOB) {
        logline("!!! '%s' at (%d,%d,%d) is %u bytes - too large to sync (limit %u). Your partner will not see "
                "changes to it; press F7 to send the whole craft instead.",
                name, vox[0], vox[1], vox[2], len, PROP_MAX_BLOB);
        return false;
    }
    unsigned total = (unsigned)sizeof(PlaceMsg) + nl + len;
    static BYTE buf[sizeof(PlaceMsg) + MAXNAME + PROP_MAX_BLOB];
    PlaceMsg* m = (PlaceMsg*)buf; memset(buf, 0, sizeof(PlaceMsg));
    m->magic=MAGIC; m->ver=1; m->kind=16; m->namelen=(uint16_t)nl;
    m->x=vox[0]; m->y=vox[1]; m->z=vox[2];
    m->rot[0] = (int32_t)len;              // blob length; the blob follows the name bytes
    m->cat = cat;
    memcpy(buf + sizeof(PlaceMsg), name, nl);
    memcpy(buf + sizeof(PlaceMsg) + nl, blob, len);

    if (g_localecho) {
        // SOLO TEST. Applying the blob back onto the component it came from proves nothing - it is already
        // in that state. Shifting the target by the same +X the place echo uses lands it on the echo copy,
        // so a settings change on one part visibly transfers to another. That exercises the whole receive
        // path (match by voxel, splice, reload, verify) on one machine.
        m->x = vox[0] + g_echo_dy;
        logline(">>> LOCAL-ECHO prop '%s' (%d,%d,%d) -> (%d,%d,%d), %u bytes",
                name, vox[0],vox[1],vox[2], m->x,vox[1],vox[2], len);
        handle_place_msg(buf, (int)total);
        return true;
    }
    // A refused send must NOT be counted as sent: the caller re-arms the baseline over anything it believes
    // it delivered, so a partner stepping out of the bench for three seconds would otherwise bury the change
    // permanently. Returning false keeps it out of the baseline and the next scan picks it up again.
    if (!p_send || !g_net || !g_peerid || sync_paused()) return false;
    int rc = p_send(g_net, g_peerIdent, buf, total, SEND_RELIABLE, CHANNEL);
    logline(">>> SEND prop-blob '%s' (%d,%d,%d) %u bytes EResult=%d %s", name, vox[0],vox[1],vox[2], len, rc,
            rc==1?"(OK)":(rc==35?"(ConnectFailed)":(rc==3?"(NoConnection)":"(err)")));
    if ((rc==35 || rc==3) && p_close) { p_close(g_net, g_peerIdent); InterlockedExchange(&g_session_ok, 0); }
    // EResult 25 = the send buffer is full. The chunked-pull path already paces on it; this one did not, so a
    // burst of property blobs was dropped on the floor with only an "EResult=25" in the log to show for it.
    // Report failure so the caller leaves the component OUT of the baseline and the next scan retries it.
    return rc == 1;
}

// ---- DETECT. Polled rather than hooked: a property is changed through the game's own UI, which writes
// straight into the component with no call we own. Diffing serialized blobs is the only signal that covers
// every property of every class uniformly - the alternative is a per-class offset table, which SS31 killed.
// OFF SWITCH. Property sync is the newest and least proven path in the mod, and it is the only one that
// reloads the whole craft as a side effect of someone touching a slider. A file beside the mod turns it off
// without a rebuild - so a session can continue when it misbehaves, instead of ending.
static int g_props_off = -1;                     // -1 = not yet checked
static bool props_disabled() {
    if (g_props_off < 0) {
        char path[MAX_PATH]; _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s", LOGP);
        char* sl = strrchr(path, '\\');
        if (sl) _snprintf_s(sl + 1, MAX_PATH - (sl + 1 - path), _TRUNCATE, "coop-noprops.txt");
        FILE* f = nullptr;
        g_props_off = (!fopen_s(&f, path, "r") && f) ? 1 : 0;
        if (f) fclose(f);
        if (g_props_off) logline("[psync] DISABLED by coop-noprops.txt - component settings will not sync.");
    }
    return g_props_off == 1;
}

// One scan cycle of settle time before a change is transmitted. Identity plus a content hash is enough to
// answer "is this the same bytes I saw a second ago" without keeping a second copy of every blob.
struct PsCand { int vox[3]; char name[64]; unsigned char cat; unsigned len, hash; };
static unsigned ps_hash(const unsigned char* p, unsigned n) {
    unsigned h = 2166136261u;                                  // FNV-1a
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void pc_prop_scan() {
    if (props_disabled()) return;
    crumb("psync: scanning components for property changes");
    if (!g_armed || !g_editor || g_suppress) return;
    unsigned n = pc_cache_live();
    if (!n) return;
    if (!g_ps_armed) { pc_prop_rearm(); logline("[psync] baseline armed (%u components)", g_ps_n); return; }
    if (InterlockedExchange(&g_ps_dirty, 0)) { pc_prop_rearm(); return; }   // a load happened - rebaseline, send nothing

    // First pass: count, so a reload can be recognised BEFORE anything is broadcast.
    unsigned changed = 0, appeared = 0;
    int hit[64]; int nhit = 0;
    for (unsigned i = 0; i < n; i++) {
        int j = -1;
        for (unsigned k = 0; k < g_ps_n; k++) {
            if (g_ps_live[k].vox[0]!=g_live[i].vox[0] || g_ps_live[k].vox[1]!=g_live[i].vox[1] ||
                g_ps_live[k].vox[2]!=g_live[i].vox[2] || g_ps_live[k].cat!=g_live[i].cat) continue;
            if (strcmp(g_ps_live[k].name, g_live[i].name) != 0) continue;
            j = (int)k; break;
        }
        if (j < 0) {
            appeared++;
            // DIAGNOSTIC. A component is matched on (voxel, name, cat). If the same name+cat exists in the
            // baseline at a DIFFERENT voxel, then identity churned across a reload rather than a part being
            // added - and every edit to that component reads as "appeared", so it is silently re-baselined
            // and never sent. That is the shape of "I changed it a second time and nothing happened".
            // Only meaningful when the name is UNIQUE. Every block is called '01_block', so "same name at a
            // different voxel" matched an unrelated block and cried identity-churn on ordinary placements.
            // Count first; one candidate means it really is the same part somewhere else, more than one
            // means the name carries no identity and the test cannot say anything.
            int cand = -1, ncand = 0;
            for (unsigned k = 0; k < g_ps_n && ncand < 2; k++) {
                if (g_ps_live[k].cat != g_live[i].cat) continue;
                if (strcmp(g_ps_live[k].name, g_live[i].name) != 0) continue;
                cand = (int)k; ncand++;
            }
            if (ncand == 1 && strcmp(g_live[i].name, "01_block") != 0)
                logline("[psync] IDENTITY MOVED: '%s' was at (%d,%d,%d) in the baseline, now at (%d,%d,%d) "
                        "(len %u -> %u) - its changes cannot be matched",
                        g_live[i].name, g_ps_live[cand].vox[0], g_ps_live[cand].vox[1], g_ps_live[cand].vox[2],
                        g_live[i].vox[0], g_live[i].vox[1], g_live[i].vox[2], g_ps_live[cand].len, g_live[i].len);
            continue;
        }
        if (g_ps_live[j].len == g_live[i].len &&
            memcmp(g_ps_arena + g_ps_live[j].off, g_arena + g_live[i].off, g_live[i].len) == 0) continue;
        changed++;
        if (nhit < 64) hit[nhit++] = (int)i;
    }
    // A component that just appeared has no baseline entry, so it is skipped above - but if the baseline is
    // never re-armed it stays skipped FOREVER and that part's settings never sync at all. Re-arm whenever the
    // craft's shape has moved on, even with nothing to send.
    if (!changed) { if (appeared || n != g_ps_n) pc_prop_rearm(); return; }

    // A person changes one setting at a time. A dozen at once is a craft reload invalidating the baseline
    // (SS34) - re-arm and send nothing, rather than broadcasting the whole craft back at the partner.
    if (changed > 8) {
        logline("[psync] %u components differ at once - treating as a reload, re-arming (nothing sent)", changed);
        pc_prop_rearm(); return;
    }
    // SETTLE BEFORE SENDING. A change is only transmitted once it has stopped changing for one scan cycle.
    // Without this, typing a name sends once per keystroke and every one of those costs the partner a whole
    // craft reload - and editing a microcontroller would reload their craft every second for as long as they
    // kept working. One second of latency is nothing; a reload per keystroke is unusable.
    unsigned sent = 0;
    static PsCand cand[64]; static unsigned ncand = 0;
    PsCand next[64]; unsigned nnext = 0;
    for (int h = 0; h < nhit; h++) {
        unsigned i = (unsigned)hit[h];
        unsigned hsh = ps_hash(g_arena + g_live[i].off, g_live[i].len);
        bool settled = false;
        for (unsigned c = 0; c < ncand; c++) {
            if (cand[c].cat != g_live[i].cat || cand[c].len != g_live[i].len || cand[c].hash != hsh) continue;
            if (cand[c].vox[0]!=g_live[i].vox[0] || cand[c].vox[1]!=g_live[i].vox[1] ||
                cand[c].vox[2]!=g_live[i].vox[2]) continue;
            if (strcmp(cand[c].name, g_live[i].name) != 0) continue;
            settled = true; break;
        }
        if (!settled) {
            if (nnext < 64) {
                memcpy(next[nnext].vox, g_live[i].vox, 12);
                strncpy_s(next[nnext].name, sizeof next[nnext].name, g_live[i].name, _TRUNCATE);
                next[nnext].cat = g_live[i].cat; next[nnext].len = g_live[i].len; next[nnext].hash = hsh;
                nnext++;
            }
            continue;                                  // still being edited - wait for it to settle
        }
        logline("[psync] CHANGED '%s' at (%d,%d,%d), %u bytes -> sending", g_live[i].name,
                g_live[i].vox[0], g_live[i].vox[1], g_live[i].vox[2], g_live[i].len);
        if (emit_prop_blob(g_live[i].vox, g_live[i].name, g_live[i].cat,
                           g_arena + g_live[i].off, g_live[i].len)) sent++;
    }
    memcpy(cand, next, sizeof(PsCand) * nnext); ncand = nnext;
    // Re-arm ONLY when something was actually sent. Re-arming while a change is still settling would fold it
    // into the baseline and it would never be sent at all - the change would simply vanish.
    if (!sent) return;
    pc_prop_rearm();
}

// ---- APPLY. One serialize, one index, every pending splice, ONE load.
// Splices run in DESCENDING record offset so each substitution leaves the offsets below it untouched - the
// reason this can batch at all without re-indexing between changes.
static unsigned char g_ps_a[0x600000], g_ps_b[0x600000];

// An apply tears down and rebuilds EVERY component. The properties panel and the microcontroller sub-editor
// both hold raw pointers into components (SS36), and the sub-editor is a whole editing session - reloading
// under it would discard someone's work mid-edit even if it did not crash. So: hold the update until they
// close it. Deferring is free (the pending set coalesces), and holding an update is always better than
// yanking the craft out from under a player who is typing into it.
static bool ui_busy(const char** why) {
    unsigned long long sel = 0, mc = 0;
    safe_copy(&sel, (void*)(g_editor + 0x1550), 8);
    safe_copy(&mc,  (void*)(g_editor + 0x9BF0), 8);
    if (mc)  { if (why) *why = "microcontroller editor open"; return true; }
    if (sel) { if (why) *why = "properties panel open";       return true; }
    return false;
}

static void pc_apply_pending() {
    if (!g_pp_cs_ok || !g_pp_n) return;
    if (!g_sync_enabled) {          // solo: drop, same reasoning as drain_apply_queue
        EnterCriticalSection(&g_pp_cs); g_pp_n = 0; g_pp_used = 0; LeaveCriticalSection(&g_pp_cs); return; }
    if (props_disabled()) { g_pp_n = 0; g_pp_used = 0; return; }
    { const char* why = nullptr;
      if (g_armed && g_editor && ui_busy(&why)) {
          static DWORD s_last = 0; DWORD now = GetTickCount();
          if (now - s_last > 4000) { s_last = now;
              logline("[psync] holding %u update(s) - %s. They apply when you close it.", g_pp_n, why); }
          return;
      } }
    if (!g_armed || !g_editor || !g_in_bench) return;

    static PropPend pend[PROP_PEND_MAX]; unsigned np;
    static unsigned char blobs[PROP_ARENA]; unsigned nblob;
    EnterCriticalSection(&g_pp_cs);
    np = g_pp_n; nblob = g_pp_used;
    memcpy(pend, g_pp, sizeof(PropPend) * np);
    memcpy(blobs, g_pp_arena, nblob);
    g_pp_n = 0; g_pp_used = 0;
    LeaveCriticalSection(&g_pp_cs);

    crumb("psync: applying peer property update");
    logline("[psync] applying %u pending property update(s)", np);

    unsigned clen = 0; char* craft = snapshot_serialize(&clen);
    if (!craft) { logline("[psync] serialize failed - update dropped"); return; }
    if (clen > sizeof g_ps_a) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[psync] craft %u bytes exceeds the buffer - update dropped", clen); return; }
    memcpy(g_ps_a, craft, clen);

    unsigned expect_n = pc_expect_components();   // every body, not just body[0]
    pc_cache_live();
    unsigned anchor = 0;
    if (!pc_find_anchor((const unsigned char*)craft, clen, &anchor)) {
        ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[psync] no anchor - update dropped"); return; }
    static PcRec recs[4096]; unsigned bad = 0;
    unsigned n = pc_index_craft((const unsigned char*)craft, clen, anchor, expect_n, recs, 4096, &bad);
    ((void(*)(void*))(g_base+0x9B15A0))(craft);
    if (!n || bad || n != expect_n) {
        // ONE RETRY BEFORE GIVING UP. The index matches records by exact memcmp against the live blob cache,
        // and the craft was serialized a moment BEFORE that cache was built - so a single float ticking in
        // between makes one record unmatchable and the walk stops there, discarding every pending update.
        // Re-serializing and re-indexing closes that window in the common case; reordering the two calls
        // would not, because the gap is inherent to doing them at different instants.
        logline("[psync] index incomplete (%u of %u, %u bad) - re-serializing and retrying once", n, expect_n, bad);
        unsigned clen2 = 0; char* craft2 = snapshot_serialize(&clen2);
        if (craft2 && clen2 <= sizeof g_ps_a) {
            memcpy(g_ps_a, craft2, clen2); clen = clen2;
            pc_cache_live();
            bad = 0; anchor = 0;
            if (pc_find_anchor((const unsigned char*)craft2, clen2, &anchor))
                n = pc_index_craft((const unsigned char*)craft2, clen2, anchor, expect_n, recs, 4096, &bad);
            else n = 0;
        }
        if (craft2) ((void(*)(void*))(g_base+0x9B15A0))(craft2);
        if (!n || bad || n != expect_n) {
            logline("[psync] index still incomplete (%u of %u, %u bad) - update dropped, craft untouched",
                    n, expect_n, bad);
            return;
        }
        logline("[psync] retry indexed cleanly (%u records)", n);
    }

    // Pair each pending update with its record, then order by DESCENDING offset.
    int tgt[PROP_PEND_MAX]; unsigned order[PROP_PEND_MAX]; unsigned nt = 0;
    for (unsigned p = 0; p < np; p++) {
        int t = -1;
        for (unsigned r = 0; r < n; r++) {
            if (recs[r].cat != pend[p].cat) continue;
            if (recs[r].vox[0]!=pend[p].vox[0] || recs[r].vox[1]!=pend[p].vox[1] ||
                recs[r].vox[2]!=pend[p].vox[2]) continue;
            if (strcmp(recs[r].name, pend[p].name) != 0) continue;
            t = (int)r; break;
        }
        if (t < 0) {
            // Its own log line names the reason, and then it threw the update away anyway. Placements forge
            // at 8 per frame, so a 50-block drag takes several frames to land - a settings change riding just
            // behind one legitimately arrives before its component. Put it back and try again shortly.
            logline("[psync] '%s' at (%d,%d,%d) not here yet - holding (place sync has not caught up)",
                    pend[p].name, pend[p].vox[0], pend[p].vox[1], pend[p].vox[2]);
            pc_queue_prop(pend[p].vox, pend[p].name, pend[p].cat, blobs + pend[p].off, pend[p].len);
            continue;
        }
        tgt[nt] = t; order[nt] = p; nt++;
    }
    if (!nt) return;
    for (unsigned a = 0; a + 1 < nt; a++)                       // insertion sort, nt <= 32
        for (unsigned b = a + 1; b < nt; b++)
            if (recs[tgt[b]].off > recs[tgt[a]].off) {
                int ti = tgt[a]; tgt[a] = tgt[b]; tgt[b] = ti;
                unsigned oi = order[a]; order[a] = order[b]; order[b] = oi;
            }

    static unsigned char orig[0x600000];
    memcpy(orig, g_ps_a, clen);                                 // pre-splice craft, for rollback
    unsigned char* cur = g_ps_a; unsigned curlen = clen;
    unsigned char* nxt = g_ps_b;
    unsigned applied = 0;
    for (unsigned k = 0; k < nt; k++) {
        PropPend* pp = &pend[order[k]];
        // IDENTITY STAYS WITH THE RECEIVER. A blob's first 12 bytes are the component's own voxel as three
        // int32s - proven by dumping them beside the position we already knew (SS37.3): '01_block' at
        // (2,0,0) begins `02 00 00 00 00 00 00 00 00 00 00 00`, and two identical parts differ from byte 0.
        // So a blob is NOT position-independent, and splicing the sender's bytes wholesale made the target
        // claim the SENDER's voxel. Two components then occupied one position and the loader kept one -
        // which is the "load lost exactly one component" signature, three times running.
        // Take the settings, keep our own position.
        // ...but ONLY for classes whose blob actually begins with it. A microprocessor's blob opens with its
        // NAME (<u16 28>"ZE Modular Engine Controller" - see pc_cache_one), so bytes 0..11 there are a length
        // prefix and the first characters. Rewriting those would always fire (they never equal a voxel) and
        // would destroy the record's length prefix - a guaranteed corrupt load on exactly the flagship case.
        // Our OWN bytes for this record settle it: if they start with our voxel, the class is base-first.
        // (cur + off + hdr is valid because splices run in descending offset order.)
        const PcRec* R = &recs[tgt[k]];
        const unsigned char* rblob = cur + R->off + R->hdr;
        bool base_first = (R->bloblen >= 12) && memcmp(rblob, R->vox, 12) == 0;
        if (base_first) {
            if (pp->len < 12) { logline("[psync]   '%s': update is %u bytes, too short to carry identity - skipped",
                                        pp->name, pp->len); continue; }
            int had[3]; memcpy(had, blobs + pp->off, 12);
            if (memcmp(had, R->vox, 12) != 0) {
                logline("[psync]   '%s': rewriting the blob's voxel (%d,%d,%d) -> our own (%d,%d,%d)",
                        pp->name, had[0], had[1], had[2], R->vox[0], R->vox[1], R->vox[2]);
                memcpy(blobs + pp->off, R->vox, 12);
            }
        } else {
            // Derived preamble: the voxel is not at offset 0 and we do not know where it is. Splice whole -
            // over the wire the record was matched BY voxel, so sender and receiver already agree on it.
            logline("[psync]   '%s': derived-prefix class, splicing whole (no identity rewrite)", pp->name);
        }
        unsigned slen = 0;
        if (!pc_build_spliced(cur, curlen, &recs[tgt[k]], blobs + pp->off, pp->len,
                              nxt, 0x600000, &slen)) {
            logline("[psync] splice failed for '%s' - skipping it", pp->name); continue; }
        logline("[psync]   '%s' at (%d,%d,%d): %u -> %u bytes", pp->name,
                pp->vox[0], pp->vox[1], pp->vox[2], recs[tgt[k]].bloblen, pp->len);
        unsigned char* sw = cur; cur = nxt; nxt = sw; curlen = slen; applied++;
    }
    if (!applied) return;

    // The loader clears the vehicle before it can fail, so a rejected load leaves a half-built craft, not
    // the original (SS30 safety note). Roll back on any mismatch.
    suppress_push();
    crumb("psync: loading spliced craft");
    snapshot_load_from_buffer((char*)cur, curlen);
    unsigned vlen = 0; char* v = snapshot_serialize(&vlen);
    // VERIFICATION BAR: exact length, plus fewer than 0.01% differing bytes. NOT byte-equality.
    // §33.2 measured that a *faithful* round-trip still differs - 18 bytes of 300,929 - because live
    // simulation state (gate_float_constant and friends) ticks between the two serialisations. Demanding
    // exact equality therefore rejects CORRECT applies on any craft with running logic, and each rejection
    // costs a second whole-craft reload for the rollback. Confirmed in the wild: a dial min/max change
    // reported "did not reproduce (747 vs 747)" - identical length, a couple of live bytes apart.
    // This is the same bar the identity splice already uses.
    unsigned ndiff = 0;
    if (v && vlen == curlen)
        for (unsigned i = 0; i < curlen; i++) if ((unsigned char)v[i] != cur[i]) ndiff++;
    // The allowance must be ABSOLUTE, not proportional. A "<0.01%" bar needs curlen > 10000 before it
    // tolerates even ONE differing byte - so on the small test craft that motivated this fix (747 bytes) it
    // still demanded byte-equality and still rolled back. The drift is a few live floats per ticking
    // component, so scale it to the COMPONENT COUNT, which is what actually generates it.
    const unsigned allow = 4 + g_nlive / 8;
    bool ok = v && vlen == curlen && ndiff <= allow;
    if (v) ((void(*)(void*))(g_base+0x9B15A0))(v);
    if (!ok) {
        logline("[psync] *** load did not reproduce the update (%u vs %u bytes, %u differing, %u allowed) - ROLLING BACK ***",
                vlen, curlen, ndiff, allow);
        crumb("psync: rolling back after a failed splice");
        snapshot_load_from_buffer((char*)orig, clen);
        // VERIFY THE ROLLBACK. It was silent before, which is the wrong place to be optimistic: the rollback
        // runs the same loader that just demonstrably failed to reproduce a craft, so "we restored it" was an
        // assumption rather than a fact. If this ever reports a mismatch the craft has quietly drifted, which
        // matters far more than the update that failed.
        unsigned rlen = 0; char* rv = snapshot_serialize(&rlen);
        bool restored = rv && rlen == clen && memcmp(rv, orig, clen) == 0;
        if (rv) ((void(*)(void*))(g_base+0x9B15A0))(rv);
        if (restored) logline("[psync] rollback verified - craft restored exactly (%u bytes)", clen);
        else          logline("[psync] !!! ROLLBACK DID NOT RESTORE (%u vs %u bytes) - the craft has drifted. "
                              "Press F7 to resync from your partner, or F4 to reload a saved copy.", rlen, clen);
    } else {
        logline("[psync] *** applied %u update(s) - craft %u -> %u bytes (%u live bytes drifted) ***",
                applied, clen, curlen, ndiff);
    }
    // Re-arm from the post-load craft: the reload rebuilt every component, so without this the next scan
    // diffs against a stale baseline and sends the partner's own change straight back.
    pc_prop_rearm();
    suppress_pop();
}

// Net-thread entry for kind 16. Kept out of enqueue_apply because the blob does not fit ApplyItem.
static void pc_net_prop(const BYTE* buf, int c) {
    const PlaceMsg* m = (const PlaceMsg*)buf;
    unsigned nl = m->namelen, bl = (unsigned)m->rot[0];
    if (nl > MAXNAME || !bl) return;
    if (bl > PROP_MAX_BLOB) { logline("<<< prop-blob REFUSED: %u bytes exceeds the %u limit", bl, PROP_MAX_BLOB); return; }
    if ((unsigned)c < sizeof(PlaceMsg) + nl + bl) {
        logline("<<< prop-blob TRUNCATED: got %d bytes, need %u (name %u + blob %u) - dropped",
                c, (unsigned)sizeof(PlaceMsg) + nl + bl, nl, bl);
        return;
    }
    char nm[MAXNAME+1]; memcpy(nm, buf + sizeof(PlaceMsg), nl); nm[nl] = 0;
    int vox[3] = { m->x, m->y, m->z };
    logline("<<< RECV prop-blob '%s' (%d,%d,%d) %u bytes", nm, vox[0], vox[1], vox[2], bl);
    pc_queue_prop(vox, nm, m->cat, buf + sizeof(PlaceMsg) + nl, bl);
}


// PROPERTY SPLICE: change a real property through the splice and see it in-game.
// The identity splice proved the pipeline moves bytes faithfully. This proves it moves MEANING: find a
// component whose blob carries a player-set text string, alter that string, splice, reload - and the part
// should show the new text in the game's own UI.
// Deliberately edits IN PLACE with the same length, so no offset in the record shifts. Renaming to a
// different length would work too (the splice handles it) but keeping the length constant isolates the
// property change from the resizing logic.
static void pc_probe_splice_property() {
    if (!g_armed || !g_editor) { logline("[prop] not armed"); return; }
    logline("[prop] === PROPERTY SPLICE (edits a real name through the pipeline) ===");

    unsigned clen = 0; char* craft = snapshot_serialize(&clen);
    if (!craft) { logline("[prop] serialize failed"); return; }
    static unsigned char orig[0x600000];
    if (clen > sizeof orig) { ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[prop] craft too large"); return; }
    memcpy(orig, craft, clen);

    unsigned nlive = pc_cache_live();
    unsigned expect_n = pc_expect_components();   // every body, not just body[0]
    unsigned anchor2 = 0;
    if (!pc_find_anchor((const unsigned char*)craft, clen, &anchor2)) {
        ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[prop] no anchor"); return; }
    static PcRec recs[4096]; unsigned bad = 0;
    unsigned n = pc_index_craft((const unsigned char*)craft, clen, anchor2, expect_n, recs, 4096, &bad);
    if (!n || bad) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[prop] index incomplete (%u/%u, %u bad)", n, expect_n, bad); return; }

    // Find the BEST player-set name in the craft, not the first text-shaped run. Colour data is full of
    // hex-looking strings ("6E6E6E+", "5,x,8F0000,x,x") that pass a naive printable-run test, so score
    // candidates by how many LOWERCASE letters they contain: a real name like "swcoopbench" scores high, a
    // colour list scores zero. Requiring lowercase also guarantees the uppercase edit below actually changes
    // something.
    int target = -1; unsigned toff = 0, tlen = 0, bestscore = 0;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char* b = (const unsigned char*)craft + recs[r].off + recs[r].hdr;
        unsigned L = recs[r].bloblen, run = 0;
        for (unsigned i = 0; i <= L; i++) {
            unsigned char c = (i < L) ? b[i] : 0;
            bool pr = (c >= 0x20 && c < 0x7F);
            if (pr) { run++; continue; }
            if (run >= 5) {
                const unsigned char* st = b + (i - run);
                unsigned lower = 0, hexish = 0;
                for (unsigned k = 0; k < run; k++) {
                    char ch = (char)st[k];
                    if (ch >= 'a' && ch <= 'z') lower++;
                    if ((ch>='0'&&ch<='9')||(ch>='A'&&ch<='F')||ch==','||ch=='x'||ch=='+') hexish++;
                }
                // A name is mostly lowercase letters; colour/rotation data is mostly hex, commas and 'x'.
                // Reject CODE. A microcontroller's blob carries its full Lua source, and the longest
                // lowercase run in a craft is therefore a script - the first attempt uppercased 254
                // characters of someone's flight controller, which the game does not round-trip. A
                // user-visible NAME is short and has no code punctuation.
                bool codey = (run > 48);
                if (!codey) for (unsigned k = 0; k < run; k++) {
                    char ch = (char)st[k];
                    if (ch=='{'||ch=='}'||ch=='('||ch==')'||ch=='='||ch=='*'||ch==';'||ch=='['||ch==']') { codey = true; break; }
                }
                if (!codey && lower >= 3 && lower * 2 > run && hexish * 2 < run) {
                    // Show the shortlist. Which string the probe chose, and why, should be visible rather
                    // than something to infer from the outcome.
                    static int shown = 0;
                    if (shown < 8) {
                        char t[64] = {0}; unsigned m = run < 63 ? run : 63; memcpy(t, st, m);
                        logline("[prop]   candidate: '%s' in %s (score %u/%u)", t, recs[r].name, lower, run);
                        shown++;
                    }
                    if (lower > bestscore) {
                        bestscore = lower; target = (int)r;
                        toff = recs[r].off + recs[r].hdr + (i - run); tlen = run;
                    }
                }
            }
            run = 0;
        }
    }
    if (target < 0) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[prop] no player-set NAME found (colour/rotation strings excluded). Rename a part in-game "
                "- e.g. give a dial a custom name - and press again."); return; }

    char before[64] = {0}; unsigned cn = tlen < 63 ? tlen : 63;
    memcpy(before, craft + toff, cn);
    logline("[prop] target record %u '%s' - text \"%s\" at craft offset %u (%u chars)",
            target, recs[target].name, before, toff, tlen);

    // Same-length edit so nothing shifts: uppercase the run.
    static unsigned char spliced[0x600000];
    memcpy(spliced, craft, clen);
    for (unsigned i = 0; i < tlen; i++) {
        unsigned char c = spliced[toff + i];
        if (c >= 'a' && c <= 'z') spliced[toff + i] = (unsigned char)(c - 32);
    }
    char after_txt[64] = {0}; memcpy(after_txt, spliced + toff, cn);
    ((void(*)(void*))(g_base+0x9B15A0))(craft);
    if (memcmp(before, after_txt, cn) == 0) { logline("[prop] text has no lowercase to change - retry on a differently named part"); return; }
    logline("[prop] splicing \"%s\" -> \"%s\" and reloading ...", before, after_txt);

    snapshot_load_from_buffer((char*)spliced, clen);

    // Did it take? Re-serialize and look for the new text at the same place.
    unsigned vlen = 0; char* v = snapshot_serialize(&vlen);
    bool took = v && vlen == clen && memcmp((unsigned char*)v + toff, after_txt, cn) == 0;
    if (v) ((void(*)(void*))(g_base+0x9B15A0))(v);
    if (took) logline("[prop] *** PASS - the property CHANGED through the splice. Look at '%s' in-game: it should now read \"%s\". ***",
                      recs[target].name, after_txt);
    else {
        logline("[prop] *** FAIL - the edit did not survive the reload. Rolling back. ***");
        snapshot_load_from_buffer((char*)orig, clen);
    }
    logline("[prop] === end ===");
}

static void pc_probe_splice_identity() {
    if (!g_armed || !g_editor) { logline("[splice] not armed"); return; }
    logline("[splice] === IDENTITY SPLICE (substitutes a component with itself) ===");

    unsigned clen = 0; char* craft = snapshot_serialize(&clen);
    if (!craft) { logline("[splice] serialize failed"); return; }

    // Keep the ORIGINAL bytes. Everything below can fail; this is what we restore from.
    static unsigned char orig[0x600000];
    if (clen > sizeof orig) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[splice] craft %u bytes exceeds the rollback buffer - refusing", clen); return; }
    memcpy(orig, craft, clen);

    unsigned nlive = pc_cache_live();
    unsigned expect_n = pc_expect_components();   // every body, not just body[0]

    unsigned anchor = 0;
    if (!pc_find_anchor((const unsigned char*)craft, clen, &anchor)) {
        ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[splice] no anchor - aborting, craft untouched"); return; }
    static PcRec recs[4096]; unsigned bad = 0;
    unsigned n = pc_index_craft((const unsigned char*)craft, clen, anchor, expect_n, recs, 4096, &bad);
    if (!n || bad) { ((void(*)(void*))(g_base+0x9B15A0))(craft);
        logline("[splice] index incomplete (%u/%u, %u bad) - aborting, craft untouched", n, expect_n, bad); return; }

    // Target the LAST record: it exercises the tail copy, which a first-record test would not.
    const PcRec* rec = &recs[n-1];
    logline("[splice] target [%u] '%s' off=%u len=%u  (craft %u bytes, %u records, %u live)",
            n-1, rec->name, rec->off, rec->bloblen, clen, n, nlive);

    static unsigned char spliced[0x600000]; unsigned slen = 0;
    if (!pc_build_spliced((const unsigned char*)craft, clen, rec,
                          (const unsigned char*)craft + rec->off + rec->hdr, rec->bloblen,
                          spliced, sizeof spliced, &slen)) {
        ((void(*)(void*))(g_base+0x9B15A0))(craft); logline("[splice] build failed - aborting, craft untouched"); return; }

    bool same_as_orig = (slen == clen) && memcmp(spliced, orig, clen) == 0;
    logline("[splice] built %u bytes, %s original (identity splice must be identical)",
            slen, same_as_orig ? "IDENTICAL to" : "*** DIFFERS from ***");
    ((void(*)(void*))(g_base+0x9B15A0))(craft);
    if (!same_as_orig) { logline("[splice] ABORT - substitution is not byte-neutral. Craft untouched."); return; }

    // Load the spliced craft. From here the craft IS being rebuilt, so every exit path must restore.
    logline("[splice] loading spliced craft ...");
    snapshot_load_from_buffer((char*)spliced, slen);

    // Verify: re-serialize and compare against what we loaded.
    unsigned vlen = 0; char* after = snapshot_serialize(&vlen);
    // VERIFICATION, corrected. Byte-identity after a load is unachievable and was never the right bar: the
    // SIMULATION IS RUNNING, so live values tick between serialises. Measured on a 300,929-byte craft the
    // whole delta was 18 bytes in 8 runs (0.006%), every one a float inside a gate_float_constant - e.g.
    // 00 00 00 00 -> CD CC 8C BE (-0.275f). That is the sim, not the splice.
    // So: require the LENGTH to match exactly (a structural error moves it) and the differing bytes to be a
    // negligible fraction. A real splice failure corrupts a record and shifts everything after it, which
    // blows the length and the ratio together.
    unsigned ndiff = 0;
    if (after && vlen == slen)
        for (unsigned i = 0; i < slen; i++) if (((unsigned char*)after)[i] != spliced[i]) ndiff++;
    bool ok = after && vlen == slen && (ndiff * 10000ull) < (slen ? (unsigned long long)slen : 1ull);   // <0.01%
    static unsigned char after2[0x600000]; unsigned after2len = 0;
    if (after && vlen <= sizeof after2) { memcpy(after2, after, vlen); after2len = vlen; }
    if (after) ((void(*)(void*))(g_base+0x9B15A0))(after);
    if (ok) {
        logline("[splice] *** PASS - %u bytes reloaded, %u differing (%.4f%%, live sim values only). "
                "Pipeline is sound. ***", vlen, ndiff, 100.0 * ndiff / (slen ? slen : 1));
    } else {
        logline("[splice] FAIL: got %u bytes (expected %u), %u differing - beyond the sim-noise floor",
                vlen, slen, ndiff);
        if (after2len == slen) pc_report_diff(spliced, (const unsigned char*)after2, slen, recs, n, "spliced vs reloaded");
        logline("[splice] rolling back to the pre-splice bytes (belt and braces - the craft above is already correct)");
        snapshot_load_from_buffer((char*)orig, clen);
        unsigned rlen = 0; char* r = snapshot_serialize(&rlen);
        bool restored = r && rlen == clen && memcmp(r, orig, clen) == 0;
        if (r) ((void(*)(void*))(g_base+0x9B15A0))(r);
        logline("[splice] rollback: %u bytes vs %u original, %s", rlen, clen,
                restored ? "byte-identical" : "same size but not byte-identical (expected - the load round-trip is not byte-preserving)");
    }
    logline("[splice] === end ===");
}

// -------- SPLICE GATE: the one experiment the whole property route rests on. --------
// The splice plan is: the receiver serialises its OWN craft, substitutes the sender's ~200-byte component
// blob, and hands the result to the proven loader. That only works if a component's standalone blob appears
// VERBATIM inside the whole-craft blob. We have never tested it - two censuses matched on LENGTH, which
// cannot tell equal-length differing blobs apart.
//
// Deliberately does NOT parse the craft header. Searching for each component's bytes is simpler, needs no
// assumption about header layout, and answers the same question: are the blobs embedded unmodified, at
// distinct non-overlapping, strictly increasing offsets?
//
// Read-only throughout: two serialises of our own craft plus memory comparisons. No writes, no loader call.
static const unsigned char* pc_find(const unsigned char* hay, unsigned hn,
                                    const unsigned char* need, unsigned nn, unsigned from) {
    if (!nn || nn > hn) return nullptr;
    for (unsigned i = from; i + nn <= hn; i++)
        if (hay[i] == need[0] && memcmp(hay + i, need, nn) == 0) return hay + i;
    return nullptr;
}
static void pc_probe_splice() {
    if (!g_armed || !g_editor) { logline("[gate] not armed - open the workbench first"); return; }
    logline("[gate] === SPLICE GATE ===");

    // (1) BYTE STABILITY. Serialize the craft twice with no edits between. If the bytes differ, every
    //     offset we compute is meaningless and both the splice and the checksum idea are dead.
    unsigned l1 = 0, l2 = 0;
    char* b1 = snapshot_serialize(&l1);
    if (!b1) { logline("[gate] FAIL: first serialize returned nothing"); return; }
    static unsigned char copy1[0x200000];
    bool big = (l1 > sizeof copy1);
    if (!big) memcpy(copy1, b1, l1);
    ((void(*)(void*))(g_base + 0x9B15A0))(b1);
    char* b2 = snapshot_serialize(&l2);
    if (!b2) { logline("[gate] FAIL: second serialize returned nothing"); return; }
    bool stable = (l1 == l2) && !big && memcmp(copy1, b2, l1) == 0;
    logline("[gate] 1/3 byte stability: len %u vs %u, %s%s", l1, l2,
            stable ? "IDENTICAL" : "*** DIFFER ***", big ? " (craft too large to compare - raise the cap)" : "");

    // (2) EMBEDDING. Every live component's standalone blob must appear verbatim in the craft blob, at
    //     strictly increasing, non-overlapping offsets.
    const unsigned char* craft = (const unsigned char*)b2;
    int sz[3]; bench_size_vox(sz);
    int mx[3] = {16, 8, 16};
    if (sz[0]) bench_max_voxel(sz, mx);
    static void* seen[512]; int nseen = 0;
    static unsigned char blob[PC_CAP];
    unsigned found = 0, missing = 0, total = 0, outoforder = 0;
    unsigned prev_end = 0;
    unsigned first_missing_logged = 0;

    for (int x = -mx[0]; x <= mx[0]; x++)
    for (int y = -mx[1]; y <= mx[1]; y++)
    for (int z = -mx[2]; z <= mx[2]; z++) {
        void* comp = nullptr;
        __try { comp = lookup_component(g_editor, x, y, z); } __except(EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; }
        if (!comp) continue;
        bool dup = false;
        for (int i = 0; i < nseen; i++) if (seen[i] == comp) { dup = true; break; }
        if (dup) continue;
        if (nseen < 512) seen[nseen++] = comp; else continue;

        unsigned len = 0; PcIdent id;
        if (!pc_serialize(comp, blob, sizeof blob, &len, &id)) continue;
        total++;
        // Search forward from the previous match, NOT from 0. A craft with 67 identical blocks has 67
        // byte-identical blobs, so a search from 0 returns the FIRST one every time and 66 components look
        // "out of order" - which is exactly what the first run of this gate reported. Scanning forward also
        // makes the ordering test meaningful rather than tautological.
        const unsigned char* at = pc_find(craft, l2, blob, len, prev_end);
        if (!at) at = pc_find(craft, l2, blob, len, 0);   // fall back, so "missing" still means missing
        if (!at) {
            missing++;
            if (first_missing_logged < 3) {
                first_missing_logged++;
                logline("[gate]   NOT EMBEDDED: %s (%d,%d,%d) len=%u", id.name, x, y, z, len);
            }
            continue;
        }
        found++;
        unsigned off = (unsigned)(at - craft);
        if (off < prev_end) outoforder++;
        else prev_end = off + len;   // only advance on a forward match, or one stray resets the whole scan
    }

    logline("[gate] 2/3 embedding: %u/%u components found verbatim in the craft blob (%u missing)",
            found, total, missing);
    logline("[gate] 3/3 ordering: %u out-of-order/overlapping", outoforder);

    // Ordering is informational, not a gate. Our enumeration is voxel-scan order and the craft blob is in
    // the game's own body/component order; those need not agree, and the splice does not require them to -
    // it needs each blob to exist verbatim at a findable offset, which is criteria 1 and 2.
    bool pass = stable && total > 0 && missing == 0;
    if (pass)
        logline("[gate] *** PASS - blobs are byte-stable and embedded verbatim. SPLICE IS VIABLE. ***");
    else
        logline("[gate] *** FAIL - splice is NOT viable as designed%s%s. Do not build on it. ***",
                stable ? "" : " (unstable bytes)",
                missing ? " (blobs not embedded verbatim)" : "");

    // (4) Settle the deque-label contradiction while we are here: 0x4B9230 treats body+0x3F0 as a COMPONENT
    //     deque, while FINDINGS 12.5 calls it the render-node deque. Walking the wrong one as components is
    //     the crash class we keep avoiding, so read both counts and compare against the live component count.
    __try {
        unsigned long long V = 0;
        if (safe_copy(&V, (void*)(g_editor + 0x13C8), 8) && V) {
            unsigned nb = *(unsigned*)(V + 0x10), bc = *(unsigned*)(V + 0x08), bh = *(unsigned*)(V + 0x0C);
            void** bb = *(void***)(V + 0x00);
            if (bb && bc && nb) {
                char* body = (char*)bb[bh % bc];
                unsigned n400 = 0, n430 = 0;
                safe_copy(&n400, body + 0x400, 4);
                safe_copy(&n430, body + 0x430, 4);
                logline("[gate] deque labels: body+0x400=%u  body+0x430=%u  live components=%u "
                        "(whichever equals the component count is the COMPONENT deque)", n400, n430, total);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { logline("[gate] deque probe EXC"); }

    ((void(*)(void*))(g_base + 0x9B15A0))(b2);
    logline("[gate] === end (nothing modified) ===");
}

static void pc_probe_census() {
    if (!g_armed || !g_editor) { logline("[pc] not armed - open the workbench first"); return; }
    int sz[3]; bench_size_vox(sz);
    int mx[3] = {16, 8, 16};
    if (sz[0]) bench_max_voxel(sz, mx);
    logline("[pc] === CENSUS: scanning +/-%d,%d,%d ===", mx[0], mx[1], mx[2]);

    static void* seen[512]; int nseen = 0;
    struct Row { char name[64]; unsigned typeidx, len, slots; };
    static Row rows[512]; int nrows = 0;
    unsigned nfail = 0;
    static unsigned char blob[PC_CAP];

    for (int x = -mx[0]; x <= mx[0] && nrows < 512; x++)
    for (int y = -mx[1]; y <= mx[1] && nrows < 512; y++)
    for (int z = -mx[2]; z <= mx[2] && nrows < 512; z++) {
        void* comp = nullptr;
        __try { comp = lookup_component(g_editor, x, y, z); } __except(EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; }
        if (!comp) continue;
        bool dup = false;                                  // a multi-voxel part answers on every cell it fills
        for (int i = 0; i < nseen; i++) if (seen[i] == comp) { dup = true; break; }
        if (dup) continue;
        if (nseen < 512) seen[nseen++] = comp;

        unsigned len = 0; PcIdent id;
        if (!pc_serialize(comp, blob, sizeof blob, &len, &id)) { nfail++; continue; }
        Row* r = &rows[nrows++];
        strncpy_s(r->name, sizeof r->name, id.name[0] ? id.name : "(unnamed)", _TRUNCATE);
        r->typeidx = id.typeidx; r->len = len; r->slots = id.nslots;
        logline("[pc]   %-28s typeidx=%-3u len=%-5u  vt=+0x%llX codec=+0x%llX",
                r->name, r->typeidx, r->len,
                (unsigned long long)id.vt - g_base,
                (unsigned long long)id.codec - g_base);
        // Dump printable ASCII runs out of the blob. Player-set text - a custom name, a control-mode label -
        // is stored as a length-prefixed string, so it appears here verbatim. That turns "the codec captures
        // properties" from an inference about byte counts into something visible in the log: rename a part
        // and the new name shows up in its blob. Skips the rotation string, which is always digits/commas.
        { char strs[240]; strs[0] = 0; unsigned run = 0, shown = 0;
          for (unsigned i = 0; i <= len && shown < 5; i++) {
              unsigned char c = (i < len) ? blob[i] : 0;
              bool pr = (c >= 0x20 && c < 0x7F);
              if (pr) { run++; continue; }
              if (run >= 4) {
                  const char* st = (const char*)blob + (i - run);
                  bool numeric = true;                       // the rotation matrix string
                  for (unsigned k = 0; k < run; k++) {
                      char ch = st[k];
                      if (!((ch >= '0' && ch <= '9') || ch == ',' || ch == '-' || ch == '.')) { numeric = false; break; }
                  }
                  // ...and the colour strings. Every component emits bc/bc2/bc3/ac as 6-digit hex BEFORE any
                  // player-set text, and "FFFFFF" is not numeric-only, so without this they ate every slot
                  // and a renamed part never showed its name.
                  bool hexcol = (run == 6);
                  if (hexcol) for (unsigned k = 0; k < run; k++) {
                      char ch = st[k];
                      if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))) { hexcol = false; break; }
                  }
                  if (!numeric && !hexcol) {
                      char one[80]; unsigned n = run < 60 ? run : 60;
                      memcpy(one, st, n); one[n] = 0;
                      if (strs[0]) strncat_s(strs, sizeof strs, " | ", _TRUNCATE);
                      strncat_s(strs, sizeof strs, one, _TRUNCATE);
                      shown++;
                  }
              }
              run = 0;
          }
          if (strs[0]) logline("[pc]       text: %s", strs);
        }
    }
    // Distinct classes, so the whitelist work is sized by classes rather than by parts.
    unsigned distinct = 0; static unsigned types[512];
    for (int i = 0; i < nrows; i++) {
        bool d = false; for (unsigned k = 0; k < distinct; k++) if (types[k] == rows[i].typeidx) { d = true; break; }
        if (!d && distinct < 512) types[distinct++] = rows[i].typeidx;
    }
    logline("[pc] === CENSUS DONE: %d components, %u distinct classes, %u failed ===", nrows, distinct, nfail);
    if (nrows == 512) logline("[pc] (hit the 512 cap - the craft has more parts than that)");
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
                // PASSIVE TEMPLATE CAPTURE. Once we know the placement struct is an editor field, read it
                // straight out - no local placement needed, so an inbound block applies even if you have not
                // touched the bench yet. Validate the +0x10 helper pointer exactly as the drag path does; a
                // half-initialised editor would otherwise hand us a struct that forges garbage.
                if(!g_have_struct && g_tmpl_off && g_tmpl_off!=0xFFFFFFFFu){
                    BYTE cand[0x80];
                    if(safe_copy(cand,(void*)(g_editor+g_tmpl_off),0x80)){
                        unsigned long long helper=0; memcpy(&helper,cand+0x10,8);
                        if(helper>0x10000ULL && helper<0x7FFFFFFFFFFFULL){
                            memcpy(g_struct,cand,0x80); g_have_struct=1; g_flush_pending=1;
                            logline("[tmpl] forge template captured passively from editor+0x%X - no placement needed", g_tmpl_off);
                        }
                    }
                }
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
            } else if (in_bench) {
                g_join_mode = 0;
                // Re-arm co-op on every bench entry. g_sync_enabled is otherwise sticky - written only by the
                // E/Q watcher - so gating inbound on it without this would let one missed keypress disable
                // sync permanently, with nothing on screen to explain why.
                InterlockedExchange(&g_sync_enabled, 1);
            }
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
    // (drain_apply_queue moved BELOW the F7 pull load - see the note there)
    // The diff scanners READ the live vehicle through g_editor every frame. Out of the bench that pointer is
    // stale, so scanning could fault or - worse - see reused memory as "changes" and broadcast garbage.
    // Gate every vehicle-touching pass on actually being in the bench.
    // Re-assert the crash handler regardless of where the player is - a crash outside the bench still wants a
    // report, and this is the only place that runs every frame.
    { static DWORD s_kf = 0; DWORD now = GetTickCount();
      if (now - s_kf > 2000) { s_kf = now; crash_filter_keepalive(); } }
    if (g_in_bench) {
        __try { drain_connection(); } __except(EXCEPTION_EXECUTE_HANDLER){}
        __try { conn_diff(); }        __except(EXCEPTION_EXECUTE_HANDLER){}   // detect + emit wire disconnects
        __try { prop_diff(); }        __except(EXCEPTION_EXECUTE_HANDLER){}   // detect + emit property/name changes
        // Component settings (SS37). The scan serializes every component, so it is polled at ~1 Hz rather
        // than per-frame; a property change is a human action and a second of latency is invisible next to
        // the reload the apply costs anyway.
        { static DWORD s_last = 0; DWORD now = GetTickCount();
          if (now - s_last > 900) { s_last = now;
              __try { pc_prop_scan(); } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[psync] scan EXC"); } } }
        __try { pc_apply_pending(); }
        __except(EXCEPTION_EXECUTE_HANDLER){
            // pc_apply_pending raises g_suppress around the load. Faulting inside that window would leave
            // it raised forever and ALL sync would stop silently - matching the [pull] load EXC handler.
            suppress_pop(); logline("[psync] apply EXC - suppression released");
        }
    }
    // NUMPAD * = per-component WRITE probe (read-only). Deliberately NOT F11: that is the game's own export
    // key, and a probe you cannot press without also triggering an export is a probe nobody will run.
    // NUMPAD * = census of EVERY component in the craft. Bound to the key that demonstrably works rather
    // than asking anyone to hunt for a second one; the single-component probe (pc_probe_write) has already
    // served its purpose across 8 classes and stays in the source, just unbound.
    { static SHORT s_dk=0; SHORT dk=GetAsyncKeyState(VK_ADD);   // NUMPAD + = property diff (arm / compare)
      if((dk&0x8000)&&!(s_dk&0x8000)) __try{ pc_probe_prop_diff(); }__except(EXCEPTION_EXECUTE_HANDLER){ logline("[pdiff] EXC"); }
      s_dk=dk; }
    { static SHORT s_pk=0; SHORT pk=GetAsyncKeyState(VK_MULTIPLY);
      if((pk&0x8000)&&!(s_pk&0x8000)) {
          __try{ pc_probe_blob_head(); }__except(EXCEPTION_EXECUTE_HANDLER){ logline("[head] EXC"); }
      }
      s_pk=pk; }
    (void)&pc_probe_reload;   // unbound - numpad * now runs the blob-head probe
    (void)&pc_probe_write; (void)&pc_probe_census; (void)&pc_probe_splice; (void)&pc_probe_index;
    (void)&pc_probe_splice_identity; (void)&pc_probe_splice_property; (void)&pc_apply_stored;
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
        }__except(EXCEPTION_EXECUTE_HANDLER){ suppress_pop(); logline("[pull] load EXC"); }
        if(g_pull_rx){ VirtualFree(g_pull_rx,0,MEM_RELEASE); g_pull_rx=nullptr; g_pull_rx_total=0; g_pull_rx_got=0; }
        InterlockedExchange(&g_sync_busy,0); }
    // Partner edits drain AFTER the pull load, never before. Draining first applied them to the craft the
    // load was about to destroy, so any edit arriving mid-transfer was silently lost - the feature whose job
    // is FIXING desync was quietly creating one. Ordering is the whole fix; a "busy" gate would have been
    // worse, because g_sync_busy only clears on a complete transfer and a partial one would wedge sync off
    // for the rest of the session.
    __try { drain_apply_queue(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // self-gated on g_in_bench
    // give up the banner if the partner never answers, rather than showing "syncing" forever
    if (g_sync_busy && !g_sync_total && (long)GetTickCount()-g_sync_started > 15000) {
        logline("[pull] no response from partner after 15s - giving up");
        sync_error("partner did not respond"); }
    // F7 = LOAD PEER'S CRAFT. This DESTROYS the local craft - the load clears the vehicle and rebuilds it
    // from the peer's blob - and until now it fired straight off the keypress with no confirmation, which
    // makes it the sharpest foot-gun in the shipped feature: one stray F7 and your own in-progress work is
    // gone with no undo. If there is local work to lose, require a SECOND press within 3s and say what will
    // be replaced. An empty or trivial craft pulls immediately, so the common case is unchanged.
    { static SHORT s_f7=0; SHORT f7=GetAsyncKeyState(VK_F7);
      static DWORD s_armed_at=0;
      if((f7&0x8000)&&!(s_f7&0x8000)) __try{
          unsigned nodes = craft_node_count();
          DWORD now = GetTickCount();
          bool confirmed = s_armed_at && (now - s_armed_at) < 3000;
          if (nodes > 1 && !confirmed) {
              s_armed_at = now;
              logline("!!! F7 will REPLACE your craft (%u nodes) with your partner's. Press F7 again within 3s to confirm.", nodes);
              sync_error("F7 AGAIN TO CONFIRM - this REPLACES your craft");
          } else {
              s_armed_at = 0;
              pull_request();
          }
      }__except(EXCEPTION_EXECUTE_HANDLER){}
      s_f7=f7; }
    // F8 disabled again: 0x4AE3F0 is the per-BODY serializer (writes body+0x147c/+0x288/+0x2c8/+0x718), NOT
    // per-component; the real per-component path is 4 levels deep (0x4AE3F0 -> 0x4B9230 component-list ->
    // factory-DTO lifecycle per comp). Deferred - the F7 PULL already carries all properties in bulk.
    __try { watch_interact_key(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // remember E/Q for the next bench open
    __try { update_workbench_prompt(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // START CO-OP vs JOIN PARTNER
    // Loaded by an ASI loader we start WITH the game, before the localisation table is populated, so the
    // initial patch attempt finds no "Create Vehicle" to replace. Retry a few times rather than losing the
    // prompt entirely. (Manual injection patches first time and never enters this.)
    if (!g_loc_patched) { static int tries=0; static DWORD last=0; DWORD nowp=GetTickCount();
        if (tries < 30 && nowp-last > 2000) { last=nowp; tries++; __try{ patch_workbench_prompt(); }__except(EXCEPTION_EXECUTE_HANDLER){} } }
    __try { sample_cursor(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // publish our hover voxel for the overlay
    __try { sample_bench_origin(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // auto-calibrate the overlay to THIS bench
    __try { sample_camera_world(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // ...and the floating-origin rebase
    // ---- MOVE-TOOL SYNC (kind 15) ----
    // The move tool changes vehicle+0x2D8 (poff) continuously, and voxel0 (vehicle+0x1F0) = benchCentre +
    // poff exactly - verified across 8 corner samples and 3 fine movements. Writing BOTH fields visibly
    // slides the craft (proven in-game with a solo probe), so this needs no schema, no per-type knowledge
    // and no forged command: 3 floats on the wire and two writes on the far side.
    // Detection is a frame-diff, like paint_diff. A drag changes poff every frame, so it is debounced;
    // the final position always ships because a settle-send fires once movement stops.
    if (g_in_bench && g_editor && !g_suppress) __try {
        unsigned long long V=0;
        if (safe_copy(&V,(void*)(g_editor+0x13C8),8) && V) {
            float p[3];
            if (safe_copy(p,(void*)(V+0x2D8),12)) {
                bool moved = g_poff_have && (p[0]!=g_poff_last[0]||p[1]!=g_poff_last[1]||p[2]!=g_poff_last[2]);
                DWORD t=GetTickCount();
                if (moved) {
                    g_poff_last[0]=p[0]; g_poff_last[1]=p[1]; g_poff_last[2]=p[2];
                    g_poff_dirty=1; g_poff_lastmove=t;
                    if (t-g_poff_lastsend>200) { g_poff_lastsend=t; g_poff_dirty=0; emit_move(p); }   // during the drag
                } else if (g_poff_dirty && (t-g_poff_lastmove)>150) {
                    g_poff_dirty=0; g_poff_lastsend=t; emit_move(g_poff_last);                        // settle: final position
                }
                if (!g_poff_have) { g_poff_last[0]=p[0]; g_poff_last[1]=p[1]; g_poff_last[2]=p[2]; g_poff_have=true; }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
    // The solo cursor self-test (own cursor replayed as the partner's) is RETIRED - the marker is confirmed
    // working on two machines and the probe has no job left. It shared F8 with the log viewer, so opening the
    // log summoned a partner cursor with nobody connected, which reads as a sync bug rather than a self-test.
    // A developer probe that fabricates a partner should not outlive its purpose. F8 is the log viewer.
    // Replay places that arrived before we had a forge template. try_arm captures the template on the detect
    // WORKER thread; forging must happen on the MAIN thread, so it only raises a flag and we drain it here.
    if (InterlockedExchange(&g_flush_pending, 0)) __try { flush_pending_places(); } __except(EXCEPTION_EXECUTE_HANDLER){}
    if ((++g_frame % 6)==0 && g_in_bench) __try { paint_diff(); } __except(EXCEPTION_EXECUTE_HANDLER){}   // ~10Hz repaint scan
    // ==== WARNING-ICON RING WATCH (§33) ====
    // editor+0x438 is the child COUNT of the UI element at editor+0x3F8 - the container the game parks the
    // yellow "incomplete connection" icons in (append `mov [r15+0x40],ecx` at 0x326C4C, remove via 0x1DD7E0
    // from 0x326920). It has to be sampled per FRAME, never inside the load: 0x326970 creates the icons
    // during the editor's render walk (0x7DCC90), which runs AFTER snapshot_load_from_buffer has returned.
    // That timing is exactly why the in-load struct diff kept reporting "no growing counters in editor+0..
    // 0x40000" while the counter sat inside that very window - the diff opens and closes before the icons
    // exist, so it always compared two identical values. (The pointer-follow scan missed it for a second
    // reason: it snapshots 0x40 bytes at the pointer TARGET, so following editor+0x428 sampled the child
    // ARRAY, never the count, which lives in the editor struct itself.)
    // Steady state = the number of currently-warned components. Logged only on CHANGE so a step-up is
    // unmissable. PASS looks like: settles at W, a load blips it, and it comes back to W.
    if (g_in_bench && g_editor) __try {
        static DWORD s_last = 0; static unsigned s_prev = 0xFFFFFFFFu;
        DWORD nowr = GetTickCount();
        if (nowr - s_last > 1000) {
            s_last = nowr;
            unsigned cnt=0, cap=0, head=0; unsigned long long data=0;
            if (safe_copy(&data,(void*)(g_editor+0x428),8) && safe_copy(&cap, (void*)(g_editor+0x430),4) &&
                safe_copy(&head,(void*)(g_editor+0x434),4) && safe_copy(&cnt, (void*)(g_editor+0x438),4) &&
                cnt < 1000000u && cap < 1000000u) {
                if (cnt != s_prev) {
                    bool first = (s_prev == 0xFFFFFFFFu);
                    logline("[warn] editor+0x438 icon ring: %u -> %u (%+d) | cap=%u head=%u data=%p%s",
                            first?cnt:s_prev, cnt, first?0:((int)cnt-(int)s_prev), cap, head, (void*)data,
                            (!first && cnt > s_prev) ? "  <-- GREW" : "");
                    s_prev = cnt;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){}
}

// ONE-SHOT ANATOMY DUMP of the placement struct, from whichever path captured it first.
// Both the single-click hook (0x7F7EB0) and the factory hook (0x45EB50, which BOTH placement paths funnel
// through) can be first, and the drag path captures SILENTLY when the mod is already passively armed - so
// this must live in a helper both call, or a drag-first session logs nothing and we are blind.
//
// Purpose: the struct is a caller stack local, so it can never be read passively and the only way to drop
// the "place one block first" requirement is to CONSTRUCT one. Disassembly of 0x7F7EB0 shows it reads
// [r9+0x10] and immediately takes [that+0x147c] - and 0x147c sits in the same range as editor fields we
// already use (0x1440 ghost cell, 0x14a0 rotation, 0x1588 arg3). If [r9+0x10] IS the editor, we already
// hold everything needed to build the struct ourselves.
static void dump_struct_anatomy(const BYTE* st, unsigned long long editor, unsigned long long arg3,
                                unsigned long long addr, const char* via) {
    static long s_dumped = 0;
    if (InterlockedExchange(&s_dumped, 1)) return;
    unsigned long long p10=0; memcpy(&p10, st+0x10, 8);
    logline("[tmpl] --- placement struct anatomy (captured via %s, struct at %p) ---", via, (void*)addr);
    logline("[tmpl] +0x10 = %p   editor = %p   -> %s", (void*)p10, (void*)editor,
            (p10==editor) ? "*** IDENTICAL: it IS the editor - struct is CONSTRUCTIBLE ***"
                          : "NOT the editor (comparisons below)");
    if (p10 != editor) {
        logline("[tmpl] +0x10 - editor = %lld ; +0x10 - arg3 = %lld ; +0x10 - base = 0x%llX",
                (long long)(p10-editor), (long long)(p10-arg3), (unsigned long long)(p10 - g_base));
        unsigned long long veh=0,gob=0;
        safe_copy(&veh,(void*)(editor+0x13C8),8); safe_copy(&gob,(void*)(editor+0x70),8);
        logline("[tmpl] (vehicle=%p gobj=%p for comparison)", (void*)veh, (void*)gob);
    }
    unsigned v=0;
    if (safe_copy(&v,(void*)(p10+0x147c),4)) logline("[tmpl] [+0x10]+0x147c = %u (0x%X) -> factory 0x45EB50 arg3", v, v);
    if (safe_copy(&v,(void*)(editor+0x147c),4)) logline("[tmpl] editor+0x147c    = %u (0x%X)", v, v);
    for (int r=0; r<8; r++) {
        const BYTE* b = st + r*16;
        logline("[tmpl] struct+0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                r*16, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7], b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    }
    // WHERE DOES THAT POINTER COME FROM? The struct is a stack local, so to construct one ourselves we must
    // be able to source +0x10 from an object we already hold at apply time: the editor, its vehicle, or the
    // game object. Hunt the pointer value in each. A hit at a fixed offset means the struct is constructible
    // and the "place one block first" requirement dies for good; no hit means it is minted per placement and
    // we need the factory path instead.
    // Do this for EVERY pointer-shaped field, not just +0x10: to construct the struct we need a source for
    // each one. Fields that resolve to a fixed editor offset are constructible; fields that resolve nowhere
    // are either minted per placement or never dereferenced at all. (We already have strong evidence for the
    // latter case: the mod has always forged from a CAPTURED template, so the stack pointers at +0x30/+0x68
    // have been stale across thousands of successful placements - the place path cannot be reading them.)
    {
        struct Rgn { const char* name; unsigned long long base; unsigned len; };
        unsigned long long veh=0, gob=0;
        safe_copy(&veh,(void*)(editor+0x13C8),8); safe_copy(&gob,(void*)(editor+0x70),8);
        Rgn rgns[] = { { "editor", editor, 0x8000 }, { "vehicle", veh, 0x4000 }, { "gobj", gob, 0x40000 } };
        for (int f=0; f<16; f++) {
            unsigned long long pv=0; memcpy(&pv, st + f*8, 8);
            if (pv < 0x10000ULL || pv > 0x7FFFFFFFFFFFULL) continue;      // not pointer-shaped
            bool stackish = (pv >> 40) == 0;   // observed stack addrs are ~0x8E_xxxxxxxx, heap ~0x25C_xxxxxxxxx
            int hits = 0;
            for (int ri=0; ri<3 && hits<4; ri++) {
                if (!rgns[ri].base) continue;
                for (unsigned o=0; o+8<=rgns[ri].len && hits<4; o+=8) {
                    unsigned long long v=0;
                    if (!safe_copy(&v, (void*)(rgns[ri].base+o), 8) || v != pv) continue;
                    logline("[tmpl] SOURCE struct+0x%02X = %p  found at %s+0x%X", f*8, (void*)pv, rgns[ri].name, o);
                    hits++;
                }
            }
            if (!hits) logline("[tmpl] SOURCE struct+0x%02X = %p  NOT reachable%s", f*8, (void*)pv,
                               stackish ? " (stack address - stale in our captured template too, so unused by the place path)" : "");
        }
    }
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
        g_flush_pending = 1;                              // any places held while unarmed can now be forged
        // LEARN where that struct lives. If r9 sits inside the editor object it is a persistent scratch field,
        // so every later session can read the template with no placement at all.
        if (!g_tmpl_off && g_cap_r9 > editor && (g_cap_r9 - editor) < 0x20000ULL) {
            g_tmpl_off = (unsigned)(g_cap_r9 - editor);
            logline("[tmpl] placement struct lives at editor+0x%X - learned; future sessions capture it passively", g_tmpl_off);
            FILE* tf=nullptr; if(!fopen_s(&tf,g_tmploffp,"w") && tf){ fprintf(tf,"%u\n",g_tmpl_off); fclose(tf); }
        } else if (!g_tmpl_off) {
            logline("[tmpl] placement struct at %p is OUTSIDE the editor (%p, delta=%lld) - transient, cannot be read passively",
                    (void*)g_cap_r9, (void*)editor, (long long)(g_cap_r9 - editor));
            g_tmpl_off = 0xFFFFFFFFu;   // mark "checked, not viable" so we stop retesting
        }
        dump_struct_anatomy(st, editor, arg3, g_cap_r9, "single-click 0x7F7EB0");
        diff_place_struct(st, editor);   // observe-only: is a CONSTRUCTED struct identical?
    }
    // (2) click-DRAG: editor from 0x7F3440, struct from the factory 0x45EB50. arg3 = editor+0x1588 (confirmed).
    // Validate the struct's +0x10 helper pointer before trusting the captured struct.
    if (g_da_arm_needed && g_da_editor && g_da_flag) {
        unsigned long long O=0; memcpy(&O, g_da_struct + 0x10, 8);
        if (O > 0x10000ULL && O < 0x7FFFFFFFFFFFULL) {
            memcpy(g_struct, g_da_struct, 0x80); g_have_struct = 1;   // capture the forge template
            g_flush_pending = 1;
            // This branch used to capture SILENTLY whenever passive auto-arm had already set g_armed, which
            // made a drag-first session look like nothing had happened at all. Always say so.
            logline("[tmpl] forge template captured via the factory 0x45EB50 (drag//click path)");
            dump_struct_anatomy(g_da_struct, g_da_editor, g_da_editor + 0x1588, g_da_ptr, "factory 0x45EB50");
            diff_place_struct(g_da_struct, g_da_editor);
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
            // The body this block actually joined. The factory stamps it at comp+0x80 (0x45F650); fall
            // back to the parent Body* at comp+0x28 if that read fails. This is the ONLY way the receiver
            // can know which body to attach to - it has no ray of its own to pick with.
            uint32_t buid = 0;
            if (compptr) {
                if (!safe_copy(&buid, (void*)(compptr + 0x80), 4)) buid = 0;
                if (!buid) { unsigned long long pb = 0;
                    if (safe_copy(&pb, (void*)(compptr + 0x28), 8) && pb) safe_copy(&buid, (void*)(pb + BODY_UID), 4); }
            }
            emit_place(name, nl, xyz, rot, aux, color, (unsigned char)(cat & 0xFF), buid);
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
            // Accept, but do NOT call this a live link. AcceptSessionWithUser registers OUR willingness and
            // returns true whether or not anybody is on the other end - it does not require the peer to be
            // online, running Stormworks, or even to still have the mod installed. Treating it as proof of a
            // partner is what let a stale pinned id show "connected" indefinitely to somebody who was not
            // there, while every send queued into the void. A link is only real once something ARRIVES.
            if (p_accept) p_accept(g_net, g_peerIdent);
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
                  if(safe_copy(&mg,data,4) && mg==MAGIC && safe_copy(&kd,(BYTE*)data+5,1) && (kd==8||kd==9||kd==12||kd==13||kd==14||kd==15||kd==16)) {
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
                              { float cf[3]; memcpy(cf,&pm.rot[0],12);
                                if (cf[0]||cf[1]||cf[2]) {
                                    if (g_peer_bench_ctr[0]!=cf[0]||g_peer_bench_ctr[1]!=cf[1]||g_peer_bench_ctr[2]!=cf[2]) szchg=true;
                                    g_peer_bench_ctr[0]=cf[0]; g_peer_bench_ctr[1]=cf[1]; g_peer_bench_ctr[2]=cf[2];
                                } }
                              if(was!=nowv) logline("[presence] partner %s the workbench", nowv?"ENTERED":"LEFT");
                              if(szchg && pm.x){   // compare benches: differing volumes = blocks that fit one but not the other
                                  int mine[3]; bench_size_vox(mine);
                                  if(mine[0] && (mine[0]!=pm.x||mine[1]!=pm.y||mine[2]!=pm.z))
                                      logline("[presence] !! BENCH MISMATCH - yours %dx%dx%d vox, partner %dx%dx%d. Blocks outside the smaller box will not place. Use the same bench type.",
                                              mine[0],mine[1],mine[2], pm.x,pm.y,pm.z);
                                  else if(mine[0] && bench_mismatch())
                                      logline("[presence] !! DIFFERENT BENCH - same size (%dx%dx%d) but a different one. Partner centre (%.1f,%.1f,%.1f). You must be at the SAME workbench.",
                                              mine[0],mine[1],mine[2], g_peer_bench_ctr[0],g_peer_bench_ctr[1],g_peer_bench_ctr[2]);
                                  else if(mine[0])
                                      logline("[presence] bench match: both %dx%dx%d vox", mine[0],mine[1],mine[2]);
                              }
                          }
                      }
                      else if(kd==15){   // move tool: the partner slid the whole craft
                          PlaceMsg pm;
                          if(sz>=(int)sizeof(PlaceMsg) && safe_copy(&pm,data,sizeof pm) && g_sync_enabled){
                              float wp[3]; memcpy(wp,&pm.rot[0],12);
                              queue_move(wp);      // MAIN thread - writes the live vehicle transform
                          }
                      }
                      else if(kd==16){
                          // PROPERTY BLOB. Must be handled HERE, not by the generic path below: that path
                          // copies into a 256-byte stack buffer, and a property blob is up to 128 KB. It was
                          // silently truncated and then dropped by pc_net_prop's length guard - i.e. property
                          // sync was a complete no-op over the wire past ~256 bytes, while local echo (which
                          // calls handle_place_msg directly) worked perfectly. Exactly the class of bug a
                          // one-machine harness cannot see.
                          BYTE* pb=(BYTE*)VirtualAlloc(nullptr,(SIZE_T)sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
                          if(pb){ if(safe_copy(pb,data,sz)) pc_net_prop(pb,sz); VirtualFree(pb,0,MEM_RELEASE); }
                          else logline("<<< prop-blob DROPPED: could not allocate %d bytes", sz);
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
      p_setrp  = (setRP_t)      GetProcAddress(s, "SteamAPI_ISteamFriends_SetRichPresence");
      p_getfrp = (getFriendRP_t)GetProcAddress(s, "SteamAPI_ISteamFriends_GetFriendRichPresence");
      logline("auto-connect: friends=%p count=%p byidx=%p game=%p", g_friends,(void*)p_fcount,(void*)p_fbyidx,(void*)p_fgame); }
    if (!pNetAcc||!p_send||!p_recv||!p_accept||!p_release||!pClear||!pSetID||!pUser||!pGetID) { logline("missing steam exports"); return false; }
    g_net = pNetAcc();
    // Under an ASI loader we may run before SteamAPI_Init, in which case the accessor hands back null.
    // Report failure so the caller can retry rather than spawning workers around a null interface.
    if (!g_net) { logline("steam: networking interface not ready yet"); return false; }
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
    // AUTO-CONNECT IS NOW ON BY DEFAULT. It was opt-in because it was unproven and had once paired with an
    // arbitrary friend in the session; both causes are fixed (the roster path now requires the `swcoop`
    // rich-presence beacon, FINDINGS 22.1) and rich presence is confirmed working cross-machine. Requiring
    // an empty file to enable the good path was friction guarding a bug that no longer exists.
    // The privacy properties that made it opt-in still hold, and are worth restating: a stranger's SteamID
    // is never logged, adopted, written to disk or messaged - only a Steam FRIEND who is ALSO advertising
    // the mod's own beacon is ever a candidate. Create coop-autoconnect-off.txt beside the mod to disable.
    { char ap[MAX_PATH]; DWORD an=GetModuleFileNameA(g_hmod,ap,MAX_PATH);
      InterlockedExchange(&g_autoconnect_on, 1);
      if (an && an<MAX_PATH) { char* s2=strrchr(ap,'\\'); if(s2)*(s2+1)=0;
          strncat_s(ap,MAX_PATH,"coop-autoconnect-off.txt",_TRUNCATE);
          if (GetFileAttributesA(ap)!=INVALID_FILE_ATTRIBUTES) { InterlockedExchange(&g_autoconnect_on,0);
              logline("auto-connect: DISABLED (coop-autoconnect-off.txt present) - set coop-peer.txt to pair"); }
          else logline("auto-connect: on - pairing with a Steam friend who is also running the mod");
          { char* s3=strrchr(ap,'\\'); if(s3)*(s3+1)=0; }   // back to the directory for the probe check
          strncat_s(ap,MAX_PATH,"coop-probe.txt",_TRUNCATE);
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
// EXPECTED PROLOGUES, captured from the shipped build. install_hook splices a 14-byte absolute jump over a
// FIXED steal length; if a game update moves these functions that splice lands in the middle of unrelated
// code and the process dies. Injection made that a non-event - the mod does not work and you move on.
// AUTO-LOAD makes it "the game crashes on launch, every launch, until you find and delete the .asi", which
// is a far worse failure for something people install and forget. So verify before writing: a mismatch
// disables that hook instead of corrupting the game.
struct HookSig { ULONGLONG rva; int steal; unsigned char bytes[20]; };
static const HookSig g_hooksigs[] = {
    { 0x7F7EB0, 16, { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 } },
    { 0x4BFE50, 15, { 0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x48,0x08 } },
    { 0x4C0940, 15, { 0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x50,0x10 } },
    { 0x804300, 15, { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20 } },
    { 0x7F3440, 15, { 0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 } },
    { 0x45EB50, 15, { 0x48,0x89,0x5C,0x24,0x10,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8B,0xEC } },
    { 0x8B70C0, 16, { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x8B,0x49,0x10,0x44,0x8B,0x43,0x08 } },
    { 0x847EE0, 18, { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xE1 } },
    { 0x789070, 15, { 0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x50,0x10 } },
};
static volatile long g_hook_refused = 0;   // nonzero = a hook declined; the game is probably patched

// Only the HARDCODED offsets are checked. Signature-scanned functions legitimately move, so a mismatch
// there means nothing; an address we hold no record for is allowed through unchanged.
static bool prologue_ok(ULONGLONG fn_addr, int steal) {
    ULONGLONG rva = fn_addr - g_base;
    for (unsigned i = 0; i < sizeof g_hooksigs / sizeof g_hooksigs[0]; i++) {
        if (g_hooksigs[i].rva != rva) continue;
        unsigned char cur[20];
        if (!safe_copy(cur, (void*)fn_addr, steal)) {
            logline("!! hook +0x%llX: prologue UNREADABLE - refusing to hook", rva); return false; }
        if (memcmp(cur, g_hooksigs[i].bytes, steal) == 0) return true;
        char a[64]={0}, b[64]={0}, t[8];
        for (int k=0;k<6;k++){ _snprintf_s(t,sizeof t,_TRUNCATE,"%02X ",g_hooksigs[i].bytes[k]); strncat_s(a,sizeof a,t,_TRUNCATE);
                               _snprintf_s(t,sizeof t,_TRUNCATE,"%02X ",cur[k]);                  strncat_s(b,sizeof b,t,_TRUNCATE); }
        logline("!! hook +0x%llX: PROLOGUE CHANGED - expected %s... got %s... GAME UPDATED? refusing to hook "
                "(the mod will not work; the game will still run)", rva, a, b);
        InterlockedIncrement(&g_hook_refused);
        return false;
    }
    return true;
}

static ULONGLONG install_hook(ULONGLONG fn_addr, int steal, void* detour, unsigned char* orig_save) {
    if (!prologue_ok(fn_addr, steal)) return 0;
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
        wsdraw_boot_step("workbench prompt patched", 1);
        // ONE-SHOT: dump the neighbouring localisation entries. A bench that already CONTAINS a vehicle
        // shows a different prompt ("Edit Vehicle" or similar), which is a different id we never patch - so
        // the prompt legitimately looks unchanged there while id 0x623 is patched perfectly. That would
        // explain a long chase in which the entry verified correct every frame. Find the sibling rather than
        // guess its id; the table is populated at runtime, so it cannot be read from the static image.
        { static bool s_dumped = false;
          if (!s_dumped) { s_dumped = true;
            for (unsigned id = LOC_CREATE_VEHICLE - 10; id <= LOC_CREATE_VEHICLE + 12; id++) {
                char* ee = (char*)(g_base + LOC_TABLE + (ULONGLONG)id * LOC_STRIDE);
                char* dflt = nullptr; char* livep = nullptr; unsigned l = 0;
                if (!safe_copy(&dflt, ee + 0x00, 8)) continue;
                safe_copy(&livep, ee + 0x08, 8); safe_copy(&l, ee + 0x10, 4);
                char a[64] = {0}, b[64] = {0};
                if (dflt)  safe_copy(a, dflt, 48);
                if (livep) safe_copy(b, livep, 48);
                if (!a[0] && !b[0]) continue;
                logline("[loc] 0x%X: default=\"%.44s\" live=\"%.44s\" len=%u%s",
                        id, a, b, l, id == LOC_CREATE_VEHICLE ? "   <-- the one we patch" : "");
            } } }
    } __except(EXCEPTION_EXECUTE_HANDLER){ logline("[prompt] patch faulted - prompt unchanged"); }
}
// Re-point the prompt as the partner's state changes: nobody paired -> plain CO-OP; paired but not building
// -> START CO-OP; partner already in their bench -> JOIN PARTNER. Length is already 0 from the initial patch,
// so this is a single aligned pointer store - the reader either sees the old or the new string, never a tear.
static void update_workbench_prompt() {
    if (!g_loc_patched) return;
    const char* want = g_peer_in_bench ? PROMPT_JOIN : (g_peerid ? PROMPT_START : PROMPT_NOPEER);
    __try {
        char* e = (char*)(g_base + LOC_TABLE + (ULONGLONG)LOC_CREATE_VEHICLE * LOC_STRIDE);
        // VERIFY, do not assume. This used to cache the DESIRED string and skip the write when it had not
        // changed - which silently lost the patch whenever the GAME rewrote the entry. Under an ASI load we
        // patch during the menu, then the world loads and repopulates the localisation table, so the prompt
        // reverted to "Create Vehicle" and only came back on entering the bench (which changes `want` and
        // forced a rewrite). Reading the live pointer costs one load per frame and cannot drift.
        const char* live = *(const char**)(e + 0x00);
        // Low-rate proof of what the loc entry actually holds. The prompt showed stock text in-world while
        // no "game rewrote" line appeared, which means the entry is still OURS and the game must compose and
        // CACHE the prompt on the workbench object rather than reading the table each frame. Log it plainly
        // so that is established rather than inferred - the two causes need completely different fixes.
        { static DWORD s_last = 0; DWORD nowv = GetTickCount();
          if (nowv - s_last > 5000) { s_last = nowv;
              unsigned lv = *(unsigned*)(e + 0x10);
              logline("[prompt] loc check: entry=%p len=%u | ours=%s | want=%p",
                      (void*)live, lv,
                      (live==PROMPT_NOPEER||live==PROMPT_START||live==PROMPT_JOIN) ? "YES" : "NO (game owns it)",
                      (void*)want); } }
        // The LENGTH matters as much as the pointer. The reader uses +0x00 only when length==0, otherwise it
        // follows the live buffer at +0x08 - so a non-zero length means our string is ignored even though
        // the pointer is still ours. Checking only the pointer is why this looked correct in the log while
        // the bench showed stock text: entry=ours, len=39, and we returned early every frame. The world load
        // repopulates the entry and restores the length; zeroing it once at patch time is not enough.
        unsigned livelen = *(unsigned*)(e + 0x10);
        // ALSO WRITE THE LIVE BUFFER at +0x08. Pointing +0x00 at our string and zeroing the length is
        // correct but LATE: the game composes and caches the workbench prompt during world load, before our
        // per-frame re-zero runs, which is why the entry reads perfectly (ours=YES, len=0) while the bench
        // still shows stock text - and why entering and exiting the bench fixed it, by forcing a rebuild.
        // Writing our text into the buffer the game itself owns means whichever field it reads, and whenever
        // it composes, it gets ours. Bounded by the length it reported, so we never overrun its allocation.
        { char* livebuf = *(char**)(e + 0x08);
          if (livebuf && livelen >= 8) {
              char cur[80] = {0};
              if (safe_copy(cur, livebuf, livelen < 79 ? livelen : 79)) {
                  size_t wl = strlen(want);
                  if (strncmp(cur, want, wl) != 0 && wl <= livelen) {
                      static DWORD s_lb = 0; DWORD nb = GetTickCount();
                      if (nb - s_lb > 3000) { s_lb = nb;
                          logline("[prompt] live buffer held \"%.40s\" (cap %u) - writing ours in place", cur, livelen); }
                      safe_copy(livebuf, want, (unsigned)wl);
                      if (wl < livelen) { char pad[80]; memset(pad, ' ', sizeof pad);
                                          safe_copy(livebuf + wl, pad, (unsigned)(livelen - wl)); }
                  }
              }
          } }
        if (live == want && livelen == 0) return;
        if (livelen != 0) {
            static DWORD s_lw = 0; DWORD nw = GetTickCount();
            if (nw - s_lw > 3000) { s_lw = nw;
                logline("[prompt] length was %u (game repopulated it) - re-zeroing so our string is read", livelen); }
        }
        bool ours = (live == PROMPT_NOPEER || live == PROMPT_START || live == PROMPT_JOIN);
        *(unsigned*)(e + 0x10) = 0;
        MemoryBarrier();
        *(const char**)(e + 0x00) = want;
        if (!ours) logline("[prompt] the game rewrote the loc entry (world load?) - re-applied \"%s\"", want);
        else       logline("[prompt] -> \"%s\"", want);
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

// ======================= CRASH REPORTER =======================
// A crash in this mod used to leave nothing behind. The log ends mid-frame and the cause has to be inferred
// from arithmetic in the surrounding lines - which is exactly how the property-sync crash was diagnosed, and
// it should not have had to be. Worse, that is the ONLY diagnostic anyone else could ever send: the mod runs
// on other people's machines, where there is no debugger and no one to ask.
//
// So: catch the unhandled exception, and write down the three things that actually identify a crash.
//   1. WHOSE CODE FAULTED. The faulting address resolved to module+RVA. "coopworkbench.asi+0x1234" means us;
//      "stormworks64.exe+0x4C6160" means the game faulted on something we handed it. That single line decides
//      where to look and is impossible to recover afterwards.
//   2. WHAT WE WERE DOING. A breadcrumb string set at each risky phase. A stack trace without symbols for the
//      game is nearly useless; "psync: loading spliced craft" is not.
//   3. WHO WAS ON THE STACK. No dbghelp dependency and no symbols - just scan the stack for qwords that land
//      inside a loaded module's code and print them as module+RVA. swdis.py disassembles those directly.
//
// The filter chains to whatever was installed before it, so the game's own crash handling still runs. The
// report is written before chaining, because there is no guarantee of getting control back.

static char  g_crashp[MAX_PATH] = "";
static volatile char g_crumb[128] = "starting up";     // what the mod is currently doing
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// Set at the entry to anything that could plausibly fault. Cheap - a string copy, no allocation, no lock.
static void crumb(const char* what) {
    strncpy_s((char*)g_crumb, sizeof g_crumb, what ? what : "?", _TRUNCATE);
}

// Resolve an address to "module+0xRVA". Returns false if it is not inside any loaded module - which is itself
// diagnostic: a fault at an address belonging to no module means a corrupted function pointer or a jump into
// freed memory.
static bool addr_to_module(void* addr, char* out, size_t cap) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &h) || !h) return false;
    char path[MAX_PATH]; if (!GetModuleFileNameA(h, path, MAX_PATH)) return false;
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX", base,
                (unsigned long long)((unsigned char*)addr - (unsigned char*)h));
    return true;
}

static const char* exception_name(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case 0xE06D7363:                      return "C++ exception";
        default:                              return "unknown";
    }
}

static LONG WINAPI coop_crash_filter(EXCEPTION_POINTERS* ep) {
    // Re-entrancy guard: a fault inside the handler must not recurse into it.
    static volatile long s_in = 0;
    if (InterlockedExchange(&s_in, 1)) return EXCEPTION_CONTINUE_SEARCH;

    FILE* f = nullptr;
    if (!fopen_s(&f, g_crashp, "w") && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "Coop Workbench crash report\n");
        fprintf(f, "===========================\n");
        fprintf(f, "version   : %s  build %s\n", COOP_VERSION, __TIME__);
        fprintf(f, "when      : %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "doing     : %s\n", (const char*)g_crumb);

        EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        CONTEXT* cx = ep ? ep->ContextRecord : nullptr;
        if (er) {
            fprintf(f, "exception : 0x%08X (%s)\n", er->ExceptionCode, exception_name(er->ExceptionCode));
            char mod[MAX_PATH + 32];
            if (addr_to_module(er->ExceptionAddress, mod, sizeof mod))
                fprintf(f, "faulted at: %s   [%p]\n", mod, er->ExceptionAddress);
            else
                fprintf(f, "faulted at: %p  - NOT INSIDE ANY LOADED MODULE (corrupt function pointer?)\n",
                        er->ExceptionAddress);
            // For an access violation the operation and the target address are the whole story.
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                const ULONG_PTR op = er->ExceptionInformation[0];
                fprintf(f, "access    : %s address %p%s\n",
                        op == 0 ? "READ from" : (op == 1 ? "WRITE to" : "EXECUTE at"),
                        (void*)er->ExceptionInformation[1],
                        er->ExceptionInformation[1] < 0x10000 ? "   (near-null - a null pointer, not corruption)" : "");
            }
        }

        // Is it OUR fault? The first question anyone asks, and the answer is a two-line check.
        if (er && g_hmod) {
            unsigned char* a = (unsigned char*)er->ExceptionAddress;
            unsigned char* mb = (unsigned char*)g_hmod;
            // 8 MB is comfortably past the end of this DLL; exact size needs psapi, and this is only a hint.
            fprintf(f, "in mod?   : %s\n", (a >= mb && a < mb + 0x800000) ? "YES - the fault is inside coopworkbench"
                                                                         : "no - the fault is in the game or another module");
        }
        fprintf(f, "mod state : armed=%d in_bench=%d suppress=%llu sync=%ld peer=%s\n",
                g_armed ? 1 : 0, g_in_bench ? 1 : 0, (unsigned long long)g_suppress,
                g_sync_enabled, g_peerid ? "paired" : "none");

        if (cx) {
            fprintf(f, "\nregisters\n---------\n");
            fprintf(f, "RIP=%016llX RSP=%016llX RBP=%016llX\n", cx->Rip, cx->Rsp, cx->Rbp);
            fprintf(f, "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n", cx->Rax, cx->Rbx, cx->Rcx, cx->Rdx);
            fprintf(f, "RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\n", cx->Rsi, cx->Rdi, cx->R8,  cx->R9);
            fprintf(f, "R10=%016llX R11=%016llX R12=%016llX R13=%016llX\n", cx->R10, cx->R11, cx->R12, cx->R13);
            fprintf(f, "R14=%016llX R15=%016llX\n", cx->R14, cx->R15);

            // Stack scan. Not a real unwind - it prints every stack slot that happens to point into loaded
            // code, so it includes stale frames. That is fine and even useful: the true return addresses are
            // in there, and with no symbols for the game an over-inclusive list beats an empty one.
            fprintf(f, "\nstack (addresses pointing into loaded code, innermost first)\n"
                       "-----------------------------------------------------------\n");
            unsigned long long* sp = (unsigned long long*)cx->Rsp;
            int printed = 0;
            for (int i = 0; i < 2048 && printed < 48; i++) {
                unsigned long long v = 0;
                __try { v = sp[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                if (v < 0x10000) continue;
                char mod[MAX_PATH + 32];
                if (!addr_to_module((void*)v, mod, sizeof mod)) continue;
                fprintf(f, "  [rsp+0x%04X]  %s\n", i * 8, mod);
                printed++;
            }
            if (!printed) fprintf(f, "  (nothing resolvable - stack may be corrupt)\n");
        }

        // The last log lines, so a report is self-contained and a user only has to send one file.
        fprintf(f, "\nlast log lines\n--------------\n");
        FILE* lf = nullptr;
        if (!fopen_s(&lf, LOGP, "rb") && lf) {
            fseek(lf, 0, SEEK_END);
            long sz = ftell(lf);
            long want = sz < 8000 ? sz : 8000;
            fseek(lf, sz - want, SEEK_SET);
            static char tail[8192];
            size_t got = fread(tail, 1, (size_t)want, lf);
            fclose(lf);
            tail[got] = 0;
            // Skip a partial first line so the report never opens mid-sentence.
            char* start = (char*)tail;
            if (want < sz) { char* nl = strchr(start, '\n'); if (nl) start = nl + 1; }
            fputs(start, f);
        } else fprintf(f, "  (log unavailable)\n");

        fprintf(f, "\n--\nPlease attach this file to a bug report. It contains no personal information:\n"
                   "no Steam IDs, no user name, no file paths outside the game folder.\n");
        fclose(f);
    }

    logline("*** CRASH - report written to %s ***", g_crashp);
    InterlockedExchange(&s_in, 0);
    // Chain, so the game's own handler (and Windows Error Reporting) still get their turn.
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// A crash file left over from LAST launch is the one a user would otherwise never notice. Surface it: in the
// log, and on the startup overlay, where it is visible without opening anything.
static void crash_report_init() {
    _snprintf_s(g_crashp, MAX_PATH, _TRUNCATE, "%s", LOGP);
    char* dot = strrchr(g_crashp, '\\');
    if (dot) _snprintf_s(dot + 1, MAX_PATH - (dot + 1 - g_crashp), _TRUNCATE, "coopworkbench-CRASH.txt");

    FILE* old = nullptr;
    if (!fopen_s(&old, g_crashp, "r") && old) {
        char line[256] = {0}, doing[256] = {0};
        // Pull the "doing:" breadcrumb out of the previous report so the log says what crashed, not just that
        // something did.
        while (fgets(line, sizeof line, old)) {
            if (strncmp(line, "doing     : ", 12) == 0) { strncpy_s(doing, sizeof doing, line + 12, _TRUNCATE); break; }
        }
        fclose(old);
        char* nl = strchr(doing, '\n'); if (nl) *nl = 0;
        logline("!!! The previous session CRASHED%s%s", doing[0] ? " while: " : "", doing);
        logline("!!! Report: %s  - please attach it to a bug report.", g_crashp);
        wsdraw_boot_step("previous session crashed - see log", 2);
    }
    g_prev_filter = SetUnhandledExceptionFilter(coop_crash_filter);
}

// There is exactly ONE top-level exception filter per process, and installing one REPLACES the previous.
// We load before the game's own code runs, so anything the game installs afterwards would silently displace
// us and the reporter would never fire - the failure mode being an empty crash file, i.e. indistinguishable
// from no crash reporter at all. Re-assert ours periodically and re-chain to whatever displaced it, so both
// handlers keep running whichever order they were installed in.
static void crash_filter_keepalive() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(coop_crash_filter);
    if (prev != coop_crash_filter) g_prev_filter = prev;
}

static DWORD WINAPI setup(LPVOID) {
    init_paths();
    // AFTER init_paths, not before: the crash path is derived from LOGP, and until init_paths runs LOGP is
    // still the bare relative filename. Deriving from that gave a crash path with no directory separator,
    // which (a) reported a phantom "previous session crashed" on every launch by finding the LOG file where
    // it looked for a report, and (b) would have written a real report into the game root - or over the log
    // it embeds. Ordering is the fix; the guard below makes it impossible to regress silently.
    crash_report_init();
    InitializeCriticalSection(&g_pcs); g_pcs_init=true;   // paint-cache lock
    InitializeCriticalSection(&g_pp_cs); g_pp_cs_ok = true;   // pending property updates
    selftest_init();
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
    // RETRY UNTIL STEAM IS MAPPED. Injected into a running game this succeeds first time. Loaded by an ASI
    // loader we start WITH the game, and if steam_api64.dll is not mapped at this instant the hook silently
    // never installs - and this IAT slot is the mod's ONLY per-frame main-thread tick. Losing it loses
    // passive auto-arm, g_in_bench, the F7 apply, E/Q watching, cursor/camera sampling, move sync, and the
    // workbench-prompt retry that itself lives in my_runcb. A permanent, near-silent failure, so wait.
    for (int i = 0; i < 300 && !sm; i++) {
        Sleep(100);
        sm = GetModuleHandleA("steam_api64.dll");
        if (sm) { realRunCb = (void*)GetProcAddress(sm, "SteamAPI_RunCallbacks");
                  logline("  steam_api64.dll appeared after %d ms - proceeding with the IAT hook", (i+1)*100); }
    }
    if (!sm) logline("  steam_api64.dll never appeared (30s) - main-thread apply will fall back to the worker");
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
    patch_workbench_prompt();                // "[E] CO-OP  [Q] SOLO" on the workbench prompt (retried below)
    DeleteFileA(CMDP);                       // clear any stale command
    spawn(detect_worker);                     // handles both arm + detect+emit
    spawn(del_worker);                        // delete detection
    // hot-unload watcher: launched OUTSIDE g_threads on purpose. It is the thread that runs the
    // unload's WaitForMultipleObjects(g_threads, bWaitAll), so it must NOT be in that set - a thread
    // waiting on its own never-signaled handle would spin the "never free on timeout" loop forever.
    CreateThread(nullptr, 0, cmd_watcher, nullptr, 0, nullptr);
    // Manual injection has Steam ready immediately and this succeeds first time. Loaded by an ASI loader we
    // start WITH the game, before steam_api64.dll is even mapped, so retry for up to ~60s instead of giving
    // up permanently (the original single attempt would have left networking dead for every ASI user).
    for (int i = 0; i < 600 && g_running; i++) {
        if (steam_init()) break;
        if (i == 0) logline("steam not ready yet (normal when loaded at game start) - retrying...");
        Sleep(100);
    }
    logline("Coop Workbench by BayneBuild " COOP_VERSION " EXPERIMENTAL (built " __DATE__ " " __TIME__ ")");
    // Boot steps for the on-screen sequence. The overlay starts after this, and the panel holds until a few
    // seconds after the LAST step, so steps recorded before it draws are still shown.
    wsdraw_boot_step(g_hook_refused ? "hooks REFUSED - game version mismatch" : "hooks installed", g_hook_refused ? 2 : 1);
    wsdraw_boot_step(g_iat_hooked ? "main-thread apply ready" : "main-thread hook unavailable", g_iat_hooked ? 1 : 2);
    wsdraw_boot_step(g_net ? "steam p2p ready" : "steam not ready", g_net ? 1 : 2);
    wsdraw_boot_step(g_peerid ? "partner paired" : "no partner yet - auto-connect watching", g_peerid ? 1 : 4);
    wsdraw_boot_step("waiting for a workbench", 3);
    wsdraw_boot_step("F6 MENU    F7 PULL CRAFT    F8 LOG", 5);
    if (g_hook_refused)
        logline("!!! %ld hook(s) REFUSED - this build does not match your game version. The mod is INACTIVE "
                "but your game is unharmed. Update the mod, or delete it from plugins/ to be sure.",
                g_hook_refused);
    logline("coop v4 ready (session poll-accept + relay). base=0x%llX. hooks: arm=0x%llX detect=0x%llX. Auto-arms on your first edit (click OR drag); build to sync. peer=%llu",
            g_base, PLACE_OFF, ADD_OFF, (unsigned long long)g_peerid);
    overlay_start(g_hmod);                    // start the in-world partner-camera overlay (merged in)
    logline("overlay started (merged into coopworkbench.dll; F9=overlay F10=hud)");
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) wsdraw_log_init();   // before ANY logline, so no startup line is lost
    if (r == DLL_PROCESS_ATTACH) { g_hmod = h; DisableThreadLibraryCalls(h); CreateThread(nullptr,0,setup,nullptr,0,nullptr); }
    return TRUE;
}
