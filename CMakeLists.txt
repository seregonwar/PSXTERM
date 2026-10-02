cmake_minimum_required(VERSION 3.16)

project(psxterm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Platform selection
#
# Host builds use the native compiler. PS4/PS5 builds are configured through
# the payload SDK CMake toolchain files (orbis.cmake / prospero.cmake), which
# define ORBIS and PROSPERO respectively.
# ---------------------------------------------------------------------------
if(PROSPERO)
  set(PSXTERM_PLATFORM ps5)
elseif(ORBIS)
  set(PSXTERM_PLATFORM ps4)
else()
  set(PSXTERM_PLATFORM host)
endif()

message(STATUS "PSXTerm platform: ${PSXTERM_PLATFORM}")

# Warnings apply to PSXTerm code only, never to the SDK sysroot headers.
add_compile_options(-Wall -Wextra -Wpedantic)
if(PSXTERM_PLATFORM STREQUAL "host")
  add_compile_options(-Werror)
  # POSIX.1-2008 interfaces on the development host (Linux/macOS).
  add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

# ---------------------------------------------------------------------------
# Core library: protocol, server, sessions, tty abstraction, shell, platform
# ---------------------------------------------------------------------------
set(PSXTERM_CORE_SOURCES
  core/log.c
  core/util.c
  core/protocol.c
  platform/${PSXTERM_PLATFORM}/platform.c
)

add_library(psxterm_core STATIC ${PSXTERM_CORE_SOURCES})
target_include_directories(psxterm_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(PSXTERM_PLATFORM STREQUAL "host")
  find_package(Threads REQUIRED)
  target_link_libraries(psxterm_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Host-side unit tests (never built for PS4/PS5 targets)
# ---------------------------------------------------------------------------
if(PSXTERM_PLATFORM STREQUAL "host" AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()

  set(PSXTERM_TESTS
    test_protocol
  )

  foreach(test_name ${PSXTERM_TESTS})
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE psxterm_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
