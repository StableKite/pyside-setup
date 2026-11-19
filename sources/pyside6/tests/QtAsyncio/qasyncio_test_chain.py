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
import random
import time
from PySide6.QtAsyncio import QAsyncioEventLoopPolicy


class QAsyncioTestCaseChain(unittest.TestCase):

    async def link(self, output, random_source, n, i):
        t = random_source.randint(0, 5)
        output.append((n, "start", i, t))
        await asyncio.sleep(i)
        result = f"result {n}-{i}"
        output.append((n, "finish", i, result))
        return result

    async def chain(self, output, n):
        # Keep each chain's input independent of timer interleaving.
        random_source = random.Random(17 + n)
        link1 = await self.link(output, random_source, n, 0.2)
        link2 = await self.link(output, random_source, n, 0.5)
        output.append((n, "result", link1, link2))
        return link1, link2

    async def gather(self, output, *args):
        return await asyncio.gather(*(self.chain(output, n) for n in args))

    def test_chain(self):
        args = [1, 2, 3]

        outputs_expected = []
        outputs_real = []

        # Run the code without QAsyncioEventLoopPolicy
        asyncio.set_event_loop_policy(asyncio.DefaultEventLoopPolicy())
        start = time.perf_counter()
        results_expected = asyncio.run(self.gather(outputs_expected, *args))
        end_expected = time.perf_counter() - start

        # Run the code with QAsyncioEventLoopPolicy and QtEventLoop
        asyncio.set_event_loop_policy(QAsyncioEventLoopPolicy())
        start = time.perf_counter()
        results_real = asyncio.run(self.gather(outputs_real, *args))
        end_real = time.perf_counter() - start

        self.assertEqual(results_expected, results_real)
        self.assertEqual(len(outputs_expected), 5 * len(args))
        self.assertEqual(len(outputs_real), 5 * len(args))
        # asyncio does not guarantee ordering among equal-deadline timers.
        # Every chain must retain all its own events, in the correct order.
        for n in args:
            self.assertEqual([event for event in outputs_expected if event[0] == n],
                             [event for event in outputs_real if event[0] == n])
        self.assertAlmostEqual(end_expected, end_real, delta=0.5)


if __name__ == '__main__':
    unittest.main()
