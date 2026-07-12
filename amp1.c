/* RK3588 AMP demo payload for cpu_l3 (MPIDR 0x300). Entry via amp_start.S (sets SP).
 * Proofs of life:
 *   UART2 (0xFEB50000) print - SHARED with Linux console (fiq-debugger).
 *     Borrow-only: poll LSR.THRE + write THR, NEVER reconfigure baud/FIFO/LCR,
 *     no IRQ (fiq-debugger owns the FIQ). Output interleaves with Linux log.
 *   Heartbeat in no-map amp-core region (Linux reads via /dev/mem):
 *     0x30000800 = magic 0x414D5033 ("AMP3"); 0x30000804 = counter++ forever
 */
#define UART2_BASE 0xFEB50000UL
#define UART_THR (*(volatile unsigned int *)(UART2_BASE + 0x00))
#define UART_LSR (*(volatile unsigned int *)(UART2_BASE + 0x14))
#define LSR_THRE (1u << 5)

#define HB_MAGIC (*(volatile unsigned int *)(0x30000800UL))
#define HB_COUNT (*(volatile unsigned int *)(0x30000804UL))

static void uart_putc(char c)
{
unsigned int spins = 0;
while (!(UART_LSR & LSR_THRE) && ++spins < 100000)
;
UART_THR = (unsigned int)c;
}

void amp_main(void)
{
const char *s = "hello from cpu3 (RK3588 AMP core alive)\r\n";
volatile unsigned int d;
unsigned int c = 0;

HB_MAGIC = 0x414D5033u;
HB_COUNT = 0u;

while (*s) {
if (*s == '\n')
uart_putc('\r');
uart_putc(*s++);
}

for (;;) {
HB_COUNT = ++c;
for (d = 0; d < 2000000; d++)
;
}
}
