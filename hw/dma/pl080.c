/*
 * Arm PrimeCell PL080/PL081 DMA controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "hw/dma/pl080.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#define PL080_CONF_E    0x1
#define PL080_CONF_M1   0x2
#define PL080_CONF_M2   0x4

#define PL080_CCONF_H   0x40000
#define PL080_CCONF_A   0x20000
#define PL080_CCONF_L   0x10000
#define PL080_CCONF_ITC 0x08000
#define PL080_CCONF_IE  0x04000
#define PL080_CCONF_E   0x00001

#define PL080_CCTRL_I   0x80000000
#define PL080_CCTRL_DI  0x08000000
#define PL080_CCTRL_SI  0x04000000
#define PL080_CCTRL_D   0x02000000
#define PL080_CCTRL_S   0x01000000

static int pl080_post_load(void *opaque, int version_id);
static void pl080_run(PL080State *s);

enum {
    PL080_SINGLE,
    PL080_BURST,
    PL080_LAST_SINGLE,
    PL080_LAST_BURST,
};

/* Host work limit, not a hardware burst size or a timing parameter. */
#define PL080_WORK_LIMIT 1024

static const VMStateDescription vmstate_pl080_transfer = {
    .name = "pl080_transfer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fifo, PL080Transfer, PL080_FIFO_BYTES),
        VMSTATE_UINT8(len, PL080Transfer),
        VMSTATE_UINT16(src_left, PL080Transfer),
        VMSTATE_UINT16(dst_left, PL080Transfer),
        VMSTATE_UINT8(src_kind, PL080Transfer),
        VMSTATE_UINT8(dst_kind, PL080Transfer),
        VMSTATE_BOOL(started, PL080Transfer),
        VMSTATE_END_OF_LIST()
    }
};

static int pl080_pre_load(void *opaque)
{
    PL080State *s = opaque;

    qemu_bh_cancel(s->bh);
    memset(s->transfer, 0, sizeof(s->transfer));
    memset(s->hw_req, 0, sizeof(s->hw_req));
    s->req_last_single = 0;
    s->req_last_burst = 0;
    s->req_ack = 0;
    s->req_busy = 0;
    return 0;
}

static const VMStateDescription vmstate_pl080_channel = {
    .name = "pl080_channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(src, pl080_channel),
        VMSTATE_UINT32(dest, pl080_channel),
        VMSTATE_UINT32(lli, pl080_channel),
        VMSTATE_UINT32(ctrl, pl080_channel),
        VMSTATE_UINT32(conf, pl080_channel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pl080 = {
    .name = "pl080",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = pl080_pre_load,
    .post_load = pl080_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_mask, PL080State),
        VMSTATE_UINT8(err_int, PL080State),
        VMSTATE_UINT8(err_mask, PL080State),
        VMSTATE_UINT32(conf, PL080State),
        VMSTATE_UINT32(sync, PL080State),
        VMSTATE_UINT32(req_single, PL080State),
        VMSTATE_UINT32(req_burst, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_STRUCT_ARRAY(chan, PL080State, PL080_MAX_CHANNELS,
                             1, vmstate_pl080_channel, pl080_channel),
        VMSTATE_INT32(running, PL080State),
        VMSTATE_UINT32_V(req_last_single, PL080State, 2),
        VMSTATE_UINT32_V(req_last_burst, PL080State, 2),
        VMSTATE_UINT16_ARRAY_V(hw_req, PL080State, 4, 2),
        VMSTATE_UINT16_V(req_ack, PL080State, 2),
        VMSTATE_UINT16_V(req_busy, PL080State, 2),
        VMSTATE_STRUCT_ARRAY(transfer, PL080State, PL080_MAX_CHANNELS,
                             2, vmstate_pl080_transfer, PL080Transfer),
        VMSTATE_END_OF_LIST()
    }
};

static const unsigned char pl080_id[] =
{ 0x80, 0x10, 0x04, 0x0a, 0x0d, 0xf0, 0x05, 0xb1 };

static const unsigned char pl081_id[] =
{ 0x81, 0x10, 0x04, 0x0a, 0x0d, 0xf0, 0x05, 0xb1 };

static void pl080_update(PL080State *s)
{
    bool tclevel, errlevel;

    s->tc_mask = 0;
    s->err_mask = 0;
    for (unsigned c = 0; c < s->nchannels; c++) {
        if (s->chan[c].conf & PL080_CCONF_ITC) {
            s->tc_mask |= 1 << c;
        }
        if (s->chan[c].conf & PL080_CCONF_IE) {
            s->err_mask |= 1 << c;
        }
    }
    tclevel = s->tc_int & s->tc_mask;
    errlevel = s->err_int & s->err_mask;

    qemu_set_irq(s->interr, errlevel);
    qemu_set_irq(s->inttc, tclevel);
    qemu_set_irq(s->irq, errlevel || tclevel);
}

static void pl080_channel_error(PL080State *s, unsigned c)
{
    /* DDI 0196G sections 3.6/3.8: bus errors disable the channel. */
    s->err_int |= 1 << c;
    s->chan[c].conf &= ~PL080_CCONF_E;
}

static uint32_t *pl080_software_request(PL080State *s, unsigned kind)
{
    switch (kind) {
    case PL080_SINGLE:
        return &s->req_single;
    case PL080_BURST:
        return &s->req_burst;
    case PL080_LAST_SINGLE:
        return &s->req_last_single;
    case PL080_LAST_BURST:
        return &s->req_last_burst;
    default:
        g_assert_not_reached();
    }
}

static uint16_t pl080_hardware_requests(PL080State *s)
{
    return s->hw_req[0] | s->hw_req[1] | s->hw_req[2] | s->hw_req[3];
}

static bool pl080_request_pending(PL080State *s, unsigned id, unsigned kind)
{
    uint16_t bit = 1u << id;

    return !((s->req_ack | s->req_busy) & bit) &&
           ((s->hw_req[kind] | *pl080_software_request(s, kind)) & bit);
}

static void pl080_request_complete(PL080State *s, unsigned id, unsigned kind)
{
    uint16_t bit = 1u << id;

    *pl080_software_request(s, kind) &= ~bit;
    s->req_busy &= ~bit;
    if (pl080_hardware_requests(s) & bit) {
        /* B.6: hold CLR until every request from this peripheral is low. */
        s->req_ack |= bit;
        qemu_set_irq(s->request_clear[id], 1);
    }
}

static void pl080_abort(PL080State *s, unsigned c)
{
    PL080Transfer *t = &s->transfer[c];
    uint32_t conf = s->chan[c].conf;

    if (t->src_left) {
        s->req_busy &= ~(1u << ((conf >> 1) & 15));
    }
    if (t->dst_left) {
        s->req_busy &= ~(1u << ((conf >> 6) & 15));
    }
    memset(t, 0, sizeof(*t));
}

static void pl080_channel_complete(PL080State *s, unsigned c)
{
    pl080_channel *ch = &s->chan[c];
    uint32_t next_lli = ch->lli & ~3u;
    uint8_t descriptor[16];

    /* The completed transfer's I bit controls TC, before loading the next. */
    if (ch->ctrl & PL080_CCTRL_I) {
        s->tc_int |= 1 << c;
    }
    if (!next_lli) {
        ch->conf &= ~PL080_CCONF_E;
        return;
    }
    /* LLI bit 0 selects an AHB master; it is not part of the address. */
    if (address_space_read(&s->downstream_as, next_lli,
                           MEMTXATTRS_UNSPECIFIED, descriptor,
                           sizeof(descriptor)) != MEMTX_OK) {
        pl080_channel_error(s, c);
        return;
    }
    ch->src = ldl_le_p(descriptor);
    ch->dest = ldl_le_p(descriptor + 4);
    ch->lli = ldl_le_p(descriptor + 8);
    ch->ctrl = ldl_le_p(descriptor + 12);
}

static unsigned pl080_burst_size(unsigned encoded)
{
    return encoded ? 1u << (encoded + 1) : 1;
}

/* One bus access, through the channel's four-word FIFO. */
static bool pl080_transfer_step(PL080State *s, unsigned c)
{
    pl080_channel *ch = &s->chan[c];
    PL080Transfer *t = &s->transfer[c];
    unsigned flow = (ch->conf >> 11) & 7;
    unsigned swidth = 1u << ((ch->ctrl >> 18) & 7);
    unsigned dwidth = 1u << ((ch->ctrl >> 21) & 7);
    unsigned count = ch->ctrl & 0xfff;
    unsigned src_id = (ch->conf >> 1) & 15;
    unsigned dst_id = (ch->conf >> 6) & 15;
    unsigned remaining = count * swidth + t->len;
    bool src_peripheral = flow == 2 || flow == 3;
    bool dst_peripheral = flow == 1 || flow == 3;

    if (!(ch->conf & PL080_CCONF_E) || !remaining) {
        return false;
    }
    if (flow >= 4) {
        qemu_log_mask(LOG_UNIMP,
                      "pl080: peripheral flow control not implemented\n");
        return false;
    }
    if (swidth > 4 || dwidth > 4 || remaining % dwidth) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl080: channel %u: invalid width or count\n", c);
        return false;
    }

    if (dst_peripheral && !t->dst_left &&
        pl080_request_pending(s, dst_id, PL080_BURST)) {
        /* B.4.2: destinations use BREQ, including a short final burst. */
        t->dst_kind = PL080_BURST;
        t->dst_left = MIN(pl080_burst_size((ch->ctrl >> 15) & 7),
                          remaining / dwidth);
        s->req_busy |= 1u << dst_id;
    }
    if (t->len >= dwidth) {
        if (!dst_peripheral || t->dst_left) {
            if (address_space_write(&s->downstream_as, ch->dest,
                                    MEMTXATTRS_UNSPECIFIED, t->fifo,
                                    dwidth) != MEMTX_OK) {
                goto bus_error;
            }
            if (ch->ctrl & PL080_CCTRL_DI) {
                ch->dest += dwidth;
            }
            t->len -= dwidth;
            memmove(t->fifo, t->fifo + dwidth, t->len);
            if (dst_peripheral && !--t->dst_left) {
                pl080_request_complete(s, dst_id, t->dst_kind);
            }
            if (!count && !t->len && t->started) {
                t->started = false;
                pl080_channel_complete(s, c);
            }
            return true;
        }
    }

    /* Halt stops new source requests; accepted requests can finish. */
    if (!count || t->len + swidth > sizeof(t->fifo) ||
        ((ch->conf & PL080_CCONF_H) && !t->src_left)) {
        return false;
    }
    /* Section 3.8.2: memory-to-peripheral starts with a peripheral request. */
    if (flow == 1 && (!t->dst_left || t->len >= t->dst_left * dwidth)) {
        return false;
    }
    if (src_peripheral && !t->src_left) {
        unsigned burst = pl080_burst_size((ch->ctrl >> 12) & 7);

        if (count >= burst && pl080_request_pending(s, src_id, PL080_BURST)) {
            t->src_kind = PL080_BURST;
            t->src_left = burst;
        } else if (pl080_request_pending(s, src_id, PL080_SINGLE)) {
            t->src_kind = PL080_SINGLE;
            t->src_left = 1;
        } else {
            return false;
        }
        s->req_busy |= 1u << src_id;
    }
    if (address_space_read(&s->downstream_as, ch->src,
                           MEMTXATTRS_UNSPECIFIED, t->fifo + t->len,
                           swidth) != MEMTX_OK) {
        goto bus_error;
    }
    if (ch->ctrl & PL080_CCTRL_SI) {
        ch->src += swidth;
    }
    t->len += swidth;
    t->started = true;
    ch->ctrl = (ch->ctrl & ~0xfffu) | (count - 1);
    if (src_peripheral && !--t->src_left) {
        pl080_request_complete(s, src_id, t->src_kind);
    }
    return true;
bus_error:
    pl080_channel_error(s, c);
    pl080_abort(s, c);
    return true;
}

static void pl080_run(PL080State *s)
{
    unsigned work = 0;

    if (s->running || !(s->conf & PL080_CONF_E)) {
        return;
    }
    s->running = 1;
    while (work < PL080_WORK_LIMIT && (s->conf & PL080_CONF_E)) {
        unsigned c;

        for (c = 0; c < s->nchannels; c++) {
            if (pl080_transfer_step(s, c)) {
                break;
            }
        }
        if (c == s->nchannels) {
            break;
        }
        work++;
    }
    s->running = 0;
    if (work == PL080_WORK_LIMIT) {
        /* Yield to the main loop even for a cyclic list or recursive DREQ. */
        qemu_bh_schedule(s->bh);
    }
    pl080_update(s);
}

static void pl080_bh(void *opaque)
{
    pl080_run(opaque);
}

static void pl080_request_input(PL080State *s, unsigned kind,
                                 unsigned id, int level)
{
    uint16_t bit = 1u << id;

    s->hw_req[kind] = (s->hw_req[kind] & ~bit) | (level ? bit : 0);
    if ((s->req_ack & bit) && !(pl080_hardware_requests(s) & bit)) {
        s->req_ack &= ~bit;
        qemu_set_irq(s->request_clear[id], 0);
    }
    pl080_run(s);
}

#define PL080_REQUEST_INPUT(name, kind) \
    static void name(void *opaque, int id, int level) \
    { \
        pl080_request_input(opaque, kind, id, level); \
    }

PL080_REQUEST_INPUT(pl080_single, PL080_SINGLE)
PL080_REQUEST_INPUT(pl080_burst, PL080_BURST)
PL080_REQUEST_INPUT(pl080_last_single, PL080_LAST_SINGLE)
PL080_REQUEST_INPUT(pl080_last_burst, PL080_LAST_BURST)

static uint64_t pl080_read(void *opaque, hwaddr offset,
                           unsigned size)
{
    PL080State *s = (PL080State *)opaque;
    uint32_t i;
    uint32_t mask;

    if (offset >= 0xfe0 && offset < 0x1000) {
        if (s->nchannels == 8) {
            return pl080_id[(offset - 0xfe0) >> 2];
        } else {
            return pl081_id[(offset - 0xfe0) >> 2];
        }
    }
    if (offset >= 0x100 && offset < 0x200) {
        i = (offset & 0xe0) >> 5;
        if (i >= s->nchannels)
            goto bad_offset;
        switch ((offset >> 2) & 7) {
        case 0: /* SrcAddr */
            return s->chan[i].src;
        case 1: /* DestAddr */
            return s->chan[i].dest;
        case 2: /* LLI */
            return s->chan[i].lli;
        case 3: /* Control */
            /* 2.4: readback counts destination progress in source units. */
            mask = 1u << ((s->chan[i].ctrl >> 18) & 7);
            return (s->chan[i].ctrl & ~0xfffu) |
                   (((s->chan[i].ctrl & 0xfff) +
                     s->transfer[i].len / mask) & 0xfff);
        case 4: /* Configuration */
            return s->chan[i].conf |
                   (s->transfer[i].len ? PL080_CCONF_A : 0);
        default:
            goto bad_offset;
        }
    }
    switch (offset >> 2) {
    case 0: /* IntStatus */
        return (s->tc_int & s->tc_mask) | (s->err_int & s->err_mask);
    case 1: /* IntTCStatus */
        return (s->tc_int & s->tc_mask);
    case 3: /* IntErrorStatus */
        return (s->err_int & s->err_mask);
    case 5: /* RawIntTCStatus */
        return s->tc_int;
    case 6: /* RawIntErrorStatus */
        return s->err_int;
    case 7: /* EnbldChns */
        mask = 0;
        for (i = 0; i < s->nchannels; i++) {
            if (s->chan[i].conf & PL080_CCONF_E)
                mask |= 1 << i;
        }
        return mask;
    case 8: /* SoftBReq */
        return s->req_burst;
    case 9: /* SoftSReq */
        return s->req_single;
    case 10: /* SoftLBReq */
        return s->req_last_burst;
    case 11: /* SoftLSReq */
        return s->req_last_single;
    case 12: /* Configuration */
        return s->conf;
    case 13: /* Sync */
        return s->sync;
    default:
    bad_offset:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl080_read: Bad offset %x\n", (int)offset);
        return 0;
    }
}

static void pl080_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    PL080State *s = (PL080State *)opaque;
    int i;

    if (offset >= 0x100 && offset < 0x200) {
        i = (offset & 0xe0) >> 5;
        if (i >= s->nchannels)
            goto bad_offset;
        switch ((offset >> 2) & 7) {
        case 0: /* SrcAddr */
            s->chan[i].src = value;
            break;
        case 1: /* DestAddr */
            s->chan[i].dest = value;
            break;
        case 2: /* LLI */
            s->chan[i].lli = value;
            break;
        case 3: /* Control */
            s->chan[i].ctrl = value;
            break;
        case 4: /* Configuration */
            if (!(value & PL080_CCONF_E)) {
                pl080_abort(s, i);
            }
            s->chan[i].conf = value & ~PL080_CCONF_A;
            pl080_run(s);
            pl080_update(s);
            break;
        }
        return;
    }
    switch (offset >> 2) {
    case 2: /* IntTCClear */
        s->tc_int &= ~value;
        break;
    case 4: /* IntErrorClear */
        s->err_int &= ~value;
        break;
    case 8: /* SoftBReq */
        s->req_burst |= value & 0xffff;
        pl080_run(s);
        break;
    case 9: /* SoftSReq */
        s->req_single |= value & 0xffff;
        pl080_run(s);
        break;
    case 10: /* SoftLBReq */
        s->req_last_burst |= value & 0xffff;
        pl080_run(s);
        break;
    case 11: /* SoftLSReq */
        s->req_last_single |= value & 0xffff;
        pl080_run(s);
        break;
    case 12: /* Configuration */
        s->conf = value;
        if (s->conf & (PL080_CONF_M1 | PL080_CONF_M2)) {
            qemu_log_mask(LOG_UNIMP,
                          "pl080_write: Big-endian DMA not implemented\n");
        }
        pl080_run(s);
        break;
    case 13: /* Sync */
        s->sync = value;
        break;
    default:
    bad_offset:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl080_write: Bad offset %x\n", (int)offset);
    }
    pl080_update(s);
}

static bool pl080_accepts(void *opaque, hwaddr addr, unsigned size,
                           bool is_write, MemTxAttrs attrs)
{
    PL080State *s = opaque;

    /* Reject DMA that would recursively reprogram the active controller. */
    return !is_write || !s->running;
}

static const MemoryRegionOps pl080_ops = {
    .read = pl080_read,
    .write = pl080_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.accepts = pl080_accepts,
};

static void pl080_reset_enter(Object *obj, ResetType type)
{
    PL080State *s = PL080(obj);
    int i;

    s->tc_int = 0;
    s->tc_mask = 0;
    s->err_int = 0;
    s->err_mask = 0;
    s->conf = 0;
    s->sync = 0;
    s->req_single = 0;
    s->req_burst = 0;
    s->req_last_single = 0;
    s->req_last_burst = 0;
    s->req_ack = 0;
    s->req_busy = 0;
    s->running = 0;
    memset(s->transfer, 0, sizeof(s->transfer));
    qemu_bh_cancel(s->bh);

    for (i = 0; i < s->nchannels; i++) {
        s->chan[i].src = 0;
        s->chan[i].dest = 0;
        s->chan[i].lli = 0;
        s->chan[i].ctrl = 0;
        s->chan[i].conf = 0;
    }
}

static void pl080_reset_hold(Object *obj)
{
    PL080State *s = PL080(obj);

    pl080_update(s);
    for (unsigned id = 0; id < PL080_NUM_PERIPHERALS; id++) {
        qemu_set_irq(s->request_clear[id], 0);
    }
}

static int pl080_post_load(void *opaque, int version_id)
{
    PL080State *s = opaque;

    for (unsigned c = 0; c < PL080_MAX_CHANNELS; c++) {
        PL080Transfer *t = &s->transfer[c];

        if (t->len > sizeof(t->fifo) || t->src_left > 256 ||
            t->dst_left > 256 || t->src_kind > PL080_LAST_BURST ||
            t->dst_kind > PL080_LAST_BURST) {
            return -EINVAL;
        }
        s->chan[c].conf &= ~PL080_CCONF_A;
    }
    s->running = 1;
    pl080_update(s);
    for (unsigned id = 0; id < PL080_NUM_PERIPHERALS; id++) {
        qemu_set_irq(s->request_clear[id], !!(s->req_ack & (1u << id)));
    }
    s->running = 0;
    qemu_bh_schedule(s->bh);
    return 0;
}

static void pl080_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PL080State *s = PL080(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &pl080_ops, s, "pl080", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->interr);
    sysbus_init_irq(sbd, &s->inttc);
    qdev_init_gpio_in_named(DEVICE(obj), pl080_single, "dreq-single",
                            PL080_NUM_PERIPHERALS);
    qdev_init_gpio_in_named(DEVICE(obj), pl080_burst, "dreq-burst",
                            PL080_NUM_PERIPHERALS);
    qdev_init_gpio_in_named(DEVICE(obj), pl080_last_single, "dreq-last-single",
                            PL080_NUM_PERIPHERALS);
    qdev_init_gpio_in_named(DEVICE(obj), pl080_last_burst, "dreq-last-burst",
                            PL080_NUM_PERIPHERALS);
    qdev_init_gpio_out_named(DEVICE(obj), s->request_clear, "dreq-clear",
                             PL080_NUM_PERIPHERALS);
    s->bh = qemu_bh_new(pl080_bh, s);
    s->nchannels = 8;
}

static void pl080_realize(DeviceState *dev, Error **errp)
{
    PL080State *s = PL080(dev);

    if (!s->downstream) {
        error_setg(errp, "PL080 'downstream' link not set");
        return;
    }

    address_space_init(&s->downstream_as, s->downstream, "pl080-downstream");
}

static void pl081_init(Object *obj)
{
    PL080State *s = PL080(obj);

    s->nchannels = 2;
}

static void pl080_unrealize(DeviceState *dev)
{
    PL080State *s = PL080(dev);

    qemu_bh_cancel(s->bh);
    address_space_destroy(&s->downstream_as);
}

static void pl080_finalize(Object *obj)
{
    qemu_bh_delete(PL080(obj)->bh);
}

static Property pl080_properties[] = {
    DEFINE_PROP_LINK("downstream", PL080State, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void pl080_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_pl080;
    dc->realize = pl080_realize;
    dc->unrealize = pl080_unrealize;
    device_class_set_props(dc, pl080_properties);
    rc->phases.enter = pl080_reset_enter;
    rc->phases.hold = pl080_reset_hold;
}

static const TypeInfo pl080_info = {
    .name          = TYPE_PL080,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL080State),
    .instance_init = pl080_init,
    .instance_finalize = pl080_finalize,
    .class_init    = pl080_class_init,
};

static const TypeInfo pl081_info = {
    .name          = TYPE_PL081,
    .parent        = TYPE_PL080,
    .instance_init = pl081_init,
};

/* The PL080 and PL081 are the same except for the number of channels
   they implement (8 and 2 respectively).  */
static void pl080_register_types(void)
{
    type_register_static(&pl080_info);
    type_register_static(&pl081_info);
}

type_init(pl080_register_types)
