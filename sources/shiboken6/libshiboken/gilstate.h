// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef GILSTATE_H
#define GILSTATE_H

#include <shibokenmacros.h>
#include "sbkpython.h"

namespace Shiboken
{

// Attached cleanup may continue after CPython clears its initialized flag.
// Once both the interpreter and its thread state are gone, native objects
// can still be destroyed, but their Python references must not be touched.
inline bool isPythonRuntimeAvailable()
{
    if (Py_IsInitialized())
        return true;
#ifdef Py_GIL_DISABLED
    return PyThreadState_GetUnchecked() != nullptr
        || PyGILState_GetThisThreadState() != nullptr;
#else
    return false;
#endif
}

class LIBSHIBOKEN_API GilState
{
public:
    GilState(const GilState &) = delete;
    GilState(GilState &&) = delete;
    GilState &operator=(const GilState &) = delete;
    GilState &operator=(GilState &&) = delete;

    explicit GilState(bool acquire=true);
    ~GilState();
    void acquire();
    void release();
    void abandon();
private:
    PyGILState_STATE m_gstate;
    bool m_locked = false;
};

} // namespace Shiboken

#endif // GILSTATE_H
