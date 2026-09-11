#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/block-backend.h"
#include "exec/cpu-common.h"
#include "hw/ide/s5l8702-ata.h"
#include "trace.h"

/*
 * Verbose register/transfer tracing, off by default. Enable with ATA_DEBUG=1
 * in the environment. Kept out of the hot path so it neither slows nor
 * perturbs the timing-sensitive retailOS boot.
 */
static inline bool ata_debug_enabled(void)
{
    static int e = -1;
    if (e < 0) {
        e = getenv("ATA_DEBUG") ? 1 : 0;
    }
    return e;
}
#define ATADBG(...) do { \
    if (ata_debug_enabled()) { \
        printf(__VA_ARGS__); \
    } \
} while (0)

#define ATA_DMA_ADDR 0x38700088

#define ATA_CONTROL 0x0000
#define ATA_STATUS 0x0004
#define ATA_COMMAND 0x0008
#define ATA_SWRST 0x000c
#define ATA_IRQ 0x0010
#define ATA_IRQ_MASK 0x0014
#define ATA_CFG 0x0018
#define ATA_MDMA_TIME 0x0028
#define ATA_PIO_TIME 0x002c
#define ATA_UDMA_TIME 0x0030
#define ATA_XFR_NUM 0x0034
#define ATA_XFR_CNT 0x0038
#define ATA_TBUF_START 0x003c
#define ATA_TBUF_SIZE 0x0040
#define ATA_SBUF_START 0x0044
#define ATA_SBUF_SIZE 0x0048
#define ATA_CADR_TBUF 0x004c
#define ATA_CADR_SBUF 0x0050
#define ATA_PIO_DTR 0x0054
#define ATA_PIO_FED 0x0058
#define ATA_PIO_SCR 0x005c
#define ATA_PIO_LLR 0x0060
#define ATA_PIO_LMR 0x0064
#define ATA_PIO_LHR 0x0068
#define ATA_PIO_DVR 0x006c
#define ATA_PIO_CSD 0x0070
#define ATA_PIO_DAD 0x0074
#define ATA_PIO_READY 0x0078
#define ATA_PIO_RDATA 0x007c
#define ATA_BUS_FIFO_STATUS 0x0080
#define ATA_FIFO_STATUS 0x0084

/* ATA_CONTROL */
#define CLK_DOWN_READY          BIT(1)
#define ATA_ENABLE              BIT(0)

/* ATA_STATUS */
#define ATADEV_CBLID             BIT(5)
#define ATADEV_IRQ               BIT(4)
#define ATADEV_IORDY             BIT(3)
#define ATADEV_DMAREQ            BIT(2)
#define ATADEV_XFR_STATE         BIT(0)
#define ATADEV_XFR_STATE_MASK    0x3

/* ATA_COMMAND */
#define XFR_COMMAND             BIT(0)
#define XFR_COMMAND_MASK        0x3

/* ATA_SWRST */
#define ATA_SWRSTN                BIT(0)

/* ATA_IRQ */
#define SBUF_EMPTY_INT          BIT(4)
#define TBUF_FULL_INT           BIT(3)
#define ATADEV_IRQ_INT          BIT(2)
#define UDMA_HOLD_INT           BIT(1)
#define XFR_DONE_INT            BIT(0)

/* ATA_IRQ_MASK */
#define MASK_SBUT_EMPTY_INT     BIT(4)
#define MASK_TBUF_FULL_INT      BIT(3)
#define MASK_ATADEV_IRQ_INT     BIT(2)
#define MASK_UDMA_HOLD_INT      BIT(1)
#define MASK_XFR_DONE_INT       BIT(0)

/* ATA_CFG */
#define UDMA_AUTO_MODE  BIT(9)
#define SBUF_FULL_MODE  BIT(8)
#define SBUF_EMPTY_MODE BIT(8)
#define TBUF_FULL_MODE  BIT(7)
#define BYTE_SWAP       BIT(6)
#define ATADEV_IRQ_AL   BIT(5)
#define DMA_DIR         BIT(4)
#define ATA_CLASS       BIT(2)
#define ATA_CLASS_MASK  0xc
#define ATA_IORDY_EN    BIT(1)
#define ATA_RST         BIT(0)

/* ATA_PIO_READY */
#define DEV_ACC_READY   BIT(1)
#define PIO_DATA_READY  BIT(0)

/* --- data transfer helpers --- */

static void s5l8702_ata_update_irq(S5L8702AtaState *s);

static BlockBackend *s5l8702_ata_blk(S5L8702AtaState *s)
{
    if (!s->blk) {
        s->blk = s->bus.ifs[0].blk;
    }
    return s->blk;
}

static uint64_t s5l8702_ata_total_sectors(S5L8702AtaState *s)
{
    BlockBackend *blk = s5l8702_ata_blk(s);
    int64_t len = blk ? blk_getlength(blk) : 0;
    if (len < 0) {
        len = 0;
    }
    return len / 512;
}

static uint32_t s5l8702_ata_lba28(S5L8702AtaState *s)
{
    return (s->ata_pio_llr & 0xff) |
           ((s->ata_pio_lmr & 0xff) << 8) |
           ((s->ata_pio_lhr & 0xff) << 16) |
           ((s->ata_pio_dvr & 0x0f) << 24);
}

static uint32_t s5l8702_ata_count(S5L8702AtaState *s)
{
    uint32_t c = s->ata_pio_scr & 0xff;
    return c ? c : 256;
}

static void s5l8702_ata_buf_reserve(S5L8702AtaState *s, uint32_t bytes)
{
    if (bytes > s->xfer_cap) {
        s->xfer_buf = g_realloc(s->xfer_buf, bytes);
        s->xfer_cap = bytes;
    }
}

static void s5l8702_ata_build_identify(S5L8702AtaState *s)
{
    uint64_t sectors = s5l8702_ata_total_sectors(s);
    uint16_t id[256];

    memset(id, 0, sizeof(id));
    id[0] = 0x0040;                 /* fixed device */
    id[1] = 16383;                  /* logical cylinders */
    id[3] = 16;                     /* heads */
    id[6] = 63;                     /* sectors per track */
    id[22] = 4;
    /*
     * Serial (words 10-19), firmware (23-26), model (27-46): ASCII, byte-
     * swapped
     */
    memcpy((char *)&id[10], "0000000000000000IPOD", 20);
    memcpy((char *)&id[23], "1.0     ", 8);
    memcpy((char *)&id[27], "iPod HDD (emulated)                     ", 40);
    for (int i = 10; i < 47; i++) {
        /* store big-endian ATA string order */
        id[i] = (id[i] >> 8) | (id[i] << 8);
    }
    id[47] = 0x8000 | 0x80;         /* max sectors per interrupt */
    id[49] = 0x0300;                /* LBA + DMA supported */
    id[50] = 0x4000;
    id[53] = 0x0007;                /* words 54-58, 64-70, 88 valid */
    id[54] = id[1];
    id[55] = id[3];
    id[56] = id[6];
    {
        uint32_t chs = 16383u * 16u * 63u;
        id[57] = chs & 0xffff;
        id[58] = chs >> 16;
    }
    if (sectors > 0x0fffffff) {
        sectors = 0x0fffffff;       /* keep LBA28 path in the guest driver */
    }
    id[60] = sectors & 0xffff;      /* LBA28 total sectors */
    id[61] = sectors >> 16;
    /* MDMA modes 0-2 supported, mode 2 selected */
    id[63] = 0x0407;
    id[64] = 0x0003;                /* PIO modes 3-4 */
    id[65] = id[66] = id[67] = id[68] = 120;
    id[75] = 0x001f;
    id[80] = 0x00fe;                /* ATA-1..7 */
    id[82] = 0x0000;
    /* bit14 set, LBA48 (bit10) not advertised */
    id[83] = 0x4000;
    id[84] = 0x4000;
    id[85] = 0x0000;
    id[86] = 0x0000;
    id[87] = 0x4000;
    /* UDMA modes 0-5 supported, mode 5 selected */
    id[88] = 0x203f;
    id[93] = 0x600b;

    s5l8702_ata_buf_reserve(s, 512);
    for (int i = 0; i < 256; i++) {
        s->xfer_buf[i * 2] = id[i] & 0xff;
        s->xfer_buf[i * 2 + 1] = id[i] >> 8;
    }
    s->xfer_len = 512;
    s->xfer_pos = 0;
    s->xfer_is_write = 0;
    s->drq = 1;
}

/* Read cur_count sectors at cur_lba into the PIO buffer. */
static void s5l8702_ata_pio_read_start(S5L8702AtaState *s)
{
    BlockBackend *blk = s5l8702_ata_blk(s);
    uint32_t bytes = s->cur_count * 512;

    s5l8702_ata_buf_reserve(s, bytes);
    if (blk && blk_pread(blk, (int64_t)s->cur_lba * 512, bytes,
                         s->xfer_buf, 0) < 0) {
        ATADBG("%s: blk_pread failed lba=%u count=%u\n", __func__,
               s->cur_lba, s->cur_count);
        memset(s->xfer_buf, 0, bytes);
    }
    s->xfer_len = bytes;
    s->xfer_pos = 0;
    s->xfer_is_write = 0;
    s->drq = 1;
}

/* Perform the deferred DMA transfer triggered by COMMAND=start. */
static void s5l8702_ata_dma_run(S5L8702AtaState *s)
{
    BlockBackend *blk = s5l8702_ata_blk(s);
    uint32_t bytes = s->cur_count * 512;
    uint32_t dma_addr;

    s5l8702_ata_buf_reserve(s, bytes);
    if (s->dma_is_write) {
        uint32_t src = s->ata_sbuf_start & 0x7fffffff;
        cpu_physical_memory_read(src, s->xfer_buf, bytes);
        if (blk) {
            blk_pwrite(blk, (int64_t)s->cur_lba * 512, bytes, s->xfer_buf, 0);
        }
        ATADBG("%s: DMA WRITE lba=%u count=%u src=0x%08x bytes=%u\n",
               __func__, s->cur_lba, s->cur_count, src, bytes);
    } else {
        uint32_t dst = s->ata_tbuf_start & 0x7fffffff;
        if (blk && blk_pread(blk, (int64_t)s->cur_lba * 512, bytes,
                             s->xfer_buf, 0) < 0) {
            memset(s->xfer_buf, 0, bytes);
        }
        cpu_physical_memory_write(dst, s->xfer_buf, bytes);
        ATADBG("%s: DMA READ lba=%u count=%u dst=0x%08x bytes=%u\n",
               __func__, s->cur_lba, s->cur_count, dst, bytes);

    }
    if (s->dma_is_write) {
        dma_addr = s->ata_sbuf_start;
    } else {
        dma_addr = s->ata_tbuf_start;
    }
    trace_s5l8702_ata_dma(s->dma_is_write, s->cur_lba, s->cur_count, dma_addr);
    s->dma_pending = 0;
    s->drq = 0;
    s->ata_irq |= XFR_DONE_INT | ATADEV_IRQ_INT;
    /*
     * Raise the ATA interrupt line: the EFI polls, but the retailOS is
     * interrupt-driven and idles in WFI until this fires.
     */
    s5l8702_ata_update_irq(s);
}

/*
 * Drive the ATA interrupt line from the pending (4:0) bits gated by the mask.
 */
static void s5l8702_ata_update_irq(S5L8702AtaState *s)
{
    qemu_set_irq(s->irq, (s->ata_irq & s->ata_irq_mask & 0x1f) != 0);
}

static void s5l8702_ata_command(S5L8702AtaState *s, uint32_t cmd)
{
    s->cur_cmd = cmd;
    s->cur_lba = s5l8702_ata_lba28(s);
    s->cur_count = s5l8702_ata_count(s);
    s->dma_pending = 0;
    s->xfer_is_write = 0;

    switch (cmd) {
    case 0xec: /* IDENTIFY DEVICE */
        s5l8702_ata_build_identify(s);
        break;
    case 0x20: /* READ SECTORS (PIO) */
    case 0x24: /* READ SECTORS EXT (PIO) */
        s5l8702_ata_pio_read_start(s);
        break;
    case 0xc8: /* READ DMA */
    case 0x25: /* READ DMA EXT */
        s->dma_pending = 1;
        s->dma_is_write = 0;
        s->drq = 0;
        break;
    case 0x30: /* WRITE SECTORS (PIO) */
    case 0x34: /* WRITE SECTORS EXT (PIO) */
        s5l8702_ata_buf_reserve(s, s->cur_count * 512);
        s->xfer_len = s->cur_count * 512;
        s->xfer_pos = 0;
        s->xfer_is_write = 1;
        s->drq = 1;
        break;
    case 0xca: /* WRITE DMA */
    case 0x35: /* WRITE DMA EXT */
        s->dma_pending = 1;
        s->dma_is_write = 1;
        s->drq = 0;
        break;
    case 0xef: /* SET FEATURES */
    case 0xb0: /* SMART */
    case 0xe0: /* STANDBY IMMEDIATE */
    case 0xe7: /* FLUSH CACHE */
    case 0xea: /* FLUSH CACHE EXT */
    default:
        /* Complete benignly, no data phase. */
        s->drq = 0;
        break;
    }

    /*
     * Commands that finish immediately (identify, PIO read, no-data commands)
     * signal completion now; DMA and PIO-write finish later.
     */
    if (!s->dma_pending && !s->xfer_is_write) {
        s->ata_irq |= XFR_DONE_INT | ATADEV_IRQ_INT;
    }
    s5l8702_ata_update_irq(s);
}

static uint64_t s5l8702_ata_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    S5L8702AtaState *s = S5L8702_ATA(opaque);
    uint32_t r = 0;

    switch (offset) {
    case ATA_CONTROL:
        r = s->ata_control;
        ATADBG("%s: ATA_CONTROL: 0x%08x\n", __func__, r);
        break;
    case ATA_STATUS:
        r = s->ata_status;
        ATADBG("%s: ATA_STATUS: 0x%08x\n", __func__, r);
        break;
    case ATA_COMMAND:
        r = s->ata_command;
        ATADBG("%s: ATA_COMMAND: 0x%08x\n", __func__, r);
        break;
    case ATA_SWRST:
        r = s->ata_swrst;
        ATADBG("%s: ATA_SWRST: 0x%08x\n", __func__, r);
        break;
    case ATA_IRQ:
        /*
         * Real pending interrupt bits (4:0). A polled EFI wants any of
         * these set after a transfer; the interrupt-driven retailOS reads
         * this in its ISR and rejects stray non-IRQ bits, so keep it clean.
         */
        r = s->ata_irq & 0x1f;
        ATADBG("%s: ATA_IRQ: 0x%08x\n", __func__, r);
        break;
    case ATA_IRQ_MASK:
        r = s->ata_irq_mask;
        ATADBG("%s: ATA_IRQ_MASK: 0x%08x\n", __func__, r);
        break;
    case ATA_CFG:
        r = s->ata_cfg;
        ATADBG("%s: ATA_CFG: 0x%08x\n", __func__, r);
        break;
    case ATA_MDMA_TIME:
        r = s->ata_mdma_time;
        ATADBG("%s: ATA_MDMA_TIME: 0x%08x\n", __func__, r);
        break;
    case ATA_PIO_TIME:
        r = s->ata_pio_time;
        ATADBG("%s: ATA_PIO_TIME: 0x%08x\n", __func__, r);
        break;
    case ATA_UDMA_TIME:
        r = s->ata_udma_time;
        ATADBG("%s: ATA_UDMA_TIME: 0x%08x\n", __func__, r);
        break;
    case ATA_XFR_NUM:
        r = s->ata_xfr_num;
        ATADBG("%s: ATA_XFR_NUM: 0x%08x\n", __func__, r);
        break;
    case ATA_XFR_CNT:
        r = s->ata_xfr_cnt;
        ATADBG("%s: ATA_XFR_CNT: 0x%08x\n", __func__, r);
        break;
    case ATA_TBUF_START:
        r = s->ata_tbuf_start;
        ATADBG("%s: ATA_TBUF_START: 0x%08x\n", __func__, r);
        break;
    case ATA_TBUF_SIZE:
        r = s->ata_tbuf_size;
        ATADBG("%s: ATA_TBUF_SIZE: 0x%08x\n", __func__, r);
        break;
    case ATA_SBUF_START:
        r = s->ata_sbuf_start;
        ATADBG("%s: ATA_SBUF_START: 0x%08x\n", __func__, r);
        break;
    case ATA_SBUF_SIZE:
        r = s->ata_sbuf_size;
        ATADBG("%s: ATA_SBUF_SIZE: 0x%08x\n", __func__, r);
        break;
    case ATA_CADR_TBUF:
        r = s->ata_cadr_tbuf;
        ATADBG("%s: ATA_CADR_TBUF: 0x%08x\n", __func__, r);
        break;
    case ATA_CADR_SBUF:
        r = s->ata_cadr_sbuf;
        ATADBG("%s: ATA_CADR_SBUF: 0x%08x\n", __func__, r);
        break;
    /* dummy-read latches the data register; value comes from RDATA */
    case ATA_PIO_DTR:
        s->last_reg_read = offset;
        break;
    case ATA_PIO_FED:
        ATADBG("%s: ATA_PIO_FED\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_SCR:
        ATADBG("%s: ATA_PIO_SCR\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_LLR:
        ATADBG("%s: ATA_PIO_LLR\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_LMR:
        ATADBG("%s: ATA_PIO_LMR\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_LHR:
        ATADBG("%s: ATA_PIO_LHR\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_DVR:
        ATADBG("%s: ATA_PIO_DVR\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_CSD: /* When read, RDATA contains status register */
        ATADBG("%s: ATA_PIO_CSD\n", __func__);
        s->last_reg_read = offset;
        break;
    /*
     * When read, RDATA contains alternate status register (status register
     * mirror)
     */
    case ATA_PIO_DAD:
        ATADBG("%s: ATA_PIO_DAD\n", __func__);
        s->last_reg_read = offset;
        break;
    case ATA_PIO_READY:
        r |= DEV_ACC_READY;
        r |= PIO_DATA_READY;
        ATADBG("%s: ATA_PIO_READY: 0x%08x\n", __func__, r);
        break;
    case ATA_PIO_RDATA:
        switch (s->last_reg_read) {
        case ATA_PIO_CSD: /* Status Register */
        case ATA_PIO_DAD: /* Alternate Status Register */
            r = (1 << 6) | (s->drq << 3); /* RDY, DRQ (BSY=0, ERR=0) */
            break;
        case ATA_PIO_FED: /* Error Register */
            r = 0;
            break;
        case ATA_PIO_DTR: /* PIO data word (16-bit) */
            if (s->xfer_pos + 1 < s->xfer_len) {
                r = s->xfer_buf[s->xfer_pos] |
                    (s->xfer_buf[s->xfer_pos + 1] << 8);
            } else if (s->xfer_pos < s->xfer_len) {
                r = s->xfer_buf[s->xfer_pos];
            } else {
                r = 0;
            }
            s->xfer_pos += 2;
            if (s->xfer_pos >= s->xfer_len) {
                s->drq = 0;
            }
            break;
        default:
            r = 0;
            break;
        }
        break;
    case ATA_BUS_FIFO_STATUS:
        r = s->ata_bus_fifo_status;
        ATADBG("%s: ATA_BUS_FIFO_STATUS: 0x%08x\n", __func__, r);
        break;
    case ATA_FIFO_STATUS:
        r = s->ata_fifo_status;
        ATADBG("%s: ATA_FIFO_STATUS: 0x%08x\n", __func__, r);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read (offset 0x%04x)\n",
                      __func__, (uint32_t) offset);
    }

    return r;
}

static void s5l8702_ata_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702AtaState *s = S5L8702_ATA(opaque);

    switch (offset) {
    case ATA_CONTROL:
        ATADBG("%s: ATA_CONTROL: 0x%08x\n", __func__, (uint32_t) val);

        if ((val & ATA_ENABLE) != (s->ata_control & ATA_ENABLE)) {
            if (val & ATA_ENABLE) {
                ATADBG("%s: ATA_CONTROL: enabling ATA\n", __func__);
            } else {
                ATADBG("%s: ATA_CONTROL: disabling ATA\n", __func__);
            }
        }

        s->ata_control = (uint32_t) val;
        break;
    case ATA_COMMAND:
        ATADBG("%s: ATA_COMMAND: 0x%08x\n", __func__, (uint32_t) val);

        if ((val & XFR_COMMAND_MASK) != (s->ata_command & XFR_COMMAND_MASK)) {
            switch ((val & XFR_COMMAND_MASK) >> XFR_COMMAND) {
            case 0:
                ATADBG("%s: ATA_COMMAND: command stop\n", __func__);
                break;
            case 1:
                ATADBG("%s: ATA_COMMAND: command start\n", __func__);
                break;
            case 2:
                ATADBG("%s: ATA_COMMAND: command abort\n", __func__);
                break;
            case 3:
                ATADBG("%s: ATA_COMMAND: command continue\n", __func__);
                break;
            }
        }

        s->ata_command = (uint32_t) val;
        if ((val & XFR_COMMAND_MASK) == 1 && s->dma_pending) {
            /* COMMAND=start kicks off the deferred DMA data transfer. */
            s5l8702_ata_dma_run(s);
        }
        break;
    case ATA_SWRST:
        ATADBG("%s: ATA_SWRST: 0x%08x\n", __func__, (uint32_t) val);

        if ((val & ATA_SWRSTN) != (s->ata_swrst & ATA_SWRSTN)) {
            if (val & ATA_SWRSTN) {
                ATADBG("%s: ATA_SWRST: reset asserted\n", __func__);
            } else {
                ATADBG("%s: ATA_SWRST: reset deasserted\n", __func__);
            }
        }

        s->ata_swrst = (uint32_t) val;
        break;
    case ATA_IRQ:
        ATADBG("%s: ATA_IRQ: 0x%08x\n", __func__, (uint32_t) val);
        if (val & SBUF_EMPTY_INT) {
            ATADBG("%s: ATA_IRQ: Clearing SBUF_EMPTY_INT\n", __func__);
        }
        if (val & TBUF_FULL_INT) {
            ATADBG("%s: ATA_IRQ: Clearing TBUF_FULL_INT\n", __func__);
        }
        if (val & ATADEV_IRQ_INT) {
            ATADBG("%s: ATA_IRQ: Clearing ATADEV_IRQ_INT\n", __func__);
        }
        if (val & UDMA_HOLD_INT) {
            ATADBG("%s: ATA_IRQ: Clearing UDMA_HOLD_INT\n", __func__);
        }
        if (val & XFR_DONE_INT) {
            ATADBG("%s: ATA_IRQ: Clearing XFR_DONE_INT\n", __func__);
        }
        s->ata_irq &= ~(uint32_t) val;
        /* Guest acked the interrupt (W1C) -> re-evaluate the line. */
        s5l8702_ata_update_irq(s);
        break;
    case ATA_IRQ_MASK:
        ATADBG("%s: ATA_IRQ_MASK: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_irq_mask = (uint32_t) val;
        s5l8702_ata_update_irq(s);
        break;
    case ATA_CFG:
        ATADBG("%s: ATA_CFG: 0x%08x\n", __func__, (uint32_t) val);

        if (val & UDMA_AUTO_MODE) {
            ATADBG("%s: ATA_CFG: UDMA_AUTO_MODE set\n", __func__);
        }
        if (val & SBUF_FULL_MODE) {
            ATADBG("%s: ATA_CFG: SBUF_FULL_MODE set\n", __func__);
        }
        if (val & SBUF_EMPTY_MODE) {
            ATADBG("%s: ATA_CFG: SBUF_EMPTY_MODE set\n", __func__);
        }
        if (val & TBUF_FULL_MODE) {
            ATADBG("%s: ATA_CFG: TBUF_FULL_MODE set\n", __func__);
        }
        if (val & BYTE_SWAP) {
            ATADBG("%s: ATA_CFG: BYTE_SWAP set\n", __func__);
        }
        if (val & ATADEV_IRQ_AL) {
            ATADBG("%s: ATA_CFG: ATADEV_IRQ_AL set\n", __func__);
        }
        if (val & DMA_DIR) {
            ATADBG("%s: ATA_CFG: DMA_DIR set\n", __func__);
        }
        switch ((val & ATA_CLASS_MASK) >> ATA_CLASS) {
        case 0:
            ATADBG("%s: ATA_CFG: ATA_CLASS: PIO\n", __func__);
            break;
        case 1:
            ATADBG("%s: ATA_CFG: ATA_CLASS: PIO DMA\n", __func__);
            break;
        case 2:
        case 3:
            ATADBG("%s: ATA_CFG: ATA_CLASS: UDMA\n", __func__);
            break;
        }
        if (val & ATA_IORDY_EN) {
            ATADBG("%s: ATA_CFG: ATA_IORDY_EN set\n", __func__);
        }
        if (val & ATA_RST) {
            ATADBG("%s: ATA_CFG: ATA_RST set\n", __func__);
        }
        s->ata_cfg = (uint32_t) val;
        break;
    case ATA_MDMA_TIME:
        ATADBG("%s: ATA_MDMA_TIME: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_mdma_time = (uint32_t) val;
        break;
    case ATA_PIO_TIME:
        ATADBG("%s: ATA_PIO_TIME: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_time = (uint32_t) val;
        break;
    case ATA_UDMA_TIME:
        ATADBG("%s: ATA_UDMA_TIME: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_udma_time = (uint32_t) val;
        break;
    case ATA_XFR_NUM:
        ATADBG("%s: ATA_XFR_NUM: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_xfr_num = (uint32_t) val;
        break;
    case ATA_XFR_CNT:
        ATADBG("%s: ATA_XFR_CNT: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_xfr_cnt = (uint32_t) val;
        break;
    case ATA_TBUF_START:
        ATADBG("%s: ATA_TBUF_START: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_tbuf_start = (uint32_t) val;
        break;
    case ATA_TBUF_SIZE:
        ATADBG("%s: ATA_TBUF_SIZE: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_tbuf_size = (uint32_t) val;
        break;
    case ATA_SBUF_START:
        ATADBG("%s: ATA_SBUF_START: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_sbuf_start = (uint32_t) val;
        break;
    case ATA_SBUF_SIZE:
        ATADBG("%s: ATA_SBUF_SIZE: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_sbuf_size = (uint32_t) val;
        break;
    case ATA_CADR_TBUF:
        ATADBG("%s: ATA_CADR_TBUF: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_cadr_tbuf = (uint32_t) val;
        break;
    case ATA_CADR_SBUF:
        ATADBG("%s: ATA_CADR_SBUF: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_cadr_sbuf = (uint32_t) val;
        break;
    case ATA_PIO_DTR:
        if (s->xfer_is_write && s->xfer_pos + 1 < s->xfer_len) {
            s->xfer_buf[s->xfer_pos] = val & 0xff;
            s->xfer_buf[s->xfer_pos + 1] = (val >> 8) & 0xff;
            s->xfer_pos += 2;
            if (s->xfer_pos >= s->xfer_len) {
                BlockBackend *blk = s5l8702_ata_blk(s);
                if (blk) {
                    blk_pwrite(blk, (int64_t)s->cur_lba * 512,
                               s->xfer_len, s->xfer_buf, 0);
                }
                s->drq = 0;
            }
        } else {
            s->ata_pio_dtr = (uint32_t) val;
        }
        break;
    case ATA_PIO_FED:
        ATADBG("%s: ATA_PIO_FED: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_fed = (uint32_t) val;
        break;
    case ATA_PIO_SCR:
        ATADBG("%s: ATA_PIO_SCR: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_scr = (uint32_t) val;
        break;
    case ATA_PIO_LLR:
        ATADBG("%s: ATA_PIO_LLR: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_llr = (uint32_t) val;
        break;
    case ATA_PIO_LMR:
        ATADBG("%s: ATA_PIO_LMR: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_lmr = (uint32_t) val;
        break;
    case ATA_PIO_LHR:
        ATADBG("%s: ATA_PIO_LHR: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_lhr = (uint32_t) val;
        break;
    case ATA_PIO_DVR:
        ATADBG("%s: ATA_PIO_DVR: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_dvr = (uint32_t) val;
        break;
    case ATA_PIO_CSD:
        ATADBG("%s: ATA_PIO_CSD: cmd=0x%02x lba=%u count=%u\n", __func__,
               (uint32_t)(val & 0xff), s5l8702_ata_lba28(s),
               s5l8702_ata_count(s));
        s->ata_pio_csd = (uint32_t) val;
        s5l8702_ata_command(s, (uint32_t)(val & 0xff));
        break;
    case ATA_PIO_DAD:
        ATADBG("%s: ATA_PIO_DAD: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_dad = (uint32_t) val;
        break;
    case ATA_PIO_READY:
        ATADBG("%s: ATA_PIO_READY: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_ready = (uint32_t) val;
        break;
    case ATA_PIO_RDATA:
        ATADBG("%s: ATA_PIO_RDATA: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_pio_rdata = (uint32_t) val;
        break;
    case ATA_BUS_FIFO_STATUS:
        ATADBG("%s: ATA_BUS_FIFO_STATUS: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_bus_fifo_status = (uint32_t) val;
        break;
    case ATA_FIFO_STATUS:
        ATADBG("%s: ATA_FIFO_STATUS: 0x%08x\n", __func__, (uint32_t) val);
        s->ata_fifo_status = (uint32_t) val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write (offset 0x%04x, "
                      "value 0x%08x)\n",
                      __func__, (uint32_t) offset, (uint32_t) val);
    }
}

static const MemoryRegionOps s5l8702_ata_ops = {
        .read = s5l8702_ata_read,
        .write = s5l8702_ata_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_ata_reset(DeviceState *dev)
{
    S5L8702AtaState *s = S5L8702_ATA(dev);

    trace_s5l8702_ata_reset();

    /* Reset registers */
    s->ata_control = (1 << 1); /* clk_down_ready */
    s->ata_status = 0;
    s->ata_command = 0;
    s->ata_swrst = 0;
    s->ata_irq = 0;
    s->ata_irq_mask = 0;
    s->ata_cfg = 0;
    s->ata_mdma_time = 0;
    s->ata_pio_time = 0;
    s->ata_udma_time = 0;
    s->ata_xfr_num = 0;
    s->ata_xfr_cnt = 0;
    s->ata_tbuf_start = 0;
    s->ata_tbuf_size = 0;
    s->ata_sbuf_start = 0;
    s->ata_sbuf_size = 0;
    s->ata_cadr_tbuf = 0;
    s->ata_cadr_sbuf = 0;
    s->ata_pio_dtr = 0;
    s->ata_pio_fed = 0;
    s->ata_pio_scr = 0;
    s->ata_pio_llr = 0;
    s->ata_pio_lmr = 0;
    s->ata_pio_lhr = 0;
    s->ata_pio_dvr = 0;
    s->ata_pio_csd = 0;
    s->ata_pio_dad = 0;
    s->ata_pio_ready = 0;
    s->ata_pio_rdata = 0;
    s->ata_bus_fifo_status = 0;
    s->ata_fifo_status = 0;

    s->drq = 0;
    s->xfer_len = 0;
    s->xfer_pos = 0;
    s->xfer_is_write = 0;
    s->dma_pending = 0;
    s->dma_is_write = 0;
    s->cur_cmd = 0;
    s->cur_lba = 0;
    s->cur_count = 0;
}

void s5l8702_ata_set_drive_info(S5L8702AtaState *s, DriveInfo *i)
{
    trace_s5l8702_ata_set_drive_info(i);
    ide_bus_create_drive(&s->bus, 0, i);
}

static void s5l8702_ata_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    S5L8702AtaState *s = S5L8702_ATA(obj);

    trace_s5l8702_ata_init();

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &s5l8702_ata_ops, s, TYPE_S5L8702_ATA, S5L8702_ATA_SIZE);
    sysbus_init_mmio(d, &s->iomem);
    sysbus_init_irq(d, &s->irq);

    ide_bus_init(&s->bus, sizeof(s->bus), DEVICE(obj), 0, 1);
}

static void s5l8702_ata_realize(DeviceState *dev, Error **errp)
{
    S5L8702AtaState *s = S5L8702_ATA(dev);

    ide_bus_init_output_irq(&s->bus, s->irq);
}

static void s5l8702_ata_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    trace_s5l8702_ata_class_init();

    dc->realize = s5l8702_ata_realize;
    dc->reset = s5l8702_ata_reset;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo s5l8702_ata_types[] = {
        {
                .name = TYPE_S5L8702_ATA,
                .parent = TYPE_SYS_BUS_DEVICE,
                .instance_init = s5l8702_ata_init,
                .instance_size = sizeof(S5L8702AtaState),
                .class_init = s5l8702_ata_class_init,
        },
};
DEFINE_TYPES(s5l8702_ata_types);
