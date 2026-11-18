// SPDX-License-Identifier: GPL-3.0-or-later
// The game's menu screens as code mods see them (coop_mod_api.h version 2, docs/mods.md "Menu screens"): which screen
// an update is (its class's vtable, read from main) and its focused button's name. Read with a reader the caller gives
// (the game's memory in the module, a fake in the tests). No game calls here: the module makes the one call
// (kButtonDecide) on the screen's update (coop_mods_runtime.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace CoopModScreens {

// main offsets (build e0f3ccfa, main at 0x80020000), read from main: each class's name getter, then its factory's
// constructor (the same three as patch 0028's hos_skip_title.h).
inline constexpr std::uint64_t kTitleVtable = 0x4bf06e8;         // UITitleScreen
inline constexpr std::uint64_t kLoadListVtable = 0x4beeba0;      // UISystemLoadListScreen
inline constexpr std::uint64_t kLoadQuestionVtable = 0x4bef840;  // UISystemSaveLoadScreen
// Button::Decide(button, 0) -> bit 0: chosen. The call the game's screens make for a button chosen without its own
// input (main+0x3405280, +0x3405930); patch 0028's Skip title screen chooses with it too.
inline constexpr std::uint64_t kButtonDecide = 0x3771db0;

// A screen's buttons, as the game's screen walks them (patch 0028, host/oot_autoload.c focused_button): the focus
// manager at *(screen+32)+96 (set while the screen takes input), the list head at +136, its first node at +144, an
// entry's offset from its node at +156, the entry's button at +24; a button's state at +56 (0 not focused, 4 and 5
// decided).
inline constexpr std::uint64_t kScreenFocus = 32, kFocusManager = 96;
inline constexpr std::uint64_t kListHead = 136, kListFirst = 144, kListEntryOffset = 156, kEntryButton = 24;
inline constexpr std::uint64_t kButtonState = 56;
// A button's pane name, as the game's own screen request reads it (main+0x3379160): its length at +0x18, its text at
// *(button+0x20), or at *(*(button+0x20)+0x30) when bit 5 of +0x41 is set.
inline constexpr std::uint64_t kNameLength = 0x18, kNameText = 0x20, kNameFlags = 0x41, kNameIndirect = 0x30;
inline constexpr std::size_t kNameMax = 63;
inline constexpr int kMaxButtons = 64;

// Reads n bytes at a guest address; false when not mapped.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t n)>;

// COOP_MOD_SCREEN_* of a screen whose vtable is at main+vtable_offset.
std::uint32_t KindOf(std::uint64_t vtable_offset);

// The screen's one focused button (0: none, more than one, or the screen takes no input now).
std::uint64_t FocusedButton(const Reader& read, std::uint64_t screen);

// A button's pane name ("" when it cannot be read or is not printable).
std::string ButtonName(const Reader& read, std::uint64_t button);

}  // namespace CoopModScreens
