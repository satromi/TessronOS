/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bc_main.c
 *	原紙集め: the templates of every application of the system, shown
 *	as the 原紙箱 shows them, an accessory running as a process (design
 *	11.2.3)
 *
 *	The list is made, not read: one closed link to each template the
 *	applications of the program box name in "tessronos.program.base",
 *	in the box's order, one to a row, each as wide as the template's
 *	name, 27 pixels from one row to the next. It is not an object and
 *	nothing in it is changed or saved. A template carried out of it is
 *	asked of the desktop with DT_F_BASE: where it is let go, the
 *	desktop makes a new object from the template and puts the link to
 *	it there, as for one carried out of a 原紙箱. Whatever was taken out
 *	of the 原紙箱, the templates are still the programs', and here.
 *
 *	The list is made again when the program box changes (a program
 *	installed or deleted) and from the menu's 再表示. What it does is
 *	said on the console for the tests ("basecol: ...").
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uapp.h>
#include <ts/obatr.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/tvsrc.h>
#include <ts/dtreq.h>
#include <ts/json.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/wm.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define BC_CHSZ		14		/* the letters of a link, as the 原紙箱's */
#define BC_PAD		8		/* left of the links, and above the first */
#define BC_GAP		8		/* between one row and the next */
#define BC_BAND		( BC_CHSZ + BC_CHSZ / 4 + 2 )	/* a closed link's height (19 at 14) */
#define BC_MAX		256		/* templates listed */
#define BC_PAPER	0x00FFFFFFU

typedef struct {
	TS_UUID	id;
	UB	name[TAD_NAME_MAX];
} BCENT;

static BCENT	g_ent[BC_MAX];
static INT	g_n = 0;
static T_TAD	*g_rec = NULL;
static T_TVFIG	*g_fig = NULL;
static TS_UUID	g_win;
static ID	g_kw = 0;
static INT	g_gid = -1;
static INT	g_sy = 0;			/* how far the list is scrolled */

static void note( CONST char *s )
{
	char	b[200];

	snprintf(b, sizeof(b), "basecol: %s\n", s);
	tm_putstring((CONST UB *)b);
}

/* ---------------------------------------------------------------- what the programs name */

/* An object's metadata, the caller's to free; NULL when it cannot be read */
static UB *meta_of( CONST TS_UUID *u, INT *p_n )
{
	UB	*m = NULL;
	INT	n;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	if ( k <= 0 ) return NULL;
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n <= 0 ) {
		free(m);
		return NULL;
	}
	*p_n = n;
	return m;
}

/* One template more, unless it is listed already */
static void put_template( CONST TS_UUID *b )
{
	T_JSON	root;
	UB	*m;
	INT	n = 0, i;

	for ( i = 0; i < g_n; i++ ) {
		if ( memcmp(&g_ent[i].id, b, sizeof(*b)) == 0 ) return;
	}
	if ( g_n >= BC_MAX || ( m = meta_of(b, &n) ) == NULL ) return;
	g_ent[g_n].id = *b;
	g_ent[g_n].name[0] = 0;
	if ( js_parse(m, n, &root) >= E_OK ) {
		(void)js_get_str(&root, "name", g_ent[g_n].name, TAD_NAME_MAX);
	}
	free(m);
	g_n++;
}

/* The templates of the applications of the program box, in its order */
static void gather( void )
{
	TS_UUID	box, *lk;
	INT	n = 0, all = 0, i;

	g_n = 0;
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) < E_OK
	  || ob_lst_lnk(&box, NULL, NULL, 0, &n) < E_OK || n <= 0 ) {
		return;
	}
	lk = malloc(sizeof(TS_UUID) * (size_t)n);
	if ( lk == NULL ) return;
	if ( ob_lst_lnk(&box, NULL, lk, n, &all) < E_OK ) all = 0;
	if ( all < n ) n = all;
	for ( i = 0; i < n; i++ ) {
		T_JSON	root, tf, g, base, it;
		UB	*m, kind[16], us[TS_UUID_STRLEN + 1];
		INT	len = 0;

		if ( ( m = meta_of(&lk[i], &len) ) == NULL ) continue;
		if ( js_parse(m, len, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
		  && js_get(&tf, "program", &g) >= E_OK
		  && ( js_get_str(&g, "kind", kind, sizeof(kind)) <= 0 || strcmp((char *)kind, "app") == 0 )
		  && js_get(&g, "base", &base) >= E_OK ) {
			for ( it.s = NULL; js_next(&base, &it); ) {
				TS_UUID	b;

				if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
				  && ts_str_to_uuid((CONST char *)us, &b) >= E_OK ) {
					put_template(&b);
				}
			}
		}
		free(m);
	}
	free(lk);
}

/* ---------------------------------------------------------------- the list as a figure */

/* A closed link's width: the name, the picture and the frame, at least 2 letters */
static INT link_w( CONST UB *name )
{
	INT	w = dp_text_width(name, BC_CHSZ);

	if ( w < 0 ) w = BC_CHSZ * 6;
	w += ( BC_CHSZ / 2 ) * 3 + BC_CHSZ;
	return ( w < BC_CHSZ * 2 ) ? BC_CHSZ * 2 : w;
}

/* The record: one closed link to each template, one to a row */
static void make_list( void )
{
	INT	max = 256 + g_n * ( 400 + TAD_NAME_MAX * 6 ), at, i, y = BC_PAD;
	char	*x = malloc((size_t)max), us[TS_UUID_STRLEN + 1];
	T_TAD	*rec = NULL;
	T_TVFIG	*fig = NULL;
	char	s[64];

	if ( x == NULL ) return;
	at = snprintf(x, (size_t)max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"原紙集め\">\n<figure>\n");
	for ( i = 0; i < g_n && at + 400 + TAD_NAME_MAX * 6 < max; i++ ) {
		ts_uuid_text(&g_ent[i].id, us);
		at += snprintf(x + at, (size_t)( max - at ),
			       "<link id=\"%s_0.xtad\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\" vobjbottom=\"%d\" "
			       "height=\"%d\" chsz=\"%d\" frcol=\"#000000\" chcol=\"#000000\" tbcol=\"#ffffff\" "
			       "bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\"/>\n",
			       us, BC_PAD, y, BC_PAD + link_w(g_ent[i].name), y + BC_BAND, BC_BAND, BC_CHSZ);
		y += BC_BAND + BC_GAP;
	}
	at += snprintf(x + at, (size_t)( max - at ), "</figure>\n</tad>\n");
	if ( tad_parse((CONST UB *)x, (SZ)at, NULL, &rec) >= E_OK && tv_fig(rec, &fig) >= E_OK ) {
		if ( g_fig != NULL ) tv_fig_free(g_fig);
		if ( g_rec != NULL ) tad_free(g_rec);
		g_rec = rec;
		g_fig = fig;
	} else if ( rec != NULL ) {
		tad_free(rec);
	}
	free(x);
	snprintf(s, sizeof(s), "%d templates", g_n);
	note(s);
}

/* ---------------------------------------------------------------- the window */

static void work_rect( T_DPRECT *r )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	r->left = r->top = 0;
	r->right = 200;
	r->bottom = 400;
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		r->right = wp.wright - wp.wleft;
		r->bottom = wp.wbottom - wp.wtop;
	}
}

static INT list_h( void )
{
	return BC_PAD + g_n * ( BC_BAND + BC_GAP );
}

static ER draw( void )
{
	T_DPRECT	all;

	work_rect(&all);
	(void)dp_fill_rect(g_gid, &all, BC_PAPER);
	if ( g_fig != NULL ) {
		(void)tv_fig_draw(g_gid, g_fig, &all, 0, g_sy, g_rec, BC_PAPER);
	}
	return wm_obj_flush(g_kw, NULL);
}

/* Scrolled by dy pixels, kept within the list */
static void scroll_by( INT dy )
{
	T_DPRECT	all;
	INT		most;

	work_rect(&all);
	most = list_h() - ( all.bottom - all.top );
	g_sy += dy;
	if ( g_sy > most ) g_sy = most;
	if ( g_sy < 0 ) g_sy = 0;
}

static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
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

/*
 * The button down over a template: the desktop carries its outline, and
 * where it is let go makes a new object from it (DT_F_BASE). A press
 * that goes up where it went down carries nothing.
 */
static void press( CONST T_OBNTM *m )
{
	T_DPRECT	all, box;
	T_VOBJ		v;
	T_DTREQ		rq;
	T_DTANS		an;
	INT		which = -1, i;
	ER		er;
	char		s[160];

	if ( g_fig == NULL ) return;
	work_rect(&all);
	if ( tv_fig_at(g_fig, &all, 0, g_sy, g_rec, m->x, m->y, &v, &box, &which) < E_OK ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.target = v.target;
	rq.recno = -1;
	rq.look.w = box.right - box.left;
	rq.look.h = box.bottom - box.top;
	rq.look.chsz = BC_CHSZ;
	rq.look.frcol = rq.look.chcol = 0x000000;
	rq.look.tbcol = rq.look.bgcol = 0xFFFFFF;
	rq.look.disp = TAD_D_DEFAULT;
	rq.grab_x = m->x - box.left;
	rq.grab_y = m->y - box.top;
	rq.flags = DT_F_BASE | DT_F_TO_PROG;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	for ( i = 0; i < g_n && memcmp(&g_ent[i].id, &v.target, sizeof(v.target)) != 0; i++ ) ;
	if ( er != E_OBJ ) {
		snprintf(s, sizeof(s), "carried %s (%d)", ( i < g_n ) ? (char *)g_ent[i].name : "?", (INT)er);
		note(s);
	}
}

static BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

/* The second button: the menu; TRUE when 閉じる was chosen */
static BOOL menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	done = FALSE;

	if ( ts_str_to_uuid(SYSDEF_MENU_BASECOL, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) {
		return FALSE;
	}
	if ( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &sel) >= E_OK ) {
		if ( is_code(&sel, "close") ) {
			done = TRUE;
		} else if ( is_code(&sel, "reread") ) {
			gather();
			make_list();
		}
	}
	(void)mn_del_men(mid);
	return done;
}

int main( void )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBNTM		m;
	TS_UUID		ch, box;
	char		json[200];
	SZ		asz = 0;
	BOOL		done = FALSE;
	INT		h;
	ID		port, kbox = 0;

	tv_src_init();
	gather();
	make_list();

	/* the window, as tall as the list (at most most of the screen) */
	h = list_h() + 48;
	if ( h < 160 ) h = 160;
	if ( h > 640 ) h = 640;
	snprintf(json, sizeof(json), "{\"rect\":[660,40,880,%d],\"attr\":%d,\"icon\":\"%s\"}",
		 40 + h, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, SYSDEF_PROG_BASECOL);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"原紙集め";
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) {
		note("no window");
		return 1;
	}
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : E_ID;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_REDRAW | OB_E_DELETE | OB_E_WHEEL;
	if ( g_kw <= 0 || g_gid < 0 || port <= 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, port) <= 0 ) {
		note("the window cannot be used");
		(void)ob_del_obj(&g_win);
		return 2;
	}
	/* the program box: a program installed or deleted is seen at once */
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) >= E_OK
	  && ( kbox = ob_opn_obj(&box, OB_OP_ATRRD) ) > 0 ) {
		memset(&req, 0, sizeof(req));
		req.events = OB_E_CHANGE;
		(void)ob_ntf_evt(kbox, OB_REC_ANY, &req, port);
	}
	note("up");
	(void)draw();

	while ( !done ) {
		BOOL	again = FALSE;

		if ( ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, TMO_FEVR) < E_OK ) break;
		do {
			if ( memcmp(&m.uuid, &g_win, sizeof(g_win)) != 0 ) {
				if ( m.event == OB_E_CHANGE ) {
					gather();
					make_list();
					again = TRUE;
				}
			} else if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				done = TRUE;
			} else if ( m.event == OB_E_PRESS && m.code == 1 ) {
				done = menu(&m);
				again = TRUE;
			} else if ( m.event == OB_E_PRESS ) {
				press(&m);
				again = TRUE;
			} else if ( m.event == OB_E_WHEEL ) {
				scroll_by(m.dz * ( BC_BAND + BC_GAP ));
				again = TRUE;
			} else if ( m.event == OB_E_REDRAW ) {
				again = TRUE;
			}
		} while ( !done && ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, TMO_POL) >= E_OK );
		if ( !done && again && draw() < E_OK ) break;
	}

	note("closed");
	if ( kbox > 0 ) ob_cls_obj(kbox);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	ob_cls_obj(g_kw);
	(void)ob_del_obj(&g_win);
	if ( g_fig != NULL ) tv_fig_free(g_fig);
	if ( g_rec != NULL ) tad_free(g_rec);
	return 0;
}
