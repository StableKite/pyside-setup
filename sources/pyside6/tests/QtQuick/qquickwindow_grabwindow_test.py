# Copyright (C) 2026 The Qt Company Ltd.
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

# An actual threaded OpenGL renderer calls the generated QSurface virtuals
# while grabWindow waits. The software renderer cannot exercise that path.
os.environ.pop("QT_QUICK_BACKEND", None)
os.environ["QSG_RHI_BACKEND"] = "opengl"
os.environ["QSG_RENDER_LOOP"] = "threaded"

from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQuick import QQuickView, QSGRendererInterface
import shiboken6


@unittest.skipIf(os.environ.get("QT_QPA_PLATFORM") in ("offscreen", "minimal"),
                 "Requires a native OpenGL window")
class GrabWindowTest(unittest.TestCase):
    def test_threaded_opengl_readback(self):
        app = QGuiApplication.instance() or QGuiApplication([])
        view = QQuickView()
        view.setSource(QUrl.fromLocalFile(str(Path(__file__).with_name("grabwindow.qml"))))
        self.assertEqual(view.status(), QQuickView.Status.Ready)
        view.resize(320, 240)
        view.show()
        deadline = time.monotonic() + 10
        while not view.isExposed():
            self.assertLess(time.monotonic(), deadline)
            app.processEvents()
            time.sleep(.01)
        for _ in range(10):
            app.processEvents()
            time.sleep(.01)
        self.assertEqual(view.rendererInterface().graphicsApi(),
                         QSGRendererInterface.GraphicsApi.OpenGL)
        image = view.grabWindow()
        self.assertFalse(image.isNull())
        self.assertGreater(image.width(), 100)
        self.assertGreater(image.height(), 100)
        view.close()
        shiboken6.delete(view)


if __name__ == "__main__":
    unittest.main()
