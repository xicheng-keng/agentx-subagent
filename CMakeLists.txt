cmake_minimum_required(VERSION 3.16)
project(agentx-subagent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

option(AGENTX_BUILD_TESTS "Build the C test binaries" ON)

# --- dependencies -----------------------------------------------------
find_path(LMDB_INCLUDE_DIR lmdb.h REQUIRED)
find_library(LMDB_LIBRARY NAMES lmdb REQUIRED)

find_program(NET_SNMP_CONFIG net-snmp-config)
if(NET_SNMP_CONFIG)
  execute_process(COMMAND ${NET_SNMP_CONFIG} --base-cflags
                  OUTPUT_VARIABLE NETSNMP_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${NET_SNMP_CONFIG} --agent-libs
                  OUTPUT_VARIABLE NETSNMP_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
  separate_arguments(NETSNMP_CFLAGS UNIX_COMMAND "${NETSNMP_CFLAGS}")
  separate_arguments(NETSNMP_LIBS   UNIX_COMMAND "${NETSNMP_LIBS}")
else()
  message(WARNING "net-snmp-config not found; the subagent target is skipped")
endif()

# --- storage abstraction ---------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/src/storage_lmdb.c)
  add_library(agentx_storage STATIC src/storage_lmdb.c)
  target_include_directories(agentx_storage PUBLIC include ${LMDB_INCLUDE_DIR})
  target_link_libraries(agentx_storage PUBLIC ${LMDB_LIBRARY})
endif()

# --- protobuf IPC (nanopb sources are vendored under third_party) ------
set(NANOPB_DIR ${CMAKE_SOURCE_DIR}/third_party/nanopb)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/ipc_server.c AND EXISTS ${NANOPB_DIR}/pb_decode.c)
  add_library(agentx_ipc STATIC
    src/ipc_server.c
    src/generated_pb/agentx_ipc.pb.c
    ${NANOPB_DIR}/pb_common.c
    ${NANOPB_DIR}/pb_encode.c
    ${NANOPB_DIR}/pb_decode.c)
  target_include_directories(agentx_ipc PUBLIC
    include src/generated_pb ${NANOPB_DIR})
  if(TARGET agentx_storage)
    target_link_libraries(agentx_ipc PUBLIC agentx_storage)
  endif()
endif()

# --- the AgentX subagent ----------------------------------------------
file(GLOB AGENTX_GENERATED_SRC ${CMAKE_SOURCE_DIR}/src/generated/*.c)
if(NET_SNMP_CONFIG AND EXISTS ${CMAKE_SOURCE_DIR}/src/main.c AND TARGET agentx_storage)
  add_executable(agentx-subagent
    src/main.c
    src/demo_trap.c
    ${AGENTX_GENERATED_SRC})
  target_include_directories(agentx-subagent PRIVATE include src/generated)
  target_compile_options(agentx-subagent PRIVATE ${NETSNMP_CFLAGS})
  target_link_libraries(agentx-subagent PRIVATE agentx_storage ${NETSNMP_LIBS})
  if(TARGET agentx_ipc)
    target_link_libraries(agentx-subagent PRIVATE agentx_ipc)
  endif()
endif()

# --- tests -------------------------------------------------------------
if(AGENTX_BUILD_TESTS)
  enable_testing()
  foreach(t test_storage test_ipc_framing)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/${t}.c)
      add_executable(${t} tests/${t}.c)
      target_include_directories(${t} PRIVATE include)
      if(TARGET agentx_storage)
        target_link_libraries(${t} PRIVATE agentx_storage)
      endif()
      if(TARGET agentx_ipc)
        target_link_libraries(${t} PRIVATE agentx_ipc)
      endif()
      add_test(NAME ${t} COMMAND ${t})
    endif()
  endforeach()
endif()
