# HLua

Lua 5.4 scripting for Half-Life. A fork of hlsdk-portable where mod
logic is written in lua with a GLua-like api instead of recompiling
the sdk. Runs on GoldSrc and Xash3D as a normal game library.

Status: early development. The server side runtime works: autorun
scripts, hooks, timers, scripted entities that can extend or replace
native classes, and hot reload. Client side, the gen tool and
networking are not built yet.

## Building

waf:

    ./waf configure -T release
    ./waf build

cmake (defaults to 32 bit on x86_64 linux for goldsrc compatibility,
force 64 bit with -D64BIT=ON):

    cmake -DCMAKE_BUILD_TYPE=Release -B build -S .
    cmake --build build

Headless test suite, drives the built server library through a fake
engine (unix only):

    cmake -DCMAKE_BUILD_TYPE=Release -D64BIT=ON -DBUILD_TESTS=ON -B build -S .
    cmake --build build
    ctest --test-dir build

## Script layout

Mods keep their lua next to the game data:

    <gamedir>/lua/autorun/*.lua    ran at startup, in name order
    <gamedir>/lua/shared/*.lua
    <gamedir>/lua/entities/*.lua
    <gamedir>/lua/server/*.lua

Scripted entities are registered from lua and hosted by the lua_ent
carrier class:

    ents.Register( "npc_drone", {
        OnSpawn = function( self )
            self:SetModel( "models/battery.mdl" )
        end,
        OnThink = function( self )
            self:NextThink( 0.5 )
        end,
    } )

Maps place lua_ent with lua_class "npc_drone", scripts use
ents.Create( "npc_drone" ). Registering the name of a native class
replaces its behaviour.

## Layout

    hlua/            runtime sources: vm, hooks, entity bridge
    external/lua54/  vendored lua 5.4.9
    tests/           headless test and the scripted test mod
    _collab/         collaboration workspace: design docs, worklog

## License

Half-Life 1 SDK license, see LICENSE. Lua is MIT, see
external/lua54/README.md.
