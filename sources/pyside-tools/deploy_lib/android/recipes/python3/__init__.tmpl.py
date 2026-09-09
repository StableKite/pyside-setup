# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

from os.path import join

from packaging.version import Version
from pythonforandroid.recipes.python3 import Python3Recipe as BasePython3Recipe


class Python3FreeThreadedRecipe(BasePython3Recipe):
    """Build the p4a target interpreter with CPython's free-threaded ABI."""

    disable_gil = True

    @property
    def link_version(self):
        version = super().link_version
        return version if version.endswith("t") else f"{version}t"

    def include_root(self, arch_name):
        version = Version(self.version)
        return join(
            self.get_build_dir(arch_name), "android-build", "android-root",
            "include", f"python{version.major}.{version.minor}t"
        )


recipe = Python3FreeThreadedRecipe()
