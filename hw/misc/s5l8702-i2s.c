/*
 * Samsung S5L8702 I2S transmitter/receiver, 0x3CA00000.
 *
 * retailOS 2.0.4 0x2200881c clears TXCOM, RXCOM and CLKCON, then waits
 * for CLKCON bit 1 before gating the interface at PWRCON1 bit 7.
 * The observed 16-bit stereo mode now serializes one DMA request at a time
 * using the codec LRCK input. The four-halfword transaction buffer is an
 * abstraction of one verified DMA burst, not a measured hardware FIFO depth.
 * Other formats, capture, wire edges and physical request thresholds remain
 * unimplemented.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic (register interface only).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "qemu/int128.h"
#include "qemu/host-utils.h"
#include "migration/vmstate.h"
#include "hw/misc/s5l8702-i2s.h"
#include "trace.h"

/*
 * The inherited +0x3c value has not been established from the original
 * firmware. Keep this approximation separate from the CLKCON stop status.
 */
#define S5L8702_I2S_STATUS_STUB  0x00000001
#define I2S_CLKCON_ENABLE       BIT(0)
#define I2S_CLKCON_STOPPED      BIT(1)

#define I2S_TXCOM_DMA            BIT(1)
#define I2S_TXCOM_ENABLE         BIT(2)
#define I2S_TXCON_STEREO16       0x0b100019
#define I2S_FRAME_ONE           (1ULL << 32)
#define I2S_WORK_FRAMES         256

static bool i2s_running(S5L8702I2SState *s)
{
    return (s->clkcon & I2S_CLKCON_ENABLE) &&
           (s->txcom & I2S_TXCOM_ENABLE) &&
           s->txcon == I2S_TXCON_STEREO16 &&
           clock_get(s->pclk) && clock_get(s->lrck);
}

static void i2s_request(S5L8702I2SState *s)
{
    bool enabled = i2s_running(s) && (s->txcom & I2S_TXCOM_DMA);

    if (s->tx_request && (!enabled || s->tx_ack)) {
        s->tx_request = false;
        qemu_set_irq(s->tx_dreq, 0);
    } else if (!s->tx_request && !s->tx_ack && enabled && !s->tx_count) {
        /* retailOS 0x22004a20 programs four destination halfwords/request. */
        s->tx_request = true;
        qemu_set_irq(s->tx_dreq, 1);
    }
}

static void i2s_clear(void *opaque, int line, int level)
{
    S5L8702I2SState *s = opaque;

    s->tx_ack = level;
    /* PL080 holds CLR until this request falls; its reply can be recursive. */
    i2s_request(s);
}

static void i2s_sync(S5L8702I2SState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned work = I2S_WORK_FRAMES;

    if (s->updating) {
        return;
    }
    s->updating = true;
    if (i2s_running(s) && now > s->last_ns) {
        Int128 elapsed = int128_make128(0, now - s->last_ns);
        Int128 progress = int128_divu(elapsed,
                                      int128_make64(clock_get(s->lrck)));
        uint64_t room = UINT64_MAX - s->phase;

        /* ns * 2^64 / (period in 2^-32 ns) gives frames in 2^-32 units. */
        s->phase += int128_ult(progress, int128_make64(room)) ?
                    int128_get64(progress) : room;
    }
    s->last_ns = now;
    while (i2s_running(s) && s->phase >= I2S_FRAME_ONE && work--) {
        if (s->tx_count < 2) {
            /* No buffered data: retain clock phase, not fictitious samples. */
            s->phase %= I2S_FRAME_ONE;
            break;
        }
        s->phase -= I2S_FRAME_ONE;
        s->last_frame = s->tx_buffer[0] | ((uint32_t)s->tx_buffer[1] << 16);
        s->tx_count -= 2;
        memmove(s->tx_buffer, s->tx_buffer + 2,
                s->tx_count * sizeof(s->tx_buffer[0]));
        s->tx_samples += 2;
        trace_s5l8702_i2s_frame(s->last_frame, s->tx_samples);
        if (s->codec) {
            s5l8702_cs42l55_dac_frame(s->codec, s->last_frame,
                                      s->last_frame >> 16);
        }
        i2s_request(s);
    }
    s->updating = false;
}

static void i2s_schedule(S5L8702I2SState *s)
{
    uint64_t delay;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->updating) {
        return;
    }
    if (!i2s_running(s) || s->tx_count < 2) {
        timer_del(&s->tx_timer);
        return;
    }
    if (s->phase >= I2S_FRAME_ONE) {
        /* Yield after bounded host work if virtual time has advanced far. */
        delay = 1;
    } else {
        uint64_t lo, hi, period;

        mulu64(&lo, &hi, I2S_FRAME_ONE - s->phase, clock_get(s->lrck));
        period = (hi << 32) | (lo >> 32);

        delay = (period >> 32) + ((uint32_t)period != 0);
    }
    timer_mod(&s->tx_timer, now + MIN(MAX(delay, 1), INT64_MAX - now));
}

static void i2s_tick(void *opaque)
{
    S5L8702I2SState *s = opaque;

    i2s_sync(s);
    i2s_request(s);
    i2s_schedule(s);
}

static void i2s_clock_changed(void *opaque, ClockEvent event)
{
    S5L8702I2SState *s = opaque;

    if (event == ClockPreUpdate) {
        i2s_sync(s);
        timer_del(&s->tx_timer);
    } else {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        i2s_request(s);
        i2s_schedule(s);
    }
}

static uint64_t s5l8702_i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702I2SState *s = S5L8702_I2S(opaque);

    switch (offset) {
    case S5L8702_I2S_CLKCON:
        /* Clock disable discards pending data; drain timing is unknown. */
        return (s->clkcon & ~I2S_CLKCON_STOPPED) |
               (s->clkcon & I2S_CLKCON_ENABLE ? 0 : I2S_CLKCON_STOPPED);
    case S5L8702_I2S_TXCON:   return s->txcon;
    case S5L8702_I2S_TXCOM:   return s->txcom;
    case S5L8702_I2S_RXCON:   return s->rxcon;
    case S5L8702_I2S_RXCOM:   return s->rxcom;
    case S5L8702_I2S_RXDB:    return s->rxdb;
    case S5L8702_I2S_CLKDIV:  return s->clkdiv;
    case S5L8702_I2S_STATUS:  return S5L8702_I2S_STATUS_STUB;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read of +0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void s5l8702_i2s_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702I2SState *s = S5L8702_I2S(opaque);

    i2s_sync(s);
    switch (offset) {
    case S5L8702_I2S_CLKCON:
        if (!(value & I2S_CLKCON_ENABLE)) {
            s->tx_count = 0;
            s->phase = 0;
        }
        s->clkcon = value & ~I2S_CLKCON_STOPPED;
        break;
    case S5L8702_I2S_TXCON:
        s->txcon = value;
        break;
    case S5L8702_I2S_TXCOM:
        s->txcom = value;
        break;
    case S5L8702_I2S_RXCON:
        s->rxcon = value;
        break;
    case S5L8702_I2S_RXCOM:
        s->rxcom = value;
        break;
    case S5L8702_I2S_CLKDIV:
        s->clkdiv = value;
        break;
    case S5L8702_I2S_TXDB0:
        if (s->tx_count == ARRAY_SIZE(s->tx_buffer)) {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-i2s: TX buffer overflow\n");
            break;
        }
        /* One halfword sample per access in the observed stereo16 mode. */
        s->tx_buffer[s->tx_count++] = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%" PRIx64 " to +0x%"
                      HWADDR_PRIx "\n", __func__, value, offset);
        break;
    }
    i2s_request(s);
    i2s_schedule(s);
}

static const MemoryRegionOps s5l8702_i2s_ops = {
    .read = s5l8702_i2s_read,
    .write = s5l8702_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* The retailOS uses 32-bit words; DMAC0 pushes PCM in 16-bit halfwords. */
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 4,
};

static void s5l8702_i2s_reset_enter(Object *obj, ResetType type)
{
    S5L8702I2SState *s = S5L8702_I2S(obj);

    s->clkcon = 0;
    s->txcon = 0;
    s->txcom = 0;
    s->rxcon = 0;
    s->rxcom = 0;
    s->rxdb = 0;
    s->clkdiv = 0;
    timer_del(&s->tx_timer);
    s->tx_samples = 0;
    s->tx_count = 0;
    s->tx_request = false;
    s->tx_ack = false;
    s->updating = false;
    s->phase = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->last_frame = 0;
    memset(s->tx_buffer, 0, sizeof(s->tx_buffer));
}

static void s5l8702_i2s_init(Object *obj)
{
    S5L8702I2SState *s = S5L8702_I2S(obj);

    s->lrck = qdev_init_clock_in(DEVICE(obj), "lrck", i2s_clock_changed,
                                s, ClockPreUpdate | ClockUpdate);
    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk", i2s_clock_changed,
                                s, ClockPreUpdate | ClockUpdate);
    timer_init_ns(&s->tx_timer, QEMU_CLOCK_VIRTUAL, i2s_tick, s);
    qdev_init_gpio_out_named(DEVICE(obj), &s->tx_dreq, "tx-dreq", 1);
    qdev_init_gpio_in_named(DEVICE(obj), i2s_clear, "tx-clear", 1);
    object_property_add_uint64_ptr(obj, "transmitted-samples", &s->tx_samples,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "last-frame", &s->last_frame,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "queued-samples", &s->tx_count,
                                   OBJ_PROP_FLAG_READ);
    memory_region_init_io(&s->iomem, obj, &s5l8702_i2s_ops, s,
                          TYPE_S5L8702_I2S, S5L8702_I2S_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void s5l8702_i2s_reset_hold(Object *obj)
{
    S5L8702I2SState *s = S5L8702_I2S(obj);

    qemu_set_irq(s->tx_dreq, 0);
    qemu_set_irq(s->irq, 0);
}

static void s5l8702_i2s_finalize(Object *obj)
{
    timer_del(&S5L8702_I2S(obj)->tx_timer);
}

static int s5l8702_i2s_post_load(void *opaque, int version_id)
{
    S5L8702I2SState *s = opaque;

    if (s->tx_count > ARRAY_SIZE(s->tx_buffer) || s->last_ns < 0 ||
        s->last_ns > qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        return -EINVAL;
    }
    s->updating = false;
    qemu_set_irq(s->tx_dreq, s->tx_request);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_i2s = {
    .name = TYPE_S5L8702_I2S,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = s5l8702_i2s_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(clkcon, S5L8702I2SState),
        VMSTATE_UINT32(txcon, S5L8702I2SState),
        VMSTATE_UINT32(txcom, S5L8702I2SState),
        VMSTATE_UINT32(rxcon, S5L8702I2SState),
        VMSTATE_UINT32(rxcom, S5L8702I2SState),
        VMSTATE_UINT32(rxdb, S5L8702I2SState),
        VMSTATE_UINT32(clkdiv, S5L8702I2SState),
        VMSTATE_UINT64(tx_samples, S5L8702I2SState),
        VMSTATE_UINT16_ARRAY(tx_buffer, S5L8702I2SState, 4),
        VMSTATE_UINT32(tx_count, S5L8702I2SState),
        VMSTATE_BOOL(tx_request, S5L8702I2SState),
        VMSTATE_BOOL(tx_ack, S5L8702I2SState),
        VMSTATE_INT64(last_ns, S5L8702I2SState),
        VMSTATE_UINT64(phase, S5L8702I2SState),
        VMSTATE_UINT32(last_frame, S5L8702I2SState),
        VMSTATE_CLOCK(lrck, S5L8702I2SState),
        VMSTATE_CLOCK(pclk, S5L8702I2SState),
        VMSTATE_TIMER(tx_timer, S5L8702I2SState),
        VMSTATE_END_OF_LIST()
    },
};

static Property s5l8702_i2s_properties[] = {
    DEFINE_PROP_LINK("codec", S5L8702I2SState, codec,
                     TYPE_S5L8702_CS42L55, S5L8702CS42L55State *),
    DEFINE_PROP_END_OF_LIST(),
};

static void s5l8702_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 I2S transmitter";
    rc->phases.enter = s5l8702_i2s_reset_enter;
    rc->phases.hold = s5l8702_i2s_reset_hold;
    dc->vmsd = &vmstate_s5l8702_i2s;
    dc->user_creatable = false;
    device_class_set_props(dc, s5l8702_i2s_properties);
}

static const TypeInfo s5l8702_i2s_types[] = {
    {
        .name          = TYPE_S5L8702_I2S,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_init = s5l8702_i2s_init,
        .instance_finalize = s5l8702_i2s_finalize,
        .instance_size = sizeof(S5L8702I2SState),
        .class_init    = s5l8702_i2s_class_init,
    },
};
DEFINE_TYPES(s5l8702_i2s_types)
