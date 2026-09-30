/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_ms.c
 *	マイクロスクリプト (design 17.15): a figure and a script made here,
 *	the program started on the figure, and what the script writes to
 *	the console with LOG looked for
 *
 *	The script computes (arrays, a loop, floating point, integer
 *	division, a string, a function), shows a segment, and waits; the
 *	test sees the segment drawn in the window and presses it, and the
 *	procedure the press starts tells it so before the script finishes.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <tm/tmonitor.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include <ts/om.h>
#include <ts/img.h>
#include <ts/dtreq.h>
#include "../../application/desktop/desktop.h"

#define MS_STORE	"/boot/MSTEST"
#define LOG_MAX		( 64 * 1024 )

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k, m = s_len(w);

	for ( i = 0; i + m <= n; i++ ) {
		for ( k = 0; k < m && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == m ) return TRUE;
	}
	return FALSE;
}

/* The console's record from pos on, the bytes in buf */
LOCAL INT log_from( UD *pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

/* An object of the store with a name and one xmlTAD record */
LOCAL ER make_object( CONST char *json, CONST char *xml, TS_UUID *u )
{
	T_OBCRE	c;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(u, OB_OP_R | OB_OP_W);
	if ( key <= 0 ) return (ER)key;
	er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, xml, s_len(xml), &asz);
	ob_cls_obj(key);
	return er;
}

/* An object whose one record is a 4 by 3 PNG, red */
LOCAL ER make_picture( CONST char *json, TS_UUID *u )
{
	T_OBCRE	c;
	UW	px[12];
	UB	*png = NULL;
	SZ	len = 0, asz = 0;
	INT	recno = -1, i;
	ID	key;
	ER	er;

	for ( i = 0; i < 12; i++ ) px[i] = 0x00FF0000;
	er = img_png_encode(px, 4, 3, &png, &len);
	if ( er < E_OK ) return er;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	er = ob_cre_obj(&c, u);
	if ( er >= E_OK ) {
		key = ob_opn_obj(u, OB_OP_R | OB_OP_W);
		er = ( key > 0 ) ? ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno) : (ER)key;
		if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, png, len, &asz);
		if ( key > 0 ) ob_cls_obj(key);
	}
	Kfree(png);
	return er;
}

LOCAL CONST char script_xml[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
	"<p>VERSION 3</p>"
	"<p>VARIABLE A:I[10], S:C[32], F:F, pressed:I</p>"
	"<p>VARIABLE T:C[20], SJ:B[20], U:C[20], n:I, m:I, g:I, NM:C[400]</p>"
	"<p>SEGMENT P</p>"
	"<p>PROLOGUE</p>"
	"<p>  REPEAT 10</p>"
	"<p>    SET A[$CNT]=$CNT*$CNT</p>"
	"<p>  ENDREPEAT</p>"
	"<p>  LOG \"sq %d %d\", A[5], A[9]</p>"
	"<p>  SET F = sqrt(2.0)</p>"
	"<p>  LOG \"f %.4f %g\", F, 10/4</p>"
	"<p>  LOG \"div %d %d\", 7/2, -7%3</p>"
	"<p>  SET S[:] = \"abc\"</p>"
	"<p>  LOG \"s %s %d\", S[:], slen(S)</p>"
	"<p>  LOG \"fn %d\", add(3, 4)</p>"
	"<p>  SCENE 花</p>"
	"<p>  LOG \"scene %d %d %d\", 花.S, 花.X, 花.Y</p>"
	"<p>  MOVE 花:10,5</p>"
	"<p>  LOG \"move %d %d\", 花.X, 花.Y</p>"
	"<p>  MOVE 花</p>"
	"<p>  LOG \"home %d %d\", 花.X, 花.Y</p>"
	"<p>  MOVE 花:-100,-100 @</p>"
	"<p>  MOVE 花:0,0 @</p>"
	"<p>  TEXT 花 \"%d\", 42</p>"
	"<p>  LOG \"text %s %d %d\", 花.TX, 花.V, 花.TL</p>"
	"<p>  LOG \"vobj %d %d\", メモ.PID, メモ.W > 0</p>"
	"<p>  VWAIT メモ : 0</p>"
	"<p>  LOG \"vwait %d\", $ERR</p>"
	"<p>  SET T[:]=\"あい123\"</p>"
	"<p>  SET n=srconv(SJ,0,T,0,5,'s')</p>"
	"<p>  SET m=sconv(U,0,SJ,0,n,'s')</p>"
	"<p>  LOG \"conv %d %d %x %x %s\", n, m, SJ[0], SJ[1], U[:]</p>"
	"<p>  SET g=setgnm(\"mstest\", 77)</p>"
	"<p>  SET $GV[3]=12</p>"
	"<p>  LOG \"gnm %d %d %d\", getgnm(\"mstest\"), $GV[3], delgnm(\"mstest\")</p>"
	"<p>  LOG \"fl %d\", filelist(\"/\", NM) > 0</p>"
	"<p>  SETSEG P=newimgseg(\"画像\", 300, 0)</p>"
	"<p>  APPEAR P</p>"
	"<p>  LOG \"img %d %d %d\", P.W, P.H, P.X</p>"
	"<p>  MOVE 花:5,0:DUP</p>"
	"<p>  LOG \"dup %d\", 花.X</p>"
	"<p>  MOVE 花:0,0 @</p>"
	"<p>  KINPUT 花</p>"
	"<p>  LOG \"kinput\"</p>"
	"<p>  WAIT pressed == 1 : 8</p>"
	"<p>  LOG \"end %d %d\", pressed, $ERR</p>"
	"<p>  LOG \"kin %s\", 花.TX</p>"
	"<p>  RSINIT 0, 0, 9600</p>"
	"<p>  LOG \"rs %d\", $ERR</p>"
	"<p>  RSPUT 0, \"ab\"</p>"
	"<p>  LOG \"rsput %d\", $ERR</p>"
	"<p>  RSGETN 0</p>"
	"<p>  LOG \"rsget %d %d\", $ERR, $RSCNT</p>"
	"<p>  FINISH</p>"
	"<p>END</p>"
	"<p>FUNC add(a:I, b:I)</p>"
	"<p>  EXIT a+b</p>"
	"<p>END</p>"
	"<p>ACTION 押した PRESS 花</p>"
	"<p>  SET pressed = 1</p>"
	"<p>  LOG \"press %d\", $ARG[1]</p>"
	"<p>END</p>"
	"</document></tad>";

LOCAL void test_run( void )
{
	TS_UUID	prog, scr, fig, pu, win, list[64], vob, memo, pic, vm, vp;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	T_WMEV	ev;
	UB	*log, figxml[2400];
	char	u[40], v[40], um[40], vmm[40], up[40], vpp[40];
	UINT	mode0;
	UD	pos = 0;
	INT	n, i, cnt = 0, wid = -1, gid;
	UW	px = 0;
	ID	pid, kw;
	BOOL	scene = FALSE;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_MICROSCRIPT, &prog), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK ) KT_SKIP("no microscript program");
	log = (UB *)Kmalloc(LOG_MAX);
	if ( log == NULL ) KT_SKIP("no memory");

	/* the script, and the figure with one segment and the script linked in */
	if ( make_object("{\"name\":\"SCRIPT:テスト\"}", script_xml, &scr) < E_OK ) {
		(void)ob_att_vol(MS_STORE, 0);
		KT_ASSERT_ER(make_object("{\"name\":\"SCRIPT:テスト\"}", script_xml, &scr), E_OK);
	}
	KT_ASSERT_ER(make_object("{\"name\":\"メモ\"}",
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>memo</p></document></tad>", &memo), E_OK);
	KT_ASSERT_ER(make_picture("{\"name\":\"画像\"}", &pic), E_OK);
	(void)ts_gen_uuid(&vob);
	(void)ts_gen_uuid(&vm);
	(void)ts_gen_uuid(&vp);
	ts_uuid_to_str(&scr, u, sizeof(u));
	ts_uuid_to_str(&vob, v, sizeof(v));
	ts_uuid_to_str(&memo, um, sizeof(um));
	ts_uuid_to_str(&vm, vmm, sizeof(vmm));
	ts_uuid_to_str(&pic, up, sizeof(up));
	ts_uuid_to_str(&vp, vpp, sizeof(vpp));
	n = 0;
	figxml[0] = 0;
	{
		CONST char *parts[] = {
			"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
			"<group left=\"100\" top=\"100\" right=\"180\" bottom=\"210\">"
			"<rect left=\"100\" top=\"100\" right=\"180\" bottom=\"180\" fillColor=\"#ff8080\" strokeColor=\"#000000\" lineWidth=\"1\"/>"
			"<rect left=\"100\" top=\"180\" right=\"180\" bottom=\"210\" fontSize=\"14\" textColor=\"#000000\" fillColor=\"transparent\" strokeColor=\"transparent\">@花</rect>"
			"</group><link id=\"", (CONST char *)u, "_0.xtad\" vobjid=\"", (CONST char *)v,
			"\" vobjleft=\"400\" vobjtop=\"50\" vobjright=\"600\" vobjbottom=\"81\"/>"
			"<link id=\"", (CONST char *)um, "_0.xtad\" vobjid=\"", (CONST char *)vmm,
			"\" vobjleft=\"400\" vobjtop=\"100\" vobjright=\"520\" vobjbottom=\"131\"/>"
			"<link id=\"", (CONST char *)up, "_0.xtad\" vobjid=\"", (CONST char *)vpp,
			"\" vobjleft=\"400\" vobjtop=\"150\" vobjright=\"520\" vobjbottom=\"181\"/></figure></tad>",
			NULL
		};

		for ( i = 0; parts[i] != NULL; i++ ) {
			INT	k = s_len(parts[i]);

			knl_memcpy(figxml + n, parts[i], k);
			n += k;
		}
		figxml[n] = 0;
	}
	KT_ASSERT_ER(make_object("{\"name\":\"MSテスト\"}", (CONST char *)figxml, &fig), E_OK);

	/* the console's record up to now: what follows is the run */
	(void)log_from(&pos, log, LOG_MAX);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &fig;
	c.argsz = sizeof(fig);
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) { Kfree(log); return; }

	/* the segment shown: its window has it at the top left */
	for ( i = 0; i < 60 && !scene; i++ ) {
		UD	p0 = pos;

		tk_dly_tsk(100);
		n = log_from(&p0, log, LOG_MAX);
		scene = has(log, n, "[MS-LOG] scene");
	}
	KT_ASSERT(scene);
	tk_dly_tsk(200);
	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW
			  && has(r.name, s_len((CONST char *)r.name), "MSテスト") ) {
				win = list[i];
				wid = 0;
			}
		}
	}
	KT_ASSERT(wid == 0);
	kw = ( wid == 0 ) ? ob_opn_obj(&win, OB_OP_R) : 0;
	if ( kw > 0 ) {
		UB	j[256];
		SZ	asz = 0;
		D	d = 0;
		INT	p;

		if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK ) {
			p = knl_oj_path(j, (INT)asz, "tessronos", "window");
			p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
			if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &d) ) wid = (INT)d;
		}
		ob_cls_obj(kw);
	}
	KT_ASSERT(wid > 0);
	gid = ( wid > 0 ) ? wm_gid(wid) : -1;
	if ( gid >= 0 ) {
		KT_ASSERT_ER(dp_get_argb(gid, 40, 40, &px, 1, 1, 1), E_OK);
		KT_ASSERT_EQ(px & 0xFFFFFF, 0xFF8080);		/* the segment's fill */
		KT_ASSERT_ER(dp_get_argb(gid, 120, 40, &px, 1, 1, 1), E_OK);
		KT_ASSERT_EQ(px & 0xFFFFFF, 0xFFFFFF);		/* the ground beside it */
	}

	/* typed into KINPUT in 日本語 by romaji: か, committed by Enter */
	mode0 = wm_msg_get_mode();
	(void)wm_msg_mode(WM_MODE_ROMAN);
	for ( i = 0; i < 50 && wid > 0; i++ ) {
		UD	p0 = pos;

		n = log_from(&p0, log, LOG_MAX);
		if ( has(log, n, "[MS-LOG] kinput") ) break;
		tk_dly_tsk(100);
	}
	if ( wid > 0 ) {
		CONST UINT keys[] = { 0x0E, 0x04, 0x28 };	/* k, a, Enter */

		for ( i = 0; i < 3; i++ ) {
			knl_memset(&ev, 0, sizeof(ev));
			ev.type = HID_EV_KEY_DOWN;
			ev.wid = wid;
			ev.code = keys[i];
			knl_wmobj_event(&ev);
			tk_dly_tsk(100);
		}
		tk_dly_tsk(200);
	}
	(void)wm_msg_mode(mode0);

	/* pressed: the procedure of the press runs, and the script finishes */
	if ( wid > 0 ) {
		knl_memset(&ev, 0, sizeof(ev));
		ev.type = HID_EV_BTN_DOWN;
		ev.wid = wid;
		ev.x = 20;
		ev.y = 20;
		knl_wmobj_event(&ev);
		ev.type = HID_EV_BTN_UP;
		knl_wmobj_event(&ev);
	}
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);

	n = log_from(&pos, log, LOG_MAX);
	KT_ASSERT(has(log, n, "[MS-LOG] sq 25 81"));
	KT_ASSERT(has(log, n, "[MS-LOG] f 1.4142 2"));
	KT_ASSERT(has(log, n, "[MS-LOG] div 3 -1"));
	KT_ASSERT(has(log, n, "[MS-LOG] s abc 3"));
	KT_ASSERT(has(log, n, "[MS-LOG] fn 7"));
	KT_ASSERT(has(log, n, "[MS-LOG] scene 1 0 0"));
	KT_ASSERT(has(log, n, "[MS-LOG] move 10 5"));
	KT_ASSERT(has(log, n, "[MS-LOG] home 0 0"));		/* where it was made: the scene's corner */
	KT_ASSERT(has(log, n, "[MS-LOG] vobj -1 1"));		/* a virtual object is a segment; nothing started */
	KT_ASSERT(has(log, n, "[MS-LOG] vwait 0"));
	KT_ASSERT(has(log, n, "[MS-LOG] conv 7 5 82 a0 あい123"));
	KT_ASSERT(has(log, n, "[MS-LOG] gnm 77 12 0"));
	KT_ASSERT(has(log, n, "[MS-LOG] fl 1"));
	KT_ASSERT(has(log, n, "[MS-LOG] img 4 3 300"));
	KT_ASSERT(has(log, n, "[MS-LOG] dup 5"));
	KT_ASSERT(has(log, n, "[MS-LOG] kin か42"));
	KT_ASSERT(has(log, n, "[MS-LOG] text 42 42 2"));
	KT_ASSERT(has(log, n, "[MS-LOG] press 0"));
	KT_ASSERT(has(log, n, "[MS-LOG] end 1 0"));
	/*
	 * serb, the port that is not the console's, is RS port 0; nothing
	 * comes in on it. A machine with one serial port (a Raspberry Pi 5
	 * without the header UART, make RP1_UART=1) has no port 0 to open.
	 */
	{
		T_RDEV	rd;

		if ( tk_ref_dev((UB *)"serb", &rd) > 0 ) {
			KT_ASSERT(has(log, n, "[MS-LOG] rs 0"));
			KT_ASSERT(has(log, n, "[MS-LOG] rsput 0"));
			KT_ASSERT(has(log, n, "[MS-LOG] rsget 0 0"));
		} else {
			KT_ASSERT(!has(log, n, "[MS-LOG] rs 0"));
			tm_printf((UB *)"  no second serial port: the RS calls answer an error\n");
		}
	}
	if ( !has(log, n, "[MS-LOG] rsget 0 0") && !has(log, n, "[MS-LOG] rs ") ) {
		log[( n < LOG_MAX ) ? n : LOG_MAX - 1] = 0;
		tm_putstring(log);
	}
	KT_ASSERT_ER(ob_ref_obj(&win, &r), E_NOEXS);	/* its window went with it */
	(void)ob_del_obj(&fig);
	(void)ob_del_obj(&scr);
	Kfree(log);
	wm_update();
}

/* The window of a name, and its number; -1 when there is none */
LOCAL INT window_of( CONST char *name, TS_UUID *out )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	cnt = 0, i, wid = -1;
	ID	kw;

	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt; i++ ) {
		if ( ob_ref_obj(&list[i], &r) >= E_OK && r.sub == OB_S_WINDOW
		  && has(r.name, s_len((CONST char *)r.name), name) ) {
			*out = list[i];
			kw = ob_opn_obj(out, OB_OP_R);
			if ( kw > 0 ) {
				UB	j[256];
				SZ	asz = 0;
				D	d = 0;
				INT	p;

				if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK ) {
					p = knl_oj_path(j, (INT)asz, "tessronos", "window");
					p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
					if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &d) ) wid = (INT)d;
				}
				ob_cls_obj(kw);
			}
		}
	}
	return wid;
}

/*
 * The template as the program box gives it: 花, 鳥 and a button, and the
 * script that shows them and shakes 花 when the button is clicked. The
 * screen is kept before and after the click (MS1.PPM, MS2.PPM).
 */
LOCAL void test_sample( void )
{
	TS_UUID	prog, base, win;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	T_WMEV	ev;
	UW	px = 0;
	INT	wid = -1, gid, i;
	ID	pid;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_MICROSCRIPT, &prog), E_OK);
	KT_ASSERT_ER(ts_str_to_uuid("a22ca516-a22c-7fdb-81fd-04cda3e1c8af", &base), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK || ob_ref_obj(&base, &r) < E_OK ) KT_SKIP("no template");

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &base;
	c.argsz = sizeof(base);
	KT_ASSERT_ER(ob_cre_obj(&c, &win), E_OK);
	pid = knl_prc_of_uuid(&win);
	KT_ASSERT(pid > 0);
	for ( i = 0; i < 50 && wid < 0; i++ ) {
		tk_dly_tsk(100);
		wid = window_of("マイクロスクリプト", &win);
	}
	KT_ASSERT(wid > 0);
	if ( wid <= 0 || pid <= 0 ) return;
	tk_dly_tsk(800);
	gid = wm_gid(wid);
	/* SCENE 花,鳥,ボタン押して: the three to the top left by their groups' corners */
	KT_ASSERT_ER(dp_get_argb(gid, 60, 20, &px, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(px & 0xFFFFFF, 0xFF8080);		/* 花 */
	KT_ASSERT_ER(dp_get_argb(gid, 250, 20, &px, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(px & 0xFFFFFF, 0x80A0FF);		/* 鳥 */
	KT_ASSERT_ER(dp_get_argb(gid, 80, 220, &px, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(px & 0xFFFFFF, 0xC0C0C0);		/* the button */
	wm_update();
	(void)kt_shot("/boot/MS1.PPM");

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_BTN_DOWN;
	ev.wid = wid;
	ev.x = 80;
	ev.y = 220;
	knl_wmobj_event(&ev);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(150);				/* in the middle of the shake */
	wm_update();
	(void)kt_shot("/boot/MS2.PPM");
	tk_dly_tsk(600);
	/* shaken and back: +10, -20, +10 */
	KT_ASSERT_ER(dp_get_argb(gid, 60, 20, &px, 1, 1, 1), E_OK);
	KT_ASSERT_EQ(px & 0xFFFFFF, 0xFF8080);
	wm_update();
	(void)kt_shot("/boot/MS3.PPM");

	KT_ASSERT_ER(ts_ter_prc(pid, 0), E_OK);
	(void)ts_wai_prc(pid, &psts, 3000);
	wm_update();
}

/*
 * What the desktop knows of it: an application of the program box, not
 * carried in the desktop, whose template is in the 原紙箱; and the
 * template's execute menu names it, second after 基本図形編集.
 */
LOCAL void test_desktop( void )
{
	CONST DTPROG	*pp, *list[32];
	TS_UUID		base;
	UB		names[8][OM_APP_NAME], id[64];
	INT		n, i;
	BOOL		found = FALSE;

	KT_ASSERT_ER(ts_str_to_uuid("a22ca516-a22c-7fdb-81fd-04cda3e1c8af", &base), E_OK);
	(void)dt_prog_start();
	pp = dt_prog_find((CONST UB *)"microscript");
	if ( pp == NULL ) KT_SKIP("no program box in the store");
	KT_ASSERT_EQ(pp->kind, DT_PK_APP);
	KT_ASSERT(!pp->builtin);
	KT_ASSERT_EQ(pp->nbase, 1);
	KT_ASSERT_EQ(ts_uuid_cmp(&pp->base[0], &base), 0);
	n = dt_prog_list(DT_PK_APP, list, 32);
	for ( i = 0; i < n; i++ ) if ( list[i] == pp ) found = TRUE;
	KT_ASSERT(found);				/* its template goes in the 原紙箱 */

	n = om_store_apps(&base, names, 8);
	KT_ASSERT(n >= 2);
	KT_ASSERT(has(names[1], s_len((CONST char *)names[1]), "マイクロスクリプト"));
	KT_ASSERT(om_store_app_id(&base, 1, id, sizeof(id)) > 0);
	KT_ASSERT(has(id, s_len((CONST char *)id), "microscript"));
	KT_ASSERT(om_store_default_app(&base, id, sizeof(id)) > 0);
	KT_ASSERT(has(id, s_len((CONST char *)id), "basic-figure-editor"));
}

LOCAL CONST UB jpg_420[708] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
	0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
	0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
	0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
	0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
	0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
	0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
	0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
	0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
	0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
	0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
	0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
	0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
	0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
	0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
	0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xf8,
	0xbe, 0xbf, 0x4a, 0x2b, 0xf0, 0x6e, 0xbf, 0xb8, 0x8a, 0x3e, 0x93, 0x1f, 0x41, 0x9f, 0xf8, 0x87,
	0x1f, 0xd9, 0x5f, 0xf1, 0x90, 0x7b, 0x7f, 0x6f, 0xed, 0xff, 0x00, 0xe6, 0x13, 0x93, 0x97, 0x93,
	0xd8, 0xff, 0x00, 0xd4, 0x4c, 0xef, 0x7e, 0x7f, 0x2b, 0x5b, 0xad, 0xf4, 0x5f, 0x4c, 0x1e, 0x25,
	0xff, 0x00, 0x89, 0xd1, 0xfe, 0xc4, 0xfd, 0xd7, 0xf6, 0x57, 0xf6, 0x57, 0xd6, 0x7e, 0xd7, 0xd6,
	0xbd, 0xaf, 0xd6, 0xbe, 0xaf, 0xe5, 0x87, 0xe4, 0xe4, 0xfa, 0xbf, 0xf7, 0xf9, 0xb9, 0xfe, 0xcf,
	0x2f, 0xbd, 0xff, 0xd9,
};
LOCAL CONST UB jpg_grey[340] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x10,
	0x00, 0x10, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
	0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03,
	0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00,
	0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32,
	0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
	0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35,
	0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
	0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
	0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2,
	0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
	0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
	0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda,
	0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0xf8, 0xbe, 0xbf, 0x4a, 0x2b, 0xf0, 0x6e, 0xbf,
	0xb8, 0x8a, 0xff, 0xd9,
};
LOCAL CONST UB jpg_prog[591] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc2,
	0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
	0x01, 0xff, 0xc4, 0x00, 0x16, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x09, 0xff, 0xc4, 0x00, 0x15, 0x01, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0x8b, 0xa5,
	0x01, 0xbb, 0x88, 0x9f, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01,
	0x00, 0x01, 0x05, 0x02, 0x1f, 0xff, 0xc4, 0x00, 0x1f, 0x11, 0x00, 0x02, 0x01, 0x02, 0x07, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x13, 0x14, 0x05, 0x41, 0x07,
	0x12, 0x17, 0x21, 0x22, 0x61, 0x73, 0xff, 0xda, 0x00, 0x08, 0x01, 0x03, 0x01, 0x01, 0x3f, 0x01,
	0xc6, 0x0a, 0x96, 0xb4, 0x42, 0xe3, 0x16, 0x2b, 0x2e, 0xdc, 0xcd, 0x5f, 0x98, 0x0b, 0xec, 0x9b,
	0x0d, 0xff, 0x00, 0xff, 0xc4, 0x00, 0x1e, 0x11, 0x00, 0x01, 0x02, 0x07, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x07, 0x11, 0x00, 0x01, 0x14, 0x16, 0x25,
	0x43, 0x51, 0x63, 0xff, 0xda, 0x00, 0x08, 0x01, 0x02, 0x01, 0x01, 0x3f, 0x01, 0x53, 0x10, 0xcb,
	0x72, 0x97, 0x20, 0x66, 0x7a, 0x99, 0x98, 0x3d, 0x27, 0xd8, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
	0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x06, 0x3f, 0x02, 0x1f, 0xff, 0xc4, 0x00, 0x14, 0x10,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x20, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3f, 0x21, 0x1f, 0xff, 0xda, 0x00, 0x0c,
	0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x07, 0xff, 0xc4, 0x00, 0x16, 0x11,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x51, 0xf0, 0xff, 0xda, 0x00, 0x08, 0x01, 0x03, 0x01, 0x01, 0x3f, 0x10, 0xbf, 0xc8, 0x86,
	0xa3, 0xff, 0xc4, 0x00, 0x19, 0x11, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x21, 0x41, 0x61, 0x71, 0xf0, 0xff, 0xda, 0x00, 0x08,
	0x01, 0x02, 0x01, 0x01, 0x3f, 0x10, 0xe3, 0x7d, 0x67, 0x75, 0x0b, 0x63, 0xff, 0xc4, 0x00, 0x14,
	0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x20, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3f, 0x10, 0x1f, 0xff, 0xd9,
};

LOCAL BOOL near_rgb( UW px, UW want )
{
	INT	i;

	for ( i = 0; i < 24; i += 8 ) {
		INT	a = (INT)( ( px >> i ) & 0xFF ), b = (INT)( ( want >> i ) & 0xFF );

		if ( a - b > 12 || b - a > 12 ) return FALSE;
	}
	return TRUE;
}

/*
 * The pictures a program may decode: a JPEG of four colours sampled
 * 4:2:0, a grey one, a progressive one refused, and BMPs of 24 bits and
 * of a palette.
 */
LOCAL void test_img( void )
{
	UW	*px = NULL;
	INT	w = 0, h = 0;
	UB	bmp[70], pal[70];
	INT	i;

	KT_ASSERT_ER(img_size(jpg_420, sizeof(jpg_420), &w, &h), E_OK);
	KT_ASSERT(w == 16 && h == 16);
	KT_ASSERT_ER(img_decode(jpg_420, sizeof(jpg_420), &px, &w, &h), E_OK);
	if ( px != NULL ) {
		KT_ASSERT(near_rgb(px[2 * 16 + 2], 0xFF0000));
		KT_ASSERT(near_rgb(px[2 * 16 + 13], 0x00FF00));
		KT_ASSERT(near_rgb(px[13 * 16 + 2], 0x0000FF));
		KT_ASSERT(near_rgb(px[13 * 16 + 13], 0xFFFFFF));
		Kfree(px);
		px = NULL;
	}
	KT_ASSERT_ER(img_decode(jpg_grey, sizeof(jpg_grey), &px, &w, &h), E_OK);
	if ( px != NULL ) {
		KT_ASSERT(near_rgb(px[2 * 16 + 2], 0x4C4C4C));
		Kfree(px);
		px = NULL;
	}
	KT_ASSERT_ER(img_decode(jpg_prog, sizeof(jpg_prog), &px, &w, &h), E_NOSPT);

	/* 2 by 2, 24 bits, bottom row first: red green / blue white */
	knl_memset(bmp, 0, sizeof(bmp));
	bmp[0] = 'B'; bmp[1] = 'M';
	bmp[10] = 54;
	bmp[14] = 40;
	bmp[18] = 2;
	bmp[22] = 2;
	bmp[26] = 1;
	bmp[28] = 24;
	{
		CONST UB rows[16] = {
			0xFF, 0x00, 0x00,  0xFF, 0xFF, 0xFF,  0, 0,	/* blue, white */
			0x00, 0x00, 0xFF,  0x00, 0xFF, 0x00,  0, 0,	/* red, green */
		};

		for ( i = 0; i < 16; i++ ) bmp[54 + i] = rows[i];
	}
	KT_ASSERT_ER(img_decode(bmp, 70, &px, &w, &h), E_OK);
	if ( px != NULL ) {
		KT_ASSERT(w == 2 && h == 2);
		KT_ASSERT_EQ(px[0], 0xFF0000);
		KT_ASSERT_EQ(px[1], 0x00FF00);
		KT_ASSERT_EQ(px[2], 0x0000FF);
		KT_ASSERT_EQ(px[3], 0xFFFFFF);
		Kfree(px);
		px = NULL;
	}

	/* an icon: one 2 by 2 picture of 32 bits, bottom row first, one pixel clear */
	{
		UB	ico[6 + 16 + 40 + 16 + 8];
		CONST UB px32[16] = {
			0xFF, 0x00, 0x00, 0xFF,  0x00, 0x00, 0x00, 0x00,	/* blue, clear */
			0x00, 0x00, 0xFF, 0xFF,  0x00, 0xFF, 0x00, 0xFF,	/* red, green */
		};

		knl_memset(ico, 0, sizeof(ico));
		ico[2] = 1;  ico[4] = 1;				/* an icon, one picture */
		ico[6] = 2;  ico[7] = 2;  ico[10] = 1;  ico[12] = 32;
		ico[14] = 40 + 16 + 8;				/* its length */
		ico[18] = 22;						/* where it is */
		ico[22] = 40;  ico[26] = 2;  ico[30] = 4;		/* 2 wide, 4 with the mask */
		ico[34] = 1;  ico[36] = 32;
		for ( i = 0; i < 16; i++ ) ico[62 + i] = px32[i];
		KT_ASSERT_ER(img_size(ico, sizeof(ico), &w, &h), E_OK);
		KT_ASSERT(w == 2 && h == 2);
		KT_ASSERT_ER(img_decode(ico, sizeof(ico), &px, &w, &h), E_OK);
		if ( px != NULL ) {
			KT_ASSERT_EQ(px[0], 0xFF0000);
			KT_ASSERT_EQ(px[1], 0x00FF00);
			KT_ASSERT_EQ(px[2], 0x0000FF);
			KT_ASSERT_EQ(px[3], IMG_CLEAR);
			Kfree(px);
			px = NULL;
		}
	}

	/* 3 by 1, a palette of 1 bit a pixel, top row first */
	knl_memset(pal, 0, sizeof(pal));
	pal[0] = 'B'; pal[1] = 'M';
	pal[10] = 62;
	pal[14] = 40;
	pal[18] = 3;
	pal[22] = 0xFF; pal[23] = 0xFF; pal[24] = 0xFF; pal[25] = 0xFF;	/* -1: top down */
	pal[26] = 1;
	pal[28] = 1;
	pal[46] = 2;
	pal[54] = 0x00; pal[55] = 0x00; pal[56] = 0x00;			/* 0: black */
	pal[58] = 0x00; pal[59] = 0x80; pal[60] = 0xFF;			/* 1: orange */
	pal[62] = 0xA0;							/* 1 0 1 */
	KT_ASSERT_ER(img_decode(pal, 66, &px, &w, &h), E_OK);
	if ( px != NULL ) {
		KT_ASSERT(w == 3 && h == 1);
		KT_ASSERT_EQ(px[0], 0xFF8000);
		KT_ASSERT_EQ(px[1], 0x000000);
		KT_ASSERT_EQ(px[2], 0xFF8000);
		Kfree(px);
	}
}

/*
 * VOPEN as the desktop sees it: the test keeps the channel the desktop
 * would, takes the request, and answers with a process; the segment's
 * PID becomes that process's number.
 */
LOCAL CONST char vopen_xml[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
	"<p>PROLOGUE</p>"
	"<p>  VOPEN メモ2</p>"
	"<p>  LOG \"vo %d %d\", $ERR, メモ2.PID</p>"
	"<p>  FINISH</p>"
	"<p>END</p>"
	"</document></tad>";

LOCAL void test_vopen( void )
{
	TS_UUID	prog, scr, memo, fig, pu, ch, v1, v2;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	T_DTREQ	rq;
	T_DTANS	an;
	UB	*log, figxml[1200], want[32];
	char	u1[40], s1[40], u2[40], s2[40];
	UD	pos = 0;
	SZ	asz = 0;
	INT	i, n = 0, k;
	ID	pid, kc, kr;
	BOOL	got = FALSE;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_MICROSCRIPT, &prog), E_OK);
	if ( ob_ref_obj(&prog, &r) < E_OK ) KT_SKIP("no microscript program");
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &ch) >= E_OK ) KT_SKIP("a desktop is running");
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.name = (CONST UB *)DT_REQ_NAME;
	c.flags = OB_F_GLOBAL;
	KT_ASSERT_ER(ob_cre_obj(&c, &ch), E_OK);
	kc = ob_opn_obj(&ch, OB_OP_READ | OB_O_NOWAIT);
	KT_ASSERT(kc > 0);
	log = (UB *)Kmalloc(LOG_MAX);
	if ( kc <= 0 || log == NULL ) KT_SKIP("no channel");

	KT_ASSERT_ER(make_object("{\"name\":\"SCRIPT:開く\"}", vopen_xml, &scr), E_OK);
	KT_ASSERT_ER(make_object("{\"name\":\"メモ2\"}",
		"<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>memo</p></document></tad>", &memo), E_OK);
	(void)ts_gen_uuid(&v1);
	(void)ts_gen_uuid(&v2);
	ts_uuid_to_str(&scr, u1, sizeof(u1));
	ts_uuid_to_str(&v1, s1, sizeof(s1));
	ts_uuid_to_str(&memo, u2, sizeof(u2));
	ts_uuid_to_str(&v2, s2, sizeof(s2));
	{
		CONST char *parts[] = {
			"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
			"<link id=\"", u1, "_0.xtad\" vobjid=\"", s1,
			"\" vobjleft=\"10\" vobjtop=\"10\" vobjright=\"200\" vobjbottom=\"41\"/>"
			"<link id=\"", u2, "_0.xtad\" vobjid=\"", s2,
			"\" vobjleft=\"10\" vobjtop=\"60\" vobjright=\"200\" vobjbottom=\"91\"/></figure></tad>",
			NULL
		};

		for ( i = 0; parts[i] != NULL; i++ ) {
			k = s_len(parts[i]);
			knl_memcpy(figxml + n, parts[i], k);
			n += k;
		}
		figxml[n] = 0;
	}
	KT_ASSERT_ER(make_object("{\"name\":\"MS開く\"}", (CONST char *)figxml, &fig), E_OK);
	(void)log_from(&pos, log, LOG_MAX);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = &fig;
	c.argsz = sizeof(fig);
	KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
	pid = knl_prc_of_uuid(&pu);
	KT_ASSERT(pid > 0);

	/* the request: this object, opened as a double click opens it */
	for ( i = 0; i < 100 && !got; i++ ) {
		if ( ob_rea_rec(kc, 0, 0, &rq, sizeof(rq), &asz) >= E_OK ) {
			got = TRUE;
			break;
		}
		tk_dly_tsk(50);
	}
	KT_ASSERT(got);
	if ( got ) {
		KT_ASSERT_EQ(asz, (SZ)sizeof(rq));
		KT_ASSERT_EQ(rq.req, DT_RQ_OPEN);
		KT_ASSERT_EQ(ts_uuid_cmp(&rq.target, &memo), 0);
		/* answered: the process started for it is, here, the script's own */
		knl_memset(&an, 0, sizeof(an));
		an.seq = rq.seq;
		an.er = E_OK;
		an.proc = pu;
		kr = ob_opn_obj(&rq.reply, OB_OP_WRITE);
		KT_ASSERT(kr > 0);
		if ( kr > 0 ) {
			KT_ASSERT_ER(ob_wri_rec(kr, 0, 0, &an, sizeof(an), &asz), E_OK);
			ob_cls_obj(kr);
		}
	}
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	n = log_from(&pos, log, LOG_MAX);
	{
		INT	d = 0, p = pid;
		UB	num[12];

		/* "vo 0 <pid>" */
		do { num[d++] = (UB)( '0' + p % 10 ); p /= 10; } while ( p > 0 && d < 11 );
		k = 0;
		for ( i = 0; "[MS-LOG] vo 0 "[i] != 0; i++ ) want[k++] = (UB)"[MS-LOG] vo 0 "[i];
		while ( d > 0 ) want[k++] = num[--d];
		want[k] = 0;
	}
	KT_ASSERT(has(log, n, (CONST char *)want));
	ob_cls_obj(kc);
	(void)ob_del_obj(&ch);
	Kfree(log);
}

/*
 * The network samples of 仮身サンプル, as they are (TADjs's statements:
 * CONSOLE, SPRINTF, HTTPREQ, HTTPHDR, JSONGET). The MCP samples talk to
 * localhost:3333 (JSON) and :3334 (an event stream): here two relays
 * listen there and pass each connection on to the test server on the
 * host (tools/httpd_test.py, started with the FTP server when NET=user).
 * The first sample is not allowed its host beforehand: the window asks,
 * and Enter answers. The others are allowed in their metadata, as a
 * person's earlier answer leaves it. HTTP通信テスト reaches the internet;
 * without it, it only has to say that it failed.
 */
#ifdef KT_HTTP_PORT
#include <ts/so.h>

#define RELAY_PORT	3333
#define HOST_ADDR	0x0a000202UL		/* 10.0.2.2: the host, through QEMU */

LOCAL volatile BOOL	relay_stop;
LOCAL volatile INT	relay_done;
LOCAL UB		relay_buf[2][4096];

LOCAL void relay_addr( struct sockaddr_in *sa, UW addr, UH port )
{
	knl_memset(sa, 0, sizeof(*sa));
	sa->sin_len = sizeof(*sa);
	sa->sin_family = AF_INET;
	sa->sin_port = lwip_htons(port);
	sa->sin_addr.s_addr = lwip_htonl(addr);
}

/* One direction's bytes, from a to b; FALSE when a has closed */
LOCAL BOOL relay_pass( INT a, INT b, UB *buf )
{
	INT	n = so_recv(a, buf, 4096, 0);

	if ( n <= 0 ) return FALSE;
	(void)so_send(b, buf, (SZ)n, 0);
	return TRUE;
}

LOCAL void relay_task( INT which, void *exinf )
{
	struct sockaddr_in	me, to;
	INT			srv, c, h, one = 1;

	(void)exinf;
	srv = so_socket(AF_INET, SOCK_STREAM, 0);
	relay_addr(&me, 0x7f000001UL, (UH)( RELAY_PORT + which ));
	(void)so_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if ( srv < 0 || so_bind(srv, (struct sockaddr *)&me, sizeof(me)) < E_OK || so_listen(srv, 2) < E_OK ) {
		tm_printf((UB *)"  relay %d: no socket\n", RELAY_PORT + which);
		relay_done++;
		tk_ext_tsk();
	}
	while ( !relay_stop ) {
		fd_set	rd;

		FD_ZERO(&rd);
		FD_SET(srv, &rd);
		if ( so_select(srv + 1, &rd, NULL, NULL, 200) <= 0 ) continue;
		c = so_accept(srv, NULL, NULL);
		if ( c < 0 ) continue;
		h = so_socket(AF_INET, SOCK_STREAM, 0);
		relay_addr(&to, HOST_ADDR, (UH)( KT_HTTP_PORT + which ));
		{
			ER	cer = ( h >= 0 ) ? so_connect(h, (struct sockaddr *)&to, sizeof(to)) : h;

			if ( cer < E_OK ) {
				tm_printf((UB *)"  relay %d: no connection to the host (%d)\n", RELAY_PORT + which, cer);
				if ( h >= 0 ) (void)so_close(h);
				h = -1;
			}
		}
		if ( h >= 0 ) {
			INT	idle = 0;

			while ( !relay_stop && idle < 150 ) {
				INT	mx = ( c > h ) ? c : h;

				FD_ZERO(&rd);
				FD_SET(c, &rd);
				FD_SET(h, &rd);
				if ( so_select(mx + 1, &rd, NULL, NULL, 200) <= 0 ) { idle++; continue; }
				idle = 0;
				if ( FD_ISSET(c, &rd) && !relay_pass(c, h, relay_buf[which]) ) break;
				if ( FD_ISSET(h, &rd) && !relay_pass(h, c, relay_buf[which]) ) break;
			}
		}
		if ( h >= 0 ) (void)so_close(h);
		(void)so_close(c);
	}
	(void)so_close(srv);
	relay_done++;
	tk_ext_tsk();
}

/* "networkGrants" put in an object's metadata; the metadata as it was, for putting back */
LOCAL INT grant_hosts( CONST TS_UUID *u, UB *was, INT max )
{
	CONST char	*add = ",\"networkGrants\":[\"localhost\",\"example.com\",\"jsonplaceholder.typicode.com\"]}";
	UB	*j = (UB *)Kmalloc(OB_ATR_MAX + 256);
	SZ	asz = 0;
	INT	n, end, k;
	ID	key;

	if ( j == NULL ) return -1;
	key = ob_opn_obj(u, OB_OP_R | OB_OP_ATRWR);
	if ( key <= 0 || ob_get_atr(key, j, OB_ATR_MAX, &asz) < E_OK || (INT)asz > max ) {
		if ( key > 0 ) ob_cls_obj(key);
		Kfree(j);
		return -1;
	}
	n = (INT)asz;
	knl_memcpy(was, j, n);
	for ( end = n; end > 0 && j[end - 1] != '}'; end-- ) ;
	if ( end > 0 && !has(j, n, "networkGrants") ) {
		for ( k = 0; add[k] != 0; k++ ) j[end - 1 + k] = (UB)add[k];
		(void)ob_set_atr(key, j, end - 1 + k);
	}
	ob_cls_obj(key);
	Kfree(j);
	return n;
}

LOCAL void atr_back( CONST TS_UUID *u, CONST UB *was, INT n )
{
	ID	key = ob_opn_obj(u, OB_OP_R | OB_OP_ATRWR);

	if ( key > 0 ) {
		if ( n > 0 ) (void)ob_set_atr(key, was, n);
		ob_cls_obj(key);
	}
}

LOCAL void test_net_samples( void )
{
	static CONST struct {
		CONST char	*fig;
		CONST char	*title;
		BOOL		ask;		/* the window asks, and is answered */
		CONST char	*want[4];
	} sm[] = {
		{ "019f4140-3d5e-72ef-af0d-d6148c537a37", "MCP通信テスト", TRUE,
		  { "サーバ = demo-mcp", "道具0 名前 = echo", "応答 = ", "完了" } },
		{ "019f4153-44f6-7522-8ec0-262e1e6aec1a", "MCP認証テスト", FALSE,
		  { "トークン取得: ", "道具0 名前 = echo", "応答 = ", "完了" } },
		{ "019f44a9-ded2-7d70-9850-3a7c99ba15d2", "MCP-SSEテスト", FALSE,
		  { "Content-Type = text/event-stream", "サーバ = sse-demo-mcp", "応答 = ", "完了" } },
		{ "019f4049-ecb3-7a75-b543-f8b23da4f0d1", "HTTP通信テスト", FALSE,
		  { "[1] https://example.com/", "[2] jsonplaceholder", NULL, NULL } },
	};
	TS_UUID	prog, base, win;
	T_OBCRE	c;
	T_OBREF	r;
	T_PSTS	psts;
	T_CTSK	ct;
	UB	*log = (UB *)Kmalloc(LOG_MAX), *was = (UB *)Kmalloc(OB_ATR_MAX);
	UD	pos;
	INT	k, i, n, wasn, wid;
	ID	pid, tid[2];

	KT_ASSERT(log != NULL && was != NULL);
	if ( log == NULL || was == NULL ) return;
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_PROG_MICROSCRIPT, &prog), E_OK);

	/* an address, a gateway and a name server from QEMU's network */
	{
		UW	addr = 0;
		INT	t;

		if ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ) {
			(void)so_dhcp_start();
			for ( t = 0; t < 150 && ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ); t++ ) {
				tk_dly_tsk(100);
			}
		}
		KT_ASSERT(addr != 0);
	}
	relay_stop = FALSE;
	relay_done = 0;
	for ( i = 0; i < 2; i++ ) {
		knl_memset(&ct, 0, sizeof(ct));
		ct.tskatr = TA_HLNG;
		ct.task = (FP)relay_task;
		ct.itskpri = KT_PRI_HIGH;
		ct.stksz = 16 * 1024;
		tid[i] = tk_cre_tsk(&ct);
		KT_ASSERT(tid[i] > 0);
		if ( tid[i] > 0 ) (void)tk_sta_tsk(tid[i], i);
	}
	tk_dly_tsk(200);

	for ( k = 0; k < (INT)( sizeof(sm) / sizeof(sm[0]) ); k++ ) {
		BOOL	internet = (BOOL)( sm[k].want[2] == NULL );

		tm_printf((UB *)"  === %s\n", sm[k].title);
		if ( ts_str_to_uuid(sm[k].fig, &base) < E_OK || ob_ref_obj(&base, &r) < E_OK ) {
			KT_ASSERT(FALSE);
			continue;
		}
		wasn = sm[k].ask ? -1 : grant_hosts(&base, was, OB_ATR_MAX);
		pos = ~(UD)0 >> 1;
		(void)tm_log_read(&pos, log, 1);
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_PROCESS;
		c.prog = prog;
		c.arg = &base;
		c.argsz = sizeof(base);
		KT_ASSERT_ER(ob_cre_obj(&c, &win), E_OK);
		pid = knl_prc_of_uuid(&win);
		n = 0;
		for ( i = 0; i < 300; i++ ) {		/* up to 30 s */
			INT	m = tm_log_read(&pos, log + n, LOG_MAX - 1 - n);

			n += ( m > 0 ) ? m : 0;
			if ( has(log, n, "完了") || ( internet && has(log, n, "[2] jsonplaceholder")
						     && ( has(log, n, "id = ") || has(log, n, "失敗") ) ) ) break;
			if ( sm[k].ask && ( i % 5 ) == 4 ) {
				/* the question, answered: Enter allows */
				T_WMEV	ev;
				TS_UUID	w;

				wid = window_of(sm[k].title, &w);
				if ( wid > 0 ) {
					knl_memset(&ev, 0, sizeof(ev));
					ev.type = HID_EV_KEY_DOWN;
					ev.wid = wid;
					ev.code = 0x28;
					knl_wmobj_event(&ev);
					ev.type = HID_EV_KEY_UP;
					knl_wmobj_event(&ev);
				}
			}
			tk_dly_tsk(100);
		}
		tk_dly_tsk(300);
		i = tm_log_read(&pos, log + n, LOG_MAX - 1 - n);
		n += ( i > 0 ) ? i : 0;
		log[n] = 0;
		KT_ASSERT(!has(log, n, "構文エラー"));
		for ( i = 0; i < 4; i++ ) {
			if ( sm[k].want[i] == NULL ) continue;
			if ( !has(log, n, sm[k].want[i]) ) tm_printf((UB *)"  missing: %s\n", sm[k].want[i]);
			KT_ASSERT(has(log, n, sm[k].want[i]));
		}
		if ( internet ) {
			tm_printf((UB *)"  the internet: %s\n",
				  has(log, n, "title = delectus aut autem") ? "reached, HTTPS and JSON read"
				  : "not reached (the sample said it failed)");
			KT_ASSERT(has(log, n, "title = delectus aut autem") || has(log, n, "失敗"));
		} else {
			KT_ASSERT(!has(log, n, "失敗"));
		}
		if ( sm[k].ask ) {
			/* what was allowed is kept in the figure's metadata */
			ID	key = ob_opn_obj(&base, OB_OP_R);
			SZ	asz = 0;

			if ( key > 0 ) {
				(void)ob_get_atr(key, was, OB_ATR_MAX, &asz);
				ob_cls_obj(key);
			}
			if ( !has(was, (INT)asz, "networkGrants") ) {
				was[( asz < OB_ATR_MAX ) ? asz : OB_ATR_MAX - 1] = 0;
				tm_printf((UB *)"  metadata: %s\n", was);
			}
			KT_ASSERT(has(was, (INT)asz, "networkGrants") && has(was, (INT)asz, "localhost"));
		}
		{
			/* each sample's window and its console, NET1.PPM and on */
			char	shot[] = "/boot/NET0.PPM";

			shot[9] = (char)( '1' + k );
			wm_update();
			(void)kt_shot(shot);
		}
		if ( pid > 0 ) {
			(void)ts_ter_prc(pid, 0);
			(void)ts_wai_prc(pid, &psts, 3000);
		}
		if ( wasn > 0 ) atr_back(&base, was, wasn);
		wm_update();
	}
	relay_stop = TRUE;
	for ( i = 0; i < 30 && relay_done < 2; i++ ) tk_dly_tsk(100);
	for ( i = 0; i < 2; i++ ) {
		if ( tid[i] > 0 ) (void)tk_del_tsk(tid[i]);
	}
	Kfree(was);
	Kfree(log);
}
#else
LOCAL void test_net_samples( void )
{
	KT_SKIP("the network samples need NET=user");
}
#endif

EXPORT void ktest_ms( void )
{
	KT_RUN(test_img);
	KT_RUN(test_run);
	KT_RUN(test_vopen);
	KT_RUN(test_sample);
	KT_RUN(test_desktop);
	KT_RUN(test_net_samples);
}
