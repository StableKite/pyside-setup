# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
from __future__ import annotations

import os
import sys
import threading
import unittest

from concurrent.futures import ThreadPoolExecutor

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths  # noqa: E402
init_test_paths(False)

from PySide6.QtCore import Property, QSize
from PySide6.QtWidgets import QApplication, QMainWindow, QWidget

is_pypy = hasattr(sys, "pypy_version_info")
if not is_pypy:
    from PySide6.support import feature  # noqa

"""
snake_prop_feature_test.py
--------------------------

Test the snake_case and true_property feature.

This works now, including class properties.
"""


class Window(QWidget):
    def __init__(self):
        super().__init__()


@unittest.skipIf(is_pypy, "__feature__ cannot yet be used with PyPy")
class FeatureTest(unittest.TestCase):
    def setUp(self):
        qApp or QApplication()  # noqa
        feature.reset()

    def tearDown(self):
        feature.reset()
        qApp.shutdown()  # noqa

    def testRenamedFunctions(self):
        window = Window()
        window.setWindowTitle('camelCase')

        # and now the same with snake_case enabled
        from __feature__ import snake_case  # noqa

        # Works with the same window! window = Window()
        window.set_window_title('snake_case')

    def testPropertyAppearVanish(self):
        window = Window()

        self.assertTrue(callable(window.isModal))
        with self.assertRaises(AttributeError):
            window.modal

        from __feature__ import snake_case, true_property  # noqa
        # PYSIDE-1548: Make sure that another import does not clear the features.
        import sys  # noqa

        self.assertTrue(isinstance(QWidget.modal, property))
        self.assertTrue(isinstance(window.modal, bool))
        with self.assertRaises(AttributeError):
            window.isModal

        # switching back
        feature.reset()

        self.assertTrue(callable(window.isModal))
        with self.assertRaises(AttributeError):
            window.modal

    def testClassProperty(self):
        from __feature__ import snake_case, true_property  # noqa
        # We check the class...
        self.assertEqual(type(QApplication.quit_on_last_window_closed), bool)
        x = QApplication.quit_on_last_window_closed
        QApplication.quit_on_last_window_closed = not x
        self.assertEqual(QApplication.quit_on_last_window_closed, not x)
        # ... and now the instance.
        self.assertEqual(type(qApp.quit_on_last_window_closed), bool)  # noqa
        x = qApp.quit_on_last_window_closed  # noqa
        qApp.quit_on_last_window_closed = not x  # noqa
        self.assertEqual(qApp.quit_on_last_window_closed, not x)  # noqa
        # make sure values are equal
        self.assertEqual(qApp.quit_on_last_window_closed,  # noqa
                         QApplication.quit_on_last_window_closed)

    def testUserClassNotAffected(self):
        FunctionType = type(lambda: 42)
        # Note: the types module does not have MethodDescriptorType in low versions.
        MethodDescriptorType = type(str.split)

        class UserClass(QWidget):

            def someFunc1(self):
                pass

            @staticmethod
            def someFunc2(a, b):
                pass

        inspect = UserClass.__dict__
        self.assertTrue(isinstance(inspect["someFunc1"], FunctionType))
        self.assertTrue(isinstance(inspect["someFunc2"], staticmethod))
        self.assertTrue(isinstance(UserClass.someFunc2, FunctionType))
        self.assertTrue(isinstance(UserClass.addAction, MethodDescriptorType))

        from __feature__ import snake_case  # noqa

        inspect = UserClass.__dict__
        self.assertTrue(isinstance(inspect["someFunc1"], FunctionType))
        self.assertTrue(isinstance(inspect["someFunc2"], staticmethod))
        self.assertTrue(isinstance(UserClass.someFunc2, FunctionType))
        self.assertTrue(isinstance(UserClass.add_action, MethodDescriptorType))

    def testTrueProperyCanOverride(self):
        from __feature__ import true_property  # noqa

        class CustomWidget(QWidget):
            global prop_result
            prop_result = None

            @Property(QSize)
            def minimumSizeHint(self):
                global prop_result
                print("called")
                prop_result = super().minimumSizeHint
                return prop_result

        window = QMainWindow()
        window.setCentralWidget(CustomWidget(window))
        window.show()
        self.assertTrue(isinstance(prop_result, QSize))

    def testConcurrentTruePropertyDoc(self):
        if not hasattr(sys, "_is_gil_enabled") or sys._is_gil_enabled():
            self.skipTest("requires a free-threaded Python build with the GIL disabled")

        from __feature__ import snake_case, true_property  # noqa

        prop = QWidget.modal
        self.assertIsInstance(prop, property)
        original_doc = prop.__doc__
        self.addCleanup(setattr, prop, "__doc__", original_doc)

        # A lazy reader must never overwrite an explicit __doc__ writer.  Reset
        # the cache before each race so both paths start from the publication
        # point that used to be unsafe without the GIL.
        rounds = 500
        start = threading.Barrier(3)
        done = threading.Barrier(3)

        def lazy_reader():
            for _ in range(rounds):
                start.wait()
                _ = prop.__doc__
                done.wait()

        def explicit_writer():
            for iteration in range(rounds):
                start.wait()
                prop.__doc__ = f"explicit-{iteration}"
                done.wait()

        with ThreadPoolExecutor(max_workers=2) as executor:
            reader_future = executor.submit(lazy_reader)
            writer_future = executor.submit(explicit_writer)
            for iteration in range(rounds):
                prop.__doc__ = None
                start.wait()
                done.wait()
                self.assertEqual(prop.__doc__, f"explicit-{iteration}")
            reader_future.result()
            writer_future.result()

        worker_count = 8
        iterations = 2000
        barrier = threading.Barrier(worker_count)

        def worker(index):
            barrier.wait()
            for iteration in range(iterations):
                if (iteration + index) & 1:
                    prop.__doc__ = f"worker-{index}-{iteration}"
                else:
                    _ = prop.__doc__

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            futures = [executor.submit(worker, index) for index in range(worker_count)]
            for future in futures:
                future.result()

        self.assertFalse(sys._is_gil_enabled())


if __name__ == '__main__':
    unittest.main()
