// The ExeFS adapter's OS part (platform_os.cpp) beyond the coop_plat_* slot functions (coop_platform_static.h).
#pragma once

#include <stdint.h>

extern "C" {
// Once from the module's _init, before anything else runs: the locks, the arena, the tick frequency.
void coop_plat_os_init();
// The arena's numbers: capacity, used, peak, largest free block, refused + bad frees.
void coop_plat_arena_stats(uint64_t out[5]);
}
