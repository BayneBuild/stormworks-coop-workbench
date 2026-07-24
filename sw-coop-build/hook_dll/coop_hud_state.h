// coop_hud_state.h - shared HUD state block for the Stormworks co-op mod.
//
// PURPOSE
//   coopworkbench.dll owns all the co-op runtime state (SteamIDs, armed flag, editor ptr, counters,
//   last error, hook status). overlay.dll draws the in-game HUD. The two are SEPARATE DLLs
//   with no link-time relationship, so the state is published through a named shared-memory
//   block instead of exported symbols. Same-process (both injected into stormworks64.exe)
//   works, and so does an out-of-process monitor .exe in the same logon session, because the
//   block is a named file mapping and is 100% SELF-CONTAINED: fixed-size PODs only, no
//   pointers into either DLL's address space, no heap, no CRT strings.
//
// PROTOCOL
//   * Named mapping "Local\SWCoopHud", fixed COOP_HUD_MAP_SIZE bytes.
//   * magic / version / struct_size are validated by the reader before it trusts anything.
//   * SEQLOCK: `seq` is even when the block is stable, odd while a writer is mid-update.
//     Reader: snap seq -> copy -> snap seq again -> accept only if both snaps are equal and
//     even. That is why the reader can never observe a torn struct even though the writer
//     runs on four different threads (detect_worker, del_worker, recv_worker, main thread
//     via the SteamAPI_RunCallbacks IAT hook). Multiple writers are serialised by a
//     process-local spinlock before they touch `seq`, so the odd/even invariant holds.
//   * Strings are fixed char arrays, always NUL-terminated by coop_hud_setstr().
//   * ev[] is a ring of the last COOP_HUD_EVENTS placements/deletes with voxel coords, the
//     definition name and a GetTickCount() stamp, so the HUD can print
//     "peer placed 01_block at (3,4,5)" and fade world-space highlights out over time.
//
// USAGE - WRITER (coop.cpp), exactly one translation unit:
//     #define COOP_HUD_WRITER
//     #include "coop_hud_state.h"
//     coop_hud_init(g_base);                                  // once, in setup()
//     COOP_HUD(H->armed = 1);                                 // any one-line field update
//     coop_hud_event(COOP_EV_REMOTE_PLACE, x, y, z, name);    // ring + counter in one call
//     coop_hud_shutdown();                                    // on hot-unload
//   Do NOT nest COOP_HUD(...) / coop_hud_event() inside each other - the writer spinlock is
//   not recursive.
//
// USAGE - READER (overlay.cpp):
//     #include "coop_hud_state.h"          // no COOP_HUD_WRITER
//     CoopHudState s;
//     if (coop_hud_read(&s)) { ... draw s.places_sent, s.ev[...] ... }
//   coop_hud_read() lazily opens the mapping and returns false while coopworkbench.dll is not loaded,
//   so the overlay can be injected first, last, or across a coopworkbench.dll hot-reload.
//
// DEPENDENCIES: <windows.h> only. Header-only. C++11 (static_assert).

#ifndef COOP_HUD_STATE_H
#define COOP_HUD_STATE_H

#include <windows.h>

// ---------------------------------------------------------------- fixed-width types
// Defined locally so the header pulls in nothing but windows.h.
typedef unsigned int       coop_u32;
typedef int                coop_i32;
typedef unsigned long long coop_u64;
static_assert(sizeof(coop_u32) == 4, "coop_u32 must be 4 bytes");
static_assert(sizeof(coop_i32) == 4, "coop_i32 must be 4 bytes");
static_assert(sizeof(coop_u64) == 8, "coop_u64 must be 8 bytes");

// ---------------------------------------------------------------- constants
#define COOP_HUD_MAP_NAME   "Local\\SWCoopHud"
#define COOP_HUD_MAP_SIZE   8192u
#define COOP_HUD_MAGIC      0x44554843u   // 'CHUD' little-endian
#define COOP_HUD_VERSION    1u
#define COOP_HUD_EVENTS     16            // ring depth (power of two)
#define COOP_HUD_NAMELEN    24            // definition name inside an event
#define COOP_HUD_STRLEN     64            // last_error / last_unknown_def
// voxel -> editor world units. coop.cpp writes the editor cursor as voxel*0.25
// (see CUR_X/CUR_Y/CUR_Z at editor+0x12F8/0x1300/0x1308), so a world-space highlight for
// event e sits at (e.x, e.y, e.z) * COOP_VOXEL_TO_WORLD in the craft's local frame.
#define COOP_VOXEL_TO_WORLD 0.25

// Kind values deliberately mirror the wire protocol's intent, not its numbering:
// PlaceMsg.kind 1=place 2=delete 3=paint, each split into a local and a remote event.
enum CoopHudEvKind {
    COOP_EV_NONE          = 0,
    COOP_EV_LOCAL_PLACE   = 1,   // we placed it, it went out on the wire
    COOP_EV_REMOTE_PLACE  = 2,   // peer placed it, we forged it in
    COOP_EV_LOCAL_DELETE  = 3,
    COOP_EV_REMOTE_DELETE = 4,
    COOP_EV_ERROR         = 5,   // name[] carries a short reason
    COOP_EV_LOCAL_PAINT   = 6,   // local repaint detected by the face-color frame-diff
    COOP_EV_REMOTE_PAINT  = 7
};

enum CoopHudHookBit {           // hooks_installed bitmask
    COOP_HOOK_PLACE  = 0x01,    // 0x7F7EB0 arm hook
    COOP_HOOK_ADD    = 0x02,    // 0x4BFE50 universal add detect
    COOP_HOOK_DEL    = 0x04,    // 0x4C0940 universal remove detect
    COOP_HOOK_DELARM = 0x08,    // 0x804300 single-delete gate
    COOP_HOOK_RUNCB  = 0x10     // SteamAPI_RunCallbacks IAT (main-thread apply)
};

// ---------------------------------------------------------------- the block
#pragma pack(push, 8)

struct CoopHudEvent {
    coop_u32 kind;                    // CoopHudEvKind
    coop_i32 x, y, z;                 // voxel coords
    coop_u32 tick;                    // GetTickCount() at record time (for fade-out)
    coop_u32 seqno;                   // 1-based monotonic id
    char     name[COOP_HUD_NAMELEN];  // definition name, always NUL-terminated
};

struct CoopHudState {
    // --- header: never reorder, the reader validates these before anything else ---
    coop_u32 magic;         // COOP_HUD_MAGIC once published; 0 while unpublished/shut down
    coop_u32 version;       // COOP_HUD_VERSION
    coop_u32 struct_size;   // sizeof(CoopHudState) as the writer compiled it
    coop_u32 seq;           // seqlock: even = stable, odd = writer inside

    // --- identity / resolved addresses (all module-absolute; subtract module_base for RVA) ---
    coop_u64 my_steamid;      // coop.cpp g_myid
    coop_u64 peer_steamid;    // coop.cpp g_peerid (0 = no coop-peer.txt)
    coop_u64 module_base;     // coop.cpp g_base
    coop_u64 editor;          // coop.cpp g_editor (0 until ARMED)
    coop_u64 arg3;            // coop.cpp g_arg3
    coop_u64 fn_place;        // signature-resolved place-cmd     (RVA 0x7F7EB0 nominal)
    coop_u64 fn_add;          // signature-resolved add funnel    (RVA 0x4BFE50 nominal)
    coop_u64 fn_gettmpl;      // signature-resolved getTemplateByName (RVA 0x46F380 nominal)
    coop_u64 tramp_place;     // nonzero => that inline hook is installed
    coop_u64 tramp_add;
    coop_u64 tramp_del;
    coop_u64 tramp_delarm;

    // --- flags ---
    coop_u32 running;         // 1 while coopworkbench.dll is live; 0 after hot-unload
    coop_u32 armed;           // g_armed - apply path bootstrapped (one local click done)
    coop_u32 session_ok;      // g_session_ok - Steam P2P session accepted / link live
    coop_u32 localecho;       // g_localecho - peer == self, round-trip test mode
    coop_u32 iat_hooked;      // g_iat_hooked - applies run on the MAIN thread
    coop_u32 suppress;        // g_suppress - currently applying a remote op (echo guard)
    coop_u32 editor_valid;    // last probe of *(editor+0x70) succeeded and was nonzero
    coop_i32 echo_dy;         // g_echo_dy - loopback +Y shift
    coop_u32 hooks_installed; // CoopHudHookBit mask
    coop_u32 writer_pid;      // GetCurrentProcessId() of the writer
    coop_u32 start_tick;      // GetTickCount() at coop_hud_init
    coop_u32 heartbeat;       // bumped by the writer's sampler; stalls => coopworkbench.dll wedged

    // --- counters ---
    coop_u32 places_sent;     // local placements emitted (wire or local-echo)
    coop_u32 places_recv;     // inbound place messages accepted off the wire
    coop_u32 places_applied;  // remote placements successfully forged in
    coop_u32 place_fail;      // apply raised an SEH exception
    coop_u32 deletes_sent;
    coop_u32 deletes_recv;
    coop_u32 deletes_applied;
    coop_u32 delete_miss;     // remote delete arrived but no component at that voxel
    coop_u32 paints_sent;     // local repaints emitted (kind=3)
    coop_u32 paints_recv;
    coop_u32 paints_applied;
    coop_u32 unknown_defs;    // name -> template resolution failures (missing mod / version)
    coop_u32 send_errors;     // p_send returned something other than k_EResultOK(1)
    coop_u32 aq_depth;        // apply-queue backlog  (g_aq_wr - g_aq_rd)
    coop_u32 ring_depth;      // detect-ring backlog  (g_ring_wr - g_ring_rd)
    coop_u32 paint_cells;     // g_npcache - voxels the paint frame-diff is watching

    // --- last error / diagnostics ---
    coop_i32 last_eresult;    // last Steam EResult (1=OK, 35=ConnectFailed, 3=NoConnection)
    coop_u32 last_send_tick;
    coop_u32 last_recv_tick;
    coop_u32 last_error_tick;
    coop_u32 last_unknown_tick;
    char     last_error[COOP_HUD_STRLEN];
    char     last_unknown_def[COOP_HUD_STRLEN];   // definition name we could not resolve

    // --- event ring: newest is ev[(ev_head - 1) % COOP_HUD_EVENTS] ---
    coop_u32 ev_head;         // total events ever written
    coop_u32 pad0;
    CoopHudEvent ev[COOP_HUD_EVENTS];
};

#pragma pack(pop)

static_assert(sizeof(CoopHudState) <= COOP_HUD_MAP_SIZE, "CoopHudState outgrew the mapping");
static_assert(sizeof(CoopHudEvent) == 48, "CoopHudEvent layout changed - bump COOP_HUD_VERSION");

// ---------------------------------------------------------------- tiny CRT-free helpers
static inline void coop_hud_rawcopy(void* d, const void* s, unsigned n) {
    unsigned char* dp = (unsigned char*)d; const unsigned char* sp = (const unsigned char*)s;
    for (unsigned i = 0; i < n; i++) dp[i] = sp[i];
}
static inline void coop_hud_rawzero(void* d, unsigned n) {
    unsigned char* dp = (unsigned char*)d;
    for (unsigned i = 0; i < n; i++) dp[i] = 0;
}
// bounded, always NUL-terminates, tolerates a null source
static inline void coop_hud_setstr(char* dst, unsigned cap, const char* src) {
    if (!cap) return;
    unsigned i = 0;
    if (src) { for (; i + 1 < cap && src[i]; i++) dst[i] = src[i]; }
    dst[i] = 0;
}
// newest-first accessor: back==0 is the most recent event, or null if that slot is empty.
static inline const CoopHudEvent* coop_hud_event_at(const CoopHudState* s, unsigned back) {
    if (!s || back >= COOP_HUD_EVENTS || s->ev_head <= back) return 0;
    return &s->ev[(s->ev_head - 1 - back) % COOP_HUD_EVENTS];
}
static inline coop_u32 coop_hud_age_ms(coop_u32 tick) { return GetTickCount() - tick; }

// ================================================================= WRITER SIDE
#ifdef COOP_HUD_WRITER

static HANDLE        g_coop_hud_map  = NULL;
static CoopHudState* g_coop_hud      = NULL;
static volatile LONG g_coop_hud_lock = 0;   // process-local; serialises the seqlock writers

// Create (or re-attach to) the block and publish it. Safe to call twice.
// If a previous coopworkbench.dll instance leaked the section (see coop_hud_shutdown), this re-opens
// the SAME named section, zeroes it and republishes - so a reader that is still mapped onto
// it survives a hot-reload without ever touching a freed view.
static inline bool coop_hud_init(coop_u64 module_base) {
    if (g_coop_hud) return true;
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, COOP_HUD_MAP_SIZE, COOP_HUD_MAP_NAME);
    if (!h) return false;
    void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!v) { CloseHandle(h); return false; }
    coop_hud_rawzero(v, COOP_HUD_MAP_SIZE);          // magic stays 0 -> readers skip it
    CoopHudState* s = (CoopHudState*)v;
    s->version     = COOP_HUD_VERSION;
    s->struct_size = (coop_u32)sizeof(CoopHudState);
    s->module_base = module_base;
    s->running     = 1;
    s->writer_pid  = GetCurrentProcessId();
    s->start_tick  = GetTickCount();
    MemoryBarrier();
    s->magic = COOP_HUD_MAGIC;                       // publish LAST
    g_coop_hud_map = h;
    g_coop_hud     = s;
    return true;
}

static inline CoopHudState* coop_hud_w_begin() {
    if (!g_coop_hud) return 0;
    while (InterlockedCompareExchange(&g_coop_hud_lock, 1, 0) != 0) YieldProcessor();
    if (!g_coop_hud) { InterlockedExchange(&g_coop_hud_lock, 0); return 0; }
    g_coop_hud->seq++;            // -> odd: writer inside
    MemoryBarrier();
    return g_coop_hud;
}
static inline void coop_hud_w_end(CoopHudState* h) {
    if (!h) return;
    MemoryBarrier();
    h->seq++;                     // -> even: stable again
    InterlockedExchange(&g_coop_hud_lock, 0);
}

// One-statement field update. Variadic so commas inside the body are fine.
//   COOP_HUD(H->armed = 1);
//   COOP_HUD(H->places_sent++; H->last_send_tick = GetTickCount());
// No-ops safely if coop_hud_init() failed or shutdown already ran.
#define COOP_HUD(...) do { CoopHudState* H = coop_hud_w_begin(); \
                           if (H) { __VA_ARGS__; } coop_hud_w_end(H); } while (0)

// Push a ring event AND bump the counter that goes with it, in one locked update.
static inline void coop_hud_event(coop_u32 kind, coop_i32 x, coop_i32 y, coop_i32 z,
                                  const char* name) {
    CoopHudState* H = coop_hud_w_begin();
    if (H) {
        CoopHudEvent* e = &H->ev[H->ev_head % COOP_HUD_EVENTS];
        e->kind = kind; e->x = x; e->y = y; e->z = z;
        e->tick = GetTickCount(); e->seqno = H->ev_head + 1;
        coop_hud_setstr(e->name, COOP_HUD_NAMELEN, name);
        H->ev_head++;
        switch (kind) {
            case COOP_EV_LOCAL_PLACE:   H->places_sent++;     H->last_send_tick  = e->tick; break;
            case COOP_EV_REMOTE_PLACE:  H->places_applied++;  H->last_recv_tick  = e->tick; break;
            case COOP_EV_LOCAL_DELETE:  H->deletes_sent++;    H->last_send_tick  = e->tick; break;
            case COOP_EV_REMOTE_DELETE: H->deletes_applied++; H->last_recv_tick  = e->tick; break;
            case COOP_EV_LOCAL_PAINT:   H->paints_sent++;     H->last_send_tick  = e->tick; break;
            case COOP_EV_REMOTE_PAINT:  H->paints_applied++;  H->last_recv_tick  = e->tick; break;
            case COOP_EV_ERROR:         H->last_error_tick = e->tick;
                                        coop_hud_setstr(H->last_error, COOP_HUD_STRLEN, name); break;
            default: break;
        }
    }
    coop_hud_w_end(H);
}

// "name -> template" resolution failed: record the offending definition name.
static inline void coop_hud_unknown_def(const char* name) {
    CoopHudState* H = coop_hud_w_begin();
    if (H) {
        H->unknown_defs++;
        H->last_unknown_tick = GetTickCount();
        coop_hud_setstr(H->last_unknown_def, COOP_HUD_STRLEN, name);
    }
    coop_hud_w_end(H);
}

// Hot-unload. Marks the block dead (magic=0 makes readers drop and re-open) and detaches.
// The view and handle are DELIBERATELY LEAKED (one 8 KB page + one handle, once per reload):
// unmapping while an overlay thread or another writer thread is mid-access would hand it a
// freed address, and the next coop_hud_init() re-opens the same named section anyway.
static inline void coop_hud_shutdown() {
    if (!g_coop_hud) return;
    while (InterlockedCompareExchange(&g_coop_hud_lock, 1, 0) != 0) YieldProcessor();
    CoopHudState* s = g_coop_hud;
    s->running = 0;
    s->magic   = 0;
    s->seq    += 2;                 // stays even
    MemoryBarrier();
    g_coop_hud = NULL;
    InterlockedExchange(&g_coop_hud_lock, 0);
    (void)s;
}

#endif // COOP_HUD_WRITER

// ================================================================= READER SIDE
static HANDLE              g_coop_hud_rmap = NULL;
static const CoopHudState* g_coop_hud_r    = NULL;

static inline void coop_hud_reader_close() {
    if (g_coop_hud_r)    { UnmapViewOfFile((void*)g_coop_hud_r); g_coop_hud_r = NULL; }
    if (g_coop_hud_rmap) { CloseHandle(g_coop_hud_rmap);         g_coop_hud_rmap = NULL; }
}
static inline bool coop_hud_reader_open() {
    if (g_coop_hud_r) return true;
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, COOP_HUD_MAP_NAME);
    if (!h) return false;                                  // coopworkbench.dll not injected (yet)
    void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!v) { CloseHandle(h); return false; }
    g_coop_hud_rmap = h;
    g_coop_hud_r    = (const CoopHudState*)v;
    return true;
}

// Seqlock snapshot. Returns false if the block is absent, mid-write for too long, dead
// (magic==0 after a hot-unload) or built from a mismatched header - never a torn struct.
// Bounded retries so it can be called straight from the SwapBuffers hook without ever
// stalling a frame on a wedged writer.
static inline bool coop_hud_read(CoopHudState* out) {
    if (!out) return false;
    if (!coop_hud_reader_open()) return false;
    const CoopHudState* src = g_coop_hud_r;
    for (int attempt = 0; attempt < 8; attempt++) {
        coop_u32 s1 = *(volatile const coop_u32*)&src->seq;
        if (s1 & 1u) { YieldProcessor(); continue; }        // writer inside
        MemoryBarrier();
        coop_hud_rawcopy(out, (const void*)src, (unsigned)sizeof(CoopHudState));
        MemoryBarrier();
        coop_u32 s2 = *(volatile const coop_u32*)&src->seq;
        if (s1 != s2) continue;                             // torn - retry
        if (out->magic != COOP_HUD_MAGIC) { coop_hud_reader_close(); return false; }
        if (out->version != COOP_HUD_VERSION ||
            out->struct_size != (coop_u32)sizeof(CoopHudState)) return false;
        return true;
    }
    return false;
}

// Frame-friendly wrapper for the SwapBuffers hook: on a contended/absent read it hands back
// the last snapshot that WAS good instead of blanking the HUD for a frame. Returns false only
// until the very first successful read. `stale_ms` (optional) reports how old the data is, so
// the overlay can grey the panel out if coopworkbench.dll has gone away.
// Measured: under a pathological 4-writer hammer ~8% of reads exhaust the retry budget; under
// the real update rate (tens per second) that is effectively never - but this makes it moot.
static inline bool coop_hud_read_cached(CoopHudState* out, coop_u32* stale_ms) {
    static CoopHudState s_last;
    static bool         s_have = false;
    static coop_u32     s_tick = 0;
    if (coop_hud_read(&s_last)) { s_have = true; s_tick = GetTickCount(); }
    if (!s_have) { if (stale_ms) *stale_ms = 0; return false; }
    if (stale_ms) *stale_ms = GetTickCount() - s_tick;
    coop_hud_rawcopy(out, &s_last, (unsigned)sizeof(CoopHudState));
    return true;
}

#endif // COOP_HUD_STATE_H
