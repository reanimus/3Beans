/*
    Copyright 2023-2026 Hydr8gon

    This file is part of 3Beans.

    3Beans is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    3Beans is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with 3Beans. If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cstring>
#include "../core.h"

std::vector<uint32_t> Pdc::latestFrame() {
    std::lock_guard<std::mutex> lock(mutex);
    return latest ? *latest : std::vector<uint32_t>();
}

uint32_t *Pdc::getFrame() {
    std::shared_ptr<std::vector<uint32_t>> frame;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (buffers.empty()) return nullptr;
        frame = buffers.front();
        buffers.pop();
    }
    uint32_t *copy = new uint32_t[frame->size()];
    std::copy(frame->begin(), frame->end(), copy);
    return copy;
}

void Pdc::drawScreen(int i, uint32_t *buffer) {
    // Draw a screen's framebuffer in the selected format if enabled
    if (~pdcInterruptType[i] & BIT(0)) return;
    int width = (i ? 320 : 400);
    // LCD power/blanking sits after the framebuffer scanout. PWM duty is kept
    // for guest readback; calibrated luminance and adaptive backlight are not
    // modeled, but disabled PWM or zero duty must blank the panel.
    bool lit = lcdReset && !(lcdSignal & BIT(i * 16)) && core.i2c.lcdPowered(i) &&
        (lcdPwm[i] & BIT(16)) && lcdBrightness[i];
    if (!lit || (lcdFill[i] & BIT(24))) {
        uint32_t color = 0xFF000000 | (lit ? lcdFill[i] & 0xFFFFFF : 0);
        for (int y = 0; y < 240; ++y)
            std::fill(buffer + y * 400, buffer + y * 400 + width, color);
        return;
    }
    switch (pdcFramebufFormat[i] & 0x7) {
    case 0: // RGBA8
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t address = screenBases[i] + x * pdcFramebufStep[i] + (239 - y) * 4;
                uint32_t color = core.memory.read<uint32_t>(ARM11A, address);
                uint8_t r = (color >> 24) & 0xFF;
                uint8_t g = (color >> 16) & 0xFF;
                uint8_t b = (color >> 8) & 0xFF;
                buffer[y * 400 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        return;

    case 1: // RGB8
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t address = screenBases[i] + x * pdcFramebufStep[i] + (239 - y) * 3;
                uint8_t r = core.memory.read<uint8_t>(ARM11, address + 2);
                uint8_t g = core.memory.read<uint8_t>(ARM11, address + 1);
                uint8_t b = core.memory.read<uint8_t>(ARM11, address + 0);
                buffer[y * 400 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        return;

    case 2: // RGB565
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t address = screenBases[i] + x * pdcFramebufStep[i] + (239 - y) * 2;
                uint16_t color = core.memory.read<uint16_t>(ARM11, address);
                uint8_t r = ((color >> 11) & 0x1F) * 255 / 31;
                uint8_t g = ((color >> 5) & 0x3F) * 255 / 63;
                uint8_t b = ((color >> 0) & 0x1F) * 255 / 31;
                buffer[y * 400 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        return;

    case 3: // RGB5A1
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t address = screenBases[i] + x * pdcFramebufStep[i] + (239 - y) * 2;
                uint16_t color = core.memory.read<uint16_t>(ARM11, address);
                uint8_t r = ((color >> 11) & 0x1F) * 255 / 31;
                uint8_t g = ((color >> 6) & 0x1F) * 255 / 31;
                uint8_t b = ((color >> 1) & 0x1F) * 255 / 31;
                buffer[y * 400 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        return;

    default: // RGBA4
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t address = screenBases[i] + x * pdcFramebufStep[i] + (239 - y) * 2;
                uint16_t color = core.memory.read<uint16_t>(ARM11, address);
                uint8_t r = ((color >> 12) & 0xF) * 255 / 15;
                uint8_t g = ((color >> 8) & 0xF) * 255 / 15;
                uint8_t b = ((color >> 4) & 0xF) * 255 / 15;
                buffer[y * 400 + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        return;
    }
}

void Pdc::writeLcdSignal(uint32_t mask, uint32_t value) {
    mask &= 0x10001;
    lcdSignal = (lcdSignal & ~mask) | (value & mask);
}

void Pdc::writeLcdReset(uint32_t mask, uint32_t value) {
    mask &= 1;
    lcdReset = (lcdReset & ~mask) | (value & mask);
    if ((mask & 1) && !lcdReset) core.i2c.resetLcd();
}

void Pdc::writeLcdFill(int i, uint32_t mask, uint32_t value) {
    mask &= 0x1FFFFFF;
    lcdFill[i] = (lcdFill[i] & ~mask) | (value & mask);
}

void Pdc::writeLcdBrightness(int i, uint32_t mask, uint32_t value) {
    mask &= 0x3FF;
    lcdBrightness[i] = (lcdBrightness[i] & ~mask) | (value & mask);
}

void Pdc::writeLcdPwm(int i, uint32_t mask, uint32_t value) {
    // Preserve the New 3DS extension bits too; only PWM enable affects scanout.
    mask &= core.n3dsMode ? 0xFFFFF3FF : 0x7F3FF;
    lcdPwm[i] = (lcdPwm[i] & ~mask) | (value & mask);
}

void Pdc::drawFrame() {
    // Trigger PDC interrupts at V-blank if not disabled
    // TODO: handle timings for different modes properly
    if (((pdcInterruptType[0] >> 8) & 0x7) != 0x7)
        core.interrupts.sendInterrupt(ARM11, 0x2A);
    if (((pdcInterruptType[1] >> 8) & 0x7) != 0x7)
        core.interrupts.sendInterrupt(ARM11, 0x2B);

    // Retain one shared snapshot for screenshots; do not copy pixels per vblank.
    // Synchronize only frames that we actually capture (including reused buffers).
    core.gpu.syncRender();
    for (int i = 0; i < 2; i++)
        screenBases[i] = (pdcFramebufSelAck[i] & BIT(0)) ? pdcFramebufLt1[i] : pdcFramebufLt0[i];

    auto buffer = std::make_shared<std::vector<uint32_t>>(400 * 480, 0);
    drawScreen(0, buffer->data());
    drawScreen(1, buffer->data() + 240 * 400 + 40);

    std::lock_guard<std::mutex> lock(mutex);
    latest = buffer;
    if (!core.bootConfig.headless && buffers.size() < 2) buffers.push(buffer);

}

void Pdc::writeFramebufLt0(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_FRAMEBUF_LT0 register
    mask &= 0xFFFFFFF0;
    pdcFramebufLt0[i] = (pdcFramebufLt0[i] & ~mask) | (value & mask);
}

void Pdc::writeFramebufLt1(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_FRAMEBUF_LT1 register
    mask &= 0xFFFFFFF0;
    pdcFramebufLt1[i] = (pdcFramebufLt1[i] & ~mask) | (value & mask);
}

void Pdc::writeFramebufFormat(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_FRAMEBUF_FORMAT register
    // TODO: handle zoom bits and maybe others?
    mask &= 0xFFFF0377;
    pdcFramebufFormat[i] = (pdcFramebufFormat[i] & ~mask) | (value & mask);
}

void Pdc::writeInterruptType(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_INTERRUPT_TYPE register
    mask &= 0x10701;
    pdcInterruptType[i] = (pdcInterruptType[i] & ~mask) | (value & mask);
}

void Pdc::writeFramebufSelAck(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_FRAMEBUF_SEL_ACK register
    // TODO: handle bits other than buffer select?
    mask &= 0x1;
    pdcFramebufSelAck[i] = (pdcFramebufSelAck[i] & ~mask) | (value & mask);
}

void Pdc::writeFramebufStep(int i, uint32_t mask, uint32_t value) {
    // Write to a screen's PDC_FRAMEBUF_STEP register
    mask &= 0xFFFFFFF0;
    pdcFramebufStep[i] = (pdcFramebufStep[i] & ~mask) | (value & mask);
}
