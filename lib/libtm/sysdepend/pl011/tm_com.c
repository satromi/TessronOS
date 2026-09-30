/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tm_com.c
 *	T-Monitor communication: PL011 UART at DBG_UART_BASE, polled.
 *	The UART is already configured by the firmware (Raspberry Pi 5) or the
 *	emulator (QEMU virt), so no baud rate setup is done here.
 */

#include <tk/tkernel.h>

#if USE_TMONITOR
#include "../../libtm.h"

#ifdef CPU_CORE_ARMV8A
#if TM_COM_SERIAL_DEV

/*
 * The console starts on the UART the firmware left running and can be
 * moved to another one later (the Raspberry Pi 5 can take the header
 * UART of the RP1 instead of the three pin debug one). Everything here
 * is PL011, so only the base changes.
 */
EXPORT UBINT	tm_uart_base = DBG_UART_BASE;

#define UART_DR		(tm_uart_base + PL011_DR)
#define UART_FR		(tm_uart_base + PL011_FR)

EXPORT	void	tm_snd_dat( const UB* buf, INT size )
{
	while ( size-- > 0 ) {
		while ( (in_w(UART_FR) & PL011_FR_TXFF) != 0 ) {
			;	/* transmit FIFO full */
		}
		out_w(UART_DR, *buf++);
	}
}

EXPORT	void	tm_rcv_dat( UB* buf, INT size )
{
	while ( size-- > 0 ) {
		while ( (in_w(UART_FR) & PL011_FR_RXFE) != 0 ) {
			;	/* receive FIFO empty */
		}
		*buf++ = (UB)in_w(UART_DR);
	}
}

EXPORT	void	tm_com_init( void )
{
	/* nothing: the firmware/emulator has set up 115200 8N1 */
}

/*
 * Move the console to another PL011. The caller has set that one up;
 * what is in flight on the old one is let go.
 */
EXPORT	void	tm_com_set_base( UBINT base )
{
	while ( (in_w(UART_FR) & PL011_FR_TXFF) != 0 ) {
		;	/* let the last characters out first */
	}
	tm_uart_base = base;
}

#endif /* TM_COM_SERIAL_DEV */
#endif /* CPU_CORE_ARMV8A */
#endif /* USE_TMONITOR */
