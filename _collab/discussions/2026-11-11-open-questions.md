# Open questions from the proposal review, 2026-11-11

Answers to the two issues raised, the entity pool question and the
distribution question. Short versions live in design.md, the
reasoning here.

## 1. Does the gen phase need to be scriptable? lua or toml?

Lua, drop toml.

The proposal had hlua.toml as the manifest and generation input. But
the runtime already registers entity classes from lua tables
(ents.Register), so a toml side channel would mean two authoring
formats for the same thing, plus a parser dependency we do not
otherwise need.

What the gen tool becomes: a small cli (hlua-gen) that creates a
sandboxed lua state (same vendored lua, no game bindings, no io or
os), sets a GEN global so scripts can skip runtime only work, loads
the mod entity definitions, then emits:

- hlua.fgd for map editors: class list, keyvalues, lua_class
  defaults
- later, optional glue for mods that want real class names compiled
  in instead of the carrier

Because definitions are lua they can compute: share constants,
derive classes, loop over data tables. Gmod's ENT tables are the
precedent and our audience already writes them.

Guardrails: the gen state is deterministic by construction (no clock,
no filesystem, no game), and what a script does at load time is
exactly what the runtime would see, so there is one source of truth
between tool and game.

## 2. Field extension: own table plus name lookup, instead of the
   user slots or in addition?

In addition, with different jobs.

entvars_t native slots: iuser1-4, fuser1-4, vuser1-4, euser1-4. They
are the only custom storage that survives save/restore without our
code, but they are shared scratch: the sdk itself clears iuser1 and
iuser2 when a player spawns, other plugins and legacy content may use
them too.

So:

- script state lives in the per entity table (GetTable), which exists
  today: unlimited keys, any lua type, cleaned up on entity removal
- the user slots get exposed raw, read and write, documented as
  interop: talking to native code, legacy content, or carrying small
  state across savegames for free
- name lookup: ents.FindByName over targetname, and euserN surfaced
  as entity values, which gives scripts a way to store cross entity
  references in native slots

This mirrors gmod: engine facing fields stay raw, script state has
its own table, and no escape hatch gets removed.

## 3. Entity pool: 32 pool, one reusable carrier, or both?

Neither a pool nor both. One carrier plus the spawn guard, which is
what the code does now and what the test suite exercises.

The pool existed to pre-link native class names for scripted classes.
But LINK_ENTITY_TO_CLASS now asks hlua::TrySpawnScripted before
constructing the native class, so every exported classname is already
a potential entry point for a registered script class, and one
exported carrier (lua_ent with the lua_class keyvalue) covers classes
that do not shadow a native one. Unlimited classes, unlimited
instances, one symbol, plus native class replacement as a side
effect.

A pool would add slot management, rebinding rules and editor versus
runtime naming drift, and would buy nothing the guard does not
already provide. If xash ever exposes runtime entity registration we
can add that as a bonus path so pure lua classes can be placed in
maps under their own names.

## 4. Distribution: what do we release?

Not just dlls. Three kinds of artifacts:

1. runtime libraries, per target: the server library now, client
   later. These go inside the mod folder next to maps and models.
   Targets: windows x86 (goldsrc and xash), windows x64, linux i386
   (steamrt for goldsrc compatibility), linux amd64, linux arm64,
   android arm32 and arm64 for xash on mobile.
2. the dev kit: hlua-gen binary, hlua.fgd, lua-language-server stubs
   for editor autocomplete, the example mod, the api docs.
3. the source repo, which is the sdk fork itself. A mod author either
   uses our prebuilt libraries or builds the fork.

Pipelines: build matrix on every push with artifacts, the headless
test on every push, and a release pipeline that builds all targets on
a tag and attaches them to a github release. Upstream hlsdk-portable
already has the linux and windows jobs in build.yml; the android and
arm64 jobs and the release workflow get adapted from xash3d-fwgs.
