# alarms daemon (REQ §7)
set(DAEMON_SRC
  src/daemon.cpp
  src/domain.cpp)
add_library(instr_alarms_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_alarms_lib PUBLIC include)
target_link_libraries(instr_alarms_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(alarms src/main.cpp)
target_link_libraries(alarms PRIVATE instr_alarms_lib instr_protos)
