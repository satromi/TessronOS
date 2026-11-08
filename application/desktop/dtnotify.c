/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtnotify.c
 *	通知: notices processes show the person (DT_RQ_NOTIFY,
 *	docs/tessronos/11-desktop.md 11.28)
 *
 *	Each notice is a small window of the desktop's (DTOWN) standing at
 *	the screen's top right, under those shown before it, which never
 *	takes the keys from the window that has them (WM_ATTR_NOFOCUS). Its
 *	band says where it comes from; its work area is a panel: the title,
 *	what it says in up to three lines, the process's own buttons, and
 *	〈開く〉 and 〈閉じる〉 (the safe one, framed thick). Its menu holds
 *	閉じる, 開く and the tail every window has.
 *
 *	What comes of it is answered to the process at the reply channel of
 *	its request, with the request's seq: a press of 〈開く〉 or of one of
 *	its buttons, 閉じる (its button, its frame, its menu), or its going
 *	by itself after DT_NOTE_MS unless it was asked to stay. A notice the
 *	process takes back (DT_F_NOTE_CLOSE) goes with nothing answered, and
 *	one shown again under the same number takes the place of the old.
 *	When one goes, those under it move up.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/mn.h>
#include <ts/fn.h>
#include <ts/dtreq.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include "desktop.h"

#define NT_MAX		8		/* notices shown at once; a new one puts the oldest away */
#define NT_W		340		/* the work area's width */
#define NT_LINES	3		/* the lines of what it says */
#define NT_LINE_H	20
#define NT_FH		16		/* the letters of the labels (PD_FH) */
#define NT_EDGE		12		/* from the screen's right and between notices */
#define NT_TOP		40		/* the first one's top */
#define NT_BTN_W	76
#define NT_BTN_H	26
#define NT_SRC_MAX	64
#define NT_TEXT_MAX	DT_CARRY_TEXT

#define ANS_OPEN	( WM_ANS_ACT + 1 )
#define ANS_ACT1	( WM_ANS_ACT + 2 )
#define ANS_ACT2	( WM_ANS_ACT + 3 )
#define ANS_CLOSE	( WM_ANS_ACT + 4 )

typedef struct {
	TS_UUID	reply;			/* where what comes of it is answered */
	UINT	seq;
	INT	id;			/* the process's number for it */
	UB	src[NT_SRC_MAX];
	UB	title[WM_LABEL_MAX];
	UB	line[NT_LINES][WM_LABEL_MAX];
	INT	nline;
	UB	act[2][WM_LABEL_MAX];
	BOOL	stay;
	BOOL	answered;		/* what came of it was answered, or nothing is to be */
	UW	shown;			/* when it was shown, ms */
	INT	pid;			/* its panel */
	INT	h;			/* its work area's height */
	DTWIN	*d;
} NOTE;

LOCAL CONST DTOWN	nt_own;
LOCAL NOTE		*notes[NT_MAX];		/* top to bottom */

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

LOCAL void copy( UB *to, INT max, CONST UB *from, INT n )
{
	INT	i;

	for ( i = 0; i < n && i < max - 1 && from[i] != 0; i++ ) {
		to[i] = from[i];
	}
	/* not cut in the middle of a letter */
	while ( i > 0 && i < n && from[i] != 0 && ( from[i] & 0xC0 ) == 0x80 ) {
		i--;
	}
	to[i] = 0;
}

/* The bytes of the UTF-8 letter that starts at s */
LOCAL INT letter_len( CONST UB *s )
{
	if ( s[0] < 0x80 ) return 1;
	if ( ( s[0] & 0xE0 ) == 0xC0 ) return 2;
	if ( ( s[0] & 0xF0 ) == 0xE0 ) return 3;
	return 4;
}

/*
 * What it says broken into lines as wide as the panel holds, at its line
 * feeds and where a line is full; what does not fit ends with "…"
 */
LOCAL void wrap( NOTE *n, CONST UB *s, INT width )
{
	ID	fid = fn_system();
	UB	b[WM_LABEL_MAX];
	INT	k = 0;

	if ( fid > 0 ) {
		(void)fn_set_size(fid, NT_FH);
	}
	n->nline = 0;
	while ( *s != 0 && n->nline < NT_LINES ) {
		INT	l = ( *s == '\n' ) ? 1 : letter_len(s), i;
		BOOL	full;

		if ( *s == '\n' ) {
			b[k] = 0;
			copy(n->line[n->nline++], WM_LABEL_MAX, b, k);
			k = 0;
			s++;
			continue;
		}
		if ( k + l >= (INT)sizeof(b) - 4 ) {
			full = TRUE;
		} else {
			for ( i = 0; i < l; i++ ) b[k + i] = s[i];
			b[k + l] = 0;
			full = ( fid > 0 && fn_width(fid, b) > width );
		}
		if ( full ) {
			b[k] = 0;
			copy(n->line[n->nline++], WM_LABEL_MAX, b, k);
			k = 0;
			continue;		/* the letter starts the next line */
		}
		k += l;
		s += l;
	}
	if ( k > 0 && n->nline < NT_LINES ) {
		b[k] = 0;
		copy(n->line[n->nline++], WM_LABEL_MAX, b, k);
	} else if ( *s != 0 && n->nline == NT_LINES ) {
		/* more than the lines hold: the last ends with an ellipsis */
		UB	*last = n->line[NT_LINES - 1];
		INT	len = (INT)knl_strlen((CONST char *)last);

		while ( len > 0 && len + 4 > WM_LABEL_MAX ) {
			do len--; while ( len > 0 && ( last[len] & 0xC0 ) == 0x80 );
		}
		last[len] = 0xE2;
		last[len + 1] = 0x80;
		last[len + 2] = 0xA6;
		last[len + 3] = 0;
	}
}

LOCAL T_WMPART *button( T_WMPANEL *def, INT num, UINT ans, INT x, INT y, CONST char *label, UW emph )
{
	T_WMPART	*pt = dt_pn_add(def, WM_PT_BUTTON | emph, num, x, y, NT_BTN_W, NT_BTN_H, label);

	if ( pt != NULL ) {
		pt->answer = ans;
	}
	return pt;
}

/* The panel: the title, the lines, the buttons along the foot */
LOCAL void build( NOTE *n )
{
	T_WMPANEL	*def;
	INT		y, x, i;

	if ( n->pid >= 0 ) {
		(void)wm_panel_close(n->pid);
		n->pid = -1;
	}
	n->h = 8 + 24 + NT_LINE_H * n->nline + 10 + NT_BTN_H + 8;
	def = dt_pn_new(NT_W - 2 * DT_PN_IN, n->h - 2 * DT_PN_IN);
	if ( def == NULL ) {
		return;
	}
	def->kind = WM_PNL_BARE;
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = NT_W;
	def->r.bottom = n->h;
	(void)dt_pn_add(def, WM_PT_LABEL, 0, 12, 8, NT_W - 24, 22, (CONST char *)n->title);
	y = 8 + 24;
	for ( i = 0; i < n->nline; i++, y += NT_LINE_H ) {
		(void)dt_pn_add(def, WM_PT_LABEL, 0, 12, y, NT_W - 24, NT_LINE_H, (CONST char *)n->line[i]);
	}
	y += 10;
	x = 12;
	for ( i = 0; i < 2; i++ ) {
		if ( n->act[i][0] != 0 ) {
			(void)button(def, 10 + i, ( i == 0 ) ? ANS_ACT1 : ANS_ACT2, x, y, (CONST char *)n->act[i], 0);
			x += NT_BTN_W + 6;
		}
	}
	(void)button(def, 1, ANS_OPEN, NT_W - 12 - 2 * NT_BTN_W - 6, y, "開く", 0);
	(void)button(def, 2, ANS_CLOSE, NT_W - 12 - NT_BTN_W, y, "閉じる", P_EMPHAS);
	n->pid = wm_panel_open(n->d->wid, def);
	Kfree(def);
}

/* The notices stood one under another from the screen's top right */
LOCAL void stack( void )
{
	INT	sw = 0, sh = 0, top = NT_TOP, i;
	T_WMWIN	w;

	wm_screen_main(&sw, &sh);
	for ( i = 0; i < NT_MAX; i++ ) {
		NOTE		*n = notes[i];
		T_DPRECT	o;
		INT		ow, oh;

		if ( n == NULL || n->d == NULL || wm_ref(n->d->wid, &w) < E_OK ) {
			continue;
		}
		/* the frame measured: the work area NT_W by the panel's height */
		ow = ( w.outer.right - w.outer.left ) + NT_W - ( w.work.right - w.work.left );
		oh = ( w.outer.bottom - w.outer.top ) + n->h - ( w.work.bottom - w.work.top );
		o.left = sw - NT_EDGE - ow;
		o.top = top;
		o.right = o.left + ow;
		o.bottom = o.top + oh;
		if ( o.left != w.outer.left || o.top != w.outer.top || ow != w.outer.right - w.outer.left
		  || oh != w.outer.bottom - w.outer.top ) {
			(void)wm_move(n->d->wid, &o);
		}
		top = o.bottom + NT_EDGE;
	}
	wm_update();
}

/* What came of a notice, told to its process (once) */
LOCAL void answer( NOTE *n, UINT how, UINT which )
{
	T_DTANS	an;

	if ( n->answered ) {
		return;
	}
	n->answered = TRUE;
	knl_memset(&an, 0, sizeof(an));
	an.seq = n->seq;
	an.er = E_OK;
	an.how = how;
	an.flags = which;
	dt_answer(&n->reply, &an, (SZ)sizeof(an));
	tm_printf((UB *)"notice %d: %s%s\n", n->id,
		  ( how == DT_HOW_NOTE_CLICK ) ? "pressed " : ( how == DT_HOW_NOTE_CLOSED ) ? "closed" : "gone",
		  ( how == DT_HOW_NOTE_CLICK ) ? ( which == 0 ? "開く" : (CONST char *)n->act[which - 1] ) : "");
}

LOCAL NOTE *note_of( DTWIN *d )
{
	return ( d != NULL ) ? (NOTE *)d->self : NULL;
}

LOCAL void nt_draw( DTWIN *d )
{
	NOTE	*n = note_of(d);

	if ( n == NULL ) {
		return;
	}
	dt_own_ground(d);
	if ( n->pid >= 0 ) {
		wm_panel_draw(n->pid);
	}
	wm_update();
}

LOCAL BOOL nt_input( DTWIN *d, CONST T_WMEV *ev )
{
	NOTE	*n = note_of(d);
	UINT	ans = WM_ANS_NONE;

	if ( n == NULL || n->pid < 0 ) {
		return FALSE;
	}
	if ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) {
		return FALSE;
	}
	if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return TRUE;			/* the second button: the desktop's menu */
	}
	if ( wm_panel_event(n->pid, ev, &ans) < E_OK ) {
		return TRUE;
	}
	if ( ans == ANS_OPEN || ans == ANS_ACT1 || ans == ANS_ACT2 ) {
		answer(n, DT_HOW_NOTE_CLICK, ( ans == ANS_OPEN ) ? 0 : ( ans == ANS_ACT1 ) ? 1 : 2);
		dt_close(d);
		return TRUE;
	}
	if ( ans == ANS_CLOSE || ans == WM_ANS_CANCEL ) {
		answer(n, DT_HOW_NOTE_CLOSED, 0);
		dt_close(d);
		return TRUE;
	}
	if ( ev->type != HID_EV_MOVE ) {
		wm_panel_draw(n->pid);
		wm_update();
	}
	return TRUE;
}

LOCAL void nt_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	NOTE	*n = note_of(d);

	if ( n != NULL && mn_is(sel, "note.open") ) {
		answer(n, DT_HOW_NOTE_CLICK, 0);
		dt_close(d);
	}
}

/* A notice not asked to stay goes by itself after DT_NOTE_MS */
LOCAL void nt_poll( DTWIN *d )
{
	NOTE	*n = note_of(d);

	if ( n != NULL && !n->stay && (UW)( now_ms() - n->shown ) >= DT_NOTE_MS ) {
		answer(n, DT_HOW_NOTE_GONE, 0);
		dt_close(d);
	}
}

/* The window going: closed by the person unless something was answered already */
LOCAL void nt_close( DTWIN *d )
{
	NOTE	*n = note_of(d);
	INT	i, k;

	if ( n == NULL ) {
		return;
	}
	answer(n, DT_HOW_NOTE_CLOSED, 0);
	if ( n->pid >= 0 ) {
		(void)wm_panel_close(n->pid);
	}
	for ( i = 0, k = 0; i < NT_MAX; i++ ) {
		if ( notes[i] != n ) {
			notes[k++] = notes[i];
		}
	}
	while ( k < NT_MAX ) {
		notes[k++] = NULL;
	}
	Kfree(n);
	d->self = NULL;
	stack();			/* those under it move up */
}

LOCAL CONST DTOWN	nt_own = {
	SYSDEF_MENU_NOTE,
	nt_draw,
	nt_input,
	NULL,
	NULL,
	nt_menu_do,
	NULL,
	nt_close,
	nt_poll
};

/* The notice of a process's number, shown now */
LOCAL NOTE *find( CONST TS_UUID *reply, INT id )
{
	INT	i;

	for ( i = 0; i < NT_MAX; i++ ) {
		if ( notes[i] != NULL && notes[i]->id == id
		  && ts_uuid_cmp(&notes[i]->reply, reply) == 0 ) {
			return notes[i];
		}
	}
	return NULL;
}

/* What a request says, into a notice */
LOCAL void take( NOTE *n, CONST T_DTREQ *rq )
{
	CONST UB	*t = rq->text, *nl;
	INT		i;

	n->reply = rq->reply;
	n->seq = rq->seq;
	n->id = rq->recno;
	n->stay = (BOOL)( ( rq->flags & DT_F_NOTE_STAY ) != 0 );
	n->answered = FALSE;
	n->shown = now_ms();
	copy(n->src, NT_SRC_MAX, rq->name, (INT)sizeof(rq->name));
	for ( nl = t; *nl != 0 && *nl != '\n' && nl < t + NT_TEXT_MAX; nl++ ) ;
	copy(n->title, WM_LABEL_MAX, t, (INT)( nl - t ));
	wrap(n, ( *nl == '\n' ) ? nl + 1 : nl, NT_W - 24);
	for ( i = 0; i < 2; i++ ) {
		copy(n->act[i], WM_LABEL_MAX, rq->ask[i], (INT)sizeof(rq->ask[i]));
	}
}

/* The band: where it comes from */
LOCAL void band( NOTE *n, UB *out, INT max )
{
	INT	k = 0, i;
	CONST char *tail = ( n->src[0] != 0 ) ? " からの通知" : "通知";

	for ( i = 0; n->src[i] != 0 && k < max - 1; i++ ) out[k++] = n->src[i];
	for ( i = 0; tail[i] != 0 && k < max - 1; i++ ) out[k++] = (UB)tail[i];
	out[k] = 0;
}

/* DT_RQ_NOTIFY (desktop.c): shown, shown again in the place of the old one, or taken back */
EXPORT ER dt_note_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	NOTE		*n = find(&rq->reply, rq->recno);
	DTWIN		*w;
	T_DPRECT	o;
	UB		title[WM_TITLE_MAX];
	INT		sw = 0, sh = 0, i;

	if ( ( rq->flags & DT_F_NOTE_CLOSE ) != 0 ) {
		if ( n == NULL ) {
			return E_NOEXS;
		}
		n->answered = TRUE;		/* the process took it back: nothing to tell it */
		dt_close(n->d);
		stack();
		return E_OK;
	}
	if ( n != NULL ) {
		/* the same number again: what it shows made anew, its time begun again */
		n->answered = TRUE;
		take(n, rq);
		band(n, title, sizeof(title));
		(void)wm_set_title(n->d->wid, (CONST char *)title);
		build(n);
		stack();
		nt_draw(n->d);
		an->how = DT_HOW_NOTE_SHOWN;
		return E_OK;
	}
	if ( notes[NT_MAX - 1] != NULL ) {
		/* no room: the oldest goes by itself */
		answer(notes[0], DT_HOW_NOTE_GONE, 0);
		dt_close(notes[0]->d);
	}
	n = (NOTE *)Kmalloc(sizeof(NOTE));
	if ( n == NULL ) {
		return E_NOMEM;
	}
	knl_memset(n, 0, sizeof(*n));
	n->pid = -1;
	take(n, rq);
	band(n, title, sizeof(title));
	wm_screen_main(&sw, &sh);
	o.left = sw - NT_EDGE - NT_W - 40;
	o.top = NT_TOP;
	o.right = o.left + NT_W + 40;
	o.bottom = o.top + 120;
	w = dt_own_open(&nt_own, n, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_NOFOCUS, (CONST char *)title, NULL);
	if ( w == NULL ) {
		Kfree(n);
		return E_LIMIT;
	}
	n->d = w;
	for ( i = 0; i < NT_MAX && notes[i] != NULL; i++ ) ;
	if ( i < NT_MAX ) {
		notes[i] = n;
	}
	build(n);
	stack();
	nt_draw(w);
	tm_printf((UB *)"notice %d shown: %s\n", n->id, n->title);
	an->how = DT_HOW_NOTE_SHOWN;
	return E_OK;
}

/* The window of the i-th notice from the top, 0 when there is none (the tests) */
EXPORT INT dt_note_wid( INT i )
{
	return ( i >= 0 && i < NT_MAX && notes[i] != NULL && notes[i]->d != NULL ) ? notes[i]->d->wid : 0;
}
