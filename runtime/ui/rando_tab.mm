// Randomizer page: its own panel on the start screen (the RANDOMIZER row: launcher.mm) and the in-game menu bar's
// Randomizer (menu.mm). Empty, and both entries hidden (RANDO_ON, runtime/rando.h): waiting for the EoW randomizer,
// which will choose and make its seeds here (rando/<seed>/: runtime/rando.c).
#include "imgui.h"
#include "i18n.h"

extern "C" bool rando_tab_took_back(void) { return false; }     // (B went back a step inside the page: none yet)
extern "C" bool rando_tab_close_pressed(void) { return false; }  // (touch screens: the page's own X: none yet)

extern "C" void rando_tab_draw(float scale) {
    (void)scale;
    ImGui::TextDisabled("%s", L("The randomizer is coming in a later version."));
}
