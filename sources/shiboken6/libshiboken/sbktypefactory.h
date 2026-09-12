// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef SBKTYPEFACTORY_H
#define SBKTYPEFACTORY_H

#include "sbkpepbuffer.h"
#include "shibokenmacros.h"
#ifdef Py_GIL_DISABLED
#  include <atomic>

namespace Shiboken::TypeInit
{

template <typename Factory>
inline PyTypeObject *publish(std::atomic<PyTypeObject *> &cache, Factory factory)
{
    if (auto *result = cache.load(std::memory_order_acquire))
        return result;

    auto *candidate = factory();
    if (candidate == nullptr)
        return nullptr;

    PyTypeObject *expected = nullptr;
    if (!cache.compare_exchange_strong(expected, candidate,
                                       std::memory_order_release,
                                       std::memory_order_acquire)) {
        Py_DECREF(reinterpret_cast<PyObject *>(candidate));
        return expected;
    }
    return candidate;
}

} // namespace Shiboken::TypeInit
#endif // Py_GIL_DISABLED

extern "C"
{

// PYSIDE-535: Encapsulation of PyType_FromSpec special-cased for PyPy
LIBSHIBOKEN_API PyTypeObject *SbkType_FromSpec(PyType_Spec *);
LIBSHIBOKEN_API PyTypeObject *SbkType_FromSpecWithMeta(PyType_Spec *, PyTypeObject *);
LIBSHIBOKEN_API PyTypeObject *SbkType_FromSpecWithBases(PyType_Spec *, PyObject *);
LIBSHIBOKEN_API PyTypeObject *SbkType_FromSpecBasesMeta(PyType_Spec *, PyObject *, PyTypeObject *);

} //extern "C"

#endif // SBKTYPEFACTORY_H
