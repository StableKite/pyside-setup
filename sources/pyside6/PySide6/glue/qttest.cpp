// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

/*********************************************************************
 * INJECT CODE
 ********************************************************************/

// @snippet qsignalspy-signal
auto *signalInst = reinterpret_cast<PySideSignalInstance *>(%PYARG_1);
PyObject *emitterPyObject = PySide::Signal::getObject(signalInst);
Shiboken::AutoDecRef emitterOwner(emitterPyObject);
if (emitterPyObject == nullptr) {
    PyErr_SetString(PyExc_ValueError, "QSignalSpy requires a live signal source");
    return -1;
}
QObject* emitter = %CONVERTTOCPP[QObject *](emitterPyObject);
QByteArray signature = PySide::Signal::getSignature(signalInst);
if (!signature.isEmpty())
    signature.prepend('2'); // SIGNAL() macro

if (emitter == nullptr || signature.isEmpty()) {
    QByteArray error = QByteArrayLiteral("Wrong parameter (")
        + PepType_GetFullyQualifiedNameStr(Py_TYPE(%PYARG_1))
        + QByteArrayLiteral(") passed, QSignalSpy requires a signal.");
    PyErr_SetString(PyExc_ValueError, error.constData());
    return -1;
}

// emitterOwner releases the temporary wrapper on every return path.
%0 = new QSignalSpy(emitter, signature.constData());
// @snippet qsignalspy-signal
