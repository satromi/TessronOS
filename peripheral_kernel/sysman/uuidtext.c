/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uuidtext.c
 *	A UUID written as text and read back, and two put in order
 *	(design 8.3)
 *
 *	Nothing of the system is used, so the same code serves the kernel
 *	and a program running as a process that reads records naming objects
 *	(include/ts/tvport.h).
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>

LOCAL CONST char hexdig[] = "0123456789abcdef";

EXPORT ER ts_uuid_to_str( CONST TS_UUID *uuid, char *buf, SZ bufsz )
{
	CONST INT dash[] = { 4, 6, 8, 10 };
	INT	i, n = 0, d = 0;

	if ( uuid == NULL || buf == NULL || bufsz < TS_UUID_STRLEN + 1 ) {
		return E_PAR;
	}
	for ( i = 0; i < 16; i++ ) {
		if ( d < 4 && i == dash[d] ) { buf[n++] = '-'; d++; }
		buf[n++] = hexdig[uuid->b[i] >> 4];
		buf[n++] = hexdig[uuid->b[i] & 0x0F];
	}
	buf[n] = '\0';

	return E_OK;
}

LOCAL INT hexval( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT ER ts_str_to_uuid( CONST char *str, TS_UUID *p_uuid )
{
	INT	i, n = 0, hi, lo;

	if ( str == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	for ( i = 0; i < 16; i++ ) {
		while ( str[n] == '-' ) n++;
		hi = hexval(str[n]);
		lo = ( hi < 0 ) ? -1 : hexval(str[n + 1]);
		if ( lo < 0 ) {
			return E_PAR;
		}
		p_uuid->b[i] = (UB)((hi << 4) | lo);
		n += 2;
	}

	return E_OK;
}

EXPORT INT ts_uuid_cmp( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a->b[i] != b->b[i] ) return ( a->b[i] < b->b[i] ) ? -1 : 1;
	}
	return 0;
}
