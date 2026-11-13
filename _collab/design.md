# HLua design

Living document: what is built, what is decided, what is open. The
original reviewed proposal is proposal-v1.md. Where this file differs,
this file wins. Decisions marked "proposed" await the user's nod.

## What HLua is

Lua 5.4 embedded in the Half-Life 1 SDK (hlsdk-portable fork). Mod
logic in Lua with a GLua-like api, running on GoldSrc and Xash3D,
without recompiling the SDK per change. Ships as a normal game
library: no engine fork, no metamod.

## Current state

Implemented and covered by the headless test suite (tests/):

- lua 5.4.9 vendored in external/lua54, static library, built by both
  waf and cmake
- one server side lua state created at GameDLLInit, GC stepped per
  frame
- script layout under the gamedir, loaded in this order:
  lua/autorun, lua/shared, lua/entities, lua/server (name order)
- package.path locked to <gamedir>/lua, cpath emptied so scripts
  cannot load binary modules
- globals: print, CurTime, FrameTime, PrecacheModel, PrecacheSound,
  IsValid, Vector, SERVER, CLIENT, MOVETYPE/SOLID constants
- hook library: hook.Add/Remove/Run, named listeners, insertion
  order, first non nil result wins for Run
- timer library (pure lua prelude): timer.Simple/Create/Remove/Exists
- entity bridge:
  - fields: classname, targetname, netname, model, origin, angles,
    velocity, health, solid, movetype, effects, nextthink, flags (ro),
    spawnflags (ro)
  - methods: GetClass, Name, GetPos, SetPos, GetAngles, SetAngles,
    GetModel, SetModel, GetTable, EmitSound, NextThink, SetSize,
    Remove
  - per entity script table via GetTable, lua registry ref stored on
    the C++ object, freed in OnFreeEntPrivateData, cleared on map
    change and refresh
  - carrier class lua_ent: placed in maps with the lua_class
    keyvalue, all other keyvalues land in the kv script table
  - spawn guard: LINK_ENTITY_TO_CLASS asks hlua::TrySpawnScripted
    first, so any classname registered by ents.Register routes to the
    carrier and scripts can extend or replace native classes
  - ents.Create, ents.FindByClass
- sdk hooks: StartFrame, InitPostEntity, ServerDeactivate,
  PlayerConnect (can reject), PlayerInitialSpawn, PlayerDisconnected,
  PlayerCommand (can consume), PlayerSay (replace, block or pass),
  PlayerPreThink, PlayerPostThink, OnEntityCreated, EntityRemoved
- scripted entity callbacks: OnSpawn, OnThink, OnTouch, OnUse,
  OnBlocked, OnRemove
- console: hlua_version, hlua_refresh, hlua_run; cvar hlua_strict
  stops loading further scripts on error when set
- hot reload: hlua_refresh closes the state, recreates it, entities
  rebind through ScriptClass on next callback

Not built yet: user slot passthrough, name lookup, the gen tool,
client side, networking, save/restore of script data, CI.

## Architecture

Four small translation units, no dependencies between them and the
sdk beyond the standard dlls headers:

- hlua/core.cpp: state lifecycle, script loading, core globals,
  console commands, the embedded prelude (prelude.h)
- hlua/hooks.cpp: hook registry (registry table hlua_hooks) and the
  sdk adapter functions called from dlls/client.cpp and friends
- hlua/entity.cpp: entity userdata (edict index), field and method
  bridge, per entity tables, the carrier class, the spawn guard, the
  ents library
- hlua_internal.h: declarations shared between hlua translation
  units only; the sdk sees just the namespace in dlls/util.h

Sdk integration is deliberately thin: one member and one virtual on
CBaseEntity, call sites in client.cpp, cbase.cpp, game.cpp, the
LINK_ENTITY_TO_CLASS macro, and GetNewDLLFunctions for
OnFreeEntPrivateData and GameShutdown. Total sdk diff about 110
lines.

Entity values are edict indexes wrapped in userdata with validity
checks, not C++ pointers, so a stale lua reference errors cleanly
instead of crashing after slot reuse.

## Decisions

D1 library first, metamod deferred
   The sdk fork is the product. A mod ships the built libraries and
   writes lua. Metamod compatibility can come later if wanted; all
   interesting metamod-era forks are x86 only anyway.

D2 GLua-like api
   The audience knows gmod. Same names (hook, timer, ents, IsValid,
   Vector), same semantics where the engines allow.

D3 raw lua C api, no sol2
   Supersedes proposal v1 section 7. The bindings are small enough
   that sol2 buys little while costing a heavy header, exception
   plumbing and build time. The sdk builds -fno-exceptions. The whole
   runtime is about 1600 lines.

D4 authoring and gen phase in lua, no toml. Proposed 2026-11-11
   Supersedes proposal v1 section 13. Entity definitions are lua
   tables registered with ents.Register; the same files feed the
   runtime and the future gen tool. One language, no parser
   dependency, compute at gen time allowed (inheritance, shared
   constants). The gen tool runs the vendored lua in a sandboxed
   state (GEN flag global, no game bindings, no io/os) and emits fgd
   text first, optional compiled glue later. Reasoning in
   discussions/2026-11-11-open-questions.md.

D5 single carrier entity plus spawn guard, no entity pool
   Supersedes the proposal v1 pool idea. One native class lua_ent
   hosts every scripted entity, and the LINK_ENTITY_TO_CLASS guard
   routes registered class names to it at spawn. Unlimited classes
   and instances, one exported symbol, native class replacement for
   free (func_breakable test case). A pool would only add slot
   management and editor naming drift.

D6 entity fields: script table primary, native user slots as
   interop. Proposed 2026-11-11
   entvars_t has iuser1-4, fuser1-4, vuser1-4, euser1-4. Shared
   scratch: the sdk itself resets iuser1/iuser2 on player spawn, and
   other plugins may use them. Plan: script state stays in the per
   entity table (GetTable); the user slots are exposed raw and
   documented as interop (native code, legacy content, state that
   survives savegames for free); ents.FindByName over targetname and
   euserN surfaced as entity values. Reasoning in
   discussions/2026-11-11-open-questions.md.

D7 hot reload resets script state
   v1 scope: hlua_refresh recreates the state and rebinds entities.
   Script data does not survive. A per class keep list (which table
   keys copy over) is a later addition if it hurts in practice.

D8 distribution: three artifact kinds
   1. runtime libraries per target (server now, client later) that go
      inside the mod folder
   2. the dev kit: hlua-gen binary, fgd, editor stubs, example mod,
      api docs
   3. the source repo (the sdk fork itself)
   CI builds all targets per push, runs the headless test, and a
   release pipeline attaches artifacts to tags. See roadmap.md.

## Deviations from proposal v1

- sol2 dropped, raw lua C api instead (D3)
- toml manifest and config generation replaced by lua authoring (D4,
  pending)
- 32 entity pool replaced by the carrier plus spawn guard (D5)
- client bridge, movement bridge and jetpack demo from proposal v1
  sections 10.2 and 10.3 are not started; they are roadmap items, not
  dropped

## Open questions

- does xash3d expose runtime entity class registration (an entity
  factory api)? If yes, pure lua classes could be placed in maps
  under their own names without the carrier. Not needed for v1.
- client side vm: same prelude, CLIENT flag, which api subset, and
  how do shared scripts stay honest about running on both sides
- save/restore: serialize whitelisted script table keys into the save
  file so scripted entities survive level transitions
- usermessages: a small net wrapper so scripts can talk to clients
- manifest and keybinds: proposal v1 section 13 wanted a manifest;
  with lua authoring this becomes lua/manifest.lua, decide when the
  gen tool lands
