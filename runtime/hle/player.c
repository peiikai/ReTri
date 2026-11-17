// Link's position: the "PlayerCollision" component (vtable GA(PLAYER_COLLISION_VT), name inline at +0x18), vec3 at
// +0x80 (a second copy at +0xb0); writing it moves Link, every other copy follows (found 2026-10-08 with
// LARECOMP_ADDF_MATCH bisection). Read from Link's actor after each level load. Used by warp.c, hookshot.c and the
// weather (runtime/nvn/weather.c).
#include "../game_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

// GA(PLAYER_STATUS_PTR): PlayerStatus*: current level name at +0

static uint64_t g_player;          // player_find: the PlayerCollision component found last
static uint64_t g_player_scan;     // frame of the last level change / heap scan
static char g_player_level[64];    // level it was found in
static int g_player_rescan;        // (a warp within the same level: the old player can't be told apart either)
static int g_player_misses;        // scans in this level that found nothing (player_find)
static uint64_t g_player_actor;    // Link's actor those misses were with

RT_ON_RESET(player_reset) {
    g_player = g_player_scan = 0;
    g_player_level[0] = 0;
    g_player_rescan = 0;
    g_player_misses = 0;
    g_player_actor = 0;
}

static int is_player_collision(uint64_t p) {
    return LD64(p) == GA(PLAYER_COLLISION_VT) && LD64(p + 8) == p &&
           !memcmp((const void *)(uintptr_t)(p + 0x18), "PlayerCollision", 16);
}

// Link's actor ([game manager + 0x4b8]) holds the component at +0x80 (and again at +0x2d8): read there, no search
// (found 2026-10-22 with a pointer-path probe from the game manager; the same in the overworld and every dungeon
// checked). As before, nothing for 30 frames after each level change (callers expect the level to have settled).
// Fallback when that slot doesn't hold it: the old heap scan (the component is 16-byte aligned; the last one found is
// the live one), which reads the whole guest heap (~3 GB: 70-200 ms on the game thread, #257): never outside play
// (intro movie, title, file select: PlayerStatus still names a level there, but there is no Link), further apart
// after each miss in a level, and no more after 3 misses with the same Link (an in-play level whose Link has no
// PlayerCollision, e.g. around the ending: nothing to find until the level or Link changes).
uint64_t player_find(uint64_t frame) {
    extern int game_in_play(void);  // hle/warp.c
    if (!game_in_play()) {  // (entering play counts as a new level: look again once settled)
        g_player = 0;
        g_player_level[0] = 0;
        return 0;
    }
    uint64_t gm = LD64(GA(GAME_MGR)), actor = gm ? LD64(gm + 0x4b8) : 0;  // Link's actor (game_in_play: non-zero)
    if (actor != g_player_actor) g_player_actor = actor, g_player_misses = 0;
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    const char *lv = ps ? (const char *)(uintptr_t)ps : "";
    if (strncmp(g_player_level, lv, sizeof g_player_level - 1) || g_player_rescan) {  // new level: look once settled
        g_player_rescan = 0;
        strlcpy(g_player_level, lv, sizeof g_player_level);
        g_player = 0;
        g_player_scan = frame;
        g_player_misses = 0;
        return 0;
    }
    if (!g_player && frame - g_player_scan < 30) return 0;
    uint64_t direct = actor ? LD64(actor + 0x80) : 0;
    if (direct && is_player_collision(direct)) {
        if (direct != g_player && ENV("LARECOMP_PLAYER_LOG"))
            LOG("[player] %s: PlayerCollision at %llx (Link's actor)\n", lv, (unsigned long long)direct);
        g_player = direct;
        g_player_misses = 0;
        return g_player;
    }
    if (g_player && is_player_collision(g_player)) return g_player;
    g_player = 0;
    if (g_player_misses >= 3) return 0;
    if (frame - g_player_scan < 30 * (uint64_t)(1 + g_player_misses)) return 0;  // 30, 60, 90 frames
    g_player_scan = frame;
    uint64_t t0 = plat_now_ns();
    extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
    uint64_t ra[64], rs[64];
    int nr = kernel_data_regions(ra, rs, 64), n = 0;
    for (int r = 0; r < nr; r++)
        for (uint64_t k = 0; k + 0x100 <= rs[r]; k += 16)
            if (LD64(ra[r] + k) == GA(PLAYER_COLLISION_VT) && is_player_collision(ra[r] + k)) {
                const float *p = (const float *)(uintptr_t)(ra[r] + k + 0x80);
                if (ENV("LARECOMP_PLAYER_LOG"))
                    LOG("[player] candidate %llx (%.2f, %.2f, %.2f) parent %llx\n", (unsigned long long)(ra[r] + k),
                        p[0], p[1], p[2], (unsigned long long)LD64(ra[r] + k + 0x10));
                g_player = ra[r] + k;
                n++;
            }
    g_player_misses = g_player ? 0 : g_player_misses + 1;
    LOG("[player] %s: PlayerCollision %s %llx by heap scan (Link's actor +0x80: %llx; %d candidates, %.0f ms)\n", lv,
        g_player ? "at" : "not found", (unsigned long long)g_player, (unsigned long long)direct, n, (plat_now_ns() - t0) / 1e6);
    return g_player;
}

void player_rescan(void) { g_player_rescan = 1; }

// Test hooks moving Link and holding the controls (runtime/hle/input.c reads these)
int32_t g_test_lx, g_test_ly;  // stick held by a test
uint64_t g_test_stick_until;
uint64_t g_test_buttons, g_test_buttons_until;  // buttons pressed by a test (bit 0 A, 1 B, 2 X, 3 Y)

// LARECOMP_PLACE=<frame>:<x>:<z>[:<script>] (test): put Link at x, z (any level) once the level has been loaded 60
// frames, then run <script> relative to that moment: steps "<delay>:<L|R|U|D|A|B|X|Y>:<frames>" separated by '/', e.g.
// "10:L:8/40:A:6/90:A:6/130:L:30" = face west, lift, throw, walk west (pot tests). Several placements: entries
// separated by ';' in frame order, each placed at its frame
// LARECOMP_PLACE_Y=<y>: also the height (a spot higher than where Link stands would put him inside the ground: he
// falls)
static void place_tick(uint64_t frame) {
    static long at = -2, done, placed_at = -1;
    static char script[4096];
    static float px, pz;
    static uint64_t since;
    static char level[64];
    static const char *next_entry;  // the next ';'-separated placement (read once the current one has been placed)
    if (at == -2) {
        at = -1;
        next_entry = getenv("LARECOMP_PLACE");
    }
    // the next entry: the first one at once, each later one at its frame once the one before has been placed (that
    // one's script runs until then)
    bool first = at == -1 && !done;
    if (next_entry && (first || (done && (long)frame >= strtol(next_entry, NULL, 10)))) {
        const char *semi = strchr(next_entry, ';');
        char one[4096];
        size_t n = semi ? (size_t)(semi - next_entry) : strlen(next_entry);
        if (n >= sizeof one) n = sizeof one - 1;
        memcpy(one, next_entry, n);
        one[n] = 0;
        next_entry = semi ? semi + 1 : NULL;
        script[0] = 0;
        if (sscanf(one, "%ld:%f:%f:%4095s", &at, &px, &pz, script) < 3) at = -1, next_entry = NULL;
        done = 0;
        placed_at = -1;
    }
    if (placed_at >= 0 && script[0]) {  // action script
        long t = (long)frame - placed_at;
        static char buf[4096];
        strlcpy(buf, script, sizeof buf);
        for (char *st = strtok(buf, "/"); st; st = strtok(NULL, "/")) {
            long d, n;
            char k;
            if (sscanf(st, "%ld:%c:%ld", &d, &k, &n) != 3 || t != d) continue;
            if (strchr("LRUD", k)) {
                g_test_lx = k == 'L' ? -32767 : k == 'R' ? 32767 : 0;
                g_test_ly = k == 'U' ? 32767 : k == 'D' ? -32767 : 0;
                g_test_stick_until = frame + n;
            } else {
                g_test_buttons = k == 'A' ? 1 : k == 'X' ? 4 : k == 'Y' ? 8 : 2;  // bit 0 A, 1 B, 2 X, 3 Y
                g_test_buttons_until = frame + n;
            }
            LOG("[place] f%llu script %c for %ld\n", (unsigned long long)frame, k, n);
        }
    }
    if (at < 0 || done || (long)frame < at) return;
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    const char *lv = ps ? (const char *)(uintptr_t)ps : "";
    if (strncmp(level, lv, sizeof level - 1)) { strlcpy(level, lv, sizeof level); since = frame; return; }
    if (frame - since < 60) return;
    uint64_t pc = player_find(frame);
    if (!pc) return;
    float *pos = (float *)(uintptr_t)(pc + 0x80), *pos2 = (float *)(uintptr_t)(pc + 0xb0);
    pos[0] = pos2[0] = px;
    pos[2] = pos2[2] = pz;
    if (ENV("LARECOMP_PLACE_Y")) pos[1] = pos2[1] = (float)atof(ENV("LARECOMP_PLACE_Y"));
    LOG("[place] f%llu Link -> (%.2f, %.2f) in %s\n", (unsigned long long)frame, px, pz, lv);
    done = 1;
    placed_at = (long)frame;
}

// Game thread, every frame (runtime/hle/warp.c): the test hooks above. LARECOMP_LOG_LINK=<every N frames>: Link's
// position (movement tests)
void player_tick(uint64_t frame) {
    place_tick(frame);
    static int every = -1;
    if (every < 0) { const char *e = getenv("LARECOMP_LOG_LINK"); every = e ? atoi(e) : 0; }
    uint64_t pc = every > 0 && frame % (uint64_t)every == 0 ? player_find(frame) : 0;
    if (pc) {
        float *p = (float *)(uintptr_t)(pc + 0x80);
        LOG("[link] f%llu (%.2f, %.2f, %.2f)\n", (unsigned long long)frame, p[0], p[1], p[2]);
    }
}
