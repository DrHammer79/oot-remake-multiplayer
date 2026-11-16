// Co-op settings for the emulator: server address, port, player name, join
// password and colour.
// From coop.ini in the emulator's config folder, then environment variables,
// which win. Co-op stays off unless a server is set. No emulator code here.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopHook {

struct Settings {
    std::string server;            // IP or host name; empty means co-op off
    std::uint16_t port = 27510;    // protocol default
    std::string name = "Player";   // nickname, 1 to 16 characters
    std::string password;          // join password; empty: none
    // The player's colour, 0xRRGGBB (coop_colors.h); unset: the others give
    // this player its slot's colour.
    std::optional<std::uint32_t> color;
    // The player's own key (the Lobby, docs/protocol.md "Lobby"): from
    // player-key.txt beside coop.ini (LoadPlayerKey); sent sealed in JOIN_PROOF so the
    // host's mutes and bans follow this player, not the name.
    std::optional<std::array<std::uint8_t, 16>> player_key;
    bool enabled() const { return !server.empty(); }
};

// The key's text: 32 hex digits (any case, spaces and line ends around them
// skipped). Nothing for anything else.
std::optional<std::array<std::uint8_t, 16>> ParsePlayerKey(std::string_view text);
std::string PlayerKeyText(const std::array<std::uint8_t, 16>& key);
// The co-op's own files, relative to its folder (coop::os files: the game's
// folder on Longshot, the emulator's config folder there).
inline constexpr const char* kSettingsFile = "coop.ini";
inline constexpr const char* kPlayerKeyFile = "player-key.txt";

// The key in `rel` (kPlayerKeyFile, beside coop.ini). A missing or broken
// file gets a new random key written to it, once, so the kit keeps it; if
// that cannot be written the key holds for this run only (`note` says so).
// Not in coop.ini: the launcher writes that file anew from its template.
std::array<std::uint8_t, 16> LoadPlayerKey(const char* rel, std::string& note);

// Reads `key = value` lines. Blank lines, `#` or `;` comments and `[section]`
// headers are skipped. Keys: server (`host` or `host:port`), port, name, password,
// color (a colour name or #RRGGBB, CoopColors::Parse; "" for none). The keys
// the game's co-op module writes there itself (client_id, last_invite,
// last_host, look_*) are passed over without a word.
// Each problem is added to `errors` with its line number. A problem only
// drops its own line: an unknown key is ignored, a bad value leaves that
// setting as it was (a bad server: none, so co-op stays off).
void ApplyIni(std::string_view text, Settings& settings, std::vector<std::string>& errors);

// LICHII_COOP_SERVER, LICHII_COOP_PORT, LICHII_COOP_NAME, LICHII_COOP_PASSWORD,
// LICHII_COOP_COLOR, when set.
// `get` returns a variable's value, or nothing when it is not set.
void ApplyEnvironment(const std::function<std::optional<std::string>(const char*)>& get,
                      Settings& settings, std::vector<std::string>& errors);

// The ini at `rel` (kSettingsFile; a missing file is fine), then the process
// environment.
// `errors` are warnings: each drops only its own line (ApplyIni), never the
// rest of the settings, so a key a newer build wrote never turns co-op off.
Settings LoadSettings(const char* rel, std::vector<std::string>& errors);

} // namespace CoopHook
