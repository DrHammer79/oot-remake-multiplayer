"""The puppet pack tool as staged in a kit (tools/release/kit_pack.ps1), run from that copy alone: every module build.py
imports loads from the kit, every path a loaded module names inside the kit (DATA, MANIFEST, CODE_PATCH, STRINGS, ...)
is there, and the data folders hold what their modules read (charsel_ui's hash and manifest deltas, coop_menu's
deltas, ui_pack's words). Run by kit_pack_test.ps1 with python -I (no repo, no script folder on sys.path):

    python -I kit_pack_check.py <kit folder>

Prints one line per problem; exits 1 when there is any."""
import json
import os
import sys


def main(kit):
    tools = os.path.abspath(os.path.join(kit, "tools"))
    stage = os.path.join(tools, "puppet_pack")
    sys.path.insert(0, tools)
    problems = []
    try:
        import puppet_pack.build  # noqa: F401  (imports every module the pack is made with)
    except Exception as e:  # noqa: BLE001  (any failure to import is the finding)
        print(f"FAIL import puppet_pack.build from the kit: {type(e).__name__}: {e}")
        return 1
    inside = os.path.normcase(stage) + os.sep
    mods = {n: m for n, m in sys.modules.items() if n == "puppet_pack" or n.startswith("puppet_pack.")}
    for name, mod in sorted(mods.items()):
        f = os.path.normcase(os.path.abspath(getattr(mod, "__file__", "") or ""))
        if not f.startswith(inside):
            problems.append(f"{name} loaded from outside the kit: {f}")
            continue
        # (every path a module names inside the kit's tool folder: its data folders, files and strings)
        for key, value in sorted(vars(mod).items()):
            if not isinstance(value, str) or not os.path.isabs(value):
                continue
            p = os.path.normcase(os.path.abspath(value))
            if p.startswith(inside) and not os.path.exists(p):
                problems.append(f"{name}.{key} names {os.path.relpath(p, stage)}, not in the kit")
    from puppet_pack import charsel_ui, coop_menu, ui_pack
    try:
        if charsel_ui.data_hash() != charsel_ui.DATA_SHA256:
            problems.append("charsel/data differs from charsel_ui.DATA_SHA256 (a file missing, extra or changed)")
        with open(charsel_ui.MANIFEST, encoding="utf-8") as f:
            manifest = json.load(f)
        for m, data_dir in ((manifest, charsel_ui.DATA), (coop_menu.MANIFEST, coop_menu.DATA)):
            for entry in m["files"]:
                # (a plain file's entry names its delta; a SARC archive's entry ("rel" or "glob") names one per
                # member: charsel_ui.make's two kinds)
                where = entry.get("rel") or entry.get("glob")
                deltas = [entry["delta"]] if "members" not in entry else [mem["delta"] for mem in entry["members"]]
                for delta in deltas:
                    if not os.path.isfile(os.path.join(data_dir, delta)):
                        problems.append(f"{os.path.relpath(data_dir, stage)}/{delta} ({where}) not in the kit")
        for lang in ("USen", "CNzh", "TWzh"):   # (en.txt and each translation the repo has)
            if not ui_pack.load_strings(lang):
                problems.append(f"ui_pack.load_strings('{lang}') read no words from the kit's strings")
    except Exception as e:  # noqa: BLE001
        problems.append(f"reading the kit's pack data: {type(e).__name__}: {e}")
    for p in problems:
        print("FAIL " + p)
    if not problems:
        print(f"ok   {len(mods)} pack modules load from the kit; every path they name there is there")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
