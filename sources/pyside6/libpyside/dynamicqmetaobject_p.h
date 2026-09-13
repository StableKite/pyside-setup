// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef DYNAMICQMETAOBJECT_P_H
#define DYNAMICQMETAOBJECT_P_H

#include <sbkpython.h>

#ifdef Py_GIL_DISABLED
#  include <pysidemacros.h>
#endif

namespace PySide
{

#ifdef Py_GIL_DISABLED

// QMetaObjectBuilder is shared by all instances of a Python QObject type and
// is not thread-safe. Keep all external accesses in one lock domain. The lock
// is recursive because building a meta object can re-enter the binding on the
// same thread. A contended waiter detaches its Python thread state before it
// blocks, matching the free-threading rules used by the signal manager.
class PYSIDE_API MetaObjectBuilderLock
{
public:
    MetaObjectBuilderLock();
    ~MetaObjectBuilderLock();

    MetaObjectBuilderLock(const MetaObjectBuilderLock &) = delete;
    MetaObjectBuilderLock &operator=(const MetaObjectBuilderLock &) = delete;
};

#endif // Py_GIL_DISABLED

} // namespace PySide

#endif // DYNAMICQMETAOBJECT_P_H
