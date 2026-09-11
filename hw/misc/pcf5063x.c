#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "sysemu/rtc.h"
#include "hw/misc/pcf5063x.h"
#include "trace.h"

#define PCF5063X_VERSION    0x00
#define PCF5063X_VARIANT    0x01
#define PCF5063X_INT1       0x02
#define PCF5063X_INT2       0x03
#define PCF5063X_INT3       0x04
#define PCF5063X_INT4       0x05
#define PCF5063X_INT5       0x06
#define PCF5063X_INT1MASK   0x07
#define PCF5063X_INT2MASK   0x08
#define PCF5063X_INT3MASK   0x09
#define PCF5063X_INT4MASK   0x0A
#define PCF5063X_INT5MASK   0x0B
#define PCF5063X_OOCSHDWN   0x0C
#define PCF5063X_OOCWAKE    0x0D
#define PCF5063X_OOCTIM1    0x0E
#define PCF5063X_OOCTIM2    0x0F
#define PCF5063X_OOCMODE    0x10
#define PCF5063X_OOCCTL     0x11
#define PCF5063X_OOCSTAT    0x12
#define PCF5063X_GPIOCTL    0x13
#define PCF5063X_GPIO1CFG   0x14
#define PCF5063X_GPIO2CFG   0x15
#define PCF5063X_GPIO3CFG   0x16
#define PCF5063X_GPOCFG     0x17
#define PCF5063X_BVMCTL     0x18
#define PCF5063X_SVMCTL     0x19
#define PCF5063X_AUTOOUT    0x1A
#define PCF5063X_AUTOENA    0x1B
#define PCF5063X_AUTOCTL    0x1C
#define PCF5063X_AUTOMXC    0x1D
#define PCF5063X_DOWN1OUT   0x1E
#define PCF5063X_DOWN1ENA   0x1F
#define PCF5063X_DOWN1CTL   0x20
#define PCF5063X_DOWN1MXC   0x21
#define PCF5063X_DOWN2OUT   0x22
#define PCF5063X_DOWN2ENA   0x23
#define PCF5063X_DOWN2CTL   0x24
#define PCF5063X_DOWN2MXC   0x25
#define PCF5063X_MEMLDOOUT  0x26
#define PCF5063X_MEMLDOENA  0x27
#define PCF5063X_LEDOUT     0x28
#define PCF5063X_LEDENA     0x29
#define PCF5063X_LEDCTL     0x2A
#define PCF5063X_LEDDIM     0x2B
#define PCF5063X_LDO1OUT    0x2D
#define PCF5063X_LDO1ENA    0x2E
#define PCF5063X_LDO2OUT    0x2F
#define PCF5063X_LDO2ENA    0x30
#define PCF5063X_LDO3OUT    0x31
#define PCF5063X_LDO3ENA    0x32
#define PCF5063X_LDO4OUT    0x33
#define PCF5063X_LDO4ENA    0x34
#define PCF5063X_LDO5OUT    0x35
#define PCF5063X_LDO5ENA    0x36
#define PCF5063X_LDO6OUT    0x37
#define PCF5063X_LDO6ENA    0x38
#define PCF5063X_HCLDOOUT   0x39
#define PCF5063X_HCLDOENA   0x3A
#define PCF5063X_STBYCTL1   0x3B
#define PCF5063X_STBYCTL2   0x3C
#define PCF5063X_DEBPF1     0x3D
#define PCF5063X_DEBPF2     0x3E
#define PCF5063X_DEBPF3     0x3F
#define PCF5063X_HCLDOOVL   0x40
#define PCF5063X_DCDCSTAT   0x41
#define PCF5063X_LDOSTAT    0x42
#define PCF5063X_MBCC1      0x43
#define PCF5063X_MBCC2      0x44
#define PCF5063X_MBCC3      0x45
#define PCF5063X_MBCC4      0x46
#define PCF5063X_MBCC5      0x47
#define PCF5063X_MBCC6      0x48
#define PCF5063X_MBCC7      0x49
#define PCF5063X_MBCC8      0x4A
#define PCF5063X_MBCS1      0x4B
#define PCF5063X_MBCS2      0x4C
#define PCF5063X_MBCS3      0x4D
#define PCF5063X_BBCCTL     0x4E
#define PCF5063X_ALMGAIN    0x4F
#define PCF5063X_ALMDATA    0x50
#define PCF5063X_ADCC3      0x52
#define PCF5063X_ADCC2      0x53
#define PCF5063X_ADCC1      0x54
#define PCF5063X_ADCS1      0x55
#define PCF5063X_ADCS2      0x56
#define PCF5063X_ADCS3      0x57
#define PCF5063X_RTCSC      0x59
#define PCF5063X_RTCMN      0x5A
#define PCF5063X_RTCHR      0x5B
#define PCF5063X_RTCWD      0x5C
#define PCF5063X_RTCDT      0x5D
#define PCF5063X_RTCMT      0x5E
#define PCF5063X_RTCYR      0x5F
#define PCF5063X_RTCSCA     0x60
#define PCF5063X_RTCMNA     0x61
#define PCF5063X_RTCHRA     0x62
#define PCF5063X_RTCWDA     0x63
#define PCF5063X_RTCDTA     0x64
#define PCF5063X_RTCMTA     0x65
#define PCF5063X_RTCYRA     0x66
#define PCF5063X_MEMBYTE0   0x67
#define PCF5063X_MEMBYTE1   0x68
#define PCF5063X_MEMBYTE2   0x69
#define PCF5063X_MEMBYTE3   0x6A
#define PCF5063X_MEMBYTE4   0x6B
#define PCF5063X_MEMBYTE5   0x6C
#define PCF5063X_MEMBYTE6   0x6D
#define PCF5063X_MEMBYTE7   0x6E
#define PCF5063X_DCDCPFM    0x84

/* undocummented PMU registers */
#define PCF50635_INT6       0x85
#define PCF50635_INT6M      0x86
#define PCF50635_GPIOSTAT   0x87
#define PCF50635_GPIO2      0x02

#define PCF5063X_ALARM      0x40
#define PCF5063X_SECOND     0x80
#define RTC_DAY_SECONDS    86400
#define RTC_CYCLE_DAYS     36525
#define RTC_EPOCH          946684800 /* 2000-01-01 00:00:00 UTC. */

static const uint8_t rtc_masks[7] = { 0x7f, 0x7f, 0x3f, 7, 0x3f, 0x1f, 0xff };
static const uint8_t rtc_min[7] = { 0, 0, 0, 0, 1, 1, 0 };
static const uint8_t rtc_max[7] = { 59, 59, 23, 6, 31, 12, 99 };

static void pcf5063x_update_irq(Pcf5063xState *s)
{
    uint8_t pending = s->regs[PCF50635_INT6] & ~s->regs[PCF50635_INT6M];

    for (unsigned i = 0; i < 5; i++) {
        pending |= s->regs[PCF5063X_INT1 + i] &
                   ~s->regs[PCF5063X_INT1MASK + i];
    }
    /* Open-drain, active-low interrupt output. */
    qemu_set_irq(s->irq, !pending);
}

static void pcf5063x_gpio2_input(void *opaque, int n, int level)
{
    Pcf5063xState *s = opaque;

    if (s->gpio2_high == !!level) {
        return;
    }
    s->gpio2_high = !!level;
    /*
     * retailOS 2.0.4 0x08362840 maps INT6[1] to its input-change event.
     * 0x080559b4 then reads GPIOSTAT[1] through 0x082da704. The firmware
     * configures GPIO2 as an input; other pin functions remain unmodeled.
     */
    s->regs[PCF50635_INT6] |= PCF50635_GPIO2;
    pcf5063x_update_irq(s);
}

/*
 * The shared RTC register layout is described in PCF50633 UM rev. 06, 8.15.
 * retailOS 2.0.4 reads seven bytes at 0x083627c0 and programs the alarm at
 * 0x083628bc: year 0xaa prevents a match during programming, weekday 7 is
 * ignored, and the remaining six fields are BCD. 0x08362840 maps INT1[6]
 * to its alarm event. Pulse phase and partially specified alarm retrigger
 * behavior have not been measured on the PCF50635.
 */
static bool pcf5063x_rtc_valid_field(unsigned i, uint8_t value)
{
    return (value & 0xf) <= 9 && (value >> 4) <= 9 &&
           from_bcd(value) >= rtc_min[i] && from_bcd(value) <= rtc_max[i];
}

static bool pcf5063x_rtc_decode(Pcf5063xState *s, struct tm *tm)
{
    const uint8_t *r = &s->regs[PCF5063X_RTCSC];
    static const unsigned month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    for (unsigned i = 0; i < 7; i++) {
        if (!pcf5063x_rtc_valid_field(i, r[i])) {
            return false;
        }
    }
    *tm = (struct tm) {
        .tm_sec = from_bcd(r[0]),
        .tm_min = from_bcd(r[1]),
        .tm_hour = from_bcd(r[2]),
        .tm_wday = r[3],
        .tm_mday = from_bcd(r[4]),
        .tm_mon = from_bcd(r[5]) - 1,
        .tm_year = from_bcd(r[6]) + 100,
    };
    return tm->tm_mday <= month_days[tm->tm_mon] +
           (tm->tm_mon == 1 && tm->tm_year % 4 == 0);
}

static void pcf5063x_rtc_encode(Pcf5063xState *s, const struct tm *tm)
{
    uint8_t *r = &s->regs[PCF5063X_RTCSC];

    r[0] = to_bcd(tm->tm_sec);
    r[1] = to_bcd(tm->tm_min);
    r[2] = to_bcd(tm->tm_hour);
    r[3] = tm->tm_wday;
    r[4] = to_bcd(tm->tm_mday);
    r[5] = to_bcd(tm->tm_mon + 1);
    r[6] = to_bcd(tm->tm_year % 100);
}

static void pcf5063x_rtc_advance(Pcf5063xState *s, int64_t seconds)
{
    struct tm tm;
    int weekday;
    int64_t total;
    time_t date;

    if (!pcf5063x_rtc_decode(s, &tm)) {
        /* Counting from invalid BCD/date values is unspecified. */
        return;
    }
    total = mktimegm(&tm) - RTC_EPOCH;
    weekday = (tm.tm_wday + (total % RTC_DAY_SECONDS + seconds) /
               RTC_DAY_SECONDS) % 7;
    total = (total + seconds) % ((int64_t)RTC_CYCLE_DAYS * RTC_DAY_SECONDS);
    date = RTC_EPOCH + total;
    gmtime_r(&date, &tm);
    /* The independent weekday counter need not agree with the date. */
    tm.tm_wday = weekday;
    pcf5063x_rtc_encode(s, &tm);
}

static bool pcf5063x_rtc_match(Pcf5063xState *s)
{
    bool enabled = false;
    struct tm tm;

    if (!pcf5063x_rtc_decode(s, &tm)) {
        return false;
    }
    for (unsigned i = 0; i < 7; i++) {
        uint8_t alarm = s->regs[PCF5063X_RTCSCA + i];

        if (alarm == rtc_masks[i]) {
            continue;
        }
        if (!pcf5063x_rtc_valid_field(i, alarm) ||
            alarm != s->regs[PCF5063X_RTCSC + i]) {
            return false;
        }
        enabled = true;
    }
    return enabled;
}

/* Seconds to the next transition into an alarm match, or -1 if disabled. */
static int64_t pcf5063x_rtc_next_alarm(Pcf5063xState *s)
{
    struct tm tm, limit;
    int alarm[7];
    int64_t skip = 1, seconds, day, tod;
    bool enabled = false;

    if (!pcf5063x_rtc_decode(s, &tm)) {
        return -1;
    }
    for (unsigned i = 0; i < 7; i++) {
        uint8_t value = s->regs[PCF5063X_RTCSCA + i];

        alarm[i] = -1;
        if (value != rtc_masks[i]) {
            if (!pcf5063x_rtc_valid_field(i, value)) {
                return -1;
            }
            alarm[i] = from_bcd(value);
            enabled = true;
        }
    }
    if (!enabled) {
        return -1;
    }
    seconds = mktimegm(&tm) - RTC_EPOCH;
    tod = seconds % RTC_DAY_SECONDS;
    if (pcf5063x_rtc_match(s)) {
        /* Do not retrigger while a partially specified alarm still matches. */
        if (alarm[0] >= 0) {
            skip = 1;
        } else if (alarm[1] >= 0) {
            skip = 60 - tm.tm_sec;
        } else if (alarm[2] >= 0) {
            skip = 3600 - tod % 3600;
        } else if (alarm[3] >= 0 || alarm[4] >= 0) {
            skip = RTC_DAY_SECONDS - tod;
        } else {
            limit = tm;
            limit.tm_sec = limit.tm_min = limit.tm_hour = 0;
            limit.tm_mday = 1;
            if (alarm[5] >= 0) {
                if (++limit.tm_mon == 12) {
                    limit.tm_mon = 0;
                    limit.tm_year++;
                }
            } else {
                limit.tm_mon = 0;
                limit.tm_year++;
            }
            skip = mktimegm(&limit) - RTC_EPOCH - seconds;
        }
    }

    /*
     * Year wraps at 99 and leap years occur every four years. Including the
     * independently writable weekday, the calendar repeats after 700 years.
     * Search by day, not by elapsed second, to bound work after clock jumps
     * and for impossible alarm dates (for example 30 February).
     */
    day = (tod + skip) / RTC_DAY_SECONDS;
    for (unsigned i = 0; i <= RTC_CYCLE_DAYS * 7; i++, day++) {
        time_t date = RTC_EPOCH + ((seconds / RTC_DAY_SECONDS + day) %
                                  RTC_CYCLE_DAYS) * RTC_DAY_SECONDS;
        struct tm candidate;
        int first = i == 0 ? (tod + skip) % RTC_DAY_SECONDS : 0;

        gmtime_r(&date, &candidate);
        if ((alarm[6] >= 0 && alarm[6] != candidate.tm_year % 100) ||
            (alarm[5] >= 0 && alarm[5] != candidate.tm_mon + 1) ||
            (alarm[4] >= 0 && alarm[4] != candidate.tm_mday) ||
            (alarm[3] >= 0 && alarm[3] != (tm.tm_wday + day) % 7)) {
            continue;
        }
        for (int h = first / 3600; h < 24; h++) {
            if (alarm[2] >= 0 && alarm[2] != h) {
                continue;
            }
            for (int m = 0; m < 60; m++) {
                int base = h * 3600 + m * 60;
                int sec = alarm[0] < 0 ? MAX(0, first - base) : alarm[0];

                if ((alarm[1] < 0 || alarm[1] == m) && sec < 60 &&
                    base + sec >= first) {
                    return day * RTC_DAY_SECONDS + base + sec - tod;
                }
            }
        }
    }
    return -1;
}

static void pcf5063x_rtc_alarm_update(Pcf5063xState *s)
{
    bool match = pcf5063x_rtc_match(s);

    if (match && !s->rtc_alarm_match) {
        trace_pcf5063x_rtc_alarm();
        s->regs[PCF5063X_INT1] |= PCF5063X_ALARM;
    }
    s->rtc_alarm_match = match;
    s->rtc_alarm_remaining = pcf5063x_rtc_next_alarm(s);
}

static void pcf5063x_rtc_schedule(Pcf5063xState *s)
{
    int64_t ticks = -1;

    if (!(s->regs[PCF5063X_INT1] & PCF5063X_ALARM)) {
        ticks = s->rtc_alarm_remaining;
    }
    if (s->rtc_pending_mask || !(s->regs[PCF5063X_INT1] & PCF5063X_SECOND)) {
        ticks = 1;
    }
    if (ticks > 0) {
        int64_t available = (INT64_MAX - s->rtc_last_ns) /
                            NANOSECONDS_PER_SECOND;

        if (available > 0) {
            timer_mod(s->rtc_timer, s->rtc_last_ns +
                      MIN(ticks, available) * NANOSECONDS_PER_SECOND);
        } else {
            timer_del(s->rtc_timer);
        }
    } else {
        /*
         * Both events are already latched or the alarm is disabled. Counters
         * are calculated on demand; repeating callbacks cannot change IRQ.
         */
        timer_del(s->rtc_timer);
    }
}

static void pcf5063x_rtc_update(Pcf5063xState *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock);
    int64_t elapsed;

    if (now < s->rtc_last_ns) {
        /* A backwards host-clock adjustment must not reverse the counters. */
        s->rtc_last_ns = now;
    }
    elapsed = (now - s->rtc_last_ns) / NANOSECONDS_PER_SECOND;
    s->rtc_last_ns += elapsed * NANOSECONDS_PER_SECOND;
    if (elapsed) {
        s->regs[PCF5063X_INT1] |= PCF5063X_SECOND;
        if (s->rtc_pending_mask) {
            struct tm tm;

            pcf5063x_rtc_advance(s, 1);
            for (unsigned i = 0; i < 7; i++) {
                if (s->rtc_pending_mask & (1 << i)) {
                    s->regs[PCF5063X_RTCSC + i] = s->rtc_pending[i];
                }
            }
            s->rtc_pending_mask = 0;
            if (!pcf5063x_rtc_decode(s, &tm)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pcf5063x: invalid RTC time/date\n");
            }
            pcf5063x_rtc_alarm_update(s);
            elapsed--;
        }
        if (elapsed) {
            bool alarm = s->rtc_alarm_remaining > 0 &&
                         elapsed >= s->rtc_alarm_remaining;

            pcf5063x_rtc_advance(s, elapsed);
            s->rtc_alarm_match = pcf5063x_rtc_match(s);
            if (alarm) {
                trace_pcf5063x_rtc_alarm();
                s->regs[PCF5063X_INT1] |= PCF5063X_ALARM;
                s->rtc_alarm_remaining = pcf5063x_rtc_next_alarm(s);
            } else if (s->rtc_alarm_remaining > 0) {
                s->rtc_alarm_remaining -= elapsed;
            }
        }
        pcf5063x_update_irq(s);
    }
    pcf5063x_rtc_schedule(s);
}

static void pcf5063x_rtc_tick(void *opaque)
{
    pcf5063x_rtc_update(opaque);
}

static uint8_t pcf5063x_read(Pcf5063xState *s, uint8_t addr)
{
    uint8_t r;

    pcf5063x_rtc_update(s);
    switch (addr) {
    /*
     * ON/OFF controller status. Apple's power manager polls this at boot; with
     * BATOK/SYSOK clear it concludes the battery is empty and either software-
     * resets ("battery low", boot tag "--------") or blocks the UI task waiting
     * for a healthy battery -- which is exactly what wedged our boot at the
     * Apple logo. Report ONKEY|SYSOK|BATOK|TMPOK (0xe1), matching the qemu-ios
     * reference (ipod_classic_pmu). MEASURED: this is the last I2C register the
     * guest reads before the RTXC idle loop WFIs forever.
     */
    case PCF5063X_OOCSTAT:
        r = 0xe1;                    /* ONKEY|SYSOK|BATOK|TMPOK */
        break;
    /*
     * Battery A/D result: a 10-bit raw of 844 (~3900 mV through Rockbox's
     * conversion), the reference's measured default. ADCS3 also carries ADCRDY
     * (0x80) so the firmware's "conversion done" poll completes.
     */
    case PCF5063X_ADCS1:
        r = (844 >> 2) & 0xff;       /* 0xD3 */
        break;
    case PCF5063X_ADCS2:
        r = 0x00;
        break;
    case PCF5063X_ADCS3:
        r = 0x80 | (844 & 0x03);     /* ADCRDY | low bits */
        break;
    /* Main-battery-charger status: TBAT_OK, no charger, no watchdog expiry. */
    case PCF5063X_MBCS1:
        r = 0x00;
        break;
    case PCF5063X_RTCSC ... PCF5063X_MEMBYTE7:
        r = s->regs[addr];
        break;
    case PCF50635_GPIOSTAT:
        r = s->gpio2_high ? PCF50635_GPIO2 : 0;
        break;
    /* Registers the firmware reads at startup; INTx are read to clear. */
    case PCF5063X_GPIO3CFG:
    case PCF5063X_OOCSHDWN:
        r = s->regs[addr];
        break;
    case PCF5063X_INT1:
    case PCF5063X_INT2:
    case PCF5063X_INT3:
    case PCF5063X_INT4:
    case PCF5063X_INT5:
    case PCF50635_INT6:
        r = s->regs[addr];
        s->regs[addr] = 0;
        pcf5063x_update_irq(s);
        pcf5063x_rtc_schedule(s);
        break;
    default:
        trace_pcf5063x_unknown_read(addr);
        r = s->regs[addr];
        break;
    }

    trace_pcf5063x_read(addr, r);
    return r;
}

static void pcf5063x_write(Pcf5063xState *s, uint8_t addr, uint8_t data)
{
    trace_pcf5063x_write(addr, data);
    pcf5063x_rtc_update(s);

    switch (addr) {
    case PCF5063X_RTCSC ... PCF5063X_RTCYR: {
        unsigned i = addr - PCF5063X_RTCSC;

        /* Time/date writes are loaded at the next 1 Hz clock pulse. */
        s->rtc_pending[i] = data & rtc_masks[i];
        s->rtc_pending_mask |= 1 << i;
        break;
    }
    case PCF5063X_RTCSCA ... PCF5063X_RTCYRA:
        s->regs[addr] = data & rtc_masks[addr - PCF5063X_RTCSCA];
        pcf5063x_rtc_alarm_update(s);
        break;
    case PCF5063X_MEMBYTE0 ... PCF5063X_MEMBYTE7:
        s->regs[addr] = data;
        break;
    case PCF5063X_INT1 ... PCF5063X_INT5:
    case PCF50635_INT6:
    case PCF50635_GPIOSTAT:
        /* Read-only event latches. */
        break;
    default:
        trace_pcf5063x_unknown_write(addr);
        s->regs[addr] = data;
        break;
    }
    pcf5063x_update_irq(s);
    pcf5063x_rtc_schedule(s);
}

static int pcf5063x_event(I2CSlave *slave, enum i2c_event event)
{
    Pcf5063xState *s = PCF5063X(slave);

    s->has_word = false;

    return 0;
}

static uint8_t pcf5063x_recv(I2CSlave *slave)
{
    Pcf5063xState *s = PCF5063X(slave);
    uint8_t r = 0;

    r = pcf5063x_read(s, s->word);
    s->word++;

    return r;
}

static int pcf5063x_send(I2CSlave *slave, uint8_t data)
{
    Pcf5063xState *s = PCF5063X(slave);

    if (!s->has_word) {
        s->has_word = true;
        s->word = data;
    } else {
        pcf5063x_write(s, s->word, data);
        s->word++;
    }

    return 0;
}

static void pcf5063x_reset_enter(Object *obj, ResetType type)
{
    Pcf5063xState *s = PCF5063X(obj);

    /* The battery-backed RTC, alarm and GPM survive a system reset. */
    memset(s->regs, 0, PCF5063X_RTCSC);
    memset(&s->regs[PCF5063X_MEMBYTE7 + 1], 0,
           sizeof(s->regs) - PCF5063X_MEMBYTE7 - 1);
    s->has_word = false;
    s->word = 0;
    s->regs[PCF5063X_OOCSHDWN] = 0x08;
    /* External pin levels survive an internal reset. */
}

static void pcf5063x_reset_hold(Object *obj)
{
    Pcf5063xState *s = PCF5063X(obj);

    pcf5063x_update_irq(s);
    pcf5063x_rtc_schedule(s);
}

static int pcf5063x_pre_save(void *opaque)
{
    pcf5063x_rtc_update(opaque);
    return 0;
}

static int pcf5063x_post_load(void *opaque, int version_id)
{
    Pcf5063xState *s = opaque;

    if (s->rtc_last_ns < 0 ||
        s->rtc_last_ns > INT64_MAX - NANOSECONDS_PER_SECOND ||
        (s->rtc_pending_mask & ~0x7f)) {
        return -EINVAL;
    }
    s->rtc_alarm_match = pcf5063x_rtc_match(s);
    s->rtc_alarm_remaining = pcf5063x_rtc_next_alarm(s);
    pcf5063x_rtc_schedule(s);
    pcf5063x_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_pcf5063x = {
    .name = TYPE_PCF5063X,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pcf5063x_pre_save,
    .post_load = pcf5063x_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, Pcf5063xState),
        VMSTATE_UINT8_ARRAY(regs, Pcf5063xState, 256),
        VMSTATE_BOOL(has_word, Pcf5063xState),
        VMSTATE_UINT8(word, Pcf5063xState),
        VMSTATE_BOOL(gpio2_high, Pcf5063xState),
        VMSTATE_INT64(rtc_last_ns, Pcf5063xState),
        VMSTATE_UINT8_ARRAY(rtc_pending, Pcf5063xState, 7),
        VMSTATE_UINT8(rtc_pending_mask, Pcf5063xState),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf5063x_init(Object *obj)
{
    Pcf5063xState *s = PCF5063X(obj);
    struct tm tm;

    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
    qdev_init_gpio_in_named(DEVICE(s), pcf5063x_gpio2_input, "gpio2", 1);
    s->gpio2_high = true;

    qemu_get_timedate(&tm, 0);
    pcf5063x_rtc_encode(s, &tm);
    memcpy(&s->regs[PCF5063X_RTCSCA], rtc_masks, sizeof(rtc_masks));
    s->rtc_alarm_remaining = -1;
    s->rtc_last_ns = qemu_clock_get_ns(rtc_clock);
    s->rtc_timer = timer_new_ns(rtc_clock, pcf5063x_rtc_tick, s);
    timer_mod(s->rtc_timer, s->rtc_last_ns + NANOSECONDS_PER_SECOND);
}

static void pcf5063x_finalize(Object *obj)
{
    Pcf5063xState *s = PCF5063X(obj);

    timer_free(s->rtc_timer);
}

static void pcf5063x_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(klass);

    rc->phases.enter = pcf5063x_reset_enter;
    rc->phases.hold = pcf5063x_reset_hold;
    dc->vmsd = &vmstate_pcf5063x;
    isc->event = pcf5063x_event;
    isc->recv = pcf5063x_recv;
    isc->send = pcf5063x_send;
}

static const TypeInfo pcf5063x_types[] = {
    {
        .name = TYPE_PCF5063X,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Pcf5063xState),
        .instance_init = pcf5063x_init,
        .instance_finalize = pcf5063x_finalize,
        .class_init = pcf5063x_class_init,
    },
};
DEFINE_TYPES(pcf5063x_types);
