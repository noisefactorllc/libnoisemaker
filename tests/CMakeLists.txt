if(NM_BUILD_GPU)
  add_executable(test_device gpu/test_device.cpp)
  target_include_directories(test_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_device PRIVATE noisemaker_gpu)
  add_test(NAME test_device COMMAND test_device)

  # nm-render's PNG codec, built with nm-render.
  add_executable(test_png gpu/test_png.cpp ${PROJECT_SOURCE_DIR}/tools/nm-render/png.cpp)
  target_include_directories(test_png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tools/nm-render)
  add_test(NAME test_png COMMAND test_png)

  # nm-render writes the frame as the reference page presents it.
  add_executable(png_layout gpu/png_layout.cpp ${PROJECT_SOURCE_DIR}/tools/nm-render/png.cpp)
  target_include_directories(png_layout PRIVATE ${PROJECT_SOURCE_DIR}/tools/nm-render)
  add_test(NAME nm_render_orientation COMMAND "${CMAKE_COMMAND}" "-DNM_RENDER=$<TARGET_FILE:nm-render>"
    "-DPNG_LAYOUT=$<TARGET_FILE:png_layout>" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nm_render_orientation"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/nm_render_orientation.cmake")

  # A minted graph renders as its DSL does.
  add_test(NAME nm_render_graph COMMAND "${CMAKE_COMMAND}" "-DNM_RENDER=$<TARGET_FILE:nm-render>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nm_render_graph"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/nm_render_graph.cmake")

  # nm-render with valid and malformed Portable sidecars.
  add_test(NAME nm_render_portable COMMAND "${CMAKE_COMMAND}" "-DNM_RENDER=$<TARGET_FILE:nm-render>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nm_render_portable"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/nm_render_portable.cmake")

  add_executable(test_context capi/test_context.c)
  target_link_libraries(test_context PRIVATE noisemaker)
  add_test(NAME test_context COMMAND test_context)
endif()

add_executable(test_compile capi/test_compile.c)
target_link_libraries(test_compile PRIVATE noisemaker_core)
add_test(NAME test_compile COMMAND test_compile)

add_executable(test_unparse capi/test_unparse.c)
target_link_libraries(test_unparse PRIVATE noisemaker_core)
add_test(NAME test_unparse COMMAND test_unparse)

# The C++ API test sees only the installed headers and the exported symbols.
add_executable(test_facade cxx/test_facade.cpp)
target_link_libraries(test_facade PRIVATE noisemaker_core)
add_test(NAME test_facade COMMAND test_facade)
set_tests_properties(test_facade PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_library(noisemaker_core_internal INTERFACE)
target_include_directories(noisemaker_core_internal INTERFACE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noisemaker_core_internal INTERFACE noisemaker_core_impl)

foreach(name test_value test_json test_js_number test_js_syntax test_catalog test_error_formatter test_lexer test_parser test_registry test_validator test_effect_validator test_expander test_resources test_program_state)
  add_executable(${name} core/${name}.cpp)
  target_link_libraries(${name} PRIVATE noisemaker_core_internal)
  target_include_directories(${name} PRIVATE ${PROJECT_BINARY_DIR}/generated)
  add_test(NAME ${name} COMMAND ${name})
endforeach()
set_tests_properties(test_effect_validator PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# Each license notice in the embedded catalog has an entry in the notices file
# that the install ships beside LICENSE.
add_test(NAME catalog_notices COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/catalog/check_notices.py"
  "${PROJECT_BINARY_DIR}/catalog" "${PROJECT_SOURCE_DIR}/src/core/catalog/catalog-NOTICES.txt")

# The tracking statement in the README names the Noisemaker release that
# parity/reference.json locks, and no stale release number.
add_test(NAME readme_lock COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check-readme-lock.py"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_executable(test_jsre wgsl/test_jsre.cpp)
target_include_directories(test_jsre PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_jsre PRIVATE noisemaker_backend)
add_test(NAME test_jsre COMMAND test_jsre)

foreach(name test_async_overlays test_midi_state test_obj_parser test_text_canvas)
  add_executable(${name} host/${name}.cpp)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE noisemaker_host)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# Install this build and use it from a separate project, as qt and the kit will.
string(REPLACE ";" "," _nm_consumer_archs "${CMAKE_OSX_ARCHITECTURES}")
add_test(NAME consumer COMMAND "${CMAKE_COMMAND}"
  "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer"
  "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer" "-DCONFIG=$<CONFIG>" "-DGENERATOR=${CMAKE_GENERATOR}"
  "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}" "-DC_COMPILER=${CMAKE_C_COMPILER}"
  "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DOSX_ARCHITECTURES=${_nm_consumer_archs}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/consumer/run.cmake")
