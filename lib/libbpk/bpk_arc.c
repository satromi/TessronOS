/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bpk_arc.c
 *	An archive (書庫, BPK) read (design 17.16)
 *
 *	The archive file is a binary TAD: a 管理情報セグメント, a figure and
 *	in it a 指定付箋 whose application ID is 8000 C003 8000. The 付箋's
 *	data begins with the global header; the compressed stream follows
 *	it. Inflated, the stream is the extension, the local headers and
 *	then each object's records in turn, each a record header and its
 *	bytes; the local headers' offsets say the same. A link record names
 *	its target by the number of its local header, and a 実行機能付箋
 *	record lists the applications that open the object.
 */

#include <ts/bpk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOCAL UINT rd16( const UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 );
}

LOCAL UINT rd32( const UB *p )
{
	return (UINT)p[0] | ( (UINT)p[1] << 8 ) | ( (UINT)p[2] << 16 ) | ( (UINT)p[3] << 24 );
}

LOCAL void wr16( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void wr32( UB *p, UINT v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
	p[3] = (UB)( v >> 24 );
}

/* ---------------------------------------------------------------- the headers */

EXPORT void bpk_ghead_get( const UB *p, BPKGHEAD *h )
{
	h->headtype = p[GH_HEADTYPE];
	h->checksum = p[GH_CHECKSUM];
	h->version = rd16(p + GH_VERSION);
	h->crc = rd16(p + GH_CRC);
	h->nfiles = rd16(p + GH_NFILES);
	h->method = rd16(p + GH_COMPMETHOD);
	h->time = rd32(p + GH_TIME);
	h->filesize = rd32(p + GH_FILESIZE);
	h->origsize = rd32(p + GH_ORIGSIZE);
	h->compsize = rd32(p + GH_COMPSIZE);
	h->extsize = rd32(p + GH_EXTSIZE);
}

/* The sum of the global header's bytes after the checksum, as one byte */
EXPORT UINT bpk_ghead_sum( const UB *p )
{
	UINT	s = 0;
	INT	i;

	for ( i = GH_VERSION; i < BPK_GLOBALHEAD; i++ ) s += p[i];
	return s & 0xFF;
}

/* The global header written, its checksum made from what it holds */
EXPORT void bpk_ghead_put( const BPKGHEAD *h, UB *p )
{
	p[GH_HEADTYPE] = (UB)h->headtype;
	wr16(p + GH_VERSION, h->version);
	wr16(p + GH_CRC, h->crc);
	wr16(p + GH_NFILES, h->nfiles);
	wr16(p + GH_COMPMETHOD, h->method);
	wr32(p + GH_TIME, h->time);
	wr32(p + GH_FILESIZE, h->filesize);
	wr32(p + GH_ORIGSIZE, h->origsize);
	wr32(p + GH_COMPSIZE, h->compsize);
	wr32(p + GH_EXTSIZE, h->extsize);
	p[GH_CHECKSUM] = (UB)bpk_ghead_sum(p);
}

EXPORT void bpk_lhead_get( const UB *p, BPKLHEAD *h )
{
	INT	i;

	h->f_type = rd16(p + LH_F_TYPE);
	h->f_atype = rd16(p + LH_F_ATYPE);
	for ( i = 0; i < 20; i++ ) h->name[i] = (UH)rd16(p + LH_NAME + 2 * i);
	h->origid = (INT)(H)rd16(p + LH_ORIGID);
	h->compmethod = (INT)(H)rd16(p + LH_COMPMETHOD);
	h->origsize = rd32(p + LH_ORIGSIZE);
	h->compsize = rd32(p + LH_COMPSIZE);
	h->f_nlink = rd16(p + LH_F_NLINK);
	h->crc = rd16(p + LH_CRC);
	h->f_size = rd32(p + LH_F_SIZE);
	h->offset = rd32(p + LH_OFFSET);
	h->f_nrec = rd32(p + LH_F_NREC);
	h->f_ltime = rd32(p + LH_F_LTIME);
	h->f_atime = rd32(p + LH_F_ATIME);
	h->f_mtime = rd32(p + LH_F_MTIME);
	h->f_ctime = rd32(p + LH_F_CTIME);
}

EXPORT void bpk_lhead_put( const BPKLHEAD *h, UB *p )
{
	INT	i;

	memset(p, 0, BPK_LOCALHEAD);
	wr16(p + LH_F_TYPE, h->f_type);
	wr16(p + LH_F_ATYPE, h->f_atype);
	for ( i = 0; i < 20; i++ ) wr16(p + LH_NAME + 2 * i, h->name[i]);
	wr16(p + LH_ORIGID, (UINT)h->origid);
	wr16(p + LH_COMPMETHOD, (UINT)h->compmethod);
	wr32(p + LH_ORIGSIZE, h->origsize);
	wr32(p + LH_COMPSIZE, h->compsize);
	wr16(p + LH_F_NLINK, h->f_nlink);
	wr16(p + LH_CRC, h->crc);
	wr32(p + LH_F_SIZE, h->f_size);
	wr32(p + LH_OFFSET, h->offset);
	wr32(p + LH_F_NREC, h->f_nrec);
	wr32(p + LH_F_LTIME, h->f_ltime);
	wr32(p + LH_F_ATIME, h->f_atime);
	wr32(p + LH_F_MTIME, h->f_mtime);
	wr32(p + LH_F_CTIME, h->f_ctime);
}

/* ---------------------------------------------------------------- applications */

/* The applications a 実行機能付箋 may name, and the programs that are them */
typedef struct {
	UH		appl;		/* the middle word; the others are 8000 */
	const char	*prog;
	const char	*name;
	UINT		bgoff;		/* where its data keeps the window's colour, 0 none */
} APPLMAP;

LOCAL const APPLMAP applmap[] = {
	{ 0x0001, "virtual-object-list", "仮身一覧", 0 },
	{ 0x0002, "basic-figure-editor", "基本図形編集", 0x1E },
	{ 0x0003, "basic-text-editor", "基本文章編集", 0x16 },
	{ 0x0009, "basic-calc-editor", "基本表計算", 0x16 },
	{ 0x000B, "microscript", "マイクロスクリプト", 0x1E },
	{ BPK_APPL_ARCHIVE, "unpack-file", "書庫解凍", 0 },
};
#define NAPPL	( (INT)( sizeof(applmap) / sizeof(applmap[0]) ) )

LOCAL const APPLMAP *appl_find( const UH *appl )
{
	INT	i;

	if ( appl[0] != BPK_APPL_HI || appl[2] != BPK_APPL_HI ) return NULL;
	for ( i = 0; i < NAPPL; i++ ) {
		if ( applmap[i].appl == appl[1] ) return &applmap[i];
	}
	return NULL;
}

EXPORT const char *bpk_appl_prog( const UH *appl, const char **p_name )
{
	const APPLMAP	*m = appl_find(appl);

	if ( p_name != NULL ) *p_name = ( m != NULL ) ? m->name : NULL;
	return ( m != NULL ) ? m->prog : NULL;
}

/* ---------------------------------------------------------------- the archive */

/* The 指定付箋 that holds the archive: where its data starts, and its size */
LOCAL BOOL find_fusen( BPKARC *a, UINT *p_at, UINT *p_len )
{
	UINT	pos = 0;

	while ( pos + 2 <= a->rawlen ) {
		UINT	w = rd16(a->raw + pos), len;

		if ( w == 0 ) break;
		if ( w <= TC_SPEC ) {
			pos += 2;
			continue;
		}
		pos += 2;
		if ( w == TS_EXT ) continue;		/* its next word is the id */
		if ( pos + 2 > a->rawlen ) break;
		len = rd16(a->raw + pos);
		pos += 2;
		if ( len == TS_LONG ) {
			if ( pos + 4 > a->rawlen ) break;
			len = rd32(a->raw + pos);
			pos += 4;
		}
		if ( len > a->rawlen - pos ) break;
		if ( w == TS_DFUSEN && len >= DF_DATA + BPK_GLOBALHEAD ) {
			const UB	*d = a->raw + pos;

			if ( rd16(d + DF_APPL) == BPK_APPL_HI && rd16(d + DF_APPL + 2) == BPK_APPL_ARCHIVE
			  && rd16(d + DF_APPL + 4) == BPK_APPL_HI ) {
				UH	name[16];
				INT	i;

				for ( i = 0; i < 16; i++ ) name[i] = (UH)rd16(d + DF_NAME + 2 * i);
				bpk_tron_utf8(name, 16, a->name, sizeof(a->name));
				*p_at = pos;
				*p_len = len;
				return TRUE;
			}
		}
		pos += len;
	}
	return FALSE;
}

/* The entries of a 実行機能付箋 record: 96 bytes and the data each */
LOCAL void read_exec( BPKFILE *f, const UB *p, UINT size )
{
	UINT	at = 0;

	while ( at + BPK_EXECHEAD <= size ) {
		UINT		dlen = rd16(p + at + EX_DLEN), adv;
		const UB	*d = p + at + BPK_EXECHEAD;
		const APPLMAP	*m;
		BPKEXEC		*e, *grow;

		grow = realloc(f->exec, sizeof(BPKEXEC) * (size_t)( f->nexec + 1 ));
		if ( grow == NULL ) return;
		f->exec = grow;
		e = &f->exec[f->nexec++];
		memset(e, 0, sizeof(*e));
		e->appl[0] = (UH)rd16(p + at + EX_APPL);
		e->appl[1] = (UH)rd16(p + at + EX_APPL + 2);
		e->appl[2] = (UH)rd16(p + at + EX_APPL + 4);
		if ( at + BPK_EXECHEAD + dlen <= size ) {
			/* the window the application opens, a RECT after 4 bytes */
			if ( dlen >= 0x10 ) {
				e->wl = (H)rd16(d + 4);
				e->wt = (H)rd16(d + 6);
				e->wr = (H)rd16(d + 8);
				e->wb = (H)rd16(d + 10);
				e->haswin = TRUE;
			}
			m = appl_find(e->appl);
			if ( m != NULL && m->bgoff != 0 && dlen >= m->bgoff + 4 ) {
				e->bgraw = rd32(d + m->bgoff);
				e->hasbg = TRUE;
			}
			/* マイクロスクリプト keeps $SV, 50 numbers, at the end of its data */
			if ( m != NULL && m->appl == 0x000B && dlen >= 50 * 4 ) {
				INT	i;

				for ( i = 0; i < 50; i++ ) e->sv[i] = (INT)rd32(d + dlen - 200 + 4 * i);
				e->hassv = TRUE;
			}
		}
		adv = BPK_EXECHEAD + dlen;
		if ( adv & 1 ) adv++;
		at += adv;
	}
}

/*
 * The extension at the stream's start: a count, the length that
 * follows, then the root's link (VLINK, 52 bytes: fs_name TC[20],
 * f_id, attr, rel, appl[3]) and its virtual object segment (view RECT,
 * height, chsz, frcol, chcol, tbcol, bgcol, dlen).
 */
#define RX_COUNT	0x00
#define RX_ATTR		0x2E
#define RX_VIEW		0x38
#define RX_CHSZ		0x42
#define RX_FRCOL	0x44
#define RX_END		0x56		/* up to the segment's dlen */

LOCAL void read_root( BPKARC *a )
{
	const UB	*p = a->plain;
	INT		i;

	a->hasroot = FALSE;
	if ( p == NULL || a->extsize < RX_END || a->plainlen < RX_END || rd16(p + RX_COUNT) < 1 ) {
		return;
	}
	a->rootattr = rd16(p + RX_ATTR);
	for ( i = 0; i < 4; i++ ) {
		a->rootview[i] = (INT)(H)rd16(p + RX_VIEW + i * 2);
		a->rootcol[i] = rd32(p + RX_FRCOL + i * 4);
	}
	a->rootchsz = rd16(p + RX_CHSZ);
	a->hasroot = (BOOL)( a->rootview[2] > a->rootview[0] && a->rootview[3] > a->rootview[1] );
}

LOCAL ER fail( char *err, INT max, const char *s, ER er )
{
	if ( err != NULL && max > 0 ) {
		strncpy(err, s, (size_t)max - 1);
		err[max - 1] = 0;
	}
	return er;
}

EXPORT ER bpk_parse( BPKARC *a, const UB *raw, UINT rawlen, char *err, INT errmax )
{
	UINT		at = 0, flen = 0, pos, i;
	INT		j;
	const UB	*g;

	memset(a, 0, sizeof(*a));
	a->raw = raw;
	a->rawlen = rawlen;
	if ( !find_fusen(a, &at, &flen) ) return fail(err, errmax, "書庫の指定付箋が見つかりません", E_OBJ);
	g = raw + at + DF_DATA;
	bpk_ghead_get(g, &a->head);
	a->nfiles = a->head.nfiles;
	a->method = a->head.method;
	a->origsize = a->head.origsize;
	a->compsize = a->head.compsize;
	a->extsize = a->head.extsize;
	if ( a->method != BPK_LH0 && a->method != BPK_LH5 ) {
		return fail(err, errmax, "対応していない圧縮方式です", E_NOSPT);
	}
	if ( a->head.checksum != bpk_ghead_sum(g) || a->nfiles == 0 || a->compsize > flen - DF_DATA - BPK_GLOBALHEAD
	  || a->origsize < a->extsize + a->nfiles * BPK_LOCALHEAD || a->origsize > 256 * 1024 * 1024 ) {
		return fail(err, errmax, "書庫の見出しが壊れています", E_OBJ);
	}

	/* the stream inflated */
	a->plain = malloc(a->origsize);
	if ( a->plain == NULL ) return fail(err, errmax, "展開する場所がありません", E_NOMEM);
	if ( a->method == BPK_LH5 ) {
		BPKLH5	*z = malloc(sizeof(BPKLH5));
		INT	n;

		if ( z == NULL ) return fail(err, errmax, "展開する場所がありません", E_NOMEM);
		n = lh5_decode(z, g + BPK_GLOBALHEAD, a->compsize, a->plain, a->origsize);
		free(z);
		if ( n != (INT)a->origsize ) return fail(err, errmax, "圧縮データが壊れています", E_OBJ);
	} else {
		if ( a->origsize > a->compsize ) return fail(err, errmax, "書庫のデータが足りません", E_OBJ);
		memcpy(a->plain, g + BPK_GLOBALHEAD, a->origsize);
	}
	a->plainlen = a->origsize;

	/* the local headers */
	a->file = calloc(a->nfiles, sizeof(BPKFILE));
	if ( a->file == NULL ) return fail(err, errmax, "展開する場所がありません", E_NOMEM);
	pos = a->extsize;
	for ( i = 0; i < a->nfiles; i++, pos += BPK_LOCALHEAD ) {
		BPKFILE	*f = &a->file[i];

		bpk_lhead_get(a->plain + pos, &f->head);
		bpk_tron_utf8(f->head.name, 20, f->name, sizeof(f->name));
		if ( f->head.f_nrec > 65536 ) return fail(err, errmax, "書庫の実身の見出しが壊れています", E_OBJ);
		f->nrec = (INT)f->head.f_nrec;
	}

	/* each object's records, one object after another */
	for ( i = 0; i < a->nfiles; i++ ) {
		BPKFILE	*f = &a->file[i];

		f->rec = calloc(f->nrec ? (size_t)f->nrec : 1, sizeof(BPKREC));
		f->link = calloc(f->nrec ? (size_t)f->nrec : 1, sizeof(INT));
		if ( f->rec == NULL || f->link == NULL ) return fail(err, errmax, "展開する場所がありません", E_NOMEM);
		for ( j = 0; j < f->nrec; j++ ) {
			BPKREC	*r = &f->rec[j];

			if ( pos + BPK_RECHEAD > a->plainlen ) {
				return fail(err, errmax, "書庫のレコードが途中で切れています", E_OBJ);
			}
			r->type = (UH)rd16(a->plain + pos + RH_TYPE);
			r->sub = (UH)rd16(a->plain + pos + RH_SUBTYPE);
			r->size = rd32(a->plain + pos + RH_SIZE);
			pos += BPK_RECHEAD;
			if ( r->size > a->plainlen - pos ) {
				return fail(err, errmax, "書庫のレコードが途中で切れています", E_OBJ);
			}
			r->data = a->plain + pos;
			pos += r->size;
			if ( r->type == BPK_RT_LINK ) {
				INT	id = ( r->size >= BPK_LINKREC ) ? (INT)rd16(r->data + LK_F_ID) : -1;

				f->link[f->nlink++] = ( id >= 0 && id < (INT)a->nfiles ) ? id : -1;
			} else if ( r->type == BPK_RT_TAD ) {
				f->ntad++;
			} else if ( r->type == BPK_RT_EXEC ) {
				read_exec(f, r->data, r->size);
			}
		}
	}
	a->root = 0;
	read_root(a);
	return E_OK;
}

EXPORT void bpk_free( BPKARC *a )
{
	UINT	i;

	if ( a->file != NULL ) {
		for ( i = 0; i < a->nfiles; i++ ) {
			free(a->file[i].rec);
			free(a->file[i].link);
			free(a->file[i].exec);
		}
		free(a->file);
	}
	free(a->plain);
	memset(a, 0, sizeof(*a));
}
