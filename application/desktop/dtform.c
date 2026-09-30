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
 *	message, and the forms of the tools and editors (位置あわせ, 検索/置換,
 *	実身/仮身検索, 用紙設定, the fields and the lists), with the helpers
 *	(dt_pn_*) the rest of the desktop builds its own panels with.
 */

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
	INT	i;

	for ( i = 0; s != NULL && s[i] != 0 && i < max - 1; i++ ) {
		to[i] = (UB)s[i];
	}
	while ( i > 0 && s != NULL && s[i] != 0 && ( (UB)s[i] & 0xC0 ) == 0x80 ) {
		i--;
	}
	to[i] = 0;
}

/*
 * How far a panel's contents stand in from its outline: its frame is
 * eight wide and four in, and what is inside keeps clear of it.
 */

EXPORT T_WMPANEL *dt_pn_new( INT w, INT h )
{
	T_WMPANEL	*p = (T_WMPANEL *)Kmalloc(sizeof(T_WMPANEL));

	if ( p != NULL ) {
		knl_memset(p, 0, sizeof(*p));
		p->num = 2;
		p->kind = WM_PNL_PLAIN;
		p->r.right = w + 2 * DT_PN_IN;
		p->r.bottom = h + 2 * DT_PN_IN;
	}

	return p;
}

/* One part, in the panel's own coordinates */
EXPORT T_WMPART *dt_pn_add( T_WMPANEL *p, UW type, INT num, INT l, INT t,
			INT w, INT h, CONST char *label )
{
	T_WMPART	*pt;

	if ( p->npart >= WM_PART_MAX ) {
		return NULL;
	}
	pt = &p->part[p->npart++];
	pt->type = type;
	pt->num = num;
	pt->r.left = DT_PN_IN + l;
	pt->r.top = DT_PN_IN + t;
	pt->r.right = DT_PN_IN + l + w;
	pt->r.bottom = DT_PN_IN + t + h;
	dt_pn_text(pt->label, WM_LABEL_MAX, label);

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
 * agreed to or abandoned. Enter agrees and Escape abandons. The panel
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
		Kfree(def);
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
	Kfree(def);
	if ( pid < 0 ) {
		return WM_ANS_CANCEL;
	}
	*p_pid = pid;
	wm_focus(d->wid);
	wm_panel_draw(pid);
	wm_update();

	while ( ans == WM_ANS_NONE ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
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
		if ( def != NULL ) Kfree(def);
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
	Kfree(def);
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

EXPORT void dt_pn_done( DTWIN *d, INT pid )
{
	if ( pid >= 0 ) {
		wm_panel_close(pid);
	}
	dt_draw(d);
	wm_composite();
}

EXPORT INT dt_pn_value( INT pid, INT num )
{
	INT	v = 0;

	wm_panel_get(pid, num, &v);

	return v;
}

/* One line of text asked for: a prompt of one or two lines and a box */
LOCAL BOOL ask_text( DTWIN *d, CONST char *p1, CONST char *p2,
		     CONST UB *start, UB *out, INT max, CONST char *ok )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, top = 10, i;
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
		for ( i = 0; start != NULL && start[i] != 0
			     && i < WM_LABEL_MAX - 1; i++ ) {
			pt->text[i] = start[i];
		}
		pt->text[i] = 0;
		pt->caret = i;
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

/* ---------------------------------------------------------------- 検索/置換 */

#define FD_WHAT		1
#define FD_WITH		2
#define FD_REGEX	5

/*
 * What to look for in a text and what to put in its place, kept from
 * one asking to the next. Answers what was pressed: DT_FIND_NEXT,
 * DT_FIND_REPLACE, DT_FIND_ALL, or 0 when the panel was put away.
 */
EXPORT INT dt_find_form( DTWIN *d, UB *what, UB *with, INT max,
			  BOOL *p_regex )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, w = 460, h = 150, i;
	UINT		ans;

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return 0;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 12, 92, 22, "検索文字列");
	pt = dt_pn_add(def, WM_PT_BOX, FD_WHAT, 110, 8, w - 124, 26, NULL);
	if ( pt != NULL ) {
		for ( i = 0; what[i] != 0 && i < WM_LABEL_MAX - 1; i++ ) {
			pt->text[i] = what[i];
		}
		pt->text[i] = 0;
		pt->caret = i;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 46, 92, 22, "置換文字列");
	pt = dt_pn_add(def, WM_PT_BOX, FD_WITH, 110, 42, w - 124, 26, NULL);
	if ( pt != NULL ) {
		for ( i = 0; with[i] != 0 && i < WM_LABEL_MAX - 1; i++ ) {
			pt->text[i] = with[i];
		}
		pt->text[i] = 0;
		pt->caret = i;
	}
	pt = dt_pn_add(def, WM_PT_CHECK, FD_REGEX, 110, 76, 160, 22, "正規表現");
	if ( pt != NULL ) {
		pt->value = *p_regex ? 1 : 0;
	}
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_CANCEL, 14, h - 36, 80, 26, "取消");
	if ( pt != NULL ) pt->answer = WM_ANS_CANCEL;
	pt = dt_pn_add(def, WM_PT_BUTTON, 3, w - 300, h - 36, 86, 26, "全置換");
	if ( pt != NULL ) pt->answer = DT_FIND_ALL;
	pt = dt_pn_add(def, WM_PT_BUTTON, 4, w - 204, h - 36, 86, 26, "置換");
	if ( pt != NULL ) pt->answer = DT_FIND_REPLACE;
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, w - 108, h - 36, 94, 26,
		    "次を検索");
	if ( pt != NULL ) pt->answer = DT_FIND_NEXT;

	ans = dt_pn_run(d, def, &pid);
	if ( pid >= 0 && ans != WM_ANS_CANCEL ) {
		wm_panel_text(pid, FD_WHAT, what, max);
		wm_panel_text(pid, FD_WITH, with, max);
		*p_regex = (BOOL)( dt_pn_value(pid, FD_REGEX) != 0 );
	}
	dt_pn_done(d, pid);
	if ( ans == WM_ANS_OK ) {
		ans = DT_FIND_NEXT;		/* Enter */
	}

	return ( ans == DT_FIND_NEXT || ans == DT_FIND_REPLACE
		 || ans == DT_FIND_ALL ) ? (INT)ans : 0;
}

/* ---------------------------------------------------------------- 実身/仮身検索 */

#define SQ_TEXT		1
#define SQ_FROM		2
#define SQ_TO		3
#define SQ_G_TARGET	10		/* 実身名 続柄 本文 全て */
#define SQ_G_DATE	20		/* なし 作成日 更新日 参照日 */

/* A group of choices across a row, the one numbered 'on' chosen */
LOCAL void sq_row( T_WMPANEL *p, INT group, INT y, CONST char * CONST *names,
		   INT n, INT on )
{
	T_WMPART	*pt;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		pt = dt_pn_add(p, WM_PT_CHOICE, group + i, 110 + i * 90, y, 84, 22,
			    names[i]);
		if ( pt != NULL ) {
			pt->group = group;
			pt->value = ( i == on ) ? 1 : 0;
		}
	}
}

LOCAL void sq_box( T_WMPANEL *p, INT num, INT x, INT y, INT w, CONST UB *s )
{
	T_WMPART	*pt = dt_pn_add(p, WM_PT_BOX, num, x, y, w, 26, NULL);
	INT		i;

	if ( pt == NULL ) {
		return;
	}
	for ( i = 0; s != NULL && s[i] != 0 && i < WM_LABEL_MAX - 1; i++ ) {
		pt->text[i] = s[i];
	}
	pt->text[i] = 0;
	pt->caret = i;
}

/*
 * What to look for: the words and where to look for them -- the name,
 * the relationship tags, the text, or all three -- and, if the dates
 * matter, which date and the days it must fall between. Answers TRUE
 * when 検索 was pressed, with the question in 'q'.
 */
EXPORT BOOL dt_search_form( DTWIN *d, T_DTQUERY *q )
{
	CONST char * CONST t_n[] = { "実身名", "続柄", "本文", "全て" };
	CONST char * CONST d_n[] = { "なし", "作成日", "更新日", "参照日" };
	T_WMPANEL	*def;
	INT		pid, i;
	UINT		ans;

	def = dt_pn_new(480, 196);
	if ( def == NULL ) {
		return FALSE;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 12, 92, 22, "検索文字列");
	sq_box(def, SQ_TEXT, 110, 8, 356, q->text);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 46, 92, 22, "検索対象");
	sq_row(def, SQ_G_TARGET, 44, t_n, 4, q->target);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 80, 92, 22, "日付");
	sq_row(def, SQ_G_DATE, 78, d_n, 4, q->date_kind);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 114, 92, 22, "期間");
	sq_box(def, SQ_FROM, 110, 110, 150, q->from);
	dt_pn_add(def, WM_PT_LABEL, 0, 268, 114, 24, 22, "〜");
	sq_box(def, SQ_TO, 296, 110, 150, q->to);
	dt_pn_buttons(def, "検索");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		wm_panel_text(pid, SQ_TEXT, q->text, WM_LABEL_MAX);
		wm_panel_text(pid, SQ_FROM, q->from, sizeof(q->from));
		wm_panel_text(pid, SQ_TO, q->to, sizeof(q->to));
		for ( i = 0; i < 4; i++ ) {
			if ( dt_pn_value(pid, SQ_G_TARGET + i) != 0 ) {
				q->target = i;
			}
			if ( dt_pn_value(pid, SQ_G_DATE + i) != 0 ) {
				q->date_kind = i;
			}
		}
	}
	dt_pn_done(d, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

/* ---------------------------------------------------------------- forms */

/*
 * A panel of labelled boxes, one under another, each with a word after
 * it saying what it is counted in. What each box holds comes in and
 * goes out in 'vals'.
 */
EXPORT BOOL dt_fields_form( DTWIN *d, CONST char *title, INT n,
			    CONST char *CONST *labels,
			    CONST char *CONST *hints,
			    UB (*vals)[DT_FIELD_MAX] )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i, k, y = 4, w = 440;
	UINT		ans;

	if ( n > 12 ) {
		n = 12;
	}
	def = dt_pn_new(w, 36 + n * 32 + 48);
	if ( def == NULL ) {
		return FALSE;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, y, w - 28, 22, title);
	y += 32;
	for ( i = 0; i < n; i++ ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, y + 2, 130, 22, labels[i]);
		pt = dt_pn_add(def, WM_PT_BOX, 1 + i, 150, y, 110, 26, NULL);
		if ( pt != NULL ) {
			for ( k = 0; vals[i][k] != 0 && k < WM_LABEL_MAX - 1
				     && k < DT_FIELD_MAX - 1; k++ ) {
				pt->text[k] = vals[i][k];
			}
			pt->text[k] = 0;
			pt->caret = k;
		}
		if ( hints != NULL && hints[i] != NULL ) {
			dt_pn_add(def, WM_PT_LABEL, 0, 268, y + 2, w - 282, 22,
			       hints[i]);
		}
		y += 32;
	}
	dt_pn_buttons(def, "設定");

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		for ( i = 0; i < n; i++ ) {
			wm_panel_text(pid, 1 + i, vals[i], DT_FIELD_MAX);
		}
	}
	dt_pn_done(d, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

/* One of a list of names chosen: its number, or -1 */
EXPORT INT dt_list_form( DTWIN *d, CONST char *title,
			 CONST char *CONST *names, INT n, INT start )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i, at = -1, cnt = 0, v = 0;
	UINT		ans;

	def = dt_pn_new(380, 290);
	if ( def == NULL ) {
		return -1;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, 352, 22, title);
	pt = dt_pn_add(def, WM_PT_LIST, 1, 14, 32, 352, 196, NULL);
	for ( i = 0; i < n; i++ ) {
		INT	k = wm_panel_name(def, (CONST UB *)names[i]);

		if ( k < 0 ) {
			break;
		}
		if ( at < 0 ) {
			at = k;
		}
		cnt++;
	}
	if ( pt != NULL ) {
		pt->pool = ( at >= 0 ) ? at : 0;
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

/* 用紙設定 */

typedef struct {
	CONST char	*name;
	INT		w, h;		/* tenths of a millimetre */
} PAPERSZ;

LOCAL CONST PAPERSZ dt_papers[] = {
	{ "A3", 2970, 4200 },		{ "A4", 2100, 2970 },
	{ "A5", 1480, 2100 },		{ "B4", 2570, 3640 },
	{ "B5", 1820, 2570 },		{ "はがき", 1000, 1480 },
	{ "往復はがき", 1000, 2000 },	{ "長形3号", 1200, 2350 },
	{ "寸法指定", 0, 0 }
};
#define N_PAPERS	( (INT)( sizeof(dt_papers) / sizeof(dt_papers[0]) ) )

#define PS_LIST		1
#define PS_H		2
#define PS_W		3
#define PS_TOP		4
#define PS_BOTTOM	5
#define PS_LEFT		6
#define PS_RIGHT	7
#define PS_ONE		8
#define PS_SPREAD	9
#define PS_STD		92
#define PS_ANS_STD	3
#define PS_HF		93
#define PS_ANS_HF	4

/* Tenths as words: "210" or "210.5" */
LOCAL void tenths_text( INT v, UB *out )
{
	char	t[12];
	INT	n = 0, k = 0, whole = v / 10;

	if ( v < 0 ) {
		v = 0;
		whole = 0;
	}
	do {
		t[n++] = (char)( '0' + whole % 10 );
		whole /= 10;
	} while ( whole > 0 && n < 10 );
	while ( n > 0 ) {
		out[k++] = (UB)t[--n];
	}
	if ( v % 10 != 0 ) {
		out[k++] = '.';
		out[k++] = (UB)( '0' + v % 10 );
	}
	out[k] = 0;
}

/* Words as tenths; -1 when they are not a number */
LOCAL INT text_tenths( CONST UB *s )
{
	INT	v = 0, f = -1;
	BOOL	any = FALSE;

	while ( *s == ' ' ) {
		s++;
	}
	for ( ; *s != 0; s++ ) {
		if ( *s >= '0' && *s <= '9' ) {
			if ( f < 0 ) {
				v = v * 10 + ( *s - '0' );
			} else if ( f == 0 ) {
				f = *s - '0';
			}
			any = TRUE;
		} else if ( *s == '.' && f < 0 ) {
			f = 0;
		} else if ( *s != ' ' ) {
			return -1;
		}
	}

	return any ? v * 10 + ( ( f > 0 ) ? f : 0 ) : -1;
}

LOCAL void ps_box( T_WMPANEL *p, INT num, INT x, INT y, CONST char *label,
		   INT v )
{
	T_WMPART	*pt;
	INT		k;

	dt_pn_add(p, WM_PT_LABEL, 0, x, y + 2, 30, 22, label);
	pt = dt_pn_add(p, WM_PT_BOX, num, x + 30, y, 76, 26, NULL);
	if ( pt != NULL ) {
		tenths_text(v, pt->text);
		for ( k = 0; pt->text[k] != 0; k++ ) {
			;
		}
		pt->caret = k;
	}
	dt_pn_add(p, WM_PT_LABEL, 0, x + 110, y + 2, 40, 22, "mm");
}

EXPORT INT dt_paper_form( DTWIN *d, T_DTPAPER *pp )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i, at = -1, cnt = 0, v, start = N_PAPERS - 1;
	INT		w = 470, h = 320;
	UINT		ans;
	UB		txt[DT_FIELD_MAX];

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return DT_PAPER_CANCEL;
	}
	for ( i = 0; i < N_PAPERS - 1; i++ ) {
		if ( dt_papers[i].w == pp->w && dt_papers[i].h == pp->h ) {
			start = i;
		}
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, 50, 22, "用紙:");
	pt = dt_pn_add(def, WM_PT_LIST, PS_LIST, 64, 4, 170, 170, NULL);
	for ( i = 0; i < N_PAPERS; i++ ) {
		INT	k = wm_panel_name(def, (CONST UB *)dt_papers[i].name);

		if ( k < 0 ) {
			break;
		}
		if ( at < 0 ) {
			at = k;
		}
		cnt++;
	}
	if ( pt != NULL ) {
		pt->pool = ( at >= 0 ) ? at : 0;
		pt->count = cnt;
		pt->top = 1;
		pt->value = start + 1;
	}
	ps_box(def, PS_H, 250, 4, "縦:", pp->h);
	ps_box(def, PS_W, 250, 38, "横:", pp->w);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 188, 50, 22, "余白:");
	ps_box(def, PS_TOP, 64, 186, "上:", pp->top);
	ps_box(def, PS_LEFT, 250, 186, "左:", pp->left);
	ps_box(def, PS_BOTTOM, 64, 218, "下:", pp->bottom);
	ps_box(def, PS_RIGHT, 250, 218, "右:", pp->right);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 254, 50, 22, "割付:");
	pt = dt_pn_add(def, WM_PT_CHOICE, PS_ONE, 64, 252, 90, 22, "片面");
	if ( pt != NULL ) {
		pt->group = PS_ONE;
		pt->value = ( pp->imposition == 0 ) ? 1 : 0;
	}
	pt = dt_pn_add(def, WM_PT_CHOICE, PS_SPREAD, 160, 252, 90, 22, "見開き");
	if ( pt != NULL ) {
		pt->group = PS_ONE;
		pt->value = ( pp->imposition != 0 ) ? 1 : 0;
	}
	dt_pn_buttons(def, "設定");
	pt = dt_pn_add(def, WM_PT_BUTTON, PS_STD, w - 292, h - 36, 86, 26, "標準設定");
	if ( pt != NULL ) {
		pt->answer = PS_ANS_STD;
	}
	pt = dt_pn_add(def, WM_PT_BUTTON, PS_HF, w - 414, h - 36, 116, 26, "ヘッダ/フッタ");
	if ( pt != NULL ) {
		pt->answer = PS_ANS_HF;
	}

	ans = dt_pn_run(d, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		CONST INT	nums[6] = { PS_H, PS_W, PS_TOP, PS_BOTTOM,
					    PS_LEFT, PS_RIGHT };
		INT		*to[6];

		to[0] = &pp->h;    to[1] = &pp->w;
		to[2] = &pp->top;  to[3] = &pp->bottom;
		to[4] = &pp->left; to[5] = &pp->right;
		for ( i = 0; i < 6; i++ ) {
			wm_panel_text(pid, nums[i], txt, DT_FIELD_MAX);
			v = text_tenths(txt);
			if ( v >= 0 ) {
				*to[i] = v;
			}
		}
		/* a size chosen from the list is that size */
		v = dt_pn_value(pid, PS_LIST);
		if ( v >= 1 && v < N_PAPERS ) {
			pp->w = dt_papers[v - 1].w;
			pp->h = dt_papers[v - 1].h;
		}
		pp->imposition = ( dt_pn_value(pid, PS_SPREAD) != 0 ) ? 1 : 0;
	}
	dt_pn_done(d, pid);

	return ( ans == WM_ANS_OK ) ? DT_PAPER_SET
	     : ( ans == PS_ANS_STD ) ? DT_PAPER_STD
	     : ( ans == PS_ANS_HF ) ? DT_PAPER_HF : DT_PAPER_CANCEL;
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
