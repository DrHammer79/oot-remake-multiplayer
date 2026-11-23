import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import kit_setup as ks  # noqa: E402


class Fake:
    def state(self):
        return {"max_life": 40}

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
                                 ("flag", "PlayerStatus.MPMax", 480), ("flag", "PlayerStatus.MP", 480),
                                 ("flag", "PlayerStatus.CurrentRupee", 99),
                                 ("flag", "DungeonInfo.ForestTemple.SmallKeyNum", 3),
                                 ("flag", "DungeonInfo.InsideJabuJabuBelly.IsGetBossKey", 1)])

    def test_no_magic_clears_the_meter(self):
        calls = ks.plan({"magic": 0})
        self.assertIn(("flag", "IsGet.Item_FairyBless_MP", 0), calls)
        self.assertIn(("flag", "PouchData.Item_FairyBless_MPDouble.StockNum", 0), calls)
        self.assertEqual(calls[-2:], [("flag", "PlayerStatus.MPMax", 0), ("flag", "PlayerStatus.MP", 0)])

    def test_single_magic_takes_double_away(self):
        calls = ks.plan({"magic": 1})
        self.assertEqual(calls[0], ("give", "Item_FairyBless_MP"))
        self.assertIn(("flag", "IsGet.Item_FairyBless_MPDouble", 0), calls)
        self.assertEqual(calls[-2:], [("flag", "PlayerStatus.MPMax", 240), ("flag", "PlayerStatus.MP", 240)])

    def test_hearts_above_the_maximum_are_refused(self):
        with self.assertRaises(ValueError):
            ks.plan({"hearts": 5, "max_hearts": 3})
        c = Fake()
        c.state = lambda: {"max_life": 12}
        with self.assertRaises(ValueError):
            ks.apply(c, {"hearts": 5})
        self.assertEqual(c.log, [])
        ks.apply(c, {"hearts": 3})

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


class Points(unittest.TestCase):
    class C:
        """A game whose flag `Stuck` ignores writes: the restore must say it is not exact."""
        def __init__(self):
            self.flags, self.log, self.pos, self.moves = {"A": 1, "Stuck": 5}, [], [1.0, 2.0, 3.0], True

        def dump(self, tag):
            return {"flags": dict(self.flags), "state": {"scene": "S", "pos": list(self.pos), "life": 12}, "frame": 1}

        def state(self):
            return {"scene": "S"}

        def flag(self, name, value=1):
            self.log.append(("flag", name, value))
            if name != "Stuck":
                self.flags[name] = value

        def act(self, line):
            self.log.append(("act", line))

        def warp(self, *a):
            self.log.append(("warp",) + a)
            if self.moves:   # a load puts Link at the scene's start
                self.pos = [9.0, 9.0, 9.0]

    def test_exact_only_when_the_second_dump_equals_the_saved_one(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.save_point(c, d, "p1", "t")
            c.flags.update({"A": 9, "Stuck": 6})
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual((r["restored"], r["exact"], r["left"]), (2, False, ["Stuck"]))
            self.assertIn(("act", "place 1.0 2.0 3.0"), c.log)
            self.assertIn(("flag", "PlayerStatus.Life", 12), c.log)
            kinds = [x[0] for x in c.log]
            self.assertLess(max(i for i, k in enumerate(kinds) if k == "flag" and c.log[i][1] != "PlayerStatus.Life"),
                            kinds.index("warp"))   # the flags go in first, then the scene loads again
            self.assertTrue(r["reloaded"])

    def test_the_flags_a_setup_wrote_come_back_though_the_dump_cannot_show_them(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.apply(c, {"flags": {"0xabc": "DefeatBoss"}}, log_dir=d, player="p1")
            ks.save_point(c, d, "p1", "t")
            c.log.clear()
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual(r["setup_flags_written"], 1)
            kinds = [x[0] for x in c.log]
            self.assertLess(c.log.index(("flag", "0xabc", "DefeatBoss")), kinds.index("warp"))   # written before the room loads

    def test_a_same_scene_load_that_cannot_be_told_is_not_exact(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            c.moves = False
            ks.save_point(c, d, "p1", "t")
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual((r["reloaded"], r["exact"], r["left"]), (False, False, ["Stuck"][:0]))
            self.assertTrue([x for x in c.log if x[0] == "warp"])

    def test_the_second_game_leaves_the_delta_shared_keys_out(self):
        import tempfile
        c = Fake()
        ks.apply(c, {"small_keys": {"ForestTemple": 2}, "rupees": 5}, shared=False)
        self.assertEqual([x for x in c.log if "SmallKeyNum" in str(x)], [])
        self.assertTrue([x for x in c.log if "CurrentRupee" in str(x)])
        with tempfile.TemporaryDirectory() as d:
            p = self.C()
            p.flags["DungeonInfo.ForestTemple.SmallKeyNum"] = 2
            ks.save_point(p, d, "p1", "t")
            p.flags["DungeonInfo.ForestTemple.SmallKeyNum"] = 0
            r = ks.load_point(p, d, "p1", "t", shared=False)
            self.assertEqual(r["restored"], 0)
            self.assertEqual(r["left"], [])

    def test_a_missing_tag_is_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d, self.assertRaises(ValueError):
            ks.load_point(self.C(), d, "p1", "nope")


if __name__ == "__main__":
    unittest.main()
