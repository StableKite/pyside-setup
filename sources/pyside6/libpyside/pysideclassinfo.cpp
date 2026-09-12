// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include <sbkpython.h>

#include "pysideclassinfo.h"
#include "pyside_p.h"
#include "pysideclassinfo_p.h"
#include "dynamicqmetaobject.h"
#include "dynamicqmetaobject_p.h"

#include <autodecref.h>
#include <signature.h>
#include <sbktypefactory.h>
#include <sbkstring.h>

extern "C"
{

static PyTypeObject *createClassInfoType()
{
    auto typeSlots =
        PySide::ClassDecorator::Methods<PySide::ClassInfo::ClassInfoPrivate>::typeSlots();

    PyType_Spec PySideClassInfoType_spec = {
        "2:PySide6.QtCore.ClassInfo",
        sizeof(PySideClassDecorator),
        0,
        Py_TPFLAGS_DEFAULT,
        typeSlots.data()};
    return SbkType_FromSpec(&PySideClassInfoType_spec);
};

PyTypeObject *PySideClassInfo_TypeF(void)
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createClassInfoType);
#else
    static auto *type = createClassInfoType();
    return type;
#endif
}

}  // extern "C"

namespace PySide::ClassInfo {

const char *ClassInfoPrivate::name() const
{
    return "ClassInfo";
}

PyObject *ClassInfoPrivate::tp_call(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    PyObject *klass = tp_call_check(args, CheckMode::QObjectType);
    if (klass == nullptr)
        return nullptr;

    auto *pData = DecoratorPrivate::get<ClassInfoPrivate>(self);

    if (pData->m_alreadyWrapped)
        return PyErr_Format(PyExc_TypeError, "This instance of ClassInfo() was already used to wrap an object");

    auto *klassType = reinterpret_cast<PyTypeObject *>(klass);
    if (!PySide::ClassInfo::setClassInfo(klassType, pData->m_data))
        return PyErr_Format(PyExc_TypeError, "This decorator can only be used on classes that are subclasses of QObject");

    pData->m_alreadyWrapped = true;

    Py_INCREF(klass);
    return klass;
}

int ClassInfoPrivate::tp_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *infoDict = nullptr;
    auto size = PyTuple_Size(args);
    if (size == 1 && kwds == nullptr) {
        PyObject *tmp = PyTuple_GetItem(args, 0);
        if (PyDict_Check(tmp))
            infoDict = tmp;
    } else if (size == 0 && kwds && PyDict_Check(kwds)) {
        infoDict = kwds;
    }

    if (infoDict == nullptr) {
        PyErr_Format(PyExc_TypeError, "ClassInfo() takes either keyword argument(s) or "
                                      "a single dictionary argument");
        return -1;
    }

    auto *pData = DecoratorPrivate::get<ClassInfoPrivate>(self);

    // PyDict_Next() returns borrowed references and does not lock the dictionary
    // on a free-threaded build. Take a stable snapshot first so conversion can
    // run without holding a critical section over Python operations.
    Shiboken::AutoDecRef items(PyDict_Items(infoDict));
    if (items.isNull())
        return -1;

    const Py_ssize_t itemCount = PyList_Size(items.object());
    for (Py_ssize_t i = 0; i < itemCount; ++i) {
        PyObject *item = PyList_GetItem(items.object(), i); // borrowed from items
        PyObject *key = PyTuple_GetItem(item, 0);
        PyObject *value = PyTuple_GetItem(item, 1);
        if (Shiboken::String::check(key) && Shiboken::String::check(value)) {
            ClassInfo info{Shiboken::String::toCString(key),
                           Shiboken::String::toCString(value)};
            pData->m_data.append(info);
        } else {
            PyErr_SetString(PyExc_TypeError, "All keys and values provided to ClassInfo() "
                                             "must be strings");
            return -1;
        }
    }

    return PyErr_Occurred() != nullptr ? -1 : 0;
}

static const char *ClassInfo_SignatureStrings[] = {
    "PySide6.QtCore.ClassInfo(self,**info:typing.Dict[str,str])",
    nullptr}; // Sentinel

void init(PyObject *module)
{
    if (InitSignatureStrings(PySideClassInfo_TypeF(), ClassInfo_SignatureStrings) < 0)
        return;

    auto *classInfoType = PySideClassInfo_TypeF();
    auto *obClassInfoType  = reinterpret_cast<PyObject *>(classInfoType);
    Py_INCREF(obClassInfoType);
    PepModule_AddType(module, classInfoType);
}

bool checkType(PyObject *pyObj)
{
    return pyObj != nullptr
        && PyType_IsSubtype(Py_TYPE(pyObj), PySideClassInfo_TypeF()) != 0;
}

ClassInfoList getClassInfoList(PyObject *decorator)
{
    auto *pData = PySide::ClassDecorator::DecoratorPrivate::get<ClassInfoPrivate>(decorator);
    return pData->m_data;
}

bool setClassInfo(PyTypeObject *type, const QByteArray &key,
                  const QByteArray &value)
{
    auto *userData = PySide::retrieveTypeUserData(type);
    const bool result = userData != nullptr;
    if (result) {
#ifdef Py_GIL_DISABLED
        PySide::MetaObjectBuilderLock builderLock;
#endif
        PySide::MetaObjectBuilder &mo = userData->mo;
        mo.addInfo(key, value);
    }
    return result;
}

bool setClassInfo(PyTypeObject *type, const ClassInfoList &list)
{
    auto *userData = PySide::retrieveTypeUserData(type);
    const bool result = userData != nullptr;
    if (result) {
#ifdef Py_GIL_DISABLED
        PySide::MetaObjectBuilderLock builderLock;
#endif
        PySide::MetaObjectBuilder &mo = userData->mo;
        for (const auto &info : list)
            mo.addInfo(info.key.constData(), info.value.constData());
    }
    return result;
}

} //namespace PySide::ClassInfo
