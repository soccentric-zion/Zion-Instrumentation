# backup daemon (REQ §7)
set(DAEMON_SRC
  src/daemon.cpp
  src/domain.cpp)
add_library(instr_backup_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_backup_lib PUBLIC include)
target_link_libraries(instr_backup_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(backup src/main.cpp)
target_link_libraries(backup PRIVATE instr_backup_lib instr_protos)
