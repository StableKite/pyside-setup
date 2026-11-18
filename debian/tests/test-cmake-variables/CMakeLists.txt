cmake_minimum_required(VERSION 3.20)
project(trace_pyside CXX)

message(STATUS "==== IMPORTING PACKAGES ====")
find_package(PySide6 REQUIRED)
find_package(Shiboken6 REQUIRED)
find_package(Shiboken6Tools REQUIRED)

message(STATUS "")
message(STATUS "==== VARIABLES ====")
get_cmake_property(allvars VARIABLES)
list(SORT allvars)
foreach(v IN LISTS allvars)
  if(v MATCHES "^(PYSIDE|PySide|SHIBOKEN|Shiboken|_pyside|_shiboken|Python)")
    message(STATUS " ${v} = [${${v}}]")
  endif()
endforeach()


message(STATUS "")
message(STATUS "==== TARGET PROPERTIES ====")

foreach(t
  PySide6::pyside6
  Shiboken6::libshiboken
  Shiboken6::shiboken6
)
  if(TARGET "${t}")
    message(STATUS "--- ${t} ---")

    foreach(prop
      INTERFACE_INCLUDE_DIRECTORIES
      INTERFACE_LINK_LIBRARIES
      INTERFACE_COMPILE_DEFINITIONS
      INTERFACE_COMPILE_OPTIONS
      IMPORTED_CONFIGURATIONS
      IMPORTED_LOCATION
      IMPORTED_LOCATION_RELEASE
      IMPORTED_IMPLIB
      IMPORTED_IMPLIB_RELEASE
    )
      get_target_property(val "${t}" "${prop}")
      if(val STREQUAL "val-NOTFOUND")
        message(STATUS "  ${prop} = <UNSET>")
      else()
        message(STATUS "  ${prop} = [${val}]")
      endif()
    endforeach()
  else()
    message(STATUS "--- ${t} does not exist ---")
  endif()
endforeach()
