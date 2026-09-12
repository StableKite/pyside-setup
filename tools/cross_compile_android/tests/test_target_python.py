# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

from __future__ import annotations

import shutil
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from zipfile import ZipFile

from jinja2 import Environment, FileSystemLoader

TEST_DIR = Path(__file__).resolve().parent
TOOL_DIR = TEST_DIR.parent
REPO_DIR = TOOL_DIR.parents[1]
sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(REPO_DIR))

from android_utilities import (  # noqa: E402
    inspect_target_python,
    snapshot_android_wheels,
    changed_android_wheels,
    validate_free_threaded_android_wheels,
    validate_android_target_python_architecture,
)
from main import (  # noqa: E402
    _PLATFORM_DATA,
    resolve_target_python_paths,
    validate_target_python_for_platform,
)
from build_scripts.wheel_override import PysideBuildWheel  # noqa: E402


class TargetPythonInfoTest(unittest.TestCase):
    def _make_prefix(self, version: str = "3.14", *, free_threaded: bool = False,
                     machine: int | None = None) -> Path:
        temp_dir = Path(tempfile.mkdtemp())
        suffix = "t" if free_threaded else ""
        include_dir = temp_dir / "include" / f"python{version}{suffix}"
        include_dir.mkdir(parents=True)
        (include_dir / "Python.h").write_text("/* probe */\n", encoding="utf-8")
        gil_define = "#define Py_GIL_DISABLED 1\n" if free_threaded else ""
        (include_dir / "pyconfig.h").write_text(gil_define, encoding="utf-8")
        lib_dir = temp_dir / "lib"
        lib_dir.mkdir()
        library = lib_dir / f"libpython{version}{suffix}.so"
        if machine is None:
            library.touch()
        else:
            library.write_bytes(self._elf_header(machine) + b"libpython")
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

    def test_dual_abi_target_python_paths(self):
        paths = resolve_target_python_paths(
            ["aarch64", "x86_64"], None, "/python/arm64", "/python/x86_64")
        self.assertEqual(paths, {
            "aarch64": "/python/arm64",
            "x86_64": "/python/x86_64",
        })

    def test_single_generic_target_python_path_is_preserved(self):
        paths = resolve_target_python_paths(["aarch64"], "/python/one", None, None)
        self.assertEqual(paths, {"aarch64": "/python/one"})

    def test_generic_target_python_path_rejects_dual_abi(self):
        with self.assertRaisesRegex(ValueError, "exactly one --plat-name"):
            resolve_target_python_paths(
                ["aarch64", "x86_64"], "/python/one", None, None)

    def test_target_python_paths_reject_mixed_modes(self):
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            resolve_target_python_paths(
                ["aarch64"], "/python/one", "/python/arm64", None)

    def test_target_python_paths_reject_partial_dual_abi(self):
        with self.assertRaisesRegex(ValueError, "missing: x86_64"):
            resolve_target_python_paths(
                ["aarch64", "x86_64"], None, "/python/arm64", None)

    def test_target_python_paths_reject_unused_abi(self):
        with self.assertRaisesRegex(ValueError, "unrequested platform"):
            resolve_target_python_paths(
                ["aarch64"], None, "/python/arm64", "/python/x86_64")

    def test_require_free_threaded_rejects_regular_target(self):
        target = inspect_target_python(self._make_prefix())
        with self.assertRaisesRegex(RuntimeError, "requires free-threaded target Python"):
            validate_target_python_for_platform(target, "aarch64", True)

    @staticmethod
    def _elf_header(machine: int) -> bytes:
        ident = b"\x7fELF" + bytes([2, 1, 1, 0, 0]) + bytes(7)
        return ident + struct.pack("<HHI", 3, machine, 1)

    def _make_ft_wheel(self, root: Path, distribution: str, plat_name: str, machine: int,
                       *, abi: str = "cp314t", member_abi: str = "cpython-314t") -> Path:
        wheel = root / f"{distribution}-6.11.0-cp314-{abi}-android_{plat_name}.whl"
        dist_info = f"{distribution}-6.11.0.dist-info"
        package = "PySide6" if distribution == "PySide6" else "shiboken6"
        extension = f"{package}/probe.{member_abi}.so"
        tag = f"cp314-{abi}-android_{plat_name}"
        with ZipFile(wheel, "w") as archive:
            archive.writestr(f"{dist_info}/WHEEL", f"Wheel-Version: 1.0\nTag: {tag}\n")
            archive.writestr(extension, self._elf_header(machine) + b"probe")
        return wheel

    def test_free_threaded_android_wheel_pair_validation(self):
        for plat_name, machine in (("aarch64", 183), ("x86_64", 62)):
            with self.subTest(plat_name=plat_name), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                target = inspect_target_python(
                    self._make_prefix(free_threaded=True, machine=machine))
                wheels = [
                    self._make_ft_wheel(root, "PySide6", plat_name, machine),
                    self._make_ft_wheel(root, "shiboken6", plat_name, machine),
                ]
                validated = validate_free_threaded_android_wheels(
                    wheels, plat_name, target)
                self.assertEqual(
                    {wheel.name for wheel in validated}, {wheel.name for wheel in wheels})

    def test_free_threaded_target_rejects_wrong_android_architecture(self):
        target = inspect_target_python(
            self._make_prefix(free_threaded=True, machine=62))
        with self.assertRaisesRegex(RuntimeError, "Target Python ELF machine mismatch"):
            validate_android_target_python_architecture(target, "aarch64")

    def test_free_threaded_android_wheel_rejects_wrong_elf_machine(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            target = inspect_target_python(
                self._make_prefix(free_threaded=True, machine=183))
            wheels = [
                self._make_ft_wheel(root, "PySide6", "aarch64", 62),
                self._make_ft_wheel(root, "shiboken6", "aarch64", 183),
            ]
            with self.assertRaisesRegex(RuntimeError, "ELF machine mismatch"):
                validate_free_threaded_android_wheels(wheels, "aarch64", target)

    def test_free_threaded_android_wheel_rejects_abi3(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            target = inspect_target_python(
                self._make_prefix(free_threaded=True, machine=62))
            wheels = [
                self._make_ft_wheel(root, "PySide6", "x86_64", 62,
                                    abi="abi3", member_abi="abi3"),
                self._make_ft_wheel(root, "shiboken6", "x86_64", 62),
            ]
            with self.assertRaisesRegex(RuntimeError, "wrong tag"):
                validate_free_threaded_android_wheels(wheels, "x86_64", target)

    def test_android_wheel_snapshot_excludes_stale_artifacts(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            stale = self._make_ft_wheel(root, "PySide6", "aarch64", 183)
            before = snapshot_android_wheels(root)
            self.assertEqual(changed_android_wheels(before, root), [])

            # Rewrite the same path to model bdist_wheel replacing an existing artifact.
            time.sleep(0.002)
            with ZipFile(stale, "a") as archive:
                archive.writestr("build-marker", b"new")
            self.assertEqual(changed_android_wheels(before, root), [stale.resolve()])

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
