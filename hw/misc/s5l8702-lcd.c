/*
 * Samsung S5L8702 LCD interface and panel.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * retailOS 2.0.4 0x081448b0 enables the frame engine at +0x70.
 * 0x080baef4 selects the display input at +0x80, then 0x080db2e0 sends
 * the panel column/page window and memory-write command. Panel command 0x35
 * enables TE: GPIO 55 reaches 0x080cc210 through the external interrupt
 * dispatcher, then 0x08143f34 submits a frame if the guest marked it dirty.
 * Host presentation reads panel GRAM, never live guest layers. The timer
 * models the panel's TE source; it does not transfer frames on its own.
 */
#include "qemu/osdep.h"
#include "hw/misc/s5l8702-lcd.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "ui/pixel_ops.h"
#include "trace.h"

#define REG(s, off) ((s)->regs[(off) / 4])
#define LCD_CONFIG       0x00
#define LCD_WCMD         0x04
#define LCD_RCMD         0x0c
#define LCD_RDATA        0x10
#define LCD_DBUFF        0x14
#define LCD_INTCON       0x18
#define LCD_STATUS       0x1c
#define LCD_PHTIME       0x20
#define LCD_WDATA        0x40
#define LCD_FRAME_ENABLE 0x70
#define LCD_FRAME_SIZE   0x74
#define LCD_FRAME_TIMING 0x78
#define LCD_FRAME_CONFIG 0x7c
#define LCD_FRAME_INPUT  0x80
#define LCD_FRAME_UNK84  0x84
#define LCD_FRAME_UNK88  0x88
#define LCD_FRAME_STATUS 0x8c

static bool lcd_window_valid(S5L8702LcdState *s)
{
    return s->sc <= s->ec && s->sp <= s->ep &&
           s->ec < S5L8702_DISP_WIDTH && s->ep < S5L8702_DISP_HEIGHT;
}

static void lcd_transfer_frame(S5L8702LcdState *s)
{
    if (!s->disp || !lcd_window_valid(s)) {
        return;
    }
    if (!s5l8702_disp_compose(s->disp, s->composed,
                             S5L8702_DISP_WIDTH, S5L8702_DISP_HEIGHT)) {
        return;
    }
    for (unsigned y = s->sp; y <= s->ep; y++) {
        unsigned offset = y * S5L8702_DISP_WIDTH + s->sc;
        memcpy(&s->framebuffer[offset], &s->composed[offset],
               (s->ec - s->sc + 1) * sizeof(uint16_t));
    }
    s->invalidate = true;
}

static void lcd_te_schedule(S5L8702LcdState *s)
{
    if (s->te_enabled && !s->sleeping) {
        timer_mod(s->te_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / s->refresh_rate);
    } else {
        timer_del(s->te_timer);
    }
}

static void lcd_te_timer(void *opaque)
{
    S5L8702LcdState *s = opaque;

    if (s->te_enabled && !s->sleeping) {
        /* Pulse width and exact panel oscillator rate remain unmeasured. */
        qemu_irq_pulse(s->te);
        lcd_te_schedule(s);
    }
}

static void lcd_command(S5L8702LcdState *s, uint8_t command)
{
    s->command = command;
    s->parameter = 0;
    trace_s5l8702_lcd_write("WCMD", command);
    switch (command) {
    case 0x04: /* Read display ID. The first byte is a dummy cycle. */
        s->reply[0] = 0;
        s->reply[1] = 0x38;
        s->reply[2] = 0xb3;
        s->reply[3] = 0x71;
        s->reply_len = 4;
        s->reply_pos = 0;
        break;
    case 0x10:
        s->sleeping = true;
        lcd_te_schedule(s);
        s->invalidate = true;
        break;
    case 0x11:
        s->sleeping = false;
        lcd_te_schedule(s);
        s->invalidate = true;
        break;
    case 0x28:
        s->display_on = false;
        s->invalidate = true;
        break;
    case 0x29:
        s->display_on = true;
        s->invalidate = true;
        break;
    case 0x34:
        s->te_enabled = false;
        lcd_te_schedule(s);
        break;
    case 0x35:
        s->te_enabled = true;
        lcd_te_schedule(s);
        break;
    case 0x2c:
        s->x = s->sc;
        s->y = s->sp;
        if ((REG(s, LCD_FRAME_ENABLE) & 1) &&
            (REG(s, LCD_FRAME_INPUT) & 1)) {
            lcd_transfer_frame(s);
        }
        break;
    default:
        break;
    }
}

static void lcd_pixel(S5L8702LcdState *s, uint16_t value)
{
    if (!lcd_window_valid(s) || s->x < s->sc || s->x > s->ec ||
        s->y < s->sp || s->y > s->ep) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-lcd: invalid pixel window\n");
        return;
    }
    s->framebuffer[s->y * S5L8702_DISP_WIDTH + s->x] = value;
    if (++s->x > s->ec) {
        s->x = s->sc;
        if (++s->y > s->ep) {
            s->y = s->sp;
        }
    }
    s->invalidate = true;
}

static void lcd_data(S5L8702LcdState *s, uint32_t value)
{
    if (s->command == 0x2c) {
        lcd_pixel(s, value);
        return;
    }
    trace_s5l8702_lcd_write("WDATA", value);
    s->panel_regs[s->command] =
        (s->panel_regs[s->command] << 8) | (value & 0xff);
    if (s->parameter < 4 && (s->command == 0x2a || s->command == 0x2b)) {
        uint16_t *p = s->command == 0x2a ?
                      (s->parameter < 2 ? &s->sc : &s->ec) :
                      (s->parameter < 2 ? &s->sp : &s->ep);
        *p = (*p << 8) | (value & 0xff);
    }
    if (s->parameter < UINT8_MAX) {
        s->parameter++;
    }
}

static uint64_t s5l8702_lcd_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702LcdState *s = opaque;

    switch (offset) {
    case LCD_STATUS:
        /* PIO operations complete synchronously: read done and FIFO empty. */
        return 3;
    case LCD_FRAME_STATUS:
        /* Transfer latency and its busy bits are not yet modeled. */
        return 0;
    case LCD_CONFIG:
    case LCD_WCMD:
    case LCD_RCMD:
    case LCD_RDATA:
    case LCD_DBUFF:
    case LCD_INTCON:
    case LCD_PHTIME:
    case LCD_WDATA:
    case LCD_FRAME_ENABLE:
    case LCD_FRAME_SIZE:
    case LCD_FRAME_TIMING:
    case LCD_FRAME_CONFIG:
    case LCD_FRAME_INPUT:
    case LCD_FRAME_UNK84:
    case LCD_FRAME_UNK88:
        return REG(s, offset);
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-lcd: read at +0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void s5l8702_lcd_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8702LcdState *s = opaque;

    switch (offset) {
    case LCD_WCMD:
        REG(s, offset) = value;
        lcd_command(s, value);
        break;
    case LCD_WDATA:
        REG(s, offset) = value;
        lcd_data(s, value);
        break;
    case LCD_RDATA:
        REG(s, offset) = value;
        if (!value) {
            REG(s, LCD_DBUFF) = s->reply_pos < s->reply_len ?
                               s->reply[s->reply_pos++] << 1 : 0;
        }
        break;
    case LCD_STATUS:
        break;
    case LCD_CONFIG:
    case LCD_RCMD:
    case LCD_DBUFF:
    case LCD_INTCON:
    case LCD_PHTIME:
    case LCD_FRAME_ENABLE:
    case LCD_FRAME_SIZE:
    case LCD_FRAME_TIMING:
    case LCD_FRAME_CONFIG:
    case LCD_FRAME_INPUT:
    case LCD_FRAME_UNK84:
    case LCD_FRAME_UNK88:
        REG(s, offset) = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-lcd: write at +0x%" HWADDR_PRIx
                      " = 0x%08" PRIx64 "\n", offset, value);
        break;
    }
}

static const MemoryRegionOps s5l8702_lcd_ops = {
    .read = s5l8702_lcd_read,
    .write = s5l8702_lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void lcd_invalidate(void *opaque)
{
    S5L8702LcdState *s = opaque;
    s->invalidate = true;
}

static void lcd_update(void *opaque)
{
    S5L8702LcdState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!s->invalidate) {
        return;
    }
    for (unsigned y = 0; y < S5L8702_DISP_HEIGHT; y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) +
                                    y * surface_stride(surface));
        for (unsigned x = 0; x < S5L8702_DISP_WIDTH; x++) {
            uint16_t p = s->display_on && !s->sleeping ?
                         s->framebuffer[y * S5L8702_DISP_WIDTH + x] : 0;
            row[x] = rgb_to_pixel32((p >> 8) & 0xf8, (p >> 3) & 0xfc,
                                    (p << 3) & 0xf8);
        }
    }
    dpy_gfx_update(s->con, 0, 0, S5L8702_DISP_WIDTH, S5L8702_DISP_HEIGHT);
    s->invalidate = false;
}

static const GraphicHwOps lcd_ops = {
    .invalidate = lcd_invalidate,
    .gfx_update = lcd_update,
};

static void s5l8702_lcd_reset(Object *obj, ResetType type)
{
    S5L8702LcdState *s = S5L8702_LCD(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->panel_regs, 0, sizeof(s->panel_regs));
    memset(s->framebuffer, 0, sizeof(s->framebuffer));
    memset(s->reply, 0, sizeof(s->reply));
    timer_del(s->te_timer);
    s->te_enabled = false;
    qemu_irq_lower(s->te);
    s->sc = s->sp = s->x = s->y = 0;
    s->ec = S5L8702_DISP_WIDTH - 1;
    s->ep = S5L8702_DISP_HEIGHT - 1;
    s->command = s->parameter = s->reply_pos = s->reply_len = 0;
    s->sleeping = true;
    s->display_on = false;
    s->invalidate = true;
}

static int lcd_post_load(void *opaque, int version_id)
{
    S5L8702LcdState *s = opaque;

    if (s->reply_len > sizeof(s->reply) || s->reply_pos > s->reply_len ||
        s->x >= S5L8702_DISP_WIDTH || s->y >= S5L8702_DISP_HEIGHT) {
        return -EINVAL;
    }
    s->invalidate = true;
    return 0;
}

static const VMStateDescription vmstate_s5l8702_lcd = {
    .name = TYPE_S5L8702_LCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lcd_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8702LcdState, 0x90 / 4),
        VMSTATE_UINT64_ARRAY(panel_regs, S5L8702LcdState, 256),
        VMSTATE_UINT16_ARRAY(framebuffer, S5L8702LcdState, S5L8702_LCD_PIXELS),
        VMSTATE_UINT16(sc, S5L8702LcdState),
        VMSTATE_UINT16(ec, S5L8702LcdState),
        VMSTATE_UINT16(sp, S5L8702LcdState),
        VMSTATE_UINT16(ep, S5L8702LcdState),
        VMSTATE_UINT16(x, S5L8702LcdState),
        VMSTATE_UINT16(y, S5L8702LcdState),
        VMSTATE_UINT8(command, S5L8702LcdState),
        VMSTATE_UINT8(parameter, S5L8702LcdState),
        VMSTATE_UINT8_ARRAY(reply, S5L8702LcdState, 4),
        VMSTATE_UINT8(reply_pos, S5L8702LcdState),
        VMSTATE_UINT8(reply_len, S5L8702LcdState),
        VMSTATE_BOOL(sleeping, S5L8702LcdState),
        VMSTATE_BOOL(display_on, S5L8702LcdState),
        VMSTATE_BOOL(te_enabled, S5L8702LcdState),
        VMSTATE_TIMER_PTR(te_timer, S5L8702LcdState),
        VMSTATE_END_OF_LIST()
    },
};

static void s5l8702_lcd_init(Object *obj)
{
    S5L8702LcdState *s = S5L8702_LCD(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_lcd_ops, s,
                          TYPE_S5L8702_LCD, S5L8702_LCD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->te_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcd_te_timer, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->te, "te", 1);
    s->con = graphic_console_init(DEVICE(obj), 0, &lcd_ops, s);
    qemu_console_resize(s->con, S5L8702_DISP_WIDTH, S5L8702_DISP_HEIGHT);
}

static void s5l8702_lcd_finalize(Object *obj)
{
    S5L8702LcdState *s = S5L8702_LCD(obj);

    timer_free(s->te_timer);
}

static void s5l8702_lcd_realize(DeviceState *dev, Error **errp)
{
    S5L8702LcdState *s = S5L8702_LCD(dev);

    if (!s->refresh_rate || s->refresh_rate > 240) {
        error_setg(errp, "s5l8702-lcd: refresh-rate must be between 1 and 240");
    }
}

/*
 * Unmeasured panel oscillator approximation, not an S5L8702 register setting.
 * Neither transfer timings (+0x78/+0x7c) nor command 0x35 establish 60 Hz.
 */
static Property lcd_properties[] = {
    DEFINE_PROP_UINT32("refresh-rate", S5L8702LcdState, refresh_rate, 60),
    DEFINE_PROP_END_OF_LIST(),
};

static void s5l8702_lcd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 LCD interface and panel";
    dc->realize = s5l8702_lcd_realize;
    device_class_set_props(dc, lcd_properties);
    dc->user_creatable = false;
    dc->vmsd = &vmstate_s5l8702_lcd;
    RESETTABLE_CLASS(klass)->phases.enter = s5l8702_lcd_reset;
}

static const TypeInfo s5l8702_lcd_types[] = {
    {
        .name = TYPE_S5L8702_LCD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(S5L8702LcdState),
        .instance_init = s5l8702_lcd_init,
        .instance_finalize = s5l8702_lcd_finalize,
        .class_init = s5l8702_lcd_class_init,
    },
};
DEFINE_TYPES(s5l8702_lcd_types)
