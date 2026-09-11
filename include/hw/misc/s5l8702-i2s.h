/*
 * Samsung S5L8702 I2S transmitter/receiver, 0x3CA00000 (I2S_BASE).
 *
 * Original retailOS 2.0.4 routines used to establish the register interface:
 *
 * 0x22004ca4 selects the I2S base; port 0 is 0x3ca00000.
 * 0x22008744 sets TXCON, RXCON and CLKDIV.
 * 0x220087e0 sets bits 1/2 in TXCOM or RXCOM.
 * 0x2200881c enables CLKCON with 1, or writes 0 and polls bit 1 before
 *            gating the interface. This is a stop acknowledgement.
 * 0x080a7838 selects CLKDIV and codec register 5 for a requested sample rate.
 *
 * The 44100 Hz path writes CLKDIV=0x110 and codec register 5=0x0b. It does
 * not by itself establish the serial sample rate. Initialization 0x0809f1c8
 * selects codec master mode, MCLK / 2, and a ratio giving about 44117.6 Hz
 * from 12 MHz. The transmitter consumes the codec LRCK input.
 *
 * The observed 16-bit stereo configuration uses four-halfword DMA requests.
 * One request is buffered and serialized at LRCK frame boundaries. This
 * transaction buffer does not establish the physical FIFO depth/threshold.
 * Other formats, capture, serial bit edges and underrun status are absent.
 * +0x3c retains an unverified constant; stopping discards pending samples.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic (register interface only).
 */

#ifndef HW_MISC_S5L8702_I2S_H
#define HW_MISC_S5L8702_I2S_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/misc/s5l8702-cs42l55.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_S5L8702_I2S "s5l8702-i2s"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702I2SState, S5L8702_I2S)

#define S5L8702_I2S_BASE       0x3ca00000
/* Unknown offsets are logged; only the listed registers have semantics. */
#define S5L8702_I2S_MEM_SIZE   0x1000

/* Register offsets from I2S_BASE. */
#define S5L8702_I2S_CLKCON     0x00
#define S5L8702_I2S_TXCON      0x04
#define S5L8702_I2S_TXCOM      0x08
#define S5L8702_I2S_TXDB0      0x10
#define S5L8702_I2S_RXCON      0x30
#define S5L8702_I2S_RXCOM      0x34
#define S5L8702_I2S_RXDB       0x38
#define S5L8702_I2S_STATUS     0x3c
#define S5L8702_I2S_CLKDIV     0x40

struct S5L8702I2SState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq tx_dreq;
    Clock *lrck;
    Clock *pclk;
    QEMUTimer tx_timer;
    S5L8702CS42L55State *codec;

    /* One observed four-halfword DMA transaction, not a FIFO-depth claim. */
    uint16_t tx_buffer[4];
    uint32_t tx_count;
    bool tx_request;
    bool tx_ack;
    bool updating;
    int64_t last_ns;
    uint64_t phase;       /* elapsed frames, in units of 2^-32 */
    uint32_t last_frame;  /* diagnostic: low halfword left, high right */

    /* Configuration storage; CLKCON bit 1 is derived on read. */
    uint32_t clkcon;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t rxcon;
    uint32_t rxcom;
    uint32_t rxdb;
    uint32_t clkdiv;

    /* Samples emitted by the serializer; still used by the provisional SM1. */
    uint64_t tx_samples;
};

#endif /* HW_MISC_S5L8702_I2S_H */
