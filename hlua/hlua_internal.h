#pragma once
#if !defined(HLUA_INTERNAL_H)
#define HLUA_INTERNAL_H

// shared between hlua translation units only, never included by sdk code

#define HLUA_VERSION "0.1.0"

struct lua_State;
struct edict_s;
typedef struct edict_s edict_t;
class CBaseEntity;

namespace hlua
{
        extern lua_State *gL;

        void ReportError( void ); // pops and logs a lua error from the stack top

        void RegisterHookLib( lua_State *L );
        void PushEntity( lua_State *L, edict_t *pent );
        void PushEntityTable( lua_State *L, CBaseEntity *ent );
        void UnrefEntityTable( CBaseEntity *ent );
        void ClearEntityTables( void );
        void RegisterEntityLib( lua_State *L );

        bool FireHook0( const char *name );
        void FireHookE( const char *name, edict_t *ent );
}
#endif
