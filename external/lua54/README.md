# Lua 5.4.9

Unmodified sources from the official Lua repository, git tag v5.4.9:
https://github.com/lua/lua/tree/v5.4.9

Built as plain portable C with no LUA_USE_* platform defines, which also
means the `require` of binary modules is disabled. lua.c is only used by
the lua54i interpreter target so `tools/gen.lua` can run without a system
wide Lua install.

License: MIT, see the notice at the top of lua.h and each source file.
