/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fsname.c
 *	Names between UTF-8 and UTF-16 for the fimps of exFAT and NTFS.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "fsname.h"

EXPORT INT knl_fs_utf8_to_16( CONST UB *s, INT len, UH *out, INT max )
{
	INT	i = 0, n = 0;

	while ( i < len ) {
		UW	c = s[i], cp;
		INT	more, k;

		if ( c < 0x80 ) {
			cp = c;
			more = 0;
		} else if ( ( c & 0xE0 ) == 0xC0 ) {
			cp = c & 0x1F;
			more = 1;
		} else if ( ( c & 0xF0 ) == 0xE0 ) {
			cp = c & 0x0F;
			more = 2;
		} else if ( ( c & 0xF8 ) == 0xF0 ) {
			cp = c & 0x07;
			more = 3;
		} else {
			return -1;
		}
		if ( i + more >= len + ( more == 0 ) ) {
			return -1;
		}
		for ( k = 1; k <= more; k++ ) {
			if ( ( s[i + k] & 0xC0 ) != 0x80 ) return -1;
			cp = ( cp << 6 ) | ( s[i + k] & 0x3F );
		}
		i += more + 1;
		if ( cp >= 0x10000 ) {
			if ( cp > 0x10FFFF ) return -1;
			if ( n + 2 > max ) return -2;
			cp -= 0x10000;
			out[n++] = (UH)( 0xD800 + ( cp >> 10 ) );
			out[n++] = (UH)( 0xDC00 + ( cp & 0x3FF ) );
		} else {
			if ( n + 1 > max ) return -2;
			out[n++] = (UH)cp;
		}
	}
	return n;
}

EXPORT INT knl_fs_utf16_to_8( CONST UH *in, INT n, UB *out, INT max )
{
	INT	i, o = 0;

	for ( i = 0; i < n; i++ ) {
		UW	cp = in[i];

		if ( cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000 ) {
			cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( in[++i] - 0xDC00 );
		}
		if ( cp < 0x80 ) {
			if ( o + 1 > max ) return -1;
			out[o++] = (UB)cp;
		} else if ( cp < 0x800 ) {
			if ( o + 2 > max ) return -1;
			out[o++] = (UB)( 0xC0 | ( cp >> 6 ) );
			out[o++] = (UB)( 0x80 | ( cp & 0x3F ) );
		} else if ( cp < 0x10000 ) {
			if ( o + 3 > max ) return -1;
			out[o++] = (UB)( 0xE0 | ( cp >> 12 ) );
			out[o++] = (UB)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
			out[o++] = (UB)( 0x80 | ( cp & 0x3F ) );
		} else {
			if ( o + 4 > max ) return -1;
			out[o++] = (UB)( 0xF0 | ( cp >> 18 ) );
			out[o++] = (UB)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
			out[o++] = (UB)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
			out[o++] = (UB)( 0x80 | ( cp & 0x3F ) );
		}
	}
	return o;
}
