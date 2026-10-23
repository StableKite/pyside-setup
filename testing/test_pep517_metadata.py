# Copyright (C) 2026
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
"""The source superproject is multi-distribution; PEP 517 metadata is not.

Metadata checks intentionally need no Qt installation, CMake build or network.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]
COMPONENTS = ("pyside6", "shiboken6", "shiboken6-generator")


class Pep517SourceMetadataTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="qt_pep517_")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in (
            "setup.py", "pyproject.toml", "README.md", "README.pyside6.md",
            "README.shiboken6.md", "README.shiboken6-generator.md",
        ):
            shutil.copy2(SOURCE / name, self.root / name)
        shutil.copytree(
            SOURCE / "build_scripts", self.root / "build_scripts",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        for name in ("wheel_artifacts/pyproject.toml.base", "sources/pyside6/.cmake.conf"):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / name, dest)
        shutil.copytree(SOURCE / "pip", self.root / "pip")
        # Package roots exist only for setuptools source-file discovery.
        for name in ("sources/pyside6/PySide6", "sources/shiboken6/shibokenmodule"):
            (self.root / name).mkdir(parents=True)

    def _run(self, *args):
        return subprocess.run(
            [sys.executable, *args], cwd=self.root,
            env={**os.environ, "PIP_NO_INDEX": "1", "PIP_DISABLE_PIP_VERSION_CHECK": "1"},
            capture_output=True, text=True, timeout=45,
        )

    def test_native_standalone_plugins_resolve_bundled_qt(self):
        from types import SimpleNamespace
        from build_scripts import main
        from build_scripts.utils import RpathTargetType
        plugin = self.root / "libPySidePlugin.so"
        plugin.touch()
        build = SimpleNamespace(is_cross_compile=False, standalone=True,
                                _patchelf_path="/unused/patchelf")
        with patch.object(main.sys, "platform", "linux"), \
                patch.object(main, "linux_fix_rpaths_for_library") as fix:
            main.PysideBuild.update_rpath_for_linux(
                build, RpathTargetType.plugins, paths=[plugin])
        fix.assert_called_once_with("/unused/patchelf", plugin,
                                    "$ORIGIN/../../lib", override=True)

    def test_build_requirements_include_packaging(self):
        build = tomllib.loads((self.root / "pyproject.toml").read_text())["build-system"]
        self.assertTrue(any(x.startswith("packaging") for x in build["requires"]))
        self.assertEqual(build["build-backend"], "build_scripts.pep517_backend")

    def test_pip_metadata_has_one_distribution_for_each_component(self):
        for component in COMPONENTS:
            with self.subTest(component=component):
                label = component.replace("-", "_")
                report_path = self.root / f"{label}.json"
                proc = self._run(
                    "-m", "pip", "install", "--dry-run", "--ignore-installed",
                    "--no-deps", "--no-build-isolation", "--no-index",
                    "--no-cache-dir", "--config-settings", f"pyside-build-type={component}",
                    "--config-settings", "--global-option=--qtpaths=/not-installed/qtpaths",
                    "--report", str(report_path), ".",
                )
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                installed = json.loads(report_path.read_text())["install"]
                self.assertEqual(len(installed), 1)
                self.assertEqual(installed[0]["metadata"]["name"].lower().replace("_", "-"),
                                 component)

    def test_helper_projects_need_no_component_config_setting(self):
        for component in ("shiboken6", "shiboken6-generator", "shiboken6-generator-build"):
            with self.subTest(component=component):
                report = self.root / f"helper-{component}.json"
                proc = self._run(
                    "-m", "pip", "install", "--dry-run", "--ignore-installed",
                    "--no-deps", "--no-build-isolation", "--no-index",
                    "--report", str(report), str(self.root / "pip" / component),
                )
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                metadata = json.loads(report.read_text())["install"][0]["metadata"]
                self.assertEqual(metadata["name"].lower().replace("_", "-"), component)

    def test_pyside_declares_recursive_shiboken_build_requirements(self):
        from build_scripts import pep517_backend as backend
        with patch.object(backend._setuptools, "get_requires_for_build_wheel", return_value=[]):
            requirements = backend.get_requires_for_build_wheel()
        self.assertEqual(len(requirements), 2)
        self.assertTrue(any("#subdirectory=pip/shiboken6" in r for r in requirements))
        self.assertTrue(any("#subdirectory=pip/shiboken6-generator-build" in r for r in requirements))

    def test_shiboken_build_finds_generator_sdk_in_isolated_environment(self):
        from types import SimpleNamespace
        from build_scripts import pep517_backend as backend
        before = os.environ.get("CMAKE_PREFIX_PATH")
        found = []
        def spec(name):
            return (SimpleNamespace(submodule_search_locations=["/isolated/generator"])
                    if name == "shiboken6_generator" else None)
        def build(*args):
            found.append(os.environ.get("CMAKE_PREFIX_PATH"))
            return "shiboken.whl"
        with patch.object(backend, "find_spec", side_effect=spec):
            with patch.object(backend._setuptools, "build_wheel", side_effect=build):
                backend.build_wheel("out", {"pyside-build-type": "shiboken6"})
        self.assertTrue(found[0].startswith("/isolated/generator"))
        self.assertEqual(os.environ.get("CMAKE_PREFIX_PATH"), before)

    def test_backend_selects_single_wheel_target_and_restores_environment(self):
        from build_scripts import pep517_backend as backend
        env_var = "_PYSIDE_PEP517_COMPONENT"
        before = os.environ.get(env_var)
        observed = []

        def record_call(*args):
            observed.append(os.environ.get(env_var))
            return "component.whl"

        with patch.object(backend._setuptools, "build_wheel", side_effect=record_call):
            result = backend.build_wheel("out", {"pyside-build-type": "shiboken6"})
        self.assertEqual(result, "component.whl")
        self.assertEqual(observed, ["shiboken6"])
        self.assertEqual(os.environ.get(env_var), before)
        with self.assertRaises(ValueError):
            backend.build_wheel("out", {"pyside-build-type": "all"})

    def test_legacy_setup_py_still_runs_all_three_egg_info_commands(self):
        proc = self._run("setup.py", "egg_info")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        actual = sorted(p.name for p in self.root.glob("*.egg-info"))
        self.assertEqual(actual, ["PySide6.egg-info", "shiboken6.egg-info",
                                  "shiboken6_generator.egg-info"])


if __name__ == "__main__":
    unittest.main()
