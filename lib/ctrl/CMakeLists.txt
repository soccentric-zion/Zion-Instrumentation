# lib/ctrl — raw-UDS control server/client framework (REQ §3.2)
add_library(instr_ctrl STATIC
  src/ctrl.cpp
)
target_include_directories(instr_ctrl PUBLIC include)
target_link_libraries(instr_ctrl PUBLIC instr_common)
target_compile_features(instr_ctrl PUBLIC cxx_std_17)
target_compile_options(instr_ctrl PRIVATE -Wall -Wextra -Wshadow)