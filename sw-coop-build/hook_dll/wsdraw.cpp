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
static unsigned g_frames = 0;
static bool g_ready = false, g_font_ready = false;
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
// Each glyph is 5 rows of 3 bits packed into a u16: bit (row*3 + col), col 0 = leftmost.
#define G(r0,r1,r2,r3,r4) ((unsigned short)((r0)|((r1)<<3)|((r2)<<6)|((r3)<<9)|((r4)<<12)))
static const unsigned short FONT_DIGIT[10] = {
    G(7,5,5,5,7), G(2,3,2,2,7), G(7,4,7,1,7), G(7,4,7,4,7), G(5,5,7,4,4),
    G(7,1,7,4,7), G(7,1,7,5,7), G(7,4,4,4,4), G(7,5,7,5,7), G(7,5,7,4,7),
};
static const unsigned short FONT_ALPHA[26] = {
    G(2,5,7,5,5), G(3,5,3,5,3), G(6,1,1,1,6), G(3,5,5,5,3), G(7,1,3,1,7), // A-E
    G(7,1,3,1,1), G(6,1,5,5,6), G(5,5,7,5,5), G(7,2,2,2,7), G(4,4,4,5,2), // F-J
    G(5,5,3,5,5), G(1,1,1,1,7), G(5,7,7,5,5), G(5,7,7,7,5), G(7,5,5,5,7), // K-O
    G(7,5,7,1,1), G(7,5,5,7,4), G(7,5,3,5,5), G(6,1,2,4,3), G(7,2,2,2,2), // P-T
    G(5,5,5,5,7), G(5,5,5,5,2), G(5,5,7,7,5), G(5,5,2,5,5), G(5,5,2,2,2), // U-Y
    G(7,4,2,1,7),                                                          // Z
};
static unsigned short glyph_of(char c) {
    if (c >= '0' && c <= '9') return FONT_DIGIT[c - '0'];
    if (c >= 'A' && c <= 'Z') return FONT_ALPHA[c - 'A'];
    if (c >= 'a' && c <= 'z') return FONT_ALPHA[c - 'a'];
    switch (c) {
        case ' ': return G(0,0,0,0,0);  case '.': return G(0,0,0,0,2);
        case ',': return G(0,0,0,2,1);  case '-': return G(0,0,7,0,0);
        case '+': return G(0,2,7,2,0);  case '=': return G(0,7,0,7,0);
        case ':': return G(0,2,0,2,0);  case '(': return G(4,2,2,2,4);
        case ')': return G(1,2,2,2,1);  case '/': return G(4,4,2,1,1);
        case '%': return G(5,4,2,1,5);  case '<': return G(4,2,1,2,4);
        case '>': return G(1,2,4,2,1);  case '_': return G(0,0,0,0,7);
        case '|': return G(2,2,2,2,2);  case '#': return G(5,7,5,7,5);
        case '!': return G(2,2,2,0,2);  case '?': return G(7,4,2,0,2);
        case '*': return G(5,2,5,0,0);  case '\'':return G(2,2,0,0,0);
        default:  return G(0,7,5,7,0);                   // unknown -> a box
    }
}
static int g_px = 3;                                     // pixel size; char cell = 3*px wide
static void draw_text_px(int x, int y_top, float r, float g, float b, const char* s, int px) {
    glColor4f(r, g, b, 1.0f);
    glBegin(GL_QUADS);
    int cx = x;
    for (const char* p = s; *p; p++, cx += 4 * px) {
        unsigned short gl = glyph_of(*p);
        if (!gl) continue;
        for (int row = 0; row < 5; row++) for (int col = 0; col < 3; col++) {
            if (!(gl & (1 << (row * 3 + col)))) continue;
            float qx = (float)(cx + col * px), qy = (float)(y_top + row * px);
            glVertex2f(qx, qy); glVertex2f(qx+px, qy);
            glVertex2f(qx+px, qy+px); glVertex2f(qx, qy+px);
        }
    }
    glEnd();
}
static void draw_text(int x, int y_top, float r, float g, float b, const char* s) {
    draw_text_px(x, y_top, r, g, b, s, g_px);
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

    // THE FEATURE: draw the partner's camera marker whenever a fresh pose has arrived over
    // Steam. Always shown (independent of the HUD toggle) so you can always see your partner.
    bool peerFresh = false;
    if (haveBest) {
        const DWORD peerAge = GetTickCount() - g_peer_last;
        CamPose peerSnap;
        peerFresh = InterlockedCompareExchange(&g_have_peer, 0, 0) && peerAge < 2000
                    && read_pose(g_peer, &g_peer_seq, peerSnap);   // seqlock: stable copy
        if (peerFresh) draw_peer_camera(peerSnap, "PARTNER", 0.3f, 1.0f, 0.5f);
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
                } else { logline("could not find the cached glUniformMatrix4fv pointer - is another probe loaded?");
                         InterlockedExchange(&g_stop, 1); }
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
            // live offset nudge - drive the lattice onto the craft instead of guessing the
            // body transform. Only while the game has focus, so we never eat someone else's keys.
            if (GetForegroundWindow() == WindowFromDC(hdc)) {
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
    uint32_t magic; uint8_t ver; uint8_t kind;   // kind 1 = camera pose, 0 = keepalive/ignore
    float pos[3], fwd[3], up[3], right[3];
};
#pragma pack(pop)

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
    auto pClear = (identClear_t) GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_Clear");
    auto pSetID = (identSetID_t) GetProcAddress(s, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
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
    g_coop_present = (GetModuleHandleA("coopworkbench.dll") != nullptr);
    logline("net: our=%llu peer=%llu channel=%d coopworkbench.dll=%s %s",
            (unsigned long long)g_myid, (unsigned long long)g_peerid, WS_CHANNEL,
            g_coop_present ? "present (will NOT close sessions)" : "absent",
            g_peerid ? "" : "(no coop-peer.txt -> no partner configured)");
    if (g_peerid) { pClear(g_peerIdent); pSetID(g_peerIdent, g_peerid); p_accept(g_net, g_peerIdent); }
    return true;
}

static void net_send_pose() {
    if (!p_send || !g_net || !g_peerid || !InterlockedCompareExchange(&g_have_local, 0, 0)) return;
    CamPose local;
    if (!read_pose(g_local_snapshot, &g_local_seq, local)) return;   // torn read; try next tick
    PoseMsg m; m.magic = WS_MAGIC; m.ver = 1; m.kind = 1;
    for (int i = 0; i < 3; i++) { m.pos[i]=local.pos[i]; m.fwd[i]=local.fwd[i]; m.up[i]=local.up[i]; m.right[i]=local.right[i]; }
    int rc = p_send(g_net, g_peerIdent, &m, sizeof m, WS_SEND_UNRELIABLE, WS_CHANNEL);  // pose = latest-wins
    // Session-close is CATASTROPHIC if coopworkbench.dll shares this per-identity session, so re-check coop
    // presence LIVE at the close site (the init-time latch goes stale when coop is injected/unloaded
    // after us). Cheap: only runs on the error branch. Err toward never-closing when coop may be up.
    bool coop_now = g_coop_present || (GetModuleHandleA("coopworkbench.dll") != nullptr);
    if ((rc == 35 || rc == 3) && p_close && !coop_now) {
        p_close(g_net, g_peerIdent); InterlockedExchange(&g_net_ok, 0);
    }
}

static DWORD WINAPI net_worker(LPVOID) {
    int tick = 0;
    while (!InterlockedCompareExchange(&g_want_exit, 0, 0) && !InterlockedCompareExchange(&g_stop, 0, 0)) {
        if (g_net && g_peerid) {
            if (p_accept) p_accept(g_net, g_peerIdent);          // accept inbound (idempotent)
            net_send_pose();                                     // ~20 Hz below
            if (p_recv) {
                void* msgs[16];
                int n = p_recv(g_net, WS_CHANNEL, msgs, 16);     // our channel only
                for (int i = 0; i < n; i++) {
                    void* mm = msgs[i];
                    void* data = *(void**)((BYTE*)mm + 0x00);
                    int   sz   = *(int*)((BYTE*)mm + 0x08);
                    PoseMsg m;
                    // SEH-guarded copy: a short/hostile inbound payload must not fault the game.
                    if (data && sz >= (int)sizeof(PoseMsg) && net_safe_copy(&m, data, sizeof m)) {
                        if (m.magic == WS_MAGIC && m.kind == 1) {
                            publish_pose(g_peer, &g_peer_seq, m.pos, m.fwd, m.up, m.right);  // seqlock
                            g_peer_last = GetTickCount();
                            InterlockedExchange(&g_have_peer, 1);
                            if (!InterlockedExchange(&g_net_ok, 1)) logline("net: *** peer camera link LIVE ***");
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
    if (steam_init()) { g_net_worker = CreateThread(nullptr, 0, net_worker, nullptr, 0, nullptr); }
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
