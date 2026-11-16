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
