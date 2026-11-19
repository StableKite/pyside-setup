# Copyright (C) 2024 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import os
import subprocess
import textwrap
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from helper.usesqapplication import UsesQApplication


from PySide6.QtCore import QCoreApplication, QObject  # noqa: F401
from PySide6.QtQml import QQmlApplicationEngine, qmlRegisterType


class BaseClass(QObject):
    def __init__(self, p=None):
        super().__init__(p)


class ChildClass(BaseClass):
    def __init__(self, p=None):
        super().__init__(p)


class TestQmlRegisterType(UsesQApplication):
    """Test the legacy QML register functions."""

    def test(self):
        qmlRegisterType(BaseClass, 'test', 1, 0, 'BaseClass')
        qmlRegisterType(ChildClass, 'test', 1, 0, 'ChildClass')
        # PYSIDE-2709: qmlRegisterType() would set additional class info
        # on the meta objects for registration which caused another meta
        # object to be created, breaking inheritance.
        child = ChildClass()
        base = BaseClass()
        self.assertTrue(child.metaObject().inherits(base.metaObject()))

        engine = QQmlApplicationEngine()
        file = Path(__file__).resolve().parent / 'qmlregistertype_test.qml'

        engine.load(file)
        rootObjects = engine.rootObjects()
        self.assertTrue(rootObjects)
        self.assertTrue(type(rootObjects[0]), ChildClass)

    def testRegisteredTypeCleanup(self):
        script = textwrap.dedent(r"""
        import gc
        import json
        import os
        import sys
        import sysconfig
        import weakref

        from PySide6.QtCore import QCoreApplication, QObject, Property, QUrl
        from PySide6.QtQml import (QQmlComponent, QQmlEngine, qmlClearTypeRegistrations,
                                   qmlRegisterType, qmlTypeId)
        import shiboken6

        uri = 'Codex.RegisteredTypeLifetime'
        finalizers = []
        references = []


        class Payload:
            def __init__(self, index):
                self.index = index

            def __del__(self):
                finalizers.append(qmlTypeId(uri, 1, 0, f'Thing{self.index}'))


        def register(index):
            class Subject(QObject):
                value = Property(int, lambda self: 42, constant=True)
                payload = Payload(index)
            result = qmlRegisterType(Subject, uri, 1, 0, f'Thing{index}')
            assert result >= 0
            references.append(weakref.ref(Subject))


        app = QCoreApplication([])
        for index in range(16):
            register(index)
        for _ in range(3):
            gc.collect()
        assert all(ref() is not None for ref in references)
        engine = QQmlEngine()
        for index in range(16):
            component = QQmlComponent(engine)
            component.setData(f'import {uri} 1.0\nThing{index} {{}}'.encode(), QUrl())
            instance = component.create()
            assert instance is not None, str(component.errors())
            assert instance.property('value') == 42
            shiboken6.delete(instance)
            shiboken6.delete(component)
            del instance, component
        shiboken6.delete(engine)
        del engine
        qmlClearTypeRegistrations()
        for _ in range(3):
            gc.collect()
        retained = sum(ref() is not None for ref in references)
        print(json.dumps(dict(retained_types=retained, finalizers=finalizers,
                              expected_finalizers=16, garbage=len(gc.garbage))), flush=True)
        shiboken6.delete(app)
        del app
        assert retained == 0
        assert finalizers == [-1] * 16
        assert not gc.garbage
        """)
        environment = os.environ.copy()
        environment["PYTHONPATH"] = os.pathsep.join(sys.path)
        for order in ('explicit',):
            with self.subTest(order=order):
                result = subprocess.run([sys.executable, "-c", script, order],
                                        env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout)


    def testAutomaticRegistrationCleanup(self):
        script = textwrap.dedent(r"""
        import atexit
        import gc
        import json
        import os
        import sys
        import sysconfig
        import weakref
        import faulthandler

        faulthandler.enable()

        references = []
        finalizers = []
        uri = 'Codex.AutomaticRegistrationCleanup'


        @atexit.register
        def observe_cleanup():
            # Registered before importing QtCore, so its module shutdown runs first.
            for _ in range(3):
                gc.collect()
            retained = sum(ref() is not None for ref in references)
            result = dict(order=sys.argv[1], retained_types=retained, finalizers=finalizers,
                          engine_valid=shiboken6.isValid(engine),
                          app_present=QCoreApplication.instance() is not None,
                          garbage=len(gc.garbage))
            passed = (retained == 0 and finalizers == [None] * 16
                      and not result['engine_valid'] and not result['app_present']
                      and not result['garbage'])
            print('AUTO_QML_REGISTRATION_CLEANUP ' + ('PASS ' if passed else 'FAIL ')
                  + json.dumps(result), flush=True)


        from PySide6.QtCore import QCoreApplication, QObject, Property
        if sys.argv[1] == 'core-first':
            app = QCoreApplication([])
        from PySide6.QtQml import QQmlApplicationEngine, qmlRegisterType, qmlTypeId
        import shiboken6
        if sys.argv[1] == 'qml-first':
            app = QCoreApplication([])


        class Payload:
            def __init__(self, index):
                self.index = index

            def __del__(self):
                # qmlTypeId needs an application; this observer runs after shutdown.
                finalizers.append(qmlTypeId(uri, 1, 0, f'Thing{self.index}')
                                  if QCoreApplication.instance() is not None else None)


        def register(index):
            class Subject(QObject):
                value = Property(int, lambda self: 42, constant=True)
                payload = Payload(index)
            assert qmlRegisterType(Subject, uri, 1, 0, f'Thing{index}') >= 0
            references.append(weakref.ref(Subject))


        for index in range(16):
            register(index)
        engine = QQmlApplicationEngine()
        print('AUTO_QML_ENGINE_CREATED ' + sys.argv[1], flush=True)
        for index in range(16):
            engine.loadData(f'import {uri} 1.0\nThing{index} {{}}'.encode())
            assert len(engine.rootObjects()) == index + 1
            assert engine.rootObjects()[-1].property('value') == 42
        print('AUTO_QML_ROOTS_CREATED ' + sys.argv[1], flush=True)
        assert all(ref() is not None for ref in references)
        print('AUTO_QML_SETUP_PASS ' + sys.argv[1], flush=True)
        # Deliberately leave the real engine/application alive for normal module shutdown.
        """)
        environment = os.environ.copy()
        environment["PYTHONPATH"] = os.pathsep.join(sys.path)
        for order in ('core-first', 'qml-first'):
            with self.subTest(order=order):
                result = subprocess.run([sys.executable, "-c", script, order],
                                        env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("AUTO_QML_REGISTRATION_CLEANUP PASS ", result.stdout)


if __name__ == '__main__':
    unittest.main()
