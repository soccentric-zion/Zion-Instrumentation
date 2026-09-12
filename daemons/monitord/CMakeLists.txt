# monitord daemon (REQ §7)
set(DAEMON_SRC
  src/domain.cpp
  src/spoke.cpp
  src/transport.cpp)
add_library(instr_monitord_lib STATIC ${DAEMON_SRC})
target_include_directories(instr_monitord_lib PUBLIC include)
target_link_libraries(instr_monitord_lib PUBLIC instr_common instr_platform instr_transport instr_protos instr_ctrl)

add_executable(monitord src/main.cpp)
target_link_libraries(monitord PRIVATE instr_monitord_lib instr_protos)
