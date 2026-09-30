/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_vol.c
 *	バックアップ: the bytes of a volume, in a file or in an object
 *	(design 17.18)
 *
 *	A volume written to a directory is a file named after the volume
 *	("１・名前.TAD"). A volume kept as an object is one of its own: its
 *	record 0 says what it is, record 1 (RT 15) holds the bytes, its
 *	applist opens it in this accessory, and it is linked from the
 *	cabinet it was made for. Reading takes either back.
 */

#include "bk.h"
#include "bk_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/sysdef.h>

/* ---------------------------------------------------------------- writing */

#define WBUF	0x10000		/* write-behind buffer */

LOCAL ER put_out( BKVOL *v, UD off, CONST UB *buf, INT len )
{
	INT	n;
	SZ	asz = 0;
	ER	er;

	if ( v->fd >= 0 ) {
		if ( fs_lseek(v->fd, (D)off, SEEK_SET_) < 0 ) return E_IO;
		n = fs_write(v->fd, buf, len);
		return ( n == len ) ? E_OK : ( n < 0 ) ? n : E_IO;
	}
	er = ob_wri_rec(v->key, 1, (D)off, buf, len, &asz);
	return ( er < E_OK ) ? er : ( asz == len ) ? E_OK : E_IO;
}

LOCAL ER wflush( BKVOL *v )
{
	ER	er = E_OK;

	if ( v->wn > 0 ) er = put_out(v, v->woff, v->wb, v->wn);
	v->wn = 0;
	return er;
}

/*
 * The writer hands over a few bytes at a time, mostly one after another;
 * they are gathered so the medium sees large writes. A write elsewhere
 * (a head filled in afterwards) puts out what is gathered first.
 */
LOCAL INT vol_out( void *arg, UD off, CONST UB *buf, INT len )
{
	BKVOL	*v = arg;
	ER	er;

	if ( v->wb == NULL ) {
		v->wb = malloc(WBUF);
		v->wn = 0;
		if ( v->wb == NULL ) return put_out(v, off, buf, len);
	}
	if ( v->wn > 0 && ( off != v->woff + (UD)v->wn || v->wn + len > WBUF ) ) {
		er = wflush(v);
		if ( er < E_OK ) return er;
	}
	if ( len > WBUF ) return put_out(v, off, buf, len);
	if ( v->wn == 0 ) v->woff = off;
	memcpy(v->wb + v->wn, buf, len);
	v->wn += len;
	return E_OK;
}

/* A link to an object added at the end of a cabinet's record 0 */
EXPORT ER bk_link_into( const TS_UUID *cab, const TS_UUID *u, const char *name )
{
	return bk_link_at(cab, u, name, NULL);
}

/*
 * The link put where box says when the record is a figure; in a text,
 * or with no box, at the foot below the links there are.
 */
EXPORT ER bk_link_at( const TS_UUID *cab, const TS_UUID *u, const char *name, const INT *box )
{
	UB	*x;
	UINT	len = 0;
	char	*end, id[40], vid[40];
	BPKBUF	b;
	T_OBREF	ref;
	TS_UUID	vu;
	INT	y = 8, wd;
	ID	k;
	ER	er;
	const char *p;

	k = ob_opn_obj(cab, OB_OP_R | OB_OP_W);
	if ( k <= 0 ) return ( k < 0 ) ? k : E_NOEXS;
	x = bk_read_rec(k, 0, &len);
	if ( x == NULL ) {
		ob_cls_obj(k);
		return E_NOEXS;
	}
	/* below the links there are */
	for ( p = (const char *)x; ( p = strstr(p, "vobjbottom=\"") ) != NULL; p++ ) {
		INT	b2 = atoi(p + 12);

		if ( b2 + 8 > y ) y = b2 + 8;
	}
	end = strstr((char *)x, "</figure>");
	if ( end == NULL ) {
		box = NULL;			/* a text has no place for a box */
		end = strstr((char *)x, "</document>");
	}
	if ( end == NULL ) {
		free(x);
		ob_cls_obj(k);
		return E_OBJ;
	}
	memset(&vu, 0, sizeof(vu));
	(void)ts_gen_uuid(&vu);
	bk_uuid_str(u, id);
	bk_uuid_str(&vu, vid);
	wd = 60 + 14 * (INT)strlen(name) / 3;
	if ( wd < 120 ) wd = 120;
	if ( wd > 480 ) wd = 480;
	bpk_buf_init(&b);
	bpk_buf_putn(&b, (const char *)x, (INT)( end - (char *)x ));
	bpk_buf_printf(&b, "<link id=\"%s_0.xtad\" vobjid=\"%s\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\""
		       " vobjbottom=\"%d\" height=\"%d\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\""
		       " tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>\n",
		       id, vid, ( box != NULL ) ? box[0] : 8, ( box != NULL ) ? box[1] : y,
		       ( box != NULL ) ? box[2] : 8 + wd, ( box != NULL ) ? box[3] : y + 25,
		       ( box != NULL ) ? box[3] - box[1] : 25);
	bpk_buf_puts(&b, end);
	er = b.fail ? E_NOMEM : bk_write_all(k, 0, b.s, (UINT)b.n);
	if ( er >= E_OK ) er = ob_trn_rec(k, 0, (UD)b.n);
	if ( er >= E_OK && ob_ref_obj(cab, &ref) >= E_OK && !( ref.flags & OB_F_AUTOREF ) ) er = ob_lnk_obj(u);
	bpk_buf_free(&b);
	free(x);
	ob_cls_obj(k);
	return er;
}

/* The number after name=" in a tag, from p to e; FALSE when it is not there */
LOCAL BOOL attr_num( const char *p, const char *e, const char *name, INT *v )
{
	INT	n = (INT)strlen(name);

	for ( ; p + n + 2 < e; p++ ) {
		if ( memcmp(p, name, (size_t)n) == 0 && p[n] == '=' && p[n + 1] == '"'
		  && ( p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r' ) ) {
			*v = atoi(p + n + 2);
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT BOOL bk_tag_box( const char *p, const char *e, INT box[4] )
{
	return (BOOL)( attr_num(p, e, "vobjleft", &box[0]) && attr_num(p, e, "vobjtop", &box[1])
		       && attr_num(p, e, "vobjright", &box[2]) && attr_num(p, e, "vobjbottom", &box[3])
		       && box[2] > box[0] && box[3] > box[1] );
}

EXPORT BOOL bk_link_box( const TS_UUID *parent, const TS_UUID *u, INT box[4] )
{
	UB	*x;
	UINT	len = 0;
	char	id[40];
	const char *p, *e;
	BOOL	got = FALSE;
	ID	k = ob_opn_obj(parent, OB_OP_R);

	if ( k <= 0 ) return FALSE;
	x = bk_read_rec(k, 0, &len);
	ob_cls_obj(k);
	if ( x == NULL ) return FALSE;
	bk_uuid_str(u, id);
	for ( p = (const char *)x; !got && ( p = strstr(p, "<link") ) != NULL; p++ ) {
		const char *i = strstr(p, " id=\"");

		e = strchr(p, '>');
		if ( i == NULL || e == NULL || i > e || strncmp(i + 5, id, 36) != 0 ) continue;
		got = bk_tag_box(p, e, box);
	}
	free(x);
	return got;
}

/* The links to u taken out of a cabinet's record 0, and no longer counted */
EXPORT ER bk_link_out( const TS_UUID *cab, const TS_UUID *u )
{
	UB	*x;
	UINT	len = 0;
	char	id[40];
	BPKBUF	b;
	T_OBREF	ref;
	const char *p, *s, *e;
	INT	gone = 0;
	ID	k;
	ER	er = E_OK;

	k = ob_opn_obj(cab, OB_OP_R | OB_OP_W);
	if ( k <= 0 ) return ( k < 0 ) ? k : E_NOEXS;
	x = bk_read_rec(k, 0, &len);
	if ( x == NULL ) {
		ob_cls_obj(k);
		return E_NOEXS;
	}
	bk_uuid_str(u, id);
	bpk_buf_init(&b);
	for ( s = (const char *)x; ( p = strstr(s, "<link") ) != NULL; ) {
		const char *i = strstr(p, " id=\"");

		e = strchr(p, '>');
		if ( e == NULL ) break;
		if ( i == NULL || i > e || strncmp(i + 5, id, 36) != 0 ) {
			bpk_buf_putn(&b, s, (INT)( e + 1 - s ));
			s = e + 1;
			continue;
		}
		if ( e[-1] != '/' ) {
			const char *end = strstr(e, "</link>");

			if ( end != NULL ) e = end + 6;
		}
		bpk_buf_putn(&b, s, (INT)( p - s ));
		s = e + 1;
		if ( *s == '\n' ) s++;
		gone++;
	}
	bpk_buf_puts(&b, s);
	if ( gone > 0 ) {
		er = b.fail ? E_NOMEM : bk_write_all(k, 0, b.s, (UINT)b.n);
		if ( er >= E_OK ) er = ob_trn_rec(k, 0, (UD)b.n);
		if ( er >= E_OK && ob_ref_obj(cab, &ref) >= E_OK && !( ref.flags & OB_F_AUTOREF ) ) {
			while ( gone-- > 0 ) (void)ob_unl_obj(u);
		}
	}
	bpk_buf_free(&b);
	free(x);
	ob_cls_obj(k);
	return er;
}

EXPORT ER bk_first_cabinet( TS_UUID *u )
{
	return bk_uuid_parse(SYSDEF_CABINET, u) ? E_OK : E_NOEXS;
}

/* A volume opened to write: a new file, or a new object linked from the cabinet */
EXPORT ER bk_vol_create( BKVOL *v, const char *name, const char *memo )
{
	char	file[FS_NAME_MAX * 2];

	v->fd = -1;
	v->key = 0;
	v->wb = NULL;
	v->wn = 0;
	if ( v->dir[0] != 0 ) {
		bk_vol_file(name, file, sizeof(file) - 8);
		snprintf(v->path, sizeof(v->path), "%s/%s", v->dir, file);
		(void)fs_unlink(v->path);
		v->fd = fs_open(v->path, O_RDWR | O_CREAT | O_TRUNC);
		return ( v->fd >= 0 ) ? E_OK : v->fd;
	} else {
		BPKBUF	m, x;
		T_OBCRE	c;
		TS_UUID	cab;
		INT	rec = -1;
		ER	er;

		static const TS_UUID zero;

		if ( memcmp(&v->into, &zero, sizeof(zero)) != 0 ) cab = v->into;
		else (void)bk_first_cabinet(&cab);
		bk_vol_file(name, file, sizeof(file) - 8);
		bpk_buf_init(&m);
		bpk_buf_puts(&m, "{\"name\":\"");
		bpk_buf_json(&m, name);
		bpk_buf_puts(&m, "\",\"relationship\":[],\"linktype\":false,\"editable\":true,\"deletable\":true,"
			     "\"readable\":true,\"maker\":\"バックアップ\",\"applist\":{\"" BK_PROG_ID "\":"
			     "{\"name\":\"バックアップ\",\"defaultOpen\":true}},\"tessronos\":{\"file\":{\"name\":\"");
		bpk_buf_json(&m, file);
		bpk_buf_puts(&m, "\",\"mediatype\":\"application/x-btron-backup\"}}}");
		memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = (const UB *)m.s;
		c.jsonsz = m.n;
		c.near = cab;
		er = m.fail ? E_NOMEM : ob_cre_obj(&c, &v->obj);
		bpk_buf_free(&m);
		if ( er < E_OK ) return er;
		v->key = ob_opn_obj(&v->obj, OB_OP_ALL);
		if ( v->key <= 0 ) return ( v->key < 0 ) ? v->key : E_NOEXS;
		bpk_buf_init(&x);
		bpk_buf_puts(&x, "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>");
		bpk_buf_xml(&x, name);
		bpk_buf_puts(&x, "</p><p>BTRON のバックアップの巻。メモ：");
		bpk_buf_xml(&x, memo);
		bpk_buf_puts(&x, "</p></document></tad>\n");
		er = ob_apd_rec(v->key, OB_RT_TAD, 0, &rec);
		if ( er >= E_OK ) er = bk_write_all(v->key, rec, x.s, (UINT)x.n);
		bpk_buf_free(&x);
		if ( er >= E_OK ) er = ob_apd_rec(v->key, OB_RT_SYSDATA, 0, &rec);
		if ( er >= E_OK && rec != 1 ) er = E_OBJ;
		if ( er >= E_OK ) er = bk_link_into(&cab, &v->obj, name);
		return er;
	}
}

EXPORT BK_OUT bk_vol_writer( BKVOL *v )
{
	(void)v;
	return vol_out;
}

EXPORT ER bk_vol_done( BKVOL *v )
{
	ER	er = E_OK, er2;

	if ( v->wb != NULL ) {
		if ( v->fd >= 0 || v->key > 0 ) er = wflush(v);
		free(v->wb);
		v->wb = NULL;
		v->wn = 0;
	}
	if ( v->fd >= 0 ) {
		er2 = fs_close(v->fd);
		if ( er >= E_OK ) er = er2;
		(void)fs_sync();
	}
	if ( v->key > 0 ) ob_cls_obj(v->key);
	v->fd = -1;
	v->key = 0;
	return er;
}

/* ---------------------------------------------------------------- reading */

LOCAL INT file_in( void *arg, UB *buf, INT len )
{
	BKVOL	*v = arg;
	INT	got = 0, n;

	while ( got < len ) {
		n = fs_read(v->fd, buf + got, len - got);
		if ( n < 0 ) return ( got > 0 ) ? got : E_IO;
		if ( n == 0 ) break;
		got += n;
	}
	return got;
}

LOCAL INT obj_in( void *arg, UB *buf, INT len )
{
	BKVOL	*v = arg;
	SZ	asz = 0;

	if ( v->at >= v->size ) return 0;
	if ( (UD)len > v->size - v->at ) len = (INT)( v->size - v->at );
	if ( ob_rea_rec(v->key, v->rec, (D)v->at, buf, len, &asz) < E_OK ) return E_IO;
	v->at += (UD)asz;
	return (INT)asz;
}

/* Whether bytes start a volume: TS_INFO, then the 0xFFFD head */
LOCAL BOOL is_volume( const UB *h, INT n )
{
	return (BOOL)( n >= 16 && h[0] == 0xE0 && h[1] == 0xFF && h[10] == 0xFD && h[11] == 0xFF
		       && ( h[15] == BK_KIND_VOL || h[15] == BK_KIND_VOL2 ) );
}

/* A volume opened to read: the file at v->path, or the record of v->obj that holds one */
EXPORT ER bk_vol_open( BKVOL *v )
{
	v->at = 0;
	v->fd = -1;
	v->key = 0;
	v->wb = NULL;
	v->wn = 0;
	if ( v->path[0] != 0 ) {
		UB	h[16];

		v->fd = fs_open(v->path, O_RDONLY);
		if ( v->fd < 0 ) return v->fd;
		if ( fs_read(v->fd, h, 16) != 16 || !is_volume(h, 16) ) {
			fs_close(v->fd);
			v->fd = -1;
			return E_OBJ;
		}
		(void)fs_lseek(v->fd, 0, SEEK_SET_);
		return E_OK;
	} else {
		T_OBREC	r[16];
		INT	cnt = 0, i;

		v->key = ob_opn_obj(&v->obj, OB_OP_R);
		if ( v->key <= 0 ) return ( v->key < 0 ) ? v->key : E_NOEXS;
		(void)ob_lst_rec(v->key, r, 16, &cnt);
		for ( i = 0; i < cnt && i < 16; i++ ) {
			UB	h[16];
			SZ	asz = 0;

			if ( ob_rea_rec(v->key, r[i].recno, 0, h, 16, &asz) >= E_OK && is_volume(h, (INT)asz) ) {
				v->rec = r[i].recno;
				v->size = r[i].size;
				return E_OK;
			}
		}
		ob_cls_obj(v->key);
		v->key = 0;
		return E_OBJ;
	}
}

EXPORT BK_SRC bk_vol_reader( BKVOL *v )
{
	return ( v->fd >= 0 ) ? file_in : obj_in;
}

EXPORT void bk_vol_rewind( BKVOL *v )
{
	v->at = 0;
	if ( v->fd >= 0 ) (void)fs_lseek(v->fd, 0, SEEK_SET_);
}

/* The name of the volume: the object's, or the file's without ".TAD" */
EXPORT void bk_vol_label( BKVOL *v, char *out, INT max )
{
	out[0] = 0;
	if ( v->path[0] != 0 ) {
		const char *s = strrchr(v->path, '/');
		char	tmp[FS_NAME_MAX * 2];
		INT	n;

		s = ( s != NULL ) ? s + 1 : v->path;
		strncpy(tmp, s, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = 0;
		n = (INT)strlen(tmp);
		if ( n > 4 && tmp[n - 4] == '.' ) tmp[n - 4] = 0;
		(void)xfu_name_in((const UB *)tmp, (UB *)out, max);
	} else {
		T_OBREF	r;

		if ( ob_ref_obj(&v->obj, &r) >= E_OK ) {
			strncpy(out, (const char *)r.name, (size_t)max - 1);
			out[max - 1] = 0;
		}
	}
}
