/*
 * Samsung S5L8702 SPI controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Supplied boot ROM: 0x20004b4c checks the 16-entry TX FIFO; 0x20004af4
 * polls RX count bits 13:9. 0x20008f90 programs the receive limit, including
 * the command/address bytes, and 0x20004e5c enables automatic reception.
 * Original updater 0x0800fb84 sets SETUP bit 3 before CLKDIV, then bit 4
 * before CTRL enable. Naming follows the older S5L8700 SPI definition;
 * the original code corroborates this sequence, not slave-mode behavior.
 * Transfers currently complete synchronously. Serial clock edges, IRQs,
 * DMA requests, and non-byte/slave modes remain unmodeled. The RX capacity
 * is provisionally the same as TX; the driver establishes its count field,
 * but does not independently establish its maximum occupancy.
 */
#include "qemu/osdep.h"
#include "hw/ssi/s5l8702-spi.h"
#include "hw/qdev-clock.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define SPICTRL     0x00
#define SPISETUP    0x04
#define SPISTATUS   0x08
#define SPIPIN      0x0c
#define SPITXDATA   0x10
#define SPIRXDATA   0x20
#define SPICLKDIV   0x30
#define SPIRXLIMIT  0x34

#define SPICTRL_ENABLE  BIT(0)
#define SPICTRL_STOPPED BIT(1)
#define SPICTRL_CLRTX   BIT(2)
#define SPICTRL_CLRRX   BIT(3)
#define SPISETUP_AUTO   BIT(0)
#define SPISETUP_MASTER BIT(3)
#define SPISETUP_SCKEN  BIT(4)

static void s5l8702_spi_run(S5L8702SpiState *s)
{
    if (!(s->spictrl & SPICTRL_ENABLE) || !clock_get(s->pclk) ||
        !(s->spisetup & SPISETUP_MASTER) ||
        !(s->spisetup & SPISETUP_SCKEN)) {
        return;
    }

    /* Each iteration consumes a queued TX byte or available RX FIFO space. */
    for (;;) {
        uint8_t tx, rx;

        if (s->rx_remaining && fifo8_is_full(&s->rx_fifo)) {
            break;
        }
        if (!fifo8_is_empty(&s->tx_fifo)) {
            tx = fifo8_pop(&s->tx_fifo);
        } else if ((s->spisetup & SPISETUP_AUTO) && s->rx_remaining) {
            tx = 0xff;
        } else {
            break;
        }
        rx = ssi_transfer(s->spi, tx);
        if (s->rx_remaining) {
            fifo8_push(&s->rx_fifo, rx);
            s->rx_remaining--;
        }
    }
}

static void s5l8702_spi_clock_changed(void *opaque, ClockEvent event)
{
    s5l8702_spi_run(opaque);
}

static uint64_t s5l8702_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702SpiState *s = opaque;
    uint32_t value;

    switch (offset) {
    case SPICTRL:
        return s->spictrl | (s->spictrl & SPICTRL_ENABLE ? 0 : SPICTRL_STOPPED);
    case SPISETUP:
        return s->spisetup;
    case SPISTATUS:
        return s->spistatus | (fifo8_num_used(&s->tx_fifo) << 4) |
               (fifo8_num_used(&s->rx_fifo) << 9);
    case SPIPIN:
        return s->spipin;
    case SPITXDATA:
        return s->spitxdata;
    case SPIRXDATA:
        if (fifo8_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-spi: empty RX FIFO\n");
            return 0;
        }
        value = fifo8_pop(&s->rx_fifo);
        /* Freeing space lets the controller receive another queued byte. */
        s5l8702_spi_run(s);
        return value;
    case SPICLKDIV:
        return s->spiclkdiv;
    case SPIRXLIMIT:
        return s->spirxlimit;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-spi: read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void s5l8702_spi_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    S5L8702SpiState *s = opaque;

    if (offset != SPITXDATA) {
        trace_s5l8702_spi_control(s, offset, value);
    }
    switch (offset) {
    case SPICTRL:
        s->spictrl = value & ~(SPICTRL_STOPPED | SPICTRL_CLRTX | SPICTRL_CLRRX);
        if (value & SPICTRL_CLRTX) {
            fifo8_reset(&s->tx_fifo);
        }
        if (value & SPICTRL_CLRRX) {
            fifo8_reset(&s->rx_fifo);
        }
        break;
    case SPISETUP:
        s->spisetup = value;
        break;
    case SPISTATUS:
        /* The low event bits are W1C; event causes remain undecoded. */
        s->spistatus &= ~(value & 0xf);
        return;
    case SPIPIN:
        s->spipin = value;
        return;
    case SPITXDATA:
        if (fifo8_is_full(&s->tx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-spi: full TX FIFO\n");
            return;
        }
        s->spitxdata = value;
        fifo8_push(&s->tx_fifo, value);
        break;
    case SPICLKDIV:
        /* The EFI SPI driver masks the divider to eleven bits. */
        s->spiclkdiv = value & 0x7ff;
        return;
    case SPIRXLIMIT:
        s->spirxlimit = value;
        s->rx_remaining = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-spi: write at 0x%" HWADDR_PRIx "\n",
                      offset);
        return;
    }
    s5l8702_spi_run(s);
}

static const MemoryRegionOps s5l8702_spi_ops = {
    .read = s5l8702_spi_read,
    .write = s5l8702_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_spi_reset_enter(Object *obj, ResetType type)
{
    S5L8702SpiState *s = S5L8702_SPI(obj);

    s->spictrl = 0;
    s->spisetup = 0;
    s->spistatus = 0;
    s->spipin = 0;
    s->spitxdata = 0;
    s->spiclkdiv = 0;
    s->spirxlimit = 0;
    s->rx_remaining = 0;
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
}

static int s5l8702_spi_post_load(void *opaque, int version_id)
{
    S5L8702SpiState *s = opaque;

    /* Do not transfer before the flash and chip-select state are loaded. */
    if (s->rx_remaining > s->spirxlimit || (s->spistatus & ~0xf) ||
        (s->spictrl & (SPICTRL_STOPPED | SPICTRL_CLRTX | SPICTRL_CLRRX)) ||
        (s->spiclkdiv & ~0x7ff) ||
        s->tx_fifo.head >= S5L8702_SPI_FIFO_SIZE ||
        s->rx_fifo.head >= S5L8702_SPI_FIFO_SIZE ||
        fifo8_num_used(&s->tx_fifo) > S5L8702_SPI_FIFO_SIZE ||
        fifo8_num_used(&s->rx_fifo) > S5L8702_SPI_FIFO_SIZE) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_s5l8702_spi = {
    .name = TYPE_S5L8702_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8702_spi_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(spictrl, S5L8702SpiState),
        VMSTATE_UINT32(spisetup, S5L8702SpiState),
        VMSTATE_UINT32(spistatus, S5L8702SpiState),
        VMSTATE_UINT32(spipin, S5L8702SpiState),
        VMSTATE_UINT32(spitxdata, S5L8702SpiState),
        VMSTATE_UINT32(spiclkdiv, S5L8702SpiState),
        VMSTATE_UINT32(spirxlimit, S5L8702SpiState),
        VMSTATE_UINT32(rx_remaining, S5L8702SpiState),
        VMSTATE_FIFO8(tx_fifo, S5L8702SpiState),
        VMSTATE_FIFO8(rx_fifo, S5L8702SpiState),
        VMSTATE_CLOCK(pclk, S5L8702SpiState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_spi_realize(DeviceState *dev, Error **errp)
{
    if (!clock_has_source(S5L8702_SPI(dev)->pclk)) {
        error_setg(errp, "s5l8702-spi: pclk input must be connected");
    }
}

static void s5l8702_spi_init(Object *obj)
{
    S5L8702SpiState *s = S5L8702_SPI(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_spi_ops, s,
                          TYPE_S5L8702_SPI, S5L8702_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->spi = ssi_create_bus(DEVICE(obj), "spi");
    fifo8_create(&s->tx_fifo, S5L8702_SPI_FIFO_SIZE);
    fifo8_create(&s->rx_fifo, S5L8702_SPI_FIFO_SIZE);
    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk",
                                s5l8702_spi_clock_changed, s, ClockUpdate);
}

static void s5l8702_spi_finalize(Object *obj)
{
    S5L8702SpiState *s = S5L8702_SPI(obj);

    fifo8_destroy(&s->tx_fifo);
    fifo8_destroy(&s->rx_fifo);
}

static void s5l8702_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = s5l8702_spi_reset_enter;
    dc->realize = s5l8702_spi_realize;
    dc->vmsd = &vmstate_s5l8702_spi;
}

static const TypeInfo s5l8702_spi_types[] = {
    {
        .name = TYPE_S5L8702_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702SpiState),
        .instance_init = s5l8702_spi_init,
        .instance_finalize = s5l8702_spi_finalize,
        .class_init = s5l8702_spi_class_init,
    },
};
DEFINE_TYPES(s5l8702_spi_types);
