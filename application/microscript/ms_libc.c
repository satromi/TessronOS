/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_libc.c
 *	What the C library needs from the system, memory, and the text
 *	helpers of マイクロスクリプト
 *
 *	The C library (newlib) takes its memory from _sbrk, which here hands
 *	out a fixed area of the program's own. Output to the standard streams
 *	goes to the console.
 */

#include "ms.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define ARENA_SIZE	( 16 * 1024 * 1024 )

static char	arena[ARENA_SIZE] __attribute__((aligned(16)));
static size_t	arena_used;

void *_sbrk( ptrdiff_t incr )
{
	char	*p;

	if ( incr < 0 ? (size_t)( -incr ) > arena_used : arena_used + (size_t)incr > ARENA_SIZE ) {
		errno = ENOMEM;
		return (void *)-1;
	}
	p = arena + arena_used;
	arena_used += (size_t)incr;
	return p;
}

int _write( int fd, const void *buf, size_t n )
{
	char	tmp[128];
	size_t	k, done = 0;

	if ( fd != 1 && fd != 2 ) {
		errno = EBADF;
		return -1;
	}
	while ( done < n ) {
		k = n - done;
		if ( k > sizeof(tmp) - 1 ) k = sizeof(tmp) - 1;
		memcpy(tmp, (const char *)buf + done, k);
		tmp[k] = 0;
		tm_putstring((const UB *)tmp);
		done += k;
	}
	return (int)n;
}

int _read( int fd, void *buf, size_t n )
{
	(void)fd; (void)buf; (void)n;
	return 0;
}

int _close( int fd )
{
	(void)fd;
	return -1;
}

int _fstat( int fd, struct stat *st )
{
	(void)fd;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR;
	return 0;
}

int _isatty( int fd )
{
	return ( fd <= 2 );
}

off_t _lseek( int fd, off_t off, int whence )
{
	(void)fd; (void)off; (void)whence;
	return 0;
}

int _open( const char *path, int flags, ... )
{
	(void)path; (void)flags;
	errno = ENOENT;
	return -1;
}

void _exit( int code )
{
	ts_ext_prc(code);
	for ( ;; ) ;
}

int _kill( int pid, int sig )
{
	(void)pid; (void)sig;
	errno = EINVAL;
	return -1;
}

int _getpid( void )
{
	return (int)ts_get_pid();
}

/* ---------------------------------------------------------------- memory */

void *ms_alloc( size_t n )
{
	void	*p = calloc(1, n > 0 ? n : 1);

	if ( p == NULL ) {
		tm_putstring((const UB *)"microscript: out of memory\n");
		ts_ext_prc(3);
	}
	return p;
}

void *ms_realloc( void *p, size_t n )
{
	void	*q = realloc(p, n > 0 ? n : 1);

	if ( q == NULL ) {
		tm_putstring((const UB *)"microscript: out of memory\n");
		ts_ext_prc(3);
	}
	return q;
}

void ms_free( void *p )
{
	free(p);
}

char *ms_strdup( const char *s )
{
	size_t	n = strlen(s);
	char	*d = ms_alloc(n + 1);

	memcpy(d, s, n);
	return d;
}

/* ---------------------------------------------------------------- UTF-8 */

INT ms_utf8_dec( const UB *s, INT n, UW *p_cp )
{
	UW	c;
	INT	k, i;

	if ( n <= 0 ) return 0;
	c = s[0];
	if ( c < 0x80 ) { *p_cp = c; return 1; }
	if ( ( c & 0xE0 ) == 0xC0 ) { k = 2; c &= 0x1F; }
	else if ( ( c & 0xF0 ) == 0xE0 ) { k = 3; c &= 0x0F; }
	else if ( ( c & 0xF8 ) == 0xF0 ) { k = 4; c &= 0x07; }
	else { *p_cp = 0xFFFD; return 1; }
	if ( k > n ) { *p_cp = 0xFFFD; return n; }
	for ( i = 1; i < k; i++ ) {
		if ( ( s[i] & 0xC0 ) != 0x80 ) { *p_cp = 0xFFFD; return i; }
		c = ( c << 6 ) | ( s[i] & 0x3F );
	}
	*p_cp = c;
	return k;
}

INT ms_utf8_enc( UW cp, UB *out )
{
	if ( cp < 0x80 ) { out[0] = (UB)cp; return 1; }
	if ( cp < 0x800 ) {
		out[0] = (UB)( 0xC0 | ( cp >> 6 ) );
		out[1] = (UB)( 0x80 | ( cp & 0x3F ) );
		return 2;
	}
	if ( cp < 0x10000 ) {
		out[0] = (UB)( 0xE0 | ( cp >> 12 ) );
		out[1] = (UB)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out[2] = (UB)( 0x80 | ( cp & 0x3F ) );
		return 3;
	}
	out[0] = (UB)( 0xF0 | ( cp >> 18 ) );
	out[1] = (UB)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
	out[2] = (UB)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
	out[3] = (UB)( 0x80 | ( cp & 0x3F ) );
	return 4;
}

/*
 * The script may be written in full width: letters, digits and signs of
 * the full width block are the plain ones, and so are a few signs from
 * elsewhere that look like them.
 */
UW ms_norm_cp( UW cp )
{
	if ( cp >= 0xFF01 && cp <= 0xFF5E ) return cp - 0xFEE0;
	switch ( cp ) {
	case 0x2010: case 0x2212:	return '-';	/* ‐ − */
	case 0x00D7:			return '*';	/* × */
	case 0x00F7:			return '/';	/* ÷ */
	case 0x3008:			return '<';	/* 〈 */
	case 0x3009:			return '>';	/* 〉 */
	case 0xFFE3: case 0x301C:	return '~';	/* ￣ 〜 */
	case 0x201D: case 0x201C: case 0x309B: case 0x2033:
					return '"';
	case 0x2019: case 0x2018:	return '\'';
	case 0xFFE5:			return '\\';	/* ￥ */
	case 0x3000:			return ' ';
	}
	return cp;
}

/* ---------------------------------------------------------------- buffers */

void mb_init( MSBUF *b )
{
	b->s = NULL;
	b->n = b->cap = 0;
}

void mb_free( MSBUF *b )
{
	ms_free(b->s);
	mb_init(b);
}

static void mb_room( MSBUF *b, INT more )
{
	INT	want = b->n + more + 1;

	if ( want <= b->cap ) return;
	b->cap = ( b->cap < 64 ) ? 64 : b->cap;
	while ( b->cap < want ) b->cap *= 2;
	b->s = ms_realloc(b->s, (size_t)b->cap);
}

void mb_putn( MSBUF *b, const char *s, INT n )
{
	if ( n <= 0 ) {
		mb_room(b, 0);
		b->s[b->n] = 0;
		return;
	}
	mb_room(b, n);
	memcpy(b->s + b->n, s, (size_t)n);
	b->n += n;
	b->s[b->n] = 0;
}

void mb_puts( MSBUF *b, const char *s )
{
	mb_putn(b, s, (INT)strlen(s));
}

void mb_putc( MSBUF *b, char c )
{
	mb_putn(b, &c, 1);
}

void mb_putcp( MSBUF *b, UW cp )
{
	UB	u[4];

	mb_putn(b, (const char *)u, ms_utf8_enc(cp, u));
}

void mb_printf( MSBUF *b, const char *fmt, ... )
{
	va_list	ap;
	char	tmp[256];
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if ( n < 0 ) return;
	if ( n < (int)sizeof(tmp) ) {
		mb_putn(b, tmp, n);
		return;
	}
	mb_room(b, n);
	va_start(ap, fmt);
	vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->n += n;
}

void mb_xml( MSBUF *b, const char *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		switch ( s[i] ) {
		case '<':	mb_puts(b, "&lt;"); break;
		case '>':	mb_puts(b, "&gt;"); break;
		case '&':	mb_puts(b, "&amp;"); break;
		case '"':	mb_puts(b, "&quot;"); break;
		default:	mb_putc(b, s[i]); break;
		}
	}
}

/* ---------------------------------------------------------------- names */

#define INTERN_BUCKETS	1024

static MSSTR	*intern_tbl[INTERN_BUCKETS];

MSSTR *ms_intern( const char *s, INT n )
{
	UW	h = 2166136261U;
	INT	i;
	MSSTR	*p;

	for ( i = 0; i < n; i++ ) h = ( h ^ (UB)s[i] ) * 16777619U;
	for ( p = intern_tbl[h % INTERN_BUCKETS]; p != NULL; p = p->next ) {
		if ( p->hash == h && p->len == n && memcmp(p->s, s, (size_t)n) == 0 ) return p;
	}
	p = ms_alloc(sizeof(MSSTR) + (size_t)n + 1);
	p->hash = h;
	p->len = n;
	memcpy(p->s, s, (size_t)n);
	p->s[n] = 0;
	p->next = intern_tbl[h % INTERN_BUCKETS];
	intern_tbl[h % INTERN_BUCKETS] = p;
	return p;
}

MSSTR *ms_intern_z( const char *s )
{
	return ms_intern(s, (INT)strlen(s));
}

MSSTR *ms_upper( MSSTR *s )
{
	char	tmp[64];
	INT	i;

	if ( s == NULL || s->len >= (INT)sizeof(tmp) ) return s;
	for ( i = 0; i < s->len; i++ ) {
		char	c = s->s[i];

		tmp[i] = ( c >= 'a' && c <= 'z' ) ? (char)( c - 32 ) : c;
	}
	return ms_intern(tmp, s->len);
}
