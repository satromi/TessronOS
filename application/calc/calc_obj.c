/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_obj.c
 *	基本表計算: the object, its record, the desktop and the tray
 *	(design 11.13)
 *
 *	The record is read once when the program starts and written when the
 *	person saves. The objects the links of the sheet point at are
 *	counted when it is written: a link that came since the last writing
 *	counts its object once more, one that went once less, unless the
 *	store counts the links of its records itself.
 *
 *	The 仮身操作 and 実身操作 of a link in a cell are the desktop's,
 *	asked for (ts/dtreq.h); what it answers the link is to become is
 *	written into the cell. Links dropped on the window are put in the
 *	cells from the one they were let go over to the right, one a cell;
 *	a link carried out of the window is put where it is let go.
 *
 *	The tray: cells copied go to it as text (their words, a tab between
 *	the columns and a paragraph a row), or, when they hold links, as a
 *	figure of the links. The program keeps the cells themselves too --
 *	their formulas, their look -- and pastes them back as they were while
 *	what it put in the tray is still the set in hand; anything else in
 *	hand is pasted as its words in one cell, or its links in a row.
 */

#include "calc.h"
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/hid.h>
#include <ts/tray.h>

/* ---------------------------------------------------------------- records */

static UB *rec_read( CONST TS_UUID *u, INT recno, INT *p_len )
{
	UB	*buf = NULL;
	INT	len = ui_rec_all(u, recno, 1, &buf);

	if ( len < 0 || buf == NULL ) {
		free(buf);
		*p_len = 0;
		return NULL;
	}
	buf[len] = 0;
	*p_len = len;
	return buf;
}

static ER rec_write( CONST TS_UUID *u, CONST UB *data, INT len )
{
	ID	k = ob_opn_obj(u, OB_OP_R | OB_OP_W);
	ER	er;

	if ( k <= 0 ) return (ER)k;
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(k, 0, data, (SZ)len);
	ob_cls_obj(k);
	return er;
}

void name_of( CONST TS_UUID *u, UB *out )
{
	T_OBREF	ref;

	out[0] = 0;
	if ( ob_ref_obj(u, &ref) >= E_OK ) {
		memcpy(out, ref.name, OB_NAME_MAX);
		out[OB_NAME_MAX - 1] = 0;
	}
}

INT meta_of( CONST TS_UUID *u )
{
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	/* the metadata whole, in memory of its own: the last one read let go */
	if ( g_meta != g_meta_none ) free(g_meta);
	g_meta = g_meta_none;
	if ( k <= 0 ) return 0;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n <= 0 ) {
		free(m);
		return 0;
	}
	g_meta = (char *)m;
	return n;
}

void new_vobjid( TS_UUID *u )
{
	static UB	seq;

	memset(u, 0, sizeof(*u));
	if ( ts_gen_uuid(u) < E_OK ) {
		*u = g_obj;
		u->b[15] ^= ++seq;
		u->b[14] ^= 0x3C;
	}
}

BOOL link_target( CONST CXCELL *c, TS_UUID *u )
{
	return (BOOL)( c != NULL && c->kind == CX_LINK && c->el != NULL && ct_target(c->el, c->ellen, u) );
}

/* ---------------------------------------------------------------- the engine */

static void taken_one( int col, int row, const unsigned char *s, int len, void *arg )
{
	CXCELL	*c = cx_get(&g_s, col, row);

	(void)arg;
	if ( c != NULL && c->kind == CX_TEXT ) (void)cx_set_shown(c, s, len);
}

void shown_all( void )
{
	(void)ce_taken(taken_one, NULL);
	g_dirty = TRUE;
}

/*
 * Many cells being put in at once (a paste): each is not told on the
 * console, nor are the words shown taken one cell at a time; that is
 * done once when they are all in.
 */
static BOOL	g_many = FALSE;

void cell_text( INT col, INT row, CONST UB *text )
{
	CXCELL	*c = cx_get(&g_s, col, row);

	/* a link or an image is not lost to an empty typing */
	if ( c != NULL && c->kind != CX_TEXT && text[0] == 0 ) return;
	c = cx_make(&g_s, col, row);
	if ( c == NULL || !cx_set_text(c, text) ) {
		say("メモリが足りません");
		return;
	}
	cx_grow(&g_s, col, row);
	(void)ce_set(col, row, text);
	g_changed = TRUE;
	if ( g_many ) {
		return;
	}
	shown_all();
	{
		char	s[200], nm[16];

		(void)cx_cell_name(col, row, nm);
		snprintf(s, sizeof(s), "set %s shows %.80s", nm, (CONST char *)cx_shown(c));
		note(s);
	}
}

void cell_clear( INT col, INT row )
{
	cx_remove(&g_s, col, row);
	(void)ce_clear(col, row);
	shown_all();
	g_changed = TRUE;
}

/* ---------------------------------------------------------------- the links counted */

static TS_UUID	*g_was;			/* the objects the record's links pointed at when last written */
static INT	g_nwas;

/* The objects the sheet's links point at now, one for each link */
static INT links_now( TS_UUID **p )
{
	INT	i, n = 0;
	TS_UUID	*u = malloc(sizeof(TS_UUID) * (size_t)( g_s.n + 1 ));

	*p = u;
	if ( u == NULL ) return 0;
	for ( i = 0; i < g_s.n; i++ ) {
		if ( link_target(g_s.c[i], &u[n]) ) n++;
	}
	return n;
}

/* Each object counted once more for a link that came, once less for one that went */
static void links_count( void )
{
	TS_UUID	*now;
	INT	n = links_now(&now), i, k;
	BOOL	*used = calloc((size_t)( g_nwas + 1 ), sizeof(BOOL));

	if ( !g_autoref && now != NULL && used != NULL ) {
		for ( i = 0; i < n; i++ ) {
			for ( k = 0; k < g_nwas && ( used[k] || memcmp(&g_was[k], &now[i], sizeof(TS_UUID)) != 0 ); k++ ) ;
			if ( k < g_nwas ) used[k] = TRUE;
			else (void)ob_lnk_obj(&now[i]);
		}
		for ( k = 0; k < g_nwas; k++ ) {
			if ( !used[k] ) (void)ob_unl_obj(&g_was[k]);
		}
	}
	free(used);
	free(g_was);
	g_was = now;
	g_nwas = n;
}

/* ---------------------------------------------------------------- reading and writing */

ER load( void )
{
	UB	*x;
	INT	n = 0, i;
	ER	er;

	x = rec_read(&g_obj, 0, &n);
	if ( x == NULL ) return E_NOEXS;
	cx_free_all(&g_s);
	(void)ce_reset();
	er = cx_read(&g_s, x, n);
	free(x);
	if ( er < E_OK ) return er;
	/* the cells' text given all at once, and the formulas worked out together */
	(void)ce_bulk(1);
	for ( i = 0; i < g_s.n; i++ ) {
		CXCELL	*c = g_s.c[i];

		if ( c->kind == CX_TEXT ) (void)ce_set(c->col, c->row, c->text);
	}
	(void)ce_bulk(0);
	shown_all();
	free(g_was);
	g_nwas = links_now(&g_was);
	g_changed = FALSE;
	return E_OK;
}

ER save( void )
{
	CTBUF	b;
	ER	er;

	/* written out as long as it comes to */
	ct_buf_grow(&b, 256 * 1024, ct_more);
	cx_write(&b, &g_s, g_name, chart_xml, NULL);
	if ( b.fail ) {
		(void)ct_more(b.p, 0);
		return E_NOMEM;
	}
	er = rec_write(&g_obj, b.p, b.n);
	(void)ct_more(b.p, 0);
	if ( er >= E_OK ) {
		links_count();
		g_changed = FALSE;
	}
	return er;
}

/* ---------------------------------------------------------------- the desktop */

/* What came while the desktop was asked: the presses and keys were the desktop's */
static void settle( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( !g_closing && ob_rea_rec(g_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( ( m.event & ( OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_ENTER
				 | OB_E_LEAVE | OB_E_WHEEL ) ) != 0 ) {
			continue;
		}
		event(&m);
	}
}

/* A page of the sheet into the object the desktop prints from */
static ER prn_put( void *arg, CONST UB *xml, INT n )
{
	return ui_prn_page((T_UIPRN *)arg, xml, n);
}

/*
 * 印刷: the sheet made pages (calc_print.c) in an object the desktop is
 * asked to print (DT_RQ_PRINT); its panels say how, and the file it
 * writes goes on the desktop.
 */
ER print_sheet( void )
{
	T_UIPRN		up;
	CXPRINT		pr;
	INT		n;
	ER		er;
	char		line[96];

	er = ui_prn_open(&up, &g_obj);
	if ( er < E_OK ) {
		return er;
	}
	memset(&pr, 0, sizeof(pr));
	pr.put = prn_put;
	pr.arg = &up;
	pr.chart = chart_print;
	pr.more = ct_more;
	n = cx_print(&g_s, &pr);
	er = ( n < 0 ) ? (ER)n : ui_prn_ready(&up);
	snprintf(line, sizeof(line), "print %d pages (%d)", n, (INT)er);
	note(line);
	if ( n == 0 ) {
		say("印刷するセルがありません");
	} else if ( er >= E_OK ) {
		T_DTREQ	rq;
		T_DTANS	an;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_name);
		/* the person works the desktop's panels: as long as that takes */
		er = ask_desktop(&rq, &an, 60 * 60 * 1000);
		snprintf(line, sizeof(line), "print answered %d", (INT)er);
		note(line);
	}
	ui_prn_close(&up);
	return er;
}

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
	rq->win = g_win;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	/* the answer waited for on its channel, at most wait_ms */
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	settle();
	return er;
}

/* The link in the cell in hand, and its element */
static CXCELL *the_link( void )
{
	CXCELL	*c = cx_get(&g_s, g_col, g_row);

	return ( c != NULL && c->kind == CX_LINK && c->el != NULL ) ? c : NULL;
}

static UINT col_attr( CONST CXCELL *c, CONST char *name )
{
	UB	col[16];

	if ( ct_attr(c->el, c->ellen, name, col, sizeof(col)) == 7 && col[0] == '#' ) {
		return (UINT)strtoul((char *)col + 1, NULL, 16);
	}
	return 0xFFFFFFFFU;
}

static CONST char *CONST disp_attr[6] = { "namedisp", "roledisp", "typedisp", "updatedisp", "framedisp", "pictdisp" };

static void ask_of( CONST CXCELL *c, UINT req, T_DTREQ *rq )
{
	T_DPRECT	r;
	INT		i;

	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	(void)link_target(c, &rq->target);
	rq->recno = -1;
	link_box(c, &r);
	rq->look.w = r.right - r.left;
	rq->look.h = r.bottom - r.top;
	rq->look.chsz = ct_attr_int(c->el, c->ellen, "chsz", 0);
	rq->look.frcol = col_attr(c, "frcol");
	rq->look.chcol = col_attr(c, "chcol");
	rq->look.tbcol = col_attr(c, "tbcol");
	rq->look.bgcol = col_attr(c, "bgcol");
	for ( i = 0; i < 6; i++ ) {
		if ( ct_attr_bool(c->el, c->ellen, disp_attr[i], (BOOL)( i == 0 || i == 5 )) ) rq->look.disp |= 1U << i;
	}
	rq->look.autoopen = ct_attr_bool(c->el, c->ellen, "autoopen", FALSE) ? 1 : 0;
	if ( ct_attr_bool(c->el, c->ellen, "hidden", FALSE) ) rq->flags |= DT_F_HIDDEN;
}

void link_open( INT col, INT row )
{
	CXCELL	*c = cx_get(&g_s, col, row);
	T_DTREQ	rq;
	T_DTANS	an;
	UB	nm[OB_NAME_MAX];
	char	s[120];
	ER	er;

	if ( c == NULL || c->kind != CX_LINK ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	if ( !link_target(c, &rq.target) ) return;
	rq.recno = -1;
	ct_link_req(&rq, c->el, c->ellen);	/* the program it keeps to, when it does */
	er = ask_desktop(&rq, &an, 3000);
	name_of(&rq.target, nm);
	snprintf(s, sizeof(s), "opened %s (%d)", nm, (INT)er);
	note(s);
	if ( er < E_OK ) say("開けませんでした");
}

/* The cell's link with attributes set (a value of NULL takes one off) */
static BOOL link_edit( CXCELL *c, CONST CTSET *set, INT n )
{
	INT	max = c->ellen + 512, k;
	UB	*tmp;
	CTBUF	b;
	BOOL	ok;

	for ( k = 0; k < n; k++ ) {
		if ( set[k].value != NULL ) max += 6 * (INT)strlen((CONST char *)set[k].value);
	}
	tmp = malloc((size_t)max);
	if ( tmp == NULL ) return FALSE;
	ct_buf(&b, tmp, max);
	ct_link_edit(&b, c->el, c->ellen, set, n);
	ok = (BOOL)( !b.fail && cx_set_el(c, CX_LINK, b.p, b.n) );
	free(tmp);
	if ( ok ) g_changed = TRUE;
	return ok;
}

/* The applist of the link: its own, or its object's */
static INT app_json( CONST CXCELL *c, UB **p_out )
{
	T_JSON	root, al;
	TS_UUID	u;
	INT	n;

	/* the link's own: no longer than the link it is written in */
	*p_out = malloc((size_t)c->ellen + 1);
	if ( *p_out == NULL ) return 0;
	n = ct_attr_text(c->el, c->ellen, "applist", *p_out, c->ellen + 1);
	if ( n > 0 ) return n;
	if ( !link_target(c, &u) ) return 0;
	n = meta_of(&u);
	if ( n <= 0 || js_parse((CONST UB *)g_meta, n, &root) < E_OK || js_get(&root, "applist", &al) < E_OK ) {
		(*p_out)[0] = 0;
		return 0;
	}
	free(*p_out);
	*p_out = malloc((size_t)al.len + 1);
	if ( *p_out == NULL ) return 0;
	memcpy(*p_out, al.s, (size_t)al.len);
	(*p_out)[al.len] = 0;
	return al.len;
}

#define APP_MAX		24
static INT	g_napp;
static UB	g_app_id[APP_MAX][CT_APP_ID], g_app_name[APP_MAX][CT_APP_NAME];
static BOOL	g_app_kind[APP_MAX];		/* offered for the object's kind, not in its applist */

/* 実行: the list the desktop's own links show (DT_RQ_APPS), asked entry by entry */
void exec_menu( ID mid )
{
	static UB	rows[APP_MAX][WM_LABEL_MAX];
	CXCELL		*c = the_link();
	TS_UUID		ed;
	T_DTREQ		rq;
	T_DTANS		an;
	INT		n = 1, k;

	g_napp = 0;
	for ( k = 0; c != NULL && k < n && g_napp < APP_MAX; k++ ) {
		ask_of(c, DT_RQ_APPS, &rq);
		ct_link_req(&rq, c->el, c->ellen);
		rq.recno = k;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		if ( ct_app_entry(&an, g_app_id[g_napp], g_app_name[g_napp], &g_app_kind[g_napp]) ) g_napp++;
	}
	for ( k = 0; k < g_napp; k++ ) snprintf((char *)rows[k], WM_LABEL_MAX, "%s", (char *)g_app_name[k]);
	if ( ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, g_napp);
	}
	(void)mn_chg_atr(mid, NULL, "exec", ( c == NULL || g_napp == 0 ) ? MN_GREY : 0);
}

BOOL app_kept( void )
{
	CXCELL	*c = the_link();
	UB	*j = NULL;
	INT	n;
	BOOL	kept = FALSE;

	if ( c != NULL ) {
		n = app_json(c, &j);
		kept = ( n > 0 ) ? ct_app_locked(j, n) : FALSE;
	}
	free(j);
	return kept;
}

static void app_keep( CXCELL *c )
{
	UB	*j = NULL, *out = NULL;
	CTBUF	b;
	CTSET	set;
	INT	n;
	BOOL	keep;

	n = app_json(c, &j);
	if ( j != NULL ) out = malloc((size_t)n + 64);	/* with "defaultOpenLock" put in */
	if ( j != NULL && out != NULL ) {
		keep = !ct_app_locked(j, n);
		ct_buf(&b, out, n + 64);
		set.name = "applist";
		set.value = b.p;
		if ( n > 0 && ct_app_lock(&b, j, n, keep) >= E_OK && link_edit(c, &set, 1) ) {
			note(keep ? "kept" : "let go");
			say(keep ? "起動アプリを固定しました" : "起動アプリを最新にしました");
		} else {
			say("起動アプリを持たない実身です");
		}
	}
	free(j);
	free(out);
}

/* The look the desktop's 属性変更 answered, written into the link */
static void link_look( CXCELL *c, CONST T_DTANS *an )
{
	char	col[4][8], cs[12];
	CTSET	set[16];
	CONST UINT *v[4];
	static CONST char *CONST cn[4] = { "frcol", "chcol", "tbcol", "bgcol" };
	INT	n = 0, k;

	v[0] = &an->look.frcol;
	v[1] = &an->look.chcol;
	v[2] = &an->look.tbcol;
	v[3] = &an->look.bgcol;
	if ( an->look.chsz > 0 ) {
		snprintf(cs, sizeof(cs), "%d", an->look.chsz);
		set[n].name = "chsz";
		set[n++].value = (CONST UB *)cs;
	}
	for ( k = 0; k < 4; k++ ) {
		if ( *v[k] == 0xFFFFFFFFU ) continue;
		snprintf(col[k], sizeof(col[k]), "#%06x", *v[k] & 0x00FFFFFFU);
		set[n].name = cn[k];
		set[n++].value = (CONST UB *)col[k];
	}
	for ( k = 0; k < 6; k++ ) {
		set[n].name = disp_attr[k];
		set[n++].value = (CONST UB *)( ( ( an->look.disp >> k ) & 1 ) ? "true" : "false" );
	}
	set[n].name = "autoopen";
	set[n++].value = (CONST UB *)( an->look.autoopen ? "true" : "false" );
	set[n].name = "hidden";
	set[n++].value = (CONST UB *)( ( an->flags & DT_F_HIDDEN ) ? "true" : "false" );
	(void)link_edit(c, set, n);
}

/* A link to a new object put in the first empty cell under the link in hand */
static void link_below( CXCELL *c, CONST TS_UUID *nid )
{
	CTSET	set[3];
	char	ids[TS_UUID_STRLEN + 8], vs[TS_UUID_STRLEN + 1];
	TS_UUID	vid;
	INT	row, max = c->ellen + 256;
	UB	*tmp = malloc((size_t)max);
	CTBUF	b;

	if ( tmp == NULL ) return;
	for ( row = c->row + 1; row <= CX_ROWS_MAX; row++ ) {
		CXCELL	*d = cx_get(&g_s, c->col, row);

		if ( cx_merge_of(&g_s, c->col, row) >= 0 ) continue;
		if ( d == NULL || ( d->kind == CX_TEXT && d->text[0] == 0 ) ) break;
	}
	ct_uuid_str(nid, ids);
	strcat(ids, "_0.xtad");
	new_vobjid(&vid);
	ct_uuid_str(&vid, vs);
	set[0].name = "id";
	set[0].value = (CONST UB *)ids;
	set[1].name = "vobjid";
	set[1].value = (CONST UB *)vs;
	set[2].name = "name";
	set[2].value = NULL;
	ct_buf(&b, tmp, max);
	ct_link_edit(&b, c->el, c->ellen, set, 3);
	if ( !b.fail && row <= CX_ROWS_MAX ) {
		CXCELL	*d = cx_make(&g_s, c->col, row);

		if ( d != NULL && cx_set_el(d, CX_LINK, b.p, b.n) ) {
			(void)ce_blank(c->col, row);
			cx_grow(&g_s, c->col, row);
			g_row = g_arow = row;
			g_changed = TRUE;
		}
	}
	free(tmp);
}

void link_op( CONST char *code, INT index )
{
	CXCELL	*c = the_link();
	T_DTREQ	rq;
	T_DTANS	an;
	UB	nm[OB_NAME_MAX];
	char	s[160];
	TS_UUID	u;
	ER	er;

	if ( c == NULL || !link_target(c, &u) ) return;
	name_of(&u, nm);
	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		link_open(g_col, g_row);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		ask_of(c, DT_RQ_CLOSE, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		ask_of(c, DT_RQ_ATTR, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			link_look(c, &an);
			snprintf(s, sizeof(s), "attr %s", nm);
			note(s);
			say("仮身属性を変更しました");
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		ask_of(c, DT_RQ_RELATION, &rq);
		(void)ct_attr_text(c->el, c->ellen, "relationship", rq.name, sizeof(rq.name));
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			CTSET	set;

			an.text[DT_TEXT_MAX - 1] = 0;
			set.name = "relationship";
			set.value = ( an.text[0] != 0 ) ? an.text : NULL;
			(void)link_edit(c, &set, 1);
			snprintf(s, sizeof(s), "relation %s [%s]", nm, (char *)an.text);
			note(s);
			say("続柄を変更しました");
		}
	} else if ( strcmp(code, "vobj.reconnect") == 0 ) {
		T_OBREF	ref;

		if ( ob_ref_obj(&u, &ref) < E_OK ) say("実身に接続できませんでした");
	} else if ( strcmp(code, "real.rename") == 0 ) {
		ask_of(c, DT_RQ_RENAME, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			an.text[DT_TEXT_MAX - 1] = 0;
			snprintf(s, sizeof(s), "renamed %s", (char *)an.text);
			note(s);
		}
	} else if ( strcmp(code, "real.duplicate") == 0 || strcmp(code, "real.newversion") == 0 ) {
		BOOL	dup = (BOOL)( code[5] == 'd' );

		ask_of(c, dup ? DT_RQ_DUPLICATE : DT_RQ_NEWVER, &rq);
		er = ask_desktop(&rq, &an, 30 * 60 * 1000);
		if ( er >= E_OK ) {
			link_below(c, &an.into);
			/* the new object is counted already: it is one of the links written */
			if ( !g_autoref ) {
				TS_UUID	*w = realloc(g_was, sizeof(TS_UUID) * (size_t)( g_nwas + 1 ));

				if ( w != NULL ) {
					g_was = w;
					g_was[g_nwas++] = an.into;
				}
			}
			snprintf(s, sizeof(s), "%s %s", dup ? "duplicated" : "new version", nm);
			note(s);
			say(dup ? "実身を複製しました" : "新版を作成しました");
		}
	} else if ( strcmp(code, "real.lock") == 0 || strcmp(code, "real.unlock") == 0 ) {
		app_keep(c);
	} else if ( strcmp(code, "real.info") == 0 ) {
		ask_of(c, DT_RQ_INFO, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.network") == 0 ) {
		ask_of(c, DT_RQ_NETWORK, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.search") == 0 ) {
		ask_of(c, DT_RQ_SEARCH, &rq);
		(void)ask_desktop(&rq, &an, 30 * 60 * 1000);
	} else if ( strcmp(code, "exec.app") == 0 ) {
		if ( index < 0 || index >= g_napp ) return;
		ask_of(c, DT_RQ_EXEC, &rq);
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_app_id[index]);
		if ( app_kept() ) rq.flags |= DT_F_KEEP;
		if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
		er = ask_desktop(&rq, &an, 5000);
		snprintf(s, sizeof(s), "exec %s %s (%d)", nm, (char *)g_app_id[index], (INT)er);
		note(s);
	}
	g_dirty = TRUE;
}

/* 背景色変更: the paper of the sheet's object */
void paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[40];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PAPER;
	rq.target = g_obj;
	rq.look.bgcol = g_bg;
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) < E_OK ) return;
	g_bg = an.look.bgcol & 0x00FFFFFFU;
	snprintf(s, sizeof(s), "paper #%06x", (UINT)g_bg);
	note(s);
	say("背景色を変更しました");
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- links put in */

static void hex_col( char *out, UINT c, CONST char *dflt )
{
	if ( c == 0xFFFFFFFFU ) strcpy(out, dflt);
	else snprintf(out, 8, "#%06x", c & 0x00FFFFFFU);
}

/* A link put in a cell, its element as given; what the cell held before goes */
static BOOL put_link( INT col, INT row, CONST UB *el, INT len )
{
	CXCELL	*c;
	INT	m = cx_merge_of(&g_s, col, row);

	if ( m >= 0 ) {
		col = g_s.m[m].c0;		/* a join takes it in its corner */
		row = g_s.m[m].r0;
	}
	c = cx_make(&g_s, col, row);
	if ( c == NULL || !cx_set_el(c, CX_LINK, el, len) ) return FALSE;
	(void)ce_blank(col, row);
	cx_grow(&g_s, col, row);
	g_changed = TRUE;
	return TRUE;
}

/* Links dropped on the window: in a row of cells from the one they were let go over */
void dropped( void )
{
	T_OBDROP	*d;
	char		el[900], fr[8], ch[8], tb[8], bg[8], vs[40], ts[40];
	INT		i, n = 0, col, row;

	if ( ( d = ui_drop(&g_ui) ) == NULL ) return;
	col = col_at(d->x - grid_left() + g_sx);
	row = row_at(d->y - grid_top() + g_sy);
	if ( d->y < grid_top() || d->x < grid_left() || col < 1 || row < 1 ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "セルの上に置いてください");
		return;
	}
	ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
	for ( i = 0; i < d->n && col + i <= CX_COLS_MAX; i++ ) {
		T_OBDROPV	*v = &d->v[i];
		INT		w = v->right - v->left, h = v->bottom - v->top;
		UINT		disp = ( v->disp != 0 ) ? v->disp : 0x31U;
		TS_UUID		vid;

		if ( w < 20 ) w = LINK_W;
		if ( h < 12 ) h = LINK_H;
		new_vobjid(&vid);
		ct_uuid_str(&v->target, ts);
		ct_uuid_str(&vid, vs);
		hex_col(fr, v->frcol, "#000000");
		hex_col(ch, v->chcol, "#000000");
		hex_col(tb, v->tbcol, "#ffffff");
		hex_col(bg, v->bgcol, "#ffffff");
		snprintf(el, sizeof(el),
			 "<link id=\"%s_0.xtad\" vobjid=\"%s\" tbcol=\"%s\" frcol=\"%s\" chcol=\"%s\" bgcol=\"%s\""
			 " chsz=\"%d\" dlen=\"0\" framedisp=\"%s\" namedisp=\"%s\" pictdisp=\"%s\" roledisp=\"%s\""
			 " typedisp=\"%s\" updatedisp=\"%s\" hidden=\"false\" width=\"%d\" height=\"%d\""
			 " autoopen=\"false\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>",
			 ts, vs, tb, fr, ch, bg, ( v->chsz > 0 ) ? v->chsz : 14,
			 ( disp & 0x10 ) ? "true" : "false", ( disp & 0x01 ) ? "true" : "false",
			 ( disp & 0x20 ) ? "true" : "false", ( disp & 0x02 ) ? "true" : "false",
			 ( disp & 0x04 ) ? "true" : "false", ( disp & 0x08 ) ? "true" : "false", w, h);
		if ( put_link(col + i, row, (CONST UB *)el, (INT)strlen(el)) ) n++;
	}
	if ( n > 0 ) {
		char	s[80];

		g_col = g_acol = col + n - 1;
		g_row = g_arow = row;
		g_cells_on = TRUE;
		g_chart = -1;
		shown_all();
		snprintf(s, sizeof(s), "dropped %d", n);
		note(s);
	}
}

/*
 * The link in the cell in hand, carried out of the window: the desktop
 * follows the pointer with its outline and answers where it was let go.
 * Over a window that takes links, it is put there and, unless the
 * second button went down on the way (a copy), taken out of the cell.
 * Pressed twice before it was carried, a link to a copy of its object is
 * put there and the cell keeps its own.
 */
void carry_out( INT x0, INT y0, BOOL twice )
{
	CXCELL		*c = the_link();
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	r;
	UB		nm[OB_NAME_MAX];
	TS_UUID		u;
	BOOL		copy;
	char		s[120];
	ER		er;
	UINT		seq;

	if ( c == NULL || !link_target(c, &u) ) return;
	name_of(&u, nm);
	link_box(c, &r);
	ask_of(c, DT_RQ_CARRY, &rq);
	rq.grab_x = x0 - r.left;
	rq.grab_y = y0 - r.top;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK ) {
		g_dirty = TRUE;
		return;
	}
	seq = rq.seq;
	copy = (BOOL)( an.how == DT_HOW_COPY );
	ask_of(c, DT_RQ_PLACE, &rq);
	rq.carry = seq;
	if ( twice ) rq.flags |= DT_F_DUPLICATE;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 10 * 60 * 1000);
	snprintf(s, sizeof(s), "carried %s (%d)%s", nm, (INT)er, twice ? " as a new object" : copy ? " as a copy" : "");
	note(s);
	if ( er >= E_OK && !twice && !copy ) {
		cell_clear(g_col, g_row);
		g_link_on = FALSE;
		say("仮身を移動しました");
	} else if ( er >= E_OK ) {
		say(twice ? "実身を複製しました" : "仮身を複写しました");
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the tray */

typedef struct {
	INT	kind;
	UB	*text, *el;
	INT	ellen;
	CONST CXSTYLE	*st;	/* shared (cx_style) */
	INT	dc, dr;			/* where in the range, from its corner */
} CLIP;

static CLIP	*g_clip;		/* the cells copied that were there, row by row */
static INT	g_nclip;
static INT	g_clip_w, g_clip_h, g_clip_col, g_clip_row;
static BOOL	g_clip_cut;
static D	g_clip_time;		/* when it went in the tray */
static UB	g_clip_name[TR_NAME_MAX];

static void clip_free( void )
{
	INT	i;

	for ( i = 0; g_clip != NULL && i < g_nclip; i++ ) {
		free(g_clip[i].text);
		free(g_clip[i].el);
	}
	free(g_clip);
	g_clip = NULL;
	g_nclip = 0;
	g_clip_w = g_clip_h = 0;
}

/* The set in hand: its time and name, FALSE when the tray is empty */
static BOOL hand_set( D *p_time, UB *name, UINT *p_kinds )
{
	T_TRSTS	*s = malloc(sizeof(T_TRSTS));
	BOOL	yes = FALSE;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS ) {
		*p_time = s->set[s->hand].time;
		if ( name != NULL ) memcpy(name, s->set[s->hand].name, TR_NAME_MAX);
		if ( p_kinds != NULL ) *p_kinds = s->set[s->hand].kinds;
		yes = TRUE;
	}
	free(s);
	return yes;
}

BOOL tray_has( void )
{
	D	t;
	UINT	k = 0;

	return (BOOL)( hand_set(&t, NULL, &k) && ( k & ( 1U << TR_TAD ) ) != 0 );
}

void tray_menu( ID mid )
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

static UB *dup_str( CONST UB *s, INT n )
{
	UB	*d = malloc((size_t)n + 1);

	if ( d != NULL ) {
		memcpy(d, s, (size_t)n);
		d[n] = 0;
	}
	return d;
}

/*
 * What is taken, to the tray: kept here as cells, put there as words or
 * links. Only the cells that are there are kept, so that a large range
 * with little in it takes little; what could not be kept is told, and
 * nothing is taken away then.
 */
void copy_cells( BOOL cut )
{
	CXRANGE	r;
	CTLNK	*l;
	CTBUF	b;
	T_TRREC	tr;
	INT	i, n = 0, nl = 0, row, last_row, last_col;
	char	s[80];
	ER	er;

	range_of(&r);
	clip_free();
	g_clip_w = r.c1 - r.c0 + 1;
	g_clip_h = r.r1 - r.r0 + 1;
	for ( i = 0; i < g_s.n; i++ ) {
		CONST CXCELL	*c = g_s.c[i];

		if ( c->col >= r.c0 && c->col <= r.c1 && c->row >= r.r0 && c->row <= r.r1 ) n++;
	}
	g_clip = calloc((size_t)( n + 1 ), sizeof(CLIP));
	l = calloc((size_t)( n + 1 ), sizeof(CTLNK));
	if ( g_clip == NULL || l == NULL ) {
		free(l);
		clip_free();
		warn("トレーに入れられませんでした", "メモリが足りません。セルは元のままです");
		return;
	}
	g_clip_col = r.c0;
	g_clip_row = r.r0;
	g_clip_cut = cut;
	/* the words, a paragraph a row and a tab between the cells, the cells in order */
	ct_buf_grow(&b, 64 * 1024, ct_more);
	ct_put(&b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n");
	last_row = r.r0;
	last_col = r.c0;
	ct_put(&b, "<p>");
	for ( i = 0; i < g_s.n; i++ ) {
		CXCELL	*c = g_s.c[i];
		CLIP	*k;

		if ( c->col < r.c0 || c->col > r.c1 || c->row < r.r0 || c->row > r.r1 ) continue;
		for ( row = last_row; row < c->row; row++ ) {
			ct_put(&b, "</p>\n<p>");
			last_col = r.c0;
		}
		last_row = c->row;
		for ( ; last_col < c->col; last_col++ ) ct_put(&b, "\t");
		k = &g_clip[g_nclip++];
		k->dc = c->col - r.c0;
		k->dr = c->row - r.r0;
		k->kind = c->kind;
		k->text = dup_str(c->text, (INT)strlen((char *)c->text));
		k->st = c->st;
		if ( c->el != NULL ) {
			k->el = dup_str(c->el, c->ellen);
			k->ellen = c->ellen;
		}
		if ( k->text == NULL || ( c->el != NULL && k->el == NULL ) ) b.fail = TRUE;
		if ( c->kind == CX_LINK && c->el != NULL ) {
			l[nl].el = c->el;
			l[nl].len = c->ellen;
			l[nl].name = (CONST UB *)"";
			nl++;
		}
		cx_esc(&b, c->text, (INT)strlen((char *)c->text));
	}
	for ( row = last_row; row < r.r1; row++ ) ct_put(&b, "</p>\n<p>");
	ct_put(&b, "</p>\n</document>\n</tad>\n");
	if ( nl > 0 && !b.fail ) {
		(void)ct_more(b.p, 0);
		ct_buf_grow(&b, 64 * 1024, ct_more);
		(void)ct_frag(&b, l, nl);
		snprintf(s, sizeof(s), "仮身%d個", nl);
	} else {
		snprintf(s, sizeof(s), "セル%d個", g_clip_w * g_clip_h);
	}
	free(l);
	memset(&tr, 0, sizeof(tr));
	tr.kind = TR_TAD;
	tr.data = b.p;
	tr.size = b.n;
	er = b.fail ? E_NOMEM : tr_psh_dat(&tr, 1, (CONST UB *)s);
	(void)ct_more(b.p, 0);
	if ( er < E_OK || !hand_set(&g_clip_time, g_clip_name, NULL) ) {
		clip_free();
		warn("トレーに入れられませんでした", ( er == E_NOMEM )
		     ? "メモリが足りません。セルは元のままです" : "セルは元のままです");
		return;
	}
	if ( cut ) {
		/* taken away all at once, and what named them worked out after */
		(void)ce_bulk(1);
		for ( i = 0; i < g_nclip; i++ ) {
			(void)ce_clear(r.c0 + g_clip[i].dc, r.r0 + g_clip[i].dr);
		}
		(void)cx_remove_range(&g_s, &r);
		(void)ce_bulk(0);
		shown_all();
		g_changed = TRUE;
	}
	snprintf(s, sizeof(s), "%s %d", cut ? "cut" : "copied", g_clip_w * g_clip_h);
	note(s);
	say(cut ? "トレーへ移動しました" : "トレーへ複写しました");
	g_dirty = TRUE;
}

/* A cell copied put back: a formula's references moved by where it goes */
static void paste_one( CONST CLIP *k, INT col, INT row, INT dc, INT dr, BOOL keep_id )
{
	if ( k->kind == CX_LINK && k->el != NULL ) {
		CXCELL	*c;
		CTBUF	b;

		ct_buf_grow(&b, k->ellen + 128, ct_more);
		if ( keep_id ) {
			ct_putn(&b, k->el, k->ellen);
		} else {
			CTSET	set;
			char	vs[TS_UUID_STRLEN + 1];
			TS_UUID	vid;

			new_vobjid(&vid);
			ct_uuid_str(&vid, vs);
			set.name = "vobjid";
			set.value = (CONST UB *)vs;
			ct_link_edit(&b, k->el, k->ellen, &set, 1);
		}
		if ( !b.fail && put_link(col, row, b.p, b.n) && ( c = cx_get(&g_s, col, row) ) != NULL ) {
			c->st = k->st;
		}
		(void)ct_more(b.p, 0);
	} else if ( k->kind == CX_IMAGE && k->el != NULL ) {
		CXCELL	*c = cx_make(&g_s, col, row);

		if ( c != NULL && cx_set_el(c, CX_IMAGE, k->el, k->ellen) ) {
			(void)cx_style_add(c, k->st);
			(void)ce_blank(col, row);
		}
	} else {
		/* a reference moved may grow by a few letters, or to #REF! */
		INT	n = (INT)strlen((char *)k->text), max = n * 2 + 64;
		UB	*t = malloc((size_t)max);
		CXCELL	*c;

		if ( t == NULL ) return;
		if ( k->text[0] == '=' ) (void)cx_move_refs(k->text, n, dc, dr, g_s.maxcol, g_s.maxrow, t, max);
		else memcpy(t, k->text, (size_t)n + 1);
		cell_text(col, row, t);
		c = cx_get(&g_s, col, row);
		if ( c != NULL && k->st->has != 0 ) (void)cx_style_add(c, k->st);
		free(t);
	}
	cx_grow(&g_s, col, row);
}

/* The xmlTAD fragment of the set in hand, read whole */
static UB *tray_frag( INT *p_len )
{
	T_TRINF	inf;
	UB	*buf;
	SZ	asz = 0;
	INT	i;

	*p_len = 0;
	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) return NULL;
		if ( inf.kind == TR_TAD && inf.size > 0 ) break;
	}
	if ( i == TR_REC_MAX ) return NULL;
	buf = malloc((size_t)inf.size + 1);
	if ( buf == NULL ) return NULL;
	if ( tr_rea_rec(TR_HAND, i, buf, inf.size, &asz) < E_OK || asz != inf.size ) {
		free(buf);
		return NULL;
	}
	buf[inf.size] = 0;
	*p_len = (INT)inf.size;
	return buf;
}

/* The words of a text fragment: its paragraphs a line each, its tabs as tabs, entities as letters */
static INT frag_words( CONST UB *x, INT n, UB *out, INT max )
{
	INT	i, k = 0, para = 0;
	BOOL	in_doc = FALSE;

	for ( i = 0; i < n && k < max - 4; ) {
		if ( x[i] == '<' ) {
			INT	e = i;

			while ( e < n && x[e] != '>' ) e++;
			if ( strncmp((CONST char *)x + i, "<document", 9) == 0 ) in_doc = TRUE;
			else if ( strncmp((CONST char *)x + i, "</document", 10) == 0 ) in_doc = FALSE;
			else if ( in_doc && strncmp((CONST char *)x + i, "<tab", 4) == 0 ) out[k++] = '\t';
			else if ( in_doc && ( strncmp((CONST char *)x + i, "<p>", 3) == 0 || strncmp((CONST char *)x + i, "<p ", 3) == 0 ) ) {
				if ( para++ > 0 ) out[k++] = '\n';
			} else if ( in_doc && strncmp((CONST char *)x + i, "<br", 3) == 0 ) out[k++] = '\n';
			i = e + 1;
			continue;
		}
		if ( !in_doc || x[i] == '\n' || x[i] == '\r' ) {
			i++;
			continue;
		}
		if ( x[i] == '&' ) {
			static CONST char *CONST ent[5] = { "&amp;", "&lt;", "&gt;", "&quot;", "&apos;" };
			static CONST char ch[5] = { '&', '<', '>', '"', '\'' };
			INT	e;

			for ( e = 0; e < 5 && strncmp((CONST char *)x + i, ent[e], strlen(ent[e])) != 0; e++ ) ;
			if ( e < 5 ) {
				out[k++] = (UB)ch[e];
				i += (INT)strlen(ent[e]);
				continue;
			}
		}
		out[k++] = x[i++];
	}
	out[k] = 0;
	return k;
}

/*
 * Pasted at the cell in hand: the cells copied here, while they are
 * what the tray holds in hand; otherwise the links of the fragment in
 * hand, a cell each to the right, or its words in the cell. 'take'
 * takes the set out of the tray after (moving from the tray).
 */
void paste_cells( BOOL take )
{
	D	t;
	UB	nm[TR_NAME_MAX];
	INT	i, n = 0;
	char	s[80];

	if ( g_clip != NULL && hand_set(&t, nm, NULL) && t == g_clip_time && memcmp(nm, g_clip_name, TR_NAME_MAX) == 0 ) {
		/* put in all at once, and worked out together */
		(void)ce_bulk(1);
		g_many = TRUE;
		for ( i = 0; i < g_nclip; i++ ) {
			CLIP	*k = &g_clip[i];
			INT	tc = g_col + k->dc, tr = g_row + k->dr;

			if ( tc > CX_COLS_MAX || tr > CX_ROWS_MAX ) continue;
			paste_one(k, tc, tr, tc - ( g_clip_col + k->dc ), tr - ( g_clip_row + k->dr ), g_clip_cut);
			n++;
		}
		g_many = FALSE;
		(void)ce_bulk(0);
		shown_all();
		/* each cell told on the console as one typed is, while they are few enough to read */
		if ( g_nclip <= 256 ) {
			for ( i = 0; i < g_nclip; i++ ) {
				CONST CXCELL	*c = cx_get(&g_s, g_col + g_clip[i].dc, g_row + g_clip[i].dr);
				char		ln[200], cn[16];

				if ( c == NULL || c->kind != CX_TEXT ) continue;
				(void)cx_cell_name(c->col, c->row, cn);
				snprintf(ln, sizeof(ln), "set %s shows %.80s", cn, (CONST char *)cx_shown(c));
				note(ln);
			}
		}
	} else {
		INT	len = 0, k;
		UB	*x = tray_frag(&len);
		CTEL	*el;

		if ( x == NULL ) {
			say("トレーに貼り付けるものがありません");
			return;
		}
		/* every link in it */
		k = ct_links(x, len, NULL, 0);
		el = malloc(sizeof(CTEL) * (size_t)( k + 1 ));
		k = ( el != NULL ) ? ct_links(x, len, el, k) : 0;
		if ( k > 0 ) {
			for ( i = 0; i < k && g_col + i <= CX_COLS_MAX; i++ ) {
				CONST UB	*e = x + el[i].at;
				UB		*tmp = malloc((size_t)el[i].len + 256);
				CTSET		set[7];
				CTBUF		b;
				char		vs[TS_UUID_STRLEN + 1], ws[12], hs[12];
				TS_UUID		vid;
				INT		w, h;

				if ( tmp == NULL ) break;
				/* the box it had elsewhere is its size here */
				w = ct_attr_int(e, el[i].len, "width", 0);
				h = ct_attr_int(e, el[i].len, "height", 0);
				if ( w <= 0 ) w = ct_attr_int(e, el[i].len, "vobjright", 0) - ct_attr_int(e, el[i].len, "vobjleft", 0);
				if ( h <= 0 ) h = ct_attr_int(e, el[i].len, "vobjbottom", 0) - ct_attr_int(e, el[i].len, "vobjtop", 0);
				if ( w <= 0 ) w = LINK_W;
				if ( h <= 0 ) h = LINK_H;
				snprintf(ws, sizeof(ws), "%d", w);
				snprintf(hs, sizeof(hs), "%d", h);
				new_vobjid(&vid);
				ct_uuid_str(&vid, vs);
				set[0].name = "vobjid";	set[0].value = (CONST UB *)vs;
				set[1].name = "vobjleft";	set[1].value = NULL;
				set[2].name = "vobjtop";	set[2].value = NULL;
				set[3].name = "vobjright";	set[3].value = NULL;
				set[4].name = "vobjbottom";	set[4].value = NULL;
				set[5].name = "width";	set[5].value = (CONST UB *)ws;
				set[6].name = "height";	set[6].value = (CONST UB *)hs;
				ct_buf(&b, tmp, el[i].len + 256);
				ct_link_edit(&b, e, el[i].len, set, 7);
				if ( !b.fail && put_link(g_col + i, g_row, b.p, b.n) ) n++;
				free(tmp);
			}
		} else {
			UB	*w = malloc((size_t)len + 8);

			if ( w != NULL ) {
				(void)frag_words(x, len, w, len + 8);
				cell_text(g_col, g_row, w);
				n = 1;
			}
			free(w);
		}
		free(el);
		free(x);
	}
	if ( n > 0 && take ) (void)tr_del_dat(TR_HAND);
	shown_all();
	g_changed = TRUE;
	snprintf(s, sizeof(s), "pasted %d", n);
	note(s);
	say(take ? "トレーから移動しました" : "トレーから複写しました");
}
