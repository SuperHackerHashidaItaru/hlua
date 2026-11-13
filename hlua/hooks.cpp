#include <string.h>
#include <stdio.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "hlua_internal.h"

namespace hlua
{

// hook storage: registry["hlua_hooks"][name] = { {id, fn}, ... }
// listeners live in an array so iteration order is insertion order and
// the empty check is one rawlen

static bool BeginFire( lua_State *L, const char *name )
{
        if( !L )
                return false;
        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_hooks" );
        if( !lua_istable( L, -1 ) )
        {
                lua_pop( L, 1 );
                return false;
        }
        lua_getfield( L, -1, name );
        lua_remove( L, -2 );
        if( !lua_istable( L, -1 ) || lua_rawlen( L, -1 ) == 0 )
        {
                lua_pop( L, 1 );
                return false;
        }
        return true;
}

bool FireHook0( const char *name )
{
        int i, n;

        if( !BeginFire( gL, name ) )
                return false;
        n = lua_rawlen( gL, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( gL, -1, i );
                if( lua_isnil( gL, -1 ) )
                {
                        lua_pop( gL, 1 );
                        continue;
                }
                lua_rawgeti( gL, -1, 2 );
                if( lua_pcall( gL, 0, 0, 0 ) != LUA_OK )
                        ReportError();
                lua_pop( gL, 1 );
        }
        lua_pop( gL, 1 );
        return true;
}

void FireHookE( const char *name, edict_t *ent )
{
        int i, n;

        if( !BeginFire( gL, name ) )
                return;
        n = lua_rawlen( gL, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( gL, -1, i );
                if( lua_isnil( gL, -1 ) )
                {
                        lua_pop( gL, 1 );
                        continue;
                }
                lua_rawgeti( gL, -1, 2 );
                PushEntity( gL, ent );
                if( lua_pcall( gL, 1, 0, 0 ) != LUA_OK )
                        ReportError();
                lua_pop( gL, 1 );
        }
        lua_pop( gL, 1 );
}

static int HookAdd( lua_State *L )
{
        const char *name = luaL_checkstring( L, 1 );
        int i, n;

        luaL_checkany( L, 2 );
        luaL_checktype( L, 3, LUA_TFUNCTION );

        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_hooks" );
        lua_getfield( L, -1, name );
        if( !lua_istable( L, -1 ) )
        {
                lua_pop( L, 1 );
                lua_createtable( L, 4, 0 );
                lua_pushvalue( L, -1 );
                lua_setfield( L, -3, name );
        }
        n = lua_rawlen( L, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( L, -1, i );
                lua_rawgeti( L, -1, 1 );
                if( lua_compare( L, -1, 2, LUA_OPEQ ) )
                {
                        lua_pop( L, 1 );
                        lua_pushvalue( L, 3 );
                        lua_rawseti( L, -2, 2 );
                        lua_pop( L, 3 );
                        return 0;
                }
                lua_pop( L, 2 );
        }
        lua_createtable( L, 2, 0 );
        lua_pushvalue( L, 2 );
        lua_rawseti( L, -2, 1 );
        lua_pushvalue( L, 3 );
        lua_rawseti( L, -2, 2 );
        lua_rawseti( L, -2, n + 1 );
        lua_pop( L, 2 );
        return 0;
}

static int HookRemove( lua_State *L )
{
        const char *name = luaL_checkstring( L, 1 );
        int i, n;

        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_hooks" );
        lua_getfield( L, -1, name );
        if( !lua_istable( L, -1 ) )
        {
                lua_pop( L, 2 );
                return 0;
        }
        n = lua_rawlen( L, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( L, -1, i );
                lua_rawgeti( L, -1, 1 );
                if( lua_compare( L, -1, 2, LUA_OPEQ ) )
                {
                        lua_pop( L, 2 );
                        lua_rawgeti( L, -1, n );
                        lua_rawseti( L, -2, i );
                        lua_pushnil( L );
                        lua_rawseti( L, -2, n );
                        lua_pop( L, 2 );
                        return 0;
                }
                lua_pop( L, 2 );
        }
        lua_pop( L, 2 );
        return 0;
}

static int HookRun( lua_State *L )
{
        int i, n, a, top = lua_gettop( L );
        int li = top + 1;
        bool got = false;

        luaL_checkstring( L, 1 );
        if( !BeginFire( L, lua_tostring( L, 1 ) ) )
                return 0;
        n = lua_rawlen( L, li );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( L, li, i );
                if( lua_isnil( L, -1 ) )
                {
                        lua_pop( L, 1 );
                        continue;
                }
                lua_rawgeti( L, -1, 2 );
                for( a = 2; a <= top; a++ )
                        lua_pushvalue( L, a );
                if( lua_pcall( L, top - 1, 1, 0 ) != LUA_OK )
                {
                        ReportError();
                        lua_pop( L, 1 );
                }
                else if( !lua_isnil( L, -1 ) )
                {
                        lua_remove( L, -2 );
                        got = true;
                        break;
                }
                else
                        lua_pop( L, 2 );
        }
        if( !got )
                lua_pop( L, 1 );
        return got ? 1 : 0;
}

void RegisterHookLib( lua_State *L )
{
        lua_createtable( L, 0, 3 );
        lua_pushcfunction( L, HookAdd );
        lua_setfield( L, -2, "Add" );
        lua_pushcfunction( L, HookRemove );
        lua_setfield( L, -2, "Remove" );
        lua_pushcfunction( L, HookRun );
        lua_setfield( L, -2, "Run" );
        lua_setglobal( L, "hook" );
}

// hooks with results share one iteration shape:
// stack per listener: [... listeners, sub, fn, args...] then result

bool OnClientConnect( const char *name, const char *address, char *reject, int rejectlen )
{
        int i, n;
        bool rejected = false;

        if( !BeginFire( gL, "PlayerConnect" ) )
                return false;
        n = lua_rawlen( gL, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( gL, -1, i );
                if( lua_isnil( gL, -1 ) )
                {
                        lua_pop( gL, 1 );
                        continue;
                }
                lua_rawgeti( gL, -1, 2 );
                lua_pushstring( gL, name );
                lua_pushstring( gL, address );
                if( lua_pcall( gL, 2, 1, 0 ) != LUA_OK )
                {
                        ReportError();
                        lua_pop( gL, 1 );
                }
                else if( lua_isstring( gL, -1 ) )
                {
                        snprintf( reject, rejectlen, "%s", lua_tostring( gL, -1 ) );
                        rejected = true;
                        lua_pop( gL, 2 );
                        break;
                }
                else
                        lua_pop( gL, 2 );
        }
        lua_pop( gL, 1 );
        return rejected;
}

bool OnClientCommand( edict_t *pent )
{
        int i, n, a, argc = CMD_ARGC();
        bool handled = false;

        if( !BeginFire( gL, "PlayerCommand" ) )
                return false;
        n = lua_rawlen( gL, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( gL, -1, i );
                if( lua_isnil( gL, -1 ) )
                {
                        lua_pop( gL, 1 );
                        continue;
                }
                lua_rawgeti( gL, -1, 2 );
                PushEntity( gL, pent );
                lua_createtable( gL, argc, 0 );
                for( a = 0; a < argc; a++ )
                {
                        lua_pushstring( gL, CMD_ARGV( a ) );
                        lua_rawseti( gL, -2, a + 1 );
                }
                if( lua_pcall( gL, 2, 1, 0 ) != LUA_OK )
                {
                        ReportError();
                        lua_pop( gL, 1 );
                }
                else if( lua_toboolean( gL, -1 ) )
                {
                        handled = true;
                        lua_pop( gL, 2 );
                        break;
                }
                else
                        lua_pop( gL, 2 );
        }
        lua_pop( gL, 1 );
        return handled;
}

// returns true when the message must be dropped, rewrites text in place
bool OnSay( edict_t *pent, char *text, int textlen, int teamonly )
{
        int i, n;
        bool block = false;

        if( !BeginFire( gL, "PlayerSay" ) )
                return false;
        n = lua_rawlen( gL, -1 );
        for( i = 1; i <= n; i++ )
        {
                lua_rawgeti( gL, -1, i );
                if( lua_isnil( gL, -1 ) )
                {
                        lua_pop( gL, 1 );
                        continue;
                }
                lua_rawgeti( gL, -1, 2 );
                PushEntity( gL, pent );
                lua_pushstring( gL, text );
                lua_pushboolean( gL, teamonly );
                if( lua_pcall( gL, 3, 1, 0 ) != LUA_OK )
                {
                        ReportError();
                        lua_pop( gL, 1 );
                }
                else if( lua_isboolean( gL, -1 ) && !lua_toboolean( gL, -1 ) )
                {
                        block = true;
                        lua_pop( gL, 2 );
                        break;
                }
                else if( lua_isstring( gL, -1 ) )
                {
                        snprintf( text, textlen, "%s", lua_tostring( gL, -1 ) );
                        lua_pop( gL, 2 );
                        break;
                }
                else
                        lua_pop( gL, 2 );
        }
        lua_pop( gL, 1 );
        return block;
}

void OnClientPutInServer( edict_t *pent )
{
        FireHookE( "PlayerInitialSpawn", pent );
}

void OnClientDisconnect( edict_t *pent )
{
        FireHookE( "PlayerDisconnected", pent );
}

void OnPlayerPreThink( edict_t *pent )
{
        FireHookE( "PlayerPreThink", pent );
}

void OnPlayerPostThink( edict_t *pent )
{
        FireHookE( "PlayerPostThink", pent );
}

void OnSpawn( edict_t *pent )
{
        FireHookE( "OnEntityCreated", pent );
}

} // namespace hlua
