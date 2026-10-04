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
  if(APPLE)
    # Keep Darwin's socket, stat and device-number extensions visible.
    add_compile_definitions(_DARWIN_C_SOURCE=1)
  endif()
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
  core/instance.c
  core/diag.c
  core/runtime.c
  core/clock.c
  shell/shell.c
  shell/display.c
  shell/parser.c
  shell/env.c
  shell/command_registry.c
  shell/builtins/builtin_fs.c
  shell/builtins/builtin_cat.c
  shell/builtins/builtin_ls.c
  shell/builtins/builtin_env.c
  shell/builtins/builtin_sys.c
  shell/builtins/builtin_help.c
  shell/builtins/builtin_exit.c
  platform/${PSXTERM_PLATFORM}/platform.c
  platform/${PSXTERM_PLATFORM}/filesystem.c
  platform/${PSXTERM_PLATFORM}/tty.c
  platform/${PSXTERM_PLATFORM}/notify.c
  platform/${PSXTERM_PLATFORM}/loader.c
  platform/${PSXTERM_PLATFORM}/process.c
)

add_library(psxterm_core STATIC ${PSXTERM_CORE_SOURCES})
target_include_directories(psxterm_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(PSXTERM_PLATFORM STREQUAL "host")
  target_compile_definitions(psxterm_core PRIVATE PSXTERM_HOST=1)
  find_package(Threads REQUIRED)
  target_link_libraries(psxterm_core PUBLIC Threads::Threads)
else()
  # Console builds link against the system libraries the payload SDK stubs
  # out. SceNotification carries the rich notification API the daemon uses
  # for its on-screen toasts; on PS4 it does not exist, and that build uses
  # the legacy notification request instead.
  if(PSXTERM_PLATFORM STREQUAL "ps5")
    target_link_libraries(psxterm_core PUBLIC SceNotification)
  endif()
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

  # hello: minimal write-and-exit probe for the spawn path. Its silence or
  # speech separates "payload never reached its entry" from "stdio not
  # connected" during bring-up.
  add_executable(hello tools/hello.c)
  set_target_properties(hello PROPERTIES SUFFIX ".elf")

  # psxterm-exec: runs one command for the daemon. Sent through the loader,
  # whose connection becomes its standard io - the configuration in which a
  # payload's libc output is visible.
  add_executable(exec_wrapper tools/exec_wrapper.c)
  target_link_libraries(exec_wrapper PRIVATE psxterm_core)
  set_target_properties(exec_wrapper PROPERTIES SUFFIX ".elf")

  # netprobe: can a payload reach the network? DNS plus a TCP connect, which is
  # what separates "curl is broken" from "a payload has no network here".
  add_executable(netprobe tools/netprobe.c)
  set_target_properties(netprobe PROPERTIES SUFFIX ".elf")

  # tlsprobe: does TLS work inside a payload? Built against the same OpenSSL
  # the curl bundle was linked with, and only when that bundle exists
  # (tools/ps5-runtime-build.sh produces it).
  if(PSXTERM_PLATFORM STREQUAL "ps5" AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libssl.a")
    add_executable(tlsprobe tools/tlsprobe.c)
    target_include_directories(tlsprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/include")
    target_link_libraries(tlsprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libssl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libcrypto.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libz.a")
    set_target_properties(tlsprobe PROPERTIES SUFFIX ".elf")

    # fetchprobe: the same link set curl uses, libpsl included, so its
    # behaviour says whether libpsl is where curl stops.
    add_executable(fetchprobe tools/fetchprobe.c)
    target_include_directories(fetchprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/include")
    target_link_libraries(fetchprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libssl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libcrypto.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libpsl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libz.a")
    set_target_properties(fetchprobe PROPERTIES SUFFIX ".elf")

    # psxterm-fetch: a real downloader on the console, built on the stack that
    # is measured working (TLS 1.3 plus certificate validation). Body on
    # standard output, messages on standard error, so it composes in a shell.
    add_executable(psxterm-fetch tools/fetch.c)
    target_include_directories(psxterm-fetch PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/include")
    target_link_libraries(psxterm-fetch PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libssl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libcrypto.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libz.a")
    set_target_properties(psxterm-fetch PROPERTIES SUFFIX ".elf")

    # curlprobe: is libcurl itself usable in a payload? It splits curl's
    # failure between the library and the CLI around it.
    add_executable(curlprobe tools/curlprobe.c)
    target_include_directories(curlprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/include")
    target_link_libraries(curlprobe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libcurl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libssl.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libcrypto.a"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ps5-runtime/lib/libz.a")
    set_target_properties(curlprobe PROPERTIES SUFFIX ".elf")
  endif()

  # pslist: PS5 bring-up helper that lists processes through sysctl.
  if(PSXTERM_PLATFORM STREQUAL "ps5")
    add_executable(pslist tools/pslist.c)
    set_target_properties(pslist PROPERTIES SUFFIX ".elf")
  endif()
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
    test_runtime
  )

  foreach(test_name ${PSXTERM_TESTS})
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE psxterm_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
