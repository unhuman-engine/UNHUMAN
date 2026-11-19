# Lua 5.4.7, built as a static library for the scripting host.
#
# No LUA_USE_* defines on purpose: the engine's sandbox opens neither `os`, `io`
# nor `package`, so the POSIX/dlopen extras those add are unused, and leaving them
# out keeps one build recipe for every platform. See README.vendor.md.

set(LUA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Everything upstream ships except the standalone interpreter (lua.c), the
# bytecode compiler (luac.c) and the single-file amalgamation (onelua.c).
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
    ${LUA_SRC_DIR}/lua.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lutf8lib.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lzio.c
)

# lua.c and luac.c are the interpreters; the list above keeps lua.c out by not
# naming it (see the file header).
list(REMOVE_ITEM LUA_SOURCES ${LUA_SRC_DIR}/lua.c)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${LUA_SRC_DIR})

# The C sources are third-party and warning-clean only under their own flags.
if(MSVC)
    target_compile_options(lua PRIVATE /W0)
else()
    target_compile_options(lua PRIVATE -w)
endif()

if(UNIX)
    target_link_libraries(lua PUBLIC dl m)
endif()
