// Guest addresses of the game's code and data, per game version (game_addresses/versions.h). GA(NAME) is the
// selected version's address: game_version_select (runtime/game_addresses.c) picks the table by the main program's
// build ID when the game boots, before any game code runs. Unknown build IDs are refused: there is no fallback table.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameAddrs {
#define GA_ADDR(name, value) uint64_t name;
#include "game_addresses/patches_v1_0_0.h"
#include "game_addresses/runtime_v1_0_0.h"
#undef GA_ADDR
} GameAddrs;

// Another game's own addresses (game_addresses/games.h: one table per build, <key>_<table>.h, the same names in each).
typedef struct OtherAddrs {
#define OTHER_ADDR(name, value) uint64_t name;
#include "game_addresses/eow_v1_0_2.h"
#undef OTHER_ADDR
} OtherAddrs;
// The selected build's (NULL for LA and before game_version_select): OTHER(NAME) is its address.
extern const OtherAddrs *g_other_ga;
#define OTHER(name) (g_other_ga->name)

// The selected table (NULL until game_version_select succeeds).
extern const GameAddrs *g_ga;
#define GA(name) (g_ga->name)
// For tables built at compile time: GA_FIELD(NAME) there, GA_AT(field) to read the selected version's address.
#define GA_FIELD(name) offsetof(GameAddrs, name)
#define GA_AT(field) (*(const uint64_t *)((const char *)g_ga + (field)))

typedef struct GameVersion {
    const char *key;       // "v1_0_1": the tables' suffix
    const char *label;     // "1.0.1 / 1.1.0": every release with this main program
    const char *build_id;  // the main program's build ID, 40 upper-case hex digits
    const GameAddrs *addrs;
} GameVersion;

// The version whose main build ID is the first 20 bytes of id (exefs/main 0x40), or NULL.
const GameVersion *game_version_by_build_id(const uint8_t id[20]);
// The version (or another game's build) whose main build ID starts with hex (upper-case, 16 digits or more: a game
// code library's id), or NULL.
const GameVersion *game_version_by_build_prefix(const char *hex);
// What a main program file (exefs/main) is: GAME_MAIN_NSO with its build ID in hex (40 upper-case digits), or why it
// has none (hex ""). For the log when the version is refused: a build ID no version has, or no program at all (#696).
typedef enum { GAME_MAIN_NSO, GAME_MAIN_MISSING, GAME_MAIN_NOT_NSO } GameMainKind;
GameMainKind game_main_build_id(const char *main_path, char hex[41]);
// The version of a main program file (exefs/main), or NULL (missing, unreadable or unknown).
const GameVersion *game_version_of_main(const char *main_path);
// The version of the game files in game_dir (its exefs/main), or NULL.
const GameVersion *game_version_of(const char *game_dir);
// Another game on the same engine (game_addresses/games.h) in game_dir, or NULL (LA, missing or unknown).
const GameVersion *game_other_of(const char *game_dir);
// The same for a main program file wherever it is (a found exefs folder needn't be named exefs: issue #92).
const GameVersion *game_other_of_main(const char *main_path);
// Where that game's sdk module is loaded (SDK_BASE for NULL): the generator needs it before the game is selected.
uint64_t game_other_sdk_base_of(const GameVersion *v);
// That game build's release ("1.0.2"), or NULL for LA's versions.
const char *game_other_release_of(const GameVersion *v);
// The release label of the exefs folder exefs_dir: "1.0.1" or "1.1.0" when its main.npdm tells releases that share one
// main program apart (by its SHA-256), else the version's label; another game's build, its release (games.h). Display
// and the log only. NULL: unknown version.
const char *game_release_label(const char *exefs_dir, char *buf, size_t size);
// Selects the address table for the game files in game_dir and logs the version. false: why says what's wrong.
bool game_version_select(const char *game_dir, char *why, size_t why_size);
// The selected version (NULL before game_version_select).
const GameVersion *game_version(void);
// Another game on the same engine (game_addresses/games.h key, experimental) or NULL for LA. Its address table is all
// zeros: LA's features must check this and stay off.
const char *game_other(void);
// Where the sdk module is loaded: SDK_BASE for LA, the game's own for another game.
uint64_t game_sdk_base(void);
// The bytes of all the game's RomFS files in version v, summed (0: unknown). A copy with every file there but another
// total has a damaged or changed file (issue #391). Only the totals ship.
uint64_t game_romfs_bytes(const GameVersion *v);
// File by file: some copies have files that no version has, or a damaged one, beside the game's (game_addresses/
// romfs_files.h: hashes of the game's paths and sizes only). Begin with the copy's version (NULL: unknown version, the
// check does nothing), pass every file found (path relative to romfs, '/' separators) and end it.
typedef struct GameRomfsCheck GameRomfsCheck;
GameRomfsCheck *game_romfs_check_begin(const GameVersion *v);
// false: not one of this version's files (another name, or another size). bytes (or NULL): the game's files' sizes
// summed, the ones that aren't the game's left out (an extra file alone isn't a changed game file: issue #553; a game
// file of another size is missing to game_romfs_check_end). With no check (c NULL) every file is added.
bool game_romfs_check_file(GameRomfsCheck *c, const char *rel, uint64_t size, uint64_t *bytes);
// How many of this version's files weren't passed (missing, or changed: another size); frees c.
int game_romfs_check_end(GameRomfsCheck *c);
// The table's hash of a file (tools/romfs_files.py hashes the same way).
uint64_t game_romfs_hash(const char *rel, uint64_t size);
// Some copies also have the game code's six files (exefs: main, main.npdm, rtld, sdk, subsdk0, subsdk1) at the romfs
// root, each the same file as in exefs (issue #389: 3842 files). Nothing reads them there, so the checks skip them.
// true: rel is one of those names at the romfs root, and <game_dir>/romfs/<rel> has size bytes, the same bytes as
// <game_dir>/exefs/<rel>. Anything else there (another name, size or content) stays an odd file.
bool game_romfs_exefs_copy(const char *game_dir, const char *rel, uint64_t size);

#ifdef __cplusplus
}
#endif
