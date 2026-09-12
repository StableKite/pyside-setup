#!/usr/bin/env python
# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

"""Stress the binding bookkeeping from many threads with the GIL disabled.

Each scenario hammers a path that the state lock protects: the wrapper
lifecycle, the ownership flags and the call leases. Without that lock these
scenarios segfault reliably, so a clean run is what keeps the lock honest.

Scenarios that race a shared C++ user object do not belong here. Mutating
ObjectType::children from several threads is a defect in the calling code,
and no binding-level lock is meant to make it safe; the coarse lock used to
serialize it as a side effect, which made it look like binding coverage.

The defaults are sized for CI. Set PYSIDE_STRESS_THREADS / PYSIDE_STRESS_ITERS
to hunt for races locally; tests/manually/freethreading/run.py drives the same
scenarios as an A/B proof against a build with the lock disabled at runtime.
"""

from __future__ import annotations

import inspect
import os
import sys
import sysconfig
import threading
import types
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from shiboken_paths import init_paths
init_paths()

import sample
from sample import Event, ObjectType, Point, VirtualMethods
from shiboken6 import Shiboken

THREADS = int(os.environ.get("PYSIDE_STRESS_THREADS", "8"))
ITERS = int(os.environ.get("PYSIDE_STRESS_ITERS", "1500"))

MSG_SKIP = "Only for GIL disabled builds."


def is_gil_disabled():
    gil_disabled_build = sysconfig.get_config_vars('Py_GIL_DISABLED')[0]
    return gil_disabled_build and not sys._is_gil_enabled()


class ThreadedVirtual(VirtualMethods):
    def virtualMethod0(self, pt, val, cpx, b):
        return float(val)


@unittest.skipUnless(is_gil_disabled(), MSG_SKIP)
class ObjectGraphStressTest(unittest.TestCase):

    def spin(self, work):
        """Run work in THREADS threads, released together for contention."""
        failures = []
        barrier = threading.Barrier(THREADS)

        def wrapped(idx):
            barrier.wait()
            try:
                work(idx)
            except BaseException as e:  # noqa: BLE001
                failures.append(e)

        threads = [threading.Thread(target=wrapped, args=(i,))
                   for i in range(THREADS)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        self.assertEqual(failures, [], f"Exceptions in worker threads: {failures}")

    def test_destroy_race(self):
        """Destroy parents that still own a child."""
        def work(idx):
            for _ in range(ITERS):
                parent = ObjectType()
                child = ObjectType()
                child.setParent(parent)
                Shiboken.delete(parent)

        self.spin(work)

    def test_call_vs_delete(self):
        """Call methods on a wrapper while other threads destroy it.

        This is what the call lease exists for: a call that passed the
        validity check must keep the C++ object alive until it returns, and a
        call on a destroyed wrapper must raise RuntimeError, never crash.
        """
        pool_size = 32
        pool = [ObjectType() for _ in range(pool_size)]

        def work(idx):
            for i in range(ITERS):
                slot = (i * 13 + idx) % pool_size
                obj = pool[slot]
                try:
                    obj.objectName()
                    obj.setObjectName("x")
                except RuntimeError:
                    pass                       # already deleted: correct
                if i % 8 == idx % 8:
                    Shiboken.delete(obj)
                    pool[slot] = ObjectType()  # list assignment is atomic

        self.spin(work)


    def test_instance_dict_initialization(self):
        """Race first access to the same wrapper instance dictionary."""
        obj = ObjectType()
        dictionaries = [None] * THREADS

        def work(idx):
            first = None
            for i in range(ITERS):
                dictionary = obj.__dict__
                if first is None:
                    first = dictionary
                else:
                    self.assertIs(dictionary, first)
                dictionary[f"thread_{idx}"] = i
            dictionaries[idx] = first

        self.spin(work)
        first = dictionaries[0]
        self.assertIsNotNone(first)
        for dictionary in dictionaries[1:]:
            self.assertIs(dictionary, first)

    def test_enum_missing_value_publication(self):
        """Race creation of one permissive out-of-range enum value."""
        enum_type = Event.EventType
        value = 0x5A17
        results = [None] * THREADS

        def work(idx):
            first = None
            for _ in range(ITERS):
                item = enum_type(value)
                self.assertEqual(item.value, value)
                self.assertIs(type(item), enum_type)
                if first is None:
                    first = item
                else:
                    self.assertIs(item, first)
            results[idx] = first

        self.spin(work)
        first = results[0]
        self.assertIsNotNone(first)
        for item in results[1:]:
            self.assertIs(item, first)
        self.assertIs(enum_type(value), first)

    def test_named_keyword_arguments(self):
        """Stress generated named-argument wrappers with free-threaded callers."""
        def work(idx):
            obj = ObjectType()
            for i in range(ITERS):
                prefix = f"t{idx}-"
                suffix = str(i)
                obj.setObjectSplittedName("", prefix=prefix, suffix=suffix)
                self.assertEqual(obj.objectName(), prefix + suffix)

                name = f"worker-{idx}-{i}"
                size = (i % len(name)) + 1
                obj.setObjectNameWithSize(name=name, size=size)
                self.assertEqual(obj.objectName(), name[:size])

        self.spin(work)

    def test_converter_registry(self):
        """Race converter lookups, including negative-cache eviction."""
        def work(idx):
            for i in range(ITERS):
                # Existing converter lookup and a stream of unique misses.
                self.assertTrue(sample.cppTypeIsObjectType("ObjectType"))
                name = f"MissingConverter_{idx}_{i}"
                with self.assertRaises(ValueError):
                    sample.cppTypeIsObjectType(name)

        self.spin(work)

    def test_virtual_override_lookup(self):
        """Race virtual override lookup and per-instance monkey patching."""
        point = Point(1.0, 2.0)

        def work(idx):
            obj = ThreadedVirtual()

            def patched(self, pt, val, cpx, b):
                return float(val + 1)

            for i in range(ITERS):
                if i & 1:
                    obj.virtualMethod0 = types.MethodType(patched, obj)
                    expected = float(idx + 1)
                else:
                    # Removing the instance override exercises the generated
                    # cache reset path; the class override must become visible
                    # immediately on the next C++ virtual call.
                    obj.__dict__.pop("virtualMethod0", None)
                    expected = float(idx)
                self.assertEqual(obj.callVirtualMethod0(point, idx, 0j, False), expected)

        self.spin(work)

    def test_signature_cache(self):
        """Race lazy signature/doc creation for generated binding callables."""
        targets = (
            ObjectType.setObjectNameWithSize,
            ObjectType.setObjectSplittedName,
            VirtualMethods.callVirtualMethod0,
        )
        results = [None] * THREADS

        def work(idx):
            for i in range(ITERS):
                target = targets[(i + idx) % len(targets)]
                signature = inspect.signature(target)
                self.assertIn("(", str(signature))
                doc = target.__doc__
                self.assertTrue(doc is None or isinstance(doc, str))
            results[idx] = tuple(str(inspect.signature(target)) for target in targets)

        self.spin(work)
        first = results[0]
        self.assertIsNotNone(first)
        for result in results[1:]:
            self.assertEqual(result, first)

    def test_shared_delete(self):
        """Race to destroy the same wrappers.

        Deleting an already invalidated wrapper must be a safe no-op, and two
        threads doing it at once must not double free.
        """
        pool_size = 400
        pool = [ObjectType() for _ in range(pool_size)]
        lock = threading.Lock()

        def work(idx):
            for i in range(ITERS):
                slot = (i * 7 + idx) % pool_size
                Shiboken.delete(pool[slot])
                if idx == 0:  # one thread refills, racing the deleters
                    with lock:
                        pool[slot] = ObjectType()

        self.spin(work)


if __name__ == "__main__":
    unittest.main()
