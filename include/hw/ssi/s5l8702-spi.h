#ifndef HW_SSI_S5L8702_SPI_H
#define HW_SSI_S5L8702_SPI_H

#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/clock.h"
#include "qemu/fifo8.h"

#define TYPE_S5L8702_SPI    "s5l8702-spi"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702SpiState, S5L8702_SPI)

#define S5L8702_SPI0_BASE   0x3c300000
#define S5L8702_SPI1_BASE   0x3ce00000
#define S5L8702_SPI2_BASE   0x3d200000
#define S5L8702_SPI_SIZE    0x00100000
#define S5L8702_SPI_FIFO_SIZE 16

struct S5L8702SpiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    SSIBus *spi;
    Clock *pclk;
    Fifo8 tx_fifo;
    Fifo8 rx_fifo;

    uint32_t spictrl;
    uint32_t spisetup;
    uint32_t spistatus;
    uint32_t spipin;
    uint32_t spitxdata;
    uint32_t spiclkdiv;
    uint32_t spirxlimit;
    uint32_t rx_remaining;
};

#endif /* HW_SSI_S5L8702_SPI_H */
