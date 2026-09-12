# lib/platform — abstraction layer + simulated/native implementations (REQ §5)
add_library(instr_platform STATIC
  src/simulated.cpp
  src/native.cpp
  src/descriptors.cpp
)
target_include_directories(instr_platform PUBLIC include)
target_link_libraries(instr_platform PUBLIC instr_common)
target_compile_features(instr_platform PUBLIC cxx_std_17)
target_compile_options(instr_platform PRIVATE -Wall -Wextra -Wshadow)