/*
 * S5L8702 SHA-1 engine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boot ROM 0x20002044 loads sixteen little-endian words at +0x40 and
 * strobes CONFIG bit 1. Bit 3 preserves the previous compression state.
 * Padding belongs to the guest; retaining the whole message is unnecessary.
 * retailOS 2.0.4 0x08081680 selects DMA at +0x80, source at +0x84 and
 * byte count at +0x8c. 0x08091bc0 restores intermediate digest words before
 * continuing, allowing software contexts to share the engine.
 */
#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/misc/s5l8702-sha.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SHA_CONFIG  0x00
#define SHA_RESET   0x04
#define SHA_STATUS  0x08
#define SHA_OUTPUT  0x20
#define SHA_INPUT   0x40
#define SHA_DMA_CONTROL 0x80
#define SHA_DMA_SOURCE  0x84
#define SHA_DMA_LENGTH  0x8c
#define SHA_DMA_MAX     0xffc0
#define SHA_START   2
#define SHA_IRQ_EN  4
#define SHA_CONTINUE 8

static void sha_update_irq(S5L8702ShaState *s)
{
    qemu_set_irq(s->irq, s->status && (s->config & SHA_IRQ_EN));
}

static void sha_init_digest(uint32_t digest[5])
{
    digest[0] = 0x67452301;
    digest[1] = 0xefcdab89;
    digest[2] = 0x98badcfe;
    digest[3] = 0x10325476;
    digest[4] = 0xc3d2e1f0;
}

static void sha_compress(uint32_t digest[5], const uint8_t block[64])
{
    uint32_t w[80], a, b, c, d, e;

    for (unsigned i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + i * 4);
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = digest[0];
    b = digest[1];
    c = digest[2];
    d = digest[3];
    e = digest[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    digest[0] += a;
    digest[1] += b;
    digest[2] += c;
    digest[3] += d;
    digest[4] += e;
}

static bool sha_process(S5L8702ShaState *s, bool continuation)
{
    uint32_t digest[5];
    uint8_t block[64];

    memcpy(digest, s->digest, sizeof(digest));
    if (!continuation) {
        sha_init_digest(digest);
    }
    if (s->dma_control & 1) {
        MemoryRegionSection section;
        bool valid;

        /* The firmware splits DMA into at most 0xffc0-byte transfers. */
        if (!s->dma_length || s->dma_length > SHA_DMA_MAX ||
            (s->dma_length & 63) ||
            (uint64_t)s->dma_source + s->dma_length > (1ULL << 32)) {
            goto invalid_dma;
        }
        section = memory_region_find(get_system_memory(), s->dma_source,
                                     s->dma_length);
        valid = section.mr && memory_region_is_ram(section.mr) &&
                int128_eq(section.size, int128_make64(s->dma_length));
        if (section.mr) {
            memory_region_unref(section.mr);
        }
        if (!valid) {
            goto invalid_dma;
        }
        for (unsigned offset = 0; offset < s->dma_length; offset += 64) {
            if (address_space_read(&address_space_memory,
                                   (hwaddr)s->dma_source + offset,
                                   MEMTXATTRS_UNSPECIFIED, block, 64) !=
                MEMTX_OK) {
                goto invalid_dma;
            }
            sha_compress(digest, block);
        }
    } else {
        for (unsigned i = 0; i < 16; i++) {
            stl_le_p(block + i * 4, s->input[i]);
        }
        sha_compress(digest, block);
    }
    memcpy(s->digest, digest, sizeof(digest));
    return true;

invalid_dma:
    /* Preserve the old digest; the hardware's error status is not decoded. */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "s5l8702-sha: invalid DMA source 0x%08x length 0x%x\n",
                  s->dma_source, s->dma_length);
    return false;
}

static uint64_t sha_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702ShaState *s = opaque;

    switch (offset) {
    case SHA_CONFIG:
        return s->config;
    case SHA_RESET:
        return 0;
    case SHA_STATUS:
        return s->status;
    case SHA_OUTPUT ... SHA_OUTPUT + 4 * 4:
        return bswap32(s->digest[(offset - SHA_OUTPUT) / 4]);
    case SHA_INPUT ... SHA_INPUT + 15 * 4:
        return s->input[(offset - SHA_INPUT) / 4];
    case SHA_DMA_CONTROL:
        return s->dma_control;
    case SHA_DMA_SOURCE:
        return s->dma_source;
    case SHA_DMA_LENGTH:
        return s->dma_length;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-sha: read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void sha_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8702ShaState *s = opaque;

    switch (offset) {
    case SHA_CONFIG:
        s->config = value & ~3u;
        if (value & SHA_START) {
            if (sha_process(s, value & SHA_CONTINUE)) {
                s->status = 1;
            }
        }
        sha_update_irq(s);
        break;
    case SHA_RESET:
        s->config = s->status = 0;
        s->dma_control = s->dma_source = s->dma_length = 0;
        memset(s->input, 0, sizeof(s->input));
        sha_init_digest(s->digest);
        sha_update_irq(s);
        break;
    case SHA_STATUS:
        s->status &= ~value;
        sha_update_irq(s);
        break;
    case SHA_INPUT ... SHA_INPUT + 15 * 4:
        s->input[(offset - SHA_INPUT) / 4] = value;
        break;
    case SHA_OUTPUT ... SHA_OUTPUT + 4 * 4:
        s->digest[(offset - SHA_OUTPUT) / 4] = bswap32(value);
        break;
    case SHA_DMA_CONTROL:
        s->dma_control = value;
        break;
    case SHA_DMA_SOURCE:
        s->dma_source = value;
        break;
    case SHA_DMA_LENGTH:
        s->dma_length = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702-sha: write at 0x%" HWADDR_PRIx
                      "\n", offset);
    }
}

static const MemoryRegionOps sha_ops = {
    .read = sha_read,
    .write = sha_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sha_reset_enter(Object *obj, ResetType type)
{
    S5L8702ShaState *s = S5L8702_SHA(obj);

    s->config = s->status = 0;
    s->dma_control = s->dma_source = s->dma_length = 0;
    memset(s->input, 0, sizeof(s->input));
    sha_init_digest(s->digest);
}

static void sha_reset_hold(Object *obj)
{
    sha_update_irq(S5L8702_SHA(obj));
}

static int sha_post_load(void *opaque, int version_id)
{
    sha_update_irq(opaque);
    return 0;
}

static const VMStateDescription sha_vmstate = {
    .name = TYPE_S5L8702_SHA,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sha_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(config, S5L8702ShaState),
        VMSTATE_UINT32(status, S5L8702ShaState),
        VMSTATE_UINT32_ARRAY(input, S5L8702ShaState, 16),
        VMSTATE_UINT32_ARRAY(digest, S5L8702ShaState, 5),
        VMSTATE_UINT32(dma_control, S5L8702ShaState),
        VMSTATE_UINT32(dma_source, S5L8702ShaState),
        VMSTATE_UINT32(dma_length, S5L8702ShaState),
        VMSTATE_END_OF_LIST()
    },
};

static void sha_init(Object *obj)
{
    S5L8702ShaState *s = S5L8702_SHA(obj);

    memory_region_init_io(&s->iomem, obj, &sha_ops, s,
                          TYPE_S5L8702_SHA, S5L8702_SHA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void sha_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &sha_vmstate;
    rc->phases.enter = sha_reset_enter;
    rc->phases.hold = sha_reset_hold;
}

static const TypeInfo sha_type = {
    .name = TYPE_S5L8702_SHA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702ShaState),
    .instance_init = sha_init,
    .class_init = sha_class_init,
};

static void sha_register_types(void)
{
    type_register_static(&sha_type);
}
type_init(sha_register_types)
