// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysideeffect_p.h"

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

struct PySideEffect
{
    PyObject_HEAD
    PyObject *propertyNames; // list[str]. property names to observe
};

static PyObject *effectPropertyNamesSnapshot(PyObject *self)
{
    PyObject *result = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    result = Py_XNewRef(reinterpret_cast<PySideEffect *>(self)->propertyNames);
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return result;
}

static void effectReplacePropertyNames(PyObject *self, PyObject *propertyNames)
{
    PyObject *old = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    auto *data = reinterpret_cast<PySideEffect *>(self);
    old = data->propertyNames;
    data->propertyNames = propertyNames;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    Py_XDECREF(old);
}

static int appendPropertyNames(PyObject *target, PyObject *propertyNames)
{
    const Py_ssize_t n = PyList_Size(propertyNames);
    for (Py_ssize_t i = 0; i < n; ++i) {
#ifdef Py_GIL_DISABLED
        AutoDecRef item(PyList_GetItemRef(propertyNames, i));
        if (item.isNull() || PyList_Append(target, item) < 0)
            return -1;
#else
        PyObject *item = PyList_GetItem(propertyNames, i);
        if (PyList_Append(target, item) < 0)
            return -1;
#endif
    }
    return 0;
}

#ifdef Py_GIL_DISABLED
static int appendEffectMetadataToFunction(PyObject *callback, PyObject *attrName,
                                          PyObject *propertyNames)
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
        AutoDecRef replacement(PyList_GetSlice(propertyNames, 0, PyList_Size(propertyNames)));
        if (replacement.isNull())
            return -1;
        return PyDict_SetItem(dict, attrName, replacement);
    }
    return appendPropertyNames(metadata, propertyNames);
}
#endif

extern "C"
{

static int effectTpInit(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    const Py_ssize_t nArgs = PyTuple_Size(args);
    if (nArgs == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "@effect requires at least one property name");
        return -1;
    }

    PyObject *propertyNames = PyList_New(nArgs);
    if (!propertyNames)
        return -1;

    for (Py_ssize_t i = 0; i < nArgs; ++i) {
        PyObject *arg = PyTuple_GetItem(args, i);
        if (!PyUnicode_Check(arg)) {
            PyErr_Format(PyExc_TypeError,
                         "@effect argument %zd must be a string, not %s",
                         i + 1, Py_TYPE(arg)->tp_name);
            Py_DECREF(propertyNames);
            return -1;
        }
        PyList_SetItem(propertyNames, i, Py_NewRef(arg));
    }

    effectReplacePropertyNames(self, propertyNames);
    return 0;
}

static PyObject *effectCall(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    PyObject *callback = nullptr;
    if (!PyArg_UnpackTuple(args, "effect.__call__", 1, 1, &callback))
        return nullptr;

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError,
                        "@effect can only decorate callable objects");
        return nullptr;
    }

    AutoDecRef propertyNames(effectPropertyNamesSnapshot(self));
    if (propertyNames.isNull()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "effect instance is not initialized");
        return nullptr;
    }

    PyObject *attrName = PySide::Effect::effectAttrName();
#ifdef Py_GIL_DISABLED
    const int functionResult = appendEffectMetadataToFunction(callback, attrName, propertyNames);
    if (functionResult < 0)
        return nullptr;
    if (functionResult == 0)
        return Py_NewRef(callback);
#endif

    // Preserve the generic attribute protocol for arbitrary callable objects.
    PyObject *existing = PyObject_GetAttr(callback, attrName);
    if (existing && PyList_Check(existing)) {
        const int result = appendPropertyNames(existing, propertyNames);
        Py_DECREF(existing);
        if (result < 0)
            return nullptr;
    } else {
        Py_XDECREF(existing);
        PyErr_Clear();
        AutoDecRef copy(PyList_GetSlice(propertyNames, 0, PyList_Size(propertyNames)));
        if (copy.isNull())
            return nullptr;
        if (PyObject_SetAttr(callback, attrName, copy) < 0)
            return nullptr;
    }

    return Py_NewRef(callback);
}

static void effectTpDealloc(PyObject *self)
{
    auto *data = reinterpret_cast<PySideEffect *>(self);
    Py_XDECREF(data->propertyNames);
    Py_DECREF(Py_TYPE(self));
    PepExt_TypeCallFree(self);
}

static PyTypeObject *createEffectType()
{
    PyType_Slot PySideEffectType_slots[] = {
        {Py_tp_call, reinterpret_cast<void *>(effectCall)},
        {Py_tp_init, reinterpret_cast<void *>(effectTpInit)},
        {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
        {Py_tp_dealloc, reinterpret_cast<void *>(effectTpDealloc)},
        {0, nullptr}
    };

    PyType_Spec PySideEffectType_spec = {
        "2:PySide6.QtQmlFeatures.effect",
        sizeof(PySideEffect),
        0,
        Py_TPFLAGS_DEFAULT,
        PySideEffectType_slots,
    };

    return SbkType_FromSpec(&PySideEffectType_spec);
}

static PyTypeObject *PySideEffect_TypeF()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createEffectType);
#else
    static auto *type = createEffectType();
    return type;
#endif
}

} // extern "C"

namespace PySide::Effect {

PyObject *effectAttrName()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyObject *> s{nullptr};
    if (auto *result = s.load(std::memory_order_acquire))
        return result;
    auto *candidate = Shiboken::String::createStaticString("_pyside_effect");
    PyObject *expected = nullptr;
    if (!s.compare_exchange_strong(expected, candidate,
                                   std::memory_order_release,
                                   std::memory_order_acquire)) {
        Py_XDECREF(candidate);
        return expected;
    }
    return candidate;
#else
    static PyObject *const s = Shiboken::String::createStaticString("_pyside_effect");
    return s;
#endif
}

static const char *Effect_SignatureStrings[] = {
    "PySide6.QtQmlFeatures.effect(self,*property_names:str)",
    "PySide6.QtQmlFeatures.effect.__call__(self,function:typing.Callable)->typing.Callable",
    nullptr}; // Sentinel

void init(PyObject *module)
{
    auto *effectType = PySideEffect_TypeF();
    if (InitSignatureStrings(effectType, Effect_SignatureStrings) < 0)
        return;
    auto *obEffectType = reinterpret_cast<PyObject *>(effectType);
    Py_INCREF(obEffectType);
    PepModule_AddType(module, effectType);
}

} // namespace PySide::Effect
