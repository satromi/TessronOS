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
UB		*g_name = (UB *)"";
static BOOL	g_name_own = FALSE;	/* g_name is memory of this program's own */
TS_UUID		g_win;
ID		g_kw;
INT		g_gid;
INT		g_w, g_h;
UI		g_ui;
ID		g_port;
T_TAD		*g_rec;
UW		g_paper = TV_PAPER_LOOK;
static UW	g_paper_own = TV_PAPER_LOOK;	/* the object's own, as its metadata keeps it */
static UW	g_link_bg = TAD_COL_NONE;	/* the 表示領域背景 of the link it was opened from, as last known */
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
static ID	g_watch_key[WATCH_MAX];	/* the key each is watched with */
static ID	g_devlist;		/* the list of the devices, watched while a medium of a watched object is out */
static INT	g_nwatched;

/* the programs 実行 offers for a link */
static UB	g_app_id[APP_MAX][APP_ID];
static UB	g_app_name[APP_MAX][APP_NAME];
static BOOL	g_app_kind[APP_MAX];
static INT	g_napp;

/* ---------------------------------------------------------------- saying things */

/*
 * A line said on the console, of TA_LOG bytes at most: what is longer
 * (a long name in it) is cut there, and never in the middle of a letter
 * -- what a caller cut short into its own line too
 */
void note( CONST char *s )
{
	char	line[TA_LOG];
	INT	n, k, need;

	n = snprintf(line, sizeof(line) - 1, "%s: %s", ( ta_host != NULL ) ? ta_host->who : "app", s);
	if ( n > (INT)sizeof(line) - 2 ) n = (INT)sizeof(line) - 2;
	/* a letter whose last bytes are not there is left out */
	for ( k = n - 1; k >= 0 && ( (UB)line[k] & 0xC0 ) == 0x80; k-- ) ;
	if ( k >= 0 && ( (UB)line[k] & 0x80 ) != 0 ) {
		UB	c = (UB)line[k];

		need = ( c >= 0xF0 ) ? 4 : ( c >= 0xE0 ) ? 3 : ( c >= 0xC0 ) ? 2 : 1;
		if ( n - k < need ) n = k;
	}
	line[n++] = '\n';
	line[n] = 0;
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
/*
 * The tail of the next request (design 11.4): its parts gathered by
 * ta_req_part, sent with it by ask_desktop and forgotten then. The
 * answer is kept whole, its tail with it, until the next request:
 * ta_ans_part finds a part of it.
 */
static UB	*rq_tail = NULL;
static UINT	rq_tailsz = 0;
static SZ	rq_cap = 0;
static BOOL	rq_short = FALSE;		/* a part could not be added: the request is not sent */
static UB	*an_whole = NULL;		/* the last answer, its tail after it */
static SZ	an_cap = 0, an_size = 0;

void ta_req_part( UINT kind, CONST UB *s, SZ len )
{
	SZ	need;

	if ( s == NULL ) len = 0;
	else if ( len < 0 ) len = (SZ)strlen((CONST char *)s);
	need = (SZ)rq_tailsz + dt_tail_room(len);
	if ( need > rq_cap ) {
		SZ	cap = ( rq_cap == 0 ) ? 256 : rq_cap;
		UB	*t;

		while ( cap < need ) cap *= 2;
		t = (UB *)realloc(rq_tail, (size_t)cap);
		if ( t == NULL ) {
			rq_short = TRUE;		/* not sent without it: ask_desktop answers E_NOMEM */
			return;
		}
		rq_tail = t;
		rq_cap = cap;
	}
	(void)dt_tail_put(rq_tail, &rq_tailsz, rq_cap, kind, s, len);
}

CONST UB *ta_ans_part( UINT kind, SZ *p_len )
{
	CONST T_DTANS	*a = (CONST T_DTANS *)an_whole;

	if ( a == NULL || an_size < (SZ)sizeof(T_DTANS) ) {
		if ( p_len != NULL ) *p_len = 0;
		return NULL;
	}
	return dt_tail_get(dt_ans_tail(a, an_size), a->tailsz, kind, p_len);
}

/* A message read whole from a channel into *p_buf (as large as *p_cap, made larger as it must be): its size, or an error */
SZ ta_read_whole( ID k, UB **p_buf, SZ *p_cap, TMO tmo )
{
	SZ	asz = 0;
	ER	er;

	for ( ;; ) {
		if ( *p_buf == NULL || *p_cap < (SZ)sizeof(T_DTANS) ) {
			UB	*b = (UB *)realloc(*p_buf, sizeof(T_DTANS) + 256);

			if ( b == NULL ) return E_NOMEM;
			*p_buf = b;
			*p_cap = (SZ)sizeof(T_DTANS) + 256;
		}
		er = ob_rea_tmo(k, 0, 0, *p_buf, *p_cap, &asz, tmo);
		if ( er == E_LIMIT && asz > *p_cap ) {
			UB	*b = (UB *)realloc(*p_buf, (size_t)asz);

			if ( b == NULL ) return E_NOMEM;
			*p_buf = b;
			*p_cap = asz;
			continue;		/* the message is still there: read it again, whole */
		}
		return ( er < E_OK ) ? (SZ)er : asz;
	}
}

ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0, msz;
	ID		k, ka;
	ER		er;
	UB		*msg;

	er = E_OK;
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) er = E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( er >= E_OK && ob_cre_obj(&c, &reply) < E_OK ) er = E_NOMEM;
	ka = ( er >= E_OK ) ? ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT) : 0;
	if ( er >= E_OK && ka <= 0 ) {
		(void)ob_del_obj(&reply);
		er = E_OBJ;
	}
	if ( er < E_OK ) {
		/* the tail gathered for it goes with it, not to the next request */
		rq_tailsz = 0;
		rq_short = FALSE;
		return er;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	{
		static CONST TS_UUID	none;

		/* asked over the program's window, unless the asker named another of the program's */
		if ( memcmp(&rq->win, &none, sizeof(none)) == 0 ) rq->win = g_win;
	}
	/* a request that lost a part of its tail is not sent short */
	if ( rq_short ) {
		rq_short = FALSE;
		rq_tailsz = 0;
		ob_cls_obj(ka);
		(void)ob_del_obj(&reply);
		return E_NOMEM;
	}
	/* the request and its tail in one message */
	rq->tailsz = rq_tailsz;
	msz = (SZ)sizeof(*rq) + (SZ)rq_tailsz;
	msg = (UB *)malloc((size_t)msz);
	if ( msg != NULL ) {
		memcpy(msg, rq, sizeof(*rq));
		if ( rq_tailsz > 0 ) memcpy(msg + sizeof(*rq), rq_tail, rq_tailsz);
	}
	rq_tailsz = 0;
	if ( msg == NULL ) {
		ob_cls_obj(ka);
		(void)ob_del_obj(&reply);
		return E_NOMEM;
	}
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, msg, msz, &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	free(msg);
	an_size = 0;
	while ( er >= E_OK ) {
		SZ	n = ta_read_whole(ka, &an_whole, &an_cap, wait_ms);

		if ( n < 0 ) {
			er = (ER)n;
			break;
		}
		if ( n >= (SZ)sizeof(*an) && ((T_DTANS *)an_whole)->seq == rq->seq ) {
			an_size = n;
			memcpy(an, an_whole, sizeof(*an));
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
	ta_link_open(v, 0, 0);
}

/*
 * The same with DT_F_AUTO in flags (自動起動): opened by no act of the
 * person, at place rank of the links of the record opened so
 */
void ta_link_open( CONST T_VOBJ *v, UINT flags, INT rank )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[TA_LOG];
	ER	er;

	char	po[40], pv[40], pc[40];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = v->target;
	rq.recno = -1;
	rq.flags = flags;
	rq.grab_y = rank;
	/* what the window opened does to the link comes back here, to be put in the record (DT_F_LINK) */
	if ( g_lport > 0 ) {
		ts_uuid_text(&g_obj, po);
		ts_uuid_text(&v->vobjid, pv);
		ts_uuid_text(&g_lch, pc);
		snprintf(s, sizeof(s), "%s\t%s\t%s", po, pv, pc);
		ta_req_part(DT_TAIL_TEXT, (CONST UB *)s, -1);
		rq.flags |= DT_F_LINK;
		/* shown as the link says */
		rq.look.frcol = v->frcol;
		rq.look.chcol = v->chcol;
		rq.look.tbcol = v->tbcol;
		rq.look.bgcol = v->bgcol;
		rq.grab_x = v->wordwrap;
		ta_req_part(DT_TAIL_VIEW, v->vview, v->vview_len);
	}
	if ( v->vapplist_len > 0 ) {
		ta_req_part(DT_TAIL_APPLIST, v->vapplist, v->vapplist_len);
		rq.flags |= DT_F_APPLIST;
	}
	er = ask_desktop(&rq, &an, 3000);
	snprintf(s, sizeof(s), "opened %.80s (%d)", (CONST char *)TAD_VS(v->vname), (INT)er);
	note(s);
	if ( er < E_OK ) say("開けませんでした");
}

/*
 * A request about link vobjid of this record (DT_RQ_ATTR) named for the
 * desktop as DT_RQ_OPEN with DT_F_LINK names it: the record's object, the
 * vobjid and the channel, parted by tabs, as the tail's text (the applist
 * is a part of its own), so that the windows opened from the link are
 * told what it changes. FALSE, and rq as it was, when there is no channel.
 */
BOOL ta_link_named( T_DTREQ *rq, CONST TS_UUID *vobjid )
{
	char	po[TS_UUID_STRLEN + 1], pv[TS_UUID_STRLEN + 1], pc[TS_UUID_STRLEN + 1];
	char	head[3 * ( TS_UUID_STRLEN + 1 ) + 1];

	if ( g_lport <= 0 ) return FALSE;
	ts_uuid_text(&g_obj, po);
	ts_uuid_text(vobjid, pv);
	ts_uuid_text(&g_lch, pc);
	snprintf(head, sizeof(head), "%s\t%s\t%s", po, pv, pc);
	ta_req_part(DT_TAIL_TEXT, (CONST UB *)head, -1);
	rq->flags |= DT_F_LINK;
	return TRUE;
}

/* ---------------------------------------------------------------- the record */

/* What a record links to, every link counted */
static INT targets_of( CONST T_TAD *rec, TS_UUID **p_out )
{
	T_VOBJ	v = { .recno = 0 };
	INT	n = tad_lnk_count(rec), i, k = 0;
	TS_UUID	*t;

	*p_out = NULL;
	if ( n <= 0 ) return 0;
	t = malloc(sizeof(TS_UUID) * (size_t)n);
	if ( t == NULL ) return -1;
	tad_vobj_init(&v);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(rec, i, &v) >= E_OK ) t[k++] = v.target;
	}
	tad_vobj_clear(&v);
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

/*
 * The window asked to close while the disk of its object is out (the
 * desktop asks so when the disk is taken out): what was changed handed
 * to the desktop, which asks the person where it is to go
 * (DT_DISK_RESCUE) -- written into an object the desktop makes for it,
 * then named to it. TRUE when the object cannot be reached: the window
 * closes without saving and without keeping where it stood; FALSE when
 * it is there, to close as usual. figure: what is held is a figure.
 */
BOOL ta_disk_gone( BOOL figure )
{
	T_OBREF	ref;
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	tmp;
	UB	*buf;
	SZ	size = 0;
	ID	k;
	ER	er;

	if ( ob_ref_obj(&g_obj, &ref) >= E_OK ) return FALSE;
	if ( g_changed && g_rec != NULL ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_DISK;
		rq.recno = DT_DISK_RESCUE;
		rq.target = g_obj;
		rq.grab_x = 0;
		rq.grab_y = figure ? 1 : 0;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 5000);
		tmp = an.into;
		buf = NULL;
		if ( er >= E_OK && tad_write_mem(g_rec, NULL, 0, &size) >= E_OK
		  && ( buf = malloc((size_t)size + 1) ) != NULL && tad_write_mem(g_rec, buf, size + 1, &size) >= E_OK ) {
			k = ob_opn_obj(&tmp, OB_OP_R | OB_OP_W);
			er = ( k > 0 ) ? ob_rpl_rec(k, 0, buf, size) : (ER)k;
			if ( k > 0 ) ob_cls_obj(k);
		} else if ( er >= E_OK ) {
			er = E_NOMEM;
		}
		free(buf);
		if ( er >= E_OK ) {
			char	tu[TS_UUID_STRLEN + 1];

			rq.grab_x = 1;
			ts_uuid_text(&tmp, tu);
			ta_req_part(DT_TAIL_TEXT, (CONST UB *)tu, -1);
			memset(&an, 0, sizeof(an));
			er = ask_desktop(&rq, &an, TMO_FEVR);
		}
		{
			char	s[48];

			snprintf(s, sizeof(s), "changes handed over, its disk gone (%d)", (INT)er);
			note(s);
		}
		g_changed = FALSE;
	}
	note("closes: its disk went");
	return TRUE;
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
	/*
	 * told to one who may read its attributes; a key that reads no
	 * records keeps nobody out who opens the object alone to write it
	 * (書庫管理 writing its book, OB_O_EXCL)
	 */
	k = ob_opn_obj(id, OB_OP_ATRRD);
	if ( k <= 0 ) return;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE | OB_E_DETACH;
	req.id = 1;
	(void)ob_ntf_evt(k, OB_REC_ANY, &req, g_port);
	/*
	 * the request goes with the key: the key is kept until the program
	 * ends, or until the medium the object is on goes (OB_E_DETACH,
	 * ta_links_event), when it is closed at once so that the volume is
	 * not held; the links watched again (ta_watch_links) watch it anew
	 */
	g_watch_key[g_nwatched] = k;
	g_watched[g_nwatched++] = *id;
}

/* The medium of a watched object went: its key closed and the object no longer counted as watched */
static BOOL watch_gone( CONST TS_UUID *id )
{
	INT	i;

	for ( i = 0; i < g_nwatched; i++ ) {
		if ( memcmp(&g_watched[i], id, sizeof(TS_UUID)) == 0 ) break;
	}
	if ( i >= g_nwatched ) return FALSE;
	(void)ob_cls_obj(g_watch_key[i]);
	g_nwatched--;
	g_watched[i] = g_watched[g_nwatched];
	g_watch_key[i] = g_watch_key[g_nwatched];
	note("watched object gone with its medium: let go");
	/*
	 * the list of the devices watched until the links are watched again:
	 * a disk that comes and a volume attached are told there (it is the
	 * kernel's, on no medium, and holds no volume)
	 */
	if ( g_devlist <= 0 && g_port > 0 ) {
		CONST TS_UUID	devlist = OB_UUID_DEVLIST_INIT;
		T_OBNTF		req;

		g_devlist = ob_opn_obj(&devlist, OB_OP_ATRRD);
		if ( g_devlist > 0 ) {
			memset(&req, 0, sizeof(req));
			req.events = OB_E_CHANGE | OB_E_ATTACH;
			req.id = 3;
			if ( ob_ntf_evt(g_devlist, OB_REC_ANY, &req, g_port) <= 0 ) {
				(void)ob_cls_obj(g_devlist);
				g_devlist = 0;
			}
		} else {
			g_devlist = 0;
		}
	}
	return TRUE;
}

void ta_watch_links( void )
{
	T_VOBJ	v = { .recno = 0 };
	INT	i, n = ( g_rec != NULL ) ? tad_lnk_count(g_rec) : 0;

	tad_vobj_init(&v);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(g_rec, i, &v) >= E_OK ) ta_watch(&v.target);
	}
	tad_vobj_clear(&v);
}

/*
 * How many of the record's links point at objects not watched now (their
 * medium out, or past WATCH_MAX): while any is, the list of the devices
 * stays watched, and they are tried again as disks and volumes come
 */
static INT links_unwatched( void )
{
	T_VOBJ	v = { .recno = 0 };
	INT	i, k, left = 0, n = ( g_rec != NULL ) ? tad_lnk_count(g_rec) : 0;

	tad_vobj_init(&v);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(g_rec, i, &v) < E_OK ) continue;
		for ( k = 0; k < g_nwatched && memcmp(&g_watched[k], &v.target, sizeof(TS_UUID)) != 0; k++ ) ;
		if ( k >= g_nwatched ) left++;
	}
	tad_vobj_clear(&v);
	return left;
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

static void paper_shown( void );

/*
 * The link this window was opened from changed by 仮身属性変更 in the
 * window that holds it, told by the desktop before that record is saved
 * (DT_HOW_LINK_LOOK): its 表示領域背景 is the paper when the object has
 * none of its own. Only bgcol of the look shows here; the frame, the
 * letters and the band of the link are drawn by the window holding it.
 */
/* The text of an answer or a notice given with its tail after it: "" when it has none */
static CONST UB *told_text( CONST T_DTANS *an, SZ *p_len )
{
	CONST UB	*t = dt_tail_get(dt_ans_tail(an, (SZ)sizeof(*an) + (SZ)an->tailsz), an->tailsz, DT_TAIL_TEXT, p_len);

	return ( t != NULL ) ? t : (CONST UB *)"";
}

static void link_look_told( CONST T_DTANS *an )
{
	TS_UUID	p, v;
	char	s[48];
	SZ	tl = 0;
	CONST UB *tx = told_text(an, &tl);

	if ( !g_has_parent || tl < 2 * TS_UUID_STRLEN + 1 || ts_str_to_uuid((CONST char *)tx, &p) < E_OK
	  || tx[TS_UUID_STRLEN] != '\t'
	  || ts_str_to_uuid((CONST char *)tx + TS_UUID_STRLEN + 1, &v) < E_OK
	  || memcmp(&p, &g_parent, sizeof(TS_UUID)) != 0 || memcmp(&v, &g_vobjid, sizeof(TS_UUID)) != 0
	  || memcmp(&an->into, &g_obj, sizeof(TS_UUID)) != 0 ) return;
	g_link_bg = an->look.bgcol;
	paper_shown();
	g_dirty = TRUE;
	snprintf(s, sizeof(s), "link look told: bgcol #%06x", (UINT)( g_link_bg & 0x00FFFFFFU ));
	note(s);
}

/*
 * What a window opened from a link of this record did to it, told by the
 * desktop (DT_F_LINK): the view it closed with written into the link, or
 * a link to the copy it was saved as put with it. The record changes as
 * a window of the desktop's changes its own: nothing to take back.
 */
static void link_told( CONST T_DTANS *an )
{
	T_VOBJ	v = { .recno = 0 };
	TS_UUID	vid, id;
	INT	at = 2 * TS_UUID_STRLEN + 1, sx, sy, zoom, wrap, i;
	BOOL	changed = FALSE;
	CONST UB *vm = NULL;
	char	s[80];
	SZ	tl = 0;
	CONST UB *tx = told_text(an, &tl);

	if ( an->how == DT_HOW_LINK_LOOK ) {
		link_look_told(an);
		return;
	}
	/* anything else the desktop says there is the program's (the 小物箱's list changed) */
	if ( an->how != DT_HOW_LINK_VIEW && an->how != DT_HOW_LINK_ADD ) {
		if ( ta_host != NULL && ta_host->told != NULL ) ta_host->told(an);
		return;
	}
	/* the record's object, a tab, the link's vobjid */
	tad_vobj_init(&v);
	if ( g_rec == NULL || tl < 2 * TS_UUID_STRLEN + 1 || ts_str_to_uuid((CONST char *)tx, &id) < E_OK
	  || memcmp(&id, &g_obj, sizeof(TS_UUID)) != 0 || tx[TS_UUID_STRLEN] != '\t'
	  || ts_str_to_uuid((CONST char *)tx + TS_UUID_STRLEN + 1, &vid) < E_OK
	  || tad_lnk_find(g_rec, &vid, &v) < E_OK ) {
		tad_vobj_clear(&v);
		return;
	}
	if ( an->how == DT_HOW_LINK_VIEW ) {
		if ( memcmp(&v.target, &an->into, sizeof(TS_UUID)) != 0 ) return;
		sx = text_num(tx, &at);
		sy = text_num(tx, &at);
		zoom = text_num(tx, &at);
		wrap = text_num(tx, &at);
		i = 0;
		if ( tx[at] == '\t' ) {
			vm = tx + at + 1;
			for ( ; vm[i] != 0; i++ ) ;
		}
		if ( v.scrollx != sx || v.scrolly != sy ) {
			v.scrollx = sx;
			v.scrolly = sy;
			changed = TRUE;
		}
		if ( vm != NULL && i > 0 && strcmp((CONST char *)TAD_VS(v.vview), (CONST char *)vm) != 0 ) {
			if ( tad_vobj_set(&v, TAD_W_VIEW, vm, i) >= E_OK ) changed = TRUE;
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
		(void)tad_vobj_set(&v, TAD_W_NAME, NULL, 0);
		v.dlen = 0;			/* a new object has no data appended to it yet */
		if ( ta_host != NULL && ta_host->picks_none != NULL ) ta_host->picks_none();
		if ( tad_lnk_add(g_rec, &v, &id) >= E_OK ) {
			tv_src_forget(&an->into);
			ta_watch(&an->into);
			remade();
			note("a link to the copy saved put with its link");
		}
	}
	tad_vobj_clear(&v);
}

void ta_links_told( void )
{
	static UB	*buf = NULL;
	static SZ	cap = 0;
	SZ		n;

	/* each notice whole, its tail after it, as the program's told() is given it */
	while ( g_lport > 0 && ( n = ta_read_whole(g_lport, &buf, &cap, TMO_POL) ) >= (SZ)sizeof(T_DTANS) ) {
		T_DTANS	*an = (T_DTANS *)buf;

		if ( n < (SZ)sizeof(T_DTANS) + (SZ)an->tailsz ) an->tailsz = 0;	/* not one: shorter than it says */
		link_told(an);
	}
}

BOOL ta_told_channel( TS_UUID *p )
{
	if ( g_lport <= 0 ) return FALSE;
	*p = g_lch;
	return TRUE;
}

/*
 * Told by the desktop at that channel about the program prog_id (its
 * id): when it is run again while its window is open (DT_HOW_AGAIN),
 * and what else the program's list follows (DT_RQ_TELL)
 */
void ta_listen( CONST char *prog_id )
{
	T_DTREQ	rq;
	T_DTANS	an;
	TS_UUID	ch;
	char	s[64];
	ER	er;

	if ( !ta_told_channel(&ch) ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_TELL;
	{
		char	t[TS_UUID_STRLEN + 1];

		ts_uuid_text(&ch, t);
		ta_req_part(DT_TAIL_NAME, (CONST UB *)prog_id, -1);
		ta_req_part(DT_TAIL_TEXT, (CONST UB *)t, -1);
	}
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "told of changes (%d)", (INT)er);
	note(s);
}

/* The window's title, as the name it shows (a list made, which is no object's record) */
void ta_title( CONST char *title )
{
	char	json[WM_TITLE_MAX * 2 + 16];
	INT	at = snprintf(json, sizeof(json), "{\"name\":\"");
	CONST UB *s;

	for ( s = (CONST UB *)title; *s != 0 && at < (INT)sizeof(json) - 4; s++ ) {
		if ( *s == '"' || *s == '\\' ) json[at++] = '\\';
		json[at++] = (char)*s;
	}
	at += snprintf(json + at, sizeof(json) - (size_t)at, "\"}");
	if ( g_kw > 0 ) (void)ob_set_atr(g_kw, (CONST UB *)json, (SZ)at);
}

BOOL ta_links_event( CONST T_OBNTM *m )
{
	CONST TS_UUID	devlist = OB_UUID_DEVLIST_INIT;

	/* a watched object's medium went: its key let go, and the window drawn again */
	if ( m->event == OB_E_DETACH && watch_gone(&m->uuid) ) {
		tv_src_forget(&m->uuid);
		g_dirty = TRUE;
		return TRUE;
	}
	/* a disk came or a volume was attached: the links watched again, the list let go */
	if ( g_devlist > 0 && memcmp(&m->uuid, &devlist, sizeof(TS_UUID)) == 0 ) {
		ta_watch(&g_obj);
		if ( g_has_parent ) ta_watch(&g_parent);
		ta_watch_links();
		if ( links_unwatched() == 0 ) {
			(void)ob_cls_obj(g_devlist);
			g_devlist = 0;
			note("links watched again");
		}
		g_dirty = TRUE;
		return TRUE;
	}
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

/* ---------------------------------------------------------------- pictures from the tray */

/* The number after the last '_' of a picture beside a record ("<object>_0_<n>.png") */
static INT pic_number( CONST UB *h )
{
	INT	j, v = 0, under = 0;

	for ( j = 0; h != NULL && h[j] != 0 && h[j] != '.'; j++ ) {
		if ( h[j] == '_' ) {
			under++;
			v = 0;
		} else if ( under == 2 && h[j] >= '0' && h[j] <= '9' ) {
			v = v * 10 + ( h[j] - '0' );
		}
	}
	return ( under == 2 ) ? v : 0;
}

static INT pic_top( CONST T_TAD *frag )
{
	CONST T_TAD	*docs[2];
	T_TADNODE	*n;
	INT		top = 0, k, v;

	docs[0] = g_rec;
	docs[1] = frag;
	for ( k = 0; k < 2; k++ ) {
		for ( n = tad_walk(docs[k], NULL); n != NULL; n = tad_walk(docs[k], n) ) {
			if ( n->kind != TAD_ND_ELEM ) continue;
			v = pic_number(tad_attr(n, "href"));
			if ( v > top ) top = v;
		}
	}
	return top;
}

/*
 * The pictures a fragment from the tray names by its set ("tray:N"),
 * written beside this object's record under names of their own past
 * every number used, and the fragment made to name them so: how many
 * (the figure editor's paste, the text editor's, a piece of text's)
 */
INT ta_tray_pictures( T_TAD *frag )
{
	T_TADNODE	*n;
	T_TRINF		inf;
	CONST UB	*h;
	char		us[TS_UUID_STRLEN + 1];
	UB		name[TS_UUID_STRLEN + 24], *png;
	SZ		asz = 0;
	INT		rn, i, done = 0;
	ID		k;

	ts_uuid_text(&g_obj, us);
	for ( n = tad_walk(frag, NULL); n != NULL; n = tad_walk(frag, n) ) {
		h = ( n->kind == TAD_ND_ELEM ) ? tad_attr(n, "href") : NULL;
		if ( h == NULL || strncmp((CONST char *)h, "tray:", 5) != 0 ) continue;
		for ( rn = 0, i = 5; h[i] >= '0' && h[i] <= '9'; i++ ) rn = rn * 10 + ( h[i] - '0' );
		if ( tr_ref_rec(TR_HAND, rn, &inf) < E_OK || inf.kind != TR_PNG || inf.size <= 0 ) continue;
		png = malloc((size_t)inf.size);
		if ( png == NULL ) continue;
		k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_W);
		if ( k > 0 && tr_rea_rec(TR_HAND, rn, png, inf.size, &asz) >= E_OK ) {
			snprintf((char *)name, sizeof(name), "%s_0_%d.png", us, pic_top(frag) + 1);
			if ( ob_wri_res(k, name + TS_UUID_STRLEN, png, asz) >= E_OK
			  && tad_set_attr(frag, n, "href", name) >= E_OK ) {
				char	s[TS_UUID_STRLEN + 40];

				snprintf(s, sizeof(s), "tray: picture %s", (char *)name);
				note(s);
				tv_src_forget(&g_obj);
				done++;
			}
		}
		if ( k > 0 ) ob_cls_obj(k);
		free(png);
	}
	return done;
}

/* ---------------------------------------------------------------- carried out of the window */

/*
 * What is carried out, as a fragment with its look, in the tray's place
 * of its own for the desktop to put in where it is let go (DT_F_FRAG);
 * FALSE when it could not be put there
 */
BOOL ta_frag_put( CONST T_TAD *frag )
{
	T_TRREC	r;
	UB	*buf;
	SZ	size = 0;
	ER	er;

	if ( frag == NULL ) return FALSE;
	(void)tad_write_mem(frag, NULL, 0, &size);
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return FALSE;
	if ( tad_write_mem(frag, buf, size + 1, &size) < E_OK ) {
		free(buf);
		return FALSE;
	}
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = buf;
	r.size = size;
	er = tr_set_dat(&r, 1);
	free(buf);
	return (BOOL)( er >= E_OK );
}

/* Where the hand let go back over this window (DT_F_BACK), in its work area */
BOOL ta_back_at( CONST T_DTANS *an, INT *p_x, INT *p_y )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return FALSE;
	*p_x = an->look.w - wp.wleft;
	*p_y = an->look.h - wp.wtop;
	return TRUE;
}

/*
 * Letters of the text e carried out of the window: the desktop follows
 * the hand on (DT_RQ_CARRY with DT_F_TEXT) and puts them where they are
 * let go -- a text of the desktop's as they looked (DT_F_FRAG), a figure
 * of the desktop's or another program's window as one line, line ends
 * and tabs as spaces. rq has the outline's look, the place the hand
 * holds it at and the window already. The answer is the editor's
 * (T_DDHOST text_away): 1 when they leave here, 0 when they stay (copied:
 * the second button on the way, or another program's window that was not
 * asked beforehand whether it takes them, and may yet refuse them; or let
 * go back over this window, where they move within it, DT_F_BACK), below
 * nought when the desktop put them nowhere.
 */
ER ta_text_away( T_DDED *e, T_DTREQ *rq, CONST UB *s, INT n )
{
	T_DTANS	an;
	T_TAD	*frag;
	INT	i, k;
	BOOL	put;
	ER	er;
	char	m[64];

	rq->req = DT_RQ_CARRY;
	rq->flags |= DT_F_TEXT;
	/* the letters, all of them, on one line */
	{
		UB	*l = (UB *)malloc((size_t)n + 1);

		k = 0;
		for ( i = 0; l != NULL && i < n; i++ ) l[k++] = ( s[i] == '\n' || s[i] == '\t' ) ? ' ' : s[i];
		ta_req_part(DT_TAIL_TEXT, l, k);
		free(l);
	}
	frag = de_sel_frag(e);
	put = ta_frag_put(frag);
	if ( frag != NULL ) tad_free(frag);
	if ( put ) rq->flags |= DT_F_FRAG;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(rq, &an, 30 * 60 * 1000);
	if ( put ) (void)tr_set_dat(NULL, 0);
	snprintf(m, sizeof(m), "letters carried out (%d)%s%s%s", (INT)er,
		 ( er >= E_OK && an.how == DT_HOW_COPY ) ? " as a copy" : "",
		 ( er >= E_OK && ( an.flags & DT_F_TO_PROG ) != 0 ) ? " to a program" : "",
		 ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) ? " back here" : "");
	note(m);
	if ( er < E_OK && ( an.flags & DT_F_BACK ) != 0 ) {
		/* back over this window: within it, where the hand let go */
		INT	x, y;

		if ( ta_back_at(&an, &x, &y) ) de_sel_back(e, x, y, (BOOL)( an.how == DT_HOW_COPY ));
		return 0;
	}
	if ( er < E_OK ) return er;	/* the desktop said so, when it was not over this window */
	return ( an.how == DT_HOW_COPY ) ? 0 : 1;
}

/*
 * The paper the window is drawn on: the object's own when its metadata
 * keeps one; else, opened from a link, that link's 表示領域背景 as it is
 * now (仮身属性と同じ); else the system's look
 */
static void paper_shown( void )
{
	if ( g_paper_own != TV_PAPER_LOOK ) {
		g_paper = g_paper_own;
	} else if ( g_link_bg != TAD_COL_NONE ) {
		g_paper = g_link_bg & 0x00FFFFFFU;
	} else {
		g_paper = TV_PAPER_LOOK;
	}
}

/* The object's paper, as the store has it */
void ta_paper_read( void )
{
	T_VOBJ	v = { .recno = 0 };
	UW	paper = TV_PAPER_LOOK;

	memset(&v, 0, sizeof(v));
	v.target = g_obj;
	v.disp = TAD_D_DEFAULT;
	if ( tv_src_look(&v, NULL, 0, &paper) < 0 ) paper = TV_PAPER_LOOK;
	g_paper_own = paper;
	paper_shown();
}

/*
 * The link it was opened from, read again from the record of the object
 * that holds it, as that record is stored now: FALSE when it is not
 * there (no longer linked, or pointing elsewhere), the link as known
 * before then standing
 */
static BOOL link_reread( void )
{
	UB	*x;
	SZ	n = 0;
	T_TAD	*rec = NULL;
	T_VOBJ	*v;
	BOOL	found = FALSE;

	if ( !g_has_parent ) return FALSE;
	x = tv_src_read(&g_parent, 0, &n);
	if ( x == NULL ) return FALSE;
	v = (T_VOBJ *)calloc(1, sizeof(T_VOBJ));
	if ( v != NULL && tad_parse(x, n, NULL, &rec) >= E_OK && rec != NULL
	  && tad_lnk_find(rec, &g_vobjid, v) >= E_OK && memcmp(&v->target, &g_obj, sizeof(TS_UUID)) == 0 ) {
		g_link_bg = v->bgcol;
		found = TRUE;
	}
	if ( rec != NULL ) tad_free(rec);
	if ( v != NULL ) tad_vobj_clear(v);
	free(v);
	free(x);
	return found;
}

/*
 * The paper as the window opens: the object's own, or the link's as the
 * desktop gave it (T_PRCOPEN, the link as the window holding it has it,
 * saved or not). Afterwards the link is followed two ways: the desktop
 * tells its new look when the window holding it changes it by 仮身属性変更
 * (DT_RQ_LINK_WATCH, link_look_told), and the object holding it is
 * watched, the link read again from its record when that is written
 * (ta_paper_changed), as when another program writes it.
 */
/*
 * Told by the desktop when the window holding the link changes it, saved
 * or not (DT_RQ_LINK_WATCH); asked again, the answer gives the look last
 * told since the window opened: TRUE with it in g_link_bg then
 */
static BOOL link_watch( CONST char *what )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	po[TS_UUID_STRLEN + 1], pv[TS_UUID_STRLEN + 1], pc[TS_UUID_STRLEN + 1];
	char	s[3 * ( TS_UUID_STRLEN + 1 ) + 64];
	ER	er;

	if ( !g_has_parent || g_lport <= 0 ) return FALSE;
	memset(&rq, 0, sizeof(rq));
	memset(&an, 0, sizeof(an));
	rq.req = DT_RQ_LINK_WATCH;
	rq.target = g_obj;
	rq.recno = -1;
	ts_uuid_text(&g_parent, po);
	ts_uuid_text(&g_vobjid, pv);
	ts_uuid_text(&g_lch, pc);
	snprintf(s, sizeof(s), "%s\t%s\t%s", po, pv, pc);
	ta_req_part(DT_TAIL_TEXT, (CONST UB *)s, -1);
	er = ask_desktop(&rq, &an, 3000);
	if ( er >= E_OK && an.how == DT_HOW_LINK_LOOK ) {
		g_link_bg = an.look.bgcol;
		snprintf(s, sizeof(s), "link %s (%d): bgcol #%06x", what, (INT)er, (UINT)( g_link_bg & 0x00FFFFFFU ));
	} else {
		snprintf(s, sizeof(s), "link %s (%d)", what, (INT)er);
	}
	note(s);
	return (BOOL)( er >= E_OK && an.how == DT_HOW_LINK_LOOK );
}

void ta_paper_open( void )
{
	ta_paper_read();
	if ( !g_has_parent ) return;
	ta_watch(&g_parent);
	(void)link_watch("watched");
}

/*
 * 再表示: the paper read again, the link with it as it is now -- as the
 * desktop was last told of it by the window holding it (saved or not),
 * else as the record holding it is stored -- not as it was when the
 * window opened
 */
void ta_paper_refresh( void )
{
	char	s[48];

	if ( g_has_parent && !link_watch("asked") && link_reread() ) {
		snprintf(s, sizeof(s), "link bgcol #%06x", (UINT)( g_link_bg & 0x00FFFFFFU ));
		note(s);
	}
	ta_paper_read();
}

/*
 * A notice that object u changed (OB_E_CHANGE): the paper read again when
 * u is the object itself (its metadata written by 背景色変更) or the one
 * whose record holds the link it was opened from. TRUE when the paper
 * shown is not what it was.
 */
BOOL ta_paper_changed( CONST TS_UUID *u )
{
	UW	was = g_paper;
	char	s[48];

	if ( memcmp(u, &g_obj, sizeof(TS_UUID)) == 0 ) ta_paper_read();
	if ( g_has_parent && memcmp(u, &g_parent, sizeof(TS_UUID)) == 0 && link_reread() ) {
		paper_shown();
		snprintf(s, sizeof(s), "link bgcol #%06x", (UINT)( g_link_bg & 0x00FFFFFFU ));
		note(s);
	}
	return (BOOL)( g_paper != was );
}

/* ---------------------------------------------------------------- starting and ending */

/* The object opened found: what its store says of it, and the links' store made ready */
static ER started( CONST T_OBREF *p_ref )
{
	UB	*whole = ui_obj_name(&g_obj);

	g_autoref = (BOOL)( ( p_ref->flags & OB_F_AUTOREF ) != 0 );
	if ( whole != NULL ) {
		(void)ta_set_name(whole);	/* no memory: the window has no name */
	}
	free(whole);
	tv_src_init();
	return E_OK;
}

CONST UB *ta_name( void )
{
	return g_name;
}

BOOL ta_set_name( CONST UB *name )
{
	size_t	n;
	UB	*c;

	if ( name == NULL ) name = (CONST UB *)"";
	n = strlen((CONST char *)name);
	c = (UB *)malloc(n + 1);
	if ( c == NULL ) return FALSE;
	memcpy(c, name, n + 1);
	if ( g_name_own ) free(g_name);
	g_name = c;
	g_name_own = TRUE;
	return TRUE;
}

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
		g_link_bg = a.fromlink ? a.bgcol : TAD_COL_NONE;
	}
	return started(p_ref);
}

ER ta_start_on( CONST T_TAHOST *host, CONST TS_UUID *obj, T_OBREF *p_ref )
{
	ta_host = host;
	g_obj = *obj;
	if ( ob_ref_obj(&g_obj, p_ref) < E_OK ) return E_NOEXS;
	return started(p_ref);
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
	if ( ta_host != NULL && ta_host->at_w > 0 && ta_host->at_h > 0 ) {
		*p_x = ta_host->at_x;
		*p_y = ta_host->at_y;
		*p_w = ta_host->at_w;
		*p_h = ta_host->at_h;
	}
	/* the program's own place, where none is kept: from the corner of the screen new windows open on */
	*p_x = wm_new_x(*p_x);
	*p_y = wm_new_y(*p_y);
	if ( k <= 0 ) return;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n > 0 && js_parse(m, n, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK
	  && js_get(&w, "pos", &pos) >= E_OK ) {
		INT	x = (INT)js_get_num(&pos, "x", -1), y = (INT)js_get_num(&pos, "y", -1);
		INT	ww = (INT)js_get_num(&w, "width", 0), wh = (INT)js_get_num(&w, "height", 0);
		UB	scr[12] = "";

		if ( ww > 0 && wh > 0 ) {
			/* kept from the corner of its screen, the main one's when it names none */
			(void)js_get_str(&w, "screen", scr, sizeof(scr));
			if ( x < 0 ) x = 0;
			if ( y < 0 ) y = 0;
			wm_place_xywh(&x, &y, ww, wh, (CONST char *)scr);
			*p_x = x;
			*p_y = y;
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
	c.name = ta_name();		/* the title, whole (the window manager shows what fits) */
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

/*
 * 全画面表示: the window made as large as the desktop makes it, or put
 * back. The desktop keeps where it stood and says which it did, for a
 * double press on the window's corner does the same; the place kept
 * here is only for saving where the window stands.
 */
void ta_fullscreen( void )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_OBWPOS	was;
	SZ		asz = 0;
	BOOL		read;

	read = ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &was, sizeof(was), &asz) >= E_OK );
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_FULL;
	if ( ask_desktop(&rq, &an, 5000) >= E_OK ) {
		g_full = (BOOL)( ( an.flags & DT_F_FULL ) != 0 && read );
		if ( g_full ) g_restore = was;
	}
	ta_work_size();
	g_dirty = TRUE;
}

/*
 * Whether the window is the screen's size by 全画面表示 now, for the tick
 * in the menu: the desktop is asked, for a double press on the window's
 * corner changes it without the program. Put back so, the place kept
 * here is no longer one to save.
 */
BOOL ta_full_shown( void )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_FULL;
	rq.flags = DT_F_ASK;
	if ( ask_desktop(&rq, &an, 1000) < E_OK ) return g_full;
	if ( ( an.flags & DT_F_FULL ) == 0 ) g_full = FALSE;
	return (BOOL)( ( an.flags & DT_F_FULL ) != 0 );
}

/*
 * 背景色変更: the object's paper, chosen in the desktop's panel and kept
 * in its metadata. 仮身属性と同じ (TAD_COL_NONE in the answer) leaves it
 * none of its own: laid on the link's 表示領域背景 as it is now when it
 * was opened from a link that gives one, else on the system's look
 * (paper_shown). The panel is asked with the object's own paper, not
 * the one shown, so that its switch says whether there is one.
 */
void ta_paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[48];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PAPER;
	rq.target = g_obj;
	rq.look.bgcol = ( g_paper_own == TV_PAPER_LOOK ) ? TAD_COL_NONE : g_paper_own;
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
		if ( an.look.bgcol == TAD_COL_NONE ) {
			g_paper_own = TV_PAPER_LOOK;
			paper_shown();
			if ( g_paper == TV_PAPER_LOOK ) {
				note("paper same as the link: the system's look");
			} else {
				snprintf(s, sizeof(s), "paper same as the link: #%06x", (UINT)g_paper);
				note(s);
			}
		} else {
			g_paper_own = an.look.bgcol & 0x00FFFFFFU;
			paper_shown();
			snprintf(s, sizeof(s), "paper #%06x", (UINT)g_paper);
			note(s);
		}
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
		ta_req_part(DT_TAIL_NAME, ta_name(), -1);
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

	char	t[2 * ( TS_UUID_STRLEN + 1 )];

	ts_uuid_text(&g_parent, p);
	ts_uuid_text(&g_vobjid, v);
	snprintf(t, sizeof(t), "%s\t%s", p, v);
	ta_req_part(DT_TAIL_TEXT, (CONST UB *)t, -1);
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
	if ( view != NULL && view[0] != 0 ) ta_req_part(DT_TAIL_VIEW, (CONST UB *)view, -1);
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

/*
 * The screen of the kept place written into the metadata's "window" (8
 * hex digits): in place of the one named, or put last where it names
 * none and the place is not the main screen's, so that a place of the
 * main screen is kept as it was before
 */
static BOOL win_screen_put( UB **p_m, INT *p_n, UW screen )
{
	T_JSON	root, w, at;
	UB	*nm;
	INT	n, max;
	char	txt[12];

	if ( js_parse(*p_m, *p_n, &root) < E_OK || js_get(&root, "window", &w) < E_OK ) return FALSE;
	if ( screen == 0 && js_get(&w, "screen", &at) < E_OK ) return TRUE;
	if ( screen != 0 ) snprintf(txt, sizeof(txt), "%08x", (UINT)screen);
	else txt[0] = 0;
	max = *p_n + 32;
	nm = malloc((size_t)max);
	if ( nm == NULL ) return FALSE;
	n = js_set_str(*p_m, *p_n, &w, "screen", txt, nm, max);
	if ( n < 0 ) {
		free(nm);
		return FALSE;
	}
	free(*p_m);
	*p_m = nm;
	*p_n = n;
	return TRUE;
}

/*
 * Where the window stood and how large it was, into the object's
 * metadata, for it to open there next time: from the corner of the
 * screen its top left corner is on, and which screen that is (none for
 * the main one), as wm_place_keep makes it
 */
void ta_window_keep( void )
{
	T_OBWPOS	wp;
	T_DPRECT	view;
	T_WMPLACE	p;
	SZ		asz = 0;
	UB		*m = NULL;
	INT		n;
	ID		k;
	BOOL		any;
	ER		er = E_NOEXS;
	char		s[80];

	if ( g_full ) {
		wp = g_restore;			/* 全画面表示 is not where it stands */
	} else if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		return;
	}
	view.left = wp.left;
	view.top = wp.top;
	view.right = wp.right;
	view.bottom = wp.bottom;
	if ( wm_place_keep(&view, &p) < E_OK ) {
		p.r = view;
		p.screen = 0;
	}
	k = ob_opn_obj(&g_obj, OB_OP_ATRRD | OB_OP_ATRWR);
	if ( k <= 0 ) return;
	n = ob_atr_read(k, 0, &m);
	if ( n > 0 ) {
		any = win_num(&m, &n, "x", TRUE, p.r.left);
		any = win_num(&m, &n, "y", TRUE, p.r.top) || any;
		any = win_num(&m, &n, "width", FALSE, p.r.right - p.r.left) || any;
		any = win_num(&m, &n, "height", FALSE, p.r.bottom - p.r.top) || any;
		if ( any ) (void)win_screen_put(&m, &n, p.screen);
		if ( any ) er = ob_set_atr(k, m, (SZ)n);
	}
	free(m);
	ob_cls_obj(k);
	if ( p.screen != 0 ) {
		snprintf(s, sizeof(s), "window kept %d,%d %dx%d on screen %08x (%d)", p.r.left, p.r.top, p.r.right - p.r.left,
			 p.r.bottom - p.r.top, (UINT)p.screen, (INT)er);
	} else {
		snprintf(s, sizeof(s), "window kept %d,%d %dx%d (%d)", p.r.left, p.r.top, p.r.right - p.r.left,
			 p.r.bottom - p.r.top, (INT)er);
	}
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
	ta_req_part(DT_TAIL_NAME, (CONST UB *)"屑実身操作", -1);
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
		SZ	tl = 0;
		CONST UB *tx;

		memset(&rq, 0, sizeof(rq));
		memset(&an, 0, sizeof(an));
		rq.req = DT_RQ_APPS;
		rq.target = v->target;
		rq.recno = k;
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		/* "id\tname", the answer's text */
		tx = ta_ans_part(DT_TAIL_TEXT, &tl);
		if ( tx == NULL ) continue;
		for ( j = 0; i < tl && tx[i] != '\t'; i++ ) {
			if ( j < APP_ID - 1 ) g_app_id[g_napp][j++] = tx[i];
		}
		g_app_id[g_napp][j] = 0;
		if ( j == 0 || i >= tl || tx[i] != '\t' ) continue;
		for ( i++, j = 0; i < tl; i++ ) {
			if ( j < APP_NAME - 1 ) g_app_name[g_napp][j++] = tx[i];
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
	char	s[TA_LOG];

	if ( index < 0 || index >= g_napp ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_EXEC;
	rq.target = v->target;
	rq.recno = -1;
	ta_req_part(DT_TAIL_NAME, g_app_id[index], -1);
	if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
	er = ask_desktop(&rq, &an, 5000);
	snprintf(s, sizeof(s), "exec %.80s %s (%d)", (char *)TAD_VS(v->vname), (char *)g_app_id[index], (INT)er);
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
 * The data box's panels are asked with the window's channel; before the
 * window is open (実身/仮身検索 asks first), with a channel of their own,
 * given back once the panel is put away. A panel of another data box
 * than the program's (box) is asked the same way.
 */
static UI	dlg_with;
static TS_UUID	dlg_own_ch;
static ID	dlg_own_port = 0;

ER ta_dlg_open( CONST char *box, INT num, UI *q )
{
	dlg_with = g_ui;
	if ( box != NULL && ui_uuid(box, &dlg_with.box) < E_OK ) return E_NOEXS;
	if ( dlg_with.port <= 0 ) {
		if ( dlg_own_port <= 0 ) {
			T_OBCRE	c;

			memset(&c, 0, sizeof(c));
			c.type = OB_T_CHANNEL;
			if ( ob_cre_obj(&c, &dlg_own_ch) >= E_OK ) {
				dlg_own_port = ob_opn_obj(&dlg_own_ch, OB_OP_READ | OB_OP_WRITE);
			}
		}
		dlg_with.port = dlg_own_port;
	}
	if ( dlg_with.port <= 0 ) return E_NOEXS;
	return ui_dlg_open(&dlg_with, num, q);
}

/*
 * A panel worked until one of its buttons is pressed or it is put away:
 * the number of the button, 'enter' for Enter, 0 for 取消 (button 101)
 * and for a panel put away
 */
INT ta_dlg_run( UI *q, INT enter )
{
	T_OBNTM	m;
	INT	num = 0, ev, how = -1;

	while ( how < 0 ) {
		ev = ( ui_wait(&dlg_with, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(q, &m, &num) : UI_EV_CLOSE;
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

void ta_dlg_close( UI *q )
{
	ui_dlg_close(q);
	if ( dlg_own_port > 0 ) {
		ob_cls_obj(dlg_own_port);
		(void)ob_del_obj(&dlg_own_ch);
		dlg_own_port = 0;
	}
	g_dirty = TRUE;
}

void ta_tell_in( CONST char *box, INT num, CONST char *l1, CONST char *l2 )
{
	UI	q;

	if ( ta_dlg_open(box, num, &q) < E_OK ) return;
	ui_set_text(&q, 1, l1);
	ui_set_text(&q, 2, ( l2 != NULL ) ? l2 : " ");
	note("tell panel open");
	(void)ta_dlg_run(&q, TA_P_OK);
	ta_dlg_close(&q);
	note("tell panel put away");
}

BOOL ta_ask_in( CONST char *box, INT num, CONST char *l1, CONST char *l2, CONST char *no, CONST char *yes )
{
	UI	q;
	INT	how;

	if ( ta_dlg_open(box, num, &q) < E_OK ) return FALSE;
	ui_set_text(&q, 1, l1);
	ui_set_text(&q, 2, ( l2 != NULL ) ? l2 : " ");
	ui_set_text(&q, TA_P_CANCEL, no);
	ui_set_text(&q, TA_P_OK, yes);
	note("ask panel open");
	how = ta_dlg_run(&q, TA_P_OK);
	ta_dlg_close(&q);
	note(( how == TA_P_OK ) ? "ask panel answered yes" : "ask panel answered no");
	return (BOOL)( how == TA_P_OK );
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
	if ( ta_dlg_open(NULL, TA_PN_NAME, &q) < E_OK ) return FALSE;
	ui_set_text(&q, 1, prompt);
	ui_set_text(&q, 2, ( start != NULL ) ? (CONST char *)start : "");
	ui_focus(&q, 2);
	note("name panel open");
	how = ta_dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) (void)ui_text(&q, 2, (char *)out, max);
	ta_dlg_close(&q);
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
	if ( ta_dlg_open(NULL, TA_PN_TEXT, &q) < E_OK ) return FALSE;
	snprintf(say, sizeof(say), "%s%s", ( p1 != NULL ) ? p1 : "", ( p2 != NULL ) ? p2 : "");
	ui_set_text(&q, 1, say);
	ui_set_text(&q, 2, ( start != NULL ) ? (CONST char *)start : "");
	if ( ok != NULL && strcmp(ok, "設定") != 0 ) ui_set_text(&q, TA_P_OK, ok);	/* the panel says 設定 */
	ui_focus(&q, 2);
	note("text panel open");
	how = ta_dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) (void)ui_text(&q, 2, (char *)out, max);
	ta_dlg_close(&q);
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

	if ( n <= 0 || n > 9 || ta_dlg_open(NULL, ( rows == 6 ) ? TA_PN_FIELDS6 : TA_PN_FIELDS9, &q) < E_OK ) {
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
	how = ta_dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) {
		for ( i = 0; i < n; i++ ) (void)ui_text(&q, 1 + i, (char *)vals[i], DE_FIELD_MAX);
	}
	ta_dlg_close(&q);
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

	if ( n <= 0 || ta_dlg_open(NULL, TA_PN_LIST, &q) < E_OK ) return -1;
	ui_set_text(&q, TA_P_TITLE, title);
	ui_names(&q, 1, names, n);
	ui_set_value(&q, 1, ( start >= 0 && start < n ) ? start + 1 : 0);
	note("list panel open");
	how = ta_dlg_run(&q, TA_P_OK);
	if ( how == TA_P_OK ) v = ui_value(&q, 1);
	ta_dlg_close(&q);
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

	if ( ta_dlg_open(NULL, TA_PN_FIND, &q) < E_OK ) return 0;
	ui_set_text(&q, P_FIND_WHAT, (CONST char *)what);
	ui_set_text(&q, P_FIND_WITH, (CONST char *)with);
	ui_set_value(&q, P_FIND_REGEX, *p_regex ? 1 : 0);
	ui_focus(&q, P_FIND_WHAT);
	note("find panel open");
	how = ta_dlg_run(&q, TA_P_OK);
	how = ( how == TA_P_OK ) ? DE_FIND_NEXT : ( how == P_FIND_REPLACE ) ? DE_FIND_REPLACE
	    : ( how == P_FIND_ALL ) ? DE_FIND_ALL : 0;
	if ( how != 0 ) {
		(void)ui_text(&q, P_FIND_WHAT, (char *)what, max);
		(void)ui_text(&q, P_FIND_WITH, (char *)with, max);
		*p_regex = (BOOL)( ui_value(&q, P_FIND_REGEX) != 0 );
	}
	ta_dlg_close(&q);
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
	if ( ta_dlg_open(NULL, TA_PN_PAPER, &q) < E_OK ) return DE_PAPER_CANCEL;
	ui_set_value(&q, PS_LIST, start + 1);
	for ( i = 0; i < 6; i++ ) {
		tenths_text(*to[i], t, sizeof(t));
		ui_set_text(&q, PS_H + i, t);
	}
	ui_set_value(&q, ( pp->imposition != 0 ) ? PS_SPREAD : PS_ONE, 1);
	note("paper panel open");
	how = ta_dlg_run(&q, TA_P_OK);
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
	ta_dlg_close(&q);
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
