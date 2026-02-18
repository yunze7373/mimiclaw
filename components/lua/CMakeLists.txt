cmake_minimum_required(VERSION 3.16)

# Lua 5.4.7 component for ESP-IDF
#
# SETUP: Run the download script first:
#   Linux/Mac: bash components/lua/download_lua.sh
#   Windows:   powershell components/lua/download_lua.ps1
#
# This will place Lua source in components/lua/src/

set(LUA_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# Check if Lua source exists
if(NOT EXISTS ${LUA_SRC_DIR}/lua.h)
    message(FATAL_ERROR
        "\n"
        "Lua source not found at ${LUA_SRC_DIR}/\n"
        "Run the download script first:\n"
        "  Linux/Mac: bash components/lua/download_lua.sh\n"
        "  Windows:   powershell components/lua/download_lua.ps1\n"
    )
endif()

# Core Lua library (exclude lua.c and luac.c — standalone binaries)
set(LUA_SOURCES
    ${LUA_SRC_DIR}/lapi.c
    ${LUA_SRC_DIR}/lauxlib.c
    ${LUA_SRC_DIR}/lbaselib.c
    ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lcorolib.c
    ${LUA_SRC_DIR}/lctype.c
    ${LUA_SRC_DIR}/ldblib.c
    ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/ldo.c
    ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lfunc.c
    ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/linit.c
    ${LUA_SRC_DIR}/liolib.c
    ${LUA_SRC_DIR}/llex.c
    ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/lmem.c
    ${LUA_SRC_DIR}/loadlib.c
    ${LUA_SRC_DIR}/lobject.c
    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/loslib.c
    ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/lstate.c
    ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/lstrlib.c
    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/ltablib.c
    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lzio.c
)

idf_component_register(
    SRCS ${LUA_SOURCES}
    INCLUDE_DIRS ${LUA_SRC_DIR}
)

# Suppress warnings from third-party Lua source
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
