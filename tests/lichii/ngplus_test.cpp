// Tests for New Game+'s rules (lichii/hook/coop_ngplus_rules.cpp, no game code) and its difficulty
// (coop/protocol/difficulty.hpp ngplus_difficulty).
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>

#include "coop/protocol/difficulty.hpp"
#include "coop_ngplus.h"

using CoopNgPlus::Give;
using CoopNgPlus::Saved;
namespace proto = coop::protocol;

TEST_CASE("ngplus: the kept file reads back what was written and clamps what is out of range") {
  const Saved s{2, 3, 40};
  CHECK(CoopNgPlus::Parse(CoopNgPlus::Format(s)) == s);
  CHECK(CoopNgPlus::Parse("level = 5\nunlocked = 2\nhearts = 999\n") == Saved{2, 2, CoopNgPlus::kMostQuarters});
  CHECK(CoopNgPlus::Parse("# nothing\r\nlevel=x\n") == Saved{});
  CHECK(CoopNgPlus::Parse("unlocked = 50\n").unlocked == proto::kMaxNgPlus);
}

TEST_CASE("ngplus: beating Ganon unlocks the next level and keeps the most hearts") {
  Saved s = CoopNgPlus::Beaten(Saved{}, 40);
  CHECK(s == Saved{0, 1, 40});
  s = CoopNgPlus::Beaten(CoopNgPlus::Choose(s, 1), 32);  // fewer hearts this time: the most stays
  CHECK(s == Saved{1, 2, 40});
  CHECK(CoopNgPlus::Beaten(Saved{0, 1, 40}, 0).hearts == 40);  // not known: kept
  CHECK(CoopNgPlus::Beaten(Saved{proto::kMaxNgPlus, proto::kMaxNgPlus, 0}, 200).unlocked == proto::kMaxNgPlus);
  CHECK(CoopNgPlus::Beaten(Saved{}, 200).hearts == CoopNgPlus::kMostQuarters);
}

TEST_CASE("ngplus: a level is chosen within what is unlocked") {
  CHECK(CoopNgPlus::Choose(Saved{0, 2, 0}, 5).level == 2);
  CHECK(CoopNgPlus::Choose(Saved{0, 2, 0}, -1).level == 0);
  CHECK(CoopNgPlus::Choose(Saved{0, 0, 0}, 1).level == 0);
}

TEST_CASE("ngplus: Ganon's death and a new file's opening are named events") {
  CHECK(CoopNgPlus::IsGanonDeath("Defeat_Ganon"));
  CHECK(CoopNgPlus::IsGanonDeath("Dm_FinalBattle_BossOut"));
  CHECK_FALSE(CoopNgPlus::IsGanonDeath("Dm_Kokiri_0000"));
  CHECK(CoopNgPlus::IsNewFileOpening("Dm_Kokiri_0000"));
  CHECK_FALSE(CoopNgPlus::IsNewFileOpening("Dm_Kokiri_0045"));  // leaving the house: a loaded save plays it too
}

namespace {
Give NewFileAlone() {
  Give g;
  g.level = 1;
  g.hearts = 40;
  g.max_life = 12;
  g.new_file = true;
  g.may_give = true;
  return g;
}
}  // namespace

TEST_CASE("ngplus: the kept hearts go into a new file of the host or of a game alone") {
  CHECK(CoopNgPlus::HeartsToGive(NewFileAlone()) == 40);
  Give g = NewFileAlone();
  g.hearts = 500;
  CHECK(CoopNgPlus::HeartsToGive(g) == CoopNgPlus::kMostQuarters);
}

TEST_CASE("ngplus: no hearts for a loaded save, a guest, with the level off or once they hold") {
  Give g = NewFileAlone();
  g.new_file = false;  // a save loaded from the title: its own hearts
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g = NewFileAlone();
  g.may_give = false;  // a guest: it plays the host's save, its own level never raises it
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g = NewFileAlone();
  g.level = 0;
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g = NewFileAlone();
  g.hearts = 0;
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g = NewFileAlone();
  g.max_life = std::nullopt;  // unread
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g = NewFileAlone();
  g.max_life = 40;  // done
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
  g.max_life = 60;  // never lowered
  CHECK_FALSE(CoopNgPlus::HeartsToGive(g));
}

TEST_CASE("ngplus: each level grows enemy and boss life and damage, never the number of enemies") {
  CHECK(proto::ngplus_difficulty({100, 100, 100, 100, 100}, 0) == proto::DifficultyValues{100, 100, 100, 100, 100});
  CHECK(proto::ngplus_difficulty({100, 100, 100, 100, 100}, 2) == proto::DifficultyValues{200, 150, 100, 200, 150});
  // past the most level: the most level
  CHECK(proto::ngplus_difficulty({100, 100, 100, 100, 100}, 200) ==
        proto::ngplus_difficulty({100, 100, 100, 100, 100}, proto::kMaxNgPlus));
  CHECK(proto::valid_difficulty(proto::ngplus_difficulty({300, 200, 200, 400, 160}, proto::kMaxNgPlus)));
}
