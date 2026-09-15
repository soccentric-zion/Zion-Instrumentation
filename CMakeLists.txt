# Arches Instrumentation — top-level build (REQ §14).
# One CMake project, one version for the whole suite (from VERSION).
cmake_minimum_required(VERSION 3.16)
project(instrumentation VERSION 0.0.0 LANGUAGES CXX)

# ---- version from VERSION file ----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" INSTR_VERSION_RAW)
string(STRIP "${INSTR_VERSION_RAW}" INSTR_VERSION)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/instr/common/version_gen.h" @ONLY)

# ---- compiler baseline ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wshadow)

# ---- dependencies ----
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPC REQUIRED grpc++)
  set(GRPCPP_LIBRARIES ${GRPC_LINK_LIBRARIES})
  set(GRPCPP_INCLUDE_DIRS ${GRPC_INCLUDE_DIRS})
else()
  set(GRPCPP_LIBRARIES gRPC::grpc++ protobuf::libprotobuf)
endif()
find_package(yaml-cpp REQUIRED)
find_package(GTest QUIET)

SET (MAINFOLDER ${PROJECT_SOURCE_DIR}/output_dir)
SET (EXECUTABLE_OUTPUT_PATH "${MAINFOLDER}/bin")
SET (LIBRARY_OUTPUT_PATH "${MAINFOLDER}/lib")

# ---- proto generation (R3: one IDL, one generator) ----
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES_ALL
  ${PROTO_DIR}/common.proto ${PROTO_DIR}/istate.proto ${PROTO_DIR}/networkd.proto
  ${PROTO_DIR}/monitord.proto ${PROTO_DIR}/hardware.proto ${PROTO_DIR}/faults.proto
  ${PROTO_DIR}/alarms.proto ${PROTO_DIR}/logmanager.proto ${PROTO_DIR}/telemetry.proto
  ${PROTO_DIR}/ota.proto ${PROTO_DIR}/backup.proto)
set(PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/protos_gen")
file(MAKE_DIRECTORY "${PROTO_GEN}")
set(PROTO_SRCS)
set(PROTO_HDRS)
# No `set(GRPC_PLUGIN "")` here: under CMP0126 OLD (cmake_minimum_required
# 3.16) a pre-set normal variable shadows find_program()'s result, and the
# plugin path ends up empty on platforms where gRPC ships no CMake config
# (Ubuntu 22.04 / JetPack 6, Yocto) — protoc then fails with "program not
# found". The host toolchains (newer gRPC, gRPC::grpc_cpp_plugin target)
# never took the find_program branch, which is why this only showed up there.
if(TARGET gRPC::grpc_cpp_plugin)
  get_target_property(GRPC_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
else()
  find_program(GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
endif()
foreach(P common istate networkd monitord hardware faults alarms logmanager telemetry ota backup)
  list(APPEND PROTO_SRCS "${PROTO_GEN}/${P}.pb.cc" "${PROTO_GEN}/${P}.grpc.pb.cc")
  list(APPEND PROTO_HDRS "${PROTO_GEN}/${P}.pb.h" "${PROTO_GEN}/${P}.grpc.pb.h")
  add_custom_command(
    OUTPUT "${PROTO_GEN}/${P}.pb.cc" "${PROTO_GEN}/${P}.pb.h"
           "${PROTO_GEN}/${P}.grpc.pb.cc" "${PROTO_GEN}/${P}.grpc.pb.h"
    COMMAND protobuf::protoc
    ARGS "--cpp_out=${PROTO_GEN}"
         "--grpc_out=${PROTO_GEN}"
         "--plugin=protoc-gen-grpc=${GRPC_PLUGIN}"
         "-I${PROTO_DIR}" "${PROTO_DIR}/${P}.proto"
    DEPENDS "${PROTO_DIR}/${P}.proto" "${PROTO_DIR}/common.proto"
    COMMENT "protoc ${P}.proto")
endforeach()
add_library(instr_protos STATIC ${PROTO_SRCS})
target_include_directories(instr_protos PUBLIC "${PROTO_GEN}" "${PROTO_DIR}")
target_link_libraries(instr_protos PUBLIC protobuf::libprotobuf)
# protobuf 4.25's generated map-field code (TypeDefinedMapFieldBase, used by
# messages with a map<..> field, e.g. hardware.proto/faults.proto) calls
# directly into Abseil's CHECK/LOG machinery. protobuf::libprotobuf is a
# shared library here and its own INTERFACE_LINK_LIBRARIES (via absl::absl_log)
# doesn't transitively pull these specific _internal targets into a
# consumer's link line, so any executable linking instr_protos statically
# gets "undefined reference to absl::...log_internal::..." -- confirmed by
# an actual build, every daemon that happens to touch a map field fails.
# Link the concrete internal targets explicitly rather than relying on the
# umbrella target.
if(TARGET absl::log_internal_message)
  target_link_libraries(instr_protos PUBLIC
    absl::log_internal_message
    absl::log_internal_check_op
    absl::log_internal_conditions
    absl::log_internal_nullguard
    absl::log_internal_globals
    absl::log_internal_log_sink_set
    absl::log_internal_proto)
endif()
if(TARGET gRPC::grpc++)
  target_link_libraries(instr_protos PUBLIC gRPC::grpc++)
else()
  target_link_libraries(instr_protos PUBLIC ${GRPCPP_LIBRARIES})
  target_include_directories(instr_protos SYSTEM PUBLIC ${GRPCPP_INCLUDE_DIRS})
endif()
target_compile_options(instr_protos PRIVATE -w)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

add_subdirectory(lib/common)
add_subdirectory(lib/platform)
add_subdirectory(lib/ctrl)
add_subdirectory(lib/transport)
target_link_libraries(instr_transport PUBLIC instr_protos)

# ---- catalog code generator (REQ §8) ----
add_subdirectory(catalog)

# ---- daemons (§7) ----
add_subdirectory(daemons/istate)
add_subdirectory(daemons/networkd)
add_subdirectory(daemons/monitord)
add_subdirectory(daemons/hardware)
add_subdirectory(daemons/faults)
add_subdirectory(daemons/alarms)
add_subdirectory(daemons/logmanager)
add_subdirectory(daemons/telemetry)
add_subdirectory(daemons/ota)
add_subdirectory(daemons/backup)

# ---- tools: one _ctrl per daemon (§3.2) ----
add_subdirectory(tools)

# ---- static analysis (§14) ----
# The Jenkins pipeline builds this target, so it has to exist: a pipeline
# stage that silently does nothing is worse than no stage at all.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_program(CLANG_TIDY_BIN NAMES run-clang-tidy run-clang-tidy-18 run-clang-tidy-17)
if(CLANG_TIDY_BIN)
  add_custom_target(run-clang-tidy
    COMMAND ${CLANG_TIDY_BIN} -p "${CMAKE_BINARY_DIR}"
            -quiet
            "${CMAKE_SOURCE_DIR}/lib" "${CMAKE_SOURCE_DIR}/daemons" "${CMAKE_SOURCE_DIR}/tools"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "clang-tidy over lib/, daemons/ and tools/"
    VERBATIM)
else()
  add_custom_target(run-clang-tidy
    COMMAND ${CMAKE_COMMAND} -E echo
            "run-clang-tidy not found: install clang-tidy to run static analysis"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "clang-tidy unavailable"
    VERBATIM)
endif()

# ---- formatting (§14) ----
# Same philosophy as run-clang-tidy: the gate exists and fails the build when
# the code drifts from .clang-format. check-format verifies (CI), format fixes
# in place. Scope is every hand-written C++ file: lib/, daemons/, tools/,
# test/, catalog/ — generated code lives in the binary dir and is never globed.
file(GLOB_RECURSE INSTR_FORMAT_SRCS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/lib/*.cpp"     "${CMAKE_SOURCE_DIR}/lib/*.h"
  "${CMAKE_SOURCE_DIR}/daemons/*.cpp" "${CMAKE_SOURCE_DIR}/daemons/*.h"
  "${CMAKE_SOURCE_DIR}/tools/*.cpp"   "${CMAKE_SOURCE_DIR}/tools/*.h"
  "${CMAKE_SOURCE_DIR}/test/*.cpp"    "${CMAKE_SOURCE_DIR}/test/*.h"
  "${CMAKE_SOURCE_DIR}/catalog/*.cpp" "${CMAKE_SOURCE_DIR}/catalog/*.h")
find_program(CLANG_FORMAT_BIN NAMES clang-format clang-format-18 clang-format-17)
if(CLANG_FORMAT_BIN)
  add_custom_target(check-format
    COMMAND ${CLANG_FORMAT_BIN} --dry-run -Werror --style=file ${INSTR_FORMAT_SRCS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "clang-format check over lib/, daemons/, tools/, test/, catalog/"
    VERBATIM)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_BIN} -i --style=file ${INSTR_FORMAT_SRCS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "clang-format apply over lib/, daemons/, tools/, test/, catalog/"
    VERBATIM)
else()
  add_custom_target(check-format
    COMMAND ${CMAKE_COMMAND} -E echo
            "clang-format not found: install clang-format to check formatting"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "clang-format unavailable"
    VERBATIM)
  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo
            "clang-format not found: install clang-format to format"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "clang-format unavailable"
    VERBATIM)
endif()

# ---- application SDK install (§7.6 R5, Task.md E-03) ----
#
# A colocated application declares alarms and reports metrics over
# ApplicationApi, which means it has to compile a gRPC client. Nothing was
# installed for that: no .proto files, no package config, no -dev package. An
# application could be given the right group (E-01) and still have nothing to
# build against, which is why no leaf links one today.
#
# What ships is the .proto files plus a package config with a codegen helper —
# NOT a prebuilt stub library. Generated protobuf/gRPC sources must be compiled
# by the same toolchain and against the same protobuf runtime as the consumer;
# handing a static archive across a sysroot boundary is how ODR and ABI
# mismatches happen. See cmake/instrumentationConfig.cmake.in.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(INSTR_PROTO_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/instrumentation/proto")
install(DIRECTORY "${PROTO_DIR}/" DESTINATION "${INSTR_PROTO_INSTALL_DIR}"
        FILES_MATCHING PATTERN "*.proto")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/instrumentationConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/instrumentationConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/instrumentation"
  PATH_VARS INSTR_PROTO_INSTALL_DIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/instrumentationConfigVersion.cmake"
  # INSTR_VERSION (from the VERSION file), not PROJECT_VERSION: project() is
  # declared 0.0.0 as a placeholder and the real version is read from VERSION
  # a few lines later. Using PROJECT_VERSION here would publish a package that
  # advertises 0.0.0 and fails every consumer's version check.
  VERSION "${INSTR_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/instrumentationConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/instrumentationConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/instrumentation")

# ---- tests (§12) ----
enable_testing()
# ---- docs (§14: generated so it cannot drift) ----
add_subdirectory(docs)
add_subdirectory(test)