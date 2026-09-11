/*
 * Samsung S5L8702 external display control registers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is a partial register model, not a working external display pipeline.
 * retailOS 2.0.4 0x0815ec10 writes the mixer background at 0x39200048.
 * Sleep routine 0x080bc4e0 writes 1 to 0x39300280 and sets bit 0 in
 * 0x39300284; 0x080b3888 clears the latter during output initialization.
 * 0x0815dfe4 then clears encoder-config bits 3:0 and stops the three blocks:
 * each control bit 0 is cleared, followed by polling bit 1 for acknowledgement.
 * The encoder/compositor, analog outputs, clocks and interrupts are absent.
 * Stopping is immediate because no scanout is modeled. Only established
 * control register locations are mapped. Reserved-bit
 * masks and physical reset values remain unverified.
 */
#include "qemu/osdep.h"
#include "hw/misc/s5l8702-tvo.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static uint64_t tvo_background_read(void *opaque, hwaddr offset, unsigned size)
{
    return S5L8702_TVO(opaque)->background;
}

static void tvo_background_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    S5L8702_TVO(opaque)->background = value;
}

static uint64_t tvo_output_read(void *opaque, hwaddr offset, unsigned size)
{
    return S5L8702_TVO(opaque)->output_control[offset / 4];
}

static void tvo_output_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702_TVO(opaque)->output_control[offset / 4] = value;
}

static uint64_t tvo_run_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t value = *(uint32_t *)opaque;

    return value | (value & 1 ? 0 : 2);
}

static void tvo_run_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    /* Stop acknowledgement is read-only and follows the enable state. */
    *(uint32_t *)opaque = value & ~2u;
}

static uint64_t tvo_config_read(void *opaque, hwaddr offset, unsigned size)
{
    return S5L8702_TVO(opaque)->encoder_config;
}

static void tvo_config_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702_TVO(opaque)->encoder_config = value;
}

static const MemoryRegionOps tvo_background_ops = {
    .read = tvo_background_read,
    .write = tvo_background_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps tvo_output_ops = {
    .read = tvo_output_read,
    .write = tvo_output_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps tvo_run_ops = {
    .read = tvo_run_read,
    .write = tvo_run_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps tvo_config_ops = {
    .read = tvo_config_read,
    .write = tvo_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void tvo_reset_enter(Object *obj, ResetType type)
{
    S5L8702TvoState *s = S5L8702_TVO(obj);

    /* Deterministic defaults; the physical reset values are not established. */
    s->background = 0;
    memset(s->output_control, 0, sizeof(s->output_control));
    memset(s->run_control, 0, sizeof(s->run_control));
    s->encoder_config = 0;
}

static const VMStateDescription vmstate_tvo = {
    .name = TYPE_S5L8702_TVO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(background, S5L8702TvoState),
        VMSTATE_UINT32_ARRAY(output_control, S5L8702TvoState, 2),
        VMSTATE_UINT32_ARRAY(run_control, S5L8702TvoState, 3),
        VMSTATE_UINT32(encoder_config, S5L8702TvoState),
        VMSTATE_END_OF_LIST()
    }
};

static void tvo_init(Object *obj)
{
    S5L8702TvoState *s = S5L8702_TVO(obj);

    memory_region_init_io(&s->background_mmio, obj, &tvo_background_ops, s,
                          TYPE_S5L8702_TVO ".background", 4);
    memory_region_init_io(&s->output_mmio, obj, &tvo_output_ops, s,
                          TYPE_S5L8702_TVO ".output-control", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->background_mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->output_mmio);
    for (unsigned i = 0; i < ARRAY_SIZE(s->run_mmio); i++) {
        g_autofree char *name = g_strdup_printf(TYPE_S5L8702_TVO ".run%u", i);

        memory_region_init_io(&s->run_mmio[i], obj, &tvo_run_ops,
                              &s->run_control[i], name, 4);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->run_mmio[i]);
    }
    memory_region_init_io(&s->config_mmio, obj, &tvo_config_ops, s,
                          TYPE_S5L8702_TVO ".encoder-config", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->config_mmio);
}

static void tvo_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 external display control registers";
    dc->user_creatable = false;
    dc->vmsd = &vmstate_tvo;
    rc->phases.enter = tvo_reset_enter;
}

static const TypeInfo tvo_types[] = {
    {
        .name = TYPE_S5L8702_TVO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702TvoState),
        .instance_init = tvo_init,
        .class_init = tvo_class_init,
    },
};
DEFINE_TYPES(tvo_types)
