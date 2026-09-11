/*
 * Cirrus Logic CS42L55 audio codec, I2C0 slave 0x4A.
 *
 * Control port and master LRCK clock. See the header for original firmware
 * addresses and DS773F1 for the control-port and clocking specifications.
 * Frame-level PCM output uses QEMU audio. Nominal gain, mute and routing are
 * modeled, including digital gain ramps and sampled analog zero crossings.
 * Digital mute ramp endpoints, analog dynamics and DSP filters remain unknown
 * or unimplemented.
 *
 * Ported from davidmonterocrespo24/qemu-ipod-classic.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/int128.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-clock.h"
#include "migration/vmstate.h"
#include "hw/misc/s5l8702-cs42l55.h"
#include "trace.h"

#define MAP_INCR        BIT(7)
#define MAP_ADDR        0x7f
#define CLOCKING1       0x04
#define CLOCKING2       0x05
#define MISC_CONTROL    0x07
#define MISC_STATUS     0x29
#define CLOCK_MASTER    BIT(5)
#define MCLK_DIV2       BIT(1)
#define MCLK_DISABLE    BIT(0)
#define FREEZE          BIT(0)
#define DIGITAL_RAMP    BIT(2)
#define ANALOG_ZC       BIT(3)
#define POWER1          0x02
#define POWER2          0x03
#define INPUT_MUX       0x08
#define PLAYBACK        0x0f
#define PCM_VOLUME      0x12
#define MASTER_VOLUME   0x18
#define HP_VOLUME       0x1a
#define CHANNEL_MIX     0x20
#define PDN             BIT(0)
#define PDN_DSP         BIT(7)
#define PLAYBACK_GANGED BIT(4)
#define VOLUME_MUTE     BIT(7)
#define GAIN_ONE        (1U << 24)

/* A host resampler bound, not a modeled restriction on the physical LRCK. */
#define MAX_AUDIO_RATE  192000

/*
 * DS773F1 6.7.2 specifies 1024 sample periods, but its 10.7 ms example at
 * 48 kHz is inconsistent. Use the literal cycle count pending measurement.
 */
#define ZC_TIMEOUT_CYCLES 1024

/* DS773F1 section 5. Undocumented initialization registers remain storage. */
static const uint8_t cs42l55_reset_regs[S5L8702_CS42L55_NUM_REGS] = {
    [CS42L55_REG_CHIPVERSION] = S5L8702_CS42L55_CHIP_ID,
    [0x02] = 0x0f,
    [0x03] = 0xff,
    [CLOCKING2] = 0x0b,
    [MISC_CONTROL] = 0x0c,
    [0x09] = 0xa0,
    [0x10] = 0x80,
    [0x11] = 0x80,
    [0x17] = 0x88,
    [0x22] = 0x7f,
    [0x25] = 0x3f,
    [0x2a] = 0x05,
};

static uint64_t cs42l55_lrck_period(S5L8702CS42L55State *s)
{
    uint8_t ctl1 = s->active[CLOCKING1];
    uint8_t ctl2 = s->active[CLOCKING2];
    unsigned speed = extract32(ctl2, 3, 2);
    unsigned ratio = extract32(ctl2, 0, 2);
    unsigned numerator, denominator = 1;
    uint64_t period = clock_get(s->mclk);

    if (!(ctl1 & CLOCK_MASTER) || (ctl1 & MCLK_DISABLE) || !period) {
        return 0;
    }
    if (!speed || !(ratio & 1)) {
        /* Reserved speed/ratio encodings have no defined output rate. */
        return 0;
    }
    numerator = (ratio == 1 ? 125 : 136) << (speed - 1);
    if (ctl1 & MCLK_DIV2) {
        numerator *= 2;
    }
    if (ctl2 & BIT(2)) {
        /* The 32 kHz group divides the rate by another 3/2 (table 1). */
        numerator *= 3;
        denominator = 2;
    }
    if (period > UINT64_MAX / numerator) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cs42l55: LRCK period exceeds clock precision range\n");
        return 0;
    }
    /* Preserve the fractional period, including 12 MHz / 272 at 44.1 kHz. */
    return period * numerator / denominator;
}

static void cs42l55_audio_callback(void *opaque, int avail)
{
    S5L8702CS42L55State *s = opaque;

    while (s->voice && avail > 0 && s->audio_count) {
        unsigned len = MIN(s->audio_count, avail);
        size_t written;

        written = AUD_write(s->voice, s->audio_buffer, len);
        if (!written) {
            break;
        }
        s->audio_count -= written;
        avail -= written;
        memmove(s->audio_buffer, s->audio_buffer + written, s->audio_count);
    }
}

static void cs42l55_update_audio(S5L8702CS42L55State *s)
{
    uint64_t period = clock_get(s->lrck);
    uint64_t rate = period ? (CLOCK_PERIOD_1SEC + period / 2) / period : 0;
    bool powered = !(s->active[POWER1] & PDN);

    /* QEMU audio accepts integer rates; the guest retains fractional LRCK. */
    if (rate > MAX_AUDIO_RATE) {
        rate = 0;
    }
    if (!s->card.name) {
        return;
    }
    if (!rate) {
        /* Preserve the backend across clock gating, including WAV capture. */
        s->audio_count = 0;
        AUD_set_active_out(s->voice, false);
        return;
    }
    if (rate != s->audio_rate) {
        struct audsettings settings = {
            .freq = rate,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .endianness = 0,
        };

        s->audio_count = 0;
        s->audio_rate = rate;
        s->voice = AUD_open_out(&s->card, s->voice, "cs42l55-output", s,
                                cs42l55_audio_callback, &settings);
    }
    if (!powered) {
        s->audio_count = 0;
    }
    AUD_set_active_out(s->voice, powered);
}

/* Deterministic nominal gain. Arguments are in eighth-dB units. */
static uint32_t cs42l55_gain(int db_eighth)
{
    /* Q30 factors for attenuation by 0.125, 0.25, 0.5, ... 64 dB. */
    static const uint32_t attenuation[] = {
        1058400094, 1043277569,
        1013677647, 956973408, 852903448, 677485290,
        427464319, 170176611, 26971175, 677485,
    };
    uint64_t gain = 66791300; /* +12 dB, Q24 */
    unsigned steps = 96 - db_eighth;

    if (!db_eighth) {
        return GAIN_ONE;
    }
    for (unsigned bit = 0; bit < ARRAY_SIZE(attenuation); bit++) {
        if (steps & BIT(bit)) {
            gain = (gain * attenuation[bit]) >> 30;
        }
    }
    return gain;
}

static unsigned cs42l55_volume_channel(S5L8702CS42L55State *s, unsigned ch)
{
    return s->active[PLAYBACK] & PLAYBACK_GANGED ? 0 : ch;
}

static int cs42l55_pcm_target(S5L8702CS42L55State *s, unsigned ch)
{
    int value = s->active[PCM_VOLUME + cs42l55_volume_channel(s, ch)] & 0x7f;

    return 4 * (value <= 24 ? value : value - 128);
}

static int cs42l55_master_target(S5L8702CS42L55State *s, unsigned ch)
{
    int value = s->active[MASTER_VOLUME + cs42l55_volume_channel(s, ch)];

    return 4 * (value <= 24 ? value : MAX(value - 256, -204));
}

static void cs42l55_rebuild_gain(S5L8702CS42L55State *s)
{
    for (unsigned i = 0; i < 2; i++) {
        s->pcm_gain[i] = cs42l55_gain(s->pcm_volume[i]);
        s->master_gain[i] = cs42l55_gain(s->master_volume[i]);
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned source = (i & 2) + cs42l55_volume_channel(s, i & 1);
        int volume = sextract32(s->output_control[i], 0, 7);

        s->output_target[i] = s->active[HP_VOLUME + source];
        s->output_gain[i] = cs42l55_gain(8 * CLAMP(volume, -60, 12));
    }
}

static void cs42l55_latch_output(S5L8702CS42L55State *s, unsigned output,
                                 const char *reason)
{
    s->zc_remaining[output] = 0;
    if (s->output_control[output] != s->output_target[output]) {
        int volume = sextract32(s->output_target[output], 0, 7);

        s->output_control[output] = s->output_target[output];
        s->output_gain[output] = cs42l55_gain(8 * CLAMP(volume, -60, 12));
        trace_s5l8702_cs42l55_latch(output, s->output_control[output],
                                   s->frames, reason);
    }
}

static bool cs42l55_ramp(int16_t *level, int target, unsigned cycles)
{
    int delta = target - *level;
    int steps = MIN(abs(delta), cycles);

    *level += delta < 0 ? -steps : steps;
    return steps != 0;
}

static void cs42l55_sync(S5L8702CS42L55State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t period = clock_get(s->lrck);
    unsigned cycles = 0;

    if (period && now > s->transition_ns) {
        Int128 elapsed = int128_make128(0, now - s->transition_ns);
        Int128 progress = int128_divu(elapsed, int128_make64(period));

        progress = int128_add(progress, int128_make64(s->transition_phase));
        s->transition_phase = int128_getlo(progress);
        /* No pending transition needs more than 1024 cycles of host work. */
        cycles = int128_uge(progress,
                            int128_make64((uint64_t)ZC_TIMEOUT_CYCLES << 32)) ?
                 ZC_TIMEOUT_CYCLES : int128_getlo(progress) >> 32;
    }
    s->transition_ns = now;
    if (!cycles || (s->active[POWER1] & PDN)) {
        return;
    }
    for (unsigned ch = 0; ch < 2; ch++) {
        if (!(s->active[PLAYBACK] & PDN_DSP) &&
            cs42l55_ramp(&s->pcm_volume[ch], cs42l55_pcm_target(s, ch),
                         cycles)) {
            s->pcm_gain[ch] = cs42l55_gain(s->pcm_volume[ch]);
        }
        /* DS773F1 figure 12: master gain/ramp is available with DSP off. */
        if (cs42l55_ramp(&s->master_volume[ch],
                         cs42l55_master_target(s, ch), cycles)) {
            s->master_gain[ch] = cs42l55_gain(s->master_volume[ch]);
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (s->zc_remaining[i]) {
            if (cycles >= s->zc_remaining[i]) {
                cs42l55_latch_output(s, i, "timeout");
            } else {
                s->zc_remaining[i] -= cycles;
            }
        }
    }
}

static void cs42l55_controls_changed(S5L8702CS42L55State *s)
{
    if (s->active[POWER1] & PDN) {
        memset(s->last_dac, 0, sizeof(s->last_dac));
    }
    if (!(s->active[MISC_CONTROL] & DIGITAL_RAMP)) {
        for (unsigned ch = 0; ch < 2; ch++) {
            s->pcm_volume[ch] = cs42l55_pcm_target(s, ch);
            s->master_volume[ch] = cs42l55_master_target(s, ch);
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned source = (i & 2) + cs42l55_volume_channel(s, i & 1);
        uint8_t target = s->active[HP_VOLUME + source];
        bool analog = s->active[INPUT_MUX] & BIT(i);

        if (target != s->output_target[i]) {
            s->output_target[i] = target;
            s->zc_remaining[i] = target == s->output_control[i] ?
                                 0 : ZC_TIMEOUT_CYCLES;
        }
        if (!(s->active[MISC_CONTROL] & ANALOG_ZC) ||
            (s->active[POWER1] & PDN) || analog || !s->last_dac[i & 1]) {
            /* Analog input is currently silent; powered-down rails are zero. */
            cs42l55_latch_output(s, i, "control");
        }
    }
    cs42l55_rebuild_gain(s);
}

static bool cs42l55_output_on(S5L8702CS42L55State *s, unsigned output)
{
    /* HP A/B occupy bits 5:4/7:6; line A/B occupy bits 1:0/3:2. */
    unsigned shift = output < 2 ? 4 + 2 * output : 2 * (output - 2);
    unsigned mode = extract32(s->active[POWER2], shift, 2);

    return mode == 2 || (mode < 2 && mode == s->hpdetect);
}

static int32_t cs42l55_scale(int32_t sample, uint32_t gain)
{
    return (int64_t)sample * gain / GAIN_ONE;
}

void s5l8702_cs42l55_dac_frame(S5L8702CS42L55State *s,
                              int16_t left, int16_t right)
{
    int32_t pcm[2] = { left * 256, right * 256 };
    int32_t dac[2];
    int16_t output[2];
    uint8_t playback = s->active[PLAYBACK];
    bool dsp = !(playback & PDN_DSP);
    unsigned first = s->line_out ? 2 : 0;

    cs42l55_sync(s);
    if ((s->active[POWER1] & PDN) || !clock_get(s->lrck)) {
        return;
    }
    /* The DSP power bit bypasses its mixer; it is not global DAC power. */
    for (unsigned ch = 0; ch < 2; ch++) {
        if (dsp && (playback & BIT(2 + ch))) {
            pcm[ch] = -pcm[ch];
        }
        if (dsp) {
            pcm[ch] = s->active[PCM_VOLUME + ch] & VOLUME_MUTE ? 0 :
                      cs42l55_scale(pcm[ch], s->pcm_gain[ch]);
        }
    }
    for (unsigned ch = 0; ch < 2; ch++) {
        unsigned swap = dsp ? extract32(s->active[CHANNEL_MIX],
                                         4 + 2 * ch, 2) : 0;
        int32_t sample = swap == 3 ? pcm[1 - ch] :
                         swap ? (pcm[0] + pcm[1]) / 2 : pcm[ch];

        sample = cs42l55_scale(sample, s->master_gain[ch]);
        /* Saturate the digital DAC input before applying analog output gain. */
        dac[ch] = playback & BIT(ch) ? 0 :
                  CLAMP(sample, -0x800000, 0x7fffff);
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned ch = i & 1;

        /* Sampled DAC zero crossing, before amplifier gain and mute. */
        if (!dac[ch] || !s->last_dac[ch] ||
            ((dac[ch] < 0) != (s->last_dac[ch] < 0))) {
            cs42l55_latch_output(s, i, "zero-cross");
        }
    }
    memcpy(s->last_dac, dac, sizeof(dac));
    for (unsigned ch = 0; ch < 2; ch++) {
        unsigned out = first + ch;
        int32_t sample = cs42l55_scale(dac[ch], s->output_gain[out]);

        if (!cs42l55_output_on(s, out) ||
            (s->output_control[out] & VOLUME_MUTE) ||
            (s->active[INPUT_MUX] & BIT(out))) {
            /* The analog PGA input is silent until capture is implemented. */
            sample = 0;
        }
        output[ch] = CLAMP(sample / 256, INT16_MIN, INT16_MAX);
    }
    s->last_frame = (uint16_t)output[0] | (uint32_t)(uint16_t)output[1] << 16;
    s->frames++;
    trace_s5l8702_cs42l55_frame(s->last_frame, s->frames);

    /* Host backpressure never gates the serializer or its DMA request. */
    if (!s->voice || !AUD_is_active_out(s->voice)) {
        return;
    }
    if (s->audio_count > sizeof(s->audio_buffer) - 4) {
        cs42l55_audio_callback(s, sizeof(s->audio_buffer));
    }
    if (s->audio_count > sizeof(s->audio_buffer) - 4) {
        s->dropped_frames++;
        return;
    }
    stl_le_p(s->audio_buffer + s->audio_count, s->last_frame);
    s->audio_count += 4;
}

static void cs42l55_update_clock(S5L8702CS42L55State *s, bool propagate)
{
    uint64_t period = cs42l55_lrck_period(s);

    s->lrck_hz = CLOCK_PERIOD_TO_HZ(period);
    if (propagate) {
        clock_update(s->lrck, period);
    } else {
        clock_set(s->lrck, period);
    }
    cs42l55_update_audio(s);
}

static void cs42l55_mclk_changed(void *opaque, ClockEvent event)
{
    if (event == ClockPreUpdate) {
        cs42l55_sync(opaque);
    } else {
        cs42l55_update_clock(opaque, true);
    }
}

static void cs42l55_advance(S5L8702CS42L55State *s)
{
    if (s->addr & MAP_INCR) {
        s->addr = MAP_INCR | ((s->addr + 1) & MAP_ADDR);
    }
}

static int s5l8702_cs42l55_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(i2c);

    switch (event) {
    case I2C_START_SEND:
        /* The next byte is the register number, not data. */
        s->addr_pending = true;
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t s5l8702_cs42l55_recv(I2CSlave *i2c)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(i2c);
    unsigned addr = s->addr & MAP_ADDR;
    uint8_t val = addr == MISC_STATUS ? s->hpdetect << 7 : s->reg[addr];

    cs42l55_advance(s);
    return val;
}

static int s5l8702_cs42l55_send(I2CSlave *i2c, uint8_t data)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(i2c);

    if (s->addr_pending) {
        s->addr = data;
        s->addr_pending = false;
    } else {
        unsigned addr = s->addr & MAP_ADDR;

        if (addr != CS42L55_REG_CHIPVERSION && addr != MISC_STATUS) {
            cs42l55_sync(s);
            s->reg[addr] = data;
            if (!(s->reg[MISC_CONTROL] & FREEZE)) {
                memcpy(s->active, s->reg, sizeof(s->active));
                cs42l55_controls_changed(s);
                cs42l55_update_clock(s, true);
            }
        }
        cs42l55_advance(s);
    }
    return 0;
}

static void s5l8702_cs42l55_reset_enter(Object *obj, ResetType type)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(obj);

    memcpy(s->reg, cs42l55_reset_regs, sizeof(s->reg));
    memcpy(s->active, s->reg, sizeof(s->active));
    memset(s->pcm_volume, 0, sizeof(s->pcm_volume));
    memset(s->master_volume, 0, sizeof(s->master_volume));
    memset(s->output_control, 0, sizeof(s->output_control));
    memset(s->zc_remaining, 0, sizeof(s->zc_remaining));
    memset(s->last_dac, 0, sizeof(s->last_dac));
    s->transition_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->transition_phase = 0;
    cs42l55_rebuild_gain(s);
    s->addr = 0;
    s->addr_pending = false;
    s->audio_count = 0;
    s->frames = s->dropped_frames = 0;
    s->last_frame = 0;
    AUD_close_out(&s->card, s->voice);
    s->voice = NULL;
    s->audio_rate = 0;
}

static void s5l8702_cs42l55_reset_hold(Object *obj)
{
    cs42l55_update_clock(S5L8702_CS42L55(obj), true);
}

static int s5l8702_cs42l55_post_load(void *opaque, int version_id)
{
    S5L8702CS42L55State *s = opaque;

    for (unsigned ch = 0; ch < 2; ch++) {
        if (s->pcm_volume[ch] < -412 || s->pcm_volume[ch] > 96 ||
            s->master_volume[ch] < -816 || s->master_volume[ch] > 96 ||
            s->last_dac[ch] < -0x800000 || s->last_dac[ch] > 0x7fffff) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (s->zc_remaining[i] > ZC_TIMEOUT_CYCLES) {
            return -EINVAL;
        }
    }
    /* Host audio already presented cannot be rolled back by a snapshot. */
    s->audio_count = 0;
    AUD_close_out(&s->card, s->voice);
    s->voice = NULL;
    s->audio_rate = 0;
    /* pre_save synchronized the phase; anchor it to restored virtual time. */
    s->transition_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    cs42l55_rebuild_gain(s);
    /* Consumers restore their clock inputs; do not run their callbacks. */
    cs42l55_update_clock(opaque, false);
    return 0;
}

static int s5l8702_cs42l55_pre_save(void *opaque)
{
    cs42l55_sync(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8702_cs42l55 = {
    .name = TYPE_S5L8702_CS42L55,
    .version_id = 4,
    .minimum_version_id = 4,
    .pre_save = s5l8702_cs42l55_pre_save,
    .post_load = s5l8702_cs42l55_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, S5L8702CS42L55State),
        VMSTATE_UINT8_ARRAY(reg, S5L8702CS42L55State, S5L8702_CS42L55_NUM_REGS),
        VMSTATE_UINT8(addr, S5L8702CS42L55State),
        VMSTATE_BOOL(addr_pending, S5L8702CS42L55State),
        VMSTATE_UINT8_ARRAY(active, S5L8702CS42L55State,
                            S5L8702_CS42L55_NUM_REGS),
        VMSTATE_INT16_ARRAY(pcm_volume, S5L8702CS42L55State, 2),
        VMSTATE_INT16_ARRAY(master_volume, S5L8702CS42L55State, 2),
        VMSTATE_UINT8_ARRAY(output_control, S5L8702CS42L55State, 4),
        VMSTATE_UINT16_ARRAY(zc_remaining, S5L8702CS42L55State, 4),
        VMSTATE_INT32_ARRAY(last_dac, S5L8702CS42L55State, 2),
        VMSTATE_UINT32(transition_phase, S5L8702CS42L55State),
        VMSTATE_BOOL(hpdetect, S5L8702CS42L55State),
        VMSTATE_UINT32(last_frame, S5L8702CS42L55State),
        VMSTATE_UINT64(frames, S5L8702CS42L55State),
        VMSTATE_UINT64(dropped_frames, S5L8702CS42L55State),
        VMSTATE_CLOCK(mclk, S5L8702CS42L55State),
        VMSTATE_END_OF_LIST()
    },
};

static void cs42l55_hpdetect(void *opaque, int line, int level)
{
    S5L8702CS42L55State *s = opaque;

    s->hpdetect = !!level;
}

static void s5l8702_cs42l55_realize(DeviceState *dev, Error **errp)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(dev);

    AUD_register_card(TYPE_S5L8702_CS42L55, &s->card);
}

static void s5l8702_cs42l55_unrealize(DeviceState *dev)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(dev);

    AUD_close_out(&s->card, s->voice);
    s->voice = NULL;
    AUD_remove_card(&s->card);
}

static void cs42l55_get_transition(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    S5L8702CS42L55State *s = S5L8702_CS42L55(obj);
    unsigned index = GPOINTER_TO_UINT(opaque);
    int64_t value;

    cs42l55_sync(s);
    value = index < 2 ? s->pcm_volume[index] :
            index < 4 ? s->master_volume[index - 2] :
            s->output_control[index - 4];
    visit_type_int64(v, name, &value, errp);
}

static void s5l8702_cs42l55_init(Object *obj)
{
    static const char * const transitions[] = {
        "pcm-volume-a", "pcm-volume-b", "master-volume-a", "master-volume-b",
        "headphone-control-a", "headphone-control-b",
        "line-control-a", "line-control-b",
    };
    S5L8702CS42L55State *s = S5L8702_CS42L55(obj);

    s->mclk = qdev_init_clock_in(DEVICE(obj), "mclk", cs42l55_mclk_changed,
                               s, ClockPreUpdate | ClockUpdate);
    s->lrck = qdev_init_clock_out(DEVICE(obj), "lrck");
    object_property_add_uint64_ptr(obj, "lrck-frequency", &s->lrck_hz,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "last-output-frame", &s->last_frame,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "output-frames", &s->frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "host-dropped-frames",
                                   &s->dropped_frames, OBJ_PROP_FLAG_READ);
    qdev_init_gpio_in_named(DEVICE(obj), cs42l55_hpdetect, "hpdetect", 1);
    for (unsigned i = 0; i < ARRAY_SIZE(transitions); i++) {
        object_property_add(obj, transitions[i], "int", cs42l55_get_transition,
                             NULL, NULL, GUINT_TO_POINTER(i));
    }
}

static Property s5l8702_cs42l55_properties[] = {
    DEFINE_AUDIO_PROPERTIES(S5L8702CS42L55State, card),
    DEFINE_PROP_BOOL("line-out", S5L8702CS42L55State, line_out, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void s5l8702_cs42l55_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Cirrus Logic CS42L55 audio codec";
    rc->phases.enter = s5l8702_cs42l55_reset_enter;
    rc->phases.hold = s5l8702_cs42l55_reset_hold;
    dc->vmsd = &vmstate_s5l8702_cs42l55;
    dc->realize = s5l8702_cs42l55_realize;
    dc->unrealize = s5l8702_cs42l55_unrealize;
    device_class_set_props(dc, s5l8702_cs42l55_properties);
    k->event = s5l8702_cs42l55_event;
    k->recv = s5l8702_cs42l55_recv;
    k->send = s5l8702_cs42l55_send;
}

static const TypeInfo s5l8702_cs42l55_types[] = {
    {
        .name          = TYPE_S5L8702_CS42L55,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(S5L8702CS42L55State),
        .instance_init = s5l8702_cs42l55_init,
        .class_init    = s5l8702_cs42l55_class_init,
    },
};
DEFINE_TYPES(s5l8702_cs42l55_types)
