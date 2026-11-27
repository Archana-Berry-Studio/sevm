cmake_minimum_required(VERSION 3.16)
project(mb_mochinode_net C)

# mochinode_net — native-first MochiBerry module.
# Builds lib/mochinode_net_x86_64.dll (or .so / .dylib) from native/mochinode_net.c.
# The plugin resolves all MBJS_* runtime symbols from the host engine at load
# time, so it must NOT link against mbjs.exe (importing it fails with error 126
# on Windows). Add -IE:/bcf/mochiberry (or the equivalent engine include dir)
# so mbjs_runtime.h / MBJS headers are visible.

set(NAME mochinode_net)
set(ARCH x86_64)
set(INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
# point this at the engine headers:
set(MBJS_INC "$ENV{MBJS_INC}")
if(NOT MBJS_INC)
  set(MBJS_INC "E:/bcf/mochiberry")
endif()

add_library(mb_${NAME} SHARED native/${NAME}.c)
target_include_directories(mb_${NAME} PRIVATE ${MBJS_INC} ${INC_DIR})

set_target_properties(mb_${NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${NAME}_${ARCH}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib")

if(MSVC)
  target_compile_definitions(mb_${NAME} PRIVATE MBJS_BUILD)
  target_link_libraries(mb_${NAME} PRIVATE bcrypt.lib winhttp.lib)
endif()
if(WIN32 AND NOT MSVC)
  target_link_libraries(mb_${NAME} PRIVATE bcrypt winhttp ws2_32)
endif()
