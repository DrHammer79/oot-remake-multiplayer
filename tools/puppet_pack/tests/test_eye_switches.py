"""Tests for eye_switches.classify (deku-ladder, A #39). Synthetic wiring only: no game files.

    python tools/puppet_pack/tests/test_eye_switches.py   (CI runs it through test_puppet_pack.py)
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import eye_switches  # noqa: E402

LINE = ("Ev", "Scene", "Obj_StepSwitch_A_01", (0.0, 0.0, 0.0), "OnState", (1.0, 0.0, 0.0))
SHARED = {"Room_Done", "Room_Other"}


def wire(on=(), results=(), delete=()):
    return {"on": list(on), "results": list(results), "delete": list(delete)}


def asker_or(flag, neg=False):
    """An actor input that ORs the asker's output with `flag` (same polarity)."""
    return (("EventRequestActor", neg, True), (flag, neg, True))


class ClassifyTest(unittest.TestCase):
    def classify(self, w):
        return eye_switches.classify(LINE, w, SHARED)

    def test_shared_flags_only_is_a(self):
        w = wire(on=[("BootUp", [("flag", "Room_Done", False)])],
                 results=[("flag", "Room_Done", "set"), ("flag", "Room_NaviHint_01", "set")],
                 delete=["Room_Done"])
        self.assertEqual(self.classify(w)[0], "A")

    def test_actor_or_shared_flag_is_a_and_and_is_not(self):
        covered = ("actor", "Obj_Shutter", ("Open", asker_or("Room_Done")))
        self.assertEqual(self.classify(wire(results=[covered]))[0], "A")
        # a False output (negated) on both sides still covers
        negated = ("actor", "Obj_Shutter", ("Lock", asker_or("Room_Done", True)))
        self.assertEqual(self.classify(wire(results=[negated]))[0], "A")
        # an AND with the flag (not an OR) does not: the actor waits for our asker
        anded = ("actor", "Obj_Fire", ("StartBurnSign", (("EventRequestActor", False, False), ("Room_Done", False, False))))
        self.assertEqual(self.classify(wire(results=[anded]))[:2], ("B", "none"))
        # an unshared flag does not either
        own = ("actor", "Obj_Shutter", ("Open", asker_or("Room_Own")))
        self.assertEqual(self.classify(wire(results=[own]))[0], "B")

    def test_the_events_moment_only_is_a(self):
        w = wire(results=[
            ("actor", "Obj_TBox_A_01_NoBake", ("isSpawnAct", (("EventRequestActor", False, False),))),
            ("actor", "PushAirWall_Box", ("BootUp", (("EventRequestActor", False, False),))),
            ("actor", "Area", ("Logic_Delete", (("EventRequestActor", False, False),))),
            ("module", "Gimmick_TrapTreasureBox_Set.logic.module", ("IsSpawnAct", ())),
            ("node", "TriggerRequestAutoSave", "IsDrive"),
            ("node", "TriggerDungeonRoomStepForward", "IsDrive"),
        ])
        self.assertEqual(self.classify(w)[0], "A")

    def test_unshared_flag_or_node_is_b(self):
        self.assertEqual(self.classify(wire(results=[("flag", "Gimmick_Water", "set")]))[0], "B")
        self.assertEqual(self.classify(wire(results=[("flag", "Gimmick_Water", "clear")]))[0], "A")
        self.assertEqual(self.classify(wire(results=[("node", "OperateGameDataAllocAutogenGameData", "IsWrite")]))[0], "B")

    def test_b_fixes(self):
        miss = [("flag", "Gimmick_Water", "set")]
        # an unwired forcing input: set it (BootUp and Active are not ON inputs to force)
        w = wire(on=[("BootUp", []), ("FirstOn", [])], results=miss)
        self.assertEqual(self.classify(w)[:2], ("B", "input FirstOn"))
        self.assertEqual(self.classify(wire(on=[("BootUp", []), ("Active", [])], results=miss))[:2], ("B", "none"))
        # the ON input reads a shared flag that does not delete the asker: hold only the delete flag
        w = wire(on=[("ForceControl", [("flag", "Room_Done", False)])], results=miss, delete=["Room_Other"])
        self.assertEqual(self.classify(w)[:2], ("B", "split"))
        # ... the same flag deletes the asker: no way
        w = wire(on=[("ForceControl", [("flag", "Room_Done", False)])], results=miss, delete=["Room_Done"])
        self.assertEqual(self.classify(w)[:2], ("B", "none"))

    def test_the_switch_itself_covers_when_its_on_input_is_a_shared_flag(self):
        reads_switch = ("actor", "Obj_Lift", ("IsOn", (("EventRequestActor", False, True), (LINE[2], False, True))))
        self.assertEqual(self.classify(wire(on=[("BootUp", [("flag", "Room_Done", False)])], results=[reads_switch]))[0], "A")
        self.assertEqual(self.classify(wire(on=[("BootUp", [])], results=[reads_switch]))[0], "B")


if __name__ == "__main__":
    unittest.main()
