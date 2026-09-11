/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_S5L8702_TVO_H
#define HW_MISC_S5L8702_TVO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8702_TVO "s5l8702-tvo"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702TvoState, S5L8702_TVO)

#define S5L8702_TVO_BACKGROUND 0x39200048
#define S5L8702_TVO_OUTPUT_CONTROL 0x39300280
#define S5L8702_TVO_RUN_BASE 0x39100000
#define S5L8702_TVO_ENCODER_CONFIG 0x3930003c

struct S5L8702TvoState {
    SysBusDevice parent_obj;

    MemoryRegion background_mmio;
    MemoryRegion output_mmio;
    MemoryRegion run_mmio[3];
    MemoryRegion config_mmio;
    uint32_t background;
    uint32_t output_control[2];
    uint32_t run_control[3];
    uint32_t encoder_config;
};

#endif
