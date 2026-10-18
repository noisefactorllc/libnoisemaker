cmake_minimum_required(VERSION 3.22)
project(libnoisemaker VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NM_BUILD_TESTS "Build the libnoisemaker tests" ${PROJECT_IS_TOP_LEVEL})
option(NM_BUILD_GPU "Build the Dawn executor. OFF builds only the GPU-free core and host helpers, with no Dawn tree" ON)
option(NM_BUILD_SHARED "Build noisemaker_core as a shared library exporting only its C and C++ APIs" OFF)
option(NM_WARNINGS_AS_ERRORS "Build libnoisemaker's own targets with -Wall -Wextra -Wpedantic -Werror, or /W4 /WX" OFF)
if(WIN32 AND NM_BUILD_SHARED AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  # Executables load noisemaker_core.dll from beside them.
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
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
  COMMAND "${NM_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/extract-runtime-shaders.mjs"
          --cache "${_nm_catalog_cache}" --out "${CMAKE_CURRENT_BINARY_DIR}/catalog"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/embed.py"
          "${CMAKE_CURRENT_BINARY_DIR}/catalog" "${_nm_catalog_cpp}" "${_nm_catalog_header}"
  DEPENDS "${PROJECT_SOURCE_DIR}/parity/reference.json"
          "${PROJECT_SOURCE_DIR}/tools/catalog/fetch-catalog.mjs"
          "${PROJECT_SOURCE_DIR}/tools/catalog/build-catalog.mjs"
          "${PROJECT_SOURCE_DIR}/tools/catalog/extract-runtime-shaders.mjs"
          "${PROJECT_SOURCE_DIR}/tools/catalog/embed.py"
  VERBATIM)
add_custom_target(noisemaker_catalog_generated DEPENDS "${_nm_catalog_cpp}" "${_nm_catalog_header}")

if(NM_BUILD_GPU)
  include(cmake/Dawn.cmake)
endif()

# After Dawn's subdirectory, so these reach only the targets defined below and
# the tool and test subdirectories added after this point.
if(NM_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/W4 /WX)
  else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

if(NM_BUILD_GPU)
  add_library(noisemaker_gpu STATIC src/gpu/device.cpp)
  target_include_directories(noisemaker_gpu PUBLIC ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(noisemaker_gpu PUBLIC dawn::webgpu_dawn)
endif()

# The core compiles once. noisemaker_core is a static library, or with
# NM_BUILD_SHARED a shared library that exports only the C API and the C++ API
# in include/noisemaker. noisemaker_core_impl links the same objects with their
# internal symbols, for the tools, the host helpers and the internal tests.
add_library(noisemaker_core_objects OBJECT
  src/capi/compile.cpp
  src/capi/error.cpp
  src/capi/facade.cpp
  src/core/catalog/catalog.cpp
  ${_nm_catalog_cpp}
  src/core/edit/error_formatter.cpp
  src/core/edit/jsv.cpp
  src/core/edit/transform.cpp
  src/core/edit/unparser.cpp
  src/core/edit/program_state/console.cpp
  src/core/edit/program_state/dsl_utils.cpp
  src/core/edit/program_state/emitter.cpp
  src/core/edit/program_state/host.cpp
  src/core/edit/program_state/js_map.cpp
  src/core/edit/program_state/palette.cpp
  src/core/edit/program_state/program_state.cpp
  src/core/graph/dsl_compiler.cpp
  src/core/graph/expander.cpp
  src/core/graph/graph.cpp
  src/core/graph/normalize.cpp
  src/core/graph/resources.cpp
  src/core/js/js_unicode.cpp
  src/core/js/js_regexp.cpp
  src/core/js/js_syntax.cpp
  src/core/lang/diagnostics.cpp
  src/core/lang/dim.cpp
  src/core/lang/enums.cpp
  src/core/lang/effect_registry.cpp
  src/core/lang/validator.cpp
  src/core/lang/effect_validator.cpp
  src/core/lang/lexer.cpp
  src/core/lang/parser.cpp
  src/core/value/js_string.cpp
  src/core/value/js_math.cpp
  src/core/value/js_number.cpp
  src/core/value/value.cpp
  src/core/value/json.cpp)
target_include_directories(noisemaker_core_objects
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(noisemaker_core_objects noisemaker_catalog_generated)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_core_objects PRIVATE -ffp-contract=off)
endif()
if(NM_BUILD_SHARED)
  target_compile_definitions(noisemaker_core_objects PRIVATE NM_SHARED NM_BUILDING)
  set_target_properties(noisemaker_core_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  add_library(noisemaker_core SHARED $<TARGET_OBJECTS:noisemaker_core_objects>)
  target_compile_definitions(noisemaker_core INTERFACE NM_SHARED)
  add_library(noisemaker_core_impl STATIC $<TARGET_OBJECTS:noisemaker_core_objects>)
else()
  add_library(noisemaker_core STATIC $<TARGET_OBJECTS:noisemaker_core_objects>)
  add_library(noisemaker_core_impl ALIAS noisemaker_core)
endif()
target_include_directories(noisemaker_core PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
if(NM_BUILD_SHARED)
  target_include_directories(noisemaker_core_impl PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
endif()
add_library(noisemaker::core ALIAS noisemaker_core)

add_library(noisemaker_host STATIC
  src/host/async_overlays.cpp
  src/host/worm_tracer.cpp
  src/host/stroke_canvas.cpp
  src/host/stroke_fill.cpp
  src/host/audio_analyzer.cpp
  src/host/audio_state.cpp
  src/host/midi_state.cpp
  src/host/obj_parser.cpp)
target_include_directories(noisemaker_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noisemaker_host PUBLIC noisemaker_core_impl)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_host PRIVATE -ffp-contract=off)
endif()
add_library(noisemaker::host ALIAS noisemaker_host)

# The reference WebGPU backend and pipeline over a device interface
# (src/gpu/backend/device.h) and the parts they use that need no GPU: WGSL
# source analysis and uniform packing. With RecordingDevice it builds and is gated in every configuration.
add_library(noisemaker_backend STATIC src/gpu/wgsl/jsre.cpp src/gpu/wgsl/wgsl.cpp src/gpu/backend/uniforms.cpp
            src/gpu/backend/automation.cpp src/gpu/backend/backend.cpp src/gpu/backend/pipeline.cpp
            src/gpu/backend/recording_device.cpp)
target_include_directories(noisemaker_backend PRIVATE ${PROJECT_SOURCE_DIR}/src)
# The host's MIDI and audio states, which automation reads; they bring core.
target_link_libraries(noisemaker_backend PUBLIC noisemaker_host)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(noisemaker_backend PRIVATE -ffp-contract=off)
endif()
add_subdirectory(tools/nm-dump)

if(NM_BUILD_GPU)
  # The backend's device on Dawn, and nm-render, which renders with it.
  add_library(noisemaker_dawn STATIC src/gpu/dawn_device.cpp)
  target_include_directories(noisemaker_dawn PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(noisemaker_dawn PUBLIC noisemaker_gpu noisemaker_backend)
  add_subdirectory(tools/nm-render)
endif()

if(NM_BUILD_GPU)
  add_library(noisemaker STATIC src/capi/context.cpp)
  target_include_directories(noisemaker PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(noisemaker PUBLIC noisemaker_core_impl PRIVATE noisemaker_gpu)
  add_library(noisemaker::noisemaker ALIAS noisemaker)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set_target_properties(noisemaker_core PROPERTIES EXPORT_NAME core)
target_include_directories(noisemaker_core INTERFACE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# The installed headers need C++17; the library itself builds as C++20.
target_compile_features(noisemaker_core INTERFACE cxx_std_17)
install(TARGETS noisemaker_core EXPORT noisemakerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/noisemaker/core.h include/noisemaker/core.hpp include/noisemaker/value.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/noisemaker)
install(FILES LICENSE src/core/js/acorn-LICENSE.txt src/core/catalog/catalog-NOTICES.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/noisemaker)
# ENGINE names the Noisemaker release the catalog and the parity gates come
# from; TOOLCHAIN names the build a C++ consumer must match (see README).
file(READ "${PROJECT_SOURCE_DIR}/parity/reference.json" _nm_reference)
string(JSON _nm_engine_version GET "${_nm_reference}" version)
string(JSON _nm_engine_commit GET "${_nm_reference}" commit)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ENGINE"
  CONTENT "Noisemaker ${_nm_engine_version} ${_nm_engine_commit}\n")
if(MSVC)
  if(CMAKE_MSVC_RUNTIME_LIBRARY)
    set(_nm_runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  else()
    set(_nm_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  set(_nm_runtime "MSVC ${MSVC_TOOLSET_VERSION} toolset, ${_nm_runtime} runtime")
elseif(APPLE)
  set(_nm_archs "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _nm_archs)
    set(_nm_archs "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(_nm_minimum "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _nm_minimum)
    set(_nm_minimum "the SDK's default")
  endif()
  set(_nm_runtime "libc++, minimum macOS ${_nm_minimum}, architectures ${_nm_archs}")
else()
  set(_nm_runtime "libstdc++ of ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
# One file per configuration: the MSVC runtime differs between them.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/TOOLCHAIN" CONTENT
"libnoisemaker ${PROJECT_VERSION}
compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}
system: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}
runtime: ${_nm_runtime}
C++ standard: library C++${CMAKE_CXX_STANDARD}, headers C++17
")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ENGINE" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/TOOLCHAIN"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/noisemaker)
install(EXPORT noisemakerTargets NAMESPACE noisemaker:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/noisemaker)
configure_package_config_file(cmake/noisemakerConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/noisemakerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/noisemaker)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/noisemakerConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/noisemakerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/noisemakerConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/noisemaker)

if(NM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
