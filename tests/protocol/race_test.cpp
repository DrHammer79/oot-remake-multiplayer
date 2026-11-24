// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "coop/protocol/race.hpp"

namespace proto = coop::protocol;

TEST_CASE("race mode: the host's setting is read from the coop entry only") {
  CHECK_FALSE(proto::race_of({}).has_value());  // an older host: off
  CHECK_FALSE(proto::race_of({{"rando", "race", "on"}}).has_value());
  CHECK(proto::race_of({{"coop", "gold_skulltula", "own"}, {"coop", "race", "on"}}) == true);
  CHECK(proto::race_of({{"coop", "race", "off"}}) == false);
  CHECK(proto::race_of({proto::race_setting(true)}) == true);
}

TEST_CASE("race mode: world messages stay in each game, the players' own do not") {
  CHECK(proto::race_private(proto::FlagSet{}));
  CHECK(proto::race_private(proto::FlagSnapshot{}));
  CHECK(proto::race_private(proto::ObjectSample{}));
  CHECK(proto::race_private(proto::DropTake{}));
  CHECK(proto::race_private(proto::SaveAsk{}));
  CHECK(proto::race_private(proto::SwitchState{}));
  CHECK(proto::race_private(proto::JoinPartner{}));
  CHECK_FALSE(proto::race_private(proto::Chat{}));
  CHECK_FALSE(proto::race_private(proto::PlayerSample{}));
  CHECK_FALSE(proto::race_private(proto::PlayerLook{}));
  CHECK_FALSE(proto::race_private(proto::VoiceFrame{}));
  CHECK_FALSE(proto::race_private(proto::PvpHit{}));
  CHECK_FALSE(proto::race_private(proto::ModSettings{}));
  CHECK_FALSE(proto::race_private(proto::Host{}));
}
