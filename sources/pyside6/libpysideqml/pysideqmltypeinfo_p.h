// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef PYSIDEQMLTYPEINFO_P_H
#define PYSIDEQMLTYPEINFO_P_H

#include <sbkpython.h>

#include <QtCore/qbytearray.h>
#include <QtCore/qflags.h>

#include <memory>
#include <mutex>

QT_FORWARD_DECLARE_CLASS(QDebug)
QT_FORWARD_DECLARE_CLASS(QObject)
QT_FORWARD_DECLARE_STRUCT(QMetaObject)

namespace PySide::Qml {

enum class QmlTypeFlag
{
    Singleton = 0x1
};

Q_DECLARE_FLAGS(QmlTypeFlags, QmlTypeFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(QmlTypeFlags)

// Type information associated with QML type objects. The same object can be
// reached through aliases, so its mutable fields need their own lock on a
// free-threaded build. Keep the lock scope to plain state snapshots/updates;
// callers must not invoke Python or Qt while holding it.
struct QmlTypeInfoData
{
    QmlTypeFlags flags{};
    PyTypeObject *foreignType = nullptr;
    PyTypeObject *attachedType = nullptr;
    PyTypeObject *extensionType = nullptr;
};

struct QmlTypeInfo
{
    QmlTypeInfoData data() const;
    void setFlag(QmlTypeFlag flag);
    void setAttachedType(PyTypeObject *type);
    void setExtensionType(PyTypeObject *type);

private:
    friend void setQmlForeignType(const PyObject *, PyTypeObject *);
    void setForeignType(PyTypeObject *type);

#ifdef Py_GIL_DISABLED
    mutable std::mutex m_mutex;
#endif
    QmlTypeInfoData m_data;
};

using QmlTypeInfoPtr = std::shared_ptr<QmlTypeInfo>;

QmlTypeInfoPtr ensureQmlTypeInfo(const PyObject *o);
void setQmlForeignType(const PyObject *o, PyTypeObject *foreignType);
QmlTypeInfoPtr qmlTypeInfo(const PyObject *o);

// Meta Object and factory function for QmlExtended/QmlAttached
struct QmlExtensionInfo
{
    using Factory = QObject *(*)(QObject *);

    Factory factory;
    const QMetaObject *metaObject;
};

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug d, const QmlTypeInfo &);
QDebug operator<<(QDebug d, const QmlExtensionInfo &);
#endif

} // namespace PySide::Qml

#endif // PYSIDEQMLTYPEINFO_P_H
