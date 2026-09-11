/*
 * Samsung S5L8702 display compositor.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_S5L8702_DISP_H
#define HW_MISC_S5L8702_DISP_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8702_DISP "s5l8702-disp"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702DispState, S5L8702_DISP)

#define S5L8702_DISP_BASE        0x38900000
#define S5L8702_DISP_MEM_SIZE    0x1000
#define S5L8702_DISP_NREG        (S5L8702_DISP_MEM_SIZE / 4)
#define S5L8702_DISP_WIDTH       320
#define S5L8702_DISP_HEIGHT      240

struct S5L8702DispState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t reg[S5L8702_DISP_NREG];

    /* Work buffers are bounded by the panel, never by guest allocations. */
    uint32_t pixels[S5L8702_DISP_WIDTH * S5L8702_DISP_HEIGHT];
    uint8_t rowbuf[S5L8702_DISP_WIDTH * 4];
};

/* Render a complete transfer. False means no enabled input or invalid DMA. */
bool s5l8702_disp_compose(S5L8702DispState *s, uint16_t *fb,
                          unsigned width, unsigned height);

#endif /* HW_MISC_S5L8702_DISP_H */
