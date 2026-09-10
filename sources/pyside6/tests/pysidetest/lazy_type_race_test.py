# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

"""Incarnate the lazy types of a module from several threads at once.

Types are created on first attribute access, from whichever thread gets there
first (PYSIDE-2404). Without serialization two threads can run the same
creation function at the same time, because the name stays in the creation map
until it returns; one of them then works with a half-built type and the
process dies. Qt reaches this through its own worker threads - the QML loader
calling a Python network factory was the first case seen - but the window is
reachable from plain Python, which is what this test does.

Free-threaded modules use a module-local PEP 562 ``__getattr__`` hook instead
of replacing ``PyModule_Type.tp_getattro`` process-wide. Clear the LazyTypeLock
bit of PYSIDE6_OPTION_FT and this test fails (and may crash); that is what makes
it a test.
"""

import os
import random
import sys
import sysconfig
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

MSG_SKIP = "Only for GIL disabled builds."

THREADS = 16
ROUNDS = 3


def is_gil_disabled():
    gil_disabled_build = sysconfig.get_config_vars('Py_GIL_DISABLED')[0]
    return gil_disabled_build and not sys._is_gil_enabled()


@unittest.skipUnless(is_gil_disabled(), MSG_SKIP)
class LazyTypeRaceTest(unittest.TestCase):

    def testConcurrentIncarnation(self):
        # Not QtCore: init_test_paths() has already touched it. dir() lists
        # the lazy names without creating them, getattr() creates them.
        import PySide6.QtGui as module

        # Free threading must not patch the process-wide module type while
        # unrelated threads can be using it. Lazy lookup is installed on each
        # PySide module instead.
        lazy_getattr = module.__dict__.get("__getattr__")
        self.assertIsNotNone(lazy_getattr)
        self.assertIs(lazy_getattr.__self__, module)

        names = [name for name in dir(module)
                 if name[0].isupper() and name not in module.__dict__]
        self.assertGreater(len(names), 20)

        # Race the same still-lazy type first. Threads can all miss the normal
        # module lookup before one of them acquires the lazy lock; waiters must
        # recheck after acquiring it instead of reporting a stale map miss.
        target = names[0]
        target_start = threading.Barrier(THREADS)
        target_failures = []
        target_seen = []

        def target_worker():
            try:
                target_start.wait()
                target_seen.append(getattr(module, target))
            except Exception as e:
                target_failures.append(e)

        target_threads = [threading.Thread(target=target_worker)
                          for _ in range(THREADS)]
        for thread in target_threads:
            thread.start()
        for thread in target_threads:
            thread.join()

        self.assertEqual(target_failures, [])
        self.assertEqual(len(target_seen), THREADS)
        self.assertTrue(all(t is target_seen[0] for t in target_seen))

        start = threading.Barrier(THREADS)
        failures = []

        def worker(seed):
            order = names[:]
            random.Random(seed).shuffle(order)
            try:
                start.wait()
                for _ in range(ROUNDS):
                    for name in order:
                        getattr(module, name)
            except Exception as e:
                failures.append(e)

        threads = [threading.Thread(target=worker, args=(seed,))
                   for seed in range(THREADS)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        self.assertEqual(failures, [])
        # Every name resolves to the same object for everyone afterwards.
        for name in names:
            self.assertIs(getattr(module, name), getattr(module, name))


if __name__ == '__main__':
    unittest.main()
