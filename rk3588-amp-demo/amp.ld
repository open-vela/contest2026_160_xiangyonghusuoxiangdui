ENTRY(_start)
SECTIONS
{
. = 0x30000000;          /* dedicated AMP core region (dts amp-core@30000000) */
.text : { *(.text.boot) *(.text*) }
.rodata : { *(.rodata*) }
.data : { *(.data*) }
.bss : { *(.bss*) *(COMMON) }
}
