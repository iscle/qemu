/*
 * A32 compare/test encodings with a nonzero SBZ destination field.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The ARM926 model ignores this field; other models retain UNDEF. Check all
 * forms, Rd = 4 and 15, flag results, unchanged operands and predication.
 * Run on versatilepb with -cpu arm926 or -cpu arm1176 and -semihosting.
 */
.syntax unified
.arm
.global _start

_start:
    b reset
    b undefined
    b fail
    b fail
    b fail
    b fail
    b fail
    b fail

/* r9 counts tests, r10 selects UNDEF, r11 counts exceptions. */
reset:
    mov r9, #0
    mov r11, #0
    mrc p15, 0, r0, c0, c0, 0
    lsr r0, r0, #4
    ldr r1, =0xfff
    and r0, r0, r1
    ldr r1, =0x926
    cmp r0, r1
    mov r10, #0
    movne r10, #1

.macro test opcode, flags, rd
    add r9, r9, #1
    mov r0, #0x80000000
    mov r1, #1
    mov r2, #4
    ldr r4, =0x13579bdf
    mov r5, #0x30000000
    msr cpsr_f, r5
    .inst (\opcode | (\rd << 12))
    mrs r6, cpsr
    cmp r10, #0
    bne 1f
    lsr r6, r6, #28
    cmp r6, #\flags
    bne fail
1:
    /* Canonical encoding is a control case on both CPU models. */
    msr cpsr_f, r5
    .inst \opcode
    mrs r6, cpsr
    lsr r6, r6, #28
    cmp r6, #\flags
    bne fail
    ldr r6, =0x13579bdf
    cmp r4, r6
    bne fail
    cmp r0, #0x80000000
    bne fail
    cmp r1, #1
    bne fail
    cmp r2, #4
    bne fail
.endm

.irp rd, 4, 15
    test 0xe3100000, 7, \rd  /* TST r0, #0 */
    test 0xe3300000, 11, \rd /* TEQ r0, #0 */
    test 0xe3500000, 10, \rd /* CMP r0, #0 */
    test 0xe3700001, 8, \rd  /* CMN r0, #1 */
    test 0xe1100081, 5, \rd  /* TST r0, r1, LSL #1 */
    test 0xe1300081, 9, \rd  /* TEQ r0, r1, LSL #1 */
    test 0xe1500081, 3, \rd  /* CMP r0, r1, LSL #1 */
    test 0xe1700081, 8, \rd  /* CMN r0, r1, LSL #1 */
    test 0xe1100211, 5, \rd  /* TST r0, r1, LSL r2 */
    test 0xe1300211, 9, \rd  /* TEQ r0, r1, LSL r2 */
    test 0xe1500211, 3, \rd  /* CMP r0, r1, LSL r2 */
    test 0xe1700211, 8, \rd  /* CMN r0, r1, LSL r2 */
.endr

    /* A failed condition must not execute or raise UNDEF. */
    mov r5, #0x30000000
    msr cpsr_f, r5
    .inst 0x03504000  /* CMPEQ r0, #0, with Rd = 4 */
    .inst 0x01504081  /* CMPEQ r0, r1, LSL #1, with Rd = 4 */
    .inst 0x01504211  /* CMPEQ r0, r1, LSL r2, with Rd = 4 */
    mrs r6, cpsr
    lsr r6, r6, #28
    cmp r6, #3
    bne fail
    mov r6, #24
    mul r6, r10, r6
    cmp r11, r6
    bne fail
    ldr r6, =thumb_test + 1
    blx r6
    mov r9, #0
    b exit

undefined:
    cmp r10, #1
    bne fail
    add r11, r11, #1
    movs pc, lr

fail:
    /* Include the failed case index in the semihosting exit status. */
    add r9, r9, #1
exit:
    ldr r1, =exit_block
    str r9, [r1, #4]
    mov r0, #0x20  /* SYS_EXIT_EXTENDED */
    svc 0x123456
    b .

.thumb
thumb_test:
    /* These share translation helpers but have no SBZ destination field. */
    movs r3, #1
    cmp r3, #1
    bne thumb_fail
    mov r8, r3
    mov r12, r9
    mov r9, r3
    cmp r8, r9
    mov r9, r12
    bne thumb_fail
    bx lr
thumb_fail:
    ldr r6, =fail
    bx r6

.arm
.align 2
exit_block:
    .word 0x20026, 0
    .ltorg
