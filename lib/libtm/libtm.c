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
#include <ts/ob.h>

/*
 * The console. Interrupts disabled keep out only this processor, so the
 * others are kept out by a spin lock. The processor that holds it may
 * take it again: a string goes out a character at a time, and a
 * formatted line (tm_printf) as a string and characters.
 */
LOCAL T_SPLOCK		tm_lock;
LOCAL volatile UW	tm_holder = ~(UW)0;	/* affinity of the holder */
LOCAL UW		tm_depth = 0;

/*
 * The log's edge (ts/ob.h, T_OBEDGE): armed by a read that found
 * nothing new, marked by what is written after it under the console's
 * lock, and kicked once the lock is let go altogether -- not with it
 * held, as the kick may enter the kernel's critical section, inside
 * which the console is written too.
 */
EXPORT T_OBEDGE		knl_tm_log_edge;
LOCAL volatile UW	tm_kick = 0;

LOCAL UW tm_cpu( void )
{
	return CpuHwId();
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
	BOOL	out = FALSE;

	if ( --tm_depth == 0 ) {
		tm_holder = ~(UW)0;
		SpinUnlock(&tm_lock);
		out = TRUE;
	}
	enaint(intsts);
	if ( out && __atomic_exchange_n(&tm_kick, 0, __ATOMIC_SEQ_CST) != 0 ) {
		knl_ob_edge_kick(&knl_tm_log_edge);
	}
}

/*
 * What went to the console, kept: the last TM_LOG_SIZE bytes of it, so
 * that a window can show the console (design 16.5.19). Written
 * under the console's lock; a reader names where it had got to, and is
 * moved on to the oldest byte still kept when it has fallen behind.
 */
#ifdef USE_TESTAGENT
#define TM_LOG_SIZE	( 1024 * 1024 )		/* the test agent reads the start-up from here too */
#else
#define TM_LOG_SIZE	65536
#endif

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
	if ( n == 0 ) {
		knl_obedge_arm(&knl_tm_log_edge);	/* nothing new: told of what comes next */
	}
	tm_out_unlock(imask);

	return n;
}

/* Bytes ever written to the console: where the log ends (OB_SYS_LOG) */
EXPORT UD tm_log_at( void )
{
	UINT	imask = tm_out_lock();
	UD	end = tm_log_end;

	tm_out_unlock(imask);
	return end;
}

/*
 * Urgent output: what a processor says as the system stops -- an
 * exception in the kernel, a processor that takes no interrupt, a lock
 * nobody lets go of (lockup.c). It does not wait for the console's lock,
 * which may be held by the very processor that stopped; it goes to the
 * UART at once, a character at a time, polled. One processor says such
 * a thing at a time; one that has been at it for a second is taken to
 * have stopped saying it, and the next goes on. Ordinary output that
 * meets it waits, at a character, until it is over (a second at most),
 * so that the two do not run into each other.
 */
#define TM_FREE		(~(UW)0)

LOCAL volatile UW	tm_urgent_cpu = TM_FREE;
LOCAL UW		tm_urgent_depth = 0;

Inline UD tm_now( void )
{
	return ReadSysCounterSync();
}

Inline UD tm_frq( void )
{
	UD	f = SysCounterFreq();

	return ( f != 0 ) ? f : 1000000;
}

EXPORT UINT tm_urgent_begin( void )
{
	UINT	intsts = disint();
	UW	me = tm_cpu();
	UD	t0;

	if ( tm_urgent_cpu == me ) {
		tm_urgent_depth++;
		return intsts;
	}
	t0 = tm_now();
	for (;;) {
		UW	exp = TM_FREE;

		if ( __atomic_compare_exchange_n(&tm_urgent_cpu, &exp, me, FALSE,
						 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ) {
			break;
		}
		if ( tm_now() - t0 > tm_frq() ) {
			__atomic_store_n(&tm_urgent_cpu, me, __ATOMIC_RELEASE);
			break;
		}
	}
	tm_urgent_depth = 1;
	return intsts;
}

EXPORT void tm_urgent_end( UINT intsts )
{
	if ( tm_urgent_cpu == tm_cpu() && --tm_urgent_depth == 0 ) {
		__atomic_store_n(&tm_urgent_cpu, TM_FREE, __ATOMIC_RELEASE);
	}
	enaint(intsts);
}

EXPORT void tm_urgent_puts( const UB *s )
{
	static const UB	CR = CHR_CR;

	for ( ; *s != '\0'; s++ ) {
		if ( *s == CHR_LF ) {
			tm_snd_dat(&CR, 1);
		}
		tm_snd_dat(s, 1);
		tm_log[tm_log_end % TM_LOG_SIZE] = *s;
		tm_log_end++;
	}
}

/* Ordinary output waits while another processor says something urgent */
LOCAL void tm_urgent_wait( void )
{
	UW	u = tm_urgent_cpu;
	UD	t0;

	if ( u == TM_FREE || u == tm_cpu() ) {
		return;
	}
	t0 = tm_now();
	while ( tm_urgent_cpu != TM_FREE && tm_now() - t0 < tm_frq() ) {
		;
	}
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
	tm_urgent_wait();
	if (buf == CHR_LF) {
		tm_snd_dat((const UB*)&CR, 1);
	}
	tm_snd_dat(&buf, 1);
	tm_log[tm_log_end % TM_LOG_SIZE] = buf;
	tm_log_end++;
	if ( knl_obedge_came(&knl_tm_log_edge) ) {
		tm_kick = 1;
	}
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