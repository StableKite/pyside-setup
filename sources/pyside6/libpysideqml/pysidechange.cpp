// Copyright (C) 2026 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysidechange.h"

#include <pep384ext.h>
#include <sbkpython.h>
#include <sbkpep.h>
#include <sbktypefactory.h>
#include <signature.h>

struct PySideChangeData
{
    PyObject *name;   // str
    PyObject *old;    // Any
    PyObject *new_;   // Any
    PyObject *owner;  // Any
};

struct PySideChange
{
    PyObject_HEAD
    PySideChangeData *d;
};

static void deleteChangeData(PySideChangeData *data)
{
    if (!data)
        return;
    Py_XDECREF(data->name);
    Py_XDECREF(data->old);
    Py_XDECREF(data->new_);
    Py_XDECREF(data->owner);
    delete data;
}

// Guards against access on an instance created via Change.__new__() without a
// subsequent __init__()
static PyObject *changeUninitializedError()
{
    PyErr_SetString(PyExc_RuntimeError,
                    "Change instance is not initialized");
    return nullptr;
}

static PyObject *changeGetField(PyObject *self, PyObject *PySideChangeData::*field)
{
    PyObject *result = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    if (auto *data = reinterpret_cast<PySideChange *>(self)->d)
        result = Py_NewRef(data->*field);
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return result ? result : changeUninitializedError();
}

extern "C"
{

static void changeTpDealloc(PyObject *self)
{
    deleteChangeData(reinterpret_cast<PySideChange *>(self)->d);
    Py_DECREF(Py_TYPE(self));
    PepExt_TypeCallFree(self);
}

static PyObject *changeGetName(PyObject *self, void * /* closure */)
{
    return changeGetField(self, &PySideChangeData::name);
}

static PyObject *changeGetOld(PyObject *self, void * /* closure */)
{
    return changeGetField(self, &PySideChangeData::old);
}

static PyObject *changeGetNew(PyObject *self, void * /* closure */)
{
    return changeGetField(self, &PySideChangeData::new_);
}

static PyObject *changeGetOwner(PyObject *self, void * /* closure */)
{
    return changeGetField(self, &PySideChangeData::owner);
}

static PyObject *changeTpRepr(PyObject *self)
{
    PyObject *name = nullptr;
    PyObject *old = nullptr;
    PyObject *new_ = nullptr;
    PyObject *owner = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    if (auto *data = reinterpret_cast<PySideChange *>(self)->d) {
        name = Py_NewRef(data->name);
        old = Py_NewRef(data->old);
        new_ = Py_NewRef(data->new_);
        owner = Py_NewRef(data->owner);
    }
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    if (!name)
        return PyUnicode_FromString("Change(<uninitialized>)");

    PyObject *result = PyUnicode_FromFormat("Change(name=%R, old=%R, new=%R, owner=%R)",
                                           name, old, new_, owner);
    Py_DECREF(name);
    Py_DECREF(old);
    Py_DECREF(new_);
    Py_DECREF(owner);
    return result;
}

static int changeTpInit(PyObject *self, PyObject *args, PyObject *kwds)
{
    static const char *kwlist[] = {"name", "old", "new", "owner", nullptr};
    PyObject *name = nullptr;
    PyObject *old = nullptr;
    PyObject *new_ = nullptr;
    PyObject *owner = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOOO:Change",
                                     const_cast<char **>(kwlist),
                                     &name, &old, &new_, &owner)) {
        return -1;
    }

    auto *newData = new PySideChangeData{Py_NewRef(name), Py_NewRef(old),
                                         Py_NewRef(new_), Py_NewRef(owner)};
    PySideChangeData *oldData = nullptr;
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    oldData = reinterpret_cast<PySideChange *>(self)->d;
    reinterpret_cast<PySideChange *>(self)->d = newData;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    deleteChangeData(oldData);
    return 0;
}

static PyGetSetDef PySideChangeGetSet[] = {
    {"name", changeGetName, nullptr, "Property name that changed", nullptr},
    {"old", changeGetOld, nullptr, "Previous value", nullptr},
    {"new", changeGetNew, nullptr, "New value", nullptr},
    {"owner", changeGetOwner, nullptr, "Object that owns the property", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

static PyTypeObject *createChangeType()
{
    PyType_Slot PySideChangeType_slots[] = {
        {Py_tp_init, reinterpret_cast<void *>(changeTpInit)},
        {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
        {Py_tp_dealloc, reinterpret_cast<void *>(changeTpDealloc)},
        {Py_tp_repr, reinterpret_cast<void *>(changeTpRepr)},
        {Py_tp_getset, reinterpret_cast<void *>(PySideChangeGetSet)},
        {0, nullptr}
    };

    PyType_Spec PySideChangeType_spec = {
        "2:PySide6.QtQmlFeatures.Change",
        sizeof(PySideChange),
        0,
        Py_TPFLAGS_DEFAULT,
        PySideChangeType_slots,
    };

    return SbkType_FromSpec(&PySideChangeType_spec);
}

PyTypeObject *PySideChange_TypeF(void)
{
    static auto *type = createChangeType();
    return type;
}

} // extern "C"

namespace PySide::Change {

static const char *Change_SignatureStrings[] = {
    "PySide6.QtQmlFeatures.Change(self,name:str,old:typing.Any,new:typing.Any,owner:typing.Any)",
    nullptr}; // Sentinel

void init(PyObject *module)
{
    auto *changeType = PySideChange_TypeF();
    if (InitSignatureStrings(changeType, Change_SignatureStrings) < 0)
        return;
    auto *obChangeType = reinterpret_cast<PyObject *>(changeType);
    Py_INCREF(obChangeType);
    PepModule_AddType(module, changeType);
}

} // namespace PySide::Change
