# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import os
import sys
import time
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QCoreApplication, QRunnable, QThread, QThreadPool


thread_function_called = False


def thread_function():
    global thread_function_called
    thread_function_called = True


def setUpModule():
    global app
    app = QCoreApplication.instance() or QCoreApplication(['threadpool-tests'])


def tearDownModule():
    import shiboken6
    QThreadPool.globalInstance().waitForDone()
    shiboken6.delete(app)


class Task(QRunnable):
    def run(self):
        QThread.msleep(100)


class QThreadPoolTest(unittest.TestCase):
    def testSlowJobs(self):
        '''This used to cause a segfault due the ownership control on
           globalInstance function'''
        for i in range(3):
            task = Task()
            QThreadPool.globalInstance().start(task)
            time.sleep(0.05)

        self.assertTrue(QThreadPool.globalInstance().waitForDone())

    def testCallable(self):
        global thread_function_called
        thread_function_called = False
        tp = QThreadPool.globalInstance()
        tp.start(thread_function)
        self.assertTrue(tp.waitForDone())
        self.assertTrue(thread_function_called)


def checkPoolCallbackOwnership(phase):
    import gc
    import os
    import sys
    import threading
    import weakref
    import shiboken6
    from PySide6.QtCore import QCoreApplication, QObject, QRunnable, QThreadPool

    created = QCoreApplication.instance() is None
    app = QCoreApplication.instance() or QCoreApplication(['native-threadpool-ownership'])
    failures = []
    class Callback:
        def __init__(self, hits):
            self.hits = hits
            self.owner = QObject()
        def __call__(self):
            self.hits.append(1)

    for iteration in range(4):
        pool = QThreadPool()
        pool.setMaxThreadCount(1)
        hits = []
        callback = Callback(hits)
        reference = weakref.ref(callback)
        callable_phase = phase.startswith('callable_')
        runnable = None if callable_phase else QRunnable.create(callback)
        if not callable_phase:
            assert shiboken6.ownedByPython(runnable)
            del callback
        if phase in ('callable_start', 'callable_try'):
            if phase == 'callable_start':
                pool.start(callback)
            else:
                accepted = pool.tryStart(callback)
                assert accepted
                if type(accepted) is not bool:
                    failures.append('callable tryStart result is not bool')
            del callback
            assert pool.waitForDone(10000)
            assert hits == [1], hits
            shiboken6.delete(pool)
        elif phase in ('callable_rejected', 'callable_clear'):
            started = threading.Event()
            release = threading.Event()
            def block():
                started.set()
                assert release.wait(10), 'blocker timed out'
            blocker = QRunnable.create(block)
            pool.start(blocker)
            assert started.wait(5)
            try:
                if phase == 'callable_rejected':
                    accepted = pool.tryStart(callback)
                    assert not accepted
                    if type(accepted) is not bool:
                        failures.append('callable tryStart result is not bool')
                else:
                    pool.start(callback)
                    pool.clear()
                del callback
                gc.collect()
                if reference() is not None:
                    failures.append('refused/cancelled callable retained')
                assert not hits, hits
            finally:
                release.set()
                assert pool.waitForDone(10000)
            shiboken6.delete(pool)
            del blocker, block, release, started
        elif phase in ('rejected', 'clear'):
            started = threading.Event()
            release = threading.Event()
            def block():
                started.set()
                assert release.wait(10), 'blocker timed out'
            blocker = QRunnable.create(block)
            pool.start(blocker)
            assert started.wait(5), 'blocker did not start'
            try:
                if phase == 'rejected':
                    assert not pool.tryStart(runnable)
                    if not shiboken6.ownedByPython(runnable):
                        failures.append('rejected task lost caller ownership')
                    assert shiboken6.isValid(runnable)
                    shiboken6.delete(runnable)
                else:
                    pool.start(runnable)
                    pool.clear()
                    assert not shiboken6.isValid(runnable)
                assert not hits, hits
            finally:
                release.set()
                assert pool.waitForDone(10000)
            shiboken6.delete(pool)
            del blocker, block, release, started
        else:
            raise AssertionError(phase)
        del runnable, pool
        gc.collect()
        app.processEvents()
        gc.collect()
        if reference() is not None:
            failures.append('callback retained after pool/runnable cleanup')
    print('ACTUAL_THREADPOOL_OWNERSHIP_RESULT', phase, len(failures), failures[:3], flush=True)
    assert not failures, (phase, failures[:3])
    if created:
        shiboken6.delete(app)
    del app
    gc.collect()
    print('ACTUAL_THREADPOOL_OWNERSHIP_PASS', phase, flush=True)


def checkPoolClearReentrant():
    import gc
    import os
    import sys
    import threading
    import weakref
    import shiboken6
    from PySide6.QtCore import QCoreApplication, QObject, QRunnable, QThreadPool

    created = QCoreApplication.instance() is None
    app = QCoreApplication.instance() or QCoreApplication(['native-pool-clear-finalizer'])
    pool = QThreadPool()
    pool.setMaxThreadCount(1)
    started, release = threading.Event(), threading.Event()
    armed, calls = [True], []
    def block():
        started.set()
        assert release.wait(10)
    blocker = QRunnable.create(block)
    pool.start(blocker)
    assert started.wait(5)

    class Cancelled:
        def __init__(self, pool, armed, calls):
            self.pool, self.armed, self.calls = pool, armed, calls
            self.owner = QObject()
        def __call__(self):
            raise AssertionError('cancelled task executed')
        def __del__(self):
            if self.armed[0]:
                self.calls.append(self.pool.activeThreadCount())

    try:
        callback = Cancelled(pool, armed, calls)
        reference = weakref.ref(callback)
        pool.start(callback)
        del callback
        pool.clear()
        gc.collect()
        passed = reference() is None and calls == [1]
    finally:
        armed[0] = False
        release.set()
        assert pool.waitForDone(10000)
    shiboken6.delete(pool)
    del pool, blocker, block
    app.processEvents()
    if created:
        shiboken6.delete(app)
    del app
    gc.collect()
    print('ACTUAL_POOL_CLEAR_REENTRANT_RESULT', passed, calls, flush=True)
    assert passed, ('cancelled callback finalizer did not run during clear', calls)
    print('ACTUAL_POOL_CLEAR_REENTRANT_PASS', flush=True)


class PoolCallbackOwnershipTest(unittest.TestCase):
    def testAcceptedRejectedAndCancelledCallbacks(self):
        for phase in ('rejected', 'clear', 'callable_start', 'callable_try',
                      'callable_rejected', 'callable_clear'):
            with self.subTest(phase=phase):
                checkPoolCallbackOwnership(phase)

    def testClearRunsCallbackFinalizer(self):
        checkPoolClearReentrant()


if __name__ == '__main__':
    unittest.main()
