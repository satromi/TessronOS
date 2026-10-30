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
	INT	tm_isdst;		/* 1 in summer time (dt_localtime), else 0 */
} TS_TM;

/*
 * Time zone: its standard offset, minutes east of UTC, and the IANA name
 * of the zone it stands for ("Asia/Tokyo"; "" when it names none).
 * dt_localtime and dt_mktime_local follow summer time in the zones that
 * keep it (the United States and Canada, the European Union, the
 * south-east of Australia, New Zealand). dt_setsystz sets the offset
 * alone, and drops the name when the offset is another.
 */
#define DT_ZONE_MAX	48		/* bytes of a zone's name, with the 0 */

IMPORT ER dt_getsystz( INT *p_minutes );
IMPORT ER dt_setsystz( INT minutes );
IMPORT INT dt_getsyszone( char *name, INT max );		/* the name's length */
IMPORT ER dt_setsyszone( CONST char *name, INT minutes );

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

#ifdef __x86_64__
/*
 * The RTC of a PC (9.5.1): kept in the local time, as Windows
 * keeps it, or in UTC. The setting is the calendar object's, and setting
 * it takes what setting the zone takes; the RTC is written again from
 * the system clock, which stays as it is. DT_RTC_FIRMWARE is told when
 * the firmware gave the clock a zone of its own, which is followed
 * whatever the setting.
 */
#define DT_RTC_UTC		0
#define DT_RTC_LOCAL		1
#define DT_RTC_FIRMWARE		0x100

IMPORT ER dt_getrtc( INT *p_mode );
IMPORT ER dt_setrtc( INT mode );

/*
 * The kernel's: the RTC's wall clock (seconds from 1970-01-01 of that
 * clock) as UTC and back (UNIX seconds), the offset looked at again, and
 * the registers themselves (kernel/sysdepend/x64/rtc.c)
 */
IMPORT D    knl_dt_rtc_to_utc( D wall );
IMPORT D    knl_dt_utc_to_rtc( D utc );
IMPORT void knl_dt_rtc_check( void );
IMPORT ER   knl_rtc_read_wall( D *p_wall );
IMPORT ER   knl_rtc_write_wall( D wall );
IMPORT BOOL knl_rtc_fw_zone( INT *p_minutes );
IMPORT void knl_rtc_shift( D sec );
#endif

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
