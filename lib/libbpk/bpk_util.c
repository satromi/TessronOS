/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_util.c
 *	Text that grows, and colours
 */

#include <ts/bpk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- buffers */

EXPORT void bpk_buf_init( BPKBUF *b )
{
	b->s = NULL;
	b->n = b->max = 0;
	b->fail = FALSE;
}

EXPORT void bpk_buf_free( BPKBUF *b )
{
	free(b->s);
	bpk_buf_init(b);
}

/* Room for more bytes and the 0 after them */
LOCAL BOOL room( BPKBUF *b, INT more )
{
	INT	want = b->n + more + 1, m;
	char	*s;

	if ( b->fail ) return FALSE;
	if ( want <= b->max ) return TRUE;
	m = ( b->max < 256 ) ? 256 : b->max;
	while ( m < want ) m *= 2;
	s = realloc(b->s, (size_t)m);
	if ( s == NULL ) {
		b->fail = TRUE;
		return FALSE;
	}
	b->s = s;
	b->max = m;
	return TRUE;
}

EXPORT void bpk_buf_putn( BPKBUF *b, const char *s, INT n )
{
	if ( n <= 0 || !room(b, n) ) return;
	memcpy(b->s + b->n, s, (size_t)n);
	b->n += n;
	b->s[b->n] = 0;
}

EXPORT void bpk_buf_puts( BPKBUF *b, const char *s )
{
	bpk_buf_putn(b, s, (INT)strlen(s));
}

EXPORT void bpk_buf_putc( BPKBUF *b, char c )
{
	bpk_buf_putn(b, &c, 1);
}

EXPORT void bpk_buf_printf( BPKBUF *b, const char *fmt, ... )
{
	va_list	ap;
	char	tmp[256];
	INT	n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if ( n < 0 ) return;
	if ( n < (INT)sizeof(tmp) ) {
		bpk_buf_putn(b, tmp, n);
		return;
	}
	if ( !room(b, n) ) return;
	va_start(ap, fmt);
	(void)vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->n += n;
}

EXPORT void bpk_buf_xml( BPKBUF *b, const char *s )
{
	for ( ; *s != 0; s++ ) {
		switch ( *s ) {
		case '&':	bpk_buf_puts(b, "&amp;");	break;
		case '<':	bpk_buf_puts(b, "&lt;");	break;
		case '>':	bpk_buf_puts(b, "&gt;");	break;
		case '"':	bpk_buf_puts(b, "&quot;");	break;
		default:
			/* the controls XML does not allow are left out */
			if ( (UB)*s >= 0x20 || *s == '\t' || *s == '\n' ) bpk_buf_putc(b, *s);
			break;
		}
	}
}

EXPORT void bpk_buf_json( BPKBUF *b, const char *s )
{
	for ( ; *s != 0; s++ ) {
		if ( *s == '"' || *s == '\\' ) {
			bpk_buf_putc(b, '\\');
			bpk_buf_putc(b, *s);
		} else if ( (UB)*s >= 0x20 ) {
			bpk_buf_putc(b, *s);
		}
	}
}

EXPORT void bpk_buf_cp( BPKBUF *b, UINT cp )
{
	char	o[4];

	bpk_buf_putn(b, o, bpk_utf8(cp, o));
}

/* A value in thousandths as a decimal: 19200 is "19.2", 12000 "12" */
EXPORT void bpk_buf_num( BPKBUF *b, INT milli )
{
	INT	frac;

	if ( milli < 0 ) {
		bpk_buf_putc(b, '-');
		milli = -milli;
	}
	bpk_buf_printf(b, "%d", milli / 1000);
	frac = milli % 1000;
	if ( frac == 0 ) return;
	if ( frac % 100 == 0 ) bpk_buf_printf(b, ".%d", frac / 100);
	else if ( frac % 10 == 0 ) bpk_buf_printf(b, ".%02d", frac / 10);
	else bpk_buf_printf(b, ".%03d", frac);
}

/* ---------------------------------------------------------------- colours */

/* The system's standard colour map, for a map index a record does not define */
LOCAL const UINT std_cmap[16] = {
	0xFFFFFF, 0x00009F, 0x00EF00, 0x00EFEF, 0xFF0000, 0xEF00FF, 0xDFDF00, 0x7F7F9F,
	0xDFDFDF, 0x7F9FFF, 0xBFFFAF, 0xCFFFFF, 0xFF6F6F, 0xEF8FFF, 0xEFFF9F, 0x000000
};

/*
 * A COLOR: bit 31 transparent, bits 28-30 the mode. Mode 1 is RGB in the
 * low 24 bits; mode 0 an index into the record's colour map, which holds
 * COLORs of its own.
 */
EXPORT void bpk_colour( UINT raw, const UINT *cmap, INT ncmap, char out[8] )
{
	UINT	mode = ( raw >> 28 ) & 7, rgb = 0;

	if ( mode == 1 ) {
		rgb = raw & 0xFFFFFF;
	} else if ( mode == 0 ) {
		UINT	i = raw & 0x0FFFFFFF;

		if ( cmap != NULL && i < (UINT)ncmap && ( ( cmap[i] >> 28 ) & 7 ) == 1 ) rgb = cmap[i] & 0xFFFFFF;
		else if ( i < 16 ) rgb = std_cmap[i];
	}
	snprintf(out, 8, "#%06x", rgb & 0xFFFFFF);
}
