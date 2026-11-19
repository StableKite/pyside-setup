# Copyright (C) 2023 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QtAsyncio'''

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

import asyncio
import time
from unittest.mock import patch
from PySide6.QtAsyncio import events
from PySide6.QtAsyncio import QAsyncioEventLoopPolicy


class QAsyncioTestCaseTime(unittest.TestCase):

    def setUp(self):
        self.previous_time = None
        self.exception = None

    def display_date(self, end_time, loop):
        if self.previous_time is not None:
            try:
                self.assertAlmostEqual(
                    time.monotonic() - self.previous_time, 1, delta=0.1)
            except AssertionError as e:
                self.exception = e
        self.previous_time = time.monotonic()
        if (loop.time() + 1.0) < end_time:
            loop.call_later(1, self.display_date, end_time, loop)
        else:
            loop.stop()

    def test_wall_clock_adjustment_does_not_reverse_loop_time(self):
        loop = QAsyncioEventLoopPolicy().new_event_loop()

        class AdjustedWallClock:
            values = iter((2_000_000, 1_000_000))

            @staticmethod
            def currentMSecsSinceEpoch():
                return next(AdjustedWallClock.values)

        try:
            with patch.object(events, "QDateTime", AdjustedWallClock, create=True):
                before = loop.time()
                after = loop.time()
            self.assertGreaterEqual(after, before)
        finally:
            loop.close()

    def test_time(self):
        asyncio.set_event_loop_policy(QAsyncioEventLoopPolicy())
        loop = asyncio.new_event_loop()

        end_time = loop.time() + 3.0
        loop.call_soon(self.display_date, end_time, loop)

        try:
            loop.run_forever()
        finally:
            loop.close()

        if self.exception is not None:
            raise self.exception


if __name__ == '__main__':
    unittest.main()
