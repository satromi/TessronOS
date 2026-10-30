/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	datetime.c
 *	Calendar (T2EX dt_, design 12.3).
 *
 *	The conversion between a day number and a civil date uses the
 *	era based formulas, which are exact for the whole range of a 64 bit
 *	second count and need no table of leap years.
 *
 *	Setting the time and the zone goes through objects (design 12.3.1).
 *	The time is the clock device's: whoever sets it must be one who may
 *	write the clock. The zone is kept in the metadata of the calendar
 *	object (SYSDEF_CALENDAR, tessronos.calendar.tz and .zone): setting it
 *	is writing that object's attributes, as its protection lets the
 *	caller, and it is read back from there when a volume comes. Either
 *	change is told on the clock with OB_E_CHANGE; the zone's also on the
 *	calendar, as any change of attributes is.
 *
 *	The zone is its standard offset, in minutes east of UTC, and the name
 *	of the IANA time zone it stands for ("Asia/Tokyo"), which programs
 *	with a time zone database of their own (the browser's ICU) use. The
 *	local time follows summer time in the zones whose name is in the
 *	table of rules below (the United States and Canada, the European
 *	Union, the south-east of Australia, New Zealand).
 *
 *	On a PC the RTC may keep the local time, as Windows keeps it, or UTC:
 *	the calendar says which (tessronos.calendar.rtc, "local" or "utc"),
 *	and the offset from UTC it was last written with (.rtcofs, minutes
 *	east), by which it is read; see the part on the RTC below.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dt.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/proc.h>
#include <ts/sysdef.h>
#include "obj/obj.h"
#ifdef _X64_
#include <tm/tmonitor.h>
#endif

#define SEC_PER_MIN	60
#define SEC_PER_HOUR	3600
#define SEC_PER_DAY	86400

#define TZ_MIN		( -12 * 60 )
#define TZ_MAX		( 14 * 60 )

LOCAL INT	sys_tz = CNF_TIMEZONE;	/* minutes east of UTC, until the calendar says */
LOCAL char	sys_zone[DT_ZONE_MAX] = CNF_TIMEZONE_NAME;	/* its IANA name, "" when none */

/* The summer time rules, and the zones that keep them */
#define DST_NONE	0
#define DST_US		1	/* 2nd Sunday of March 2:00 to 1st Sunday of November 2:00, local */
#define DST_EU		2	/* last Sunday of March to last Sunday of October, 1:00 UTC */
#define DST_AU		3	/* 1st Sunday of October 2:00 to 1st Sunday of April 3:00, local */
#define DST_NZ		4	/* last Sunday of September 2:00 to 1st Sunday of April 3:00, local */

LOCAL CONST struct {
	CONST char	*name;
	UB		rule;
} dst_zones[] = {
	{ "America/Anchorage", DST_US },	{ "America/Los_Angeles", DST_US },
	{ "America/Denver", DST_US },		{ "America/Chicago", DST_US },
	{ "America/New_York", DST_US },		{ "America/Halifax", DST_US },
	{ "Atlantic/Azores", DST_EU },		{ "Europe/London", DST_EU },
	{ "Europe/Paris", DST_EU },		{ "Europe/Berlin", DST_EU },
	{ "Europe/Athens", DST_EU },		{ "Australia/Adelaide", DST_AU },
	{ "Australia/Sydney", DST_AU },		{ "Australia/Melbourne", DST_AU },
	{ "Pacific/Auckland", DST_NZ },
};

LOCAL UB	sys_dst = DST_NONE;	/* the rule of sys_zone */

#ifdef _X64_
#define UNIX_TRON	( 473385600LL )	/* seconds from 1970-01-01 to 1985-01-01 */
#define RTC_OFS_NONE	( 0x7fffffff )
#define RTC_OFS_MAX	( 24 * 60 )
#define RTC_CHECK_MS	( 60 * 1000 )	/* how often the offset the RTC is to hold is looked at */

LOCAL BOOL	rtc_local = (BOOL)CNF_RTC_LOCAL;	/* the setting: the RTC keeps the local time */
LOCAL INT	rtc_kept = RTC_OFS_NONE;	/* minutes east the calendar says the RTC was written with */
LOCAL INT	rtc_held = RTC_OFS_NONE;	/* minutes east the RTC holds, as last read or written */
LOCAL BOOL	rtc_written;			/* written since the start */
LOCAL BOOL	rtc_seen;			/* read at the start, the system clock taken from it */
LOCAL D		rtc_boot_wall, rtc_boot_utc;	/* what it read then, and what that was taken as */
LOCAL BOOL	rtc_conf;			/* the calendar has said how it is kept */
LOCAL ID	rtc_tskid;
LOCAL ID	rtc_sem;			/* one writer of the calendar at a time: the volume takes one */
#ifdef USE_KTEST
LOCAL INT	rtc_fw_test = RTC_OFS_NONE;
#endif
#define ZONE_V_MAX	( 96 + DT_ZONE_MAX * 2 )
#else
#define ZONE_V_MAX	( 32 + DT_ZONE_MAX * 2 )
#endif

LOCAL CONST char *wday_short[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
LOCAL CONST char *wday_long[]  = { "Sunday", "Monday", "Tuesday", "Wednesday",
				   "Thursday", "Friday", "Saturday" };
LOCAL CONST char *mon_short[]  = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
				   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
LOCAL CONST char *mon_long[]   = { "January", "February", "March", "April",
				   "May", "June", "July", "August",
				   "September", "October", "November", "December" };

/*
 * Days since 1985-01-01 to a civil date. The arithmetic counts from
 * 0000-03-01; 1985-01-01 is day 724947 of it.
 */
LOCAL void civil_from_days( D days, D *p_y, INT *p_m, INT *p_d )
{
	D	era, doe, yoe, y, doy, mp;
	INT	m, d;

	days += 724947;
	era = ( (days >= 0) ? days : days - 146096 ) / 146097;
	doe = days - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y   = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp  = (5 * doy + 2) / 153;
	d   = (INT)(doy - (153 * mp + 2) / 5 + 1);
	m   = (INT)(mp + ( mp < 10 ? 3 : -9 ));

	*p_y = y + ( (m <= 2) ? 1 : 0 );
	*p_m = m;
	*p_d = d;
}

/*
 * A civil date to days since 1985-01-01
 */
LOCAL D days_from_civil( D y, INT m, INT d )
{
	D	era, yoe, doy, doe;

	y -= ( m <= 2 ) ? 1 : 0;
	era = ( (y >= 0) ? y : y - 399 ) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + ( (m > 2) ? -3 : 9 )) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 724947;
}

/* The day of the week of a day number, Sunday 0 (1985-01-01 was a Tuesday) */
LOCAL INT wday_of( D days )
{
	D	w = ( days + 2 ) % 7;

	return (INT)( ( w < 0 ) ? w + 7 : w );
}

/* The n-th Sunday of a month (the last when n is 0), as a day number */
LOCAL D sunday_of( D y, INT m, INT n )
{
	D	d;

	if ( n > 0 ) {
		d = days_from_civil(y, m, 1);
		return d + ( 7 - wday_of(d) ) % 7 + (D)( n - 1 ) * 7;
	}
	d = ( ( m == 12 ) ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, m + 1, 1) ) - 1;
	return d - wday_of(d);
}

/* The rule of a zone's name */
LOCAL UB dst_rule( CONST char *name )
{
	INT	i;

	for ( i = 0; i < (INT)( sizeof(dst_zones) / sizeof(dst_zones[0]) ); i++ ) {
		if ( knl_strcmp(dst_zones[i].name, name) == 0 ) {
			return dst_zones[i].rule;
		}
	}
	return DST_NONE;
}

LOCAL ER to_tm( TS_TIME t, TS_TM *tm );

/* Whether the system's zone keeps summer time at a moment (UTC) */
LOCAL BOOL dst_at( TS_TIME t )
{
	TS_TM	tm;
	TS_TIME	std = (TS_TIME)sys_tz * SEC_PER_MIN, a, b;
	D	y;

	if ( sys_dst == DST_NONE ) {
		return FALSE;
	}
	(void)to_tm(t + std, &tm);
	y = (D)tm.tm_year + 1900;
	switch ( sys_dst ) {
	case DST_US:
		a = sunday_of(y, 3, 2) * SEC_PER_DAY + 2 * SEC_PER_HOUR - std;
		b = sunday_of(y, 11, 1) * SEC_PER_DAY + 1 * SEC_PER_HOUR - std;
		return (BOOL)( t >= a && t < b );
	case DST_EU:
		a = sunday_of(y, 3, 0) * SEC_PER_DAY + 1 * SEC_PER_HOUR;
		b = sunday_of(y, 10, 0) * SEC_PER_DAY + 1 * SEC_PER_HOUR;
		return (BOOL)( t >= a && t < b );
	case DST_AU:
	case DST_NZ:
		a = sunday_of(y, ( sys_dst == DST_AU ) ? 10 : 9, ( sys_dst == DST_AU ) ? 1 : 0) * SEC_PER_DAY
		  + 2 * SEC_PER_HOUR - std;
		b = sunday_of(y, 4, 1) * SEC_PER_DAY + 2 * SEC_PER_HOUR - std;
		return (BOOL)( t >= a || t < b );
	default:
		return FALSE;
	}
}

LOCAL BOOL is_leap( D y )
{
	return ( (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0) );
}

/* ---------------------------------------------------------------- the objects */

/* The time, or the zone, changed: those who watch the clock are told */
LOCAL void clock_told( void )
{
	knl_ob_post(&ob_uuid_clock, 1, OB_E_CHANGE, NULL);
}

/*
 * Whether the caller may set the time: it may write the clock device.
 * The kernel's own tasks act as the system, before the objects are
 * there as well.
 */
EXPORT ER knl_dt_may_set( void )
{
	ID	pid = ts_get_pid();

	if ( pid <= 0 ) {
		return E_OK;
	}
	return ( knl_ob_permit_on(&ob_uuid_clock, knl_ob_crd_of(pid)) & OB_OP_WRITE )
		? E_OK : E_OACV;
}

/* The time was set, by whatever call: told on the clock */
EXPORT void knl_dt_time_set( void )
{
	clock_told();
}

/* The zone written into the calendar's metadata, through a key that may */
LOCAL ER zone_store( ID key, INT minutes, CONST char *name )
{
	UB	*j = NULL;
	UB	v[ZONE_V_MAX];
	SZ	asz = 0;
	INT	n, len, cap = 0;
	ER	er;

	/* the metadata whole, with room for the zone written into it */
	len = knl_ob_atr_all(key, ZONE_V_MAX + 64, &j, &cap);
	er = ( len < 0 ) ? (ER)len : E_OK;
	asz = ( len > 0 ) ? (SZ)len : 0;
	if ( er >= E_OK ) {
		n = knl_oj_put(v, 0, sizeof(v), "{\"tz\":");
		n = knl_oj_put_num(v, n, sizeof(v), minutes);
		n = knl_oj_put(v, n, sizeof(v), ",\"zone\":");
		n = knl_oj_put_str(v, n, sizeof(v), (CONST UB *)name);
#ifdef _X64_
		n = knl_oj_put(v, n, sizeof(v), rtc_local ? ",\"rtc\":\"local\"" : ",\"rtc\":\"utc\"");
		if ( rtc_held != RTC_OFS_NONE ) {
			n = knl_oj_put(v, n, sizeof(v), ",\"rtcofs\":");
			n = knl_oj_put_num(v, n, sizeof(v), rtc_held);
		}
#endif
		n = knl_oj_put(v, n, sizeof(v), "}");
		len = knl_oj_set_path(j, (INT)asz, cap, "tessronos", "calendar", v, n);
		er = ( len > 0 ) ? ob_set_atr(key, j, len) : E_LIMIT;
#ifdef _X64_
		if ( er >= E_OK ) {
			rtc_kept = rtc_held;
		}
#endif
	}
	if ( j != NULL ) Kfree(j);
	return er;
}

/*
 * The zone the calendar's metadata says, and its name into name ("" when
 * it names none); FALSE when it says no zone
 */
LOCAL BOOL zone_read( ID key, INT *p_minutes, char *name )
{
	UB	*j = NULL;
	SZ	asz = 0;
	D	v = 0;
	INT	p, c, len;
	BOOL	got = FALSE;

	name[0] = '\0';
	len = knl_ob_atr_all(key, 0, &j, NULL);
	if ( len < 0 ) {
		return FALSE;
	}
	asz = (SZ)len;
	{
		c = knl_oj_path(j, (INT)asz, "tessronos", "calendar");
		p = ( c >= 0 ) ? knl_oj_member(j, (INT)asz, c, "tz") : -1;
		got = (BOOL)( p >= 0 && knl_oj_num(j, (INT)asz, p, &v) && v >= TZ_MIN && v <= TZ_MAX );
		p = ( c >= 0 ) ? knl_oj_member(j, (INT)asz, c, "zone") : -1;
		if ( p < 0 || knl_oj_str(j, (INT)asz, p, (UB *)name, DT_ZONE_MAX) < 0 ) {
			name[0] = '\0';
		}
	}
	Kfree(j);
	if ( got ) {
		*p_minutes = (INT)v;
	}
	return got;
}

#ifdef _X64_
LOCAL void rtc_conf_read( ID key );
#endif

/*
 * The zone kept in the calendar, taken as the system's: at the start,
 * and whenever a volume comes, since the calendar may be on it.
 */
#ifdef _X64_
LOCAL void zone_load_cal( void )
#else
EXPORT void knl_dt_zone_load( void )
#endif
{
	TS_UUID	cal;
	INT	z = 0;
	char	name[DT_ZONE_MAX];
	ID	key;

	if ( ob_fnd_sys(SYSROLE_CALENDAR, SYSDEF_CALENDAR, &cal) < E_OK ) {
		return;
	}
	key = ob_opn_obj(&cal, OB_OP_ATRRD);
	if ( key <= 0 ) {
		return;
	}
	if ( zone_read(key, &z, name) ) {
		if ( name[0] == '\0' && z == CNF_TIMEZONE ) {
			knl_strcpy(name, CNF_TIMEZONE_NAME);	/* kept before zones had names: the default's */
		}
		if ( z != sys_tz || knl_strcmp(name, sys_zone) != 0 ) {
			sys_tz = z;
			knl_strcpy(sys_zone, name);
			sys_dst = dst_rule(sys_zone);
			clock_told();
		}
	}
#ifdef _X64_
	rtc_conf_read(key);
#endif
	ob_cls_obj(key);
}

#ifdef _X64_
/* ---------------------------------------------------------------- the PC's RTC */

/*
 * The CMOS clock of a PC holds a wall clock (kernel/sysdepend/x64/rtc.c):
 * the local time where Windows keeps it, else UTC. Which is the zone the
 * firmware gives it when it gives one (EFI_TIME.TimeZone, from the
 * loader), else the setting kept in the calendar (rtc_local; until the
 * calendar is read, CNF_RTC_LOCAL).
 *
 * A clock in the local time is read by the offset it was last written
 * with, which the calendar records (rtcofs), not by the zone's rules
 * alone: after the end or the start of summer time with the machine off
 * the clock still holds the old offset, and the hour that comes twice
 * at the end of summer time is told apart. With no record it is read by
 * the rules, that hour taken as summer time, as dt_mktime_local takes
 * it. Whenever the offset it is to hold changes -- summer time begins
 * or ends, the zone or the setting is changed -- it is written again
 * from the system clock, which stays as it is, and the offset recorded.
 */

LOCAL INT put_str( char *buf, SZ bufsz, INT n, CONST char *s );
LOCAL INT put_num( char *buf, SZ bufsz, INT n, D v, INT width, char pad );

/* The zone the firmware gave its clock, minutes east */
LOCAL BOOL rtc_fw( INT *p_minutes )
{
#ifdef USE_KTEST
	if ( rtc_fw_test != RTC_OFS_NONE ) {
		*p_minutes = rtc_fw_test;
		return TRUE;
	}
#endif
	return knl_rtc_fw_zone(p_minutes);
}

/* The offset the RTC is to hold at a moment (UNIX seconds), minutes east */
LOCAL INT rtc_ofs_at( D utc )
{
	INT	fw;

	if ( rtc_fw(&fw) ) {
		return fw;
	}
	if ( !rtc_local ) {
		return 0;
	}
	return sys_tz + ( dst_at((TS_TIME)( utc - UNIX_TRON )) ? 60 : 0 );
}

/* The RTC's wall clock (seconds from 1970-01-01 of that clock) as UTC */
EXPORT D knl_dt_rtc_to_utc( D wall )
{
	INT	fw;
	D	utc;

	if ( !rtc_conf ) {
		sys_dst = dst_rule(sys_zone);	/* the zone of the build, until the calendar's */
	}
	if ( rtc_fw(&fw) ) {
		utc = wall - (D)fw * SEC_PER_MIN;
	} else if ( !rtc_local ) {
		utc = wall;
	} else if ( rtc_kept != RTC_OFS_NONE ) {
		utc = wall - (D)rtc_kept * SEC_PER_MIN;
	} else {
		utc = wall - (D)sys_tz * SEC_PER_MIN;
		if ( dst_at((TS_TIME)( utc - SEC_PER_HOUR - UNIX_TRON )) ) {
			utc -= SEC_PER_HOUR;
		}
	}
	rtc_held = (INT)( ( wall - utc ) / SEC_PER_MIN );
	if ( !rtc_seen ) {
		rtc_seen = TRUE;
		rtc_boot_wall = wall;
		rtc_boot_utc = utc;
	}
	return utc;
}

/* UTC (UNIX seconds) as the RTC is to hold it */
EXPORT D knl_dt_utc_to_rtc( D utc )
{
	INT	ofs = rtc_ofs_at(utc);

	rtc_held = ofs;
	rtc_written = TRUE;
	return utc + (D)ofs * SEC_PER_MIN;
}

/* Whether the system clock says the time: set, or taken from an RTC that had one */
LOCAL BOOL rtc_clock_set( void )
{
	return (BOOL)( rtc_written || ( rtc_seen && rtc_boot_utc >= UNIX_TRON ) );
}

/* The RTC written from the system clock, which stays as it is */
LOCAL void rtc_resync( void )
{
	TS_TIME	t;

	if ( rtc_clock_set() && dt_gettime(&t) >= E_OK ) {
		(void)knl_rtc_write((UD)( t + UNIX_TRON ));
	}
}

/*
 * The calendar written by one of the calendar's own writers at a time
 * (the zone, the setting, the task that looks at the RTC): the volume
 * takes one transaction at a time and refuses another with E_BUSY
 */
LOCAL void rtc_lock( void )
{
	if ( rtc_sem > 0 ) {
		(void)tk_wai_sem(rtc_sem, 1, TMO_FEVR);
	}
}

LOCAL void rtc_unlock( void )
{
	if ( rtc_sem > 0 ) {
		(void)tk_sig_sem(rtc_sem, 1);
	}
}

/* The setting and the offset the RTC holds, recorded in the calendar (rtc_lock held) */
LOCAL void rtc_store( void )
{
	TS_UUID	cal;
	ID	key;

	if ( ob_fnd_sys(SYSROLE_CALENDAR, SYSDEF_CALENDAR, &cal) < E_OK ) {
		return;
	}
	key = ob_opn_obj(&cal, OB_OP_ATRRD | OB_OP_ATRWR | OB_O_WEXCL);	/* read, changed, written back */
	if ( key > 0 ) {
		(void)zone_store(key, sys_tz, sys_zone);
		ob_cls_obj(key);
	}
}

/* The calendar's word on the RTC: the setting, and the offset it was written with */
LOCAL void rtc_conf_read( ID key )
{
	UB	*j = NULL;
	UB	w[8];
	SZ	asz = 0;
	D	v = 0;
	INT	c, p, len;

	len = knl_ob_atr_all(key, 0, &j, NULL);
	if ( len < 0 ) {
		return;
	}
	asz = (SZ)len;
	{
		c = knl_oj_path(j, (INT)asz, "tessronos", "calendar");
		p = ( c >= 0 ) ? knl_oj_member(j, (INT)asz, c, "rtc") : -1;
		if ( p >= 0 && knl_oj_str(j, (INT)asz, p, w, sizeof(w)) >= 0 ) {
			if ( knl_strcmp((char *)w, "local") == 0 ) rtc_local = TRUE;
			if ( knl_strcmp((char *)w, "utc") == 0 ) rtc_local = FALSE;
		}
		p = ( c >= 0 ) ? knl_oj_member(j, (INT)asz, c, "rtcofs") : -1;
		if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &v) && v >= -RTC_OFS_MAX && v <= RTC_OFS_MAX ) {
			rtc_kept = (INT)v;
		}
	}
	Kfree(j);
}

/* A moment (UNIX seconds) as "2026-10-19 21:00:05" */
LOCAL void rtc_text( D sec, char *b )
{
	TS_TM	tm;
	INT	n;

	(void)to_tm((TS_TIME)( sec - UNIX_TRON ), &tm);
	n = put_num(b, 24, 0, tm.tm_year + 1900, 4, '0');
	n = put_str(b, 24, n, "-");
	n = put_num(b, 24, n, tm.tm_mon + 1, 2, '0');
	n = put_str(b, 24, n, "-");
	n = put_num(b, 24, n, tm.tm_mday, 2, '0');
	n = put_str(b, 24, n, " ");
	n = put_num(b, 24, n, tm.tm_hour, 2, '0');
	n = put_str(b, 24, n, ":");
	n = put_num(b, 24, n, tm.tm_min, 2, '0');
	n = put_str(b, 24, n, ":");
	n = put_num(b, 24, n, tm.tm_sec, 2, '0');
	b[( n > 0 ) ? n : 0] = '\0';
}

/*
 * The calendar read: the system clock taken from the RTC again as the
 * calendar says it is kept, when nothing has set the clock since the
 * start. The RTC is left as it is.
 */
LOCAL BOOL	rtc_told;

LOCAL void rtc_follow( void )
{
	char	w[24], u[24];
	INT	fw;
	D	utc, d = 0;

	if ( !rtc_seen ) {
		return;
	}
	if ( !rtc_written && rtc_boot_utc >= UNIX_TRON ) {
		utc = knl_dt_rtc_to_utc(rtc_boot_wall);
		d = utc - rtc_boot_utc;
		if ( d != 0 ) {
			knl_rtc_shift(d);
			rtc_boot_utc = utc;
			clock_told();
		}
	}
	if ( rtc_told && d == 0 ) {
		return;
	}
	rtc_told = TRUE;
	rtc_text(rtc_boot_wall, w);
	rtc_text(rtc_boot_utc, u);
	if ( rtc_fw(&fw) ) {
		tm_printf((UB *)"rtc: read %s at the start, in the firmware's zone (%d min east): UTC %s\n",
			  w, fw, u);
	} else if ( rtc_local ) {
		tm_printf((UB *)"rtc: read %s at the start, kept as the local time (%s, %d min east, %s): UTC %s\n",
			  w, ( sys_zone[0] != 0 ) ? sys_zone : "no name", (INT)( ( rtc_boot_wall - rtc_boot_utc ) / SEC_PER_MIN ),
			  ( rtc_kept != RTC_OFS_NONE ) ? "as last written" : "by the zone's rules", u);
	} else {
		tm_printf((UB *)"rtc: read %s at the start, kept as UTC: UTC %s\n", w, u);
	}
	if ( d != 0 ) {
		tm_printf((UB *)"rtc: the system clock moved %d s, as the calendar says the RTC is kept\n", (INT)d);
	}
}

/*
 * The offset the RTC is to hold looked at: written again when it has
 * changed (summer time began or ended, here or while the machine was
 * off), and recorded in the calendar
 */
EXPORT void knl_dt_rtc_check( void )
{
	TS_TIME	t;
	INT	fw, want;

	if ( !rtc_conf || !rtc_clock_set() || dt_gettime(&t) < E_OK ) {
		return;
	}
	want = rtc_ofs_at((D)t + UNIX_TRON);
	if ( !rtc_fw(&fw) && rtc_held != RTC_OFS_NONE && want != rtc_held ) {
		tm_printf((UB *)"rtc: written again: %d min east of UTC, it held %d\n", want, rtc_held);
		(void)knl_rtc_write((UD)( t + UNIX_TRON ));
	}
	if ( rtc_held != RTC_OFS_NONE && rtc_held != rtc_kept ) {
		rtc_lock();
		rtc_store();
		rtc_unlock();
		rtc_kept = rtc_held;		/* not asked again every time, whether or not the calendar took it */
	}
}

LOCAL void rtc_task( INT stacd, void *exinf )
{
	for (;;) {
		knl_dt_rtc_check();
		(void)tk_slp_tsk(RTC_CHECK_MS);
	}
}

/* The zone changed: an RTC in the local time written again for it, the system clock kept */
LOCAL void rtc_zone_moved( void )
{
	INT	fw;

	if ( rtc_conf && rtc_local && !rtc_fw(&fw) ) {
		rtc_resync();
		if ( rtc_held != rtc_kept ) {
			rtc_store();
		}
	}
}

EXPORT void knl_dt_zone_load( void )
{
	T_CSEM	csem;
	T_CTSK	ctsk;
	ID	id;

	zone_load_cal();
	rtc_conf = TRUE;		/* the calendar's word, or none: the defaults stand */
	rtc_follow();
	if ( rtc_tskid > 0 ) {
		return;
	}
	if ( rtc_sem <= 0 ) {
		csem.exinf   = NULL;
		csem.sematr  = TA_TFIFO | TA_FIRST;
		csem.isemcnt = 1;
		csem.maxsem  = 1;
		id = tk_cre_sem(&csem);
		if ( id > 0 ) {
			rtc_sem = id;
		}
	}
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)rtc_task;
	ctsk.itskpri = 30;
	ctsk.stksz   = 16 * 1024;
	id = tk_cre_tsk(&ctsk);
	if ( id > 0 && tk_sta_tsk(id, 0) >= E_OK ) {
		rtc_tskid = id;
	}
}

/* ---------------------------------------------------------------- the RTC's setting */

EXPORT ER dt_getrtc( INT *p_mode )
{
	INT	fw;

	if ( p_mode == NULL ) {
		return E_PAR;
	}
	if ( rtc_fw(&fw) ) {
		*p_mode = DT_RTC_FIRMWARE | ( ( fw != 0 ) ? DT_RTC_LOCAL : DT_RTC_UTC );
	} else {
		*p_mode = rtc_local ? DT_RTC_LOCAL : DT_RTC_UTC;
	}
	return E_OK;
}

/*
 * The RTC kept in the local time or in UTC: written into the calendar
 * as the caller may write its attributes (as the zone is), and the RTC
 * written again from the system clock, which stays as it is. With no
 * calendar to write -- no volume yet -- only an administrator may set
 * it, until the calendar says otherwise.
 */
EXPORT ER dt_setrtc( INT mode )
{
	TS_UUID	cal;
	ID	key;
	ER	er = E_OK;
	BOOL	was = rtc_local;

	if ( mode != DT_RTC_UTC && mode != DT_RTC_LOCAL ) {
		return E_PAR;
	}
	if ( ob_fnd_sys(SYSROLE_CALENDAR, SYSDEF_CALENDAR, &cal) < E_OK ) {
		return E_SYS;
	}
	rtc_lock();
	key = ob_opn_obj(&cal, OB_OP_ATRRD | OB_OP_ATRWR | OB_O_WEXCL);	/* read, changed, written back */
	if ( key <= 0 && ( key != E_NOEXS || !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) ) {
		rtc_unlock();
		return ( key == E_NOEXS ) ? E_OACV : (ER)key;
	}
	rtc_local = (BOOL)( mode == DT_RTC_LOCAL );
	if ( rtc_local != was ) {
		rtc_resync();
	}
	if ( key > 0 ) {
		er = zone_store(key, sys_tz, sys_zone);
		ob_cls_obj(key);
		if ( er < E_OK && rtc_local != was ) {
			rtc_local = was;
			rtc_resync();
		}
	}
	rtc_unlock();
	if ( er >= E_OK && rtc_local != was ) {
		tm_printf((UB *)"rtc: kept as %s from now\n", rtc_local ? "the local time" : "UTC");
		clock_told();
	}
	return er;
}

#ifdef USE_KTEST
/*
 * For the tests: the firmware's zone as if it gave one (RTC_OFS_NONE: as
 * it does), the offset recorded, the one held, and the start taken
 * again -- the RTC read as at the start with the setting given, and the
 * system clock set from it -- before the calendar is read again
 */
EXPORT void knl_dt_rtc_test_fw( INT minutes )
{
	rtc_fw_test = minutes;
}

EXPORT void knl_dt_rtc_test_kept( INT minutes )
{
	rtc_kept = minutes;
}

EXPORT INT knl_dt_rtc_test_held( void )
{
	return rtc_held;
}

EXPORT ER knl_dt_rtc_test_start( BOOL local )
{
	TS_TIME	t;
	D	wall;
	ER	er = knl_rtc_read_wall(&wall);

	if ( er < E_OK || dt_gettime(&t) < E_OK ) {
		return ( er < E_OK ) ? er : E_SYS;
	}
	rtc_local = local;
	rtc_seen = FALSE;
	rtc_written = FALSE;
	rtc_told = FALSE;
	(void)knl_dt_rtc_to_utc(wall);
	knl_rtc_shift(rtc_boot_utc - ( (D)t + UNIX_TRON ));
	return E_OK;
}
#endif /* USE_KTEST */
#endif /* _X64_ */

/* ---------------------------------------------------------------- zone */

EXPORT ER dt_getsystz( INT *p_minutes )
{
	if ( p_minutes == NULL ) {
		return E_PAR;
	}
	*p_minutes = sys_tz;

	return E_OK;
}

/* The zone's IANA name into name (max bytes with the 0): its length, 0 when it has none */
EXPORT INT dt_getsyszone( char *name, INT max )
{
	INT	n = (INT)knl_strlen(sys_zone);

	if ( name == NULL || max <= n ) {
		return E_PAR;
	}
	knl_strcpy(name, sys_zone);
	return n;
}

/*
 * The zone set by its offset alone: the name kept when the offset is the
 * zone's already, dropped otherwise (it would say another offset)
 */
EXPORT ER dt_setsystz( INT minutes )
{
	char	name[DT_ZONE_MAX];

	knl_strcpy(name, ( minutes == sys_tz ) ? sys_zone : "");
	return dt_setsyszone(name, minutes);
}

/*
 * The zone set, its name and its standard offset: written into the
 * calendar as the caller may write its attributes. With no calendar to
 * write -- no volume yet -- only an administrator may set it, and only
 * until the calendar says otherwise. A name is letters, digits and
 * "/_+-", as the IANA names are.
 */
#ifdef _X64_
LOCAL ER zone_set( CONST char *name, INT minutes )
#else
EXPORT ER dt_setsyszone( CONST char *name, INT minutes )
#endif
{
	TS_UUID	cal;
	ID	key;
	ER	er;
	INT	i;

	if ( minutes < TZ_MIN || minutes > TZ_MAX || name == NULL ) {
		return E_PAR;
	}
	for ( i = 0; name[i] != '\0'; i++ ) {
		char	c = name[i];

		if ( i >= DT_ZONE_MAX - 1 || !( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' )
		  || ( c >= '0' && c <= '9' ) || c == '/' || c == '_' || c == '+' || c == '-' ) ) {
			return E_PAR;
		}
	}
	if ( ob_fnd_sys(SYSROLE_CALENDAR, SYSDEF_CALENDAR, &cal) < E_OK ) {
		return E_SYS;
	}
	key = ob_opn_obj(&cal, OB_OP_ATRRD | OB_OP_ATRWR | OB_O_WEXCL);	/* read, changed, written back */
	if ( key > 0 ) {
		er = zone_store(key, minutes, name);
		ob_cls_obj(key);
		if ( er < E_OK ) {
			return er;
		}
	} else if ( key != E_NOEXS || !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return ( key == E_NOEXS ) ? E_OACV : (ER)key;
	}
	if ( minutes != sys_tz || knl_strcmp(name, sys_zone) != 0 ) {
		sys_tz = minutes;
		knl_strcpy(sys_zone, name);
		sys_dst = dst_rule(sys_zone);
		clock_told();
#ifdef _X64_
		rtc_zone_moved();
#endif
	}
	return E_OK;
}

#ifdef _X64_
/* The zone set, one writer of the calendar at a time */
EXPORT ER dt_setsyszone( CONST char *name, INT minutes )
{
	ER	er;

	rtc_lock();
	er = zone_set(name, minutes);
	rtc_unlock();
	return er;
}
#endif

/* ---------------------------------------------------------------- convert */

LOCAL ER to_tm( TS_TIME t, TS_TM *tm )
{
	D	days = t / SEC_PER_DAY;
	D	rem  = t % SEC_PER_DAY;
	D	y;
	INT	m, d;

	if ( rem < 0 ) { rem += SEC_PER_DAY; days--; }

	civil_from_days(days, &y, &m, &d);

	tm->tm_sec  = (INT)(rem % SEC_PER_MIN);
	tm->tm_min  = (INT)((rem / SEC_PER_MIN) % SEC_PER_MIN);
	tm->tm_hour = (INT)(rem / SEC_PER_HOUR);
	tm->tm_mday = d;
	tm->tm_mon  = m - 1;
	tm->tm_year = (INT)(y - 1900);
	/* 1985-01-01 was a Tuesday */
	tm->tm_wday = (INT)(((days % 7) + 9) % 7);
	tm->tm_yday = (INT)(days - days_from_civil(y, 1, 1));
	tm->tm_isdst = 0;

	return E_OK;
}

EXPORT ER dt_gmtime( CONST TS_TIME *t, TS_TM *tm )
{
	if ( t == NULL || tm == NULL ) {
		return E_PAR;
	}
	return to_tm(*t, tm);
}

EXPORT ER dt_localtime( CONST TS_TIME *t, TS_TM *tm )
{
	if ( t == NULL || tm == NULL ) {
		return E_PAR;
	}
	BOOL	dst = dst_at(*t);
	ER	er = to_tm(*t + (TS_TIME)( sys_tz + ( dst ? 60 : 0 ) ) * SEC_PER_MIN, tm);

	tm->tm_isdst = dst ? 1 : 0;
	return er;
}

EXPORT ER dt_mktime( CONST TS_TM *tm, TS_TIME *p_t )
{
	D	y, days;
	INT	m;

	if ( tm == NULL || p_t == NULL ) {
		return E_PAR;
	}
	/* months outside 0-11 are carried into the year */
	y = (D)tm->tm_year + 1900 + tm->tm_mon / 12;
	m = tm->tm_mon % 12;
	if ( m < 0 ) { m += 12; y--; }

	days = days_from_civil(y, m + 1, tm->tm_mday);
	*p_t = days * SEC_PER_DAY
	     + (TS_TIME)tm->tm_hour * SEC_PER_HOUR
	     + (TS_TIME)tm->tm_min * SEC_PER_MIN
	     + tm->tm_sec;

	return E_OK;
}

EXPORT ER dt_mktime_local( CONST TS_TM *tm, TS_TIME *p_t )
{
	ER	er = dt_mktime(tm, p_t);

	if ( er >= E_OK ) {
		/* in summer time the clock is an hour ahead of the standard offset */
		*p_t -= (TS_TIME)sys_tz * SEC_PER_MIN;
		if ( dst_at(*p_t - SEC_PER_HOUR) ) {
			*p_t -= SEC_PER_HOUR;
		}
	}
	return er;
}

/* ---------------------------------------------------------------- clock */

EXPORT ER dt_gettime( TS_TIME *p_t )
{
	SYSTIM	tim;
	ER	er;

	if ( p_t == NULL ) {
		return E_PAR;
	}
	er = tk_get_tim(&tim);
	if ( er < E_OK ) {
		return er;
	}
	*p_t = (D)((((UD)(UW)tim.hi << 32) | tim.lo) / 1000);

	return E_OK;
}

/* The clock set, the caller's right to already known */
EXPORT ER knl_dt_set_clock( TS_TIME t )
{
	SYSTIM	tim;
	UD	ms;

	if ( t < 0 ) {
		return E_PAR;		/* before the origin of the system time */
	}
	ms = (UD)t * 1000;
	tim.hi = (W)(ms >> 32);
	tim.lo = (UW)ms;

	return tk_set_tim(&tim);
}

/* The clock set, as the caller may write the clock device; told on it */
EXPORT ER dt_settime( TS_TIME t )
{
	ER	er;

	if ( t < 0 ) {
		return E_PAR;
	}
	er = knl_dt_may_set();
	if ( er >= E_OK ) {
		er = knl_dt_set_clock(t);
	}
	if ( er >= E_OK ) {
		clock_told();
	}
	return er;
}

/* ---------------------------------------------------------------- format */

LOCAL INT put_str( char *buf, SZ bufsz, INT n, CONST char *s )
{
	while ( *s != '\0' ) {
		if ( n >= (INT)bufsz - 1 ) return -1;
		buf[n++] = *s++;
	}
	return n;
}

LOCAL INT put_num( char *buf, SZ bufsz, INT n, D v, INT width, char pad )
{
	char	tmp[24];
	INT	i = 0, neg = 0;

	if ( v < 0 ) { neg = 1; v = -v; }
	do {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	} while ( v > 0 && i < (INT)sizeof(tmp) );
	if ( neg && i < (INT)sizeof(tmp) ) tmp[i++] = '-';

	while ( i < width ) {
		if ( n >= (INT)bufsz - 1 ) return -1;
		buf[n++] = pad;
		width--;
	}
	while ( i > 0 ) {
		if ( n >= (INT)bufsz - 1 ) return -1;
		buf[n++] = tmp[--i];
	}
	return n;
}

EXPORT INT dt_strftime( char *buf, SZ bufsz, CONST char *fmt, CONST TS_TM *tm )
{
	INT	n = 0;
	INT	h12;

	if ( buf == NULL || fmt == NULL || tm == NULL || bufsz < 1 ) {
		return E_PAR;
	}
	while ( *fmt != '\0' ) {
		if ( *fmt != '%' ) {
			if ( n >= (INT)bufsz - 1 ) return E_PAR;
			buf[n++] = *fmt++;
			continue;
		}
		fmt++;
		switch ( *fmt ) {
		  case 'Y': n = put_num(buf, bufsz, n, tm->tm_year + 1900, 4, '0'); break;
		  case 'y': n = put_num(buf, bufsz, n, (tm->tm_year + 1900) % 100, 2, '0'); break;
		  case 'm': n = put_num(buf, bufsz, n, tm->tm_mon + 1, 2, '0'); break;
		  case 'd': n = put_num(buf, bufsz, n, tm->tm_mday, 2, '0'); break;
		  case 'e': n = put_num(buf, bufsz, n, tm->tm_mday, 2, ' '); break;
		  case 'H': n = put_num(buf, bufsz, n, tm->tm_hour, 2, '0'); break;
		  case 'M': n = put_num(buf, bufsz, n, tm->tm_min, 2, '0'); break;
		  case 'S': n = put_num(buf, bufsz, n, tm->tm_sec, 2, '0'); break;
		  case 'j': n = put_num(buf, bufsz, n, tm->tm_yday + 1, 3, '0'); break;
		  case 'I':
			h12 = tm->tm_hour % 12;
			if ( h12 == 0 ) h12 = 12;
			n = put_num(buf, bufsz, n, h12, 2, '0');
			break;
		  case 'p': n = put_str(buf, bufsz, n, ( tm->tm_hour < 12 ) ? "AM" : "PM"); break;
		  case 'a': n = put_str(buf, bufsz, n, wday_short[tm->tm_wday % 7]); break;
		  case 'A': n = put_str(buf, bufsz, n, wday_long[tm->tm_wday % 7]); break;
		  case 'b': n = put_str(buf, bufsz, n, mon_short[tm->tm_mon % 12]); break;
		  case 'B': n = put_str(buf, bufsz, n, mon_long[tm->tm_mon % 12]); break;
		  case 'Z': n = put_str(buf, bufsz, n, ( sys_tz == 0 ) ? "UTC" : "LOC"); break;
		  case 'F':
			n = put_num(buf, bufsz, n, tm->tm_year + 1900, 4, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, "-");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, tm->tm_mon + 1, 2, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, "-");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, tm->tm_mday, 2, '0');
			break;
		  case 'T':
		  case 'X':
			n = put_num(buf, bufsz, n, tm->tm_hour, 2, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, ":");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, tm->tm_min, 2, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, ":");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, tm->tm_sec, 2, '0');
			break;
		  case 'D':
		  case 'x':
			n = put_num(buf, bufsz, n, tm->tm_mon + 1, 2, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, "/");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, tm->tm_mday, 2, '0');
			if ( n >= 0 ) n = put_str(buf, bufsz, n, "/");
			if ( n >= 0 ) n = put_num(buf, bufsz, n, (tm->tm_year + 1900) % 100, 2, '0');
			break;
		  case 'n': n = put_str(buf, bufsz, n, "\n"); break;
		  case 't': n = put_str(buf, bufsz, n, "\t"); break;
		  case '%': n = put_str(buf, bufsz, n, "%"); break;
		  case '\0': return E_PAR;
		  default:
			/* an unknown conversion is copied through */
			if ( n >= (INT)bufsz - 2 ) return E_PAR;
			buf[n++] = '%';
			buf[n++] = *fmt;
			break;
		}
		if ( n < 0 ) {
			return E_PAR;		/* the buffer is too small */
		}
		fmt++;
	}
	buf[n] = '\0';

	return n;
}

/* Kept for the leap year rule, which the file systems need */
EXPORT BOOL knl_is_leap_year( D y )
{
	return is_leap(y);
}
