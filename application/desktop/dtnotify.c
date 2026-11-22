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
#include <ts/kinsoku.h>
#include <ts/dtreq.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include "desktop.h"

#define NT_W		340		/* the work area's width */
#define NT_LINE_H	20
#define NT_FH		16		/* the letters of the labels (PD_FH) */
#define NT_EDGE		12		/* from the screen's right and between notices */
#define NT_TOP		40		/* the first one's top */
#define NT_BTN_W	76
#define NT_BTN_H	26

#define ANS_OPEN	( WM_ANS_ACT + 1 )
#define ANS_ACT1	( WM_ANS_ACT + 2 )
#define ANS_ACT2	( WM_ANS_ACT + 3 )
#define ANS_CLOSE	( WM_ANS_ACT + 4 )

typedef struct {
	TS_UUID	reply;			/* where what comes of it is answered */
	UINT	seq;
	INT	id;			/* the process's number for it */
	SZ	anssz;			/* the size of the answers, the form it asked in */
	/* what it says, as long as it is (Kmalloc, "" none): the panel shows what it holds */
	UB	*src;
	UB	*title;
	UB	**line;			/* the lines laid out as they are shown, each whole (Kmalloc) */
	INT	nline, lroom;
	UB	*act[2];
	BOOL	stay;
	BOOL	answered;		/* what came of it was answered, or nothing is to be */
	UW	shown;			/* when it was shown, ms */
	INT	pid;			/* its panel */
	INT	h;			/* its work area's height */
	DTWIN	*d;
} NOTE;

LOCAL CONST DTOWN	nt_own;
LOCAL NOTE		**notes;		/* top to bottom, a table that grows */
LOCAL INT		nnote, noteroom;

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

/* The bytes of the UTF-8 letter that starts at s */
LOCAL INT letter_len( CONST UB *s )
{
	if ( s[0] < 0x80 ) return 1;
	if ( ( s[0] & 0xE0 ) == 0xC0 ) return 2;
	if ( ( s[0] & 0xF0 ) == 0xE0 ) return 3;
	return 4;
}

/* A line kept whole, the table of lines grown to take it: FALSE for no memory */
LOCAL BOOL line_add( NOTE *n, CONST UB *b, INT len )
{
	UB	*l;

	if ( n->nline >= n->lroom ) {
		INT	room = ( n->lroom > 0 ) ? n->lroom * 2 : 16;
		UB	**t = (UB **)Kmalloc(sizeof(UB *) * (SZ)room);

		if ( t == NULL ) {
			return FALSE;
		}
		if ( n->line != NULL ) {
			knl_memcpy(t, n->line, sizeof(UB *) * (SZ)n->nline);
			Kfree(n->line);
		}
		n->line = t;
		n->lroom = room;
	}
	l = (UB *)Kmalloc((SZ)len + 1);
	if ( l == NULL ) {
		return FALSE;
	}
	knl_memcpy(l, b, (SZ)len);
	l[len] = 0;
	n->line[n->nline++] = l;
	return TRUE;
}

LOCAL void lines_free( NOTE *n )
{
	INT	i;

	for ( i = 0; i < n->nline; i++ ) Kfree(n->line[i]);
	if ( n->line != NULL ) Kfree(n->line);
	n->line = NULL;
	n->nline = n->lroom = 0;
}

/*
 * What it says broken into lines as wide as the panel holds, at its line
 * feeds and where a line is full, as 禁則 lets it (kin_break). Every line
 * is kept: the panel shows what fits and scrolls the rest. E_NOMEM when
 * the lines cannot be kept.
 */
LOCAL ER wrap( NOTE *n, CONST UB *s, INT width )
{
	ID	fid = fn_system();
	UB	*b = (UB *)Kmalloc((SZ)knl_strlen((CONST char *)s) + 2);
	INT	k = 0;

	if ( b == NULL ) {
		return E_NOMEM;
	}
	if ( fid > 0 ) {
		(void)fn_set_size(fid, NT_FH);
	}
	lines_free(n);
	while ( *s != 0 ) {
		INT	l = ( *s == '\n' ) ? 1 : letter_len(s), i;
		BOOL	full;

		if ( *s == '\n' ) {
			if ( !line_add(n, b, k) ) goto nomem;
			k = 0;
			s++;
			continue;
		}
		for ( i = 0; i < l; i++ ) b[k + i] = s[i];
		b[k + l] = 0;
		full = ( fid > 0 && fn_width(fid, b) > width );
		if ( full && k > 0 ) {
			/* broken where 禁則 lets it: the letters after the break start the next line */
			INT	brk = kin_break(s - k, k);

			if ( !line_add(n, b, brk) ) goto nomem;
			s -= k - brk;
			while ( *s == ' ' ) s++;
			k = 0;
			continue;
		}
		k += l;
		s += l;
	}
	if ( k > 0 && !line_add(n, b, k) ) goto nomem;
	Kfree(b);
	return E_OK;
nomem:
	Kfree(b);
	lines_free(n);
	return E_NOMEM;
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
	INT		y, x, i, rows;
	BOOL		scroll;
	T_DPRECT	m;

	if ( n->pid >= 0 ) {
		(void)wm_panel_close(n->pid);
		n->pid = -1;
	}
	/* as many lines as fit in half the screen; more are in a list that scrolls */
	wm_screen_main_rect(&m);
	rows = ( ( m.bottom - m.top ) / 2 - ( 8 + 24 + 10 + NT_BTN_H + 8 ) ) / NT_LINE_H;
	if ( rows < 3 ) rows = 3;
	scroll = (BOOL)( n->nline > rows );
	if ( !scroll ) rows = n->nline;
	n->h = 8 + 24 + NT_LINE_H * rows + ( scroll ? 4 : 0 ) + 10 + NT_BTN_H + 8;
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
	if ( scroll ) {
		T_WMPART	*ls = dt_pn_add(def, WM_PT_LIST, 3, 12, y, NT_W - 24, NT_LINE_H * rows + 4, NULL);
		INT		cnt = 0;

		for ( i = 0; ls != NULL && i < n->nline; i++ ) {
			if ( wm_part_name(ls, n->line[i]) < 0 ) break;
			cnt++;
		}
		if ( ls != NULL ) {
			ls->count = cnt;
			ls->top = 1;
		}
		y += NT_LINE_H * rows + 4;
	} else {
		for ( i = 0; i < n->nline; i++, y += NT_LINE_H ) {
			(void)dt_pn_add(def, WM_PT_LABEL, 0, 12, y, NT_W - 24, NT_LINE_H, (CONST char *)n->line[i]);
		}
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
	wm_panel_free(def);
}

/* The notices stood one under another from the screen's top right */
LOCAL void stack( void )
{
	INT		top, i, col = 0, first;
	T_WMWIN		w;
	T_DPRECT	m;

	wm_screen_main_rect(&m);	/* the main screen's top right, wherever it is in the view */
	first = top = m.top + NT_TOP;
	for ( i = 0; i < nnote; i++ ) {
		NOTE		*n = notes[i];
		T_DPRECT	o;
		INT		ow, oh;

		if ( n == NULL || n->d == NULL || wm_ref(n->d->wid, &w) < E_OK ) {
			continue;
		}
		/* the frame measured: the work area NT_W by the panel's height */
		ow = ( w.outer.right - w.outer.left ) + NT_W - ( w.work.right - w.work.left );
		oh = ( w.outer.bottom - w.outer.top ) + n->h - ( w.work.bottom - w.work.top );
		if ( top > first && top + oh > m.bottom - NT_EDGE ) {
			/* the column is full: the next notices stand in a column to its left */
			col += ow + NT_EDGE;
			top = first;
		}
		o.left = m.right - NT_EDGE - ow - col;
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
	dt_answer(&n->reply, &an, n->anssz);
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
LOCAL void words_free( NOTE *n );

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
	for ( i = 0, k = 0; i < nnote; i++ ) {
		if ( notes[i] != n ) {
			notes[k++] = notes[i];
		}
	}
	nnote = k;
	words_free(n);
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
	nt_poll,
	NULL
};

/* The notice of a process's number, shown now */
LOCAL NOTE *find( CONST TS_UUID *reply, INT id )
{
	INT	i;

	for ( i = 0; i < nnote; i++ ) {
		if ( notes[i] != NULL && notes[i]->id == id
		  && ts_uuid_cmp(&notes[i]->reply, reply) == 0 ) {
			return notes[i];
		}
	}
	return NULL;
}

/* A word of a notice kept whole (Kmalloc); NULL no memory */
LOCAL UB *word_dup( CONST UB *s, SZ len )
{
	UB	*w = (UB *)Kmalloc((size_t)( len + 1 ));

	if ( w != NULL ) {
		knl_memcpy(w, s, len);
		w[len] = 0;
	}
	return w;
}

LOCAL void words_free( NOTE *n )
{
	INT	i;

	if ( n->src != NULL ) Kfree(n->src);
	if ( n->title != NULL ) Kfree(n->title);
	for ( i = 0; i < 2; i++ ) {
		if ( n->act[i] != NULL ) Kfree(n->act[i]);
		n->act[i] = NULL;
	}
	n->src = n->title = NULL;
	lines_free(n);
}

/* What a request says, into a notice: E_NOMEM when its words cannot be kept */
LOCAL ER take( NOTE *n, CONST T_DTREQ *rq )
{
	SZ		tl, sl, a0, a1;
	CONST UB	*t = dt_rq_word(rq, DT_TAIL_TEXT, &tl), *nl;
	CONST UB	*src = dt_rq_word(rq, DT_TAIL_NAME, &sl);
	CONST UB	*ask1 = dt_rq_word(rq, DT_TAIL_ASK1, &a0), *ask2 = dt_rq_word(rq, DT_TAIL_ASK2, &a1);

	words_free(n);
	n->reply = rq->reply;
	n->seq = rq->seq;
	n->id = rq->recno;
	n->anssz = dt_rq_anssz();
	n->stay = (BOOL)( ( rq->flags & DT_F_NOTE_STAY ) != 0 );
	n->answered = FALSE;
	n->shown = now_ms();
	for ( nl = t; *nl != 0 && *nl != '\n'; nl++ ) ;
	n->src = word_dup(src, sl);
	n->title = word_dup(t, (SZ)( nl - t ));
	n->act[0] = word_dup(ask1, a0);
	n->act[1] = word_dup(ask2, a1);
	if ( n->src == NULL || n->title == NULL || n->act[0] == NULL || n->act[1] == NULL ) {
		words_free(n);
		return E_NOMEM;
	}
	if ( wrap(n, ( *nl == '\n' ) ? nl + 1 : nl, NT_W - 24) < E_OK ) {
		words_free(n);
		return E_NOMEM;
	}
	return E_OK;
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
	T_DPRECT	o, m;
	UB		title[WM_TITLE_MAX];

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
		ER	er;

		n->answered = TRUE;
		if ( ( er = take(n, rq) ) < E_OK ) {
			dt_close(n->d);
			stack();
			return er;
		}
		band(n, title, sizeof(title));
		(void)wm_set_title(n->d->wid, (CONST char *)title);
		build(n);
		stack();
		nt_draw(n->d);
		an->how = DT_HOW_NOTE_SHOWN;
		return E_OK;
	}
	if ( nnote >= noteroom ) {
		/* the table grows; none is put away for want of a place */
		INT	room = ( noteroom > 0 ) ? noteroom * 2 : 8;
		NOTE	**t = (NOTE **)Kmalloc(sizeof(NOTE *) * (SZ)room);

		if ( t == NULL ) {
			return E_NOMEM;
		}
		if ( notes != NULL ) {
			knl_memcpy(t, notes, sizeof(NOTE *) * (SZ)nnote);
			Kfree(notes);
		}
		notes = t;
		noteroom = room;
	}
	n = (NOTE *)Kmalloc(sizeof(NOTE));
	if ( n == NULL ) {
		return E_NOMEM;
	}
	knl_memset(n, 0, sizeof(*n));
	n->pid = -1;
	if ( take(n, rq) < E_OK ) {
		Kfree(n);
		return E_NOMEM;
	}
	band(n, title, sizeof(title));
	wm_screen_main_rect(&m);
	o.left = m.right - NT_EDGE - NT_W - 40;
	o.top = m.top + NT_TOP;
	o.right = o.left + NT_W + 40;
	o.bottom = o.top + 120;
	w = dt_own_open(&nt_own, n, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_NOFOCUS, (CONST char *)title, NULL);
	if ( w == NULL ) {
		words_free(n);
		Kfree(n);
		return E_LIMIT;
	}
	n->d = w;
	notes[nnote++] = n;
	build(n);
	stack();
	nt_draw(w);
	tm_printf((UB *)"notice %d shown: %s\n", n->id, n->title);
	an->how = DT_HOW_NOTE_SHOWN;
	return E_OK;
}

/*
 * A notice of the desktop's own: no process to answer, so that its
 * buttons only put it away. name is where it comes from, text its title,
 * a line feed and what it says, as DT_RQ_NOTIFY's text; id its number
 * among the desktop's own (one shown again under it takes its place).
 */
EXPORT ER dt_note_own( INT id, CONST char *name, CONST char *text )
{
	T_DTREQ	*r;
	T_DTANS	an;
	UINT	tailsz = 0;
	SZ	room;
	ER	er;

	if ( text == NULL ) {
		return E_PAR;
	}
	room = (SZ)knl_strlen(text) + ( ( name != NULL ) ? (SZ)knl_strlen(name) : 0 ) + 256;
	r = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ) + (size_t)room);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	knl_memset(r, 0, sizeof(*r));
	knl_memset(&an, 0, sizeof(an));
	r->req = DT_RQ_NOTIFY;
	r->recno = id;
	r->flags = DT_F_NOTE_STAY;
	(void)dt_tail_put((UB *)( r + 1 ), &tailsz, room, DT_TAIL_TEXT, (CONST UB *)text, -1);
	(void)dt_tail_put((UB *)( r + 1 ), &tailsz, room, DT_TAIL_NAME,
			  (CONST UB *)( ( name != NULL ) ? name : "" ), -1);
	(void)dt_tail_put((UB *)( r + 1 ), &tailsz, room, DT_TAIL_ASK1, (CONST UB *)"", -1);
	(void)dt_tail_put((UB *)( r + 1 ), &tailsz, room, DT_TAIL_ASK2, (CONST UB *)"", -1);
	r->tailsz = tailsz;
	er = dt_note_req(r, &an);
	Kfree(r);
	return er;
}

/* The window of the i-th notice from the top, 0 when there is none (the tests) */
EXPORT INT dt_note_wid( INT i )
{
	return ( i >= 0 && i < nnote && notes[i] != NULL && notes[i]->d != NULL ) ? notes[i]->d->wid : 0;
}

#if USE_KTEST
/*
 * For a test: n notices of nlines lines each asked to stay, more than the
 * eight that fitted before: answers how many are shown, *p_lines how many
 * lines the first keeps. They are taken back again.
 */
EXPORT INT dt_notes_test( INT n, INT nlines, INT *p_lines )
{
	INT	i, k, shown = 0;
	UB	*text, *rq;
	SZ	tl = 8 + (SZ)nlines * 40 + 1;
	UINT	tailsz;

	text = (UB *)Kmalloc(tl);
	rq = (UB *)Kmalloc(sizeof(T_DTREQ) + 4096 + tl);
	*p_lines = 0;
	if ( text == NULL || rq == NULL ) {
		if ( text != NULL ) Kfree(text);
		if ( rq != NULL ) Kfree(rq);
		return -1;
	}
	{
		SZ	at = 0;

		knl_memcpy(text + at, "title\n", 6);
		at += 6;
		for ( k = 0; k < nlines; k++ ) {
			knl_memcpy(text + at, "a line of the notice", 20);
			at += 20;
			text[at++] = ( k + 1 < nlines ) ? '\n' : 0;
		}
		if ( nlines == 0 ) text[at] = 0;
	}
	for ( i = 0; i < n + 0; i++ ) {
		T_DTREQ	*r = (T_DTREQ *)rq;
		T_DTANS	an;

		knl_memset(r, 0, sizeof(*r));
		knl_memset(&an, 0, sizeof(an));
		r->req = DT_RQ_NOTIFY;
		r->recno = 9000 + i;
		r->flags = DT_F_NOTE_STAY;
		r->reply.b[0] = 0xDD;
		tailsz = 0;
		(void)dt_tail_put((UB *)( r + 1 ), &tailsz, 4096 + tl, DT_TAIL_TEXT, text, -1);
		(void)dt_tail_put((UB *)( r + 1 ), &tailsz, 4096 + tl, DT_TAIL_NAME, (CONST UB *)"test", -1);
		(void)dt_tail_put((UB *)( r + 1 ), &tailsz, 4096 + tl, DT_TAIL_ASK1, (CONST UB *)"", -1);
		(void)dt_tail_put((UB *)( r + 1 ), &tailsz, 4096 + tl, DT_TAIL_ASK2, (CONST UB *)"", -1);
		r->tailsz = tailsz;
		if ( dt_note_req(r, &an) >= E_OK ) shown++;
	}
	for ( i = 0; i < nnote; i++ ) {
		if ( notes[i]->id >= 9000 && notes[i]->id < 9000 + n ) {
			if ( *p_lines == 0 ) *p_lines = notes[i]->nline;
		}
	}
	for ( i = 0; i < n; i++ ) {
		T_DTREQ	*r = (T_DTREQ *)rq;
		T_DTANS	an;

		knl_memset(r, 0, sizeof(*r));
		knl_memset(&an, 0, sizeof(an));
		r->req = DT_RQ_NOTIFY;
		r->recno = 9000 + i;
		r->flags = DT_F_NOTE_CLOSE;
		r->reply.b[0] = 0xDD;
		(void)dt_note_req(r, &an);
	}
	Kfree(text);
	Kfree(rq);
	return shown;
}
#endif
