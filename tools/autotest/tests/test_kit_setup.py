import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import kit_setup as ks  # noqa: E402


class Fake:
    def __init__(self, fail_on=None):
        self.log, self.fail_on = [], fail_on

    def _do(self, *call):
        if self.fail_on == call[0]:
            raise RuntimeError("refused")
        self.log.append(call)
        return {"ok": True}

    def give(self, item):
        return self._do("give", item)

    def flag(self, name, value=1):
        return self._do("flag", name, value)

    def warp(self, scene, entrance=None, timeout=60):
        return self._do("warp", scene, entrance)


class Plan(unittest.TestCase):
    def test_life_is_quarter_hearts_and_the_warp_comes_last(self):
        calls = ks.plan({"warp": {"scene": "GohmaStage", "entrance": "GohmaStage"}, "hearts": 5, "max_hearts": 5})
        self.assertEqual(calls, [("flag", "PlayerStatus.MaxLife", 20), ("flag", "PlayerStatus.Life", 20),
                                 ("warp", "GohmaStage", "GohmaStage")])

    def test_items_first_then_magic_rupees_keys(self):
        calls = ks.plan({"magic": 2, "rupees": 99, "items": ["bow"], "small_keys": {"ForestTemple": 3},
                         "boss_keys": ["JabuJabu"]})
        self.assertEqual(calls, [("give", "bow"), ("give", "Item_FairyBless_MP"), ("give", "Item_FairyBless_MPDouble"),
                                 ("flag", "PlayerStatus.CurrentRupee", 99),
                                 ("flag", "DungeonInfo.ForestTemple.SmallKeyNum", 3),
                                 ("flag", "DungeonInfo.InsideJabuJabuBelly.IsGetBossKey", 1)])

    def test_no_magic_clears_the_meter(self):
        self.assertEqual(ks.plan({"magic": 0}), [("flag", "PlayerStatus.MPMax", 0), ("flag", "PlayerStatus.MP", 0)])

    def test_a_warp_without_an_entrance(self):
        self.assertEqual(ks.plan({"warp": {"scene": "KokiriForest_Child"}}), [("warp", "KokiriForest_Child", None)])

    def test_bad_specs_are_refused(self):
        for bad in ({"hart": 3}, {"hearts": 0}, {"hearts": True}, {"hearts": 21}, {"magic": 3}, {"rupees": -1},
                    {"small_keys": {"Nowhere": 1}}, {"small_keys": {"ForestTemple": 10}}, {"boss_keys": "ForestTemple"},
                    {"items": ["a b"]}, {"flags": {"x y": 1}}, {"flags": {"X": 1.5}}, {"warp": {"scene": "A", "x": 1}},
                    {"warp": {}}, [], {"max_hearts": 2}):
            with self.assertRaises(ValueError, msg=repr(bad)):
                ks.plan(bad)

    def test_flags_pass_through_in_order(self):
        calls = ks.plan({"flags": {"Gohma_DefeatBoss": 0, "IsGet.Item_Stone_Forest": True}})
        self.assertEqual(calls, [("flag", "Gohma_DefeatBoss", 0), ("flag", "IsGet.Item_Stone_Forest", True)])


class Apply(unittest.TestCase):
    def test_runs_every_call_in_order(self):
        c = Fake()
        done = ks.apply(c, {"hearts": 3, "items": ["bombs"], "warp": {"scene": "S"}})
        self.assertEqual(c.log, [("give", "bombs"), ("flag", "PlayerStatus.Life", 12), ("warp", "S", None)])
        self.assertEqual(len(done), 3)

    def test_a_refusal_says_which_step_and_what_was_done(self):
        c = Fake(fail_on="flag")
        with self.assertRaises(RuntimeError) as e:
            ks.apply(c, {"items": ["bombs"], "hearts": 3})
        self.assertIn("step 2 of 2", str(e.exception))
        self.assertIn("PlayerStatus.Life", str(e.exception))

    def test_a_bad_spec_runs_nothing(self):
        c = Fake()
        with self.assertRaises(ValueError):
            ks.apply(c, {"items": ["bombs"], "hearts": 0})
        self.assertEqual(c.log, [])


REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def _source_text():
    out = []
    for sub in ("coop/host/src", "lichii/hook", "tools/autotest/scenarios", "docs"):
        for dp, _, fs in os.walk(os.path.join(REPO, sub)):
            for f in fs:
                if f.endswith((".cpp", ".h", ".hpp", ".inc", ".txt", ".md")):
                    with open(os.path.join(dp, f), encoding="utf-8", errors="replace") as fh:
                        out.append(fh.read())
    return chr(10).join(out)


class Presets(unittest.TestCase):
    def test_the_shipped_presets_are_there(self):
        names = set(ks.presets())
        self.assertTrue({"deku-start", "deku-before-gohma", "before-dodongo", "before-barinade", "before-phantom-ganon",
                         "before-volvagia", "before-morpha", "before-bongo-bongo", "before-twinrova",
                         "before-ganondorf"} <= names, names)

    def test_every_preset_plans_and_every_name_is_one_the_repository_already_uses(self):
        text = _source_text()
        starts = open(os.path.join(REPO, "lichii/hook/coop_start_points.inc"), encoding="utf-8").read()
        for name in ks.presets():
            calls = ks.plan(ks.load_preset(name))
            self.assertEqual(calls[-1][0], "warp", name)       # the warp is last
            scene, start = calls[-1][1], calls[-1][2]
            self.assertIn('{"%s", "%s"' % (scene, start), starts, name)
            for c in calls:
                if c[0] == "give":
                    self.assertIn(c[1].split(".")[1] if c[1].startswith("PouchData.") else c[1] if "_" in c[1] else "",
                                  text, (name, c))
                elif c[0] == "flag" and not c[1].startswith(("PlayerStatus.", "DungeonInfo.")):
                    self.assertIn(c[1], text, (name, c))

    def test_a_bad_preset_name_is_refused_with_the_list(self):
        for bad in ("nope", "../x", "", None, "A"):
            with self.assertRaises(ValueError) as e:
                ks.load_preset(bad)
            self.assertIn("deku-start", str(e.exception))


if __name__ == "__main__":
    unittest.main()
