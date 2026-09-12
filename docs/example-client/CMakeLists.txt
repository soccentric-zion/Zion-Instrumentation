# Minimal ApplicationApi client (Task.md E-03).
#
# This is a STANDALONE project on purpose: it is built the way a real
# application is built -- against the *installed* instrumentation package, not
# from inside this source tree. If it only compiled as a subdirectory of the
# suite it would not prove that an application can consume the package at all,
# which is the exact gap E-03 exists to close.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<staging-or-install-prefix>
#   cmake --build build
cmake_minimum_required(VERSION 3.16)
project(instr-example-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(instrumentation REQUIRED)

add_executable(instr-example-client main.cpp)

# Generates istate.pb.cc / istate.grpc.pb.cc plus everything istate.proto
# imports (common, telemetry, faults, logmanager) — resolved automatically.
instrumentation_generate_client(instr-example-client PROTOS istate.proto)

target_link_libraries(instr-example-client PRIVATE instrumentation::client_deps)
