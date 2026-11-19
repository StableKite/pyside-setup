# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import gc
import os
import weakref
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from helper.usesqapplication import UsesQApplication

import shiboken6

from PySide6.QtWidgets import (QApplication, QComboBox, QFontComboBox, QLabel, QProxyStyle,
                               QStyleFactory, QWidget)


class ProxyStyle(QProxyStyle):

    def __init__(self, style):
        QProxyStyle.__init__(self, style)
        self.polished = 0

    def polish(self, widget):
        self.polished = self.polished + 1
        super(ProxyStyle, self).polish(widget)


class SetStyleTest(UsesQApplication):
    '''Tests setting the same QStyle for all objects in a UI hierarchy.'''

    def testSetStyle(self):
        '''All this test have to do is not break with some invalid Python wrapper.'''

        def setStyleHelper(widget, style):
            widget.setStyle(style)
            widget.setPalette(style.standardPalette())
            for child in widget.children():
                if isinstance(child, QWidget):
                    setStyleHelper(child, style)

        container = QWidget()
        # QFontComboBox is used because it has an QLineEdit created in C++ inside it,
        # and if the QWidget.setStyle(style) steals the ownership of the style
        # for the C++ originated widget everything will break.
        fontComboBox = QFontComboBox(container)  # noqa: F841
        label = QLabel(container)
        label.setText('Label')
        style = QStyleFactory.create(QStyleFactory.keys()[0])
        setStyleHelper(container, style)

    def testSetProxyStyle(self):
        label = QLabel("QtWidgets/ProxyStyle test")
        baseStyle = QStyleFactory.create(QApplication.instance().style().objectName())
        self.assertTrue(baseStyle)
        proxyStyle = ProxyStyle(baseStyle)
        label.setStyle(proxyStyle)
        label.show()
        while not label.windowHandle().isExposed():
            QApplication.instance().processEvents()
        self.assertTrue(proxyStyle.polished > 0)

    @unittest.skipUnless(hasattr(sys, "getrefcount"), f"{sys.implementation.name} has no refcount")
    def testSetStyleOwnership(self):
        style = QStyleFactory.create(QStyleFactory.keys()[0])
        base_ref_count = sys.getrefcount(style)
        QApplication.instance().setStyle(style)
        self.assertEqual(sys.getrefcount(style), base_ref_count + 1)


    def testStyleSurvivesNativeWidgetUsers(self):
        style = QStyleFactory.create(QStyleFactory.keys()[0])
        styleName = style.objectName()
        destroyed = []
        style.destroyed.connect(lambda: destroyed.append(True))
        owners = [QComboBox(), QComboBox()]
        children = []
        for owner in owners:
            owner.setEditable(True)
            child = owner.lineEdit()
            self.assertFalse(shiboken6.createdByPython(child))
            child.setStyle(style)
            self.assertIs(child.style(), style)
            children.append(child)
        del owner, child
        ownerRefs = [weakref.ref(owner) for owner in owners]
        childRefs = [weakref.ref(child) for child in children]
        owners.pop()
        gc.collect()
        self.assertTrue(shiboken6.isValid(style))
        self.assertIs(children[0].style(), style)
        owners.clear()
        children.clear()
        gc.collect()
        self.assertTrue(all(ref() is None for ref in ownerRefs + childRefs))
        self.assertTrue(shiboken6.isValid(style))
        self.assertTrue(shiboken6.ownedByPython(style))
        self.assertEqual(style.objectName(), styleName)
        reference = weakref.ref(style)
        del style
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(destroyed, [True])

    def testReplacingNativeWidgetStyleReleasesPreviousStyle(self):
        owner = QComboBox()
        owner.setEditable(True)
        child = owner.lineEdit()
        previous = QStyleFactory.create(QStyleFactory.keys()[0])
        destroyed = []
        previous.destroyed.connect(lambda: destroyed.append(True))
        child.setStyle(previous)
        reference = weakref.ref(previous)
        del previous
        gc.collect()
        self.assertIsNotNone(reference())
        replacement = QStyleFactory.create(QStyleFactory.keys()[0])
        child.setStyle(replacement)
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(destroyed, [True])
        self.assertIs(child.style(), replacement)
        del child, owner
        gc.collect()
        self.assertTrue(shiboken6.isValid(replacement))
        self.assertTrue(shiboken6.ownedByPython(replacement))

    def testWidgetStyleReferenceCycleIsCollected(self):
        widget = QWidget()
        style = ProxyStyle(QStyleFactory.create(QStyleFactory.keys()[0]))
        style.widget = widget
        widget.setStyle(style)
        references = [weakref.ref(widget), weakref.ref(style)]
        del widget, style
        gc.collect()
        self.assertTrue(all(ref() is None for ref in references))


if __name__ == '__main__':
    unittest.main()
