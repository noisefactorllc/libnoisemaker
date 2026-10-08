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

if(NM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
