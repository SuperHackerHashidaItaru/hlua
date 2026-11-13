# HLua - Embedding Lua 5.4 into the Half-Life 1 SDK

**A design proposal for scriptable GoldSrc / Xash3D modding**

| | |
|---|---|
| **Status** | Draft v0.1 - written for review; decisions requested in section 20 |
| **Date** | 2026-11-11 |
| **Project** | HLua (repository already created by you; link TBD) |
| **Scope** | Half-Life 1 via [hlsdk-portable](https://github.com/FWGS/hlsdk-portable), targeting GoldSrc **and** Xash3D FWGS; server side, client side, and `pm_shared`; Windows, Linux, arm32, arm64 |
| **Evidence base** | The portable SDK was cloned and measured for this document (see section 4 and Appendix A); external claims are linked inline |

---

## 1. Summary

HLua turns the Half-Life 1 portable SDK into a **scriptable mod platform**: we embed Lua 5.4 into the SDK's server and client libraries once, ship prebuilt binaries per platform, and from then on mods are written as Lua scripts dropped into a conventional directory - no compiler, no build system, no touching the ~15 scattered config files that GoldSrc mods normally require. One `hlua.toml` manifest declares keybinds, cvar UI entries, and scripted entities; a generator emits `kb_act.lst`, `user.scr`, FGD, and friends from it. Server logic, client HUD, shared player movement, console commands, networked messages, and (tiered) hot reload are all reachable from Lua.

This is, deliberately, "Garry's Mod for GoldSrc" in spirit - but built on the open, cross-platform hlsdk-portable rather than a closed fork of a specific game.

Headline findings from the research behind this document:

- **The SDK is big; the surface you bind is small.** hlsdk-portable is ~157k lines over 429 C/C++ files - but you don't *wrap* that code, you *reach* it through seams: 161 server engine functions, 131 client engine functions, 55 engine->game callbacks, 251 entity classnames dispatched through a single macro, 56 shared movement functions behind one entry point, and ~30 client export hooks. Everything else (monster AI, weapon logic) is implementation you override or compose with, not translate. The hand-written binding layer is realistically **~6-9k LOC of C++** plus data tables (section 4, section 9).
- **Your sol2-in-one-TU plan is right, with one mandatory correction.** The SDK builds with `-fno-exceptions` (waf) and `-fno-exceptions -fno-rtti` (CMake). sol2 works in that mode, but only with `SOL_NO_EXCEPTIONS` / `SOL_NO_RTTI` defined and a panic handler installed. Details in section 7.
- **Coexistence with other mods is a real fork in the road, and the evidence picks a side.** Being your own mod costs nothing and gives the whole surface. The "spoof the compiled DLL's known surface" idea you asked about is literally what Metamod does server-side - it's proven, but every Metamod fork today is **i386-only**, which kills it for your arm32/arm64 targets. Recommendation: own mod first, embeddable library second, Metamod overlay only if the ecosystem begs (section 14).
- **Hot reload is tractable in tiers** because HLua dispatches hooks *by name*, not by captured pointer - existing entities pick up new logic on their next call. Full state-preserving reload is a research item; logic reload is not (section 11).
- **Name check:** no Half-Life project named "HLua" exists on GitHub today; the collisions are a Rust Lua crate (`hlua`), a Michigan water utility, and HAProxy internals. All different domains. The name is yours.

The document ends with an MVP ladder whose M1 acceptance test is porting **your existing 40-line C++ jetpack to Lua** - a deliberate "dogfood the exact thing you already know" milestone.

---

## 2. Goals and non-goals

**Goals**

1. **Zero-compilation modding.** After installing HLua binaries once, creating or changing a mod requires only a text editor. No compiler, no waf/CMake/MSVC, no DLL hell.
2. **Full-stack coverage**: server (`dlls/`), client (`cl_dll/`), and shared player movement (`pm_shared/`) from one script tree, with a shared/client/server split that mirrors the SDK's own.
3. **Both engines, both roles**: GoldSrc (Windows client, Linux dedicated server) and Xash3D FWGS (Windows/Linux, x86/x64/arm32/arm64, incl. Android-derived targets), server and client side.
4. **Declarative project spec**: one `hlua.toml` describes the mod; generators produce every scattered config file (keybind lists, cvar UI, FGD for map editors, `liblist.gam` fields). Modders never hand-edit `gfx/shell/kb_act.lst` again.
5. **Hot reload**, tiered: instant refresh during development, honest about what can't be refreshed.
6. **Documentation as a product**: generated, stable, example-heavy API docs that serve human modders *and* AI coding agents - you explicitly wanted mods to be writable by agents.
7. **A runtime you can also embed**: HLua core is a static library first, a mod second, so other portable-SDK mod teams can adopt it (section 14, Option B).

**Non-goals (for v1)**

- Source engine anything. GoldSrc/Xash3D only.
- Running *alongside* an arbitrary closed mod's DLLs (the Metamod overlay) - investigated and deliberately deferred; see section 14.
- A hard security sandbox for untrusted scripts. We sandbox best-effort and do not auto-download scripts (section 15).
- Rewriting vanilla HL content (monsters/weapons) in Lua. The C++ stays; scripts override and extend.
- Replacing the asset pipeline (models, maps, sounds). We reduce *config* friction around it, not the content tools themselves.

---

## 3. How Half-Life modding works today (the constraint map)

A GoldSrc/Xash3D mod is **one server library + one client library + a folder of data**. Understanding this model is prerequisite to every design decision below.

**The load chain.** The engine finds a mod folder via Steam (or Xash3D's launcher), reads `liblist.gam` - a plain key/value file - and loads the `gamedll` it names (e.g. `dlls/hl.dll` on Windows, `dlls/hl.so` on Linux). That library is the *entire* server game logic. Independently, the engine loads the client library from `cl_dlls/client.dll`. There is exactly one of each per mod: **a mod *is* its DLLs**. That is both the friction you're removing (change a line -> rebuild -> redistribute binaries) and the reason coexistence with other mods is awkward (section 14).

**How the engine talks to the game library.** The engine hands the DLL two things on load: an `enginefuncs_t` table (161 function pointers - the game calls the engine through it) and a `globalvars_t`. The DLL hands back a `DLL_FUNCTIONS` table (50 callbacks - Spawn, Think, Touch, StartFrame, ClientConnect, PM_Move, ...) plus an optional `NEW_DLL_FUNCTIONS` table (5 more, versioned). The client mirror: `cl_enginefunc_t` (131 pointers) in, ~30 exported `HUD_*` functions out. Everything a mod can ever do passes through these tables - which is why they're also the complete, finite seam list for binding (section 4).

**Entity spawning is export-name lookup.** This is the single most important mechanism for HLua's entity story. The engine, when it needs to create an entity of classname `"monster_zombie"`, resolves a C symbol *of that exact name* from the game library (`GetProcAddress` / `dlsym`). That's why the SDK's `LINK_ENTITY_TO_CLASS` macro generates an exported function per classname - 251 of them in this SDK. Two consequences:

- Script code can *override* any existing entity by intercepting dispatch before the C++ class is constructed (section 10).
- Script code *cannot* invent brand-new classnames at runtime - the export table is fixed when the library is loaded. New scripted entities spawn through a small pool of pre-registered carrier exports (`lua_ent`, `lua_ent_02`...`lua_ent_NN`) with the logical class carried in a keyvalue, and the FGD generator makes the pool look like real classnames to mappers (section 13).

**`pm_shared` runs on both sides.** Player movement is one set of ~9 files / ~4.6k LOC compiled into *both* libraries: the server runs it authoritatively, the client runs it again to *predict* movement between server updates. This is why your jetpack mod in C++ had to be careful - and why scripted movement must load identically on both sides (section 12).

**The config sprawl.** This is the "million scattered files" you called out, measured:

| File | Lives in | Controls | Format |
|---|---|---|---|
| `liblist.gam` | mod root | which game DLLs load, mod metadata, startmap | key/value lines |
| `gfx/shell/kb_act.lst` | mod's `gfx/shell/` | the **keybind list** shown in Options -> Keyboard | `"command" "caption"` per line; `"blank"` rows make headers |
| `gfx/shell/kb_def.lst` | mod's `gfx/shell/` | default bindings for the above (GoldSrc era) | same |
| `user.scr` | mod root | cvar UI in Options -> Advanced (client cvars) | nested braces: cvar, prompt, type, default |
| `settings.scr` | mod root | cvar UI in Create Server -> Game (server cvars) | same format |
| `*.fgd` | editor config | entity list for Hammer/J.A.C.K./TrenchBroom-class editors | `@PointClass base(...) = name : "help" []` |
| `titles.txt`, `sentences.txt` | mod root | HUD text / sentence speech scripting | custom text formats |
| `events/*.sc` | mod's `events/` | client-side playback of animation events | custom |
| `sprites/*.txt`, `hud.txt` | mod's `sprites/` | HUD sprite rectangles, weapon slots | custom |
| `maps/<map>.res` | `maps/` | files to download from server for a map | plain list |

Sources: Valve Developer Community on [liblist.gam](https://developer.valvesoftware.com/wiki/Liblist.gam) and [kb_act.lst](https://developer.valvesoftware.com/wiki/Customizing_Options:_Keyboard); TWHL wiki on [settings.scr / user.scr](https://twhl.info/wiki/page/settings.scr_and_user.scr) and [file types](https://twhl.info/wiki/page/File_Types_and_Formats) (GoldSrc keybind files live under `gfx/shell/`). Every one of these is generated from `hlua.toml` in HLua (section 13).

---

## 4. The SDK by the numbers

Everything in this section was measured by cloning `FWGS/hlsdk-portable` (master, Nov 2026) and running a stats script over the tree (script + raw output preserved in the project). Full tables in Appendix A.

**Size:**

| Module | Files | LOC | What it is |
|---|---:|---:|---|
| `dlls/` (server) | 147 | 95,584 | all game logic: entities, AI, weapons, gamerules |
| `cl_dll/` (client) | 94 | 36,469 | HUD, input, prediction glue, VGUI, studio renderer |
| `pm_shared/` | 9 | 4,586 | player movement, compiled into both of the above |
| `game_shared/` | 24 | 3,327 | code shared client/server (voice, VGUI helpers) |
| `common/` + `engine/` + `public/` | 51 | 5,902 | interface headers (the contracts, effectively) |
| `external/`, `utils/` | 104 | 11,507 | vendored compat code, VGUI1 headers, tools |
| **Total (C/C++/H)** | **429** | **157,375** | |

**The useful surface - the actual answer to "how big are we talking":**

| Seam | Count | Nature |
|---|---:|---|
| `enginefuncs_t` (server -> engine) | 161 fn pointers | mechanical to bind, one-liners each |
| `cl_enginefunc_t` (client -> engine) | 131 fn pointers | same |
| `DLL_FUNCTIONS` + `NEW_DLL_FUNCTIONS` (engine -> server) | 50 + 5 callbacks | these *are* the server hook list |
| Client exports (`HUD_*`, `V_CalcRefdef`, `IN_*`...) | ~30 functions | these *are* the client hook list |
| Entity classnames (`LINK_ENTITY_TO_CLASS`) | 251 | reachable via **one** macro patch + base-class virtuals |
| C++ entity classes deriving `CBase*` | ~166 | override/compose, not wrap |
| `PM_*` functions in `pm_shared` | 56 | behind one entry point (`PM_Move`) |
| `playermove_t` fields exposed to scripts | ~62 | direct struct access |
| `UTIL_*` helper functions | 84 | curated subset worth binding |

Read the table this way: **~350 API entries and ~10 structural seams cover 100% of what any Half-Life mod can do.** The other ~150k LOC is what those seams *call into*. GMod made the same observation about Source and consequently exposes the seams, not the implementation. Concretely for HLua, tier by tier (rationale in section 9-section 10):

- **Tier 0 - engine function tables** (292 entries): thin, data-driven binding. Most are `sol::function`-registration one-liners; ~3k LOC including marshalling helpers for `vec3_t`, `entvars_t`, edicts.
- **Tier 1 - gameplay hooks** (55 server callbacks + client exports): the hook bus; ~2k LOC.
- **Tier 2 - entity bridge**: macro patch + generic carrier + pev/entvars mapping; ~1.5k LOC.
- **Tier 3 - movement bridge**: `PM_Move` gating + `playermove_t` usertype; ~800 LOC.
- **Tier 4 - HUD/draw/input** (later): ~1-2k LOC.
- Total hand-written C++ ~ **6-9k LOC**, plus the Lua-side standard library we ship (timers, math helpers, manifest runtime) in Lua itself.

**Build facts that shape the design** (verified in `wscript` and the `CMakeLists.txt` files):

- The waf build appends **`-fno-exceptions`** to CXXFLAGS globally; the CMake build adds **`-fno-exceptions -fno-rtti`** on GCC/Clang. Consequence: sol2 must run in no-exceptions/no-RTTI mode (section 7).
- C++ standard is conservative (C++14-era, MSVC 2015+ compatible per the [metamod-fwgs toolchain notes](https://github.com/FWGS/metamod-fwgs) - hlsdk-portable itself documents VS2015-and-later or any modern GCC/Clang). sol2 needs C++17: we compile **only the single HLua backend TU** with `-std=c++17` (or C++20) while leaving the rest of the SDK untouched. This is one TU of divergence, not a repo-wide migration.
- Engine detection is **runtime, not ifdef**: the client uses `IsXashFWGS()` (`cl_dll/cdll_int.cpp:341`) and split files (`input_goldsource.cpp` / `input_xash3d.cpp`). Only 3 `XASH_*` compile-time macros exist (big-endian, build metadata). HLua follows the same pattern - same binaries source, engine-specifics resolved at runtime.
- Platform support per the README: Windows x86/x64 (MSVC or MinGW via CMake), Linux x86/x64/**arm**/FreeBSD, Android (gradle), Nintendo Switch (aarch64), PS Vita (ARM), PS4 (amd64, as `.prx`). `GOLDSOURCE_SUPPORT` defaults ON only on x86 Windows/Linux - correct, since GoldSrc itself is a 32-bit x86 engine. CI (`build.yml`) covers Linux gcc/clang (in Valve's Steam Runtime scout i386 chroot) and Windows MSVC x86; the broader matrix lands in their [hlsdk-mega-build](https://github.com/FWGS/hlsdk-mega-build/releases/tag/continuous) artifacts.
- `mod_options.txt` at the repo root is the SDK's own single-file build-option registry (feature macros, `GAMEDIR`, install dirs, library names). HLua's build options integrate there - the SDK already believes in "one file instead of scattered config".

**License:** the repo ships Valve's **Half-Life 1 SDK License**: modify and distribute in source and object form, *free of charge only*, keep the LICENSE file, contact Valve for commercial use. Lua 5.4 and sol2 are MIT. Xash3D FWGS (the engine) is GPLv3, but it loads game libraries at arm's length - the entire non-free HLSDK-derived mod ecosystem runs on it. Our plan (section 15): HLua core (`hlua/` directory, our code) under MIT; SDK-derived binaries distributed per the Valve license with the LICENSE file included; nothing relicensed.

---

## 5. Prior art and lessons

| Project | What it is | What it proves for HLua |
|---|---|---|
| **Garry's Mod** | Source-engine sandbox, modding via GLua (Lua 5.1 derivative; the [x86-64 branch moved to LuaJIT](https://steamcommunity.com/sharedfiles/filedetails/?id=2882777333)) | The entire product thesis: expose engine seams to Lua, keep the C++ underneath. Also a caution: GMod's 64-bit LuaJIT reportedly performs *worse* in some cases - vindicating your choice of plain Lua 5.4 over LuaJIT. And GLua's API shape is the vocabulary a generation of modders already knows - we borrow its naming (section 9). |
| **Sven Co-op 5.x AngelScript** | GoldSrc mod with an embedded AngelScript VM; [mapper-facing script API](https://wiki.svencoop.com/Angelscript) with [auto-generated docs](https://sven-coop.github.io), map scripts and server plugins (e.g. [Mikk155's collection](https://github.com/Mikk155/Sven-Co-op)) | **The existence proof on GoldSrc itself**: a scripting VM inside an HLSDK-derived server DLL, with new script-defined entities, shipping to real players for a decade. Also proves the docs-generation pipeline you want. |
| **AMX Mod X + Metamod** | [Metamod](https://github.com/FWGS/metamod-fwgs) inserts itself between engine and game DLL; AMX Mod X is a Pawn-scripting plugin on it; still the default admin stack on CS 1.6 servers | Server-side "spoof the DLL surface" is 20+ years battle-tested. Also the ecosystem lesson: script plugins flourished *because* no recompilation was needed - exactly your friction argument. |
| **NodeMod** | [A Metamod plugin embedding Node.js](https://nodemod.org/docs) in Half-Life servers (i386, plus Xash3D FWGS x64) | Embedding a *large* scripting VM as an overlay works in production today; also shows the overlay's architectural ceiling (engine-level API only - no entity-class access, since the game DLL is a black box to it). |
| **MetaHook / MetaHookSv** | [Client-side hooking framework](https://github.com/MetaHookSv) for GoldSrc/Sven Co-op ("plugins can hook the engine without modifying the game's own files"), plus [svenmod](https://github.com/sw1ft747/svenmod)'s C++ plugin env | Client-side DLL hooking is possible and has a community - but note it's C++ plugins, Windows-centric, engine-version-sensitive. This is the maintenance tax section 14 warns about. |
| **hlsdk-portable itself** | The SDK fork we build on; actively maintained (bugfix changelog in README, Nov 2026 activity), multi-platform, runtime engine detection | We're not maintaining an SDK - we're maintaining a **small patch set on top of an actively-maintained one** (section 6 patch-surface table). |

The gap in this landscape - and HLua's slot - is a *first-class, cross-platform, both-sides, hot-reloadable Lua runtime for the portable SDK itself*, rather than an overlay bolted beside it or a single-game feature.
---

## 6. Architecture overview

```
                 +------------------------------------------------+
                 |                GoldSrc / Xash3D engine          |
                 +-------^---------------------------^------------+
            enginefuncs_t| (161)                     | ~30 client exports
            cl_enginefunc_t (131)        DLL_FUNCTIONS (50+5)
                 +-------+---------------------------+------------+
                 |        hlsdk-portable (fork, ~10 patch points) |
                 |  dlls/  --+                    +--  cl_dll/    |
                 |           |  both link:        |               |
                 |  pm_shared/ (movement)  game_shared/            |
                 +-------^---------------------------^------------+
                 +-------+---------------------------+------------+
                 |  HLua core (new, ~6-9k LOC C++ + Lua stdlib)   |
                 |  +----------+ +-----------+ +---------------+  |
                 |  | backend  | | hook bus  | | bridges: ent, |  |
                 |  | (one TU, | | (by-name  | | pm, hud, net, |  |
                 |  |  sol2)   | |  dispatch)| | input, cvar   |  |
                 |  +----------+ +-----------+ +---------------+  |
                 |  manifest compiler, hot reload, docgen data  |
                 +-------^---------------------------^------------+
                 +-------+---------------------------+------------+
                 |  lua/ scripts (autorun, server, client,      |
                 |  shared, entities)  +  hlua.toml manifest     |
                 +------------------------------------------------+
```

Two HLua states exist per game: one inside the server library, one inside the client library. They never share memory; they coordinate via normal game channels (user messages, cvars, and the fact that `lua/shared/` + `pm_shared` scripts run identically on both sides, section 12).

**The fork stays small on purpose.** This is the entire patch surface outside `hlua/` (every location verified in the cloned tree):

| File | Patch | Purpose |
|---|---|---|
| `dlls/util.h` (line 110) | extend `LINK_ENTITY_TO_CLASS` macro by one guard line | route every entity spawn through script dispatch first |
| `dlls/h_export.cpp` (`GiveFnptrsToDll`) | +2 lines | server-side `hlua::Startup()` |
| `dlls/game.cpp` (`GameDLLInit`) / `dlls/client.cpp` (`StartFrame`) | +2 lines | server frame tick, map-change reload point |
| `dlls/cbase.cpp` (`GetEntityAPI*`) | 0 lines (read-only seam) | the 55 engine->game callbacks are the hook list |
| `cl_dll/cdll_int.cpp` (`Initialize`, `HUD_Init`, `HUD_Redraw`) | +3 lines | client startup, command registration, frame tick |
| `pm_shared/pm_shared.c` (`PM_Move`) | +2 lines | movement gate + post-move hook |
| `wscript`, `CMakeLists.txt`, `mod_options.txt` | build glue | add `hlua/` + Lua 5.4 static lib + sol2 include |

Under ~30 changed lines of SDK code, everything else in new files. That's the number that keeps the fork cheap to rebase onto upstream hlsdk-portable (which is actively maintained - see its changelog).

**HLua core modules** (all under `hlua/` in the repo):

- `hlua/backend.cpp` - the single sol2 TU (section 7)
- `hlua/hookbus.*` - by-name hook registry with a bitmask fast path
- `hlua/entity_bridge.*` - scripted-entity dispatch, carrier class, pev marshalling
- `hlua/pm_bridge.*` - movement gating, `playermove_t` usertype
- `hlua/client_bridge.*` - HUD, input, draw API
- `hlua/net_bridge.*` - usermsg wrapper
- `hlua/manifest.*` - `hlua.toml` parsing + generators (also callable in-game, section 17)
- `hlua/reload.*` - tiered refresh (section 11)

---

## 7. The binding layer: sol2 in one TU

Your plan - sol2 for iteration speed, isolated in a single translation unit - is the right call, and the SDK measurements confirm the isolation is *necessary*, not just tidy:

**1. The exception problem is real and solved.** The SDK builds with `-fno-exceptions` (waf) / `-fno-exceptions -fno-rtti` (CMake). sol2 explicitly supports this mode:

```cpp
// hlua/backend.cpp - the ONLY file in the repo that sees sol2 or lua.hpp
#define SOL_ALL_SAFETIES_ON 1
#define SOL_NO_EXCEPTIONS 1   // SDK builds -fno-exceptions (wscript: CXXFLAGS)
#define SOL_NO_RTTI 1         // CMake builds also use -fno-rtti
#include <sol/sol.hpp>

namespace {
    sol::state lua;   // one state per side (server or client), never both
}

// installed once at startup: a script error becomes a console error,
// never a process abort
static void hlua_panic(sol::optional<std::string> what) {
    Con_Printf("HLua: panic: %s\n", what.value_or("unknown").c_str());
}
```

With `SOL_NO_EXCEPTIONS`, C++-side errors inside bound functions are converted to Lua errors surfaced through `sol::protected_function` results - scripts can fail loudly without taking down the server. Note MSVC builds may *not* disable exceptions (the flags are GCC/Clang in CMake; waf sets them globally); we define the `SOL_NO_*` pair unconditionally anyway so all targets behave identically. RTTI absence is handled by sol2's static-type paths; we avoid `sol::object`-style dynamic casts in hot code regardless.

**2. The boundary is a narrow, sol-free header.** Nothing outside `backend.cpp` includes sol2 - the rest of the SDK (and our bridges) see only:

```cpp
// hlua/hlua.h - the public boundary: no templates leaking sol types
namespace hlua {
    enum Side { Server, Client };
    bool Startup(Side side);            // reads lua/ tree, runs autorun
    void Shutdown();
    void Frame(double time, double dt); // tick: timers, GC step, frame hooks

    // hook bus (by name; C++ caches sol::function refs, invalidated on reload)
    bool HasHook(const char *name);     // O(1) via pre-registered ids + bitmask
    // typed Call<...> variants per hook signature, all no-throw

    // entity dispatch, called from the patched LINK_ENTITY_TO_CLASS
    bool TrySpawnScripted(const char *classname, entvars_t *pev);
}
```

This keeps compile times sane (one heavy TU, everything else cheap), keeps the SDK build flags untouched, and - crucially for later - makes the backend *swappable*. If sol2 ever becomes a liability (a console toolchain where C++17 is a fight, or compile-time pain in CI), the same `hlua.h` contract can be reimplemented over the raw Lua C API. That's the fallback; we don't build it first.

**3. Everything else is data, not code.** The engine function tables are bound from a declarative registry - one row per function: name, signature marshalling, docs string, tier tag. That same table drives (a) runtime registration, (b) the generated API docs, and (c) `api.d.lua` type annotations for the Lua language server (section 17). Bindings written once, used three ways.

**Pinned versions:** Lua 5.4.x (unmodified, built as C, static), sol2 v3.5.x (amalgamated single include). Both are MIT; both build on every platform the SDK itself builds on, including the console targets.

---

## 8. Script runtime and layout

A mod's script tree, by convention:

```
my-mod/
  hlua.toml              # the project spec (section 13)
  liblist.gam            # generated/patched from the spec
  gfx/shell/kb_act.lst   # generated from the spec
  user.scr, settings.scr # generated from the spec
  hlua.fgd               # generated from the spec (for map editors)
  lua/
    autorun/    *.lua    # both sides, at startup (ordered by name)
    server/     *.lua    # server library only
    client/     *.lua    # client library only
    shared/     *.lua    # both sides, after autorun (movement, shared ents)
    entities/   *.lua    # entity classes (server; client part when needed)
    gamemode/   *.lua    # optional higher-level rules (later milestone)
```

Semantics:

- **Side detection:** `SERVER` / `CLIENT` compile-time-ish constants (globals set at state init). Idiom: `if SERVER then ... end`, mirroring GLua/GMod vocabulary modders already know.
- **Load order:** `autorun/` -> manifest-declared `include` order -> `shared/` -> side directories. Deterministic, alphabetical inside a directory, and the manifest can pin explicit ordering where it matters.
- **One state per side**, no cross-side global access. Server -> client communication is user messages (section 12); that's the engine's model and we don't fight it.
- **Script errors are non-fatal by default:** a failed chunk logs to console with file/line, the rest of the tree still loads, and a cvar (`hlua_strict 1`) escalates to hard failure for CI runs.
- **`print()` goes to the console**, `hlua.log` wraps engine logging, and `debug.traceback` is wired into the error path - boring ergonomics that matter enormously when your target audience includes agents writing mods blind.

---

## 9. The Lua-facing surface (how it looks from scripts)

API shape is **GLua-familiar by design** (lower the onboarding wall for the biggest existing pool of game-Lua modders), with HL-specific additions where GoldSrc demands them. All names below are the *proposed contract for review*, not a commitment.

**Hooks, commands, cvars - the daily driver:**

```lua
-- lua/autorun/hello.lua
if SERVER then
    hook.Add("PlayerSpawn", "give_welcome", function(pl)
        pl:GiveItem("item_suit")
        print(pl:GetName() .. " spawned with " .. pl:GetHealth() .. " hp")
    end)

    concommand.Add("shipments", function(pl, args)
        if pl and not pl:IsAdmin() then return end
        print(("player edicts in use: %d / %d"):format(ents.Count(), ents.MaxCount()))
    end)
end

local cv = cvar.Create("hlua_welcome_hp", "100", FCVAR_ARCHIVE,
                       "HP granted on spawn (see user.scr entry)")
```

**Overriding an existing entity** - remember, dispatch is by classname, so this needs no registration beyond the table:

```lua
-- lua/entities/func_breakable.lua
ents.Override("func_breakable", {
    OnSpawn = function(self)
        self:SetHealth(self:Health() * 2)      -- tougher glass
    end,
    OnBreak = function(self)
        sound.Emit(self:GetPos(), "debris/bustglass" .. math.random(1, 4))
        return "default"                       -- run vanilla break afterwards
    end,
})
```

**A brand-new scripted entity** (spawns through the `lua_ent` carrier, section 10; FGD entry generated from the manifest, section 13):

```lua
-- lua/entities/jetpad.lua
ents.Register("jetpad", {
    Base = "lua_ent",
    Fields = { fuel = "number", recharge = "number" },  -- save/restore + keyvalues
    OnSpawn = function(self)
        self.fuel = tonumber(self:GetKeyValue("fuel", "25"))
    end,
    OnThink = function(self)
        self:NextThink(0.5)
        for _, pl in ipairs(ents.FindInSphere(self:GetPos(), 96)) do
            net.Start("JetFuel"):Entity(pl):Send(pl)
        end
    end,
})
```

**Client HUD and input:**

```lua
-- lua/client/hud_fuel.lua
local FUEL = hud.Add("JetFuel", { x = 8, y = 48 })
function FUEL:Draw(w, h)
    if fuel <= 0 then return end
    draw.Rect(0, 0, 120 * fuelFraction, 8, { r = 80, g = 200, b = 255 })
    draw.Text(0, 12, ("FUEL %d%%"):format(math.floor(fuelFraction * 100)))
end

input.BindCommand("+hlua_jetpack", function(down) jetActive = down end)
```

**The draw layer** is an immediate-mode wrapper over the client engine's own 2D calls (`SPR_Draw`, console text, triangles via `TriAPI`) - no VGUI in v1. VGUI1 exists in the SDK tree (`utils/vgui`) and stays reachable for ambitious scripts later, but shipping UI on immediate-mode primitives is what makes the client bridge ~1k LOC instead of ~5k.

**What's deliberately *not* in Lua v1:** full weapon prediction plumbing (the SDK's `com_weapons`/`cl_dll/hl/hl_weapons.cpp` machinery is intricate; scripts can add *unpredicted* client effects and *server-side* weapons from day one, predicted weapons land in a later milestone), and VGUI panels. Both are scoped in section 18's ladder, not dropped.

---

## 10. Bridges in depth

### 10.1 Entity bridge

The SDK's spawn macro (verified, `dlls/util.h:110`) is the single choke point for all 251 classnames:

```cpp
// upstream:
#define LINK_ENTITY_TO_CLASS(mapClassName,DLLClassName) \
    extern "C" EXPORT void mapClassName( entvars_t *pev ); \
    void mapClassName( entvars_t *pev ) { GetClassPtr( (DLLClassName *)pev ); }

// HLua fork - one added guard line:
#define LINK_ENTITY_TO_CLASS(mapClassName,DLLClassName) \
    extern "C" EXPORT void mapClassName( entvars_t *pev ); \
    void mapClassName( entvars_t *pev ) { \
        if( HLUA_TrySpawnScripted( #mapClassName, pev ) ) return; \
        GetClassPtr( (DLLClassName *)pev ); }
```

`HLUA_TrySpawnScripted` is a hash lookup into the registered-script class table - when no scripts are loaded the table is empty and the cost is one failed lookup per entity spawn (spawn-time only, not per-frame). On hit, the entity is constructed as a `CLuaEntity` carrier: a thin C++ class implementing the same virtuals as `CBaseEntity` and forwarding each one (`Spawn/Think/Use/Touch/Blocked/KeyValue/Save/Restore`) into the Lua table *looked up by classname at call time*.

Two properties fall out of this design and they pay for it:

1. **Overrides compose with vanilla.** A script table can return `"default"` from a hook to fall through to the original C++ behavior (partial override), or handle everything (full replacement).
2. **Existing instances adopt new code on reload.** Because dispatch is by name, not by captured `sol::function`, a hot-reloaded entity class affects entities already in the map on their next `Think`. This is the foundation of tier-2 hot reload (section 11).

**New classnames** cannot be invented at runtime (the engine resolves spawn functions by export name at load time - section 3), so scripted entities spawn through pre-registered carriers:

```cpp
// hlua/entity_bridge.cpp (new file) - the export pool
extern "C" EXPORT void lua_ent( entvars_t *pev)     { hlua_spawn_carrier(pev); }
extern "C" EXPORT void lua_ent_02( entvars_t *pev ) { hlua_spawn_carrier(pev); }
//   ... lua_ent_03 .. lua_ent_32 (pool size a build option, default 32)
```

The carrier reads `lua_class` from its keyvalues, resolves the script table, and the FGD generator emits real-looking classnames (`@PointClass ... = lua_ent_02`) mapped to script classes via the manifest - mappers never see the trick.

### 10.2 The movement bridge (and your jetpack, in Lua)

`PM_Move` is the verified entry point the engine calls on both sides (`pm_shared/pm_shared.c:3376`). The patch is two lines:

```cpp
void PM_Move( struct playermove_s *ppmove, int server )
{
    assert( pm_shared_initialized );
    pmove = ppmove;

    if( !hlua::pm::Begin( server ) )                    // HLua: script takeover?
        PM_PlayerMove( ( server != 0 ) ? true : false );
    hlua::pm::End( server );                            // HLua: post-move hook

    // ... upstream tail (FL_ONGROUND fixup, friction reset) unchanged ...
}
```

Scripts get the `playermove_t` struct as a direct usertype - all ~62 fields are real, verified names (`player_index`, `frametime`, `velocity`, `origin`, `onground`, `dead`, `waterjumptime`, `usehull`, the `iuser*/fuser*/vuser*` "for mods" slots, `cmd.buttons`, ...). Your 40-line C++ jetpack becomes, roughly:

```lua
-- lua/shared/jetpack.lua  - runs on server AND in client prediction
local THRUST, MAX_FUEL, BURN, REGEN = 350.0, 100.0, 30.0, 10.0

hook.Add("PlayerMove", "jetpack", function(pm)
    if pm.dead or pm.onground ~= 0 then pm.fuser1 = MAX_FUEL; return end
    if pm.cmd.buttons & IN_JUMP == 0 then return end   -- Lua 5.4 native bit ops

    local fuel = pm.fuser1                      -- pmove's "for mods" slot:
    if fuel <= 0 then return end                -- predicted on both sides
    pm.fuser1 = fuel - BURN * pm.frametime

    pm.velocity.z = pm.velocity.z + THRUST * pm.frametime
    if pm.velocity.z > 300 then pm.velocity.z = 300 end

    if SERVER and pm.time % 0.2 < pm.frametime then
        sound.EmitAtEntity(pm.player_index, "ambience/steamburst1.wav")
    end
end)
```

No `#include`, no rebuild, works in multiplayer because both sides run the same chunk (section 12 covers the handshake that guarantees that). This file - literally this file - is the M1 acceptance test (section 18).

### 10.3 Client bridge

Three verified insertion points in `cl_dll/cdll_int.cpp`: `Initialize` (state startup + `IsXashFWGS()` is available right there), `HUD_Init` (register script console commands and binds), `HUD_Redraw` (frame tick + HUD element draw calls). Input goes through registered `+commands` (the `input.BindCommand` above), which the engine treats exactly like native `IN_*` buttons - that's also what makes them keybindable and what the `kb_act.lst` generator advertises.
---

## 11. Hot reload

You asked whether this is feasible before seeing the SDK's shape; now that we have, the answer is **yes, tiered** - because HLua's dispatch-by-name design makes reloads much cheaper than they'd be in a captured-pointer design.

- **R1 - logic refresh (instant, dev-only console command `hlua_refresh`).** At a frame boundary (server: end of `StartFrame`; client: end of `HUD_Redraw`), spin up a fresh `lua_State`, replay the load order, re-run manifest generators, then swap the C++-side hook registry to the new state's registrations and destroy the old state. Timers, hooks, HUD elements, concommands: all swapped between two frames. Nothing about the world changes - entities keep existing, players keep playing. This alone covers the majority of iteration loops (tweaking damage numbers, hook logic, HUD layouts).
- **R2 - entity re-dispatch (comes free).** Because the carrier forwards virtuals to a table *looked up by classname at call time* (section 10.1), entities already in the map adopt re-registered class tables on their next `Think`/`Touch`. Script-side per-entity state is preserved across R1 via a C++-side side-channel: before destroying the old state we serialize each scripted entity's `Fields` (the manifest-declared persistent fields) and rehydrate them in the new state.
- **R3 - full state-preserving reload (research, post-M2).** Snapshot all entities ( classname, keyvalues, `Fields`), restart the map, re-spawn from snapshot. This is the honest fix for the things R1/R2 can't refresh: scripted entities whose *carrier type* or spawn-time choices changed, movement constants baked into stuck players, closed-over coroutines (documented as lost). GMod-scale addons live with the same limit; we surface it as a one-command "soft restart" instead of pretending it's free.

Guard rails: reload is refused while a script is mid-call (single-threaded engine makes this a non-issue - the engine is the only caller), and a changed file watcher (dev mode only) triggers R1 automatically. Save/load of real savegames is *not* a reload path and is handled separately (section 16).

## 12. Networking and prediction

- **User messages are the only cross-side channel**, wrapped in a small typed API: server `net.Start("name") :Byte(42) :Entity(pl) :Send(pl)`; client `net.Receive("name", fn)`. Underneath it's `REG_USER_MSG` + the engine's write functions on one side and the `HUD_*` message hooks the client already routes (`hud_msg.cpp`) on the other - the same plumbing every GoldSrc mod uses, minus the boilerplate.
- **Prediction correctness is a contract, and we enforce it at connect.** `pm_shared` scripts run on both sides; if client and server run *different* script versions, prediction diverges and players rubber-band. At connect the server sends a hash of the `lua/shared/` + `lua/entities/` tree (one tiny user message); the client compares and either logs a loud mismatch (dev builds refuse to predict - the engine degrades gracefully to unpredicted movement) or stays silent. This is cheap, and it converts a class of "impossible to debug" jank into a console line.
- **Determinism rules for movement scripts** (documented, and linted by `hlua doctor`): no `math.random` without a seeded RNG exposed by HLua (deterministic per usercmd), no wall-clock reads, no side effects in shared code guarded only by `SERVER`/`CLIENT`. The pmove `iuser*/fuser*/vuser*` slots are the sanctioned cross-side per-player state - exactly what the jetpack uses for fuel.

## 13. Manifest, keybinds, and the asset pipeline

One file describes the mod; the generators write every scattered config file from section 3's table. Format proposal - **TOML** (human-writable, agent-writable, commented, no executable content - deliberately not Lua, so specs stay data):

```toml
# hlua.toml
[mod]
name        = "Jetpack Plus"
author      = "you"
version     = "0.1.0"
gamedir     = "jetpack"          # -> liblist.gam game/liblist fields

[binds]                              # -> gfx/shell/kb_act.lst (+ kb_def.lst)
jetpack = { caption = "Jetpack thrust", cmd = "+hlua_jetpack", default = "v" }

[cvars]                              # -> user.scr (client) / settings.scr (server)
hlua_jetpack_fuel   = { default = "100", prompt = "Fuel capacity", type = "number", min = 10,  max = 999 }
hlua_jetpack_thrust = { default = "350", prompt = "Thrust",        type = "number", min = 50,  max = 1000 }

[entities]                           # -> hlua.fgd (maps to the lua_ent export pool)
jetpad = { carrier = "auto", base = "lua_ent", class = "Jetpad", help = "Refuels jetpacks" }
```

Generated outputs (formats verified - section 3 sources):

```text
# gfx/shell/kb_act.lst            # user.scr (excerpt)
"Jetpack thrust"  "+hlua_jetpack"  VERSION 1.0
                                  DESCRIPTION INFO_OPTIONS
                                  {
                                    "hlua_jetpack_fuel"
                                    {
                                      "Fuel capacity"
                                      { NUMBER 10.000000 999.000000 }
                                      { "100.000000" }
                                    }
                                  }
```

```text
// hlua.fgd - generated; add to Hammer/J.ACK once per editor install
@PointClass base(Targetname) = lua_ent_02 : "Jetpad (refuels jetpacks)" []
```

The manifest compiler runs in three places, from the same code: in-game (`hlua_gen` console command - no external tooling needed), as a standalone `hlua` CLI for CI, and at server startup (auto-detect drift between manifest and generated files, warn loudly). Note the deliberate limitation inherited from GoldSrc: `kb_act.lst` and friends are read by the *engine* at menu time, so bind/cvar-UI changes need a game restart - the generator tells you when.

## 14. Coexistence with other mods

You flagged this as undecided; here are the options with evidence, and a recommendation.

**First, a distinction that dissolves half the worry:** "using another mod" usually means (a) playing its *content* - maps, models, sounds - or (b) running its *code* - its custom DLLs. HLua-the-mod plays all standard HL content forever (maps are data; the engine loads them regardless). What it cannot do is run another mod's *compiled game logic* simultaneously, because a mod directory has exactly one server DLL and one client DLL. That's engine physics, not an HLua choice.

**Option A - HLua is its own mod (fork of hlsdk-portable).** Full power, zero friction, everything in this doc works as designed. Cost: while running HLua you're not running Counter-Strike's or Sven's DLLs. Your "cloned repo" instinct is already this option.

**Option B - HLua as an embeddable library.** The core is a static lib with a 3-call API (`Startup`, `Frame`, `Shutdown`); any mod built on hlsdk-portable links it and gains a `lua/` tree. Cost to us: zero (same code). Cost to adopting mods: they integrate ~30 lines. This is how HLua spreads through the *portable-SDK* ecosystem, where mod teams already maintain builds.

**Option C - the overlay: what you called "spoof and hook into the already compiled DLL's known surface".** This exists, and it is exactly Metamod (server) and MetaHook (client):

- *Server side is proven.* Metamod sits between engine and game DLL, forwarding and intercepting both function tables; AMX Mod X ran a scripting ecosystem on it for two decades, and NodeMod embeds a whole Node.js VM through it today. But - decisive for your targets - **every Metamod fork is currently i386-only**. FWGS maintainers, Sept 2025: "All of Metamod forks are locked only for i386 architecture from the ground up... nobody did [cross-platform] yet" ([xash3d-fwgs #2319](https://github.com/FWGS/xash3d-fwgs/issues/2319)); their Dec 2025 status post reiterates that Metamod/AMX Mod X lack LP64 support while "FWGS and most of mods (thankfully to hlsdk-portable) support LP64-compatible architectures for years" ([blog](https://snmetamorph.github.io/posts/tags/xash-3-d)). The cross-platform attempt ([metamod-fwgs](https://github.com/FWGS/metamod-fwgs)) is real but early. An overlay strategy would forfeit your arm32/arm64 goals on day one.
- *Client side exists but is Windows-centric engine surgery.* [MetaHookSv](https://github.com/MetaHookSv) (active, Oct 2026) hooks the GoldSrc client without touching game files - via engine internals, with all the ABI sensitivity that implies. Feasible, maintained by people who enjoy it, and a permanent maintenance tax relative to owning the DLL.

There's also an API-shape cost even where the overlay works: through Metamod you see *edicts and engine functions*, not the SDK's C++ classes - no clean `CBaseEntity` virtuals, no gamerules objects. You'd be writing an AMX Mod X successor, not a GMod successor.

**Recommendation:** ship **A**, architect for **B** (the boundary header in section 7 is exactly the seam a host mod needs), and keep **C** as a post-M2 experiment *if* metamod-fwgs matures on ARM - the adapter would be a thin Metamod plugin exposing the same `hlua` API at engine level. This is also the honest answer to "is the spoofing route viable or a headache": viable and battle-tested on i386 servers, a headache everywhere else.

## 15. Platforms, distribution, licensing, security

**Platform matrix** (targets you named in bold):

| Target | Engine | Server | Client | Notes |
|---|---|---|---|---|
| **Windows x86** | GoldSrc | x | x | the only GoldSrc client that exists; MSVC build, `GOLDSOURCE_SUPPORT=ON` |
| **Windows x86/x64** | Xash3D | x | x | same source; x64 via `-D64BIT=1` |
| **Linux x86** | GoldSrc | x (dedicated) | - | GoldSrc client is Windows-only; build in Steam Runtime scout chroot like upstream CI |
| **Linux x86/x64** | Xash3D | x | x | primary dev loop; `xashds` dedicated server is the free headless test target |
| **Linux arm32 / arm64** | Xash3D | x | x | upstream README: arm and FreeBSD explicitly supported; SBCs, Android-derived environments |
| Android arm32/arm64 | Xash3D | x | x | rides upstream's gradle build - free if the SDK builds, tested at M2 |
| Switch / Vita / PS4 | Xash3D | later | later | toolchains exist in-tree (devkitPro, VitaSDK, OpenOrbis); Lua builds everywhere; explicit non-goal until someone asks |

**Distribution:** GitHub Releases with per-platform archives (the [hlsdk-mega-build](https://github.com/FWGS/hlsdk-mega-build/releases/tag/continuous) pattern), each containing a ready mod folder: `dlls/` + `cl_dlls/` binaries, `lua/` stdlib, generated configs, LICENSE files. End modders never build anything - they copy a folder and write Lua. CI: extend upstream's matrix (Linux gcc/clang in Steam Runtime, Windows MSVC) plus arm64/arm cross builds and a smoke test that boots `xashds` with an HLua mod and runs scripted assertions headlessly.

**Licensing:** HLua core (`hlua/`) - MIT, your copyright. SDK-derived files remain under Valve's **Half-Life 1 SDK License** (free distribution in source and object form, LICENSE file must ship, commercial use needs Valve's blessing - read directly from the repo's LICENSE). Lua 5.4 and sol2 - MIT. The Xash3D FWGS engine is GPLv3 but loads game libraries at arm's length; the entire non-free HLSDK mod ecosystem already runs on it, and we change nothing about that boundary.

**Security posture for v1:**

- Scripts are **install-time content**, like every GoldSrc mod DLL today. Neither engine auto-downloads DLLs, and HLua v1 adds no script-download mechanism - a malicious server cannot push Lua to clients through us. (Xash3D does have an HTTP download path for *content*; we explicitly do not register `lua/` as downloadable.)
- Best-effort sandbox mode (`hlua_sandbox 1`): restricted environment without `os`, `io`, `debug`, package loading - for running third-party scripts you don't trust with your homedir. Documented as best-effort, not a hard boundary; plain Lua 5.4 with a whitelist is decent but not a security VM.
- Server operators running their own scripts are, as with AMX Mod X, running their own code - no additional threat model.

## 16. Performance, save/restore, determinism

**Performance.** The 1998 engine budget is generous for 2026 Lua. Design rules that keep it that way:

- The hook bus keeps a bitmask of active hooks per seam - an un-hooked seam costs one bit test in C++, nanoseconds per frame.
- Hot paths (`StartFrame`, `PM_Move`, `Think` storms) use pre-marshalled argument buffers; no per-call table allocation; `vec3_t` maps to a plain Lua table with cached C-side conversion, not a usertype allocation per access.
- Lua 5.4's **generational GC** stepped per frame (`lua_gc(LUA_GCSTEP)` sized by a cvar); movement scripts avoid garbage by construction (the jetpack sample allocates nothing per move).
- Order-of-magnitude sanity, clearly labeled as *estimate pending M0 benchmarks*: a sol2 protected call with a couple of arguments is sub-microsecond; a 32-player server at 60 Hz with, say, 300 active hooks fires on the order of 20k script calls/sec - single-digit milliseconds of frame time. The M0 milestone includes a micro-benchmark harness so we replace this paragraph with numbers.
- One real hazard: per-entity `Think` loops with tiny think intervals (100 monsters x 0.1 s). Mitigation: `NextThink` coalescing guidance in docs, and the profiler (`hlua_profile 1` prints per-hook cost) to shame hot scripts.

**Save/restore.** GoldSrc savegames flow through `CSave`/`CRestore` with `TYPEDESCRIPTION` tables (visible in `engine/eiface.h` - the save system is part of the game/engine contract). The carrier entity implements `Save`/`Restore` by serializing the manifest-declared `Fields` into a single string field. Limits, stated up front: scripted state survives same-version reloads; cross-version savegame compat is best-effort (fields matched by name, missing fields defaulted - the R2 rehydration mechanism, reused); and HLua *itself* changing between save and load is unsupported (same as any mod changing its DLL). Single-player HLua mods therefore pin versions per campaign - acceptable, documented.

**Determinism** is section 12's contract: shared-tree hash handshake, seeded RNG, linted shared code.

## 17. Tooling, docs, and the agent story

You want mods writable by "agents and humans" - which means the docs *are* the product as much as the runtime.

- **One binding registry, three outputs.** Every bound function/table is a row with name, signature, tier, and a docs string (section 7). From it we generate: (a) the `docs/api/` markdown reference, (b) `api.d.lua` annotation files for [Lua Language Server](https://luals.github.io) - autocomplete and type checking in VS Code for modders, and (c) the in-game `hlua_apidump` reference. Never hand-maintained, never stale.
- **The `hlua` CLI and console commands are the same tool**: `hlua new <mod>`, `hlua gen` (manifest -> configs/FGD), `hlua doctor` (lint: determinism rules, manifest drift, missing assets), `hlua docs`. Everything also runs *in-game* (`hlua_gen`, `hlua_doctor`) so a modder with just the game installed is never blocked.
- **Examples repo as the tutorial:** five small, complete, commented mods - jetpack (movement), a custom pickup (entity), a HUD mutator (client), an admin command pack (server), a scripted scripted_sequence-style cutscene (composition). The jetpack example *is* your original mod, ported; that's the first thing anyone reads.
- **Agent-friendly by construction:** stable markdown API reference, deterministic load order, typed annotations, `doctor` as an automated feedback loop, and one-file-per-convention layout - everything an LLM needs to write a correct mod without a human in the loop, and everything a human needs to read what the agent wrote.
- **CI as documentation:** every example mod runs headlessly on `xashds` per commit with scripted assertions (spawn this, move there, expect that). When examples rot, the build says so.

## 18. MVP ladders and recommendation

You said MVP scope was undecided - so here's a ladder where every rung is shippable and each one answers one of the open questions with evidence instead of speculation.

**M0 - "Hello, GoldSrc" (server-only proof).**
Lua 5.4 builds under both waf and CMake; backend TU compiles with `SOL_NO_EXCEPTIONS/RTTI`; `lua/autorun` loads on Linux `xashds` *and* GoldSrc Windows dedicated; `print`, `hlua_version`, `hook.Add("StartFrame")`, one cvar; clean shutdown; micro-benchmark harness. **Exits when:** a stranger can drop a `.lua` in a folder on a dedicated server and see output, and we have real numbers for section 16's estimates.

**M1 - the jetpack milestone (the one that matters).**
Movement bridge + `hook.Add("PlayerMove")`; `pmove_t` usertype; cvars, concommands; one entity override (`func_breakable`); one net message; client-side autorun on Xash3D + HUD text; `kb_act.lst` generation for the jetpack bind. **Acceptance test: your 40-line C++ jetpack, deleted; `lua/shared/jetpack.lua` running instead - bindable in Options -> Keyboard, on GoldSrc Windows and Xash3D Windows/Linux, server and prediction both correct.** If this milestone works, the architecture is proven end-to-end on every axis you care about: server, client, shared movement, manifest -> keybinds.

**M2 - modder's release.**
Manifest v1 complete (`kb_act`/`kb_def`, `user.scr`, `settings.scr`, FGD with carrier-pool mapping, liblist patching); `ents.Register` new entities through the carrier pool; hot reload R1+R2; docs generation + `api.d.lua`; example pack (5 mods); release binaries for win32, linux x86/x64, arm32/arm64; headless CI smoke tests.

**M3+ (explicitly not promised):** VGUI bridge, save/restore for scripted fields, predicted weapons for scripts, gamemode layer, Metamod adapter experiment (contingent on metamod-fwgs ARM maturity), package signing.

**Recommendation:** M0 immediately, M1 next, then *decide M2's shape from what M1 taught you* - in particular whether the carrier-pool entity story feels right to mappers, which is the one design here no amount of analysis can fully settle.

## 19. Risks and open questions

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Upstream SDK churn makes the fork painful | Medium | Medium | patch surface is ~30 lines in 7 files (section 6); rebase monthly, CI diffs the patch set |
| sol2 no-exceptions mode bites in an untested way | Medium | Medium | M0 exercises it on day one; raw-C-API fallback behind an unchanged `hlua.h` |
| GoldSrc quirks (32-bit only client, no script download) constrain distribution | Certain | Low | platform matrix states it; Xash3D covers everything GoldSrc can't |
| Movement prediction desync confuses modders | Medium | High (UX) | connect-time hash handshake + `hlua doctor` lint rules (section 12) |
| Savegame breakage in scripted campaigns | Medium | Medium | `Fields` versioning policy (section 16); single-player mods pin versions |
| Carrier-pool entities confuse mappers | Medium | Medium | FGD hides it; M1/M2 feedback loop; worst case: codegen-at-package-time alternative |
| Scope creep toward rebuilding GMod | High | High | the ladder says no; every feature lands with an example mod, not a promise |
| Name collision | Resolved | - | checked: no Half-Life project named HLua exists; collisions are unrelated domains (a Rust crate, a water utility, HAProxy) |

**Open questions (yours to call, section 20):** coexistence strategy pacing; hot-reload depth target for v1 (R1 only vs R1+R2); manifest format (TOML proposed vs JSON vs Lua); API naming convention (GLua-familiar proposed); carrier pool size; whether M2 includes Android binaries or defers.

## 20. Decisions requested from you

1. **Coexistence:** adopt "A now, B by design, C deferred" (section 14)? Or is Metamod-class coexistence a hard requirement that changes the architecture?
2. **MVP:** approve the M0 -> M1 ladder with the jetpack as M1's acceptance test?
3. **Manifest format:** TOML as proposed?
4. **API naming:** GLua-familiar (`hook.Add`, `net.Start`, `ents.Register`) as the contract, accepting that renaming later is a compatibility break?
5. **Entity story:** carrier pool (32 exports) with FGD mapping - acceptable for v1, or do you want the codegen-at-package-time variant specced now?
6. **Hot reload target:** R1 only for M2, or R1+R2?

## 21. References

- FWGS/hlsdk-portable - <https://github.com/FWGS/hlsdk-portable> (cloned and measured for this document; stats in Appendix A)
- FWGS/xash3d-fwgs - <https://github.com/FWGS/xash3d-fwgs>; Metamod-on-ARM discussion: issue #2319 - <https://github.com/FWGS/xash3d-fwgs/issues/2319>
- FWGS/metamod-fwgs - <https://github.com/FWGS/metamod-fwgs>
- SNMetamorph, Xash3D FWGS status posts - <https://snmetamorph.github.io/posts/tags/xash-3-d>, <https://snmetamorph.github.io/posts/xash3d-fwgs-dec-2024>
- NodeMod - <https://nodemod.org/docs>
- Sven Co-op AngelScript - wiki: <https://wiki.svencoop.com/Angelscript>; API docs: <https://sven-coop.github.io>; server-plugin guide: <https://steamcommunity.com/sharedfiles/filedetails/?id=605607881>; script collection: <https://github.com/Mikk155/Sven-Co-op>
- MetaHook / MetaHookSv - <https://github.com/MetaHookSv>, <https://github.com/nagist/metahook>; svenmod - <https://github.com/sw1ft747/svenmod>
- Garry's Mod 64-bit branch & LuaJIT - <https://steamcommunity.com/sharedfiles/filedetails/?id=2882777333>
- Valve Developer Community - liblist.gam: <https://developer.valvesoftware.com/wiki/Liblist.gam>; kb_act.lst: <https://developer.valvesoftware.com/wiki/Customizing_Options:_Keyboard>; Xash3D: <https://developer.valvesoftware.com/wiki/Xash3D>; GoldSrc: <https://developer.valvesoftware.com/wiki/GoldSrc>
- TWHL wiki - settings.scr/user.scr: <https://twhl.info/wiki/page/settings.scr_and_user.scr>; file types (GoldSrc keybind file locations): <https://twhl.info/wiki/page/File_Types_and_Formats>; liblist.gam: <https://twhl.info/wiki/page/liblist.gam>
- AlliedMods - liblist.gam: <https://wiki.alliedmods.net/Liblist.gam>
- Half-Life 1 SDK License - as shipped in the hlsdk-portable repository (`LICENSE`); historical text corroborated at <https://github.com/twhl-community/HalfLifeSDKHistory>
- hlsdk-mega-build (multi-platform CI artifacts) - <https://github.com/FWGS/hlsdk-mega-build/releases/tag/continuous>

---

## Appendix A: SDK statistics (measured 2026-11-11, `FWGS/hlsdk-portable` master)

Counts produced by a script over the cloned tree (C/C++/header files, physical lines; script kept in the project for re-runs after rebases).

**Per-module:**

| Directory | Files | LOC |
|---|---:|---:|
| `dlls/` | 147 | 95,584 |
| `cl_dll/` | 94 | 36,469 |
| `external/` | 24 | 6,443 |
| `utils/` | 80 | 5,064 |
| `pm_shared/` | 9 | 4,586 |
| `game_shared/` | 24 | 3,327 |
| `common/` | 33 | 3,105 |
| `engine/` | 15 | 2,467 |
| `public/` | 3 | 330 |
| **Total** | **429** | **157,375** |

**API surface:**

| Interface | Members | Defined in |
|---|---:|---|
| `enginefuncs_t` | 161 | `engine/eiface.h` |
| `cl_enginefunc_t` | 131 | `engine/cdll_int.h` |
| `DLL_FUNCTIONS` | 50 | `engine/eiface.h` |
| `NEW_DLL_FUNCTIONS` | 5 | `engine/eiface.h` |
| Client exports (`HUD_*` etc.) | ~30 | `cl_dll/cdll_int.cpp` |
| Entity classnames (`LINK_ENTITY_TO_CLASS`) | 251 | across `dlls/` |
| `CBase*`-derived classes | ~166 | across `dlls/` |
| `UTIL_*` functions | 84 | across `dlls/` |
| `PM_*` functions | 56 | `pm_shared/` |
| `playermove_t` fields | ~62 | `pm_shared/pm_defs.h` |

**The 56 `PM_*` functions** (the movement surface): PM_Accelerate, PM_AddCorrectGravity, PM_AddGravity, PM_AddToTouched, PM_AirAccelerate, PM_AirMove, PM_CalcRoll, PM_CatagorizePosition, PM_CatagorizeTextureType, PM_CheckFalling, PM_CheckParamters, PM_CheckStuck, PM_CheckVelocity, PM_CheckWater, PM_CheckWaterJump, PM_ClipVelocity, PM_CreateStuckTable, PM_DropPunchAngle, PM_Duck, PM_FixPlayerCrouchStuck, PM_FixupGravityVelocity, PM_FlyMove, PM_Friction, PM_GetPhysEntInfo, PM_GetRandomStuckOffsets, PM_GetVisEntInfo, PM_InWater, PM_Init, PM_InitTextureTypes, PM_Jump, PM_LadderMove, PM_MapTextureTypeStepType, PM_NoClip, PM_ParticleLine, PM_Physics_Toss, PM_PlayStepSound, PM_PlayWaterSounds, PM_PlayerMove, PM_PreventMegaBunnyJumping, PM_ReduceTimers, PM_ResetStuckOffsets, PM_ShowClipBox, PM_SortTextures, PM_SpectatorMove, PM_SplineFraction, PM_SwapTextures, PM_UnDuck, PM_UpdateStepSound, PM_ViewEntity, PM_WalkMove, PM_WaterJump, PM_WaterMove (plus debug/draw helpers).

**Key entry points (verified locations used by section 6's patch table):**

| Symbol | File:line |
|---|---|
| `LINK_ENTITY_TO_CLASS` macro | `dlls/util.h:110` |
| `GiveFnptrsToDll` | `dlls/h_export.cpp:52` |
| `GetEntityAPI` / `GetEntityAPI2` | `dlls/cbase.cpp:106` / `:117` |
| `GameDLLInit` | `dlls/game.cpp:468` |
| `StartFrame` | `dlls/client.cpp:807` |
| `Initialize` / `HUD_Init` / `HUD_Redraw` | `cl_dll/cdll_int.cpp:148` / `:223` / `:239` |
| `IsXashFWGS` | `cl_dll/cdll_int.cpp:341` |
| `PM_PlayerMove` / `PM_Move` / `PM_Init` | `pm_shared/pm_shared.c:3016` / `:3376` / `:3421` |

**Build flags evidence:** `wscript`: `conf.env.append_unique('CXXFLAGS', ['-Wno-invalid-offsetof', '-fno-exceptions'])`; `dlls/CMakeLists.txt` and `cl_dll/CMakeLists.txt`: `add_compile_options(-fno-exceptions)` and `add_compile_options(-fno-rtti)` (GCC/Clang). GitHub Actions `build.yml`: Linux (gcc, clang) inside Valve Steam Runtime scout i386 chroot + Windows x86 MSVC; broader platforms via hlsdk-mega-build.

**Engine abstraction:** runtime detection via `IsXashFWGS()`; split implementations `cl_dll/input_goldsource.cpp` / `cl_dll/input_xash3d.cpp`; only three compile-time `XASH_*` macros exist (`XASH_BIG_ENDIAN`, `XASH_BUILD_BRANCH`, `XASH_BUILD_COMMIT`).
