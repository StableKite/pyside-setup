# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

"""
iterable_test.py

This test checks that the Iterable protocol is implemented correctly.
"""

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

import PySide6
from PySide6 import QtCore, QtGui

try:
    import numpy as np
    have_numpy = True
except ImportError:
    have_numpy = False


class PySequenceTest(unittest.TestCase):

    def test_iterable(self):
        def gen(lis):
            for item in lis:
                if item == "crash":
                    raise IndexError
                yield item
        # testing "pyseq_to_cpplist_conversion"
        testfunc = QtCore.QUrl.fromStringList
        # use a generator (iterable)
        self.assertEqual(testfunc(gen(["asd", "ghj"])),
                         [PySide6.QtCore.QUrl('asd'), PySide6.QtCore.QUrl('ghj')])
        # use an iterator
        self.assertEqual(testfunc(iter(["asd", "ghj"])),
                         [PySide6.QtCore.QUrl('asd'), PySide6.QtCore.QUrl('ghj')])
        self.assertRaises(IndexError, testfunc, gen(["asd", "crash", "ghj"]))
        # testing QMatrix4x4
        testfunc = QtGui.QMatrix4x4
        self.assertEqual(testfunc(gen(range(16))), testfunc(range(16)))
        # Note: The errormessage needs to be improved!
        # We should better get a ValueError
        self.assertRaises((TypeError, ValueError), testfunc, gen(range(15)))
        # All other matrix sizes:
        testfunc = QtGui.QMatrix2x2
        self.assertEqual(testfunc(gen(range(4))), testfunc(range(4)))
        testfunc = QtGui.QMatrix2x3
        self.assertEqual(testfunc(gen(range(6))), testfunc(range(6)))

    def test_matrix_invalid_size_can_retry(self):
        import shiboken6
        for length in (0, 15, 17):
            with self.subTest(length=length):
                matrix = QtGui.QMatrix4x4.__new__(QtGui.QMatrix4x4)
                with self.assertRaises(ValueError):
                    QtGui.QMatrix4x4.__init__(matrix, iter(range(length)))
                self.assertFalse(shiboken6.isValid(matrix))
                QtGui.QMatrix4x4.__init__(matrix, range(16))
                self.assertEqual(matrix, QtGui.QMatrix4x4(range(16)))

    def test_matrix_iterator_error_can_retry(self):
        import shiboken6
        error = IndexError("matrix iterator witness")

        def values():
            yield 1
            raise error

        matrix = QtGui.QMatrix4x4.__new__(QtGui.QMatrix4x4)
        with self.assertRaises(IndexError) as caught:
            QtGui.QMatrix4x4.__init__(matrix, values())
        self.assertIs(caught.exception, error)
        self.assertFalse(shiboken6.isValid(matrix))
        QtGui.QMatrix4x4.__init__(matrix, range(16))
        self.assertTrue(shiboken6.isValid(matrix))

    def test_matrix_float_error_can_retry(self):
        import shiboken6
        error = ValueError("matrix float witness")

        class BrokenFloat:
            def __float__(self):
                raise error

        matrix = QtGui.QMatrix4x4.__new__(QtGui.QMatrix4x4)
        with self.assertRaises(ValueError) as caught:
            QtGui.QMatrix4x4.__init__(matrix, [BrokenFloat()] + list(range(15)))
        self.assertIs(caught.exception, error)
        self.assertFalse(shiboken6.isValid(matrix))
        QtGui.QMatrix4x4.__init__(matrix, range(16))
        self.assertTrue(shiboken6.isValid(matrix))

    @unittest.skipUnless(have_numpy, "requires numpy")
    def test_iterable_numpy(self):
        # Demo for numpy: We create a unit matrix.
        num_mat = np.eye(4)
        num_mat.shape = 16
        unit = QtGui.QMatrix4x4(num_mat)
        self.assertEqual(unit, QtGui.QMatrix4x4())


if __name__ == '__main__':
    unittest.main()
