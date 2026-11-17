"""Prints the room-object tables of lichii/hook/coop_room_objects.cpp (kPlacements, kRiderNames).

Run by a developer against their own romfs dump; nothing from the game is in
this tool:

    python -m puppet_pack.room_objects --romfs <romfs>

The rule (room-objects, issues 196, 249, 194): in a game that follows another,
a room object moves only through its own logic, the input its own switch,
sensor or rider would set (CoopEyeSwitch's switch plug, CoopLiftRide's
RideOnPlayer, the push block's node), never by a word written into it from
outside. The clock path (coop_platforms.h) is for free-running movers only:
whose logic node in the room graph has no wired control input, and whose
logic type has no rider plug.

For every placement of a platform name (CoopPlatforms::IsDriver, the same
name rule as below) this prints one line: scene, name, placement, and whether
it is driven by its own logic and why: the wired input names (the room graph
feeds them from a switch, a flag, a sensor or an event) or `RideOnPlayer` (its
logic type has that plug: a rider lift). Inputs that only reset or warp it
(Logic_Create, Logic_Delete, RespawnTrigger, ForceWarp, IisForceWarp) do not
drive it. A placement with no logic node is free.

Then the names whose logic type has the bool output RideOnPlayer (the rider
lifts CoopLiftRide drives).
"""
import argparse
import glob
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402
from .eye_switches import SWITCH, _int, _place, _prefix, read_graph  # noqa: E402

PARTS = ("ElevatorLift", "_Elevator_", "MoveFloor", "RotateFloor", "RotateBlade", "MoveSpike",
         "ExtendingPlatform", "_lift_", "ElevatorMirror", "ElevatorPit", "UpDown")
NOT_DRIVING = {"Logic_Create", "Logic_Delete", "RespawnTrigger", "ForceWarp", "IisForceWarp"}
RIDER_PLUG = "RideOnPlayer"


def is_driver(name):
    """CoopPlatforms::IsDriver."""
    if name.startswith(("GroundControlActor_", "GrouondControlActor_")):
        return True
    if not name.startswith(("Obj_", "Land_")):
        return False
    return any(p in name for p in PARTS)


def wired_inputs(node):
    """The input names of a logic node that the graph feeds (not a constant, not reset-only)."""
    out = set()
    for t in ("bool", "int", "float", "vec3"):
        for name, src, _ in node["in"][t]:
            if name not in NOT_DRIVING and (src >= 0 or src <= -100):
                out.add(name)
    return out


def room_objects(romfs, dicts=None):
    """([(scene, name, place, why)], rider names): why is '' for a free placement."""
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = read_graph(open(path, "rb").read()) if os.path.exists(path) else None
        return graphs[path]

    rows, riders = {}, set()
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        why = {}
        for a in actors:
            name = str(a.get("Gyaml", ""))
            if is_driver(name) and "Hash" in a:
                why.setdefault(_int(a["Hash"]), set())
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""))] + [(_prefix(r["Path"]), r["Logic"]) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): _int(r["Reference"])
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}
            for pre, logic in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for node in g_[0]:
                    h = by_id.get((pre, node["imm"].get("InstanceName")))
                    if h is None or h not in why:
                        continue
                    why[h] |= wired_inputs(node)
                    if RIDER_PLUG in node["out"]["bool"]:
                        why[h].add(RIDER_PLUG)
                        riders.add(str(by_hash[h]["Gyaml"]))
        # A part with no logic of its own placed where a driven one is (the Deku Tree's ElevatorLiftLim_A_0n on its
        # ElevatorLift_A_0n) moves with it: driven too.
        driven = [(by_hash[h], r) for h, r in why.items() if r]
        for h, reasons in why.items():
            if reasons:
                continue
            p = _place(by_hash[h])
            for other, r in driven:
                q = _place(other)
                if sum((p[i] - q[i]) ** 2 for i in range(3)) <= 0.25:
                    reasons.add("with " + str(other["Gyaml"]))
        for h, reasons in why.items():
            a = by_hash[h]
            key = (scene, str(a["Gyaml"]), _place(a))
            rows[key] = "+".join(sorted(reasons | set(rows.get(key, "").split("+")) - {""}))
    return sorted((s, n, p, w) for (s, n, p), w in rows.items()), sorted(riders)


ASKERS = ("ActorLogicEventRequestActor", "ActorLogicEventProxyRequestActor")


def leaf_kind(leaf, eye_lines):
    """How a leaf's value reaches a game that follows another (room-objects delivery map)."""
    kind = leaf[0]
    if kind == "actor":
        _, scene, gyaml, place, plug, ltype = leaf
        if SWITCH.match(gyaml):
            if (scene, gyaml, place, plug) in eye_lines:
                return f"{gyaml}.{plug}: CoopEyeSwitch line (the remote event sets it)"
            return f"{gyaml}.{plug}: SWITCH_STATE (the presser's plug, set on ours)"
        if ltype in ASKERS:
            return f"{gyaml}.{plug}: our asker, started by its switch (CoopEyeSwitch) or DUNGEON_EVENT"
        return f"{gyaml}.{plug}: that actor's own state"
    _, ntype, imm = leaf
    if ntype.startswith("HoldGameDataGetBool") or ntype.startswith("OperateGameData"):
        return f"{ntype}: a game flag (shared flags)"
    if "SensorHitInfo" in ntype:
        return f"{ntype}: a player's contact (our Link; another player's by CoopLiftRide where it has RideOnPlayer)"
    if "Blackboard" in ntype:
        return f"{ntype}: the actor's own blackboard (its own state)"
    if not ntype:
        return f"module input {imm.get('Name')} with no caller"
    return f"{ntype}: own logic node"


def room_switches(romfs, eye_lines=frozenset(), dicts=None):
    """(shared switch plugs, delivery) from the room graphs.

    Shared: [(scene, switch name, place, plug, consumers)] for every switch output plug that a room object (not a
    switch, not an event asker) reads, straight or through gates, timers, delays and module inputs, and that is not a
    CoopEyeSwitch line (`eye_lines`: (scene, name, place, plug)). These are the presses SWITCH_STATE carries.
    Delivery: {(scene, name, place): [(input, leaf kind)]} for the platform placements (is_driver).
    """
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = read_graph(open(path, "rb").read()) if os.path.exists(path) else None
        return graphs[path]

    shared, delivery = {}, {}
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
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

            def leaves(pre, g_, src, outp, depth, seen):
                nodes, multi = g_
                if src <= -100:
                    return [x for a, b in multi(-100 - src, outp) for x in leaves(pre, g_, a, b, depth + 1, seen)]
                if src < 0 or src >= len(nodes) or depth > 16 or (id(g_), src) in seen:
                    return []
                seen.add((id(g_), src))
                node = nodes[src]
                outs = node["out"]["bool"]
                plug = outs[outp] if 0 <= outp < len(outs) else ""
                inst = node["imm"].get("InstanceName")
                if inst:
                    actor = by_id.get((pre, inst))
                    if actor is None:
                        return []
                    return [("actor", scene, str(actor.get("Gyaml", "")), _place(actor), plug, node["type"])]
                if not node["type"] and "Name" in node["imm"]:  # a module input: its caller's input
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
                                            found += leaves(ppre, parent, a, b, depth + 1, seen)
                    return found or [("node", "", dict(node["imm"]))]
                wired = [(a, b) for _, a, b in node["in"]["bool"] if a >= 0 or a <= -100]
                if not wired:
                    return [("node", node["type"], dict(node["imm"]))]
                return [x for a, b in wired for x in leaves(pre, g_, a, b, depth + 1, seen)]

            for pre, logic, _ in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for node in g_[0]:
                    obj = by_id.get((pre, node["imm"].get("InstanceName")))
                    if obj is None or node["type"] in ASKERS:
                        continue
                    name = str(obj.get("Gyaml", ""))
                    if SWITCH.match(name):
                        continue
                    for inp, a, b in node["in"]["bool"]:
                        if inp in NOT_DRIVING or not (a >= 0 or a <= -100):
                            continue
                        found = leaves(pre, g_, a, b, 0, set())
                        if is_driver(name):
                            key = (scene, name, _place(obj))
                            delivery.setdefault(key, set()).update((inp, leaf_kind(x, eye_lines)) for x in found)
                        for x in found:
                            if x[0] == "actor" and SWITCH.match(x[2]) and x[4] and (x[1], x[2], x[3], x[4]) not in eye_lines:
                                shared.setdefault((x[1], x[2], x[3], x[4]), set()).add(name)
    rows = sorted((s, n, p, plug, "+".join(sorted(c))) for (s, n, p, plug), c in shared.items())
    return rows, {k: sorted(v) for k, v in delivery.items()}


def eye_switch_lines(path):
    """{(scene, switch, place, plug)} of lichii/hook/coop_eye_switch.cpp's kSwitches."""
    text = open(path, encoding="utf-8").read()
    found = re.findall(r'\{"\w+", "(\w+)", "(\w+)", \{([-\d.]+)f, ([-\d.]+)f, ([-\d.]+)f\}, "(\w+)"', text)
    return {(sc, sw, (float(x), float(y), float(z)), plug) for sc, sw, x, y, z, plug in found}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--eye-switches", help="coop_eye_switch.cpp: its lines are left to CoopEyeSwitch")
    ap.add_argument("--delivery", action="store_true", help="print how each driven placement's input reaches a follower")
    args = ap.parse_args(argv)
    rows, riders = room_objects(args.romfs)
    eye = eye_switch_lines(args.eye_switches) if args.eye_switches else frozenset()
    switches, delivery = room_switches(args.romfs, eye)
    if args.delivery:
        for scene, name, p, why in rows:
            if why and not why.startswith("with "):
                print(f"{scene} {name} {p} [{why}]")
                for inp, kind in delivery.get((scene, name, p), []):
                    print(f"    {inp} <- {kind}")
            elif why:
                print(f"{scene} {name} {p} [{why}]: moves with it")
        return 0
    for scene, name, p, why in rows:
        print(f'    {{"{scene}", "{name}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{why}"}},')
    print()
    for name in riders:
        print(f'    "{name}",')
    print()
    for scene, name, p, plug, users in switches:
        print(f'    {{"{scene}", "{name}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{plug}"}},  // {users}')
    return 0


if __name__ == "__main__":
    sys.exit(main())
