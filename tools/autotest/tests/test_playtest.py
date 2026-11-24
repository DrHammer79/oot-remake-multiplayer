"""Tests for the live play-test kit's pieces that need no game: the desync checker, the MCP server's protocol, the
script parser's `dump` line, and live.py's lane set-up, watchdog (lane lock, maximum and idle end, clean-up), save
restore default and checkpoint screenshots. Run: python -I tools/autotest/tests/test_playtest.py"""
import argparse
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
import lanes  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
import playtest_helpers as ph  # noqa: E402


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

    def test_no_agree_without_a_comparison(self):
        # A checkpoint where a game wrote no flags is not compared: alone it is "nothing compared" (2), never "agree".
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 2)
            self.assertIn("t1: not compared", lines[0])
            self.assertIn("nothing compared", lines[-1])
            # One agrees, another was not compared: incomplete (3), not agree.
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 3)
            self.assertIn("incomplete: 1 checkpoint(s) not compared", lines[-1])

    def test_shots_named_with_the_checkpoint(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "checkpoints"))
            with open(os.path.join(d, "checkpoints", "t1.json"), "w", encoding="utf-8") as f:
                json.dump({"tag": "t1", "A": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-A.png"},
                           "B": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-B.png"}}, f)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 0)
            self.assertEqual(lines, ["t1: agree (A M, B M; shots t1-A.png, t1-B.png)"])


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

    def test_desync_check_tool_says_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            self.assertEqual(mcp_playtest.call(d, "desync_check", {})["result"], "incomplete")


class Parse(unittest.TestCase):
    def test_dump_is_a_module_step(self):
        steps = hookshot.parse("dump after_chest\n")
        self.assertEqual(steps[0].kind, "dump")
        self.assertIn("dump", hookshot.MODULE_ONLY)
        for bad in ("dump\n", "dump a b\n", "dump ../x\n"):
            with self.assertRaises(hookshot.ScriptError):
                hookshot.parse(bad)


def start_args(**kw):
    a = dict(session="s", lane=None, game_a=None, game_b=None, server="coop_server.exe", port=None, boot_a=None,
             boot_b=None, load=None, no_load=False, no_restore=False, env=[], boot_timeout=900, max_minutes=30,
             idle_minutes=10, lock_wait=0)
    a.update(kw)
    return argparse.Namespace(**a)


def write_session(d, **kw):
    s = {"started": 1000.0, "dir": d, "lane": None, "port": 27593, "booted": True, "max_s": 1800, "idle_s": 600,
         "lock_wait": 0, "offsets": {},
         "games": {x: {"pipe": "p-" + x, "commands": os.path.join(d, "commands-%s.txt" % x),
                       "log": os.path.join(d, "game-out-%s.txt" % x)} for x in live.TAGS}}
    s.update(kw)
    live.save(d, s)
    return s


class LiveLane(unittest.TestCase):
    def test_a_lane_gives_its_pair_port_and_pipes(self):
        L = lanes.lane(1)
        self.assertEqual(live.resolve(start_args(lane=1)), (L["a"], L["b"], L["port"], "coop-lane1"))
        self.assertEqual(live.resolve(start_args(lane=1, port=27999))[2], 27999)
        self.assertEqual(live.resolve(start_args(game_a="ga", game_b="gb")), ("ga", "gb", live.PORT, live.PIPE))
        with self.assertRaises(SystemExit):
            live.resolve(start_args(game_a="ga"))

    def test_runner_restores_the_saves_and_never_takes_the_lane_itself(self):
        argv = live.runner_argv(start_args(lane=1), "ga", "gb", 27520, "coop-lane1", "dir")
        self.assertNotIn("--no-restore", argv)   # the baseline save, as lane runs
        self.assertNotIn("--lane", argv)         # the watchdog holds the lock for the whole session
        self.assertIn("--keep-open", argv)
        self.assertEqual(argv[argv.index("--pipe") + 1], "coop-lane1")
        self.assertEqual(argv[argv.index("--port") + 1], "27520")
        self.assertIn("--no-restore", live.runner_argv(start_args(no_restore=True), "ga", "gb", 1, "p", "dir"))

    def test_the_runner_keeps_pids_for_the_watchdog(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "pids.json"), "w", encoding="utf-8") as f:
                json.dump({"games": {"A": 11, "B": 12}, "server": 13}, f)
            self.assertEqual(live.game_pids(d), ({"A": 11, "B": 12}, 13))
            os.remove(os.path.join(d, "pids.json"))
            self.assertEqual(live.game_pids(d), ({}, None))


class FakeClock:
    def __init__(self, t):
        self.t = t

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class LiveWatchdog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def pids(self, games, server=None):
        with open(os.path.join(self.d, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": games, "server": server}, f)

    def test_maximum_duration_ends_the_session(self):
        write_session(self.d, idle_s=0)
        c = FakeClock(1000.0)
        self.assertIn("maximum duration (30 min)", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        self.assertGreaterEqual(c.t, 1000.0 + 1800)

    def test_maximum_minutes_0_means_no_limit(self):
        # 0 must not end the session at once: only a stop request (here after a simulated day) does.
        write_session(self.d, max_s=0, idle_s=0)
        c = FakeClock(1000.0)

        def sleep(s):
            c.sleep(s)
            if c.t > 1000.0 + 86400:
                open(live.stop_file(self.d), "w").close()
        self.assertEqual(live.watch(self.d, clock=c, sleep=sleep, alive=lambda p: True), "stop asked")
        self.assertGreater(c.t, 1000.0 + 86400)

    def test_no_limits_say_so_in_the_answer(self):
        self.assertEqual(live.ends_text(0, 0), "never by itself (live.py stop, or a game that ended)")
        self.assertEqual(live.ends_text(30, 10), "after 30 min, or 10 min without a command (live.py stop, or a game that ended)")
        self.assertEqual(live.ends_text(0, 5), "5 min without a command (live.py stop, or a game that ended)")

    def test_idle_ends_only_after_the_boot(self):
        write_session(self.d, booted=False)
        live.touch(self.d)
        os.utime(live.active_file(self.d), (1000.0, 1000.0))
        c = FakeClock(1000.0)
        # still booting: no idle end, only the maximum
        self.assertIn("maximum", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        write_session(self.d, booted=True)
        c = FakeClock(1000.0)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True),
                         "no tester command for 10 min")

    def test_stop_request_and_a_game_that_ended(self):
        write_session(self.d)
        live.touch(self.d)
        self.pids({"A": 11, "B": 12})
        c = FakeClock(time.time())
        write_session(self.d, started=c.t)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: p != 12), "game B ended")
        open(live.stop_file(self.d), "w").close()
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True), "stop asked")

    def test_close_quits_kills_what_stays_and_lets_the_lane_go(self):
        s = write_session(self.d, lane=1)
        self.pids({"A": 11, "B": 12}, server=13)
        quits, kills, released = [], [], []
        c = FakeClock(0.0)
        out = live.close(self.d, s, alive=lambda p: p in (12, 13) and p not in kills,
                         quit=lambda s, x: quits.append(x), killer=kills.append,
                         release=lambda n, o: released.append((n, o)) or True, clock=c, sleep=c.sleep)
        self.assertEqual(quits, ["A", "B"])
        self.assertEqual(kills, [12, 13])   # B did not quit; the server always goes
        self.assertEqual(released, [(1, live.lock_owner(self.d))])
        self.assertEqual(out["B"], "quit asked, killed")
        self.assertEqual(out["lock"], "lane1.lock released")

    def test_close_without_a_lane_releases_nothing(self):
        s = write_session(self.d)
        released = []

        def unreachable(s, x):
            raise OSError("no pipe")
        out = live.close(self.d, s, alive=lambda p: False, quit=unreachable, killer=lambda p: None,
                         release=lambda n, o: released.append(n), wait=0)
        self.assertEqual(released, [])
        self.assertIn("not reached", out["A"])

    def test_lane_lock_taken_held_and_released_also_on_an_error(self):
        write_session(self.d, lane=1, lock_wait=5)
        taken, closed = [], []

        def boom(d):
            w = live.read_json(live.watchdog_file(d))
            self.assertEqual((w["lock"], w["pid"], w["ended"]), ("held", 4242, None))
            raise RuntimeError("bridge gone")
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait: taken.append((n, o, wait)) or True,
                                 watcher=boom, closer=lambda d, s: closed.append(s["lane"]) or {"lock": "released"},
                                 pid=4242)
        self.assertEqual(code, 0)
        self.assertEqual(taken, [(1, live.lock_owner(self.d), 5)])
        self.assertEqual(closed, [1])
        w = live.read_json(live.watchdog_file(self.d))
        self.assertIn("watchdog error: RuntimeError: bridge gone", w["ended"])
        self.assertEqual(w["closed"], {"lock": "released"})

    def test_a_signal_still_cleans_up(self):
        write_session(self.d)
        closed = []

        def signalled(d):
            raise SystemExit("signal 15")
        live.run_watchdog(self.d, watcher=signalled, closer=lambda d, s: closed.append(1) or {}, pid=1)
        self.assertEqual(closed, [1])
        self.assertIn("watchdog stopped (signal 15)", live.ended(self.d))

    def test_a_busy_lane_refuses_the_session(self):
        write_session(self.d, lane=1)
        closed = []
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait: False, watcher=lambda d: "x",
                                 closer=lambda d, s: closed.append(1), pid=1)
        self.assertEqual(code, 1)
        self.assertEqual(closed, [])   # nothing started, nothing to close
        w = live.read_json(live.watchdog_file(self.d))
        self.assertEqual(w["lock"], "refused")
        self.assertIn("lane 1 is not free", w["ended"])

    def test_commands_refused_after_the_end(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d),
                        {"pid": 1, "lock": "none", "ended": "maximum duration (30 min) reached"})
        with self.assertRaises(SystemExit) as e:
            live.state(argparse.Namespace(session=self.d, player=None))
        self.assertIn("maximum duration", str(e.exception))

    def test_stop_without_a_watchdog_closes_here(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d), {"pid": 999999, "lock": "none", "ended": None})
        old = live.close
        live.close = lambda d, s, alive=None: {"A": "quit asked"}
        try:
            out = live.stop(argparse.Namespace(session=self.d), alive=lambda p: False)
        finally:
            live.close = old
        self.assertIn("without the watchdog", out["ended"])
        self.assertTrue(os.path.exists(live.stop_file(self.d)))


class FakeBridge:
    def __init__(self, x, shots):
        self.x, self.shots = x, shots

    def request(self, cmd, **kw):
        if cmd == "shot":
            self.shots.append((self.x, kw["path"]))
            return {}
        return {"map": "ForestTemple", "game": {}}

    def close(self):
        pass


class LiveCheckpoint(unittest.TestCase):
    def test_a_checkpoint_has_a_shot_of_each_game(self):
        with tempfile.TemporaryDirectory() as d:
            write_session(d)
            shots = []

            class Player:
                def __init__(self, b, log, commands=None, **kw):
                    self.commands = commands

                def play(self, steps):
                    with open(self.commands + ".dump-%s.json" % steps[0].name, "w", encoding="utf-8") as f:
                        json.dump({"flags": {"F": 1}}, f)
            saved = live.bridge, hookshot.Player, hookshot.Log
            live.bridge = lambda s, x: FakeBridge(x, shots)
            hookshot.Player = Player
            hookshot.Log = lambda path: None
            try:
                out = live.checkpoint(argparse.Namespace(session=d, tag="after_chest"))
            finally:
                live.bridge, hookshot.Player, hookshot.Log = saved
            cps = os.path.join(d, "checkpoints")
            self.assertEqual(shots, [("A", os.path.join(cps, "after_chest-A.png")),
                                     ("B", os.path.join(cps, "after_chest-B.png"))])
            self.assertEqual(out["A"]["shot"], os.path.join(cps, "after_chest-A.png"))
            cp = live.read_json(os.path.join(cps, "after_chest.json"))
            self.assertEqual(cp["B"]["shot"], os.path.join(cps, "after_chest-B.png"))
            self.assertEqual(cp["A"]["flags"], {"F": 1})


class LiveMenu(unittest.TestCase):
    """live.py state's `menu`: co-op's pages as text from the play-test bridge (a bridge shot leaves them out)."""

    class Fake:
        def __init__(self, reply=None, error=None):
            self.reply, self.error, self.asked, self.closed = reply, error, [], False

        def request(self, cmd):
            self.asked.append(cmd)
            if self.error:
                raise self.error
            return self.reply

        def close(self):
            self.closed = True

    def test_the_menu_member_of_the_state(self):
        menu = {"coop": {"open": True, "page": "customize"}, "customize_screen": {"open": False}}
        f = self.Fake({"ok": True, "frame": 3, "menu": menu})
        self.assertEqual(live.menu_of(5001, f), {"menu": menu})
        self.assertEqual(f.asked, ["state"])
        self.assertTrue(f.closed)

    def test_an_older_module_says_so(self):
        self.assertIn("no menu", live.menu_of(5001, self.Fake({"ok": True}))["menu_error"])

    def test_no_bridge_is_an_error_not_a_crash(self):
        f = self.Fake(error=ConnectionRefusedError("refused"))
        self.assertIn("refused", live.menu_of(5001, f)["menu_error"])
        self.assertTrue(f.closed)

    def test_a_game_without_the_bridge_is_not_asked(self):
        with tempfile.TemporaryDirectory() as d:
            s = {"games": {"A": {}}}
            asked = []
            orig = (live.bridge, live.snapshot, live.new_lines, live.menu_of)
            try:
                live.bridge = lambda s, x: type("B", (), {"close": lambda self: None})()
                live.snapshot = lambda b: {"map": "m"}
                live.new_lines = lambda d, s, x: []
                live.menu_of = lambda port: asked.append(port) or {"menu": {}}
                self.assertNotIn("menu", live.look(d, s, "A"))
                s["games"]["A"]["playtest_port"] = 5011
                self.assertEqual(live.look(d, s, "A")["menu"], {})
                self.assertEqual(asked, [5011])
            finally:
                live.bridge, live.snapshot, live.new_lines, live.menu_of = orig


class FakeTalk:
    """A game seen through the bridge for `talk`: an NPC whose talk is a list of boxes ("box", speaker, text) and
    choices ("choice", question, [options]). A shows the next one; Down moves the choice cursor (from 0). The texts go
    in a `text` log as patch 0037's wrap keeps them (one entry per pane set, the same text again not kept), laid out as
    a lane run read them: MessageDialog_00 (T_Name_00 the speaker), the reply buttons PaMessageBtn_00 (one pane,
    T_Btn_00, set once per button). `body` False: the box text does not come through the wrap (as in that run)."""

    def __init__(self, talk, hooked=True, verb=True, prompt="Talk", body=True, body_pane=None):
        self.talk, self.hooked, self.verb, self.prompt, self.body = talk, hooked, verb, prompt, body
        self.body_pane = body_pane   # (a pane name: the box text comes through patch 0038's wrap 26 in that pane)
        self.at, self.event, self.cursor, self.picked = None, False, 0, []
        self.frame, self.texts, self.shown, self.presses = 0, [], {}, []
        self.texts.append(self._entry("HUD_00", "T_Rupee_00", "5"))   # (a text before the talk: not a box)

    def _entry(self, layout, pane, text, wrap=25):
        return {"seq": len(self.texts) + 1, "frame": self.frame, "layout": layout, "pane": pane, "text": text,
                "length": len(text), "cut": False, "markup": False, "wrap": wrap}

    def _set(self, pane, text, layout="MessageDialog_00", wrap=25):
        if self.shown.get((layout, pane)) != text:
            self.shown[(layout, pane)] = text
            self.texts.append(self._entry(layout, pane, text, wrap))

    def _box(self, text):
        if self.body_pane:
            self._set(self.body_pane, text, wrap=26)
        elif self.body:
            self._set("T_MessageWhiteOut_00", text)

    def _show(self):
        item = self.talk[self.at]
        if item[0] == "box":
            self._set("T_Name_00", item[1])
            self._box(item[2])
        else:
            self._box(item[1])
            for option in item[2]:
                self._set("T_Btn_00", option, "PaMessageBtn_00")
            self.cursor = 0

    def state(self):
        return {"ok": True, "scene": "Kokiri", "loading": False, "event": self.event,
                "prompt": None if self.event else self.prompt}

    def step(self, frames=1):
        self.frame += frames
        return self.state()

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        self.presses.extend(buttons)
        self.frame += frames
        if "DOWN" in buttons and self.event and self.talk[self.at][0] == "choice":
            self.cursor = min(self.cursor + 1, len(self.talk[self.at][2]) - 1)
        if "A" in buttons:
            if not self.event and self.prompt:
                self.event, self.at = True, 0
                self._show()
            elif self.event:
                if self.talk[self.at][0] == "choice":
                    self.picked.append(self.talk[self.at][2][self.cursor])
                self.at += 1
                if self.at < len(self.talk):
                    self._show()
                else:
                    self.event = False
                    self._box("")
        return {"ok": True}

    def text(self, since=0, layout=None):
        if not self.verb:
            raise pc.BridgeError({"ok": False, "error": "bad_request", "message": "cmd: hello, ..."})
        texts = [t for t in self.texts if t["seq"] > since and (not layout or layout in t["layout"])] if self.hooked else []
        return {"ok": True, "hooked": self.hooked, "last": len(self.texts) if self.hooked else 0, "lost": False,
                "texts": texts}


# Placeholders only: no text from the game (AGENTS.md).
SHOP = [("box", "Keeper", "Line 1"), ("choice", "Item: 40", ["Pick one", "Pick two"]),
        ("choice", "Sure?", ["Yes", "No"]), ("box", "Keeper", "Line 2")]


class TalkTest(unittest.TestCase):
    def test_reads_every_box_with_its_speaker_and_ends(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker A", "Line 2")])
        self.assertEqual(r["speakers"], ["Speaker A"])
        self.assertFalse(g.event)
        self.assertNotIn("5", [b["text"] for b in r["boxes"]])     # (the HUD's text before the talk)

    def test_without_the_body_text_it_still_ends_with_the_speakers(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2"), ("box", "Speaker B", "x")],
                     body=False)
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual(r["boxes"], [])
        self.assertEqual(r["speakers"], ["Speaker A", "Speaker B"])
        self.assertFalse(g.event)

    def test_a_box_from_the_body_wrap_is_read_whatever_its_pane(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")], body_pane="T_Pane_X")
        g._set("T_Pane_Y", "Not a box", layout="HUD_00", wrap=26)    # (a body-wrap text outside a message layout)
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker A", "Line 2")])

    def test_picks_each_reply_by_its_text_in_order(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g, reply=["pick TWO", "Yes"])
        self.assertTrue(r["done"], r)
        self.assertEqual(g.picked, ["Pick two", "Yes"])
        self.assertEqual(r["picked"], [{"text": "Pick two", "index": 1}, {"text": "Yes", "index": 0}])
        self.assertEqual(g.presses.count("DOWN"), 1)
        self.assertEqual(r["boxes"][-1]["text"], "Line 2")

    def test_choices_with_no_reply_left_stop_and_list_them(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g)
        self.assertFalse(r["done"])
        self.assertIn("no reply is left", r["result"])
        self.assertEqual(r["choices"], ["Pick one", "Pick two"])
        self.assertEqual(g.picked, [])                     # (nothing bought blindly)
        self.assertTrue(g.event)                           # (left open for the tester to decide)
        self.assertIsNotNone(r["since"])
        # going on from there: the same buttons read again, no new A first
        r2 = ph.talk(g, reply="Pick one", since=r["since"])
        self.assertFalse(r2["pressed"])
        self.assertEqual(r2["picked"], [{"text": "Pick one", "index": 0}])
        self.assertIn("no reply is left", r2["result"])
        self.assertEqual(r2["choices"], ["Yes", "No"])
        r3 = ph.talk(g, reply="No", since=r2["since"])
        self.assertTrue(r3["done"], r3)
        self.assertEqual(g.picked, ["Pick one", "No"])

    def test_a_reply_not_offered_stops_and_lists_the_choices(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g, reply="Other")
        self.assertFalse(r["done"])
        self.assertIn("reply not offered", r["result"])
        self.assertIn("'Pick one'", r["result"])
        self.assertEqual(g.picked, [])
        self.assertTrue(g.event)

    def test_a_reply_with_no_choices_is_not_done(self):
        r = ph.talk(FakeTalk([("box", "Speaker A", "Line 1")]), reply="Yes")
        self.assertFalse(r["done"])
        self.assertIn("no choices showed", r["result"])

    def test_no_prompt_presses_nothing(self):
        g = FakeTalk(SHOP, prompt=None)
        r = ph.talk(g)
        self.assertFalse(r["pressed"])
        self.assertEqual(g.presses, [])

    def test_an_id_with_the_prompt_already_up_does_not_walk(self):
        g = FakeTalk([("box", "Speaker A", "Line 1")])
        r = ph.talk(g, ident=5, name="Npc_X")             # (FakeTalk has no world: a walk would fail)
        self.assertTrue(r["done"], r)
        self.assertNotIn("walk", r)

    def test_without_the_wrap_or_the_verb_one_a_and_why(self):
        for g, why in ((FakeTalk(SHOP, hooked=False), "patch 0037"), (FakeTalk(SHOP, verb=False), "no `text` verb")):
            r = ph.talk(g)
            self.assertTrue(r["pressed"])
            self.assertEqual(g.presses, ["A"])
            self.assertIn(why, r["result"])
            self.assertFalse(r["done"])

    def test_bounded_by_boxes_and_frames(self):
        r = ph.talk(FakeTalk([("box", "Speaker A", "Line %d" % i) for i in range(40)]), max_boxes=5)
        self.assertIn("too many boxes", r["result"])
        self.assertLessEqual(len(r["boxes"]), 6)
        r = ph.talk(FakeTalk([("box", "K", "Line %d" % i) for i in range(400)]), max_boxes=1000, max_frames=200)
        self.assertIn("timed out", r["result"])

    def test_a_box_that_grows_is_one_box(self):
        g = FakeTalk([("box", "Speaker A", "Line 1")])
        orig = g._show

        def typed():
            g._set("T_Name_00", "Speaker A")
            g._set("T_MessageWhiteOut_00", "Li")
            orig()
        g._show = typed
        r = ph.talk(g)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 1"])

    def test_the_mcp_tools_take_reply_and_since(self):
        names = {t["name"]: t for t in mcp_playtest.PLAYER_TOOLS}
        props = names["talk"]["inputSchema"]["properties"]
        self.assertIn("reply", props)
        self.assertIn("since", props)
        self.assertIn("since", names["get_text"]["inputSchema"]["properties"])


if __name__ == "__main__":
    unittest.main()
