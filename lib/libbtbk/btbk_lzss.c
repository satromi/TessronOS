/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btbk_lzss.c
 *	LZSS of backup archives (design 11.19.3)
 *
 *	The encoder keeps its input in a ring of 8192 bytes: `cur` is the
 *	position being encoded, `oldest` the one leaving the window, always
 *	0xFFE behind, and `wp` where input goes. Every position between
 *	them is in a hash chain of its first three bytes. The chains run
 *	from the oldest position to the newest and a match replaces the
 *	best only when it is longer, so of equal matches the farther one
 *	wins. Positions 0..0xFFD of a fresh encoder were never put in a
 *	chain, so `delay` keeps them from being taken out.
 *
 *	Output is sent as blocks: a count and up to 32 literals, the match
 *	that ended them joined to the same block, the count left out when
 *	there are no literals before a match.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/btbk.h>

#define RMASK		( BK_ENC_RING - 1 )
#define DMASK		( BK_DEC_RING - 1 )
#define WINDOW		0xFFE
#define MAXMATCH	0x100

LOCAL void bytes_copy( UB *d, CONST UB *s, INT n )
{
	while ( n-- > 0 ) *d++ = *s++;
}

LOCAL void bytes_zero( UB *d, INT n )
{
	while ( n-- > 0 ) *d++ = 0;
}

/* ------------------------------------------------------------------ */
/* Encoder */

EXPORT void bk_enc_init( BK_ENC *e, BK_SINK sink, void *arg )
{
	INT	i;

	e->sink = sink;
	e->arg = arg;
	e->oldest = 0;
	e->cur = WINDOW;
	e->wp = WINDOW;
	e->outn = 0;
	e->delay = WINDOW;
	bytes_zero(e->ring, BK_ENC_RING);
	for ( i = 0; i < 0x400; i++ ) {
		e->head[i] = 0;
		e->tail[i] = 0;
	}
	for ( i = 0; i < BK_ENC_RING; i++ ) e->chain[i] = 0;
}

LOCAL UINT hash3( CONST BK_ENC *e, INT p )
{
	return ( e->ring[p] + 2 * e->ring[(p + 1) & RMASK]
		 + 2 * e->ring[(p + 2) & RMASK] ) & 0x3FF;
}

LOCAL ER emit( BK_ENC *e, INT n )
{
	return e->sink(e->arg, e->outbuf, n);
}

/*
 * Encode what the ring holds. Without flush it stops while fewer than
 * 0x104 bytes are ahead of `cur`, so a match can always look 256 bytes
 * further; with flush it goes to the end, the last three bytes always
 * as literals.
 */
LOCAL ER encode( BK_ENC *e, BOOL flush )
{
	UB	*ring = e->ring;
	INT	cur_s, avail, maxlen, best, dist, ln, p, n, a, d, o, c, t;
	UINT	h;
	ER	er;

	cur_s = e->cur;
	if ( cur_s == e->wp ) return E_OK;
	for ( ;; ) {
		avail = ( e->wp - cur_s ) & RMASK;
		if ( flush ) {
			maxlen = avail;
		} else {
			maxlen = avail - 3;
			if ( maxlen <= MAXMATCH ) return E_OK;
		}

		best = 0;
		dist = 0;
		if ( maxlen > 3 ) {
			if ( maxlen > MAXMATCH ) maxlen = MAXMATCH;
			p = (INT)e->head[hash3(e, cur_s)] - 1;
			while ( p >= 0 ) {
				if ( ring[(cur_s + best) & RMASK] == ring[(p + best) & RMASK] ) {
					n = 0;
					if ( ring[cur_s] == ring[p] ) {
						do {
							n++;
							if ( n >= maxlen ) break;
						} while ( ring[(cur_s + n) & RMASK] == ring[(p + n) & RMASK] );
					}
					if ( n > best ) {
						dist = cur_s - p;
						best = n;
						if ( n > 0xFF ) break;
					}
				}
				p = (INT)e->chain[p] - 1;
			}
		}

		ln = best - 1;
		if ( ln <= 1 ) {
			/* literal: a full block of 32 goes at once */
			e->outbuf[++e->outn] = ring[e->cur];
			if ( e->outn > 0x1F ) {
				e->outbuf[0] = (UB)( e->outn - 1 );
				er = emit(e, e->outn + 1);
				if ( er < E_OK ) return er;
				e->outn = 0;
			}
			ln = 0;
		} else {
			/* match, after the literals before it in one block */
			if ( e->outn > 0 ) {
				e->outbuf[0] = (UB)( e->outn - 1 );
			} else {
				e->outn = -1;
			}
			d = dist;
			if ( d < 0 ) d += BK_ENC_RING;
			if ( ln > 0xE ) {
				a = e->outn++;
				e->outbuf[a + 1] = (UB)( 0xF0 | ( ln >> 4 ) );
			}
			d |= ( ln & 0xF ) << 12;
			a = e->outn;
			e->outbuf[a + 1] = (UB)( d >> 8 );
			e->outbuf[a + 2] = (UB)d;
			er = emit(e, a + 3);
			if ( er < E_OK ) return er;
			e->outn = 0;
		}

		/* the window moves over the ln + 1 bytes just encoded */
		do {
			o = e->oldest++;
			if ( e->delay > 0 ) {
				e->delay--;
			} else {
				h = hash3(e, o);
				if ( e->head[h] == e->oldest ) {
					t = e->chain[o];
					e->head[h] = (UH)t;
					if ( t == 0 ) e->tail[h] = 0;
					e->chain[o] = 0;
				}
			}
			e->oldest &= RMASK;

			c = e->cur++;
			h = hash3(e, c);
			e->chain[c] = 0;
			t = e->tail[h];
			if ( t != 0 ) {
				e->chain[t - 1] = (UH)e->cur;
			} else {
				e->head[h] = (UH)e->cur;
			}
			e->tail[h] = (UH)e->cur;
			e->cur &= RMASK;
		} while ( --ln >= 0 );

		cur_s = e->cur;
		if ( cur_s == e->wp ) {
			if ( !flush || e->outn <= 0 ) return E_OK;
			e->outbuf[0] = (UB)( e->outn - 1 );
			er = emit(e, e->outn + 1);
			e->outn = 0;
			return ( er < E_OK ) ? er : E_OK;
		}
	}
}

EXPORT ER bk_enc_put( BK_ENC *e, CONST UB *buf, INT len )
{
	INT	n;
	ER	er;

	if ( len == 0 ) return encode(e, TRUE);
	while ( len > 0 ) {
		if ( e->oldest == e->wp ) {
			/* the ring is full: encode until there is room */
			er = encode(e, FALSE);
			if ( er < E_OK ) return er;
			continue;
		}
		n = ( e->oldest >= e->wp ) ? e->oldest - e->wp : BK_ENC_RING - e->wp;
		if ( n > len ) n = len;
		bytes_copy(e->ring + e->wp, buf, n);
		buf += n;
		len -= n;
		e->wp = ( e->wp + n ) & RMASK;
	}
	return E_OK;
}

EXPORT ER bk_enc_flush( BK_ENC *e )
{
	return encode(e, TRUE);
}

/* ------------------------------------------------------------------ */
/* Decoder */

#define DEC_DRY		1		/* the input ran out */

EXPORT void bk_dec_init( BK_DEC *d, BK_SRC src, void *arg, INT inlen )
{
	d->src = src;
	d->arg = arg;
	d->rest = inlen;
	d->rp = 0;
	d->wp = 0;
	d->ip = 0;
	d->iend = 0;
	bytes_zero(d->ring, BK_DEC_RING);
}

/* The next input byte into *p_c: E_OK, DEC_DRY, or the source's error */
LOCAL ER next_in( BK_DEC *d, UB *p_c )
{
	INT	n;

	if ( d->ip >= d->iend ) {
		if ( d->rest <= 0 ) return DEC_DRY;
		n = ( d->rest > BK_DEC_INBUF ) ? BK_DEC_INBUF : d->rest;
		n = d->src(d->arg, d->in, n);
		if ( n < 0 ) return n;
		if ( n == 0 ) return DEC_DRY;
		d->iend = n;
		d->ip = 0;
		d->rest -= n;
	}
	*p_c = d->in[d->ip++];
	return E_OK;
}

/* One token into the ring, which is empty */
LOCAL ER token( BK_DEC *d )
{
	UB	b0, b1, b2;
	INT	ln, dist, src, n;
	UINT	w;
	ER	er;

	er = next_in(d, &b0);
	if ( er != E_OK ) return er;
	if ( ( b0 & 0xE0 ) == 0 ) {
		for ( n = b0; n >= 0; n-- ) {
			er = next_in(d, &b1);
			if ( er != E_OK ) return er;
			d->ring[d->wp] = b1;
			d->wp = ( d->wp + 1 ) & DMASK;
		}
		return E_OK;
	}
	if ( b0 <= 0xEF ) {
		er = next_in(d, &b1);
		if ( er != E_OK ) return er;
		w = ( (UINT)b0 << 8 ) | b1;
		ln = ( w >> 12 ) & 0xF;
	} else {
		er = next_in(d, &b1);
		if ( er != E_OK ) return er;
		er = next_in(d, &b2);
		if ( er != E_OK ) return er;
		w = ( (UINT)b1 << 8 ) | b2;
		ln = ( ( b0 & 0xF ) << 4 ) + ( ( w >> 12 ) & 0xF );
	}
	dist = w & 0xFFF;
	src = ( d->wp - dist + BK_DEC_RING ) & DMASK;
	for ( n = ln; n >= 0; n-- ) {
		d->ring[d->wp] = d->ring[src];
		src = ( src + 1 ) & DMASK;
		d->wp = ( d->wp + 1 ) & DMASK;
	}
	return E_OK;
}

EXPORT INT bk_dec_read( BK_DEC *d, UB *buf, INT len )
{
	INT	total = 0, k;
	ER	er;

	while ( len > 0 ) {
		if ( d->wp == d->rp ) {
			er = token(d);
			if ( er == DEC_DRY ) break;
			if ( er < E_OK ) return er;
		}
		k = ( d->wp > d->rp ) ? d->wp - d->rp : BK_DEC_RING - d->rp;
		if ( k > len ) k = len;
		bytes_copy(buf, d->ring + d->rp, k);
		d->rp = ( d->rp + k ) & DMASK;
		buf += k;
		len -= k;
		total += k;
	}
	return total;
}

EXPORT BOOL bk_dec_done( CONST BK_DEC *d )
{
	return (BOOL)( d->rest <= 0 && d->ip >= d->iend && d->rp == d->wp );
}

/* ------------------------------------------------------------------ */
/* Memory to memory */

typedef struct {
	UB	*p;
	INT	n, max;
	CONST UB *in;
	INT	inlen, at;
} MEMIO;

LOCAL INT mem_sink( void *arg, CONST UB *buf, INT len )
{
	MEMIO	*m = arg;

	if ( m->n + len > m->max ) return E_LIMIT;
	bytes_copy(m->p + m->n, buf, len);
	m->n += len;
	return E_OK;
}

LOCAL INT mem_src( void *arg, UB *buf, INT len )
{
	MEMIO	*m = arg;

	if ( len > m->inlen - m->at ) len = m->inlen - m->at;
	bytes_copy(buf, m->in + m->at, len);
	m->at += len;
	return len;
}

EXPORT INT bk_compress( BK_ENC *work, CONST UB *in, INT inlen, UB *out, INT outmax )
{
	MEMIO	m;
	ER	er;

	m.p = out;
	m.n = 0;
	m.max = outmax;
	bk_enc_init(work, mem_sink, &m);
	er = bk_enc_put(work, in, inlen);
	if ( er >= E_OK ) er = bk_enc_flush(work);
	return ( er < E_OK ) ? er : m.n;
}

EXPORT INT bk_decompress( BK_DEC *work, CONST UB *in, INT inlen, UB *out, INT outmax )
{
	MEMIO	m;
	INT	n;

	m.in = in;
	m.inlen = inlen;
	m.at = 0;
	bk_dec_init(work, mem_src, &m, inlen);
	n = bk_dec_read(work, out, outmax);
	if ( n < 0 ) return n;
	if ( n == outmax && !bk_dec_done(work) ) return E_LIMIT;
	return n;
}
