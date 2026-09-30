/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_btbk.c
 *	Backup archives (design 11.19): lib/libbtbk in the kernel
 *
 *	The LZSS codec on the tokens it must make and on sizes around its
 *	window; a real backup volume of four cabinets read, its values
 *	checked, every object stream compressed again to the same bytes and
 *	the whole volume written again byte for byte; the same objects cut
 *	into volumes and read back as a set; and records of etc/xtad written
 *	as binary TAD and walked segment by segment.
 */

#include "ktest.h"
#include <ts/btbk.h>

IMPORT CONST UB kt_bk_vol[], kt_bk_vol_end[];
IMPORT CONST UB kt_bk_fig[], kt_bk_fig_end[];
IMPORT CONST UB kt_bk_box[], kt_bk_box_end[];
IMPORT CONST UB kt_bk_doc[], kt_bk_doc_end[];

#define VOL_LEN		1671
#define NOBJ		4		/* in the volume */
#define OBJMAX		5		/* and one made here */
#define BIGREC		3000
#define BIG		0x3000		/* bytes of the codec's test data */

LOCAL BK_ENC	*enc;
LOCAL BK_DEC	*dec;

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL UINT rd16( CONST UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL UINT rd32( CONST UB *p )
{
	return rd16(p) | ( rd16(p + 2) << 16 );
}

/* ------------------------------------------------------------------ */
/* LZSS */

/* inputs and the streams the format has for them */
LOCAL void test_tokens( void )
{
	UB	in[300], out[400], back[300];
	INT	i, n;
	static CONST UB a1[] = { 0x00, 0x41 };
	static CONST UB a3[] = { 0x02, 0x41, 0x41, 0x41 };
	static CONST UB a16[] = { 0x00, 0x41, 0xE0, 0x01 };
	static CONST UB a17[] = { 0x00, 0x41, 0xF0, 0xF0, 0x01 };
	static CONST UB a257[] = { 0x00, 0x41, 0xFF, 0xF0, 0x01 };
	static CONST UB a258[] = { 0x00, 0x41, 0xFF, 0xF0, 0x01, 0x00, 0x41 };
	static CONST UB a300[] = { 0x00, 0x41, 0xFF, 0xF0, 0x01, 0xF2, 0xA1, 0x01 };
	static CONST UB abcd[] = { 0x04, 0x61, 0x62, 0x63, 0x64, 0x58, 0x30, 0x05 };
	static CONST struct { INT len; CONST UB *s; INT slen; } as[] = {
		{ 1, a1, 2 }, { 3, a3, 4 }, { 16, a16, 4 }, { 17, a17, 5 },
		{ 257, a257, 5 }, { 258, a258, 7 }, { 300, a300, 8 },
	};

	enc = Kmalloc(sizeof(BK_ENC));
	dec = Kmalloc(sizeof(BK_DEC));
	KT_ASSERT(enc != NULL && dec != NULL);
	if ( enc == NULL || dec == NULL ) return;

	KT_ASSERT_EQ(bk_compress(enc, in, 0, out, sizeof(out)), 0);	/* nothing: nothing */

	for ( i = 0; i < 300; i++ ) in[i] = 'A';
	for ( i = 0; i < (INT)( sizeof(as) / sizeof(as[0]) ); i++ ) {
		n = bk_compress(enc, in, as[i].len, out, sizeof(out));
		KT_ASSERT_EQ(n, as[i].slen);
		KT_ASSERT(n == as[i].slen && same_bytes(out, as[i].s, n));
		KT_ASSERT_EQ(bk_decompress(dec, out, n, back, sizeof(back)), as[i].len);
		KT_ASSERT(same_bytes(back, in, as[i].len));
	}

	/* 32 literals are one block, 33 are two */
	for ( i = 0; i < 33; i++ ) in[i] = (UB)i;
	n = bk_compress(enc, in, 32, out, sizeof(out));
	KT_ASSERT_EQ(n, 33);
	KT_ASSERT_EQ(out[0], 0x1F);
	n = bk_compress(enc, in, 33, out, sizeof(out));
	KT_ASSERT_EQ(n, 35);
	KT_ASSERT_EQ(out[33], 0x00);
	KT_ASSERT_EQ(out[34], 0x20);

	in[0] = 'a'; in[1] = 'b'; in[2] = 'c'; in[3] = 'd'; in[4] = 'X';
	in[5] = 'a'; in[6] = 'b'; in[7] = 'c'; in[8] = 'd';
	n = bk_compress(enc, in, 9, out, sizeof(out));
	KT_ASSERT(n == (INT)sizeof(abcd) && same_bytes(out, abcd, n));
}

/* sizes around the window and the ring, in several kinds of data: back
   the same, the distances in 1..4094, and the output not depending on
   the pieces the input came in */
LOCAL INT	pieces_n;
LOCAL UB	*pieces_out;

LOCAL INT piece_sink( void *arg, CONST UB *buf, INT len )
{
	INT	i;

	for ( i = 0; i < len; i++ ) pieces_out[pieces_n++] = buf[i];
	return E_OK;
}

LOCAL BOOL tokens_ok( CONST UB *s, INT n )
{
	INT	i = 0;

	while ( i < n ) {
		UINT	b0 = s[i];

		if ( ( b0 & 0xE0 ) == 0 ) {
			i += b0 + 2;
		} else {
			UINT	w, dist, ln;

			if ( b0 <= 0xEF ) {
				w = ( b0 << 8 ) | s[i + 1];
				ln = ( w >> 12 ) & 0xF;
				i += 2;
			} else {
				w = ( (UINT)s[i + 1] << 8 ) | s[i + 2];
				ln = ( ( b0 & 0xF ) << 4 ) + ( ( w >> 12 ) & 0xF );
				if ( ln < 15 ) return FALSE;
				i += 3;
			}
			dist = w & 0xFFF;
			if ( dist < 1 || dist > 4094 || ln < 2 ) return FALSE;
		}
	}
	return (BOOL)( i == n );
}

LOCAL void test_sizes( void )
{
	static CONST INT sizes[] = { 2, 4, 5, 15, 31, 32, 33, 255, 256, 257, 258,
				     4093, 4094, 4095, 4096, 4097, 8191, 8192, 8193, BIG };
	UB	*in = Kmalloc(BIG), *out = Kmalloc(BIG * 2), *back = Kmalloc(BIG), *alt = Kmalloc(BIG * 2);
	INT	s, k, i, n, m, bad = 0, badtok = 0;
	UINT	seed = 12345;

	KT_ASSERT(in != NULL && out != NULL && back != NULL && alt != NULL && enc != NULL);
	if ( in == NULL || out == NULL || back == NULL || alt == NULL || enc == NULL ) goto done;

	for ( k = 0; k < 4; k++ ) {
		for ( i = 0; i < BIG; i++ ) {
			seed = seed * 1103515245U + 12345U;
			in[i] = ( k == 0 ) ? 0
			      : ( k == 1 ) ? (UB)( seed >> 16 )
			      : ( k == 2 ) ? (UB)( "abcab"[i % 5] )
			      : (UB)( ( seed >> 20 ) & 3 );
		}
		for ( s = 0; s < (INT)( sizeof(sizes) / sizeof(sizes[0]) ); s++ ) {
			n = bk_compress(enc, in, sizes[s], out, BIG * 2);
			m = ( n > 0 ) ? bk_decompress(dec, out, n, back, BIG) : -1;
			if ( m != sizes[s] || !same_bytes(back, in, sizes[s]) ) bad++;
			if ( n > 0 && !tokens_ok(out, n) ) badtok++;

			/* the same input put a few bytes at a time */
			pieces_out = alt;
			pieces_n = 0;
			bk_enc_init(enc, piece_sink, NULL);
			for ( i = 0; i < sizes[s]; i += 7 ) {
				(void)bk_enc_put(enc, in + i, ( sizes[s] - i > 7 ) ? 7 : sizes[s] - i);
			}
			(void)bk_enc_flush(enc);
			if ( pieces_n != n || !same_bytes(alt, out, n) ) bad++;
		}
	}
	KT_ASSERT_EQ(bad, 0);
	KT_ASSERT_EQ(badtok, 0);
done:
	if ( in != NULL ) Kfree(in);
	if ( out != NULL ) Kfree(out);
	if ( back != NULL ) Kfree(back);
	if ( alt != NULL ) Kfree(alt);
}

/* ------------------------------------------------------------------ */
/* The volume */

typedef struct {
	CONST UB *p;
	INT	n, at;
} SRC;

LOCAL INT src_read( void *arg, UB *buf, INT len )
{
	SRC	*s = arg;
	INT	i;

	if ( len > s->n - s->at ) len = s->n - s->at;
	for ( i = 0; i < len; i++ ) buf[i] = s->p[s->at + i];
	s->at += len;
	return len;
}

/* one object as read: its head, and its records whole */
#define MAXREC		6
#define MAXDATA		4096

typedef struct {
	BK_OBJHEAD oh;
	UINT	pos;			/* where its segment starts in the volume */
	INT	nrec;
	UH	type[MAXREC], sub[MAXREC];
	INT	size[MAXREC];
	BK_LINKDESC ld[MAXREC];
	UB	data[MAXREC][MAXDATA];
} OBJ;

LOCAL OBJ	*objs;
LOCAL BK_RD	*rd;
LOCAL BK_WR	*wr;
LOCAL BK_FRAG	frag[16];

/* A set of volumes read into objs[]: how many objects, or an error */
LOCAL INT read_set( CONST UB *const *vol, CONST INT *len, INT nvol, BK_VOLHEAD *vh0 )
{
	INT	v, nobj = 0, r;
	ER	er;

	bk_rd_init(rd, frag, 16);
	for ( v = 0; v < nvol; v++ ) {
		SRC		s = { vol[v], len[v], 0 };
		BK_VOLHEAD	vh;

		er = bk_rd_open(rd, src_read, &s, &vh);
		if ( er < E_OK ) return er;
		if ( v == 0 && vh0 != NULL ) *vh0 = vh;
		for ( ;; ) {
			UINT		at = (UINT)s.at;
			BK_OBJHEAD	oh;
			BK_FRAG		*f;
			BK_RECHEAD	rh;
			OBJ		*o;

			r = bk_rd_object(rd, &oh, &f);
			if ( r < 0 ) return r;
			if ( r == 0 ) break;
			if ( ( oh.seg & 0x7FFF ) == 0 ) {
				if ( nobj >= OBJMAX ) return E_LIMIT;
				o = &objs[nobj++];
				o->oh = oh;
				o->pos = at;
				o->nrec = 0;
				f->user = o;
			} else {
				o = f->user;
			}
			while ( ( r = bk_rd_record(rd, &rh) ) > 0 ) {
				INT	i = rh.recno;

				if ( i >= MAXREC ) return E_LIMIT;
				if ( rh.offset == 0 ) {
					o->nrec = i + 1;
					o->type[i] = rh.type;
					o->sub[i] = rh.subtype;
					o->size[i] = 0;
				}
				if ( rh.type == 0 ) {
					er = bk_rd_link(rd, &o->ld[i]);
					if ( er < E_OK ) return er;
					o->size[i] = BK_LINK_SIZE;
				} else {
					if ( rh.offset + rh.len > MAXDATA ) return E_LIMIT;
					if ( bk_rd_read(rd, o->data[i] + rh.offset, rh.len) != rh.len ) return E_OBJ;
					o->size[i] = rh.offset + rh.len;
				}
			}
			if ( r < 0 ) return r;
		}
	}
	er = bk_rd_close(rd);
	return ( er < E_OK ) ? er : nobj;
}

/* what the volume holds, as the format's description of it gives it */
LOCAL void test_read( void )
{
	CONST UB *vol[1] = { kt_bk_vol };
	INT	len[1] = { (INT)( kt_bk_vol_end - kt_bk_vol ) };
	BK_VOLHEAD vh;
	INT	n;
	static CONST UINT objid[NOBJ] = { 0x18B40001, 0x18C80001, 0x18C70001, 0x18C60001 };
	static CONST UINT llen[NOBJ] = { 412, 363, 331, 331 };
	static CONST INT nrec[NOBJ] = { 5, 4, 3, 3 }, nlink[NOBJ] = { 3, 2, 1, 1 };
	static CONST INT fsize[NOBJ] = { 368, 302, 236, 236 };
	static CONST UINT pos[NOBJ] = { 0x0CA, 0x26E, 0x3E1, 0x534 };
	INT	i;

	objs = Kmalloc(sizeof(OBJ) * OBJMAX);
	rd = Kmalloc(sizeof(BK_RD));
	wr = Kmalloc(sizeof(BK_WR));
	KT_ASSERT(objs != NULL && rd != NULL && wr != NULL);
	if ( objs == NULL || rd == NULL || wr == NULL ) return;
	KT_ASSERT_EQ(len[0], VOL_LEN);

	n = read_set(vol, len, 1, &vh);
	KT_ASSERT_EQ(n, NOBJ);
	KT_ASSERT_EQ(vh.kind, BK_KIND_VOL);
	KT_ASSERT_EQ(vh.vol, 0);
	KT_ASSERT(!vh.more);
	KT_ASSERT_EQ(vh.total, 3012);
	KT_ASSERT_EQ(vh.nobj, 4);
	KT_ASSERT_EQ(vh.src, 262144);
	KT_ASSERT_EQ(vh.memo[0], 0x2331);		/* "１０１８キャビネットデモ" */
	KT_ASSERT_EQ(vh.memo[4], 0x252D);
	if ( n != NOBJ ) return;

	for ( i = 0; i < NOBJ; i++ ) {
		OBJ	*o = &objs[i];
		INT	r, links = 0, bytes = 0;

		KT_ASSERT_EQ(o->oh.objid, objid[i]);
		KT_ASSERT_EQ(o->oh.llen, llen[i]);
		KT_ASSERT_EQ(o->oh.seg, 0);
		KT_ASSERT_EQ(o->oh.cmp, 1);
		KT_ASSERT_EQ(o->pos, pos[i]);
		KT_ASSERT_EQ(o->oh.st.f_type, 0x1000);
		KT_ASSERT_EQ(o->oh.st.f_atype, 6);		/* cabinets */
		KT_ASSERT_EQ(o->oh.st.f_pubacc, 0x0FFF);
		KT_ASSERT_EQ(o->oh.st.f_nrec, nrec[i]);
		KT_ASSERT_EQ(o->oh.st.f_nlink, nlink[i]);
		KT_ASSERT_EQ(o->oh.st.f_size, fsize[i]);
		KT_ASSERT_EQ(o->oh.st.f_nblk, 2);
		KT_ASSERT_EQ(o->oh.st.f_ltime, 0xFFFFFFFFU);
		KT_ASSERT_EQ(o->nrec, nrec[i]);
		KT_ASSERT_EQ(o->type[0], 8);			/* 実行機能付箋 first */
		KT_ASSERT_EQ(o->type[o->nrec - 1], 1);		/* the TAD last */
		for ( r = 0; r < o->nrec; r++ ) {
			if ( o->type[r] == 0 ) {
				links++;
				/* every link is to another object of the set */
				KT_ASSERT(o->ld[r].objid == objid[0] || o->ld[r].objid == objid[1]
					  || o->ld[r].objid == objid[2] || o->ld[r].objid == objid[3]);
				KT_ASSERT_EQ(o->ld[r].f_atype, 6);
				KT_ASSERT_EQ(o->ld[r].f_id, 0);
			} else {
				bytes += o->size[r];
			}
		}
		KT_ASSERT_EQ(links, nlink[i]);
		KT_ASSERT_EQ(bytes, fsize[i]);			/* f_size leaves the links out */
	}
	KT_ASSERT_EQ(objs[0].oh.name[0], 0x2331);
	KT_ASSERT_EQ(objs[0].oh.st.f_mtime, 0x4CB01A1CU);
	KT_ASSERT_EQ(objs[0].ld[1].objid, 0x18C60001U);	/* → キャビネット */
	KT_ASSERT_EQ(objs[0].ld[1].atr[0], 0x0008);
	KT_ASSERT_EQ(objs[0].ld[1].atr[2], 0x8000);
	KT_ASSERT_EQ(objs[0].ld[1].atr[3], 0x0001);
	KT_ASSERT_EQ(objs[3].ld[1].objid, 0x18C70001U);	/* キャビネット → キャビネット２ */
	KT_ASSERT_EQ(objs[1].size[3], 174);
}

/* the object streams compressed again: the stored bytes */
typedef struct {
	CONST UB *ref;
	INT	n, bad;
} CMP;

LOCAL INT cmp_sink( void *arg, CONST UB *buf, INT len )
{
	CMP	*c = arg;
	INT	i;

	for ( i = 0; i < len; i++ ) {
		if ( c->n + i >= 0x7FFF || buf[i] != c->ref[c->n + i] ) c->bad++;
	}
	c->n += len;
	return E_OK;
}

LOCAL void put_rec_head( UB *h, INT recno, UINT type, UINT sub, INT off, INT len )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		h[i] = (UB)( recno >> ( 8 * i ) );
		h[8 + i] = (UB)( off >> ( 8 * i ) );
		h[12 + i] = (UB)( len >> ( 8 * i ) );
	}
	h[4] = (UB)type;  h[5] = (UB)( type >> 8 );
	h[6] = (UB)sub;   h[7] = (UB)( sub >> 8 );
}

LOCAL void test_recompress( void )
{
	INT	i, r;

	if ( objs == NULL || enc == NULL ) return;
	for ( i = 0; i < NOBJ; i++ ) {
		OBJ	*o = &objs[i];
		CMP	c = { kt_bk_vol + o->pos + 8 + BK_OBJ_META, 0, 0 };
		UB	h[BK_REC_HEAD], lb[BK_LINK_SIZE];

		bk_enc_init(enc, cmp_sink, &c);
		for ( r = 0; r < o->nrec; r++ ) {
			put_rec_head(h, r, o->type[r], o->sub[r], 0, o->size[r]);
			(void)bk_enc_put(enc, h, BK_REC_HEAD);
			if ( o->type[r] == 0 ) {
				bk_link_put(lb, &o->ld[r]);
				(void)bk_enc_put(enc, lb, BK_LINK_SIZE);
			} else {
				(void)bk_enc_put(enc, o->data[r], o->size[r]);
			}
		}
		(void)bk_enc_flush(enc);
		KT_ASSERT_EQ(c.n, (INT)o->oh.llen - BK_OBJ_META);
		KT_ASSERT_EQ(c.bad, 0);
	}
}

/* the source the writer takes an object's records from */
LOCAL ER os_info( void *arg, INT rec, UH *type, UH *subtype, INT *size )
{
	OBJ	*o = arg;

	if ( rec < 0 || rec >= o->nrec ) return E_PAR;
	*type = o->type[rec];
	*subtype = o->sub[rec];
	*size = o->size[rec];
	return E_OK;
}

LOCAL INT os_read( void *arg, INT rec, INT off, UB *buf, INT len )
{
	OBJ	*o = arg;
	INT	i;

	if ( rec < 0 || rec >= o->nrec || off < 0 || off + len > o->size[rec] ) return E_PAR;
	for ( i = 0; i < len; i++ ) buf[i] = o->data[rec][off + i];
	return len;
}

LOCAL ER os_link( void *arg, INT rec, BK_LINKDESC *ld )
{
	OBJ	*o = arg;

	*ld = o->ld[rec];
	return E_OK;
}

#define OUT_MAX		4096
#define NVOL_MAX	8

typedef struct {
	UB	b[NVOL_MAX][OUT_MAX];
	INT	len[NVOL_MAX];
	INT	cur;
} OUTS;

LOCAL OUTS	*outs;

LOCAL INT out_write( void *arg, UD off, CONST UB *buf, INT len )
{
	OUTS	*o = arg;
	INT	i;

	if ( off + len > OUT_MAX ) return E_LIMIT;
	for ( i = 0; i < len; i++ ) o->b[o->cur][off + i] = buf[i];
	if ( (INT)off + len > o->len[o->cur] ) o->len[o->cur] = (INT)off + len;
	return E_OK;
}

/* The first n objects written as volumes of `cap` bytes: how many volumes */
LOCAL INT write_set( INT n, UD cap, CONST BK_VOLHEAD *vh )
{
	INT	i;
	ER	er;

	outs->cur = 0;
	for ( i = 0; i < NVOL_MAX; i++ ) outs->len[i] = 0;
	bk_wr_init(wr, vh->total, vh->nobj, vh->src, vh->memo);
	if ( bk_wr_begin(wr, out_write, outs, cap) < E_OK ) return E_IO;
	for ( i = 0; i < n; i++ ) {
		BK_OBJSRC os = { objs[i].nrec, os_info, os_read, os_link, &objs[i] };

		while ( ( er = bk_wr_object(wr, objs[i].oh.objid, objs[i].oh.meta, &os) ) == BK_FULL ) {
			if ( bk_wr_end(wr, TRUE) < E_OK || outs->cur + 1 >= NVOL_MAX ) return E_LIMIT;
			outs->cur++;
			if ( bk_wr_begin(wr, out_write, outs, cap) < E_OK ) return E_IO;
		}
		if ( er < E_OK ) return er;
	}
	if ( bk_wr_end(wr, FALSE) < E_OK ) return E_IO;
	return outs->cur + 1;
}

/* written again from what was read: the same volume */
LOCAL void test_rewrite( void )
{
	BK_VOLHEAD vh;
	CONST UB *vol[1] = { kt_bk_vol };
	INT	len[1] = { VOL_LEN };
	UD	total = 0, src = 0;
	INT	i;

	if ( objs == NULL ) return;
	outs = Kmalloc(sizeof(OUTS));
	KT_ASSERT(outs != NULL);
	if ( outs == NULL ) return;
	KT_ASSERT_EQ(read_set(vol, len, 1, &vh), NOBJ);

	/* the totals of the head are what the objects add up to on 32 KB blocks */
	for ( i = 0; i < NOBJ; i++ ) bk_estimate(&objs[i].oh.st, 32768, &total, &src);
	KT_ASSERT_EQ(total, vh.total);
	KT_ASSERT_EQ(src, vh.src);

	KT_ASSERT_EQ(write_set(NOBJ, 0x60000000, &vh), 1);
	KT_ASSERT_EQ(outs->len[0], VOL_LEN);
	KT_ASSERT(same_bytes(outs->b[0], kt_bk_vol, VOL_LEN));
}

/*
 * With an object of a record bigger than a volume, cut into small
 * volumes and read back as a set: the same records. The room is counted
 * in bytes already compressed, so a volume can go past its capacity by
 * what the encoder still holds; the format keeps a reserve for that.
 */
LOCAL void test_volumes( void )
{
	BK_VOLHEAD vh;
	CONST UB *vol[NVOL_MAX];
	INT	len[NVOL_MAX], nvol, i, r, bad = 0, split = 0;
	UINT	seed = 12345;
	OBJ	*keep, *o;

	if ( objs == NULL || outs == NULL ) return;
	keep = Kmalloc(sizeof(OBJ) * OBJMAX);
	KT_ASSERT(keep != NULL);
	if ( keep == NULL ) return;
	vol[0] = kt_bk_vol;
	len[0] = VOL_LEN;
	KT_ASSERT_EQ(read_set(vol, len, 1, &vh), NOBJ);

	/* the fifth: a 実行機能付箋 and 3000 bytes that do not compress */
	o = &objs[NOBJ];
	*o = objs[NOBJ - 1];
	o->oh.objid = 0x12340001;
	o->nrec = 2;
	o->type[1] = 1;
	o->sub[1] = 0;
	o->size[1] = BIGREC;
	for ( i = 0; i < BIGREC; i++ ) {
		seed = seed * 1103515245U + 12345U;
		o->data[1][i] = (UB)( seed >> 16 );
	}
	o->oh.st.f_nrec = 2;
	o->oh.st.f_nlink = 0;
	o->oh.st.f_size = o->size[0] + BIGREC;
	bk_meta_put(o->oh.meta, o->oh.name, &o->oh.st);
	for ( i = 0; i < (INT)( sizeof(OBJ) * OBJMAX ); i++ ) ((UB *)keep)[i] = ((UB *)objs)[i];

	nvol = write_set(OBJMAX, 1600, &vh);
	KT_ASSERT_EQ(nvol, 4);
	if ( nvol < 2 ) goto done;
	for ( i = 0; i < nvol; i++ ) {
		vol[i] = outs->b[i];
		len[i] = outs->len[i];
		KT_ASSERT_EQ(rd16(outs->b[i] + 16) & 0x7FFF, (UINT)i);		/* vol */
		KT_ASSERT_EQ(( rd16(outs->b[i] + 16) & BK_MORE ) != 0, i + 1 < nvol);
	}
	/* a piece that goes on is marked, and the one after it numbered */
	for ( i = 0; i + 1 < nvol; i++ ) {
		UINT	at = 202, last = 0;

		while ( at + 8 <= (UINT)len[i] ) {
			last = at;
			at += 8 + rd32(outs->b[i] + at + 4);
		}
		if ( rd16(outs->b[i] + last + 10) & BK_MORE ) split++;
	}
	KT_ASSERT(split > 0);

	KT_ASSERT_EQ(read_set(vol, len, nvol, NULL), OBJMAX);
	for ( i = 0; i < OBJMAX; i++ ) {
		if ( objs[i].nrec != keep[i].nrec || objs[i].oh.objid != keep[i].oh.objid ) bad++;
		for ( r = 0; r < keep[i].nrec && r < objs[i].nrec; r++ ) {
			if ( objs[i].size[r] != keep[i].size[r] || objs[i].type[r] != keep[i].type[r] ) bad++;
			else if ( keep[i].type[r] != 0
				  && !same_bytes(objs[i].data[r], keep[i].data[r], keep[i].size[r]) ) bad++;
			else if ( keep[i].type[r] == 0 && objs[i].ld[r].objid != keep[i].ld[r].objid ) bad++;
		}
	}
	KT_ASSERT_EQ(bad, 0);

	/* a later volume alone is not a set */
	KT_ASSERT(read_set(vol + 1, len + 1, 1, NULL) < E_OK);
done:
	Kfree(keep);
}

/* ------------------------------------------------------------------ */
/* xmlTAD to binary TAD */

LOCAL INT	nlinks;
LOCAL UB	targets[4][40];

LOCAL ER on_link( void *arg, CONST BK_TADLINK *lk )
{
	INT	i;

	if ( lk->n != nlinks ) return E_PAR;		/* told in order */
	if ( nlinks < 4 ) {
		for ( i = 0; i < 40; i++ ) targets[nlinks][i] = lk->target[i];
	}
	nlinks++;
	return E_OK;
}

LOCAL CONST BK_TADHOOK link_hook = { on_link, NULL, NULL };

typedef struct {
	INT	nseg, nchar, nvobj, ntext, nfig, nprim, npat, depth, maxdepth;
	BOOL	bad;
} WALK;

/* The record walked word by word: segments land where their lengths say */
LOCAL void walk( CONST UB *s, INT len, WALK *wk )
{
	INT	at = 0;
	UINT	id, body;

	wk->nseg = wk->nchar = wk->nvobj = wk->ntext = wk->nfig = 0;
	wk->nprim = wk->npat = wk->depth = wk->maxdepth = 0;
	wk->bad = FALSE;
	while ( at < len ) {
		if ( at + 2 > len ) { wk->bad = TRUE; return; }
		id = rd16(s + at);
		if ( id < 0xFF80 ) {
			wk->nchar++;
			at += 2;
			continue;
		}
		if ( wk->nseg == 0 && id != 0xFFE0 ) wk->bad = TRUE;
		body = rd16(s + at + 2);
		at += 4;
		if ( body == 0xFFFF ) {
			body = rd32(s + at);
			at += 4;
		}
		if ( at + (INT)body > len ) { wk->bad = TRUE; return; }
		switch ( id ) {
		case 0xFFE1: wk->ntext++; wk->depth++; break;
		case 0xFFE3: wk->nfig++; wk->depth++; break;
		case 0xFFE2: case 0xFFE4: wk->depth--; break;
		case 0xFFE6: wk->nvobj++; if ( body != 30 ) wk->bad = TRUE; break;
		case 0xFFB0: wk->nprim++; break;
		case 0xFFB1: wk->npat++; break;
		}
		if ( ( id == 0xFFE1 || id == 0xFFE3 ) && body != 24 ) wk->bad = TRUE;
		if ( ( id == 0xFFE2 || id == 0xFFE4 ) && body != 0 ) wk->bad = TRUE;
		if ( wk->depth < 0 ) wk->bad = TRUE;
		if ( wk->depth > wk->maxdepth ) wk->maxdepth = wk->depth;
		at += (INT)body;
		wk->nseg++;
	}
	if ( at != len || wk->depth != 0 ) wk->bad = TRUE;
}

LOCAL UB *to_tad( CONST UB *x, INT n, INT *p_len, BK_TADSTAT *st )
{
	BK_TADW	*w = Kmalloc(sizeof(BK_TADW));
	INT	len = 0, len2 = 0;
	UB	*b = NULL;

	*p_len = 0;
	if ( w == NULL ) return NULL;
	nlinks = 0;
	if ( bk_tad_from_xml(w, x, n, NULL, 0, &len, NULL, st) >= E_OK && len > 0 ) {
		b = Kmalloc(len);
		if ( b != NULL
		  && ( bk_tad_from_xml(w, x, n, b, len, &len2, &link_hook, st) < E_OK
		       || len2 != len ) ) {
			Kfree(b);
			b = NULL;
		}
	}
	Kfree(w);
	*p_len = len;
	return b;
}

LOCAL BOOL str_is( CONST UB *s, CONST char *w )
{
	INT	i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( s[i] != (UB)w[i] ) return FALSE;
	}
	return (BOOL)( s[i] == 0 );
}

/* a figure with links, a line with an arrow, texts in it and a picture */
LOCAL void test_tad_fig( void )
{
	BK_TADSTAT st;
	WALK	wk;
	INT	len, nseg = 0, nchar = 0;
	UB	*b = to_tad(kt_bk_fig, (INT)( kt_bk_fig_end - kt_bk_fig ), &len, &st);

	KT_ASSERT(b != NULL);
	if ( b == NULL ) return;
	walk(b, len, &wk);
	KT_ASSERT(!wk.bad);
	KT_ASSERT_EQ(rd16(b + 10), 0xFFE3);		/* the record is a figure */
	KT_ASSERT_EQ(wk.nfig, 1);
	KT_ASSERT_EQ(wk.ntext, 2);			/* the two texts in it */
	KT_ASSERT_EQ(wk.maxdepth, 2);
	KT_ASSERT_EQ(wk.nvobj, 2);
	KT_ASSERT_EQ(st.nlink, 2);
	KT_ASSERT_EQ(nlinks, 2);
	KT_ASSERT(str_is(targets[0], "019a6c9b-e67e-7a35-a461-0d199550e4cf"));
	KT_ASSERT(str_is(targets[1], "019a6c9a-fbc2-76c1-b965-4ac1f7e4dcb7"));
	KT_ASSERT_EQ(st.noimage, 1);			/* no pictures given: left out */
	KT_ASSERT_EQ(wk.nprim, 1);			/* the line */
	KT_ASSERT(wk.nchar > 40);
	KT_ASSERT_ER(bk_tad_check(b, len, &nseg, &nchar), E_OK);
	KT_ASSERT_EQ(nseg, wk.nseg);
	KT_ASSERT_EQ(nchar, wk.nchar);
	KT_ASSERT_EQ(rd16(b + 14 + 16), (UINT)(UH)-96);	/* the figure's units */
	Kfree(b);
}

/* groups of filled rectangles with text in boxes, and a link */
LOCAL void test_tad_box( void )
{
	BK_TADSTAT st;
	WALK	wk;
	INT	len;
	UB	*b = to_tad(kt_bk_box, (INT)( kt_bk_box_end - kt_bk_box ), &len, &st);

	KT_ASSERT(b != NULL);
	if ( b == NULL ) return;
	walk(b, len, &wk);
	KT_ASSERT(!wk.bad);
	KT_ASSERT_EQ(wk.nfig, 1);
	KT_ASSERT_EQ(wk.ntext, 3);			/* a text for each box */
	KT_ASSERT_EQ(wk.nprim, 6);
	KT_ASSERT_EQ(wk.npat, 4);			/* black and three fills */
	KT_ASSERT_EQ(wk.nvobj, 1);
	KT_ASSERT_EQ(st.nlink, 1);
	KT_ASSERT_ER(bk_tad_check(b, len, NULL, NULL), E_OK);
	Kfree(b);
}

/* a text: every character is there and the record stays a text */
LOCAL void test_tad_doc( void )
{
	BK_TADSTAT st;
	WALK	wk;
	INT	len;
	UB	*b = to_tad(kt_bk_doc, (INT)( kt_bk_doc_end - kt_bk_doc ), &len, &st);

	KT_ASSERT(b != NULL);
	if ( b == NULL ) return;
	walk(b, len, &wk);
	KT_ASSERT(!wk.bad);
	KT_ASSERT_EQ(rd16(b + 10), 0xFFE1);
	KT_ASSERT_EQ(wk.ntext, 1);
	KT_ASSERT_EQ(wk.nfig, 0);
	KT_ASSERT_EQ(rd16(b + 14 + 16), (UINT)(UH)-72);	/* docScale */
	KT_ASSERT_EQ(st.nlink, 0);
	KT_ASSERT_EQ(st.nuni, 0);			/* all of it in JIS X 0208 */
	KT_ASSERT(st.nchar > 100);
	KT_ASSERT_ER(bk_tad_check(b, len, NULL, NULL), E_OK);
	Kfree(b);

	/* a record that is not TAD, or cut short, is refused */
	{
		static CONST UB cut[] = { 0xE0, 0xFF, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x22, 0x01,
					  0xE1, 0xFF, 0x18, 0x00, 0x00, 0x00 };
		static CONST UB open[] = { 0xE0, 0xFF, 0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x22, 0x01,
					   0xE3, 0xFF, 0x18, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
					   0, 0, 0, 0, 0x88, 0xFF, 0x88, 0xFF, 0, 0, 0, 0 };

		KT_ASSERT_ER(bk_tad_check(cut, sizeof(cut), NULL, NULL), E_OBJ);
		KT_ASSERT_ER(bk_tad_check(open, sizeof(open), NULL, NULL), E_OBJ);
	}
}

EXPORT void ktest_btbk( void )
{
	KT_RUN(test_tokens);
	KT_RUN(test_sizes);
	KT_RUN(test_read);
	KT_RUN(test_recompress);
	KT_RUN(test_rewrite);
	KT_RUN(test_volumes);
	KT_RUN(test_tad_fig);
	KT_RUN(test_tad_box);
	KT_RUN(test_tad_doc);

	if ( outs != NULL ) Kfree(outs);
	if ( wr != NULL ) Kfree(wr);
	if ( rd != NULL ) Kfree(rd);
	if ( objs != NULL ) Kfree(objs);
	if ( dec != NULL ) Kfree(dec);
	if ( enc != NULL ) Kfree(enc);
	outs = NULL;
	wr = NULL;
	rd = NULL;
	objs = NULL;
	dec = NULL;
	enc = NULL;
}
