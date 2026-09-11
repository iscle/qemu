/*
 * Samsung S5L8702 JPEG decoder (as used in iPod Classic).
 *
 * This is a partial emulation tailored to the Apple OF boot-time JPEG
 * decode path. The implementation has known limitations:
 *   - Image geometry is hard-coded to 320x240, 4:2:0 chroma subsampling.
 *   - The hardware's incremental, block-at-a-time decode is faked by
 *     running a full software JPEG decode on the first JPEG_REG_CTRL
 *     write and staging one MCU per subsequent trigger.
 *
 * This file is licensed under the GNU GPL, version 2 or later.
 */
#ifndef HW_MISC_S5L8702_JPEG_H
#define HW_MISC_S5L8702_JPEG_H

#include "qom/object.h"
#include "hw/sysbus.h"
#include "exec/memory.h"

#define TYPE_S5L8702_JPEG       "s5l8702-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702JpegState, S5L8702_JPEG)

/* MMIO base address and region size. */
#define S5L8702_JPEG_BASE       0x39600000
#define S5L8702_JPEG_SIZE       0x00100000

/* Image geometry (hardware path is hard-coded to these). */
#define S5L8702_JPEG_IMG_WIDTH       320
#define S5L8702_JPEG_IMG_HEIGHT      240
#define S5L8702_JPEG_MCU_SIZE        16     /* 16x16 luma per MCU */
#define S5L8702_JPEG_MCUS_X          (S5L8702_JPEG_IMG_WIDTH  / S5L8702_JPEG_MCU_SIZE)
#define S5L8702_JPEG_MCUS_Y          (S5L8702_JPEG_IMG_HEIGHT / S5L8702_JPEG_MCU_SIZE)
#define S5L8702_JPEG_NUM_MCUS        (S5L8702_JPEG_MCUS_X * S5L8702_JPEG_MCUS_Y)
#define S5L8702_JPEG_CHROMA_WIDTH    (S5L8702_JPEG_IMG_WIDTH  / 2)
#define S5L8702_JPEG_CHROMA_HEIGHT   (S5L8702_JPEG_IMG_HEIGHT / 2)

/* Sizes of the per-plane software-conversion output buffers. */
#define S5L8702_JPEG_Y_PLANE_SIZE  \
    (S5L8702_JPEG_IMG_WIDTH * S5L8702_JPEG_IMG_HEIGHT)
#define S5L8702_JPEG_C_PLANE_SIZE  \
    (S5L8702_JPEG_CHROMA_WIDTH * S5L8702_JPEG_CHROMA_HEIGHT)

/* Per-MCU staging row stride observed in the firmware's CopyMem source. */
#define S5L8702_JPEG_STAGE_STRIDE    32
#define S5L8702_JPEG_STAGE_ROWS      8
#define S5L8702_JPEG_STAGE_SIZE      \
    (S5L8702_JPEG_STAGE_STRIDE * S5L8702_JPEG_STAGE_ROWS)

/* Each MCU triggers JPEG_REG_CTRL three times in the firmware sequence. */
#define S5L8702_JPEG_CTRL_PER_MCU    3

/* Hardware register offsets within the 1 MiB MMIO region. */
#define S5L8702_JPEG_REG_QTABLE1     0x41200
#define S5L8702_JPEG_REG_QTABLE2     0x41300
#define S5L8702_JPEG_QTABLE_BYTES    256        /* 64 uint32 entries */

#define S5L8702_JPEG_REG_CTRL        0x5000C

#define S5L8702_JPEG_REG_UNK_STATUS  0x60000    /* read returns -1 */
#define S5L8702_JPEG_REG_COEFF_BASE  0x60018
#define S5L8702_JPEG_REG_OUT_Y       0x6002C
#define S5L8702_JPEG_REG_OUT_CB      0x6003C
#define S5L8702_JPEG_REG_OUT_CR      0x6004C

/* Offset from the OUT_CR register value where the software-conversion
 * Y plane begins. The firmware sets up four 64 KiB sub-buffers in its
 * allocated region; the SW conversion path reads from OUT_CR + 0x10000. */
#define S5L8702_JPEG_SW_PLANE_OFFSET 0x10000

struct S5L8702JpegState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    AddressSpace *nsas;

    /* Quantization tables (uploaded by guest before each decode). */
    uint32_t qtable1[64];
    uint32_t qtable2[64];

    /* Register values we actually use. */
    uint32_t coeff_base;        /* JPEG_REG_COEFF_BASE  */
    uint32_t out_y_addr;        /* JPEG_REG_OUT_Y staging address */
    uint32_t out_cb_addr;       /* JPEG_REG_OUT_CB staging address */
    uint32_t out_cr_addr;       /* JPEG_REG_OUT_CR staging address */

    /* Cached full-decode output, valid between the first CTRL trigger of a
     * frame and the final one. The firmware iterates one MCU at a time and
     * gBS->CopyMem's it from the staging registers above into its plane
     * buffer; we satisfy each CopyMem by writing the current MCU into the
     * staging region on every trigger. */
    uint8_t *cached_y;
    uint8_t *cached_cb;
    uint8_t *cached_cr;
    uint32_t ctrl_trigger_count;
    uint32_t status_toggle;     /* toggles the 0x41808 busy bit between reads */
};

#endif /* HW_MISC_S5L8702_JPEG_H */
