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

CLI options `--sd`, `--nand`, `--boot9`, `--boot11`, and `--firm` set session overrides before scripts
load. Lua can change or clear them:

```lua
emu:setPathOverride('sd', 'alternate.img')
emu:start()  -- or emu:reset() if already booted
local paths = emu:getPaths()
console:log(paths.mounted.sd)
emu:clearPathOverride('sd')
emu:reset()  -- now uses the saved SD path
```

Names are `sd`, `nand`, `boot9`, `boot11`, and `firm`. `getPaths()` returns `saved`, `effective`
(next boot), and `mounted` tables; `mounted` is empty without a core. Overrides are
resolved to absolute paths when supplied, validated, and checked again before reboot.
Preflight validation failure preserves the current core. A later construction failure
(for example a file disappearing after validation) leaves the session stopped.
Boot ROMs must contain at least 64 KiB. SD/NAND overrides must open read/write.

Changing a path never hot-swaps an image. Overrides survive emulator resets, script
reloads, and scripting-environment resets, until explicitly cleared or process exit.
Saved Path Settings still edits the persistent values. The title indicates active
overrides; saving settings never persists those overrides. The images themselves retain
normal writable behavior. `firm` is always empty in `saved`; it has no persistent setting.

## Direct homebrew FIRM loading

Load a host build directly without copying it into your SD image:

```sh
3beans --firm build/my-homebrew.firm --sd test.img
```

On desktop, this starts the FIRM automatically when no `--script` is supplied.
**System → Restart** rereads the FIRM file, so rebuilding and restarting is sufficient
to test the next version. Boot ROM paths still come from saved settings or `--boot9`
and `--boot11`. The SD/NAND images remain available to the payload as storage.

For automated tests:

```sh
3beans --headless --firm build/my-homebrew.firm --sd test.img --script test.lua --timeout 60
```

```lua
emu:start()  -- loads the --firm selection, initially paused
for i = 1, 120 do emu:runFrame() end
emu:screenshot('ui.png')
emu:loadFirm('build/another.firm')  -- selects and boots immediately, initially paused
emu:reset()                       -- rereads that same host file
emu:clearPathOverride('firm')
emu:reset()                       -- returns to the normal boot-ROM/SD/NAND boot chain
```

`emu:setPathOverride('firm', path)` selects the next boot without replacing the current
core. `emu:loadFirm(path)` also reboots, returns true on success, and restores the previous
selection on failure. Header, range, entrypoint, overlap, and SHA-256 checks happen before
the current core is destroyed. Clearing the override does not reboot until requested.
The FIRM selection survives Stop, emulator resets, and Reset scripting, and never enters
`3beans.ini`. A failed rebuild or truncated FIRM leaves the current core available.

This is a homebrew handoff, with these conventions:

- Unencrypted FIRM sections load into ARM9 RAM (first 1 MiB), VRAM, DSP/AXI WRAM,
  or the first 128 MiB of FCRAM. Section offsets/sizes use 512-byte alignment and
  destinations use 4-byte alignment. RSA signatures are not required.
- ARM9 starts in supervisor mode with IRQ/FIQ disabled and MPU/caches disabled.
  ITCM/DTCM are enabled; `r0` is argc, `r1` points to argv in the `0x01FF8000` ITCM
  mirror, and `r2` is `0xBEEF`. `argv[0]` is the conventional `sdmc:/boot.firm` name;
  it does **not** create a file on the SD or expose the host path to the guest filesystem.
- FIRM header byte `0x10`, bit 0 requests initialized BGR8 framebuffers. When set,
  argc is 2 and `argv[1]` describes two framebuffer sets at `0x18300000`/`0x18400000`,
  with bottom screens at `+0x46500`. GPU engines are enabled (`CFG11_GPU_CNT = 0x1007F`)
  for fills and copies as well as display output. LCD controllers are out of reset,
  signals are routed, PWM is enabled at Luma's default brightness, and MCU panel and
  backlight rails are on. Boot power operations are complete with no stale completion
  interrupts. Otherwise argc is 1 and GPU/LCD setup belongs to the payload. Sections
  must not overlap requested framebuffers.
- ARM11A starts at its entrypoint with MMU/caches and interrupts disabled. A zero
  entrypoint leaves it polling `0x1FFFFFFC`, allowing ARM9 to launch it later. The top
  1 KiB of AXI WRAM is reserved for this handoff.
  Both ARM and Thumb entrypoints are supported; ARM9 must have a nonzero entrypoint.
- ARM11B (core 1) starts in standby with interrupts masked in CPSR, its interrupt
  interface enabled, and the global distributor enabled. To launch it, write an ARM
  or Thumb entrypoint to `0x1FFFFFDC` and send SGI 1 to core 1. A mailbox write alone,
  an unrelated interrupt, or a zero entrypoint does not launch it. The boot SGI remains
  pending for the payload to acknowledge, as with boot11. On New 3DS, cores 2/3 remain
  powered off until the payload starts them through the normal hardware registers.
- Both 64 KiB boot ROM dumps are still required: payloads such as GodMode9 call
  unprotected boot-ROM routines. Protected halves are locked at handoff. Direct loading
  skips boot-ROM execution and does not reproduce its AES key setup, OTP caches, or the
  entire hardware state left by boot9strap. NAND decryption and payloads relying on that
  state may require normal boot instead. This does not add Horizon or GBA-mode support.

The ABI follows [boot9strap's chainloader](https://github.com/SciresM/boot9strap/blob/934e10092a9caab0dbc83038449878862b7fc7f7/stage2/arm9/source/chainloader.c)
and [entry handoff](https://github.com/SciresM/boot9strap/blob/934e10092a9caab0dbc83038449878862b7fc7f7/stage2/arm9/source/chainloader_entry.s).
Automated tests cover synthetic payloads on Old and New 3DS, delayed ARM11 launch,
core 1 mailbox/SGI handoff (including ARM/Thumb, early interrupts, null entrypoints,
and reset), GPU fills/copies, framebuffer output, rebuilding/reset, malformed files,
and returning to normal boot. They also cover
the fixed ARM11 interrupt-controller aliases used by GodMode9 during LCD initialization
and reading an SD card with its default block length, as open_agb_firm does.
LCD reset, signal blanking, color fill, PWM enable/zero duty, controller power, and
MCU backlight power affect captured and displayed output. LCD I2C reads use address/data
pairs via register `0x40`; the controllers report revision 1. MCU power changes complete
synchronously. Analog startup delays, calibrated luminance, and adaptive backlight
processing are not modeled.
GodMode9 v2.2.3 (20260331144941) has been checked on macOS for direct boot, browsing
the SD card, and resetting. open_agb_firm nightly `401c3dff900f658d3edf77ababcfaf5dca54a0a8`
has been checked for reaching its file selector, navigating directories, and resetting.
These checks do not establish full crypto/NAND functionality or GBA game support.

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
| `emu:loadFirm(path)` | Select a host homebrew FIRM and reboot; returns true |
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
