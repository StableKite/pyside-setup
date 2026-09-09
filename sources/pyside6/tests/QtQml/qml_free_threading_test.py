# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import os
import sys
import sysconfig
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths  # noqa: E402
init_test_paths(False)

from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtQml import (QmlAttached, QmlExtended, QmlForeign,  # noqa: E402
                           QmlSingleton)


@unittest.skipUnless(sysconfig.get_config_var("Py_GIL_DISABLED"),
                     "Requires a free-threaded Python build")
class QmlFreeThreadingTest(unittest.TestCase):
    def test_concurrent_type_info_updates(self):
        class Shared(QObject):
            pass

        class Foreign(QObject):
            pass

        class Attached(QObject):
            pass

        class Extension(QObject):
            pass

        thread_count = 8
        iterations = 200
        barrier = threading.Barrier(thread_count + 1)
        errors = []

        def worker(thread_id):
            try:
                barrier.wait()
                for i in range(iterations):
                    # Distinct classes force concurrent QHash growth while the
                    # shared class exercises concurrent updates of one info
                    # object and repeated publication of its foreign alias.
                    local_type = type(f"QmlFt_{thread_id}_{i}", (QObject,), {})
                    QmlSingleton(local_type)
                    QmlSingleton(Shared)
                    QmlForeign(Foreign)(Shared)
                    QmlAttached(Attached)(Shared)
                    QmlExtended(Extension)(Shared)
            except BaseException as e:
                errors.append(e)

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(thread_count)]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join()

        self.assertEqual(errors, [])
        self.assertFalse(sys._is_gil_enabled())


if __name__ == '__main__':
    unittest.main()
