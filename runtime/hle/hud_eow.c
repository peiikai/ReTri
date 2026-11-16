// Echoes of Wisdom's HUD on phones and tablets (as runtime/hle/hud_corners.c does LA's): the hearts go to the screen's
// upper left corner, the mini map from the lower right to the upper right, and the HUD's Y / X item buttons to the
// left of the map, in a row with it. With "Hide X / Y on the HUD" (touch_hide_xy_hud) they're put out of
// sight while the on-screen controls (which show the same icons) are up: moved off the screen, not hidden, so the HUD
// still draws the Y slot's icon and the touch Y follows it (runtime/nvn/texpatch.c texpatch_eow_y_icon).
//
// EoW's HUD is several layouts, each its own pane tree (1920 x 1080 units, y up, origin in the middle):
//   HeartGaugeList  N_Life_00 (under N_HUD_00)                          the hearts, (-903, 514)
//   MiniMap         N_InOut_00 (child P_MiniMapBase_00, 300 x 300)        the map, (786, -386)
//   SetSlotPasteActor N_Item_00 (child L_CopySetItem_00 at (724, 442))  Y: the echo
//   SetSlotUseItem  N_Item_00 (child L_SetItem_00 at (866, 458))        X: an item
//   SetSlotLink     N_InOut_00 (children L_SetItem_00..02)              Swordfighter Form's buttons
// There is no hook in the HUD's code: the panes are found by their names in the game's memory (a scan on a thread of
// our own, again whenever one is gone), each checked by its parent and children. The game thread (hud_eow_tick, at
// nvnWindowAcquireTexture, once a frame) moves them: the game's own position plus the move, every frame.
// nn::ui2d::Pane as in LA (hud_corners.c): parent +0x18, children +0x20 (next +0x28), translate +0x30, flags +0x58
// (0x10: matrix dirty), name +0xB0.
// The draws are widened into the screen's bars by hud_corners.c's render-thread part (hud_corners_set_other).
#include "../game_addresses.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

float widescreen_factor(void);
void hud_corners_set_other(bool active, float left, float right, float up);  // runtime/hle/hud_corners.c
extern int kernel_data_regions(uint64_t *a, uint64_t *s, int max);
extern uint64_t renderer_present_count(void);
#if PLAT_TOUCH
bool touch_visible(void);
void menu_safe_insets(float *l, float *r);
#endif

enum { HEARTS, MAP, SLOT_Y, SLOT_X, SLOT_LINK, NPANE };
static const struct { const char *name, *parent, *child; } kPane[NPANE] = {
    {"N_Life_00", "N_HUD_00", "A_alignment_00"},
    {"N_InOut_00", "RootPane", "P_MiniMapBase_00"},
    {"N_Item_00", "RootPane", "L_CopySetItem_00"},
    {"N_Item_00", "RootPane", "L_SetItem_00"},
    {"N_InOut_00", "RootPane", "L_SetItem_02"},
};
static _Atomic uint64_t g_pane[NPANE];
// (the game's own translate, and the one written here: the game animates some of these panes - in / out, idle - so a
// value not ours is the game's new one, and ours is its value plus the move)
static float g_ax[NPANE], g_ay[NPANE], g_sx[NPANE], g_sy[NPANE];
static uint64_t g_was[NPANE];  // (the pane those are for)
static atomic_bool g_scanning;
static volatile float g_slot_x[2], g_slot_y[2];  // (where the Y and X buttons' icons are drawn: layout units)
static volatile bool g_slot_known[2];
static uint64_t g_next_scan;

static uint64_t g_ra[256], g_rs[256];
static int g_nr;
static bool mapped(uint64_t p, uint64_t n) {
    for (int k = 0; k < g_nr; k++)
        if (p >= g_ra[k] && p + n > p && p + n <= g_ra[k] + g_rs[k]) return true;
    return false;
}
static bool name_is(uint64_t p, const char *n) {
    return mapped(p, 0xC8) && !strncmp((const char *)(uintptr_t)(p + 0xB0), n, 24);
}
static bool has_child(uint64_t p, const char *n) {
    uint64_t root = p + 0x20, node = LD64(root + 8);
    for (int i = 0; i < 64 && node != root; i++, node = LD64(node + 8)) {
        if (!mapped(node, 16)) return false;
        uint64_t k = node - 8;
        if (name_is(k, n)) return true;
    }
    return false;
}
static bool is_pane(uint64_t p, int i) {
    if (!p || !name_is(p, kPane[i].name)) return false;
    uint64_t up = LD64(p + 0x18);
    return mapped(up, 0xC8) && name_is(up, kPane[i].parent) && has_child(p, kPane[i].child);
}

// One pass over the game's data, a word at a time: a pane's name is 8-aligned (+0xB0 in an 8-aligned object), so each
// word is compared with the names' first 8 bytes. With some panes known, only near them (the game's UI heap).
static void *scan(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), lo = 0, hi = ~0ull;
    for (int i = 0; i < NPANE; i++)
        if (g_pane[i]) {
            uint64_t p = g_pane[i];
            if (!lo || p - (256ull << 20) < lo) lo = p - (256ull << 20);
            if (hi == ~0ull || p + (256ull << 20) > hi) hi = p + (256ull << 20);
        }
    uint64_t key[NPANE];
    for (int i = 0; i < NPANE; i++) memcpy(&key[i], kPane[i].name, 8);
    int found = 0;
    for (int r = 0; r < g_nr; r++) {
        uint64_t a = g_ra[r] > lo ? g_ra[r] : lo, e = g_ra[r] + g_rs[r] < hi ? g_ra[r] + g_rs[r] : hi;
        a = (a + 7) & ~7ull;
        for (const uint64_t *w = (const uint64_t *)(uintptr_t)a, *end = (const uint64_t *)(uintptr_t)e; w < end; w++) {
            uint64_t v = *w;
            if (v != key[HEARTS] && v != key[MAP] && v != key[SLOT_Y]) continue;  // (the other names share these)
            uint64_t p = (uint64_t)(uintptr_t)w - 0xB0;
            if (p < g_ra[r]) continue;
            for (int i = 0; i < NPANE; i++)
                if (!g_pane[i] && v == key[i] && is_pane(p, i)) g_pane[i] = p, found++;
        }
    }
    LOG("[hud] EoW: %d more panes found in %.0f ms\n", found, (plat_now_ns() - t0) / 1e6);
    g_scanning = false;
    return NULL;
}

static void set_xy(int i, uint64_t p, float x, float y) {
    float *t = (float *)(uintptr_t)(p + 0x30);
    g_sx[i] = x, g_sy[i] = y;
    if (t[0] == x && t[1] == y) return;
    t[0] = x, t[1] = y;
    *(uint8_t *)(uintptr_t)(p + 0x58) |= 0x10;
}

// Where the HUD's Y (slot 0) or X (1) button's icon is drawn now (layout units): false while its pane isn't known
bool hud_eow_slot_at(int slot, float *x, float *y) {
    if (slot < 0 || slot > 1 || !g_slot_known[slot] || !g_pane[slot ? SLOT_X : SLOT_Y]) return false;
    *x = g_slot_x[slot], *y = g_slot_y[slot];
    return true;
}

void hud_eow_tick(void) {
#if PLAT_TOUCH
    bool on = !ENV("LARECOMP_NO_HUD_CORNERS");
#else
    bool on = ENV("LARECOMP_TOUCH_HUD") != 0;  // (test: the touch layout on a computer)
#endif
    if (!on || g_scanning) return;
    g_nr = kernel_data_regions(g_ra, g_rs, 256);
    bool missing = false;
    for (int i = 0; i < NPANE; i++) {
        if (g_pane[i] && !is_pane(g_pane[i], i)) g_pane[i] = 0;  // (gone: its memory may be something else now)
        missing |= !g_pane[i];
    }
    uint64_t now = renderer_present_count();
    // (a scan every 2 seconds while the hearts, the map or Y are missing; X and Swordfighter Form's buttons may never
    // be there: every 30)
    bool needed = !g_pane[HEARTS] || !g_pane[MAP] || !g_pane[SLOT_Y];
    if (missing && now >= g_next_scan) {
        g_next_scan = now + (needed ? 120 : 1800);
        g_scanning = true;
        pthread_t t;
        if (pthread_create(&t, NULL, scan, NULL) == 0) pthread_detach(t);
        else g_scanning = false;
        return;
    }
    // where things go, in layout units: the screen's spare height above the 16:9 band (a tablet), its spare width each
    // side of it (a phone, less the camera cutout)
    float f = widescreen_factor(), up = 0, l = 0, r = 0;
    if (f < 0.99f) up = 540 * (1 / f - 1);
    else if (f > 1.01f) {
        float il = 0, ir = 0;
#if PLAT_TOUCH
        menu_safe_insets(&il, &ir);
#endif
        float span = 1920 * f;
        l = 960 * (f - 1) - il * span, r = 960 * (f - 1) - ir * span;
        if (l < 0) l = 0;
        if (r < 0) r = 0;
    }
    bool hide = false;
#if PLAT_TOUCH
    hide = g_settings.touch_hide_xy_hud && touch_visible();
#endif
    if (ENV("LARECOMP_HIDE_XY_HUD")) hide = atoi(ENV("LARECOMP_HIDE_XY_HUD")) != 0;  // (test: 1 hidden, 0 shown)
    // the map: from the lower right to the upper right, its middle level with the buttons' (Y 442, X 458); the buttons
    // stay at the game's height and move left past the map's left edge (the X button's right edge, 866 + 88, to
    // 786 - 150 - 20). Only the hearts go up into a tablet's spare height: the Y button's icon lifted there showed
    // above its disc on the iPad (not on a Mac, at the same window shape; the icon's draw had the disc's position).
    const float map_dy = 450 + 386, slot_dx = (786 - 150 - 20) - (866 + 88);
    for (int i = 0; i < NPANE; i++) {
        uint64_t p = g_pane[i];
        if (!p) continue;
        float cx = *(const float *)(uintptr_t)(p + 0x30), cy = *(const float *)(uintptr_t)(p + 0x34);
        if (g_was[i] != p) g_was[i] = p, g_ax[i] = cx, g_ay[i] = cy;
        else {
            if (cx != g_sx[i]) g_ax[i] = cx;
            if (cy != g_sy[i]) g_ay[i] = cy;
        }
        float x = g_ax[i], y = g_ay[i] + (i == HEARTS ? up : 0);
        if (i == HEARTS) x -= l;
        else if (i == MAP) x += r, y += map_dy;
        else x += r + slot_dx + (hide ? 4000 : 0);  // (hidden: drawn far off the screen, so its icon still shows on Y)
        set_xy(i, p, x, y);
        if (i == SLOT_Y || i == SLOT_X) {  // (their buttons: the pane's offset from authored plus the part's place)
            int s = i == SLOT_X;
            g_slot_x[s] = (s ? 866 : 724) + x - g_ax[i], g_slot_y[s] = (s ? 458 : 442) + y - g_ay[i], g_slot_known[s] = true;
        }
    }
    hud_corners_set_other(up > 0 || l > 0 || r > 0, l, r, up);
}
