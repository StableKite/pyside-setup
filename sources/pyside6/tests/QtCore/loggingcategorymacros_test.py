# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

"""Tests for category logging macros qCDebug, qCInfo, qCWarning, qCCritical"""

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import (QLoggingCategory, QtMsgType, qCDebug, qCWarning, qCInfo,
                            qCCritical, qInstallMessageHandler)

param = None


def handler(msgt, ctx, msg):
    global param
    param = ctx.category + ": " + msg.strip()


class TestQLoggingCategory(unittest.TestCase):
    def setUp(self) -> None:
        super().setUp()
        self.defaultCategory = QLoggingCategory("default")
        self.debugCategory = QLoggingCategory("debug.log", QtMsgType.QtDebugMsg)
        self.infoCategory = QLoggingCategory("info.log", QtMsgType.QtInfoMsg)
        self.warningCategory = QLoggingCategory("warning.log", QtMsgType.QtWarningMsg)
        self.criticalCategory = QLoggingCategory("critical.log", QtMsgType.QtCriticalMsg)
        qInstallMessageHandler(handler)
        self.no_devices = 2

    def testCallbackReplacementOwnsPreviousHandler(self):
        import gc
        import weakref
        from PySide6.QtCore import QObject

        owner = QObject()
        reference = weakref.ref(owner)
        received = []
        def callback(kind, context, message, retained=owner):
            received.append((retained, message))
        previous = qInstallMessageHandler(callback)
        try:
            del owner
            gc.collect()
            self.assertIsNotNone(reference())
            qCInfo(self.defaultCategory, 'replacement callback')
            self.assertEqual(received[0][1], 'replacement callback')
            received.clear()
            with self.assertRaises(TypeError):
                qInstallMessageHandler(42)
            returned = qInstallMessageHandler(None)
            self.assertIs(returned, callback)
            self.assertIsNone(qInstallMessageHandler(None))
            del callback, returned
            gc.collect()
            self.assertIsNone(reference())
        finally:
            qInstallMessageHandler(previous)

    def test_qCDebug(self):
        qCDebug(self.defaultCategory, "no device")
        self.assertEqual(param, "default: no device")
        qCDebug(self.debugCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "debug.log: devices: 2")

        # not updated because category is Info which is above Debug
        # nothing will be printed here
        qCDebug(self.infoCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "debug.log: devices: 2")

    def test_qCInfo(self):
        qCInfo(self.defaultCategory, "no device")
        self.assertEqual(param, "default: no device")
        qCInfo(self.debugCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "debug.log: devices: 2")
        qCInfo(self.infoCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "info.log: devices: 2")

        # not updated because category is Warning which is above Info
        # nothing will be printed here
        qCInfo(self.warningCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "info.log: devices: 2")

    def test_qCWarning(self):
        qCWarning(self.defaultCategory, "no device")
        self.assertEqual(param, "default: no device")
        qCWarning(self.debugCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "debug.log: devices: 2")
        qCWarning(self.warningCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "warning.log: devices: 2")

        # not updated because category is Critical which is above Warning
        # nothing will be printed here
        qCWarning(self.criticalCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "warning.log: devices: 2")

    def test_qCritical(self):
        qCCritical(self.defaultCategory, "no device")
        self.assertEqual(param, "default: no device")
        qCCritical(self.warningCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "warning.log: devices: 2")
        qCCritical(self.criticalCategory, f"devices: {self.no_devices}")
        self.assertEqual(param, "critical.log: devices: 2")


if __name__ == '__main__':
    unittest.main()
