/*
 * Samsung S5L8702 clock controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * retailOS 2.0.4: 0x2200200c selects sources and divides clocks;
 * 0x0835fe18 sets the HCLK/PCLK dividers; 0x22002dcc gates clocks for sleep.
 * Boot ROM 0x200014f0 calculates PLL frequencies. PLL2's divide-only path
 * is corroborated by the bootloader's PMS=0x01002401 and retailOS's
 * 216 MHz source value at 0x2200ad0c, with the 12 MHz board oscillator.
 * PLL locking is instantaneous; analog startup timing remains unmodeled.
 */
#include "qemu/osdep.h"
#include "hw/misc/s5l8702-clk.h"
#include "hw/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/runstate.h"
#include "trace.h"

#define CLKCON0  0x00
#define CLKCON1  0x04
#define CLKCON3  0x0c
#define CLKCON4  0x10
#define PLLPMS   0x20
#define PLLLOCK  0x40
#define PLLMODE  0x44
#define PWRCON1  0x4c
#define SWRCON   0x50
#define PLLMOD2  0x60
#define SWRCON_RESET 0xaa5
#define TIMER_GATE BIT(5)

#define REG(s, offset) ((s)->regs[(offset) / sizeof(uint32_t)])

enum {
    OSC0,
    OSC1,
    ALT0,
    ALT1,
};

static bool pll_divide_mode(S5L8702ClkState *s, unsigned pll)
{
    return pll == 2 || (REG(s, PLLMODE) & BIT(4 + pll));
}

static uint64_t pll_frequency(S5L8702ClkState *s, unsigned pll)
{
    uint32_t pms = REG(s, PLLPMS + pll * 4);
    unsigned p = extract32(pms, 24, 6);
    unsigned m = extract32(pms, 8, 8);
    unsigned shift = extract32(pms, 0, 3);
    unsigned source = OSC0;
    uint32_t mode = REG(s, PLLMOD2);
    uint64_t rate;

    if (!p || !m) {
        return 0;
    }
    if (!pll_divide_mode(s, pll)) {
        return (clock_get_hz(s->osc[OSC1]) * m * p) >> shift;
    }
    if (pll == 2 ? (REG(s, PLLMODE) & BIT(6)) : (mode & BIT(4 + pll))) {
        source = mode & BIT(pll) ? ALT1 : ALT0;
    }
    rate = clock_get_hz(s->osc[source]);
    return ((rate * m) / p) >> shift;
}

static uint64_t clock_source(S5L8702ClkState *s, unsigned source)
{
    uint32_t mode = REG(s, PLLMODE);
    unsigned pll;

    if (!source) {
        return clock_get_hz(s->osc[mode & BIT(8) ? OSC1 : OSC0]);
    }
    pll = source - 1;
    if (!(mode & BIT(16 + pll))) {
        return clock_get_hz(s->osc[OSC1]);
    }
    return mode & BIT(pll) ? pll_frequency(s, pll) : 0;
}

static uint64_t clock_group(S5L8702ClkState *s, uint16_t config,
                            bool second_divider)
{
    uint64_t rate;

    if (config & BIT(15)) {
        return 0;
    }
    rate = clock_source(s, extract32(config, 12, 2));
    rate /= extract32(config, 0, 4) + 1;
    if (second_divider) {
        rate /= extract32(config, 4, 4) + 1;
    }
    return rate;
}

static unsigned bus_divider(S5L8702ClkState *s, unsigned shift)
{
    uint32_t config = REG(s, CLKCON1) >> shift;

    return config & BIT(6) ? 2 * (extract32(config, 0, 5) + 1) : 1;
}

static void set_clock_rate(Clock *clk, uint64_t rate, bool propagate)
{
    if (propagate) {
        clock_update_hz(clk, rate);
    } else {
        clock_set_hz(clk, rate);
    }
}

static void s5l8702_clk_update(S5L8702ClkState *s, bool propagate)
{
    /* Boot ROM 0x20002fb4 selects gates 0x22/0x2b/0x2f via 0x2000147c. */
    static const unsigned spi_gates[] = { 2, 11, 15 };
    uint64_t source = clock_group(s, REG(s, CLKCON0), false);
    uint64_t cclk = source / bus_divider(s, 24);
    uint64_t hclk = source / bus_divider(s, 16);
    uint64_t pclk = source / bus_divider(s, 8);
    uint64_t eclk = clock_group(s, REG(s, CLKCON4) >> 16, true);
    bool timer_enabled = !(REG(s, PWRCON1) & TIMER_GATE);

    set_clock_rate(s->cclk, cclk, propagate);
    set_clock_rate(s->hclk, hclk, propagate);
    set_clock_rate(s->pclk, pclk, propagate);
    set_clock_rate(s->eclk, eclk, propagate);
    /* retailOS 0x220021a0: clock group 6 drives the codec's MCLK pin. */
    set_clock_rate(s->codec_mclk, clock_group(s, REG(s, CLKCON3), true),
                   propagate);
    set_clock_rate(s->timer_pclk, timer_enabled ? pclk : 0, propagate);
    set_clock_rate(s->timer_eclk, timer_enabled ? eclk : 0, propagate);
    /* retailOS 0x2200881c gates I2S0 through PWRCON1 bit 7. */
    set_clock_rate(s->i2s_pclk, REG(s, PWRCON1) & BIT(7) ? 0 : pclk,
                   propagate);
    for (unsigned i = 0; i < ARRAY_SIZE(spi_gates); i++) {
        bool enabled = !(REG(s, PWRCON1) & BIT(spi_gates[i]));

        set_clock_rate(s->spi_pclk[i], enabled ? pclk : 0, propagate);
    }
    /* aupd diagnostic 0x08009434 -> 0x080164f4: I2C gates 4 and 6. */
    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c_pclk); i++) {
        bool enabled = !(REG(s, PWRCON1) & BIT(4 + 2 * i));

        set_clock_rate(s->i2c_pclk[i], enabled ? pclk : 0, propagate);
    }
    /* aupd diagnostic 0x0801ab30 -> 0x080164f4: UART block gate 9. */
    set_clock_rate(s->uart_pclk, REG(s, PWRCON1) & BIT(9) ? 0 : pclk,
                   propagate);
    trace_s5l8702_clk_rates(cclk, hclk, pclk, eclk, timer_enabled);
}

static void s5l8702_clk_source_changed(void *opaque, ClockEvent event)
{
    s5l8702_clk_update(opaque, true);
}

static uint64_t s5l8702_clk_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702ClkState *s = opaque;
    uint32_t value = 0;

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-clk: read at 0x%" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
    if (offset == PLLLOCK) {
        for (unsigned pll = 0; pll < 3; pll++) {
            if ((REG(s, PLLMODE) & BIT(pll)) && pll_frequency(s, pll)) {
                value |= BIT(pll);
                if (pll_divide_mode(s, pll)) {
                    value |= BIT(4 + pll);
                }
            }
        }
    } else {
        value = REG(s, offset);
    }
    trace_s5l8702_clk_access(false, offset, value);
    return value;
}

static void s5l8702_clk_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    S5L8702ClkState *s = opaque;

    trace_s5l8702_clk_access(true, offset, value);
    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-clk: write at 0x%" HWADDR_PRIx "\n",
                      offset);
        return;
    }
    if (offset == PLLLOCK) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-clk: PLLLOCK is read-only\n");
        return;
    }
    REG(s, offset) = value;
    if (offset == SWRCON) {
        /* Only this command is established by retailOS 0x0835d028. */
        if (value == SWRCON_RESET) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        } else if (value) {
            qemu_log_mask(LOG_UNIMP, "s5l8702-clk: reset command 0x%08x\n",
                          (uint32_t)value);
        }
        return;
    }
    s5l8702_clk_update(s, true);
}

static const MemoryRegionOps s5l8702_clk_ops = {
    .read = s5l8702_clk_read,
    .write = s5l8702_clk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_clk_reset_enter(Object *obj, ResetType type)
{
    S5L8702ClkState *s = S5L8702_CLK(obj);

    trace_s5l8702_clk_reset();
    memset(s->regs, 0, sizeof(s->regs));
}

static void s5l8702_clk_reset_hold(Object *obj)
{
    s5l8702_clk_update(S5L8702_CLK(obj), true);
}

static int s5l8702_clk_post_load(void *opaque, int version_id)
{
    /* Consumers restore their input periods; do not run their callbacks. */
    s5l8702_clk_update(opaque, false);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_clk = {
    .name = TYPE_S5L8702_CLK,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8702_clk_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8702ClkState, S5L8702_CLK_NUM_REGS),
        VMSTATE_ARRAY_CLOCK(osc, S5L8702ClkState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_clk_init(Object *obj)
{
    static const char *const sources[] = { "osc0", "osc1", "alt0", "alt1" };
    S5L8702ClkState *s = S5L8702_CLK(obj);
    DeviceState *dev = DEVICE(obj);

    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        s->osc[i] = qdev_init_clock_in(dev, sources[i],
                                       s5l8702_clk_source_changed, s,
                                       ClockUpdate);
    }
    s->cclk = qdev_init_clock_out(dev, "cclk");
    s->hclk = qdev_init_clock_out(dev, "hclk");
    s->pclk = qdev_init_clock_out(dev, "pclk");
    s->eclk = qdev_init_clock_out(dev, "eclk");
    s->codec_mclk = qdev_init_clock_out(dev, "codec-mclk");
    s->i2s_pclk = qdev_init_clock_out(dev, "i2s0-pclk");
    s->uart_pclk = qdev_init_clock_out(dev, "uart-pclk");
    s->timer_pclk = qdev_init_clock_out(dev, "timer-pclk");
    s->timer_eclk = qdev_init_clock_out(dev, "timer-eclk");
    s->spi_pclk[0] = qdev_init_clock_out(dev, "spi0-pclk");
    s->spi_pclk[1] = qdev_init_clock_out(dev, "spi1-pclk");
    s->spi_pclk[2] = qdev_init_clock_out(dev, "spi2-pclk");
    s->i2c_pclk[0] = qdev_init_clock_out(dev, "i2c0-pclk");
    s->i2c_pclk[1] = qdev_init_clock_out(dev, "i2c1-pclk");
    memory_region_init_io(&s->iomem, obj, &s5l8702_clk_ops, s,
                          TYPE_S5L8702_CLK, S5L8702_CLK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void s5l8702_clk_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = s5l8702_clk_reset_enter;
    rc->phases.hold = s5l8702_clk_reset_hold;
    dc->vmsd = &vmstate_s5l8702_clk;
}

static const TypeInfo s5l8702_clk_types[] = {
    {
        .name = TYPE_S5L8702_CLK,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_init = s5l8702_clk_init,
        .instance_size = sizeof(S5L8702ClkState),
        .class_init = s5l8702_clk_class_init,
    },
};
DEFINE_TYPES(s5l8702_clk_types);
