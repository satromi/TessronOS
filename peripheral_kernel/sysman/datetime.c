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
 *	object (SYSDEF_CALENDAR, tessronos.calendar.tz): setting it is writing
 *	that object's attributes, as its protection lets the caller, and it
 *	is read back from there when a volume comes. Either change is told
 *	on the clock with OB_E_CHANGE; the zone's also on the calendar, as
 *	any change of attributes is.
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

#define SEC_PER_MIN	60
#define SEC_PER_HOUR	3600
#define SEC_PER_DAY	86400

#define TZ_MIN		( -12 * 60 )
#define TZ_MAX		( 14 * 60 )

LOCAL INT	sys_tz = CNF_TIMEZONE;	/* minutes east of UTC, until the calendar says */

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
LOCAL ER zone_store( ID key, INT minutes )
{
	UB	*j = (UB *)Kmalloc(OB_META_MAX);
	UB	v[32];
	SZ	asz = 0;
	INT	n, len;
	ER	er;

	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = ob_get_atr(key, j, OB_META_MAX - 1, &asz);
	if ( er >= E_OK ) {
		n = knl_oj_put(v, 0, sizeof(v), "{\"tz\":");
		n = knl_oj_put_num(v, n, sizeof(v), minutes);
		n = knl_oj_put(v, n, sizeof(v), "}");
		len = knl_oj_set_path(j, (INT)asz, OB_META_MAX, "tessronos", "calendar", v, n);
		er = ( len > 0 ) ? ob_set_atr(key, j, len) : E_LIMIT;
	}
	Kfree(j);
	return er;
}

/* The zone the calendar's metadata says; FALSE when it says none */
LOCAL BOOL zone_read( ID key, INT *p_minutes )
{
	UB	*j = (UB *)Kmalloc(OB_META_MAX);
	SZ	asz = 0;
	D	v = 0;
	INT	p;
	BOOL	got = FALSE;

	if ( j == NULL ) {
		return FALSE;
	}
	if ( ob_get_atr(key, j, OB_META_MAX - 1, &asz) >= E_OK ) {
		p = knl_oj_path(j, (INT)asz, "tessronos", "calendar");
		p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "tz") : -1;
		got = (BOOL)( p >= 0 && knl_oj_num(j, (INT)asz, p, &v) && v >= TZ_MIN && v <= TZ_MAX );
	}
	Kfree(j);
	if ( got ) {
		*p_minutes = (INT)v;
	}
	return got;
}

/*
 * The zone kept in the calendar, taken as the system's: at the start,
 * and whenever a volume comes, since the calendar may be on it.
 */
EXPORT void knl_dt_zone_load( void )
{
	TS_UUID	cal;
	INT	z = 0;
	ID	key;

	if ( ts_str_to_uuid(SYSDEF_CALENDAR, &cal) < E_OK ) {
		return;
	}
	key = ob_opn_obj(&cal, OB_OP_ATRRD);
	if ( key <= 0 ) {
		return;
	}
	if ( zone_read(key, &z) && z != sys_tz ) {
		sys_tz = z;
		clock_told();
	}
	ob_cls_obj(key);
}

/* ---------------------------------------------------------------- zone */

EXPORT ER dt_getsystz( INT *p_minutes )
{
	if ( p_minutes == NULL ) {
		return E_PAR;
	}
	*p_minutes = sys_tz;

	return E_OK;
}

/*
 * The zone set: written into the calendar as the caller may write its
 * attributes. With no calendar to write -- no volume yet -- only an
 * administrator may set it, and only until the calendar says otherwise.
 */
EXPORT ER dt_setsystz( INT minutes )
{
	TS_UUID	cal;
	ID	key;
	ER	er;

	if ( minutes < TZ_MIN || minutes > TZ_MAX ) {
		return E_PAR;
	}
	if ( ts_str_to_uuid(SYSDEF_CALENDAR, &cal) < E_OK ) {
		return E_SYS;
	}
	key = ob_opn_obj(&cal, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key > 0 ) {
		er = zone_store(key, minutes);
		ob_cls_obj(key);
		if ( er < E_OK ) {
			return er;
		}
	} else if ( key != E_NOEXS || !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return ( key == E_NOEXS ) ? E_OACV : (ER)key;
	}
	if ( minutes != sys_tz ) {
		sys_tz = minutes;
		clock_told();
	}
	return E_OK;
}

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
	return to_tm(*t + (TS_TIME)sys_tz * SEC_PER_MIN, tm);
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
		*p_t -= (TS_TIME)sys_tz * SEC_PER_MIN;
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
