# Copyright (C) 2023 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

project(libsmart)

set(libsmart_SRC
libsmartmacros.h
smart.cpp smart.h
smart_integer.h
smart_obj.h
smart_registry.h
smart_sharedptr.h
smart_test.h
stdoptionaltestbench.cpp stdoptionaltestbench.h
stdsharedptrtestbench.cpp stdsharedptrtestbench.h
stduniqueptrtestbench.cpp stduniqueptrtestbench.h
)

add_library(libsmart SHARED ${libsmart_SRC})
target_include_directories(libsmart PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(libsmart PRIVATE LIBSMART_BUILD)
set_property(TARGET libsmart PROPERTY PREFIX "")

# The Python ownership tests depend on a registry that counts C++ lifetimes,
# independently of compiler copy elision and move construction.
add_executable(smart_cpp_registry_lifetime smart_registry_lifetime_test.cpp)
target_link_libraries(smart_cpp_registry_lifetime PRIVATE libsmart)
add_test(NAME smart_cpp_registry_lifetime COMMAND smart_cpp_registry_lifetime)
