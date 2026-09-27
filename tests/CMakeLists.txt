# Space Capacity - test suite.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Every test is a plain program. CTest runs them without a timeout, because a
# hanging test is a defect to diagnose rather than a test to abort.
#
# Each test runs in its own working directory, prepared and emptied by a setup
# test, so a test that writes a durable store never collides with another test
# and a second `ctest` invocation starts as clean as the first.

set(SPACE_CAPACITY_TESTS
  units_test
  identity_test
  model_test
  claim_region_test
  accounting_test
  rollup_test
  fit_test
  observation_test
  diff_test
  persistence_test
  corruption_test
  adversarial_test
  property_test
  randomized_test
  concurrency_test
  multiprocess_test
  installed_api_test)

set(SPACE_CAPACITY_RUN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/run")

foreach(test IN LISTS SPACE_CAPACITY_TESTS)
  space_capacity_add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE dccp::space_capacity)
  target_include_directories(${test} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

  set(run_dir "${SPACE_CAPACITY_RUN_ROOT}/${test}")

  # The setup test empties and recreates the working directory. CTest runs it
  # before the test that requires the fixture.
  add_test(NAME ${test}_prepare
           COMMAND ${CMAKE_COMMAND} -DSC_RUN_DIR=${run_dir}
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare_run_dir.cmake")
  add_test(NAME ${test} COMMAND ${test})
  set_tests_properties(${test}_prepare PROPERTIES FIXTURES_SETUP "${test}_dir")
  set_tests_properties(${test} PROPERTIES
    FIXTURES_REQUIRED "${test}_dir"
    WORKING_DIRECTORY "${run_dir}")
endforeach()

# The process-level tests need the proof harnesses and the CLI. They receive
# those paths explicitly so that no test has to guess a build layout.
if(TARGET sc_crash_child)
  target_compile_definitions(multiprocess_test
    PRIVATE SC_CRASH_CHILD="$<TARGET_FILE:sc_crash_child>")
endif()
if(TARGET sc_fence_child)
  target_compile_definitions(multiprocess_test
    PRIVATE SC_FENCE_CHILD="$<TARGET_FILE:sc_fence_child>")
endif()
if(TARGET spacecap)
  target_compile_definitions(multiprocess_test
    PRIVATE SC_CLI="$<TARGET_FILE:spacecap>")
endif()
