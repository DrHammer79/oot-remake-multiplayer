"""kit_fight: fight on a scripted game (no game here)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import kit_fight  # noqa: E402
import playtest_client as pc  # noqa: E402


class Game:
    """Link at the origin, one enemy (id 3). A press of the sword button takes `dmg` life off it when it is in reach
    and the lock-on button is in the same press; `bite` takes Link's life off after each round."""
    def __init__(self, dist=6.0, life=3, dmg=1, bite=0, link_life=12, hits_work=True, scene_after=None, stale_after=None):
        self.dist, self.life, self.dmg, self.bite, self.link = dist, life, dmg, bite, link_life
        self.hits_work, self.scene_after, self.stale_after = hits_work, scene_after, stale_after
        self.presses, self.moves, self.rounds, self.scene = [], [], 0, "A"
        self.gone = False

    def state(self):
        return {"scene": self.scene, "life": self.link, "max_life": 12, "pos": [0.0, 0.0, 0.0], "event": False,
                "loading": False}

    def world(self):
        if self.stale_after is not None and self.rounds >= self.stale_after:
            return {"frame": 1, "world": {"actors": [], "stale": True, "why": "no walk for 900 ms"}}
        acts = [] if self.gone else [{"id": 3, "name": "Deku Baba", "kind": "enemy", "pos": [self.dist, 0.0, 0.0],
                                      "dist": self.dist, "life": self.life, "alive": self.life > 0}]
        return {"frame": 1, "world": {"actors": acts, "frame": 1}}

    def input(self, stick, buttons, frames, rstick=None):
        self.presses.append(list(buttons))
        if self.hits_work and "Y" in buttons and "ZL" in buttons and self.dist <= kit_fight.REACH_M:
            self.life = max(0, self.life - self.dmg)
        self.link = max(0, self.link - self.bite)
        self.rounds += 1
        if self.scene_after is not None and self.rounds >= self.scene_after:
            self.scene = "B"
        return {"ok": True}

    def step(self, n):
        return {"ok": True}

    def move(self, yaw, strength=1.0, frames=1, check=True):
        self.moves.append(yaw)
        return {"ok": True}


class FightTest(unittest.TestCase):
    def setUp(self):
        self.orig = pc.walk_to
        self.game = None

        def walk(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
            client.dist = tolerance
            return {"arrived": True, "frames": 8, "state": client.state()}
        pc.walk_to = walk

    def tearDown(self):
        pc.walk_to = self.orig

    def test_walks_up_then_slashes_until_dead(self):
        g = Game(dist=6.0, life=3)
        out = kit_fight.fight(g, 3, "Deku Baba")
        self.assertEqual((out["result"], out["life_before"], out["life_after"]), ("dead", 3, 0))
        self.assertEqual(out["hits"], 3)
        self.assertTrue(all(p == ["ZL", "Y"] for p in g.presses))       # lock-on held while the sword is pressed

    def test_hurt_makes_link_shield_once_then_slash(self):
        g = Game(dist=1.0, life=4, bite=1)
        out = kit_fight.fight(g, 3)
        self.assertGreaterEqual(out["hurt"], 1)
        self.assertIn(["ZL", "R"], g.presses)
        self.assertNotEqual(g.presses[g.presses.index(["ZL", "R"]) + 1], ["ZL", "R"])     # never two shields in a row

    def test_low_life_backs_off_and_stops(self):
        g = Game(dist=1.0, life=50, bite=3, link_life=9)
        out = kit_fight.fight(g, 3)
        self.assertEqual(out["result"], "low")
        self.assertEqual(len(g.moves), 1)
        self.assertAlmostEqual(g.moves[0], 270.0)                       # the enemy is at +x: away is -x (yaw 270)
        self.assertLessEqual(out["link_after"], kit_fight.MIN_LIFE)

    def test_a_scene_change_stops_the_fight_not_a_death(self):
        g = Game(dist=1.0, life=50, scene_after=2)
        out = kit_fight.fight(g, 3)
        self.assertEqual((out["result"], out["stopped"]), ("stopped", "scene changed"))

    def test_a_stale_list_is_not_a_death(self):
        g = Game(dist=1.0, life=50, stale_after=2)
        out = kit_fight.fight(g, 3)
        self.assertEqual(out["result"], "stopped")
        self.assertIn("stale", out["stopped"])

    def test_gone_is_not_called_dead(self):
        g = Game(dist=1.0, life=50)
        orig = g.input

        def vanish(stick, buttons, frames, rstick=None):
            g.gone = True
            return orig(stick, buttons, frames, rstick)
        g.input = vanish
        self.assertEqual(kit_fight.fight(g, 3)["result"], "gone")

    def test_life_that_never_moves_says_the_buttons_may_be_wrong(self):
        g = Game(dist=1.0, life=50, hits_work=False)
        out = kit_fight.fight(g, 3, max_frames=60)
        self.assertEqual(out["result"], "alive")
        self.assertIn("button", out["hint"])
        self.assertIn("ZL", out["hint"])

    def test_other_buttons_can_be_given(self):
        g = Game(dist=1.0, life=1)
        kit_fight.fight(g, 3, lock="L", sword="B", max_frames=24)
        self.assertEqual(g.presses[0], ["L", "B"])

    def test_bad_frame_limit_is_refused(self):
        with self.assertRaises(ValueError):
            kit_fight.fight(Game(), 3, max_frames=0)


if __name__ == "__main__":
    unittest.main()
