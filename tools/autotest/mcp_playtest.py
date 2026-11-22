"""An MCP server (stdio) that gives Claude Code the controls of a live play-test session (tools/autotest/live.py).

Register it once from the repository folder, pointing at a session started with `live.py start`:

    claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <DIR>

The tools act on the session's two games (A and B): look (`state`), act (`act`: test script lines), see (`shot`),
checkpoint both games' shared flags (`checkpoint`) and compare them (`desync_check`). The MCP protocol is spoken by
hand (newline-delimited JSON-RPC 2.0 on stdin/stdout: initialize, tools/list, tools/call), standard library only.
"""
import argparse
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desync_check  # noqa: E402
import live  # noqa: E402

SCRIPT_HELP = (
    "Test script lines (tools/autotest/hookshot.py's format), ';' between them. Times are game frames (60 a second). "
    "Movement: `goto X Z TOLERANCE FRAMES` (walk to a point, metres), `toward X Z FRAMES [BUTTONS]` (walk that way, "
    "holding buttons), `stick LX LY FRAMES`, `wait FRAMES`. Buttons: `press A 8`, `hold ZL+B 30` (A B X Y L R ZL ZR "
    "PLUS MINUS UP DOWN LEFT RIGHT). Co-op test commands (need LICHII_COOP_CHEATS=1): `warp SCENE` (e.g. "
    "ForestTemple, SacredForestMeadow_Adult), `place X Y Z`, `face X Z`, `give ITEM`, `setflag NAME VALUE`, "
    "`kill ACTOR`, `hit ACTOR N`, `hurt N`, `chat TEXT`, `mark NAME`.")

TOOLS = [
    {"name": "state", "description": "What a game shows now: map, Link's position [x,y,z] in metres, facing and "
     "camera yaw (0 = +z, 90 = +x), life and max life (quarter hearts), rupees, age, whether an event or a load is "
     "running, the A-button prompt, and the new co-op log lines since the last look. Both games without `player`.",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]}}}},
    {"name": "act", "description": "Play test script lines in one game, then return its state. " + SCRIPT_HELP,
     "inputSchema": {"type": "object", "required": ["player", "script"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]},
                                    "script": {"type": "string"}}}},
    {"name": "shot", "description": "Screenshot one game or both: the PNG paths (open them to see the screen). "
     "Use sparingly; state is cheaper.",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]}}}},
    {"name": "checkpoint", "description": "Write both games' shared dungeon and story flags and a screenshot of "
     "each game under a tag (letters, digits, - and _). Take one after every action that should sync, then run "
     "desync_check; open the shots for anything visible (flags alone are no proof of what a player sees).",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "desync_check", "description": "Compare every checkpoint of the session: the first tag where the two "
     "games' shared flags differ, and which flags.",
     "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}}}},
]


class Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def call(session, name, args):
    if name == "state":
        return live.state(Args(session=session, player=args.get("player")))
    if name == "act":
        return live.do(Args(session=session, player=args["player"], script=args["script"]))
    if name == "shot":
        return live.shot(Args(session=session, player=args.get("player")))
    if name == "checkpoint":
        return live.checkpoint(Args(session=session, tag=args["tag"]))
    if name == "desync_check":
        buf = io.StringIO()
        code = desync_check.check(session, bool(args.get("all")), out=lambda s: buf.write(s + "\n"))
        return {"result": ["agree", "desync", "nothing to compare", "incomplete"][code], "report": buf.getvalue()}
    raise ValueError("no tool %s" % name)


def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def serve(session):
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        method, id_ = req.get("method"), req.get("id")
        if id_ is None:
            continue  # a notification (initialized, cancelled): nothing to answer
        if method == "initialize":
            reply(id_, {"protocolVersion": (req.get("params") or {}).get("protocolVersion", "2024-11-05"),
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "oot-playtest", "version": "1"}})
        elif method == "tools/list":
            reply(id_, {"tools": TOOLS})
        elif method == "tools/call":
            params = req.get("params") or {}
            try:
                out = call(session, params.get("name"), params.get("arguments") or {})
                reply(id_, {"content": [{"type": "text", "text": json.dumps(out, indent=1)}]})
            except (Exception, SystemExit) as e:  # (SystemExit from a bad tag too: an error the tester reads)
                reply(id_, {"content": [{"type": "text", "text": "%s: %s" % (e.__class__.__name__, e)}],
                            "isError": True})
        elif method == "ping":
            reply(id_, {})
        else:
            reply(id_, error={"code": -32601, "message": "no method %s" % method})


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--session", required=True)
    a = p.parse_args(argv)
    serve(os.path.abspath(a.session))
    return 0


if __name__ == "__main__":
    sys.exit(main())
