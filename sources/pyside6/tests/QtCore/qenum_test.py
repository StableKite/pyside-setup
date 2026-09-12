#!/usr/bin/python
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QEnum and QFlags'''

import gc
import os
import sys
import pickle
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import Qt, QIODevice, QObject, Property, QEnum, QFlag


class TestEnum(unittest.TestCase):
    def testOperations(self):
        k = Qt.Key.Key_1

        # Integers
        self.assertEqual(k + 2, 2 + k)
        self.assertEqual(k - 2, -(2 - k))
        self.assertEqual(k * 2, 2 * k)

    @unittest.skipUnless(getattr(sys, "getobjects", None), "requires --with-trace-refs")
    @unittest.skipUnless(getattr(sys, "gettotalrefcount", None), "requires --with-pydebug")
    def testEnumNew_NoLeak(self):
        gc.collect()
        total = sys.gettotalrefcount()
        for _ in range(1000):
            ret = Qt.Key(42)  # noqa: F841

        gc.collect()
        delta = sys.gettotalrefcount() - total
        print("delta total refcount =", delta)
        if abs(delta) >= 10:
            all = [(sys.getrefcount(x), x) for x in sys.getobjects(0)]
            all.sort(key=lambda x: x[0], reverse=True)
            for ob in all[:10]:
                print(ob)
        self.assertTrue(abs(delta) < 10)


class TestQFlags(unittest.TestCase):

    def testToItn(self):
        om = QIODevice.OpenModeFlag.NotOpen
        omcmp = om.value

        self.assertEqual(om, QIODevice.OpenModeFlag.NotOpen)
        self.assertTrue(omcmp == 0)

        self.assertTrue(omcmp != QIODevice.OpenModeFlag.ReadOnly)
        self.assertTrue(omcmp != 1)

    def testToIntInFunction(self):
        om = QIODevice.OpenModeFlag.WriteOnly
        self.assertEqual(int(om.value), 2)

    def testNonExtensibleEnums(self):
        try:
            om = QIODevice.OpenMode(QIODevice.OpenModeFlag.WriteOnly)  # noqa: F841
            self.assertFail()
        except:  # noqa: E722
            pass


# PYSIDE-15: Pickling of enums
class TestEnumPickling(unittest.TestCase):
    def testPickleEnum(self):

        # Pickling of enums with different depth works.
        ret = pickle.loads(pickle.dumps(QIODevice.OpenModeFlag.Append))
        self.assertEqual(ret, QIODevice.OpenModeFlag.Append)

        ret = pickle.loads(pickle.dumps(Qt.Key.Key_Asterisk))
        self.assertEqual(ret, Qt.Key.Key_Asterisk)
        self.assertEqual(ret, Qt.Key(42))

        # We can also pickle the whole enum class (built in):
        ret = pickle.loads(pickle.dumps(QIODevice))

        # This works also with nested classes for Python 3, after we
        # introduced the correct __qualname__ attribute.
        func = lambda: pickle.loads(pickle.dumps(Qt.Key))  # noqa: E731
        func()

# PYSIDE-957: The QEnum macro


try:
    import enum
    HAVE_ENUM = True
except ImportError:
    HAVE_ENUM = False
    QEnum = QFlag = lambda x: x  # noqa: F811
    import types

    class Enum:
        pass
    enum = types.ModuleType("enum")
    enum.Enum = enum.Flag = enum.IntEnum = enum.IntFlag = Enum
    Enum.__module__ = "enum"
    Enum.__members__ = {}
    del Enum
    # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
    gc.collect()
    enum.auto = lambda: 42

HAVE_FLAG = hasattr(enum, "Flag")


@QEnum
class OuterEnum(enum.Enum):
    A = 1
    B = 2


class SomeClass(QObject):

    @QEnum
    class SomeEnum(enum.Enum):
        A = 1
        B = 2
        C = 3

    @QEnum
    class OtherEnum(enum.IntEnum):
        A = 1
        B = 2
        C = 3
        D = 0x100000000  # >32bit
        E = 0x200000000
        F = 0x400000000

    class InnerClass(QObject):

        @QEnum
        class InnerEnum(enum.Enum):
            X = 42

    class SomeEnum(enum.Enum):  # noqa: F811
        A = 4
        B = 5
        C = 6

    QEnum(SomeEnum)     # works even without the decorator assignment


class TestQEnumMacro(unittest.TestCase):
    meta_name = "EnumType" if sys.version_info[:2] >= (3, 11) else "EnumMeta"

    def testTopLevel(self):
        self.assertEqual(type(OuterEnum).__name__, self.meta_name)
        self.assertEqual(len(OuterEnum.__members__), 2)

    def testSomeClass(self):
        self.assertEqual(type(SomeClass.SomeEnum).__name__, self.meta_name)
        self.assertEqual(len(SomeClass.SomeEnum.__members__), 3)
        with self.assertRaises(TypeError):
            int(SomeClass.SomeEnum.C) == 6
        self.assertEqual(SomeClass.OtherEnum.C, 3)

    def testInnerClass(self):
        self.assertEqual(SomeClass.InnerClass.InnerEnum.__qualname__,
                         "SomeClass.InnerClass.InnerEnum")
        with self.assertRaises(TypeError):
            int(SomeClass.InnerClass.InnerEnum.X) == 42

    @unittest.skipUnless(HAVE_FLAG, "some older Python versions have no 'Flag'")
    def testEnumFlag(self):
        with self.assertRaises(TypeError):
            class WrongFlagForEnum(QObject):
                @QEnum
                class Bad(enum.Flag):
                    pass
        with self.assertRaises(TypeError):
            class WrongEnuForFlag(QObject):
                @QFlag
                class Bad(enum.Enum):
                    pass

    def testIsRegistered(self):
        mo = SomeClass.staticMetaObject
        self.assertEqual(mo.enumeratorCount(), 2)

        # 64 bit / IntEnum
        other_metaenum = mo.enumerator(0)
        self.assertEqual(other_metaenum.scope(), "SomeClass")
        self.assertEqual(other_metaenum.name(), "OtherEnum")
        self.assertTrue(other_metaenum.is64Bit())
        key_count = other_metaenum.keyCount()
        self.assertEqual(key_count, 6)
        self.assertEqual(other_metaenum.value(key_count - 1), SomeClass.OtherEnum.F)
        # Test lookup
        v, ok = other_metaenum.keyToValue("F")
        self.assertTrue(ok)
        self.assertEqual(v, SomeClass.OtherEnum.F)
        v, ok = other_metaenum.keysToValue("E")
        self.assertTrue(ok)
        self.assertEqual(v, SomeClass.OtherEnum.E)

        # 32 bit / Enum
        some_metaenum = mo.enumerator(1)
        self.assertEqual(some_metaenum.scope(), "SomeClass")
        self.assertEqual(some_metaenum.name(), "SomeEnum")
        self.assertFalse(some_metaenum.is64Bit())
        key_count = some_metaenum.keyCount()
        self.assertEqual(key_count, 3)
        self.assertEqual(some_metaenum.value(key_count - 1), SomeClass.SomeEnum.C.value)
        # Test lookup
        v, ok = some_metaenum.keyToValue("C")
        self.assertTrue(ok)
        self.assertEqual(v, SomeClass.SomeEnum.C.value)
        v, ok = some_metaenum.keysToValue("C")
        self.assertTrue(ok)
        self.assertEqual(v, SomeClass.SomeEnum.C.value)

        moi = SomeClass.InnerClass.staticMetaObject
        self.assertEqual(moi.enumerator(0).name(), "InnerEnum")
        self.assertEqual(moi.enumerator(0).scope(), "SomeClass.InnerClass")


def _free_threading_enabled():
    is_gil_enabled = getattr(sys, "_is_gil_enabled", None)
    return is_gil_enabled is not None and not is_gil_enabled()


@unittest.skipUnless(_free_threading_enabled(), "requires a free-threaded Python runtime")
class TestQEnumFreeThreading(unittest.TestCase):
    def testConcurrentDelayedCollectors(self):
        """Parallel class bodies must not exchange their delayed QEnum objects."""
        thread_count = 8
        rounds = 20
        before = threading.Barrier(thread_count)
        after = threading.Barrier(thread_count)
        failures = [None] * thread_count

        def worker(worker_id):
            try:
                for round_id in range(rounds):
                    expected = worker_id * 1000 + round_id + 1
                    class_name = f"ConcurrentQEnum_{worker_id}_{round_id}"
                    source = f"""
class {class_name}(QObject):
    before.wait(timeout=20)
    @QEnum
    class Marker(enum.Enum):
        Value = {expected}
    after.wait(timeout=20)
"""
                    namespace = {
                        "QObject": QObject,
                        "QEnum": QEnum,
                        "enum": enum,
                        "before": before,
                        "after": after,
                    }
                    exec(compile(source, f"<qenum-ft-{worker_id}>", "exec"), namespace)
                    container = namespace[class_name]
                    marker = container.Marker
                    if marker is None or marker.Value.value != expected:
                        raise AssertionError(
                            f"wrong delayed enum for {class_name}: {marker!r}")
                    if container.staticMetaObject.indexOfEnumerator("Marker") < 0:
                        raise AssertionError(f"Marker not registered for {class_name}")
            except BaseException as error:
                failures[worker_id] = repr(error)
                try:
                    before.abort()
                    after.abort()
                except BaseException:
                    pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=60)

        self.assertFalse([thread for thread in threads if thread.is_alive()])
        self.assertEqual(failures, [None] * thread_count)

    def testConcurrentGenericEnumRegistry(self):
        """Convert existing enum properties while new generic enum types register."""
        class EnumTypes(QObject):
            @QEnum
            class Kind(enum.Enum):
                A = 1
                B = 2

        class Holder(QObject):
            def __init__(self):
                super().__init__()
                self._kind = EnumTypes.Kind.A

            def getKind(self):
                return self._kind

            def setKind(self, value):
                self._kind = value

            kind = Property(EnumTypes.Kind, getKind, setKind)

        reader_count = 8
        writer_count = 2
        start = threading.Barrier(reader_count + writer_count)
        failures = [None] * (reader_count + writer_count)

        def reader(index):
            try:
                obj = Holder()
                start.wait(timeout=20)
                for i in range(1200):
                    value = EnumTypes.Kind.A if i % 2 == 0 else EnumTypes.Kind.B
                    if not obj.setProperty("kind", value):
                        raise AssertionError("setProperty() rejected a decorated Python enum")
                    if obj.kind is not value:
                        raise AssertionError("decorated enum conversion returned the wrong member")
            except BaseException as error:
                failures[index] = repr(error)
                try:
                    start.abort()
                except BaseException:
                    pass

        def writer(index):
            slot = reader_count + index
            try:
                start.wait(timeout=20)
                for i in range(120):
                    class_name = f"RegistryQEnum_{index}_{i}"
                    value = index * 1000 + i + 1
                    source = f"""
class {class_name}(QObject):
    @QEnum
    class Marker(enum.Enum):
        Value = {value}
"""
                    namespace = {"QObject": QObject, "QEnum": QEnum, "enum": enum}
                    exec(compile(source, f"<qenum-registry-{index}>", "exec"), namespace)
                    container = namespace[class_name]
                    if container.Marker.Value.value != value:
                        raise AssertionError(f"wrong enum value for {class_name}")
            except BaseException as error:
                failures[slot] = repr(error)
                try:
                    start.abort()
                except BaseException:
                    pass

        threads = [threading.Thread(target=reader, args=(i,)) for i in range(reader_count)]
        threads += [threading.Thread(target=writer, args=(i,)) for i in range(writer_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=90)

        self.assertFalse([thread for thread in threads if thread.is_alive()])
        self.assertEqual(failures, [None] * len(failures))


if __name__ == '__main__':
    unittest.main()
