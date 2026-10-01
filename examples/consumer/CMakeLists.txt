cmake_minimum_required(VERSION 3.25)

project(CapacityObservatoryConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent out-of-tree consumer of the installed Capacity Observatory package"
  LANGUAGES CXX)

# This project is deliberately NOT part of the Capacity Observatory build tree.
# It exists to prove that an installed package is consumable by find_package()
# from an unrelated project with its own build directory.

find_package(CapacityObservatory 1.0 REQUIRED CONFIG)

add_executable(capacity-consumer main.cpp)
target_link_libraries(capacity-consumer PRIVATE CapacityObservatory::capacity_observatory)
target_compile_features(capacity-consumer PRIVATE cxx_std_20)
set_target_properties(capacity-consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(capacity-consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_compile_definitions(capacity-consumer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(capacity-consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "CapacityObservatory_DIR = ${CapacityObservatory_DIR}")
message(STATUS "CapacityObservatory_VERSION = ${CapacityObservatory_VERSION}")
