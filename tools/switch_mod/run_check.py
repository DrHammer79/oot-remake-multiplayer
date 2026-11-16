#!/usr/bin/env python3
"""Verdict for one emulator run of the ExeFS module (run_fritchii.ps1 calls it).

    run_check.py --log eden_log.txt [--preset NAME] [--expect NAME=REGEX]... [--forbid NAME=REGEX]...
                 [--listen listen.txt --datagrams N] [--alive 0|1]

Prints one `PASS <check>` / `FAIL <check>` line per check, then `RESULT PASS` or `RESULT FAIL`; exit 0 / 1.
Only the module's own lines (`coop-proto: ...`) are matched, so text elsewhere in the emulator log cannot pass a check.
--alive 1 says the game was still running at the end (no crash); 0 fails the run.
"""
from __future__ import annotations

import argparse
import re
import sys

MODULE_TAG = "coop-proto: "
HELLO = ("1 loaded (hello)", r"hello from subsdk1")
HOOK_IN = ("2 frame hook in", r"hook \d+: frame hook in at main\+0x6e70f8")
NULL_GUARDS = 29  # switch/proto/null_guards.inc kNullGuardCount
# switch/adapter/input.h: main's GOT entries for the five GetNpadStates imports.
NPAD_GOT = {"FullKey": 0x4cbf5c8, "Handheld": 0x4cbf5e0, "JoyDual": 0x4cbf5c0, "JoyLeft": 0x4cbf5d0, "JoyRight": 0x4cbf5d8}

# name -> (expects, forbids, datagrams)
PRESETS: dict[str, tuple[list[tuple[str, str]], list[tuple[str, str]], int]] = {
    # The prototype (reports v1-proto-a): hello, frame hook, one game call, one UDP hello.
    "proto": ([
        HELLO, HOOK_IN,
        ("2 frame hook runs", r"^frame 600$"),
        ("3 game call returned", r"game call: set_time returned"),
        ("4 net sent", r"net: connect 0, sent 29 bytes"),
    ], [], 3),
    # Null guards Run 2 (v1 step 8 (g)): every guard in, none left out, the game plays on.
    "nullguards": ([
        HELLO, HOOK_IN,
        # Pinned, so "0 of 0" cannot pass; run_check_test.py keeps it equal to null_guards.inc's kNullGuardCount.
        ("2 null guards all applied", rf"null guards: {NULL_GUARDS} of {NULL_GUARDS} applied"),
        ("3 game plays (frame 6000)", r"^frame 6000$"),
    ], [
        ("2 no guard left out", r"null guard at main\+0x[0-9a-f]+: .*left out"),
        ("2 island page usable", r"null guards: the island page could not"),
        ("2 code pages back to RX", r"restore RX failed"),
    ], 0),
    # The pad hook (v1 step 8 (d), scenarios/input.txt): five imports hooked, player 1 read through the module, the
    # scripted A, B, D-pad up and stick up each logged with its bit, then L+R+Minus opens the menu and it closes by
    # itself 5 s later. Whether Link stood still meanwhile is in the shots (swallow-start/-end vs after-start/-end).
    "input": ([
        HELLO, HOOK_IN,
        *[(f"2 {style} import hooked", rf"hook \d+: import at GOT main\+{got:#x} now goes through the module")
          for style, got in NPAD_GOT.items()],
        ("3 player 1 read through the module", r"pad: player 1 read through the module"),
        ("4a edge A (bit 0)", r"pad: player 1 down A \(bit 0,"),
        ("4b edge B (bit 1)", r"pad: player 1 down B \(bit 1,"),
        ("4c edge D-pad up (bit 13)", r"pad: player 1 down Up \(bit 13,"),
        ("4d edge left stick up (bit 17)", r"pad: player 1 down StickL-Up \(bit 17, left stick -?\d+,[1-9]\d{3,}\)"),
        ("5a menu opened (L+R+Minus)", r"^ui: OPEN$"),
        ("5a game has no pad from player 1", r"^pad: menu open, the game gets no pad from player 1"),
        ("5b menu closed by itself", r"^ui: CLOSE$"),
        ("5b game has the pad again", r"^pad: menu closed, the game has the pad again"),
        ("6 game plays (frame 3000)", r"^frame 3000$"),
    ], [
        ("2 no import skipped", r"hook \d+: GOT main\+0x[0-9a-f]+ (holds|not writable)"),
        ("4 no edge or event lost", r"lost edges [1-9]|lost events [1-9]"),
    ], 0),
}
# Presets that take a shot at scenario marks (run_fritchii.ps1 -Preset input: "coop: test mark <name>" in the log).
MARK_SHOTS = {"input": ["before", "swallow-start", "swallow-mid", "swallow-end", "after-start", "after-end"]}


def module_lines(log_text: str) -> list[str]:
    out = []
    for line in log_text.splitlines():
        i = line.find(MODULE_TAG)
        if i >= 0:
            out.append(line[i + len(MODULE_TAG):].rstrip())
    return out


def pair(text: str) -> tuple[str, str]:
    name, sep, regex = text.partition("=")
    if not sep or not name or not regex:
        raise argparse.ArgumentTypeError(f"want NAME=REGEX, got {text!r}")
    re.compile(regex)
    return name, regex


def verdict(lines: list[str], expects, forbids, datagrams: int | None = None, listen_text: str = "",
            alive: bool | None = None) -> tuple[bool, list[str]]:
    report = []
    ok_all = True

    def say(ok: bool, name: str) -> None:
        nonlocal ok_all
        ok_all = ok_all and ok
        report.append(f"{'PASS' if ok else 'FAIL'} {name}")

    for name, regex in expects:
        say(any(re.search(regex, line) for line in lines), name)
    for name, regex in forbids:
        hit = next((line for line in lines if re.search(regex, line)), None)
        say(hit is None, name if hit is None else f"{name}: {hit}")
    if datagrams:
        got = len(re.findall(r"coop-proto hello from subsdk1", listen_text))
        say(got == datagrams, f"listener datagrams ({got} of {datagrams})")
    if alive is not None:
        say(alive, "game still running at the end (no crash)")
    if not expects and not forbids and not datagrams and alive is None:
        say(False, "no checks given")
    report.append("RESULT PASS" if ok_all else "RESULT FAIL")
    return ok_all, report


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--log", required=True)
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--expect", type=pair, action="append", default=[])
    ap.add_argument("--forbid", type=pair, action="append", default=[])
    ap.add_argument("--listen")
    ap.add_argument("--datagrams", type=int)
    ap.add_argument("--alive", type=int, choices=(0, 1))
    a = ap.parse_args(argv)
    expects, forbids, datagrams = ([], [], 0)
    if a.preset:
        expects, forbids, datagrams = PRESETS[a.preset]
        expects, forbids = list(expects), list(forbids)
    expects += a.expect
    forbids += a.forbid
    if a.datagrams is not None:
        datagrams = a.datagrams
    try:
        log_text = open(a.log, encoding="utf-8", errors="replace").read()
    except OSError as e:
        log_text = ""
        print(f"run_check: no log ({e})")
    listen_text = ""
    if datagrams:
        try:
            listen_text = open(a.listen or "", encoding="utf-8", errors="replace").read()
        except OSError:
            pass
    alive = None if a.alive is None else bool(a.alive)
    lines = module_lines(log_text)
    for line in lines[:40]:
        print(f"  {line}")
    ok, report = verdict(lines, expects, forbids, datagrams, listen_text, alive)
    print("\n".join(report))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
