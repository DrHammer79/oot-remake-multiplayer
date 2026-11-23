// SPDX-License-Identifier: GPL-3.0-or-later
// Gold Skulltulas, Shared or Not shared (coop_gold_mode.h, sync-coverage row 63).
#include <doctest/doctest.h>

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
