// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysideqmltypeinfo_p.h"

#include <QtCore/qdebug.h>
#include <QtCore/qhash.h>

#include <sbkpep.h>

#include <algorithm>
#include <mutex>

namespace PySide::Qml {

using QmlTypeInfoHash = QHash<const PyObject *, QmlTypeInfoPtr>;

Q_GLOBAL_STATIC(QmlTypeInfoHash, qmlTypeInfoHashStatic);

#ifdef Py_GIL_DISABLED
static std::mutex qmlTypeInfoHashMutex;
#endif

QmlTypeInfoData QmlTypeInfo::data() const
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(m_mutex);
#endif
    return m_data;
}

void QmlTypeInfo::setFlag(QmlTypeFlag flag)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(m_mutex);
#endif
    m_data.flags.setFlag(flag);
}

void QmlTypeInfo::setForeignType(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(m_mutex);
#endif
    m_data.foreignType = type;
}

void QmlTypeInfo::setAttachedType(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(m_mutex);
#endif
    m_data.attachedType = type;
}

void QmlTypeInfo::setExtensionType(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(m_mutex);
#endif
    m_data.extensionType = type;
}

QmlTypeInfoPtr ensureQmlTypeInfo(const PyObject *o)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(qmlTypeInfoHashMutex);
#endif
    auto *hash = qmlTypeInfoHashStatic();
    auto it = hash->find(o);
    if (it == hash->end())
        it = hash->insert(o, std::make_shared<QmlTypeInfo>());
    return it.value();
}

void setQmlForeignType(const PyObject *o, PyTypeObject *foreignType)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(qmlTypeInfoHashMutex);
#endif
    auto *hash = qmlTypeInfoHashStatic();
    auto it = hash->find(o);
    if (it == hash->end())
        it = hash->insert(o, std::make_shared<QmlTypeInfo>());
    const auto &info = it.value();
    info->setForeignType(foreignType);
    hash->insert(reinterpret_cast<const PyObject *>(foreignType), info);
}

QmlTypeInfoPtr qmlTypeInfo(const PyObject *o)
{
#ifdef Py_GIL_DISABLED
    const std::lock_guard guard(qmlTypeInfoHashMutex);
#endif
    auto *hash = qmlTypeInfoHashStatic();
    auto it = hash->constFind(o);
    return it != hash->cend() ? it.value() : QmlTypeInfoPtr{};
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug d, const QmlTypeInfo &i)
{
    const auto data = i.data();
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "QmlTypeInfo(" << data.flags;
    if (data.foreignType)
        d << ", foreignType=" << PepType_GetFullyQualifiedNameStr(data.foreignType);
    if (data.attachedType)
        d << ", attachedType=" << PepType_GetFullyQualifiedNameStr(data.attachedType);
    if (data.extensionType)
        d << ", extensionType=" << PepType_GetFullyQualifiedNameStr(data.extensionType);
    d << ')';
    return d;
}

QDebug operator<<(QDebug d, const QmlExtensionInfo &e)
{
    QDebugStateSaver saver(d);
    d.noquote();
    d.nospace();
    d << "QmlExtensionInfo(";
    if (e.factory  != nullptr && e.metaObject != nullptr)
        d << '\"' << e.metaObject->className() << "\", factory="
          << reinterpret_cast<const void *>(e.factory);
    d << ')';
    return d;
}

#endif // QT_NO_DEBUG_STREAM

} // namespace PySide::Qml
