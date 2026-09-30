/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	unp_main.c
 *	the program (design 17.16)
 *
 *	It is started with the UUID of an archive object as its argument --
 *	an object whose applist opens it, with the archive's bytes in a
 *	record of plain data. It reads the archive at once and shows in its
 *	window what is in it -- the archive's name, how many objects there
 *	are and how big they are -- and the root object as a virtual object,
 *	looking as the archive says its link looks. Carried out of the
 *	window and let go over a window that takes links (a figure, a
 *	cabinet, a text), the archive is taken out there, after the desktop
 *	has asked whether to: the objects are made on that window's volume
 *	and the link to the root is put where it was let go. It can be done
 *	again: each time makes a set of its own. How far it is and what came
 *	of it is shown in the window and on the message line; anything that
 *	goes wrong is said in both places and on the console.
 *
 *	Started with no argument (the program object opened itself) it says
 *	what it is for and ends.
 */

#include "unp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/sysdef.h>

#define WORK_W		480
#define WORK_H		260
#define TITLE_H		34
#define FRAME_W		16
#define TEXT_PX		16
#define LINE_H		26

#define M_CTRL		0x11

#define C_TEXT		0x00000000
#define C_DIM		0x00606060
#define C_GROUND	0x00FFFFFF
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

enum { S_READY, S_BUSY, S_FAILED };

static UNPJOB	job;
static TS_UUID	win, ch;
static ID	kw, port;
static INT	gid = -1;
static INT	state;
static BOOL	loaded, closing, dirty = TRUE;
static INT	done_n, done_total;
static char	status[256];
static T_DTLOOK	look;			/* the root's link: its outline carried, the link put */
static T_DPRECT	rootbox;		/* where the window shows it */

/* ---------------------------------------------------------------- saying things */

/* A line on the window's status, the system's message line and the console */
static void say( const char *s )
{
	char	line[300];

	strncpy(status, s, sizeof(status) - 1);
	status[sizeof(status) - 1] = 0;
	(void)wm_msg_put(s);
	snprintf(line, sizeof(line), "unpack: %s\n", s);
	tm_putstring((const UB *)line);
	dirty = TRUE;
}

/* 147780 as "147,780" */
static void grouped( UINT v, char *out )
{
	char	d[16];
	INT	n = snprintf(d, sizeof(d), "%u", v), i, k = 0;

	for ( i = 0; i < n; i++ ) {
		if ( i > 0 && ( n - i ) % 3 == 0 ) out[k++] = ',';
		out[k++] = d[i];
	}
	out[k] = 0;
}

/* ---------------------------------------------------------------- the root's link */

/* A COLOR of the archive as 0x00rrggbb, or COL_NONE when it is none */
static UINT colour_of( UINT raw )
{
	char	s[8];

	bpk_colour(raw, NULL, 0, s);
	if ( s[0] != '#' ) return COL_NONE;
	return (UINT)strtoul(s + 1, NULL, 16);
}

/*
 * How the root's link looks: as the archive's extension describes it --
 * its frame's size, its letters, colours and what the frame shows --
 * or, when it says nothing, a frame wide enough for the root's name.
 */
static void look_of_root( void )
{
	const BPKARC	*a = &job.arc;
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
	if ( !a->hasroot || look.w < 40 || look.h < 12 ) {
		w = dp_text_width((const UB *)a->file[a->root].name, 14) + 40;
		look.w = ( w < 120 ) ? 120 : ( w > 480 ) ? 480 : w;
		look.h = 25;
		look.chsz = 14;
		look.frcol = look.chcol = 0x00000000;
		look.tbcol = look.bgcol = 0x00FFFFFF;
		look.disp = 0;
	}
	if ( look.chsz <= 0 ) look.chsz = 14;
}

/* The root drawn as its link looks, its corner at (x, y) */
static void draw_root( INT x, INT y )
{
	T_DPRECT	r = { x, y, x + look.w, y + look.h };
	UINT		fr = ( look.frcol != COL_NONE ) ? look.frcol : 0x00000000;
	UINT		tb = ( look.tbcol != COL_NONE ) ? look.tbcol : 0x00FFFFFF;
	UINT		chc = ( look.chcol != COL_NONE ) ? look.chcol : 0x00000000;
	INT		px = ( look.chsz * 4 + 2 ) / 3;		/* points to screen pixels */

	rootbox = r;
	(void)dp_fill_rect(gid, &r, tb);
	if ( look.disp == 0 || ( look.disp & D_FRAME ) ) (void)dp_frame_rect(gid, &r, fr, 1);
	if ( look.disp == 0 || ( look.disp & D_NAME ) ) {
		(void)dp_text(gid, x + 8, y + ( look.h + px ) / 2 - 2,
			      (const UB *)job.arc.file[job.arc.root].name, chc, px);
	}
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

static void draw( void )
{
	T_DPRECT	all, bar;
	INT		w, h, y = 30;
	char		line[256], a[24], b[24];
	const BPKARC	*ar = &job.arc;

	if ( gid < 0 ) return;
	work_size(&w, &h);
	all.left = 0;
	all.top = 0;
	all.right = w;
	all.bottom = h;
	(void)dp_fill_rect(gid, &all, C_GROUND);

	snprintf(line, sizeof(line), "書庫：%s", job.name[0] != 0 ? job.name : ar->name);
	text(16, y, line, C_TEXT);
	y += LINE_H;
	if ( loaded ) {
		snprintf(line, sizeof(line), "実身の数：%u", ar->nfiles);
		text(16, y, line, C_TEXT);
		y += LINE_H;
		grouped(ar->origsize, a);
		grouped(ar->compsize, b);
		snprintf(line, sizeof(line), "大きさ：%s バイト（圧縮 %s バイト、%s）", a, b,
			 ( ar->method == 5 ) ? "LH5" : "無圧縮");
		text(16, y, line, C_TEXT);
		y += 14;

		/* the root, to be carried to where it is to be taken out */
		draw_root(16, y);
		y += look.h + 22;
		text(16, y, "ルート実身の仮身を置きたい所へ運ぶと、そこに解凍します", C_DIM);
	} else {
		text(16, y, "書庫を読めませんでした。", C_ERR);
	}
	y += 12;

	if ( state == S_BUSY && done_total > 0 ) {
		bar.left = 16;
		bar.top = y;
		bar.right = w - 16;
		bar.bottom = y + 10;
		(void)dp_frame_rect(gid, &bar, C_DIM, 1);
		bar.right = 16 + ( w - 32 ) * done_n / done_total;
		(void)dp_fill_rect(gid, &bar, C_BAR);
	}
	y += 16 + LINE_H - 8;
	text(16, y, status, ( state == S_FAILED ) ? C_ERR : C_TEXT);
	(void)wm_obj_flush(kw, NULL);
	dirty = FALSE;
}

/* ---------------------------------------------------------------- the work */

static void progress( UNPJOB *j, INT n, INT total )
{
	char	s[128];
	INT	half = total / 2;

	done_n = n;
	done_total = total;
	if ( n <= half ) snprintf(s, sizeof(s), "実身を作っています %d/%d", n, half);
	else snprintf(s, sizeof(s), "レコードを書いています %d/%d", n - half, half);
	strncpy(status, s, sizeof(status) - 1);
	(void)wm_msg_put(s);
	draw();
}

/*
 * The root carried out from where the hand took it: the desktop follows
 * the hand, asks, and says where it went. There the archive is taken
 * out and the link to the root put.
 */
static void carry( INT x, INT y )
{
	TS_UUID	into;
	UINT	seq = 0;
	char	s[256];
	INT	i, n;
	ER	er;

	er = unp_carry(&job, &look, x - rootbox.left, y - rootbox.top, &into, &seq);
	snprintf(s, sizeof(s), "unpack: carried from %d,%d: %d\n", (INT)x, (INT)y, (INT)er);
	tm_putstring((const UB *)s);
	if ( er == E_OBJ || er == E_ABORT ) return;	/* a click, or not wanted */
	if ( er == E_NOEXS ) {
		say("解凍する所は、図形・仮身一覧・文章の窓です");
		return;
	}
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "デスクトップに運ばせられません（%d）", (INT)er);
		say(s);
		return;
	}
	state = S_BUSY;
	say("解凍しています");
	draw();
	er = unp_job_run(&job, &into, progress);
	if ( er < E_OK ) {
		state = S_FAILED;
		snprintf(s, sizeof(s), "解凍できませんでした：%s", job.err);
		say(s);
		return;
	}
	er = unp_place(&job, &look, seq);
	if ( er < E_OK ) {
		unp_job_undo(&job);
		state = S_FAILED;
		snprintf(s, sizeof(s), "仮身を置けませんでした（%d）。取り出した実身は消しました", (INT)er);
		say(s);
		return;
	}
	state = S_READY;

	/* what was kept only as <tadseg>, for whoever reads the console */
	n = snprintf(s, sizeof(s), "unpack: kept %u segments", job.nskipped);
	for ( i = 0; i < job.nskip && n < (INT)sizeof(s) - 16; i++ ) {
		n += snprintf(s + n, sizeof(s) - (size_t)n, " %x:%u", job.skip[i].id, job.skip[i].count);
	}
	snprintf(s + n, sizeof(s) - (size_t)n, "\n");
	tm_putstring((const UB *)s);
	{
		char	id[40];

		unp_uuid_str(&job.root, id);
		snprintf(s, sizeof(s), "unpack: done %d objects, root %s, %d pictures, %d without icon\n",
			 job.nmade, id, job.npic, job.noicon);
		tm_putstring((const UB *)s);
	}
	tm_putstring((const UB *)"unpack: root placed on the desktop\n");
	snprintf(s, sizeof(s), "%d 個の実身を取り出しました（%s）", job.nmade, job.arc.file[job.arc.root].name);
	say(s);
}

/* ---------------------------------------------------------------- the window */

static void menu( const T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( !unp_uuid_parse(SYSDEF_MENU_UNPACK, &def) || mn_cre_men(&def, &mid) < E_OK ) return;
	chosen = (BOOL)( mn_pop_men(mid, kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( strcmp((const char *)sel.code, "close") == 0 ) closing = TRUE;
}

static BOOL in_root( INT x, INT y )
{
	return (BOOL)( loaded && x >= rootbox.left && x < rootbox.right
		       && y >= rootbox.top && y < rootbox.bottom );
}

static void event( const T_OBNTM *m )
{
	UB	c;

	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) {
			menu(m);
		} else if ( m->code == 0 && state != S_BUSY && in_root(m->x, m->y) ) {
			carry(m->x, m->y);
		} else if ( m->code == 0 ) {
			/* why a press took nothing, for the console */
			char	line[120];

			snprintf(line, sizeof(line), "unpack: press at %d,%d, root %d,%d-%d,%d, %s\n",
				 (INT)m->x, (INT)m->y, (INT)rootbox.left, (INT)rootbox.top, (INT)rootbox.right,
				 (INT)rootbox.bottom, !loaded ? "not read" : state == S_BUSY ? "busy" : "off the root");
			tm_putstring((const UB *)line);
		}
		break;
	case OB_E_KEY:
		c = wm_key_char(m->code, m->mods);
		if ( ( m->mods & M_CTRL ) && ( c == 'e' || c == 'E' ) ) closing = TRUE;
		break;
	case OB_E_REDRAW:
		dirty = TRUE;
		break;
	default:
		break;
	}
}

static ER open_window( const char *title )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[192], id[40];

	unp_uuid_str(&job.archive, id);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7,\"shows\":\"%s\"}",
		 180, 140, 180 + WORK_W + FRAME_W, 140 + WORK_H + TITLE_H, id);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (const UB *)title;
	c.json = (const UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &win) < E_OK ) return E_OBJ;
	kw = ob_opn_obj(&win, OB_OP_ALL);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE;
	if ( kw <= 0 || gid < 0 || port <= 0 || ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) return E_OBJ;

	/* the frame kept as it is: the work area made the size wanted */
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + WORK_W + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + WORK_H + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
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

int main( void )
{
	TS_UUID	arc;
	T_OBNTM	m;
	SZ	asz = 0;
	char	s[256];
	ER	er;

	if ( ts_get_arg(&arc, sizeof(arc)) < (SZ)sizeof(arc) ) {
		(void)wm_msg_put("書庫解凍：書庫の実身を開き、ルート実身の仮身を置きたい所へ運ぶと解凍します");
		tm_putstring((const UB *)"unpack: started without an archive\n");
		return 0;
	}
	er = unp_job_load(&job, &arc);
	loaded = (BOOL)( er >= E_OK );
	state = loaded ? S_READY : S_FAILED;
	if ( loaded ) look_of_root();
	if ( open_window(job.name[0] != 0 ? job.name : "書庫解凍") < E_OK ) {
		tm_putstring((const UB *)"unpack: no window\n");
		close_window();
		unp_job_free(&job);
		return 3;
	}
	if ( !loaded ) {
		snprintf(s, sizeof(s), "書庫を読めません：%s", job.err);
		say(s);
	} else {
		snprintf(s, sizeof(s), "unpack: ready %u objects, root %dx%d%s\n", job.arc.nfiles,
			 look.w, look.h, job.arc.hasroot ? " as the archive says" : "");
		tm_putstring((const UB *)s);
		status[0] = 0;
	}
	draw();

	while ( !closing ) {
		while ( !closing && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) event(&m);
		if ( dirty ) draw();
		if ( !closing ) (void)tk_dly_tsk(20);
	}
	close_window();
	(void)wm_msg_put(NULL);
	unp_job_free(&job);
	return 0;
}
