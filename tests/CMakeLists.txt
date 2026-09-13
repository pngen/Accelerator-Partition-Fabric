# Test binaries. Test commands run plainly: no timeouts, no forced
# termination. A hanging test is a defect to be diagnosed, not hidden.

add_library(apf_test_support STATIC
  support/test_main.cpp
  support/fabric_fixture.cpp)
target_link_libraries(apf_test_support PUBLIC apf apf_project_warnings)
target_include_directories(apf_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

set(APF_TEST_SOURCES
  unit_core.cpp
  unit_runtime.cpp
  property_fabric.cpp
  concurrency_fabric.cpp
  adversarial.cpp)

add_executable(apf_tests ${APF_TEST_SOURCES})
target_link_libraries(apf_tests PRIVATE apf_test_support)
set_target_properties(apf_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

add_test(NAME apf_unit COMMAND apf_tests)

add_executable(apf_mp_tests multiprocess.cpp)
target_link_libraries(apf_mp_tests PRIVATE apf_test_support)
target_compile_definitions(apf_mp_tests PRIVATE
  APF_COORDINATOR_BINARY="$<TARGET_FILE:apfcoord>"
  APF_WORKER_BINARY="$<TARGET_FILE:apfworker>")
set_target_properties(apf_mp_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

add_test(NAME apf_multiprocess COMMAND apf_mp_tests)
set_tests_properties(apf_multiprocess PROPERTIES DEPENDS apf_unit)
