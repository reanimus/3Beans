#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../core/boot_config.h"
#include "../core/defines.h"

struct lua_State;
struct lua_Debug;
class Core;

// All Lua and execution calls belong to one host thread. Other threads may cancel
// or read core snapshots under mutex; they never enter Lua.
class ScriptSession {
  public:
    std::recursive_mutex mutex;
    // Only protects core replacement against audio/display consumers.
    std::mutex consumerMutex;
    std::unique_ptr<Core> core;
    std::atomic<bool> cancelled{false}, autoRun{false};
    std::atomic<uint64_t> cancelGeneration{0};
    void requestCancel() {
        ++cancelGeneration;
        cancelled = true;
    }
    // Execution thread only: also restore hooks before any surviving callbacks run.
    void clearCancel();
    std::function<void(const std::string&)> output;
    std::function<void(const std::string&, const std::string&)> bufferOutput;
    std::function<void()> changed;
    std::function<void()>* context = nullptr;
    std::string cartPath;
    bool headless;

    explicit ScriptSession(bool headless = false);
    ~ScriptSession();
    bool runFile(const std::string& path);
    bool runString(const std::string& code);
    void resetScripts();
    void start(bool reset = false);
    void stop();
    void pause();
    void resume();
    std::string advance(int cpu = -1, unsigned timeoutMs = 1000);
    void setPath(const std::string& name, const std::string& path);
    void clearPath(const std::string& name);
    std::map<std::string, std::string> paths(const std::string& kind) const;
    bool hasOverrides() const {
        return !overrides.empty();
    }
    void setTimeout(double seconds);
    bool failed() const {
        return error;
    }
    void checkCancelled();
    static std::string absolutePath(const std::string& path);

  private:
    lua_State* lua = nullptr;
    bool paused = true, error = false;
    unsigned callbackDepth = 0;
    int nextId = 1, stepCpu = -1;
    bool stepDone = false;
    bool skipPending[MAX_CPUS] = {};
    uint32_t skipAddress[MAX_CPUS] = {};
    std::map<std::string, std::string> overrides;
    std::chrono::steady_clock::time_point deadline{}, quantumEnd{};
    struct Callback {
        std::string event;
        int ref;
    };
    struct Point {
        int ref;
        CpuId cpu;
        uint64_t min, max;
        unsigned type;
    };
    struct Access {
        CpuId cpu;
        uint32_t pc, address, value;
        unsigned width;
        bool write;
    };
    struct Buffer {
        std::string name;
        unsigned cols = 80, rows = 24, x = 0, y = 0;
        std::vector<std::string> lines;
    };
    struct Domain {
        const char* name;
        uint8_t* data;
        uint32_t base, size;
        bool writable;
        CpuId cpu;
    };
    std::map<int, Callback> callbacks;
    std::map<int, Point> points;
    std::map<int, Buffer> buffers;
    std::vector<Access> accesses;

    std::vector<int> interruptedThreads;
    void restoreInterruptHooks();
    void initLua();
    void bindTable(int cpu, int domain);
    void bindMethod(const char* name, int cpu = ARM11A, int domain = -1);
    static int invoke(lua_State* L);
    enum class Method {
        add,
        addKey,
        addKeys,
        advance,
        base,
        bound,
        clear,
        clearBreakpoint,
        clearKey,
        clearKeys,
        clearPathOverride,
        cols,
        createBuffer,
        currentCycle,
        currentFrame,
        error,
        frameCycles,
        frequency,
        getCPU,
        getKey,
        getKeys,
        getPaths,
        getX,
        getY,
        loadFile,
        log,
        moveCursor,
        name,
        pause,
        pressHome,
        pressScreen,
        print,
        read16,
        read32,
        read8,
        readRange,
        readRegister,
        releaseHome,
        releaseLStick,
        releaseScreen,
        remove,
        reset,
        resume,
        rows,
        runFrame,
        screenshot,
        setBreakpoint,
        setKeys,
        setLStick,
        setName,
        setPathOverride,
        setRangeWatchpoint,
        setSize,
        setWatchpoint,
        size,
        start,
        step,
        stop,
        warn,
        write16,
        write32,
        write8,
        writeRange,
        writeRegister
    };
    static const char* methodName(Method method);
    int dispatch(Method method, int cpu, int domain);
    static void interrupt(lua_State* L, lua_Debug*);
    bool execute(int loadStatus);
    bool call(int ref, const Access* access = nullptr, int registration = 0);
    void emit(const std::string& event);
    void attach();
    void refreshHooks();
    bool before(CpuId cpu);
    bool after(CpuId cpu);
    std::vector<Domain> domains();
    Core& requireCore();
    void requireOutsideCallback();
    void refreshPipelines();
    void logError(const std::string& message);
    void publishBuffer(int id);
};
