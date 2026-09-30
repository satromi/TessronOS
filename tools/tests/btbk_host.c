/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btbk_host.c
 *	lib/libbtbk built for the host, for tools/tests/test_btbackup.py
 *
 *	  btbk_host compress IN OUT
 *	  btbk_host decompress IN OUT
 *	  btbk_host check [--loose] ARCHIVE...
 *	      Reads the volumes as a set with bk_rd_*, compresses every
 *	      object stream again (it must give the stored bytes), and
 *	      writes every volume again with bk_wr_* comparing each byte
 *	      written with the volume read. Prints one line of counts.
 *	  btbk_host tad XML [OUT] | tadq XML
 *	      Writes an xmlTAD as binary TAD with bk_tad_from_xml and walks
 *	      it with bk_tad_check; tadq says only what went wrong.
 *	  btbk_host rt TAD | rtarc ARCHIVE... | rtbpk BPK
 *	      Each TAD record (of a file, of the objects of a set of
 *	      volumes, or of a 書庫 of BPK) taken to xmlTAD with lib/libbpk,
 *	      written back as TAD and taken to xmlTAD again: the two must
 *	      be the same, their pictures pixel for pixel. With BTBK_BAD set
 *	      to a path, the xmlTAD of a record written badly is kept there.
 *
 *	cc -O2 -I include -o btbk_host tools/tests/btbk_host.c \
 *	   lib/libbtbk/btbk_*.c lib/libbpk/bpk_*.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/btbk.h>
#include <ts/bpk.h>

LOCAL UB *load( CONST char *path, long *p_len )
{
	FILE	*f = fopen(path, "rb");
	UB	*b;
	long	n;

	if ( f == NULL ) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(n > 0 ? n : 1);
	if ( b == NULL || (long)fread(b, 1, n, f) != n ) { perror(path); exit(2); }
	fclose(f);
	*p_len = n;
	return b;
}

LOCAL void save( CONST char *path, CONST UB *b, long n )
{
	FILE	*f = fopen(path, "wb");

	if ( f == NULL || (long)fwrite(b, 1, n, f) != n ) { perror(path); exit(2); }
	fclose(f);
}

typedef struct {
	UB	*p;
	long	n, max;
} GROW;

LOCAL INT grow_sink( void *arg, CONST UB *buf, INT len )
{
	GROW	*g = arg;

	if ( g->n + len > g->max ) {
		g->max = ( g->n + len ) * 2 + 4096;
		g->p = realloc(g->p, g->max);
		if ( g->p == NULL ) return E_NOMEM;
	}
	memcpy(g->p + g->n, buf, len);
	g->n += len;
	return E_OK;
}

LOCAL BK_ENC	enc;
LOCAL BK_DEC	dec;

LOCAL int do_compress( CONST char *in, CONST char *out )
{
	long	n;
	UB	*b = load(in, &n);
	GROW	g = { NULL, 0, 0 };
	long	i;

	bk_enc_init(&enc, grow_sink, &g);
	/* in odd pieces, which must not matter */
	for ( i = 0; i < n; i += 777 ) {
		if ( bk_enc_put(&enc, b + i, ( n - i > 777 ) ? 777 : (INT)( n - i )) < 0 ) return 1;
	}
	if ( bk_enc_flush(&enc) < 0 ) return 1;
	save(out, g.p, g.n);
	return 0;
}

typedef struct {
	CONST UB *p;
	long	n, at;
} MEM;

LOCAL INT mem_src( void *arg, UB *buf, INT len )
{
	MEM	*m = arg;

	if ( len > m->n - m->at ) len = (INT)( m->n - m->at );
	memcpy(buf, m->p + m->at, len);
	m->at += len;
	return len;
}

LOCAL int do_decompress( CONST char *in, CONST char *out )
{
	long	n;
	UB	*b = load(in, &n);
	MEM	m = { b, n, 0 };
	GROW	g = { NULL, 0, 0 };
	UB	tmp[1000];
	INT	k;

	bk_dec_init(&dec, mem_src, &m, (INT)n);
	while ( ( k = bk_dec_read(&dec, tmp, sizeof(tmp)) ) > 0 ) grow_sink(&g, tmp, k);
	if ( k < 0 ) return 1;
	save(out, g.p, g.n);
	return 0;
}

/* ------------------------------------------------------------------ */
/* check */

typedef struct {
	UH	type, subtype;
	INT	start;			/* offset of the first byte held */
	INT	size;			/* size of the whole record */
	long	at;			/* where its bytes are in `data` */
	BK_LINKDESC ld;
} REC;

LOCAL REC	*recs;
LOCAL INT	nrecs, maxrecs;
LOCAL GROW	data;
LOCAL INT	first_rec;

LOCAL ER src_info( void *arg, INT rec, UH *type, UH *subtype, INT *size )
{
	REC	*r;

	rec -= first_rec;
	if ( rec < 0 || rec >= nrecs ) return E_PAR;
	r = &recs[rec];
	*type = r->type;
	*subtype = r->subtype;
	*size = r->size;
	return E_OK;
}

LOCAL INT src_read( void *arg, INT rec, INT off, UB *buf, INT len )
{
	REC	*r;

	rec -= first_rec;
	if ( rec < 0 || rec >= nrecs ) return E_PAR;
	r = &recs[rec];
	if ( off < r->start || off + len > r->size ) return E_PAR;
	memcpy(buf, data.p + r->at + ( off - r->start ), len);
	return len;
}

LOCAL ER src_link( void *arg, INT rec, BK_LINKDESC *ld )
{
	rec -= first_rec;
	if ( rec < 0 || rec >= nrecs ) return E_PAR;
	*ld = recs[rec].ld;
	return E_OK;
}

typedef struct {
	CONST UB *orig;
	long	len;
	long	top;			/* bytes written so far */
	long	bad;
	UB	*out;			/* what was written, heads written again included */
} CMP;

LOCAL INT cmp_out( void *arg, UD off, CONST UB *buf, INT len )
{
	CMP	*c = arg;

	if ( (long)off + len > c->len ) {
		if ( c->bad < 0 ) c->bad = (long)off;
		return E_OK;
	}
	memcpy(c->out + off, buf, len);
	if ( (long)off + len > c->top ) c->top = (long)off + len;
	return E_OK;
}

/* the compressed bytes of one piece gathered while it is read */
LOCAL GROW	stream;
LOCAL MEM	vsrc;
LOCAL long	piece_at, piece_end;

LOCAL INT vol_src( void *arg, UB *buf, INT len )
{
	INT	n = mem_src(arg, buf, len);

	if ( n > 0 && vsrc.at - n >= piece_at && vsrc.at <= piece_end ) {
		grow_sink(&stream, buf, n);
	}
	return n;
}

LOCAL BK_RD	rd;
LOCAL BK_WR	wr;

LOCAL int do_check( int nvol, char **paths, BOOL loose )
{
	BK_FRAG	*frag = NULL;
	INT	nfrag = 0;
	long	nobj = 0, npiece = 0, nrec = 0, bad_re = 0, bad_wr = 0, skipped = 0;
	double	plain = 0;
	int	v;
	UB	tmp[4096];

	for ( v = 0; v < nvol; v++ ) {
		BK_VOLHEAD vh;
		BK_OBJHEAD oh;
		BK_FRAG	*f;
		long	len;
		UB	*vb = load(paths[v], &len);
		CMP	cmp = { vb, len, 0, -1, malloc(len + 1) };
		BOOL	whole = TRUE;
		INT	r;
		ER	er;

		vsrc.p = vb;
		vsrc.n = len;
		vsrc.at = 0;
		piece_at = piece_end = -1;
		if ( v == 0 ) {
			/* the table must hold every object of the set */
			rd.loose = loose;
			er = bk_rd_open(&rd, vol_src, &vsrc, &vh);
			nfrag = (INT)vh.nobj + 16;
			frag = calloc(nfrag, sizeof(BK_FRAG));
			bk_rd_init(&rd, frag, nfrag);
			rd.loose = loose;
			vsrc.at = 0;
		}
		er = bk_rd_open(&rd, vol_src, &vsrc, &vh);
		if ( er < E_OK ) { printf("volume %d: head %d\n", v + 1, er); return 1; }

		bk_wr_init(&wr, vh.total, vh.nobj, vh.src, vh.memo);
		wr.vol = vh.vol;
		bk_wr_begin(&wr, cmp_out, &cmp, 0x7FFFFFFFFFFFFFFFULL);

		for ( ;; ) {
			long	at = (long)vsrc.at;
			BK_RECHEAD rh;
			BK_OBJSRC os;

			stream.n = 0;
			piece_at = at + 16 + BK_META_SIZE;	/* after the head of the piece */
			piece_end = 0x7FFFFFFFFFFFFFFFL;
			r = bk_rd_object(&rd, &oh, &f);
			if ( r < 0 ) { printf("object at 0x%lX: %d\n", at, r); return 1; }
			if ( r == 0 ) break;
			if ( oh.seg == 0 ) nobj++;
			npiece++;
			piece_at = at + 8 + BK_OBJ_META;
			piece_end = piece_at + (long)oh.llen - BK_OBJ_META;

			nrecs = 0;
			data.n = 0;
			first_rec = -1;
			while ( ( r = bk_rd_record(&rd, &rh) ) > 0 ) {
				REC	*q;

				if ( nrecs > 0 && recs[nrecs - 1].type == rh.type
				     && first_rec + nrecs - 1 == rh.recno ) {
					q = &recs[nrecs - 1];	/* a later piece of the same record */
				} else {
					if ( nrecs == maxrecs ) {
						maxrecs = maxrecs * 2 + 64;
						recs = realloc(recs, maxrecs * sizeof(REC));
					}
					if ( first_rec < 0 ) first_rec = rh.recno;
					q = &recs[nrecs++];
					q->type = rh.type;
					q->subtype = rh.subtype;
					q->start = rh.offset;
					q->size = rh.offset;
					q->at = data.n;
				}
				if ( rh.type == 0 ) {
					er = bk_rd_link(&rd, &q->ld);
					if ( er < E_OK ) { printf("link: %d\n", er); return 1; }
				} else {
					INT	left = rh.len, k;

					while ( left > 0 ) {
						k = bk_rd_read(&rd, tmp, left > (INT)sizeof(tmp) ? (INT)sizeof(tmp) : left);
						if ( k <= 0 ) { printf("read: %d\n", k); return 1; }
						grow_sink(&data, tmp, k);
						left -= k;
					}
				}
				q->size += rh.len;
				nrec++;
			}
			if ( r < 0 ) {
				printf("object 0x%08X at 0x%lX: records %d\n", oh.objid, at, r);
				return 1;
			}
			plain += data.n;

			/* compressed again: the same bytes */
			if ( oh.cmp ) {
				GROW	g = { NULL, 0, 0 };
				INT	i;

				bk_enc_init(&enc, grow_sink, &g);
				for ( i = 0; i < nrecs; i++ ) {
					REC	*q = &recs[i];
					UB	h[16];
					INT	n = q->size - q->start;

					memset(h, 0, 16);
					h[0] = (UB)( first_rec + i ); h[1] = (UB)( ( first_rec + i ) >> 8 );
					h[2] = (UB)( ( first_rec + i ) >> 16 ); h[3] = (UB)( ( first_rec + i ) >> 24 );
					h[4] = (UB)q->type; h[5] = (UB)( q->type >> 8 );
					h[6] = (UB)q->subtype; h[7] = (UB)( q->subtype >> 8 );
					h[8] = (UB)q->start; h[9] = (UB)( q->start >> 8 );
					h[10] = (UB)( q->start >> 16 ); h[11] = (UB)( q->start >> 24 );
					h[12] = (UB)n; h[13] = (UB)( n >> 8 );
					h[14] = (UB)( n >> 16 ); h[15] = (UB)( n >> 24 );
					bk_enc_put(&enc, h, 16);
					if ( q->type == 0 ) {
						UB	lb[BK_LINK_SIZE];

						bk_link_put(lb, &q->ld);
						bk_enc_put(&enc, lb, BK_LINK_SIZE);
					} else {
						bk_enc_put(&enc, data.p + q->at, n);
					}
				}
				bk_enc_flush(&enc);
				if ( g.n != stream.n || memcmp(g.p, stream.p, g.n) != 0 ) {
					bad_re++;
					printf("0x%08X at 0x%lX: compressed again differs (%ld / %ld)\n",
					       oh.objid, at, g.n, stream.n);
				}
				free(g.p);
			}

			/* written again: only a piece whose records all end in it */
			if ( !( oh.seg & BK_MORE ) && nrecs >= 0 ) {
				os.nrec = oh.st.f_nrec;
				os.info = src_info;
				os.read = src_read;
				os.link = src_link;
				os.arg = NULL;
				if ( first_rec < 0 ) first_rec = oh.st.f_nrec;
				wr.segno = oh.seg & 0x7FFF;
				wr.resume_rec = first_rec;
				wr.resume_off = ( nrecs > 0 ) ? recs[0].start : 0;
				er = bk_wr_object(&wr, oh.objid, oh.meta, &os);
				if ( er != E_OK ) { printf("write 0x%08X: %d\n", oh.objid, er); return 1; }
			} else {
				whole = FALSE;
				skipped++;
			}
		}
		if ( whole ) {
			long	i;

			bk_wr_end(&wr, vh.more);
			for ( i = 0; cmp.bad < 0 && i < cmp.top; i++ ) {
				if ( cmp.out[i] != vb[i] ) cmp.bad = i;
			}
			if ( cmp.bad >= 0 || cmp.top != len ) {
				bad_wr++;
				printf("volume %d written again: differs at 0x%lX (%ld / %ld bytes)\n",
				       v + 1, cmp.bad, cmp.top, len);
			}
		}
		free(cmp.out);
		free(vb);
	}
	if ( nvol > 0 && !rd.more && bk_rd_close(&rd) < E_OK ) {
		printf("the set is not whole\n");
	}
	printf("objects %ld pieces %ld records %ld expanded %.0f recompress-differ %ld "
	       "rewrite-differ %ld not-rewritten %ld\n",
	       nobj, npiece, nrec, plain, bad_re, bad_wr, skipped);
	free(frag);
	return ( bad_re || bad_wr ) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* tad */

LOCAL BK_TADW	tadw;
LOCAL INT	vseq;
LOCAL BOOL	quiet;

/* the pictures the forward conversion gave, by their number */
#define PICS	4096
LOCAL UB	*pic[PICS];
LOCAL INT	piclen[PICS];
LOCAL UINT	*pixels;

LOCAL ER tad_link( void *arg, CONST BK_TADLINK *lk )
{
	if ( !quiet ) printf("link %d -> '%s' attr 0x%04X\n", lk->n, lk->target, lk->attr);
	return E_OK;
}

LOCAL const char *hk_target( void *ctx, INT n )
{
	static char	s[40];

	snprintf(s, sizeof(s), "00000000-0000-7000-8000-%012d", n);
	return s;
}

LOCAL void hk_vobjid( void *ctx, char *out37 )
{
	snprintf(out37, 37, "00000000-0000-7000-9000-%012d", ++vseq);
}

LOCAL ER hk_picture( void *ctx, INT recno, INT n, const UB *png, INT len )
{
	if ( n < 0 || n >= PICS ) return E_OK;
	free(pic[n]);
	pic[n] = malloc(len);
	memcpy(pic[n], png, len);
	piclen[n] = len;
	return E_OK;
}

LOCAL UINT be32( CONST UB *p )
{
	return ( (UINT)p[0] << 24 ) | ( (UINT)p[1] << 16 ) | ( (UINT)p[2] << 8 ) | p[3];
}

/* A PNG of stored deflate blocks (as lib/libbpk writes them) as pixels */
LOCAL ER png_px( CONST UB *png, INT len, INT *p_w, INT *p_h )
{
	INT	at = 8, w = 0, h = 0, ct = 0, bpp, x, y;
	GROW	z = { NULL, 0, 0 };
	CONST UB *d;
	long	k;

	while ( at + 12 <= len ) {
		UINT	n = be32(png + at);

		if ( memcmp(png + at + 4, "IHDR", 4) == 0 ) {
			w = (INT)be32(png + at + 8);
			h = (INT)be32(png + at + 12);
			ct = png[at + 17];
		} else if ( memcmp(png + at + 4, "IDAT", 4) == 0 ) {
			grow_sink(&z, png + at + 8, (INT)n);
		}
		at += 12 + (INT)n;
	}
	bpp = ( ct == 6 ) ? 4 : 3;
	if ( w <= 0 || h <= 0 || z.n < 2 ) return E_NOEXS;
	{
		GROW	raw = { NULL, 0, 0 };

		for ( k = 2; k + 5 <= z.n; ) {
			UINT	bl = z.p[k + 1] | ( z.p[k + 2] << 8 );
			BOOL	last = z.p[k] & 1;

			grow_sink(&raw, z.p + k + 5, (INT)bl);
			k += 5 + bl;
			if ( last ) break;
		}
		free(pixels);
		pixels = malloc(sizeof(UINT) * w * h);
		for ( y = 0; y < h; y++ ) {
			d = raw.p + (long)y * ( 1 + w * bpp ) + 1;
			for ( x = 0; x < w; x++, d += bpp ) {
				pixels[y * w + x] = ( bpp == 4 && d[3] < 128 ) ? BK_PX_CLEAR
						    : ( (UINT)d[0] << 16 ) | ( (UINT)d[1] << 8 ) | d[2];
			}
		}
		free(raw.p);
	}
	free(z.p);
	*p_w = w;
	*p_h = h;
	return E_OK;
}

/* Two PNGs of the same pixels, where a pixel not there is any pixel not there */
LOCAL BOOL same_pixels( CONST UB *a, INT alen, CONST UB *b, INT blen )
{
	INT	w1, h1, w2, h2, i;
	UINT	*p1;
	BOOL	ok;

	if ( png_px(a, alen, &w1, &h1) < E_OK ) return FALSE;
	p1 = pixels;
	pixels = NULL;
	if ( png_px(b, blen, &w2, &h2) < E_OK || w1 != w2 || h1 != h2 ) {
		free(p1);
		return FALSE;
	}
	ok = TRUE;
	for ( i = 0; i < w1 * h1 && ok; i++ ) ok = ( p1[i] == pixels[i] );
	if ( !ok ) printf("  pixel %d: %06X / %06X (%dx%d)\n", i - 1, p1[i - 1], pixels[i - 1], w1, h1);
	free(p1);
	return ok;
}

/* "self_0_3.png": the third picture */
LOCAL ER tad_image( void *arg, CONST UB *href, CONST UINT **p_px, INT *p_w, INT *p_h )
{
	CONST char *u = strrchr((CONST char *)href, '_');
	INT	n = ( u != NULL ) ? atoi(u + 1) : -1;
	ER	er;

	if ( n < 0 || n >= PICS || pic[n] == NULL ) return E_NOEXS;
	er = png_px(pic[n], piclen[n], p_w, p_h);
	*p_px = pixels;
	return er;
}

LOCAL CONST BK_TADHOOK tadhk = { tad_link, tad_image, NULL };

/* A binary TAD as xmlTAD, the links and pictures numbered from 0 */
LOCAL ER to_xml( CONST UB *tad, INT len, BPKBUF *xb )
{
	BPKHOOK	hk = { hk_target, hk_vobjid, hk_picture, NULL };
	BPKCONV	*cv = calloc(1, sizeof(BPKCONV));
	ER	er;

	vseq = 0;
	bpk_conv_init(cv, &hk);
	bpk_conv_object(cv);
	bpk_buf_init(xb);
	er = bpk_tad_to_xml(cv, "back", "self", 0, tad, (UINT)len, xb);
	free(cv);
	return er;
}

/* xmlTAD as a binary TAD in memory the caller frees */
LOCAL UB *to_tad( CONST UB *x, INT n, INT *p_len, BK_TADSTAT *st )
{
	INT	len = 0, len2 = 0;
	UB	*b;

	if ( bk_tad_from_xml(&tadw, x, n, NULL, 0, &len, &tadhk, st) < E_OK ) return NULL;
	b = malloc(len + 1);
	if ( bk_tad_from_xml(&tadw, x, n, b, len, &len2, &tadhk, st) < E_OK || len2 != len ) {
		free(b);
		return NULL;
	}
	*p_len = len;
	return b;
}

LOCAL int do_tad( CONST char *in, CONST char *out )
{
	long	n;
	UB	*x = load(in, &n), *b;
	INT	len = 0, nseg = 0, nchar = 0;
	BK_TADSTAT st;
	BPKBUF	xb;
	ER	er;

	b = to_tad(x, (INT)n, &len, &st);
	if ( b == NULL ) { printf("write failed\n"); return 1; }
	er = bk_tad_check(b, len, &nseg, &nchar);
	printf("bytes %d segments %d chars %d check %d links %d images %d/%d skipped %d unicode %d\n",
	       len, nseg, nchar, er, st.nlink, st.nimage, st.noimage, st.nskip, st.nuni);
	if ( out != NULL ) save(out, b, len);
	if ( to_xml(b, len, &xb) >= E_OK && !quiet ) fwrite(xb.s, 1, xb.n, stdout);
	bpk_buf_free(&xb);
	free(b);
	return ( er < E_OK ) ? 1 : 0;
}

/*
 * A TAD record taken to xmlTAD, back to TAD and to xmlTAD again: the two
 * xmlTAD the same. 0, or 1 and where they part.
 */
LOCAL long	rt_n, rt_bad, rt_bytes;

/* 240 bytes of xmlTAD on one line */
LOCAL void show( CONST char *head, CONST char *s, INT n )
{
	INT	i;

	fputs(head, stdout);
	for ( i = 0; i < n && i < 240; i++ ) putchar(( s[i] == '\n' ) ? '|' : s[i]);
	putchar('\n');
}

LOCAL int rt_one( const UB *tad, INT len, CONST char *what )
{
	BPKBUF	x1, x2;
	UB	*t2;
	INT	len2 = 0, i;
	BK_TADSTAT st;
	int	bad = 0;

	rt_n++;
	rt_bytes += len;
	for ( i = 0; i < PICS; i++ ) { free(pic[i]); pic[i] = NULL; }
	if ( to_xml((CONST UB *)(UBINT)tad, len, &x1) < E_OK ) {
		bpk_buf_free(&x1);
		return 0;			/* not TAD to begin with */
	}
	t2 = to_tad((CONST UB *)x1.s, x1.n, &len2, &st);
	if ( t2 == NULL || bk_tad_check(t2, len2, NULL, NULL) < E_OK ) {
		printf("%s: written TAD bad\n", what);
		if ( getenv("BTBK_BAD") != NULL ) {
			/* the xmlTAD that gave it, for a look */
			FILE	*f = fopen(getenv("BTBK_BAD"), "wb");

			if ( f != NULL ) { fwrite(x1.s, 1, x1.n, f); fclose(f); }
		}
		bad = 1;
	} else {
		/* the pictures of the second pass come from the first */
		UB	*keep[PICS];
		INT	keeplen[PICS];

		memcpy(keep, pic, sizeof(pic));
		memcpy(keeplen, piclen, sizeof(piclen));
		memset(pic, 0, sizeof(pic));
		(void)to_xml(t2, len2, &x2);
		for ( i = 0; i < PICS; i++ ) {
			if ( keep[i] != NULL && pic[i] != NULL && !same_pixels(keep[i], keeplen[i], pic[i], piclen[i]) ) {
				printf("%s: picture %d differs\n", what, i);
				bad = 1;
			}
			free(keep[i]);
		}
		if ( x1.n != x2.n || memcmp(x1.s, x2.s, x1.n) != 0 ) {
			INT	k = 0, from;

			while ( k < x1.n && k < x2.n && x1.s[k] == x2.s[k] ) k++;
			from = ( k > 120 ) ? k - 120 : 0;
			printf("%s: xmlTAD differs at %d\n", what, k);
			show("  1: ", x1.s + from, x1.n - from);
			show("  2: ", x2.s + from, x2.n - from);
			bad = 1;
		}
		bpk_buf_free(&x2);
	}
	free(t2);
	bpk_buf_free(&x1);
	rt_bad += bad;
	return bad;
}

LOCAL int do_rt_file( CONST char *in )
{
	long	n;
	UB	*b = load(in, &n);

	rt_one(b, (INT)n, in);
	free(b);
	printf("records %ld differ %ld\n", rt_n, rt_bad);
	return rt_bad ? 1 : 0;
}

/* Every TAD record (types 1-3) of a set of backup volumes */
LOCAL int do_rt_arc( int nvol, char **paths )
{
	INT	nfrag = 0, v, r;
	BK_FRAG	*frag = NULL;

	for ( v = 0; v < nvol; v++ ) {
		long	len;
		UB	*vb = load(paths[v], &len);
		BK_VOLHEAD vh;
		BK_OBJHEAD oh;
		BK_FRAG	*f;
		BK_RECHEAD rh;

		vsrc.p = vb;
		vsrc.n = len;
		vsrc.at = 0;
		piece_at = piece_end = -1;
		if ( v == 0 ) {
			rd.loose = TRUE;
			(void)bk_rd_open(&rd, vol_src, &vsrc, &vh);
			nfrag = (INT)vh.nobj + 16;
			frag = calloc(nfrag, sizeof(BK_FRAG));
			bk_rd_init(&rd, frag, nfrag);
			rd.loose = TRUE;
			vsrc.at = 0;
		}
		if ( bk_rd_open(&rd, vol_src, &vsrc, &vh) < E_OK ) return 1;
		while ( ( r = bk_rd_object(&rd, &oh, &f) ) > 0 ) {
			while ( bk_rd_record(&rd, &rh) > 0 ) {
				char	what[64];
				UB	*b;

				if ( rh.type < 1 || rh.type > 3 || rh.offset != 0 ) continue;
				b = malloc(rh.len + 2);
				if ( bk_rd_read(&rd, b, rh.len) != rh.len ) return 1;
				snprintf(what, sizeof(what), "0x%08X rec %d", oh.objid, rh.recno);
				rt_one(b, rh.len & ~1, what);
				free(b);
			}
		}
		free(vb);
	}
	printf("records %ld (%ld bytes) differ %ld\n", rt_n, rt_bytes, rt_bad);
	return rt_bad ? 1 : 0;
}

/* Every TAD record of a BTRON archive (書庫) */
LOCAL int do_rt_bpk( CONST char *in )
{
	long	n;
	UB	*b = load(in, &n);
	BPKARC	a;
	char	err[160];
	UINT	i;
	INT	r;

	if ( bpk_parse(&a, b, (UINT)n, err, sizeof(err)) < E_OK ) {
		printf("%s\n", err);
		return 1;
	}
	for ( i = 0; i < a.nfiles; i++ ) {
		for ( r = 0; r < a.file[i].nrec; r++ ) {
			char	what[64];

			if ( a.file[i].rec[r].type != BPK_RT_TAD ) continue;
			snprintf(what, sizeof(what), "file %u rec %d", i, r);
			rt_one(a.file[i].rec[r].data, (INT)a.file[i].rec[r].size & ~1, what);
		}
	}
	bpk_free(&a);
	printf("records %ld (%ld bytes) differ %ld\n", rt_n, rt_bytes, rt_bad);
	return rt_bad ? 1 : 0;
}

int main( int argc, char **argv )
{
	if ( argc >= 3 && strcmp(argv[1], "tad") == 0 ) return do_tad(argv[2], ( argc > 3 ) ? argv[3] : NULL);
	if ( argc >= 3 && strcmp(argv[1], "tadq") == 0 ) {
		quiet = TRUE;
		return do_tad(argv[2], NULL);
	}
	if ( argc == 3 && strcmp(argv[1], "rt") == 0 ) return do_rt_file(argv[2]);
	if ( argc >= 3 && strcmp(argv[1], "rtarc") == 0 ) {
		quiet = TRUE;
		return do_rt_arc(argc - 2, argv + 2);
	}
	if ( argc == 3 && strcmp(argv[1], "rtbpk") == 0 ) {
		quiet = TRUE;
		return do_rt_bpk(argv[2]);
	}
	if ( argc == 4 && strcmp(argv[1], "compress") == 0 ) return do_compress(argv[2], argv[3]);
	if ( argc == 4 && strcmp(argv[1], "decompress") == 0 ) return do_decompress(argv[2], argv[3]);
	if ( argc >= 4 && strcmp(argv[1], "check") == 0 && strcmp(argv[2], "--loose") == 0 ) {
		return do_check(argc - 3, argv + 3, TRUE);
	}
	if ( argc >= 3 && strcmp(argv[1], "check") == 0 ) return do_check(argc - 2, argv + 2, FALSE);
	fprintf(stderr, "usage: btbk_host compress|decompress IN OUT | check ARCHIVE...\n");
	return 2;
}
