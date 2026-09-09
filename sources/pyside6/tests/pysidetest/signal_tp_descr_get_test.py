#!/usr/bin/python
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

"""
PYSIDE-68: Test that signals have a `__get__` function after all.

We supply a `tp_descr_get` slot for the signal type.
That creates the `__get__` method via `PyType_Ready`.

The original test script was converted to a unittest (see PYSIDE-68).

Created:    16 May '12 21:25
Updated:    17 Sep '20 17:02

This fix was over 8 years late. :)
"""

import os
import sys
import sysconfig
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QObject, Signal


def emit_upon_success(signal):
    def f_(f):
        def f__(self):
            result = f(self)
            s = signal.__get__(self)  # noqa: F841
            print(result)
            return result
        return f__
    return f_


class Foo(QObject):
    SIG = Signal()

    @emit_upon_success(SIG)
    def do_something(self):
        print("hooka, it worrrks")
        return 42


class UnderUnderGetUnderUnderTest(unittest.TestCase):
    def test_tp_descr_get(self):
        foo = Foo()
        ret = foo.do_something()
        self.assertEqual(ret, 42)

    @unittest.skipUnless(sysconfig.get_config_var("Py_GIL_DISABLED")
                         and not sys._is_gil_enabled(),
                         "Only for GIL disabled builds.")
    def test_concurrent_signal_instance_creation(self):
        foo = Foo()
        thread_count = 8
        start = threading.Barrier(thread_count)
        instances = [None] * thread_count
        failures = []

        def worker(index):
            try:
                start.wait()
                first = None
                for _ in range(2000):
                    instance = foo.SIG
                    if first is None:
                        first = instance
                    elif instance is not first:
                        raise AssertionError("Signal descriptor returned a different instance")
                instances[index] = first
            except BaseException as e:
                failures.append(e)

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        self.assertEqual(failures, [])
        first = instances[0]
        self.assertIsNotNone(first)
        for instance in instances[1:]:
            self.assertIs(instance, first)


if __name__ == "__main__":
    unittest.main()
