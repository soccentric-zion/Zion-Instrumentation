# docs — generated interface reference (REQ §14: generated so it cannot drift)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(DOCS_OUT "${CMAKE_SOURCE_DIR}/docs/interface.md")
add_custom_target(gen_docs ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/docs/docgen.py
  DEPENDS ${CMAKE_SOURCE_DIR}/docs/docgen.py ${PROTO_FILES_ALL}
  COMMENT "Generating interface reference from protos and schemas")
