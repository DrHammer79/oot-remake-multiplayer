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
import playtest_client as pc

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
        names = ("Item_FairyBless_MP", "Item_FairyBless_MPDouble")
        for item in names[:level]:
            calls.append(("give", item))
        for item in names[level:]:   # what is above the asked level is taken away, so "none" is none after a reload too
            calls += [("flag", "IsGet." + item, 0), ("flag", "PouchData.%s.StockNum" % item, 0)]
        calls += [("flag", "PlayerStatus.MPMax", 240 * level), ("flag", "PlayerStatus.MP", 240 * level)]
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
    if "hearts" in spec and "max_hearts" in spec and spec["hearts"] > spec["max_hearts"]:
        raise ValueError("hearts: %r is above max_hearts %r" % (spec["hearts"], spec["max_hearts"]))
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


def delta_shared(call):
    """True for a call that writes a count the co-op share moves by deltas (a dungeon's small keys: docs/dungeon.md
    "Small keys: one shared count"). Written in both games it would be shared twice."""
    return call[0] == "flag" and str(call[1]).endswith(".SmallKeyNum")


def _memory_file(log_dir, player):
    return os.path.join(log_dir, "%s.setup-flags.json" % player.lower())


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _remember(log_dir, player, flags):
    """The flags a setup wrote, kept by game: the module's dump does not list them (a boss quest, a boss key), so a test
    point could not otherwise put them back."""
    if not flags:
        return
    data = _read_json(_memory_file(log_dir, player))
    data.update(flags)
    os.makedirs(log_dir, exist_ok=True)
    with open(_memory_file(log_dir, player), "w", encoding="utf-8") as fh:
        json.dump(data, fh)


def apply(client, spec, warp_timeout=60, shared=True, log_dir=None, player=None):
    """Run a spec's calls in order. Stops at the first refused call and says which; returns the answers. With
    `shared` false (the second game of a pair) the counts the share carries as deltas are left out: the first game's
    write reaches this game by itself. With `log_dir` and `player` the flags it wrote are kept for `save_point`."""
    done = []
    calls = plan(spec)
    if not shared:
        calls = [c for c in calls if not delta_shared(c)]
    if "hearts" in spec and "max_hearts" not in spec:   # life above the game's own maximum is refused before any call
        have = pc.game_of(client.state()).get("max_life")
        if isinstance(have, int) and 4 * spec["hearts"] > have:
            raise ValueError("hearts: %d is above Link's maximum of %s hearts (give max_hearts too)" % (spec["hearts"], have / 4))
    for call in calls:
        try:
            if call[0] == "give":
                reply = client.give(call[1])
            elif call[0] == "flag":
                reply = client.flag(call[1], call[2])
            else:
                reply = client.warp(call[1], call[2], warp_timeout)
        except Exception as e:  # the client raises its own error with the bridge's code and message
            raise RuntimeError("step %d of %d (%s) failed: %s; done so far: %s" % (len(done) + 1, len(calls), " ".join(str(x) for x in call if x is not None), e, [d["call"] for d in done])) from e
        done.append({"call": list(call), "reply": reply})
        if call[0] == "flag" and log_dir and player:
            _remember(log_dir, player, {call[1]: call[2]})
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


# --- save_point / load_point -----------------------------------------------------------------------------------------
# A test point is a `checkpoint` (the module's flag list plus Link's state, in the run's checkpoint log). Restoring it
# writes back every flag that differs, goes to the scene, places Link and sets his life, then takes a second dump and
# says what still differs: `exact` is true only when that dump equals the saved one. Only the flags the module's dump
# lists can be saved (the shared dungeon, story and item flags: docs/playtest.md "dump"); anything else stays as it is.


def _extra_file(log_dir, player):
    return os.path.join(log_dir, "%s.points-setup.json" % player.lower())


def _saved(log_dir, player, tag):
    path = pc.checkpoint_file(log_dir, player)
    found = None
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                e = json.loads(line)
                if e.get("tag") == tag and isinstance(e.get("flags"), dict):
                    found = e
    except OSError:
        pass
    if not found:
        raise ValueError("test point %r: none saved by this game (save_point first)" % tag)
    return found


def save_point(client, log_dir, player, tag):
    """Write a test point under `tag` (letters, digits, - and _)."""
    out = pc.checkpoint(client, log_dir, player, tag)
    if out.get("error") or out.get("flags") is None:
        raise RuntimeError("save_point: %s" % (out.get("error") or "the dump has no flags"))
    extra = _read_json(_memory_file(log_dir, player))   # what the last setup wrote and the dump cannot show
    points = _read_json(_extra_file(log_dir, player))
    points[tag] = extra
    with open(_extra_file(log_dir, player), "w", encoding="utf-8") as fh:
        json.dump(points, fh)
    out["setup_flags"] = len(extra)
    return out


def load_point(client, log_dir, player, tag, warp_timeout=60, shared=True):
    """Restore the last test point of `tag`: the flags first, then the scene is loaded again (also when Link is already in
    it: a room reads its boss state when it loads, so a beaten room only comes back by a load), then Link is placed and
    his life set. Returns {"restored": n flags written, "reloaded": bool, "exact": bool, "left": [...]}. `reloaded` is
    true when the scene changed or Link stood somewhere else after the warp; false (and `exact` false) when no load can
    be told: the room may then still be as the fight left it."""
    e = _saved(log_dir, player, tag)
    want, st = e["flags"], e.get("state") or {}
    now, cur = pc.read_dump(client.dump("restore-before"))
    scene = e.get("scene")
    written = 0
    extra = _read_json(_extra_file(log_dir, player)).get(tag) or {}
    for name, value in extra.items():   # not in the dump, so written always, and not checked after
        if shared or not delta_shared(("flag", name)):
            client.flag(name, value)
    for name, value in want.items():
        if not shared and delta_shared(("flag", name)):
            continue   # the other game's write comes here by the share
        if (now or {}).get(name) != value:
            client.flag(name, value)
            written += 1
    reloaded = True
    if scene:
        was = pc.game_of(cur or {}).get("scene")
        pos0 = pc.game_of(cur or {}).get("pos")
        client.warp(scene, None, warp_timeout)
        if was == scene:   # the same scene: a load put Link at its start, so he is not where he stood
            _, cur2 = pc.read_dump(client.dump("restore-loaded"))
            pos1 = pc.game_of(cur2 or {}).get("pos")
            reloaded = bool(pos0 and pos1 and max(abs(a - b) for a, b in zip(pos0, pos1)) > 0.5)
    pos = st.get("pos")
    if isinstance(pos, list) and len(pos) == 3:
        client.act("place %s %s %s" % tuple(repr(round(float(v), 3)) for v in pos))
    if isinstance(st.get("life"), int):
        client.flag("PlayerStatus.Life", st["life"])
    after, _ = pc.read_dump(client.dump("restore-after"))
    left = sorted(n for n, v in want.items() if (after or {}).get(n) != v and (shared or not delta_shared(("flag", n))))
    return {"tag": tag, "restored": written, "reloaded": reloaded, "exact": not left and reloaded, "left": left[:20],
            "scene": scene, "setup_flags_written": len(extra)}
