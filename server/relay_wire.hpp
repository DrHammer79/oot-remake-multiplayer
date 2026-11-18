#pragma once

// The relay's own datagrams (docs/play-online.md, "Relay with join codes").
// They are not co-op messages: the relay never reads the game's traffic, it
// only forwards the host's ENet datagrams to the guests and back.
//
// Every relay datagram starts with kMagic. Its first byte, 'O' (0x4F), has
// ENet's "compressed" header bit set; we never turn ENet's compression on, so
// no ENet datagram starts like that and the relay can tell them apart on one
// port. All numbers are big-endian.
//
// Room names (issue 261) came later as three more kinds. A relay from before
// them decodes those as nothing and answers nothing, so a host or guest that
// gets no answer to them can say the relay is too old; hosts and guests that
// use join codes work with either relay.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coop::server::relay {

inline constexpr std::uint16_t kDefaultRelayPort = 27530;
inline constexpr std::array<std::uint8_t, 4> kMagic = {'O', 'C', 'R', 1};
// ENet datagrams are at most protocol::kDatagramBytes (1100); a relayed one
// with our header is still under a 1200-byte path (issue 143).
inline constexpr std::size_t kMaxPayloadBytes = 1400;
inline constexpr std::size_t kCodeLength = 8;
inline constexpr std::size_t kMaxKeyBytes = 64;
// Room names: kMinRoomLength to kMaxRoomLength of A-Z, a-z, 0-9, '-' and '_',
// compared without case.
inline constexpr std::size_t kMinRoomLength = 3;
inline constexpr std::size_t kMaxRoomLength = 24;

enum class Kind : std::uint8_t {
  // Host link -> relay. All carry the session token the link chose.
  kRegister = 0x01,      // token, key length u8, key: "give me a join code"
  kKeepalive = 0x02,     // token (the relay answers kRegistered)
  kLinkData = 0x03,      // token, flow u16, one ENet datagram for that guest
  kLinkClose = 0x04,     // token, flow u16: that guest is gone on the host
  kBye = 0x05,           // token: the host stopped
  kRegisterName = 0x06,  // token, key length u8, key, name length u8, name:
                         // "give me a join code and this room name"
  // Guest -> relay.
  kLookup = 0x10,      // the 8-letter code: lets this address join that host
  kLookupName = 0x11,  // name length u8, name: the same with a room name
  // Relay -> host link.
  kRegistered = 0x81,      // token, code
  kRefused = 0x82,         // token, Refusal u8
  kRelayData = 0x83,       // flow u16, one ENet datagram from that guest
  kRelayClose = 0x84,      // flow u16: that guest went quiet
  kRegisteredName = 0x85,  // token, code, name length u8, name: the room is this host's
  // Relay -> guest.
  kFound = 0x90,  // LookupResult u8
};

enum class Refusal : std::uint8_t {
  kUnknownSession = 1,  // the relay does not know this token (it restarted)
  kFull = 2,            // the relay has as many hosts as it takes
  kWrongKey = 3,        // the relay wants a host key and this was not it
  kTooMany = 4,         // too many hosts from this address
  kSlowDown = 5,        // too many registrations from this address lately
  kNameTaken = 6,       // another host has this room name
  kBadName = 7,         // not a room name
};

enum class LookupResult : std::uint8_t {
  kOk = 0,
  kUnknown = 1,   // no host has this code (mistyped, or the host stopped)
  kSlowDown = 2,  // too many wrong codes from this address: wait 10 minutes
  kFull = 3,      // this host already has the most guest addresses it takes
};

struct Message {
  Kind kind = Kind::kKeepalive;
  std::uint64_t token = 0;
  std::uint16_t flow = 0;
  std::uint8_t status = 0;  // Refusal or LookupResult
  std::string code;         // kRegistered, kRegisteredName, kLookup: kCodeLength letters
  std::string key;          // kRegister, kRegisterName
  std::string name;         // kRegisterName, kRegisteredName, kLookupName: at most kMaxRoomLength bytes
  std::vector<std::uint8_t> payload;
};

bool has_magic(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode(const Message& message);
// Nothing for a datagram that is not a well-formed relay message.
std::optional<Message> decode(std::span<const std::uint8_t> bytes);

// Join codes: kCodeLength letters from 32 that cannot be mixed up when read
// aloud or typed (no I, O, 0, 1): 40 bits.
inline constexpr std::string_view kCodeLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
// A code from 40 random bits.
std::string make_code(std::uint64_t random_bits);
// What a player typed ("abcd-efgh", " ABCD EFGH ") as the bare code, or
// nothing when it cannot be one.
std::optional<std::string> normalize_code(std::string_view typed);
// "ABCDEFGH" -> "ABCD-EFGH", the way it is shown.
std::string show_code(std::string_view code);

// What a player typed as a room name, without spaces or tabs at either end and
// with its letters' case kept; nothing when it is not one.
std::optional<std::string> normalize_room(std::string_view typed);
// The form room names are compared in: lower case ("Forest-Gang" and
// "forest-gang" are one room).
std::string room_key(std::string_view room);

std::string_view refusal_text(Refusal why);

}  // namespace coop::server::relay
