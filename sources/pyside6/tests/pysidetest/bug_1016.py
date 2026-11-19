# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

import gc
import os
import weakref
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(True)

from testbinding import getHiddenObject
import shiboken6


class TestBug1016 (unittest.TestCase):

    def testIt(self):
        obj = getHiddenObject()
        self.assertIsNone(obj.parent())
        self.assertTrue(shiboken6.ownedByPython(obj))
        self.assertEqual(obj.callMe(), None)
        self.assertTrue(obj.wasCalled())
        reference = weakref.ref(obj)
        del obj
        gc.collect()
        self.assertIsNone(reference())


if __name__ == "__main__":
    unittest.main()
