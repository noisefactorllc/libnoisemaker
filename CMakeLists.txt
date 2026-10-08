cmake_minimum_required(VERSION 3.22)
project(libnoisemaker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NM_BUILD_TESTS "Build the libnoisemaker tests" ${PROJECT_IS_TOP_LEVEL})
set(NM_DAWN_SOURCE_DIR "${PROJECT_SOURCE_DIR}/.cache/dawn/src" CACHE PATH
    "Dawn source tree prepared by tools/fetch_dawn.py")
set(NM_CATALOG_CACHE_DIR "" CACHE PATH "Prefilled, verified catalog cache for offline builds")
find_program(NM_NODE_EXECUTABLE node REQUIRED)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)

if(NM_CATALOG_CACHE_DIR)
  set(_nm_catalog_cache "${NM_CATALOG_CACHE_DIR}")
  set(_nm_catalog_offline --offline)
else()
  set(_nm_catalog_cache "${PROJECT_SOURCE_DIR}/.cache/catalog")
  set(_nm_catalog_offline)
endif()
set(_nm_catalog_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/catalog_data.cpp")
set(_nm_catalog_header "${CMAKE_CURRENT_BINARY_DIR}/generated/catalog_version.h")
add_custom_command(
  OUTPUT "${_nm_catalog_cpp}" "${_nm_catalog_header}"
  COMMAND "${NM_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/fetch-catalog.mjs"
          --cache "${_nm_catalog_cache}" ${_nm_catalog_offline}
  COMMAND "${NM_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/build-catalog.mjs"
          --cache "${_nm_catalog_cache}" --out "${CMAKE_CURRENT_BINARY_DIR}/catalog"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/embed.py"
          "${CMAKE_CURRENT_BINARY_DIR}/catalog" "${_nm_catalog_cpp}" "${_nm_catalog_header}"
  DEPENDS "${PROJECT_SOURCE_DIR}/parity/reference.json"
          "${PROJECT_SOURCE_DIR}/tools/catalog/fetch-catalog.mjs"
          "${PROJECT_SOURCE_DIR}/tools/catalog/build-catalog.mjs"
          "${PROJECT_SOURCE_DIR}/tools/catalog/embed.py"
  VERBATIM)
add_custom_target(noisemaker_catalog_generated DEPENDS "${_nm_catalog_cpp}" "${_nm_catalog_header}")

include(cmake/Dawn.cmake)

add_library(noisemaker_gpu STATIC src/gpu/device.cpp)
target_include_directories(noisemaker_gpu PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker_gpu PUBLIC dawn::webgpu_dawn)

add_library(noisemaker_core STATIC
  src/capi/error.cpp
  src/core/catalog/catalog.cpp
  ${_nm_catalog_cpp}
  src/core/js/js_unicode.cpp
  src/core/js/js_regexp.cpp
  src/core/js/js_syntax.cpp
  src/core/value/js_string.cpp
  src/core/value/js_number.cpp
  src/core/value/value.cpp
  src/core/value/json.cpp)
target_include_directories(noisemaker_core
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(noisemaker_core noisemaker_catalog_generated)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_core PRIVATE -ffp-contract=off)
endif()
add_library(noisemaker::core ALIAS noisemaker_core)

add_library(noisemaker_host STATIC
  src/host/worm_tracer.cpp
  src/host/stroke_canvas.cpp
  src/host/stroke_fill.cpp
  src/host/audio_analyzer.cpp
  src/host/audio_state.cpp
  src/host/midi_state.cpp
  src/host/obj_parser.cpp)
target_include_directories(noisemaker_host PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker_host PUBLIC noisemaker_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_host PRIVATE -ffp-contract=off)
endif()
add_library(noisemaker::host ALIAS noisemaker_host)
add_subdirectory(tools/nm-dump)

add_library(noisemaker STATIC src/capi/context.cpp)
target_include_directories(noisemaker PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker PUBLIC noisemaker_core PRIVATE noisemaker_gpu)
add_library(noisemaker::noisemaker ALIAS noisemaker)

if(NM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
