/*
 * S5L8702 peripheral regression tests. No Apple firmware is needed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define VIC0 0x38e00000
#define VIC1 0x38e01000
#define I2C0 0x3c600000
#define I2C1 0x3c900000
#define TIMER 0x3c700000
#define WHEEL 0x3c200000
#define AES 0x38c00000
#define CLK 0x3c500000
#define SPI0 0x3c300000
#define SPI1 0x3ce00000
#define I2S0 0x3ca00000
#define DMA0 0x38200000
#define SM1 0x38500000

static char *rom, *nor, *disk;

static QTestState *start_with_audio(const char *args, const char *audio)
{
    return qtest_initf("-M ipod-classic,bootrom=%s "
                       "-drive if=mtd,file=%s,format=raw "
                       "-drive if=ide,file=%s,format=raw -snapshot "
                       "-global s5l8702-aes.fused-key-bypass=on "
                       "-audiodev %s,id=codec-test "
                       "-global s5l8702-cs42l55.audiodev=codec-test "
                       "-rtc base=2024-02-28T23:59:58,clock=vm %s",
                       rom, nor, disk, audio, args);
}

static QTestState *start_with_args(const char *args)
{
    return start_with_audio(args, "none");
}

static QTestState *start(void)
{
    return start_with_args("");
}

static void test_vic(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    /*
     * Repeated spurious acknowledgements must not corrupt the priority stack.
     */
    for (int i = 0; i < 40; i++) {
        g_assert_cmphex(qtest_readl(q, VIC0 + 0xf00), ==, 0);
    }
    qtest_writel(q, VIC0 + 0x10, 3);
    qtest_writel(q, VIC0 + 0x100, 0x11110000);
    qtest_writel(q, VIC0 + 0x104, 0x22220000);
    qtest_writel(q, VIC0 + 0x200, 5);
    qtest_writel(q, VIC0 + 0x204, 3);
    qtest_writel(q, VIC0 + 0x18, 1);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, VIC0 + 0xf00), ==, 0x11110000);
    /* Active priority masks its own still-pending level. */
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, VIC0 + 0x18, 2);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, VIC0 + 0xf00), ==, 0x22220000);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, VIC0 + 0x1c, 2);
    qtest_writel(q, VIC0 + 0xf00, 0);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, VIC0 + 0x1c, 1);
    qtest_writel(q, VIC0 + 0xf00, 0);
    qtest_writel(q, VIC0 + 0x18, 1);
    g_assert_true(qtest_get_irq(q, 0));
    qtest_quit(q);
}

static void test_vic_cascade(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    qtest_writel(q, VIC1 + 0x10, 1);
    qtest_writel(q, VIC1 + 0x100, 0x33330000);
    qtest_writel(q, VIC1 + 0x18, 1);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, VIC0 + 0xf00), ==, 0x33330000);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, VIC1 + 0x1c, 1);
    qtest_writel(q, VIC0 + 0xf00, 0);
    qtest_writel(q, VIC1 + 0x18, 1);
    g_assert_true(qtest_get_irq(q, 0));
    /* Clearing a cascaded FIQ must preserve a local FIQ. */
    qtest_writel(q, VIC0 + 0x10, 2);
    qtest_writel(q, VIC0 + 0x0c, 2);
    qtest_writel(q, VIC0 + 0x18, 2);
    qtest_writel(q, VIC1 + 0x0c, 1);
    g_assert_true(qtest_get_irq(q, 1));
    qtest_writel(q, VIC1 + 0x1c, 1);
    g_assert_true(qtest_get_irq(q, 1));
    qtest_writel(q, VIC0 + 0x1c, 2);
    g_assert_false(qtest_get_irq(q, 1));
    qtest_quit(q);
}

static void test_i2s_stop(void)
{
    QTestState *q = start();

    /* retailOS 0x2200881c enables the gate before the interface clock. */
    qtest_writel(q, CLK + 0x4c, qtest_readl(q, CLK + 0x4c) & ~0x80);
    qtest_writel(q, I2S0, 1);
    g_assert_cmphex(qtest_readl(q, I2S0) & 3, ==, 1);
    qtest_writel(q, I2S0 + 4, 0x0b100019);
    qtest_writel(q, I2S0 + 0x30, 0x1000);
    qtest_writel(q, I2S0 + 8, 6);

    /* With no pending samples, the stop acknowledgement must arrive. */
    qtest_writel(q, I2S0 + 8, 0);
    qtest_writel(q, I2S0 + 0x34, 0);
    qtest_writel(q, I2S0, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, I2S0) & 3, ==, 2);
    qtest_writel(q, CLK + 0x4c, qtest_readl(q, CLK + 0x4c) | 0x80);

    /* A subsequent enable must not retain the stop acknowledgement. */
    qtest_writel(q, CLK + 0x4c, qtest_readl(q, CLK + 0x4c) & ~0x80);
    qtest_writel(q, I2S0, 1);
    g_assert_cmphex(qtest_readl(q, I2S0) & 3, ==, 1);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, I2S0) & 3, ==, 2);
    qtest_quit(q);
}

static void test_i2c(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    qtest_writel(q, I2C0, 0x184);
    qtest_writel(q, I2C0 + 0x0c, 0xe6);
    qtest_writel(q, I2C0 + 4, 0xf0);
    g_assert_false(qtest_get_irq(q, 21));
    qtest_clock_step(q, 25000);
    g_assert_true(qtest_get_irq(q, 21));
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x20) & 0x1100, ==, 0x1100);
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    g_assert_false(qtest_get_irq(q, 21));
    /* W1C must not advance the bus or consume the byte-done handshake. */
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0x10);
    qtest_writel(q, I2C0 + 0x0c, 0x87);
    qtest_writel(q, I2C0, 0x194);
    qtest_clock_step(q, 25000);
    qtest_writel(q, I2C0 + 4, 0xd0);
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    qtest_writel(q, I2C0 + 0x0c, 0xe7);
    qtest_writel(q, I2C0 + 4, 0xb0);
    qtest_clock_step(q, 25000);
    /* START transfers the address, not the first register value. */
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==, 0xe7);
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    qtest_writel(q, I2C0, 0x114);
    qtest_clock_step(q, 25000);
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==, 2);
    qtest_writel(q, I2C0 + 4, 0x90);
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    g_assert_false(qtest_get_irq(q, 21));
    qtest_quit(q);
}

static void i2c_start(QTestState *q, uint8_t address, bool receive)
{
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    qtest_writel(q, I2C0, 0x184);
    qtest_writel(q, I2C0 + 0x0c, (address << 1) | receive);
    qtest_writel(q, I2C0 + 4, receive ? 0xb0 : 0xf0);
    qtest_clock_step(q, 25000);
}

static void i2c_put(QTestState *q, uint8_t value)
{
    qtest_writel(q, I2C0 + 0x0c, value);
    qtest_writel(q, I2C0, 0x194);
    qtest_clock_step(q, 25000);
}

static void i2c_write_register(QTestState *q, uint8_t address,
                               uint8_t reg, uint8_t value)
{
    i2c_start(q, address, false);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 0);
    i2c_put(q, reg);
    i2c_put(q, value);
    qtest_writel(q, I2C0 + 4, 0xd0);
}

static uint8_t i2c_read_register(QTestState *q, uint8_t address, uint8_t reg)
{
    uint8_t value;

    i2c_start(q, address, false);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 0);
    i2c_put(q, reg);
    /* Repeated START must preserve the register pointer. */
    i2c_start(q, address, true);
    qtest_writel(q, I2C0, 0x114);
    qtest_clock_step(q, 25000);
    value = qtest_readl(q, I2C0 + 0x0c);
    qtest_writel(q, I2C0 + 4, 0x90);
    return value;
}

static void test_codec_control(void)
{
    QTestState *q = start();
    uint8_t value, revision;

    /* retailOS 0x0809f378 must preserve PDN until its explicit power-up. */
    value = i2c_read_register(q, 0x4a, 2);
    g_assert_cmphex(value, ==, 0x0f);
    i2c_write_register(q, 0x4a, 2, value & ~0x0e);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 2), ==, 1);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 3), ==, 0xff);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 5), ==, 0x0b);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 7), ==, 0x0c);

    /* DS773F1: MAP.INCR controls both block reads and block writes. */
    for (unsigned incr = 0; incr < 2; incr++) {
        i2c_write_register(q, 0x4a, 0x19, 0);
        i2c_start(q, 0x4a, false);
        i2c_put(q, 0x18 | (incr << 7));
        i2c_put(q, 0x5a);
        i2c_put(q, 0xa5);
        qtest_writel(q, I2C0 + 4, 0xd0);
        g_assert_cmphex(i2c_read_register(q, 0x4a, 0x18), ==,
                        incr ? 0x5a : 0xa5);
        g_assert_cmphex(i2c_read_register(q, 0x4a, 0x19), ==,
                        incr ? 0xa5 : 0);

        i2c_start(q, 0x4a, false);
        i2c_put(q, 0x18 | (incr << 7));
        /* The selected MAP also survives a STOP before the read. */
        qtest_writel(q, I2C0 + 4, 0xd0);
        i2c_start(q, 0x4a, true);
        for (unsigned i = 0; i < 2; i++) {
            qtest_writel(q, I2C0, i ? 0x114 : 0x194);
            qtest_clock_step(q, 25000);
            g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==,
                            incr && !i ? 0x5a : 0xa5);
        }
        qtest_writel(q, I2C0 + 4, 0x90);
    }
    revision = i2c_read_register(q, 0x4a, 1);
    i2c_write_register(q, 0x4a, 1, ~revision);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 1), ==, revision);
    i2c_write_register(q, 0x4a, 0x29, 0xff);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x29), ==, 0);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(i2c_read_register(q, 0x4a, 2), ==, 0x0f);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x18), ==, 0);
    qtest_quit(q);
}

static uint64_t codec_lrck(QTestState *q)
{
    QDict *response = qtest_qmp(q, "{'execute':'qom-get', 'arguments':{"
                                  "'path':'/machine/codec',"
                                  "'property':'lrck-frequency'}}");
    uint64_t rate;

    g_assert_true(qdict_haskey(response, "return"));
    rate = qdict_get_int(response, "return");
    qobject_unref(response);
    return rate;
}

static void test_codec_clocks(void)
{
    /* Original 0x080a7838 selections, DS773F1 table 1 clock ratios. */
    static const struct {
        uint8_t reg5;
        unsigned mclk_divisor;
    } rates[] = {
        { 0x1d, 1500 }, { 0x1b, 1088 }, { 0x19, 1000 },
        { 0x15, 750 }, { 0x13, 544 }, { 0x11, 500 },
        { 0x0d, 375 }, { 0x0b, 272 }, { 0x09, 250 },
    };
    QTestState *q = start();

    g_assert_cmpuint(codec_lrck(q), ==, 0); /* Slave mode at reset. */
    i2c_write_register(q, 0x4a, 4, 0x2e); /* Original master/div2 setup. */
    i2c_write_register(q, 0x4a, 2, 0x0e);
    for (unsigned i = 0; i < ARRAY_SIZE(rates); i++) {
        i2c_write_register(q, 0x4a, 5, rates[i].reg5);
        g_assert_cmpuint(codec_lrck(q), ==, 12000000 / rates[i].mclk_divisor);
    }
    /* Original 32 kHz path selects PLL2 / 27 and register 5 = 9. */
    qtest_writel(q, CLK + 0x28, 0x01002401);
    qtest_writel(q, CLK + 0x44, 0x40034);
    qtest_writel(q, CLK + 0x0c, 0x3028);
    g_assert_cmpuint(codec_lrck(q), ==, 32000);
    qtest_writel(q, CLK + 0x0c, 0xb028);
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    qtest_writel(q, CLK + 0x0c, 0x80000000);
    g_assert_cmpuint(codec_lrck(q), ==, 48000); /* Upper gate is separate. */

    i2c_write_register(q, 0x4a, 4, 0x0e); /* Codec becomes a clock slave. */
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    i2c_write_register(q, 0x4a, 2, 0x0f);
    i2c_write_register(q, 0x4a, 4, 0x2f); /* Explicit MCLK disable. */
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    i2c_write_register(q, 0x4a, 4, 0x2c); /* Master without MCLK / 2. */
    qtest_writel(q, CLK + 0x0c, 1); /* Feed 6 MHz. */
    g_assert_cmpuint(codec_lrck(q), ==, 48000);
    i2c_write_register(q, 0x4a, 5, 0); /* Reserved speed/ratio. */
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    qtest_quit(q);
}

static void test_codec_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    i2c_write_register(q, 0x4a, 4, 0x2e);
    g_assert_cmpuint(codec_lrck(q), ==, 44117);
    i2c_write_register(q, 0x4a, 7, 0x0d); /* Freeze active controls. */
    i2c_write_register(q, 0x4a, 5, 9);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 5), ==, 9);
    g_assert_cmpuint(codec_lrck(q), ==, 44117);
    result = qtest_hmp(q, "savevm codec-test");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    i2c_write_register(q, 0x4a, 7, 0x0c);
    g_assert_cmpuint(codec_lrck(q), ==, 48000);
    qtest_writel(q, CLK + 0x0c, 0x8000);
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    result = qtest_hmp(q, "loadvm codec-test");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmpuint(codec_lrck(q), ==, 44117);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 5), ==, 9);

    /* FREEZE holds controls; the external MCLK can still change or stop. */
    qtest_writel(q, CLK + 0x0c, 1);
    g_assert_cmpuint(codec_lrck(q), ==, 22058);
    qtest_writel(q, CLK + 0x0c, 0x8001);
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    qtest_writel(q, CLK + 0x0c, 1);
    g_assert_cmpuint(codec_lrck(q), ==, 22058);
    i2c_write_register(q, 0x4a, 7, 0x0c);
    g_assert_cmpuint(codec_lrck(q), ==, 24000);
    i2c_write_register(q, 0x4a, 7, 0x0d);
    i2c_write_register(q, 0x4a, 4, 0x0e);
    g_assert_cmpuint(codec_lrck(q), ==, 24000);
    i2c_write_register(q, 0x4a, 7, 0x0c);
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmpuint(codec_lrck(q), ==, 0);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 4), ==, 0);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 5), ==, 0x0b);
    qtest_quit(q);
}

static void test_i2c_stop(void)
{
    QTestState *q = start();

    /* STOP must release the slave so the next address selects a new one. */
    i2c_write_register(q, 0x73, 0x67, 0xa5);
    i2c_write_register(q, 0x4a, 0x67, 0x5a);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0xa5);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x67), ==, 0x5a);

    /* An absent slave after STOP must NACK, not use the previous slave. */
    i2c_start(q, 0x7e, false);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 1);
    qtest_writel(q, I2C0 + 4, 0xd0);

    /* Disabling the interface also releases an addressed slave. */
    i2c_start(q, 0x73, false);
    qtest_writel(q, I2C0 + 4, 0);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x67), ==, 0x5a);

    /* Aborting an address must cancel its pending byte completion. */
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    qtest_writel(q, I2C0 + 0x0c, 0xe6);
    qtest_writel(q, I2C0 + 4, 0xf0);
    qtest_writel(q, I2C0 + 4, 0);
    qtest_clock_step(q, 25000);
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x20), ==, 0);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
    qtest_quit(q);
}

static void test_i2c_completion(void)
{
    QTestState *q = start();

    /* Do not round the start down to a microsecond. */
    qtest_clock_step(q, 123);
    /* An address cannot produce a NACK before its transfer completes. */
    qtest_writel(q, I2C0, 0x184);
    qtest_writel(q, I2C0 + 0x0c, 0xfc);
    qtest_writel(q, I2C0 + 4, 0xf0);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 0);
    qtest_clock_step(q, 24999);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, I2C0 + 4) & 1, ==, 1);
    qtest_writel(q, I2C0 + 4, 0xd0);

    /* Reading the PMU event latch must wait for the receive operation. */
    qtest_irq_intercept_out(q, "/machine/pmu");
    i2c_write_register(q, 0x73, 0x86, 0);
    qtest_qmp_assert_success(q, "{'execute':'qom-set', 'arguments':{"
                            "'path':'/machine', 'property':'hold',"
                            "'value':true}}");
    g_assert_false(qtest_get_irq(q, 0)); /* PMU IRQ is active-low. */
    i2c_start(q, 0x73, false);
    i2c_put(q, 0x85);
    i2c_start(q, 0x73, true);
    qtest_writel(q, I2C0, 0x114);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==, 0xe7);
    qtest_clock_step(q, 24000);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==, 0xe7);
    qtest_clock_step(q, 1000);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, I2C0 + 0x0c), ==, 2);
    qtest_writel(q, I2C0 + 4, 0x90);
    qtest_quit(q);
}

static void test_i2c_cancel(void)
{
    QTestState *q = start();

    i2c_write_register(q, 0x73, 0x67, 0xa5);
    for (unsigned i = 0; i < 3; i++) {
        i2c_start(q, 0x73, false);
        i2c_put(q, 0x67);
        qtest_writel(q, I2C0 + 0x20, 0x3f00);
        qtest_writel(q, I2C0 + 0x0c, 0x5a);
        qtest_writel(q, I2C0, 0x194);
        /* Cancel before any transfer time: STOP, disable, then reset. */
        if (i == 2) {
            qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
        } else {
            qtest_writel(q, I2C0 + 4, i ? 0 : 0xd0);
        }
        qtest_clock_step(q, 25000);
        g_assert_cmphex(qtest_readl(q, I2C0 + 0x20) & 0x1100, ==, 0);
        g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
        g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0xa5);
    }
    qtest_quit(q);
}

static void test_i2c_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    /* Save a transmit byte in flight, then cancel it without advancing time. */
    i2c_start(q, 0x73, false);
    i2c_put(q, 0x67);
    qtest_writel(q, I2C0 + 0x20, 0x3f00);
    qtest_writel(q, I2C0 + 0x0c, 0xa5);
    qtest_writel(q, I2C0, 0x194);
    qtest_clock_step(q, 10000);
    result = qtest_hmp(q, "savevm i2c-test");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_writel(q, I2C0 + 4, 0xd0);
    qtest_writel(q, I2C0 + 0x0c, 0x5a);
    result = qtest_hmp(q, "loadvm i2c-test");
    g_assert_cmpstr(result, ==, "");
    qtest_clock_step(q, 14000);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
    qtest_clock_step(q, 1000);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0x10);
    qtest_writel(q, I2C0 + 4, 0xd0);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0xa5);
    qtest_quit(q);
}

static void test_i2c_gates(void)
{
    static const struct {
        uint32_t base;
        unsigned gate;
        unsigned irq;
    } ports[] = { { I2C0, 4, 21 }, { I2C1, 6, 22 } };
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    for (unsigned i = 0; i < G_N_ELEMENTS(ports); i++) {
        uint32_t base = ports[i].base;
        uint32_t gate = 1u << ports[i].gate;

        qtest_writel(q, CLK + 0x4c, gate);
        qtest_writel(q, base, 0x184);
        qtest_writel(q, base + 0x0c, 0xfc); /* Absent address, both ports. */
        qtest_writel(q, base + 4, 0xf0);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, base) & 0x10, ==, 0);
        g_assert_cmphex(qtest_readl(q, base + 4) & 1, ==, 0);
        g_assert_false(qtest_get_irq(q, ports[i].irq));

        /* The other controller's gate must not suspend this one. */
        qtest_writel(q, CLK + 0x4c, 1u << ports[!i].gate);
        qtest_clock_step(q, 10001);
        qtest_writel(q, CLK + 0x4c, (1u << 4) | (1u << 6));
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, base) & 0x10, ==, 0);
        g_assert_false(qtest_get_irq(q, ports[i].irq));
        qtest_writel(q, CLK + 0x4c, 0);
        qtest_clock_step(q, 14998);
        g_assert_false(qtest_get_irq(q, ports[i].irq));
        qtest_clock_step(q, 1);
        g_assert_true(qtest_get_irq(q, ports[i].irq));
        g_assert_cmphex(qtest_readl(q, base + 4) & 1, ==, 1);

        /* Gating completed work must leave its pending interrupt intact. */
        qtest_writel(q, CLK + 0x4c, gate);
        g_assert_true(qtest_get_irq(q, ports[i].irq));
        qtest_writel(q, base + 4, 0xd0);
        qtest_writel(q, base + 0x20, 0x3f00);
        g_assert_false(qtest_get_irq(q, ports[i].irq));
    }
    qtest_quit(q);
}

static void test_i2c_gated_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    i2c_write_register(q, 0x73, 0x67, 0xa5);
    i2c_start(q, 0x73, false);
    i2c_put(q, 0x67);
    qtest_writel(q, I2C0 + 0x0c, 0x5a);
    qtest_writel(q, I2C0, 0x194);
    qtest_clock_step(q, 10001);
    qtest_writel(q, CLK + 0x4c, 1u << 4);
    result = qtest_hmp(q, "savevm i2c-gated");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_writel(q, I2C0 + 4, 0xd0);
    qtest_clock_step(q, 1000000);
    result = qtest_hmp(q, "loadvm i2c-gated");
    g_assert_cmpstr(result, ==, "");
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
    qtest_writel(q, CLK + 0x4c, 0);
    qtest_clock_step(q, 14998);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0x10);
    qtest_writel(q, I2C0 + 4, 0xd0);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0x5a);

    /* STOP, interface disable and reset cancel a suspended data write. */
    for (unsigned i = 0; i < 3; i++) {
        i2c_start(q, 0x73, false);
        i2c_put(q, 0x67);
        qtest_writel(q, I2C0 + 0x0c, 0xa5);
        qtest_writel(q, I2C0, 0x194);
        qtest_writel(q, CLK + 0x4c, 1u << 4);
        if (i == 2) {
            qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
        } else {
            qtest_writel(q, I2C0 + 4, i ? 0 : 0xd0);
        }
        qtest_writel(q, CLK + 0x4c, 0);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, I2C0) & 0x10, ==, 0);
        g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0x5a);
    }
    qtest_quit(q);
}

static void test_spi_fifo(void)
{
    QTestState *q = start();

    qtest_writel(q, SPI1 + 4, 0x18);
    qtest_writel(q, SPI1 + 0x34, 32);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writel(q, SPI1 + 0x10, i);
    }
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 16 << 4);
    /* A full FIFO must reject another write without corrupting its contents. */
    qtest_writel(q, SPI1 + 0x10, 0xff);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 16 << 4);
    qtest_writel(q, SPI1, 1);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 16 << 9);
    qtest_writel(q, SPI1 + 0x10, 0x80);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, (16 << 9) | (1 << 4));
    qtest_readl(q, SPI1 + 0x20);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 16 << 9);
    qtest_writel(q, SPI1, 0);
    qtest_writel(q, SPI1, 8);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 0);
    g_assert_cmphex(qtest_readl(q, SPI1), ==, 2);
    qtest_writel(q, SPI1 + 0x10, 0x55);
    qtest_writel(q, SPI1, 4);
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 0);
    qtest_writel(q, SPI1 + 0x30, 0x7ff);
    g_assert_cmphex(qtest_readl(q, SPI1 + 0x30), ==, 0x7ff);
    qtest_quit(q);
}

static void test_spi_controls(void)
{
    static const uint32_t ports[] = { SPI0, SPI1, 0x3d200000 };
    QTestState *q = start();

    for (unsigned i = 0; i < ARRAY_SIZE(ports); i++) {
        uint32_t port = ports[i];

        qtest_writel(q, port, 1);
        qtest_writel(q, port + 0x34, 4);
        qtest_writel(q, port + 0x10, 0x55);
        qtest_writel(q, port + 0x10, 0xaa);
        /* Neither control alone may initiate a master transfer. */
        qtest_writel(q, port + 4, 8);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 2 << 4);
        qtest_writel(q, port + 4, 0x10);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 2 << 4);
        qtest_writel(q, port + 4, 0x18);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 2 << 9);
        qtest_readl(q, port + 0x20);
        qtest_readl(q, port + 0x20);

        /* Keep the automatic receive budget until both controls are set. */
        qtest_writel(q, port + 4, 9);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 0);
        qtest_writel(q, port + 4, 0x11);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 0);
        qtest_writel(q, port + 4, 0x19);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 2 << 9);
        qtest_writel(q, port, 0);
        qtest_writel(q, port + 0x34, 1);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 2 << 9);
        qtest_writel(q, port, 1);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 3 << 9);
    }
    qtest_quit(q);
}

static void test_spi_nor(void)
{
    const uint8_t command[] = { 3, 0, 1, 0 };
    uint8_t pattern[64];
    QTestState *q;
    int fd;

    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = i ^ 0xa5;
    }
    fd = open(nor, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, pattern, sizeof(pattern), 0x100), ==,
                    sizeof(pattern));
    close(fd);
    q = start();
    qtest_writel(q, 0x3cf00200, 0xf); /* GPIO 0: deassert NOR chip select. */
    qtest_writel(q, 0x3cf00200, 0xe);
    qtest_writel(q, SPI0 + 4, 0x10618);
    qtest_writel(q, SPI0 + 0x34, 37); /* Command/address plus 33 data bytes. */
    qtest_writel(q, SPI0, 1);
    for (unsigned i = 0; i < sizeof(command); i++) {
        qtest_writel(q, SPI0 + 0x10, command[i]);
        g_assert_cmphex(qtest_readl(q, SPI0 + 8) & 0x3e00, ==, 1 << 9);
        qtest_readl(q, SPI0 + 0x20);
    }
    qtest_writel(q, SPI0 + 4, 0x10619);
    g_assert_cmphex(qtest_readl(q, SPI0 + 8) & 0x3e00, ==, 16 << 9);
    for (unsigned i = 0; i < 33; i++) {
        if (i == 10) {
            g_autofree char *saved = NULL;
            g_autofree char *loaded = NULL;

            /* Suspend automatic reception while preserving NOR/FIFO state. */
            qtest_writel(q, SPI0 + 4, 0x10609);
            saved = qtest_hmp(q, "savevm spi-nor-test");
            g_assert_cmpstr(saved, ==, "");
            for (unsigned j = 0; j < 3; j++) {
                g_assert_cmphex(qtest_readl(q, SPI0 + 0x20), ==,
                                pattern[i + j]);
            }
            qtest_clock_step(q, 1000000);
            g_assert_cmphex(qtest_readl(q, SPI0 + 8), ==, 13 << 9);
            loaded = qtest_hmp(q, "loadvm spi-nor-test");
            g_assert_cmpstr(loaded, ==, "");
            g_assert_cmphex(qtest_readl(q, SPI0 + 4), ==, 0x10609);
            g_assert_cmphex(qtest_readl(q, SPI0 + 8), ==, 16 << 9);
            qtest_writel(q, SPI0 + 4, 0x10619);
        }
        g_assert_cmphex(qtest_readl(q, SPI0 + 0x20), ==, pattern[i]);
    }
    g_assert_cmphex(qtest_readl(q, SPI0 + 8) & 0x3e00, ==, 0);
    /* Empty reads cannot generate additional clocks or consume NOR data. */
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(q, SPI0 + 0x20), ==, 0);
    }
    qtest_writel(q, SPI0 + 0x34, 1);
    g_assert_cmphex(qtest_readl(q, SPI0 + 0x20), ==, pattern[33]);
    qtest_writel(q, SPI0 + 4, 0x10618);
    qtest_writel(q, 0x3cf00200, 0xf);
    qtest_quit(q);
}

static void test_spi_gate(void)
{
    static const uint32_t ports[] = { SPI0, SPI1, 0x3d200000 };
    static const uint32_t gates[] = { 1 << 2, 1 << 11, 1 << 15 };
    QTestState *q = start();

    for (unsigned i = 0; i < ARRAY_SIZE(ports); i++) {
        uint32_t port = ports[i];

        qtest_writel(q, CLK + 0x4c, gates[i]);
        qtest_writel(q, port + 4, 0x18);
        qtest_writel(q, port + 0x34, 1);
        qtest_writel(q, port, 1);
        qtest_writel(q, port + 0x10, 0xff);
        qtest_clock_step(q, 1000000);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 1 << 4);
        qtest_writel(q, CLK + 0x4c, 0);
        g_assert_cmphex(qtest_readl(q, port + 8), ==, 1 << 9);
        qtest_readl(q, port + 0x20);
    }
    qtest_quit(q);
}

static void test_spi_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    qtest_writel(q, SPI1 + 4, 0x18);
    qtest_writel(q, SPI1 + 0x34, 32);
    qtest_writel(q, SPI1, 1);
    for (unsigned i = 0; i < 19; i++) {
        qtest_writel(q, SPI1 + 0x10, i);
    }
    qtest_writel(q, CLK + 0x4c, 1 << 11);
    result = qtest_hmp(q, "savevm spi-test");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 0);
    result = qtest_hmp(q, "loadvm spi-test");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, (16 << 9) | (3 << 4));
    qtest_writel(q, CLK + 0x4c, 0);
    for (unsigned i = 0; i < 19; i++) {
        g_assert_cmphex(qtest_readl(q, SPI1 + 8) & 0x3e00, !=, 0);
        qtest_readl(q, SPI1 + 0x20);
    }
    g_assert_cmphex(qtest_readl(q, SPI1 + 8), ==, 0);
    qtest_quit(q);
}

static void test_timers(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    /* Timer B: 12 MHz / 2, interval reload at 6000 => 1 ms. */
    qtest_writel(q, TIMER + 0x28, 6000);
    qtest_writel(q, TIMER + 0x20, 0x1040);
    qtest_writel(q, TIMER + 0x24, 3);
    qtest_clock_step(q, 999999);
    g_assert_false(qtest_get_irq(q, 8));
    qtest_clock_step(q, 1);
    g_assert_true(qtest_get_irq(q, 8));
    g_assert_cmpuint(qtest_readl(q, TIMER + 0x34), ==, 0);
    qtest_writel(q, TIMER + 0x20, 0x11040);
    g_assert_false(qtest_get_irq(q, 8));
    qtest_clock_step(q, 1000000);
    g_assert_true(qtest_get_irq(q, 8));
    /* Timer E's compare flag belongs to TSTAT bit 24. */
    qtest_writel(q, TIMER + 0xa8, 12000);
    qtest_writel(q, TIMER + 0xa0, 0x1440);
    qtest_writel(q, TIMER + 0xa4, 3);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, TIMER + 0x118), ==, 0x01000000);
    g_assert_true(qtest_get_irq(q, 7));
    qtest_writel(q, TIMER + 0x118, 0x01000000);
    g_assert_false(qtest_get_irq(q, 7));
    /* Unknown offsets must never index outside the timer array. */
    qtest_writel(q, TIMER + 0xfffc, 0xffffffff);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xfffc), ==, 0);
    qtest_quit(q);
}

static void clock_test_timer(QTestState *q, uint32_t control,
                             uint32_t prescaler)
{
    qtest_writel(q, TIMER + 0xa4, 0);
    qtest_writel(q, TIMER + 0xa8, UINT32_MAX);
    qtest_writel(q, TIMER + 0xac, UINT32_MAX);
    qtest_writel(q, TIMER + 0xb0, prescaler);
    qtest_writel(q, TIMER + 0xa0, control);
    qtest_writel(q, TIMER + 0xa4, 3);
}

static void test_clocks(void)
{
    QTestState *q = start();
    uint32_t count;

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    g_assert_cmphex(qtest_readl(q, CLK + 0x40), ==, 0);
    qtest_writel(q, CLK + 0x40, UINT32_MAX);
    g_assert_cmphex(qtest_readl(q, CLK + 0x40), ==, 0);

    clock_test_timer(q, 0x1420, 0); /* Timer E, PCLK, one-shot. */
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 12000);
    qtest_writel(q, CLK + 4, 0x4100); /* PCLK / 4. */
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 15000);

    /* Firmware's 216 MHz PLL2 source and its active/idle bus dividers. */
    qtest_writel(q, CLK + 0x28, 0x01002401);
    qtest_writel(q, CLK + 0x44, 0x40034);
    g_assert_cmphex(qtest_readl(q, CLK + 0x40), ==, 0x44);
    qtest_writel(q, CLK, 0x3000);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 69000);
    qtest_writel(q, CLK + 4, 0x454b4b00);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 78000);

    /* Gating freezes both counting and the pending compare deadline. */
    qtest_writel(q, TIMER + 0xa8, 87000);
    qtest_clock_step(q, 500000);
    count = qtest_readl(q, TIMER + 0xb4);
    g_assert_cmpuint(count, ==, 82500);
    qtest_writel(q, CLK + 0x4c, 0x20);
    qtest_clock_step(q, 100000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, count);
    g_assert_false(qtest_get_irq(q, 7));
    qtest_writel(q, CLK + 0x4c, 0);
    qtest_clock_step(q, 499999);
    g_assert_false(qtest_get_irq(q, 7));
    qtest_clock_step(q, 1);
    g_assert_true(qtest_get_irq(q, 7));
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 87000);
    qtest_writel(q, TIMER + 0x118, 0x01000000);

    /* A clock change must preserve the fractional tick already elapsed. */
    qtest_writel(q, CLK, 0);
    qtest_writel(q, CLK + 4, 0x4100);
    clock_test_timer(q, 0x420, 2); /* 12 MHz / 4 / 3 = 1 MHz. */
    qtest_clock_step(q, 500);
    qtest_writel(q, CLK + 4, 0x4300); /* Now 500 kHz. */
    qtest_clock_step(q, 999);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 1);

    /* ECLK has its own source and two cascaded divider fields. */
    qtest_writel(q, CLK + 0x10, 0x30110000);
    clock_test_timer(q, 0x460, 0); /* PLL2 / 2 / 2 = 54 MHz. */
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 54000);
    qtest_writel(q, CLK + 0x10, 0xb0110000);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 54000);

    /* PLL0 multiply mode: 32768 Hz * 2 * 3 / 2 = 98304 Hz. */
    qtest_writel(q, CLK + 0x20, 0x02000301);
    qtest_writel(q, CLK + 0x44, 0x10001);
    qtest_writel(q, CLK, 0x1000);
    qtest_writel(q, CLK + 4, 0);
    g_assert_cmphex(qtest_readl(q, CLK + 0x40), ==, 1);
    clock_test_timer(q, 0x420, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 98);
    qtest_writel(q, CLK + 0x44, 1); /* PLL output selects the slow clock. */
    clock_test_timer(q, 0x420, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 32);

    /* Reserved offsets do not alias the implemented clock registers. */
    qtest_writel(q, CLK + 0x10000, UINT32_MAX);
    g_assert_cmphex(qtest_readl(q, CLK + 0x10000), ==, 0);
    qtest_quit(q);
}

static void test_clock_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    /* Snapshot at half a tick, then change the clock without advancing time. */
    clock_test_timer(q, 0x420, 11);
    qtest_clock_step(q, 500);
    result = qtest_hmp(q, "savevm clock-test");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_writel(q, CLK + 4, 0x4100);
    qtest_writel(q, CLK + 0x4c, 0x20);
    qtest_writel(q, TIMER + 0xa4, 2);
    result = qtest_hmp(q, "loadvm clock-test");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmphex(qtest_readl(q, CLK + 4), ==, 0);
    g_assert_cmphex(qtest_readl(q, CLK + 0x4c), ==, 0);
    qtest_clock_step(q, 500);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 1);
    /* Derived output clocks must also be ready for subsequent propagation. */
    qtest_writel(q, CLK + 4, 0x4100);
    qtest_clock_step(q, 4000);
    g_assert_cmpuint(qtest_readl(q, TIMER + 0xb4), ==, 2);
    qtest_quit(q);
}

static void test_wheel(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    qtest_writel(q, WHEEL + 0x1c, 0xc000011d);
    qtest_writel(q, WHEEL + 4, 1);
    g_assert_false(qtest_get_irq(q, 23));
    qtest_writel(q, WHEEL + 0x10, 1);
    g_assert_true(qtest_get_irq(q, 23));
    /* Clearing status cannot hide a receive word still in the FIFO. */
    qtest_writel(q, WHEEL + 0x14, 7);
    g_assert_true(qtest_get_irq(q, 23));
    g_assert_cmphex(qtest_readl(q, WHEEL + 0x18), ==, 0x8000023a);
    g_assert_false(qtest_get_irq(q, 23));
    g_assert_cmpuint(qtest_readl(q, WHEEL + 4), ==, 0);
    qtest_qmp_assert_success(q,
        "{'execute':'input-send-event', 'arguments': {'events': ["
        "{'type':'key','data':{'down':true,"
        "'key':{'type':'qcode','data':'right'}}}]}}");
    qtest_clock_step(q, 40000000);
    g_assert_cmphex(qtest_readl(q, WHEEL + 0x18), ==, 0x8000021a);
    qtest_writel(q, WHEEL + 0x1c, 0xc000011d);
    qtest_writel(q, WHEEL + 4, 1);
    /* Query replies and unsolicited event packets have different ordering. */
    g_assert_cmphex(qtest_readl(q, WHEEL + 0x18), ==, 0x8010023a);
    qtest_quit(q);
}

static void test_dma_gpio(void)
{
    QTestState *q = start();
    const uint8_t pattern[16] = "bounded AES DMA";
    uint8_t result[16];

    qtest_memwrite(q, 0x08000000, pattern, sizeof(pattern));
    qtest_writel(q, AES + 0x6c, 1);
    qtest_writel(q, AES + 0x28, 0x08000000);
    qtest_writel(q, AES + 0x20, 0x08001000);
    qtest_writel(q, AES + 0x24, sizeof(pattern));
    qtest_writel(q, AES + 4, 1);
    qtest_memread(q, 0x08001000, result, sizeof(result));
    g_assert_cmpmem(pattern, sizeof(pattern), result, sizeof(result));
    /* A malformed descriptor must not allocate gigabytes or re-enter MMIO. */
    qtest_writel(q, AES + 0x28, 7);
    qtest_writel(q, AES + 0x24, 0x38e00000);
    qtest_writel(q, AES + 4, 1);
    qtest_writel(q, 0x3cf00140, 0x12345678);
    g_assert_cmphex(qtest_readl(q, 0x3cf00140), ==, 0x12345678);
    qtest_writel(q, 0x3cf00200, 0xffffff);
    qtest_quit(q);
}

static void pl080_transfer(QTestState *q, uint32_t src, uint32_t dst,
                           uint32_t lli, uint32_t ctrl, uint32_t conf)
{
    qtest_writel(q, DMA0 + 0x110, 0);
    qtest_writel(q, DMA0 + 0x100, src);
    qtest_writel(q, DMA0 + 0x104, dst);
    qtest_writel(q, DMA0 + 0x108, lli);
    qtest_writel(q, DMA0 + 0x10c, ctrl);
    qtest_writel(q, DMA0 + 0x30, 1);
    qtest_writel(q, DMA0 + 0x110, conf);
}

static void test_pl080_widths(void)
{
    QTestState *q = start();
    const uint8_t pattern[16] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
        0x13, 0x57, 0x9b, 0xdf, 0x24, 0x68, 0xac, 0xe0,
    };
    uint8_t expected[32], result[32];

    qtest_memwrite(q, 0x08000000, pattern, sizeof(pattern));
    for (unsigned sw = 0; sw < 3; sw++) {
        for (unsigned dw = 0; dw < 3; dw++) {
            for (unsigned increment = 0; increment < 4; increment++) {
                uint32_t ctrl = 0x80000000 | (sw << 18) | (dw << 21) |
                                (increment << 26) | (16 >> sw);
                uint8_t transferred[16];

                qtest_memset(q, 0x08001000, 0, sizeof(result));
                memset(expected, 0, sizeof(expected));
                for (unsigned i = 0; i < sizeof(transferred); i++) {
                    transferred[i] = pattern[increment & 1 ? i : i % (1 << sw)];
                }
                if (increment & 2) {
                    memcpy(expected, transferred, sizeof(transferred));
                } else {
                    memcpy(expected, transferred + 16 - (1 << dw), 1 << dw);
                }
                pl080_transfer(q, 0x08000000, 0x08001000, 0, ctrl, 0x8001);
                qtest_memread(q, 0x08001000, result, sizeof(result));
                g_assert_cmpmem(result, sizeof(result),
                                expected, sizeof(expected));
                g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==,
                                0x08000000 + (increment & 1 ? 16 : 0));
                g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==,
                                0x08001000 + (increment & 2 ? 16 : 0));
                g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 0);
                g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 1, ==, 0);
                g_assert_cmphex(qtest_readl(q, DMA0 + 4), ==, 1);
                qtest_writel(q, DMA0 + 8, 1);
            }
        }
    }
    qtest_quit(q);
}

static void test_pl080_lli(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    qtest_writel(q, 0x08000000, 0x12345678);
    qtest_writel(q, 0x08002000, 0x08000004);
    qtest_writel(q, 0x08002004, 0x08001004);
    qtest_writel(q, 0x08002008, 0);
    for (unsigned interrupt = 0; interrupt < 2; interrupt++) {
        /* A zero-count next LLI waits; it must not choose the earlier IRQ. */
        qtest_writel(q, 0x0800200c, interrupt ? 0 : 0x80000000);
        pl080_transfer(q, 0x08000000, 0x08001000, 0x08002000,
                       (interrupt << 31) | 0x480001, 0x8001);
        g_assert_cmphex(qtest_readl(q, 0x08001000), ==, 0x12345678);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, interrupt);
        g_assert_cmpint(qtest_get_irq(q, 16), ==, interrupt);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000004);
        qtest_writel(q, DMA0 + 8, 1);
    }
    /* Bit 0 selects an AHB master, not a byte offset in the descriptor. */
    qtest_writel(q, 0x0800200c, 0);
    pl080_transfer(q, 0x08000000, 0x08001000, 0x08002001, 0x480001, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000004);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001004);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x108), ==, 0);

    /* A nonempty final LLI completes and disables the channel normally. */
    qtest_writel(q, 0x08000004, 0x9abcdef0);
    qtest_writel(q, 0x0800200c, 0x88480001);
    pl080_transfer(q, 0x08000000, 0x08001000, 0x08002000, 0x480001, 0x8001);
    g_assert_cmphex(qtest_readl(q, 0x08001004), ==, 0x9abcdef0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 1, ==, 0);
    g_assert_true(qtest_get_irq(q, 16));
    qtest_quit(q);
}

static void test_pl080_errors(void)
{
    QTestState *q = start();

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    qtest_writel(q, 0x08000000, 0x12345678);
    for (unsigned failure = 0; failure < 3; failure++) {
        uint32_t src = failure == 0 ? 0xf0000000 : 0x08000000;
        uint32_t dst = failure == 1 ? 0xf0000000 : 0x08001000;
        uint32_t lli = failure == 2 ? 0xf0000000 : 0;

        pl080_transfer(q, src, dst, lli, 0x80480001, 0xc001);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 1);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 1, ==, 0);
        /* A failed next-LLI fetch follows a successfully completed transfer. */
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, failure == 2);
        g_assert_true(qtest_get_irq(q, 16));
        qtest_writel(q, DMA0 + 8, 1);
        qtest_writel(q, DMA0 + 0x110, 0); /* Mask without clearing raw error. */
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 1);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x0c), ==, 0);
        g_assert_false(qtest_get_irq(q, 16));
        qtest_writel(q, DMA0 + 0x110, 0x4000);
        g_assert_true(qtest_get_irq(q, 16));
        qtest_writel(q, DMA0 + 0x10, 1);
        g_assert_false(qtest_get_irq(q, 16));
    }
    qtest_quit(q);
}

static void test_pl080_invalid(void)
{
    QTestState *q = start();

    /* Unsupported peripheral flow control must not terminate the host. */
    for (unsigned flow = 4; flow < 8; flow++) {
        pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x480001,
                       1 | (flow << 11));
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 1);
    }
    qtest_writel(q, 0x08001000, 0x12345678);
    for (unsigned width = 3; width < 8; width++) {
        for (unsigned shift = 18; shift <= 21; shift += 3) {
            pl080_transfer(q, 0x08000000, 0x08001000, 0,
                           (width << shift) | 1, 1);
            g_assert_cmphex(qtest_readl(q, 0x08001000), ==, 0x12345678);
            g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 1);
        }
    }
    /* Three bytes cannot supply a complete word-wide destination transfer. */
    pl080_transfer(q, 0x08000000, 0x08001000, 0, (2 << 21) | 3, 1);
    g_assert_cmphex(qtest_readl(q, 0x08001000), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 3);
    qtest_quit(q);
}

static void test_pl080_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    qtest_irq_intercept_in(q, "/machine/soc/vic0");
    pl080_transfer(q, 0xf0000000, 0x08001000, 0, 0x480001, 0x4001);
    g_assert_true(qtest_get_irq(q, 16));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 4), ==, 0);
    result = qtest_hmp(q, "savevm dma-test");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_false(qtest_get_irq(q, 16));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 0);
    result = qtest_hmp(q, "loadvm dma-test");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 1);
    g_assert_true(qtest_get_irq(q, 16));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 4), ==, 0);
    qtest_writel(q, DMA0 + 0x10, 1);
    g_assert_false(qtest_get_irq(q, 16));
    qtest_quit(q);
}

static void pl080_request(QTestState *q, const char *name, unsigned id,
                           int level)
{
    qtest_set_irq_in(q, "/machine/soc/dma[0]", name, id, level);
}

static void test_pl080_requests(void)
{
    QTestState *q = start();
    uint8_t data[20], actual[20];
    const uint32_t conf = 0x8001 | (1 << 11) | (3 << 6);

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i + 1;
    }
    qtest_memwrite(q, 0x08000000, data, sizeof(data));
    pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x8c008014, conf);
    /* No source or destination transaction before the peripheral request. */
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 0x20001, ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 20);
    pl080_request(q, "dreq-single", 3, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001000);
    for (unsigned burst = 0; burst < 5; burst++) {
        pl080_request(q, "dreq-burst", 3, 1);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==,
                       0x08001004 + 4 * burst);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==,
                       16 - 4 * burst);
        /* A held request must not be consumed twice. */
        pl080_request(q, "dreq-burst", 3, 1);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==,
                       0x08001004 + 4 * burst);
        pl080_request(q, "dreq-burst", 3, 0);
        if (!burst) {
            /* CLR remains asserted until the other request also falls. */
            pl080_request(q, "dreq-burst", 3, 1);
            g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001004);
            pl080_request(q, "dreq-burst", 3, 0);
            pl080_request(q, "dreq-single", 3, 0);
        }
    }
    qtest_memread(q, 0x08001000, actual, sizeof(actual));
    g_assert_cmpmem(data, sizeof(data), actual, sizeof(actual));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 0x20001, ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, 1);
    /* A final short destination burst still uses BREQ. */
    qtest_writel(q, DMA0 + 8, 1);
    pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x8c008006, conf);
    qtest_writel(q, DMA0 + 0x20, 1 << 3);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001004);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, 0);
    qtest_writel(q, DMA0 + 0x20, 1 << 3);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001006);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, 1);
    qtest_quit(q);
}

static void test_pl080_single_pack(void)
{
    QTestState *q = start();
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t actual[8];

    qtest_memwrite(q, 0x08000000, data, sizeof(data));
    pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x8c400008,
                   0x8001 | (2 << 11) | (7 << 1));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000000);
    for (unsigned i = 0; i < sizeof(data); i++) {
        qtest_writel(q, DMA0 + 0x24, 1 << 7);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x24), ==, 0);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000001 + i);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==,
                       0x08001000 + (i + 1) / 4 * 4);
        g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==,
                       8 - (i + 1) / 4 * 4);
    }
    qtest_memread(q, 0x08001000, actual, sizeof(actual));
    g_assert_cmpmem(data, sizeof(data), actual, sizeof(actual));
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, 1);
    /* Software request bits are W1S and survive until they are serviced. */
    qtest_writel(q, DMA0 + 0x20, 0xffff0004);
    qtest_writel(q, DMA0 + 0x20, 0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x20), ==, 4);
    qtest_writel(q, DMA0 + 0x28, 0x10);
    qtest_writel(q, DMA0 + 0x2c, 0x20);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x28), ==, 0x10);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x2c), ==, 0x20);
    qtest_quit(q);
}

static void test_pl080_halt_fifo(void)
{
    QTestState *q = start();
    uint8_t data[20], actual[16];
    uint32_t conf = 1 | (3 << 11) | (1 << 1) | (2 << 6);

    memset(data, 0x5a, sizeof(data));
    qtest_memwrite(q, 0x08000000, data, sizeof(data));
    /* Source bursts of 16 bytes, destination bursts of four. */
    pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x0c00b014, conf);
    pl080_request(q, "dreq-burst", 1, 1);
    pl080_request(q, "dreq-burst", 1, 0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000010);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 20);
    qtest_writel(q, DMA0 + 0x110, conf | 0x40000);
    pl080_request(q, "dreq-single", 1, 1);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(q, DMA0 + 0x20, 1 << 2);
    }
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x100), ==, 0x08000010);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x104), ==, 0x08001010);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 0x20001, ==, 1);
    qtest_memread(q, 0x08001000, actual, sizeof(actual));
    g_assert_cmpmem(data, sizeof(actual), actual, sizeof(actual));
    qtest_quit(q);
}

static void test_pl080_fifo_state(void)
{
    QTestState *q = start();
    uint32_t conf = 1 | (3 << 11) | (2 << 1) | (3 << 6);
    g_autofree char *result = NULL;

    qtest_writel(q, 0x08000000, 0x12345678);
    pl080_transfer(q, 0x08000000, 0x08001000, 0, 0x00480001, conf);
    qtest_writel(q, DMA0 + 0x20, 1 << 2);
    /* The source has been read, but the destination has not requested it. */
    qtest_writel(q, 0x08000000, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 0x20000, ==, 0x20000);
    result = qtest_hmp(q, "savevm dma-fifo");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    result = qtest_hmp(q, "loadvm dma-fifo");
    g_assert_cmpstr(result, ==, "");
    qtest_writel(q, DMA0 + 0x20, 1 << 3);
    g_assert_cmphex(qtest_readl(q, 0x08001000), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 0x20001, ==, 0);
    qtest_quit(q);
}

static void test_pl080_bounded(void)
{
    QTestState *q = start();
    uint32_t lli[] = { cpu_to_le32(0x08000000), cpu_to_le32(0x08001000),
                       cpu_to_le32(0x08002000), cpu_to_le32(0x00480001) };

    qtest_writel(q, 0x08000000, 0x12345678);
    qtest_memwrite(q, 0x08002000, lli, sizeof(lli));
    /* A nonempty circular list must leave the monitor/CPU able to run. */
    pl080_transfer(q, 0x08000000, 0x08001000, 0x08002000, 0x00480001, 1);
    g_assert_cmphex(qtest_readl(q, 0x08001000), ==, 0x12345678);
    qtest_writel(q, DMA0 + 0x110, 0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110), ==, 0);
    /* Recursive channel reprogramming must not corrupt the host FIFO. */
    qtest_writel(q, 0x08000000, 0);
    pl080_transfer(q, 0x08000000, DMA0 + 0x110, 0, 0x00480001, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x110) & 1, ==, 0);
    qtest_quit(q);
}

static uint64_t i2s_property(QTestState *q, const char *name)
{
    QDict *reply = qtest_qmp(q, "{'execute':'qom-get','arguments':{"
                               "'path':'/machine/soc/i2s','property':%s}}",
                            name);
    uint64_t value;

    g_assert_true(qdict_haskey(reply, "return"));
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

static void i2s_setup(QTestState *q)
{
    /* 12 MHz, codec master, MCLK / 2 / 125 = 48 kHz. */
    i2c_write_register(q, 0x4a, 4, 0x2e);
    i2c_write_register(q, 0x4a, 5, 9);
    qtest_writel(q, I2S0, 1);
    qtest_writel(q, I2S0 + 4, 0x0b100019);
    qtest_writel(q, I2S0 + 0x40, 250);
}

static int64_t codec_property(QTestState *q, const char *name)
{
    QDict *reply = qtest_qmp(q, "{'execute':'qom-get','arguments':{"
                               "'path':'/machine/codec','property':%s}}", name);
    int64_t value;

    g_assert_true(qdict_haskey(reply, "return"));
    value = qdict_get_int(reply, "return");
    qobject_unref(reply);
    return value;
}

static void codec_playback_setup(QTestState *q)
{
    i2s_setup(q);
    i2c_write_register(q, 0x4a, 2, 0x0e);
    i2c_write_register(q, 0x4a, 3, 0xaa);
    i2c_write_register(q, 0x4a, 7, 0); /* No transition shaping. */
    i2c_write_register(q, 0x4a, 0x0f, 0x80); /* Original DSP bypass. */
    qtest_writel(q, I2S0 + 8, 4);
}

static void codec_frame(QTestState *q, int16_t left, int16_t right,
                         int16_t expected_left, int16_t expected_right)
{
    uint32_t frame;

    qtest_writew(q, I2S0 + 0x10, left);
    qtest_writew(q, I2S0 + 0x10, right);
    qtest_clock_step(q, 21000);
    frame = codec_property(q, "last-output-frame");
    /* Nominal dB conversion allows one final S16 rounding unit. */
    g_assert_cmpint(abs((int16_t)frame - expected_left), <=, 1);
    g_assert_cmpint(abs((int16_t)(frame >> 16) - expected_right), <=, 1);
}

static void test_codec_pcm(void)
{
    QTestState *q = start();

    codec_playback_setup(q);
    codec_frame(q, 12000, -8000, 12000, -8000);
    i2c_write_register(q, 0x4a, 0x0f, 0x84); /* DSP bypasses PCM polarity. */
    codec_frame(q, 12000, -8000, 12000, -8000);
    i2c_write_register(q, 0x4a, 0x0f, 0x80);
    i2c_write_register(q, 0x4a, 0x1a, 0x74); /* HP A -12 dB. */
    i2c_write_register(q, 0x4a, 0x19, 0xf4); /* Master B -6 dB. */
    codec_frame(q, 12000, -8000, 3014, -4009);
    i2c_write_register(q, 0x4a, 0x12, 0x80);
    codec_frame(q, 12000, -8000, 3014, -4009); /* DSP bypasses PMIX mute. */
    i2c_write_register(q, 0x4a, 0x0f, 0);
    codec_frame(q, 12000, -8000, 0, -4009);
    i2c_write_register(q, 0x4a, 0x1a, 0);
    i2c_write_register(q, 0x4a, 0x19, 0);

    /* The wrap at code 25 is not a conventional signed seven-bit gain. */
    i2c_write_register(q, 0x4a, 0x12, 24);
    codec_frame(q, 6000, -8000, 23886, -8000);
    i2c_write_register(q, 0x4a, 0x12, 25);
    codec_frame(q, 6000, -8000, 15, -8000);
    i2c_write_register(q, 0x4a, 0x12, 0);
    i2c_write_register(q, 0x4a, 0x18, 24);
    codec_frame(q, 6000, -8000, 23886, -8000);
    i2c_write_register(q, 0x4a, 0x18, 25); /* Master clamps at -102 dB. */
    codec_frame(q, 32767, -8000, 0, -8000);
    i2c_write_register(q, 0x4a, 0x18, 0);

    /* Gang volume, but preserve independent digital mute bits. */
    i2c_write_register(q, 0x4a, 0x18, 0xf4);
    i2c_write_register(q, 0x4a, 0x0f, 0x11);
    codec_frame(q, 12000, -8000, 0, -4009);
    i2c_write_register(q, 0x4a, 0x0f, 0x10);
    i2c_write_register(q, 0x4a, 0x12, 0x80);
    codec_frame(q, 12000, -8000, 0, -4009);
    i2c_write_register(q, 0x4a, 0x12, 0);
    i2c_write_register(q, 0x4a, 0x18, 0);
    i2c_write_register(q, 0x4a, 0x0f, 0);
    i2c_write_register(q, 0x4a, 0x20, 0xf0); /* Swap. */
    codec_frame(q, 12000, -8000, -8000, 12000);
    i2c_write_register(q, 0x4a, 0x20, 0x60); /* Both mono encodings. */
    codec_frame(q, 12000, -8000, 2000, 2000);
    i2c_write_register(q, 0x4a, 0x20, 0);
    i2c_write_register(q, 0x4a, 0x0f, 4); /* Invert left, saturate output. */
    codec_frame(q, -32768, -8000, 32767, -8000);
    qtest_quit(q);
}

static void test_codec_output_power(void)
{
    QTestState *q = start();
    uint64_t count;

    codec_playback_setup(q);
    i2c_write_register(q, 0x4a, 3, 0xea); /* HP B forced off. */
    codec_frame(q, 12000, -8000, 12000, 0);
    i2c_write_register(q, 0x4a, 3, 0x4a); /* A low, B high on HPDETECT. */
    codec_frame(q, 12000, -8000, 12000, 0);
    qtest_set_irq_in(q, "/machine/codec", "hpdetect", 0, 1);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x29), ==, 0x80);
    codec_frame(q, 12000, -8000, 0, -8000);
    i2c_write_register(q, 0x4a, 3, 0xaa);
    i2c_write_register(q, 0x4a, 8, 1); /* Analog input selected for HP A. */
    codec_frame(q, 12000, -8000, 0, -8000);
    i2c_write_register(q, 0x4a, 8, 0);
    i2c_write_register(q, 0x4a, 0x1b, 0x80);
    codec_frame(q, 12000, -8000, 12000, 0);
    count = codec_property(q, "output-frames");
    i2c_write_register(q, 0x4a, 2, 0x0f);
    qtest_writew(q, I2S0 + 0x10, 123);
    qtest_writew(q, I2S0 + 0x10, 456);
    qtest_clock_step(q, 21000);
    g_assert_cmpuint(codec_property(q, "output-frames"), ==, count);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==,
                     2 * (count + 1));
    i2c_write_register(q, 0x4a, 2, 0x0e);
    codec_frame(q, 12000, -8000, 12000, 0);
    qtest_quit(q);
}

static void test_codec_output_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    codec_playback_setup(q);
    codec_frame(q, 12000, -8000, 12000, -8000);
    i2c_write_register(q, 0x4a, 7, 1);
    i2c_write_register(q, 0x4a, 0x1a, 0x80);
    i2c_write_register(q, 0x4a, 0x19, 0xf4);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x1a), ==, 0x80);
    codec_frame(q, 12000, -8000, 12000, -8000);
    result = qtest_hmp(q, "savevm codec-output");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    i2c_write_register(q, 0x4a, 7, 0);
    codec_frame(q, 12000, -8000, 0, -4009);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmpuint(codec_property(q, "output-frames"), ==, 0);
    result = qtest_hmp(q, "loadvm codec-output");
    g_assert_cmpstr(result, ==, "");
    codec_frame(q, 12000, -8000, 12000, -8000);
    i2c_write_register(q, 0x4a, 7, 0);
    codec_frame(q, 12000, -8000, 0, -4009);
    qtest_quit(q);
}

static void test_codec_line_out(void)
{
    QTestState *q = start_with_args("-global s5l8702-cs42l55.line-out=on");

    codec_playback_setup(q);
    i2c_write_register(q, 0x4a, 0x1a, 0x80); /* HP mute leaves line alone. */
    i2c_write_register(q, 0x4a, 0x1c, 0x7a); /* Line A -6 dB. */
    codec_frame(q, 12000, -8000, 6014, -8000);
    i2c_write_register(q, 0x4a, 3, 0xaf); /* Original line disable. */
    codec_frame(q, 12000, -8000, 0, 0);
    qtest_quit(q);
}

static void test_codec_host_backpressure(void)
{
    /* Delay host consumption beyond the duration of this guest transfer. */
    QTestState *q = start_with_audio("", "none,timer-period=1000000");

    codec_playback_setup(q);
    for (unsigned i = 0; i < 12000; i++) {
        qtest_writew(q, I2S0 + 0x10, i);
        qtest_writew(q, I2S0 + 0x10, -i);
        qtest_clock_step(q, 21000);
    }
    g_assert_cmpuint(codec_property(q, "host-dropped-frames"), >, 0);
    g_assert_cmpuint(codec_property(q, "output-frames"), ==, 12000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 24000);
    g_assert_cmpuint(i2s_property(q, "queued-samples"), ==, 0);
    qtest_quit(q);
}

static void test_codec_ramp(void)
{
    QTestState *q = start();
    int64_t turning_level;

    codec_playback_setup(q);
    i2c_write_register(q, 0x4a, 0x0f, 0);
    i2c_write_register(q, 0x4a, 7, 4);
    i2c_write_register(q, 0x4a, 0x12, 0x74); /* PCM A: 0 to -6 dB. */
    g_assert_cmpint(codec_property(q, "pcm-volume-a"), ==, 0);
    qtest_clock_step(q, 125000); /* Six LRCK cycles, six eighth-dB steps. */
    g_assert_cmpint(codec_property(q, "pcm-volume-a"), ==, -6);
    qtest_clock_step(q, 875000);
    g_assert_cmpint(codec_property(q, "pcm-volume-a"), ==, -48);
    codec_frame(q, 12000, -8000, 6014, -8000);

    /* Master ramp remains operational with the DSP mixer powered down. */
    i2c_write_register(q, 0x4a, 0x0f, 0x80);
    i2c_write_register(q, 0x4a, 0x18, 0xf4);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, 0);
    qtest_clock_step(q, 500000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, -24);
    qtest_writew(q, I2S0 + 0x10, 12000);
    qtest_writew(q, I2S0 + 0x10, 8000);
    qtest_clock_step(q, 21000);
    /* The emitted sample must use the intermediate gain, not either end. */
    g_assert_cmpint((int16_t)codec_property(q, "last-output-frame"), >, 8100);
    g_assert_cmpint((int16_t)codec_property(q, "last-output-frame"), <, 8600);
    i2c_write_register(q, 0x4a, 0x18, 0); /* Reverse an unfinished ramp. */
    turning_level = codec_property(q, "master-volume-a");
    g_assert_cmpint(turning_level, <, -24); /* I2C time also counts. */
    qtest_clock_step(q, 250000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==,
                    turning_level + 12);
    qtest_clock_step(q, 1000000);
    codec_frame(q, 12000, -8000, 12000, -8000);

    /* FREEZE stages both channels; the ramp starts only on commit. */
    i2c_write_register(q, 0x4a, 7, 5);
    i2c_write_register(q, 0x4a, 0x18, 0xf4);
    i2c_write_register(q, 0x4a, 0x19, 0xf4);
    qtest_clock_step(q, 1000000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, 0);
    g_assert_cmpint(codec_property(q, "master-volume-b"), ==, 0);
    i2c_write_register(q, 0x4a, 7, 4);
    qtest_clock_step(q, 500000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, -24);
    g_assert_cmpint(codec_property(q, "master-volume-b"), ==, -24);
    i2c_write_register(q, 0x4a, 7, 0); /* Disabled ramp takes effect now. */
    codec_frame(q, 12000, -8000, 6014, -4009);
    qtest_quit(q);
}

static void test_codec_ramp_clock_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;
    int64_t saved_level, next_level;

    codec_playback_setup(q);
    i2c_write_register(q, 0x4a, 7, 4);
    i2c_write_register(q, 0x4a, 0x18, 0xf4);
    qtest_clock_step(q, 10000); /* Retain a partial cycle across clock stop. */
    saved_level = codec_property(q, "master-volume-a");
    qtest_writel(q, CLK + 0x0c, 0x8000);
    qtest_clock_step(q, 1000000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, saved_level);
    qtest_writel(q, CLK + 0x0c, 1); /* 24 kHz LRCK after MCLK / 2. */
    qtest_clock_step(q, 500000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==,
                    saved_level - 12);
    qtest_writel(q, CLK + 0x4c, 0x80); /* I2S bus gate is not codec LRCK. */
    qtest_clock_step(q, 500000);
    saved_level = codec_property(q, "master-volume-a");
    g_assert_cmpint(saved_level, >=, -25);
    g_assert_cmpint(saved_level, <=, -24);

    result = qtest_hmp(q, "savevm codec-ramp");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_clock_step(q, 20000);
    next_level = codec_property(q, "master-volume-a");
    result = qtest_hmp(q, "loadvm codec-ramp");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, saved_level);
    qtest_clock_step(q, 20000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, next_level);
    qtest_clock_step(q, 2000000);
    g_assert_cmpint(codec_property(q, "master-volume-a"), ==, -48);
    qtest_quit(q);
}

static void test_codec_zero_cross(void)
{
    QTestState *q = start();

    codec_playback_setup(q);
    codec_frame(q, 12000, 8000, 12000, 8000);
    i2c_write_register(q, 0x4a, 7, 8);
    i2c_write_register(q, 0x4a, 0x1a, 0x74);
    g_assert_cmphex(i2c_read_register(q, 0x4a, 0x1a), ==, 0x74);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    codec_frame(q, 12000, 8000, 12000, 8000);
    codec_frame(q, -12000, 8000, -3014, 8000);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0x74);

    /* Analog mute waits for its own channel's crossing. */
    i2c_write_register(q, 0x4a, 0x1b, 0x80);
    codec_frame(q, -12000, 8000, -3014, 8000);
    codec_frame(q, -12000, -8000, -3014, 0);
    g_assert_cmphex(codec_property(q, "headphone-control-b"), ==, 0x80);
    i2c_write_register(q, 0x4a, 0x0f, 0x90); /* Ganged target, separate ZC. */
    codec_frame(q, -12000, -8000, -3014, 0);
    codec_frame(q, -12000, 8000, -3014, 2009);
    g_assert_cmphex(codec_property(q, "headphone-control-b"), ==, 0x74);
    i2c_write_register(q, 0x4a, 0x1a, 0);
    i2c_write_register(q, 0x4a, 7, 0); /* Disable ZC with a pending target. */
    codec_frame(q, -12000, 8000, -12000, 8000);
    qtest_quit(q);
}

static void test_codec_zero_cross_timeout(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    codec_playback_setup(q);
    codec_frame(q, 12000, 8000, 12000, 8000);
    i2c_write_register(q, 0x4a, 7, 8);
    i2c_write_register(q, 0x4a, 0x1a, 0x74);
    qtest_clock_step(q, 10000000); /* 480 of the modeled 1024 cycles. */
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    qtest_writel(q, CLK + 0x0c, 0x8000);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    qtest_writel(q, CLK + 0x0c, 1); /* Half rate, preserving elapsed cycles. */
    qtest_clock_step(q, 22000000); /* Another 528 cycles, 1008 total. */
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0x74);

    /* A pending zero crossing and its remaining timeout survive a snapshot. */
    i2c_write_register(q, 0x4a, 0x1a, 0);
    qtest_clock_step(q, 5000000); /* 120 cycles. */
    result = qtest_hmp(q, "savevm codec-zero-cross");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    i2c_write_register(q, 0x4a, 7, 0);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    result = qtest_hmp(q, "loadvm codec-zero-cross");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0x74);
    qtest_clock_step(q, 37000000); /* 888 additional cycles: still pending. */
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0x74);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(codec_property(q, "headphone-control-a"), ==, 0);
    qtest_quit(q);
}

static void test_i2s_dma_clock(void)
{
    QTestState *q = start();

    i2s_setup(q);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writew(q, 0x08000000 + 2 * i, 0x1100 + i);
    }
    /* The original order enables DMA before TXCOM. It must still wait. */
    pl080_transfer(q, 0x08000000, I2S0 + 0x10, 0, 0x84249008, 0x8a81);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 8);
    qtest_writel(q, I2S0 + 8, 6);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 4);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    qtest_clock_step(q, 20833);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 2);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0x11011100);
    qtest_clock_step(q, 20833);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0x11031102);
    /* Only the next request can deliver the remaining four samples. */
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x14), ==, 1);
    qtest_clock_step(q, 41667);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 8);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0x11071106);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 8);

    /* Neither a gated bus clock nor an absent LRCK may request more data. */
    qtest_writel(q, CLK + 0x4c, 0x80);
    pl080_transfer(q, 0x08000000, I2S0 + 0x10, 0, 0x84249004, 0x8a81);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 4);
    qtest_writel(q, CLK + 0x0c, 0x8000);
    qtest_writel(q, CLK + 0x4c, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x10c) & 0xfff, ==, 4);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 8);
    qtest_writel(q, CLK + 0x0c, 0);
    qtest_clock_step(q, 50000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 12);
    qtest_quit(q);
}

static void test_sm1_memory_descriptors(void)
{
    QTestState *q = start();
    uint32_t descriptor[4][2];

    /*
     * retailOS 0x080ab624 adds +0xa48 directly to the memory base and
     * returns +0xa44 as the length. 0x080cd04c uses that pair for an address
     * range check. Serializing I2S audio must not move these memory regions.
     * Their physical sizes/offsets still require decoding; do not assert
     * the current placeholder values here.
     */
    for (unsigned bank = 0; bank < 4; bank++) {
        for (unsigned field = 0; field < 2; field++) {
            descriptor[bank][field] =
                qtest_readl(q, SM1 + 0xa44 + bank * 0x14 + field * 4);
        }
    }
    i2s_setup(q);
    qtest_writel(q, I2S0 + 8, 4); /* PIO, no DMA requests. */
    for (unsigned frame = 0; frame < 4; frame++) {
        qtest_writew(q, I2S0 + 0x10, 0x1234);
        qtest_writew(q, I2S0 + 0x10, 0x5678);
        qtest_clock_step(q, 20834);
        g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==,
                         (frame + 1) * 2);
        for (unsigned bank = 0; bank < 4; bank++) {
            for (unsigned field = 0; field < 2; field++) {
                g_assert_cmphex(qtest_readl(q, SM1 + 0xa44 + bank * 0x14 +
                                           field * 4), ==,
                                descriptor[bank][field]);
            }
        }
    }
    qtest_quit(q);
}

static void test_i2s_clock_gate(void)
{
    QTestState *q = start();
    uint64_t count;

    i2s_setup(q);
    qtest_writew(q, I2S0 + 0x10, 0x1234);
    qtest_writew(q, I2S0 + 0x10, 0xabcd);
    qtest_writel(q, I2S0 + 8, 4); /* PIO, no DMA requests. */
    qtest_clock_step(q, 10000);
    qtest_writel(q, CLK + 0x4c, 0x80);
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    /* Halving MCLK while gated doubles the remaining frame time. */
    qtest_writel(q, CLK + 0x0c, 1);
    qtest_writel(q, CLK + 0x4c, 0);
    qtest_clock_step(q, 21666);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 2);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0xabcd1234);
    qtest_writew(q, I2S0 + 0x10, 0x5678);
    qtest_writew(q, I2S0 + 0x10, 0xef01);
    qtest_writel(q, CLK + 0x0c, 0x8001); /* Codec LRCK stops. */
    count = i2s_property(q, "transmitted-samples");
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, count);
    qtest_writel(q, CLK + 0x0c, 1);
    qtest_clock_step(q, 41667);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, count + 2);
    qtest_quit(q);
}

static void test_i2s_tx_state(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    i2s_setup(q);
    qtest_writel(q, 0x08000000, 0x22114433);
    qtest_writel(q, 0x08000004, 0x66558877);
    qtest_writel(q, 0x08000008, 0xaabbccdd);
    qtest_writel(q, 0x0800000c, 0xeeff0011);
    qtest_writel(q, I2S0 + 8, 6);
    pl080_transfer(q, 0x08000000, I2S0 + 0x10, 0, 0x84249008, 0x8a81);
    qtest_clock_step(q, 10000);
    result = qtest_hmp(q, "savevm i2s-tx");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    result = qtest_hmp(q, "loadvm i2s-tx");
    g_assert_cmpstr(result, ==, "");
    /* Queued values and fractional frame position must survive the snapshot. */
    qtest_writel(q, 0x08000000, 0);
    qtest_clock_step(q, 10833);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0x22114433);
    qtest_clock_step(q, 62500);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 8);
    g_assert_cmphex(i2s_property(q, "last-frame"), ==, 0xeeff0011);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_clock_step(q, 1000000);
    g_assert_cmpuint(i2s_property(q, "transmitted-samples"), ==, 0);
    g_assert_cmpuint(i2s_property(q, "queued-samples"), ==, 0);
    qtest_quit(q);
}

static void test_tvo_controls(void)
{
    QTestState *q = start();
    g_autofree char *result = NULL;

    /* The known sleep register must accept a bus transaction. */
    qtest_writel(q, 0x08000000, 1);
    pl080_transfer(q, 0x08000000, 0x39300280, 0, 0x00480001, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 0);
    qtest_writel(q, 0x39200048, 0x00108080);
    qtest_writel(q, 0x39300284, qtest_readl(q, 0x39300284) | 1);
    qtest_writel(q, 0x3930003c, 0x01000700);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t control = 0x39100000 + 0x100000 * i;

        qtest_writel(q, control, 7);
        g_assert_cmphex(qtest_readl(q, control), ==, 5);
        qtest_writel(q, control, qtest_readl(q, control) & ~1u);
        g_assert_cmphex(qtest_readl(q, control), ==, 6);
    }
    result = qtest_hmp(q, "savevm tvo-controls");
    g_assert_cmpstr(result, ==, "");
    g_clear_pointer(&result, g_free);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    result = qtest_hmp(q, "loadvm tvo-controls");
    g_assert_cmpstr(result, ==, "");
    g_assert_cmphex(qtest_readl(q, 0x39200048), ==, 0x00108080);
    g_assert_cmphex(qtest_readl(q, 0x39300280), ==, 1);
    g_assert_cmphex(qtest_readl(q, 0x39300284), ==, 1);
    g_assert_cmphex(qtest_readl(q, 0x3930003c), ==, 0x01000700);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(q, 0x39100000 + 0x100000 * i), ==, 6);
    }
    /* The rest of the unimplemented encoder must not hide bus errors. */
    pl080_transfer(q, 0x08000000, 0x39300288, 0, 0x00480001, 1);
    g_assert_cmphex(qtest_readl(q, DMA0 + 0x18), ==, 1);
    qtest_quit(q);
}

static void test_aes(void)
{
    /* NIST SP 800-38A F.2, first CBC block for each supported key length. */
    static const uint8_t keys[3][32] = {
        { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
          0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c },
        { 0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52,
          0xc8, 0x10, 0xf3, 0x2b, 0x80, 0x90, 0x79, 0xe5,
          0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b },
        { 0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
          0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
          0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
          0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4 },
    };
    static const uint8_t plain[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    };
    static const uint8_t encrypted[3][16] = {
        { 0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
          0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d },
        { 0x4f, 0x02, 0x1d, 0xb2, 0x43, 0xbc, 0x63, 0x3d,
          0x71, 0x78, 0x18, 0x3a, 0x9f, 0xa0, 0x71, 0xe8 },
        { 0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba,
          0x77, 0x9e, 0xab, 0xfb, 0x5f, 0x7b, 0xfb, 0xd6 },
    };
    QTestState *q = start();
    uint8_t result[16];

    qtest_writel(q, AES + 0x28, 0x08000000);
    qtest_writel(q, AES + 0x20, 0x08000000);
    qtest_writel(q, AES + 0x24, 16);
    for (unsigned n = 0; n < 3; n++) {
        unsigned len = 16 + n * 8;

        for (unsigned i = 0; i < len; i += 4) {
            qtest_writel(q, AES + 0x6c - len + i, ldl_be_p(keys[n] + i));
        }
        for (unsigned i = 0; i < 4; i++) {
            qtest_writel(q, AES + 0x74 + i * 4, 0x00010203 + i * 0x04040404);
        }
        qtest_memwrite(q, 0x08000000, plain, 16);
        qtest_writel(q, AES + 0x14, 0xf | (n << 4));
        qtest_writel(q, AES + 4, 1);
        qtest_memread(q, 0x08000000, result, 16);
        g_assert_cmpmem(result, 16, encrypted[n], 16);
        qtest_writel(q, AES + 0x14, 0xe | (n << 4));
        qtest_writel(q, AES + 4, 1);
        qtest_memread(q, 0x08000000, result, 16);
        g_assert_cmpmem(result, 16, plain, 16);
    }
    qtest_quit(q);
}

static void test_sha(void)
{
    static const uint8_t digest[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
        0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
        0x9c, 0xd0, 0xd8, 0x9d,
    };
    QTestState *q = start();
    uint8_t block[64] = { 'a', 'b', 'c', 0x80 };
    uint8_t result[20];

    qtest_irq_intercept_in(q, "/machine/soc/vic1");
    block[63] = 24;
    for (unsigned i = 0; i < 16; i++) {
        qtest_writel(q, 0x38000040 + i * 4, ldl_le_p(block + i * 4));
    }
    qtest_writel(q, 0x38000000, 6);
    g_assert_true(qtest_get_irq(q, 8));
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(result + i * 4, qtest_readl(q, 0x38000020 + i * 4));
    }
    g_assert_cmpmem(result, sizeof(result), digest, sizeof(digest));
    qtest_writel(q, 0x38000008, 1);
    g_assert_false(qtest_get_irq(q, 8));
    /* Processing > 1 MiB previously overran the whole-message buffer. */
    for (unsigned i = 0; i < 16400; i++) {
        qtest_writel(q, 0x38000000, 0xa);
    }
    /* These formerly indexed beyond the input/output arrays. */
    qtest_writel(q, 0x38000080, 0xffffffff);
    g_assert_cmphex(qtest_readl(q, 0x38000060), ==, 0);
    qtest_writel(q, 0x38000004, 1);
    g_assert_cmphex(qtest_readl(q, 0x38000000), ==, 0);
    qtest_quit(q);
}

static void test_remap(void)
{
    QTestState *q = start();

    qtest_writel(q, 0x22000000, 0x11223344);
    qtest_writel(q, 0x22020000, 0x55667788);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0);
    qtest_writel(q, 0x38100000, 0x80d);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(q, 0x20000), ==, 0x55667788);
    qtest_writel(q, 0x38100000, 0x80c);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0);
    qtest_quit(q);
}

static void wait_for_boot(QTestState *q, uint32_t count)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    do {
        if (qtest_readl(q, 0x08000000) == count) {
            g_autofree char *regs = qtest_hmp(q, "info registers");

            if (strstr(regs, "R15=22000010")) {
                return;
            }
        }
        g_usleep(10000);
    } while (g_get_monotonic_time() < deadline);
    g_error("CPU did not complete boot %u and enter WFI", count);
}

static void test_cpu_reset(void)
{
    /* Count entries through the reset vector, then branch to IRAM. */
    static const uint32_t boot[] = {
        0xe3a00302, /* mov r0, #0x08000000 */
        0xe5901000, /* ldr r1, [r0] */
        0xe2811001, /* add r1, r1, #1 */
        0xe5801000, /* str r1, [r0] */
        0xe3a00422, /* mov r0, #0x22000000 */
        0xe12fff10, /* bx r0 */
    };
    /* Remap IRAM over the boot ROM alias and halt with IRQ/FIQ masked. */
    static const uint32_t sleep[] = {
        0xe59f000c, /* ldr r0, [pc, #12] */
        0xe3a01001, /* mov r1, #1 */
        0xe5801000, /* str r1, [r0] */
        0xee070f90, /* mcr p15, 0, r0, c7, c0, 4 (WFI) */
        0xeafffffd, /* b WFI */
        0x38100000,
    };
    QTestState *q;
    g_autofree char *regs = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is required to execute the reset test ROM");
        return;
    }
    q = start_with_args("-accel tcg -S");
    for (unsigned i = 0; i < ARRAY_SIZE(boot); i++) {
        qtest_writel(q, 0x20000000 + 4 * i, boot[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(sleep); i++) {
        qtest_writel(q, 0x22000000 + 4 * i, sleep[i]);
    }
    qtest_qmp_assert_success(q, "{'execute':'cont'}");
    wait_for_boot(q, 1);
    g_assert_cmphex(qtest_readl(q, 0x38100000), ==, 1);

    /* Reset while paused must restore the CPU and the boot ROM mapping. */
    qtest_qmp_assert_success(q, "{'execute':'stop'}");
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(q, "RESET");
    regs = qtest_hmp(q, "info registers");
    g_assert_nonnull(strstr(regs, "R15=00000000"));
    g_assert_cmphex(qtest_readl(q, 0x38100000), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0), ==, boot[0]);
    g_assert_cmpuint(qtest_readl(q, 0x08000000), ==, 1);
    qtest_qmp_assert_success(q, "{'execute':'cont'}");
    wait_for_boot(q, 2);

    /* A running VM must leave WFI and execute the reset vector again. */
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(q, "RESET");
    wait_for_boot(q, 3);

    /* retailOS 0x0835d028 requests reset by writing 0xaa5 to SWRCON. */
    qtest_writel(q, 0x3c500050, 0xaa5);
    qtest_qmp_eventwait(q, "RESET");
    wait_for_boot(q, 4);
    qtest_quit(q);
}

static void test_sha_dma(void)
{
    static const char message[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t digest[20] = {
        0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e,
        0xba, 0xae, 0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5,
        0xe5, 0x46, 0x70, 0xf1,
    };
    QTestState *q = start();
    uint8_t blocks[128] = { 0 }, result[20];
    uint32_t intermediate[5];

    qtest_irq_intercept_in(q, "/machine/soc/vic1");
    memcpy(blocks, message, sizeof(message) - 1);
    blocks[sizeof(message) - 1] = 0x80;
    stq_be_p(blocks + 120, (sizeof(message) - 1) * 8);
    qtest_memwrite(q, 0x08010000, blocks, sizeof(blocks));
    qtest_writel(q, 0x38000080, 1);
    qtest_writel(q, 0x38000084, 0x08010000);
    qtest_writel(q, 0x3800008c, 64);
    qtest_writel(q, 0x38000000, 6);
    g_assert_true(qtest_get_irq(q, 8));
    for (unsigned i = 0; i < 5; i++) {
        intermediate[i] = qtest_readl(q, 0x38000020 + i * 4);
    }
    qtest_writel(q, 0x38000008, 1);
    g_assert_false(qtest_get_irq(q, 8));

    /* Another context uses the engine before the saved context resumes. */
    qtest_writel(q, 0x3800008c, 128);
    qtest_writel(q, 0x38000000, 2);
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(result + i * 4, qtest_readl(q, 0x38000020 + i * 4));
        qtest_writel(q, 0x38000020 + i * 4, intermediate[i]);
    }
    g_assert_cmpmem(result, sizeof(result), digest, sizeof(digest));
    qtest_writel(q, 0x38000084, 0x08010040);
    qtest_writel(q, 0x3800008c, 64);
    qtest_writel(q, 0x38000000, 0xa);
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(result + i * 4, qtest_readl(q, 0x38000020 + i * 4));
    }
    g_assert_cmpmem(result, sizeof(result), digest, sizeof(digest));

    /* A malformed DMA must neither recurse into MMIO nor report success. */
    qtest_writel(q, 0x38000008, 1);
    qtest_writel(q, 0x38000084, 0x38000000);
    qtest_writel(q, 0x38000000, 0xe);
    g_assert_false(qtest_get_irq(q, 8));
    g_assert_cmphex(qtest_readl(q, 0x38000008), ==, 0);
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(result + i * 4, qtest_readl(q, 0x38000020 + i * 4));
    }
    g_assert_cmpmem(result, sizeof(result), digest, sizeof(digest));
    qtest_writel(q, 0x38000084, 0xfffffff0);
    qtest_writel(q, 0x38000000, 0xe);
    g_assert_cmphex(qtest_readl(q, 0x38000008), ==, 0);
    qtest_writel(q, 0x38000084, 0x08010000);
    qtest_writel(q, 0x3800008c, UINT32_MAX);
    qtest_writel(q, 0x38000000, 0xe);
    g_assert_cmphex(qtest_readl(q, 0x38000008), ==, 0);
    qtest_writel(q, 0x38000004, 1);
    g_assert_cmphex(qtest_readl(q, 0x38000080), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x38000084), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x3800008c), ==, 0);
    qtest_quit(q);
}

static char *empty_image(const char *tmpl, size_t size)
{
    char *path;
    int fd = g_file_open_tmp(tmpl, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    close(fd);
    return path;
}

#define DISP 0x38900000
#define LCD  0x38300000
#define SYSIC 0x39a00000

static void lcd_command(QTestState *q, unsigned command)
{
    qtest_writel(q, LCD + 4, command);
}

static void lcd_assert_pixel(QTestState *q, unsigned x, unsigned y,
                              unsigned r, unsigned g, unsigned b)
{
    static const char header[] = "P6\n320 240\n255\n";
    g_autofree char *path = empty_image("s5l-screen-XXXXXX", 0);
    g_autofree char *data = NULL;
    gsize size;
    unsigned offset = sizeof(header) - 1 + (y * 320 + x) * 3;

    qtest_qmp_assert_success(q, "{'execute':'screendump',"
                            "'arguments':{'filename':%s}}", path);
    g_assert_true(g_file_get_contents(path, &data, &size, NULL));
    g_assert_cmpuint(size, ==, sizeof(header) - 1 + 320 * 240 * 3);
    g_assert_cmpmem(data, sizeof(header) - 1, header, sizeof(header) - 1);
    g_assert_cmpuint((uint8_t)data[offset], ==, r);
    g_assert_cmpuint((uint8_t)data[offset + 1], ==, g);
    g_assert_cmpuint((uint8_t)data[offset + 2], ==, b);
    unlink(path);
}

static void test_display(void)
{
    QTestState *q = start();
    const uint8_t y[8] = {126, 126, 126, 126, 126, 126, 126, 126};
    const uint8_t c[2] = {128, 128};

    qtest_memwrite(q, 0x08010000, y, sizeof(y));
    qtest_memwrite(q, 0x08011000, c, sizeof(c));
    qtest_memwrite(q, 0x08012000, c, sizeof(c));
    qtest_writel(q, DISP + 0x08, 0xc0); /* Planar + window 0. */
    qtest_writel(q, DISP + 0x28, 0x100); /* Planar 4:2:0. */
    qtest_writel(q, DISP + 0x2c, 0x00020004);
    qtest_writel(q, DISP + 0x34, 0x00040002);
    qtest_writel(q, DISP + 0x38, 0x08010000);
    qtest_writel(q, DISP + 0x3c, 0x08011000);
    qtest_writel(q, DISP + 0x44, 0x08012000);
    qtest_writel(q, DISP + 0x4c, 0x10001000);
    qtest_writel(q, DISP + 0x54, 0x00040002);
    qtest_writel(q, DISP + 0x58, 8);
    qtest_writel(q, DISP + 0x5c, 0x700);
    qtest_writel(q, DISP + 0x60, 0x08013000);
    qtest_writel(q, DISP + 0x64, 0x00020002);
    qtest_writel(q, 0x08013000, 0x00ff0000); /* Transparent red. */
    qtest_writel(q, 0x08013004, 0x80ff0000); /* Half-alpha red. */
    qtest_writel(q, 0x08013008, 0xffff0000);
    qtest_writel(q, 0x0801300c, 0x40ff0000);
    qtest_writel(q, DISP + 0xd4, 1); /* Planar, multi-window, window 0. */
    qtest_writel(q, DISP + 0xd8, 0x10000000);
    qtest_writel(q, DISP + 0xe0, 0x10000000);
    qtest_writel(q, DISP + 0xe8, 0x50004000);
    lcd_command(q, 0x11);
    lcd_command(q, 0x29);
    qtest_writel(q, LCD + 0x70, 1);
    qtest_writel(q, LCD + 0x80, 1);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 128, 128, 128);
    lcd_assert_pixel(q, 1, 0, 192, 64, 64);
    lcd_assert_pixel(q, 0, 1, 248, 0, 0);

    /* Source memory/register changes must not alter an already sent frame. */
    qtest_writel(q, 0x08013000, 0xffff0000);
    qtest_writel(q, DISP + 0x08, 0x80);
    lcd_assert_pixel(q, 0, 0, 128, 128, 128);
    lcd_assert_pixel(q, 0, 1, 248, 0, 0);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 1, 128, 128, 128);

    /* Window 3 obeys its enable; this used to draw even when disabled. */
    qtest_writel(q, DISP + 0xa0, 4);
    qtest_writel(q, DISP + 0xa4, 0x700);
    qtest_writel(q, DISP + 0xa8, 0x08014000);
    qtest_writel(q, DISP + 0xac, 0x00010001);
    qtest_writel(q, 0x08014000, 0xff0000ff);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 128, 128, 128);
    qtest_writel(q, DISP + 0x08, 0x88);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);

    /* Panel refresh/TE must not copy a frame without guest submission. */
    lcd_command(q, 0x35);
    qtest_writel(q, 0x08014000, 0xff00ff00);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);
    qtest_clock_step(q, 100000000);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 0, 252, 0);
    qtest_writel(q, LCD + 0x70, 0);
    qtest_writel(q, 0x08014000, 0xff0000ff);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 0, 252, 0);
    qtest_writel(q, LCD + 0x70, 1);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);
    qtest_writel(q, LCD + 0x80, 1);

    /* MMIO is not a valid framebuffer: no recursion or partial frame. */
    qtest_writel(q, DISP + 0xa8, LCD);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);
    lcd_command(q, 0x28);
    lcd_assert_pixel(q, 0, 0, 0, 0, 0);
    lcd_command(q, 0x29);
    lcd_assert_pixel(q, 0, 0, 0, 0, 248);

    /* A color vector distinguishes the Cb/Cr planes and range conversion. */
    qtest_writel(q, DISP + 0x08, 0x80);
    qtest_writeb(q, 0x08010000, 81);
    qtest_writeb(q, 0x08011000, 90);
    qtest_writeb(q, 0x08012000, 240);
    lcd_command(q, 0x2c);
    lcd_assert_pixel(q, 0, 0, 248, 0, 0);
    qtest_quit(q);
}

static void test_lcd_te(void)
{
    QTestState *q = start();
    const uint32_t te = 1u << 15; /* GPIO 55: group 5, bit 15. */

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    qtest_writel(q, VIC0 + 0x10, 1u << 1);
    qtest_writel(q, SYSIC + 0x94, te); /* Rising edge. */
    qtest_writel(q, SYSIC + 0xd4, te);
    lcd_command(q, 0x35);
    qtest_clock_step(q, 100000000);
    g_assert_false(qtest_get_irq(q, 0)); /* Panel is still asleep. */
    lcd_command(q, 0x11);
    qtest_clock_step(q, 16666667);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xb4), ==, te);
    g_assert_cmphex(qtest_readl(q, VIC0), ==, 1u << 1);
    qtest_writel(q, SYSIC + 0xb4, te);
    g_assert_false(qtest_get_irq(q, 0));
    lcd_command(q, 0x34);
    qtest_clock_step(q, 100000000);
    g_assert_false(qtest_get_irq(q, 0));
    lcd_command(q, 0x35);
    qtest_clock_step(q, 16666667);
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0xb4, te);
    lcd_command(q, 0x10);
    qtest_clock_step(q, 100000000);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_quit(q);
}

static void test_gpio_irq(void)
{
    QTestState *q = start();
    const uint32_t pin = 1u << 14; /* GPIO 86: group 4, bit 14. */

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    qtest_writel(q, VIC0 + 0x10, 1u << 2);
    qtest_writel(q, SYSIC + 0x90, pin);
    qtest_writel(q, SYSIC + 0xd0, pin);
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 86, 1);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, VIC0), ==, 1u << 2);
    qtest_writel(q, SYSIC + 0xd0, 0);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xb0), ==, pin);
    qtest_writel(q, SYSIC + 0xd0, pin);
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0xb0, pin);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 86, 1);
    g_assert_false(qtest_get_irq(q, 0)); /* No new edge. */
    qtest_writel(q, SYSIC + 0xf0, pin); /* Active high level. */
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0xb0, pin);
    g_assert_true(qtest_get_irq(q, 0)); /* W1C cannot clear an active level. */
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 86, 0);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0x90, 0); /* Active low level. */
    g_assert_true(qtest_get_irq(q, 0));
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 86, 1);
    g_assert_false(qtest_get_irq(q, 0));
    qtest_quit(q);
}

static void test_pmu_hold(void)
{
    QTestState *q = start();
    const uint32_t pmu_irq = 1u << 3; /* GPIO 123: group 3, bit 3. */

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    qtest_writel(q, VIC0 + 0x10, pmu_irq);
    qtest_writel(q, SYSIC + 0x8c, 0);
    qtest_writel(q, SYSIC + 0xec, pmu_irq);
    qtest_writel(q, SYSIC + 0xcc, pmu_irq);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x87), ==, 2);

    /* A masked input change is still latched and visible in GPIOSTAT. */
    i2c_write_register(q, 0x73, 0x86, 2);
    qtest_qmp_assert_success(q, "{'execute':'qom-set', 'arguments':{"
                            "'path':'/machine', 'property':'hold',"
                            "'value':true}}");
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x87), ==, 0);
    g_assert_false(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 0x86, 0);
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0xac, pmu_irq);
    g_assert_true(qtest_get_irq(q, 0)); /* PMU still holds the pin low. */
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x85), ==, 2);
    g_assert_false(qtest_get_irq(q, 0));

    /* An unchanged input creates no event; either edge creates one. */
    qtest_qmp_assert_success(q, "{'execute':'qom-set', 'arguments':{"
                            "'path':'/machine', 'property':'hold',"
                            "'value':true}}");
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x85), ==, 0);
    qtest_qmp_assert_success(q, "{'execute':'qom-set', 'arguments':{"
                            "'path':'/machine', 'property':'hold',"
                            "'value':false}}");
    g_assert_true(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 0x85, 0xff);
    g_assert_true(qtest_get_irq(q, 0)); /* Event registers are read-only. */
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x85), ==, 2);
    g_assert_false(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 0x87, 0);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x87), ==, 2);

    /* Reset clears the event, but cannot move the physical switch. */
    qtest_qmp_assert_success(q, "{'execute':'qom-set', 'arguments':{"
                            "'path':'/machine', 'property':'hold',"
                            "'value':true}}");
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x87), ==, 0);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x85), ==, 0);
    qtest_quit(q);
}

static void test_gpio_data(void)
{
    QTestState *q = start();
    const uint32_t gpio = 0x3cf00000;

    /* Every bank, including E and F, must report its external inputs. */
    for (unsigned port = 0; port < 16; port++) {
        uint8_t pattern = 0xa5 ^ port;
        uint32_t base = gpio + port * 32;

        for (unsigned pin = 0; pin < 8; pin++) {
            qtest_set_irq_in(q, "/machine/soc/gpio", NULL, port * 8 + pin,
                             (pattern >> pin) & 1);
        }
        qtest_writel(q, base + 4, ~pattern);
        g_assert_cmphex(qtest_readl(q, base + 4), ==, pattern);
        /* Repeated reads must not toggle wheel-clock bits in input mode. */
        g_assert_cmphex(qtest_readl(q, base + 4), ==, pattern);
        qtest_writel(q, base, 0x11111111);
        g_assert_cmphex(qtest_readl(q, base + 4), ==, (uint8_t)~pattern);
    }

    /* Bit-banged wheel traffic may change E3/E5, but never E6/E7. */
    qtest_writel(q, gpio + 14 * 32, 0x00010100);
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 118, 1);
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 119, 1);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(q, gpio + 14 * 32 + 4) & 0xc0, ==, 0xc0);
    }

    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, gpio + 14 * 32), ==, 0);
    /* Physical input levels survive the controller reset. */
    g_assert_cmphex(qtest_readl(q, gpio + 14 * 32 + 4) & 0xc0, ==, 0xc0);
    qtest_quit(q);
}

static void test_gpio_output(void)
{
    QTestState *q = start();
    const uint32_t gpio = 0x3cf00000;

    qtest_irq_intercept_out(q, "/machine/soc/gpio");
    for (unsigned port = 0; port < 16; port++) {
        uint32_t base = gpio + port * 32;

        /* Writing the latch in input mode must not drive a GPIO output. */
        qtest_writel(q, base + 4, 0xff);
        for (unsigned pin = 0; pin < 8; pin++) {
            g_assert_false(qtest_get_irq(q, port * 8 + pin));
        }
        qtest_writel(q, base, 0x11111111);
        for (unsigned pin = 0; pin < 8; pin++) {
            g_assert_true(qtest_get_irq(q, port * 8 + pin));
        }
        /* Firmware saves/restores output values as E/F PCON nibbles. */
        qtest_writel(q, base, 0xefefefef);
        g_assert_cmphex(qtest_readl(q, base), ==, 0x11111111);
        g_assert_cmphex(qtest_readl(q, base + 4), ==, 0x55);
        for (unsigned pin = 0; pin < 8; pin++) {
            g_assert_cmpint(qtest_get_irq(q, port * 8 + pin), ==, !(pin & 1));
        }
        /* GPIOCMD changes one pin, preserving the other seven. */
        qtest_writel(q, gpio + 0x200, (port << 16) | 0x070f);
        g_assert_true(qtest_get_irq(q, port * 8 + 7));
        g_assert_cmphex(qtest_readl(q, base + 4), ==, 0xd5);
        qtest_writel(q, gpio + 0x200, (port << 16) | 0x0700);
        g_assert_false(qtest_get_irq(q, port * 8 + 7));
        qtest_writel(q, gpio + 0x200, (port << 16) | 0x0701);
        g_assert_true(qtest_get_irq(q, port * 8 + 7));
    }

    /* Invalid commands and reserved offsets cannot alias an existing port. */
    qtest_writel(q, gpio + 0x200, 0x190000);
    qtest_writel(q, gpio + 0x200, 0x0f080e);
    qtest_writel(q, gpio + 0x204, 0);
    qtest_writel(q, gpio + 0x1000, 0);
    g_assert_cmphex(qtest_readl(q, gpio + 15 * 32), ==, 0x11111111);
    g_assert_true(qtest_get_irq(q, 127));

    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    for (unsigned pin = 0; pin < 128; pin++) {
        g_assert_false(qtest_get_irq(q, pin));
    }
    qtest_quit(q);
}

static void test_gpio_state(void)
{
    QTestState *q = start();
    const uint32_t gpio = 0x3cf00000;
    g_autofree char *saved = NULL;
    g_autofree char *loaded = NULL;

    qtest_irq_intercept_out(q, "/machine/soc/gpio");
    qtest_writel(q, gpio + 15 * 32, 0xf0000000);
    qtest_writel(q, gpio + 14 * 32 + 0xc, 0x40);
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 119, 1);
    g_assert_true(qtest_get_irq(q, 127));
    saved = qtest_hmp(q, "savevm gpio-test");
    g_assert_cmpstr(saved, ==, "");

    qtest_writel(q, gpio + 15 * 32, 0xe0000000);
    qtest_writel(q, gpio + 14 * 32 + 0xc, 0);
    qtest_set_irq_in(q, "/machine/soc/gpio", NULL, 119, 0);
    g_assert_false(qtest_get_irq(q, 127));
    loaded = qtest_hmp(q, "loadvm gpio-test");
    g_assert_cmpstr(loaded, ==, "");
    g_assert_cmphex(qtest_readl(q, gpio + 15 * 32), ==, 0x10000000);
    g_assert_cmphex(qtest_readl(q, gpio + 15 * 32 + 4) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readl(q, gpio + 14 * 32 + 0xc), ==, 0x40);
    g_assert_cmphex(qtest_readl(q, gpio + 14 * 32 + 4) & 0x80, ==, 0x80);
    g_assert_true(qtest_get_irq(q, 127));
    qtest_quit(q);
}

static void pmu_rtc_assert(QTestState *q, const uint8_t expected[7])
{
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(i2c_read_register(q, 0x73, 0x59 + i), ==, expected[i]);
    }
}

static void pmu_rtc_write(QTestState *q, const uint8_t value[7])
{
    for (unsigned i = 0; i < 7; i++) {
        i2c_write_register(q, 0x73, 0x59 + i, value[i]);
    }
}

static void test_pmu_rtc(void)
{
    QTestState *q = start();
    const uint8_t initial[] = { 0x58, 0x59, 0x23, 3, 0x28, 2, 0x24 };
    const uint8_t leap[] = { 0, 0, 0, 4, 0x29, 2, 0x24 };
    const uint8_t written[] = { 0x59, 0x59, 0x23, 6, 0x28, 2, 0x23 };
    const uint8_t march[] = { 0, 0, 0, 0, 1, 3, 0x23 };
    const uint8_t century[] = { 0x59, 0x59, 0x23, 1, 0x31, 0x12, 0x99 };
    const uint8_t wrapped[] = { 0, 0, 0, 2, 1, 1, 0 };
    const uint8_t year_zero[] = { 0x59, 0x59, 0x23, 2, 0x28, 2, 0 };
    const uint8_t leap_zero[] = { 0, 0, 0, 3, 0x29, 2, 0 };

    i2c_write_register(q, 0x73, 7, 0xff);
    pmu_rtc_assert(q, initial);
    qtest_clock_set(q, 2000000000);
    pmu_rtc_assert(q, leap);
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x80);
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0);

    /* Writes take effect on the next second pulse, independently of STOP. */
    pmu_rtc_write(q, written);
    pmu_rtc_assert(q, leap);
    qtest_clock_set(q, 3000000000);
    pmu_rtc_assert(q, written);
    qtest_clock_set(q, 4000000000);
    pmu_rtc_assert(q, march); /* No leap day in 2023; independent weekday. */

    pmu_rtc_write(q, century);
    i2c_write_register(q, 0x73, 0x67, 0xa5);
    i2c_write_register(q, 0x73, 0x66, 0xaa);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x67), ==, 0xa5);
    g_assert_cmphex(i2c_read_register(q, 0x73, 0x66), ==, 0xaa);
    qtest_clock_set(q, 5000000000);
    /* A system reset also preserves pending writes. */
    pmu_rtc_assert(q, century);
    qtest_clock_set(q, 6000000000);
    pmu_rtc_assert(q, wrapped);

    /* The two-digit year 00 is a leap year after the century wrap. */
    pmu_rtc_write(q, year_zero);
    qtest_clock_set(q, 7000000000);
    pmu_rtc_assert(q, year_zero);
    qtest_clock_set(q, 8000000000);
    pmu_rtc_assert(q, leap_zero);
    qtest_quit(q);
}

static void test_pmu_alarm(void)
{
    QTestState *q = start();
    const uint32_t pmu_irq = 1u << 3;
    const uint8_t disabled[] = { 0x7f, 0x7f, 0x3f, 7, 0x3f, 0x1f, 0xff };
    const uint8_t alarm[] = { 1, 0, 0, 7, 0x29, 2, 0x24 };

    qtest_irq_intercept_in(q, "/machine/soc/cpu");
    qtest_writel(q, VIC0 + 0x10, pmu_irq);
    qtest_writel(q, SYSIC + 0x8c, 0);
    qtest_writel(q, SYSIC + 0xec, pmu_irq);
    qtest_writel(q, SYSIC + 0xcc, pmu_irq);
    i2c_write_register(q, 0x73, 7, 0xff);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(i2c_read_register(q, 0x73, 0x60 + i), ==, disabled[i]);
    }

    /* retailOS writes an invalid year first, then programs six BCD fields. */
    i2c_write_register(q, 0x73, 0x66, 0xaa);
    for (unsigned i = 0; i < 6; i++) {
        i2c_write_register(q, 0x73, 0x60 + i, alarm[i]);
    }
    qtest_clock_set(q, 3000000000); /* The other fields now match. */
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x80);
    g_assert_false(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 0x66, alarm[6]);
    g_assert_false(qtest_get_irq(q, 0)); /* Masked but latched. */
    i2c_write_register(q, 0x73, 7, 0xbf);
    g_assert_true(qtest_get_irq(q, 0));
    qtest_writel(q, SYSIC + 0xac, pmu_irq);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x40);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0);

    /* The alarm must remain latched after its matching second has ended. */
    i2c_write_register(q, 0x73, 0x60, 3);
    qtest_clock_set(q, 8000000000); /* 00:00:06, past the alarm at 00:00:03. */
    g_assert_true(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 2, 0); /* Event latch is not writable. */
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0xc0);
    g_assert_false(qtest_get_irq(q, 0));

    /* The periodic second event is independently masked and read-to-clear. */
    i2c_write_register(q, 0x73, 7, 0x7f);
    qtest_clock_set(q, 9000000000);
    g_assert_true(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x80);
    g_assert_false(qtest_get_irq(q, 0));
    i2c_write_register(q, 0x73, 7, 0xbf);

    /* All wildcards disable the comparator, including across a large jump. */
    i2c_write_register(q, 0x73, 0x66, 0xaa);
    for (unsigned i = 0; i < 7; i++) {
        i2c_write_register(q, 0x73, 0x60 + i, disabled[i]);
    }
    qtest_clock_set(q, 86400000000000LL);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x80);

    /* An impossible date must neither trigger nor cause an unbounded search. */
    i2c_write_register(q, 0x73, 0x66, 0xaa);
    i2c_write_register(q, 0x73, 0x64, 0x30);
    i2c_write_register(q, 0x73, 0x65, 2);
    i2c_write_register(q, 0x73, 0x66, 0xff);
    qtest_clock_set(q, 366LL * 86400 * 1000000000);
    g_assert_false(qtest_get_irq(q, 0));
    g_assert_cmphex(i2c_read_register(q, 0x73, 2), ==, 0x80);
    qtest_quit(q);
}

static void test_lcd_bounds(void)
{
    QTestState *q = start();

    lcd_command(q, 0x11);
    lcd_command(q, 0x29);
    /* A guest command cannot index beyond the 256-byte command space. */
    lcd_command(q, UINT32_MAX);
    qtest_writel(q, LCD + 0x40, 0x12345678);
    lcd_command(q, 0x2a);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(q, LCD + 0x40, 0xff);
    }
    lcd_command(q, 0x2c);
    qtest_writew(q, LCD + 0x40, 0xffff);
    lcd_assert_pixel(q, 0, 0, 0, 0, 0);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    lcd_command(q, 0x11);
    lcd_command(q, 0x29);
    lcd_command(q, 0x2c);
    qtest_writew(q, LCD + 0x40, 0xf800);
    lcd_assert_pixel(q, 0, 0, 248, 0, 0);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    rom = empty_image("s5l-rom-XXXXXX", 65536);
    nor = empty_image("s5l-nor-XXXXXX", 1024 * 1024);
    disk = empty_image("s5l-disk-XXXXXX", 16 * 1024 * 1024);
    qtest_add_func("/s5l8702/vic/priority", test_vic);
    qtest_add_func("/s5l8702/vic/cascade", test_vic_cascade);
    qtest_add_func("/s5l8702/i2s-stop", test_i2s_stop);
    qtest_add_func("/s5l8702/i2c", test_i2c);
    qtest_add_func("/s5l8702/i2c-stop", test_i2c_stop);
    qtest_add_func("/s5l8702/i2c-completion", test_i2c_completion);
    qtest_add_func("/s5l8702/i2c-cancel", test_i2c_cancel);
    qtest_add_func("/s5l8702/i2c-state", test_i2c_state);
    qtest_add_func("/s5l8702/i2c-gates", test_i2c_gates);
    qtest_add_func("/s5l8702/i2c-gated-state", test_i2c_gated_state);
    qtest_add_func("/s5l8702/codec-control", test_codec_control);
    qtest_add_func("/s5l8702/codec-clocks", test_codec_clocks);
    qtest_add_func("/s5l8702/codec-state", test_codec_state);
    qtest_add_func("/s5l8702/codec-pcm", test_codec_pcm);
    qtest_add_func("/s5l8702/codec-output-power", test_codec_output_power);
    qtest_add_func("/s5l8702/codec-output-state", test_codec_output_state);
    qtest_add_func("/s5l8702/codec-line-out", test_codec_line_out);
    qtest_add_func("/s5l8702/codec-host-backpressure",
                    test_codec_host_backpressure);
    qtest_add_func("/s5l8702/codec-ramp", test_codec_ramp);
    qtest_add_func("/s5l8702/codec-ramp-clock-state",
                    test_codec_ramp_clock_state);
    qtest_add_func("/s5l8702/codec-zero-cross", test_codec_zero_cross);
    qtest_add_func("/s5l8702/codec-zero-cross-timeout",
                    test_codec_zero_cross_timeout);
    qtest_add_func("/s5l8702/spi-fifo", test_spi_fifo);
    qtest_add_func("/s5l8702/spi-controls", test_spi_controls);
    qtest_add_func("/s5l8702/spi-nor", test_spi_nor);
    qtest_add_func("/s5l8702/spi-gate", test_spi_gate);
    qtest_add_func("/s5l8702/spi-state", test_spi_state);
    qtest_add_func("/s5l8702/remap", test_remap);
    qtest_add_func("/s5l8702/cpu-reset", test_cpu_reset);
    qtest_add_func("/s5l8702/timers", test_timers);
    qtest_add_func("/s5l8702/clocks", test_clocks);
    qtest_add_func("/s5l8702/clock-state", test_clock_state);
    qtest_add_func("/s5l8702/wheel", test_wheel);
    qtest_add_func("/s5l8702/dma-gpio", test_dma_gpio);
    qtest_add_func("/s5l8702/pl080-widths", test_pl080_widths);
    qtest_add_func("/s5l8702/pl080-lli", test_pl080_lli);
    qtest_add_func("/s5l8702/pl080-errors", test_pl080_errors);
    qtest_add_func("/s5l8702/pl080-invalid", test_pl080_invalid);
    qtest_add_func("/s5l8702/pl080-state", test_pl080_state);
    qtest_add_func("/s5l8702/pl080-requests", test_pl080_requests);
    qtest_add_func("/s5l8702/pl080-single-pack", test_pl080_single_pack);
    qtest_add_func("/s5l8702/pl080-halt-fifo", test_pl080_halt_fifo);
    qtest_add_func("/s5l8702/pl080-fifo-state", test_pl080_fifo_state);
    qtest_add_func("/s5l8702/pl080-bounded", test_pl080_bounded);
    qtest_add_func("/s5l8702/tvo-controls", test_tvo_controls);
    qtest_add_func("/s5l8702/i2s-dma-clock", test_i2s_dma_clock);
    qtest_add_func("/s5l8702/sm1-memory-descriptors",
                   test_sm1_memory_descriptors);
    qtest_add_func("/s5l8702/i2s-clock-gate", test_i2s_clock_gate);
    qtest_add_func("/s5l8702/i2s-tx-state", test_i2s_tx_state);
    qtest_add_func("/s5l8702/aes", test_aes);
    qtest_add_func("/s5l8702/sha", test_sha);
    qtest_add_func("/s5l8702/sha-dma", test_sha_dma);
    qtest_add_func("/s5l8702/display", test_display);
    qtest_add_func("/s5l8702/lcd-bounds", test_lcd_bounds);
    qtest_add_func("/s5l8702/lcd-te", test_lcd_te);
    qtest_add_func("/s5l8702/gpio-irq", test_gpio_irq);
    qtest_add_func("/s5l8702/pmu-hold", test_pmu_hold);
    qtest_add_func("/s5l8702/gpio-data", test_gpio_data);
    qtest_add_func("/s5l8702/gpio-output", test_gpio_output);
    qtest_add_func("/s5l8702/gpio-state", test_gpio_state);
    qtest_add_func("/s5l8702/pmu-rtc", test_pmu_rtc);
    qtest_add_func("/s5l8702/pmu-alarm", test_pmu_alarm);
    result = g_test_run();
    unlink(rom);
    unlink(nor);
    unlink(disk);
    g_free(rom);
    g_free(nor);
    g_free(disk);
    return result;
}
