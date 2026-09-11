/*
 * Samsung S5L8702 clickwheel serial controller.
 *
 * Register and packet layout: Rockbox button-clickwheel.c, corroborated by
 * retailOS 2.0.4. Command bit 0 starts a transfer; it is not an enable bit.
 * The receive interrupt follows FIFO occupancy and the interrupt mask.
 */
#include "qemu/osdep.h"
#include "hw/misc/s5l8702-clickwheel.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define WHEEL_CON  0x00
#define WHEEL_GO   0x04
#define WHEEL_DIV  0x08
#define WHEEL_STAT 0x0c
#define WHEEL_MASK 0x10
#define WHEEL_PEND 0x14
#define WHEEL_RX   0x18
#define WHEEL_TX   0x1c
#define WHEEL_RX_READY 1
#define WHEEL_EVENT 0x8000001a
#define WHEEL_TOUCH 0x40000000
#define WHEEL_BUTTONS 0x8000023a
#define WHEEL_POSITION 0x8000063a
#define WHEEL_ACK 0x8000062a
/* Report cadence is an emulation choice; the wire protocol carries state. */
#define WHEEL_REPORT_NS (40 * SCALE_MS)

static uint32_t wheel_pending(S5L8702ClickwheelState *s)
{
    return s->pending | (s->count ? WHEEL_RX_READY : 0);
}

static void wheel_update_irq(S5L8702ClickwheelState *s)
{
    qemu_set_irq(s->irq, (wheel_pending(s) & s->mask & 7) != 0);
}

static void wheel_push(S5L8702ClickwheelState *s, uint32_t packet)
{
    if (s->count == S5L8702_WHEEL_FIFO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-clickwheel: receive overflow\n");
        return;
    }
    s->fifo[(s->head + s->count) % S5L8702_WHEEL_FIFO_SIZE] = packet;
    s->count++;
    wheel_update_irq(s);
}

static void wheel_schedule(S5L8702ClickwheelState *s)
{
    timer_mod(&s->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WHEEL_REPORT_NS);
}

static void wheel_tick(void *opaque)
{
    S5L8702ClickwheelState *s = opaque;
    uint32_t packet = WHEEL_EVENT | (s->buttons << 8);

    if (s->count >= S5L8702_WHEEL_FIFO_SIZE / 2) {
        wheel_schedule(s);
        return;
    }
    if (s->steps && !s->touched) {
        /* The first touch establishes a position; later reports move it. */
        s->touched = true;
    } else if (s->steps) {
        int step = CLAMP(s->steps, -8, 8);
        s->position = (s->position + 96 + step) % 96;
        s->steps -= step;
    } else if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->untouch_at) {
        s->touched = false;
    }
    if (s->touched) {
        packet |= WHEEL_TOUCH | (s->position << 16);
    }
    wheel_push(s, packet);
    if (s->reports) {
        s->reports--;
    }
    if (s->buttons || s->steps || s->touched || s->reports) {
        wheel_schedule(s);
    }
}

static void wheel_transfer(S5L8702ClickwheelState *s)
{
    uint32_t cmd = s->tx;

    /* Apple's serial setup shifts the command one bit on transmission. */
    if (cmd != WHEEL_BUTTONS && cmd != WHEEL_POSITION && cmd != WHEEL_ACK) {
        cmd = (cmd << 1) | 0x80000000;
    }
    switch (cmd) {
    case WHEEL_BUTTONS:
        /* 0x08362a9c: query packets order Select, Play, Prev, Menu, Next. */
        wheel_push(s, WHEEL_BUTTONS |
                   (((s->buttons & 1) | ((s->buttons & 2) << 3) |
                     (s->buttons & 4) | ((s->buttons & 8) >> 2) |
                     ((s->buttons & 16) >> 1)) << 16));
        break;
    case WHEEL_POSITION:
        wheel_push(s, WHEEL_POSITION | (s->position << 16) |
                   (s->touched ? WHEEL_TOUCH : 0));
        break;
    case WHEEL_ACK:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-clickwheel: command 0x%08x\n", cmd);
    }
}

static uint64_t wheel_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702ClickwheelState *s = opaque;
    uint32_t value = 0;

    switch (offset) {
    case WHEEL_CON:
        value = s->control;
        break;
    case WHEEL_GO:
        value = s->command;
        break;
    case WHEEL_DIV:
        value = s->divider;
        break;
    case WHEEL_STAT:
        value = s->count ? WHEEL_RX_READY : 0;
        break;
    case WHEEL_MASK:
        value = s->mask;
        break;
    case WHEEL_PEND:
        value = wheel_pending(s);
        break;
    case WHEEL_TX:
        value = s->tx;
        break;
    case WHEEL_RX:
        if (s->count) {
            value = s->fifo[s->head];
            s->head = (s->head + 1) % S5L8702_WHEEL_FIFO_SIZE;
            s->count--;
            wheel_update_irq(s);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-clickwheel: read at 0x%" HWADDR_PRIx
                      "\n", offset);
    }
    trace_s5l8702_clickwheel_access(false, offset, value);
    return value;
}

static void wheel_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    S5L8702ClickwheelState *s = opaque;

    trace_s5l8702_clickwheel_access(true, offset, value);
    switch (offset) {
    case WHEEL_CON:
        s->control = value;
        break;
    case WHEEL_DIV:
        s->divider = value;
        break;
    case WHEEL_TX:
        s->tx = value;
        break;
    case WHEEL_GO:
        s->command = value & ~1u;
        if (value & 1) {
            wheel_transfer(s);
        }
        break;
    case WHEEL_MASK:
        s->mask = value;
        wheel_update_irq(s);
        break;
    case WHEEL_PEND:
        s->pending &= ~value;
        wheel_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-clickwheel: write at 0x%" HWADDR_PRIx
                      "\n", offset);
    }
}

static void wheel_input(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    S5L8702ClickwheelState *s = S5L8702_CLICKWHEEL(dev);
    InputKeyEvent *key = evt->u.key.data;
    QKeyCode code = qemu_input_key_value_to_qcode(key->key);
    uint32_t bit;

    switch (code) {
    case Q_KEY_CODE_RET:
        bit = 1;
        break;
    case Q_KEY_CODE_RIGHT:
    case Q_KEY_CODE_D:
        bit = 2;
        break;
    case Q_KEY_CODE_LEFT:
    case Q_KEY_CODE_A:
        bit = 4;
        break;
    case Q_KEY_CODE_S:
    case Q_KEY_CODE_SPC:
        bit = 8;
        break;
    case Q_KEY_CODE_ESC:
    case Q_KEY_CODE_W:
        bit = 16;
        break;
    case Q_KEY_CODE_UP:
    case Q_KEY_CODE_Q:
    case Q_KEY_CODE_DOWN:
    case Q_KEY_CODE_E:
        if (key->down) {
            int delta = (code == Q_KEY_CODE_UP || code == Q_KEY_CODE_Q) ?
                        -8 : 8;
            s->steps = CLAMP(s->steps + delta, -96, 96);
            s->untouch_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            500 * SCALE_MS;
            wheel_schedule(s);
        }
        return;
    default:
        return;
    }
    if (key->down) {
        s->buttons |= bit;
    } else {
        s->buttons &= ~bit;
    }
    s->reports = 10;
    wheel_schedule(s);
}

static QemuInputHandler wheel_input_handler = {
    .name = "iPod clickwheel",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = wheel_input,
};

static const MemoryRegionOps wheel_ops = {
    .read = wheel_read,
    .write = wheel_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void wheel_reset_enter(Object *obj, ResetType type)
{
    S5L8702ClickwheelState *s = S5L8702_CLICKWHEEL(obj);

    timer_del(&s->timer);
    s->control = s->command = s->divider = s->mask = s->pending = s->tx = 0;
    s->head = s->count = s->buttons = s->position = s->reports = 0;
    s->steps = 0;
    s->untouch_at = 0;
    s->touched = false;
    memset(s->fifo, 0, sizeof(s->fifo));
}

static void wheel_reset_hold(Object *obj)
{
    wheel_update_irq(S5L8702_CLICKWHEEL(obj));
}

static int wheel_post_load(void *opaque, int version_id)
{
    S5L8702ClickwheelState *s = opaque;

    if (s->head >= S5L8702_WHEEL_FIFO_SIZE ||
        s->count > S5L8702_WHEEL_FIFO_SIZE || s->position >= 96) {
        return -EINVAL;
    }
    wheel_update_irq(s);
    return 0;
}

static const VMStateDescription wheel_vmstate = {
    .name = TYPE_S5L8702_CLICKWHEEL,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = wheel_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(control, S5L8702ClickwheelState),
        VMSTATE_UINT32(command, S5L8702ClickwheelState),
        VMSTATE_UINT32(divider, S5L8702ClickwheelState),
        VMSTATE_UINT32(mask, S5L8702ClickwheelState),
        VMSTATE_UINT32(pending, S5L8702ClickwheelState),
        VMSTATE_UINT32(tx, S5L8702ClickwheelState),
        VMSTATE_UINT32_ARRAY(fifo, S5L8702ClickwheelState,
                             S5L8702_WHEEL_FIFO_SIZE),
        VMSTATE_UINT32(head, S5L8702ClickwheelState),
        VMSTATE_UINT32(count, S5L8702ClickwheelState),
        VMSTATE_UINT32(buttons, S5L8702ClickwheelState),
        VMSTATE_UINT32(position, S5L8702ClickwheelState),
        VMSTATE_UINT32(reports, S5L8702ClickwheelState),
        VMSTATE_INT32(steps, S5L8702ClickwheelState),
        VMSTATE_BOOL(touched, S5L8702ClickwheelState),
        VMSTATE_INT64(untouch_at, S5L8702ClickwheelState),
        VMSTATE_TIMER(timer, S5L8702ClickwheelState),
        VMSTATE_END_OF_LIST()
    },
};

static void wheel_realize(DeviceState *dev, Error **errp)
{
    S5L8702ClickwheelState *s = S5L8702_CLICKWHEEL(dev);

    s->input = qemu_input_handler_register(dev, &wheel_input_handler);
}

static void wheel_unrealize(DeviceState *dev)
{
    S5L8702ClickwheelState *s = S5L8702_CLICKWHEEL(dev);

    qemu_input_handler_unregister(s->input);
    timer_del(&s->timer);
}

static void wheel_init(Object *obj)
{
    S5L8702ClickwheelState *s = S5L8702_CLICKWHEEL(obj);

    memory_region_init_io(&s->iomem, obj, &wheel_ops, s,
                          TYPE_S5L8702_CLICKWHEEL, S5L8702_CLICKWHEEL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, wheel_tick, s);
}

static void wheel_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 clickwheel serial controller";
    dc->realize = wheel_realize;
    dc->unrealize = wheel_unrealize;
    dc->vmsd = &wheel_vmstate;
    rc->phases.enter = wheel_reset_enter;
    rc->phases.hold = wheel_reset_hold;
}

static const TypeInfo wheel_type = {
    .name = TYPE_S5L8702_CLICKWHEEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702ClickwheelState),
    .instance_init = wheel_init,
    .class_init = wheel_class_init,
};

static void wheel_register_types(void)
{
    type_register_static(&wheel_type);
}
type_init(wheel_register_types)
