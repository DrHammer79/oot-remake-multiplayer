"""Tests for kit_move.py (shoot, climb, push, jump) against a small simulated game: no game, no bridge. Each test
builds the world the move meets (a ladder, a block, a gap, a target) and checks the result and the reason.
Run: python -I tools/autotest/tests/test_kit_move.py"""
import math
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import kit_move as km  # noqa: E402
import mcp_playtest  # noqa: E402


class Sim:
    """A held game: Link moves 0.15 m a frame at full stick along the world yaw, stands on `ground(x, z)` (None: a
    pit, which puts him back at `respawn` and takes 2 life unless he runs across it at full stick, at most `jump_m`
    wide). A ladder (`ladder`: x, z, bottom, top) holds him when he is within 0.6 m of it: the stick up climbs 0.05 m a
    frame. A block moves after Link leaned on it `lean` frames. A shot: ZR held 30 frames or more and let go spends one
    seed and, when the target `takes` shots and Link faces it, takes one life."""

    def __init__(self, pos=(0.0, 0.0, 0.0), ground=None, ladder=None, actors=None, seeds=5, item_owned=True):
        self.pos = list(pos)
        self.ground = ground or (lambda x, z: 0.0)
        self.ladder = ladder
        self.actors = actors or []
        self.inv = {"PouchData.Item_DekuSeed.StockNum": seeds}
        self.item_owned = item_owned
        self.frame, self.life, self.scene, self.event, self.prompt = 100, 12, "Ydan", False, None
        self.facing = 0.0
        self.respawn = list(pos)
        self.jump_m = 3.0
        self.zr = 0
        self.assigned = None
        self.assist = False
        self.lines = []
        self.lean = 0
        self.lean_needed = 20
        self.block_limit = None
        self.event_on_draw = False
        self.hurt_at = None
        self.no_list = False

    # --- the bridge ---
    def state(self):
        return {"scene": self.scene, "pos": list(self.pos), "life": self.life, "max_life": 12, "event": self.event,
                "loading": False, "prompt": self.prompt, "inventory": dict(self.inv), "frame": self.frame}

    def world(self):
        if self.no_list:
            return {"frame": self.frame, "world": {"actors": [], "why": "no session (solo)"}}
        return {"frame": self.frame, "world": {"frame": self.frame, "actors": [dict(a, pos=list(a["pos"])) for a in self.actors]}}

    def act(self, line):
        self.lines.append(line)
        word = line.split()
        if word[0] == "assign":
            self.assigned = word[1]
        elif word[0] == "face":
            self.facing = math.degrees(math.atan2(float(word[1]) - self.pos[0], float(word[2]) - self.pos[2])) % 360
        elif word[0] == "aim":
            self.assist = True
        return {"ok": True}

    def step(self, frames=1):
        self._frames(frames)
        return {"ok": True, "state": self.state()}

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        for _ in range(frames):
            if "ZR" in buttons:
                self.zr += 1
                if self.event_on_draw and self.zr == 10:
                    self.event = True
            else:
                if self.zr >= 30:
                    self._fire()
                self.zr = 0
            if stick:
                self._stick(stick)
            self._frames(1)
        return {"ok": True, "state": self.state()}

    def move(self, yaw, strength=1.0, frames=1, check=True):
        for _ in range(frames):
            self._walk(yaw, strength)
            self._frames(1)
        return {"ok": True, "stick": [0, strength], "state": self.state()}

    # --- the world ---
    def _frames(self, n):
        self.frame += n
        if self.hurt_at is not None and self.frame >= self.hurt_at:
            self.life -= 2
            self.hurt_at = None

    def _fire(self):
        if self.assigned != "SlingShot" or not self.item_owned:
            return
        n = self.inv["PouchData.Item_DekuSeed.StockNum"]
        if n <= 0:
            return
        self.inv["PouchData.Item_DekuSeed.StockNum"] = n - 1
        for a in self.actors:
            if a.get("takes"):
                want = math.degrees(math.atan2(a["pos"][0] - self.pos[0], a["pos"][2] - self.pos[2])) % 360
                if self.assist or abs((want - self.facing + 180) % 360 - 180) < 3:
                    a["life"] -= 1
                    if a["life"] <= 0:
                        a["alive"] = False

    def _on_ladder(self):
        if not self.ladder:
            return False
        x, z, bottom, top = self.ladder
        return math.hypot(self.pos[0] - x, self.pos[2] - z) < 0.6 and bottom - 0.01 <= self.pos[1] <= top + 0.01

    def _stick(self, stick):
        angle, strength = stick
        if self._on_ladder():
            x, z, bottom, top = self.ladder
            dy = 0.05 * strength * (1 if angle == 0 else -1)
            self.pos[1] = max(bottom, min(top, self.pos[1] + dy))
            return
        self._walk((self.facing + angle) % 360, strength)

    def _walk(self, yaw, strength):
        step = 0.15 * strength
        nx = self.pos[0] + math.sin(math.radians(yaw)) * step
        nz = self.pos[2] + math.cos(math.radians(yaw)) * step
        for a in self.actors:
            if a.get("block") and math.hypot(nx - a["pos"][0], nz - a["pos"][2]) < 0.9:
                self.lean += 1
                if self.lean < self.lean_needed:
                    return
                if self.block_limit is not None and a["pos"][2] + (nz - self.pos[2]) > self.block_limit:
                    return
                a["pos"][0] += nx - self.pos[0]
                a["pos"][2] += nz - self.pos[2]
        g = self.ground(nx, nz)
        if g is None:
            self.gap_run = getattr(self, "gap_run", 0.0) + step
            if strength < 1.0 or self.gap_run > self.jump_m:
                self.pos = list(self.respawn)
                self.life -= 2
                self.gap_run = 0.0
                return
            self.pos = [nx, self.pos[1] + 0.05, nz]   # in the air
            return
        self.gap_run = 0.0
        self.pos = [nx, g, nz]


def eye(life=1, takes=True, pos=(0.0, 3.0, 8.0)):
    return {"id": 7, "name": "Obj_Eye", "kind": "switch", "pos": list(pos), "life": life, "alive": True, "takes": takes}


class Shoot(unittest.TestCase):
    def test_hit_takes_a_seed_and_the_target_life(self):
        g = Sim(actors=[eye()])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "hit", r)
        self.assertEqual((r["ammo_before"], r["ammo_after"]), (5, 4))
        self.assertIn("assign SlingShot", g.lines)
        self.assertTrue(any(line.startswith("face 0.00 8.00") for line in g.lines))
        self.assertEqual(r["aim"]["yaw"], 0.0)
        self.assertAlmostEqual(r["aim"]["pitch"], math.degrees(math.atan2(3, 8)), places=1)
        self.assertFalse(r["aim"]["pitch_read"])

    def test_miss_when_the_target_does_not_take_it(self):
        g = Sim(actors=[eye(takes=False)])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "miss", r)
        self.assertIn("5 -> 4", r["reason"])

    def test_no_ammo_is_not_fired_and_runs_no_frame(self):
        g = Sim(actors=[eye()], seeds=0)
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("no ammo", r["reason"])
        self.assertEqual(g.frame, 100)

    def test_ammo_that_does_not_move_is_not_fired(self):
        g = Sim(actors=[eye()], item_owned=False)
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("did not go down", r["reason"])

    def test_a_help_box_while_drawing(self):
        g = Sim(actors=[eye()])
        g.event_on_draw = True
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("help box", r["reason"])

    def test_assist_adds_the_aim_line(self):
        g = Sim(actors=[eye(pos=(4.0, 3.0, 8.0))])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye", assist=True)
        self.assertIn("aim 4.00 3.00 8.00", g.lines)
        self.assertEqual(r["result"], "hit")

    def test_a_point_target_is_only_fired(self):
        g = Sim()
        r = km.shoot(g, "slingshot", at=[0, 1, 5])
        self.assertEqual(r["result"], "fired")
        self.assertEqual(r["ammo_after"], 4)

    def test_no_list_and_gone_and_bad_args(self):
        g = Sim(actors=[eye()])
        g.no_list = True
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("no world list", r["reason"])
        g = Sim(actors=[eye()])
        self.assertEqual(km.shoot(g, "slingshot", ident=7, name="Enemy_Other")["reason"], "gone")
        self.assertEqual(km.shoot(g, "slingshot", ident=99)["reason"], "gone")
        with self.assertRaises(ValueError):
            km.shoot(g, "harp", ident=7)
        with self.assertRaises(ValueError):
            km.shoot(g, "slingshot")
        with self.assertRaises(ValueError):
            km.shoot(g, "slingshot", ident=7, at=[1, 2, 3])

    def test_tilt_holds_the_stick_with_zr(self):
        g = Sim(actors=[eye()])
        calls = []
        orig = g.input
        g.input = lambda stick=None, buttons=(), frames=1, rstick=None: (calls.append((stick, list(buttons), frames)),
                                                                         orig(stick, buttons, frames))[1]
        km.shoot(g, "slingshot", ident=7, name="Obj_Eye", tilt_frames=-6)
        self.assertIn(([180, 1.0], ["ZR"], 6), calls)


class Climb(unittest.TestCase):
    def test_up_to_a_height(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 6.0))
        r = km.climb(g, up=True, to_y=4.0)
        self.assertEqual(r["result"], "arrived", r)
        self.assertGreaterEqual(r["pos"][1], 3.9)

    def test_up_to_the_top_without_a_height(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 2.0))
        r = km.climb(g)
        self.assertEqual(r["result"], "stopped", r)
        self.assertIn("top", r["reason"])

    def test_down(self):
        g = Sim(pos=(0.0, 5.0, 0.0), ladder=(0.0, 0.0, 0.0, 5.0))
        r = km.climb(g, up=False, to_y=1.0)
        self.assertEqual(r["result"], "arrived", r)

    def test_not_on_a_ladder_walks_across(self):
        g = Sim()
        r = km.climb(g, to_y=3.0)
        self.assertEqual(r["result"], "not climbing", r)
        self.assertIn("walking", r["reason"])

    def test_stuck_when_nothing_moves(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 0.0))     # a ladder with no height: the stick does nothing
        r = km.climb(g, to_y=3.0)
        self.assertEqual(r["result"], "stuck", r)
        self.assertIn("never climbed", r["reason"])

    def test_hurt_stops_it(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 9.0))
        g.hurt_at = 120
        r = km.climb(g, to_y=8.0)
        self.assertEqual(r["result"], "hurt", r)

    def test_already_there(self):
        g = Sim(pos=(0.0, 5.0, 0.0))
        self.assertEqual(km.climb(g, to_y=4.0)["result"], "arrived")
        self.assertEqual(g.frame, 100)


def block(pos=(0.0, 0.0, 3.0)):
    return {"id": 3, "name": "Obj_PushBlock", "kind": "other", "pos": list(pos), "alive": True, "block": True}


class Push(unittest.TestCase):
    def test_pushes_the_distance(self):
        g = Sim(actors=[block()])
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "pushed", r)
        self.assertGreaterEqual(r["moved_m"], 1.85)
        self.assertEqual(r["yaw"], 0.0)

    def test_stuck_when_the_block_is_blocked(self):
        g = Sim(actors=[block()])
        g.block_limit = 3.5
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "stuck", r)
        self.assertIn("did not move", r["reason"])

    def test_no_list_and_gone(self):
        g = Sim(actors=[block()])
        g.no_list = True
        self.assertIn("no world list", km.push(g, 3, 1.0)["reason"])
        g = Sim(actors=[block()])
        self.assertEqual(km.push(g, 4, 1.0)["result"], "gone")


class Jump(unittest.TestCase):
    def gap(self, width):
        return lambda x, z: None if 5.0 < z < 5.0 + width else 0.0

    def test_runs_across_a_gap(self):
        g = Sim(ground=self.gap(2.0))
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertTrue(r["airborne"])

    def test_too_wide_is_respawned(self):
        g = Sim(ground=self.gap(6.0))
        r = km.jump(g, 0.0, 14.0, land_y=0.0)
        self.assertEqual(r["result"], "respawned", r)

    def test_fell_below_the_landing(self):
        g = Sim(ground=lambda x, z: 0.0 if z < 5 else -3.0)
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "fell", r)

    def test_stuck_at_a_wall(self):
        g = Sim()
        orig = g._walk
        g._walk = lambda yaw, strength: orig(yaw, strength) if g.pos[2] < 2.0 else None
        r = km.jump(g, 0.0, 10.0)
        self.assertEqual(r["result"], "stuck", r)


class Angles(unittest.TestCase):
    def test_aim_angles_and_ammo(self):
        self.assertEqual(km.aim_angles([0, 0, 0], [5, 0, 0]), (90.0, 0.0))
        self.assertEqual(km.aim_angles([0, 0, 0], [0, 5, 0])[1], 90.0)
        self.assertEqual(km.aim_angles([0, 2, 0], [0, 0, -2])[0], 180.0)
        self.assertEqual(km.ammo_of({"inventory": {"PouchData.Item_Arrow.StockNum": 9}}, "Item_Arrow"), 9)
        self.assertEqual(km.ammo_of({"inventory": {"x": 1}}, "Item_Arrow"), 0)
        self.assertIsNone(km.ammo_of({}, "Item_Arrow"))
        self.assertIsNone(km.ammo_of({"inventory": {"x": 1}}, None))


class Mcp(unittest.TestCase):
    def game(self, sim):
        g = mcp_playtest.Game.__new__(mcp_playtest.Game)
        g.client, g.recorder, g.last_look = sim, None, None
        return g

    def test_the_tools_are_listed_and_routed(self):
        names = [t["name"] for t in mcp_playtest.PLAYER_TOOLS]
        for verb in km.VERBS:
            self.assertIn(verb, names)
        g = self.game(Sim(actors=[eye()]))
        out = g._call("shoot", {"item": "slingshot", "id": 7, "name": "Obj_Eye"})
        self.assertEqual(out["result"], "hit")
        self.assertIn("look", out)
        out = g._call("climb", {"to_y": 1.0})
        self.assertEqual(out["result"], "not climbing")
        out = g._call("jump", {"x": 0, "z": 1})
        self.assertEqual(out["result"], "landed")
        out = g._call("push", {"id": 7, "name": "Obj_Eye", "distance_m": 1})
        self.assertEqual(out["result"], "slipped")   # (an eye is no block: Link walks past it)

    def test_bad_arguments_are_refused(self):
        g = self.game(Sim(actors=[eye()]))
        for name, args in (("shoot", {"item": "slingshot", "at": [1, 2]}), ("push", {"distance_m": 1}),
                           ("jump", {"x": "far", "z": 1}), ("climb", {"max_frames": 99999})):
            with self.assertRaises(ValueError, msg=name):
                g._call(name, args)


if __name__ == "__main__":
    unittest.main()
