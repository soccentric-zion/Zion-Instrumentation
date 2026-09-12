# ota daemon (REQ §7)
set(DAEMON_SRC
  src/daemon.cpp
  src/domain.cpp)
add_library(instr_ota_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_ota_lib PUBLIC include)
target_link_libraries(instr_ota_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(ota src/main.cpp)
target_link_libraries(ota PRIVATE instr_ota_lib instr_protos)
