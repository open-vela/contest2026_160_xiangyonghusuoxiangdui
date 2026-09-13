.section .text.boot,"ax"
.global _start
_start:
    movz x0, #0x3020, lsl #16
    mov sp, x0
    bl amp_main
1:  wfe
    b 1b
