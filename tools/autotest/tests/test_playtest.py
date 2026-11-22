"""Tests for the live play-test kit's pieces that need no game: the desync checker, the MCP server's protocol, and
the script parser's `dump` line. Run: python -I tools/autotest/tests/test_playtest.py"""
import io
import json
import os
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import desync_check  # noqa: E402
import hookshot  # noqa: E402
import mcp_playtest  # noqa: E402


def write_checkpoint(d, tag, a_flags, b_flags, when):
    os.makedirs(os.path.join(d, "checkpoints"), exist_ok=True)
    path = os.path.join(d, "checkpoints", tag + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"tag": tag, "A": {"status": {"map": "ForestTemple"}, "flags": a_flags},
                   "B": {"status": {"map": "ForestTemple"}, "flags": b_flags}}, f)
    os.utime(path, (when, when))


class DesyncCheck(unittest.TestCase):
    def test_agree_then_first_desync_named(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", {"F1": 0, "F2": 1}, {"F1": 0, "F2": 1}, now - 30)
            write_checkpoint(d, "t2", {"F1": 1, "F2": 1}, {"F1": 0, "F2": 1}, now - 20)
            write_checkpoint(d, "t3", {"F1": 1, "F2": 0}, {"F1": 0, "F2": 1}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 1)
            self.assertEqual(lines[0], "t1: agree (A ForestTemple, B ForestTemple)")
            self.assertIn("t2: 1 flag(s) differ", lines[1])
            self.assertIn("<- first desync", lines[1])
            self.assertEqual(lines[2], "    F1: A 1, B 0")
            self.assertEqual(len(lines), 3)  # stops at the first desync
            lines = []
            self.assertEqual(desync_check.check(d, show_all=True, out=lines.append), 1)
            self.assertTrue(any(ln.startswith("t3: 2 flag(s) differ") for ln in lines))

    def test_a_flag_one_game_cannot_read_is_no_desync(self):
        diff, note = desync_check.compare({"A": {"flags": {"F": 1, "G": 0}}, "B": {"flags": {"G": 0}}})
        self.assertEqual(diff, {})
        self.assertIsNone(note)
        diff, note = desync_check.compare({"A": {"flags": None, "error": "no dump"}, "B": {"flags": {}}})
        self.assertIn("no dump", note)

    def test_nothing_to_compare(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(desync_check.check(d, out=lambda s: None), 2)


class Mcp(unittest.TestCase):
    def run_server(self, requests):
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in requests))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve("no-session")
            return [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout

    def test_handshake_tools_and_errors(self):
        out = self.run_server([
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "nope", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "nope"},
        ])
        self.assertEqual([m["id"] for m in out], [1, 2, 3, 4])  # the notification is not answered
        self.assertEqual(out[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual([t["name"] for t in out[1]["result"]["tools"]],
                         ["state", "act", "shot", "checkpoint", "desync_check"])
        self.assertTrue(out[2]["result"]["isError"])
        self.assertEqual(out[3]["error"]["code"], -32601)


class Parse(unittest.TestCase):
    def test_dump_is_a_module_step(self):
        steps = hookshot.parse("dump after_chest\n")
        self.assertEqual(steps[0].kind, "dump")
        self.assertIn("dump", hookshot.MODULE_ONLY)
        for bad in ("dump\n", "dump a b\n", "dump ../x\n"):
            with self.assertRaises(hookshot.ScriptError):
                hookshot.parse(bad)


if __name__ == "__main__":
    unittest.main()
