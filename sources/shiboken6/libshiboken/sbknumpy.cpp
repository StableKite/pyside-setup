// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default


#ifdef HAVE_NUMPY
// Include numpy before sbknumpycheck.h to get the proper PyArray_Check.
#  include <numpy/numpyconfig.h>
#  if defined(NPY_2_1_API_VERSION) && NPY_API_VERSION >= NPY_2_1_API_VERSION
// NumPy 2.1 introduced both free-threading support and this API declaration
// attribute. Each thread imports and validates its own pointer to NumPy's
// immutable API table. Python's import machinery handles module publication;
// no C++ lock may be held across that potentially reentrant Python call.
#    define PY_ARRAY_UNIQUE_SYMBOL ShibokenNumpyAPI
#    define NPY_API_SYMBOL_ATTRIBUTE thread_local NPY_VISIBILITY_HIDDEN
#  endif
#  include <numpy/arrayobject.h>
#  if defined(Py_GIL_DISABLED) && !defined(PY_ARRAY_UNIQUE_SYMBOL)
#    error "Free-threaded Shiboken requires NumPy 2.1 or newer headers"
#  endif
#endif

#include "autodecref.h"
#include "helper.h"
#include "sbknumpycheck.h"
#include "sbkcpptonumpy.h"
#include "sbknumpyview.h"

#include <algorithm>

namespace Shiboken::Numpy
{

#ifdef HAVE_NUMPY
static bool initNumPy()
{
    // PYSIDE-2404: Keep NumPy lazy. Import can detach this thread or call back
    // into Shiboken, even with the GIL enabled. Publish readiness only after
    // the complete API import and ABI validation succeed, and allow retries.
    static thread_local bool initialized = false;
    static thread_local bool initializing = false;
    if (initialized)
        return true;
    if (initializing)
        return false;
    initializing = true;
    initialized = _import_array() >= 0;
    initializing = false;
    return initialized;
}
#endif // HAVE_NUMPY

#ifdef HAVE_NUMPY
static bool initNumPyForCheck()
{
    if (initNumPy())
        return true;
    // An ordinary non-NumPy conversion must still work when the optional
    // root package is absent. Preserve allocation/ABI errors and missing
    // dependencies inside an installed NumPy. Explicit array creators keep
    // their original initNumPy() failure and raise the missing-package error.
    if (!PyErr_ExceptionMatches(PyExc_ModuleNotFoundError))
        return false;
    PyObject *errorType = nullptr;
    PyObject *errorValue = nullptr;
    PyObject *errorTraceback = nullptr;
    PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
    PyErr_NormalizeException(&errorType, &errorValue, &errorTraceback);
    AutoDecRef name;
    if (errorType != nullptr
        && PyErr_GivenExceptionMatches(errorType, PyExc_ModuleNotFoundError)) {
        name.reset(PyObject_GetAttrString(errorValue, "name"));
    }
    const bool optionalRootMissing = !name.isNull() && PyUnicode_Check(name.object())
        && PyUnicode_CompareWithASCIIString(name.object(), "numpy") == 0;
    if (!optionalRootMissing && PyErr_Occurred() == nullptr) {
        PyErr_Restore(errorType, errorValue, errorTraceback);
    } else {
        Py_XDECREF(errorType);
        Py_XDECREF(errorValue);
        Py_XDECREF(errorTraceback);
    }
    return false;
}
#endif

bool check(PyObject *pyIn)
{
#ifdef HAVE_NUMPY
    return pyIn != nullptr && initNumPyForCheck() && PyArray_Check(pyIn);
#else
    SBK_UNUSED(pyIn);
    return false;
#endif
}

} //namespace Shiboken::Numpy

// Include all sources files using numpy so that they are in the same
// translation unit (see comment at initNumPyArrayConverters()).

#include "sbknumpyview.cpp"
#include "sbkcpptonumpy.cpp"
#ifdef HAVE_NUMPY
#  include "sbknumpyarrayconverter.cpp"
#endif
