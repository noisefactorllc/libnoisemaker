add_executable(test_device gpu/test_device.cpp)
target_include_directories(test_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_device PRIVATE noisemaker_gpu)
add_test(NAME test_device COMMAND test_device)

add_executable(test_context capi/test_context.c)
target_link_libraries(test_context PRIVATE noisemaker)
add_test(NAME test_context COMMAND test_context)
