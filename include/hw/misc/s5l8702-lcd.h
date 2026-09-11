/*
 * Samsung S5L8702 LCD interface and panel.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_S5L8702_LCD_H
#define HW_MISC_S5L8702_LCD_H

#include "hw/sysbus.h"
#include "hw/misc/s5l8702-disp.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define TYPE_S5L8702_LCD "s5l8702-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702LcdState, S5L8702_LCD)

#define S5L8702_LCD_BASE 0x38300000
#define S5L8702_LCD_SIZE 0x100000
#define S5L8702_LCD_PIXELS (S5L8702_DISP_WIDTH * S5L8702_DISP_HEIGHT)

struct S5L8702LcdState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *con;
    S5L8702DispState *disp;
    QEMUTimer *te_timer;
    qemu_irq te;
    uint32_t refresh_rate;
    uint32_t regs[0x90 / 4];
    uint64_t panel_regs[256];
    uint16_t framebuffer[S5L8702_LCD_PIXELS];
    uint16_t composed[S5L8702_LCD_PIXELS];
    uint16_t sc, ec, sp, ep;
    uint16_t x, y;
    uint8_t command, parameter;
    uint8_t reply[4], reply_pos, reply_len;
    bool sleeping, display_on;
    bool te_enabled;
    bool invalidate;
};

#endif /* HW_MISC_S5L8702_LCD_H */
