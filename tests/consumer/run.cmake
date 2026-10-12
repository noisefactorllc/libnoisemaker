# ctest driver: install this build to a fresh prefix, then configure, build
# and run tests/consumer against the installed package alone.
#   cmake -DBUILD_DIR=... -DWORK_DIR=... -DSOURCE_DIR=... -DCONFIG=... -DGENERATOR=...
#         [-DPLATFORM=...] [-DC_COMPILER=...] [-DCXX_COMPILER=...] [-DOSX_ARCHITECTURES=...]
#         -P run.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
set(prefix "${WORK_DIR}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${prefix}"
  RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "install failed: ${result}")
endif()
set(args -S "${SOURCE_DIR}" -B "${WORK_DIR}/build" -G "${GENERATOR}" "-DCMAKE_PREFIX_PATH=${prefix}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}")
if(PLATFORM)
  list(APPEND args -A "${PLATFORM}")
endif()
foreach(var C_COMPILER CXX_COMPILER OSX_ARCHITECTURES)
  if(${var})
    list(APPEND args "-DCMAKE_${var}=${${var}}")
  endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" ${args} RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "consumer configure failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/build" --config "${CONFIG}" RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "consumer build failed: ${result}")
endif()
# Windows loads the DLL from the install's bin directory.
set(ENV{PATH} "${prefix}/bin;$ENV{PATH}")
foreach(exe consumer_c consumer_cxx20 consumer_cxx17)
  file(GLOB_RECURSE found "${WORK_DIR}/build/${exe}" "${WORK_DIR}/build/${exe}.exe")
  list(FILTER found EXCLUDE REGEX "CMakeFiles")
  if(NOT found)
    message(FATAL_ERROR "${exe} was not built")
  endif()
  list(GET found 0 path)
  execute_process(COMMAND "${path}" RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "${exe} failed: ${result}")
  endif()
endforeach()
