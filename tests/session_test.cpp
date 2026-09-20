#include "../src/scripting/session.h"
#include "../src/core/core.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <thread>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static std::string contents(const std::string& path) {
    std::ifstream stream(path);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected temporary fixture directory");
        std::string dir = argv[1];
        Settings::load(dir, false);
        ScriptSession session(true);
        std::string errors;
        session.output = [&](const std::string& text) { errors += text + "\n"; };
        session.setPath("sd", dir + "/override.img");
        session.setPath("nand", dir + "/nand.bin");
        session.setPath("boot9", dir + "/boot9.bin");
        session.setPath("boot11", dir + "/boot11.bin");
        session.start();
        auto readSdMarker = [&]() {
            SdMmc& sd = session.core->sdMmcs[0];
            sd.writeData16Blklen(0xFFFF, 512);
            sd.writeCmdParam(UINT32_MAX, 512);
            sd.writeCmd(0xFFFF, 16);
            sd.writeCmdParam(UINT32_MAX, 0);
            sd.writeCmd(0xFFFF, 17);
            session.advance();
            return sd.readData16Fifo();
        };
        check(readSdMarker() == 0x1111, "Controller did not read the overridden SD image");
        session.setPath("sd", dir + "/moved.img");
        session.start(true);
        check(readSdMarker() == 0x2222, "Reset did not mount the replacement SD image");
        session.clearPath("sd");
        session.start(true);
        check(readSdMarker() == 0x3333, "Clearing override did not mount the saved SD image");
        session.setPath("sd", dir + "/override.img");
        session.start(true);
        // Saving unrelated settings serializes only the user's saved paths.
        Settings::fpsLimiter = 0;
        check(Settings::save(), "Could not save test settings");
        auto saved = contents(dir + "/3beans.ini");
        check(saved.find("sdPath=saved.img\n") != std::string::npos, "SD override leaked into settings");
        check(saved.find("override.img") == std::string::npos && saved.find("boot9Path=missing9") != std::string::npos,
              "Boot override leaked into settings");
        check(session.runString("survivor = 42; calls = 0; callbacks:add('frame', function() calls = calls + 1 end)"),
              "Could not register callback");
        session.start(true);
        check(session.runString("assert(survivor == 42); emu:runFrame(); assert(calls == 1)"), "Reset lost Lua state");
        session.resetScripts();
        check(session.runString("assert(survivor == nil); emu:runFrame(); assert(calls == nil)"),
              "Scripting reset retained callbacks");
        check(session.paths("effective").at("sd") == dir + "/override.img", "Scripting reset lost path override");
        // Held physical controls survive script ownership changes.
        session.core->input.pressKey(0);
        session.core->input.pressHome();
        session.core->input.pressScreen(12, 34);
        session.core->input.setLStick(100, -100);
        check(session.runString(
                  "emu:addKey(C.KEY.A); emu:clearKey(C.KEY.A); assert(emu:getKey(C.KEY.A)); emu:addKey(C.KEY.B)"),
              "Input ownership failed");
        session.resetScripts();
        check(session.runString("assert(emu:getKeys() == 1)"), "Reset released host key or retained script key");
        session.core->input.releaseKey(0);
        // Virtual access honors the ARM11 MMU, whereas physical access does not.
        Core& core = *session.core;
        core.memory.write<uint32_t>(ARM11A, 0x20004000 + (0x400 * 4), 0x20000002);
        core.cp15.writeReg(ARM11A, 2, 0, 0, 0x20004000);
        core.cp15.writeReg(ARM11A, 1, 0, 0, 1);
        check(session.runString(
                  "emu:write32(0x40008000, 0x87654321); assert(emu.physical:read32(0x20008000) == 0x87654321)"),
              "MMU mapping failed");
        core.cp15.writeReg(ARM11A, 1, 0, 0, 0);
        core.arms[ARM11A].debugRefreshPipeline();
        // ARM9 DTCM is independently addressable from its physical backing bus.
        core.cp15.writeReg(ARM9, 9, 1, 0, 0x0800000A);
        core.cp15.writeReg(ARM9, 1, 0, 0, BIT(16));
        check(session.runString("local n = emu:getCPU('ARM9'); n.physical:write32(0x08000000, 11); "
                                "n:write32(0x08000000, 22); assert(n:read32(0x08000000) == 22 and "
                                "n.physical:read32(0x08000000) == 11); assert(emu.memory.dtcm:read32(0) == 22)"),
              "TCM mapping failed");
        session.start(true);
        check(session.runString("emu:getCPU('ARM11A'):setWatchpoint(function() error('not CPU data') end, 0x20000000, "
                                "C.WATCHPOINT_TYPE.RW)"),
              "Watchpoint registration failed");
        session.core->memory.write<uint32_t>(ARM11A, 0x20000000, 4);
        check(session.core->memory.read<uint32_t>(ARM11A, 0x20000000) == 4, "Physical/DMA-style access failed");
        check(session.runString("emu:runFrame()"), "Non-CPU memory access triggered watchpoint");
        session.resetScripts();
        // A failed callback is disabled and cannot repeatedly fail future frames.
        check(!session.runString(
                  "count = 0; callbacks:add('frame', function() count = count + 1; error('once') end); emu:runFrame()"),
              "Callback failure did not fail headless run");
        check(session.runString("emu:runFrame(); assert(count == 1)"), "Failed callback was not disabled");
        session.resetScripts();
        check(!session.runString(
                  "local id; id = callbacks:add('frame', function() callbacks:remove(id); callbacks:add('frame', "
                  "function() replacement = true end); error('removed callback') end); emu:runFrame()"),
              "Self-removing callback error was lost");
        check(session.runString("emu:runFrame(); assert(replacement)"), "Failed callback disabled its replacement");
        session.resetScripts();
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            session.cancelled = true;
        });
        bool success = session.runString("while true do pcall(function() while true do end end) end");
        cancel.join();
        check(!success, "Cancellation did not interrupt Lua");
        session.cancelled = false;
        check(session.runString("assert(1 + 1 == 2)"), "Runtime did not recover after cancellation");
        // A halted selected processor returns a bounded timeout instead of hanging.
        session.core->arms[ARM9].halt(BIT(0));
        check(session.advance(ARM9, 5) == "timeout", "Halted CPU step did not time out");
        session.stop();
        for (int i = 0; i < 30; ++i) {
            session.start(true);
            check(session.core->events.size() >= 3, "Reset corrupted scheduler initialization");
            session.advance();
        }
        session.stop();
        // Desktop consumers remain live while Lua holds the execution mutex.
        session.headless = false;
        Settings::gpuRenderer = 1;
        Settings::threadedGpu = 0;
        Settings::fpsLimiter = 0;
        session.context = nullptr; // Exercise the software desktop fallback too.
        session.start();
        check(session.core->bootConfig.audioPacing, "Desktop audio pacing was disabled");
        std::atomic<bool> consume{true};
        std::atomic<unsigned> reads{0}, framesRead{0};
        std::thread consumer([&] {
            while (consume.load()) {
                {
                    std::lock_guard<std::mutex> lock(session.consumerMutex);
                    if (session.core) {
                        session.core->csnd.getSamples(48000, 800);
                        uint32_t* frame = session.core->pdc.getFrame();
                        if (frame) {
                            ++framesRead;
                            delete[] frame;
                        }
                        ++reads;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        bool responsive =
            session.runString("local id; id = callbacks:add('frame', function() local t=os.clock(); while os.clock()-t "
                              "< 0.15 do end; callbacks:remove(id) end); emu:runFrame()");
        for (int i = 0; i < 3; ++i) {
            session.start(true);
            session.advance();
        }
        consume = false;
        consumer.join();
        check(responsive && reads > 20 && framesRead > 0, "Audio/display consumers stalled behind Lua");
        session.stop();
        // Lua conversion errors unwind C++ locks, including on coroutine stacks.
        check(session.runString("local co=coroutine.create(function() local b=console:createBuffer('unwind'); "
                                "b:print(setmetatable({}, {__tostring=function() error('convert') end})) end); "
                                "assert(not coroutine.resume(co)); assert(1+1==2)"),
              "Lua exception did not unwind coroutine binding");
        bool unlocked = false;
        std::thread probe([&] {
            unlocked = session.mutex.try_lock();
            if (unlocked)
                session.mutex.unlock();
        });
        probe.join();
        check(unlocked, "Lua exception leaked the session mutex");
        // Deferred wrapping retains the final row until another character arrives.
        std::string snapshot;
        session.bufferOutput = [&](const std::string&, const std::string& text) { snapshot = text; };
        check(session.runString("buf = console:createBuffer('exact'); buf:setSize(4, 2); buf:print('12345678')"),
              "Buffer setup failed");
        check(snapshot == "1234\n5678\n", "Exact buffer fill scrolled too early");
        check(session.runString("buf:print(9)"), "Buffer numeric print failed");
        check(snapshot == "5678\n9   \n", "Buffer did not scroll on the next character");
        check(session.runString(
                  "buf:advance(8); buf:print('Z'); buf:setSize(2,1); assert(buf:getX()<2 and buf:getY()<1)"),
              "Buffer advance/resize failed");
        puts("Session tests passed");
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Session test failed: %s\n", e.what());
        return 1;
    }
}
