# Lua 5.4.9, unmodified upstream sources
# built as plain portable C, no platform defines: no dl module loading

set(LUA54_SOURCES
        lapi.c
        lauxlib.c
        lbaselib.c
        lcode.c
        lcorolib.c
        lctype.c
        ldblib.c
        ldebug.c
        ldo.c
        ldump.c
        lfunc.c
        lgc.c
        linit.c
        liolib.c
        llex.c
        lmathlib.c
        lmem.c
        loadlib.c
        lobject.c
        lopcodes.c
        loslib.c
        lparser.c
        lstate.c
        lstring.c
        lstrlib.c
        ltablib.c
        ltable.c
        ltm.c
        lundump.c
        lutf8lib.c
        lvm.c
        lzio.c
)

add_library(lua54 STATIC ${LUA54_SOURCES})
set_target_properties(lua54 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lua54 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT WIN32)
        target_link_libraries(lua54 PUBLIC m)
endif()

# standalone interpreter, used to run tools/gen.lua without a system lua
add_executable(lua54i lua.c)
target_link_libraries(lua54i lua54)
