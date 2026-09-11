#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "hw/qdev-clock.h"
#include "hw/arm/s5l8702.h"
#include "hw/misc/unimp.h"
#include "hw/arm/exynos4210.h"
#include "hw/misc/s5l8702-usbotg.h"
#include "hw/misc/s5l8702-usbphy.h"
#include "hw/misc/s5l8702-sysic.h"
#include "trace.h"


static void s5l8702_miu_remap(void *opaque, int n, int level)
{
    S5L8702State *s = opaque;

    memory_region_transaction_begin();
    for (unsigned i = 0; i < 2; i++) {
        memory_region_set_enabled(&s->iram_alias[i], level);
    }
    memory_region_transaction_commit();
}

static void s5l8702_init(Object *obj) {
    S5L8702State *s = S5L8702(obj);

    trace_s5l8702_init();

    object_initialize_child(obj, "cpu", &(s->cpu), ARM_CPU_TYPE_NAME("arm926"));

    /* The board oscillator used by the firmware's 216 MHz PLL2 setup. */
    object_initialize_child(obj, "osc0", &s->osc0, TYPE_CLOCK);
    clock_setup_canonical_path(&s->osc0);
    clock_set_hz(&s->osc0, 12000000);

    object_initialize_child(obj, "vic0", &s->vic[0], TYPE_PL192);
    object_initialize_child(obj, "vic1", &s->vic[1], TYPE_PL192);

    /*
     * EXTCLK: Ext. Clock 0 is unconnected on this board; Ext. Clock 1 is the
     * 32768 Hz oscillator (OSC1). Matches the reference clock-source table.
     */
    object_initialize_child(obj, "extclk0", &s->extclk0, TYPE_CLOCK);
    clock_setup_canonical_path(&s->extclk0);
    object_initialize_child(obj, "extclk1", &s->extclk1, TYPE_CLOCK);
    clock_setup_canonical_path(&s->extclk1);
    clock_set_hz(&s->extclk1, 32768);
    
    object_initialize_child(obj, "clk", &s->clk, TYPE_S5L8702_CLK);
    object_initialize_child(obj, "aes", &s->aes, TYPE_S5L8702_AES);
    object_initialize_child(obj, "sha", &s->sha, TYPE_S5L8702_SHA);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_S5L8702_GPIO);

    for (uint32_t i = 0; i < ARRAY_SIZE(s->spi); i++) {
        object_initialize_child(obj, "spi[*]", &s->spi[i], TYPE_S5L8702_SPI);
    }

    for (uint32_t i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_S5L8702_I2C);
    }

    object_initialize_child(obj, "timer", &s->timer, TYPE_S5L8702_TIMER);
    object_initialize_child(obj, "lcd", &s->lcd, TYPE_S5L8702_LCD);
    object_initialize_child(obj, "disp", &s->disp, TYPE_S5L8702_DISP);
    object_initialize_child(obj, "tvo", &s->tvo, TYPE_S5L8702_TVO);
    object_initialize_child(obj, "jpeg", &s->jpeg, TYPE_S5L8702_JPEG);

    for (uint32_t i = 0; i < ARRAY_SIZE(s->dma); i++) {
        object_initialize_child(obj, "dma[*]", &s->dma[i], TYPE_PL080);
    }
    
    object_initialize_child(obj, "ata", &s->ata, TYPE_S5L8702_ATA);
    object_initialize_child(obj, "clickwheel", &s->clickwheel, TYPE_S5L8702_CLICKWHEEL);
    object_initialize_child(obj, "chipid", &s->chipid, TYPE_S5L8702_CHIPID);
    object_initialize_child(obj, "nand", &s->nand, TYPE_S5L8702_NAND);
    object_initialize_child(obj, "nand_ecc", &s->nand_ecc, TYPE_S5L8702_NAND_ECC);
    object_initialize_child(obj, "miu", &s->miu, TYPE_S5L8702_MIU);
    object_initialize_child(obj, "usbotg", &s->usbotg, TYPE_S5L8702_USBOTG);
    object_initialize_child(obj, "usbphy", &s->usbphy, TYPE_S5L8702_USBPHY);
    object_initialize_child(obj, "sysic", &s->sysic, TYPE_S5L8702_SYSIC);
    object_initialize_child(obj, "rng", &s->rng, TYPE_S5L8702_RNG);
    object_initialize_child(obj, "i2s", &s->i2s, TYPE_S5L8702_I2S);
    qdev_alias_clock(DEVICE(&s->i2s), "lrck", DEVICE(obj), "i2s0-lrck");
    object_initialize_child(obj, "sm1", &s->sm1, TYPE_S5L8702_SM1);
}

static qemu_irq s5l8702_get_irq(S5L8702State *s, unsigned irq)
{
    return s->irq[irq / 32][irq % 32];
}

static void s5l8702_realize(DeviceState *dev, Error **errp) {
    S5L8702State *s = S5L8702(dev);
    MemoryRegion *system_memory = get_system_memory();

    trace_s5l8702_realize();
    qdev_init_gpio_in_named(dev, s5l8702_miu_remap, "miu-remap", 1);

    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);
    /* The CPU has no parent bus, so the system bus reset cannot reach it. */
    qemu_register_reset(resettable_cold_reset_fn, &s->cpu);

    /* VIC */
    s->vic[1].daisy = &s->vic[0];
    s->vic[0].daisy_callback = &s->vic[1];
    for (unsigned bank = 0; bank < 2; bank++) {
        SysBusDevice *vic = SYS_BUS_DEVICE(&s->vic[bank]);

        sysbus_realize(vic, &error_fatal);
        sysbus_mmio_map(vic, 0, S5L8702_VIC0_MEM_BASE + bank * 0x1000);
        for (unsigned i = 0; i < 32; i++) {
            s->irq[bank][i] = qdev_get_gpio_in(DEVICE(vic), i);
        }
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vic[0]), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vic[0]), 1,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_FIQ));

    /* Peripheral status registers hold interrupt levels until acknowledged. */
    create_unimplemented_device("s5l8702-irq-edge", 0x38e02000, 0x10);

    /* CLK */
    qdev_connect_clock_in(DEVICE(&s->clk), "osc0", &s->osc0);
    qdev_connect_clock_in(DEVICE(&s->clk), "osc1", &s->extclk1);
    sysbus_realize(SYS_BUS_DEVICE(&s->clk), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clk), 0, S5L8702_CLK_BASE);

    /* AES */
    sysbus_realize(SYS_BUS_DEVICE(&s->aes), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aes), 0, S5L8702_AES_BASE);

    /* SHA */
    sysbus_realize(SYS_BUS_DEVICE(&s->sha), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sha), 0, S5L8702_SHA_BASE);
    /* Boot ROM 0x20002030 installs the SHA completion ISR on source 40. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sha), 0,
                       s5l8702_get_irq(s, 40));

    /* GPIO */
    sysbus_realize(SYS_BUS_DEVICE(&s->gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, S5L8702_GPIO_BASE);

    /* SPI */
    for (uint32_t i = 0; i < ARRAY_SIZE(s->spi); i++) {
        qdev_connect_clock_in(DEVICE(&s->spi[i]), "pclk", s->clk.spi_pclk[i]);
        sysbus_realize(SYS_BUS_DEVICE(&s->spi[i]), &error_fatal);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi[0]), 0, S5L8702_SPI0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi[1]), 0, S5L8702_SPI1_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi[2]), 0, S5L8702_SPI2_BASE);

    /* I2C */
    for (uint32_t i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        qdev_connect_clock_in(DEVICE(&s->i2c[i]), "pclk", s->clk.i2c_pclk[i]);
        sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), &error_fatal);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[0]), 0, S5L8702_I2C0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[1]), 0, S5L8702_I2C1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[0]), 0,
                       s5l8702_get_irq(s, S5L8702_I2C0_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[1]), 0,
                       s5l8702_get_irq(s, S5L8702_I2C1_IRQ));

    /* Timer */
    qdev_connect_clock_in(DEVICE(&s->timer), "pclk", s->clk.timer_pclk);
    qdev_connect_clock_in(DEVICE(&s->timer), "eclk", s->clk.timer_eclk);
    qdev_connect_clock_in(DEVICE(&s->timer), "extclk0", &s->extclk0);
    qdev_connect_clock_in(DEVICE(&s->timer), "extclk1", &s->extclk1);
    sysbus_realize(SYS_BUS_DEVICE(&s->timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 0, S5L8702_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), 0,
                       s5l8702_get_irq(s, S5L8702_TIMER_IRQ_16BIT));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), 1,
                       s5l8702_get_irq(s, S5L8702_TIMER_IRQ_32BIT));

    /* LCD */
    s->lcd.disp = &s->disp;
    sysbus_realize(SYS_BUS_DEVICE(&s->lcd), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lcd), 0, S5L8702_LCD_BASE);

    /*
     * Display pipe / video compositor (0x38900000). The LCD frame engine
     * presents whatever this block composes from the layer descriptors.
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->disp), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 0, S5L8702_DISP_BASE);

    /* Partial external display model: boot background and sleep controls. */
    sysbus_realize(SYS_BUS_DEVICE(&s->tvo), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tvo), 0, S5L8702_TVO_BACKGROUND);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tvo), 1, S5L8702_TVO_OUTPUT_CONTROL);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tvo), 2 + i,
                       S5L8702_TVO_RUN_BASE + i * 0x100000);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tvo), 5, S5L8702_TVO_ENCODER_CONFIG);

    /* JPEG */
    s->jpeg.nsas = cpu_get_address_space(CPU(&s->cpu), ARMASIdx_NS);
    sysbus_realize(SYS_BUS_DEVICE(&s->jpeg), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->jpeg), 0, S5L8702_JPEG_BASE);

    /* DMA */
    for (uint32_t i = 0; i < ARRAY_SIZE(s->dma); i++) {
        object_property_set_link(OBJECT(&s->dma[i]), "downstream", OBJECT(get_system_memory()), &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(&s->dma[i]), &error_fatal);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma[0]), 0, S5L8702_DMA0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma[1]), 0, S5L8702_DMA1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma[0]), 0,
                       s5l8702_get_irq(s, 16));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma[1]), 0,
                       s5l8702_get_irq(s, 17));

    /* ATA */
    sysbus_realize(SYS_BUS_DEVICE(&s->ata), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ata), 0, S5L8702_ATA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ata), 0,
                       s5l8702_get_irq(s, S5L8702_IRQ_ATA));

    /* Clickwheel controller */
    sysbus_realize(SYS_BUS_DEVICE(&s->clickwheel), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clickwheel), 0, S5L8702_CLICKWHEEL_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->clickwheel), 0,
                       s5l8702_get_irq(s, S5L8702_CWHEEL_IRQ));

    /* ChipID */
    sysbus_realize(SYS_BUS_DEVICE(&s->chipid), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->chipid), 0, S5L8702_CHIPID_BASE);

    /* NAND Flash Controller */
    sysbus_realize(SYS_BUS_DEVICE(&s->nand), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->nand), 0, S5L8702_NAND_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->nand), 0,
                       s5l8702_get_irq(s, S5L8702_NAND_IRQ));

    /* NAND ECC Engine */
    sysbus_realize(SYS_BUS_DEVICE(&s->nand_ecc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->nand_ecc), 0, S5L8702_NAND_ECC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->nand_ecc), 0,
                       s5l8702_get_irq(s, S5L8702_NAND_ECC_IRQ));

    /* BootROM */
    memory_region_init_ram(&s->brom, OBJECT(dev), "s5l8702.bootrom", S5L8702_BOOTROM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, S5L8702_BOOTROM_BASE_ADDR, &s->brom);
    memory_region_init_alias(&s->brom_alias, OBJECT(dev), "s5l8702.bootrom-alias", &s->brom, 0, S5L8702_BOOTROM_SIZE);
    memory_region_add_subregion(system_memory, S5L8702_BASE_BOOT_ADDR, &s->brom_alias);

    /* IRAM0 */
    memory_region_init_ram(&s->iram0, OBJECT(dev), "s5l8702.iram0", S5L8702_IRAM0_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, S5L8702_IRAM0_BASE_ADDR, &s->iram0);

    /* IRAM1 */
    memory_region_init_ram(&s->iram1, OBJECT(dev), "s5l8702.iram1", S5L8702_IRAM1_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, S5L8702_IRAM1_BASE_ADDR, &s->iram1);

    for (unsigned i = 0; i < 2; i++) {
        memory_region_init_alias(&s->iram_alias[i], OBJECT(dev),
                                  "s5l8702.iram-alias",
                                  i ? &s->iram1 : &s->iram0,
                                  0, S5L8702_IRAM0_SIZE);
        memory_region_set_enabled(&s->iram_alias[i], false);
        memory_region_add_subregion_overlap(system_memory,
                                             i * S5L8702_IRAM0_SIZE,
                                             &s->iram_alias[i], 1);
    }

    /* UART */
    exynos4210_uart_create(S5L8702_UART0_MEM_BASE, 256, 0, serial_hd(0),
                           s5l8702_get_irq(s, S5L8702_IRQ_UART0));
    exynos4210_uart_create(S5L8702_UART1_MEM_BASE, 256, 0, serial_hd(1),
                           s5l8702_get_irq(s, S5L8702_IRQ_UART1));
    exynos4210_uart_create(S5L8702_UART2_MEM_BASE, 256, 0, serial_hd(2),
                           s5l8702_get_irq(s, S5L8702_IRQ_UART2));
    exynos4210_uart_create(S5L8702_UART3_MEM_BASE, 256, 0, serial_hd(3),
                           s5l8702_get_irq(s, S5L8702_IRQ_UART3));
    exynos4210_uart_create(S5L8702_UART4_MEM_BASE, 256, 0, serial_hd(4),
                           s5l8702_get_irq(s, S5L8702_IRQ_UART4));

    /* MIU */
    sysbus_realize(SYS_BUS_DEVICE(&s->miu), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->miu), 0, S5L8702_MIU_BASE);

    qdev_connect_gpio_out_named(DEVICE(&s->miu), "remap", 0,
                                qdev_get_gpio_in_named(dev, "miu-remap", 0));

    /* USB OTG */
    sysbus_realize(SYS_BUS_DEVICE(&s->usbotg), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbotg), 0, S5L8702_USBOTG_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usbotg), 0,
                       s5l8702_get_irq(s, S5L8702_IRQ_USBOTG));

    /* USB PHY */
    sysbus_realize(SYS_BUS_DEVICE(&s->usbphy), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbphy), 0, S5L8702_USBPHY_BASE);

    /*
     * GPIO interrupt groups run in reverse order. In retailOS 2.0.4,
     * 0x080cd8d4..0x080cd8f8 dispatch VIC 33, 3, 2, 1, 0 to groups
     * 0, 3, 4, 5, 6. Groups 1 and 2 have no registered firmware handler.
     */
    static const unsigned sysic_irq[] = { 33, 32, 31, 3, 2, 1, 0 };

    sysbus_realize(SYS_BUS_DEVICE(&s->sysic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysic), 0, S5L8702_SYSIC_BASE);
    for (unsigned grp = 0; grp < ARRAY_SIZE(sysic_irq); grp++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sysic), grp,
                           s5l8702_get_irq(s, sysic_irq[grp]));
    }
    for (unsigned pin = 0; pin < S5L8702_GPIO_PINS; pin++) {
        qdev_connect_gpio_out_named(DEVICE(&s->gpio), "input-irq", pin,
                                   qdev_get_gpio_in(DEVICE(&s->sysic), pin));
    }
    /* 0x083602c0 supplies GPIO 55 to the panel update callback registration. */
    qdev_connect_gpio_out_named(DEVICE(&s->lcd), "te", 0,
                               qdev_get_gpio_in(DEVICE(&s->gpio), 55));

    /*
     * NOTE: previously we forced USB "connected" at boot and raised the SYSIC
     * USB GPIO IRQ, on the theory that Disk Mode is entered over USB. But with
     * USB asserted, retailOS shows the boot logo and then waits in a
     * "connected to host" path instead of continuing to mount storage and
     * compose the menu UI. The working reference (qemu-ios) asserts no
     * USB-connected GPIO and boots straight to the UI. Gate it behind an env
     * var so Disk Mode experiments can still force it, but leave it OFF by
     * default so the normal retailOS boot can reach the menu.
     */
    if (getenv("IPOD_USB_CONNECTED")) {
        s->sysic.usb_connected = true;
        s->sysic.gpio_int_status[S5L8702_SYSIC_USB_GPIO_GROUP] |=
            (1 << S5L8702_SYSIC_USB_GPIO_PIN);
        s->sysic.gpio_int_enabled[S5L8702_SYSIC_USB_GPIO_GROUP] |=
            (1 << S5L8702_SYSIC_USB_GPIO_PIN);
        qemu_irq_raise(s->sysic.gpio_irqs[S5L8702_SYSIC_USB_GPIO_GROUP]);
    }

    /*
     * RNG (0x3C100000): a plain LCG. Without it the diskless boot spins for
     * ever on the first draw -- a black screen -- because there is no enable
     * bit and the ready poll has no timeout.
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->rng), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rng), 0, S5L8702_RNG_BASE);

    /* retailOS selects DMAC0 request 10 for the I2S0 TX data register. */
    qdev_connect_clock_in(DEVICE(&s->i2s), "pclk",
                          qdev_get_clock_out(DEVICE(&s->clk), "i2s0-pclk"));
    qdev_connect_gpio_out_named(DEVICE(&s->i2s), "tx-dreq", 0,
                                qdev_get_gpio_in_named(DEVICE(&s->dma[0]),
                                                       "dreq-burst", 10));
    qdev_connect_gpio_out_named(DEVICE(&s->dma[0]), "dreq-clear", 10,
                                qdev_get_gpio_in_named(DEVICE(&s->i2s),
                                                       "tx-clear", 0));
    sysbus_realize(SYS_BUS_DEVICE(&s->i2s), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2s), 0, S5L8702_I2S_BASE);

    /* Partial SM1 engine used by the retailOS audio bring-up. */
    sysbus_realize(SYS_BUS_DEVICE(&s->sm1), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sm1), 0, S5L8702_SM1_BASE);

    create_unimplemented_device("wdt", 0x3c800000, 0x100000);
}

static void s5l8702_unrealize(DeviceState *dev)
{
    S5L8702State *s = S5L8702(dev);

    qemu_unregister_reset(resettable_cold_reset_fn, &s->cpu);
}

static void s5l8702_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    trace_s5l8702_class_init();

    dc->realize = s5l8702_realize;
    dc->unrealize = s5l8702_unrealize;
}

static const TypeInfo s5l8702_types[] = {
    {
        .name = TYPE_S5L8702,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702State),
        .instance_init = s5l8702_init,
        .class_init = s5l8702_class_init,
    },
};
DEFINE_TYPES(s5l8702_types);
