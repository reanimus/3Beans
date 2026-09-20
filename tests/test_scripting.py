#!/usr/bin/env python3
"""CLI integration tests: no console dumps, display, sound device, or third-party Python modules."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else './3beans').resolve())
source = Path(__file__).resolve().parent


def png_first_pixel(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, compressed = 8, b''
    while offset < len(data):
        size = struct.unpack('>I', data[offset:offset + 4])[0]
        kind, payload = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
        if kind == b'IHDR':
            assert struct.unpack('>IIBB', payload[:10]) == (400, 480, 8, 6)
        if kind == b'IDAT':
            compressed += payload
        offset += size + 12
    raw = zlib.decompress(compressed)
    # At the top-left all PNG filter predictors are zero.
    return raw[1:5]


with tempfile.TemporaryDirectory(prefix='3beans-scripting-') as directory:
    root = Path(directory)
    directory = root.as_posix()
    for name in ('boot9.bin', 'boot11.bin'):
        (root / name).write_bytes(struct.pack('<I', 0xEAFFFFFE) * (0x10000 // 4))
    for index, name in ((1, 'override.img'), (2, 'second.img'), (3, 'saved.img'), (2, 'preflight.img')):
        (root / name).write_bytes(struct.pack('<H', index * 0x1111) + bytes(4094))
    (root / 'nand.bin').write_bytes(bytes(0x20000))
    ini = 'sdPath=saved.img\nboot9Path=missing9\nboot11Path=missing11\nnandPath=missingnand\ngpuRenderer=1\nthreadedGpu=1\nfpsLimiter=1\n'
    (root / '3beans.ini').write_text(ini)
    common = ['--headless', '--config-dir', directory, '--boot9', (root / 'boot9.bin').as_posix(),
              '--boot11', (root / 'boot11.bin').as_posix(), '--nand', (root / 'nand.bin').as_posix(), '--sd', (root / 'override.img').as_posix()]
    env = dict(os.environ, DISPLAY='', WAYLAND_DISPLAY='')

    def run(code, success=True, extra=(), timeout=30):
        script = root / 'test.lua'
        script.write_text('TESTDIR = ' + json.dumps(directory) + '\n' + code)
        result = subprocess.run([binary, *common, '--script', str(script), *extra], cwd=root,
                                env=env, capture_output=True, text=True, timeout=timeout)
        assert (result.returncode == 0) == success, (result.returncode, result.stdout, result.stderr)
        assert (root / '3beans.ini').read_text() == ini, 'Saved configuration changed'
        return result.stdout + result.stderr

    assert 'integration passed' in run((source / 'integration.lua').read_text(), timeout=60)
    assert png_first_pixel(root / 'red.png') == bytes((255, 0, 0, 255))
    assert png_first_pixel(root / 'green.png') == bytes((0, 255, 0, 255))
    assert 'deliberate failure' in run("error('deliberate failure')", False)
    assert 'stack traceback' in run("local function f() error('trace') end; f()", False)
    assert 'frame failed' in run("emu:start(); callbacks:add('frame', function() error('frame failed') end); emu:runFrame()", False)
    run('this is not Lua', False)
    run('while true do end', False, ['--timeout', '0.1'], timeout=5)
    run('while true do pcall(function() while true do end end) end', False, ['--timeout', '0.1'], timeout=5)
    run('pcall(function() while true do end end)', False, ['--timeout', '0.1'], timeout=5)
    run("emu:start(); callbacks:add('frame', function() while true do pcall(function() while true do end end) end end); emu:runFrame()", False, ['--timeout', '0.1'], timeout=5)
    run('emu:start(); while true do emu:runFrame() end', False, ['--timeout', '0.3'], timeout=5)
    run("emu:setPathOverride('boot9', TESTDIR .. '/override.img')", False)
    # A pure script never implicitly boots, and multiple script files share globals.
    first, second = root / 'first.lua', root / 'second.lua'
    first.write_text('shared = 123')
    second.write_text('assert(shared == 123); assert(next(emu:getPaths().mounted) == nil)')
    result = subprocess.run([binary, '--headless', '--config-dir', str(root / 'no-settings'),
                             '--script', str(first), '--script', str(second)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not (root / 'no-settings').exists(), 'Headless startup wrote default settings'
    # Global package mutation cannot panic the host when the next file loads.
    for mutation in ('package = nil', 'package.path = nil', 'package.path = 42',
                     'package = setmetatable({}, {__index = function() error("metatable") end})'):
        first.write_text(mutation)
        second.write_text('assert(1 + 1 == 2)')
        result = subprocess.run([binary, '--headless', '--script', str(first), '--script', str(second)],
                                cwd=root, capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, (mutation, result.stdout, result.stderr)
    if len(sys.argv) > 2:
        subprocess.run([str(Path(sys.argv[2]).resolve()), directory], cwd=root, check=True, timeout=30)
        assert png_first_pixel(root / 'desktop-latest.png') == bytes((255, 0, 0, 255))
    for args in (['--headless'], ['--unknown'], ['--script'], ['--timeout', 'nan']):
        assert subprocess.run([binary, *args], capture_output=True).returncode == 2
print('CLI scripting tests passed')
