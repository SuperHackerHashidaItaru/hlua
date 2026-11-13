# apply plan

What exists, what to do with it and in which order, and what each step
is for. Commands are examples: type everything your own way.

## what exists right now

QaisAlsaid/hlua (your repo) sits at 89e83b9, the upstream sdk tip.
Collab main is your tip plus a short series. Apply the code commits,
skip every commit that only touches _collab/ (their subjects start
with "collab:"):

code, apply these:

- 1eb72b5  external: vendor lua 5.4.9 as static library
  65 files. The lua sources under external/lua54, three subproject
  lines in wscript, one add_subdirectory in CMakeLists.txt
- 5cc1f5a  hlua: script runtime with hooks, timers and scripted entities
  The hlua/ module (about 1500 lines) plus the sdk integration:
  113 insertions and 10 deletions across ten files
- 49b4778  tests: headless server library test and scripted test mod
  tests/hlua_test.cpp loads the built server library through a fake
  engine and drives it, tests/mod is the scripted test content
- 07336e9  readme: describe the project
- cc05a34  hlua: use space indent in hooks.cpp

collab only, skip these:

- 4e2a3dc, 019cb32, 8f69410, 610c1d6 and any later "collab:" commit

Shas move whenever history is rebuilt. Identify commits by subject.

Nothing else is touched: no cl_dll changes, no engine headers, the
freevgui submodule pointer is unchanged.

The ten sdk files, so you know where to look:

    .gitignore            build*/ pattern
    CMakeLists.txt        lua54 subdir, BUILD_TESTS option
    wscript               lua54 in the three subproject lists
    dlls/CMakeLists.txt   hlua sources, lua54 include and link
    dlls/wscript          same for waf
    dlls/cbase.h          one member, one virtual
    dlls/cbase.cpp        spawn guard dispatch, entity callbacks
    dlls/client.cpp       player hooks, say filter
    dlls/game.cpp         dispatch calls
    dlls/util.h           the hlua namespace the sdk sees

## step 1: review

    git clone https://github.com/SuperHackerHashidaItaru/hlua.git hlua-review
    cd hlua-review

Read in this order:

1. _collab/design.md: what is built, decisions D1 to D8, open questions
2. _collab/roadmap.md: what comes next
3. _collab/discussions/2026-11-11-open-questions.md: the reasoning
   behind D4 to D6
4. the code: hlua/hlua_internal.h, core.cpp, prelude.h, hooks.cpp,
   entity.cpp

Diffs per commit:

    git show 5cc1f5a

or everything your repo would take:

    git diff 89e83b9..main -- dlls hlua wscript CMakeLists.txt

## step 2: apply to QaisAlsaid/hlua

Option A, cherry-pick (fastest):

    git clone git@github.com:QaisAlsaid/hlua.git hlua
    cd hlua
    git remote add collab https://github.com/SuperHackerHashidaItaru/hlua.git
    git fetch collab
    git cherry-pick 1eb72b5 5cc1f5a 49b4778 07336e9 cc05a34
    git push

Skip the _collab/ commits. If a cherry-pick conflicts, your fork moved
past 89e83b9: say so and the series gets rebased for you.

Option B, type it yourself (the point of this project):

- 1eb72b5 is a vendor drop. Take it as is, no value in retyping lua
- 5cc1f5a is the one worth typing. Order that worked when writing it:
  1. hlua/hlua_internal.h, the surface the sdk sees
  2. hlua/core.cpp, vm lifecycle, script loading, console
  3. hlua/prelude.h, the pure lua part, the timer library
  4. hlua/hooks.cpp, hook registry, sdk adapters
  5. hlua/entity.cpp, entity bridge, carrier, spawn guard
  6. the sdk touch points, smallest first: game.cpp, cbase.h, cbase.cpp,
     util.h, client.cpp, build files
  The collab repo is the reference: diff your typing against it as
  you go
- 49b4778: take it or retype, the fake engine is the interesting part
- 07336e9: your readme, your words

## step 3: build and test

cmake. Linux x86_64 needs -D64BIT=ON, upstream defaults to 32 bit:

    cmake -G"Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -D64BIT=ON \
      -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=ON \
      -B build-cmake -S .
    cmake --build build-cmake
    ctest --test-dir build-cmake --output-on-failure

waf:

    python3 waf configure -T release -o build-waf -8
    python3 waf build -o build-waf

Expected: ctest passes. The direct run, here against the waf build,
prints "TEST: all checks passed":

    ./build-cmake/tests/hlua_test build-waf/dlls/hl_amd64.so tests/mod

## step 4: sync back

After your version is public, the agent reads QaisAlsaid/hlua directly
and resyncs collab to match. Or push to collab yourself, you have
write access. Then the next roadmap item: entity fields round 2 or CI,
your call.
