/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	deflate.c
 *	Making DEFLATE (RFC 1951) inside a zlib stream (RFC 1950)
 *
 *	What writes a file of pages (prn_) packs its streams with this:
 *	the drawing of a page, the faces and the pictures. A reader of
 *	those files undoes it with the usual inflate, so what is written
 *	here only has to be correct DEFLATE, and it is worth making it
 *	small because a page of letters repeats itself a great deal.
 *
 *	The matching is the usual one: the last three bytes hashed into a
 *	table of where they were last seen, a chain back through the
 *	earlier places with the same hash, and the longest match taken,
 *	put off by one byte when the next byte starts a longer one. Each
 *	block is coded with its own Huffman codes made from what it holds;
 *	a code that comes out longer than fifteen bits is made again from
 *	counts halved until it fits, which costs a little of the packing
 *	and nothing of the correctness.
 *
 *	No floating point: the kernel is built without it.
 */

#include <ts/tvport.h>
#include <ts/img.h>

#define WSIZE		32768		/* how far back a match may reach */
#define WMASK		( WSIZE - 1 )
#define HBITS		15
#define HSIZE		( 1 << HBITS )
#define MIN_MATCH	3
#define MAX_MATCH	258
#define BLOCK_SYMS	16384		/* symbols coded in one block */

#define NLIT		286
#define NDIST		30
#define NCLEN		19
#define MAX_BITS	15
#define MAX_CL_BITS	7

/* The base length and extra bits of each length code (257..285) */
LOCAL CONST UH len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
LOCAL CONST UB len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
LOCAL CONST UH dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577
};
LOCAL CONST UB dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* The order the lengths of the code lengths are written in */
LOCAL CONST UB clen_order[NCLEN] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* One symbol of a block: a literal byte, or a length and a distance */
typedef struct {
	UH	lit;			/* 0..255 a byte, 257.. a length code */
	UH	dist;			/* 0 for a byte, else the distance */
	UH	len;			/* the match's length */
} SYM;

typedef struct {
	UB	*out;
	SZ	size, at;
	BOOL	fail;
	UW	bit_buf;
	INT	bit_cnt;

	INT	*head;			/* HSIZE: the last place of each hash */
	INT	*prev;			/* WSIZE: the place before, by place & WMASK */
	SYM	*sym;
	INT	nsym;
	INT	chain;			/* how far down a chain to look */
	INT	nice;			/* a match this long is taken at once */

	UW	lfreq[NLIT];
	UW	dfreq[NDIST];
	struct clwork	*cl;		/* code_lengths' work, 7KB */
} DEF;

/* ---------------------------------------------------------------- output */

LOCAL void out_byte( DEF *z, UB b )
{
	if ( z->at >= z->size ) {
		SZ	ns = z->size * 2 + 4096;
		UB	*n = (UB *)Krealloc(z->out, ns);

		if ( n == NULL ) {
			z->fail = TRUE;
			return;
		}
		z->out = n;
		z->size = ns;
	}
	z->out[z->at++] = b;
}

/* Bits are written from the least significant, as DEFLATE wants */
LOCAL void put_bits( DEF *z, UW v, INT n )
{
	z->bit_buf |= v << z->bit_cnt;
	z->bit_cnt += n;
	while ( z->bit_cnt >= 8 ) {
		out_byte(z, (UB)z->bit_buf);
		z->bit_buf >>= 8;
		z->bit_cnt -= 8;
	}
}

LOCAL void flush_bits( DEF *z )
{
	if ( z->bit_cnt > 0 ) {
		out_byte(z, (UB)z->bit_buf);
	}
	z->bit_buf = 0;
	z->bit_cnt = 0;
}

/* A Huffman code is written with its first bit the most significant */
LOCAL UW reverse( UW code, INT n )
{
	UW	r = 0;

	while ( n-- > 0 ) {
		r = ( r << 1 ) | ( code & 1 );
		code >>= 1;
	}
	return r;
}

/* ---------------------------------------------------------------- codes */

/*
 * The lengths of a Huffman code for these counts, none longer than
 * 'limit'. The tree is built the plain way -- the two least taken
 * joined again and again -- which for at most 286 symbols is cheap
 * enough done by looking for the two smallest each time. When a
 * length comes out too long the counts are halved (keeping every
 * symbol that was used) and the tree built again.
 */
typedef struct clwork {
	UW	f[2 * NLIT], cnt[NLIT];
	INT	parent[2 * NLIT];
	BOOL	alive[2 * NLIT];
} CLWORK;

LOCAL void code_lengths( CLWORK *cw, CONST UW *freq, INT n, INT limit, UB *len )
{
	UW	*f = cw->f, *cnt = cw->cnt;
	INT	*parent = cw->parent;
	BOOL	*alive = cw->alive;
	INT	i, nodes, used, maxlen;

	for ( i = 0; i < n; i++ ) {
		cnt[i] = freq[i];
	}
	for ( ;; ) {
		used = 0;
		for ( i = 0; i < n; i++ ) {
			f[i] = cnt[i];
			alive[i] = (BOOL)( cnt[i] > 0 );
			parent[i] = -1;
			len[i] = 0;
			if ( cnt[i] > 0 ) {
				used++;
			}
		}
		if ( used == 0 ) {
			return;
		}
		if ( used == 1 ) {
			for ( i = 0; i < n; i++ ) {
				if ( cnt[i] > 0 ) {
					len[i] = 1;
				}
			}
			return;
		}
		nodes = n;
		while ( used > 1 ) {
			INT	a = -1, b = -1;

			for ( i = 0; i < nodes; i++ ) {
				if ( !alive[i] ) {
					continue;
				}
				if ( a < 0 || f[i] < f[a] ) {
					b = a;
					a = i;
				} else if ( b < 0 || f[i] < f[b] ) {
					b = i;
				}
			}
			f[nodes] = f[a] + f[b];
			alive[nodes] = TRUE;
			parent[nodes] = -1;
			alive[a] = alive[b] = FALSE;
			parent[a] = parent[b] = nodes;
			nodes++;
			used--;
		}
		maxlen = 0;
		for ( i = 0; i < n; i++ ) {
			INT	d = 0, p;

			if ( cnt[i] == 0 ) {
				continue;
			}
			for ( p = parent[i]; p >= 0; p = parent[p] ) {
				d++;
			}
			len[i] = (UB)( ( d > 255 ) ? 255 : d );
			if ( d > maxlen ) {
				maxlen = d;
			}
		}
		if ( maxlen <= limit ) {
			return;
		}
		for ( i = 0; i < n; i++ ) {
			if ( cnt[i] > 0 ) {
				cnt[i] = ( cnt[i] + 1 ) / 2;
			}
		}
	}
}

/* The canonical codes for lengths (RFC 1951 3.2.2), already reversed */
LOCAL void make_codes( CONST UB *len, INT n, UH *code )
{
	UH	bl_count[MAX_BITS + 1], next[MAX_BITS + 2];
	INT	i, b;
	UH	c = 0;

	for ( b = 0; b <= MAX_BITS; b++ ) {
		bl_count[b] = 0;
	}
	for ( i = 0; i < n; i++ ) {
		bl_count[len[i]]++;
	}
	bl_count[0] = 0;
	for ( b = 1; b <= MAX_BITS; b++ ) {
		c = (UH)( ( c + bl_count[b - 1] ) << 1 );
		next[b] = c;
	}
	for ( i = 0; i < n; i++ ) {
		if ( len[i] != 0 ) {
			code[i] = (UH)reverse(next[len[i]]++, len[i]);
		} else {
			code[i] = 0;
		}
	}
}

/* ---------------------------------------------------------------- blocks */

LOCAL INT len_code( INT l )
{
	INT	i;

	for ( i = 28; i > 0; i-- ) {
		if ( l >= len_base[i] ) {
			return i;
		}
	}
	return 0;
}

LOCAL INT dist_code( INT d )
{
	INT	i;

	for ( i = 29; i > 0; i-- ) {
		if ( d >= dist_base[i] ) {
			return i;
		}
	}
	return 0;
}

/*
 * The code lengths of both codes, run-length coded with 16, 17 and 18
 * as RFC 1951 3.2.7 says, into 'rl' (symbol, extra) pairs.
 */
LOCAL INT rle_lengths( CONST UB *lens, INT n, UB *rl )
{
	INT	i = 0, k = 0;

	while ( i < n ) {
		INT	run = 1;

		while ( i + run < n && lens[i + run] == lens[i] ) {
			run++;
		}
		if ( lens[i] == 0 && run >= 3 ) {
			INT	r = ( run > 138 ) ? 138 : run;

			if ( r >= 11 ) {
				rl[k++] = 18;
				rl[k++] = (UB)( r - 11 );
			} else {
				rl[k++] = 17;
				rl[k++] = (UB)( r - 3 );
			}
			i += r;
			continue;
		}
		if ( lens[i] != 0 && run >= 4 ) {
			INT	r = run - 1;

			rl[k++] = lens[i];
			rl[k++] = 0;
			if ( r > 6 ) {
				r = 6;
			}
			rl[k++] = 16;
			rl[k++] = (UB)( r - 3 );
			i += 1 + r;
			continue;
		}
		rl[k++] = lens[i];
		rl[k++] = 0;
		i++;
	}

	return k;
}

/* What is gathered of a block written out with codes of its own */
LOCAL void write_block( DEF *z, BOOL last )
{
	UB	llen[NLIT], dlen[NDIST], clen[NCLEN], all[NLIT + NDIST];
	UH	lcode[NLIT], dcode[NDIST], ccode[NCLEN];
	UB	rl[2 * ( NLIT + NDIST )];
	UW	cfreq[NCLEN];
	INT	i, nl, nd, nc, nrl, ndist_used = 0;

	if ( z->nsym == 0 ) {
		/* nothing in it: the fixed code's end of block, seven noughts */
		put_bits(z, last ? 1 : 0, 1);
		put_bits(z, 1, 2);
		put_bits(z, 0, 7);
		return;
	}
	z->lfreq[256] = 1;			/* the end of the block */
	for ( i = 0; i < NDIST; i++ ) {
		if ( z->dfreq[i] != 0 ) {
			ndist_used++;
		}
	}
	code_lengths(z->cl, z->lfreq, NLIT, MAX_BITS, llen);
	code_lengths(z->cl, z->dfreq, NDIST, MAX_BITS, dlen);
	if ( ndist_used == 0 ) {
		dlen[0] = 1;			/* one distance code, never used */
	} else if ( ndist_used == 1 ) {
		/* a lone code of one bit is allowed; give it a partner to be safe */
		for ( i = 0; i < NDIST; i++ ) {
			if ( dlen[i] == 0 ) {
				dlen[i] = 1;
				break;
			}
		}
	}
	make_codes(llen, NLIT, lcode);
	make_codes(dlen, NDIST, dcode);
	for ( nl = NLIT; nl > 257 && llen[nl - 1] == 0; nl-- ) ;
	for ( nd = NDIST; nd > 1 && dlen[nd - 1] == 0; nd-- ) ;
	for ( i = 0; i < nl; i++ ) {
		all[i] = llen[i];
	}
	for ( i = 0; i < nd; i++ ) {
		all[nl + i] = dlen[i];
	}
	nrl = rle_lengths(all, nl + nd, rl);
	for ( i = 0; i < NCLEN; i++ ) {
		cfreq[i] = 0;
	}
	for ( i = 0; i < nrl; i += 2 ) {
		cfreq[rl[i]]++;
	}
	code_lengths(z->cl, cfreq, NCLEN, MAX_CL_BITS, clen);
	make_codes(clen, NCLEN, ccode);
	for ( nc = NCLEN; nc > 4 && clen[clen_order[nc - 1]] == 0; nc-- ) ;

	put_bits(z, last ? 1 : 0, 1);
	put_bits(z, 2, 2);			/* dynamic codes */
	put_bits(z, (UW)( nl - 257 ), 5);
	put_bits(z, (UW)( nd - 1 ), 5);
	put_bits(z, (UW)( nc - 4 ), 4);
	for ( i = 0; i < nc; i++ ) {
		put_bits(z, clen[clen_order[i]], 3);
	}
	for ( i = 0; i < nrl; i += 2 ) {
		INT	s = rl[i];

		put_bits(z, ccode[s], clen[s]);
		if ( s == 16 ) {
			put_bits(z, rl[i + 1], 2);
		} else if ( s == 17 ) {
			put_bits(z, rl[i + 1], 3);
		} else if ( s == 18 ) {
			put_bits(z, rl[i + 1], 7);
		}
	}
	for ( i = 0; i < z->nsym; i++ ) {
		CONST SYM	*s = &z->sym[i];

		if ( s->dist == 0 ) {
			put_bits(z, lcode[s->lit], llen[s->lit]);
		} else {
			INT	lc = len_code(s->len), dc = dist_code(s->dist);

			put_bits(z, lcode[257 + lc], llen[257 + lc]);
			if ( len_extra[lc] != 0 ) {
				put_bits(z, (UW)( s->len - len_base[lc] ), len_extra[lc]);
			}
			put_bits(z, dcode[dc], dlen[dc]);
			if ( dist_extra[dc] != 0 ) {
				put_bits(z, (UW)( s->dist - dist_base[dc] ), dist_extra[dc]);
			}
		}
	}
	put_bits(z, lcode[256], llen[256]);

	z->nsym = 0;
	knl_memset(z->lfreq, 0, sizeof(z->lfreq));
	knl_memset(z->dfreq, 0, sizeof(z->dfreq));
}

LOCAL void add_lit( DEF *z, UB b )
{
	z->sym[z->nsym].lit = b;
	z->sym[z->nsym].dist = 0;
	z->sym[z->nsym].len = 0;
	z->nsym++;
	z->lfreq[b]++;
}

LOCAL void add_match( DEF *z, INT len, INT dist )
{
	z->sym[z->nsym].lit = (UH)( 257 + len_code(len) );
	z->sym[z->nsym].dist = (UH)dist;
	z->sym[z->nsym].len = (UH)len;
	z->nsym++;
	z->lfreq[257 + len_code(len)]++;
	z->dfreq[dist_code(dist)]++;
}

/* ---------------------------------------------------------------- matching */

LOCAL UW hash3( CONST UB *p )
{
	return ( ( (UW)p[0] << 10 ) ^ ( (UW)p[1] << 5 ) ^ p[2] ) & ( HSIZE - 1 );
}

LOCAL void insert( DEF *z, CONST UB *in, INT at )
{
	UW	h = hash3(in + at);

	z->prev[at & WMASK] = z->head[h];
	z->head[h] = at;
}

/* The longest match for 'at' among the earlier places; its length, 0 for none */
LOCAL INT longest( DEF *z, CONST UB *in, INT n, INT at, INT *p_dist )
{
	INT	cand = z->head[hash3(in + at)], best = 0, chain = z->chain;
	INT	maxlen = n - at;

	if ( maxlen > MAX_MATCH ) {
		maxlen = MAX_MATCH;
	}
	if ( maxlen < MIN_MATCH ) {
		return 0;
	}
	while ( cand >= 0 && at - cand <= WSIZE && at - cand > 0 && chain-- > 0 ) {
		if ( in[cand + best] == in[at + best] && in[cand] == in[at] ) {
			INT	l = 0;

			while ( l < maxlen && in[cand + l] == in[at + l] ) {
				l++;
			}
			if ( l > best ) {
				best = l;
				*p_dist = at - cand;
				if ( l >= z->nice || l == maxlen ) {
					break;
				}
			}
		}
		{
			INT	p = z->prev[cand & WMASK];

			if ( p >= cand ) {
				break;		/* the slot was taken by a later place */
			}
			cand = p;
		}
	}

	return ( best >= MIN_MATCH ) ? best : 0;
}

/*
 * 'in' packed into a zlib stream, into memory that is the caller's to
 * Kfree. 'level' 1 looks little and is quick; 9 looks hard.
 */
EXPORT ER ts_zlib_deflate( CONST UB *in, SZ in_len, UB **p_out, SZ *p_len,
			   INT level )
{
	DEF	z;
	INT	n = (INT)in_len, at = 0, i;
	UW	a = 1, b = 0;

	if ( ( in == NULL && in_len > 0 ) || p_out == NULL || p_len == NULL
	  || in_len > 0x7FFFFFFF ) {
		return E_PAR;
	}
	knl_memset(&z, 0, sizeof(z));
	z.size = in_len / 2 + 1024;
	z.out = (UB *)Kmalloc(z.size);
	z.head = (INT *)Kmalloc(sizeof(INT) * HSIZE);
	z.prev = (INT *)Kmalloc(sizeof(INT) * WSIZE);
	z.sym = (SYM *)Kmalloc(sizeof(SYM) * ( BLOCK_SYMS + 2 ));
	z.cl = (CLWORK *)Kmalloc(sizeof(CLWORK));
	if ( z.out == NULL || z.head == NULL || z.prev == NULL || z.sym == NULL || z.cl == NULL ) {
		if ( z.out != NULL ) Kfree(z.out);
		if ( z.head != NULL ) Kfree(z.head);
		if ( z.prev != NULL ) Kfree(z.prev);
		if ( z.sym != NULL ) Kfree(z.sym);
		if ( z.cl != NULL ) Kfree(z.cl);
		return E_NOMEM;
	}
	if ( level < 1 ) level = 1;
	if ( level > 9 ) level = 9;
	z.chain = ( level <= 2 ) ? 8 : ( level <= 6 ) ? 64 : 512;
	z.nice = ( level <= 2 ) ? 32 : ( level <= 6 ) ? 128 : MAX_MATCH;
	for ( i = 0; i < HSIZE; i++ ) {
		z.head[i] = -1;
	}
	for ( i = 0; i < WSIZE; i++ ) {
		z.prev[i] = -1;
	}

	out_byte(&z, 0x78);			/* deflate, a window of 32K */
	out_byte(&z, 0xDA);			/* the best packing, checks with 0x78 */

	while ( at < n ) {
		INT	dist = 0, len = 0;

		if ( at + MIN_MATCH <= n ) {
			len = longest(&z, in, n, at, &dist);
		}
		if ( len > 0 && len < z.nice && at + 1 + MIN_MATCH <= n ) {
			/* put off by one byte when the next starts a longer match */
			INT	d2 = 0, l2;

			insert(&z, in, at);
			l2 = longest(&z, in, n, at + 1, &d2);
			if ( l2 > len ) {
				add_lit(&z, in[at]);
				at++;
				len = l2;
				dist = d2;
			} else {
				/* 'at' is already in the table */
				add_match(&z, len, dist);
				for ( i = 1; i < len; i++ ) {
					if ( at + i + MIN_MATCH <= n ) {
						insert(&z, in, at + i);
					}
				}
				at += len;
				goto next;
			}
		}
		if ( len > 0 ) {
			add_match(&z, len, dist);
			for ( i = 0; i < len; i++ ) {
				if ( at + i + MIN_MATCH <= n ) {
					insert(&z, in, at + i);
				}
			}
			at += len;
		} else {
			if ( at + MIN_MATCH <= n ) {
				insert(&z, in, at);
			}
			add_lit(&z, in[at]);
			at++;
		}
	next:
		if ( z.nsym >= BLOCK_SYMS ) {
			write_block(&z, FALSE);
		}
	}
	write_block(&z, TRUE);
	flush_bits(&z);

	for ( i = 0; i < n; i++ ) {
		a += in[i];
		if ( a >= 65521U ) a -= 65521U;
		b += a;
		if ( b >= 65521U ) b -= 65521U;
	}
	out_byte(&z, (UB)( b >> 8 ));
	out_byte(&z, (UB)b);
	out_byte(&z, (UB)( a >> 8 ));
	out_byte(&z, (UB)a);

	Kfree(z.head);
	Kfree(z.prev);
	Kfree(z.sym);
	Kfree(z.cl);
	if ( z.fail ) {
		Kfree(z.out);
		return E_NOMEM;
	}
	*p_out = z.out;
	*p_len = z.at;

	return E_OK;
}
