/*
 * Partial Samsung S5L8702 "SM1" engine, 0x38500000.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic; the reference's
 * experimental PCM transport is omitted. Power/command acknowledgements
 * remain approximations; the processing engine and memory map are incomplete.
 * See docs/system/arm/ipod-classic-audit.rst for original firmware evidence.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/misc/s5l8702-sm1.h"

/*
 * The eight sub-block "state" registers (sub+0x14), at the sub-block bases
 * {0,0x20,0x40,0x60,0x100,0x180,0x200,0x230} + 0x14. The command register
 * (sub+0x10) sits 4 bytes below each. Enumerated rather than "ends in 4" so an
 * unrelated register (e.g. +0x404) is never mistaken for a sub-block state.
 */
static const hwaddr sm1_sub_state_off[] = {
    0x0014, 0x0034, 0x0054, 0x0074,   /* idx 0..3: base + idx*0x20 + 0x14 */
    0x0114, 0x0194, 0x0214, 0x0244,   /* idx 4..7                          */
};

static bool sm1_is_sub_state(hwaddr offset)
{
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i]) {
            return true;
        }
    }
    return false;
}

static bool sm1_is_sub_cmd(hwaddr offset)
{
    for (size_t i = 0; i < ARRAY_SIZE(sm1_sub_state_off); i++) {
        if (offset == sm1_sub_state_off[i] - 4) {
            return true;
        }
    }
    return false;
}

static uint64_t s5l8702_sm1_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702SM1State *s = S5L8702_SM1(opaque);
    uint32_t val;

    /* Above the modelled low page: a plain zero window. */
    if (offset >= S5L8702_SM1_REG_BYTES) {
        return 0;
    }

    switch (offset) {
    case S5L8702_SM1_ID:
        /* Block identity. A mismatch here is an eternal `b .`. */
        val = S5L8702_SM1_ID_MAGIC;
        break;

    case S5L8702_SM1_STAT:
        /*
         * Clock/engine status; bit 2 is "clock ready", answered from powered.
         */
        val = s->powered ? S5L8702_SM1_STAT_READY : 0;
        break;

    case S5L8702_SM1_RUN:
        /*
         * Run/run-state: write 1 -> read 1. The &3 the poll applies is theirs.
         */
        val = s->reg[S5L8702_SM1_RUN / 4];
        break;

    default:
        if (sm1_is_sub_state(offset)) {
            /*
             * Sub-block state (sub+0x14): a state machine. 7 while the last
             * command was power-up (4) or run (16); 0 once stopped (96) or
             * never
             * commanded. These instantaneous transitions remain approximate.
             */
            uint32_t cmd = s->reg[(offset - 4) / 4];
            switch (cmd) {
            case S5L8702_SM1_SUB_CMD_UP:
            case S5L8702_SM1_SUB_CMD_RUN:
                val = S5L8702_SM1_SUB_STATE_READY;
                break;
            default:
                val = 0;
                break;
            }
        } else {
            /*
             * Undecoded registers retain the register-file placeholder.
             * In particular, +0xa44/+0xa48 (four pairs, stride 0x14) describe
             * memory length/byte offset in retailOS 0x080ab624/0x080cd04c.
             * They must not advance with the unrelated I2S sample count.
             * Their actual region layout still needs reverse engineering.
             */
            val = s->reg[offset / 4];
        }
        break;
    }
    return val;
}

static void s5l8702_sm1_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702SM1State *s = S5L8702_SM1(opaque);

    if (offset >= S5L8702_SM1_REG_BYTES) {
        return;
    }

    /*
     * The engine is "powered" (clock will lock, run bit follows) once the
     * firmware issues any of its bring-up writes -- deriving this from the
     * guest's own first move keeps the model coherent instead of hard-wiring
     * the bit on.
     */
    switch (offset) {
    case S5L8702_SM1_CODEC_EN:
    case S5L8702_SM1_COMMIT:
        s->powered = true;
        break;
    case S5L8702_SM1_RUN:
        if (value & S5L8702_SM1_RUN_MASK) {
            s->powered = true;
        }
        break;
    default:
        if (sm1_is_sub_cmd(offset) && value != 0) {
            s->powered = true;   /* a channel was commanded up */
        }
        break;
    }

    s->reg[offset / 4] = (uint32_t)value;
}

static const MemoryRegionOps s5l8702_sm1_ops = {
    .read = s5l8702_sm1_read,
    .write = s5l8702_sm1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_sm1_reset_enter(Object *obj, ResetType type)
{
    S5L8702SM1State *s = S5L8702_SM1(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->powered = false;
}

static void s5l8702_sm1_init(Object *obj)
{
    S5L8702SM1State *s = S5L8702_SM1(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_sm1_ops, s,
                          TYPE_S5L8702_SM1, S5L8702_SM1_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_s5l8702_sm1 = {
    .name = TYPE_S5L8702_SM1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, S5L8702SM1State, S5L8702_SM1_REG_WORDS),
        VMSTATE_BOOL(powered, S5L8702SM1State),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_sm1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 SM1 audio clock/stream engine";
    rc->phases.enter = s5l8702_sm1_reset_enter;
    dc->vmsd = &vmstate_s5l8702_sm1;
    dc->user_creatable = false;
}

static const TypeInfo s5l8702_sm1_types[] = {
    {
        .name          = TYPE_S5L8702_SM1,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_init = s5l8702_sm1_init,
        .instance_size = sizeof(S5L8702SM1State),
        .class_init    = s5l8702_sm1_class_init,
    },
};
DEFINE_TYPES(s5l8702_sm1_types)
