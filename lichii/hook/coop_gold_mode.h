// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gold Skulltulas, a host setting (docs/world.md "Gold Skulltulas: shared or
// not"; sync-coverage row 63):
//
// - Shared (the default): a Gold Skulltula is a shared enemy. Its token drops
//   in every game (CoopWorld::WorldSync makes it in a game whose own copy
//   died with no token), one pickup counts once for everyone, and the
//   placement keys, the count and IsGet go through the flag sync.
// - Not shared (race mode): each game keeps its own Skulltulas, tokens and
//   count. Nothing about them crosses the wire: the enemy is not on the
//   objects sync, a token's taking is no WORLD_BREAK, and the flag watcher
//   keeps the keys, the count and IsGet in this game (KeepLocal).
//
// The host's choice is saved in coop-options.ini (`gold_skulltulas = own`;
// no line: shared). It is read when this game starts hosting: changing it in
// a session takes effect at the next Host. The mode goes to the guests in the
// host's MOD_SETTINGS (491) as mod id "coop", key "gold_skulltula", value
// "shared" or "own" (docs/protocol.md "Mods"); the server keeps it for late
// joiners, and a host after a migration sends the mode in force, not its own
// choice. A guest that hears no value (a host from before this) plays Shared.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopGoldMode {

enum class Mode : std::uint8_t { shared, own };

inline constexpr std::string_view kModId = "coop";
inline constexpr std::string_view kKey = "gold_skulltula";

std::string_view Value(Mode mode);                         // "shared", "own"
std::optional<Mode> Parse(std::string_view value);         // nullopt: not one of them
// The mode a list of shared settings carries (the coop entry), nullopt without one.
std::optional<Mode> FromSettings(const std::vector<coop::protocol::ModSetting>& settings);
coop::protocol::ModSetting AsSetting(Mode mode);

// This player's own choice for its next Host (the menu, coop-options.ini). Any thread.
void SetChoice(Mode mode);
Mode Choice();

// The session's mode, as the connection events give it (CoopModShare, the network thread):
// a new connection (and a new session, CoopHook's Begin, before its client starts) plays Shared
// until told; the first time this game hosts in a connection
// that nobody told a mode, it takes its own choice; a host's MOD_SETTINGS set it.
void OnConnection();
void OnHosting();
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);

// The mode in force. Any thread, cheap (one atomic load).
Mode InForce();
inline bool NotShared() { return InForce() == Mode::own; }
// The connection has been told a mode (a host's settings, or our own Host).
bool Told();
// The hook keeps the Gold Skulltula flags in this game (FlagWatcher::keep_local, no way back)
// only once the session's mode is known to be Not shared: never in the frames before the
// welcome, where a mode left from an earlier session could still be in force.
inline bool KeepLocalNow() { return Told() && NotShared(); }

// Not shared, a guest: the host's save (at join, and at Resync from host) is written with this game's own Gold
// Skulltulas (review of f36a8852): `host` is the host's progress.sav, changed in place; `own` this game's last
// progress.sav (its co-op slot, else its own save; nullptr: none, a race from nothing). Each placement key in
// `skulltula_keys` and key + 2 take `own`'s state in the save's 64-bit key set, and the Int `count_hash` and the
// Bool `get_hash` take `own`'s value (0 without one). The save layout: tools/autotest/save_flags.py. A host's save
// that does not read as one is left as it is.
struct GoldKept {
    bool done = false;        // the host's save read and changed as said
    int host_keys = 0;        // gold keys the host's save had
    int own_keys = 0;         // gold keys it has now (ours)
    std::int32_t host_count = 0;
    std::int32_t own_count = 0;
    bool keys_left = false;   // ours could not be added: the host's save has no key set
    std::string why;          // not done: why
};
GoldKept KeepOwnGold(std::vector<std::uint8_t>& host, const std::vector<std::uint8_t>* own,
                     std::span<const std::uint64_t> skulltula_keys, std::uint32_t count_hash,
                     std::uint32_t get_hash);

// Not shared: this game's race so far, the progress.sav of the newest slot in the co-op folder `coop_dir`. Never the
// player's own (single-player) saves: every player starts the race from 0 tokens (decision of 2026-11-23). None:
// nothing, and KeepOwnGold starts the race from nothing.
std::optional<std::vector<std::uint8_t>> RaceSoFar(const std::filesystem::path& coop_dir);

// Test only: back to a fresh start (Shared, choice Shared).
void ResetForTest();

}  // namespace CoopGoldMode
