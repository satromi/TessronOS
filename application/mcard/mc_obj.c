/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_obj.c
 *	マイクロカード: the object, its records, the desktop and the tray
 *	(design 11.18)
 *
 *	The records are read once when the program starts and written when
 *	the person saves: record 0 (xmlTAD) and the records of type 31 --
 *	the format, the cards and the links; the order record is left out,
 *	the cards being written in their order, and taken away when the
 *	object had one. A record of the subtypes kept with no type of its
 *	own (type 15, as an archive or a backup brings them in) is read and
 *	written as it is. The objects the links point at are counted when
 *	record 0 is written: a link that came since the last writing counts
 *	its object once more, one that went once less, unless the store
 *	counts the links of its records itself.
 *
 *	Read in from another object: a マイクロカード実身 by its records, a
 *	基本表計算 sheet by its cells, a text by its letters -- record 1, the
 *	file ファイル変換 took in, in whatever code it is written, or else
 *	record 0's text. Written out, the cards worked on make a new object
 *	of one of the forms, and its link goes to the tray to be put where
 *	the person wants it. 差し込み makes the merged pages an object the
 *	same way and opens it.
 */

#include "mc.h"
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/txc.h>


/* ---------------------------------------------------------------- records */

static UB *rec_read( ID k, INT recno, UD size, INT *p_len )
{
	UB	*buf;
	INT	at = 0;
	SZ	asz = 0;

	*p_len = 0;
	if ( size > 64 * 1024 * 1024 ) return NULL;
	buf = malloc((size_t)size + 1);
	while ( buf != NULL && (UD)at < size ) {
		SZ	n = ( size - at > CHUNK ) ? CHUNK : (SZ)( size - at );

		if ( ob_rea_rec(k, recno, (D)at, buf + at, n, &asz) < E_OK || asz <= 0 ) break;
		at += (INT)asz;
	}
	if ( buf != NULL && (UD)at != size ) {
		free(buf);
		return NULL;
	}
	if ( buf != NULL ) {
		buf[size] = 0;
		*p_len = (INT)size;
	}
	return buf;
}

static ER rec_write( ID k, INT recno, CONST UB *data, INT len )
{
	INT	at = 0;
	SZ	asz = 0;
	ER	er = E_OK;

	while ( at < len && er >= E_OK ) {
		SZ	n = ( len - at > CHUNK ) ? CHUNK : (SZ)( len - at );

		er = ob_wri_rec(k, recno, (D)at, data + at, n, &asz);
		if ( er >= E_OK && asz != n ) er = E_IO;
		at += (INT)n;
	}
	if ( er >= E_OK ) er = ob_trn_rec(k, recno, (UD)len);
	return er;
}

/* Whether a record is one of マイクロカード実身's, and which: its subtype, 0 not */
static INT mc_sub( CONST T_OBREC *r )
{
	if ( r->sub < MC_SUB_FORMAT || r->sub > MC_SUB_VOBJ ) return 0;
	if ( r->rt == MC_RT || r->rt == OB_RT_SYSDATA ) return (INT)r->sub;
	return 0;
}

/* The records of an object read: record 0 and those of type 31; the caller frees them */
typedef struct {
	MCRECS	rs;
	UB	*mem[MC_SUBS + 1];
	INT	recno[MC_SUBS];		/* where each subtype is, -1 none */
	INT	xrec;
	INT	r1;			/* record 1 when it is not one of these, -1 none */
	UD	r1size;
} RECS;

static void recs_free( RECS *s )
{
	INT	i;

	for ( i = 0; i <= MC_SUBS; i++ ) free(s->mem[i]);
	memset(s, 0, sizeof(*s));
}

static ER recs_read( CONST TS_UUID *u, RECS *s )
{
	T_OBREC	*r;
	INT	cnt = 0, i, len;
	ID	k;

	memset(s, 0, sizeof(*s));
	for ( i = 0; i < MC_SUBS; i++ ) s->recno[i] = -1;
	s->xrec = -1;
	s->r1 = -1;
	k = ob_opn_obj(u, OB_OP_R);
	if ( k <= 0 ) return (ER)k;
	r = ob_rec_read(k, &cnt);
	for ( i = 0; i < cnt; i++ ) {
		INT	sub = mc_sub(&r[i]);

		if ( sub > 0 && s->recno[sub] < 0 ) {
			s->recno[sub] = r[i].recno;
			s->mem[sub] = rec_read(k, r[i].recno, r[i].size, &len);
			s->rs.r[sub] = s->mem[sub];
			s->rs.n[sub] = len;
		} else if ( r[i].rt == OB_RT_TAD && s->xrec < 0 ) {
			s->xrec = r[i].recno;
			s->mem[0] = rec_read(k, r[i].recno, r[i].size, &len);
			s->rs.x = s->mem[0];
			s->rs.xn = len;
		} else if ( r[i].recno == 1 && sub == 0 ) {
			s->r1 = 1;
			s->r1size = r[i].size;
		}
	}
	free(r);
	ob_cls_obj(k);
	return E_OK;
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
		u->b[14] ^= 0x4D;
	}
}

BOOL link_target( CONST UB *el, TS_UUID *u )
{
	return (BOOL)( el != NULL && ct_target(el, (INT)strlen((CONST char *)el), u) );
}

/* ---------------------------------------------------------------- the links counted */

static TS_UUID	*g_was;			/* the objects record 0 pointed at when last written */
static INT	g_nwas;

/* The objects the database's links point at now, each once */
static INT links_now( TS_UUID **p )
{
	INT	i, f, n = 0, max = mc_link_count(&g_db) + 1;
	TS_UUID	*u = malloc(sizeof(TS_UUID) * (size_t)max);

	*p = u;
	if ( u == NULL ) return 0;
	for ( i = -1; i < g_db.n; i++ ) {
		for ( f = ( i < 0 ) ? -1 : 0; f < ( ( i < 0 ) ? 0 : g_db.nf ); f++ ) {
			CONST UB	*el = ( i < 0 ) ? g_db.layout : ( g_db.f[f].type == MC_T_LINK ) ? mc_get(g_db.c[i], f) : NULL;
			TS_UUID		t;
			INT		k;

			if ( el == NULL || !link_target(el, &t) ) continue;
			for ( k = 0; k < n && memcmp(&u[k], &t, sizeof(t)) != 0; k++ ) ;
			if ( k == n && n < max ) u[n++] = t;
		}
	}
	return n;
}

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
	RECS	s;
	ER	er = recs_read(&g_obj, &s);
	char	line[120];

	if ( er < E_OK ) return er;
	er = mc_read(&g_db, &s.rs);
	snprintf(line, sizeof(line), "read %s (%d) fields %d cards %d", ( er == E_PAR ) ? "new" : "records", (INT)er,
		 g_db.nf, g_db.n);
	note(line);
	recs_free(&s);
	if ( er == E_OBJ ) warn("カードのデータの一部が読めませんでした。", NULL);
	free(g_was);
	g_nwas = links_now(&g_was);
	g_changed = FALSE;
	return ( er == E_NOMEM ) ? er : E_OK;
}

/* The records of a database into memory: record 0 and the binary ones by subtype */
typedef struct {
	UB	*x;
	INT	xn;
	UB	*bin[MC_SUBS];
	INT	n[MC_SUBS];
} WRECS;

static void wrecs_free( WRECS *w )
{
	INT	i;

	free(w->x);
	for ( i = 0; i < MC_SUBS; i++ ) free(w->bin[i]);
	memset(w, 0, sizeof(*w));
}

static ER wrecs_make( WRECS *w, CONST UB *name, BOOL found_only )
{
	INT	max = 256 * 1024, sub;

	memset(w, 0, sizeof(*w));
	for ( ;; ) {
		CTBUF	b;

		free(w->x);
		w->x = malloc((size_t)max);
		if ( w->x == NULL ) return E_NOMEM;
		ct_buf(&b, w->x, max);
		mc_write_x(&b, &g_db, name, found_only);
		if ( !b.fail ) {
			w->xn = b.n;
			break;
		}
		if ( max >= 64 * 1024 * 1024 ) return E_LIMIT;
		max *= 4;
	}
	for ( sub = MC_SUB_FORMAT; sub <= MC_SUB_VOBJ; sub++ ) {
		max = 64 * 1024;
		for ( ;; ) {
			CTBUF	b;

			free(w->bin[sub]);
			w->bin[sub] = malloc((size_t)max);
			if ( w->bin[sub] == NULL ) return E_NOMEM;
			ct_buf(&b, w->bin[sub], max);
			w->n[sub] = mc_write_bin(&b, &g_db, sub, found_only);
			if ( !b.fail ) break;
			if ( max >= 64 * 1024 * 1024 ) return E_LIMIT;
			max *= 4;
		}
	}
	return E_OK;
}

ER save( void )
{
	WRECS	w;
	RECS	s;
	ID	k;
	INT	sub, del[MC_SUBS], nd = 0, i;
	ER	er;

	ed_commit();
	er = wrecs_make(&w, g_name, FALSE);
	if ( er < E_OK ) {
		wrecs_free(&w);
		return er;
	}
	er = recs_read(&g_obj, &s);
	for ( i = 0; i <= MC_SUBS; i++ ) {
		/* only where the records are is wanted */
		free(s.mem[i]);
		s.mem[i] = NULL;
	}
	if ( er < E_OK ) {
		wrecs_free(&w);
		return er;
	}
	k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* its records written together */
	if ( k <= 0 ) {
		wrecs_free(&w);
		return (ER)k;
	}
	if ( s.xrec < 0 ) {
		INT	r = -1;

		er = ob_apd_rec(k, OB_RT_TAD, 0, &r);
		s.xrec = r;
	}
	if ( er >= E_OK ) er = rec_write(k, s.xrec, w.x, w.xn);
	for ( sub = MC_SUB_FORMAT; sub <= MC_SUB_VOBJ && er >= E_OK; sub++ ) {
		if ( w.n[sub] == 0 ) {
			if ( s.recno[sub] >= 0 ) del[nd++] = s.recno[sub];
			continue;
		}
		if ( s.recno[sub] < 0 ) {
			INT	r = -1;

			er = ob_apd_rec(k, MC_RT, (UINT)sub, &r);
			if ( er < E_OK ) er = ob_apd_rec(k, OB_RT_SYSDATA, (UINT)sub, &r);
			s.recno[sub] = r;
		}
		if ( er >= E_OK ) er = rec_write(k, s.recno[sub], w.bin[sub], w.n[sub]);
	}
	/* records the object no longer has: from the last, so the numbers before stay */
	while ( er >= E_OK && nd > 0 ) {
		INT	hi = 0;

		for ( i = 1; i < nd; i++ ) {
			if ( del[i] > del[hi] ) hi = i;
		}
		(void)ob_del_rec(k, del[hi]);
		del[hi] = del[--nd];
	}
	ob_cls_obj(k);
	wrecs_free(&w);
	if ( er >= E_OK ) {
		links_count();
		g_changed = FALSE;
	}
	return er;
}

/* ---------------------------------------------------------------- the desktop */

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
	rq->win = g_cw.win;
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

void link_open_el( CONST UB *el )
{
	T_DTREQ	rq;
	T_DTANS	an;
	UB	nm[OB_NAME_MAX];
	char	s[120];
	ER	er;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	if ( !link_target(el, &rq.target) ) return;
	rq.recno = -1;
	ct_link_req(&rq, el, (INT)strlen((CONST char *)el));	/* the program it keeps to, when it does */
	er = ask_desktop(&rq, &an, 3000);
	name_of(&rq.target, nm);
	snprintf(s, sizeof(s), "opened %s (%d)", (char *)nm, (INT)er);
	note(s);
	if ( er < E_OK ) say("開けませんでした");
}

/* The link in the field in hand */
static CONST UB *the_link( void )
{
	if ( g_screen != SCR_CARD || g_field < 0 || g_field >= g_db.nf || g_db.f[g_field].type != MC_T_LINK ) return NULL;
	return mc_get(cur_card(), g_field);
}

static UINT col_attr( CONST UB *el, CONST char *name )
{
	UB	col[16];

	if ( ct_attr(el, (INT)strlen((CONST char *)el), name, col, sizeof(col)) == 7 && col[0] == '#' ) {
		return (UINT)strtoul((char *)col + 1, NULL, 16);
	}
	return 0xFFFFFFFFU;
}

static CONST char *CONST disp_attr[6] = { "namedisp", "roledisp", "typedisp", "updatedisp", "framedisp", "pictdisp" };

static void ask_of( CONST UB *el, UINT req, T_DTREQ *rq )
{
	T_DPRECT	r;
	INT		i, n = (INT)strlen((CONST char *)el);

	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	(void)link_target(el, &rq->target);
	rq->recno = -1;
	if ( g_field >= 0 ) link_box(g_field, &r);
	else {
		r.left = r.top = 0;
		r.right = LINK_W;
		r.bottom = LINK_H;
	}
	rq->look.w = r.right - r.left;
	rq->look.h = r.bottom - r.top;
	rq->look.chsz = ct_attr_int(el, n, "chsz", 0);
	rq->look.frcol = col_attr(el, "frcol");
	rq->look.chcol = col_attr(el, "chcol");
	rq->look.tbcol = col_attr(el, "tbcol");
	rq->look.bgcol = col_attr(el, "bgcol");
	for ( i = 0; i < 6; i++ ) {
		if ( ct_attr_bool(el, n, disp_attr[i], (BOOL)( i == 0 || i == 5 )) ) rq->look.disp |= 1U << i;
	}
	rq->look.autoopen = ct_attr_bool(el, n, "autoopen", FALSE) ? 1 : 0;
	if ( ct_attr_bool(el, n, "hidden", FALSE) ) rq->flags |= DT_F_HIDDEN;
}

/* The link in hand with attributes set (a value of NULL takes one off) */
static BOOL link_edit( CONST CTSET *set, INT n )
{
	CONST UB	*el = the_link();
	INT		len, max, k;
	UB		*tmp;
	CTBUF		b;
	BOOL		ok;

	if ( el == NULL ) return FALSE;
	len = (INT)strlen((CONST char *)el);
	max = len + 512;
	for ( k = 0; k < n; k++ ) {
		if ( set[k].value != NULL ) max += 6 * (INT)strlen((CONST char *)set[k].value);
	}
	tmp = malloc((size_t)max);
	if ( tmp == NULL ) return FALSE;
	ct_buf(&b, tmp, max);
	ct_link_edit(&b, el, len, set, n);
	ok = (BOOL)( !b.fail && mc_set(&g_db, cur_card(), g_field, b.p, b.n) >= E_OK );
	free(tmp);
	if ( ok ) g_changed = TRUE;
	return ok;
}

static INT app_json( CONST UB *el, UB **p_out )
{
	T_JSON	root, al;
	TS_UUID	u;
	INT	len = (INT)strlen((CONST char *)el), n;

	/* the link's own: no longer than the link it is written in */
	*p_out = malloc((size_t)len + 1);
	if ( *p_out == NULL ) return 0;
	n = ct_attr_text(el, len, "applist", *p_out, len + 1);
	if ( n > 0 ) return n;
	if ( !link_target(el, &u) ) return 0;
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
	static UB	rws[APP_MAX][WM_LABEL_MAX];
	CONST UB	*el = the_link();
	TS_UUID		ed;
	T_DTREQ		rq;
	T_DTANS		an;
	INT		n = 1, k;

	g_napp = 0;
	for ( k = 0; el != NULL && k < n && g_napp < APP_MAX; k++ ) {
		ask_of(el, DT_RQ_APPS, &rq);
		ct_link_req(&rq, el, (INT)strlen((CONST char *)el));
		rq.recno = k;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		if ( ct_app_entry(&an, g_app_id[g_napp], g_app_name[g_napp], &g_app_kind[g_napp]) ) g_napp++;
	}
	for ( k = 0; k < g_napp; k++ ) snprintf((char *)rws[k], WM_LABEL_MAX, "%s", (char *)g_app_name[k]);
	if ( ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rws, WM_LABEL_MAX, g_napp);
	}
	(void)mn_chg_atr(mid, NULL, "exec", ( el == NULL || g_napp == 0 ) ? MN_GREY : 0);
}

BOOL app_kept( void )
{
	CONST UB	*el = the_link();
	UB		*j = NULL;
	INT		n;
	BOOL		kept = FALSE;

	if ( el != NULL ) {
		n = app_json(el, &j);
		kept = ( n > 0 ) ? ct_app_locked(j, n) : FALSE;
	}
	free(j);
	return kept;
}

static void link_look( CONST T_DTANS *an )
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
	(void)link_edit(set, n);
}

void link_op( CONST char *code, INT index )
{
	CONST UB	*el = the_link();
	T_DTREQ		rq;
	T_DTANS		an;
	UB		nm[OB_NAME_MAX];
	char		s[160];
	TS_UUID		u;
	ER		er;

	if ( el == NULL || !link_target(el, &u) ) return;
	name_of(&u, nm);
	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		link_open_el(el);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		ask_of(el, DT_RQ_CLOSE, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		ask_of(el, DT_RQ_ATTR, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			link_look(&an);
			snprintf(s, sizeof(s), "attr %s", (char *)nm);
			note(s);
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		ask_of(el, DT_RQ_RELATION, &rq);
		(void)ct_attr_text(el, (INT)strlen((CONST char *)el), "relationship", rq.name, sizeof(rq.name));
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			CTSET	set;

			an.text[DT_TEXT_MAX - 1] = 0;
			set.name = "relationship";
			set.value = ( an.text[0] != 0 ) ? an.text : NULL;
			(void)link_edit(&set, 1);
		}
	} else if ( strcmp(code, "real.rename") == 0 ) {
		ask_of(el, DT_RQ_RENAME, &rq);
		(void)ask_desktop(&rq, &an, 30 * 60 * 1000);
	} else if ( strcmp(code, "real.duplicate") == 0 || strcmp(code, "real.newversion") == 0 ) {
		ask_of(el, ( code[5] == 'd' ) ? DT_RQ_DUPLICATE : DT_RQ_NEWVER, &rq);
		er = ask_desktop(&rq, &an, 30 * 60 * 1000);
		if ( er >= E_OK ) {
			/* the field points at the copy from now on */
			char	ids[TS_UUID_STRLEN + 8];
			CTSET	set;

			ct_uuid_str(&an.into, ids);
			strcat(ids, "_0.xtad");
			set.name = "id";
			set.value = (CONST UB *)ids;
			(void)link_edit(&set, 1);
			snprintf(s, sizeof(s), "duplicated %s", (char *)nm);
			note(s);
		}
	} else if ( strcmp(code, "real.lock") == 0 || strcmp(code, "real.unlock") == 0 ) {
		UB	*j = NULL, *o = NULL;
		CTBUF	b;
		CTSET	set;
		INT	n;

		n = app_json(el, &j);
		if ( j != NULL ) o = malloc((size_t)n + 64);	/* with "defaultOpenLock" put in */
		if ( j != NULL && o != NULL ) {
			ct_buf(&b, o, n + 64);
			set.name = "applist";
			set.value = b.p;
			if ( n > 0 && ct_app_lock(&b, j, n, (BOOL)!ct_app_locked(j, n)) >= E_OK ) {
				o[b.n] = 0;
				(void)link_edit(&set, 1);
			}
		}
		free(j);
		free(o);
	} else if ( strcmp(code, "real.info") == 0 ) {
		ask_of(el, DT_RQ_INFO, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.network") == 0 ) {
		ask_of(el, DT_RQ_NETWORK, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.search") == 0 ) {
		ask_of(el, DT_RQ_SEARCH, &rq);
		(void)ask_desktop(&rq, &an, 30 * 60 * 1000);
	} else if ( strcmp(code, "exec.app") == 0 ) {
		if ( index < 0 || index >= g_napp ) return;
		ask_of(el, DT_RQ_EXEC, &rq);
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_app_id[index]);
		if ( app_kept() ) rq.flags |= DT_F_KEEP;
		if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
		er = ask_desktop(&rq, &an, 5000);
		snprintf(s, sizeof(s), "exec %s %s (%d)", (char *)nm, (char *)g_app_id[index], (INT)er);
		note(s);
	}
	redraw_all();
}

ER paper_ask( void )
{
	return E_OK;
}

/* ---------------------------------------------------------------- links put in */

static void hex_col( char *o, UINT c, CONST char *dflt )
{
	if ( c == 0xFFFFFFFFU ) strcpy(o, dflt);
	else snprintf(o, 8, "#%06x", c & 0x00FFFFFFU);
}

UB *link_el_from_drop( CONST T_OBDROPV *v, INT *p_len )
{
	char	*el = malloc(1024), fr[8], ch[8], tb[8], bg[8], vs[40], ts[40];
	INT	w = v->right - v->left, h = v->bottom - v->top;
	UINT	disp = ( v->disp != 0 ) ? v->disp : 0x31U;
	TS_UUID	vid;

	if ( el == NULL ) return NULL;
	if ( w < 20 ) w = LINK_W;
	if ( h < 12 ) h = LINK_H;
	new_vobjid(&vid);
	ct_uuid_str(&v->target, ts);
	ct_uuid_str(&vid, vs);
	hex_col(fr, v->frcol, "#000000");
	hex_col(ch, v->chcol, "#000000");
	hex_col(tb, v->tbcol, "#ffffff");
	hex_col(bg, v->bgcol, "#ffffff");
	snprintf(el, 1024,
		 "<link id=\"%s_0.xtad\" vobjid=\"%s\" vobjleft=\"0\" vobjtop=\"0\" vobjright=\"%d\" vobjbottom=\"%d\""
		 " tbcol=\"%s\" frcol=\"%s\" chcol=\"%s\" bgcol=\"%s\" chsz=\"%d\" dlen=\"0\" framedisp=\"%s\" namedisp=\"%s\""
		 " pictdisp=\"%s\" roledisp=\"%s\" typedisp=\"%s\" updatedisp=\"%s\" hidden=\"false\" width=\"%d\" height=\"%d\""
		 " autoopen=\"false\" scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>",
		 ts, vs, w, h, tb, fr, ch, bg, ( v->chsz > 0 ) ? v->chsz : 14,
		 ( disp & 0x10 ) ? "true" : "false", ( disp & 0x01 ) ? "true" : "false",
		 ( disp & 0x20 ) ? "true" : "false", ( disp & 0x02 ) ? "true" : "false",
		 ( disp & 0x04 ) ? "true" : "false", ( disp & 0x08 ) ? "true" : "false", w, h);
	*p_len = (INT)strlen(el);
	return (UB *)el;
}

/* A link to a new object, as a link of the program writes it */
static INT link_new_el( CONST TS_UUID *u, UB *out, INT max )
{
	T_OBDROPV	v;
	UB		*el;
	INT		n = 0;

	memset(&v, 0, sizeof(v));
	v.target = *u;
	v.right = LINK_W;
	v.bottom = LINK_H;
	v.frcol = v.chcol = v.tbcol = v.bgcol = 0xFFFFFFFFU;
	el = link_el_from_drop(&v, &n);
	if ( el == NULL || n >= max ) {
		free(el);
		return 0;
	}
	memcpy(out, el, (size_t)n + 1);
	free(el);
	return n;
}

/* ---------------------------------------------------------------- carried out of the window */

void carry_link_out( INT x0, INT y0, BOOL twice )
{
	CONST UB	*el = the_link();
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	r;
	UB		nm[OB_NAME_MAX];
	TS_UUID		u;
	BOOL		copy;
	char		s[120];
	ER		er;
	UINT		seq;

	if ( el == NULL || !link_target(el, &u) ) return;
	name_of(&u, nm);
	link_box(g_field, &r);
	ask_of(el, DT_RQ_CARRY, &rq);
	rq.grab_x = x0 - r.left;
	rq.grab_y = y0 - r.top;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK ) {
		redraw_all();
		return;
	}
	seq = rq.seq;
	copy = (BOOL)( an.how == DT_HOW_COPY );
	ask_of(el, DT_RQ_PLACE, &rq);
	rq.carry = seq;
	if ( twice ) rq.flags |= DT_F_DUPLICATE;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 10 * 60 * 1000);
	snprintf(s, sizeof(s), "carried %s (%d)%s", (char *)nm, (INT)er, copy ? " as a copy" : "");
	note(s);
	if ( er >= E_OK && !copy && !twice ) {
		(void)mc_set(&g_db, cur_card(), g_field, NULL, 0);
		g_link_on = FALSE;
		g_changed = TRUE;
		say("仮身を移動しました");
	} else if ( er >= E_OK ) {
		say("仮身を複写しました");
	}
	redraw_all();
}

/* A new object beside the database: its metadata, its record 0 */
static ER obj_make( CONST char *name, CONST char *app, CONST char *appname, CONST UB *rec, INT len, TS_UUID *p_u )
{
	T_OBCRE	c;
	char	json[900], qn[600];
	INT	recno = -1, i, q = 0;
	ER	er;
	ID	k;

	/*
	 * The name as a string of JSON: the person may have put " or \ or a
	 * control in it. A character begun is ended, so that a long name is
	 * cut between characters.
	 */
	for ( i = 0; name[i] != 0 && ( q < (INT)sizeof(qn) - 7 || ( (UB)name[i] & 0xC0 ) == 0x80 ); i++ ) {
		UB	ch = (UB)name[i];

		if ( ch == '"' || ch == '\\' ) {
			qn[q++] = '\\';
			qn[q++] = (char)ch;
		} else if ( ch < 0x20 ) {
			q += snprintf(qn + q, sizeof(qn) - (size_t)q, "\\u%04x", ch);
		} else {
			qn[q++] = (char)ch;
		}
	}
	qn[q] = 0;
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"editable\":true,\"deletable\":true,\"readable\":true,"
		 "\"applist\":{\"%s\":{\"name\":\"%s\",\"defaultOpen\":true}}}", qn, app, appname);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	c.near = g_obj;
	er = ob_cre_obj(&c, p_u);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(p_u, OB_OP_R | OB_OP_W | OB_O_EXCL);
	if ( k <= 0 ) {
		(void)ob_del_obj(p_u);
		return (ER)k;
	}
	er = ob_apd_rec(k, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = rec_write(k, recno, rec, len);
	ob_cls_obj(k);
	if ( er < E_OK ) (void)ob_del_obj(p_u);
	return er;
}

/* A link to an object put in the tray, as a figure of it */
static ER tray_link( CONST TS_UUID *u, CONST UB *name )
{
	UB	el[1024], fr[2048];
	CTLNK	l;
	CTBUF	b;
	T_TRREC	r;
	INT	n = link_new_el(u, el, sizeof(el));

	if ( n <= 0 ) return E_NOMEM;
	l.el = el;
	l.len = n;
	l.name = name;
	ct_buf(&b, fr, sizeof(fr));
	(void)ct_frag(&b, &l, 1);
	if ( b.fail ) return E_LIMIT;
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = b.p;
	r.size = b.n;
	return tr_psh_dat(&r, 1, name);
}

void carry_rows_out( INT from, INT to )
{
	CTBUF	b, d;
	UB	*t = malloc(OUT_MAX / 4);
	TS_UUID	u;
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[120], name[OB_NAME_MAX];
	ER	er;
	UINT	seq;

	if ( t == NULL ) return;
	ct_buf(&b, t, OUT_MAX / 4);
	mc_out_rows(&b, &g_db, from, to, TRUE, 0);
	ct_buf(&d, g_out, OUT_MAX);
	snprintf(name, sizeof(name), "%sのカード", (char *)g_name);
	mc_text_doc(&d, b.p, b.n, (CONST UB *)name);
	free(t);
	if ( b.fail || d.fail ) return;
	/* the rows go as a text of their own, whose link is put where the hand lets go */
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.recno = -1;
	rq.look.w = LINK_W;
	rq.look.h = LINK_H;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK ) {
		redraw_all();
		return;
	}
	seq = rq.seq;
	if ( obj_make(name, "basic-text-editor", "基本文章編集", d.p, d.n, &u) < E_OK ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PLACE;
	rq.target = u;
	rq.recno = -1;
	rq.carry = seq;
	rq.look.w = LINK_W;
	rq.look.h = LINK_H;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	er = ask_desktop(&rq, &an, 10 * 60 * 1000);
	if ( er < E_OK ) (void)ob_del_obj(&u);
	snprintf(s, sizeof(s), "rows carried %d..%d (%d)", from + 1, to + 1, (INT)er);
	note(s);
	redraw_all();
}

/* ---------------------------------------------------------------- reading in */

/* A text of record 1 (a file taken in) as UTF-8, whatever its code; NULL none */
static UB *file_text( CONST TS_UUID *u, UD size, INT *p_len )
{
	ID	k = ob_opn_obj(u, OB_OP_R);
	UB	*raw, *t;
	INT	n = 0;
	SZ	tn;

	*p_len = 0;
	if ( k <= 0 ) return NULL;
	raw = rec_read(k, 1, size, &n);
	ob_cls_obj(k);
	if ( raw == NULL ) return NULL;
	t = malloc((size_t)n * 3 + 8);
	if ( t != NULL ) {
		INT	bad = 0, enc = txc_detect(raw, n);

		tn = txc_to_utf8(enc, raw, n, t, (SZ)n * 3 + 4, &bad);
		if ( tn < 0 ) tn = 0;
		t[tn] = 0;
		*p_len = (INT)tn;
	}
	free(raw);
	return t;
}

INT import_obj( CONST TS_UUID *u, INT at )
{
	RECS	s;
	INT	r = 0;
	ER	er;

	if ( memcmp(u, &g_obj, sizeof(*u)) == 0 ) return 0;
	er = recs_read(u, &s);
	if ( er < E_OK ) return er;
	if ( s.rs.r[MC_SUB_FORMAT] != NULL ) {
		MCDB	*src = malloc(sizeof(MCDB));

		if ( src != NULL ) {
			mc_init(src);
			if ( mc_read(src, &s.rs) != E_PAR ) r = mc_in_db(&g_db, src, at);
			mc_clear(src);
			free(src);
		}
	} else if ( s.rs.x != NULL && strstr((CONST char *)s.rs.x, "<calcPos") != NULL ) {
		CXSHEET	*sh = malloc(sizeof(CXSHEET));

		if ( sh != NULL ) {
			cx_init(sh);
			if ( cx_read(sh, s.rs.x, s.rs.xn) >= E_OK ) {
				cx_order(sh);
				r = mc_in_sheet(&g_db, sh, at);
			}
			cx_free_all(sh);
			free(sh);
		}
	} else {
		UB	*t = NULL;
		INT	n = 0;

		if ( s.r1 >= 0 && s.r1size > 0 ) t = file_text(u, s.r1size, &n);
		if ( t == NULL && s.rs.x != NULL ) {
			t = malloc((size_t)s.rs.xn + 8);
			if ( t != NULL ) n = mc_doc_text(s.rs.x, s.rs.xn, t, s.rs.xn + 8);
		}
		if ( t != NULL ) r = mc_in_text(&g_db, t, n, at, MC_IN_AUTO);
		free(t);
	}
	recs_free(&s);
	if ( r > 0 ) g_changed = TRUE;
	return r;
}

/* ---------------------------------------------------------------- the tray */

static BOOL hand_kinds( UINT *p_kinds )
{
	T_TRSTS	*s = malloc(sizeof(T_TRSTS));
	BOOL	yes = FALSE;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS ) {
		*p_kinds = s->set[s->hand].kinds;
		yes = TRUE;
	}
	free(s);
	return yes;
}

BOOL tray_has( void )
{
	UINT	k = 0;

	return (BOOL)( hand_kinds(&k) && ( k & ( 1U << TR_TAD ) ) != 0 );
}

void tray_menu( ID mid )
{
	static UB	rws[TR_LEVELS][WM_LABEL_MAX];
	TS_UUID		td;
	T_TRSTS		*s = malloc(sizeof(T_TRSTS));
	INT		i;

	if ( s == NULL ) return;
	if ( ui_uuid(SYSDEF_MENU_TRAY, &td) >= E_OK && tr_get_sts(s) >= E_OK ) {
		for ( i = 1; i <= s->nlevel && i <= TR_LEVELS; i++ ) {
			snprintf((char *)rws[i - 1], WM_LABEL_MAX, "%d %s", i, (char *)s->set[i].name);
		}
		(void)mn_set_lst(mid, &td, "tray.level", (CONST UB *)rws, WM_LABEL_MAX, s->nlevel);
		if ( s->hand > 0 ) (void)mn_chg_idx(mid, &td, "tray.level", s->hand - 1, MN_TICK);
		(void)mn_chg_atr(mid, &td, "tray.clear", ( s->nlevel == 0 ) ? MN_GREY : 0);
	}
	free(s);
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
		if ( inf.kind == TR_TAD && inf.size > 0 && inf.size < 16 * 1024 * 1024 ) break;
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

static ER push_text( CONST UB *t, INT n, CONST char *name )
{
	CTBUF	b;
	T_TRREC	r;

	ct_buf(&b, g_out, OUT_MAX);
	mc_text_doc(&b, t, n, NULL);
	if ( b.fail ) return E_LIMIT;
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = b.p;
	r.size = b.n;
	return tr_psh_dat(&r, 1, (CONST UB *)name);
}

void tray_copy_field( BOOL cut )
{
	MCCARD	*c = cur_card();
	INT	f = g_field;
	char	s[80];
	ER	er;

	ed_commit();
	if ( c == NULL || f < 0 || f >= g_db.nf || mc_get(c, f) == NULL ) {
		say("トレーへ入れるデータがありません");
		return;
	}
	if ( g_db.f[f].type == MC_T_LINK ) {
		CTLNK	l;
		CTBUF	b;
		T_TRREC	r;

		l.el = mc_get(c, f);
		l.len = (INT)strlen((CONST char *)l.el);
		l.name = (CONST UB *)"";
		ct_buf(&b, g_out, OUT_MAX);
		(void)ct_frag(&b, &l, 1);
		memset(&r, 0, sizeof(r));
		r.kind = TR_TAD;
		r.data = b.p;
		r.size = b.n;
		er = tr_psh_dat(&r, 1, (CONST UB *)"仮身1個");
	} else {
		UB	v[MC_VALUE_MAX + 8];
		INT	n = mc_shown(&g_db, c, f, v, sizeof(v));

		er = push_text(v, n, (CONST char *)v);
	}
	if ( er < E_OK ) {
		say("トレーに入れられませんでした");
		return;
	}
	if ( cut ) {
		(void)mc_set(&g_db, c, f, NULL, 0);
		g_changed = TRUE;
		g_link_on = FALSE;
	}
	snprintf(s, sizeof(s), "%s field %d", cut ? "cut" : "copied", f + 1);
	note(s);
	say(cut ? "トレーへ移動しました" : "トレーへ複写しました");
	redraw_all();
}

void tray_paste_field( BOOL take )
{
	MCCARD	*c = cur_card();
	INT	f = g_field, len = 0;
	UB	*x;
	char	s[80];

	if ( c == NULL || f < 0 || f >= g_db.nf ) {
		say("項目を選んでから複写してください");
		return;
	}
	x = tray_frag(&len);
	if ( x == NULL ) {
		say("トレーに複写できるものがありません");
		return;
	}
	if ( g_db.f[f].type == MC_T_LINK ) {
		CTEL	el[1];

		if ( ct_links(x, len, el, 1) > 0 ) {
			UB	*tmp = malloc((size_t)el[0].len + 256);
			CTSET	set[1];
			CTBUF	b;
			char	vs[TS_UUID_STRLEN + 1];
			TS_UUID	vid;

			if ( tmp != NULL ) {
				new_vobjid(&vid);
				ct_uuid_str(&vid, vs);
				set[0].name = "vobjid";
				set[0].value = (CONST UB *)vs;
				ct_buf(&b, tmp, el[0].len + 256);
				ct_link_edit(&b, x + el[0].at, el[0].len, set, 1);
				if ( !b.fail ) {
					ed_commit();
					(void)mc_set(&g_db, c, f, b.p, b.n);
					g_changed = TRUE;
				}
				free(tmp);
			}
		} else {
			say("仮身の項目には仮身だけを入れられます");
			free(x);
			return;
		}
	} else {
		UB	*t = malloc((size_t)len + 8);
		INT	n, e;

		if ( t == NULL ) {
			free(x);
			return;
		}
		n = mc_doc_text(x, len, t, len + 8);
		for ( e = 0; e < n && t[e] != '\n'; e++ ) {
			if ( t[e] == '\t' ) t[e] = ' ';
		}
		if ( ed_on() && ( ed_kind() == ED_VALUE || ed_kind() == ED_COND ) ) {
			ed_insert(t, e);
		} else if ( mc_set(&g_db, c, f, t, e) < E_OK ) {
			warn("数値として読めない値です。", NULL);
		} else {
			g_changed = TRUE;
		}
		free(t);
	}
	free(x);
	if ( take ) (void)tr_del_dat(TR_HAND);
	snprintf(s, sizeof(s), "pasted field %d", f + 1);
	note(s);
	say(take ? "トレーから移動しました" : "トレーから複写しました");
	redraw_all();
}

void tray_copy_rows( INT from, INT to )
{
	UB	*t = malloc(OUT_MAX / 4);
	CTBUF	b;
	char	s[80];
	INT	n = ( from < 0 ) ? mc_count(&g_db) : to - from + 1;

	if ( t == NULL ) return;
	ct_buf(&b, t, OUT_MAX / 4);
	mc_out_rows(&b, &g_db, from, to, TRUE, 0);
	snprintf(s, sizeof(s), "カード%d枚", n);
	if ( b.fail || push_text(b.p, b.n, s) < E_OK ) say("トレーに入れられませんでした");
	else say("トレーへ複写しました");
	free(t);
	snprintf(s, sizeof(s), "rows copied %d", n);
	note(s);
}

void tray_paste_rows( BOOL take )
{
	INT	len = 0, k, n = 0, at = cur_index() + 1;
	UB	*x = tray_frag(&len);
	CTEL	*el;
	char	s[80];

	if ( x == NULL ) {
		say("トレーに複写できるものがありません");
		return;
	}
	el = malloc(sizeof(CTEL) * 64);
	k = ( el != NULL ) ? ct_links(x, len, el, 64) : 0;
	if ( k > 0 ) {
		/* links: the objects they point at read in as cards */
		INT	i;

		for ( i = 0; i < k; i++ ) {
			TS_UUID	u;

			if ( !ct_target(x + el[i].at, el[i].len, &u) ) continue;
			{
				INT	r = import_obj(&u, at);

				if ( r > 0 ) {
					n += r;
					at += r;
				}
			}
		}
	} else {
		UB	*t = malloc((size_t)len + 8);

		if ( t != NULL ) {
			INT	tn = mc_doc_text(x, len, t, len + 8);

			n = mc_in_rows(&g_db, t, tn, at);
			free(t);
		}
	}
	free(el);
	free(x);
	if ( n > 0 ) {
		g_changed = TRUE;
		if ( take ) (void)tr_del_dat(TR_HAND);
		show_card(mc_pos(&g_db, cur_index() + 1 < g_db.n ? cur_index() + 1 : cur_index()));
	}
	snprintf(s, sizeof(s), "rows pasted %d", n);
	note(s);
	snprintf(s, sizeof(s), "%d 枚のカードを読み込みました", n > 0 ? n : 0);
	say(s);
	redraw_all();
}

/* ---------------------------------------------------------------- written out */

ER export_obj( INT form, CONST UB *name )
{
	TS_UUID	u;
	CTBUF	b;
	ER	er;
	char	s[160];

	ct_buf(&b, g_out, OUT_MAX);
	switch ( form ) {
	case EX_MCARD:
		{
			WRECS	w;
			ID	k;
			INT	sub;

			er = wrecs_make(&w, name, TRUE);
			if ( er >= E_OK ) er = obj_make((CONST char *)name, "tessronos-microcard", "マイクロカード", w.x, w.xn, &u);
			if ( er >= E_OK && ( k = ob_opn_obj(&u, OB_OP_R | OB_OP_W | OB_O_EXCL) ) > 0 ) {
				for ( sub = MC_SUB_FORMAT; sub <= MC_SUB_VOBJ && er >= E_OK; sub++ ) {
					INT	r = -1;

					if ( w.n[sub] == 0 ) continue;
					er = ob_apd_rec(k, MC_RT, (UINT)sub, &r);
					if ( er < E_OK ) er = ob_apd_rec(k, OB_RT_SYSDATA, (UINT)sub, &r);
					if ( er >= E_OK ) er = rec_write(k, r, w.bin[sub], w.n[sub]);
				}
				ob_cls_obj(k);
				if ( er >= E_OK && !g_autoref ) {
					/* its links count their objects */
					TS_UUID	*now;
					INT	n = links_now(&now), i;

					for ( i = 0; i < n; i++ ) (void)ob_lnk_obj(&now[i]);
					free(now);
				}
			}
			wrecs_free(&w);
		}
		break;
	case EX_AT:
	case EX_CSV:
		{
			UB	*t = malloc(OUT_MAX / 2);
			CTBUF	tb;

			if ( t == NULL ) return E_NOMEM;
			ct_buf(&tb, t, OUT_MAX / 2);
			if ( form == EX_AT ) mc_out_at(&tb, &g_db, TRUE);
			else mc_out_rows(&tb, &g_db, -1, -1, TRUE, MC_OUT_HEAD | MC_OUT_CSV | MC_OUT_QUOTE);
			mc_text_doc(&b, tb.p, tb.n, name);
			free(t);
			er = ( tb.fail || b.fail ) ? E_LIMIT : obj_make((CONST char *)name, "basic-text-editor", "基本文章編集", b.p, b.n, &u);
		}
		break;
	case EX_SHEET:
		{
			CXSHEET	*sh = malloc(sizeof(CXSHEET));

			if ( sh == NULL ) return E_NOMEM;
			cx_init(sh);
			mc_out_sheet(sh, &g_db, TRUE);
			cx_order(sh);
			cx_write(&b, sh, name, NULL, NULL);
			cx_free_all(sh);
			free(sh);
			er = b.fail ? E_LIMIT : obj_make((CONST char *)name, "basic-calc-editor", "基本表計算", b.p, b.n, &u);
		}
		break;
	default:
		return E_PAR;
	}
	if ( er < E_OK ) return er;
	(void)tray_link(&u, name);
	snprintf(s, sizeof(s), "exported %d %s", form, (char *)name);
	note(s);
	snprintf(s, sizeof(s), "「%s」に書き出しました。仮身をトレーに入れました", (char *)name);
	say(s);
	return E_OK;
}

/* ---------------------------------------------------------------- 差し込み */

/* The layouts: the objects of the layout cabinet's links */
INT layout_names( UB (*names)[MC_LAYOUT_NAME], TS_UUID *objs, INT max )
{
	TS_UUID	cab;
	RECS	s;
	CTEL	el[32];
	INT	n = 0, k, i;

	if ( g_db.layout == NULL || !link_target(g_db.layout, &cab) ) return 0;
	if ( recs_read(&cab, &s) < E_OK ) return 0;
	k = ( s.rs.x != NULL ) ? ct_links(s.rs.x, s.rs.xn, el, 32) : 0;
	for ( i = 0; i < k && n < max; i++ ) {
		UB	nm[OB_NAME_MAX];

		if ( !ct_target(s.rs.x + el[i].at, el[i].len, &objs[n]) ) continue;
		name_of(&objs[n], nm);
		if ( nm[0] == 0 ) continue;
		snprintf((char *)names[n], MC_LAYOUT_NAME, "%s", (char *)nm);
		n++;
	}
	recs_free(&s);
	return n;
}

/* The pages put in one record: a text's one after another with a break between, a figure's down */
typedef struct {
	CTBUF	*b;
	INT	kind, w, h, n;
} OSINK;

static void os_start( void *arg, INT kind, INT w, INT h )
{
	OSINK	*o = arg;

	o->kind = kind;
	o->w = w;
	o->h = h;
	o->n = 0;
	ct_put(o->b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n");
	ct_put(o->b, ( kind == MC_PG_FIG ) ? "<figure>\n" : "<document>\n");
}

static void os_page( void *arg, CONST UB *body, INT n )
{
	OSINK	*o = arg;

	if ( o->kind == MC_PG_FIG ) {
		mc_shift(o->b, body, n, 0, o->n * ( o->h + 40 ));
	} else {
		if ( o->n > 0 ) ct_put(o->b, "<p><pagebreak/></p>\n");
		ct_putn(o->b, body, n);
	}
	o->n++;
}

static void os_end( void *arg )
{
	OSINK	*o = arg;

	ct_put(o->b, ( o->kind == MC_PG_FIG ) ? "</figure>\n</tad>\n" : "</document>\n</tad>\n");
}

static void today( INT *y, INT *m, INT *d )
{
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		t[48];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&clk, OB_OP_READ);

	*y = 2026;
	*m = 1;
	*d = 1;
	if ( k > 0 ) {
		if ( ob_rea_rec(k, 1, 0, t, sizeof(t) - 1, &asz) >= E_OK && asz >= 10 ) {
			t[asz] = 0;
			*y = atoi((char *)t);
			*m = atoi((char *)t + 5);
			*d = atoi((char *)t + 8);
		}
		ob_cls_obj(k);
	}
}

/* A page of the merge into the object the desktop prints from */
static ER prn_put( void *arg, CONST UB *xml, INT n )
{
	return ui_prn_page((T_UIPRN *)arg, xml, n);
}

/*
 * 印刷: the cards merged into the layout, page by page, into an object
 * of pages that the desktop is asked to print (DT_RQ_PRINT). The
 * desktop's panels say how; the file it writes goes on the desktop.
 */
ER merge_print( CONST UB *layout_name, BOOL one )
{
	UB		names[16][MC_LAYOUT_NAME];
	TS_UUID		objs[16];
	RECS		s;
	CONST UB	*x = NULL;
	INT		xn = 0, nl, k, n, *idx;
	MCMINFO		mi;
	MCPRINT		*p = mc_print_of(&g_db, layout_name);
	MCSINK		sink;
	MCPSINK		ps;
	T_UIPRN		up;
	UB		*work, *page;
	ER		er;
	char		line[160];
	BOOL		fig = FALSE;

	memset(&s, 0, sizeof(s));
	if ( layout_name[0] != 0 ) {
		nl = layout_names(names, objs, 16);
		for ( k = 0; k < nl && strcmp((char *)names[k], (CONST char *)layout_name) != 0; k++ ) ;
		if ( k == nl || recs_read(&objs[k], &s) < E_OK || s.rs.x == NULL ) {
			recs_free(&s);
			return E_NOEXS;
		}
		x = s.rs.x;
		xn = s.rs.xn;
		fig = (BOOL)( strstr((CONST char *)x, "<figure") != NULL );
	}
	idx = malloc(sizeof(INT) * (size_t)( g_db.n + 1 ));
	work = malloc(4 * 1024 * 1024);
	page = malloc(4 * 1024 * 1024 + 4096);
	if ( idx == NULL || work == NULL || page == NULL ) {
		free(idx);
		free(work);
		free(page);
		recs_free(&s);
		return E_NOMEM;
	}
	er = ui_prn_open(&up, &g_obj);
	if ( er >= E_OK ) {
		n = mc_targets(&g_db, one, g_cur, idx, g_db.n);
		memset(&mi, 0, sizeof(mi));
		snprintf((char *)mi.name, sizeof(mi.name), "%s", (char *)g_name);
		today(&mi.y, &mi.m, &mi.d);
		mi.pages = mc_pages(&g_db, p, fig, (BOOL)( x == NULL ), n);
		mc_prn_sink(&sink, &ps, p, page, 4 * 1024 * 1024 + 4096, prn_put, &up);
		er = mc_merge(&g_db, p, x, xn, idx, n, &mi, &sink, work, 4 * 1024 * 1024);
		if ( er >= E_OK && ps.er < E_OK ) er = ps.er;
		if ( er >= E_OK ) er = ui_prn_ready(&up);
		snprintf(line, sizeof(line), "print %s cards %d pages %d (%d)",
			 layout_name[0] ? (CONST char *)layout_name : "list", n, ps.pages, (INT)er);
		note(line);
		if ( er >= E_OK ) {
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
	}
	free(idx);
	free(work);
	free(page);
	recs_free(&s);
	return er;
}

ER merge_obj( CONST UB *layout_name, BOOL one )
{
	UB		names[16][MC_LAYOUT_NAME];
	TS_UUID		objs[16], u;
	RECS		s;
	CONST UB	*x = NULL;
	INT		xn = 0, nl, k, n, *idx;
	MCMINFO		mi;
	MCPRINT		*p = mc_print_of(&g_db, layout_name);
	OSINK		o;
	MCSINK		sink;
	CTBUF		b;
	UB		*work;
	ER		er;
	char		name[OB_NAME_MAX + 32], line[160];
	BOOL		fig = FALSE;

	memset(&s, 0, sizeof(s));
	if ( layout_name[0] != 0 ) {
		nl = layout_names(names, objs, 16);
		for ( k = 0; k < nl && strcmp((char *)names[k], (CONST char *)layout_name) != 0; k++ ) ;
		if ( k == nl || recs_read(&objs[k], &s) < E_OK || s.rs.x == NULL ) {
			recs_free(&s);
			return E_NOEXS;
		}
		x = s.rs.x;
		xn = s.rs.xn;
		fig = (BOOL)( strstr((CONST char *)x, "<figure") != NULL );
	}
	idx = malloc(sizeof(INT) * (size_t)( g_db.n + 1 ));
	work = malloc(4 * 1024 * 1024);
	if ( idx == NULL || work == NULL ) {
		free(idx);
		free(work);
		recs_free(&s);
		return E_NOMEM;
	}
	n = mc_targets(&g_db, one, g_cur, idx, g_db.n);
	memset(&mi, 0, sizeof(mi));
	snprintf((char *)mi.name, sizeof(mi.name), "%s", (char *)g_name);
	today(&mi.y, &mi.m, &mi.d);
	mi.pages = mc_pages(&g_db, p, fig, (BOOL)( x == NULL ), n);
	ct_buf(&b, g_out, OUT_MAX);
	o.b = &b;
	sink.arg = &o;
	sink.start = os_start;
	sink.page = os_page;
	sink.end = os_end;
	er = mc_merge(&g_db, p, x, xn, idx, n, &mi, &sink, work, 4 * 1024 * 1024);
	free(idx);
	free(work);
	recs_free(&s);
	if ( er >= E_OK && b.fail ) er = E_LIMIT;
	snprintf(name, sizeof(name), "%sの差し込み", (char *)g_name);
	if ( er >= E_OK ) {
		er = fig ? obj_make(name, "basic-figure-editor", "基本図形編集", b.p, b.n, &u)
			 : obj_make(name, "basic-text-editor", "基本文章編集", b.p, b.n, &u);
	}
	snprintf(line, sizeof(line), "merged %s cards %d pages %d (%d)", layout_name[0] ? (CONST char *)layout_name : "list", n,
		 mi.pages, (INT)er);
	note(line);
	if ( er < E_OK ) return er;
	(void)tray_link(&u, (CONST UB *)name);
	{
		T_DTREQ	rq;
		T_DTANS	an;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_OPEN;
		rq.target = u;
		rq.recno = -1;
		(void)ask_desktop(&rq, &an, 3000);
	}
	snprintf(line, sizeof(line), "差し込みの結果「%s」を作りました（%dページ）。仮身をトレーに入れました", name, mi.pages);
	say(line);
	return E_OK;
}
