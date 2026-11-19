# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QQmlNetwork'''

import os
import sys
import unittest
import gc
import weakref


from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

import shiboken6

from PySide6.QtCore import QUrl, QTimer
from PySide6.QtGui import QGuiApplication
from PySide6.QtQuick import QQuickView
from PySide6.QtQml import QQmlNetworkAccessManagerFactory
from PySide6.QtNetwork import QNetworkAccessManager

from helper.helper import quickview_errorstring
from helper.usesqapplication import UsesQApplication


request_created = False


def check_done():
    global request_created
    if request_created:
        windows = QGuiApplication.topLevelWindows()
        if windows:
            windows[0].close()


class CustomManager(QNetworkAccessManager):
    """CustomManager (running in a different thread)"""
    def createRequest(self, op, req, data=None):
        global request_created
        print(">> createRequest ", self, op, req.url(), data)
        request_created = True
        return QNetworkAccessManager.createRequest(self, op, req, data)


class CustomFactory(QQmlNetworkAccessManagerFactory):
    def __init__(self):
        super().__init__()
        self.managers = []

    def create(self, parent=None):
        # Qt requires a new manager with the parent supplied by its QML loader.
        manager = CustomManager(parent)
        assert manager.parent() is parent
        self.managers.append(weakref.ref(manager))
        return manager


class TestQQmlNetworkFactory(UsesQApplication):
    def setUp(self):
        global request_created
        request_created = False
        super().setUp()

    def testQQuickNetworkFactory(self):
        view = QQuickView()
        timer = QTimer()
        deadline = QTimer()
        deadline.setSingleShot(True)
        deadline.timeout.connect(self.app.quit)
        self.factory = CustomFactory()
        try:
            view.engine().setNetworkAccessManagerFactory(self.factory)

            file = Path(__file__).resolve().parent / 'hw.qml'
            self.assertTrue(file.is_file())
            view.setSource(QUrl.fromLocalFile(file))
            self.assertTrue(view.rootObject(), quickview_errorstring(view))
            view.show()
            self.assertEqual(view.status(), QQuickView.Status.Ready)

            timer.timeout.connect(check_done)
            timer.start(50)
            # Count the same two seconds from the tested event loop, after
            # window/component initialization has finished.
            deadline.start(2000)
            self.app.exec()
            self.assertTrue(request_created)
            self.assertTrue(self.factory.managers)
        finally:
            deadline.stop()
            timer.stop()
            view.close()
            # Finish the loader and its children while the application is live.
            shiboken6.delete(view)
        del view, timer, deadline
        gc.collect()
        self.assertTrue(all(reference() is None for reference in self.factory.managers))


if __name__ == '__main__':
    unittest.main()
