/* S5L8702 clickwheel serial controller. */
#ifndef HW_MISC_S5L8702_CLICKWHEEL_H
#define HW_MISC_S5L8702_CLICKWHEEL_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_S5L8702_CLICKWHEEL "s5l8702-clickwheel"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702ClickwheelState, S5L8702_CLICKWHEEL)

#define S5L8702_CLICKWHEEL_BASE 0x3c200000
#define S5L8702_CLICKWHEEL_SIZE 0x100
#define S5L8702_CWHEEL_IRQ 23
#define S5L8702_WHEEL_FIFO_SIZE 16
#define S5L8702_WHEEL_BUTTON_COUNT 5

struct S5L8702ClickwheelState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq button[S5L8702_WHEEL_BUTTON_COUNT];
    QemuInputHandlerState *input;
    QEMUTimer timer;
    uint32_t control;
    uint32_t command;
    uint32_t divider;
    uint32_t mask;
    uint32_t pending;
    uint32_t tx;
    uint32_t fifo[S5L8702_WHEEL_FIFO_SIZE];
    uint32_t head;
    uint32_t count;
    uint32_t buttons;
    uint16_t keys;
    uint32_t position;
    int32_t steps;
    uint32_t reports;
    bool touched;
    int64_t untouch_at;
};
#endif
