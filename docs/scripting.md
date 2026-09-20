# Lua scripting

3Beans embeds Lua 5.4.7 on every target, with mGBA-style `emu`, `callbacks`, `console`, and `C` objects.
Use colon calls (`emu:runFrame()`). GB/GBA scripts need changes for 3DS addresses,
CPU selection, and keys. Standard Lua libraries are available; scripts run with your
normal filesystem permissions. Scripting is not a sandbox.

## Launch and lifetime

**File → Scripting** opens the console. Load files with **Load script**, or enter Lua
in the command field and press Return. **Cancel execution** interrupts Lua and emulator
advancement. **Reset scripting** clears globals, callbacks, breakpoints, text buffers,
and script-held input. Closing the console hides it; scripts remain active.

```sh
3beans --headless --script scripts/boot-test.lua --sd test.img --timeout 60
```

`--script` can repeat; files execute in order in one Lua environment. In headless mode
there is no implicit boot or execution after scripts return. Use `emu:start()`, explicit
frame/instruction advancement, and assertions. Exit status is 0 for success, 1 for
script/boot/file-open failure, and 2 for invalid command-line syntax or values. `--timeout` applies only to headless mode and bounds total execution in
seconds (up to 86400); it interrupts Lua bytecode and emulator execution, but cannot
interrupt a blocking external library or OS call. Catching a timeout with `pcall` or
`xpcall` does not permit continued execution; the next Lua instruction is interrupted. No display or audio device is opened.
Software rendering and disabled audio pacing are runtime-only choices.

Without `--headless`, scripts run on the desktop emulation thread. `emu:resume()` enables
continuous desktop execution after the current script returns. Scripts and GUI input
are serialized. Commands queued while a script runs execute after it returns. Pause/Restart/Stop
queue behind the script. Cancel interrupts the active script and discards previously
queued script commands; new commands can run afterward. Settings dialogs queue at a
worker boundary, pause emulation while open, and restore the previous run state on
close. A running script must finish or be cancelled before its queued dialog opens. Keep automation loops bounded when using the desktop.

`--config-dir DIR` selects the settings directory. Otherwise 3Beans uses `3beans.ini`
in the working directory if present, then the platform's application settings directory.
Headless mode does not create or save `3beans.ini`. MCU RAM in the configuration
directory, cartridge saves, and mounted SD/NAND contents retain normal writable behavior. Script-directory Lua
module paths are added to `package.path`; filesystem paths remain relative to the process
working directory.

## Temporary boot paths

CLI options `--sd`, `--nand`, `--boot9`, and `--boot11` set session overrides before scripts
load. Lua can change or clear them:

```lua
emu:setPathOverride('sd', 'alternate.img')
emu:start()  -- or emu:reset() if already booted
local paths = emu:getPaths()
console:log(paths.mounted.sd)
emu:clearPathOverride('sd')
emu:reset()  -- now uses the saved SD path
```

Names are `sd`, `nand`, `boot9`, and `boot11`. `getPaths()` returns `saved`, `effective`
(next boot), and `mounted` tables; `mounted` is empty without a core. Overrides are
resolved to absolute paths when supplied, validated, and checked again before reboot.
Preflight validation failure preserves the current core. A later construction failure
(for example a file disappearing after validation) leaves the session stopped.
Boot ROMs must contain at least 64 KiB. SD/NAND overrides must open read/write.

Changing a path never hot-swaps an image. Overrides survive emulator resets, script
reloads, and scripting-environment resets, until explicitly cleared or process exit.
Saved Path Settings still edits the persistent values. The title indicates active
overrides; saving settings never persists those overrides. The images themselves retain
normal writable behavior. FIRM files boot through the normal firmware/SD boot chain;
there is no direct FIRM loader.

## Execution and events

| Method | Result |
|---|---|
| `emu:start()` | Boot if no core exists; initially paused |
| `emu:reset()` | Reconstruct the core with effective paths; initially paused |
| `emu:stop()` | Unload the core |
| `emu:pause()` / `resume()` | Pause execution / enable continuous desktop execution |
| `emu:runFrame()` | Advance to the next completed frame; return `"frame"` or `"paused"` |
| `cpu:step([timeoutMs])` | Advance until this CPU executes one instruction; return `"step"`, `"paused"`, or `"timeout"`; default 1000 ms |
| `emu:currentFrame()` / `currentCycle()` | Frame and master-clock counts since boot/reset |
| `emu:frequency()` / `frameCycles()` | Master clock frequency and integer cycles per frame |
| `emu:loadFile(path)` | Select a cartridge and reboot; returns true |
| `emu:screenshot(path)` | Write the latest completed combined 400×480 display as PNG |

Stepping advances the shared scheduler: other CPUs and devices can progress before the
selected CPU's next instruction. While a halted selected CPU waits for its host-time deadline, other CPUs, devices,
and frame callbacks continue to advance. Halted CPUs time out; ARM11C/D are unavailable in Old
3DS mode. `runFrame()` starts explicit execution even if paused. Screenshots require a
completed captured frame and do not consume the desktop presentation queue. Captures
remain current even if the desktop presenter falls behind.

```lua
local id = callbacks:add('frame', function()
    if emu:currentFrame() == 120 then emu:pause() end
end)
callbacks:remove(id)
```

Emitted events are `start` (new boot, including `reset()` without a core), `reset`
(restart of an existing core), `stop` (before unload), and
`frame` (after completed display capture). Registrations run in registration order.
New callbacks wait until the next event; removed callbacks are skipped immediately.
Other event names may be registered for script compatibility, but are not emitted.
Lua state and registrations survive resets; frame/cycle counts reset.

Callbacks may inspect/mutate memory and registers, change input, update registrations,
and request pause. They may not start/stop/reset/load a core, step, or advance a frame
recursively. Uncaught errors produce tracebacks, disable the failing registration, and
pause desktop execution. They fail a headless run even if a surrounding `pcall` catches
the frame-advancement error.

## CPUs, memory, and registers

`C.CPU` values are string names; `emu:getCPU(C.CPU.ARM9)` returns a CPU object. Other names are `ARM11A`, `ARM11B`,
`ARM11C`, and `ARM11D`. Unqualified memory/register/debugger methods on `emu` target ARM11A.
CPU and memory-domain handles resolve to the current core after reset; access without a
core fails safely.

Each CPU provides `read8/16/32(address)`, `write8/16/32(address, value)`,
`readRange(address, length)`, and `writeRange(address, binaryString)`. These use the
CPU's virtual MMU/TCM address space, including existing emulator alignment and MMIO
behavior. Reads of MMIO can have side effects. `cpu.physical` exposes the same methods
on the physical bus. Addresses/values are unsigned; overflowing ranges are rejected.
Ranges are limited to 16 MiB per call and are binary Lua strings, including NUL bytes.

`emu.memory` exposes raw `arm9ram`, `vram`, `dspwram`, `axiwram`, `fcram`, `boot9`,
`boot11`, `itcm`, `dtcm`, `fcramExt`, and `vramExt` domains. The extended domains require
New 3DS. Each provides the memory methods with offsets relative to the raw backing
region, plus `name()`, `base()`, `bound()` (exclusive), and `size()`. Raw domains bypass
mapping/protection; boot-ROM domains are read-only. Not every byte of a raw backing
region is necessarily currently mapped into a CPU's address space.

`cpu:readRegister(name)` / `writeRegister(name, value)` support `r0`–`r15`, `sp`, `lr`,
`pc`, `cpsr`, and the current mode's `spsr`. PC is the address of the **next instruction**,
not the pipeline-adjusted hardware register. CPSR mode changes switch register banks;
PC and ARM/Thumb changes refill the pipeline. SPSR is unavailable in user/system mode.
Script memory writes refresh instruction pipelines so code patches take effect.

## ARM breakpoints and watchpoints

```lua
local cpu = emu:getCPU('ARM9')
local bp = cpu:setBreakpoint(function(info)
    console:log(string.format('PC = %08X', info.pc))
    emu:pause()
end, 0xFFFF0000)
local wp = cpu:setRangeWatchpoint(function(info)
    console:log(string.format('%08X = %08X', info.address, info.value))
end, 0x08000000, 0x08000004, C.WATCHPOINT_TYPE.WRITE)
cpu:clearBreakpoint(bp) -- also removes watchpoints by ID
```

`setWatchpoint(callback, address, type)` watches one byte; `setRangeWatchpoint` uses an
exclusive upper bound. Types match mGBA: `C.WATCHPOINT_TYPE.WRITE = 1`, `READ = 2`, and `RW = 3`.
`WRITE_CHANGE` is unsupported.
Any overlapping CPU data access matches, including direct-memory and MMIO paths.

Execution callbacks fire before an instruction; data callbacks fire after the instruction
completes. They continue execution unless they request pause. Resuming/stepping past a
paused breakpoint suppresses that same hit once. Callback fields are `cpu` (name), `pc`,
`address`, `width` (bytes), `value` (transferred data), and `accessType` (READ or WRITE).
Writes also expose `newValue`. Unlike mGBA, execution callbacks have `width = 0` and
`accessType = 0`; `segment` and `oldValue` are unavailable. Reading MMIO to obtain an
old value could itself change emulated device state. ARM9 watch addresses are aligned
to the access width; ARM11 addresses retain their byte offset.

Only ARM CPU data accesses trigger watchpoints. Instruction fetches, Lua inspection,
physical device/DMA traffic, GPU, and DSP accesses do not. This implementation does not
add a remote debugger, DSP stepping, or DMA watchpoints.

## Input and console

`C.KEY` contains zero-based indices: A, B, SELECT, START, RIGHT, LEFT, UP, DOWN, R, L,
X, Y. Use `setKeys(mask)`, `addKeys(mask)`, `clearKeys(mask)`, `addKey(index)`,
`clearKey(index)`, `getKeys()`, and `getKey(index)`. Queries reflect combined host/script
input; `getKey` returns integer 0 or 1, matching mGBA. Script key releases never release a physically held key.

3DS controls: `pressScreen(x, y)` (x 0–319, y 0–239), `releaseScreen()`,
`setLStick(x, y)` (each −2047…2047), `releaseLStick()`, `pressHome()`, `releaseHome()`.
Script touch/stick ownership overrides the corresponding host position until released.
Resetting scripting releases script input and restores host input.

`console:log/warn/error(text)` and `print(...)` write to the desktop console or headless
stdout. Logging an error message does not throw; use Lua `error()`/`assert()` for test
failures. `console:createBuffer([name])` creates a named monospace text buffer with
`print`, `clear`, `setSize(cols, rows)`, `moveCursor(x, y)`, `advance(columns)`, `setName`,
`getX`, `getY`, `cols`, and `rows` methods. Buffers wrap and scroll within their bounds;
headless output includes text snapshots. Buffer names must be unique. A completely
filled buffer scrolls only when another character or cursor advance needs space. At
exact fill, `(getX(), getY())` is `(0, rows())`, a pending-wrap position beyond the
last visible row. `moveCursor` accepts only visible positions and cancels that wrap.

Savestates, graphical overlays, sockets, persistent script storage, platform-specific
GB/GBA peripherals, and full mGBA script compatibility are not included.
