/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtform.c
 *	The desktop's panels that ask and tell: a line of text, yes or no, a
 *	message, and the forms of the tools (位置あわせ, 実身/仮身検索 and the
 *	lists), with the helpers
 *	(dt_pn_*) the rest of the desktop builds its own panels with.
 */

#include <stdarg.h>
#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/fn.h>
#include "desktop.h"

#define KEY_ENTER	0x28

/* ---------------------------------------------------------------- panels */

/* Words copied, cut at a letter's edge when they do not fit */
EXPORT void dt_pn_text( UB *to, INT max, CONST char *s )
{
	(void)dt_fit(to, max, (CONST UB *)s);
}

/*
 * How far a panel's contents stand in from its outline: its frame is
 * eight wide and four in, and what is inside keeps clear of it.
 */

EXPORT T_WMPANEL *dt_pn_new( INT w, INT h )
{
	T_WMPANEL	*p = wm_panel_new(0);

	if ( p != NULL ) {
		p->num = 2;
		p->kind = WM_PNL_PLAIN;
		p->r.right = w + 2 * DT_PN_IN;
		p->r.bottom = h + 2 * DT_PN_IN;
	}

	return p;
}

/*
 * One part, in the panel's own coordinates: NULL when the panel's table
 * of parts could not be made larger. The part answered is to be filled
 * in before the next is added, as the table may move.
 */
EXPORT T_WMPART *dt_pn_add( T_WMPANEL *p, UW type, INT num, INT l, INT t,
			INT w, INT h, CONST char *label )
{
	T_WMPART	*pt = wm_panel_part_add(p);

	if ( pt == NULL ) {
		return NULL;
	}
	pt->type = type;
	pt->num = num;
	pt->r.left = DT_PN_IN + l;
	pt->r.top = DT_PN_IN + t;
	pt->r.right = DT_PN_IN + l + w;
	pt->r.bottom = DT_PN_IN + t + h;
	dt_pn_text(pt->label, WM_LABEL_MAX, label);

	return pt;
}

EXPORT T_WMPART *dt_pn_colour( T_WMPANEL *p, INT num, INT l, INT t, INT w, INT h, UW col )
{
	T_WMPART	*pt = dt_pn_add(p, WM_PT_SWATCH, num, l, t, w, h, NULL);

	if ( pt != NULL ) {
		pt->colour = col & 0x00FFFFFFU;
		pt->answer = DT_PN_COLOUR_ANS + (UINT)num;
	}

	return pt;
}

/* The two buttons every panel here ends with, at its foot */

EXPORT void dt_pn_buttons( T_WMPANEL *p, CONST char *ok )
{
	INT		w = p->r.right - 2 * DT_PN_IN, h = p->r.bottom - 2 * DT_PN_IN;
	T_WMPART	*pt;

	pt = dt_pn_add(p, WM_PT_BUTTON, DT_PN_CANCEL, w - 196, h - 36, 86, 26, "取消");
	if ( pt != NULL ) {
		pt->answer = WM_ANS_CANCEL;
	}
	pt = dt_pn_add(p, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, w - 100, h - 36, 86, 26,
		    ok);
	if ( pt != NULL ) {
		pt->answer = WM_ANS_OK;
	}
}

/*
 * The panel, put in the middle of the window and worked until it is
 * agreed to or abandoned, or dt_quit ends the session (dt_stopping,
 * as abandoned). Enter agrees and Escape abandons. The panel
 * is left open, so that what was put in it can be read; pn_done puts
 * it away.
 */
EXPORT UINT dt_pn_run( DTWIN *d, T_WMPANEL *def, INT *p_pid )
{
	T_WMWIN	w;
	T_WMEV	ev;
	INT	pid, cx, cy, pw, ph;
	UINT	ans = WM_ANS_NONE;

	*p_pid = -1;
	if ( wm_ref(d->wid, &w) < E_OK ) {
		wm_panel_free(def);
		return WM_ANS_CANCEL;
	}
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	cx = ( w.work.right - w.work.left ) / 2;
	cy = ( w.work.bottom - w.work.top ) / 2;
	def->r.left = cx - pw / 2;
	def->r.top = cy - ph / 2;
	if ( def->r.left < 0 ) def->r.left = 0;
	if ( def->r.top < 0 )  def->r.top = 0;
	def->r.right = def->r.left + pw;
	def->r.bottom = def->r.top + ph;

	pid = wm_panel_open_centre(d->wid, def);
	wm_panel_free(def);
	if ( pid < 0 ) {
		return WM_ANS_CANCEL;
	}
	*p_pid = pid;
	wm_focus(d->wid);
	wm_panel_draw(pid);
	wm_update();

	while ( ans == WM_ANS_NONE ) {
		if ( dt_stopping() ) {
			ans = WM_ANS_CANCEL;	/* dt_quit ends the session: nobody is left to answer */
			break;
		}
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		/*
		 * A key typed into another window -- one the person moved the
		 * keys to, a program's question that took them -- is that
		 * window's: the panel is answered only by what is typed to it
		 */
		if ( ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) && ev.wid != d->wid ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ENTER ) {
			ans = WM_ANS_OK;
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) {
			ev.wid = d->wid;	/* the letters are the panel's */
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		if ( ans >= DT_PN_COLOUR_ANS ) {
			INT	num = (INT)( ans - DT_PN_COLOUR_ANS );
			UW	col = 0;

			ans = WM_ANS_NONE;
			if ( wm_panel_colour(pid, num, &col) >= E_OK && dt_pick_colour_part(d->wid, pid, num, &col) ) {
				(void)wm_panel_set_colour(pid, num, col);
			}
			dt_draw(d);
			wm_panel_draw(pid);
		}
		wm_update();
	}

	return ans;
}

LOCAL INT pn_fit_width( CONST char *l1, CONST char *l2, INT least );

/*
 * A question that answers itself if it is not answered: Enter or the
 * button that agrees within 'ms' agrees; any other key, the other
 * button, or the time running out does not. For a change that may have
 * left the person unable to see the screen to answer.
 */
EXPORT BOOL dt_confirm_timed( DTWIN *d, CONST char *CONST *lines, INT nline,
			      CONST char *no, CONST char *yes, INT ms )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_WMWIN		w;
	T_WMEV		ev;
	UD		now = 0, end;
	INT		pid, i, pw = 480, ph = 24 * nline + 56;
	UINT		ans = WM_ANS_NONE;

	for ( i = 0; i < nline; i++ ) {
		INT	t = pn_fit_width(lines[i], NULL, 480);

		if ( t > pw ) pw = t;
	}
	def = dt_pn_new(pw, ph);
	if ( def == NULL || wm_ref(d->wid, &w) < E_OK ) {
		wm_panel_free(def);
		return FALSE;
	}
	def->kind = WM_PNL_WARN;
	for ( i = 0; i < nline; i++ ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, 10 + i * 24, pw - 28, 22, lines[i]);
	}
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_CANCEL, pw - 212, ph - 36, 94, 26, no);
	if ( pt != NULL ) pt->answer = WM_ANS_CANCEL;
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, pw - 108, ph - 36, 94, 26, yes);
	if ( pt != NULL ) pt->answer = WM_ANS_OK;
	pid = wm_panel_open_centre(d->wid, def);
	wm_panel_free(def);
	if ( pid < 0 ) {
		return FALSE;
	}
	wm_focus(d->wid);
	wm_panel_draw(pid);
	wm_update();
	(void)ts_get_mono(&now);
	end = now + (UD)ms * 1000000ULL;
	while ( ans == WM_ANS_NONE ) {
		(void)ts_get_mono(&now);
		if ( now >= end ) {
			ans = WM_ANS_CANCEL;
			break;
		}
		if ( wm_read_event(&ev, 500) < E_OK ) {
			continue;
		}
		/*
		 * A key typed into another window -- one the person moved the
		 * keys to, a program's question that took them -- is that
		 * window's: the panel is answered only by what is typed to it
		 */
		if ( ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) && ev.wid != d->wid ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN ) {
			ans = ( ev.code == KEY_ENTER ) ? WM_ANS_OK : WM_ANS_CANCEL;
			break;
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		wm_update();
	}
	dt_pn_done(d, pid);
	return (BOOL)( ans == WM_ANS_OK );
}

/*
 * Each screen's number shown on it for ms milliseconds, in a panel at its
 * middle (システム環境設定's 識別, 10.4.10), so that the person can tell
 * which screen is which
 */
EXPORT void dt_identify( DTWIN *d, INT ms )
{
	T_WMPANEL	*def;
	INT		pid[DISP_SCREENS_MAX], n = ts_disp_screens(), i, x, y, w, h;
	char		t[16];

	if ( n > DISP_SCREENS_MAX ) n = DISP_SCREENS_MAX;
	for ( i = 0; i < n; i++ ) {
		pid[i] = -1;
		if ( ts_disp_screen(i, &x, &y, &w, &h) < E_OK ) continue;
		def = dt_pn_new(200, 64);
		if ( def == NULL ) continue;
		tm_sprintf((UB *)t, (UB *)"画面 %d", i + 1);
		(void)dt_pn_add(def, WM_PT_LABEL, 0, 14, 20, 172, 24, t);
		pid[i] = wm_panel_open_at(d->wid, def, x + ( w - 200 ) / 2, y + ( h - 64 ) / 2);
		wm_panel_free(def);
		if ( pid[i] >= 0 ) wm_panel_draw(pid[i]);
	}
	wm_update();
	tm_printf((UB *)"TessronOS desktop: the screens' numbers shown, %d\n", n);
	tk_dly_tsk((RELTIM)ms);
	for ( i = 0; i < n; i++ ) {
		if ( pid[i] >= 0 ) wm_panel_close(pid[i]);
	}
	dt_draw(d);
	wm_update();
}

EXPORT void dt_pn_done( DTWIN *d, INT pid )
{
	if ( pid >= 0 ) {
		wm_panel_close(pid);
	}
	dt_draw(d);
	wm_update();
}

EXPORT INT dt_pn_value( INT pid, INT num )
{
	INT	v = 0;

	wm_panel_get(pid, num, &v);

	return v;
}

EXPORT INT dt_fit( UB *dst, INT max, CONST UB *s )
{
	SZ	n = 0;

	if ( dst == NULL || max < 1 ) {
		return 0;
	}
	if ( s != NULL ) {
		while ( s[n] != 0 ) {
			n++;
		}
		n = knl_utf8_fit(s, n, (SZ)max - 1);
		knl_memcpy(dst, s, (size_t)n);
	}
	dst[n] = 0;
	return (INT)n;
}

EXPORT UB *dt_cat( CONST char *s, ... )
{
	va_list		ap;
	CONST char	*t;
	SZ		n = 0, k;
	UB		*out;

	va_start(ap, s);
	for ( t = s; t != NULL; t = va_arg(ap, CONST char *) ) {
		n += knl_strlen(t);
	}
	va_end(ap);
	out = (UB *)Kmalloc((size_t)n + 1);
	if ( out == NULL ) {
		tm_printf((UB *)"desktop: no memory for a message of %d bytes\n", (INT)n);
		return NULL;
	}
	n = 0;
	va_start(ap, s);
	for ( t = s; t != NULL; t = va_arg(ap, CONST char *) ) {
		k = knl_strlen(t);
		knl_memcpy(out + n, t, (size_t)k);
		n += k;
	}
	va_end(ap);
	out[n] = 0;
	return out;
}

EXPORT UB *dt_vol_label( CONST char *path )
{
	UB	*lab = NULL;
	SZ	size = 0, asz = 0;

	for ( ;; ) {
		if ( knl_obfile_vol_label(path, lab, size, &asz) < E_OK ) {
			break;
		}
		if ( lab != NULL && asz <= size ) {
			if ( lab[0] != 0 ) {
				return lab;
			}
			break;
		}
		if ( lab != NULL ) Kfree(lab);
		size = asz;
		lab = (UB *)Kmalloc((size_t)size);
		if ( lab == NULL ) {
			tm_printf((UB *)"desktop: no memory for a volume label of %d bytes\n", (INT)size);
			return NULL;
		}
	}
	if ( lab != NULL ) Kfree(lab);
	return NULL;
}

/* One line of text asked for: a prompt of one or two lines and a box */
LOCAL BOOL ask_text( DTWIN *d, CONST char *p1, CONST char *p2,
		     CONST UB *start, UB *out, INT max, CONST char *ok )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, top = 10;
	INT		w = ( p2 != NULL ) ? 620 : 460;
	UINT		ans;

	def = dt_pn_new(w, ( p2 != NULL ) ? 134 : 112);
	if ( def == NULL ) {
		return FALSE;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, top, w - 28, 22, p1);
	top += 24;
	if ( p2 != NULL ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, top, w - 28, 22, p2);
		top += 24;
	}
	pt = dt_pn_add(def, WM_PT_BOX, 1, 14, top + 2, w - 28, 26, NULL);
	if ( pt != NULL ) {
		pt->caret = dt_fit(pt->text, WM_LABEL_MAX, start);
	}
	dt_pn_buttons(def, ok);

	ans = dt_pn_run(d, def, &pid);
	out[0] = 0;
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		wm_panel_text(pid, 1, out, max);
	}
	dt_pn_done(d, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

EXPORT BOOL dt_ask_text( DTWIN *d, CONST char *p1, CONST char *p2,
			 CONST UB *start, UB *out, INT max, CONST char *ok )
{
	return ask_text(d, p1, p2, start, out, max, ok);
}

/*
 * How wide a panel of one or two lines must be for them to fit, at the
 * size panels are written at, and never narrower than 'least'.
 */
LOCAL INT pn_fit_width( CONST char *l1, CONST char *l2, INT least )
{
	ID	fid = fn_system();
	INT	w = least, t;

	if ( fid <= 0 ) {
		return w;
	}
	fn_set_size(fid, 16);
	t = ( l1 != NULL ) ? fn_width(fid, (CONST UB *)l1) + 32 : 0;
	if ( t > w ) w = t;
	t = ( l2 != NULL ) ? fn_width(fid, (CONST UB *)l2) + 32 : 0;
	if ( t > w ) w = t;

	return w;
}

/*
 * A question with two answers, in a warning panel: one or two lines,
 * and the two buttons at the foot -- the one that does nothing first,
 * the one that goes ahead after it. Escape is the first; Enter is the
 * second. TRUE when it was the second.
 */
EXPORT BOOL dt_confirm( DTWIN *d, CONST char *l1, CONST char *l2,
			CONST char *no, CONST char *yes )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, w = pn_fit_width(l1, l2, 480);
	INT		h = ( l2 != NULL ) ? 100 : 76;
	UINT		ans;

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return FALSE;
	}
	def->kind = WM_PNL_WARN;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 10, w - 28, 22, l1);
	if ( l2 != NULL ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, 34, w - 28, 22, l2);
	}
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_CANCEL, w - 212, h - 36, 94, 26, no);
	if ( pt != NULL ) {
		pt->answer = WM_ANS_CANCEL;
	}
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, w - 108, h - 36, 94,
		    26, yes);
	if ( pt != NULL ) {
		pt->answer = WM_ANS_OK;
	}
	ans = dt_pn_run(d, def, &pid);
	dt_pn_done(d, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

/* Something told, in a panel of one or two lines and 了解 */
EXPORT void dt_tell( DTWIN *d, CONST char *l1, CONST char *l2 )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, w = pn_fit_width(l1, l2, 480);
	INT		h = ( l2 != NULL ) ? 100 : 76;

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 10, w - 28, 22, l1);
	if ( l2 != NULL ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, 34, w - 28, 22, l2);
	}
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, w - 108, h - 36, 94,
		    26, "了解");
	if ( pt != NULL ) {
		pt->answer = WM_ANS_OK;
	}
	(void)dt_pn_run(d, def, &pid);
	dt_pn_done(d, pid);
}

/* An edit given up for want of memory, what was there left as it was */
EXPORT void dt_no_memory( DTWIN *d )
{
	dt_tell(d, "メモリが足りないため、この操作をできませんでした", "データは操作の前のままです");
}

/* ---------------------------------------------------------------- 位置あわせ */

#define AL_G_H		10		/* なし 左 中央 右 */
#define AL_G_V		20		/* なし 上 中央 下 */

EXPORT BOOL dt_align_form( DTWIN *d, INT *p_h, INT *p_v )
{
	CONST char * CONST h_n[] = { "なし", "左", "中央", "右" };
	CONST char * CONST v_n[] = { "なし", "上", "中央", "下" };
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i;
	UINT		ans;

	def = dt_pn_new(420, 118);
	if ( def == NULL ) {
		return FALSE;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 12, 80, 22, "横");
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 46, 80, 22, "縦");
	for ( i = 0; i < 4; i++ ) {
		pt = dt_pn_add(def, WM_PT_CHOICE, AL_G_H + i, 70 + i * 86, 10, 80, 22,
			    h_n[i]);
		if ( pt != NULL ) {
			pt->group = AL_G_H;
			pt->value = ( i == 1 ) ? 1 : 0;
		}
		pt = dt_pn_add(def, WM_PT_CHOICE, AL_G_V + i, 70 + i * 86, 44, 80, 22,
			    v_n[i]);
		if ( pt != NULL ) {
			pt->group = AL_G_V;
			pt->value = ( i == 0 ) ? 1 : 0;
		}
	}
	dt_pn_buttons(def, "実行");
	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		*p_h = *p_v = 0;
		for ( i = 0; i < 4; i++ ) {
			if ( dt_pn_value(pid, AL_G_H + i) != 0 ) *p_h = i;
			if ( dt_pn_value(pid, AL_G_V + i) != 0 ) *p_v = i;
		}
	}
	dt_pn_done(d, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

/* ---------------------------------------------------------------- forms */

/* One of a list of names chosen: its number, or -1 */
EXPORT INT dt_list_form( DTWIN *d, CONST char *title,
			 CONST char *CONST *names, INT n, INT start )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i, cnt = 0, v = 0;
	UINT		ans;

	def = dt_pn_new(380, 290);
	if ( def == NULL ) {
		return -1;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, 352, 22, title);
	pt = dt_pn_add(def, WM_PT_LIST, 1, 14, 32, 352, 196, NULL);
	for ( i = 0; pt != NULL && i < n; i++ ) {
		if ( wm_part_name(pt, (CONST UB *)names[i]) < 0 ) {
			break;
		}
		cnt++;
	}
	if ( pt != NULL ) {
		pt->count = cnt;
		pt->top = 1;
		pt->value = ( start >= 0 && start < cnt ) ? start + 1 : 0;
	}
	dt_pn_buttons(def, "決定");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		v = dt_pn_value(pid, 1);
	}
	dt_pn_done(d, pid);

	return ( ans == WM_ANS_OK && v >= 1 && v <= cnt ) ? v - 1 : -1;
}

/* ---------------------------------------------------------------- colours */

EXPORT void dt_colour_text( UW col, UB *out )
{
	CONST char	*hex = "0123456789abcdef";
	INT		i;

	out[0] = '#';
	for ( i = 0; i < 6; i++ ) {
		out[1 + i] = (UB)hex[( col >> ( 20 - i * 4 ) ) & 0xF];
	}
	out[7] = 0;
}

/* "#rrggbb", with or without the hash; FALSE for anything else */
EXPORT BOOL dt_colour_of( CONST UB *s, UW *p_col )
{
	UW	v = 0;
	INT	i;

	while ( *s == ' ' ) {
		s++;
	}
	if ( *s == '#' ) {
		s++;
	}
	for ( i = 0; i < 6; i++ ) {
		UB	c = s[i];
		UW	d;

		if ( c >= '0' && c <= '9' )      d = (UW)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) d = (UW)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) d = (UW)( c - 'A' + 10 );
		else return FALSE;
		v = ( v << 4 ) | d;
	}
	*p_col = v;

	return TRUE;
}
