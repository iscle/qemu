/*
 * Samsung S5L8702 JPEG block processor.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original EFI JpegDecoder 0xe34 and retailOS 0x08090cb0 arm output banks
 * with 0x5000c, submit an IDCT command at 0x41800, and request coefficient
 * input at 0x3010c. Each pair of 8x8 blocks completes one output bank.
 * The CPU assembles the image; the device has no fixed frame dimensions.
 *
 * Only the observed dequantization/IDCT and paired-block DMA mode is
 * implemented. Processing is synchronous; hardware clock timing, entropy
 * decoding, other DMA layouts and precise error flags remain unverified.
 */
#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/misc/s5l8702-jpeg.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include <math.h>

#define JPEG_STATUS         0x00000
#define JPEG_MASK           0x00004
#define JPEG_CONFIG         0x00010
#define JPEG_INPUT_COMMAND  0x3010c
#define JPEG_QTABLE0        0x41200
#define JPEG_QTABLE1        0x41300
#define JPEG_COMMAND        0x41800
#define JPEG_CORE_STATUS    0x41808
#define JPEG_OUTPUT_COMMAND 0x5000c
#define JPEG_OUTPUT_STATUS  0x50014
#define JPEG_DMA_STATUS     0x60000
#define JPEG_DMA_MASK       0x60004
#define JPEG_DMA_CONTROL    0x6000c
#define JPEG_DMA_CONFIG     0x60010
#define JPEG_INPUT_BASE     0x60018
#define JPEG_INPUT_END      0x6001c
#define JPEG_OUTPUT0        0x6002c
#define JPEG_OUTPUT1        0x6003c
#define JPEG_OUTPUT2        0x6004c
#define JPEG_LAYOUT         0x6006c

#define JPEG_DMA_IRQ        BIT(6)
#define JPEG_PAIR_DONE      BIT(1)
#define JPEG_QSELECT        BIT(19)
#define JPEG_IDCT_COMMAND   0x20341

/* Natural-order coefficient position to zigzag input index. */
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

/* Immutable after class initialization. No frame-sized intermediate buffers. */
static double idct_basis[8][8][8][8];

static void jpeg_build_basis(void)
{
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            for (unsigned u = 0; u < 8; u++) {
                for (unsigned v = 0; v < 8; v++) {
                    double cu = u == 0 ? 1.0 / sqrt(2.0) : 1.0;
                    double cv = v == 0 ? 1.0 / sqrt(2.0) : 1.0;
                    idct_basis[y][x][u][v] = cu * cv *
                        cos((2 * y + 1) * u * M_PI / 16.0) *
                        cos((2 * x + 1) * v * M_PI / 16.0);
                }
            }
        }
    }
}

static void jpeg_idct(const uint8_t *input, const uint32_t *qtable,
                      uint8_t output[8][8])
{
    double dct[8][8];

    for (unsigned i = 0; i < 64; i++) {
        int32_t coefficient = (int32_t)ldl_be_p(input + 4 * jpeg_zigzag[i]);
        dct[i / 8][i % 8] = (double)coefficient * qtable[i];
    }
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            double sum = 0;
            for (unsigned u = 0; u < 8; u++) {
                for (unsigned v = 0; v < 8; v++) {
                    sum += idct_basis[y][x][u][v] * dct[u][v];
                }
            }
            /* The observed DMA mode reverses bytes within each output word. */
            output[y][x ^ 3] = MIN(255, MAX(0, round(sum / 4.0 + 128.0)));
        }
    }
}

static void jpeg_update_irq(S5L8702JpegState *s)
{
    qemu_set_irq(s->irq, (s->irq_mask & JPEG_DMA_IRQ) &&
                         (s->dma_status & s->dma_mask));
}

/* Reject device recursion and transfers outside the 32-bit address space. */
static bool jpeg_valid_ram(uint64_t address, size_t length, bool write)
{
    MemoryRegionSection section;
    bool valid;

    if (address + length > (1ULL << 32)) {
        return false;
    }
    section = memory_region_find(get_system_memory(), address, length);
    valid = section.mr && memory_region_is_ram(section.mr) &&
            (!write || !memory_region_is_rom(section.mr)) &&
            int128_eq(section.size, int128_make64(length));
    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid;
}

static void jpeg_process_block(S5L8702JpegState *s, uint32_t input_command)
{
    uint8_t coefficients[256], pixels[8][8];
    uint64_t output;
    unsigned bank;

    if (!s->pending || !(s->dma_control & 1) || !s->output_count) {
        return;
    }
    if ((s->command & ~JPEG_QSELECT) != JPEG_IDCT_COMMAND ||
        (input_command != 0x31 && input_command != 0x39) ||
        s->dma_config != 0x182 || s->layout != 0x10001) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-jpeg: unsupported block mode\n");
        return;
    }
    bank = s->output_queue[0];
    output = (uint64_t)s->output_address[bank] + s->block_in_pair * 8;
    if ((uint64_t)s->input_cursor + sizeof(coefficients) > s->input_end ||
        !jpeg_valid_ram(s->input_cursor, sizeof(coefficients), false)) {
        goto invalid_dma;
    }
    for (unsigned y = 0; y < 8; y++) {
        if (!jpeg_valid_ram(output + y * 32, 8, true)) {
            goto invalid_dma;
        }
    }
    if (address_space_read(&address_space_memory, s->input_cursor,
                           MEMTXATTRS_UNSPECIFIED, coefficients,
                           sizeof(coefficients)) != MEMTX_OK) {
        goto invalid_dma;
    }
    jpeg_idct(coefficients, s->qtable[!!(s->command & JPEG_QSELECT)], pixels);
    for (unsigned y = 0; y < 8; y++) {
        if (address_space_write(&address_space_memory, output + y * 32,
                                MEMTXATTRS_UNSPECIFIED, pixels[y], 8) !=
            MEMTX_OK) {
            goto invalid_dma;
        }
    }
    trace_s5l8702_jpeg_block(s->input_cursor, output, s->command, bank,
                            s->block_in_pair);
    s->input_cursor += sizeof(coefficients);
    s->pending = false;
    if (++s->block_in_pair == 2) {
        s->block_in_pair = 0;
        s->output_count--;
        memmove(s->output_queue, s->output_queue + 1, s->output_count);
        s->dma_status |= JPEG_PAIR_DONE;
        jpeg_update_irq(s);
    }
    return;

invalid_dma:
    /* Error status is not established; do not invent successful completion. */
    qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-jpeg: invalid block DMA\n");
}

static uint64_t jpeg_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702JpegState *s = opaque;

    if (offset >= JPEG_QTABLE0 && offset < JPEG_QTABLE1 + 256) {
        return s->qtable[(offset - JPEG_QTABLE0) / 256][(offset & 255) / 4];
    }
    switch (offset) {
    case JPEG_STATUS:
        return s->dma_status ? JPEG_DMA_IRQ : 0;
    case JPEG_MASK:
        return s->irq_mask;
    case JPEG_CONFIG:
        return s->config;
    case JPEG_CORE_STATUS:
        return s->pending ? BIT(1) : 0;
    case JPEG_OUTPUT_STATUS:
        return 0;
    case JPEG_DMA_STATUS:
        return s->dma_status;
    case JPEG_DMA_MASK:
        return s->dma_mask;
    case JPEG_DMA_CONTROL:
        return s->dma_control;
    case JPEG_DMA_CONFIG:
        return s->dma_config;
    case JPEG_INPUT_BASE:
        return s->input_base;
    case JPEG_INPUT_END:
        return s->input_end;
    case JPEG_OUTPUT0:
    case JPEG_OUTPUT1:
    case JPEG_OUTPUT2:
        return s->output_base[(offset - JPEG_OUTPUT0) / 16];
    case JPEG_LAYOUT:
        return s->layout;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-jpeg: read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void jpeg_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    S5L8702JpegState *s = opaque;
    unsigned bank;

    if (offset >= JPEG_QTABLE0 && offset < JPEG_QTABLE1 + 256) {
        s->qtable[(offset - JPEG_QTABLE0) / 256][(offset & 255) / 4] = value;
        return;
    }
    trace_s5l8702_jpeg_write(offset, value);
    switch (offset) {
    case JPEG_STATUS:
        /* The DMA summary remains asserted while a child event is pending. */
        break;
    case JPEG_MASK:
        s->irq_mask = value;
        jpeg_update_irq(s);
        break;
    case JPEG_CONFIG:
        s->config = value;
        break;
    case JPEG_DMA_STATUS:
        s->dma_status &= ~value;
        jpeg_update_irq(s);
        break;
    case JPEG_DMA_MASK:
        s->dma_mask = value;
        jpeg_update_irq(s);
        break;
    case JPEG_DMA_CONTROL:
        s->dma_control = value;
        if (!(value & 1)) {
            s->pending = false;
        }
        break;
    case JPEG_DMA_CONFIG:
        s->dma_config = value;
        break;
    case JPEG_INPUT_BASE:
        s->input_base = s->input_cursor = value;
        break;
    case JPEG_INPUT_END:
        s->input_end = value;
        break;
    case JPEG_OUTPUT0:
    case JPEG_OUTPUT1:
    case JPEG_OUTPUT2:
        s->output_base[(offset - JPEG_OUTPUT0) / 16] = value;
        break;
    case JPEG_LAYOUT:
        s->layout = value;
        break;
    case JPEG_COMMAND:
        if (s->pending) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s5l8702-jpeg: command while busy\n");
            break;
        }
        s->command = value;
        s->pending = true;
        break;
    case JPEG_INPUT_COMMAND:
        jpeg_process_block(s, value);
        break;
    case JPEG_OUTPUT_COMMAND:
        bank = value >> 30;
        if (!(value & BIT(7)) || bank >= ARRAY_SIZE(s->output_queue)) {
            qemu_log_mask(LOG_UNIMP, "s5l8702-jpeg: output command 0x%08x\n",
                          (uint32_t)value);
            break;
        }
        for (unsigned i = 0; i < s->output_count; i++) {
            if (s->output_queue[i] == bank) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "s5l8702-jpeg: output bank already armed\n");
                return;
            }
        }
        s->output_address[bank] = s->output_base[bank];
        s->output_queue[s->output_count++] = bank;
        break;
    /* Acknowledgements and setup for sub-engines not yet implemented. */
    case 0x0000c:
    case 0x0001c:
    case 0x0002c:
    case 0x10000:
    case 0x30100:
    case 0x30104:
    case 0x30110:
    case 0x41804:
    case 0x41810:
    case 0x50000:
    case 0x50010:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-jpeg: write at 0x%" HWADDR_PRIx
                      " value 0x%08x\n", offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps jpeg_ops = {
    .read = jpeg_read,
    .write = jpeg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void jpeg_reset_enter(Object *obj, ResetType type)
{
    S5L8702JpegState *s = S5L8702_JPEG(obj);

    memset(s->qtable, 0, sizeof(s->qtable));
    s->irq_mask = s->config = s->dma_status = s->dma_mask = 0;
    s->dma_control = s->dma_config = 0;
    s->input_base = s->input_end = s->input_cursor = 0;
    memset(s->output_base, 0, sizeof(s->output_base));
    memset(s->output_address, 0, sizeof(s->output_address));
    memset(s->output_queue, 0, sizeof(s->output_queue));
    s->layout = s->command = 0;
    s->output_count = s->block_in_pair = 0;
    s->pending = false;
}

static void jpeg_reset_hold(Object *obj)
{
    jpeg_update_irq(S5L8702_JPEG(obj));
}

static int jpeg_post_load(void *opaque, int version_id)
{
    S5L8702JpegState *s = opaque;
    unsigned seen = 0;

    if (s->output_count > 3 || s->block_in_pair > 1 ||
        (!s->output_count && s->block_in_pair)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < s->output_count; i++) {
        unsigned bank = s->output_queue[i];
        if (bank >= 3 || (seen & BIT(bank))) {
            return -EINVAL;
        }
        seen |= BIT(bank);
    }
    jpeg_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_jpeg = {
    .name = TYPE_S5L8702_JPEG,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = jpeg_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(qtable, S5L8702JpegState, 2, 64),
        VMSTATE_UINT32(irq_mask, S5L8702JpegState),
        VMSTATE_UINT32(config, S5L8702JpegState),
        VMSTATE_UINT32(dma_status, S5L8702JpegState),
        VMSTATE_UINT32(dma_mask, S5L8702JpegState),
        VMSTATE_UINT32(dma_control, S5L8702JpegState),
        VMSTATE_UINT32(dma_config, S5L8702JpegState),
        VMSTATE_UINT32(input_base, S5L8702JpegState),
        VMSTATE_UINT32(input_end, S5L8702JpegState),
        VMSTATE_UINT32(input_cursor, S5L8702JpegState),
        VMSTATE_UINT32_ARRAY(output_base, S5L8702JpegState, 3),
        VMSTATE_UINT32_ARRAY(output_address, S5L8702JpegState, 3),
        VMSTATE_UINT32(layout, S5L8702JpegState),
        VMSTATE_UINT32(command, S5L8702JpegState),
        VMSTATE_UINT8_ARRAY(output_queue, S5L8702JpegState, 3),
        VMSTATE_UINT8(output_count, S5L8702JpegState),
        VMSTATE_UINT8(block_in_pair, S5L8702JpegState),
        VMSTATE_BOOL(pending, S5L8702JpegState),
        VMSTATE_END_OF_LIST()
    }
};

static void jpeg_init(Object *obj)
{
    S5L8702JpegState *s = S5L8702_JPEG(obj);

    memory_region_init_io(&s->iomem, obj, &jpeg_ops, s,
                          TYPE_S5L8702_JPEG, S5L8702_JPEG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void jpeg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "S5L8702 JPEG block processor";
    dc->user_creatable = false;
    dc->vmsd = &vmstate_jpeg;
    rc->phases.enter = jpeg_reset_enter;
    rc->phases.hold = jpeg_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    jpeg_build_basis();
}

static const TypeInfo jpeg_info = {
    .name = TYPE_S5L8702_JPEG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702JpegState),
    .instance_init = jpeg_init,
    .class_init = jpeg_class_init,
};

static void jpeg_register_types(void)
{
    type_register_static(&jpeg_info);
}
type_init(jpeg_register_types)
