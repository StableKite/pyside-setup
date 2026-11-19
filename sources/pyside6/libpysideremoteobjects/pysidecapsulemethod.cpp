// Copyright (C) 2025 Ford Motor Company
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysidecapsulemethod_p.h"

#include <autodecref.h>
#include <sbktypefactory.h>
#include <cstring>

extern "C"
{

struct CapsuleDescriptor
{
    PyObject_HEAD
    PyObject *capsule;
    PyMethodDef methodDef;

    bool configure(PyObject *value, const PyMethodDef *method)
    {
        char *name = copyString(method->ml_name);
        if (method->ml_name != nullptr && name == nullptr)
            return false;
        char *doc = copyString(method->ml_doc);
        if (method->ml_doc != nullptr && doc == nullptr) {
            PyMem_RawFree(name);
            return false;
        }
        capsule = Py_NewRef(value);
        methodDef = {name, method->ml_meth, method->ml_flags, doc};
        return true;
    }

    static char *copyString(const char *value)
    {
        if (value == nullptr)
            return nullptr;
        const auto size = std::strlen(value) + 1;
        auto *result = static_cast<char *>(PyMem_RawMalloc(size));
        if (result == nullptr)
            PyErr_NoMemory();
        else
            std::memcpy(result, value, size);
        return result;
    }
};

static int CapsuleDescriptor_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(reinterpret_cast<CapsuleDescriptor *>(self)->capsule);
    return 0;
}

static int CapsuleDescriptor_clear(PyObject *self)
{
    Py_CLEAR(reinterpret_cast<CapsuleDescriptor *>(self)->capsule);
    return 0;
}

static void CapsuleDescriptor_dealloc(PyObject *self)
{
    auto *descriptor = reinterpret_cast<CapsuleDescriptor *>(self);
    auto *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    CapsuleDescriptor_clear(self);
    PyMem_RawFree(const_cast<char *>(descriptor->methodDef.ml_name));
    PyMem_RawFree(const_cast<char *>(descriptor->methodDef.ml_doc));
    PyObject_GC_Del(self);
    Py_DECREF(type);
}

static PyObject *CapsuleDescriptor_tp_new(PyTypeObject *type, PyObject *, PyObject *)
{
    auto *self = reinterpret_cast<CapsuleDescriptor *>(PyType_GenericAlloc(type, 0));
    if (self != nullptr) {
        self->capsule = nullptr;
        self->methodDef = {nullptr, nullptr, METH_NOARGS, nullptr};
    }
    return reinterpret_cast<PyObject *>(self);
}

static PyObject *CapsuleMethod_descr_get(PyObject *self, PyObject *instance, PyObject *)
{
    if (instance == nullptr || instance == Py_None)
        return Py_NewRef(self);
    auto *descriptor = reinterpret_cast<CapsuleDescriptor *>(self);
    // The callable owns its instance, runtime data and the storage containing
    // PyMethodDef. Tuple traversal also exposes a saved-bound-method cycle.
    Shiboken::AutoDecRef context(PyTuple_Pack(3, instance, descriptor->capsule, self));
    if (context.isNull())
        return nullptr;
    return PyCFunction_New(&descriptor->methodDef, context.object());
}

static PyObject *CapsuleProperty_descr_get(PyObject *self, PyObject *instance, PyObject *owner)
{
    if (instance == nullptr || instance == Py_None)
        return Py_NewRef(self);
    Shiboken::AutoDecRef callable(CapsuleMethod_descr_get(self, instance, owner));
    return callable.isNull() ? nullptr : PyObject_CallNoArgs(callable.object());
}

static int CapsuleProperty_descr_set(PyObject *self, PyObject *instance, PyObject *value)
{
    if (value == nullptr) {
        PyErr_SetString(PyExc_AttributeError, "This native property cannot be deleted");
        return -1;
    }
    Shiboken::AutoDecRef callable(CapsuleMethod_descr_get(self, instance, nullptr));
    if (callable.isNull())
        return -1;
    Shiboken::AutoDecRef result(PyObject_CallOneArg(callable.object(), value));
    return result.isNull() ? -1 : 0;
}

static PyTypeObject *createCapsuleDescriptorType(const char *name, bool property, bool writable)
{
    PyType_Slot typeSlots[] = {
        {Py_tp_new, reinterpret_cast<void *>(CapsuleDescriptor_tp_new)},
        {Py_tp_descr_get, property ? reinterpret_cast<void *>(CapsuleProperty_descr_get)
                                  : reinterpret_cast<void *>(CapsuleMethod_descr_get)},
        {Py_tp_free, reinterpret_cast<void *>(PyObject_GC_Del)},
        {Py_tp_dealloc, reinterpret_cast<void *>(CapsuleDescriptor_dealloc)},
        {Py_tp_traverse, reinterpret_cast<void *>(CapsuleDescriptor_traverse)},
        {Py_tp_clear, reinterpret_cast<void *>(CapsuleDescriptor_clear)},
        {0, nullptr}, // Optional setter replaces this sentinel.
        {0, nullptr}
    };
    if (writable)
        typeSlots[6] = {Py_tp_descr_set, reinterpret_cast<void *>(CapsuleProperty_descr_set)};
    PyType_Spec spec = {name, sizeof(CapsuleDescriptor), 0,
                       Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, typeSlots};
    return reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&spec));
}

static PyTypeObject *createCapsuleMethodType()
{
    return createCapsuleDescriptorType("PySide6.QtRemoteObjects.CapsuleMethod", false, false);
}

PyTypeObject *CapsuleMethod_TypeF()
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createCapsuleMethodType);
#else
    static auto *type = createCapsuleMethodType();
    return type;
#endif
}

PyTypeObject *CapsuleProperty_TypeF(bool writable)
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> writableType{nullptr};
    static std::atomic<PyTypeObject *> readOnlyType{nullptr};
    auto &type = writable ? writableType : readOnlyType;
    return Shiboken::TypeInit::publish(type, [writable] {
        return createCapsuleDescriptorType("PySide6.QtRemoteObjects.CapsuleProperty", true, writable);
    });
#else
    if (writable) {
        static auto *type = createCapsuleDescriptorType("PySide6.QtRemoteObjects.CapsuleProperty", true, true);
        return type;
    }
    static auto *type = createCapsuleDescriptorType("PySide6.QtRemoteObjects.CapsuleProperty", true, false);
    return type;
#endif
}

static PyObject *makeCapsuleDescriptor(PyTypeObject *type, PyMethodDef *method, PyObject *capsule)
{
    if (type == nullptr)
        return nullptr;
    Shiboken::AutoDecRef result(PyObject_CallNoArgs(reinterpret_cast<PyObject *>(type)));
    if (result.isNull())
        return nullptr;
    auto *descriptor = reinterpret_cast<CapsuleDescriptor *>(result.object());
    return descriptor->configure(capsule, method) ? result.release() : nullptr;
}

bool add_capsule_method_to_type(PyTypeObject *type, PyMethodDef *method, PyObject *capsule)
{
    if (PyType_Ready(type) < 0)
        return false;
    Shiboken::AutoDecRef descriptor(makeCapsuleDescriptor(CapsuleMethod_TypeF(), method, capsule));
    return !descriptor.isNull()
        && PyObject_SetAttrString(reinterpret_cast<PyObject *>(type), method->ml_name,
                                 descriptor.object()) == 0;
}

PyObject *make_capsule_property(PyMethodDef *method, PyObject *capsule, bool writable)
{
    return makeCapsuleDescriptor(CapsuleProperty_TypeF(writable), method, capsule);
}

} // extern "C"
