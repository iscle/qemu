/* Samsung S5L8702 I2C master. */
#ifndef HW_I2C_S5L8702_I2C_H
#define HW_I2C_S5L8702_I2C_H
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/clock.h"
#include "qemu/timer.h"
#include "qom/object.h"
#define TYPE_S5L8702_I2C "s5l8702-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702I2cState, S5L8702_I2C)
#define S5L8702_I2C0_BASE 0x3c600000
#define S5L8702_I2C1_BASE 0x3c900000
#define S5L8702_I2C0_IRQ 21
#define S5L8702_I2C1_IRQ 22
#define S5L8702_I2C_MEM_SIZE   0x24

#define S5L8702_IICCON     0x00
#define S5L8702_IICSTAT    0x04
#define S5L8702_IICADD     0x08
#define S5L8702_IICDS      0x0c
#define S5L8702_IICUNK10   0x10
#define S5L8702_IICUNK14   0x14
#define S5L8702_IICUNK18   0x18
#define S5L8702_IICSTAT2   0x20

#define S5L8702_IICCON_CK_REG_MASK  0x0f
#define S5L8702_IICCON_GO           (1u << 4)
#define S5L8702_IICCON_CKSEL        (1u << 6)
#define S5L8702_IICCON_ACK_GEN      (1u << 7)
#define S5L8702_IICCON_INT_MASK     0x3f00

#define S5L8702_IICSTAT_LRB         (1u << 0)
#define S5L8702_IICSTAT_SOE         (1u << 4)
#define S5L8702_IICSTAT_BUSY        (1u << 5)
#define S5L8702_IICSTAT_MODE_MASK   0xf0
#define S5L8702_IICSTAT_START_TX    0xf0
#define S5L8702_IICSTAT_START_RX    0xb0
#define S5L8702_IICSTAT_STOP_TX     0xd0
#define S5L8702_IICSTAT_STOP_RX     0x90

#define S5L8702_IICSTAT2_BYTE       (1u << 8)
#define S5L8702_IICSTAT2_START      (1u << 12)
#define S5L8702_IICSTAT2_STOP       (1u << 13)

/* Provisional byte duration; clock source/divider timing is not modeled. */
#define S5L8702_I2C_BYTE_US   25

#define S5L8702_I2C_BOOTROM_CON   0x184

struct S5L8702I2cState {
    SysBusDevice parent_obj;


    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    QEMUTimer *byte_timer;
    Clock *pclk;
    uint32_t remaining_ns;

    uint32_t iiccon;
    uint32_t iicstat;
    uint32_t iicadd;
    uint32_t iicds;
    uint32_t iicunk14;
    uint32_t iicunk18;
    uint32_t iicstat2;

    bool byte_done;
    bool pending;

    bool in_address;
    bool receive;
    bool nack;
    uint8_t tx_byte;
};

#endif
