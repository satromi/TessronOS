/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_calc.c
 *	基本表計算 (design 11.13): the sheet's record, and the program's own test
 *
 *	The records of application/calc/calc_cases.h are read and written
 *	again, and must come out byte for byte as the plugin's own reading
 *	and writing of them did (tools/calc_cases.js); so must the loading
 *	conversion of a cell (the marks of full width, ［row，col］) and the
 *	moving of a formula's references when it is copied. The sample
 *	sheet the system brings (基本表計算 in the samples) is read for its
 *	cells, look, links, sizes, joins and charts, and written twice to
 *	the same bytes. A row and a column are put in and taken out, and a
 *	range framed. The menus the 操作パネル opens are made, and a window
 *	made a sub window of another as the program makes its panel is
 *	carried and raised with it. Then the program itself is started with
 *	"selftest": it works out the sheets of the table with its engine, V8
 *	or its own, draws the charts and compares them, works the panel's
 *	parts, and answers 0 when all match.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/mn.h>
#include "../../application/calc/calc_tad.h"
#include "../../application/calc/calc_cases.h"

#define N_OF(a)		( (INT)( sizeof(a) / sizeof((a)[0]) ) )
#define OUT_MAX		( 256 * 1024 )
#define SAMPLE		"019b15f8-72b8-7151-a57f-149fb1f59a17"
#define RUN_TMO		600000		/* ms: the program with V8 read in and started under TCG */

EXPORT void *cx_alloc( INT size )
{
	return Kmalloc((SZ)( size > 0 ? size : 1 ));
}

EXPORT void cx_free( void *p )
{
	if ( p != NULL ) Kfree(p);
}

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL s_eq( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 );
}

/* Where two strings first differ, -1 when they do not */
LOCAL INT differ( CONST UB *a, INT n, CONST char *b )
{
	INT	i, m = s_len(b);

	for ( i = 0; i < n && i < m; i++ ) {
		if ( a[i] != (UB)b[i] ) return i;
	}
	return ( n == m ) ? -1 : i;
}

LOCAL void test_names( void )
{
	char	nm[16];
	INT	col = 0, row = 0;
	CXRANGE	r;

	KT_ASSERT_EQ(cx_col_name(1, nm), 1);
	KT_ASSERT(s_eq((UB *)nm, "A"));
	(void)cx_col_name(26, nm);
	KT_ASSERT(s_eq((UB *)nm, "Z"));
	(void)cx_col_name(27, nm);
	KT_ASSERT(s_eq((UB *)nm, "AA"));
	(void)cx_col_name(16384, nm);
	KT_ASSERT(s_eq((UB *)nm, "XFD"));
	KT_ASSERT_EQ(cx_col_num((CONST UB *)"XFD", 3), 16384);
	(void)cx_cell_name(28, 107, nm);
	KT_ASSERT(s_eq((UB *)nm, "AB107"));
	KT_ASSERT(cx_cell_parse((CONST UB *)"C12", 3, &col, &row));
	KT_ASSERT_EQ(col, 3);
	KT_ASSERT_EQ(row, 12);
	KT_ASSERT(!cx_cell_parse((CONST UB *)"c12", 3, &col, &row));
	KT_ASSERT(cx_range_parse((CONST UB *)"E7:G13", 6, &r));
	KT_ASSERT_EQ(r.c0, 5);
	KT_ASSERT_EQ(r.r1, 13);
}

/* What a cell read holds: as the plugin's conversion makes it */
LOCAL void test_old_form( void )
{
	UB	out[256];
	INT	i, n;

	for ( i = 0; i < N_OF(calc_old); i++ ) {
		n = cx_old_form((CONST UB *)calc_old[i].in, s_len(calc_old[i].in), out, sizeof(out));
		if ( differ(out, n, calc_old[i].out) >= 0 ) {
			tm_printf((UB *)"  conversion %d: \"%s\", not \"%s\"\n", i, out, calc_old[i].out);
		}
		KT_ASSERT_EQ(differ(out, n, calc_old[i].out), -1);
	}
}

/* A formula copied: its references moved as the plugin moves them */
LOCAL void test_refs( void )
{
	UB	out[256];
	INT	i, n;

	for ( i = 0; i < N_OF(calc_adj); i++ ) {
		n = cx_move_refs((CONST UB *)calc_adj[i].in, s_len(calc_adj[i].in), calc_adj[i].dc, calc_adj[i].dr,
				 CX_COLS, CX_ROWS, out, sizeof(out));
		if ( differ(out, n, calc_adj[i].out) >= 0 ) {
			tm_printf((UB *)"  move %d: \"%s\", not \"%s\"\n", i, out, calc_adj[i].out);
		}
		KT_ASSERT_EQ(differ(out, n, calc_adj[i].out), -1);
	}
}

/* A record read and written again: as the plugin writes it */
LOCAL void test_records( void )
{
	UB	*out = (UB *)Kmalloc(OUT_MAX);
	INT	i;

	KT_ASSERT(out != NULL);
	if ( out == NULL ) return;
	for ( i = 0; i < N_OF(calc_records); i++ ) {
		CXSHEET		s;
		CTBUF		b;
		UB		name[64];
		CONST char	*in = calc_records[i].in;
		INT		k = 0, at, d;

		at = 0;
		while ( in[at] != 0 && !( in[at] == 'n' && in[at + 1] == 'a' && in[at + 2] == 'm' && in[at + 3] == 'e'
				   && in[at + 4] == '=' ) ) at++;
		at += 6;
		while ( in[at] != 0 && in[at] != '"' && k < 63 ) name[k++] = (UB)in[at++];
		name[k] = 0;
		cx_init(&s);
		KT_ASSERT_ER(cx_read(&s, (CONST UB *)in, s_len(in)), E_OK);
		ct_buf(&b, out, OUT_MAX);
		cx_write(&b, &s, name, NULL, NULL);
		KT_ASSERT(!b.fail);
		d = differ(b.p, b.n, calc_records[i].out);
		if ( d >= 0 ) {
			tm_printf((UB *)"  record %d differs at %d: \"%.60s\"\n", i, d, b.p + d);
		}
		KT_ASSERT_EQ(d, -1);
		cx_free_all(&s);
	}
	Kfree(out);
}

/* The sample sheet: what it holds, and written twice the same */
LOCAL void test_sample( void )
{
	TS_UUID		u;
	SZ		size = 0;
	UB		*x, *out1, *out2;
	CXSHEET		s;
	CTBUF		b, c;
	CXCELL		*cl;
	TS_UUID		t;
	INT		i;

	KT_ASSERT_ER(ts_str_to_uuid(SAMPLE, &u), E_OK);
	x = om_obj_record(&u, 0, &size);
	if ( x == NULL ) KT_SKIP("no sample sheet on the system volume");
	out1 = (UB *)Kmalloc(OUT_MAX);
	out2 = (UB *)Kmalloc(OUT_MAX);
	KT_ASSERT(out1 != NULL && out2 != NULL);
	cx_init(&s);
	KT_ASSERT_ER(cx_read(&s, x, (INT)size), E_OK);
	tm_printf((UB *)"  %d bytes: %d cells, %d charts, %d joins\n", (INT)size, s.n, s.nch, s.nm);
	KT_ASSERT(s.paper != NULL);
	KT_ASSERT(s.margin != NULL);
	KT_ASSERT_EQ(cx_width(&s, 2), 137);
	KT_ASSERT_EQ(cx_width(&s, 4), 245);
	KT_ASSERT_EQ(cx_width(&s, 3), CX_CELL_W);
	KT_ASSERT_EQ(cx_height(&s, 5), 49);
	KT_ASSERT_EQ(cx_height(&s, 6), 55);
	cl = cx_get(&s, 2, 5);
	KT_ASSERT(cl != NULL && cl->kind == CX_TEXT && s_eq(cl->text, "仮身も入れられます"));
	cl = cx_get(&s, 4, 5);
	KT_ASSERT(cl != NULL && cl->kind == CX_LINK);
	if ( cl != NULL && cl->kind == CX_LINK ) {
		KT_ASSERT(ct_target(cl->el, cl->ellen, &t));
		KT_ASSERT_EQ(ct_attr_int(cl->el, cl->ellen, "width", 0), 143);
	}
	cl = cx_get(&s, 5, 7);
	KT_ASSERT(cl != NULL && s_eq(cl->text, "A"));
	KT_ASSERT(cl != NULL && cl->st->side[CX_TOP].on && cl->st->side[CX_LEFT].on && cl->st->side[CX_RIGHT].on
		  && cl->st->side[CX_BOTTOM].on);
	cl = cx_get(&s, 7, 13);
	KT_ASSERT(cl != NULL && s_eq(cl->text, "13"));
	KT_ASSERT_EQ(s.nch, 2);
	if ( s.nch == 2 ) {
		KT_ASSERT_EQ(s.ch[0].kind, CX_CH_BAR);
		KT_ASSERT_EQ(s.ch[1].kind, CX_CH_LINE);
		KT_ASSERT(s.ch[0].has_range && s.ch[0].range.c0 == 5 && s.ch[0].range.r0 == 7
			  && s.ch[0].range.c1 == 7 && s.ch[0].range.r1 == 13);
		KT_ASSERT_EQ(s.ch[0].left, 574);
		KT_ASSERT_EQ(s.ch[0].bottom, 595);
	}
	ct_buf(&b, out1, OUT_MAX);
	cx_write(&b, &s, (CONST UB *)"基本表計算", NULL, NULL);
	KT_ASSERT(!b.fail);
	cx_free_all(&s);

	/* and read back: the same sheet, the same bytes */
	cx_init(&s);
	KT_ASSERT_ER(cx_read(&s, b.p, b.n), E_OK);
	ct_buf(&c, out2, OUT_MAX);
	cx_write(&c, &s, (CONST UB *)"基本表計算", NULL, NULL);
	KT_ASSERT_EQ(c.n, b.n);
	KT_ASSERT_EQ(differ(c.p, c.n, (CONST char *)b.p), -1);
	for ( i = 0; i < s.n && s.c[i]->kind != CX_LINK; i++ ) ;
	KT_ASSERT(i < s.n);
	cx_free_all(&s);
	Kfree(out1);
	Kfree(out2);
	Kfree(x);
}

/* Rows and columns put in and taken out, a range framed */
LOCAL void test_edit( void )
{
	CXSHEET	s;
	CXCELL	*c;
	CXRANGE	r;
	CXSIDE	sd;
	INT	col, row;

	cx_init(&s);
	for ( row = 1; row <= 3; row++ ) {
		for ( col = 1; col <= 3; col++ ) {
			char	t[8];

			t[0] = (char)( 'a' + col - 1 );
			t[1] = (char)( '0' + row );
			t[2] = 0;
			c = cx_make(&s, col, row);
			KT_ASSERT(c != NULL && cx_set_text(c, (CONST UB *)t));
		}
	}
	cx_set_height(&s, 2, 40);
	cx_set_width(&s, 3, 120);
	cx_insert_row(&s, 2);
	KT_ASSERT(cx_get(&s, 1, 2) == NULL);
	KT_ASSERT(s_eq(cx_get(&s, 1, 3)->text, "a2"));
	KT_ASSERT_EQ(cx_height(&s, 3), 40);
	KT_ASSERT_EQ(cx_height(&s, 2), CX_CELL_H);
	cx_delete_col(&s, 1);
	KT_ASSERT(s_eq(cx_get(&s, 1, 1)->text, "b1"));
	KT_ASSERT_EQ(cx_width(&s, 2), 120);
	cx_delete_row(&s, 2);
	KT_ASSERT(s_eq(cx_get(&s, 2, 2)->text, "c2"));
	KT_ASSERT_EQ(s.n, 6);

	r.c0 = 1;
	r.r0 = 1;
	r.c1 = 2;
	r.r1 = 2;
	sd.on = TRUE;
	sd.type = CX_DASHED;
	sd.width = 2;
	sd.colour[0] = '#'; sd.colour[1] = 'f'; sd.colour[2] = 'f'; sd.colour[3] = '0';
	sd.colour[4] = '0'; sd.colour[5] = '0'; sd.colour[6] = '0'; sd.colour[7] = 0;
	cx_frame_range(&s, &r, CX_FR_OUTLINE, &sd);
	c = cx_get(&s, 1, 1);
	KT_ASSERT(c->st->side[CX_TOP].on && c->st->side[CX_LEFT].on && !c->st->side[CX_RIGHT].on && !c->st->side[CX_BOTTOM].on);
	c = cx_get(&s, 2, 2);
	KT_ASSERT(!c->st->side[CX_TOP].on && c->st->side[CX_RIGHT].on && c->st->side[CX_BOTTOM].on);
	KT_ASSERT_EQ(c->st->side[CX_RIGHT].type, CX_DASHED);
	cx_frame_range(&s, &r, CX_FR_NONE, &sd);
	KT_ASSERT(!cx_get(&s, 2, 2)->st->side[CX_RIGHT].on);
	KT_ASSERT(cx_merge_add(&s, &r));
	KT_ASSERT_EQ(cx_merge_of(&s, 2, 2), 0);
	KT_ASSERT_EQ(cx_merge_of(&s, 3, 2), -1);
	cx_free_all(&s);
}

/* A window made as the program makes its windows; its number */
LOCAL INT made( CONST char *name, CONST char *json, TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)name;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)knl_strlen(json);
	if ( ob_cre_obj(&c, u) < E_OK ) return -1;
	return wm_obj_wid(u);
}

/*
 * The 操作パネル: the menus its buttons open are there and are made; its
 * window, made a sub window of the sheet's with "sub" as the program
 * makes it, is carried with the sheet's window, stops at the top of the
 * screen, and when it is raised the sheet's window comes forward with
 * it, the panel just in front.
 */
LOCAL void test_panel( void )
{
	static CONST char *CONST menus[] = {
		SYSDEF_MENU_CALC_FACE, SYSDEF_MENU_CALC_SIZE, SYSDEF_MENU_CALC_COLOUR, SYSDEF_MENU_CALC_GROUND,
		SYSDEF_MENU_CALC_LCOLOUR, SYSDEF_MENU_CALC_ALIGN, SYSDEF_MENU_CALC_VALIGN, SYSDEF_MENU_CALC_LINE,
		SYSDEF_MENU_CALC_LWIDTH, SYSDEF_MENU_CALC
	};
	TS_UUID		u, win, sub;
	T_WMWIN		w, s0, s1;
	T_DPRECT	o;
	char		j[160], par[TS_UUID_STRLEN + 1];
	INT		i, wid, sid, other, sw = 0, sh = 0;
	ID		mid;

	for ( i = 0; i < N_OF(menus); i++ ) {
		KT_ASSERT_ER(ts_str_to_uuid(menus[i], &u), E_OK);
		KT_ASSERT_ER(mn_cre_men(&u, &mid), E_OK);
		(void)mn_del_men(mid);
	}

	wm_screen(&sw, &sh);
	if ( sw <= 0 ) KT_SKIP("no windows");
	wid = made("calc sheet", "{\"rect\":[200,300,600,600],\"attr\":7}", &win);
	KT_ASSERT(wid > 0);
	if ( wid <= 0 ) return;
	(void)ts_uuid_to_str(&win, par, sizeof(par));
	knl_strcpy(j, "{\"rect\":[220,200,540,296],\"attr\":1,\"sub\":\"");
	knl_strcat(j, par);
	knl_strcat(j, "\"}");
	sid = made("calc panel", j, &sub);
	KT_ASSERT(sid > 0);
	if ( sid > 0 && wm_ref(sid, &s0) >= E_OK && wm_ref(wid, &w) >= E_OK ) {
		KT_ASSERT_EQ(s0.parent, wid);

		/* carried with the sheet's window */
		o = w.outer;
		o.left += 30;  o.right += 30;
		o.top += 40;   o.bottom += 40;
		KT_ASSERT_ER(wm_move(wid, &o), E_OK);
		KT_ASSERT_ER(wm_ref(sid, &s1), E_OK);
		KT_ASSERT_EQ(s1.outer.left, s0.outer.left + 30);
		KT_ASSERT_EQ(s1.outer.top, s0.outer.top + 40);
		KT_ASSERT_EQ(s1.outer.right - s1.outer.left, s0.outer.right - s0.outer.left);

		/* only resized: it stays */
		o.right += 50;
		KT_ASSERT_ER(wm_move(wid, &o), E_OK);
		KT_ASSERT_ER(wm_ref(sid, &s0), E_OK);
		KT_ASSERT_EQ(s0.outer.left, s1.outer.left);
		KT_ASSERT_EQ(s0.outer.top, s1.outer.top);

		/* to the top of the screen: the panel above it stops there */
		o.bottom -= o.top;
		o.top = 0;
		KT_ASSERT_ER(wm_move(wid, &o), E_OK);
		KT_ASSERT_ER(wm_ref(sid, &s1), E_OK);
		KT_ASSERT_EQ(s1.outer.top, 0);
		KT_ASSERT_EQ(s1.outer.bottom - s1.outer.top, s0.outer.bottom - s0.outer.top);

		/* another window in front; the panel raised brings the sheet's forward */
		o.left = 100;  o.top = 100;  o.right = 700;  o.bottom = 700;
		other = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "calc other");
		KT_ASSERT(other > 0);
		KT_ASSERT_ER(wm_raise(sid), E_OK);
		KT_ASSERT_ER(wm_ref(sid, &s1), E_OK);
		KT_ASSERT_ER(wm_ref(wid, &w), E_OK);
		KT_ASSERT_EQ(s1.z, 0);
		KT_ASSERT_EQ(w.z, 1);
		if ( other > 0 ) (void)wm_close(other);
	}
	if ( sid > 0 ) (void)ob_del_obj(&sub);
	(void)ob_del_obj(&win);
}

/* The program started with "selftest": its formulas, charts and records checked by itself */
LOCAL void test_program( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	SYSTIM	t0, t1;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_CALC, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	(void)tk_get_otm(&t0);
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of 基本表計算");
	}
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, RUN_TMO);
	(void)tk_get_otm(&t1);
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
		return;
	}
	tm_printf((UB *)"  exit code %d in %d ms\n", psts.exitcd, (INT)( t1.lo - t0.lo ));
	KT_ASSERT_EQ(psts.exitcd, 0);
}

EXPORT void ktest_calc( void )
{
	tm_printf((UB *)"\n[calc] 基本表計算 (design 11.13)\n");
	tm_printf((UB *)"  %d sheets, %d charts in the table\n", N_OF(calc_sheets), N_OF(calc_charts));
	(void)calc_chart_cells;
	KT_RUN(test_names);
	KT_RUN(test_old_form);
	KT_RUN(test_refs);
	KT_RUN(test_records);
	KT_RUN(test_sample);
	KT_RUN(test_edit);
	KT_RUN(test_panel);
	KT_RUN(test_program);
}
