/*
 * Samsung S5L8702 I2C master.
 * Adapted from davidmonterocrespo24/qemu-ipod-classic's I2C controller.
 * IICCON[4] advances a byte; IICSTAT2 acknowledges interrupt status.
 * A START transfers only the slave address, never the first data byte.
 * retailOS 0x083608dc consumes receive data and ACK status on completion.
 * Transfers are atomic at byte completion; wire timing remains approximate.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/i2c/s5l8702-i2c.h"

static void s5l8702_i2c_update_irq(S5L8702I2cState *s)
{
    uint32_t active = s->iicstat2 & s->iiccon & S5L8702_IICCON_INT_MASK;

    qemu_set_irq(s->irq, active != 0);
}

static void s5l8702_i2c_sync(S5L8702I2cState *s)
{
    if (timer_pending(s->byte_timer)) {
        s->remaining_ns = MAX(0, timer_expire_time_ns(s->byte_timer) -
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void s5l8702_i2c_schedule(S5L8702I2cState *s)
{
    if (s->pending && clock_get(s->pclk)) {
        timer_mod(s->byte_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  s->remaining_ns);
    }
}

static void s5l8702_i2c_clock_changed(void *opaque, ClockEvent event)
{
    S5L8702I2cState *s = opaque;

    if (event == ClockPreUpdate) {
        s5l8702_i2c_sync(s);
        timer_del(s->byte_timer);
    } else {
        s5l8702_i2c_schedule(s);
    }
}

static void s5l8702_i2c_start_byte(S5L8702I2cState *s, bool address)
{
    timer_del(s->byte_timer);
    s->byte_done = false;
    s->pending = true;
    s->in_address = address;
    s->receive = (s->iicstat & S5L8702_IICSTAT_MODE_MASK) ==
                 S5L8702_IICSTAT_START_RX;
    s->nack = !(s->iiccon & S5L8702_IICCON_ACK_GEN);
    s->tx_byte = s->iicds;
    /* The original diagnostic establishes the gates, not this duration. */
    s->remaining_ns = S5L8702_I2C_BYTE_US * SCALE_US;
    s5l8702_i2c_schedule(s);
}

static void s5l8702_i2c_end_transfer(S5L8702I2cState *s)
{
    i2c_end_transfer(s->bus);
    timer_del(s->byte_timer);
    s->iicstat &= ~S5L8702_IICSTAT_BUSY;
    s->byte_done = false;
    s->pending = false;
    s->remaining_ns = 0;
    s->in_address = false;
}

static void s5l8702_i2c_set_lrb(S5L8702I2cState *s, int nack)
{
    if (nack) {
        s->iicstat |= S5L8702_IICSTAT_LRB;
    } else {
        s->iicstat &= ~S5L8702_IICSTAT_LRB;
    }
}

static void s5l8702_i2c_byte_done(void *opaque)
{
    S5L8702I2cState *s = opaque;

    s->pending = false;
    s->remaining_ns = 0;
    if (s->in_address) {
        int nack = s->receive ? i2c_start_recv(s->bus, s->tx_byte >> 1) :
                               i2c_start_send(s->bus, s->tx_byte >> 1);

        s5l8702_i2c_set_lrb(s, nack);
    } else if (s->receive) {
        s->iicds = i2c_recv(s->bus);
        if (s->nack) {
            i2c_nack(s->bus);
        }
    } else {
        s5l8702_i2c_set_lrb(s, i2c_send(s->bus, s->tx_byte));
    }
    s->byte_done = true;
    s->iicstat2 |= S5L8702_IICSTAT2_BYTE;
    if (s->in_address) {
        s->iicstat2 |= S5L8702_IICSTAT2_START;
        s->in_address = false;
    }
    s5l8702_i2c_update_irq(s);
}

static void s5l8702_i2c_go(S5L8702I2cState *s)
{
    uint32_t mode = s->iicstat & S5L8702_IICSTAT_MODE_MASK;

    s->byte_done = false;

    switch (mode) {
    case S5L8702_IICSTAT_START_TX:
    case S5L8702_IICSTAT_START_RX:
        s5l8702_i2c_start_byte(s, false);
        break;
    default:
        break;
    }
}

static uint64_t s5l8702_i2c_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    S5L8702I2cState *s = opaque;

    switch (offset) {
    case S5L8702_IICCON:
        return s->iiccon | (s->byte_done ? S5L8702_IICCON_GO : 0);
    case S5L8702_IICSTAT:
        return s->iicstat;
    case S5L8702_IICADD:
        return s->iicadd;
    case S5L8702_IICDS:
        return s->iicds;
    case S5L8702_IICUNK10:

        return 0;
    case S5L8702_IICUNK14:
        return s->iicunk14;
    case S5L8702_IICUNK18:
        return s->iicunk18;
    case S5L8702_IICSTAT2:
        return s->iicstat2;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read of 0x%" HWADDR_PRIx "\n", __func__,
                      offset);
        return 0;
    }
}

static void s5l8702_i2c_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    S5L8702I2cState *s = opaque;

    switch (offset) {
    case S5L8702_IICCON:
        s->iiccon = value & ~S5L8702_IICCON_GO;
        if ((value & S5L8702_IICCON_GO) && s->byte_done) {
            s5l8702_i2c_go(s);
        }
        s5l8702_i2c_update_irq(s);
        break;

    case S5L8702_IICSTAT: {
        uint32_t mode = value & S5L8702_IICSTAT_MODE_MASK;

        s->iicstat = (s->iicstat & ~(S5L8702_IICSTAT_MODE_MASK |
                                     S5L8702_IICSTAT_SOE)) |
                     (value & (S5L8702_IICSTAT_MODE_MASK |
                               S5L8702_IICSTAT_SOE));

        if (!(value & S5L8702_IICSTAT_SOE)) {
            s5l8702_i2c_end_transfer(s);
            break;
        }

        switch (mode) {
        case S5L8702_IICSTAT_START_TX:
        case S5L8702_IICSTAT_START_RX:
            s->iicstat |= S5L8702_IICSTAT_BUSY;
            s5l8702_i2c_start_byte(s, true);
            break;
        case S5L8702_IICSTAT_STOP_TX:
        case S5L8702_IICSTAT_STOP_RX:
            /*
             * retailOS 0x083608dc writes 0xd0/0x90 to end TX/RX. These
             * commands have already cleared BUSY in the register above;
             * release the bus even though the new register value is idle.
             */
            s5l8702_i2c_end_transfer(s);
            s->iicstat2 |= S5L8702_IICSTAT2_STOP;
            break;
        default:
            break;
        }
        s5l8702_i2c_update_irq(s);
        break;
    }

    case S5L8702_IICADD:
        s->iicadd = value;
        break;
    case S5L8702_IICDS:
        s->iicds = value;
        break;
    case S5L8702_IICUNK10:

        break;
    case S5L8702_IICUNK14:
        s->iicunk14 = value;
        break;
    case S5L8702_IICUNK18:
        s->iicunk18 = value;
        break;
    case S5L8702_IICSTAT2:
        s->iicstat2 &= ~(uint32_t)value;
        s5l8702_i2c_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%" PRIx64 " to 0x%"
                      HWADDR_PRIx "\n", __func__, value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8702_i2c_ops = {
    .read = s5l8702_i2c_read,
    .write = s5l8702_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_i2c_reset_enter(Object *obj, ResetType type)
{
    S5L8702I2cState *s = S5L8702_I2C(obj);

    timer_del(s->byte_timer);
    s->iiccon = 0;
    s->iicstat = 0;
    s->iicadd = 0;
    s->iicds = 0;
    s->iicunk14 = 0;
    s->iicunk18 = 0;
    s->iicstat2 = 0;
    s->byte_done = false;
    s->in_address = false;
    s->receive = false;
    s->nack = false;
    s->tx_byte = 0;
    s->pending = false;
    s->remaining_ns = 0;
}

static void s5l8702_i2c_reset_hold(Object *obj)
{
    S5L8702I2cState *s = S5L8702_I2C(obj);

    i2c_end_transfer(s->bus);
    s5l8702_i2c_update_irq(s);
}

static void s5l8702_i2c_init(Object *obj)
{
    S5L8702I2cState *s = S5L8702_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_i2c_ops, s,
                          TYPE_S5L8702_I2C, S5L8702_I2C_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    s->byte_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 s5l8702_i2c_byte_done, s);
    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk",
                                 s5l8702_i2c_clock_changed, s,
                                 ClockPreUpdate | ClockUpdate);
}

static int s5l8702_i2c_pre_save(void *opaque)
{
    s5l8702_i2c_sync(opaque);
    return 0;
}

static int s5l8702_i2c_post_load(void *opaque, int version_id)
{
    S5L8702I2cState *s = opaque;

    if ((s->byte_done && s->pending) ||
        (s->in_address && !s->pending) ||
        (!s->pending && s->remaining_ns) ||
        s->remaining_ns > S5L8702_I2C_BYTE_US * SCALE_US ||
        timer_pending(s->byte_timer) != (s->pending && clock_get(s->pclk))) {
        return -EINVAL;
    }
    s5l8702_i2c_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_i2c = {
    .name = TYPE_S5L8702_I2C,
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = s5l8702_i2c_pre_save,
    .post_load = s5l8702_i2c_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(iiccon, S5L8702I2cState),
        VMSTATE_UINT32(iicstat, S5L8702I2cState),
        VMSTATE_UINT32(iicadd, S5L8702I2cState),
        VMSTATE_UINT32(iicds, S5L8702I2cState),
        VMSTATE_UINT32(iicunk14, S5L8702I2cState),
        VMSTATE_UINT32(iicunk18, S5L8702I2cState),
        VMSTATE_UINT32(iicstat2, S5L8702I2cState),
        VMSTATE_BOOL(byte_done, S5L8702I2cState),
        VMSTATE_BOOL(in_address, S5L8702I2cState),
        VMSTATE_BOOL(receive, S5L8702I2cState),
        VMSTATE_BOOL(nack, S5L8702I2cState),
        VMSTATE_UINT8(tx_byte, S5L8702I2cState),
        VMSTATE_BOOL(pending, S5L8702I2cState),
        VMSTATE_UINT32(remaining_ns, S5L8702I2cState),
        VMSTATE_CLOCK(pclk, S5L8702I2cState),
        VMSTATE_TIMER_PTR(byte_timer, S5L8702I2cState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_i2c_finalize(Object *obj)
{
    S5L8702I2cState *s = S5L8702_I2C(obj);

    timer_free(s->byte_timer);
}

static void s5l8702_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 I2C master";
    rc->phases.enter = s5l8702_i2c_reset_enter;
    rc->phases.hold = s5l8702_i2c_reset_hold;
    dc->vmsd = &vmstate_s5l8702_i2c;
}

static const TypeInfo s5l8702_i2c_info = {
    .name          = TYPE_S5L8702_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702I2cState),
    .instance_init = s5l8702_i2c_init,
    .class_init    = s5l8702_i2c_class_init,
    .instance_finalize = s5l8702_i2c_finalize,
};

static void s5l8702_i2c_register_types(void)
{
    type_register_static(&s5l8702_i2c_info);
}

type_init(s5l8702_i2c_register_types)
