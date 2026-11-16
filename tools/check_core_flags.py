#!/usr/bin/env python3
"""Checks that coop_core's files are compiled with the same flags as before the library existed.

Before coop_core (cmake/coop_core.cmake), every build compiled these files itself, so a core file got
exactly the flags of its build's other files. This compares, in a compile database (compile_commands.json):

  --build emulator  each coop_core hook file is compiled in the emulator's `core` target with flags EQUAL
                    to a hook file that always was there (lichii/hook/coop_dungeon.cpp): Fritchii's
                    -fwrapv, -fno-rtti, /GT, /GR- ... included. (lichii/hook/core.cmake compiles the list there.)
  --build module    each coop_core file (target coop_core) against a module file (hookshot/src/coop_title.cpp,
                    target hookshot_coop): every flag the same, except that the module file may
                    have more -D/-I: LICHII_COOP, COOP_HOOKSHOT, hookshot_coop_EXPORTS (tools/check_core.py
                    proves no core file or header tests them) and the include folders of compat/, server/, fmt and Monocypher
                    (check_core proves no core file includes from them).

Flags are the same when both lists hold the same flags and, within each option family (-fwrapv/-fno-wrapv,
/O1//O2, ...), in the same order: there a later flag overrides an earlier one (same_flags).

Usage: python tools/check_core_flags.py --compdb build/compile_commands.json --build module|emulator
Exit 1 with one line per difference.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_core  # noqa: E402

TARGET = re.compile(r'CMakeFiles[/\\]([^/\\]+)\.dir[/\\]')
# Output and dependency-file flags: they name the file being built, not how it is built.
DROP_WITH_VALUE = {'-o', '-MT', '-MF', '-MQ'}
DROP = {'-c', '/c', '-MD', '-MMD', '/showIncludes', '/FS', '--'}
DROP_PREFIX = ('/Fo', '-Fo', '/Fd', '-Fd', '/Fp', '-Fp')
INCLUDE_FLAGS = ('-isystem', '-iquote', '-external:I', '/external:I', '-I', '/I')
LAUNCHERS = {'ccache', 'sccache', 'ccache.exe', 'sccache.exe'}

# Module only: defines and include folders the module's files have and the core's need not.
MODULE_ONLY_DEFINES = {'LICHII_COOP', 'COOP_HOOKSHOT', 'hookshot_coop_EXPORTS'}  # the last: CMake's, for a MODULE; no file reads it
MODULE_ONLY_INCLUDES = re.compile(r'(^|/)hookshot/compat$|(^|/)server$|fmt|monocypher', re.I)

REFERENCE = {'emulator': ('core', 'lichii/hook/coop_dungeon.cpp'),
             'module': ('hookshot_coop', 'hookshot/src/coop_title.cpp')}


def norm_path(path: str, cwd: str) -> str:
    p = os.path.normpath(os.path.join(cwd, path.strip('"')))
    return os.path.normcase(p).replace('\\', '/')


def tokens(entry: dict) -> list[str]:
    if 'arguments' in entry:
        return list(entry['arguments'])
    cmd = entry['command']
    msvc = bool(re.search(r'(^|[\\/"\s])(cl|clang-cl)(\.exe)?"?\s', cmd, re.I))
    return [t.strip('"') for t in shlex.split(cmd, posix=not msvc)]


class Unit:
    """One compile: its target, its file and its flags split into kinds."""

    def __init__(self, entry: dict):
        cwd = entry.get('directory', '.')
        args = tokens(entry)
        while args and os.path.basename(args[0]).lower() in LAUNCHERS:
            args = args[1:]
        args = args[1:]   # the compiler
        self.file = norm_path(entry['file'], cwd)
        out = entry.get('output', '')
        self.flags: list[str] = []
        self.defines: list[str] = []
        self.includes: list[str] = []
        i = 0
        while i < len(args):
            a = args[i]
            nxt = args[i + 1] if i + 1 < len(args) else ''
            if a in DROP_WITH_VALUE:
                if a == '-o':
                    out = out or nxt
                i += 2
                continue
            if a in DROP or a.startswith(DROP_PREFIX):
                if a.startswith(('/Fo', '-Fo')):
                    out = out or a[3:]
                i += 1
                continue
            if norm_path(a, cwd) == self.file:
                i += 1
                continue
            inc = next((f for f in INCLUDE_FLAGS if a == f or (a.startswith(f) and f != '-isystem' and f != '-iquote')), None)
            if inc:
                value = nxt if a == inc else a[len(inc):]
                self.includes.append(norm_path(value, cwd))
                i += 2 if a == inc else 1
                continue
            if a in ('-D', '/D'):
                self.defines.append(nxt)
                i += 2
                continue
            if a.startswith(('-D', '/D')):
                self.defines.append(a[2:])
                i += 1
                continue
            self.flags.append(a)
            i += 1
        m = TARGET.search(out)
        self.target = m.group(1) if m else ''


def load(path: Path) -> list[Unit]:
    return [Unit(e) for e in json.loads(path.read_text(encoding='utf-8'))]


def find(units: list[Unit], target: str, root: Path, rel: str) -> list[Unit]:
    want = norm_path(str(root / rel), '.')
    return [u for u in units if u.target == target and u.file == want]


def family(flag: str) -> str:
    """The option a flag sets, so -fwrapv/-fno-wrapv, /O1//O2, -std=c++17/-std=c++20 or /W3//W4 share one."""
    s = flag.lstrip('-/')
    s = re.sub(r'^([fWm])no-', r'\1', s)
    s = re.split(r'[=:]', s, maxsplit=1)[0]
    return s.rstrip('0123456789+-').lower()


def same_flags(ref: list[str], got: list[str]) -> bool:
    """The same flags, and in the same order wherever order can matter: within one option family a later flag
    overrides an earlier one, across families it does not (CMake may place a de-duplicated /utf-8 elsewhere)."""
    if sorted(ref) != sorted(got):
        return False
    return all([f for f in ref if family(f) == fam] == [f for f in got if family(f) == fam]
               for fam in {family(f) for f in ref})


def diff_list(name: str, ref: list[str], got: list[str]) -> str:
    missing = [x for x in ref if x not in got]
    extra = [x for x in got if x not in ref]
    if missing or extra:
        return f'{name}: missing {missing}, extra {extra}'
    return f'{name}: same flags, but an option family in another order ({ref} vs {got})'


def check(units: list[Unit], build: str, root: Path) -> list[str]:
    problems: list[str] = []
    ref_target, ref_file = REFERENCE[build]
    refs = find(units, ref_target, root, ref_file)
    if len(refs) != 1:
        return [f'{ref_file}: {len(refs)} compiles in target {ref_target} (want 1); is this a {build} build?']
    ref = refs[0]
    if build == 'emulator':
        files = [(f'lichii/hook/{n}', 'core') for n in check_core.cmake_list(
            (root / 'cmake' / 'coop_core_sources.cmake').read_text(encoding='utf-8'), 'COOP_CORE_HOOK_SOURCES')]
    else:
        files = [(p.relative_to(root).as_posix(), 'coop_core') for p in check_core.core_sources(root)]
    for rel, target in files:
        got = find(units, target, root, rel)
        if len(got) != 1:
            problems.append(f'{rel}: {len(got)} compiles in target {target} (want 1)')
            continue
        u = got[0]
        if not same_flags(ref.flags, u.flags):
            problems.append(f'{rel}: {diff_list("flags", ref.flags, u.flags)}')
        extra_defines = [d for d in u.defines if d not in ref.defines]
        extra_includes = [d for d in u.includes if d not in ref.includes]
        if build == 'emulator':
            if u.defines != ref.defines:
                problems.append(f'{rel}: {diff_list("defines", ref.defines, u.defines)}')
            if u.includes != ref.includes:
                problems.append(f'{rel}: {diff_list("includes", ref.includes, u.includes)}')
            continue
        missing_defines = [d for d in ref.defines if d not in u.defines and d.split('=')[0] not in MODULE_ONLY_DEFINES]
        missing_includes = [d for d in ref.includes if d not in u.includes and not MODULE_ONLY_INCLUDES.search(d)]
        if extra_defines or missing_defines:
            problems.append(f'{rel}: defines: missing {missing_defines}, extra {extra_defines}')
        if extra_includes or missing_includes:
            problems.append(f'{rel}: include folders: missing {missing_includes}, extra {extra_includes}')
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--compdb', type=Path, required=True)
    ap.add_argument('--build', choices=sorted(REFERENCE), required=True)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    problems = check(load(args.compdb), args.build, args.root.resolve())
    for p in problems:
        print(f'check_core_flags: {p}')
    if problems:
        print(f'check_core_flags: {len(problems)} difference(s) in the {args.build} build')
        return 1
    ref_target, ref_file = REFERENCE[args.build]
    print(f'check_core_flags: the core files compile with the flags of {ref_file} ({args.build} build)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
