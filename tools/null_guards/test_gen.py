#!/usr/bin/env python3
"""Tests of the null guards' one table (sites.py) and its copies (gen.py; switch/proto/null_guards.inc; Hookshot patch
0023's nx2/coop_null_guards.py), and of lift_check.py's rules for the null guard hook kinds.

    python tools/null_guards/test_gen.py -v
"""
import os
import re
import struct
import sys
import tempfile
import unittest

NL = chr(10)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "switch_mod"))
import gen  # noqa: E402
import lift_check  # noqa: E402
import sites  # noqa: E402


class SitesTest(unittest.TestCase):
    def test_guards(self):
        g = sites.guards()
        self.assertEqual(len(g), 29)
        self.assertEqual([o for o, _, _ in g], sorted({o for o, _, _ in g}))   # one each, in main order
        for off, word, checks in g:
            rt, rn, _ = sites.decode(word)
            self.assertNotEqual(rn, 31, hex(off))
            if off in dict(sites.PLAYER_LOADS):
                self.assertEqual(checks, ((off - 4, sites.bl(off - 4, sites.PLAYER_OF)),))
                self.assertEqual(rn, 0)
        later = dict((o, c) for o, _, c in g)[0x27975f0]
        self.assertEqual(later, ((0x27975d0, sites.bl(0x27975d0, sites.PLAYER_OF)), (0x27975d4, 0x394422a8),
                                 (0x27975d8, 0x360000c8)))
        self.assertEqual(dict((o, c) for o, _, c in g)[0x4b85e4], ())

    def test_bl(self):
        self.assertEqual(sites.bl(0x2dceef4, 0x82acc0), 0x97696f73)   # the game's own word there
        self.assertEqual(sites.bl(0x82ac18, 0x82acc0), 0x9400002a)

    def test_decode(self):
        self.assertEqual(sites.decode(0xb947d808), (8, 0, 0x7d8))
        self.assertEqual(sites.decode(0xf940e508), (8, 8, 0x1c8))
        self.assertIsNone(sites.decode(0xf8408c08))


class GenTest(unittest.TestCase):
    def test_inc_is_current(self):
        self.assertEqual(gen.check()[0], [], "run tools/null_guards/gen.py")

    def test_inc_entries(self):
        text = gen.inc_text()
        guards = re.findall(r"\{hooks::kNullGuard, 0, 0x([0-9a-f]{8})u, 0x([0-9a-f]+)u, nullptr, nullptr\}", text)
        checks = re.findall(r"\{hooks::kCheck, 0, 0x([0-9a-f]{8})u, 0x([0-9a-f]+)u, nullptr, nullptr\}", text)
        self.assertEqual([(int(o, 16), int(w, 16)) for w, o in guards], [(o, w) for o, w, _ in sites.guards()])
        self.assertEqual(len(checks), sum(len(c) for _, _, c in sites.guards()))
        self.assertIn(f"kNullGuardCount = {len(guards)};", text)
        self.assertIn(f"kNullGuardEntries = {len(guards) + len(checks)};", text)
        # each guard comes right after its own checks
        order = re.findall(r"hooks::(kNullGuard|kCheck), 0, 0x[0-9a-f]{8}u, 0x([0-9a-f]+)u", text)
        i = 0
        for off, _, cs in sites.guards():
            self.assertEqual(order[i:i + len(cs) + 1],
                             [("kCheck", f"{o:x}") for o, _ in cs] + [("kNullGuard", f"{off:x}")])
            i += len(cs) + 1

    def test_lift_table_compare(self):
        """The patch's table is read from the + lines of its nx2/coop_null_guards.py, and a difference is named."""
        body = "\n".join("+" + line for line in (
            f"BUILD_ID = {sites.BUILD_ID!r}", f"PLAYER_OF = {sites.PLAYER_OF:#x}",
            f"PLAYER_LOADS = {sites.PLAYER_LOADS!r}", f"PLAYER_LOADS_LATER = {sites.PLAYER_LOADS_LATER!r}",
            f"SLOT10_LOADS = {sites.SLOT10_LOADS!r}"))
        patch = ("diff --git a/nx2/other.py b/nx2/other.py\n+PLAYER_OF = 1\n"
                 f"diff --git a/{gen.LIFT_FILE} b/{gen.LIFT_FILE}\n+++ b/{gen.LIFT_FILE}\n{body}\n")
        t = gen.lift_table(patch)
        self.assertEqual({k: t[k] == getattr(sites, k) for k in gen.TABLES}, {k: True for k in gen.TABLES})
        t = gen.lift_table(patch.replace(f"{sites.PLAYER_OF:#x}", "0x82acc4"))
        self.assertNotEqual(t["PLAYER_OF"], sites.PLAYER_OF)
        with self.assertRaises(ValueError):
            gen.lift_table("diff --git a/x b/x\n+y = 1\n")


def lift_patch(**changes):
    """A patch text that adds nx2/coop_null_guards.py with sites.py's tables (or `changes`), as 0023 does."""
    values = {k: getattr(sites, k) for k in gen.TABLES}
    values.update(changes)
    lines = ["From 0 Mon Sep 17 00:00:00 2001", "Subject: [PATCH] x", "---",
             "diff --git a/nx2/coop_hooks.py b/nx2/coop_hooks.py", "+import coop_null_guards",
             gen.LIFT_DIFF, "new file mode 100644", "--- /dev/null", f"+++ b/{gen.LIFT_FILE}"]
    lines += [f"+{k} = {v!r}" for k, v in values.items()]
    return NL.join(lines) + NL


class CheckTest(unittest.TestCase):
    """--check finds the lift's patch by its content (any number), fails when its table differs, says when it skipped."""

    def run_check(self, patches):
        with tempfile.TemporaryDirectory() as d:
            for name, text in patches.items():
                with open(os.path.join(d, name), "w", encoding="utf-8", newline=NL) as f:
                    f.write(text)
            return gen.check(d, gen.INC)

    def test_same_table_under_any_number(self):
        for name in ("0023-coop-null-guards.patch", "0031-renumbered.patch"):
            problems, lift = self.run_check({"0001-other.patch": "diff --git a/x b/x" + NL + "+y" + NL, name: lift_patch()})
            self.assertEqual(problems, [])
            self.assertEqual(lift, f"compared with {name}")

    def test_a_different_table_fails(self):
        moved = sites.PLAYER_LOADS[:-1] + ((sites.PLAYER_LOADS[-1][0] + 4, sites.PLAYER_LOADS[-1][1]),)
        for change in ({"PLAYER_LOADS": moved}, {"PLAYER_OF": sites.PLAYER_OF + 4}, {"BUILD_ID": "0" * 40},
                       {"SLOT10_LOADS": sites.SLOT10_LOADS[:2]}, {"PLAYER_LOADS_LATER": ()}):
            problems, _ = self.run_check({"0023-coop-null-guards.patch": lift_patch(**change)})
            self.assertEqual(len(problems), 1, change)
            self.assertIn(next(iter(change)), problems[0])

    def test_a_patch_that_does_not_run_fails(self):
        bad = lift_patch().replace("+PLAYER_OF = ", "+PLAYER_OF = (")
        problems, lift = self.run_check({"0023-coop-null-guards.patch": bad})
        self.assertTrue(problems)
        self.assertEqual(lift, "compared with 0023-coop-null-guards.patch")

    def test_absent_says_skipped(self):
        problems, lift = self.run_check({"0022-other.patch": "diff --git a/nx2/coop_hooks.py b/nx2/coop_hooks.py" + NL
                                                             + "+import coop_null_guards" + NL})
        self.assertEqual(problems, [])
        self.assertTrue(lift.startswith("skipped: no patch in "), lift)
        self.assertIn(gen.LIFT_FILE, lift)

    def test_two_homes_fail(self):
        problems, _ = self.run_check({"0023-a.patch": lift_patch(), "0030-b.patch": lift_patch()})
        self.assertEqual(len(problems), 1)
        self.assertIn("2 patches", problems[0])

    def test_main_prints_what_it_did(self):
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            gen.main(["--check"])
        self.assertRegex(out.getvalue(), r"null guards: Hookshot's table (compared with |skipped: )")


class FakeElf:
    """Just what lift_check.check reads: a dynsym with the hook table and one slot, and the table's bytes."""

    def __init__(self, entries):
        self.table = 0x9000
        self.slot = 0x9800
        head = struct.pack("<8sIIIIQ", b"COOPHOOK", 1, 32, len(entries), 0, 0)
        self.blob = head + b"".join(struct.pack("<HxxIQQQ", *e) for e in entries)
        self.sections = []
        self.data = b""
        S = lift_check.Symbol
        self.dyn = [S("_init", 0x100, 4, lift_check.STT_FUNC, True), S("coop_frame_entry", 0x200, 4, lift_check.STT_FUNC, True),
                    S("coop_proto_frame", 0x300, 4, lift_check.STT_FUNC, True),
                    S("coop_hooks", self.table, len(self.blob), lift_check.STT_OBJECT, True),
                    S("coop_slot_frame_step", self.slot, 8, lift_check.STT_OBJECT, True)]

    def section(self, name):
        return lift_check.Section(name, 1, 0, 0, 8, 0) if name in (".eh_frame", ".eh_frame_hdr") else None

    def symbols(self, sh_type):
        return self.dyn if sh_type == lift_check.SHT_DYNSYM else []

    def read(self, addr, size):
        return self.blob[addr - self.table:addr - self.table + size]


class LiftCheckKindsTest(unittest.TestCase):
    def problems(self, entries):
        return [p for p in lift_check.check(FakeElf(entries))[0] if "hook" in p]

    def test_guard_and_check_have_no_slot(self):
        ok = [(1, 0x94d19c82, 0x6e70f8, 0x200, 0x9800), (5, 0x97696f73, 0x2dceef4, 0, 0), (4, 0xb947d808, 0x2dceef8, 0, 0)]
        self.assertEqual(self.problems(ok), [])
        self.assertTrue(self.problems([(4, 0xb947d808, 0x2dceef8, 0, 0x9800)]))   # a slot
        self.assertTrue(self.problems([(5, 0x97696f73, 0x2dceef4, 0x200, 0)]))    # a replacement
        self.assertTrue(self.problems([(4, 0, 0x2dceef8, 0, 0)]))                 # no instruction
        self.assertTrue(self.problems([(6, 1, 0x10, 0, 0)]))                      # unknown kind


if __name__ == "__main__":
    unittest.main()
