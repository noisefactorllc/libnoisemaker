add_executable(test_device gpu/test_device.cpp)
target_include_directories(test_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_device PRIVATE noisemaker_gpu)
add_test(NAME test_device COMMAND test_device)

add_executable(test_context capi/test_context.c)
target_link_libraries(test_context PRIVATE noisemaker)
add_test(NAME test_context COMMAND test_context)

add_library(noisemaker_core_internal INTERFACE)
target_include_directories(noisemaker_core_internal INTERFACE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noisemaker_core_internal INTERFACE noisemaker_core)

foreach(name test_value test_json test_js_number)
  add_executable(${name} core/${name}.cpp)
  target_link_libraries(${name} PRIVATE noisemaker_core_internal)
  add_test(NAME ${name} COMMAND ${name})
endforeach()
