// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysidewatch_p.h"

#include <autodecref.h>
#include <pep384ext.h>
#include <sbkpython.h>
#include <sbkpep.h>
#include <sbkstring.h>
#include <sbktypefactory.h>
#include <signature.h>

#ifdef Py_GIL_DISABLED
#include <atomic>
#endif

using namespace Shiboken;

struct PySideWatch
{
    PyObject_HEAD
    PyObject *propertyName; // str — the single property name to watch
};

static PyObject *watchPropertyNameSnapshot(PyObject *self)
{
    PyObject *result = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    result = Py_XNewRef(reinterpret_cast<PySideWatch *>(self)->propertyName);
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return result;
}

static void watchReplacePropertyName(PyObject *self, PyObject *propertyName)
{
    PyObject *old = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    auto *data = reinterpret_cast<PySideWatch *>(self);
    old = data->propertyName;
    data->propertyName = propertyName;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    Py_XDECREF(old);
}

#ifdef Py_GIL_DISABLED
static int appendWatchMetadataToFunction(PyObject *callback, PyObject *attrName,
                                         PyObject *propertyName)
{
    if (!PyFunction_Check(callback))
        return 1; // Use the generic attribute protocol below.

    AutoDecRef dict(PyObject_GetAttrString(callback, "__dict__"));
    if (dict.isNull() || !PyDict_Check(dict)) {
        PyErr_Clear();
        return 1;
    }

    AutoDecRef newList(PyList_New(0));
    if (newList.isNull())
        return -1;

    PyObject *metadata = nullptr;
    const int found = PyDict_SetDefaultRef(dict, attrName, newList, &metadata);
    if (found < 0)
        return -1;
    AutoDecRef metadataRef(metadata);

    if (!PyList_Check(metadata)) {
        AutoDecRef replacement(PyList_New(1));
        if (replacement.isNull())
            return -1;
        PyList_SetItem(replacement, 0, Py_NewRef(propertyName));
        return PyDict_SetItem(dict, attrName, replacement);
    }
    return PyList_Append(metadata, propertyName);
}
#endif

extern "C"
{

static int watchTpInit(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    const char *propName = nullptr;
    if (!PyArg_ParseTuple(args, "s:Watch", &propName))
        return -1;

    PyObject *propertyName = PyUnicode_FromString(propName);
    if (!propertyName)
        return -1;
    watchReplacePropertyName(self, propertyName);
    return 0;
}


// currently only passive implementation and only stores the watched property names
// once @auto_properties is implemented, this will be extended to also support active watching of
// property changes and calling the decorated function i.e. callback
static PyObject *watchCall(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    PyObject *callback = nullptr;
    if (!PyArg_UnpackTuple(args, "Watch.__call__", 1, 1, &callback))
        return nullptr;

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError,
                        "@watch can only decorate callable objects");
        return nullptr;
    }

    // Validate that the callback accepts at least 2 positional params (self, change)
    // Use __code__.co_argcount which works for regular Python functions.
    {
        AutoDecRef codeAttrName(PyUnicode_FromString("__code__"));
        PyObject *code = PyObject_GetAttr(callback, codeAttrName);
        if (code) {
            AutoDecRef codeRef(code);
            AutoDecRef argcount(PyObject_GetAttrString(code, "co_argcount"));
            if (!argcount.isNull() && PyLong_Check(argcount)) {
                const long nargs = PyLong_AsLong(argcount);
                if (nargs < 2) {
                    PyErr_Format(PyExc_TypeError,
                                 "@watch-decorated method must accept at least "
                                 "one parameter in addition to 'self': "
                                 "the 'change: Change' argument");
                    return nullptr;
                }
            }
        }
        PyErr_Clear(); // clear any error (e.g. no __code__ for bound methods)
    }

    AutoDecRef propertyName(watchPropertyNameSnapshot(self));
    if (propertyName.isNull()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "watch instance is not initialized");
        return nullptr;
    }

    PyObject *attrName = PySide::Watch::watchAttrName();
#ifdef Py_GIL_DISABLED
    const int functionResult = appendWatchMetadataToFunction(callback, attrName, propertyName);
    if (functionResult < 0)
        return nullptr;
    if (functionResult == 0)
        return Py_NewRef(callback);
#endif

    // Preserve the generic attribute protocol for arbitrary callable objects.
    PyObject *existing = PyObject_GetAttr(callback, attrName);
    if (existing && PyList_Check(existing)) {
        const int result = PyList_Append(existing, propertyName);
        Py_DECREF(existing);
        if (result < 0)
            return nullptr;
    } else {
        Py_XDECREF(existing);
        PyErr_Clear();
        AutoDecRef newList(PyList_New(1));
        if (newList.isNull())
            return nullptr;
        PyList_SetItem(newList, 0, Py_NewRef(propertyName));
        if (PyObject_SetAttr(callback, attrName, newList) < 0)
            return nullptr;
    }

    return Py_NewRef(callback);
}

static void watchTpDealloc(PyObject *self)
{
    auto *data = reinterpret_cast<PySideWatch *>(self);
    Py_XDECREF(data->propertyName);
    Py_DECREF(Py_TYPE(self));
    PepExt_TypeCallFree(self);
}

static PyTypeObject *createWatchType()
{
    PyType_Slot PySideWatchType_slots[] = {
        {Py_tp_call, reinterpret_cast<void *>(watchCall)},
        {Py_tp_init, reinterpret_cast<void *>(watchTpInit)},
        {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
        {Py_tp_dealloc, reinterpret_cast<void *>(watchTpDealloc)},
        {0, nullptr}
    };

    PyType_Spec PySideWatchType_spec = {
        "2:PySide6.QtQmlFeatures.watch",
        sizeof(PySideWatch),
        0,
        Py_TPFLAGS_DEFAULT,
        PySideWatchType_slots,
    };

    return SbkType_FromSpec(&PySideWatchType_spec);
}

static PyTypeObject *PySideWatch_TypeF()
{
    static auto *type = createWatchType();
    return type;
}

} // extern "C"

namespace PySide::Watch {

PyObject *watchAttrName()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyObject *> s{nullptr};
    if (auto *result = s.load(std::memory_order_acquire))
        return result;
    auto *candidate = Shiboken::String::createStaticString("_pyside_watch");
    PyObject *expected = nullptr;
    if (!s.compare_exchange_strong(expected, candidate,
                                   std::memory_order_release,
                                   std::memory_order_acquire)) {
        Py_XDECREF(candidate);
        return expected;
    }
    return candidate;
#else
    static PyObject *const s = Shiboken::String::createStaticString("_pyside_watch");
    return s;
#endif
}

static const char *Watch_SignatureStrings[] = {
    "PySide6.QtQmlFeatures.watch(self,property_name:str)",
    "PySide6.QtQmlFeatures.watch.__call__(self,function:typing.Callable)->typing.Callable",
    nullptr}; // Sentinel

void init(PyObject *module)
{
    auto *watchType = PySideWatch_TypeF();
    if (InitSignatureStrings(watchType, Watch_SignatureStrings) < 0)
        return;
    auto *obWatchType = reinterpret_cast<PyObject *>(watchType);
    Py_INCREF(obWatchType);
    PepModule_AddType(module, watchType);
}

} // namespace PySide::Watch
