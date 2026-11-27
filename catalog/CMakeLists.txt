# catalog — alarm/fault catalog and the code generator (REQ §8)
# alarms.yaml is code-generated into a shared C++ header at build time so
# every application uses the same IDs.
add_library(instr_alarm_codegen STATIC src/codegen.cpp)
target_include_directories(instr_alarm_codegen PUBLIC include)
target_link_libraries(instr_alarm_codegen PUBLIC instr_common)

add_executable(alarmgen src/alarmgen_main.cpp)
target_link_libraries(alarmgen PRIVATE instr_alarm_codegen)

# Generated header, produced at build time.
set(ALARM_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/alarms.yaml")
set(ALARM_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_alarms")
file(MAKE_DIRECTORY "${ALARM_GEN_DIR}")

# Cross-compiling (Yocto): the alarmgen built by this tree is a TARGET-arch
# binary that cannot run on the build host, and a bare target-name COMMAND is
# rewritten by CMake to that binary's own path — bypassing PATH entirely, so
# staging a host-native alarmgen on PATH is not enough by itself. When cross
# compiling, resolve the host tool with find_program(NO_CMAKE_FIND_ROOT_PATH)
# instead; the Yocto recipe stages it via DEPENDS instrumentation-native.
# Native builds keep the target-name COMMAND (CMake rewrites it to the
# freshly built binary).
if(CMAKE_CROSSCOMPILING)
  # find_program's REQUIRED keyword needs CMake >= 3.18 while this project
  # declares 3.16 — check by hand, same as the grpc_cpp_plugin lookup.
  find_program(ALARMGEN_HOST alarmgen NO_CMAKE_FIND_ROOT_PATH)
  if(NOT ALARMGEN_HOST)
    message(FATAL_ERROR
      "Cross-compiling: no host-native alarmgen on PATH. Build the native "
      "variant first (Yocto: DEPENDS instrumentation-native stages it).")
  endif()
  set(ALARMGEN_CMD "${ALARMGEN_HOST}")
else()
  set(ALARMGEN_CMD "alarmgen")
endif()
add_custom_command(
  OUTPUT "${ALARM_GEN_DIR}/instr/alarms_gen.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ALARM_GEN_DIR}/instr"
  COMMAND ${ALARMGEN_CMD} "${ALARM_CATALOG}" "${ALARM_GEN_DIR}/instr/alarms_gen.h"
  DEPENDS alarmgen "${ALARM_CATALOG}"
  COMMENT "Generating alarm ID header from catalog/alarms.yaml")
add_custom_target(alarm_gen_header ALL DEPENDS "${ALARM_GEN_DIR}/instr/alarms_gen.h")
add_library(instr_alarm_ids INTERFACE)
target_include_directories(instr_alarm_ids INTERFACE "${ALARM_GEN_DIR}")
add_dependencies(instr_alarm_ids alarm_gen_header)