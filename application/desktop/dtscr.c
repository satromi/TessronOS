/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtscr.c
 *	画面印刷 as the desktop serves it: the screen taken as pixels and
 *	kept, put in the tray or printed (docs/tessronos/11-desktop.md 11.24)
 *
 *	The accessory is a process (application/scrprint). Chosen from 小物,
 *	it shows the panel 画面印刷／画面保存: where the picture goes
 *	(図形実身へ保存, トレーへ複写, プリンタへ印刷), whether the pointer is
 *	in it (ポインタ印刷), what is taken (全画面, the window in front with
 *	those subordinate to it, or a rectangle dragged out on the screen),
 *	and when (so many seconds after 実行, at the PrintScreen key, or at
 *	every press of it until the panel is put away with 取り消し). It asks
 *	the desktop (DT_RQ_SCRPRINT) for what was set last, which the desktop
 *	keeps in the machine's settings (SCRPRINT), and for the volumes a
 *	figure object can go on, and hands over what 実行 set.
 *
 *	What follows is the desktop's alone, for it reads the screen and
 *	every input: the panel gone, printing asks its 印刷設定 of 基本印刷
 *	(without 画面表示, for there is nothing yet to show), and a rectangle
 *	is dragged out, its outline drawn over every window while the input
 *	is told to no program. Counting down, the buzzer sounds short at
 *	three, two and one seconds left and long at nought, and the screen
 *	is taken at the long one. The message line is cleared and the screen
 *	built before it is read, so what this accessory said is not in the
 *	picture.
 *
 *	The pixels are the screen as the window manager last built it
 *	(wm_grab_screen), the pointer left as it lies or replaced by what it
 *	covers. For the window in front, the rectangle that holds it and its
 *	subordinate windows is taken and what lies outside those windows is
 *	made white; a panel standing in its own window in front of every
 *	window is taken alone. The message line is never part of a window.
 *
 *	A figure object (図形実身へ保存) holds the picture as a PNG beside
 *	its record, named "<object>_0_1.png", and one <image> of it at its
 *	full size; its layout paper is made as large as the picture (a pixel
 *	is three quarters of a point), and a link to it is put in the
 *	desktop's first window. ファイル変換 gives such a figure out as a PNG
 *	file (its first picture, XFU_F_IMAGE). For the tray no object is
 *	made: a set of two is pushed, a figure fragment of one <image> whose
 *	href "tray:1" names the set's second record, and the picture as a
 *	PNG, which a figure pasting the fragment writes beside its own
 *	record. Printing lays the picture on one page of a layout paper its
 *	size.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/tad.h>
#include <ts/tray.h>
#include <ts/img.h>
#include <ts/print.h>
#include <ts/time.h>
#include <ts/uuid.h>
#include <ts/proc.h>
#include <ts/dtreq.h>
#include "desktop.h"

#define KEY_PRTSC	0x46
#define KEY_ESC		0x29

/* where the picture goes */
#define SC_TO_FIG	0		/* 図形実身へ保存 */
#define SC_TO_TRAY	1		/* トレーへ複写 */
#define SC_TO_PRINTER	2		/* プリンタへ印刷 */

/* when it is taken */
#define SC_BY_TIME	0		/* 指定時間後に実行 */
#define SC_BY_KEY	1		/* PrntScrnキーで実行 */
#define SC_BY_KEYS	2		/* PrntScrnキーで連続実行 */

#define SC_SEC_MIN	3
#define SC_SEC_MAX	99
#define SC_SEC_DFLT	5
#define SC_BEEPS	3		/* short sounds before the long one */

#define SC_GROUP_MAX	16		/* windows of the group in front */
#define SC_DRAG_LEAST	4		/* a rectangle dragged out is at least this wide and high */
#define SC_WHITE	0x00FFFFFFU
#define SC_VOLS		12		/* volumes offered: as many as a panel holds on the screen */
#define SC_VOL_PATH	64		/* bytes of a volume's path kept */

/* what is under way */
#define SC_IDLE		0
#define SC_PICK		1		/* a rectangle being dragged out */
#define SC_TIMING	2		/* counting down */
#define SC_KEYING	3		/* waiting for PrintScreen */

typedef struct {
	INT	to;			/* SC_TO_* */
	BOOL	pointer;		/* ポインタ印刷 */
	INT	area;			/* DT_SCR_* */
	INT	by;			/* SC_BY_* */
	INT	secs;			/* 指定時間 */
} SCSET;

LOCAL SCSET	sc = { SC_TO_FIG, FALSE, DT_SCR_SCREEN, SC_BY_TIME, SC_SEC_DFLT };
LOCAL BOOL	sc_loaded = FALSE;
LOCAL INT	sc_state = SC_IDLE;
LOCAL T_DPRECT	sc_range;		/* the rectangle dragged out, on the screen */
LOCAL BOOL	sc_drag = FALSE;
LOCAL INT	sc_x0, sc_y0;
LOCAL UD	sc_due;			/* when the count reaches nought (ns) */
LOCAL INT	sc_beeped;		/* the seconds left at the last short sound */
LOCAL UB	sc_name[TAD_NAME_MAX] = "画面印刷";
LOCAL UB	sc_vol[SC_VOL_PATH];		/* the volume saved to, "" the first */
LOCAL INT	sc_taken = 0;		/* pictures kept since 実行 */
LOCAL T_PRNSET	sc_prn;

/*
 * What happened, told on the console with how many times it has, so
 * that one reading the console can wait for the next of a kind
 */
#define SN_PANEL	0
#define SN_ARMED	1
#define SN_PICK		2
#define SN_RANGE	3
#define SN_TAKEN	4
#define SN_TRAY		5
#define SN_PRINTED	6
#define SN_CANCELLED	7
#define SN_KINDS	8
LOCAL INT	sc_told[SN_KINDS];

LOCAL INT told( INT kind )
{
	return ++sc_told[kind];
}

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && at + 1 < max ) {
		b[at++] = (UB)*s++;
	}
	b[at] = 0;
	return at;
}

LOCAL INT put_num( UB *b, INT at, INT max, INT v )
{
	char	t[12];
	INT	n = 0;

	if ( v < 0 ) {
		at = put(b, at, max, "-");
		v = -v;
	}
	do {
		t[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < 11 );
	while ( n > 0 && at + 1 < max ) {
		b[at++] = (UB)t[--n];
	}
	b[at] = 0;
	return at;
}

/* Words inside an attribute of xmlTAD, the five that cannot stand there written as entities */
LOCAL INT put_attr( UB *b, INT at, INT max, CONST UB *s )
{
	for ( ; s != NULL && *s != 0; s++ ) {
		switch ( *s ) {
		case '&':	at = put(b, at, max, "&amp;");	break;
		case '<':	at = put(b, at, max, "&lt;");	break;
		case '>':	at = put(b, at, max, "&gt;");	break;
		case '"':	at = put(b, at, max, "&quot;");	break;
		case '\'':	at = put(b, at, max, "&apos;");	break;
		default:
			if ( at + 1 < max ) b[at++] = *s;
			b[at] = 0;
			break;
		}
	}
	return at;
}

LOCAL void say( CONST char *a, CONST UB *b, CONST char *c )
{
	UB	m[WM_LABEL_MAX];
	INT	at = 0;

	at = put(m, at, sizeof(m), a);
	at = put(m, at, sizeof(m), (CONST char *)b);
	(void)put(m, at, sizeof(m), c);
	(void)wm_msg_put((CONST char *)m);
}

/* ---------------------------------------------------------------- what is set */

/* "to pointer area by secs", the machine's setting SCRPRINT */
LOCAL void settings_load( void )
{
	char	v[64];
	INT	n[5], k = 0, i = 0;

	sc_loaded = TRUE;
	if ( !wm_dev_conf_get("SCRPRINT", v, sizeof(v)) ) {
		return;
	}
	while ( k < 5 && v[i] != 0 ) {
		INT	x = 0;
		BOOL	any = FALSE;

		while ( v[i] == ' ' ) i++;
		while ( v[i] >= '0' && v[i] <= '9' ) {
			x = x * 10 + ( v[i++] - '0' );
			any = TRUE;
		}
		if ( !any ) break;
		n[k++] = x;
	}
	if ( k < 5 ) {
		return;
	}
	sc.to = ( n[0] <= SC_TO_PRINTER ) ? n[0] : SC_TO_FIG;
	sc.pointer = (BOOL)( n[1] != 0 );
	sc.area = ( n[2] <= DT_SCR_RANGE ) ? n[2] : DT_SCR_SCREEN;
	sc.by = ( n[3] <= SC_BY_KEYS ) ? n[3] : SC_BY_TIME;
	sc.secs = ( n[4] < SC_SEC_MIN ) ? SC_SEC_MIN : ( n[4] > SC_SEC_MAX ) ? SC_SEC_MAX : n[4];
}

LOCAL void settings_save( void )
{
	UB	v[64];
	INT	at = 0;

	at = put_num(v, at, sizeof(v), sc.to);
	at = put(v, at, sizeof(v), " ");
	at = put_num(v, at, sizeof(v), sc.pointer ? 1 : 0);
	at = put(v, at, sizeof(v), " ");
	at = put_num(v, at, sizeof(v), sc.area);
	at = put(v, at, sizeof(v), " ");
	at = put_num(v, at, sizeof(v), sc.by);
	at = put(v, at, sizeof(v), " ");
	(void)put_num(v, at, sizeof(v), sc.secs);
	(void)wm_dev_conf_put("SCRPRINT", (CONST char *)v);
}

/* ---------------------------------------------------------------- the pixels */

LOCAL BOOL inside( CONST T_DPRECT *r, INT x, INT y )
{
	return (BOOL)( x >= r->left && x < r->right && y >= r->top && y < r->bottom );
}

/*
 * The pixels of what is taken, the caller's to Kfree, and where on the
 * screen they were: the whole screen, the window in front with the
 * windows that go with it (outside them white), or 'range' cut to the
 * screen. E_NOEXS when there is no window in front, E_PAR when the
 * range is off the screen.
 */
EXPORT ER dt_scr_grab( UINT area, BOOL pointer, CONST T_DPRECT *range, UW **p_px,
		       T_DPRECT *p_at )
{
	T_DPRECT	r, outer[SC_GROUP_MAX];
	INT		wids[SC_GROUP_MAX], n = 0, sw = 0, sh = 0, w, h, x, y, i, front;
	UW		*px;
	ER		er;

	if ( p_px == NULL || p_at == NULL ) {
		return E_PAR;
	}
	*p_px = NULL;
	wm_screen(&sw, &sh);
	if ( sw <= 0 || sh <= 0 ) {
		return E_NOEXS;
	}
	r.left = 0;
	r.top = 0;
	r.right = sw;
	r.bottom = sh;
	if ( area == DT_SCR_RANGE ) {
		if ( range == NULL ) {
			return E_PAR;
		}
		r = *range;
		if ( r.left < 0 ) r.left = 0;
		if ( r.top < 0 ) r.top = 0;
		if ( r.right > sw ) r.right = sw;
		if ( r.bottom > sh ) r.bottom = sh;
	} else if ( area == DT_SCR_FRONT ) {
		/* a panel in front of every window alone, else the window in front and its own */
		front = wm_panel_front();
		if ( front > 0 ) {
			wids[0] = front;
			n = 1;
		} else {
			n = wm_front_group(wids, SC_GROUP_MAX);
		}
		for ( i = 0; i < n; i++ ) {
			T_WMWIN	ww;

			if ( wm_ref(wids[i], &ww) < E_OK ) {
				outer[i].left = outer[i].top = outer[i].right = outer[i].bottom = 0;
				continue;
			}
			outer[i] = ww.outer;
			if ( i == 0 ) {
				r = ww.outer;
			} else {
				if ( ww.outer.left < r.left ) r.left = ww.outer.left;
				if ( ww.outer.top < r.top ) r.top = ww.outer.top;
				if ( ww.outer.right > r.right ) r.right = ww.outer.right;
				if ( ww.outer.bottom > r.bottom ) r.bottom = ww.outer.bottom;
			}
		}
		if ( n <= 0 ) {
			return E_NOEXS;
		}
		if ( r.left < 0 ) r.left = 0;
		if ( r.top < 0 ) r.top = 0;
		if ( r.right > sw ) r.right = sw;
		if ( r.bottom > sh ) r.bottom = sh;
	}
	if ( r.left >= r.right || r.top >= r.bottom ) {
		return E_PAR;
	}
	w = r.right - r.left;
	h = r.bottom - r.top;
	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	if ( px == NULL ) {
		return E_NOMEM;
	}
	er = wm_grab_screen(px, w, &r, pointer);
	if ( er < E_OK ) {
		Kfree(px);
		return er;
	}
	for ( y = 0; y < h; y++ ) {
		UW	*row = px + (SZ)y * w;

		for ( x = 0; x < w; x++ ) {
			row[x] &= SC_WHITE;
			if ( area != DT_SCR_FRONT ) {
				continue;
			}
			for ( i = 0; i < n && !inside(&outer[i], r.left + x, r.top + y); i++ ) ;
			if ( i == n ) {
				row[x] = SC_WHITE;
			}
		}
	}
	*p_px = px;
	*p_at = r;

	return E_OK;
}

/* ---------------------------------------------------------------- where it goes */

/*
 * An object's metadata given the applist of 基本図形編集, the one it
 * opens with, as the last member: what the store made keeps its place,
 * its name first among them.
 */
LOCAL ER with_app( CONST TS_UUID *u )
{
	CONST char	*add = ",\"applist\":{\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true}}";
	UB		*meta, *out;
	SZ		size = 0;
	INT		end, k, n = (INT)knl_strlen(add);
	ER		er;

	meta = om_obj_meta(u, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	for ( end = (INT)size - 1; end >= 0 && meta[end] != '}'; end-- ) ;
	out = ( end > 0 ) ? (UB *)Kmalloc(size + (SZ)n + 1) : NULL;
	if ( out == NULL ) {
		Kfree(meta);
		return ( end > 0 ) ? E_NOMEM : E_OBJ;
	}
	knl_memcpy(out, meta, (SZ)end);
	for ( k = 0; k < n; k++ ) out[end + k] = (UB)add[k];
	knl_memcpy(out + end + n, meta + end, size - (SZ)end);
	er = om_obj_meta_put(u, out, size + (SZ)n);
	Kfree(out);
	Kfree(meta);
	return er;
}

/*
 * A figure object holding the picture, on the volume attached with
 * 'vol' (NULL the first): its record a figure of one <image> at full
 * size on a layout paper as large, the picture a PNG beside it.
 * Answers its identity.
 */
EXPORT ER dt_scr_save( CONST UW *px, INT w, INT h, CONST UB *name, CONST char *vol, TS_UUID *p_uuid )
{
	TS_UUID	u;
	char	us[TS_UUID_STRLEN + 1];
	UB	href[TS_UUID_STRLEN + 16];
	UB	*x;
	INT	at = 0, max = 2048 + TAD_NAME_MAX * 6;
	ER	er;

	if ( px == NULL || w <= 0 || h <= 0 || name == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	er = om_cre_obj(vol, name, OM_BODY_FIG, &u);
	if ( er < E_OK ) {
		return er;
	}
	if ( ts_uuid_to_str(&u, us, sizeof(us)) < E_OK ) {
		(void)ob_del_obj(&u);
		return E_SYS;
	}
	at = put(href, 0, sizeof(href), us);
	(void)put(href, at, sizeof(href), "_0_1.png");
	x = (UB *)Kmalloc((SZ)max);
	if ( x == NULL ) {
		(void)ob_del_obj(&u);
		return E_NOMEM;
	}
	at = put(x, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	at = put_attr(x, at, max, name);
	at = put(x, at, max, "\">\n<paper type=\"fig\" length=\"");
	at = put_num(x, at, max, ( h * 3 + 3 ) / 4);		/* points, to hold every pixel */
	at = put(x, at, max, "\" width=\"");
	at = put_num(x, at, max, ( w * 3 + 3 ) / 4);
	at = put(x, at, max, "\" binding=\"0\" imposition=\"0\" top=\"0\" bottom=\"0\" left=\"0\" right=\"0\"/>\n");
	at = put(x, at, max, "<figure>\n<figView top=\"0\" left=\"0\" right=\"");
	at = put_num(x, at, max, w);
	at = put(x, at, max, "\" bottom=\"");
	at = put_num(x, at, max, h);
	at = put(x, at, max, "\"/>\n<figDraw top=\"0\" left=\"0\" right=\"");
	at = put_num(x, at, max, w);
	at = put(x, at, max, "\" bottom=\"");
	at = put_num(x, at, max, h);
	at = put(x, at, max, "\"/>\n<figScale hunit=\"-96\" vunit=\"-96\"/>\n");
	at = put(x, at, max, "<image lineType=\"0\" lineWidth=\"0\" l_pat=\"0\" f_pat=\"0\" angle=\"0\""
			     " rotation=\"0\" flipH=\"false\" flipV=\"false\" left=\"0\" top=\"0\" right=\"");
	at = put_num(x, at, max, w);
	at = put(x, at, max, "\" bottom=\"");
	at = put_num(x, at, max, h);
	at = put(x, at, max, "\" href=\"");
	at = put(x, at, max, (CONST char *)href);
	at = put(x, at, max, "\" zIndex=\"1\"/>\n</figure>\n</tad>\n");
	er = om_obj_record_put(&u, 0, OB_RT_TAD, 0, x, (SZ)at);
	Kfree(x);
	if ( er >= E_OK ) {
		er = om_store_put_picture(href, px, w, h);
	}
	if ( er >= E_OK ) {
		/* opened in 基本図形編集, with the icon its template gives what is made from it */
		UB	*ico;
		SZ	isz = 0;

		er = with_app(&u);
		ico = om_store_app_icon((CONST UB *)"basic-figure-editor", &isz);
		if ( ico != NULL ) {
			(void)om_obj_icon_put(&u, ico, isz);
			Kfree(ico);
		}
	}
	if ( er < E_OK ) {
		(void)ob_del_obj(&u);
		return er;
	}
	om_store_forget(&u);			/* read as it now is when it is next opened */
	*p_uuid = u;

	return E_OK;
}

/*
 * トレーへ複写: a set of two pushed onto the tray, and no object made: a
 * figure fragment of one <image> at the picture's size whose href,
 * "tray:1", names the set's second record, and the picture as a PNG.
 * Pasted into a figure, the picture is written beside that figure's
 * record (dt_tray_pictures) and becomes an image of it.
 */
EXPORT ER dt_scr_tray( CONST UW *px, INT w, INT h )
{
	UB	frag[512];
	UB	*png = NULL;
	SZ	plen = 0;
	T_TRREC	r[2];
	INT	at;
	ER	er;

	if ( px == NULL || w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	er = img_png_encode(px, w, h, &png, &plen);
	if ( er < E_OK ) {
		return er;
	}
	at = put(frag, 0, sizeof(frag), "<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
				       "<image lineType=\"0\" lineWidth=\"0\" l_pat=\"0\" f_pat=\"0\" angle=\"0\""
				       " rotation=\"0\" flipH=\"false\" flipV=\"false\" left=\"0\" top=\"0\" right=\"");
	at = put_num(frag, at, sizeof(frag), w);
	at = put(frag, at, sizeof(frag), "\" bottom=\"");
	at = put_num(frag, at, sizeof(frag), h);
	at = put(frag, at, sizeof(frag), "\" href=\"tray:1\" zIndex=\"1\"/></figure></tad>");
	knl_memset(r, 0, sizeof(r));
	r[0].kind = TR_TAD;
	r[0].data = frag;
	r[0].size = (SZ)at;
	r[1].kind = TR_PNG;
	r[1].data = png;
	r[1].size = plen;
	er = tr_psh_dat(r, 2, (CONST UB *)"画面印刷");	/* the level it went to */
	Kfree(png);

	return ( er < E_OK ) ? er : E_OK;
}

/* The picture laid at the corner of its page */
LOCAL ER draw_pic( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	CONST DTSCRPIC	*p = (CONST DTSCRPIC *)arg;

	(void)page;
	dp_fill_rect(gid, sheet, SC_WHITE);
	dp_put_argb(gid, sheet->left, sheet->top, p->px, p->w, p->w, p->h, IMG_CLEAR);

	return E_OK;
}

/* A job of one page: the picture on a layout paper its size, a pixel three quarters of a point */
EXPORT void dt_scr_job( T_PRNJOB *job, CONST DTSCRPIC *pic, CONST UB *name )
{
	knl_memset(job, 0, sizeof(*job));
	job->npage = 1;
	job->lw = pic->w;
	job->lh = pic->h;
	job->unit = 750;
	job->name = name;
	job->draw = draw_pic;
	job->arg = (void *)pic;
}

/* ---------------------------------------------------------------- taking it */

/* The screen taken and sent where it was set to go */
LOCAL void take( void )
{
	UW		*px = NULL;
	T_DPRECT	at;
	TS_UUID		u;
	UB		name[TAD_NAME_MAX];
	INT		w, h, k;
	ER		er;

	/* nothing of this accessory's on the screen when it is read */
	(void)wm_set_drag(NULL, 0);
	(void)wm_msg_put(NULL);
	(void)wm_update();
	(void)wm_flush();
	er = dt_scr_grab((UINT)sc.area, sc.pointer, &sc_range, &px, &at);
	if ( er < E_OK || px == NULL ) {
		tm_printf((UB *)"scrprint: not taken (%d)\n", (INT)er);
		(void)wm_msg_put(( er == E_NOEXS ) ? "画面印刷: 最前面のウィンドウがありません"
						   : "画面印刷: 画面を取り込めませんでした");
		return;
	}
	w = at.right - at.left;
	h = at.bottom - at.top;
	sc_taken++;
	tm_printf((UB *)"scrprint: taken %d %d %d %d %d\n", told(SN_TAKEN), at.left, at.top, at.right, at.bottom);
	switch ( sc.to ) {
	case SC_TO_FIG:
		k = put(name, 0, sizeof(name), (CONST char *)sc_name);
		if ( sc_taken > 1 ) {
			k = put(name, k, sizeof(name), "(");
			k = put_num(name, k, sizeof(name), sc_taken);
			(void)put(name, k, sizeof(name), ")");
		}
		er = dt_scr_save(px, w, h, name, ( sc_vol[0] != 0 ) ? (CONST char *)sc_vol : NULL, &u);
		if ( er >= E_OK ) {
			TS_UUID	top;

			/* on a disk of its own, in that disk's top object; else in the first window */
			if ( sc_vol[0] != 0 && dt_disk_top((CONST char *)sc_vol, &top)
			  && dt_place_in(&top, &u, name) >= E_OK ) {
				tm_printf((UB *)"scrprint: placed in the top of %s\n", (CONST char *)sc_vol);
			} else {
				dt_place_on_desktop(&u, name);
			}
			say("画面印刷: 「", name, "」に保存しました");
			tm_printf((UB *)"scrprint: saved %s\n", name);
		} else {
			(void)wm_msg_put("画面印刷: 図形実身を作れませんでした");
		}
		break;
	case SC_TO_TRAY:
		er = dt_scr_tray(px, w, h);
		(void)wm_msg_put(( er >= E_OK ) ? "画面印刷: トレーに複写しました"
						: "画面印刷: トレーに複写できませんでした");
		if ( er >= E_OK ) tm_printf((UB *)"scrprint: tray %d\n", told(SN_TRAY));
		break;
	default: {
		T_PRNJOB	job;
		DTSCRPIC	pic;
		DTWIN		*host = dt_first_win();

		pic.px = px;
		pic.w = w;
		pic.h = h;
		dt_scr_job(&job, &pic, (CONST UB *)"画面印刷");
		er = ( host != NULL ) ? dt_print_out(host, &job, &sc_prn) : E_NOEXS;
		if ( er >= E_OK ) tm_printf((UB *)"scrprint: printed %d\n", told(SN_PRINTED));
		break;
	}
	}
	Kfree(px);
	wm_update();
}

/* The count or the key, once what is taken is known */
LOCAL void arm( void )
{
	UD	now = 0;
	UB	t[16];

	if ( sc.by == SC_BY_TIME ) {
		(void)ts_get_mono(&now);
		sc_due = now + (UD)sc.secs * 1000000000ULL;
		sc_beeped = SC_BEEPS + 1;
		sc_state = SC_TIMING;
		(void)put_num(t, 0, sizeof(t), sc.secs);
		say("画面印刷: ", t, "秒後に取り込みます");
	} else {
		sc_state = SC_KEYING;
		(void)wm_msg_put(( sc.by == SC_BY_KEYS ) ? "画面印刷: PrintScreenキーを押すたびに取り込みます"
							 : "画面印刷: PrintScreenキーで取り込みます");
	}
	tm_printf((UB *)"scrprint: armed %d: %d %d %d %d %d\n", told(SN_ARMED), sc.to, sc.pointer ? 1 : 0, sc.area,
		  sc.by, sc.secs);
	wm_update();
}

/* ---------------------------------------------------------------- the loupe */

#define SC_OUTSIDE	0x00808080U		/* a pixel off the screen, in the loupe */
#define SC_BLACK	0x00000000U

LOCAL UW	*sc_loupe = NULL;		/* the loupe's pixels, DT_SCR_LOUPE_SIDE squared */
LOCAL BOOL	sc_loupe_on = FALSE;

LOCAL void loupe_box( UW *out, INT l, INT t, INT r, INT b, UW c )
{
	INT	i;

	for ( i = l; i < r; i++ ) {
		out[t * DT_SCR_LOUPE_SIDE + i] = c;
		out[( b - 1 ) * DT_SCR_LOUPE_SIDE + i] = c;
	}
	for ( i = t; i < b; i++ ) {
		out[i * DT_SCR_LOUPE_SIDE + l] = c;
		out[i * DT_SCR_LOUPE_SIDE + r - 1] = c;
	}
}

EXPORT ER dt_scr_loupe( INT x, INT y, UW *out, T_DPRECT *p_at )
{
	UW		src[DT_SCR_LOUPE_SRC * DT_SCR_LOUPE_SRC];
	T_DPRECT	r;
	INT		sw = 0, sh = 0, half = DT_SCR_LOUPE_SRC / 2, side = DT_SCR_LOUPE_SIDE;
	INT		i, j, k, c0;
	ER		er = E_OK;

	wm_screen(&sw, &sh);
	if ( sw <= 0 || sh <= 0 || out == NULL ) {
		return E_NOEXS;
	}
	for ( i = 0; i < DT_SCR_LOUPE_SRC * DT_SCR_LOUPE_SRC; i++ ) {
		src[i] = SC_OUTSIDE;
	}
	r.left = ( x - half < 0 ) ? 0 : x - half;
	r.top = ( y - half < 0 ) ? 0 : y - half;
	r.right = ( x + half + 1 > sw ) ? sw : x + half + 1;
	r.bottom = ( y + half + 1 > sh ) ? sh : y + half + 1;
	if ( r.right > r.left && r.bottom > r.top ) {
		er = wm_grab_screen(src + ( r.top - ( y - half ) ) * DT_SCR_LOUPE_SRC + ( r.left - ( x - half ) ),
				    DT_SCR_LOUPE_SRC, &r, FALSE);
	}

	/* each pixel a square, in a frame black outside and white inside */
	for ( j = 0; j < DT_SCR_LOUPE_SRC; j++ ) {
		for ( i = 0; i < DT_SCR_LOUPE_SRC; i++ ) {
			UW	c = src[j * DT_SCR_LOUPE_SRC + i] & 0x00FFFFFFU;
			INT	oy = 2 + j * DT_SCR_LOUPE_ZOOM, ox = 2 + i * DT_SCR_LOUPE_ZOOM, a;

			for ( a = 0; a < DT_SCR_LOUPE_ZOOM; a++ ) {
				UW	*p = out + ( oy + a ) * side + ox;

				for ( k = 0; k < DT_SCR_LOUPE_ZOOM; k++ ) {
					p[k] = c;
				}
			}
		}
	}
	loupe_box(out, 0, 0, side, side, SC_BLACK);
	loupe_box(out, 1, 1, side - 1, side - 1, SC_WHITE);

	/* the pixel under the pointer: a black square round a white one */
	c0 = 2 + half * DT_SCR_LOUPE_ZOOM;
	loupe_box(out, c0 - 2, c0 - 2, c0 + DT_SCR_LOUPE_ZOOM + 2, c0 + DT_SCR_LOUPE_ZOOM + 2, SC_BLACK);
	loupe_box(out, c0 - 1, c0 - 1, c0 + DT_SCR_LOUPE_ZOOM + 1, c0 + DT_SCR_LOUPE_ZOOM + 1, SC_WHITE);

	/* below and right of the pointer, or on the other side where there is no room */
	p_at->left = x + DT_SCR_LOUPE_GAP;
	if ( p_at->left + side > sw ) p_at->left = x - DT_SCR_LOUPE_GAP - side;
	if ( p_at->left < 0 ) p_at->left = 0;
	p_at->top = y + DT_SCR_LOUPE_GAP;
	if ( p_at->top + side > sh ) p_at->top = y - DT_SCR_LOUPE_GAP - side;
	if ( p_at->top < 0 ) p_at->top = 0;
	p_at->right = p_at->left + side;
	p_at->bottom = p_at->top + side;

	return er;
}

/* The loupe shown beside the pointer at (x, y) */
LOCAL void loupe_show( INT x, INT y )
{
	T_DPRECT	at;

	if ( sc_loupe == NULL ) {
		sc_loupe = (UW *)Kmalloc((SZ)DT_SCR_LOUPE_SIDE * DT_SCR_LOUPE_SIDE * sizeof(UW));
		if ( sc_loupe == NULL ) {
			return;
		}
	}
	/* the pointer laid where it is now, and what it covers kept, before the pixels are read */
	(void)wm_pointer_moved(x, y);
	(void)wm_flush();
	if ( dt_scr_loupe(x, y, sc_loupe, &at) < E_OK ) {
		return;
	}
	if ( wm_set_loupe(sc_loupe, DT_SCR_LOUPE_SIDE, DT_SCR_LOUPE_SIDE, at.left, at.top) >= E_OK && !sc_loupe_on ) {
		sc_loupe_on = TRUE;
		tm_printf((UB *)"scrprint: loupe on\n");
	}
}

LOCAL void loupe_hide( void )
{
	(void)wm_set_loupe(NULL, 0, 0, 0, 0);
	if ( sc_loupe_on ) {
		sc_loupe_on = FALSE;
		tm_printf((UB *)"scrprint: loupe off\n");
	}
	if ( sc_loupe != NULL ) {
		Kfree(sc_loupe);
		sc_loupe = NULL;
	}
}

LOCAL void pick_end( void )
{
	sc_drag = FALSE;
	loupe_hide();
	(void)wm_set_drag(NULL, 0);
	(void)wm_set_pointer(WM_PT_SELECT);
	wm_obj_quiet(FALSE);
}

LOCAL void stop( void )
{
	if ( sc_state == SC_PICK ) {
		pick_end();
	}
	sc_state = SC_IDLE;
}


/* ---------------------------------------------------------------- what 画面印刷 asks */

/*
 * The process (application/scrprint) whose panel is up: one asked for
 * again meanwhile brings it to the front instead of a second panel
 */
LOCAL ID	sc_panel_pid = 0;
LOCAL TS_UUID	sc_panel_uuid;
LOCAL BOOL	sc_was = FALSE;		/* something was under way when the panel came up */
LOCAL UINT	sc_gen = 0;		/* what 実行 started: an answer of 基本印刷 for an older one is not used */

LOCAL BOOL panel_up( void )
{
	TS_UUID	u;

	return (BOOL)( sc_panel_pid > 0 && knl_prc_uuid(sc_panel_pid, &u, NULL) >= E_OK
		       && ts_uuid_cmp(&u, &sc_panel_uuid) == 0 );
}

/* The rectangle dragged out, or else the count or the key */
LOCAL void go( void )
{
	INT	x = 0, y = 0;
	UINT	btn = 0;

	if ( sc.area != DT_SCR_RANGE ) {
		arm();
		return;
	}
	sc_state = SC_PICK;
	sc_drag = FALSE;
	wm_obj_quiet(TRUE);
	(void)wm_set_pointer(WM_PT_PICK);
	(void)wm_msg_put("画面印刷: 取り込む範囲をドラッグしてください (Escで取り消し)");
	tm_printf((UB *)"scrprint: pick %d\n", told(SN_PICK));
	/* the loupe at once where the pointer is, before it moves */
	if ( ts_hid_pointer(&x, &y, &btn) >= E_OK ) loupe_show(x, y);
	wm_update();
}

/* The page the screen will be printed on, for its 印刷設定: the screen's size, before anything is taken */
typedef struct {
	T_PRNJOB	job;
	DTSCRPIC	pic;
	UINT		gen;
} SCASK;

LOCAL void print_set( void *arg, ER er, CONST T_PRNSET *set )
{
	SCASK	*a = (SCASK *)arg;
	UINT	gen = a->gen;

	Kfree(a);
	if ( gen != sc_gen ) {
		return;				/* the panel came up again meanwhile */
	}
	if ( er < E_OK || set == NULL ) {
		tm_printf((UB *)"scrprint: cancelled %d\n", told(SN_CANCELLED));
		return;
	}
	sc_prn = *set;
	go();
}

/* "to pointer area by secs<tab>volume<tab>name": what 実行 set */
LOCAL BOOL set_from( CONST UB *t )
{
	INT	n[5], k = 0, i = 0, j;

	while ( k < 5 ) {
		INT	x = 0;
		BOOL	any = FALSE;

		while ( t[i] == ' ' ) i++;
		while ( t[i] >= '0' && t[i] <= '9' && x < 1000 ) {
			x = x * 10 + ( t[i++] - '0' );
			any = TRUE;
		}
		if ( !any ) return FALSE;
		n[k++] = x;
	}
	sc.to = ( n[0] <= SC_TO_PRINTER ) ? n[0] : SC_TO_FIG;
	sc.pointer = (BOOL)( n[1] != 0 );
	sc.area = ( n[2] <= DT_SCR_RANGE ) ? n[2] : DT_SCR_SCREEN;
	sc.by = ( n[3] <= SC_BY_KEYS ) ? n[3] : SC_BY_TIME;
	sc.secs = ( n[4] < SC_SEC_MIN ) ? SC_SEC_MIN : ( n[4] > SC_SEC_MAX ) ? SC_SEC_MAX : n[4];
	if ( t[i] != '\t' ) return TRUE;
	i++;
	for ( j = 0; t[i] != 0 && t[i] != '\t' && j < SC_VOL_PATH - 1; ) sc_vol[j++] = t[i++];
	sc_vol[j] = 0;
	while ( t[i] != 0 && t[i] != '\t' ) i++;
	if ( t[i] != '\t' ) return TRUE;
	i++;
	while ( t[i] == ' ' ) i++;
	if ( t[i] != 0 ) {
		for ( j = 0; t[i] != 0 && j < TAD_NAME_MAX - 1; ) sc_name[j++] = t[i++];
		while ( j > 0 && ( sc_name[j] & 0xC0 ) == 0x80 ) j--;	/* cut at a letter's edge */
		sc_name[j] = 0;
	}
	return TRUE;
}

/*
 * DT_RQ_SCRPRINT: 画面印刷's process asks for what it set and the
 * volumes to save to, and hands over what 実行 set; the desktop takes
 * the screen as that says
 */
EXPORT ER dt_scr_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		t[DT_TEXT_MAX];
	UB		vols[SC_VOLS][SC_VOL_PATH];
	INT		at, n, i;

	if ( !sc_loaded ) {
		settings_load();
	}
	knl_memcpy(t, rq->text, sizeof(t));
	t[sizeof(t) - 1] = 0;
	switch ( rq->recno ) {
	case DT_SP_GET:
		/* the panel coming up: what was under way stops, and 実行 starts it again */
		if ( panel_up() && sc_panel_pid != pid ) {
			dt_raise_pid(sc_panel_pid);
			return E_BUSY;
		}
		if ( knl_prc_uuid(pid, &sc_panel_uuid, NULL) < E_OK ) {
			return E_NOEXS;
		}
		sc_panel_pid = pid;
		sc_was = (BOOL)( sc_state != SC_IDLE );
		stop();
		sc_gen++;
		at = put_num(an->text, 0, DT_TEXT_MAX, sc.to);
		at = put(an->text, at, DT_TEXT_MAX, " ");
		at = put_num(an->text, at, DT_TEXT_MAX, sc.pointer ? 1 : 0);
		at = put(an->text, at, DT_TEXT_MAX, " ");
		at = put_num(an->text, at, DT_TEXT_MAX, sc.area);
		at = put(an->text, at, DT_TEXT_MAX, " ");
		at = put_num(an->text, at, DT_TEXT_MAX, sc.by);
		at = put(an->text, at, DT_TEXT_MAX, " ");
		at = put_num(an->text, at, DT_TEXT_MAX, sc.secs);
		at = put(an->text, at, DT_TEXT_MAX, "\t");
		at = put(an->text, at, DT_TEXT_MAX, (CONST char *)sc_vol);
		at = put(an->text, at, DT_TEXT_MAX, "\t");
		(void)put(an->text, at, DT_TEXT_MAX, (CONST char *)sc_name);
		an->how = (UINT)told(SN_PANEL);
		return E_OK;
	case DT_SP_VOL:
		/* 保存先ディスク: the volumes attached, the first the system's */
		n = knl_obfile_vols(&vols[0][0], SC_VOL_PATH, SC_VOLS);
		an->how = (UINT)( ( n > 0 ) ? n : 0 );
		i = rq->grab_x;
		if ( i < 0 || i >= n ) {
			return E_NOEXS;
		}
		at = put(an->text, 0, DT_TEXT_MAX, (CONST char *)vols[i]);
		at = put(an->text, at, DT_TEXT_MAX, "\t");
		if ( i > 0 ) {
			T_OBFVREF	ref;

			/* a disk attached with ディスク操作: its name */
			if ( knl_obfile_vol_ref((CONST char *)vols[i], &ref) >= E_OK && ref.label[0] != 0 ) {
				(void)put(an->text, at, DT_TEXT_MAX, (CONST char *)ref.label);
			}
		}
		return E_OK;
	case DT_SP_ARM:
		if ( pid != sc_panel_pid || !set_from(t) ) {
			return ( pid != sc_panel_pid ) ? E_NOEXS : E_PAR;
		}
		sc_panel_pid = 0;
		settings_save();
		sc_taken = 0;
		stop();
		if ( sc.to == SC_TO_PRINTER ) {
			SCASK	*a = (SCASK *)Kmalloc(sizeof(SCASK));
			INT	sw = 0, sh = 0;
			ER	er;

			if ( a == NULL ) {
				return E_NOMEM;
			}
			/* the pages are not there yet: the setting for a page the screen's size */
			wm_screen(&sw, &sh);
			a->pic.px = NULL;
			a->pic.w = ( sw > 0 ) ? sw : 1;
			a->pic.h = ( sh > 0 ) ? sh : 1;
			a->gen = sc_gen;
			dt_scr_job(&a->job, &a->pic, (CONST UB *)"画面印刷");
			er = dt_print_start(&a->job, NULL, DT_PRINT_SET, print_set, a);
			if ( er < E_OK ) {
				Kfree(a);
			}
			return er;
		}
		go();
		return E_OK;
	case DT_SP_STOP:
		/* 取り消し: what was set kept all the same, when it is said */
		if ( pid == sc_panel_pid ) {
			sc_panel_pid = 0;
		}
		if ( t[0] != 0 && set_from(t) ) {
			settings_save();
		}
		stop();
		tm_printf((UB *)"scrprint: cancelled %d\n", told(SN_CANCELLED));
		if ( sc_was ) (void)wm_msg_put("画面印刷を取り消しました");
		sc_was = FALSE;
		wm_update();
		return E_OK;
	default:
		return E_PAR;
	}
}

/* ---------------------------------------------------------------- the calls */

EXPORT void dt_scr_poll( void )
{
	UD	now = 0;
	INT	left;

	if ( sc_state != SC_TIMING ) {
		return;
	}
	(void)ts_get_mono(&now);
	if ( now >= sc_due ) {
		sc_state = SC_IDLE;
		(void)wm_beep(WM_BEEP_LONG);
		take();
		return;
	}
	left = (INT)( ( sc_due - now + 999999999ULL ) / 1000000000ULL );
	if ( left <= SC_BEEPS && left < sc_beeped ) {
		sc_beeped = left;
		(void)wm_beep(WM_BEEP_TICK);
	}
}

/* The rectangle from where the drag began to (x, y), its corners in either order */
LOCAL void drag_rect( INT x, INT y, T_DPRECT *r )
{
	r->left = ( x < sc_x0 ) ? x : sc_x0;
	r->right = ( x < sc_x0 ) ? sc_x0 : x;
	r->top = ( y < sc_y0 ) ? y : sc_y0;
	r->bottom = ( y < sc_y0 ) ? sc_y0 : y;
	r->right++;
	r->bottom++;
}

EXPORT BOOL dt_scr_event( CONST T_WMEV *ev )
{
	T_DPRECT	r;
	INT		x = 0, y = 0;
	UINT		btn = 0;

	if ( sc_state == SC_KEYING ) {
		if ( ev->type == HID_EV_KEY_DOWN && ev->code == KEY_PRTSC ) {
			if ( sc.by != SC_BY_KEYS ) {
				sc_state = SC_IDLE;
			}
			take();
			return TRUE;
		}
		return (BOOL)( ev->type == HID_EV_KEY_UP && ev->code == KEY_PRTSC );
	}
	if ( sc_state != SC_PICK ) {
		return FALSE;
	}

	/* dragging out the rectangle: every input is the drag's */
	if ( ev->type == HID_EV_KEY_DOWN && ev->code == KEY_ESC ) {
		stop();
		(void)wm_msg_put("画面印刷を取り消しました");
		tm_printf((UB *)"scrprint: cancelled %d\n", told(SN_CANCELLED));
		wm_update();
		return TRUE;
	}
	if ( ev->type != HID_EV_BTN_DOWN && ev->type != HID_EV_BTN_UP && ev->type != HID_EV_MOVE ) {
		return TRUE;
	}
	/* where the event happened, on the screen: the pointer may have gone on since */
	x = ev->x;
	y = ev->y;
	if ( ev->wid > 0 ) {
		T_WMWIN	w;

		if ( wm_ref(ev->wid, &w) >= E_OK ) {
			x += w.work.left;
			y += w.work.top;
		} else if ( ts_hid_pointer(&x, &y, &btn) < E_OK ) {
			return TRUE;
		}
	}
	if ( ev->type == HID_EV_BTN_DOWN && ev->code == 0 ) {
		sc_drag = TRUE;
		sc_x0 = x;
		sc_y0 = y;
		drag_rect(x, y, &r);
		(void)wm_set_drag(&r, 1);
		loupe_show(x, y);
		return TRUE;
	}
	if ( !sc_drag ) {
		if ( ev->type == HID_EV_MOVE ) loupe_show(x, y);
		return TRUE;
	}
	drag_rect(x, y, &r);
	if ( ev->type == HID_EV_MOVE ) {
		(void)wm_set_drag(&r, 1);
		loupe_show(x, y);
		return TRUE;
	}
	if ( ev->type == HID_EV_BTN_UP && ev->code == 0 ) {
		if ( r.right - r.left < SC_DRAG_LEAST || r.bottom - r.top < SC_DRAG_LEAST ) {
			sc_drag = FALSE;		/* too small to mean anything: again */
			(void)wm_set_drag(NULL, 0);
			return TRUE;
		}
		sc_range = r;
		pick_end();
		tm_printf((UB *)"scrprint: range %d %d %d %d %d\n", told(SN_RANGE), r.left, r.top, r.right, r.bottom);
		arm();
	}
	return TRUE;
}
