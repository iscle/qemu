/*
 * Samsung S5L8702 UART, partial register-level model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original retailOS 2.0.4: 0x08363120 initializes the four port bases;
 * 0x08362f50 acknowledges UTRSTAT and reads the autobaud counter at +0x2c,
 * then programs the TX/RX fine-tuning registers at +0x34/+0x38.
 * 0x080b0b14 polls UFSTAT & 0x2f0 before writing at most 16 bytes.
 * The original aupd diagnostic driver 0x0801a274 decodes the 16-byte RX
 * FIFO; 0x0801b270/0x0801b384 enable IRQs through UCON bits 12/13;
 * 0x0801b42c programs the FIFO triggers and self-clearing resets.
 *
 * This is not an Exynos UART: there is no UINTP/UINTSP/UINTM register bank.
 * Remaining limitations: TX drains at host backend speed, with no separate
 * shift register or baud timing. RX is byte-oriented; timeout, autobaud,
 * modem signals, per-byte errors, DMA and infrared are not implemented.
 * Baud/fine-tuning registers retain their values without timing effects.
 */
#include "qemu/osdep.h"
#include "hw/char/s5l8702-uart.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"

#define ULCON     0x00
#define UCON      0x04
#define UFCON     0x08
#define UMCON     0x0c
#define UTRSTAT   0x10
#define UERSTAT   0x14
#define UFSTAT    0x18
#define UMSTAT    0x1c
#define UTXH      0x20
#define URXH      0x24
#define UBRDIV    0x28
#define UABRCNT   0x2c
#define UABRSTAT  0x30
#define UBRCONTX  0x34
#define UBRCONRX  0x38

#define RX_MODE   3
#define TX_MODE   (3 << 2)
#define LOOPBACK  BIT(5)
#define FIFO_EN   BIT(0)
#define RX_RESET  BIT(1)
#define TX_RESET  BIT(2)
#define RX_IRQ    BIT(4)
#define TX_IRQ    BIT(5)
#define ERR_IRQ   BIT(6)
#define IRQ_MASK  0x1f8

static void s5l8702_uart_update_irq(S5L8702UartState *s)
{
    qemu_set_irq(s->irq, !!(s->pending & (s->ucon >> 8) & IRQ_MASK));
}

static unsigned fifo_capacity(S5L8702UartState *s)
{
    return s->ufcon & FIFO_EN ? S5L8702_UART_FIFO_SIZE : 1;
}

static unsigned rx_trigger(S5L8702UartState *s)
{
    return s->ufcon & FIFO_EN ? 4 * (extract32(s->ufcon, 4, 2) + 1) : 1;
}

static unsigned tx_trigger(S5L8702UartState *s)
{
    return s->ufcon & FIFO_EN ? 4 * extract32(s->ufcon, 6, 2) : 0;
}

static void s5l8702_uart_cancel_watch(S5L8702UartState *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
}

static void s5l8702_uart_rx_byte(S5L8702UartState *s, uint8_t value)
{
    if ((s->ucon & RX_MODE) != 1 || !clock_get(s->pclk)) {
        return;
    }
    if (s->rx_count >= fifo_capacity(s)) {
        s->errors |= BIT(0); /* Overrun; retain the unread bytes. */
        s->pending |= ERR_IRQ;
    } else {
        s->rx[(s->rx_head + s->rx_count) % S5L8702_UART_FIFO_SIZE] = value;
        s->rx_count++;
        if (s->rx_count >= rx_trigger(s)) {
            s->pending |= RX_IRQ;
        }
    }
    s5l8702_uart_update_irq(s);
}

static void s5l8702_uart_transmit(void *opaque);

static gboolean s5l8702_uart_writable(void *unused, GIOCondition cond,
                                     void *opaque)
{
    S5L8702UartState *s = opaque;

    s->watch = 0;
    s5l8702_uart_transmit(s);
    return G_SOURCE_REMOVE;
}

static void s5l8702_uart_transmit(void *opaque)
{
    S5L8702UartState *s = opaque;

    if (s->watch || (s->ucon & TX_MODE) != 4 || !clock_get(s->pclk)) {
        return;
    }
    while (s->tx_count) {
        uint8_t value = s->tx[s->tx_head];

        if (s->ucon & LOOPBACK) {
            s5l8702_uart_rx_byte(s, value);
        } else if (qemu_chr_fe_backend_open(&s->chr) &&
                   qemu_chr_fe_write(&s->chr, &value, 1) <= 0) {
            s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                            s5l8702_uart_writable, s);
            if (s->watch) {
                s5l8702_uart_update_irq(s);
                return;
            }
            /* An absent or disconnected backend consumes output. */
        }
        s->tx_head = (s->tx_head + 1) % S5L8702_UART_FIFO_SIZE;
        s->tx_count--;
        if (s->tx_count == tx_trigger(s)) {
            s->pending |= TX_IRQ;
        }
    }
    s5l8702_uart_update_irq(s);
}

static int s5l8702_uart_can_receive(void *opaque)
{
    S5L8702UartState *s = opaque;

    if ((s->ucon & RX_MODE) != 1 || (s->ucon & LOOPBACK) ||
        !clock_get(s->pclk)) {
        return 0;
    }
    return MAX(0, (int)fifo_capacity(s) - s->rx_count);
}

static void s5l8702_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    S5L8702UartState *s = opaque;

    for (int i = 0; i < size; i++) {
        s5l8702_uart_rx_byte(s, buf[i]);
    }
}

static void s5l8702_uart_event(void *opaque, QEMUChrEvent event)
{
    S5L8702UartState *s = opaque;

    if (event == CHR_EVENT_BREAK && s5l8702_uart_can_receive(s)) {
        s5l8702_uart_rx_byte(s, 0);
        s->errors |= BIT(3);
        s->pending |= ERR_IRQ;
        s5l8702_uart_update_irq(s);
    } else if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        s5l8702_uart_cancel_watch(s);
        qemu_bh_schedule(s->tx_bh);
    }
}

static uint64_t s5l8702_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702UartState *s = opaque;
    uint32_t value;

    switch (offset) {
    case ULCON:
        return s->ulcon;
    case UCON:
        return s->ucon;
    case UFCON:
        return s->ufcon;
    case UMCON:
        return s->umcon;
    case UBRDIV:
        return s->ubrdiv;
    case UBRCONTX:
        return s->ubrcontx;
    case UBRCONRX:
        return s->ubrconrx;
    case UTRSTAT:
        return s->pending | (s->rx_count ? 1 : 0) | (s->tx_count ? 0 : 6);
    case UERSTAT:
        value = s->errors;
        s->errors = 0;
        return value;
    case UFSTAT:
        return (s->rx_count & 0xf) | ((s->tx_count & 0xf) << 4) |
               (s->rx_count == S5L8702_UART_FIFO_SIZE ? BIT(8) : 0) |
               (s->tx_count == S5L8702_UART_FIFO_SIZE ? BIT(9) : 0);
    case URXH:
        if (!s->rx_count) {
            return 0;
        }
        value = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % S5L8702_UART_FIFO_SIZE;
        s->rx_count--;
        qemu_chr_fe_accept_input(&s->chr);
        return value;
    case UMSTAT:
    case UABRCNT:
    case UABRSTAT:
        /* No modem or serial edge source is connected. */
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-uart: read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void s5l8702_uart_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    S5L8702UartState *s = opaque;

    switch (offset) {
    case ULCON:
        s->ulcon = value;
        break;
    case UMCON:
        s->umcon = value;
        break;
    case UBRDIV:
        s->ubrdiv = value;
        break;
    case UBRCONTX:
        s->ubrcontx = value;
        break;
    case UBRCONRX:
        s->ubrconrx = value;
        break;
    case UCON:
        s->ucon = value;
        if (!(s->ucon & RX_MODE)) {
            s->rx_count = s->rx_head = 0;
        }
        if (!(s->ucon & TX_MODE)) {
            s->tx_count = s->tx_head = 0;
        }
        s5l8702_uart_cancel_watch(s);
        s5l8702_uart_transmit(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UFCON:
        s->ufcon = value & ~(RX_RESET | TX_RESET);
        if (value & RX_RESET) {
            s->rx_count = s->rx_head = 0;
        }
        if (value & TX_RESET) {
            s->tx_count = s->tx_head = 0;
            s5l8702_uart_cancel_watch(s);
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UTRSTAT:
        s->pending &= ~(value & IRQ_MASK);
        break;
    case UTXH:
        if ((s->ucon & TX_MODE) == 4 && s->tx_count < fifo_capacity(s)) {
            s->tx[(s->tx_head + s->tx_count) % S5L8702_UART_FIFO_SIZE] = value;
            s->tx_count++;
            s5l8702_uart_transmit(s);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-uart: write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", offset, value);
        break;
    }
    s5l8702_uart_update_irq(s);
}

static const MemoryRegionOps s5l8702_uart_ops = {
    .read = s5l8702_uart_read,
    .write = s5l8702_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Resume backend work after clock propagation or VMState restoration. */
static void s5l8702_uart_resume(void *opaque)
{
    S5L8702UartState *s = opaque;

    s5l8702_uart_transmit(s);
    qemu_chr_fe_accept_input(&s->chr);
}

static void s5l8702_uart_clock_changed(void *opaque, ClockEvent event)
{
    S5L8702UartState *s = opaque;

    s5l8702_uart_cancel_watch(s);
    qemu_bh_schedule(s->tx_bh);
    qemu_chr_fe_accept_input(&s->chr);
}

static void s5l8702_uart_reset_enter(Object *obj, ResetType type)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    qemu_bh_cancel(s->tx_bh);
    s5l8702_uart_cancel_watch(s);
    s->ulcon = s->ucon = s->ufcon = s->umcon = 0;
    s->ubrdiv = s->ubrcontx = s->ubrconrx = 0;
    s->pending = s->errors = 0;
    s->rx_count = s->rx_head = s->tx_count = s->tx_head = 0;
    memset(s->rx, 0, sizeof(s->rx));
    memset(s->tx, 0, sizeof(s->tx));
}

static void s5l8702_uart_reset_hold(Object *obj)
{
    s5l8702_uart_update_irq(S5L8702_UART(obj));
}

static int s5l8702_uart_pre_load(void *opaque)
{
    S5L8702UartState *s = opaque;

    qemu_bh_cancel(s->tx_bh);
    s5l8702_uart_cancel_watch(s);
    return 0;
}

static int s5l8702_uart_post_load(void *opaque, int version_id)
{
    S5L8702UartState *s = opaque;

    if (s->rx_head >= S5L8702_UART_FIFO_SIZE ||
        s->tx_head >= S5L8702_UART_FIFO_SIZE ||
        s->rx_count > S5L8702_UART_FIFO_SIZE ||
        s->tx_count > S5L8702_UART_FIFO_SIZE ||
        (s->pending & ~IRQ_MASK) || (s->errors & ~0xf)) {
        return -EINVAL;
    }
    s5l8702_uart_update_irq(s);
    qemu_bh_schedule(s->tx_bh);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_uart = {
    .name = TYPE_S5L8702_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = s5l8702_uart_pre_load,
    .post_load = s5l8702_uart_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ulcon, S5L8702UartState),
        VMSTATE_UINT32(ucon, S5L8702UartState),
        VMSTATE_UINT32(ufcon, S5L8702UartState),
        VMSTATE_UINT32(umcon, S5L8702UartState),
        VMSTATE_UINT32(ubrdiv, S5L8702UartState),
        VMSTATE_UINT32(ubrcontx, S5L8702UartState),
        VMSTATE_UINT32(ubrconrx, S5L8702UartState),
        VMSTATE_UINT32(pending, S5L8702UartState),
        VMSTATE_UINT32(errors, S5L8702UartState),
        VMSTATE_UINT8_ARRAY(rx, S5L8702UartState, S5L8702_UART_FIFO_SIZE),
        VMSTATE_UINT8_ARRAY(tx, S5L8702UartState, S5L8702_UART_FIFO_SIZE),
        VMSTATE_UINT8(rx_head, S5L8702UartState),
        VMSTATE_UINT8(rx_count, S5L8702UartState),
        VMSTATE_UINT8(tx_head, S5L8702UartState),
        VMSTATE_UINT8(tx_count, S5L8702UartState),
        VMSTATE_CLOCK(pclk, S5L8702UartState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_uart_realize(DeviceState *dev, Error **errp)
{
    S5L8702UartState *s = S5L8702_UART(dev);

    if (!clock_has_source(s->pclk)) {
        error_setg(errp, "S5L8702 UART requires pclk");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, s5l8702_uart_can_receive,
                             s5l8702_uart_receive, s5l8702_uart_event,
                             NULL, s, NULL, true);
}

static void s5l8702_uart_init(Object *obj)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    s->tx_bh = qemu_bh_new(s5l8702_uart_resume, s);
    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk",
                                s5l8702_uart_clock_changed, s, ClockUpdate);
    memory_region_init_io(&s->iomem, obj, &s5l8702_uart_ops, s,
                          TYPE_S5L8702_UART, 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8702_uart_finalize(Object *obj)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    s5l8702_uart_cancel_watch(s);
    qemu_bh_delete(s->tx_bh);
    qemu_chr_fe_deinit(&s->chr, false);
}

static Property s5l8702_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", S5L8702UartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void s5l8702_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_uart_realize;
    dc->vmsd = &vmstate_s5l8702_uart;
    dc->desc = "Samsung S5L8702 UART (partial)";
    rc->phases.enter = s5l8702_uart_reset_enter;
    rc->phases.hold = s5l8702_uart_reset_hold;
    device_class_set_props(dc, s5l8702_uart_properties);
}

static const TypeInfo s5l8702_uart_types[] = {
    {
        .name = TYPE_S5L8702_UART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702UartState),
        .instance_init = s5l8702_uart_init,
        .instance_finalize = s5l8702_uart_finalize,
        .class_init = s5l8702_uart_class_init,
    },
};
DEFINE_TYPES(s5l8702_uart_types);
