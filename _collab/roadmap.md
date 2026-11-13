# Roadmap

Ordered by what unblocks mod authors soonest. Sizes are guesses.

## Done

- lua 5.4.9 vendored, waf and cmake integration
- runtime: vm, hooks, timers, entity bridge with native class
  replacement, hot reload
- headless test suite driving the built library through a fake engine
- collab repo bootstrapped, design decisions recorded

## Next

1. entity fields round 2 (small)
   user slot passthrough (iuser1-4, fuser1-4, vuser1-4, euser1-4,
   euserN as entity values), ents.FindByName over targetname, more
   field coverage (rendermode, renderamt, rendercolor, gravity,
   friction, takedamage, dmg)

2. ci (medium)
   adapt .github/workflows from upstream hlsdk-portable and
   xash3d-fwgs:
   - keep the upstream matrix: linux steamrt i386 with gcc and clang,
     windows x86 and x64
   - add android arm32 and arm64 (xash3d-fwgs drives the ndk
     directly, see their android workflows)
   - add linux arm64 for 64 bit xash targets
   - add a test job: cmake -D64BIT=ON -DBUILD_TESTS=ON then ctest
     (upstream cmake defaults to 32 bit on x86_64 linux, the test
     harness is 64 bit clean either way when built with 64BIT=ON)
   - add a release workflow: on tag, build all targets, attach to a
     github release, naming hlua-<tag>-<target>
   - release artifacts: runtime libraries per target now, dev kit
     (gen tool, fgd, stubs) when those exist

3. gen tool (medium)
   hlua-gen cli: sandboxed lua state, GEN flag, loads entity defs
   from the mod, emits hlua.fgd for map editors. Optional glue
   emission for mods that want real class names compiled in.

4. example mod (small)
   grow tests/mod into examples/: a scripted npc, a gameplay tweak,
   a say filter, something runnable in xash right away

5. api docs and editor stubs (small)
   one readme per library, a lua-language-server definition file
   generated from the bindings

6. client side (large, later)
   second vm in the client library, shared scripts, usermessage
   bridge, CLIENT flag

7. save/restore of script state (medium, later)
   per class keep list of table keys, serialized through the sdk
   save/restore system
