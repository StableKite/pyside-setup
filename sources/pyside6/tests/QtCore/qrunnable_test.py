# Copyright (C) 2023 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QRunnable'''

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QRunnable, QThreadPool
from helper.usesqapplication import UsesQApplication
test_result = ""


def check_test():
    global test_result
    test_result = "test works"


class QRunnableTest(UsesQApplication):
    def testCreateWithAutoDelete(self):
        global test_result
        test_result = ""  # reset
        runnable = QRunnable.create(check_test)
        runnable.run()
        self.assertEqual(test_result, "test works")

    def testwithQThreadPool(self):
        global test_result
        test_result = ""  # reset
        runnable = QRunnable.create(check_test)
        tp = QThreadPool.globalInstance()
        tp.start(runnable)
        self.assertTrue(tp.waitForDone())
        self.assertEqual(test_result, "test works")


def checkRunnableFactoryLifetime(phase):
    import gc
    import os
    import sys
    import weakref
    import shiboken6
    from PySide6.QtCore import QCoreApplication, QObject, QRunnable, QThreadPool

    created = QCoreApplication.instance() is None
    app = QCoreApplication.instance() or QCoreApplication(['native-runnable-lifetime'])
    failures = []
    class RunnableCallbackError(Exception):
        pass

    class Callback:
        def __init__(self, hits, fail=False):
            self.owner = QObject()
            self.hits = hits
            self.fail = fail
        def __call__(self):
            self.hits.append(1)
            if self.fail:
                raise RunnableCallbackError('actual runnable callback error')

    if phase == 'invalid':
        for value in (None, 42, 'not callable'):
            try:
                runnable = QRunnable.create(value)
            except TypeError:
                continue
            failures.append('invalid callable accepted')
            shiboken6.delete(runnable)
            del runnable
    else:
        for iteration in range(2 if phase == 'errors' else 4):
            hits = []
            callback = Callback(hits, fail=phase == 'errors')
            reference = weakref.ref(callback)
            runnable = QRunnable.create(callback)
            owned = shiboken6.ownedByPython(runnable)
            if not owned:
                failures.append('fresh runnable lacks Python ownership')
            del callback
            gc.collect()
            assert reference() is not None, 'factory lost callable before use'
            if phase == 'unrun':
                del runnable
            elif phase == 'cancel':
                shiboken6.delete(runnable)
                del runnable
            elif phase == 'errors':
                for _ in range(32):
                    try:
                        runnable.run()
                    except RunnableCallbackError as error:
                        assert str(error) == 'actual runnable callback error'
                    else:
                        failures.append('callback exception lost')
                assert len(hits) == 32, len(hits)
                assert reference() is not None, 'callable lost after failed runs'
                shiboken6.delete(runnable)
                del runnable
            elif phase == 'repeat':
                runnable.run()
                gc.collect()
                if reference() is None:
                    failures.append('callable released after first run')
                else:
                    for _ in range(31):
                        runnable.run()
                    assert len(hits) == 32, len(hits)
                shiboken6.delete(runnable)
                del runnable
            elif phase in ('pool', 'pool_manual'):
                pool = QThreadPool()
                if phase == 'pool_manual':
                    runnable.setAutoDelete(False)
                pool.start(runnable)
                assert pool.waitForDone(10000)
                app.processEvents()
                assert len(hits) == 1, len(hits)
                if phase == 'pool':
                    if shiboken6.isValid(runnable):
                        failures.append('auto-deleted native runnable remains marked valid')
                else:
                    if reference() is None:
                        failures.append('reusable pool callable released after first run')
                    else:
                        for _ in range(7):
                            pool.start(runnable)
                            assert pool.waitForDone(10000)
                        assert len(hits) == 8, len(hits)
                    shiboken6.delete(runnable)
                shiboken6.delete(pool)
                del runnable, pool
            else:
                raise AssertionError(phase)
            gc.collect()
            app.processEvents()
            gc.collect()
            if reference() is not None:
                failures.append('callable retained after native runnable lifetime')
    print('ACTUAL_RUNNABLE_FACTORY_RESULT', phase, 'failures', len(failures), failures[:3], flush=True)
    assert not failures, (phase, len(failures), failures[:3])
    if created:
        shiboken6.delete(app)
    del app
    gc.collect()
    print('ACTUAL_RUNNABLE_FACTORY_LIFETIME_PASS', phase, flush=True)


class FactoryLifetimeTest(unittest.TestCase):
    def testCallableAndNativeLifetime(self):
        for phase in ('unrun', 'cancel', 'repeat', 'pool', 'pool_manual', 'invalid', 'errors'):
            with self.subTest(phase=phase):
                checkRunnableFactoryLifetime(phase)


if __name__ == '__main__':
    unittest.main()
