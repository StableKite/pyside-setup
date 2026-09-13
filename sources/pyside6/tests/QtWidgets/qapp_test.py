# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

''' Test the presence of qApp Macro'''

import builtins
import os
import sys
import sysconfig
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtWidgets import QApplication


class QAppPresence(unittest.TestCase):

    def testQApp(self):
        # QtGui.qApp variable is instance of QApplication
        self.assertTrue(isinstance(qApp, QApplication))  # noqa: F821


def is_gil_disabled():
    return bool(sysconfig.get_config_var("Py_GIL_DISABLED")) and not sys._is_gil_enabled()


def create_application():
    if not is_gil_disabled():
        return QApplication([])

    # Make the old publication window observable: readers are released before
    # QApplication construction and repeatedly touch the wrapper dictionary.
    # A qApp published by MakeQAppWrapper() before _setupNew() has no initialized
    # SbkObjectPrivate/ob_dict state yet and is unsafe to expose to these readers.
    thread_count = 16
    barrier = threading.Barrier(thread_count + 1)
    stop = threading.Event()
    observed = threading.Event()
    failures = []

    def reader():
        try:
            barrier.wait()
            while not stop.is_set():
                app = builtins.qApp
                if app is not None:
                    self_dict = app.__dict__
                    if not isinstance(self_dict, dict):
                        raise AssertionError("qApp.__dict__ is not a dict")
                    observed.set()
        except BaseException as error:  # noqa: BLE001
            failures.append(error)
            stop.set()

    threads = [threading.Thread(target=reader) for _ in range(thread_count)]
    for thread in threads:
        thread.start()

    barrier.wait()
    app = QApplication([])

    # Keep the readers running after publication too. They need to observe at
    # least one initialized wrapper; otherwise a very slow worker could turn
    # this into a vacuous pass.
    observed.wait(2.0)
    stop.set()
    for thread in threads:
        thread.join()

    if failures:
        raise failures[0]
    if not observed.is_set():
        raise AssertionError("reader threads did not observe qApp publication")
    return app


def main():
    app = create_application()  # noqa: F841
    unittest.main()


if __name__ == '__main__':
    main()
