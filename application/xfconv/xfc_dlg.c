/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_dlg.c
 *	ファイル変換: what it asks, in panels of its data box (design 17.17)
 *
 *	Each panel is the window manager's, made from a definition of the
 *	data box SYSDEF_BOX_XFCONV and put over the middle of the window it
 *	is asked for; the parts are read and written by their numbers. While
 *	a panel is up, what comes for the accessory's windows is handed to
 *	xfc_meanwhile, so that they are still drawn.
 */

#include "xfc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/sysdef.h>

/* The panels and their parts, by the data box's numbers */
#define PN_FTP		20
#define PN_IMPORT	21
#define PN_EXPORT	22
#define PN_TELL		23

#define F_HOST		11
#define F_PORT		12
#define F_SSH		13
#define F_USER		14
#define F_PASS		15
#define F_KEEP		16
#define F_PASV		17
#define F_LPORT		18
#define F_ENC		19
#define F_NAME		20
#define F_NOTE		21

#define I_WHAT		1
#define I_HOW		11
#define I_ENC		12
#define I_SHRINK	13

#define X_WHAT		1
#define X_WHERE		2
#define X_NAME		11
#define X_HOW		12
#define X_LAYOUT	13

#define B_CANCEL	101
#define B_OK		102

static ID	chan;

EXPORT void xfc_dlg_init( ID port )
{
	chan = port;
}

/* The window a panel is asked for, as libui wants to know it */
static BOOL host_of( XWIN *w, UI *u )
{
	memset(u, 0, sizeof(*u));
	if ( ui_uuid(SYSDEF_BOX_XFCONV, &u->box) < E_OK ) return FALSE;
	u->kw = w->kw;
	u->port = chan;
	xfc_work_size(w, &u->w, &u->h);
	return TRUE;
}

/*
 * The panel worked until a push button is pushed (its number), or Esc
 * or its window closed (0). changed is told of each other part changed.
 */
static INT run( UI *u, UI *q, void (*changed)( UI *q, INT num, void *arg ), void *arg )
{
	T_OBNTM	m;
	INT	num, ev;

	for (;;) {
		ev = ui_dlg_next(u, q, &m, &num);
		switch ( ev ) {
		case UI_EV_BUTTON:
			return num;
		case UI_EV_CLOSE:
			return 0;
		case UI_EV_CHANGE:
			if ( changed != NULL ) changed(q, num, arg);
			break;
		case UI_EV_OTHER:
			xfc_meanwhile(&m);
			break;
		case UI_EV_NONE:
			(void)tk_dly_tsk(20);
			break;
		default:
			break;
		}
	}
}

/* A name cut to fit a line of the panel, at a character */
static void short_name( CONST UB *s, char *out, INT max )
{
	INT	n = (INT)strlen((CONST char *)s);

	if ( n > max - 4 ) {
		n = max - 4;
		while ( n > 0 && ( s[n] & 0xC0 ) == 0x80 ) n--;
		memcpy(out, s, n);
		memcpy(out + n, "…", 4);
		out[n + 3] = 0;
	} else {
		memcpy(out, s, n + 1);
	}
}

/* ---------------------------------------------------------------- files to 実身 */

EXPORT BOOL xfc_ask_import( XWIN *w, INT n, CONST UB *first, XFCIMP *how )
{
	static CONST UINT	as[] = { XFU_AS_AUTO, XFU_AS_TEXT, XFU_AS_ZEN, XFU_AS_IMAGE, XFU_AS_RAW,
					 XFU_AS_ARCHIVE };
	static CONST INT	enc[] = { TXC_AUTO, TXC_SJIS, TXC_EUCJP, TXC_JIS, TXC_UTF8 };
	UI	u, q;
	char	s[200], nm[80];
	INT	k, v;

	if ( !host_of(w, &u) || ui_dlg_open(&u, PN_IMPORT, &q) < E_OK ) {
		xfc_log("no import panel");
		return FALSE;
	}
	short_name(first, nm, 40);
	if ( n > 1 ) snprintf(s, sizeof(s), "「%s」ほか %d 個を実身にします", nm, n - 1);
	else snprintf(s, sizeof(s), "「%s」を実身にします", nm);
	ui_set_text(&q, I_WHAT, s);
	snprintf(s, sizeof(s), "asked how to import %d: %s", n, first);
	xfc_log(s);
	k = run(&u, &q, NULL, NULL);
	memset(how, 0, sizeof(*how));
	v = ui_value(&q, I_HOW);
	how->as = ( v >= 1 && v <= (INT)( sizeof(as) / sizeof(as[0]) ) ) ? as[v - 1] : XFU_AS_AUTO;
	v = ui_value(&q, I_ENC);
	how->enc = ( v >= 1 && v <= (INT)( sizeof(enc) / sizeof(enc[0]) ) ) ? enc[v - 1] : TXC_AUTO;
	v = ui_value(&q, I_SHRINK);
	how->shrink = ( v >= 2 && v <= 4 ) ? ( 1 << ( v - 1 ) ) : 1;
	ui_dlg_close(&q);
	if ( k != B_OK ) {
		xfc_log("import cancelled");
		return FALSE;
	}
	snprintf(s, sizeof(s), "import as %u enc %d shrink %d", how->as, how->enc, how->shrink);
	xfc_log(s);
	return TRUE;
}

/* ---------------------------------------------------------------- 実身 to files */

EXPORT BOOL xfc_ask_export( XWIN *w, INT n, CONST UB *first, CONST UB *dir, XFCEXP *how,
			    BOOL tadjs, BOOL layout, BOOL sjis )
{
	UI	u, q;
	char	s[300], nm[80];
	INT	k, v;

	if ( !host_of(w, &u) || ui_dlg_open(&u, PN_EXPORT, &q) < E_OK ) {
		xfc_log("no export panel");
		return FALSE;
	}
	short_name(first, nm, 40);
	if ( n > 1 ) {
		snprintf(s, sizeof(s), "「%s」ほか %d 個をファイルにします", nm, n - 1);
		ui_off(&q, X_NAME, TRUE);
	} else {
		snprintf(s, sizeof(s), "「%s」をファイルにします（名前を空けると元の名前）", nm);
	}
	ui_set_text(&q, X_WHAT, s);
	short_name(dir, nm, 70);
	snprintf(s, sizeof(s), "書出先：%s/%s", w->label, nm);
	ui_set_text(&q, X_WHERE, s);
	ui_set_value(&q, X_HOW, tadjs ? 5 : sjis ? 2 : 1);
	ui_set_value(&q, X_LAYOUT, layout ? 1 : 0);
	snprintf(s, sizeof(s), "asked how to export %d: %s", n, first);
	xfc_log(s);
	k = run(&u, &q, NULL, NULL);
	memset(how, 0, sizeof(*how));
	if ( n <= 1 ) (void)ui_text(&q, X_NAME, (char *)how->name, sizeof(how->name));
	how->outenc = TXC_AUTO;
	v = ui_value(&q, X_HOW);
	switch ( v ) {
	case 2:	how->flags = XFU_F_TEXT; how->outenc = TXC_SJIS; break;
	case 3:	how->flags = XFU_F_TEXT; how->outenc = TXC_UTF8; break;
	case 4:	how->flags = XFU_F_TAD | XFU_F_ONE; break;
	case 5:	how->flags = XFU_F_TADJS; break;
	default: break;
	}
	if ( ui_value(&q, X_LAYOUT) != 0 ) how->flags |= XFU_F_LAYOUT;
	ui_dlg_close(&q);
	if ( k != B_OK ) {
		xfc_log("export cancelled");
		return FALSE;
	}
	snprintf(s, sizeof(s), "export as %d flags %x", v, how->flags);
	xfc_log(s);
	return TRUE;
}

/* ---------------------------------------------------------------- telling */

EXPORT void xfc_tell( XWIN *w, CONST char *l1, CONST char *l2, CONST char *l3 )
{
	UI	u, q;

	if ( !host_of(w, &u) || ui_dlg_open(&u, PN_TELL, &q) < E_OK ) return;
	ui_set_text(&q, 1, ( l1 != NULL ) ? l1 : " ");
	ui_set_text(&q, 2, ( l2 != NULL ) ? l2 : " ");
	ui_set_text(&q, 3, ( l3 != NULL ) ? l3 : " ");
	(void)run(&u, &q, NULL, NULL);
	ui_dlg_close(&q);
}

/* ---------------------------------------------------------------- FTP */

/* SSH switched: the port that goes with it, when the port is still the other's */
static void ftp_changed( UI *q, INT num, void *arg )
{
	char	p[8];

	(void)arg;
	if ( num != F_SSH ) return;
	(void)ui_text(q, F_PORT, p, sizeof(p));
	if ( ui_value(q, F_SSH) != 0 && ( p[0] == 0 || strcmp(p, "21") == 0 ) ) ui_set_text(q, F_PORT, "22");
	if ( ui_value(q, F_SSH) == 0 && ( p[0] == 0 || strcmp(p, "22") == 0 ) ) ui_set_text(q, F_PORT, "21");
}

static void ftp_fields( UI *q, XFCCONN *c )
{
	INT	v;

	(void)ui_text(q, F_HOST, (char *)c->host, sizeof(c->host));
	(void)ui_text(q, F_PORT, (char *)c->port, sizeof(c->port));
	(void)ui_text(q, F_USER, (char *)c->user, sizeof(c->user));
	(void)ui_text(q, F_PASS, (char *)c->pass, sizeof(c->pass));
	(void)ui_text(q, F_LPORT, (char *)c->lport, sizeof(c->lport));
	(void)ui_text(q, F_NAME, (char *)c->name, sizeof(c->name));
	c->ssh = (BOOL)( ui_value(q, F_SSH) != 0 );
	c->keep = (BOOL)( ui_value(q, F_KEEP) != 0 );
	c->mode = ( ui_value(q, F_PASV) != 0 ) ? ( ( c->mode == XFC_DM_PASV ) ? XFC_DM_PASV : XFC_DM_EPSV )
		: XFC_DM_ACTIVE;
	v = ui_value(q, F_ENC);
	c->enc = ( v == 2 ) ? TXC_SJIS : ( v == 3 ) ? TXC_EUCJP : TXC_UTF8;
}

EXPORT BOOL xfc_ask_ftp( XWIN *w, XFCCONN *c, XFC_TRY try_connect )
{
	UI	u, q;
	char	why[200];
	INT	k;
	ER	er;

	if ( !host_of(w, &u) || ui_dlg_open(&u, PN_FTP, &q) < E_OK ) {
		xfc_log("no FTP panel");
		return FALSE;
	}
	ui_set_text(&q, F_HOST, (CONST char *)c->host);
	ui_set_text(&q, F_PORT, c->port[0] != 0 ? (CONST char *)c->port : "21");
	ui_set_value(&q, F_SSH, c->ssh ? 1 : 0);
	ui_set_text(&q, F_USER, (CONST char *)c->user);
	ui_set_text(&q, F_PASS, (CONST char *)c->pass);
	ui_set_value(&q, F_KEEP, c->keep ? 1 : 0);
	ui_set_value(&q, F_PASV, ( c->mode == XFC_DM_ACTIVE ) ? 0 : 1);
	ui_set_text(&q, F_LPORT, (CONST char *)c->lport);
	ui_set_value(&q, F_ENC, ( c->enc == TXC_SJIS ) ? 2 : ( c->enc == TXC_EUCJP ) ? 3 : 1);
	ui_set_text(&q, F_NAME, (CONST char *)c->name);
	ui_set_text(&q, F_NOTE, "ユーザ名を空けると anonymous で接続します");
	xfc_log("asked for an FTP connection");
	for (;;) {
		k = run(&u, &q, ftp_changed, NULL);
		if ( k != B_OK ) {
			ui_dlg_close(&q);
			xfc_log("ftp cancelled");
			return FALSE;
		}
		ftp_fields(&q, c);
		if ( c->host[0] == 0 ) {
			ui_set_text(&q, F_NOTE, "接続先を入れてください");
			continue;
		}
		ui_set_text(&q, F_NOTE, "接続しています…");
		why[0] = 0;
		er = try_connect(c, why, sizeof(why));
		if ( er >= E_OK ) break;
		ui_set_text(&q, F_NOTE, why[0] != 0 ? why : "接続できません");
	}
	ui_dlg_close(&q);
	return TRUE;
}
