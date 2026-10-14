cmake_minimum_required(VERSION 3.20)
project(BtA2dpSrc VERSION 0.1.0 LANGUAGES C CXX)

# User-mode stack of the independent Windows 11 A2DP source (LDAC + SBC).
# Kernel drivers live in src/driver and are built with MSBuild + WDK (see docs/BUILD.md).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT WIN32)
  message(FATAL_ERROR "BtA2dpSrc targets Windows 11 only")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc>")

option(BTA_BUILD_TESTS "Build unit and integration tests" ON)

add_subdirectory(third_party)
add_subdirectory(src)

if(BTA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
