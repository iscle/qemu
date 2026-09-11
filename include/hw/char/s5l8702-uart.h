/*
 * S5L8702 UART register interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_S5L8702_UART_H
#define HW_CHAR_S5L8702_UART_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"

#define TYPE_S5L8702_UART "s5l8702-uart"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702UartState, S5L8702_UART)

#define S5L8702_UART_FIFO_SIZE 16
/* Host transport buffer; this is not part of the hardware FIFO. */
#define S5L8702_UART_OUTPUT_SIZE 4096

struct S5L8702UartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *pclk;
    Clock *uclk;
    QEMUTimer timer;
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

    uint8_t tx_shift;
    bool tx_busy;
    /* Remaining source cycles in units of 2^-32, including partial cycles. */
    uint64_t tx_left;
    uint64_t rx_left;
    int64_t last_ns;
    uint8_t output[S5L8702_UART_OUTPUT_SIZE];
    uint16_t output_head;
    uint16_t output_count;
    uint64_t output_dropped;
};

#endif /* HW_CHAR_S5L8702_UART_H */
