// Warp for another game on the engine (experimental: Echoes of Wisdom).
//
// EoW's event actions GameControl.RequestLevelJump (main+0x15a9fcc) and the respawn action (main+0x15a6520) don't
// change level themselves: they write the level's jump request object R and return, and the game acts on it once it
// sees a destination there. R has LA's request layout at another offset: level name at R+0x218 (length R+0x258),
// locator (spawn point) at R+0x260 (length R+0x2a0, the field that says "a destination"), offset vec4 at R+0x2b0. The
// actions find R through main+0x160a0b0: the level object's +0x1c0. A level registers R there while it loads; that
// store is where we hear of it (rt_other_request_obj).
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

static volatile uint64_t g_req;     // the current level's request object
static volatile uint64_t g_holder;  // the request processor's object (the request at +0x10)
static volatile uint64_t g_owner, g_ctx;  // (its owner - the job queue - and context, at the last run)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_level[64], g_locator[64];
static volatile int g_pending;

void rt_other_request_obj(uint64_t r) {
    if (r != g_req) LOG("[warp] level-jump request object %llx\n", (unsigned long long)r);
    g_req = r;
}

// Generated code (tools/gen.py): the request processor is running with its object P; the request is at P+0x10.
void rt_other_request_holder(uint64_t p) {
    uint64_t r = *(const uint64_t *)(uintptr_t)(p + 0x10);
    static uint64_t calls;
    if (getenv("LARECOMP_LOG_WARP") && !(++calls & (calls - 1)))
        LOG("[warp] processor run #%llu (holder %llx, request %llx, destination length %llu)\n", (unsigned long long)calls,
            (unsigned long long)p, (unsigned long long)r, r ? (unsigned long long)*(const uint64_t *)(uintptr_t)(r + 0x2a0) : 0ull);
    if (getenv("LARECOMP_LOG_WARP")) {
        uint64_t o = *(const uint64_t *)(uintptr_t)(p + 8);
        LOG("[warp] job %llx: vt %llx owner %llx req %llx ctl %llx ctx %llx +28 %llx; owner queue blocks %llx index %llu count %llu\n",
            (unsigned long long)p, (unsigned long long)*(const uint64_t *)(uintptr_t)p, (unsigned long long)o, (unsigned long long)r,
            (unsigned long long)*(const uint64_t *)(uintptr_t)(p + 0x18), (unsigned long long)*(const uint64_t *)(uintptr_t)(p + 0x20),
            (unsigned long long)*(const uint64_t *)(uintptr_t)(p + 0x28), (unsigned long long)*(const uint64_t *)(uintptr_t)(o + 0x200),
            (unsigned long long)*(const uint64_t *)(uintptr_t)(o + 0x218), (unsigned long long)*(const uint64_t *)(uintptr_t)(o + 0x220));
    }
    if (r >= GUEST_LO && r < GUEST_HI) { g_holder = p; g_owner = *(const uint64_t *)(uintptr_t)(p + 8); g_ctx = *(const uint64_t *)(uintptr_t)(p + 0x20); rt_other_request_obj(r); }
}

void warp_other_request(const char *level, const char *locator) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_level, sizeof g_level, "%s", level);
    snprintf(g_locator, sizeof g_locator, "%s", locator);
    g_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

static void tour_frame(uint64_t frame);

// Render thread, once a frame: the test hooks queue warps (handed to the game by warp_other_game_tick).
void warp_other_frame(uint64_t frame) {
    static long test_at = -1;  // (test) LARECOMP_WARP_AT=<frame>:<level>:<locator>
    static char tl[64], tc[64];
    if (test_at < 0) {
        const char *e = getenv("LARECOMP_WARP_AT");
        test_at = 0;
        if (e && sscanf(e, "%ld:%63[^:]:%63s", &test_at, tl, tc) != 3) test_at = 0;
    }
    tour_frame(frame);
    if (test_at && (long)frame == test_at) { warp_other_request(tl, tc); LOG("[warp] test warp queued at frame %llu (request %llx)\n", (unsigned long long)frame, (unsigned long long)g_req); }
}

// Game main thread, once per frame (input.c, the controller read): a queued warp is written into the request and
// the game's request processor (main+0x160f7e0, its object: the request at +0x10) is run on it, as LA's warp does.
// The request the processor ran with at the last load is that load's own (consumed); the level since registered a
// new one of the same class. Found by its vtable: an object with the old request's, empty, at another address.
static uint64_t find_current_request(uint64_t old) {
    extern uint64_t kernel_heap_size(void);
    uint64_t vt = *(const uint64_t *)(uintptr_t)old, found = 0;
    const uint64_t *h = (const uint64_t *)(uintptr_t)HEAP_BASE, *end = h + kernel_heap_size() / 8;
    int n = 0;
    for (const uint64_t *q = h; q < end; q++) {
        if (*q != vt) continue;
        uint64_t a = (uint64_t)(uintptr_t)q;
        if (a == old || a + 0x2c0 > HEAP_BASE + kernel_heap_size()) continue;
        uint64_t ll = q[0x258 / 8], lc = q[0x2a0 / 8];
        LOG("[warp] same class as the request: %llx (level length %llu, locator length %llu)\n", (unsigned long long)a,
            (unsigned long long)ll, (unsigned long long)lc);
        if (ll < 0x40 && lc < 0x40) found = a, n++;
    }
    LOG("[warp] %d candidate request(s) besides %llx (vtable %llx)\n", n, (unsigned long long)old, (unsigned long long)vt);
    return n == 1 ? found : 0;
}

// (debug, LARECOMP_WARP_SCAN) globals in main's data that lead to a level object holding a request at +0x1c0:
// G -> S (or G -> X, X+0x28 -> S), S+0x78 -> level object, its +0x1c0 -> R with a request's empty fields
static int in_heap(uint64_t a, uint64_t n) {
    extern uint64_t kernel_heap_size(void);
    return a >= HEAP_BASE && a < HEAP_BASE + kernel_heap_size() - n && !(a & 7);  // (no a + n: wraps near 2^64)
}
static uint64_t ld(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }
static int request_shaped(uint64_t r) {
    if (!in_heap(r, 0x2c0)) return 0;
    uint64_t ll = ld(r + 0x258), lc = ld(r + 0x2a0);
    return ll < 0x40 && lc < 0x40 && strnlen((const char *)(uintptr_t)(r + 0x218), 0x40) == ll &&
           strnlen((const char *)(uintptr_t)(r + 0x260), 0x40) == lc;
}
static void scan_globals(void) {
    int hits = 0;
    for (uint64_t g = 0x7003d00000ull; g < 0x7004400000ull && hits < 40; g += 8) {
        uint64_t v = ld(g);
        if (!in_heap(v, 0x80)) continue;
        uint64_t ss[2] = {v, in_heap(v, 0x30) ? ld(v + 0x28) : 0};
        for (int k = 0; k < 2; k++) {
            uint64_t S = ss[k];
            if (!in_heap(S, 0x80)) continue;
            uint64_t obj = ld(S + 0x78);
            if (!in_heap(obj, 0x1c8)) continue;
            uint64_t r = ld(obj + 0x1c0);
            if (!request_shaped(r)) continue;
            LOG("[warp-scan] global %llx%s -> S %llx -> level %llx -> request %llx ('%s' / '%s')\n", (unsigned long long)g,
                k ? " (+0x28)" : "", (unsigned long long)S, (unsigned long long)obj, (unsigned long long)r,
                (const char *)(uintptr_t)(r + 0x218), (const char *)(uintptr_t)(r + 0x260));
            hits++;
        }
    }
    LOG("[warp-scan] %d hits\n", hits);
}

void warp_other_game_tick(Cpu *c) {
    {   // (debug) LARECOMP_WATCH_LIVE=<frame>: from then, who touches the request reached through LARECOMP_WARP_GLOBAL
        static long at = -1;
        if (at < 0) at = getenv("LARECOMP_WATCH_LIVE") ? atol(getenv("LARECOMP_WATCH_LIVE")) : 0;
        extern uint64_t renderer_present_count(void);
        const char *e = getenv("LARECOMP_WARP_GLOBAL");
        if (at && e && (long)renderer_present_count() >= at) {
            at = 0;
            uint64_t S = ld(strtoull(e, NULL, 16)), obj = in_heap(S, 0x80) ? ld(S + 0x78) : 0, rr = in_heap(obj, 0x1c8) ? ld(obj + 0x1c0) : 0;
            if (request_shaped(rr)) {
                extern uint64_t g_watch_lo, g_watch_hi;
                extern void watch_arm(void);
                g_watch_lo = rr + 0x200, g_watch_hi = rr + 0x2c0;
                watch_arm();
                LOG("[warp] watching the live request %llx\n", (unsigned long long)rr);
            }
        }
    }
    if (!g_pending) return;
    uint64_t r = g_req;
    if (!r) return;
    if (getenv("LARECOMP_WARP_SCAN")) scan_globals();
    {   // (test) LARECOMP_WARP_GLOBAL=<hex>: the request through that global: [[[G]+0x78]+0x1c0]
        const char *e = getenv("LARECOMP_WARP_GLOBAL");
        if (e) {
            uint64_t S = ld(strtoull(e, NULL, 16)), obj = in_heap(S, 0x80) ? ld(S + 0x78) : 0, rr = in_heap(obj, 0x1c8) ? ld(obj + 0x1c0) : 0;
            if (request_shaped(rr)) r = rr;
            else LOG("[warp] no request through global %s\n", e);
        }
    }
    if (*(volatile uint64_t *)(uintptr_t)(r + 0x2a0)) {  // (a jump of the game's own is under way)
        static int said;
        if (!said++) LOG("[warp] waiting: request %llx holds a destination already (%.64s)\n", (unsigned long long)r,
                         (const char *)(uintptr_t)(r + 0x260));
        return;
    }
    pthread_mutex_lock(&g_lock);
    size_t ll = strlen(g_level), lc = strlen(g_locator);
    memcpy((void *)(uintptr_t)(r + 0x218), g_level, ll + 1);
    *(uint64_t *)(uintptr_t)(r + 0x258) = ll;
    memcpy((void *)(uintptr_t)(r + 0x260), g_locator, lc + 1);
    memset((void *)(uintptr_t)(r + 0x2b0), 0, 16);
    *(uint64_t *)(uintptr_t)(r + 0x2a0) = lc;
    g_pending = 0;
    pthread_mutex_unlock(&g_lock);
    LOG("[warp] jumping to %s / %s (request %llx)\n", g_level, g_locator, (unsigned long long)r);
    if (getenv("LARECOMP_WARP_CALL") && g_owner && g_ctx) {  // (experiment) run the request processor on a job of our own
        // P: {vtable, the game's owner, our request, 0, the last context, 0} - no reference counts of the game's to drop.
        // The processor first pops its own entry off the owner's job queue (+0x200 block array of 256 16-byte entries,
        // +0x218 front index, +0x220 count): with the queue empty, one empty entry is put at the front for it.
        uint64_t O = g_owner, idx = *(uint64_t *)(uintptr_t)(O + 0x218), cnt = *(uint64_t *)(uintptr_t)(O + 0x220);
        uint64_t blocks = *(uint64_t *)(uintptr_t)(O + 0x200), blk = blocks ? *(uint64_t *)(uintptr_t)(blocks + (idx >> 8) * 8) : 0;
        if (cnt || !blk) {
            LOG("[warp] job queue busy (index %llu, count %llu, block %llx): not now\n", (unsigned long long)idx, (unsigned long long)cnt, (unsigned long long)blk);
            return;
        }
        static uint64_t P, R2;
        if (!P) { P = (uint64_t)(uintptr_t)hle_alloc(0x200); R2 = (uint64_t)(uintptr_t)hle_alloc(0x400); }
        memset((void *)(uintptr_t)(blk + (idx & 0xff) * 16), 0, 16);
        *(uint64_t *)(uintptr_t)(blk + (idx & 0xff) * 16) = P;  // (the entry names the job, no reference count)
        *(uint64_t *)(uintptr_t)(O + 0x220) = 1;
        memset((void *)(uintptr_t)R2, 0, 0x400);
        memcpy((void *)(uintptr_t)(R2 + 0x200), (void *)(uintptr_t)(r + 0x200), 0xc0);
        memset((void *)(uintptr_t)P, 0, 0x200);
        *(uint64_t *)(uintptr_t)P = 0x7003dc6f98ull;
        *(uint64_t *)(uintptr_t)(P + 8) = O;
        *(uint64_t *)(uintptr_t)(P + 0x10) = R2;
        *(uint64_t *)(uintptr_t)(P + 0x20) = g_ctx;
        uint64_t saved[31], sp = c->sp, pc = c->pc;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = P;
        LOG("[warp] calling the request processor on job %llx (owner %llx, context %llx)\n", (unsigned long long)P,
            (unsigned long long)O, (unsigned long long)g_ctx);
        guest_call(c, 0x700160f7e0ull);
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp, c->pc = pc;
        LOG("[warp] processor returned; owner queue index %llu count %llu\n", (unsigned long long)*(uint64_t *)(uintptr_t)(O + 0x218),
            (unsigned long long)*(uint64_t *)(uintptr_t)(O + 0x220));
    }
    if (getenv("LARECOMP_WATCH_REQ")) {  // (debug) who reads it now
        extern uint64_t g_watch_lo, g_watch_hi;
        extern void watch_arm(void);
        g_watch_lo = r + 0x200, g_watch_hi = r + 0x2c0;
        watch_arm();
    }
}

// ---- the places: built from the game's own level files at first use (no names kept here). A level's spawn points
// are the strings in its .elo named "<level>_<digits>" (Church_001, BS_Dungeon001_002); the overworld's ("Field")
// are "Field_<region>_<digits>" - one per region.
#include <ctype.h>
#include <dirent.h>

typedef struct { char level[64], locator[64]; } WarpPlace;
static WarpPlace *g_places;
static int g_nplaces;

static int name_char(int ch) { return isalnum(ch) || ch == '_'; }
static int digits_after(const char *s, size_t n) {  // s[n..] = "_" digits, then the end or "_" more
    if (s[n] != '_' || !isdigit((unsigned char)s[n + 1])) return 0;
    size_t i = n + 1;
    while (isdigit((unsigned char)s[i])) i++;
    return s[i] == 0;
}
static int cmp_place(const void *a, const void *b) {
    const WarpPlace *x = a, *y = b;
    int c = strcmp(x->level, y->level);
    return c ? c : strcmp(x->locator, y->locator);
}

static void scan_level(const char *dir, const char *level) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.elo", dir, level);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return; }
    fclose(f);
    b[n] = 0;
    size_t ll = strlen(level);
    bool field = !strcmp(level, "Field");
    char regions[64][64];
    int nregions = 0;
    for (long i = 0; i < n; i++) {
        if (!name_char((unsigned char)b[i]) || (i && b[i - 1])) continue;  // (strings start after a NUL)
        long j = i;
        while (j < n && name_char((unsigned char)b[j])) j++;
        if (j >= n || b[j] || j - i < 4 || j - i >= 64) { i = j; continue; }
        const char *s = b + i;
        bool ok = false;
        if (!strncmp(s, level, ll) && digits_after(s, ll)) ok = !field;
        else if (field && !strncmp(s, "Field_", 6)) {  // Field_<region>_<digits>: the first of each region
            const char *u = strrchr(s, '_');
            if (u > s + 6 && isdigit((unsigned char)u[1])) {
                int k = 0;
                size_t rl = (size_t)(u - s);
                for (; k < nregions && !(strlen(regions[k]) == rl && !strncmp(regions[k], s, rl)); k++) {}
                if (k == nregions && nregions < 64) { snprintf(regions[nregions++], 64, "%.*s", (int)rl, s); ok = true; }
            }
        }
        if (ok) {
            bool dup = false;
            for (int k = 0; k < g_nplaces && !dup; k++) dup = !strcmp(g_places[k].level, level) && !strcmp(g_places[k].locator, s);
            if (!dup) {
                g_places = realloc(g_places, sizeof *g_places * (size_t)(g_nplaces + 1));
                snprintf(g_places[g_nplaces].level, 64, "%s", level);
                snprintf(g_places[g_nplaces].locator, 64, "%s", s);
                g_nplaces++;
            }
        }
        i = j;
    }
    free(b);
}

int warp_other_places(const void **outv) {
    const WarpPlace **out = (const WarpPlace **)outv;
    static int built;
    if (!built) {
        built = 1;
        extern const char *g_romfs_dir;
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/region_common/level", g_romfs_dir ? g_romfs_dir : "");
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".elo")) continue;
            char level[64];
            snprintf(level, sizeof level, "%.*s", (int)(l - 4), e->d_name);
            scan_level(dir, level);
        }
        if (d) closedir(d);
        if (g_nplaces) qsort(g_places, (size_t)g_nplaces, sizeof *g_places, cmp_place);
        LOG("[warp] %d places from %s\n", g_nplaces, dir);
    }
    if (out) *out = g_places;
    return g_nplaces;
}
const char *warp_other_place_level(int i) { return i >= 0 && i < g_nplaces ? g_places[i].level : ""; }
const char *warp_other_place_locator(int i) { return i >= 0 && i < g_nplaces ? g_places[i].locator : ""; }

// ---- level loads (fs.c: the first read of each romfs file)
static volatile uint64_t g_loads;
static char g_loaded[64];
const char *warp_other_current_level(void) { return g_loaded; }
void warp_other_file_read(const char *path) {
    const char *p = strstr(path, "region_common/level/");
    if (!p) return;
    p += 20;
    const char *dot = strstr(p, ".elo");
    if (!dot || dot - p >= 63) return;
    snprintf(g_loaded, sizeof g_loaded, "%.*s", (int)(dot - p), p);
    g_loads++;
}

// ---- (test) the tour: LARECOMP_WARP_TOUR=<from>[:<to>] warps to every place in turn, once the game is in play
// (a request object registered, past frame LARECOMP_WARP_TOUR_START, default 1700). Each place: the warp, its
// level's file read (or 30 s: "no load"), then 6 s there - frame times measured, a screenshot into
// build/tour/eow_<n>.png. "[tour]" lines: start / result per place; a crash ends the process (the driver script
// starts again after it).
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void tour_frame(uint64_t frame) {
    static int on = -1, cur, to, state;  // state: 0 idle, 1 warped (waiting for the load), 2 settling
    static long start_frame;
    static uint64_t loads_at;
    static double t0, t_load, last, worst, sum;
    static int nframes, slow;
    if (on < 0) {
        const char *e = getenv("LARECOMP_WARP_TOUR");
        on = e != NULL;
        if (on) {
            cur = atoi(e);
            const char *c = strchr(e, ':');
            to = c ? atoi(c + 1) : 1 << 30;
            const char *s = getenv("LARECOMP_WARP_TOUR_START");
            start_frame = s ? atol(s) : 1700;
            mkdir("build", 0755);
            mkdir("build/tour", 0755);
        }
    }
    if (!on || !g_req || (long)frame < start_frame) return;
    int n = warp_other_places(NULL);
    double t = now_s();
    if (state == 2) {  // frame times while settling
        double dt = t - last;
        if (dt > worst) worst = dt;
        if (dt > 1.5 / 60) slow++;
        sum += dt, nframes++;
    }
    last = t;
    if (state == 0) {
        if (cur >= n || cur > to) {
            LOG("[tour] done (%d places)\n", n);
            on = 0;
            fflush(stderr);
            _exit(0);
        }
        LOG("[tour] start %d %s %s\n", cur, g_places[cur].level, g_places[cur].locator);
        fflush(stderr);
        warp_other_request(g_places[cur].level, g_places[cur].locator);
        loads_at = g_loads, t0 = t, state = 1;
    } else if (state == 1) {
        if (g_loads != loads_at && !strcmp(g_loaded, g_places[cur].level)) {
            t_load = t - t0, state = 2, worst = sum = 0, nframes = slow = 0;
        } else if (t - t0 > 30) {
            LOG("[tour] result %d %s %s: no load in 30 s (last level read: %s)\n", cur, g_places[cur].level,
                g_places[cur].locator, g_loaded);
            cur++, state = 0;
        }
    } else if (state == 2 && t - t0 - t_load > 6.0) {
        extern char g_window_shot_request[256];
        snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/tour/eow_%03d.png", cur);
        LOG("[tour] result %d %s %s: loaded in %.1f s, %.1f fps, worst frame %.0f ms, %d slow frames\n", cur,
            g_places[cur].level, g_places[cur].locator, t_load, nframes ? nframes / sum : 0.0, worst * 1000, slow);
        nframes = 0;
        fflush(stderr);
        state = 3;
    } else if (state == 3) {  // (the screenshot is taken with this frame: the next warp after it)
        if (++nframes > 70) cur++, state = 0;
    }
}
