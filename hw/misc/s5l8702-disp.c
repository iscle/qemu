/*
 * Samsung S5L8702 display compositor.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * retailOS 2.0.4: 0x081436dc programs the five packed-pixel windows and
 * the separate planar input; 0x08144078 sets their addresses. 0x08143b64
 * controls six input enables. 0x081442d8 selects the blend routing and
 * 0x081441d8/0x08143ebc program blend factors/global alpha. See the machine
 * documentation for the disassembly and captured register/pixel evidence.
 */
#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/misc/s5l8702-disp.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define REG(s, off) ((s)->reg[(off) / 4])
#define CONTROL          0x008
#define BACKGROUND       0x024
#define PLANAR_FORMAT    0x028
#define PLANAR_STRIDE    0x02c
#define SOURCE_ORIGIN    0x030
#define SOURCE_LIMIT     0x034
#define PLANE_Y          0x038
#define PLANE_CB         0x03c
#define PLANE_CR         0x044
#define SCALE            0x04c
#define DEST_ORIGIN      0x050
#define DEST_SIZE        0x054
#define WINDOW_BASE      0x058
#define WINDOW_STEP      0x018
#define BLEND_ORDER      0x0d4
#define BLEND_BASE       0x0d8
#define ROTATION         0x3ac

/*
 * Three blend stages take the single packed window, the multi-window input,
 * and the planar input. Rows map these input groups to stages, back to front.
 * The six permutations are used by the firmware table at 0x083f18fc.
 */
static const uint8_t blend_stage[6][3] = {
    { 1, 2, 0 }, { 2, 1, 0 }, { 0, 2, 1 },
    { 2, 0, 1 }, { 0, 1, 2 }, { 1, 0, 2 },
};

static uint64_t s5l8702_disp_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702DispState *s = opaque;
    uint32_t value = REG(s, offset);

    trace_s5l8702_disp_access(false, offset, value);
    return value;
}

static void s5l8702_disp_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    S5L8702DispState *s = opaque;

    REG(s, offset) = value;
    trace_s5l8702_disp_access(true, offset, value);
}

/* Reject MMIO sources before reading: a guest must not recurse into devices. */
static bool disp_read_ram(hwaddr address, void *buf, size_t length)
{
    MemoryRegionSection section = memory_region_find(get_system_memory(),
                                                     address, length);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 int128_eq(section.size, int128_make64(length));

    if (section.mr) {
        memory_region_unref(section.mr);
    }
    if (!valid || address_space_read(&address_space_memory, address,
                                     MEMTXATTRS_UNSPECIFIED, buf, length) !=
                  MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-disp: invalid DMA read at 0x%" HWADDR_PRIx
                      " length %zu\n", address, length);
        return false;
    }
    return true;
}

static unsigned disp_factor(unsigned selector, unsigned alpha, unsigned global)
{
    /* The firmware's modes select (1,0), (0,1), (5,4), (9,8), (11,10). */
    switch (selector) {
    case 0:
        return 0;
    case 1:
        return 255;
    case 4:
        return 255 - alpha;
    case 5:
        return alpha;
    case 8:
        return 255 - global;
    case 9:
        return global;
    case 10:
        return 255 - alpha * global / 255;
    case 11:
        return alpha * global / 255;
    default:
        return 0;
    }
}

static uint32_t disp_blend(uint32_t src, uint32_t dst, uint32_t config)
{
    unsigned alpha = src >> 24;
    unsigned global = config & 0xff;
    unsigned sf = disp_factor(config >> 28, alpha, global);
    unsigned df = disp_factor((config >> 12) & 0xf, alpha, global);
    uint32_t result = 0xff000000;

    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned c = (((src >> shift) & 0xff) * sf +
                      ((dst >> shift) & 0xff) * df + 127) / 255;
        result |= MIN(c, 255) << shift;
    }
    return result;
}

static uint32_t disp_rgb565(uint16_t pixel)
{
    unsigned r = (pixel >> 11) & 31;
    unsigned g = (pixel >> 5) & 63;
    unsigned b = pixel & 31;

    return 0xff000000 | ((r * 255 / 31) << 16) |
           ((g * 255 / 63) << 8) | (b * 255 / 31);
}

static bool disp_draw_window(S5L8702DispState *s, unsigned window,
                             uint32_t blend, unsigned width, unsigned height)
{
    unsigned base = WINDOW_BASE + window * WINDOW_STEP;
    uint32_t stride = REG(s, base);
    unsigned fmt = (REG(s, base + 4) >> 8) & 0xff;
    hwaddr src = REG(s, base + 8);
    uint32_t size = REG(s, base + 12);
    uint32_t pos = REG(s, base + 20);
    int w = (int16_t)(size >> 16), h = (int16_t)size;
    int x = (int16_t)(pos >> 16), y = (int16_t)pos;
    unsigned bpp;

    if (!(REG(s, CONTROL) & BIT(6 - window)) || w <= 0 || h <= 0 ||
        x >= (int)width || y >= (int)height || x + w <= 0 || y + h <= 0) {
        return true;
    }
    if (fmt == 3) {
        bpp = 2;
    } else if (fmt == 6 || fmt == 7) {
        bpp = 4;
    } else {
        qemu_log_mask(LOG_UNIMP, "s5l8702-disp: pixel format %u\n", fmt);
        return false;
    }
    if (x < 0) {
        src += (hwaddr)-x * bpp;
        w += x;
        x = 0;
    }
    if (y < 0) {
        src += (hwaddr)-y * stride;
        h += y;
        y = 0;
    }
    w = MIN(w, width - x);
    h = MIN(h, height - y);
    for (int row = 0; row < h; row++) {
        uint32_t *dst = &s->pixels[(y + row) * width + x];

        if (!disp_read_ram(src + (hwaddr)row * stride, s->rowbuf, w * bpp)) {
            return false;
        }
        for (int col = 0; col < w; col++) {
            uint32_t pixel;

            if (bpp == 2) {
                pixel = disp_rgb565(lduw_le_p(s->rowbuf + col * 2));
            } else {
                pixel = ldl_le_p(s->rowbuf + col * 4);
            }
            if (fmt == 6) {
                pixel |= 0xff000000;
            }
            dst[col] = disp_blend(pixel, dst[col], blend);
        }
    }
    return true;
}

static unsigned disp_clamp(int value)
{
    return MIN(MAX(value, 0), 255);
}

static uint32_t disp_ycbcr(uint8_t y, uint8_t cb, uint8_t cr)
{
    int c = y - 16, d = cb - 128, e = cr - 128;
    unsigned r = disp_clamp((298 * c + 409 * e + 128) >> 8);
    unsigned g = disp_clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
    unsigned b = disp_clamp((298 * c + 516 * d + 128) >> 8);

    /* Limited-range conversion checked against the firmware's gradient. */
    return 0xff000000 | (r << 16) | (g << 8) | b;
}

static bool disp_draw_planar(S5L8702DispState *s, uint32_t blend,
                             unsigned width, unsigned height)
{
    uint32_t stride = REG(s, PLANAR_STRIDE);
    unsigned ys = stride & 0xffff, cs = stride >> 16;
    uint32_t origin = REG(s, SOURCE_ORIGIN);
    uint32_t limit = REG(s, SOURCE_LIMIT);
    unsigned sx0 = origin >> 16, sy0 = origin & 0xffff;
    unsigned sx1 = limit >> 16, sy1 = limit & 0xffff;
    uint32_t dest = REG(s, DEST_ORIGIN), size = REG(s, DEST_SIZE);
    int dx = (int16_t)(dest >> 16), dy = (int16_t)dest;
    unsigned w = size >> 16, h = size & 0xffff;
    uint32_t scale = REG(s, SCALE);
    unsigned xstep = scale >> 16, ystep = scale & 0xffff;
    uint8_t luma[S5L8702_DISP_WIDTH], cb[S5L8702_DISP_WIDTH];
    uint8_t cr[S5L8702_DISP_WIDTH];

    if (!(REG(s, CONTROL) & BIT(7))) {
        return true;
    }
    if (!(REG(s, PLANAR_FORMAT) & BIT(8)) || (REG(s, ROTATION) & 1)) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-disp: planar format/rotation\n");
        return false;
    }
    if (!xstep || !ystep || sx1 <= sx0 || sy1 <= sy0 || !ys || !cs) {
        return false;
    }
    /* Output is clipped before multiplying guest-controlled geometry. */
    for (unsigned row = 0; row < height; row++) {
        unsigned sy;
        hwaddr ybase, ubase, vbase;
        bool cached;

        if ((int)row < dy || (uint64_t)((int64_t)row - dy) >= h) {
            continue;
        }
        sy = sy0 + ((uint64_t)((int64_t)row - dy) * ystep >> 12);
        if (sy >= sy1) {
            continue;
        }
        ybase = (hwaddr)REG(s, PLANE_Y) + sy * (hwaddr)ys;
        ubase = (hwaddr)REG(s, PLANE_CB) + (sy / 2) * (hwaddr)cs;
        vbase = (hwaddr)REG(s, PLANE_CR) + (sy / 2) * (hwaddr)cs;
        /* Cache contiguous rows for the common 1:1 scanout. */
        cached = xstep == 0x1000 && dx >= 0 && sx0 + w <= ys &&
                 sx0 + w <= cs * 2 && w > 0 && w <= S5L8702_DISP_WIDTH;
        if (cached) {
            unsigned chroma_len = ((sx0 & 1) + w + 1) / 2;

            if (!disp_read_ram(ybase + sx0, luma, w) ||
                !disp_read_ram(ubase + sx0 / 2, cb, chroma_len) ||
                !disp_read_ram(vbase + sx0 / 2, cr, chroma_len)) {
                return false;
            }
        }
        for (unsigned col = 0; col < width; col++) {
            unsigned sx;
            uint8_t y, u, v;
            uint32_t *pixel = &s->pixels[row * width + col];

            if ((int)col < dx || (uint64_t)((int64_t)col - dx) >= w) {
                continue;
            }
            sx = sx0 + ((uint64_t)((int64_t)col - dx) * xstep >> 12);
            if (sx >= sx1 || sx >= ys || sx / 2 >= cs) {
                continue;
            }
            if (cached) {
                y = luma[col - dx];
                u = cb[((sx0 & 1) + col - dx) / 2];
                v = cr[((sx0 & 1) + col - dx) / 2];
            } else {
                if (!disp_read_ram(ybase + sx, &y, 1) ||
                    !disp_read_ram(ubase + sx / 2, &u, 1) ||
                    !disp_read_ram(vbase + sx / 2, &v, 1)) {
                    return false;
                }
            }
            *pixel = disp_blend(disp_ycbcr(y, u, v), *pixel, blend);
        }
    }
    return true;
}

bool s5l8702_disp_compose(S5L8702DispState *s, uint16_t *fb,
                          unsigned width, unsigned height)
{
    unsigned order = REG(s, BLEND_ORDER) & 0xff;
    bool valid = true;

    if (!width || !height || width > S5L8702_DISP_WIDTH ||
        height > S5L8702_DISP_HEIGHT || !(REG(s, CONTROL) & 0xfc)) {
        return false;
    }
    if (order >= ARRAY_SIZE(blend_stage)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-disp: invalid blend order %u\n", order);
        return false;
    }
    for (unsigned i = 0; i < width * height; i++) {
        s->pixels[i] = REG(s, BACKGROUND) | 0xff000000;
    }
    for (unsigned stage = 0; stage < 3; stage++) {
        uint32_t blend = REG(s, BLEND_BASE + stage * 8);
        if (blend_stage[order][0] == stage) {
            valid &= disp_draw_window(s, 0, blend, width, height);
        } else if (blend_stage[order][1] == stage) {
            for (int window = 3; window >= 1; window--) {
                valid &= disp_draw_window(s, window, blend, width, height);
            }
        } else {
            valid &= disp_draw_planar(s, blend, width, height);
        }
    }
    /* The cursor window does not pass through the three blend stages. */
    valid &= disp_draw_window(s, 4, 0x10000000, width, height);
    if (!valid) {
        return false;
    }
    for (unsigned i = 0; i < width * height; i++) {
        uint32_t p = s->pixels[i];
        fb[i] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x7e0) | ((p >> 3) & 31);
    }
    return true;
}

static const MemoryRegionOps s5l8702_disp_ops = {
    .read = s5l8702_disp_read,
    .write = s5l8702_disp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8702_disp_reset(Object *obj, ResetType type)
{
    S5L8702DispState *s = S5L8702_DISP(obj);

    memset(s->reg, 0, sizeof(s->reg));
}

static void s5l8702_disp_init(Object *obj)
{
    S5L8702DispState *s = S5L8702_DISP(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_disp_ops, s,
                          TYPE_S5L8702_DISP, S5L8702_DISP_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_disp = {
    .name = TYPE_S5L8702_DISP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, S5L8702DispState, S5L8702_DISP_NREG),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_disp_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 display compositor";
    RESETTABLE_CLASS(klass)->phases.enter = s5l8702_disp_reset;
    dc->vmsd = &vmstate_s5l8702_disp;
    dc->user_creatable = false;
}

static const TypeInfo s5l8702_disp_types[] = {
    {
        .name = TYPE_S5L8702_DISP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702DispState),
        .instance_init = s5l8702_disp_init,
        .class_init = s5l8702_disp_class_init,
    },
};
DEFINE_TYPES(s5l8702_disp_types)
