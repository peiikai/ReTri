// Echoes of Wisdom's live player status (runtime/hle/cheats_other.c finds it; the layout is described there):
// cheats_other_status() is the address of its rupee count, or 0 (no save in play, or not a status we know). The
// other fields are at these offsets from it, in every supported build.
#pragma once
#include <stdint.h>

enum {
    PLAYER_OTHER_MAX_HP = -0x90,     // s32, quarter hearts
    PLAYER_OTHER_HP = -0x8c,         // s32, quarter hearts (0 while Zelda is dying or respawning)
    PLAYER_OTHER_RUPEES = 0,         // s32, 0..9999
    PLAYER_OTHER_ENERGY = 0x18,      // float, Swordfighter Form energy
    PLAYER_OTHER_ENERGY_LVL = 0x1c,  // u32: 0x2000 / 0x4000 / 0x8000 the gauge upgrades (max 20 / 30 / 40, else 15)
};
enum { PLAYER_OTHER_RUPEE_MAX = 9999 };

#ifdef __cplusplus
extern "C" {
#endif
uint64_t cheats_other_status(void);
#ifdef __cplusplus
}
#endif
