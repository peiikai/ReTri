// Warp for another game on the engine (experimental: Echoes of Wisdom).
//
// A warp starts the game's own fast-travel event (start_map_warp: event/MapWarp.bfevfl with our level and locator as
// its parameters), as the map screen does; the event's GameControl.RequestLevelJump fills the level's jump request
// and the event system performs it when the event ends. Writing the request alone does nothing: the game reads it only
// when an event ends (its processor, OTHER(WARP_REQUEST_PROC), runs then). The request has LA's layout at another
// offset: level at R+0x218 (length +0x258), locator at +0x260 (length +0x2a0), offset +0x2b0; runtime/gen/patches.cpp
// hooks the processor to tell us where it is (rt_other_request_holder). The places come from the game's level files (warp_other_places).
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../game_addresses.h"

static volatile uint64_t g_loads;   // level files read (warp_other_file_read)
static volatile uint64_t g_proc_runs;  // the request processor's runs (one per level change)
static char g_loaded[64];           // the last level read
static char g_current[64];          // the level the last level jump went to (the request at its processor's run)
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

// Generated code (runtime/gen/patches.cpp eow_request): the request processor is running with its object P; the request is at P+0x10.
void rt_other_request_holder(uint64_t p) {
    uint64_t r = *(const uint64_t *)(uintptr_t)(p + 0x10);
    g_proc_runs++;
    static uint64_t calls;
    calls++;
    if (getenv("LARECOMP_LOG_WARP") && !(calls & (calls - 1)))
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
    // (the request's level is where this jump goes - also back to a level still loaded, which no file read shows:
    // the overworld after a cave)
    if (r >= GUEST_LO && r < GUEST_HI) {
        uint64_t ll = *(const uint64_t *)(uintptr_t)(r + 0x258);
        const char *lv = (const char *)(uintptr_t)(r + 0x218);
        if (ll > 0 && ll < 0x40 && strnlen(lv, 0x40) == ll) memcpy(g_current, lv, ll + 1);
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

// (debug) LARECOMP_TRIGGER_DIR=<dir>: while a copy runs, files dropped in that folder act once, then are deleted.
// "flags": "<save main.bin path> <section start hex> <byte.bit ...>" clears those bits of a save's flag bitfield in the
// game's own copy (found in guest memory by the save section's first 64 bytes), so a fight already won comes back at
// the next room entry. "capture": "<every N frames> <for M frames>" saves the window then to build/cap/frame_<n>.png.
static char *trigger_take(const char *name) {
    const char *d = getenv("LARECOMP_TRIGGER_DIR");
    if (!d) return NULL;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", d, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 65536);
    if (b) fread(b, 1, 65535, f);
    fclose(f);
    unlink(path);
    return b;
}
static void trigger_flags(void) {
    static int n;
    if (++n % 30) return;  // (twice a second)
    char *t = trigger_take("flags");
    if (!t) return;
    char save[512];
    unsigned sec = 0;
    int used = 0;
    if (sscanf(t, "%511s %x%n", save, &sec, &used) == 2) {
        FILE *f = fopen(save, "rb");
        uint8_t key[64];
        if (f && fseek(f, sec + 16, SEEK_SET) == 0 && fread(key, 1, 64, f) == 64) {
            extern uint64_t kernel_heap_size(void);
            uint8_t *h = (uint8_t *)(uintptr_t)HEAP_BASE, *end = h + kernel_heap_size() - 0x2000;
            int hits = 0;
            for (uint8_t *q = h; q < end; q += 4) {
                if (memcmp(q, key, 64)) continue;
                hits++;
                const char *p = t + used;
                unsigned off, bit;
                int k, nb = 0;
                while (sscanf(p, " %x.%u%n", &off, &bit, &k) == 2) { q[off] &= (uint8_t)~(1u << bit); p += k; nb++; }
                LOG("[trigger] flags: cleared %d bits at %llx\n", nb, (unsigned long long)(uintptr_t)q);
            }
            LOG("[trigger] flags: %d copies of %s's flags found\n", hits, save);
        } else LOG("[trigger] flags: can't read %s\n", save);
        if (f) fclose(f);
    }
    free(t);
}
// "teleport": "<x> <y> <z> <r> <x2> <y2> <z2>" (or LARECOMP_TELEPORT_AT=<frame>:<same 7 numbers>): every float triple in
// guest memory within r of x,y,z (Zelda's position and its copies, right after a warp to a spawn point) is moved by
// (x2,y2,z2)-(x,y,z) - a place with no spawn point of its own. Ugly, debug only.
static void teleport(const float *v) {
    extern uint64_t kernel_heap_size(void);
    uint8_t *h = (uint8_t *)(uintptr_t)HEAP_BASE, *end = h + kernel_heap_size() - 16;
    int hits = 0;
    float d[3] = {v[4] - v[0], v[5] - v[1], v[6] - v[2]}, r = v[3];
    for (uint8_t *q = h; q < end; q += 4) {
        float *f = (float *)q;
        if (!(fabsf(f[1] - v[1]) <= 1.0f && fabsf(f[0] - v[0]) <= r && fabsf(f[2] - v[2]) <= r)) continue;
        if (f[0] == 0 || f[2] == 0) continue;
        for (int i = 0; i < 3; i++) f[i] += d[i];
        hits++;
        q += 8;
    }
    LOG("[trigger] teleport: %d triples moved by %.1f %.1f %.1f\n", hits, d[0], d[1], d[2]);
}
static void trigger_teleport(void) {
    static long at = -1;
    static float tv[7];
    if (at < 0) {
        const char *e = getenv("LARECOMP_TELEPORT_AT");
        at = 0;
        if (e && sscanf(e, "%ld:%f:%f:%f:%f:%f:%f:%f", &at, &tv[0], &tv[1], &tv[2], &tv[3], &tv[4], &tv[5], &tv[6]) != 8) at = 0;
    }
    extern uint64_t renderer_present_count(void);
    if (at && (long)renderer_present_count() >= at) { at = 0; teleport(tv); }
    static int n;
    if (++n % 30) return;
    char *t = trigger_take("teleport");
    if (!t) return;
    float v[7];
    if (sscanf(t, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) teleport(v);
    free(t);
}
// "hpsnap": "<x> <y> <z> <r>": keep a copy of 16 KB around every float triple near x,y,z (an actor's position and
// its copies); "hpdiff": log the 32-bit words there that went down by 1-255 since (its health, after a hit);
// "poke": "<hex address> <i32|f32|u16|u8> <value>". Ugly, debug only.
typedef struct { uint8_t *at; uint8_t old[0x4000]; } HpWin;
static HpWin *g_hpw;
static int g_nhpw;
static void trigger_hp(void) {
    static int n;
    if (++n % 30) return;
    char *t = trigger_take("hpsnap");
    if (t) {
        float v[4];
        if (sscanf(t, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4) {
            extern uint64_t kernel_heap_size(void);
            uint8_t *h = (uint8_t *)(uintptr_t)HEAP_BASE, *end = h + kernel_heap_size() - 0x2000;
            free(g_hpw); g_hpw = NULL; g_nhpw = 0;
            for (uint8_t *q = h + 0x2000; q < end && g_nhpw < 512; q += 4) {
                float *f = (float *)q;
                if (!(fabsf(f[0] - v[0]) <= v[3] && fabsf(f[1] - v[1]) <= 1.5f && fabsf(f[2] - v[2]) <= v[3])) continue;
                uint8_t *w = (uint8_t *)((uintptr_t)(q - 0x2000) & ~(uintptr_t)0xf);
                if (g_nhpw && w < g_hpw[g_nhpw - 1].at + 0x4000) continue;
                g_hpw = realloc(g_hpw, sizeof *g_hpw * (size_t)(g_nhpw + 1));
                g_hpw[g_nhpw].at = w;
                memcpy(g_hpw[g_nhpw].old, w, 0x4000);
                g_nhpw++;
            }
            LOG("[trigger] hpsnap: %d windows\n", g_nhpw);
        }
        free(t);
    }
    t = trigger_take("hpdiff");
    if (t) {
        int hits = 0;
        for (int i = 0; i < g_nhpw; i++)
            for (int o = 0; o < 0x4000; o += 4) {
                int32_t a, b;
                memcpy(&a, g_hpw[i].old + o, 4); memcpy(&b, g_hpw[i].at + o, 4);
                float fa, fb;
                memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                bool ints = a > 0 && a < 100000 && b >= 0 && a - b >= 1 && a - b <= 255;
                bool flts = fa > 0 && fa < 100000 && fb >= 0 && fa - fb >= 0.25f && fa - fb <= 255 && fa == (int)fa && fb == (int)fb;
                if ((ints || flts) && hits++ < 200)
                    LOG("[trigger] hpdiff: %llx %s %d -> %d\n", (unsigned long long)(uintptr_t)(g_hpw[i].at + o), ints ? "i32" : "f32",
                        ints ? a : (int)fa, ints ? b : (int)fb);
            }
        LOG("[trigger] hpdiff: %d candidates\n", hits);
        free(t);
    }
    t = trigger_take("poke");
    if (t) {
        unsigned long long a;
        char ty[8];
        double val;
        if (sscanf(t, "%llx %7s %lf", &a, ty, &val) == 3 && a >= HEAP_BASE) {
            void *p = (void *)(uintptr_t)a;
            if (!strcmp(ty, "i32")) { int32_t x = (int32_t)val; memcpy(p, &x, 4); }
            else if (!strcmp(ty, "f32")) { float x = (float)val; memcpy(p, &x, 4); }
            else if (!strcmp(ty, "u16")) { uint16_t x = (uint16_t)val; memcpy(p, &x, 2); }
            else if (!strcmp(ty, "u8")) { uint8_t x = (uint8_t)val; memcpy(p, &x, 1); }
            LOG("[trigger] poke %llx %s %g\n", a, ty, val);
        }
        free(t);
    }
}
static void trigger_capture(uint64_t frame) {
    static long every, until;
    if (!(frame % 30)) {
        char *t = trigger_take("capture");
        if (t) {
            long m = 0;
            if (sscanf(t, "%ld %ld", &every, &m) == 2 && every > 0) { until = (long)frame + m; mkdir("build", 0755); mkdir("build/cap", 0755); }
            LOG("[trigger] capture every %ld frames until %ld\n", every, until);
            free(t);
        }
    }
    if ((long)frame < until && !(frame % every)) {
        extern char g_window_shot_request[256];
        snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/cap/frame_%06llu.png", (unsigned long long)frame);
    }
}

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
    trigger_capture(frame);
    if (test_at && (long)frame == test_at) { warp_other_request(tl, tc); LOG("[warp] test warp queued at frame %llu (request %llx)\n", (unsigned long long)frame, (unsigned long long)g_req); }
}

// Game main thread, once per frame (input.c, the controller read): a queued warp is written into the request and
// the game's request processor (OTHER(WARP_REQUEST_PROC), its object: the request at +0x10) is run on it, as LA's warp
// does.
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
    // (main's data: from around the request processor's vtable up to the sdk)
    for (uint64_t g = OTHER(WARP_REQUEST_PROC_VTABLE) & ~0xfffffull; g < game_sdk_base() && hits < 40; g += 8) {
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

// Start the game's own fast-travel event (event/MapWarp.bfevfl, entry LevelWarpStartMenu) with our level and locator
// as its parameters - the sequence the map screen's warp runs (1.0.2: main+0x256f0c0): load the flow file through the
// resource manager ([WARP_RES_MGR]), the entry, the event's flags, a two-entry parameter list, then hand the event to
// the event system (WARP_EVENT_START with [WARP_EVENT_SYS]). The event's RequestLevelJump does the rest. The addresses
// are the build's own (OTHER: game_addresses/eow_<version>.h).
static void call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    guest_call(c, fn);
}
// Drop one strong reference through a shared-pointer control block, as the game's code does: the use count at +8;
// at zero its vtable's +0x10 (dispose) and then SPTR_WEAK_RELEASE.
static void sptr_release(Cpu *c, uint64_t ctrl) {
    if (!ctrl) return;
    uint64_t old = __atomic_fetch_sub((uint64_t *)(uintptr_t)(ctrl + 8), 1, __ATOMIC_ACQ_REL);
    if (old != 1) return;
    uint64_t vt = *(uint64_t *)(uintptr_t)ctrl;
    c->x[0] = ctrl;
    guest_call(c, *(uint64_t *)(uintptr_t)(vt + 0x10));
    c->x[0] = ctrl;
    guest_call(c, OTHER(SPTR_WEAK_RELEASE));
}

static bool g_kept_flow;
static bool kept_flow_any(void) { return g_kept_flow; }
static bool start_map_warp(Cpu *c, const char *level, const char *locator) {
    static uint64_t m;  // scratch: 0x000 path text, 0x100 path view, 0x110 entry view, 0x120 level view, 0x130 locator
    // view, 0x140 resource out, 0x150 flow out, 0x160 event out, 0x170 params, 0x180 start out, 0x200 level text,
    // 0x280 locator text, 0x300 entry text
    if (!m) m = (uint64_t)(uintptr_t)hle_alloc(0x400);
    memset((void *)(uintptr_t)m, 0, 0x400);
    char *mm = (char *)(uintptr_t)m;
    // (another level: LevelWarpStartMenu; a place in this one: LocationWarpStartMenu. Where the map's warp flow isn't
    // loaded - the overworld has it, a house doesn't: its file comes back 0 - an exit's: AreaExitCommon's WalkOut, with
    // the same two parameters, which every level with a door has)
    static int alt = -1;
    if (alt < 0) alt = getenv("LARECOMP_WARP_EXIT_FLOW") ? 1 : 0;  // (test: the exit's flow first)
    const char *path = alt ? "event/AreaExitCommon.bfevfl" : "event/MapWarp.bfevfl";
    // (LocationWarpStartMenu, the map's entry within a level, didn't start from here: LevelWarpStartMenu for all;
    // LARECOMP_WARP_SAME_LOCATION=1 tries it again)
    const char *entry = alt ? "WalkOut" : strcmp(level, g_loaded) || !getenv("LARECOMP_WARP_SAME_LOCATION") ? "LevelWarpStartMenu" : "LocationWarpStartMenu";
    strcpy(mm, path);
    strcpy(mm + 0x200, level);
    strcpy(mm + 0x280, locator);
    strcpy(mm + 0x300, entry);
    uint64_t *q = (uint64_t *)(uintptr_t)m;
    q[0x100 / 8] = m, q[0x108 / 8] = strlen(path);
    q[0x110 / 8] = m + 0x300, q[0x118 / 8] = strlen(entry);
    q[0x120 / 8] = m + 0x200, q[0x128 / 8] = strlen(level);
    q[0x130 / 8] = m + 0x280, q[0x138 / 8] = strlen(locator);
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    memcpy(saved, c->x, sizeof saved);
    bool ok = false;
    call(c, OTHER(WARP_RES_LOAD), m + 0x140, *(uint64_t *)(uintptr_t)OTHER(WARP_RES_MGR), m + 0x100, 0);  // the flow file
    uint64_t res = q[0x140 / 8];
    static int nlog;
    if (res || nlog++ % 60 == 0) {
        uint64_t mgr = *(uint64_t *)(uintptr_t)OTHER(WARP_RES_MGR);
        LOG("[warp] event file %s: %llx (manager %llx, its +0x110 %llx / %llx)\n", path, (unsigned long long)res, (unsigned long long)mgr,
            mgr ? (unsigned long long)*(uint64_t *)(uintptr_t)(mgr + 0x110) : 0ull, mgr ? (unsigned long long)*(uint64_t *)(uintptr_t)(mgr + 0x118) : 0ull);
    }
    if (!res && !kept_flow_any()) alt = !alt;  // (the other flow next time)
    // (the map's warp flow from the first load is kept - our reference holds it - and reused: after the first warp the
    // resource manager no longer hands the file out)
    static uint64_t kept_flow[2];
    if (!res && kept_flow[0] && !alt) {
        q[0x150 / 8] = kept_flow[0], q[0x158 / 8] = kept_flow[1];
        LOG("[warp] reusing the map-warp flow %llx\n", (unsigned long long)kept_flow[0]);
    }
    if (res || (q[0x150 / 8] && !alt)) {
        if (res) {
            call(c, OTHER(WARP_RES_READY), res, 0, 0, 0);
            call(c, OTHER(WARP_FLOW_OF_RES), m + 0x150, m + 0x140, 0, 0);       // the flow
            if (!alt && !kept_flow[0]) kept_flow[0] = q[0x150 / 8], kept_flow[1] = q[0x158 / 8], g_kept_flow = true;
        }
        call(c, OTHER(WARP_FLOW_ENTRY), m + 0x160, m + 0x150, m + 0x110, 0);  // its entry: the event
        uint64_t ev = q[0x160 / 8];
        LOG("[warp] flow %llx event %llx\n", (unsigned long long)q[0x150 / 8], (unsigned long long)ev);
        if (ev) {
            call(c, OTHER(WARP_EVENT_FLAG_A), ev, 1, 0, 0);
            call(c, OTHER(WARP_EVENT_FLAG_B), ev, 0, 0, 0);
            *(uint64_t *)(uintptr_t)(ev + 0x2f0) = 0xffffffffffffbffeull;
            *(uint32_t *)(uintptr_t)(ev + 0x2f8) = 0x40;
            call(c, OTHER(WARP_PARAMS_NEW), m + 0x170, 2, 0, 0);  // the parameters
            uint64_t prm = q[0x170 / 8];
            call(c, OTHER(WARP_PARAMS_SET), prm, OTHER(WARP_STR_LEVEL), 5, m + 0x120);  // "level"
            call(c, OTHER(WARP_PARAMS_SET), prm, OTHER(WARP_STR_LOCATOR), 7, m + 0x130);  // "locator"
            call(c, OTHER(WARP_EVENT_START), m + 0x180, *(uint64_t *)(uintptr_t)OTHER(WARP_EVENT_SYS), m + 0x160, m + 0x170);
            LOG("[warp] map-warp event started (%llx)\n", (unsigned long long)q[0x180 / 8]);
            ok = true;
        }
        // (our references, as the map screen drops its own: the start's result, the event, the flow - shared
        // pointers {object, control}; the file's handle at +0x140 is a plain pointer)
        sptr_release(c, q[0x188 / 8]);
        if (getenv("LARECOMP_WARP_RELEASE_ALL")) { sptr_release(c, q[0x168 / 8]); sptr_release(c, q[0x158 / 8]); }
    }
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    return ok;
}

void warp_other_game_tick(Cpu *c) {
    trigger_flags();
    trigger_teleport();
    trigger_hp();
    if (g_pending && !getenv("LARECOMP_WARP_REQUEST_ONLY")) {  // the game's own fast-travel event
        pthread_mutex_lock(&g_lock);
        char lv[64], lc[64];
        snprintf(lv, sizeof lv, "%s", g_level);
        snprintf(lc, sizeof lc, "%s", g_locator);
        g_pending = 0;
        pthread_mutex_unlock(&g_lock);
        static int tries;
        if (!tries) LOG("[warp] map-warp event to %s / %s\n", lv, lc);
        if (!start_map_warp(c, lv, lc)) {  // (the flow file not loaded yet - the first ask starts its load: again next frame)
            if (++tries < 600) { pthread_mutex_lock(&g_lock); if (!g_pending) g_pending = 1; pthread_mutex_unlock(&g_lock); }
            else { LOG("[warp] gave up: the event never started\n"); tries = 0; }
            return;
        }
        if (tries) LOG("[warp] started after %d tries\n", tries);
        tries = 0;
        return;
    }
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
        *(uint64_t *)(uintptr_t)P = OTHER(WARP_REQUEST_PROC_VTABLE);
        *(uint64_t *)(uintptr_t)(P + 8) = O;
        *(uint64_t *)(uintptr_t)(P + 0x10) = R2;
        *(uint64_t *)(uintptr_t)(P + 0x20) = g_ctx;
        uint64_t saved[31], sp = c->sp, pc = c->pc;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = P;
        LOG("[warp] calling the request processor on job %llx (owner %llx, context %llx)\n", (unsigned long long)P,
            (unsigned long long)O, (unsigned long long)g_ctx);
        guest_call(c, OTHER(WARP_REQUEST_PROC));
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
    for (long i = 0; i < n; i++) {
        if (!name_char((unsigned char)b[i]) || (i && b[i - 1])) continue;  // (strings start after a NUL)
        long j = i;
        while (j < n && name_char((unsigned char)b[j])) j++;
        if (j >= n || b[j] || j - i < 4 || j - i >= 64) { i = j; continue; }
        const char *s = b + i;
        bool ok = false;
        const char *in = level;
        if (!strncmp(s, level, ll) && digits_after(s, ll)) ok = !field;
        else if (!field && !strncmp(s, "Field_", 6)) {  // (another level's exit to the overworld names a real spawn point
            const char *u = strrchr(s, '_');           // there: Field_HyrulePlains_009 - the overworld's own strings hold
            ok = u > s + 6 && isdigit((unsigned char)u[1]) && strchr(s + 6, '_') == u;  // other things of that shape)
            in = "Field";
        }
        if (ok) {
            bool dup = false;
            for (int k = 0; k < g_nplaces && !dup; k++) dup = !strcmp(g_places[k].level, in) && !strcmp(g_places[k].locator, s);
            if (!dup) {
                g_places = realloc(g_places, sizeof *g_places * (size_t)(g_nplaces + 1));
                snprintf(g_places[g_nplaces].level, 64, "%s", in);
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
        if (getenv("LARECOMP_LOG_PLACES"))
            for (int i = 0; i < g_nplaces; i++) LOG("[place] %d %s %s\n", i, g_places[i].level, g_places[i].locator);
    }
    if (out) *out = g_places;
    return g_nplaces;
}
const char *warp_other_place_level(int i) { return i >= 0 && i < g_nplaces ? g_places[i].level : ""; }
const char *warp_other_place_locator(int i) { return i >= 0 && i < g_nplaces ? g_places[i].locator : ""; }

// ---- level loads (fs.c: the first read of each romfs file)
// in play: a level registered its jump request (none on the title and the file select; --load-save, load_save.c)
int warp_other_in_play(void) { return g_req != 0; }
// The current level: where the last level jump went, else the last level file read
const char *warp_other_current_level(void) { return g_current[0] ? g_current : g_loaded; }
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
// starts again after it). LARECOMP_WARP_TOUR_WALK=1: Link walks a loop there too (right, down, left, up and back, 12 s
// in all, from 1 s after the load) so the camera scrolls - what only shows while it moves.
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void tour_frame(uint64_t frame) {
    static int on = -1, cur, to, state;  // state: 0 idle, 1 warped (waiting for the load), 2 settling
    static long start_frame;
    static uint64_t loads_at, runs_at;
    static bool same_level;
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
        same_level = !strcmp(g_places[cur].level, g_loaded);
        warp_other_request(g_places[cur].level, g_places[cur].locator);
        loads_at = g_loads, runs_at = g_proc_runs, t0 = t, state = 1;
    } else if (state == 1) {
        if ((g_loads != loads_at && !strcmp(g_loaded, g_places[cur].level)) || (same_level && g_proc_runs != runs_at)) {
            t_load = t - t0, state = 2, worst = sum = 0, nframes = slow = 0;
        } else if (t - t0 > 30) {
            LOG("[tour] result %d %s %s: no load in 30 s (last level read: %s)\n", cur, g_places[cur].level,
                g_places[cur].locator, g_loaded);
            // (some places forbid warping out - a battle arena: the driver starts again from the save, at the next one)
            LOG("[tour] restart after %d\n", cur);
            fflush(stderr);
            _exit(3);
        }
    }
    static int walk = -1;
    if (walk < 0) walk = getenv("LARECOMP_WARP_TOUR_WALK") != NULL;
    if (walk && state == 2) {  // (the loop: seconds after the load -> stick direction)
        extern volatile int32_t g_tour_lx, g_tour_ly;
        static const struct { double until; int dx, dy; } loop[] = {
            {1, 0, 0}, {2.5, 1, 0}, {4, 0, -1}, {7, -1, 0}, {10, 0, 1}, {11.5, 1, 0}, {13, 0, -1}};
        double w = t - t0 - t_load;
        int dx = 0, dy = 0;
        for (size_t i = 0; i < sizeof loop / sizeof loop[0]; i++) if (w < loop[i].until) { dx = loop[i].dx, dy = loop[i].dy; break; }
        g_tour_lx = dx * 26000, g_tour_ly = dy * 26000;
    } else if (walk) {
        extern volatile int32_t g_tour_lx, g_tour_ly;
        g_tour_lx = g_tour_ly = 0;
    }
    if (0) {
    } else if (state == 2 && t - t0 - t_load > (walk ? 14.0 : 6.0)) {
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
