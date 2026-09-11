/*
 * S5L8702 UART register interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_S5L8702_UART_H
#define HW_CHAR_S5L8702_UART_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "chardev/char-fe.h"

#define TYPE_S5L8702_UART "s5l8702-uart"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702UartState, S5L8702_UART)

#define S5L8702_UART_FIFO_SIZE 16

struct S5L8702UartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *pclk;
    CharBackend chr;
    QEMUBH *tx_bh;
    guint watch;

    uint32_t ulcon;
    uint32_t ucon;
    uint32_t ufcon;
    uint32_t umcon;
    uint32_t ubrdiv;
    uint32_t ubrcontx;
    uint32_t ubrconrx;
    uint32_t pending;
    uint32_t errors;
    uint8_t rx[S5L8702_UART_FIFO_SIZE];
    uint8_t tx[S5L8702_UART_FIFO_SIZE];
    uint8_t rx_head;
    uint8_t rx_count;
    uint8_t tx_head;
    uint8_t tx_count;
};

#endif /* HW_CHAR_S5L8702_UART_H */
