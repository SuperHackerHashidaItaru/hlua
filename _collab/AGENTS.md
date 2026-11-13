# AGENTS.md

Working guide for the AI collaborator (Z.ai). Read fully at session
start, then read the latest worklog entries and the open questions in
design.md before touching anything.

## What this is

HLua: Lua 5.4 embedded in the Half-Life SDK (hlsdk-portable fork),
GLua-like api, GoldSrc and Xash3D targets, no engine fork.

Repos:

- main: github.com/QaisAlsaid/hlua, the user's curated fork. Never
  push there, never write to it in any way.
- collab: github.com/SuperHackerHashidaItaru/hlua, this repo. Shared
  workspace: reference code, docs, worklog, discussions.

The user applies reviewed work to main by typing it themselves, then
syncs main back here (code and public docs). _collab/ never lands in
main. The user is a collaborator, not a patch applier: design and code
happen together, in steps small enough to type.

## Session bootstrap

1. Working copy: /home/z/my-project/work/hlua (git, branch master,
   full history, origin = main repo)
2. Collab remote "collab" embeds the PAT stored at
   /home/z/my-project/.secrets/github.pat (fine grained, rw, collab
   repo only, given by the user for this purpose). If missing:
   git remote add collab https://x-access-token:$(cat /home/z/my-project/.secrets/github.pat)@github.com/SuperHackerHashidaItaru/hlua.git
3. Push with: git push collab master:main
4. Commit identity: git config user.name "Z.ai", user.email "zai@localhost"
5. Never commit secrets or absolute sandbox paths. Verify with
   git diff --staged before every commit.

## Working agreement

- present work as small steps the user can type; the repo is the
  reference implementation and the fallback, chat is the explanation
- mechanical work (vendoring, mirrors, formatting repair, ci yaml) may
  be committed directly, still in small reviewable commits
- every session ends with a worklog entry in
  _collab/worklog/YYYY-MM-DD.md, including honest notes on mistakes
- if the working copy disagrees with this guide, trust the worklog

## Style rules (hard, code and docs)

- small code, no bloat, no speculative features
- no big comment blocks unless absolutely necessary
- ASCII only: no em dashes, no smart quotes, no section symbols, no
  emoji, no unicode arrows
- no references to design docs, milestones or task ids in code,
  comments or commit messages
- new hlua code follows hlua/*.cpp style: 8 space indent, if( )
  spacing, one statement per line
- edits inside sdk files match the surrounding file style instead
- never reindent or reformat whole sdk files, keep diffs minimal

## Layout

    hlua/            runtime sources (core, hooks, entity bridge)
    external/lua54/  vendored lua 5.4.9
    tests/           headless test, tests/mod is the scripted test mod
    _collab/         this guide, design.md, roadmap.md, discussions,
                     worklog, proposal-v1.md (historical)

## Build and test quick reference

waf (64 bit, out dir build-waf):

    python3 waf configure -T release -o build-waf -8
    python3 waf build -o build-waf

cmake (upstream defaults to 32 bit on x86_64 linux, sandbox needs
-D64BIT=ON):

    cmake -G"Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -D64BIT=ON \
      -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=ON \
      -B build-cmake -S .
    cmake --build build-cmake
    ctest --test-dir build-cmake

Or compile the harness by hand:

    g++ -O1 -o /tmp/hlua_test tests/hlua_test.cpp -I dlls -I common \
      -I engine -I pm_shared -I game_shared -I public -I hlua \
      -I external/lua54 -ldl
    /tmp/hlua_test build-waf/dlls/hl_amd64.so tests/mod

Expected output ends with: TEST: all checks passed
