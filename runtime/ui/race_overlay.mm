// Speedrun overlay, drawn by menu.mm under the menu: the run timer (General tab "Timer"; runtime/hle/run_timer.c) in
// the bottom-left corner while Link is in play - the game's HUD keeps the top (hearts, item buttons) and the bottom
// right (rupees) - on a small see-through parchment panel, gold once the run is finished
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "../settings.h"
int game_in_play(void);
extern int g_start_screen;
int run_timer_state(uint64_t *us, bool *counting);  // runtime/hle/run_timer.c
bool inventory_items_page_shown(void);              // pause_pages.mm
}

extern "C" bool race_overlay_wanted(void) { return g_settings.timer && game_in_play(); }

// The timer's panel: game-styled parchment (dark brown frame, parchment, gold rim, brown ink), see-through so the game shows under it
static void timer_panel(ImDrawList *dl, ImVec2 a, ImVec2 z, float u, bool done) {
    float r = 10 * u;
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + 3 * u), ImVec2(z.x + 2 * u, z.y + 3 * u), IM_COL32(0, 0, 0, 45), r);
    dl->AddRectFilled(ImVec2(a.x - 2 * u, a.y - 2 * u), ImVec2(z.x + 2 * u, z.y + 2 * u), IM_COL32(0x4A, 0x30, 0x20, 150), r + 2 * u);
    dl->AddRectFilled(a, z, done ? IM_COL32(0xF6, 0xE2, 0xA8, 165) : IM_COL32(0xF3, 0xE6, 0xC6, 140), r);
    dl->AddRect(a, z, done ? IM_COL32(0xE8, 0xB8, 0x4A, 255) : IM_COL32(0xC8, 0x9B, 0x3C, 200), r, 0, (done ? 3.0f : 2.0f) * u);
}

// The run timer's text (tenths) and state (2: finished); false when it isn't shown
static bool timer_text(char *t, size_t cap, int *st) {
    if (!(g_settings.timer && game_in_play() && !inventory_items_page_shown())) return false;  // (not over the Items page: it still counts)
    uint64_t us = 0;
    bool counting = false;
    *st = run_timer_state(&us, &counting);
    uint64_t ds = us / 100000;  // tenths
    if (ds >= 36000) snprintf(t, cap, "%llu:%02llu:%02llu.%llu", (unsigned long long)(ds / 36000),
                              (unsigned long long)(ds / 600 % 60), (unsigned long long)(ds / 10 % 60), (unsigned long long)(ds % 10));
    else snprintf(t, cap, "%llu:%02llu.%llu", (unsigned long long)(ds / 600), (unsigned long long)(ds / 10 % 60),
                  (unsigned long long)(ds % 10));
    return true;
}

// (menu.mm's overlay reuse) what race_overlay_draw would draw now, as text: the same key = the same drawing at the
// same display size
extern "C" bool race_overlay_key(char *out, size_t cap) {
    char t[32];
    int st = 0;
    if (timer_text(t, sizeof t, &st)) snprintf(out, cap, "timer %s %d %d", t, st == 2, (int)g_settings.timer_pos);
    else snprintf(out, cap, "-");
    return true;
}

extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    char t[32];
    int st = 0;
    if (timer_text(t, sizeof t, &st)) {
        float size = 62 * scale;
        // (digits at a fixed width: the panel and the time don't wobble as it counts)
        float dw = font->CalcTextSizeA(size, FLT_MAX, 0, "0").x, tw = 0;
        for (const char *c = t; *c; c++) {
            char one[2] = {*c, 0};
            tw += *c >= '0' && *c <= '9' ? dw : font->CalcTextSizeA(size, FLT_MAX, 0, one).x;
        }
        // Baloo's line box is tall: the panel hugs the digits (cap height ~ 0.5 of the size, from ~0.3 down)
        float padx = 20 * scale, pady = 10 * scale, top = size * 0.24f, bot = size * 0.80f;
        float pw = tw + 2 * padx, ph = (bot - top) + 2 * pady;
        // bottom left, or bottom right. The game's HUD sits in a 16:9 box centred in the window (wider windows: the
        // corners are free): hearts top left, item buttons top right, and bottom right the "Now saving." note (about
        // 0.79-1.0 of the box's width, 0.86-0.94 of its height; found 2026-10-20) - at the right, the panel goes above
        // that note when they'd meet.
        float ax = g_settings.timer_pos == TIMER_POS_RIGHT ? display.x - 30 * scale - pw : 30 * scale;
        ImVec2 a(ax, display.y - 30 * scale - ph), z(a.x + pw, a.y + ph);
        if (g_settings.timer_pos == TIMER_POS_RIGHT) {
            float bw = fminf(display.x, display.y * 16 / 9), bx = (display.x - bw) * 0.5f;
            float sx0 = bx + 0.79f * bw, sx1 = bx + bw, sy0 = 0.86f * display.y, sy1 = 0.94f * display.y;
            if (z.x > sx0 && a.x < sx1 && z.y > sy0 && a.y < sy1) a.y = sy0 - 10 * scale - ph, z.y = a.y + ph;
        }
        timer_panel(dl, a, z, scale, st == 2);
        ImU32 ink = st == 2 ? IM_COL32(0x8A, 0x55, 0x00, 255) : IM_COL32(0x3B, 0x2A, 0x14, 255);
        float x = a.x + padx, y = a.y + pady - top;
        for (const char *c = t; *c; c++) {
            char one[2] = {*c, 0};
            float gw = font->CalcTextSizeA(size, FLT_MAX, 0, one).x, cw = *c >= '0' && *c <= '9' ? dw : gw;
            ImVec2 p(roundf(x + (cw - gw) * 0.5f), roundf(y));
            dl->AddText(font, size, ImVec2(p.x, p.y + 1.2f * scale), IM_COL32(255, 248, 228, 150), one);  // (a light emboss)
            dl->AddText(font, size, p, ink, one);
            x += cw;
        }
    }
}
