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
# Core library: protocol, sessions, tty abstraction, shell, platform
# ---------------------------------------------------------------------------
set(PSXTERM_CORE_SOURCES
  core/log.c
  core/util.c
  core/protocol.c
  core/tty.c
  core/process.c
  core/session.c
  core/diag.c
  shell/shell.c
  shell/parser.c
  shell/env.c
  shell/command_registry.c
  shell/builtins/builtin_fs.c
  shell/builtins/builtin_env.c
  shell/builtins/builtin_sys.c
  shell/builtins/builtin_help.c
  shell/builtins/builtin_exit.c
  platform/${PSXTERM_PLATFORM}/platform.c
  platform/${PSXTERM_PLATFORM}/filesystem.c
  platform/${PSXTERM_PLATFORM}/tty.c
  platform/${PSXTERM_PLATFORM}/process.c
)

add_library(psxterm_core STATIC ${PSXTERM_CORE_SOURCES})
target_include_directories(psxterm_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(PSXTERM_PLATFORM STREQUAL "host")
  find_package(Threads REQUIRED)
  target_link_libraries(psxterm_core PUBLIC Threads::Threads)
endif()

# psxtermd: the daemon (also the payload entry point on PS4/PS5)
add_executable(psxtermd core/server.c)
target_link_libraries(psxtermd PRIVATE psxterm_core)

# psxterm-ttyprobe: runtime TTY capability probe
add_executable(psxterm-ttyprobe tools/ttyprobe.c)
target_link_libraries(psxterm-ttyprobe PRIVATE psxterm_core)

# cli_test: controlled external-execution target (host integration tests and
# PS4/PS5 payload ELF).
add_executable(cli_test tools/cli_test.c)
if(NOT PSXTERM_PLATFORM STREQUAL "host")
  set_target_properties(cli_test PROPERTIES SUFFIX ".elf")
endif()

# Host client: POSIX terminals only, not part of the console payloads.
if(PSXTERM_PLATFORM STREQUAL "host")
  add_executable(psxterm client/client.c)
  target_link_libraries(psxterm PRIVATE psxterm_core)
endif()

# ---------------------------------------------------------------------------
# Host-side unit tests (never built for PS4/PS5 targets)
# ---------------------------------------------------------------------------
if(PSXTERM_PLATFORM STREQUAL "host" AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()

  set(PSXTERM_TESTS
    test_protocol
    test_parser
    test_env
    test_registry
    test_session
    test_tty
    test_shell
    test_diag
  )

  foreach(test_name ${PSXTERM_TESTS})
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE psxterm_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
