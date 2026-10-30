/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tc_db.c
 *	電子手帳: the 予定表, the 住所録 and the 休日 of the object (design 11.23)
 *
 *	The record is read paragraph by paragraph: the letters of a <p> up
 *	to its end or a <br/>, a <tab/> as a tab, the escapes of XML undone
 *	and the line ends of the file itself left out. A <tab-format> ends
 *	the part it is in, all but the first, which begins the 予定表 (what
 *	comes before it is the 予定表's as well). What a link, a
 *	figure or a note holds is passed over. The rows of the 予定表 are
 *	kept in the order of their days, a day's in the order they came; the
 *	people in the order of their よみ.
 *
 *	No system call is made here: the kernel's test uses it as it is.
 */

#include "tc_db.h"

/* ---------------------------------------------------------------- bytes and letters */

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	if ( s == NULL ) return 0;
	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL void b_copy( UB *d, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) d[i] = s[i];
}

LOCAL UB *s_dup( CONST UB *s, INT n )
{
	UB	*p = tc_alloc(n + 1);

	if ( p == NULL ) return NULL;
	b_copy(p, s, n);
	p[n] = 0;
	return p;
}

LOCAL BOOL at_word( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( i >= n || s[i] != (UB)w[i] ) return FALSE;
	}
	return TRUE;
}

/* One letter of UTF-8 at s: its code, and how many bytes it took (*p_len, at least 1) */
LOCAL UW u8_get( CONST UB *s, INT n, INT *p_len )
{
	UB	c = s[0];
	INT	k, i;
	UW	u;

	if ( c < 0x80 || n < 2 ) {
		*p_len = 1;
		return c;
	}
	if ( ( c & 0xE0 ) == 0xC0 ) {
		k = 2;
		u = c & 0x1F;
	} else if ( ( c & 0xF0 ) == 0xE0 ) {
		k = 3;
		u = c & 0x0F;
	} else if ( ( c & 0xF8 ) == 0xF0 ) {
		k = 4;
		u = c & 0x07;
	} else {
		*p_len = 1;
		return c;
	}
	if ( k > n ) {
		*p_len = 1;
		return c;
	}
	for ( i = 1; i < k; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) {
			*p_len = 1;
			return c;
		}
		u = ( u << 6 ) | ( s[i] & 0x3F );
	}
	*p_len = k;
	return u;
}

LOCAL INT u8_put( UB *d, UW u )
{
	if ( u < 0x80 ) {
		d[0] = (UB)u;
		return 1;
	}
	if ( u < 0x800 ) {
		d[0] = (UB)( 0xC0 | ( u >> 6 ) );
		d[1] = (UB)( 0x80 | ( u & 0x3F ) );
		return 2;
	}
	if ( u < 0x10000 ) {
		d[0] = (UB)( 0xE0 | ( u >> 12 ) );
		d[1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
		d[2] = (UB)( 0x80 | ( u & 0x3F ) );
		return 3;
	}
	d[0] = (UB)( 0xF0 | ( u >> 18 ) );
	d[1] = (UB)( 0x80 | ( ( u >> 12 ) & 0x3F ) );
	d[2] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
	d[3] = (UB)( 0x80 | ( u & 0x3F ) );
	return 4;
}

/*
 * A letter as searching and sorting see it: katakana as hiragana, capital
 * letters as small ones, the full width letters and digits as ASCII.
 */
LOCAL UW fold( UW u )
{
	if ( u >= 0x30A1 && u <= 0x30F6 ) return u - 0x60;
	if ( u >= 'A' && u <= 'Z' ) return u + 0x20;
	if ( u >= 0xFF21 && u <= 0xFF3A ) return u - 0xFF21 + 'a';
	if ( u >= 0xFF41 && u <= 0xFF5A ) return u - 0xFF41 + 'a';
	if ( u >= 0xFF10 && u <= 0xFF19 ) return u - 0xFF10 + '0';
	return u;
}

/* The folded letters of s, at most max; how many */
LOCAL INT folded( CONST UB *s, INT n, UW *out, INT max )
{
	INT	i = 0, k = 0, len;

	while ( i < n && s[i] != 0 && k < max ) {
		out[k++] = fold(u8_get(s + i, n - i, &len));
		i += len;
	}
	return k;
}

#define FOLD_MAX	TC_LINE_MAX

EXPORT BOOL tc_match( CONST UB *hay, CONST UB *needle )
{
	UW	*h, *w;
	INT	nh, nw, i, k;
	BOOL	found = FALSE;

	if ( hay == NULL || needle == NULL ) return FALSE;
	/* two folded fields are 4KB: from the heap, since this may run in a
	   task of the kernel, whose stack is small */
	h = (UW *)tc_alloc((INT)( sizeof(UW) * 2 * FOLD_MAX ));
	if ( h == NULL ) return FALSE;
	w = h + FOLD_MAX;
	nw = folded(needle, s_len(needle), w, FOLD_MAX);
	nh = ( nw > 0 ) ? folded(hay, s_len(hay), h, FOLD_MAX) : 0;
	for ( i = 0; nw > 0 && i + nw <= nh && !found; i++ ) {
		for ( k = 0; k < nw && h[i + k] == w[k]; k++ ) ;
		if ( k == nw ) found = TRUE;
	}
	tc_free(h);
	return found;
}

/* Whether two names are the same as searching sees them, the spaces round them left out */
LOCAL BOOL same_name( CONST UB *a, INT na, CONST char *b )
{
	UW	x[64], y[64];
	INT	nx, ny, i;

	while ( na > 0 && ( a[0] == ' ' || a[0] == '\t' ) ) {
		a++;
		na--;
	}
	while ( na > 0 && ( a[na - 1] == ' ' || a[na - 1] == '\t' ) ) na--;
	nx = folded(a, na, x, 64);
	ny = folded((CONST UB *)b, s_len((CONST UB *)b), y, 64);
	if ( nx != ny ) return FALSE;
	for ( i = 0; i < nx; i++ ) {
		if ( x[i] != y[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the store */

EXPORT void tc_init( TCDB *db )
{
	UB	*z = (UB *)db;
	INT	i;

	for ( i = 0; i < (INT)sizeof(*db); i++ ) z[i] = 0;
	{
		static CONST INT mm[4] = { 20, 20, 20, 20 };

		tc_set_paper(db, TC_PAPER_A4, FALSE, mm);
	}
}

LOCAL void addr_free( TCADDR *a )
{
	INT	f;

	if ( a == NULL ) return;
	for ( f = 0; f < TC_NF; f++ ) tc_free(a->f[f]);
	tc_free(a);
}

LOCAL void hol_free( TCDB *db )
{
	INT	i;

	for ( i = 0; i < db->nhl; i++ ) tc_free(db->hl[i]);
	tc_free(db->hl);
	db->hl = NULL;
	db->nhl = 0;
	db->nr = 0;
	db->week = 0;
	db->jp = FALSE;
}

EXPORT void tc_clear( TCDB *db )
{
	INT	i;

	for ( i = 0; i < db->np; i++ ) tc_free(db->p[i].text);
	tc_free(db->p);
	db->p = NULL;
	db->np = db->pmax = 0;
	for ( i = 0; i < db->na; i++ ) addr_free(db->a[i]);
	tc_free(db->a);
	db->a = NULL;
	db->na = db->amax = 0;
	hol_free(db);
	db->dropped = 0;
}

/* Room for one more row of the 予定表 */
LOCAL BOOL plan_room( TCDB *db, INT more )
{
	TCPLAN	*p;
	INT	m, i;

	if ( db->np + more <= db->pmax ) return TRUE;
	m = ( db->pmax < 64 ) ? 64 : db->pmax * 2;
	while ( m < db->np + more ) m *= 2;
	p = tc_alloc(m * (INT)sizeof(TCPLAN));
	if ( p == NULL ) return FALSE;
	for ( i = 0; i < db->np; i++ ) p[i] = db->p[i];
	tc_free(db->p);
	db->p = p;
	db->pmax = m;
	return TRUE;
}

LOCAL BOOL addr_room( TCDB *db )
{
	TCADDR	**a;
	INT	m, i;

	if ( db->na < db->amax ) return TRUE;
	m = ( db->amax < 32 ) ? 32 : db->amax * 2;
	a = tc_alloc(m * (INT)sizeof(TCADDR *));
	if ( a == NULL ) return FALSE;
	for ( i = 0; i < db->na; i++ ) a[i] = db->a[i];
	tc_free(db->a);
	db->a = a;
	db->amax = m;
	return TRUE;
}

/* ---------------------------------------------------------------- days */

EXPORT INT tc_day( INT y, INT m, INT d )
{
	return y * 10000 + m * 100 + d;
}

EXPORT void tc_day_split( INT day, INT *p_y, INT *p_m, INT *p_d )
{
	*p_y = day / 10000;
	*p_m = day / 100 % 100;
	*p_d = day % 100;
}

EXPORT BOOL tc_day_ok( INT day )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	return (BOOL)( y >= TC_Y_FIRST && y <= TC_Y_LAST && m >= 1 && m <= 12 && d >= 1 && d <= ct_days(y, m) );
}

EXPORT INT tc_day_add( INT day, INT n )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	while ( n > 0 ) {
		if ( ++d > ct_days(y, m) ) {
			d = 1;
			if ( ++m > 12 ) {
				m = 1;
				y++;
			}
		}
		n--;
	}
	while ( n < 0 ) {
		if ( --d < 1 ) {
			if ( --m < 1 ) {
				m = 12;
				y--;
			}
			d = ct_days(y, m);
		}
		n++;
	}
	return tc_day(y, m, d);
}

EXPORT INT tc_era( INT y, INT m, INT d, UB *out, INT max )
{
	static CONST struct {
		INT		from;
		CONST char	*name;
	} era[] = {
		{ 20190501, "令和" }, { 19890108, "平成" }, { 19261225, "昭和" },
		{ 19120730, "大正" }, { 18681023, "明治" }
	};
	INT	day = tc_day(y, m, d), i, n, k, v;
	UB	num[12];

	if ( max > 0 ) out[0] = 0;
	for ( i = 0; i < 5 && day < era[i].from; i++ ) ;
	if ( i == 5 || max < 16 ) return 0;
	n = s_len((CONST UB *)era[i].name);
	b_copy(out, (CONST UB *)era[i].name, n);
	v = y - era[i].from / 10000 + 1;
	if ( v == 1 ) {
		b_copy(out + n, (CONST UB *)"元", 3);
		n += 3;
	} else {
		k = 0;
		while ( v > 0 && k < 10 ) {
			num[k++] = (UB)( '0' + v % 10 );
			v /= 10;
		}
		while ( k > 0 ) out[n++] = num[--k];
	}
	b_copy(out + n, (CONST UB *)"年", 3);
	n += 3;
	out[n] = 0;
	return n;
}

/* Digits at s: the number and how many bytes, 0 when there are none */
LOCAL INT digits_at( CONST UB *s, INT n, INT *p_v )
{
	INT	i = 0, v = 0;

	while ( i < n && i < 9 && s[i] >= '0' && s[i] <= '9' ) v = v * 10 + ( s[i++] - '0' );
	*p_v = v;
	return i;
}

EXPORT INT tc_parse_day( CONST UB *s, INT n, INT *p_day )
{
	INT	i = 0, k, y, m, d, day;

	k = digits_at(s, n, &y);
	if ( k == 0 ) return 0;
	i += k;
	if ( i >= n || s[i++] != '/' ) return 0;
	k = digits_at(s + i, n - i, &m);
	if ( k == 0 ) return 0;
	i += k;
	if ( i >= n || s[i++] != '/' ) return 0;
	k = digits_at(s + i, n - i, &d);
	if ( k == 0 ) return 0;
	i += k;
	if ( m < 1 || m > 12 || d < 1 || d > ct_days(y, m) ) return 0;
	day = tc_day(y, m, d);
	*p_day = day;
	return i;
}

/* ---------------------------------------------------------------- 休日 */

LOCAL CONST char *CONST wd_name[7] = { "日", "月", "火", "水", "木", "金", "土" };

/* The weekday a letter at s names (3 bytes), -1 none */
LOCAL INT wday_at( CONST UB *s, INT n )
{
	INT	w;

	for ( w = 0; w < 7; w++ ) {
		if ( at_word(s, n, wd_name[w]) ) return w;
	}
	return -1;
}

/* A line of weekdays only ("土日"): their bits, 0 when it is not one */
LOCAL UINT week_line( CONST UB *s, INT n )
{
	UINT	bits = 0;
	INT	i = 0, w;

	while ( i < n ) {
		if ( s[i] == ' ' ) {
			i++;
			continue;
		}
		w = wday_at(s + i, n - i);
		if ( w < 0 ) return 0;
		bits |= 1U << w;
		i += 3;
	}
	return bits;
}

/* "Y/M/D", "*", "Y0-Y1" and "nW" as a rule; the rest of the line after a space or a tab is a note */
LOCAL BOOL rule_line( CONST UB *s, INT n, TCRULE *r )
{
	INT	i = 0, k, v;

	r->y0 = r->y1 = r->m = r->d = r->nth = r->wday = 0;
	while ( i < n && s[i] == ' ' ) i++;
	if ( i < n && s[i] == '*' ) {
		i++;
	} else {
		k = digits_at(s + i, n - i, &v);
		if ( k == 0 ) return FALSE;
		i += k;
		r->y0 = r->y1 = v;
		if ( i < n && s[i] == '-' ) {
			i++;
			k = digits_at(s + i, n - i, &v);
			if ( k == 0 || v < r->y0 ) return FALSE;
			i += k;
			r->y1 = v;
		}
	}
	if ( i >= n || s[i++] != '/' ) return FALSE;
	if ( i < n && s[i] == '*' ) {
		i++;
	} else {
		k = digits_at(s + i, n - i, &v);
		if ( k == 0 || v < 1 || v > 12 ) return FALSE;
		i += k;
		r->m = v;
	}
	if ( i >= n || s[i++] != '/' ) return FALSE;
	if ( i < n && s[i] == '*' ) {
		i++;
	} else {
		k = digits_at(s + i, n - i, &v);
		if ( k == 0 ) return FALSE;
		i += k;
		r->wday = wday_at(s + i, n - i);
		if ( r->wday >= 0 ) {
			if ( v < 1 || v > 5 ) return FALSE;
			r->nth = v;
			i += 3;
		} else {
			if ( v < 1 || v > 31 ) return FALSE;
			r->wday = 0;
			r->d = v;
		}
	}
	return (BOOL)( i == n || s[i] == ' ' || s[i] == '\t' );
}

/* The rules read again from the lines */
LOCAL void rules_read( TCDB *db )
{
	INT	i;

	db->week = 0;
	db->nr = 0;
	db->jp = FALSE;
	for ( i = 0; i < db->nhl; i++ ) {
		CONST UB	*s = db->hl[i];
		INT		n = s_len(s), e, a = 0;
		UINT		w;

		for ( e = 0; e < n && s[e] != '\t'; e++ ) ;
		while ( a < e && s[a] == ' ' ) a++;
		while ( e > a && s[e - 1] == ' ' ) e--;
		w = week_line(s, e);
		if ( e - a == 6 && at_word(s + a, 6, "祝日") ) {
			db->jp = TRUE;
		} else if ( w != 0 ) {
			db->week |= w;
		} else if ( db->nr < TC_RULE_MAX && rule_line(s, n, &db->r[db->nr]) ) {
			db->nr++;
		}
	}
}

LOCAL ER hol_add( TCDB *db, CONST UB *s, INT n )
{
	UB	**h;
	INT	i;

	if ( db->nhl >= TC_HOLLINE_MAX ) return E_LIMIT;
	if ( ( db->nhl & 31 ) == 0 ) {
		h = tc_alloc(( db->nhl + 32 ) * (INT)sizeof(UB *));
		if ( h == NULL ) return E_NOMEM;
		for ( i = 0; i < db->nhl; i++ ) h[i] = db->hl[i];
		tc_free(db->hl);
		db->hl = h;
	}
	db->hl[db->nhl] = s_dup(s, n);
	if ( db->hl[db->nhl] == NULL ) return E_NOMEM;
	db->nhl++;
	return E_OK;
}

EXPORT ER tc_set_holidays( TCDB *db, CONST UB *CONST *lines, INT n )
{
	INT	i;
	ER	er = E_OK;

	hol_free(db);
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		if ( lines[i] != NULL && lines[i][0] != 0 ) er = hol_add(db, lines[i], s_len(lines[i]));
	}
	rules_read(db);
	return er;
}

EXPORT BOOL tc_holiday( CONST TCDB *db, INT y, INT m, INT d )
{
	INT	i, wd = ct_wday(y, m, d);

	if ( ( db->week & ( 1U << wd ) ) != 0 ) return TRUE;
	if ( db->jp && tc_jp_holiday(y, m, d) != NULL ) return TRUE;
	for ( i = 0; i < db->nr; i++ ) {
		CONST TCRULE	*r = &db->r[i];

		if ( r->y0 != 0 && ( y < r->y0 || y > r->y1 ) ) continue;
		if ( r->m != 0 && m != r->m ) continue;
		if ( r->nth != 0 ) {
			if ( wd == r->wday && ( d - 1 ) / 7 + 1 == r->nth ) return TRUE;
			continue;
		}
		if ( r->d == 0 || r->d == d ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- 国民の祝日 */

/* The day of the n-th weekday w of a month */
LOCAL INT nth_wday( INT y, INT m, INT n, INT w )
{
	return 1 + ( w - ct_wday(y, m, 1) + 7 ) % 7 + 7 * ( n - 1 );
}

/*
 * The day of the vernal (spring) or autumnal equinox in a year, by the
 * reckoning the almanacs use: a mean day that moves 0.242194 of a day a
 * year, put back a day every leap year. The mean days are those of the
 * reckoning for 1900-1979, 1980-2099 and 2100-2150, kept in millionths
 * of a day so that no floating point is needed.
 */
LOCAL INT equinox( INT y, BOOL spring )
{
	INT	base, ref = 1983;
	INT	k = y - 1980;

	if ( y < 1980 ) {
		base = spring ? 20835700 : 23258800;
	} else if ( y <= 2099 ) {
		base = spring ? 20843100 : 23248800;
		ref = 1980;
	} else {
		base = spring ? 21851000 : 24248800;
		ref = 1980;
	}
	return ( base + 242194 * k ) / 1000000 - ( y - ref ) / 4;
}

/* The holidays the law names, by their rules and the days that came once; NULL for none */
LOCAL CONST char *jp_named( INT y, INT m, INT d )
{
	static CONST struct {
		INT		day;
		CONST char	*name;
	} once[] = {
		{ 19590410, "皇太子明仁親王の結婚の儀" }, { 19890224, "昭和天皇の大喪の礼" },
		{ 19901112, "即位礼正殿の儀" }, { 19930609, "皇太子徳仁親王の結婚の儀" },
		{ 20190501, "天皇の即位の日" }, { 20191022, "即位礼正殿の儀" }
	};
	INT	day = tc_day(y, m, d), i;

	if ( day < 19490101 ) return NULL;		/* before the law */
	for ( i = 0; i < (INT)( sizeof(once) / sizeof(once[0]) ); i++ ) {
		if ( once[i].day == day ) return once[i].name;
	}
	switch ( m ) {
	case 1:
		if ( d == 1 ) return "元日";
		if ( y <= 1999 ? d == 15 : d == nth_wday(y, 1, 2, 1) ) return "成人の日";
		break;
	case 2:
		if ( d == 11 && y >= 1967 ) return "建国記念の日";
		if ( d == 23 && y >= 2020 ) return "天皇誕生日";
		break;
	case 3:
		if ( d == equinox(y, TRUE) ) return "春分の日";
		break;
	case 4:
		if ( d == 29 ) return ( y <= 1988 ) ? "天皇誕生日" : ( y <= 2006 ) ? "みどりの日" : "昭和の日";
		break;
	case 5:
		if ( d == 3 ) return "憲法記念日";
		if ( d == 4 && y >= 2007 ) return "みどりの日";
		if ( d == 5 ) return "こどもの日";
		break;
	case 7:
		if ( y >= 1996 && y <= 2002 && d == 20 ) return "海の日";
		if ( ( ( y >= 2003 && y <= 2019 ) || y >= 2022 ) && d == nth_wday(y, 7, 3, 1) ) return "海の日";
		if ( ( y == 2020 && d == 23 ) || ( y == 2021 && d == 22 ) ) return "海の日";
		if ( ( y == 2020 && d == 24 ) || ( y == 2021 && d == 23 ) ) return "スポーツの日";
		break;
	case 8:
		if ( ( ( y >= 2016 && y <= 2019 ) || y >= 2022 ) && d == 11 ) return "山の日";
		if ( ( y == 2020 && d == 10 ) || ( y == 2021 && d == 8 ) ) return "山の日";
		break;
	case 9:
		if ( y >= 1966 && y <= 2002 && d == 15 ) return "敬老の日";
		if ( y >= 2003 && d == nth_wday(y, 9, 3, 1) ) return "敬老の日";
		if ( y >= 1948 && d == equinox(y, FALSE) ) return "秋分の日";
		break;
	case 10:
		if ( y >= 1966 && y <= 1999 && d == 10 ) return "体育の日";
		if ( y >= 2000 && y <= 2019 && d == nth_wday(y, 10, 2, 1) ) return "体育の日";
		if ( y >= 2022 && d == nth_wday(y, 10, 2, 1) ) return "スポーツの日";
		break;
	case 11:
		if ( d == 3 ) return "文化の日";
		if ( d == 23 ) return "勤労感謝の日";
		break;
	case 12:
		if ( d == 23 && y >= 1989 && y <= 2018 ) return "天皇誕生日";
		break;
	default:
		break;
	}
	return NULL;
}

LOCAL CONST char *jp_named_day( INT day )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	return jp_named(y, m, d);
}

LOCAL INT wday_of( INT day )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	return ct_wday(y, m, d);
}

/*
 * 振替休日: a holiday on a Sunday makes the next day that is no holiday
 * one -- from 2007 on the first after it that is none, before then only
 * the Monday, when it is none (from 1973/4/12).
 */
LOCAL BOOL jp_substitute( INT day )
{
	INT	p = tc_day_add(day, -1);

	if ( day < 19730412 || jp_named_day(day) != NULL ) return FALSE;
	if ( wday_of(p) == 0 && jp_named_day(p) != NULL ) return TRUE;
	if ( day < 20070101 ) return FALSE;
	while ( jp_named_day(p) != NULL ) {
		if ( wday_of(p) == 0 ) return (BOOL)( p >= 20070101 );
		p = tc_day_add(p, -1);
	}
	return FALSE;
}

EXPORT CONST char *tc_jp_holiday( INT y, INT m, INT d )
{
	INT		day = tc_day(y, m, d);
	CONST char	*n = jp_named(y, m, d);

	if ( n != NULL ) return n;
	if ( jp_substitute(day) ) return "振替休日";
	/* 国民の休日: a day that is none, between two that are (from 1985/12/27), not a Sunday */
	if ( day >= 19851227 && ct_wday(y, m, d) != 0
	  && jp_named_day(tc_day_add(day, -1)) != NULL && jp_named_day(tc_day_add(day, 1)) != NULL ) {
		return "国民の休日";
	}
	return NULL;
}

/* ---------------------------------------------------------------- 予定表 */

/* The first row of a day on or after day */
LOCAL INT plan_at( CONST TCDB *db, INT day )
{
	INT	lo = 0, hi = db->np;

	while ( lo < hi ) {
		INT	mid = ( lo + hi ) / 2;

		if ( db->p[mid].day < day ) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

EXPORT INT tc_plans( CONST TCDB *db, INT day, CONST UB **out, INT max )
{
	INT	i = plan_at(db, day), n = 0;

	for ( ; i < db->np && db->p[i].day == day; i++ ) {
		if ( n < max ) out[n] = db->p[i].text;
		n++;
	}
	return n;
}

/* Rows put in at 'at', the day's own place; the text is copied */
LOCAL ER plan_put( TCDB *db, INT at, INT day, CONST UB *s, INT n )
{
	INT	i;
	UB	*t;

	if ( !plan_room(db, 1) ) return E_NOMEM;
	t = s_dup(s, n);
	if ( t == NULL ) return E_NOMEM;
	for ( i = db->np; i > at; i-- ) db->p[i] = db->p[i - 1];
	db->p[at].day = day;
	db->p[at].text = t;
	db->np++;
	return E_OK;
}

EXPORT ER tc_set_plans( TCDB *db, INT day, CONST UB *CONST *rows, INT n )
{
	INT	a = plan_at(db, day), e = a, i, last = -1, k;
	ER	er = E_OK;

	while ( e < db->np && db->p[e].day == day ) e++;
	for ( i = a; i < e; i++ ) tc_free(db->p[i].text);
	for ( i = e; i < db->np; i++ ) db->p[a + i - e] = db->p[i];
	db->np -= e - a;
	for ( i = 0; i < n; i++ ) {
		if ( rows[i] != NULL && rows[i][0] != 0 ) last = i;
	}
	for ( i = 0, k = a; i <= last && er >= E_OK; i++, k++ ) {
		CONST UB	*r = ( rows[i] != NULL ) ? rows[i] : (CONST UB *)"";

		er = plan_put(db, k, day, r, s_len(r));
	}
	return er;
}

EXPORT INT tc_next_planned( CONST TCDB *db, INT after )
{
	INT	i;

	for ( i = plan_at(db, after + 1); i < db->np; i++ ) {
		if ( db->p[i].text[0] != 0 && tc_day_ok(db->p[i].day) ) return db->p[i].day;
	}
	return 0;
}

/* A row of the record: "西暦/月/日" and the plan after a tab */
LOCAL ER plan_line( TCDB *db, CONST UB *s, INT n )
{
	INT	day = 0, k = tc_parse_day(s, n, &day);

	if ( k == 0 || ( k < n && s[k] != '\t' ) ) {
		db->dropped++;
		return E_OK;
	}
	if ( k < n ) k++;
	/* after the last of the same day: the rows keep the order they came in */
	return plan_put(db, plan_at(db, day + 1), day, s + k, n - k);
}

/* ---------------------------------------------------------------- 住所録 */

EXPORT CONST INT tc_show_order[TC_NF] = {
	TC_F_ZIP, TC_F_YOMI, TC_F_ADDR1, TC_F_ADDR2, TC_F_ADDR3,
	TC_F_NAME, TC_F_TEL, TC_F_FAX, TC_F_MEMO1, TC_F_MEMO2
};

EXPORT CONST char *CONST tc_field_name[TC_NF] = {
	"よみ", "〒", "住所1", "住所2", "住所3", "氏名", "電話", "FAX", "メモ1", "メモ2"
};

LOCAL CONST char *CONST idx_name[TC_IDX_N] = {
	"あ", "か", "さ", "た", "な", "は", "ま", "や", "ら", "わ", "他"
};

EXPORT CONST char *tc_index_name( INT idx )
{
	return ( idx >= 0 && idx < TC_IDX_N ) ? idx_name[idx] : idx_name[TC_IDX_OTHER];
}

EXPORT INT tc_index_of( CONST UB *yomi )
{
	static CONST UW	top[TC_IDX_OTHER] = {
		0x3041, 0x304B, 0x3055, 0x305F, 0x306A, 0x306F, 0x307E, 0x3083, 0x3089, 0x308E
	};
	INT	len, i;
	UW	u;

	if ( yomi == NULL || yomi[0] == 0 ) return TC_IDX_OTHER;
	u = fold(u8_get(yomi, s_len(yomi), &len));
	if ( u == 0x3094 ) return 0;			/* ゔ */
	if ( u == 0x3095 || u == 0x3096 ) return 1;	/* ゕ ゖ */
	if ( u < 0x3041 || u > 0x3093 ) return TC_IDX_OTHER;
	for ( i = TC_IDX_OTHER - 1; i > 0 && u < top[i]; i-- ) ;
	return i;
}

/* The order of two people: by index, then by the letters of よみ */
LOCAL INT addr_cmp( CONST TCADDR *a, CONST TCADDR *b )
{
	CONST UB	*x = ( a->f[TC_F_YOMI] != NULL ) ? a->f[TC_F_YOMI] : (CONST UB *)"";
	CONST UB	*y = ( b->f[TC_F_YOMI] != NULL ) ? b->f[TC_F_YOMI] : (CONST UB *)"";
	INT		nx = s_len(x), ny = s_len(y), i = 0, k = 0, lx, ly;

	if ( a->idx != b->idx ) return a->idx - b->idx;
	while ( i < nx && k < ny ) {
		UW	u = fold(u8_get(x + i, nx - i, &lx)), v = fold(u8_get(y + k, ny - k, &ly));

		if ( u != v ) return ( u < v ) ? -1 : 1;
		i += lx;
		k += ly;
	}
	return ( i < nx ) ? 1 : ( k < ny ) ? -1 : 0;
}

/* Where a person goes: after every one who is not after it */
LOCAL INT addr_place( CONST TCDB *db, CONST TCADDR *x )
{
	INT	lo = 0, hi = db->na;

	while ( lo < hi ) {
		INT	mid = ( lo + hi ) / 2;

		if ( addr_cmp(db->a[mid], x) <= 0 ) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

LOCAL void addr_insert( TCDB *db, INT at, TCADDR *x )
{
	INT	i;

	for ( i = db->na; i > at; i-- ) db->a[i] = db->a[i - 1];
	db->a[at] = x;
	db->na++;
}

LOCAL TCADDR *addr_take( TCDB *db, INT at )
{
	TCADDR	*x = db->a[at];
	INT	i;

	for ( i = at; i < db->na - 1; i++ ) db->a[i] = db->a[i + 1];
	db->na--;
	return x;
}

LOCAL TCADDR *addr_new( void )
{
	TCADDR	*x = tc_alloc((INT)sizeof(TCADDR));
	INT	f;

	if ( x == NULL ) return NULL;
	for ( f = 0; f < TC_NF; f++ ) x->f[f] = NULL;
	x->idx = TC_IDX_OTHER;
	return x;
}

EXPORT INT tc_addr_add( TCDB *db )
{
	TCADDR	*x;
	INT	at;

	if ( !addr_room(db) || ( x = addr_new() ) == NULL ) return -1;
	at = addr_place(db, x);
	addr_insert(db, at, x);
	return at;
}

EXPORT INT tc_addr_set( TCDB *db, INT i, INT f, CONST UB *text )
{
	TCADDR	*x;
	UB	*t = NULL;
	INT	n = s_len(text);

	if ( i < 0 || i >= db->na || f < 0 || f >= TC_NF ) return i;
	x = db->a[i];
	if ( n > 0 && ( t = s_dup(text, n) ) == NULL ) return i;
	tc_free(x->f[f]);
	x->f[f] = t;
	if ( f != TC_F_YOMI ) return i;
	(void)addr_take(db, i);
	x->idx = tc_index_of(x->f[TC_F_YOMI]);
	i = addr_place(db, x);
	addr_insert(db, i, x);
	return i;
}

EXPORT void tc_addr_del( TCDB *db, INT i )
{
	INT	f;

	if ( i < 0 || i >= db->na ) return;
	if ( db->na == 1 ) {
		for ( f = 0; f < TC_NF; f++ ) {
			tc_free(db->a[0]->f[f]);
			db->a[0]->f[f] = NULL;
		}
		db->a[0]->idx = TC_IDX_OTHER;
		return;
	}
	addr_free(addr_take(db, i));
}

EXPORT CONST UB *tc_addr_get( CONST TCDB *db, INT i, INT f )
{
	if ( i < 0 || i >= db->na || f < 0 || f >= TC_NF || db->a[i]->f[f] == NULL ) return (CONST UB *)"";
	return db->a[i]->f[f];
}

EXPORT INT tc_index_first( CONST TCDB *db, INT idx )
{
	INT	i;

	for ( i = 0; i < db->na; i++ ) {
		if ( db->a[i]->idx == idx ) return i;
	}
	return -1;
}

EXPORT void tc_index_pos( CONST TCDB *db, INT i, INT *p_pos, INT *p_count )
{
	INT	k, idx;

	*p_pos = *p_count = 0;
	if ( i < 0 || i >= db->na ) return;
	idx = db->a[i]->idx;
	for ( k = 0; k < db->na; k++ ) {
		if ( db->a[k]->idx != idx ) continue;
		( *p_count )++;
		if ( k <= i ) ( *p_pos )++;
	}
}

LOCAL INT num_text( INT v, UB *out )
{
	UB	t[12];
	INT	k = 0, n = 0;

	if ( v < 0 ) v = 0;
	do {
		t[k++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 ) out[n++] = t[--k];
	return n;
}

EXPORT void tc_pos_text( INT pos, INT count, UB *out, INT max )
{
	INT	n = 0;

	if ( max < 24 ) {
		if ( max > 0 ) out[0] = 0;
		return;
	}
	if ( count >= 1000 ) {
		b_copy(out, (CONST UB *)"***", 4);
		return;
	}
	n += num_text(pos, out + n);
	out[n++] = '/';
	n += num_text(count, out + n);
	out[n] = 0;
}

/* A row of the record: the fields of a person cut by tabs */
LOCAL ER addr_line( TCDB *db, CONST UB *s, INT n )
{
	TCADDR	*x;
	INT	f = 0, i = 0, a = 0;

	if ( !addr_room(db) || ( x = addr_new() ) == NULL ) return E_NOMEM;
	for ( ; i <= n; i++ ) {
		if ( i < n && s[i] != '\t' ) continue;
		if ( f < TC_NF && i > a && ( x->f[f] = s_dup(s + a, i - a) ) == NULL ) {
			addr_free(x);
			return E_NOMEM;
		}
		f++;
		a = i + 1;
	}
	x->idx = tc_index_of(x->f[TC_F_YOMI]);
	db->a[db->na++] = x;
	return E_OK;
}

/* The people put in order, those of the same よみ as they came (a merge sort) */
LOCAL ER addr_sort( TCDB *db )
{
	TCADDR	**t;
	INT	w, i;

	if ( db->na < 2 ) return E_OK;
	t = tc_alloc(db->na * (INT)sizeof(TCADDR *));
	if ( t == NULL ) return E_NOMEM;
	for ( w = 1; w < db->na; w *= 2 ) {
		for ( i = 0; i < db->na; i += 2 * w ) {
			INT	a = i, m = ( i + w < db->na ) ? i + w : db->na;
			INT	e = ( i + 2 * w < db->na ) ? i + 2 * w : db->na, b = m, k = i;

			while ( a < m && b < e ) t[k++] = ( addr_cmp(db->a[b], db->a[a]) < 0 ) ? db->a[b++] : db->a[a++];
			while ( a < m ) t[k++] = db->a[a++];
			while ( b < e ) t[k++] = db->a[b++];
		}
		for ( i = 0; i < db->na; i++ ) db->a[i] = t[i];
	}
	tc_free(t);
	return E_OK;
}

/* ---------------------------------------------------------------- 検索 */

EXPORT INT tc_find_plan( CONST TCDB *db, INT after, CONST UB *needle )
{
	INT	i;

	for ( i = plan_at(db, after + 1); i < db->np; i++ ) {
		if ( tc_day_ok(db->p[i].day) && tc_match(db->p[i].text, needle) ) return db->p[i].day;
	}
	return 0;
}

/* The fields a name before a colon says, as bits; 0 when it names none */
LOCAL UINT field_bits( CONST UB *s, INT n )
{
	static CONST struct {
		CONST char	*name;
		UINT		bits;
	} names[] = {
		{ "〒", 1U << TC_F_ZIP }, { "郵便番号", 1U << TC_F_ZIP }, { "よみ", 1U << TC_F_YOMI },
		{ "住所1", 1U << TC_F_ADDR1 }, { "住所2", 1U << TC_F_ADDR2 }, { "住所3", 1U << TC_F_ADDR3 },
		{ "住所", ( 1U << TC_F_ADDR1 ) | ( 1U << TC_F_ADDR2 ) | ( 1U << TC_F_ADDR3 ) },
		{ "氏名", 1U << TC_F_NAME }, { "電話", 1U << TC_F_TEL }, { "FAX", 1U << TC_F_FAX },
		{ "メモ1", 1U << TC_F_MEMO1 }, { "メモ2", 1U << TC_F_MEMO2 },
		{ "メモ", ( 1U << TC_F_MEMO1 ) | ( 1U << TC_F_MEMO2 ) }
	};
	INT	i;

	for ( i = 0; i < (INT)( sizeof(names) / sizeof(names[0]) ); i++ ) {
		if ( same_name(s, n, names[i].name) ) return names[i].bits;
	}
	return 0;
}

EXPORT INT tc_find_addr( CONST TCDB *db, INT from, CONST UB *needle )
{
	UINT		bits = ( 1U << TC_NF ) - 1;
	CONST UB	*w = needle;
	INT		n = s_len(needle), i, f;

	for ( i = 0; i < n; i++ ) {
		INT	at = -1, skip = 0;

		if ( needle[i] == ':' ) {
			at = i;
			skip = 1;
		} else if ( at_word(needle + i, n - i, "：") ) {
			at = i;
			skip = 3;
		}
		if ( at < 0 ) continue;
		if ( field_bits(needle, at) != 0 ) {
			bits = field_bits(needle, at);
			w = needle + at + skip;
		}
		break;
	}
	if ( from < 0 ) from = 0;
	for ( i = from; i < db->na; i++ ) {
		for ( f = 0; f < TC_NF; f++ ) {
			if ( ( bits & ( 1U << f ) ) != 0 && tc_match(db->a[i]->f[f], w) ) return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- reading */

/* The paragraph being read */
typedef struct {
	UB	s[TC_LINE_MAX * TC_NF];
	INT	n;
	BOOL	full;
} TCPARA;

typedef struct {
	TCDB	*db;
	INT	part;			/* 0 予定表, 1 住所録, 2 休日, 3 after them */
	BOOL	seen;			/* a tab format has come: the next one ends the part */
	ER	er;
	TCPARA	*p;
} TCRD;

LOCAL void para_add( TCPARA *p, CONST UB *s, INT n )
{
	if ( p->n + n >= (INT)sizeof(p->s) ) {
		p->full = TRUE;
		return;
	}
	b_copy(p->s + p->n, s, n);
	p->n += n;
}

/* A paragraph ended: given to the part it is in */
LOCAL void para_end( TCRD *r )
{
	TCPARA	*p = r->p;
	INT	n = p->n;

	p->n = 0;
	p->full = FALSE;
	if ( n == 0 || r->er < E_OK ) return;
	switch ( r->part ) {
	case 0:
		r->er = plan_line(r->db, p->s, n);
		break;
	case 1:
		r->er = addr_line(r->db, p->s, n);
		break;
	case 2:
		r->er = hol_add(r->db, p->s, n);
		break;
	default:
		r->db->dropped++;
		break;
	}
}

/* An element's name at s (after '<' or "</"): its length */
LOCAL INT name_len( CONST UB *s, INT n )
{
	INT	i = 0;

	while ( i < n && ( ( s[i] >= 'a' && s[i] <= 'z' ) || ( s[i] >= 'A' && s[i] <= 'Z' )
			|| ( s[i] >= '0' && s[i] <= '9' ) || s[i] == '-' || s[i] == '_' || s[i] == ':' ) ) i++;
	return i;
}

/* The end of a tag that begins at i: the place after its '>' */
LOCAL INT tag_end( CONST UB *x, INT n, INT i )
{
	UB	q = 0;

	for ( ; i < n; i++ ) {
		if ( q != 0 ) {
			if ( x[i] == q ) q = 0;
		} else if ( x[i] == '"' || x[i] == '\'' ) {
			q = x[i];
		} else if ( x[i] == '>' ) {
			return i + 1;
		}
	}
	return n;
}

LOCAL BOOL name_is( CONST UB *s, INT n, CONST char *w )
{
	INT	i;

	for ( i = 0; i < n && w[i] != 0; i++ ) {
		if ( s[i] != (UB)w[i] ) return FALSE;
	}
	return (BOOL)( i == n && w[i] == 0 );
}

/* Elements whose content is not the text's letters */
LOCAL BOOL passed_over( CONST UB *s, INT n )
{
	static CONST char *CONST skip[] = {
		"figure", "paper-overlay-define", "link", "image", "docmemo", "ruby", "fusen", "memo"
	};
	INT	i;

	for ( i = 0; i < (INT)( sizeof(skip) / sizeof(skip[0]) ); i++ ) {
		if ( name_is(s, n, skip[i]) ) return TRUE;
	}
	return FALSE;
}

/* From e, after the start tag of an element named nm, to the place after its own end tag */
LOCAL INT skip_to_end( CONST UB *x, INT n, INT e, CONST UB *nm, INT nl )
{
	INT	level = 1, k = e;

	while ( k < n && level > 0 ) {
		if ( x[k] == '<' ) {
			BOOL		ke = (BOOL)( k + 1 < n && x[k + 1] == '/' );
			CONST UB	*kn = x + k + ( ke ? 2 : 1 );
			INT		kl = name_len(kn, n - k - ( ke ? 2 : 1 ));
			INT		ee = tag_end(x, n, k);

			if ( kl == nl ) {
				INT	j;

				for ( j = 0; j < nl && kn[j] == nm[j]; j++ ) ;
				if ( j == nl ) {
					if ( ke ) level--;
					else if ( !( ee >= 2 && x[ee - 2] == '/' ) ) level++;
				}
			}
			k = ee;
		} else {
			k++;
		}
	}
	return k;
}

/* An escape of XML at s ('&'): the letter in out, how many bytes it took (0: not one) */
LOCAL INT escape_at( CONST UB *s, INT n, UB *out, INT *p_olen )
{
	static CONST struct {
		CONST char	*w;
		UB		c;
	} e[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
	INT	i;

	for ( i = 0; i < 5; i++ ) {
		if ( at_word(s, n, e[i].w) ) {
			out[0] = e[i].c;
			*p_olen = 1;
			return s_len((CONST UB *)e[i].w);
		}
	}
	if ( n > 3 && s[1] == '#' ) {
		UW	u = 0;
		INT	k = 2;
		BOOL	hex = (BOOL)( s[2] == 'x' || s[2] == 'X' );

		if ( hex ) k++;
		for ( ; k < n && k < 12 && s[k] != ';'; k++ ) {
			UB	c = s[k];
			INT	v = ( c >= '0' && c <= '9' ) ? c - '0' : ( hex && c >= 'a' && c <= 'f' ) ? c - 'a' + 10
				  : ( hex && c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;

			if ( v < 0 ) return 0;
			u = u * ( hex ? 16 : 10 ) + (UW)v;
		}
		if ( k < n && s[k] == ';' && u > 0 && u < 0x110000 ) {
			*p_olen = u8_put(out, u);
			return k + 1;
		}
	}
	return 0;
}

LOCAL void paper_read( TCDB *db, CONST UB *el, INT n )
{
	INT	w = ct_attr_int(el, n, "width", 0), h = ct_attr_int(el, n, "length", 0);

	if ( w < 100 || h < 100 ) return;
	db->pw = w;
	db->ph = h;
	db->mt = ct_attr_int(el, n, "top", db->mt);
	db->mb = ct_attr_int(el, n, "bottom", db->mb);
	db->ml = ct_attr_int(el, n, "left", db->ml);
	db->mr = ct_attr_int(el, n, "right", db->mr);
}

EXPORT ER tc_read( TCDB *db, CONST UB *x, INT n )
{
	TCRD	r;
	INT	i = 0, depth = 0;		/* inside a <p>, how deep */
	BOOL	in_doc = FALSE;

	tc_clear(db);
	r.db = db;
	r.part = 0;
	r.seen = FALSE;
	r.er = E_OK;
	r.p = tc_alloc((INT)sizeof(TCPARA));
	if ( r.p == NULL ) return E_NOMEM;
	r.p->n = 0;
	r.p->full = FALSE;
	while ( i < n && r.er >= E_OK ) {
		if ( x[i] == '<' ) {
			INT	e, nl;
			BOOL	end = FALSE, empty;

			if ( at_word(x + i, n - i, "<!--") ) {
				for ( e = i + 4; e + 2 < n && !at_word(x + e, n - e, "-->"); e++ ) ;
				i = ( e + 2 < n ) ? e + 3 : n;
				continue;
			}
			if ( i + 1 < n && ( x[i + 1] == '?' || x[i + 1] == '!' ) ) {
				i = tag_end(x, n, i);
				continue;
			}
			if ( i + 1 < n && x[i + 1] == '/' ) end = TRUE;
			nl = name_len(x + i + ( end ? 2 : 1 ), n - i - ( end ? 2 : 1 ));
			e = tag_end(x, n, i);
			empty = (BOOL)( e >= 2 && x[e - 2] == '/' );
			{
				CONST UB	*nm = x + i + ( end ? 2 : 1 );

				if ( end ) {
					if ( name_is(nm, nl, "p") && depth > 0 ) {
						para_end(&r);
						depth = 0;
					}
				} else if ( name_is(nm, nl, "document") ) {
					in_doc = TRUE;
				} else if ( name_is(nm, nl, "p") ) {
					if ( depth > 0 ) para_end(&r);
					depth = empty ? 0 : 1;
				} else if ( name_is(nm, nl, "tab-format") ) {
					if ( r.p->n > 0 ) para_end(&r);
					if ( r.seen ) r.part++;
					r.seen = TRUE;
				} else if ( name_is(nm, nl, "tab") && depth > 0 ) {
					para_add(r.p, (CONST UB *)"\t", 1);
				} else if ( name_is(nm, nl, "br") && depth > 0 ) {
					para_end(&r);
				} else if ( name_is(nm, nl, "paper") && in_doc ) {
					paper_read(db, x + i, e - i);
				} else if ( !empty && passed_over(nm, nl) ) {
					e = skip_to_end(x, n, e, nm, nl);
				}
			}
			i = e;
			continue;
		}
		if ( depth > 0 ) {
			if ( x[i] == '&' ) {
				UB	o[4];
				INT	ol = 0, k = escape_at(x + i, n - i, o, &ol);

				if ( k > 0 ) {
					para_add(r.p, o, ol);
					i += k;
					continue;
				}
			}
			if ( x[i] != '\r' && x[i] != '\n' ) para_add(r.p, x + i, 1);
		}
		i++;
	}
	if ( r.p->n > 0 ) para_end(&r);
	tc_free(r.p);
	if ( r.er < E_OK ) return r.er;
	rules_read(db);
	return addr_sort(db);
}

EXPORT INT tc_doc_text( CONST UB *x, INT n, UB *out, INT max )
{
	INT	i = 0, k = 0;
	BOOL	in_p = FALSE;

	if ( max <= 0 ) return 0;
	while ( i < n && k < max - 4 ) {
		if ( x[i] == '<' ) {
			BOOL		end = (BOOL)( i + 1 < n && x[i + 1] == '/' );
			CONST UB	*nm = x + i + ( end ? 2 : 1 );
			INT		nl = name_len(nm, n - i - ( end ? 2 : 1 )), e = tag_end(x, n, i);
			BOOL		empty = (BOOL)( e >= 2 && x[e - 2] == '/' );

			if ( name_is(nm, nl, "p") ) {
				if ( ( end || in_p ) && k > 0 ) break;
				in_p = (BOOL)( !end && !empty );
			} else if ( name_is(nm, nl, "br") && in_p && k > 0 ) {
				break;
			} else if ( name_is(nm, nl, "tab") && in_p ) {
				out[k++] = ' ';
			} else if ( !end && !empty && passed_over(nm, nl) ) {
				e = skip_to_end(x, n, e, nm, nl);
			}
			i = e;
			continue;
		}
		if ( in_p ) {
			if ( x[i] == '&' ) {
				UB	o[4];
				INT	ol = 0, c = escape_at(x + i, n - i, o, &ol);

				if ( c > 0 ) {
					b_copy(out + k, o, ol);
					k += ol;
					i += c;
					continue;
				}
			}
			if ( x[i] == '\t' ) out[k++] = ' ';
			else if ( x[i] != '\r' && x[i] != '\n' ) out[k++] = x[i];
		}
		i++;
	}
	if ( k >= max - 4 ) {
		/* cut at the limit: no part of a letter left at the end */
		while ( k > 0 && ( out[k - 1] & 0xC0 ) == 0x80 ) k--;
		if ( k > 0 && out[k - 1] >= 0xC0 ) k--;
	}
	out[k] = 0;
	return k;
}

/* ---------------------------------------------------------------- writing */

LOCAL void day_put( CTBUF *b, INT day )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	ct_num(b, y);
	ct_put(b, "/");
	ct_num(b, m);
	ct_put(b, "/");
	ct_num(b, d);
}

LOCAL void paper_put( CONST TCDB *db, CTBUF *b )
{
	ct_put(b, "<paper type=\"doc\" length=\"");
	ct_num(b, db->ph);
	ct_put(b, "\" width=\"");
	ct_num(b, db->pw);
	ct_put(b, "\" binding=\"0\" imposition=\"0\" top=\"");
	ct_num(b, db->mt);
	ct_put(b, "\" bottom=\"");
	ct_num(b, db->mb);
	ct_put(b, "\" left=\"");
	ct_num(b, db->ml);
	ct_put(b, "\" right=\"");
	ct_num(b, db->mr);
	ct_put(b, "\"/>\n<docmargin top=\"");
	ct_num(b, db->mt);
	ct_put(b, "\" bottom=\"");
	ct_num(b, db->mb);
	ct_put(b, "\" left=\"");
	ct_num(b, db->ml);
	ct_put(b, "\" right=\"");
	ct_num(b, db->mr);
	ct_put(b, "\"/>\n");
}

/* The tab format that begins each part: a tab stop every eight letters */
#define TAB_FORMAT	"<tab-format R=\"0\" P=\"0\" left=\"0\" right=\"0\" indent=\"0\" ntabs=\"1\" tabs=\"112\"/>"

EXPORT ER tc_write( CONST TCDB *db, CTBUF *b, CONST UB *name )
{
	INT	i, f, last;

	ct_put(b, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	ct_esc(b, name, TC_LINE_MAX);
	ct_put(b, "\">\n<document>\n");
	paper_put(db, b);

	/* 予定表 */
	ct_put(b, "<p>" TAB_FORMAT);
	for ( i = 0; i < db->np; i++ ) {
		if ( i > 0 ) ct_put(b, "<p>");
		day_put(b, db->p[i].day);
		ct_put(b, "<tab/>");
		ct_esc(b, db->p[i].text, TC_LINE_MAX);
		ct_put(b, "</p>\n");
	}
	if ( db->np == 0 ) ct_put(b, "</p>\n");

	/* 住所録: a person with every field empty is not written */
	ct_put(b, "<p>" TAB_FORMAT);
	for ( i = 0, last = 0; i < db->na; i++ ) {
		BOOL	any = FALSE;

		for ( f = 0; f < TC_NF; f++ ) {
			if ( db->a[i]->f[f] != NULL ) any = TRUE;
		}
		if ( !any ) continue;
		if ( last++ > 0 ) ct_put(b, "<p>");
		for ( f = 0; f < TC_NF; f++ ) {
			if ( f > 0 ) ct_put(b, "<tab/>");
			if ( db->a[i]->f[f] != NULL ) ct_esc(b, db->a[i]->f[f], TC_LINE_MAX);
		}
		ct_put(b, "</p>\n");
	}
	if ( last == 0 ) ct_put(b, "</p>\n");

	/* 休日 */
	ct_put(b, "<p>" TAB_FORMAT);
	for ( i = 0; i < db->nhl; i++ ) {
		if ( i > 0 ) ct_put(b, "<p>");
		for ( f = 0; db->hl[i][f] != 0; f++ ) {
			if ( db->hl[i][f] == '\t' ) {
				ct_put(b, "<tab/>");
			} else {
				ct_esc(b, db->hl[i] + f, 1);
			}
		}
		ct_put(b, "</p>\n");
	}
	if ( db->nhl == 0 ) ct_put(b, "</p>\n");
	ct_put(b, "<p>" TAB_FORMAT "</p>\n</document>\n</tad>\n");
	return b->fail ? E_LIMIT : E_OK;
}

/* ---------------------------------------------------------------- 印刷 */

LOCAL void print_head( CONST TCDB *db, CTBUF *b )
{
	ct_put(b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n");
	paper_put(db, b);
}

/* "2005年9月1日(木)" of a day */
LOCAL void day_title( CTBUF *b, INT day )
{
	INT	y, m, d;

	tc_day_split(day, &y, &m, &d);
	ct_num(b, y);
	ct_put(b, "年");
	ct_num(b, m);
	ct_put(b, "月");
	ct_num(b, d);
	ct_put(b, "日（");
	ct_put(b, wd_name[ct_wday(y, m, d)]);
	ct_put(b, "）");
}

LOCAL void print_day( CONST TCDB *db, CTBUF *b, INT day )
{
	CONST UB	*rows[TC_ROWS * 4];
	INT		n = tc_plans(db, day, rows, TC_ROWS * 4), i, y, m, d;

	tc_day_split(day, &y, &m, &d);
	ct_put(b, "<p><font size=\"16\"/><bold>");
	day_title(b, day);
	ct_put(b, "</bold>");
	if ( tc_holiday(db, y, m, d) ) ct_put(b, "　休日");
	ct_put(b, "</p>\n");
	if ( n > TC_ROWS * 4 ) n = TC_ROWS * 4;
	for ( i = 0; i < ( n > TC_ROWS ? n : TC_ROWS ); i++ ) {
		ct_put(b, "<p><font size=\"12\"/>");
		if ( i < n ) ct_esc(b, rows[i], TC_LINE_MAX);
		ct_put(b, "</p>\n");
	}
}

EXPORT ER tc_print_plans( CONST TCDB *db, CTBUF *b, INT day )
{
	INT	k = 0;

	print_head(db, b);
	if ( day > 0 ) {
		print_day(db, b, day);
	} else {
		for ( day = tc_next_planned(db, TC_DAY_FIRST - 1); day != 0; day = tc_next_planned(db, day) ) {
			if ( k++ > 0 ) ct_put(b, "<p></p>\n");
			print_day(db, b, day);
		}
		if ( k == 0 ) return E_NOEXS;
	}
	ct_put(b, "</document>\n</tad>\n");
	return b->fail ? E_LIMIT : E_OK;
}

LOCAL void print_addr( CONST TCDB *db, CTBUF *b, INT i )
{
	INT	k;

	ct_put(b, "<p><font size=\"16\"/><bold>");
	ct_esc(b, tc_addr_get(db, i, TC_F_NAME), TC_LINE_MAX);
	ct_put(b, "</bold>　<font size=\"10\"/>");
	ct_esc(b, tc_addr_get(db, i, TC_F_YOMI), TC_LINE_MAX);
	ct_put(b, "</p>\n");
	for ( k = 0; k < TC_NF; k++ ) {
		INT	f = tc_show_order[k];

		if ( f == TC_F_YOMI || f == TC_F_NAME ) continue;
		ct_put(b, "<p><font size=\"12\"/>");
		ct_put(b, tc_field_name[f]);
		ct_put(b, "<tab/>");
		ct_esc(b, tc_addr_get(db, i, f), TC_LINE_MAX);
		ct_put(b, "</p>\n");
	}
}

EXPORT ER tc_print_addrs( CONST TCDB *db, CTBUF *b, INT i )
{
	INT	k;

	if ( db->na == 0 ) return E_NOEXS;
	print_head(db, b);
	ct_put(b, "<p>" TAB_FORMAT "</p>\n");
	if ( i >= 0 ) {
		print_addr(db, b, i);
	} else {
		for ( k = 0; k < db->na; k++ ) {
			if ( k > 0 ) ct_put(b, "<p></p>\n");
			print_addr(db, b, k);
		}
	}
	ct_put(b, "</document>\n</tad>\n");
	return b->fail ? E_LIMIT : E_OK;
}

/* ---------------------------------------------------------------- the paper */

LOCAL CONST INT paper_pt[TC_PAPER_N][2] = {
	{ 595, 842 }, { 420, 595 }, { 516, 729 }, { 729, 1032 }, { 842, 1191 }, { 612, 792 }
};

EXPORT void tc_set_paper( TCDB *db, INT kind, BOOL turn, CONST INT *mm )
{
	if ( kind < 0 || kind >= TC_PAPER_N ) kind = TC_PAPER_A4;
	db->pw = paper_pt[kind][turn ? 1 : 0];
	db->ph = paper_pt[kind][turn ? 0 : 1];
	db->mt = ( mm[0] * 7200 + 1270 ) / 2540;
	db->mb = ( mm[1] * 7200 + 1270 ) / 2540;
	db->ml = ( mm[2] * 7200 + 1270 ) / 2540;
	db->mr = ( mm[3] * 7200 + 1270 ) / 2540;
}

EXPORT INT tc_paper_kind( CONST TCDB *db, BOOL *p_turn, INT *mm )
{
	INT	k;

	mm[0] = ( db->mt * 2540 + 3600 ) / 7200;
	mm[1] = ( db->mb * 2540 + 3600 ) / 7200;
	mm[2] = ( db->ml * 2540 + 3600 ) / 7200;
	mm[3] = ( db->mr * 2540 + 3600 ) / 7200;
	*p_turn = (BOOL)( db->pw > db->ph );
	for ( k = 0; k < TC_PAPER_N; k++ ) {
		if ( ( db->pw == paper_pt[k][0] && db->ph == paper_pt[k][1] )
		  || ( db->pw == paper_pt[k][1] && db->ph == paper_pt[k][0] ) ) return k;
	}
	return TC_PAPER_N;
}
