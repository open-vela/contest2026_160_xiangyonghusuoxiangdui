/*
 * Link the minimal PMU M0 firmware for the REMAP-TO-ZERO address model.
 *
 * v1 assumed CODE_START (0x07a00000) was the M0 instruction-fetch base and
 * linked there. On target that produced no heartbeat at all (0xFFFFFFFF), which
 * showed the M0 fetches from its OWN address 0 and the hardware remaps that
 * window to CODE_START. Evidence: u-boot passes only the top 16 bits to the SIP
 * (0xffff0000 | (load >> 16)), i.e. a 64KB remap window base, not an absolute PC.
 *
 * So: code + vectors are linked at 0x00000000 (M0's view). Data accesses still
 * use PHYSICAL addresses (0x07a1xxxx) because the remap only covers the low
 * fetch window; the M0 is a bus master for normal loads/stores - that is also
 * why Linux can observe the same words via /dev/mem.
 */
ENTRY(reset)

/*
 * Window size per TRM Table 9-3: the M0's code window (0x00000000-0x1FFFFFFF)
 * is 512MB, so the image size is limited only by the reserved region behind it.
 * CODE_START points at mcu_reserved (mcu@7a00000, 2MB), so cap the link at 2MB.
 * (The earlier 64KB LENGTH here was a guess made before the TRM was available;
 * 64KB is just the *relocation granularity*, not the window size.)
 */
MEMORY
{
	/* M0's view of the remapped window (maps to CODE_START = 0x07a00000) */
	CODE (rwx) : ORIGIN = 0x00000000, LENGTH = 0x00200000  /* 2MB = mcu_reserved */
}

SECTIONS
{
	.vectors 0x00000000 : { KEEP(*(.vectors)) } > CODE
	.text            : { *(.text*) *(.rodata*) } > CODE
	/DISCARD/         : { *(.ARM.exidx*) *(.comment) *(.note*) *(.eh_frame*) }
}
