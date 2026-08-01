// wsdraw.cpp - the in-world partner-camera OVERLAY, compiled into coopworkbench.dll.
//
// Renders custom UI inside the game's own 3D scene: it IAT-hooks SwapBuffers and draws with the
// game's OpenGL, and it pointer-swaps the cached glUniformMatrix4fv to capture the workbench MVP
// each frame. inv(P)*MVP is perfectly rigid, so that MVP maps craft-local voxel space straight to
// clip space - meaning any world point (a partner's camera, a voxel) projects to screen with no
// camera reconstruction. P decodes to fovy 69.90 deg, near 0.025, voxel scale 0.25 m.
//
// It has NO DllMain of its own: coop.cpp's DllMain drives it via overlay_start()/overlay_stop(),
// so the overlay and the block-sync mod live in one DLL and tear down together. A small Steam
// channel (separate from the block-sync channel) carries the local camera pose to the peer and
// back; the "PARTNER" marker is drawn from the peer's pose.
//
// In-game keys:  F9 = show/hide overlay,  F10 = show/hide the top-left calibration readouts.
// Live-tunable via wsdraw-cfg.txt (re-read once a second, no rebuild needed):
//     scale 0.25        metres per voxel
//     box   0 0 0       draw a calibration box at this voxel (repeatable, up to 32)
//     span  3           also draw a span x span x span lattice from the origin

#include <windows.h>
#include "coop_version.h"
#include <gl/GL.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define GL_CURRENT_PROGRAM      0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
typedef void (APIENTRY *PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (APIENTRY *PFN_glBindVertexArray)(GLuint);

static HMODULE g_self = nullptr;
static HANDLE  g_net_worker = nullptr;
static HANDLE  g_boot_worker = nullptr;
static char    g_logpath[MAX_PATH], g_cfgpath[MAX_PATH];
static void**  g_iat_swap = nullptr;
static BOOL (WINAPI *g_orig_swap)(HDC) = nullptr;
static PFN_glUniformMatrix4fv g_real_um4 = nullptr, g_next_um4 = nullptr;
static PFN_glUseProgram       p_glUseProgram = nullptr;
static PFN_glBindVertexArray  p_glBindVertexArray = nullptr;
static void**  g_slot = nullptr;
static volatile LONG g_inhook = 0, g_stop = 0, g_visible = 1, g_want_exit = 0, g_gl_freed = 0;
// The F6 panel is now OFF by default. It was permanently on screen, which is fine while
// debugging and clutter for a player - the boot sequence says how to bring it up instead.
// Camera-matrix hook unavailable. Costs the partner CAMERA marker only - the menu, sync banner and
// partner cursor do not need the view matrix and must keep drawing.
static bool g_um4_gave_up = false;
static unsigned g_frames = 0;
static bool g_ready = false, g_font_ready = false;
extern "C" volatile long g_selftest_on;   // defined in coop.cpp - local-echo is active
static char  g_toast[96] = {0};
static DWORD g_toast_at = 0;
extern "C" __declspec(dllexport) void wsdraw_toast(const char* text) {
    if (!text) return;
    strncpy_s(g_toast, sizeof g_toast, text, _TRUNCATE);
    g_toast_at = GetTickCount();
}
static GLuint g_font_base = 0; static const int FONT_H = 15;

static void logline(const char* fmt, ...) {
    FILE* f = nullptr; if (fopen_s(&f, g_logpath, "a") || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
// SEH-guarded read of possibly-bad memory (e.g. a hostile/short Steam message payload).
// Returns false instead of faulting the game, mirroring coop.cpp's safe_copy.
static bool net_safe_copy(void* dst, const void* src, int n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- config (live-reloaded) ------------------------------------------------
static float g_scale = 0.25f;
static int   g_boxes[32][3]; static int g_nboxes = 0;
static int   g_span = 3;
static float g_off[3] = {0, 0, 0};   // WORLD-metre offset applied to everything we draw
static bool  g_off_dirty = false;    // set on nudge; stops the file reload clobbering it
static int   g_grid = 1;             // draw the labelled world ground grid
static float g_gridext = 40.0f;      // half-extent of that grid, metres
static volatile LONG g_hud = 0;      // F10 toggles the top-left calibration/status readouts (off by default)
static volatile LONG g_menu = 0;     // F6 toggles the coop status menu. OFF by default: it used to sit
                                     // on screen permanently, which suits debugging and not playing.
                                     // The boot sequence tells the player how to open it.
// The F6 status panel's geometry, at file scope so the log viewer can sit UNDER it rather than over it.
// Duplicating these numbers in two places is exactly how two panels end up overlapping after someone edits
// one of them, so there is a single definition.
static const int MENU_X = 8, MENU_Y = 8, MENU_ROW = 18, MENU_W = 560;
static const int MENU_H = 6*MENU_ROW + 12;

static DWORD g_cfg_last = 0;

static void load_cfg() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_cfgpath, "r") || !f) return;
    int nb = 0; float sc = g_scale; int sp = g_span, gr = g_grid;
    float ox = g_off[0], oy = g_off[1], oz = g_off[2], ge = g_gridext;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        float v, a, b, c; int x, y, z;
        if      (sscanf_s(line, " scale %f", &v) == 1) sc = v;
        else if (sscanf_s(line, " span %d", &sp) == 1) {}
        else if (sscanf_s(line, " grid %d", &gr) == 1) {}
        else if (sscanf_s(line, " gridext %f", &ge) == 1) {}
        else if (sscanf_s(line, " offset %f %f %f", &a, &b, &c) == 3) { ox=a; oy=b; oz=c; }
        // NOTE: the offset read here is DISCARDED once the user has nudged (see g_off_dirty).
        // Without that, the once-a-second reload silently undid every adjustment.
        else if (sscanf_s(line, " box %d %d %d", &x, &y, &z) == 3 && nb < 32) {
            g_boxes[nb][0] = x; g_boxes[nb][1] = y; g_boxes[nb][2] = z; nb++;
        }
    }
    fclose(f);
    g_scale = sc; g_nboxes = nb; g_span = sp; g_grid = gr; g_gridext = ge;
    if (!g_off_dirty) { g_off[0] = ox; g_off[1] = oy; g_off[2] = oz; }
}
static void save_cfg() {   // so live nudges survive a reinject
    FILE* f = nullptr; if (fopen_s(&f, g_cfgpath, "w") || !f) return;
    fprintf(f, "scale %.5f\nspan %d\ngrid %d\ngridext %.1f\noffset %.3f %.3f %.3f\n",
            g_scale, g_span, g_grid, g_gridext, g_off[0], g_off[1], g_off[2]);
    for (int i = 0; i < g_nboxes; i++) fprintf(f, "box %d %d %d\n", g_boxes[i][0], g_boxes[i][1], g_boxes[i][2]);
    fclose(f);
}

// ---- matrix helpers --------------------------------------------------------
// GL column-major: m[col*4 + row]. mat_get(m,r,c) reads row r col c.
static inline float mget(const float* m, int r, int c) { return m[c*4 + r]; }

static bool mat_inverse(const float* m, float* out) {
    double a[16]; for (int i = 0; i < 16; i++) a[i] = m[i];
    double inv[16];
    inv[0]= a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4]=-a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8]= a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12]=-a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1]=-a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5]= a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9]=-a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13]= a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2]= a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6]=-a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10]= a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14]=-a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3]=-a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7]= a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11]=-a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15]= a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (fabs(det) < 1e-12) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] * det);
    return true;
}
static void mat_mul(const float* A, const float* B, float* out) {  // out = A*B, column-major
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        float s = 0; for (int k = 0; k < 4; k++) s += mget(A, r, k) * mget(B, k, c);
        out[c*4 + r] = s;
    }
}

// ---- candidate MVP table ---------------------------------------------------
#define NCAND 32
static float g_cand[NCAND][16]; static unsigned g_chash[NCAND]; static long g_chits[NCAND];
static int   g_ncand = 0;
static float g_proj[16]; static volatile LONG g_have_proj = 0;
static float g_best[16];  static volatile LONG g_have_best = 0;
static float g_cam[3], g_fwd[3]; static long g_best_hits = 0;
struct RigidInfo { long hits; float cam[3]; };
static RigidInfo g_rigid[16]; static int g_nrigid_log = 0;
static float g_up[3], g_right[3];
struct CamPose { float pos[3], fwd[3], up[3], right[3]; DWORD tick; };
// peer/local camera pose shared with the net worker (declared here; used by select_best + draw,
// which precede the transport code near the bottom of the file)
static CamPose g_peer; static volatile LONG g_have_peer = 0; static DWORD g_peer_last = 0;
// set by coop.cpp every frame: 1 while the LOCAL player is actually in the workbench editor.
extern "C" volatile long g_in_bench;
// set from the partner's presence beacon: 1 while the PARTNER is in their workbench.
extern "C" volatile long g_peer_in_bench;
// 1 when both bench volumes are known and DIFFER - sync is hard-blocked until both use the same bench type.
extern "C" volatile long g_bench_mismatch;
// full-craft sync in flight (a pull clears the craft before rebuilding it - say so, or it looks broken)
extern "C" volatile long g_sync_busy, g_sync_got, g_sync_total, g_sync_err;
extern "C" volatile long g_sync_ask;   // a confirmation prompt - amber, and held longer than an error
extern "C" char g_sync_err_msg[96];
// local hover voxel, published every frame by coop.cpp's sample_cursor()
extern "C" volatile long g_cur_valid, g_cur_vx, g_cur_vy, g_cur_vz, g_cur_tool;
// world position of voxel (0,0,0) for the CURRENT bench - auto-calibration source (coop.cpp, vehicle+0x1F0)
extern "C" volatile long g_bench_org_valid;
extern "C" double g_bench_org[3];
// camera in ABSOLUTE world (coop.cpp, editor+0xE0) - lets us derive the floating-origin rebase exactly
extern "C" volatile long g_cam_world_valid;
extern "C" double g_cam_world[3];
// ONE partner colour for everything that represents them (camera frustum, cursor cell, HUD) so the player
// learns "orange = my partner" instead of decoding a different colour per feature.
static const float PARTNER_R = 1.00f, PARTNER_G = 0.55f, PARTNER_B = 0.15f;
// partner's hovered voxel (from CursorMsg kind 3, or from the F8 solo self-test)
static volatile LONG g_peer_cur_valid = 0;
static int   g_peer_cur_vx=0, g_peer_cur_vy=0, g_peer_cur_vz=0;
static DWORD g_peer_cur_last = 0;
static CamPose g_local_snapshot; static volatile LONG g_have_local = 0;
static volatile LONG g_net_ok = 0;
static uint64_t g_peerid = 0;   // 0 = no partner configured (used by draw())
// seqlocks so the 12-float pose copies between the render thread and net_worker cannot tear
// (single writer / single reader each). Odd seq = write in progress; reader retries.
static volatile LONG g_local_seq = 0, g_peer_seq = 0;

static void publish_pose(CamPose& dst, volatile LONG* seq,
                         const float* pos, const float* fwd, const float* up, const float* right) {
    InterlockedIncrement(seq);                 // -> odd
    MemoryBarrier();
    for (int i = 0; i < 3; i++) { dst.pos[i]=pos[i]; dst.fwd[i]=fwd[i]; dst.up[i]=up[i]; dst.right[i]=right[i]; }
    MemoryBarrier();
    InterlockedIncrement(seq);                 // -> even
}
static bool read_pose(const CamPose& src, volatile LONG* seq, CamPose& out) {
    for (int t = 0; t < 8; t++) {
        LONG s1 = *seq;
        if (s1 & 1) { continue; }              // writer mid-update
        MemoryBarrier();
        for (int i = 0; i < 3; i++) { out.pos[i]=src.pos[i]; out.fwd[i]=src.fwd[i]; out.up[i]=src.up[i]; out.right[i]=src.right[i]; }
        MemoryBarrier();
        if (*seq == s1) return true;           // no write straddled the copy
    }
    return false;                              // writer too hot this instant; skip a frame/tick
}

static unsigned hash16(const float* m) {
    unsigned h = 2166136261u;
    for (int i = 0; i < 16; i++) { int q = (int)lrintf(m[i] * 1000.0f); h ^= (unsigned)q; h *= 16777619u; }
    return h;
}
static bool wrow_trivial(const float* m) {
    return fabsf(m[3]) < 1e-5f && fabsf(m[7]) < 1e-5f && fabsf(m[11]) < 1e-5f && fabsf(m[15]-1.0f) < 1e-5f;
}
static bool is_pure_persp(const float* m) { return fabsf(m[11]+1.0f) < 0.01f && fabsf(m[15]) < 0.01f; }

static void APIENTRY my_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (v && count >= 1 && g_ready && !InterlockedCompareExchange(&g_stop, 0, 0)) {
        if (is_pure_persp(v)) { memcpy(g_proj, v, 64); InterlockedExchange(&g_have_proj, 1); }
        else if (!wrow_trivial(v)) {
            unsigned h = hash16(v);
            int i = 0;
            for (; i < g_ncand; i++) if (g_chash[i] == h) { g_chits[i]++; break; }
            if (i == g_ncand && g_ncand < NCAND) {
                memcpy(g_cand[g_ncand], v, 64); g_chash[g_ncand] = h; g_chits[g_ncand] = 1; g_ncand++;
            }
        }
    }
    g_next_um4(loc, count, transpose, v);
}

// Pick the craft's MVP STRUCTURALLY, never by GL program id (those are not stable across
// machines): the winner is the most-uploaded candidate whose inv(P)*MVP is a rigid transform.
static void select_best() {
    if (!InterlockedCompareExchange(&g_have_proj, 0, 0)) return;
    float pinv[16];
    if (!mat_inverse(g_proj, pinv)) return;
    int best = -1; long besth = 0; float bestvm[16];
    for (int i = 0; i < g_ncand; i++) {
        if (g_chits[i] <= besth) continue;
        float vm[16]; mat_mul(pinv, g_cand[i], vm);
        if (fabsf(mget(vm,3,0)) > 1e-3f || fabsf(mget(vm,3,1)) > 1e-3f ||
            fabsf(mget(vm,3,2)) > 1e-3f || fabsf(mget(vm,3,3)-1.0f) > 1e-3f) continue;
        bool rigid = true;                              // R*Rt == I ?
        for (int r = 0; r < 3 && rigid; r++) for (int c = 0; c < 3; c++) {
            float d = 0; for (int k = 0; k < 3; k++) d += mget(vm,r,k) * mget(vm,c,k);
            if (fabsf(d - (r == c ? 1.0f : 0.0f)) > 2e-3f) { rigid = false; break; }
        }
        if (!rigid) continue;
        best = i; besth = g_chits[i]; memcpy(bestvm, vm, 64);
    }
    // Every rigid candidate is SOME object's model-view. The one we lock has an identity
    // model matrix (world space); if another rigid candidate is the CRAFT BODY's, then the
    // difference between the two derived camera positions is exactly the body's world
    // translation - i.e. the offset, measured rather than eyeballed.
    {
        g_nrigid_log = 0;
        for (int i = 0; i < g_ncand; i++) {
            if (g_chits[i] <= 0 || g_nrigid_log >= 16) continue;
            float vm[16]; mat_mul(pinv, g_cand[i], vm);
            if (fabsf(mget(vm,3,3)-1.0f) > 1e-3f) continue;
            bool rg = true;
            for (int r = 0; r < 3 && rg; r++) for (int c = 0; c < 3; c++) {
                float d = 0; for (int k = 0; k < 3; k++) d += mget(vm,r,k) * mget(vm,c,k);
                if (fabsf(d - (r == c ? 1.0f : 0.0f)) > 2e-3f) { rg = false; break; }
            }
            if (!rg) continue;
            RigidInfo& ri = g_rigid[g_nrigid_log++];
            ri.hits = g_chits[i];
            for (int r = 0; r < 3; r++) {
                ri.cam[r] = 0;
                for (int k = 0; k < 3; k++) ri.cam[r] -= mget(vm,k,r) * mget(vm,k,3);
            }
        }
    }
    if (best < 0) { InterlockedExchange(&g_have_best, 0); return; }
    memcpy(g_best, g_cand[best], 64);
    g_best_hits = besth;
    for (int r = 0; r < 3; r++) {                       // cam = -Rt * t
        g_cam[r] = 0;
        for (int k = 0; k < 3; k++) g_cam[r] -= mget(bestvm,k,r) * mget(bestvm,k,3);
        g_right[r] =  mget(bestvm, 0, r);
        g_up[r]    =  mget(bestvm, 1, r);
        g_fwd[r]   = -mget(bestvm, 2, r);
    }
    // publish the latest local pose for the net worker to send (seqlock: single writer here)
    publish_pose(g_local_snapshot, &g_local_seq, g_cam, g_fwd, g_up, g_right);
    InterlockedExchange(&g_have_local, 1);
    InterlockedExchange(&g_have_best, 1);
}

// ---- projection + drawing --------------------------------------------------
static int g_W = 0, g_H = 0;
static bool project(const float* mvp, float x, float y, float z, float* sx, float* sy) {
    float cx = mget(mvp,0,0)*x + mget(mvp,0,1)*y + mget(mvp,0,2)*z + mget(mvp,0,3);
    float cy = mget(mvp,1,0)*x + mget(mvp,1,1)*y + mget(mvp,1,2)*z + mget(mvp,1,3);
    float cw = mget(mvp,3,0)*x + mget(mvp,3,1)*y + mget(mvp,3,2)*z + mget(mvp,3,3);
    if (cw <= 1e-5f) return false;                      // behind the eye
    *sx = (cx/cw * 0.5f + 0.5f) * g_W;
    *sy = (1.0f - (cy/cw * 0.5f + 0.5f)) * g_H;         // our ortho is y-down
    return true;
}
static void draw_text(int x, int y_top, float r, float g, float b, const char* s);   // fwd

static void world_line(float ax, float ay, float az, float bx, float by, float bz) {
    float p1x, p1y, p2x, p2y;
    if (!project(g_best, ax, ay, az, &p1x, &p1y)) return;
    if (!project(g_best, bx, by, bz, &p2x, &p2y)) return;
    glVertex2f(p1x, p1y); glVertex2f(p2x, p2y);
}
// A labelled ground grid in WORLD metres. The craft's position can be read straight off it,
// which is what tells us the body transform without guessing.
static void draw_world_grid() {
    const float e = g_gridext;
    glLineWidth(1.0f);
    glColor4f(0.35f, 0.75f, 1.0f, 0.35f);
    glBegin(GL_LINES);
    for (float t = -e; t <= e + 0.01f; t += 5.0f) {
        world_line(-e, 0, t,  e, 0, t);
        world_line( t, 0, -e, t, 0, e);
    }
    glEnd();
    glLineWidth(3.0f);
    glBegin(GL_LINES);                       // world axes at the origin
      glColor4f(1, 0.25f, 0.25f, 1); world_line(0,0,0, 10,0,0);   // +X red
      glColor4f(0.25f, 1, 0.25f, 1); world_line(0,0,0, 0,10,0);   // +Y green
      glColor4f(0.4f, 0.5f, 1, 1);   world_line(0,0,0, 0,0,10);   // +Z blue
    glEnd();
    glLineWidth(2.0f);
    char lab[48]; float lx, ly;
    for (float t = -e; t <= e + 0.01f; t += 10.0f) {
        if (project(g_best, t, 0, 0, &lx, &ly)) { sprintf_s(lab, "x=%.0f", t); draw_text((int)lx, (int)ly, 1, 0.6f, 0.6f, lab); }
        if (project(g_best, 0, 0, t, &lx, &ly)) { sprintf_s(lab, "z=%.0f", t); draw_text((int)lx, (int)ly, 0.6f, 0.7f, 1, lab); }
    }
    if (project(g_best, 10, 0, 0, &lx, &ly)) draw_text((int)lx, (int)ly, 1, 0.3f, 0.3f, "+X");
    if (project(g_best, 0, 10, 0, &lx, &ly)) draw_text((int)lx, (int)ly, 0.3f, 1, 0.3f, "+Y");
    if (project(g_best, 0, 0, 10, &lx, &ly)) draw_text((int)lx, (int)ly, 0.4f, 0.5f, 1, "+Z");
}
static void draw_voxel_box(int vx, int vy, int vz, float r, float g, float b) {
    const float s = g_scale;
    const float x0 = vx*s + g_off[0], y0 = vy*s + g_off[1], z0 = vz*s + g_off[2];
    const float x1 = x0+s, y1 = y0+s, z1 = z0+s;
    const float cx[8] = {x0,x1,x1,x0, x0,x1,x1,x0};
    const float cy[8] = {y0,y0,y1,y1, y0,y0,y1,y1};
    const float cz[8] = {z0,z0,z0,z0, z1,z1,z1,z1};
    float px[8], py[8]; bool ok[8];
    for (int i = 0; i < 8; i++) ok[i] = project(g_best, cx[i], cy[i], cz[i], &px[i], &py[i]);
    static const int E[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    glColor4f(r, g, b, 0.95f);
    glBegin(GL_LINES);
    for (int e = 0; e < 12; e++) {
        int a = E[e][0], c = E[e][1];
        if (!ok[a] || !ok[c]) continue;
        glVertex2f(px[a], py[a]); glVertex2f(px[c], py[c]);
    }
    glEnd();
}
// 3x5 bitmap font drawn as QUADS.
//
// This replaces wglUseFontBitmaps, which rendered nothing here: it bakes glBitmap into
// display lists, so the pixel-unpack state the game had bound at build time (a bound
// GL_PIXEL_UNPACK_BUFFER will do it) silently produced blank glyphs. Quads depend on no
// pixel state whatsoever, and they scale - which world-space name tags will want.
//
// A 5x7 font. The overlay ran on a 3x5 one - three pixels of glyph width scaled up 2-3x is a blocky
// letterform that no amount of smoothing can rescue, and it is why every panel read as pixel art.
// 5x7 is the classic readable minimum and nearly triples the resolution for the same cost: still one
// quad per set pixel, in the same immediate-mode batch.
// Rows are 5 bits, bit 4 = leftmost column.
static const unsigned char FONT57[][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ' '
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },  // '!'
    { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '"'
    { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },  // '#'
    { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04 },  // '$'
    { 0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13 },  // '%'
    { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D },  // '&'
    { 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '''
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },  // '('
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },  // ')'
    { 0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00 },  // '*'
    { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },  // '+'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08 },  // ','
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },  // '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },  // '.'
    { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 },  // '/'
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  // '0'
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  // '1'
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  // '2'
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },  // '3'
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  // '4'
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },  // '5'
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  // '6'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  // '7'
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  // '8'
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },  // '9'
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },  // ':'
    { 0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x04 },  // ';'
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 },  // '<'
    { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },  // '='
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 },  // '>'
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },  // '?'
    { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E },  // '@'
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },  // 'A'
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },  // 'B'
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },  // 'C'
    { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },  // 'D'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },  // 'E'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },  // 'F'
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },  // 'G'
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },  // 'H'
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },  // 'I'
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },  // 'J'
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },  // 'K'
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },  // 'L'
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },  // 'M'
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },  // 'N'
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },  // 'O'
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },  // 'P'
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },  // 'Q'
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },  // 'R'
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },  // 'S'
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },  // 'T'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },  // 'U'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },  // 'V'
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },  // 'W'
    { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11 },  // 'X'
    { 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04 },  // 'Y'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },  // 'Z'
    { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E },  // '['
    { 0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01 },  // backslash
    { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E },  // ']'
    { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00 },  // '^'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },  // '_'
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },  // '|'
};
static const char FONT57_SET[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_|";
static int font57_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');   // the set is upper-case only
    for (int i = 0; FONT57_SET[i]; i++) if (FONT57_SET[i] == c) return i;
    return 0;                                              // unknown -> space, never a garbage glyph
}
static int g_px = 3;                                     // pixel size; char cell = 3*px wide
// 5 columns + 1 of spacing.
static const int FONT_ADV = 6, FONT_ROWS = 7;

static void draw_text_px(int x, int y_top, float r, float g, float b, const char* s, int px) {
    // A soft drop shadow first. These panels sit over whatever the game is drawing, and a one-pixel dark
    // offset is what stops light text dissolving into a bright scene - it costs one extra batch and does
    // more for legibility than any amount of glyph detail.
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
        else           glColor4f(r, g, b, 1.0f);
        const int off = pass == 0 ? px : 0;
        glBegin(GL_QUADS);
        int cx = x;
        for (const char* q = s; *q; q++, cx += FONT_ADV * px) {
            const unsigned char* gl = FONT57[font57_index(*q)];
            for (int row = 0; row < FONT_ROWS; row++) {
                unsigned char bits = gl[row];
                if (!bits) continue;
                // Runs of set pixels become ONE quad rather than several: fewer vertices, and no seam
                // artefacts between adjacent pixels within a glyph.
                int col = 0;
                while (col < 5) {
                    if (!(bits & (0x10 >> col))) { col++; continue; }
                    int run = 0;
                    while (col + run < 5 && (bits & (0x10 >> (col + run)))) run++;
                    float qx = (float)(cx + col * px + off), qy = (float)(y_top + row * px + off);
                    float w = (float)(run * px), h = (float)px;
                    glVertex2f(qx, qy);         glVertex2f(qx + w, qy);
                    glVertex2f(qx + w, qy + h); glVertex2f(qx, qy + h);
                    col += run;
                }
            }
        }
        glEnd();
    }
}

// ======================= BOOT SEQUENCE OVERLAY =======================
// The mod now loads WITH the game rather than being injected into a running one, so there is a real startup
// period - hooks going in, Steam warming up, the localisation table filling, a partner pairing - that used
// to be invisible unless someone opened a log file. Showing it does three jobs at once: it proves the mod
// loaded (the first thing anyone wants to know), it shows WHERE a failed start got to, and a screenshot of
// it is a far better bug report than "it didn't work".
//
// Deliberately cheap: text lines and two quads, drawn only for a few seconds, in the same immediate-mode GL
// the rest of the overlay already uses. It fades out on its own so it never becomes clutter.

#define BOOT_MAX 12
// `status` is stored rather than inferred: the draw code used to recover it by comparing the colour back
// (amber-ish => in progress), which breaks the moment a palette entry changes.
//   0 = working   1 = ok   2 = failed   3 = neutral   4 = waiting (normal, still watching)   5 = note
// A note is a hint line, not a startup step - it gets a text row but NO voxel, because a dot that does not
// correspond to a step is exactly what makes the row look arbitrary.
struct BootStep { char text[72]; float r, g, b; DWORD at; int status; };
static BootStep g_boot[BOOT_MAX];
static int      g_nboot = 0;
static DWORD    g_boot_t0 = 0;
static bool     g_boot_done = false;      // set once the sequence has faded; stops all of this drawing

// Called from coop.cpp as startup progresses. Thread-safe enough for the purpose: the writer is the setup
// thread, the reader is the render thread, and a torn read costs one frame of a half-written string.
extern "C" __declspec(dllexport) void wsdraw_boot_step(const char* text, int status) {
    if (g_nboot >= BOOT_MAX || g_boot_done) return;
    BootStep* b = &g_boot[g_nboot];
    strncpy_s(b->text, sizeof b->text, text ? text : "", _TRUNCATE);
    // 0 = working (amber), 1 = ok (green), 2 = failed (red), 3 = skipped/neutral (grey)
    b->status = status;
    switch (status) {
        case 1:  b->r=0.35f; b->g=0.95f; b->b=0.55f; break;   // ok
        case 2:  b->r=1.00f; b->g=0.35f; b->b=0.30f; break;   // failed
        case 3:  b->r=0.55f; b->g=0.58f; b->b=0.64f; break;   // neutral
        case 4:  b->r=0.40f; b->g=0.70f; b->b=1.00f; break;   // waiting - calm blue, NOT amber: "no partner
                                                              // yet" is a normal state, and amber reads as a
                                                              // fault sitting among the greens
        case 5:  b->r=0.50f; b->g=0.54f; b->b=0.60f; break;   // note (no voxel)
        default: b->r=1.00f; b->g=0.78f; b->b=0.30f; break;   // working
    }
    b->at = GetTickCount();
    if (!g_boot_t0) g_boot_t0 = b->at;
    g_nboot++;
}

// Update the status of the most recent line with the same prefix - so a step can appear as "working" and
// then resolve, rather than every state change adding another row.
extern "C" __declspec(dllexport) void wsdraw_boot_resolve(const char* prefix, const char* text, int status) {
    if (g_boot_done || !prefix) return;
    for (int i = g_nboot - 1; i >= 0; i--) {
        if (strncmp(g_boot[i].text, prefix, strlen(prefix)) != 0) continue;
        strncpy_s(g_boot[i].text, sizeof g_boot[i].text, text ? text : g_boot[i].text, _TRUNCATE);
        g_boot[i].status = status;
        switch (status) {
            case 1:  g_boot[i].r=0.35f; g_boot[i].g=0.95f; g_boot[i].b=0.55f; break;
            case 2:  g_boot[i].r=1.00f; g_boot[i].g=0.35f; g_boot[i].b=0.30f; break;
            case 3:  g_boot[i].r=0.55f; g_boot[i].g=0.58f; g_boot[i].b=0.64f; break;
            case 4:  g_boot[i].r=0.40f; g_boot[i].g=0.70f; g_boot[i].b=1.00f; break;
            case 5:  g_boot[i].r=0.50f; g_boot[i].g=0.54f; g_boot[i].b=0.60f; break;
            default: g_boot[i].r=1.00f; g_boot[i].g=0.78f; g_boot[i].b=0.30f; break;
        }
        return;
    }
    wsdraw_boot_step(text ? text : prefix, status);
}

// Small helpers so the sequence below reads as animation rather than as GL bookkeeping.
static void quad(float x, float y, float w, float h, float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
      glVertex2f(x, y);     glVertex2f(x + w, y);
      glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}
// ease-out cubic - fast in, gentle landing. Everything here uses it so the motion feels like one system.
static float ease_out(float t) { if (t < 0) t = 0; if (t > 1) t = 1; float u = 1.0f - t; return 1.0f - u*u*u; }
static float ease_back(float t) {   // slight overshoot, for things that "pop" into place
    if (t < 0) t = 0; if (t > 1) t = 1;
    float u = t - 1.0f; const float s = 1.70158f;
    return u*u*((s + 1.0f)*u + s) + 1.0f;
}

// A rotated rectangle. The panel is all axis-aligned quads, but a hammer that cannot swing is not a hammer.
// Rect given in LOCAL coords around a pivot, then rotated by `a` radians and translated to (px,py).
// Screen y grows downward, so a NEGATIVE angle lifts the far end of the hammer upwards.
static void quad_rot(float px, float py, float x0, float y0, float x1, float y1,
                     float a, float r, float g, float b, float al) {
    const float c = (float)cos(a), s = (float)sin(a);
    const float vx[4] = { x0, x1, x1, x0 };
    const float vy[4] = { y0, y0, y1, y1 };
    glColor4f(r, g, b, al);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++)
        glVertex2f(px + vx[i]*c - vy[i]*s, py + vx[i]*s + vy[i]*c);
    glEnd();
}

// ---- curves. The overlay has only ever drawn axis-aligned rectangles, which is why anything built from
// them reads as blocky. These are the same immediate-mode GL, just not squares.
static void ring(float cx, float cy, float rad, float w, int seg,
                 float r, float g, float b, float al) {
    glColor4f(r, g, b, al);
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= seg; i++) {
        float a = (float)i / seg * 6.2831853f, c = (float)cos(a), s2 = (float)sin(a);
        glVertex2f(cx + c*(rad-w), cy + s2*(rad-w));
        glVertex2f(cx + c*(rad+w), cy + s2*(rad+w));
    }
    glEnd();
}

// THE RADAR. It replaced a hammer that was both too blocky and too close to another game's forge - but the
// better reason is that this one MEANS something. At boot the only genuinely pending thing is the partner
// search, so the sweep runs while we are looking and locks onto a contact when one is found. Decoration that
// doubles as the status readout for the one step that is actually waiting.
static void draw_boot_radar(float cx, float cy, float rad, bool searching, DWORD now, float A) {
    const float R = 0.30f, G = 0.85f, B = 0.75f;         // phosphor green-cyan

    // dish: face, range rings, crosshairs
    glColor4f(0.04f, 0.10f, 0.11f, 0.55f * A);
    glBegin(GL_TRIANGLE_FAN);
      glVertex2f(cx, cy);
      for (int i = 0; i <= 48; i++) { float a = (float)i/48*6.2831853f;
          glVertex2f(cx + (float)cos(a)*rad, cy + (float)sin(a)*rad); }
    glEnd();
    ring(cx, cy, rad,          1.1f, 56, R, G, B, 0.55f * A);
    ring(cx, cy, rad * 0.66f,  0.7f, 44, R, G, B, 0.24f * A);
    ring(cx, cy, rad * 0.33f,  0.7f, 32, R, G, B, 0.24f * A);
    quad(cx - rad, cy - 0.5f, rad*2, 1.0f, R, G, B, 0.18f * A);
    quad(cx - 0.5f, cy - rad, 1.0f, rad*2, R, G, B, 0.18f * A);

    // sweep: a wedge trailing the leading edge, fading with a square falloff so it reads as persistence
    const float PERIOD = 2600.0f;
    float ang = searching ? (float)(now % (DWORD)PERIOD) / PERIOD * 6.2831853f : -1.05f;
    const float TRAIL = 1.85f;
    const int   SEG   = 26;
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < SEG; i++) {
        float t0 = (float)i/SEG, t1 = (float)(i+1)/SEG;
        float a0 = ang - TRAIL*t0, a1 = ang - TRAIL*t1;
        float f0 = (1.0f-t0)*(1.0f-t0), f1 = (1.0f-t1)*(1.0f-t1);
        glColor4f(R, G, B, 0.30f * f0 * A); glVertex2f(cx, cy);
        glColor4f(R, G, B, 0.22f * f0 * A); glVertex2f(cx + (float)cos(a0)*rad, cy + (float)sin(a0)*rad);
        glColor4f(R, G, B, 0.22f * f1 * A); glVertex2f(cx + (float)cos(a1)*rad, cy + (float)sin(a1)*rad);
    }
    glEnd();
    glColor4f(0.55f, 1.0f, 0.90f, 0.85f * A);            // bright leading edge
    glBegin(GL_TRIANGLES);
      glVertex2f(cx, cy);
      glVertex2f(cx + (float)cos(ang-0.02f)*rad, cy + (float)sin(ang-0.02f)*rad);
      glVertex2f(cx + (float)cos(ang+0.02f)*rad, cy + (float)sin(ang+0.02f)*rad);
    glEnd();

    // faint returns - fixed bearings, brightening as the sweep passes and decaying after. No RNG: a hash of
    // the index, so the picture is identical every launch instead of flickering differently each time.
    for (int i = 0; i < 5; i++) {
        unsigned h = (unsigned)(i*2654435761u);
        float ba = (float)(h % 6283) / 1000.0f;
        float br = rad * (0.28f + (float)((h >> 11) % 620) / 1000.0f);
        float d = ang - ba; while (d < 0) d += 6.2831853f; while (d > 6.2831853f) d -= 6.2831853f;
        float f = d < 2.2f ? (1.0f - d / 2.2f) : 0.0f;
        if (f <= 0.0f) continue;
        float px = cx + (float)cos(ba)*br, py = cy + (float)sin(ba)*br;
        ring(px, py, 2.4f, 1.2f, 10, 0.45f, 0.95f, 0.80f, f*f * 0.75f * A);
    }

    if (!searching) {
        // CONTACT. Locked bearing, a pulsing blip and an expanding ring - the moment a partner is found.
        float px = cx + (float)cos(-1.05f)*rad*0.58f, py = cy + (float)sin(-1.05f)*rad*0.58f;
        float ph = (float)(now % 1400) / 1400.0f;
        ring(px, py, 3.0f + ph*rad*0.30f, 1.0f, 20, 0.45f, 1.0f, 0.70f, (1.0f-ph) * 0.7f * A);
        glColor4f(0.60f, 1.0f, 0.72f, 0.95f * A);
        glBegin(GL_TRIANGLE_FAN);
          glVertex2f(px, py);
          for (int i = 0; i <= 14; i++) { float a = (float)i/14*6.2831853f;
              glVertex2f(px + (float)cos(a)*3.4f, py + (float)sin(a)*3.4f); }
        glEnd();
        draw_text_px((int)(cx - rad), (int)(cy + rad + 6), 0.45f*A, 1.0f*A, 0.72f*A, "CONTACT", 2);
    } else {
        draw_text_px((int)(cx - rad), (int)(cy + rad + 6), 0.35f*A, 0.72f*A, 0.68f*A, "SCANNING", 2);
    }
}

static void draw_boot_sequence(int sw, int sh) {
    if (g_boot_done || !g_nboot || !g_boot_t0) return;
    DWORD now = GetTickCount();

    // ANCHOR EVERYTHING TO THE FIRST FRAME WE ACTUALLY DRAW, not to when the steps were pushed. Under the
    // ASI loader the mod loads and finishes its whole startup during process init - long before the game
    // opens a window - so by the time this first renders, every push timestamp is already seconds in the
    // past and the entire sequence has "played" to an empty screen. Anchoring here means the animation
    // starts when someone can first see it, whatever happened before that.
    static DWORD s_first_draw = 0;
    if (!s_first_draw) s_first_draw = now;
    DWORD age = now - s_first_draw;

    // REVEAL ORDER. Most of setup() finishes in one burst, so the steps arrive within a few milliseconds of
    // each other and the whole row appeared at once - which reads as a flash, not as loading. Enforce a
    // minimum spacing between reveals so the dots light up left to right. A step that genuinely took time
    // still shows at its real moment; only bursts get spread out, so this never fakes progress that did not
    // happen, it just stops simultaneous events from being indistinguishable.
    const DWORD STAGGER = 220;      // per dot. 130 ms put the whole row up in half a second - too fast to read
    const DWORD LEADIN  = 300;      // so the panel is on screen before the first dot lights

    // DISPLAY ORDER: settled steps first in the order they happened, then the hint line, then anything
    // still PENDING. So both the strip and the list END on what we are still waiting for - a blue
    // "no partner yet" belongs on the right as the thing in progress, not buried mid-row where it reads as
    // a fault among the greens, and it should be the last line your eye lands on.
    // Ordering by STATUS rather than by reordering the boot_step() calls also means a step that resolves
    // late (the workbench prompt patch retries from the frame hook) cannot land after it.
    int ord[BOOT_MAX]; int nord = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < g_nboot && nord < BOOT_MAX; i++) {
            int st = g_boot[i].status;
            int cls = (st == 5) ? 1 : ((st == 4 || st == 0) ? 2 : 0);   // 0 settled, 1 note, 2 pending
            if (cls == pass) ord[nord++] = i;
        }

    DWORD reveal[BOOT_MAX];
    for (int d = 0; d < nord; d++) {
        DWORD want = s_first_draw + LEADIN + (DWORD)d * STAGGER;
        // A step that genuinely happens LATER than its slot still shows at its real moment - so a slow Steam
        // handshake reads as slow, and the animation never claims progress that has not happened.
        reveal[d] = g_boot[ord[d]].at > want ? g_boot[ord[d]].at : want;
        if (d > 0 && reveal[d] < reveal[d-1] + STAGGER) reveal[d] = reveal[d-1] + STAGGER;
    }
    const DWORD last_reveal = nord ? reveal[nord-1] : now;

    DWORD since_last = now > last_reveal ? now - last_reveal : 0;
    float alpha = 1.0f;
    if (since_last > 6000) {
        if (since_last > 8000) { g_boot_done = true; return; }
        alpha = 1.0f - (float)(since_last - 6000) / 2000.0f;
        alpha = ease_out(alpha);            // fade out on the same curve everything else moves on
    }

    const int PAD = 22, ROW = 26, W = 760;   // room for the text column AND the radar beside it
    const int HEAD = 60, H = PAD*2 + HEAD + g_nboot*ROW;
    const float X = (float)((sw - W) / 2);
    // Slide up and settle, with a touch of overshoot.
    float intro = ease_back(age / 420.0f);
    // BOTTOM-ANCHORED, not a fixed fraction from the top. At 0.24 it landed straight on the Stormworks
    // logo, which occupies the upper-middle of both the loading screen and the main menu (wordmark ends
    // ~53% down, the menu button row sits ~69%). Anchoring to the bottom edge clears all of it at any
    // resolution and any step count, since the panel grows upward as steps arrive rather than downward
    // into the buttons.
    const float Y = (float)sh - (float)H - (float)sh * 0.045f + (1.0f - intro) * 26.0f;
    float A = alpha * ease_out(age / 260.0f);

    // ---- panel: dark body, a brighter rim, and a soft top edge so it reads as lit from above ----
    quad(X, Y, (float)W, (float)H, 0.035f, 0.05f, 0.085f, 0.88f * A);
    quad(X, Y, (float)W, 2.0f,     0.35f,  0.72f, 1.0f,   0.55f * A);   // top hairline
    quad(X, Y + H - 1.5f, (float)W, 1.5f, 0.10f, 0.20f, 0.32f, 0.6f * A);

    // ---- a slow highlight sweeping across the panel, like a scanline ----
    float sweep = (float)((age % 2600) / 2600.0f);
    float sx = X - 120.0f + sweep * (W + 240.0f);
    for (int i = 0; i < 10; i++) {                     // cheap soft edge: a few stacked slivers
        float f = 1.0f - (float)i / 10.0f;
        quad(sx + i*6.0f, Y, 6.0f, (float)H, 0.35f, 0.75f, 1.0f, 0.030f * f * A);
        quad(sx - i*6.0f, Y, 6.0f, (float)H, 0.35f, 0.75f, 1.0f, 0.030f * f * A);
    }

    // ---- voxel row: blocks assembling, one per boot step, because this is a block-building game ----
    const float VS = 15.0f, VG = 6.0f;                 // size, gap
    const float vx0 = X + PAD, vy = Y + 20.0f;
    // One voxel per REAL step - notes (status 5) are hints, not steps, and a dot with no step behind it is
    // what made the row look arbitrary. No trailing empty slots either: BOOT_MAX is a buffer bound, not a
    // promise about how many steps this launch has, and six empty outlines read as "something didn't finish".
    int slot = 0;
    int slot_of[BOOT_MAX];
    for (int d = 0; d < BOOT_MAX; d++) slot_of[d] = -1;
    for (int d = 0; d < nord; d++) {
        const BootStep* st = &g_boot[ord[d]];
        if (st->status == 5) continue;
        float bx = vx0 + slot * (VS + VG);
        slot_of[d] = slot;
        slot++;
        // Not yet its turn: a faint outline, so you can see how much is still to come.
        if (now < reveal[d]) { quad(bx, vy, VS, VS, 0.16f, 0.22f, 0.30f, 0.35f * A); continue; }
        float bt = (float)(now - reveal[d]) / 300.0f;
        float sc = ease_back(bt);
        if (sc > 1.0f) sc = 1.0f;
        float s = VS * sc, off = (VS - s) * 0.5f;
        // A waiting step breathes, so it reads as "still going" rather than "stopped here".
        float a = 0.95f;
        if (st->status == 4 || st->status == 0) {
            float ph = (float)((now % 1600) / 1600.0f) * 6.2832f;
            a = 0.62f + 0.33f * (0.5f + 0.5f * (float)cos(ph));
        }
        quad(bx + off, vy + off, s, s, st->r, st->g, st->b, a * A);
        if (bt < 1.0f) quad(bx - 3, vy - 3, VS + 6, VS + 6, st->r, st->g, st->b, 0.35f * (1.0f - bt) * A);
    }

    // ---- the hammer: swings onto whichever dot is about to light ----
    {
        int td = -1;
        for (int d = 0; d < nord; d++) if (slot_of[d] >= 0 && now < reveal[d]) { td = d; break; }
        if (td < 0)                                        // all struck - keep it on the last for the recoil
            for (int d = nord - 1; d >= 0; d--) if (slot_of[d] >= 0) { td = d; break; }
        (void)td;
        // Right-hand side of the panel - the text only ever fills the left half, so this was dead space.
        // "Searching" is taken from the boot steps themselves: status 4 is the pending partner search, so
        // the sweep runs exactly while that step is unresolved and locks the moment it is not.
        bool searching = false;
        for (int d = 0; d < nord; d++) if (g_boot[ord[d]].status == 4) { searching = true; break; }
        float rad = (float)H * 0.34f; if (rad > 62.0f) rad = 62.0f; if (rad < 26.0f) rad = 26.0f;
        draw_boot_radar(X + W * 0.79f, Y + (float)H * 0.47f, rad, searching, now, A);
    }

    // ---- title, revealed a character at a time ----
    {
        static const char* T = "COOP WORKBENCH  " COOP_VERSION;
        int n = (int)strlen(T);
        int shown = (int)(age / 22);                  // ~45 chars/sec
        if (shown > n) shown = n;
        char buf[96]; int m = shown < 95 ? shown : 95;
        memcpy(buf, T, m); buf[m] = 0;
        draw_text_px((int)X + PAD, (int)Y + 40, 0.55f*A, 0.90f*A, 1.0f*A, buf, 3);
        // caret while typing
        if (shown < n && ((age / 260) % 2) == 0)
            quad(X + PAD + shown*16.0f, Y + 40, 10.0f, 19.0f, 0.55f, 0.90f, 1.0f, 0.7f * A);
    }

    // ---- step lines: each types itself in, with a live glyph on the left ----
    static const char SPIN[] = { '|', '/', '-', '\\' };
    for (int d = 0; d < nord; d++) {
        const int i = ord[d];
        const BootStep* st = &g_boot[i];
        if (now < reveal[d]) continue;                 // same cadence as its voxel
        DWORD ra = now - reveal[d];
        float rf = ease_out(ra / 200.0f);
        float a = A * rf;
        int ly = (int)Y + PAD + HEAD + d*ROW;   // display slot, not push index

        // glyph: spinner while working, a tick once resolved, a dot when neutral
        char gl[2] = { ' ', 0 };
        switch (st->status) {
            case 1:  gl[0] = '+'; break;                       // done
            case 2:  gl[0] = 'x'; break;                       // failed
            case 3:  gl[0] = '-'; break;                       // neutral
            case 4:  gl[0] = SPIN[(now / 160) % 4]; break;     // waiting - still watching
            case 5:  gl[0] = ' '; break;                       // note
            default: gl[0] = SPIN[(now / 110) % 4]; break;     // working
        }
        draw_text_px((int)X + PAD, ly, st->r*a, st->g*a, st->b*a, gl, 2);

        // the text types out just after its row appears
        int n = (int)strlen(st->text);
        int shown = (int)((ra > 120 ? ra - 120 : 0) / 14);
        if (shown > n) shown = n;
        char buf[80]; int m = shown < 79 ? shown : 79;
        memcpy(buf, st->text, m); buf[m] = 0;
        draw_text_px((int)X + PAD + 24, ly, st->r*a, st->g*a, st->b*a, buf, 2);
    }

    // ---- progress rail along the bottom, with a soft leading edge ----
    int shown_steps = 0;
    for (int d = 0; d < nord; d++) if (now >= reveal[d]) shown_steps++;
    float prog = nord ? (float)shown_steps / (float)nord : 0.0f;
    if (prog > 1.0f) prog = 1.0f;
    static float s_shown = 0.0f;
    s_shown += (prog - s_shown) * 0.08f;                                     // eases toward the target
    float py = Y + H - 6.0f;
    quad(X, py, (float)W, 3.5f, 0.12f, 0.18f, 0.26f, 0.8f * A);
    quad(X, py, W * s_shown, 3.5f, 0.30f, 0.78f, 1.0f, 0.95f * A);
    quad(X + W*s_shown - 14.0f, py - 1.5f, 14.0f, 6.0f, 0.6f, 0.9f, 1.0f, 0.7f * A);
}
static void draw_text(int x, int y_top, float r, float g, float b, const char* s) {
    draw_text_px(x, y_top, r, g, b, s, g_px);
}


// ======================= IN-GAME LOG VIEWER (F8) =======================
// Every diagnosis in this project has run the same loop: do a thing in game, alt-tab, open a text file,
// scroll to the bottom, work out which lines belong to the thing you just did, alt-tab back. That loop is
// the single biggest cost in testing, it is worse in fullscreen, and it is impossible for anyone else -
// a tester who cannot read the log can only report "it didn't work".
//
// So the log draws in the game. F8 toggles it, PageUp/PageDown scroll, Home/End jump, F2 filters. Lines are
// coloured by what they are, which matters more than it sounds: a wall of monospaced text is unreadable at a
// glance, but "red = something refused, cyan = we sent, green = we received" is readable at speed.
//
// The ring is memory-only and separate from the file - the file stays the artifact you attach to a bug
// report, this is the one you read while playing.

#define LOG_RING  1024
#define LOG_COLS  220
static char  g_lr[LOG_RING][LOG_COLS];
static volatile LONG g_lr_total = 0;          // lines ever pushed; index = (total-1) % LOG_RING
static CRITICAL_SECTION g_lr_cs;
static bool  g_lr_ok = false;
static volatile LONG g_logview = 0;           // F8
static volatile LONG g_lr_scroll = 0;         // lines back from the newest; 0 = following the tail
static volatile LONG g_lr_filter = 0;         // F2 cycles

// Called from coop.cpp's logline for every line written. Must stay cheap and must never block the caller for
// long - logging happens on the Steam thread, the main thread and the setup thread.
extern "C" __declspec(dllexport) void wsdraw_log_init() { InitializeCriticalSection(&g_lr_cs); g_lr_ok = true; }
extern "C" __declspec(dllexport) void wsdraw_log_push(const char* text) {
    if (!g_lr_ok || !text) return;
    EnterCriticalSection(&g_lr_cs);
    LONG idx = g_lr_total % LOG_RING;
    strncpy_s(g_lr[idx], LOG_COLS, text, _TRUNCATE);
    g_lr_total++;
    // Following the tail: stay there. Scrolled back: hold position relative to the line you were reading,
    // so new output does not yank the view out from under you mid-read.
    if (g_lr_scroll > 0 && g_lr_scroll < LOG_RING) g_lr_scroll++;
    LeaveCriticalSection(&g_lr_cs);
}

static const char* const LR_FILTERS[] = { "all", "sync", "errors", "properties" };
static bool lr_passes(const char* s, LONG f) {
    switch (f) {
        case 1: return strstr(s, ">>>") || strstr(s, "<<<") || strstr(s, "<detect>") || strstr(s, "[psync]");
        case 2: return strstr(s, "!!!") || strstr(s, "FAIL") || strstr(s, "REFUS") || strstr(s, "EXC")
                    || strstr(s, "CRASH") || strstr(s, "***") || strstr(s, "BLOCKED");
        case 3: return strstr(s, "[psync]") || strstr(s, "[pdiff]") || strstr(s, "[apply]")
                    || strstr(s, "[splice]") || strstr(s, "[head]") || strstr(s, "[idx]");
        default: return true;
    }
}
// Colour by meaning, not by severity alone - at speed you are asking "did it send / did it arrive / did it
// refuse", and three distinguishable colours answer that faster than reading the words.
static void lr_colour(const char* s, float* r, float* g, float* b) {
    if (strstr(s, "!!!") || strstr(s, "FAIL") || strstr(s, "CRASH") || strstr(s, "REFUS") || strstr(s, "EXC"))
         { *r=1.00f; *g=0.38f; *b=0.34f; return; }               // refused / broke
    if (strstr(s, "***"))                { *r=1.00f; *g=0.82f; *b=0.35f; return; }   // a verdict line
    if (strstr(s, ">>>"))                { *r=0.45f; *g=0.82f; *b=1.00f; return; }   // we sent
    if (strstr(s, "<<<"))                { *r=0.45f; *g=0.95f; *b=0.60f; return; }   // we received
    if (strstr(s, "[psync]"))            { *r=0.80f; *g=0.70f; *b=1.00f; return; }   // property sync
    if (strstr(s, "<detect>"))           { *r=0.70f; *g=0.90f; *b=0.80f; return; }   // local edit detected
    *r=0.64f; *g=0.68f; *b=0.74f;                                                    // everything else
}

static void draw_log_view(int sw, int sh) {
    if (!InterlockedCompareExchange(&g_logview, 0, 0) || !g_lr_ok) return;

    const int PX = 2, CH = FONT_ADV*2, ROW = FONT_ROWS*2 + 4;   // 5x7 glyph: 12 wide, 18 per row
    const int PAD = 10, HEAD = 20;
    // Top-left, in the same column as the status panel - and when BOTH are open the log starts below it, so
    // the panel stays readable. It also draws before the menu, so any overlap resolves in the menu's favour.
    const int X = MENU_X;
    const int Y = MENU_Y + (InterlockedCompareExchange(&g_menu, 0, 0) ? MENU_H + 6 : 0);
    int W = sw - X*2; if (W > 1200) W = 1200;
    int rows = (sh - Y - 24 - PAD*2 - HEAD) / ROW;
    if (rows > 40) rows = 40;                   // a wall of text is harder to read than a window onto it
    if (rows < 4)  rows = 4;
    const int ROWS = rows;
    const int H = PAD*2 + HEAD + ROWS*ROW;

    quad((float)X, (float)Y, (float)W, (float)H, 0.02f, 0.03f, 0.05f, 0.90f);
    quad((float)X, (float)Y, (float)W, 1.5f,     0.35f, 0.72f, 1.00f, 0.55f);

    // Snapshot under the lock, then draw - GL calls must not happen with the log lock held, or a logging
    // thread stalls for a whole frame.
    static char view[256][LOG_COLS];
    int nview = 0; LONG total, scroll, filter;
    EnterCriticalSection(&g_lr_cs);
    total = g_lr_total; scroll = g_lr_scroll; filter = g_lr_filter;
    int have = total < LOG_RING ? (int)total : LOG_RING;
    // Collect matching lines newest-first, skipping `scroll` of them, until the page is full.
    int skipped = 0;
    for (int k = 1; k <= have && nview < ROWS && nview < 256; k++) {
        const char* s = g_lr[(total - k) % LOG_RING];
        if (!lr_passes(s, filter)) continue;
        if (skipped < scroll) { skipped++; continue; }
        strncpy_s(view[nview++], LOG_COLS, s, _TRUNCATE);
    }
    LeaveCriticalSection(&g_lr_cs);

    char head[160];
    _snprintf_s(head, sizeof head, _TRUNCATE,
                "LOG  [%s]   %ld lines%s    F8 close   PgUp/PgDn scroll   Home/End jump   F2 filter",
                LR_FILTERS[filter % 4], total, scroll ? "   (scrolled back - End to follow)" : "   (live)");
    draw_text_px(X + PAD, Y + PAD - 2, 0.55f, 0.88f, 1.0f, head, PX);

    // Newest at the bottom, so it reads like a terminal.
    for (int i = 0; i < nview; i++) {
        float r, g, b; lr_colour(view[i], &r, &g, &b);
        int y = Y + PAD + HEAD + (nview - 1 - i) * ROW;
        // Clip rather than wrap: a wrapped line costs two rows and the interesting part of every line in
        // this log is at the front.
        char cut[LOG_COLS];
        int maxc = (W - PAD*2) / CH; if (maxc > LOG_COLS - 1) maxc = LOG_COLS - 1;
        strncpy_s(cut, LOG_COLS, view[i], _TRUNCATE);
        if ((int)strlen(cut) > maxc) cut[maxc] = 0;
        draw_text_px(X + PAD, y, r, g, b, cut, PX);
    }
    if (!nview)
        draw_text_px(X + PAD, Y + PAD + HEAD, 0.5f, 0.5f, 0.55f, "(no lines match this filter - F2 to change)", PX);
}

// Key handling. The scroll keys are only consumed while the viewer is open, so they stay available to the
// game the rest of the time.
static void log_view_keys() {
    static SHORT p8 = 0; SHORT k8 = GetAsyncKeyState(VK_F8);
    if ((k8 & 0x8000) && !(p8 & 0x8000)) InterlockedExchange(&g_logview, !g_logview);
    p8 = k8;
    if (!InterlockedCompareExchange(&g_logview, 0, 0)) return;

    const int PAGE = 14;
    struct { int vk; int delta; } nav[] = { { VK_PRIOR, +PAGE }, { VK_NEXT, -PAGE },
                                            { VK_UP, +1 }, { VK_DOWN, -1 } };
    static SHORT pv[4] = {0,0,0,0};
    for (int i = 0; i < 4; i++) {
        SHORT k = GetAsyncKeyState(nav[i].vk);
        // Repeat while held for the page keys, edge-triggered for the line keys - holding PageDown to get
        // back to the tail is the common motion.
        bool fire = (nav[i].vk == VK_PRIOR || nav[i].vk == VK_NEXT) ? ((k & 0x8000) != 0)
                                                                    : ((k & 0x8000) && !(pv[i] & 0x8000));
        if (fire) {
            LONG s = g_lr_scroll + nav[i].delta;
            if (s < 0) s = 0; if (s > LOG_RING - 1) s = LOG_RING - 1;
            InterlockedExchange(&g_lr_scroll, s);
        }
        pv[i] = k;
    }
    static SHORT ph = 0; SHORT kh = GetAsyncKeyState(VK_HOME);
    if ((kh & 0x8000) && !(ph & 0x8000)) InterlockedExchange(&g_lr_scroll, LOG_RING - 1);
    ph = kh;
    static SHORT pe = 0; SHORT ke = GetAsyncKeyState(VK_END);
    if ((ke & 0x8000) && !(pe & 0x8000)) InterlockedExchange(&g_lr_scroll, 0);
    pe = ke;
    static SHORT pf = 0; SHORT kf = GetAsyncKeyState(VK_F2);
    if ((kf & 0x8000) && !(pf & 0x8000)) {
        InterlockedExchange(&g_lr_filter, (g_lr_filter + 1) % 4);
        InterlockedExchange(&g_lr_scroll, 0);       // a new filter means a new list; start at the tail
    }
    pf = kf;
}

// A remote player's camera, drawn as a frustum in world space + a drop line to the ground
// so it stays locatable. This is THE peer-presence marker; only the transport is missing.
static void draw_peer_camera(const CamPose& p, const char* label, float r, float g, float b) {
    const float D = 0.9f, W = 0.55f, H = 0.32f;      // frustum depth / half-width / half-height
    float ap[3] = { p.pos[0], p.pos[1], p.pos[2] };
    float c[4][3];
    for (int i = 0; i < 4; i++) {
        const float sw = (i == 0 || i == 3) ? -W : W;
        const float sh = (i < 2) ? H : -H;
        for (int k = 0; k < 3; k++)
            c[i][k] = ap[k] + p.fwd[k]*D + p.right[k]*sw + p.up[k]*sh;
    }
    glLineWidth(2.5f);
    glColor4f(r, g, b, 0.95f);
    glBegin(GL_LINES);
    for (int i = 0; i < 4; i++) {
        world_line(ap[0], ap[1], ap[2], c[i][0], c[i][1], c[i][2]);
        const int j = (i + 1) & 3;
        world_line(c[i][0], c[i][1], c[i][2], c[j][0], c[j][1], c[j][2]);
    }
    // a stalk down to y=0 so the marker can be found even when it is off in the distance
    glColor4f(r*0.6f, g*0.6f, b*0.6f, 0.55f);
    world_line(ap[0], ap[1], ap[2], ap[0], 0.0f, ap[2]);
    glEnd();
    // orientation tick: a short spike along +up from the frustum's top edge
    glColor4f(r, g, b, 0.9f);
    glBegin(GL_LINES);
    float tm[3], tp[3];
    for (int k = 0; k < 3; k++) { tm[k] = (c[0][k] + c[1][k]) * 0.5f; tp[k] = tm[k] + p.up[k] * 0.25f; }
    world_line(tm[0], tm[1], tm[2], tp[0], tp[1], tp[2]);
    glEnd();
    float sx, sy;
    if (project(g_best, ap[0], ap[1], ap[2], &sx, &sy))
        draw_text_px((int)sx + 10, (int)sy - 6, r, g, b, label, 2);
}

static void draw(HDC hdc) {
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    g_W = vp[2]; g_H = vp[3];
    if (g_W <= 0 || g_H <= 0) return;
    GLint prevProg = 0, prevVao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao); glGetError();
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, g_W, g_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    if (p_glUseProgram)      p_glUseProgram(0);
    if (p_glBindVertexArray) p_glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glLineWidth(2.0f);

    char line[160];
    const bool hud      = InterlockedCompareExchange(&g_hud, 0, 0) != 0;
    const bool haveBest = InterlockedCompareExchange(&g_have_best, 0, 0) != 0;

    // AUTO-CALIBRATE to the current bench. g_off used to be hand-dialled with the arrow keys and saved to
    // wsdraw-cfg.txt, so it was correct for exactly ONE bench - at any other bench every world-space marker
    // drew in the wrong place (kilometres off, if the bench was on another island). vehicle+0x1F0 gives the
    // world position of voxel (0,0,0) directly. Manual nudging still wins while g_off_dirty is set, so the
    // arrow keys remain usable for verification.
    // ...but ONLY if it is in the same coordinate space the renderer uses. Stormworks has a FLOATING ORIGIN:
    // the render/camera space is rebased near the player, while vehicle+0x1F0 is ABSOLUTE world. Near the
    // world origin (the starter bench) the two coincide, which is why this looked right there - at a distant
    // bench they differ by kilometres and every marker lands off-screen. The editor camera orbits the craft,
    // so if the bench origin is nowhere near the camera the two spaces disagree and we must not use it.
    if (InterlockedCompareExchange(&g_bench_org_valid,0,0)
        && InterlockedCompareExchange(&g_cam_world_valid,0,0) && !g_off_dirty) {
        // Derive the rebase R by SUBTRACTION rather than by applying the game's rounding rule: both terms
        // are the SAME camera, one in absolute world and one decoded from the MVP, so their difference IS
        // the rebase. The whole-km snap absorbs the orbit radius and assumes nothing about tile size or
        // rounding mode. Residual should be ~0 - if it is not, g_best is not the main camera pass.
        auto snap1000 = [](double v){ return 1000.0 * floor(v/1000.0 + 0.5); };
        const double Rx = snap1000(g_cam_world[0] - (double)g_cam[0]);
        const double Ry = snap1000(g_cam_world[1] - (double)g_cam[1]);
        const double Rz = snap1000(g_cam_world[2] - (double)g_cam[2]);
        const float ox=(float)(g_bench_org[0]-Rx), oy=(float)(g_bench_org[1]-Ry), oz=(float)(g_bench_org[2]-Rz);
        // sanity: the bench must now land near the camera that is looking at it
        const float dx=ox-g_cam[0], dy=oy-g_cam[1], dz=oz-g_cam[2];
        if (dx*dx+dy*dy+dz*dz < 1000.0f*1000.0f) { g_off[0]=ox; g_off[1]=oy; g_off[2]=oz; }
        else { static DWORD s_lw=0; DWORD nowt=GetTickCount();
               if (nowt-s_lw>10000){ s_lw=nowt;
                   logline("calib: rebase R=(%.0f,%.0f,%.0f) gives off=(%.1f,%.1f,%.1f) but camera is (%.1f,%.1f,%.1f) - rejected",
                           Rx,Ry,Rz, ox,oy,oz, g_cam[0],g_cam[1],g_cam[2]); } }
    }

    // THE FEATURE: draw the partner's camera marker whenever a fresh pose has arrived over
    // Steam. Always shown (independent of the HUD toggle) so you can always see your partner.
    bool peerFresh = false;
    if (haveBest) {
        const DWORD peerAge = GetTickCount() - g_peer_last;
        CamPose peerSnap;
        peerFresh = InterlockedCompareExchange(&g_have_peer, 0, 0) && peerAge < 2000
                    && read_pose(g_peer, &g_peer_seq, peerSnap);   // seqlock: stable copy
        if (peerFresh) draw_peer_camera(peerSnap, "PARTNER", PARTNER_R, PARTNER_G, PARTNER_B);
    }
    // PARTNER CURSOR: the voxel cell they are hovering. Drawn deliberately LARGER than one voxel - a 0.25 m
    // wire cube on a 35 m craft is only a few pixels and is impossible to spot - plus a label so it can be
    // found even against busy block geometry. (All of this is screen-space projected, so nothing occludes it.)
    if (haveBest && InterlockedCompareExchange(&g_peer_cur_valid,0,0)
        && (GetTickCount() - g_peer_cur_last) < 1500) {
        const float s = g_scale;
        // Blocks are CENTRED ON their voxel: the game's bounds test is v +/- 0.5 and the bench origin
        // (vehicle+0x1F0) is the centre of voxel (0,0,0). So the cell centre is v*0.25 + origin exactly -
        // adding half a voxel here shifted the marker half a block off. The half-voxel is the EXTENT below.
        const float cx = g_peer_cur_vx*s + g_off[0];
        const float cy = g_peer_cur_vy*s + g_off[1];
        const float cz = g_peer_cur_vz*s + g_off[2];
        const float h  = s*0.6f;                                    // just over one voxel: outlines the cell
        const float X0=cx-h, X1=cx+h, Y0=cy-h, Y1=cy+h, Z0=cz-h, Z1=cz+h;
        const float bx[8]={X0,X1,X1,X0, X0,X1,X1,X0};
        const float by[8]={Y0,Y0,Y1,Y1, Y0,Y0,Y1,Y1};
        const float bz[8]={Z0,Z0,Z0,Z0, Z1,Z1,Z1,Z1};
        float pxs[8], pys[8]; bool okc[8];
        for (int i=0;i<8;i++) okc[i]=project(g_best,bx[i],by[i],bz[i],&pxs[i],&pys[i]);
        static const int EE[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        glColor4f(PARTNER_R,PARTNER_G,PARTNER_B,0.95f);
        glBegin(GL_LINES);
        for (int e=0;e<12;e++){ int a=EE[e][0],c=EE[e][1]; if(!okc[a]||!okc[c]) continue;
            glVertex2f(pxs[a],pys[a]); glVertex2f(pxs[c],pys[c]); }
        glEnd();
    }

    // SYNC BANNER: a pull CLEARS the local craft before rebuilding it, so without this it looks like the mod
    // just deleted your work. Centre screen, unmissable, with real chunk progress.
    const long syncErrAt = InterlockedCompareExchange(&g_sync_err,0,0);
    const bool syncErr = syncErrAt && ((long)GetTickCount() - syncErrAt) < 5000;
    // A confirmation prompt is amber, not red, and only lives as long as the 3s window it is asking about -
    // lingering after the window closes would invite a press that no longer counts.
    const long syncAskAt = InterlockedCompareExchange(&g_sync_ask,0,0);
    const bool syncAsk = syncAskAt && ((long)GetTickCount() - syncAskAt) < 3000;
    if (InterlockedCompareExchange(&g_sync_busy,0,0) || syncErr || syncAsk) {
        const int BW=380, BH=64, BX=(g_W-BW)/2, BY=(g_H-BH)/2 - 40;
        glColor4f(0.05f,0.07f,0.11f,0.86f);
        glBegin(GL_QUADS);
          glVertex2f((float)BX,(float)BY);         glVertex2f((float)(BX+BW),(float)BY);
          glVertex2f((float)(BX+BW),(float)(BY+BH)); glVertex2f((float)BX,(float)(BY+BH));
        glEnd();
        if (syncErr)      glColor4f(1.00f,0.45f,0.35f,0.95f);   // failure  - red
        else if (syncAsk) glColor4f(1.00f,0.80f,0.35f,0.95f);   // question - amber
        else              glColor4f(0.55f,0.90f,1.00f,0.95f);   // progress - blue
        glBegin(GL_LINE_LOOP);
          glVertex2f((float)BX,(float)BY);         glVertex2f((float)(BX+BW),(float)BY);
          glVertex2f((float)(BX+BW),(float)(BY+BH)); glVertex2f((float)BX,(float)(BY+BH));
        glEnd();
        g_px = 2;
        if (syncErr) {
            draw_text(BX+16, BY+14, 1.0f,0.45f,0.35f, "SYNC FAILED");
            draw_text(BX+16, BY+36, 0.90f,0.85f,0.85f, g_sync_err_msg);
        } else if (syncAsk) {
            draw_text(BX+16, BY+14, 1.0f,0.80f,0.35f, "ARE YOU SURE?");
            draw_text(BX+16, BY+36, 0.92f,0.88f,0.80f, g_sync_err_msg);
        } else {
            draw_text(BX+16, BY+14, 0.55f,0.90f,1.0f, "SYNCING WITH PARTNER");
            const long tot=InterlockedCompareExchange(&g_sync_total,0,0), got=InterlockedCompareExchange(&g_sync_got,0,0);
            char sl[96];
            if (tot > 0) sprintf_s(sl,sizeof sl,"receiving craft   %ld / %ld KB", got/1024, tot/1024);
            else         sprintf_s(sl,sizeof sl,"requesting craft from partner...");
            draw_text(BX+16, BY+36, 0.85f,0.90f,0.95f, sl);
        }
    }

    // COOP MENU: always-on status panel (F6 hides it). Top-left, above the calibration HUD.
    draw_boot_sequence(g_W, g_H);
    draw_log_view(g_W, g_H);

    // TOAST. A whole-craft reload tears down and rebuilds every component, and until now it was completely
    // invisible - "I cannot even tell if a reload is happening". That is bad on one machine and worse on two,
    // where "did my F7 do anything?" is the question you ask constantly. Brief, centred, fades itself out.
    if (g_toast_at) {
        DWORD age = GetTickCount() - g_toast_at;
        if (age > 2600) g_toast_at = 0;
        else {
            float f = age < 160 ? (float)age / 160.0f
                                : (age > 2000 ? 1.0f - (float)(age - 2000) / 600.0f : 1.0f);
            // Top-left, in the same column as everything else, tucked under the F6 panel when that is open.
            // Centre-screen is where the game's own messages live and it sits right over the build area.
            int w = (int)strlen((const char*)g_toast) * FONT_ADV * 2 + 28;
            int x = MENU_X;
            int y = MENU_Y + (InterlockedCompareExchange(&g_menu, 0, 0) ? MENU_H + 6 : 0)
                    + (int)((1.0f - f) * 8.0f);
            quad((float)x, (float)y, (float)w, 26.0f, 0.05f, 0.09f, 0.13f, 0.86f * f);
            quad((float)x, (float)y, (float)w, 1.6f, 0.35f, 0.80f, 1.0f, 0.85f * f);
            draw_text_px(x + 14, y + 6, 0.60f*f, 0.90f*f, 1.0f*f, (const char*)g_toast, 2);
        }
    }

    // SELF-TEST BANNER. Permanent, top of screen, while local-echo is on. This is not decoration: the file
    // that enables it was found still present in a live install, and in that state the mod sends nothing to
    // anyone while looking like it works.
    if (g_selftest_on) {
        const int BW = 560, BH = 26, BX = (g_W - BW) / 2;
        float pulse = 0.55f + 0.30f * (float)fabs(sin((double)GetTickCount() * 0.0022));
        quad((float)BX, 0.0f, (float)BW, (float)BH, 0.35f, 0.05f, 0.05f, 0.88f);
        quad((float)BX, (float)(BH - 2), (float)BW, 2.0f, 1.0f, 0.30f, 0.25f, pulse);
        draw_text_px(BX + 14, 6, 1.0f, 0.55f, 0.45f, "SELF-TEST MODE - NOT SENDING TO A PARTNER", 2);
    }

    if (InterlockedCompareExchange(&g_menu, 0, 0)) {
        g_px = 2;
        const int MX=MENU_X, MY=MENU_Y, MROW=MENU_ROW, MW=MENU_W, MH=MENU_H;
        glColor4f(0.05f, 0.07f, 0.11f, 0.82f);
        glBegin(GL_QUADS);
          glVertex2f((float)MX,(float)MY);          glVertex2f((float)(MX+MW),(float)MY);
          glVertex2f((float)(MX+MW),(float)(MY+MH)); glVertex2f((float)MX,(float)(MY+MH));
        glEnd();
        int mr=0;
        #define MROWY (MY + 6 + (mr++)*MROW)
        // Build stamp on screen, not just in the log: several builds can share a version within one session,
        // and "which DLL am I actually running" has cost real testing time.
        { static char s_title[128]; static bool s_made = false;
          if (!s_made) { const char hhmm[] = COOP_BUILD_HHMM;
              _snprintf_s(s_title, sizeof s_title, _TRUNCATE,
                          "COOP WORKBENCH by BAYNEBUILD  %s b%s  EXPERIMENTAL", COOP_VERSION, hhmm);
              s_made = true; }
          draw_text(MX+8, MROWY, 0.55f, 0.90f, 1.0f, s_title); }
        const bool live = InterlockedCompareExchange(&g_net_ok,0,0)!=0;
        if      (!g_peerid) draw_text(MX+8, MROWY, 1.0f,0.70f,0.30f, "Link:    no partner set");
        else if (live)      draw_text(MX+8, MROWY, 0.3f,1.00f,0.50f, "Link:    LIVE");
        else                draw_text(MX+8, MROWY, 1.0f,0.85f,0.30f, "Link:    connecting...");
        // NEVER put a SteamID64 on screen: screenshots and video of the overlay would leak the partner's id
        // (and the whole point of the overlay is that it is visible in footage). The id stays in the log,
        // which testers are told to scrub before sharing.
        draw_text(MX+8, MROWY, 0.80f,0.85f,0.95f,
                  g_peerid ? "Partner: connected" : "Partner: none (set coop-peer.txt)");
        if      (g_bench_mismatch) draw_text(MX+8, MROWY, 1.0f,0.35f,0.30f, "!! WRONG BENCH - SYNC BLOCKED");
        else if (g_peer_in_bench)  draw_text(MX+8, MROWY, 0.3f,1.00f,0.50f, "Partner: IN THE WORKBENCH");
        else if (peerFresh)        draw_text(MX+8, MROWY, 0.3f,1.00f,0.50f, "Partner cam: visible");
        else                       draw_text(MX+8, MROWY, 0.6f,0.65f,0.70f, "Partner: away (sync paused)");
        draw_text(MX+8, MROWY, 0.55f,0.90f,1.0f, "[F7] LOAD PARTNER CRAFT   [F6] hide");
        draw_text(MX+8, MROWY, 0.7f,0.75f,0.85f, "[F4] load file [F5] save [F9] ovl [F10] cal");
        #undef MROWY
    }

    // The calibration/status HUD (top-left readouts + the dev lattice). Hidden by default;
    // F10 toggles it. Everything in here is for tuning/diagnostics, not normal play.
    if (hud) {
        const int HX = 8, HY = 100, ROW = 14, NROWS = 6;
        const int HW = 620, HH = NROWS * ROW + 10;
        g_px = 2;                                    // 8px per char cell
        glColor4f(0.05f, 0.06f, 0.09f, 0.85f);
        glBegin(GL_QUADS);
          glVertex2f((float)HX, (float)HY); glVertex2f((float)(HX+HW), (float)HY);
          glVertex2f((float)(HX+HW), (float)(HY+HH)); glVertex2f((float)HX, (float)(HY+HH));
        glEnd();
        int row = 0;
        #define HUDROW (HY + 5 + (row++) * ROW)
        if (haveBest) {
            if (g_grid) draw_world_grid();
            for (int i = 0; i < g_nboxes; i++)
                draw_voxel_box(g_boxes[i][0], g_boxes[i][1], g_boxes[i][2], 0.2f, 1.0f, 0.4f);
            for (int x = 0; x < g_span; x++) for (int y = 0; y < g_span; y++) for (int z = 0; z < g_span; z++)
                draw_voxel_box(x, y, z, 1.0f, 0.55f, 0.1f);
            float ox, oy;                                    // where our lattice origin lands
            if (project(g_best, g_off[0], g_off[1], g_off[2], &ox, &oy)) {
                glColor4f(1, 0.2f, 0.2f, 1);
                glBegin(GL_LINES);
                  glVertex2f(ox-16, oy); glVertex2f(ox+16, oy);
                  glVertex2f(ox, oy-16); glVertex2f(ox, oy+16);
                glEnd();
                draw_text((int)ox + 18, (int)oy - 8, 1, 0.4f, 0.4f, "lattice origin");
            }
            sprintf_s(line, "MVP LOCKED  hits=%ld  scale=%.4f m/voxel  span=%d",
                      g_best_hits, g_scale, g_span);
            draw_text(HX+6, HUDROW, 0.3f, 1.0f, 0.5f, line);
            sprintf_s(line, "cam %.2f %.2f %.2f  fwd %.2f %.2f %.2f",
                      g_cam[0], g_cam[1], g_cam[2], g_fwd[0], g_fwd[1], g_fwd[2]);
            draw_text(HX+6, HUDROW, 0.8f, 0.9f, 1.0f, line);
            sprintf_s(line, "OFFSET  %8.3f %8.3f %8.3f m", g_off[0], g_off[1], g_off[2]);
            draw_text(HX+6, HUDROW, 1.0f, 0.85f, 0.3f, line);
            sprintf_s(line, "     =  %8.2f %8.2f %8.2f voxels",
                      g_off[0]/g_scale, g_off[1]/g_scale, g_off[2]/g_scale);
            draw_text(HX+6, HUDROW, 1.0f, 0.7f, 0.2f, line);
            draw_text(HX+6, HUDROW, 0.6f, 0.7f, 0.8f,
                      "arrows=X/Z  pgup/pgdn=Y  shift=fast ctrl=fine  F7=bring  F8=save  F9=overlay  F10=hud");
            if (!g_peerid)
                draw_text(HX+6, HUDROW, 0.6f, 0.6f, 0.6f, "PEER: none (set coop-peer.txt)");
            else if (peerFresh)
                draw_text(HX+6, HUDROW, 0.3f, 1.0f, 0.5f, "PEER: PARTNER LIVE");
            else
                draw_text(HX+6, HUDROW, 1.0f, 0.8f, 0.3f, "PEER: waiting for partner");
        } else {
            draw_text(HX+6, HUDROW, 1.0f, 0.5f, 0.2f, "no rigid MVP candidate - is a craft on screen?");
            sprintf_s(line, "candidates=%d  haveProj=%ld", g_ncand,
                      InterlockedCompareExchange(&g_have_proj, 0, 0));
            draw_text(HX+6, HUDROW, 0.8f, 0.8f, 0.8f, line);
        }
        #undef HUDROW
    }

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
    if (p_glUseProgram      && prevProg) p_glUseProgram((GLuint)prevProg);
    if (p_glBindVertexArray && prevVao)  p_glBindVertexArray((GLuint)prevVao);
    glGetError();
}

// ---- hooks -----------------------------------------------------------------
static void** locate_slot(ULONGLONG base, void* realfn) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) continue;
        ULONGLONG s = base + sec[i].VirtualAddress, e = s + sec[i].Misc.VirtualSize;
        for (ULONGLONG a = (s + 7) & ~7ULL; a + 8 <= e; a += 8) {
            __try { if (*(void**)a == realfn) return (void**)a; }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        }
    }
    return nullptr;
}

static BOOL WINAPI my_SwapBuffers(HDC hdc) {
    InterlockedIncrement(&g_inhook);
    if (InterlockedCompareExchange(&g_want_exit, 0, 0)) {
        if (g_font_ready) { glDeleteLists(g_font_base, 256); g_font_ready = false; }
        InterlockedExchange(&g_gl_freed, 1);
        BOOL rr = g_orig_swap(hdc); InterlockedDecrement(&g_inhook); return rr;
    }
    if (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        __try {
            if (!g_ready) {
                g_real_um4 = (PFN_glUniformMatrix4fv)wglGetProcAddress("glUniformMatrix4fv");
                p_glUseProgram      = (PFN_glUseProgram)     wglGetProcAddress("glUseProgram");
                p_glBindVertexArray = (PFN_glBindVertexArray)wglGetProcAddress("glBindVertexArray");
                void** slot = g_real_um4 ? locate_slot((ULONGLONG)GetModuleHandleA(nullptr),
                                                       (void*)g_real_um4) : nullptr;
                if (slot) {
                    DWORD o;
                    if (VirtualProtect(slot, 8, PAGE_READWRITE, &o)) {
                        g_next_um4 = (PFN_glUniformMatrix4fv)*slot;
                        *slot = (void*)&my_glUniformMatrix4fv;
                        VirtualProtect(slot, 8, o, &o);
                        g_slot = slot; g_ready = true;
                        logline("uniform hook installed (chaining to %p)", (void*)g_next_um4);
                    }
                } else {
                    // RETRY, and NEVER stop the overlay over this. Injected into a running game the engine
                    // has long since cached this pointer and the first attempt succeeds. Loaded by an ASI
                    // loader we get here on the FIRST FRAME, before the game has drawn anything with a
                    // shader, so the pointer does not exist yet - and setting g_stop killed the entire
                    // overlay, menu included, for the whole session. That is what "no F6 overlay under ASI"
                    // was.
                    // Losing this hook costs only the PARTNER CAMERA MARKER, which needs the view matrix.
                    // The menu, the sync banner and the cursor do not, so they must keep working.
                    static long tries = 0;
                    if (++tries == 1)
                        logline("glUniformMatrix4fv not cached yet (normal at game start) - retrying per frame");
                    else if (tries == 1800) {
                        logline("glUniformMatrix4fv never appeared after 1800 frames - partner CAMERA marker "
                                "disabled; menu, banner and cursor still work");
                        g_um4_gave_up = true;
                    }
                }
                if (!g_ready && !g_um4_gave_up) { InterlockedDecrement(&g_inhook); return g_orig_swap(hdc); }
                load_cfg(); g_cfg_last = GetTickCount();
            }
            g_frames++;
            if (GetTickCount() - g_cfg_last > 1000) { load_cfg(); g_cfg_last = GetTickCount(); }
            select_best();
            for (int i = 0; i < g_ncand; i++) g_chits[i] = 0;   // hits are per-frame
            g_ncand = 0;
            static SHORT prev = 0; SHORT now = GetAsyncKeyState(VK_F9);
            if ((now & 0x8000) && !(prev & 0x8000)) InterlockedExchange(&g_visible, !g_visible);
            prev = now;
            static SHORT prevH = 0; SHORT nowH = GetAsyncKeyState(VK_F10);   // F10 = show/hide the HUD readouts
            if ((nowH & 0x8000) && !(prevH & 0x8000)) InterlockedExchange(&g_hud, !g_hud);
            prevH = nowH;
            log_view_keys();                                                 // F8 = in-game log
            static SHORT prevM = 0; SHORT nowM = GetAsyncKeyState(VK_F6);    // F6 = show/hide the coop menu
            if ((nowM & 0x8000) && !(prevM & 0x8000)) InterlockedExchange(&g_menu, !g_menu);
            prevM = nowM;
            // live offset nudge - drive the lattice onto the craft instead of guessing the
            // body transform. Only while the game has focus, so we never eat someone else's keys.
            // CALIBRATION KEYS - only while the F10 calibration HUD is actually up. They used to be live at
            // all times, which collided with everything: the arrows and PgUp/PgDn are the log viewer's scroll
            // keys, F8 opens the log, and F7 - the whole-craft PULL - also teleported the lattice on every
            // press. Tying them to the HUD that displays their result is both the fix and the obvious rule.
            if (InterlockedCompareExchange(&g_hud, 0, 0) && GetForegroundWindow() == WindowFromDC(hdc)) {
                // Rate is per SECOND, not per frame. At ~200fps a per-frame step flew off
                // the map instantly; this is frame-rate independent.
                static DWORD s_last = 0;
                DWORD nowt = GetTickCount();
                float dt = s_last ? (nowt - s_last) / 1000.0f : 0.016f;
                s_last = nowt;
                if (dt > 0.1f) dt = 0.1f;                 // ignore hitches/alt-tab gaps
                float speed = 1.0f;                       // m/s
                if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) speed = 8.0f;    // coarse
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) speed = 0.15f;   // fine
                const float step = speed * dt;
                bool moved = false;
                if (GetAsyncKeyState(VK_LEFT)  & 0x8000) { g_off[0] -= step; moved = true; }
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { g_off[0] += step; moved = true; }
                if (GetAsyncKeyState(VK_UP)    & 0x8000) { g_off[2] -= step; moved = true; }
                if (GetAsyncKeyState(VK_DOWN)  & 0x8000) { g_off[2] += step; moved = true; }
                if (GetAsyncKeyState(VK_PRIOR) & 0x8000) { g_off[1] += step; moved = true; }
                if (GetAsyncKeyState(VK_NEXT)  & 0x8000) { g_off[1] -= step; moved = true; }
                if (moved) g_off_dirty = true;
                // F7 = teleport the lattice 5m in front of the camera, so it is always findable
                static SHORT pf7 = 0; SHORT f7 = GetAsyncKeyState(VK_F7);
                if ((f7 & 0x8000) && !(pf7 & 0x8000) && InterlockedCompareExchange(&g_have_best,0,0)) {
                    for (int i = 0; i < 3; i++) g_off[i] = g_cam[i] + g_fwd[i] * 5.0f;
                    g_off_dirty = true;
                    logline("F7 -> lattice moved to %.2f %.2f %.2f", g_off[0], g_off[1], g_off[2]);
                }
                pf7 = f7;
                static SHORT pf8 = 0; SHORT f8 = GetAsyncKeyState(VK_F8);
                if ((f8 & 0x8000) && !(pf8 & 0x8000)) {
                    save_cfg(); g_off_dirty = false;      // file now matches; reload is safe again
                    logline("SAVED offset = %.3f %.3f %.3f  scale = %.5f  (camera was %.2f %.2f %.2f)",
                            g_off[0], g_off[1], g_off[2], g_scale, g_cam[0], g_cam[1], g_cam[2]);
                }
                pf8 = f8;
            }
            if (InterlockedCompareExchange(&g_visible, 0, 0)) draw(hdc);
            if (g_frames == 90 && InterlockedCompareExchange(&g_have_best, 0, 0))
                logline("locked MVP: hits=%ld cam=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f)",
                        g_best_hits, g_cam[0], g_cam[1], g_cam[2], g_fwd[0], g_fwd[1], g_fwd[2]);
            // Dump every rigid candidate periodically. If one of these is the craft body's
            // model-view, (its cam) - (locked cam) is the exact body offset, no eyeballing.
            if ((g_frames % 600) == 0 && g_nrigid_log > 0) {
                logline("--- rigid candidates @frame %u  (locked cam %.3f %.3f %.3f) ---",
                        g_frames, g_cam[0], g_cam[1], g_cam[2]);
                for (int i = 0; i < g_nrigid_log; i++)
                    logline("   hits=%-6ld cam=(%9.3f %9.3f %9.3f)  delta-from-locked=(%8.3f %8.3f %8.3f)",
                            g_rigid[i].hits, g_rigid[i].cam[0], g_rigid[i].cam[1], g_rigid[i].cam[2],
                            g_rigid[i].cam[0]-g_cam[0], g_rigid[i].cam[1]-g_cam[1], g_rigid[i].cam[2]-g_cam[2]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logline("EXCEPTION at frame %u - drawing disabled", g_frames);
            InterlockedExchange(&g_stop, 1);
        }
    }
    BOOL r = g_orig_swap(hdc);
    InterlockedDecrement(&g_inhook);
    return r;
}

static void** find_iat_slot(ULONGLONG b, const char* dll, const char* fn) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + rva);
    for (; imp->Name; imp++) {
        if (_stricmp((const char*)(b + imp->Name), dll)) continue;
        DWORD oftRVA = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(b + oftRVA);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(b + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            IMAGE_IMPORT_BY_NAME* n = (IMAGE_IMPORT_BY_NAME*)(b + oft->u1.AddressOfData);
            if (!strcmp((const char*)n->Name, fn)) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}

// ======================= STEAM P2P TRANSPORT (peer camera) =======================
// Self-contained, so wsdraw never has to touch coop.cpp (a parallel session owns it).
// Reuses the game's already-initialised steam_api64.dll, exactly like coopworkbench.dll, but on
// its OWN channel so the two DLLs' traffic never mixes. Peer SteamID64 comes from the same
// coop-peer.txt the co-op mod uses, so the user configures a partner only once.
//
// SESSION-SHARING SAFETY: ISteamNetworkingMessages sessions are per-identity, not per-channel,
// so coopworkbench.dll and wsdraw share ONE session with the peer. We therefore ACCEPT and KEEPALIVE
// (both safe/idempotent) but NEVER CloseSessionWithUser while coopworkbench.dll is loaded - closing
// would tear down coop's live block-sync link. Standalone (no coop), we may close to recover.
typedef void*    (*ifaceAccessor_t)();
typedef uint64_t (*getSteamID_t)(void*);
typedef int      (*sendToUser_t)(void*, const void*, const void*, uint32_t, int, int);
typedef int      (*recvOnChannel_t)(void*, int, void**, int);
typedef bool     (*acceptSession_t)(void*, const void*);
typedef void     (*identClear_t)(void*);
typedef void     (*identSetID_t)(void*, uint64_t);
typedef void     (*msgRelease_t)(void*);
typedef void     (*initRelay_t)(void*);
typedef void     (*closeSession_t)(void*, const void*);

static sendToUser_t    p_send    = nullptr;
static recvOnChannel_t p_recv    = nullptr;
static acceptSession_t p_accept  = nullptr;
// File scope, not locals in net_init: wsdraw_set_peer needs them to build the identity when the partner is
// discovered AFTER init (auto-connect).
static identClear_t    p_identClear = nullptr;
static identSetID_t    p_identSetID = nullptr;
static msgRelease_t    p_release = nullptr;
static closeSession_t  p_close   = nullptr;
static void*    g_net = nullptr;
static uint64_t g_myid = 0;   // g_peerid is declared up top (draw() reads it)
static BYTE     g_peerIdent[144];
static bool     g_coop_present = false;     // if so, never close the shared session
static char     g_peerpath[MAX_PATH];

static const int WS_SEND_RELIABLE = 8, WS_SEND_UNRELIABLE = 0, WS_CHANNEL = 7;  // coop uses 0
#define WS_MAGIC 0x50445357u   // 'WSDP'

#pragma pack(push,1)
struct PoseMsg {
    uint32_t magic; uint8_t ver; uint8_t kind;   // kind 1 = camera pose, 0 = keepalive/ignore, 2 = left bench
    float pos[3], fwd[3], up[3], right[3];
};
struct CursorMsg {                               // kind 3 = hovered voxel (partner cursor marker)
    uint32_t magic; uint8_t ver; uint8_t kind;
    uint16_t flags;                              // bit0 = valid
    int32_t  vx, vy, vz;
    uint32_t tool;
};
#pragma pack(pop)
// SOLO SELF-TEST (F8): replay OUR OWN cursor back as the "partner" cursor, delayed ~1s, so the whole
// capture -> voxel -> world -> render path can be validated on ONE machine (same trick as the old delayed
// camera ghost). Purely local - no network involved; the wire hop is already proven by the camera pose.
extern "C" volatile long g_cursor_selftest = 0;   // RETIRED - the partner cursor is confirmed on two
                                                  // machines, so nothing fakes a partner any more.
#define CURBUF 128
static struct { DWORD t; int vx,vy,vz; } g_curbuf[CURBUF];
static int g_curbuf_n = 0;

static bool steam_init() {
    HMODULE s = GetModuleHandleA("steam_api64.dll");
    if (!s) { logline("net: steam_api64.dll not loaded - peer camera disabled"); return false; }
    auto pUser  = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamUser_v023");
    auto pGetID = (getSteamID_t)GetProcAddress(s, "SteamAPI_ISteamUser_GetSteamID");
    auto pNet   = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    p_send    = (sendToUser_t)   GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    p_recv    = (recvOnChannel_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    p_accept  = (acceptSession_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    p_release = (msgRelease_t)   GetProcAddress(s, "SteamAPI_SteamNetworkingMessage_t_Release");
    p_close   = (closeSession_t) GetProcAddress(s, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    auto pClear = p_identClear = (identClear_t) GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_Clear");
    auto pSetID = p_identSetID = (identSetID_t) GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    if (!pNet||!p_send||!p_recv||!p_accept||!p_release||!pClear||!pSetID||!pUser||!pGetID) {
        logline("net: missing steam exports - peer camera disabled"); return false;
    }
    g_net  = pNet();
    g_myid = pGetID(pUser());
    auto pUtils = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    if (!pUtils) pUtils = (ifaceAccessor_t)GetProcAddress(s, "SteamAPI_SteamNetworkingUtils_SteamAPI_v003");
    auto pInit = (initRelay_t)GetProcAddress(s, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if (pUtils && pInit) pInit(pUtils());

    g_peerid = 0;
    FILE* pf = nullptr;
    if (!fopen_s(&pf, g_peerpath, "r") && pf) {
        char t[64] = {0}; if (fgets(t, sizeof t, pf)) { uint64_t v = _strtoui64(t, nullptr, 10); if (v) g_peerid = v; }
        fclose(pf);
    }
    // wsdraw.cpp is compiled INTO the same binary as coop, so this is really "is coop in this process" -
    // and it is always true. Asking by FILENAME breaks under an ASI install, where the module is called
    // coopworkbench.asi: both lookups return null, and the send-error path below then closes the shared
    // per-identity Steam session, which the comment there calls catastrophic. Ask the linker, not the file.
    g_coop_present = true;
    logline("net: our=%llu peer=%llu channel=%d coopworkbench.dll=%s %s",
            (unsigned long long)g_myid, (unsigned long long)g_peerid, WS_CHANNEL,
            g_coop_present ? "present (will NOT close sessions)" : "absent",
            g_peerid ? "" : "(no coop-peer.txt -> no partner configured)");
    if (g_peerid) { pClear(g_peerIdent); pSetID(g_peerIdent, g_peerid); p_accept(g_net, g_peerIdent); }
    return true;
}

// Late peer adoption. The overlay used to learn the partner ONLY from coop-peer.txt at init, so an
// auto-connected session (which discovers the partner a moment AFTER inject, and where the launcher has
// deliberately emptied coop-peer.txt) left the overlay on peer=0 forever: HUD stuck on "Partner: none",
// no partner camera, no partner cursor - while coop's own sync worked, which is exactly the confusing
// half-working state seen in the 2026-07-30 two-machine test. coop::adopt_peer now calls this.
// Idempotent, and never overrides an id we already have (coop applies the same rule).
extern "C" __declspec(dllexport) void wsdraw_set_peer(unsigned long long id) {
    if (!id || id == g_myid || g_peerid) return;
    g_peerid = id;
    if (p_identClear && p_identSetID && p_accept && g_net) { p_identClear(g_peerIdent); p_identSetID(g_peerIdent, g_peerid); p_accept(g_net, g_peerIdent); }
    logline("net: partner adopted late (auto-connect) -> peer=%llu, camera/cursor link enabled",
            (unsigned long long)g_peerid);
}

static void net_send_pose() {
    if (!p_send || !g_net || !g_peerid) return;
    // Broadcast our camera ONLY while we are actually in the workbench. A partner who is walking around the
    // world has no meaningful build-space camera, so they must not appear as a marker to whoever is building.
    // The reverse is deliberately ALLOWED: someone outside the bench still sees the builder's marker (you can
    // watch your partner work from outside). Both outside -> neither sends -> no markers (you see avatars).
    // On the in->out edge send one kind=2 so the partner drops our marker instantly, not after the timeout.
    static long s_was_in = 0;
    if (!g_in_bench) {
        if (s_was_in) {
            s_was_in = 0;
            PoseMsg m; memset(&m, 0, sizeof m); m.magic = WS_MAGIC; m.ver = 1; m.kind = 2;   // 2 = left the bench
            p_send(g_net, g_peerIdent, &m, sizeof m, WS_SEND_RELIABLE, WS_CHANNEL);
            logline("net: left the workbench - told partner to drop our camera marker");
        }
        return;
    }
    s_was_in = 1;
    if (!InterlockedCompareExchange(&g_have_local, 0, 0)) return;
    CamPose local;
    if (!read_pose(g_local_snapshot, &g_local_seq, local)) return;   // torn read; try next tick
    PoseMsg m; m.magic = WS_MAGIC; m.ver = 1; m.kind = 1;
    for (int i = 0; i < 3; i++) { m.pos[i]=local.pos[i]; m.fwd[i]=local.fwd[i]; m.up[i]=local.up[i]; m.right[i]=local.right[i]; }
    int rc = p_send(g_net, g_peerIdent, &m, sizeof m, WS_SEND_UNRELIABLE, WS_CHANNEL);  // pose = latest-wins
    // Session-close is CATASTROPHIC if coopworkbench.dll shares this per-identity session, so re-check coop
    // presence LIVE at the close site (the init-time latch goes stale when coop is injected/unloaded
    // after us). Cheap: only runs on the error branch. Err toward never-closing when coop may be up.
    bool coop_now = g_coop_present;   // same binary - see the note in net_init
    if ((rc == 35 || rc == 3) && p_close && !coop_now) {
        p_close(g_net, g_peerIdent); InterlockedExchange(&g_net_ok, 0);
    }
}

// broadcast our hovered voxel (kind 3). Same in-bench rule as the camera pose.
static void net_send_cursor() {
    if (!p_send || !g_net || !g_peerid || !g_in_bench) return;
    CursorMsg c; memset(&c, 0, sizeof c);
    c.magic = WS_MAGIC; c.ver = 1; c.kind = 3;
    c.flags = (uint16_t)(g_cur_valid ? 1 : 0);
    c.vx = g_cur_vx; c.vy = g_cur_vy; c.vz = g_cur_vz; c.tool = (uint32_t)g_cur_tool;
    p_send(g_net, g_peerIdent, &c, sizeof c, WS_SEND_UNRELIABLE, WS_CHANNEL);   // latest-wins
}
// SOLO SELF-TEST: feed our own cursor back as the partner's, ~1s late, so one machine can verify the marker.
static void cursor_selftest_tick() {
    if (!g_cursor_selftest) return;
    DWORD now = GetTickCount();
    { static DWORD s_lw=0;
      if (now - s_lw > 2000) { s_lw = now;
          logline("[cursor] selftest: local valid=%ld v=(%ld,%ld,%ld) buf=%d | peer valid=%ld v=(%d,%d,%d) age=%lu | haveBest=%ld scale=%.3f off=(%.2f,%.2f,%.2f)",
                  g_cur_valid, g_cur_vx, g_cur_vy, g_cur_vz, g_curbuf_n,
                  (long)g_peer_cur_valid, g_peer_cur_vx, g_peer_cur_vy, g_peer_cur_vz,
                  (unsigned long)(now - g_peer_cur_last),
                  (long)InterlockedCompareExchange(&g_have_best,0,0), g_scale, g_off[0],g_off[1],g_off[2]); } }
    if (g_cur_valid) {                                   // record current sample
        if (g_curbuf_n < CURBUF) {
            g_curbuf[g_curbuf_n].t = now; g_curbuf[g_curbuf_n].vx = g_cur_vx;
            g_curbuf[g_curbuf_n].vy = g_cur_vy; g_curbuf[g_curbuf_n].vz = g_cur_vz; g_curbuf_n++;
        } else {                                         // slide the window
            memmove(&g_curbuf[0], &g_curbuf[1], sizeof(g_curbuf[0])*(CURBUF-1));
            g_curbuf[CURBUF-1].t = now; g_curbuf[CURBUF-1].vx = g_cur_vx;
            g_curbuf[CURBUF-1].vy = g_cur_vy; g_curbuf[CURBUF-1].vz = g_cur_vz;
        }
    }
    for (int i = g_curbuf_n - 1; i >= 0; --i) {           // newest sample at least 1s old
        if (now - g_curbuf[i].t >= 1000) {
            g_peer_cur_vx = g_curbuf[i].vx; g_peer_cur_vy = g_curbuf[i].vy; g_peer_cur_vz = g_curbuf[i].vz;
            g_peer_cur_last = now; InterlockedExchange(&g_peer_cur_valid, 1);
            break;
        }
    }
}

static DWORD WINAPI net_worker(LPVOID) {
    int tick = 0;
    while (!InterlockedCompareExchange(&g_want_exit, 0, 0) && !InterlockedCompareExchange(&g_stop, 0, 0)) {
        // (the solo cursor self-test is retired - see coop.cpp; the marker is confirmed on two machines)
        if (g_net && g_peerid) {
            if (p_accept) p_accept(g_net, g_peerIdent);      // accept inbound (idempotent)
            net_send_pose();                                 // ~20 Hz below
            net_send_cursor();
            if (p_recv) {
                void* msgs[16];
                int n = p_recv(g_net, WS_CHANNEL, msgs, 16);     // our channel only
                for (int i = 0; i < n; i++) {
                    void* mm = msgs[i];
                    void* data = *(void**)((BYTE*)mm + 0x00);
                    int   sz   = *(int*)((BYTE*)mm + 0x08);
                    // Peek magic+kind BEFORE copying: messages are no longer all PoseMsg-sized, so the old
                    // "sz >= sizeof(PoseMsg)" test would silently reject the smaller CursorMsg.
                    uint32_t mg=0; uint8_t kd=0;
                    if (data && sz >= 6 && net_safe_copy(&mg, data, 4) && mg == WS_MAGIC
                        && net_safe_copy(&kd, (BYTE*)data + 5, 1)) {
                        if (kd == 1 && sz >= (int)sizeof(PoseMsg)) {
                            PoseMsg m;
                            if (net_safe_copy(&m, data, sizeof m)) {
                                publish_pose(g_peer, &g_peer_seq, m.pos, m.fwd, m.up, m.right);  // seqlock
                                g_peer_last = GetTickCount();
                                InterlockedExchange(&g_have_peer, 1);
                                if (!InterlockedExchange(&g_net_ok, 1)) logline("net: *** peer camera link LIVE ***");
                            }
                        } else if (kd == 2) {
                            InterlockedExchange(&g_have_peer, 0);       // partner left the bench -> drop marker now
                            InterlockedExchange(&g_peer_cur_valid, 0);
                        } else if (kd == 3 && sz >= (int)sizeof(CursorMsg)) {
                            CursorMsg c;
                            if (net_safe_copy(&c, data, sizeof c)) {
                                g_peer_cur_vx=c.vx; g_peer_cur_vy=c.vy; g_peer_cur_vz=c.vz;
                                g_peer_cur_last=GetTickCount();
                                InterlockedExchange(&g_peer_cur_valid, (c.flags & 1) ? 1 : 0);
                            }
                        }
                    }
                    if (p_release) p_release(mm);
                }
            }
        }
        tick++;
        Sleep(50);   // 20 Hz - ample for a smooth remote cursor, negligible bandwidth
    }
    logline("net_worker exiting");
    return 0;
}

// Tear down the overlay: stop the net worker, restore both the SwapBuffers IAT and the chained
// glUniformMatrix4fv slot, and wait for any in-flight SwapBuffers call to leave our code. Called
// by overlay_stop() from coop.cpp's unload path - the MERGED DLL does its single
// FreeLibraryAndExitThread over in coop's cmd_watcher, never here.
static void overlay_teardown() {
    logline("overlay teardown requested");
    InterlockedExchange(&g_want_exit, 1);
    // Join boot FIRST. boot installs the SwapBuffers IAT hook and spawns net_worker, so it must
    // finish before we restore hooks or read g_net_worker - otherwise a late boot could re-hook or
    // spawn a thread into the about-to-be-unmapped image (use-after-free). boot has no blocking
    // calls, so this returns promptly.
    if (g_boot_worker) {
        WaitForSingleObject(g_boot_worker, INFINITE);
        CloseHandle(g_boot_worker); g_boot_worker = nullptr;
    }
    // stop the net worker before anything frees - it touches Steam + our globals. MANDATORY join:
    // the worker checks g_want_exit every 50ms and Steam's calls are non-blocking, so this is prompt.
    if (g_net_worker) {
        while (WaitForSingleObject(g_net_worker, 1000) != WAIT_OBJECT_0)
            logline("still waiting for net_worker to exit before unload...");
        CloseHandle(g_net_worker); g_net_worker = nullptr;
    }
    // let my_SwapBuffers observe g_want_exit and delete its font/GL lists on a live frame (bounded)
    for (int i = 0; i < 200 && !InterlockedCompareExchange(&g_gl_freed, 0, 0); i++) Sleep(10);
    DWORD o;
    if (g_slot && g_next_um4 && VirtualProtect(g_slot, 8, PAGE_READWRITE, &o)) {
        *g_slot = (void*)g_next_um4; VirtualProtect(g_slot, 8, o, &o);
    }
    if (g_iat_swap && g_orig_swap && VirtualProtect(g_iat_swap, sizeof(void*), PAGE_READWRITE, &o)) {
        *g_iat_swap = (void*)g_orig_swap; VirtualProtect(g_iat_swap, sizeof(void*), o, &o);
    }
    // wait (bounded) for any thread currently inside my_SwapBuffers to leave before coop frees us
    for (int i = 0; i < 400 && InterlockedCompareExchange(&g_inhook, 0, 0); i++) Sleep(5);
    Sleep(250);
    logline("overlay unhooked cleanly (frames %u)", g_frames);
}

static DWORD WINAPI boot(LPVOID) {
    char p[MAX_PATH]; GetModuleFileNameA(g_self, p, MAX_PATH);
    char* s = strrchr(p, '\\'); if (s) *(s+1) = 0;
    sprintf_s(g_logpath, "%swsdraw-log.txt", p);
    sprintf_s(g_cfgpath, "%swsdraw-cfg.txt", p);
    sprintf_s(g_peerpath, "%scoop-peer.txt", p);   // share the co-op mod's partner config
    logline("=== wsdraw boot ===");
    ULONGLONG base = (ULONGLONG)GetModuleHandleA(nullptr);
    g_iat_swap = find_iat_slot(base, "GDI32.dll", "SwapBuffers");
    if (!g_iat_swap) { logline("no SwapBuffers IAT slot - aborting"); return 0; }
    DWORD o;
    if (VirtualProtect(g_iat_swap, sizeof(void*), PAGE_READWRITE, &o)) {
        g_orig_swap = (BOOL (WINAPI*)(HDC))*g_iat_swap;
        *g_iat_swap = (void*)&my_SwapBuffers;
        VirtualProtect(g_iat_swap, sizeof(void*), o, &o);
        logline("SwapBuffers hooked - orange lattice = voxels 0..%d, red cross = craft origin", g_span);
    }
    // RETRY, like coop's own steam_init does. This is a SEPARATE copy with its own single attempt, and it
    // survives today only by ordering - overlay_start() runs after coop's 60s retry loop has already
    // succeeded. That is incidental, not a guarantee: if coop's loop ever times out (slow disk, big mod
    // list, ASI load), this would fail permanently and the partner camera and cursor would never appear,
    // with only a log line to show for it.
    { bool ok = false;
      for (int i = 0; i < 300 && !ok; i++) { ok = steam_init(); if (!ok) Sleep(100); }
      if (ok) g_net_worker = CreateThread(nullptr, 0, net_worker, nullptr, 0, nullptr);
      else logline("net: steam never became ready (30s) - partner camera/cursor disabled for this session"); }
    return 0;
}

// ---- merge entry points ----------------------------------------------------
// This file has NO DllMain of its own - it is compiled straight into coopworkbench.dll, and coop.cpp's
// DllMain drives the overlay's lifecycle: overlay_start() at setup, overlay_stop() on unload.
extern "C" void overlay_start(HMODULE self) {
    g_self = self;
    g_boot_worker = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
}
extern "C" void overlay_stop() {
    overlay_teardown();
}
