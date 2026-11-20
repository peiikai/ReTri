// Per-instruction replacements applied by the generator (port of tools/patches.py): guest address -> function
// returning C code, plus hook sites (the instruction stays; runtime/hle/hooks.c decides at run time).
//
// Every patch checks the original instruction word, so a wrong address in a version table fails loudly.
// The sites come from the selected version's address table (runtime/game_addresses/patches_<key>.h).
#include "gen_internal.hpp"

#include <algorithm>
#include <cstdlib>

namespace lasgen {

thread_local const AddressTable *PATCH_ADDRESSES = nullptr;

namespace {

uint64_t A(const char *name) {
    if (!PATCH_ADDRESSES) throw GenError("patches: no address table selected");
    auto it = PATCH_ADDRESSES->find(name);
    if (it == PATCH_ADDRESSES->end()) throw GenError(std::string("AttributeError: ") + name);
    return it->second;
}

std::string at(const std::string &code) { return addr_text(*PATCH_ADDRESSES, code); }

// f'{v:#x}'
std::string ax(uint64_t v) { return "0x" + hexs(v); }

void check(bool ok, uint64_t a, uint32_t w) {
    if (!ok) throw GenError("AssertionError: patch " + ax(a) + ": " + ax(w));
}

// Per-site data tables, filled by build_patches (patches.py module-level dicts; later keys win, as in a dict)
struct Tables {
    std::map<uint64_t, int> aspect_sites;
    std::map<uint64_t, std::pair<uint32_t, int>> fovy_sites;
    struct Sky { uint32_t want; int off; const char *sign; };
    std::map<uint64_t, Sky> skysea_box_sites;
    std::map<uint64_t, int> warp_request_sites;
    std::vector<uint64_t> equip_y_read_sites;
    std::map<uint64_t, std::pair<int, int>> walk_speed_sites;
    struct NoHandler { uint32_t want; std::string code; const char *what; };
    std::map<uint64_t, NoHandler> no_handler_sites;
};
thread_local Tables T;

// Widescreen: the game's camera code loads a 16:9 aspect-ratio literal as the default projection aspect. These loads
// are redirected to rt_aspect_site(), which returns the aspect for the current output shape (or 16:9) per site, so
// only the 3D cameras widen.
std::string aspect_load(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffc00000) == 0xbd400000 && ((w >> 10) & 0xfff) * 4 == 0xba4, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_aspect_site(int); float f = rt_aspect_site(", T.aspect_sites.at(a), "); ",
               "c->v[", rt, "] = 0; memcpy(&c->v[", rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", =16/9 */");
}

// Off-screen culling: the scene's visibility pass loads the camera's aspect into s9; we load the runtime aspect
// instead (site 6).
std::string cull_aspect(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xbd457369, a, w);  // ldr s9, [x27, #0x570]
    return cat("{ extern float rt_cull_aspect(float); float o; memcpy(&o, (const void *)(uintptr_t)(c->x[27] + 0x570), 4); ",
               "float f = rt_cull_aspect(o); c->v[9] = 0; memcpy(&c->v[9], &f, 4); }",
               "  /* ", ax(a), ": ldr s9, [x27, #0x570] (culling aspect) */");
}

// Vertical field of view (camera field +0x564): taller-than-16:9 outputs keep the designed horizontal view and widen
// the vertical one ("Vert+").
std::string fovy_load(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto [want, rn] = T.fovy_sites.at(a);
    check(w == want, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_fovy(float); float o; ",
               "memcpy(&o, (const void *)(uintptr_t)(c->x[", rn, "] + 0x564), 4); float f = rt_fovy(o); ",
               "c->v[", rt, "] = 0; memcpy(&c->v[", rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", [x", rn,
               ", #0x564] (fovy) */");
}

// Sky and sea beyond the overworld's north edge: each piece is drawn only while Link stands inside its box; with a
// wider view the next piece is needed sooner. The box's across bounds are widened by the extra view
// (runtime/hle/widescreen.c rt_skysea_margin).
std::string skysea_box(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto s = T.skysea_box_sites.at(a);
    check(w == s.want, a, w);  // ldr qN, [x23, #off]
    int rt = w & 31;
    return cat("{ extern float rt_skysea_margin(void); float b[4]; ",
               "memcpy(b, (const void *)(uintptr_t)(c->x[23] + ", ax(s.off), "), 16); b[0] += ", s.sign,
               "f * rt_skysea_margin(); ", "memcpy(&c->v[", rt, "], b, 16); }  /* ", ax(a), ": ldr q", rt, ", [x23, #",
               ax(s.off), "] (sky / sea piece box) */");
}

// Indoor detection: the `ldr s1, =16/9` in the indoor shadow setup keeps loading 16:9 and tells the runtime an
// interior is being rendered.
std::string indoor(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffc00000) == 0xbd400000 && ((w >> 10) & 0xfff) * 4 == 0xba4, a, w);
    int rt = w & 31;
    return cat("{ extern float rt_indoor_aspect(void); float f = rt_indoor_aspect(); ", "c->v[", rt, "] = 0; memcpy(&c->v[",
               rt, "], &f, 4); }  /* ", ax(a), ": ldr s", rt, ", =16/9 (indoor shadow) */");
}

// Warp support: the level-jump request object registers itself in its parent while a level loads; record it so
// runtime/hle/warp.c can fill the request itself.
std::string warp_request(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    int rt = T.warp_request_sites.at(a);
    check(w == (0xf900e100u | (uint32_t)rt), a, w);  // str xRT, [x8, #0x1c0]
    return cat("{ extern void rt_warp_request_obj(uint64_t); ", "*(uint64_t *)(uintptr_t)(c->x[8] + 0x1c0) = c->x[", rt,
               "]; rt_warp_request_obj(c->x[", rt, "]); }", "  /* ", ax(a), ": str x", rt,
               ", [x8, #0x1c0] (warp: level-jump request) */");
}

// Difficulty (runtime/ui/difficulty.mm): Link's damage is `damage << hero` at these `lsl Wd, Wn, Wm` sites, then
// subtracted from health. Each is redirected to rt_damage(damage, shift) so the difficulty multiplier / one-hit KO
// applies.
std::string damage_lsl(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffe0fc00) == 0x1ac02000, a, w);  // lslv (32-bit)
    int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    return cat("{ extern int32_t rt_damage(int32_t, int32_t); c->x[", rd, "] = (uint32_t)rt_damage((int32_t)c->x[", rn,
               "], ", "(int32_t)(c->x[", rm, "] & 31)); }  /* ", ax(a), ": lsl w", rd, ", w", rn, ", w", rm, " (damage) */");
}

// Fixed damage (-2 quarter hearts, -4 in Hero Mode: `csel w20, w10, w9, eq` with w10=-4, w9=-2).
std::string damage_fixed(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xffe00c00) == 0x1a800000 && (w & 31) == 20, a, w);  // csel w20, ...
    return cat("{ extern int32_t rt_damage(int32_t, int32_t); extern int rt_hero_mode(void); ",
               "c->x[20] = (uint32_t)(-rt_damage(2, rt_hero_mode())); }", "  /* ", ax(a), ": csel w20, -4, -2 (fixed damage) */");
}

// Inventory overlay: runs once per frame only while the pause menu's Items page is shown.
std::string items_page(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_items_page_tick(void); rt_items_page_tick(); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (items page shown) */");
}

// Touch controls: the pause menu's update, once per frame on every page.
std::string pause_menu(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10383ff, a, w);  // sub sp, sp, #0xe0
    return cat("{ extern void rt_pause_menu_tick(void); rt_pause_menu_tick(); c->sp -= 0xe0; }", "  /* ", ax(a),
               ": sub sp, sp, #0xe0 (pause menu shown) */");
}

// The Items page's cursor move: a tap on a cell asks for that cell, through the game's own move.
std::string items_nav(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb9400116, a, w);  // ldr w22, [x8]
    return cat("{ extern uint32_t rt_items_nav_pick(uint32_t); c->x[22] = rt_items_nav_pick(LD32(c->x[8])); }", "  /* ",
               ax(a), ": ldr w22, [x8] (items page: the next cell) */");
}

// The file select's cursor: a tap on a file puts the cursor there right after the list helper.
std::string file_select_nav(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x370003c0, a, w);  // tbnz w0, #0, <moved>
    uint64_t t = a + ((w >> 5) & 0x3FFF) * 4;
    return cat("{ extern int rt_file_nav_pick(uint64_t cursor, int moved); ",
               "if (rt_file_nav_pick(c->x[19], (int)(c->x[0] & 1))) goto L_", hexs(t), "; }", "  /* ", ax(a),
               ": tbnz w0, #0 (file select: the cursor moved) */");
}

// Check tracker: runs once per frame only while the pause menu's Map page is shown.
std::string map_page(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10203ff, a, w);  // sub sp, sp, #0x80
    return cat("{ extern void rt_map_page_tick(Cpu *); rt_map_page_tick(c); c->sp -= 0x80; }", "  /* ", ax(a),
               ": sub sp, sp, #0x80 (map page shown) */");
}

// HUD hearts refresh (runtime/hle/hud_hearts.c): reruns the HUD's own set-up when a menu changed Link's hearts.
std::string hud_hearts(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd102c3ff, a, w);  // sub sp, sp, #0xb0
    return cat("{ extern void rt_hud_hearts_tick(Cpu *); rt_hud_hearts_tick(c); c->sp -= 0xb0; }", "  /* ", ax(a),
               ": sub sp, sp, #0xb0 (HUD update: hearts refresh) */");
}

// Chaos map letters: runs once per frame while the DUNGEON map page is shown.
std::string dgn_map(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_dgnmap_tick(Cpu *); rt_dgnmap_tick(c); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (dungeon map page shown) */");
}

// Debug (LARECOMP_LOG_RESNAME=1): logs resource names looked up by name.
std::string res_name(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10503ff, a, w);  // sub sp, sp, #0x140
    return cat("{ extern void rt_log_resname(uint64_t); rt_log_resname(c->x[2]); c->sp -= 0x140; }", "  /* ", ax(a),
               ": sub sp, sp, #0x140 (resource-by-name debug log) */");
}

// Frame pacing (runtime/hle/framepace.c): the frame timer gets the number of vsyncs that really passed.
std::string frame_steps(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb8696908, a, w);  // ldr w8, [x8, x9]
    return cat("{ extern uint32_t rt_frame_steps(uint32_t interval); ",
               "c->x[8] = rt_frame_steps(LD32(c->x[8] + c->x[9])); }", "  /* ", ax(a),
               ": ldr w8, [x8, x9] (swap interval -> vsyncs elapsed: runtime/hle/framepace.c) */");
}

// The game's own dynamic resolution, off: resolution is ours to choose (runtime/nvn/output_scale.c).
std::string game_dynres(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x395922e8, a, w);  // ldrb w8, [x23, #0x648]
    return cat("{ extern int rt_game_dynres(void); ", "c->x[8] = rt_game_dynres() ? LD8(c->x[23] + 0x648) : 0; }", "  /* ",
               ax(a), ": ldrb w8, [x23, #0x648] (the game's dynamic resolution: off) */");
}

std::string game_dynres_init(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb86d698c, a, w);  // ldr w12, [x12, x13] (x13 = 0x6e4c)
    return cat("{ extern int rt_game_dynres(void); ", "c->x[12] = rt_game_dynres() ? LD32(c->x[12] + c->x[13]) : 0; }",
               "  /* ", ax(a), ": ldr w12, [x12, x13] (the game's starting resolution level: full size) */");
}

// D-pad items (runtime/hle/dpad_items.c): the d-pad as a third set of item buttons, in the game's own "use it now?"
// checks. The Y test also passes for a newly pressed d-pad direction, and the read then answers the item itself.
std::string equip_y_test(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xFFF8001F) == 0x37180008, a, w);  // tbnz w8, #3, <read>
    int64_t disp = (int64_t)(((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000;
    uint64_t t = a + (uint64_t)(disp * 4);
    return cat("{ extern int rt_equip_y_test(uint32_t pressed); if (rt_equip_y_test((uint32_t)c->x[8])) goto L_", hexs(t),
               "; }", "  /* ", ax(a), ": tbnz w8, #3 (Y newly pressed, or a d-pad direction) */");
}

const uint32_t EQUIP_Y_READ_SLOTS[12] = {0xf, 0xe, 0xd, 0x6, 0x5, 0x4, 0x3, 0x2, 0x7, 0xc, 0x8, 0x9};  // (each check's item)

std::string equip_y_read(Module &m, uint64_t a) {
    uint64_t off = a - m.base;
    uint32_t w = m.image_word(a);
    check((w & 0xFFFFFFE0) == 0x39428900 && (w & 31) < 30, a, w);  // ldrb wN, [x8, #0xa2]
    size_t idx = 0;
    while (idx < T.equip_y_read_sites.size() && T.equip_y_read_sites[idx] != a) idx++;
    if (idx >= T.equip_y_read_sites.size() || idx >= 12) throw GenError("ValueError: equip_y_read site");
    uint32_t slot = EQUIP_Y_READ_SLOTS[idx];
    if (off + 8 <= m.nso.image.size()) {
        uint32_t n = m.nso.u32(off + 4);
        check(n == (0x7100001Fu | slot << 10 | (w & 31) << 5), a, n);  // cmp wN, #<slot>
    }
    return cat("{ extern uint32_t rt_equip_y_read(uint64_t ps, int slot); c->x[", (int)(w & 31),
               "] = rt_equip_y_read(c->x[8], ", slot, "); }", "  /* ", ax(a), ": ldrb w", (int)(w & 31),
               ", [x8, #0xa2] (item on Y; item ", ax(slot), " on a d-pad direction) */");
}

// No Guardian Acorns / Pieces of Power (runtime/hle/qol.c rt_actor_create_fn): the actor factory's create function
// lookup; a null fn is its "no such actor" path. (The site's name is from LA's randomizer, which held back bosses here.)
std::string boss_spawn(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf9400118, a, w);  // ldr x24, [x8]
    if (a - 4 >= m.base) {  // (the address table test gives this one word only)
        w = m.image_word(a - 4);
        check(w == 0x9b086ee8, a, w);  // madd x8, x23, x8, x27
    }
    return cat("{ extern uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type); ",
               "c->x[24] = rt_actor_create_fn(LD64(c->x[8]), (uint32_t)c->x[23]); }", "  /* ", ax(a),
               ": ldr x24, [x8] (actor create fn: no-buff-drops) */");
}

// Hookshot anything (runtime/hle/hookshot.c): the attribute word of the surface the hookshot's ray hit.
std::string hookshot_attr(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb9403bf7, a, w);  // ldr w23, [sp, #0x38]
    return cat("{ extern uint32_t rt_hookshot_attr(Cpu *, uint32_t); ", "c->x[23] = rt_hookshot_attr(c, LD32(c->sp + 0x38)); }",
               "  /* ", ax(a), ": ldr w23, [sp, #0x38] (hookshot hit attribute) */");
}

// Hookshot 8-way aim (runtime/hle/hookshot.c): the runtime can round the firing direction to one of 8 first.
std::string hookshot_dir(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3d80be60, a, w);  // str q0, [x19, #0x2f0]
    return cat("{ extern void rt_hookshot_dir(Cpu *); rt_hookshot_dir(c); memcpy((void *)(uintptr_t)(c->x[19] + 0x2f0), &c->v[0], 16); }",
               "  /* ", ax(a), ": str q0, [x19, #0x2f0] (hookshot firing direction) */");
}

// 360-degree movement (runtime/hle/qol.c): the runtime returns the stick's own angle instead of the 8-way snap.
std::string snap_dir(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10103ff, a, w);  // sub sp, sp, #0x40
    return cat("{ extern int rt_snap_dir(Cpu *); if (rt_snap_dir(c)) return; c->sp -= 0x40; }", "  /* ", ax(a),
               ": sub sp, sp, #0x40 (8-way stick snap: 360-degree movement) */");
}

// Walk speed (runtime/hle/qol.c): rt_walk_speed multiplies the walk speed multiplier in s0 by the setting. Jump
// distances stay the game's own.
std::string walk_speed(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto [rn, site] = T.walk_speed_sites.at(a);
    check(w == (0xf94ca408u | (uint32_t)rn << 5), a, w);  // ldr x8, [x19|x20, #0x1948]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); ",
               "m_ = rt_walk_speed(c->x[", rn, "], m_, ", site, "); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); ",
               "c->x[8] = LD64(c->x[", rn, "] + 0x1948); }", "  /* ", ax(a), ": ldr x8, [x", rn,
               ", #0x1948] (walk speed multiplier in s0) */");
}

// Wind push (runtime/nvn/weather.c rt_wind_push): a push added to Link's frame displacement is stopped by walls and
// ledges like his walking.
std::string wind_push(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x38686a68, a, w);  // ldrb w8, [x19, x8]
    return cat("{ extern void rt_wind_push(Cpu *); rt_wind_push(c); c->x[8] = LD8(c->x[19] + c->x[8]); }", "  /* ", ax(a),
               ": ldrb w8, [x19, x8] (Link move: the wind push) */");
}

// Walk animation rate (walk state): the walk animation's play rate in s8.
std::string walk_anim(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf94ab668, a, w);  // ldr x8, [x19, #0x1568]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[8], 4); ",
               "m_ = rt_walk_speed(c->x[19], m_, 100); c->v[8] = 0; memcpy(&c->v[8], &m_, 4); ",
               "c->x[8] = LD64(c->x[19] + 0x1568); }", "  /* ", ax(a), ": ldr x8, [x19, #0x1568] (walk animation rate in s8) */");
}

// The same for the shield walk.
std::string shield_anim(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xf94006c8, a, w);  // ldr x8, [x22, #8]
    return cat("{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); ",
               "m_ = rt_walk_speed(c->x[20], m_, 101); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); ",
               "c->x[8] = LD64(c->x[22] + 8); }", "  /* ", ax(a), ": ldr x8, [x22, #8] (shield walk animation rate in s0) */");
}

// No Guardian Acorns / Pieces of Power (Gameplay tab, runtime/hle/qol.c): the drop roll reads its ItemDrop row's
// mAllowBuffReplace as false with the setting on.
std::string buff_replace(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3940e308, a, w);  // ldrb w8, [x24, #0x38]
    return cat("{ extern int rt_buff_replace(int); c->x[8] = (uint32_t)rt_buff_replace(LD8(c->x[24] + 0x38)); }", "  /* ",
               ax(a), ": ldrb w8, [x24, #0x38] (mAllowBuffReplace: no-buff-drops) */");
}

// Blur removal (runtime/hle/qol.c): qol.c keeps the tilt-shift object and takes the parameter handle out / puts it
// back live.
std::string tilt_init(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xd10243ff, a, w);  // sub sp, sp, #0x90
    return cat("{ extern void rt_tilt_obj(uint64_t); rt_tilt_obj(c->x[0]); c->sp -= 0x90; }", "  /* ", ax(a),
               ": sub sp, sp, #0x90 (tilt-shift set-up: blur removal) */");
}

// Event parameters: a missing String parameter of an event action throws on the Switch (caught there); recompiled
// code can't unwind. A missing String reads as "" instead.
std::string missing_string(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x321c03e0, a, w);  // mov w0, #0x10 (__cxa_allocate_exception size)
    // "" (the terminator of ".ptcl" right before the message; callers strlen() the result, so not NULL)
    return at(cat("{ ST64(c->sp + 0x18, 0x@EMPTY_STRING@ull); ST32(c->sp + 0x20, 0); goto L_@MISSING_STRING_RETURN@; }",
                  "  /* ", ax(a), ": mov w0, #0x10 (missing String event parameter: return \"\", no throw) */"));
}

// The Float version: a missing Float reads as 0.0 instead (the reader treats time <= 0 as instant).
std::string missing_float(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x321c03e0, a, w);  // mov w0, #0x10 (__cxa_allocate_exception size)
    // s0 = 0.0 (q0 at Cpu+512), then the function's own epilogue
    return cat("{ __builtin_memset((char *)c + 512, 0, 16);", " c->x[29] = LD64(c->sp + 48); c->x[30] = LD64(c->sp + 56);",
               " c->x[20] = LD64(c->sp + 32); c->x[19] = LD64(c->sp + 40); c->sp += 0x40; return; }", "  /* ", ax(a),
               ": mov w0, #0x10 (missing Float event parameter: return 0.0, no throw) */");
}

// Event actions with no handler: recompiled code can't unwind, so the throw goes straight into the catch block,
// without the C++ runtime's begin / end catch (nothing was thrown).
std::string no_handler(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    auto &s = T.no_handler_sites.at(a);
    check(w == s.want, a, w);
    return cat("{ ", s.code, " }  /* ", ax(a), ": ", s.what, " */");
}

// Mirror mode: the in-world action prompt's x is mirrored for that pane (runtime/hle/mirror.c).
std::string prompt_pos(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x2d060102, a, w);  // stp s2, s0, [x8, #0x30]
    return cat("{ extern float rt_prompt_x(uint64_t pane, float x); float px_, py_; memcpy(&px_, &c->v[2], 4); ",
               "memcpy(&py_, &c->v[0], 4); px_ = rt_prompt_x(c->x[8], px_); ",
               "memcpy((void *)(uintptr_t)(c->x[8] + 0x30), &px_, 4); memcpy((void *)(uintptr_t)(c->x[8] + 0x34), &py_, 4); }",
               "  /* ", ax(a), ": stp s2, s0, [x8, #0x30] (action prompt spot: mirror mode) */");
}

// Issue #45: the font's missing-character box glyph is marked without checking for none; no glyph skips the
// marking as the second lookup's check does.
std::string font_box_glyph(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x3940e008, a, w);  // ldrb w8, [x0, #0x38]
    return at("if (c->x[0] == 0) goto L_@FONT_BOX_GLYPH_SKIP@; c->x[8] = LD8(c->x[0] + 0x38);") + "  /* " + ax(a) +
           ": ldrb w8, [x0, #0x38] (font box glyph: none skips the marking) */";
}

// Issue #452: a character the font has no glyph for is drawn and measured as the box glyph, read without checking
// for none. No box glyph reads as an empty one (no width, nothing drawn).
std::string font_fallback(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    const char *as = nullptr, *op = nullptr;
    switch (w) {
        case 0x79409800: as = "ldrh w0, [x0, #0x4c]", op = "c->x[0] = LD16(c->x[0] + 0x4c);"; break;
        case 0xaa0003e9: as = "mov x9, x0", op = "c->x[9] = c->x[0];"; break;
        case 0x79409816: as = "ldrh w22, [x0, #0x4c]", op = "c->x[22] = LD16(c->x[0] + 0x4c);"; break;
        case 0x79409c08: as = "ldrh w8, [x0, #0x4e]", op = "c->x[8] = LD16(c->x[0] + 0x4e);"; break;
    }
    check(as != nullptr, a, w);
    return cat("{ static uint8_t empty_glyph_[0x60] __attribute__((aligned(16))); ",
               "if (c->x[0] == 0) c->x[0] = (uint64_t)(uintptr_t)empty_glyph_; } ", op, "  /* ", ax(a), ": ", as,
               " (font fallback glyph: none reads as an empty one) */");
}

// Issue #19: two delegate forwarders read a missing delegate as "nothing to do" (runtime/hle/delegate_null.c).
std::string delegate_forwarder(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xaa0003e1, a, w);  // mov x1, x0
    int slot = a == A("DELEGATE_FORWARDER_SITES_0") ? 0x28 : 0x30;
    return cat("{ extern int rt_delegate_null(Cpu *, int); if (rt_delegate_null(c, ", ax((uint64_t)slot),
               ")) return; c->x[1] = c->x[0]; }", "  /* ", ax(a), ": mov x1, x0 (delegate forwarder: nothing to forward to) */");
}

// ---- Echoes of Wisdom (gen.py --game eow): main's own patches. None of LA's apply (they are LA addresses); the sites
// are the build's own (runtime/game_addresses/eow_<table>.h, set by build_other_patches), each checked by its word.
std::vector<uint64_t> eow_aspect_sites, eow_spot_sites;
uint64_t eow_fps_limit_30;

// (warp, runtime/hle/warp_other.c) the level-jump request processor (WARP_REQUEST_PROC, x0 = its object, the request at
// +0x10): its `mov x20, x0` tells us where the request is
std::string eow_request(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xaa0003f4, a, w);  // mov x20, x0
    return cat("{ extern void rt_other_request_holder(uint64_t); rt_other_request_holder(c->x[0]); c->x[20] = c->x[0]; }",
               "  /* ", ax(a), ": mov x20, x0 (level-jump request processor) */");
}

// (blur removal, runtime/hle/qol.c) the renderer's per-frame set-up makes the tilt-shift object (renderer +0x600) once,
// the first time it's reached, as LA's main+0x1071220 does: name copy at +0x58, uniform buffer at +0x50. Its
// `add x19, x23, #0x600` hands qol.c the object.
std::string eow_tilt(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x911802f3, a, w);  // add x19, x23, #0x600
    return cat("{ extern void rt_tilt_obj(uint64_t); c->x[19] = c->x[23] + 0x600; rt_tilt_obj(c->x[19]); }", "  /* ",
               ax(a), ": add x19, x23, #0x600 (tilt-shift set-up: blur removal) */");
}

// (resolution, runtime/nvn/output_scale.c) the game's own dynamic resolution, off - as LA's GAME_DYNRES_SITE. Its frame
// controller (1.0.2: main+0x346530) reads the scaler's enabled byte (+0x480 of its object); on, it turns the GPU timing
// counters into a load figure and steps its 3D a level (1080 - 36 x level lines) after 6 samples over 90% / under 70%
// (1.0.2: main+0x3468c0). It started at 1408x792 and climbed to 1920x1080 on a fast GPU: our scale (Match display maps 792
// lines to the window) then drew the 3D at up to 1.9x the window's pixels (6982x1964 for 5120x1440), new targets at
// each step, and frames over 16.7 ms once our own window step (SMAA, upscale) came on top. Read as 0: it stays at
// 1408x792. LARECOMP_GAME_DYNRES=1 (test): the game's own again.
std::string eow_dynres(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x39520348, a, w);  // ldrb w8, [x26, #0x480]
    return cat("{ extern int rt_game_dynres(void); c->x[8] = rt_game_dynres() ? LD8(c->x[26] + 0x480) : 0; }", "  /* ",
               ax(a), ": ldrb w8, [x26, #0x480] (the game's dynamic resolution: off) */");
}

// (frame pacing, runtime/nvn/pace.c rt_other_fps_window) the frame controller at 30 fps, at the end of a 12-frame
// window, reads how many of its frames took over 1/60 s; 0 takes it back to 60. On phone and tablet GPUs its frames
// read slow at 30 by the lowered clock alone: the runtime may answer 0 to try 60
// (1.0.2: `ldr w9, [x26, #0x4fc]`; 1.0.0 keeps its counters 8 bytes lower: the offset is the instruction's)
std::string eow_fps_window(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xFFC003FF) == 0xB9400349, a, w);  // ldr w9, [x26, #off]
    uint64_t off = ((w >> 10) & 0xFFF) * 4;
    return cat("{ extern uint32_t rt_other_fps_window(uint32_t); c->x[9] = rt_other_fps_window(LD32(c->x[26] + ", ax(off),
               ")); }", "  /* ", ax(a), ": ldr w9, [x26, #", ax(off), "] (30 fps window: its slow frames) */");
}

// ... and at 60, its count of frames in a row over 1/60 s (at 1 or 2 it goes to 30): 0 for a moment after a try, while
// the clock comes up. 1.0.0 has no such count (one slow frame takes it to 30: the runtime's 60 fps limit gives the same
// grace there, rt_other_fps_limit)
std::string eow_slow60(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0xb945034a, a, w);  // ldr w10, [x26, #0x500]
    return cat("{ extern uint32_t rt_other_slow60(uint32_t); c->x[10] = rt_other_slow60(LD32(c->x[26] + 0x500)); }",
               "  /* ", ax(a), ": ldr w10, [x26, #0x500] (60 fps: frames over 1/60 s in a row) */");
}

// ... and the frame time it calls slow, 1/60 s (`movk w9, #0x3c88, lsl #16` after `mov w9, #0x8889`), in its 30 and its
// 60 fps branch: with Dynamic resolution the FPS target decides instead (rt_other_fps_limit)
std::string eow_fps_limit(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check(w == 0x72a79109, a, w);  // movk w9, #0x3c88, lsl #16
    int at30 = a == eow_fps_limit_30 ? 1 : 0;
    return cat("{ extern uint32_t rt_other_fps_limit(int); c->x[9] = rt_other_fps_limit(", at30, "); }", "  /* ", ax(a),
               ": movk w9, #0x3c88, lsl #16 (1/60 s: a slow frame, ", at30 ? "30" : "60", " fps branch) */");
}

// (widescreen, runtime/hle/widescreen.c rt_aspect_other) the code builds 16:9 (0x3fe38e39) as
// `movz wD, #0x8e39; movk wD, #0x3fe3, lsl #16`: the movk gets the runtime's aspect instead
std::string eow_aspect(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xFFFFFFE0) == 0x72A7FC60, a, w);  // movk wD, #0x3fe3, lsl #16
    int d = (int)(w & 31), idx = 0;
    while (eow_aspect_sites[idx] != a) idx++;
    return cat("{ extern uint32_t rt_aspect_other(int); c->x[", d, "] = rt_aspect_other(", idx, "); }", "  /* ", ax(a),
               ": movk w", d, ", #0x3fe3, lsl #16 (16:9) */");
}

// (widescreen, runtime/hle/widescreen.c rt_screen_spot) layouts placed at a target's spot on screen (the lock-on
// reticle L_LockOn_00 and the others below): the code works the spot out with the widened camera and turns it into the
// 16:9 layout's units (x 960, y 540 from the centre), then stores it as the root pane's translation,
// `stp sA, sB, [xN, #0x30]` = x, y. The runtime rescales them to the window's shape.
std::string eow_spot(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    check((w & 0xFFFF8000) == 0x2D060000, a, w);  // stp sA, sB, [xN, #0x30]
    int ra = (int)(w & 31), rn = (int)((w >> 5) & 31), rb = (int)((w >> 10) & 31), idx = 0;
    while (eow_spot_sites[idx] != a) idx++;
    return cat("{ extern void rt_screen_spot(int site, uint64_t pane, float *xy); float xy_[2]; ",
               "memcpy(&xy_[0], &c->v[", ra, "], 4); memcpy(&xy_[1], &c->v[", rb, "], 4); ",
               "rt_screen_spot(", idx, ", c->x[", rn, "], xy_); memcpy((void *)(uintptr_t)(c->x[", rn, "] + 0x30), xy_, 8); }",
               "  /* ", ax(a), ": stp s", ra, ", s", rb, ", [x", rn, ", #0x30] (a layout at a spot on screen: widescreen) */");
}

// (debug, GEN_TRACE_STORE_OFF) every `str xT, [xN, #off]` and `stp` whose either slot is at one of these offsets: the
// runtime hears of it (rt_trace_store: site, base, value) - finding where an object is registered
thread_local std::set<int64_t> trace_offs;
std::string trace_reg(int r) { return r == 31 ? "c->sp" : cat("c->x[", r, "]"); }
std::string trace_store(Module &m, uint64_t a) {
    uint32_t w = m.image_word(a);
    std::string base = trace_reg((int)((w >> 5) & 31));
    std::vector<std::pair<int64_t, int>> pairs;
    if ((w & 0xFFC00000) == 0xF9000000) {
        pairs.push_back({(int64_t)((w >> 10) & 0xFFF) * 8, (int)(w & 31)});
    } else {  // stp (signed offset)
        int64_t imm = (w >> 15) & 0x7F, off = (imm & 64 ? imm - 128 : imm) * 8;
        pairs.push_back({off, (int)(w & 31)});
        pairs.push_back({off + 8, (int)((w >> 10) & 31)});
    }
    auto val = [](int r) { return r == 31 ? std::string("0ull") : trace_reg(r); };
    std::string body, calls;
    for (auto &p : pairs) body += cat("*(uint64_t *)(uintptr_t)(", base, " + ", (long long)p.first, ") = ", val(p.second), "; ");
    body += "uint64_t b_ = " + base + "; ";
    for (auto &p : pairs)
        if (trace_offs.count(p.first))
            calls += cat("rt_trace_store(", ax(a), "ull, b_ + ", (long long)p.first, ", ", val(p.second), "); ");
    return "{ extern void rt_trace_store(uint64_t, uint64_t, uint64_t); " + body + calls + "}" + "  /* " + ax(a) +
           ": traced store */";
}

}  // namespace

PatchSet build_patches(const std::string &version_key) {
    PATCH_ADDRESSES = &address_table(version_key);
    T = Tables();
    PatchSet ps;
    auto &P = ps.patches;
    auto set = [&](uint64_t a, PatchFn f) { P[a] = f; };

    T.aspect_sites = {};
    for (int i = 0; i < 6; i++) T.aspect_sites[A(("ASPECT_SITES_" + std::to_string(i)).c_str())] = i;
    for (auto &e : T.aspect_sites) set(e.first, aspect_load);

    set(A("CULL_ASPECT"), cull_aspect);

    T.fovy_sites[A("FOVY_SITES_0")] = {0xbd4566e0, 23};
    T.fovy_sites[A("FOVY_SITES_1")] = {0xbd456768, 27};
    for (auto &e : T.fovy_sites) set(e.first, fovy_load);

    T.skysea_box_sites[A("SKYSEA_BOX_MAX_SITE")] = {0x3dc0bae1, 0x2e0, "1.0"};
    T.skysea_box_sites[A("SKYSEA_BOX_MIN_SITE")] = {0x3dc0b6e2, 0x2d0, "-1.0"};
    for (auto &e : T.skysea_box_sites) set(e.first, skysea_box);

    set(A("INDOOR_SITE"), indoor);

    T.warp_request_sites[A("WARP_REQUEST_SITES_0")] = 20;
    T.warp_request_sites[A("WARP_REQUEST_SITES_1")] = 19;
    for (auto &e : T.warp_request_sites) set(e.first, warp_request);


    for (const char *n : {"DAMAGE_LSL_SITES_0", "DAMAGE_LSL_SITES_1", "DAMAGE_LSL_SITES_2"}) set(A(n), damage_lsl);
    set(A("DAMAGE_FIXED_SITE"), damage_fixed);

    set(A("ITEMS_PAGE_SITE"), items_page);
    set(A("PAUSE_MENU_SITE"), pause_menu);
    set(A("ITEMS_NAV_SITE"), items_nav);
    set(A("FILE_SELECT_NAV_SITE"), file_select_nav);
    set(A("MAP_PAGE_SITE"), map_page);
    set(A("HUD_HEARTS_SITE"), hud_hearts);
    set(A("DGN_MAP_SITE"), dgn_map);
    set(A("RES_NAME_SITE"), res_name);
    set(A("FRAME_STEPS_SITE"), frame_steps);
    set(A("GAME_DYNRES_SITE"), game_dynres);
    set(A("GAME_DYNRES_INIT_SITE"), game_dynres_init);

    std::vector<uint64_t> y_test;
    for (int i = 0; i < 12; i++) y_test.push_back(A(("EQUIP_Y_TEST_SITES_" + std::to_string(i)).c_str()));
    for (int i = 0; i < 12; i++) T.equip_y_read_sites.push_back(A(("EQUIP_Y_READ_SITES_" + std::to_string(i)).c_str()));
    for (uint64_t a : y_test) set(a, equip_y_test);
    for (uint64_t a : T.equip_y_read_sites) set(a, equip_y_read);

    set(A("BOSS_SPAWN_SITE"), boss_spawn);
    set(A("HOOKSHOT_ATTR_SITE"), hookshot_attr);
    set(A("HOOKSHOT_DIR_SITE"), hookshot_dir);
    set(A("SNAP_DIR_SITE"), snap_dir);

    T.walk_speed_sites[A("WALK_SPEED_SITES_0")] = {19, 0};
    T.walk_speed_sites[A("WALK_SPEED_SITES_1")] = {19, 1};
    T.walk_speed_sites[A("WALK_SPEED_SITES_2")] = {20, 2};
    T.walk_speed_sites[A("WALK_SPEED_SITES_3")] = {20, 3};
    for (auto &e : T.walk_speed_sites) set(e.first, walk_speed);

    set(A("WIND_PUSH_SITE"), wind_push);
    set(A("WALK_ANIM_SITE_0"), walk_anim);
    set(A("WALK_ANIM_SITE_1"), shield_anim);
    set(A("BUFF_REPLACE_SITE"), buff_replace);
    set(A("TILT_INIT_SITE"), tilt_init);
    set(A("MISSING_STRING_SITE"), missing_string);
    set(A("MISSING_FLOAT_SITE"), missing_float);



    T.no_handler_sites[A("NO_HANDLER_SITES_0")] = {0x321d03e0, at("goto L_@NO_HANDLER_CATCH@;"),
                                                   "mov w0, #8 (bad_function_call: to the catch block)"};
    T.no_handler_sites[A("NO_HANDLER_SITES_1")] = {0x940e8868, "", "bl __cxa_begin_catch (skipped)"};
    T.no_handler_sites[A("NO_HANDLER_SITES_2")] = {0x940e887b, "", "bl __cxa_end_catch (skipped)"};
    for (auto &e : T.no_handler_sites) set(e.first, no_handler);

    set(A("PROMPT_POS_SITE"), prompt_pos);
    set(A("FONT_BOX_GLYPH_SITE"), font_box_glyph);
    for (const char *n : {"FONT_FALLBACK_SITES_0", "FONT_FALLBACK_SITES_1", "FONT_FALLBACK_SITES_2", "FONT_FALLBACK_SITES_3"})
        set(A(n), font_fallback);
    for (const char *n : {"DELEGATE_FORWARDER_SITES_0", "DELEGATE_FORWARDER_SITES_1"}) set(A(n), delegate_forwarder);

    // ---- hook sites: the instruction stays as it is; just before it the runtime's rt_hook (runtime/hle/hooks.c) is
    // asked what to do - 0 run it, 1 the hook did it all, 2.. jump to the site's first.. target (each a label in its
    // function). The game code they generate never changes when a hook's behaviour does: only a new site needs the
    // game code rebuilt. New game-code changes go here, as a hook, unless they can't.
    // Power-up music (powerup_music.c), issues #19, #41, #183, #314, #349, #681: see the address tables' names and the
    // runtime files for each.
    auto &H = ps.hooks;
    auto hook = [&](const char *site, uint32_t word, std::vector<const char *> targets) {
        Hook h;
        h.word = word;
        for (const char *t : targets) h.targets.push_back(A(t));
        H[A(site)] = h;
    };
    hook("BUFF_MUSIC_PICKUP_SITE", 0x97ccd77b, {});            // bl main+0xaf560: Guardian Acorn / Piece of Power music
    hook("BUFF_MUSIC_FLAGS_SITES_0", 0xb95b8d15, {});          // ldr w21, [x8, #0x1b8c]: music update, power-up sound flag
    hook("BUFF_MUSIC_FLAGS_SITES_1", 0xb95b8d14, {});          // ldr w20: zone music
    hook("BUFF_MUSIC_FLAGS_SITES_2", 0xb95b8d15, {});          // event music
    hook("BUFF_MUSIC_FLAGS_SITES_3", 0xb95b8d15, {});          // power-up presentation
    hook("EMPTY_HANDLER_SITE", 0x9400005e, {"EMPTY_HANDLER_CATCH"});  // an empty event handler, as its catch (#41)
    hook("CHOICE_COUNT_SITE", 0xb91cc668, {});                 // a question's answer count (#183)
    hook("FRAME_JOB_KICKOFF_SITES_0", 0x910782d3, {});         // frame job kick-off, previous finish first (#19)
    hook("FRAME_JOB_KICKOFF_SITES_1", 0x910782d3, {});
    hook("FRAME_JOB_KICKOFF_SITES_2", 0x910782d3, {});
    hook("FRAME_JOB_KICKOFF_SITES_3", 0x910782b3, {});
    hook("FRAME_JOB_FINISH_SITE", 0xf9400008, {});             // frame job finish (debug delay)
    hook("OWNER_CALL_NOTIFY_SITE", 0x9100e2a0, {});            // owner-thread call done, the caller is waiting first
    hook("SHAPE_SIM_SITES_0", 0xaa0003f4, {"SHAPE_SIM_SKIP_0"});  // PhysX shape change without its sim object (#314)
    hook("SHAPE_SIM_SITES_1", 0xaa0003e1, {"SHAPE_SIM_SKIP_1"});
    hook("EFFECT_BONE_SITE", 0xf9402ac0, {"EFFECT_BONE_SKIP"});  // a bone-following effect without its bone (#349)
    hook("TEXT_RATE_SITE", 0xb9028408, {});                    // a message pane's text step (#681)
    hook("TEXT_SNAP_SITE", 0x540002cb, {});                    // message text short of the page's end (#681)
    return ps;
}

void build_other_patches(const std::string &game, const AddressTable *t, Module &m, PatchSet &ps,
                         std::map<uint64_t, uint32_t> &optnone) {
    if (m.name != "main" || !t) return;
    auto &P = ps.patches;
    auto T = [&](const char *name) {
        auto it = t->find(name);
        if (it == t->end()) throw GenError(std::string("KeyError: ") + name);
        return it->second;
    };
    if (game == "eow") {
        // (optnone) a 25 500-line function behind a data table (1.0.2: main+0x3d85e90), never entered in 4 200 frames
        // of start, file load and play (LARECOMP_FNPROF); at -O1 it took 30+ minutes to compile
        optnone[T("OPTNONE_TABLE_FN")] = 0xa9ba7bfd;  // stp x29, x30, [sp, #-0x60]!
        // a 27 300-line function called once while the game starts (LARECOMP_FNPROF: once in boot to the overworld);
        // at -O2 it took 8 minutes on a Mac and 30+ on a Snapdragon 865 (Android builds it on the phone)
        optnone[T("OPTNONE_SLOW_FN")] = 0xa9ba7bfd;  // stp x29, x30, [sp, #-0x60]!
        for (auto &e : optnone) {
            uint32_t got = m.image_word(e.first);
            if (got != e.second)
                throw GenError("AssertionError: OPTNONE " + ax(e.first) + ": " + ax(got) + ", not " + ax(e.second));
        }
        P[T("REQUEST_SITE")] = eow_request;
        P[T("TILT_SITE")] = eow_tilt;
        P[T("DYNRES_SITE")] = eow_dynres;
        P[T("FPS_WINDOW_SITE")] = eow_fps_window;
        if (T("SLOW60_SITE")) P[T("SLOW60_SITE")] = eow_slow60;
        eow_fps_limit_30 = T("FPS_LIMIT_30_SITE");
        P[eow_fps_limit_30] = eow_fps_limit;
        P[T("FPS_LIMIT_60_SITE")] = eow_fps_limit;
        // (in name order, as gen.py: the index is the site's number for the runtime)
        eow_aspect_sites.clear(), eow_spot_sites.clear();
        for (auto &e : *t) {
            if (!e.first.compare(0, 12, "ASPECT_SITE_")) eow_aspect_sites.push_back(e.second);
            if (!e.first.compare(0, 10, "SPOT_SITE_")) eow_spot_sites.push_back(e.second);
        }
        for (uint64_t a : eow_aspect_sites) P[a] = eow_aspect;
        for (uint64_t a : eow_spot_sites) P[a] = eow_spot;
    }
}

int add_trace_store_patches(Module &m, PatchSet &ps, const char *list) {
    trace_offs.clear();
    for (const char *p = list; *p;) {
        char *e;
        trace_offs.insert(strtoll(p, &e, 0));
        if (e == p) throw GenError(std::string("GEN_TRACE_STORE_OFF: bad offset list ") + list);
        p = *e == ',' ? e + 1 : e;
    }
    int n = 0;
    for (size_t i = 0; i < m.words.size(); i++) {
        uint32_t w = m.words[i];
        uint32_t rn = (w >> 5) & 31;
        if ((w & 0xFFC00000) == 0xF9000000 && trace_offs.count((int64_t)((w >> 10) & 0xFFF) * 8) && rn != (w & 31)) {
            ps.patches[m.lo + i * 4] = trace_store;
            n++;
        } else if ((w & 0xFFC00000) == 0xA9000000) {
            int64_t imm = (w >> 15) & 0x7F, off = (imm & 64 ? imm - 128 : imm) * 8;
            if ((trace_offs.count(off) || trace_offs.count(off + 8)) && rn != (w & 31) && rn != ((w >> 10) & 31)) {
                ps.patches[m.lo + i * 4] = trace_store;
                n++;
            }
        }
    }
    return n;
}

std::string trace_store_offsets_text() {
    std::vector<std::string> v;
    for (int64_t o : trace_offs) v.push_back(o < 0 ? "-0x" + hexs((uint64_t)-o) : "0x" + hexs((uint64_t)o));
    std::sort(v.begin(), v.end());
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", '" : "'") + v[i] + "'";
    return s + "]";
}

}  // namespace lasgen
