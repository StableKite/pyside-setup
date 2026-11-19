// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#ifndef SBKREFERENCE_H
#define SBKREFERENCE_H

#include "sbkpython.h"
#include "shibokenmacros.h"

struct SbkObject;

namespace Shiboken::Object {

// Retain an object whose native lifetime is independent of self. The reference
// remains visible to cyclic GC and is released by the ordinary wrapper cleanup.
// Invalidating self must not invalidate the objects retained under this key.
// A later keepReference() with the same key restores its default invalidation.
LIBSHIBOKEN_API void keepReferenceToIndependentObject(SbkObject *self, const char *key,
                                                     PyObject *object, bool append = false);

} // namespace Shiboken::Object

#endif // SBKREFERENCE_H
