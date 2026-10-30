/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lockup.h
 *	Finding the machine stopped, and the watchdog (design 14.10)
 *
 *	Every processor takes an interrupt of its own virtual timer twice a
 *	second and marks itself alive (lockup.c). Each of them looks at the
 *	others as it does: one that has taken none for five seconds has its
 *	interrupts masked or is stalled on an access, and what is known of
 *	it -- its task, where it was last seen, the locks it holds or waits
 *	for -- goes to the console at once, without the console's lock. A
 *	processor that waits for a spin lock as long says so itself, with
 *	the processor holding it; the kernel lock held as long, a circle of
 *	tasks waiting on each other's mutexes, and the desktop's loop not
 *	coming back are told too.
 *
 *	The hardware watchdog (the PM block of the Raspberry Pi 4 and 5) is
 *	fed from the same interrupts while nothing of the kind stands. Once
 *	something has stood for the time the setting gives (30 seconds),
 *	it is no longer fed and resets the machine; with the setting 0 it is
 *	stopped and the machine stays as it is, to be looked at.
 */

#ifndef __TS_LOCKUP_H__
#define __TS_LOCKUP_H__

#include <tk/typedef.h>

#define LK_RESET_S_DEFAULT	30	/* seconds from the report to the reset */
#define LK_KICK_S		20	/* the desktop's kicks stopped this long: reported */
#define LK_SILENT_S		10	/* the desktop's loop away this long: reported */

/*
 * The desktop's caretaker kicks the watchdog every second or so; once
 * it has, kicks that stop for LK_KICK_S count as the machine stopped.
 * knl_wdog_kick_stop takes the desktop off the watch (it is ending).
 */
IMPORT void knl_wdog_kick( void );
IMPORT void knl_wdog_kick_stop( void );

/*
 * The task whose loop is watched (0: none), and the loop come round
 * again. A loop away for LK_SILENT_S is told, with where the task
 * waits; the machine is not reset for it.
 */
IMPORT void knl_wdog_watch( ID tskid );
IMPORT void knl_wdog_alive( void );

/*
 * The watched task waits for input (wm_read_event): it is not stuck,
 * however long nobody presses anything. Called by any task; only the
 * watched one counts.
 */
IMPORT void knl_wdog_idle( BOOL on );

/*
 * Whether the watched loop has stayed away for LK_SILENT_S (or the time
 * the tests set): TRUE with the milliseconds it has been away and the
 * task in *p_ms and *p_tsk (either may be NULL). FALSE while nothing is
 * watched, the loop came round, or its task waits for input.
 */
IMPORT BOOL knl_wdog_stuck( UW *p_ms, ID *p_tsk );

/* The task whose loop is watched, 0 none */
IMPORT ID   knl_wdog_watched( void );

/*
 * Seconds from a report to the reset; 0 turns the hardware watchdog
 * off (the reports go on). The machine's setting WATCHDOG.
 */
IMPORT void knl_wdog_set( INT reset_s );

/* The seconds set (0: off), and whether the board's watchdog is fed */
IMPORT INT  knl_wdog_get( BOOL *p_hw );

/* Everything off: the machine is being shut down or started again */
IMPORT void knl_lockup_stop( void );

/*
 * A processor stopped by an exception in the kernel (exc_hdl.c):
 * reported as stopped, not as taking no interrupts.
 */
IMPORT void knl_lockup_halted( void );

/* What was found, for the tests */
typedef struct {
	UW	hard;			/* processors found taking no interrupt */
	UW	back;			/* found taking them again */
	UW	sgi_taken;		/* of those, ones that took the interrupt sent to them */
	UW	sgi_lost;		/* ones that did not */
	UW	spin;			/* waits for a spin lock gone on too long */
	UW	klock;			/* the kernel lock held too long */
	UW	deadlock;		/* circles of tasks waiting on mutexes */
	UW	silent;			/* the watched loop away too long */
	UW	starved;		/* the kicks stopped */
	UW	feeds;			/* times the hardware watchdog was fed */
	BOOL	failing;		/* something stands: the reset is counted down */
	BOOL	hw;			/* there is a hardware watchdog */
	ID	last_prc;		/* the processor last reported (PRC ID) */
} T_LKSTAT;

IMPORT void knl_lockup_stat( T_LKSTAT *st );

/* The interrupts of each processor's timer so far, up to n of them: how many were written */
IMPORT INT knl_lockup_beats( UD *beats, INT n );

#if USE_KTEST
/*
 * The times shortened for the tests (ms; 0 leaves one as it is), and a
 * processor made to look stopped while its interrupts still come
 * (prc is a PRC ID). knl_lockup_tune(0, 0, 0) puts them back.
 */
IMPORT void knl_lockup_tune( UW stop_ms, UW kick_ms, UW silent_ms );
IMPORT void knl_lockup_mute( ID prc, BOOL mute );
IMPORT void knl_lockup_scan( void );	/* look for circles of waits now */
#endif

#endif /* __TS_LOCKUP_H__ */
