#!/usr/bin/python
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QObject methods'''

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QObject, QTimer, Signal, SignalInstance, Slot, Qt
import shiboken6


class Obj(QObject):
    signal = Signal()

    def empty(self):
        pass

    def emitSignal(self):
        self.signal.emit()


class Receiver(QObject):

    def __init__(self, parent=None):
        super().__init__(parent)
        self._count = 0

    def count(self):
        return self._count

    @Slot()
    def testSlot(self):
        self._count += 1


class ObjectNameCase(unittest.TestCase):
    '''Tests related to QObject object name'''

    def testSimple(self):
        # QObject.objectName(string)
        name = 'object1'
        obj = QObject()
        obj.setObjectName(name)

        self.assertEqual(name, obj.objectName())

    def testEmpty(self):
        # QObject.objectName('')
        name = ''
        obj = QObject()
        obj.setObjectName(name)

        self.assertEqual(name, obj.objectName())

    def testDefault(self):
        # QObject.objectName() default
        obj = QObject()
        self.assertEqual('', obj.objectName())

    def testUnicode(self):
        name = 'não'
        # FIXME Strange error on upstream when using equal(name, obj)
        obj = QObject()
        obj.setObjectName(name)
        self.assertEqual(obj.objectName(), name)

    def testUniqueConnection(self):
        obj = Obj()
        # On first connect, UniqueConnection returns True, and on the second
        # it must return False, and not a RuntimeError (PYSIDE-34)
        self.assertTrue(obj.signal.connect(obj.empty, Qt.ConnectionType.UniqueConnection))
        self.assertFalse(obj.signal.connect(obj.empty, Qt.ConnectionType.UniqueConnection))

    def testDisconnect(self):
        obj = Obj()
        receiver = Receiver()
        conn_id = obj.signal.connect(receiver.testSlot)
        self.assertTrue(conn_id)

        obj.emitSignal()
        self.assertEqual(receiver.count(), 1)

        obj.signal.disconnect(conn_id)
        obj.emitSignal()
        self.assertEqual(receiver.count(), 1)


class ConstructorSignalRollback(unittest.TestCase):
    def test_failed_property_rolls_back_signal_instances(self):
        for base in (QObject, QTimer):
            with self.subTest(base=base):
                class Probe(base):
                    channel = Signal((int,), (str,))

                parent = QObject()
                obj = Probe.__new__(Probe)
                marker = object()
                obj.keep = marker
                with self.assertRaises((AttributeError, TypeError)):
                    Probe.__init__(obj, parent, doesNotExistCodex=17)
                self.assertEqual(parent.children(), [])
                self.assertFalse(shiboken6.isValid(obj))
                self.assertIs(obj.keep, marker)
                self.assertFalse(any(isinstance(value, SignalInstance)
                                     for value in vars(obj).values()))
                Probe.__init__(obj, parent)
                names, integers, strings = [], [], []
                obj.objectNameChanged.connect(names.append)
                obj.channel[int].connect(integers.append)
                obj.channel[str].connect(strings.append)
                obj.setObjectName('retry')
                obj.channel[int].emit(17)
                obj.channel[str].emit('recovered')
                self.assertEqual((names, integers, strings),
                                 (['retry'], [17], ['recovered']))
                shiboken6.delete(parent)


class SlotMetadataLifetimeTest(unittest.TestCase):
    def testTemporarySlotTypes(self):
        import gc
        import weakref
        from PySide6.QtCore import Slot

        types = []
        for _ in range(64):
            class Temporary(QObject):
                @Slot(int)
                def accept(self, value):
                    self.received = value
            value = Temporary()
            self.assertGreaterEqual(value.metaObject().indexOfSlot('accept(int)'), 0)
            value.accept(42)
            self.assertEqual(value.received, 42)
            types.append(weakref.ref(Temporary))
            del value, Temporary
        gc.collect()
        gc.collect()
        self.assertTrue(all(ref() is None for ref in types))


if __name__ == '__main__':
    unittest.main()
