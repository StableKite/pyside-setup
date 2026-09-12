// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "pysideqmlattached.h"
#include "pysideqmlattached_p.h"
#include "pysideqmltypeinfo_p.h"
#include "pysideqmlregistertype_p.h"

#include <signalmanager.h>
#include <pysideqobject.h>
#include <pyside_p.h>
#include <pysideclassdecorator_p.h>

#include <autodecref.h>
#include <gilstate.h>
#include <sbkconverter.h>
#include <sbkpep.h>
#include <sbkstring.h>
#include <sbktypefactory.h>
#include <signature.h>

#include <QtQml/qqml.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

// The QmlAttached decorator modifies QmlElement to register an attached property
// type. Due to the (reverse) execution order of decorators, it needs to follow
// QmlElement.
class PySideQmlAttachedPrivate : public PySide::ClassDecorator::TypeDecoratorPrivate
{
public:
    PyObject *tp_call(PyObject *self, PyObject *args, PyObject * /* kw */) override;
    const char *name() const override;
};

// The call operator is passed the class type and registers the type
// in QmlTypeInfo.
PyObject *PySideQmlAttachedPrivate::tp_call(PyObject *self, PyObject *args, PyObject * /* kw */)
{
    PyObject *klass = tp_call_check(args, CheckMode::WrappedType);
    if (klass == nullptr)
        return nullptr;

    auto *data = DecoratorPrivate::get<PySideQmlAttachedPrivate>(self);
    PySide::Qml::ensureQmlTypeInfo(klass)->setAttachedType(data->type());

    Py_INCREF(klass);
    return klass;
}

const char *PySideQmlAttachedPrivate::name() const
{
    return "QmlAttached";
}

extern "C" {

static PyTypeObject *createPySideQmlAttachedType()
{
    auto typeSlots =
        PySide::ClassDecorator::Methods<PySideQmlAttachedPrivate>::typeSlots();

    PyType_Spec PySideQmlAttachedType_spec = {
        "2:PySide6.QtCore.QmlAttached",
        sizeof(PySideClassDecorator),
        0,
        Py_TPFLAGS_DEFAULT,
        typeSlots.data()
    };
    return SbkType_FromSpec(&PySideQmlAttachedType_spec);
}

PyTypeObject *PySideQmlAttached_TypeF(void)
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createPySideQmlAttachedType);
#else
    static auto *type = createPySideQmlAttachedType();
    return type;
#endif
}

} // extern "C"

static const char *qmlAttached_SignatureStrings[] = {
    "PySide6.QtQml.QmlAttached(self,type:type)",
    nullptr // Sentinel
};

namespace PySide::Qml {

static QObject *attachedFactoryHelper(PyTypeObject *attachingType, QObject *o)
{
    // Call static qmlAttachedProperties() on type. If there is an error
    // and nullptr is returned, a crash occurs. So, errors should at least be
    // printed.

    Shiboken::GilState gilState;
    Shiboken::Conversions::SpecificConverter converter("QObject");
    Q_ASSERT(converter);

    static const char methodName[] = "qmlAttachedProperties";
#ifdef Py_GIL_DISABLED
    Shiboken::AutoDecRef pyMethodNameRef(Shiboken::String::createStaticString(methodName));
    if (pyMethodNameRef.isNull())
        return nullptr;
    auto *pyMethodName = pyMethodNameRef.object();
#else
    static PyObject *const pyMethodName = Shiboken::String::createStaticString(methodName);
#endif
    auto *attachingTypeObj = reinterpret_cast<PyObject *>(attachingType);
    Shiboken::AutoDecRef pyResult(PyObject_CallMethodObjArgs(attachingTypeObj, pyMethodName,
                                                             attachingTypeObj /* self */,
                                                             converter.toPython(static_cast<void*>(&o)),
                                                             nullptr));
    if (pyResult.isNull() || PyErr_Occurred()) {
        PyErr_Print();
        return nullptr;
    }

    if (PyType_IsSubtype(pyResult->ob_type, PySide::qObjectType()) == 0) {
        qWarning("QmlAttached: Attached objects must inherit QObject, got %s.",
                 PepType_GetFullyQualifiedNameStr(Py_TYPE(pyResult)));
        return nullptr;
    }

    QObject *result = nullptr;
    converter.toCpp(pyResult.object(), static_cast<void*>(&result));
    return result;
}

// Since the required attached factory signature does not have a void *user
// parameter to store the attaching type, we employ a template trick, storing
// the attaching types in an array and create non-type-template (int) functions
// taking the array index as template parameter.
// We initialize the attachedFactories array with factory functions
// accessing the attachingTypes[N] using template metaprogramming.

enum { MAX_ATTACHING_TYPES = 50};

using AttachedFactory = QObject *(*)(QObject *);

#ifdef Py_GIL_DISABLED
static std::atomic<int> nextAttachingType{0};
static std::array<std::atomic<PyTypeObject *>, MAX_ATTACHING_TYPES> attachingTypes{};
static std::mutex attachingTypeMutex;
#else
static int nextAttachingType = 0;
static PyTypeObject *attachingTypes[MAX_ATTACHING_TYPES];
#endif
static AttachedFactory attachedFactories[MAX_ATTACHING_TYPES];

template <int N>
static QObject *attachedFactory(QObject *o)
{
#ifdef Py_GIL_DISABLED
    auto *type = attachingTypes[N].load(std::memory_order_acquire);
#else
    auto *type = attachingTypes[N];
#endif
    return attachedFactoryHelper(type, o);
}

template<int N>
struct AttachedFactoryInitializerBase
{
};

template<int N>
struct AttachedFactoryInitializer : AttachedFactoryInitializerBase<N>
{
    static void init()
    {
        attachedFactories[N] = attachedFactory<N>;
        AttachedFactoryInitializer<N-1>::init();
    }
};

template<>
struct  AttachedFactoryInitializer<0> : AttachedFactoryInitializerBase<0>
{
    static void init()
    {
        attachedFactories[0] = attachedFactory<0>;
    }
};

void initQmlAttached(PyObject *module)
{
#ifdef Py_GIL_DISABLED
    for (auto &type : attachingTypes)
        type.store(nullptr, std::memory_order_relaxed);
    nextAttachingType.store(0, std::memory_order_relaxed);
#else
    std::fill(attachingTypes, attachingTypes + MAX_ATTACHING_TYPES, nullptr);
    nextAttachingType = 0;
#endif
    AttachedFactoryInitializer<MAX_ATTACHING_TYPES - 1>::init();

    auto *qmlAttachedType = PySideQmlAttached_TypeF();
    if (InitSignatureStrings(qmlAttachedType, qmlAttached_SignatureStrings) < 0)
        return;

    auto *obQmlAttachedType = reinterpret_cast<PyObject *>(qmlAttachedType);
    Py_INCREF(obQmlAttachedType);
    PepModule_AddType(module, qmlAttachedType);
}

PySide::Qml::QmlExtensionInfo qmlAttachedInfo(PyTypeObject *t,
                                              const std::shared_ptr<QmlTypeInfo> &info)
{
    PySide::Qml::QmlExtensionInfo result{nullptr, nullptr};
    if (!info)
        return result;
    const auto data = info->data();
    if (data.attachedType == nullptr)
        return result;

    const auto *name = PepType_GetFullyQualifiedNameStr(t);
    result.metaObject = PySide::retrieveMetaObject(data.attachedType);
    if (result.metaObject == nullptr) {
        qWarning("Unable to retrieve meta object for %s", name);
        return result;
    }

    int index = -1;
#ifdef Py_GIL_DISABLED
    {
        const std::lock_guard guard(attachingTypeMutex);
        index = nextAttachingType.load(std::memory_order_relaxed);
        if (index < MAX_ATTACHING_TYPES) {
            attachingTypes[index].store(t, std::memory_order_release);
            nextAttachingType.store(index + 1, std::memory_order_release);
        }
    }
#else
    index = nextAttachingType;
    if (index < MAX_ATTACHING_TYPES) {
        attachingTypes[index] = t;
        ++nextAttachingType;
    }
#endif
    if (index >= MAX_ATTACHING_TYPES) {
        qWarning("Unable to initialize attached type \"%s\": "
                 "The limit %d of  attached types has been reached.",
                 name, MAX_ATTACHING_TYPES);
        return {nullptr, nullptr};
    }

    result.factory = attachedFactories[index];
    return result;
}

QObject *qmlAttachedPropertiesObject(PyObject *typeObject, QObject *obj, bool create)
{
    auto *type = reinterpret_cast<PyTypeObject *>(typeObject);
#ifdef Py_GIL_DISABLED
    const int count = nextAttachingType.load(std::memory_order_acquire);
#else
    const int count = nextAttachingType;
#endif
    int index = 0;
    for (; index < count; ++index) {
#ifdef Py_GIL_DISABLED
        const auto *candidate = attachingTypes[index].load(std::memory_order_acquire);
#else
        const auto *candidate = attachingTypes[index];
#endif
        if (candidate == type)
            break;
    }
    if (index == count) {
        qWarning("%s: Attaching type \"%s\" not found.", __FUNCTION__,
                 PepType_GetFullyQualifiedNameStr(type));
        return nullptr;
    }

    return ::qmlAttachedPropertiesObject(obj, attachedFactories[index], create);
}

} // namespace PySide::Qml
