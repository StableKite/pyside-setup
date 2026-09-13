// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "feature_select.h"
#include "basewrapper.h"
#include "pysidestaticstrings.h"
#include "class_property.h"
#include "pysideglobals_p.h"

#include <autodecref.h>
#include <sbkfeature_base.h>
#include <pep384ext.h>
#include <sbkpep.h>
#include <sbkstaticstrings.h>
#include <sbkstring.h>
#include <signature_p.h>

#include <QtCore/qstringlist.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

//////////////////////////////////////////////////////////////////////////////
//
// PYSIDE-1019: Support switchable extensions
//
// This functionality is no longer implemented in the signature module, since
// the PyCFunction getsets do not have to be modified any longer.
// Instead, we simply exchange the complete class dicts. This is done in the
// basewrapper.cpp file and in every generated `tp_(get|set)attro`.
//
// This is the general framework of the switchable extensions.
// A maximum of eight features is planned so far. This seems to be enough.
// More features are possible, but then we must somehow register the
// extra `select_id`s above 255.
//

/*****************************************************************************

    How Does This Feature Selection Work?
    -------------------------------------

The basic idea is to replace the `tp_dict` of a QObject derived type.
This way, we can replace the methods of the class in no time.

The crucial point to understand is how the `tp_dict` is actually accessed:
When you type "QObject.__dict__", the descriptor of `SbkObjectType_Type`
is called. This descriptor is per default unassigned, so the base class
PyType_Type provides the tp_getset method `type_dict`:

    static PyObject *
    type_dict(PyTypeObject *type, void *context)
    {
        if (type->tp_dict == NULL) {
            Py_RETURN_NONE;
        }
        return PyDictProxy_New(type->tp_dict);
    }

In order to change that, we need to insert our own version into SbkObjectType:

    static PyObject *Sbk_TypeGet___dict__(PyTypeObject *type, void *context)
    {
        auto dict = type->tp_dict;
        if (dict == NULL)
            Py_RETURN_NONE;
        if (SelectFeatureSet != nullptr)
            dict = SelectFeatureSet(type);
        return PyDictProxy_New(dict);
    }

This way, the Python function `type_ready()` does not fill in the default,
but uses our modified version. It a similar way, we overwrite type_getattro
with our own version, again in SbkObjectType, replacing the default of
PyType_Type.

Now we can exchange the dict with a customized version.
We have our own derived type `ChameleonDict` with additional attributes.
These allow us to create a ring of dicts which can be rotated to the actual
needed dict version:

Every dict has a field `select_id` which is selected by the `from __feature__`
import. The dicts are cyclic connected by the `dict_ring` field.

When a class dict is required, now always `SelectFeatureSet` is called, which
looks into the `__name__` attribute of the active module and decides which
version of `tp_dict` is needed. Then the right dict is searched in the ring
and created if not already there.

Furthermore, we need to overwrite every `tp_(get|set)attro`  with a version
that switches dicts right before looking up methods.
The dict changing must walk the whole `tp_mro` in order to change all names.

This is everything that the following code does.

*****************************************************************************/


namespace PySide::Feature {

using namespace Shiboken;

using FeatureProc = bool(*)(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);

#ifdef Py_GIL_DISABLED
static std::atomic<FeatureProc *> featurePointer{nullptr};
static std::atomic<unsigned> featureSelectionGeneration{1};
static FeatureProc *currentFeaturePointer()
{
    return featurePointer.load(std::memory_order_acquire);
}
static void setFeaturePointer(FeatureProc *value)
{
    featurePointer.store(value, std::memory_order_release);
}
#else
static FeatureProc *featurePointer = nullptr;
static FeatureProc *currentFeaturePointer() { return featurePointer; }
static void setFeaturePointer(FeatureProc *value) { featurePointer = value; }
#endif

// Create a derived dict class
static PyTypeObject *
createDerivedDictType()
{
    // It is not easy to create a compatible dict object with the
    // limited API. Easier is to use Python to create a derived
    // type and to modify that a bit from the C code.
    PyObject *ChameleonDict = PepRun_GetResult(R"CPP(if True:

        class ChameleonDict(dict):
            __slots__ = ("dict_ring", "select_id", "orig_dict")

        result = ChameleonDict

        )CPP");
    return reinterpret_cast<PyTypeObject *>(ChameleonDict);
}

static PyTypeObject *ensureNewDictType()
{
    auto *globals = PySide::globals();
    if (globals->newFeatureDictType == nullptr) {
        globals->newFeatureDictType = createDerivedDictType();
        if (globals->newFeatureDictType == nullptr) {
            PyErr_Print();
            Py_FatalError("libshiboken: Problem creating ChameleonDict");
        }
    }
    return globals->newFeatureDictType;
}

static inline PyObject *nextInCircle(PyObject *dict)
{
    // returns a borrowed ref
    AutoDecRef next_dict(PyObject_GetAttr(dict, PySideName::dict_ring()));
    return next_dict;
}

static inline void setNextDict(PyObject *dict, PyObject *next_dict)
{
    PyObject_SetAttr(dict, PySideName::dict_ring(), next_dict);
}

static inline void setSelectId(PyObject *dict, int select_id)
{
    AutoDecRef pySelectId(PyLong_FromLong(select_id));
    if (!pySelectId.isNull())
        PyObject_SetAttr(dict, PySideName::select_id(), pySelectId.object());
}

static inline int getSelectId(PyObject *dict)
{
    auto *py_select_id = PyObject_GetAttr(dict, PyName::select_id());
    if (py_select_id == nullptr) {
        PyErr_Clear();
        return 0;
    }
    int ret = PyLong_AsLong(py_select_id);
    Py_DECREF(py_select_id);
    return ret;
}

static bool replaceClassDict(PyTypeObject *type)
{
    /*
     * Replace the type dict by the derived ChameleonDict.
     * This is mandatory for all type dicts when they are touched.
     */
    auto *ob_ndt = reinterpret_cast<PyObject *>(ensureNewDictType());
    AutoDecRef dict(PepType_GetDict(type));
    auto *new_dict = PyObject_CallObject(ob_ndt, nullptr);
    if (new_dict == nullptr || PyDict_Update(new_dict, dict) < 0)
        return false;
    // Insert the default id. Cannot fail for small numbers.
    setSelectId(new_dict, 0);
    // insert the dict into itself as ring
    setNextDict(new_dict, new_dict);
    // We have now an exact copy of the dict with a new type.
    PepType_SetDict(type, new_dict);
    // PYSIDE-2404: Retain the original dict for easy late init.
    PyObject_SetAttr(new_dict, PySideName::orig_dict(), dict);
    return true;
}

static bool addNewDict(PyTypeObject *type, int select_id)
{
    /*
     * Add a new dict to the ring and set it as `type->tp_dict`.
     * A 'false' return is fatal.
     */
    AutoDecRef dict(PepType_GetDict(type));
    AutoDecRef orig_dict(PyObject_GetAttr(dict, PySideName::orig_dict()));
    auto *ob_ndt = reinterpret_cast<PyObject *>(ensureNewDictType());
    auto *new_dict = PyObject_CallObject(ob_ndt, nullptr);
    if (new_dict == nullptr)
        return false;
    setSelectId(new_dict, select_id);
    // insert the dict into the ring
    auto *next_dict = nextInCircle(dict);
    setNextDict(dict, new_dict);
    setNextDict(new_dict, next_dict);
    PepType_SetDict(type, new_dict);
    // PYSIDE-2404: Retain the original dict for easy late init.
    PyObject_SetAttr(new_dict, PySideName::orig_dict(), orig_dict);
    return true;
}

static inline bool moveToFeatureSet(PyTypeObject *type, int select_id)
{
    /*
     * Rotate the ring to the given `select_id` and return `true`.
     * If not found, stay at the current position and return `false`.
     */
    AutoDecRef tpDict(PepType_GetDict(type));
    auto *initial_dict = tpDict.object();
    auto *dict = initial_dict;
    do {
        int current_id = getSelectId(dict);
        // This works because small numbers are singleton objects.
        if (current_id == select_id) {
            PepType_SetDict(type, dict);
            return true;
        }
        dict = nextInCircle(dict);
    } while (dict != initial_dict);
    PepType_SetDict(type, initial_dict);
    return false;
}

static bool createNewFeatureSet(PyTypeObject *type, int select_id)
{
    /*
     * Create a new feature set.
     * A `false` return value is a fatal error.
     *
     * A FeatureProc sees an empty `type->tp_dict` and the previous dict
     * content in `prev_dict`. It is responsible of filling `type->tp_dict`
     * with modified content.
     */

    bool ok = moveToFeatureSet(type, 0);
    Q_UNUSED(ok);
    assert(ok);

    AutoDecRef prev_dict(PepType_GetDict(type));
    if (!addNewDict(type, select_id))
        return false;
    int id = select_id;
    if (id == -1)
        return false;
    FeatureProc *proc = currentFeaturePointer();
    for (int idx = id; *proc != nullptr; ++proc, idx >>= 1) {
        if (idx & 1) {
            // clear the tp_dict that will get new content
            AutoDecRef tpDict(PepType_GetDict(type));
            PyDict_Clear(tpDict);
            // let the proc re-fill the tp_dict
            if (!(*proc)(type, tpDict.object(), prev_dict, id))
                return false;
            // if there is still a step, prepare `prev_dict`
            if (idx >> 1) {
                prev_dict.reset(PyDict_Copy(tpDict.object()));
                if (prev_dict.isNull())
                    return false;
            }
        }
    }
    return true;
}

static inline void SelectFeatureSetSubtype(PyTypeObject *type, int select_id)
{
    /*
     * This is the selector for one sublass. We need to call this for
     * every subclass until no more subclasses or reaching the wanted id.
     */
    static auto *pyTypeType_tp_dict = PepType_GetDict(&PyType_Type);
    AutoDecRef tpDict(PepType_GetDict(type));
    if (Py_TYPE(tpDict.object()) == Py_TYPE(pyTypeType_tp_dict)) {
        // On first touch, we initialize the dynamic naming.
        // The dict type will be replaced after the first call.
        if (!replaceClassDict(type)) {
            Py_FatalError("libshiboken: failed to replace class dict!");
            return;
        }
    }
    if (!moveToFeatureSet(type, select_id)) {
        if (!createNewFeatureSet(type, select_id)) {
            PyErr_Print();
            Py_FatalError("libshiboken: failed to create a new feature set!");
            return;
        }
    }
 }

static inline int getFeatureSelectId()
{
#ifdef Py_GIL_DISABLED
    static thread_local int lastSelectId = 0;
    static thread_local unsigned seenGeneration = 0;
    static thread_local int64_t seenInterpreter = -1;

    // An OS thread can be attached to different interpreters over its lifetime.
    // Do not let the module-selection cache from one interpreter leak into the
    // next one merely because no process-wide feature generation changed.
    auto *threadState = PyThreadState_Get();
    const int64_t interpreter =
        PyInterpreterState_GetID(PyThreadState_GetInterpreter(threadState));
    if (seenInterpreter != interpreter) {
        seenInterpreter = interpreter;
        seenGeneration = 0;
        lastSelectId = 0;
    }

    const unsigned generation = featureSelectionGeneration.load(std::memory_order_acquire);
    if (seenGeneration != generation) {
        seenGeneration = generation;
        lastSelectId = 0;
    }

    auto *featureDict = PySide::globals()->featureDict;
    if (featureDict == nullptr)
        return lastSelectId;

    AutoDecRef frameGlobals(PepEval_GetFrameGlobals());
    if (frameGlobals.isNull())
        return lastSelectId;

    PyObject *modNameRaw = nullptr;
    const int haveModName = PyDict_GetItemRef(frameGlobals.object(), PyMagicName::name(),
                                              &modNameRaw);
    if (haveModName <= 0) {
        if (haveModName < 0)
            PyErr_Clear();
        return lastSelectId;
    }
    AutoDecRef modName(modNameRaw);

    PyObject *selectIdRaw = nullptr;
    const int haveSelectId = PyDict_GetItemRef(featureDict, modName.object(), &selectIdRaw);
    if (haveSelectId <= 0) {
        if (haveSelectId < 0)
            PyErr_Clear();
        return lastSelectId;
    }
    AutoDecRef pySelectId(selectIdRaw);
    if (!PyLong_Check(pySelectId.object()))
        return lastSelectId;

    const long value = PyLong_AsLong(pySelectId.object());
    if (value < 0 || (value == -1 && PyErr_Occurred())) {
        PyErr_Clear();
        return lastSelectId;
    }
    lastSelectId = int(value) & 0xff;
    return lastSelectId;
#else
    static auto *undef = PyLong_FromLong(-1);

    auto *libGlobals = PySide::globals();
    PyObject *&feature_dict = PySide::globals()->featureDict;
    if (feature_dict == nullptr)
        feature_dict = GetFeatureDict();
    PyObject *&cached_globals = libGlobals->cachedFeatureGlobals;
    int &last_select_id = libGlobals->lastSelectedFeatureId;

    Shiboken::AutoDecRef globals(PepEval_GetFrameGlobals());
    if (globals.isNull() || globals.object() == cached_globals)
        return last_select_id;

    auto *modname = PyDict_GetItem(globals.object(), PyMagicName::name());
    if (modname == nullptr)
        return last_select_id;

    auto *py_select_id = PyDict_GetItem(feature_dict, modname);
    if (py_select_id == nullptr
        || !PyLong_Check(py_select_id)
        || py_select_id == undef)
        return last_select_id;

    cached_globals = globals;
    last_select_id = PyLong_AsLong(py_select_id) & 0xff;
    return last_select_id;
#endif
}

#ifdef Py_GIL_DISABLED

struct FeatureProxyObject
{
    PyObject_HEAD
    PyObject *ownerWeakRef;
    PyObject *name;
};

static std::recursive_mutex featureMutationMutex;
static std::atomic<bool> featureInfrastructureReady{false};

static bool isFeatureProxy(PyObject *object)
{
    auto *type = PySide::globals()->featureProxyType;
    return type != nullptr && Py_TYPE(object) == type;
}

static void featureProxyDealloc(PyObject *self)
{
    auto *proxy = reinterpret_cast<FeatureProxyObject *>(self);
    Py_XDECREF(proxy->ownerWeakRef);
    Py_XDECREF(proxy->name);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *featureProxyDescrGet(PyObject *self, PyObject *obj, PyObject *typeObject)
{
    auto *proxy = reinterpret_cast<FeatureProxyObject *>(self);
    AutoDecRef owner(PyObject_CallNoArgs(proxy->ownerWeakRef));
    if (owner.isNull())
        return nullptr;
    if (owner.object() == Py_None) {
        PyErr_SetString(PyExc_ReferenceError, "feature proxy owner no longer exists");
        return nullptr;
    }

    PyTypeObject *runtimeType = nullptr;
    if (obj != nullptr) {
        runtimeType = PyType_Check(obj)
            ? reinterpret_cast<PyTypeObject *>(obj) : Py_TYPE(obj);
    } else if (typeObject != nullptr && PyType_Check(typeObject)) {
        runtimeType = reinterpret_cast<PyTypeObject *>(typeObject);
    } else {
        runtimeType = reinterpret_cast<PyTypeObject *>(owner.object());
    }

    PyObject *mroRaw = nullptr;
    Py_BEGIN_CRITICAL_SECTION(reinterpret_cast<PyObject *>(runtimeType));
    mroRaw = Py_XNewRef(runtimeType->tp_mro);
    Py_END_CRITICAL_SECTION();
    AutoDecRef mro(mroRaw);
    if (mro.isNull()) {
        PyErr_SetString(PyExc_RuntimeError, "feature proxy runtime type has no MRO");
        return nullptr;
    }
    bool seenOwner = false;
    const Py_ssize_t size = PyTuple_Size(mro.object());
    for (Py_ssize_t i = 0; i < size; ++i) {
        auto *baseObject = PyTuple_GetItem(mro.object(), i); // mro is a private strong snapshot
        if (baseObject == owner.object())
            seenOwner = true;
        if (!seenOwner || !PyType_Check(baseObject))
            continue;
        auto *base = reinterpret_cast<PyTypeObject *>(baseObject);
        AutoDecRef dict(SbkObjectType_GetFeatureDict(base));
        if (dict.isNull())
            return nullptr;
        PyObject *value = nullptr;
        const int found = PyDict_GetItemRef(dict.object(), proxy->name, &value);
        if (found < 0)
            return nullptr;
        if (found == 0)
            continue;
        AutoDecRef descriptor(value);
        if (descriptor.object() == self)
            continue;
        if (auto get = PepExt_Type_GetDescrGetSlot(Py_TYPE(descriptor.object())))
            return get(descriptor.object(), obj, reinterpret_cast<PyObject *>(runtimeType));
        return descriptor.release();
    }

    PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U'",
                 runtimeType->tp_name, proxy->name);
    return nullptr;
}

static PyTypeObject *createFeatureProxyType()
{
    static PyType_Slot slots[] = {
        {Py_tp_dealloc, reinterpret_cast<void *>(featureProxyDealloc)},
        {Py_tp_descr_get, reinterpret_cast<void *>(featureProxyDescrGet)},
        {0, nullptr}
    };
    static PyType_Spec spec = {
        "PySide6._FeatureDescriptorProxy",
        sizeof(FeatureProxyObject),
        0,
        Py_TPFLAGS_DEFAULT,
        slots
    };
    return reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&spec));
}

static bool ensureFeatureInfrastructure()
{
    // FinalizeType() can run before the first __feature__ import and can be
    // reached concurrently from type materialization. Publish the three
    // process-lifetime objects as one initialized state; afterwards the hot
    // path is lock-free.
    if (featureInfrastructureReady.load(std::memory_order_acquire))
        return true;

    std::lock_guard<std::recursive_mutex> guard(featureMutationMutex);
    if (featureInfrastructureReady.load(std::memory_order_relaxed))
        return true;

    auto *globals = PySide::globals();
    if (globals->featureVariantCache == nullptr)
        globals->featureVariantCache = PyDict_New();
    if (globals->featureBaseDictCache == nullptr)
        globals->featureBaseDictCache = PyDict_New();
    if (globals->featureProxyType == nullptr)
        globals->featureProxyType = createFeatureProxyType();
    const bool ready = globals->featureVariantCache != nullptr
        && globals->featureBaseDictCache != nullptr
        && globals->featureProxyType != nullptr;
    if (ready)
        featureInfrastructureReady.store(true, std::memory_order_release);
    return ready;
}

static PyObject *BaseFeatureDict(PyTypeObject *type)
{
    if (!ensureFeatureInfrastructure())
        return nullptr;
    auto *cache = PySide::globals()->featureBaseDictCache;
    PyObject *result = nullptr;
    const int found = PyDict_GetItemRef(cache, reinterpret_cast<PyObject *>(type), &result);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return result;
    return PepType_GetDict(type);
}

static PyObject *newFeatureProxy(PyTypeObject *owner, PyObject *name)
{
    auto *type = PySide::globals()->featureProxyType;
    if (type == nullptr)
        return nullptr;
    auto *proxy = reinterpret_cast<FeatureProxyObject *>(type->tp_alloc(type, 0));
    if (proxy == nullptr)
        return nullptr;
    proxy->ownerWeakRef = PyWeakref_NewRef(reinterpret_cast<PyObject *>(owner), nullptr);
    proxy->name = Py_NewRef(name);
    if (proxy->ownerWeakRef == nullptr) {
        Py_DECREF(reinterpret_cast<PyObject *>(proxy));
        return nullptr;
    }
    return reinterpret_cast<PyObject *>(proxy);
}

static bool typeNeedsFeatureVariant(PyTypeObject *type)
{
    if (!SbkObjectType_Check(type))
        return false;
    return type->tp_methods != nullptr || SbkObjectType_GetPropertyStrings(type) != nullptr;
}

static PyObject *newFeatureVariant(PyObject *origDict, int selectId)
{
    auto *obNdt = reinterpret_cast<PyObject *>(ensureNewDictType());
    auto *dict = PyObject_CallObject(obNdt, nullptr);
    if (dict == nullptr)
        return nullptr;
    setSelectId(dict, selectId);
    if (PyErr_Occurred()) {
        Py_DECREF(dict);
        return nullptr;
    }
    setNextDict(dict, dict);
    if (PyErr_Occurred() || PyObject_SetAttr(dict, PySideName::orig_dict(), origDict) < 0) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

static PyObject *buildFeatureVariant(PyTypeObject *type, int selectId)
{
    AutoDecRef original(BaseFeatureDict(type));
    if (original.isNull())
        return nullptr;
    if (selectId == 0 || !typeNeedsFeatureVariant(type))
        return original.release();

    AutoDecRef previous(PyDict_Copy(original.object()));
    if (previous.isNull())
        return nullptr;

    int remaining = selectId;
    FeatureProc *proc = currentFeaturePointer();
    for (; *proc != nullptr; ++proc, remaining >>= 1) {
        if ((remaining & 1) == 0)
            continue;
        AutoDecRef target(newFeatureVariant(original.object(), selectId));
        if (target.isNull())
            return nullptr;
        if (!(*proc)(type, target.object(), previous.object(), selectId))
            return nullptr;
        previous.reset(target.release());
    }
    return previous.release();
}

static PyObject *featureVariantCacheForType(PyTypeObject *type)
{
    auto *cache = PySide::globals()->featureVariantCache;
    if (cache == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "feature variant cache is not initialized");
        return nullptr;
    }

    PyObject *perTypeRaw = nullptr;
    int found = PyDict_GetItemRef(cache, reinterpret_cast<PyObject *>(type), &perTypeRaw);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return perTypeRaw;

    AutoDecRef candidate(PyDict_New());
    if (candidate.isNull())
        return nullptr;
    PyObject *published = nullptr;
    if (PyDict_SetDefaultRef(cache, reinterpret_cast<PyObject *>(type), candidate.object(),
                             &published) < 0)
        return nullptr;
    return published;
}

static PyObject *SelectFeatureDict(PyTypeObject *type)
{
    const int selectId = getFeatureSelectId();
    if (selectId == 0 || !typeNeedsFeatureVariant(type))
        return BaseFeatureDict(type);

    AutoDecRef perType(featureVariantCacheForType(type));
    if (perType.isNull())
        return nullptr;
    AutoDecRef key(PyLong_FromLong(selectId));
    if (key.isNull())
        return nullptr;

    PyObject *cached = nullptr;
    int found = PyDict_GetItemRef(perType.object(), key.object(), &cached);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return cached;

    // Feature processors can call into signature/property machinery. Serialize only
    // the cache-miss construction; already published immutable variants remain lock-free.
    std::lock_guard<std::recursive_mutex> guard(featureMutationMutex);
    found = PyDict_GetItemRef(perType.object(), key.object(), &cached);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return cached;

    AutoDecRef candidate(buildFeatureVariant(type, selectId));
    if (candidate.isNull())
        return nullptr;
    if (PyDict_SetItem(perType.object(), key.object(), candidate.object()) < 0)
        return nullptr;
    return candidate.release();
}
#endif


#ifdef Py_GIL_DISABLED
static bool addChangedFeatureNames(PyObject *names, PyObject *base, PyObject *variant)
{
    PyObject *key = nullptr;
    PyObject *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(base, &pos, &key, &value)) {
        PyObject *other = nullptr;
        const int found = PyDict_GetItemRef(variant, key, &other);
        if (found < 0)
            return false;
        AutoDecRef otherRef(other);
        if (found == 0 || other != value) {
            if (PySet_Add(names, key) < 0)
                return false;
        }
    }
    pos = 0;
    while (PyDict_Next(variant, &pos, &key, &value)) {
        PyObject *other = nullptr;
        const int found = PyDict_GetItemRef(base, key, &other);
        if (found < 0)
            return false;
        AutoDecRef otherRef(other);
        if (found == 0 || other != value) {
            if (PySet_Add(names, key) < 0)
                return false;
        }
    }
    return true;
}

static bool restoreCanonicalProxies(PyTypeObject *type, PyObject *base)
{
    AutoDecRef canonical(PepType_GetDict(type));
    if (canonical.isNull())
        return false;
    AutoDecRef snapshot(PyDict_Copy(canonical.object()));
    if (snapshot.isNull())
        return false;
    PyObject *key = nullptr;
    PyObject *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(snapshot.object(), &pos, &key, &value)) {
        if (!isFeatureProxy(value))
            continue;
        PyObject *baseValue = nullptr;
        const int found = PyDict_GetItemRef(base, key, &baseValue);
        if (found < 0)
            return false;
        AutoDecRef baseValueRef(baseValue);
        if (found > 0) {
            if (PyDict_SetItem(canonical.object(), key, baseValue) < 0)
                return false;
        } else if (PyDict_DelItem(canonical.object(), key) < 0
                   && !PyErr_ExceptionMatches(PyExc_KeyError)) {
            return false;
        } else {
            PyErr_Clear();
        }
    }
    return true;
}

static bool installFeatureProxies(PyTypeObject *type)
{
    if (currentFeaturePointer() == nullptr || !typeNeedsFeatureVariant(type))
        return true;

    AutoDecRef base(BaseFeatureDict(type));
    if (base.isNull())
        return false;
    AutoDecRef names(PySet_New(nullptr));
    if (names.isNull())
        return false;
    AutoDecRef perType(featureVariantCacheForType(type));
    if (perType.isNull())
        return false;

    // 1/2 are the real public features; 3 covers their chained interaction.
    // The remaining single bits are the feature-test placeholders.
    static constexpr int ids[] = {1, 2, 3, 4, 8, 16, 32, 64, 128};
    for (int id : ids) {
        AutoDecRef key(PyLong_FromLong(id));
        AutoDecRef variant(buildFeatureVariant(type, id));
        if (key.isNull() || variant.isNull())
            return false;
        if (PyDict_SetItem(perType.object(), key.object(), variant.object()) < 0
            || !addChangedFeatureNames(names.object(), base.object(), variant.object())) {
            return false;
        }
    }

    AutoDecRef canonical(PepType_GetDict(type));
    if (canonical.isNull())
        return false;
    AutoDecRef iter(PyObject_GetIter(names.object()));
    if (iter.isNull())
        return false;
    while (PyObject *name = PyIter_Next(iter.object())) {
        AutoDecRef nameRef(name);
        AutoDecRef proxy(newFeatureProxy(type, name));
        if (proxy.isNull() || PyDict_SetItem(canonical.object(), name, proxy.object()) < 0)
            return false;
    }
    if (PyErr_Occurred())
        return false;
    PyType_Modified(type);
    return true;
}

static bool setBaseSnapshot(PyTypeObject *type, PyObject *base)
{
    return PyDict_SetItem(PySide::globals()->featureBaseDictCache,
                          reinterpret_cast<PyObject *>(type), base) == 0;
}

static bool rebuildFeatureType(PyTypeObject *type, bool refreshBase)
{
    if (!ensureFeatureInfrastructure())
        return false;
    std::lock_guard<std::recursive_mutex> guard(featureMutationMutex);

    auto *baseCache = PySide::globals()->featureBaseDictCache;
    PyObject *oldBaseRaw = nullptr;
    int haveBase = PyDict_GetItemRef(baseCache, reinterpret_cast<PyObject *>(type), &oldBaseRaw);
    if (haveBase < 0)
        return false;
    AutoDecRef oldBase(oldBaseRaw);

    if (haveBase > 0 && !restoreCanonicalProxies(type, oldBase.object()))
        return false;

    if (refreshBase || haveBase == 0) {
        AutoDecRef canonical(PepType_GetDict(type));
        if (canonical.isNull())
            return false;
        AutoDecRef newBase(PyDict_Copy(canonical.object()));
        if (newBase.isNull() || !setBaseSnapshot(type, newBase.object()))
            return false;
    }

    auto *variantCache = PySide::globals()->featureVariantCache;
    if (PyDict_DelItem(variantCache, reinterpret_cast<PyObject *>(type)) < 0) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError))
            return false;
        PyErr_Clear();
    }
    return installFeatureProxies(type);
}

#endif // Py_GIL_DISABLED

[[maybe_unused]] static inline void SelectFeatureSet(PyTypeObject *type)
{
    /*
     * This is the main function of the module.
     * The purpose of this function is to switch the dict of a class right
     * before a (get|set)attro call is performed.
     *
     * Generated functions call this directly.
     * Shiboken will assign it via a public hook of `basewrapper.cpp`.
     */
    static auto *pyTypeType_tp_dict = PepType_GetDict(&PyType_Type);
    AutoDecRef tpDict(PepType_GetDict(type));
    if (Py_TYPE(tpDict.object()) == Py_TYPE(pyTypeType_tp_dict)) {
        // We initialize the dynamic features by using our own dict type.
        if (!replaceClassDict(type)) {
            Py_FatalError("libshiboken: failed to replace class dict!");
            return;
        }
    }

    int select_id = getFeatureSelectId();
    static int last_select_id{};
    PyTypeObject *&last_type = PySide::globals()->lastFeatureType;

    // PYSIDE-2029: Implement a very simple but effective cache that cannot fail.
    if (type == last_type && select_id == last_select_id)
        return;
    last_type = type;
    last_select_id = select_id;

    auto *mro = type->tp_mro;
    const Py_ssize_t n = PyTuple_Size(mro);
    // We leave 'Shiboken.Object' and 'object' alone, therefore "n - 2".
    for (Py_ssize_t idx = 0; idx < n - 2; idx++) {
        auto *sub_type = reinterpret_cast<PyTypeObject *>(PyTuple_GetItem(mro, idx));
        SelectFeatureSetSubtype(sub_type, select_id);
    }
    // PYSIDE-1436: Clear all caches for the type and subtypes.
    PyType_Modified(type);
}

// For cppgenerator:
void Select(PyObject *obj)
{
#ifndef Py_GIL_DISABLED
    if (featurePointer == nullptr)
        return;
    auto *type = Py_TYPE(obj);
    SelectFeatureSet(type);
#else
    Q_UNUSED(obj);
#endif
}

void Select(PyTypeObject *type)
{
#ifndef Py_GIL_DISABLED
    if (featurePointer != nullptr)
        SelectFeatureSet(type);
#else
    Q_UNUSED(type);
#endif
}

void Invalidate(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    auto *baseCache = PySide::globals()->featureBaseDictCache;
    if (baseCache == nullptr)
        return;
    PyObject *base = nullptr;
    const int found = PyDict_GetItemRef(baseCache, reinterpret_cast<PyObject *>(type), &base);
    Py_XDECREF(base);
    if (found <= 0) {
        if (found < 0)
            PyErr_Clear();
        return;
    }
    if (!rebuildFeatureType(type, true))
        PyErr_WriteUnraisable(reinterpret_cast<PyObject *>(type));
#else
    Q_UNUSED(type);
#endif
}

bool FinalizeType(PyTypeObject *type)
{
#ifdef Py_GIL_DISABLED
    return rebuildFeatureType(type, false);
#else
    Q_UNUSED(type);
    return true;
#endif
}

static bool feature_01_addLowerNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_02_true_property(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_04_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_08_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_10_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_20_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_40_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);
static bool feature_80_addDummyNames(PyTypeObject *type, PyObject *dict, PyObject *prev_dict, int id);

static FeatureProc featureProcArray[] = {
    feature_01_addLowerNames,
    feature_02_true_property,
    feature_04_addDummyNames,
    feature_08_addDummyNames,
    feature_10_addDummyNames,
    feature_20_addDummyNames,
    feature_40_addDummyNames,
    feature_80_addDummyNames,
    nullptr
};

static bool patch_property_impl();
static bool is_initialized = false;

static void featureEnableCallback(bool enable)
{
#ifndef Py_GIL_DISABLED
    setFeaturePointer(enable ? featureProcArray : nullptr);
#else
    Q_UNUSED(enable);
#endif
}

void init()
{
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::recursive_mutex> guard(featureMutationMutex);
#endif
    // This function can be called multiple times.
    if (!is_initialized) {
        setFeaturePointer(featureProcArray);
#ifdef Py_GIL_DISABLED
        auto *globals = PySide::globals();
        if (!ensureFeatureInfrastructure())
            Py_FatalError("libpyside: failed to initialize feature state");
        if (globals->featureDict == nullptr)
            globals->featureDict = GetFeatureDict();
        if (globals->featureDict == nullptr)
            Py_FatalError("libpyside: failed to obtain the feature dictionary");
        initSelectableFeature(nullptr);
        initSelectableFeatureBaseDict(BaseFeatureDict);
        initSelectableFeatureDict(SelectFeatureDict);
        initSelectableFeatureUpdate(Invalidate);
#else
        initSelectableFeature(SelectFeatureSet);
#endif
        setSelectableFeatureCallback(featureEnableCallback);
        patch_property_impl();
        is_initialized = true;

#ifdef Py_GIL_DISABLED
        // Types can be materialized before the first __feature__ import. Their base
        // snapshots were captured by FinalizeType(); install canonical bridge proxies now.
        AutoDecRef types(PyDict_Keys(globals->featureBaseDictCache));
        if (types.isNull())
            Py_FatalError("libpyside: failed to enumerate feature base dictionaries");
        const Py_ssize_t size = PyList_Size(types.object());
        for (Py_ssize_t i = 0; i < size; ++i) {
            auto *typeObject = PyList_GetItem(types.object(), i); // private list snapshot
            if (PyType_Check(typeObject)
                && !rebuildFeatureType(reinterpret_cast<PyTypeObject *>(typeObject), true)) {
                PyErr_Print();
                Py_FatalError("libpyside: failed to finalize feature state");
            }
        }
#endif
    }

    // Reset the selection cache. This is called at any "from __feature__ import".
#ifdef Py_GIL_DISABLED
    featureSelectionGeneration.fetch_add(1, std::memory_order_release);
#else
    auto *globals = PySide::globals();
    globals->lastSelectedFeatureId = 0;
    globals->cachedFeatureGlobals = nullptr;
#endif
}

void Enable(bool enable)
{
    if (!is_initialized)
        return;
#ifdef Py_GIL_DISABLED
    if (enable)
        SbkObjectType_PopFeatureDisable();
    else
        SbkObjectType_PushFeatureDisable();
#else
    setFeaturePointer(enable ? featureProcArray : nullptr);
    initSelectableFeature(enable ? SelectFeatureSet : nullptr);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// PYSIDE-1019: Support switchable extensions
//
//     Feature 0x01: Allow snake_case instead of camelCase
//
// This functionality is no longer implemented in the signature module, since
// the PyCFunction getsets do not have to be modified any longer.
// Instead, we simply exchange the complete class dicts. This is done in the
// basewrapper.cpp file.
//

static PyObject *methodWithNewName(PyTypeObject *type,
                                   PyMethodDef *meth,
                                   const char *new_name)
{
    /*
     * Create a method with a lower case name.
     */
    auto *obtype = reinterpret_cast<PyObject *>(type);
    const auto len = std::strlen(new_name);
    auto *name = new char[len + 1];
    std::strcpy(name, new_name);
    auto *new_meth = new PyMethodDef;
    new_meth->ml_name = name;
    new_meth->ml_meth = meth->ml_meth;
    new_meth->ml_flags = meth->ml_flags;
    new_meth->ml_doc = meth->ml_doc;
    if ((new_meth->ml_flags & METH_STATIC) != 0) {
        AutoDecRef cfunc(PyCFunction_NewEx(new_meth, obtype, nullptr));
        return cfunc.isNull() ? nullptr : PyStaticMethod_New(cfunc);
    }
    if ((new_meth->ml_flags & METH_CLASS) != 0)
        return PyDescr_NewClassMethod(type, new_meth);
    return PyDescr_NewMethod(type, new_meth);
}

static bool feature_01_addLowerNames(PyTypeObject *type, PyObject *lower_dict,
                                     PyObject *prev_dict, int /* id */)
{
    PyMethodDef *meth = type->tp_methods;

    // PYSIDE-1702: A user-defined class in Python has no internal method list.
    //              We are not going to change anything.
    if (!meth)
        return PyDict_Update(lower_dict, prev_dict) >= 0;

    /*
     * Add objects with lower names to `type->tp_dict` from 'prev_dict`.
     */
    PyObject *key{};
    PyObject *value{};
    Py_ssize_t pos = 0;

    // We first copy the things over which will not be changed:
    while (PyDict_Next(prev_dict, &pos, &key, &value)) {
        if (Py_TYPE(value) != PepMethodDescr_TypePtr
            && Py_TYPE(value) != PepStaticMethod_TypePtr) {
            if (PyDict_SetItem(lower_dict, key, value))
                return false;
            continue;
        }
    }

    // Then we walk over the tp_methods to get all methods and insert
    // them with changed names.

    for (; meth != nullptr && meth->ml_name != nullptr; ++meth) {
        PyObject *oldMethod = prev_dict ? PyDict_GetItemString(prev_dict, meth->ml_name) : nullptr;
        const char *newName = String::toCString(String::getSnakeCaseName(meth->ml_name, true));
        if (oldMethod != nullptr && PyCallable_Check(oldMethod) != 0) {
            if (PyDict_SetItemString(lower_dict, newName, oldMethod) < 0)
                return false;
        } else {
            // Code path appears to be obsolete?
            AutoDecRef new_method(methodWithNewName(type, meth, newName));
            if (new_method.isNull())
                return false;
            if (PyDict_SetItemString(lower_dict, newName, new_method) < 0)
                return false;
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// PYSIDE-1019: Support switchable extensions
//
//     Feature 0x02: Use true properties instead of getters and setters
//

// This is the Python 2 version for inspection of m_ml, only.
// The actual Python 3 version is larget.

struct PyCFunctionObject {
    PyObject_HEAD
    PyMethodDef *m_ml; /* Description of the C function to call */
    PyObject    *m_self; /* Passed as 'self' arg to the C func, can be NULL */
    PyObject    *m_module; /* The __module__ attribute, can be anything */
};

static PyObject *modifyStaticToClassMethod(PyTypeObject *type, PyObject *sm)
{
    AutoDecRef func_ob(PyObject_GetAttr(sm, PyMagicName::func()));
    if (func_ob.isNull())
        return nullptr;
    auto *func = reinterpret_cast<PyCFunctionObject *>(func_ob.object());
    auto *new_func = new PyMethodDef;
    new_func->ml_name = func->m_ml->ml_name;
    new_func->ml_meth = func->m_ml->ml_meth;
    new_func->ml_flags = (func->m_ml->ml_flags & ~METH_STATIC) | METH_CLASS;
    new_func->ml_doc = func->m_ml->ml_doc;
    PyCFunction_NewEx(new_func, nullptr, nullptr);
    return PyDescr_NewClassMethod(type, new_func);
}

static PyObject *createProperty(PyTypeObject *type, PyObject *getter, PyObject *setter)
{
    assert(getter != nullptr);
    if (setter == nullptr)
        setter = Py_None;
    auto *ptype = &PyProperty_Type;
    if (Py_TYPE(getter) == PepStaticMethod_TypePtr) {
        ptype = PyClassProperty_TypeF();
        getter = modifyStaticToClassMethod(type, getter);
        if (setter != Py_None)
            setter = modifyStaticToClassMethod(type, setter);
    }
    auto *obtype = reinterpret_cast<PyObject *>(ptype);
    PyObject *prop = PyObject_CallFunctionObjArgs(obtype, getter, setter, nullptr);
    return prop;
}

static QByteArrayList parseFields(const char *propStr, bool *stdWrite)
{
    /*
     * Break the string into subfields at ':' and add defaults.
     */
    if (stdWrite)
        *stdWrite = true;
    QByteArray s = QByteArray(propStr);
    auto list = s.split(u':');
    assert(list.size() == 2 || list.size() == 3);
    auto name = list[0];
    auto read = list[1];
    if (read.isEmpty())
        list[1] = name;
    if (list.size() == 2)
        return list;
    auto write = list[2];
    if (stdWrite)
        *stdWrite = write.isEmpty();
    if (write.isEmpty()) {
        list[2] = "set" + name;
        list[2][3] = std::toupper(list[2][3]);
    }
    return list;
}

static PyObject *make_snake_case(const QByteArray &s, bool lower)
{
    if (s.isNull())
        return nullptr;
    return String::getSnakeCaseName(s.constData(), lower);
}

PyObject *adjustPropertyName(PyObject *dict, PyObject *name)
{
    // PYSIDE-1670: If this is a function with multiple arity or with
    // parameters, we use a mangled name for the property.
    PyObject *existing = PyDict_GetItem(dict, name);    // borrowed
    if (existing) {
        Shiboken::AutoDecRef sig(get_signature_intern(existing, nullptr));
        if (sig.object()) {
            bool name_clash = false;
            if (PyList_CheckExact(sig)) {
                name_clash = true;
            } else {
                Shiboken::AutoDecRef params(PyObject_GetAttr(sig, PySideName::parameters()));
                // Are there parameters except self or cls?
                if (PyObject_Size(params.object()) > 1)
                    name_clash = true;
            }
            if (name_clash) {
                // PyPy has no PyUnicode_AppendAndDel function, yet
                Shiboken::AutoDecRef hold(name);
                Shiboken::AutoDecRef under(Py_BuildValue("s", "_"));
                name = PyUnicode_Concat(hold, under);
            }
        }
    }
    return name;
}

static QByteArrayList GetPropertyStringsMro(PyTypeObject *type)
{
    /*
     * PYSIDE-2042: There are possibly more methods which should become properties,
     *              because the wrapping process does not obey inheritance.
     *              Therefore, we need to walk the mro to find property strings.
     */
    auto res = QByteArrayList();

#ifdef Py_GIL_DISABLED
    PyObject *mroRaw = nullptr;
    Py_BEGIN_CRITICAL_SECTION(reinterpret_cast<PyObject *>(type));
    mroRaw = Py_XNewRef(type->tp_mro);
    Py_END_CRITICAL_SECTION();
    AutoDecRef mroRef(mroRaw);
    PyObject *mro = mroRef.object();
    if (mro == nullptr)
        return res;
#else
    PyObject *mro = type->tp_mro;
#endif
    const Py_ssize_t n = PyTuple_Size(mro);
    // We leave 'Shiboken.Object' and 'object' alone, therefore "n - 2".
    for (Py_ssize_t idx = 0; idx < n - 2; idx++) {
        auto *subType = reinterpret_cast<PyTypeObject *>(PyTuple_GetItem(mro, idx));
        auto *props = SbkObjectType_GetPropertyStrings(subType);
        if (props != nullptr)
            for (; *props != nullptr; ++props)
                res << QByteArray(*props);
    }
    return res;
}

static bool feature_02_true_property(PyTypeObject *type, PyObject *prop_dict,
                                     PyObject *prev_dict, int id)
{
    /*
     * Use the property info to create true Python property objects.
     */

    PyMethodDef *meth = type->tp_methods;

    // The empty `tp_dict` gets populated by the previous dict.
    if (PyDict_Update(prop_dict, prev_dict) < 0)
        return false;

    // PYSIDE-1702: A user-defined class in Python has no internal method list.
    //              We are not going to change anything.
    if (!meth)
        return true;

    // For speed, we establish a helper dict that maps the removed property
    // method names to property name.
    PyObject *prop_methods = PyDict_GetItem(prop_dict, PyMagicName::property_methods());
    if (prop_methods == nullptr) {
        prop_methods = PyDict_New();
        if (prop_methods == nullptr
            || PyDict_SetItem(prop_dict, PyMagicName::property_methods(), prop_methods))
            return false;
    }
    // We then replace methods by properties.
    bool lower = (id & 0x01) != 0;
    auto props = GetPropertyStringsMro(type);
    if (props.isEmpty())
        return true;

    for (const auto &propStr : std::as_const(props)) {
        bool isStdWrite{};
        auto fields = parseFields(propStr, &isStdWrite);
        bool haveWrite = fields.size() == 3;
        PyObject *name = make_snake_case(fields[0], lower);
        PyObject *read = make_snake_case(fields[1], lower);
        PyObject *write = haveWrite ? make_snake_case(fields[2], lower) : nullptr;
        PyObject *getter = PyDict_GetItem(prev_dict, read);
        if (getter == nullptr || !(Py_TYPE(getter) == PepMethodDescr_TypePtr ||
                                   Py_TYPE(getter) == PepStaticMethod_TypePtr))
            continue;
        PyObject *setter = haveWrite ? PyDict_GetItem(prev_dict, write) : nullptr;

        // PYSIDE-1670: If multiple arities exist as a property name, rename it.
        name = adjustPropertyName(prop_dict, name);

        AutoDecRef PyProperty(createProperty(type, getter, setter));
        if (PyProperty.isNull())
            return false;
        if (PyDict_SetItem(prop_dict, name, PyProperty) < 0
            || PyDict_SetItem(prop_methods, read, name) < 0
            || (setter != nullptr && PyDict_SetItem(prop_methods, write, name) < 0))
            return false;
        if (fields[0] != fields[1] && PyDict_GetItem(prop_dict, read))
            if (PyDict_DelItem(prop_dict, read) < 0)
                return false;
        // Theoretically, we need to check for multiple signatures to be exact.
        // But we don't do so intentionally because it would be confusing.
        if (haveWrite && PyDict_GetItem(prop_dict, write) && isStdWrite) {
            if (PyDict_DelItem(prop_dict, write) < 0)
                return false;
        }
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// These are a number of patches to make Python's property object better
// suitable for us.
// We turn `__doc__` into a lazy attribute saving signature initialization.
//
// There is now also a class property implementation which inherits
// from this one.
//

static PyObject *property_doc_get(PyObject *self, void *)
{
    auto *po = reinterpret_cast<propertyobject *>(self);
    PyObject *doc = nullptr;
    PyObject *getter = nullptr;

#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    doc = Py_XNewRef(po->prop_doc);
    if ((doc == nullptr || doc == Py_None) && po->prop_get != nullptr)
        getter = Py_NewRef(po->prop_get);
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif

    if (doc != nullptr && doc != Py_None)
        return doc;
    Py_XDECREF(doc);

    if (getter != nullptr) {
        // PYSIDE-1019: Fetch the default `__doc__` from fget. We do it late.
        AutoDecRef getterRef(getter);
        auto *txt = PyObject_GetAttr(getterRef.object(), PyMagicName::doc());
        if (txt != nullptr) {
            PyObject *oldDoc = nullptr;
            PyObject *result = nullptr;
            bool publishedLazy = false;
#ifdef Py_GIL_DISABLED
            Py_BEGIN_CRITICAL_SECTION(self);
#endif
            if (po->prop_doc == nullptr || po->prop_doc == Py_None) {
                oldDoc = po->prop_doc;
                po->prop_doc = Py_NewRef(txt);
                result = txt;
                publishedLazy = true;
            } else {
                result = Py_NewRef(po->prop_doc);
            }
#ifdef Py_GIL_DISABLED
            Py_END_CRITICAL_SECTION();
#endif
            Py_XDECREF(oldDoc);
            if (!publishedLazy)
                Py_DECREF(txt);
            return result;
        }
        PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static int property_doc_set(PyObject *self, PyObject *value, void *)
{
    if (value == nullptr) {
        PyErr_SetString(PyExc_AttributeError, "cannot delete __doc__");
        return -1;
    }

    auto *po = reinterpret_cast<propertyobject *>(self);
    PyObject *oldDoc = nullptr;
    Py_INCREF(value);
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(self);
#endif
    oldDoc = po->prop_doc;
    po->prop_doc = value;
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    Py_XDECREF(oldDoc);
    return 0;
}

static PyGetSetDef property_getset[] = {
    // This gets added to the existing getsets
    {"__doc__", property_doc_get, property_doc_set, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}
};

static bool patch_property_impl()
{
    // Turn `__doc__` into a computed attribute without changing writability.
    auto *gsp = property_getset;
    auto *type = &PyProperty_Type;
    AutoDecRef dict(PepType_GetDict(type));
    AutoDecRef descr(PyDescr_NewGetSet(type, gsp));
    if (descr.isNull())
        return false;
    if (PyDict_SetItemString(dict.object(), gsp->name, descr) < 0)
        return false;
    PyType_Modified(type);
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// PYSIDE-1019: Support switchable extensions
//
//     Feature 0x04..0x80: A fake switchable option for testing
//

#define SIMILAR_FEATURE(xx)  \
static bool feature_##xx##_addDummyNames(PyTypeObject * /* type */, PyObject *dict, \
                                         PyObject *prev_dict, int /* id */) \
{ \
    if (PyDict_Update(dict, prev_dict) < 0) \
        return false; \
    if (PyDict_SetItemString(dict, "fake_feature_" #xx, Py_None) < 0) \
        return false; \
    return true; \
}

SIMILAR_FEATURE(04)
SIMILAR_FEATURE(08)
SIMILAR_FEATURE(10)
SIMILAR_FEATURE(20)
SIMILAR_FEATURE(40)
SIMILAR_FEATURE(80)

} // namespace PySide::Feature
