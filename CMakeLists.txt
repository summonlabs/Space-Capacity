# Space Capacity - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the benchmarks and the tests each carry exactly the
# options they need and nothing this project uses for itself can leak into the
# installed package.

cmake_minimum_required(VERSION 3.20)

project(SpaceCapacity
        VERSION 1.0.0
        DESCRIPTION "Physical-space capacity accounting for the Data Center Control Plane"
        HOMEPAGE_URL "https://github.com/summonlabs/Space-Capacity"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(SPACE_CAPACITY_BUILD_TESTS "Build the Space Capacity test suite" ON)
option(SPACE_CAPACITY_BUILD_TOOLS "Build the Space Capacity command line tools" ON)
option(SPACE_CAPACITY_BUILD_EXAMPLES "Build the Space Capacity example programs" ON)
option(SPACE_CAPACITY_BUILD_BENCHMARKS "Build the Space Capacity benchmarks" ON)
option(SPACE_CAPACITY_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(SPACE_CAPACITY_ENABLE_ASAN "Build with AddressSanitizer" OFF)

# Space Capacity is built as a static library. The versioned CMake package and
# the namespaced target are unaffected by that choice.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Space Capacity is built as a static library" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(SPACE_CAPACITY_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/SpaceCapacity"
    CACHE STRING "Install destination of the Space Capacity CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it becomes part of the installed package.
function(space_capacity_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(SPACE_CAPACITY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(SPACE_CAPACITY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(SPACE_CAPACITY_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# Adds one executable and applies the project build policy to it.
function(space_capacity_add_executable target)
  add_executable(${target} ${ARGN})
  space_capacity_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(SPACE_CAPACITY_PUBLIC_HEADERS
  include/dccp/space_capacity/capacity.hpp
  include/dccp/space_capacity/checked.hpp
  include/dccp/space_capacity/claim.hpp
  include/dccp/space_capacity/diff.hpp
  include/dccp/space_capacity/digest.hpp
  include/dccp/space_capacity/error.hpp
  include/dccp/space_capacity/evidence.hpp
  include/dccp/space_capacity/export.hpp
  include/dccp/space_capacity/limits.hpp
  include/dccp/space_capacity/model.hpp
  include/dccp/space_capacity/observation.hpp
  include/dccp/space_capacity/query.hpp
  include/dccp/space_capacity/region.hpp
  include/dccp/space_capacity/registry.hpp
  include/dccp/space_capacity/registry_ref.hpp
  include/dccp/space_capacity/requests.hpp
  include/dccp/space_capacity/snapshot.hpp
  include/dccp/space_capacity/space_capacity.hpp
  include/dccp/space_capacity/store.hpp
  include/dccp/space_capacity/strong_id.hpp
  include/dccp/space_capacity/text.hpp
  include/dccp/space_capacity/units.hpp
  include/dccp/space_capacity/version.hpp)

set(SPACE_CAPACITY_SOURCES
  src/capacity.cpp
  src/claim.cpp
  src/codec.cpp
  src/diff.cpp
  src/digest.cpp
  src/engine.cpp
  src/error.cpp
  src/evidence.cpp
  src/geometry.cpp
  src/model.cpp
  src/mutations.cpp
  src/observation.cpp
  src/platform.cpp
  src/query.cpp
  src/region.cpp
  src/registry.cpp
  src/registry_ref.cpp
  src/snapshot.cpp
  src/store.cpp
  src/strong_id.cpp
  src/text.cpp
  src/units.cpp
  src/version.cpp)

add_library(space_capacity ${SPACE_CAPACITY_SOURCES} ${SPACE_CAPACITY_PUBLIC_HEADERS})
add_library(dccp::space_capacity ALIAS space_capacity)

target_include_directories(space_capacity
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties(space_capacity PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  EXPORT_NAME space_capacity)

space_capacity_configure_target(space_capacity)

# ---------------------------------------------------------------------------
# Tools, examples, tests and benchmarks
# ---------------------------------------------------------------------------

if(SPACE_CAPACITY_BUILD_TOOLS)
  space_capacity_add_executable(spacecap tools/spacecap.cpp)
  target_link_libraries(spacecap PRIVATE dccp::space_capacity)
  set_target_properties(spacecap PROPERTIES OUTPUT_NAME spacecap)

  if(SPACE_CAPACITY_BUILD_TESTS)
    # Process-level proof harnesses. They are real programs that a test starts
    # as separate operating-system processes to inject process death at a
    # durable step and to compete for writer authority. They are not installed.
    space_capacity_add_executable(sc_crash_child tools/crash_child.cpp)
    target_link_libraries(sc_crash_child PRIVATE dccp::space_capacity)
    space_capacity_add_executable(sc_fence_child tools/fence_child.cpp)
    target_link_libraries(sc_fence_child PRIVATE dccp::space_capacity)
  endif()
endif()

if(SPACE_CAPACITY_BUILD_EXAMPLES)
  foreach(example IN ITEMS
      example_containment_ledger
      example_durable_store
      example_fit_assessment)
    space_capacity_add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE dccp::space_capacity)
  endforeach()
endif()

if(SPACE_CAPACITY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SPACE_CAPACITY_BUILD_BENCHMARKS)
  space_capacity_add_executable(bench_space_capacity benchmarks/bench_space_capacity.cpp)
  target_link_libraries(bench_space_capacity PRIVATE dccp::space_capacity)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS space_capacity
        EXPORT SpaceCapacityTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/dccp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

if(SPACE_CAPACITY_BUILD_TOOLS)
  install(TARGETS spacecap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT SpaceCapacityTargets
        FILE SpaceCapacityTargets.cmake
        NAMESPACE dccp::
        DESTINATION ${SPACE_CAPACITY_INSTALL_CMAKEDIR})

configure_package_config_file(
  cmake/SpaceCapacityConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/SpaceCapacityConfig.cmake"
  INSTALL_DESTINATION ${SPACE_CAPACITY_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/SpaceCapacityConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/SpaceCapacityConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/SpaceCapacityConfigVersion.cmake"
        DESTINATION ${SPACE_CAPACITY_INSTALL_CMAKEDIR})

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "Space Capacity ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests               : ${SPACE_CAPACITY_BUILD_TESTS}")
message(STATUS "  tools               : ${SPACE_CAPACITY_BUILD_TOOLS}")
message(STATUS "  examples            : ${SPACE_CAPACITY_BUILD_EXAMPLES}")
message(STATUS "  benchmarks          : ${SPACE_CAPACITY_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors  : ${SPACE_CAPACITY_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer    : ${SPACE_CAPACITY_ENABLE_ASAN}")
