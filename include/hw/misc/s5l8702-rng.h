/*
 * Samsung S5L8702 random number generator, 0x3C100000.
 *
 * NO PUBLIC SOURCE NAMES THIS BLOCK. Reverse-engineered from Apple's retailOS:
 * exactly three instructions in the whole image reference this base --
 *
 *   0x08031d9c  str r0,[r1,#8]   seed  (stirred with two timer reads)
 *   0x08327738  str #0,[r1]      writes ZERO to +0 before drawing
 *   0x08327760  "while (!([+0] & 7)) ; return [+4]"
 *
 * 0x08031d30 seeds +8, then draws twenty words through 0x08327760 discarding
 * nineteen -- the classic "throw away a freshly seeded generator's first
 * outputs" RNG idiom. The identity is INFERRED; the three-register contract is
 * VERIFIED.
 *
 * There is NO enable bit: the only write to +0 is a zero, and it happens before
 * the draws, not after. So the ready bits must be set on their own -- modelling
 * "+0 becomes ready once the guest enables it" deadlocks the very first draw,
 * which spins with no timeout. That is the diskless-boot black screen.
 */

#ifndef HW_MISC_S5L8702_RNG_H
#define HW_MISC_S5L8702_RNG_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_S5L8702_RNG "s5l8702-rng"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702RngState, S5L8702_RNG)

#define S5L8702_RNG_BASE    0x3c100000
#define S5L8702_RNG_SIZE    0x10

struct S5L8702RngState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t seed;   /* what the guest wrote to +8; write-only to the guest */
    uint32_t state;  /* LCG state, advanced on every read of +4 */
};

#endif /* HW_MISC_S5L8702_RNG_H */
