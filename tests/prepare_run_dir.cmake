# Space Capacity - per-test run directory preparation.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Every test gets its own working directory, emptied before it runs, so a test
# that writes a durable store can never collide with another test and a second
# `ctest` invocation starts from the same clean state as the first.
#
# Usage: cmake -DSC_RUN_DIR=<path> -P prepare_run_dir.cmake

if(NOT DEFINED SC_RUN_DIR)
  message(FATAL_ERROR "SC_RUN_DIR is required")
endif()

file(REMOVE_RECURSE "${SC_RUN_DIR}")
file(MAKE_DIRECTORY "${SC_RUN_DIR}")
