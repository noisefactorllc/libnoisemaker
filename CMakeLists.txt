cmake_minimum_required(VERSION 3.22)
project(libnoisemaker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NM_BUILD_TESTS "Build the libnoisemaker tests" ${PROJECT_IS_TOP_LEVEL})
set(NM_DAWN_SOURCE_DIR "${PROJECT_SOURCE_DIR}/.cache/dawn/src" CACHE PATH
    "Dawn source tree prepared by tools/fetch_dawn.py")

include(cmake/Dawn.cmake)

add_library(noisemaker_gpu STATIC src/gpu/device.cpp)
target_include_directories(noisemaker_gpu PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker_gpu PUBLIC dawn::webgpu_dawn)

add_library(noisemaker_core STATIC
  src/capi/error.cpp
  src/core/value/js_string.cpp
  src/core/value/js_number.cpp
  src/core/value/value.cpp
  src/core/value/json.cpp)
target_include_directories(noisemaker_core
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_core PRIVATE -ffp-contract=off)
endif()
add_library(noisemaker::core ALIAS noisemaker_core)

add_library(noisemaker STATIC src/capi/context.cpp)
target_include_directories(noisemaker PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker PUBLIC noisemaker_core PRIVATE noisemaker_gpu)
add_library(noisemaker::noisemaker ALIAS noisemaker)

if(NM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
