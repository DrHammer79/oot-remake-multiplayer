// SPDX-License-Identifier: GPL-3.0-or-later
// Gold Skulltulas, Shared or Not shared (coop_gold_mode.h, sync-coverage row 63).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_gold_mode.h"

namespace proto = coop::protocol;
using CoopGoldMode::Mode;

TEST_CASE("gold mode: the values, and the coop entry a host's settings carry") {
    CHECK(CoopGoldMode::Value(Mode::shared) == "shared");
    CHECK(CoopGoldMode::Value(Mode::own) == "own");
    CHECK(CoopGoldMode::Parse("shared") == Mode::shared);
    CHECK(CoopGoldMode::Parse("own") == Mode::own);
    CHECK_FALSE(CoopGoldMode::Parse("Own").has_value());
    CHECK_FALSE(CoopGoldMode::Parse("").has_value());

    const proto::ModSetting s = CoopGoldMode::AsSetting(Mode::own);
    CHECK(s.mod_id == "coop");
    CHECK(s.key == "gold_skulltula");
    CHECK(s.value == "own");
    CHECK(CoopGoldMode::FromSettings({{"ootrr", "gold_skulltula", "own"}, s}) == Mode::own);
    CHECK_FALSE(CoopGoldMode::FromSettings({{"ootrr", "gold_skulltula", "own"}}).has_value());
    CHECK_FALSE(CoopGoldMode::FromSettings({{"coop", "gold_skulltula", "nonsense"}}).has_value());
}

TEST_CASE("gold mode: a host plays its own choice, a guest the host's, and with no word from the host Shared") {
    CoopGoldMode::ResetForTest();
    CHECK(CoopGoldMode::InForce() == Mode::shared);  // the default

    // Host: its choice from the menu.
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CHECK_FALSE(CoopGoldMode::NotShared());  // not before it hosts
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::NotShared());

    // Guest with its own choice Not shared, host says Shared: Shared.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CHECK(CoopGoldMode::InForce() == Mode::shared);

    // Guest of a host from before this setting (settings with no coop entry): Shared.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({{"ootrr", "seed", "1"}});
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: a change in the menu during a session takes effect at the next Host") {
    CoopGoldMode::ResetForTest();
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::SetChoice(Mode::own);  // changed while hosting
    CoopGoldMode::OnHosting();           // (a host event again in the same session)
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CHECK(CoopGoldMode::Choice() == Mode::own);
    // Stop hosting, Host again: a new connection.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::own);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: a guest that becomes host after a migration keeps the session's mode, not its own choice") {
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});  // the first host's
    CHECK(CoopGoldMode::NotShared());
    CoopGoldMode::OnHosting();  // the host left; we host now
    CHECK(CoopGoldMode::NotShared());
    CoopGoldMode::ResetForTest();

    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: after a Not shared session, a new session keeps nothing local before its mode is told") {
    // Review of f36a8852, bug 1: keep_local has no way back, so the frames of a new session before its
    // welcome must not see the last session's Not shared.
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::KeepLocalNow());  // hosting Not shared

    // Stop hosting; the player picks Shared and joins a friend (or hosts again). Begin resets the mode
    // before its client starts; no welcome or settings yet.
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnConnection();
    CHECK_FALSE(CoopGoldMode::Told());
    CHECK_FALSE(CoopGoldMode::NotShared());
    CHECK_FALSE(CoopGoldMode::KeepLocalNow());

    // A host that says Not shared: now it keeps them.
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});
    CHECK(CoopGoldMode::KeepLocalNow());
    // A host that says Shared: not.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CHECK_FALSE(CoopGoldMode::KeepLocalNow());
    CoopGoldMode::ResetForTest();
}

namespace {
constexpr std::uint32_t kCount = 0x04735978;  // PouchData.Item_Token_Gold.StockNum
constexpr std::uint32_t kGet = 0x6ba394fe;    // IsGet.Item_Token_Gold
constexpr std::uint32_t kOther = 0x11111111;  // another Int, not touched
constexpr std::uint64_t kSkull[] = {0x1000, 0x2000, 0x3000};
constexpr std::uint64_t kWeb = 0x1500;        // a burned web: not a Gold Skulltula's

void Put32(std::vector<std::uint8_t>& d, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Put64(std::vector<std::uint8_t>& d, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint32_t Get32(const std::vector<std::uint8_t>& d, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | d[at + static_cast<std::size_t>(i)];
    return v;
}

// A save in tools/autotest/save_flags.py's layout: Bool IsGet, Int count and another Int, the types in order,
// the end record and its pair, then (with `with_set`) the key set.
std::vector<std::uint8_t> MakeSave(std::uint32_t count, std::uint32_t get, std::vector<std::uint64_t> keys,
                                   bool with_set = true) {
    std::vector<std::uint8_t> d;
    Put32(d, 0x01020304);
    Put32(d, 1);
    Put32(d, 0);  // the pool's offset, below
    while (d.size() < 0x20) d.push_back(0);
    for (std::uint32_t t = 0; t < 0x20; ++t) {
        Put32(d, 0);
        Put32(d, t);
        if (t == 0) {
            Put32(d, kGet);
            Put32(d, get);
        } else if (t == 2) {
            Put32(d, kCount);
            Put32(d, count);
            Put32(d, kOther);
            Put32(d, 77);
        }
    }
    Put32(d, 0);
    Put32(d, 0x20);
    const std::size_t pair = d.size();
    Put32(d, 0xabcdef01);  // the file's hash
    Put32(d, 0);           // the set's offset
    const std::size_t pool = d.size();
    for (int i = 0; i < 4; ++i) d[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pool >> (8 * i));
    if (with_set) {
        for (int i = 0; i < 4; ++i) d[pair + 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pool >> (8 * i));
        std::sort(keys.begin(), keys.end());
        for (const std::uint64_t k : keys) Put64(d, k);
        Put64(d, 0);
    }
    return d;
}
}  // namespace

TEST_CASE("gold mode: Not shared, the host's save keeps this game's own tokens, count and IsGet") {
    // Review of f36a8852: a race guest got the host's tokens and count at join and at Resync from host.
    const std::vector<std::uint8_t> own = MakeSave(1, 1, {kSkull[1] + 2, kSkull[1], 0x9000});
    std::vector<std::uint8_t> host = MakeSave(2, 1, {kSkull[0], kSkull[0] + 2, kSkull[2], kSkull[2] + 2, kWeb});
    const auto kept = CoopGoldMode::KeepOwnGold(host, &own, kSkull, kCount, kGet);
    REQUIRE(kept.done);
    CHECK(kept.host_keys == 4);
    CHECK(kept.own_keys == 2);
    CHECK(kept.host_count == 2);
    CHECK(kept.own_count == 1);
    CHECK_FALSE(kept.keys_left);
    // Ours, and the host's own (the web) kept; not our other keys (0x9000 is not a Skulltula's).
    const std::vector<std::uint8_t> want = MakeSave(1, 1, {kSkull[1], kSkull[1] + 2, kWeb});
    CHECK(host == want);
    CHECK(Get32(host, 0x20 + 8 * 4 + 4 + 8) == 77);  // the other Int (after type 0's two records and type 1's)
}

TEST_CASE("gold mode: Not shared, a guest with no save of its own starts the race from nothing") {
    std::vector<std::uint8_t> host = MakeSave(3, 1, {kSkull[0], kSkull[0] + 2, kWeb});
    const auto kept = CoopGoldMode::KeepOwnGold(host, nullptr, kSkull, kCount, kGet);
    REQUIRE(kept.done);
    CHECK(host == MakeSave(0, 0, {kWeb}));
    // An unreadable save of ours: the same.
    std::vector<std::uint8_t> host2 = MakeSave(3, 1, {kSkull[0], kWeb});
    const std::vector<std::uint8_t> junk(64, 0xee);
    CHECK(CoopGoldMode::KeepOwnGold(host2, &junk, kSkull, kCount, kGet).done);
    CHECK(host2 == MakeSave(0, 0, {kWeb}));
}

TEST_CASE("gold mode: Not shared, a host's save with no key set or that does not read") {
    // No key set: count and IsGet are ours, our keys cannot be added (said in the log).
    const std::vector<std::uint8_t> own = MakeSave(1, 1, {kSkull[1]});
    std::vector<std::uint8_t> host = MakeSave(0, 0, {}, false);
    const auto kept = CoopGoldMode::KeepOwnGold(host, &own, kSkull, kCount, kGet);
    REQUIRE(kept.done);
    CHECK(kept.keys_left);
    CHECK(host == MakeSave(1, 1, {}, false));
    // Not a save: left as it is.
    std::vector<std::uint8_t> junk(64, 0xee);
    const std::vector<std::uint8_t> was = junk;
    const auto bad = CoopGoldMode::KeepOwnGold(junk, &own, kSkull, kCount, kGet);
    CHECK_FALSE(bad.done);
    CHECK(bad.why.find("not a saved game") != std::string::npos);
    CHECK(junk == was);
}
