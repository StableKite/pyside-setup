// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef SBKPYTHON_H
#define SBKPYTHON_H

// PYSIDE-2701: This definition is needed for all Python formats with "#".
#define PY_SSIZE_T_CLEAN

// Qt's "slots" macro collides with the "slots" member variables
// used in some Python structs. For compilers that support push_macro,
// temporarily undefine it.
#if defined(slots) && (defined(__GNUC__) || defined(_MSC_VER) || defined(__clang__))
#  pragma push_macro("slots")
#  undef slots

extern "C" {
#  include <Python.h>
}

#  include <structmember.h>
#  pragma pop_macro("slots")

#else

extern "C" {
#  include <Python.h>
}

#  include <structmember.h>
#endif

// In Python 3, Py_TPFLAGS_DEFAULT contains Py_TPFLAGS_HAVE_VERSION_TAG,
// which will trigger the attribute cache, which is not intended in Qt for Python.
// Use a customized Py_TPFLAGS_DEFAULT by defining Py_TPFLAGS_HAVE_VERSION_TAG = 0.
#undef Py_TPFLAGS_HAVE_VERSION_TAG
#define Py_TPFLAGS_HAVE_VERSION_TAG  (0)

#include <type_traits>

namespace Shiboken {

// METH_NOARGS is called through PyCFunction, which takes two PyObject*
// parameters. Some wrappers also serve unary type slots and must retain their
// one-parameter signature there. Use a real adapter for the method table;
// reinterpret_cast alone leaves an incompatible indirect call.
template <auto Function>
inline PyObject *callNoArgsMethod(PyObject *self, PyObject *unused)
{
    if constexpr (std::is_invocable_r_v<PyObject *, decltype(Function),
                                       PyObject *, PyObject *>) {
        return Function(self, unused);
    } else {
        return Function(self);
    }
}

} // namespace Shiboken

using SbkObjectType [[deprecated]] = PyTypeObject; // FIXME PYSIDE 7 remove

#endif
