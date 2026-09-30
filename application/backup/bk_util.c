/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_util.c
 *	バックアップ: names, identities, records and times (design 17.18)
 */

#include "bk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/xfu.h>

#define CHUNK		( 64 * 1024 )
#define NAME_TC		20		/* characters a volume's name has at most */
#define NAKAGURO	"\xE3\x83\xBB"	/* ・ */

/* ---------------------------------------------------------------- what lib/libxfu asks of a program */

EXPORT void *xfu_sys_alloc( SZ size )
{
	return malloc((size_t)size);
}

EXPORT void xfu_sys_free( void *p )
{
	free(p);
}

/* ---------------------------------------------------------------- UUIDs */

EXPORT void bk_uuid_str( const TS_UUID *u, char *out )
{
	static const char	hex[] = "0123456789abcdef";
	INT			i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[u->b[i] >> 4];
		out[n++] = hex[u->b[i] & 0x0F];
	}
	out[n] = 0;
}

/* The UUID a text starts with ("019a...-..._0.xtad"); FALSE when it does not */
EXPORT BOOL bk_uuid_parse( const char *s, TS_UUID *u )
{
	INT	i, k = 0;

	for ( i = 0; s[i] != 0 && k < 32; i++ ) {
		char	c = s[i];
		INT	v;

		if ( c == '-' ) continue;
		if ( c >= '0' && c <= '9' ) v = c - '0';
		else if ( c >= 'a' && c <= 'f' ) v = c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) v = c - 'A' + 10;
		else return FALSE;
		if ( ( k & 1 ) == 0 ) u->b[k / 2] = (UB)( v << 4 );
		else u->b[k / 2] |= (UB)v;
		k++;
	}
	return (BOOL)( k == 32 );
}

/* ---------------------------------------------------------------- records */

/* A whole record of an object read, 0 after it; NULL when it cannot be */
EXPORT UB *bk_read_rec( ID key, INT recno, UINT *p_len )
{
	T_OBREC	r[64];
	INT	cnt = 0, i;
	UD	size = 0;
	SZ	asz = 0;
	UB	*buf;
	BOOL	found = FALSE;

	*p_len = 0;
	if ( ob_lst_rec(key, r, 64, &cnt) < E_OK ) return NULL;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( r[i].recno == recno ) {
			size = r[i].size;
			found = TRUE;
		}
	}
	if ( !found || size > 256 * 1024 * 1024 ) return NULL;
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return NULL;
	while ( (UD)*p_len < size ) {
		SZ	n = ( size - *p_len > CHUNK ) ? CHUNK : (SZ)( size - *p_len );

		if ( ob_rea_rec(key, recno, (D)*p_len, buf + *p_len, n, &asz) < E_OK || asz <= 0 ) break;
		*p_len += (UINT)asz;
	}
	if ( (UD)*p_len != size ) {
		free(buf);
		*p_len = 0;
		return NULL;
	}
	buf[size] = 0;
	return buf;
}

EXPORT ER bk_write_all( ID key, INT recno, const void *data, UINT len )
{
	UINT	at = 0;
	SZ	asz = 0;
	ER	er;

	while ( at < len ) {
		SZ	n = ( len - at > CHUNK ) ? CHUNK : (SZ)( len - at );

		er = ob_wri_rec(key, recno, (D)at, (const UB *)data + at, n, &asz);
		if ( er < E_OK ) return er;
		if ( asz != n ) return E_IO;
		at += (UINT)n;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- times */

/* STIME and TS_TIME are both seconds from 1985-01-01 00:00 UTC */
EXPORT INT bk_iso_of( UINT stime, char *out )
{
	UB	b[40];
	INT	n = xfu_time_iso((D)stime, b);

	if ( n <= 0 ) {
		out[0] = 0;
		return 0;
	}
	memcpy(out, b, (size_t)n);
	out[n] = 0;
	return n;
}

EXPORT UINT bk_stime_of( const char *iso )
{
	D	t = xfu_time_parse((const UB *)iso);

	return ( t > 0 && t < 0xFFFFFFFFLL ) ? (UINT)t : 0;
}

/* ---------------------------------------------------------------- the names of volumes */

/* The bytes of UTF-8 the first n characters take */
LOCAL INT utf8_prefix( const char *s, INT n )
{
	INT	i = 0;

	while ( s[i] != 0 && n > 0 ) {
		UB	c = (UB)s[i];

		i += ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 1;
		n--;
	}
	return i;
}

/* Full width digits: each three bytes, EF BC 90 + d */
LOCAL INT digit_at( const char *s )
{
	const UB *u = (const UB *)s;

	if ( u[0] == 0xEF && u[1] == 0xBC && u[2] >= 0x90 && u[2] <= 0x99 ) return u[2] - 0x90;
	return -1;
}

EXPORT INT bk_vol_number( const char *name )
{
	INT	i = 0, n = 0, d, k = 0;

	while ( ( d = digit_at(name + i) ) >= 0 ) {
		if ( k == 0 && d == 0 ) return -1;
		n = n * 10 + d;
		i += 3;
		k++;
	}
	if ( k == 0 || k > 18 || strncmp(name + i, NAKAGURO, 3) != 0 || name[i + 3] == 0 ) return -1;
	return n;
}

EXPORT void bk_vol_name( const char *base, INT volno, char *out, INT max )
{
	char	num[64];
	INT	i = 0, n, k, left;

	/* a name that is already "N・..." has its number taken off */
	while ( digit_at(base + i) >= 0 ) i += 3;
	if ( i > 0 && strncmp(base + i, NAKAGURO, 3) == 0 ) base += i + 3;

	snprintf(num, sizeof(num), "%d", ( volno > 0 ) ? volno : 1);
	for ( k = 0, n = 0; num[k] != 0 && n + 4 < max; k++ ) {
		out[n++] = (char)0xEF;
		out[n++] = (char)0xBC;
		out[n++] = (char)( 0x90 + num[k] - '0' );
	}
	memcpy(out + n, NAKAGURO, 3);
	n += 3;
	left = NAME_TC - k - 1;			/* 20 characters in all */
	i = utf8_prefix(base, left > 0 ? left : 0);
	if ( n + i >= max ) i = max - n - 1;
	memcpy(out + n, base, (size_t)i);
	out[n + i] = 0;
}

EXPORT void bk_vol_file( const char *name, char *out, INT max )
{
	INT	n = xfu_name_out((const UB *)name, (UB *)out, max - 5);

	if ( n < 0 ) n = 0;
	strcpy(out + n, ".TAD");
}
