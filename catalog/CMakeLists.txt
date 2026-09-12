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
add_custom_command(
  OUTPUT "${ALARM_GEN_DIR}/instr/alarms_gen.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ALARM_GEN_DIR}/instr"
  COMMAND alarmgen "${ALARM_CATALOG}" "${ALARM_GEN_DIR}/instr/alarms_gen.h"
  DEPENDS alarmgen "${ALARM_CATALOG}"
  COMMENT "Generating alarm ID header from catalog/alarms.yaml")
add_custom_target(alarm_gen_header ALL DEPENDS "${ALARM_GEN_DIR}/instr/alarms_gen.h")
add_library(instr_alarm_ids INTERFACE)
target_include_directories(instr_alarm_ids INTERFACE "${ALARM_GEN_DIR}")
add_dependencies(instr_alarm_ids alarm_gen_header)