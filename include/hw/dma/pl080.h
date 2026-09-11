/*
 * ARM PrimeCell PL080/PL081 DMA controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Copyright (c) 2018 Linaro Limited
 * Written by Paul Brook, Peter Maydell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

/*
 * This is a model of the Arm PrimeCell PL080/PL081 DMA controller:
 * The PL080 TRM is:
 * https://developer.arm.com/documentation/ddi0196/latest
 * and the PL081 TRM is:
 * https://developer.arm.com/documentation/ddi0218/latest
 *
 * QEMU interface:
 * + sysbus IRQ 0: DMACINTR combined interrupt line
 * + sysbus IRQ 1: DMACINTERR error interrupt request
 * + sysbus IRQ 2: DMACINTTC count interrupt request
 * + sysbus MMIO region 0: MemoryRegion for the device's registers
 * + QOM property "downstream": MemoryRegion defining where DMA
 *   bus master transactions are made
 * + GPIO inputs "dreq-single", "dreq-burst", "dreq-last-single" and
 *   "dreq-last-burst": 16 peripheral request lines of each type
 * + GPIO outputs "dreq-clear": 16 DMACCLR handshake lines
 */

#ifndef HW_DMA_PL080_H
#define HW_DMA_PL080_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define PL080_MAX_CHANNELS 8
#define PL080_NUM_PERIPHERALS 16
#define PL080_FIFO_BYTES 16

typedef struct {
    uint32_t src;
    uint32_t dest;
    uint32_t lli;
    uint32_t ctrl;
    uint32_t conf;
} pl080_channel;

typedef struct {
    uint8_t fifo[PL080_FIFO_BYTES];
    uint8_t len;
    uint16_t src_left;
    uint16_t dst_left;
    uint8_t src_kind;
    uint8_t dst_kind;
    bool started;
} PL080Transfer;

#define TYPE_PL080 "pl080"
#define TYPE_PL081 "pl081"
OBJECT_DECLARE_SIMPLE_TYPE(PL080State, PL080)

struct PL080State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t tc_int;
    uint8_t tc_mask;
    uint8_t err_int;
    uint8_t err_mask;
    uint32_t conf;
    uint32_t sync;
    uint32_t req_single;
    uint32_t req_burst;
    uint32_t req_last_single;
    uint32_t req_last_burst;
    uint16_t hw_req[4];
    uint16_t req_ack;
    uint16_t req_busy;
    qemu_irq request_clear[PL080_NUM_PERIPHERALS];
    pl080_channel chan[PL080_MAX_CHANNELS];
    PL080Transfer transfer[PL080_MAX_CHANNELS];
    QEMUBH *bh;
    int nchannels;
    /* Flag to avoid recursive DMA invocations.  */
    int running;
    qemu_irq irq;
    qemu_irq interr;
    qemu_irq inttc;

    MemoryRegion *downstream;
    AddressSpace downstream_as;
};

#endif
