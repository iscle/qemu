/*
 * S5L8702 AES DMA engine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boot ROM 0x20001a20 and 0x20001b8c establish the key register ordering
 * and mode fields. Fused keys are not available in a firmware dump; an
 * explicit property permits identity transfers for pre-decrypted images.
 */
#include "qemu/osdep.h"
#include "crypto/cipher.h"
#include "exec/address-spaces.h"
#include "hw/misc/s5l8702-aes.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define REG(s, offset) ((s)->regs[(offset) / 4])

/* DMA must not re-enter devices through a guest-controlled MMIO address. */
static bool aes_dma_ram(hwaddr addr, uint32_t len, bool write)
{
    MemoryRegionSection section = memory_region_find(get_system_memory(),
                                                     addr, len);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 (!write || !memory_region_is_rom(section.mr)) &&
                 int128_get64(section.size) == len;

    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid;
}

static void aes_run(S5L8702AesState *s)
{
    static const QCryptoCipherAlgorithm algorithms[] = {
        QCRYPTO_CIPHER_ALG_AES_128,
        QCRYPTO_CIPHER_ALG_AES_192,
        QCRYPTO_CIPHER_ALG_AES_256,
    };
    g_autoptr(QCryptoCipher) cipher = NULL;
    uint8_t data[4096], output[4096], key[32], iv[16];
    uint32_t len = REG(s, AES_INSIZE);
    uint32_t src = REG(s, AES_INADDR), dst = REG(s, AES_OUTADDR);
    uint32_t mode = REG(s, AES_KEYLEN);
    unsigned key_size = (mode >> 4) & 3;
    bool encrypt = mode & 1;
    bool cbc = mode & 8;
    bool fused = REG(s, AES_TYPE) != 0;

    if (!len) {
        REG(s, AES_OUTSIZE) = 0;
        REG(s, AES_STATUS) = 0xf;
        return;
    }
    if ((len & 15) || !aes_dma_ram(src, len, false) ||
        !aes_dma_ram(dst, len, true)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s5l8702-aes: invalid DMA 0x%x -> 0x%x (0x%x bytes)\n",
                      src, dst, len);
        return;
    }
    if (fused && !s->fused_key_bypass) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-aes: fused key unavailable\n");
        return;
    }
    if (!fused) {
        unsigned key_bytes = 16 + key_size * 8;

        if (key_size >= ARRAY_SIZE(algorithms)) {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-aes: invalid key size\n");
            return;
        }
        /* Shorter keys are right-aligned in the eight-word register bank. */
        for (unsigned i = 0; i < key_bytes / 4; i++) {
            stl_be_p(key + i * 4,
                     REG(s, AES_TYPE - key_bytes + i * 4));
        }
        for (unsigned i = 0; i < 4; i++) {
            stl_be_p(iv + i * 4, REG(s, AES_IV_REG + i * 4));
        }
        cipher = qcrypto_cipher_new(algorithms[key_size],
                                    cbc ? QCRYPTO_CIPHER_MODE_CBC :
                                          QCRYPTO_CIPHER_MODE_ECB,
                                    key, key_bytes, NULL);
        if (!cipher || (cbc && qcrypto_cipher_setiv(cipher, iv, 16, NULL))) {
            qemu_log_mask(LOG_UNIMP, "s5l8702-aes: cipher unavailable\n");
            return;
        }
    }
    for (uint32_t off = 0; off < len;) {
        unsigned chunk = MIN(sizeof(data), len - off);
        int ret = 0;

        cpu_physical_memory_read(src + off, data, chunk);
        if (cipher) {
            if (encrypt) {
                ret = qcrypto_cipher_encrypt(cipher, data, output, chunk, NULL);
            } else {
                ret = qcrypto_cipher_decrypt(cipher, data, output, chunk, NULL);
            }
        }
        if (ret < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8702-aes: cipher failed\n");
            return;
        }
        cpu_physical_memory_write(dst + off, cipher ? output : data, chunk);
        off += chunk;
    }
    trace_s5l8702_aes_operation(encrypt ? "encrypted" : "decrypted",
                               len, src, dst);
    REG(s, AES_OUTSIZE) = len;
    REG(s, AES_STATUS) = 0xf;
}

static uint64_t aes_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702AesState *s = opaque;

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-aes: read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
    return REG(s, offset);
}

static void aes_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8702AesState *s = opaque;

    trace_s5l8702_aes_write(offset, value);
    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "s5l8702-aes: write at 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    switch (offset) {
    case AES_GO:
        if (value & 1) {
            aes_run(s);
        }
        break;
    case AES_STATUS:
        REG(s, offset) &= ~value;
        break;
    case AES_UNKREG0:
        /* The software reset strobe preserves the selected key source. */
        if (value & 1) {
            REG(s, AES_STATUS) = 0xf;
        }
        break;
    default:
        REG(s, offset) = value;
        break;
    }
}

static const MemoryRegionOps aes_ops = {
    .read = aes_read,
    .write = aes_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aes_reset_enter(Object *obj, ResetType type)
{
    S5L8702AesState *s = S5L8702_AES(obj);

    memset(s->regs, 0, sizeof(s->regs));
    REG(s, AES_STATUS) = 0xf;
}

static const VMStateDescription aes_vmstate = {
    .name = TYPE_S5L8702_AES,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8702AesState, 33),
        VMSTATE_END_OF_LIST()
    },
};

static Property aes_properties[] = {
    DEFINE_PROP_BOOL("fused-key-bypass", S5L8702AesState,
                     fused_key_bypass, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void aes_init(Object *obj)
{
    S5L8702AesState *s = S5L8702_AES(obj);

    memory_region_init_io(&s->iomem, obj, &aes_ops, s,
                          TYPE_S5L8702_AES, S5L8702_AES_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void aes_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = aes_reset_enter;
    dc->vmsd = &aes_vmstate;
    device_class_set_props(dc, aes_properties);
}

static const TypeInfo aes_type = {
    .name = TYPE_S5L8702_AES,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = aes_init,
    .instance_size = sizeof(S5L8702AesState),
    .class_init = aes_class_init,
};

static void aes_register_types(void)
{
    type_register_static(&aes_type);
}
type_init(aes_register_types)
