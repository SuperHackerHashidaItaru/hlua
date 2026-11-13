// headless hlua test: dlopens the built server library and drives it
// through a fake engine, the same way a real engine would

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdint.h>
#include <limits.h>

#include "extdll.h"
#include "util.h"

#define MAX_EDICTS 64
#define POOL_SIZE 65536

typedef void (*entity_fn)( entvars_t *pev );

// defined here, matching the extern declarations the sdk headers make
enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;
static globalvars_t g_globals;
static DLL_FUNCTIONS g_funcs;
static NEW_DLL_FUNCTIONS *g_newFuncs;
static void *g_libHandle;
static edict_t g_edicts[MAX_EDICTS];
static char g_strpool[POOL_SIZE];
static int g_strpoolSize = 1;
static char g_gameDir[PATH_MAX];
static char g_lastServerPrint[256];
static int g_hluaErrors;
static int g_sawTwo;
static double g_time;

struct servercmd_t { char name[64]; void (*fn)( void ); };
static servercmd_t g_cmds[32];
static int g_cmdCount;

static const char *g_cmdArgv[8];
static int g_cmdArgc;

static void FakeAlert( ALERT_TYPE type, const char *fmt, ... )
{
        char buf[1024];
        va_list ap;
        va_start( ap, fmt );
        vsnprintf( buf, sizeof( buf ), fmt, ap );
        va_end( ap );
        printf( "%s", buf );
        if( type == at_error && strstr( buf, "hlua:" ) )
                g_hluaErrors++;
        if( strstr( buf, "HLUATEST FAIL" ) )
                g_hluaErrors++;
        if( !strcmp( buf, "2\n" ) )
                g_sawTwo = 1;
}

static void FakeGetGameDir( char *out )
{
        strcpy( out, g_gameDir );
}

static void FakeAddServerCommand( const char *name, void (*fn)( void ) )
{
        if( g_cmdCount < 32 )
        {
                snprintf( g_cmds[g_cmdCount].name, sizeof( g_cmds[0].name ), "%s", name );
                g_cmds[g_cmdCount].fn = fn;
                g_cmdCount++;
        }
}

static int FakeIndexOfEdict( const edict_t *e )
{
        return e ? (int)( e - g_edicts ) : -1;
}

static edict_t *FakePEntityOfEntIndex( int i )
{
        if( i < 0 || i >= MAX_EDICTS )
                return NULL;
        return &g_edicts[i];
}

static void *FakeAllocPrivate( edict_t *e, int size )
{
        // engines hand out zeroed memory, the sdk relies on that
        e->pvPrivateData = calloc( 1, size ? size : 1 );
        return e->pvPrivateData;
}

static void FakeFreePrivate( edict_t *e )
{
        free( e->pvPrivateData );
        e->pvPrivateData = NULL;
}

static const char *PoolStr( int off )
{
        if( off <= 0 || off >= g_strpoolSize )
                return "";
        return g_strpool + off;
}

static int FakeAllocString( const char *s )
{
        int off = g_strpoolSize;
        int len = strlen( s ) + 1;
        if( g_strpoolSize + len >= POOL_SIZE )
                return 0;
        memcpy( g_strpool + g_strpoolSize, s, len );
        g_strpoolSize += len;
        return off;
}

static edict_t *FakeCreateNamedEntity( int className )
{
        entity_fn fn;
        edict_t *e;
        int i;

        // what the engine does: find a slot, set the class, resolve the
        // spawn function from the library exports, call it
        for( i = 1; i < MAX_EDICTS; i++ )
        {
                if( g_edicts[i].free )
                        break;
                if( g_edicts[i].pvPrivateData || g_edicts[i].v.classname )
                        continue;
                break;
        }
        if( i >= MAX_EDICTS )
                return NULL;
        e = &g_edicts[i];
        e->free = 0;
        e->v.classname = className;
        e->v.pContainingEntity = e;

        fn = (entity_fn)dlsym( g_libHandle, PoolStr( className ) );
        if( !fn )
        {
                printf( "TEST: no spawn function for '%s'\n", PoolStr( className ) );
                e->v.classname = 0;
                return NULL;
        }
        fn( &e->v );
        return e;
}

static void FakeRemoveEntity( edict_t *e )
{
        if( !e || e->free )
                return;
        // engines call the dll hook right before freeing private data
        if( g_newFuncs && g_newFuncs->pfnOnFreeEntPrivateData )
                g_newFuncs->pfnOnFreeEntPrivateData( e );
        FakeFreePrivate( e );
        e->v.classname = 0;
        e->free = 1;
}

static void FakeServerPrint( const char *s )
{
        snprintf( g_lastServerPrint, sizeof( g_lastServerPrint ), "%s", s );
        printf( "SERVER: %s", s );
}

static int FakeCmdArgc( void )
{
        return g_cmdArgc;
}

static const char *FakeCmdArgv( int i )
{
        return ( i >= 0 && i < g_cmdArgc ) ? g_cmdArgv[i] : "";
}

static const char *FakeCmdArgs( void )
{
        static char buf[256];
        int i;
        buf[0] = 0;
        for( i = 1; i < g_cmdArgc; i++ )
        {
                if( i > 1 )
                        strncat( buf, " ", sizeof( buf ) - strlen( buf ) - 1 );
                strncat( buf, g_cmdArgv[i], sizeof( buf ) - strlen( buf ) - 1 );
        }
        return buf;
}

static edict_t *FakeFindEntityByString( edict_t *start, const char *field, const char *value )
{
        return NULL;
}

static const char *FakeGetPlayerAuthId( edict_t *e )
{
        return "";
}

static int FakeGetPlayerUserId( edict_t *e )
{
        return -1;
}

static char *FakeGetInfoKeyBuffer( edict_t *e )
{
        static char empty[1] = "";
        return empty;
}

static char *FakeInfoKeyValue( char *infobuffer, const char *key )
{
        return (char *)"";
}

static void Unimplemented( void )
{
        printf( "TEST: unimplemented engine function called\n" );
}

static void SetCmd( const char *a, const char *b, const char *c )
{
        g_cmdArgv[0] = a;
        g_cmdArgv[1] = b;
        g_cmdArgv[2] = c;
        g_cmdArgc = ( a == NULL ) ? 0 : ( ( b == NULL ) ? 1 : ( ( c == NULL ) ? 2 : 3 ) );
}

static edict_t *SpawnMapEntity( const char *classname, const char *kv[][2], int nkv )
{
        KeyValueData kvd;
        edict_t *e = FakeCreateNamedEntity( FakeAllocString( classname ) );
        int i;

        if( !e )
                return NULL;
        for( i = 0; i < nkv; i++ )
        {
                memset( &kvd, 0, sizeof( kvd ) );
                kvd.szClassName = classname;
                kvd.szKeyName = kv[i][0];
                kvd.szValue = kv[i][1];
                g_funcs.pfnKeyValue( e, &kvd );
        }
        return e;
}

static void RunFrames( DLL_FUNCTIONS *funcs, double from, double to )
{
        double t;
        size_t i;
        edict_t *e;

        for( t = from; t <= to; t += 0.1 )
        {
                g_time = t;
                g_globals.time = (float)t;
                g_globals.frametime = 0.1f;
                funcs->pfnStartFrame();
                for( i = 0; i < MAX_EDICTS; i++ )
                {
                        e = &g_edicts[i];
                        if( e->free || !e->pvPrivateData )
                                continue;
                        if( e->v.nextthink > 0 && e->v.nextthink <= t )
                        {
                                e->v.nextthink = 0;
                                funcs->pfnThink( e );
                        }
                }
                // engines free FL_KILLME entities at the end of the frame
                for( i = 0; i < MAX_EDICTS; i++ )
                {
                        e = &g_edicts[i];
                        if( e->free || !e->pvPrivateData )
                                continue;
                        if( e->v.flags & FL_KILLME )
                                FakeRemoveEntity( e );
                }
        }
}

static void RunCommand( const char *name )
{
        int i;
        for( i = 0; i < g_cmdCount; i++ )
        {
                if( !strcmp( g_cmds[i].name, name ) )
                        g_cmds[i].fn();
        }
}

int main( int argc, char **argv )
{
        void ( *giveFnptrs )( enginefuncs_t *, globalvars_t * );
        int ( *getAPI )( DLL_FUNCTIONS *, int );
        int ( *getNewAPI )( NEW_DLL_FUNCTIONS *, int * );
        NEW_DLL_FUNCTIONS newfuncs;
        edict_t *breakable, *droneMap, *player;
        int newver = NEW_DLL_FUNCTIONS_VERSION;
        char reject[128];
        const char *breakableKv[1][2] = { { "reward", "50" } };
        const char *droneKv[1][2] = { { "lua_class", "npc_drone" } };
        uintptr_t stub = (uintptr_t)&Unimplemented;
        uintptr_t *slots;
        size_t i, nslots;

        if( argc < 3 )
        {
                printf( "usage: hlua_test <server library> <gamedir>\n" );
                return 2;
        }
        setvbuf( stdout, NULL, _IONBF, 0 );

        g_libHandle = dlopen( argv[1], RTLD_NOW | RTLD_LOCAL );
        if( !g_libHandle )
        {
                printf( "TEST: dlopen failed: %s\n", dlerror() );
                return 1;
        }
        if( !realpath( argv[2], g_gameDir ) )
        {
                printf( "TEST: bad gamedir\n" );
                return 1;
        }
        g_strpool[0] = 0;

        giveFnptrs = ( void ( * )( enginefuncs_t *, globalvars_t * ) )dlsym( g_libHandle, "GiveFnptrsToDll" );
        getAPI = ( int ( * )( DLL_FUNCTIONS *, int ) )dlsym( g_libHandle, "GetEntityAPI" );
        getNewAPI = ( int ( * )( NEW_DLL_FUNCTIONS *, int * ) )dlsym( g_libHandle, "GetNewDLLFunctions" );
        if( !giveFnptrs || !getAPI || !getNewAPI )
        {
                printf( "TEST: missing library exports\n" );
                return 1;
        }

        // every callback starts as a loud stub, real ones follow
        memset( &g_engfuncs, 0, sizeof( g_engfuncs ) );
        nslots = sizeof( g_engfuncs ) / sizeof( uintptr_t );
        slots = (uintptr_t *)&g_engfuncs;
        for( i = 0; i < nslots; i++ )
                slots[i] = stub;

        g_engfuncs.pfnAlertMessage = FakeAlert;
        g_engfuncs.pfnGetGameDir = FakeGetGameDir;
        g_engfuncs.pfnAddServerCommand = FakeAddServerCommand;
        g_engfuncs.pfnIndexOfEdict = FakeIndexOfEdict;
        g_engfuncs.pfnPEntityOfEntIndex = FakePEntityOfEntIndex;
        g_engfuncs.pfnPvAllocEntPrivateData = FakeAllocPrivate;
        g_engfuncs.pfnFreeEntPrivateData = FakeFreePrivate;
        g_engfuncs.pfnAllocString = FakeAllocString;
        g_engfuncs.pfnCreateNamedEntity = FakeCreateNamedEntity;
        g_engfuncs.pfnRemoveEntity = FakeRemoveEntity;
        g_engfuncs.pfnServerPrint = FakeServerPrint;
        g_engfuncs.pfnCmd_Argc = FakeCmdArgc;
        g_engfuncs.pfnCmd_Argv = FakeCmdArgv;
        g_engfuncs.pfnCmd_Args = FakeCmdArgs;
        g_engfuncs.pfnFindEntityByString = FakeFindEntityByString;
        g_engfuncs.pfnGetPlayerAuthId = FakeGetPlayerAuthId;
        g_engfuncs.pfnGetPlayerUserId = FakeGetPlayerUserId;
        g_engfuncs.pfnGetInfoKeyBuffer = FakeGetInfoKeyBuffer;
        g_engfuncs.pfnInfoKeyValue = FakeInfoKeyValue;

        memset( &g_globals, 0, sizeof( g_globals ) );
        g_globals.maxEntities = MAX_EDICTS;
        g_globals.pStringBase = g_strpool;

        giveFnptrs( &g_engfuncs, &g_globals );
        

        if( !getAPI( &g_funcs, INTERFACE_VERSION ) )
        {
                printf( "TEST: GetEntityAPI failed\n" );
                return 1;
        }
        if( !getNewAPI( &newfuncs, &newver ) )
        {
                printf( "TEST: GetNewDLLFunctions failed\n" );
                return 1;
        }
        g_newFuncs = &newfuncs;

        g_funcs.pfnGameInit();

        // map style entities, in engine order: create, keyvalues, spawn
        breakable = SpawnMapEntity( "func_breakable", breakableKv, 1 );
        if( !breakable )
        {
                printf( "TEST: func_breakable spawn failed\n" );
                return 1;
        }
        droneMap = SpawnMapEntity( "lua_ent", droneKv, 1 );
        if( !droneMap )
        {
                printf( "TEST: lua_ent spawn failed\n" );
                return 1;
        }
        g_funcs.pfnSpawn( breakable );
        g_funcs.pfnSpawn( droneMap );

        player = FakeCreateNamedEntity( FakeAllocString( "player" ) );
        if( !player )
        {
                printf( "TEST: player spawn failed\n" );
                return 1;
        }

        g_funcs.pfnServerActivate( g_edicts, MAX_EDICTS, 0 );

        RunFrames( &g_funcs, 0.1, 2.6 );

        // connect reject
        if( g_funcs.pfnClientConnect( player, "tester", "127.0.0.1", reject ) )
        {
                printf( "TEST: connect should have been rejected\n" );
                return 1;
        }
        if( strcmp( reject, "no testers allowed" ) )
        {
                printf( "TEST: bad reject reason: %s\n", reject );
                return 1;
        }

        // console style commands
        SetCmd( "makemecoffee", NULL, NULL );
        g_funcs.pfnClientCommand( player );
        SetCmd( "closemenus", NULL, NULL );
        g_funcs.pfnClientCommand( player );

        // say hook: replace, block, default
        g_time += 2;
        g_globals.time = (float)g_time;
        SetCmd( "say", "hello", "world" );
        g_funcs.pfnClientCommand( player );
        if( !strstr( g_lastServerPrint, "filtered" ) )
        {
                printf( "TEST: say replace failed\n" );
                return 1;
        }
        g_time += 2;
        g_globals.time = (float)g_time;
        SetCmd( "say", "blockme", NULL );
        g_funcs.pfnClientCommand( player );
        if( strstr( g_lastServerPrint, "blockme" ) )
        {
                printf( "TEST: say block failed\n" );
                return 1;
        }
        g_time += 2;
        g_globals.time = (float)g_time;
        SetCmd( "say", "unhooked", NULL );
        g_funcs.pfnClientCommand( player );
        if( !strstr( g_lastServerPrint, "unhooked" ) )
        {
                printf( "TEST: say default failed\n" );
                return 1;
        }

        SetCmd( "hlua_run", "return 1+1", NULL );
        RunCommand( "hlua_run" );
        if( !g_sawTwo )
        {
                printf( "TEST: hlua_run did not print\n" );
                return 1;
        }

        // refresh, then prove entities rebind to the new state
        RunCommand( "hlua_refresh" );
        g_funcs.pfnServerActivate( g_edicts, MAX_EDICTS, 0 );
        if( !breakable->free )
                g_funcs.pfnThink( breakable );

        RunFrames( &g_funcs, g_time + 0.1, g_time + 2.6 );

        if( g_hluaErrors )
        {
                printf( "TEST: %d hlua errors\n", g_hluaErrors );
                return 1;
        }
        printf( "TEST: all checks passed\n" );
        return 0;
}
