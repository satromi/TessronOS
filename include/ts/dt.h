/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dt.h
 *	Calendar (T2EX dt_, design 12.3)
 *
 *	Time is counted from 1985-01-01 00:00:00 UTC, the origin of the
 *	T-Kernel system time (and of BTRON's): TS_TIME is the system time in
 *	seconds. These calls turn that into a broken down date and back, and
 *	format it. A time before the origin is negative.
 */

#ifndef __TS_DT_H__
#define __TS_DT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef D	TS_TIME;		/* seconds since 1985-01-01 UTC */

typedef struct {
	INT	tm_sec;			/* 0-60 (a leap second is not produced) */
	INT	tm_min;			/* 0-59 */
	INT	tm_hour;		/* 0-23 */
	INT	tm_mday;		/* 1-31 */
	INT	tm_mon;			/* 0-11 */
	INT	tm_year;		/* years since 1900 */
	INT	tm_wday;		/* 0-6, Sunday is 0 */
	INT	tm_yday;		/* 0-365 */
	INT	tm_isdst;		/* always 0: summer time is not kept */
} TS_TM;

/*
 * Time zone: minutes east of UTC. Summer time rules are not supported.
 */
IMPORT ER dt_getsystz( INT *p_minutes );
IMPORT ER dt_setsystz( INT minutes );

IMPORT ER dt_gmtime( CONST TS_TIME *t, TS_TM *tm );		/* UTC */
IMPORT ER dt_localtime( CONST TS_TIME *t, TS_TM *tm );		/* system time zone */
IMPORT ER dt_mktime( CONST TS_TM *tm, TS_TIME *p_t );		/* UTC, fields normalised */
IMPORT ER dt_mktime_local( CONST TS_TM *tm, TS_TIME *p_t );

/*
 * Now, from the system clock. Setting it, and setting the zone, go by
 * the protection of the clock device and of the calendar object (design
 * 12.3.1): E_OACV for one who may not.
 */
IMPORT ER dt_gettime( TS_TIME *p_t );
IMPORT ER dt_settime( TS_TIME t );

/*
 * The kernel's: whether the caller may set the time (it may write the
 * clock device); the clock set by a call that has asked already; a
 * setting of the time told on the clock; the zone taken from the
 * calendar object, as when a volume comes.
 */
IMPORT ER   knl_dt_may_set( void );
IMPORT ER   knl_dt_set_clock( TS_TIME t );
IMPORT void knl_dt_time_set( void );
IMPORT void knl_dt_zone_load( void );

/*
 * Format: %Y %m %d %H %M %S %y %j %a %A %b %B %p %I %Z %% and %F (Y-m-d),
 * %T (H:M:S), %D (m/d/y), %c, %x, %X. Returns the length written, or
 * E_PAR when the buffer is too small.
 */
IMPORT INT dt_strftime( char *buf, SZ bufsz, CONST char *fmt, CONST TS_TM *tm );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DT_H__ */
