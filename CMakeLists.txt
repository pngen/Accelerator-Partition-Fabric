cmake_minimum_required(VERSION 3.20)

project(AcceleratorPartitionFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for governing accelerator partition lifecycle, capacity, placement, admission, isolation, fragmentation, reconfiguration and generation-bound authority"
  HOMEPAGE_URL "https://github.com/pngen/Accelerator-Partition-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(APF_BUILD_TESTS       "Build the Accelerator Partition Fabric test suite"     ON)
option(APF_BUILD_EXAMPLES    "Build the Accelerator Partition Fabric examples"       ON)
option(APF_BUILD_TOOLS       "Build apfctl/apfcoord/apfworker command line tools"    ON)
option(APF_BUILD_BENCHMARKS  "Build the benchmark driver"                            ON)
option(APF_ENABLE_WERROR     "Treat first-party compiler warnings as errors"         ON)
option(APF_ENABLE_ASAN       "Build with AddressSanitizer instrumentation"           OFF)
option(APF_BUILD_SHARED      "Build the runtime as a shared library"                 OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APF_BUILD_SHARED)
  set(APF_LIBRARY_TYPE SHARED)
else()
  set(APF_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Dependency discovery (deliberately dependency-free core)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(APF_HAVE_NVML_DYNAMIC TRUE)

# ---------------------------------------------------------------------------
# Runtime library
# ---------------------------------------------------------------------------
set(APF_PUBLIC_HEADERS
  include/apf/version.hpp
  include/apf/result.hpp
  include/apf/id.hpp
  include/apf/time.hpp
  include/apf/limits.hpp
  include/apf/resource.hpp
  include/apf/isolation.hpp
  include/apf/evidence.hpp
  include/apf/lifecycle.hpp
  include/apf/profile.hpp
  include/apf/capability.hpp
  include/apf/accelerator.hpp
  include/apf/partition.hpp
  include/apf/policy.hpp
  include/apf/request.hpp
  include/apf/explain.hpp
  include/apf/accounting.hpp
  include/apf/fragmentation.hpp
  include/apf/plan.hpp
  include/apf/client.hpp
  include/apf/reservation.hpp
  include/apf/reconciliation.hpp
  include/apf/snapshot.hpp
  include/apf/backend.hpp
  include/apf/backend_synthetic.hpp
  include/apf/backend_nvml.hpp
  include/apf/persistence.hpp
  include/apf/codec.hpp
  include/apf/fabric.hpp
  include/apf/transport.hpp
  include/apf/protocol.hpp
  include/apf/coordinator.hpp
  include/apf/worker.hpp
  include/apf/process.hpp
)

set(APF_SOURCES
  src/version.cpp
  src/result.cpp
  src/limits.cpp
  src/id.cpp
  src/time.cpp
  src/resource.cpp
  src/isolation.cpp
  src/evidence.cpp
  src/lifecycle.cpp
  src/profile.cpp
  src/capability.cpp
  src/accelerator.cpp
  src/partition.cpp
  src/policy.cpp
  src/request.cpp
  src/explain.cpp
  src/accounting.cpp
  src/fragmentation.cpp
  src/plan.cpp
  src/client.cpp
  src/reservation.cpp
  src/reconciliation.cpp
  src/snapshot.cpp
  src/backend.cpp
  src/backend_synthetic.cpp
  src/backend_nvml.cpp
  src/codec.cpp
  src/persistence.cpp
  src/planner.cpp
  src/admission.cpp
  src/reconfiguration.cpp
  src/fabric.cpp
  src/transport.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/worker.cpp
  src/process.cpp
)

add_library(apf ${APF_LIBRARY_TYPE} ${APF_SOURCES} ${APF_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::AcceleratorPartitionFabric ALIAS apf)

target_include_directories(apf
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(apf PUBLIC Threads::Threads)
if(WIN32)
  # The framed transport uses the Winsock API directly and the runtime is a
  # static library, so consumers must link the same system library.
  target_link_libraries(apf PUBLIC ws2_32)
endif()
target_compile_features(apf PUBLIC cxx_std_20)

set_target_properties(apf PROPERTIES
  # The installed target is SummonSoftwareLabs::AcceleratorPartitionFabric: the
  # in-tree target name and the exported name must not drift apart.
  EXPORT_NAME AcceleratorPartitionFabric
  OUTPUT_NAME AcceleratorPartitionFabric
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# Warnings: strict, cause-fixing, never globally suppressed
# ---------------------------------------------------------------------------
add_library(apf_project_warnings INTERFACE)
if(MSVC)
  target_compile_options(apf_project_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
  if(APF_ENABLE_WERROR)
    target_compile_options(apf_project_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(apf_project_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00)
else()
  target_compile_options(apf_project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
  if(APF_ENABLE_WERROR)
    target_compile_options(apf_project_warnings INTERFACE -Werror)
  endif()
endif()

if(APF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(apf_project_warnings INTERFACE /fsanitize=address /Oy-)
    target_link_options(apf_project_warnings INTERFACE /INCREMENTAL:NO)
    message(STATUS "Accelerator Partition Fabric: MSVC AddressSanitizer enabled")
  else()
    target_compile_options(apf_project_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(apf_project_warnings INTERFACE -fsanitize=address)
    message(STATUS "Accelerator Partition Fabric: AddressSanitizer enabled")
  endif()
endif()

# The warning configuration is a build-time concern only: it must not appear in
# the installed export set and must never impose our flags on a consumer.
target_link_libraries(apf PRIVATE $<BUILD_INTERFACE:apf_project_warnings>)

# ---------------------------------------------------------------------------
# Installation / packaging
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS apf
  EXPORT AcceleratorPartitionFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/apf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT AcceleratorPartitionFabricTargets
  FILE AcceleratorPartitionFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AcceleratorPartitionFabric)

configure_package_config_file(
  cmake/AcceleratorPartitionFabricConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/AcceleratorPartitionFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AcceleratorPartitionFabric)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/AcceleratorPartitionFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/AcceleratorPartitionFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/AcceleratorPartitionFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AcceleratorPartitionFabric)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/AcceleratorPartitionFabric)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(APF_BUILD_TOOLS)
  add_executable(apfctl tools/apfctl_main.cpp)
  add_executable(apfcoord tools/apfcoord_main.cpp)
  add_executable(apfworker tools/apfworker_main.cpp)
  foreach(t apfctl apfcoord apfworker)
    target_link_libraries(${t} PRIVATE apf apf_project_warnings)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    install(TARGETS ${t} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endforeach()
  target_include_directories(apfctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(APF_BUILD_EXAMPLES)
  set(APF_EXAMPLES
    ex_plan_basic
    ex_reserve_commit
    ex_fragmentation_rejection
    ex_drain_reconfigure
    ex_synthetic_heterogeneous
    ex_reconcile_after_change)
  foreach(ex ${APF_EXAMPLES})
    add_executable(${ex} examples/${ex}.cpp)
    target_link_libraries(${ex} PRIVATE apf apf_project_warnings)
    set_target_properties(${ex} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
  endforeach()
  add_executable(ex_installed_consumer examples/ex_installed_consumer.cpp)
  target_link_libraries(ex_installed_consumer PRIVATE apf apf_project_warnings)
  set_target_properties(ex_installed_consumer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(APF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(APF_BUILD_BENCHMARKS)
  add_executable(apf_bench bench/apf_bench.cpp)
  target_link_libraries(apf_bench PRIVATE apf apf_project_warnings)
  set_target_properties(apf_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
endif()

message(STATUS "Accelerator Partition Fabric ${PROJECT_VERSION}: type=${APF_LIBRARY_TYPE} asan=${APF_ENABLE_ASAN} werror=${APF_ENABLE_WERROR} tests=${APF_BUILD_TESTS}")