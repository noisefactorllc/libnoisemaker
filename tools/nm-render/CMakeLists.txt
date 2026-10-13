add_executable(nm-render main.cpp png.cpp)
target_include_directories(nm-render PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(nm-render PRIVATE noisemaker_dawn)
set_target_properties(nm-render PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
