# istate daemon (REQ §7)
set(DAEMON_SRC
  src/config_service.cpp
  src/core.cpp
  src/hubs.cpp
  src/northbound.cpp
  src/orchestrator.cpp
  src/provisioning.cpp
  src/pst.cpp
  src/table.cpp)
add_library(instr_istate_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_istate_lib PUBLIC include)
# instr_alarm_ids carries the generated alarm-id header (§8): istate declares
# its own alarms by id, from the same catalog every other component uses.
target_link_libraries(instr_istate_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl instr_alarm_ids)
add_dependencies(instr_istate_lib alarm_gen_header)

add_executable(istate src/main.cpp)
target_link_libraries(istate PRIVATE instr_istate_lib instr_protos)
