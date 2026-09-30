/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfc_main.c
 *	ファイル変換: the accessory (design 17.17)
 *
 *	The main window lists the media as 仮身: every disk the device
 *	manager has (the partitions among them), mounted or not, then
 *	ネットワーク(FTP) and the FTP connections kept. Opening a disk (a
 *	double click, or Enter) mounts it when it is not and opens a window
 *	of its own that lists the root of the volume as 仮身 coloured by
 *	what they are (xfc_draw.c); a double click on a directory goes into
 *	it. Opening ネットワーク(FTP), or a connection kept, asks for the
 *	server in a panel, and 接続 opens the same kind of window on it.
 *	Several may be open at once; closing one takes its volume off again
 *	when it was this window that mounted it.
 *
 *	In a file list the 仮身 are chosen by a click, added to with Shift,
 *	or taken in a rectangle pulled out over them. Carried out of the
 *	window, the desktop moves their outline with the pointer (a carry,
 *	include/ts/dtreq.h); let go over a window of the desktop that takes
 *	links, a panel asks how the files are to be made 実身, they are
 *	made there, on that window's volume, and a link to each is put
 *	where they were let go. Let go anywhere else, nothing happens.
 *
 *	仮身 dropped from the desktop on a file list are written into the
 *	directory it shows (or the directory let go on) after a panel has
 *	asked how, and the list shows what was written.
 *
 *	The keyboard does the same without the pointer: the arrows move
 *	(with Shift they choose a run), Space chooses, Enter opens,
 *	Backspace goes up, a letter goes to the next name that starts with
 *	what was typed, Ctrl+I makes what is chosen 実身 in a new box of
 *	ファイル変換の箱 (or in the figure the accessory was started on),
 *	Ctrl+O writes the object the accessory was started on (or what was
 *	taken in last) into the directory shown, Ctrl+M mounts or takes off
 *	the disk chosen in the main window.
 *
 *	What it did is said on the console too ("xfconv: ..."), which is
 *	what the tests follow.
 */

#include "xfc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/json.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/dt.h>
#include <ts/time.h>
#include <ts/hid.h>

#define TITLE_H		34
#define FRAME_W		16
#define MEDIA_W		380
#define MEDIA_H		360
#define FILES_W		560
#define FILES_H		430
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */

/* Keys: the keyboard's usages */
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_BS		0x2A
#define K_SPACE		0x2C
#define K_HOME		0x4A
#define K_PGUP		0x4B
#define K_END		0x4D
#define K_PGDN		0x4E
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define K_KPENTER	0x58
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* ---------------------------------------------------------------- state */

static XWIN	xw[XFC_WIN_MAX];
static XWIN	*media;			/* the main window: xw[0] */
static TS_UUID	ch;
static ID	port;
static ID	klist;			/* the list of the devices, watched */
static BOOL	closing;

static XFCCONN	conn[XFC_CONN_MAX];
static INT	nconn;

static BOOL	o_tadjs, o_layout, o_sjis, o_ro, o_argbox;
static TS_UUID	argobj;
static BOOL	hasarg, argfig;
static char	argname[OB_NAME_MAX];
static TS_UUID	lastbox, lasttop;
static BOOL	haslast;
static char	lastname[OB_NAME_MAX];

/* a job under way */
static BOOL	busy, cancel;
static XWIN	*jobw;
static INT	done_n;
static char	job_what[64];

static void rescan( XWIN *w );
static void event( CONST T_OBNTM *m );

static BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

/* ---------------------------------------------------------------- saying things */

EXPORT void xfc_log( CONST char *s )
{
	char	line[600];

	snprintf(line, sizeof(line), "xfconv: %s\n", s);
	tm_putstring((CONST UB *)line);
}

/* A line at the foot of the window, on the message line and the console */
static void say( XWIN *w, CONST char *s, BOOL err )
{
	if ( w == NULL ) w = media;
	strncpy(w->status, s, sizeof(w->status) - 1);
	w->status[sizeof(w->status) - 1] = 0;
	w->status_err = err;
	(void)wm_msg_put(s);
	xfc_log(s);
	w->dirty = TRUE;
}

static CONST char *err_text( ER er )
{
	switch ( er ) {
	case E_NOEXS:	return "ありません";
	case E_OACV:	return "許されていません";
	case E_NOMEM:	return "メモリが足りません";
	case E_LIMIT:	return "大きすぎるか、場所がありません";
	case E_TMOUT:	return "応答がありません";
	case E_ABORT:	return "中止しました";
	case E_IO:	return "読み書きできません";
	case E_BUSY:	return "使用中です";
	case E_NOSPT:	return "扱えません";
	default:	return "失敗しました";
	}
}

static void paint_all( void )
{
	INT	i;

	for ( i = 0; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used && xw[i].dirty ) xfc_paint(&xw[i]);
	}
}

/* ---------------------------------------------------------------- windows */

static XWIN *win_of( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used && same_uuid(&xw[i].win, u) ) return &xw[i];
	}
	return NULL;
}

static XWIN *win_free( void )
{
	INT	i;

	for ( i = 1; i < XFC_WIN_MAX; i++ ) {
		if ( !xw[i].used ) return &xw[i];
	}
	return NULL;
}

/* The window's object made, its drawing, and its notices at the accessory's channel */
static ER win_open( XWIN *w, CONST char *title, INT x, INT y, INT ww, INT wh )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[192];
	TS_UUID		prog;

	if ( w->ent == NULL ) w->ent = (XFCENT *)malloc(sizeof(XFCENT) * XFC_ENT_MAX);
	if ( w->ent == NULL ) return E_NOMEM;
	if ( xfc_uuid_parse(SYSDEF_PROG_XFCONV, &prog) ) {
		char	ps[40];

		xfc_uuid_str(&prog, ps);
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7,\"icon\":\"%s\"}",
			 x, y, x + ww + FRAME_W, y + wh + TITLE_H, ps);
	} else {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7}", x, y, x + ww + FRAME_W, y + wh + TITLE_H);
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &w->win) < E_OK ) return E_OBJ;
	w->kw = ob_opn_obj(&w->win, OB_OP_ALL);
	w->gid = ( w->kw > 0 ) ? wm_obj_gid(w->kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
		   | OB_E_DELETE | OB_E_DROP | OB_E_WHEEL;
	if ( w->kw <= 0 || w->gid < 0 || ob_ntf_evt(w->kw, OB_REC_ANY, &req, port) <= 0 ) {
		if ( w->kw > 0 ) ob_cls_obj(w->kw);
		(void)ob_del_obj(&w->win);
		w->kw = 0;
		return E_OBJ;
	}
	/* the work area the size asked for, whatever the frame takes */
	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + ww + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + wh + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	w->used = TRUE;
	w->dirty = TRUE;
	w->press_row = -1;
	return E_OK;
}

static void win_raise( XWIN *w )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.z = 0;
		wp.flags |= OB_WP_SHOWN;
		(void)ob_wri_rec(w->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
}

/* Where a new file list goes: stepped from the main window */
static void place_new( INT *x, INT *y )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	INT		i, n = 0;

	for ( i = 1; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used ) n++;
	}
	*x = 160 + n * 26;
	*y = 110 + n * 26;
	if ( media->kw > 0 && ob_rea_rec(media->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		*x = wp.left + 60 + n * 26;
		*y = wp.top + 40 + n * 26;
		if ( wp.sw > 0 && *x + FILES_W + FRAME_W > wp.sw ) *x = wp.sw - FILES_W - FRAME_W;
		if ( wp.sh > 0 && *y + FILES_H + TITLE_H > wp.sh ) *y = wp.sh - FILES_H - TITLE_H;
		if ( *x < 0 ) *x = 0;
		if ( *y < 0 ) *y = 0;
	}
}

/* ---------------------------------------------------------------- lists */

static INT ent_cmp( CONST void *a, CONST void *b )
{
	CONST XFCENT	*x = (CONST XFCENT *)a, *y = (CONST XFCENT *)b;

	if ( x->kind != y->kind ) return (INT)x->kind - (INT)y->kind;
	return strcmp((CONST char *)x->name, (CONST char *)y->name);
}

static XFCENT *ent_add( XWIN *w, UINT kind, CONST UB *name )
{
	XFCENT	*e;

	if ( w->nent >= XFC_ENT_MAX ) return NULL;
	e = &w->ent[w->nent++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->size = -1;
	strncpy((char *)e->name, (CONST char *)name, sizeof(e->name) - 1);
	e->hidden = (BOOL)( name[0] == '.' );
	return e;
}

static void keep_cur( XWIN *w )
{
	INT	rows = xfc_rows(w);

	if ( w->cur >= w->nent ) w->cur = w->nent - 1;
	if ( w->cur < 0 ) w->cur = 0;
	if ( w->cur < w->top ) w->top = w->cur;
	if ( w->cur >= w->top + rows ) w->top = w->cur - rows + 1;
	if ( w->top > w->nent - rows ) w->top = w->nent - rows;
	if ( w->top < 0 ) w->top = 0;
}

static INT marked( XWIN *w )
{
	INT	i, n = 0;

	for ( i = 0; i < w->nent; i++ ) {
		if ( w->ent[i].marked ) n++;
	}
	return n;
}

/* What is chosen, said at the foot and on the console */
static void tell_chosen( XWIN *w )
{
	char	s[400];
	INT	i, n = 0, at;

	at = snprintf(s, sizeof(s), "selected %d:", marked(w));
	for ( i = 0; i < w->nent && at < (INT)sizeof(s) - 40; i++ ) {
		if ( !w->ent[i].marked ) continue;
		at += snprintf(s + at, sizeof(s) - at, "%s %s", ( n++ > 0 ) ? "," : "", w->ent[i].name);
	}
	xfc_log(s);
	if ( w->kind == W_FILES ) {
		if ( n == 0 ) snprintf(w->status, sizeof(w->status), "%s%s%s", w->label, w->dir[0] != 0 ? "/" : "", w->dir);
		else snprintf(w->status, sizeof(w->status), "%d 個を選択（運ぶと実身にします）", n);
		w->status_err = FALSE;
	}
	w->dirty = TRUE;
}

static void choose_none( XWIN *w )
{
	INT	i;

	for ( i = 0; i < w->nent; i++ ) w->ent[i].marked = FALSE;
}

/* Only entry i chosen */
static void choose_one( XWIN *w, INT i )
{
	choose_none(w);
	if ( i >= 0 && i < w->nent ) w->ent[i].marked = TRUE;
	w->cur = w->anchor = i;
	keep_cur(w);
}

/* The run from the anchor to i chosen */
static void choose_run( XWIN *w, INT i )
{
	INT	a = w->anchor, k;

	if ( a < 0 || a >= w->nent ) a = i;
	choose_none(w);
	for ( k = ( a < i ) ? a : i; k <= ( ( a < i ) ? i : a ); k++ ) {
		if ( w->ent[k].kind == E_DIR || w->ent[k].kind == E_FILE || w->kind == W_MEDIA ) w->ent[k].marked = TRUE;
	}
	w->cur = i;
	keep_cur(w);
}

/* Where a disk is mounted, from its attributes (tessronos.mount) */
static void disk_mount( ID key, XFCENT *e )
{
	T_JSON	root, tf, m;
	UB	*a;
	SZ	len = 0;

	e->mount[0] = 0;
	e->ro = FALSE;
	a = (UB *)malloc(OB_ATR_MAX + 1);
	if ( a == NULL ) return;
	if ( ob_get_atr(key, a, OB_ATR_MAX, &len) >= E_OK && js_parse(a, (INT)len, &root) >= E_OK
	  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "mount", &m) >= E_OK ) {
		(void)js_get_str(&m, "path", e->mount, sizeof(e->mount));
		e->ro = js_get_bool(&m, "readonly", FALSE);
	}
	free(a);
}

/*
 * Every disk watched with one request on the list of the devices: a
 * disk that comes or goes, a medium put in or taken out, a volume
 * mounted or taken off, each told naming the disk.
 */
static void watch_devices( void )
{
	CONST TS_UUID	devlist = OB_UUID_DEVLIST_INIT;
	T_OBNTF		req;

	klist = ob_opn_obj(&devlist, OB_OP_ATRRD);
	if ( klist <= 0 ) return;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE | OB_E_ATTACH | OB_E_DETACH | OB_E_DELETE;
	req.id = 1;
	if ( ob_ntf_evt(klist, OB_REC_ANY, &req, port) <= 0 ) xfc_log("no notices of the devices");
}

/* The name a connection kept is shown by */
static void conn_label( CONST XFCCONN *c, UB *out, INT max )
{
	if ( c->name[0] != 0 ) snprintf((char *)out, max, "%s", c->name);
	else snprintf((char *)out, max, "%s://%s%s%s", c->ssh ? "sftp" : "ftp", c->user, c->user[0] != 0 ? "@" : "",
		      c->host);
}

static void media_scan( XWIN *w )
{
	TS_UUID	*list;
	T_OBREF	r;
	XFCENT	*e;
	INT	cnt = 0, i, n0;
	UB	nm[XFC_FIELD + 32];
	ID	key;
	char	s[160], sz[40];
	UINT	sum = 0;

	w->nent = 0;
	list = (TS_UUID *)malloc(sizeof(TS_UUID) * 64);
	if ( list != NULL && ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, list, 64, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 64; i++ ) {
			if ( ob_ref_obj(&list[i], &r) < E_OK || r.sub != OB_S_DISK ) continue;
			e = ent_add(w, E_DISK, r.name);
			if ( e == NULL ) break;
			e->uuid = list[i];
			e->size = (D)r.size;
			key = ob_opn_obj(&list[i], OB_OP_ATRRD);
			if ( key > 0 ) {
				T_OBREC	rc[2];
				INT	nr = 0;

				disk_mount(key, e);
				if ( ob_lst_rec(key, rc, 2, &nr) >= E_OK && nr >= 2 ) e->size = (D)rc[1].size;
				ob_cls_obj(key);
			}
			e->faint = (BOOL)( e->size == 0 );
		}
	}
	free(list);
	n0 = w->nent;
	qsort(w->ent, n0, sizeof(XFCENT), ent_cmp);
	(void)ent_add(w, E_NEWFTP, (CONST UB *)"ネットワーク(FTP)");
	for ( i = 0; i < nconn; i++ ) {
		conn_label(&conn[i], nm, sizeof(nm));
		e = ent_add(w, E_FTP, nm);
		if ( e != NULL ) e->conn = i;
	}
	keep_cur(w);

	/* said, and drawn, when it is not what it was */
	for ( i = 0; i < w->nent; i++ ) {
		sum = xfu_crc32(sum, w->ent[i].name, strlen((char *)w->ent[i].name) + 1);
		sum = xfu_crc32(sum, w->ent[i].mount, strlen((char *)w->ent[i].mount) + 1);
		sum = xfu_crc32(sum, (CONST UB *)&w->ent[i].ro, sizeof(w->ent[i].ro));
	}
	if ( sum == w->sum ) return;
	w->sum = sum;
	w->dirty = TRUE;
	for ( i = 0; i < n0; i++ ) {
		xfc_size_text(w->ent[i].size, sz, sizeof(sz));
		snprintf(s, sizeof(s), "media %s %s %s%s", w->ent[i].name, sz,
			 w->ent[i].mount[0] != 0 ? (CONST char *)w->ent[i].mount : "-", w->ent[i].ro ? " ro" : "");
		xfc_log(s);
	}
}

/* The names of the directory shown */
static INT list_put( void *arg, CONST T_XFUENT *x )
{
	XWIN	*w = (XWIN *)arg;
	XFCENT	*e;

	e = ent_add(w, ( x->kind == XFU_K_DIR ) ? E_DIR : E_FILE, x->name);
	if ( e == NULL ) return -1;
	e->size = x->size;
	e->mtime = x->mtime;
	return 0;
}

/* The directory listed again; the names in keep (NUL between, NUL NUL at the end) chosen */
static ER files_scan( XWIN *w, CONST UB *keep )
{
	char	s[XFU_PATH_MAX + 64];
	INT	i, n0;
	ER	er;
	CONST UB *k;

	w->nent = 0;
	(void)ent_add(w, E_VOL, w->dev);
	if ( w->dir[0] != 0 ) (void)ent_add(w, E_UP, (CONST UB *)"..");
	n0 = w->nent;
	er = w->tree.list(w->tree.ctx, w->dir, list_put, w);
	qsort(w->ent + n0, w->nent - n0, sizeof(XFCENT), ent_cmp);
	for ( k = keep; k != NULL && *k != 0; k += strlen((CONST char *)k) + 1 ) {
		for ( i = n0; i < w->nent; i++ ) {
			if ( strcmp((CONST char *)w->ent[i].name, (CONST char *)k) == 0 ) {
				w->ent[i].marked = TRUE;
				w->cur = i;
			}
		}
	}
	keep_cur(w);
	w->dirty = TRUE;
	snprintf(s, sizeof(s), "dir %s/%s %d entries", w->label, w->dir, w->nent - n0);
	xfc_log(s);
	for ( i = n0; i < w->nent && i < n0 + 64; i++ ) {
		snprintf(s, sizeof(s), "- %s%s %d", w->ent[i].name, ( w->ent[i].kind == E_DIR ) ? "/" : "",
			 (INT)w->ent[i].size);
		xfc_log(s);
	}
	if ( er >= E_OK ) {
		snprintf(w->status, sizeof(w->status), "%s%s%s（%d 個）", w->label, w->dir[0] != 0 ? "/" : "",
			 w->dir, w->nent - n0);
		w->status_err = FALSE;
	}
	return er;
}

static void rescan( XWIN *w )
{
	if ( w->kind == W_MEDIA ) {
		w->sum = 0;
		media_scan(w);
	} else if ( files_scan(w, NULL) < E_OK ) {
		say(w, "一覧を読めません", TRUE);
	}
}

/* ---------------------------------------------------------------- directories */

/* The directory shown remembered, the latest first, each once */
static void hist_put( XWIN *w )
{
	INT	i;

	if ( w->hist == NULL ) return;
	for ( i = 0; i < w->nhist; i++ ) {
		if ( strcmp((char *)w->hist[i], (char *)w->dir) == 0 ) break;
	}
	if ( i == w->nhist && w->nhist < XFC_HIST ) w->nhist++;
	if ( i >= XFC_HIST ) i = XFC_HIST - 1;
	memmove(w->hist[1], w->hist[0], (size_t)i * XFU_PATH_MAX);
	strcpy((char *)w->hist[0], (char *)w->dir);
}

static void go_to( XWIN *w, CONST UB *dir )
{
	UB	was[XFU_PATH_MAX];

	strcpy((char *)was, (char *)w->dir);
	snprintf((char *)w->dir, sizeof(w->dir), "%s", dir);
	w->cur = 0;
	w->top = 0;
	w->anchor = -1;
	if ( files_scan(w, NULL) < E_OK ) {
		say(w, "ディレクトリを読めません", TRUE);
		strcpy((char *)w->dir, (char *)was);
		(void)files_scan(w, NULL);
		return;
	}
	hist_put(w);
}

static void go_up( XWIN *w )
{
	UB	d[XFU_PATH_MAX];
	INT	i, cut = 0;

	if ( w->kind != W_FILES || w->dir[0] == 0 ) return;
	strcpy((char *)d, (char *)w->dir);
	for ( i = 0; d[i] != 0; i++ ) {
		if ( d[i] == '/' ) cut = i;
	}
	d[cut] = 0;
	go_to(w, d);
}

/* dir, '/', name into out */
static ER path_join( UB *out, INT max, CONST UB *dir, CONST UB *name )
{
	INT	n = snprintf((char *)out, max, "%s%s%s", dir, dir[0] != 0 ? "/" : "", name);

	return ( n < max ) ? E_OK : E_LIMIT;
}

/* ---------------------------------------------------------------- media opened */

static XWIN *files_new( CONST char *title )
{
	XWIN	*w = win_free();
	INT	x, y;

	if ( w == NULL ) {
		say(media, "これ以上窓を開けません", TRUE);
		return NULL;
	}
	memset(w, 0, sizeof(*w));
	w->kind = W_FILES;
	w->fsx = (T_XFUFS *)malloc(sizeof(T_XFUFS));
	w->hist = (UB (*)[XFU_PATH_MAX])malloc((size_t)XFC_HIST * XFU_PATH_MAX);
	place_new(&x, &y);
	if ( w->fsx == NULL || w->hist == NULL || win_open(w, title, x, y, FILES_W, FILES_H) < E_OK ) {
		free(w->fsx);
		free(w->hist);
		free(w->ent);
		memset(w, 0, sizeof(*w));
		say(media, "窓を開けません", TRUE);
		return NULL;
	}
	w->anchor = -1;
	return w;
}

static void files_free( XWIN *w )
{
	if ( w->kw > 0 ) ob_cls_obj(w->kw);
	(void)ob_del_obj(&w->win);
	free(w->ent);
	free(w->fsx);
	free(w->ftx);
	free(w->ftp);
	free(w->hist);
	memset(w, 0, sizeof(*w));
}

/* A file list closed: its server let go, its volume taken off when this window mounted it */
static void files_close( XWIN *w )
{
	char	s[200];
	ID	key;
	ER	er;

	if ( w->is_ftp && w->ftp != NULL ) {
		(void)ftp_close(w->ftp);
		snprintf(s, sizeof(s), "disconnected %s", w->label);
		xfc_log(s);
	}
	if ( w->mounted ) {
		key = ob_opn_obj(&w->disk, OB_OP_R | ( w->ro ? 0 : OB_OP_WRITE ));
		er = ( key > 0 ) ? fs_detach_dev(key) : (ER)key;
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < 0 ) {
			snprintf(s, sizeof(s), "%s を取り外せません（%d）", w->dev, (INT)er);
			say(media, s, TRUE);
		} else {
			snprintf(s, sizeof(s), "unmounted %s", w->dev);
			xfc_log(s);
			snprintf(s, sizeof(s), w->removable ? "%s を取り外しました。媒体を取り出せます"
				 : "%s を取り外しました", w->dev);
			say(media, s, FALSE);
		}
	}
	files_free(w);
	if ( media != NULL && media->used ) {
		media->sum = 0;
		media_scan(media);
	}
}

static XWIN *files_of_disk( CONST TS_UUID *disk )
{
	INT	i;

	for ( i = 1; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used && !xw[i].is_ftp && same_uuid(&xw[i].disk, disk) ) return &xw[i];
	}
	return NULL;
}

/* The volume of a disk, mounted when it is not, shown in a window of its own from its root */
static void open_disk( XFCENT *e )
{
	XWIN	*w;
	char	s[200];
	BOOL	mounted = FALSE;
	ID	key;
	ER	er;

	w = files_of_disk(&e->uuid);
	if ( w != NULL ) {
		win_raise(w);
		return;
	}
	if ( e->faint ) {
		snprintf(s, sizeof(s), "%s に媒体が入っていません", e->name);
		say(media, s, TRUE);
		return;
	}
	if ( e->mount[0] == 0 ) {
		key = ob_opn_obj(&e->uuid, OB_OP_R | ( o_ro ? 0 : OB_OP_WRITE ));
		if ( key <= 0 ) {
			snprintf(s, sizeof(s), "%s を開けません：%s", e->name, err_text((ER)key));
			say(media, s, TRUE);
			return;
		}
		er = fs_attach_dev(key, "fatfs", o_ro ? FS_MNT_RDONLY : 0);
		ob_cls_obj(key);
		if ( er < 0 ) {
			snprintf(s, sizeof(s), "%s をマウントできません（%d）", e->name, (INT)er);
			say(media, s, TRUE);
			return;
		}
		snprintf((char *)e->mount, sizeof(e->mount), "%s/%s", FS_MEDIA_DIR, e->name);
		e->ro = o_ro;
		mounted = TRUE;
		snprintf(s, sizeof(s), "mounted %s%s", e->mount, o_ro ? " ro" : "");
		xfc_log(s);
	}
	w = files_new((CONST char *)e->name);
	if ( w == NULL ) return;
	w->disk = e->uuid;
	w->mounted = mounted;
	w->ro = e->ro;
	w->removable = (BOOL)( e->name[0] == 'u' || e->name[0] == 's' || e->name[0] == 'm' );
	snprintf((char *)w->dev, sizeof(w->dev), "%s", e->name);
	snprintf(w->label, sizeof(w->label), "%s", e->mount);
	snprintf(w->source, sizeof(w->source), "%s", ( e->name[0] == 'u' ) ? "usb"
		 : ( e->name[0] == 's' || e->name[0] == 'm' ) ? "sd" : "fat");
	if ( xfu_fs_tree(w->fsx, (CONST char *)e->mount, &w->tree) < E_OK ) {
		say(media, "ディレクトリを開けません", TRUE);
		files_close(w);
		return;
	}
	snprintf(s, sizeof(s), "window %s", w->dev);
	xfc_log(s);
	go_to(w, (CONST UB *)"");
	snprintf(s, sizeof(s), "%s を開きました", w->label);
	say(media, s, FALSE);
	media->sum = 0;
	media_scan(media);
}

/* The mount of the disk under the cursor taken off, or made */
static void mount_toggle( void )
{
	XFCENT	*e;
	XWIN	*w;
	char	s[200];
	ID	key;
	ER	er;

	if ( media->cur >= media->nent || media->ent[media->cur].kind != E_DISK ) {
		say(media, "マウントするディスクを選んでください", TRUE);
		return;
	}
	e = &media->ent[media->cur];
	w = files_of_disk(&e->uuid);
	if ( w != NULL && w->mounted ) {
		files_close(w);			/* its window goes, and the volume with it */
		return;
	}
	if ( w != NULL ) files_free(w);
	if ( e->mount[0] == 0 ) {
		key = ob_opn_obj(&e->uuid, OB_OP_R | ( o_ro ? 0 : OB_OP_WRITE ));
		er = ( key > 0 ) ? fs_attach_dev(key, "fatfs", o_ro ? FS_MNT_RDONLY : 0) : (ER)key;
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < 0 ) {
			snprintf(s, sizeof(s), "%s をマウントできません（%d）", e->name, (INT)er);
			say(media, s, TRUE);
		} else {
			snprintf(s, sizeof(s), "mounted %s/%s%s", FS_MEDIA_DIR, e->name, o_ro ? " ro" : "");
			xfc_log(s);
			snprintf(s, sizeof(s), "%s をマウントしました", e->name);
			say(media, s, FALSE);
		}
	} else {
		key = ob_opn_obj(&e->uuid, OB_OP_R | ( e->ro ? 0 : OB_OP_WRITE ));
		er = ( key > 0 ) ? fs_detach_dev(key) : (ER)key;
		if ( key > 0 ) ob_cls_obj(key);
		if ( er < 0 ) {
			snprintf(s, sizeof(s), "%s を取り外せません（%d）%s", e->name, (INT)er,
				 ( er == EX_ACCES ) ? "：システムがマウントしたものです" : "");
			say(media, s, TRUE);
		} else {
			snprintf(s, sizeof(s), "unmounted %s", e->name);
			xfc_log(s);
			snprintf(s, sizeof(s), "%s を取り外しました", e->name);
			say(media, s, FALSE);
		}
	}
	media->sum = 0;
	media_scan(media);
}

/* ---------------------------------------------------------------- FTP */

static XWIN	*pend;			/* the window an FTP connection is being made for */
static XFCCONN	pconn;

/* The connection kept with the others, the same one replaced */
static void conn_keep( CONST XFCCONN *c )
{
	INT	i;

	for ( i = 0; i < nconn; i++ ) {
		if ( strcmp((char *)conn[i].host, (char *)c->host) == 0
		  && strcmp((char *)conn[i].port, (char *)c->port) == 0
		  && strcmp((char *)conn[i].user, (char *)c->user) == 0 ) break;
	}
	if ( i == nconn ) {
		if ( nconn == XFC_CONN_MAX ) {
			memmove(&conn[0], &conn[1], sizeof(XFCCONN) * ( XFC_CONN_MAX - 1 ));
			i = XFC_CONN_MAX - 1;
		} else {
			nconn++;
		}
	}
	conn[i] = *c;
	if ( !c->keep ) conn[i].pass[0] = 0;
	if ( xfc_conf_save(conn, nconn) < E_OK ) xfc_log("settings not kept");
}

/* One 接続 of the panel: the server reached and logged in to, or why not */
static ER ftp_try( XFCCONN *c, char *why, INT max )
{
	UINT	p;
	ER	er;

	if ( c->ssh ) {
		xfc_log("ssh not supported");
		snprintf(why, max, "SSH（SFTP）での接続はまだ使えません。FTP なら SSH を外してください");
		return E_NOSPT;
	}
	p = (UINT)atoi((char *)c->port);
	if ( p == 0 || p > 65535 ) p = FTP_PORT;
	er = xfc_net_up();
	if ( er < E_OK ) {
		snprintf(why, max, "網に繋がりません（%d）", (INT)er);
		xfc_log(why);
		return er;
	}
	memset(pend->ftp, 0, sizeof(T_FTP));
	pend->ftp->ctl = -1;
	xfc_net_init(&pend->xnet, &pend->fnet, 20000, (UINT)atoi((char *)c->lport));
	er = ftp_open(pend->ftp, &pend->fnet, (CONST char *)c->host, p, c->user[0] != 0 ? c->user : NULL,
		      c->user[0] != 0 ? c->pass : NULL, c->enc);
	if ( er < E_OK ) {
		snprintf(why, max, "%s に接続できません：%s（%d %s）", c->host, err_text(er), pend->ftp->code,
			 pend->ftp->text);
		xfc_log(why);
		return er;
	}
	return E_OK;
}

/* A connection asked for (c filled in from one kept, or NULL for a new one) and opened in a window */
static void open_ftp( CONST XFCCONN *from )
{
	XWIN	*w;
	char	s[300];
	UB	lb[XFC_FIELD + 32];

	memset(&pconn, 0, sizeof(pconn));
	if ( from != NULL ) {
		pconn = *from;
	} else {
		strcpy((char *)pconn.port, "21");
		pconn.enc = TXC_UTF8;
	}
	pend = (XWIN *)malloc(sizeof(XWIN));
	if ( pend == NULL ) return;
	memset(pend, 0, sizeof(*pend));
	pend->ftp = (T_FTP *)malloc(sizeof(T_FTP));
	pend->ftx = (T_XFUFTP *)malloc(sizeof(T_XFUFTP));
	if ( pend->ftp == NULL || pend->ftx == NULL || !xfc_ask_ftp(media, &pconn, ftp_try) ) {
		free(pend->ftp);
		free(pend->ftx);
		free(pend);
		pend = NULL;
		return;
	}
	if ( pconn.mode == XFC_DM_PASV ) pend->ftp->pasv = TRUE;
	if ( pconn.mode == XFC_DM_ACTIVE ) pend->ftp->active = TRUE;
	(void)dt_gettime(&pend->ftp->now);
	conn_keep(&pconn);
	snprintf(s, sizeof(s), "connected %s %s %s", pconn.host, ( pend->ftp->enc == TXC_UTF8 ) ? "utf-8"
		 : txc_name(pend->ftp->enc), pend->ftp->active ? "active" : pend->ftp->pasv ? "pasv" : "epsv");
	xfc_log(s);

	conn_label(&pconn, lb, sizeof(lb));
	w = files_new((CONST char *)lb);
	if ( w == NULL ) {
		(void)ftp_close(pend->ftp);
	} else {
		w->ftp = pend->ftp;
		w->ftx = pend->ftx;
		w->xnet = pend->xnet;
		w->fnet = pend->fnet;
		xfc_net_init(&w->xnet, &w->fnet, 20000, (UINT)atoi((char *)pconn.lport));
		memcpy(w->xnet.s, pend->xnet.s, sizeof(w->xnet.s));
		memcpy(w->xnet.addr, pend->xnet.addr, sizeof(w->xnet.addr));
		w->ftp->net = &w->fnet;
		w->is_ftp = TRUE;
		snprintf((char *)w->dev, sizeof(w->dev), "%s", lb);
		snprintf(w->label, sizeof(w->label), "ftp://%s", pconn.host);
		strcpy(w->source, "ftp");
		(void)xfu_ftp_tree(w->ftx, w->ftp, (CONST UB *)"", &w->tree);
		pend->ftp = NULL;
		pend->ftx = NULL;
		snprintf(s, sizeof(s), "window %s", w->label);
		xfc_log(s);
		go_to(w, (CONST UB *)"");
		snprintf(s, sizeof(s), "%s に接続しました（%s）", pconn.host, w->ftp->syst);
		say(w, s, FALSE);
	}
	free(pend->ftp);
	free(pend->ftx);
	free(pend);
	pend = NULL;
	media->sum = 0;
	media_scan(media);
}

/* ---------------------------------------------------------------- the jobs */

/* What came while a job ran or a panel was up: drawn, and Esc or a close stops the job */
EXPORT void xfc_meanwhile( CONST T_OBNTM *m )
{
	XWIN	*w = win_of(&m->uuid);

	if ( w == NULL ) {
		if ( m->id == 1 && media != NULL ) media_scan(media);	/* a disk: listed again later */
		return;
	}
	switch ( m->event ) {
	case OB_E_REDRAW:
		w->dirty = TRUE;
		break;
	case OB_E_KEY:
		if ( busy && ( m->code == K_ESC || ( ( m->mods & M_CTRL ) != 0 && wm_key_char(m->code, 0) == '.' ) ) ) {
			cancel = TRUE;
		}
		break;
	case OB_E_CLOSE:
	case OB_E_DELETE:
		cancel = TRUE;
		if ( w == media ) closing = TRUE;
		break;
	case OB_E_DROP: {
		T_OBDROP	*d = (T_OBDROP *)malloc(sizeof(T_OBDROP));
		T_OBDRANS	a;
		SZ		asz = 0;

		if ( d != NULL && ob_rea_rec(w->kw, OB_WR_DROP, 0, d, sizeof(*d), &asz) >= E_OK ) {
			memset(&a, 0, sizeof(a));
			a.seq = d->seq;
			a.answer = OB_DR_REFUSE;
			strcpy((char *)a.msg, "ファイル変換は作業中です");
			(void)ob_wri_rec(w->kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
			xfc_log("drop refused");
		}
		free(d);
		break;
	}
	default:
		break;
	}
	paint_all();
}

static void job_events( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( port > 0 && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
		xfc_meanwhile(&m);
	}
}

static ER note( void *arg, CONST UB *name, ER er )
{
	char	s[400];

	(void)arg;
	if ( er >= E_OK ) done_n++;
	if ( er < E_OK && er != E_ABORT ) {
		snprintf(s, sizeof(s), "skip %s %d", name, (INT)er);
		xfc_log(s);
	}
	if ( jobw != NULL ) {
		snprintf(jobw->status, sizeof(jobw->status), "%s %d 件：%s（Esc で中止）", job_what, done_n, name);
		jobw->status_err = FALSE;
		jobw->dirty = TRUE;
		(void)wm_msg_put(jobw->status);
	}
	job_events();
	return cancel ? E_ABORT : E_OK;
}

static ER tick( void *arg, D bytes )
{
	(void)arg;
	(void)bytes;
	job_events();
	return cancel ? E_ABORT : E_OK;
}

static void job_begin( XWIN *w, CONST char *what )
{
	busy = TRUE;
	cancel = FALSE;
	done_n = 0;
	jobw = w;
	snprintf(job_what, sizeof(job_what), "%s", what);
	say(w, what, FALSE);
	xfc_paint(w);
}

static void job_end( void )
{
	busy = FALSE;
	jobw = NULL;
}

/* What is asked of the desktop, and its answer waited for, at most wait_ms */
static ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0;
	ID		k, ka;
	INT		i;
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
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( i = 0; er >= E_OK; i += 20 ) {
		if ( ob_rea_rec(ka, 0, 0, an, sizeof(*an), &asz) >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
		if ( i >= wait_ms ) {
			er = E_TMOUT;
			break;
		}
		(void)tk_dly_tsk(20);
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* The desktop asked to open an object as a double click would */
static ER open_obj( CONST TS_UUID *u )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = *u;
	rq.recno = -1;
	return ask_desktop(&rq, &an, 3000);
}

/* The object an entry names, taken in; E_OK and its UUID */
static ER import_one( XWIN *w, XFCENT *x, CONST XFCIMP *how, CONST TS_UUID *box, CONST TS_UUID *near,
		      TS_UUID *u, T_XFUSTAT *st )
{
	T_XFUOPT	opt;
	UB		path[XFU_PATH_MAX];

	if ( x->kind == E_UP || x->kind == E_VOL ) {
		snprintf((char *)path, sizeof(path), "%s", w->dir);	/* the directory itself */
	} else if ( path_join(path, sizeof(path), w->dir, x->name) < E_OK ) {
		return E_LIMIT;
	}
	memset(&opt, 0, sizeof(opt));
	opt.source = w->source;
	opt.note = note;
	opt.tick = tick;
	opt.stat = st;
	if ( how != NULL ) {
		opt.as = how->as;
		opt.enc = how->enc;
		opt.shrink = how->shrink;
	}
	opt.near = near;
	return xfu_import(&w->tree, path, &opt, box, u);
}

/* The name of the first entry chosen, or of the cursor's */
static CONST UB *first_chosen( XWIN *w )
{
	INT	i;

	for ( i = 0; i < w->nent; i++ ) {
		if ( w->ent[i].marked ) return w->ent[i].name;
	}
	return ( w->cur >= 0 && w->cur < w->nent ) ? w->ent[w->cur].name : (CONST UB *)"";
}

/*
 * What is chosen (or the entry under the cursor) made 実身 in a new box,
 * a figure of links in ファイル変換の箱, or in the figure the accessory
 * was started on when asked to; the desktop then opens the box.
 */
static void do_import( XWIN *w )
{
	T_XFUSTAT	st;
	XFCIMP		how;
	TS_UUID		boxu, u, home, *tops;
	UB		bname[OB_NAME_MAX];
	char		s[400], a[40], b[40];
	INT		i, k, n, made = 0, fails = 0, objs = 0, skipped = 0, geta = 0;
	BOOL		newbox;
	ER		er = E_OK, e;

	if ( w->kind != W_FILES ) {
		say(w, "実身にするファイルのある窓で選んでください", TRUE);
		return;
	}
	n = marked(w);
	if ( n == 0 && w->cur >= 0 && w->cur < w->nent ) n = 1;
	if ( n == 0 ) return;
	if ( !xfc_ask_import(w, n, first_chosen(w), &how) ) return;
	tops = (TS_UUID *)malloc(sizeof(TS_UUID) * ( w->nent + 1 ));
	if ( tops == NULL ) {
		say(w, "メモリが足りません", TRUE);
		return;
	}
	newbox = !( o_argbox && hasarg && argfig );
	job_begin(w, "実身にしています");
	for ( i = 0; i < w->nent && !cancel; i++ ) {
		XFCENT	*x = &w->ent[i];

		if ( marked(w) > 0 ? !x->marked : ( i != w->cur ) ) continue;
		memset(&st, 0, sizeof(st));
		e = import_one(w, x, &how, newbox ? NULL : &argobj, NULL, &u, &st);
		if ( e >= E_OK ) {
			tops[made++] = u;
			objs += st.objects;
		} else {
			fails++;
			er = e;
			snprintf(s, sizeof(s), "%s を実身にできません：%s（%d）", x->name, err_text(e), (INT)e);
			say(w, s, TRUE);
		}
		skipped += st.skipped;
		geta += st.geta;
	}
	job_end();
	boxu = argobj;
	if ( made > 0 && newbox ) {
		snprintf((char *)bname, sizeof(bname), "取込 %s", w->label);
		e = xfc_uuid_parse(SYSDEF_XFCONV_BOX, &home) ? xfu_make_box(bname, &home, &boxu) : E_SYS;
		if ( e >= E_OK ) e = xfu_box_link(&home, &boxu);
		for ( k = 0; e >= E_OK && k < made; k++ ) e = xfu_box_link(&boxu, &tops[k]);
		if ( e < E_OK ) {
			snprintf(s, sizeof(s), "取り込んだものを箱に入れられません：%s", err_text(e));
			say(w, s, TRUE);
			er = e;
		}
	}
	if ( made > 0 ) {
		T_OBREF	r;

		haslast = TRUE;
		lastbox = boxu;
		lasttop = tops[0];
		lastname[0] = 0;
		if ( ob_ref_obj(&tops[0], &r) >= E_OK ) strncpy(lastname, (char *)r.name, sizeof(lastname) - 1);
	}
	xfc_uuid_str(&boxu, a);
	if ( made > 0 ) xfc_uuid_str(&tops[0], b);
	else b[0] = 0;
	snprintf(s, sizeof(s), "imported %d of %d objects %d box %s first %s", made, made + fails, objs, a, b);
	xfc_log(s);
	if ( cancel ) {
		snprintf(s, sizeof(s), "中止しました（%d 件は実身にしました）", made);
		say(w, s, TRUE);
	} else if ( made > 0 ) {
		snprintf(s, sizeof(s), "%d 件を実身にしました（実身 %d、飛ばした %d、〓 %d）%s", made, objs,
			 skipped, geta, ( fails > 0 ) ? " 失敗もあります" : "");
		say(w, s, fails > 0 || er < E_OK);
	}
	if ( made > 0 && open_obj(&boxu) < E_OK ) xfc_log("the desktop did not open the box");
	free(tops);
}

/* ---------------------------------------------------------------- carried out of a file list */

/* How a link to an object made from entry e looks where it is put */
static void look_of( CONST XFCENT *e, T_DTLOOK *look )
{
	INT	w = dp_text_width(e->name, 18) + 40;

	memset(look, 0, sizeof(*look));
	look->w = ( w < 120 ) ? 120 : ( w > 400 ) ? 400 : w;
	look->h = XFC_VOBJ_H;
	look->frcol = look->chcol = look->tbcol = look->bgcol = 0xFFFFFFFFU;
}

/*
 * The 仮身 chosen carried out, held where the press was: the desktop
 * follows the pointer with their outline and says where they were let
 * go. Over a window of the desktop's that takes links, a panel asks how
 * the files are made 実身; they are made on that window's volume and a
 * link to each is put where it was let go, as they lay in the list.
 */
static void carry_out( XWIN *w, INT px, INT py )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	bb, r;
	T_XFUSTAT	st;
	XFCIMP		how;
	TS_UUID		*tops;
	INT		*rx, *ry, *who;
	INT		i, n = 0, made = 0, fails = 0, objs = 0, placed = 0, below = 0;
	char		s[400], a[40], b[40];
	UINT		seq;
	ER		er;

	/* the outline: the box round the chosen 仮身 that are shown */
	bb.left = bb.top = 1 << 30;
	bb.right = bb.bottom = -1;
	for ( i = 0; i < w->nent; i++ ) {
		if ( !w->ent[i].marked || ( w->ent[i].kind != E_DIR && w->ent[i].kind != E_FILE ) ) continue;
		n++;
		if ( !xfc_vobj_rect(w, i, &r) ) continue;
		if ( r.left < bb.left ) bb.left = r.left;
		if ( r.top < bb.top ) bb.top = r.top;
		if ( r.right > bb.right ) bb.right = r.right;
		if ( r.bottom > bb.bottom ) bb.bottom = r.bottom;
	}
	if ( n == 0 || bb.right < 0 ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	rq.look.w = bb.right - bb.left;
	rq.look.h = bb.bottom - bb.top;
	rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	rq.grab_x = px - bb.left;
	rq.grab_y = py - bb.top;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	seq = rq.seq;
	if ( er == E_OBJ ) {
		/* a click: what was pressed alone is chosen */
		i = xfc_hit(w, px, py);
		if ( i >= 0 && marked(w) > 1 ) {
			choose_one(w, i);
			tell_chosen(w);
		}
		return;
	}
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "carry %d", (INT)er);
		xfc_log(s);
		return;
	}
	xfc_uuid_str(&an.into, a);
	snprintf(s, sizeof(s), "carried %d into %s", n, a);
	xfc_log(s);
	if ( !xfc_ask_import(w, n, first_chosen(w), &how) ) return;

	tops = (TS_UUID *)malloc(sizeof(TS_UUID) * n);
	rx = (INT *)malloc(sizeof(INT) * n * 3);
	if ( tops == NULL || rx == NULL ) {
		free(tops);
		free(rx);
		say(w, "メモリが足りません", TRUE);
		return;
	}
	ry = rx + n;
	who = ry + n;
	job_begin(w, "実身にしています");
	for ( i = 0; i < w->nent && !cancel; i++ ) {
		XFCENT	*x = &w->ent[i];
		ER	e;

		if ( !x->marked || ( x->kind != E_DIR && x->kind != E_FILE ) ) continue;
		memset(&st, 0, sizeof(st));
		e = import_one(w, x, &how, NULL, &an.into, &tops[made], &st);
		if ( e < E_OK ) {
			fails++;
			snprintf(s, sizeof(s), "%s を実身にできません：%s（%d）", x->name, err_text(e), (INT)e);
			say(w, s, TRUE);
			continue;
		}
		objs += st.objects;
		if ( xfc_vobj_rect(w, i, &r) ) {
			rx[made] = r.left - bb.left;
			ry[made] = r.top - bb.top;
		} else {
			rx[made] = 0;
			ry[made] = bb.bottom - bb.top + 6 + ( below++ ) * XFC_ROW_H;
		}
		who[made] = i;
		made++;
	}
	job_end();

	/* the links put where they were let go, each where it lay in the outline */
	for ( i = 0; i < made; i++ ) {
		T_DTREQ	pq;
		T_DTANS	pa;

		memset(&pq, 0, sizeof(pq));
		pq.req = ( i + 1 < made ) ? DT_RQ_PLACE_MORE : DT_RQ_PLACE;
		pq.target = tops[i];
		pq.recno = -1;
		look_of(&w->ent[who[i]], &pq.look);
		pq.grab_x = rx[i];
		pq.grab_y = ry[i];
		pq.carry = seq;
		memset(&pa, 0, sizeof(pa));
		if ( ask_desktop(&pq, &pa, 5000) >= E_OK ) {
			placed++;
		} else {
			(void)ob_del_obj(&tops[i]);	/* nothing would link to it */
		}
	}
	if ( made > 0 ) {
		T_OBREF	ref;

		haslast = TRUE;
		lastbox = an.into;
		lasttop = tops[0];
		lastname[0] = 0;
		if ( ob_ref_obj(&tops[0], &ref) >= E_OK ) strncpy(lastname, (char *)ref.name, sizeof(lastname) - 1);
		xfc_uuid_str(&tops[0], b);
	} else {
		b[0] = 0;
	}
	snprintf(s, sizeof(s), "imported %d of %d objects %d into %s first %s", made, made + fails, objs, a, b);
	xfc_log(s);
	snprintf(s, sizeof(s), "placed %d links", placed);
	xfc_log(s);
	if ( cancel ) {
		snprintf(s, sizeof(s), "中止しました（%d 件は実身にしました）", made);
		say(w, s, TRUE);
	} else if ( placed > 0 ) {
		snprintf(s, sizeof(s), "%d 件を実身にして置きました（実身 %d）%s", placed, objs,
			 ( fails > 0 || placed < made ) ? " 失敗もあります" : "");
		say(w, s, fails > 0 || placed < made);
	}
	free(tops);
	free(rx);
}

/* ---------------------------------------------------------------- written out */

/* Objects written into directory dir of a file list; the names written, for choosing them after */
static INT export_to( XWIN *w, CONST TS_UUID *src, INT n, CONST UB *dir, CONST XFCEXP *how, UB *names, INT max )
{
	T_XFUOPT	opt;
	T_XFUSTAT	st;
	UB		name[XFU_NAME_MAX];
	char		s[400];
	INT		i, done = 0, at = 0;
	ER		er;

	job_begin(w, "ファイルにしています");
	for ( i = 0; i < n && !cancel; i++ ) {
		memset(&opt, 0, sizeof(opt));
		opt.flags = how->flags;
		opt.outenc = how->outenc;
		opt.name = ( n == 1 && how->name[0] != 0 ) ? how->name : NULL;
		opt.note = note;
		opt.tick = tick;
		memset(&st, 0, sizeof(st));
		opt.stat = &st;
		er = xfu_export(&src[i], &w->tree, dir, &opt, name, sizeof(name));
		if ( er < E_OK ) {
			snprintf(s, sizeof(s), "exported none %d", (INT)er);
			xfc_log(s);
			snprintf(s, sizeof(s), "書き出せません：%s", err_text(er));
			say(w, s, TRUE);
			continue;
		}
		snprintf(s, sizeof(s), "exported %s files %d objects %d skipped %d", name, st.files, st.objects, st.skipped);
		xfc_log(s);
		snprintf(s, sizeof(s), "%s を書き出しました（ファイル %d、飛ばした %d、〓 %d）", name, st.files,
			 st.skipped, st.geta);
		say(w, s, FALSE);
		done++;
		if ( names != NULL && at + (INT)strlen((char *)name) + 2 < max ) {
			strcpy((char *)names + at, (char *)name);
			at += (INT)strlen((char *)name) + 1;
			names[at] = 0;
		}
	}
	job_end();
	return done;
}

/* What the file list writes when asked from the keyboard: the object started on, or what was taken in last */
static void do_export( XWIN *w )
{
	XFCEXP	how;
	TS_UUID	*src;
	UB	names[1024];
	char	s[80];

	if ( w->kind != W_FILES ) {
		say(w, "書き出す先の窓で選んでください", TRUE);
		return;
	}
	if ( hasarg ) src = &argobj;
	else if ( haslast ) src = &lasttop;
	else {
		say(w, "書き出す実身がありません（仮身をこの窓に置くと、その実身をファイルにします）", TRUE);
		return;
	}
	if ( !xfc_ask_export(w, 1, (CONST UB *)( hasarg ? argname : lastname ), w->dir, &how, o_tadjs, o_layout,
			     o_sjis) ) {
		return;
	}
	names[0] = names[1] = 0;
	snprintf(s, sizeof(s), "export into %s", w->dir);
	xfc_log(s);
	(void)export_to(w, src, 1, w->dir, &how, names, sizeof(names));
	(void)files_scan(w, names);
	tell_chosen(w);
}

/* The drop answered: taken or not, and what the one who dropped it is told */
static void drop_answer( XWIN *w, CONST T_OBDROP *d, UINT answer, CONST char *msg )
{
	T_OBDRANS	a;
	SZ		asz = 0;

	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	a.answer = answer;
	strncpy((char *)a.msg, msg, sizeof(a.msg) - 1);
	(void)ob_wri_rec(w->kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	xfc_log(( answer == OB_DR_ACCEPT ) ? "drop taken" : "drop refused");
}

/* The object started on or dropped: its name, and whether it is a figure */
static void arg_look( void )
{
	T_OBREF	r;
	UB	head[512];
	SZ	asz = 0;
	ID	key;

	if ( ob_ref_obj(&argobj, &r) < E_OK ) {
		hasarg = FALSE;
		return;
	}
	strncpy(argname, (char *)r.name, sizeof(argname) - 1);
	key = ob_opn_obj(&argobj, OB_OP_R);
	if ( key > 0 ) {
		if ( ob_rea_rec(key, 0, 0, head, sizeof(head) - 1, &asz) >= E_OK ) {
			head[asz] = 0;
			argfig = (BOOL)( strstr((char *)head, "<figure") != NULL );
		}
		ob_cls_obj(key);
	}
}

/*
 * 仮身 dropped on a window. On a file list each object is written into
 * the directory it shows, or into the directory whose 仮身 it was let
 * go on, after the panel has asked how; the list then shows the files
 * written, chosen. The main window takes none.
 */
static void dropped( XWIN *w )
{
	T_OBDROP	*d;
	XFCEXP		how;
	TS_UUID		src[OB_DROP_MAX];
	UB		dir[XFU_PATH_MAX], *names;
	char		s[400], id[40];
	SZ		asz = 0;
	INT		i, row;

	d = (T_OBDROP *)malloc(sizeof(T_OBDROP));
	if ( d == NULL ) return;
	memset(d, 0, sizeof(*d));
	if ( ob_rea_rec(w->kw, OB_WR_DROP, 0, d, sizeof(*d), &asz) < E_OK || d->n <= 0 ) {
		free(d);
		return;
	}
	xfc_uuid_str(&d->v[0].target, id);
	snprintf(s, sizeof(s), "drop %d at %d,%d first %s ops %x", d->n, d->x, d->y, id, d->v[0].ops);
	xfc_log(s);
	if ( busy ) {
		drop_answer(w, d, OB_DR_REFUSE, "ファイル変換は作業中です");
		free(d);
		return;
	}
	if ( w->kind != W_FILES ) {
		drop_answer(w, d, OB_DR_REFUSE, "ディスクを開き、ファイル一覧の窓に置いてください");
		free(d);
		return;
	}
	strncpy((char *)dir, (char *)w->dir, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = 0;
	row = xfc_hit(w, d->x, d->y);
	if ( row >= 0 && w->ent[row].kind == E_DIR && path_join(dir, sizeof(dir), w->dir, w->ent[row].name) < E_OK ) {
		strncpy((char *)dir, (char *)w->dir, sizeof(dir) - 1);
	}
	snprintf(s, sizeof(s), "%d 個の実身を %s/%s に書き出します", d->n, w->label, dir);
	drop_answer(w, d, OB_DR_ACCEPT, s);
	for ( i = 0; i < d->n; i++ ) src[i] = d->v[i].target;
	if ( d->n == 1 ) {
		argobj = src[0];
		hasarg = TRUE;
		argfig = FALSE;
		arg_look();
	}
	snprintf(s, sizeof(s), "drop into %s", dir);
	xfc_log(s);
	if ( xfc_ask_export(w, d->n, d->v[0].name, dir, &how, o_tadjs, o_layout, o_sjis) ) {
		names = (UB *)malloc(2048);
		if ( names != NULL ) names[0] = names[1] = 0;
		(void)export_to(w, src, d->n, dir, &how, names, 2048);
		(void)files_scan(w, ( strcmp((char *)dir, (char *)w->dir) == 0 ) ? names : NULL);
		tell_chosen(w);
		free(names);
	}
	free(d);
}

/* ---------------------------------------------------------------- commands */

static void toggle( XWIN *w, BOOL *o, CONST char *what )
{
	char	s[128];

	*o = (BOOL)!*o;
	snprintf(s, sizeof(s), "%s：%s", what, *o ? "する" : "しない");
	say(w, s, FALSE);
}

static void close_win( XWIN *w )
{
	if ( w == media ) closing = TRUE;
	else files_close(w);
}

/* What an entry does when it is opened */
static void open_entry( XWIN *w, INT i )
{
	XFCENT	*e;
	UB	d[XFU_PATH_MAX];

	if ( i < 0 || i >= w->nent ) return;
	e = &w->ent[i];
	switch ( e->kind ) {
	case E_DISK:
		open_disk(e);
		break;
	case E_FTP:
		open_ftp(&conn[e->conn]);
		break;
	case E_NEWFTP:
		open_ftp(NULL);
		break;
	case E_VOL:
		go_to(w, (CONST UB *)"");
		break;
	case E_UP:
		go_up(w);
		break;
	case E_DIR:
		if ( path_join(d, sizeof(d), w->dir, e->name) < E_OK ) {
			say(w, "ディレクトリが深すぎます", TRUE);
			break;
		}
		go_to(w, d);
		break;
	default:
		say(w, "ファイルは、仮身を TessronOS の窓へ運ぶと実身になります", FALSE);
		break;
	}
}

static void command( XWIN *w, CONST char *code, INT index )
{
	if ( busy ) return;
	if ( strcmp(code, "close") == 0 ) close_win(w);
	else if ( strcmp(code, "open") == 0 ) open_entry(w, w->cur);
	else if ( strcmp(code, "import") == 0 ) do_import(w);
	else if ( strcmp(code, "export") == 0 ) do_export(w);
	else if ( strcmp(code, "mount") == 0 ) {
		if ( w == media ) mount_toggle();
		else say(w, "マウント・取外しは、ファイル変換の窓でディスクを選んで行います", TRUE);
	} else if ( strcmp(code, "ftp") == 0 ) open_ftp(NULL);
	else if ( strcmp(code, "up") == 0 ) go_up(w);
	else if ( strcmp(code, "root") == 0 && w->kind == W_FILES ) go_to(w, (CONST UB *)"");
	else if ( strcmp(code, "dir.hist") == 0 && w->kind == W_FILES && index >= 0 && index < w->nhist ) {
		UB	d[XFU_PATH_MAX];

		strcpy((char *)d, (char *)w->hist[index]);
		go_to(w, d);
	} else if ( strcmp(code, "all") == 0 && w->kind == W_FILES ) {
		INT	i;

		for ( i = 0; i < w->nent; i++ ) {
			w->ent[i].marked = (BOOL)( w->ent[i].kind == E_DIR || w->ent[i].kind == E_FILE );
		}
		tell_chosen(w);
	} else if ( strcmp(code, "refresh") == 0 ) rescan(w);
	else if ( strcmp(code, "tadjs") == 0 ) toggle(w, &o_tadjs, "TADjs の組で書き出す");
	else if ( strcmp(code, "layout") == 0 ) toggle(w, &o_layout, "図形の配置も書き出す");
	else if ( strcmp(code, "sjis") == 0 ) toggle(w, &o_sjis, "文章を Shift_JIS で書き出す");
	else if ( strcmp(code, "ro") == 0 ) toggle(w, &o_ro, "読込みのみでマウント");
	else if ( strcmp(code, "argbox") == 0 ) {
		if ( hasarg && argfig ) toggle(w, &o_argbox, "起動した実身に取り込む");
		else say(w, "起動した実身が図形ではないので、新しい箱に取り込みます", TRUE);
	}
	w->dirty = TRUE;
}

/* Ctrl and a letter: the menu's item of that letter */
static CONST struct {
	UB		key;
	CONST char	*code;
} ctrl_key[] = {
	{ 'e', "close" }, { 'i', "import" }, { 'o', "export" }, { 'm', "mount" }, { 'f', "ftp" },
	{ 'u', "up" }, { 'r', "refresh" }, { 't', "tadjs" }, { 'l', "layout" }, { 'j', "sjis" },
	{ 'w', "ro" }, { 'b', "argbox" }, { 'a', "all" },
};

static void menu( XWIN *w, CONST T_OBNTM *m )
{
	TS_UUID	def, dirdef;
	T_MNSEL	s;
	ID	mid;
	BOOL	chosen, files = (BOOL)( w->kind == W_FILES );

	if ( !xfc_uuid_parse(SYSDEF_MENU_XFCONV, &def) || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( o_tadjs ) (void)mn_chg_atr(mid, NULL, "tadjs", MN_TICK);
	if ( o_layout ) (void)mn_chg_atr(mid, NULL, "layout", MN_TICK);
	if ( o_sjis ) (void)mn_chg_atr(mid, NULL, "sjis", MN_TICK);
	if ( o_ro ) (void)mn_chg_atr(mid, NULL, "ro", MN_TICK);
	if ( o_argbox ) (void)mn_chg_atr(mid, NULL, "argbox", MN_TICK);
	if ( !( hasarg && argfig ) ) (void)mn_chg_atr(mid, NULL, "argbox", MN_GREY);
	if ( !files ) {
		(void)mn_chg_atr(mid, NULL, "import", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "export", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "dir", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "all", MN_GREY);
	} else {
		(void)mn_chg_atr(mid, NULL, "mount", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "ro", MN_GREY);
	}
	if ( files && xfc_uuid_parse(SYSDEF_MENU_XFC_DIR, &dirdef) && w->nhist > 0 ) {
		UB	*lb = (UB *)malloc((size_t)XFC_HIST * WM_LABEL_MAX);
		INT	i;

		if ( lb != NULL ) {
			for ( i = 0; i < w->nhist; i++ ) {
				snprintf((char *)lb + i * WM_LABEL_MAX, WM_LABEL_MAX, "/%s", w->hist[i]);
			}
			(void)mn_set_lst(mid, &dirdef, "dir.hist", lb, WM_LABEL_MAX, w->nhist);
			free(lb);
		}
		if ( w->dir[0] == 0 ) (void)mn_chg_atr(mid, &dirdef, "up", MN_GREY);
	}
	chosen = (BOOL)( mn_pop_men(mid, w->kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) command(w, (CONST char *)s.code, s.index);
}

/* To the next name that starts with what was typed (within a second) */
static void type_ahead( XWIN *w, UB c )
{
	UD	now = 0;
	INT	n, i, k;

	(void)ts_get_mono(&now);
	if ( now - w->typed_at > 1000000000ULL ) w->typed[0] = 0;
	w->typed_at = now;
	n = (INT)strlen(w->typed);
	if ( n < (INT)sizeof(w->typed) - 1 ) {
		w->typed[n++] = (char)c;
		w->typed[n] = 0;
	}
	for ( k = 0; k < w->nent; k++ ) {
		i = ( w->cur + k ) % w->nent;
		if ( strncasecmp((char *)w->ent[i].name, w->typed, n) == 0 ) {
			choose_one(w, i);
			tell_chosen(w);
			return;
		}
	}
}

static void key( XWIN *w, CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, 0);
	INT	i, to = -1;
	BOOL	shift = (BOOL)( ( m->mods & M_SHIFT ) != 0 );

	/* what was typed to find a name starts again at any other key */
	if ( ( m->mods & M_CTRL ) != 0 || c <= 0x20 || c >= 0x7F ) w->typed[0] = 0;
	if ( ( m->mods & M_CTRL ) != 0 ) {
		for ( i = 0; i < (INT)( sizeof(ctrl_key) / sizeof(ctrl_key[0]) ); i++ ) {
			if ( c == ctrl_key[i].key || c == ctrl_key[i].key - 'a' + 'A' ) {
				command(w, ctrl_key[i].code, -1);
				return;
			}
		}
		return;
	}
	if ( busy ) return;
	switch ( m->code ) {
	case K_UP:	to = w->cur - 1; break;
	case K_DOWN:	to = w->cur + 1; break;
	case K_PGUP:	to = w->cur - xfc_rows(w); break;
	case K_PGDN:	to = w->cur + xfc_rows(w); break;
	case K_HOME:	to = 0; break;
	case K_END:	to = w->nent - 1; break;
	case K_ENTER:
	case K_KPENTER:	open_entry(w, w->cur); return;
	case K_BS:	go_up(w); return;
	case K_ESC:
		choose_none(w);
		tell_chosen(w);
		return;
	case K_SPACE:
		if ( w->cur < w->nent && ( w->ent[w->cur].kind == E_DIR || w->ent[w->cur].kind == E_FILE ) ) {
			w->ent[w->cur].marked = (BOOL)!w->ent[w->cur].marked;
			w->anchor = w->cur;
			if ( w->cur + 1 < w->nent ) w->cur++;
			keep_cur(w);
			tell_chosen(w);
		}
		return;
	default:
		c = wm_key_char(m->code, m->mods);
		if ( c > 0x20 && c < 0x7F ) type_ahead(w, c);
		return;
	}
	if ( to < 0 ) to = 0;
	if ( to >= w->nent ) to = w->nent - 1;
	if ( shift && w->kind == W_FILES ) {
		if ( w->anchor < 0 ) w->anchor = w->cur;
		choose_run(w, to);
	} else {
		choose_one(w, to);
	}
	tell_chosen(w);
}

/* ---------------------------------------------------------------- the pointer */

static BOOL on_bar( XWIN *w, INT x, INT y )
{
	INT	ww, wh;

	xfc_work_size(w, &ww, &wh);
	return (BOOL)( x >= ww - XFC_SB_W && y < wh - XFC_STATUS_H );
}

/* The scroll bar's knob moved to where the pointer is */
static void bar_to( XWIN *w, INT y )
{
	INT	ww, wh, rows = xfc_rows(w), h;

	xfc_work_size(w, &ww, &wh);
	h = wh - XFC_STATUS_H;
	if ( h <= 0 || w->nent <= rows ) return;
	w->top = y * w->nent / h - rows / 2;
	if ( w->top > w->nent - rows ) w->top = w->nent - rows;
	if ( w->top < 0 ) w->top = 0;
	w->dirty = TRUE;
}

/*
 * A rectangle whose button was let go over another window is never told
 * of it: it is put away when anything is next pressed.
 */
static void band_drop( void )
{
	INT	i;

	for ( i = 0; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used && ( xw[i].band || xw[i].thumb ) ) {
			xw[i].band = FALSE;
			xw[i].thumb = FALSE;
			xw[i].dirty = TRUE;
		}
	}
}

static void press( XWIN *w, CONST T_OBNTM *m )
{
	INT	i;
	BOOL	shift = (BOOL)( ( m->mods & M_SHIFT ) != 0 );

	if ( busy ) return;
	band_drop();			/* a rectangle let go outside its window ends here */
	if ( on_bar(w, m->x, m->y) ) {
		w->thumb = TRUE;
		bar_to(w, m->y);
		return;
	}
	i = xfc_hit(w, m->x, m->y);
	if ( i < 0 ) {
		/* a rectangle begins: what it takes is chosen when it is let go */
		if ( !shift ) choose_none(w);
		w->band = TRUE;
		w->bx0 = w->bx1 = m->x;
		w->by0 = w->by1 = m->y;
		w->press_row = -1;
		w->dirty = TRUE;
		return;
	}
	if ( i == w->press_row && m->when - w->press_at < DBL_NS ) {
		w->press_row = -1;
		choose_one(w, i);
		open_entry(w, i);
		return;
	}
	w->press_row = i;
	w->press_at = m->when;
	if ( shift ) {
		w->ent[i].marked = (BOOL)!w->ent[i].marked;
		w->cur = w->anchor = i;
		tell_chosen(w);
		return;
	}
	if ( !w->ent[i].marked ) choose_one(w, i);
	w->cur = i;
	tell_chosen(w);
	xfc_paint(w);
	if ( w->kind == W_FILES && ( w->ent[i].kind == E_DIR || w->ent[i].kind == E_FILE ) ) {
		carry_out(w, m->x, m->y);
	}
}

static void band_take( XWIN *w )
{
	T_DPRECT	b, r;
	INT		i;

	b.left = ( w->bx0 < w->bx1 ) ? w->bx0 : w->bx1;
	b.right = ( w->bx0 < w->bx1 ) ? w->bx1 : w->bx0;
	b.top = ( w->by0 < w->by1 ) ? w->by0 : w->by1;
	b.bottom = ( w->by0 < w->by1 ) ? w->by1 : w->by0;
	for ( i = 0; i < w->nent; i++ ) {
		if ( w->kind == W_FILES && w->ent[i].kind != E_DIR && w->ent[i].kind != E_FILE ) continue;
		if ( !xfc_vobj_rect(w, i, &r) ) continue;
		if ( r.left < b.right && r.right > b.left && r.top < b.bottom && r.bottom > b.top ) {
			w->ent[i].marked = TRUE;
			w->cur = i;
		}
	}
}

static void moved( XWIN *w, CONST T_OBNTM *m )
{
	if ( w->thumb ) {
		bar_to(w, m->y);
	} else if ( w->band ) {
		w->bx1 = m->x;
		w->by1 = m->y;
		w->dirty = TRUE;
	}
}

static void released( XWIN *w, CONST T_OBNTM *m )
{
	if ( w->thumb ) {
		w->thumb = FALSE;
		return;
	}
	if ( w->band ) {
		w->bx1 = m->x;
		w->by1 = m->y;
		w->band = FALSE;
		band_take(w);
		tell_chosen(w);
	}
}

static void wheel( XWIN *w, CONST T_OBNTM *m )
{
	INT	rows = xfc_rows(w);

	if ( m->code != HID_WHEEL_V ) return;
	w->top -= m->dz * 3;
	if ( w->top > w->nent - rows ) w->top = w->nent - rows;
	if ( w->top < 0 ) w->top = 0;
	w->dirty = TRUE;
}

/* ---------------------------------------------------------------- notices */

/* A notice about a disk: the list made again; a file list whose volume went closed */
static void disk_notice( CONST T_OBNTM *m )
{
	XWIN	*w;
	char	s[200];
	T_FSTAT	st;

	w = files_of_disk(&m->uuid);
	if ( w != NULL && ( m->event == OB_E_DETACH || m->event == OB_E_DELETE || m->event == OB_E_CHANGE )
	  && fs_stat(w->label, &st) < 0 ) {
		snprintf(s, sizeof(s), "%s が外されました", w->label);
		w->mounted = FALSE;
		files_close(w);
		say(media, s, TRUE);
		return;
	}
	media_scan(media);
}

static void event( CONST T_OBNTM *m )
{
	XWIN	*w = win_of(&m->uuid);

	if ( w == NULL ) {
		if ( m->id == 1 ) disk_notice(m);
		return;
	}
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		close_win(w);
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(w, m);
		else if ( m->code == 0 ) press(w, m);
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(w, m);
		break;
	case OB_E_MOVE:
		moved(w, m);
		break;
	case OB_E_WHEEL:
		wheel(w, m);
		break;
	case OB_E_KEY:
		key(w, m);
		break;
	case OB_E_REDRAW:
		w->dirty = TRUE;
		break;
	case OB_E_DROP:
		dropped(w);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBNTM	m;
	T_OBCRE	c;
	SZ	asz = 0;
	char	s[128];
	INT	i;

	media = &xw[0];
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( port <= 0 ) {
		xfc_log("no channel");
		return 3;
	}
	xfc_dlg_init(port);
	hasarg = (BOOL)( ts_get_arg(&argobj, sizeof(argobj)) >= (SZ)sizeof(argobj) );
	if ( hasarg ) arg_look();
	nconn = xfc_conf_load(conn, XFC_CONN_MAX);
	media->kind = W_MEDIA;
	if ( win_open(media, "ファイル変換", 120, 90, MEDIA_W, MEDIA_H) < E_OK ) {
		xfc_log("no window");
		ob_cls_obj(port);
		(void)ob_del_obj(&ch);
		return 3;
	}
	watch_devices();
	media_scan(media);
	if ( hasarg ) {
		char	id[40];

		xfc_uuid_str(&argobj, id);
		snprintf(s, sizeof(s), "ready arg %s %s", id, argfig ? "figure" : "object");
	} else {
		snprintf(s, sizeof(s), "ready");
	}
	xfc_log(s);
	say(media, "ディスクを開くと、そのファイルを仮身で示します", FALSE);
	xfc_paint(media);

	while ( !closing ) {
		while ( !closing && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) event(&m);
		paint_all();
		if ( !closing ) (void)tk_dly_tsk(20);
	}
	for ( i = 1; i < XFC_WIN_MAX; i++ ) {
		if ( xw[i].used ) files_close(&xw[i]);
	}
	media = NULL;
	if ( klist > 0 ) ob_cls_obj(klist);
	if ( xw[0].kw > 0 ) ob_cls_obj(xw[0].kw);
	(void)ob_del_obj(&xw[0].win);
	free(xw[0].ent);
	ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	(void)wm_msg_put(NULL);
	xfc_log("closed");
	return 0;
}
