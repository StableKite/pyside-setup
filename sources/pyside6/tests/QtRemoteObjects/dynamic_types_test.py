#!/usr/bin/python
# Copyright (C) 2025 Ford Motor Company
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for dynamic source/replica types'''

import os
import sys
import sysconfig
import threading
import unittest
from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtRemoteObjects import RepFile

from test_shared import wrap_tests_for_cleanup


contents = """
class Simple
{
    PROP(int i = 2);
    PROP(float f = -1. READWRITE);
    SIGNAL(random(int i));
    SLOT(void reset());
};
"""


@wrap_tests_for_cleanup(extra=['rep_file'])
class QDynamicReplicas(unittest.TestCase):
    '''Test case for dynamic Replicas'''

    def setUp(self):
        '''Set up test environment'''
        self.rep_file = RepFile(contents)

    def testDynamicReplica(self):
        '''Verify that a valid Replica is created'''
        Replica = self.rep_file.replica["Simple"]
        self.assertIsNotNone(Replica)
        replica = Replica()
        self.assertIsNotNone(replica)
        self.assertIsNotNone(replica.metaObject())
        meta = replica.metaObject()
        self.assertEqual(meta.className(), "Simple")
        self.assertEqual(meta.superClass().className(), "QRemoteObjectReplica")
        i = meta.indexOfProperty("i")
        self.assertNotEqual(i, -1)
        self.assertEqual(replica.propAsVariant(0), int(2))
        self.assertEqual(replica.propAsVariant(1), float(-1.0))
        self.assertEqual(replica.i, int(2))
        self.assertEqual(replica.f, float(-1.0))


@wrap_tests_for_cleanup(extra=['rep_file'])
class QDynamicSources(unittest.TestCase):
    '''Test case for dynamic Sources'''

    def setUp(self):
        '''Set up test environment'''
        self.rep_file = RepFile(contents)
        self.test_val = 0

    def on_changed(self, val):
        self.test_val = val

    def testDynamicSource(self):
        '''Verify that a valid Source is created'''
        Source = self.rep_file.source["Simple"]
        self.assertIsNotNone(Source)
        source = Source()
        self.assertIsNotNone(source)
        self.assertIsNotNone(source.metaObject())
        meta = source.metaObject()
        self.assertEqual(meta.className(), "SimpleSource")
        self.assertEqual(meta.superClass().className(), "QObject")
        i = meta.indexOfProperty("i")
        self.assertNotEqual(i, -1)
        self.assertIsNotNone(source.__dict__.get('__PROPERTIES__'))
        self.assertEqual(source.i, int(2))
        self.assertEqual(source.f, float(-1.0))
        source.iChanged.connect(self.on_changed)
        source.fChanged.connect(self.on_changed)
        source.i = 7
        self.assertEqual(source.i, int(7))
        self.assertEqual(self.test_val, int(7))
        source.i = 3
        self.assertEqual(self.test_val, int(3))
        source.f = 3.14
        self.assertAlmostEqual(self.test_val, float(3.14), places=5)


def _free_threading_enabled():
    return (sysconfig.get_config_var("Py_GIL_DISABLED")
            and getattr(sys, "_is_gil_enabled", lambda: True)() is False)


@unittest.skipUnless(_free_threading_enabled(), "requires a free-threaded Python runtime")
class QDynamicFreeThreading(unittest.TestCase):
    def testConcurrentRepFileTypeCreation(self):
        """Create dynamic Source/Replica types and defaults from many threads."""
        thread_count = 8
        rounds = 12
        barrier = threading.Barrier(thread_count)
        failures = []

        def worker(index):
            try:
                barrier.wait(timeout=20)
                for round_id in range(rounds):
                    barrier.wait(timeout=20)
                    class_name = f"Concurrent_{index}_{round_id}"
                    pod_name = f"Pod_{index}_{round_id}"
                    source = f"""
POD {pod_name}(int value, QString name)
class {class_name}
{{
    ENUM Kind {{ A, B, C }}
    PROP(int i = {index + round_id + 1});
    PROP(Kind kind);
    PROP({pod_name} data);
    SLOT(void reset());
}};
"""
                    rep = RepFile(source)
                    Pod = rep.pod[pod_name]
                    Source = rep.source[class_name]
                    Replica = rep.replica[class_name]
                    self.assertEqual(Pod.__module__, "PySide6.QtRemoteObjects")
                    pod = Pod(7, "payload")
                    self.assertIn("payload", repr(pod))
                    obj = Source()
                    self.assertEqual(obj.i, index + round_id + 1)
                    self.assertEqual(Source.__base__.__name__, "QObject")
                    self.assertEqual(Replica.__base__.__name__, "QRemoteObjectReplica")
                    Kind = Source.get_enum("Kind")
                    self.assertEqual(Kind.B.value, 1)
                    self.assertIs(Source.get_enum("Kind"), Kind)
            except BaseException as error:  # noqa: BLE001
                failures.append(error)
                try:
                    barrier.abort()
                except BaseException:
                    pass

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=60)
        self.assertTrue(all(not thread.is_alive() for thread in threads),
                        "RemoteObjects type creation deadlocked")
        self.assertEqual(failures, [])


if __name__ == '__main__':
    unittest.main()
