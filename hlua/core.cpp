#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "hlua_internal.h"
#include "prelude.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace hlua
{

lua_State *gL = NULL;

static char sGameDir[512];
static int sTickRef = LUA_NOREF;
static cvar_t s_strict = { "hlua_strict", "0", 0, 0, 0 };
static bool sCommandsAdded = false;

void ReportError( void )
{
        const char *err = lua_tostring( gL, -1 );
        ALERT( at_error, "hlua: %s\n", err ? err : "unknown error" );
        lua_pop( gL, 1 );
}

static void Print( lua_State *L )
{
        luaL_Buffer b;
        int i, n = lua_gettop( L );
        luaL_buffinit( L, &b );
        for( i = 1; i <= n; i++ )
        {
                if( i > 1 )
                        luaL_addstring( &b, "\t" );
                luaL_tolstring( L, i, NULL );
                luaL_addvalue( &b );
        }
        luaL_pushresult( &b );
        ALERT( at_console, "%s\n", lua_tostring( L, -1 ) );
        lua_pop( L, 1 );
}

static int LuaPrint( lua_State *L ) { Print( L ); return 0; }

static int LuaCurTime( lua_State *L )
{
        lua_pushnumber( L, gpGlobals->time );
        return 1;
}

static int LuaFrameTime( lua_State *L )
{
        lua_pushnumber( L, gpGlobals->frametime );
        return 1;
}

static int LuaPrecacheModel( lua_State *L )
{
        const char *s = luaL_checkstring( L, 1 );
        lua_pushinteger( L, PRECACHE_MODEL( s ) );
        return 1;
}

static int LuaPrecacheSound( lua_State *L )
{
        const char *s = luaL_checkstring( L, 1 );
        lua_pushinteger( L, PRECACHE_SOUND( s ) );
        return 1;
}

// runs one file, reports errors, returns false on failure
static bool RunChunk( const char *path )
{
        FILE *f = fopen( path, "rb" );
        char *buf = NULL;
        long len;
        bool ok = false;

        if( !f )
                return true;
        fseek( f, 0, SEEK_END );
        len = ftell( f );
        fseek( f, 0, SEEK_SET );
        if( len < 0 )
                goto done;
        buf = (char *)malloc( (size_t)len );
        if( !buf || fread( buf, 1, (size_t)len, f ) != (size_t)len )
                goto done;
        if( luaL_loadbuffer( gL, buf, (size_t)len, path ) != LUA_OK )
        {
                ReportError();
                goto done;
        }
        if( lua_pcall( gL, 0, 0, 0 ) != LUA_OK )
        {
                ReportError();
                goto done;
        }
        ok = true;
done:
        if( f )
                fclose( f );
        free( buf );
        return ok;
}

static void LoadDir( const char *subdir )
{
        char dir[1024];
        std::vector<std::string> files;

        snprintf( dir, sizeof( dir ), "%s/%s", sGameDir, subdir );

#ifdef _WIN32
        {
                char pattern[1100];
                WIN32_FIND_DATAA fd;
                HANDLE h;
                snprintf( pattern, sizeof( pattern ), "%s\\*.lua", dir );
                h = FindFirstFileA( pattern, &fd );
                if( h != INVALID_HANDLE_VALUE )
                {
                        do
                        {
                                files.push_back( std::string( fd.cFileName ) );
                        } while( FindNextFileA( h, &fd ) );
                        FindClose( h );
                }
        }
#else
        {
                DIR *d = opendir( dir );
                if( d )
                {
                        struct dirent *e;
                        while( ( e = readdir( d ) ) )
                        {
                                size_t n = strlen( e->d_name );
                                if( n > 4 && !strcmp( e->d_name + n - 4, ".lua" ) )
                                        files.push_back( std::string( e->d_name ) );
                        }
                        closedir( d );
                }
        }
#endif

        std::sort( files.begin(), files.end() );
        for( size_t i = 0; i < files.size(); i++ )
        {
                char path[1536];
                snprintf( path, sizeof( path ), "%s/%s", dir, files[i].c_str() );
                if( !RunChunk( path ) && s_strict.value != 0 )
                        break;
        }
}

static void RegisterCore( lua_State *L )
{
        lua_pushcfunction( L, LuaPrint );
        lua_setglobal( L, "print" );

        lua_pushcfunction( L, LuaCurTime );
        lua_setglobal( L, "CurTime" );

        lua_pushcfunction( L, LuaFrameTime );
        lua_setglobal( L, "FrameTime" );

        lua_pushcfunction( L, LuaPrecacheModel );
        lua_setglobal( L, "PrecacheModel" );

        lua_pushcfunction( L, LuaPrecacheSound );
        lua_setglobal( L, "PrecacheSound" );

        RegisterHookLib( L );
        RegisterEntityLib( L );
}

static void CreateState( void )
{
        char path[1100];

        gL = luaL_newstate();
        if( !gL )
        {
                ALERT( at_error, "hlua: can't create lua state\n" );
                return;
        }
        luaL_openlibs( gL );

        // no binary module loading from scripts
        lua_getglobal( gL, "package" );
        lua_pushliteral( gL, "" );
        lua_setfield( gL, -2, "cpath" );
        snprintf( path, sizeof( path ), "%s/lua/?.lua;%s/lua/?/init.lua", sGameDir, sGameDir );
        lua_pushstring( gL, path );
        lua_setfield( gL, -2, "path" );
        lua_pop( gL, 1 );

        lua_newtable( gL );
        lua_setfield( gL, LUA_REGISTRYINDEX, "hlua_hooks" );

        RegisterCore( gL );

        lua_pushboolean( gL, 1 );
        lua_setglobal( gL, "SERVER" );
        lua_pushboolean( gL, 0 );
        lua_setglobal( gL, "CLIENT" );

        if( luaL_loadbuffer( gL, HLUA_PRELUDE, strlen( HLUA_PRELUDE ), "prelude" ) != LUA_OK
                || lua_pcall( gL, 0, 0, 0 ) != LUA_OK )
                ReportError();

        lua_getglobal( gL, "_hlua" );
        lua_getfield( gL, -1, "tick" );
        sTickRef = luaL_ref( gL, LUA_REGISTRYINDEX );
        lua_pop( gL, 1 );

        LoadDir( "lua/autorun" );
        LoadDir( "lua/shared" );
        LoadDir( "lua/entities" );
        LoadDir( "lua/server" );
}

static void CmdVersion( void )
{
        ALERT( at_console, "hlua %s, %s\n", HLUA_VERSION, LUA_RELEASE );
}

static void CmdRefresh( void )
{
        Refresh();
}

static void CmdRun( void )
{
        const char *code = CMD_ARGS();

        if( !gL )
                return;
        if( !code || !code[0] )
        {
                ALERT( at_console, "usage: hlua_run <code>\n" );
                return;
        }
        if( luaL_loadstring( gL, code ) != LUA_OK || lua_pcall( gL, 0, 1, 0 ) != LUA_OK )
        {
                ReportError();
                return;
        }
        if( !lua_isnil( gL, -1 ) )
                Print( gL );
        lua_pop( gL, 1 );
}

void OnGameInit( void )
{
        CVAR_REGISTER( &s_strict );

        if( !sCommandsAdded )
        {
                g_engfuncs.pfnAddServerCommand( "hlua_version", CmdVersion );
                g_engfuncs.pfnAddServerCommand( "hlua_refresh", CmdRefresh );
                g_engfuncs.pfnAddServerCommand( "hlua_run", CmdRun );
                sCommandsAdded = true;
        }

        if( gL )
                return;

        g_engfuncs.pfnGetGameDir( sGameDir );
        CreateState();

        if( gL )
                ALERT( at_console, "hlua %s ready, gamedir %s\n", HLUA_VERSION, sGameDir );
}

void Shutdown( void )
{
        if( !gL )
                return;
        ClearEntityTables();
        lua_close( gL );
        gL = NULL;
        sTickRef = LUA_NOREF;
}

void Refresh( void )
{
        if( !gL )
                return;
        // entities stay alive, their tables and hooks do not
        ClearEntityTables();
        lua_close( gL );
        gL = NULL;
        CreateState();
        if( gL )
                ALERT( at_console, "hlua refreshed\n" );
}

void Frame( void )
{
        if( !gL )
                return;

        if( sTickRef != LUA_NOREF )
        {
                lua_rawgeti( gL, LUA_REGISTRYINDEX, sTickRef );
                lua_pushnumber( gL, gpGlobals->time );
                if( lua_pcall( gL, 1, 0, 0 ) != LUA_OK )
                        ReportError();
        }

        lua_gc( gL, LUA_GCSTEP, 0 );

        FireHook0( "StartFrame" );
}

void OnServerActivate( void )
{
        FireHook0( "InitPostEntity" );
}

void OnServerDeactivate( void )
{
        // engines that do not call pfnOnFreeEntPrivateData for every entity
        // would leak table refs across map changes otherwise
        ClearEntityTables();
        FireHook0( "ServerDeactivate" );
}

} // namespace hlua
