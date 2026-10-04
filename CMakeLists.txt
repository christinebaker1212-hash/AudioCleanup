cmake_minimum_required(VERSION 3.22)

project(AudioFinisher VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(AF_BUILD_GUI   "Build the JUCE desktop application" ON)
option(AF_BUILD_CLI   "Build the command-line renderer" ON)
option(AF_BUILD_TESTS "Build the DSP validation tests" ON)

if(MSVC)
  # Static CRT so the release .exe runs without a VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(io)

if(AF_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(AF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(AF_BUILD_GUI)
  add_subdirectory(app)
endif()
