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
 * The original updater 0x0801b8fc computes UBRDIV from clock/(baud*sample)
 * and encodes 16-sample in bits 16+. UCON bit 10 selects the clock.
 * retailOS 0x080d00c4 waits for the shift register to empty separately from
 * its FIFO polling above. TX now advances on this emulated serial clock,
 * independent of the host backend. Active frame configuration is latched.
 *
 * Fine-tuning bit widths and the three-frame RX timeout follow the related
 * S5L8700/Rockbox UC87xx descriptions; physical S5L8702 timing is unverified.
 * RX is still a completed-byte backend abstraction. Autobaud edge detection,
 * modem pins, per-byte errors, DMA and infrared remain unimplemented.
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
#include "qemu/host-utils.h"
#include "qemu/int128.h"
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
#define RX_TIMEOUT BIT(3)
#define RX_TIMEOUT_EN BIT(7)
#define CLOCK_SELECT BIT(10)

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

static Clock *s5l8702_uart_clock(S5L8702UartState *s)
{
    return s->ucon & CLOCK_SELECT ? s->uclk : s->pclk;
}

static bool s5l8702_uart_clocked(S5L8702UartState *s)
{
    return clock_get(s->pclk) && clock_get(s5l8702_uart_clock(s));
}

static uint64_t s5l8702_uart_frame_cycles(S5L8702UartState *s, bool tx)
{
    unsigned bits = 1 + 5 + (s->ulcon & 3) + 1 + !!(s->ulcon & BIT(2)) +
                    !!(s->ulcon & BIT(5));
    unsigned samples = 16 - extract32(s->ubrdiv, 16, 4);
    uint32_t tuning = tx ? s->ubrcontx : s->ubrconrx;
    unsigned cycles = 0;

    for (unsigned i = 0; i < bits; i++) {
        unsigned adjust = extract32(tuning, i * 2, 2);

        /* UC87xx hypothesis: 1 stretches, 3 shortens, 0/2 do not adjust. */
        cycles += samples + (adjust == 1) - (adjust == 3);
    }
    /* Undocumented configurations must not create zero-period event loops. */
    return MAX(cycles, 1) * ((uint64_t)(s->ubrdiv & 0xffff) + 1);
}

static void s5l8702_uart_restart_timeout(S5L8702UartState *s)
{
    if ((s->ucon & RX_MODE) == 1 && (s->ucon & RX_TIMEOUT_EN) &&
        (s->ufcon & FIFO_EN) && s->rx_count && s->rx_count < rx_trigger(s)) {
        s->rx_left = (3 * s5l8702_uart_frame_cycles(s, false)) << 32;
    } else {
        s->rx_left = 0;
    }
}

static void s5l8702_uart_rx_byte(S5L8702UartState *s, uint8_t value)
{
    if ((s->ucon & RX_MODE) != 1 || !s5l8702_uart_clocked(s)) {
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
    s5l8702_uart_restart_timeout(s);
    s5l8702_uart_update_irq(s);
}

static void s5l8702_uart_flush_output(void *opaque);

static gboolean s5l8702_uart_writable(void *unused, GIOCondition cond,
                                     void *opaque)
{
    S5L8702UartState *s = opaque;

    s->watch = 0;
    s5l8702_uart_flush_output(s);
    return G_SOURCE_REMOVE;
}

static void s5l8702_uart_flush_output(void *opaque)
{
    S5L8702UartState *s = opaque;

    if (s->watch) {
        return;
    }
    if (!qemu_chr_fe_backend_open(&s->chr)) {
        s->output_count = s->output_head = 0;
        return;
    }
    while (s->output_count) {
        uint8_t value = s->output[s->output_head];

        if (qemu_chr_fe_write(&s->chr, &value, 1) <= 0) {
            s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                            s5l8702_uart_writable, s);
            if (s->watch) {
                return;
            }
            /* An unavailable backend cannot stall the physical serializer. */
            s->output_dropped++;
        }
        s->output_head = (s->output_head + 1) % S5L8702_UART_OUTPUT_SIZE;
        s->output_count--;
    }
}

static void s5l8702_uart_queue_output(S5L8702UartState *s, uint8_t value)
{
    if (!qemu_chr_fe_backend_open(&s->chr)) {
        return;
    }
    if (s->output_count == S5L8702_UART_OUTPUT_SIZE) {
        s->output_dropped++;
        return;
    }
    s->output[(s->output_head + s->output_count) %
              S5L8702_UART_OUTPUT_SIZE] = value;
    s->output_count++;
    qemu_bh_schedule(s->tx_bh);
}

static void s5l8702_uart_load_shift(S5L8702UartState *s)
{
    if (s->tx_busy || !s->tx_count || (s->ucon & TX_MODE) != 4 ||
        !s5l8702_uart_clocked(s)) {
        return;
    }
    s->tx_shift = s->tx[s->tx_head] & MAKE_64BIT_MASK(0, 5 + (s->ulcon & 3));
    s->tx_head = (s->tx_head + 1) % S5L8702_UART_FIFO_SIZE;
    s->tx_count--;
    s->tx_busy = true;
    s->tx_left = s5l8702_uart_frame_cycles(s, true) << 32;
    if (s->tx_count == tx_trigger(s)) {
        s->pending |= TX_IRQ;
    }
}

static void s5l8702_uart_sync(S5L8702UartState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = 0;

    if (s5l8702_uart_clocked(s) && now > s->last_ns) {
        Int128 scaled = int128_make128(0, now - s->last_ns);
        Int128 cycles = int128_divu(scaled,
            int128_make64(clock_get(s5l8702_uart_clock(s))));

        elapsed = int128_ult(cycles, int128_make64(UINT64_MAX)) ?
                  int128_get64(cycles) : UINT64_MAX;
    }
    s->last_ns = now;
    while (elapsed && (s->tx_busy || s->rx_left)) {
        uint64_t step = elapsed;
        bool timeout = s->rx_left != 0;

        if (s->tx_busy) {
            step = MIN(step, s->tx_left);
        }
        if (timeout) {
            step = MIN(step, s->rx_left);
            s->rx_left -= step;
        }
        if (s->tx_busy) {
            s->tx_left -= step;
        }
        elapsed -= step;
        if (timeout && !s->rx_left) {
            s->pending |= RX_TIMEOUT;
        }
        if (s->tx_busy && !s->tx_left) {
            s->tx_busy = false;
            if (s->ucon & LOOPBACK) {
                s5l8702_uart_rx_byte(s, s->tx_shift);
            } else {
                s5l8702_uart_queue_output(s, s->tx_shift);
            }
            s5l8702_uart_load_shift(s);
        }
    }
    s5l8702_uart_update_irq(s);
}

static void s5l8702_uart_schedule(S5L8702UartState *s)
{
    uint64_t left = s->tx_busy ? s->tx_left : s->rx_left;
    uint64_t lo, hi, delay;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s5l8702_uart_clocked(s) || (!s->tx_busy && !s->rx_left)) {
        timer_del(&s->timer);
        return;
    }
    if (s->rx_left) {
        left = MIN(left, s->rx_left);
    }
    mulu64(&lo, &hi, left, clock_get(s5l8702_uart_clock(s)));
    /* Cycles * 2^32 times ns/cycle * 2^32: round up to the next ns. */
    delay = hi + (lo != 0);
    timer_mod(&s->timer, now + MIN(MAX(delay, 1), INT64_MAX - now));
}

static void s5l8702_uart_tick(void *opaque)
{
    S5L8702UartState *s = opaque;

    s5l8702_uart_sync(s);
    s5l8702_uart_schedule(s);
}

static int s5l8702_uart_can_receive(void *opaque)
{
    S5L8702UartState *s = opaque;

    if ((s->ucon & RX_MODE) != 1 || (s->ucon & LOOPBACK) ||
        !s5l8702_uart_clocked(s)) {
        return 0;
    }
    return MAX(0, (int)fifo_capacity(s) - s->rx_count);
}

static void s5l8702_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    S5L8702UartState *s = opaque;

    s5l8702_uart_sync(s);
    for (int i = 0; i < size; i++) {
        s5l8702_uart_rx_byte(s, buf[i]);
    }
    s5l8702_uart_schedule(s);
}

static void s5l8702_uart_event(void *opaque, QEMUChrEvent event)
{
    S5L8702UartState *s = opaque;

    if (event == CHR_EVENT_BREAK && s5l8702_uart_can_receive(s)) {
        s5l8702_uart_sync(s);
        s5l8702_uart_rx_byte(s, 0);
        s5l8702_uart_schedule(s);
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

    s5l8702_uart_sync(s);
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
        return s->pending | (s->rx_count ? 1 : 0) |
               (s->tx_count ? 0 : 2) | (!s->tx_count && !s->tx_busy ? 4 : 0);
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
        s5l8702_uart_restart_timeout(s);
        s5l8702_uart_schedule(s);
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
    uint32_t old;

    s5l8702_uart_sync(s);
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
        old = s->ucon;
        s->ucon = value;
        if (!(s->ucon & RX_MODE)) {
            s->rx_count = s->rx_head = 0;
        }
        if (!(s->ucon & TX_MODE)) {
            s->tx_count = s->tx_head = 0;
            s->tx_busy = false;
            s->tx_left = 0;
        }
        if ((old ^ s->ucon) & (RX_MODE | RX_TIMEOUT_EN)) {
            s5l8702_uart_restart_timeout(s);
        }
        s5l8702_uart_load_shift(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case UFCON:
        old = s->ufcon;
        s->ufcon = value & ~(RX_RESET | TX_RESET);
        if (value & RX_RESET) {
            s->rx_count = s->rx_head = 0;
        }
        if (value & TX_RESET) {
            s->tx_count = s->tx_head = 0;
        }
        if ((value & RX_RESET) || ((old ^ s->ufcon) & (FIFO_EN | 0x30))) {
            s5l8702_uart_restart_timeout(s);
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
            s5l8702_uart_load_shift(s);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-uart: write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", offset, value);
        break;
    }
    s5l8702_uart_update_irq(s);
    s5l8702_uart_schedule(s);
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

    s5l8702_uart_flush_output(s);
    qemu_chr_fe_accept_input(&s->chr);
}

static void s5l8702_uart_clock_changed(void *opaque, ClockEvent event)
{
    S5L8702UartState *s = opaque;

    if (event == ClockPreUpdate) {
        s5l8702_uart_sync(s);
        timer_del(&s->timer);
    } else {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s5l8702_uart_load_shift(s);
        s5l8702_uart_update_irq(s);
        s5l8702_uart_schedule(s);
        qemu_bh_schedule(s->tx_bh);
    }
}

static void s5l8702_uart_reset_enter(Object *obj, ResetType type)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    qemu_bh_cancel(s->tx_bh);
    timer_del(&s->timer);
    s5l8702_uart_cancel_watch(s);
    s->ulcon = s->ucon = s->ufcon = s->umcon = 0;
    s->ubrdiv = s->ubrcontx = s->ubrconrx = 0;
    s->pending = s->errors = 0;
    s->rx_count = s->rx_head = s->tx_count = s->tx_head = 0;
    memset(s->rx, 0, sizeof(s->rx));
    memset(s->tx, 0, sizeof(s->tx));
    memset(s->output, 0, sizeof(s->output));
    s->output_head = s->output_count = 0;
    s->output_dropped = 0;
    s->tx_shift = 0;
    s->tx_busy = false;
    s->tx_left = s->rx_left = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void s5l8702_uart_reset_hold(Object *obj)
{
    s5l8702_uart_update_irq(S5L8702_UART(obj));
}

static int s5l8702_uart_pre_save(void *opaque)
{
    S5L8702UartState *s = opaque;

    s5l8702_uart_sync(s);
    s5l8702_uart_schedule(s);
    return 0;
}

static int s5l8702_uart_pre_load(void *opaque)
{
    S5L8702UartState *s = opaque;

    qemu_bh_cancel(s->tx_bh);
    timer_del(&s->timer);
    s5l8702_uart_cancel_watch(s);
    /* Version 1 had no serial timing or separate host output queue. */
    s->tx_busy = false;
    s->tx_shift = 0;
    s->tx_left = s->rx_left = 0;
    s->last_ns = 0;
    s->output_count = s->output_head = 0;
    s->output_dropped = 0;
    memset(s->output, 0, sizeof(s->output));
    return 0;
}

static int s5l8702_uart_post_load(void *opaque, int version_id)
{
    S5L8702UartState *s = opaque;
    uint64_t max_frame = (12ULL * 17 * 65536) << 32;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->rx_head >= S5L8702_UART_FIFO_SIZE ||
        s->tx_head >= S5L8702_UART_FIFO_SIZE ||
        s->rx_count > S5L8702_UART_FIFO_SIZE ||
        s->tx_count > S5L8702_UART_FIFO_SIZE ||
        (s->pending & ~IRQ_MASK) || (s->errors & ~0xf) ||
        s->tx_busy != !!s->tx_left || s->tx_left > max_frame ||
        s->rx_left > 3 * max_frame || s->last_ns < 0 || s->last_ns > now ||
        s->output_head >= S5L8702_UART_OUTPUT_SIZE ||
        s->output_count > S5L8702_UART_OUTPUT_SIZE) {
        return -EINVAL;
    }
    s->last_ns = now;
    if (version_id == 1) {
        s5l8702_uart_restart_timeout(s);
    }
    s5l8702_uart_load_shift(s);
    s5l8702_uart_update_irq(s);
    s5l8702_uart_schedule(s);
    qemu_bh_schedule(s->tx_bh);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_uart = {
    .name = TYPE_S5L8702_UART,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = s5l8702_uart_pre_load,
    .pre_save = s5l8702_uart_pre_save,
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
        VMSTATE_CLOCK_V(uclk, S5L8702UartState, 2),
        VMSTATE_UINT8_V(tx_shift, S5L8702UartState, 2),
        VMSTATE_BOOL_V(tx_busy, S5L8702UartState, 2),
        VMSTATE_UINT64_V(tx_left, S5L8702UartState, 2),
        VMSTATE_UINT64_V(rx_left, S5L8702UartState, 2),
        VMSTATE_INT64_V(last_ns, S5L8702UartState, 2),
        VMSTATE_UINT8_ARRAY_V(output, S5L8702UartState,
                             S5L8702_UART_OUTPUT_SIZE, 2),
        VMSTATE_UINT16_V(output_head, S5L8702UartState, 2),
        VMSTATE_UINT16_V(output_count, S5L8702UartState, 2),
        VMSTATE_UINT64_V(output_dropped, S5L8702UartState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_uart_realize(DeviceState *dev, Error **errp)
{
    S5L8702UartState *s = S5L8702_UART(dev);

    if (!clock_has_source(s->pclk) || !clock_has_source(s->uclk)) {
        error_setg(errp, "S5L8702 UART requires pclk and uclk");
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
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, s5l8702_uart_tick, s);
    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk",
                                s5l8702_uart_clock_changed, s,
                                ClockPreUpdate | ClockUpdate);
    s->uclk = qdev_init_clock_in(DEVICE(obj), "uclk",
                                s5l8702_uart_clock_changed, s,
                                ClockPreUpdate | ClockUpdate);
    object_property_add_uint64_ptr(obj, "dropped-output", &s->output_dropped,
                                   OBJ_PROP_FLAG_READ);
    memory_region_init_io(&s->iomem, obj, &s5l8702_uart_ops, s,
                          TYPE_S5L8702_UART, 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8702_uart_finalize(Object *obj)
{
    S5L8702UartState *s = S5L8702_UART(obj);

    s5l8702_uart_cancel_watch(s);
    timer_del(&s->timer);
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
