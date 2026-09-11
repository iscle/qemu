/*
 * Samsung S5L8702 JPEG decoder (as used in iPod Classic).
 *
 * Tailored to the Apple OF boot-time JPEG path. See the header for
 * documented limitations (hard-coded 320x240 4:2:0; software full-decode
 * with faked per-MCU staging).
 *
 * This file is licensed under the GNU GPL, version 2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "hw/misc/s5l8702-jpeg.h"

#include <math.h>

/* Encoded data the JPEG hardware would receive: 4 luma + 1 Cb + 1 Cr DCT
 * blocks of 64 coefficients each. Each coefficient comes in as a
 * big-endian uint32_t. */
typedef struct {
    uint32_t coeff[64];
} S5L8702JpegBlock;

typedef struct {
    S5L8702JpegBlock lum[4];
    S5L8702JpegBlock chromb;
    S5L8702JpegBlock chromr;
} S5L8702JpegEncMcu;

typedef struct {
    /* Per-MCU IDCT results, before reconstruction into the full planes.
     * yplane is 16x16 (one MCU). cbplane/crplane are 8x8 chroma blocks;
     * the trailing rows/columns of the 16x16 array are unused but kept
     * to simplify indexing. */
    double yplane[16][16];
    double crplane[16][16];
    double cbplane[16][16];
} S5L8702JpegDecMcu;

/* Standard JPEG zigzag scan order. Indexed by natural-order position,
 * gives the zigzag-scan index. */
static const uint8_t jpeg_zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

/* Precomputed 4D IDCT basis table: idct_basis[y][x][u][v] =
 *   c(u) * c(v) * cos((2x+1)uπ/16) * cos((2y+1)vπ/16)
 * Built once on first use. */
static double idct_basis[8][8][8][8];
static bool idct_basis_ready;

static void s5l8702_jpeg_build_idct_basis(void)
{
    if (idct_basis_ready) {
        return;
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            for (int u = 0; u < 8; u++) {
                for (int v = 0; v < 8; v++) {
                    double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    double cv = (v == 0) ? (1.0 / sqrt(2.0)) : 1.0;
                    idct_basis[y][x][u][v] = cu * cv *
                        cos(((2 * x + 1) * u * M_PI) / 16.0) *
                        cos(((2 * y + 1) * v * M_PI) / 16.0);
                }
            }
        }
    }
    idct_basis_ready = true;
}

static uint8_t clamp_to_byte(double x)
{
    if (x < 0.0) {
        return 0;
    }
    if (x > 255.0) {
        return 255;
    }
    return (uint8_t)x;
}

/* Decode a single 8x8 block via dequantize + IDCT. Output samples are
 * stored at out[(*)(y_off + y)][x_off + x] with `level shift` +128. */
static void s5l8702_jpeg_idct_block(const S5L8702JpegBlock *block,
                                    const uint32_t *qtable,
                                    double out[16][16],
                                    int y_off, int x_off)
{
    int32_t dct[8][8];

    for (int l = 0; l < 64; l++) {
        uint32_t coeff = __builtin_bswap32(block->coeff[jpeg_zigzag[l]]);
        dct[l / 8][l % 8] = (int32_t)coeff * (int32_t)qtable[l];
    }

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double sum = 0.0;
            for (int u = 0; u < 8; u++) {
                for (int v = 0; v < 8; v++) {
                    sum += idct_basis[y][x][u][v] * dct[u][v];
                }
            }
            out[y_off + y][x_off + x] = round(sum / 4.0 + 128.0);
        }
    }
}

/* Run the full software JPEG decode for one frame. The encoded MCUs are
 * expected in (width/16)x(height/16) raster order. */
static void s5l8702_jpeg_decode_frame(const S5L8702JpegEncMcu *mcus,
                                      const uint32_t *qtable1,
                                      const uint32_t *qtable2,
                                      uint8_t *y_out,
                                      uint8_t *cb_out,
                                      uint8_t *cr_out)
{
    S5L8702JpegDecMcu *decoded = g_new(S5L8702JpegDecMcu, S5L8702_JPEG_NUM_MCUS);
    double *y_dbl  = g_new(double, S5L8702_JPEG_Y_PLANE_SIZE);
    double *cb_dbl = g_new(double, S5L8702_JPEG_C_PLANE_SIZE);
    double *cr_dbl = g_new(double, S5L8702_JPEG_C_PLANE_SIZE);

    s5l8702_jpeg_build_idct_basis();

    /* IDCT every block of every MCU. */
    for (int i = 0; i < S5L8702_JPEG_NUM_MCUS; i++) {
        /* 4 luma 8x8 blocks make up the 16x16 MCU. */
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                s5l8702_jpeg_idct_block(&mcus[i].lum[row * 2 + col],
                                        qtable1, decoded[i].yplane,
                                        col * 8, row * 8);
            }
        }
        s5l8702_jpeg_idct_block(&mcus[i].chromb, qtable2,
                                decoded[i].cbplane, 0, 0);
        s5l8702_jpeg_idct_block(&mcus[i].chromr, qtable2,
                                decoded[i].crplane, 0, 0);
    }

    /* Reconstruct full planes. The IDCT here stores f(x_loop,y_loop) at
     * yplane[y_loop][x_loop] (a transposed layout); reconstruction reads
     * yplane[x][y] which inverts the transpose. Net effect: pixel (x,y)
     * inside an MCU lands at image (mcu_col*16 + x, mcu_row*16 + y). */
    for (int i = 0; i < S5L8702_JPEG_NUM_MCUS; i++) {
        int mcu_col = i % S5L8702_JPEG_MCUS_X;
        int mcu_row = i / S5L8702_JPEG_MCUS_X;
        int y_base  = mcu_col * 16 + mcu_row * 16 * S5L8702_JPEG_IMG_WIDTH;
        int c_base  = mcu_col * 8  + mcu_row * 8  * S5L8702_JPEG_CHROMA_WIDTH;

        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                y_dbl[y_base + y * S5L8702_JPEG_IMG_WIDTH + x] =
                    decoded[i].yplane[x][y];
            }
        }
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int off = c_base + y * S5L8702_JPEG_CHROMA_WIDTH + x;
                cb_dbl[off] = decoded[i].cbplane[x][y];
                cr_dbl[off] = decoded[i].crplane[x][y];
            }
        }
    }

    /* Clamp to 8-bit. */
    for (int i = 0; i < S5L8702_JPEG_Y_PLANE_SIZE; i++) {
        y_out[i] = clamp_to_byte(y_dbl[i]);
    }
    for (int i = 0; i < S5L8702_JPEG_C_PLANE_SIZE; i++) {
        cb_out[i] = clamp_to_byte(cb_dbl[i]);
        cr_out[i] = clamp_to_byte(cr_dbl[i]);
    }

    /* The hardware/firmware contract is to read each plane back as
     * big-endian 32-bit words. Reverse byte order within each 4-pixel
     * group to match. */
    for (int i = 0; i < S5L8702_JPEG_Y_PLANE_SIZE; i += 4) {
        uint32_t *w = (uint32_t *)&y_out[i];
        *w = __builtin_bswap32(*w);
    }
    for (int i = 0; i < S5L8702_JPEG_C_PLANE_SIZE; i += 4) {
        uint32_t *cb_w = (uint32_t *)&cb_out[i];
        uint32_t *cr_w = (uint32_t *)&cr_out[i];
        *cb_w = __builtin_bswap32(*cb_w);
        *cr_w = __builtin_bswap32(*cr_w);
    }

    g_free(decoded);
    g_free(y_dbl);
    g_free(cb_dbl);
    g_free(cr_dbl);
}

/* Build a single MCU's staging buffers in the layout that the firmware's
 * gBS->CopyMem source expects:
 *   y_stage[r*32 + c]  : luma rows 0..7    of the MCU, cols 0..15
 *   cb_stage[r*32 + c] : luma rows 8..15   of the MCU, cols 0..15
 *   cr_stage[r*32 + 0..7]  : Cb 8x8 of the MCU
 *   cr_stage[r*32 + 8..15] : Cr 8x8 of the MCU
 *
 * The 16-byte tail of each 32-byte row is unused; we leave it zero.
 */
static void s5l8702_jpeg_build_mcu_stage(S5L8702JpegState *s,
                                         uint32_t mcu_idx,
                                         uint8_t *y_stage,
                                         uint8_t *cb_stage,
                                         uint8_t *cr_stage)
{
    uint32_t mc_col = mcu_idx % S5L8702_JPEG_MCUS_X;
    uint32_t mc_row = mcu_idx / S5L8702_JPEG_MCUS_X;

    memset(y_stage,  0, S5L8702_JPEG_STAGE_SIZE);
    memset(cb_stage, 0, S5L8702_JPEG_STAGE_SIZE);
    memset(cr_stage, 0, S5L8702_JPEG_STAGE_SIZE);

    for (int r = 0; r < S5L8702_JPEG_STAGE_ROWS; r++) {
        const uint8_t *y_top = &s->cached_y[
            (mc_row * 16 + r) * S5L8702_JPEG_IMG_WIDTH + mc_col * 16];
        const uint8_t *y_bot = &s->cached_y[
            (mc_row * 16 + 8 + r) * S5L8702_JPEG_IMG_WIDTH + mc_col * 16];
        memcpy(&y_stage[r  * S5L8702_JPEG_STAGE_STRIDE], y_top, 16);
        memcpy(&cb_stage[r * S5L8702_JPEG_STAGE_STRIDE], y_bot, 16);

        const uint8_t *cb_row = &s->cached_cb[
            (mc_row * 8 + r) * S5L8702_JPEG_CHROMA_WIDTH + mc_col * 8];
        const uint8_t *cr_row = &s->cached_cr[
            (mc_row * 8 + r) * S5L8702_JPEG_CHROMA_WIDTH + mc_col * 8];
        memcpy(&cr_stage[r * S5L8702_JPEG_STAGE_STRIDE + 0], cb_row, 8);
        memcpy(&cr_stage[r * S5L8702_JPEG_STAGE_STRIDE + 8], cr_row, 8);
    }
}

/* Drop any cached decode state (called between frames and on reset). */
static void s5l8702_jpeg_drop_cache(S5L8702JpegState *s)
{
    g_free(s->cached_y);
    g_free(s->cached_cb);
    g_free(s->cached_cr);
    s->cached_y = NULL;
    s->cached_cb = NULL;
    s->cached_cr = NULL;
    s->ctrl_trigger_count = 0;
}

/* JPEG_REG_CTRL write handler.
 *
 * The first trigger of a new frame runs the full software decode and
 * writes Y/Cb/Cr to the software-conversion buffer (the firmware reads
 * these directly for its YCbCr->RGB conversion).
 *
 * Every trigger (including the first) also "stages" the current MCU's
 * data into the YPLANE/CBPLANE/CRPLANE registers so that the firmware's
 * per-MCU gBS->CopyMem populates every position of its plane buffer.
 * Without this, the very last MCU's CopyMem reads stale staging and the
 * bottom-right 16x16 corner of the image renders as zeros (green).
 */
static void s5l8702_jpeg_handle_ctrl(S5L8702JpegState *s, uint32_t val)
{
    if (s->cached_y == NULL) {
        S5L8702JpegEncMcu *mcus =
            g_new(S5L8702JpegEncMcu, S5L8702_JPEG_NUM_MCUS);

        s->cached_y  = g_malloc(S5L8702_JPEG_Y_PLANE_SIZE);
        s->cached_cb = g_malloc(S5L8702_JPEG_C_PLANE_SIZE);
        s->cached_cr = g_malloc(S5L8702_JPEG_C_PLANE_SIZE);

        address_space_read(s->nsas, s->coeff_base, MEMTXATTRS_UNSPECIFIED,
                           mcus, sizeof(*mcus) * S5L8702_JPEG_NUM_MCUS);
        s5l8702_jpeg_decode_frame(mcus, s->qtable1, s->qtable2,
                                  s->cached_y, s->cached_cb, s->cached_cr);

        uint32_t sw_base = s->out_cr_addr + S5L8702_JPEG_SW_PLANE_OFFSET;
        address_space_write(s->nsas, sw_base, MEMTXATTRS_UNSPECIFIED,
                            s->cached_y, S5L8702_JPEG_Y_PLANE_SIZE);
        address_space_write(s->nsas, sw_base + S5L8702_JPEG_Y_PLANE_SIZE,
                            MEMTXATTRS_UNSPECIFIED,
                            s->cached_cb, S5L8702_JPEG_C_PLANE_SIZE);
        address_space_write(s->nsas, sw_base + S5L8702_JPEG_Y_PLANE_SIZE
                                              + S5L8702_JPEG_C_PLANE_SIZE,
                            MEMTXATTRS_UNSPECIFIED,
                            s->cached_cr, S5L8702_JPEG_C_PLANE_SIZE);
        g_free(mcus);
    }

    uint32_t mcu_idx = s->ctrl_trigger_count / S5L8702_JPEG_CTRL_PER_MCU;
    if (mcu_idx >= S5L8702_JPEG_NUM_MCUS) {
        mcu_idx = S5L8702_JPEG_NUM_MCUS - 1;
    }

    uint8_t y_stage[S5L8702_JPEG_STAGE_SIZE];
    uint8_t cb_stage[S5L8702_JPEG_STAGE_SIZE];
    uint8_t cr_stage[S5L8702_JPEG_STAGE_SIZE];
    s5l8702_jpeg_build_mcu_stage(s, mcu_idx, y_stage, cb_stage, cr_stage);

    address_space_write(s->nsas, s->out_y_addr, MEMTXATTRS_UNSPECIFIED,
                        y_stage, sizeof(y_stage));
    address_space_write(s->nsas, s->out_cb_addr, MEMTXATTRS_UNSPECIFIED,
                        cb_stage, sizeof(cb_stage));
    address_space_write(s->nsas, s->out_cr_addr, MEMTXATTRS_UNSPECIFIED,
                        cr_stage, sizeof(cr_stage));

    s->ctrl_trigger_count++;
    if (s->ctrl_trigger_count >=
        S5L8702_JPEG_NUM_MCUS * S5L8702_JPEG_CTRL_PER_MCU) {
        s5l8702_jpeg_drop_cache(s);
    }

    (void)val;  /* val is logged at the call site if needed */
}

static uint64_t s5l8702_jpeg_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702JpegState *s = S5L8702_JPEG(opaque);

    if (getenv("JPEG_TRACE") && offset == 0x41808 && current_cpu) {
        static uint32_t last;
        uint32_t pc = (uint32_t)CPU_GET_CLASS(current_cpu)->get_pc(current_cpu);
        if (pc != last) {
            fprintf(stderr, "JPEG41808 read pc=0x%08x\n", pc);
            last = pc;
        }
    }
    switch (offset) {
    case S5L8702_JPEG_REG_UNK_STATUS:
        /* The firmware polls this; returning -1 satisfies its check. */
        return 0xFFFFFFFF;
    case 0x41808:
        /*
         * retailOS 35.2.0.4 decode-status; bit 1 (0x2) is the engine "busy"
         * flag. The guest writes a start command then polls this until busy
         * clears. We do not decode for this register layout, so complete
         * instantly: report busy on the first read after a start (so the guest
         * sees the op accepted) and idle afterwards (so its wait loop
         * advances).
         * Other bits stay set for the status-present check the guest makes.
         */
        if (s->status_toggle) {
            s->status_toggle = 0;
            return 0xFFFFFFFF;       /* busy, just this once */
        }
        return 0xFFFFFFFD;           /* idle / done */
    case 0x50014:
        /*
         * Engine status polled at 0x0bf0eeec: bit 16 = "busy". Report not-busy
         * (0) so the JPEG-engine drive loop advances instead of spinning.
         */
        return 0x00000000;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read (offset 0x%05x)\n",
                      __func__, (uint32_t)offset);
        return 0;
    }
}

static void s5l8702_jpeg_write(void *opaque, hwaddr offset, uint64_t val,
                               unsigned size)
{
    S5L8702JpegState *s = S5L8702_JPEG(opaque);

    /* Quantization table uploads. */
    if (offset >= S5L8702_JPEG_REG_QTABLE1 &&
        offset <  S5L8702_JPEG_REG_QTABLE1 + S5L8702_JPEG_QTABLE_BYTES) {
        s->qtable1[(offset - S5L8702_JPEG_REG_QTABLE1) / 4] = val;
        return;
    }
    if (offset >= S5L8702_JPEG_REG_QTABLE2 &&
        offset <  S5L8702_JPEG_REG_QTABLE2 + S5L8702_JPEG_QTABLE_BYTES) {
        s->qtable2[(offset - S5L8702_JPEG_REG_QTABLE2) / 4] = val;
        return;
    }

    switch (offset) {
    case 0x41800:
        /*
         * Decode start/command (retailOS 35.2.0.4). Mark the engine busy for
         * the next status read at 0x41808; we complete instantly.
         */
        s->status_toggle = 1;
        break;
    case S5L8702_JPEG_REG_COEFF_BASE:
        s->coeff_base = val;
        break;
    case S5L8702_JPEG_REG_OUT_Y:
        s->out_y_addr = val;
        break;
    case S5L8702_JPEG_REG_OUT_CB:
        s->out_cb_addr = val;
        break;
    case S5L8702_JPEG_REG_OUT_CR:
        s->out_cr_addr = val;
        break;
    case S5L8702_JPEG_REG_CTRL:
        s5l8702_jpeg_handle_ctrl(s, val);
        break;
    default:
        /* The firmware writes to many additional offsets to drive the
         * real hardware's block-decode FSM. They have no effect on our
         * full-decode-and-stage emulation. */
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write (offset 0x%05x, value 0x%08x)\n",
                      __func__, (uint32_t)offset, (uint32_t)val);
        break;
    }
}

static const MemoryRegionOps s5l8702_jpeg_ops = {
    .read = s5l8702_jpeg_read,
    .write = s5l8702_jpeg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_jpeg_reset(DeviceState *dev)
{
    S5L8702JpegState *s = S5L8702_JPEG(dev);

    s5l8702_jpeg_drop_cache(s);
    memset(s->qtable1, 0, sizeof(s->qtable1));
    memset(s->qtable2, 0, sizeof(s->qtable2));
    s->coeff_base = 0;
    s->out_y_addr = 0;
    s->out_cb_addr = 0;
    s->out_cr_addr = 0;

    s5l8702_jpeg_build_idct_basis();
}

static void s5l8702_jpeg_init(Object *obj)
{
    S5L8702JpegState *s = S5L8702_JPEG(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_jpeg_ops, s,
                          TYPE_S5L8702_JPEG, S5L8702_JPEG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void s5l8702_jpeg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = s5l8702_jpeg_reset;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s5l8702_jpeg_types[] = {
    {
        .name          = TYPE_S5L8702_JPEG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_init = s5l8702_jpeg_init,
        .instance_size = sizeof(S5L8702JpegState),
        .class_init    = s5l8702_jpeg_class_init,
    },
};
DEFINE_TYPES(s5l8702_jpeg_types);
