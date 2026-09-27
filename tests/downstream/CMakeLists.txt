# Space Capacity - independent downstream consumer.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately NOT part of the Space Capacity build. It is
# configured on its own against an INSTALLED Space Capacity package, so that it
# proves the exported CMake package works from an arbitrary prefix with no
# build-tree path baked into it.
#
#   cmake -S tests/downstream -B <somewhere> -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build <somewhere> --config Release
#   <somewhere>/space_capacity_downstream

cmake_minimum_required(VERSION 3.20)

project(SpaceCapacityDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SpaceCapacity 1.0 REQUIRED)

if(NOT TARGET dccp::space_capacity)
  message(FATAL_ERROR "dccp::space_capacity was not provided by the installed package")
endif()

add_executable(space_capacity_downstream main.cpp)
target_link_libraries(space_capacity_downstream PRIVATE dccp::space_capacity)

if(MSVC)
  target_compile_options(space_capacity_downstream PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_options(space_capacity_downstream PRIVATE /WX)
  target_compile_definitions(space_capacity_downstream PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(space_capacity_downstream PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "Space Capacity package version: ${SpaceCapacity_VERSION}")
message(STATUS "Space Capacity libraries:       ${SPACE_CAPACITY_LIBRARIES}")
message(STATUS "Space Capacity include dirs:    ${SPACE_CAPACITY_INCLUDE_DIRS}")
