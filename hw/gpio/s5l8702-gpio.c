/*
 * S5L8702 GPIO controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * retailOS 2.0.4: 0x083606d8 encodes GPIOCMD; 0x083607f4/0x0836086c
 * read/write PDAT. 0x0835eb10 saves all 16 ports and encodes output mode
 * 1 as E/F according to PDAT. 0x0835ead0 restores those encoded PCONs.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/gpio/s5l8702-gpio.h"
#include "trace.h"

#define GPIO_PCON       0x00
#define GPIO_PDAT       0x04
#define GPIO_PUNA       0x08
#define GPIO_PUNB       0x0c
#define GPIO_PUNC       0x10
#define GPIO_CMD        0x200

static unsigned gpio_mode(S5L8702GpioState *s, unsigned port, unsigned pin)
{
    return extract32(s->pcon[port], pin * 4, 4);
}

static void gpio_update_outputs(S5L8702GpioState *s, unsigned port)
{
    for (unsigned pin = 0; pin < 8; pin++) {
        bool output = gpio_mode(s, port, pin) == 1;

        qemu_set_irq(s->output[port * 8 + pin],
                     output && (s->pdat[port] & (1 << pin)));
    }
}

static void gpio_configure_pin(S5L8702GpioState *s, unsigned port,
                               unsigned pin, unsigned mode)
{
    if (mode >= 0xe) {
        /* E/F select output mode and preload its low/high data latch. */
        s->pdat[port] = deposit32(s->pdat[port], pin, 1, mode & 1);
        mode = 1;
    }
    s->pcon[port] = deposit32(s->pcon[port], pin * 4, 4, mode);
}

static bool gpio_wheel_bitbang(S5L8702GpioState *s)
{
    return gpio_mode(s, 14, 2) == 1 && gpio_mode(s, 14, 3) == 0 &&
           gpio_mode(s, 14, 4) == 1 && gpio_mode(s, 14, 5) == 0;
}

/*
 * Retained bootstrap workaround: each read advances the wheel serial clock.
 * Only E3/E5 are synthesized, and only while configured for GPIO bit-banging.
 * A physical wheel serial device and its clock still need implementation.
 */
static uint8_t gpio_wheel_read(S5L8702GpioState *s)
{
    uint8_t r = 0;
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->clickwheel_clk) {
        s->clickwheel_clk = 0;
        if (s->pdat[14] & (1 << 2)) {
            s->clickwheel_bit_to_send = s->clickwheel_tx_buf & 1;
            s->clickwheel_tx_buf >>= 1;
            r |= s->clickwheel_bit_to_send << 5;
            s->clickwheel_skip_cycle = 1;
            trace_clickwheel_sending(s->clickwheel_bit_to_send,
                                    s->clickwheel_tx_buf, ns);
        }
    } else {
        if (s->clickwheel_skip_cycle) {
            s->clickwheel_skip_cycle = 0;
            trace_clickwheel_skip_cycle();
        } else {
            s->clickwheel_clk = 1;
            r |= 1 << 3;
        }
        if (s->pdat[14] & (1 << 2)) {
            r |= s->clickwheel_bit_to_send << 5;
            trace_clickwheel_sending(s->clickwheel_bit_to_send,
                                    s->clickwheel_tx_buf, ns);
        } else {
            s->clickwheel_rx_buf <<= 1;
            s->clickwheel_rx_buf |= (s->pdat[14] >> 4) & 1;
            trace_clickwheel_receiving(s->clickwheel_rx_buf, ns);
            if (s->clickwheel_rx_buf == 0xb8800003) {
                /* Bit-reversed button query, 0xc000011d. */
                s->clickwheel_tx_buf = 0x8000023a |
                    (s->clickwheel_select_pressed << 16) |
                    (s->clickwheel_play_pressed << 17) |
                    (s->clickwheel_prev_pressed << 18) |
                    (s->clickwheel_menu_pressed << 19) |
                    (s->clickwheel_next_pressed << 20);
                s->clickwheel_skip_cycle = 1;
                s->clickwheel_clk = 0;
                trace_clickwheel_read_buttons(s->clickwheel_tx_buf);
            }
        }
    }
    trace_clickwheel_out(r);
    return r;
}

static uint8_t gpio_read_data(S5L8702GpioState *s, unsigned port)
{
    uint8_t r = s->pdat[port];

    for (unsigned pin = 0; pin < 8; pin++) {
        if (gpio_mode(s, port, pin) == 0) {
            r = deposit32(r, pin, 1, (s->input_level[port] >> pin) & 1);
        }
    }
    if (port == 14 && gpio_wheel_bitbang(s)) {
        r = (r & ~((1 << 3) | (1 << 5))) | gpio_wheel_read(s);
    }
    return r;
}

static uint64_t s5l8702_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702GpioState *s = opaque;
    unsigned port = offset >> 5;
    uint32_t r;
    const char *name;

    if (offset == GPIO_CMD) {
        trace_s5l8702_gpio_read_cmd(s->gpiocmd);
        return s->gpiocmd;
    }
    if (port >= S5L8702_GPIO_PORTS) {
        goto unimplemented;
    }
    switch (offset & 0x1f) {
    case GPIO_PCON:
        r = s->pcon[port];
        name = "PCON";
        break;
    case GPIO_PDAT:
        r = gpio_read_data(s, port);
        name = "PDAT";
        break;
    case GPIO_PUNA:
        r = s->puna[port];
        name = "PUNA";
        break;
    case GPIO_PUNB:
        r = s->punb[port];
        name = "PUNB";
        break;
    case GPIO_PUNC:
        r = s->punc[port];
        name = "PUNC";
        break;
    default:
        goto unimplemented;
    }
    trace_s5l8702_gpio_read(name, port, r);
    return r;

unimplemented:
    qemu_log_mask(LOG_UNIMP, "s5l8702-gpio: unimplemented read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void s5l8702_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702GpioState *s = opaque;
    unsigned port = offset >> 5;
    const char *name;

    if (offset == GPIO_CMD) {
        unsigned pin = extract32(value, 8, 8);
        unsigned mode = extract32(value, 0, 4);

        port = extract32(value, 16, 8);
        if (port >= S5L8702_GPIO_PORTS || pin >= 8) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8702-gpio: invalid GPIOCMD 0x%08x\n",
                          (uint32_t)value);
            return;
        }
        s->gpiocmd = value;
        gpio_configure_pin(s, port, pin, mode);
        gpio_update_outputs(s, port);
        trace_s5l8702_gpio_write_cmd(port, pin, mode);
        return;
    }
    if (port >= S5L8702_GPIO_PORTS) {
        goto unimplemented;
    }
    switch (offset & 0x1f) {
    case GPIO_PCON:
        for (unsigned pin = 0; pin < 8; pin++) {
            gpio_configure_pin(s, port, pin, extract32(value, pin * 4, 4));
        }
        gpio_update_outputs(s, port);
        name = "PCON";
        break;
    case GPIO_PDAT:
        s->pdat[port] = value;
        gpio_update_outputs(s, port);
        name = "PDAT";
        break;
    case GPIO_PUNA:
        s->puna[port] = value;
        name = "PUNA";
        break;
    case GPIO_PUNB:
        s->punb[port] = value;
        name = "PUNB";
        break;
    case GPIO_PUNC:
        s->punc[port] = value;
        name = "PUNC";
        break;
    default:
        goto unimplemented;
    }
    trace_s5l8702_gpio_write(name, port, value);
    return;

unimplemented:
    qemu_log_mask(LOG_UNIMP, "s5l8702-gpio: unimplemented write at 0x%"
                  HWADDR_PRIx " = 0x%08x\n", offset, (uint32_t)value);
}

static const MemoryRegionOps s5l8702_gpio_ops = {
    .read = s5l8702_gpio_read,
    .write = s5l8702_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void s5l8702_gpio_set(void *opaque, int n, int level)
{
    S5L8702GpioState *s = opaque;
    unsigned port = S5L8702_GPIO_PORT(n);
    unsigned pin = S5L8702_GPIO_PIN(n);

    s->input_level[port] = deposit32(s->input_level[port], pin, 1, !!level);
    qemu_set_irq(s->input_irq[n], !!level);
}

static void gpio_wheel_button(void *opaque, int n, int level)
{
    S5L8702GpioState *s = opaque;
    uint8_t *button[] = {
        &s->clickwheel_select_pressed, &s->clickwheel_play_pressed,
        &s->clickwheel_prev_pressed, &s->clickwheel_menu_pressed,
        &s->clickwheel_next_pressed,
    };

    *button[n] = !!level;
}

static void s5l8702_gpio_reset_enter(Object *obj, ResetType type)
{
    S5L8702GpioState *s = S5L8702_GPIO(obj);

    memset(s->pcon, 0, sizeof(s->pcon));
    memset(s->pdat, 0, sizeof(s->pdat));
    memset(s->puna, 0, sizeof(s->puna));
    memset(s->punb, 0, sizeof(s->punb));
    memset(s->punc, 0, sizeof(s->punc));
    s->gpiocmd = 0;
    s->clickwheel_rx_buf = 0;
    s->clickwheel_tx_buf = 0;
    s->clickwheel_clk = 0;
    s->clickwheel_bit_to_send = 0;
    s->clickwheel_skip_cycle = 0;
    /* Physical input and button levels survive a controller reset. */
}

static void gpio_sync_lines(S5L8702GpioState *s)
{
    for (unsigned port = 0; port < S5L8702_GPIO_PORTS; port++) {
        gpio_update_outputs(s, port);
        for (unsigned pin = 0; pin < 8; pin++) {
            qemu_set_irq(s->input_irq[port * 8 + pin],
                         (s->input_level[port] >> pin) & 1);
        }
    }
}

static void s5l8702_gpio_reset_hold(Object *obj)
{
    gpio_sync_lines(S5L8702_GPIO(obj));
}

static int s5l8702_gpio_post_load(void *opaque, int version_id)
{
    gpio_sync_lines(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_gpio = {
    .name = TYPE_S5L8702_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8702_gpio_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pcon, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(pdat, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(input_level, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(puna, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(punb, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(punc, S5L8702GpioState, S5L8702_GPIO_PORTS),
        VMSTATE_UINT8(gpiocmd, S5L8702GpioState),
        VMSTATE_UINT32(clickwheel_rx_buf, S5L8702GpioState),
        VMSTATE_UINT32(clickwheel_tx_buf, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_clk, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_bit_to_send, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_skip_cycle, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_select_pressed, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_menu_pressed, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_play_pressed, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_prev_pressed, S5L8702GpioState),
        VMSTATE_UINT8(clickwheel_next_pressed, S5L8702GpioState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_gpio_init(Object *obj)
{
    S5L8702GpioState *s = S5L8702_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_gpio_ops, s,
                          TYPE_S5L8702_GPIO, S5L8702_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in(DEVICE(s), s5l8702_gpio_set, S5L8702_GPIO_PINS);
    qdev_init_gpio_in_named(DEVICE(s), gpio_wheel_button, "wheel-button", 5);
    qdev_init_gpio_out(DEVICE(s), s->output, S5L8702_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(s), s->input_irq, "input-irq",
                            S5L8702_GPIO_PINS);
}

static void s5l8702_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8702_gpio;
    rc->phases.enter = s5l8702_gpio_reset_enter;
    rc->phases.hold = s5l8702_gpio_reset_hold;
}

static const TypeInfo s5l8702_gpio_types[] = {
    {
        .name = TYPE_S5L8702_GPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702GpioState),
        .instance_init = s5l8702_gpio_init,
        .class_init = s5l8702_gpio_class_init,
    },
};
DEFINE_TYPES(s5l8702_gpio_types);
