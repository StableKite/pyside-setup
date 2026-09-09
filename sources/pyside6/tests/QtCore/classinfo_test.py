# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import os
import sys
import sysconfig
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QObject, QCoreApplication, ClassInfo


class TestClassInfo(unittest.TestCase):
    def test_metadata(self):
        @ClassInfo(author='pyside', url='http://www.pyside.org')
        class MyObject(QObject):
            pass

        o = MyObject()
        mo = o.metaObject()
        self.assertEqual(mo.classInfoCount(), 2)

        ci = mo.classInfo(0)  # author
        self.assertEqual(ci.name(), 'author')
        self.assertEqual(ci.value(), 'pyside')

        ci = mo.classInfo(1)  # url
        self.assertEqual(ci.name(), 'url')
        self.assertEqual(ci.value(), 'http://www.pyside.org')

    def test_dictionary(self):
        @ClassInfo({'author': 'pyside', 'author company': 'The Qt Company'})
        class MyObject(QObject):
            pass

        o = MyObject()
        mo = o.metaObject()
        self.assertEqual(mo.classInfoCount(), 2)

        ci = mo.classInfo(0)  # author
        self.assertEqual(ci.name(), 'author')
        self.assertEqual(ci.value(), 'pyside')

        ci = mo.classInfo(1)  # url
        self.assertEqual(ci.name(), 'author company')
        self.assertEqual(ci.value(), 'The Qt Company')

    def test_verify_metadata_types(self):
        valid_dict = {'123': '456'}

        invalid_dict_1 = {'123': 456}
        invalid_dict_2 = {123: 456}
        invalid_dict_3 = {123: '456'}

        ClassInfo(**valid_dict)

        self.assertRaises(TypeError, ClassInfo, **invalid_dict_1)

        # assertRaises only allows for string keywords, so a `try` must be used here.
        try:
            ClassInfo(**invalid_dict_2)
            self.fail('ClassInfo() accepted invalid_dict_2!')
        except TypeError:
            pass

        try:
            ClassInfo(**invalid_dict_3)
            self.fail('ClassInfo() accepted invalid_dict_3!')
        except TypeError:
            pass


    def test_concurrent_metadata_updates(self):
        class SharedObject(QObject):
            pass

        thread_count = 8
        entries_per_thread = 25
        errors = []
        start = threading.Barrier(thread_count + 1)

        def writer(thread_id):
            try:
                start.wait()
                for i in range(entries_per_thread):
                    key = f"ft_{thread_id}_{i}"
                    value = f"value_{thread_id}_{i}"
                    ClassInfo({key: value})(SharedObject)
                    # Force the builder to publish intermediate meta objects
                    # while other threads continue mutating it.
                    obj = SharedObject()
                    obj.metaObject().classInfoCount()
            except BaseException as e:
                errors.append(e)

        threads = [threading.Thread(target=writer, args=(i,))
                   for i in range(thread_count)]
        for thread in threads:
            thread.start()
        start.wait()
        for thread in threads:
            thread.join()

        self.assertEqual(errors, [])

        obj = SharedObject()
        meta_object = obj.metaObject()
        actual = {}
        for i in range(meta_object.classInfoOffset(), meta_object.classInfoCount()):
            info = meta_object.classInfo(i)
            actual[info.name()] = info.value()

        expected = {f"ft_{thread_id}_{i}": f"value_{thread_id}_{i}"
                    for thread_id in range(thread_count)
                    for i in range(entries_per_thread)}
        self.assertEqual({key: actual.get(key) for key in expected}, expected)

        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.assertFalse(sys._is_gil_enabled())

    def test_can_not_use_instance_twice(self):
        decorator = ClassInfo(author='pyside', url='http://www.pyside.org')

        @decorator
        class MyObject1(QObject):
            pass

        class MyObject2(QObject):
            pass

        self.assertRaises(TypeError, decorator, MyObject2)

    def test_can_only_be_used_on_qobjects(self):
        def make_info():
            return ClassInfo(author='pyside')

        def test_function():
            pass
        self.assertRaises(TypeError, make_info(), test_function)

        class NotAQObject:
            pass
        self.assertRaises(TypeError, make_info(), NotAQObject)

        class QObjectSubclass(QObject):
            pass
        make_info()(QObjectSubclass)

        class SubclassOfNativeQObjectSubclass(QCoreApplication):
            pass
        make_info()(SubclassOfNativeQObjectSubclass)

        class SubclassOfPythonQObjectSubclass(QObjectSubclass):
            pass
        make_info()(SubclassOfPythonQObjectSubclass)


if __name__ == '__main__':
    unittest.main()
