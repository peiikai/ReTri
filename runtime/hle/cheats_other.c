// Echoes of Wisdom's cheats (the Cheats page: runtime/ui/cheats.mm): the "Always on" ones are applied on the game
// thread once a frame (input.c, the controller read), the one-time buttons are queued for it.
//
// The live player status is an object the game reaches as [[OTHER(PLAYER_STATUS)] + 8]: its event commands
// (CheckRupee, AddRupee, AddPasteEnergy) and the rupee pickups all read it so, and skip it while its +0x17 bit 6 is
// set. The save's copies of the same data (main.bin's "Pl" block) are only written from it when the game saves.
// Its fields, from the rupee count (R):
//   R - 0x90  max health, R - 0x8c  health (s32, quarter hearts; 0 while Zelda is dying or respawning)
//   R         rupees (s32; the game keeps them in 0..9999)
//   R + 0x18  Swordfighter Form energy (float; AddPasteEnergy refills it to 15, 20, 30 or 40 by the gauge upgrades,
//             bits 0x2000 / 0x4000 / 0x8000 of the word at R + 0x1c)
// R itself is +0x1b98 in 1.0.x and +0x1858 in 1.1.0 (the object grew; every field moved with it): it is read from the
// game's own AddRupee code (OTHER(RUPEE_LOAD_SITE), its `ldr w9, [x8, #R]`), so a build whose instruction isn't that
// load gets no cheats instead of wrong writes.
// Nothing is kept from a cheat: with one off the game runs on as it is. Rupees set to 9999 stay, as they would after
// a big find.
#include "../game_addresses.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
#include "../settings.h"
#include "player_other.h"

extern int warp_other_in_play(void);  // runtime/hle/warp_other.c

enum { MAX_HP = PLAYER_OTHER_MAX_HP, HP = PLAYER_OTHER_HP, RUPEES = PLAYER_OTHER_RUPEES, ENERGY = PLAYER_OTHER_ENERGY,
       ENERGY_LVL = PLAYER_OTHER_ENERGY_LVL, RUPEE_MAX = PLAYER_OTHER_RUPEE_MAX };

static int32_t rupee_field(void) {
    static int32_t off = -2;
    if (off == -2) {
        off = -1;
        uint64_t site = g_other_ga ? OTHER(RUPEE_LOAD_SITE) : 0;
        uint32_t w = site ? *(const uint32_t *)(uintptr_t)site : 0;
        if (!w) return off = -2, -1;  // (the game isn't loaded yet: again later)
        if ((w & 0xffc00000u) == 0xb9400000u) off = (int32_t)((w >> 10) & 0xfff) * 4;  // ldr w<t>, [x<n>, #imm]
        if (off < 0x1000 || off >= 0x2000) off = -1;
        LOG("[cheats] EoW: player status rupees at +%#x%s\n", off > 0 ? off : 0, off > 0 ? "" : " (not found: no cheats)");
    }
    return off;
}

static bool in_heap(uint64_t p, uint64_t n) { return p >= HEAP_BASE && p + n <= HEAP_BASE + kernel_heap_size(); }

// The rupee count's address (R above), or 0: no save in play, or the status isn't one we know
static uint64_t status_r(void) {
    int32_t off = rupee_field();
    if (off < 0 || !warp_other_in_play()) return 0;
    uint64_t a = *(const uint64_t *)(uintptr_t)OTHER(PLAYER_STATUS);
    if (!in_heap(a, 16)) return 0;
    uint64_t x = *(const uint64_t *)(uintptr_t)(a + 8);
    if (!in_heap(x, (uint64_t)off + 0x20) || (*(const uint8_t *)(uintptr_t)(x + 0x17) & 0x40)) return 0;
    uint64_t r = x + (uint64_t)off;
    int32_t max = *(const int32_t *)(uintptr_t)(r + MAX_HP), hp = *(const int32_t *)(uintptr_t)(r + HP),
            rupees = *(const int32_t *)(uintptr_t)r;
    if (max < 4 || max > 4 * 40 || hp < 0 || hp > max || rupees < 0 || rupees > RUPEE_MAX) return 0;  // (not it)
    return r;
}
#define I32(r, f) (*(volatile int32_t *)(uintptr_t)((r) + (f)))
#define F32(r, f) (*(volatile float *)(uintptr_t)((r) + (f)))

static float energy_max(uint64_t r) {
    uint32_t lv = (uint32_t)I32(r, ENERGY_LVL);  // (the game's own table, the same in every build)
    return lv & 0x8000 ? 40.0f : lv & 0x4000 ? 30.0f : lv & 0x2000 ? 20.0f : 15.0f;
}

static _Atomic int g_once;  // the one-time buttons (render thread) for the game thread
enum { ONCE_HEARTS = 1, ONCE_ENERGY = 2, ONCE_RUPEES = 4 };
void cheats_other_once(int what) { atomic_fetch_or(&g_once, what); }
bool cheats_other_ready(void) { return status_r() != 0; }
uint64_t cheats_other_status(void) { return status_r(); }  // (player_other.h: the Save Editor's too)

// Game thread, once a frame
void cheats_other_tick(void) {
    static int env = -1;  // LARECOMP_CHEATS=hearts,energy,rupees: force on for tests (not saved)
    if (env < 0) {
        const char *e = getenv("LARECOMP_CHEATS");
        env = e != NULL;
        if (e) {
            g_settings.cheat_hearts |= strstr(e, "hearts") != NULL;
            g_settings.cheat_energy |= strstr(e, "energy") != NULL;
            g_settings.cheat_rupees |= strstr(e, "rupees") != NULL;
        }
    }
    uint64_t r = status_r();
    if (!r) return;
    static long drain_at = -2;  // (test) LARECOMP_CHEATS_DRAIN=<frame>[,<frame>...]: a quarter heart and no energy left
    static const char *drain;
    if (drain_at == -2) drain = getenv("LARECOMP_CHEATS_DRAIN"), drain_at = drain ? atol(drain) : -1;
    extern uint64_t renderer_present_count(void);
    if (drain_at >= 0 && renderer_present_count() >= (uint64_t)drain_at) {
        I32(r, HP) = 1, F32(r, ENERGY) = 0;
        LOG("[cheats] test: drained at frame %llu\n", (unsigned long long)renderer_present_count());
        drain = strchr(drain, ',');
        drain_at = drain ? atol(++drain) : -1;
    }
    int once = atomic_exchange(&g_once, 0);
    int32_t max = I32(r, MAX_HP), hp = I32(r, HP);
    if ((g_settings.cheat_hearts || (once & ONCE_HEARTS)) && hp > 0 && hp < max) I32(r, HP) = max;  // (0: dying)
    float em = energy_max(r), en = F32(r, ENERGY);
    if ((g_settings.cheat_energy || (once & ONCE_ENERGY)) && en >= 0 && en < em) F32(r, ENERGY) = em;
    if ((g_settings.cheat_rupees || (once & ONCE_RUPEES)) && I32(r, RUPEES) != RUPEE_MAX) I32(r, RUPEES) = RUPEE_MAX;
}
