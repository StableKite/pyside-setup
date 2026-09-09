# Copyright (C) 2026 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
TOOL_DIR = TEST_DIR.parent
sys.path.insert(0, str(TOOL_DIR))

from main import _PLATFORM_DATA  # noqa: E402


class AndroidCompilerFlagsTest(unittest.TestCase):
    def test_platform_compiler_flags_match_abi(self):
        aarch64_flags = _PLATFORM_DATA["aarch64"][2]
        x86_64_flags = _PLATFORM_DATA["x86_64"][2]
        self.assertIn("-march=armv8-a", aarch64_flags)
        self.assertNotIn("-msse", aarch64_flags)
        self.assertNotIn("-m64", aarch64_flags)
        self.assertIn("-march=x86-64", x86_64_flags)
        self.assertIn("-msse4.2", x86_64_flags)


if __name__ == "__main__":
    unittest.main()
