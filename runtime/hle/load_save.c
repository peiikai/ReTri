// --load-save <file 1-2[:row] | save folder> (debug): boot straight into a save, past the start screen, the title and
// the file select. LARECOMP_LOAD_SAVE=<same> does the same. (Ported from LAS Recomp's; the screens and the save
// layout are Echoes of Wisdom's.)
//
// The game loads the save itself: we press what a player would, one press at a time with a pause after each, and only
// on the screen it is for. The title (its movie, rd000_Title.webm, playing): A. "Select the file": Down for File 2,
// A. "Which data would you like to load?" (the file's data, newest first): Down to the row, A. "Load this data?" (the
// cursor starts on No): Up, A. So the game ends up exactly where it would after a normal load.
//   1, 2      File 1 or File 2 of the save folder (save/save_0/slot00, slot01), its newest data
//   1:3       File 1, the third row of its data list (newest first)
//   path      a save folder from elsewhere (a reporter's): the folder holding save.meta (save_0), or the save folder
//             above it, or anything inside it (a slot0N or fileNN folder, a main.bin). Copied into a scratch save
//             folder, load_save/save_0 in the data folder, and loaded from there; the game saves there too, for the
//             rest of the session: the player's own saves are never read or written. A path inside slot01 picks
//             File 2, one inside a fileNN folder picks that data's row.
// The save is checked first (save.meta, and each data's main.bin: tag, size, checksum): missing or bad -> one log line
// and the normal start, as if the option weren't given. Gives up (log line) if the game isn't in play 90 s after boot;
// never pressed again after that.
//
// EoW save layout (save_0/): save.meta ("Ms", u32 size at +8: the file list the file select shows), slot00 (File 1)
// and slot01 (File 2), each with up to six data folders file00-file05 (main.bin, maps.bin, thumb.bin). main.bin:
// "S4" + u16 1, u32 size at +8 (the file's size), u32 CRC-32 (zlib's) of bytes 0x10.. at +0xC, the save time (Unix
// seconds) at +0x28.
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../game_addresses.h"
#include "../hle.h"

#define SCRATCH "load_save"
enum { NDATA = 6 };

static char g_arg[4096];  // the option as given (a path: made absolute where we started)
static int g_file = -1;   // the file to load, 0-1 (-1: off)
static int g_row;         // the row of its data list, 0 = the newest
static int g_done;        // loaded, given up, or never on

static uint32_t crc32_zlib(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 && len < (64 << 20) ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = b ? (size_t)len : 0;
    return b;
}

// Why the data main.bin at `path` can't be loaded, or NULL; its save time into *when
static const char *data_problem(const char *path, uint32_t *when) {
    size_t n;
    uint8_t *b = read_all(path, &n);
    if (!b) return errno == ENOENT ? "missing" : strerror(errno);
    const char *why = NULL;
    uint32_t size, crc;
    if (n < 0x30) why = "too short for a save";
    else if (memcmp(b, "S4\1\0", 4)) why = "not a save of this game (tag)";
    else if (memcpy(&size, b + 8, 4), size != n) why = "damaged (size)";
    else if (memcpy(&crc, b + 0xC, 4), crc32_zlib(b + 0x10, n - 0x10) != crc) why = "damaged (checksum)";
    else memcpy(when, b + 0x28, 4);
    free(b);
    return why;
}

// Why File `file` of the save folder `save0` (the one holding save.meta) can't be loaded at `row`, or NULL. Every
// data of the file is checked (the game reads them all for its list).
static const char *file_problem(const char *save0, int file, int row) {
    static char why[1400];
    char p[1200];
    snprintf(p, sizeof p, "%s/save.meta", save0);
    size_t n;
    uint8_t *m = read_all(p, &n);
    uint32_t size = 0;
    if (m && n >= 12) memcpy(&size, m + 8, 4);
    bool meta_ok = m && n >= 12 && !memcmp(m, "Ms\1\0", 4) && size == n;
    free(m);
    if (!meta_ok) { snprintf(why, sizeof why, "%s %s", p, m ? "is damaged" : "is missing (no save there)"); return why; }
    int have = 0;
    for (int i = 0; i < NDATA; i++) {
        snprintf(p, sizeof p, "%s/slot%02d/file%02d/main.bin", save0, file, i);
        struct stat sb;
        if (stat(p, &sb)) continue;
        uint32_t when;
        const char *w = data_problem(p, &when);
        if (w) { snprintf(why, sizeof why, "%s: %s", p, w); return why; }
        have++;
    }
    if (!have) { snprintf(why, sizeof why, "File %d is empty (no data in %s/slot%02d)", file + 1, save0, file); return why; }
    if (row >= have) { snprintf(why, sizeof why, "File %d has %d data, no row %d", file + 1, have, row + 1); return why; }
    return NULL;
}

// The row of data folder `data` in File `file`'s list: newest first, by the save time in main.bin
static int data_row(const char *save0, int file, int data) {
    uint32_t t[NDATA] = {0};
    bool have[NDATA] = {0};
    for (int i = 0; i < NDATA; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/slot%02d/file%02d/main.bin", save0, file, i);
        have[i] = !data_problem(p, &t[i]);
    }
    int row = 0;
    for (int i = 0; i < NDATA; i++) if (have[i] && i != data && (t[i] > t[data] || (t[i] == t[data] && i < data))) row++;
    return row;
}

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

// Copy the folder tree `from` into `to` (files and folders only; `to` is made)
static bool copy_tree(const char *from, const char *to) {
    if (mkdir(to, 0755) && errno != EEXIST) return false;
    DIR *d = opendir(from);
    if (!d) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
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

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1300];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        remove_tree(p);
    }
    closedir(d);
    rmdir(path);
}

// (main, before the data root becomes the working directory) the option's value
void load_save_arg(const char *arg) {
    if (!realpath(arg, g_arg)) snprintf(g_arg, sizeof g_arg, "%s", arg);
#ifdef _WIN32
    for (char *p = g_arg; *p; p++) if (*p == '\\') *p = '/';  // (the folder walk below looks for '/')
#endif
}

static void off(const char *why) {
    LOG("[loadsave] %s: %s - normal start\n", g_arg, why);
    fprintf(stderr, "--load-save %s: %s - normal start\n", g_arg, why);
    g_file = -1, g_done = 1;
}

static bool has_meta(const char *dir) {
    char p[1200];
    struct stat sb;
    snprintf(p, sizeof p, "%s/save.meta", dir);
    return !stat(p, &sb) && S_ISREG(sb.st_mode);
}

// (main, in the data root, before the first boot) check the save and set it up; false: off (a normal start)
bool load_save_prepare(void) {
    if (!g_arg[0]) {
        const char *e = getenv("LARECOMP_LOAD_SAVE");
        if (!e || !*e) return false;
        load_save_arg(e);
    }
    const char *a = g_arg;
    int file, row = 1;
    char tail;
    if ((sscanf(a, "%d%c", &file, &tail) == 1 || (sscanf(a, "%d:%d%c", &file, &row, &tail) == 2)) && !strchr(a, '/')) {
        if (file < 1 || file > 2 || row < 1 || row > NDATA) { off("not a file 1-2 (with a row 1-6: 1:3) or a save folder"); return false; }
        const char *dir = getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") : "save";
        char save0[1100];
        snprintf(save0, sizeof save0, "%s/save_0", dir);
        const char *why = file_problem(save0, file - 1, row - 1);
        if (why) { off(why); return false; }
        g_file = file - 1, g_row = row - 1;
        LOG("[loadsave] loading File %d, row %d of its data (%s)\n", file, row, save0);
        return true;
    }
    // a path: the folder holding save.meta is it, a folder above it (save/), or one of its parents
    struct stat sb;
    if (stat(a, &sb)) { off(a[0] == '-' || !strchr(a, '/') ? "not a file 1-2 or a save folder" : "no such save folder"); return false; }
    char save0[4096];
    snprintf(save0, sizeof save0, "%s", a);
    if (!S_ISDIR(sb.st_mode)) *strrchr(save0, '/') = 0;
    char below[4200];
    snprintf(below, sizeof below, "%s/save_0", save0);
    if (!has_meta(save0) && has_meta(below)) snprintf(save0, sizeof save0, "%s", below);
    while (!has_meta(save0) && strrchr(save0, '/') && strrchr(save0, '/') != save0) *strrchr(save0, '/') = 0;
    if (!has_meta(save0)) { off("no save.meta in or above it: not a save folder of this game"); return false; }
    file = 0, row = 0;
    const char *rest = a + strlen(save0);  // (slot0N/fileNN/... below the save folder)
    int s, d;
    if (sscanf(rest, "/slot%2d", &s) == 1 && s >= 0 && s <= 1) file = s;
    if (sscanf(rest, "/slot%*2d/file%2d", &d) == 1 && d >= 0 && d < NDATA) row = data_row(save0, file, d);
    const char *why = file_problem(save0, file, row);
    if (why) { off(why); return false; }
    remove_tree(SCRATCH);
    mkdir(SCRATCH, 0755);
    if (!copy_tree(save0, SCRATCH "/save_0")) { off("couldn't copy it to the scratch save folder"); return false; }
    setenv("LARECOMP_SAVE_DIR", SCRATCH, 1);  // (runtime/services/fs.c: the game's save folder for this session)
    g_file = file, g_row = row;
    LOG("[loadsave] loading %s: File %d, row %d of its data (a copy in the scratch save folder %s; the game saves there "
        "this session)\n", save0, file + 1, row + 1, SCRATCH);
    return true;
}

// ---- driving the title and the file select (game thread)
extern int warp_other_in_play(void);          // runtime/hle/warp_other.c: a level's jump request registered
extern const char *warp_other_current_level(void);
extern bool movie_title_playing(void);        // runtime/hle/movie.c: the title's movie (Press Any Button)
extern uint64_t fs_mount_count(void);         // runtime/services/fs.c: save folder mounts
extern uint64_t renderer_present_count(void);

// (runtime/hle/input.c's button bits)
enum { A_BIT = 1 << 0, UP_BIT = 1 << 13, DOWN_BIT = 1 << 15 };
enum { HOLD = 6, GAP = 75 };  // a press is held 6 frames; the next one waits 75 (the menus' slide-ins are done)

// (game thread, each controller read) the buttons to add for the game
uint64_t load_save_buttons(void) {
    if (g_file < 0 || g_done || !game_other()) return 0;
    uint64_t f = renderer_present_count();
    static uint64_t start, press_at, held, title_gone_at, mounts_at;
    static int step, presses, title_a;
    // steps: 0 waiting for the title, 1 the title (A), 2 the file select, 3 the data list, 4 "load this data?",
    // 5 loading
    if (!start) start = f + 1;
    if (warp_other_in_play()) {
        LOG("[loadsave] File %d loaded: in play (%s), %llu frames after boot\n", g_file + 1, warp_other_current_level(),
            (unsigned long long)(f + 1 - start));
        g_done = 1;
        return 0;
    }
    if (f + 1 - start > 90 * 60) {
        LOG("[loadsave] File %d: not in play 90 s after boot (at step %d) - stopped (the game is left as it is)\n",
            g_file + 1, step);
        g_done = 1;
        return 0;
    }
    if (press_at && f < press_at + HOLD) return held;
    if (press_at && f < press_at + GAP) return 0;
    uint64_t want = 0;
    bool title = movie_title_playing();
    switch (step) {
    case 0:  // the title's movie up (the boot's logos before it take no presses)
        if (title) step = 1;
        return 0;
    case 1:  // the title: A, again if it's still up after a while (a press during its fade-in is dropped)
        if (!title) {
            if (!title_gone_at) title_gone_at = f;
            if (f - title_gone_at < 30) return 0;  // (gone for half a second: the file select)
            LOG("[loadsave] title passed: the file select\n");
            step = 2, presses = 0;
            return 0;
        }
        title_gone_at = 0;
        if (title_a >= 6) return 0;
        want = A_BIT, title_a++;
        break;
    case 2:  // "Select the file": File 2 is below File 1
        if (presses < g_file) want = DOWN_BIT, presses++;
        else { want = A_BIT, step = 3, presses = 0; LOG("[loadsave] file select: File %d\n", g_file + 1); }
        break;
    case 3:  // "Which data would you like to load?": newest first
        if (presses < g_row) want = DOWN_BIT, presses++;
        else { want = A_BIT, step = 4, presses = 0; LOG("[loadsave] data list: row %d\n", g_row + 1); }
        break;
    case 4:  // "Load this data?": Yes is above No (the cursor starts on No)
        if (!presses) want = UP_BIT, presses++;
        else { want = A_BIT, step = 5, mounts_at = fs_mount_count(); LOG("[loadsave] load this data: Yes\n"); }
        break;
    case 5:  // the load: the save folder is read, then a level loads
        if (fs_mount_count() != mounts_at && mounts_at != ~0ull) {
            LOG("[loadsave] the save is being read\n");
            mounts_at = ~0ull;
        } else if (mounts_at != ~0ull && f - press_at > 10 * 60) {
            LOG("[loadsave] File %d: the load didn't start 10 s after Yes - stopped (the game is left as it is)\n",
                g_file + 1);
            g_done = 1;
        }
        return 0;
    }
    press_at = f, held = want;
    return want;
}
