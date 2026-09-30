/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btbk_arc.c
 *	Reading and writing backup volumes (design 11.19.2, 11.19.4)
 *
 *	Volume:
 *	  FFE0 0006 | 0000 0002 0121			TS_INFO
 *	  FFFD 00BC | cmp=0 kind vol 0 total nobj src total_hi src_hi pad[6]
 *		      memo TC[80]
 *	Object (piece), repeated:
 *	  FFFD FFFF llen | cmp kind=F1 seg objid | name TC[20] | F_STATE
 *	  then llen - 0x90 bytes of the record stream, compressed when cmp
 *	  is 1, with the encoder started afresh for each piece.
 *	Record stream:
 *	  recno type subtype offset len (16 bytes) and len bytes, or for a
 *	  link record the 146 byte descriptor.
 *
 *	The writer sets llen and seg when the piece is done, by writing its
 *	head again; a volume found full is marked by writing the head of
 *	the volume again with BK_MORE in vol.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/btbk.h>

#define LINK_NEED	( BK_REC_HEAD + BK_LINK_SIZE )
#define PIECE_MIN	0x3FF		/* less free than this starts no piece */
#define XFER		0x800		/* bytes put to the encoder at a time */

LOCAL void bytes_zero( UB *d, INT n )
{
	while ( n-- > 0 ) *d++ = 0;
}

LOCAL UH get16( CONST UB *p )
{
	return (UH)( p[0] | ( p[1] << 8 ) );
}

LOCAL UINT get32( CONST UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 ) | ( (UINT)p[2] << 16 ) | ( (UINT)p[3] << 24 );
}

LOCAL void put16( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void put32( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
	p[3] = (UB)( v >> 24 );
}

LOCAL void tc_get( UH *d, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) d[i] = get16(s + 2 * i);
}

LOCAL void tc_put( UB *d, CONST UH *s, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) put16(d + 2 * i, s[i]);
}

/* ------------------------------------------------------------------ */
/* The stored forms */

EXPORT void bk_meta_get( CONST UB *m, UH *name, BK_FSTATE *st )
{
	CONST UB *f = m + 2 * BK_NAME_TC;

	if ( name != NULL ) tc_get(name, m, BK_NAME_TC);
	st->f_type = get16(f + 0x00);
	st->f_atype = get16(f + 0x02);
	tc_get(st->f_owner, f + 0x04, 14);
	tc_get(st->f_group, f + 0x20, 14);
	st->f_grpacc = get16(f + 0x3C);
	st->f_pubacc = get16(f + 0x3E);
	st->f_nlink = (H)get16(f + 0x40);
	st->f_index = (H)get16(f + 0x42);
	st->f_size = (INT)get32(f + 0x44);
	st->f_nblk = (INT)get32(f + 0x48);
	st->f_nrec = (INT)get32(f + 0x4C);
	st->f_ltime = get32(f + 0x50);
	st->f_atime = get32(f + 0x54);
	st->f_mtime = get32(f + 0x58);
	st->f_ctime = get32(f + 0x5C);
}

EXPORT void bk_meta_put( UB *m, CONST UH *name, CONST BK_FSTATE *st )
{
	UB	*f = m + 2 * BK_NAME_TC;

	tc_put(m, name, BK_NAME_TC);
	put16(f + 0x00, st->f_type);
	put16(f + 0x02, st->f_atype);
	tc_put(f + 0x04, st->f_owner, 14);
	tc_put(f + 0x20, st->f_group, 14);
	put16(f + 0x3C, st->f_grpacc);
	put16(f + 0x3E, st->f_pubacc);
	put16(f + 0x40, (UH)st->f_nlink);
	put16(f + 0x42, (UH)st->f_index);
	put32(f + 0x44, (UINT)st->f_size);
	put32(f + 0x48, (UINT)st->f_nblk);
	put32(f + 0x4C, (UINT)st->f_nrec);
	put32(f + 0x50, st->f_ltime);
	put32(f + 0x54, st->f_atime);
	put32(f + 0x58, st->f_mtime);
	put32(f + 0x5C, st->f_ctime);
}

/* Descriptor: atr[5] objid, then F_LINK: f_ctime f_atype f_name[20]
   f_id rf_ctime fs_name[20] fs_locat[20] */
EXPORT void bk_link_get( CONST UB *b, BK_LINKDESC *ld )
{
	CONST UB *r = b + 14;

	tc_get(ld->atr, b, 5);
	ld->objid = get32(b + 10);
	ld->f_ctime = get32(r + 0x00);
	ld->f_atype = get16(r + 0x04);
	tc_get(ld->f_name, r + 0x06, 20);
	ld->f_id = get16(r + 0x2E);
	ld->rf_ctime = get32(r + 0x30);
	tc_get(ld->fs_name, r + 0x34, 20);
	tc_get(ld->fs_locat, r + 0x5C, 20);
}

EXPORT void bk_link_put( UB *b, CONST BK_LINKDESC *ld )
{
	UB	*r = b + 14;

	tc_put(b, ld->atr, 5);
	put32(b + 10, ld->objid);
	put32(r + 0x00, ld->f_ctime);
	put16(r + 0x04, ld->f_atype);
	tc_put(r + 0x06, ld->f_name, 20);
	put16(r + 0x2E, ld->f_id);
	put32(r + 0x30, ld->rf_ctime);
	tc_put(r + 0x34, ld->fs_name, 20);
	tc_put(r + 0x5C, ld->fs_locat, 20);
}

EXPORT void bk_estimate( CONST BK_FSTATE *st, UINT bsize, UD *total, UD *src )
{
	*total += (UD)(UINT)st->f_size + 16 * (UD)(UINT)st->f_nrec + 0x98
		+ 146 * (D)st->f_nlink;
	*src += (UD)bsize * (UD)(UINT)st->f_nblk;
}

/* ------------------------------------------------------------------ */
/* Reader */

#define RD_VOL		0		/* between objects */
#define RD_OBJ		1		/* in an object, between records */
#define RD_BODY		2		/* in the body of a record piece */
#define RD_BAD		3		/* stopped on an error */

EXPORT void bk_rd_init( BK_RD *rd, BK_FRAG *frag, INT nfrag )
{
	rd->frag = frag;
	rd->nfrag = nfrag;
	rd->nused = 0;
	rd->nvol = 0;
	rd->more = FALSE;
	rd->loose = FALSE;
	rd->state = RD_BAD;
	rd->cur = NULL;
}

/* Exactly n raw bytes: E_OK, E_NOEXS when the volume ends first, or an error */
LOCAL ER raw_read( BK_RD *rd, UB *buf, INT n, BOOL at_start )
{
	INT	k;

	while ( n > 0 ) {
		k = rd->src(rd->arg, buf, n);
		if ( k < 0 ) return k;
		if ( k == 0 ) return at_start ? E_NOEXS : E_OBJ;
		at_start = FALSE;
		buf += k;
		n -= k;
	}
	return E_OK;
}

LOCAL ER raw_skip( BK_RD *rd, INT n )
{
	UB	tmp[64];
	INT	k;
	ER	er;

	while ( n > 0 ) {
		k = ( n > (INT)sizeof(tmp) ) ? (INT)sizeof(tmp) : (INT)n;
		er = raw_read(rd, tmp, k, FALSE);
		if ( er < E_OK ) return er;
		n -= k;
	}
	return E_OK;
}

LOCAL INT raw_src( void *arg, UB *buf, INT len )
{
	return ((BK_RD *)arg)->src(((BK_RD *)arg)->arg, buf, len);
}

EXPORT ER bk_rd_open( BK_RD *rd, BK_SRC src, void *arg, BK_VOLHEAD *vh )
{
	UB	h[BK_MEMO_TC * 2];
	UH	id, len, vol;
	ER	er;

	if ( rd->nvol > 0 && !rd->more ) return E_OBJ;	/* the set had ended */
	rd->src = src;
	rd->arg = arg;
	rd->state = RD_BAD;

	for ( ;; ) {
		er = raw_read(rd, h, 4, FALSE);
		if ( er < E_OK ) return er;
		id = get16(h);
		len = get16(h + 2);
		if ( id == BK_SEG_INFO && len != 0xFFFF ) {
			er = raw_skip(rd, len);
			if ( er < E_OK ) return er;
			continue;
		}
		break;
	}
	if ( id != BK_SEG || len == 0xFFFF || len < 0x1C ) return E_OBJ;
	er = raw_read(rd, h, 0x1C, FALSE);
	if ( er < E_OK ) return er;
	if ( h[1] != BK_KIND_VOL && h[1] != BK_KIND_VOL2 ) return E_OBJ;
	vol = get16(h + 2);
	if ( rd->loose && rd->nvol == 0 ) rd->nvol = vol & 0x7FFF;
	if ( ( vol & 0x7FFF ) != rd->nvol ) return E_OBJ;

	vh->kind = h[1];
	vh->vol = vol & 0x7FFF;
	vh->more = ( ( vol & BK_MORE ) != 0 );
	vh->total = get32(h + 6);
	vh->nobj = get32(h + 10);
	vh->src = get32(h + 14);
	if ( h[1] == BK_KIND_VOL2 ) {
		vh->total |= (UD)get16(h + 18) << 32;
		vh->src |= (UD)get16(h + 20) << 32;
	}
	len -= 0x1C;
	bytes_zero((UB *)vh->memo, sizeof(vh->memo));
	if ( len > 0 ) {
		INT	k = ( len > BK_MEMO_TC * 2 ) ? BK_MEMO_TC * 2 : len;

		er = raw_read(rd, h, k, FALSE);
		if ( er < E_OK ) return er;
		tc_get(vh->memo, h, k / 2);
		er = raw_skip(rd, len - k);
		if ( er < E_OK ) return er;
	}
	rd->nvol++;
	rd->more = vh->more;
	rd->state = RD_VOL;
	return E_OK;
}

LOCAL BK_FRAG *frag_find( BK_RD *rd, UINT objid )
{
	INT	i;

	for ( i = rd->nused - 1; i >= 0; i-- ) {
		if ( rd->frag[i].objid == objid ) return &rd->frag[i];
	}
	return NULL;
}

/* Stream bytes of the object: from the decoder, or raw */
LOCAL INT obj_read( BK_RD *rd, UB *buf, INT len )
{
	INT	k;
	ER	er;

	if ( rd->cmp ) return bk_dec_read(&rd->dec, buf, len);
	if ( len > rd->raw ) len = rd->raw;
	er = raw_read(rd, buf, len, FALSE);
	if ( er < E_OK ) return er;
	rd->raw -= len;
	k = len;
	return k;
}

LOCAL BOOL obj_ended( BK_RD *rd )
{
	return rd->cmp ? bk_dec_done(&rd->dec) : ( rd->raw == 0 );
}

/* What is left of the record piece being read, passed over */
LOCAL ER body_skip( BK_RD *rd )
{
	UB	tmp[64];
	INT	k, n;

	while ( rd->body > 0 ) {
		k = ( rd->body > (INT)sizeof(tmp) ) ? (INT)sizeof(tmp) : (INT)rd->body;
		n = obj_read(rd, tmp, k);
		if ( n < 0 ) return n;
		if ( n < k ) return E_OBJ;
		rd->body -= k;
	}
	return E_OK;
}

/* The end of an object's stream: nothing may be left, and a last piece
   must have brought every record */
LOCAL ER obj_close( BK_RD *rd )
{
	BK_FRAG	*f = rd->cur;

	if ( !obj_ended(rd) ) return E_OBJ;
	if ( f->nrec_done > f->nrec ) return E_OBJ;
	if ( rd->last ) {
		if ( f->nrec_done != f->nrec ) return E_OBJ;
		f->done = TRUE;
	}
	rd->cur = NULL;
	rd->state = RD_VOL;
	return E_OK;
}

EXPORT INT bk_rd_object( BK_RD *rd, BK_OBJHEAD *oh, BK_FRAG **p_frag )
{
	UB	h[8];
	UH	id, len;
	UINT	llen;
	BK_FRAG	*f;
	ER	er;

	if ( rd->state == RD_BODY || rd->state == RD_OBJ ) {
		/* the caller left the object before its end */
		er = body_skip(rd);
		while ( er >= E_OK && rd->state != RD_VOL ) {
			BK_RECHEAD rh;

			er = bk_rd_record(rd, &rh);
			if ( er > 0 ) er = body_skip(rd);
		}
		if ( er < E_OK ) goto bad;
	}
	if ( rd->state != RD_VOL ) return E_OBJ;

	er = raw_read(rd, h, 4, TRUE);
	if ( er == E_NOEXS ) return 0;			/* the volume's end */
	if ( er < E_OK ) goto bad;
	id = get16(h);
	len = get16(h + 2);
	if ( id != BK_SEG ) { er = E_OBJ; goto bad; }
	if ( len == 0xFFFF ) {
		er = raw_read(rd, h + 4, 4, FALSE);
		if ( er < E_OK ) goto bad;
		llen = get32(h + 4);
	} else {
		llen = len;
	}
	if ( llen < BK_OBJ_META ) { er = E_OBJ; goto bad; }

	er = raw_read(rd, h, 8, FALSE);
	if ( er < E_OK ) goto bad;
	er = raw_read(rd, oh->meta, BK_META_SIZE, FALSE);
	if ( er < E_OK ) goto bad;
	if ( h[1] != BK_KIND_OBJ || h[0] > 1 ) { er = E_OBJ; goto bad; }
	oh->cmp = h[0];
	oh->seg = get16(h + 2);
	oh->objid = get32(h + 4);
	oh->llen = llen;
	bk_meta_get(oh->meta, oh->name, &oh->st);

	if ( ( oh->seg & 0x7FFF ) == 0 ) {
		/* first piece: a new object; its id must not be in the set yet */
		f = frag_find(rd, oh->objid);
		if ( f != NULL && !f->done ) { er = E_OBJ; goto bad; }
		if ( rd->nused >= rd->nfrag ) { er = E_LIMIT; goto bad; }
		f = &rd->frag[rd->nused++];
		f->objid = oh->objid;
		f->nrec = oh->st.f_nrec;
		f->nrec_done = 0;
		f->recoff = 0;
		f->done = FALSE;
		f->user = NULL;
	} else {
		f = frag_find(rd, oh->objid);
		if ( ( f == NULL || f->done ) && rd->loose && rd->nused < rd->nfrag ) {
			/* the earlier pieces are not here: go on from this one */
			f = &rd->frag[rd->nused++];
			f->objid = oh->objid;
			f->nrec = oh->st.f_nrec;
			f->nrec_done = -1;
			f->recoff = 0;
			f->done = FALSE;
			f->user = NULL;
		}
		if ( f == NULL || f->done ) { er = E_NOEXS; goto bad; }
	}
	rd->cur = f;
	rd->cmp = ( oh->cmp != 0 );
	rd->last = ( ( oh->seg & BK_MORE ) == 0 );
	rd->body = 0;
	if ( rd->cmp ) {
		bk_dec_init(&rd->dec, raw_src, rd, (INT)( llen - BK_OBJ_META ));
	} else {
		rd->raw = (INT)( llen - BK_OBJ_META );
	}
	rd->state = RD_OBJ;
	*p_frag = f;
	return 1;

bad:
	rd->state = RD_BAD;
	return er;
}

EXPORT INT bk_rd_record( BK_RD *rd, BK_RECHEAD *rh )
{
	UB	h[BK_REC_HEAD];
	BK_FRAG	*f = rd->cur;
	INT	n;
	ER	er;

	if ( rd->state == RD_BODY ) {
		er = body_skip(rd);
		if ( er < E_OK ) goto bad;
		rd->state = RD_OBJ;
	}
	if ( rd->state != RD_OBJ ) return E_OBJ;
	if ( obj_ended(rd) ) {
		er = obj_close(rd);
		if ( er < E_OK ) goto bad;
		return 0;
	}

	n = obj_read(rd, h, BK_REC_HEAD);
	if ( n < 0 ) { er = n; goto bad; }
	if ( n < BK_REC_HEAD ) { er = E_OBJ; goto bad; }
	rh->recno = (INT)get32(h);
	rh->type = get16(h + 4);
	rh->subtype = get16(h + 6);
	rh->offset = (INT)get32(h + 8);
	rh->len = (INT)get32(h + 12);

	if ( f->nrec_done < 0 ) {
		/* loose: the first piece read of an object seen part way */
		f->nrec_done = ( rh->offset != 0 ) ? rh->recno + 1 : rh->recno;
		f->recoff = rh->offset;
	}
	if ( f->nrec_done == rh->recno ) {
		/* a record begins */
		if ( rh->offset != 0 ) { er = E_OBJ; goto bad; }
		f->recoff = 0;
		f->nrec_done = rh->recno + 1;
	} else if ( !( f->nrec_done == rh->recno + 1 && rh->offset != 0
		       && rh->offset == f->recoff ) ) {
		er = E_OBJ;				/* not where the last one stopped */
		goto bad;
	}
	if ( f->nrec_done > f->nrec || rh->len < 0 ) { er = E_OBJ; goto bad; }
	if ( rh->type == 0 && ( rh->offset != 0 || rh->len != BK_LINK_SIZE ) ) {
		er = E_OBJ;
		goto bad;
	}
	f->recoff += rh->len;
	rd->type = rh->type;
	rd->body = rh->len;
	rd->state = RD_BODY;
	return 1;

bad:
	rd->state = RD_BAD;
	return er;
}

EXPORT INT bk_rd_read( BK_RD *rd, UB *buf, INT len )
{
	INT	n;

	if ( rd->state != RD_BODY ) return E_OBJ;
	if ( len > rd->body ) len = (INT)rd->body;
	n = obj_read(rd, buf, len);
	if ( n < 0 ) { rd->state = RD_BAD; return n; }
	if ( n < len ) { rd->state = RD_BAD; return E_OBJ; }
	rd->body -= n;
	return n;
}

EXPORT ER bk_rd_link( BK_RD *rd, BK_LINKDESC *ld )
{
	UB	b[BK_LINK_SIZE];
	INT	n;

	if ( rd->state != RD_BODY || rd->type != 0 || rd->body != BK_LINK_SIZE ) {
		return E_OBJ;
	}
	n = bk_rd_read(rd, b, BK_LINK_SIZE);
	if ( n < 0 ) return n;
	bk_link_get(b, ld);
	return E_OK;
}

EXPORT ER bk_rd_close( BK_RD *rd )
{
	INT	i;

	if ( rd->nvol == 0 || rd->more ) return E_OBJ;
	for ( i = 0; i < rd->nused; i++ ) {
		if ( !rd->frag[i].done ) return E_OBJ;
	}
	return E_OK;
}

/* ------------------------------------------------------------------ */
/* Writer */

EXPORT void bk_wr_init( BK_WR *w, UD total, UINT nobj, UD src, CONST UH *memo )
{
	INT	i;

	w->total = total;
	w->nobj = nobj;
	w->src = src;
	for ( i = 0; i < BK_MEMO_TC; i++ ) {
		w->memo[i] = ( memo != NULL ) ? memo[i] : 0;
	}
	w->vol = 0;
	w->resume_rec = 0;
	w->resume_off = 0;
	w->segno = 0;
	w->out = NULL;
}

/* Raw bytes at the end of the volume, counted against its room */
LOCAL ER wr_raw( BK_WR *w, CONST UB *buf, INT len )
{
	ER	er;

	er = w->out(w->arg, w->pos, buf, len);
	if ( er < E_OK ) return er;
	w->pos += len;
	w->free -= len;
	w->objbytes += len;
	return E_OK;
}

LOCAL INT wr_sink( void *arg, CONST UB *buf, INT len )
{
	return wr_raw((BK_WR *)arg, buf, len);
}

LOCAL void head_make( BK_WR *w, UB *h, BOOL more )
{
	bytes_zero(h, BK_HEAD_FIXED);
	put16(h + 0, BK_SEG_INFO);
	put16(h + 2, 6);
	put16(h + 4, 0);
	put16(h + 6, 2);
	put16(h + 8, BK_TADVER);
	put16(h + 10, BK_SEG);
	put16(h + 12, 0x1C + BK_MEMO_TC * 2);
	h[14] = 0;
	h[15] = ( w->total > BK_BIG ) ? BK_KIND_VOL2 : BK_KIND_VOL;
	put16(h + 16, w->vol | ( more ? BK_MORE : 0 ));
	put32(h + 20, (UINT)w->total);
	put32(h + 24, w->nobj);
	put32(h + 28, (UINT)w->src);
	put16(h + 32, (UH)( w->total >> 32 ));
	put16(h + 34, (UH)( w->src >> 32 ));
}

EXPORT ER bk_wr_begin( BK_WR *w, BK_OUT out, void *arg, UD capacity )
{
	UB	h[BK_HEAD_FIXED];
	ER	er;

	w->out = out;
	w->arg = arg;
	w->pos = 0;
	w->free = ( capacity > 0x3FFFFFFFFFFFFFFFULL ) ? 0x3FFFFFFFFFFFFFFFLL : (D)capacity;
	w->wrote = FALSE;
	head_make(w, h, FALSE);
	er = wr_raw(w, h, BK_HEAD_FIXED);
	if ( er < E_OK ) return er;
	tc_put(w->buf, w->memo, BK_MEMO_TC);
	return wr_raw(w, w->buf, BK_MEMO_TC * 2);
}

EXPORT ER bk_wr_end( BK_WR *w, BOOL more )
{
	UB	h[BK_HEAD_FIXED];
	ER	er;

	if ( more ) {
		head_make(w, h, TRUE);
		er = w->out(w->arg, 0, h, BK_HEAD_FIXED);
		if ( er < E_OK ) return er;
	}
	w->vol++;
	return E_OK;
}

/* What fits of `need`: a thirty-second of the room is kept back */
LOCAL INT room( BK_WR *w, INT need )
{
	D	f = w->free;
	D	r = f - f / 32;

	if ( r < 0 ) r = 0;
	return ( need < r ) ? need : (INT)r;
}

LOCAL ER rec_head( BK_WR *w, UH type, UH subtype, INT off, INT len )
{
	UB	h[BK_REC_HEAD];

	put32(h, (UINT)w->resume_rec);
	put16(h + 4, type);
	put16(h + 6, subtype);
	put32(h + 8, (UINT)off);
	put32(h + 12, (UINT)len);
	return bk_enc_put(&w->enc, h, BK_REC_HEAD);
}

/* One record from where it stopped: E_OK, BK_FULL, or an error */
LOCAL ER wr_record( BK_WR *w, CONST BK_OBJSRC *os )
{
	UH	type, subtype;
	INT	size, rest, n, k, at;
	INT	got;
	ER	er;

	er = os->info(os->arg, w->resume_rec, &type, &subtype, &size);
	if ( er < E_OK ) return er;

	if ( type == 0 ) {
		BK_LINKDESC ld;

		if ( room(w, LINK_NEED) < LINK_NEED ) return BK_FULL;
		er = os->link(os->arg, w->resume_rec, &ld);
		if ( er < E_OK ) return er;
		er = rec_head(w, 0, subtype, 0, BK_LINK_SIZE);
		if ( er < E_OK ) return er;
		bk_link_put(w->buf, &ld);
		er = bk_enc_put(&w->enc, w->buf, BK_LINK_SIZE);
		if ( er < E_OK ) return er;
		w->wrote = TRUE;
		return E_OK;
	}

	rest = size - w->resume_off;
	for ( ;; ) {
		n = room(w, rest + BK_REC_HEAD) - BK_REC_HEAD;
		if ( n < rest && n <= PIECE_MIN ) return BK_FULL;
		er = rec_head(w, type, subtype, w->resume_off, n);
		if ( er < E_OK ) return er;
		for ( at = w->resume_off, k = n; k > 0; ) {
			got = os->read(os->arg, w->resume_rec, at,
				       w->buf, ( k > XFER ) ? XFER : (INT)k);
			if ( got < 0 ) return got;
			if ( got == 0 ) return E_OBJ;	/* shorter than it said */
			er = bk_enc_put(&w->enc, w->buf, got);
			if ( er < E_OK ) return er;
			at += got;
			k -= got;
		}
		w->wrote = TRUE;
		rest -= n;
		if ( rest <= 0 ) break;
		w->resume_off += n;
	}
	w->resume_off = 0;
	return E_OK;
}

EXPORT ER bk_wr_object( BK_WR *w, UINT objid, CONST UB *meta, CONST BK_OBJSRC *os )
{
	UB	oh[16];
	UD	hdrpos;
	BOOL	split = FALSE;
	UH	seg;
	ER	er;

	if ( w->out == NULL ) return E_OBJ;
	if ( w->free <= PIECE_MIN ) {
		return w->wrote ? BK_FULL : E_LIMIT;
	}

	/* the head is written now and again when the piece is done */
	hdrpos = w->pos;
	bytes_zero(oh, sizeof(oh));
	put16(oh + 0, BK_SEG);
	put16(oh + 2, 0xFFFF);
	oh[8] = 1;
	oh[9] = BK_KIND_OBJ;
	put32(oh + 12, objid);
	er = wr_raw(w, oh, 16);
	if ( er < E_OK ) return er;
	er = wr_raw(w, meta, BK_META_SIZE);
	if ( er < E_OK ) return er;

	bk_enc_init(&w->enc, wr_sink, w);
	w->objbytes = BK_OBJ_META;
	while ( w->resume_rec < os->nrec ) {
		er = wr_record(w, os);
		if ( er < E_OK ) return er;
		if ( er == BK_FULL ) {
			split = TRUE;
			break;
		}
		w->resume_rec++;
	}
	er = bk_enc_flush(&w->enc);
	if ( er < E_OK ) return er;

	put32(oh + 4, (UINT)w->objbytes);
	if ( split ) {
		seg = w->segno | BK_MORE;
		w->segno++;
	} else {
		seg = w->segno;
		w->segno = 0;
		w->resume_rec = 0;
		w->resume_off = 0;
	}
	put16(oh + 10, seg);
	er = w->out(w->arg, hdrpos, oh, 16);
	if ( er < E_OK ) return er;
	if ( !split ) {
		w->wrote = TRUE;
		return E_OK;
	}
	return w->wrote ? BK_FULL : E_LIMIT;		/* E_LIMIT: the volume holds nothing */
}
