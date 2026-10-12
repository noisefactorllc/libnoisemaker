# Dawn's build options for libnoisemaker. cmake/Dawn.cmake includes this file,
# and tools/build_dawn.py passes it to Dawn's own configure with -C, so a
# prebuilt Dawn and an in-tree Dawn are built the same way.

set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_NODE_BINDINGS OFF CACHE BOOL "" FORCE)
# Dawn's optional C++ module target cannot be generated with the Linux CI
# compiler/CMake combination. libnoisemaker only uses the C WebGPU headers.
set(DAWN_SUPPORTS_CXX_MODULES OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WAYLAND OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_SWIFTSHADER OFF CACHE BOOL "" FORCE)
set(DAWN_USE_BUILT_DXC OFF CACHE BOOL "" FORCE)
# tools/build_dawn.py reads this file with -C, before project(); test the host too.
if(WIN32 OR CMAKE_HOST_WIN32)
  # Dawn copies an x64 SDK compiler DLL to the build root, while test and
  # consumer executables may live elsewhere or target ARM64. Load the
  # architecture-matched Windows system component instead.
  set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD ON CACHE BOOL "" FORCE)
endif()
set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_IR_BINARY OFF CACHE BOOL "" FORCE)
