/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om_obj.c
 *	What the store reads and writes of a real object, through the
 *	basic operations of the object layer (design 18.5)
 *
 *	The metadata, a record, a resource, each read whole or written
 *	whole. Each call opens the object with just what it needs, so the
 *	protection is asked every time and a key is never held between
 *	calls: the store keeps what it has read, not the objects open.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/tad.h>

/* ---------------------------------------------------------------- the metadata */

EXPORT UB *om_obj_meta( CONST TS_UUID *id, SZ *p_size )
{
	UB	*buf, *fit;
	SZ	n = 0;
	ID	key;

	key = ob_opn_obj(id, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return NULL;
	}
	buf = (UB *)Kmalloc(OB_ATR_MAX + 1);
	if ( buf == NULL || ob_get_atr(key, buf, OB_ATR_MAX, &n) < E_OK || n <= 0 ) {
		ob_cls_obj(key);
		if ( buf != NULL ) Kfree(buf);
		return NULL;
	}
	ob_cls_obj(key);

	/* no more room kept than the text takes */
	fit = (UB *)Kmalloc(n + 1);
	if ( fit != NULL ) {
		knl_memcpy(fit, buf, (INT)n);
		Kfree(buf);
		buf = fit;
	}
	buf[n] = 0;
	if ( p_size != NULL ) {
		*p_size = n;
	}
	return buf;
}

EXPORT ER om_obj_meta_put( CONST TS_UUID *id, CONST UB *buf, SZ size )
{
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = ob_set_atr(key, buf, size);
	ob_cls_obj(key);

	return er;
}

/* ---------------------------------------------------------------- records */

EXPORT INT om_obj_records( CONST TS_UUID *id, T_OBREC *buf, INT max )
{
	INT	cnt = 0;
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return (INT)key;
	}
	er = ob_lst_rec(key, buf, max, &cnt);
	ob_cls_obj(key);

	return ( er < E_OK ) ? (INT)er : cnt;
}

/* A record read whole, with a nought after it; the caller owns it */
EXPORT UB *om_obj_record( CONST TS_UUID *id, INT recno, SZ *p_size )
{
	T_OBREC	rec[OM_REC_MAX];
	UB	*buf;
	SZ	size = 0, got = 0;
	INT	cnt = 0, i;
	ID	key;

	key = ob_opn_obj(id, OB_OP_READ | OB_OP_ATRRD);
	if ( key < E_OK ) {
		return NULL;
	}
	if ( ob_lst_rec(key, rec, OM_REC_MAX, &cnt) < E_OK ) {
		ob_cls_obj(key);
		return NULL;
	}
	for ( i = 0; i < cnt && rec[i].recno != recno; i++ ) ;
	if ( i == cnt ) {
		ob_cls_obj(key);
		return NULL;
	}
	size = (SZ)rec[i].size;
	buf = (UB *)Kmalloc(size + 1);
	if ( buf == NULL ) {
		ob_cls_obj(key);
		return NULL;
	}
	while ( got < size ) {
		SZ	n = 0;

		if ( ob_rea_rec(key, recno, got, buf + got, size - got, &n) < E_OK || n <= 0 ) {
			break;
		}
		got += n;
	}
	ob_cls_obj(key);
	buf[got] = 0;
	if ( p_size != NULL ) {
		*p_size = got;
	}
	return buf;
}

/*
 * A record written whole: the one there replaced, or, when it is the
 * next number, added with the type given.
 */
EXPORT ER om_obj_record_put( CONST TS_UUID *id, INT recno, UINT rt, UINT sub,
			     CONST UB *buf, SZ size )
{
	T_OBREC	rec[OM_REC_MAX];
	INT	cnt = 0, n;
	SZ	done = 0;
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRRD);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = ob_lst_rec(key, rec, OM_REC_MAX, &cnt);
	if ( er >= E_OK && recno == cnt ) {
		er = ob_apd_rec(key, rt, sub, &n);
	} else if ( er >= E_OK && ( recno < 0 || recno > cnt ) ) {
		er = E_PAR;
	}
	while ( er >= E_OK && done < size ) {
		SZ	w = 0;

		er = ob_wri_rec(key, recno, done, buf + done, size - done, &w);
		if ( er >= E_OK && w <= 0 ) er = E_IO;
		done += w;
	}
	if ( er >= E_OK ) {
		er = ob_trn_rec(key, recno, (UD)size);	/* what was longer is cut */
	}
	ob_cls_obj(key);

	return er;
}

/* ---------------------------------------------------------------- resources */

/* "<uuid>_0_1.png": the object, and the resource's name after it */
EXPORT BOOL om_obj_href( CONST UB *href, TS_UUID *id, CONST UB **p_name )
{
	char	txt[TS_UUID_STRLEN + 1];
	INT	k;

	for ( k = 0; k < TS_UUID_STRLEN; k++ ) {
		if ( href[k] == 0 ) return FALSE;
		txt[k] = (char)href[k];
	}
	txt[k] = 0;
	if ( ts_str_to_uuid(txt, id) < E_OK
	  || ( href[k] != '_' && href[k] != '.' ) ) {
		return FALSE;
	}
	if ( p_name != NULL ) {
		*p_name = href + k;
	}
	return TRUE;
}

/* A resource read whole; the caller owns it */
EXPORT UB *om_obj_res( CONST TS_UUID *id, CONST UB *name, SZ *p_size )
{
	UB	*buf = NULL, *nb;
	SZ	cap = 0, got = 0;
	ID	key;

	key = ob_opn_obj(id, OB_OP_READ);
	if ( key < E_OK ) {
		return NULL;
	}
	for (;;) {
		SZ	n = 0;

		if ( got == cap ) {
			cap = ( cap == 0 ) ? 16384 : cap * 2;
			nb = (UB *)Kmalloc(cap + 1);
			if ( nb == NULL ) {
				break;
			}
			if ( buf != NULL ) {
				knl_memcpy(nb, buf, (INT)got);
				Kfree(buf);
			}
			buf = nb;
		}
		if ( ob_rea_res(key, name, got, buf + got, cap - got, &n) < E_OK || n <= 0 ) {
			break;
		}
		got += n;
	}
	ob_cls_obj(key);
	if ( buf == NULL || got == 0 ) {
		if ( buf != NULL ) Kfree(buf);
		return NULL;
	}
	buf[got] = 0;
	if ( p_size != NULL ) {
		*p_size = got;
	}
	return buf;
}

EXPORT ER om_obj_res_put( CONST TS_UUID *id, CONST UB *name, CONST UB *buf, SZ size )
{
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_WRITE);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = ob_wri_res(key, name, buf, size);
	ob_cls_obj(key);

	return er;
}

/* The icon read whole, the caller's to Kfree; NULL when it has none */
EXPORT UB *om_obj_icon( CONST TS_UUID *id, SZ *p_size )
{
	UB	*buf = NULL;
	SZ	len = 0;
	ID	key;

	key = ob_opn_obj(id, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return NULL;
	}
	if ( ob_get_ico(key, NULL, 0, &len) >= E_OK && len > 0 ) {
		buf = (UB *)Kmalloc(len);
		if ( buf != NULL && ob_get_ico(key, buf, len, &len) < E_OK ) {
			Kfree(buf);
			buf = NULL;
		}
	}
	ob_cls_obj(key);
	if ( buf != NULL && p_size != NULL ) {
		*p_size = len;
	}
	return buf;
}

/* The icon replaced; size 0 takes it away */
EXPORT ER om_obj_icon_put( CONST TS_UUID *id, CONST UB *buf, SZ size )
{
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_ATRWR);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = ob_set_ico(key, buf, size);
	ob_cls_obj(key);

	return er;
}

/* The names of an object's resources, each ending in 0: how many */
EXPORT INT om_obj_res_list( CONST TS_UUID *id, UB *buf, SZ size )
{
	INT	cnt = 0;
	ID	key;
	ER	er;

	key = ob_opn_obj(id, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return (INT)key;
	}
	er = ob_lst_res(key, buf, size, &cnt);
	ob_cls_obj(key);

	return ( er < E_OK ) ? 0 : cnt;
}

/* ---------------------------------------------------------------- a box */

/* A box's record 0 parsed; NULL when it cannot be */
LOCAL T_TAD *box_read( CONST TS_UUID *box, ER *p_er )
{
	T_TAD	*doc = NULL;
	UB	*rec;
	SZ	size = 0;

	rec = om_obj_record(box, 0, &size);
	if ( rec == NULL ) {
		*p_er = E_NOEXS;
		return NULL;
	}
	*p_er = tad_parse(rec, size, NULL, &doc);
	Kfree(rec);
	return ( *p_er >= E_OK ) ? doc : NULL;
}

/* A box's record 0 written back, and the document freed */
LOCAL ER box_write( CONST TS_UUID *box, T_TAD *doc )
{
	UB	*out;
	SZ	size = 0;
	ER	er;

	(void)tad_write_mem(doc, NULL, 0, &size);
	out = (UB *)Kmalloc(size + 1);
	er = ( out != NULL ) ? tad_write_mem(doc, out, size + 1, &size) : E_NOMEM;
	if ( er >= E_OK ) {
		er = om_obj_record_put(box, 0, OB_RT_TAD, 0, out, size);
	}
	if ( out != NULL ) Kfree(out);
	tad_free(doc);
	return er;
}

/*
 * A link to an object added to a box, a figure of links: below the
 * lowest link there, as wide as the box's links usually are. A box that
 * links to it already is left as it is.
 */
EXPORT ER om_obj_link_add( CONST TS_UUID *box, CONST TS_UUID *uuid )
{
	T_TAD	*doc;
	T_VOBJ	v;
	TS_UUID	id;
	INT	i, n, low = 0;
	ER	er;

	doc = box_read(box, &er);
	if ( doc == NULL ) {
		return er;
	}
	n = tad_lnk_count(doc);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(doc, i, &v) < E_OK ) continue;
		if ( ts_uuid_cmp(&v.target, uuid) == 0 ) {
			tad_free(doc);
			return E_OK;
		}
		if ( v.bottom > low ) low = v.bottom;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = *uuid;
	v.left = 8;
	v.top = low + 8;
	v.right = 248;
	v.bottom = v.top + 25;
	v.chsz = 14;
	v.frcol = 0x000000;
	v.chcol = 0x000000;
	v.tbcol = 0xffffff;
	v.bgcol = 0xffffff;
	v.height = 25;
	v.zoom = 100;
	v.disp = TAD_D_PICT | TAD_D_NAME | TAD_D_FRAME;
	er = tad_lnk_add(doc, &v, &id);
	if ( er < E_OK ) {
		tad_free(doc);
		return er;
	}
	return box_write(box, doc);
}

/* Every link of a box to an object taken out */
EXPORT ER om_obj_link_del( CONST TS_UUID *box, CONST TS_UUID *uuid )
{
	T_TAD	*doc;
	T_VOBJ	v;
	INT	i, gone = 0;
	ER	er;

	doc = box_read(box, &er);
	if ( doc == NULL ) {
		return er;
	}
	for ( i = tad_lnk_count(doc) - 1; i >= 0; i-- ) {
		if ( tad_lnk_get(doc, i, &v) >= E_OK && ts_uuid_cmp(&v.target, uuid) == 0
		  && tad_lnk_del(doc, &v.vobjid) >= E_OK ) {
			gone++;
		}
	}
	if ( gone == 0 ) {
		tad_free(doc);
		return E_OK;
	}
	return box_write(box, doc);
}
