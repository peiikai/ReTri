// Other games on the same engine (experimental), by the main program's build ID (exefs/main, 0x40..0x53). They have
// no address tables: none of LA's patches, hooks or randomizer code applies to them. Their main program can be larger
// than LA's, so each one names where its sdk module is loaded (main stays at MAIN_BASE).
// An X-macro list (no include guard): GA_GAME(key, label, main build ID, sdk base).
// tools/game_addresses.py reads this same file.

GA_GAME(eow, "Echoes of Wisdom", "D1ED438C8BDAF4368B30DAEC53ED8E9BACD31A34", 0x7008000000)
