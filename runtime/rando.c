// Randomizer seeds: game-neutral plumbing, waiting for the EoW randomizer (RANDO_ON, rando.h).
//
// A seed lives in rando/<name>/: romfs/ (replacement game files), settings.json and save/ (this seed's own save
// slots). The active seed is chosen with g_settings.rando_seed (larecomp.ini); switching reboots the game in place
// (runtime/main.c), which calls rando_boot.
#include "rando.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle.h"
#include "settings.h"

static char g_seed_dir[512];
static bool g_active;
static int g_inited;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

void rando_init(void);
const char *rando_seed_dir(void) {
    rando_init();  // (lazy: callers can run before anything else touched the seed, e.g. the launch movie)
    return g_active ? g_seed_dir : NULL;
}

static void init(void) {
    if (!RANDO_ON) return;  // (every boot vanilla, a seed chosen before included)
    settings_load();
    const char *seed = getenv("LARECOMP_RANDO_SEED") ? getenv("LARECOMP_RANDO_SEED") : g_settings.rando_seed;
    if (!seed || !seed[0]) return;
    snprintf(g_seed_dir, sizeof g_seed_dir, "rando/%s", seed);
    struct stat sb;
    if (stat(g_seed_dir, &sb) || !S_ISDIR(sb.st_mode)) { LOG("[rando] seed folder %s missing; playing vanilla\n", g_seed_dir); return; }
    g_active = true;
    char p[600];
    // the seed ID (the start screen writes it next to a seed it makes): it regenerates the same seed, so a log a
    // player attaches to an issue is enough to reproduce their seed
    char id[96] = "";
    snprintf(p, sizeof p, "%s/seed_id.txt", g_seed_dir);
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
        id[strcspn(id, "\r\n")] = 0;
    }
    if (id[0]) LOG("[rando] seed \"%s\" active, seed ID %s\n", seed, id);
    else LOG("[rando] seed \"%s\" active (no seed ID saved with it)\n", seed);
}

void rando_init(void) {
    if (__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&g_init_lock);
    if (!g_inited) { init(); __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_init_lock);
}

// Each boot, right after the modules are loaded (runtime/main.c): the chosen seed (it may have changed since the
// last boot).
void rando_boot(void) {
    pthread_mutex_lock(&g_init_lock);
    g_active = false;
    g_seed_dir[0] = 0;
    init();
    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_init_lock);
}

// Pick the seed for the next boot ("" = vanilla) and remember it (larecomp.ini); a choice made in the game wins over
// LARECOMP_RANDO_SEED (test runs).
void rando_select(const char *seed) {
    strlcpy(g_settings.rando_seed, seed ? seed : "", sizeof g_settings.rando_seed);
    if (seed && *seed) strlcpy(g_settings.rando_last, seed, sizeof g_settings.rando_last);
    settings_save();
    unsetenv("LARECOMP_RANDO_SEED");
}

bool rando_seed_there(const char *seed) {
    if (!seed || !seed[0] || seed[0] == '.' || strchr(seed, '/') || strchr(seed, '\\')) return false;
    char p[600];
    struct stat sb;
    snprintf(p, sizeof p, "rando/%s/settings.json", seed);
    if (stat(p, &sb) || !S_ISREG(sb.st_mode)) return false;
    snprintf(p, sizeof p, "rando/%s/romfs", seed);
    return !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

// Host path of a seed's replacement for romfs file `path` (relative, e.g. "region_common/event/Tarin.bfevfl").
bool rando_romfs_override(const char *path, char *out, size_t cap) {
    rando_init();
    if (!g_active) return false;
    snprintf(out, cap, "%s/romfs/%s", g_seed_dir, path);
    struct stat sb;
    return stat(out, &sb) == 0 && S_ISREG(sb.st_mode);
}
