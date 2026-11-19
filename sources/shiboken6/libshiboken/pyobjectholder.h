// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#ifndef PYOBJECTHOLDER_H
#define PYOBJECTHOLDER_H

#include "sbkpython.h"
#include "gilstate.h"

#include <cassert>
#include <utility>

namespace Shiboken
{

/// PyObjectHolder holds a PyObject pointer, keeping a reference decrementing
/// its reference counter when destroyed. It makes sure to hold the GIL when
/// releasing. It implements copy/move semantics and is mainly intended as a
/// base class for functors holding a callable which can be passed around and
/// stored in containers or moved from freely.
/// For one-shot functors, release() can be invoked after the call.
class PyObjectHolder
{
public:
    PyObjectHolder() noexcept = default;

    /// PyObjectHolder constructor.
    /// \param pyobj A reference to a Python object
    explicit PyObjectHolder(PyObject *pyObj) noexcept : m_pyObj(pyObj)
    {
        assert(pyObj != nullptr);
        Py_INCREF(m_pyObj);
    }

    PyObjectHolder(const PyObjectHolder &o) noexcept : m_pyObj(o.m_pyObj)
    {
        Py_XINCREF(m_pyObj);
    }

    PyObjectHolder &operator=(const PyObjectHolder &o) noexcept
    {
        if (this != &o) {
            PyObjectHolder replacement(o);
            // Publish the replacement before releasing the previous object.
            std::swap(m_pyObj, replacement.m_pyObj);
        }
        return *this;
    }

    PyObjectHolder(PyObjectHolder &&o) noexcept : m_pyObj{std::exchange(o.m_pyObj, nullptr)} {}

    PyObjectHolder &operator=(PyObjectHolder &&o) noexcept
    {
        if (this != &o) {
            PyObjectHolder replacement(std::move(o));
            std::swap(m_pyObj, replacement.m_pyObj);
        }
        return *this;
    }

    /// Decref the python reference
    ~PyObjectHolder() { release(); }

    [[nodiscard]] bool isNull() const { return m_pyObj == nullptr; }
    [[nodiscard]] operator bool() const { return m_pyObj != nullptr; }

    /// Returns the pointer of the Python object being held.
    [[nodiscard]] PyObject *object() const { return m_pyObj; }
    [[nodiscard]] operator PyObject *() const { return m_pyObj; }

    [[nodiscard]] PyObject *operator->() { return m_pyObj; }

protected:
    void release()
    {
        auto *previous = std::exchange(m_pyObj, nullptr);
        if (previous == nullptr)
            return;
        if (!isPythonRuntimeAvailable())
            return;
        GilState gil;
        Py_DECREF(previous);
    }

private:
    PyObject *m_pyObj = nullptr;
};

} // namespace Shiboken

#endif // PYOBJECTHOLDER_H
