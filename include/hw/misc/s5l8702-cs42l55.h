/*
 * Cirrus Logic CS42L55 audio codec, on I2C0 at 7-bit address 0x4A.
 *
 * retailOS 2.0.4 0x080a79bc supplies slave 0x4a and a two-byte register/value
 * buffer to 0x08360cac. A live trace confirms its initial 0x00 = 0x99 write
 * and STOP at 0x08360968. The previous I2C STOP bug sent codec writes to the
 * previously addressed PMU; address switching is now covered by a qtest.
 * 0x0809f188 selects a register, then reads one byte; 0x080ca298 implements
 * read-modify-write. Initialization 0x0809f1c8 selects master clock mode and
 * MCLK divide-by-two; 0x080a7838 selects the sample-rate ratio.
 * Volume routine 0x080ca36c calls 0x08088bc4 to compute headphone gain;
 * 0x080cf76c sends the incrementing register address and two equal values.
 * DS773F1 sections 4.8, 4.14, 5 and 6.4/6.5 describe the modeled clock and
 * control port. Frame-level PCM playback models nominal gain and routing;
 * Digital gain ramps follow LRCK; analog controls wait for sampled zero
 * crossings. Analog dynamics and DSP filters remain unimplemented.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic (ipod_classic_cs42l55.*).
 */

#ifndef HW_MISC_S5L8702_CS42L55_H
#define HW_MISC_S5L8702_CS42L55_H

#include "hw/i2c/i2c.h"
#include "hw/clock.h"
#include "audio/audio.h"
#include "qom/object.h"

#define TYPE_S5L8702_CS42L55 "s5l8702-cs42l55"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702CS42L55State, S5L8702_CS42L55)

#define S5L8702_CS42L55_I2C_ADDR   0x4a   /* 0x94 >> 1 */
#define S5L8702_CS42L55_NUM_REGS   0x80
/* Host audio staging only, unrelated to any physical codec FIFO. */
#define S5L8702_CS42L55_AUDIO_BYTES 16384

#define CS42L55_REG_CHIPVERSION    0x01
/*
 * Retained reference-model value; the physical chip revision is unverified.
 * DS773F1 specifies only REVID[2:0]; the other five bits are reserved.
 */
#define S5L8702_CS42L55_CHIP_ID    0x5a

struct S5L8702CS42L55State {
    I2CSlave parent_obj;

    uint8_t reg[S5L8702_CS42L55_NUM_REGS];
    uint8_t addr;         /* register pointer, survives a STOP like the PMU */
    bool addr_pending;    /* next byte written is the register number */
    uint8_t active[S5L8702_CS42L55_NUM_REGS]; /* held during FREEZE */
    Clock *mclk;
    Clock *lrck;
    uint64_t lrck_hz;     /* derived, read-only QOM diagnostic */

    QEMUSoundCard card;
    SWVoiceOut *voice;
    bool line_out;       /* host connector selection; default is headphones */
    bool hpdetect;       /* physical input level, not jack insertion policy */
    unsigned audio_rate;
    uint8_t audio_buffer[S5L8702_CS42L55_AUDIO_BYTES];
    unsigned audio_count;
    uint32_t pcm_gain[2]; /* derived Q24 linear gains */
    uint32_t master_gain[2];
    uint32_t output_gain[4];
    int16_t pcm_volume[2];    /* current levels in units of 1/8 dB */
    int16_t master_volume[2];
    uint8_t output_control[4]; /* latched HP A/B, line A/B gain and mute */
    uint8_t output_target[4]; /* derived from active controls and ganging */
    uint16_t zc_remaining[4]; /* LRCK cycles until zero-cross timeout */
    int32_t last_dac[2];      /* pre-amplifier samples for zero detection */
    int64_t transition_ns;
    uint32_t transition_phase; /* fractional LRCK cycle, units of 2^-32 */
    uint32_t last_frame;  /* selected output, low halfword left */
    uint64_t frames;
    uint64_t dropped_frames; /* host staging overflow, no guest side effects */
};

void s5l8702_cs42l55_dac_frame(S5L8702CS42L55State *s,
                              int16_t left, int16_t right);

#endif /* HW_MISC_S5L8702_CS42L55_H */
