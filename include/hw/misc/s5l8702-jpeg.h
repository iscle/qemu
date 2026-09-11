/*
 * Samsung S5L8702 JPEG block processor.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_S5L8702_JPEG_H
#define HW_MISC_S5L8702_JPEG_H

#include "hw/sysbus.h"

#define TYPE_S5L8702_JPEG "s5l8702-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702JpegState, S5L8702_JPEG)

#define S5L8702_JPEG_BASE 0x39600000
#define S5L8702_JPEG_SIZE 0x00100000

struct S5L8702JpegState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t qtable[2][64];
    uint32_t irq_mask;
    uint32_t config;
    uint32_t dma_status;
    uint32_t dma_mask;
    uint32_t dma_control;
    uint32_t dma_config;
    uint32_t input_base;
    uint32_t input_end;
    uint32_t input_cursor;
    uint32_t output_base[3];
    uint32_t output_address[3];
    uint32_t layout;
    uint32_t command;
    uint8_t output_queue[3];
    uint8_t output_count;
    uint8_t block_in_pair;
    bool pending;
};

#endif /* HW_MISC_S5L8702_JPEG_H */
