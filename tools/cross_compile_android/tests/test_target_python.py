# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from jinja2 import Environment, FileSystemLoader

TEST_DIR = Path(__file__).resolve().parent
TOOL_DIR = TEST_DIR.parent
REPO_DIR = TOOL_DIR.parents[1]
sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(REPO_DIR))

from android_utilities import inspect_target_python  # noqa: E402
from main import _PLATFORM_DATA  # noqa: E402
from build_scripts.wheel_override import PysideBuildWheel  # noqa: E402


class TargetPythonInfoTest(unittest.TestCase):
    def _make_prefix(self, version: str = "3.14", *, free_threaded: bool = False) -> Path:
        temp_dir = Path(tempfile.mkdtemp())
        suffix = "t" if free_threaded else ""
        include_dir = temp_dir / "include" / f"python{version}{suffix}"
        include_dir.mkdir(parents=True)
        (include_dir / "Python.h").write_text("/* probe */\n", encoding="utf-8")
        gil_define = "#define Py_GIL_DISABLED 1\n" if free_threaded else ""
        (include_dir / "pyconfig.h").write_text(gil_define, encoding="utf-8")
        lib_dir = temp_dir / "lib"
        lib_dir.mkdir()
        (lib_dir / f"libpython{version}{suffix}.so").touch()
        self.addCleanup(shutil.rmtree, temp_dir)
        return temp_dir

    def test_regular_target(self):
        prefix = self._make_prefix()
        info = inspect_target_python(prefix, expected_version="3.14")
        self.assertFalse(info.free_threaded)
        self.assertEqual(info.so_abi, "cpython-314")
        self.assertEqual(info.include_dir.name, "python3.14")
        self.assertEqual(info.library.name, "libpython3.14.so")

    def test_free_threaded_target(self):
        prefix = self._make_prefix(free_threaded=True)
        info = inspect_target_python(prefix, expected_version="3.14")
        self.assertTrue(info.free_threaded)
        self.assertEqual(info.so_abi, "cpython-314t")
        self.assertEqual(info.include_dir.name, "python3.14t")
        self.assertEqual(info.library.name, "libpython3.14t.so")

    def test_rejects_inconsistent_free_threaded_prefix(self):
        prefix = self._make_prefix(free_threaded=True)
        pyconfig = prefix / "include" / "python3.14t" / "pyconfig.h"
        pyconfig.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "ABI is inconsistent"):
            inspect_target_python(prefix)

    def test_free_threaded_cross_wheel_tag(self):
        class BuildCommand:
            python_target_info = {
                "python_info": {"version": "3.14.7", "so_abi": "cpython-314t"}
            }

        class WheelCommand:
            @staticmethod
            def get_finalized_command(name):
                if name != "build":
                    raise AssertionError(name)
                return BuildCommand()

        tag = PysideBuildWheel.get_cross_compiling_tag_tuple(
            WheelCommand(), ("cp311", "cp311", "android_aarch64"))
        self.assertEqual(tag, ("cp314", "cp314t", "android_aarch64"))

    def test_platform_compiler_flags_match_abi(self):
        aarch64_flags = _PLATFORM_DATA["aarch64"][2]
        x86_64_flags = _PLATFORM_DATA["x86_64"][2]
        self.assertIn("-march=armv8-a", aarch64_flags)
        self.assertNotIn("-msse", aarch64_flags)
        self.assertNotIn("-m64", aarch64_flags)
        self.assertIn("-march=x86-64", x86_64_flags)
        self.assertIn("-msse4.2", x86_64_flags)

    def test_free_threaded_toolchain_uses_target_artifacts(self):
        prefix = self._make_prefix(free_threaded=True)
        info = inspect_target_python(prefix)
        environment = Environment(loader=FileSystemLoader(TOOL_DIR / "templates"))
        template = environment.get_template("toolchain_default.tmpl.cmake")
        content = template.render(
            ndk_path="/ndk",
            sdk_path="/sdk",
            api_level="35",
            qt_install_path="/qt",
            plat_name="aarch64",
            android_abi="arm64-v8a",
            qt_plat_name="arm64_v8a",
            compiler_flags="-march=armv8-a",
            target_python_path=prefix,
            python_include_dir=info.include_dir,
            python_library=info.library,
            python_soabi=info.so_abi,
            python_free_threaded="ON",
            min_android_api="28",
        )
        self.assertIn(f"set(Python_INCLUDE_DIR {info.include_dir}", content)
        self.assertIn(f"set(Python_LIBRARY {info.library}", content)
        self.assertIn("set(Python_SOABI cpython-314t", content)
        self.assertIn("set(QFP_PYTHON_FREE_THREADED ON", content)
        self.assertIn(f"-I{info.include_dir}", content)


if __name__ == "__main__":
    unittest.main()
