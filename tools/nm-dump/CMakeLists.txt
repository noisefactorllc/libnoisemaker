add_executable(nm-dump main.cpp host_commands.cpp)
target_include_directories(nm-dump PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(nm-dump PRIVATE noisemaker_host)
set_target_properties(nm-dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(alias js-syntax registry midi-state audio-state obj-parser)
  add_custom_command(TARGET nm-dump POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nm-dump>
            $<TARGET_FILE_DIR:nm-dump>/nm-dump-${alias}${CMAKE_EXECUTABLE_SUFFIX})
endforeach()
