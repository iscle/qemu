#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/registerfields.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "qom/object.h"
#include "hw/arm/ipod-classic.h"
#include "hw/misc/s5l8702-cs42l55.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "trace.h"

static char *ipod_classic_get_bootrom_path(Object *obj, Error **errp)
{
    IpodClassicState *s = IPOD_CLASSIC_MACHINE(obj);
    return g_strdup(s->bootrom_path);
}

static void ipod_classic_set_bootrom_path(Object *obj, const char *value,
                                          Error **errp)
{
    IpodClassicState *s = IPOD_CLASSIC_MACHINE(obj);
    g_free(s->bootrom_path);
    s->bootrom_path = g_strdup(value);
}

static bool ipod_classic_get_hold(Object *obj, Error **errp)
{
    IpodClassicState *s = IPOD_CLASSIC_MACHINE(obj);

    return s->hold_input ? !s->pcf5063x.gpio2_high : s->hold;
}

static void ipod_classic_set_hold(Object *obj, bool value, Error **errp)
{
    IpodClassicState *s = IPOD_CLASSIC_MACHINE(obj);

    s->hold = value;
    if (s->hold_input) {
        qemu_set_irq(s->hold_input, !value);
    }
}

static void ipod_classic_init(Object *obj)
{
    trace_ipod_classic_init();

    if (!object_property_add_str(obj, "bootrom", ipod_classic_get_bootrom_path, ipod_classic_set_bootrom_path)) {
        error_report("ipod_classic_init: failed to add bootrom property\n");
        exit(1);
    }
    object_property_add_bool(obj, "hold", ipod_classic_get_hold,
                             ipod_classic_set_hold);
    object_property_set_description(obj, "hold",
                                    "Position of the physical hold switch");
}

static void ipod_classic_machine_init(MachineState *machine)
{
    IpodClassicState *s = IPOD_CLASSIC_MACHINE(machine);

    trace_ipod_classic_machine_init();

    /* BIOS is not supported by this board */
    if (machine->firmware) {
        error_report("BIOS not supported for this machine");
        exit(1);
    }

    /* Only allow ARM926 for this board */
    if (strcmp(machine->cpu_type, ARM_CPU_TYPE_NAME("arm926")) != 0) {
        error_report("This board can only be used with arm926 CPU");
        exit(1);
    }

    /* This board has fixed size RAM (64MiB) */
    if (machine->ram_size != 64 * MiB) {
        error_report("This machine can only be used with 64MiB RAM");
        exit(1);
    }

    /* Only allow 1 CPU for this board */
    if (machine->smp.cpus != 1) {
        error_report("This machine can only be used with 1 CPU");
        exit(1);
    }

    if (!s->bootrom_path) {
        error_report("bootrom property not set");
        exit(1);
    }

    /* Initialize s5l8702 soc */
    object_initialize_child(OBJECT(s), "soc", &s->soc, TYPE_S5L8702);
    object_initialize_child(OBJECT(s), "codec", &s->codec,
                            TYPE_S5L8702_CS42L55);
    object_property_set_link(OBJECT(&s->soc.i2s), "codec", OBJECT(&s->codec),
                             &error_fatal);
    qdev_connect_clock_in(DEVICE(&s->soc), "i2s0-lrck",
                          qdev_get_clock_out(DEVICE(&s->codec), "lrck"));
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /*
     * Force DFU (USB recovery) mode on boot when IPOD_DFU is set. The bootrom
     * checks this GPIO input (paired with the USB PHY reg set in usbphy reset)
     * to enter DFU instead of booting osos -- the entry point for a from-
     * scratch USB restore.
     */
    if (getenv("IPOD_DFU")) {
        s->soc.gpio.pdat[1] = 0x01;
    }

    /* DRAM */
    memory_region_init_ram(&s->dram, OBJECT(s), "dram", machine->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), S5L8702_DRAM_BASE_ADDR, &s->dram);

    memory_region_init_alias(&s->dram_alias, OBJECT(s), "dram-alias", &s->dram, 0, machine->ram_size);
    memory_region_add_subregion(get_system_memory(), 0x88000000, &s->dram_alias);

    /* Connect an SPI flash to SPI0 */
    DeviceState *flash_dev = qdev_new("sst25vf080b"); // According to https://freemyipod.org/wiki/Classic_3G
    DriveInfo *flash_info = drive_get_by_index(IF_MTD, 0);
    if (!flash_info) {
        error_report("NOR image not found");
        exit(1);
    }

    trace_ipod_classic_nor_loaded();
    qdev_prop_set_drive(flash_dev, "drive", blk_by_legacy_dinfo(flash_info));
    qdev_realize_and_unref(flash_dev, BUS(s->soc.spi[0].spi), &error_fatal);

    qemu_irq flash_cs = qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out(DEVICE(&s->soc.gpio), 0, flash_cs);

    /* Connect an IDE HDD to s5l8702-ata */
    DriveInfo *hdd_info = drive_get_by_index(IF_IDE, 0);
    if (!hdd_info) {
        error_report("HDD image not found");
        exit(1);
    }
    s5l8702_ata_set_drive_info(&s->soc.ata, hdd_info);
    
    /* PCF5063x and the active-low hold switch on its GPIO2 input. */
    object_initialize_child(OBJECT(s), "pmu", &s->pcf5063x, TYPE_PCF5063X);
    DeviceState *pmu = DEVICE(&s->pcf5063x);

    qdev_prop_set_uint8(pmu, "address", 0x73);
    qdev_realize(pmu, BUS(s->soc.i2c[0].bus), &error_fatal);
    /* retailOS 0x080559ac masks GPIO 123 while its PMU task handles events. */
    qdev_connect_gpio_out(pmu, 0,
                         qdev_get_gpio_in(DEVICE(&s->soc.gpio), 123));
    s->hold_input = qdev_get_gpio_in_named(pmu, "gpio2", 0);
    qemu_set_irq(s->hold_input, !s->hold);

    /* retailOS 0x0809f1c8 configures clock group 6 before codec setup. */
    qdev_prop_set_uint8(DEVICE(&s->codec), "address", 0x4a);
    qdev_connect_clock_in(DEVICE(&s->codec), "mclk",
                          qdev_get_clock_out(DEVICE(&s->soc.clk),
                                              "codec-mclk"));
    qdev_realize(DEVICE(&s->codec), BUS(s->soc.i2c[0].bus), &error_fatal);

    /* Read the bootrom, copy it to memory and execute it */
    g_autofree uint8_t *bootrom = NULL;
    size_t bootrom_size = 0;
    if (g_file_get_contents(s->bootrom_path, (char **) &bootrom, &bootrom_size, NULL)) {
        if (bootrom_size != S5L8702_BOOTROM_SIZE) {
            error_report("Boot ROM must be exactly 64 KiB");
            exit(1);
        }
        trace_ipod_classic_bootrom_read(s->bootrom_path);
        AddressSpace *nsas = cpu_get_address_space(CPU(&s->soc.cpu), ARMASIdx_NS);
        address_space_write(nsas, 0x20000000, MEMTXATTRS_UNSPECIFIED, bootrom, bootrom_size);
        trace_ipod_classic_bootrom_copied(bootrom_size);
    } else {
        error_report("Failed to read bootrom from %s", s->bootrom_path);
        exit(1);
    }
}

static void ipod_classic_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = ipod_classic_machine_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = 64 * MiB;
    mc->default_cpus = 1;
};

static const TypeInfo ipod_classic_types[] = {
    {
        .name = TYPE_IPOD_CLASSIC_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(IpodClassicState),
        .instance_init = ipod_classic_init,
        .class_init = ipod_classic_class_init,
    },
};
DEFINE_TYPES(ipod_classic_types)
