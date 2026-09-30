/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_uuid.c
 *	Random source, UUID version 7 and the calendar.
 */

#include "ktest.h"
#include <ts/uuid.h>
#include <ts/dt.h>

/* the generator does not repeat itself and sets every bit over a run */
LOCAL void test_random( void )
{
	UB	a[32], b[32];
	UD	ones = 0, zeros = 0;
	INT	i, same = 0;

	KT_ASSERT_ER(ts_get_random(a, sizeof(a)), E_OK);
	KT_ASSERT_ER(ts_get_random(b, sizeof(b)), E_OK);

	for ( i = 0; i < (INT)sizeof(a); i++ ) {
		if ( a[i] == b[i] ) same++;
		ones  |= a[i];
		zeros |= (UB)~a[i];
	}
	KT_ASSERT(same < 8);			/* two draws are not the same */
	KT_ASSERT_EQ(ones, 0xff);		/* every bit was seen set */
	KT_ASSERT_EQ(zeros, 0xff);		/* and clear */
}

/* version 7 layout, and the order of two UUIDs follows the order of creation */
LOCAL void test_uuid_v7( void )
{
	TS_UUID	u[16];
	char	s[TS_UUID_STRLEN + 1];
	TS_UUID	back;
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		KT_ASSERT_ER(ts_gen_uuid(&u[i]), E_OK);
	}
	KT_ASSERT_EQ(u[0].b[6] & 0xf0, 0x70);		/* version 7 */
	KT_ASSERT_EQ(u[0].b[8] & 0xc0, 0x80);		/* variant 10 */

	for ( i = 1; i < 16; i++ ) {
		KT_ASSERT(ts_uuid_cmp(&u[i - 1], &u[i]) < 0);	/* strictly increasing */
	}

	KT_ASSERT_ER(ts_uuid_to_str(&u[0], s, sizeof(s)), E_OK);
	KT_ASSERT_EQ(s[8], (UB)'-');
	KT_ASSERT_EQ(s[13], (UB)'-');
	KT_ASSERT_EQ(s[14], (UB)'7');			/* the version digit */
	KT_ASSERT_EQ(s[18], (UB)'-');
	KT_ASSERT_EQ(s[23], (UB)'-');
	KT_ASSERT_EQ(s[TS_UUID_STRLEN], 0);
	tm_printf((UB*)"  uuid: %s\n", s);

	KT_ASSERT_ER(ts_str_to_uuid(s, &back), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&u[0], &back), 0);
	KT_ASSERT_ER(ts_str_to_uuid("not a uuid", &back), E_PAR);
	KT_ASSERT_ER(ts_uuid_to_str(&u[0], s, 8), E_PAR);
}

/* the timestamp of a UUID matches the system clock */
LOCAL void test_uuid_time( void )
{
	TS_UUID	u;
	TS_TIME	now;
	UD	ms = 0;
	INT	i;

	KT_ASSERT_ER(dt_gettime(&now), E_OK);
	if ( now < 1000000000LL ) {
		KT_SKIP("the clock has not been set");
	}
	KT_ASSERT_ER(ts_gen_uuid(&u), E_OK);
	for ( i = 0; i < 6; i++ ) {
		ms = (ms << 8) | u.b[i];
	}
	/* the UUID counts from 1970 (RFC 9562), the system time from 1985 */
	now += 473385600LL;
	KT_ASSERT(ms / 1000 >= (UD)now - 2 && ms / 1000 <= (UD)now + 2);
}

/* known dates convert both ways */
LOCAL void test_calendar( void )
{
	TS_TM	tm;
	TS_TIME	t;
	char	buf[64];
	INT	n, tz;

	/* 2026-03-21 12:34:56 UTC is 1300710896 from 1985-01-01 */
	t = 1300710896LL;
	KT_ASSERT_ER(dt_gmtime(&t, &tm), E_OK);
	KT_ASSERT_EQ(tm.tm_year + 1900, 2026);
	KT_ASSERT_EQ(tm.tm_mon + 1, 3);
	KT_ASSERT_EQ(tm.tm_mday, 21);
	KT_ASSERT_EQ(tm.tm_hour, 12);
	KT_ASSERT_EQ(tm.tm_min, 34);
	KT_ASSERT_EQ(tm.tm_sec, 56);

	/* the origin itself: a Tuesday */
	t = 0;
	KT_ASSERT_ER(dt_gmtime(&t, &tm), E_OK);
	KT_ASSERT_EQ(tm.tm_year + 1900, 1985);
	KT_ASSERT_EQ(tm.tm_mon, 0);
	KT_ASSERT_EQ(tm.tm_mday, 1);
	KT_ASSERT_EQ(tm.tm_wday, 2);
	KT_ASSERT_EQ(tm.tm_yday, 0);

	/* before it, negative: 1970-01-01 was a Thursday */
	t = -473385600LL;
	KT_ASSERT_ER(dt_gmtime(&t, &tm), E_OK);
	KT_ASSERT_EQ(tm.tm_year + 1900, 1970);
	KT_ASSERT_EQ(tm.tm_mon, 0);
	KT_ASSERT_EQ(tm.tm_mday, 1);
	KT_ASSERT_EQ(tm.tm_wday, 4);
	KT_ASSERT_ER(dt_settime(-1), E_PAR);

	/* a leap day */
	tm.tm_year = 2024 - 1900; tm.tm_mon = 1; tm.tm_mday = 29;
	tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
	KT_ASSERT_ER(dt_mktime(&tm, &t), E_OK);
	KT_ASSERT_EQ(t, 1235779200LL);
	KT_ASSERT_ER(dt_gmtime(&t, &tm), E_OK);
	KT_ASSERT_EQ(tm.tm_mon + 1, 2);
	KT_ASSERT_EQ(tm.tm_mday, 29);
	KT_ASSERT_EQ(tm.tm_wday, 4);		/* 2024-02-29 was a Thursday */

	/* round trip over a spread of dates */
	{
		TS_TIME	v[] = { -473385599LL, 1LL, 478396800LL, 761182290LL,
				2147483647LL, 3629059200LL };
		TS_TIME	r;
		INT	i;

		for ( i = 0; i < (INT)(sizeof(v) / sizeof(v[0])); i++ ) {
			KT_ASSERT_ER(dt_gmtime(&v[i], &tm), E_OK);
			KT_ASSERT_ER(dt_mktime(&tm, &r), E_OK);
			KT_ASSERT_EQ(r, v[i]);
		}
	}

	/* the time zone shifts local time only */
	KT_ASSERT_ER(dt_getsystz(&tz), E_OK);
	KT_ASSERT_EQ(tz, 0);
	KT_ASSERT_ER(dt_setsystz(9 * 60), E_OK);	/* Japan */
	t = 0;
	KT_ASSERT_ER(dt_localtime(&t, &tm), E_OK);
	KT_ASSERT_EQ(tm.tm_hour, 9);
	KT_ASSERT_ER(dt_mktime_local(&tm, &t), E_OK);
	KT_ASSERT_EQ(t, 0);
	KT_ASSERT_ER(dt_setsystz(15 * 60), E_PAR);
	KT_ASSERT_ER(dt_setsystz(0), E_OK);

	/* formatting */
	t = 1300710896LL;
	KT_ASSERT_ER(dt_gmtime(&t, &tm), E_OK);
	n = dt_strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a", &tm);
	KT_ASSERT_EQ(n, 23);
	tm_printf((UB*)"  formatted: %s\n", buf);
	KT_ASSERT_EQ(buf[0], (UB)'2');
	KT_ASSERT_EQ(buf[4], (UB)'-');
	KT_ASSERT_EQ(buf[10], (UB)' ');
	n = dt_strftime(buf, sizeof(buf), "%F %T", &tm);
	KT_ASSERT_EQ(n, 19);
	KT_ASSERT_ER(dt_strftime(buf, 4, "%Y-%m-%d", &tm), E_PAR);
}

EXPORT void ktest_uuid( void )
{
	KT_RUN(test_random);
	KT_RUN(test_uuid_v7);
	KT_RUN(test_uuid_time);
	KT_RUN(test_calendar);
}
