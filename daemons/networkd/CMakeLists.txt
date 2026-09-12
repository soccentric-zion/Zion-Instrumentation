# networkd daemon (REQ §7)
set(DAEMON_SRC
  src/daemon.cpp
  src/domain.cpp)
add_library(instr_networkd_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_networkd_lib PUBLIC include)
target_link_libraries(instr_networkd_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(networkd src/main.cpp)
target_link_libraries(networkd PRIVATE instr_networkd_lib instr_protos)
