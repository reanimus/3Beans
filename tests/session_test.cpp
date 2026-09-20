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
static void i2cSend(Core& core, uint8_t data, uint8_t control) {
    core.memory.write<uint8_t>(ARM9, 0x10144000, data);
    core.memory.write<uint8_t>(ARM9, 0x10144001, control);
    check(core.memory.read<uint8_t>(ARM9, 0x10144001) & 0x10, "I2C write was not acknowledged");
}
static void i2cWrite(Core& core, uint8_t device, uint8_t reg, uint8_t value) {
    i2cSend(core, device, 0x82);
    i2cSend(core, reg, 0x80);
    i2cSend(core, value, 0x81);
}
static void i2cReadStart(Core& core, uint8_t device, uint8_t reg) {
    i2cSend(core, device, 0x82);
    i2cSend(core, reg, 0x80);
    i2cSend(core, device | 1, 0x82);
}
static uint8_t i2cReadByte(Core& core, bool last = true) {
    core.memory.write<uint8_t>(ARM9, 0x10144001, last ? 0xA1 : 0xB0);
    return core.memory.read<uint8_t>(ARM9, 0x10144000);
}
static uint8_t i2cRead(Core& core, uint8_t device, uint8_t reg) {
    i2cReadStart(core, device, reg);
    return i2cReadByte(core);
}
static uint8_t lcdRead(Core& core, uint8_t device, uint8_t reg) {
    i2cWrite(core, device, 0x40, reg);
    i2cReadStart(core, device, 0x40);
    check(i2cReadByte(core, false) == reg, "LCD read did not return its address first");
    return i2cReadByte(core);
}
static void initLcd(Core& core) {
    auto& mem = core.memory;
    mem.write<uint32_t>(ARM11A, 0x10202014, 1);
    mem.write<uint32_t>(ARM11A, 0x1020200C, 0);
    for (int i = 0; i < 2; ++i) {
        mem.write<uint32_t>(ARM11A, 0x10202240 + i * 0x800, 0x5F);
        mem.write<uint32_t>(ARM11A, 0x10202244 + i * 0x800, 0x1023E);
        i2cWrite(core, 0x2C + i * 2, 0xFE, 0xAA);
        i2cWrite(core, 0x2C + i * 2, 0x60, 0);
        i2cWrite(core, 0x2C + i * 2, 0x01, 0x10);
    }
    i2cWrite(core, 0x4A, 0x22, 2);
    i2cWrite(core, 0x4A, 0x22, 0x28);
}
static void testLcd(Core& core) {
    auto& mem = core.memory;
    check(i2cRead(core, 0x4A, 0x0F) == 2, "Cold MCU reports powered LCDs");
    check(mem.read<uint32_t>(ARM11A, 0x10202014) == 0, "LCD reset was not asserted");
    check(lcdRead(core, 0x2C, 0xFE) == 0 && lcdRead(core, 0x2E, 0x60) == 1,
          "Cold LCD controller state is incorrect");
    initLcd(core);
    check(i2cRead(core, 0x4A, 0x0F) == 0xE2 && i2cRead(core, 0x4A, 0x22) == 0,
          "MCU did not complete LCD power requests");
    check(i2cRead(core, 0x4A, 0x13) == 0x2A && i2cRead(core, 0x4A, 0x13) == 0,
          "LCD completion events were incorrect or not cleared on read");
    for (int i = 0; i < 2; ++i) {
        uint8_t dev = 0x2C + i * 2;
        check(lcdRead(core, dev, 0x01) == 0x10 && lcdRead(core, dev, 0x62) == 1,
              "LCD controller never became ready");
        i2cWrite(core, dev, 0x40, 0xFE);
        i2cReadStart(core, dev, 0x40);
        check(i2cReadByte(core, false) == 0xFE && i2cReadByte(core, false) == 0xAA &&
              i2cReadByte(core, false) == 0xFF && i2cReadByte(core) == 1,
              "LCD address/data pairs did not auto-increment");
        core.pdc.writeInterruptType(i, UINT32_MAX, 1);
    }
    auto pixels = [&](uint32_t top, uint32_t bottom) {
        core.pdc.drawFrame();
        auto frame = core.pdc.latestFrame();
        check(frame[0] == top && frame[239 * 400 + 399] == top &&
              frame[240 * 400 + 40] == bottom && frame[479 * 400 + 359] == bottom,
              "LCD output did not follow power/blanking state");
    };
    mem.write<uint32_t>(ARM11A, 0x10202204, 0x010000FF); // red
    mem.write<uint32_t>(ARM11A, 0x10202A04, 0x01FF0000); // blue
    pixels(0xFF0000FF, 0xFFFF0000);
    // Subword accesses preserve the other byte lanes and do not reset a panel.
    mem.write<uint8_t>(ARM11A, 0x10202205, 0x12);
    check(mem.read<uint32_t>(ARM11A, 0x10202204) == 0x010012FF, "LCD byte write lost adjacent lanes");
    mem.write<uint8_t>(ARM11A, 0x10202205, 0);
    mem.write<uint8_t>(ARM11A, 0x10202015, 0);
    check(lcdRead(core, 0x2C, 1) == 0x10, "Unused reset byte reset the panel");
    i2cWrite(core, 0x4A, 0x22, 0x10);
    check(i2cRead(core, 0x4A, 0x0F) == 0xA2 && i2cRead(core, 0x4A, 0x13) == 0x10,
          "Top backlight power affected another rail or IRQ");
    pixels(0xFF000000, 0xFFFF0000);
    check(lcdRead(core, 0x2C, 0x62) == 1, "LCD readiness incorrectly depends on backlight power");
    i2cWrite(core, 0x4A, 0x22, 0x20);
    i2cRead(core, 0x4A, 0x13);
    mem.write<uint16_t>(ARM11A, 0x1020200E, 1);
    pixels(0xFF0000FF, 0xFF000000);
    mem.write<uint16_t>(ARM11A, 0x1020200E, 0);
    mem.write<uint32_t>(ARM11A, 0x10202244, 0);
    pixels(0xFF000000, 0xFFFF0000);
    mem.write<uint32_t>(ARM11A, 0x10202244, 0x1023E);
    mem.write<uint32_t>(ARM11A, 0x10202240, 0);
    pixels(0xFF000000, 0xFFFF0000);
    mem.write<uint32_t>(ARM11A, 0x10202240, 0x5F);
    i2cWrite(core, 0x2E, 1, 0x11);
    pixels(0xFF0000FF, 0xFF000000);
    i2cWrite(core, 0x2E, 1, 0x10);
    // Panel power-off shuts down all rails, with only the requested completion.
    i2cWrite(core, 0x4A, 0x22, 1);
    check(i2cRead(core, 0x4A, 0x0F) == 2 && i2cRead(core, 0x4A, 0x13) == 1,
          "Panel power-off emitted extra backlight events");
    pixels(0xFF000000, 0xFF000000);
    initLcd(core);
    mem.write<uint32_t>(ARM11A, 0x10202014, 0);
    check(lcdRead(core, 0x2C, 0xFE) == 0 && lcdRead(core, 0x2E, 1) == 0 &&
          i2cRead(core, 0x4A, 0x0F) == 0xE2, "LCD controller reset corrupted MCU rails");
    pixels(0xFF000000, 0xFF000000);
}
static void testColdSdRead(ScriptSession& session) {
    // open_agb_firm reads through controller 1 without issuing CMD16 first.
    // Exercise both controllers on fresh cores, including FIFO/IRQ completion.
    for (int controller = 0; controller < 2; ++controller) {
        session.start(true);
        auto& mem = session.core->memory;
        uint32_t base = 0x10006000 + controller * 0x1000;
        uint32_t irq = BIT(16 + controller * 2);
        mem.write<uint32_t>(ARM9, 0x10001000, irq);
        mem.write<uint16_t>(ARM9, base + 0xD8, 2); // 32-bit FIFO mode
        mem.write<uint16_t>(ARM9, base + 0x104, 512);
        mem.write<uint16_t>(ARM9, base + 0x100, BIT(1) | BIT(11));
        mem.write<uint32_t>(ARM9, base + 0x20, UINT32_MAX); // FIFO IRQ only
        mem.write<uint32_t>(ARM9, base + 4, 0);
        mem.write<uint16_t>(ARM9, base, 0x1C11); // CMD17, no preceding CMD16
        session.advance();
        check((mem.read<uint16_t>(ARM9, base + 0x100) & 0x300) == 0x300,
              "Cold SD read did not fill a 512-byte FIFO");
        check(mem.read<uint32_t>(ARM9, 0x10001004) & irq, "Cold SD read did not signal FIFO IRQ");
        for (int word = 0; word < 128; ++word) {
            check(!(mem.read<uint32_t>(ARM9, base + 0x1C) & BIT(2)), "SD read completed early");
            check(mem.read<uint32_t>(ARM9, base + 0x10C) == (word ? 0U : 0x1111U),
                  "Cold SD read returned incorrect data");
        }
        check(mem.read<uint32_t>(ARM9, base + 0x1C) & BIT(2), "Cold SD read never completed");
        check(!(mem.read<uint16_t>(ARM9, base + 0x100) & 0x300), "SD FIFO did not drain");
    }
}
static void testCpuInterfaceAliases(Core& core) {
    auto& mem = core.memory;
    for (int i = 0; i < 4; ++i) {
        CpuId target = CpuId(i), caller = CpuId((i + 1) % 4);
        uint32_t alias = 0x17E00200 + i * 0x100;
        check(mem.read<uint32_t>(target, 0x17E00100) == 0, "CPU interface should reset disabled");
        mem.write<uint8_t>(caller, alias, 1);
        check(mem.read<uint32_t>(target, 0x17E00100) == 1, "Aliased enable missed the target core");
        mem.write<uint16_t>(caller, alias + 4, 0xA0 + i * 0x10);
        check(mem.read<uint32_t>(target, 0x17E00104) == 0xA0 + i * 0x10,
              "Aliased priority mask missed the target core");
        mem.write<uint32_t>(target, 0x17E00104, 0xF0);
        check(mem.read<uint8_t>(caller, alias + 4) == 0xF0, "Alias did not read the target core's bank");
        check(mem.read<uint8_t>(caller, alias + 5) == 0, "Alias subword offset changed");
    }
    // Reproduce the GodMode9 LCD wait: initialization uses the fixed core-0
    // interface, then the MCU raises IRQ 0x71. No inherited boot-ROM state.
    core.interrupts.writeMpIge(UINT32_MAX, 1);
    core.interrupts.writeMpTarget(0x71, BIT(ARM11A));
    core.interrupts.writeMpIeSet(3, UINT32_MAX, BIT(0x11));
    core.i2c.mcuInterrupt(BIT(25));
    check(mem.read<uint32_t>(ARM11B, 0x17E00218) == 0x71, "MCU IRQ missing through core-0 alias");
    check(mem.read<uint32_t>(ARM11B, 0x17E00118) == 0x3FF, "MCU IRQ leaked to the caller's bank");
    check(mem.read<uint32_t>(ARM11B, 0x17E0020C) == 0x71, "Aliased IRQ acknowledgement failed");
    check(core.interrupts.readMpIa(ARM11A, 3) == BIT(0x11) &&
          core.interrupts.readMpIa(ARM11B, 3) == 0, "Aliased acknowledgement activated the wrong core");
    check(mem.read<uint32_t>(ARM11A, 0x17E00118) == 0x3FF, "Acknowledgement did not clear pending IRQ");
    mem.write<uint32_t>(ARM11B, 0x17E00210, 0x71);
    check(core.interrupts.readMpIa(ARM11A, 3) == 0, "Aliased EOI did not clear active IRQ");
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected temporary fixture directory");
        std::string dir = argv[1];
        auto digest = [](const std::string& input) {
            auto hash = Sha::digest256(reinterpret_cast<const uint8_t*>(input.data()), input.size());
            std::string result;
            for (uint8_t byte : hash) {
                result += "0123456789abcdef"[byte >> 4];
                result += "0123456789abcdef"[byte & 15];
            }
            return result;
        };
        check(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "Empty SHA-256 failed");
        check(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "Short SHA-256 failed");
        check(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "Two-block SHA-256 padding failed");
        Settings::load(dir, false);
        ScriptSession session(true);
        session.requestCancel();
        session.clearCancel(); // Safe before the lazy Lua runtime exists.
        check(!session.cancelled, "Fresh session did not clear cancellation");
        std::string errors;
        session.output = [&](const std::string& text) { errors += text + "\n"; };
        session.setPath("sd", dir + "/override.img");
        session.setPath("nand", dir + "/nand.bin");
        session.setPath("boot9", dir + "/boot9.bin");
        session.setPath("boot11", dir + "/boot11.bin");
        session.start();
        testLcd(*session.core);
        session.start(true);
        testCpuInterfaceAliases(*session.core);
        testColdSdRead(session);
        session.start(true);
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
        check(session.paths("effective").at("sd") == ScriptSession::absolutePath(dir + "/override.img"),
              "Scripting reset lost path override");
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
        session.clearCancel(); // Also safe after the runtime has been reset.
        session.headless = false;
        for (const char* code : {"while true do pcall(function() while true do end end) end",
                                 "cancelledCo=coroutine.create(function() while true do end end); "
                                 "coroutine.resume(cancelledCo); continuedAfterCancel=true"}) {
            check(
                session.runString(
                    "local id; id=callbacks:add('frame', function() "
                    "local _,_,count=debug.gethook(); assert(count==10000); "
                    "if cancelledCo then local _,_,n=debug.gethook(cancelledCo); assert(n==10000) end; "
                    "assert(continuedAfterCancel==nil); "
                    "local child=coroutine.create(function() local _,_,n=debug.gethook(); assert(n==10000) end); "
                    "assert(coroutine.resume(child)); callbacks:remove(id); console:log('Resume hooks restored') end)"),
                "Could not register cancellation recovery callback");
            std::thread cancel([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                session.requestCancel();
            });
            bool success = session.runString(code);
            cancel.join();
            check(!success, "Cancellation did not interrupt Lua and its caller");
            auto generation = session.cancelGeneration.load();
            errors.clear();
            // Match desktop Resume, with no intervening runString/runFile to restore hooks.
            session.clearCancel();
            session.start(false);
            session.resume();
            check(session.advance() == "frame", "Surviving callback failed after desktop Resume");
            check(errors == "Resume hooks restored\n", "Desktop Resume did not restore Lua hooks");
            check(session.cancelGeneration == generation, "Recovery reset the cancellation generation");
            session.pause();
            check(session.runString("assert(1 + 1 == 2)"), "Runtime did not recover after cancellation");
        }
        session.headless = true;
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
                // A 1 ms sleep can take a full Windows timer tick. This probe
                // checks lock independence, so yield without throttling reads.
                std::this_thread::yield();
            }
        });
        bool responsive =
            session.runString("local id; id = callbacks:add('frame', function() local t=os.clock(); while os.clock()-t "
                              "< 0.15 do end; callbacks:remove(id) end); emu:runFrame()");
        unsigned bootReads = 0, bootReadStart = 0;
        auto previousOutput = session.output;
        session.output = [&](const std::string& message) {
            if (message == "boot begin")
                bootReadStart = reads.load();
            else if (message == "boot end")
                bootReads += reads.load() - bootReadStart;
            else
                previousOutput(message);
        };
        responsive &=
            session.runString("local function boot() console:log('boot begin'); local t=os.clock(); "
                              "while os.clock()-t < 0.15 do end; console:log('boot end') end; "
                              "local a=callbacks:add('start',boot); local b=callbacks:add('reset',boot); "
                              "emu:reset(); emu:stop(); emu:start(); callbacks:remove(a); callbacks:remove(b)");
        session.output = previousOutput;
        for (int i = 0; i < 3; ++i) {
            session.start(true);
            session.advance();
        }
        consume = false;
        consumer.join();
        check(responsive && reads > 20 && framesRead > 0 && bootReads > 20,
              "Audio/display consumers stalled behind Lua");
        session.stop();
        // Screenshots follow the current frame even with an undrained desktop queue.
        session.start();
        initLcd(*session.core);
        check(session.runString("local fb=0x20040000; emu.physical:write32(0x10400468,fb); "
                                "emu.physical:write32(0x10400470,0); emu.physical:write32(0x10400474,1); "
                                "emu.physical:write32(0x10400490,960); emu:runFrame(); emu:runFrame(); "
                                "emu:write32(fb+239*4,0xFF0000FF); "
                                "local id=callbacks:add('frame',function() emu:screenshot('desktop-latest.png') end); "
                                "emu:runFrame(); callbacks:remove(id)"),
              "Desktop screenshot failed");
        auto latest = session.core->pdc.latestFrame();
        check(!latest.empty() && latest[0] == 0xFF0000FF, "Full queue left the latest capture stale");
        session.stop();
        // An 8192-sample host buffer takes 171 ms at 48 kHz, beyond the old 50 ms cap.
        session.start();
        Settings::fpsLimiter = 1;
        session.core->csnd.getSamples(48000, 8192);
        std::atomic<bool> producerDone{false};
        std::thread producer([&] {
            for (unsigned i = 0; i < 2 * (8192 * 130914 / 48000); ++i)
                session.core->csnd.runSample();
            producerDone = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        bool waitedForConsumer = !producerDone.load();
        session.core->csnd.getSamples(48000, 8192);
        producer.join();
        check(waitedForConsumer, "Large audio buffer was overwritten before consumption");
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
        check(session.runString("assert(buf:getX()==0 and buf:getY()==buf:rows())"), "Deferred cursor changed");
        check(snapshot == "1234\n5678\n", "Exact buffer fill scrolled too early");
        check(session.runString("buf:print(9)"), "Buffer numeric print failed");
        check(snapshot == "5678\n9   \n", "Buffer did not scroll on the next character");
        check(session.runString(
                  "buf:advance(8); buf:print('Z'); buf:setSize(2,1); assert(buf:getX()<2 and buf:getY()<1)"),
              "Buffer advance/resize failed");
        // FIRM selection survives script/core lifetimes and never gets saved.
        session.resetScripts();
        session.setPath("firm", dir + "/homebrew.firm");
        session.start();
        session.resume();
        session.setPath("firm", dir + "/hash.firm");
        Core* original = session.core.get();
        try { session.start(true); check(false, "Invalid FIRM boot succeeded"); }
        catch (const std::runtime_error&) {}
        check(session.core.get() == original && session.autoRun, "Invalid FIRM disrupted execution");
        session.setPath("firm", dir + "/homebrew.firm");
        session.start(true);
        check(!session.autoRun, "Reset did not pause continuous execution");
        session.resume();
        check(session.runString("emu:loadFirm(emu:getPaths().effective.firm)"), "FIRM reload failed");
        check(!session.autoRun, "loadFirm did not pause continuous execution");
        session.resetScripts();
        session.stop();
        session.start();
        check(session.core->arms[ARM9].debugPc() == 0x08000000, "FIRM selection was lost");
        check(Settings::save(), "Saving settings with a FIRM override failed");
        check(contents(dir + "/3beans.ini").find("homebrew.firm") == std::string::npos,
              "FIRM override leaked into settings");
        session.clearPath("firm");
        session.stop();
        puts("Session tests passed");
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Session test failed: %s\n", e.what());
        return 1;
    }
}
