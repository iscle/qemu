/*
 * ARM PrimeCell PL192 Vector Interrupt Controller
 *
 * Copyright (c) 2009 Samsung Electronics.
 * Contributed by Kirill Batuzov <batuzovk@ispras.ru>
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/intc/pl192.h"
#include "trace.h"

static void pl192_update(PL192State *s);

static uint32_t pl192_priority_sorter(PL192State *s)
{
    uint32_t best = PL192_NO_IRQ;
    uint32_t priority = s->priority;

    if (s->daisy_input && s->daisy_priority < priority &&
        (s->sw_priority_mask & (1u << s->daisy_priority))) {
        best = PL192_DAISY_IRQ;
        priority = s->daisy_priority;
    }
    /* Lower numbered local inputs win ties, including ties with the daisy. */
    for (int i = PL192_INT_SOURCES - 1; i >= 0; i--) {
        uint32_t p = s->vect_priority[i];

        if ((s->irq_status & (1u << i)) && p < s->priority &&
            p <= priority && (s->sw_priority_mask & (1u << p))) {
            best = i;
            priority = p;
        }
    }
    return best;
}

static void pl192_update(PL192State *s)
{
    bool irq, fiq;

    s->irq_status = (s->rawintr | s->softint) & s->intenable & ~s->intselect;
    s->fiq_status = (s->rawintr | s->softint) & s->intenable & s->intselect;
    s->current_highest = pl192_priority_sorter(s);
    irq = s->current_highest != PL192_NO_IRQ;
    fiq = s->fiq_status || s->daisy_fiq;
    if (s->current_highest < PL192_INT_SOURCES) {
        s->address = s->vect_addr[s->current_highest];
    } else if (s->current_highest == PL192_DAISY_IRQ) {
        s->address = s->daisy_vectaddr;
    } else {
        s->address = 0;
    }
    qemu_set_irq(s->irq, irq);
    qemu_set_irq(s->fiq, fiq);
    if (s->daisy) {
        s->daisy->daisy_input = irq;
        s->daisy->daisy_fiq = fiq;
        s->daisy->daisy_vectaddr = s->address;
        pl192_update(s->daisy);
    }
}

static uint32_t pl192_irq_ack(PL192State *s)
{
    uint32_t res = s->address;
    uint32_t next = s->current_highest;

    trace_pl192_irq_ack(res);
    /* A spurious vector read must neither index the vector array nor push. */
    if (next == PL192_NO_IRQ || s->stack_i >= PL192_PRIO_LEVELS) {
        return 0;
    }
    s->current = next;
    s->priority = next == PL192_DAISY_IRQ ? s->daisy_priority :
                                          s->vect_priority[next];
    s->stack_i++;
    s->priority_stack[s->stack_i] = s->priority;
    s->irq_stack[s->stack_i] = next;
    if (next == PL192_DAISY_IRQ && s->daisy_callback) {
        pl192_irq_ack(s->daisy_callback);
    }
    pl192_update(s);
    return res;
}

static void pl192_irq_fin(PL192State *s)
{
    trace_pl192_irq_fin();
    if (!s->stack_i) {
        return;
    }
    if (s->current == PL192_DAISY_IRQ && s->daisy_callback) {
        pl192_irq_fin(s->daisy_callback);
    }
    s->stack_i--;
    s->priority = s->priority_stack[s->stack_i];
    s->current = s->irq_stack[s->stack_i];
    pl192_update(s);
}

static uint64_t pl192_read(void *opaque, hwaddr offset, unsigned size)
{
    PL192State *s = opaque;
    uint64_t ret;

    if (offset & 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192: bad read offset (1) 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }

    if (offset >= 0xfe0 && offset < 0x1000) {
        unsigned char pl192_id[] = { 0x92, 0x11, 0x04, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };
        ret = pl192_id[(offset - 0xfe0) >> 2];
        trace_pl192_read(offset, ret);
        return ret;
    }
    if (offset >= 0x100 && offset < 0x180) {
        ret = s->vect_addr[(offset - 0x100) >> 2];
        trace_pl192_read(offset, ret);
        return ret;
    }
    if (offset >= 0x200 && offset < 0x280) {
        ret = s->vect_priority[(offset - 0x200) >> 2];
        trace_pl192_read(offset, ret);
        return ret;
    }

    switch (offset) {
    case PL192_IRQSTATUS:
        ret = s->irq_status;
        break;
    case PL192_FIQSTATUS:
        ret = s->fiq_status;
        break;
    case PL192_RAWINTR:
        ret = s->rawintr | s->softint;
        break;
    case PL192_INTSELECT:
        ret = s->intselect;
        break;
    case PL192_INTENABLE:
        ret = s->intenable;
        break;
    case PL192_SOFTINT:
        ret = s->softint;
        break;
    case PL192_PROTECTION:
        ret = s->protection;
        break;
    case PL192_SWPRIORITYMASK:
        ret = s->sw_priority_mask;
        break;
    case PL192_PRIORITYDAISY:
        ret = s->daisy_priority;
        break;
    case PL192_INTENCLEAR:
        ret = 0;
        break;
    case PL192_SOFTINTCLEAR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192: attempt to read write-only register (offset = "
                 "0x%" HWADDR_PRIx ")\n", offset);
        return 0;
    case PL192_VECTADDR:
        ret = pl192_irq_ack(s);
        trace_pl192_read(offset, ret);
        return ret;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192: bad read offset (2) 0x%" HWADDR_PRIx "\n",
                      offset);
        ret = 0;
        break;
    }

    trace_pl192_read(offset, ret);
    return ret;
}

static void pl192_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    PL192State *s = opaque;

    trace_pl192_write(offset, value);

    if (offset & 3) {
        qemu_log_mask(LOG_GUEST_ERROR, "pl192: unaligned write\n");
        return;
    }

    if (offset >= 0xfe0 && offset < 0x1000) {
        qemu_log_mask(LOG_GUEST_ERROR, "pl192: read-only register write\n");
        return;
    }
    if (offset >= 0x100 && offset < 0x180) {
        s->vect_addr[(offset - 0x100) >> 2] = value;
        pl192_update(s);
        return;
    }
    if (offset >= 0x200 && offset < 0x280) {
        s->vect_priority[(offset - 0x200) >> 2] = value & 0xf;
        pl192_update(s);
        return;
    }

    switch (offset) {
    case PL192_IRQSTATUS:
    case PL192_FIQSTATUS:
    case PL192_RAWINTR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192: read-only register write\n");
        break;
    case PL192_INTSELECT:
        s->intselect = value;
        break;
    case PL192_INTENABLE:
        s->intenable |= value;
        break;
    case PL192_INTENCLEAR:
        s->intenable &= ~value;
        break;
    case PL192_SOFTINT:
        s->softint |= value;
        break;
    case PL192_SOFTINTCLEAR:
        s->softint &= ~value;
        break;
    case PL192_PROTECTION:
        /* TODO: implement protection */
        s->protection = value & 1;
        break;
    case PL192_SWPRIORITYMASK:
        s->sw_priority_mask = value & 0xffff;
        break;
    case PL192_PRIORITYDAISY:
        s->daisy_priority = value & 0xf;
        break;
    case PL192_VECTADDR:
        pl192_irq_fin(s);
        return;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl192: bad write offset (2) 0x%" HWADDR_PRIx "\n",
                      offset);
        return;
    }

    pl192_update(s);
}

static void pl192_irq_handler(void *opaque, int irq, int level)
{
    PL192State *s = opaque;

    trace_pl192_irq_handler(irq, level);

    if (level) {
        s->rawintr |= 1u << irq;
    } else {
        s->rawintr &= ~(1u << irq);
    }
    pl192_update(opaque);
}

static void pl192_reset_enter(Object *obj, ResetType type)
{
    PL192State *s = PL192(obj);

    s->rawintr = s->intselect = s->intenable = s->softint = 0;
    s->irq_status = s->fiq_status = s->protection = s->address = 0;
    s->daisy_input = s->daisy_fiq = s->daisy_vectaddr = 0;
    memset(s->vect_addr, 0, sizeof(s->vect_addr));
    memset(s->priority_stack, 0, sizeof(s->priority_stack));
    memset(s->irq_stack, 0, sizeof(s->irq_stack));
    for (int i = 0; i < PL192_INT_SOURCES; i++) {
        s->vect_priority[i] = 0xf;
    }
    s->sw_priority_mask = 0xffff;
    s->daisy_priority = 0xf;
    s->current = s->current_highest = PL192_NO_IRQ;
    s->stack_i = 0;
    s->priority_stack[0] = 0x10;
    s->irq_stack[0] = PL192_NO_IRQ;
    s->priority = 0x10;
}

static void pl192_reset_hold(Object *obj)
{
    pl192_update(PL192(obj));
}

static const MemoryRegionOps pl192_ops = {
    .read = pl192_read,
    .write = pl192_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int pl192_post_load(void *opaque, int version_id)
{
    PL192State *s = opaque;

    if (s->stack_i < 0 || s->stack_i > PL192_PRIO_LEVELS ||
        s->priority > PL192_PRIO_LEVELS || s->daisy_priority >= 16 ||
        s->current > PL192_NO_IRQ) {
        return -EINVAL;
    }
    for (int i = 0; i < PL192_INT_SOURCES; i++) {
        if (s->vect_priority[i] >= 16) {
            return -EINVAL;
        }
    }
    for (int i = 0; i <= PL192_PRIO_LEVELS; i++) {
        if (s->priority_stack[i] > 16 || s->irq_stack[i] > PL192_NO_IRQ) {
            return -EINVAL;
        }
    }
    pl192_update(s);
    return 0;
}

static const VMStateDescription vmstate_pl192 = {
    .name = TYPE_PL192,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pl192_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(rawintr, PL192State),
        VMSTATE_UINT32(intselect, PL192State),
        VMSTATE_UINT32(intenable, PL192State),
        VMSTATE_UINT32(softint, PL192State),
        VMSTATE_UINT32(protection, PL192State),
        VMSTATE_UINT32(sw_priority_mask, PL192State),
        VMSTATE_UINT32_ARRAY(vect_addr, PL192State, PL192_INT_SOURCES),
        VMSTATE_UINT32_ARRAY(vect_priority, PL192State, PL192_INT_SOURCES),
        VMSTATE_UINT32(current, PL192State),
        VMSTATE_INT32(stack_i, PL192State),
        VMSTATE_UINT32_ARRAY(priority_stack, PL192State, PL192_PRIO_LEVELS + 1),
        VMSTATE_UINT8_ARRAY(irq_stack, PL192State, PL192_PRIO_LEVELS + 1),
        VMSTATE_UINT32(priority, PL192State),
        VMSTATE_UINT32(daisy_vectaddr, PL192State),
        VMSTATE_UINT32(daisy_priority, PL192State),
        VMSTATE_UINT8(daisy_input, PL192State),
        VMSTATE_BOOL(daisy_fiq, PL192State),
        VMSTATE_END_OF_LIST()
    },
};

static void pl192_init(Object *obj)
{
    PL192State *s = PL192(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &pl192_ops, s, TYPE_PL192, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), pl192_irq_handler, PL192_INT_SOURCES);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
}

static void pl192_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "ARM PrimeCell PL192 vectored interrupt controller";
    dc->vmsd = &vmstate_pl192;
    rc->phases.enter = pl192_reset_enter;
    rc->phases.hold = pl192_reset_hold;
}

static const TypeInfo pl192_info = {
    .name = TYPE_PL192,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL192State),
    .instance_init = pl192_init,
    .class_init = pl192_class_init,
};

static void pl192_register_types(void)
{
    type_register_static(&pl192_info);
}
type_init(pl192_register_types)
