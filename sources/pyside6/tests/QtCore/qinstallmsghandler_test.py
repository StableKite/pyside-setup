# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for qInstallMsgHandler'''

import os
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor
from queue import SimpleQueue
from threading import Barrier

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import (QLibraryInfo, QtMsgType,
                            QMessageLogContext,
                            qCritical, qFormatLogMessage, qDebug,
                            qInstallMessageHandler, qWarning)


param = []


def handler(msgt, ctx, msg):
    global param
    param = [msgt, ctx, msg.strip()]


def handleruseless(msgt, ctx, msg):
    pass


class QInstallMsgHandlerTest(unittest.TestCase):

    def testConcurrentReplacement(self):
        messages = SimpleQueue()
        ready = Barrier(8)

        def first(_kind, _context, message):
            messages.put(message)

        def second(_kind, _context, message):
            messages.put(message)

        def send(worker):
            ready.wait(timeout=10)
            for index in range(250):
                qWarning(f'{worker}:{index}')

        def replace():
            ready.wait(timeout=10)
            for _ in range(125):
                self.assertIn(qInstallMessageHandler(first), (first, second))
                self.assertIn(qInstallMessageHandler(second), (first, second))

        qInstallMessageHandler(first)
        with ThreadPoolExecutor(max_workers=8) as pool:
            workers = [pool.submit(send, worker) for worker in range(4)]
            workers.extend(pool.submit(replace) for _ in range(4))
            for worker in workers:
                worker.result(timeout=30)

        self.assertEqual({messages.get_nowait() for _ in range(1000)},
                         {f'{worker}:{index}' for worker in range(4) for index in range(250)})
        self.assertTrue(messages.empty())

        def replaceFromCallback(_kind, _context, message):
            messages.put(message)
            qInstallMessageHandler(first)

        qInstallMessageHandler(replaceFromCallback)
        qWarning('reentrant')
        qWarning('replacement')
        self.assertEqual(messages.get_nowait(), 'reentrant')
        self.assertEqual(messages.get_nowait(), 'replacement')

    def tearDown(self):
        # Ensure that next test will have a clear environment
        qInstallMessageHandler(None)

    def testNone(self):
        ret = qInstallMessageHandler(None)
        self.assertEqual(ret, None)

    @unittest.skipUnless(hasattr(sys, "getrefcount"), f"{sys.implementation.name} has no refcount")
    def testRet(self):
        ret = qInstallMessageHandler(None)
        self.assertEqual(ret, None)
        refcount = sys.getrefcount(handleruseless)
        retNone = qInstallMessageHandler(handleruseless)  # noqa: F841
        self.assertEqual(sys.getrefcount(handleruseless), refcount + 1)
        rethandler = qInstallMessageHandler(None)
        self.assertEqual(rethandler, handleruseless)
        del rethandler
        self.assertEqual(sys.getrefcount(handleruseless), refcount)

    def testHandler(self):
        rethandler = qInstallMessageHandler(handler)  # noqa: F841
        if QLibraryInfo.isDebugBuild():
            qDebug("Test Debug")
            self.assertEqual(param[0], QtMsgType.QtDebugMsg)
            self.assertEqual(param[2], "Test Debug")
        qWarning("Test Warning")
        self.assertEqual(param[0], QtMsgType.QtWarningMsg)
        self.assertEqual(param[2], "Test Warning")
        qCritical("Test Critical")
        self.assertEqual(param[0], QtMsgType.QtCriticalMsg)
        self.assertEqual(param[2], "Test Critical")

    def testFormat(self):
        ctx = QMessageLogContext()
        s = qFormatLogMessage(QtMsgType.QtInfoMsg, ctx, 'bla')
        self.assertTrue(s)


if __name__ == '__main__':
    unittest.main()
