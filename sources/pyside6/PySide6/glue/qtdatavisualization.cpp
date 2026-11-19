// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

// @snippet releaseownership
Shiboken::Object::releaseOwnership(%PYARG_1);
// @snippet releaseownership

// @snippet qcustom3dvolume-settexturedata
using VectorType = decltype(%1);
%CPPSELF.setTextureData(new VectorType(%1));
// @snippet qcustom3dvolume-settexturedata

// @snippet dataproxy-addrow
using ListType = decltype(%1);
%RETURN_TYPE %0 = %CPPSELF.addRow(new ListType(%1));
%PYARG_0 = %CONVERTTOPYTHON[%RETURN_TYPE](%0);
// @snippet dataproxy-addrow

// @snippet dataproxy-addrow-string
using ListType = decltype(%1);
%RETURN_TYPE %0 = %CPPSELF.addRow(new ListType(%1), %2);
%PYARG_0 = %CONVERTTOPYTHON[%RETURN_TYPE](%0);
// @snippet dataproxy-addrow-string

// @snippet dataproxy-insertrow
using ListType = decltype(%2);
%CPPSELF.insertRow(%1, new ListType(%2));
// @snippet dataproxy-insertrow

// @snippet dataproxy-insertrow-string
using ListType = decltype(%2);
%CPPSELF.insertRow(%1, new ListType(%2), %3);
// @snippet dataproxy-insertrow-string

// @snippet dataproxy-setrow
using ListType = decltype(%2);
%CPPSELF.setRow(%1, new ListType(%2));
// @snippet dataproxy-setrow

// @snippet dataproxy-setrow-string
using ListType = decltype(%2);
%CPPSELF.setRow(%1, new ListType(%2), %3);
// @snippet dataproxy-setrow-string

// @snippet dataproxy-resetarray
using ListType = decltype(%1);
%CPPSELF.resetArray(new ListType(%1));
// @snippet dataproxy-resetarray

// @snippet dataproxy-resetarray2
using ListType = decltype(%1);
%CPPSELF.resetArray(new ListType(%1), %2, %3);
// @snippet dataproxy-resetarray2

// @snippet scatterdataproxy-resetarray
%CPPSELF.resetArray(new QScatterDataArray(*%1));
// @snippet scatterdataproxy-resetarray

// @snippet qsurfacedataproxy-resetarraynp
auto *data = QtDataVisualizationHelper::surfaceDataFromNp(%1, %2, %3, %4, %5);
// %CPPSELF.%FUNCTION_NAME
%CPPSELF.resetArray(data);
// @snippet qsurfacedataproxy-resetarraynp

// @snippet qvalue3daxisformatter-friend
class QFriendlyValue3DAxisFormatter : public QValue3DAxisFormatter
{
public:
using QValue3DAxisFormatter::gridPositions;
using QValue3DAxisFormatter::labelPositions;
using QValue3DAxisFormatter::labelStrings;
};

// @snippet qvalue3daxisformatter-friend

// @snippet qvalue3daxisformatter-setgridpositions
(%CPPSELF->*&QFriendlyValue3DAxisFormatter::gridPositions)() = %1;
// @snippet qvalue3daxisformatter-setgridpositions

// @snippet qvalue3daxisformatter-setlabelpositions
(%CPPSELF->*&QFriendlyValue3DAxisFormatter::labelPositions)() = %1;
// @snippet qvalue3daxisformatter-setlabelpositions

// @snippet qvalue3daxisformatter-setlabelstrings
(%CPPSELF->*&QFriendlyValue3DAxisFormatter::labelStrings)() = %1;
// @snippet qvalue3daxisformatter-setlabelstrings
