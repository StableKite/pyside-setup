// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysidecomputed_p.h"

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

struct PySideComputed
{
    PyObject_HEAD
    PyObject *depNames; // list[str]. dependency property names
};

static PyObject *computedDepNamesSnapshot(PyObject *self)
{
    PyObject *result = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    result = Py_XNewRef(reinterpret_cast<PySideComputed *>(self)->depNames);
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return result;
}

static void computedReplaceDepNames(PyObject *self, PyObject *depNames)
{
    PyObject *old = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    auto *data = reinterpret_cast<PySideComputed *>(self);
    old = data->depNames;
    data->depNames = depNames;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    Py_XDECREF(old);
}

extern "C"
{

static int computedTpInit(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    const Py_ssize_t nArgs = PyTuple_Size(args);
    if (nArgs == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "@computed requires at least one dependency name");
        return -1;
    }

    PyObject *depNames = PyList_New(nArgs);
    if (!depNames)
        return -1;

    for (Py_ssize_t i = 0; i < nArgs; ++i) {
        PyObject *arg = PyTuple_GetItem(args, i);
        if (!PyUnicode_Check(arg)) {
            PyErr_Format(PyExc_TypeError,
                         "@computed argument %zd must be a string, not %s",
                         i + 1, Py_TYPE(arg)->tp_name);
            Py_DECREF(depNames);
            return -1;
        }
        PyList_SetItem(depNames, i, Py_NewRef(arg));
    }

    computedReplaceDepNames(self, depNames);
    return 0;
}

static PyObject *computedCall(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    PyObject *callback = nullptr;
    if (!PyArg_UnpackTuple(args, "computed.__call__", 1, 1, &callback))
        return nullptr;

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError,
                        "@computed can only decorate callable objects");
        return nullptr;
    }

    AutoDecRef depNames(computedDepNamesSnapshot(self));
    if (depNames.isNull()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "computed instance is not initialized");
        return nullptr;
    }

    // Store a private dependency-list snapshot directly on the callback.
    AutoDecRef depsCopy(PyList_GetSlice(depNames, 0, PyList_Size(depNames)));
    if (depsCopy.isNull())
        return nullptr;
    if (PyObject_SetAttr(callback, PySide::Computed::computedAttrName(), depsCopy) < 0)
        return nullptr;

    return Py_NewRef(callback);
}

static void computedTpDealloc(PyObject *self)
{
    auto *data = reinterpret_cast<PySideComputed *>(self);
    Py_XDECREF(data->depNames);
    Py_DECREF(Py_TYPE(self));
    PepExt_TypeCallFree(self);
}

static PyTypeObject *createComputedType()
{
    PyType_Slot PySideComputedType_slots[] = {
        {Py_tp_call, reinterpret_cast<void *>(computedCall)},
        {Py_tp_init, reinterpret_cast<void *>(computedTpInit)},
        {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
        {Py_tp_dealloc, reinterpret_cast<void *>(computedTpDealloc)},
        {0, nullptr}
    };

    PyType_Spec PySideComputedType_spec = {
        "2:PySide6.QtQmlFeatures.computed",
        sizeof(PySideComputed),
        0,
        Py_TPFLAGS_DEFAULT,
        PySideComputedType_slots,
    };

    return SbkType_FromSpec(&PySideComputedType_spec);
}

static PyTypeObject *PySideComputed_TypeF()
{
    static auto *type = createComputedType();
    return type;
}

} // extern "C"

namespace PySide::Computed {

PyObject *computedAttrName()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyObject *> s{nullptr};
    if (auto *result = s.load(std::memory_order_acquire))
        return result;
    auto *candidate = Shiboken::String::createStaticString("_pyside_computed");
    PyObject *expected = nullptr;
    if (!s.compare_exchange_strong(expected, candidate,
                                   std::memory_order_release,
                                   std::memory_order_acquire)) {
        Py_XDECREF(candidate);
        return expected;
    }
    return candidate;
#else
    static PyObject *const s = Shiboken::String::createStaticString("_pyside_computed");
    return s;
#endif
}

static const char *Computed_SignatureStrings[] = {
    "PySide6.QtQmlFeatures.computed(self,*dep_names:str)",
    "PySide6.QtQmlFeatures.computed.__call__(self,function:typing.Callable)->typing.Callable",
    nullptr}; // Sentinel

void init(PyObject *module)
{
    auto *computedType = PySideComputed_TypeF();
    if (InitSignatureStrings(computedType, Computed_SignatureStrings) < 0)
        return;
    auto *obComputedType = reinterpret_cast<PyObject *>(computedType);
    Py_INCREF(obComputedType);
    PepModule_AddType(module, computedType);
}

} // namespace PySide::Computed
