/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_drop.c
 *	Links dropped from the desktop on the windows of processes
 *	(design 16.5.21, 18.14)
 *
 *	The desktop is started as it starts on the machine, a cabinet of
 *	two texts is opened in it, and the pointer and its button are put
 *	in as the device would report them: a link is taken in the cabinet,
 *	carried over the window of an accessory running as a process and
 *	let go there. The accessory is told with OB_E_DROP, reads the drop
 *	from its window, answers it and does what the drop asked:
 *
 *	  ファイル変換	refused on its main window, where the media are
 *			listed; on the window of a disk it opened, the
 *			object written into the directory shown after its
 *			panel is answered; and the other way, two files of
 *			that window carried into a figure of the desktop
 *			become 実身 there, linked where they were let go
 *	  バックアップ	two links, picked and carried together, become the
 *			list and are marked to be saved; Enter saves both,
 *			and a restore puts them back into the cabinet
 *			beside where they were
 *
 *	What the accessories did is read from the console, where they say
 *	it; the files they wrote are looked for and taken away again.
 *
 *	Last, a process logged in as a user that is not an administrator
 *	(tests/uprog/svcprog.c, SP_DROP) is dropped an object only the
 *	system may open, and uses it as far as the drop grants.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/wm.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"
#include "../../application/desktop/desktop.h"
#include "../../application/backup/bk_arg.h"

#define LOG_MAX		( 128 * 1024 )
#define SF_BYTES	( (UD)34816 * 1024 )	/* the superfloppy's size */

#define K_ENTER		0x28
#define K_BS		0x2A

/* Where the cabinet's two links are in its figure */
#define L1_LEFT		10
#define L1_TOP		10
#define L2_LEFT		10
#define L2_TOP		50
#define L_W		160
#define L_H		25

/* Where the accessories' windows are put: clear of the cabinet's */
#define ACC_LEFT	380
#define ACC_TOP		60

#define BKP_FILE	"/boot/\xEF\xBC\x91\xE3\x83\xBBKT\xE6\x8A\x95\xE4\xB8\x8B\xE6\x96\x87\xE7\xAB\xA0\x41.TAD"	/* １・KT投下文章A.TAD */

LOCAL UB	*logb;
LOCAL TS_UUID	cab0, txa, txb, cab;
LOCAL TS_UUID	back[2];		/* what a restore put back into the cabinet */
LOCAL INT	nback;
LOCAL BOOL	made, up;
LOCAL INT	cwid = -1;		/* the cabinet's window */

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL INT find( CONST UB *s, INT n, CONST char *w, INT from )
{
	INT	i, k, wl = s_len(w);

	for ( i = from; i + wl <= n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return i;
	}
	return -1;
}

/* How many times w is in s */
LOCAL INT count_of( CONST UB *s, INT n, CONST char *w )
{
	INT	at = 0, k = 0;

	while ( ( at = find(s, n, w, at) ) >= 0 ) {
		k++;
		at++;
	}
	return k;
}

LOCAL INT log_from( UD pos, UB *buf, INT max )
{
	INT	n = 0, k;

	while ( n < max && ( k = tm_log_read(&pos, buf + n, max - n) ) > 0 ) n += k;
	return n;
}

LOCAL UD log_now( void )
{
	UD	pos = 0;

	while ( tm_log_read(&pos, logb, LOG_MAX) > 0 ) ;
	return pos;
}

/* Waited for (tries tenths of a second) until the console says w after pos; the rest of its line */
LOCAL BOOL wait_for( UD pos, CONST char *w, INT tries, char *line, INT max )
{
	INT	i, n, at, k;

	for ( i = 0; i < tries; i++ ) {
		n = log_from(pos, logb, LOG_MAX);
		at = find(logb, n, w, 0);
		if ( at >= 0 ) {
			if ( line != NULL ) {
				at += s_len(w);
				for ( k = 0; k < max - 1 && at + k < n && logb[at + k] != '\n'; k++ ) line[k] = (char)logb[at + k];
				line[k] = 0;
			}
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  waited in vain for \"%s\"\n", w);
	return FALSE;
}

/* ---------------------------------------------------------------- the objects */

/* An object of a name, a program and a record 0, on the system's volume */
LOCAL ER make( CONST char *name, CONST char *app, CONST char *win, CONST char *xml, TS_UUID *u )
{
	char	json[320];
	T_OBCRE	c;
	INT	rec = -1;
	SZ	asz = 0;
	ID	k;
	ER	er;

	tm_sprintf((UB *)json, (UB *)"{\"name\":\"%s\",\"applist\":{\"%s\":{\"name\":\"x\",\"defaultOpen\":true}}%s}",
		   name, app, win);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = s_len(json);
	c.near = cab0;
	er = ob_cre_obj(&c, u);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(u, OB_OP_ALL);
	if ( k <= 0 ) return E_NOEXS;
	er = ob_apd_rec(k, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = ob_wri_rec(k, rec, 0, xml, s_len(xml), &asz);
	ob_cls_obj(k);
	return er;
}

LOCAL char	xk[1024];

/* The window whose attributes say it shows u, and its number */
LOCAL INT window_showing( CONST TS_UUID *u )
{
	TS_UUID	list[64];
	char	us[40], want[64];
	UB	j[512];
	INT	cnt = 0, i, p, wid = -1;
	SZ	asz = 0;
	D	d = 0;
	ID	kw;

	ts_uuid_to_str(u, us, sizeof(us));
	tm_sprintf((UB *)want, (UB *)"\"shows\":\"%s\"", us);
	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt && i < 64 && wid < 0; i++ ) {
		kw = ob_opn_obj(&list[i], OB_OP_ATRRD);
		if ( kw <= 0 ) continue;
		if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK && find(j, (INT)asz, want, 0) >= 0 ) {
			p = knl_oj_path(j, (INT)asz, "tessronos", "window");
			p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
			if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &d) ) wid = (INT)d;
		}
		ob_cls_obj(kw);
	}
	return wid;
}

/* The window of that name, and its number */
LOCAL INT window_named( CONST char *name, TS_UUID *out )
{
	TS_UUID	list[64];
	T_OBREF	r;
	UB	j[256];
	INT	cnt = 0, i, p, wid = -1;
	SZ	asz = 0;
	D	d = 0;
	ID	kw;

	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_WINDOW
		  || find(r.name, s_len((CONST char *)r.name), name, 0) < 0 ) continue;
		*out = list[i];
		kw = ob_opn_obj(out, OB_OP_ATRRD);
		if ( kw > 0 ) {
			if ( ob_get_atr(kw, j, sizeof(j) - 1, &asz) >= E_OK ) {
				p = knl_oj_path(j, (INT)asz, "tessronos", "window");
				p = ( p >= 0 ) ? knl_oj_member(j, (INT)asz, p, "number") : -1;
				if ( p >= 0 && knl_oj_num(j, (INT)asz, p, &d) ) wid = (INT)d;
			}
			ob_cls_obj(kw);
		}
	}
	return wid;
}

/* The desktop asked to open an object, as a process asks it; its answer */
LOCAL ER ask_open( CONST TS_UUID *u )
{
	TS_UUID	d, reply;
	T_DTREQ	rq;
	T_DTANS	an;
	T_OBCRE	c;
	SZ	asz = 0;
	ID	k, ka;
	INT	i;
	ER	er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	knl_memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.seq = 7;
	rq.target = *u;
	rq.recno = -1;
	rq.reply = reply;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( i = 0; er >= E_OK && i < 150; i++ ) {
		if ( ob_rea_rec(ka, 0, 0, &an, sizeof(an), &asz) >= E_OK && asz == (SZ)sizeof(an) && an.seq == rq.seq ) {
			er = an.er;
			break;
		}
		tk_dly_tsk(20);
	}
	if ( i >= 150 ) er = E_TMOUT;
	if ( ka > 0 ) ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* ---------------------------------------------------------------- the hand */

LOCAL void hid( UINT type, INT x, INT y, UINT mods )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.x = x;
	e.y = y;
	e.mods = mods;
	(void)kt_inject(&e);
	tk_dly_tsk(60);
}

/* Pressed and let go where it is, as a click */
LOCAL void click( INT x, INT y, UINT mods )
{
	hid(HID_EV_MOVE, x, y, 0);
	hid(HID_EV_BTN_DOWN, x, y, mods);
	hid(HID_EV_BTN_UP, x, y, 0);
	tk_dly_tsk(500);			/* not taken for a second press */
}

/* Pressed at one place, carried to another in steps, let go there */
LOCAL void drag( INT x0, INT y0, INT x1, INT y1, UINT mods )
{
	INT	k;

	hid(HID_EV_MOVE, x0, y0, 0);
	hid(HID_EV_BTN_DOWN, x0, y0, mods);
	for ( k = 1; k <= 8; k++ ) {
		hid(HID_EV_MOVE, x0 + ( x1 - x0 ) * k / 8, y0 + ( y1 - y0 ) * k / 8, 0);
	}
	hid(HID_EV_BTN_UP, x1, y1, 0);
	tk_dly_tsk(300);
}

/* A link of the cabinet on the screen: a point inside its box, clear of its edges */
LOCAL void link_at( INT left, INT top, INT *x, INT *y )
{
	T_WMWIN	w;

	*x = *y = -1;
	if ( cwid > 0 && wm_ref(cwid, &w) >= E_OK ) {
		*x = w.work.left + DT_MARGIN + left + 20;
		*y = w.work.top + DT_MARGIN + top + L_H / 2;
	}
}

/* A place in the work area of a window, on the screen */
LOCAL BOOL at_work( INT wid, INT dx, INT dy, INT *x, INT *y )
{
	T_WMWIN	w;

	if ( wm_ref(wid, &w) < E_OK ) return FALSE;
	*x = w.work.left + dx;
	*y = w.work.top + dy;
	return TRUE;
}

/* A window put at (left, top), its size kept */
LOCAL void place_window( CONST TS_UUID *win )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		w, h;
	ID		k = ob_opn_obj(win, OB_OP_ALL);

	if ( k <= 0 ) return;
	if ( ob_rea_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		w = wp.right - wp.left;
		h = wp.bottom - wp.top;
		wp.left = ACC_LEFT;
		wp.top = ACC_TOP;
		wp.right = ACC_LEFT + w;
		wp.bottom = ACC_TOP + h;
		wp.z = 0;
		wp.flags = OB_WP_SHOWN;
		(void)ob_wri_rec(k, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	ob_cls_obj(k);
	tk_dly_tsk(200);
}

/* A key pressed on a window */
LOCAL void key_on( INT wid, UINT code )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.wid = wid;
	ev.code = code;
	knl_wmobj_event(&ev);
	tk_dly_tsk(100);
}

LOCAL void type_on( INT wid, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		UINT	code = 0;

		if ( *s >= 'a' && *s <= 'z' ) code = 0x04 + ( *s - 'a' );
		else if ( *s >= '1' && *s <= '9' ) code = 0x1E + ( *s - '1' );
		else if ( *s == '0' ) code = 0x27;
		if ( code != 0 ) key_on(wid, code);
	}
}

/* An accessory started as a process: its process, its window put clear of the cabinet */
LOCAL ID start( CONST char *prog_id, CONST char *ready, CONST char *title, TS_UUID *win, INT *wid )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	UD	pos = log_now();
	INT	i;
	ID	pid;

	*wid = -1;
	if ( ts_str_to_uuid(prog_id, &prog) < E_OK ) return E_SYS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	if ( ob_cre_obj(&c, &pu) < E_OK ) return E_OBJ;
	pid = knl_prc_of_uuid(&pu);
	if ( pid <= 0 || !wait_for(pos, ready, 100, NULL, 0) ) return E_TMOUT;
	for ( i = 0; i < 30 && *wid <= 0; i++ ) {
		*wid = window_named(title, win);
		if ( *wid <= 0 ) tk_dly_tsk(100);
	}
	if ( *wid > 0 ) place_window(win);
	return pid;
}

LOCAL void finish( ID pid, CONST TS_UUID *win )
{
	T_PSTS	psts;

	(void)ob_del_obj(win);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

/* ---------------------------------------------------------------- the tests */

/* Two texts and a cabinet of links to them; the desktop up, the cabinet open in it */
LOCAL void test_setup( void )
{
	char	ia[40], ib[40];
	UD	pos;
	INT	i;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CABINET, &cab0), E_OK);
	KT_ASSERT_ER(make("KT投下文章A", "basic-text-editor", "",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>落とす文章 A</p></document></tad>\n",
			  &txa), E_OK);
	KT_ASSERT_ER(make("KT投下文章B", "basic-text-editor", "",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>落とす文章 B</p></document></tad>\n",
			  &txb), E_OK);
	ts_uuid_to_str(&txa, ia, sizeof(ia));
	ts_uuid_to_str(&txb, ib, sizeof(ib));
	tm_sprintf((UB *)xk, (UB *)"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000d001\" vobjleft=\"%d\" vobjtop=\"%d\""
		   " vobjright=\"%d\" vobjbottom=\"%d\" height=\"25\" chsz=\"14\"/>"
		   "<link id=\"%s_0.xtad\" vobjid=\"01a0d8c4-0000-7000-8000-00000000d002\" vobjleft=\"%d\" vobjtop=\"%d\""
		   " vobjright=\"%d\" vobjbottom=\"%d\" height=\"25\" chsz=\"14\"/>"
		   "</figure></tad>\n",
		   ia, L1_LEFT, L1_TOP, L1_LEFT + L_W, L1_TOP + L_H,
		   ib, L2_LEFT, L2_TOP, L2_LEFT + L_W, L2_TOP + L_H);
	KT_ASSERT_ER(make("KT投下箱", "virtual-object-list",
			  ",\"window\":{\"pos\":{\"x\":20,\"y\":80},\"width\":320,\"height\":240}", xk, &cab), E_OK);
	made = TRUE;

	pos = log_now();
	KT_ASSERT_ER(dt_start(), E_OK);
	up = wait_for(pos, "TessronOS desktop: start", 100, NULL, 0);
	KT_ASSERT(up);
	if ( !up ) return;
	for ( i = 0; i < 50; i++ ) {
		TS_UUID	d;

		if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) >= E_OK ) break;
		tk_dly_tsk(100);
	}
	tk_dly_tsk(1000);			/* the first cabinet opened, and drawn */
	KT_ASSERT_ER(ask_open(&cab), E_OK);
	for ( i = 0; i < 30 && cwid <= 0; i++ ) {
		cwid = window_showing(&cab);
		if ( cwid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(cwid > 0);
}

/* A file of the medium taken away, whatever it was called; FALSE when there was none */
LOCAL BOOL rm_named( CONST char *dir, CONST char *name )
{
	char	p[FS_PATH_MAX];

	knl_strcpy(p, dir);
	knl_strcat(p, "/");
	knl_strcat(p, name);
	return (BOOL)( fs_unlink(p) >= 0 );
}

/* The superfloppy: the disk object whose blocks (record 1) are 34 MB */
LOCAL BOOL superfloppy( char *name, INT max )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	cnt = 0, i;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, list, 64, &cnt) < E_OK ) return FALSE;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		T_OBREC	rc[2];
		INT	nr = 0;
		ID	k;

		if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_DISK ) continue;
		k = ob_opn_obj(&list[i], OB_OP_ATRRD);
		if ( k <= 0 ) continue;
		if ( ob_lst_rec(k, rc, 2, &nr) >= E_OK && nr >= 2 && rc[1].size == SF_BYTES ) {
			ob_cls_obj(k);
			knl_strncpy(name, (CONST char *)r.name, max - 1);
			name[max - 1] = 0;
			return TRUE;
		}
		ob_cls_obj(k);
	}
	return FALSE;
}

LOCAL INT links_to( DTWIN *d, CONST TS_UUID *u, T_VOBJ *v );

/* A key pressed on a window with the keys held */
LOCAL void key_mods( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.wid = wid;
	ev.code = code;
	ev.mods = mods;
	knl_wmobj_event(&ev);
	tk_dly_tsk(100);
}

/* A panel of the accessory's, by its name, answered with Enter */
LOCAL BOOL panel_enter( CONST char *name )
{
	TS_UUID	u;
	INT	i, pw = -1;

	for ( i = 0; i < 50 && pw <= 0; i++ ) {
		pw = window_named(name, &u);
		if ( pw <= 0 ) tk_dly_tsk(100);
	}
	if ( pw <= 0 ) {
		tm_printf((UB *)"  waited in vain for the panel \"%s\"\n", name);
		return FALSE;
	}
	tk_dly_tsk(200);
	key_on(pw, K_ENTER);
	return TRUE;
}

#define XF_ROW_X	40		/* the middle of the 仮身 of row i of a file list (xfc_draw.c) */
#define XF_ROW_Y(i)	( 10 + (i) * 32 + 3 + 12 )
#define K_DOWN		0x51

LOCAL TS_UUID	fig2;			/* the figure the files are carried into */

/*
 * The row of the file list that shows name, from the last listing the
 * accessory said since pos ("xfconv: dir ..." and a "- name" line for
 * each entry): the volume is row 0 and the entries follow in the order
 * said; -1 when it is not there
 */
LOCAL INT row_of( UD pos, CONST char *name )
{
	INT	n = log_from(pos, logb, LOG_MAX), at = 0, dir = -1, k, row, nl = s_len(name);

	while ( ( at = find(logb, n, "xfconv: dir ", at) ) >= 0 ) dir = at++;
	if ( dir < 0 ) return -1;
	row = 1;
	for ( at = dir; ( at = find(logb, n, "xfconv: - ", at) ) >= 0; at++ ) {
		k = at + 10;
		if ( k + nl < n && find(logb, k + nl + 1, name, k) == k
		  && ( logb[k + nl] == ' ' || logb[k + nl] == '/' ) ) return row;
		row++;
	}
	return -1;
}


/*
 * ファイル変換: a link let go on its main window, where the media are
 * listed, is refused; the superfloppy opened in a window of its own,
 * a link let go on it is written into its root after the panel is
 * answered; two files chosen there and carried into a figure of the
 * desktop become 実身 there, after their panel, with a link to each
 * put where they were let go.
 */
LOCAL void test_xfconv( void )
{
	TS_UUID	win, fwin, prog, u;
	T_OBREF	r;
	char	sf[16], dir[64], line[256], name[128], ia[40], fs[40];
	UD	pos;
	INT	wid, fwid, twid = -1, x0 = 0, y0 = 0, x1 = 0, y1 = 0, k;
	UD	lpos;
	ID	pid;

	if ( !up || cwid <= 0 ) KT_SKIP("no desktop");
	if ( ts_str_to_uuid(SYSDEF_PROG_XFCONV, &prog) < E_OK || ob_ref_obj(&prog, &r) < E_OK ) {
		KT_SKIP("no ファイル変換 program");
	}
	if ( !superfloppy(sf, sizeof(sf)) ) KT_SKIP("no superfloppy disk");
	pid = start(SYSDEF_PROG_XFCONV, "xfconv: ready", "ファイル変換", &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;
	ts_uuid_to_str(&txa, ia, sizeof(ia));

	/* the media listed: nowhere to write, refused */
	pos = log_now();
	link_at(L1_LEFT, L1_TOP, &x0, &y0);
	KT_ASSERT(at_work(wid, 200, 300, &x1, &y1));
	drag(x0, y0, x1, y1, 0);
	KT_ASSERT(wait_for(pos, "xfconv: drop 1 at ", 50, line, sizeof(line)));
	KT_ASSERT(find((CONST UB *)line, s_len(line), ia, 0) >= 0);
	KT_ASSERT(find((CONST UB *)line, s_len(line), "ops 3f", 0) >= 0);	/* read and write, not delete nor protect */
	KT_ASSERT(wait_for(pos, "xfconv: drop refused", 50, NULL, 0));

	/* the superfloppy opened: a window of its own, put where the main one is */
	pos = log_now();
	lpos = pos;
	type_on(wid, sf);
	key_on(wid, K_ENTER);
	KT_ASSERT(wait_for(pos, "xfconv: mounted /media/", 100, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: - SUB/", 100, NULL, 0));
	fwid = -1;
	for ( k = 0; k < 30 && fwid <= 0; k++ ) {
		fwid = window_named(sf, &fwin);
		if ( fwid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(fwid > 0);
	if ( fwid <= 0 ) {
		finish(pid, &win);
		return;
	}
	place_window(&fwin);
	knl_strcpy(dir, FS_MEDIA_DIR);
	knl_strcat(dir, "/");
	knl_strcat(dir, sf);

	/* a link let go on it: the panel, and written into the root */
	pos = log_now();
	KT_ASSERT(at_work(fwid, 300, 330, &x1, &y1));
	drag(x0, y0, x1, y1, 0);
	KT_ASSERT(wait_for(pos, "xfconv: drop taken", 50, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: asked how to export", 50, NULL, 0));
	KT_ASSERT(panel_enter("ファイルへの変換"));
	KT_ASSERT(wait_for(pos, "xfconv: exported ", 300, line, sizeof(line)));
	for ( k = 0; line[k] != 0 && !( line[k] == ' ' && line[k + 1] == 'f' ); k++ ) name[k] = line[k];
	name[k] = 0;
	KT_ASSERT(find((CONST UB *)name, k, "KT投下文章A", 0) == 0);
	knl_strcpy(fs, "xfconv: selected 1: ");
	KT_ASSERT(wait_for(pos, fs, 50, line, sizeof(line)));
	KT_ASSERT(find((CONST UB *)line, s_len(line), "KT投下文章A", 0) == 0);	/* shown, and chosen */
	KT_ASSERT(rm_named(dir, name));

	/* a figure of the desktop's to carry into, clear of the other windows */
	KT_ASSERT_ER(make("KT取込先", "virtual-object-list",
			  ",\"window\":{\"pos\":{\"x\":20,\"y\":360},\"width\":320,\"height\":200}",
			  "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n", &fig2), E_OK);
	KT_ASSERT_ER(ask_open(&fig2), E_OK);
	for ( k = 0; k < 30 && twid <= 0; k++ ) {
		twid = window_showing(&fig2);
		if ( twid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(twid > 0);

	/* DATA.BIN and HELLO.TXT chosen and carried there */
	if ( twid > 0 ) {
		TS_UUID	made[2];
		DTWIN	*d;

		pos = log_now();
		type_on(fwid, "data");
		key_mods(fwid, K_DOWN, HID_MOD_LSHIFT);
		KT_ASSERT(wait_for(pos, "xfconv: selected 2: DATA.BIN, HELLO.TXT", 30, NULL, 0));
		KT_ASSERT(at_work(fwid, XF_ROW_X, XF_ROW_Y(row_of(lpos, "DATA.BIN")), &x0, &y0));
		KT_ASSERT(at_work(twid, DT_MARGIN + 60, DT_MARGIN + 40, &x1, &y1));
		pos = log_now();
		drag(x0, y0, x1, y1, 0);
		ts_uuid_to_str(&fig2, fs, sizeof(fs));
		knl_strcpy(line, "xfconv: carried 2 into ");
		knl_strcat(line, fs);
		KT_ASSERT(wait_for(pos, line, 50, NULL, 0));
		KT_ASSERT(panel_enter("実身への変換"));
		KT_ASSERT(wait_for(pos, "xfconv: imported 2 of 2 objects 2 into ", 300, line, sizeof(line)));
		KT_ASSERT(wait_for(pos, "xfconv: placed 2 links", 100, NULL, 0));
		tk_dly_tsk(300);
		d = dt_win_of(twid);
		KT_ASSERT(d != NULL && d->rec != NULL && tad_lnk_count(d->rec) == 2);
		for ( k = 0; d != NULL && d->rec != NULL && k < 2 && k < tad_lnk_count(d->rec); k++ ) {
			T_VOBJ	v;
			UB	*rec;
			SZ	size = 0;

			if ( tad_lnk_get(d->rec, k, &v) < E_OK ) continue;
			made[k] = v.target;
			rec = om_obj_record(&made[k], 1, &size);
			KT_ASSERT(rec != NULL && ( size == 4608 || ( size == 25 && find(rec, 25, "TessronOS FAT test file.", 0) == 0 ) ));
			if ( rec != NULL ) Kfree(rec);
			KT_ASSERT_EQ(links_to(d, &made[k], NULL), 1);
		}
		(void)u;
	}

	/* the file list closed: the disk taken off */
	pos = log_now();
	key_mods(fwid, 0x08, HID_MOD_LCTRL);		/* Ctrl+E */
	KT_ASSERT(wait_for(pos, "xfconv: unmounted ", 100, NULL, 0));
	finish(pid, &win);
}

/*
 * バックアップ: two links picked (the second with Shift) and carried
 * together become its list, marked to be saved; Enter saves both to
 * /boot, and the volume restored puts them back into the cabinet
 */
LOCAL void test_backup( void )
{
	TS_UUID	win, prog;
	T_OBREF	r;
	UD	pos;
	INT	wid, x0, y0, x1, y1;
	ID	pid;

	if ( !up || cwid <= 0 ) KT_SKIP("no desktop");
	if ( ts_str_to_uuid(SYSDEF_PROG_BACKUP, &prog) < E_OK || ob_ref_obj(&prog, &r) < E_OK ) {
		KT_SKIP("no backup program");
	}
	(void)fs_unlink(BKP_FILE);
	pid = start(SYSDEF_PROG_BACKUP, "backup: ready", "バックアップ", &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;

	pos = log_now();
	link_at(L1_LEFT, L1_TOP, &x0, &y0);
	click(x0, y0, 0);
	link_at(L2_LEFT, L2_TOP, &x0, &y0);
	KT_ASSERT(at_work(wid, 200, 150, &x1, &y1));
	drag(x0, y0, x1, y1, HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "backup: dropped 2", 50, NULL, 0));
	KT_ASSERT(wait_for(pos, "backup: drop taken", 50, NULL, 0));

	pos = log_now();
	key_on(wid, K_ENTER);
	KT_ASSERT(wait_for(pos, "backup: saved 2 objects in 1 volumes", 300, NULL, 0));
	KT_ASSERT(wait_for(pos, ", roots 2", 10, NULL, 0));
	{
		T_FSTAT	st;

		KT_ASSERT(fs_stat(BKP_FILE, &st) >= EX_OK && st.size > 0);
	}
	finish(pid, &win);

	/*
	 * Restored: the drop said which cabinet the links were in and where,
	 * so both new objects go back into it, each beside where it was
	 */
	{
		static T_BKARG	a;
		TS_UUID		pu, got[8];
		UB		*x = NULL;
		SZ		size = 0;
		T_PSTS		psts;
		INT		i, n = 0;
		ID		p2;

		knl_memset(&a, 0, sizeof(a));
		a.magic = BK_ARG_MAGIC;
		a.op = BK_OP_RESTORE;
		a.obj = cab0;
		knl_strcpy((char *)a.path, BKP_FILE);
		{
			T_OBCRE	c;

			knl_memset(&c, 0, sizeof(c));
			c.type = OB_T_PROCESS;
			c.prog = prog;
			c.arg = &a;
			c.argsz = sizeof(a);
			pos = log_now();
			KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
		}
		p2 = knl_prc_of_uuid(&pu);
		psts.exitcd = -1;
		KT_ASSERT(p2 > 0 && ts_wai_prc(p2, &psts, 60000) >= E_OK);
		KT_ASSERT_EQ(psts.exitcd, 0);
		KT_ASSERT(wait_for(pos, ", roots 2 back 2", 10, NULL, 0));
		x = om_obj_record(&cab, 0, &size);
		if ( x != NULL ) {
			n = om_store_links(x, size, got, 8);
			KT_ASSERT_EQ(count_of(x, (INT)size, "vobjleft=\"10\" vobjtop=\"10\""), 2);
			KT_ASSERT_EQ(count_of(x, (INT)size, "vobjleft=\"10\" vobjtop=\"50\""), 2);
			Kfree(x);
		}
		KT_ASSERT_EQ(n, 4);
		for ( i = 0; i < n; i++ ) {
			if ( ts_uuid_cmp(&got[i], &txa) != 0 && ts_uuid_cmp(&got[i], &txb) != 0 && nback < 2 ) {
				back[nback++] = got[i];
			}
		}
		KT_ASSERT_EQ(nback, 2);
	}
	(void)fs_unlink(BKP_FILE);
}

/* ---------------------------------------------------------------- a user's process */

#define SVCPROG		"/boot/SVCPROG.ELF"
#define SP_DROP		18		/* as tests/uprog/svcprog.c */

/* The object dropped on the window, as the desktop drops a link */
LOCAL ER drop_one( INT wid, CONST TS_UUID *target )
{
	T_OBDROPV	v;
	T_WMWIN		w;

	if ( wm_ref(wid, &w) < E_OK ) return E_NOEXS;
	knl_memset(&v, 0, sizeof(v));
	v.target = *target;
	knl_strcpy((char *)v.name, "ktdrop-secret");
	v.right = L_W;
	v.bottom = L_H;
	return wm_obj_drop(wid, w.work.left + 20, w.work.top + 20, 0, NULL, &v, 1);
}

/*
 * A process that is not an administrator's is dropped an object only
 * the system may open: while the drop is not answered it may open it
 * for reading and writing, as the one who dropped it may; refused, it
 * may not; taken, it may again, but it may not delete it nor change its
 * protection; and when the process ends, what it was granted goes.
 */
LOCAL void test_grant( void )
{
	CONST char	*uj = "{\"name\":\"ktdropuser\",\"tessronos\":{\"user\":{\"name\":\"ktdropuser\",\"groups\":[]}}}";
	T_OBCRE		c;
	T_OBPRT		*prt;
	T_FSTAT		st;
	T_CPRC		cprc;
	T_PSTS		psts;
	T_TSMSG		*m;
	TS_UUID		user, secret, win;
	UB		b[8];
	UW		arg[8];
	SZ		asz = 0;
	INT		wid = -1, recno = -1, t;
	ID		pid, taker = 0, k;

	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)uj;
	c.jsonsz = s_len(uj);
	if ( ob_cre_obj(&c, &user) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&user, (CONST UB *)"ktest-pw"), E_OK);

	/* the system's, and nobody else's */
	prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	KT_ASSERT(prt != NULL && m != NULL);
	if ( prt == NULL || m == NULL ) goto done0;
	knl_memset(prt, 0, sizeof(*prt));
	prt->owner = ob_user_system;
	prt->group = ob_group_admin;
	prt->mode = 0600;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.name = (CONST UB *)"ktdrop-secret";
	c.prt = prt;
	KT_ASSERT_ER(ob_cre_obj(&c, &secret), E_OK);
	k = ob_opn_obj(&secret, OB_OP_ALL);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_apd_rec(k, OB_RT_SYSDATA, 0, &recno), E_OK);
		KT_ASSERT_ER(ob_wri_rec(k, 0, 0, "secret", 6, &asz), E_OK);
		ob_cls_obj(k);
	}

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_DROP;
	knl_memcpy(&arg[1], &user, sizeof(user));
	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(arg);
	pid = ts_cre_prc(SVCPROG, &cprc);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) goto done1;
	for ( t = 0; t < 100 && ( wid <= 0 || !knl_ob_taker(&win, OB_E_DROP, &taker) ); t++ ) {
		tk_dly_tsk(50);
		wid = window_named("ktdrop", &win);
	}
	KT_ASSERT(wid > 0);
	KT_ASSERT_EQ(taker, pid);
	if ( wid > 0 && taker == pid ) {
		/* the first: granted until refused (the process opens it meanwhile) */
		KT_ASSERT_EQ(knl_ob_granted(pid, &secret), 0);
		KT_ASSERT_ER(drop_one(wid, &secret), E_OK);
		for ( t = 0; t < 100 && knl_ob_granted(pid, &secret) != 0; t++ ) tk_dly_tsk(50);
		KT_ASSERT_EQ(knl_ob_granted(pid, &secret), 0);

		/* the second: taken, and used */
		KT_ASSERT_ER(drop_one(wid, &secret), E_OK);
		for ( t = 0; t < 100; t++ ) {
			k = ob_opn_obj(&secret, OB_OP_R);
			b[0] = 0;
			if ( k > 0 ) {
				(void)ob_rea_rec(k, 0, 0, b, 6, &asz);
				ob_cls_obj(k);
			}
			if ( b[0] == 'o' ) break;
			tk_dly_tsk(50);
		}
		KT_ASSERT_EQ(b[0], 'o');			/* "opened": written through the grant */
		KT_ASSERT_EQ(knl_ob_granted(pid, &secret), OB_DROP_OPS);
		KT_ASSERT_ER(ob_get_prt(&secret, prt), E_OK);
		KT_ASSERT_EQ(prt->mode, 0600);			/* as it was */
	}
	knl_memset(m, 0, sizeof(T_TSMSG));
	m->type = 1;
	m->size = 1;
	KT_ASSERT_ER(ts_snd_msg(pid, m, 1000), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	if ( psts.exitcd != 0 ) tm_printf((UB *)"  the program stopped at step %d\n", psts.exitcd);
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT_EQ(knl_ob_granted(pid, &secret), 0);	/* gone with it */
	if ( window_named("ktdrop", &win) > 0 ) {
		KT_ASSERT(FALSE);				/* the program takes its window away */
		(void)ob_del_obj(&win);
	}
done1:
	KT_ASSERT_ER(ob_del_obj(&secret), E_OK);
done0:
	if ( prt != NULL ) Kfree(prt);
	if ( m != NULL ) Kfree(m);
	KT_ASSERT_ER(ob_del_obj(&user), E_OK);
	wm_update();
}

/* ---------------------------------------------------------------- an archive taken out where it is carried */

#define UNP_ARCHIVE	"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1"	/* the sample archive object */
#define UNP_TITLE	"20251018発表資料公開用"
#define UNP_ROOT	"BTRON　CLUB発表公開用"
#define UNP_NOBJ	33
#define UNP_ROOT_X	(16 + 8)		/* in the root's link in the program's window */
#define UNP_ROOT_Y	(30 + 26 + 26 + 14 + 6)
#define UNP_FAT		"/boot/UNPFAT"

/* The program started on an archive object: its process, its window put clear of the cabinet */
LOCAL ID start_on( CONST char *prog_id, CONST TS_UUID *arg, CONST char *ready, CONST char *title,
		   TS_UUID *win, INT *wid )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	UD	pos = log_now();
	INT	i;
	ID	pid;

	*wid = -1;
	if ( ts_str_to_uuid(prog_id, &prog) < E_OK ) return E_SYS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	c.arg = arg;
	c.argsz = sizeof(*arg);
	if ( ob_cre_obj(&c, &pu) < E_OK ) return E_OBJ;
	pid = knl_prc_of_uuid(&pu);
	if ( pid <= 0 || !wait_for(pos, ready, 100, NULL, 0) ) return E_TMOUT;
	for ( i = 0; i < 30 && *wid <= 0; i++ ) {
		*wid = window_named(title, win);
		if ( *wid <= 0 ) tk_dly_tsk(100);
	}
	if ( *wid > 0 ) place_window(win);
	return pid;
}

/* A key pressed and let go as the keyboard reports it, to whatever has the keys */
LOCAL void key_hid( UINT code )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = HID_EV_KEY_DOWN;
	e.code = code;
	(void)kt_inject(&e);
	tk_dly_tsk(60);
	e.type = HID_EV_KEY_UP;
	(void)kt_inject(&e);
	tk_dly_tsk(200);
}

/*
 * The root's link taken in the program's window and carried to (x, y)
 * of the window twid, the desktop's question answered yes with Enter:
 * the objects are taken out and the link put. The root, from what the
 * program said; FALSE when it did not get that far.
 */
/* How many panels are open */
LOCAL INT open_panels( void )
{
	INT	pid, n = 0;

	for ( pid = 1; pid <= WM_PANEL_MAX; pid++ ) {
		if ( wm_panel_wid(pid) > 0 ) n++;
	}
	return n;
}

/*
 * Until more than 'before' panels are open, for up to 'tries' tenths of
 * a second. A panel comes up in its own time, later on a larger screen;
 * a key pressed before it is there goes to whatever else has the focus.
 */
LOCAL BOOL wait_panel( INT before, INT tries )
{
	INT	i;

	for ( i = 0; i < tries; i++ ) {
		if ( open_panels() > before ) {
			tk_dly_tsk(100);	/* and taking keys */
			return TRUE;
		}
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  waited in vain for a panel\n");
	return FALSE;
}

LOCAL BOOL carry_root( INT uwid, INT twid, INT tx, INT ty, TS_UUID *root )
{
	char	line[128];
	UD	pos = log_now();
	INT	x0, y0, x1, y1, np = open_panels();

	if ( !at_work(uwid, UNP_ROOT_X, UNP_ROOT_Y, &x0, &y0) || !at_work(twid, tx, ty, &x1, &y1) ) return FALSE;
	drag(x0, y0, x1, y1, 0);
	if ( !wait_panel(np, 50) ) return FALSE;	/* the question up */
	key_hid(K_ENTER);
	if ( !wait_for(pos, "unpack: done 33 objects, root ", 600, line, sizeof(line)) ) return FALSE;
	line[TS_UUID_STRLEN] = 0;
	if ( ts_str_to_uuid(line, root) < E_OK ) return FALSE;
	return wait_for(pos, "unpack: root placed on the desktop", 100, NULL, 0);
}

/* The links of a window's record to an object: how many, and the first's box */
LOCAL INT links_to( DTWIN *d, CONST TS_UUID *u, T_VOBJ *v )
{
	INT	i, n = 0, cnt = ( d != NULL && d->rec != NULL ) ? tad_lnk_count(d->rec) : 0;
	T_VOBJ	w;

	for ( i = 0; i < cnt; i++ ) {
		if ( tad_lnk_get(d->rec, i, &w) >= E_OK && ts_uuid_cmp(&w.target, u) == 0 ) {
			if ( n++ == 0 && v != NULL ) *v = w;
		}
	}
	return n;
}

/*
 * Every object from the root on: each counted once, with a name and
 * an icon, its record 0 a whole xmlTAD; the root the text of 27 links.
 * How many there were; into objs, and the links each is pointed at by
 * from the others into nref (plus what the caller counts itself).
 */
LOCAL INT walk_root( CONST TS_UUID *root, TS_UUID *objs, INT *nref, INT max )
{
	TS_UUID	list[64];
	UB	*x, name[TAD_NAME_MAX];
	SZ	len = 0;
	INT	nobj = 0, i, k, j, nl;

	objs[nobj] = *root;
	nref[nobj++] = 0;
	for ( i = 0; i < nobj; i++ ) {
		ID	key;
		SZ	asz = 0;

		KT_ASSERT(om_store_name(&objs[i], name, sizeof(name)) > 0);
		key = ob_opn_obj(&objs[i], OB_OP_R);
		KT_ASSERT(key > 0);
		if ( key > 0 ) {
			KT_ASSERT(ob_get_ico(key, NULL, 0, &asz) >= E_OK && asz > 0);
			ob_cls_obj(key);
		}
		x = om_obj_record(&objs[i], 0, &len);
		KT_ASSERT(x != NULL);
		if ( x == NULL ) continue;
		KT_ASSERT(find(x, (INT)len, "<tad version=\"1.0\"", 0) >= 0 && find(x, (INT)len, "</tad>", 0) >= 0);
		nl = om_store_links(x, len, list, 64);
		if ( nl > 64 ) nl = 64;
		if ( i == 0 ) {
			KT_ASSERT(find(name, s_len((CONST char *)name), UNP_ROOT, 0) == 0);
			KT_ASSERT(find(x, (INT)len, "<document>", 0) >= 0);
			KT_ASSERT_EQ(nl, 27);
		}
		for ( k = 0; k < nl; k++ ) {
			for ( j = 0; j < nobj && ts_uuid_cmp(&objs[j], &list[k]) != 0; j++ ) ;
			if ( j < nobj ) {
				nref[j]++;
			} else if ( nobj < max ) {
				objs[nobj] = list[k];
				nref[nobj++] = 1;
			}
		}
		Kfree(x);
	}
	return nobj;
}

/*
 * 書庫解凍: the root's link, shown in the program's window, carried into
 * the cabinet and let go; asked, and answered yes, the archive is taken
 * out onto the cabinet's volume and the link to the root is put where
 * it was let go, looking as the archive says. The cabinet saved, every
 * object is counted. A click on the root, or a carry let go on the
 * ground, takes nothing out.
 */
LOCAL void test_unpack( void )
{
	TS_UUID	arc, win, root, objs[UNP_NOBJ + 8];
	INT	nref[UNP_NOBJ + 8];
	T_OBREF	r;
	T_VOBJ	v;
	DTWIN	*d;
	UD	pos;
	INT	wid = -1, nobj, i, x, y;
	ID	pid;

	if ( !up || cwid <= 0 ) KT_SKIP("no desktop");
	KT_ASSERT_ER(ts_str_to_uuid(UNP_ARCHIVE, &arc), E_OK);
	if ( ob_ref_obj(&arc, &r) < E_OK ) KT_SKIP("no sample archive");
	pid = start_on(SYSDEF_PROG_UNPACK, &arc, "unpack: ready 33 objects", UNP_TITLE, &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;

	/* a click is no carry, and let go on the ground takes nothing out */
	pos = log_now();
	if ( at_work(wid, UNP_ROOT_X, UNP_ROOT_Y, &x, &y) ) click(x, y, 0);
	drag(x, y, 900, 700, 0);
	tk_dly_tsk(500);
	KT_ASSERT(!wait_for(pos, "unpack: done", 10, NULL, 0));

	/* carried into the cabinet, beside its two links */
	d = dt_win_of(cwid);
	KT_ASSERT(carry_root(wid, cwid, DT_MARGIN + 200, DT_MARGIN + 100, &root));
	KT_ASSERT_EQ(links_to(d, &root, &v), 1);
	KT_ASSERT(v.left >= 150 && v.right - v.left > 40);	/* where it was let go, as wide as the archive says */
	KT_ASSERT(d != NULL && d->dirty);
	if ( d != NULL ) KT_ASSERT_ER(dt_save(d), E_OK);

	nobj = walk_root(&root, objs, nref, UNP_NOBJ + 8);
	KT_ASSERT_EQ(nobj, UNP_NOBJ);
	for ( i = 0; i < nobj; i++ ) {
		KT_ASSERT_ER(ob_ref_obj(&objs[i], &r), E_OK);
		KT_ASSERT(r.refcnt >= 1);
	}

	/* the program ends when its window is asked to close; what it made taken away */
	finish(pid, &win);
	if ( d != NULL ) {
		(void)tad_lnk_del((T_TAD *)d->rec, &v.vobjid);
		ed_model(d);
		(void)dt_save(d);
	}
	for ( i = nobj - 1; i >= 0; i-- ) (void)ob_del_obj(&objs[i]);
	wm_update();
}

/*
 * The same onto a store that does not count the links of its records (a
 * FAT directory): a figure there opened on the desktop, the root carried
 * into it. Saved, every object's count is the number of links to it --
 * from the others, and the root's from the figure. Then all of it let
 * go of and deleted, and nothing is left.
 */
LOCAL void test_unpack_fat( void )
{
	TS_UUID	arc, win, fig, fwin, root, list[64], objs[UNP_NOBJ + 8];
	INT	nref[UNP_NOBJ + 8];
	T_OBCRE	c;
	T_OBREF	r;
	CONST char *j = "{\"name\":\"KT解凍先\",\"applist\":{\"basic-figure-editor\":{\"name\":\"x\",\"defaultOpen\":true}},"
			"\"window\":{\"pos\":{\"x\":20,\"y\":380},\"width\":320,\"height\":200}}";
	CONST char *fx = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure></figure></tad>\n";
	UB	*x;
	SZ	asz = 0, len = 0;
	INT	wid = -1, fwid = -1, nobj, i, k, nl, rec = -1;
	ID	pid, key;
	BOOL	att = FALSE;

	if ( !up ) KT_SKIP("no desktop");
	KT_ASSERT_ER(ts_str_to_uuid(UNP_ARCHIVE, &arc), E_OK);
	if ( ob_ref_obj(&arc, &r) < E_OK ) KT_SKIP("no sample archive");
	if ( ob_att_vol(UNP_FAT, TSFS_STORE_FAT) < E_OK ) KT_SKIP("no FAT store");
	att = TRUE;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)j;
	c.jsonsz = s_len(j);
	c.vol = UNP_FAT;
	KT_ASSERT_ER(ob_cre_obj(&c, &fig), E_OK);
	key = ob_opn_obj(&fig, OB_OP_ALL);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_apd_rec(key, OB_RT_TAD, 0, &rec), E_OK);
		KT_ASSERT_ER(ob_wri_rec(key, rec, 0, fx, s_len(fx), &asz), E_OK);
		ob_cls_obj(key);
	}
	KT_ASSERT_ER(ask_open(&fig), E_OK);
	for ( i = 0; i < 30 && fwid <= 0; i++ ) {
		fwid = window_showing(&fig);
		if ( fwid <= 0 ) tk_dly_tsk(100);
	}
	KT_ASSERT(fwid > 0);
	pid = start_on(SYSDEF_PROG_UNPACK, &arc, "unpack: ready 33 objects", UNP_TITLE, &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( fwid <= 0 || pid <= 0 || wid <= 0 ) goto out;

	KT_ASSERT(carry_root(wid, fwid, DT_MARGIN + 20, DT_MARGIN + 20, &root));
	KT_ASSERT_ER(dt_save(dt_win_of(fwid)), E_OK);
	finish(pid, &win);

	/* the counts: the root's from the figure, the rest from each other */
	nobj = walk_root(&root, objs, nref, UNP_NOBJ + 8);
	KT_ASSERT_EQ(nobj, UNP_NOBJ);
	nref[0]++;
	for ( i = 0; i < nobj; i++ ) {
		KT_ASSERT_ER(ob_ref_obj(&objs[i], &r), E_OK);
		KT_ASSERT_EQ(r.flags & OB_F_AUTOREF, 0);
		KT_ASSERT_EQ(r.refcnt, nref[i]);
		if ( r.refcnt != nref[i] ) tm_printf((UB *)"  object %d: refcnt %d, links %d\n", i, r.refcnt, nref[i]);
	}

	/* the figure's window closed; every link let go of, then each object deleted */
	if ( window_named("KT解凍先", &fwin) > 0 ) (void)ob_del_obj(&fwin);
	tk_dly_tsk(300);
	for ( i = -1; i < nobj; i++ ) {
		x = om_obj_record(( i < 0 ) ? &fig : &objs[i], 0, &len);
		if ( x == NULL ) continue;
		nl = om_store_links(x, len, list, 64);
		for ( k = 0; k < nl && k < 64; k++ ) KT_ASSERT_ER(ob_unl_obj(&list[k]), E_OK);
		Kfree(x);
	}
	for ( i = nobj - 1; i >= 0; i-- ) KT_ASSERT_ER(ob_del_obj(&objs[i]), E_OK);
	for ( i = 0; i < nobj; i++ ) KT_ASSERT(ob_ref_obj(&objs[i], &r) < E_OK);
out:
	(void)ob_del_obj(&fig);
	if ( att ) KT_ASSERT_ER(ob_det_vol(UNP_FAT), E_OK);
	wm_update();
}

EXPORT void ktest_drop( void )
{
	logb = (UB *)Kmalloc(LOG_MAX);
	if ( logb == NULL ) return;
	KT_RUN(test_setup);
	KT_RUN(test_xfconv);
	KT_RUN(test_backup);
	KT_RUN(test_grant);
	KT_RUN(test_unpack);
	KT_RUN(test_unpack_fat);
	if ( up ) {
		dt_quit();
		up = FALSE;
	}
	if ( made ) {
		(void)ob_del_obj(&cab);
		while ( nback > 0 ) (void)ob_del_obj(&back[--nback]);
		(void)ob_del_obj(&txa);
		(void)ob_del_obj(&txb);
		made = FALSE;
	}
	wm_update();
	Kfree(logb);
	logb = NULL;
}
