#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/s5l8702-sysic.h"
#include "hw/irq.h"
#include "trace.h"

/* Power Management */
#define SYSIC_CLOCK_CONTROL  0x00
#define SYSIC_POWER_SETSTATE 0x08
#define SYSIC_POWER_ONCTRL   0x0C
#define SYSIC_POWER_OFFCTRL  0x10
#define SYSIC_POWER_STATE    0x14
#define SYSIC_POWER_ID       0x44

#define POWER_ID_ADM         0x10

/* GPIO Interrupt Controller */
#define GPIO_INTLEVEL 0x80
#define GPIO_INTSTAT  0xA0
#define GPIO_INTEN    0xC0
#define GPIO_INTTYPE  0xE0

/*
 * retailOS 2.0.4 0x083607c0/0x083607e0 map pin numbers to groups/bits.
 * 0x08360824 programs polarity; 0x0836075c programs type and enable.
 * The interrupt dispatcher at IRAM 0x22002ee0 reads status & enable,
 * invokes the registered callbacks, then writes the captured status (W1C).
 */
static uint32_t sysic_pending(S5L8702SysICState *s, unsigned group)
{
    uint32_t type = s->gpio_int_type[group];
    uint32_t active = ~(s->gpio_input[group] ^ s->gpio_int_level[group]);

    return (s->gpio_int_status[group] & ~type) | (active & type);
}

static void sysic_update_irq(S5L8702SysICState *s, unsigned group)
{
    qemu_set_irq(s->gpio_irqs[group],
                 !!(sysic_pending(s, group) & s->gpio_int_enabled[group]));
}

static void sysic_input(void *opaque, int pin, int level)
{
    S5L8702SysICState *s = opaque;
    unsigned group = 6 - (pin >> 5);
    unsigned bit = (0x18 - (pin & 0x18)) | (pin & 7);
    uint32_t mask = 1u << bit;
    bool old = !!(s->gpio_input[group] & mask);

    if (level) {
        s->gpio_input[group] |= mask;
    } else {
        s->gpio_input[group] &= ~mask;
    }
    if (old != !!level && !(s->gpio_int_type[group] & mask) &&
        !!level == !!(s->gpio_int_level[group] & mask)) {
        s->gpio_int_status[group] |= mask;
    }
    sysic_update_irq(s, group);
}

static uint64_t s5l8702_sysic_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8702SysICState *s = S5L8702_SYSIC(opaque);
    uint32_t val = 0;

    switch (addr) {
    case SYSIC_CLOCK_CONTROL:
        val = s->clock_control;
        break;
    case SYSIC_POWER_ID:
        val = (2 << 0x18);
        break;
    case SYSIC_POWER_SETSTATE:
    case SYSIC_POWER_STATE:
        val = s->power_state;
        break;
    case 0x7a:
    case 0x7c:
        val = 1;
        break;
    case GPIO_INTLEVEL ... (GPIO_INTLEVEL + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        val = s->gpio_int_level[(addr - GPIO_INTLEVEL) / 4];
        break;
    case GPIO_INTSTAT ... (GPIO_INTSTAT + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        val = sysic_pending(s, (addr - GPIO_INTSTAT) / 4);
        break;
    case GPIO_INTEN ... (GPIO_INTEN + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        val = s->gpio_int_enabled[(addr - GPIO_INTEN) / 4];
        break;
    case GPIO_INTTYPE ... (GPIO_INTTYPE + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        val = s->gpio_int_type[(addr - GPIO_INTTYPE) / 4];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%04x\n", __func__, (uint32_t)addr);
        break;
    }

    trace_s5l8702_sysic_read((uint32_t)addr, val);
    return val;
}

static void s5l8702_sysic_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    S5L8702SysICState *s = S5L8702_SYSIC(opaque);
    uint8_t group;

    trace_s5l8702_sysic_write((uint32_t)addr, (uint32_t)val);

    switch (addr) {
    case SYSIC_CLOCK_CONTROL:
        /*
         * retailOS 2.0.4, IRAM 0x22002dcc: preserve clock control across
         * WFI, setting and polling bit 0 before WFI and bit 12 afterwards.
         * Clock switching completes synchronously in this model.
         */
        s->clock_control = val;
        break;
    case SYSIC_POWER_ONCTRL:
        trace_s5l8702_sysic_power_onctrl((uint32_t)val);
        if ((val & 0x20) != 0 || (val & 0x4) != 0 || (val & POWER_ID_ADM) != 0) {
            break;
        }
        s->power_state = val;
        break;
    case SYSIC_POWER_OFFCTRL:
        trace_s5l8702_sysic_power_offctrl((uint32_t)val);
        s->power_state = val;
        break;
    case GPIO_INTLEVEL ... (GPIO_INTLEVEL + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        group = (addr - GPIO_INTLEVEL) / 4;
        s->gpio_int_level[group] = val;
        sysic_update_irq(s, group);
        break;
    case GPIO_INTSTAT ... (GPIO_INTSTAT + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        group = (addr - GPIO_INTSTAT) / 4;
        /* Write-1-to-clear interrupt status */
        trace_s5l8702_sysic_gpio_intstat_clear(group, (uint32_t)val);
        s->gpio_int_status[group] &= ~val;
        sysic_update_irq(s, group);
        break;
    case GPIO_INTEN ... (GPIO_INTEN + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        group = (addr - GPIO_INTEN) / 4;
        trace_s5l8702_sysic_gpio_inten_set(group, (uint32_t)val);
        s->gpio_int_enabled[group] = val;
        sysic_update_irq(s, group);
        break;
    case GPIO_INTTYPE ... (GPIO_INTTYPE + (S5L8702_SYSIC_GPIO_GROUPS - 1) * 4):
        group = (addr - GPIO_INTTYPE) / 4;
        trace_s5l8702_sysic_gpio_inttype_set(group, (uint32_t)val);
        s->gpio_int_type[group] = val;
        sysic_update_irq(s, group);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%04x = 0x%08x\n",
                      __func__, (uint32_t)addr, (uint32_t)val);
        break;
    }
}

static const MemoryRegionOps s5l8702_sysic_ops = {
    .read = s5l8702_sysic_read,
    .write = s5l8702_sysic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void s5l8702_sysic_reset(DeviceState *dev)
{
    S5L8702SysICState *s = S5L8702_SYSIC(dev);

    trace_s5l8702_sysic_reset();
    s->power_state = 0;
    s->clock_control = 0;
    s->usb_connected = false;
    for (int i = 0; i < S5L8702_SYSIC_GPIO_GROUPS; i++) {
        s->gpio_int_level[i] = 0;
        s->gpio_int_status[i] = 0;
        s->gpio_int_enabled[i] = 0;
        s->gpio_int_type[i] = 0;
        qemu_irq_lower(s->gpio_irqs[i]);
    }
}

static void s5l8702_sysic_init(Object *obj)
{
    S5L8702SysICState *s = S5L8702_SYSIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    trace_s5l8702_sysic_init();
    memory_region_init_io(&s->iomem, obj, &s5l8702_sysic_ops, s, TYPE_S5L8702_SYSIC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (int i = 0; i < S5L8702_SYSIC_GPIO_GROUPS; i++) {
        sysbus_init_irq(sbd, &s->gpio_irqs[i]);
    }
    qdev_init_gpio_in(DEVICE(s), sysic_input, S5L8702_SYSIC_INPUTS);
}

static int s5l8702_sysic_post_load(void *opaque, int version_id)
{
    S5L8702SysICState *s = opaque;

    for (unsigned i = 0; i < S5L8702_SYSIC_GPIO_GROUPS; i++) {
        sysic_update_irq(s, i);
    }
    return 0;
}

static const VMStateDescription vmstate_s5l8702_sysic = {
    .name = TYPE_S5L8702_SYSIC,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = s5l8702_sysic_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(power_state, S5L8702SysICState),
        VMSTATE_UINT32(clock_control, S5L8702SysICState),
        VMSTATE_UINT32_ARRAY(gpio_int_level, S5L8702SysICState, 7),
        VMSTATE_UINT32_ARRAY(gpio_int_status, S5L8702SysICState, 7),
        VMSTATE_UINT32_ARRAY(gpio_int_enabled, S5L8702SysICState, 7),
        VMSTATE_UINT32_ARRAY(gpio_int_type, S5L8702SysICState, 7),
        VMSTATE_UINT32_ARRAY(gpio_input, S5L8702SysICState, 7),
        VMSTATE_BOOL(usb_connected, S5L8702SysICState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_sysic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = s5l8702_sysic_reset;
    dc->vmsd = &vmstate_s5l8702_sysic;
}

static const TypeInfo s5l8702_sysic_type_info = {
    .name = TYPE_S5L8702_SYSIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702SysICState),
    .instance_init = s5l8702_sysic_init,
    .class_init = s5l8702_sysic_class_init,
};

static void s5l8702_sysic_register_types(void)
{
    type_register_static(&s5l8702_sysic_type_info);
}

type_init(s5l8702_sysic_register_types)
