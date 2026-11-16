// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_name_privacy.h"

#include <array>
#include <coop/os.hpp>
#include <mutex>
#include <utility>

namespace CoopNamePrivacy {
namespace {

// Words of the game's world, 9 letters at most (with "-NNNN": 14 of the 16 a name may have).
constexpr std::array<std::string_view, 24> kWords{
    "Kokiri", "Goron",   "Zora",    "Deku",    "Hylian",  "Sheikah", "Gerudo",  "Fairy",
    "Cucco",  "Keese",   "Octorok", "Tektite", "Skulltula", "Epona", "Navi",    "Ocarina",
    "Hookshot", "Bombchu", "Rupee",  "Triforce", "Malon", "Kakariko", "Dodongo", "Biggoron",
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool SameIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}

// The first 16 code points (the old default cut a longer user name so).
std::string_view First16(std::string_view s) {
    std::size_t points = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++points > 16) return s.substr(0, i);
    }
    return s;
}

coop::os::Mutex g_mutex;
std::vector<std::string> g_lines;

}  // namespace

std::string RandomName(std::uint32_t r) {
    const auto word = kWords[r % kWords.size()];
    const auto digits = static_cast<unsigned>(1000 + (r / kWords.size()) % 9000);
    return std::string(word) + "-" + std::to_string(digits);
}

std::string NewName() {
    std::uint32_t r = 0;
    coop::os::RandomFill(&r, sizeof r);   // without a random source still filled (clock, thread): fine for a name
    return RandomName(r);
}

Source Matches(std::string_view name, std::string_view user, std::string_view pc) {
    name = Trim(name);
    user = Trim(user);
    pc = Trim(pc);
    if (name.empty()) return Source::none;
    if (!user.empty() && (SameIgnoringCase(name, user) || SameIgnoringCase(name, Trim(First16(user))))) {
        return Source::user;
    }
    if (!pc.empty() && (SameIgnoringCase(name, pc) || SameIgnoringCase(name, Trim(First16(pc))))) {
        return Source::pc;
    }
    return Source::none;
}

Action Decide(std::string_view saved, bool checked, Source match) {
    if (Trim(saved).empty()) return Action::fill;
    if (checked || match == Source::none) return Action::keep;
    return Action::replace;
}

std::string Notice(Source was, std::string_view now, bool windows) {
    const char* what = was == Source::pc ? "PC's name" : windows ? "Windows user name" : "user name";
    return std::string("Your co-op name was your ") + what + "; it is now " + std::string(now) +
           ". Change it in the Multiplayer tab.";
}

void Say(std::string line) {
    if (line.empty()) return;
    std::lock_guard lock(g_mutex);
    g_lines.push_back(std::move(line));
}

std::vector<std::string> TakeLines() {
    std::lock_guard lock(g_mutex);
    return std::exchange(g_lines, {});
}

}  // namespace CoopNamePrivacy
