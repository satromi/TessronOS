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
	const char	*kind;		/* the kind of data it opens */
	UINT		atype;		/* the F_STATE's application type */
} APPLMAP;

/*
 * The application 8000 C003 8000 (書庫) is 書庫管理, which an archive
 * read opens with. The second entry is the id 書庫管理 had before it
 * took in 書庫解凍 (unpack-file): an object whose applist still names
 * it is written with the archive's application all the same.
 */
LOCAL const APPLMAP applmap[] = {
	{ 0x0001, "virtual-object-list", "仮身一覧", 0, "仮身一覧", 6 },
	{ 0x0002, "basic-figure-editor", "基本図形編集", 0x1E, "図形", 2 },
	{ 0x0003, "basic-text-editor", "基本文章編集", 0x16, "文章", 1 },
	{ 0x0009, "basic-calc-editor", "基本表計算", 0x16, "表計算", 1 },
	{ 0x000B, "microscript", "マイクロスクリプト", 0x1E, "図形", 2 },
	{ BPK_APPL_ARCHIVE, "archive-manager", "書庫管理", 0, "書庫", 1 },
	{ BPK_APPL_ARCHIVE, "unpack-file", "書庫管理", 0, "書庫", 1 },
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

EXPORT BOOL bpk_prog_appl( const char *prog, BPKAPPL *a )
{
	INT	i;

	for ( i = 0; prog != NULL && i < NAPPL; i++ ) {
		if ( strcmp(applmap[i].prog, prog) != 0 ) continue;
		a->appl[0] = BPK_APPL_HI;
		a->appl[1] = applmap[i].appl;
		a->appl[2] = BPK_APPL_HI;
		a->name = applmap[i].name;
		a->kind = applmap[i].kind;
		a->atype = applmap[i].atype;
		a->bgoff = applmap[i].bgoff;
		return TRUE;
	}
	return FALSE;
}

EXPORT BOOL bpk_mc_format( const UB *p, UINT n )
{
	return (BOOL)( p != NULL && n >= 8 && rd16(p) == 0x2343 && rd16(p + 2) == 0x2352
		       && rd16(p + 4) == 0x2344 && rd16(p + 6) == 0x0100 );
}

EXPORT BOOL bpk_mc_file( const BPKFILE *f )
{
	INT	i;

	for ( i = 0; i < f->nrec; i++ ) {
		const BPKREC	*r = &f->rec[i];

		if ( r->type == BPK_RT_MCARD && r->sub == BPK_MC_FORMAT && bpk_mc_format(r->data, r->size) ) return TRUE;
	}
	return FALSE;
}

EXPORT void bpk_mc_xml( BPKBUF *b, const char *name, INT nlink, const BPKHOOK *hook )
{
	INT	n;

	bpk_buf_puts(b, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	bpk_buf_xml(b, name);
	bpk_buf_puts(b, "\">\n<document>\n<p>");
	bpk_buf_xml(b, name);
	bpk_buf_puts(b, "</p>\n<p><mclinks/>");
	for ( n = 0; n < nlink; n++ ) {
		const char	*t = ( hook->link_target != NULL ) ? hook->link_target(hook->ctx, n) : NULL;
		char		vid[40];

		if ( t == NULL ) {
			bpk_buf_puts(b, "<link id=\"\"/>");
			continue;
		}
		vid[0] = 0;
		if ( hook->new_vobjid != NULL ) hook->new_vobjid(hook->ctx, vid);
		bpk_buf_printf(b, "<link id=\"%s_0.xtad\" vobjid=\"%s\"/>", t, vid);
	}
	bpk_buf_puts(b, "</p>\n</document>\n</tad>\n");
}

EXPORT UINT bpk_crc16( const UB *p, UINT n, UINT crc )
{
	UINT	i;
	INT	k;

	for ( i = 0; i < n; i++ ) {
		crc ^= p[i];
		for ( k = 0; k < 8; k++ ) crc = ( crc & 1 ) ? ( crc >> 1 ) ^ 0xA001 : crc >> 1;
	}
	return crc & 0xFFFF;
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

/* A number of a 実行機能付箋, high byte first when be */
LOCAL UINT ex16( const UB *p, BOOL be )
{
	return be ? ( ( (UINT)p[0] << 8 ) | (UINT)p[1] ) : rd16(p);
}

LOCAL UINT ex32( const UB *p, BOOL be )
{
	return be ? ( ( ex16(p, TRUE) << 16 ) | ex16(p + 2, TRUE) ) : rd32(p);
}

/*
 * The entries of a 実行機能付箋 record: 96 bytes and the data each. A
 * record of the standard form, whose numbers have the high byte first,
 * is told by its first application ID: its first word is 8000.
 */
LOCAL void read_exec( BPKFILE *f, const UB *p, UINT size )
{
	UINT	at = 0;
	BOOL	be = (BOOL)( size >= BPK_EXECHEAD && rd16(p + EX_APPL) == 0x0080 && rd16(p + EX_APPL + 4) == 0x0080 );

	while ( at + BPK_EXECHEAD <= size ) {
		UINT		dlen = ex16(p + at + EX_DLEN, be), adv;
		const UB	*d = p + at + BPK_EXECHEAD;
		const APPLMAP	*m;
		BPKEXEC		*e, *grow;

		grow = realloc(f->exec, sizeof(BPKEXEC) * (size_t)( f->nexec + 1 ));
		if ( grow == NULL ) return;
		f->exec = grow;
		e = &f->exec[f->nexec++];
		memset(e, 0, sizeof(*e));
		e->appl[0] = (UH)ex16(p + at + EX_APPL, be);
		e->appl[1] = (UH)ex16(p + at + EX_APPL + 2, be);
		e->appl[2] = (UH)ex16(p + at + EX_APPL + 4, be);
		if ( at + BPK_EXECHEAD + dlen <= size ) {
			/* the window the application opens, a RECT after 4 bytes */
			if ( dlen >= 0x10 ) {
				e->wl = (H)ex16(d + 4, be);
				e->wt = (H)ex16(d + 6, be);
				e->wr = (H)ex16(d + 8, be);
				e->wb = (H)ex16(d + 10, be);
				e->haswin = TRUE;
			}
			m = appl_find(e->appl);
			if ( m != NULL && m->bgoff != 0 && dlen >= m->bgoff + 4 ) {
				e->bgraw = ex32(d + m->bgoff, be);
				e->hasbg = TRUE;
			}
			/* マイクロスクリプト keeps $SV, 50 numbers, at the end of its data */
			if ( m != NULL && m->appl == 0x000B && dlen >= 50 * 4 ) {
				INT	i;

				for ( i = 0; i < 50; i++ ) e->sv[i] = (INT)ex32(d + dlen - 200 + 4 * i, be);
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
#define RX_APPL		0x32
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
	for ( i = 0; i < 3; i++ ) a->rootappl[i] = (UH)rd16(p + RX_APPL + i * 2);
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
		return fail(err, errmax, "書庫のヘッダが壊れています", E_OBJ);
	}

	/* the stream inflated */
	a->plain = malloc(a->origsize);
	if ( a->plain == NULL ) return fail(err, errmax, "展開するメモリが足りません", E_NOMEM);
	if ( a->method == BPK_LH5 ) {
		BPKLH5	*z = malloc(sizeof(BPKLH5));
		INT	n;

		if ( z == NULL ) return fail(err, errmax, "展開するメモリが足りません", E_NOMEM);
		n = lh5_decode(z, g + BPK_GLOBALHEAD, a->compsize, a->plain, a->origsize);
		free(z);
		if ( n != (INT)a->origsize ) return fail(err, errmax, "圧縮データが壊れています", E_OBJ);
	} else {
		if ( a->origsize > a->compsize ) return fail(err, errmax, "書庫のデータが足りません", E_OBJ);
		memcpy(a->plain, g + BPK_GLOBALHEAD, a->origsize);
	}
	a->plainlen = a->origsize;
	a->crc = bpk_crc16(a->plain, a->plainlen, 0);
	a->crcok = (BOOL)( a->head.crc == 0 || a->head.crc == a->crc );

	/* the local headers */
	a->file = calloc(a->nfiles, sizeof(BPKFILE));
	if ( a->file == NULL ) return fail(err, errmax, "展開するメモリが足りません", E_NOMEM);
	pos = a->extsize;
	for ( i = 0; i < a->nfiles; i++, pos += BPK_LOCALHEAD ) {
		BPKFILE	*f = &a->file[i];

		bpk_lhead_get(a->plain + pos, &f->head);
		bpk_tron_utf8(f->head.name, 20, f->name, sizeof(f->name));
		if ( f->head.f_nrec > 65536 ) return fail(err, errmax, "書庫の実身のヘッダが壊れています", E_OBJ);
		f->nrec = (INT)f->head.f_nrec;
	}

	/*
	 * The application each object opens with from a link to it: the
	 * root's from the extension, then the first link record that names
	 * one, in the order the records come.
	 */
	read_root(a);
	if ( a->rootappl[0] != 0 ) memcpy(a->file[0].vappl, a->rootappl, sizeof(a->rootappl));

	/* each object's records, one object after another */
	for ( i = 0; i < a->nfiles; i++ ) {
		BPKFILE	*f = &a->file[i];

		f->rec = calloc(f->nrec ? (size_t)f->nrec : 1, sizeof(BPKREC));
		f->link = calloc(f->nrec ? (size_t)f->nrec : 1, sizeof(INT));
		if ( f->rec == NULL || f->link == NULL ) return fail(err, errmax, "展開するメモリが足りません", E_NOMEM);
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
				if ( id >= 0 && id < (INT)a->nfiles && a->file[id].vappl[0] == 0
				  && rd16(r->data + LK_APPL) != 0 ) {
					UH	*va = a->file[id].vappl;

					va[0] = (UH)rd16(r->data + LK_APPL);
					va[1] = (UH)rd16(r->data + LK_APPL + 2);
					va[2] = (UH)rd16(r->data + LK_APPL + 4);
				}
			} else if ( r->type == BPK_RT_TAD ) {
				f->ntad++;
			} else if ( r->type == BPK_RT_EXEC ) {
				read_exec(f, r->data, r->size);
			}
		}
	}
	a->root = 0;
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
