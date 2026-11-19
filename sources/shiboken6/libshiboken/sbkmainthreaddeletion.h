// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#ifndef SBK_MAINTHREADDELETION_H
#define SBK_MAINTHREADDELETION_H
#include "sbkpython.h"
#include "shibokenmacros.h"
#ifdef Py_GIL_DISABLED
namespace Shiboken::Object {
// Called without binding-state locks. Implementations wake an existing main
// event dispatcher without allocating another deletion job or event.
using MainThreadDeletionWakeUp = void (*)() noexcept;
LIBSHIBOKEN_API void setMainThreadDeletionWakeUp(MainThreadDeletionWakeUp wakeUp) noexcept;
}
#endif
#endif
