#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/s5l8702-miu.h"
#include "trace.h"

#define REG_INDEX(offset) ((offset) / sizeof(uint32_t))

static uint64_t s5l8702_miu_read(void *opaque, hwaddr offset, unsigned size)
{
    const S5L8702MiuState *s = S5L8702_MIU(opaque);
    const uint32_t idx = REG_INDEX(offset);
    uint32_t val = s->regs[idx];

    trace_s5l8702_miu_read((uint32_t)offset, val);
    return val;
}

static void s5l8702_miu_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702MiuState *s = S5L8702_MIU(opaque);
    const uint32_t idx = REG_INDEX(offset);

    trace_s5l8702_miu_write((uint32_t)offset, (uint32_t)value);
    s->regs[idx] = (uint32_t)value;
    if (offset == 0) {
        qemu_set_irq(s->remap, value & 1);
    }
}

static const MemoryRegionOps s5l8702_miu_ops = {
    .read = s5l8702_miu_read,
    .write = s5l8702_miu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_miu_reset_enter(Object *obj, ResetType type)
{
    S5L8702MiuState *s = S5L8702_MIU(obj);

    trace_s5l8702_miu_reset();
    memset(s->regs, 0, sizeof(s->regs));
}

static void s5l8702_miu_reset_hold(Object *obj)
{
    qemu_set_irq(S5L8702_MIU(obj)->remap, 0);
}

static int s5l8702_miu_post_load(void *opaque, int version_id)
{
    S5L8702MiuState *s = opaque;

    qemu_set_irq(s->remap, s->regs[0] & 1);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_miu = {
    .name = TYPE_S5L8702_MIU,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8702_miu_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8702MiuState, S5L8702_MIU_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_miu_init(Object *obj)
{
    S5L8702MiuState *s = S5L8702_MIU(obj);

    trace_s5l8702_miu_init();

    memory_region_init_io(&s->iomem, OBJECT(s), &s5l8702_miu_ops, s, TYPE_S5L8702_MIU, S5L8702_MIU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->remap, "remap", 1);
}

static void s5l8702_miu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = s5l8702_miu_reset_enter;
    rc->phases.hold = s5l8702_miu_reset_hold;
    dc->vmsd = &vmstate_s5l8702_miu;
}

static const TypeInfo s5l8702_miu_types[] = {
    {
        .name          = TYPE_S5L8702_MIU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702MiuState),
        .instance_init = s5l8702_miu_init,
        .class_init    = s5l8702_miu_class_init,
    },
};
DEFINE_TYPES(s5l8702_miu_types);
