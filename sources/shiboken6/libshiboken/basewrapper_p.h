// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef BASEWRAPPER_P_H
#define BASEWRAPPER_P_H

#include "sbkpython.h"
#include "basewrapper.h"
#include "sbkrelaxedflag.h"
#ifdef Py_GIL_DISABLED
#  include "sbkstatelock.h"
#  include <atomic>
#endif

#include <unordered_map>
#include <set>
#include <string>
#include <vector>
#include <iosfwd>

struct SbkObject;
struct SbkConverter;
struct SbkDynamicMethodDef;
namespace Shiboken::Object { struct PendingDestruction; }

namespace Shiboken
{
/**
    * This mapping associates a method and argument of an wrapper object with the wrapper of
    * said argument when it needs the binding to help manage its reference count.
    */
class RefCountMap : public std::unordered_multimap<std::string, PyObject *>
{
    using Base = std::unordered_multimap<std::string, PyObject *>;
public:
    using Base::Base;

    void setInvalidates(const std::string &key, bool invalidate)
    {
        if (invalidate)
            m_independentKeys.erase(key);
        else
            m_independentKeys.insert(key);
    }

    bool invalidates(const std::string &key) const
    {
        return m_independentKeys.find(key) == m_independentKeys.end();
    }

    void clear()
    {
        Base::clear();
        m_independentKeys.clear();
    }
private:
    std::set<std::string> m_independentKeys;
};

// CPython clears Py_IsInitialized() before clearing module globals. Owned
// native-provider references must still be released during that finalization,
// while an attached thread state guarantees that the C API remains usable.
inline bool canReleaseOwnedPythonReferences() noexcept
{
#ifdef Py_GIL_DISABLED
    return PyThreadState_GetUnchecked() != nullptr;
#else
    return Py_IsInitialized() != 0;
#endif
}

/// Linked list of SbkBaseWrapper pointers
using ChildrenList = std::set<SbkObject *>;


/// Structure used to store information about object parent and children.
struct ParentInfo
{
    /// Pointer to parent object.
    SbkObject *parent = nullptr;
    /// List of object children.
    ChildrenList children;
    /// has internal ref
    bool hasWrapperRef = false;
};

} // namespace Shiboken

extern "C"
{

/**
 * \internal
 * Private data for SbkBaseWrapper
 */
struct SbkObjectPrivate
{
    SbkObjectPrivate() noexcept = default;
    SbkObjectPrivate(const SbkObjectPrivate &) = delete;
    SbkObjectPrivate(SbkObjectPrivate &&o) = delete;
    SbkObjectPrivate &operator=(const SbkObjectPrivate &) = delete;
    SbkObjectPrivate &operator=(SbkObjectPrivate &&o) = delete;

    /// Pointer to the C++ class.
    void ** cptr = nullptr;
    int cptrCount = 0;
    // Immutable allocation layout, independent of later Python __bases__ changes.
    // Each native provider owns one reference, visited by tp_traverse.
    std::vector<PyTypeObject *> cppTypes;
    // Read inside destruction transactions without any Python C API access.
    std::vector<ObjectDestructor> cppDestructors;
    // Reserved C++ slots for a generated constructor. Only the same Python
    // thread may recursively initialize another native base. State lock on FT.
    std::vector<int> constructionSlots;
    unsigned long constructionThread = 0;
#ifdef Py_GIL_DISABLED
    // One memory location per flag: a run of bit-fields shares a byte, so an
    // unlocked writer would revert what a lock holder just wrote. See
    // RelaxedFlag; the meaning of each flag is in the twin below.
    Shiboken::RelaxedFlag hasOwnership;
    Shiboken::RelaxedFlag containsCppWrapper;
    Shiboken::RelaxedFlag validCppObject;
    Shiboken::RelaxedFlag cppObjectCreated;
    Shiboken::RelaxedFlag isQAppSingleton;
    /// Set once Shiboken.delete() has been requested. No new call lease is
    /// handed out from that point on, so the object is unreachable for new
    /// calls while in-flight calls finish. Destruction from the C++ side
    /// cannot defer and clears validCppObject and cptr instead, which refuses
    /// a lease just the same.
    /// State lock.
    ///
    /// One-way: nothing clears it again. A wrapper marked here is refused for
    /// good, even where a build with a GIL would let the call through - into
    /// memory the C++ destructor has freed.
    Shiboken::RelaxedFlag pendingDestruction;
    /// Number of C++ calls currently using cptr under a call lease. The C++
    /// object is not destroyed while this is non-zero; the last lease release
    /// runs a destruction that was requested meanwhile. State lock.
    unsigned int activeCalls;
    // Immutable native-provider policy, independent of mutable Python bases.
    bool deleteInMainThread = false;
    // Intrusive destruction bookkeeping; all access is under the state lock.
    Shiboken::Object::PendingDestruction *destructionPlan = nullptr;
    SbkObject *destructionNext = nullptr;
    bool destructionVisiting = false;
    bool destructionMember = false;
    bool destructionDropExtra = false;
    unsigned destructionRefsToDrop = 0;
    void **destructionPointers = nullptr;
    Shiboken::RefCountMap *destructionReferences = nullptr;
#else // Py_GIL_DISABLED
    /// True when Python is responsible for freeing the used memory.
    unsigned int hasOwnership : 1;
    /// This is true when the C++ class of the wrapped object has a virtual destructor AND was created by Python.
    unsigned int containsCppWrapper : 1;
    /// Marked as false when the object is lost to C++ and the binding can not know if it was deleted or not.
    unsigned int validCppObject : 1;
    /// Marked as true when the object constructor was called
    unsigned int cppObjectCreated : 1;
    /// PYSIDE-1470: Marked as true if this is the Q*Application singleton.
    /// This bit allows app deletion from shiboken?.delete() .
    unsigned int isQAppSingleton : 1;
#endif // Py_GIL_DISABLED
    /// Information about the object parents and children, may be null.
    Shiboken::ParentInfo *parentInfo = nullptr;
    /// Manage reference count of objects that are referred to but not owned from.
    Shiboken::RefCountMap *referredObjects = nullptr;

    ~SbkObjectPrivate()
    {
        if (Shiboken::canReleaseOwnedPythonReferences()) {
            for (auto *type : cppTypes)
                Py_DECREF(type);
        }
        delete parentInfo;
        parentInfo = nullptr;
        delete referredObjects;
        referredObjects = nullptr;
    }
};

// TODO-CONVERTERS: to be deprecated/removed
/// The type behaviour was not defined yet
#define BEHAVIOUR_UNDEFINED 0
/// The type is a value type
#define BEHAVIOUR_VALUETYPE 1
/// The type is an object type
#define BEHAVIOUR_OBJECTTYPE 2

struct SbkObjectTypePrivate
{
    SbkConverter *converter;
    int *mi_offsets;
    MultipleInheritanceInitFunction mi_init;

    /// Special cast function, null if this class doesn't have multiple inheritance.
    SpecialCastFunction mi_specialcast;
    TypeDiscoveryFuncV2 type_discovery;
    /// Pointer to a function responsible for deletion of the C++ instance calling the proper destructor.
    ObjectDestructor cpp_dtor;
    /// C++ name
    char *original_name;
    /// Type user data
    void *user_data;
    DeleteUserDataFunc d_func;
    void (*subtype_init)(PyTypeObject *, PyObject *, PyObject *);
    const char **propertyStrings;
    const char **enumFlagInfo;
    PyObject *enumFlagsDict;
    PyObject *enumTypeDict;
#ifdef Py_GIL_DISABLED
    // Own the factory reference backing generated raw type-cache pointers.
    PyTypeObject *generatedTypeReference;
    SbkObjectTypePrivate *nextGeneratedTypeReference;
#endif

    /// True if this type holds two or more C++ instances, e.g.: a Python class which inherits from two C++ classes.
    unsigned int is_multicpp : 1;
    /// True if this type was defined by the user (a class written in Python inheriting
    /// a class provided by a Shiboken binding).
    unsigned int is_user_type : 1;
    /// Tells is the type is a value type or an object-type, see BEHAVIOUR_ *constants.
    unsigned int type_behaviour : 2;
    unsigned int delete_in_main_thread : 1;
    // Bound C functions and descriptors keep this type alive. Definitions are
    // released after CPython has disposed of its dictionary and descriptors.
    SbkDynamicMethodDef *dynamicMethodDefs;
};


} // extern "C"

namespace Shiboken
{
/**
 * Utility function used to transform a PyObject that implements sequence protocol into a std::list.
 **/
std::vector<SbkObject *> splitPyObject(PyObject *pyObj);

int getNumberOfCppBaseClasses(PyTypeObject *baseType);
#ifdef Py_GIL_DISABLED
namespace ObjectType {
void releaseGeneratedTypeReferences();
}
#endif

namespace ObjectType {
LIBSHIBOKEN_API PyMethodDef *createOwnedMethodDef(PyTypeObject *type, const PyMethodDef *method,
                                                const char *name, int flags);
}

namespace Object
{
/**
*   Decrements the reference counters of every object referred by self.
*   \param self    the wrapper instance that keeps references to other objects.
*/
void clearReferences(SbkObject *self);

/**
 * Destroy internal data
 **/
void deallocData(SbkObject *self, bool doCleanup);


void _debugFormat(std::ostream &str, SbkObject *self);
} // namespace Object

inline PyTypeObject *pyType(SbkObject *sbo)
{
    return Py_TYPE(reinterpret_cast<PyObject *>(sbo));
}

} // namespace Shiboken

#endif
