/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_xfc.c
 *	ファイル変換, the accessory, run from end to end (design 17.17)
 *
 *	The accessory is started as a process and worked with keys and
 *	presses sent to its windows, as a person would: the main window
 *	lists the disks; the USB disk with no partition table (the 34 MB
 *	FAT32 one) is chosen by typing its name and opened with Enter, which
 *	mounts it and opens a window listing its root. There files are
 *	chosen several at a time -- Shift and the arrows, Shift and the
 *	button, a rectangle -- and two are made 実身 through the panel that
 *	asks how, into a new box; the folder SUB is made 実身 and written
 *	back beside itself through the other panel, and the two folders are
 *	compared byte for byte; closing the window takes the disk off.
 *	Started on an object, the accessory writes that object and what it
 *	links to as TADjs file sets. Under NET=user it also asks for SSH,
 *	which is refused as not there yet, then connects to the FTP server
 *	on the host through its panel, takes a file in, writes it back, and
 *	the settings keep the connection but not the password.
 *
 *	What the accessory did is read from the console, where it says it.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/wm.h>
#include <ts/fs.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "../../peripheral_kernel/obj/obj.h"
#include "../../outer_kernel/wm/wmobj.h"

#define LOG_MAX		( 128 * 1024 )
#define SF_BYTES	( (UD)34816 * 1024 )	/* the superfloppy's size */
#define WIN_NAME	"ファイル変換"

#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_TAB		0x2B
#define K_DOWN		0x51

LOCAL UB	*logb;

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

LOCAL INT bytes_cmp( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*x = (CONST UB *)a, *y = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( x[i] != y[i] ) return (INT)x[i] - (INT)y[i];
	}
	return 0;
}

/* The console from pos on */
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

/*
 * Waited for (at most tries tenths of a second) until the console says
 * w after pos; the rest of its line into line (may be NULL)
 */
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
	n = log_from(pos, logb, LOG_MAX);
	tm_printf((UB *)"  waited in vain for \"%s\"\n", w);
	return FALSE;
}

/* The window of that name, and its number */
LOCAL INT window_of( CONST char *name, TS_UUID *out )
{
	TS_UUID	list[64];
	T_OBREF	r;
	INT	cnt = 0, i, wid = -1, p;
	ID	kw;

	if ( ob_lst_obj(OB_T_WINDOW, 0, NULL, list, 64, &cnt) < E_OK ) return -1;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_WINDOW
		  || find(r.name, s_len((CONST char *)r.name), name, 0) < 0 ) continue;
		*out = list[i];
		kw = ob_opn_obj(out, OB_OP_R);
		if ( kw > 0 ) {
			UB	j[256];
			SZ	asz = 0;
			D	d = 0;

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

/* A key pressed on the window */
LOCAL void press( INT wid, UINT code, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = HID_EV_KEY_DOWN;
	ev.wid = wid;
	ev.code = code;
	ev.mods = mods;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
}

/* Letters, digits and dots typed, as the keyboard's usages */
LOCAL void type( INT wid, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		char	c = *s;
		UINT	code = 0, mods = 0;

		if ( c >= 'a' && c <= 'z' ) code = 0x04 + ( c - 'a' );
		else if ( c >= 'A' && c <= 'Z' ) {
			code = 0x04 + ( c - 'A' );
			mods = HID_MOD_LSHIFT;
		} else if ( c >= '1' && c <= '9' ) code = 0x1E + ( c - '1' );
		else if ( c == '0' ) code = 0x27;
		else if ( c == '.' ) code = 0x37;
		else if ( c == ' ' ) code = 0x2C;
		if ( code != 0 ) press(wid, code, mods);
	}
}

LOCAL void ctrl( INT wid, char c )
{
	press(wid, 0x04 + ( c - 'a' ), HID_MOD_LCTRL);
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

/* A file of the file layer whole into buf; its length or -1 */
LOCAL INT fs_all( CONST char *path, UB *buf, INT max )
{
	INT	fd = fs_open(path, O_RDONLY), n = 0, k;

	if ( fd < 0 ) return -1;
	while ( n < max && ( k = fs_read(fd, buf + n, max - n) ) > 0 ) n += k;
	fs_close(fd);
	return n;
}

LOCAL void cat3( char *out, CONST char *a, CONST char *b, CONST char *c )
{
	knl_strcpy(out, a);
	knl_strcat(out, b);
	knl_strcat(out, c);
}

/*
 * Every file of folder a the same as the file of that name in folder b;
 * how many there were, -1 for one that differs or is missing
 */
LOCAL INT same_dirs( CONST char *a, CONST char *b )
{
	T_DIRENT	*de;
	UB		*x, *y;
	char		*p, *q;
	INT		fd, cnt, i, nx, ny, files = 0;

	de = (T_DIRENT *)Kmalloc(sizeof(T_DIRENT) * 4);
	x = (UB *)Kmalloc(65536);
	y = (UB *)Kmalloc(65536);
	p = (char *)Kmalloc(FS_PATH_MAX);
	q = (char *)Kmalloc(FS_PATH_MAX);
	fd = fs_open(a, O_RDONLY | O_DIRECTORY);
	if ( de == NULL || x == NULL || y == NULL || p == NULL || q == NULL || fd < 0 ) {
		files = -1;
	}
	while ( files >= 0 && ( cnt = fs_getdents(fd, de, 4) ) > 0 ) {
		for ( i = 0; i < cnt && files >= 0; i++ ) {
			if ( de[i].name[0] == '.' || ( de[i].mode & FS_IFDIR ) != 0 ) continue;
			cat3(p, a, "/", (CONST char *)de[i].name);
			cat3(q, b, "/", (CONST char *)de[i].name);
			nx = fs_all(p, x, 65536);
			ny = fs_all(q, y, 65536);
			if ( nx < 0 || nx != ny || bytes_cmp(x, y, nx) != 0 ) {
				tm_printf((UB *)"  %s and %s differ (%d, %d)\n", p, q, nx, ny);
				files = -1;
			} else {
				files++;
			}
		}
	}
	if ( fd >= 0 ) fs_close(fd);
	if ( de != NULL ) Kfree(de);
	if ( x != NULL ) Kfree(x);
	if ( y != NULL ) Kfree(y);
	if ( p != NULL ) Kfree(p);
	if ( q != NULL ) Kfree(q);
	return files;
}

/* A folder and the files in it taken away */
LOCAL void rm_dir( CONST char *d )
{
	T_DIRENT	de[2];
	char		*p;
	INT		fd, cnt;

	p = (char *)Kmalloc(FS_PATH_MAX);
	if ( p == NULL ) return;
	for (;;) {
		fd = fs_open(d, O_RDONLY | O_DIRECTORY);
		if ( fd < 0 ) break;
		cnt = fs_getdents(fd, de, 1);
		fs_close(fd);
		while ( cnt > 0 && de[0].name[0] == '.' ) {
			fd = fs_open(d, O_RDONLY | O_DIRECTORY);
			cnt = 0;
			if ( fd < 0 ) break;
			while ( ( cnt = fs_getdents(fd, de, 1) ) > 0 && de[0].name[0] == '.' ) ;
			fs_close(fd);
		}
		if ( cnt <= 0 ) break;
		cat3(p, d, "/", (CONST char *)de[0].name);
		if ( fs_unlink(p) < 0 ) break;
	}
	(void)fs_rmdir(d);
	Kfree(p);
}

/* The UUID after a word of a line */
LOCAL BOOL uuid_after( CONST char *line, CONST char *w, TS_UUID *u )
{
	INT	at = find((CONST UB *)line, s_len(line), w, 0);
	char	s[37];

	if ( at < 0 ) return FALSE;
	at += s_len(w);
	knl_strncpy(s, line + at, 36);
	s[36] = 0;
	return (BOOL)( ts_str_to_uuid(s, u) >= E_OK );
}

LOCAL INT refs( CONST TS_UUID *u )
{
	T_OBREF	r;

	return ( ob_ref_obj(u, &r) >= E_OK ) ? r.refcnt : -1;
}

/* An object and, when nothing else links to them, what it links to, thrown away */
LOCAL void drop( CONST TS_UUID *u, INT depth )
{
	TS_UUID	ids[16];
	UB	*rec;
	SZ	size = 0;
	INT	n = 0, i;
	ER	er;

	rec = om_obj_record(u, 0, &size);
	if ( rec != NULL ) {
		n = om_store_links(rec, size, ids, 16);
		Kfree(rec);
	}
	er = ob_del_obj(u);
	for ( i = 0; er == E_OBJ && i < 8; i++ ) {
		(void)ob_unl_obj(u);
		er = ob_del_obj(u);
	}
	for ( i = 0; i < n && i < 16 && depth > 0; i++ ) {
		if ( refs(&ids[i]) == 0 ) drop(&ids[i], depth - 1);
	}
}

/* The box a 取込 made taken out of ファイル変換の箱 and thrown away with what it holds */
LOCAL void drop_box( CONST TS_UUID *box )
{
	TS_UUID	home;

	if ( ts_str_to_uuid(SYSDEF_XFCONV_BOX, &home) >= E_OK ) (void)om_obj_link_del(&home, box);
	drop(box, 4);
}

/* The accessory started (on an object, or none); its process and window */
LOCAL ID start( CONST TS_UUID *arg, TS_UUID *p_win, INT *p_wid )
{
	TS_UUID	prog, pu;
	T_OBCRE	c;
	UD	pos = log_now();
	INT	i;
	ID	pid;

	*p_wid = -1;
	if ( ts_str_to_uuid(SYSDEF_PROG_XFCONV, &prog) < E_OK ) return E_SYS;
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = prog;
	if ( arg != NULL ) {
		c.arg = arg;
		c.argsz = sizeof(*arg);
	}
	if ( ob_cre_obj(&c, &pu) < E_OK ) return E_OBJ;
	pid = knl_prc_of_uuid(&pu);
	if ( pid <= 0 || !wait_for(pos, "xfconv: ready", 100, NULL, 0) ) return E_TMOUT;
	for ( i = 0; i < 30 && *p_wid <= 0; i++ ) {
		*p_wid = window_of(WIN_NAME, p_win);
		if ( *p_wid <= 0 ) tk_dly_tsk(100);
	}
	return pid;
}

/* The window closed: the process ends with 0 */
LOCAL void finish( ID pid, CONST TS_UUID *win )
{
	T_PSTS	psts;

	(void)ob_del_obj(win);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, 0);
}

LOCAL char	sfname[16];
LOCAL TS_UUID	sub_obj, sub_box;
LOCAL BOOL	have_sub;
LOCAL UD	when_ns = 100000000000ULL;	/* the time a press says it came, in ns */

/* The button pressed (and let go) at (x, y) of a window's work area, with the keys held */
LOCAL void tap( INT wid, INT x, INT y, UINT mods )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.wid = wid;
	ev.x = x;
	ev.y = y;
	ev.mods = mods;
	ev.code = 0;
	when_ns += 1000000000ULL;		/* a second apart: never a double press */
	ev.when = when_ns;
	ev.type = HID_EV_MOVE;
	knl_wmobj_event(&ev);
	ev.type = HID_EV_BTN_DOWN;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(150);
}

/* The button pressed at one place, the pointer moved to another and let go there */
LOCAL void pull( INT wid, INT x0, INT y0, INT x1, INT y1 )
{
	T_WMEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.wid = wid;
	ev.x = x0;
	ev.y = y0;
	when_ns += 1000000000ULL;
	ev.when = when_ns;
	ev.type = HID_EV_BTN_DOWN;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
	ev.x = x1;
	ev.y = y1;
	ev.type = HID_EV_MOVE;
	knl_wmobj_event(&ev);
	tk_dly_tsk(80);
	ev.type = HID_EV_BTN_UP;
	knl_wmobj_event(&ev);
	tk_dly_tsk(150);
}

/* A window of that name, waited for (tenths of a second) */
LOCAL INT wait_window( CONST char *name, TS_UUID *out, INT tries )
{
	INT	i, wid = -1;

	for ( i = 0; i < tries && wid <= 0; i++ ) {
		wid = window_of(name, out);
		if ( wid <= 0 ) tk_dly_tsk(100);
	}
	if ( wid <= 0 ) tm_printf((UB *)"  waited in vain for the window \"%s\"\n", name);
	else tk_dly_tsk(200);			/* and its panel taking keys */
	return wid;
}

/* A panel of the accessory's answered with Enter: its 実行 or 接続 */
LOCAL BOOL answer_panel( CONST char *name )
{
	TS_UUID	u;
	INT	wid = wait_window(name, &u, 50);

	if ( wid <= 0 ) return FALSE;
	press(wid, K_ENTER, 0);
	return TRUE;
}

/* The middle of the 仮身 of row i of a file list, as xfc_draw.c lays them out */
#define ROW_X		40
#define ROW_Y(i)	( 10 + (i) * 32 + 3 + 12 )

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

/* The superfloppy's file list: opened from the main window by typing its name and Enter */
LOCAL INT open_sf( INT wid, TS_UUID *fwin )
{
	char	line[64];
	UD	pos = log_now();

	type(wid, sfname);
	press(wid, K_ENTER, 0);
	if ( !wait_for(pos, "xfconv: mounted /media/", 100, NULL, 0) ) return -1;
	cat3(line, "xfconv: window ", sfname, "");
	if ( !wait_for(pos, line, 50, NULL, 0) ) return -1;
	if ( !wait_for(pos, "xfconv: - SUB/", 100, NULL, 0) ) return -1;
	return wait_window(sfname, fwin, 30);
}

/* A file list closed: the volume it mounted taken off again */
LOCAL BOOL close_sf( INT fwid )
{
	char	line[64];
	UD	pos = log_now();

	ctrl(fwid, 'e');
	cat3(line, "xfconv: unmounted ", sfname, "");
	return wait_for(pos, line, 100, NULL, 0);
}

/*
 * The disks listed; the superfloppy opened: mounted, and its root in a
 * window of its own, the directory and the files told apart; several
 * chosen with Shift and the arrows, with Shift and the button and with
 * a rectangle; two files made 実身 through the panel into a new box;
 * the folder SUB made 実身 and written back beside itself, the two the
 * same; the window closed, the disk taken off
 */
LOCAL void test_disk( void )
{
	TS_UUID	win, fwin, box, inner, ids[4];
	T_OBREF	r;
	char	line[256], dir[64], a[128], b[128];
	UB	*rec;
	SZ	size = 0;
	UD	pos;
	INT	wid, fwid, n, k, rsub, rdata, rhello;
	UD	lpos;
	ID	pid;

	if ( !superfloppy(sfname, sizeof(sfname)) ) KT_SKIP("no superfloppy disk");
	if ( ts_str_to_uuid(SYSDEF_PROG_XFCONV, &win) < E_OK || ob_ref_obj(&win, &r) < E_OK ) {
		KT_SKIP("no ファイル変換 program");
	}
	pos = log_now();
	pid = start(NULL, &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;
	cat3(line, "xfconv: media ", sfname, " ");
	KT_ASSERT(wait_for(pos, line, 10, NULL, 0));		/* the disk listed */
	cat3(dir, FS_MEDIA_DIR, "/", sfname);
	cat3(a, dir, "/", "SUB (2)");
	rm_dir(a);				/* from a run before */

	/* opened: mounted, its root listed in a window of its own */
	pos = log_now();
	lpos = pos;
	fwid = open_sf(wid, &fwin);
	KT_ASSERT(fwid > 0);
	if ( fwid <= 0 ) {
		finish(pid, &win);
		return;
	}
	KT_ASSERT(wait_for(pos, "xfconv: - HELLO.TXT ", 10, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: - DATA.BIN 4608", 10, NULL, 0));

	rsub = row_of(lpos, "SUB");
	rdata = row_of(lpos, "DATA.BIN");
	rhello = row_of(lpos, "HELLO.TXT");
	KT_ASSERT(rsub > 0 && rdata == rsub + 1 && rhello == rdata + 1);

	/* chosen: by name, then the next with Shift and the arrow */
	pos = log_now();
	type(fwid, "data");
	press(fwid, K_DOWN, HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "xfconv: selected 2: DATA.BIN, HELLO.TXT", 30, NULL, 0));

	/* the button with Shift takes one away and puts it back */
	pos = log_now();
	tap(fwid, ROW_X, ROW_Y(rhello), HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: DATA.BIN", 30, NULL, 0));
	pos = log_now();
	tap(fwid, ROW_X, ROW_Y(rhello), HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "xfconv: selected 2: DATA.BIN, HELLO.TXT", 30, NULL, 0));

	/* a rectangle pulled out from clear ground over SUB, DATA.BIN and HELLO.TXT */
	pos = log_now();
	pull(fwid, 300, ROW_Y(rsub) - 4, 20, ROW_Y(rhello) + 2);
	KT_ASSERT(wait_for(pos, "xfconv: selected 3: SUB, DATA.BIN, HELLO.TXT", 30, NULL, 0));

	/* two files made 実身 through the panel: a new box of two links */
	pos = log_now();
	type(fwid, "data");
	press(fwid, K_DOWN, HID_MOD_LSHIFT);
	KT_ASSERT(wait_for(pos, "xfconv: selected 2: DATA.BIN, HELLO.TXT", 30, NULL, 0));
	ctrl(fwid, 'i');
	KT_ASSERT(wait_for(pos, "xfconv: asked how to import", 50, NULL, 0));
	KT_ASSERT(answer_panel("実身への変換"));
	KT_ASSERT(wait_for(pos, "xfconv: imported 2 of 2 objects 2 box ", 300, line, sizeof(line)));
	if ( uuid_after(line, "", &box) ) {
		rec = om_obj_record(&box, 0, &size);
		n = ( rec != NULL ) ? om_store_links(rec, size, ids, 4) : -1;
		KT_ASSERT(rec != NULL && find(rec, (INT)size, "<figure>", 0) >= 0);
		if ( rec != NULL ) Kfree(rec);
		KT_ASSERT_EQ(n, 2);
		for ( k = 0; k < n && k < 4; k++ ) {
			rec = om_obj_record(&ids[k], 1, &size);
			KT_ASSERT(rec != NULL && ( size == 4608 || ( size == 25 && bytes_cmp(rec, "TessronOS FAT test file.\n", 25) == 0 ) ));
			if ( rec != NULL ) Kfree(rec);
		}
		drop_box(&box);
	} else {
		KT_ASSERT(FALSE);
	}

	/* SUB made 実身 */
	pos = log_now();
	type(fwid, "sub");
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: SUB", 30, NULL, 0));
	ctrl(fwid, 'i');
	KT_ASSERT(answer_panel("実身への変換"));
	KT_ASSERT(wait_for(pos, "xfconv: imported 1 of 1 objects ", 300, line, sizeof(line)));
	have_sub = uuid_after(line, "first ", &sub_obj);
	KT_ASSERT(have_sub && uuid_after(line, "box ", &box));
	if ( have_sub ) {
		KT_ASSERT_EQ(refs(&sub_obj), 1);
		rec = om_obj_record(&sub_obj, 0, &size);
		n = ( rec != NULL ) ? om_store_links(rec, size, ids, 4) : -1;
		KT_ASSERT(rec != NULL && find(rec, (INT)size, "<figure>", 0) >= 0);
		if ( rec != NULL ) Kfree(rec);
		KT_ASSERT_EQ(n, 1);
		if ( n == 1 ) {
			inner = ids[0];
			rec = om_obj_record(&inner, 1, &size);
			KT_ASSERT(rec != NULL && size == 11 && bytes_cmp(rec, "inner file\n", 11) == 0);
			if ( rec != NULL ) Kfree(rec);
		}
	}

	/* written back beside itself through the panel, as "SUB (2)": the same bytes, and chosen in the list */
	pos = log_now();
	ctrl(fwid, 'o');
	KT_ASSERT(wait_for(pos, "xfconv: asked how to export", 50, NULL, 0));
	KT_ASSERT(answer_panel("ファイルへの変換"));
	KT_ASSERT(wait_for(pos, "xfconv: exported SUB (2) ", 300, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: - SUB (2)/", 50, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: selected 1: SUB (2)", 50, NULL, 0));
	cat3(b, dir, "/", "SUB");
	KT_ASSERT_EQ(same_dirs(b, a), 1);
	rm_dir(a);

	/* the window closed: the disk taken off */
	KT_ASSERT(close_sf(fwid));
	KT_ASSERT(fs_open(b, O_RDONLY | O_DIRECTORY) < 0);
	finish(pid, &win);
	sub_box = box;
	if ( !have_sub ) drop_box(&box);
}

/* Whether a name is a file of a TADjs set: {uuid}... */
LOCAL BOOL set_file( CONST UB *n )
{
	TS_UUID	u;
	char	s[37];

	if ( s_len((CONST char *)n) < 40 ) return FALSE;
	knl_strncpy(s, (CONST char *)n, 36);
	s[36] = 0;
	return (BOOL)( ts_str_to_uuid(s, &u) >= E_OK );
}

/* The files of TADjs sets in the root of a folder: how many, taken away */
LOCAL INT rm_sets( CONST char *d )
{
	T_DIRENT	de[1];
	char		*p;
	INT		fd, n = 0, gone;

	p = (char *)Kmalloc(FS_PATH_MAX);
	if ( p == NULL ) return -1;
	do {
		gone = 0;
		fd = fs_open(d, O_RDONLY | O_DIRECTORY);
		if ( fd < 0 ) break;
		while ( fs_getdents(fd, de, 1) > 0 ) {
			if ( set_file(de[0].name) ) {
				cat3(p, d, "/", (CONST char *)de[0].name);
				break;
			}
			de[0].name[0] = 0;
		}
		fs_close(fd);
		if ( de[0].name[0] != 0 && set_file(de[0].name) && fs_unlink(p) >= 0 ) {
			n++;
			gone = 1;
		}
	} while ( gone );
	Kfree(p);
	return n;
}

/*
 * Started on the object taken in: written as TADjs file sets (the
 * panel's choice when Ctrl+T asked for them), the object and the one
 * it links to, onto the disk mounted again
 */
LOCAL void test_arg( void )
{
	TS_UUID	win, fwin;
	char	us[40], line[256], dir[64], j[128];
	UD	pos;
	INT	wid, fwid;
	ID	pid;

	if ( !have_sub ) KT_SKIP("nothing taken in");
	pid = start(&sub_obj, &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) {
		drop_box(&sub_box);
		return;
	}
	cat3(dir, FS_MEDIA_DIR, "/", sfname);
	(void)ts_uuid_to_str(&sub_obj, us, sizeof(us));

	fwid = open_sf(wid, &fwin);
	KT_ASSERT(fwid > 0);
	if ( fwid > 0 ) {
		pos = log_now();
		ctrl(fwid, 't');
		ctrl(fwid, 'o');
		KT_ASSERT(answer_panel("ファイルへの変換"));
		KT_ASSERT(wait_for(pos, "xfconv: export as 5 ", 50, NULL, 0));
		cat3(line, "xfconv: exported ", us, " ");
		KT_ASSERT(wait_for(pos, line, 300, NULL, 0));
		cat3(j, dir, "/", us);
		knl_strcat(j, ".json");
		{
			T_FSTAT	st;

			KT_ASSERT(fs_stat(j, &st) >= EX_OK && st.size > 0);
		}
		/* the set of SUB (its metadata and record 0) and of INNER (and its record 1) */
		KT_ASSERT(rm_sets(dir) >= 5);
		KT_ASSERT(close_sf(fwid));
	}
	finish(pid, &win);
	drop_box(&sub_box);
	KT_ASSERT_EQ(refs(&sub_obj), -1);		/* gone with its box */
}

#ifdef KT_FTP_PORT
#define STR2(x)	#x
#define STR(x)	STR2(x)

/* The FTP panel opened from the main window and filled in: the host, and the port the test server listens on */
LOCAL INT ftp_panel( INT wid )
{
	TS_UUID	u;
	INT	dwid;

	ctrl(wid, 'f');
	dwid = wait_window("FTP 接続", &u, 50);
	if ( dwid <= 0 ) return -1;
	type(dwid, "10.0.2.2");
	press(dwid, K_TAB, 0);
	press(dwid, K_BS, 0);
	press(dwid, K_BS, 0);
	type(dwid, STR(KT_FTP_PORT));
	return dwid;
}

/*
 * The FTP server on the host (10.0.2.2), through the panel: SSH asked
 * for and refused as not there yet; connected by FTP, a file taken in,
 * written back under a new name, the connection kept in the settings
 * without the password
 */
LOCAL void test_ftp( void )
{
	TS_UUID	win, box, u, prog, fwin;
	char	line[256], want[64];
	UB	*rec, *meta;
	SZ	size = 0;
	UD	pos;
	INT	wid, dwid, fwid, i;
	ID	pid;

	pid = start(NULL, &win, &wid);
	KT_ASSERT(pid > 0 && wid > 0);
	if ( pid <= 0 || wid <= 0 ) return;

	/* SSH: said not to be there yet; the panel stays, and is put away with Esc */
	pos = log_now();
	dwid = ftp_panel(wid);
	KT_ASSERT(dwid > 0);
	if ( dwid > 0 ) {
		tap(dwid, 300, 65, 0);			/* SSH を使う */
		press(dwid, K_ENTER, 0);
		KT_ASSERT(wait_for(pos, "xfconv: ssh not supported", 50, NULL, 0));
		press(dwid, K_ESC, 0);
		KT_ASSERT(wait_for(pos, "xfconv: ftp cancelled", 50, NULL, 0));
	}

	pos = log_now();
	dwid = ftp_panel(wid);
	KT_ASSERT(dwid > 0);
	if ( dwid <= 0 ) {
		finish(pid, &win);
		return;
	}
	press(dwid, K_ENTER, 0);
	KT_ASSERT(wait_for(pos, "xfconv: connected 10.0.2.2", 300, NULL, 0));
	KT_ASSERT(wait_for(pos, "xfconv: - KTEST.TXT 24", 100, NULL, 0));
	fwid = wait_window("ftp://10.0.2.2", &fwin, 30);
	KT_ASSERT(fwid > 0);
	if ( fwid <= 0 ) {
		finish(pid, &win);
		return;
	}

	/* taken in */
	pos = log_now();
	type(fwid, "ktest.txt");
	ctrl(fwid, 'i');
	KT_ASSERT(answer_panel("実身への変換"));
	KT_ASSERT(wait_for(pos, "xfconv: imported 1 of 1 objects ", 300, line, sizeof(line)));
	KT_ASSERT(uuid_after(line, "first ", &u) && uuid_after(line, "box ", &box));
	rec = om_obj_record(&u, 1, &size);
	KT_ASSERT(rec != NULL && size == 24 && bytes_cmp(rec, "TessronOS FTP test file\n", 24) == 0);
	if ( rec != NULL ) Kfree(rec);

	/* written back: a new name beside it, the same size */
	pos = log_now();
	ctrl(fwid, 'o');
	KT_ASSERT(answer_panel("ファイルへの変換"));
	KT_ASSERT(wait_for(pos, "xfconv: exported KTEST (", 300, line, sizeof(line)));
	for ( i = 0; line[i] != 0 && line[i] != ')'; i++ ) ;
	line[i] = 0;
	cat3(want, "xfconv: - KTEST (", line, ").TXT 24");
	KT_ASSERT(wait_for(pos, want, 100, NULL, 0));

	/* kept: the host, not the password */
	(void)ts_str_to_uuid(SYSDEF_PROG_XFCONV, &prog);
	meta = om_obj_meta(&prog, &size);
	KT_ASSERT(meta != NULL && find(meta, (INT)size, "\"host\":\"10.0.2.2\"", 0) >= 0
		  && find(meta, (INT)size, "\"pass\"", 0) < 0);
	if ( meta != NULL ) Kfree(meta);

	pos = log_now();
	ctrl(fwid, 'e');
	KT_ASSERT(wait_for(pos, "xfconv: disconnected ftp://10.0.2.2", 50, NULL, 0));
	drop_box(&box);

#ifdef KT_FTP_ACT_PORT
	/*
	 * The active way: パッシブ switched off in the panel and the port
	 * the forward of the test network comes in by given; the server
	 * connects into this machine for the listing and the file
	 */
	pos = log_now();
	dwid = ftp_panel(wid);
	KT_ASSERT(dwid > 0);
	if ( dwid > 0 ) {
		tap(dwid, 200, 202, 0);			/* パッシブ off */
		for ( i = 0; i < 3; i++ ) press(dwid, K_TAB, 0);	/* the port, the user, the password, the active port */
		type(dwid, STR(KT_FTP_ACT_PORT));
		press(dwid, K_ENTER, 0);
		{
			SYSTIM	t0, t1;

			(void)tk_get_otm(&t0);
			KT_ASSERT(wait_for(pos, "xfconv: connected 10.0.2.2 utf-8 active", 900, NULL, 0));
			(void)tk_get_otm(&t1);
			tm_printf((UB *)"  connected in %d ms\n", (INT)( t1.lo - t0.lo ));
		}
		KT_ASSERT(wait_for(pos, "xfconv: - KTEST.TXT 24", 100, NULL, 0));
		fwid = wait_window("ftp://10.0.2.2", &fwin, 30);
		KT_ASSERT(fwid > 0);
		if ( fwid > 0 ) {
			pos = log_now();
			type(fwid, "ktest.txt");
			ctrl(fwid, 'i');
			KT_ASSERT(answer_panel("実身への変換"));
			KT_ASSERT(wait_for(pos, "xfconv: imported 1 of 1 objects ", 300, line, sizeof(line)));
			KT_ASSERT(uuid_after(line, "first ", &u) && uuid_after(line, "box ", &box));
			rec = om_obj_record(&u, 1, &size);
			KT_ASSERT(rec != NULL && size == 24 && bytes_cmp(rec, "TessronOS FTP test file\n", 24) == 0);
			if ( rec != NULL ) Kfree(rec);
			meta = om_obj_meta(&prog, &size);
			KT_ASSERT(meta != NULL && find(meta, (INT)size, "\"data\":\"active\"", 0) >= 0);
			if ( meta != NULL ) Kfree(meta);
			pos = log_now();
			ctrl(fwid, 'e');
			KT_ASSERT(wait_for(pos, "xfconv: disconnected ", 50, NULL, 0));
			drop_box(&box);
		}
	}
#endif
	finish(pid, &win);
}
#endif

EXPORT void ktest_xfc( void )
{
	logb = (UB *)Kmalloc(LOG_MAX);
	if ( logb == NULL ) return;
	KT_RUN(test_disk);
	KT_RUN(test_arg);
#ifdef KT_FTP_PORT
	KT_RUN(test_ftp);
#endif
	Kfree(logb);
}
