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


ADAPTER = "[  95.1] Debug.Emulated <Info> core\\hle\\kernel\\svc\\svc_debug_string.cpp:25:OutputDebugString: coop: "
NET_PROTO = [
    "hello from subsdk1 (ExeFS prototype)",
    "hook 0: frame hook in at main+0x6e70f8, continues at main+0x3b4e300",
    "net: connect 0, sent 29 bytes x3 to 127.0.0.1:27520",
]
NET_ADAPTER = [
    "net: udp bound to port 61375 through the platform slots",
    "net: udp sent 26 bytes to 127.0.0.1:27520",
    "net: udp echo 26 bytes back from 127.0.0.1:27520, same text",
    "net: enet connecting to 127.0.0.1:27521",
    "net: enet connected to the server (mtu 1392)",
    "net: enet disconnected",
    "net: self-test done",
]
HELLOS = "listen: 127.0.0.1:61000 b'coop-proto hello from subsdk1'\n" * 3


def net_run(adapter_lines, alive=True, other=()):
    text = "\n".join([log(NET_PROTO)] + [ADAPTER + x for x in list(other) + list(adapter_lines)])
    e, f, d = run_check.PRESETS["net"]
    return run_check.verdict(run_check.module_lines(text), e, f, d, HELLOS, alive)


class NetPresetTest(unittest.TestCase):
    def test_pass(self):
        ok, report = net_run(NET_ADAPTER)
        self.assertTrue(ok, report)
        self.assertEqual(report[-1], "RESULT PASS")

    def test_adapter_lines_count_only_for_their_topics(self):
        lines = run_check.module_lines("\n".join(ADAPTER + x for x in ("boss: moved Link", "net: self-test done")))
        self.assertEqual(lines, ["net: self-test done"])

    def test_no_echo_fails(self):
        lines = [x for x in NET_ADAPTER if "udp echo" not in x] + ["net: FAIL no udp echo in 300 frames"]
        ok, report = net_run(lines)
        self.assertFalse(ok)
        self.assertIn("FAIL 5 udp echo read back, same text", report)
        self.assertTrue(any(r.startswith("FAIL 3-8 no net failure: net: FAIL no udp echo") for r in report))

    def test_different_echo_text_fails(self):
        lines = [x.replace("same text", "DIFFERENT text") for x in NET_ADAPTER]
        self.assertFalse(net_run(lines)[0])

    def test_enet_timeout_fails(self):
        lines = [x for x in NET_ADAPTER if "enet connected" not in x and "disconnected" not in x]
        ok, report = net_run(lines + ["net: FAIL enet connect timed out"])
        self.assertFalse(ok)
        self.assertIn("FAIL 6 enet connected to coop_server", report)

    def test_out_of_memory_fails(self):
        ok, report = net_run(NET_ADAPTER, other=["enet: out of memory (the arena is full)"])
        self.assertFalse(ok)
        self.assertTrue(any(r.startswith("FAIL 9 ENet never out of memory") for r in report))

    def test_net_lines_outside_the_module_do_not_count(self):
        text = "\n".join([log(NET_PROTO)] + ["[ 95.0] Network <Info> " + x for x in NET_ADAPTER])
        e, f, d = run_check.PRESETS["net"]
        self.assertFalse(run_check.verdict(run_check.module_lines(text), e, f, d, HELLOS, True)[0])

    def test_echo_length_is_the_module_text(self):
        src = (Path(__file__).resolve().parents[2] / "switch" / "adapter" / "net_selftest.cpp").read_text(encoding="utf-8")
        hello = re.search(r'kHello\[\] = "([^"]*)"', src).group(1)
        self.assertEqual(len(hello), 26)

    def test_crash_fails(self):
        self.assertFalse(net_run(NET_ADAPTER, alive=False)[0])


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


class HarnessPresetsTest(unittest.TestCase):
    def test_harness_offers_exactly_the_checked_presets(self):
        ps1 = (Path(__file__).resolve().parent / "run_fritchii.ps1").read_text(encoding="utf-8")
        offered = re.search(r"\[ValidateSet\(([^)]*)\)\]\[string\]\$Preset", ps1).group(1)
        self.assertEqual(sorted(re.findall(r"'([a-z]+)'", offered)), sorted(run_check.PRESETS))
        for table in re.findall(r"@\{ (proto = [^}]*)\}\[\$Preset\]", ps1):  # seconds and datagrams per preset
            self.assertEqual(sorted(re.findall(r"([a-z]+) = ", table)), sorted(run_check.PRESETS), table)

    def test_net_datagrams_match_the_preset(self):
        ps1 = (Path(__file__).resolve().parent / "run_fritchii.ps1").read_text(encoding="utf-8")
        table = re.search(r"\$datagrams = @\{([^}]*)\}", ps1).group(1)
        self.assertIn(f"net = {run_check.PRESETS['net'][2]}", table)


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
