// Widescreen: the camera code's 16:9 aspect literal loads are patched (tools/patches.py) to call
// rt_aspect_site(); the renderer sets the wide factor (output aspect / 16:9) each frame.
#include "../game_addresses.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "../hle.h"
#include "../settings.h"

#define BACKDROP_HASHES 0x71fe31bbf03c3515ull

static float g_wide = 1.0f;  // output aspect divided by 16:9 (1 = normal)

void widescreen_set_factor(float f) { g_wide = f; }
float widescreen_factor(void) { return g_wide; }

// Effects drawn as a mesh around the camera, sized to cover a 16:9 view: with the widened camera their outer edge
// shows (Mysterious Woods fog ring: a hard-edged oval at 32:9, a stray cloud piece at the far left). Their draws
// get a viewport widened by the widescreen factor (centred), so they cover the window as they cover 16:9.
// Identified by fragment shader hash (both backends hash the shader code the same way).
float widescreen_effect_stretch(uint64_t fs_hash) {
    static const uint64_t k[] = {
        0xffad3b2d795231a7ull,  // Mysterious Woods fog ring (Field 06C...; found 2026-10-09 by skipping shaders)
        0xd66b33b13b2eac29ull,  // Mysterious Woods haze layer
        0x1372d4fe34dd4bb4ull,  // Mysterious Woods light-shaft boxes (their sides: vertical streaks at the far left)
    };
    if (g_wide <= 1.0f) return 1.0f;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i] == fs_hash) return g_wide;
    // dungeon darkness around the camera: a hard-edged dark oval once a room shows wider than 16:9 (boxed rooms
    // keep the original vignette)
    extern bool g_room_wide;
    if (fs_hash == 0xc42e882c08c5e5ebull && g_room_wide) return g_wide;
    return 1.0f;
}

// Untextured quads drawn with a full-screen viewport are taken for fades and flashes and fill the window
// (runtime/nvn/renderer.m), but a layout pane draws that way too, whatever its size: the speaker-less message box's
// dark band (622x46 behind the rounded box, "Wow! This looks pretty heavy!...") came out stretched to the window's
// width, two dark bars beside the box at 32:9 (user 2026-10-20). A layout (ui2d) draw's vertex uniforms hold the
// projection (floats 0-15), the pane's model-view rows (16-27) and its rect (132-135: width, height, left, top; y up):
// true when they describe a pane that doesn't cover the screen (such a draw stays in the 16:9 box).
bool widescreen_ui_pane_partial(const float *u, uint64_t size) {
    if (!u || size != 560) return false;
    if (u[15] != 1 || u[12] != 0 || u[13] != 0 || !(u[0] > 0) || u[5] == 0) return false;  // (orthographic projection)
    float w = u[132], h = u[133], l = u[134], t = u[135];
    if (!(w > 0) || !(h > 0) || !(w < 1e5f) || !(h < 1e5f)) return false;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (int i = 0; i < 4; i++) {
        float x = l + (i & 1 ? w : 0), y = t - (i & 2 ? h : 0);
        float X = u[16] * x + u[17] * y + u[19], Y = u[20] * x + u[21] * y + u[23], Z = u[24] * x + u[25] * y + u[27];
        float n[2] = {u[0] * X + u[1] * Y + u[2] * Z + u[3], u[4] * X + u[5] * Y + u[6] * Z + u[7]};
        for (int k = 0; k < 2; k++) lo[k] = fminf(lo[k], n[k]), hi[k] = fmaxf(hi[k], n[k]);
    }
    return lo[0] > -0.99f || hi[0] < 0.99f || lo[1] > -0.99f || hi[1] < 0.99f;
}

// Off in rooms that should keep 16:9 framing (interiors, dungeons) -- see widescreen_update.
static bool g_allowed = true;
static bool g_indoor;
bool widescreen_allowed(void) { return g_allowed && g_settings.aspect == ASPECT_UNLOCKED && !ENV("LARECOMP_NO_WIDESCREEN"); }
void widescreen_set_allowed(bool on) { g_allowed = on; }

// Interiors and dungeons use cameras that never load the patched overworld aspect (site 0). Count
// site-0 loads per presented frame; frames without any are framed 16:9 (pillar/letterboxed).
static volatile uint64_t g_site0_frame = ~0ull;
static int g_indoor_streak, g_outdoor_streak;

RT_ON_RESET(widescreen_reset) { g_indoor = false; g_indoor_streak = g_outdoor_streak = 0; }

void widescreen_frame_end(uint64_t frame) {
    // (another game: its camera runs a frame ahead of the one presented - the GPU at most one frame behind)
    bool saw = g_site0_frame == frame || (game_other() && g_site0_frame == frame + 1);
    if (saw) { g_outdoor_streak++; g_indoor_streak = 0; }
    else { g_indoor_streak++; g_outdoor_streak = 0; }
    bool was = g_indoor;
    if (!g_indoor && g_indoor_streak >= 3) g_indoor = true;
    if (g_indoor && g_outdoor_streak >= 1) g_indoor = false;
    if (was != g_indoor && ENV("LARECOMP_LOG_INDOOR"))
        LOG("[widescreen] %s at frame %llu\n", g_indoor ? "interior (16:9 framing)" : "overworld (wide)", (unsigned long long)frame);
}

float rt_aspect_site(int site) {
    if (site == 0) { extern uint64_t renderer_present_count(void); g_site0_frame = renderer_present_count(); }
    static long mask = -2;
    if (mask == -2) mask = getenv("LARECOMP_ASPECT_SITES") ? strtol(getenv("LARECOMP_ASPECT_SITES"), NULL, 0) : -1;
    float base = 16.0f / 9.0f;
    if (mask != -1 && !(mask & (1L << site))) return base;
    return base * g_wide;  // the output's aspect (wider or narrower than 16:9)
}

#define NSPOT 32
static struct { float x, y; uint64_t frame; } g_spot[NSPOT];  // spots placed by rt_screen_spot (moved: k != 1)
static volatile unsigned g_nspot;
static float g_cam_shape = 1.0f;  // (another game) the shape the camera's projection was last built with (site 2)

// Another game (tools/gen.py eow_aspect): the places its code builds 16:9 get the output's aspect instead.
// Only site 2 is the camera's projection (EoW, 2026-11-05: each site alone at 32:9 in the town; site 3, also every
// frame, stretches the picture, the rest run once at a level load). LARECOMP_ASPECT_OTHER=mask picks other sites;
// LARECOMP_LOG_ASPECT counts each site's uses.
#define OTHER_SITES 0x4ull
uint32_t rt_aspect_other(int site) {
    static long long mask = -2;
    if (mask == -2) mask = getenv("LARECOMP_ASPECT_OTHER") ? strtoll(getenv("LARECOMP_ASPECT_OTHER"), NULL, 0) : (long long)OTHER_SITES;
    float f = 16.0f / 9.0f;
    if (mask & (1LL << site)) f *= g_wide;
    // (the camera ran this frame: frames without it - title, file select, loading - are boxed, as LA's site 0)
    if (site == 2) { extern uint64_t renderer_present_count(void); g_site0_frame = renderer_present_count(); g_cam_shape = f / (16.0f / 9.0f); }
    static uint64_t n[32], last;
    if (ENV("LARECOMP_LOG_ASPECT")) {
        n[site & 31]++;
        extern uint64_t renderer_present_count(void);
        uint64_t fr = renderer_present_count();
        if (fr / 300 != last / 300) {
            char s[400]; int k = 0;
            for (int i = 0; i < 26 && k < 380; i++) if (n[i]) k += snprintf(s + k, sizeof s - k, " %d:%llu", i, (unsigned long long)n[i]);
            LOG("[aspect] frame %llu sites%s\n", (unsigned long long)fr, s);
            memset(n, 0, sizeof n);
            last = fr;
        }
    }
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

// Another game (tools/gen.py eow_spot): layouts the game places at a spot on screen - the lock-on reticle
// (L_LockOn_00) and the other sites there - get their root pane's translation (layout units from the centre, y up)
// here. The game works the spot out with the camera's projection, widened to the window's shape, and turns it into
// the 16:9 layout's units, which are drawn in the centred 16:9 box: at 32:9 the reticle sat halfway between its
// target and the centre (user report 2026-11-16). Scaled back out to the window's shape: x by the shape on a wide
// window, y by 1 / shape on a tall one (the camera keeps the 16:9 view's width there and shows more above and below).
// Only while the camera runs widened (site 2 this frame or the last two: interiors keep a 16:9 camera).
// LARECOMP_LOG_SPOTS: each site's pane name and spot, once a second.
void rt_screen_spot(int site, uint64_t pane, float *xy) {
    extern uint64_t renderer_present_count(void);
    uint64_t now = renderer_present_count();
    int64_t ago = (int64_t)(now - g_site0_frame);
    float k = ago >= -2 && ago <= 2 ? g_cam_shape : 1.0f;
    if (ENV("LARECOMP_LOG_SPOTS")) {
        static uint64_t last[8];
        if (site >= 0 && site < 8 && now >= last[site] + 60) {
            last[site] = now;
            LOG("[spot] site %d %s (%.1f, %.1f) shape %.3f\n", site, (const char *)(uintptr_t)(pane + 0xB0), xy[0], xy[1], k);
        }
    }
    if (k > 1.0f) xy[0] *= k;
    else if (k > 0.1f && k < 1.0f) xy[1] /= k;
    if (k != 1.0f) {  // (for widescreen_spot_ubo: the render thread draws these past the 16:9 box)
        unsigned i = g_nspot++ % NSPOT;
        g_spot[i].x = xy[0], g_spot[i].y = xy[1], g_spot[i].frame = now;
    }
}

// Render thread: layout draws go into the centred 16:9 box (renderer.m / vulkan/exec.c), which cuts off a layout
// placed past its sides above. A draw whose pane sits near a spot placed in the last few frames gets the window-wide
// viewport and a copy of its uniforms with the projection divided by the stretch (k, ky), as the phones' HUD corners
// do (runtime/hle/hud_corners.c): the same pixels inside the box, and no longer cut off at its edge. NULL otherwise.
// (vertex uniforms: projection in floats 0-15, the pane's model-view rows in 16-27; 560 bytes: panes, 128: text)
const void *widescreen_spot_ubo(const float *u, uint64_t size, float k, float ky) {
    if (!u || (size != 560 && size != 128) || (k <= 1.001f && ky <= 1.001f) || !g_nspot) return NULL;
    if (u[15] != 1 || u[12] != 0 || u[13] != 0 || !(u[0] > 0) || u[5] == 0) return NULL;  // (orthographic projection)
    extern uint64_t renderer_exec_frame(void);
    uint64_t fr = renderer_exec_frame();
    float px = u[0] * u[19] + u[1] * u[23] + u[2] * u[27] + u[3], py = u[4] * u[19] + u[5] * u[23] + u[6] * u[27] + u[7];
    bool near = false;
    for (unsigned i = 0; i < NSPOT && !near; i++) {
        int64_t d = (int64_t)(fr - g_spot[i].frame);
        if (d < -3 || d > 3) continue;
        float sx = u[0] * g_spot[i].x + u[1] * g_spot[i].y + u[3], sy = u[4] * g_spot[i].x + u[5] * g_spot[i].y + u[7];
        near = fabsf(px - sx) < 0.35f && fabsf(py - sy) < 0.5f;  // (a reticle, a prompt with its text box)
    }
    if (!near) return NULL;
    static float ring[1024][140];
    static unsigned at;
    float *c = ring[at++ & 1023];
    memcpy(c, u, size);
    for (int i = 0; i < 4; i++) c[i] /= k, c[4 + i] /= ky;
    return c;
}

// Culling aspect: the game's own value (camera field), widened like the camera.
float rt_cull_aspect(float orig) {
    static float last = -1;
    if (orig != last && ENV("LARECOMP_LOG_CULL")) { LOG("[widescreen] culling aspect field %f\n", orig); last = orig; }
    return rt_aspect_site(6) * (orig > 0.1f && orig < 10.0f ? orig / (16.0f / 9.0f) : 1.0f);
}

// Sky and sea pieces past the north edge (tools/patches.py SKYSEA_BOX_SITES): a piece shows while Link is within
// its reach plus half a 16:9 view (7.5). A wider view reaches further: half its extra width, doubled for the far
// (top) edge of the perspective view, where the sky is.
float rt_skysea_margin(void) {
    return g_wide > 1.0f ? 15.0f * (g_wide - 1.0f) : 0.0f;
}

// Vertical field of view: unchanged for wide outputs; for tall ones (factor < 1) widened so the
// horizontal view stays what the game shows at 16:9.
float rt_fovy(float orig) {
    static float last = -1;
    if (orig != last && ENV("LARECOMP_LOG_CULL")) { LOG("[widescreen] fovy field %f\n", orig); last = orig; }
    if (g_wide >= 1.0f || !(orig > 0)) return orig;
    bool degrees = orig > 3.2f;  // radians otherwise
    float half = (degrees ? orig * (float)M_PI / 180.0f : orig) * 0.5f;
    float wide = 2.0f * atanf(tanf(half) / g_wide);
    return degrees ? wide * 180.0f / (float)M_PI : wide;
}

// Interiors: the indoor shadow setup runs while an interior is rendered (tools/patches.py).
static uint64_t g_indoor_frame;
float rt_indoor_aspect(void) {
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    if (ENV("LARECOMP_LOG_INDOOR") && f / 60 != g_indoor_frame / 60) LOG("[widescreen] indoor at frame %llu\n", (unsigned long long)f);
    g_indoor_frame = f;
    return 16.0f / 9.0f;
}

void romfs_file_read_hook(const char *path) { (void)path; }

// Interior backdrop (16:9 quad covering the overworld behind rooms), identified by shader code hash.
static uint64_t g_backdrop_hash = 0;  // set below once known
static uint64_t g_backdrop_frame = ~0ull;
void widescreen_backdrop_seen(uint64_t hash) {
    static const uint64_t known[] = {BACKDROP_HASHES};
    // another game (EoW): the blurred picture behind its menus (pause screen, echo list): boxed when it isn't drawn over the
    // whole window (Aspect ratio not Unlocked; runtime/hle/pause_fill.c)
    static const uint64_t other[] = {0x78e8be3f9e103151ull};
    const uint64_t *k = game_other() ? other : known;
    unsigned n = game_other() ? sizeof other / sizeof other[0] : sizeof known / sizeof known[0];
    for (unsigned i = 0; i < n; i++)
        if (k[i] && hash == k[i]) { extern uint64_t renderer_exec_frame(void); g_backdrop_frame = renderer_exec_frame(); }
    (void)g_backdrop_hash;
}
// Interiors and dungeons only render their 16:9 centre. Besides the backdrop detection, the game's current level
// decides: in play and not on the overworld ("Field") -> boxed. That also covers fades and loading frames, where
// no backdrop is drawn and the sides would show stale pixels.
static bool level_is_interior(void) {
    extern int game_in_play(void);
    if (!game_in_play()) return false;
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));  // PlayerStatus*: current level name at +0
    if (!ps) return false;
    const char *lv = (const char *)(uintptr_t)ps;
    return lv[0] && strcmp(lv, "Field") != 0;
}

bool widescreen_frame_boxed(uint64_t frame) {
    static bool last;
    static uint64_t interior_until;  // keep boxing briefly after leaving: the first (dark) frames still show stale sides
    // (another game: frames its camera doesn't draw, rt_aspect_other, and the menus' blurred backdrop - unless that
    // backdrop was drawn over the whole window, runtime/hle/pause_fill.c: then the menu fills the window too)
    extern bool pause_fill_frame(uint64_t frame);
    if (game_other()) return (g_indoor || g_backdrop_frame == frame) && !pause_fill_frame(frame);
    bool in = level_is_interior();
    if (in) interior_until = frame + 8;
    // title / file select (not in play): 16:9 screens; the title's logo glow is drawn too large in the wide image
    // (a blurred copy in the left bar, user 2026-10-09)
    extern int game_in_play(void);
    bool b = g_backdrop_frame == frame || in || frame < interior_until || !game_in_play();
    if (b != last && ENV("LARECOMP_LOG_INDOOR")) LOG("[widescreen] boxed %d at frame %llu\n", b, (unsigned long long)frame);
    last = b;
    return b;
}

// Rooms wider than one screen (multi-cell dungeon rooms, caves, side-view passages) show what the wide camera sees as
// far as the 16:9 view ever reaches on each side, at its scroll limit there (the whole end wall, its doors and the
// dark past it), instead of the 16:9 box; past that, black. Single-screen rooms stay boxed (the 16:9 view doesn't
// scroll there).
// The current camera zone (the level's zone record: id, AABB min at +0x10, max at +0x20, camera params at +0x30)
// is reached through [[sdk+0xcf9e38]+0x680] (found 2026-10-09 by pointer scans, LARECOMP_FIND_FLOATS_PATH);
// the camera's view-projection comes from the scene UBO (vertex slot 5: view, projection, view-projection).
static float g_vp[16];  // rows
bool g_room_wide;  // the last presented frame showed a room wider than 16:9
static uint64_t g_vp_frame = ~0ull;
void widescreen_scene_ubo(const float *u, uint64_t size) {
    // (the frame being translated, as the present that crops it numbers it: with the render thread on, the game
    // thread's counter is already a frame ahead here, and every room read as "no scene camera" - boxed 16:9)
    extern uint64_t renderer_exec_frame(void);
    uint64_t f = renderer_exec_frame();
    if (size != 320 || g_vp_frame == f) return;  // (the frame's first scene draw)
    if (u[12] != 0 || u[13] != 0 || u[14] != 0 || u[15] != 1 || u[30] != -1 || u[31] != 0) return;  // perspective
    memcpy(g_vp, u + 32, sizeof g_vp);
    g_vp_frame = f;
}

static bool heap_ptr(uint64_t p) { return p >= HEAP_BASE && p + 0x40 <= HEAP_BASE + HEAP_MAX && !(p & 7); }

// The world point seen at ndc (x, y) of the scene camera (g_vp): on the floor (y 0) for top-down rooms, on the plane
// z = zc for side views.
static bool view_point(float x, float y, bool side, float zc, float p[3]) {
    const float *r0 = g_vp, *r1 = g_vp + 4, *r3 = g_vp + 12;
    // (r0 - x r3) . p = 0, (r1 - y r3) . p = 0 with p = (x, 0, z, 1) or (x, y, zc, 1)
    float q0[4], q1[4];
    for (int i = 0; i < 4; i++) q0[i] = r0[i] - x * r3[i], q1[i] = r1[i] - y * r3[i];
    int j = side ? 1 : 2, k = side ? 2 : 1;  // (j: the unknown besides x; k: the fixed coordinate)
    float kv = side ? zc : 0;
    float a = q0[0], b = q0[j], e = -(q0[k] * kv + q0[3]), d = q1[0], f = q1[j], g = -(q1[k] * kv + q1[3]);
    float det = a * f - b * d;
    if (fabsf(det) < 1e-6f) return false;
    p[0] = (e * f - b * g) / det, p[j] = (a * g - e * d) / det, p[k] = kv;
    return true;
}

// Current camera zone's box; false when there is none (title, overworld, loading).
static bool current_zone(float lo[3], float hi[3], uint32_t *id) {
    uint64_t o = LD64(GA(SDK_SYSTEM_OBJECT));
    if (!heap_ptr(o)) return false;
    uint64_t z = LD64(o + 0x680);
    if (!heap_ptr(z)) return false;
    const float *f = (const float *)(uintptr_t)(z + 0x10);
    for (int i = 0; i < 3; i++) {
        lo[i] = f[i], hi[i] = f[4 + i];
        if (!(hi[i] > lo[i]) || hi[i] - lo[i] > 2000) return false;
    }
    *id = *(const uint32_t *)(uintptr_t)z;
    return true;
}

// The current camera zone record (a dungeon room, an interior), 0 when there is none (overworld, title, loading).
// (Frame pacing: runtime/nvn/pace.c - a room change is a new scene.)
uint64_t game_camera_zone(void) {
    float lo[3], hi[3];
    uint32_t id;
    return current_zone(lo, hi, &id) ? LD64(LD64(GA(SDK_SYSTEM_OBJECT)) + 0x680) : 0;
}

// The camera zone's id (a dungeon room, an interior; Show advanced information's "where you are"); false: none
bool game_camera_zone_id(uint32_t *id) {
    float lo[3], hi[3];
    return current_zone(lo, hi, id);
}

// Horizontal crop (uv of the wide image): ROOM_CROP for a room wider than one screen (u0..u1), ROOM_BOX for a
// single-screen room (16:9 box), ROOM_FULL when the zone doesn't match the camera (no box: the game's camera clamps
// for the wide view, so a 16:9 box around a stale zone's centre cut Link out - user 2026-10-09), ROOM_KEEP when
// there's no recent scene camera (keep the last choice; after half a second without one, e.g. menus: box).
enum { ROOM_KEEP, ROOM_BOX, ROOM_CROP, ROOM_FULL };
const char *game_level_name(void);
static int room_crop(uint64_t frame, double shape, float *u0, float *u1) {
    float lo[3], hi[3];
    uint32_t id;
    if (ENV("LARECOMP_NO_ROOM_WIDE")) return ROOM_BOX;
    if (g_vp_frame > frame || g_vp_frame + 4 < frame) return g_vp_frame + 30 < frame ? ROOM_BOX : ROOM_KEEP;  // (menus: no scene)
    if (!current_zone(lo, hi, &id)) return ROOM_FULL;
    const float *r0 = g_vp, *r3 = g_vp + 12;
    bool side = hi[2] - lo[2] < 6.0f;  // side-view zones are thin in z (the camera looks along z)
    // the point under the screen centre (ndc 0, 0): on the floor (y 0) for top-down rooms, on the zone's centre
    // plane for side views
    float c[3];
    if (!view_point(0, 0, side, (lo[2] + hi[2]) / 2, c)) return ROOM_KEEP;
    int r;
    bool wide = hi[0] - lo[0] >= 16.0f;  // (cells are 15)
    if (c[0] < lo[0] - 1 || c[0] > hi[0] + 1) r = ROOM_FULL;  // camera outside the zone: stale zone
    else if (!wide) r = fabsf(c[0] - (lo[0] + hi[0]) / 2) < 1.0f ? ROOM_BOX : ROOM_FULL;  // centred or stale
    else {
        // The 16:9 game camera's scroll range in this zone: it stops where the bottom corner of its view on the floor
        // is one tile (1.5) past the zone's edge (Key Cavern 03B-03C at 16:9 and 21:9, Ukuku Prairie cave 02C), in
        // side views where its edge meets the zone's (Bottle Grotto 05D); a zone narrower than that keeps it at the
        // centre. The wide camera is clamped the same way for its own wider view, so it scrolls less (or not at all).
        float zc = (lo[2] + hi[2]) / 2, pb[3], pb0[3], pm[3];
        if (!view_point(1, -1, side, zc, pb) || !view_point(0, -1, side, zc, pb0) || !view_point(1, 0, side, zc, pm))
            return ROOM_KEEP;
        float bottom16 = (pb[0] - pb0[0]) / g_wide, mid16 = (pm[0] - c[0]) / g_wide;  // half widths of a 16:9 view
        float mid = (lo[0] + hi[0]) / 2;
        float margin = side ? 0.0f : 1.5f;
        float cmin = fminf(mid, lo[0] - margin + bottom16), cmax = fmaxf(mid, hi[0] + margin - bottom16);
        // Shown: as far as the 16:9 view ever reaches on each side (at the scroll limit: the whole wall, its doors and
        // the dark past it), taken on the screen's middle row, never less than the 16:9 box. A room the wide view
        // holds whole shows black on both sides.
        float ndc[2];
        r = ROOM_CROP;
        // The fishing pond is built 3 units past its zone's west edge (x -12), which its fixed camera never shows at 16:9
        float west = side && !strcmp(game_level_name(), "FishingPond") ? 3.0f : 0.0f;
        for (int e = 0; e < 2; e++) {
            float x = e ? cmax + mid16 : cmin - mid16 - west;
            float w = r3[0] * x + r3[1] * c[1] + r3[2] * c[2] + r3[3];
            if (!(w > 0.01f)) return ROOM_KEEP;
            ndc[e] = (r0[0] * x + r0[1] * c[1] + r0[2] * c[2] + r0[3]) / w;
        }
        float b0 = (float)((1.0 - 1.0 / shape) / 2), b1 = 1.0f - b0;
        *u0 = fminf(b0, fmaxf(0.0f, (ndc[0] + 1) / 2));
        *u1 = fmaxf(b1, fminf(1.0f, (ndc[1] + 1) / 2));
        if (ENV("LARECOMP_LOG_ROOMCAM") && frame % 30 == 0)
            LOG("[roomcam] f%llu zone %u x %.2f..%.2f, camera x %.2f, 16:9 camera range %.2f..%.2f -> shown x %.2f..%.2f "
                "(u %.3f..%.3f)\n", (unsigned long long)frame, id, lo[0], hi[0], c[0], cmin, cmax, cmin - mid16,
                cmax + mid16, *u0, *u1);
    }
    static uint32_t last_id = ~0u;
    static int last_r = -1;
    if ((id != last_id || r != last_r) && ENV("LARECOMP_LOG_INDOOR"))
        LOG("[widescreen] zone %u x %.1f..%.1f z %.1f..%.1f%s, camera x %.1f: %s at frame %llu\n", id, lo[0], hi[0], lo[2],
            hi[2], side ? " (side view)" : "", c[0], r == ROOM_CROP ? "crop" : r == ROOM_BOX ? "box" : "full (stale zone)",
            (unsigned long long)frame);
    last_id = id, last_r = r;
    return r;
}

// Presented part of the image (crop: uv origin and size) and its place in the window: interiors and dungeons show
// their 16:9 centre (taller outputs: the 16:9 band), rooms wider than one screen what 16:9 shows of them (room_crop).
void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw, double *vh) {
    static uint64_t last;  // (last wide boxed frame: room changes slide the edges instead of jumping)
    static float cu0, cu1;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;
    g_room_wide = false;
    if (shape == 1.0 && ENV("LARECOMP_LOG_ROOMCAM")) {  // (LARECOMP_LOG_ROOMCAM: the camera and crop every 30 frames,
        float a, b;                                     // at 16:9 too, to check the scroll range against the game's)
        extern int game_in_play(void);
        if (game_in_play()) room_crop(frame, shape, &a, &b);
    }
    if (shape == 1.0 || !widescreen_frame_boxed(frame)) return;
    extern void hud_corners_note_crop(uint64_t frame, float u0, float u1, float v0, float v1);
    extern bool hud_corners_shown_whole(uint64_t frame);
    if (shape < 1.0) {
        hud_corners_note_crop(frame, 0, 1, (float)((1.0 - shape) / 2), (float)((1.0 + shape) / 2));
        if (hud_corners_shown_whole(frame)) return;  // (phones and tablets: the HUD in the bars, runtime/hle/hud_corners.c)
        crop[1] = (float)((1.0 - shape) / 2); crop[3] = (float)shape;
        double nh = *vh * shape; *vy += (*vh - nh) / 2; *vh = nh;
        return;
    }
    double full_vx = *vx, full_vw = *vw;
    static int mode = ROOM_BOX;
    float u0 = 0, u1 = 1;
    static float k0, k1;  // (ROOM_KEEP: the last target)
    extern int game_in_play(void);
    int m = game_in_play() ? room_crop(frame, shape, &u0, &u1) : ROOM_BOX;
    if (m == ROOM_KEEP) m = mode, u0 = k0, u1 = k1;
    if (m == ROOM_BOX) u0 = (float)((1.0 - 1.0 / shape) / 2), u1 = 1.0f - u0;
    if (m == ROOM_FULL) u0 = 0, u1 = 1;
    mode = m, k0 = u0, k1 = u1;
    g_room_wide = m != ROOM_BOX;
    if (frame <= last + 2 && frame > last) {
        const float step = 0.03f;  // of the image width per frame
        cu0 += fmaxf(-step, fminf(step, u0 - cu0));
        cu1 += fmaxf(-step, fminf(step, u1 - cu1));
    } else {
        cu0 = u0, cu1 = u1;
    }
    last = frame;
    // (phones: the HUD at the screen corners, in the bars: the bars were cleared after the scene, runtime/hle/hud_corners.c)
    hud_corners_note_crop(frame, cu0, cu1, 0, 1);
    crop[0] = cu0, crop[2] = cu1 - cu0;
    *vx += *vw * cu0, *vw *= cu1 - cu0;
    if (hud_corners_shown_whole(frame)) crop[0] = 0, crop[2] = 1, *vx = full_vx, *vw = full_vw;
}

// Current level name for logs ("-" when not in play).
const char *game_level_name(void) {
    extern int game_in_play(void);
    if (!game_in_play()) return "-";  // (also another game on the engine: no LA state to read)
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return game_in_play() && ps && *(const char *)(uintptr_t)ps ? (const char *)(uintptr_t)ps : "-";
}
