# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
from __future__ import annotations

import os
import sys
import sysconfig
import threading
import unittest

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QObject, Property
from PySide6.QtQmlFeatures import (Change, auto_properties, computed, effect,
                                   load_qml_component, watch)


_FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))


class ParallelAutoPropertiesObject(QObject):
    def __init__(self):
        super().__init__()
        self.value = 1


def _run_parallel(worker, count=8):
    barrier = threading.Barrier(count)

    def run(index):
        barrier.wait()
        return worker(index)

    with ThreadPoolExecutor(max_workers=count) as pool:
        return list(pool.map(run, range(count)))


@unittest.skipUnless(_FREE_THREADED, "Only for GIL disabled builds.")
class FreeThreadingTest(unittest.TestCase):
    def test_import_keeps_gil_disabled(self):
        self.assertFalse(sys._is_gil_enabled())

    def test_change_shared_reinitialization(self):
        change = Change("value", 0, 1, None)

        def worker(index):
            for iteration in range(500):
                Change.__init__(change, f"value{index}", iteration,
                                iteration + 1, index)
                self.assertIsInstance(change.name, str)
                self.assertIn("Change(name=", repr(change))
                _ = change.old
                _ = change.new
                _ = change.owner

        _run_parallel(worker)

    def test_shared_decorator_reinitialization(self):
        computed_decorator = computed("left", "right")
        effect_decorator = effect("left", "right")
        watch_decorator = watch("value")

        def worker(index):
            for iteration in range(250):
                suffix = f"{index}_{iteration}"

                computed.__init__(computed_decorator, f"a_{suffix}", f"b_{suffix}")

                def computed_callback(self):
                    return 0

                computed_decorator(computed_callback)
                metadata = computed_callback._pyside_computed
                self.assertEqual(len(metadata), 2)
                self.assertTrue(all(isinstance(name, str) for name in metadata))

                effect.__init__(effect_decorator, f"a_{suffix}", f"b_{suffix}")

                def effect_callback(self):
                    pass

                effect_decorator(effect_callback)
                metadata = effect_callback._pyside_effect
                self.assertEqual(len(metadata), 2)
                self.assertTrue(all(isinstance(name, str) for name in metadata))

                watch.__init__(watch_decorator, f"value_{suffix}")

                def watch_callback(self, change):
                    pass

                watch_decorator(watch_callback)
                metadata = watch_callback._pyside_watch
                self.assertEqual(len(metadata), 1)
                self.assertIsInstance(metadata[0], str)

        _run_parallel(worker)

    def test_concurrent_metadata_publication(self):
        count = 8
        effect_decorators = [effect(f"effect_{i}") for i in range(count)]
        watch_decorators = [watch(f"watch_{i}") for i in range(count)]

        for _ in range(20):
            def effect_callback(self):
                pass

            _run_parallel(lambda index: effect_decorators[index](effect_callback), count)
            self.assertEqual(set(effect_callback._pyside_effect),
                             {f"effect_{i}" for i in range(count)})

            def watch_callback(self, change):
                pass

            _run_parallel(lambda index: watch_decorators[index](watch_callback), count)
            self.assertEqual(set(watch_callback._pyside_watch),
                             {f"watch_{i}" for i in range(count)})

    def test_auto_properties_same_class(self):
        results = _run_parallel(
            lambda _index: auto_properties(ParallelAutoPropertiesObject))
        self.assertTrue(all(result is ParallelAutoPropertiesObject for result in results))
        self.assertTrue(ParallelAutoPropertiesObject._pyside_auto_props_applied)
        self.assertIsInstance(ParallelAutoPropertiesObject.__dict__["value"], Property)

    def test_qml_component_helper_cache(self):
        def worker(_index):
            for _ in range(50):
                with self.assertRaises(TypeError):
                    load_qml_component(None, "unused.qml")

        _run_parallel(worker)


if __name__ == "__main__":
    unittest.main()
