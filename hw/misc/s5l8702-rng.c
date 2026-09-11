/*
 * Samsung S5L8702 random number generator, 0x3C100000.
 *
 * See include/hw/misc/s5l8702-rng.h for the reverse-engineered register
 * contract. A plain 32-bit LCG seeded from what the guest writes: a given boot
 * produces the same sequence twice, which the firmware is fine with -- it only
 * wants variety, and reproducibility is worth more here for debugging.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic (the inline block in
 * ipod_classic.c), adapted to this fork's s5l8702-* device convention.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/s5l8702-rng.h"

#define S5L8702_RNG_STATUS  0x00   /* read: ready in [2:0]. Written with 0. */
#define S5L8702_RNG_DATA    0x04
#define S5L8702_RNG_SEED    0x08

static uint64_t s5l8702_rng_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702RngState *s = S5L8702_RNG(opaque);

    switch (offset) {
    case S5L8702_RNG_STATUS:
        /*
         * osos 0x08327760 spins on "& 7" with no timeout, and nothing ever
         * enables the block, so a word is always available. Which of the three
         * bits means what is UNDETERMINED: the firmware only tests them
         * together.
         */
        return 7;
    case S5L8702_RNG_DATA:
        s->state = s->state * 1103515245u + 12345u;
        return s->state;
    case S5L8702_RNG_SEED:
        return s->seed;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read of +0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void s5l8702_rng_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702RngState *s = S5L8702_RNG(opaque);

    switch (offset) {
    case S5L8702_RNG_STATUS:
        /* Only ever written with zero, and readiness does not depend on it. */
        break;
    case S5L8702_RNG_SEED:
        s->seed = value;
        s->state = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%" PRIx64 " to +0x%"
                      HWADDR_PRIx "\n", __func__, value, offset);
        break;
    }
}

static const MemoryRegionOps s5l8702_rng_ops = {
    .read = s5l8702_rng_read,
    .write = s5l8702_rng_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_rng_reset(DeviceState *dev)
{
    S5L8702RngState *s = S5L8702_RNG(dev);

    s->seed = 0;
    s->state = 0;
}

static void s5l8702_rng_init(Object *obj)
{
    S5L8702RngState *s = S5L8702_RNG(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_rng_ops, s,
                          TYPE_S5L8702_RNG, S5L8702_RNG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_rng = {
    .name = TYPE_S5L8702_RNG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(seed, S5L8702RngState),
        VMSTATE_UINT32(state, S5L8702RngState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_rng_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 random number generator";
    dc->reset = s5l8702_rng_reset;
    dc->vmsd = &vmstate_s5l8702_rng;
    dc->user_creatable = false;
}

static const TypeInfo s5l8702_rng_types[] = {
    {
        .name          = TYPE_S5L8702_RNG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_init = s5l8702_rng_init,
        .instance_size = sizeof(S5L8702RngState),
        .class_init    = s5l8702_rng_class_init,
    },
};
DEFINE_TYPES(s5l8702_rng_types)
