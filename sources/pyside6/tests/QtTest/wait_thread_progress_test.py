# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations
import os
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)
from PySide6.QtCore import QCoreApplication, QObject, Signal
from PySide6.QtTest import QTest, QSignalSpy


class Emitter(QObject):
    finished = Signal()


class WaitProgressTest(unittest.TestCase):
    def setUp(self):
        self.app = QCoreApplication.instance() or QCoreApplication([])

    def test_worker_progress(self):
        for wait in (lambda: time.sleep(1), lambda: QTest.qWait(1000),
                     lambda: QTest.qSleep(1000)):
            ready = threading.Event()
            go = threading.Event()
            completed = []
            def worker():
                ready.set()
                go.wait()
                time.sleep(.05)
                completed.append(time.perf_counter())
            thread = threading.Thread(target=worker)
            thread.start()
            self.assertTrue(ready.wait(5))
            go.set()
            wait()
            returned = time.perf_counter()
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(len(completed), 1)
            self.assertLess(completed[0], returned - .4)

    def test_signalspy_receives_worker_signal(self):
        emitter = Emitter()
        spy = QSignalSpy(emitter.finished)
        ready = threading.Event()
        go = threading.Event()
        def worker():
            ready.set()
            go.wait()
            time.sleep(.05)
            emitter.finished.emit()
        thread = threading.Thread(target=worker)
        thread.start()
        self.assertTrue(ready.wait(5))
        go.set()
        signalled = spy.wait(1000)
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertTrue(signalled)
        self.assertEqual(spy.count(), 1)


if __name__ == "__main__":
    unittest.main()
