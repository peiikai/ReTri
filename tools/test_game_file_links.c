// The data folder's game files are hard links to the player's own (or APFS clones), as in LAS Recomp: nothing ReTri
// does to them may change the player's files (runtime/romfs_build.c, runtime/paths.c, plat_remove_tree).
//   1. A setup over an earlier one whose link couldn't be removed (held open on Windows, a read-only folder
//      elsewhere) fails instead of writing into it: "wb" on the link truncated the player's file.
//   2. Deleting the data folder's copy removes the link only: the player's file stays, with its bytes, and a
//      read-only one stays read-only (Windows cleared the attribute to delete, and a link shares it).
//   3. Game files with main beside region_common in romfs/ (issue #92): the copy keeps the program files and the
//      romfs apart.
// Scratch folders only: tools/test_game_file_links.py <empty scratch dir>.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "platform.h"  // (Windows: win_compat.h's mkdir)
#include "romfs_build.h"

// (the platform file's crash handling, never reached here)
void rt_crash_explain(char *out, size_t n, const char *log_path) { (void)log_path; if (n) out[0] = 0; }
bool rt_fault_watch(uint64_t a, uint64_t pc) { (void)a, (void)pc; return false; }
uint64_t g_fault_lr;
void rt_fault_report(const char *what, uint64_t a, uint64_t pc, uint64_t lr) {
    (void)a, (void)pc, (void)lr;
    fprintf(stderr, "fault: %s\n", what);
    abort();
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
    } while (0)

static const char PLAYER[] = "the player's own game file";

static void put(const char *p, const char *s) {
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", p); exit(2); }
    fputs(s, f);
    fclose(f);
}
static bool same(const char *p, const char *s) {
    char b[256] = {0};
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    return n == strlen(s) && !memcmp(b, s, n);
}
static bool hard_link(const char *from, const char *to) {
#ifdef _WIN32
    return CreateHardLinkA(to, from, NULL) != 0;
#else
    return link(from, to) == 0;
#endif
}
static bool read_only(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
#else
    struct stat st;
    return stat(p, &st) == 0 && !(st.st_mode & 0222);
#endif
}
static void make_read_only(const char *p, bool on) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    SetFileAttributesA(p, on ? a | FILE_ATTRIBUTE_READONLY : a & ~FILE_ATTRIBUTE_READONLY);
#else
    chmod(p, on ? 0444 : 0644);
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <empty scratch dir>\n", argv[0]); return 2; }
    char player[1100], data[1100], src[1200], dst[1200];
    snprintf(player, sizeof player, "%s/player", argv[1]);
    snprintf(data, sizeof data, "%s/data", argv[1]);
    snprintf(src, sizeof src, "%s/main", player);
    snprintf(dst, sizeof dst, "%s/main", data);
    mkdir(player, 0755);
    put(src, PLAYER);

    // 1. a setup again while the earlier copy's link can't be removed
    mkdir(data, 0755);
    CHECK(hard_link(src, dst));
#ifdef _WIN32
    // (held open without delete sharing - an indexer or antivirus: remove() fails, writing doesn't)
    HANDLE held = CreateFileA(dst, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
#else
    chmod(data, 0555);  // (the folder read-only: remove() fails, writing into the file doesn't)
#endif
    bool copied = romfs_copy_tree(player, data, NULL);
#ifdef _WIN32
    CloseHandle(held);
#else
    chmod(data, 0755);
#endif
    CHECK(same(src, PLAYER));  // (the bug: truncated, then "copied" onto itself - empty)
    printf("setup over a link it can't remove: %s, the player's file %s\n", copied ? "copied" : "failed",
           same(src, PLAYER) ? "unchanged" : "CHANGED");

    // 2. deleting the data folder's copy: the link only, the player's read-only file stays as it was. Made read-only
    //    before setup linked it, and after (Windows: the data folder's entry then still lists it as writable)
    remove(dst);
    for (int after = 0; after < 2; after++) {
        mkdir(data, 0755);
        if (!after) make_read_only(src, true);
        CHECK(hard_link(src, dst));
        if (after) make_read_only(src, true);
        CHECK(plat_remove_tree(data));
        struct stat st;
        CHECK(stat(data, &st) != 0);
        CHECK(same(src, PLAYER));
        CHECK(read_only(src));
        printf("delete through a link, read-only %s linking: the player's file %s, %s\n", after ? "after" : "before",
               same(src, PLAYER) ? "there" : "GONE", read_only(src) ? "still read-only" : "NOT READ-ONLY ANY MORE");
        make_read_only(src, false);
        plat_remove_tree(data);
    }
    plat_remove_tree(player);

    // 3. #92: main and main.npdm beside region_common in a folder named romfs. Found there as both folders; the
    //    copy's exefs gets the program files alone, its romfs everything but them.
    char flat[1100], p[1300], exefs[2200], romfs[2200];
    snprintf(flat, sizeof flat, "%s/flat", argv[1]);
    mkdir(flat, 0755);
    snprintf(p, sizeof p, "%s/romfs", flat), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/romfs/region_common", flat), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/romfs/region_common/a.bin", flat), put(p, "romfs data");
    snprintf(p, sizeof p, "%s/romfs/top.bin", flat), put(p, "romfs root file");
    static const char *const prog[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
    for (int i = 0; i < 6; i++) snprintf(p, sizeof p, "%s/romfs/%s", flat, prog[i]), put(p, prog[i]);
    bool unusable = false;
    romfs_find_game(flat, exefs, sizeof exefs, romfs, sizeof romfs, &unusable);
    snprintf(p, sizeof p, "%s/romfs", flat);
    CHECK(!strcmp(exefs, p) && !strcmp(romfs, p) && !unusable);
    char de[1200], dr[1200];
    snprintf(de, sizeof de, "%s/exefs", data), snprintf(dr, sizeof dr, "%s/romfs", data);
    mkdir(data, 0755);
    CHECK(romfs_copy_exefs(exefs, de));
    CHECK(romfs_copy_romfs(romfs, dr, !strcmp(exefs, romfs), NULL));
    for (int i = 0; i < 6; i++) {
        snprintf(p, sizeof p, "%s/%s", de, prog[i]);
        CHECK(same(p, prog[i]));
        snprintf(p, sizeof p, "%s/%s", dr, prog[i]);
        CHECK(access(p, F_OK) != 0);
    }
    snprintf(p, sizeof p, "%s/region_common", de);
    CHECK(access(p, F_OK) != 0);
    snprintf(p, sizeof p, "%s/top.bin", de);
    CHECK(access(p, F_OK) != 0);
    snprintf(p, sizeof p, "%s/region_common/a.bin", dr);
    CHECK(same(p, "romfs data"));
    snprintf(p, sizeof p, "%s/top.bin", dr);
    CHECK(same(p, "romfs root file"));
    // (an exefs folder without main: refused)
    snprintf(p, sizeof p, "%s/romfs/region_common", flat);
    CHECK(!romfs_copy_exefs(p, de));
    printf("main beside region_common: exefs %s, romfs %s\n", !strcmp(exefs, romfs) ? "found there" : "NOT FOUND",
           failures ? "copied WRONG" : "copied apart");
    plat_remove_tree(data);
    plat_remove_tree(flat);

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
