// SPDX-License-Identifier: GPL-3.0-or-later
// SharedFlags from a file named by a path (the emulator's LICHII_COOP_FLAGS, tests). Apart from flags.hpp so the
// core, which reaches flags.hpp, reads no files by path (v1 step 4, os-wrap: the core's files go through coop::os).
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "coop/host/flags.hpp"

namespace coop::host {

// SharedFlags::parse of the whole file; nothing (and `error`, when given, says why) when it cannot be read or a line
// is bad.
std::optional<SharedFlags> load_shared_flags(const std::filesystem::path& path, std::string* error = nullptr);

}  // namespace coop::host
