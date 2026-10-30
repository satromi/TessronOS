/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_scr.c
 *	画面印刷 (docs/tessronos/11-desktop.md 11.24)
 *
 *	What is taken is held against the screen pixel by pixel: the whole
 *	screen with the pointer and without it (the latter against a screen
 *	built with the pointer hidden), the window in front with the window
 *	subordinate to it (inside them the screen, outside them white, the
 *	rectangle the two make), a panel in its own window alone, and a
 *	rectangle given. Then where it goes: the figure object made of it
 *	(its record, its layout paper, and the picture read back), the
 *	tray's set (an image naming the set's PNG, and the PNG) and the
 *	picture written beside a figure it is pasted into, the figure saved
 *	on another volume attached, the figure given out by ファイル変換 as a
 *	PNG file, and a file of PDF printed from it, left on /boot as
 *	SCRPRN.PDF.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/img.h>
#include <ts/print.h>
#include <ts/tray.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/fs.h>
#include <ts/xf.h>
#include <ts/uuid.h>
#include <ts/xfu.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/docmenu.h>
#include "../../application/desktop/desktop.h"

#define RGB		0x00FFFFFFU

LOCAL INT	sw = 0, sh = 0;

LOCAL INT s_len( CONST char *t )
{
	INT	n = 0;

	while ( t[n] != 0 ) n++;
	return n;
}

/* Where a word is in the bytes, or -1 */
LOCAL INT find( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, wl = s_len(w);

	for ( i = 0; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return i;
	}
	return -1;
}

/* Words added at 'at' of b, the end after them; where they end */
LOCAL INT cat( UB *b, INT at, CONST char *w )
{
	while ( *w != 0 ) b[at++] = (UB)*w++;
	b[at] = 0;
	return at;
}

LOCAL void rect( T_DPRECT *r, INT l, INT t, INT rr, INT b )
{
	r->left = l;  r->top = t;  r->right = rr;  r->bottom = b;
}

LOCAL BOOL in( CONST T_DPRECT *r, INT x, INT y )
{
	return (BOOL)( x >= r->left && x < r->right && y >= r->top && y < r->bottom );
}

LOCAL void hid( UINT type, INT x, INT y )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.x = x;
	e.y = y;
	(void)kt_inject(&e);
	tk_dly_tsk(60);
}

/* A window whose work area is painted in bands of a colour of its own */
LOCAL INT painted( INT l, INT t, INT r, INT b, UW col, CONST char *name )
{
	T_DPRECT	o, a;
	T_WMWIN		w;
	INT		wid, gid, y;

	rect(&o, l, t, r, b);
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, name);
	if ( wid <= 0 || wm_ref(wid, &w) < E_OK ) {
		return wid;
	}
	gid = wm_gid(wid);
	for ( y = 0; y < w.work.bottom - w.work.top; y += 8 ) {
		rect(&a, 0, y, w.work.right - w.work.left, y + 8);
		dp_fill_rect(gid, &a, ( ( y / 8 ) & 1 ) ? col : ( col ^ 0x00404040U ));
	}
	return wid;
}

/* The screen as last built, the caller's to Kfree */
LOCAL UW *screen( void )
{
	UW	*s = (UW *)Kmalloc(sizeof(UW) * (SZ)sw * sh);
	INT	i;

	if ( s != NULL ) {
		if ( wm_copy_screen(s, sw) < E_OK ) {
			Kfree(s);
			return NULL;
		}
		for ( i = 0; i < sw * sh; i++ ) s[i] &= RGB;
	}
	return s;
}

/* Pixels of px (taken at 'at') that differ from the screen s */
LOCAL INT differ( CONST UW *px, CONST T_DPRECT *at, CONST UW *s )
{
	INT	x, y, n = 0, w = at->right - at->left;

	for ( y = at->top; y < at->bottom; y++ ) {
		for ( x = at->left; x < at->right; x++ ) {
			if ( px[( y - at->top ) * w + ( x - at->left )] != s[y * sw + x] ) n++;
		}
	}
	return n;
}

LOCAL BOOL start( void )
{
	wm_screen(&sw, &sh);
	return (BOOL)( sw > 0 && sh > 0 );
}

/* ---------------------------------------------------------------- the whole screen */

LOCAL void test_screen( void )
{
	UW		*with = NULL, *bare = NULL, *px = NULL, *np = NULL;
	T_DPRECT	at, pb;
	INT		a, b, x, y, n = 0, inbox = 0;

	if ( !start() ) KT_SKIP("no screen");
	a = painted(100, 100, 500, 400, 0x00C03030U, "画面印刷A");
	b = painted(300, 250, 700, 550, 0x003030C0U, "画面印刷B");
	KT_ASSERT(a > 0 && b > 0);
	wm_show_pointer(TRUE);
	hid(HID_EV_MOVE, 450, 300);
	KT_ASSERT_ER(wm_composite(), E_OK);
	with = screen();
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_SCREEN, TRUE, NULL, &px, &at), E_OK);
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_SCREEN, FALSE, NULL, &np, &at), E_OK);
	wm_show_pointer(FALSE);
	KT_ASSERT_ER(wm_composite(), E_OK);
	bare = screen();
	if ( with != NULL && bare != NULL && px != NULL && np != NULL ) {
		KT_ASSERT(at.left == 0 && at.top == 0 && at.right == sw && at.bottom == sh);
		n = differ(px, &at, with);
		tm_printf((UB *)"  with the pointer: %d of %d pixels differ\n", n, sw * sh);
		KT_ASSERT_EQ(n, 0);
		n = differ(np, &at, bare);
		tm_printf((UB *)"  without it, against the screen built without it: %d differ\n", n);
		KT_ASSERT_EQ(n, 0);
		/* the pointer was there: the two screens differ, and only in its square */
		wm_pointer_box(450, 300, &pb);
		n = 0;
		for ( y = 0; y < sh; y++ ) {
			for ( x = 0; x < sw; x++ ) {
				if ( with[y * sw + x] != bare[y * sw + x] ) {
					n++;
					if ( in(&pb, x, y) ) inbox++;
				}
			}
		}
		tm_printf((UB *)"  the pointer's pixels: %d, %d of them in its square\n", n, inbox);
		KT_ASSERT(n > 20);
		KT_ASSERT_EQ(inbox, n);
	}
	if ( with != NULL ) Kfree(with);
	if ( bare != NULL ) Kfree(bare);
	if ( px != NULL ) Kfree(px);
	if ( np != NULL ) Kfree(np);
	(void)wm_close(b);
	(void)wm_close(a);
	(void)wm_composite();
}

/* ---------------------------------------------------------------- the window in front */

LOCAL void test_front( void )
{
	UW		*s = NULL, *px = NULL;
	T_DPRECT	at, ob, oc, want;
	T_WMWIN		w;
	INT		a, b, c, d, wids[8], n, x, y, bad = 0, white = 0, inside = 0;

	if ( !start() ) KT_SKIP("no screen");
	a = painted(80, 80, 520, 420, 0x00C03030U, "画面印刷A");
	b = painted(200, 200, 560, 480, 0x003030C0U, "画面印刷B");
	c = painted(540, 300, 760, 420, 0x0030A030U, "画面印刷C");
	d = painted(600, 100, 900, 260, 0x00A0A030U, "画面印刷D");
	KT_ASSERT(a > 0 && b > 0 && c > 0 && d > 0);
	KT_ASSERT_ER(wm_set_parent(c, b), E_OK);
	KT_ASSERT_ER(wm_raise(b), E_OK);
	KT_ASSERT_ER(wm_focus(b), E_OK);	/* its group in use: c is shown */
	KT_ASSERT_ER(wm_composite(), E_OK);

	n = wm_front_group(wids, 8);
	KT_ASSERT_EQ(n, 2);
	KT_ASSERT_EQ(wids[0], b);
	KT_ASSERT(n >= 2 && wids[1] == c);

	s = screen();
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_FRONT, FALSE, NULL, &px, &at), E_OK);
	KT_ASSERT_ER(wm_ref(b, &w), E_OK);
	ob = w.outer;
	KT_ASSERT_ER(wm_ref(c, &w), E_OK);
	oc = w.outer;
	want.left = ( ob.left < oc.left ) ? ob.left : oc.left;
	want.top = ( ob.top < oc.top ) ? ob.top : oc.top;
	want.right = ( ob.right > oc.right ) ? ob.right : oc.right;
	want.bottom = ( ob.bottom > oc.bottom ) ? ob.bottom : oc.bottom;
	tm_printf((UB *)"  taken %d,%d-%d,%d; the two windows make %d,%d-%d,%d\n", at.left, at.top,
		  at.right, at.bottom, want.left, want.top, want.right, want.bottom);
	KT_ASSERT(at.left == want.left && at.top == want.top && at.right == want.right
		  && at.bottom == want.bottom);
	if ( s != NULL && px != NULL ) {
		INT	pw = at.right - at.left;

		for ( y = at.top; y < at.bottom; y++ ) {
			for ( x = at.left; x < at.right; x++ ) {
				UW	got = px[( y - at.top ) * pw + ( x - at.left )];

				if ( in(&ob, x, y) || in(&oc, x, y) ) {
					inside++;
					if ( got != s[y * sw + x] ) bad++;
				} else {
					if ( s[y * sw + x] != RGB ) white++;	/* not white on the screen */
					if ( got != RGB ) bad++;
				}
			}
		}
		tm_printf((UB *)"  %d pixels of the windows, %d made white, %d wrong\n", inside, white, bad);
		KT_ASSERT_EQ(bad, 0);
		KT_ASSERT(white > 0);
		KT_ASSERT(inside > 0);
	}
	if ( px != NULL ) {
		Kfree(px);
		px = NULL;
	}

	/* a panel in a window of its own in front: it alone */
	{
		T_WMPANEL	*def = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));
		INT		pid = -1, pop;

		if ( def != NULL ) {
			knl_memset(def, 0, sizeof(*def));
			def->num = 2;
			def->kind = WM_PNL_PLAIN;
			def->r.right = 300;
			def->r.bottom = 120;
			pid = wm_panel_open_centre(b, def);
			Kfree(def);
		}
		KT_ASSERT(pid > 0);
		if ( pid > 0 ) {
			(void)wm_panel_draw(pid);
			KT_ASSERT_ER(wm_composite(), E_OK);
			pop = wm_panel_front();
			KT_ASSERT(pop > 0);
			KT_ASSERT_ER(dt_scr_grab(DT_SCR_FRONT, FALSE, NULL, &px, &at), E_OK);
			if ( pop > 0 && wm_ref(pop, &w) >= E_OK ) {
				KT_ASSERT(at.left == w.outer.left && at.top == w.outer.top
					  && at.right == w.outer.right && at.bottom == w.outer.bottom);
			}
			if ( s != NULL ) Kfree(s);
			s = screen();
			if ( s != NULL && px != NULL ) KT_ASSERT_EQ(differ(px, &at, s), 0);
			(void)wm_panel_close(pid);
			KT_ASSERT_EQ(wm_panel_front(), 0);
		}
	}
	if ( px != NULL ) Kfree(px);
	if ( s != NULL ) Kfree(s);
	(void)wm_close(d);
	(void)wm_close(c);
	(void)wm_close(b);
	(void)wm_close(a);
	(void)wm_composite();
}

/* ---------------------------------------------------------------- a rectangle */

LOCAL void test_range( void )
{
	UW		*s = NULL, *px = NULL;
	T_DPRECT	r, at;
	INT		a;

	if ( !start() ) KT_SKIP("no screen");
	a = painted(100, 100, 500, 400, 0x00C03030U, "画面印刷A");
	KT_ASSERT_ER(wm_composite(), E_OK);
	s = screen();
	rect(&r, 37, 41, 237, 191);
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_RANGE, FALSE, &r, &px, &at), E_OK);
	KT_ASSERT(at.left == 37 && at.top == 41 && at.right == 237 && at.bottom == 191);
	if ( s != NULL && px != NULL ) KT_ASSERT_EQ(differ(px, &at, s), 0);
	if ( px != NULL ) Kfree(px);
	px = NULL;

	/* cut to the screen */
	rect(&r, sw - 50, sh - 30, sw + 100, sh + 100);
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_RANGE, FALSE, &r, &px, &at), E_OK);
	KT_ASSERT(at.left == sw - 50 && at.top == sh - 30 && at.right == sw && at.bottom == sh);
	if ( s != NULL && px != NULL ) KT_ASSERT_EQ(differ(px, &at, s), 0);
	if ( px != NULL ) Kfree(px);
	px = NULL;

	/* off it altogether */
	rect(&r, sw + 10, 0, sw + 100, 50);
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_RANGE, FALSE, &r, &px, &at), E_PAR);
	KT_ASSERT(px == NULL);
	if ( s != NULL ) Kfree(s);
	(void)wm_close(a);
	(void)wm_composite();
}

/* ---------------------------------------------------------------- where it goes */

/* A rectangle of the screen with a window in it, for what follows */
LOCAL UW *sample( T_DPRECT *at )
{
	T_DPRECT	r;
	UW		*px = NULL;
	INT		a = painted(100, 100, 500, 400, 0x00C03030U, "画面印刷A");

	(void)wm_composite();
	rect(&r, 60, 60, 460, 360);
	if ( dt_scr_grab(DT_SCR_RANGE, FALSE, &r, &px, at) < E_OK ) px = NULL;
	(void)wm_close(a);
	(void)wm_composite();
	return px;
}

LOCAL void test_save( void )
{
	T_DPRECT	at;
	UW		*px;
	CONST UW	*back = NULL;
	TS_UUID		u;
	UB		name[TAD_NAME_MAX], *rec, href[64];
	char		us[TS_UUID_STRLEN + 1];
	SZ		size = 0;
	INT		w, h, bw = 0, bh = 0, i, bad = 0, k;
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;

	if ( !start() ) KT_SKIP("no screen");
	px = sample(&at);
	KT_ASSERT(px != NULL);
	if ( px == NULL ) return;
	w = at.right - at.left;
	h = at.bottom - at.top;
	KT_ASSERT_ER(dt_scr_save(px, w, h, (CONST UB *)"画面印刷の試験 <1>", NULL, &u), E_OK);
	KT_ASSERT(om_store_name(&u, name, sizeof(name)) > 0);
	KT_ASSERT(find(name, s_len((CONST char *)name), "画面印刷の試験 <1>") == 0);
	/* it opens in 基本図形編集 */
	KT_ASSERT(om_store_default_app(&u, name, sizeof(name)) > 0
		  && find(name, s_len((CONST char *)name), "basic-figure-editor") == 0);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL);
	if ( rec != NULL ) {
		(void)ts_uuid_to_str(&u, us, sizeof(us));
		k = 0;
		for ( i = 0; us[i] != 0; i++ ) href[k++] = (UB)us[i];
		for ( i = 0; "_0_1.png"[i] != 0; i++ ) href[k++] = (UB)"_0_1.png"[i];
		href[k] = 0;
		KT_ASSERT(find(rec, (INT)size, (CONST char *)href) > 0);
		KT_ASSERT(find(rec, (INT)size, "<image ") > 0);
		KT_ASSERT(find(rec, (INT)size, "filename=\"画面印刷の試験 &lt;1&gt;\"") > 0);
		KT_ASSERT(find(rec, (INT)size, "<paper type=\"fig\" length=\"225\" width=\"300\"") > 0);
		KT_ASSERT_ER(tad_parse(rec, size, NULL, &doc), E_OK);
		if ( doc != NULL ) {
			KT_ASSERT_EQ((INT)tv_kind(doc), TV_KIND_FIG);
			KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
			if ( f != NULL ) {
				KT_ASSERT_EQ(f->paper_w, 300);
				KT_ASSERT_EQ(f->paper_h, 225);
				KT_ASSERT_EQ(f->nsh, 1);
				KT_ASSERT(f->nsh >= 1 && f->sh[0].kind == TV_SH_IMAGE);
				KT_ASSERT(f->nsh >= 1 && f->sh[0].r.right == w && f->sh[0].r.bottom == h);
				tv_fig_free(f);
			}
			tad_free(doc);
		}
		KT_ASSERT_ER(om_store_picture(href, &back, &bw, &bh), E_OK);
		KT_ASSERT(bw == w && bh == h);
		if ( back != NULL && bw == w && bh == h ) {
			for ( i = 0; i < w * h; i++ ) {
				if ( ( back[i] & RGB ) != px[i] ) bad++;
			}
			tm_printf((UB *)"  the picture read back: %dx%d, %d pixels differ\n", bw, bh, bad);
			KT_ASSERT_EQ(bad, 0);
		}
		om_store_picture_done(back);
		Kfree(rec);
	}
	(void)ob_del_obj(&u);
	Kfree(px);
}

LOCAL void test_tray( void )
{
	T_DPRECT	at;
	UW		*px, *dec = NULL;
	CONST UW	*back = NULL;
	TS_UUID		fo;
	T_TRINF		inf;
	T_TAD		*rec = NULL, *frag = NULL;
	T_TADNODE	*nd;
	UB		*b, *r0, want[TS_UUID_STRLEN + 16];
	CONST UB	*href = NULL;
	SZ		asz = 0, n0 = 0;
	char		us[TS_UUID_STRLEN + 1];
	INT		w, h, dw = 0, dh = 0, bw = 0, bh = 0, i, k, bad = 0;

	if ( !start() ) KT_SKIP("no screen");
	px = sample(&at);
	KT_ASSERT(px != NULL);
	if ( px == NULL ) return;
	w = at.right - at.left;
	h = at.bottom - at.top;
	KT_ASSERT_ER(dt_scr_tray(px, w, h), E_OK);

	/* the set: an image naming the set's PNG, no link, and the PNG */
	KT_ASSERT_ER(tr_ref_rec(TR_HAND, 0, &inf), E_OK);
	KT_ASSERT_EQ(inf.kind, TR_TAD);
	b = (UB *)Kmalloc((SZ)inf.size + 1);
	if ( b != NULL && tr_rea_rec(TR_HAND, 0, b, inf.size, &asz) >= E_OK ) {
		KT_ASSERT(find(b, (INT)asz, "<image ") > 0);
		KT_ASSERT(find(b, (INT)asz, "href=\"tray:1\"") > 0);
		KT_ASSERT(find(b, (INT)asz, "right=\"400\" bottom=\"300\"") > 0);
		KT_ASSERT(find(b, (INT)asz, "<link") < 0);
	}
	if ( b != NULL ) Kfree(b);
	KT_ASSERT_ER(tr_ref_rec(TR_HAND, 1, &inf), E_OK);
	KT_ASSERT_EQ(inf.kind, TR_PNG);
	b = (UB *)Kmalloc((SZ)inf.size + 1);
	if ( b != NULL && tr_rea_rec(TR_HAND, 1, b, inf.size, &asz) >= E_OK ) {
		KT_ASSERT_ER(img_png_decode(b, asz, &dec, &dw, &dh), E_OK);
		KT_ASSERT(dw == w && dh == h);
		if ( dec != NULL && dw == w && dh == h ) {
			for ( i = 0; i < w * h; i++ ) {
				if ( ( dec[i] & RGB ) != px[i] ) bad++;
			}
			KT_ASSERT_EQ(bad, 0);
		}
		if ( dec != NULL ) Kfree(dec);
	}
	if ( b != NULL ) Kfree(b);

	/*
	 * Pasted into a figure: the PNG written beside the figure's record,
	 * under a number past the picture it already has, and the image
	 * named so
	 */
	KT_ASSERT_ER(om_cre_obj(NULL, (CONST UB *)"画面印刷の貼り込み先", OM_BODY_FIG, &fo), E_OK);
	(void)ts_uuid_to_str(&fo, us, sizeof(us));
	{
		UB	x[200];

		k = cat(x, 0, "<tad version=\"1.0\" encoding=\"UTF-8\"><figure><image left=\"0\" top=\"0\""
			       " right=\"8\" bottom=\"8\" href=\"");
		k = cat(x, k, us);
		k = cat(x, k, "_0_3.png\"/></figure></tad>");
		KT_ASSERT_ER(om_obj_record_put(&fo, 0, OB_RT_TAD, 0, x, (SZ)k), E_OK);
	}
	r0 = om_obj_record(&fo, 0, &n0);
	if ( r0 != NULL ) {
		(void)tad_parse(r0, n0, NULL, &rec);
		Kfree(r0);
	}
	frag = dt_tray_frag();
	KT_ASSERT(rec != NULL && frag != NULL);
	if ( rec != NULL && frag != NULL ) {
		KT_ASSERT_EQ(dt_tray_pictures(&fo, rec, frag), 1);
		for ( nd = tad_walk(frag, NULL); nd != NULL; nd = tad_walk(frag, nd) ) {
			if ( nd->kind == TAD_ND_ELEM && tad_attr(nd, "href") != NULL ) href = tad_attr(nd, "href");
		}
		k = 0;
		for ( i = 0; us[i] != 0; i++ ) want[k++] = (UB)us[i];
		for ( i = 0; "_0_4.png"[i] != 0; i++ ) want[k++] = (UB)"_0_4.png"[i];
		want[k] = 0;
		KT_ASSERT(href != NULL && find(href, s_len((CONST char *)href), (CONST char *)want) == 0
			  && s_len((CONST char *)href) == k);
		tm_printf((UB *)"  pasted: %s\n", ( href != NULL ) ? (CONST char *)href : "-");
		if ( href != NULL ) {
			KT_ASSERT_ER(om_store_picture(href, &back, &bw, &bh), E_OK);
			KT_ASSERT(bw == w && bh == h);
			bad = 0;
			if ( back != NULL && bw == w && bh == h ) {
				for ( i = 0; i < w * h; i++ ) {
					if ( ( back[i] & RGB ) != px[i] ) bad++;
				}
				KT_ASSERT_EQ(bad, 0);
			}
			om_store_picture_done(back);
		}
	}
	if ( frag != NULL ) tad_free(frag);
	if ( rec != NULL ) tad_free(rec);
	(void)tr_del_dat(TR_HAND);
	(void)ob_del_obj(&fo);
	Kfree(px);
}

/*
 * Pasted into a text: the PNG written beside the text's record, and a
 * figure standing in the text holding the image under that name
 */
LOCAL void test_tray_doc( void )
{
	static CONST char doc0[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>前の文字</p></document></tad>";
	T_DPRECT	at;
	UW		*px;
	CONST UW	*back = NULL;
	TS_UUID		du;
	DTWIN		*d = NULL;
	T_TAD		*doc = NULL;
	T_TADNODE	*nd, *img = NULL, *fig = NULL;
	CONST UB	*href = NULL;
	UB		want[TS_UUID_STRLEN + 16];
	char		us[TS_UUID_STRLEN + 1];
	INT		w, h, bw = 0, bh = 0, i, k, bad = 0;

	if ( !start() ) KT_SKIP("no screen");
	px = sample(&at);
	KT_ASSERT(px != NULL);
	if ( px == NULL ) return;
	w = at.right - at.left;
	h = at.bottom - at.top;
	KT_ASSERT_ER(dt_scr_tray(px, w, h), E_OK);
	KT_ASSERT_EQ(dt_tray_kind(NULL), DT_TRAY_FIG);

	KT_ASSERT_ER(om_cre_obj(NULL, (CONST UB *)"画面印刷の貼り込み先の文章", OM_BODY_DOC, &du), E_OK);
	(void)ts_uuid_to_str(&du, us, sizeof(us));
	KT_ASSERT_ER(tad_parse((CONST UB *)doc0, s_len(doc0), NULL, &doc), E_OK);
	d = (DTWIN *)Kmalloc(sizeof(DTWIN));
	KT_ASSERT(d != NULL && doc != NULL);
	if ( d == NULL || doc == NULL ) goto done;
	knl_memset(d, 0, sizeof(*d));
	d->used = TRUE;
	d->id = du;
	d->rec = doc;
	d->kind = TV_KIND_DOC;
	d->paper = TV_PAPER_LOOK;
	ed_model(d);
	KT_ASSERT(dd_command(d, DM_PASTE));

	for ( nd = tad_walk(doc, NULL); nd != NULL; nd = tad_walk(doc, nd) ) {
		if ( nd->kind != TAD_ND_ELEM || nd->name == NULL ) continue;
		if ( s_len((CONST char *)nd->name) == 6 && find(nd->name, 6, "figure") == 0 ) fig = nd;
		if ( s_len((CONST char *)nd->name) == 5 && find(nd->name, 5, "image") == 0 ) img = nd;
	}
	KT_ASSERT(fig != NULL && img != NULL);
	if ( img != NULL ) {
		T_TADNODE	*p;

		for ( p = img->parent; p != NULL && p != fig; p = p->parent ) ;
		KT_ASSERT(p == fig);			/* the image in the figure in the text */
		href = tad_attr(img, "href");
	}
	k = 0;
	for ( i = 0; us[i] != 0; i++ ) want[k++] = (UB)us[i];
	for ( i = 0; "_0_1.png"[i] != 0; i++ ) want[k++] = (UB)"_0_1.png"[i];
	want[k] = 0;
	KT_ASSERT(href != NULL && find(href, s_len((CONST char *)href), (CONST char *)want) == 0
		  && s_len((CONST char *)href) == k);
	tm_printf((UB *)"  pasted in a text: %s\n", ( href != NULL ) ? (CONST char *)href : "-");
	if ( href != NULL ) {
		KT_ASSERT_ER(om_store_picture(href, &back, &bw, &bh), E_OK);
		KT_ASSERT(bw == w && bh == h);
		if ( back != NULL && bw == w && bh == h ) {
			for ( i = 0; i < w * h; i++ ) {
				if ( ( back[i] & RGB ) != px[i] ) bad++;
			}
			KT_ASSERT_EQ(bad, 0);
		}
		om_store_picture_done(back);
	}
done:
	if ( d != NULL ) {
		if ( d->doc != NULL ) tv_doc_free(d->doc);
		Kfree(d);
	}
	if ( doc != NULL ) tad_free(doc);
	(void)tr_del_dat(TR_HAND);
	(void)ob_del_obj(&du);
	Kfree(px);
}

/*
 * The loupe while a rectangle is dragged out: each pixel round the
 * pointer, without the pointer, a square of its own; the frame and the
 * pixel under the pointer outlined; below and right of the pointer, on
 * the other side near the screen's edge, grey off the screen; laid over
 * every window and taken away again.
 */
LOCAL void test_loupe( void )
{
	UW		*out = NULL, *px = NULL, *s0 = NULL, *s1 = NULL;
	T_DPRECT	at, r, pat;
	INT		a, x = 160, y = 170, side = DT_SCR_LOUPE_SIDE, half = DT_SCR_LOUPE_SRC / 2;
	INT		i, j, bad = 0, c0 = 2 + half * DT_SCR_LOUPE_ZOOM;

	if ( !start() ) KT_SKIP("no screen");
	a = painted(100, 100, 500, 400, 0x00C03030U, "画面印刷A");
	KT_ASSERT_ER(wm_composite(), E_OK);
	out = (UW *)Kmalloc(sizeof(UW) * (SZ)side * side);
	KT_ASSERT(out != NULL);
	if ( out == NULL ) goto done;

	KT_ASSERT_ER(dt_scr_loupe(x, y, out, &at), E_OK);
	KT_ASSERT(at.left == x + DT_SCR_LOUPE_GAP && at.top == y + DT_SCR_LOUPE_GAP
		  && at.right == at.left + side && at.bottom == at.top + side);
	rect(&r, x - half, y - half, x + half + 1, y + half + 1);
	KT_ASSERT_ER(dt_scr_grab(DT_SCR_RANGE, FALSE, &r, &px, &pat), E_OK);
	if ( px != NULL ) {
		for ( j = 0; j < DT_SCR_LOUPE_SRC; j++ ) {
			for ( i = 0; i < DT_SCR_LOUPE_SRC; i++ ) {
				INT	o = ( 2 + j * DT_SCR_LOUPE_ZOOM + DT_SCR_LOUPE_ZOOM / 2 ) * side
					  + 2 + i * DT_SCR_LOUPE_ZOOM + DT_SCR_LOUPE_ZOOM / 2;

				if ( ( out[o] & RGB ) != px[j * DT_SCR_LOUPE_SRC + i] ) bad++;
			}
		}
		KT_ASSERT_EQ(bad, 0);
		Kfree(px);
		px = NULL;
	}
	KT_ASSERT_EQ(out[0] & RGB, 0);					/* black outside */
	KT_ASSERT_EQ(out[side + 1] & RGB, RGB);				/* white inside */
	KT_ASSERT_EQ(out[( c0 - 2 ) * side + c0 - 2] & RGB, 0);		/* round the pixel under the pointer */
	KT_ASSERT_EQ(out[( c0 - 1 ) * side + c0 + 2] & RGB, RGB);

	/* near the bottom right corner: above and left of it, grey off the screen */
	KT_ASSERT_ER(dt_scr_loupe(sw - 3, sh - 3, out, &at), E_OK);
	KT_ASSERT(at.right == sw - 3 - DT_SCR_LOUPE_GAP && at.bottom == sh - 3 - DT_SCR_LOUPE_GAP);
	i = ( 2 + ( DT_SCR_LOUPE_SRC - 1 ) * DT_SCR_LOUPE_ZOOM + 3 ) * side + 2 + ( DT_SCR_LOUPE_SRC - 1 ) * DT_SCR_LOUPE_ZOOM + 3;
	KT_ASSERT_EQ(out[i] & RGB, 0x00808080U);

	/* laid over every window, and taken away */
	KT_ASSERT_ER(dt_scr_loupe(x, y, out, &at), E_OK);
	s0 = screen();
	KT_ASSERT_ER(wm_set_loupe(out, side, side, at.left, at.top), E_OK);
	(void)wm_flush();
	s1 = screen();
	if ( s1 != NULL ) {
		bad = 0;
		for ( j = 0; j < side; j++ ) {
			for ( i = 0; i < side; i++ ) {
				if ( s1[( at.top + j ) * sw + at.left + i] != ( out[j * side + i] & RGB ) ) bad++;
			}
		}
		KT_ASSERT_EQ(bad, 0);
		Kfree(s1);
	}
	KT_ASSERT_ER(wm_set_loupe(NULL, 0, 0, 0, 0), E_OK);
	(void)wm_flush();
	s1 = screen();
	if ( s0 != NULL && s1 != NULL ) {
		bad = 0;
		for ( j = at.top; j < at.bottom; j++ ) {
			for ( i = at.left; i < at.right; i++ ) {
				if ( s0[j * sw + i] != s1[j * sw + i] ) bad++;
			}
		}
		KT_ASSERT_EQ(bad, 0);
	}
	KT_ASSERT_ER(wm_set_loupe(out, WM_LOUPE_MAX + 1, 1, 0, 0), E_PAR);
done:
	if ( s0 != NULL ) Kfree(s0);
	if ( s1 != NULL ) Kfree(s1);
	if ( out != NULL ) Kfree(out);
	(void)wm_close(a);
	(void)wm_composite();
}

/* 保存先ディスク: a volume attached beside the system's, offered and saved to */
LOCAL void test_volume( void )
{
	T_DPRECT	at;
	UW		*px;
	UB		vols[4][64];
	TS_UUID		u, u0;
	INT		n, i, found = -1;
	CONST char	*dev = kt_scratch();

	if ( !start() ) KT_SKIP("no screen");
	if ( dev == NULL ) KT_SKIP(KT_NO_SCRATCH);
	(void)ob_det_vol(dev);
	if ( ts_format_blk(dev, "SCRVOL") < E_OK || ob_att_vol(dev, TSFS_STORE_BLK) < E_OK ) {
		KT_SKIP("no block device for a store");
	}
	n = knl_obfile_vols(&vols[0][0], 64, 4);
	for ( i = 0; i < n; i++ ) {
		tm_printf((UB *)"  volume %d: %s\n", i, vols[i]);
		if ( find(vols[i], s_len((CONST char *)vols[i]), dev) == 0 && s_len((CONST char *)vols[i]) == s_len(dev) ) found = i;
	}
	KT_ASSERT(n >= 2);
	KT_ASSERT(found >= 1);
	px = sample(&at);
	KT_ASSERT(px != NULL);
	if ( px != NULL ) {
		KT_ASSERT_ER(dt_scr_save(px, at.right - at.left, at.bottom - at.top, (CONST UB *)"別のディスクへ", dev, &u), E_OK);
		KT_ASSERT(knl_obfile_on(&u, dev));
		KT_ASSERT_ER(dt_scr_save(px, at.right - at.left, at.bottom - at.top, (CONST UB *)"システムへ", NULL, &u0), E_OK);
		KT_ASSERT(!knl_obfile_on(&u0, dev));
		KT_ASSERT(knl_obfile_on(&u0, (CONST char *)vols[0]));
		KT_ASSERT(dt_scr_save(px, 8, 8, (CONST UB *)"無いディスク", "nodisk", &u0) < E_OK);
		(void)ob_del_obj(&u);
		(void)ob_del_obj(&u0);
		Kfree(px);
	}
	(void)ob_det_vol(dev);
}

/* 画像: the figure given out by ファイル変換 as the PNG of its first picture */
LOCAL void test_png( void )
{
	T_DPRECT	at;
	UW		*px, *dec = NULL;
	TS_UUID		u, e;
	T_XFUFS		*fs;
	T_XFUTREE	t;
	T_XFUOPT	opt;
	UB		name[XFU_NAME_MAX], *b = NULL;
	char		path[FS_PATH_MAX];
	INT		w, h, dw = 0, dh = 0, i, bad = 0, fd, n = 0, k;

	if ( !start() ) KT_SKIP("no screen");
	fs = (T_XFUFS *)Kmalloc(sizeof(T_XFUFS));
	px = sample(&at);
	KT_ASSERT(fs != NULL && px != NULL);
	if ( fs == NULL || px == NULL ) {
		if ( fs != NULL ) Kfree(fs);
		if ( px != NULL ) Kfree(px);
		return;
	}
	w = at.right - at.left;
	h = at.bottom - at.top;
	KT_ASSERT_ER(dt_scr_save(px, w, h, (CONST UB *)"画面印刷の画像", NULL, &u), E_OK);
	(void)fs_mkdir("/boot/SCRPNG");
	KT_ASSERT_ER(xfu_fs_tree(fs, "/boot", &t), E_OK);
	knl_memset(&opt, 0, sizeof(opt));
	opt.flags = XFU_F_IMAGE;
	KT_ASSERT_ER(xfu_export(&u, &t, (CONST UB *)"SCRPNG", &opt, name, sizeof(name)), E_OK);
	tm_printf((UB *)"  given out as %s\n", name);
	KT_ASSERT(find(name, s_len((CONST char *)name), "画面印刷の画像.png") == 0);
	knl_strcpy(path, "/boot/SCRPNG/");
	knl_strcat(path, (CONST char *)name);
	b = (UB *)Kmalloc(4 * 1024 * 1024);
	fd = fs_open(path, O_RDONLY);
	KT_ASSERT(fd >= 0 && b != NULL);
	if ( fd >= 0 && b != NULL ) {
		while ( ( k = fs_read(fd, b + n, 4 * 1024 * 1024 - n) ) > 0 ) n += k;
		fs_close(fd);
		KT_ASSERT(n > 8 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G');
		KT_ASSERT_ER(img_png_decode(b, (SZ)n, &dec, &dw, &dh), E_OK);
		KT_ASSERT(dw == w && dh == h);
		if ( dec != NULL && dw == w && dh == h ) {
			for ( i = 0; i < w * h; i++ ) {
				if ( ( dec[i] & RGB ) != px[i] ) bad++;
			}
			tm_printf((UB *)"  the file: %d bytes, %dx%d, %d pixels differ\n", n, dw, dh, bad);
			KT_ASSERT_EQ(bad, 0);
		}
		if ( dec != NULL ) Kfree(dec);
	} else if ( fd >= 0 ) {
		fs_close(fd);
	}
	(void)fs_unlink(path);

	/* a figure with no picture is given out as it would be otherwise */
	KT_ASSERT_ER(om_cre_obj(NULL, (CONST UB *)"絵の無い図形", OM_BODY_FIG, &e), E_OK);
	KT_ASSERT_ER(xfu_export(&e, &t, (CONST UB *)"SCRPNG", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(find(name, s_len((CONST char *)name), ".xtad") > 0);
	knl_strcpy(path, "/boot/SCRPNG/");
	knl_strcat(path, (CONST char *)name);
	(void)fs_unlink(path);
	(void)fs_rmdir("/boot/SCRPNG");
	(void)ob_del_obj(&e);
	(void)ob_del_obj(&u);
	if ( b != NULL ) Kfree(b);
	Kfree(px);
	Kfree(fs);
}

/* A file left on /boot for the tools on the host */
LOCAL void leave( CONST char *path, CONST UB *b, SZ n )
{
	INT	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC), at = 0, k;

	if ( fd < 0 ) return;
	while ( at < (INT)n ) {
		k = fs_write(fd, b + at, (SZ)( n - at ));
		if ( k <= 0 ) break;
		at += k;
	}
	fs_close(fd);
}

LOCAL void test_print( void )
{
	T_DPRECT	at;
	UW		*px, *sheet = NULL;
	DTSCRPIC	pic;
	T_PRNJOB	job;
	T_PRNSET	set;
	T_PRNSTAT	st;
	UB		*pdf = NULL;
	SZ		len = 0;
	TS_UUID		u;
	INT		sw2 = 300, sh2 = 300, pw, ph, pct;

	if ( !start() ) KT_SKIP("no screen");
	px = sample(&at);
	KT_ASSERT(px != NULL);
	if ( px == NULL ) return;
	pic.px = px;
	pic.w = at.right - at.left;
	pic.h = at.bottom - at.top;
	dt_scr_job(&job, &pic, (CONST UB *)"画面印刷");
	KT_ASSERT_EQ(job.npage, 1);
	prn_default(&set);
	KT_ASSERT_EQ(prn_sheets(&job, &set), 1);
	KT_ASSERT_ER(prn_pdf(&job, &set, &pdf, &len), E_OK);
	if ( pdf != NULL ) {
		prn_stat(&st);
		tm_printf((UB *)"  the file: %d bytes, %d pages, %d pictures\n", (INT)len, st.pages, st.images);
		KT_ASSERT_EQ(st.pages, 1);
		KT_ASSERT_EQ(st.images, 1);
		KT_ASSERT(find(pdf, (INT)len, "/Subtype /Image") > 0);
		KT_ASSERT(find(pdf, (INT)len, "/Width 400") > 0);
		KT_ASSERT(find(pdf, (INT)len, "/Height 300") > 0);
		leave("/boot/SCRPRN.PDF", pdf, len);
		Kfree(pdf);
	}
	/* the picture on the sheet as 画面表示 would draw it: the paper's middle is the picture's */
	prn_paper(&job, &set, &pw, &ph, &pct);
	sheet = (UW *)Kmalloc(sizeof(UW) * (SZ)sw2 * sh2);
	if ( sheet != NULL ) {
		sh2 = (INT)( (D)sw2 * ph / pw );
		Kfree(sheet);
		sheet = (UW *)Kmalloc(sizeof(UW) * (SZ)sw2 * sh2);
	}
	if ( sheet != NULL ) {
		KT_ASSERT_ER(prn_sheet(&job, &set, 0, sheet, sw2, sh2), E_OK);
		Kfree(sheet);
	}
	/* and made an object, as the desktop does */
	KT_ASSERT_ER(prn_object(&job, &set, NULL, &u), E_OK);
	KT_ASSERT_EQ(xf_data_rec(&u), 1);
	(void)ob_del_obj(&u);
	Kfree(px);
}

EXPORT void ktest_scr( void )
{
	KT_RUN(test_screen);
	KT_RUN(test_front);
	KT_RUN(test_range);
	KT_RUN(test_save);
	KT_RUN(test_tray);
	KT_RUN(test_tray_doc);
	KT_RUN(test_loupe);
	KT_RUN(test_volume);
	KT_RUN(test_png);
	KT_RUN(test_print);
}
