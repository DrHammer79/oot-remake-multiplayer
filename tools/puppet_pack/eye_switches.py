"""Prints the switch table of lichii/hook/coop_eye_switch.cpp (kSwitches).

Run by a developer against their own romfs dump; nothing from the game is in
this tool:

    python -m puppet_pack.eye_switches --romfs <romfs> [--places lichii/hook/coop_live_places.cpp]
                                       [--dungeon lichii/hook/coop_dungeon.cpp] [--classify]

A room's gimmick event is asked by its EventRequestActor when the actor's
input `EventRequestTrigger` rises. That input is wired in the room's logic
graph (an AiGroup in Banc/<scene>/*.bcett: its own `Logic` and the nested
modules in its References), straight or through AND/OR gates and module
inputs, to a switch's output plug: `OnState` (eye, step, hit, hammer and dive
switches), `FirstOn`, `isHit` (the Forest Temple's silver eyes) or
`isLightMax` (sun switches). Another player's copy of the event, asked from
outside, plays its camera only: our asker never saw its trigger (deku-ladder,
tracker rows 90 and 102). So the hook sets that plug on OUR switch instead.

This tool follows the wiring (the graph's input links, its multi-input list,
and module inputs up to the calling graph) from every asker's trigger to the
switch plugs at its leaves, and prints one line per (event, switch): the event,
its scene, the switch's actor name and placement, the plug, and the asker's
placement. With --places only the events that have a live place (shared,
startable) are printed.

AI file layout read here (version 0x408), as far as needed:
  header +0x0c commands, +0x10 nodes, +0x24 string pool, +0x2c immediate
  parameters, +0x34 input/output parameters, +0x38 the multi-input list
  node 0x3c each after the commands (0x18 each): +0x08 type name, +0x14 body
  body: +0x18 the string immediates (index, count); +0x30 + 16*t the inputs
  (index, count) and outputs (index, count) of type t (int, bool, float,
  string, vec3, user)
  input entry: +0x00 name, then source node (s16) and its output (s16) at +4
  (+8 for user types); a source <= -100 is the multi-input list from entry
  (-100 - source), count = the output field; multi entry 8 bytes: node, output
  string immediate 0x0c: +0x00 name, +0x08 value (InstanceName, Name)
  string input: +0x0c its default value (HoldGameDataGetBool's GameDataNameBool: the flag's name)
"""
import argparse
import glob
import os
import re
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402

TYPES = ("int", "bool", "float", "string", "vec3", "udt")
IN_SIZE = {"int": 0x10, "bool": 0x10, "float": 0x10, "string": 0x10, "vec3": 0x18, "udt": 0x14}
OUT_SIZE = {"int": 4, "bool": 4, "float": 4, "string": 4, "vec3": 4, "udt": 8}
# Switch actors whose output starts an event (not the shutters they open).
SWITCH = re.compile(r"^Obj_(?:[A-Za-z]+_)*(?:EyeSwitch|StepSwitch|SwitchHit|HammerSwitch|DiveSwitch|SolarSwitch)(?:_|$)")


def read_graph(data):
    """[node]: {"type", "imm": {name: str}, "in": {type: [(name, src, out)]}, "out": {type: [name]}}, multi(base, n)."""
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]  # noqa: E731
    s16 = lambda o: struct.unpack_from("<h", data, o)[0]  # noqa: E731
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]  # noqa: E731
    pool = u32(0x24)

    def text(o):
        return data[pool + o: data.index(b"\0", pool + o)].decode("utf-8", "replace")

    commands, count = u32(0x0C), u32(0x10)
    io, multi_at, imm = u32(0x34), u32(0x38), u32(0x2C)
    starts = [u32(io + 4 * i) for i in range(12)] + [multi_at]
    ins, outs, defaults = {}, {}, []
    for t, name in enumerate(TYPES):
        lst, o = [], starts[2 * t]
        while o < starts[2 * t + 1]:
            at = o + (8 if name == "udt" else 4)
            lst.append((text(u32(o) & 0x3FFFFFFF), s16(at), s16(at + 2)))
            if name == "string":  # its default value (a flag's name) at +0x0c
                defaults.append(text(u32(o + 12)))
            o += IN_SIZE[name]
        ins[name] = lst
        lst, o = [], starts[2 * t + 1]
        while o < starts[2 * t + 2]:
            lst.append(text(u32(o) & 0x3FFFFFFF))
            o += OUT_SIZE[name]
        outs[name] = lst
    string_imm = u32(imm + 12)
    nodes = []
    for i in range(count):
        n = 0x74 + commands * 0x18 + i * 0x3C
        body = u32(n + 0x14)
        node = {"type": text(u32(n + 8)), "in": {}, "out": {}, "imm": {}, "sdef": {}}
        for t, name in enumerate(TYPES):
            a, b = u32(body + 0x30 + 16 * t), u32(body + 0x34 + 16 * t)
            c, d = u32(body + 0x38 + 16 * t), u32(body + 0x3C + 16 * t)
            node["in"][name] = ins[name][a: a + b]
            node["out"][name] = outs[name][c: c + d]
            if name == "string":
                node["sdef"] = {k[0]: v for k, v in zip(ins[name][a: a + b], defaults[a: a + b])}
        first, n_str = u32(body + 0x18), u32(body + 0x1C)
        for k in range(first, first + n_str):
            o = string_imm + 0x0C * k
            node["imm"][text(u32(o))] = text(u32(o + 8))
        nodes.append(node)

    def multi(base, n):
        return [(u16(multi_at + 8 * (base + k)), u16(multi_at + 8 * (base + k) + 2)) for k in range(n)]

    return nodes, multi


def _int(v):
    return int(v.v if hasattr(v, "v") else v)


def _place(actor):
    return tuple(round(float(x), 1) for x in (actor.get("Translate") or [0, 0, 0]))


def _prefix(path):
    return path.rsplit("/", 1)[0] + "/" if "/" in path else ""


# A switch's inputs that put it ON through its own AI (deku-ladder, A #39): a forced, first-on or cleared input, or
# BootUp where the room graph boots the switch from its saved flag.
ON_INPUTS = ("ForceControl", "FirstOn", "isCleared", "IsSkipLight", "Active", "BootUp")
# The asker's outputs a room result can hang on (Dead and Logic_Alive are its own life).
ASKER_OUTPUTS = ("EventBool", "EventBoolEX1", "EventBoolEX2", "EventFinish")
# Results on the asker's outputs that change nothing kept by the room: a save prompt, Navi's balloon, the dungeon
# room step (DungeonRoomStep.*: counted by the map, not drawn: reports/dungeon-sync-b.md).
COSMETIC = ("TriggerRequestAutoSave", "TriggerNaviRequestBallonMessage", "TriggerDungeonRoomStep")
# Actor inputs fed by the asker that only play the event's moment: a chest's appear act (the chest itself is created
# by the shared flag), and the event's own air walls and areas (made for the event; after a reload they are as before
# it, so the game itself keeps nothing of them).
TRANSIENT_INPUTS = ("isSpawnAct", "IsSpawnAct")
TRANSIENT_ACTORS = re.compile(r"AirWall|^Area$")


def switch_events(romfs, dicts=None, wiring=None):
    """(lines, guards, extras), each sorted.

    lines:  (event, scene, switch name, switch place, plug, asker place)
    guards: (flag, scene, event, why) - a saved flag that keeps the event from starting in our game when it comes
            from another player first: one that deletes the asker (why "deletes the asker"), or one the trigger reads
            negated (why "the trigger reads it negated")
    extras: (event, scene, actor name, actor place, plug) - another actor's plug the trigger ANDs with the switch
    others: (event, scene, what) - any other input on the trigger (a positive flag, a player state, ...), for notes
    wiring: when a dict, filled per line (the lines' tuple) with {"on": [(input, [(kind, what, negated)])] (the
            switch's ON inputs and their sources, [] = unwired), "results": [(kind, what, detail)] (what the asker's
            outputs drive, forward through gates, module outputs and module calls), "delete": [flags that delete the
            asker]}. classify() reads it.
    """
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = read_graph(open(path, "rb").read()) if os.path.exists(path) else None
        return graphs[path]

    lines, guards, extras, others = set(), set(), set(), set()
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in glob.glob(os.path.join(folder, "*.bcett.byml.zs")):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""), None)]
            mods += [(_prefix(r["Path"]), r["Logic"], r.get("InstanceName")) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): by_hash[_int(r["Reference"])]
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}

            def leaves(pre, g_, src, outp, depth, seen, neg=False, under_or=False):
                """[(kind, actor or flag name, plug, negated, under an OR)], kind "actor", "flag" or "node"."""
                nodes, multi = g_
                if src <= -100:
                    found = []
                    for a, b in multi(-100 - src, outp):
                        found += leaves(pre, g_, a, b, depth + 1, seen, neg, under_or)
                    return found
                if src < 0 or src >= len(nodes) or depth > 12 or (id(g_), src, outp) in seen:
                    return []
                seen.add((id(g_), src, outp))
                node = nodes[src]
                plugs = node["out"]["bool"]
                plug = plugs[outp] if 0 <= outp < len(plugs) else ""
                inst = node["imm"].get("InstanceName")
                if inst and (pre, inst) in by_id:
                    return [("actor", by_id[(pre, inst)], plug, neg, under_or)]
                if not node["type"] and "Name" in node["imm"]:  # a module input: follow it in the calling graph
                    found = []
                    for mpre, logic, minst in mods:
                        if mpre != pre or minst is None:
                            continue
                        mtype = os.path.basename(str(logic))[: -len(".ain")]
                        for ppre, plogic, _ in mods:
                            parent = graph(plogic) if plogic and ppre != pre and pre.startswith(ppre) else None
                            if parent is None:
                                continue
                            for call in parent[0]:
                                if call["type"] == mtype and call["imm"].get("InstanceName") == minst:
                                    for name, a, b in call["in"]["bool"]:
                                        if name == node["imm"]["Name"]:
                                            found += leaves(ppre, parent, a, b, depth + 1, seen, neg, under_or)
                    return found
                if node["type"].startswith("HoldGameDataGetBool"):
                    name = node["sdef"].get("GameDataNameBool", "")
                    return [("flag", name, plug, neg, under_or)] if name else []
                if not node["in"]["bool"]:
                    return [("node", node["type"], plug, neg, under_or)]
                # A gate, or a module call (InvBool, ...): its False output (or an inverter) negates.
                flip = plug == "False" or "InvBool" in node["type"]
                is_or = "OR" in node["type"].upper().replace("OPERATE", "")[:2] or node["type"].startswith("OperateOR")
                found = []
                for _, a, b in node["in"]["bool"]:
                    found += leaves(pre, g_, a, b, depth + 1, seen, neg != flip, under_or or is_or)
                return found

            def calls(pre, name, inner):
                """[(graph, pre, node index, output index)] where a module port continues: inner False: `pre`'s
                module output `name` -> the calls' outputs in the calling graphs; inner (an input name): the call
                node `name` -> that input's node in the called module."""
                found = []
                if inner:
                    call = name
                    for mpre, logic, minst in mods:
                        sub = graph(logic) if logic and mpre != pre and mpre.startswith(pre) else None
                        if sub is None or minst != call["imm"].get("InstanceName") or \
                                os.path.basename(str(logic))[: -len(".ain")] != call["type"]:
                            continue
                        for k, n in enumerate(sub[0]):
                            if not n["type"] and not n["in"]["bool"] and n["imm"].get("Name") == inner:
                                found.append((sub, mpre, k, 0))
                    return found
                for mpre, logic, minst in mods:
                    if mpre != pre or minst is None:
                        continue
                    mtype = os.path.basename(str(logic))[: -len(".ain")]
                    for ppre, plogic, _ in mods:
                        parent = graph(plogic) if plogic and ppre != pre and pre.startswith(ppre) else None
                        if parent is None:
                            continue
                        for k, call in enumerate(parent[0]):
                            if call["type"] == mtype and call["imm"].get("InstanceName") == minst and \
                                    name in call["out"]["bool"]:
                                found.append((parent, ppre, k, call["out"]["bool"].index(name)))
                return found

            def forward(pre, g_, src, outp, depth, seen):
                """What the plug (src, outp) drives: [("actor", actor, input) | ("flag", name, "set"/"clear") |
                ("node", type, input)], through gates, delays, module outputs and module calls. An actor's detail is
                (input, [(flag or actor name, negated, under an OR)] that input reads)."""
                nodes, multi = g_
                found = []
                if depth > 12:
                    return found
                for j, m in enumerate(nodes):
                    for name, a, b in m["in"]["bool"]:
                        if (src, outp) not in (multi(-100 - a, b) if a <= -100 else [(a, b)]):
                            continue
                        if (id(g_), j, name) in seen:
                            continue
                        seen.add((id(g_), j, name))
                        inst = m["imm"].get("InstanceName")
                        if inst and (pre, inst) in by_id:
                            # what else that input reads (a flag or a switch beside the asker): the flags arriving
                            # from the other game may already drive it
                            also = tuple(sorted({(w if k == "flag" else str(w.get("Gyaml")), ng, uo)
                                                 for k, w, _, ng, uo in leaves(pre, g_, a, b, 0, set())
                                                 if k in ("flag", "actor")}))
                            found.append(("actor", by_id[(pre, inst)], (name, also)))
                        elif m["type"].startswith("TriggerGameDataSetBool"):
                            found.append(("flag", m["sdef"].get("GameDataNameBool", ""),
                                          "set" if m["type"].endswith("True") else "clear"))
                        elif m["type"].endswith(".module") and inst and not calls(pre, m, name):
                            # a module call we cannot enter (not in this group): its input, like an actor's
                            also = tuple(sorted({(w if k == "flag" else str(w.get("Gyaml")), ng, uo)
                                                 for k, w, _, ng, uo in leaves(pre, g_, a, b, 0, set())
                                                 if k in ("flag", "actor")}))
                            found.append(("module", m["type"], (name, also)))
                        elif not m["type"] and "Name" in m["imm"]:  # a module output
                            for g2, p2, k, y in calls(pre, m["imm"]["Name"], False):
                                found += forward(p2, g2, k, y, depth + 1, seen)
                        else:
                            inner = calls(pre, m, name) if inst else []
                            for g2, p2, k, y in inner:
                                found += forward(p2, g2, k, y, depth + 1, seen)
                            if m["type"].startswith("OperateGameDataAlloc") or not m["out"]["bool"]:
                                found.append(("node", m["type"], name))
                            if not inner:
                                for y in range(len(m["out"]["bool"])):
                                    found += forward(pre, g_, j, y, depth + 1, seen)
                return found

            def switch_node(actor):
                """(pre, graph, node) of the switch actor's own logic node in this group."""
                for mpre, logic, _ in mods:
                    g2 = graph(logic) if logic else None
                    for n in (g2[0] if g2 else []):
                        if by_id.get((mpre, n["imm"].get("InstanceName"))) is actor:
                            return mpre, g2, n
                return None

            for pre, logic, _ in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for index, node in enumerate(g_[0]):
                    if node["type"] != "ActorLogicEventRequestActor":
                        continue
                    asker = by_id.get((pre, node["imm"].get("InstanceName")))
                    event = (asker or {}).get("Dynamic", {}).get("RequestEventName") if asker else None
                    if not event:
                        continue
                    trigger, delete = [], []
                    for name, a, b in node["in"]["bool"]:
                        if name == "EventRequestTrigger":
                            trigger = leaves(pre, g_, a, b, 0, set())
                        elif name == "Logic_Delete":
                            delete = leaves(pre, g_, a, b, 0, set())
                    switches = [t for t in trigger if t[0] == "actor" and SWITCH.match(str(t[1].get("Gyaml", ""))) and t[2]]
                    if not switches:
                        continue
                    event = str(event)
                    for _, actor, plug, _, _ in switches:
                        lines.add((event, scene, str(actor["Gyaml"]), _place(actor), plug, _place(asker)))
                    for kind, what, plug, neg, under_or in trigger:
                        if kind == "actor" and SWITCH.match(str(what.get("Gyaml", ""))):
                            continue
                        if kind == "flag" and neg:
                            guards.add((what, scene, event, "the trigger reads it negated"))
                        elif kind == "actor" and not neg and not under_or and plug:
                            extras.add((event, scene, str(what["Gyaml"]), _place(what), plug))
                        else:
                            label = str(what.get("Gyaml")) if kind == "actor" else what
                            others.add((event, scene, f"{'not ' if neg else ''}{label}.{plug}{' (in an OR)' if under_or else ''}"))
                    for kind, what, plug, neg, under_or in delete:
                        if kind == "flag" and not neg and not what.startswith("HavePlayedEvent."):
                            guards.add((what, scene, event, "deletes the asker"))
                    if wiring is None:
                        continue
                    results = []
                    for out in ASKER_OUTPUTS:
                        if out in node["out"]["bool"]:
                            results += forward(pre, g_, index, node["out"]["bool"].index(out), 0, set())
                    results = [(k, str(w.get("Gyaml")) if k == "actor" else w, d) for k, w, d in results
                               if not (k == "actor" and w is asker)]
                    deletes = sorted({w for k, w, _, ng, _ in delete if k == "flag" and not ng})
                    for _, actor, plug, _, _ in switches:
                        found = switch_node(actor)
                        on = []
                        if found:
                            spre, sg, sn = found
                            for name, a, b in sn["in"]["bool"]:
                                if name in ON_INPUTS:
                                    srcs = [] if a == -1 else leaves(spre, sg, a, b, 0, set())
                                    on.append((name, [(k, str(w.get("Gyaml")) if k == "actor" else w, ng)
                                                      for k, w, _, ng, _ in srcs]))
                        line = (event, scene, str(actor["Gyaml"]), _place(actor), plug, _place(asker))
                        wiring[line] = {"on": on, "results": sorted(set(results)), "delete": deletes}
    return sorted(lines), sorted(guards), sorted(extras), sorted(others)


def classify(line, wire, shared):
    """(class, fix, why) of a switch line whose plug does not latch (deku-ladder, A #39).

    class "A": every room result of the event reaches the room through a shared flag: the asker's outputs only set
    shared flags, or the actors they drive also read a shared flag (or the switch, itself on from a shared flag) in
    the same OR, or what they drive is the event's moment only (COSMETIC, TRANSIENT_*: after a reload the game itself
    shows it as before the event). Nothing to drive: the other player's flags (live, coop_dungeon) are the native
    path, and the room ends as the other player's own room does after a reload.
    class "B": a result hangs on our asker only (an actor input, an unshared flag, a dungeon step), so our asker must
    run the event. fix: "input <name>" - the switch's ON input is unwired: set it (the type-0 store) so the
    switch's own AI turns on and keeps its output; "split" - the ON input reads a shared flag that does not delete
    the asker: hold only the asker's delete flags (the ON flag comes, our switch turns on, our asker asks); "none" -
    the ON input's flag also deletes the asker (or no ON input): no native way, reported.
    """
    event, scene, switch, at, plug, asker_at = line
    on_flags = {w for _, srcs in wire["on"] for k, w, ng in srcs if k == "flag" and not ng and w in shared}
    misses = []
    for kind, what, detail in wire["results"]:
        if kind == "flag":
            if detail == "set" and what not in shared and "NaviHint" not in what:
                misses.append(f"unshared flag {what}")
        elif kind == "node":
            if not any(what.startswith(c) for c in COSMETIC) and detail not in TRANSIENT_INPUTS:
                misses.append(f"{what}.{detail}")
        else:  # an actor's or a module call's input
            name, also = detail
            if name in TRANSIENT_INPUTS or (kind == "actor" and TRANSIENT_ACTORS.search(what)):
                continue
            # the asker's own polarity on that input (a False output or an inverter flips it); not found (it is
            # behind a module port): any
            polarity = {ng for w, ng, _ in also if w == "EventRequestActor"}
            covered = any((uo or not polarity) and (not polarity or ng in polarity) and
                          (w in shared or (w == switch and on_flags)) for w, ng, uo in also)
            if not covered:
                misses.append(f"{what}.{name}")
    if not misses:
        return "A", "", "results through shared flags"
    misses = sorted(set(misses))
    forced = [n for n, srcs in wire["on"] if n != "BootUp" and n != "Active" and not srcs]
    wired = [w for n, srcs in wire["on"] if n in ("BootUp", "ForceControl", "FirstOn", "isCleared")
             for k, w, ng in srcs if k == "flag" and not ng]
    if wired and set(wired) <= shared and not set(wired) & set(wire["delete"]):
        return "B", "split", "; ".join(misses)
    if forced:
        return "B", f"input {forced[0]}", "; ".join(misses)
    return "B", "none", "; ".join(misses)


def live_places(path):
    """{(event, scene)} of lichii/hook/coop_live_places.cpp."""
    text = open(path, encoding="utf-8").read()
    return set(re.findall(r'\{"(\w+)", "(\w+)", [-\d.]+f', text))


def shared_flags(path):
    """The flag names of lichii/hook/coop_dungeon.cpp's tables (the flags another player's game sends us)."""
    text = open(path, encoding="utf-8").read()
    return set(re.findall(r'\{0x[0-9a-f]+, (?:true|false), "([A-Za-z0-9_.]+)"\}', text))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--places", help="coop_live_places.cpp: print only events that have a live place")
    ap.add_argument("--dungeon", help="coop_dungeon.cpp: print only guard flags that are shared")
    ap.add_argument("--classify", action="store_true",
                    help="print the A/B class of each line whose plug does not latch (needs --dungeon)")
    args = ap.parse_args(argv)
    keep = live_places(args.places) if args.places else None
    shared = shared_flags(args.dungeon) if args.dungeon else None
    wiring = {} if args.classify else None
    lines, guards, extras, others = switch_events(args.romfs, wiring=wiring)
    if args.classify:
        rows = []
        for line in lines:
            event, scene, switch, at, plug, _ = line
            if (keep is not None and (event, scene) not in keep) or switch.startswith("Obj_EyeSwitch") or \
                    plug == "FirstOn" or line not in wiring:
                continue
            rows.append((line, *classify(line, wiring[line], shared or set())))
        print("// kInputs (class B, the switch's ON input unwired: set it)")
        for (event, scene, switch, at, _, _), _, fix, why in rows:
            if fix.startswith("input "):
                print(f'    {{"{event}", "{scene}", "{switch}", {{{at[0]}f, {at[1]}f, {at[2]}f}}, "{fix[6:]}"}},  // {why}')
        print("// the other lines whose plug does not latch: class (fix) and why")
        for (event, _, switch, _, _, _), cls, fix, why in rows:
            if not fix.startswith("input "):
                print(f"//   {cls}{' (' + fix + ')' if fix else ''} {event} ({switch}): {why}")
        return 0
    wanted = {(e, s) for e, s, *_ in lines if keep is None or (e, s) in keep}
    print("// kSwitches")
    for event, scene, switch, s, plug, a in lines:
        if (event, scene) in wanted:
            print(f'    {{"{event}", "{scene}", "{switch}", {{{s[0]}f, {s[1]}f, {s[2]}f}}, "{plug}", '
                  f'{{{a[0]}f, {a[1]}f, {a[2]}f}}}},')
    print("// kExtras")
    for event, scene, actor, p, plug in extras:
        if (event, scene) in wanted:
            print(f'    {{"{event}", "{scene}", "{actor}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{plug}"}},')
    print("// kGuards")
    whys = {}
    for flag, scene, event, why in guards:
        if (event, scene) in wanted and (shared is None or flag in shared):
            whys.setdefault((flag, scene, event), set()).add(why)
    for (flag, scene, event), why in sorted(whys.items()):
        print(f'    {{"{flag}", "{scene}", "{event}"}},  // {" + ".join(sorted(why))}')
    print("// not covered (other inputs on a trigger: a guard flag that is not shared, a player state, an OR branch)")
    for event, scene, what in others:
        if (event, scene) in wanted:
            print(f"//   {event}: {what}")
    for flag, scene, event, why in guards:
        if (event, scene) in wanted and shared is not None and flag not in shared:
            print(f"//   {event}: {flag} ({why}; not shared: each game's own)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
