// The game's own UI textures (item icons...) for our overlays, read from the player's own game files at runtime: nothing
// of the game ships.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

extern "C" {
uint8_t *texpatch_ui_rgba(const char *name, int *w, int *h);  // runtime/nvn/texpatch.c
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}

static ImTextureID upload(const std::vector<uint8_t> &rgba, int w, int h) {
#ifdef __APPLE__
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    if (!dev) return (ImTextureID)0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4];
    return (ImTextureID)(__bridge_retained void *)t;  // (kept for the session)
#else
    return (ImTextureID)vk_imgui_texture_rgba(rgba.data(), w, h);
#endif
}

// An RGBA8 image made here (straight alpha), uploaded and kept for the session (the touch controls' EoW fill)
ImTextureID ui_rgba_texture(const uint8_t *rgba, int w, int h) {
    std::vector<uint8_t> v(rgba, rgba + (size_t)w * h * 4);
    return upload(v, w, h);
}

// One of the game's own UI textures by name (Game.arc: item icons...), uploaded once and kept; 0 when it isn't there.
// The touch controls draw the items on their buttons with these (runtime/ui/mobile/touch_controls.mm).
ImTextureID ui_game_icon(const char *name) {
    static std::map<std::string, ImTextureID> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    int w = 0, h = 0;
    uint8_t *src = texpatch_ui_rgba(name, &w, &h);
    ImTextureID id = (ImTextureID)0;
    if (src && w > 0 && h > 0) {
        std::vector<uint8_t> v(src, src + (size_t)w * h * 4);
        id = upload(v, w, h);
    }
    free(src);
    cache.emplace(name, id);
    return id;
}

