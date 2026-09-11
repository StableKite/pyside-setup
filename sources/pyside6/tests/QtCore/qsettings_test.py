# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QDate'''

import gc
import os
import subprocess
import sys
import tempfile
import threading
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from PySide6.QtCore import QDir, QSettings, QTemporaryDir, QByteArray


def _free_threaded_pickle_cache_stress():
    if not (hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()):
        return 77

    thread_count = 16
    errors = []
    errors_lock = threading.Lock()

    def record_error(exc):
        with errors_lock:
            errors.append(repr(exc))

    with tempfile.TemporaryDirectory(prefix="pyside-qsettings-ft-") as temp_dir:
        paths = [os.path.join(temp_dir, f"thread-{i}.ini") for i in range(thread_count)]
        write_barrier = threading.Barrier(thread_count)

        def writer(index):
            try:
                settings = QSettings(paths[index], QSettings.Format.IniFormat)
                write_barrier.wait()
                settings.setValue("payload", complex(index, index + 0.5))
                settings.sync()
                if settings.status() != QSettings.Status.NoError:
                    raise RuntimeError(f"writer {index}: status={settings.status()}")
            except BaseException as exc:
                record_error(exc)

        threads = [threading.Thread(target=writer, args=(i,)) for i in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        if errors:
            raise RuntimeError("concurrent QSettings writes failed: " + "; ".join(errors))

        read_barrier = threading.Barrier(thread_count)

        def reader(index):
            try:
                settings = QSettings(paths[index], QSettings.Format.IniFormat)
                read_barrier.wait()
                value = settings.value("payload")
                expected = complex(index, index + 0.5)
                if value != expected or type(value) is not complex:
                    raise RuntimeError(
                        f"reader {index}: expected {expected!r}, got {value!r} ({type(value)!r})")
            except BaseException as exc:
                record_error(exc)

        threads = [threading.Thread(target=reader, args=(i,)) for i in range(thread_count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

    if errors:
        raise RuntimeError("concurrent QSettings reads failed: " + "; ".join(errors))
    return 0


class TestQSettings(unittest.TestCase):
    def testConversions(self):
        file = Path(__file__).resolve().parent / 'qsettings_test.ini'
        self.assertTrue(file.is_file())
        file_path = QDir.fromNativeSeparators(os.fspath(file))
        settings = QSettings(file_path, QSettings.Format.IniFormat)

        r = settings.value('var1')
        self.assertEqual(type(r), list)

        r = settings.value('var2')
        self.assertEqual(type(r), str)

        r = settings.value('var2', type=list)
        self.assertEqual(type(r), list)

        # Test mixed conversions
        ba = QByteArray("hello".encode("utf-8"))

        r = settings.value("test", ba, type=QByteArray)
        self.assertEqual(type(r), QByteArray)

        r = settings.value("test", ba, type=str)
        self.assertEqual(type(r), str)

        # Test invalid conversions
        with self.assertRaises(TypeError):
            r = settings.value("test", ba, type=dict)

    def testDefaultValueConversion(self):
        temp_dir = QDir.tempPath()
        dir = QTemporaryDir(f'{temp_dir}/qsettings_XXXXXX')
        self.assertTrue(dir.isValid())
        file_name = dir.filePath('foo.ini')
        settings = QSettings(file_name, QSettings.Format.IniFormat)
        sample_list = ["a", "b"]
        string_list_of_empty = [""]
        settings.setValue('zero_value', 0)
        settings.setValue('empty_list', [])
        settings.setValue('some_strings', sample_list)
        settings.setValue('string_list_of_empty', string_list_of_empty)
        settings.setValue('bool1', False)
        settings.setValue('bool2', True)
        del settings
        # PYSIDE-535: Need to collect garbage in PyPy to trigger deletion
        gc.collect()

        # Loading values already set
        settings = QSettings(file_name, QSettings.Format.IniFormat)

        # Getting value that doesn't exist
        r = settings.value("variable")
        self.assertEqual(type(r), type(None))

        r = settings.value("variable", type=list)
        self.assertEqual(type(r), list)
        self.assertEqual(len(r), 0)

        # Handling zero value
        r = settings.value('zero_value')
        self.assertEqual(type(r), int)

        r = settings.value('zero_value', type=int)
        self.assertEqual(type(r), int)

        # Empty list
        r = settings.value('empty_list')
        self.assertTrue(len(r) == 0)
        self.assertEqual(type(r), list)

        r = settings.value('empty_list', type=list)
        self.assertTrue(len(r) == 0)
        self.assertEqual(type(r), list)

        r = settings.value('some_strings')
        self.assertEqual(r, sample_list)

        r = settings.value('some_strings', type=list)
        self.assertEqual(r, sample_list)

        r = settings.value('string_list_of_empty', type=list)
        self.assertEqual(r, string_list_of_empty)

        # Booleans
        r = settings.value('bool1')
        self.assertEqual(type(r), bool)

        r = settings.value('bool2')
        self.assertEqual(type(r), bool)

        r = settings.value('bool1', type=bool)
        self.assertEqual(type(r), bool)

        r = settings.value('bool2', type=int)
        self.assertEqual(type(r), int)

        r = settings.value('bool2', type=bool)
        self.assertEqual(type(r), bool)

        # Not set variable, but with default value
        r = settings.value('lala', 22, type=bytes)
        self.assertEqual(type(r), bytes)

        r = settings.value('lala', 22, type=int)
        self.assertEqual(type(r), int)

        r = settings.value('lala', 22, type=float)
        self.assertEqual(type(r), float)

    @unittest.skipUnless(hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled(),
                         "Only for GIL disabled builds.")
    def testFreeThreadedPickleCachePublication(self):
        proc = subprocess.run([sys.executable, __file__, "--free-thread-pickle-cache"],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == '__main__':
    if "--free-thread-pickle-cache" in sys.argv:
        sys.exit(_free_threaded_pickle_cache_stress())
    unittest.main()
