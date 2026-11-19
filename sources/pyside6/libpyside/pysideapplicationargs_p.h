// SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#ifndef PYSIDE_APPLICATIONARGS_P_H
#define PYSIDE_APPLICATIONARGS_P_H

#include <sbkpython.h>
#include <autodecref.h>
#include <helper.h>
#include <QtCore/qobject.h>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace PySide::Detail {

class ApplicationArgumentStorage
{
public:
    ApplicationArgumentStorage() = default;
    ApplicationArgumentStorage(const ApplicationArgumentStorage &) = delete;
    ApplicationArgumentStorage &operator=(const ApplicationArgumentStorage &) = delete;
    ~ApplicationArgumentStorage()
    {
        if (m_captured) {
            // Qt may remove options and reorder argv. Free the original strings.
            for (char *argument : m_originalArguments)
                delete [] argument;
            delete [] m_argv;
        } else {
            // Initialization failed before Qt could mutate the pointer array.
            Shiboken::deleteArgv(m_argc, m_argv);
        }
    }

    bool initialize(PyObject *arguments, const char *defaultName)
    {
        if (!PyList_Check(arguments))
            return false;
        // No external thread can mutate this private snapshot during conversion.
        Shiboken::AutoDecRef snapshot(PySequence_List(arguments));
        if (snapshot.isNull()
            || !Shiboken::listToArgcArgv(snapshot, &m_argc, &m_argv, defaultName))
            return false;
        m_originalArguments.assign(m_argv, m_argv + m_argc);
        m_captured = true;
        // Keep argv[argc] == nullptr even if Qt removes command-line options.
        auto mutableArguments = std::make_unique<char *[]>(std::size_t(m_argc) + 1);
        std::copy(m_originalArguments.begin(), m_originalArguments.end(), mutableArguments.get());
        delete [] m_argv;
        m_argv = mutableArguments.release();
        return true;
    }

    int &argc() { return m_argc; }
    char **argv() const { return m_argv; }

private:
    int m_argc = 0;
    char **m_argv = nullptr;
    bool m_captured = false;
    std::vector<char *> m_originalArguments;
};

} // namespace PySide::Detail

namespace PySide {

template <typename Application, typename... Extra>
bool constructApplicationWithArguments(PyObject *arguments, Application **result,
                                       const char *defaultName, Extra &&...extra)
{
    auto storage = std::make_shared<Detail::ApplicationArgumentStorage>();
    if (!storage->initialize(arguments, defaultName))
        return false;
    // argc is a reference retained by Qt, so its address must also stay stable.
    auto application = std::make_unique<Application>(storage->argc(), storage->argv(),
                                                     std::forward<Extra>(extra)...);
    const auto connection = QObject::connect(application.get(), &QObject::destroyed,
        [storage](QObject *) { (void)storage; });
    if (!connection) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot bind Qt application argument lifetime");
        return false;
    }
    *result = application.release();
    return true;
}

} // namespace PySide
#endif // PYSIDE_APPLICATIONARGS_P_H
