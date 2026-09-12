# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import concurrent.futures
import sys
import sysconfig
import types
import unittest

from PySide6.QtCore import QObject


def _is_free_threaded():
    return bool(sysconfig.get_config_var("Py_GIL_DISABLED")) and not sys._is_gil_enabled()


def _make_class(module_name, feature_line, body):
    module = types.ModuleType(module_name)
    module.__dict__["QObject"] = QObject
    sys.modules[module_name] = module
    source = ""
    if feature_line:
        source += f"from __feature__ import {feature_line}\n"
    source += "class Probe(QObject):\n"
    source += "    def run(self, value):\n"
    for line in body:
        source += f"        {line}\n"
    exec(compile(source, f"<{module_name}>", "exec"), module.__dict__)
    return module.Probe


def _make_multiple_inheritance_class():
    module_name = "_feature_super_multiple_inheritance"
    module = types.ModuleType(module_name)
    module.__dict__["QObject"] = QObject
    sys.modules[module_name] = module
    source = """from __feature__ import snake_case
class Fallback:
    def objectName(self):
        return "fallback"

class Probe(QObject, Fallback):
    def run(self):
        return super().objectName()
"""
    exec(compile(source, f"<{module_name}>", "exec"), module.__dict__)
    return module.Probe


class FeatureSuperFreeThreadingTest(unittest.TestCase):
    def test_concurrent_builtin_super_feature_contexts(self):
        if not _is_free_threaded():
            self.skipTest("requires a free-threaded Python runtime with the GIL disabled")

        # QObject was imported/materialized above before any __feature__ import.
        cases = [
            _make_class("_feature_super_camel", "", [
                "self.setObjectName(value)",
                "return super().objectName()",
            ]),
            _make_class("_feature_super_snake", "snake_case", [
                "self.set_object_name(value)",
                "return super().object_name()",
            ]),
            _make_class("_feature_super_prop", "true_property", [
                "self.objectName = value",
                "return super().objectName",
            ]),
            _make_class("_feature_super_combo", "snake_case, true_property", [
                "self.object_name = value",
                "return super().object_name",
            ]),
        ]

        workers = 16
        rounds = 750

        def work(index):
            cls = cases[index % len(cases)]
            for iteration in range(rounds):
                expected = f"{index}:{iteration}"
                obj = cls()
                actual = obj.run(expected)
                if actual != expected:
                    return index, iteration, expected, actual
            return None

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
            failures = [result for result in executor.map(work, range(workers)) if result]

        self.assertEqual(failures, [])
        self.assertFalse(sys._is_gil_enabled())

    def test_builtin_super_continues_through_runtime_mro(self):
        if not _is_free_threaded():
            self.skipTest("requires a free-threaded Python runtime with the GIL disabled")

        # In the snake_case feature variant QObject.objectName is absent. builtin
        # super() still sees the canonical non-data proxy in QObject.__dict__, so
        # that proxy must continue through Probe's runtime MRO and reach the later
        # Python sibling instead of stopping at QObject's own MRO.
        probe_type = _make_multiple_inheritance_class()
        self.assertEqual(probe_type().run(), "fallback")
        self.assertFalse(sys._is_gil_enabled())


if __name__ == "__main__":
    unittest.main()
