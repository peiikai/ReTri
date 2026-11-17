// Quality-of-life options re-implemented from community GameBanana mods (credits in README.md; none of their files
// are used): 360-degree movement and blur removal ("360 Controls (And More!)"). Button prompts are
// runtime/nvn/texpatch.c, the Hero Mode icon is hidden there too.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"


// 360-degree movement: main+0xded9f0 (tools/patches.py SNAP_DIR_SITE) turns the stick vector (s1 = x, s2 = y) into
// one of 8 directions, returned in w0 as an angle (full turn = 2^32, 0 = down, 2^30 = right). With the setting on
// it returns the stick's own angle (returns 1: the original function is skipped).
int rt_snap_dir(Cpu *c) {
    static int env = -1, logn;  // LARECOMP_MOVE_360=0/1: test override; LARECOMP_LOG_SNAP: log some calls
    if (env < 0) { const char *e = getenv("LARECOMP_MOVE_360"); env = e ? atoi(e) != 0 : 2; logn = ENV("LARECOMP_LOG_SNAP") ? 0 : -1; }
    float x, y;
    memcpy(&x, &c->v[1], 4);
    memcpy(&y, &c->v[2], 4);
    bool on = env == 2 ? g_settings.move_360 : env;
    static bool inner;
    if (inner) return 0;
    double turns = atan2(y, x) / (2 * M_PI) + 0.25;  // (the mod: floor(degrees) + 90)
    uint32_t a = (uint32_t)(int64_t)llround(turns * 4294967296.0);
    if (logn >= 0 && logn++ % 30 == 0) {  // (with the game's own 8-way answer for comparison)
        static Cpu tmp;
        tmp = *c;
        inner = true;
        dispatch_lookup(GA(SNAP_DIR_SITE))(&tmp);
        inner = false;
        LOG("[qol] snap (%.3f, %.3f): 8-way %#x, 360 %#x (%s)\n", x, y, (uint32_t)tmp.x[0], a, on ? "360" : "8-way");
    }
    if (!on) return 0;
    c->x[0] = a;
    return 1;
}

// Walk speed (Gameplay tab, setting walk_speed): tools/patches.py WALK_SPEED_SITES hand over the multiplier Link's walk
// / shield-walk state applies to its ramped speed (1, or 1.15 with a Piece of Power; s0) with the player actor.
float rt_walk_speed(uint64_t player, float m, int site) {
    static float env = -1;  // LARECOMP_WALK_SPEED=<mult>: test override; LARECOMP_LOG_WALK: log some calls
    static int logn = -1;
    static long mask = -1;  // LARECOMP_WALK_SITES=<bitmask>: (test) which sites scale (bits 30 / 31: animations)
    if (env < 0) {
        const char *e = getenv("LARECOMP_WALK_SPEED");
        env = e ? (float)atof(e) : 0;
        logn = ENV("LARECOMP_LOG_WALK") ? 0 : -1;
        e = getenv("LARECOMP_WALK_SITES");
        mask = e ? strtol(e, NULL, 0) : -1;
    }
    float k = env > 0 ? env : g_settings.walk_speed;
    if (!(k > 0.5f && k <= 4)) k = 1;
    if (k != 1 && !g_settings.walk_speed_dungeons) {  // (off indoors: every level but the overworld, "Field")
        uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
        if (ps && strcmp((const char *)(uintptr_t)ps, "Field")) k = 1;
    }
    if (!(mask >> (site >= 100 ? 30 + site - 100 : site) & 1)) k = 1;
    if (logn >= 0) {
        static int cnt[102];
        if (cnt[site]++ % 30 == 0) LOG("[qol] walk site %d #%d: %.2f x %.2f\n", site, cnt[site], m, k);
    }
    return m * k;
}

// No Guardian Acorns / Pieces of Power (Gameplay tab, setting no_buff_drops). Enemy kills count up two bytes of
// PlayerStatus (+0xa4, +0xa5, saturating at 255; shared death code main+0x7c6950). The drop roll main+0x89b960 takes the
// dropper's ItemDrop row; a row with mAllowBuffReplace (+0x38: NormalLow / NormalMid / RupeeMid...) first checks the
// kill counters in PlayerStatus (+0xa5 >= 14: "DefenceUp" = Guardian Acorn; +0xa4 past a threshold: "PowerUp" =
// Piece of Power; the counter is then wrapped) and drops that item symbol directly - the ItemDrop rows GuardianAcorn /
// PowerPiece aren't involved. tools/patches.py BUFF_REPLACE_SITE reads the flag as false here: the counters are never
// consumed (they stay at 255) and the row's own lot table is rolled as usual. Anything else that would make one
// (ItemPowerPiece 0xb2 / ItemGuardianAcorn 0xb3) is held back at the actor factory (rt_actor_create_fn).
static bool no_buff(void) {
    const char *e = getenv("LARECOMP_NO_BUFF");  // test: 1 / 0 forces it
    if (e) return atoi(e) != 0;
    return g_settings.no_buff_drops;
}

int rt_buff_replace(int allow) {
    // LARECOMP_BUFF_PRIME=1 (test): every roll finds the Guardian Acorn counter full; =2: and every row allows buffs
    // (grass then drops Acorns unless the option is on)
    static int test = -1;
    if (test < 0) test = ENV("LARECOMP_BUFF_PRIME") ? atoi(ENV("LARECOMP_BUFF_PRIME")) : 0;
    if (test) {
        uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));  // PlayerStatus*
        if (ps) ST8(ps + 0xa5, 14);
        if (test == 2) allow = 1;
        { extern uint64_t renderer_present_count(void); LOG("[qol] f%llu drop roll: buff replace %d -> %d\n", (unsigned long long)renderer_present_count(), allow, allow && !no_buff()); }
    }
    return allow && !no_buff();
}

// The actor factory's create function for an actor type (tools/patches.py BOSS_SPAWN_SITE); 0: the factory's own path
// for a type without a class (the caller gets an empty handle)
uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type) {
    if ((type != 0xb2 && type != 0xb3) || !no_buff()) return fn;
    LOG("[qol] %s held back (no-buff-drops)\n", type == 0xb2 ? "Piece of Power" : "Guardian Acorn");
    return 0;
}

// Blur removal: the post-process's tilt-shift object (post-process manager +0x620, set up once at boot by
// main+0x1071220) copies the name at main+0x16cbd73 ("PFXTiltShiftParam") into a std::string (+0x58) and makes its
// uniform buffer (+0x50) with a pointer to that text (buffer +0x60). The buffer is bound to postprocess.bfsha's block
// of that name each frame; under any other name the shader gets no parameters and draws no blur (LAS-Randomizer
// 'blur-removal', patch by theboy181, renames the string in the module - which only took effect at the next start,
// the copy being made once). tools/patches.py TILT_INIT_SITE hands us the object; its copy's first letter is
// switched P <-> X as the setting changes, live. EoW has the same object (renderer +0x600, hooked in tools/gen.py),
// but its buffer keeps the name pointer at +0x68.
static uint64_t g_tilt_obj;
static int g_tilt_applied = -1;  // blur removal as last applied (-1: not yet)
RT_ON_RESET(qol_reset) { g_tilt_obj = 0; g_tilt_applied = -1; }  // (reboot: a new post-process)

void rt_tilt_obj(uint64_t obj) { g_tilt_obj = obj; g_tilt_applied = -1; }

static bool apply_tilt(int removed) {  // false: not set up yet (or not the name we know)
    uint64_t buf = g_tilt_obj ? LD64(g_tilt_obj + 0x50) : 0;
    char *name = buf ? (char *)(uintptr_t)LD64(buf + (game_other() ? 0x68 : 0x60)) : NULL;
    if (!name || memcmp(name + 1, "FXTiltShiftParam", 17) || (name[0] != 'P' && name[0] != 'X')) return false;
    name[0] = removed ? 'X' : 'P';
    return true;
}

// LARECOMP_SETTING_AT="frame:key=value,..." (test): flip an in-game menu toggle at that presented frame, as the menu
// would (move_360, blur_removal, frame_interp: 0 / 1; dpad_mode: 0 nothing / 1 items / 2 move; Cosmetics colour rows: a palette name or #RRGGBB - tunic, hearts,
// hair, shield, skin, leaves, grass, sand; enemy_colours: 0 / 1 / 2; screen_filter: 0-3; text_speed: 0-2).
static void setting_at_tick(void) {
    static const char *spec;
    static int init;
    static uint64_t last;
    if (!init) { init = 1; spec = getenv("LARECOMP_SETTING_AT"); }
    if (!spec) return;
    extern uint64_t renderer_present_count(void);
    uint64_t now = renderer_present_count();
    if (now == last) return;
    char buf[4096];  // (a long test script: many rows switched in one run)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned long f;
        char key[32], val[32];
        if (sscanf(tok, "%lu:%31[a-z_0-9]=%31s", &f, key, val) != 3 || f <= last || f > now) continue;
        int v = atoi(val);
        if (!strcmp(key, "screen_filter")) { g_settings.screen_filter = v; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        if (!strcmp(key, "enemy_colours")) { g_settings.enemy_colours = v; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        if (!strcmp(key, "text_speed") && v >= 0 && v < N_TEXT_SPEEDS) { g_settings.text_speed = v; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        if (!strcmp(key, "dpad_mode")) { g_settings.dpad_mode = v; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        if (!strcmp(key, "shadows")) { g_settings.shadow_quality = v; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        bool *b = !strcmp(key, "move_360") ? &g_settings.move_360 : !strcmp(key, "blur_removal") ? &g_settings.blur_removal :
                  !strcmp(key, "frame_interp") ? &g_settings.frame_interp :
                  !strcmp(key, "powerup_music") ? &g_settings.powerup_music : NULL;
        if (b) { *b = v != 0; LOG("[qol] test: f%llu %s = %d\n", (unsigned long long)now, key, v); }
        CosColour *c = !strcmp(key, "tunic") ? &g_settings.tunic : !strcmp(key, "hearts") ? &g_settings.hearts :
                       !strcmp(key, "hair") ? &g_settings.hair : !strcmp(key, "shield") ? &g_settings.shield :
                       !strcmp(key, "skin") ? &g_settings.skin : !strcmp(key, "leaves") ? &g_settings.leaves :
                       !strcmp(key, "grass") ? &g_settings.grass : !strcmp(key, "sand") ? &g_settings.sand : NULL;
        if (c) { col_parse(c, val); LOG("[qol] test: f%llu %s = %s\n", (unsigned long long)now, key, val); }
    }
    last = now;
}

// Game thread, once per frame (runtime/hle/input.c).
void qol_tick(void) {
    setting_at_tick();
    static int env = -1;  // LARECOMP_BLUR_REMOVAL=0/1: test override
    if (env < 0) { const char *e = getenv("LARECOMP_BLUR_REMOVAL"); env = e ? atoi(e) != 0 : 2; }
    int want = env == 2 ? g_settings.blur_removal : env;
    if (want != g_tilt_applied && apply_tilt(want)) {
        LOG("[qol] blur removal %s\n", want ? "on" : "off");
        g_tilt_applied = want;
    }
}
