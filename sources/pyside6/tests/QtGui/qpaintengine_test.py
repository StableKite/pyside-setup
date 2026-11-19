# Copyright (C) 2025 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import gc
import os
import sys
import unittest
import weakref

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from helper.usesqapplication import UsesQApplication
from PySide6.QtGui import QPaintEngine, QPainter, QPaintDevice
from PySide6.QtCore import QPoint, QPointF, QRect, QRectF, QLine, QLineF


class PaintEngine(QPaintEngine):
    def __init__(self):
        super().__init__()
        self.line_count = 0
        self.point_count = 0
        self.rect_count = 0
        self.value_refs = []
        self.retained_values = []

    def drawPoints(self, points):
        self.observeValues(points)
        self.point_count = len(points)

    def drawRects(self, rects):
        self.observeValues(rects)
        self.rect_count = len(rects)

    def drawLines(self, lines):
        self.observeValues(lines)
        self.line_count = len(lines)

    def drawPolygon(self, points, mode):
        self.observeValues(points)

    def observeValues(self, values):
        self.value_refs.extend(weakref.ref(value) for value in values)
        self.retained_values.append(values[0])

    def updateState(self, s):
        pass

    def begin(self, _dev):
        return True

    def end(self):
        return True


class PaintDevice(QPaintDevice):
    def __init__(self):
        super().__init__()
        self._engine = PaintEngine()

    def paintEngine(self):
        return self._engine

    def metric(self, metric):
        if metric == QPaintDevice.PaintDeviceMetric.PdmDevicePixelRatioScaled:
            return super().metric(metric)
        return 1


class QPaintEngineTest(UsesQApplication):
    """PYSIDE-3002: test whether virtual functions of QPaintEngine taking
       a C-style array of geometry primitives can be overridden."""
    def setUp(self):
        super().setUp()
        self._paint_device = PaintDevice()

    def tearDown(self):
        self._paint_device = None
        super().tearDown()

    def test(self):
        points = [QPoint(1, 2), QPoint(3, 4)]
        rectangles = [QRect(1, 1, 1, 1), QRect(2, 2, 2, 2)]
        lines = [QLine(1, 2, 3, 4), QLine(3, 4, 5, 6)]

        with QPainter(self._paint_device) as painter:
            painter.drawPoints(points)
            painter.drawRects(rectangles)
            painter.drawLines(lines)

        engine = self._paint_device.paintEngine()
        self.assertEqual(engine.line_count, 2)
        self.assertEqual(engine.point_count, 2)
        self.assertEqual(engine.rect_count, 2)

    def testCopiedArrayValuesLifetime(self):
        arrays = [
            ('drawPoints', [QPoint(1, 2), QPoint(3, 4)]),
            ('drawPoints', [QPointF(1.25, 2.5), QPointF(3.75, 4.5)]),
            ('drawLines', [QLine(1, 2, 3, 4), QLine(5, 6, 7, 8)]),
            ('drawLines', [QLineF(1.25, 2.5, 3.75, 4.5), QLineF(5.25, 6.5, 7.75, 8.5)]),
            ('drawRects', [QRect(1, 2, 3, 4), QRect(5, 6, 7, 8)]),
            ('drawRects', [QRectF(1.25, 2.5, 3.75, 4.5), QRectF(5.25, 6.5, 7.75, 8.5)]),
            ('drawPolygon', [QPoint(1, 2), QPoint(3, 4), QPoint(5, 1)]),
            ('drawPolygon', [QPointF(1.25, 2.5), QPointF(3.75, 4.5), QPointF(5.25, 1.5)]),
        ]
        engine = self._paint_device.paintEngine()
        with QPainter(self._paint_device) as painter:
            for method, values in arrays:
                getattr(painter, method)(values)

        self.assertEqual(len(engine.value_refs), sum(len(values) for _, values in arrays))
        self.assertEqual(len(engine.retained_values), len(arrays))
        for value, (_, original) in zip(engine.retained_values, arrays):
            self.assertEqual(value, original[0])
            self.assertIsNot(value, original[0])
        del value
        engine.retained_values.clear()
        gc.collect()
        self.assertTrue(all(reference() is None for reference in engine.value_refs))


if __name__ == '__main__':
    unittest.main()
