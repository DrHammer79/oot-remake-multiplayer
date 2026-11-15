// SPDX-License-Identifier: GPL-3.0-or-later
// The Longshot/Hookshot module as a platform adapter (docs/platform.md, v1 step 5 'hookshot-on-platform'): the
// CoopPlatform table (coop_platform.h) filled from Hookshot's HosCoopApi (hos_coop_api.h). The game slots (memory,
// calls, scratch, pad word, keys, log) forward to Hookshot's table; the OS slots (sockets, threads, locks, time,
// memory, files) are the std backend's (coop::os::StdPlatform), with files relative to the game's folder.
// The compat runtime (hookshot/compat/coop_runtime.h) answers the emulator's API slice from this table.
#pragma once
#include <cstdint>
#include <filesystem>

#include "coop_platform.h"
#include "hos_coop_api.h"

namespace CoopHsPlatform {
// Fills the module's one table from Hookshot's (null when api is). A later slot of HosCoopApi that an older Hookshot
// does not have (size) or left NULL stays NULL in the table: the core then does without, as before.
const CoopPlatform* Make(const HosCoopApi* api);
// The table Make filled, or null before it / after Clear (co-op off).
const CoopPlatform* Get();
// Co-op off: Get answers null again (CoopRuntime::Running() false).
void Clear();
// The game's folder as Hookshot gave it (UTF-8), "." without one: the table's file root.
std::filesystem::path GameDir();
}  // namespace CoopHsPlatform
