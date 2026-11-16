#!/usr/bin/env python3
"""Unit tests for run_check.py (made-up log lines in the emulator's format)."""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_check  # noqa: E402

PREFIX = "[  54.301562] Debug.Emulated <Info> core\\hle\\kernel\\svc\\svc_debug_string.cpp:25:OutputDebugString: coop-proto: "
PROTO_RUN = [
    "hello from subsdk1 (ExeFS prototype)",
    "main at 0x802c6000, malloc +0x14c6c90",
    "hook 0: frame hook in at main+0x6e70f8, continues at main+0x3b4e300",
    "frame 600",
    "game call: set_time returned",
    "net: connect 0, sent 29 bytes x3 to 127.0.0.1:27520",
]
NULL_RUN = [
    "hello from subsdk1 (ExeFS prototype)",
    "hook 0: frame hook in at main+0x6e70f8, continues at main+0x3b4e300",
    "null guards: 29 of 29 applied (a load through a null base reads 0)",
    "frame 6000",
]

OS_RUN = PROTO_RUN + [
    "os selftest: arena before: 8388608 bytes, used 0, peak 0, largest free 8388608, refused/bad 0",
    "os selftest: start at frame 1200, now_us 81234567, thread id 72",
] + [f"os selftest: thread tick {k}, now_us {81234567 + k * 120000} (+120000)" for k in range(1, 11)] + [
    "os selftest: thread ticks 10, now_us delta min 120000 max 121000",
    "os selftest: mutex: game took it 70 times, waited (>1 ms) 6 times, longest 19876 us; counter 80 (expected 80)",
    "os selftest: file: wrote ok, read ok 4096 bytes, hash out 0x1 in 0x1",
    "os selftest: arena after: 8388608 bytes, used 0, peak 132176, largest free 8388608, refused/bad 0",
    "os selftest: PASS (thread, mutex, now_us, file, arena)",
]

CODEMEM_RUN = [
    "hello from subsdk1 (ExeFS prototype)",
    "main at 0x802c6000, malloc +0x14c6c90",
    "codemem: alias at 0x5000000000 for 0x802c6000 +0x3c5b000 (RW; the code stays RX)",
    "codemem: alias at 0x5003c5b000 for 0x87a1f000 +0x1000 (RW; the code stays RX)",
    "hook 0: frame hook in at main+0x6e70f8, continues at main+0x3b4e300",
    "null guards: 29 of 29 applied (a load through a null base reads 0)",
    "codemem: frame site page perm 0x5 (RX), state 0x3",
    "frame 3000",
]


def log(lines):
    return "\n".join(["[   1.0] Loader <Info> something else"] + [PREFIX + x for x in lines])


def run(preset, lines, listen="", alive=True):
    e, f, d = run_check.PRESETS[preset]
    return run_check.verdict(run_check.module_lines(log(lines)), e, f, d, listen, alive)


class ModuleLinesTest(unittest.TestCase):
    def test_only_the_module_text_after_the_tag(self):
        self.assertEqual(run_check.module_lines(log(["frame 600"])), ["frame 600"])

    def test_text_outside_module_lines_does_not_count(self):
        ok, rep = run_check.verdict(run_check.module_lines("frame hook in at main+0x6e70f8 hello from subsdk1"),
                                    [run_check.HELLO], [])
        self.assertFalse(ok)


class ProtoPresetTest(unittest.TestCase):
    def test_the_real_pass(self):
        ok, rep = run("proto", PROTO_RUN, "listen: x b'coop-proto hello from subsdk1'\n" * 3)
        self.assertTrue(ok, rep)
        self.assertEqual(rep[-1], "RESULT PASS")

    def test_two_datagrams_fail(self):
        ok, rep = run("proto", PROTO_RUN, "coop-proto hello from subsdk1\n" * 2)
        self.assertFalse(ok)
        self.assertIn("FAIL listener datagrams (2 of 3)", rep)

    def test_frame_6000_is_not_frame_600(self):
        lines = [x for x in PROTO_RUN if x != "frame 600"] + ["frame 6000"]
        ok, rep = run("proto", lines, "coop-proto hello from subsdk1\n" * 3)
        self.assertIn("FAIL 2 frame hook runs", rep)


class OsPresetTest(unittest.TestCase):
    LISTEN = "coop-proto hello from subsdk1\n" * 3

    def test_the_expected_pass(self):
        ok, rep = run("os", OS_RUN, self.LISTEN)
        self.assertTrue(ok, rep)

    def test_no_contention_fails(self):
        lines = [x.replace("waited (>1 ms) 6 times", "waited (>1 ms) 0 times") for x in OS_RUN]
        ok, rep = run("os", lines, self.LISTEN)
        self.assertIn("FAIL 6 os: mutex contended", rep)

    def test_tick_1_is_not_tick_10(self):
        lines = [x for x in OS_RUN if not x.startswith("os selftest: thread tick 10,")]
        ok, rep = run("os", lines, self.LISTEN)
        self.assertIn("FAIL 5 os: thread ticked 10x", rep)

    def test_self_test_fail_and_sd_refusal_fail(self):
        lines = OS_RUN + ["os selftest: FAIL file round trip",
                          "files: the SD card could not be mounted (no file slots), result 0x00000202"]
        ok, rep = run("os", lines, self.LISTEN)
        self.assertFalse(ok)
        self.assertTrue(any(r.startswith("FAIL 8 no self-test FAIL") for r in rep), rep)
        self.assertTrue(any(r.startswith("FAIL 7 SD card mounted") for r in rep), rep)

    def test_failed_file_read_fails(self):
        lines = [x.replace("read ok 4096", "read FAILED 0") for x in OS_RUN]
        ok, rep = run("os", lines, self.LISTEN)
        self.assertIn("FAIL 7 os: file round trip", rep)

    def test_harness_offers_the_preset(self):
        ps1 = (Path(__file__).resolve().parent / "run_fritchii.ps1").read_text(encoding="utf-8")
        self.assertIn("'os')]", ps1)
        self.assertIn("os = 200", ps1)
        self.assertIn("os = 3", ps1)


class NullGuardPresetTest(unittest.TestCase):
    def test_pass(self):
        ok, rep = run("nullguards", NULL_RUN)
        self.assertTrue(ok, rep)

    def test_partial_application_fails(self):
        lines = [x.replace("29 of 29", "28 of 29") for x in NULL_RUN]
        self.assertIn("FAIL 2 null guards all applied", run("nullguards", lines)[1])

    def test_left_out_guard_fails_and_names_the_line(self):
        bad = "null guard at main+0x4b8240: not the code it was made for, or out of reach: left out"
        ok, rep = run("nullguards", NULL_RUN + [bad])
        self.assertFalse(ok)
        self.assertTrue(any(r.startswith("FAIL 2 no guard left out: null guard at main+0x4b8240") for r in rep))

    def test_island_page_and_rx_restore_failures(self):
        rep = run("nullguards", NULL_RUN + ["null guards: the island page could not be made writable: none applied",
                                           "hook: restore RX failed, result 0xd401"])[1]
        self.assertTrue(any(r.startswith("FAIL 2 island page usable") for r in rep))
        self.assertTrue(any(r.startswith("FAIL 2 code pages back to RX") for r in rep))

    def test_zero_of_zero_fails(self):
        lines = [x.replace("29 of 29", "0 of 0") for x in NULL_RUN]
        self.assertIn("FAIL 2 null guards all applied", run("nullguards", lines)[1])

    def test_pinned_count_matches_the_generated_table(self):
        inc = (Path(__file__).resolve().parents[2] / "switch" / "proto" / "null_guards.inc").read_text(encoding="utf-8")
        m = re.search(r"kNullGuardCount = (\d+);", inc)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), run_check.NULL_GUARDS)

    def test_crash_fails(self):
        self.assertIn("FAIL game still running at the end (no crash)", run("nullguards", NULL_RUN, alive=False)[1])


class CodememPresetTest(unittest.TestCase):
    def test_pass(self):
        ok, rep = run("codemem", CODEMEM_RUN)
        self.assertTrue(ok, rep)

    def test_one_alias_is_not_enough(self):
        for drop, name in ((2, "2 alias of main's text"), (3, "2 alias of the island page")):
            lines = [x for i, x in enumerate(CODEMEM_RUN) if i != drop]
            self.assertIn(f"FAIL {name}", run("codemem", lines)[1])

    def test_page_left_rw_fails(self):
        lines = [x.replace("perm 0x5 (RX)", "perm 0x3 (not RX)") for x in CODEMEM_RUN]
        self.assertIn("FAIL 3 frame site page RX after patching", run("codemem", lines)[1])

    def test_fallback_and_refusals_fail_and_name_the_line(self):
        rep = run("codemem", CODEMEM_RUN + [
            "codemem: MapProcessMemory (svc 0x74) refused, result 0xe401: the rest goes through svc 0x73",
            "codemem: 3 writes through svc 0x73 (refused 0x0, RX back refused 0xd401: those pages stay RW, emulator only)",
        ])[1]
        self.assertTrue(any(r.startswith("FAIL 2 no refused step: codemem: MapProcessMemory") for r in rep))
        self.assertTrue(any(r.startswith("FAIL 3 no write through svc 0x73") for r in rep))
        self.assertTrue(any(r.startswith("FAIL 3 no 0xd401") for r in rep))

    def test_guard_count_is_pinned(self):
        lines = [x.replace("29 of 29", "0 of 0") for x in CODEMEM_RUN]
        self.assertIn("FAIL 4 null guards all applied", run("codemem", lines)[1])

    def test_short_run_fails(self):
        lines = [x.replace("frame 3000", "frame 2400") for x in CODEMEM_RUN]
        self.assertIn("FAIL 5 game plays (frame 3000)", run("codemem", lines)[1])

    def test_unwritten_hook_fails(self):
        rep = run("codemem", CODEMEM_RUN + ["hook: main+0x6e70f8 could not be written (svc 0x73 result 0xd801)"])[1]
        self.assertTrue(any(r.startswith("FAIL 4 every hook written") for r in rep))

    def test_crash_fails(self):
        self.assertIn("FAIL game still running at the end (no crash)", run("codemem", CODEMEM_RUN, alive=False)[1])


HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
INPUT_RUN = [
    "hello from subsdk1 (ExeFS prototype)",
    "hook 0: frame hook in at main+0x6e70f8, continues at main+0x3b4e300",
    "hook 2: import at GOT main+0x4cbf5c8 now goes through the module (not bound yet)",
    "hook 3: import at GOT main+0x4cbf5e0 now goes through the module (not bound yet)",
    "hook 4: import at GOT main+0x4cbf5c0 now goes through the module (not bound yet)",
    "hook 5: import at GOT main+0x4cbf5d0 now goes through the module (not bound yet)",
    "hook 6: import at GOT main+0x4cbf5d8 now goes through the module (not bound yet)",
    "pad: player 1 read through the module (frame 1)",
    "pad: player 1 down A (bit 0, left stick 0,0)",
    "pad: player 1 up A (bit 0, left stick 0,0)",
    "pad: player 1 down B (bit 1, left stick 0,0)",
    "pad: player 1 down Up (bit 13, left stick 0,0)",
    "pad: player 1 down StickL-Up (bit 17, left stick 0,32767)",
    "ui: OPEN",
    "pad: menu open, the game gets no pad from player 1 (closes by itself in 5 s)",
    "ui: CLOSE",
    "pad: menu closed, the game has the pad again",
    "pad: reads FullKey 900 Handheld 900 JoyDual 900 JoyLeft 900 JoyRight 900, lost edges 0, lost events 0",
    "frame 3000",
]


class InputPresetTest(unittest.TestCase):
    def test_pass(self):
        ok, rep = run("input", INPUT_RUN)
        self.assertTrue(ok, rep)

    def test_each_import_must_be_hooked(self):
        for got in run_check.NPAD_GOT.values():
            lines = [x for x in INPUT_RUN if f"main+{got:#x}" not in x]
            self.assertFalse(run("input", lines)[0], f"{got:#x}")

    def test_a_skipped_import_fails_and_names_the_line(self):
        bad = "hook 7: GOT main+0x4cbf5c8 holds 0x1234, not the import or main's PLT (another mod?): skipped"
        ok, rep = run("input", INPUT_RUN + [bad])
        self.assertFalse(ok)
        self.assertIn(f"FAIL 2 no import skipped: {bad}", rep)

    def test_stick_edge_needs_the_stick_pushed(self):
        lines = [x.replace("0,32767", "0,0") for x in INPUT_RUN]
        self.assertIn("FAIL 4d edge left stick up (bit 17)", run("input", lines)[1])

    def test_no_close_fails(self):
        lines = [x for x in INPUT_RUN if x != "ui: CLOSE"]
        self.assertIn("FAIL 5b menu closed by itself", run("input", lines)[1])

    def test_lost_events_fail(self):
        lines = INPUT_RUN + ["pad: reads FullKey 1 Handheld 1 JoyDual 1 JoyLeft 1 JoyRight 1, lost edges 0, lost events 3"]
        self.assertFalse(run("input", lines)[0])

    def test_crash_fails(self):
        self.assertFalse(run("input", INPUT_RUN, alive=False)[0])

    def test_got_offsets_match_the_module(self):
        h = (REPO / "switch" / "adapter" / "input.h").read_text(encoding="utf-8")
        got = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"kGot(\w+) = (0x[0-9a-f]+);", h)}
        self.assertEqual(got, run_check.NPAD_GOT)

    def test_log_texts_are_the_modules(self):  # the preset matches what main.cpp writes
        cpp = (REPO / "switch" / "proto" / "main.cpp").read_text(encoding="utf-8")
        for text in ('": import at GOT main+"', '" now goes through the module ("', '"pad: player 1 read through the module (frame "',
                     '"pad: player "', '" down " : " up "', '"StickL-Up"', '", left stick "', '"ui: "', '"OPEN"', '"CLOSE"',
                     '"pad: menu open, the game gets no pad from player 1', '"pad: menu closed, the game has the pad again"',
                     '", lost edges "', '", lost events "'):
            self.assertIn(text, cpp)

    def test_mark_shots_are_the_harness_and_the_scenario(self):
        ps1 = (HERE / "run_fritchii.ps1").read_text(encoding="utf-8")
        m = re.search(r"input = @\(([^)]*)\) \}\[\$Preset\]", ps1)
        self.assertIsNotNone(m)
        self.assertEqual(re.findall(r"'([^']+)'", m.group(1)), run_check.MARK_SHOTS["input"])
        scenario = (HERE / "scenarios" / "input.txt").read_text(encoding="utf-8").splitlines()
        marks = [line.split()[1] for line in scenario if line.startswith("mark ")]
        for name in run_check.MARK_SHOTS["input"]:
            self.assertIn(name, marks)
        self.assertIn("hold L+R+MINUS 10", scenario)


class HarnessPrivacyTest(unittest.TestCase):
    """Screenshots hold only the game window's own pixels: run_fritchii.ps1 may use PrintWindow, never a screen copy."""

    SCREEN_COPY = ("CopyFromScreen", "BitBlt", "StretchBlt", "GetDC(", "GetWindowDC(", "CreateDC(",
                   "PrintScreen", "SendKeys", "{PRTSC}")

    def test_no_screen_copy_in_the_run_harness(self):
        ps1 = (Path(__file__).resolve().parent / "run_fritchii.ps1").read_text(encoding="utf-8")
        code = "\n".join(line.split("#", 1)[0] for line in ps1.splitlines())  # comments may name them
        for api in self.SCREEN_COPY:
            self.assertNotIn(api, code, f"run_fritchii.ps1 uses {api}: shots must come from PrintWindow only")
        self.assertIn("PrintWindow(", code)


class CommandLineTest(unittest.TestCase):
    def test_pair_parsing(self):
        self.assertEqual(run_check.pair("x=a=b"), ("x", "a=b"))
        with self.assertRaises(Exception):
            run_check.pair("no-equals")

    def test_no_checks_is_a_fail(self):
        ok, rep = run_check.verdict([], [], [])
        self.assertFalse(ok)

    def test_main_with_a_missing_log_fails(self):
        self.assertEqual(run_check.main(["--log", "does-not-exist.txt", "--preset", "nullguards", "--alive", "1"]), 1)


if __name__ == "__main__":
    unittest.main()
