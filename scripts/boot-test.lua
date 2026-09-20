-- Pass boot paths on the command line. Assertions produce a failing process status.
emu:start()
local frames = 120
for i = 1, frames do assert(emu:runFrame() == 'frame') end
assert(emu:currentFrame() == frames)
emu:screenshot('boot.png')
console:log('Boot test completed')
