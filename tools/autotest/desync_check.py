"""Compares the two games' checkpoints of a live play-test session (tools/autotest/live.py checkpoint TAG).

Each checkpoint holds both games' shared dungeon and story flags (the module's `dump TAG`) and their bridge status.
A flag the two games disagree on is a desync: co-op shares every one of them. The checkpoints are read in the order
they were taken; the first one with a difference is named, with every flag that differs in it. Standard library only.

    python -I tools/autotest/desync_check.py SESSION_DIR [--all]

Exit status: 0 when every checkpoint was compared and agrees, 1 when one does not, 2 when there is nothing to compare
(no checkpoint, or none where both games wrote their flags), 3 when some agree but another could not be compared (a game
wrote no flags): never "agree" for a checkpoint that was not compared.
"""
import argparse
import glob
import json
import os
import sys


def compare(cp):
    """(differences, note): the flags whose values differ between A and B ({name: (a, b)}), and why not compared."""
    a, b = cp.get("A") or {}, cp.get("B") or {}
    fa, fb = a.get("flags"), b.get("flags")
    if fa is None or fb is None:
        return {}, "a game wrote no flags (%s)" % (a.get("error") or b.get("error") or "no dump")
    diff = {}
    for name in sorted(set(fa) | set(fb)):
        va, vb = fa.get(name), fb.get(name)
        if va is not None and vb is not None and va != vb:
            diff[name] = (va, vb)
    return diff, None


def check(session, show_all=False, out=print):
    paths = sorted(glob.glob(os.path.join(session, "checkpoints", "*.json")), key=os.path.getmtime)
    if not paths:
        out("no checkpoints in %s" % os.path.join(session, "checkpoints"))
        return 2
    bad = compared = skipped = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            cp = json.load(f)
        tag = cp.get("tag") or os.path.splitext(os.path.basename(path))[0]
        maps = "A %s, B %s" % ((cp.get("A") or {}).get("status", {}).get("map"),
                               (cp.get("B") or {}).get("status", {}).get("map"))
        diff, note = compare(cp)
        if note:
            out("%s: not compared: %s" % (tag, note))
            skipped += 1
            continue
        compared += 1
        shots = [s for s in ((cp.get(x) or {}).get("shot") for x in ("A", "B")) if s]
        if shots:
            maps += "; shots " + ", ".join(shots)
        if not diff:
            out("%s: agree (%s)" % (tag, maps))
            continue
        bad += 1
        out("%s: %d flag(s) differ (%s)%s" % (tag, len(diff), maps, "" if bad > 1 else "  <- first desync"))
        for name, (va, vb) in diff.items():
            out("    %s: A %s, B %s" % (name, va, vb))
        if not show_all:
            break
    if bad:
        return 1
    if not compared:
        out("nothing compared: no checkpoint has both games' flags")
        return 2
    if skipped:
        out("incomplete: %d checkpoint(s) not compared" % skipped)
        return 3
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("session")
    p.add_argument("--all", action="store_true", help="every checkpoint with a difference, not only the first")
    a = p.parse_args(argv)
    return check(a.session, a.all)


if __name__ == "__main__":
    sys.exit(main())
