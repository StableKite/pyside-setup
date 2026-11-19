// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:low-level-memory-management

#include "autodecref.h"
#include "basewrapper.h"
#include "sbkreference.h"
#include <new>
#include "basewrapper_p.h"
#include "bindingmanager.h"
#include "gilstate.h"
#include "helper.h"
#include "pep384ext.h"
#include "pep384impl_p.h"
#include "sbkconverter.h"
#include "sbkerrors.h"
#include "sbkfeature_base.h"
#include "sbkstaticstrings.h"
#include "sbkstaticstrings_p.h"
#include "sbkstring.h"
#include "sbktypefactory.h"
#include "signature.h"
#include "signature_p.h"
#include "threadstatesaver.h"
#include "voidptr.h"
#ifdef Py_GIL_DISABLED
#  include "sbkftoptions.h"
#  include "sbkstatelock.h"
#  include "sbkmainthreaddeletion.h"
#  include <condition_variable>
#  include <mutex>
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <string>

#ifdef __APPLE__
#  include <dlfcn.h>
#endif

namespace {
    void _destroyParentInfo(SbkObject *obj, bool keepReference);
    void _detachChildren(SbkObject *obj, bool keepReference);
}

struct BaseWrapperGlobals
{
#ifndef Py_GIL_DISABLED
    PyTypeObject *sbkObjectType = nullptr;
    PyTypeObject *sbkObjectMetaType = nullptr;
#endif
    PyObject *qAppLast = nullptr;
};

static BaseWrapperGlobals *baseWrapperGlobals()
{
    static BaseWrapperGlobals result;
    return &result;
}

extern "C" { static int mainThreadDeletionHandler(void *); }

namespace Shiboken
{
#ifdef Py_GIL_DISABLED
namespace Object {
static std::atomic<MainThreadDeletionWakeUp> mainThreadDeletionWakeUp{nullptr};

void setMainThreadDeletionWakeUp(MainThreadDeletionWakeUp wakeUp) noexcept
{
    mainThreadDeletionWakeUp.store(wakeUp, std::memory_order_release);
}

static void notifyMainThreadDeletion()
{
    if (auto wakeUp = mainThreadDeletionWakeUp.load(std::memory_order_acquire))
        wakeUp();
    // The Python pending call also covers a main thread outside a Qt loop.
    // A Qt dispatcher wake-up remains effective if the Python queue is full.
    Py_AddPendingCall(mainThreadDeletionHandler, nullptr);
}
} // namespace Object
// Serializes ONLY raw SbkObjectTypePrivate MI fields, across unrelated wrappers.
// Do not acquire the wrapper map lock while this is held. In particular,
// no CPython API, QObject or destructor call may be made in this section.
static std::mutex miTypeMetadataMutex;
#endif

// Walk through the first level of non-user-type Sbk base classes relevant for
// C++ object allocation. Return true from the predicate to terminate.
template <class Predicate>
bool walkThroughBases(PyTypeObject *currentType, Predicate predicate)
{
#ifdef Py_GIL_DISABLED
    // Pin a coherent bases tuple before releasing the type critical section.
    // Never keep that section across recursion or a predicate's Python C API.
    PyObject *bases = nullptr;
    Py_BEGIN_CRITICAL_SECTION(reinterpret_cast<PyObject *>(currentType));
    bases = Py_NewRef(currentType->tp_bases);
    Py_END_CRITICAL_SECTION();
    AutoDecRef basesReference(bases);
#else
    PyObject *bases = currentType->tp_bases;
#endif
    const Py_ssize_t numBases = PyTuple_Size(bases);
    bool result = false;
    for (Py_ssize_t i = 0; !result && i < numBases; ++i) {
        auto *type = reinterpret_cast<PyTypeObject *>(PyTuple_GetItem(bases, i));
        if (PyType_IsSubtype(type, SbkObject_TypeF()) != 0) {
            result = PepType_SOTP(type)->is_user_type
                     ? walkThroughBases(type, predicate) : predicate(type);
        }
    }
    return result;
}

int getTypeIndexOnHierarchy(PyTypeObject *baseType, PyTypeObject *desiredType)
{
    if (desiredType == nullptr)
        return -1;
    int index = -1;
    const bool found = walkThroughBases(baseType, [&index, desiredType](PyTypeObject *node) {
        ++index;
        return PyType_IsSubtype(node, desiredType) != 0;
    });
    // A missing C++ base must NOT alias the last occupied pointer slot.
    return found ? index : -1;
}

int getNumberOfCppBaseClasses(PyTypeObject *baseType)
{
    int count = 0;
    walkThroughBases(baseType, [&count](PyTypeObject *) {
        ++count;
        return false;
    });
    return count;
}

std::vector<PyTypeObject *> getCppBaseClasses(PyTypeObject *baseType)
{
    std::vector<PyTypeObject *> cppBaseClasses;
    walkThroughBases(baseType, [&cppBaseClasses](PyTypeObject *node) {
        cppBaseClasses.push_back(node);
        return false;
    });
    return cppBaseClasses;
}

using DestructorEntries = std::vector<DestructorEntry>;

// Types and destructors describe actual C++ allocations, never a mutable MRO.
static PyTypeObject *nativeTargetType(PyTypeObject *type)
{
    if (type == nullptr)
        return nullptr;
    auto *metadata = PepType_SOTP(type);
    if (metadata != nullptr && metadata->is_user_type) {
        const auto bases = getCppBaseClasses(type);
        return bases.size() == 1 ? bases.front() : nullptr;
    }
    return type;
}

static int objectTypeIndex(SbkObject *self, PyTypeObject *desiredType)
{
    desiredType = nativeTargetType(desiredType);
    if (desiredType == nullptr)
        return -1;
    const auto &types = self->d->cppTypes;
    for (size_t i = 0; i < types.size(); ++i) {
        if (PyType_IsSubtype(types[i], desiredType) != 0)
            return int(i);
    }
    return -1;
}

// Keep metadata alive until after a deallocation's C++ destructors, even if
// __bases__ no longer retains the native providers. Move, never allocate here.
struct NativeTypeReferences
{
    std::vector<PyTypeObject *> types;
    ~NativeTypeReferences()
    {
        if (canReleaseOwnedPythonReferences()) {
            for (auto *type : types)
                Py_DECREF(type);
        }
    }
};

DestructorEntries getDestructorEntries(SbkObject *o)
{
    DestructorEntries result;
    auto *d = o->d;
    if (d->cptr == nullptr)
        return result;
    result.reserve(d->cppTypes.size());
    for (size_t i = 0; i < d->cppTypes.size(); ++i) {
        auto destructor = d->cppDestructors[i];
        if (destructor != nullptr && d->cptr[i] != nullptr)
            result.push_back(DestructorEntry{destructor, d->cptr[i]});
    }
    return result;
}

static void callDestructor(const DestructorEntries &dts)
{
    for (const auto &e : dts) {
        Shiboken::ThreadStateSaver threadSaver;
        threadSaver.save();
        e.destructor(e.cppInstance);
    }
}

} // namespace Shiboken

extern "C"
{

// PYSIDE-939: A general replacement for object_dealloc.
void Sbk_object_dealloc(PyObject *self)
{
    // PYSIDE-939: Handling references correctly.
    // This was not needed before Python 3.8 (Python issue 35810)
    Py_DECREF(Py_TYPE(self));

    PepExt_TypeCallFree(self);
}

struct SbkDynamicMethodDef
{
    PyMethodDef method;
    char *name;
    SbkDynamicMethodDef *next;
};

static void SbkObjectType_tp_dealloc(PyTypeObject *type);
static PyTypeObject *SbkObjectType_tp_new(PyTypeObject *metatype, PyObject *args, PyObject *kwds);

static DestroyQAppHook DestroyQApplication = nullptr;

// PYSIDE-1470: Provide a hook to kill an Application from Shiboken.
void setDestroyQApplication(DestroyQAppHook func)
{
    DestroyQApplication = func;
}

// PYSIDE-535: Use the C API in PyPy instead of `op->ob_dict`, directly.
// A free-threaded build needs the same path: PyObject_GenericGetDict()
// synchronizes lazy dictionary creation, whereas a check-then-store of
// ob_dict lets two threads publish different dictionaries.
LIBSHIBOKEN_API PyObject *SbkObject_GetDict_NoRef(PyObject *op)
{
    assert(Shiboken::Object::checkType(op));
#if defined(PYPY_VERSION) || defined(Py_GIL_DISABLED)
    Shiboken::GilState state;
    auto *ret = PyObject_GenericGetDict(op, nullptr);
    if (ret != nullptr)
        Py_DECREF(ret);
    return ret;
#else
    auto *sbkObj = reinterpret_cast<SbkObject *>(op);
    if (!sbkObj->ob_dict) {
        Shiboken::GilState state;
        sbkObj->ob_dict = PyDict_New();
    }
    return sbkObj->ob_dict;
#endif
}

static int
check_set_special_type_attr(PyTypeObject *type, PyObject *value, const char *name)
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_Format(PyExc_TypeError,
                     "can't set %s.%s", type->tp_name, name);
        return 0;
    }
    if (!value) {
        PyErr_Format(PyExc_TypeError,
                     "can't delete %s.%s", type->tp_name, name);
        return 0;
    }
    return 1;
}

// PYSIDE-1177: Add a setter to allow setting type doc.
static int
type_set_doc(PyObject *obType, PyObject *value, void * /* context */)
{
    auto *type = reinterpret_cast<PyTypeObject *>(obType);
    if (!check_set_special_type_attr(type, value, "__doc__"))
        return -1;
    PyType_Modified(type);
    Shiboken::AutoDecRef tpDict(PepType_GetDict(type));
    const int result = PyDict_SetItem(tpDict.object(), Shiboken::PyMagicName::doc(), value);
    if (result == 0)
        SbkObjectType_NotifyFeatureUpdate(type);
    return result;
}

static int SbkObjectType_tp_setattro(PyObject *typeOb, PyObject *name, PyObject *value)
{
    const int result = PepExt_Type_GetSetAttroSlot(&PyType_Type)(typeOb, name, value);
    if (result == 0)
        SbkObjectType_NotifyFeatureUpdate(reinterpret_cast<PyTypeObject *>(typeOb));
    return result;
}

// PYSIDE-908: The function PyType_Modified does not work in PySide, so we need to
// explicitly pass __doc__.
static PyGetSetDef SbkObjectType_tp_getset[] = {
    {"__doc__",  Sbk_TypeGet___doc__, type_set_doc, nullptr, nullptr},
    {"__dict__",  Sbk_TypeGet___dict__, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}  // Sentinel
};

// CPython calls Py_tp_getattro through getattrofunc, whose first argument
// is PyObject*. Casting a function taking PyTypeObject* to that slot is an
// incompatible indirect call (caught by UBSan's function check).
static PyObject *SbkObjectType_tp_getattro(PyObject *object, PyObject *name)
{
    return mangled_type_getattro(reinterpret_cast<PyTypeObject *>(object), name);
}

static PyObject *SbkObjectType_tp_new_slot(PyTypeObject *type, PyObject *args,
                                             PyObject *keywords)
{
    return reinterpret_cast<PyObject *>(SbkObjectType_tp_new(type, args, keywords));
}

static void SbkObjectType_tp_dealloc_slot(PyObject *object)
{
    SbkObjectType_tp_dealloc(reinterpret_cast<PyTypeObject *>(object));
}

static PyTypeObject *createObjectTypeType()
{
    PyType_Slot SbkObjectType_Type_slots[] = {
        {Py_tp_dealloc, reinterpret_cast<void *>(SbkObjectType_tp_dealloc_slot)},
        {Py_tp_getattro, reinterpret_cast<void *>(SbkObjectType_tp_getattro)},
        {Py_tp_base, static_cast<void *>(&PyType_Type)},
        {Py_tp_alloc, reinterpret_cast<void *>(PyType_GenericAlloc)},
        {Py_tp_new, reinterpret_cast<void *>(SbkObjectType_tp_new_slot)},
        {Py_tp_setattro, reinterpret_cast<void *>(SbkObjectType_tp_setattro)},
        {Py_tp_free, reinterpret_cast<void *>(PyObject_GC_Del)},
        {Py_tp_getset, reinterpret_cast<void *>(SbkObjectType_tp_getset)},
        {0, nullptr}
    };

    // PYSIDE-535: The tp_itemsize field is inherited and does not need to be set.
    //             In PyPy, it _must_ not be set, because it would have the meanin
    //             that a `__len__` field must be defined. Not doing so creates
    //             a hard-to-find crash.
    //
    // PYSIDE-2230: In Python < 3.12, the decision which base class should create
    //              the instance is arbitrarily drawn by the size of the type.
    //              Ignoring this creates a bug in the new version of bug_825 that
    //              selects the wrong metatype.
    //
    PyType_Spec SbkObjectType_Type_spec = {
#if SHIBOKEN_MAJOR_VERSION >= 7 // PYSIDE-3336
        "2:shiboken6.Shiboken.ObjectType",
#else
        "1:Shiboken.ObjectType",
#endif
        static_cast<int>(PyType_Type.tp_basicsize) + 1,           // see above
        0, // sizeof(PyMemberDef), not for PyPy without a __len__ defined
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_TYPE_SUBCLASS,
        SbkObjectType_Type_slots,
    };

    PyType_Spec SbkObjectType_Type_spec_312 = {
#if SHIBOKEN_MAJOR_VERSION >= 7 // PYSIDE-3336
        "2:shiboken6.Shiboken.ObjectType",
#else
        "1:Shiboken.ObjectType",
#endif
        -long(sizeof(SbkObjectTypePrivate)),
        0, // sizeof(PyMemberDef), not for PyPy without a __len__ defined
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_TYPE_SUBCLASS,
        SbkObjectType_Type_slots,
    };

    return SbkType_FromSpec(_PepRuntimeVersion() >= 0x030C00 ?
                            &SbkObjectType_Type_spec_312 :
                            &SbkObjectType_Type_spec);
}

PyTypeObject *SbkObjectType_TypeF(void)
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createObjectTypeType);
#else
    auto *globals = baseWrapperGlobals();
    if (globals->sbkObjectMetaType == nullptr)
        globals->sbkObjectMetaType = createObjectTypeType();
    return globals->sbkObjectMetaType;
#endif
}

static PyObject *SbkObjectGetDict(PyObject *pObj, void *)
{
    auto *ret = SbkObject_GetDict_NoRef(pObj);
    Py_XINCREF(ret);
    return ret;
}

static PyGetSetDef SbkObject_tp_getset[] = {
    {const_cast<char *>("__dict__"), SbkObjectGetDict, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} // Sentinel
};

static int SbkObject_tp_traverse(PyObject *self, visitproc visit, void *arg)
{
    auto *sbkSelf = reinterpret_cast<SbkObject *>(self);

    // Native allocation types remain owned even when Python bases are changed.
    for (auto *type : sbkSelf->d->cppTypes)
        Py_VISIT(type);

    //Visit children
    if (auto *pInfo = sbkSelf->d->parentInfo) {
        for (SbkObject *c : pInfo->children)
             Py_VISIT(c);
    }

    //Visit refs
    if (auto *rInfo = sbkSelf->d->referredObjects) {
        for (const auto &p : *rInfo)
            Py_VISIT(p.second);
    }

    if (sbkSelf->ob_dict)
        Py_VISIT(sbkSelf->ob_dict);

    // GC_NewVar owns the ordinary heap-type reference. _setupNew also
    // takes a separate wrapper reference, paired with the second DECREF in
    // SbkDeallocWrapperCommon. Both edges must be visible to cyclic GC.
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static int SbkObject_tp_clear(PyObject *self)
{
    auto *sbkSelf = reinterpret_cast<SbkObject *>(self);

    // Only the children are detached here, never the link to our own C++
    // parent: that parent still holds this object in its C++ child list and
    // deletes it, so handing ownership back to Python would make the wrapper
    // delete it a second time. The reference cycle is broken by clearing the
    // instance dict below, and the parent link is torn down when the parent
    // itself is cleared or deallocated.
    if (sbkSelf->d->parentInfo)
        _detachChildren(sbkSelf, true);

    Shiboken::Object::clearReferences(sbkSelf);

    if (sbkSelf->ob_dict)
        Py_CLEAR(sbkSelf->ob_dict);
    return 0;
}

static PyTypeObject *createObjectType()
{
    PyType_Slot SbkObject_Type_slots[] = {
        {Py_tp_getattro, reinterpret_cast<void *>(SbkObject_GenericGetAttr)},
        {Py_tp_setattro, reinterpret_cast<void *>(SbkObject_GenericSetAttr)},
        {Py_tp_dealloc, reinterpret_cast<void *>(SbkDeallocWrapperWithPrivateDtor)},
        {Py_tp_traverse, reinterpret_cast<void *>(SbkObject_tp_traverse)},
        {Py_tp_clear, reinterpret_cast<void *>(SbkObject_tp_clear)},
        // unsupported: {Py_tp_weaklistoffset, (void *)offsetof(SbkObject, weakreflist)},
        {Py_tp_getset, reinterpret_cast<void *>(SbkObject_tp_getset)},
        // unsupported: {Py_tp_dictoffset, (void *)offsetof(SbkObject, ob_dict)},
        {0, nullptr}
    };

    PyType_Spec SbkObject_Type_spec = {
#if SHIBOKEN_MAJOR_VERSION >= 7 // PYSIDE-3336
        "2:shiboken6.Shiboken.Object",
#else
        "1:Shiboken.Object",
#endif
        sizeof(SbkObject),
        0,
        Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_HAVE_GC,
        SbkObject_Type_slots,
    };

    // PYSIDE-2230: When creating this type, we cannot easily handle the metaclass.
    //              In versions < Python 3.12, the metaclass can only be set
    //              indirectly by a base which has that metaclass.
    //              But before 3.12 is the minimum version, we cannot use the new
    //              function, although we would need this for 3.12 :-D
    //              We do a special patching here that is triggered through Py_None.
    auto *type = SbkType_FromSpec_BMDWB(&SbkObject_Type_spec,
                                        Py_None,     // bases, spectial flag!
                                        SbkObjectType_TypeF(),
                                        offsetof(SbkObject, ob_dict),
                                        offsetof(SbkObject, weakreflist),
                                        nullptr);    // bufferprocs
    return type;
}

PyTypeObject *SbkObject_TypeF(void)
{
#ifdef Py_GIL_DISABLED
    static std::atomic<PyTypeObject *> type{nullptr};
    return Shiboken::TypeInit::publish(type, createObjectType);
#else
    auto *globals = baseWrapperGlobals();
    if (globals->sbkObjectType == nullptr)
        globals->sbkObjectType = createObjectType();    // bufferprocs
    return globals->sbkObjectType;
#endif
}

static const char *SbkObject_SignatureStrings[] = {
    "Shiboken.Object(self)",
    nullptr}; // Sentinel

static int mainThreadDeletionHandler(void *)
{
    if (Py_IsInitialized()) {
        Shiboken::BindingManager::instance().runDeletionInMainThread();
#ifdef Py_GIL_DISABLED
        Shiboken::Object::runPendingDestructions();
#endif
    }
    return 0;
}

#ifdef Py_GIL_DISABLED
// Free-threaded twin of the deallocation path. Kept here, not in the
// state-lock chapter, because it is static and used a few lines down. It
// follows the twin below step for step, apart from the transactions and the
// __APPLE__ diagnostic, which is not worth having twice.
static void SbkDeallocWrapperCommon(PyObject *pyObj, bool canDelete)
{
    SBK_ASSERT_STATE_UNLOCKED();
    auto *sbkObj = reinterpret_cast<SbkObject *>(pyObj);
    PyTypeObject *pyType = Py_TYPE(pyObj);

    // Need to decref the type if this is the dealloc func; if type
    // is subclassed, that dealloc func will decref (see subtype_dealloc
    // in typeobject.c in the python sources)
    auto *dealloc = PyType_GetSlot(pyType, Py_tp_dealloc);

    // PYSIDE-939: Additional rule: Also when a subtype is heap allocated,
    // then the subtype_dealloc deref will be suppressed, and we need again
    // to supply a decref.
    const bool needTypeDecref = dealloc == SbkDeallocWrapper
        || dealloc == SbkDeallocWrapperWithPrivateDtor
        || (pyType->tp_base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0;

    // Ensure that the GC is no longer tracking this object to avoid a
    // possible reentrancy problem.  Since there are multiple steps involved
    // in deallocating a SbkObject it is possible for the garbage collector to
    // be invoked and it trying to delete this object while it is still in
    // progress from the first time around, resulting in a double delete and a
    // crash.
    PyObject_GC_UnTrack(pyObj);

    // Take the object out of the wrapper map before anything below can run
    // Python code. The refcount is already zero here, but the map used to keep
    // the entry until deallocData(), several steps down. A lookup in between -
    // a weakref callback, a __del__, C++ re-entering the binding - was handed
    // this very wrapper and increfed it back to life, while the deallocation
    // carried on and freed it underneath. The invariant callers rely on is
    // "in the map implies alive", and this is where it has to be restored.
    // Only the entry goes; the flags stay, canDelete below still reads them.
    auto &bindingManager = Shiboken::BindingManager::instance();
    bindingManager.unregisterWrapper(sbkObj);

    // Check that Python is still initialized as sometimes this is called by a static destructor
    // after Python interpeter is shutdown.
    if (sbkObj->weakreflist && Py_IsInitialized())
        PyObject_ClearWeakRefs(pyObj);

    // If I have ownership and is valid delete C++ pointer
    const bool isMultiCpp = sbkObj->d->cptrCount > 1;
    auto *nativeType = !isMultiCpp && !sbkObj->d->cppTypes.empty()
        ? sbkObj->d->cppTypes.front() : pyType;
    auto *sotp = PepType_SOTP(nativeType);
    const auto destructor = sbkObj->d->cppDestructors.front();
    Shiboken::NativeTypeReferences nativeTypeReferences;
    Shiboken::DestructorEntries entries;
    void *cptr = nullptr;
    {
        // Read-only: at refcount zero nobody else is here, but the lock owns
        // these fields. getDestructorEntries() below is the same exemption
        // extractDestructionLocked() takes - it reads tp_bases and the type
        // extension, runs no user code and allocates nothing through Python.
        Shiboken::StateLockGuard guard;
        auto *priv = sbkObj->d;
        // destroy() and callCppDestructors() detach cptr under this lock, so a
        // wrapper that went through them arrives with none.
        canDelete &= priv->hasOwnership && priv->validCppObject && priv->cptr != nullptr;
        if (canDelete) {
            if (isMultiCpp)
                entries = Shiboken::getDestructorEntries(sbkObj);
            else
                cptr = priv->cptr[0];
        }
    }

    if (canDelete && sotp->delete_in_main_thread
        && Shiboken::currentThreadId() != Shiboken::mainThreadId()) {
        if (isMultiCpp) {
            for (const auto &e : entries)
                bindingManager.addToDeletionInMainThread(e);
        } else {
            bindingManager.addToDeletionInMainThread({destructor, cptr});
        }
        Shiboken::Object::notifyMainThreadDeletion();
        canDelete = false;
    }

    /* Save the current exception, if any. */
    Shiboken::Errors::Stash errorStash;

    // deallocData() frees the wrapper, so everything the destructors need was
    // read above.
    nativeTypeReferences.types.swap(sbkObj->d->cppTypes);
    Shiboken::Object::deallocData(sbkObj, true);
    if (canDelete) {
        if (isMultiCpp) {
            callDestructor(entries);
        } else {
            Shiboken::ThreadStateSaver threadSaver;
            if (Py_IsInitialized())
                threadSaver.save();
            destructor(cptr);
        }
    }

    /* Restore the saved exception. */
    errorStash.restore();

    if (needTypeDecref)
        Py_DECREF(pyType);
    // PYSIDE-939: Handling references correctly.
    // This was not needed before Python 3.8 (Python issue 35810)
    Py_DECREF(pyType);
}
#else // Py_GIL_DISABLED
static void SbkDeallocWrapperCommon(PyObject *pyObj, bool canDelete)
{
    auto *sbkObj = reinterpret_cast<SbkObject *>(pyObj);
    PyTypeObject *pyType = Py_TYPE(pyObj);

    // Need to decref the type if this is the dealloc func; if type
    // is subclassed, that dealloc func will decref (see subtype_dealloc
    // in typeobject.c in the python sources)
    auto *dealloc = PyType_GetSlot(pyType, Py_tp_dealloc);

    // PYSIDE-939: Additional rule: Also when a subtype is heap allocated,
    // then the subtype_dealloc deref will be suppressed, and we need again
    // to supply a decref.
    const bool needTypeDecref = dealloc == SbkDeallocWrapper
        || dealloc == SbkDeallocWrapperWithPrivateDtor
        || (pyType->tp_base->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0;

#ifdef __APPLE__
    // Just checking once that our assumptions are right.
    if (false) {
        void *p = PyType_GetSlot(pyType, Py_tp_dealloc);
        Dl_info dl_info;
        dladdr(p, &dl_info);
        fprintf(stderr, "tp_dealloc is %s\n", dl_info.dli_sname);
    }
    // Gives one of our functions
    //  "Sbk_object_dealloc"
    //  "SbkDeallocWrapperWithPrivateDtor"
    //  "SbkDeallocQAppWrapper"
    //  "SbkDeallocWrapper"
    // but for typedealloc_test.py we get
    //  "subtype_dealloc"
#endif

    // Ensure that the GC is no longer tracking this object to avoid a
    // possible reentrancy problem.  Since there are multiple steps involved
    // in deallocating a SbkObject it is possible for the garbage collector to
    // be invoked and it trying to delete this object while it is still in
    // progress from the first time around, resulting in a double delete and a
    // crash.
    PyObject_GC_UnTrack(pyObj);

    // Take the object out of the wrapper map before anything below can run
    // Python code. The refcount is already zero here, but the map used to keep
    // the entry until deallocData(), several steps down. A lookup in between -
    // a weakref callback, a __del__, C++ re-entering the binding - was handed
    // this very wrapper and increfed it back to life, while the deallocation
    // carried on and freed it underneath. The invariant callers rely on is
    // "in the map implies alive", and this is where it has to be restored.
    // Only the entry goes; the flags stay, canDelete below still reads them.
    auto &bindingManager = Shiboken::BindingManager::instance();
    bindingManager.unregisterWrapper(sbkObj);

    // Check that Python is still initialized as sometimes this is called by a static destructor
    // after Python interpeter is shutdown.
    if (sbkObj->weakreflist && Py_IsInitialized())
        PyObject_ClearWeakRefs(pyObj);

    // If I have ownership and is valid delete C++ pointer
    const bool isMultiCpp = sbkObj->d->cptrCount > 1;
    auto *nativeType = !isMultiCpp && !sbkObj->d->cppTypes.empty()
        ? sbkObj->d->cppTypes.front() : pyType;
    auto *sotp = PepType_SOTP(nativeType);
    const auto destructor = sbkObj->d->cppDestructors.front();
    Shiboken::NativeTypeReferences nativeTypeReferences;
    canDelete &= sbkObj->d->hasOwnership && sbkObj->d->validCppObject;
    if (canDelete) {
        if (sotp->delete_in_main_thread && Shiboken::currentThreadId() != Shiboken::mainThreadId()) {
            if (isMultiCpp) {
                 const auto entries = Shiboken::getDestructorEntries(sbkObj);
                 for (const auto &e : entries)
                     bindingManager.addToDeletionInMainThread(e);
            } else {
                Shiboken::DestructorEntry e{destructor, sbkObj->d->cptr[0]};
                bindingManager.addToDeletionInMainThread(e);
            }
            Py_AddPendingCall(mainThreadDeletionHandler, nullptr);
            canDelete = false;
        }
    }

    /* Save the current exception, if any. */
    Shiboken::Errors::Stash errorStash;

    if (canDelete) {
        if (isMultiCpp) {
            const auto entries = Shiboken::getDestructorEntries(sbkObj);
            nativeTypeReferences.types.swap(sbkObj->d->cppTypes);
            Shiboken::Object::deallocData(sbkObj, true);
            callDestructor(entries);
        } else {
            void *cptr = sbkObj->d->cptr[0];
            nativeTypeReferences.types.swap(sbkObj->d->cppTypes);
            Shiboken::Object::deallocData(sbkObj, true);

            Shiboken::ThreadStateSaver threadSaver;
            if (Py_IsInitialized())
                threadSaver.save();
            destructor(cptr);
        }
    } else {
        nativeTypeReferences.types.swap(sbkObj->d->cppTypes);
        Shiboken::Object::deallocData(sbkObj, true);
    }

    /* Restore the saved exception. */
    errorStash.restore();

    if (needTypeDecref)
        Py_DECREF(pyType);
    // PYSIDE-939: Handling references correctly.
    // This was not needed before Python 3.8 (Python issue 35810)
    Py_DECREF(pyType);
}
#endif // Py_GIL_DISABLED

static inline PyObject *_Sbk_NewVarObject(PyTypeObject *type)
{
    // PYSIDE-1970: Support __slots__, implemented by PyVarObject
    auto const baseSize = sizeof(SbkObject);
    auto varCount = Py_SIZE(reinterpret_cast<PyObject *>(type));
    auto *self = PyObject_GC_NewVar(PyObject, type, varCount);
    if (self != nullptr && varCount)
        std::memset(reinterpret_cast<char *>(self) + baseSize, 0, varCount * sizeof(void *));
    return self;
}

void SbkDeallocWrapper(PyObject *pyObj)
{
    SbkDeallocWrapperCommon(pyObj, true);
}

#ifdef Py_GIL_DISABLED
static bool beginQAppDestruction(PyObject *pyObj);
static void finishQAppDestruction();
#endif

void SbkDeallocQAppWrapper(PyObject *pyObj)
{
#ifdef Py_GIL_DISABLED
    const bool trackedQApp = beginQAppDestruction(pyObj);
    SbkDeallocWrapper(pyObj);
    if (trackedQApp)
        finishQAppDestruction();
#else
    SbkDeallocWrapper(pyObj);
    // PYSIDE-571: make sure to create a singleton deleted qApp.
    Py_DECREF(MakeQAppWrapper(nullptr));
#endif
}

void SbkDeallocWrapperWithPrivateDtor(PyObject *self)
{
    SbkDeallocWrapperCommon(self, false);
}

void SbkObjectType_tp_dealloc(PyTypeObject *type)
{
    SbkObjectTypePrivate *sotp = PepType_SOTP(type);
    auto *pyObj = reinterpret_cast<PyObject *>(type);
    SbkDynamicMethodDef *definitions = nullptr;

    PyObject_GC_UnTrack(pyObj);
#if !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
    Py_TRASHCAN_BEGIN(pyObj, 1);
#endif
    if (sotp) {
        definitions = sotp->dynamicMethodDefs;
        sotp->dynamicMethodDefs = nullptr;
        if (sotp->user_data && sotp->d_func) {
            sotp->d_func(sotp->user_data);
            sotp->user_data = nullptr;
        }
        free(sotp->original_name);
        sotp->original_name = nullptr;
        if (!Shiboken::ObjectType::isUserType(type))
            Shiboken::Conversions::deleteConverter(sotp->converter);
        PepType_SOTP_delete(type);
    }
#if !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
    Py_TRASHCAN_END;
#endif
#if !defined(Py_LIMITED_API) && !defined(PYPY_VERSION) && PY_VERSION_HEX >= 0x030C0000
    // The embedded Shiboken metadata is gone, but CPython still owns the
    // heap type's dict, bases, MRO, weakrefs and allocation. Its metatype's
    // deallocator releases those, including the metatype reference itself.
    PyType_Type.tp_dealloc(pyObj);
#else
    // PYSIDE-939: Handling references correctly.
    // This was not needed before Python 3.8 (Python issue 35810)
    Py_DECREF(Py_TYPE(pyObj));
#endif
    while (definitions != nullptr) {
        auto *next = definitions->next;
        PyMem_RawFree(definitions->name);
        PyMem_RawFree(definitions);
        definitions = next;
    }
}

////////////////////////////////////////////////////////////////////////////
//
// Support for the qApp macro.
//
// qApp is a macro in Qt5. In Python, we simulate that a little by a
// variable that monitors Q*Application.instance().
// This variable is also able to destroy the app by qApp.shutdown().
//

#ifdef Py_GIL_DISABLED
enum class QAppState
{
    Ready,
    Updating,
    Destroying
};

static std::mutex qAppMutex;
static std::condition_variable qAppCondition;
static QAppState qAppState = QAppState::Ready;

static void waitForQAppState(std::unique_lock<std::mutex> &lock, QAppState state)
{
    while (qAppState == state) {
        // A waiter must not keep an attached Python thread while the owner is
        // doing Python C API work. Otherwise a free-threaded stop-the-world
        // pause can wait for this thread while this thread waits for the owner.
        lock.unlock();
        Shiboken::ThreadStateSaver saver;
        saver.save();
        lock.lock();
        qAppCondition.wait(lock, [state] { return qAppState != state; });
        lock.unlock();
        saver.restore();
        lock.lock();
    }
}

static bool beginQAppCreation(PyObject **previous)
{
    std::unique_lock<std::mutex> lock(qAppMutex);
    waitForQAppState(lock, QAppState::Updating);

    auto *globals = baseWrapperGlobals();
    if (qAppState == QAppState::Destroying || globals->qAppLast != Py_None) {
        auto *last = qAppState == QAppState::Destroying ? nullptr : globals->qAppLast;
        Py_XINCREF(last);
        *previous = last;
        return false;
    }

    qAppState = QAppState::Updating;
    return true;
}

static void beginQAppNoneUpdate()
{
    std::unique_lock<std::mutex> lock(qAppMutex);
    while (qAppState != QAppState::Ready) {
        const auto state = qAppState;
        waitForQAppState(lock, state);
    }
    qAppState = QAppState::Updating;
}

static void finishQAppUpdate(PyObject *current)
{
    {
        std::lock_guard<std::mutex> lock(qAppMutex);
        if (current != nullptr)
            baseWrapperGlobals()->qAppLast = current;
        qAppState = QAppState::Ready;
    }
    qAppCondition.notify_all();
}

static bool beginQAppDestruction(PyObject *pyObj)
{
    std::unique_lock<std::mutex> lock(qAppMutex);
    waitForQAppState(lock, QAppState::Updating);

    auto *globals = baseWrapperGlobals();
    if (qAppState != QAppState::Ready || globals->qAppLast != pyObj)
        return false;

    // No creator may inspect the pointer after the wrapper starts dying.
    // Keep a distinct Destroying state so a concurrent constructor fails as
    // it did with the GIL instead of dereferencing a zero-refcount wrapper.
    globals->qAppLast = nullptr;
    qAppState = QAppState::Destroying;
    return true;
}

static void finishQAppDestruction()
{
    // Preserve an exception from the deallocation path. Updating builtins is
    // bookkeeping and must not replace it with an unraisable error.
    Shiboken::Errors::Stash errorStash;
    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    if (builtins.isNull()
        || PyDict_SetItem(builtins.object(), Shiboken::PyName::qApp(), Py_None) < 0) {
        PyErr_Clear();
    }

    {
        std::lock_guard<std::mutex> lock(qAppMutex);
        baseWrapperGlobals()->qAppLast = Py_None;
        qAppState = QAppState::Ready;
    }
    qAppCondition.notify_all();
    errorStash.restore();
}

static PyObject *_setupNew(PyObject *obSelf, PyTypeObject *subtype);
#endif

PyObject *MakeQAppWrapper(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    PyObject *previous = nullptr;
    if (type != nullptr) {
        if (!beginQAppCreation(&previous)) {
            const char *res_name = previous != nullptr
                ? PepType_GetNameStr(Py_TYPE(previous)) : "<Unknown>";
            const char *type_name = PepType_GetNameStr(type);
            PyErr_Format(PyExc_RuntimeError,
                         "libshiboken: Please destroy the %s singleton before"
                         " creating a new %s instance.", res_name, type_name);
            Py_XDECREF(previous);
            return nullptr;
        }
    } else {
        beginQAppNoneUpdate();
    }

    PyObject *qApp_curr = type != nullptr ? _Sbk_NewVarObject(type) : Py_None;
    if (qApp_curr == nullptr) {
        finishQAppUpdate(nullptr);
        return nullptr;
    }

    // Finish the Python wrapper before publishing it through builtins. With a
    // GIL no thread could observe the old order between MakeQAppWrapper() and
    // SbkQApp_tp_new(); on a free-threaded build it otherwise sees d == nullptr.
    if (type != nullptr) {
        qApp_curr = _setupNew(qApp_curr, type);
        if (qApp_curr == nullptr) {
            finishQAppUpdate(nullptr);
            return nullptr;
        }
        reinterpret_cast<SbkObject *>(qApp_curr)->d->isQAppSingleton = true;
    }

    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    if (builtins.isNull()
        || PyDict_SetItem(builtins.object(), Shiboken::PyName::qApp(), qApp_curr) < 0) {
        finishQAppUpdate(nullptr);
        if (type != nullptr) {
            Shiboken::Errors::Stash errorStash;
            Py_DECREF(qApp_curr);
            errorStash.restore();
        }
        return nullptr;
    }
    builtins.reset(nullptr);

    // This is the keepalive reference used by the historical qApp protocol.
    // Publication of both qAppLast and the Ready state happens only after the
    // object is initialized and the reference is in place.
    Py_INCREF(qApp_curr);
    finishQAppUpdate(qApp_curr);
    return qApp_curr;
#else
    PyObject *&qApp_last = baseWrapperGlobals()->qAppLast;

    // protecting from multiple application instances
    if (type != nullptr && qApp_last != Py_None) {
        const char *res_name = qApp_last != nullptr
            ? PepType_GetNameStr(Py_TYPE(qApp_last)) : "<Unknown>";
        const char *type_name = PepType_GetNameStr(type);
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Please destroy the %s singleton before"
                     " creating a new %s instance.", res_name, type_name);
        return nullptr;
    }

    // monitoring the last application state
    PyObject *qApp_curr = type != nullptr ? _setupNew(_Sbk_NewVarObject(type), type) : Py_None;
    if (qApp_curr == nullptr)
        return nullptr;
    if (type != nullptr)
        reinterpret_cast<SbkObject *>(qApp_curr)->d->isQAppSingleton = true;
    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    if (PyDict_SetItem(builtins.object(), Shiboken::PyName::qApp(), qApp_curr) < 0)
        return nullptr;
    builtins.reset(nullptr);
    qApp_last = qApp_curr;
    // Note: This Py_INCREF would normally be wrong because the qApp
    // object already has a reference from PyObject_GC_New. But this is
    // exactly the needed reference that keeps qApp alive from alone!
    Py_INCREF(qApp_curr);
    // PYSIDE-1470: As a side effect, the interactive "_" variable tends to
    //              create reference cycles. This is disturbing when trying
    //              to remove qApp with del.
    // PYSIDE-1758: Since we moved to an explicit qApp.shutdown() call, we
    //              no longer initialize "_" with Py_None.
    return qApp_curr;
#endif
}

static PyTypeObject *SbkObjectType_tp_new(PyTypeObject *metatype, PyObject *args, PyObject *kwds)
{
    // Check if all bases are new style before calling type.tp_new
    // Was causing gc assert errors in test_bug704.py when
    // this check happened after creating the type object.
    // Argument parsing take from type.tp_new code.

    // PYSIDE-595: Also check if all bases allow inheritance.
    // Before we changed to heap types, it was sufficient to remove the
    // Py_TPFLAGS_BASETYPE flag. That does not work, because PySide does
    // not respect this flag itself!
    PyObject *name{};
    PyObject *pyBases{};
    PyObject *dict{};
    static const char *kwlist[] = { "name", "bases", "dict", nullptr};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO!O!:sbktype", const_cast<char **>(kwlist),
                                     &name,
                                     &PyTuple_Type, &pyBases,
                                     &PyDict_Type, &dict))
        return nullptr;

    for (Py_ssize_t i=0, i_max=PyTuple_Size(pyBases); i < i_max; i++) {
        PyObject *baseType = PyTuple_GetItem(pyBases, i);
        if (PepExt_Type_GetNewSlot(reinterpret_cast<PyTypeObject *>(baseType)) == SbkDummyNew) {
            // PYSIDE-595: A base class does not allow inheritance.
            return reinterpret_cast<PyTypeObject *>(SbkDummyNew(metatype, args, kwds));
        }
    }

    PyTypeObject *newType = PepType_Type_tp_new(metatype, args, kwds);

    if (!newType)
        return nullptr;

    SbkObjectTypePrivate *sotp = PepType_SOTP(newType);
    sotp->dynamicMethodDefs = nullptr;
#ifdef Py_GIL_DISABLED
    sotp->generatedTypeReference = nullptr;
    sotp->nextGeneratedTypeReference = nullptr;
#endif

    const auto bases = Shiboken::getCppBaseClasses(newType);
    if (bases.size() == 1) {
        SbkObjectTypePrivate *parentType = PepType_SOTP(bases.front());
        Shiboken::ObjectType::copyMultipleInheritance(newType, bases.front());
        sotp->type_discovery = parentType->type_discovery;
        sotp->cpp_dtor = parentType->cpp_dtor;
        sotp->is_multicpp = 0;
        sotp->converter = parentType->converter;
    } else {
        sotp->mi_offsets = nullptr;
        sotp->mi_init = nullptr;
        sotp->mi_specialcast = nullptr;
        sotp->type_discovery = nullptr;
        sotp->cpp_dtor = nullptr;
        sotp->is_multicpp = 1;
        sotp->converter = nullptr;
    }
    if (bases.size() == 1) {
        const char *original_name = PepType_SOTP(bases.front())->original_name;
        if (original_name == nullptr)
            original_name = "object";
        sotp->original_name = strdup(original_name);
    }
    else
        sotp->original_name = strdup("object");
    sotp->user_data = nullptr;
    sotp->d_func = nullptr;
    sotp->is_user_type = 1;

    // PYSIDE-1463: Prevent feature switching while in the creation process.
#ifdef Py_GIL_DISABLED
    SbkObjectType_PushFeatureDisable();
#else
    auto saveFeature = initSelectableFeature(nullptr);
#endif
    for (auto it = bases.cbegin(); it != bases.cend(); ++it) {
        const auto hook = PepType_SOTP(*it)->subtype_init;
        if (hook == nullptr
            || std::any_of(bases.cbegin(), it, [hook](PyTypeObject *base) {
                   return PepType_SOTP(base)->subtype_init == hook;
               })) {
            continue;
        }
        // A hook receives the entire new type and its complete bases tuple.
        // Repeating the same hook can overwrite its owned per-type metadata.
        hook(newType, args, kwds);
        if (PyErr_Occurred() != nullptr)
            break;
    }
#ifdef Py_GIL_DISABLED
    SbkObjectType_PopFeatureDisable();
#else
    initSelectableFeature(saveFeature);
#endif
    if (PyErr_Occurred() != nullptr) {
        Py_DECREF(newType);
        return nullptr;
    }
    return newType;
}

static PyObject *_setupNew(PyObject *obSelf, PyTypeObject *subtype)
{
    if (obSelf == nullptr)
        return nullptr;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
#endif
        auto d = std::make_unique<SbkObjectPrivate>();
        auto *sotp = PepType_SOTP(subtype);
        if (sotp != nullptr && sotp->is_user_type) {
            // Take ownership while each bases tuple is still pinned by the
            // walker. Concurrent __bases__ updates cannot free our providers.
            Shiboken::walkThroughBases(subtype, [&d](PyTypeObject *nativeType) {
                d->cppTypes.push_back(nativeType); // allocation before INCREF
                Py_INCREF(nativeType);
                return false;
            });
        }
        if (d->cppTypes.empty()) {
            d->cppTypes.push_back(subtype);
            Py_INCREF(subtype);
        }
        d->cppDestructors.reserve(d->cppTypes.size());
        for (auto *nativeType : d->cppTypes) {
            auto *metadata = PepType_SOTP(nativeType);
            d->cppDestructors.push_back(metadata->cpp_dtor);
#ifdef Py_GIL_DISABLED
            d->deleteInMainThread |= metadata->delete_in_main_thread != 0;
#endif
        }
        d->cptr = new void *[d->cppTypes.size()]{};
        d->cptrCount = int(d->cppTypes.size());
        d->hasOwnership = true;
        d->containsCppWrapper = false;
        d->validCppObject = false;
        d->cppObjectCreated = false;
        d->isQAppSingleton = false;
#ifdef Py_GIL_DISABLED
        d->pendingDestruction = false;
        d->activeCalls = 0;
#endif
        // Publish only after all throwing allocations have succeeded. Failed
        // setup never invokes user __del__, nor the normal wrapper deallocator.
        Py_INCREF(subtype); // paired with the extra type DECREF in deallocation
        auto *self = reinterpret_cast<SbkObject *>(obSelf);
        self->ob_dict = nullptr;
        self->weakreflist = nullptr;
        self->d = d.release();
        PyObject_GC_Track(obSelf);
        return obSelf;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    } catch (const std::bad_alloc &) {
        // GC_NewVar owns one heap-type reference even before GC_Track.
        // No extra wrapper reference was taken before the successful commit.
        PyObject_GC_Del(obSelf);
        if ((subtype->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0)
            Py_DECREF(subtype);
        return PyErr_NoMemory();
    }
#endif
}

PyObject *SbkObject_tp_new(PyTypeObject *subtype, PyObject * /* args */, PyObject * /* kwds */)
{
    PyObject *self = _Sbk_NewVarObject(subtype);
    return _setupNew(self, subtype);
}

PyObject *SbkQApp_tp_new(PyTypeObject *subtype, PyObject *, PyObject *)
{
    return MakeQAppWrapper(subtype);
}

PyObject *SbkDummyNew(PyTypeObject *type, PyObject *, PyObject *)
{
    // PYSIDE-595: Give the same error as type_call does when tp_new is NULL.
    const char regret[] = "¯\\_(ツ)_/¯";
    PyErr_Format(PyExc_TypeError,
                 "cannot create '%.100s' instances %s", type->tp_name, regret);
    return nullptr;
}

// PYSIDE-74: Fallback used in all types now.
PyObject *FallbackRichCompare(PyObject *self, PyObject *other, int op)
{
    // This is a very simple implementation that supplies a simple identity.
    static const char * const opstrings[] = {"<", "<=", "==", "!=", ">", ">="};
    PyObject *res{};

    switch (op) {

    case Py_EQ:
        res = (self == other) ? Py_True : Py_False;
        break;
    case Py_NE:
        res = (self != other) ? Py_True : Py_False;
        break;
    default:
        PyErr_Format(PyExc_TypeError,
                     "'%s' not supported between instances of '%.100s' and '%.100s'",
                     opstrings[op],
                     self->ob_type->tp_name,
                     other->ob_type->tp_name);
        return nullptr;
    }
    Py_INCREF(res);
    return res;
}

bool SbkObjectType_Check(PyTypeObject *type)
{
    auto *meta = SbkObjectType_TypeF();
    auto *obType = reinterpret_cast<PyObject *>(type);
    return Py_TYPE(obType) == meta || PyType_IsSubtype(Py_TYPE(obType), meta);
}

// Global functions from folding.

// The common end.
PyObject *Sbk_ReturnFromPython_None()
{
    if (Shiboken::Errors::occurred() != nullptr) {
        return {};
    }
    Py_RETURN_NONE;
}

PyObject *Sbk_ReturnFromPython_Result(PyObject *pyResult)
{
    if (Shiboken::Errors::occurred() != nullptr || pyResult == nullptr) {
        Py_XDECREF(pyResult);
        return {};
    }
    return pyResult;
}

PyObject *Sbk_ReturnFromPython_Self(PyObject *self)
{
    if (Shiboken::Errors::occurred() != nullptr) {
        return {};
    }
    Py_INCREF(self);
    return self;
}

} //extern "C"

// Determine name of a Python override of a virtual method according to features
// and populate name cache.
static PyObject *overrideMethodName(PyObject *pySelf, const char *methodName,
                                    PyObject **nameCache)
{
    // PYSIDE-1626: Touch the type to initiate switching early.
    auto *obType = Py_TYPE(pySelf);
    SbkObjectType_UpdateFeature(obType);

    const int flag = currentSelectId(obType);
    const int propFlag = isdigit(methodName[0]) ? methodName[0] - '0' : 0;
    const bool is_snake = flag & 0x01;
#ifdef Py_GIL_DISABLED
    (void)nameCache;
    // The process-wide name cache is a plain PyObject* array generated for
    // every virtual method. Do not share it on a free-threaded build; the
    // interned string is cheap to retrieve and this gives the caller a strong
    // reference with no mutable cache involved.
    if (propFlag)
        methodName += 2; // skip the propFlag and ':'
    return Shiboken::String::getSnakeCaseName(methodName, is_snake);
#else
    PyObject *pyMethodName = nameCache[is_snake];  // borrowed
    if (pyMethodName == nullptr) {
        if (propFlag)
            methodName += 2; // skip the propFlag and ':'
        pyMethodName = Shiboken::String::getSnakeCaseName(methodName, is_snake);
        nameCache[is_snake] = pyMethodName;
    }
    return pyMethodName;
#endif
}

// The virtual function call
PyObject *Sbk_GetPyOverride(const void *voidThis, PyTypeObject *typeObject,
                            Shiboken::GilState &gil, const char *funcName,
                            PyObject *&resultCache, PyObject **nameCache)
{
    if (Py_IsInitialized() == 0
#ifndef Py_GIL_DISABLED
        || resultCache == Py_None
#endif
        ) {
        return nullptr; // Bail out, execute C++ call (wrappers may outlive Python).
    }

    auto &bindingManager = Shiboken::BindingManager::instance();
#ifdef Py_GIL_DISABLED
    (void)resultCache;
    // Cheap and without touching a reference count, so it needs no thread
    // state - we may have none yet. It is also the answer for a C++ object
    // never handed out to Python, which keeps that case as cheap as it was.
    // Not free, though: with a type it walks the bucket and asks
    // PyType_IsSubtype, so the positive case looks the map up twice. That is
    // why this is not done on a build with a GIL, where a borrow is safe.
    if (!bindingManager.hasWrapper(voidThis, typeObject))
        return nullptr;

    gil.acquire();

    // A virtual call can arrive from a C++ destructor, so the wrapper may
    // already be dying; an empty result covers that as well as "gone in the
    // meantime". Needs the thread state taken just above, and for the same
    // reason every path below that calls gil.release() has to reset() this
    // first - letting it go is a decref.
    auto wrapperRef = bindingManager.acquireWrapper(voidThis, typeObject);
    if (wrapperRef.isNull()) {
        gil.release();
        return nullptr;
    }
    auto *wrapper = wrapperRef.object();
    auto *pySelf = wrapperRef.pyObject();
#else // Py_GIL_DISABLED
    SbkObject *wrapper = bindingManager.retrieveWrapper(voidThis, typeObject);
    // The refcount can be 0 if the object is dieing and someone called
    // a virtual method from the destructor
    if (wrapper == nullptr)
        return nullptr;
    auto *pySelf = reinterpret_cast<PyObject *>(wrapper);
    if (Py_REFCNT(pySelf) == 0)
        return nullptr;

    gil.acquire();
#endif // Py_GIL_DISABLED
#ifndef Py_GIL_DISABLED
    if (resultCache == Py_None) { // PYSIDE 3246, some other thread may have determined the override
        gil.release();
        return nullptr;
    }

    if (resultCache != nullptr) // recreate the callable from function/self
        return PepExt_Type_CallDescrGet(resultCache, pySelf, nullptr);

    PyObject *pyMethodName = overrideMethodName(pySelf, funcName, nameCache);
#else
    // resultCache and nameCache are generated as plain PyObject* storage and
    // can be reset by another thread when Python overrides change. A pointer
    // cache cannot make the lifetime of a removed function safe, so the
    // free-threaded build deliberately bypasses both caches.
    Shiboken::AutoDecRef pyMethodNameRef(overrideMethodName(pySelf, funcName, nameCache));
    PyObject *pyMethodName = pyMethodNameRef.object();
#endif
    auto *wrapper_dict = SbkObject_GetDict_NoRef(pySelf);

    // Note: This special case was implemented for duck-punching, which happens
    // in the instance dict. It does not work with properties.
    // This is not cached to avoid leaking. FIXME PYSIDE 7: Remove (PYSIDE-2916)?
#ifdef Py_GIL_DISABLED
    PyObject *method = nullptr;
    if (PyDict_GetItemRef(wrapper_dict, pyMethodName, &method) == 1)
        return method;
#else
    if (PyObject *method = PyDict_GetItem(wrapper_dict, pyMethodName)) {
        Py_INCREF(method);
        return method;
    }
#endif

#ifdef Py_GIL_DISABLED
    Shiboken::AutoDecRef pyOverride(Shiboken::BindingManager::getOverride(wrapper, pyMethodName));
    if (pyOverride.isNull()) {
        wrapperRef.reset();
        gil.release();
        return nullptr; // No override, execute C++ call
    }

    if (Shiboken::Errors::occurred() != nullptr) {
        wrapperRef.reset();
        gil.release();
        return nullptr; // Give up.
    }

    return PepExt_Type_CallDescrGet(pyOverride.object(), pySelf, nullptr);
#else
    auto *pyOverride = Shiboken::BindingManager::getOverride(wrapper, pyMethodName);
    if (pyOverride == nullptr) {
        resultCache = Py_None;
        Py_INCREF(resultCache);
        gil.release();
        return nullptr; // No override, execute C++ call
    }

    if (Shiboken::Errors::occurred() != nullptr) {
        // Give up.
        Py_XDECREF(pyOverride);
        resultCache = Py_None;
        Py_INCREF(resultCache);
        gil.release();
        return nullptr; // Give up.
    }

    resultCache = pyOverride;
    // recreate the callable from function/self
    return PepExt_Type_CallDescrGet(resultCache, pySelf, nullptr);
#endif
}

namespace
{

#ifdef Py_GIL_DISABLED
// Invalidate and detach the children of obj. Their C++ instances belong to
// obj's C++ instance and are deleted by it, so no ownership is handed back.
void _detachChildren(SbkObject *obj, bool keepReference)
{
    SBK_ASSERT_STATE_UNLOCKED();
    // One child per round, read inside a transaction: the container is the
    // one setParentLocked() and removeParentLocked() mutate. The loop body
    // must stay outside, because invalidate() and removeParent() take the
    // lock themselves and it is not recursive. The child is pinned for the
    // trip: nothing else keeps it alive once removeParent() has handed the
    // parent reference back.
    while (true) {
        SbkObject *first = nullptr;
        {
            Shiboken::StateLockGuard guard;
            Shiboken::ParentInfo *pInfo = obj->d->parentInfo;
            if (pInfo == nullptr || pInfo->children.empty())
                break;
            first = *pInfo->children.begin();
            Py_INCREF(reinterpret_cast<PyObject *>(first));
        }
        // Mark child as invalid
        Shiboken::Object::invalidate(first);
        Shiboken::Object::removeParent(first, false, keepReference);
        Py_DECREF(reinterpret_cast<PyObject *>(first));
    }
}
#else // Py_GIL_DISABLED
// Invalidate and detach the children of obj. Their C++ instances belong to
// obj's C++ instance and are deleted by it, so no ownership is handed back.
void _detachChildren(SbkObject *obj, bool keepReference)
{
    Shiboken::ParentInfo *pInfo = obj->d->parentInfo;
    if (pInfo) {
        while(!pInfo->children.empty()) {
            SbkObject *first = *pInfo->children.begin();
            // Mark child as invalid
            Shiboken::Object::invalidate(first);
            Shiboken::Object::removeParent(first, false, keepReference);
        }
    }
}
#endif // Py_GIL_DISABLED

void _destroyParentInfo(SbkObject *obj, bool keepReference)
{
    if (obj->d->parentInfo) {
        _detachChildren(obj, keepReference);
        Shiboken::Object::removeParent(obj, false);
    }
}

}

namespace Shiboken
{

// Wrapper metatype and base type ----------------------------------------------------------

void _initMainThreadId(); // helper.cpp

static std::string msgFailedToInitializeType(const char *description)
{
    std::ostringstream stream;
    stream << "libshiboken: Failed to initialize " << description;
    if (PyErr_Occurred() != nullptr) {
        Shiboken::Errors::Stash stash;
        if (auto *str = PyObject_Str(stash.getException()))
            stream << ": " << Shiboken::String::toCString(str);
    }
    stream << '.';
    return stream.str();
}

namespace Conversions { void init(); }

#ifdef Py_GIL_DISABLED
enum class ShibokenInitState
{
    NotStarted,
    Running,
    Done
};

static std::mutex shibokenInitMutex;
static std::condition_variable shibokenInitCondition;
static ShibokenInitState shibokenInitState = ShibokenInitState::NotStarted;
static thread_local bool shibokenInitOwner = false;

static bool beginShibokenInit()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(shibokenInitMutex);
        if (shibokenInitState == ShibokenInitState::Done)
            return false;
        if (shibokenInitState == ShibokenInitState::NotStarted) {
            shibokenInitState = ShibokenInitState::Running;
            shibokenInitOwner = true;
            return true;
        }
        if (shibokenInitOwner)
            return false;

        // Do not block a free-threaded stop-the-world pause while waiting for
        // the thread that owns initialization to finish Python C API work.
        lock.unlock();
        ThreadStateSaver saver;
        saver.save();
        lock.lock();
        shibokenInitCondition.wait(lock, [] {
            return shibokenInitState != ShibokenInitState::Running;
        });
        lock.unlock();
        saver.restore();
    }
}

static void finishShibokenInit()
{
    {
        std::lock_guard<std::mutex> lock(shibokenInitMutex);
        shibokenInitState = ShibokenInitState::Done;
        shibokenInitOwner = false;
    }
    shibokenInitCondition.notify_all();
}
#endif

void init()
{
#ifdef Py_GIL_DISABLED
    if (!beginShibokenInit())
        return;
#else
    static bool shibokenAlreadInitialised = false;
    if (shibokenAlreadInitialised) // Leave guard in place until fully ported to multi phase init
        return;
#endif

    _initMainThreadId();

    Conversions::init();

    //Init private data
    Pep384_Init();

    auto *type = SbkObjectType_TypeF();
    if (type == nullptr || PyType_Ready(type) < 0)
        Py_FatalError(msgFailedToInitializeType("Shiboken.BaseWrapperType metatype").c_str());

    type = SbkObject_TypeF();
    if (type == nullptr || PyType_Ready(type) < 0)
        Py_FatalError(msgFailedToInitializeType("Shiboken.BaseWrapper type").c_str());

    VoidPtr::init();

#ifdef Py_GIL_DISABLED
    finishShibokenInit();
#else
    shibokenAlreadInitialised = true;
#endif
}

// PYSIDE-1415: Publish Shiboken objects.
// PYSIDE-1735: Initialize the whole Shiboken startup.
void initShibokenSupport(PyObject *module)
{
    auto *type = SbkObject_TypeF();
    auto *obType = reinterpret_cast<PyObject *>(type);
    Py_INCREF(obType);
    PepModule_AddType(module, type);

    // PYSIDE-1735: When the initialization was moved into Shiboken import, this
    //              Py_INCREF became necessary. No idea why.
    Py_INCREF(module);
    init_shibokensupport_module();

    if (InitSignatureStrings(type, SbkObject_SignatureStrings) < 0)
        Py_FatalError("libshiboken: Error in initShibokenSupport");
}

// setErrorAboutWrongArguments now gets overload info from the signature module.
// Info can be nullptr and contains extra info.
void setErrorAboutWrongArguments(PyObject *args, const char *funcName, PyObject *info,
                                 const char *className)
{
    if (className != nullptr) {
        std::string text = std::string(className);
        text += '.';
        text += funcName;
        SetError_Argument(args, text.c_str(), info);
        return;
    }
    SetError_Argument(args, funcName, info);
}

PyObject *returnWrongArguments(PyObject *args, const char *memberName, PyObject *info,
                               const Module::TypeInitStruct &initStruct)
{
    setErrorAboutWrongArguments(args, memberName, info, initStruct.fullName);
    return {};
}

PyObject *returnWrongArguments(PyObject *args, const char *memberName,
                               const Module::TypeInitStruct &initStruct)
{
    setErrorAboutWrongArguments(args, memberName, nullptr, initStruct.fullName);
    return {};
}

PyObject *returnWrongArguments(PyObject *args, const char *globalFuncName, PyObject *info)
{
    setErrorAboutWrongArguments(args, globalFuncName, info);
    return {};
}

PyObject *returnWrongArguments(PyObject *args, const char *globalFuncName)
{
    setErrorAboutWrongArguments(args, globalFuncName, nullptr);
    return {};
}

int returnWrongArguments_Zero(PyObject *args, const char *memberName, PyObject *info,
                              const Module::TypeInitStruct &initStruct)
{
    setErrorAboutWrongArguments(args, memberName, info, initStruct.fullName);
    return 0;
}

int returnWrongArguments_Zero(PyObject *args, const char *globalFuncName, PyObject *info)
{
    setErrorAboutWrongArguments(args, globalFuncName, info);
    return 0;
}

int returnWrongArguments_MinusOne(PyObject *args, const char *memberName, PyObject *info,
                                  const Module::TypeInitStruct &initStruct)
{
    setErrorAboutWrongArguments(args, memberName, info, initStruct.fullName);
    return -1;
}

int returnWrongArguments_MinusOne(PyObject *args, const char *memberName,
                                  const Module::TypeInitStruct &initStruct)
{
    setErrorAboutWrongArguments(args, memberName, nullptr, initStruct.fullName);
    return -1;
}

int returnWrongArguments_MinusOne(PyObject *args, const char *globalFuncName, PyObject *info)
{
    setErrorAboutWrongArguments(args, globalFuncName, info);
    return -1;
}

PyObject *returnFromRichCompare(PyObject *result)
{
    if (result && !PyErr_Occurred())
        return result;
    Shiboken::Errors::setOperatorNotImplemented();
    return {};
}

PyObject *checkInvalidArgumentCount(Py_ssize_t numArgs, Py_ssize_t minArgs, Py_ssize_t maxArgs)
{
    PyObject *result = nullptr;
    // for seterror_argument(), signature/errorhandler.py
#ifdef Py_GIL_DISABLED
    if (numArgs > maxArgs)
        return Shiboken::String::createStaticString(">");
    if (numArgs < minArgs)
        return Shiboken::String::createStaticString(numArgs > 0 ? "<" : "0");
#else
    if (numArgs > maxArgs) {
        static PyObject *const tooMany = Shiboken::String::createStaticString(">");
        result = tooMany;
        Py_INCREF(result);
    } else if (numArgs < minArgs) {
        static PyObject *const tooFew = Shiboken::String::createStaticString("<");
        static PyObject *const noArgs = Shiboken::String::createStaticString("0");
        result = numArgs > 0 ? tooFew : noArgs;
        Py_INCREF(result);
    }
#endif
    return result;
}

std::vector<SbkObject *> splitPyObject(PyObject *pyObj)
{
    std::vector<SbkObject *> result;
    if (PySequence_Check(pyObj)) {
        AutoDecRef lst(PySequence_Fast(pyObj, "Invalid keep reference object."));
        if (!lst.isNull()) {
            for (Py_ssize_t i = 0, i_max = PySequence_Size(lst.object()); i < i_max; ++i) {
                Shiboken::AutoDecRef item(PySequence_GetItem(lst.object(), i));
                if (Object::checkType(item))
                    result.push_back(reinterpret_cast<SbkObject *>(item.object()));
            }
        }
    } else {
        result.push_back(reinterpret_cast<SbkObject *>(pyObj));
    }
    return result;
}

template <class Iterator>
inline void decRefPyObjectList(Iterator i1, Iterator i2)
{
    for (; i1 != i2; ++i1)
        Py_DECREF(i1->second);
}

namespace ObjectType
{

PyMethodDef *createOwnedMethodDef(PyTypeObject *type, const PyMethodDef *method,
                                 const char *name, int flags)
{
    if (type == nullptr || !SbkObjectType_Check(type) || method == nullptr || name == nullptr) {
        PyErr_SetString(PyExc_TypeError, "A Shiboken type and method definition are required");
        return nullptr;
    }
    void *storage = PyMem_RawMalloc(sizeof(SbkDynamicMethodDef));
    if (storage == nullptr) {
        PyErr_NoMemory();
        return nullptr;
    }
    auto *definition = ::new (storage) SbkDynamicMethodDef{};
    const auto length = std::strlen(name);
    auto *ownedName = static_cast<char *>(PyMem_RawMalloc(length + 1));
    if (ownedName == nullptr) {
        PyMem_RawFree(definition);
        PyErr_NoMemory();
        return nullptr;
    }
    std::memcpy(ownedName, name, length + 1);
    definition->method = *method;
    definition->method.ml_name = ownedName;
    definition->method.ml_flags = flags;
    definition->name = ownedName;
    auto *data = PepType_SOTP(type);
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(reinterpret_cast<PyObject *>(type));
#endif
    definition->next = data->dynamicMethodDefs;
    data->dynamicMethodDefs = definition;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return &definition->method;
}


#ifdef Py_GIL_DISABLED
static SbkObjectTypePrivate *generatedTypeReferences;
static std::mutex &generatedTypeReferenceMutex()
{
    static std::mutex mutex;
    return mutex;
}

void registerGeneratedTypeReference(PyTypeObject *type)
{
    // A finalizer may import a module after the late cleanup callback. Its
    // normal module owner retains the new type; do not add a new lifetime pin.
    if (!Py_IsInitialized()) {
        Py_DECREF(type);
        return;
    }
    auto *sotp = PepType_SOTP(type);
    std::lock_guard<std::mutex> guard(generatedTypeReferenceMutex());
    assert(sotp->generatedTypeReference == nullptr);
    sotp->generatedTypeReference = type;
    sotp->nextGeneratedTypeReference = generatedTypeReferences;
    generatedTypeReferences = sotp;
}

void releaseGeneratedTypeReferences()
{
    for (;;) {
        PyTypeObject *type = nullptr;
        {
            std::lock_guard<std::mutex> guard(generatedTypeReferenceMutex());
            auto *sotp = generatedTypeReferences;
            if (sotp == nullptr)
                break;
            generatedTypeReferences = sotp->nextGeneratedTypeReference;
            type = sotp->generatedTypeReference;
            sotp->generatedTypeReference = nullptr;
            sotp->nextGeneratedTypeReference = nullptr;
        }
        // The type may be destroyed or run Python callbacks here.
        Py_DECREF(type);
    }
}
#endif

bool checkType(PyTypeObject *type)
{
    return PyType_IsSubtype(type, SbkObject_TypeF()) != 0;
}

bool isUserType(PyTypeObject *type)
{
    return checkType(type) && PepType_SOTP(type)->is_user_type;
}

bool canCallConstructor(PyTypeObject *myType, PyTypeObject *ctorType)
{
    auto findBasePred = [ctorType](PyTypeObject *type) { return type == ctorType; };
    if (!walkThroughBases(myType, findBasePred)) {
        PyErr_Format(PyExc_TypeError,
                     "libshiboken: %s isn't a direct base class of %s", ctorType->tp_name, myType->tp_name);
        return false;
    }
    return true;
}

bool hasCast(PyTypeObject *type)
{
    auto *sotp = PepType_SOTP(type);
    // Each native slot can have a different C++ hierarchy and cast function.
    // Route multi-slot instances through cast() even if one slot needs no cast.
    if (sotp->is_multicpp)
        return true;
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    return sotp->mi_specialcast != nullptr;
}

void *cast(PyTypeObject *sourceType, SbkObject *obj, PyTypeObject *pyTargetType)
{
    pyTargetType = nativeTargetType(pyTargetType);
    const int index = objectTypeIndex(obj, pyTargetType);
    if (index < 0)
        return nullptr;
    sourceType = obj->d->cppTypes[size_t(index)];
    auto *sotp = PepType_SOTP(sourceType);
    SpecialCastFunction specialCast;
    {
#ifdef Py_GIL_DISABLED
        std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
        specialCast = sotp->mi_specialcast;
    }
    // Do not run generated C++ code with the type metadata lock held.
    void *cptr = Object::cppPointer(obj, pyTargetType);
    return cptr != nullptr && specialCast != nullptr
        ? specialCast(cptr, pyTargetType) : cptr;
}

void setCastFunction(PyTypeObject *type, SpecialCastFunction func)
{
    auto *sotp = PepType_SOTP(type);
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    sotp->mi_specialcast = func;
}

void setOriginalName(PyTypeObject *type, const char *name)
{
    auto *sotp = PepType_SOTP(type);
    if (sotp->original_name)
        free(sotp->original_name);
    sotp->original_name = strdup(name);
}

const char *getOriginalName(PyTypeObject *type)
{
    return PepType_SOTP(type)->original_name;
}

void setTypeDiscoveryFunctionV2(PyTypeObject *type, TypeDiscoveryFuncV2 func)
{
    PepType_SOTP(type)->type_discovery = func;
}

void copyMultipleInheritance(PyTypeObject *type, PyTypeObject *other)
{
    auto *sotp_type = PepType_SOTP(type);
    // A Python class with several C++ allocations has no single MI triple.
    // Its immutable native base types remain the metadata providers instead.
    if (sotp_type->is_multicpp)
        return;
    auto *sotp_other = PepType_SOTP(other);
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    // One coherent publication for all three MI fields.
    sotp_type->mi_init = sotp_other->mi_init;
    sotp_type->mi_offsets = sotp_other->mi_offsets;
    sotp_type->mi_specialcast = sotp_other->mi_specialcast;
}

void setMultipleInheritanceFunction(PyTypeObject *type, MultipleInheritanceInitFunction function)
{
    auto *sotp = PepType_SOTP(type);
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    sotp->mi_init = function;
}

MultipleInheritanceInitFunction getMultipleInheritanceFunction(PyTypeObject *type)
{
    auto *sotp = PepType_SOTP(type);
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    return sotp->mi_init;
}

// Also used by BindingManager with its wrapper-map lock held.  This
// dedicated lock must NEVER acquire the wrapper-map or state locks.
// mi_init is generated offset arithmetic only (no Python / Qt callbacks);
// the MA29 emitter fix separately makes its first invocation thread-safe.
const int *getMultipleInheritanceOffsets(PyTypeObject *type, const void *cptr)
{
    auto *sotp = PepType_SOTP(type);
    if (sotp == nullptr)
        return nullptr;
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> guard(miTypeMetadataMutex);
#endif
    if (cptr != nullptr && sotp->mi_init != nullptr && sotp->mi_offsets == nullptr)
        sotp->mi_offsets = sotp->mi_init(cptr);
    return sotp->mi_offsets;
}

void setDestructorFunction(PyTypeObject *type, ObjectDestructor func)
{
    PepType_SOTP(type)->cpp_dtor = func;
}

PyTypeObject *
introduceWrapperType(PyObject *enclosingObject,
                     const char *typeName,
                     const char *originalName,
                     PyType_Spec *typeSpec,
                     ObjectDestructor cppObjDtor,
                     PyObject *bases,
                     unsigned wrapperFlags)
{
    assert(PySequence_Size(bases) > 0);
    AutoDecRef primaryBase(PySequence_GetItem(bases, 0));
    if (primaryBase.isNull())
        return nullptr;
    typeSpec->slots[0].pfunc = primaryBase.object();

    auto *type = SbkType_FromSpecBasesMeta(typeSpec, bases, SbkObjectType_TypeF());
    if (type == nullptr) {
        // Reporting the failure beats crashing in PepType_SOTP() below: a
        // failure here means the type spec or one of its bases is unusable,
        // and the traceback names the type instead of a null dereference.
        PyErr_Format(PyExc_SystemError, "libshiboken: Cannot create type '%s'", typeName);
        return nullptr;
    }

    AutoDecRef ownedType(reinterpret_cast<PyObject *>(type));
    auto *sotp = PepType_SOTP(type);
    sotp->dynamicMethodDefs = nullptr;
    if (wrapperFlags & DeleteInMainThread)
        sotp->delete_in_main_thread = 1;
    sotp->type_behaviour = (wrapperFlags & Value) != 0
                           ? BEHAVIOUR_VALUETYPE : BEHAVIOUR_OBJECTTYPE;

    setOriginalName(type, originalName);
    setDestructorFunction(type, cppObjDtor);
    auto *ob_type = reinterpret_cast<PyObject *>(type);

    if (wrapperFlags & InternalWrapper) {
        // Type wraps another wrapper class and isn't part of any module.  This is used to extend
        // Qt types with additional functionality, but from within PySide itself.  In order for
        // the re-wrapped class to call the autogenerated __init__ method, is_user_type must be set
        // to 1.  Otherwise the Shiboken::ObjectType::canCallConstructor test will fail.
        // This is currently only used by QtRemoteObjects to create types dynamically.
        sotp->is_user_type = 1;
        return reinterpret_cast<PyTypeObject *>(ownedType.release());
    }

    if (wrapperFlags & InnerClass) {
        // PYSIDE-2230: Instead of tp_dict, use the enclosing type.
        //              This stays interface compatible.
        if (PyType_Check(enclosingObject)) {
            auto *enclosingType = reinterpret_cast<PyTypeObject *>(enclosingObject);
            AutoDecRef tpDict(PepType_GetDict(enclosingType));
            if (PyDict_SetItemString(tpDict, typeName, ob_type) != 0)
                return nullptr;
            PyType_Modified(enclosingType);
            SbkObjectType_NotifyFeatureUpdate(enclosingType);
            return reinterpret_cast<PyTypeObject *>(ownedType.release());
        }
        if (PyDict_Check(enclosingObject)) {
            if (PyDict_SetItemString(enclosingObject, typeName, ob_type) != 0)
                return nullptr;
            return reinterpret_cast<PyTypeObject *>(ownedType.release());
        }
    }

    // PyModule_AddObject steals type's reference.
    Py_INCREF(ob_type);
    if (PepModule_AddType(enclosingObject, type) != 0) {
        std::cerr << "Warning: " << __FUNCTION__ << " returns nullptr for "
            << typeName << '/' << originalName << " due to PyModule_AddObject(enclosingObject="
            << enclosingObject << ", ob_type=" << ob_type << ") failing\n";
        return nullptr;
    }
    return reinterpret_cast<PyTypeObject *>(ownedType.release());
}

void setSubTypeInitHook(PyTypeObject *type, SubTypeInitHook func)
{
    assert(SbkObjectType_Check(type));
    PepType_SOTP(type)->subtype_init = func;
}

void *getTypeUserData(PyTypeObject *type)
{
    assert(SbkObjectType_Check(type));
    return PepType_SOTP(type)->user_data;
}

void setTypeUserData(PyTypeObject *type, void *userData, DeleteUserDataFunc d_func)
{
    assert(SbkObjectType_Check(type));
    auto *sotp = PepType_SOTP(type);
    sotp->user_data = userData;
    sotp->d_func = d_func;
}

// Try to find the exact type of cptr.
PyTypeObject *typeForTypeName(const char *typeName)
{
    PyTypeObject *result{};
    if (typeName) {
        if (PyTypeObject *pyType = Shiboken::Conversions::getPythonTypeObject(typeName))
            result = pyType;
    }
    return result;
}

bool hasSpecialCastFunction(PyTypeObject *sbkType)
{
    const auto *d = PepType_SOTP(sbkType);
    return d != nullptr && hasCast(sbkType);
}

// Find whether base is a direct single line base class of type
// (no multiple inheritance), that is, a C++ pointer cast can safely be done.
static bool isDirectAncestor(PyTypeObject *type, PyTypeObject *base)
{
    if (type == base)
        return true;
    if (PyTuple_Size(type->tp_bases) == 0)
        return false;
    auto *sbkObjectType = SbkObject_TypeF();
    auto *firstBase = reinterpret_cast<PyTypeObject *>(PyTuple_GetItem(type->tp_bases, 0));
    return firstBase != sbkObjectType
           && PyType_IsSubtype(type, sbkObjectType) != 0
           && isDirectAncestor(firstBase, base);
}

bool canDowncastTo(PyTypeObject *baseType, PyTypeObject *targetType)
{
    return isDirectAncestor(targetType, baseType);
}

} // namespace ObjectType


namespace Object
{

#ifndef Py_GIL_DISABLED
static void recursive_invalidate(SbkObject *self, std::set<SbkObject *> &seen);
#endif

bool checkType(PyObject *pyObj)
{
    return ObjectType::checkType(Py_TYPE(pyObj));
}

bool isUserType(PyObject *pyObj)
{
    return ObjectType::isUserType(Py_TYPE(pyObj));
}

Py_hash_t hash(PyObject *pyObj)
{
    assert(Shiboken::Object::checkType(pyObj));
    return reinterpret_cast<Py_hash_t>(pyObj);
}

static void setSequenceOwnership(PyObject *pyObj, bool owner)
{
    if (!pyObj)
        return;

    const Py_ssize_t length = PyList_Check(pyObj) != 0 || PyTuple_Check(pyObj)  != 0
                                  ? PySequence_Size(pyObj) : -1;
    if (length >= 0) {
        if (length > 0) {
            const auto objs = splitPyObject(pyObj);
            if (owner) {
                for (SbkObject *o : objs)
                    getOwnership(o);
            } else {
                for (SbkObject *o : objs)
                    releaseOwnership(o);
            }
        }
    } else if (Object::checkType(pyObj)) {
        if (owner)
            getOwnership(reinterpret_cast<SbkObject *>(pyObj));
        else
            releaseOwnership(reinterpret_cast<SbkObject *>(pyObj));
    }
}

// One of the writers of validCppObject that takes no state lock, like
// setCppPointer(): the callers are generated constructors, and there no other
// thread can reach the object yet. The flag is a RelaxedFlag, so the write
// itself is defined; what would need the lock is a decision made from it,
// and none is made here.
void setValidCpp(SbkObject *pyObj, bool value)
{
    pyObj->d->validCppObject = value;
}

void setHasCppWrapper(SbkObject *pyObj, bool value)
{
    pyObj->d->containsCppWrapper = value;
}

bool hasCppWrapper(SbkObject *pyObj)
{
    return pyObj->d->containsCppWrapper;
}

bool wasCreatedByPython(SbkObject *pyObj)
{
    return pyObj->d->cppObjectCreated;
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void callCppDestructors(SbkObject *pyObj)
{
    auto *priv = pyObj->d;
    // Idempotent under the guard: a concurrent Shiboken.delete() of the same
    // object may already have claimed the C++ pointer and cleared cptr.
    if (priv->cptr == nullptr || !priv->validCppObject)
        return;
    if (priv->isQAppSingleton && DestroyQApplication) {
        // PYSIDE-1470: Allow to destroy the application from Shiboken.
        DestroyQApplication();
        return;
    }
    const bool isMultiCpp = priv->cptrCount > 1;
    const auto destructor = priv->cppDestructors.front();

    // All bookkeeping first, while the guard cannot have been suspended; the
    // C++ destructors run last, on a snapshot of the pointers. Running the
    // destructor first is unsafe: ThreadStateSaver detaches the thread, which
    // suspends the critical section, and a concurrent delete of the same
    // object then passes the check above and destroys the C++ object a second
    // time. Same order as the dealloc path in SbkDeallocWrapperCommon().
    DestructorEntries entries;
    void *cptr0 = nullptr;
    if (isMultiCpp)
        entries = getDestructorEntries(pyObj);
    else
        cptr0 = priv->cptr[0];

    if (priv->containsCppWrapper)
        BindingManager::instance().releaseWrapper(pyObj);

    /* invalidate needs to be called before deleting pointer array because
       it needs to delete entries for them from the BindingManager hash table;
       also release wrapper explicitly if object contains C++ wrapper because
       invalidate doesn't */
    invalidate(pyObj);

    delete[] priv->cptr;
    priv->cptr = nullptr;
    priv->validCppObject = false;

    if (isMultiCpp) {
        callDestructor(entries);
    } else {
        Shiboken::ThreadStateSaver threadSaver;
        threadSaver.save();
        destructor(cptr0);
    }
}
#endif // !Py_GIL_DISABLED

bool hasOwnership(SbkObject *pyObj)
{
    return pyObj->d->hasOwnership;
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void getOwnership(SbkObject *sbkObj)
{
    // skip if already have the ownership
    if (sbkObj->d->hasOwnership)
        return;

    // skip if this object has parent
    if (sbkObj->d->parentInfo && sbkObj->d->parentInfo->parent)
        return;

    // Get back the ownership
    sbkObj->d->hasOwnership = true;

    if (sbkObj->d->containsCppWrapper)
        Py_DECREF(reinterpret_cast<PyObject *>(sbkObj)); // Remove extra ref
    else
        makeValid(sbkObj); // Make the object valid again
}
#endif // !Py_GIL_DISABLED

void getOwnership(PyObject *pyObj)
{
    if (pyObj)
        setSequenceOwnership(pyObj, true);
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void releaseOwnership(SbkObject *sbkObj)
{
    // skip if the ownership have already moved to c++
    auto *ob  = reinterpret_cast<PyObject *>(sbkObj);
    auto *selfType = Py_TYPE(ob);
    if (!sbkObj->d->hasOwnership || Shiboken::Conversions::pythonTypeIsValueType(PepType_SOTP(selfType)->converter))
        return;

    // remove object ownership
    sbkObj->d->hasOwnership = false;

    // If We have control over object life
    if (sbkObj->d->containsCppWrapper)
        Py_INCREF(ob); // keep the python object alive until the wrapper destructor call
    else
        invalidate(sbkObj); // If I do not know when this object will die We need to invalidate this to avoid use after
}
#endif // !Py_GIL_DISABLED

void releaseOwnership(PyObject *pyObj)
{
    setSequenceOwnership(pyObj, false);
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
/* Needed forward declarations */
static void recursive_invalidate(PyObject *pyobj, std::set<SbkObject *> &seen);

void invalidate(PyObject *pyobj)
{
    std::set<SbkObject *> seen;
    recursive_invalidate(pyobj, seen);
}

void invalidate(SbkObject *self)
{
    std::set<SbkObject *> seen;
    recursive_invalidate(self, seen);
}

static void recursive_invalidate(PyObject *pyobj, std::set<SbkObject *> &seen)
{
    const auto objs = splitPyObject(pyobj);
    for (SbkObject *o : objs)
        recursive_invalidate(o, seen);
}

static void recursive_invalidate(SbkObject *self, std::set<SbkObject *> &seen)
{
    // Skip if this object not is a valid object or if it's already been seen
    if (!self || reinterpret_cast<PyObject *>(self) == Py_None || seen.find(self) != seen.end())
        return;
    seen.insert(self);

    if (!self->d->containsCppWrapper) {
        self->d->validCppObject = false; // Mark object as invalid only if this is not a wrapper class
        BindingManager::instance().releaseWrapper(self);
    }

    // If it is a parent invalidate all children.
    if (self->d->parentInfo) {
        // Create a copy because this list can be changed during the process
        ChildrenList copy = self->d->parentInfo->children;

        for (SbkObject *child : copy) {
            // invalidate the child
            recursive_invalidate(child, seen);

            // if the parent not is a wrapper class, then remove children from him, because We do not know when this object will be destroyed
            if (!self->d->validCppObject)
                removeParent(child, true, true);
        }
    }

    // If has ref to other objects invalidate all
    if (auto *rInfo = self->d->referredObjects) {
        for (const auto &p : *rInfo) {
            if (rInfo->invalidates(p.first))
                recursive_invalidate(p.second, seen);
        }
    }
}

void makeValid(SbkObject *self)
{
    // Skip if this object not is a valid object
    if (!self || reinterpret_cast<PyObject *>(self) == Py_None || self->d->validCppObject)
        return;

    // Mark object as invalid only if this is not a wrapper class
    self->d->validCppObject = true;

    // If it is a parent make  all children valid
    if (self->d->parentInfo) {
        for (SbkObject *child : self->d->parentInfo->children)
            makeValid(child);
    }

    // If has ref to other objects make all valid again
    if (self->d->referredObjects) {
        const RefCountMap &refCountMap = *(self->d->referredObjects);
        for (const auto &p : refCountMap) {
            if (refCountMap.invalidates(p.first) && Shiboken::Object::checkType(p.second))
                makeValid(reinterpret_cast<SbkObject *>(p.second));
        }
    }
}
#endif // !Py_GIL_DISABLED

void *cppPointer(SbkObject *pyObj, PyTypeObject *desiredType)
{
    const int idx = objectTypeIndex(pyObj, desiredType);
    if (idx < 0)
        return nullptr;
#ifdef Py_GIL_DISABLED
    StateLockGuard guard;
#endif
    auto *d = pyObj->d;
    return d->cptr != nullptr && idx < d->cptrCount ? d->cptr[idx] : nullptr;
}

PyTypeObject *cppTypeForPointer(SbkObject *pyObj, const void *pointer)
{
    // Snapshot is immutable. No Python calls or map lock inside this section.
#ifdef Py_GIL_DISABLED
    StateLockGuard guard;
#endif
    auto *d = pyObj->d;
    if (pointer != nullptr && d->cptr != nullptr) {
        for (int i = 0; i < d->cptrCount; ++i) {
            if (d->cptr[i] == pointer)
                return d->cppTypes[size_t(i)];
        }
    }
    return nullptr;
}

std::vector<void *> cppPointers(SbkObject *pyObj)
{
    const int n = pyObj->d->cptrCount;
    std::vector<void *> ptrs(n, nullptr);
#ifdef Py_GIL_DISABLED
    StateLockGuard guard;
#endif
    if (pyObj->d->cptr != nullptr) {
        for (int i = 0; i < n; ++i)
            ptrs[i] = pyObj->d->cptr[i];
    }
    return ptrs;
}


// Constructor admission must precede all generated native/Python side effects.
ConstructorGuard::ConstructorGuard(SbkObject *self, PyTypeObject *desiredType)
{
    auto *type = Shiboken::pyType(self);
    const int index = objectTypeIndex(self, desiredType);
    const int count = self->d->cptrCount;
    const auto thread = PyThread_get_thread_ident();
    bool accepted = false;
    if (index >= 0 && index < count) {
#ifdef Py_GIL_DISABLED
        StateLockGuard guard;
#endif
        auto *d = self->d;
        const bool busy = !d->constructionSlots.empty()
            && (d->constructionThread != thread
                || std::find(d->constructionSlots.begin(), d->constructionSlots.end(), index)
                    != d->constructionSlots.end());
        bool deleted = d->cptr == nullptr;
#ifdef Py_GIL_DISABLED
        deleted = deleted || d->pendingDestruction;
#endif
        if (!deleted && !busy && index < d->cptrCount && d->cptr[index] == nullptr) {
            d->constructionSlots.push_back(index);
            d->constructionThread = thread;
            m_count = d->cptrCount;
            m_oldCppObjectCreated = d->cppObjectCreated;
            m_oldHasCppWrapper = d->containsCppWrapper;
            m_oldOwnership = d->hasOwnership;
#ifdef Py_GIL_DISABLED
            ++d->activeCalls;
#endif
            accepted = true;
        }
    }
    if (accepted) {
        m_self = self;
        m_index = index;
    } else {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Cannot initialize %s in %s: native slot is initialized, busy, deleted, or absent",
                     desiredType->tp_name, type->tp_name);
    }
}

#ifdef Py_GIL_DISABLED
static void releaseCallLease(SbkObject *self);
#endif

ConstructorGuard::~ConstructorGuard()
{
    // A failed native slot must not destroy previously committed MI slots.
    // Remove only aliases added by this attempt BEFORE calling its destructor,
    // so a generated wrapper destructor cannot rediscover the surviving self.
    if (m_self != nullptr && m_cptr != nullptr && !m_finished) {
        Shiboken::Errors::Stash error;
        BindingManager::instance().unregisterAliases(m_self, m_newAliases);
        bool detached = false;
        {
#ifdef Py_GIL_DISABLED
            StateLockGuard guard;
#endif
            auto *d = m_self->d;
            if (d->cptr != nullptr && d->cptr[m_index] == m_cptr) {
                d->cptr[m_index] = nullptr;
                detached = true;
                bool hasRemaining = false;
                for (int i = 0; i < m_count; ++i)
                    hasRemaining |= d->cptr[i] != nullptr;
                d->validCppObject = hasRemaining;
                d->cppObjectCreated = hasRemaining || m_oldCppObjectCreated;
                d->containsCppWrapper = m_oldHasCppWrapper;
                d->hasOwnership = m_oldOwnership;
            }
        }
        if (detached && m_cleanup != nullptr)
            m_cleanup(m_cptr);
    }
    if (m_self != nullptr) {
        {
#ifdef Py_GIL_DISABLED
            StateLockGuard guard;
#endif
            auto *d = m_self->d;
            const auto it = std::find(d->constructionSlots.begin(), d->constructionSlots.end(), m_index);
            assert(it != d->constructionSlots.end());
            d->constructionSlots.erase(it);
            if (d->constructionSlots.empty())
                d->constructionThread = 0;
        }
#ifdef Py_GIL_DISABLED
        releaseCallLease(m_self);
#endif
    }
    Py_XDECREF(m_parent);
}

void ConstructorGuard::setPendingParent(PyObject *parent)
{
    Py_XINCREF(parent);
    Py_XDECREF(m_parent);
    m_parent = parent;
}

bool ConstructorGuard::registerWrapper(void *cptr, ObjectDestructor cleanup)
{
    m_cptr = cptr;
    m_cleanup = cleanup;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
#endif
        BindingManager::instance().registerWrapperWithAliases(m_self, cptr, &m_newAliases);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    } catch (const std::bad_alloc &) {
        PyErr_NoMemory();
        return false;
    }
#endif
    return true;
}

bool ConstructorGuard::finish()
{
    if (m_parent != nullptr)
        setParent(m_parent, reinterpret_cast<PyObject *>(m_self));
    if (PyErr_Occurred() != nullptr)
        return false;
    m_finished = true;
    return true;
}

bool setCppPointer(SbkObject *sbkObj, PyTypeObject *desiredType, void *cptr)
{
    auto *type = Shiboken::pyType(sbkObj);
    const int idx = objectTypeIndex(sbkObj, desiredType);
    const int count = sbkObj->d->cptrCount;
    const auto thread = PyThread_get_thread_ident();
    bool accepted = false;
    if (cptr != nullptr && idx >= 0 && idx < count) {
#ifdef Py_GIL_DISABLED
        StateLockGuard guard;
#endif
        auto *d = sbkObj->d;
        const bool reservedByOther = !d->constructionSlots.empty()
            && (d->constructionThread != thread
                || std::find(d->constructionSlots.begin(), d->constructionSlots.end(), idx)
                    == d->constructionSlots.end());
        bool deleted = d->cptr == nullptr;
#ifdef Py_GIL_DISABLED
        deleted = deleted || d->pendingDestruction;
#endif
        if (!deleted && !reservedByOther && idx < d->cptrCount && d->cptr[idx] == nullptr) {
            d->cptr[idx] = cptr;
            d->cppObjectCreated = true;
            accepted = true;
        }
    }
    if (!accepted) {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Cannot commit %s in %s: native slot is initialized, busy, deleted, or absent",
                     desiredType->tp_name, type->tp_name);
    }
    return accepted;
}

bool isValid(PyObject *pyObj)
{
    if (pyObj == nullptr || pyObj == Py_None || PyType_Check(pyObj) != 0)
        return true;

    PyTypeObject *type = Py_TYPE(pyObj);
    if (Py_TYPE(reinterpret_cast<PyObject *>(type)) != SbkObjectType_TypeF())
        return true;

    auto *priv = reinterpret_cast<SbkObject *>(pyObj)->d;

    if (!priv->cppObjectCreated && isUserType(pyObj)) {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: '__init__' method of object's base class (%s) not called.",
                     Py_TYPE(pyObj)->tp_name);
        return false;
    }

    if (!priv->validCppObject) {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Internal C++ object (%s) already deleted.",
                     Py_TYPE(pyObj)->tp_name);
        return false;
    }

#ifdef Py_GIL_DISABLED
    // An object whose destruction is only waiting for a lease to end is gone
    // as far as callers are concerned: acquireCallLeaseLocked() refuses it,
    // so every method call on it raises. Answering "valid" here would let
    // Shiboken.isValid() disagree with what the next call does. A wrapper
    // subclass keeps validCppObject set through that, which is why the flag
    // above does not catch it.
    if (priv->pendingDestruction) {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Internal C++ object (%s) already deleted.",
                     Py_TYPE(pyObj)->tp_name);
        return false;
    }
#endif // Py_GIL_DISABLED

    return true;
}

bool isValid(SbkObject *pyObj, bool throwPyError)
{
    if (!pyObj)
        return false;

    SbkObjectPrivate *priv = pyObj->d;
    auto *ob = reinterpret_cast<PyObject *>(pyObj);
    if (!priv->cppObjectCreated && isUserType(ob)) {
        if (throwPyError)
            PyErr_Format(PyExc_RuntimeError,
                         "libshiboken: Base constructor of the object (%s) not called.",
                         Py_TYPE(ob)->tp_name);
        return false;
    }

    if (!priv->validCppObject) {
        if (throwPyError)
            PyErr_Format(PyExc_RuntimeError,
                         "libshiboken: Internal C++ object (%s) already deleted.",
                         (Py_TYPE(ob))->tp_name);
        return false;
    }

#ifdef Py_GIL_DISABLED
    // See the overload above: pending destruction refuses every lease, so the
    // object is deleted for every purpose a caller has.
    if (priv->pendingDestruction) {
        if (throwPyError)
            PyErr_Format(PyExc_RuntimeError,
                         "libshiboken: Internal C++ object (%s) already deleted.",
                         (Py_TYPE(ob))->tp_name);
        return false;
    }
#endif

    return true;
}

bool isValid(PyObject *pyObj, bool throwPyError)
{
    if (!pyObj || pyObj == Py_None ||
        !PyType_IsSubtype(Py_TYPE(pyObj), SbkObject_TypeF())) {
        return true;
    }
    return isValid(reinterpret_cast<SbkObject *>(pyObj), throwPyError);
}

#ifndef Py_GIL_DISABLED
// Not compiled under free threading: it walks the parent/child graph and
// reads cptr without the state lock, and hands back a borrowed wrapper. It
// has no caller left; should one appear, it needs a transaction and an
// AcquiredWrapper, not this.
SbkObject *findColocatedChild(SbkObject *wrapper,
                              const PyTypeObject *instanceType)
{
    // Degenerate case, wrapper is the correct wrapper.
    if (reinterpret_cast<const void *>(Shiboken::pyType(wrapper)) == reinterpret_cast<const void *>(instanceType))
        return wrapper;

    if (!(wrapper->d && wrapper->d->cptr))
        return nullptr;

    ParentInfo *pInfo = wrapper->d->parentInfo;
    if (!pInfo)
        return nullptr;

    ChildrenList &children = pInfo->children;

    for (SbkObject *child : children) {
        if (!(child->d && child->d->cptr))
            continue;
        if (child->d->cptr[0] == wrapper->d->cptr[0]) {
            auto *childType = Shiboken::pyType(child);
            return reinterpret_cast<const void *>(childType) == reinterpret_cast<const void *>(instanceType)
                ? child : findColocatedChild(child, instanceType);
        }
    }
    return nullptr;
}
#endif // !Py_GIL_DISABLED

// Legacy, for compatibility only.
PyObject *newObject(PyTypeObject *instanceType,
                    void *cptr,
                    bool hasOwnership,
                    bool isExactType,
                    const char *typeName)
{
    return isExactType
        ? newObjectForType(instanceType, cptr, hasOwnership)
        : newObjectWithHeuristics(instanceType, cptr, hasOwnership, typeName);
}

static PyObject *newObjectWithHeuristicsHelper(PyTypeObject *instanceType,
                                               const char *cppTypeIdName,
                                               void *cptr,
                                               bool hasOwnership)
{
    // Try to find the exact type of cptr via the name obtained from "typeId(ptr).name()"
    // as registered with the shiboken converters.
    PyTypeObject *exactType = ObjectType::typeForTypeName(cppTypeIdName);

    // Nothing found or same name returned (in case of a hierarchy with non-virtual destructors
    // like QStyleOption). Try type discovery in these cases.
    if (exactType == nullptr || exactType == instanceType) {
        if (auto resolved = BindingManager::instance().findDerivedType(cptr, instanceType);
            resolved.first != nullptr) {
            return newObjectForType(resolved.first, resolved.second, hasOwnership);
        }
    } else {
        // Direct, single line inheritance: Use exactType
        if (Shiboken::ObjectType::canDowncastTo(instanceType, exactType))
            return newObjectForType(exactType, cptr, hasOwnership);
        // Multiple inheritance: Try to run type discovery on the type found to cast cptr
        if (auto *derivedCptr = BindingManager::runTypeDiscovery(cptr, exactType, instanceType))
            return newObjectForType(exactType, derivedCptr, hasOwnership);
    }

    // Fall back to base type
    return newObjectForType(instanceType, cptr, hasOwnership);
}

PyObject *newObjectForPointer(PyTypeObject *instanceType,
                              void *cptr,
                              bool hasOwnership,
                              const char *typeName)
{
    return newObjectWithHeuristicsHelper(instanceType, typeName, cptr, hasOwnership);
}


PyObject *newObjectWithHeuristics(PyTypeObject *instanceType,
                                  void *cptr,
                                  bool hasOwnership,
                                  const char *typeName)
{
    return newObjectWithHeuristicsHelper(instanceType, typeName,
                                         cptr, hasOwnership);
}

PyObject *newObjectForType(PyTypeObject *instanceType, void *cptr, bool hasOwnership)
{
    auto &bindingManager = BindingManager::instance();
#ifdef Py_GIL_DISABLED
    if (auto existing = bindingManager.acquireWrapper(cptr, instanceType))
        return reinterpret_cast<PyObject *>(existing.release());

    auto *self = reinterpret_cast<SbkObject *>(SbkObject_tp_new(instanceType, nullptr, nullptr));
    self->d->cptr[0] = cptr;
    self->d->hasOwnership = hasOwnership;
    self->d->validCppObject = true;

    // Creating the wrapper runs Python, so it cannot happen under the map
    // lock; another thread may have registered one for the same pointer
    // meanwhile. Registering asks and inserts in one hold, and hands back the
    // winner if it is not ours.
    if (auto winner = bindingManager.registerWrapperUnlessPresent(self, cptr, instanceType)) {
        // Detach ours before dropping it: it never reached the map, and with
        // the pointer cleared its deallocation leaves the C++ object alone.
        // Ownership goes with the wrapper that won. If it has none and we
        // were given some, the C++ object outlives both - a leak, where
        // keeping ownership here would be the second delete.
        self->d->cptr[0] = nullptr;
        self->d->hasOwnership = false;
        self->d->validCppObject = false;
        Py_DECREF(reinterpret_cast<PyObject *>(self));
        return reinterpret_cast<PyObject *>(winner.release());
    }
    return reinterpret_cast<PyObject *>(self);
#else // Py_GIL_DISABLED
    SbkObject *self = bindingManager.retrieveWrapper(cptr, instanceType);
    if (self != nullptr) {
        Py_IncRef(reinterpret_cast<PyObject *>(self));
    } else {
        self = reinterpret_cast<SbkObject *>(SbkObject_tp_new(instanceType, nullptr, nullptr));
        self->d->cptr[0] = cptr;
        self->d->hasOwnership = hasOwnership;
        self->d->validCppObject = true;
        bindingManager.registerWrapper(self, cptr);
    }
    return reinterpret_cast<PyObject *>(self);
#endif // Py_GIL_DISABLED
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void destroy(SbkObject *self, void *cppData)
{
    // Skip if this is called with NULL pointer this can happen in derived classes
    if (!self)
        return;

    // This can be called in c++ side
    Shiboken::GilState gil;

    // Remove all references attached to this object
    clearReferences(self);

    // Remove the object from parent control

    // Verify if this object has parent
    bool hasParent = (self->d->parentInfo && self->d->parentInfo->parent);

    if (self->d->parentInfo) {
        // Check for children information and make all invalid if they exists
        _destroyParentInfo(self, true);
        // If this object has parent then the pyobject can be invalid now, because we remove the last ref after remove from parent
    }

    //if !hasParent this object could still alive
    if (!hasParent && self->d->containsCppWrapper && !self->d->hasOwnership) {
        // Remove extra ref used by c++ object this will case the pyobject destruction
        // This can cause the object death
        Py_DECREF(reinterpret_cast<PyObject *>(self));
    }

    //Python Object is not destroyed yet
    if (cppData && Shiboken::BindingManager::instance().hasWrapper(cppData)) {
        // Remove from BindingManager
        Shiboken::BindingManager::instance().releaseWrapper(self);
        self->d->hasOwnership = false;

        // the cpp object instance was deleted
        delete[] self->d->cptr;
        self->d->cptr = nullptr;
    }

    // After this point the object can be death do not use the self pointer bellow
}
#endif // !Py_GIL_DISABLED

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void removeParent(SbkObject *child, bool giveOwnershipBack, bool keepReference)
{
    ParentInfo *pInfo = child->d->parentInfo;
    if (!pInfo || !pInfo->parent) {
        if (pInfo && pInfo->hasWrapperRef) {
            pInfo->hasWrapperRef = false;
        }
        return;
    }

    ChildrenList &oldBrothers = pInfo->parent->d->parentInfo->children;
    // Verify if this child is part of parent list
    auto iChild = oldBrothers.find(child);
    if (iChild == oldBrothers.end())
        return;

    oldBrothers.erase(iChild);

    pInfo->parent = nullptr;

    // This will keep the wrapper reference, will wait for wrapper destruction to remove that
    if (keepReference &&
        child->d->containsCppWrapper) {
        //If have already a extra ref remove this one
        if (pInfo->hasWrapperRef)
            Py_DECREF(child);
        else
            pInfo->hasWrapperRef = true;
        return;
    }

    // Transfer ownership back to Python
    child->d->hasOwnership = giveOwnershipBack;

    // Remove parent ref
    Py_DECREF(child);
}
#endif // !Py_GIL_DISABLED

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void setParent(PyObject *parent, PyObject *child)
{
    if (!child || child == Py_None || child == parent)
        return;

    /*
     * setParent is recursive when the child is a native Python sequence, i.e. objects not binded by Shiboken
     * like tuple and list.
     *
     * This "limitation" exists to fix the following problem: A class multiple inherits QObject and QString,
     * so if you pass this class to someone that takes the ownership, we CAN'T enter in this if, but hey! QString
     * follows the sequence protocol.
     */
    if (PySequence_Check(child) && !Object::checkType(child)) {
        // The sequence typically comes from a container conversion that may
        // have failed - a concurrent destruction of an element is enough.
        // Calling into Python with an exception set aborts the interpreter
        // ("succeeded with an exception set"), so check before and after
        // instead of walking on. The exception is left for the caller.
        if (PyErr_Occurred() != nullptr)
            return;
        Shiboken::AutoDecRef seq(PySequence_Fast(child, nullptr));
        if (seq.isNull())
            return;
        const Py_ssize_t max = PySequence_Size(seq);
        if (max < 0)
            return;
        for (Py_ssize_t i = 0; i < max; ++i) {
            Shiboken::AutoDecRef obj(PySequence_GetItem(seq.object(), i));
            if (obj.isNull())
                return;
            setParent(parent, obj);
        }
        return;
    }

    bool parentIsNull = !parent || parent == Py_None;
    auto *parent_ = reinterpret_cast<SbkObject *>(parent);
    auto *child_ = reinterpret_cast<SbkObject *>(child);

    if (!parentIsNull) {
        if (!parent_->d->parentInfo)
            parent_->d->parentInfo = new ParentInfo;

        // do not re-add a child
        if (child_->d->parentInfo && (child_->d->parentInfo->parent == parent_)) {
            if (Shiboken::pyVerbose()) {
                std::cerr << "Warning: Attempt to re-add child "
                          << child << '/' << Py_TYPE(child)->tp_name << " to parent "
                          << parent << '/' << Py_TYPE(parent)->tp_name << '\n';
            }
            return;
        }
    }

    ParentInfo *pInfo = child_->d->parentInfo;
    bool hasAnotherParent = pInfo && pInfo->parent && pInfo->parent != parent_;

    //Avoid destroy child during reparent operation
    Py_INCREF(child);

    // check if we need to remove this child from the old parent
    if (parentIsNull || hasAnotherParent)
        removeParent(child_);

    // Add the child to the new parent
    pInfo = child_->d->parentInfo;
    if (!parentIsNull) {
        if (!pInfo)
            pInfo = child_->d->parentInfo = new ParentInfo;

        pInfo->parent = parent_;
        parent_->d->parentInfo->children.insert(child_);

        // Add Parent ref
        Py_INCREF(child);

        // Remove ownership
        child_->d->hasOwnership = false;
    }

    // Remove previous safe ref
    Py_DECREF(child);
}
#endif // !Py_GIL_DISABLED

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
void deallocData(SbkObject *self, bool cleanup)
{
    // Make cleanup if this is not a wrapper otherwise this will be done on wrapper destructor
    if(cleanup) {
        removeParent(self);

        if (self->d->parentInfo)
            _destroyParentInfo(self, true);

        clearReferences(self);
    }

    if (self->d->cptr) {
        // Remove from BindingManager
        Shiboken::BindingManager::instance().releaseWrapper(self);
        delete[] self->d->cptr;
        self->d->cptr = nullptr;
        // delete self->d; PYSIDE-205: wrong!
    }
    delete self->d; // PYSIDE-205: always delete d.
    Py_XDECREF(self->ob_dict);
    PepExt_TypeCallFree(reinterpret_cast<PyObject *>(self));
}
#endif // !Py_GIL_DISABLED

void setTypeUserData(SbkObject *wrapper, void *userData, DeleteUserDataFunc d_func)
{
    auto *type = Shiboken::pyType(wrapper);
    auto *sotp = PepType_SOTP(type);
    if (sotp->user_data)
        sotp->d_func(sotp->user_data);

    sotp->d_func = d_func;
    sotp->user_data = userData;
}

void *getTypeUserData(SbkObject *wrapper)
{
    auto *type = Shiboken::pyType(wrapper);
    return PepType_SOTP(type)->user_data;
}

static inline bool isNone(const PyObject *o)
{
    return o == nullptr || o == Py_None;
}

static void keepReferenceImpl(SbkObject *self, const char *key, PyObject *object,
                              bool append, bool invalidate);

void keepReference(SbkObject *self, const char *key, PyObject *object, bool append)
{
    keepReferenceImpl(self, key, object, append, true);
}

void keepReferenceToIndependentObject(SbkObject *self, const char *key, PyObject *object, bool append)
{
    keepReferenceImpl(self, key, object, append, false);
}

#ifndef Py_GIL_DISABLED  // free-threaded twin: see the state-lock chapter below
static void removeRefCountKey(SbkObject *self, const std::string &key)
{
    if (self->d->referredObjects) {
        self->d->referredObjects->setInvalidates(key, true);
        const auto iterPair = self->d->referredObjects->equal_range(key);
        if (iterPair.first != iterPair.second) {
            decRefPyObjectList(iterPair.first, iterPair.second);
            self->d->referredObjects->erase(iterPair.first, iterPair.second);
        }
    }
}

static void keepReferenceImpl(SbkObject *self, const char *keyC, PyObject *referredObject,
                              bool append, bool invalidate)
{
    std::string key(keyC);

    if (isNone(referredObject)) {
        removeRefCountKey(self, key);
        return;
    }

    if (!self->d->referredObjects) {
        auto references = std::unique_ptr<RefCountMap>(
            new RefCountMap{RefCountMap::value_type{key, referredObject}});
        references->setInvalidates(key, invalidate);
        self->d->referredObjects = references.release();
        Py_INCREF(referredObject);
        return;
    }

    RefCountMap &refCountMap = *(self->d->referredObjects);
    refCountMap.setInvalidates(key, invalidate);
    const auto iterPair = refCountMap.equal_range(key);
    if (std::any_of(iterPair.first, iterPair.second,
                    [referredObject](const RefCountMap::value_type &v) { return v.second == referredObject; })) {
        return;
    }

    if (!append && iterPair.first != iterPair.second) {
        decRefPyObjectList(iterPair.first, iterPair.second);
        refCountMap.erase(iterPair.first, iterPair.second);
    }

    refCountMap.insert(RefCountMap::value_type{key, referredObject});
    Py_INCREF(referredObject);
}

void removeReference(SbkObject *self, const char *key, PyObject *referredObject)
{
    if (!isNone(referredObject))
        removeRefCountKey(self, std::string(key));
}

void clearReferences(SbkObject *self)
{
    if (!self->d->referredObjects)
        return;

    RefCountMap &refCountMap = *(self->d->referredObjects);
    for (auto it = refCountMap.begin(), end = refCountMap.end(); it != end; ++it)
        Py_DECREF(it->second);
    self->d->referredObjects->clear();
}
#endif // !Py_GIL_DISABLED

#ifdef Py_GIL_DISABLED
// ---- State-lock transactions ----------------------------------------------
//
// The free-threaded implementations of everything above that touches the
// shared binding state. They are collected here rather than placed next to
// their counterparts so that a build with a GIL compiles the original file,
// unmoved and unindented - what it gets is this chapter removed, nothing
// else. Read them as one unit: they share the contract in sbkstatelock.h.
//
// Anyone fixing a bug in one of the originals above has to fix its twin here.
//
// The "Locked" suffix means: the caller holds the state lock, and the function
// obeys the contract in sbkstatelock.h - no Python call-out, no decref, no
// destructor, no other lock. Anything of that kind goes into the
// DeferredActions list and runs after the unlock.

// Prepared before publishing pendingDestruction. The root's immutable native
// slots are the only independently destroyed allocations; its owned children
// are destroyed by those C++ destructors. All bookkeeping after preparation
// uses intrusive links in already allocated wrapper private data.
struct PendingDestruction
{
    SbkObject *root = nullptr;
    SbkObject *objects = nullptr;
    PendingDestruction *next = nullptr;
    DestructorEntries destructors;
    bool needsMainThread = false;
};

static PendingDestruction *&pendingDestructions()
{
    static PendingDestruction *head = nullptr;
    return head;
}

static void enrollDestructionLocked(SbkObject *object, PendingDestruction *plan)
{
    SBK_ASSERT_STATE_LOCKED();
    auto *priv = object->d;
    // Another pending root owns its entire subtree until completion. An
    // ancestor plan waits for that root instead of scheduling it twice.
    if (priv->destructionPlan != nullptr)
        return;
    priv->destructionPlan = plan;
    priv->destructionNext = plan->objects;
    plan->objects = object;
    Py_INCREF(reinterpret_cast<PyObject *>(object));
    priv->pendingDestruction = true;
    plan->needsMainThread |= priv->deleteInMainThread;
    if (auto *info = priv->parentInfo) {
        for (SbkObject *child : info->children)
            enrollDestructionLocked(child, plan);
    }
}

static bool destructionIsBusyLocked(SbkObject *object, PendingDestruction *plan,
                                    bool &needsMainThread)
{
    SBK_ASSERT_STATE_LOCKED();
    auto *priv = object->d;
    if (priv->destructionVisiting)
        return false;
    priv->destructionVisiting = true;
    needsMainThread |= priv->deleteInMainThread;
    bool busy = priv->activeCalls != 0
        || (priv->destructionPlan != nullptr && priv->destructionPlan != plan);
    if (auto *info = priv->parentInfo) {
        for (SbkObject *child : info->children)
            busy |= destructionIsBusyLocked(child, plan, needsMainThread);
    }
    priv->destructionVisiting = false;
    return busy;
}

static void markCurrentDestructionLocked(SbkObject *object, PendingDestruction *plan)
{
    SBK_ASSERT_STATE_LOCKED();
    auto *priv = object->d;
    if (priv->destructionMember)
        return;
    if (priv->destructionPlan == nullptr)
        enrollDestructionLocked(object, plan);
    assert(priv->destructionPlan == plan);
    priv->destructionMember = true;
    if (auto *info = priv->parentInfo) {
        for (SbkObject *child : info->children)
            markCurrentDestructionLocked(child, plan);
    }
}

static void commitDestructionLocked(PendingDestruction *plan)
{
    SBK_ASSERT_STATE_LOCKED();
    // A call already in flight may have reparented children before releasing
    // its lease. Only the current owned graph is destroyed. Its traversal and
    // any additional pins require no allocation.
    markCurrentDestructionLocked(plan->root, plan);
    if (!plan->root->d->validCppObject || plan->root->d->cptr == nullptr)
        plan->destructors.clear(); // A native destructor already claimed it.
    for (SbkObject *object = plan->objects; object != nullptr;
         object = object->d->destructionNext) {
        auto *priv = object->d;
        if (!priv->destructionMember)
            continue;
        assert(priv->activeCalls == 0);
        const bool hasParent = priv->parentInfo != nullptr
            && priv->parentInfo->parent != nullptr;
        priv->destructionDropExtra = !hasParent && priv->containsCppWrapper
            && !priv->hasOwnership;
        priv->destructionPointers = priv->cptr;
        priv->cptr = nullptr;
        priv->validCppObject = false;
        priv->hasOwnership = false;
        priv->destructionReferences = priv->referredObjects;
        priv->referredObjects = nullptr;
    }
}

static void finishDestruction(PendingDestruction *plan)
{
    SBK_ASSERT_STATE_UNLOCKED();
    auto &manager = BindingManager::instance();
    for (SbkObject *object = plan->objects; object != nullptr;
         object = object->d->destructionNext) {
        if (object->d->destructionMember)
            manager.unregisterWrapper(object, object->d->destructionPointers);
    }
    callDestructor(plan->destructors);

    // Keep parent links until the native destructors finish: an ancestor
    // pending root must continue to see this foreign plan and wait for it.
    {
        StateLockGuard guard;
        for (SbkObject *object = plan->objects; object != nullptr;
             object = object->d->destructionNext) {
            auto *priv = object->d;
            if (!priv->destructionMember)
                continue;
            if (auto *info = priv->parentInfo) {
                if (auto *parent = info->parent) {
                    if (parent->d->parentInfo->children.erase(object) != 0)
                        ++priv->destructionRefsToDrop;
                    info->parent = nullptr;
                }
                info->hasWrapperRef = false;
            }
            if (priv->destructionDropExtra)
                ++priv->destructionRefsToDrop;
        }
    }

    // Every node stays pinned until its own cleanup; all other plan nodes
    // retain their pins if a decref calls arbitrary Python code.
    SbkObject *object = plan->objects;
    while (object != nullptr) {
        SbkObject *next = nullptr;
        RefCountMap *references = nullptr;
        void **pointers = nullptr;
        unsigned refsToDrop = 0;
        {
            StateLockGuard guard;
            auto *priv = object->d;
            next = priv->destructionNext;
            references = priv->destructionReferences;
            pointers = priv->destructionPointers;
            refsToDrop = priv->destructionRefsToDrop;
            // A child reparented out while the request waited remains alive.
            if (!priv->destructionMember)
                priv->pendingDestruction = false;
            priv->destructionPlan = nullptr;
            priv->destructionNext = nullptr;
            priv->destructionMember = false;
            priv->destructionDropExtra = false;
            priv->destructionReferences = nullptr;
            priv->destructionPointers = nullptr;
            priv->destructionRefsToDrop = 0;
        }
        if (references != nullptr) {
            for (const auto &reference : *references)
                Py_DECREF(reference.second);
            delete references;
        }
        delete[] pointers;
        while (refsToDrop-- != 0)
            Py_DECREF(reinterpret_cast<PyObject *>(object));
        Py_DECREF(reinterpret_cast<PyObject *>(object)); // The plan's pin.
        object = next;
    }
    delete plan;
}

// Free-threaded CPython supplies the normalized raised-exception API.
// Errors::Stash owns a heap-allocated ErrorStore, which is unsafe on a lease
// destructor or after publishing an irreversible destruction request.
class DestructionErrorStash
{
public:
    DestructionErrorStash() : m_exception(PyErr_GetRaisedException()) {}
    ~DestructionErrorStash()
    {
        if (m_exception != nullptr)
            PyErr_SetRaisedException(m_exception);
        else
            PyErr_Clear();
    }
    DestructionErrorStash(const DestructionErrorStash &) = delete;
    DestructionErrorStash &operator=(const DestructionErrorStash &) = delete;
private:
    PyObject *m_exception;
};

void runPendingDestructions()
{
    SBK_ASSERT_STATE_UNLOCKED();
    const bool onMainThread = Shiboken::currentThreadId() == Shiboken::mainThreadId();
    bool notifyMainThread = false;
    DestructionErrorStash errorStash;
    while (true) {
        PendingDestruction *ready = nullptr;
        {
            StateLockGuard guard;
            for (auto **link = &pendingDestructions(); *link != nullptr;
                 link = &(*link)->next) {
                auto *plan = *link;
                bool needsMainThread = plan->needsMainThread;
                const bool busy = destructionIsBusyLocked(plan->root, plan, needsMainThread);
                plan->needsMainThread = needsMainThread;
                if (needsMainThread && !onMainThread) {
                    notifyMainThread = true;
                    continue;
                }
                if (busy)
                    continue;
                ready = plan;
                *link = plan->next;
                commitDestructionLocked(plan);
                break;
            }
        }
        if (ready == nullptr)
            break;
        finishDestruction(ready);
    }
    if (notifyMainThread)
        notifyMainThreadDeletion();
}

void callCppDestructors(SbkObject *pyObj)
{
    SBK_ASSERT_STATE_UNLOCKED();
    bool destroyQApp = false;
    try {
        auto plan = std::make_unique<PendingDestruction>();
        {
            StateLockGuard guard;
            auto *priv = pyObj->d;
            if (priv->cptr == nullptr || priv->pendingDestruction)
                return;
            if (!priv->validCppObject && priv->constructionSlots.empty())
                return;
            if (priv->isQAppSingleton && DestroyQApplication) {
                destroyQApp = true;
            } else {
                // Every throwing allocation precedes graph/validity changes.
                plan->destructors = getDestructorEntries(pyObj);
                plan->root = pyObj;
                enrollDestructionLocked(pyObj, plan.get());
                plan->next = pendingDestructions();
                pendingDestructions() = plan.release();
            }
        }
    } catch (const std::bad_alloc &) {
        PyErr_NoMemory();
        return;
    }
    if (destroyQApp)
        DestroyQApplication();
    else
        runPendingDestructions();
}

// The C++ side gave up the object: the wrapper's C++ instance is gone or is
// about to be. Called from the generated wrapper destructor and, at shutdown,
// from ~BindingManager. Every caller holds a reference to the wrapper (an
// AcquiredWrapper on this build), so the decref below is never the last one
// and self stays valid to the end.
void destroy(SbkObject *self, void *cppData)
{
    SBK_ASSERT_STATE_UNLOCKED();
    // Skip if this is called with NULL pointer this can happen in derived classes
    if (!self)
        return;

    // This can be called in c++ side
    Shiboken::GilState gil;

    // Remove all references attached to this object
    clearReferences(self);

    // Remove the object from parent control

    // Verify if this object has parent
    bool hasParentInfo = false;
    bool hasParent = false;
    {
        StateLockGuard guard;
        auto *pInfo = self->d->parentInfo;
        hasParentInfo = pInfo != nullptr;
        hasParent = hasParentInfo && pInfo->parent != nullptr;
    }

    if (hasParentInfo) {
        // Check for children information and make all invalid if they exists
        _destroyParentInfo(self, true);
    }

    // Without a parent the object may still be alive.
    bool dropExtraRef = false;
    if (!hasParent) {
        StateLockGuard guard;
        auto *priv = self->d;
        dropExtraRef = priv->containsCppWrapper && !priv->hasOwnership;
    }
    if (dropExtraRef) {
        // Remove extra ref used by c++ object
        Py_DECREF(reinterpret_cast<PyObject *>(self));
    }

    //Python Object is not destroyed yet
    auto &bindingManager = BindingManager::instance();
    if (cppData != nullptr && bindingManager.hasWrapper(cppData)) {
        void **cptrs = nullptr;
        {
            StateLockGuard guard;
            auto *priv = self->d;
            // the cpp object instance was deleted
            cptrs = priv->cptr;
            priv->cptr = nullptr;
            priv->hasOwnership = false;
            priv->validCppObject = false;
        }
        // Like finishDestruction(): the registry entry goes after the unlock
        // and with the detached pointers, because the map lock must not be
        // taken inside a transaction.
        if (cptrs != nullptr) {
            bindingManager.unregisterWrapper(self, cptrs);
            delete[] cptrs;
        }
    }
}

// Free everything the wrapper owns and the wrapper itself. Reached only from
// the deallocation path, so the object is at refcount zero and already out of
// the wrapper map.
void deallocData(SbkObject *self, bool cleanup)
{
    SBK_ASSERT_STATE_UNLOCKED();
    // Make cleanup if this is not a wrapper otherwise this will be done on
    // wrapper destructor. All three take the state lock themselves.
    if (cleanup) {
        removeParent(self);

        if (self->d->parentInfo)
            _destroyParentInfo(self, true);

        clearReferences(self);
    }

    void **cptrs = nullptr;
    {
        StateLockGuard guard;
        auto *priv = self->d;
        // A lease does not hold a reference, so at refcount zero none can be
        // open.
        assert(priv->activeCalls == 0);
        cptrs = priv->cptr;
        priv->cptr = nullptr;
        priv->validCppObject = false;
    }

    if (cptrs != nullptr) {
        // Remove from BindingManager, with the detached pointers.
        BindingManager::instance().unregisterWrapper(self, cptrs);
        delete[] cptrs;
    }
    delete self->d; // PYSIDE-205: always delete d.
    Py_XDECREF(self->ob_dict);
    PepExt_TypeCallFree(reinterpret_cast<PyObject *>(self));
}

// ---- C++ call leases -------------------------------------------------------

enum class LeaseFailure
{
    None,
    NotInitialized, // Base constructor not called.
    Deleted         // C++ object gone, or destruction pending.
};

/// Takes the lease and hands out the C++ pointer it validated. The pointer
/// has to leave the transaction with the lease: Object::destroy() clears
/// cptr under the lock without consulting activeCalls, so re-reading it
/// after the unlock can find a null pointer the lease was granted against.
static LeaseFailure acquireCallLeaseLocked(SbkObject *self, void **cppObject)
{
    SBK_ASSERT_STATE_LOCKED();
    auto *priv = self->d;
    if (!priv->cppObjectCreated && isUserType(reinterpret_cast<PyObject *>(self)))
        return LeaseFailure::NotInitialized;
    if (!priv->validCppObject || priv->pendingDestruction || priv->cptr == nullptr)
        return LeaseFailure::Deleted;
    ++priv->activeCalls;
    *cppObject = priv->cptr[0];
    return LeaseFailure::None;
}

static void releaseCallLease(SbkObject *self)
{
    SBK_ASSERT_STATE_UNLOCKED();
    bool drain = false;
    {
        StateLockGuard guard;
        assert(self->d->activeCalls != 0);
        drain = --self->d->activeCalls == 0 && pendingDestructions() != nullptr;
    }
    if (drain)
        runPendingDestructions();
}

CallLease::CallLease(PyObject *pyObj, Guard guardMode)
{
    // Same acceptance as isValid(PyObject *): anything that is not a wrapper
    // instance needs no lease.
    if (pyObj == nullptr || pyObj == Py_None || PyType_Check(pyObj) != 0
        || Py_TYPE(reinterpret_cast<PyObject *>(Py_TYPE(pyObj))) != SbkObjectType_TypeF()) {
        m_valid = true;
        return;
    }

    auto *self = reinterpret_cast<SbkObject *>(pyObj);
    LeaseFailure failure = LeaseFailure::None;
    void *cppObject = nullptr;
    {
        StateLockGuard guard;
        failure = acquireCallLeaseLocked(self, &cppObject);
    }

    if (failure == LeaseFailure::None) {
        m_self = self;

        // Qt is not thread-safe per object, and without a GIL nothing else
        // serializes two threads that reach the same one. Taken outside the
        // state lock, because it spans the C++ call.
        //
        // Only for the receiver: nesting critical sections does not lock two
        // objects, the inner one suspends the outer - see Guard in the header.
        if (guardMode == Guard::Take)
            m_guard.acquire(cppObject);

        m_valid = true;
        return;
    }

    // Raising is a Python call-out and happens after the transaction.
    if (failure == LeaseFailure::NotInitialized) {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: '__init__' method of object's base class (%s) not called.",
                     Py_TYPE(pyObj)->tp_name);
    } else {
        PyErr_Format(PyExc_RuntimeError,
                     "libshiboken: Internal C++ object (%s) already deleted.",
                     Py_TYPE(pyObj)->tp_name);
    }
}

CallLease::~CallLease()
{
    // Let go before the release, which may run the C++ destructor.
    m_guard.release();
    if (m_self != nullptr)
        releaseCallLease(m_self);
}

void getOwnership(SbkObject *sbkObj)
{
    SBK_ASSERT_STATE_UNLOCKED();
    DeferredActions deferred;
    bool revalidate = false;

    {
        StateLockGuard guard;
        auto *priv = sbkObj->d;
        // skip if already have the ownership
        if (priv->hasOwnership)
            return;
        // skip if this object has parent
        if (priv->parentInfo && priv->parentInfo->parent)
            return;

        // Get back the ownership
        priv->hasOwnership = true;

        if (priv->containsCppWrapper)
            deferred.addDecref(sbkObj);  // Remove extra ref
        else
            revalidate = true;
    }

    if (revalidate)
        makeValid(sbkObj);  // Make the object valid again; walks the graph
    deferred.run();
}

void releaseOwnership(SbkObject *sbkObj)
{
    SBK_ASSERT_STATE_UNLOCKED();
    auto *ob = reinterpret_cast<PyObject *>(sbkObj);
    auto *selfType = Py_TYPE(ob);
    const bool isValueType =
        Shiboken::Conversions::pythonTypeIsValueType(PepType_SOTP(selfType)->converter);
    bool needsInvalidate = false;

    {
        StateLockGuard guard;
        auto *priv = sbkObj->d;
        // skip if the ownership have already moved to c++
        if (!priv->hasOwnership || isValueType)
            return;

        // remove object ownership
        priv->hasOwnership = false;

        // If We have control over object life
        if (priv->containsCppWrapper) {
            // Pinning increment: an incref cannot run user code, so it is one
            // of the audited exceptions to the "no refcount work" rule. Keeps
            // the Python object alive until the wrapper destructor call.
            Py_INCREF(ob);
        } else {
            needsInvalidate = true;
        }
    }

    // If I do not know when this object will die We need to invalidate this to
    // avoid use after free. Walks the graph and touches the wrapper map, so it
    // runs outside the transaction.
    if (needsInvalidate)
        invalidate(sbkObj);
}

// What invalidation decides under the state lock but must not do there:
// releasing wrappers takes the wrapper map lock, detaching children takes the
// state lock again (it is not recursive), and referred objects have to go
// through the sequence protocol first.
struct InvalidationPlan
{
    std::vector<SbkObject *> releaseWrappers;
    std::vector<SbkObject *> detachChildren;
    std::vector<PyObject *> referred;   // to be split outside the lock
};

// Pin an object the plan has to survive the unlock. Running the plan decrefs
// (removeParent() hands the parent reference back), and that cascade can free
// an object a later step - or a later round - still has to touch. Increments
// are permitted inside the transaction, decrements are not, so the pins are
// released in invalidateRoots() with no lock held.
static void pinForPlan(PyObject *pyObj, std::vector<PyObject *> &pins)
{
    SBK_ASSERT_STATE_LOCKED();
    Py_INCREF(pyObj);
    pins.push_back(pyObj);
}

static void collectInvalidateLocked(SbkObject *self, std::set<SbkObject *> &seen,
                                    InvalidationPlan &plan,
                                    std::vector<PyObject *> &pins)
{
    SBK_ASSERT_STATE_LOCKED();
    // Skip if this object not is a valid object or if it's already been seen
    if (!self || reinterpret_cast<PyObject *>(self) == Py_None || seen.find(self) != seen.end())
        return;
    seen.insert(self);

    if (!self->d->containsCppWrapper) {
        self->d->validCppObject = false; // Mark object as invalid only if this is not a wrapper class
        plan.releaseWrappers.push_back(self);
    }

    // If it is a parent invalidate all children.
    if (self->d->parentInfo) {
        // Create a copy because this list can be changed during the process
        ChildrenList copy = self->d->parentInfo->children;

        for (SbkObject *child : copy) {
            // invalidate the child
            collectInvalidateLocked(child, seen, plan, pins);

            // if the parent not is a wrapper class, then remove children from him, because We do not know when this object will be destroyed
            if (!self->d->validCppObject) {
                plan.detachChildren.push_back(child);
                pinForPlan(reinterpret_cast<PyObject *>(child), pins);
            }
        }
    }

    // If has ref to other objects invalidate all
    if (auto *rInfo = self->d->referredObjects) {
        for (const auto &p : *rInfo) {
            if (!rInfo->invalidates(p.first))
                continue;
            plan.referred.push_back(p.second);
            pinForPlan(p.second, pins);
        }
    }
}

// Run what the transaction collected, with no lock held.
static void runInvalidationPlan(const InvalidationPlan &plan)
{
    SBK_ASSERT_STATE_UNLOCKED();
    auto &bindingManager = BindingManager::instance();
    for (SbkObject *o : plan.releaseWrappers)
        bindingManager.releaseWrapper(o);
    for (SbkObject *child : plan.detachChildren)
        removeParent(child, true, true);
}

// Walk the graph in rounds: a round collects under the lock, the referred
// objects it found are split outside it (that runs the sequence protocol),
// and the objects behind them feed the next round. Terminates because `seen`
// only grows.
static void invalidateRoots(std::vector<SbkObject *> roots)
{
    SBK_ASSERT_STATE_UNLOCKED();
    std::set<SbkObject *> seen;
    // Held until the whole walk is over, not per round: the objects a round
    // reaches through plan.referred become the next round's roots, so dropping
    // the pins early would just move the dangling pointer one round on.
    std::vector<PyObject *> pins;
    while (!roots.empty()) {
        InvalidationPlan plan;
        {
            StateLockGuard guard;
            for (SbkObject *o : roots)
                collectInvalidateLocked(o, seen, plan, pins);
        }
        runInvalidationPlan(plan);

        roots.clear();
        for (PyObject *ref : plan.referred) {
            for (SbkObject *o : splitPyObject(ref)) {
                if (seen.find(o) == seen.end())
                    roots.push_back(o);
            }
        }
    }
    // No lock held: these decrefs may run destructors that re-enter the
    // binding layer.
    for (PyObject *o : pins)
        Py_DECREF(o);
}

void invalidate(PyObject *pyobj)
{
    SBK_ASSERT_STATE_UNLOCKED();
    // splitPyObject() runs the sequence protocol: never under the lock.
    invalidateRoots(splitPyObject(pyobj));
}

void invalidate(SbkObject *self)
{
    SBK_ASSERT_STATE_UNLOCKED();
    invalidateRoots({self});
}

// Counterpart of collectInvalidateLocked(): it walks the same containers -
// parentInfo->children and referredObjects - so it moves to the state lock
// together with invalidate(). It qualifies as a transaction: it only sets
// flags and calls checkType() (PyType_IsSubtype), no decref, no Python
// protocol, no second lock.
static void makeValidLocked(SbkObject *self)
{
    SBK_ASSERT_STATE_LOCKED();
    // Skip if this object not is a valid object
    if (!self || reinterpret_cast<PyObject *>(self) == Py_None || self->d->validCppObject)
        return;

    // Mark object as invalid only if this is not a wrapper class
    self->d->validCppObject = true;

    // If it is a parent make  all children valid
    if (self->d->parentInfo) {
        for (SbkObject *child : self->d->parentInfo->children)
            makeValidLocked(child);
    }

    // If has ref to other objects make all valid again
    if (self->d->referredObjects) {
        const RefCountMap &refCountMap = *(self->d->referredObjects);
        for (const auto &p : refCountMap) {
            if (refCountMap.invalidates(p.first) && Shiboken::Object::checkType(p.second))
                makeValidLocked(reinterpret_cast<SbkObject *>(p.second));
        }
    }
}

void makeValid(SbkObject *self)
{
    SBK_ASSERT_STATE_UNLOCKED();
    StateLockGuard guard;
    makeValidLocked(self);
}

static void removeParentLocked(SbkObject *child, bool giveOwnershipBack,
                               bool keepReference, DeferredActions &deferred)
{
    SBK_ASSERT_STATE_LOCKED();
    ParentInfo *pInfo = child->d->parentInfo;
    if (!pInfo || !pInfo->parent) {
        if (pInfo && pInfo->hasWrapperRef) {
            pInfo->hasWrapperRef = false;
        }
        return;
    }

    ChildrenList &oldBrothers = pInfo->parent->d->parentInfo->children;
    // Verify if this child is part of parent list
    auto iChild = oldBrothers.find(child);
    if (iChild == oldBrothers.end())
        return;

    oldBrothers.erase(iChild);

    pInfo->parent = nullptr;

    // This will keep the wrapper reference, will wait for wrapper destruction to remove that
    if (keepReference &&
        child->d->containsCppWrapper) {
        //If have already a extra ref remove this one
        if (pInfo->hasWrapperRef)
            deferred.addDecref(child);
        else
            pInfo->hasWrapperRef = true;
        return;
    }

    // Transfer ownership back to Python
    child->d->hasOwnership = giveOwnershipBack;

    // Remove parent ref. This can be the last one: it must not run while the
    // graph is locked, because deallocation re-enters the binding layer.
    deferred.addDecref(child);
}

void removeParent(SbkObject *child, bool giveOwnershipBack, bool keepReference)
{
    SBK_ASSERT_STATE_UNLOCKED();
    DeferredActions deferred;
    {
        StateLockGuard guard;
        removeParentLocked(child, giveOwnershipBack, keepReference, deferred);
    }
    deferred.run();
}

// Result of the setParent() transaction, for the diagnostics that must not be
// printed while the state lock is held.
enum class SetParentResult
{
    Done,
    ReAddedChild
};

static SetParentResult setParentLocked(SbkObject *parent, SbkObject *child,
                                       DeferredActions &deferred)
{
    SBK_ASSERT_STATE_LOCKED();
    const bool parentIsNull = parent == nullptr;

    if (!parentIsNull) {
        // Plain operator new, not a Python allocation: permitted under the
        // state lock.
        if (!parent->d->parentInfo)
            parent->d->parentInfo = new ParentInfo;

        // do not re-add a child
        if (child->d->parentInfo && child->d->parentInfo->parent == parent)
            return SetParentResult::ReAddedChild;
    }

    ParentInfo *pInfo = child->d->parentInfo;
    const bool hasAnotherParent = pInfo && pInfo->parent && pInfo->parent != parent;

    // check if we need to remove this child from the old parent.
    // giveOwnershipBack must stay true: this is what removeParent()'s default
    // argument did before the transaction was made explicit. Without it,
    // setParent(None) leaves the object owned by C++, it is never destroyed
    // and destroyed() never fires (bug_576).
    if (parentIsNull || hasAnotherParent)
        removeParentLocked(child, true, false, deferred);

    // Add the child to the new parent
    pInfo = child->d->parentInfo;
    if (!parentIsNull) {
        if (!pInfo)
            pInfo = child->d->parentInfo = new ParentInfo;

        pInfo->parent = parent;
        parent->d->parentInfo->children.insert(child);

        // Add Parent ref (pinning increment, see releaseOwnership())
        Py_INCREF(reinterpret_cast<PyObject *>(child));

        // Remove ownership
        child->d->hasOwnership = false;
    }

    return SetParentResult::Done;
}

void setParent(PyObject *parent, PyObject *child)
{
    SBK_ASSERT_STATE_UNLOCKED();
    if (!child || child == Py_None || child == parent)
        return;

    /*
     * setParent is recursive when the child is a native Python sequence, i.e. objects not binded by Shiboken
     * like tuple and list.
     *
     * This "limitation" exists to fix the following problem: A class multiple inherits QObject and QString,
     * so if you pass this class to someone that takes the ownership, we CAN'T enter in this if, but hey! QString
     * follows the sequence protocol.
     */
    if (PySequence_Check(child) && !Object::checkType(child)) {
        // The sequence typically comes from a container conversion that may
        // have failed - a concurrent destruction of an element is enough.
        // Calling into Python with an exception set aborts the interpreter
        // ("succeeded with an exception set"), so check before and after
        // instead of walking on. The exception is left for the caller.
        if (PyErr_Occurred() != nullptr)
            return;
        Shiboken::AutoDecRef seq(PySequence_Fast(child, nullptr));
        if (seq.isNull())
            return;
        const Py_ssize_t max = PySequence_Size(seq);
        if (max < 0)
            return;
        for (Py_ssize_t i = 0; i < max; ++i) {
            Shiboken::AutoDecRef obj(PySequence_GetItem(seq.object(), i));
            if (obj.isNull())
                return;
            setParent(parent, obj);
        }
        return;
    }

    const bool parentIsNull = !parent || parent == Py_None;
    auto *parent_ = parentIsNull ? nullptr : reinterpret_cast<SbkObject *>(parent);
    auto *child_ = reinterpret_cast<SbkObject *>(child);

    //Avoid destroy child during reparent operation
    Py_INCREF(child);

    DeferredActions deferred;
    SetParentResult result{};
    {
        StateLockGuard guard;
        result = setParentLocked(parent_, child_, deferred);
    }
    deferred.run();

    if (result == SetParentResult::ReAddedChild && Shiboken::pyVerbose()) {
        // I/O: never under the state lock.
        std::cerr << "Warning: Attempt to re-add child "
                  << child << '/' << Py_TYPE(child)->tp_name << " to parent "
                  << parent << '/' << Py_TYPE(parent)->tp_name << '\n';
    }

    // Remove previous safe ref
    Py_DECREF(child);
}

static void removeRefCountKeyLocked(SbkObject *self, const std::string &key,
                                    DeferredActions &deferred)
{
    SBK_ASSERT_STATE_LOCKED();
    if (self->d->referredObjects) {
        self->d->referredObjects->setInvalidates(key, true);
        const auto iterPair = self->d->referredObjects->equal_range(key);
        for (auto it = iterPair.first; it != iterPair.second; ++it)
            deferred.addDecref(it->second);
        self->d->referredObjects->erase(iterPair.first, iterPair.second);
    }
}

static void keepReferenceLocked(SbkObject *self, const std::string &key,
                                PyObject *referredObject, bool append, bool invalidate,
                                DeferredActions &deferred)
{
    SBK_ASSERT_STATE_LOCKED();
    if (!self->d->referredObjects) {
        auto references = std::unique_ptr<RefCountMap>(
            new RefCountMap{RefCountMap::value_type{key, referredObject}});
        references->setInvalidates(key, invalidate);
        self->d->referredObjects = references.release();
        Py_INCREF(referredObject);  // pinning increment
        return;
    }

    RefCountMap &refCountMap = *(self->d->referredObjects);
    refCountMap.setInvalidates(key, invalidate);
    const auto iterPair = refCountMap.equal_range(key);
    if (std::any_of(iterPair.first, iterPair.second,
                    [referredObject](const RefCountMap::value_type &v) { return v.second == referredObject; })) {
        return;
    }

    if (!append && iterPair.first != iterPair.second) {
        for (auto it = iterPair.first; it != iterPair.second; ++it)
            deferred.addDecref(it->second);
        refCountMap.erase(iterPair.first, iterPair.second);
    }

    refCountMap.insert(RefCountMap::value_type{key, referredObject});
    Py_INCREF(referredObject);  // pinning increment
}

static void keepReferenceImpl(SbkObject *self, const char *keyC, PyObject *referredObject,
                              bool append, bool invalidate)
{
    SBK_ASSERT_STATE_UNLOCKED();
    std::string key(keyC);  // allocate the key before the transaction

    DeferredActions deferred;
    {
        StateLockGuard guard;
        if (isNone(referredObject))
            removeRefCountKeyLocked(self, key, deferred);
        else
            keepReferenceLocked(self, key, referredObject, append, invalidate, deferred);
    }
    deferred.run();
}

void removeReference(SbkObject *self, const char *key, PyObject *referredObject)
{
    SBK_ASSERT_STATE_UNLOCKED();
    if (isNone(referredObject))
        return;

    const std::string keyString(key);
    DeferredActions deferred;
    {
        StateLockGuard guard;
        removeRefCountKeyLocked(self, keyString, deferred);
    }
    deferred.run();
}

static void clearReferencesLocked(SbkObject *self, DeferredActions &deferred)
{
    SBK_ASSERT_STATE_LOCKED();
    if (!self->d->referredObjects)
        return;

    RefCountMap &refCountMap = *(self->d->referredObjects);
    for (const auto &p : refCountMap)
        deferred.addDecref(p.second);
    refCountMap.clear();
}

void clearReferences(SbkObject *self)
{
    SBK_ASSERT_STATE_UNLOCKED();
    DeferredActions deferred;
    {
        StateLockGuard guard;
        clearReferencesLocked(self, deferred);
    }
    deferred.run();
}

#endif // Py_GIL_DISABLED

SbkConverter *getConverter(PyTypeObject *type)
{
    return PepType_SOTP(type)->converter;
}

// Helpers for debug / info formatting

static std::vector<PyTypeObject *> getBases(SbkObject *self)
{
    return self->d->cppTypes;
}

static bool isValueType(SbkObject *self)
{
    return PepType_SOTP(Shiboken::pyType(self))->type_behaviour == BEHAVIOUR_VALUETYPE;
}

void _debugFormat(std::ostream &s, SbkObject *self)
{
    assert(self);
    auto *d = self->d;
    if (!d) {
        s  << "[Invalid]";
        return;
    }
    if (d->cptr) {
        const std::vector<PyTypeObject *> bases = getBases(self);
        for (size_t i = 0, size = bases.size(); i < size; ++i)
            s << ", C++: " << bases[i]->tp_name << '/' << self->d->cptr[i];
    } else {
         s << " [Deleted]";
    }
    if (d->hasOwnership)
        s << " [hasOwnership]";
    if (d->containsCppWrapper)
        s << " [containsCppWrapper]";
    if (d->validCppObject)
        s << " [validCppObject]";
    if (d->cppObjectCreated)
        s << " [wasCreatedByPython]";
    s << (isValueType(self) ? " [value]" : " [object]");

    if (d->parentInfo) {
        if (auto *parent = d->parentInfo->parent)
            s << ", parent=" << reinterpret_cast<PyObject *>(parent)->ob_type->tp_name
                << '/' << parent;
        if (!d->parentInfo->children.empty())
            s << ", " << d->parentInfo->children.size() << " child(ren)";
    }
    if (d->referredObjects && !d->referredObjects->empty())
        s << ", " << d->referredObjects->size() << " referred object(s)";
}

std::string info(SbkObject *self)
{
    std::ostringstream s;

    s << "id................ " << self << '\n';
    if (self->d && self->d->cptr) {
        const std::vector<PyTypeObject *> bases = getBases(self);

        s << "C++ address....... ";
        for (size_t i = 0, size = bases.size(); i < size; ++i)
            s << bases[i]->tp_name << '/' << self->d->cptr[i] << ' ';
        s << "\n";
    }
    else {
        s << "C++ address....... <<Deleted>>\n";
    }

    s << "hasOwnership...... " << bool(self->d->hasOwnership) << "\n"
         "containsCppWrapper " << self->d->containsCppWrapper << "\n"
         "validCppObject.... " << self->d->validCppObject << "\n"
         "wasCreatedByPython " << self->d->cppObjectCreated << "\n"
         "value......        " << isValueType(self) << "\n"
         "reference count... " << Py_REFCNT(reinterpret_cast<PyObject *>(self)) << '\n';

    if (self->d->parentInfo && self->d->parentInfo->parent) {
        auto *obParent = reinterpret_cast<PyObject *>(self->d->parentInfo->parent);
        s << "parent............ <" << Py_TYPE(obParent)->tp_name << " at " << obParent << ">\n";
    }

    if (self->d->parentInfo && !self->d->parentInfo->children.empty()) {
        const auto &children = self->d->parentInfo->children;
        s << "children.......... [" << children.size() << "] ";
        int n = 0;
        for (SbkObject *sbkChild : children) {
            auto *obChild = reinterpret_cast<PyObject *>(sbkChild);
            if (n++ > 5) {
                s << "...";
                break;
            }
            s << '<' << Py_TYPE(obChild)->tp_name << " at " << obChild << "> ";
        }
        s << '\n';
    }

    if (self->d->referredObjects && !self->d->referredObjects->empty()) {
        const Shiboken::RefCountMap &map = *self->d->referredObjects;
        s << "referred objects.. ";
        std::string lastKey;
        for (const auto &p : map) {
            if (p.first != lastKey) {
                if (!lastKey.empty())
                    s << "                   ";
                s << '"' << p.first << "\" => ";
                lastKey = p.first;
            }
            s << '<' << Py_TYPE(p.second)->tp_name << " at " << p.second << "> ";
        }
        s << '\n';
    }
    return s.str();
}

} // namespace Object

} // namespace Shiboken
