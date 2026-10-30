/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_calcu.c
 *	電卓 (design 11.17): decimal numbers, the keys, the tray
 *
 *	The decimal numbers (application/calculator/cu_dec.c) are read,
 *	written and worked with directly: sums of fractions that a binary
 *	fraction cannot hold come out exact, results with more digits than
 *	are kept are rounded half away from zero, the display rounds to its
 *	places and goes over to an exponent, and overflow is caught. The
 *	engine (cu_eng.c) works the cases of cu_cases.h that need no double
 *	precision: the four operations, %, √, the memory, = again, the
 *	plain keys' order and the function keys' ranks and brackets. The
 *	keyboard's keys are turned into the calculator's, the numeric keypad
 *	among them, and numbers are read from the texts a tray may hold.
 *
 *	Then the program is started: keys typed into its window (the digits
 *	of the main rows, the keypad, Enter) give the sum, which Ctrl+C puts
 *	in the tray as a text; a text with a number written in full width
 *	put in the tray and taken with Ctrl+V is worked on; the switch to
 *	the function calculator is pressed in its menu's place by a key, and
 *	Ctrl+E closes. Last the program is started with "selftest", which
 *	works every case with its functions in double precision.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/tray.h>
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/calculator/cu_dec.h"
#include "../../application/calculator/cu_eng.h"
#include "../../application/calculator/cu_cases.h"

#define N_OF(a)		( (INT)( sizeof(a) / sizeof((a)[0]) ) )
#define RUN_TMO		120000		/* ms: the selftest under TCG */

LOCAL BOOL same( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

/* Whether s holds w */
LOCAL BOOL holds( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, m = (INT)knl_strlen(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

LOCAL CUDEC num( CONST char *s )
{
	CUDEC	v;

	cu_zero(&v);
	(void)cu_parse(&v, s, (INT)knl_strlen(s), NULL);
	return v;
}

/* The display's form of a */
LOCAL CONST char *shown( CONST CUDEC *a )
{
	LOCAL char	b[64];

	(void)cu_format(a, b, sizeof(b), TRUE);
	return b;
}

/* Every digit of a */
LOCAL CONST char *digits( CONST CUDEC *a )
{
	LOCAL char	b[64];

	(void)cu_string(a, b, sizeof(b));
	return b;
}

#define SAYS(expr, want) do { \
	CONST char *got_ = ( expr ); \
	if ( !same(got_, want) ) tm_printf((UB *)"  %s: \"%s\", not \"%s\"\n", #expr, got_, want); \
	KT_ASSERT(same(got_, want)); \
} while ( 0 )

/* ---------------------------------------------------------------- the numbers */

LOCAL void test_decimal( void )
{
	CUDEC	a, b, r;
	D	v = 0;
	INT	used = 0;

	/* read, and kept in one form */
	a = num("0.1");
	KT_ASSERT(a.c == 1 && a.e == -1 && !a.neg);
	a = num("120.500");
	KT_ASSERT(a.c == 1205 && a.e == -1);
	a = num("-0");
	KT_ASSERT(a.c == 0 && !a.neg);
	KT_ASSERT_EQ(cu_parse(&a, "1.5E3xyz", 8, &used), CU_OK);
	KT_ASSERT_EQ(used, 5);
	SAYS(digits(&a), "15E2");
	KT_ASSERT_EQ(cu_parse(&a, "abc", 3, NULL), CU_ESYN);
	KT_ASSERT_EQ(cu_parse(&a, "1E100", 5, NULL), CU_EOVER);
	a = num("1E-100");
	KT_ASSERT(cu_is_zero(&a));

	/* sums a binary fraction cannot hold */
	a = num("0.1");
	b = num("0.2");
	KT_ASSERT_EQ(cu_add(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "3E-1");
	KT_ASSERT_EQ(cu_cmp(&r, &(CUDEC){ 3, -1, FALSE }), 0);
	a = num("1.1");
	KT_ASSERT_EQ(cu_mul(&r, &a, &a), CU_OK);
	SAYS(digits(&r), "121E-2");
	a = num("1");
	b = num("0.9");
	KT_ASSERT_EQ(cu_sub(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "1E-1");

	/* digits past what is kept: rounded half away from zero */
	a = num("1");
	b = num("3");
	KT_ASSERT_EQ(cu_div(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "3333333333333333333E-19");
	b = num("-1.5");
	KT_ASSERT_EQ(cu_div(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "-6666666666666666667E-19");
	a = num("12345678901234567895");			/* 20 digits */
	SAYS(digits(&a), "123456789012345679E2");
	a = num("1E30");
	b = num("1");
	KT_ASSERT_EQ(cu_add(&r, &a, &b), CU_OK);		/* far apart: the 1 is lost */
	SAYS(digits(&r), "1E30");
	a = num("9999999999999999999");
	b = num("0.5");
	KT_ASSERT_EQ(cu_add(&r, &a, &b), CU_OK);		/* a half rounds up */
	SAYS(digits(&r), "1E19");
	b = num("0.4999999999999");
	KT_ASSERT_EQ(cu_add(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "9999999999999999999");
	a = num("1E40");
	b = num("-0.5");
	KT_ASSERT_EQ(cu_add(&r, &a, &b), CU_OK);
	SAYS(digits(&r), "1E40");

	/* rounding to places and to digits */
	a = num("2.5");
	KT_ASSERT_EQ(cu_round_at(&r, &a, 0), CU_OK);
	SAYS(digits(&r), "3");
	a = num("-2.5");
	KT_ASSERT_EQ(cu_round_at(&r, &a, 0), CU_OK);
	SAYS(digits(&r), "-3");
	a = num("0.04");
	KT_ASSERT_EQ(cu_round_at(&r, &a, 1), CU_OK);
	KT_ASSERT(cu_is_zero(&r));
	a = num("0.05");
	KT_ASSERT_EQ(cu_round_at(&r, &a, 1), CU_OK);
	SAYS(digits(&r), "1E-1");
	a = num("123456");
	KT_ASSERT_EQ(cu_round_sig(&r, &a, 3), CU_OK);
	SAYS(digits(&r), "123E3");

	/* roots, powers, the whole part */
	a = num("2.25");
	KT_ASSERT_EQ(cu_sqrt(&r, &a), CU_OK);
	SAYS(digits(&r), "15E-1");
	a = num("0.0001");
	KT_ASSERT_EQ(cu_sqrt(&r, &a), CU_OK);
	SAYS(digits(&r), "1E-2");
	a = num("-4");
	KT_ASSERT_EQ(cu_sqrt(&r, &a), CU_EDOM);
	a = num("1.5");
	KT_ASSERT_EQ(cu_ipow(&r, &a, 3), CU_OK);
	SAYS(digits(&r), "3375E-3");
	a = num("2");
	KT_ASSERT_EQ(cu_ipow(&r, &a, -2), CU_OK);
	SAYS(digits(&r), "25E-2");
	a = num("10");
	KT_ASSERT_EQ(cu_ipow(&r, &a, 100), CU_EOVER);
	a = num("-2.5");
	KT_ASSERT_EQ(cu_floor(&r, &a), CU_OK);
	SAYS(digits(&r), "-3");
	a = num("1E18");
	KT_ASSERT_EQ(cu_to_int(&a, &v), CU_EDOM);
	a = num("-123456789012345678");
	KT_ASSERT_EQ(cu_to_int(&a, &v), CU_OK);
	KT_ASSERT(v == -123456789012345678LL);

	/* the end of the range */
	a = num("9E99");
	b = num("10");
	KT_ASSERT_EQ(cu_mul(&r, &a, &b), CU_EOVER);
	KT_ASSERT_EQ(cu_div(&r, &b, &(CUDEC){ 0, 0, FALSE }), CU_EDIV0);
	a = num("1E-60");
	KT_ASSERT_EQ(cu_mul(&r, &a, &a), CU_OK);
	KT_ASSERT(cu_is_zero(&r));
	KT_ASSERT_EQ(cu_cmp(&(CUDEC){ 9, 99, FALSE }, &(CUDEC){ 1, -99, TRUE }), 1);

	/* as the display shows it */
	a = num("1234567.89");
	SAYS(shown(&a), "1,234,567.89");
	a = num("-1000");
	SAYS(shown(&a), "-1,000");
	a = num("0.000001");
	SAYS(shown(&a), "0.000001");
	a = num("0.0000001");
	SAYS(shown(&a), "1E-7");
	a = num("999999999999.4");
	SAYS(shown(&a), "999,999,999,999");
	a = num("999999999999.5");
	SAYS(shown(&a), "1E+12");
	a = num("-12345678901234");
	SAYS(shown(&a), "-1.23456789E+13");
	a = num("1.5E-30");
	SAYS(shown(&a), "1.5E-30");
	a = num("0.1234567890123456");
	SAYS(shown(&a), "0.12345678901");
}

/* ---------------------------------------------------------------- the keys */

LOCAL void test_cases( void )
{
	char	show[64], expr[64];
	INT	i, n = 0, bad = 0;

	for ( i = 0; i < N_OF(cu_cases); i++ ) {
		CONST CUCASE	*k = &cu_cases[i];

		if ( k->fn ) continue;
		n++;
		cu_case_run(k, NULL, show, expr);
		if ( !same(show, k->show) || ( k->expr != NULL && !same(expr, k->expr) ) ) {
			tm_printf((UB *)"  case %d \"%s\": \"%s\" [%s], not \"%s\" [%s]\n", i, k->keys, show, expr,
				  k->show, ( k->expr != NULL ) ? k->expr : "-");
			bad++;
		}
	}
	tm_printf((UB *)"  %d cases\n", n);
	KT_ASSERT_EQ(bad, 0);
}

/* Without the program's functions those in double precision are errors, not wrong numbers */
LOCAL void test_no_fn( void )
{
	CUENG	g;
	char	s[64];

	cu_init(&g, TRUE, NULL);
	cu_key(&g, CU_K_DIGIT(3));
	cu_key(&g, CU_K_DIGIT(0));
	cu_key(&g, CU_K_SIN);
	(void)cu_show(&g, s, sizeof(s));
	SAYS(s, "E");
	KT_ASSERT_EQ(g.err, CU_EDOM);
	cu_key(&g, CU_K_CE);
	KT_ASSERT_EQ(g.err, CU_OK);
	cu_key(&g, CU_K_DIGIT(2));
	cu_key(&g, CU_K_MPLUS);
	cu_key(&g, CU_K_AC);
	cu_key(&g, CU_K_MR);
	(void)cu_show(&g, s, sizeof(s));
	SAYS(s, "2");					/* the memory outlives 全消去 */
	g.group = FALSE;
	cu_key(&g, CU_K_MUL);
	cu_key(&g, CU_K_DIGIT(1));
	cu_key(&g, CU_K_00);
	cu_key(&g, CU_K_00);
	cu_key(&g, CU_K_EQ);
	(void)cu_show(&g, s, sizeof(s));
	SAYS(s, "20000");
	cu_mode(&g, FALSE);
	KT_ASSERT(!g.sci);
}

LOCAL void test_hid( void )
{
	/* the main rows, by the letters the layout gives */
	KT_ASSERT_EQ(cu_hid_key(0x1E, 0, '1'), CU_K_DIGIT(1));
	KT_ASSERT_EQ(cu_hid_key(0x27, 0, '0'), CU_K_DIGIT(0));
	KT_ASSERT_EQ(cu_hid_key(0x33, HID_MOD_LSHIFT, '+'), CU_K_ADD);
	KT_ASSERT_EQ(cu_hid_key(0x2D, 0, '-'), CU_K_SUB);
	KT_ASSERT_EQ(cu_hid_key(0x34, HID_MOD_LSHIFT, '*'), CU_K_MUL);
	KT_ASSERT_EQ(cu_hid_key(0x38, 0, '/'), CU_K_DIV);
	KT_ASSERT_EQ(cu_hid_key(0x2D, HID_MOD_LSHIFT, '='), CU_K_EQ);
	KT_ASSERT_EQ(cu_hid_key(0x22, HID_MOD_LSHIFT, '%'), CU_K_PCT);
	KT_ASSERT_EQ(cu_hid_key(0x37, 0, '.'), CU_K_POINT);
	KT_ASSERT_EQ(cu_hid_key(0x25, HID_MOD_LSHIFT, '('), CU_K_LP);
	KT_ASSERT_EQ(cu_hid_key(0x2E, 0, '^'), CU_K_POW);
	KT_ASSERT_EQ(cu_hid_key(0x04, 0, 'a'), 0);

	/* the keys of their own */
	KT_ASSERT_EQ(cu_hid_key(0x28, 0, 0), CU_K_EQ);
	KT_ASSERT_EQ(cu_hid_key(0x29, 0, 0), CU_K_AC);
	KT_ASSERT_EQ(cu_hid_key(0x2A, 0, 0), CU_K_BS);
	KT_ASSERT_EQ(cu_hid_key(0x4C, 0, 0), CU_K_CE);

	/* the numeric keypad */
	KT_ASSERT_EQ(cu_hid_key(0x59, 0, 0), CU_K_DIGIT(1));
	KT_ASSERT_EQ(cu_hid_key(0x61, 0, 0), CU_K_DIGIT(9));
	KT_ASSERT_EQ(cu_hid_key(0x62, 0, 0), CU_K_DIGIT(0));
	KT_ASSERT_EQ(cu_hid_key(0x63, 0, 0), CU_K_POINT);
	KT_ASSERT_EQ(cu_hid_key(0x54, 0, 0), CU_K_DIV);
	KT_ASSERT_EQ(cu_hid_key(0x55, 0, 0), CU_K_MUL);
	KT_ASSERT_EQ(cu_hid_key(0x56, 0, 0), CU_K_SUB);
	KT_ASSERT_EQ(cu_hid_key(0x57, 0, 0), CU_K_ADD);
	KT_ASSERT_EQ(cu_hid_key(0x58, 0, 0), CU_K_EQ);

	/* Ctrl and Alt are the program's own */
	KT_ASSERT_EQ(cu_hid_key(0x06, HID_MOD_LCTRL, 'c'), 0);
	KT_ASSERT_EQ(cu_hid_key(0x1E, HID_MOD_RCTRL, '1'), 0);
	KT_ASSERT_EQ(cu_hid_key(0x1E, HID_MOD_LALT, '1'), 0);
}

/* A text from the tray: the number in it */
LOCAL CONST char *pasted( CONST char *text )
{
	LOCAL char	s[64];
	CUENG		g;

	cu_init(&g, FALSE, NULL);
	if ( !cu_paste(&g, (CONST UB *)text, (INT)knl_strlen(text)) ) return "(none)";
	(void)cu_show(&g, s, sizeof(s));
	return s;
}

LOCAL void test_paste( void )
{
	SAYS(pasted("1,234.5"), "1,234.5");
	SAYS(pasted("  合計：１２，３４５円 "), "12,345");
	SAYS(pasted("−3.5℃"), "-3.5");
	SAYS(pasted("－０．２５"), "-0.25");
	SAYS(pasted("x=-0.25 y=3"), "-0.25");
	SAYS(pasted("1.5E3"), "1,500");
	SAYS(pasted("2.5E-7m"), "2.5E-7");
	SAYS(pasted("12E"), "12");
	SAYS(pasted("約.5"), "0.5");
	SAYS(pasted("1,23"), "123");
	SAYS(pasted("12,"), "12");
	SAYS(pasted("文字だけ"), "(none)");
	SAYS(pasted(""), "(none)");
}

/* ---------------------------------------------------------------- the program */

LOCAL INT win_named( CONST char *title, INT ms )
{
	INT	wid, k;
	char	t[WM_TITLE_MAX];

	for ( ; ms > 0; ms -= 100 ) {
		for ( wid = 1; wid <= knl_wm_nwin(); wid++ ) {
			if ( wm_title(wid, t, sizeof(t)) < 0 ) continue;
			for ( k = 0; title[k] != 0 && t[k] == title[k]; k++ ) ;
			if ( title[k] == 0 && t[k] == 0 ) return wid;
		}
		tk_dly_tsk(100);
	}
	return 0;
}

LOCAL void key_to( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.code = code;
	ev.mods = mods;
	ev.wid = wid;
	knl_wmobj_event(&ev);
	tk_dly_tsk(30);
	ev.type = HID_EV_KEY_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
}

/* The text of the set in hand in the tray, waited for until it holds w */
LOCAL BOOL tray_holds( CONST char *w, INT ms )
{
	T_TRINF	inf;
	UB	b[512];
	SZ	asz = 0;

	for ( ; ms > 0; ms -= 100 ) {
		if ( tr_ref_rec(TR_HAND, 0, &inf) >= E_OK && inf.kind == TR_TAD
		  && tr_rea_rec(TR_HAND, 0, b, sizeof(b), &asz) >= E_OK
		  && holds(b, (INT)( ( asz < (SZ)sizeof(b) ) ? asz : (SZ)sizeof(b) ), w) ) {
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  the tray does not hold \"%s\"\n", w);
	return FALSE;
}

LOCAL CONST char cu_frag[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>合計　１，２３４．５円</p></document></tad>";

LOCAL void test_window( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_TRREC	r;
	T_PSTS	psts;
	ID	pid;
	INT	wid;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_CALCU, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of 電卓");
	}
	pid = knl_prc_of_uuid(&pu);
	wid = win_named("電卓", 20000);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) goto stop;
	(void)wm_focus(wid);
	tk_dly_tsk(500);

	/* 12 + 30 = by the main rows' digits, the keypad's + and Enter */
	key_to(wid, 0x1E, 0);
	key_to(wid, 0x1F, 0);
	key_to(wid, 0x57, 0);
	key_to(wid, 0x20, 0);
	key_to(wid, 0x27, 0);
	key_to(wid, 0x28, 0);
	key_to(wid, 0x06, HID_MOD_LCTRL);		/* Ctrl+C */
	KT_ASSERT(tray_holds("<p>42</p>", 3000));

	/* 0.1 + 0.2 on the keypad alone */
	key_to(wid, 0x29, 0);				/* Esc: 全消去 */
	key_to(wid, 0x62, 0);
	key_to(wid, 0x63, 0);
	key_to(wid, 0x59, 0);
	key_to(wid, 0x57, 0);
	key_to(wid, 0x63, 0);
	key_to(wid, 0x5A, 0);
	key_to(wid, 0x58, 0);
	(void)kt_shot("/boot/CALCU0.PPM");
	key_to(wid, 0x06, HID_MOD_LCTRL);
	KT_ASSERT(tray_holds("<p>0.3</p>", 3000));

	/* a number written in full width, from the tray, doubled */
	knl_memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = cu_frag;
	r.size = sizeof(cu_frag) - 1;
	KT_ASSERT_EQ(tr_psh_dat(&r, 1, (CONST UB *)"合計"), 1);
	key_to(wid, 0x19, HID_MOD_LCTRL);		/* Ctrl+V */
	key_to(wid, 0x55, 0);
	key_to(wid, 0x1F, 0);
	key_to(wid, 0x58, 0);
	(void)kt_shot("/boot/CALCU1.PPM");
	key_to(wid, 0x06, HID_MOD_LCTRL);
	KT_ASSERT(tray_holds("<p>2,469</p>", 3000));

	/* the last digit taken back, then Delete takes the number */
	key_to(wid, 0x22, 0);
	key_to(wid, 0x23, 0);
	key_to(wid, 0x2A, 0);				/* Backspace: 5 */
	key_to(wid, 0x57, 0);
	key_to(wid, 0x24, 0);
	key_to(wid, 0x4C, 0);				/* Delete: the 7 taken back */
	key_to(wid, 0x21, 0);
	key_to(wid, 0x28, 0);
	key_to(wid, 0x06, HID_MOD_LCTRL);
	KT_ASSERT(tray_holds("<p>9</p>", 3000));

	key_to(wid, 0x08, HID_MOD_LCTRL);		/* Ctrl+E: closed */
	psts.exitcd = -1;
	er = ts_wai_prc(pid, &psts, 10000);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(win_named("電卓", 300), 0);
stop:
	if ( pid > 0 && er < E_OK ) {
		(void)ts_ter_prc(pid, TS_ABORT_TERM);
		(void)ts_wai_prc(pid, &psts, 5000);
	}
}

/* The program started with "selftest": every case, with its functions */
LOCAL void test_program( void )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	T_PSTS	psts;
	ID	pid;
	SYSTIM	t0, t1;
	ER	er;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_CALCU, &prog), E_OK);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = "selftest";
	c.argsz = 8;
	(void)tk_get_otm(&t0);
	er = ob_cre_obj(&c, &pu);
	if ( er < E_OK ) {
		tm_printf((UB *)"  the program cannot be started (%d)\n", (INT)er);
		KT_SKIP("no program object of 電卓");
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

EXPORT void ktest_calcu( void )
{
	tm_printf((UB *)"\n[calcu] 電卓 (design 11.17)\n");
	KT_RUN(test_decimal);
	KT_RUN(test_cases);
	KT_RUN(test_no_fn);
	KT_RUN(test_hid);
	KT_RUN(test_paste);
	KT_RUN_SCREEN(test_window);
	KT_RUN(test_program);
}
