# Copyright (C) 2022 The Qt Company Ltd.
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

import shiboken6

from PySide6.QtGui import QAction
from PySide6.QtWidgets import QWidget, QMenu
from helper.usesqapplication import UsesQApplication


class QPainterDrawText(UsesQApplication):

    def _cb(self):
        self._called = True

    def testSignal(self):
        o = QWidget()
        act = QAction(o)
        self._called = False
        act.triggered.connect(self._cb)
        act.trigger()
        self.assertTrue(self._called)

    def testNewCtor(self):
        o = QWidget()
        self._called = False
        myAction = QAction("&Quit", o, triggered=self._cb)
        myAction.trigger()
        self.assertTrue(self._called)


class SetShortcutTest(UsesQApplication):

    def testSetShortcut(self):
        # Somehow an exception was leaking from the constructor
        # and appearing in setShortcut.
        o = QWidget()
        action = QAction('aaaa', o)
        shortcut = 'Ctrl+N'
        action.setShortcut(shortcut)
        s2 = action.shortcut()
        self.assertEqual(s2, shortcut)

    def testMenu(self):
        # Test the setMenu()/menu() old functionality removed in Qt6
        # that was added via helper functions.
        menu = QMenu("menu")
        action = QAction("action")

        # Using QAction::setMenu(QObject*)
        action.setMenu(menu)

        self.assertEqual(action.menu(), menu)


class MenuReferenceOwnership(UsesQApplication):
    def testMenuSurvivesActionDeletion(self):
        menu = QMenu('menu')
        action = QAction('action')
        action.setMenu(menu)
        self.assertTrue(shiboken6.ownedByPython(menu))
        self.assertIsNone(menu.parent())
        shiboken6.delete(action)
        self.assertTrue(shiboken6.isValid(menu))
        self.assertEqual(menu.title(), 'menu')

    def testReplacingMenuReleasesPreviousReference(self):
        action = QAction('action')
        first = QMenu('first')
        second = QMenu('second')
        destroyed = []
        first.destroyed.connect(lambda: destroyed.append('first'))
        second.destroyed.connect(lambda: destroyed.append('second'))
        first_ref, second_ref = weakref.ref(first), weakref.ref(second)
        action.setMenu(first)
        del first
        gc.collect()
        self.assertIsNotNone(first_ref())
        action.setMenu(second)
        gc.collect()
        self.assertIsNone(first_ref())
        self.assertEqual(destroyed, ['first'])
        del second
        action.setMenu(None)
        gc.collect()
        self.assertIsNone(second_ref())
        self.assertEqual(destroyed, ['first', 'second'])

    def testMenuActionCycleIsCollected(self):
        action = QAction('action')
        menu = QMenu('menu')
        action.setMenu(menu)
        menu.owner_action = action
        action_ref, menu_ref = weakref.ref(action), weakref.ref(menu)
        destroyed = []
        menu.destroyed.connect(lambda: destroyed.append(True))
        del action, menu
        gc.collect()
        self.assertIsNone(action_ref())
        self.assertIsNone(menu_ref())
        self.assertEqual(destroyed, [True])


if __name__ == '__main__':
    unittest.main()
