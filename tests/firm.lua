local function normalized(path) return path:gsub('\\', '/') end
local a9 = emu:getCPU('ARM9')
local selected = TESTDIR .. '/homebrew.firm'
local function checkLcdHandoff(on)
    local bus = emu.physical
    assert(bus:read32(0x10202014) == (on and 1 or 0))
    assert(bus:read32(0x1020200C) == (on and 0 or 0x10001))
    for i = 0, 1 do
        assert(bus:read32(0x10202240 + i * 0x800) == (on and 0x5F or 0))
        assert(bus:read32(0x10202244 + i * 0x800) == (on and 0x1023E or 0))
    end
    local function readMcu(reg)
        for _, pair in ipairs({{0x4A, 0x82}, {reg, 0x80}, {0x4B, 0x82}}) do
            bus:write8(0x10144000, pair[1]); bus:write8(0x10144001, pair[2])
        end
        bus:write8(0x10144001, 0xA1)
        return bus:read8(0x10144000)
    end
    assert(readMcu(0x0F) == (on and 0xE2 or 2), 'Incorrect LCD/backlight handoff power')
    assert(readMcu(0x22) == 0 and readMcu(0x13) == 0, 'Stale LCD request or boot completion IRQ')
end
assert(emu:getPaths().saved.firm == '')
assert(normalized(emu:getPaths().effective.firm) == selected)
emu:start()
checkLcdHandoff(true)
assert(normalized(emu:getPaths().mounted.firm) == selected)
assert(a9:readRegister('pc') == 0x08000000)
assert(emu:readRegister('pc') == 0x1FF80000)
assert(a9:readRegister('cpsr') == 0xD3)
assert(a9:readRegister('r0') == 2)
assert(a9:readRegister('r2') == 0xBEEF)
local argv = a9:readRegister('r1')
assert(a9:readRange(a9:read32(argv), 16) == 'sdmc:/boot.firm\0')
local fb = a9:read32(argv + 4)
assert(a9:read32(fb) == 0x18300000 and a9:read32(fb + 20) == 0x18446500)
assert(a9:read8(0x10000000) == 1 and a9:read8(0x10000001) == 1)
assert(emu:runFrame() == 'frame')
assert(emu:read32(0x20001000) == 0x42 and emu:read32(0x20001004) == 0x24)
-- A framebuffer-only UI is visible without booting the SD firmware chain.
a9:writeRange(0x18300000, string.rep('\0\0\255', 400 * 240))
emu:runFrame()
emu:screenshot(TESTDIR .. '/firm-ui.png')
assert(emu:getCPU('ARM11B'):step(5) == 'timeout')
-- Requested screen setup must support GPU work as well as scanout.
local bus = emu.physical
assert(bus:read32(0x17E00100) == 0 and bus:read32(0x17E00400) == 0 and bus:read32(0x17E00500) == 0)
assert(bus:read8(0x10141312) == 0 and bus:read8(0x10141313) == 0)
if bus:read16(0x10140FFC) & 2 ~= 0 then
    for _, name in ipairs({'ARM11C', 'ARM11D'}) do
        local cpu = emu:getCPU(name)
        assert(cpu:step(5) == 'timeout' and cpu:readRegister('pc') == 0x10000)
    end
end
assert(bus:read32(0x10141200) == 0x1007F)
for engine = 0, 1 do
    local regs, destination = 0x10400010 + engine * 16, 0x20002000 + engine * 0x100
    local pattern = 0x12345678 + engine
    bus:write32(regs, destination >> 3)
    bus:write32(regs + 4, (destination + 16) >> 3)
    bus:write32(regs + 8, pattern)
    bus:write32(regs + 12, 0x201) -- 32-bit fill; start
    emu:runFrame()
    assert(bus:read32(regs + 12) & 2 ~= 0, 'GPU fill did not complete')
    for offset = 0, 12, 4 do assert(bus:read32(destination + offset) == pattern) end
end
bus:write32(0x10400C00, 0x20002000 >> 3)
bus:write32(0x10400C04, 0x20002200 >> 3)
bus:write32(0x10400C10, 8) -- texture copy
bus:write32(0x10400C20, 16)
bus:write32(0x10400C18, 1)
emu:runFrame()
assert(bus:read32(0x10400C18) & 0x100 ~= 0, 'GPU copy did not complete')
assert(bus:readRange(0x20002200, 16) == bus:readRange(0x20002000, 16))
local frames = emu:currentFrame()
for _, bad in ipairs({'short', 'hash', 'magic', 'offset', 'size', 'alignment', 'mmio', 'wrap',
    'reserved', 'copy', 'entry', 'noarm9', 'private', 'misaligned-entry', 'overlap-file',
    'overlap-ram', 'framebuffers'}) do
    local ok, message = pcall(function() emu:loadFirm(TESTDIR .. '/' .. bad .. '.firm') end)
    assert(not ok and message:find('Invalid homebrew FIRM'), bad .. ': ' .. tostring(message))
    assert(emu:currentFrame() == frames)
    assert(normalized(emu:getPaths().effective.firm) == selected)
    assert(normalized(emu:getPaths().mounted.firm) == selected)
end
local callback = callbacks:add('frame', function()
    assert(not pcall(function() emu:loadFirm(selected) end))
end)
emu:runFrame()
callbacks:remove(callback)
-- Rebuild the host file in place; reset must reread it (the SD is untouched).
local input = assert(io.open(TESTDIR .. '/replacement.firm', 'rb'))
local bytes = input:read('a'); input:close()
local output = assert(io.open(selected, 'wb')); output:write(bytes); output:close()
emu:reset()
assert(emu:currentFrame() == 0)
checkLcdHandoff(true)
emu:runFrame()
assert(emu:read32(0x20001000) == 0x43)
-- A failed reset also preserves the mounted image and running core.
output = assert(io.open(selected, 'wb')); output:write('bad'); output:close()
frames = emu:currentFrame()
assert(not pcall(function() emu:reset() end))
assert(emu:currentFrame() == frames and emu:read32(0x20001000) == 0x43)
output = assert(io.open(selected, 'wb')); output:write(bytes); output:close()
assert(emu:loadFirm(TESTDIR .. '/delayed.firm'))
checkLcdHandoff(false)
assert(a9:readRegister('r0') == 1 and a9:read32(a9:readRegister('r1') + 4) == 0)
assert(bus:read32(0x10141200) == 0, 'Unrequested GPU setup was inherited across reset')
bus:write32(0x10400010, 0x20002000 >> 3)
bus:write32(0x10400014, 0x20002010 >> 3)
bus:write32(0x10400018, 0x12345678)
bus:write32(0x1040001C, 0x201)
emu:runFrame()
assert(bus:read32(0x20002000) == 0 and bus:read32(0x1040001C) & 2 == 0)
assert(emu:read32(0x20001004) == 0)
a9:write32(0x1FFFFFFC, 0x1FF80000)
emu:runFrame()
assert(emu:read32(0x20001004) == 0x24)
assert(emu:loadFirm(TESTDIR .. '/thumb.firm'))
assert(a9:readRegister('pc') == 0x20002000 and a9:readRegister('cpsr') & 0x20 ~= 0)
emu:runFrame()
assert(a9:readRegister('r4') == 0x45 and emu:read32(0x20001004) == 0x24)
-- Core 1 uses the boot11 mailbox + SGI 1 protocol, independently of core 0.
local b = emu:getCPU('ARM11B')
local function signal(id) bus:write32(0x17E01F00, (2 << 16) | id) end
local function marker() return bus:read32(0x20001008) end
local function checkStandby()
    local pc = b:readRegister('pc')
    assert(marker() == 0 and pc >= 0x1FFFFC40 and pc <= 0x1FFFFC64,
        string.format('Core 1 left standby without a valid launch: pc=%08X', pc))
end
local function checkLaunched(value, thumb)
    emu:runFrame()
    assert(marker() == value, 'Secondary CPU did not execute the payload')
    assert((b:readRegister('cpsr') & 0x20 ~= 0) == thumb)
    assert(b.physical:read32(0x17E01280) & 2 ~= 0, 'Boot SGI should remain pending for the payload')
end
emu:loadFirm(TESTDIR .. '/secondary.firm')
assert(bus:read32(0x17E00300) == 1 and bus:read32(0x17E01000) == 1)
emu:runFrame()
assert(marker() == 0 and b:step(5) == 'timeout')
bus:write32(0x1FFFFFDC, 0x1FF80100)
emu:runFrame()
assert(marker() == 0, 'Mailbox write alone launched core 1')
signal(0)
emu:runFrame()
checkStandby()
signal(1)
checkLaunched(0x61, false)
-- Reset must discard the old mailbox and restore standby, including a null-entry wake.
emu:reset()
assert(bus:read32(0x1FFFFFDC) == 0 and marker() == 0)
signal(1)
-- The boot SGI stays pending, so later interrupt checks can wake WFI again.
-- Every wake must remain in standby while the mailbox is empty.
for i = 1, 3 do emu:runFrame(); checkStandby() end
bus:write32(0x1FFFFFDC, 0x1FF80121)
signal(1)
checkLaunched(0x62, true)
-- A valid interrupt posted before the first frame must not be lost.
emu:loadFirm(TESTDIR .. '/secondary.firm')
bus:write32(0x1FFFFFDC, 0x1FF80100)
signal(1)
checkLaunched(0x61, false)
emu:clearPathOverride('firm')
assert(emu:getPaths().effective.firm == '' and emu:getPaths().mounted.firm ~= '')
emu:reset()
assert(emu:getPaths().mounted.firm == '')
checkLcdHandoff(false)
assert(a9:readRegister('pc') == 0xFFFF0000 and emu:readRegister('pc') == 0x10000)
assert(bus:read32(0x10141200) == 0 and bus:read32(0x17E00300) == 0 and bus:read32(0x17E01000) == 0)
print('FIRM tests passed')
