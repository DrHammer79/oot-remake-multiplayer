#!/usr/bin/env python3
"""Checks that coop_core (cmake/coop_core.cmake, lists in cmake/coop_core_sources.cmake) stays platform-free.

coop_core is built once and linked by the adapters (the Longshot module, later an ExeFS code mod; the emulator
compiles the same files in its core, tools/check_core_flags.py), so none of its files, nor anything they include, may use:
  - an OS header (windows.h and friends, POSIX system headers),
  - an emulator header (core/, common/, video_core/, Qt) or a Hookshot/Longshot one (hos_coop_*, Vulkan, SDL),
  - an #if on LICHII_COOP or COOP_HOOKSHOT (the library is built without them, so such a block would
    silently leave the module),
  - an OS call the core makes through coop::os (coop/os.hpp) since v1 step 4, os-wrap (OS_CALLS: std mutexes,
    condition variables and threads; their headers <thread>, <condition_variable>, ...).
A hook file listed in coop_core must also not be listed in lichii/hook/core.cmake (it would be built twice).

Usage: python tools/check_core.py [--root REPO]   (exit 1 and one line per problem when it fails)
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef)\b(.*)$', re.M)
BUILD_MACRO = re.compile(r'\b(LICHII_COOP|COOP_HOOKSHOT)\b')
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)

# Headers the core may not include (matched against the include's text, case-insensitively).
FORBIDDEN = [
    (re.compile(r'^(windows|winsock2?|ws2tcpip|iphlpapi|shlobj|shellapi|bcrypt|winternl|ntstatus)\.h$', re.I),
     'an OS header (Windows)'),
    (re.compile(r'^(unistd|dlfcn|pthread|fcntl|poll|netdb)\.h$|^(sys|arpa|netinet)/'), 'an OS header (POSIX)'),
    (re.compile(r'^(core|common|video_core|yuzu|input_common|audio_core)/'), 'an emulator header'),
    (re.compile(r'^Q[A-Z]\w*$|^Qt'), 'a Qt header (emulator)'),
    (re.compile(r'(^|/)hos_coop'), 'a Hookshot/Longshot ABI header'),
    (re.compile(r'^(vulkan|SDL2?|SDL3)/|^vulkan', re.I), 'a renderer/window header (Hookshot)'),
    (re.compile(r'^fmt/'), 'fmt (the emulator\'s logging)'),
    (re.compile(r'^(thread|condition_variable|shared_mutex|future|stop_token)$'), 'an OS header (std threads)'),
    (re.compile(r'^(filesystem|fstream)$'), 'an OS header (std files: coop::os::ReadFile / WriteFile)'),
]

# OS calls the core makes through coop::os (coop/os.hpp) since v1 step 4 (os-wrap): (pattern, what to use instead).
OS_CALLS = [
    (re.compile(r'\bstd::(recursive_|timed_|recursive_timed_|shared_|shared_timed_)?mutex\b'), 'coop::os::Mutex'),
    (re.compile(r'\bstd::(condition_variable(_any)?|thread|jthread|this_thread)\b'), 'coop::os::Thread / SleepUs'),
    (re.compile(r'\b(steady_clock|system_clock|high_resolution_clock|utc_clock|file_clock)\b'),
     'coop::os::SteadyClock / NowUs'),
    (re.compile(r'\b(getenv|_dupenv_s|_wgetenv|secure_getenv|setenv|putenv|_putenv)\b'), 'coop::os::GetEnv'),
    (re.compile(r'\brandom_device\b'), 'coop::os::RandomFill'),
    (re.compile(r'\bstd::filesystem\b|\b[io]?fstream\b|\b_?w?fopen(_s)?\b'), 'coop::os::ReadFile / WriteFile'),
]


def cmake_list(text: str, name: str) -> list[str]:
    m = re.search(r'set\(\s*' + re.escape(name) + r'\b(.*?)\)', text, re.S)
    if not m:
        raise ValueError(f'no set({name} ...) in cmake/coop_core_sources.cmake')
    body = re.sub(r'#[^\n]*', '', m.group(1))
    return body.split()


def core_sources(root: Path) -> list[Path]:
    text = (root / 'cmake' / 'coop_core_sources.cmake').read_text(encoding='utf-8')
    hook = [root / 'lichii' / 'hook' / n for n in cmake_list(text, 'COOP_CORE_HOOK_SOURCES')]
    ui = [root / 'hookshot' / 'src' / n for n in cmake_list(text, 'COOP_CORE_UI_SOURCES')]
    return hook + ui


def include_dirs(root: Path) -> list[Path]:
    dirs = [root / 'lichii' / 'hook', root / 'hookshot' / 'src']
    dirs += sorted(p for p in (root / 'coop').glob('*/include') if p.is_dir())
    return dirs


def check(root: Path) -> list[str]:
    problems: list[str] = []
    sources = core_sources(root)
    dirs = include_dirs(root)

    emulator_list = root / 'lichii' / 'hook' / 'core.cmake'
    if emulator_list.exists():
        listed = set(re.findall(r'coop_[a-z0-9_]+\.cpp', emulator_list.read_text(encoding='utf-8')))
        for src in sources:
            if src.parent.name == 'hook' and src.name in listed:
                problems.append(f'{src.name}: in coop_core and in lichii/hook/core.cmake (built twice)')

    def rel(p: Path) -> str:
        try:
            return p.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            return p.as_posix()

    seen: dict[Path, str] = {}   # file -> the core source that first reached it
    todo = [(s, s) for s in sources]
    while todo:
        path, origin = todo.pop()
        key = path.resolve()
        if key in seen:
            continue
        seen[key] = rel(origin)
        if not path.exists():
            problems.append(f'{rel(path)}: listed in coop_core but missing')
            continue
        text = COMMENT.sub('', path.read_text(encoding='utf-8', errors='replace'))
        where = rel(path) if path == origin else f'{rel(path)} (included by {rel(origin)})'
        for m in CONDITIONAL.finditer(text):
            if BUILD_MACRO.search(m.group(2)):
                problems.append(f'{where}: #{m.group(1)} on {BUILD_MACRO.search(m.group(2)).group(1)}'
                                ' (coop_core is built without it)')
        code = INCLUDE.sub('', text)   # the includes are checked below (FORBIDDEN); line numbers stay
        for rx, instead in OS_CALLS:
            for m in rx.finditer(code):
                line = code.count('\n', 0, m.start()) + 1
                problems.append(f'{where}:{line}: {m.group(0)}: an OS call; the core uses {instead} (coop/os.hpp)')
        for kind, name in INCLUDE.findall(text):
            bad = next((why for rx, why in FORBIDDEN if rx.search(name)), None)
            if bad:
                problems.append(f'{where}: includes <{name}>: {bad}')
                continue
            if kind == '<':
                continue   # the standard library
            found = next((d / name for d in [path.parent, *dirs] if (d / name).is_file()), None)
            if found is None:
                problems.append(f'{where}: includes "{name}", which is not in the core\'s folders')
                continue
            todo.append((found, origin))
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    problems = check(args.root)
    for p in problems:
        print(f'check_core: {p}')
    if problems:
        print(f'check_core: {len(problems)} problem(s); coop_core must stay platform-free (cmake/coop_core.cmake)')
        return 1
    print(f'check_core: {len(core_sources(args.root))} coop_core files and their headers are platform-free')
    return 0


if __name__ == '__main__':
    sys.exit(main())
