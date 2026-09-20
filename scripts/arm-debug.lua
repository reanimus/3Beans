local arm9 = emu:getCPU(C.CPU.ARM9)
local hit = false
local breakpoint = arm9:setBreakpoint(function(info)
    console:log(string.format('%s paused before 0x%08X', info.cpu, info.pc))
    hit = true
    emu:pause()
end, 0xFFFF0000)
emu:start()
assert(emu:runFrame() == 'paused' and hit)
assert(arm9:step() == 'step')
console:log(string.format('Next instruction: 0x%08X', arm9:readRegister('pc')))
arm9:clearBreakpoint(breakpoint)
