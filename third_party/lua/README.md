# Lua 5.4.7

Unmodified runtime sources and headers from https://www.lua.org/ftp/lua-5.4.7.tar.gz.
Archive SHA-256: `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`.
The standalone interpreters (`lua.c`, `luac.c`) are omitted.

The root Makefile compiles these unmodified sources as C++ into a static archive and links it into
3Beans on Windows, macOS, and Linux. No system Lua installation is needed.
C++ exceptions unwind host resources safely on Lua errors; `../lua_cpp.h` preserves
the public C ABI. Linux exports only the Lua API for native modules.
See LICENSE for the upstream MIT license.
