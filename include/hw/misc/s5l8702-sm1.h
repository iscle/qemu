/*
 * Partial Samsung S5L8702 "SM1" engine, 0x38500000.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic (ipod_classic_sm1.*).
 * Original retailOS 2.0.4 confirms a memory-description interface at
 * +0xa44/+0xa48, used by 0x080ab624 and 0x080cd04c. It is independent of
 * I2S playback. Its actual memory layout and processing remain unimplemented.
 * Power/command responses below are inherited approximations; the audit
 * distinguishes confirmed accesses from incomplete hardware behavior.
 */

#ifndef HW_MISC_S5L8702_SM1_H
#define HW_MISC_S5L8702_SM1_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8702_SM1 "s5l8702-sm1"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702SM1State, S5L8702_SM1)

#define S5L8702_SM1_BASE        0x38500000
/*
 * Only the low 4 KiB has a register file. The rest is an unimplemented zero
 * window. Original 0x080ab4b8 also accesses +0x1004/+0x1008; these are missing.
 */
#define S5L8702_SM1_MEM_SIZE    0x100000
#define S5L8702_SM1_REG_BYTES   0x1000
#define S5L8702_SM1_REG_WORDS   (S5L8702_SM1_REG_BYTES / 4)

/* Expected identity constant occurs at original retailOS 0x0809b3c4. */
#define S5L8702_SM1_ID          0xc80
#define S5L8702_SM1_ID_MAGIC    0x004e7309

/* Original 0x080c1774 reads status +0xa98; ready semantics remain partial. */
#define S5L8702_SM1_STAT        0xa98
#define S5L8702_SM1_STAT_READY  (1u << 2)

/* Run/start control and state readback. */
#define S5L8702_SM1_RUN         0x400
#define S5L8702_SM1_RUN_MASK    0x3

/* Original 0x080c1754 writes +0x824; 0x080c17bc writes +0xc48. */
#define S5L8702_SM1_CODEC_EN    0x824
#define S5L8702_SM1_COMMIT      0xc48

/*
 * Inherited per-channel sub-blocks: idx 0..3 -> idx*0x20, idx4 -> 0x100,
 * idx5 -> 0x180, idx6 -> 0x200, idx7 -> 0x230. Within:
 *   +0x10 (W): state command. 4 (power up), 16 (run), 96 (stop).
 *   +0x14 (R, masked &7): state, polled until 7.
 * Stop returns the modeled state to zero; transitions are instantaneous.
 */
#define S5L8702_SM1_SUB_STATE_READY  0x7
#define S5L8702_SM1_SUB_CMD_UP       4
#define S5L8702_SM1_SUB_CMD_RUN      16
#define S5L8702_SM1_SUB_CMD_STOP     96

struct S5L8702SM1State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t reg[S5L8702_SM1_REG_WORDS];

    /*
     * Derived engine state: set once the bring-up has powered the block. The
     * status bit at +0xa98 bit2 and the run-state at +0x400 are answered from
     * it, so the poll loops end when the firmware's own writes say it is up.
     */
    bool powered;
};

#endif /* HW_MISC_S5L8702_SM1_H */
