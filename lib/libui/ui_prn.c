/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ui_prn.c
 *	The object a process hands its pages to the desktop in, for 印刷
 *	(design 11.22.6)
 *
 *	Only the object's calls are used, so the same code serves a process
 *	(through the SVC) and the kernel's tests (directly).
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/uiprn.h>

#define UIPRN_META	"{\"name\":\"印刷のページ\",\"relationship\":[],\"refCount\":0," \
			"\"editable\":true,\"deletable\":true,\"readable\":true}"

LOCAL SZ slen( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}
	return n;
}

EXPORT ER ui_prn_open( T_UIPRN *p, CONST TS_UUID *near )
{
	T_OBCRE	c;
	UB	*z = (UB *)&c;
	SZ	i;
	ER	er;

	for ( i = 0; i < (SZ)sizeof(c); i++ ) {
		z[i] = 0;
	}
	z = (UB *)p;
	for ( i = 0; i < (SZ)sizeof(*p); i++ ) {
		z[i] = 0;
	}
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)UIPRN_META;
	c.jsonsz = slen(UIPRN_META);
	if ( near != NULL ) {
		c.near = *near;
	}
	er = ob_cre_obj(&c, &p->obj);
	if ( er < E_OK ) {
		p->er = er;
		return er;
	}
	p->key = ob_opn_obj(&p->obj, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* written page by page */
	if ( p->key <= 0 ) {
		er = ( p->key < 0 ) ? (ER)p->key : E_OBJ;
		(void)ob_del_obj(&p->obj);
		p->key = 0;
		p->er = er;
		return er;
	}
	return E_OK;
}

EXPORT ER ui_prn_page( T_UIPRN *p, CONST UB *xml, INT len )
{
	INT	rec = -1, at = 0;
	SZ	asz = 0;
	ER	er;

	if ( p->key <= 0 || xml == NULL || len <= 0 ) {
		return E_PAR;
	}
	if ( p->er < E_OK ) {
		return p->er;
	}
	er = ob_apd_rec(p->key, OB_RT_TAD, 0, &rec);
	/* in pieces: a page of a large table runs to hundreds of kilobytes */
	while ( er >= E_OK && at < len ) {
		INT	n = ( len - at > 65536 ) ? 65536 : len - at;

		er = ob_wri_rec(p->key, rec, (D)at, xml + at, (SZ)n, &asz);
		at += n;
	}
	if ( er < E_OK ) {
		p->er = er;
		return er;
	}
	p->pages++;
	return E_OK;
}

EXPORT ER ui_prn_ready( T_UIPRN *p )
{
	if ( p->key > 0 ) {
		(void)ob_cls_obj(p->key);
		p->key = 0;
	}
	if ( p->er < E_OK ) {
		return p->er;
	}
	return ( p->pages > 0 ) ? E_OK : E_NOEXS;
}

EXPORT void ui_prn_close( T_UIPRN *p )
{
	if ( p->key > 0 ) {
		(void)ob_cls_obj(p->key);
		p->key = 0;
	}
	(void)ob_del_obj(&p->obj);
}
