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

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QValidator, QIntValidator
from PySide6.QtWidgets import QLineEdit, QComboBox, QCompleter
from PySide6.QtTest import QTest
from helper.usesqapplication import UsesQApplication


class MyValidator1(QValidator):
    def fixup(self, input):
        return "fixed"

    def validate(self, input, pos):
        return (QValidator.State.Acceptable, "fixed", 1)


class MyValidator2(QValidator):
    def fixup(self, input):
        return "fixed"

    def validate(self, input, pos):
        return (QValidator.State.Acceptable, "fixed")


class MyValidator3(QValidator):
    def fixup(self, input):
        return "fixed"

    def validate(self, input, pos):
        return (QValidator.State.Acceptable,)


class MyValidator4(QValidator):
    def fixup(self, input):
        return "fixed"

    def validate(self, input, pos):
        return QValidator.State.Acceptable


class MyValidator5(QValidator):
    def validate(self, input, pos):
        if input.islower():
            return (QValidator.State.Intermediate, input, pos)
        else:
            return (QValidator.State.Acceptable, input, pos)

    def fixup(self, input):
        return "22"


class QValidatorTest(UsesQApplication):
    def testValidator1(self):
        line = QLineEdit()
        line.setValidator(MyValidator1())
        line.show()
        line.setText("foo")

        QTimer.singleShot(0, line.close)
        self.app.exec()

        self.assertEqual(line.text(), "fixed")
        self.assertEqual(line.cursorPosition(), 1)

    def testValidator2(self):
        line = QLineEdit()
        line.setValidator(MyValidator2())
        line.show()
        line.setText("foo")

        QTimer.singleShot(0, line.close)
        self.app.exec()

        self.assertEqual(line.text(), "fixed")
        self.assertEqual(line.cursorPosition(), 3)

    def testValidator3(self):
        line = QLineEdit()
        line.setValidator(MyValidator3())
        line.show()
        line.setText("foo")

        QTimer.singleShot(0, line.close)
        self.app.exec()

        self.assertEqual(line.text(), "foo")
        self.assertEqual(line.cursorPosition(), 3)

    def testValidator4(self):
        line = QLineEdit()
        line.setValidator(MyValidator4())
        line.show()
        line.setText("foo")

        QTimer.singleShot(0, line.close)
        self.app.exec()

        self.assertEqual(line.text(), "foo")
        self.assertEqual(line.cursorPosition(), 3)

    def testValidator5(self):
        line = QLineEdit()
        line.show()
        line.setValidator(MyValidator5())
        line.setText("foo")
        QTest.keyClick(line, Qt.Key.Key_Return)
        self.assertEqual(line.text(), "22")


class IndependentEditorProviders(UsesQApplication):
    @staticmethod
    def makeEditor(kind):
        editor = QLineEdit() if kind == 'line' else QComboBox()
        if kind == 'combo':
            editor.setEditable(True)
        return editor

    def testProvidersSurviveEditorDeletion(self):
        for kind in ('line', 'combo'):
            for member, factory in (('Validator', QIntValidator), ('Completer', QCompleter)):
                with self.subTest(editor=kind, provider=member):
                    editor = self.makeEditor(kind)
                    provider = factory()
                    destroyed = []
                    provider.destroyed.connect(lambda: destroyed.append(True))
                    reference = weakref.ref(provider)
                    getattr(editor, 'set'+member)(provider)
                    self.assertIs(getattr(editor, member.lower())(), provider)
                    self.assertTrue(shiboken6.ownedByPython(provider))
                    self.assertIsNone(provider.parent())
                    del editor
                    gc.collect()
                    self.assertTrue(shiboken6.isValid(provider))
                    del provider
                    gc.collect()
                    self.assertIsNone(reference())
                    self.assertEqual(destroyed, [True])

    def testReplacementReleasesOldProvider(self):
        for kind in ('line', 'combo'):
            for member, factory in (('Validator', QIntValidator), ('Completer', QCompleter)):
                with self.subTest(editor=kind, provider=member):
                    editor = self.makeEditor(kind)
                    first, second = factory(), factory()
                    first_ref, second_ref = weakref.ref(first), weakref.ref(second)
                    getattr(editor, 'set'+member)(first)
                    del first
                    gc.collect()
                    self.assertIsNotNone(first_ref())
                    getattr(editor, 'set'+member)(second)
                    gc.collect()
                    self.assertIsNone(first_ref())
                    del second
                    getattr(editor, 'set'+member)(None)
                    gc.collect()
                    self.assertIsNone(second_ref())

    def testIgnoredSetterDoesNotRetainProvider(self):
        for member, factory in (('Validator', QIntValidator), ('Completer', QCompleter)):
            with self.subTest(provider=member):
                editor = QComboBox()
                provider = factory()
                reference = weakref.ref(provider)
                getattr(editor, 'set'+member)(provider)
                self.assertTrue(shiboken6.ownedByPython(provider))
                del provider
                gc.collect()
                self.assertIsNone(reference())

    def testReplacingLineEditReleasesProviders(self):
        for member, factory in (('Validator', QIntValidator), ('Completer', QCompleter)):
            with self.subTest(provider=member):
                editor = self.makeEditor('combo')
                provider = factory()
                reference = weakref.ref(provider)
                getattr(editor, 'set'+member)(provider)
                del provider
                gc.collect()
                self.assertIsNotNone(reference())
                editor.setLineEdit(QLineEdit())
                gc.collect()
                self.assertIsNone(reference())


if __name__ == '__main__':
    unittest.main()
