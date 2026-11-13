#pragma once

// Shared embedded-interpreter bootstrap for slic3rutils tests that need a live Python
// interpreter (test_plugin_host_api.cpp, test_slicing_pipeline_bindings.cpp, ...).
//
// The runtime is process-wide: once initialized it stays up for the rest of the test process.
// CPython cannot be reinitialized after Py_Finalize (a second init aborts inside _datetime), and
// the app likewise finalizes it only at exit, so nothing here tears it down.
// slic3rutils_tests_main.cpp finalizes the production interpreter once, after the last test case.
//
// There is exactly one bring-up: PythonInterpreter::initialize(), the same one
// PluginManager::initialize() uses. An earlier private scoped_interpreter here made whichever
// of the two ran first own Python, so in one process (declared order, or any random order) the
// other either failed with "The interpreter is already running" or ran Python without the GIL
// (PythonInterpreter releases it after init) and crashed. PythonInterpreter leaves the GIL
// released, so ensure_python_initialized() takes it for the rest of the test case and the Catch
// listener in slic3rutils_tests_main.cpp calls release_test_gil() when the case ends.
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <memory.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>

#include "plugin_test_utils.hpp"

// Outside the anonymous namespace so every translation unit shares one holder.
// Held by the main thread from ensure_python_initialized() until the test case ends.
inline std::unique_ptr<pybind11::gil_scoped_acquire>& test_gil_holder()
{
    static std::unique_ptr<pybind11::gil_scoped_acquire> holder;
    return holder;
}

inline void release_test_gil() { test_gil_holder().reset(); }

namespace {

void ensure_python_initialized()
{
    if (test_gil_holder())
        return;

    auto& interpreter = Slic3r::PythonInterpreter::instance();
    if (!interpreter.is_initialized()) {
        // The interpreter records data_dir() paths (bytecode cache, shared packages) at init.
        Slic3r::ScopedDataDir data_dir("python-test-support");
        if (!interpreter.initialize())
            throw std::runtime_error("PythonInterpreter::initialize failed: " + interpreter.last_error());
    }
    test_gil_holder() = std::make_unique<pybind11::gil_scoped_acquire>();
}

pybind11::module_ import_orca_module()
{
    ensure_python_initialized();

    // Force PythonPluginBridge.cpp into the test binary so the embedded
    // PYBIND11_EMBEDDED_MODULE(orca, ...) registration is available.
    (void) Slic3r::PythonPluginBridge::instance();
    return pybind11::module_::import("orca");
}

} // namespace
