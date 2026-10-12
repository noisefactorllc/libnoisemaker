# A project that uses an installed libnoisemaker the way qt and the export kit
# will: find_package, then link noisemaker::core. It runs no Node or Python.
cmake_minimum_required(VERSION 3.22)
project(noisemaker_consumer C CXX)

find_package(noisemaker CONFIG REQUIRED COMPONENTS core)

add_executable(consumer_c consumer.c)
target_link_libraries(consumer_c PRIVATE noisemaker::core)

add_executable(consumer_cxx20 consumer.cpp)
target_link_libraries(consumer_cxx20 PRIVATE noisemaker::core)
set_target_properties(consumer_cxx20 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(consumer_cxx17 consumer.cpp)
target_link_libraries(consumer_cxx17 PRIVATE noisemaker::core)
set_target_properties(consumer_cxx17 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
