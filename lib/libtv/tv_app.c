/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_app.c
 *	What an editor running as a process shares with the others
 *	(include/ts/tvapp.h, design 11.3)
 *
 *	The program is started with the link it was opened from (T_PRCOPEN,
 *	"openArg": "link"). Its record is held in its memory and written back
 *	whole, in one change: on a store that does not count the links of a
 *	record itself (OB_F_AUTOREF clear), the objects the record links to
 *	more or less often than before have their counts moved to match when
 *	it is written. Opened from a link of a window of the desktop's,
 *	新たな実身に保存 puts a link to the copy with that one, and when the
 *	window closes, the way it was shown is written back into it
 *	(DT_RQ_SAVE_NEW, DT_RQ_LINK_VIEW), as the desktop's own windows do;
 *	what a window opened from one of the record's own links did to that
 *	link is told back on a channel of its own (DT_F_LINK). Where the
 *	window stood and how large it was go into the object's metadata
 *	("window"), for it to open there next time.
 *
 *	The converter of かな漢字: the keys go to a session of the system's
 *	converter (kc_hid_key) in the input mode each came in, the notice's
 *	imode. What it commits goes into the text as typing does; what it is
 *	still working on is drawn over the text at the caret, each clause
 *	underlined and the one being converted turned over. While candidates
 *	are chosen the system lists them under the composition (OB_WR_CAND);
 *	a digit, or a press on a name there, takes one. In 英語 a letter goes
 *	in in the width and case the mode says. One composition at a time; it
 *	is committed when the hand presses, the mode becomes 英語 or the
 *	editing ends.
 */

#include <ts/tvapp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/look.h>
#include <ts/wm.h>
#include <ts/tray.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/tvsrc.h>
#include <ts/tadlnk.h>
#include <ts/time.h>
#include <ts/kconv.h>
#include <ts/fn.h>
#include <ts/uiprn.h>

#define WATCH_MAX	64		/* the objects the record links to, watched for changes */
#define APP_MAX		32		/* programs offered in 実行 */
#define APP_ID		64
#define APP_NAME	WM_LABEL_MAX

CONST T_TAHOST	*ta_host;

TS_UUID		g_obj;
INT		g_recno;
BOOL		g_has_parent;
TS_UUID		g_parent, g_vobjid;
BOOL		g_from_link;
T_PRCOPEN	g_start_arg;
UB		g_name[OB_NAME_MAX];
TS_UUID		g_win;
ID		g_kw;
INT		g_gid;
INT		g_w, g_h;
UI		g_ui;
ID		g_port;
T_TAD		*g_rec;
UW		g_paper = TV_PAPER_LOOK;
BOOL		g_full;
BOOL		g_changed;
BOOL		g_dirty;
BOOL		g_closing;
UD		g_input_at;

static TS_UUID	g_ch;
static TS_UUID	g_lch;			/* where the desktop says what windows opened from its links did to them */
static ID	g_lport;
static BOOL	g_autoref;		/* its store counts the links of its records itself */
static T_OBWPOS	g_restore;
static TS_UUID	*g_was;			/* what the record linked to as it was last read or written */
static INT	g_nwas;
static TS_UUID	g_watched[WATCH_MAX];
static INT	g_nwatched;

/* the programs 実行 offers for a link */
static UB	g_app_id[APP_MAX][APP_ID];
static UB	g_app_name[APP_MAX][APP_NAME];
static BOOL	g_app_kind[APP_MAX];
static INT	g_napp;

/* ---------------------------------------------------------------- saying things */

void note( CONST char *s )
{
	char	line[TAD_NAME_MAX + 160];

	snprintf(line, sizeof(line), "%s: %s\n", ( ta_host != NULL ) ? ta_host->who : "app", s);
	tm_putstring((CONST UB *)line);
}

void say( CONST char *s )
{
	(void)wm_msg_put(s);
}

UD ta_now( void )
{
	UD	t = 0;

	(void)ts_get_mono(&t);
	return t;
}

/*
 * How deep the main task's stack has been used. The system gives the
 * stack in pages of noughts, and the main task starts within the first
 * page under its top: the top is the page boundary above where
 * ta_stack_start was called from, and the deepest use is where the
 * first word that is not nought lies, seen from the far end. Only
 * TA_STACK_SEEN under the top is looked at, inside the stack the
 * system gives (TA_STACK_SIZE) whatever the start took of its first page.
 */
static UBINT	ta_stk_top;

void ta_stack_start( void )
{
	volatile UB	here = 0;

	ta_stk_top = ( (UBINT)&here + 4095 ) & ~(UBINT)4095;
}

INT ta_stack_used( void )
{
	volatile CONST UD	*p, *e;

	if ( ta_stk_top == 0 ) return 0;
	p = (volatile CONST UD *)( ta_stk_top - TA_STACK_SEEN );
	e = (volatile CONST UD *)ta_stk_top;
	while ( p < e && *p == 0 ) p++;
	return (INT)( (CONST volatile UB *)e - (CONST volatile UB *)p );
}

/* ---------------------------------------------------------------- the desktop */

/* A request to the desktop, its panels over this window; the answer waited for */
ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0;
	ID		k, ka;
	ER		er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	{
		static CONST TS_UUID	none;

		/* asked over the program's window, unless the asker named another of the program's */
		if ( memcmp(&rq->win, &none, sizeof(none)) == 0 ) rq->win = g_win;
	}
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* A link in the record opened as a double press opens it */
void link_open( CONST T_VOBJ *v )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[TAD_NAME_MAX + 32];
	ER	er;

	char	po[40], pv[40], pc[40];
	INT	n = 0;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = v->target;
	rq.recno = -1;
	/* what the window opened does to the link comes back here, to be put in the record (DT_F_LINK) */
	if ( g_lport > 0 ) {
		ts_uuid_text(&g_obj, po);
		ts_uuid_text(&v->vobjid, pv);
		ts_uuid_text(&g_lch, pc);
		n = snprintf((char *)rq.text, sizeof(rq.text), "%s\t%s\t%s\n", po, pv, pc);
		rq.flags |= DT_F_LINK;
		/* shown as the link says */
		rq.look.frcol = v->frcol;
		rq.look.chcol = v->chcol;
		rq.look.tbcol = v->tbcol;
		rq.look.bgcol = v->bgcol;
		rq.grab_x = v->wordwrap;
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (CONST char *)v->viewmode);
	}
	if ( v->applist[0] != 0 && (size_t)n + strlen((CONST char *)v->applist) < sizeof(rq.text) - 1 ) {
		snprintf((char *)rq.text + n, sizeof(rq.text) - (size_t)n, "%s", (CONST char *)v->applist);
		rq.flags |= DT_F_APPLIST;
	}
	er = ask_desktop(&rq, &an, 3000);
	snprintf(s, sizeof(s), "opened %s (%d)", (CONST char *)v->name, (INT)er);
	note(s);
	if ( er < E_OK ) say("開けませんでした");
}

/* ---------------------------------------------------------------- the record */

/* What a record links to, every link counted */
static INT targets_of( CONST T_TAD *rec, TS_UUID **p_out )
{
	T_VOBJ	v;
	INT	n = tad_lnk_count(rec), i, k = 0;
	TS_UUID	*t;

	*p_out = NULL;
	if ( n <= 0 ) return 0;
	t = malloc(sizeof(TS_UUID) * (size_t)n);
	if ( t == NULL ) return -1;
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(rec, i, &v) >= E_OK ) t[k++] = v.target;
	}
	*p_out = t;
	return k;
}

/*
 * The counts of the objects moved as the links changed: what the two
 * have in common cancels, one link against one; what is left of the
 * new is counted once more, what is left of the old once less
 */
static void relink( TS_UUID *was, INT nwas, TS_UUID *now, INT nnow )
{
	INT	i, k;

	for ( i = 0; i < nwas; i++ ) {
		for ( k = 0; k < nnow; k++ ) {
			if ( memcmp(&was[i], &now[k], sizeof(TS_UUID)) == 0 ) {
				now[k] = now[--nnow];
				was[i] = was[--nwas];
				i--;
				break;
			}
		}
	}
	for ( k = 0; k < nnow; k++ ) (void)ob_lnk_obj(&now[k]);
	for ( i = 0; i < nwas; i++ ) (void)ob_unl_obj(&was[i]);
}

static ER vol_of( CONST TS_UUID *obj, TS_UUID *p_vol, void *arg )
{
	(void)arg;
	return ob_ref_vid(obj, p_vol, NULL);
}

/*
 * Each link made to say on which volume its object is, as seen from the
 * volume of this object (design 8.3), as the desktop does when it saves
 */
static void links_fix_doc( void )
{
	TS_UUID	hv, mine;
	INT	failed = 0;
	char	s[64];

	if ( ob_ref_vid(&g_obj, &hv, &mine) < E_OK ) return;
	(void)tad_lnk_vol_doc(g_rec, &hv, &mine, vol_of, NULL, &failed);
	if ( failed > 0 ) {
		snprintf(s, sizeof(s), "%d links were not given their volume", failed);
		note(s);
	}
}

ER ta_read( UB **p_x, SZ *p_len )
{
	*p_len = 0;
	*p_x = tv_src_read(&g_obj, g_recno, p_len);
	return ( *p_x != NULL ) ? E_OK : E_NOEXS;
}

void ta_was( void )
{
	free(g_was);
	g_was = NULL;
	g_nwas = ( g_rec != NULL ) ? targets_of(g_rec, &g_was) : 0;
	if ( g_nwas < 0 ) g_nwas = 0;
}

/* 元の実身に保存: the record written back whole, in one change */
ER ta_save( void )
{
	TS_UUID	*now = NULL, *tmp = NULL;
	INT	nnow = 0;
	UB	*buf;
	SZ	size = 0;
	ID	k;
	ER	er;
	char	s[48];

	if ( g_rec == NULL ) return E_OK;
	links_fix_doc();
	(void)tad_write_mem(g_rec, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return E_NOMEM;
	er = tad_write_mem(g_rec, buf, size + 1, &size);
	if ( er >= E_OK && !g_autoref ) {
		nnow = targets_of(g_rec, &now);
		if ( nnow < 0 ) er = E_NOMEM;
	}
	if ( er >= E_OK ) {
		k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_W);
		er = ( k > 0 ) ? ob_rpl_rec(k, g_recno, buf, size) : (ER)k;
		if ( k > 0 ) ob_cls_obj(k);
	}
	free(buf);
	if ( er >= E_OK && !g_autoref ) {
		/* the counts moved against copies: the list kept is what it links to now */
		tmp = ( nnow > 0 ) ? malloc(sizeof(TS_UUID) * (size_t)nnow) : NULL;
		if ( tmp != NULL ) memcpy(tmp, now, sizeof(TS_UUID) * (size_t)nnow);
		relink(g_was, g_nwas, ( tmp != NULL ) ? tmp : now, nnow);
		free(tmp);
		free(g_was);
		g_was = now;
		g_nwas = nnow;
		now = NULL;
	}
	free(now);
	snprintf(s, sizeof(s), "saved (%d) %d bytes", (INT)er, (INT)size);
	note(s);
	if ( er >= E_OK ) {
		g_changed = FALSE;
		say("保存しました");
	} else {
		say("保存できませんでした");
	}
	return er;
}

/*
 * Record 0 of another object written whole from rec, as the desktop
 * saves one (om_store_save): each link given its volume as seen from
 * that object, the record replaced in one change, and on a store that
 * does not count the links of its records itself, the objects counted
 * by what changed against the record it had.
 */
ER ta_rec_put( CONST TS_UUID *id, T_TAD *rec )
{
	TS_UUID	hv, mine, *was = NULL, *now = NULL;
	INT	nwas = 0, nnow = 0, failed = 0;
	T_OBREF	ref;
	T_TAD	*old = NULL;
	UB	*buf, *ob = NULL;
	SZ	size = 0;
	INT	olen;
	ID	k;
	ER	er;

	if ( ob_ref_vid(id, &hv, &mine) >= E_OK ) {
		(void)tad_lnk_vol_doc(rec, &hv, &mine, vol_of, NULL, &failed);
	}
	er = ob_ref_obj(id, &ref);
	if ( er < E_OK ) return er;
	(void)tad_write_mem(rec, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return E_NOMEM;
	er = tad_write_mem(rec, buf, size + 1, &size);
	if ( er >= E_OK && ( ref.flags & OB_F_AUTOREF ) == 0 ) {
		olen = ui_rec_all(id, 0, 0, &ob);
		if ( olen > 0 && tad_parse(ob, (SZ)olen, NULL, &old) >= E_OK && old != NULL ) {
			nwas = targets_of(old, &was);
		}
		if ( old != NULL ) tad_free(old);
		free(ob);
		nnow = targets_of(rec, &now);
		if ( nwas < 0 || nnow < 0 ) er = E_NOMEM;
	}
	if ( er >= E_OK ) {
		k = ob_opn_obj(id, OB_OP_R | OB_OP_W);
		er = ( k > 0 ) ? ob_rpl_rec(k, 0, buf, size) : (ER)k;
		if ( k > 0 ) ob_cls_obj(k);
	}
	if ( er >= E_OK && ( ref.flags & OB_F_AUTOREF ) == 0 ) {
		relink(was, ( nwas > 0 ) ? nwas : 0, now, ( nnow > 0 ) ? nnow : 0);
	}
	free(was);
	free(now);
	free(buf);
	return er;
}

/* Notices of changes to an object, at the window's channel */
void ta_watch( CONST TS_UUID *id )
{
	T_OBNTF	req;
	ID	k;
	INT	i;

	for ( i = 0; i < g_nwatched; i++ ) {
		if ( memcmp(&g_watched[i], id, sizeof(TS_UUID)) == 0 ) return;
	}
	if ( g_nwatched >= WATCH_MAX ) return;
	k = ob_opn_obj(id, OB_OP_READ | OB_OP_ATRRD);	/* told to one who may read its attributes */
	if ( k <= 0 ) return;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 1;
	(void)ob_ntf_evt(k, OB_REC_ANY, &req, g_port);
	/* the request goes with the key: the key is kept until the program ends */
	g_watched[g_nwatched++] = *id;
}

void ta_watch_links( void )
{
	T_VOBJ	v;
	INT	i, n = ( g_rec != NULL ) ? tad_lnk_count(g_rec) : 0;

	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(g_rec, i, &v) >= E_OK ) ta_watch(&v.target);
	}
}

/* The next number of text after a tab, from *p; 0 when there is none */
static INT text_num( CONST UB *text, INT *p )
{
	INT	i = *p, v = 0;
	BOOL	neg = FALSE;

	if ( text[i] != '\t' ) return 0;
	i++;
	if ( text[i] == '-' ) {
		neg = TRUE;
		i++;
	}
	while ( text[i] >= '0' && text[i] <= '9' ) v = v * 10 + ( text[i++] - '0' );
	*p = i;
	return neg ? -v : v;
}

static void remade( void )
{
	if ( ta_host != NULL && ta_host->remade != NULL ) ta_host->remade();
	g_changed = TRUE;
	g_dirty = TRUE;
}

/*
 * What a window opened from a link of this record did to it, told by the
 * desktop (DT_F_LINK): the view it closed with written into the link, or
 * a link to the copy it was saved as put with it. The record changes as
 * a window of the desktop's changes its own: nothing to take back.
 */
static void link_told( CONST T_DTANS *an )
{
	T_VOBJ	v;
	TS_UUID	vid, id;
	INT	at = 2 * TS_UUID_STRLEN + 1, sx, sy, zoom, wrap, i;
	BOOL	changed = FALSE;
	UB	vm[TAD_VIEW_MAX];
	char	s[80];

	/* the record's object, a tab, the link's vobjid */
	if ( g_rec == NULL || ts_str_to_uuid((CONST char *)an->text, &id) < E_OK
	  || memcmp(&id, &g_obj, sizeof(TS_UUID)) != 0 || an->text[TS_UUID_STRLEN] != '\t'
	  || ts_str_to_uuid((CONST char *)an->text + TS_UUID_STRLEN + 1, &vid) < E_OK
	  || tad_lnk_find(g_rec, &vid, &v) < E_OK ) return;
	if ( an->how == DT_HOW_LINK_VIEW ) {
		if ( memcmp(&v.target, &an->into, sizeof(TS_UUID)) != 0 ) return;
		sx = text_num(an->text, &at);
		sy = text_num(an->text, &at);
		zoom = text_num(an->text, &at);
		wrap = text_num(an->text, &at);
		i = 0;
		if ( an->text[at] == '\t' ) {
			for ( at++; an->text[at] != 0 && i < TAD_VIEW_MAX - 1; ) vm[i++] = an->text[at++];
		}
		vm[i] = 0;
		if ( v.scrollx != sx || v.scrolly != sy ) {
			v.scrollx = sx;
			v.scrolly = sy;
			changed = TRUE;
		}
		if ( vm[0] != 0 && strcmp((CONST char *)v.viewmode, (CONST char *)vm) != 0 ) {
			memcpy(v.viewmode, vm, (size_t)i + 1);
			changed = TRUE;
		}
		if ( wrap != TAD_WRAP_NONE && v.wordwrap != wrap ) {
			v.wordwrap = wrap;
			changed = TRUE;
		}
		if ( zoom > 0 && v.zoom != zoom ) {
			v.zoom = zoom;
			changed = TRUE;
		}
		if ( changed && tad_lnk_set(g_rec, &v) >= E_OK ) remade();
		snprintf(s, sizeof(s), "link view written back: %d,%d zoom %d%s", sx, sy, zoom, changed ? "" : " (as it was)");
		note(s);
	} else if ( an->how == DT_HOW_LINK_ADD ) {
		v.target = an->into;
		memset(&v.vobjid, 0, sizeof(v.vobjid));
		v.name[0] = 0;
		v.dlen = 0;			/* a new object has no data appended to it yet */
		if ( ta_host != NULL && ta_host->picks_none != NULL ) ta_host->picks_none();
		if ( tad_lnk_add(g_rec, &v, &id) >= E_OK ) {
			tv_src_forget(&an->into);
			ta_watch(&an->into);
			remade();
			note("a link to the copy saved put with its link");
		}
	}
}

void ta_links_told( void )
{
	T_DTANS	an;
	SZ	asz = 0;

	while ( g_lport > 0 && ob_rea_tmo(g_lport, 0, 0, &an, sizeof(an), &asz, TMO_POL) >= E_OK ) {
		if ( asz == (SZ)sizeof(an) ) link_told(&an);
	}
}

BOOL ta_links_event( CONST T_OBNTM *m )
{
	if ( g_lport <= 0 || memcmp(&m->uuid, &g_lch, sizeof(TS_UUID)) != 0 ) return FALSE;
	ta_links_told();
	return TRUE;
}

/*
 * The channel the desktop says on what windows opened from this record's
 * links did to them, and a notice at the window's channel when it does
 */
static void links_channel( void )
{
	T_OBCRE	c;
	T_OBNTF	req;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_lch) < E_OK ) return;
	g_lport = ob_opn_obj(&g_lch, OB_OP_READ | OB_OP_ATRRD | OB_O_NOWAIT);
	if ( g_lport <= 0 ) {
		g_lport = 0;
		(void)ob_del_obj(&g_lch);
		return;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 2;
	(void)ob_ntf_evt(g_lport, OB_REC_ANY, &req, g_port);
}

/*
 * Asked whether links (or, with OB_ASK_LETTERS, letters) let go where the
 * hand is would be taken (OB_E_ASK, before the one carrying them makes
 * their objects or takes them away as a move): answered as a drop there
 * would be, and said on the console
 */
void ta_ask_answer( CONST T_OBNTM *m, BOOL takes )
{
	T_OBDRANS	an;
	SZ		asz = 0;
	char		s[64];

	memset(&an, 0, sizeof(an));
	an.seq = m->code;
	an.answer = takes ? OB_DR_ACCEPT : OB_DR_REFUSE;
	(void)ob_wri_rec(g_kw, OB_WR_ASK, 0, &an, sizeof(an), &asz);
	snprintf(s, sizeof(s), "asked to take %s at %d,%d: %s", ( m->mods & OB_ASK_LETTERS ) ? "letters" : "links",
		 (INT)m->x, (INT)m->y, takes ? "yes" : "no");
	note(s);
}

/* The object's paper, as the store has it */
void ta_paper_read( void )
{
	T_VOBJ	v;
	UW	paper = TV_PAPER_LOOK;

	memset(&v, 0, sizeof(v));
	v.target = g_obj;
	v.disp = TAD_D_DEFAULT;
	if ( tv_src_look(&v, NULL, 0, &paper) < 0 ) paper = TV_PAPER_LOOK;
	g_paper = paper;
}

/* ---------------------------------------------------------------- starting and ending */

ER ta_start( CONST T_TAHOST *host, T_OBREF *p_ref )
{
	T_PRCOPEN	a;
	TS_UUID		none;
	SZ		n;

	ta_host = host;
	memset(&a, 0, sizeof(a));
	memset(&none, 0, sizeof(none));
	n = ts_get_arg(&a, sizeof(a));
	g_obj = a.target;
	if ( n < (SZ)sizeof(TS_UUID) || ob_ref_obj(&g_obj, p_ref) < E_OK ) return E_NOEXS;
	/* the link it was opened from, when the desktop gave it */
	if ( n >= (SZ)sizeof(a) ) {
		g_recno = ( a.recno > 0 ) ? a.recno : 0;
		g_parent = a.parent;
		g_vobjid = a.vobjid;
		g_has_parent = (BOOL)( memcmp(&a.parent, &none, sizeof(none)) != 0 );
		g_from_link = a.fromlink;
		g_start_arg = a;
		g_start_arg.viewmode[DT_VIEW_MAX - 1] = 0;
	}
	g_autoref = (BOOL)( ( p_ref->flags & OB_F_AUTOREF ) != 0 );
	memcpy(g_name, p_ref->name, OB_NAME_MAX);
	g_name[OB_NAME_MAX - 1] = 0;
	tv_src_init();
	return E_OK;
}

ER ta_channel( void )
{
	T_OBCRE	c;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port <= 0 ) return E_NOEXS;
	links_channel();
	return E_OK;
}

void ta_work_size( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		g_w = wp.wright - wp.wleft;
		g_h = wp.wbottom - wp.wtop;
	}
	g_ui.w = g_w;
	g_ui.h = g_h;
}

/* Where the window stood last, as the object's metadata says; else where the desktop puts one */
static void place_of( INT *p_x, INT *p_y, INT *p_w, INT *p_h )
{
	T_JSON	root, w, pos;
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(&g_obj, OB_OP_ATRRD);

	*p_x = 60;
	*p_y = 40;
	*p_w = 900;
	*p_h = 640;
	if ( k <= 0 ) return;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n > 0 && js_parse(m, n, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK
	  && js_get(&w, "pos", &pos) >= E_OK ) {
		INT	x = (INT)js_get_num(&pos, "x", -1), y = (INT)js_get_num(&pos, "y", -1);
		INT	ww = (INT)js_get_num(&w, "width", 0), wh = (INT)js_get_num(&w, "height", 0);

		if ( ww > 0 && wh > 0 ) {
			*p_x = ( x < 0 ) ? 0 : x;
			*p_y = ( y < 0 ) ? 0 : y;
			*p_w = ww;
			*p_h = wh;
		}
	}
	free(m);
}

ER ta_win_open( UW events )
{
	T_OBCRE	c;
	T_OBNTF	req;
	char	json[260], shown[TS_UUID_STRLEN + 1];
	INT	x, y, w, h;

	place_of(&x, &y, &w, &h);
	ts_uuid_text(&g_obj, shown);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 x, y, x + w, y + h,
		 WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR | WM_ATTR_BBAR, ta_host->prog, shown);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = g_name;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) return E_OBJ;
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = events;
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( g_kw > 0 ) ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
		g_kw = 0;
		return E_OBJ;
	}
	g_ui.kw = g_kw;
	g_ui.port = g_port;
	(void)ui_uuid(ta_host->box, &g_ui.box);
	ta_work_size();
	return E_OK;
}

void ta_end( void )
{
	if ( g_kw > 0 ) ob_cls_obj(g_kw);
	g_kw = 0;
	(void)ob_del_obj(&g_win);
}

/* The channels let go of, when nothing more is to come at them */
void ta_channel_close( void )
{
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	if ( g_lport > 0 ) {
		ob_cls_obj(g_lport);
		(void)ob_del_obj(&g_lch);
	}
}

/* ---------------------------------------------------------------- the window */

/* 全画面表示: the window made as large as the desktop makes it, or put back */
void ta_fullscreen( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	SZ	asz = 0;

	if ( g_full ) {
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz);
		g_full = FALSE;
	} else if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz) >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_FULL;
		if ( ask_desktop(&rq, &an, 5000) >= E_OK ) g_full = TRUE;
	}
	ta_work_size();
	g_dirty = TRUE;
}

/* 背景色変更: the object's paper, chosen in the desktop's panel and kept in its metadata */
void ta_paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PAPER;
	rq.target = g_obj;
	rq.look.bgcol = ( g_paper == TV_PAPER_LOOK ) ? TAD_COL_NONE : g_paper;
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
		g_paper = an.look.bgcol & 0x00FFFFFFU;
		tv_src_forget(&g_obj);
	}
	g_dirty = TRUE;
}

/*
 * 印刷: rec handed to the desktop as a page (ts/uiprn.h, DT_RQ_PRINT),
 * on the paper the window shows; flags DT_F_PR_DETAIL and
 * DT_F_PR_HIDDEN as a document is shown. The desktop's panels say how.
 */
ER ta_print( CONST T_TAD *rec, UINT flags )
{
	T_UIPRN	up;
	T_DTREQ	rq;
	T_DTANS	an;
	UB	*x;
	SZ	size = 0;
	ER	er;
	char	s[48];

	(void)tad_write_mem(rec, NULL, 0, &size);
	x = malloc((size_t)size + 1);
	er = ui_prn_open(&up, &g_obj);
	if ( er >= E_OK ) er = ( x != NULL ) ? tad_write_mem(rec, x, size + 1, &size) : E_NOMEM;
	if ( er >= E_OK ) er = ui_prn_page(&up, x, (INT)size);
	if ( er >= E_OK ) er = ui_prn_ready(&up);
	if ( er >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		rq.flags = DT_F_PR_PAPER | ( flags & ( DT_F_PR_DETAIL | DT_F_PR_HIDDEN ) );
		rq.look.bgcol = g_paper;
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_name);
		er = ask_desktop(&rq, &an, 60 * 60 * 1000);
	}
	free(x);
	snprintf(s, sizeof(s), "print (%d)", (INT)er);
	note(s);
	ui_prn_close(&up);
	g_dirty = TRUE;
	return er;
}

/* The link the window was opened from, named for the desktop: the parent's UUID, a tab, the vobjid */
static void link_named( T_DTREQ *rq )
{
	char	p[TS_UUID_STRLEN + 1], v[TS_UUID_STRLEN + 1];

	ts_uuid_text(&g_parent, p);
	ts_uuid_text(&g_vobjid, v);
	snprintf((char *)rq->text, sizeof(rq->text), "%s\t%s", p, v);
	rq->target = g_obj;
	rq->recno = g_recno;
}

/*
 * 新たな実身に保存: the record saved, then copied by the desktop under a
 * name asked for over the window, a link to the copy put with the link
 * the window was opened from. The window goes on showing this object.
 */
void ta_save_new( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[48];

	if ( !g_has_parent ) return;
	if ( g_changed && ta_save() < E_OK ) return;
	memset(&rq, 0, sizeof(rq));
	memset(&an, 0, sizeof(an));
	rq.req = DT_RQ_SAVE_NEW;
	link_named(&rq);
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	snprintf(s, sizeof(s), "saved as new (%d)", (INT)er);
	note(s);
	g_dirty = TRUE;
}

/*
 * Closing, opened from a link: where it was scrolled to, how large it
 * was shown (zoom, in hundredths; 0 not said), whether its lines
 * wrapped (TAD_WRAP_*; TAD_WRAP_NONE not said) and the way it was
 * viewed written back into it
 */
void ta_link_view_back( INT sx, INT sy, INT zoom, INT wrap, CONST char *view )
{
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[64];

	if ( !g_has_parent ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_LINK_VIEW;
	link_named(&rq);
	rq.grab_x = sx;
	rq.grab_y = sy;
	rq.look.w = zoom;
	rq.look.h = wrap;
	snprintf((char *)rq.name, sizeof(rq.name), "%s", ( view != NULL ) ? view : "");
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "link view %d,%d %d%% (%d)%s", sx, sy, zoom, (INT)er,
		 ( wrap == TAD_WRAP_ON ) ? " wrap" : ( wrap == TAD_WRAP_OFF ) ? " nowrap" : "");
	note(s);
}

/*
 * One number of the metadata's "window" (in its "pos" when in_pos)
 * written in place of what is there: a member that is not there is left
 * so, as the desktop leaves it
 */
static BOOL win_num( UB **p_m, INT *p_n, CONST char *key, BOOL in_pos, INT v )
{
	T_JSON	root, w, at;
	UB	*m = *p_m, *nm;
	INT	n = *p_n, a, e, k;
	char	txt[16];

	if ( js_parse(m, n, &root) < E_OK || js_get(&root, "window", &w) < E_OK ) return FALSE;
	if ( in_pos && js_get(&w, "pos", &w) < E_OK ) return FALSE;
	if ( js_get(&w, key, &at) < E_OK ) return FALSE;
	a = (INT)( at.s - m );
	e = a + at.len;
	k = snprintf(txt, sizeof(txt), "%d", v);
	nm = malloc((size_t)( n - at.len + k ) + 1);
	if ( nm == NULL ) return FALSE;
	memcpy(nm, m, (size_t)a);
	memcpy(nm + a, txt, (size_t)k);
	memcpy(nm + a + k, m + e, (size_t)( n - e ));
	n = n - at.len + k;
	nm[n] = 0;
	free(m);
	*p_m = nm;
	*p_n = n;
	return TRUE;
}

/* Where the window stood and how large it was, into the object's metadata, for it to open there next time */
void ta_window_keep( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	UB		*m = NULL;
	INT		n;
	ID		k;
	BOOL		any;
	ER		er = E_NOEXS;
	char		s[64];

	if ( g_full ) {
		wp = g_restore;			/* 全画面表示 is not where it stands */
	} else if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		return;
	}
	k = ob_opn_obj(&g_obj, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( k <= 0 ) return;
	n = ob_atr_read(k, 0, &m);
	if ( n > 0 ) {
		any = win_num(&m, &n, "x", TRUE, wp.left);
		any = win_num(&m, &n, "y", TRUE, wp.top) || any;
		any = win_num(&m, &n, "width", FALSE, wp.right - wp.left) || any;
		any = win_num(&m, &n, "height", FALSE, wp.bottom - wp.top) || any;
		if ( any ) er = ob_set_atr(k, m, (SZ)n);
	}
	free(m);
	ob_cls_obj(k);
	snprintf(s, sizeof(s), "window kept %d,%d %dx%d (%d)", wp.left, wp.top, wp.right - wp.left,
		 wp.bottom - wp.top, (INT)er);
	note(s);
}

/* 屑実身操作: the desktop's accessory of that name brought up */
void ta_trash_open( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[40];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_TOOL;
	strncpy((char *)rq.name, "屑実身操作", sizeof(rq.name) - 1);
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "trash asked %d", (INT)( ( er < E_OK ) ? er : an.er ));
	note(s);
}

/* ---------------------------------------------------------------- the menu */

void ta_tray_menu( ID mid )
{
	static UB	rows[TR_LEVELS][WM_LABEL_MAX];
	TS_UUID		td;
	T_TRSTS		*s = malloc(sizeof(T_TRSTS));
	INT		i;

	if ( s == NULL ) return;
	if ( ui_uuid(SYSDEF_MENU_TRAY, &td) >= E_OK && tr_get_sts(s) >= E_OK ) {
		for ( i = 1; i <= s->nlevel && i <= TR_LEVELS; i++ ) {
			snprintf((char *)rows[i - 1], WM_LABEL_MAX, "%d %s", i, (char *)s->set[i].name);
		}
		(void)mn_set_lst(mid, &td, "tray.level", (CONST UB *)rows, WM_LABEL_MAX, s->nlevel);
		if ( s->hand > 0 ) (void)mn_chg_idx(mid, &td, "tray.level", s->hand - 1, MN_TICK);
		(void)mn_chg_atr(mid, &td, "tray.clear", ( s->nlevel == 0 ) ? MN_GREY : 0);
	}
	free(s);
}

/* 実行 for a link: the programs the desktop offers for it */
INT ta_exec_menu( ID mid, CONST T_VOBJ *v )
{
	UB	(*rows)[WM_LABEL_MAX];
	TS_UUID	ed;
	T_DTREQ	rq;
	T_DTANS	an;
	INT	n = 1, k;

	g_napp = 0;
	for ( k = 0; k < n && g_napp < APP_MAX; k++ ) {
		INT	i = 0, j;

		memset(&rq, 0, sizeof(rq));
		memset(&an, 0, sizeof(an));
		rq.req = DT_RQ_APPS;
		rq.target = v->target;
		rq.recno = k;
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		/* "id\tname" */
		for ( j = 0; i < DT_TEXT_MAX && an.text[i] != 0 && an.text[i] != '\t'; i++ ) {
			if ( j < APP_ID - 1 ) g_app_id[g_napp][j++] = an.text[i];
		}
		g_app_id[g_napp][j] = 0;
		if ( j == 0 || i >= DT_TEXT_MAX || an.text[i] != '\t' ) continue;
		for ( i++, j = 0; i < DT_TEXT_MAX && an.text[i] != 0; i++ ) {
			if ( j < APP_NAME - 1 ) g_app_name[g_napp][j++] = an.text[i];
		}
		g_app_name[g_napp][j] = 0;
		g_app_kind[g_napp] = (BOOL)( ( an.flags & DT_F_KIND ) != 0 );
		g_napp++;
	}
	rows = malloc(WM_LABEL_MAX * (size_t)( g_napp + 1 ));
	if ( rows == NULL ) g_napp = 0;
	for ( k = 0; k < g_napp; k++ ) snprintf((char *)rows[k], WM_LABEL_MAX, "%s", (char *)g_app_name[k]);
	if ( rows != NULL && ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, g_napp);
	}
	free(rows);
	return g_napp;
}

void ta_exec_app( CONST T_VOBJ *v, INT index )
{
	T_DTREQ	rq;
	T_DTANS	an;
	ER	er;
	char	s[TAD_NAME_MAX + 64];

	if ( index < 0 || index >= g_napp ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_EXEC;
	rq.target = v->target;
	rq.recno = -1;
	snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_app_id[index]);
	if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "exec %s %s (%d)", (char *)v->name, (char *)g_app_id[index], (INT)er);
	note(s);
}

/* ---------------------------------------------------------------- かな漢字変換 */

/* keyboard usages */
#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_SPACE	0x2C
#define KEY_ZENKAKU	0x35		/* 半角/全角 */
#define KEY_EISU	0x39		/* 英数 */
#define KEY_F9		0x42
#define KEY_DOWN	0x51
#define KEY_KANA	0x88		/* ひらがなカタカナ */
#define KEY_HENKAN	0x8A
#define KEY_RCTRL	0xE4
#define MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define MOD_ALT		( HID_MOD_LALT | HID_MOD_RALT )

#define IME_LIST_MAX	4096
#define IME_GROUND	0x00FFFFFFU
#define IME_INK		0x00000000U
#define IME_TARGET	0x000050A0U	/* the clause being converted */
#define IME_CARET	0x00D00000U

static INT	ime_kid = 0;		/* the converter's session, 0 none yet */
static T_KCOUT	ime_out;		/* the composition, as the converter gave it */
static INT	ime_base;		/* where the composition starts in it */
static UINT	ime_kmode;		/* the input mode it was typed in */
static UINT	ime_imode;		/* the input mode of the key being worked */
static BOOL	ime_conv;		/* converted, not just typed */
static BOOL	ime_list;		/* candidates shown */
static UB	ime_cand[IME_LIST_MAX];
static INT	ime_ncand, ime_first, ime_total, ime_sel;
static BOOL	ime_listed;		/* the system lists them for the window */

void ta_ime_mode( UINT imode )
{
	ime_imode = imode;
}

BOOL ta_ime_busy( void )
{
	return (BOOL)( ime_kid > 0 && ime_out.cl[ime_out.n_cl] > ime_base );
}

/* The system's list of candidates put away */
static void ime_unlist( void )
{
	if ( ime_listed ) (void)ob_cand_put(g_kw, 0, 0, 0, NULL, 0, -1, 0, 0);
	ime_listed = FALSE;
}

/*
 * What the converter gave, taken: the committed clauses into the text,
 * the rest kept as the composition, and the list fetched when there is
 * one to show.
 */
static void ime_take( T_DDED *e, INT flags )
{
	T_KCOUT	*o = &ime_out;

	ime_base = 0;
	if ( o->n_out > 0 ) {
		INT	end = o->cl[o->n_out];

		if ( end > 0 && e != NULL ) de_type_text(e, (CONST UB *)o->text, end);
		ime_base = end;
	}
	if ( !ta_ime_busy() ) ime_conv = FALSE;
	ime_list = FALSE;
	if ( ( flags & TSMOZC_LIST ) != 0 && ta_ime_busy() ) {
		INT	r = kc_list(ime_kid, ime_cand, IME_LIST_MAX, &ime_ncand, &ime_first, &ime_total);

		if ( r > 0 && ime_ncand > 0 ) {
			ime_list = TRUE;
			ime_sel = r;
		}
	}
	if ( !ime_list ) ime_unlist();
	g_dirty = TRUE;
}

/* Whatever is being composed, committed into e */
void ta_ime_commit( T_DDED *e )
{
	INT	r;

	if ( e != NULL && ta_ime_busy() ) {
		r = kc_hid_key(ime_kid, KEY_ENTER, HID_MOD_LSHIFT, ime_kmode, &ime_out);
		if ( r >= 0 && r != KC_NOTMINE ) ime_take(e, r);
	}
	memset(&ime_out, 0, sizeof(ime_out));
	ime_base = 0;
	ime_conv = FALSE;
	ime_list = FALSE;
	ime_unlist();
}

/* The session ended with the editing */
void ta_ime_end( T_DDED *e )
{
	ta_ime_commit(e);
	if ( ime_kid > 0 ) kc_close(ime_kid);
	ime_kid = 0;
}

/* A letter typed as it is, in the width and case of the mode: TRUE when it went in */
static BOOL ime_plain( T_DDED *e, UB ch )
{
	UB	t[4];
	UINT	u;

	if ( ch < 0x20 || ch >= 0x7F ) return FALSE;
	if ( ( ime_imode & WM_MODE_ALPH ) != 0 && ( ime_imode & WM_MODE_KANA ) != 0
	  && ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ) ) {
		ch ^= 0x20;		/* 英語 in capitals: the shift turns it back */
	}
	if ( ( ime_imode & WM_MODE_HAN ) != 0 ) {
		de_type_text(e, &ch, 1);
		return TRUE;
	}
	u = ( ch == ' ' ) ? 0x3000 : 0xFF01 + ( ch - 0x21 );
	t[0] = (UB)( 0xE0 | ( u >> 12 ) );
	t[1] = (UB)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
	t[2] = (UB)( 0x80 | ( u & 0x3F ) );
	de_type_text(e, t, 3);
	return TRUE;
}

/* A key in the text: TRUE when the converter or the mode took it */
BOOL ta_ime_key( T_DDED *e, UINT code, UINT mods )
{
	UB	ch = wm_key_char(code, mods);
	BOOL	busy, jp = (BOOL)( ( ime_imode & WM_MODE_ALPH ) == 0 );
	INT	r;

	/* a mode key: the mode is already as it set it; 右Ctrl (入力終) commits */
	if ( code == KEY_RCTRL ) {
		ta_ime_commit(e);
		return TRUE;
	}
	if ( code == KEY_KANA || code == KEY_ZENKAKU
	  || ( code == KEY_EISU && ( ( mods & HID_MOD_LCTRL ) != 0 || ( mods & MOD_ALT ) == 0 ) ) ) {
		if ( !jp || ( code == KEY_KANA && ( mods & MOD_ALT ) != 0 ) ) ta_ime_commit(e);
		g_dirty = TRUE;		/* the caret's colour */
		return TRUE;
	}
	if ( code >= 0xE0 && code <= 0xE7 ) return FALSE;	/* a modifier key, alone */
	if ( !jp ) {
		if ( ta_ime_busy() ) ta_ime_commit(e);
		if ( ( mods & ( MOD_CTRL | MOD_ALT ) ) != 0 ) return FALSE;
		return ime_plain(e, ch);		/* 英語 */
	}
	if ( ime_kid <= 0 ) {
		ime_kid = kc_open();
		if ( ime_kid <= 0 ) {
			ime_kid = 0;
			return FALSE;
		}
		memset(&ime_out, 0, sizeof(ime_out));
		ime_base = 0;
	}
	busy = ta_ime_busy();

	/* a candidate by its number */
	if ( ime_list && ch >= '1' && ch <= '9' && ch - '1' < ime_ncand ) {
		r = kc_choose(ime_kid, ime_first + ( ch - '1' ), &ime_out);
		if ( r >= 0 && r != KC_NOTMINE ) ime_take(e, r);
		return TRUE;
	}
	/* a key that is not a letter: the converter's while it composes */
	if ( !busy && ( ch <= 0x20 || ch >= 0x7F || ( mods & ( MOD_CTRL | MOD_ALT ) ) != 0 )
	  && ( code == KEY_SPACE || ( ime_imode & WM_MODE_ROMAN ) != 0 ) ) {
		return ( code == KEY_SPACE && ( mods & ( MOD_CTRL | MOD_ALT ) ) == 0 ) ? ime_plain(e, ' ') : FALSE;
	}
	switch ( code ) {
	case KEY_SPACE:
	case KEY_HENKAN:
	case KEY_DOWN:
		ime_conv = TRUE;
		break;
	case KEY_BS:
	case KEY_ESC:
	case KEY_F9:
		ime_conv = FALSE;
		break;
	default:
		if ( ch > 0x20 && ch < 0x7F ) ime_conv = FALSE;	/* a letter */
		break;
	}
	ime_kmode = ime_imode;
	r = kc_hid_key(ime_kid, code, mods, ime_imode, &ime_out);
	if ( r >= 0 && r != KC_NOTMINE ) {
		ime_take(e, r);
		return TRUE;
	}
	return busy;			/* nothing else has the keys meanwhile */
}

void ta_ime_pick( T_DDED *e, INT row )
{
	INT	r;

	if ( e == NULL || !ime_list || ime_kid <= 0 || row < 0 || row >= ime_ncand ) return;
	r = kc_choose(ime_kid, ime_first + row, &ime_out);
	if ( r >= 0 && r != KC_NOTMINE ) ime_take(e, r);
}

/* Bytes a..b of the composition, as a string */
static void ime_piece( INT a, INT b, UB *buf, INT size )
{
	INT	n = 0;

	for ( ; a < b && n < size - 1; a++ ) buf[n++] = (UB)ime_out.text[a];
	buf[n] = 0;
}

static INT ime_width( INT a, INT b, INT px )
{
	UB	buf[TSMOZC_TEXT_MAX];

	ime_piece(a, b, buf, sizeof(buf));
	return dp_text_width(buf, px);
}

/* The candidates listed by the system by the composition, or put away */
static void ime_cand_show( INT left, INT top, INT bottom )
{
	if ( !ime_list ) {
		ime_unlist();
	} else if ( ob_cand_put(g_kw, left, top, bottom, ime_cand, ime_ncand,
				ime_sel - ime_first, ime_first, ime_total) >= E_OK ) {
		ime_listed = TRUE;
	}
}

static INT ime_vwidth( ID fid, INT a, INT b )
{
	UB	buf[TSMOZC_TEXT_MAX];

	ime_piece(a, b, buf, sizeof(buf));
	return fn_vwidth(fid, buf);
}

/*
 * The composition down the line (縦書き): from the caret downwards in
 * the vertical forms, on white, each clause lined on its left side, the
 * one being converted turned over, the caret a bar across; the
 * candidates under the end of it.
 */
static void ime_draw_v( T_DDED *e, INT gid, CONST T_DPRECT *box )
{
	T_KCOUT		*o = &ime_out;
	T_DPRECT	b;
	ID		fid = fn_system();
	INT		px, cx, y, c, end;

	if ( fid <= 0 ) return;
	/* the size of the letters it goes in among, no wider than the line */
	px = de_caret_px(e);
	if ( px > ( box->right - box->left ) ) px = box->right - box->left;
	if ( px < 10 ) px = 10;
	(void)fn_set_size(fid, px);
	(void)fn_set_style(fid, FN_ST_VERT, 0);
	end = o->cl[o->n_cl];
	b.left = box->left;
	b.right = box->right;
	b.top = box->top;
	b.bottom = box->top + ime_vwidth(fid, ime_base, end) + 2;
	(void)dp_fill_rect(gid, &b, IME_GROUND);
	cx = ( b.left + b.right ) / 2;

	y = b.top + 1;
	for ( c = o->n_out; c < o->n_cl; c++ ) {
		INT		h = ime_vwidth(fid, o->cl[c], o->cl[c + 1]);
		BOOL		target = (BOOL)( c == o->clause
					&& ( ime_conv || ime_list || o->n_cl - o->n_out > 1 ) );
		UB		buf[TSMOZC_TEXT_MAX];
		T_DPRECT	u;

		ime_piece(o->cl[c], o->cl[c + 1], buf, sizeof(buf));
		if ( target ) {
			u.left = b.left;  u.right = b.right;
			u.top = y;  u.bottom = y + h;
			(void)dp_fill_rect(gid, &u, IME_TARGET);
		}
		(void)fn_vdraw(gid, fid, cx, y, buf, target ? IME_GROUND : IME_INK);
		/* each clause its own line at its left, a gap between them */
		u.top = y + 1;  u.bottom = y + h - 1;
		u.left = b.left;  u.right = b.left + ( target ? 2 : 1 );
		(void)dp_fill_rect(gid, &u, target ? IME_TARGET : IME_INK);
		y += h;
	}
	if ( !ime_conv && !ime_list && o->caret >= ime_base ) {
		T_DPRECT	k;

		k.left = b.left;  k.right = b.right;
		k.top = b.top + 1 + ime_vwidth(fid, ime_base, o->caret);
		k.bottom = k.top + 2;
		(void)dp_fill_rect(gid, &k, IME_CARET);
	}
	ime_cand_show(b.left, b.top, b.bottom);
}

/*
 * The composition, over the text at the caret: on white, each clause
 * underlined, the one being converted turned over, and the caret in it
 * while it is typed; the candidates, listed by the system under it.
 * In vertical text it runs down the line from the caret.
 */
void ta_ime_draw( T_DDED *e, INT gid )
{
	T_DPRECT	page, box, b;
	T_KCOUT		*o = &ime_out;
	T_TVDOC		*doc = *e->doc;
	T_FNMET		met;
	ID		fid;
	INT		px, x, base, c, end;

	if ( !ta_ime_busy() || gid < 0 || doc == NULL ) {
		ime_unlist();
		return;
	}
	e->host->work_rect(e, &page);
	doc = *e->doc;
	if ( tv_doc_caret_box(doc, &page, *e->scroll_y, *e->rec, e->cpara, e->cpos, &box) < E_OK ) {
		return;
	}
	if ( doc->vert ) {
		ime_draw_v(e, gid, &box);
		return;
	}
	px = ( box.bottom - box.top ) * 3 / 4;
	if ( px < 10 ) px = 10;
	/* the letters stood in the line by the system face's own ascent and descent at that size */
	fid = fn_system();
	if ( fid <= 0 || fn_set_size(fid, px) < E_OK || fn_metrics(fid, &met) < E_OK ) {
		met.ascent = px;
		met.descent = px / 4;
	}
	end = o->cl[o->n_cl];
	b.left = box.left;
	b.top = box.top;
	b.right = box.left + ime_width(ime_base, end, px) + 2;
	b.bottom = box.bottom;
	(void)dp_fill_rect(gid, &b, IME_GROUND);
	base = b.bottom - ( b.bottom - b.top - met.ascent - met.descent ) / 2 - met.descent;

	x = b.left + 1;
	for ( c = o->n_out; c < o->n_cl; c++ ) {
		INT		w = ime_width(o->cl[c], o->cl[c + 1], px);
		BOOL		target = (BOOL)( c == o->clause
					&& ( ime_conv || ime_list || o->n_cl - o->n_out > 1 ) );
		UB		buf[TSMOZC_TEXT_MAX];
		T_DPRECT	u;

		ime_piece(o->cl[c], o->cl[c + 1], buf, sizeof(buf));
		if ( target ) {
			u.left = x;  u.right = x + w;
			u.top = b.top;  u.bottom = b.bottom;
			(void)dp_fill_rect(gid, &u, IME_TARGET);
		}
		(void)dp_text(gid, x, base, buf, target ? IME_GROUND : IME_INK, px);
		/* each clause its own underline, a gap between them */
		u.left = x + 1;  u.right = x + w - 1;
		u.top = b.bottom - 2;  u.bottom = b.bottom - ( target ? 0 : 1 );
		(void)dp_fill_rect(gid, &u, target ? IME_TARGET : IME_INK);
		x += w;
	}
	if ( !ime_conv && !ime_list && o->caret >= ime_base ) {
		T_DPRECT	k;

		k.left = b.left + 1 + ime_width(ime_base, o->caret, px);
		k.right = k.left + 2;
		k.top = b.top;  k.bottom = b.bottom;
		(void)dp_fill_rect(gid, &k, IME_CARET);
	}
	ime_cand_show(b.left, b.top, b.bottom);
}

/* かな input: the caret drawn in its colour */
BOOL ta_ime_on( void )
{
	return (BOOL)( ( kc_mode() & WM_MODE_ALPH ) == 0 );
}

/* ---------------------------------------------------------------- the panels of the data box */

#define TA_P_CANCEL	101		/* 取消, in every panel of the data box */
#define TA_P_OK		102		/* the button that goes ahead */
#define TA_P_TITLE	61		/* the words over a form's boxes or a list */

/*
 * A panel of the data box worked until one of its buttons is pressed or
 * it is put away: the number of the button, 'enter' for Enter, 0 for 取消
 * and for a panel put away
 */
static INT dlg_run( UI *q, INT enter )
{
	T_OBNTM	m;
	INT	num = 0, ev, how = -1;

	while ( how < 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(q, &m, &num) : UI_EV_CLOSE;
		if ( ev == UI_EV_BUTTON ) {
			how = ( num == TA_P_CANCEL ) ? 0 : num;
		} else if ( ev == UI_EV_CLOSE ) {
			how = 0;
		} else if ( m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0
			 && ( m.code == KEY_ENTER || m.code == 0x58 ) ) {
			how = enter;
		}
	}
	return how;
}

/*
 * A name asked (TA_PN_NAME): what is asked over a box with the name
 * offered in it, 取消 and 設定; Enter is 設定. FALSE when the panel was
 * put away or the name left empty.
 */
BOOL ta_ask_name( CONST char *prompt, CONST UB *start, UB *out, INT max )
{
	UI	q;
	INT	how;

	out[0] = 0;
	if ( ui_dlg_open(&g_ui, TA_PN_NAME, &q) < E_OK ) return FALSE;
	ui_set_text(&q, 1, prompt);
	ui_set_text(&q, 2, ( start != NULL ) ? (CONST char *)start : "");
	ui_focus(&q, 2);
	note("name panel open");
	how = dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) (void)ui_text(&q, 2, (char *)out, max);
	ui_dlg_close(&q);
	g_dirty = TRUE;
	note(( how == TA_P_OK && out[0] != 0 ) ? "name panel answered" : "name panel put away");
	return (BOOL)( how == TA_P_OK && out[0] != 0 );
}

/*
 * One line of text asked (TA_PN_TEXT): what is asked over a box with
 * what there is offered in it, 取消 and the button that goes ahead (its
 * words 'ok'); Enter is that button. TRUE when it was pressed, the box
 * empty or not. The editor asks with one line; a second (p2) is put
 * after the first.
 */
BOOL ta_ask_text( CONST char *p1, CONST char *p2, CONST UB *start, UB *out, INT max, CONST char *ok )
{
	UI	q;
	INT	how;
	char	say[WM_LABEL_MAX];

	out[0] = 0;
	if ( ui_dlg_open(&g_ui, TA_PN_TEXT, &q) < E_OK ) return FALSE;
	snprintf(say, sizeof(say), "%s%s", ( p1 != NULL ) ? p1 : "", ( p2 != NULL ) ? p2 : "");
	ui_set_text(&q, 1, say);
	ui_set_text(&q, 2, ( start != NULL ) ? (CONST char *)start : "");
	if ( ok != NULL && strcmp(ok, "設定") != 0 ) ui_set_text(&q, TA_P_OK, ok);	/* the panel says 設定 */
	ui_focus(&q, 2);
	note("text panel open");
	how = dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) (void)ui_text(&q, 2, (char *)out, max);
	ui_dlg_close(&q);
	g_dirty = TRUE;
	note(( how == TA_P_OK ) ? "text panel answered" : "text panel put away");
	return (BOOL)( how == TA_P_OK );
}

/*
 * Labelled boxes one under another, each with the word after it saying
 * what it is counted in (TA_PN_FIELDS6 for up to six, TA_PN_FIELDS9 for
 * up to nine: the panel as tall as the desktop's for that many); what
 * each holds comes in and goes out in vals. TRUE when 設定 was pressed.
 */
BOOL ta_fields_form( CONST char *title, INT n, CONST char *CONST *labels,
		     CONST char *CONST *hints, UB (*vals)[DE_FIELD_MAX] )
{
	UI	q;
	INT	how, i, rows = ( n <= 6 ) ? 6 : 9;

	if ( n <= 0 || n > 9 || ui_dlg_open(&g_ui, ( rows == 6 ) ? TA_PN_FIELDS6 : TA_PN_FIELDS9, &q) < E_OK ) {
		return FALSE;
	}
	ui_set_text(&q, TA_P_TITLE, title);
	for ( i = 0; i < rows; i++ ) {
		BOOL	on = (BOOL)( i < n );

		ui_show(&q, 21 + i, on);
		ui_show(&q, 1 + i, on);
		ui_show(&q, 41 + i, on);
		if ( !on ) continue;
		ui_set_text(&q, 21 + i, labels[i]);
		ui_set_text(&q, 1 + i, (CONST char *)vals[i]);
		ui_set_text(&q, 41 + i, ( hints != NULL && hints[i] != NULL ) ? hints[i] : "");
	}
	ui_focus(&q, 1);
	note("fields panel open");
	how = dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) {
		for ( i = 0; i < n; i++ ) (void)ui_text(&q, 1 + i, (char *)vals[i], DE_FIELD_MAX);
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
	note(( how == TA_P_OK ) ? "fields panel answered" : "fields panel put away");
	return (BOOL)( how == TA_P_OK );
}

/* One of a list of names chosen (TA_PN_LIST), start picked to begin with: its number, or -1 */
INT ta_list_form( CONST char *title, CONST char *CONST *names, INT n, INT start )
{
	UI	q;
	INT	how, v = 0;
	char	t[40];

	if ( n <= 0 || ui_dlg_open(&g_ui, TA_PN_LIST, &q) < E_OK ) return -1;
	ui_set_text(&q, TA_P_TITLE, title);
	ui_names(&q, 1, names, n);
	ui_set_value(&q, 1, ( start >= 0 && start < n ) ? start + 1 : 0);
	note("list panel open");
	how = dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) v = ui_value(&q, 1);
	ui_dlg_close(&q);
	g_dirty = TRUE;
	v = ( how == TA_P_OK && v >= 1 && v <= n ) ? v - 1 : -1;
	snprintf(t, sizeof(t), "list panel answered %d", v);
	note(t);
	return v;
}

/*
 * 検索/置換 (TA_PN_FIND): what to look for and what to put in its place,
 * kept by the editor from one asking to the next. Answers what was
 * pressed -- DE_FIND_NEXT (次を検索, or Enter), DE_FIND_REPLACE,
 * DE_FIND_ALL -- or 0 when the panel was put away.
 */
#define P_FIND_WHAT	1		/* 検索文字列, 置換文字列 */
#define P_FIND_WITH	2
#define P_FIND_ALL	3		/* 全置換 */
#define P_FIND_REPLACE	4		/* 置換 */
#define P_FIND_REGEX	5		/* 正規表現 */

INT ta_find_form( UB *what, UB *with, INT max, BOOL *p_regex )
{
	UI	q;
	INT	how;
	char	t[48];

	if ( ui_dlg_open(&g_ui, TA_PN_FIND, &q) < E_OK ) return 0;
	ui_set_text(&q, P_FIND_WHAT, (CONST char *)what);
	ui_set_text(&q, P_FIND_WITH, (CONST char *)with);
	ui_set_value(&q, P_FIND_REGEX, *p_regex ? 1 : 0);
	ui_focus(&q, P_FIND_WHAT);
	note("find panel open");
	how = dlg_run(&q, TA_P_OK);
	how = ( how == TA_P_OK ) ? DE_FIND_NEXT : ( how == P_FIND_REPLACE ) ? DE_FIND_REPLACE
	    : ( how == P_FIND_ALL ) ? DE_FIND_ALL : 0;
	if ( how != 0 ) {
		(void)ui_text(&q, P_FIND_WHAT, (char *)what, max);
		(void)ui_text(&q, P_FIND_WITH, (char *)with, max);
		*p_regex = (BOOL)( ui_value(&q, P_FIND_REGEX) != 0 );
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
	snprintf(t, sizeof(t), "find panel answered %d", how);
	note(t);
	return how;
}

/* ---------------------------------------------------------------- 用紙設定 */

#define PS_LIST		1
#define PS_H		2		/* 2..7: 縦 横 上 下 左 右, in tenths of a millimetre */
#define PS_ONE		8		/* 片面, 見開き: selector switches of one group */
#define PS_SPREAD	9
#define PS_STD		92		/* 標準設定 */
#define PS_HF		93		/* ヘッダ/フッタ */

static CONST struct {
	CONST char	*name;
	INT		w, h;		/* tenths of a millimetre */
} papers[] = {
	{ "A3", 2970, 4200 },		{ "A4", 2100, 2970 },
	{ "A5", 1480, 2100 },		{ "B4", 2570, 3640 },
	{ "B5", 1820, 2570 },		{ "はがき", 1000, 1480 },
	{ "往復はがき", 1000, 2000 },	{ "長形3号", 1200, 2350 },
	{ "寸法指定", 0, 0 }
};
#define N_PAPERS	( (INT)( sizeof(papers) / sizeof(papers[0]) ) )

/* Tenths as words: "210" or "210.5" */
static void tenths_text( INT v, char *out, INT max )
{
	if ( v < 0 ) v = 0;
	if ( v % 10 != 0 ) snprintf(out, (size_t)max, "%d.%d", v / 10, v % 10);
	else snprintf(out, (size_t)max, "%d", v / 10);
}

/* Words as tenths; -1 when they are not a number */
static INT text_tenths( CONST char *s )
{
	INT	v = 0, f = -1;
	BOOL	any = FALSE;

	while ( *s == ' ' ) s++;
	for ( ; *s != 0; s++ ) {
		if ( *s >= '0' && *s <= '9' ) {
			if ( f < 0 ) v = v * 10 + ( *s - '0' );
			else if ( f == 0 ) f = *s - '0';
			any = TRUE;
		} else if ( *s == '.' && f < 0 ) {
			f = 0;
		} else if ( *s != ' ' ) {
			return -1;
		}
	}
	return any ? v * 10 + ( ( f > 0 ) ? f : 0 ) : -1;
}

/*
 * 用紙設定 (TA_PN_PAPER): the paper chosen from the list or its size
 * typed, the margins, 片面 or 見開き. Answers DE_PAPER_SET (設定, or
 * Enter) with what was put in it in pp, DE_PAPER_STD (標準設定),
 * DE_PAPER_HF (ヘッダ/フッタ), or DE_PAPER_CANCEL.
 */
INT ta_paper_form( T_DEPAPER *pp )
{
	UI	q;
	INT	how, i, v, start = N_PAPERS - 1;
	INT	*to[6];
	char	t[24];

	for ( i = 0; i < N_PAPERS - 1; i++ ) {
		if ( papers[i].w == pp->w && papers[i].h == pp->h ) start = i;
	}
	to[0] = &pp->h;    to[1] = &pp->w;
	to[2] = &pp->top;  to[3] = &pp->bottom;
	to[4] = &pp->left; to[5] = &pp->right;
	if ( ui_dlg_open(&g_ui, TA_PN_PAPER, &q) < E_OK ) return DE_PAPER_CANCEL;
	ui_set_value(&q, PS_LIST, start + 1);
	for ( i = 0; i < 6; i++ ) {
		tenths_text(*to[i], t, sizeof(t));
		ui_set_text(&q, PS_H + i, t);
	}
	ui_set_value(&q, ( pp->imposition != 0 ) ? PS_SPREAD : PS_ONE, 1);
	note("paper panel open");
	how = dlg_run(&q, TA_P_OK);
	how = ( how == TA_P_OK ) ? DE_PAPER_SET : ( how == PS_STD ) ? DE_PAPER_STD
	    : ( how == PS_HF ) ? DE_PAPER_HF : DE_PAPER_CANCEL;
	if ( how == DE_PAPER_SET ) {
		for ( i = 0; i < 6; i++ ) {
			if ( ui_text(&q, PS_H + i, t, sizeof(t)) >= 0 && ( v = text_tenths(t) ) >= 0 ) *to[i] = v;
		}
		/* a size chosen from the list is that size */
		v = ui_value(&q, PS_LIST);
		if ( v >= 1 && v < N_PAPERS ) {
			pp->w = papers[v - 1].w;
			pp->h = papers[v - 1].h;
		}
		pp->imposition = ( ui_value(&q, PS_SPREAD) != 0 ) ? 1 : 0;
	}
	ui_dlg_close(&q);
	g_dirty = TRUE;
	snprintf(t, sizeof(t), "paper panel answered %d", how);
	note(t);
	return how;
}

/* A colour chosen with the system's panel over the window, from *io: FALSE when it was abandoned */
BOOL ta_pick_colour( UW *io )
{
	T_DTREQ	rq;
	T_DTANS	an;
	BOOL	ok;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_COLOUR;
	rq.win = g_win;
	rq.look.bgcol = *io;
	ok = (BOOL)( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK );
	if ( ok ) *io = an.look.bgcol & 0x00FFFFFFU;
	g_dirty = TRUE;
	note(ok ? "colour chosen" : "colour put away");
	return ok;
}
