-- Run from a directory containing these two disposable test images.
for _, image in ipairs({'sd-a.img', 'sd-b.img'}) do
    emu:setPathOverride('sd', image)
    emu:reset()
    for i = 1, 120 do assert(emu:runFrame() == 'frame') end
    emu:screenshot(image .. '.png')
end
emu:clearPathOverride('sd') -- Saved SD path will be used on the next start/reset.
