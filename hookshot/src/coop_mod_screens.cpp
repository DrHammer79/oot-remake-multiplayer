// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_mod_screens.h"

#include "coop_mod_api.h"

namespace CoopModScreens {

namespace {

// Addresses below 1 MB (a null pointer's page) read as 0, as patch 0028's peek.
template <class T>
T Peek(const Reader& read, std::uint64_t a) {
    T v{};
    if (a < 0x100000 || !read(a, &v, sizeof v)) {
        return T{};
    }
    return v;
}

}  // namespace

std::uint32_t KindOf(std::uint64_t vtable_offset) {
    switch (vtable_offset) {
    case kTitleVtable: return COOP_MOD_SCREEN_TITLE;
    case kLoadListVtable: return COOP_MOD_SCREEN_LOAD_LIST;
    case kLoadQuestionVtable: return COOP_MOD_SCREEN_LOAD_QUESTION;
    default: return COOP_MOD_SCREEN_OTHER;
    }
}

std::uint64_t FocusedButton(const Reader& read, std::uint64_t screen) {
    if (Peek<std::uint64_t>(read, Peek<std::uint64_t>(read, screen + kScreenFocus) + kFocusManager) == 0) {
        return 0;
    }
    const std::uint64_t head = screen + kListHead;
    const auto off = static_cast<std::int64_t>(Peek<std::int32_t>(read, screen + kListEntryOffset));
    std::uint64_t node = Peek<std::uint64_t>(read, screen + kListFirst), found = 0;
    for (int i = 0; i < kMaxButtons && node != 0 && node != head; ++i) {
        const std::uint64_t button = Peek<std::uint64_t>(read, node - static_cast<std::uint64_t>(off) + kEntryButton);
        const std::uint8_t state = button != 0 ? Peek<std::uint8_t>(read, button + kButtonState) : 0;
        if (state != 0 && (state & 0xfe) != 4) {
            if (found != 0) {
                return 0;
            }
            found = button;
        }
        node = Peek<std::uint64_t>(read, node + 8);
    }
    return found;
}

std::string ButtonName(const Reader& read, std::uint64_t button) {
    if (button == 0) {
        return {};
    }
    std::uint64_t n = Peek<std::uint64_t>(read, button + kNameLength);
    const std::uint64_t at = (Peek<std::uint8_t>(read, button + kNameFlags) & 0x20) != 0
                                 ? Peek<std::uint64_t>(read, Peek<std::uint64_t>(read, button + kNameText) + kNameIndirect)
                                 : Peek<std::uint64_t>(read, button + kNameText);
    if (n == 0 || at < 0x100000) {
        return {};
    }
    if (n > kNameMax) {
        n = kNameMax;
    }
    std::string name(static_cast<std::size_t>(n), '\0');
    if (!read(at, name.data(), name.size())) {
        return {};
    }
    for (const char c : name) {
        if (c < 0x20 || c > 0x7e) {
            return {};
        }
    }
    return name;
}

}  // namespace CoopModScreens
