// Link's Awakening 1.0.1 / 1.1.0: runtime-only addresses and code limits.
// Changes here rebuild the host runtime without invalidating generated game code.
// An X-macro list (no include guard): included with GA_ADDR(name, value) defined (runtime/game_addresses.h).

// Runtime globals and functions
GA_ADDR(PLAYER_STATUS_PTR,                0x7001cc1368)
// (the level-jump request processor: runtime/hle/warp.c calls it)
GA_ADDR(LEVEL_REQUEST_SITE,               0x700081ba80)
GA_ADDR(GAME_MGR,                         0x7001c97188)
GA_ADDR(PLAYER_COLLISION_VT,              0x7001c501f0)
GA_ADDR(LIVE_SAVE,                        0x7001cbb220)
GA_ADDR(HUD_SETUP,                        0x7000ea85c0)
GA_ADDR(LINK_HIT_FILTER,                 0x7000d4ce50)
GA_ADDR(LINK_DAMAGE_CB,                  0x7000d4c690)
GA_ADDR(BUZZBLOB_ATTACK,                 0x70016efe94)
GA_ADDR(LINK_DEFENCE,                    0x7001701178)
GA_ADDR(ADD_ITEM,                         0x70007e18b0)
GA_ADDR(BUFF_MUSIC_START,                 0x70000af560)
GA_ADDR(BUFF_MUSIC_STOP,                  0x70000afa60)
GA_ADDR(SAVE_SCAN_BEGIN,                  0x7001c00000)
GA_ADDR(SAVE_SCAN_END,                    0x7001d00000)
GA_ADDR(MAIN_IMAGE_SCAN_END,              0x7002000000)
GA_ADDR(SDK_SYSTEM_OBJECT,                0x7004cf9e38)
GA_ADDR(DELEGATE_JOB_VTABLE_LIVE,         0x7001c51308)
GA_ADDR(DELEGATE_JOB_VTABLE_BASE,         0x7001c51358)
GA_ADDR(HAS_ITEM,                         0x70008d1a30)
