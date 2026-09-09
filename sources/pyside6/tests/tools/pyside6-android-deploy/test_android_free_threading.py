# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

PYSIDE_ROOT = Path(__file__).parents[5].resolve()
PYSIDE_TOOLS = PYSIDE_ROOT / "sources" / "pyside-tools"
sys.path.insert(0, str(PYSIDE_TOOLS))

try:
    import pkginfo  # noqa: F401
except ImportError:
    pkginfo = types.ModuleType("pkginfo")
    pkginfo.Wheel = type("Wheel", (), {})
    sys.modules["pkginfo"] = pkginfo

from deploy_lib.android.android_config import AndroidConfig  # noqa: E402
from deploy_lib.android.android_helper import (  # noqa: E402
    create_free_threaded_python_recipe,
    is_free_threaded_wheel,
)


class AndroidFreeThreadingTest(unittest.TestCase):
    def test_wheel_abi_detection(self):
        regular = Path("PySide6-6.11.0-cp310-abi3-android_aarch64.whl")
        free_threaded = Path("PySide6-6.11.0-cp314-cp314t-android_aarch64.whl")
        self.assertFalse(is_free_threaded_wheel(regular))
        self.assertTrue(is_free_threaded_wheel(free_threaded))

    def test_free_threaded_recipe_is_required(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            recipe_dir = Path(temp_dir)
            (recipe_dir / "PySide6").mkdir()
            (recipe_dir / "shiboken6").mkdir()

            config = types.SimpleNamespace(
                _recipe_dir=recipe_dir,
                recipe_dir=recipe_dir,
                python_free_threaded=True,
            )
            self.assertFalse(AndroidConfig.recipes_exist(config))
            (recipe_dir / "python3").mkdir()
            self.assertTrue(AndroidConfig.recipes_exist(config))

    def test_regular_runtime_does_not_require_python_recipe(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            recipe_dir = Path(temp_dir)
            (recipe_dir / "PySide6").mkdir()
            (recipe_dir / "shiboken6").mkdir()
            config = types.SimpleNamespace(
                _recipe_dir=recipe_dir,
                recipe_dir=recipe_dir,
                python_free_threaded=False,
            )
            self.assertTrue(AndroidConfig.recipes_exist(config))

    def test_generated_python_recipe_uses_t_abi(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            create_free_threaded_python_recipe(root)
            recipe_file = root / "recipes" / "python3" / "__init__.py"

            pythonforandroid = types.ModuleType("pythonforandroid")
            recipes = types.ModuleType("pythonforandroid.recipes")
            python3 = types.ModuleType("pythonforandroid.recipes.python3")

            class BasePython3Recipe:
                version = "3.14.2"

                @property
                def link_version(self):
                    return "3.14"

                @property
                def _libpython(self):
                    return f"libpython{self.link_version}.so"

                def get_build_dir(self, arch_name):
                    return f"/build/{arch_name}"

            python3.Python3Recipe = BasePython3Recipe
            modules = {
                "pythonforandroid": pythonforandroid,
                "pythonforandroid.recipes": recipes,
                "pythonforandroid.recipes.python3": python3,
            }
            namespace = {"__file__": str(recipe_file), "__name__": "local_python3_recipe"}
            with patch.dict(sys.modules, modules):
                exec(compile(recipe_file.read_text(encoding="utf-8"), recipe_file, "exec"),
                     namespace)

            recipe = namespace["recipe"]
            self.assertTrue(recipe.disable_gil)
            self.assertEqual(recipe.link_version, "3.14t")
            self.assertEqual(recipe._libpython, "libpython3.14t.so")
            self.assertEqual(
                recipe.include_root("arm64-v8a"),
                "/build/arm64-v8a/android-build/android-root/include/python3.14t",
            )


if __name__ == "__main__":
    unittest.main()
