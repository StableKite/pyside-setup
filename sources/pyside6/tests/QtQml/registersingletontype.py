# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths  # noqa: E402
init_test_paths(False)

from helper.helper import quickview_errorstring  # noqa: E402

from PySide6.QtCore import Property, QTimer, QUrl, QObject, Slot  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import (qmlRegisterSingletonType, qmlRegisterSingletonInstance,  # noqa: E402
                           QmlElement, QmlSingleton, QJSValue)  # noqa: E402
from PySide6.QtQuick import QQuickView  # noqa: E402


URI = "Singletons"


finalResult = 0
qObjectQmlTypeId = 0


class SingletonQObject(QObject):
    def __init__(self, parent=None):
        QObject.__init__(self, parent)
        self._data = 100

    def getData(self):
        return self._data

    def setData(self, data):
        global finalResult
        finalResult = self._data = data

    data = Property(int, getData, setData)


def singletonQObjectCallback(engine):
    obj = SingletonQObject()
    obj.setData(50)
    return obj


def singletonQJSValueCallback(engine):
    return engine.evaluate("new Object({data: 50})")


QML_IMPORT_NAME = URI
QML_IMPORT_MAJOR_VERSION = 1


@QmlElement
@QmlSingleton
class DecoratedSingletonQObject(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._data = 200

    def getData(self):
        return self._data

    def setData(self, data):
        self._data = data

    data = Property(int, getData, setData)


@QmlElement
@QmlSingleton
class DecoratedSingletonWithCreate(QObject):
    def __init__(self, data, parent=None):
        super().__init__(parent)
        self._data = data

    @staticmethod
    def create(engine):
        return DecoratedSingletonWithCreate(400)

    def getData(self):
        return self._data

    def setData(self, data):
        self._data = data

    data = Property(int, getData, setData)


class TestQuickView(QQuickView):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._singleton_instance_qobject_int = False
        self._singleton_instance_qobject_str = False
        self._singleton_instance_jsvalue_int = False

    @Slot()
    def testSlot(self):
        engine = self.engine()
        instance = engine.singletonInstance(qObjectQmlTypeId)
        if instance is not None and isinstance(instance, QObject):
            self._singleton_instance_qobject_int = True
        instance = engine.singletonInstance(URI, 'SingletonQObjectNoCallback')
        if instance is not None and isinstance(instance, QObject):
            self._singleton_instance_qobject_str = True
        instance = engine.singletonInstance(URI, 'SingletonQJSValue')
        if instance is not None and isinstance(instance, QJSValue):
            self._singleton_instance_jsvalue_int = True
        self.close()


class TestQmlSupport(unittest.TestCase):
    def testRegistrationCallbackCleanup(self):
        import subprocess
        import textwrap

        # Clearing the process-wide QML registry must not affect this module's
        # decorated types or the other test's engine.
        code = textwrap.dedent("""
            import gc
            import weakref
            from init_paths import init_test_paths
            init_test_paths(False)
            from PySide6.QtCore import QCoreApplication, QObject
            from PySide6.QtQml import qmlClearTypeRegistrations, qmlRegisterSingletonType, qmlTypeId
            app = QCoreApplication(['singleton-registration-cleanup'])
            references = []
            calls = []
            class Owner:
                def __init__(self):
                    self.native = QObject()
                def __del__(self):
                    calls.append(qmlTypeId('Native.CallbackCleanup', 1, 0, 'Absent'))
            def register(index):
                owner = Owner()
                def factory(engine):
                    owner.native.objectName()
                    return QObject(engine)
                references.append((weakref.ref(factory), weakref.ref(owner),
                                   weakref.ref(owner.native)))
                result = qmlRegisterSingletonType(QObject, 'Native.CallbackCleanup',
                                                  1, 0, 'Singleton' + str(index), factory)
                assert result >= 0
            for index in range(16):
                register(index)
                qmlClearTypeRegistrations()
            gc.collect()
            assert all(callback() is None and owner() is None and native() is None
                       for callback, owner, native in references)
            assert calls == [-1] * 16, calls
            app.shutdown()
        """)
        environment = os.environ.copy()
        environment['PYTHONPATH'] = os.pathsep.join(sys.path)
        result = subprocess.run([sys.executable, '-c', code], env=environment,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def testIt(self):
        app = QGuiApplication([])

        qObjectQmlTypeId = qmlRegisterSingletonType(SingletonQObject, URI, 1, 0,  # noqa: F841
                                                    'SingletonQObjectNoCallback')
        qmlRegisterSingletonType(SingletonQObject, URI, 1, 0, 'SingletonQObjectCallback',
                                 singletonQObjectCallback)

        qmlRegisterSingletonType(URI, 1, 0, 'SingletonQJSValue', singletonQJSValueCallback)

        # Accepts only QObject derived types
        _list = [1, 2]
        with self.assertRaises(TypeError):
            qmlRegisterSingletonInstance(SingletonQObject, URI, 1, 0, 'SingletonInstance', _list)

        # Modify value on the instance
        s = SingletonQObject()
        s.setData(99)
        qmlRegisterSingletonInstance(SingletonQObject, URI, 1, 0, 'SingletonInstance', s)

        view = TestQuickView()
        file = Path(__file__).resolve().parent / 'registersingletontype.qml'
        self.assertTrue(file.is_file())
        view.setSource(QUrl.fromLocalFile(file))
        self.assertTrue(view.rootObject(), quickview_errorstring(view))
        view.resize(200, 200)
        view.show()
        QTimer.singleShot(250, view.testSlot)
        app.exec()
        self.assertEqual(finalResult, 899)
        self.assertTrue(view._singleton_instance_qobject_int)
        self.assertTrue(view._singleton_instance_qobject_str)
        self.assertTrue(view._singleton_instance_jsvalue_int)


if __name__ == '__main__':
    unittest.main()
