"""Tests for the play-test kit helpers that need no game (kit_tools.py): step_until's condition and wait, against the
fake bridge of test_playtest_bridge.py. Run: python -I tools/autotest/tests/test_kit_tools.py"""
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import kit_tools  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
from test_playtest_bridge import FakeGame  # noqa: E402


class Condition(unittest.TestCase):
    def ev(self, text, state):
        return kit_tools.evaluate(kit_tools.parse_condition(text), state)

    def test_parse(self):
        self.assertEqual(kit_tools.parse_condition("event == false"), (["event"], "==", False, False))
        self.assertEqual(kit_tools.parse_condition("equips.weapon!=null"), (["equips", "weapon"], "!=", None, False))
        self.assertEqual(kit_tools.parse_condition("scene == KokiriForest_Child")[2], "KokiriForest_Child")
        self.assertEqual(kit_tools.parse_condition('scene == "A B"')[2], "A B")
        self.assertEqual(kit_tools.parse_condition("life <= 4")[1:3], ("<=", 4))
        self.assertEqual(kit_tools.parse_condition("!prompt"), (["prompt"], None, None, True))
        self.assertEqual(kit_tools.parse_condition("pos.1 > -2.5")[2], -2.5)
        for bad in ("", None, "== 3", "event ==", "!life < 3", "a b", "1x == 2", "pos. == 1"):
            with self.assertRaises(kit_tools.ConditionError, msg=repr(bad)):
                kit_tools.parse_condition(bad)

    def test_equal_and_not_equal(self):
        st = {"event": False, "life": 12, "scene": "A", "prompt": None, "pos": [1.0, 2.0, 3.0], "equips": {"weapon": "S"}}
        self.assertTrue(self.ev("event == false", st)[0])
        self.assertFalse(self.ev("event == true", st)[0])
        self.assertTrue(self.ev("life == 12.0", st)[0])           # 12 and 12.0 are one number
        self.assertFalse(self.ev("event == 0", st)[0])            # false is not 0
        self.assertFalse(self.ev("life == true", st)[0])          # 12 is not true
        self.assertTrue(self.ev("prompt == null", st)[0])
        self.assertTrue(self.ev("scene != B", st)[0])
        self.assertTrue(self.ev("equips.weapon == S", st)[0])
        self.assertTrue(self.ev("pos.2 == 3", st)[0])

    def test_ordering_needs_numbers_and_a_null_is_unknown(self):
        st = {"life": None, "rupees": 7, "scene": "A"}
        met, got, why = self.ev("life < 3", st)
        self.assertFalse(met)
        self.assertIn("needs numbers", why)
        self.assertFalse(self.ev("life >= 0", st)[0])             # not "0 or more": nobody knows
        self.assertFalse(self.ev("scene > 1", st)[0])
        self.assertTrue(self.ev("rupees > 6", st)[0])
        self.assertTrue(self.ev("rupees <= 7", st)[0])
        self.assertFalse(self.ev("rupees < 7", st)[0])

    def test_bare_field_is_truthy(self):
        st = {"prompt": "Talk", "event": False, "life": 0, "equips": {}}
        self.assertTrue(self.ev("prompt", st)[0])
        self.assertFalse(self.ev("event", st)[0])
        self.assertTrue(self.ev("!event", st)[0])
        self.assertTrue(self.ev("!life", st)[0])
        self.assertTrue(self.ev("!equips", st)[0])

    def test_a_missing_field_is_said(self):
        met, got, why = self.ev("evnt == false", {"event": False})
        self.assertFalse(met)
        self.assertIn("no field evnt", why)


class StepUntil(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.c = pc.Client(self.game.port)

    def tearDown(self):
        self.c.close()
        self.game.close()

    def steps(self):
        return [m for m in self.game.log if m["cmd"] == "step"]

    def test_a_condition_that_holds_costs_no_frame(self):
        r = kit_tools.step_until(self.c, "scene == ForestTemple")
        self.assertTrue(r["met"])
        self.assertEqual((r["frames_run"], r["value"]), (0, "ForestTemple"))
        self.assertEqual(self.steps(), [])

    def test_steps_in_chunks_until_the_scene_changes(self):
        self.game.scene_at = (1020, "DekuTree")
        r = kit_tools.step_until(self.c, "scene == DekuTree", max_frames=100)
        self.assertTrue(r["met"])
        self.assertEqual(r["frames_run"], 24)                      # 3 chunks of 8: the first look past frame 1020
        self.assertEqual([m["frames"] for m in self.steps()], [8, 8, 8])
        self.assertEqual(r["state"]["scene"], "DekuTree")
        self.assertEqual(self.c.last_frame, 1024)

    def test_chunk_is_the_look_interval(self):
        self.game.scene_at = (1020, "DekuTree")
        r = kit_tools.step_until(self.c, "scene == DekuTree", max_frames=100, chunk=3)
        self.assertEqual(r["frames_run"], 21)

    def test_times_out_and_never_runs_past_max_frames(self):
        r = kit_tools.step_until(self.c, "scene == Nowhere", max_frames=30)
        self.assertFalse(r["met"])
        self.assertEqual(r["frames_run"], 30)
        self.assertEqual([m["frames"] for m in self.steps()], [8, 8, 8, 6])
        self.assertIn("not met after 30 frames", r["why"])
        self.assertEqual(self.c.last_frame, 1030)

    def test_waits_for_a_prompt_and_for_an_event_to_end(self):
        self.game.event = True
        self.game.prompt = None
        r = kit_tools.step_until(self.c, "prompt", max_frames=16)
        self.assertFalse(r["met"])
        self.game.prompt = "Talk"
        self.assertTrue(kit_tools.step_until(self.c, "prompt", max_frames=16)["met"])
        r = kit_tools.step_until(self.c, "event == false", max_frames=8)
        self.assertFalse(r["met"])
        self.game.event = False
        self.assertTrue(kit_tools.step_until(self.c, "event == false", max_frames=8)["met"])

    def test_a_stale_snapshot_is_not_a_match(self):
        self.game.stale = True                                     # Link members are null: "do not know"
        r = kit_tools.step_until(self.c, "rupees >= 0", max_frames=8)
        self.assertFalse(r["met"])
        self.assertIn("needs numbers", r["why"])

    def test_a_typo_is_an_error_not_a_time_out(self):
        with self.assertRaises(kit_tools.ConditionError) as e:
            kit_tools.step_until(self.c, "sene == Nowhere")
        self.assertIn("no field sene", str(e.exception))
        self.assertIn("scene", str(e.exception))                   # the fields it does have
        self.assertEqual(self.steps(), [])

    def test_bounds(self):
        for kw in ({"max_frames": 0}, {"max_frames": 3601}, {"chunk": 0}, {"chunk": 61}):
            with self.assertRaises(ValueError):
                kit_tools.step_until(self.c, "event", **kw)


class StepUntilTool(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.tool = mcp_playtest.Game("p1", pc.Client(self.game.port), os.getcwd())

    def tearDown(self):
        self.tool.client.close()
        self.game.close()

    def test_the_tool_is_listed_and_runs(self):
        names = [t["name"] for t in mcp_playtest.PLAYER_TOOLS]
        self.assertIn("step_until", names)
        tool = mcp_playtest.PLAYER_TOOLS[names.index("step_until")]
        self.assertEqual(tool["inputSchema"]["required"], ["condition"])
        self.game.scene_at = (1010, "DekuTree")
        r = self.tool.call("step_until", {"condition": "scene == DekuTree", "max_frames": 60})
        self.assertTrue(r["met"])
        self.assertEqual(r["frames_run"], 16)
        self.assertNotIn("inventory", r["state"])                  # the MCP layer leaves the bulk out
        json.dumps(r)

    def test_bad_arguments_are_refused(self):
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {})
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {"condition": "event", "max_frames": 99999})
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {"condition": "event", "chunk": 0})
        self.assertEqual([m for m in self.game.log if m["cmd"] == "step"], [])


if __name__ == "__main__":
    unittest.main()
