// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_gold_mode.h"

#include <atomic>

namespace CoopGoldMode {

namespace proto = coop::protocol;

namespace {
std::atomic<Mode> g_choice{Mode::shared};
std::atomic<Mode> g_in_force{Mode::shared};
std::atomic<bool> g_told{false};  // this connection has a mode (a host's settings, or our own Host)
}  // namespace

std::string_view Value(Mode mode) {
    return mode == Mode::own ? "own" : "shared";
}

std::optional<Mode> Parse(std::string_view value) {
    if (value == "shared") return Mode::shared;
    if (value == "own") return Mode::own;
    return std::nullopt;
}

std::optional<Mode> FromSettings(const std::vector<proto::ModSetting>& settings) {
    for (const proto::ModSetting& s : settings) {
        if (s.mod_id == kModId && s.key == kKey) return Parse(s.value);
    }
    return std::nullopt;
}

proto::ModSetting AsSetting(Mode mode) {
    return proto::ModSetting{std::string(kModId), std::string(kKey), std::string(Value(mode))};
}

void SetChoice(Mode mode) {
    g_choice.store(mode);
}

Mode Choice() {
    return g_choice.load();
}

void OnConnection() {
    g_told.store(false);
    g_in_force.store(Mode::shared);
}

void OnHosting() {
    // A host after a migration keeps the mode its old host told it.
    if (!g_told.exchange(true)) g_in_force.store(g_choice.load());
}

void OnHostSettings(const std::vector<proto::ModSetting>& settings) {
    if (const auto mode = FromSettings(settings)) {
        g_in_force.store(*mode);
        g_told.store(true);
    }
}

Mode InForce() {
    return g_in_force.load(std::memory_order_relaxed);
}

void ResetForTest() {
    g_choice.store(Mode::shared);
    g_in_force.store(Mode::shared);
    g_told.store(false);
}

}  // namespace CoopGoldMode
