/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.03
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/03/31.
 *
 *----------------------------------------------------------------------
 */

/*
 *    libtm.c
 *    T-Monitor compatible calls library
 */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#if USE_TMONITOR
#include "libtm.h"

/*
 * The console. Interrupts disabled keep out only this processor, so the
 * others are kept out by a spin lock. The processor that holds it may
 * take it again: a string goes out a character at a time, and a
 * formatted line (tm_printf) as a string and characters.
 */
LOCAL T_SPLOCK		tm_lock;
LOCAL volatile UW	tm_holder = ~(UW)0;	/* affinity of the holder */
LOCAL UW		tm_depth = 0;

LOCAL UW tm_cpu( void )
{
	UD	m;

	Asm("mrs %0, mpidr_el1" : "=r"(m));
	return (UW)( m & 0xFFFFFF );
}

EXPORT UINT tm_out_lock( void )
{
	UINT	intsts = disint();
	UW	me = tm_cpu();

	if ( tm_holder == me ) {
		tm_depth++;
		return intsts;
	}
	SpinLock(&tm_lock);
	tm_holder = me;
	tm_depth = 1;
	return intsts;
}

EXPORT void tm_out_unlock( UINT intsts )
{
	if ( --tm_depth == 0 ) {
		tm_holder = ~(UW)0;
		SpinUnlock(&tm_lock);
	}
	enaint(intsts);
}

/*
 * What went to the console, kept: the last TM_LOG_SIZE bytes of it, so
 * that a window can show the console (design 16.5.19). Written
 * under the console's lock; a reader names where it had got to, and is
 * moved on to the oldest byte still kept when it has fallen behind.
 */
#define TM_LOG_SIZE	65536

LOCAL UB	tm_log[TM_LOG_SIZE];
LOCAL UD	tm_log_end = 0;		/* bytes ever written */

EXPORT INT tm_log_read( UD *p_pos, UB *buf, INT max )
{
	UINT	imask;
	UD	pos, end;
	INT	n = 0;

	if ( p_pos == NULL || buf == NULL || max <= 0 ) {
		return 0;
	}
	imask = tm_out_lock();
	end = tm_log_end;
	pos = *p_pos;
	if ( pos > end ) {
		pos = end;
	}
	if ( end - pos > TM_LOG_SIZE ) {
		pos = end - TM_LOG_SIZE;
	}
	while ( pos < end && n < max ) {
		buf[n++] = tm_log[pos % TM_LOG_SIZE];
		pos++;
	}
	*p_pos = pos;
	tm_out_unlock(imask);

	return n;
}

/*
 * libtm_init() - libtm Initialize
 * supported only on wait != 0 (polling not supported)
 */
EXPORT void libtm_init(void)
{
	tm_com_init();
}

/*
 * Where the console's input is read from once a driver has the port:
 * the driver takes what comes in off the UART into a buffer of its own,
 * so reading the UART here would find it empty. The hook answers a byte,
 * or -1 when none has come.
 */
EXPORT INT	(*tm_getc_hook)( void ) = NULL;

/*
 * tm_getchar() - Get Character
 * supported only on wait != 0 (polling not supported)
 */
EXPORT INT tm_getchar( INT wait )
{
	UB	p;
	UINT	imask;
	INT	c;

	if ( tm_getc_hook != NULL ) {
		while ( ( c = tm_getc_hook() ) < 0 ) {
			;			/* nothing yet: the hook looks at the UART too */
		}
		return c;
	}
	imask = tm_out_lock();
	tm_rcv_dat(&p, 1);
	tm_out_unlock(imask);

	return (INT)p;
}

/*
 * tm_getline() - Get Line
 * special key is not supported
 */
EXPORT INT tm_getline( UB *buff )
{
	UB* p = buff;
	int len = 0;
	static const char LF = CHR_LF;
	UINT imask;

	imask = tm_out_lock();
	while (1) {
		tm_rcv_dat(p, 1);
		tm_snd_dat(p, 1); /* echo back */
		if (*p == CHR_CR) {
			tm_snd_dat((const UB*)&LF, 1);
			break;
		} else if (*p == CHR_ETX) {
			len = -1;
			break;
		}
		p++; len++;
	}
	*p = 0x00;
	tm_out_unlock(imask);

	return len;
}

/*
 * tm_putchar()
 * Ctrl-C is not supported
 */
EXPORT INT tm_putchar( INT c )
{
	static const char CR = CHR_CR;
	UB buf = (UB)c;
	UINT imask;

	imask = tm_out_lock();
	if (buf == CHR_LF) {
		tm_snd_dat((const UB*)&CR, 1);
	}
	tm_snd_dat(&buf, 1);
	tm_log[tm_log_end % TM_LOG_SIZE] = buf;
	tm_log_end++;
	tm_out_unlock(imask);

	return 0;
}

/*
 * tm_putstring() - Put String
 * Ctrl-C is not supported
 */
EXPORT INT tm_putstring( const UB *buff )
{
	const UB* p = buff;
	UINT imask;

	imask = tm_out_lock();
	while ( *p != (UB)'\0' ) {
		tm_putchar(*p++);
	}
	tm_out_unlock(imask);

	return 0;
}

#endif /* USE_TMONITOR */