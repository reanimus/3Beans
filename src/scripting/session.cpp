#include "session.h"
#include "../core/core.h"
#include "../core/firm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <png.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#if LUA_VERSION_NUM < 504
#error "3Beans scripting requires Lua 5.4 or newer"
#endif
#ifdef WINDOWS
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {
std::string str(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TSTRING)
        throw std::runtime_error("Expected a string");
    size_t size;
    const char* s = lua_tolstring(L, index, &size);
    return std::string(s, size);
}
uint64_t integer(lua_State* L, int index, uint64_t max = UINT32_MAX) {
    int valid = 0;
    lua_Integer value = lua_tointegerx(L, index, &valid);
    valid = valid && lua_type(L, index) == LUA_TNUMBER;
    if (!valid || value < 0 || uint64_t(value) > max)
        throw std::runtime_error("Argument " + std::to_string(index - 1) + ": integer out of range");
    return uint64_t(value);
}
int signedInteger(lua_State* L, int index, int min, int max) {
    int valid = 0;
    lua_Integer value = lua_tointegerx(L, index, &valid);
    valid = valid && lua_type(L, index) == LUA_TNUMBER;
    if (!valid || value < min || value > max)
        throw std::runtime_error("Argument " + std::to_string(index - 1) + ": integer out of range");
    return int(value);
}
int functionRef(lua_State* L, int index) {
    if (!lua_isfunction(L, index))
        throw std::runtime_error("Expected a callback function");
    lua_pushvalue(L, index);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}
void pushString(lua_State* L, const std::string& s) {
    lua_pushlstring(L, s.data(), s.size());
}
void field(lua_State* L, const char* name, uint64_t value) {
    lua_pushinteger(L, lua_Integer(value));
    lua_setfield(L, -2, name);
}
const char* cpuNames[] = {"ARM11A", "ARM11B", "ARM11C", "ARM11D", "ARM9"};
int cpuByName(const std::string& name) {
    for (int i = 0; i < MAX_CPUS; ++i)
        if (name == cpuNames[i])
            return i;
    throw std::runtime_error("Unknown CPU: " + name);
}
void validPathName(const std::string& name) {
    if (name != "sd" && name != "nand" && name != "boot9" && name != "boot11" && name != "firm")
        throw std::runtime_error("Unknown boot path: " + name);
}
void readable(const std::string& path, bool writable, size_t minimum = 0) {
    FILE* f = fopen(path.c_str(), writable ? "rb+" : "rb");
    if (!f)
        throw std::runtime_error("Cannot open " + path + (writable ? " for reading and writing" : " for reading"));
    bool valid = true;
    if (minimum) {
        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        valid = length >= 0 && uint64_t(length) >= minimum;
    }
    fclose(f);
    if (!valid)
        throw std::runtime_error("File is too short: " + path);
}
int traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "Lua error (non-string value)", 1);
    return 1;
}
} // namespace

ScriptSession::ScriptSession(bool headless) : headless(headless) {
    output = [](const std::string& text) { fprintf(stdout, "%s\n", text.c_str()); };
}
ScriptSession::~ScriptSession() {
    core.reset();
    if (lua)
        lua_close(lua);
}
std::string ScriptSession::absolutePath(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos)
        throw std::runtime_error("Expected a nonempty filesystem path");
#ifdef WINDOWS
    char* full = _fullpath(nullptr, path.c_str(), 0);
    if (!full)
        throw std::runtime_error("Cannot resolve path");
    std::string result(full);
    free(full);
    return result;
#else
    if (path[0] == '/')
        return path;
    char* cwd = getcwd(nullptr, 0);
    if (!cwd)
        throw std::runtime_error("Cannot resolve current directory");
    std::string result = std::string(cwd) + "/" + path;
    free(cwd);
    return result;
#endif
}
void ScriptSession::setPath(const std::string& name, const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    validPathName(name);
    std::string absolute = absolutePath(path);
    readable(absolute, name == "sd" || name == "nand", (name == "boot9" || name == "boot11") ? 0x10000 : 0);
    overrides[name] = absolute;
}
void ScriptSession::clearPath(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    validPathName(name);
    overrides.erase(name);
}
std::map<std::string, std::string> ScriptSession::paths(const std::string& kind) const {
    if (kind == "mounted") {
        if (!core)
            return {};
        const BootConfig& b = core->bootConfig;
        return {{"sd", b.sd}, {"nand", b.nand}, {"boot9", b.boot9}, {"boot11", b.boot11},
            {"firm", b.firm ? b.firm->path : ""}};
    }
    std::map<std::string, std::string> result = {{"sd", Settings::sdPath},
                                                 {"nand", Settings::nandPath},
                                                 {"boot9", Settings::boot9Path},
                                                 {"boot11", Settings::boot11Path}, {"firm", ""}};
    if (kind == "effective")
        for (auto& entry : overrides)
            result[entry.first] = entry.second;
    return result;
}
void ScriptSession::setTimeout(double seconds) {
    deadline = seconds > 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000))
                           : std::chrono::steady_clock::time_point{};
}
void ScriptSession::checkCancelled() {
    if (cancelled.load())
        throw std::runtime_error("Script execution cancelled");
    if (deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Script execution timed out");
}
Core& ScriptSession::requireCore() {
    if (!core)
        throw std::runtime_error("No running core; call emu:start() first");
    return *core;
}
void ScriptSession::requireOutsideCallback() {
    if (callbackDepth)
        throw std::runtime_error("Execution and core replacement are not allowed inside callbacks");
}
void ScriptSession::start(bool reset) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    requireOutsideCallback();
    checkCancelled();
    if (core && !reset)
        return;
    auto p = paths("effective");
    readable(p["boot9"], false, 0x10000);
    readable(p["boot11"], false, 0x10000);
    for (auto& entry : overrides)
        readable(entry.second, entry.first == "sd" || entry.first == "nand");
    if (!cartPath.empty())
        readable(cartPath, false);
    BootConfig config;
    config.sd = p["sd"];
    config.nand = p["nand"];
    config.boot9 = p["boot9"];
    config.boot11 = p["boot11"];
    if (!p["firm"].empty()) config.firm = std::make_shared<FirmImage>(p["firm"]);
    config.headless = headless;
    config.forceSoftware = headless || !context;
    config.audioPacing = !headless;
    bool existed = bool(core);
    // Validation precedes replacement; never keep stale host pointers on failure.
    {
        std::lock_guard<std::mutex> consumers(consumerMutex);
        core.reset();
        if (changed)
            changed();
        try {
            core.reset(new Core(cartPath, context, config));
        } catch (...) {
            autoRun = false;
            paused = true;
            throw std::runtime_error("Core initialization failed");
        }
    }
    std::fill_n(skipPending, MAX_CPUS, false);
    accesses.clear();
    stepCpu = -1;
    pause();
    attach();
    if (changed)
        changed();
    emit(existed ? "reset" : "start");
}
void ScriptSession::stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    requireOutsideCallback();
    autoRun = false;
    paused = true;
    if (core) {
        emit("stop");
        std::lock_guard<std::mutex> consumers(consumerMutex);
        core.reset();
        if (changed)
            changed();
    }
    accesses.clear();
    std::fill_n(skipPending, MAX_CPUS, false);
}
void ScriptSession::pause() {
    paused = true;
    autoRun = false;
}
void ScriptSession::resume() {
    requireCore();
    paused = false;
    autoRun = true;
}
void ScriptSession::refreshPipelines() {
    for (int i = 0; i < MAX_CPUS; ++i)
        core->arms[i].debugRefreshPipeline();
}
void ScriptSession::attach() {
    core->shouldYield = [this] { return cancelled.load() || std::chrono::steady_clock::now() >= quantumEnd; };
    core->beforeInstruction = [this](CpuId cpu) { return before(cpu); };
    core->afterInstruction = [this](CpuId cpu) { return after(cpu); };
    core->memoryAccess = [this](CpuId cpu, uint32_t address, unsigned width, bool write, uint32_t value) {
        accesses.push_back({cpu, core->instructionAddress, address, value, width, write});
    };
    refreshHooks();
}
void ScriptSession::refreshHooks() {
    if (!core)
        return;
    core->watchReads = core->watchWrites = 0;
    core->beforeMask = core->afterMask = 0;
    for (auto& entry : points) {
        const Point& p = entry.second;
        if (!p.type)
            core->beforeMask |= BIT(p.cpu);
        if (p.type & 2)
            core->watchReads |= BIT(p.cpu);
        if (p.type & 1)
            core->watchWrites |= BIT(p.cpu);
    }
    core->afterMask = core->watchReads | core->watchWrites;
    if (stepCpu >= 0)
        core->afterMask |= BIT(stepCpu);
    core->setDebugging(core->beforeMask || core->afterMask);
}
bool ScriptSession::before(CpuId cpu) {
    uint32_t address = core->arms[cpu].debugPc();
    if (skipPending[cpu]) {
        skipPending[cpu] = false;
        if (skipAddress[cpu] == address)
            return false;
    }
    std::vector<int> ids;
    for (auto& p : points)
        if (!p.second.type && p.second.cpu == cpu && p.second.min == address)
            ids.push_back(p.first);
    Access access{cpu, address, address, 0, 0, false};
    for (int id : ids) {
        auto it = points.find(id);
        if (it != points.end())
            call(it->second.ref, &access, id);
    }
    if (paused && !ids.empty()) {
        skipPending[cpu] = true;
        skipAddress[cpu] = address;
    }
    return paused;
}
bool ScriptSession::after(CpuId cpu) {
    skipPending[cpu] = false;
    std::vector<Access> pending;
    pending.swap(accesses);
    for (auto& access : pending) {
        std::vector<int> ids;
        for (auto& p : points)
            if (p.second.cpu == access.cpu && (p.second.type & (access.write ? 1 : 2)) &&
                uint64_t(access.address) < p.second.max && uint64_t(access.address) + access.width > p.second.min)
                ids.push_back(p.first);
        for (int id : ids) {
            auto it = points.find(id);
            if (it != points.end())
                call(it->second.ref, &access, id);
        }
    }
    if (stepCpu == cpu)
        stepDone = true;
    return paused || stepDone;
}
std::string ScriptSession::advance(int cpu, unsigned timeoutMs) {
    requireOutsideCallback();
    checkCancelled();
    uint64_t frame;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        Core& c = requireCore();
        if (cpu == ARM11C || cpu == ARM11D)
            if (!c.n3dsMode)
                throw std::runtime_error("CPU is unavailable on Old 3DS");
        frame = c.frameCounter;
        paused = false;
        stepCpu = cpu;
        stepDone = false;
        if (cpu >= 0)
            autoRun = false;
        refreshHooks();
    }
    auto stepDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string reason;
    try {
        for (;;) {
            checkCancelled();
            std::lock_guard<std::recursive_mutex> lock(mutex);
            quantumEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            uint64_t oldFrame = core->frameCounter;
            core->runFrame();
            if (core->frameCounter != oldFrame)
                emit("frame");
            if (paused) {
                reason = "paused";
                break;
            }
            if (cpu >= 0 ? stepDone : core->frameCounter != frame) {
                reason = cpu >= 0 ? "step" : "frame";
                break;
            }
            if (cpu >= 0 && std::chrono::steady_clock::now() >= stepDeadline) {
                reason = "timeout";
                break;
            }
        }
    } catch (...) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        stepCpu = -1;
        refreshHooks();
        pause();
        throw;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    stepCpu = -1;
    refreshHooks();
    if (cpu >= 0)
        paused = true;
    if (error && headless)
        throw std::runtime_error("Lua callback failed");
    return reason;
}

std::vector<ScriptSession::Domain> ScriptSession::domains() {
    Core& c = requireCore();
    std::vector<Domain> result = {{"arm9ram", c.memory.arm9Ram, 0x08000000, sizeof(c.memory.arm9Ram), true, ARM9},
                                  {"vram", c.memory.vram, 0x18000000, sizeof(c.memory.vram), true, ARM11A},
                                  {"dspwram", c.memory.dspWram, 0x1FF00000, sizeof(c.memory.dspWram), true, ARM11A},
                                  {"axiwram", c.memory.axiWram, 0x1FF80000, sizeof(c.memory.axiWram), true, ARM11A},
                                  {"fcram", c.memory.fcram, 0x20000000, sizeof(c.memory.fcram), true, ARM11A},
                                  {"boot9", c.memory.boot9, 0xFFFF0000, sizeof(c.memory.boot9), false, ARM9},
                                  {"boot11", c.memory.boot11, 0x00010000, sizeof(c.memory.boot11), false, ARM11A},
                                  {"itcm", c.cp15.itcm, 0, sizeof(c.cp15.itcm), true, ARM9},
                                  {"dtcm", c.cp15.dtcm, c.cp15.dtcmAddr, sizeof(c.cp15.dtcm), true, ARM9}};
    if (c.n3dsMode) {
        result.push_back({"fcramExt", c.memory.fcramExt, 0x28000000, 0x8000000, true, ARM11A});
        result.push_back({"vramExt", c.memory.vramExt, 0x1F000000, 0x400000, true, ARM11A});
    }
    return result;
}
const char* ScriptSession::methodName(Method method) {
    switch (method) {
    case Method::add:
        return "add";
    case Method::addKey:
        return "addKey";
    case Method::addKeys:
        return "addKeys";
    case Method::advance:
        return "advance";
    case Method::base:
        return "base";
    case Method::bound:
        return "bound";
    case Method::clear:
        return "clear";
    case Method::clearBreakpoint:
        return "clearBreakpoint";
    case Method::clearKey:
        return "clearKey";
    case Method::clearKeys:
        return "clearKeys";
    case Method::clearPathOverride:
        return "clearPathOverride";
    case Method::cols:
        return "cols";
    case Method::createBuffer:
        return "createBuffer";
    case Method::currentCycle:
        return "currentCycle";
    case Method::currentFrame:
        return "currentFrame";
    case Method::error:
        return "error";
    case Method::frameCycles:
        return "frameCycles";
    case Method::frequency:
        return "frequency";
    case Method::getCPU:
        return "getCPU";
    case Method::getKey:
        return "getKey";
    case Method::getKeys:
        return "getKeys";
    case Method::getPaths:
        return "getPaths";
    case Method::getX:
        return "getX";
    case Method::getY:
        return "getY";
    case Method::loadFile:
        return "loadFile";
    case Method::loadFirm:
        return "loadFirm";
    case Method::log:
        return "log";
    case Method::moveCursor:
        return "moveCursor";
    case Method::name:
        return "name";
    case Method::pause:
        return "pause";
    case Method::pressHome:
        return "pressHome";
    case Method::pressScreen:
        return "pressScreen";
    case Method::print:
        return "print";
    case Method::read16:
        return "read16";
    case Method::read32:
        return "read32";
    case Method::read8:
        return "read8";
    case Method::readRange:
        return "readRange";
    case Method::readRegister:
        return "readRegister";
    case Method::releaseHome:
        return "releaseHome";
    case Method::releaseLStick:
        return "releaseLStick";
    case Method::releaseScreen:
        return "releaseScreen";
    case Method::remove:
        return "remove";
    case Method::reset:
        return "reset";
    case Method::resume:
        return "resume";
    case Method::rows:
        return "rows";
    case Method::runFrame:
        return "runFrame";
    case Method::screenshot:
        return "screenshot";
    case Method::setBreakpoint:
        return "setBreakpoint";
    case Method::setKeys:
        return "setKeys";
    case Method::setLStick:
        return "setLStick";
    case Method::setName:
        return "setName";
    case Method::setPathOverride:
        return "setPathOverride";
    case Method::setRangeWatchpoint:
        return "setRangeWatchpoint";
    case Method::setSize:
        return "setSize";
    case Method::setWatchpoint:
        return "setWatchpoint";
    case Method::size:
        return "size";
    case Method::start:
        return "start";
    case Method::step:
        return "step";
    case Method::stop:
        return "stop";
    case Method::warn:
        return "warn";
    case Method::write16:
        return "write16";
    case Method::write32:
        return "write32";
    case Method::write8:
        return "write8";
    case Method::writeRange:
        return "writeRange";
    case Method::writeRegister:
        return "writeRegister";
    }
    return "unknown";
}

void ScriptSession::bindMethod(const char* name, int cpu, int domain) {
    Method method = Method::read8;
    bool found = false;
    for (int i = 0; i <= int(Method::writeRegister); ++i) {
        if (!strcmp(name, methodName(Method(i)))) {
            method = Method(i);
            found = true;
            break;
        }
    }
    if (!found)
        throw std::runtime_error(std::string("Unknown binding: ") + name);
    lua_pushlightuserdata(lua, this);
    lua_pushinteger(lua, int(method));
    lua_pushinteger(lua, cpu);
    lua_pushinteger(lua, domain);
    lua_pushcclosure(lua, invoke, 4);
    lua_setfield(lua, -2, name);
}
void ScriptSession::bindTable(int cpu, int domain) {
    lua_newtable(lua);
    for (const char* name : {"read8", "read16", "read32", "readRange", "write8", "write16", "write32", "writeRange"})
        bindMethod(name, cpu, domain);
    if (domain >= 0) {
        for (const char* name : {"base", "bound", "size", "name"})
            bindMethod(name, cpu, domain);
    } else if (domain == -1) {
        for (const char* name : {"readRegister", "writeRegister", "step", "setBreakpoint", "setWatchpoint",
                                 "setRangeWatchpoint", "clearBreakpoint"})
            bindMethod(name, cpu);
        bindTable(cpu, -2);
        lua_setfield(lua, -2, "physical");
    }
}
void ScriptSession::initLua() {
    if (lua)
        return;
    lua = luaL_newstate();
    if (!lua)
        throw std::runtime_error("Cannot allocate Lua state");
    *static_cast<ScriptSession**>(lua_getextraspace(lua)) = this;
    luaL_openlibs(lua);
    lua_sethook(lua, interrupt, LUA_MASKCOUNT, 10000);
    bindTable(ARM11A, -1);
    for (const char* name : {"start",
                             "reset",
                             "stop",
                             "pause",
                             "resume",
                             "runFrame",
                             "currentFrame",
                             "currentCycle",
                             "frequency",
                             "frameCycles",
                             "getCPU",
                             "setPathOverride",
                             "clearPathOverride",
                             "getPaths",
                             "loadFile",
                             "loadFirm",
                             "setKeys",
                             "addKeys",
                             "clearKeys",
                             "addKey",
                             "clearKey",
                             "getKey",
                             "getKeys",
                             "pressScreen",
                             "releaseScreen",
                             "setLStick",
                             "releaseLStick",
                             "pressHome",
                             "releaseHome",
                             "screenshot"})
        bindMethod(name);
    // Domain handles resolve against the current core on every call, never retain pointers.
    lua_newtable(lua);
    const char* names[] = {"arm9ram", "vram", "dspwram", "axiwram",  "fcram",  "boot9",
                           "boot11",  "itcm", "dtcm",    "fcramExt", "vramExt"};
    for (int i = 0; i < 11; ++i) {
        bindTable(ARM11A, i);
        lua_setfield(lua, -2, names[i]);
    }
    lua_setfield(lua, -2, "memory");
    lua_setglobal(lua, "emu");
    lua_newtable(lua);
    bindMethod("add");
    bindMethod("remove");
    lua_setglobal(lua, "callbacks");
    lua_newtable(lua);
    for (const char* name : {"log", "warn", "error", "createBuffer"})
        bindMethod(name);
    lua_setglobal(lua, "console");
    lua_newtable(lua);
    bindMethod("print");
    lua_getfield(lua, -1, "print");
    lua_setglobal(lua, "print");
    lua_pop(lua, 1);
    lua_newtable(lua);
    lua_newtable(lua);
    for (int i = 0; i < MAX_CPUS; ++i) {
        lua_pushstring(lua, cpuNames[i]);
        lua_setfield(lua, -2, cpuNames[i]);
    }
    lua_setfield(lua, -2, "CPU");
    lua_newtable(lua);
    const char* keys[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    for (int i = 0; i < 12; ++i)
        field(lua, keys[i], i);
    lua_setfield(lua, -2, "KEY");
    lua_newtable(lua);
    field(lua, "READ", 2);
    field(lua, "WRITE", 1);
    field(lua, "RW", 3);
    lua_setfield(lua, -2, "WATCHPOINT_TYPE");
    lua_setglobal(lua, "C");
}
void ScriptSession::clearCancel() {
    cancelled = false;
    if (lua)
        restoreInterruptHooks();
}

void ScriptSession::restoreInterruptHooks() {
    // Registry references keep interrupted coroutine objects alive until restoration.
    for (int ref : interruptedThreads) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, ref);
        lua_sethook(lua_tothread(lua, -1), interrupt, LUA_MASKCOUNT, 10000);
        lua_pop(lua, 1);
        luaL_unref(lua, LUA_REGISTRYINDEX, ref);
    }
    interruptedThreads.clear();
    lua_sethook(lua, interrupt, LUA_MASKCOUNT, 10000);
}

void ScriptSession::interrupt(lua_State* L, lua_Debug*) {
    ScriptSession* session = *static_cast<ScriptSession**>(lua_getextraspace(L));
    if (session->cancelled.load() || (session->deadline != std::chrono::steady_clock::time_point{} &&
                                      std::chrono::steady_clock::now() >= session->deadline)) {
        // Escalate both this coroutine and its main thread, so resume/pcall cannot
        // continue executing in the parent after catching the interruption.
        if (lua_gethookcount(L) != 1) {
            lua_pushthread(L);
            session->interruptedThreads.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        lua_sethook(lua_tothread(L, -1), interrupt, LUA_MASKCOUNT, 1);
        lua_pop(L, 1);
        lua_sethook(L, interrupt, LUA_MASKCOUNT, 1);
        luaL_error(L, "Script cancelled or timed out");
    }
}
int ScriptSession::invoke(lua_State* L) {
    ScriptSession* session = static_cast<ScriptSession*>(lua_touserdata(L, lua_upvalueindex(1)));
    // Lua is built as C++; its internal exceptions must pass through unchanged.
    try {
        struct ActiveLua {
            lua_State *&current, *previous;
            ActiveLua(lua_State*& current, lua_State* state) : current(current), previous(current) {
                current = state;
            }
            ~ActiveLua() {
                current = previous;
            }
        } active(session->lua, L);
        session->checkCancelled();
        return session->dispatch(Method(lua_tointeger(L, lua_upvalueindex(2))),
                                 int(lua_tointeger(L, lua_upvalueindex(3))),
                                 int(lua_tointeger(L, lua_upvalueindex(4))));
    } catch (const std::exception& e) {
        lua_pushstring(L, e.what());
    }
    return lua_error(L);
}
void ScriptSession::logError(const std::string& message) {
    error = true;
    pause();
    if (output)
        output("ERROR: " + message);
}
bool ScriptSession::execute(int status) {
    if (status == LUA_OK) {
        lua_pushcfunction(lua, traceback);
        lua_insert(lua, -2);
        status = lua_pcall(lua, 0, 0, -2);
        if (status != LUA_OK) {
            logError(lua_tostring(lua, -1));
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);
    } else {
        logError(lua_tostring(lua, -1));
        lua_pop(lua, 1);
    }
    checkCancelled();
    return status == LUA_OK && !error;
}
bool ScriptSession::runFile(const std::string& path) {
    initLua();
    error = false;
    restoreInterruptHooks();
    try {
        checkCancelled();
        std::string absolute = absolutePath(path);
        // Local helper modules resolve beside the script, without changing cwd.
        lua_pushcfunction(lua, [](lua_State* L) -> int {
            // Raw accesses also avoid hostile __index/__newindex metamethods.
            lua_pushglobaltable(L);
            lua_pushliteral(L, "package");
            lua_rawget(L, -2);
            if (!lua_istable(L, -1))
                return 0;
            lua_pushliteral(L, "path");
            lua_rawget(L, -2);
            if (lua_type(L, -1) != LUA_TSTRING)
                return 0;
            if (strstr(lua_tostring(L, -1), lua_tostring(L, 1)))
                return 0;
            lua_pushliteral(L, "path");
            lua_pushvalue(L, 1);
            lua_pushvalue(L, -3);
            lua_concat(L, 2);
            lua_rawset(L, -4);
            return 0;
        });
        std::string dir = absolute.substr(0, absolute.find_last_of("/\\") + 1);
        pushString(lua, dir + "?.lua;" + dir + "?/init.lua;");
        if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
            logError(lua_tostring(lua, -1));
            lua_pop(lua, 1);
            return false;
        }
        return execute(luaL_loadfile(lua, absolute.c_str()));
    } catch (const std::exception& e) {
        logError(e.what());
        return false;
    }
}
bool ScriptSession::runString(const std::string& code) {
    initLua();
    error = false;
    restoreInterruptHooks();
    try {
        checkCancelled();
        return execute(luaL_loadbuffer(lua, code.data(), code.size(), "console"));
    } catch (const std::exception& e) {
        logError(e.what());
        return false;
    }
}
void ScriptSession::resetScripts() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    requireOutsideCallback();
    callbacks.clear();
    interruptedThreads.clear();
    points.clear();
    accesses.clear();
    if (bufferOutput)
        for (auto& b : buffers)
            bufferOutput(b.second.name, "");
    buffers.clear();
    if (lua)
        lua_close(lua);
    lua = nullptr;
    error = false;
    if (core)
        core->input.clearScriptInput();
    refreshHooks();
}
bool ScriptSession::call(int ref, const Access* access, int registration) {
    int top = lua_gettop(lua);
    lua_pushcfunction(lua, traceback);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, ref);
    int nargs = 0;
    if (access) {
        lua_newtable(lua);
        lua_pushstring(lua, cpuNames[access->cpu]);
        lua_setfield(lua, -2, "cpu");
        field(lua, "pc", access->pc);
        field(lua, "address", access->address);
        field(lua, "width", access->width);
        field(lua, "value", access->value);
        field(lua, "accessType", access->width ? (access->write ? 1 : 2) : 0);
        if (access->width && access->write)
            field(lua, "newValue", access->value);
        nargs = 1;
    }
    ++callbackDepth;
    int status = lua_pcall(lua, nargs, 0, top + 1);
    --callbackDepth;
    if (status != LUA_OK) {
        logError(lua_tostring(lua, -1));
        // Disable precisely the failed registration, including a failed breakpoint.
        if (access) {
            auto it = points.find(registration);
            if (it != points.end()) {
                luaL_unref(lua, LUA_REGISTRYINDEX, it->second.ref);
                points.erase(it);
            }
        } else {
            auto it = callbacks.find(registration);
            if (it != callbacks.end()) {
                luaL_unref(lua, LUA_REGISTRYINDEX, it->second.ref);
                callbacks.erase(it);
            }
        }
        refreshHooks();
    }
    lua_settop(lua, top);
    return status == LUA_OK;
}
void ScriptSession::emit(const std::string& event) {
    if (!lua)
        return;
    std::vector<int> ids;
    for (auto& cb : callbacks)
        if (cb.second.event == event)
            ids.push_back(cb.first);
    for (int id : ids) {
        auto it = callbacks.find(id);
        if (it != callbacks.end())
            call(it->second.ref, nullptr, id);
    }
}
void ScriptSession::publishBuffer(int id) {
    Buffer& b = buffers.at(id);
    std::string text;
    for (auto& line : b.lines) {
        text += line;
        text += '\n';
    }
    if (bufferOutput)
        bufferOutput(b.name, text);
    else if (output)
        output("[" + b.name + "]\n" + text);
}

int ScriptSession::dispatch(Method method, int cpu, int domain) {
    // Advancement takes/releases the core lock per short scheduler quantum.
    if (method == Method::runFrame || method == Method::step) {
        unsigned timeout = lua_isnoneornil(lua, 2) ? 1000 : unsigned(integer(lua, 2, 3600000));
        pushString(lua, advance(method == Method::step ? cpu : -1, timeout));
        return 1;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (domain <= -100) {
        int id = -100 - domain;
        auto it = buffers.find(id);
        if (it == buffers.end())
            throw std::runtime_error("Text buffer has been destroyed");
        Buffer& b = it->second;
        if (method == Method::getX || method == Method::getY || method == Method::cols || method == Method::rows) {
            lua_pushinteger(lua, method == Method::getX   ? b.x
                                 : method == Method::getY ? b.y
                                 : method == Method::cols ? b.cols
                                                          : b.rows);
            return 1;
        }
        if (method == Method::setSize) {
            unsigned cols = unsigned(integer(lua, 2, 4096)), rows = unsigned(integer(lua, 3, 4096));
            if (!cols || !rows || uint64_t(cols) * rows > 1048576)
                throw std::runtime_error("Invalid text buffer dimensions");
            b.cols = cols;
            b.rows = rows;
            b.lines.resize(rows);
            for (auto& line : b.lines)
                line.resize(cols, ' ');
            b.x = std::min(b.x, cols - 1);
            b.y = std::min(b.y, rows - 1);
        } else if (method == Method::moveCursor) {
            unsigned x = unsigned(integer(lua, 2, b.cols - 1)), y = unsigned(integer(lua, 3, b.rows - 1));
            b.x = x;
            b.y = y;
        } else if (method == Method::advance) {
            int delta = signedInteger(lua, 2, -1048576, 1048576);
            int pos = std::max(0, int(b.y * b.cols + b.x) + delta);
            while (pos > int(b.rows * b.cols)) {
                b.lines.erase(b.lines.begin());
                b.lines.push_back(std::string(b.cols, ' '));
                pos -= b.cols;
            }
            b.x = pos % b.cols;
            b.y = pos / b.cols;
        } else if (method == Method::setName) {
            std::string name = str(lua, 2);
            for (auto& entry : buffers)
                if (entry.first != id && entry.second.name == name)
                    throw std::runtime_error("A text buffer already has this name");
            if (name != b.name && bufferOutput)
                bufferOutput(b.name, "");
            b.name = name;
        } else if (method == Method::clear) {
            b.lines.assign(b.rows, std::string(b.cols, ' '));
            b.x = b.y = 0;
        } else if (method == Method::print) {
            size_t size;
            const char* value = luaL_tolstring(lua, 2, &size);
            std::string text(value, size);
            lua_pop(lua, 1);
            for (char ch : text) {
                if (b.y == b.rows) {
                    b.lines.erase(b.lines.begin());
                    b.lines.push_back(std::string(b.cols, ' '));
                    --b.y;
                }
                if (ch == '\n') {
                    b.x = 0;
                    ++b.y;
                } else {
                    b.lines[b.y][b.x++] = ch;
                    if (b.x == b.cols) {
                        b.x = 0;
                        ++b.y;
                    }
                }
            }
        }
        publishBuffer(id);
        return 0;
    }
    if (method == Method::print || method == Method::log || method == Method::warn || method == Method::error) {
        std::string text;
        for (int i = method == Method::print ? 1 : 2; i <= lua_gettop(lua); ++i) {
            if (!text.empty())
                text += '\t';
            if (lua_isstring(lua, i)) {
                size_t size;
                const char* s = lua_tolstring(lua, i, &size);
                text.append(s, size);
            } else if (lua_isboolean(lua, i))
                text += lua_toboolean(lua, i) ? "true" : "false";
            else
                text += luaL_typename(lua, i);
        }
        if (output)
            output((method == Method::warn ? "WARNING: " : method == Method::error ? "ERROR: " : "") + text);
        return 0;
    }
    if (method == Method::createBuffer) {
        int id = nextId++;
        Buffer b;
        b.name = lua_isnoneornil(lua, 2) ? "Buffer " + std::to_string(id) : str(lua, 2);
        for (auto& entry : buffers)
            if (entry.second.name == b.name)
                throw std::runtime_error("A text buffer already has this name");
        b.lines.assign(b.rows, std::string(b.cols, ' '));
        buffers[id] = b;
        lua_newtable(lua);
        for (const char* name :
             {"print", "clear", "setSize", "moveCursor", "advance", "setName", "getX", "getY", "cols", "rows"})
            bindMethod(name, ARM11A, -100 - id);
        publishBuffer(id);
        return 1;
    }
    if (method == Method::add) {
        std::string event = str(lua, 2);
        int ref = functionRef(lua, 3), id = nextId++;
        callbacks[id] = {event, ref};
        lua_pushinteger(lua, id);
        return 1;
    }
    if (method == Method::remove || method == Method::clearBreakpoint) {
        int id = int(integer(lua, 2, INT32_MAX));
        bool found = false;
        if (method == Method::remove) {
            auto it = callbacks.find(id);
            if (it != callbacks.end()) {
                luaL_unref(lua, LUA_REGISTRYINDEX, it->second.ref);
                callbacks.erase(it);
                found = true;
            }
        } else {
            auto it = points.find(id);
            if (it != points.end()) {
                luaL_unref(lua, LUA_REGISTRYINDEX, it->second.ref);
                points.erase(it);
                found = true;
                refreshHooks();
            }
        }
        lua_pushboolean(lua, found);
        return 1;
    }
    if (method == Method::getCPU) {
        bindTable(cpuByName(str(lua, 2)), -1);
        return 1;
    }
    if (method == Method::setPathOverride) {
        setPath(str(lua, 2), str(lua, 3));
        return 0;
    }
    if (method == Method::clearPathOverride) {
        clearPath(str(lua, 2));
        return 0;
    }
    if (method == Method::getPaths) {
        lua_newtable(lua);
        for (const char* kind : {"saved", "effective", "mounted"}) {
            lua_newtable(lua);
            for (auto& p : paths(kind)) {
                pushString(lua, p.second);
                lua_setfield(lua, -2, p.first.c_str());
            }
            lua_setfield(lua, -2, kind);
        }
        return 1;
    }
    if (method == Method::start || method == Method::reset) {
        start(method == Method::reset);
        return 0;
    }
    if (method == Method::stop) {
        stop();
        return 0;
    }
    if (method == Method::pause) {
        pause();
        return 0;
    }
    if (method == Method::resume) {
        resume();
        return 0;
    }
    if (method == Method::loadFile) {
        requireOutsideCallback();
        std::string path = absolutePath(str(lua, 2));
        readable(path, false);
        std::string previous = cartPath;
        cartPath = path;
        try {
            start(true);
        } catch (...) {
            cartPath = previous;
            throw;
        }
        lua_pushboolean(lua, true);
        return 1;
    }
    if (method == Method::loadFirm) {
        requireOutsideCallback();
        auto previous = overrides;
        setPath("firm", str(lua, 2));
        try {
            start(true);
        } catch (...) {
            overrides = std::move(previous);
            throw;
        }
        lua_pushboolean(lua, true);
        return 1;
    }
    if (method == Method::setBreakpoint || method == Method::setWatchpoint || method == Method::setRangeWatchpoint) {
        uint64_t min = integer(lua, 3), max = min + 1;
        unsigned type = 0;
        if (method == Method::setWatchpoint)
            type = unsigned(integer(lua, 4, 3));
        if (method == Method::setRangeWatchpoint) {
            max = integer(lua, 4, UINT64_C(0x100000000));
            type = unsigned(integer(lua, 5, 3));
        }
        if (max <= min || (method != Method::setBreakpoint && !type))
            throw std::runtime_error("Invalid watchpoint range or type");
        int ref = functionRef(lua, 2), id = nextId++;
        points[id] = {ref, CpuId(cpu), min, max, type};
        refreshHooks();
        lua_pushinteger(lua, id);
        return 1;
    }
    Core& c = requireCore();
    if ((cpu == ARM11C || cpu == ARM11D) && !c.n3dsMode)
        throw std::runtime_error("CPU is unavailable on Old 3DS");
    if (method == Method::currentFrame || method == Method::currentCycle || method == Method::frequency ||
        method == Method::frameCycles) {
        uint64_t n = method == Method::currentFrame   ? c.frameCounter
                     : method == Method::currentCycle ? c.currentCycle()
                     : method == Method::frequency    ? 268111856
                                                      : 268111856 / 60;
        lua_pushinteger(lua, lua_Integer(n));
        return 1;
    }
    if (method == Method::readRegister) {
        lua_pushinteger(lua, c.arms[cpu].debugReadRegister(str(lua, 2)));
        return 1;
    }
    if (method == Method::writeRegister) {
        std::string name = str(lua, 2);
        uint32_t value = uint32_t(integer(lua, 3));
        c.arms[cpu].debugWriteRegister(name, value);
        return 0;
    }
    if (method == Method::getKeys) {
        lua_pushinteger(lua, (~c.input.readHidPad()) & 0xFFF);
        return 1;
    }
    if (method == Method::getKey) {
        unsigned key = unsigned(integer(lua, 2, 11));
        lua_pushinteger(lua, ((~c.input.readHidPad()) >> key) & 1);
        return 1;
    }
    if (method == Method::setKeys || method == Method::addKeys || method == Method::clearKeys ||
        method == Method::addKey || method == Method::clearKey) {
        uint16_t keys = (method == Method::addKey || method == Method::clearKey) ? BIT(integer(lua, 2, 11))
                                                                                 : integer(lua, 2, 0xFFF);
        if (method == Method::addKeys || method == Method::addKey)
            keys |= c.input.getScriptKeys();
        if (method == Method::clearKeys || method == Method::clearKey)
            keys = c.input.getScriptKeys() & ~keys;
        c.input.setScriptKeys(keys);
        return 0;
    }
    if (method == Method::pressScreen) {
        int x = int(integer(lua, 2, 319)), y = int(integer(lua, 3, 239));
        c.input.setScriptTouch(x, y, true);
        return 0;
    }
    if (method == Method::releaseScreen) {
        c.input.setScriptTouch(0, 0, false);
        return 0;
    }
    if (method == Method::setLStick) {
        int x = signedInteger(lua, 2, -2047, 2047), y = signedInteger(lua, 3, -2047, 2047);
        c.input.setScriptStick(x, y, true);
        return 0;
    }
    if (method == Method::releaseLStick) {
        c.input.setScriptStick(0, 0, false);
        return 0;
    }
    if (method == Method::pressHome || method == Method::releaseHome) {
        c.input.setScriptHome(method == Method::pressHome);
        return 0;
    }
    if (method == Method::screenshot) {
        std::string path = absolutePath(str(lua, 2));
        auto pixels = c.pdc.latestFrame();
        if (pixels.empty())
            throw std::runtime_error("No completed frame to capture");
        // Convert packed native integers explicitly so PNG bytes are endian-independent.
        std::vector<uint8_t> rgba(pixels.size() * 4);
        for (size_t i = 0; i < pixels.size(); ++i)
            for (unsigned j = 0; j < 4; ++j)
                rgba[i * 4 + j] = pixels[i] >> (8 * j);
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        image.width = 400;
        image.height = 480;
        image.format = PNG_FORMAT_RGBA;
        if (!png_image_write_to_file(&image, path.c_str(), 0, rgba.data(), 0, nullptr)) {
            std::string message = image.message;
            png_image_free(&image);
            throw std::runtime_error("Screenshot failed: " + message);
        }
        png_image_free(&image);
        return 0;
    }
    c.gpu.syncRender();
    Domain d{};
    if (domain >= 0) {
        auto list = domains();
        if (unsigned(domain) >= list.size())
            throw std::runtime_error("Memory domain is unavailable on this system");
        d = list[domain];
        if (method == Method::name) {
            lua_pushstring(lua, d.name);
            return 1;
        }
        if (method == Method::base || method == Method::bound || method == Method::size) {
            lua_pushinteger(lua, method == Method::base    ? d.base
                                 : method == Method::bound ? uint64_t(d.base) + d.size
                                                           : d.size);
            return 1;
        }
    }
    if (method != Method::read8 && method != Method::read16 && method != Method::read32 &&
        method != Method::readRange && method != Method::write8 && method != Method::write16 &&
        method != Method::write32 && method != Method::writeRange)
        throw std::runtime_error(std::string("Unknown scripting method: ") + methodName(method));
    bool write = method == Method::write8 || method == Method::write16 || method == Method::write32 ||
                 method == Method::writeRange;
    bool range = method == Method::readRange || method == Method::writeRange;
    unsigned width = method == Method::read8 || method == Method::write8     ? 1
                     : method == Method::read16 || method == Method::write16 ? 2
                                                                             : 4;
    uint64_t address = integer(lua, 2), length = width;
    std::string data;
    uint32_t value = 0;
    if (range) {
        if (write) {
            data = str(lua, 3);
            length = data.size();
        } else
            length = integer(lua, 3, 16 * 1024 * 1024);
    } else if (write)
        value = uint32_t(integer(lua, 3, width == 4 ? UINT32_MAX : (1U << (width * 8)) - 1));
    if (length > 16 * 1024 * 1024 || address + length > (domain >= 0 ? d.size : UINT64_C(0x100000000)))
        throw std::runtime_error("Memory access exceeds domain or address bounds (maximum range: 16 MiB)");
    if (domain >= 0 && write && !d.writable)
        throw std::runtime_error("Memory domain is read-only");
    auto readByte = [&](uint32_t addr) -> uint8_t {
        if (domain >= 0)
            return d.data[addr];
        return domain == -2 ? c.memory.read<uint8_t>(CpuId(cpu), addr) : c.cp15.read<uint8_t>(CpuId(cpu), addr);
    };
    auto writeByte = [&](uint32_t addr, uint8_t v) {
        if (domain >= 0)
            d.data[addr] = v;
        else if (domain == -2)
            c.memory.write<uint8_t>(CpuId(cpu), addr, v);
        else
            c.cp15.write<uint8_t>(CpuId(cpu), addr, v);
    };
    if (range && domain >= 0) {
        if (write)
            memcpy(d.data + address, data.data(), length);
        else
            data.assign(reinterpret_cast<char*>(d.data + address), length);
    } else if (range && !write) {
        data.resize(length);
        for (uint64_t offset = 0; offset < length;) {
            checkCancelled();
            uint32_t addr = uint32_t(address + offset);
            size_t chunk = std::min<uint64_t>(length - offset, 0x1000 - (addr & 0xFFF));
            uint8_t* page = domain == -2 ? (cpu == ARM9 ? c.memory.memMap9 : c.memory.memMap11)[addr >> 12].read
                                         : c.cp15.getReadPtr(CpuId(cpu), addr);
            if (page)
                memcpy(&data[offset], page + (addr & 0xFFF), chunk);
            else
                for (size_t i = 0; i < chunk; ++i)
                    data[offset + i] = char(readByte(addr + i));
            offset += chunk;
        }
    } else if (range || domain >= 0 || (domain == -2 && (address & 0xFFF) + width > 0x1000)) {
        if (!write && range)
            data.resize(length);
        for (uint64_t i = 0; i < length; ++i) {
            if (!(i & 4095))
                checkCancelled();
            if (write)
                writeByte(uint32_t(address + i), range ? uint8_t(data[i]) : uint8_t(value >> (i * 8)));
            else {
                uint8_t byte = readByte(uint32_t(address + i));
                if (range)
                    data[i] = char(byte);
                else
                    value |= uint32_t(byte) << (i * 8);
            }
        }
    } else {
#define ACCESS(T)                                                                                                      \
    if (write) {                                                                                                       \
        if (domain == -2)                                                                                              \
            c.memory.write<T>(CpuId(cpu), uint32_t(address), T(value));                                                \
        else                                                                                                           \
            c.cp15.write<T>(CpuId(cpu), uint32_t(address), T(value));                                                  \
    } else                                                                                                             \
        value = domain == -2 ? c.memory.read<T>(CpuId(cpu), uint32_t(address))                                         \
                             : c.cp15.read<T>(CpuId(cpu), uint32_t(address));
        switch (width) {
        case 1: {
            ACCESS(uint8_t);
            break;
        }
        case 2: {
            ACCESS(uint16_t);
            break;
        }
        default: {
            ACCESS(uint32_t);
            break;
        }
        }
#undef ACCESS
    }
    if (write) {
        if (domain >= 0 && length) {
            // TCM has no GPU-visible physical backing; all other domains do.
            if (domain != 7 && domain != 8)
                c.memory.invalidateRange(d.base, uint32_t(address), uint32_t(length));
        }
        refreshPipelines();
        return 0;
    }
    if (range)
        pushString(lua, data);
    else
        lua_pushinteger(lua, value);
    return 1;
}
