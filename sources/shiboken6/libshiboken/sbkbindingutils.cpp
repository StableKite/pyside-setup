// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "sbkbindingutils.h"

#include "autodecref.h"
#include "sbkstring.h"
#include "sbkpep.h"
#include "sbkstaticstrings_p.h"

#include <algorithm>
#ifdef Py_GIL_DISABLED
#  include <atomic>
#endif

namespace Shiboken {

static const ArgumentNameIndexMapping *
    findMapping(const ArgumentNameIndexMapping *i1,
                const ArgumentNameIndexMapping *i2, PyObject *needle)
{
    return std::find_if(i1, i2, [needle](const ArgumentNameIndexMapping &m) {
        return PyUnicode_CompareWithASCIIString(needle, m.name) == 0;
    });
}

bool parseKeywordArguments(PyObject *kwds,
                           const ArgumentNameIndexMapping *mapping, size_t size,
                           Shiboken::AutoDecRef &errInfo, PyObject **pyArgs)
{
    if (kwds == nullptr || PyDict_Size(kwds) == 0)
        return true;
    PyObject *key{};
    PyObject *value{};
    Py_ssize_t pos = 0;
    const ArgumentNameIndexMapping *mappingEnd = mapping + size;
    while (PyDict_Next(kwds, &pos, &key, &value) != 0) {
        auto it = findMapping(mapping, mappingEnd, key);
        // Missing key: Create a new dict as error context (see signature/errorhandler.py)
        if (it == mappingEnd) {
            errInfo.reset(PyDict_New());
            PyDict_SetItem(errInfo.object(), key, value);
            return false;
        }
        if (pyArgs[it->index] != nullptr) { // duplicate entry, set string as error context
            errInfo.reset(key);
            return false;
        }
        pyArgs[it->index] = value;
    }
    return true;
}

bool parseConstructorKeywordArguments(PyObject *kwds,
                                      const ArgumentNameIndexMapping *mapping, size_t size,
                                      Shiboken::AutoDecRef &errInfo, PyObject **pyArgs)
{
    assert(kwds);
    Shiboken::AutoDecRef result(PyDict_New());
    PyObject *key{};
    PyObject *value{};
    Py_ssize_t pos = 0;
    const ArgumentNameIndexMapping *mappingEnd = mapping + size;
    while (PyDict_Next(kwds, &pos, &key, &value) != 0) {
        auto it = findMapping(mapping, mappingEnd, key);
        // Ignore missing key, assuming it is a property to be handled later
        if (it != mappingEnd) {
            // duplicate entry, set string as error context (see signature/errorhandler.py)
            if (pyArgs[it->index] != nullptr) {
                errInfo.reset(key);
                return false;
            }
            pyArgs[it->index] = value;
        } else {
            PyDict_SetItem(result.object(), key, value);
        }
    }
    errInfo.reset(result.release());
    return true;
}

static bool isCompiledHelper()
{
    Shiboken::AutoDecRef globals(PepEval_GetFrameGlobals());
    if (globals.isNull())
        return false;

#ifdef Py_GIL_DISABLED
    const int compiled = PyDict_Contains(globals.object(), PyMagicName::compiled());
    if (compiled < 0) {
        PyErr_Clear();
        return false;
    }
    if (compiled != 0)
        return true;
#else
    if (PyDict_GetItem(globals.object(), PyMagicName::compiled()) != nullptr)
        return true;
#endif
    globals.reset(nullptr);

    // __compiled__ may not be set in initialization phases, check builtins
#ifdef Py_GIL_DISABLED
    Shiboken::AutoDecRef nuitkaDir(
        Shiboken::String::createStaticString("__nuitka_binary_exe"));
    if (nuitkaDir.isNull())
        return false;
    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    if (builtins.isNull())
        return false;
    const int haveNuitkaDir = PyDict_Contains(builtins.object(), nuitkaDir.object());
    if (haveNuitkaDir < 0) {
        PyErr_Clear();
        return false;
    }
    return haveNuitkaDir != 0;
#else
    static PyObject *nuitkaDir = Shiboken::String::createStaticString("__nuitka_binary_exe");
    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    return !builtins.isNull() && PyDict_GetItem(builtins.object(), nuitkaDir) != nullptr;
#endif
}

bool isCompiled()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<int> result{-1};
    if (const int cached = result.load(std::memory_order_acquire); cached >= 0)
        return cached != 0;

    const int candidate = isCompiledHelper() ? 1 : 0;
    int expected = -1;
    if (!result.compare_exchange_strong(expected, candidate,
                                        std::memory_order_release,
                                        std::memory_order_acquire))
        return expected != 0;
    return candidate != 0;
#else
    static const bool result = isCompiledHelper();
    return result;
#endif
}

} // namespace Shiboken
