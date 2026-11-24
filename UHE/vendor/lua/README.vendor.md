# Lua 5.4.7 (vendored)

Source: https://www.lua.org/ftp/lua-5.4.7.tar.gz — `src/*.c`, `src/*.h`, minus the
standalone interpreter and compiler (`lua.c`, `luac.c`, `onelua.c`), which are not
part of a library build. `README.upstream.md` is upstream's own.

License: MIT (see `../lua/LICENSE` upstream text in `README`); Lua is distributed
under the same MIT license as this engine.

Built as a static `lua` target with no `LUA_USE_*` defines: the engine's sandbox
does not open `os`, `io` or `package`, so the POSIX/dlopen extras those defines
add are not needed and the build stays identical on every platform.

`scripting.md` describes the embedding; `UHE/src/UHE/Scripting/ScriptEngine.cpp`
is the only place that talks to the C API (through sol2).
