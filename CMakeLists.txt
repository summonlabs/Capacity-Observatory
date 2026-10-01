cmake_minimum_required(VERSION 3.25)

project(CapacityObservatory
  VERSION 1.0.0
  DESCRIPTION "Facility capacity observability runtime for Summon Software Labs Data Center Control Plane deployments"
  HOMEPAGE_URL "https://github.com/summonlabs/Capacity-Observatory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CO_IS_TOP_LEVEL ON)
else()
  set(CO_IS_TOP_LEVEL OFF)
endif()

option(CO_BUILD_TESTS "Build the Capacity Observatory test suites" ${CO_IS_TOP_LEVEL})
option(CO_BUILD_BENCHMARKS "Build the Capacity Observatory benchmarks" ${CO_IS_TOP_LEVEL})
option(CO_BUILD_CLI "Build the capacity-observatory command line tool" ON)
option(CO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(CO_ENABLE_ASAN "Build with AddressSanitizer (MSVC /fsanitize=address)" OFF)
option(CO_ENABLE_ANALYZE "Run MSVC static analysis (/analyze) at build time" OFF)
option(CO_ENABLE_LOCK_AUDIT "Enable the runtime lock-order auditor in the concurrency core" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Capacity Observatory core library
# ---------------------------------------------------------------------------
add_library(capacity_observatory
  src/concurrency.cpp
  src/core.cpp
  src/dimension.cpp
  src/evidence.cpp
  src/fragmentation.cpp
  src/hash.cpp
  src/ingest.cpp
  src/json.cpp
  src/ledger.cpp
  src/platform.cpp
  src/snapshot.cpp
  src/store_file.cpp
  src/topology.cpp
  src/watermark.cpp
)
add_library(CapacityObservatory::capacity_observatory ALIAS capacity_observatory)

target_include_directories(capacity_observatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(capacity_observatory PUBLIC cxx_std_20)

set_target_properties(capacity_observatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  DEFINE_SYMBOL capacity_observatory_EXPORTS
)

if(CO_ENABLE_LOCK_AUDIT)
  target_compile_definitions(capacity_observatory PUBLIC CO_ENABLE_LOCK_AUDIT=1)
endif()

target_compile_definitions(capacity_observatory
  PUBLIC
    CO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  PRIVATE
    CO_BUILDING_LIBRARY=1
)

if(MSVC)
  # __cplusplus reports the real standard only with /Zc:__cplusplus.
  target_compile_options(capacity_observatory PRIVATE /Zc:__cplusplus /permissive- /utf-8 /EHsc /W4)
  if(CO_WARNINGS_AS_ERRORS)
    target_compile_options(capacity_observatory PRIVATE /WX)
  endif()
  target_compile_definitions(capacity_observatory PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(CO_ENABLE_ASAN)
    target_compile_options(capacity_observatory PUBLIC /fsanitize=address /Zi)
    target_link_options(capacity_observatory PUBLIC /fsanitize=address)
  endif()
  if(CO_ENABLE_ANALYZE)
    target_compile_options(capacity_observatory PRIVATE /analyze)
  endif()
else()
  target_compile_options(capacity_observatory PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast)
  if(CO_WARNINGS_AS_ERRORS)
    target_compile_options(capacity_observatory PRIVATE -Werror)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(capacity_observatory PRIVATE Threads::Threads)
target_link_libraries(capacity_observatory PUBLIC $<$<PLATFORM_ID:Windows>:>)

# ---------------------------------------------------------------------------
# Command line interface
# ---------------------------------------------------------------------------
if(CO_BUILD_CLI)
  add_executable(capacity-observatory src/main.cpp)
  target_link_libraries(capacity-observatory PRIVATE CapacityObservatory::capacity_observatory)
  if(MSVC)
    target_compile_options(capacity-observatory PRIVATE /Zc:__cplusplus /permissive- /utf-8 /EHsc /W4)
    if(CO_WARNINGS_AS_ERRORS)
      target_compile_options(capacity-observatory PRIVATE /WX)
    endif()
    target_compile_definitions(capacity-observatory PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(CO_ENABLE_ASAN)
      target_compile_options(capacity-observatory PRIVATE /fsanitize=address /Zi)
      target_link_options(capacity-observatory PRIVATE /fsanitize=address)
    endif()
  else()
    target_compile_options(capacity-observatory PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast)
    if(CO_WARNINGS_AS_ERRORS)
      target_compile_options(capacity-observatory PRIVATE -Werror)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS capacity_observatory
  EXPORT CapacityObservatoryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CO_BUILD_CLI)
  install(TARGETS capacity-observatory RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CapacityObservatoryTargets
  FILE CapacityObservatoryTargets.cmake
  NAMESPACE CapacityObservatory::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityObservatory
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CapacityObservatoryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CapacityObservatoryConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityObservatory
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CapacityObservatoryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/CapacityObservatoryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/CapacityObservatoryConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CapacityObservatory
)

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/CapacityObservatory)

# ---------------------------------------------------------------------------
# Tests / benchmarks
# ---------------------------------------------------------------------------
if(CO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CO_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
