// Which page of LA's pause menu is up (Items, Map, dungeon map), and touch taps that move its cursors. The pages' own
// updates report themselves through tools/patches.py ITEMS_PAGE_SITE, PAUSE_MENU_SITE, MAP_PAGE_SITE and DGN_MAP_SITE;
// D-pad items (runtime/hle/dpad_items.c, dpad_overlay.mm), the run timer (race_overlay.mm) and the touch controls
// (mobile/touch_controls.mm) read them here.
#include "../game_addresses.h"
#include <stdint.h>
#include <stdlib.h>

#include <atomic>

extern "C" {
#include "../hle.h"
uint64_t renderer_present_count(void);
Cpu *kernel_current_cpu(void);
}

static bool recent(uint64_t f) { return f && renderer_present_count() - f <= 2; }

// ---- the Items page (x0 of ITEMS_PAGE_SITE; +0x4020 u32 = its cursor, 0-11, 4 columns)
static volatile uint64_t g_items_frame;
static uint64_t g_items_obj;

// tools/patches.py PAUSE_MENU_SITE: the pause menu's update ran (any page, and between pages)
static std::atomic<uint64_t> g_pause_frame{0};

RT_ON_RESET(pause_pages_reset) { g_items_frame = 0, g_items_obj = 0, g_pause_frame = 0; }

extern "C" void rt_items_page_tick(void) {
    g_items_frame = renderer_present_count();
    Cpu *c = kernel_current_cpu();
    if (c) g_items_obj = c->x[0];
}
extern "C" void rt_pause_menu_tick(void) { g_pause_frame = renderer_present_count(); }
// mobile/touch_controls.mm: the pause menu is up (L / R there turn its pages, not the boots / shield)
extern "C" bool pause_menu_shown(void) { return recent(g_pause_frame); }
// race_overlay.mm, dpad_overlay.mm: the pause menu's Items page is up
extern "C" bool inventory_items_page_shown(void) { return recent(g_items_frame); }
// The grid cursor while the Items page is shown, else -1
extern "C" int inventory_items_cursor(void) {
    if (!inventory_items_page_shown() || !g_items_obj) return -1;
    uint32_t i = LD32(g_items_obj + 0x4020);
    return i < 12 ? (int)i : -1;
}
// dpad_items.c: the Items page's object (its cells: +0x598 + 0x280 * cell = the cell's item slot)
extern "C" uint64_t inventory_items_obj(void) { return g_items_obj; }

// A tap on an Items page cell or a file select file (runtime/ui/mobile/touch_controls.mm): where the next cursor move
// goes, whatever the direction pressed, so the cursor goes straight there with the game's own move (tools/patches.py
// ITEMS_NAV_SITE, FILE_SELECT_NAV_SITE). An ask lapses after 15 frames, so a later press of the player's own is never
// taken over. LARECOMP_ITEMS_JUMP / LARECOMP_FILE_JUMP=<n> (tests): the first move there goes to n.
struct NavJump {
    std::atomic<int> to{-1};
    std::atomic<uint64_t> frame{0};
    int count;
    const char *test_env;
    bool test_read, test;
    void ask(int n) { frame = renderer_present_count(), to = n >= 0 && n < count ? n : -1; }
    int take(void) {  // (game thread) the place asked for, or -1
        if (!test_read) {
            test_read = true;
            if (const char *e = getenv(test_env)) test = true, to = atoi(e);
        }
        int n = to.exchange(-1);
        if (n < 0 || n >= count || (!test && renderer_present_count() - frame > 15)) return -1;
        test = false;
        return n;
    }
};
static NavJump g_items_jump{{-1}, {0}, 12, "LARECOMP_ITEMS_JUMP", false, false};
static NavJump g_file_jump{{-1}, {0}, 4, "LARECOMP_FILE_JUMP", false, false};
extern "C" void inventory_items_jump(int cell) { g_items_jump.ask(cell); }
extern "C" void file_select_jump(int file) { g_file_jump.ask(file); }
// (game thread) the Items page: the cell a move goes to, the neighbour table's or the one a tap asked for
extern "C" uint32_t rt_items_nav_pick(uint32_t next) {
    int cell = g_items_jump.take();
    if (cell < 0) return next;
    LOG("[items] tap: cursor to cell %d\n", cell);
    return (uint32_t)cell;
}
// (game thread) the file select, after its list moved the cursor or not: the file a tap asked for instead. Moved: the
// game's own change of file then runs (the old file at cursor + 1).
extern "C" int rt_file_nav_pick(uint64_t cursor, int moved) {
    int file = g_file_jump.take();
    if (file < 0) return moved;
    ST8(cursor, (uint8_t)file);
    LOG("[files] tap: cursor to file %d\n", file);
    return LD8(cursor) != LD8(cursor + 1);
}

// ---- the Map page (main+0xe93800) and the dungeon map page (main+0xe78740), x0 = the page. A page's state byte
// (+0xfd: 0x01 / 0x10 opening, 0x02 shown, 0x0f closing, 0x00 closed) says it is closing as soon as it fades out.
// The objects are guest memory the game frees when the page closes: only the game thread reads them.
static volatile uint64_t g_map_frame, g_map_obj, g_dm_frame, g_dm_obj;
static volatile bool g_map_closing, g_dm_closing;
static bool page_closing(uint64_t obj) {
    uint8_t st = LD8(obj + 0xfd);
    return st == 0x0f || st == 0x00;
}
RT_ON_RESET(pause_maps_reset) { g_map_frame = g_map_obj = g_dm_frame = g_dm_obj = 0, g_map_closing = g_dm_closing = false; }

extern "C" void rt_map_page_tick(Cpu *c) {
    g_map_frame = renderer_present_count();
    g_map_obj = c->x[0];
    g_map_closing = page_closing(c->x[0]);
}
extern "C" void rt_dgnmap_tick(Cpu *c) {
    g_dm_frame = renderer_present_count();
    g_dm_obj = c->x[0];
    g_dm_closing = page_closing(c->x[0]);
}
// mobile/touch_controls.mm: a map page is up and not closing
extern "C" bool map_page_shown(void) { return !g_map_closing && recent(g_map_frame); }
extern "C" bool dgnmap_shown(void) { return !g_dm_closing && recent(g_dm_frame); }
// runtime/hle/warp.c LARECOMP_PANE_SCAN: the pages' objects (addr 2, addr 1)
extern "C" uint64_t map_page_obj(void) { return g_map_obj; }
extern "C" uint64_t dgnmap_obj(void) { return g_dm_obj; }
