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
        # Package roots exist only for setuptools source-file discovery.
        for name in ("sources/pyside6/PySide6", "sources/shiboken6/shibokenmodule"):
            (self.root / name).mkdir(parents=True)

    def _run(self, *args):
        return subprocess.run(
            [sys.executable, *args], cwd=self.root,
            env={**os.environ, "PIP_NO_INDEX": "1", "PIP_DISABLE_PIP_VERSION_CHECK": "1"},
            capture_output=True, text=True, timeout=45,
        )

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
