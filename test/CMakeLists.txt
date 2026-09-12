# test — the complete suite (REQ §12): unit, contract, integration,
# fault-injection, secret-leak. Coverage gate enforced in CI (Jenkinsfile).
find_package(GTest REQUIRED)
include(GoogleTest)

if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
  link_libraries(GTest::gtest GTest::gtest_main)
else()
  link_libraries(gtest gtest_main pthread)
endif()

# ---- lib/common ----
add_executable(unit_common unit/test_common.cpp)
target_link_libraries(unit_common PRIVATE instr_common)
gtest_discover_tests(unit_common)

add_executable(unit_json_schema unit/test_json_schema.cpp)
target_link_libraries(unit_json_schema PRIVATE instr_common)
gtest_discover_tests(unit_json_schema)

add_executable(unit_command_table unit/test_command_table.cpp)
target_link_libraries(unit_command_table PRIVATE instr_common)
gtest_discover_tests(unit_command_table)

add_executable(unit_filesystem unit/test_filesystem.cpp)
target_link_libraries(unit_filesystem PRIVATE instr_common instr_ctrl)
gtest_discover_tests(unit_filesystem)

add_executable(unit_default_configs unit/test_default_configs.cpp)
target_link_libraries(unit_default_configs PRIVATE instr_common)
target_compile_definitions(unit_default_configs PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(unit_default_configs)

add_executable(unit_system_files unit/test_system_files.cpp)
target_link_libraries(unit_system_files PRIVATE instr_istate_lib)
target_compile_definitions(unit_system_files PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(unit_system_files)

add_executable(unit_platform unit/test_platform.cpp)
target_link_libraries(unit_platform PRIVATE instr_platform)
gtest_discover_tests(unit_platform)

add_executable(unit_catalog unit/test_catalog_codegen.cpp)
target_link_libraries(unit_catalog PRIVATE instr_alarm_codegen)
gtest_discover_tests(unit_catalog)

# ---- per-daemon domain tests ----
foreach(d monitord networkd hardware faults alarms logmanager telemetry ota backup)
  add_executable(unit_${d} ../daemons/${d}/test/test_domain.cpp)
  target_link_libraries(unit_${d} PRIVATE instr_${d}_lib)
  # Fixtures (test/fixtures) are real artefacts — a genuine kernel log, a
  # manufacturing identity — so parsers are tested against what devices
  # actually produce.
  target_compile_definitions(unit_${d} PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(unit_${d})
endforeach()

# ---- istate domain tests ----
# istate follows the same src/ include/ test/ shape as every other daemon
# (§2), so its tests live with it rather than in a directory of their own.
foreach(t pst provisioning orchestrator northbound config)
  add_executable(unit_istate_${t} ../daemons/istate/test/test_${t}.cpp)
  target_link_libraries(unit_istate_${t} PRIVATE instr_istate_lib)
  target_compile_definitions(unit_istate_${t} PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(unit_istate_${t})
endforeach()

# ---- contract tests (§12): breaking .proto change fails CI ----
add_executable(contract_protos contract/contract_tests.cpp)
add_dependencies(contract_protos protos_golden)
target_link_libraries(contract_protos PRIVATE instr_common instr_istate_lib instr_protos)
gtest_discover_tests(contract_protos)

# ---- integration harness: hub + nine spokes + simulated platform on a
# build host with no hardware (§12) ----
add_executable(integration_harness integration/harness.cpp)
target_link_libraries(integration_harness PRIVATE
  instr_istate_lib instr_monitord_lib instr_networkd_lib instr_hardware_lib
  instr_faults_lib instr_alarms_lib instr_logmanager_lib instr_telemetry_lib
  instr_ota_lib instr_backup_lib)
gtest_discover_tests(integration_harness)

# ---- the real topology: hub + spokes over gRPC/UDS on the simulated
# platform (§12: "the full topology must be testable on a laptop") ----
add_executable(integration_topology integration/topology.cpp)
target_link_libraries(integration_topology PRIVATE
  instr_istate_lib instr_monitord_lib instr_networkd_lib instr_hardware_lib
  instr_faults_lib instr_alarms_lib instr_logmanager_lib instr_telemetry_lib
  instr_ota_lib instr_backup_lib)
gtest_discover_tests(integration_topology)

# ---- colocated applications (R5, §7.6, §7.8, §6.2): the app-facing surface
# driven over a real socket against a live hub with alarms + telemetry ----
add_executable(integration_application integration/application.cpp)
target_link_libraries(integration_application PRIVATE
  instr_istate_lib instr_alarms_lib instr_telemetry_lib)
gtest_discover_tests(integration_application)

# ---- fault injection (§12): storage errors, link loss, hangs, corrupt
# /pst, full disk, clock jumps, proto mismatch ----
add_executable(fault_injection fault-injection/fault_tests.cpp)
target_link_libraries(fault_injection PRIVATE
  instr_istate_lib instr_monitord_lib instr_telemetry_lib instr_networkd_lib)
gtest_discover_tests(fault_injection)

# ---- secret-leak test (§12): credentials never in logs, backups, telemetry ----
add_executable(secret_leak_test secret-leak/secret_leak_test.cpp)
target_link_libraries(secret_leak_test PRIVATE instr_common instr_backup_lib instr_logmanager_lib instr_telemetry_lib)
gtest_discover_tests(secret_leak_test)

# ---- ctrl surface: every daemon's operations reachable from its tool ----
add_executable(unit_ctrl_surface unit/test_ctrl_surface.cpp)
target_link_libraries(unit_ctrl_surface PRIVATE
  instr_istate_lib instr_monitord_lib instr_networkd_lib instr_hardware_lib
  instr_faults_lib instr_alarms_lib instr_logmanager_lib instr_telemetry_lib
  instr_ota_lib instr_backup_lib instr_ctrl)
gtest_discover_tests(unit_ctrl_surface)

# ---- coverage gate inputs (Jenkins parses ctest; coverage via gcovr) ----
# The gate is a hard requirement (§12), so a missing gcovr must fail the
# build rather than quietly removing the target and letting CI "pass" a
# stage that never ran.
find_program(GCOVR_BIN gcovr)
if(NOT GCOVR_BIN AND COVERAGE)
  message(FATAL_ERROR
    "COVERAGE=ON but gcovr was not found. The coverage gate cannot run, and "
    "silently skipping it would let CI report success for a stage that did "
    "not execute. Install gcovr or configure without COVERAGE.")
endif()
if(GCOVR_BIN)
  add_custom_target(coverage
    COMMAND ${GCOVR_BIN} --root ${CMAKE_SOURCE_DIR}
            --filter '.*\\.cpp$' --xml -o ${CMAKE_BINARY_DIR}/coverage.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Coverage report (gate enforced in Jenkins)")
endif()

# ---- golden descriptor for the contract tests ----
set(PROTO_DESC "${CMAKE_BINARY_DIR}/contract/protos.pb")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/contract")
add_custom_command(
  OUTPUT "${PROTO_DESC}"
  COMMAND protobuf::protoc ARGS "-I${CMAKE_SOURCE_DIR}/proto"
          "--descriptor_set_out=${PROTO_DESC}" ${PROTO_FILES_ALL}
  DEPENDS ${PROTO_FILES_ALL}
  COMMENT "Contract: building proto descriptor set")
add_custom_target(protos_golden DEPENDS "${PROTO_DESC}")
target_compile_definitions(contract_protos PRIVATE PROTO_DESC_FILE="${PROTO_DESC}"
                          GOLDEN_FILE="${CMAKE_SOURCE_DIR}/test/contract/protos.golden.sha256")