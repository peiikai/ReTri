#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The randomizer: off until EoW has one of its own (LA's was removed). RANDO_ON 0 hides the start screen's RANDOMIZER
// row and the menu's Randomizer page, and every boot is vanilla; the seed plumbing below stays for the EoW randomizer.
#define RANDO_ON 0

void rando_init(void);
const char *rando_seed_dir(void);  // "rando/<seed>" when a seed is active, else NULL
bool rando_romfs_override(const char *path, char *out, size_t cap);
void rando_boot(void);               // each boot after the modules load: (re)read the chosen seed
void rando_select(const char *seed); // seed for the next boot ("" = vanilla), saved; then rt_request_reboot
bool rando_seed_there(const char *seed);  // rando/<seed> holds a finished seed (settings.json and romfs/)
