# telemetry daemon (REQ §7)
set(DAEMON_SRC
  src/daemon.cpp
  src/domain.cpp)
add_library(instr_telemetry_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_telemetry_lib PUBLIC include)
target_link_libraries(instr_telemetry_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(telemetry src/main.cpp)
target_link_libraries(telemetry PRIVATE instr_telemetry_lib instr_protos)
