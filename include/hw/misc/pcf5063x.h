#ifndef HW_MISC_PCF5063X_H
#define HW_MISC_PCF5063X_H

#include "qom/object.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define TYPE_PCF5063X    "pcf5063x"
OBJECT_DECLARE_SIMPLE_TYPE(Pcf5063xState, PCF5063X)

struct Pcf5063xState {
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
    bool has_word;
    uint8_t word;

    uint8_t regs[256];
    qemu_irq irq;
    bool gpio2_high;

    QEMUTimer *rtc_timer;
    int64_t rtc_last_ns;
    int64_t rtc_alarm_remaining;
    uint8_t rtc_pending[7];
    uint8_t rtc_pending_mask;
    bool rtc_alarm_match;
};

#endif /* HW_MISC_PCF5063X_H */
