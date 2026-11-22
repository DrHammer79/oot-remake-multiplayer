"""The play-test bridge's proof in two running games (docs/playtest.md, "Bridge protocol").

Talks to both games' bridges (standard library only) and checks, in this order, so a failed hold is known first:
  1. hold: both paused for --hold seconds (past the server's 10 s silence timeout); the frame count and Link's place
     unchanged on both, and both still connected in co-op;
  2. step: 60 frames on each advance exactly 60;
  3. shot: one screenshot of each game;
  4. input: A walks (stick up, 30 frames): Link moved, and the frame advanced 30;
  5. warp: both to --scene; arrived, held;
  6. flag + dump: --flag set to 1 in A; after a few frames both dumps have it 1 (synced);
  7. give: bombs in A (needs LICHII_COOP_CHEATS=1);
  8. resume: both run again.
Each check prints PASS or FAIL with what was seen; the whole log is written as JSON to --out. Exit 0 all passed.

    python -I tools/autotest/bridge_proof.py --port-a 5011 --port-b 5012 --out <dir> [--hold 30]
        [--scene KokiriForest_Child] [--flag <a shared flag name>]
"""
import argparse
import json
import os
import socket
import sys
import time


class Bridge:
    def __init__(self, port, name):
        self.name = name
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=120)
        self.file = self.sock.makefile("rwb")
        self.next_id = 0

    def call(self, cmd, **fields):
        self.next_id += 1
        msg = dict(fields, cmd=cmd, id=self.next_id)
        self.file.write((json.dumps(msg) + "\n").encode())
        self.file.flush()
        line = self.file.readline()
        if not line:
            raise RuntimeError("%s: the bridge closed the connection" % self.name)
        reply = json.loads(line)
        if reply.get("id") != self.next_id:
            raise RuntimeError("%s: reply out of order: %r" % (self.name, reply))
        return reply

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def players_connected(state):
    players = ((state or {}).get("coop") or {}).get("players") or []
    return len(players) >= 2 and all(p.get("state") == "connected" for p in players)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port-a", type=int, required=True)
    ap.add_argument("--port-b", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hold", type=float, default=30.0)
    ap.add_argument("--scene", default="KokiriForest_Child")
    ap.add_argument("--flag", default="")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    log = []
    results = []

    def note(check, ok, seen):
        results.append({"check": check, "pass": bool(ok), "seen": seen})
        print("%s %s: %s" % ("PASS" if ok else "FAIL", check, json.dumps(seen)[:400]), flush=True)

    A, B = Bridge(a.port_a, "A"), Bridge(a.port_b, "B")
    games = {"A": A, "B": B}

    def call(g, cmd, **f):
        r = games[g].call(cmd, **f)
        log.append({"game": g, "cmd": cmd, "fields": f, "reply": r, "t": time.time()})
        return r

    try:
        for g in games:
            r = call(g, "hello")
            note("hello " + g, r.get("ok") and r.get("protocol") == 1 and r.get("control"), r)

        # 1. the hold, first
        before = {}
        for g in games:
            r = call(g, "pause")
            note("pause " + g, r.get("ok") and r.get("paused"), r)
            before[g] = call(g, "state")
        time.sleep(a.hold)
        for g in games:
            s = call(g, "state")
            b = before[g]
            same = s.get("frame") == b.get("frame") and s.get("pos") == b.get("pos")
            note("hold %ds %s: frame and place unchanged" % (a.hold, g), same and s.get("paused"),
                 {"frame": [b.get("frame"), s.get("frame")], "pos": [b.get("pos"), s.get("pos")]})
            note("hold %s: co-op still connected" % g, players_connected(s), (s.get("coop") or {}))

        # 2. step exact
        for g in games:
            r = call(g, "step", frames=60)
            note("step 60 " + g, r.get("ok") and r.get("frame") - r.get("from", -1) == 60 and r.get("paused"), r)

        # 3. shots
        for g in games:
            r = call(g, "shot", name="proof-hold-" + g)
            note("shot " + g, r.get("ok") and os.path.isfile(r.get("path", "")), r)

        # 4. input moves Link
        s0 = call("A", "state")
        r = call("A", "input", stick=[0, 1.0], frames=30)
        s1 = r.get("state") or {}
        p0, p1 = s0.get("pos"), s1.get("pos")
        moved = bool(p0 and p1) and abs(p0[0] - p1[0]) + abs(p0[2] - p1[2]) > 0.3
        note("input stick 30 frames A: Link moved, frame +30",
             r.get("ok") and moved and r.get("frame") - r.get("from", -1) == 30,
             {"from": p0, "to": p1, "frames": [r.get("from"), r.get("frame")], "camera": s1.get("camera"),
              "facing": [s0.get("facing"), s1.get("facing")]})

        # 5. warp both
        for g in games:
            r = call(g, "warp", scene=a.scene, timeout=60)
            st = r.get("state") or {}
            note("warp %s %s" % (a.scene, g), r.get("ok") and r.get("arrived") and st.get("scene") == a.scene and r.get("paused"),
                 {"ok": r.get("ok"), "error": r.get("error"), "scene": st.get("scene"), "frame": r.get("frame")})

        # 6. flag in A, synced to B (both stepped), dumps agree
        if a.flag:
            r = call("A", "flag", name=a.flag, value=1)
            note("flag %s=1 A" % a.flag, r.get("ok"), r)  # (the queue applies it at the next frame: the dumps prove it)
            for _ in range(6):  # ~3 s of game, both stepped the same
                for g in games:
                    call(g, "step", frames=30)
            dumps = {}
            for g in games:
                r = call(g, "dump", tag="proof-flag")
                ok = r.get("ok") and os.path.isfile(r.get("path", ""))
                dumps[g] = json.load(open(r["path"], encoding="utf-8")) if ok else {}
                note("dump " + g, ok and dumps[g].get("frame") == r.get("frame"), {"path": r.get("path"), "flags": r.get("flags")})
            fa = (dumps.get("A", {}).get("flags") or {}).get(a.flag)
            fb = (dumps.get("B", {}).get("flags") or {}).get(a.flag)
            note("flag %s synced: A and B dumps both 1" % a.flag, fa == 1 and fb == 1, {"A": fa, "B": fb})

        # 7. give
        r = call("A", "give", item="bombs")
        call("A", "step", frames=2)
        s = call("A", "state")
        bombs = (s.get("inventory") or {}).get("PouchData.Item_Bomb.StockNum")
        note("give bombs A", r.get("ok") and (bombs or 0) > 0, {"reply": r, "bombs": bombs})

        for g in games:
            r = call(g, "shot", name="proof-end-" + g)
            note("shot end " + g, r.get("ok") and os.path.isfile(r.get("path", "")), {"path": r.get("path")})
    except Exception as e:  # (a broken connection, a refused reply: a FAIL, and the games are let go below)
        note("exception", False, repr(e))
    finally:
        for g in games:
            try:
                r = call(g, "resume")
                note("resume " + g, r.get("ok") and not r.get("paused"), r)
            except Exception as e:
                note("resume " + g, False, repr(e))
            games[g].close()
        with open(os.path.join(a.out, "bridge-proof.json"), "w", encoding="utf-8") as f:
            json.dump({"results": results, "log": log}, f, indent=1)
    failed = [x["check"] for x in results if not x["pass"]]
    print("bridge proof: %d checks, %d failed%s" % (len(results), len(failed), (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
