#!/usr/bin/python
# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0
from __future__ import annotations

'''Test cases for QtDataVisualization'''

import os
import sys
import unittest

from pathlib import Path
sys.path.append(os.fspath(Path(__file__).resolve().parents[1]))
from init_paths import init_test_paths
init_test_paths(False)

from helper.usesqapplication import UsesQApplication
from PySide6.QtCore import QTimer
from PySide6.QtDataVisualization import (Q3DBars, QBar3DSeries, QBarDataItem,
                                         QBarDataProxy, QCategory3DAxis,
                                         QValue3DAxis, QValue3DAxisFormatter,
                                         qDefaultSurfaceFormat)


def dataToBarDataRow(data):
    result = []
    for d in data:
        result.append(QBarDataItem(d))
    return result


def dataToBarDataArray(data):
    result = []
    for row in data:
        result.append(dataToBarDataRow(row))
    return result


class QtDataVisualizationTestCase(UsesQApplication):
    '''Tests related to QtDataVisualization'''

    def testBars(self):
        self.bars = Q3DBars()
        self.columnAxis = QCategory3DAxis()
        self.columnAxis.setTitle('Columns')
        self.columnAxis.setTitleVisible(True)
        self.columnAxis.setLabels(['Column1', 'Column2'])

        self.rowAxis = QCategory3DAxis()
        self.rowAxis.setTitle('Rows')
        self.rowAxis.setTitleVisible(True)
        self.rowAxis.setLabels(['Row1', 'Row2'])

        self.valueAxis = QValue3DAxis()
        self.valueAxis.setTitle('Values')
        self.valueAxis.setTitleVisible(True)
        self.valueAxis.setRange(0, 5)

        self.bars.setRowAxis(self.rowAxis)
        self.bars.setColumnAxis(self.columnAxis)
        self.bars.setValueAxis(self.valueAxis)

        self.series = QBar3DSeries()
        self.arrayData = [[1, 2], [3, 4]]
        self.series.dataProxy().addRows(dataToBarDataArray(self.arrayData))

        self.bars.setPrimarySeries(self.series)

        self.bars.show()
        QTimer.singleShot(500, self.app.quit)
        self.app.exec()

    def testBarDataProxy(self):
        '''PSYSIDE-1438, crashes in QBarDataProxy.addRow()'''
        items = [QBarDataItem(v) for v in [1.0, 2.0]]
        data_proxy = QBarDataProxy()
        data_proxy.addRow(items)
        data_proxy.addRow(items, 'bla')
        data_proxy.insertRow(0, items)
        data_proxy.insertRow(0, items, 'bla')
        data_proxy.setRow(0, items)
        data_proxy.setRow(0, items, 'bla')
        self.assertTrue(data_proxy.rowCount(), 4)

    def testDefaultSurfaceFormat(self):
        format = qDefaultSurfaceFormat(True)
        print(format)

    def testQValue3DAxisFormatter(self):
        """PYSIDE-2025: Test the added setters of QValue3DAxisFormatter."""
        formatter = QValue3DAxisFormatter()
        float_values = [float(10)]
        formatter.setGridPositions(float_values)
        self.assertEqual(formatter.gridPositions(), float_values)

        formatter.setLabelPositions(float_values)
        self.assertEqual(formatter.labelPositions(), float_values)

        label_strings = ["bla"]
        formatter.setLabelStrings(label_strings)
        self.assertEqual(formatter.labelStrings(), label_strings)


def checkCopiedDataItemOwnership(phase):
    import gc
    import json
    import os
    import sys
    import weakref

    import shiboken6
    from PySide6.QtGui import QVector3D
    from PySide6.QtDataVisualization import (QBarDataItem, QBarDataProxy,
        QScatterDataItem, QScatterDataProxy, QSurfaceDataItem, QSurfaceDataProxy)

    failures = []
    names = (['addRows', 'addRowsLabels', 'insertRows', 'insertRowsLabels',
              'setRows', 'setRowsLabels'] if phase == 'bar' else
             ['addItem', 'addItems', 'insertItem', 'insertItems', 'setItem', 'setItems']
             if phase == 'scatter' else ['resetArray'])
    for name in names:
        for iteration in range(2):
            if phase == 'bar':
                proxy, item = QBarDataProxy(), QBarDataItem(2.0)
                rows = [[item]]
                if name.startswith('set'):
                    proxy.addRow([QBarDataItem(-1.0)])
                method = name.removesuffix('Labels')
                args = [rows] if method == 'addRows' else [0, rows]
                if name.endswith('Labels'):
                    args.append(['row'])
                getattr(proxy, method)(*args)
                item.setValue(9.0)
                copied = proxy.itemAt(0, 0).value() == 2.0
            elif phase == 'scatter':
                proxy = QScatterDataProxy()
                item = QScatterDataItem(QVector3D(1, 2, 3))
                rows = [item]
                if name.startswith('set'):
                    proxy.addItem(QScatterDataItem(QVector3D()))
                value = rows if name.endswith('Items') else item
                args = [value] if name.startswith('add') else [0, value]
                getattr(proxy, name)(*args)
                item.setPosition(QVector3D(4, 5, 6))
                copied = proxy.itemAt(0).position() == QVector3D(1, 2, 3)
            else:
                assert phase == 'surface', phase
                proxy = QSurfaceDataProxy()
                item = QSurfaceDataItem(QVector3D(1, 2, 3))
                rows = [[item]]
                args = [rows]
                proxy.resetArray(rows)
                item.setPosition(QVector3D(4, 5, 6))
                copied = proxy.itemAt(0, 0).position() == QVector3D(1, 2, 3)
            owned = shiboken6.ownedByPython(item)
            shiboken6.delete(proxy)
            valid = shiboken6.isValid(item)
            state = dict(method=name, copied=copied, owned=owned, valid_after_proxy=valid)
            if not all((copied, owned, valid)):
                failures.append(state)
            reference = weakref.ref(item)
            del args, rows, item, proxy
            if phase == 'scatter':
                del value
            gc.collect()
            assert reference() is None, (name, 'Python item retained after cleanup')
        print('ACTUAL_DATAVISUALIZATION_VALUE_METHOD', json.dumps(state), flush=True)
    assert not failures, (phase, len(failures), failures[:2])
    print('ACTUAL_DATAVISUALIZATION_VALUE_OWNERSHIP_PASS', phase, len(names)*2, flush=True)


class CopiedDataItemOwnershipTest(unittest.TestCase):
    def testBarCopies(self):
        checkCopiedDataItemOwnership('bar')

    def testScatterCopies(self):
        checkCopiedDataItemOwnership('scatter')

    def testSurfaceCopies(self):
        checkCopiedDataItemOwnership('surface')


if __name__ == '__main__':
    unittest.main()
