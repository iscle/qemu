#ifndef HW_MISC_S5L8702_SHA_H
#define HW_MISC_S5L8702_SHA_H

#include "qom/object.h"
#include "hw/sysbus.h"

#define TYPE_S5L8702_SHA    "s5l8702-sha"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702ShaState, S5L8702_SHA)

#define S5L8702_SHA_BASE    0x38000000
#define S5L8702_SHA_SIZE    0x00100000

struct S5L8702ShaState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    qemu_irq irq;
    uint32_t config;
    uint32_t status;
    uint32_t input[16];
    uint32_t digest[5];
    uint32_t dma_control;
    uint32_t dma_source;
    uint32_t dma_length;
};

#endif /* HW_MISC_S5L8702_SHA_H */
