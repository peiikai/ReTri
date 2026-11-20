// Other games on the same engine, by the main program's build ID (exefs/main, 0x40..0x53). None of LA's patches, hooks
// or randomizer code applies to them. Their main program can be larger than LA's, so each one names where its sdk
// module is loaded (main stays at MAIN_BASE). Each build of a game has its own row and its own address table,
// <key>_<table>.h (the game code ReTri patches or calls, OTHER_ADDR lines, the same names in every table).
// An X-macro list (no include guard): GA_GAME(key, table, label, release, main build ID, sdk base).
// tools/game_addresses.py reads this same file.

GA_GAME(eow, v1_0_0, "Echoes of Wisdom", "1.0.0", "A581406BC985865113D18CB19F11D5448CEEA783", 0x7008000000)
GA_GAME(eow, v1_0_1, "Echoes of Wisdom", "1.0.1", "09A15C8259828A41B91B036B12F0687973A4627A", 0x7008000000)
GA_GAME(eow, v1_0_2, "Echoes of Wisdom", "1.0.2", "D1ED438C8BDAF4368B30DAEC53ED8E9BACD31A34", 0x7008000000)
