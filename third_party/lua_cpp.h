// Preserve the public C ABI while using C++ exceptions for Lua errors.
#include "lua/lprefix.h"
#define LUA_CORE
extern "C" {
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
}
