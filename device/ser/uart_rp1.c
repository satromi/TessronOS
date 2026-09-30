/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uart_rp1.c
 *	UART0 of the RP1: the serial lines of the 40 pin header
 *	(design 4.6, 10.7).
 *
 *	The RP1 UARTs are PL011, the same part as the three pin debug UART
 *	of the BCM2712, so only the base address and the clock differ. The
 *	board brings this one out on header pins 8 and 10, which is where a
 *	usual USB serial lead goes, so it is the console most people have.
 *
 *	The console is moved here by tm_com_set_base() once the port is set
 *	up. Until then everything goes to the debug UART.
 *
 *	NOT VERIFIED ON HARDWARE (docs/private/checklists/phase7.md item 32).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/gpio.h>
#include <tm/tmonitor.h>
#include "sysdepend.h"

#define UART_DR		(RP1_UART0_BASE + 0x00)
#define UART_FR		(RP1_UART0_BASE + 0x18)
#define UART_IBRD	(RP1_UART0_BASE + 0x24)
#define UART_FBRD	(RP1_UART0_BASE + 0x28)
#define UART_LCRH	(RP1_UART0_BASE + 0x2c)
#define UART_CR		(RP1_UART0_BASE + 0x30)
#define UART_IFLS	(RP1_UART0_BASE + 0x34)
#define UART_IMSC	(RP1_UART0_BASE + 0x38)
#define UART_ICR	(RP1_UART0_BASE + 0x44)

#define FR_BUSY		0x0008
#define FR_TXFF		0x0020
#define UART_BUSY_SPIN	1000000		/* reads of FR before giving up on a port that never drains */

#define LCRH_FEN	0x0010			/* the FIFOs */
#define LCRH_WLEN_8	0x0060

#define CR_UARTEN	0x0001
#define CR_TXE		0x0100
#define CR_RXE		0x0200

/*
 * The clock the RP1 feeds its UARTs. The device tree of the board gives
 * 50MHz; a board that differs needs this changed (item 32).
 */
#define RP1_UART_CLK	50000000

/* The header pins the UART comes out on, and the function number */
#define RP1_UART0_TX_PIN	14
#define RP1_UART0_RX_PIN	15
#define RP1_UART0_FUNC		4

/*
 * Set the port up at the given speed, 8 bits, no parity, one stop bit.
 */
EXPORT ER rp1_uart_init( UW baud )
{
	UW	div, ibrd, fbrd;
	INT	i;

	if ( baud == 0 ) {
		return E_PAR;
	}
	if ( in_w(UART_CR) == 0xFFFFFFFF ) {
		return E_NOEXS;			/* the window is not there */
	}

	/* the two pins belong to the UART, not to the software */
	rp1_gpio_set_func(RP1_UART0_TX_PIN, RP1_UART0_FUNC);
	rp1_gpio_set_func(RP1_UART0_RX_PIN, RP1_UART0_FUNC);
	rp1_gpio_set_pull(RP1_UART0_RX_PIN, GPIO_PULL_UP);

	/* stop, then wait for what is in flight */
	out_w(UART_CR, 0);
	for ( i = 0; (in_w(UART_FR) & FR_BUSY) != 0; i++ ) {
		if ( i >= UART_BUSY_SPIN ) {
			return E_TMOUT;		/* the port does not run: no clock */
		}
	}
	out_w(UART_ICR, 0x7ff);			/* every pending cause */

	/*
	 * The divisor is the clock over sixteen times the speed, kept in
	 * six fractional bits: div = clk * 4 / baud gives both at once.
	 */
	div  = (RP1_UART_CLK * 4) / baud;
	ibrd = div / 64;
	fbrd = div % 64;
	out_w(UART_IBRD, ibrd);
	out_w(UART_FBRD, fbrd);

	out_w(UART_LCRH, LCRH_WLEN_8 | LCRH_FEN);
	out_w(UART_IFLS, 0);			/* the shallowest FIFO marks */
	out_w(UART_IMSC, 0);			/* polled for now */
	out_w(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);

	return E_OK;
}

/*
 * Set the port up and send everything after this to it.
 */
EXPORT ER rp1_uart_console( UW baud )
{
	ER	er = rp1_uart_init(baud);

	if ( er < E_OK ) {
		return er;
	}
	tm_com_set_base(RP1_UART0_BASE);

	return E_OK;
}

#endif /* RPI5 */
