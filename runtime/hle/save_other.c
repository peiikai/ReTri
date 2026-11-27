// Echoes of Wisdom's save, live (the Save Editor: runtime/ui/save_editor.mm). Layout: docs/save-format.md.
//
// The game keeps its save data in memory as images of main.bin (the same "S4" header and chunks: Ba, Lc, Pl, Cp,
// Fg, ...), three in a row in one object, (size + 0x420) apart: the save as it was loaded, the game's current data,
// and an empty one. The middle one is live: the game reads it while playing (the max health computation reads its
// Pl chunk's Heart Containers and Pieces of Heart through the object) and saves from it. Some of the player's data
// is kept in a status object of its own while playing instead, and copied into the Pl chunk only when the game saves:
// health, max health, rupees and the energy gauge. The editor changes both, so a change shows at once and the
// game's next save writes it, with its own checksum, as any change made in play. Nothing here writes a save file:
// before the first change to a loaded save, its folder is copied to a backup.
//
// The images are found by a scan of the heap (their header, size, Ba and Pl chunks), again whenever the one kept is
// no longer one (another save loaded, the object freed). Both main.bin sizes (0x9d70 in 1.0.x, 0xad80 in 1.1.0: a
// bigger Ui chunk) lay them out so, with Pl at the same place.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"
#include "player_other.h"  // (the player status: cheats_other_status())

extern int warp_other_in_play(void);  // runtime/hle/warp_other.c
extern uint64_t renderer_present_count(void);

enum { BA_HASH = 0x9232106du, PL_TAG = 0x00016c50u, PL_HASH = 0x597a2cabu, PL_OFF = 0x160, PL_SIZE = 0x90, TAIL = 0x420 };

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static bool is_image(const uint8_t *q) {
    if (q[0] != 'S' || q[1] != '4' || q[2] != 1 || q[3] != 0 || q[0x10] != 'B' || q[0x11] != 'a') return false;
    uint32_t size = u32_at(q + 8);
    return size >= 0x8000 && size <= 0x20000 && u32_at(q + 0x14) == BA_HASH && u32_at(q + PL_OFF) == PL_TAG &&
           u32_at(q + PL_OFF + 4) == PL_HASH && u32_at(q + PL_OFF + 8) == PL_SIZE;
}

static uint64_t g_live;  // the live image (0: none known)
static uint64_t g_scan_frame = ~0ull;

// The heap scan (16-byte steps: the images are allocated so): the middle image of three in a row
static uint64_t find_live(void) {
    uint64_t heap = kernel_heap_size();
    if (heap < 0x100000) return 0;
    const uint8_t *h = (const uint8_t *)(uintptr_t)HEAP_BASE, *end = h + heap - 0x40000;
    for (const uint8_t *q = h; q < end; q += 16) {
        if (!is_image(q)) continue;
        uint32_t size = u32_at(q + 8);
        const uint8_t *b = q + size + TAIL, *c = b + size + TAIL;
        if (c + size < end && is_image(b) && is_image(c) && u32_at(b + 8) == size && u32_at(c + 8) == size)
            return (uint64_t)(uintptr_t)b;
    }
    return 0;
}

// The live image's Pl chunk body (0x80 bytes), or NULL: no save in play
uint8_t *save_other_pl(void) {
    if (!warp_other_in_play()) return NULL;
    if (g_live && is_image((const uint8_t *)(uintptr_t)g_live)) return (uint8_t *)(uintptr_t)(g_live + PL_OFF + 0x10);
    uint64_t f = renderer_present_count();
    if (g_scan_frame != ~0ull && f - g_scan_frame < 60) return NULL;  // (a scan a second at most)
    g_scan_frame = f;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_live = find_live();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    LOG("[saveedit] live save image %s%llx (%.0f ms)\n", g_live ? "at " : "not found ", (unsigned long long)g_live,
        (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    return g_live ? (uint8_t *)(uintptr_t)(g_live + PL_OFF + 0x10) : NULL;
}

uint64_t save_other_status(void) { return warp_other_in_play() ? cheats_other_status() : 0; }

// ---- the backup: the save folder as it is, once for each loaded save, before its first change
extern const char *g_save_dir;  // runtime/services/fs.c

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[1 << 15];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    fclose(in);
    return fclose(out) == 0 && ok;
}
static bool copy_tree(const char *from, const char *to) {
#ifdef _WIN32
    if (mkdir(to) && errno != EEXIST) return false;
#else
    if (mkdir(to, 0755) && errno != EEXIST) return false;
#endif
    DIR *d = opendir(from);
    if (!d) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".pending")) continue;  // (a save being written isn't one yet)
        char a[1300], b[1300];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat sb;
        if (stat(a, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) ok = copy_tree(a, b);
        else if (S_ISREG(sb.st_mode)) ok = copy_file(a, b);
    }
    closedir(d);
    return ok;
}

static uint32_t g_backed_up;  // the loaded save (its save time) whose folder has a backup

// Before a change: true when the loaded save's folder has a backup (made now, or before for this load). The editor
// changes nothing without one.
bool save_other_backup(void) {
    if (!g_live) return false;
    const uint8_t *live = (const uint8_t *)(uintptr_t)g_live, *loaded = live - (u32_at(live + 8) + TAIL);
    uint32_t when = u32_at((is_image(loaded) ? loaded : live) + 0x28);  // (the save as loaded: its save time)
    if (g_backed_up && g_backed_up == when) return true;
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char dst[1100];
    snprintf(dst, sizeof dst, "%s-backup-%04d%02d%02d-%02d%02d%02d", g_save_dir, tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    bool ok = copy_tree(g_save_dir, dst);
    LOG("[saveedit] backup of the save folder to %s: %s\n", dst, ok ? "done" : "failed (nothing changed)");
    if (ok) g_backed_up = when;
    return ok;
}
