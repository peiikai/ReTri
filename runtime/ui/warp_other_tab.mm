// Warp tab for another game on the engine (experimental: Echoes of Wisdom). The places are the game's own levels and
// their spawn points, read from its level files (runtime/hle/warp_other.c); each level opens like a folder onto its
// spawn points, and picking one queues the jump (handed to the game once a frame).
#include <stdint.h>
#include <string.h>

#include "imgui.h"

extern "C" {
int warp_other_places(const void **out);
const char *warp_other_place_level(int i);
const char *warp_other_place_locator(int i);
const char *warp_other_current_level(void);
void warp_other_request(const char *level, const char *locator);
}

// true when a warp was queued (the menu closes)
extern "C" bool warp_other_tab_draw(float scale) {
    (void)scale;
    int n = warp_other_places(nullptr);
    const char *here = warp_other_current_level();
    ImGui::TextDisabled("Experimental. Now in: %s", here[0] ? here : "-");
    if (!n) {
        ImGui::TextDisabled("No levels found in the game files.");
        return false;
    }
    bool go = false;
    for (int i = 0; i < n;) {
        const char *level = warp_other_place_level(i);
        int j = i;
        while (j < n && !strcmp(warp_other_place_level(j), level)) j++;
        ImGui::PushID(level);
        if (ImGui::TreeNodeEx(level, !strcmp(level, here) ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s  (%d)", level, j - i)) {
            for (int k = i; k < j; k++) {
                if (ImGui::Selectable(warp_other_place_locator(k))) {
                    warp_other_request(level, warp_other_place_locator(k));
                    go = true;
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        i = j;
    }
    return go;
}
