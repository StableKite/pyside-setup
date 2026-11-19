# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

''' Test cases related to QGraphicsItem and subclasses'''

import gc
import os
import sys
import unittest
import weakref

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

import shiboken6

from PySide6.QtGui import QPolygonF, QColor, QBrush
from PySide6.QtWidgets import QGraphicsScene, QGraphicsWidget

from helper.usesqapplication import UsesQApplication


class QColorOnSetBrush(UsesQApplication):
    '''Test case for passing a QColor directly to setBrush'''

    def setUp(self):
        # Acquire resources
        super(QColorOnSetBrush, self).setUp()

        self.scene = QGraphicsScene()
        poly = QPolygonF()
        self.item = self.scene.addPolygon(poly)
        self.color = QColor('black')

    def tearDown(self):
        # Release resources
        del self.color
        del self.item
        del self.scene
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()
        super(QColorOnSetBrush, self).tearDown()

    def testQColor(self):
        # QGraphicsAbstractShapeItem.setBrush(QColor)
        self.item.setBrush(self.color)
        self.assertEqual(QBrush(self.color), self.item.brush())


class ParentOverrideOwnership(UsesQApplication):
    def testRejectedParentPreservesOwnership(self):
        class RejectParent(QGraphicsWidget):
            def itemChange(self, change, value):
                if change == self.GraphicsItemChange.ItemParentChange:
                    return None
                return super().itemChange(change, value)

        parent = QGraphicsWidget()
        child = RejectParent()
        destroyed = []
        child.destroyed.connect(lambda: destroyed.append(True))
        reference = weakref.ref(child)
        child.setParentItem(parent)
        self.assertTrue(shiboken6.ownedByPython(child))
        del parent
        gc.collect()
        self.assertTrue(shiboken6.isValid(child))
        self.assertIsNone(child.parentItem())
        del child
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(destroyed, [True])

    def testRedirectedParentRetainsChild(self):
        class RedirectParent(QGraphicsWidget):
            def itemChange(self, change, value):
                if (change == self.GraphicsItemChange.ItemParentChange
                        and hasattr(self, 'new_parent')):
                    return self.new_parent
                return super().itemChange(change, value)

        requested = QGraphicsWidget()
        actual = QGraphicsWidget()
        child = RedirectParent()
        child.new_parent = actual
        destroyed = []
        child.destroyed.connect(lambda: destroyed.append(True))
        reference = weakref.ref(child)
        child.setParentItem(requested)
        del requested
        gc.collect()
        self.assertTrue(shiboken6.isValid(child))
        del child.new_parent
        del child
        gc.collect()
        self.assertIsNotNone(reference())
        del actual
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(destroyed, [True])

    def testDetachedItemRemainsOwnedByScene(self):
        scene = QGraphicsScene()
        parent = QGraphicsWidget()
        child = QGraphicsWidget()
        destroyed = []
        child.destroyed.connect(lambda: destroyed.append(True))
        reference = weakref.ref(child)
        scene.addItem(parent)
        child.setParentItem(parent)
        child.setParentItem(None)
        self.assertFalse(shiboken6.ownedByPython(child))
        scene.removeItem(parent)
        del parent, child
        gc.collect()
        self.assertIsNotNone(reference())
        self.assertEqual(destroyed, [])
        scene.clear()
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(destroyed, [True])


if __name__ == '__main__':
    unittest.main()
