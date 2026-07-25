/*
 * Minimal RK3588 PMU Cortex-M0 (ARMv6-M) bring-up firmware.
 *
 * Step 1 (DONE, verified on target): heartbeat proves the AMP "standalone"
 * bring-up path works and the PMU M0 really executes instructions.
 * Step 2a (this version): also print to the shared UART2, proving the M0 can
 * reach SoC peripherals - the prerequisite for talking to the mailbox later.
 *
 * u-boot loads this at CODE_START = 0x07a00000 (mcu_reserved: mcu@7a00000,
 * no-map, 2MB, from rk3588-amp.dtsi) and releases the M0 via SIP 0x82000028.
 * It coexists with the NuttX firmware on cpu_l3 (A55): different core,
 * different reserved region, so the A55 side is untouched.
 *
 * ADDRESS MODEL - now confirmed by TRM Table 9-3 "PMU_M0 Address Remap".
 * The M0 sees six fixed 512MB windows, each relocated by a pmu1_sgrf_soc_conN
 * register (64KB granularity, which is why u-boot passes load>>16):
 *
 *   M0 view       size   type          usage            relocated by
 *   0x00000000+   512MB  Normal WT     ROM/flash (code) soc_con9  <- CODE_START
 *   0x20000000+   512MB  Normal WBWA   on-chip RAM      soc_con10 <- SRAM_START
 *   0x40000000+   512MB  Device XN     peripherals      FIXED -> 0xF0000000
 *   0x60000000+   512MB  Normal WBWA   off-chip DDR     soc_con11
 *   0x80000000+   512MB  Normal WT     off-chip DDR     soc_con11
 *   0xA0000000+     1GB  Device XN     peripherals      soc_con12 <- EXPERI
 *
 * Consequences used here:
 *   - Code/vectors link at 0x00000000 (ARMv6-M has no VTOR; the vector table is
 *     fixed at address 0, and the hardware maps CODE_START there). The code
 *     window is 512MB, so image size is a non-issue.
 *   - Peripherals need NO experi_start config: the 0x40000000 window is fixed to
 *     physical 0xF0000000. So UART2 (0xFEB50000) is at 0x4EB50000 from the M0,
 *     and mailbox0 (0xFEC60000) would be at 0x4EC60000.
 *
 * UART use is "borrow-only", exactly like the A55 bare-metal demo: only poll
 * LSR.THRE and write THR. Never touch baud/LCR/FIFO and never enable an IRQ -
 * Linux's fiq-debugger owns this UART (console ttyFIQ0 @1500000 8N1).
 *
 * Progress markers in mcu_reserved, read from Linux with /dev/mem:
 *   0x07a00800  magic   0x414D5030 ("AMP0")  - M0 started, in-window store OK
 *   0x07a00804  stage   1 = about to touch UART, 2 = UART hello completed
 *   0x07a00808  counter increments forever   - M0 still alive
 * If the peripheral window were wrong, the first UART access would HardFault and
 * lock the core: stage would stay 1 and counter would stay 0. That is the whole
 * point of splitting the markers this way.
 */
	.syntax unified
	.cpu cortex-m0
	.thumb

/* Marker offsets inside the code window (-> physical 0x07a008xx) */
	.equ HB_BASE,   0x00000800
	.equ UART_BASE, 0x4EB50000	/* UART2 0xFEB50000 via fixed periph window */
	.equ UART_THR,  0x00		/* transmit holding register        */
	.equ UART_LSR,  0x14		/* line status register, bit5 = THRE */

/* --- ARMv6-M vector table at the M0's address 0 (remapped to CODE_START) --- */
	.section .vectors, "a"
	.align 2
	.word 0x0000f000          /* [0] initial SP: inside the code window      */
	.word (reset + 1)         /* [1] reset vector, M0-view addr, thumb bit   */

	.text
	.thumb_func
	.global reset
reset:
	ldr  r6, =HB_BASE         /* r6 = marker base (phys 0x07a00800)          */

	ldr  r1, =0x414d5030      /* magic 'AMP0'                                */
	str  r1, [r6, #0]         /* [0x07a00800] = magic                        */

	movs r1, #1
	str  r1, [r6, #4]         /* [0x07a00804] = stage 1 (before UART)        */

	movs r1, #0
	str  r1, [r6, #8]         /* [0x07a00808] = counter 0                    */

	/* --- say hello on the shared UART2 (first peripheral access) --- */
	ldr  r4, =UART_BASE
	ldr  r7, =msg
	bl   puts

	movs r1, #2
	str  r1, [r6, #4]         /* [0x07a00804] = stage 2 (UART worked)        */

	/* --- liveness loop: bump the counter forever --- */
	movs r2, #0
hb_loop:
	adds r2, r2, #1
	str  r2, [r6, #8]         /* [0x07a00808] = counter                      */
	ldr  r3, =0x00300000      /* delay ~1s @200MHz                           */
delay:
	subs r3, r3, #1
	bne  delay
	b    hb_loop

/*
 * puts: write the NUL-terminated string at r7 to the UART at r4.
 * Polled, transmit-only. Clobbers r0, r1, r3, r7.
 */
	.thumb_func
puts:
	ldrb r0, [r7]
	cmp  r0, #0
	beq  puts_done
wait_thre:
	ldr  r1, [r4, #UART_LSR]
	movs r3, #0x20            /* LSR.THRE (bit5): holding reg empty          */
	ands r1, r3
	beq  wait_thre
	str  r0, [r4, #UART_THR]
	adds r7, r7, #1
	b    puts
puts_done:
	bx   lr

	.section .rodata
	.align 2
msg:
	.asciz "\r\n[M0] hello from Cortex-M0 (RK3588 PMU_M0)\r\n"

	.align 2
	.pool
