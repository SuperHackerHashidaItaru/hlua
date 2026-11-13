#include <string.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "hlua_internal.h"

extern int DispatchSpawn( edict_t *pent );

namespace hlua
{

// entity values are edict indexes so refs survive entity reuse of the
// same slot only for as long as the slot stays alive

struct EntUD
{
        int index;
};

static edict_t *ValidEntity( lua_State *L, int arg )
{
        EntUD *ud = (EntUD *)luaL_checkudata( L, arg, "hlua_ent" );
        edict_t *pent = NULL;

        if( ud->index >= 0 && ud->index < gpGlobals->maxEntities )
                pent = INDEXENT( ud->index );
        if( !pent || pent->free || !pent->pvPrivateData )
                luaL_error( L, "entity is not valid" );
        return pent;
}

void PushEntity( lua_State *L, edict_t *pent )
{
        EntUD *ud = (EntUD *)lua_newuserdata( L, sizeof( EntUD ) );
        ud->index = pent ? ENTINDEX( pent ) : -1;
        luaL_getmetatable( L, "hlua_ent" );
        lua_setmetatable( L, -2 );
}

static void PushVector( lua_State *L, const Vector &v )
{
        lua_createtable( L, 0, 3 );
        lua_pushnumber( L, v.x );
        lua_setfield( L, -2, "x" );
        lua_pushnumber( L, v.y );
        lua_setfield( L, -2, "y" );
        lua_pushnumber( L, v.z );
        lua_setfield( L, -2, "z" );
}

static Vector ToVector( lua_State *L, int idx )
{
        Vector v( 0, 0, 0 );

        luaL_checktype( L, idx, LUA_TTABLE );
        lua_getfield( L, idx, "x" );
        if( lua_isnumber( L, -1 ) )
                v.x = (float)lua_tonumber( L, -1 );
        lua_pop( L, 1 );
        lua_getfield( L, idx, "y" );
        if( lua_isnumber( L, -1 ) )
                v.y = (float)lua_tonumber( L, -1 );
        lua_pop( L, 1 );
        lua_getfield( L, idx, "z" );
        if( lua_isnumber( L, -1 ) )
                v.z = (float)lua_tonumber( L, -1 );
        lua_pop( L, 1 );
        return v;
}

// ---- per entity script table, ref stored on the C++ object

void PushEntityTable( lua_State *L, CBaseEntity *ent )
{
        if( ent->hluaTable > 0 )
        {
                lua_rawgeti( L, LUA_REGISTRYINDEX, ent->hluaTable );
                return;
        }
        lua_createtable( L, 0, 0 );
        ent->hluaTable = luaL_ref( L, LUA_REGISTRYINDEX );
        lua_rawgeti( L, LUA_REGISTRYINDEX, ent->hluaTable );
}

void UnrefEntityTable( CBaseEntity *ent )
{
        if( !gL || ent->hluaTable <= 0 )
                return;
        luaL_unref( gL, LUA_REGISTRYINDEX, ent->hluaTable );
        ent->hluaTable = 0;
}

void ClearEntityTables( void )
{
        int i;

        if( !gL )
                return;
        for( i = 0; i < gpGlobals->maxEntities; i++ )
        {
                edict_t *pent = INDEXENT( i );
                if( pent && pent->pvPrivateData )
                        UnrefEntityTable( (CBaseEntity *)pent->pvPrivateData );
        }
}

// ---- entity fields

enum
{
        FLD_CLASSNAME, FLD_TARGETNAME, FLD_NETNAME, FLD_MODEL,
        FLD_ORIGIN, FLD_ANGLES, FLD_VELOCITY,
        FLD_HEALTH, FLD_SOLID, FLD_MOVETYPE, FLD_EFFECTS, FLD_NEXTTHINK,
        FLD_FLAGS, FLD_SPAWNFLAGS,
        FLD_COUNT
};

static const struct
{
        const char *name;
        int type; // 's' string ro, 'v' vector rw, 'n' number rw, 'r' number ro
}
sFields[FLD_COUNT] =
{
        { "classname", 's' },
        { "targetname", 's' },
        { "netname", 's' },
        { "model", 's' },
        { "origin", 'v' },
        { "angles", 'v' },
        { "velocity", 'v' },
        { "health", 'n' },
        { "solid", 'n' },
        { "movetype", 'n' },
        { "effects", 'n' },
        { "nextthink", 'n' },
        { "flags", 'r' },
        { "spawnflags", 'r' },
};

static int FindField( const char *key )
{
        int i;
        for( i = 0; i < FLD_COUNT; i++ )
        {
                if( !strcmp( sFields[i].name, key ) )
                        return i;
        }
        return -1;
}

static int EntIndex( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        const char *key = luaL_checkstring( L, 2 );
        entvars_t *pev = &pent->v;
        int f = FindField( key );

        // methods first
        lua_getmetatable( L, 1 );
        lua_getfield( L, -1, "__methods" );
        lua_pushvalue( L, 2 );
        lua_rawget( L, -2 );
        if( !lua_isnil( L, -1 ) )
                return 1;
        lua_pop( L, 3 );

        if( f < 0 )
        {
                lua_pushnil( L );
                return 1;
        }

        switch( f )
        {
        case FLD_CLASSNAME: lua_pushstring( L, STRING( pev->classname ) ); break;
        case FLD_TARGETNAME: lua_pushstring( L, STRING( pev->targetname ) ); break;
        case FLD_NETNAME: lua_pushstring( L, STRING( pev->netname ) ); break;
        case FLD_MODEL: lua_pushstring( L, STRING( pev->model ) ); break;
        case FLD_ORIGIN: PushVector( L, pev->origin ); break;
        case FLD_ANGLES: PushVector( L, pev->angles ); break;
        case FLD_VELOCITY: PushVector( L, pev->velocity ); break;
        case FLD_HEALTH: lua_pushnumber( L, pev->health ); break;
        case FLD_SOLID: lua_pushinteger( L, pev->solid ); break;
        case FLD_MOVETYPE: lua_pushinteger( L, pev->movetype ); break;
        case FLD_EFFECTS: lua_pushinteger( L, pev->effects ); break;
        case FLD_NEXTTHINK: lua_pushnumber( L, pev->nextthink ); break;
        case FLD_FLAGS: lua_pushinteger( L, pev->flags ); break;
        case FLD_SPAWNFLAGS: lua_pushinteger( L, pev->spawnflags ); break;
        default: lua_pushnil( L ); break;
        }
        return 1;
}

static int EntNewIndex( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        const char *key = luaL_checkstring( L, 2 );
        entvars_t *pev = &pent->v;
        int f = FindField( key );

        if( f < 0 )
                luaL_error( L, "entity has no settable field '%s'", key );

        switch( f )
        {
        case FLD_ORIGIN:
                pev->origin = ToVector( L, 3 );
                SET_ORIGIN( pent, pev->origin );
                return 0;
        case FLD_ANGLES:
                pev->angles = ToVector( L, 3 );
                return 0;
        case FLD_VELOCITY:
                pev->velocity = ToVector( L, 3 );
                return 0;
        case FLD_HEALTH:
                pev->health = (float)luaL_checknumber( L, 3 );
                return 0;
        case FLD_SOLID:
                pev->solid = (int)luaL_checkinteger( L, 3 );
                return 0;
        case FLD_MOVETYPE:
                pev->movetype = (int)luaL_checkinteger( L, 3 );
                return 0;
        case FLD_EFFECTS:
                pev->effects = (int)luaL_checkinteger( L, 3 );
                return 0;
        case FLD_NEXTTHINK:
                pev->nextthink = (float)luaL_checknumber( L, 3 );
                return 0;
        default:
                luaL_error( L, "entity field '%s' is read only", key );
                return 0;
        }
}

// ---- entity methods

static int EntGetClass( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        CBaseEntity *ent = (CBaseEntity *)pent->pvPrivateData;
        const char *name = ent->ScriptClass();

        if( !name || !name[0] )
                name = STRING( pent->v.classname );
        lua_pushstring( L, name );
        return 1;
}

static int EntName( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        lua_pushstring( L, STRING( pent->v.netname ) );
        return 1;
}

static int EntGetPos( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        PushVector( L, pent->v.origin );
        return 1;
}

static int EntSetPos( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        pent->v.origin = ToVector( L, 2 );
        SET_ORIGIN( pent, pent->v.origin );
        return 0;
}

static int EntGetAngles( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        PushVector( L, pent->v.angles );
        return 1;
}

static int EntSetAngles( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        pent->v.angles = ToVector( L, 2 );
        return 0;
}

static int EntGetModel( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        lua_pushstring( L, STRING( pent->v.model ) );
        return 1;
}

static int EntSetModel( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        const char *s = luaL_checkstring( L, 2 );
        PRECACHE_MODEL( s );
        SET_MODEL( pent, s );
        return 0;
}

static int EntGetTable( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        PushEntityTable( L, (CBaseEntity *)pent->pvPrivateData );
        return 1;
}

static int EntEmitSound( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        const char *name = luaL_checkstring( L, 2 );
        double vol = luaL_optnumber( L, 3, 1.0 );
        int pitch = (int)luaL_optinteger( L, 4, 100 );

        g_engfuncs.pfnEmitSound( pent, CHAN_VOICE, name, (float)vol, ATTN_NORM, 0, pitch );
        return 0;
}

static int EntNextThink( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        double delay = luaL_checknumber( L, 2 );

        pent->v.nextthink = gpGlobals->time + (float)delay;
        return 0;
}

static int EntSetSize( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );
        Vector mins = ToVector( L, 2 );
        Vector maxs = ToVector( L, 3 );

        SET_SIZE( pent, mins, maxs );
        return 0;
}

static int EntRemove( lua_State *L )
{
        edict_t *pent = ValidEntity( L, 1 );

        UTIL_Remove( (CBaseEntity *)pent->pvPrivateData );
        return 0;
}

static int EntToString( lua_State *L )
{
        EntUD *ud = (EntUD *)luaL_checkudata( L, 1, "hlua_ent" );
        lua_pushfstring( L, "entity (%d)", ud->index );
        return 1;
}

static int EntEq( lua_State *L )
{
        EntUD *a = (EntUD *)luaL_testudata( L, 1, "hlua_ent" );
        EntUD *b = (EntUD *)luaL_testudata( L, 2, "hlua_ent" );

        lua_pushboolean( L, ( a && b && a->index == b->index ) ? 1 : 0 );
        return 1;
}

static int LuaIsValid( lua_State *L )
{
        EntUD *ud = (EntUD *)luaL_testudata( L, 1, "hlua_ent" );
        edict_t *pent = NULL;

        if( ud && ud->index >= 0 && ud->index < gpGlobals->maxEntities )
                pent = INDEXENT( ud->index );
        lua_pushboolean( L, ( pent && !pent->free && pent->pvPrivateData ) ? 1 : 0 );
        return 1;
}

// ---- scripted entity carrier

static bool PushClassMethod( lua_State *L, CBaseEntity *ent, const char *method )
{
        const char *cls = ent->ScriptClass();

        if( !L || !cls || !cls[0] )
                return false;
        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_classes" );
        if( !lua_istable( L, -1 ) )
        {
                lua_pop( L, 1 );
                return false;
        }
        lua_getfield( L, -1, cls );
        lua_remove( L, -2 );
        if( !lua_istable( L, -1 ) )
        {
                lua_pop( L, 1 );
                return false;
        }
        lua_getfield( L, -1, method );
        lua_remove( L, -2 );
        if( !lua_isfunction( L, -1 ) )
        {
                lua_pop( L, 1 );
                return false;
        }
        return true;
}

static void CallMethod0( CBaseEntity *ent, const char *method )
{
        if( !PushClassMethod( gL, ent, method ) )
                return;
        PushEntity( gL, ENT( ent->pev ) );
        if( lua_pcall( gL, 1, 0, 0 ) != LUA_OK )
                ReportError();
}

static void CallMethodE( CBaseEntity *ent, const char *method, CBaseEntity *other )
{
        if( !PushClassMethod( gL, ent, method ) )
                return;
        PushEntity( gL, ENT( ent->pev ) );
        PushEntity( gL, other ? ENT( other->pev ) : NULL );
        if( lua_pcall( gL, 2, 0, 0 ) != LUA_OK )
                ReportError();
}

class CHLuaEnt : public CBaseEntity
{
public:
        string_t m_iszScriptClass;

        virtual const char *ScriptClass( void ) { return STRING( m_iszScriptClass ); }

        virtual void Spawn( void )
        {
                if( pev->model )
                        SET_MODEL( ENT( pev ), STRING( pev->model ) );
                CallMethod0( this, "OnSpawn" );
        }

        virtual void Think( void )
        {
                // the base call runs SetThink targets like SUB_Remove, keep it
                CallMethod0( this, "OnThink" );
                CBaseEntity::Think();
        }
        virtual void Touch( CBaseEntity *pOther )
        {
                CallMethodE( this, "OnTouch", pOther );
                CBaseEntity::Touch( pOther );
        }
        virtual void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value )
        {
                CallMethodE( this, "OnUse", pActivator );
                CBaseEntity::Use( pActivator, pCaller, useType, value );
        }
        virtual void Blocked( CBaseEntity *pOther )
        {
                CallMethodE( this, "OnBlocked", pOther );
                CBaseEntity::Blocked( pOther );
        }

        virtual void KeyValue( KeyValueData *pkvd )
        {
                if( !strcmp( pkvd->szKeyName, "lua_class" ) )
                {
                        m_iszScriptClass = ALLOC_STRING( pkvd->szValue );
                        pkvd->fHandled = TRUE;
                        return;
                }
                if( gL )
                {
                        // keep map keyvalues for scripts on self.kv
                        PushEntityTable( gL, this );
                        lua_getfield( gL, -1, "kv" );
                        if( !lua_istable( gL, -1 ) )
                        {
                                lua_pop( gL, 1 );
                                lua_createtable( gL, 0, 4 );
                                lua_pushvalue( gL, -1 );
                                lua_setfield( gL, -3, "kv" );
                        }
                        lua_pushstring( gL, pkvd->szValue );
                        lua_setfield( gL, -2, pkvd->szKeyName );
                        lua_pop( gL, 2 );
                }
                pkvd->fHandled = TRUE;
        }
};

LINK_ENTITY_TO_CLASS( lua_ent, CHLuaEnt )

bool TrySpawnScripted( const char *mapClassName, entvars_t *pev )
{
        if( !gL )
                return false;
        lua_getfield( gL, LUA_REGISTRYINDEX, "hlua_classes" );
        if( !lua_istable( gL, -1 ) )
        {
                lua_pop( gL, 1 );
                return false;
        }
        lua_getfield( gL, -1, mapClassName );
        bool reg = !lua_isnil( gL, -1 );
        lua_pop( gL, 2 );
        if( !reg )
                return false;

        CHLuaEnt *ent = GetClassPtr( (CHLuaEnt *)pev );
        ent->m_iszScriptClass = ALLOC_STRING( mapClassName );
        return true;
}

// ---- ents library

static int EntsCreate( lua_State *L )
{
        const char *cls = luaL_checkstring( L, 1 );
        edict_t *pent;
        bool reg;

        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_classes" );
        if( lua_istable( L, -1 ) )
                lua_getfield( L, -1, cls );
        else
                lua_pushnil( L );
        reg = !lua_isnil( L, -1 );
        lua_pop( L, 2 );
        if( !reg )
        {
                // unregistered class, refuse rather than spawn an inert entity
                lua_pushnil( L );
                return 1;
        }

        pent = CREATE_NAMED_ENTITY( MAKE_STRING( "lua_ent" ) );
        if( FNullEnt( pent ) )
        {
                lua_pushnil( L );
                return 1;
        }
        ( (CHLuaEnt *)pent->pvPrivateData )->m_iszScriptClass = ALLOC_STRING( cls );
        if( lua_istable( L, 2 ) )
                pent->v.origin = ToVector( L, 2 );
        if( lua_istable( L, 3 ) )
                pent->v.angles = ToVector( L, 3 );
        DispatchSpawn( pent );
        PushEntity( L, pent );
        return 1;
}

static int EntsFindByClass( lua_State *L )
{
        const char *cls = luaL_checkstring( L, 1 );
        int i, count = 0;

        lua_createtable( L, 8, 0 );
        for( i = 0; i < gpGlobals->maxEntities; i++ )
        {
                edict_t *pent = INDEXENT( i );
                CBaseEntity *ent;
                const char *name;

                if( !pent || pent->free || !pent->pvPrivateData )
                        continue;
                ent = (CBaseEntity *)pent->pvPrivateData;
                name = ent->ScriptClass();
                if( !name || !name[0] )
                        name = STRING( pent->v.classname );
                if( !strcmp( name, cls ) )
                {
                        PushEntity( L, pent );
                        lua_rawseti( L, -2, ++count );
                }
        }
        return 1;
}

void OnFreePrivateData( edict_t *pent )
{
        CBaseEntity *ent = (CBaseEntity *)GET_PRIVATE( pent );

        if( !ent )
                return;
        CallMethod0( ent, "OnRemove" );
        FireHookE( "EntityRemoved", pent );
        UnrefEntityTable( ent );
}

void RegisterEntityLib( lua_State *L )
{
        luaL_newmetatable( L, "hlua_ent" );

        lua_createtable( L, 0, 16 );
        lua_pushcfunction( L, EntGetClass ); lua_setfield( L, -2, "GetClass" );
        lua_pushcfunction( L, EntName ); lua_setfield( L, -2, "Name" );
        lua_pushcfunction( L, EntGetPos ); lua_setfield( L, -2, "GetPos" );
        lua_pushcfunction( L, EntSetPos ); lua_setfield( L, -2, "SetPos" );
        lua_pushcfunction( L, EntGetAngles ); lua_setfield( L, -2, "GetAngles" );
        lua_pushcfunction( L, EntSetAngles ); lua_setfield( L, -2, "SetAngles" );
        lua_pushcfunction( L, EntGetModel ); lua_setfield( L, -2, "GetModel" );
        lua_pushcfunction( L, EntSetModel ); lua_setfield( L, -2, "SetModel" );
        lua_pushcfunction( L, EntGetTable ); lua_setfield( L, -2, "GetTable" );
        lua_pushcfunction( L, EntEmitSound ); lua_setfield( L, -2, "EmitSound" );
        lua_pushcfunction( L, EntNextThink ); lua_setfield( L, -2, "NextThink" );
        lua_pushcfunction( L, EntSetSize ); lua_setfield( L, -2, "SetSize" );
        lua_pushcfunction( L, EntRemove ); lua_setfield( L, -2, "Remove" );
        lua_setfield( L, -2, "__methods" );

        lua_pushcfunction( L, EntIndex );
        lua_setfield( L, -2, "__index" );
        lua_pushcfunction( L, EntNewIndex );
        lua_setfield( L, -2, "__newindex" );
        lua_pushcfunction( L, EntToString );
        lua_setfield( L, -2, "__tostring" );
        lua_pushcfunction( L, EntEq );
        lua_setfield( L, -2, "__eq" );
        lua_pop( L, 1 );

        // script class registry, read by the spawn guard and ents.Create
        lua_newtable( L );
        lua_setfield( L, LUA_REGISTRYINDEX, "hlua_classes" );
        lua_createtable( L, 0, 2 );
        lua_getfield( L, LUA_REGISTRYINDEX, "hlua_classes" );
        lua_setfield( L, -2, "classes" );
        lua_setglobal( L, "_hlua" );

        lua_createtable( L, 0, 2 );
        lua_pushcfunction( L, EntsCreate );
        lua_setfield( L, -2, "Create" );
        lua_pushcfunction( L, EntsFindByClass );
        lua_setfield( L, -2, "FindByClass" );
        lua_setglobal( L, "ents" );

        lua_pushcfunction( L, LuaIsValid );
        lua_setglobal( L, "IsValid" );
}

} // namespace hlua
