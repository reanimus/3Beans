#include "firm.h"
#include "core.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {
uint32_t word(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool within(uint32_t address, uint32_t size, uint32_t low, uint32_t high) {
    return address >= low && uint64_t(address) + size <= high;
}
bool overlaps(uint32_t a, uint32_t as, uint32_t b, uint32_t bs) {
    return uint64_t(a) < uint64_t(b) + bs && uint64_t(b) < uint64_t(a) + as;
}
}

FirmImage::FirmImage(const std::string &path): path(path) {
    auto fail = [&](const std::string &reason) {
        throw std::runtime_error("Invalid homebrew FIRM: " + reason + " (" + path + ")");
    };
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) fail("cannot open file");
    auto length = file.tellg();
    // Bound allocation even for sparse/malicious host files. This covers all
    // supported destination RAM plus the header, including DSP WRAM.
    if (length < 0x200 || length > 0x08800200) fail("file size out of range");
    data.resize(size_t(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), data.size())) fail("short read");
    if (memcmp(data.data(), "FIRM", 4)) fail("missing FIRM magic");
    arm11Entry = word(&data[8]);
    arm9Entry = word(&data[12]);
    screenInit = data[0x10] & 1;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *header = &data[0x40 + i * 0x30];
        Section section = {word(header), word(header + 4), word(header + 8)};
        if (!section.size) continue;
        if (section.offset < 0x200 || (section.offset & 0x1FF) || (section.size & 0x1FF) ||
            (section.address & 3) || uint64_t(section.offset) + section.size > data.size())
            fail("section bounds or alignment");
        if (word(header + 12) > 2) fail("unknown section copy method");
        // Reserve the top 1 KiB of AXI WRAM for the ARM11 launch mailbox/stub.
        if (!within(section.address, section.size, 0x08000000, 0x08100000) &&
            !within(section.address, section.size, 0x18000000, 0x18600000) &&
            !within(section.address, section.size, 0x1FF00000, 0x1FFFFC00) &&
            !within(section.address, section.size, 0x20000000, 0x28000000))
            fail("section destination is outside supported RAM");
        for (const auto &previous : sections)
            if (overlaps(section.address, section.size, previous.address, previous.size) ||
                overlaps(section.offset, section.size, previous.offset, previous.size))
                fail("overlapping sections");
        if (screenInit && (overlaps(section.address, section.size, 0x18300000, 0x7E900) ||
            overlaps(section.address, section.size, 0x18400000, 0x7E900)))
            fail("section overlaps requested framebuffers");
        auto hash = Sha::digest256(&data[section.offset], section.size);
        if (memcmp(hash.data(), header + 16, hash.size())) fail("section SHA-256 mismatch");
        sections.push_back(section);
    }
    auto entryValid = [&](uint32_t entry, bool arm9) {
        unsigned width = (entry & 1) ? 2 : 4;
        uint32_t address = entry & ~uint32_t(1);
        if (address & (width - 1)) return false;
        if (!arm9 && address < 0x18000000) return false; // ARM9 RAM is private.
        for (const auto &section : sections)
            if (within(address, width, section.address, section.address + section.size)) return true;
        return false;
    };
    if (!arm9Entry || !entryValid(arm9Entry, true)) fail("ARM9 entrypoint is not in a section");
    if (arm11Entry && !entryValid(arm11Entry, false)) fail("ARM11 entrypoint is not in shared RAM");
}

void FirmImage::boot(Core &core) const {
    // Map DSP WRAM linearly, as at a homebrew FIRM handoff.
    for (unsigned i = 0; i < 16; ++i)
        core.memory.write<uint8_t>(ARM11A, 0x10140000 + i, 0x80 | ((i & 7) << 2));
    for (const auto &section : sections) {
        for (uint32_t offset = 0; offset < section.size;) {
            uint32_t address = section.address + offset;
            uint32_t count = std::min(section.size - offset, 0x1000 - (address & 0xFFF));
            MemMap &map = core.memory.memMap9[address >> 12];
            // Every destination was validated and mapped before construction.
            memcpy(map.write + (address & 0xFFF), &data[section.offset + offset], count);
            ++map.tag;
            offset += count;
        }
    }

    // boot9strap-compatible argument ABI. ITCM mirrors preserve argv when a
    // payload reconfigures TCMs (e.g. GodMode9 uses the 0x01FF8000 mirror).
    core.cp15.writeReg(ARM9, 9, 1, 0, 0xFFF0000A);
    core.cp15.writeReg(ARM9, 9, 1, 1, 0x24);
    core.cp15.writeReg(ARM9, 1, 0, 0, 0x52078); // TCMs/high vectors on; caches/MPU off
    const uint32_t argv = 0x01FF8000;
    core.cp15.write<uint32_t>(ARM9, argv, argv + 0x20);
    core.cp15.write<uint32_t>(ARM9, argv + 4, screenInit ? argv + 0x40 : 0);
    const char guestPath[] = "sdmc:/boot.firm";
    for (unsigned i = 0; i < sizeof(guestPath); ++i)
        core.cp15.write<uint8_t>(ARM9, argv + 0x20 + i, guestPath[i]);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t top = 0x18300000 + i * 0x100000;
        core.cp15.write<uint32_t>(ARM9, argv + 0x40 + i * 12, top);
        core.cp15.write<uint32_t>(ARM9, argv + 0x44 + i * 12, top);
        core.cp15.write<uint32_t>(ARM9, argv + 0x48 + i * 12, top + 0x46500);
    }
    if (screenInit) {
        // Luma's screen setup also releases the GPU engines from reset.
        // Framebuffer presentation alone does not enable fills or copies.
        core.gpu.writeCfg11GpuCnt(-1, 0x1007F);
        core.pdc.writeLcdReset(-1, 1);
        core.pdc.writeLcdSignal(-1, 0);
        for (int i = 0; i < 2; ++i) {
            core.pdc.writeLcdFill(i, -1, 0);
            core.pdc.writeLcdBrightness(i, -1, 0x5F); // Luma's default brightness
            core.pdc.writeLcdPwm(i, -1, 0x1023E);
            core.pdc.writeFramebufLt0(i, -1, 0x18300000 + (i ? 0x46500 : 0));
            core.pdc.writeFramebufLt1(i, -1, 0x18400000 + (i ? 0x46500 : 0));
            core.pdc.writeFramebufFormat(i, -1, 1); // BGR8, rotated 240-pixel rows
            core.pdc.writeFramebufStep(i, -1, 720);
            core.pdc.writeInterruptType(i, -1, 0x10501);
        }
        core.i2c.initFirmLcd();
    }

    // A zero ARM11 entry is allowed: ARM9 may publish it later. Keep this
    // tiny guest polling loop in the same reserved area as boot9strap.
    const uint32_t poll[] = {0xE3A00202, // mov r0, #0x20000000
        0xE5101004,                    // ldr r1, [r0, #-4]
        0xE3510000,                    // cmp r1, #0
        0x0AFFFFFC,                    // beq to ldr
        0xE12FFF11};                   // bx r1
    for (unsigned i = 0; i < sizeof(poll) / sizeof(poll[0]); ++i)
        core.memory.write<uint32_t>(ARM9, 0x1FFFFC00 + i * 4, poll[i]);
    core.memory.write<uint32_t>(ARM9, 0x1FFFFFFC, arm11Entry);

    // Core 1 normally inherits boot11's standby protocol: SGI 1 wakes it and
    // 0x1FFFFFDC supplies its entrypoint. Use our own guest stub so the handoff
    // does not depend on code offsets in a particular boot-ROM dump. Like
    // boot11, leave the SGI pending for the payload's interrupt initialization.
    const uint32_t standby[] = {
        0xE59F0020, // ldr r0, [pc, #0x20] (pending register)
        0xE5901000, // ldr r1, [r0]
        0xE3110002, // tst r1, #2 (SGI 1)
        0x0A000003, // beq wait
        0xE59F0014, // ldr r0, [pc, #0x14] (mailbox)
        0xE5900000, // ldr r0, [r0]
        0xE3500000, // cmp r0, #0
        0x112FFF10, // bxne r0 (ARM or Thumb)
        0xEE070F90, // wait: mcr p15, 0, r0, c7, c0, 4 (ARM11 WFI)
        0xEAFFFFF5, // b to pending check
        0x17E01280, 0x1FFFFFDC
    };
    for (unsigned i = 0; i < sizeof(standby) / sizeof(standby[0]); ++i)
        core.memory.write<uint32_t>(ARM9, 0x1FFFFC40 + i * 4, standby[i]);
    core.memory.write<uint32_t>(ARM9, 0x1FFFFFDC, 0);
    core.interrupts.writeMpIge(-1, 1);
    core.interrupts.writeMpIle(ARM11B, -1, 1);
    core.memory.write<uint8_t>(ARM9, 0x10000000, 1); // protected boot9 locked
    core.memory.write<uint8_t>(ARM9, 0x10000001, 1); // protected boot11 locked
    core.arms[ARM9].init(arm9Entry & ~1U, 0xD3 | ((arm9Entry & 1) << 5));
    *core.arms[ARM9].registers[0] = screenInit ? 2 : 1;
    *core.arms[ARM9].registers[1] = argv;
    *core.arms[ARM9].registers[2] = 0xBEEF;
    *core.arms[ARM9].registers[13] = 0x08000000; // top of ITCM mirror
    core.arms[ARM11A].init(arm11Entry ? arm11Entry & ~1U : 0x1FFFFC00,
        0x1D3 | ((arm11Entry & 1) << 5));
    // Start already waiting, with the PC at the post-WFI pending check. An
    // SGI sent before the first frame therefore cannot be lost at initial WFI.
    core.arms[ARM11B].init(0x1FFFFC40, 0xD3);
    for (CpuId id : {ARM11B, ARM11C, ARM11D}) core.arms[id].halt(BIT(0));
}
