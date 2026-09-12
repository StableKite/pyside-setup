// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "sbkenum.h"
#include "sbkenum_p.h"
#include "sbkpep.h"
#include "sbkstring.h"
#include "helper.h"
#include "sbkstaticstrings.h"
#include "sbkfeature_base.h"
#include "sbkstaticstrings_p.h"
#include "sbkconverter.h"
#include "basewrapper_p.h"
#include "autodecref.h"
#include "sbktypefactory.h"

#include <cstring>

#ifdef Py_GIL_DISABLED
#  include <atomic>
#  include <new>
#endif

using namespace Shiboken;

extern "C"
{

struct SbkEnumType
{
    PyTypeObject type;
};

// Initialization
static bool _init_enum()
{
    AutoDecRef shibo(PyImport_ImportModule("shiboken6.Shiboken"));
    return !shibo.isNull();
}

struct EnumGlobalData
{
    PyObject *PyEnumModule{};
    PyTypeObject *PyEnumMeta{};
    PyObject *PyEnum{};
    PyObject *PyIntEnum{};
    PyObject *PyFlag{};
    PyObject *PyIntFlag{};
    PyObject *PyFlag_KEEP{};
};

static bool initEnumGlobals(EnumGlobalData *globals)
{
    auto *mod = PyImport_ImportModule("enum");
    if (mod == nullptr)
        return false;
    globals->PyEnumModule = mod;
    auto *PyEnumMeta = PyObject_GetAttrString(mod, "EnumMeta");
    if (PyEnumMeta == nullptr || PyType_Check(PyEnumMeta) == 0)
        return false;
    globals->PyEnumMeta = reinterpret_cast<PyTypeObject *>(PyEnumMeta);
    globals->PyEnum = PyObject_GetAttrString(mod, "Enum");
    if (globals->PyEnum == nullptr || PyType_Check(globals->PyEnum) == 0)
        return false;
    globals->PyIntEnum = PyObject_GetAttrString(mod, "IntEnum");
    if (globals->PyIntEnum == nullptr || PyType_Check(globals->PyIntEnum) == 0)
        return false;
    globals->PyFlag = PyObject_GetAttrString(mod, "Flag");
    if (globals->PyFlag == nullptr || PyType_Check(globals->PyFlag) == 0)
        return false;
    globals->PyIntFlag = PyObject_GetAttrString(mod, "IntFlag");
    if (globals->PyIntFlag == nullptr || PyType_Check(globals->PyIntFlag) == 0)
        return false;
    // KEEP is defined from Python 3.11 on.
    globals->PyFlag_KEEP = PyObject_GetAttrString(mod, "KEEP");
    PyErr_Clear();
    return true;
}

#ifdef Py_GIL_DISABLED
static std::atomic<EnumGlobalData *> enumGlobalData{nullptr};
static std::atomic<unsigned char> enumInitState{0}; // 0=new, 1=publishing, 2=ready

static void deleteEnumGlobals(EnumGlobalData *globals)
{
    if (globals == nullptr)
        return;
    Py_XDECREF(globals->PyFlag_KEEP);
    Py_XDECREF(globals->PyIntFlag);
    Py_XDECREF(globals->PyFlag);
    Py_XDECREF(globals->PyIntEnum);
    Py_XDECREF(globals->PyEnum);
    Py_XDECREF(reinterpret_cast<PyObject *>(globals->PyEnumMeta));
    Py_XDECREF(globals->PyEnumModule);
    delete globals;
}
#endif

EnumGlobalData *enumGlobals()
{
#ifdef Py_GIL_DISABLED
    if (auto *result = enumGlobalData.load(std::memory_order_acquire))
        return result;

    // Building this table only imports `enum` and takes strong references to
    // process-lifetime objects. Duplicate candidates are therefore harmless;
    // publish one complete table instead of exposing partially initialized
    // fields to another thread.
    auto *candidate = new (std::nothrow) EnumGlobalData;
    if (candidate == nullptr) {
        PyErr_NoMemory();
        return nullptr;
    }
    if (!initEnumGlobals(candidate)) {
        deleteEnumGlobals(candidate);
        return nullptr;
    }
    EnumGlobalData *expected = nullptr;
    if (!enumGlobalData.compare_exchange_strong(expected, candidate,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
        deleteEnumGlobals(candidate);
        return expected;
    }
    return candidate;
#else
    static EnumGlobalData result;
    return &result;
#endif
}

bool PyEnumMeta_Check(PyObject *ob)
{
#ifdef Py_GIL_DISABLED
    // Keep the predicate side-effect free: before enum initialization it simply
    // reports false, as the historical zero-initialized globals did.
    auto *globals = enumGlobalData.load(std::memory_order_acquire);
#else
    auto *globals = enumGlobals();
#endif
    return globals != nullptr && Py_TYPE(ob) == globals->PyEnumMeta;
}

PyTypeObject *getPyEnumMeta()
{
    auto *globals = enumGlobals();
#ifndef Py_GIL_DISABLED
    if (globals->PyEnumMeta == nullptr && !initEnumGlobals(globals)) {
#else
    if (globals == nullptr) {
#endif
        PyErr_Print();
        Py_FatalError("libshiboken: Python module 'enum' not found");
        return nullptr;
    }
    return globals->PyEnumMeta;
}

// PYSIDE-1735: Determine whether we should use the old or the new enum implementation.
static int enumOption()
{
#ifdef Py_GIL_DISABLED
    // PySys_GetObject() returns a borrowed reference. The option is normally
    // fixed during module initialization, but using a strong attribute result
    // also makes an explicit concurrent init_enum() safe.
    AutoDecRef sysModule(PyImport_ImportModule("sys"));
    if (sysModule.isNull()) {
        PyErr_Clear();
        return 1;
    }
    PyObject *optionRaw = nullptr;
    const int found = PyObject_GetOptionalAttrString(sysModule.object(),
                                                     "pyside6_option_python_enum",
                                                     &optionRaw);
    AutoDecRef option(optionRaw);
    if (found > 0 && PyLong_Check(option.object()) != 0) {
        int ignoreOver{};
        return PyLong_AsLongAndOverflow(option.object(), &ignoreOver);
    }
    if (found < 0)
        PyErr_Clear();
#else
    if (PyObject *option = PySys_GetObject("pyside6_option_python_enum")) {
        if (PyLong_Check(option) != 0) {
            int ignoreOver{};
            return PyLong_AsLongAndOverflow(option, &ignoreOver);
        }
    }
    PyErr_Clear();
#endif
    return 1;
}

// Called from init_shibokensupport_module().
void init_enum()
{
#ifdef Py_GIL_DISABLED
    // Do all potentially re-entrant Python work before claiming publication.
    // The publishing state is held only for the two plain stores below, so a
    // losing thread can never block while the winner imports or executes
    // Python code.
    if (enumInitState.load(std::memory_order_acquire) == 2)
        return;

    if (!_init_enum())
        Py_FatalError("libshiboken: could not init enum");
    const int option = enumOption();
    getPyEnumMeta();

    unsigned char expected = 0;
    if (enumInitState.compare_exchange_strong(expected, 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        Enum::enumOption = option;
        enumInitState.store(2, std::memory_order_release);
        return;
    }
    // The winner has already completed every Python operation. State 1 spans
    // only the enumOption assignment, so this wait cannot participate in a
    // Python lock cycle or stop-the-world deadlock.
    while (enumInitState.load(std::memory_order_acquire) != 2) {
    }
#else
    static bool isInitialized = false;
    if (isInitialized)
        return;
    if (!(isInitialized || _init_enum()))
        Py_FatalError("libshiboken: could not init enum");

    Enum::enumOption = enumOption();
    getPyEnumMeta();
    isInitialized = true;
#endif
}

// PYSIDE-1735: Helper function supporting QEnum
int enumIsFlag(PyObject *ob_type)
{
    init_enum();

    auto *globals = enumGlobals();
    auto *metatype = Py_TYPE(ob_type);
    if (metatype != globals->PyEnumMeta)
        return -1;
    auto *type = reinterpret_cast<PyTypeObject *>(ob_type);
#ifdef Py_GIL_DISABLED
    PyObject *mroRaw = nullptr;
    Py_BEGIN_CRITICAL_SECTION(reinterpret_cast<PyObject *>(type));
    mroRaw = Py_XNewRef(type->tp_mro);
    Py_END_CRITICAL_SECTION();
    AutoDecRef mroRef(mroRaw);
    auto *mro = mroRef.object();
    if (mro == nullptr)
        return -1;
#else
    auto *mro = type->tp_mro;
#endif
    const Py_ssize_t n = PyTuple_Size(mro);
    for (Py_ssize_t idx = 0; idx < n; ++idx) {
        auto *sub_type = reinterpret_cast<PyTypeObject *>(PyTuple_GetItem(mro, idx));
        if (sub_type == reinterpret_cast<PyTypeObject *>(globals->PyFlag))
            return 1;
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////
//
// Support for Missing Values
// ==========================
//
// Qt enums sometimes use undefined values in enums.
// The enum module handles this by the option "KEEP" for Flag and
// IntFlag. The handling of missing enum values is still strict.
//
// We changed that (also for compatibility with some competitor)
// and provide a `_missing_` function that creates the missing value.
//
// The idea:
// ---------
// We cannot modify the already created class.
// But we can create a one-element class with the new value and
// pretend that this is the already existing class.
//
// We create each constant only once and keep the result in a dict
// "_sbk_missing_". This is similar to a competitor's "_sip_missing_".
//

#ifdef Py_GIL_DISABLED
static PyObject *missing_func_ft(PyObject *klass, PyObject *value)
{
    if (!PyLong_Check(value))
        Py_RETURN_NONE;

    // The class dictionary and both cache lookups are shared with arbitrary
    // Python threads. Keep strong references throughout and use SetDefaultRef
    // for both publications so concurrent creators converge on one object.
    AutoDecRef missingName(Shiboken::String::createStaticString("_sbk_missing_"));
    if (missingName.isNull())
        return nullptr;
    auto *type = reinterpret_cast<PyTypeObject *>(klass);
    AutoDecRef tpDict(PepType_GetDict(type));
    if (tpDict.isNull())
        return nullptr;

    PyObject *missingRaw = nullptr;
    int found = PyDict_GetItemRef(tpDict.object(), missingName.object(), &missingRaw);
    if (found < 0)
        return nullptr;
    AutoDecRef missing(missingRaw);
    if (found == 0) {
        AutoDecRef candidate(PyDict_New());
        if (candidate.isNull())
            return nullptr;
        PyObject *published = nullptr;
        if (PyDict_SetDefaultRef(tpDict.object(), missingName.object(), candidate.object(),
                                 &published) < 0) {
            return nullptr;
        }
        missing.reset(published);
    }
    if (!PyDict_Check(missing.object())) {
        PyErr_SetString(PyExc_TypeError, "libshiboken: invalid enum missing-value cache");
        return nullptr;
    }

    AutoDecRef valStr(PyObject_Str(value));
    if (valStr.isNull())
        return nullptr;
    PyObject *ret = nullptr;
    found = PyDict_GetItemRef(missing.object(), valStr.object(), &ret);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return ret; // strong reference from PyDict_GetItemRef()

    // Create outside any dictionary transaction. More than one thread may
    // construct a candidate, but only the winner is published below.
    AutoDecRef clsName(PyObject_GetAttr(klass, Shiboken::PyMagicName::name()));
    AutoDecRef mro(PyObject_GetAttr(klass, Shiboken::PyMagicName::mro()));
    if (clsName.isNull() || mro.isNull())
        return nullptr;
    if (!PyTuple_Check(mro.object()) || PyTuple_Size(mro.object()) < 2) {
        PyErr_SetString(PyExc_TypeError, "libshiboken: enum type has no usable base class");
        return nullptr;
    }
    auto *baseClass = PyTuple_GetItem(mro.object(), 1); // mro owns this reference
    AutoDecRef param(PyDict_New());
    if (param.isNull() || PyDict_SetItem(param.object(), valStr.object(), value) < 0)
        return nullptr;
    AutoDecRef fake(PyObject_CallFunctionObjArgs(baseClass, clsName.object(), param.object(),
                                                 nullptr));
    if (fake.isNull())
        return nullptr;
    AutoDecRef candidate(PyObject_GetAttr(fake.object(), valStr.object()));
    if (candidate.isNull()
        || PyObject_SetAttr(candidate.object(), Shiboken::PyMagicName::class_(), klass) < 0) {
        return nullptr;
    }

    PyObject *published = nullptr;
    if (PyDict_SetDefaultRef(missing.object(), valStr.object(), candidate.object(),
                             &published) < 0) {
        return nullptr;
    }
    return published;
}

static PyMethodDef missing_method = {
    "_missing_", missing_func_ft, METH_O, nullptr
};

static PyObject *create_missing_func(PyObject *klass)
{
    // Bind the enum class directly as m_self. This avoids the historical
    // lazily-created helper type/function/partial chain, whose C++ local-static
    // guards can block another attached thread while Python code is running.
    return PyCFunction_NewEx(&missing_method, klass, nullptr);
}
#else
static PyObject *missing_func(PyObject * /* self */ , PyObject *args)
{
    // In order to relax matters to be more compatible with C++, we need
    // to create a pseudo-member with that value.
    static auto *const _sbk_missing = Shiboken::String::createStaticString("_sbk_missing_");
    static auto *const _name = Shiboken::String::createStaticString("__name__");
    static auto *const _mro = Shiboken::String::createStaticString("__mro__");
    static auto *const _class = Shiboken::String::createStaticString("__class__");

    PyObject *klass{};
    PyObject *value{};
    if (!PyArg_UnpackTuple(args, "missing", 2, 2, &klass, &value))
        Py_RETURN_NONE;
    if (!PyLong_Check(value))
        Py_RETURN_NONE;
    auto *type = reinterpret_cast<PyTypeObject *>(klass);
    AutoDecRef tpDict(PepType_GetDict(type));
    auto *sbk_missing = PyDict_GetItem(tpDict.object(), _sbk_missing);
    if (!sbk_missing) {
        sbk_missing = PyDict_New();
        PyDict_SetItem(tpDict.object(), _sbk_missing, sbk_missing);
    }
    // See if the value is already in the dict.
    AutoDecRef val_str(PyObject_CallMethod(value, "__str__", nullptr));
    auto *ret = PyDict_GetItem(sbk_missing, val_str);
    if (ret) {
        Py_INCREF(ret);
        return ret;
    }
    // No, we must create a new object and insert it into the dict.
    AutoDecRef cls_name(PyObject_GetAttr(klass, _name));
    AutoDecRef mro(PyObject_GetAttr(klass, _mro));
    auto *baseClass(PyTuple_GetItem(mro, 1));
    AutoDecRef param(PyDict_New());
    PyDict_SetItem(param, val_str, value);
    AutoDecRef fake(PyObject_CallFunctionObjArgs(baseClass, cls_name.object(), param.object(),
                                                 nullptr));
    ret = PyObject_GetAttr(fake, val_str);
    PyDict_SetItem(sbk_missing, val_str, ret);
    // Now the real fake: Pretend that the type is our original type!
    PyObject_SetAttr(ret, _class, klass);
    return ret;
}

static struct PyMethodDef dummy_methods[] = {
    {"_missing_", missing_func, METH_VARARGS|METH_STATIC, nullptr},
    {nullptr, nullptr, 0, nullptr}
};

static PyType_Slot dummy_slots[] = {
    {Py_tp_base, reinterpret_cast<void *>(&PyType_Type)},
    {Py_tp_methods, reinterpret_cast<void *>(dummy_methods)},
    {0, nullptr}
};

static PyType_Spec dummy_spec = {
    "1:builtins.EnumType",
    0,
    0,
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,
    dummy_slots,
};

static PyObject *create_missing_func(PyObject *klass)
{
    // When creating the class, memorize it in the missing function by
    // a partial function argument.
    static auto *const type = SbkType_FromSpec(&dummy_spec);
    static auto *const obType = reinterpret_cast<PyObject *>(type);
    static auto *const _missing = Shiboken::String::createStaticString("_missing_");
    static auto *const func = PyObject_GetAttr(obType, _missing);
    static auto *const partial = Pep_GetPartialFunction();
    return PyObject_CallFunctionObjArgs(partial, func, klass, nullptr);
}
#endif
//
////////////////////////////////////////////////////////////////////////

} // extern "C"

namespace Shiboken::Enum {

int enumOption{};

bool check(PyObject *pyObj)
{
    return checkType(Py_TYPE(pyObj));
}

bool checkType(PyTypeObject *pyTypeObj)
{
    init_enum();

    return Py_TYPE(reinterpret_cast<PyObject *>(pyTypeObj)) == getPyEnumMeta();
}

PyObject *getEnumItemFromValue(PyTypeObject *enumType, EnumValueType itemValue)
{
    init_enum();

    auto *obEnumType = reinterpret_cast<PyObject *>(enumType);
    AutoDecRef val2members(PyObject_GetAttrString(obEnumType, "_value2member_map_"));
    if (val2members.isNull()) {
        PyErr_Clear();
        return nullptr;
    }
    AutoDecRef ob_value(PyLong_FromLongLong(itemValue));
#ifdef Py_GIL_DISABLED
    PyObject *result = nullptr;
    const int found = PyDict_GetItemRef(val2members.object(), ob_value.object(), &result);
    return found > 0 ? result : nullptr;
#else
    auto *result = PyDict_GetItem(val2members, ob_value);
    Py_XINCREF(result);
    return result;
#endif
}

PyObject *newItem(PyTypeObject *enumType, EnumValueType itemValue,
                  const char *itemName)
{
    init_enum();

    auto *obEnumType = reinterpret_cast<PyObject *>(enumType);
    if (!itemName)
        return PyObject_CallFunction(obEnumType, "L", itemValue);

#ifdef Py_GIL_DISABLED
    AutoDecRef memberMapName(String::createStaticString("_member_map_"));
    AutoDecRef tpDict(PepType_GetDict(enumType));
    if (memberMapName.isNull() || tpDict.isNull())
        return nullptr;
    PyObject *memberMapRaw = nullptr;
    const int haveMemberMap = PyDict_GetItemRef(tpDict.object(), memberMapName.object(),
                                                &memberMapRaw);
    AutoDecRef memberMap(memberMapRaw);
    if (haveMemberMap <= 0 || !PyDict_Check(memberMap.object()))
        return nullptr;
    AutoDecRef pyItemName(String::fromCString(itemName));
    if (pyItemName.isNull())
        return nullptr;
    PyObject *result = nullptr;
    const int found = PyDict_GetItemRef(memberMap.object(), pyItemName.object(), &result);
    return found > 0 ? result : nullptr;
#else
    static PyObject *const _member_map_ = String::createStaticString("_member_map_");
    AutoDecRef tpDict(PepType_GetDict(enumType));
    auto *member_map = PyDict_GetItem(tpDict.object(), _member_map_);
    if (!(member_map && PyDict_Check(member_map)))
        return nullptr;
    auto *result = PyDict_GetItemString(member_map, itemName);
    Py_XINCREF(result);
    return result;
#endif
}

EnumValueType getValue(PyObject *enumItem)
{
    init_enum();

    assert(Enum::check(enumItem));

    AutoDecRef pyValue(PyObject_GetAttrString(enumItem, "value"));
    return PyLong_AsLongLong(pyValue);
}

void setTypeConverter(PyTypeObject *type, SbkConverter *converter,
                      SbkConverter *flagsConverter)
{
    SbkEnumTypePrivate *priv = PepType_SETP(reinterpret_cast<SbkEnumType *>(type));
    priv->converter = converter;
    priv->flagsConverter = flagsConverter;
}

SbkConverter *getConverter(SbkEnumType *type)
{
    SbkEnumTypePrivate *priv = PepType_SETP(type);
    return priv->converter;
}

SbkConverter *getFlagsConverter(SbkEnumType *type)
{
    SbkEnumTypePrivate *priv = PepType_SETP(type);
    return priv->flagsConverter;
}

static void setModuleAndQualnameOnType(PyObject *type, const char *fullName)
{
    const char *colon = std::strchr(fullName, ':');
    assert(colon);
    int package_level = atoi(fullName);
    const char *mod = colon + 1;

    const char *qual = mod;
    for (int idx = package_level; idx > 0; --idx) {
        const char *dot = std::strchr(qual, '.');
        if (!dot)
            break;
        qual = dot + 1;
    }
    int mlen = qual - mod - 1;
    AutoDecRef module(Shiboken::String::fromCString(mod, mlen));
    AutoDecRef qualname(Shiboken::String::fromCString(qual));

    PyObject_SetAttr(type, Shiboken::PyMagicName::module(), module);
    PyObject_SetAttr(type, Shiboken::PyMagicName::qualname(), qualname);
}

static PyTypeObject *createEnumForPython(PyObject *scopeOrModule,
                                         const char *fullName,
                                         PyObject *pyEnumItems)
{
    const char *dot = std::strrchr(fullName, '.');
    AutoDecRef name(Shiboken::String::fromCString(dot ? dot + 1 : fullName));

#ifdef Py_GIL_DISABLED
    AutoDecRef intEnumNameRef(String::createStaticString("IntEnum"));
    if (intEnumNameRef.isNull())
        return nullptr;
    PyObject *enumName = intEnumNameRef.object();
#else
    static PyObject *const intEnumName = String::createStaticString("IntEnum");
    PyObject *enumName = intEnumName;
#endif
    AutoDecRef enumNameHolder{};
    if (PyType_Check(scopeOrModule)) {
        // For global objects, we have no good solution, yet where to put the int info.
        auto *type = reinterpret_cast<PyTypeObject *>(scopeOrModule);
#ifdef Py_GIL_DISABLED
        PyObject *flagsRaw = nullptr;
        PyObject *typesRaw = nullptr;
        if (SbkObjectType_GetEnumFlagDicts(type, &flagsRaw, &typesRaw) < 0)
            return nullptr;
        AutoDecRef flagsDict(flagsRaw);
        AutoDecRef typeDict(typesRaw);
        PyObject *enumNameRaw = nullptr;
        const int found = PyDict_GetItemRef(typeDict.object(), name.object(), &enumNameRaw);
        if (found < 0)
            return nullptr;
        if (found > 0) {
            enumNameHolder.reset(enumNameRaw);
            enumName = enumNameHolder.object();
        }
#else
        auto *sotp = PepType_SOTP(type);
        if (!sotp->enumFlagsDict)
            initEnumFlagsDict(type);
        enumName = PyDict_GetItem(sotp->enumTypeDict, name);
#endif
    }

    SBK_UNUSED(getPyEnumMeta()); // enforce PyEnumModule creation
    auto *globals = enumGlobals();
    assert(globals->PyEnumModule != nullptr);
    AutoDecRef PyEnumType(PyObject_GetAttr(globals->PyEnumModule, enumName));
    assert(PyEnumType.object());
    bool isFlag = PyObject_IsSubclass(PyEnumType, globals->PyFlag);

    // See if we should use the Int versions of the types, again
    bool useIntInheritance = Enum::enumOption & Enum::ENOPT_INHERIT_INT;
    if (useIntInheritance) {
        auto *surrogate = PyObject_IsSubclass(PyEnumType, globals->PyFlag) ? globals->PyIntFlag : globals->PyIntEnum;
        Py_INCREF(surrogate);
        PyEnumType.reset(surrogate);
    }

    // Walk the enumItemStrings and create a Python enum type.
    auto *pyName = name.object();

    // We now create the new type. Since Python 3.11, we need to pass in
    // `boundary=KEEP` because the default STRICT crashes on us.
    // See  QDir.Filter.Drives | QDir.Filter.Files
    AutoDecRef callArgs(Py_BuildValue("(OO)", pyName, pyEnumItems));
    AutoDecRef callDict(PyDict_New());
#ifdef Py_GIL_DISABLED
    AutoDecRef boundaryRef(String::createStaticString("boundary"));
    if (boundaryRef.isNull())
        return nullptr;
    PyObject *boundary = boundaryRef.object();
#else
    static PyObject *boundary = String::createStaticString("boundary");
#endif
    if (globals->PyFlag_KEEP)
        PyDict_SetItem(callDict, boundary, globals->PyFlag_KEEP);
    auto *obNewType = PyObject_Call(PyEnumType, callArgs, callDict);
    if (!obNewType || PyObject_SetAttr(scopeOrModule, pyName, obNewType) < 0)
        return nullptr;

    // For compatibility with Qt enums, provide a permissive missing method for (Int)?Enum.
    if (!isFlag) {
        bool supportMissing = !(Enum::enumOption & Enum::ENOPT_NO_MISSING);
        if (supportMissing) {
            AutoDecRef enum_missing(create_missing_func(obNewType));
            PyObject_SetAttrString(obNewType, "_missing_", enum_missing);
        }
    }

    setModuleAndQualnameOnType(obNewType, fullName);

    // See if we should re-introduce shortcuts in the enclosing object.
    const bool useGlobalShortcut = (Enum::enumOption & Enum::ENOPT_GLOBAL_SHORTCUT) != 0;
    const bool useScopedShortcut = (Enum::enumOption & Enum::ENOPT_SCOPED_SHORTCUT) != 0;
    if (useGlobalShortcut || useScopedShortcut) {
        // We have to use the iterator protokol because the values dict is a mappingproxy.
        AutoDecRef values(PyObject_GetAttr(obNewType, PyMagicName::members()));
        AutoDecRef mapIterator(PyObject_GetIter(values));
        AutoDecRef mapKey{};
        bool isModule = PyModule_Check(scopeOrModule);
        while ((mapKey.reset(PyIter_Next(mapIterator))), mapKey.object()) {
            if ((useGlobalShortcut && isModule) || (useScopedShortcut && !isModule)) {
                AutoDecRef value(PyObject_GetItem(values, mapKey));
                if (PyObject_SetAttr(scopeOrModule, mapKey, value) < 0)
                    return nullptr;
            }
        }
    }

    return reinterpret_cast<PyTypeObject *>(obNewType);
}

template <typename IntT>
static PyObject *toPyObject(IntT v)
{
    if constexpr (sizeof(IntT) == 8) {
        if constexpr (std::is_unsigned_v<IntT>)
            return PyLong_FromUnsignedLongLong(v);
        return PyLong_FromLongLong(v);
    }
    if constexpr (std::is_unsigned_v<IntT>)
        return PyLong_FromUnsignedLong(v);
    return PyLong_FromLong(v);
}

template <typename IntT>
static PyTypeObject *createPythonEnumHelper(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const IntT enumValues[])
{
    AutoDecRef args(PyList_New(0));
    auto *pyEnumItems = args.object();
    for (size_t idx = 0; enumItemStrings[idx] != nullptr; ++idx) {
        const char *kv = enumItemStrings[idx];
        auto *key = PyUnicode_FromString(kv);
        auto *value = toPyObject(enumValues[idx]);
        auto *key_value = PyTuple_New(2);
        PyTuple_SetItem(key_value, 0, key);
        PyTuple_SetItem(key_value, 1, value);
        PyList_Append(pyEnumItems, key_value);
    }
    return createEnumForPython(module, fullName, pyEnumItems);
}

// Now we have to concretize these functions explicitly,
// otherwise templates will not work across modules.

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const int64_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const uint64_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const int32_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const uint32_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const int16_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const uint16_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const int8_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(PyObject *module,
    const char *fullName, const char *enumItemStrings[], const uint8_t enumValues[])
{
    return createPythonEnumHelper(module, fullName, enumItemStrings, enumValues);
}

PyTypeObject *createPythonEnum(const char *fullName, PyObject *pyEnumItems,
                               const char *enumTypeName, PyObject *callDict)
{
    SBK_UNUSED(getPyEnumMeta());
    AutoDecRef PyEnumTypeName(Shiboken::String::fromCString(enumTypeName));
    AutoDecRef PyEnumType(PyObject_GetAttr(enumGlobals()->PyEnumModule, PyEnumTypeName));
    if (!PyEnumType) {
        PyErr_Format(PyExc_RuntimeError, "libshiboken: Failed to get enum type %s", enumTypeName);
        return nullptr;
    }

    const char *dot = std::strrchr(fullName, '.');
    AutoDecRef name(Shiboken::String::fromCString(dot ? dot + 1 : fullName));
    AutoDecRef callArgs(Py_BuildValue("(OO)", name.object(), pyEnumItems));
    auto *newType = PyObject_Call(PyEnumType, callArgs, callDict);

    setModuleAndQualnameOnType(newType, fullName);

    return reinterpret_cast<PyTypeObject *>(newType);
}

} // namespace Shiboken::Enum
