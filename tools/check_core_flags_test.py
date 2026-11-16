#!/usr/bin/env python3
"""Tests for tools/check_core_flags.py with made-up compile databases over the real file lists."""
from __future__ import annotations

import shlex
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_core  # noqa: E402
import check_core_flags as ccf  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HOOK = [f'lichii/hook/{n}' for n in check_core.cmake_list(
    (ROOT / 'cmake' / 'coop_core_sources.cmake').read_text(encoding='utf-8'), 'COOP_CORE_HOOK_SOURCES')]
CORE = [p.relative_to(ROOT).as_posix() for p in check_core.core_sources(ROOT)]


def gcc(target: str, rel: str, flags: str) -> dict:
    obj = f'CMakeFiles/{target}.dir/{rel}.o'
    src = (ROOT / rel).as_posix()
    return {'directory': (ROOT / 'build').as_posix(), 'file': src,
            'command': f'/usr/bin/ccache /usr/bin/g++ {flags} -MD -MT {obj} -MF {obj}.d -o {obj} -c {shlex.quote(src)}'}


def msvc(target: str, rel: str, flags: str) -> dict:
    obj = f'CMakeFiles\\{target}.dir\\{rel}.obj'
    src = str(ROOT / rel)   # native: backslashes on Windows, as MSVC's database has them
    return {'directory': str(ROOT / 'build'), 'file': src,
            'command': f'C:\\PROGRA~1\\MICROS~1\\cl.exe  /nologo /TP {flags} /showIncludes /Fo{obj} /Fdx.pdb /FS -c "{src}"'}


EMU = '-DLICHII_COOP -I/src/core -I../lichii/hook -O2 -fwrapv -fno-rtti -std=c++20'
MOD = ('-DCOOP_HOOKSHOT -DLICHII_COOP -Dhookshot_coop_EXPORTS -I../hookshot/compat -I../hookshot/src -I../lichii/hook -I../server '
       '-I_deps/fmt-src/include -I../coop/protocol/include -O2 -fPIC -fvisibility=hidden -std=c++20')
CORE_FLAGS = '-I../lichii/hook -I../hookshot/src -I../coop/protocol/include -O2 -fPIC -fvisibility=hidden -std=c++20'


class FlagsTest(unittest.TestCase):
    def run_check(self, entries: list[dict], build: str) -> list[str]:
        return ccf.check([ccf.Unit(e) for e in entries], build, ROOT)

    def test_emulator_equal_passes(self):
        db = [gcc('core', 'lichii/hook/coop_dungeon.cpp', EMU)] + [gcc('core', f, EMU) for f in HOOK]
        db += [gcc('coop_core', f, '-O2') for f in HOOK]   # the unused library target may be there too
        self.assertEqual(self.run_check(db, 'emulator'), [])

    def test_emulator_lost_wrapv_fails(self):
        db = [gcc('core', 'lichii/hook/coop_dungeon.cpp', EMU)] + [gcc('core', f, EMU) for f in HOOK[1:]]
        db.append(gcc('core', HOOK[0], EMU.replace(' -fwrapv', '')))
        problems = self.run_check(db, 'emulator')
        self.assertEqual(len(problems), 1)
        self.assertIn("missing ['-fwrapv']", problems[0])

    def test_emulator_core_file_not_in_core_fails(self):
        db = [gcc('core', 'lichii/hook/coop_dungeon.cpp', EMU)] + [gcc('coop_core', f, EMU) for f in HOOK]
        self.assertEqual(len(self.run_check(db, 'emulator')), len(HOOK))

    def test_module_allowed_differences_pass(self):
        db = [gcc('hookshot_coop', 'hookshot/src/coop_title.cpp', MOD)] + [gcc('coop_core', f, CORE_FLAGS) for f in CORE]
        self.assertEqual(self.run_check(db, 'module'), [])

    def test_module_other_differences_fail(self):
        for bad, why in ((CORE_FLAGS.replace(' -fvisibility=hidden', ''), 'flags'),
                         (CORE_FLAGS + ' -DNDEBUG', 'defines'),
                         (CORE_FLAGS.replace(' -O2', ' -O3'), 'flags'),
                         (CORE_FLAGS + ' -I/somewhere/else', 'include')):
            with self.subTest(bad=bad):
                db = [gcc('hookshot_coop', 'hookshot/src/coop_title.cpp', MOD)]
                db += [gcc('coop_core', f, CORE_FLAGS) for f in CORE[1:]] + [gcc('coop_core', CORE[0], bad)]
                problems = self.run_check(db, 'module')
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(why, problems[0])

    def test_module_msvc_command_lines(self):
        # As CI's MSVC database writes them: -external:I glued to its folder (Monocypher's, module only).
        mod = ('-DLICHII_COOP -DCOOP_HOOKSHOT -I../hookshot/compat -external:I../.deps/monocypher/src '
               '-external:W0 /W4 /utf-8 /wd4456 /bigobj /GR /EHsc')
        core = '-external:W0 /W4 /utf-8 /wd4456 /bigobj /GR /EHsc'
        db = [msvc('hookshot_coop', 'hookshot/src/coop_title.cpp', mod)] + [msvc('coop_core', f, core) for f in CORE]
        self.assertEqual(self.run_check(db, 'module'), [])
        db[-1] = msvc('coop_core', CORE[-1], core.replace(' /bigobj', ''))
        self.assertEqual(len(self.run_check(db, 'module')), 1)

    def test_flag_order(self):
        self.assertTrue(ccf.same_flags(['/W4', '/utf-8', '/permissive-'], ['/utf-8', '/W4', '/permissive-']))
        self.assertFalse(ccf.same_flags(['-fno-wrapv', '-fwrapv'], ['-fwrapv', '-fno-wrapv']))
        self.assertFalse(ccf.same_flags(['/O1', '/O2'], ['/O2', '/O1']))
        self.assertFalse(ccf.same_flags(['-std=c++17', '-std=c++20'], ['-std=c++20', '-std=c++17']))
        self.assertFalse(ccf.same_flags(['/W4'], ['/W4', '/W4']))
        self.assertEqual(ccf.family('/wd4456'), ccf.family('/wd4459'))

    def test_missing_reference_is_reported(self):
        problems = self.run_check([gcc('coop_core', f, CORE_FLAGS) for f in CORE], 'module')
        self.assertEqual(len(problems), 1)
        self.assertIn('is this a module build', problems[0])


if __name__ == '__main__':
    unittest.main()
