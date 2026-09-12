# tools — one _ctrl per daemon with full operation coverage (REQ §3.2).
# The command table lives in the daemon library (single source of truth);
# each tool links its daemon's library so help and completion are structural.
set(TOOLS
  istate_ctrl:istate
  network_ctrl:networkd
  monitor_ctrl:monitord
  hardware_ctrl:hardware
  fault_ctrl:faults
  alarm_ctrl:alarms
  log_ctrl:logmanager
  telemetry_ctrl:telemetry
  ota_ctrl:ota
  backup_ctrl:backup
)
foreach(pair IN LISTS TOOLS)
  string(REPLACE ":" ";" pair_list ${pair})
  list(GET pair_list 0 prog)
  list(GET pair_list 1 daemon)
  add_executable(${prog} ${prog}/main.cpp)
  target_link_libraries(${prog} PRIVATE instr_${daemon}_lib instr_ctrl)
  target_compile_definitions(${prog} PRIVATE
    INSTR_CTRL_PROG="${prog}"
    INSTR_CTRL_SOCKET="/run/instrumentation/${daemon}-ctrl.sock")
endforeach()
