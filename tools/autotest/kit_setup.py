"""Set a test up in one call: hearts, magic, rupees, keys, items, flags and a warp, as one ordered list of bridge calls.

A spec is a dict (the body of a preset file, tools/playtest/data/presets/*.json, and the `setup` tool's arguments):

    {"warp": {"scene": "GohmaStage", "entrance": "GohmaStage"},   # last: a warp moves Link, so set things first
     "hearts": 12,            # current life in hearts (PlayerStatus.Life, quarter hearts: 4 a heart)
     "max_hearts": 3,         # PlayerStatus.MaxLife (the game may write it back: docs/items.md "not where the game keeps")
     "magic": 0|1|2,          # 0 none, 1 the meter, 2 double magic (give Item_FairyBless_MP / _MPDouble: MPMax, MP full)
     "rupees": 200,
     "small_keys": {"ForestTemple": 2}, "boss_keys": ["ForestTemple"], "maps": [...], "compasses": [...],
     "items": ["bow", "Item_Boomerang"],      # `give` names
     "flags": {"Gohma_DefeatBoss": 0}}        # any flag name: number, true/false or an enum value's name

Names only: nothing from the game. `plan` checks a spec and returns the calls, `apply` runs them on a client and
returns what each one answered. Unknown spec keys are an error, so a typo never passes as "set".
"""
import glob
import json
import os
import re

# The flag-name dungeon key (DungeonInfo.<key>.SmallKeyNum): the game's own spelling, which is not always the
# scene's. Every key here is a DungeonInfo.* name the repository's code or docs already use.
KEYS = {"DekuTree": "InsideTheDekuTree", "DodongoCavern": "DodongoCavern", "JabuJabu": "InsideJabuJabuBelly",
        "ForestTemple": "ForestTemple", "FireTemple": "FireTemple", "WaterTemple": "WaterTemple",
        "ShadowTemple": "ShadowTemple", "SpiritTemple": "SpiritTemple", "BottomOfTheWell": "BottomOfTheWell",
        "IceCavern": "IceCavern", "GerudoTrainingGround": "GerudoTrainingGround",
        "GerudoFortress": "GerudoFortress", "GanonsCastle": "InsideGanonCastle",
        "FinalBattleField": "FinalBattleField"}
SPEC_KEYS = {"warp", "hearts", "max_hearts", "magic", "rupees", "small_keys", "boss_keys", "maps", "compasses",
             "items", "flags", "name", "version", "note"}
NAME = re.compile(r"^[A-Za-z0-9_.]{1,128}$")


def _name(v, what):
    if not isinstance(v, str) or not NAME.match(v):
        raise ValueError("%s: a name of letters, digits, _ and . (got %r)" % (what, v))
    return v


def _int(spec, key, lo, hi):
    v = spec[key]
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise ValueError("%s: a whole number from %d to %d (got %r)" % (key, lo, hi, v))
    return v


def _dungeon(d, what):
    if d not in KEYS:
        raise ValueError("%s: a dungeon, one of %s (got %r)" % (what, ", ".join(sorted(KEYS)), d))
    return "DungeonInfo." + KEYS[d]


def plan(spec):
    """The ordered bridge calls for a spec: ("flag", name, value), ("give", item) or ("warp", scene, entrance)."""
    if not isinstance(spec, dict):
        raise ValueError("a spec is an object")
    bad = sorted(set(spec) - SPEC_KEYS)
    if bad:
        raise ValueError("unknown key(s): %s (allowed: %s)" % (", ".join(bad), ", ".join(sorted(SPEC_KEYS))))
    calls = []
    # Items first (a give writes its own flags), then counts and flags, the life last of the numbers, the warp last.
    for item in spec.get("items", []):
        calls.append(("give", _name(item, "items")))
    if "magic" in spec:
        level = _int(spec, "magic", 0, 2)
        for item in ("Item_FairyBless_MP", "Item_FairyBless_MPDouble")[:level]:
            calls.append(("give", item))
        if level == 0:
            calls += [("flag", "PlayerStatus.MPMax", 0), ("flag", "PlayerStatus.MP", 0)]
    if "rupees" in spec:
        calls.append(("flag", "PlayerStatus.CurrentRupee", _int(spec, "rupees", 0, 9999)))
    for key, suffix, number in (("small_keys", "SmallKeyNum", True), ("boss_keys", "IsGetBossKey", False),
                                ("maps", "IsGetMap", False), ("compasses", "IsGetCompass", False)):
        val = spec.get(key)
        if val is None:
            continue
        if number:
            if not isinstance(val, dict):
                raise ValueError("small_keys: an object, dungeon -> count")
            for d, n in val.items():
                if isinstance(n, bool) or not isinstance(n, int) or not 0 <= n <= 9:
                    raise ValueError("small_keys.%s: a whole number from 0 to 9" % d)
                calls.append(("flag", "%s.%s" % (_dungeon(d, "small_keys"), suffix), n))
        else:
            if not isinstance(val, list):
                raise ValueError("%s: a list of dungeons" % key)
            for d in val:
                calls.append(("flag", "%s.%s" % (_dungeon(d, key), suffix), 1))
    for name, value in (spec.get("flags") or {}).items():
        if not isinstance(value, (int, str, bool)):
            raise ValueError("flags.%s: a number, true/false or an enum value's name" % name)
        calls.append(("flag", _name(name, "flags"), value))
    if "max_hearts" in spec:
        calls.append(("flag", "PlayerStatus.MaxLife", 4 * _int(spec, "max_hearts", 3, 20)))
    if "hearts" in spec:
        calls.append(("flag", "PlayerStatus.Life", 4 * _int(spec, "hearts", 1, 20)))
    w = spec.get("warp")
    if w is not None:
        if not isinstance(w, dict) or "scene" not in w or set(w) - {"scene", "entrance"}:
            raise ValueError("warp: {scene, entrance?}")
        calls.append(("warp", _name(w["scene"], "warp.scene"), _name(w["entrance"], "warp.entrance") if w.get("entrance") else None))
    return calls


def apply(client, spec, warp_timeout=60):
    """Run a spec's calls in order. Stops at the first refused call and says which; returns the answers."""
    done = []
    for call in plan(spec):
        try:
            if call[0] == "give":
                reply = client.give(call[1])
            elif call[0] == "flag":
                reply = client.flag(call[1], call[2])
            else:
                reply = client.warp(call[1], call[2], warp_timeout)
        except Exception as e:  # the client raises its own error with the bridge's code and message
            raise RuntimeError("step %d of %d (%s) failed: %s; done so far: %s" % (len(done) + 1, len(plan(spec)), " ".join(str(x) for x in call if x is not None), e, done)) from e
        done.append({"call": list(call), "reply": reply})
    return done


PRESETS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                       "tools", "playtest", "data", "presets")


def presets(root=PRESETS):
    """name -> title of every preset file (data only: flag, item and scene names)."""
    out = {}
    for f in sorted(glob.glob(os.path.join(root, "*.json"))):
        with open(f, encoding="utf-8") as fh:
            out[os.path.basename(f)[:-5]] = json.load(fh).get("title", "")
    return out


def load_preset(name, root=PRESETS):
    """The spec of a preset by name. A name is letters, digits and - only (no path); the file is checked by `plan`."""
    if not isinstance(name, str) or not re.match(r"^[a-z0-9-]{1,48}$", name):
        raise ValueError("preset: a name of lowercase letters, digits and - (one of: %s)" % ", ".join(presets(root)))
    path = os.path.join(root, name + ".json")
    if not os.path.isfile(path):
        raise ValueError("preset %r: not found (one of: %s)" % (name, ", ".join(presets(root))))
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    if data.get("preset") != name or data.get("version") != 1 or not isinstance(data.get("spec"), dict):
        raise ValueError("preset %r: the file needs preset (its name), version 1 and a spec" % name)
    plan(data["spec"])   # a bad file is refused before any call
    return data["spec"]
