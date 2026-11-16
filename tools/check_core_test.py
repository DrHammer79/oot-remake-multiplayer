#!/usr/bin/env python3
"""Tests for tools/check_core.py on small made-up trees (and once on the real repo)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_core  # noqa: E402

CMAKE = """set(COOP_CORE_HOOK_SOURCES
  coop_a.cpp  # a comment
)
set(COOP_CORE_UI_SOURCES
  coop_menu.cpp
)
"""


class CheckCoreTest(unittest.TestCase):
    def tree(self, files: dict[str, str], cmake: str = CMAKE) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        files = {'cmake/coop_core_sources.cmake': cmake,
                 'lichii/hook/coop_a.cpp': '#include "coop_a.h"\n#include <vector>\n',
                 'lichii/hook/coop_a.h': '#pragma once\n#include "coop/host/x.hpp"\n',
                 'coop/host/include/coop/host/x.hpp': '#pragma once\n',
                 'hookshot/src/coop_menu.cpp': '#include <string>\n',
                 **files}
        for name, text in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding='utf-8')
        return root

    def test_clean_tree_passes(self):
        self.assertEqual(check_core.check(self.tree({})), [])

    def test_windows_header_fails(self):
        problems = check_core.check(self.tree({'hookshot/src/coop_menu.cpp': '#include <Windows.h>\n'}))
        self.assertEqual(len(problems), 1)
        self.assertIn('OS header (Windows)', problems[0])

    def test_emulator_header_through_a_header_fails(self):
        problems = check_core.check(self.tree({'coop/host/include/coop/host/x.hpp': '#include "common/logging.h"\n'}))
        self.assertEqual(len(problems), 1)
        self.assertIn('emulator header', problems[0])
        self.assertIn('included by lichii/hook/coop_a.cpp', problems[0])

    def test_hookshot_abi_and_qt_fail(self):
        problems = check_core.check(self.tree({'lichii/hook/coop_a.cpp': '#include "hos_coop_menu.h"\n#include <QWidget>\n'}))
        self.assertEqual(len(problems), 2)

    def test_build_macro_conditional_fails(self):
        for line in ('#ifdef LICHII_COOP', '#if defined(COOP_HOOKSHOT) && 1', '  #  ifndef LICHII_COOP',
                     '#elif LICHII_COOP'):
            with self.subTest(line=line):
                text = f'#if 0\n{line}\n#endif\n' if 'elif' in line else f'{line}\n#endif\n'
                problems = check_core.check(self.tree({'lichii/hook/coop_a.cpp': text}))
                self.assertEqual(len(problems), 1, problems)
                self.assertIn('built without it', problems[0])

    def test_macro_in_comments_or_longer_names_passes(self):
        text = ('// the glue is built with LICHII_COOP only\n/* #ifdef LICHII_COOP */\n'
                '#ifdef LICHII_COOP_DEBUG\n#endif\nconst char* k = "LICHII_COOP_DROPS";\n')
        self.assertEqual(check_core.check(self.tree({'lichii/hook/coop_a.cpp': text})), [])

    def test_listed_in_emulator_list_too_fails(self):
        problems = check_core.check(self.tree({'lichii/hook/core.cmake': '"${_coop_hook}/coop_a.cpp"\n'}))
        self.assertEqual(len(problems), 1)
        self.assertIn('built twice', problems[0])

    def test_include_outside_the_core_folders_fails(self):
        problems = check_core.check(self.tree({'lichii/hook/coop_a.cpp': '#include "server_only.hpp"\n'}))
        self.assertEqual(len(problems), 1)
        self.assertIn('not in the core', problems[0])

    def test_missing_list_is_an_error(self):
        with self.assertRaises(ValueError):
            check_core.check(self.tree({}, cmake='set(COOP_CORE_HOOK_SOURCES coop_a.cpp)\n'))

    def test_std_locks_and_threads_fail(self):
        text = ('#include <mutex>\nstd::mutex a;\nstd::recursive_mutex b;\nstd::condition_variable c;\n'
                'void f() { std::this_thread::yield(); }\n')
        problems = check_core.check(self.tree({'lichii/hook/coop_a.h': text}))
        self.assertEqual(len(problems), 4)
        self.assertTrue(all('an OS call' in p for p in problems))
        self.assertIn('coop_a.h (included by lichii/hook/coop_a.cpp):2: std::mutex', problems[0])
        problems = check_core.check(self.tree({'hookshot/src/coop_menu.cpp': '#include <thread>\n'}))
        self.assertEqual(len(problems), 1)
        self.assertIn('std threads', problems[0])

    def test_std_clocks_fail(self):
        text = ('#include <chrono>\nauto a = std::chrono::steady_clock::now();\nusing namespace std::chrono;\n'
                'auto b = system_clock::now();\nstd::chrono::milliseconds ok(5);\n')
        problems = check_core.check(self.tree({'lichii/hook/coop_a.cpp': text}))
        self.assertEqual(len(problems), 2)
        self.assertTrue(all('coop::os::SteadyClock' in p for p in problems))

    def test_coop_os_and_comments_pass(self):
        text = ('#include <mutex>\n#include <coop/os.hpp>\n// was std::mutex\n/* std::thread */\n'
                'coop::os::Mutex m;\nstd::lock_guard<coop::os::Mutex> g(m);\nstd::unique_lock<coop::os::Mutex> u;\n'
                'int my_std_mutex_count;\n')
        self.assertEqual(check_core.check(self.tree({'lichii/hook/coop_a.cpp': text})), [])

    def test_the_repo_passes(self):
        root = Path(__file__).resolve().parent.parent
        self.assertEqual(check_core.check(root), [])
        self.assertGreater(len(check_core.core_sources(root)), 30)


if __name__ == '__main__':
    unittest.main()
