# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import gc
import os
import sysconfig
import weakref
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from shiboken_paths import init_paths
init_paths()

from sample import Point


class MetaA(type):
    pass


class A:
    __metaclass__ = MetaA


MetaB = type(Point)
B = Point


class MetaC(MetaA, MetaB):
    pass


class C(A, B):
    __metaclass__ = MetaC


class D(C):
    pass


class TestMetaClass(unittest.TestCase):
    def test_type_instance_cycle_is_collectible(self):
        def make_cycle():
            class Ephemeral(Point):
                pass
            instance = Ephemeral()
            Ephemeral.instance = instance
            return weakref.ref(Ephemeral), weakref.ref(instance)

        references = make_cycle()
        for _ in range(3):
            gc.collect()
        self.assertTrue(all(reference() is None for reference in references))

    @unittest.skipUnless(sysconfig.get_config_var("Py_GIL_DISABLED"),
                         "Requires the native free-threaded heap-type layout")
    def test_collected_types_release_native_base_references(self):
        def make_types(count):
            return [weakref.ref(type(f"Ephemeral{index}", (Point,),
                                     {"__module__": __name__}))
                    for index in range(count)]

        make_types(5)
        for _ in range(3):
            gc.collect()
        before = sys.getrefcount(Point)
        references = make_types(100)
        for _ in range(3):
            gc.collect()
        self.assertTrue(all(reference() is None for reference in references))
        self.assertEqual(sys.getrefcount(Point), before)

    def testIt(self):
        w1 = C()  # works
        w1.setX(1)
        w1.setY(2)

        w2 = D()  # should work!
        w2.setX(3)
        w2.setY(4)

        w3 = w1 + w2
        self.assertEqual(w3.x(), 4)
        self.assertEqual(w3.y(), 6)
