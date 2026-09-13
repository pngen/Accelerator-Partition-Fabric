cmake_minimum_required(VERSION 3.20)

# An independent consumer of the installed Accelerator Partition Fabric package.
# It links only what the package exports; it does not see the runtime source
# tree, its build tree, or its private headers.

project(ApfDownstreamConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AcceleratorPartitionFabric CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SummonSoftwareLabs::AcceleratorPartitionFabric)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Werror)
endif()
