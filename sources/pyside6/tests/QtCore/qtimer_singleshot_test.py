#!/usr/bin/python
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QTimer.singleShot'''

import gc
import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QObject, QThread, QTimer, Signal, Slot, SLOT
from helper.usesqapplication import UsesQApplication


class WatchDog(QObject):
    '''Exits the QCoreApplication main loop after sometime.'''

    def __init__(self, watched):
        super().__init__()
        self.times_called = 0
        self.watched = watched

    def timerEvent(self, evt):
        self.times_called += 1
        if self.times_called == 20:
            self.watched.exit_app_cb()


class ThreadForContext(QThread):
    def __init__(self):
        super().__init__()
        self.called = False
        self.qthread = None
        self.context = QObject()

    def run(self):
        self.called = True
        self.qthread = QThread.currentThread()
        self.exec()


class TestSingleShot(UsesQApplication):
    '''Test case for QTimer.singleShot'''

    def setUp(self):
        # Acquire resources
        UsesQApplication.setUp(self)
        self.watchdog = WatchDog(self)
        self.called = False
        self.qthread = None

    def tearDown(self):
        # Release resources
        del self.watchdog
        del self.called
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()
        UsesQApplication.tearDown(self)

    def callback(self):
        self.called = True
        self.qthread = QThread.currentThread()
        self.qthread.exit()
        self.app.quit()

    def testPendingSingleShotDispatcherOwnership(self):
        import weakref
        import shiboken6
        from PySide6.QtCore import QAbstractEventDispatcher

        class Callback:
            def __init__(self):
                self.owner = QObject()

            def __call__(self):
                raise AssertionError('cancelled timer executed')

        dispatcher = QAbstractEventDispatcher.instance()
        for with_context in (False, True):
            with self.subTest(with_context=with_context):
                before = set(dispatcher.findChildren(QTimer))
                callback = Callback()
                reference = weakref.ref(callback)
                owner_reference = weakref.ref(callback.owner)
                context = QObject()
                if with_context:
                    QTimer.singleShot(60000, context, callback)
                else:
                    QTimer.singleShot(60000, callback)
                del callback
                pending = set(dispatcher.findChildren(QTimer)) - before
                self.assertEqual(len(pending), 1)
                self.assertIsNotNone(reference())
                for timer in pending:
                    self.assertIs(timer.parent(), dispatcher)
                    shiboken6.delete(timer)
                gc.collect()
                self.assertIsNone(reference())
                self.assertIsNone(owner_reference())
                del context

    def testZeroShotReleasesCallbackWhenContextIsDeleted(self):
        import weakref
        import shiboken6

        class Callback:
            def __init__(self):
                self.owner = QObject()

            def __call__(self):
                raise AssertionError('cancelled timer executed')

        context = QObject()
        callback = Callback()
        reference = weakref.ref(callback)
        owner_reference = weakref.ref(callback.owner)
        QTimer.singleShot(0, context, callback)
        del callback
        shiboken6.delete(context)
        gc.collect()
        self.assertIsNone(reference())
        self.assertIsNone(owner_reference())

    def testSingleShot(self):
        QTimer.singleShot(100, self.callback)
        self.app.exec()
        self.assertTrue(self.called)

    def testSingleShotZero(self):
        QTimer.singleShot(0, self.callback)
        self.app.exec()
        self.assertTrue(self.called)

    def testSingleShotWithContext(self):
        thread = ThreadForContext()
        thread.start()
        thread.context.moveToThread(thread)
        QTimer.singleShot(100, thread.context, self.callback)
        self.app.exec()
        thread.wait()
        self.assertTrue(self.called)
        self.assertTrue(thread.called)
        self.assertEqual(self.qthread, thread.qthread)

    def testSingleShotWithContextZero(self):
        thread = ThreadForContext()
        thread.start()
        thread.context.moveToThread(thread)
        QTimer.singleShot(0, thread.context, self.callback)
        self.app.exec()
        thread.wait()
        self.assertTrue(self.called)
        self.assertTrue(thread.called)
        self.assertEqual(self.qthread, thread.qthread)


class TestSingleShotCallableObject(UsesQApplication):
    '''Test case for QTimer.singleShot with callable inside an object'''

    def setUp(self):
        # Acquire resources
        UsesQApplication.setUp(self)
        self.watchdog = WatchDog(self)

    def tearDown(self):
        # Release resources
        del self.watchdog
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()
        UsesQApplication.tearDown(self)

    class CallbackObject(QObject):
        def __init__(self, app) -> None:
            super().__init__()
            self.app = app

        @Slot()
        def func(self):
            self.called = True
            self.app.quit()

    def testSingleShotWithObjectAndMember(self):
        callback = self.CallbackObject(self.app)
        QTimer.singleShot(100, callback, SLOT("func()"))
        self.app.exec()
        self.assertTrue(callback.called)

    def testSingleShotWithObjectAndMemberZero(self):
        callback = self.CallbackObject(self.app)
        QTimer.singleShot(0, callback, SLOT("func()"))
        self.app.exec()
        self.assertTrue(callback.called)

    def testSingleShotWithCallableInObject(self):
        callback = self.CallbackObject(self.app)
        QTimer.singleShot(100, callback.func)
        self.app.exec()
        self.assertTrue(callback.called)

    def testSingleShotWithCallableInObjectZero(self):
        callback = self.CallbackObject(self.app)
        QTimer.singleShot(0, callback.func)
        self.app.exec()
        self.assertTrue(callback.called)


class SigEmitter(QObject):

    sig1 = Signal()


class TestSingleShotSignal(UsesQApplication):
    '''Test case for QTimer.singleShot connecting to signals'''

    def setUp(self):
        UsesQApplication.setUp(self)
        self.watchdog = WatchDog(self)
        self.called = False

    def tearDown(self):
        del self.watchdog
        del self.called
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()
        UsesQApplication.tearDown(self)

    def callback(self):
        self.called = True
        self.app.quit()

    def testSingleShotSignal(self):
        emitter = SigEmitter()
        emitter.sig1.connect(self.callback)
        QTimer.singleShot(100, emitter.sig1)
        self.app.exec()
        self.assertTrue(self.called)

    def testSingleShotSignalZero(self):
        emitter = SigEmitter()
        emitter.sig1.connect(self.callback)
        QTimer.singleShot(0, emitter.sig1)
        self.app.exec()
        self.assertTrue(self.called)


if __name__ == '__main__':
    unittest.main()
