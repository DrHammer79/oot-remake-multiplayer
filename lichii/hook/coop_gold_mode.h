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
#include <optional>
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
// a new connection plays Shared until told; the first time this game hosts in a connection
// that nobody told a mode, it takes its own choice; a host's MOD_SETTINGS set it.
void OnConnection();
void OnHosting();
void OnHostSettings(const std::vector<coop::protocol::ModSetting>& settings);

// The mode in force. Any thread, cheap (one atomic load).
Mode InForce();
inline bool NotShared() { return InForce() == Mode::own; }

// Test only: back to a fresh start (Shared, choice Shared).
void ResetForTest();

}  // namespace CoopGoldMode
