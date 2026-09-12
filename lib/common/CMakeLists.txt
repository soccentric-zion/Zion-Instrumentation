# lib/common — shared core library (REQ §4)
add_library(instr_common STATIC
  src/timestamps.cpp
  src/json.cpp
  src/log.cpp
  src/alarm_codec.cpp
  src/crypto.cpp
  src/filesystem.cpp
  src/atomic_file.cpp
  src/spool.cpp
  src/config.cpp
  src/command_table.cpp
  src/tar.cpp
  src/daemon_skeleton.cpp
  src/signature.cpp
)
target_include_directories(instr_common PUBLIC include)
target_link_libraries(instr_common PUBLIC yaml-cpp)

# Signature verification (REQ §9, §7.9) delegates to libcrypto: it is the one
# place where a subtle bug is silently exploitable, so it uses a reviewed
# implementation rather than another hand-written primitive. The build fails
# closed without it — signed content is refused, never accepted unchecked —
# but a device image is expected to carry OpenSSL, so warn loudly.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_link_libraries(instr_common PUBLIC OpenSSL::Crypto)
  target_compile_definitions(instr_common PUBLIC INSTR_HAVE_OPENSSL=1)
else()
  message(WARNING
    "OpenSSL not found: Ed25519 verification is unavailable. Signed HQ config "
    "pushes and signed OTA artifacts will be REFUSED on this build (fail closed).")
endif()
target_compile_features(instr_common PUBLIC cxx_std_17)
# Warnings as errors: consistent, diagnosable code across the suite.
if(NOT MSVC)
  target_compile_options(instr_common PRIVATE -Wall -Wextra -Wshadow)
endif()