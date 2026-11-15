// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_map_refresh.h"

#include <cstdio>
#include <utility>

namespace CoopMapRefresh {

void Refresher::Want(std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    due_ms_ = now_ms + kDelayMs;  // a later arrival moves it: one refresh after the last
}

void Refresher::SetScreen(std::uint64_t screen, std::uint64_t main) {
    std::lock_guard lock(mutex_);
    screen_ = screen;
    main_ = main;
}

std::vector<std::string> Refresher::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::string Refresher::Copy(CoopPuppet::Guest& guest, std::uint64_t screen) {
    std::uint8_t floors = 0;
    std::uint8_t held[4] = {};
    if (!guest.Read(screen + addr::kFloors, &floors, 1) || !guest.Read(screen + addr::kHeld, held, sizeof held)) {
        return "not readable";
    }
    char line[96];
    std::snprintf(line, sizeof line, "floors 0x%02x, map %u compass %u boss key %u", floors, held[0], held[1],
                  held[2]);
    return line;
}

std::optional<CoopPuppet::Call> Refresher::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t now_ms) {
    if (after_call) {
        if (called_on_ == 0) return std::nullopt;
        const std::uint64_t screen = std::exchange(called_on_, 0);
        const std::string after = Copy(guest, screen);
        std::lock_guard lock(mutex_);
        notes_.push_back("map refresh: another player's map or floors: the HUD screen's copy " + before_ + " -> " +
                         after);
        return std::nullopt;
    }
    std::uint64_t screen = 0, main = 0;
    {
        std::lock_guard lock(mutex_);
        if (due_ms_ == 0 || now_ms < due_ms_ || screen_ == 0 || main_ == 0) return std::nullopt;
        due_ms_ = 0;
        screen = screen_;
        main = main_;
    }
    before_ = Copy(guest, screen);
    called_on_ = screen;
    CoopPuppet::Call c;
    c.function = main + addr::kRefresh;
    c.x[0] = screen;
    return c;
}

Refresher& TheRefresher() {
    static Refresher refresher;
    return refresher;
}

}  // namespace CoopMapRefresh
