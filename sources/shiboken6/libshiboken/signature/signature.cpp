// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

////////////////////////////////////////////////////////////////////////////
//
// signature.cpp
// -------------
//
// This is the main file of the signature module.
// It contains the most important functions and avoids confusion
// by moving many helper functions elsewhere.
//
// General documentation can be found in `signature_doc.rst`.
//

#include "signature.h"
#include "signature_p.h"

#include "basewrapper.h"
#include "autodecref.h"
#include "sbkpep.h"
#include "sbkstring.h"
#include "sbkstaticstrings.h"
#include "sbkstaticstrings_p.h"
#include "sbkfeature_base.h"

#include <structmember.h>

#include <algorithm>
#include <cstring>

using namespace Shiboken;

extern "C"
{

static PyObject *CreateSignature(PyObject *props, PyObject *key)
{
    /*
     * Here is the new function to create all signatures. It simply calls
     * into Python and creates a signature object directly.
     * This is so much simpler than using all the attributes explicitly
     * to support '_signature_is_functionlike()'.
     */
    return PyObject_CallFunction(signatureGlobals()->create_signature_func,
                                 "(OO)", props, key);
}

PyObject *GetClassOrModOf(PyObject *ob)
{
    /*
     * Return the type or module of a function or type.
     * The purpose is finally to use the name of the object.
     */
    if (PyType_Check(ob)) {
        // PySide-928: The type case must do refcounting like the others as well.
        Py_INCREF(ob);
        return ob;
    }
#ifdef PYPY_VERSION
    // PYSIDE-535: PyPy has a special builtin method that acts almost like PyCFunction.
    if (Py_TYPE(ob) == PepBuiltinMethod_TypePtr)
        return _get_class_of_bm(ob);
#endif
    if (PyType_IsSubtype(Py_TYPE(ob), &PyCFunction_Type))
        return _get_class_of_cf(ob);
    if (Py_TYPE(ob) == PepStaticMethod_TypePtr)
        return _get_class_of_sm(ob);
    if (Py_TYPE(ob) == PepMethodDescr_TypePtr)
        return _get_class_of_descr(ob);
    if (Py_TYPE(ob) == &PyWrapperDescr_Type)
        return _get_class_of_descr(ob);
    Py_FatalError("libshiboken: unexpected type in GetClassOrModOf");
    return nullptr;
}

PyObject *GetTypeKey(PyObject *ob)
{
    assert(PyType_Check(ob) || PyModule_Check(ob));
    /*
     * Obtain a unique key using the module name and the type name.
     *
     * PYSIDE-1286: We use correct __module__ and __qualname__, now.
     */
    AutoDecRef module_name(PyObject_GetAttr(ob, PyMagicName::module()));
    if (module_name.isNull()) {
        // We have no module_name because this is a module ;-)
        PyErr_Clear();
        module_name.reset(PyObject_GetAttr(ob, PyMagicName::name()));
        return Py_BuildValue("O", module_name.object());
    }
    AutoDecRef class_name(PyObject_GetAttr(ob, PyMagicName::qualname()));
    if (class_name.isNull()) {
        Py_FatalError("libshiboken: missing class name in GetTypeKey");
        return nullptr;
    }
    return Py_BuildValue("(OO)", module_name.object(), class_name.object());
}

PyObject *TypeKey_to_PropsDict(PyObject *type_key)
{
    auto *globals = signatureGlobals();
#ifdef Py_GIL_DISABLED
    PyObject *dict = nullptr;
    const int found = PyDict_GetItemRef(globals->arg_dict, type_key, &dict);
    if (found < 0)
        return nullptr;
    if (found == 0)
        return PyDict_New();
    if (PyDict_Check(dict))
        return dict; // strong reference from PyDict_GetItemRef()
    Py_DECREF(dict);
    return PySide_BuildSignatureProps(type_key); // strong reference on FT
#else
    PyObject *&empty_dict = globals->empty_dict;
    PyObject *dict = PyDict_GetItem(globals->arg_dict, type_key);
    if (dict == nullptr) {
        if (empty_dict == nullptr)
            empty_dict = PyDict_New();
        dict = empty_dict;
    }
    if (!PyDict_Check(dict))
        dict = PySide_BuildSignatureProps(type_key);
    return dict;
#endif
}

static PyObject *_GetSignature_Cached(PyObject *props, PyObject *func_kind, PyObject *modifier)
{
    // Special case: We want to know the func_kind.
    if (modifier) {
        PyUnicode_InternInPlace(&modifier);
        if (modifier == PyMagicName::func_kind())
            return Py_BuildValue("O", func_kind);
    }

    AutoDecRef key(modifier == nullptr ? Py_BuildValue("O", func_kind)
                                       : Py_BuildValue("(OO)", func_kind, modifier));
#ifdef Py_GIL_DISABLED
    if (key.isNull())
        return nullptr;
    PyObject *value = nullptr;
    int found = PyDict_GetItemRef(props, key.object(), &value);
    if (found < 0)
        return nullptr;
    if (found > 0)
        return value; // strong reference from PyDict_GetItemRef()

    // Signature construction executes Python. More than one thread may compute
    // an equivalent candidate, but only the first published value becomes the
    // cache entry and every caller receives a strong reference to that winner.
    AutoDecRef candidate(CreateSignature(props, key.object()));
    if (candidate.isNull())
        Py_RETURN_NONE;
    PyObject *published = nullptr;
    if (PyDict_SetDefaultRef(props, key.object(), candidate.object(), &published) < 0)
        return nullptr;
    return published;
#else
    PyObject *value = PyDict_GetItem(props, key);
    if (value == nullptr) {
        // we need to compute a signature object
        value = CreateSignature(props, key);
        if (value != nullptr) {
            if (PyDict_SetItem(props, key, value) < 0)
                // this is an error
                return nullptr;
        }
        else {
            // key not found
            Py_RETURN_NONE;
        }
    }
    return Py_INCREF(value), value;
#endif
}

#ifdef PYPY_VERSION
PyObject *GetSignature_Method(PyObject *obfunc, PyObject *modifier)
{
    AutoDecRef obtype_mod(GetClassOrModOf(obfunc));
    AutoDecRef type_key(GetTypeKey(obtype_mod));
    if (type_key.isNull())
        Py_RETURN_NONE;
#ifdef Py_GIL_DISABLED
    AutoDecRef dict(TypeKey_to_PropsDict(type_key.object()));
    if (dict.isNull())
        return nullptr;
    AutoDecRef func_name(PyObject_GetAttr(obfunc, PyMagicName::name()));
    if (func_name.isNull())
        Py_RETURN_NONE;
    PyObject *propsRaw = nullptr;
    const int found = PyDict_GetItemRef(dict.object(), func_name.object(), &propsRaw);
    AutoDecRef props(propsRaw);
    if (found < 0)
        return nullptr;
    if (found == 0)
        Py_RETURN_NONE;
    return _GetSignature_Cached(props.object(), PyName::method(), modifier);
#else
    PyObject *dict = TypeKey_to_PropsDict(type_key);
    if (dict == nullptr)
        return nullptr;
    AutoDecRef func_name(PyObject_GetAttr(obfunc, PyMagicName::name()));
    PyObject *props = !func_name.isNull() ? PyDict_GetItem(dict, func_name) : nullptr;
    if (props == nullptr)
        Py_RETURN_NONE;
    return _GetSignature_Cached(props, PyName::method(), modifier);
#endif
}
#endif

PyObject *GetSignature_Function(PyObject *obfunc, PyObject *modifier)
{
    // make sure that we look into PyCFunction, only...
    if (Py_TYPE(obfunc) == PepFunction_TypePtr)
        Py_RETURN_NONE;
    AutoDecRef obtype_mod(GetClassOrModOf(obfunc));
    AutoDecRef type_key(GetTypeKey(obtype_mod));
    if (type_key.isNull())
        Py_RETURN_NONE;
#ifdef Py_GIL_DISABLED
    AutoDecRef dict(TypeKey_to_PropsDict(type_key.object()));
    if (dict.isNull())
        return nullptr;
    AutoDecRef func_name(PyObject_GetAttr(obfunc, PyMagicName::name()));
    if (func_name.isNull())
        Py_RETURN_NONE;
    PyObject *propsRaw = nullptr;
    const int haveProps = PyDict_GetItemRef(dict.object(), func_name.object(), &propsRaw);
    AutoDecRef props(propsRaw);
    if (haveProps < 0)
        return nullptr;
    if (haveProps == 0)
        Py_RETURN_NONE;
#else
    PyObject *dict = TypeKey_to_PropsDict(type_key);
    if (dict == nullptr)
        return nullptr;
    AutoDecRef func_name(PyObject_GetAttr(obfunc, PyMagicName::name()));
    PyObject *props = !func_name.isNull() ? PyDict_GetItem(dict, func_name) : nullptr;
    if (props == nullptr)
        Py_RETURN_NONE;
#endif

    int flags = PyCFunction_GetFlags(obfunc);
    PyObject *func_kind{};
    if (PyModule_Check(obtype_mod.object()))
        func_kind = PyName::function();
    else if (flags & METH_CLASS)
        func_kind = PyName::classmethod();
    else if (flags & METH_STATIC)
        func_kind = PyName::staticmethod();
    else
        func_kind = PyName::method();
#ifdef Py_GIL_DISABLED
    return _GetSignature_Cached(props.object(), func_kind, modifier);
#else
    return _GetSignature_Cached(props, func_kind, modifier);
#endif
}

PyObject *GetSignature_Wrapper(PyObject *ob, PyObject *modifier)
{
    AutoDecRef func_name(PyObject_GetAttr(ob, PyMagicName::name()));
    AutoDecRef objclass(PyObject_GetAttr(ob, PyMagicName::objclass()));
    AutoDecRef class_key(GetTypeKey(objclass));
    if (func_name.isNull() || objclass.isNull() || class_key.isNull())
        return nullptr;
#ifdef Py_GIL_DISABLED
    AutoDecRef dict(TypeKey_to_PropsDict(class_key.object()));
    if (dict.isNull())
        return nullptr;
    PyObject *propsRaw = nullptr;
    const int haveProps = PyDict_GetItemRef(dict.object(), func_name.object(), &propsRaw);
    AutoDecRef props(propsRaw);
    if (haveProps < 0)
        return nullptr;
    if (haveProps == 0) {
        // handle `__init__` like the class itself
        if (PyUnicode_CompareWithASCIIString(func_name.object(), "__init__") == 0)
            return GetSignature_TypeMod(objclass.object(), modifier);
        Py_RETURN_NONE;
    }
    return _GetSignature_Cached(props.object(), PyName::method(), modifier);
#else
    PyObject *dict = TypeKey_to_PropsDict(class_key);
    if (dict == nullptr)
        return nullptr;
    PyObject *props = PyDict_GetItem(dict, func_name);
    if (props == nullptr) {
        // handle `__init__` like the class itself
        if (PyUnicode_CompareWithASCIIString(func_name, "__init__") == 0)
            return GetSignature_TypeMod(objclass, modifier);
        Py_RETURN_NONE;
    }
    return _GetSignature_Cached(props, PyName::method(), modifier);
#endif
}

PyObject *GetSignature_TypeMod(PyObject *ob, PyObject *modifier)
{
    AutoDecRef ob_name(PyObject_GetAttr(ob, PyMagicName::name()));
    AutoDecRef ob_key(GetTypeKey(ob));

#ifdef Py_GIL_DISABLED
    if (ob_name.isNull() || ob_key.isNull())
        return nullptr;
    AutoDecRef dict(TypeKey_to_PropsDict(ob_key.object()));
    if (dict.isNull())
        return nullptr;
    PyObject *propsRaw = nullptr;
    const int haveProps = PyDict_GetItemRef(dict.object(), ob_name.object(), &propsRaw);
    AutoDecRef props(propsRaw);
    if (haveProps < 0)
        return nullptr;
    if (haveProps == 0)
        Py_RETURN_NONE;
    return _GetSignature_Cached(props.object(), PyName::method(), modifier);
#else
    PyObject *dict = TypeKey_to_PropsDict(ob_key);
    if (dict == nullptr)
        return nullptr;
    PyObject *props = PyDict_GetItem(dict, ob_name);
    if (props == nullptr)
        Py_RETURN_NONE;
    return _GetSignature_Cached(props, PyName::method(), modifier);
#endif
}

////////////////////////////////////////////////////////////////////////////
//
// get_signature  --  providing a superior interface
//
// Additional to the interface via `__signature__`, we also provide
// a general function, which allows for different signature layouts.
// The `modifier` argument is a string that is passed in from `loader.py`.
// Configuration what the modifiers mean is completely in Python.
//
// PYSIDE-2101: The __signature__ attribute is gone due to rlcompleter.
//

PyObject *get_signature_intern(PyObject *ob, PyObject *modifier)
{
#ifdef PYPY_VERSION
    // PYSIDE-535: PyPy has a special builtin method that acts almost like PyCFunction.
    if (Py_TYPE(ob) == PepBuiltinMethod_TypePtr) {
        return pyside_bm_get___signature__(ob, modifier);
    }
#endif
    if (PyType_IsSubtype(Py_TYPE(ob), &PyCFunction_Type))
        return pyside_cf_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == PepStaticMethod_TypePtr)
        return pyside_sm_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == PepMethodDescr_TypePtr)
        return pyside_md_get___signature__(ob, modifier);
    if (PyType_Check(ob))
        return pyside_tp_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == &PyWrapperDescr_Type)
        return pyside_wd_get___signature__(ob, modifier);
    // For classmethods we use the simple wrapper description implementation.
    if (Py_TYPE(ob) == &PyClassMethodDescr_Type)
        return pyside_wd_get___signature__(ob, modifier);
    return nullptr;
}

static PyObject *get_signature(PyObject * /* self */, PyObject *args)
{
    PyObject *ob{};
    PyObject *modifier = nullptr;

    if (!PyArg_ParseTuple(args, "O|O", &ob, &modifier))
        return nullptr;
    if (Py_TYPE(ob) == PepFunction_TypePtr)
        Py_RETURN_NONE;
    PyObject *ret = get_signature_intern(ob, modifier);
    if (ret != nullptr)
        return ret;
    if (PyErr_Occurred())
        return nullptr;
    Py_RETURN_NONE;
}

////////////////////////////////////////////////////////////////////////////
//
// make_snake_case_name  --  make efficient name change available in Python
//
//

static PyObject *make_snake_case_name(PyObject * /* self */, PyObject *arg)
{
    if (!PyUnicode_Check(arg))
        return PyErr_Format(PyExc_TypeError, "libshiboken: %S is not a string");
    return Shiboken::String::getSnakeCaseName(arg, true);
}

////////////////////////////////////////////////////////////////////////////
//
// feature_import  --  special handling for `from __feature__ import ...`
//
// The actual function is implemented in Python.
// When no features are involved, we redirect to the original import.
// This avoids an extra function level in tracebacks that is irritating.
//

#ifdef Py_GIL_DISABLED
static thread_local bool featureImportFallbackActive = false;

class FeatureImportFallbackGuard
{
public:
    FeatureImportFallbackGuard() { featureImportFallbackActive = true; }
    ~FeatureImportFallbackGuard() { featureImportFallbackActive = false; }
};

static PyObject *callOriginalImport(PyObject *args, PyObject *kwds)
{
    AutoDecRef builtins(PepEval_GetFrameBuiltins());
    if (builtins.isNull())
        return nullptr;
    PyObject *origImportRaw = nullptr;
    const int found = PyDict_GetItemStringRef(builtins.object(), "__orig_import__",
                                               &origImportRaw);
    AutoDecRef origImport(origImportRaw);
    if (found < 0)
        return nullptr;
    if (found == 0) {
        Py_FatalError("libshiboken: builtins has no \"__orig_import__\" function");
        return nullptr;
    }
    return PyObject_Call(origImport.object(), args, kwds);
}
#endif

static PyObject *feature_import(PyObject * /* self */, PyObject *args, PyObject *kwds)
{
#ifdef Py_GIL_DISABLED
    // The historical implementation temporarily rewrote builtins.__import__
    // process-wide around the fallback import. That is not thread-local and can
    // expose another thread to the wrong import hook. Preserve the recursion
    // suppression semantics with a per-thread guard instead.
    if (featureImportFallbackActive)
        return callOriginalImport(args, kwds);
#endif

    auto *pyside_globals = signatureGlobals();
    PyObject *ret = PyObject_Call(pyside_globals->feature_import_func, args, kwds);
    if (ret != Py_None)
        return ret;
    // feature_import did not handle it, so call the normal import.
    Py_DECREF(ret);
#ifdef Py_GIL_DISABLED
    FeatureImportFallbackGuard guard;
    ret = callOriginalImport(args, kwds);
    if (ret != nullptr) {
        // PYSIDE-2029: Intercept after the import to search for PySide usage.
        AutoDecRef post(PyObject_CallFunctionObjArgs(pyside_globals->feature_imported_func,
                                                     ret, nullptr));
        if (post.isNull()) {
            Py_DECREF(ret);
            ret = nullptr;
        }
    }
#else
    Shiboken::AutoDecRef builtins(PepEval_GetFrameBuiltins());
    PyObject *origImportFunc = PyDict_GetItemString(builtins.object(), "__orig_import__");
    if (origImportFunc == nullptr) {
        Py_FatalError("libshiboken: builtins has no \"__orig_import__\" function");
    }
    ret = PyObject_Call(origImportFunc, args, kwds);
    if (ret) {
        // PYSIDE-3054: Instead of just calling the original import, we temporarily
        //              reset the whole import function to the previous version.
        //              This prevents unforeseen recursions like in settrace.
        PyObject *featureImportFunc = PyDict_GetItemString(builtins.object(), "__import__");
        Py_INCREF(origImportFunc);
        Py_INCREF(featureImportFunc);
        PyDict_SetItemString(builtins.object(), "__import__", origImportFunc);

        // PYSIDE-2029: Intercept after the import to search for PySide usage.
        PyObject *post = PyObject_CallFunctionObjArgs(pyside_globals->feature_imported_func,
                                                      ret, nullptr);
        Py_XDECREF(post);

        PyDict_SetItemString(builtins.object(), "__import__", featureImportFunc);
        Py_DECREF(origImportFunc);
        Py_DECREF(featureImportFunc);

        if (post == nullptr) {
            Py_DECREF(ret);
            ret = nullptr;
        }
    }
#endif
    return ret;
}

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#endif
PyMethodDef signature_methods[] = {
    {"__feature_import__", reinterpret_cast<PyCFunction>(feature_import),
        METH_VARARGS | METH_KEYWORDS, nullptr},
    {"get_signature", get_signature, METH_VARARGS,
        "get the signature, passing an optional string parameter"},
    {"make_snake_case_name", make_snake_case_name, METH_O,
        "turn a camelCase name into snake_case"},
    {nullptr, nullptr, 0, nullptr}
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

////////////////////////////////////////////////////////////////////////////
//
// Argument Handling
// -----------------
//
// * PySide_BuildSignatureArgs
//
// Called during class or module initialization.
// The signature strings from the C modules are stored in a dict for
// later use.
//
// * PySide_BuildSignatureProps
//
// Called on demand during signature retieval. This function calls all the way
// through `parser.py` and prepares all properties for the functions of the class.
// The parsed properties can then be used to create signature objects.
//

static int PySide_BuildSignatureArgs(PyObject *obtype_mod, const char *signatures[])
{
    auto *pyside_globals = signatureGlobals();
    AutoDecRef type_key(GetTypeKey(obtype_mod));
    /*
     * PYSIDE-996: Avoid string overflow in MSVC, which has a limit of
     * 2**15 unicode characters (64 K memory).
     * Instead of one huge string, we take a ssize_t that is the
     * address of a string array. It will not be turned into a real
     * string list until really used by Python. This is quite optimal.
     */
    AutoDecRef numkey(PyLong_FromVoidPtr(static_cast<void*>(signatures)));
    if (type_key.isNull() || numkey.isNull()
        || PyDict_SetItem(pyside_globals->arg_dict, type_key, numkey) < 0)
        return -1;
    /*
     * We record also a mapping from type key to type/module. This helps to
     * lazily initialize the Py_LIMITED_API in name_key_to_func().
     */
    return PyDict_SetItem(pyside_globals->map_dict, type_key, obtype_mod) == 0 ? 0 : -1;
}

static int PySide_BuildSignatureArgsByte(PyObject *obtype_mod, const uint8_t *signatures,
                                         size_t size)
{
    auto *pyside_globals = signatureGlobals();
    AutoDecRef type_key(GetTypeKey(obtype_mod));
    AutoDecRef numkey(PyTuple_New(2));
    PyTuple_SetItem(numkey.object(), 0, PyLong_FromVoidPtr(const_cast<uint8_t *>(signatures)));
    PyTuple_SetItem(numkey.object(), 1, PyLong_FromSize_t(size));
    if (type_key.isNull() || numkey.isNull()
        || PyDict_SetItem(pyside_globals->arg_dict, type_key, numkey) < 0)
        return -1;
    return PyDict_SetItem(pyside_globals->map_dict, type_key, obtype_mod) == 0 ? 0 : -1;
}

// PYSIDE-2701: MS cannot use the name "_expand".
static PyObject *byteExpand(PyObject *packed)
{
    const char commonMsg[] = "Please disable compression by passing  --unoptimize=compression";

#ifdef Py_GIL_DISABLED
    // Avoid Python-backed function-local statics: their C++ guard can make one
    // attached thread block while the initializing thread executes Python.
    AutoDecRef compressModule(PyImport_ImportModule("zlib"));
    if (compressModule.isNull())
        return PyErr_Format(PyExc_ImportError,
                            "The zlib module cannot be imported. %s", commonMsg);
    AutoDecRef expandFunc(PyObject_GetAttrString(compressModule.object(), "decompress"));
    if (expandFunc.isNull())
        return PyErr_Format(PyExc_NameError,
                            "The expand function of zlib was not fount. %s", commonMsg);
    PyObject *unpacked = PyObject_CallFunctionObjArgs(expandFunc.object(), packed, nullptr);
#else
    static PyObject *compressModule = PyImport_ImportModule("zlib");
    if (compressModule == nullptr)
        return PyErr_Format(PyExc_ImportError,
                            "The zlib module cannot be imported. %s", commonMsg);

    static PyObject *expandFunc = PyObject_GetAttrString(compressModule, "decompress");
    if (expandFunc == nullptr)
        return PyErr_Format(PyExc_NameError,
                            "The expand function of zlib was not fount. %s", commonMsg);
    PyObject *unpacked = PyObject_CallFunctionObjArgs(expandFunc, packed, nullptr);
#endif
    if (unpacked == nullptr)
        return PyErr_Format(PyExc_ValueError,
                            "Some packed strings could not be unpacked. %s", commonMsg);
    return unpacked;
}

const char **bytesToStrings(const uint8_t signatures[], Py_ssize_t size)
{
    // PYSIDE-2701: Unpack a ZLIB compressed string.
    // The result is a single char* with newlines after each string. Convert
    // this into single char* objects that InitSignatureStrings expects.

    const auto *chars = reinterpret_cast<const char *>(signatures);
    PyObject *packed = PyBytes_FromStringAndSize(chars, size);

    // The Qt compressor treats empty arrays specially.
    PyObject *data = size > 0 ? byteExpand(packed) : PyBytes_FromStringAndSize(chars, 0);
    if (data == nullptr)
        return nullptr;

    char *cdata{};
    Py_ssize_t len{};
    PyBytes_AsStringAndSize(data, &cdata, &len);
    char *cdataEnd = cdata + len;
    size_t nlines = std::count(cdata, cdataEnd, '\n');

    char **lines = new char *[nlines + 1];
    int pos = 0;
    char *line = cdata;

    for (char *p = cdata; p < cdataEnd; ++p) {
        if (*p == '\n') {
            lines[pos++] = line;
            *p = 0;
            line = p + 1;
        }
    }
    lines[pos] = nullptr;
    return const_cast<const char **>(lines);
}

PyObject *PySide_BuildSignatureProps(PyObject *type_key)
{
    /*
     * Here is the second part of the function.
     * This part will be called on-demand when needed by some attribute.
     * We simply pick up the arguments that we stored here and replace
     * them by the function result.
     */
    if (type_key == nullptr)
        return nullptr;
    auto *pyside_globals = signatureGlobals();
    AutoDecRef strings{};
#ifdef Py_GIL_DISABLED
    PyObject *numkeyRaw = nullptr;
    const int haveNumkey = PyDict_GetItemRef(pyside_globals->arg_dict, type_key, &numkeyRaw);
    AutoDecRef numkeyRef(numkeyRaw);
    if (haveNumkey < 0)
        return nullptr;
    if (haveNumkey == 0)
        return PyDict_New();
    PyObject *numkey = numkeyRef.object();
    if (PyDict_Check(numkey))
        return numkeyRef.release(); // another thread already published parsed properties
#else
    PyObject *numkey = PyDict_GetItem(pyside_globals->arg_dict, type_key);
#endif
    if (PyTuple_Check(numkey)) {
        // The tuple itself is strongly pinned on FT and tuples are immutable, so
        // its borrowed items remain valid for the complete conversion below.
        PyObject *obAddress = PyTuple_GetItem(numkey, 0);
        PyObject *obSize = PyTuple_GetItem(numkey, 1);
        const void *addr = PyLong_AsVoidPtr(obAddress);
        const Py_ssize_t size = PyLong_AsSsize_t(obSize);
        const char **cstrings = bytesToStrings(reinterpret_cast<const uint8_t *>(addr), size);
        if (cstrings == nullptr)
            return nullptr;
        strings.reset(_address_ptr_to_stringlist(cstrings));
    } else {
        strings.reset(_address_to_stringlist(numkey));
    }
    if (strings.isNull())
        return nullptr;
    AutoDecRef arg_tup(Py_BuildValue("(OO)", type_key, strings.object()));
    if (arg_tup.isNull())
        return nullptr;
#ifdef Py_GIL_DISABLED
    AutoDecRef candidate(PyObject_CallObject(pyside_globals->pyside_type_init_func, arg_tup));
    if (candidate.isNull()) {
        if (PyErr_Occurred())
            return nullptr;
        // No error: preserve the historical empty-result semantics without a
        // process-global lazy empty_dict publication.
        return PyDict_New();
    }
    // PYSIDE-1019: Build snake case versions before the candidate is published.
    if (insert_snake_case_variants(candidate.object()) < 0)
        return nullptr;

    PyObject *published = nullptr;
    int publishResult = 0;
    Py_BEGIN_CRITICAL_SECTION(pyside_globals->arg_dict);
    PyObject *current = PyDict_GetItem(pyside_globals->arg_dict, type_key);
    if (current != nullptr && PyDict_Check(current)) {
        published = Py_NewRef(current);
    } else {
        publishResult = PyDict_SetItem(pyside_globals->arg_dict, type_key, candidate.object());
        if (publishResult == 0)
            published = Py_NewRef(candidate.object());
    }
    Py_END_CRITICAL_SECTION();
    if (publishResult < 0)
        return nullptr;
    return published;
#else
    PyObject *dict = PyObject_CallObject(pyside_globals->pyside_type_init_func, arg_tup);
    if (dict == nullptr) {
        if (PyErr_Occurred())
            return nullptr;
        PyObject *&empty_dict = pyside_globals->empty_dict;
        // No error: return an empty dict.
        if (empty_dict == nullptr)
            empty_dict = PyDict_New();
        return empty_dict;
    }
    // PYSIDE-1019: Build snake case versions of the functions.
    if (insert_snake_case_variants(dict) < 0)
        return nullptr;
    // We replace the arguments by the result dict.
    if (PyDict_SetItem(pyside_globals->arg_dict, type_key, dict) < 0)
        return nullptr;
    return dict;
#endif
}
//
////////////////////////////////////////////////////////////////////////////

#ifdef PYPY_VERSION
static bool get_lldebug_flag()
{
    auto *dic = PySys_GetObject("pypy_translation_info");
    int lldebug = PyObject_IsTrue(PyDict_GetItemString(dic, "translation.lldebug"));
    int lldebug0 = PyObject_IsTrue(PyDict_GetItemString(dic, "translation.lldebug0"));
    return lldebug || lldebug0;
}

#endif

static int _finishSignaturesCommon(PyObject *module)
{
    /*
     * Note: This function crashed when called from PySide_BuildSignatureArgs.
     * Probably this was an import timing problem.
     *
     * Pep384: We need to switch this always on since we have no access
     * to the PyCFunction attributes. Therefore I simplified things
     * and always use our own mapping.
     */
    auto *pyside_globals = signatureGlobals();
    PyObject *key{};
    PyObject *func{};
#ifdef Py_GIL_DISABLED
    AutoDecRef obdictSnapshot(PyDict_Copy(PyModule_GetDict(module)));
    if (obdictSnapshot.isNull())
        return -1;
    PyObject *obdict = obdictSnapshot.object();
#else
    PyObject *obdict = PyModule_GetDict(module);
#endif
    Py_ssize_t pos = 0;

    // Here we collect all global functions to finish our mapping. On FT the
    // snapshot owns every borrowed key/value for the complete PyDict_Next walk.
    while (PyDict_Next(obdict, &pos, &key, &func)) {
        if (PyCFunction_Check(func))
            if (PyDict_SetItem(pyside_globals->map_dict, func, module) < 0)
                return -1;
    }
    // The finish_import function will not work the first time since phase 2
    // was not yet run. But that is ok, because the first import is always for
    // the shiboken module (or a test module).
    [[maybe_unused]] const char *name = PyModule_GetName(module);
    if (pyside_globals->finish_import_func == nullptr) {
        assert(std::strncmp(name, "PySide6.", 8) != 0);
        return 0;
    }
    // Call a Python function which has to finish something as well.
    AutoDecRef ret(PyObject_CallFunction(
        pyside_globals->finish_import_func, "(O)", module));
    return ret.isNull() ? -1 : 0;
}

static int PySide_FinishSignatures(PyObject *module, const char *signatures[])
{
#ifdef PYPY_VERSION
    static const bool have_problem = get_lldebug_flag();
    if (have_problem)
        return 0; // crash with lldebug at `PyDict_Next`
#endif

    // Initialization of module functions and resolving of static methods.
    const char *name = PyModule_GetName(module);
    if (name == nullptr)
        return -1;

    // we abuse the call for types, since they both have a __name__ attribute.
    if (PySide_BuildSignatureArgs(module, signatures) < 0)
        return -1;
    return _finishSignaturesCommon(module);
}

static int PySide_FinishSignaturesByte(PyObject *module, const uint8_t signatures[], size_t size)
{
#ifdef PYPY_VERSION
    static const bool have_problem = get_lldebug_flag();
    if (have_problem)
        return 0; // crash with lldebug at `PyDict_Next`
#endif
    const char *name = PyModule_GetName(module);
    if (name == nullptr)
        return -1;

    if (PySide_BuildSignatureArgsByte(module, signatures, size) < 0)
        return -1;
    return _finishSignaturesCommon(module);
}

////////////////////////////////////////////////////////////////////////////
//
// External functions interface
//
// These are exactly the supported functions from `signature.h`.
//

int InitSignatureStrings(PyTypeObject *type, const char *signatures[])
{
    // PYSIDE-2404: This function now also builds the mapping for static methods.
    //              It was one missing spot to let Lazy import work.
    init_shibokensupport_module();
    auto *ob_type = reinterpret_cast<PyObject *>(type);
    int ret = PySide_BuildSignatureArgs(ob_type, signatures);
    if (ret < 0 || _build_func_to_type(ob_type) < 0) {
        PyErr_Print();
        PyErr_SetNone(PyExc_ImportError);
    }
    return ret;
}

int InitSignatureBytes(PyTypeObject *type, const uint8_t signatures[], size_t size)
{
    // PYSIDE-2701: Store the compressed bytes and produce input for
    //              InitSignatureStrings later.
    init_shibokensupport_module();
    auto *obType = reinterpret_cast<PyObject *>(type);
    const int ret = PySide_BuildSignatureArgsByte(obType, signatures, size);
    if (ret < 0 || _build_func_to_type(obType) < 0) {
        PyErr_Print();
        PyErr_SetNone(PyExc_ImportError);
    }
    return ret;
}

int FinishSignatureInitialization(PyObject *module, const char *signatures[])
{
    /*
     * This function is called at the very end of a module initialization.
     * We now patch certain types to support the __signature__ attribute,
     * initialize module functions and resolve static methods.
     *
     * Still, it is not possible to call init phase 2 from here,
     * because the import is still running. Do it from Python!
     */
    init_shibokensupport_module();

#ifndef PYPY_VERSION
    static const bool patch_types = true;
#else
    // PYSIDE-535: On PyPy we cannot patch builtin types. This can be
    //             re-implemented later. For now, we use `get_signature`, instead.
    static const bool patch_types = false;
#endif

    if ((patch_types && PySide_PatchTypes() < 0)
        || PySide_FinishSignatures(module, signatures) < 0) {
        PyErr_Print();
        PyErr_SetNone(PyExc_ImportError);
    }
    return 0;
}

int FinishSignatureInitBytes(PyObject *module, const uint8_t signatures[], size_t size)
{
    init_shibokensupport_module();

#ifndef PYPY_VERSION
    static const bool patch_types = true;
#else
    // PYSIDE-535: On PyPy we cannot patch builtin types. This can be
    //             re-implemented later. For now, we use `get_signature`, instead.
    static const bool patch_types = false;
#endif

    if ((patch_types && PySide_PatchTypes() < 0)
        || PySide_FinishSignaturesByte(module, signatures, size) < 0) {
        return -1;
    }
    return 0;
}

static PyObject *adjustFuncName(const char *func_name)
{
    /*
     * PYSIDE-1019: Modify the function name expression according to feature.
     *
     * - snake_case
     *      The function name must be converted.
     * - full_property
     *      The property name must be used and "fset" appended.
     *
     *          modname.subname.classsname.propname.fset
     *
     *      Class properties must use the expression
     *
     *          modname.subname.classsname.__dict__['propname'].fset
     *
     * Note that fget is impossible because there are no parameters.
     */
    static const char mapping_name[] = "shibokensupport.signature.mapping";
#ifdef Py_GIL_DISABLED
    // Do not retain borrowed sys.modules/module-dict entries in process-global
    // C++ statics. Importing returns a strong module reference for this call.
    AutoDecRef mapping(PyImport_ImportModule(mapping_name));
    if (mapping.isNull())
        return nullptr;
    PyObject *ns = PyModule_GetDict(mapping.object());
#else
    static PyObject *sys_modules = PySys_GetObject("modules");
    static PyObject *mapping = PyDict_GetItemString(sys_modules, mapping_name);
    static PyObject *ns = PyModule_GetDict(mapping);
#endif

    char _path[200 + 1] = {};
    const char *_name = std::strrchr(func_name, '.');
    std::strncat(_path, func_name, _name - func_name);
    ++_name;

    // This is a very cheap call into `mapping.py`.
#ifdef Py_GIL_DISABLED
    AutoDecRef updateMapping(PyObject_GetAttrString(mapping.object(), "update_mapping"));
    if (updateMapping.isNull())
        return nullptr;
    AutoDecRef res(PyObject_CallFunctionObjArgs(updateMapping.object(), nullptr));
#else
    PyObject *update_mapping = PyDict_GetItemString(ns, "update_mapping");
    AutoDecRef res(PyObject_CallFunctionObjArgs(update_mapping, nullptr));
#endif
    if (res.isNull())
        return nullptr;

    // Run `eval` on the type string to get the object.
    // PYSIDE-1710: If the eval does not work, return the given string.
    AutoDecRef obtype(PyRun_String(_path, Py_eval_input, ns, ns));
    if (obtype.isNull())
        return String::fromCString(func_name);

    if (PyModule_Check(obtype.object())) {
        // This is a plain function. Return the unmangled name.
        return String::fromCString(func_name);
    }
    assert(PyType_Check(obtype));   // This was not true for __init__!

    // Find the feature flags
    auto *type = reinterpret_cast<PyTypeObject *>(obtype.object());
    AutoDecRef dict(PepType_GetDict(type));
    int id = currentSelectId(type);
    id = id < 0 ? 0 : id;   // if undefined, set to zero
    auto lower = id & 0x01;
    auto is_prop = id & 0x02;
    bool is_class_prop = false;

    // Compute all needed info.
#ifdef Py_GIL_DISABLED
    AutoDecRef nameRef(String::getSnakeCaseName(_name, lower));
    if (nameRef.isNull())
        return nullptr;
    PyObject *name = nameRef.object();
#else
    PyObject *name = String::getSnakeCaseName(_name, lower);
#endif
    PyObject *prop_name{};
#ifdef Py_GIL_DISABLED
    AutoDecRef propNameRef;
    AutoDecRef propRef;
    if (is_prop) {
        PyObject *propMethodsRaw = nullptr;
        const int havePropMethods = PyDict_GetItemRef(dict.object(),
                                                       PyMagicName::property_methods(),
                                                       &propMethodsRaw);
        AutoDecRef propMethods(propMethodsRaw);
        if (havePropMethods < 0)
            return nullptr;
        if (havePropMethods > 0) {
            PyObject *propNameRaw = nullptr;
            const int havePropName = PyDict_GetItemRef(propMethods.object(), name,
                                                       &propNameRaw);
            if (havePropName < 0)
                return nullptr;
            propNameRef.reset(propNameRaw);
            prop_name = propNameRef.object();
        }
        if (prop_name != nullptr) {
            PyObject *propRaw = nullptr;
            const int haveProp = PyDict_GetItemRef(dict.object(), prop_name, &propRaw);
            if (haveProp < 0)
                return nullptr;
            propRef.reset(propRaw);
            if (haveProp > 0)
                is_class_prop = Py_TYPE(propRef.object()) != &PyProperty_Type;
        }
    }
#else
    if (is_prop) {
        PyObject *prop_methods = PyDict_GetItem(dict, PyMagicName::property_methods());
        prop_name = PyDict_GetItem(prop_methods, name);
        if (prop_name != nullptr) {
            PyObject *prop = PyDict_GetItem(dict, prop_name);
            is_class_prop = Py_TYPE(prop) != &PyProperty_Type;
        }
    }
#endif

    // Finally, generate the correct path expression.
    char _buf[250 + 1] = {};
    if (prop_name) {
        const auto *_prop_name = String::toCString(prop_name);
        if (is_class_prop)
            snprintf(_buf, sizeof(_buf), "%s.__dict__['%s'].fset", _path, _prop_name);
        else
            snprintf(_buf, sizeof(_buf), "%s.%s.fset", _path, _prop_name);
    }
    else {
        const auto *_name = String::toCString(name);
        snprintf(_buf, sizeof(_buf), "%s.%s", _path, _name);
    }
    return String::fromCString(_buf);
}

void SetError_Argument(PyObject *args, const char *func_name, PyObject *info)
{
    init_shibokensupport_module();
    /*
     * This function replaces the type error construction with extra
     * overloads parameter in favor of using the signature module.
     * Error messages are rare, so we do it completely in Python.
     */

    // PYSIDE-1305: Handle errors set by fillQtProperties.
    if (PyErr_Occurred()) {
        PyObject *e{};
        PyObject *v{};
        PyObject *t{};
        // Note: These references are all borrowed.
        PyErr_Fetch(&e, &v, &t);
        Py_DECREF(e);
        info = v;
        Py_XDECREF(t);
    }
    // PYSIDE-1019: Modify the function name expression according to feature.
    AutoDecRef new_func_name(adjustFuncName(func_name));
    if (new_func_name.isNull()) {
        PyErr_Print();
        Py_FatalError("libshiboken: seterror_argument failed to call update_mapping");
    }
    if (info == nullptr)
        info = Py_None;
    AutoDecRef res(PyObject_CallFunctionObjArgs(signatureGlobals()->seterror_argument_func,
                                                args, new_func_name.object(), info, nullptr));
    if (res.isNull()) {
        PyErr_Print();
        Py_FatalError("libshiboken: seterror_argument did not receive a result");
    }
    PyObject *err{};
    PyObject *msg{};
    if (!PyArg_UnpackTuple(res, func_name, 2, 2, &err, &msg)) {
        PyErr_Print();
        Py_FatalError("libshiboken: unexpected failure in seterror_argument");
    }
    PyErr_SetObject(err, msg);
}

/*
 * Support for the metatype SbkObjectType_Type's tp_getset.
 *
 * This was not necessary for __signature__, because PyType_Type inherited it.
 * But the __doc__ attribute existed already by inheritance, and calling
 * PyType_Modified() is not supported. So we added the getsets explicitly
 * to the metatype.
 *
 * PYSIDE-2101: The __signature__ attribute is gone due to rlcompleter.
 */

PyObject *Sbk_TypeGet___doc__(PyObject *ob, void *)
{
    init_shibokensupport_module();
    return pyside_tp_get___doc__(ob);
}

PyObject *GetFeatureDict()
{
    init_shibokensupport_module();
    return signatureGlobals()->feature_dict;
}

} //extern "C"
