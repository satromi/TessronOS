/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	arc_main.c
 *	書庫管理: the program (design 11.20)
 *
 *	It is started with the UUID of an archive object (書庫実身) and
 *	opens a window on it. An empty archive takes a link put into the
 *	window: the objects reachable from it are found, and a panel of the
 *	data box (SYSDEF_BOX_ARCHIVE) lists them and asks: 〈作成〉 packs
 *	them into the archive as its book (書籍), 〈取消し〉 or Esc leaves the
 *	archive empty. An archive that
 *	has a book shows it as a 付箋 with its name; carried out of the
 *	window and let go over a window that takes links, the book is taken
 *	out there. The menu has the book's figures
 *	(書籍情報), the names of its objects (実身名一覧), each a panel of the
 *	data box put away with 〈確認〉, the book taken away again (書籍の削除)
 *	and whether to compress it. The window shows only what the archive
 *	holds; every part that is worked is a panel's.
 *
 *	A link put into an empty archive whose object is an archive itself
 *	(a file taken in) has its book copied in. An archive holds one book:
 *	a link put into one that has a book is refused. An object taken in
 *	from a .bpk file is an archive that has a book, the file kept as it
 *	was in its record of plain data, and is opened the same way.
 *
 *	Started with a T_ARCARG it does one piece of that at once without a
 *	window and says how it went on the console; with nothing it says what
 *	it is for and ends.
 */

#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/vobjsz.h>
#include <ts/sysdef.h>
#include <ts/obdrop.h>
#include <ts/obatr.h>
#include <ts/json.h>
#include <ts/ui.h>

#define WORK_W		520
#define WORK_H		300
#define TITLE_H		34
#define FRAME_W		16
#define TEXT_PX		16
#define LINE_H		24

#define M_CTRL		0x11

#define C_TEXT		0x00000000
#define C_DIM		0x00606060
#define C_BAR		0x004070C0
#define C_ERR		0x00C00000

/* What a link's frame shows (TAD_D_* of the window layer) */
#define D_NAME		0x0001
#define D_ROLE		0x0002
#define D_TYPE		0x0004
#define D_UPDATE	0x0008
#define D_FRAME		0x0010
#define D_PICT		0x0020

#define COL_NONE	0xFFFFFFFFU

/* What the window shows */
enum { S_EMPTY, S_FOUND, S_BUSY, S_BOOK, S_BROKEN };

/* The panels of the data box, and their parts */
#define PN_MAKE		2		/* the objects found: 〈作成〉 or 〈取消し〉 */
#define PN_INFO		3		/* 書籍情報 */
#define PN_LIST		4		/* 実身名一覧 */
#define PT_MAKE		11
#define PT_LIST		10
#define PT_OK		101

static TS_UUID	archive, win, ch;
static char	arcname[ARC_NAME_MAX];
static ID	kw, port;
static INT	gid = -1;
static INT	state;
static BOOL	closing, dirty = TRUE, store;
static INT	done_n, done_total;
static char	status[256];
static BOOL	status_err;
static UINT	ground = 0x00FFFFFF;
static UNPJOB	book;			/* the book, read */
static ARCJOB	job;			/* the objects found, to pack */
static ARCLOOK	droplook;		/* how the link put in looked */
static T_DTLOOK	look;			/* the book's 付箋: its outline carried, the link put */
static T_DPRECT	bookbox;
static UI	ui;			/* what the panels are asked over */

/* ---------------------------------------------------------------- saying things */

static void log_line( const char *s )
{
	char	line[400];

	snprintf(line, sizeof(line), "archive: %s\n", s);
	tm_putstring((const UB *)line);
}

/* A line on the window's foot, the system's message line and the console */
static void say( const char *s, BOOL err )
{
	strncpy(status, s, sizeof(status) - 1);
	status[sizeof(status) - 1] = 0;
	status_err = err;
	(void)wm_msg_put(s);
	log_line(s);
	dirty = TRUE;
}

/* 147780 as "147,780" */
static void grouped( UD v, char *out )
{
	char	d[24];
	INT	n = snprintf(d, sizeof(d), "%llu", (unsigned long long)v), i, k = 0;

	for ( i = 0; i < n; i++ ) {
		if ( i > 0 && ( n - i ) % 3 == 0 ) out[k++] = ',';
		out[k++] = d[i];
	}
	out[k] = 0;
}

/* A part of a whole in percent, with one decimal */
static void percent( UD part, UD whole, char *out )
{
	UD	t = ( whole > 0 ) ? ( part * 1000 + whole / 2 ) / whole : 0;

	snprintf(out, 16, "%llu.%llu%%", (unsigned long long)( t / 10 ), (unsigned long long)( t % 10 ));
}

/* An STIME as the local time, "2025/10/18 10:02:07" */
static void local_time( UINT stime, char *out )
{
	INT	off = 0;
	char	iso[24];

	(void)arc_now(&off);
	bpk_stime_iso((UINT)( (INT)stime + off ), iso);
	snprintf(out, 24, "%.4s/%.2s/%.2s %.8s", iso, iso + 5, iso + 8, iso + 11);
}

/* ---------------------------------------------------------------- the book */

static UINT colour_of( UINT raw )
{
	char	s[8];

	bpk_colour(raw, NULL, 0, s);
	if ( s[0] != '#' ) return COL_NONE;
	return (UINT)strtoul(s + 1, NULL, 16);
}

/*
 * The book's 付箋 looks as the archive says the root's link looks -- its
 * frame's size, letters, colours, what it shows -- and shows the book's
 * name; an archive that says nothing gets a frame wide enough for it.
 */
static void look_of_book( void )
{
	const BPKARC	*a = &book.arc;
	const char	*name = a->name[0] != 0 ? a->name : a->file[a->root].name;
	INT		w;

	memset(&look, 0, sizeof(look));
	if ( a->hasroot ) {
		look.w = a->rootview[2] - a->rootview[0];
		look.h = a->rootview[3] - a->rootview[1];
		look.chsz = bpk_chsize_pt(a->rootchsz);
		look.frcol = colour_of(a->rootcol[0]);
		look.chcol = colour_of(a->rootcol[1]);
		look.tbcol = colour_of(a->rootcol[2]);
		look.bgcol = colour_of(a->rootcol[3]);
		look.disp = D_FRAME | D_PICT | D_NAME;
		if ( a->rootattr & BPK_V_NONAME ) look.disp &= ~D_NAME;
		if ( !( a->rootattr & BPK_V_NORELN ) ) look.disp |= D_ROLE;
		if ( !( a->rootattr & BPK_V_NOTYPE ) ) look.disp |= D_TYPE;
		if ( !( a->rootattr & BPK_V_NOTIME ) ) look.disp |= D_UPDATE;
		if ( a->rootattr & BPK_V_NOPICT ) look.disp &= ~D_PICT;
		if ( a->rootattr & BPK_V_NOFDISP ) look.disp &= ~D_FRAME;
		look.autoopen = (UINT)( ( a->rootattr & BPK_V_AUTEXE ) != 0 );
	}
	w = dp_text_width((const UB *)name, 14) + 40;
	if ( !a->hasroot || look.w < 40 || look.h < 12 ) {
		look.w = 0;
		look.h = VOBJ_CLOSED_H(14, 1, 1);	/* closed, at 14 */
		look.chsz = 14;
		look.frcol = look.chcol = 0x00000000;
		look.tbcol = look.bgcol = 0x00FFFFFF;
		look.disp = 0;
	}
	if ( look.w < w ) look.w = ( w < 120 ) ? 120 : ( w > 480 ) ? 480 : w;
	if ( look.chsz <= 0 ) look.chsz = 14;
}

/* The archive object looked at again: empty, a book, or a book that cannot be read */
static void load( void )
{
	INT	recno = -1;
	char	s[256];

	unp_job_free(&book);
	memset(&book, 0, sizeof(book));
	if ( arc_book_rec(&archive, &recno, NULL, NULL) < E_OK ) {
		state = S_BROKEN;
		say("書庫の実身を読めません", TRUE);
		return;
	}
	if ( recno < 0 ) {
		state = S_EMPTY;
		return;
	}
	if ( unp_job_load(&book, &archive) < E_OK ) {
		state = S_BROKEN;
		snprintf(s, sizeof(s), "書籍を読めません：%s", book.err);
		say(s, TRUE);
		return;
	}
	state = S_BOOK;
	look_of_book();
	if ( !book.arc.crcok ) say("書籍のデータが壊れています（CRC が合いません）", TRUE);
}

/* ---------------------------------------------------------------- drawing */

static void work_size( INT *p_w, INT *p_h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	*p_w = WORK_W;
	*p_h = WORK_H;
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
		*p_w = wp.wright - wp.wleft;
		*p_h = wp.wbottom - wp.wtop;
	}
}

static void text( INT x, INT y, const char *s, UW colour )
{
	(void)dp_text(gid, x, y, (const UB *)s, colour, TEXT_PX);
}

/* The book's 付箋, its corner at (x, y) */
static void draw_book( INT x, INT y )
{
	T_DPRECT	r = { x, y, x + look.w, y + look.h };
	UINT		fr = ( look.frcol != COL_NONE ) ? look.frcol : 0x00000000;
	UINT		tb = ( look.tbcol != COL_NONE ) ? look.tbcol : 0x00FFFFFF;
	UINT		chc = ( look.chcol != COL_NONE ) ? look.chcol : 0x00000000;
	INT		px = ( look.chsz * 4 + 2 ) / 3;
	const char	*name = book.arc.name[0] != 0 ? book.arc.name : book.arc.file[book.arc.root].name;

	bookbox = r;
	(void)dp_fill_rect(gid, &r, tb);
	if ( look.disp == 0 || ( look.disp & D_FRAME ) ) (void)dp_frame_rect(gid, &r, fr, 1);
	if ( look.disp == 0 || ( look.disp & D_NAME ) ) {
		(void)dp_text(gid, x + 8, y + ( look.h + px ) / 2 - 2, (const UB *)name, chc, px);
	}
}

static void draw( void )
{
	T_DPRECT	all, bar;
	INT		w, h, y = 30, i;
	char		line[300], a[32], b[32];

	if ( gid < 0 ) return;
	work_size(&w, &h);
	all.left = 0;
	all.top = 0;
	all.right = w;
	all.bottom = h;
	(void)dp_fill_rect(gid, &all, ground);
	snprintf(line, sizeof(line), "書庫：%s", arcname);
	text(16, y, line, C_TEXT);
	y += LINE_H + 4;

	switch ( state ) {
	case S_EMPTY:
		text(16, y, "空の書庫です。", C_TEXT);
		y += LINE_H;
		text(16, y, "書籍にする実身の仮身をここに置くと、", C_DIM);
		y += LINE_H;
		text(16, y, "その実身から辿れる実身を探して書籍にまとめます。", C_DIM);
		y += LINE_H;
		snprintf(line, sizeof(line), "圧縮：%s", store ? "しない" : "LH5");
		text(16, y, line, C_DIM);
		break;
	case S_FOUND:
		grouped(job.total, a);
		snprintf(line, sizeof(line), "「%s」から辿れる実身：%d 個（%s バイト）", job.obj[0].name, job.nobj, a);
		text(16, y, line, C_TEXT);
		y += LINE_H;
		for ( i = 0; i < job.nobj && i < 4; i++ ) {
			snprintf(line, sizeof(line), "  %s", job.obj[i].name);
			text(16, y, line, C_DIM);
			y += LINE_H - 4;
		}
		if ( job.nobj > 4 ) {
			snprintf(line, sizeof(line), "  ほか %d 個", job.nobj - 4);
			text(16, y, line, C_DIM);
		}
		snprintf(line, sizeof(line), "圧縮：%s", store ? "しない" : "LH5");
		text(16, h - 40, line, C_DIM);
		break;
	case S_BUSY:
		text(16, y, "書籍を作っています", C_TEXT);
		y += LINE_H;
		if ( done_total > 0 ) {
			bar.left = 16;
			bar.top = y;
			bar.right = w - 16;
			bar.bottom = y + 10;
			(void)dp_frame_rect(gid, &bar, C_DIM, 1);
			bar.right = 16 + ( w - 32 ) * done_n / done_total;
			(void)dp_fill_rect(gid, &bar, C_BAR);
		}
		break;
	case S_BOOK:
		grouped(book.arc.origsize, a);
		grouped(book.arc.compsize, b);
		snprintf(line, sizeof(line), "実身 %u 個、%s バイト（書籍 %s バイト、%s）", book.arc.nfiles, a, b,
			 ( book.arc.method == BPK_LH5 ) ? "LH5" : "無圧縮");
		text(16, y, line, C_TEXT);
		y += 14;
		draw_book(16, y);
		y += look.h + 24;
		text(16, y, "書籍を置きたい所へ運ぶと、そこに取り出します", C_DIM);
		break;
	default:
		text(16, y, "書籍を読めません。書籍の削除で空の書庫にできます。", C_ERR);
		break;
	}
	text(16, h - 12, status, status_err ? C_ERR : C_TEXT);
	(void)wm_obj_flush(kw, NULL);
	dirty = FALSE;
}

/* Where the work area is on the screen, and the book's 付箋 in it, for the console */
static void log_place( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		s[160];

	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	if ( state == S_BOOK ) {
		snprintf(s, sizeof(s), "work at %d,%d, book at %d,%d-%d,%d", wp.wleft, wp.wtop, wp.wleft + bookbox.left,
			 wp.wtop + bookbox.top, wp.wleft + bookbox.right, wp.wtop + bookbox.bottom);
	} else {
		snprintf(s, sizeof(s), "work at %d,%d", wp.wleft, wp.wtop);
	}
	log_line(s);
}

/* ---------------------------------------------------------------- the work */

static void ask_make( void );

static void pack_tell( ARCJOB *j, INT n, INT total, const char *what )
{
	char	s[ARC_NAME_MAX + 64];

	done_n = n;
	done_total = total;
	snprintf(s, sizeof(s), "%s %d/%d", what, n, total);
	strncpy(status, s, sizeof(status) - 1);
	draw();
}

static void unpack_tell( UNPJOB *j, INT n, INT total )
{
	char	s[128];

	snprintf(s, sizeof(s), "取り出しています %d/%d", n, total);
	strncpy(status, s, sizeof(status) - 1);
	(void)wm_msg_put(s);
	draw();
}

/* What was found packed into the archive */
static void start( void )
{
	char	s[256], a[32], b[32];
	ER	er;

	state = S_BUSY;
	say("書籍を作っています", FALSE);
	draw();
	job.tell = pack_tell;
	er = arc_pack(&job, store ? BPK_LH0 : BPK_LH5, droplook.has ? &droplook : NULL);
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "書籍を作れません：%s", job.err);
		arc_free(&job);
		load();
		say(s, TRUE);
		return;
	}
	grouped(job.st.origsize, a);
	grouped(job.st.compsize, b);
	snprintf(s, sizeof(s), "packed %d objects, %s bytes into %s (%s), crc %04x", job.nobj, a, b,
		 store ? "stored" : "lh5", job.st.crc);
	log_line(s);
	snprintf(s, sizeof(s), "書籍「%s」を作りました（実身 %d 個、%s バイト）", job.obj[0].name, job.nobj, b);
	arc_free(&job);
	load();
	say(s, FALSE);
	draw();
	log_place();
}

/*
 * The book carried out from where the hand took it: the desktop follows
 * the hand, asks, and says where it went. There the objects are made and
 * the link to the root put.
 */
static void carry( INT x, INT y )
{
	TS_UUID	into;
	UINT	seq = 0;
	char	s[256], id[40];
	ER	er;

	if ( !book.arc.crcok ) {
		say("書籍のデータが壊れているので取り出せません", TRUE);
		return;
	}
	er = unp_carry(&book, &look, x - bookbox.left, y - bookbox.top, &into, &seq);
	snprintf(s, sizeof(s), "carried from %d,%d: %d", (INT)x, (INT)y, (INT)er);
	log_line(s);
	if ( er == E_OBJ || er == E_ABORT ) return;
	if ( er == E_NOEXS ) {
		say("取り出す所は、図形・仮身一覧・文章のウインドウです", FALSE);
		return;
	}
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "デスクトップに運ばせられません（%d）", (INT)er);
		say(s, TRUE);
		return;
	}
	state = S_BUSY;
	say("取り出しています", FALSE);
	draw();
	er = unp_job_run(&book, &into, unpack_tell);
	state = S_BOOK;
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "取り出せませんでした：%s", book.err);
		say(s, TRUE);
		return;
	}
	er = unp_place(&book, &look, seq);
	if ( er < E_OK ) {
		unp_job_undo(&book);
		snprintf(s, sizeof(s), "仮身を置けませんでした（%d）。取り出した実身は消しました", (INT)er);
		say(s, TRUE);
		return;
	}
	unp_uuid_str(&book.root, id);
	snprintf(s, sizeof(s), "unpacked %d objects, root %s, placed", book.nmade, id);
	log_line(s);
	snprintf(s, sizeof(s), "%d 個の実身を取り出しました（%s）", book.nmade, book.arc.file[book.arc.root].name);
	say(s, FALSE);
}

/* The drop answered: taken or not, and what the one who dropped it is told */
static void drop_answer( const T_OBDROP *d, UINT answer, const char *msg )
{
	T_OBDRANS	a;
	SZ		asz = 0;

	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	a.answer = answer;
	strncpy((char *)a.msg, msg, sizeof(a.msg) - 1);
	(void)ob_wri_rec(kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	log_line(( answer == OB_DR_ACCEPT ) ? "drop taken" : "drop refused");
}

/* How the link put in looked, as the archive keeps it for the root */
static void look_of_drop( const T_OBDROPV *v )
{
	memset(&droplook, 0, sizeof(droplook));
	if ( v->right - v->left < 40 || v->bottom - v->top < 12 ) return;
	droplook.has = TRUE;
	droplook.box[0] = v->left;
	droplook.box[1] = v->top;
	droplook.box[2] = v->right;
	droplook.box[3] = v->bottom;
	droplook.chsz = ( v->chsz > 0 ) ? v->chsz : 14;
	droplook.col[0] = v->frcol;
	droplook.col[1] = v->chcol;
	droplook.col[2] = v->tbcol;
	droplook.col[3] = v->bgcol;
	if ( v->disp == 0 ) {
		droplook.attr = BPK_V_NORELN | BPK_V_NOTYPE | BPK_V_NOTIME;
		return;
	}
	if ( !( v->disp & D_NAME ) ) droplook.attr |= BPK_V_NONAME;
	if ( !( v->disp & D_ROLE ) ) droplook.attr |= BPK_V_NORELN;
	if ( !( v->disp & D_TYPE ) ) droplook.attr |= BPK_V_NOTYPE;
	if ( !( v->disp & D_UPDATE ) ) droplook.attr |= BPK_V_NOTIME;
	if ( !( v->disp & D_PICT ) ) droplook.attr |= BPK_V_NOPICT;
	if ( !( v->disp & D_FRAME ) ) droplook.attr |= BPK_V_NOFDISP;
}

/*
 * A link put into the window. An empty archive takes it: the objects it
 * reaches are found and listed for 開始, or, when it is an archive with
 * a book, that book is copied in. Anything else refuses it.
 */
static void dropped( void )
{
	T_OBDROP	*d = ob_drop_read(kw);
	char		s[300], id[40];
	INT		recno = -1;
	UB		*raw = NULL;
	UINT		len = 0;
	ER		er;

	if ( d == NULL ) return;
	if ( d->n <= 0 ) {
		free(d);
		return;
	}
	unp_uuid_str(&d->v[0].target, id);
	snprintf(s, sizeof(s), "drop %d first %s", d->n, id);
	log_line(s);
	if ( state == S_BUSY ) {
		drop_answer(d, OB_DR_REFUSE, "書庫管理は作業中です");
		free(d);
		return;
	}
	if ( state != S_EMPTY && state != S_FOUND ) {
		drop_answer(d, OB_DR_REFUSE, "書庫には書籍が一つしか入りません");
		say("書籍は一つだけです。先に書籍の削除をしてください", TRUE);
		free(d);
		return;
	}
	if ( memcmp(&d->v[0].target, &archive, sizeof(archive)) == 0 ) {
		drop_answer(d, OB_DR_REFUSE, "書庫の実身そのものは書籍にできません");
		free(d);
		return;
	}
	(void)arc_book_rec(&d->v[0].target, &recno, &raw, &len);
	if ( recno >= 0 && raw != NULL ) {
		/* an archive: its book copied in */
		drop_answer(d, OB_DR_ACCEPT, "書庫の書籍を写します");
		er = arc_book_put(&archive, raw, len);
		free(raw);
		arc_free(&job);
		load();
		if ( er < E_OK ) {
			snprintf(s, sizeof(s), "書籍を写せません（%d）", (INT)er);
			say(s, TRUE);
		} else {
			log_line("copied a book");
			say("書庫の書籍を写しました", FALSE);
		}
		free(d);
		return;
	}
	free(raw);
	drop_answer(d, OB_DR_ACCEPT, "書籍にする実身を探します");
	look_of_drop(&d->v[0]);
	state = S_BUSY;
	say("実身を探しています", FALSE);
	draw();
	job.tell = pack_tell;
	er = arc_find(&job, &archive, &d->v[0].target);
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "実身を探せません：%s", job.err);
		arc_free(&job);
		state = S_EMPTY;
		say(s, TRUE);
	} else {
		state = S_FOUND;
		snprintf(s, sizeof(s), "found %d objects", job.nobj);
		log_line(s);
		say("作成で書籍を作ります", FALSE);
	}
	free(d);
	if ( state == S_FOUND ) ask_make();
}

static void delete_book( void )
{
	ER	er;

	if ( state != S_BOOK && state != S_BROKEN ) return;
	er = arc_book_put(&archive, NULL, 0);
	load();
	if ( er < E_OK ) say("書籍を削除できません", TRUE);
	else {
		log_line("deleted the book");
		say("書籍を削除しました。空の書庫です", FALSE);
	}
}

/* ---------------------------------------------------------------- the window */

/* The panels are asked over the window, as large as its work area is now */
static void ui_place( void )
{
	work_size(&ui.w, &ui.h);
}

/*
 * The objects found, listed in a panel that asks: 〈作成〉 (or Enter)
 * packs them, 〈取消し〉 (or Esc) leaves the archive empty.
 */
static void ask_make( void )
{
	char		l[6][ARC_NAME_MAX + 96], a[32];
	const char	*lines[6];
	INT		i, n = 0, answer;

	draw();
	grouped(job.total, a);
	snprintf(l[n++], sizeof(l[0]), "「%s」から辿れる実身：%d 個（%s バイト）", job.obj[0].name, job.nobj, a);
	for ( i = 0; i < job.nobj && n < 5; i++ ) {
		if ( n == 4 && job.nobj > 4 ) {
			snprintf(l[n++], sizeof(l[0]), "  ほか %d 個", job.nobj - 3);
			break;
		}
		snprintf(l[n++], sizeof(l[0]), "  %s", job.obj[i].name);
	}
	snprintf(l[n++], sizeof(l[0]), "圧縮：%s", store ? "しない" : "LH5");
	for ( i = 0; i < 6; i++ ) lines[i] = ( i < n ) ? l[i] : " ";
	ui_place();
	log_line("asked to make");
	answer = ui_ask(&ui, PN_MAKE, lines, 6);
	if ( answer == PT_MAKE ) {
		start();
	} else {
		arc_free(&job);
		state = S_EMPTY;
		say("取り消しました", FALSE);
	}
}

/* 書籍情報: the book's figures in a panel, put away with 〈確認〉 */
static void show_info( void )
{
	const BPKARC	*a = &book.arc;
	char		l[7][ARC_NAME_MAX + 96], v[32], p[16];
	const char	*lines[7];
	INT		i;

	if ( state != S_BOOK ) return;
	snprintf(l[0], sizeof(l[0]), "書籍名：%s", a->name);
	local_time(a->head.time, v);
	snprintf(l[1], sizeof(l[0]), "書籍作成日時：%s", v);
	grouped(a->head.filesize, v);
	snprintf(l[2], sizeof(l[0]), "実身総サイズ：%s バイト（%u 個）", v, a->nfiles);
	grouped(a->origsize, v);
	percent(a->origsize, a->head.filesize, p);
	snprintf(l[3], sizeof(l[0]), "パックサイズ：%s バイト（%s）", v, p);
	grouped(a->compsize, v);
	percent(a->compsize, a->head.filesize, p);
	snprintf(l[4], sizeof(l[0]), "書籍サイズ：%s バイト（%s）", v, p);
	snprintf(l[5], sizeof(l[0]), "圧縮方式：%s", ( a->method == BPK_LH5 ) ? "LH5" : "無圧縮");
	snprintf(l[6], sizeof(l[0]), "CRC：%s", a->crcok ? "一致" : "不一致");
	for ( i = 0; i < 7; i++ ) lines[i] = l[i];
	ui_place();
	log_line("info shown");
	(void)ui_ask(&ui, PN_INFO, lines, 7);
	log_line("info put away");
	dirty = TRUE;
}

/* 実身名一覧: the names of the book's objects in the list of a panel, put away with 〈確認〉 */
static void show_list( void )
{
	const BPKARC	*a = &book.arc;
	UI		q;
	T_OBNTM		m;
	char		head[80], **names;
	INT		i, n = (INT)a->nfiles, ev, num = 0;
	BOOL		done = FALSE;

	if ( state != S_BOOK ) return;
	names = (char **)calloc((size_t)( ( n > 0 ) ? n : 1 ), sizeof(char *));
	if ( names == NULL ) return;
	for ( i = 0; i < n; i++ ) {
		size_t	len = strlen(a->file[i].name) + 16;

		names[i] = (char *)malloc(len);
		if ( names[i] != NULL ) snprintf(names[i], len, "%d\t%s", i + 1, a->file[i].name);
	}
	ui_place();
	if ( ui_dlg_open(&ui, PN_LIST, &q) >= E_OK ) {
		snprintf(head, sizeof(head), "書籍の実身：%d 個", n);
		ui_set_text(&q, 1, head);
		ui_names(&q, PT_LIST, (const char *const *)names, n);
		log_line("list shown");
		while ( !done && !closing ) {
			if ( ui_wait(&ui, &m, TMO_FEVR) < E_OK ) {
				closing = TRUE;
				break;
			}
			ev = ui_dlg_take(&q, &m, &num);
			if ( ev == UI_EV_CLOSE || ( ev == UI_EV_BUTTON && num == PT_OK ) ) {
				done = TRUE;
			} else if ( ev == UI_EV_OTHER && ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) ) {
				closing = TRUE;
			}
		}
		ui_dlg_close(&q);
		log_line("list put away");
	}
	for ( i = 0; i < n; i++ ) free(names[i]);
	free(names);
	dirty = TRUE;
}

static void menu( const T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;
	BOOL	has = (BOOL)( state == S_BOOK );

	if ( !unp_uuid_parse(SYSDEF_MENU_ARCHIVE, &def) || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "info", has ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "list", has ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "delete", ( state == S_BOOK || state == S_BROKEN ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "store", store ? MN_TICK : 0);
	chosen = (BOOL)( mn_pop_men(mid, kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( strcmp((const char *)sel.code, "close") == 0 ) closing = TRUE;
	else if ( strcmp((const char *)sel.code, "info") == 0 ) show_info();
	else if ( strcmp((const char *)sel.code, "list") == 0 ) show_list();
	else if ( strcmp((const char *)sel.code, "delete") == 0 ) delete_book();
	else if ( strcmp((const char *)sel.code, "store") == 0 ) {
		store = (BOOL)!store;
		say(store ? "圧縮しないで書籍を作ります" : "LH5 で圧縮して書籍を作ります", FALSE);
	}
}

/* A press in the window: on the book's 付箋, the book carried out */
static void press( const T_OBNTM *m )
{
	if ( state == S_BOOK && m->x >= bookbox.left && m->x < bookbox.right && m->y >= bookbox.top
	     && m->y < bookbox.bottom ) {
		carry(m->x, m->y);
	}
}

static void key( const T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);

	if ( m->mods & M_CTRL ) {
		if ( c == 'e' || c == 'E' ) closing = TRUE;
		else if ( c == 'i' || c == 'I' ) show_info();
		else if ( c == 'l' || c == 'L' ) show_list();
	}
}

static void event( const T_OBNTM *m )
{
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(m);
		else if ( m->code == 0 ) press(m);
		break;
	case OB_E_KEY:
		key(m);
		break;
	case OB_E_DROP:
		dropped();
		break;
	case OB_E_REDRAW:
		dirty = TRUE;
		break;
	default:
		break;
	}
}

/* The archive object's name, and the colour its window is to have */
static void read_archive( void )
{
	UB		*meta = NULL;
	T_JSON		root, w;
	UB		col[16];
	INT		asz;
	ID		k = ob_opn_obj(&archive, OB_OP_ATRRD);

	strncpy(arcname, "書庫管理", sizeof(arcname) - 1);
	if ( k <= 0 ) return;
	asz = ob_atr_read(k, 0, &meta);
	if ( asz >= 0 && js_parse(meta, asz, &root) >= E_OK ) {
		(void)js_get_str(&root, "name", (UB *)arcname, sizeof(arcname));
		if ( js_get(&root, "window", &w) >= E_OK && js_get_str(&w, "backgroundColor", col, sizeof(col)) > 0
		  && col[0] == '#' ) {
			ground = (UINT)strtoul((const char *)col + 1, NULL, 16) & 0xFFFFFF;
		}
	}
	free(meta);
	ob_cls_obj(k);
}

static ER open_window( void )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[192], id[40];

	unp_uuid_str(&archive, id);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7,\"shows\":\"%s\"}",
		 200, 150, 200 + WORK_W + FRAME_W, 150 + WORK_H + TITLE_H, id);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (const UB *)arcname;
	c.json = (const UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &win) < E_OK ) return E_OBJ;
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE | OB_E_DROP;
	if ( kw <= 0 || gid < 0 || port <= 0 || ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) return E_OBJ;

	/* the frame kept as it is: the work area made the size wanted */
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + WORK_W + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + WORK_H + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}

	/* what the panels are asked over: the data box, this window and its channel */
	memset(&ui, 0, sizeof(ui));
	if ( !unp_uuid_parse(SYSDEF_BOX_ARCHIVE, &ui.box) ) return E_OBJ;
	ui.kw = kw;
	ui.port = port;
	ui.win = win;
	ui_place();
	return E_OK;
}

static void close_window( void )
{
	if ( port > 0 ) {
		ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}
	if ( kw > 0 ) ob_cls_obj(kw);
	(void)ob_del_obj(&win);
	port = kw = 0;
	gid = -1;
}

/* ---------------------------------------------------------------- work without the window */

static void batch_tell( ARCJOB *j, INT n, INT total, const char *what )
{
}

static INT batch( const T_ARCARG *a )
{
	char	s[300], x[32], y[32], id[40];
	INT	recno = -1, i;
	UB	*raw = NULL;
	UINT	len = 0;
	ER	er;

	archive = a->archive;
	switch ( a->op ) {
	case ARC_OP_PACK:
		if ( arc_book_rec(&archive, &recno, NULL, NULL) < E_OK ) {
			log_line("error: no archive object");
			return 3;
		}
		if ( recno >= 0 ) {
			log_line("error: the archive has a book already");
			return 4;
		}
		job.tell = batch_tell;
		er = arc_find(&job, &archive, &a->obj);
		if ( er >= E_OK ) er = arc_pack(&job, ( a->method == BPK_LH0 ) ? BPK_LH0 : BPK_LH5, NULL);
		if ( er < E_OK ) {
			snprintf(s, sizeof(s), "error: %s", job.err);
			log_line(s);
			arc_free(&job);
			return 5;
		}
		snprintf(s, sizeof(s), "packed %d objects, %u bytes into %u (%s), crc %04x, files %u", job.nobj,
			 job.st.origsize, job.st.compsize, ( a->method == BPK_LH0 ) ? "stored" : "lh5", job.st.crc,
			 job.st.filesize);
		log_line(s);
		arc_free(&job);
		return 0;
	case ARC_OP_DELETE:
		er = arc_book_put(&archive, NULL, 0);
		log_line(( er >= E_OK ) ? "deleted the book" : "error: cannot delete");
		return ( er >= E_OK ) ? 0 : 5;
	case ARC_OP_COPY:
		if ( arc_book_rec(&archive, &recno, NULL, NULL) < E_OK || recno >= 0 ) {
			log_line("error: the archive is not empty");
			return 4;
		}
		(void)arc_book_rec(&a->obj, &recno, &raw, &len);
		if ( raw == NULL ) {
			log_line("error: no book to copy");
			return 5;
		}
		er = arc_book_put(&archive, raw, len);
		free(raw);
		log_line(( er >= E_OK ) ? "copied a book" : "error: cannot copy");
		return ( er >= E_OK ) ? 0 : 5;
	default:
		break;
	}

	/* the rest read the book */
	if ( arc_book_rec(&archive, &recno, NULL, NULL) < E_OK ) {
		log_line("error: no archive object");
		return 3;
	}
	if ( recno < 0 ) {
		log_line("empty");
		return 2;
	}
	if ( unp_job_load(&book, &archive) < E_OK ) {
		snprintf(s, sizeof(s), "error: %s", book.err);
		log_line(s);
		return 6;
	}
	switch ( a->op ) {
	case ARC_OP_LIST:
		snprintf(s, sizeof(s), "list %u", book.arc.nfiles);
		log_line(s);
		for ( i = 0; i < (INT)book.arc.nfiles; i++ ) {
			snprintf(s, sizeof(s), "object %d %s", i, book.arc.file[i].name);
			log_line(s);
		}
		break;
	case ARC_OP_INFO:
		local_time(book.arc.head.time, x);
		snprintf(s, sizeof(s), "info name %s, %u objects, files %u, orig %u, comp %u, %s, crc %s, made %s",
			 book.arc.name, book.arc.nfiles, book.arc.head.filesize, book.arc.origsize, book.arc.compsize,
			 ( book.arc.method == BPK_LH5 ) ? "lh5" : "stored", book.arc.crcok ? "ok" : "bad", x);
		log_line(s);
		break;
	case ARC_OP_UNPACK:
		if ( !book.arc.crcok ) {
			log_line("error: the book's CRC is wrong");
			unp_job_free(&book);
			return 7;
		}
		er = unp_job_run(&book, &a->obj, NULL);
		if ( er >= E_OK ) er = arc_link_into(&a->obj, &book.root, book.arc.file[book.arc.root].name);
		if ( er < E_OK ) {
			unp_job_undo(&book);
			snprintf(s, sizeof(s), "error: %s (%d)", book.err, (INT)er);
			log_line(s);
			unp_job_free(&book);
			return 5;
		}
		unp_uuid_str(&book.root, id);
		grouped(book.arc.origsize, y);
		snprintf(s, sizeof(s), "unpacked %d objects, root %s, %s bytes", book.nmade, id, y);
		log_line(s);
		break;
	default:
		log_line("error: what to do");
		unp_job_free(&book);
		return 2;
	}
	unp_job_free(&book);
	return 0;
}

int main( void )
{
	static T_ARCARG	a;
	T_OBNTM		m;
	SZ		n, asz = 0;

	n = ts_get_arg(&a, sizeof(a));
	if ( n == (SZ)sizeof(a) && a.magic == ARC_ARG_MAGIC ) return batch(&a);
	if ( n < (SZ)sizeof(TS_UUID) ) {
		(void)wm_msg_put("書庫管理：書庫の実身を開き、書籍にする実身の仮身を置くと書籍を作ります");
		log_line("started without an archive");
		return 0;
	}
	memcpy(&archive, &a, sizeof(archive));
	read_archive();
	load();
	if ( open_window() < E_OK ) {
		log_line("no window");
		close_window();
		unp_job_free(&book);
		return 3;
	}
	{
		char	s[128];

		snprintf(s, sizeof(s), "ready %s", ( state == S_EMPTY ) ? "empty" : ( state == S_BOOK ) ? "book" : "broken");
		log_line(s);
		if ( state == S_BOOK ) {
			snprintf(s, sizeof(s), "book %u objects", book.arc.nfiles);
			log_line(s);
		}
	}
	draw();
	log_place();
	/* the window's channel waited on: the first notice of a round for as long as it takes, the rest as they are */
	while ( !closing ) {
		for ( TMO tmo = TMO_FEVR; !closing && ob_rea_tmo(port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK;
		      tmo = TMO_POL ) {
			event(&m);
		}
		if ( dirty ) draw();
	}
	close_window();
	(void)wm_msg_put(NULL);
	arc_free(&job);
	unp_job_free(&book);
	return 0;
}
