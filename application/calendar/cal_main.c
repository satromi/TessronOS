/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cal_main.c
 *	予定表: a month on a grid, a day's plans in its cells (design 11.10)
 *
 *	The program is started on a 予定表 object and shows one month of it
 *	at a time. Along the top of the window is a strip of parts -- the
 *	push buttons 前月, 今日 and 翌月, the month shown, which asks for a
 *	year and month when pushed, and a line that says what was done --
 *	from panel 1 of the program's data box; the window manager draws and
 *	works them. Under the strip the program draws the days of the week
 *	and six rows of seven cells, and over them the links of the month
 *	object: a day's link in its cell, any other where it was put. The
 *	links are drawn by the kernel from a figure made of them, so they
 *	look as links look everywhere else, opened ones with what they show.
 *
 *	A day's plans are an object of its own, named "YYYY/MM/DD", made
 *	from the 年月日実身 template when a rectangle is pulled out in the
 *	day's cell (or the cell is pressed twice). A press on a link takes
 *	it (with Shift or Ctrl, adds it to or takes it from what is taken);
 *	a second press opens it with its own program. A link that is not a
 *	day's is moved by carrying it and sized by its right and lower
 *	edges; carried out of the window, what is taken is moved into the
 *	window of the desktop's it is let go over, copied when the second
 *	button went down on the way (複写ドラッグ), and a link pressed twice
 *	and carried out puts a link to a copy of its object there
 *	(実身複製ドラッグ). Links dropped on the window are put in the month
 *	where they were let go.
 *
 *	The edit menu works through the tray: what is taken is copied or
 *	moved to it as a figure of links, and a figure's links are copied or
 *	moved from it into the month. Every change of the month can be taken
 *	back (取消), one after another. The 仮身操作 and 実身操作 of a link
 *	are the desktop's, asked for (ts/dtreq.h): the desktop shows its
 *	panels over this window and answers what the link is to become.
 *
 *	The month object is made when the month first has a link, and the
 *	month's record is written again, grid and all, every time its links
 *	change; nothing waits to be saved. What was done is said on the
 *	message line and, for the tests, on the console ("calendar: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/dtreq.h>
#include <ts/uiprn.h>
#include <ts/hid.h>
#include <ts/tray.h>
#include <ts/tadlnk.h>
#include "cal_tad.h"

#define TICK_MS		20
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */
#define BAND_MIN	10			/* a rectangle this big each way makes a day */
#define MOVE_MIN	3			/* a carry shorter than this is a press */
#define EDGE		5			/* a link's right and lower edges size it */
#define LINK_MIN_W	40			/* a link is no narrower than this */
#define STEP		16			/* a copy is put this far down and across */

#define STRIP_H		40			/* the strip of parts along the top */
#define HEAD_H		24			/* the row of the week's days */
#define FRAME_W		16
#define TITLE_H		34
#define MIN_W		400
#define MIN_H		300

#define LINK_MAX	256
#define HOL_MAX		400
#define OUT_MAX		( 512 * 1024 )
#define CHUNK		( 64 * 1024 )
#define UNDO_MAX	16			/* changes that can be taken back */

/* The strip and the panel that asks for a month (the data box) */
#define PN_STRIP	1
#define PN_MONTH	2
#define P_STATUS	1
#define P_PREV		20
#define P_TODAY		21
#define P_NEXT		22
#define P_MONTH		23
#define P_YM		11
#define P_YM_CANCEL	101
#define P_YM_OK		102

/* Keys: the keyboard's usages */
#define K_ENTER		0x28
#define K_ESC		0x29
#define K_DEL		0x4C
#define K_HOME		0x4A
#define K_PGUP		0x4B
#define K_PGDN		0x4E
#define K_KPENTER	0x58
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )
#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )

/* Colours of the grid on the screen */
#define C_PAPER		0x00DEDEDEU		/* the object's paper when it says none */
#define C_LINE		0x00808080U
#define C_CELL		0x00FFFFFFU
#define C_OTHER		0x00F0F0F0U
#define C_TODAY		0x00FFFFD0U
#define C_TEXT		0x00000000U
#define C_SUNDAY	0x00CC0000U
#define C_SATURDAY	0x000000CCU
#define C_GREY		0x00808080U
#define C_PICK		0x000050C0U

/* What the hand holds */
#define HOLD_NONE	0
#define HOLD_MOVE	1			/* links that are not a day's, moved where they are let go */
#define HOLD_DAY	2			/* a day's link: it goes only out of the window */
#define HOLD_SIZE	3			/* a link's edge */

/* One link of the month shown */
typedef struct {
	UB		*el;			/* its element, as read or made */
	INT		len;
	TS_UUID		target;
	UB		name[OB_NAME_MAX];	/* its object's */
	T_DPRECT	r;			/* where it is on the screen */
	BOOL		shown;
	BOOL		day;			/* a day's of this month: kept in its cell */
	BOOL		pick;			/* taken */
} CLINK;

/* The month's links as they were before a change */
typedef struct {
	INT	n;
	INT	len[LINK_MAX];
	UB	*bytes;
} SNAP;

/* ---------------------------------------------------------------- state */

static TS_UUID	g_cal;			/* the 予定表 object */
static UB	*g_calx;		/* its record 0 */
static INT	g_caln;
static BOOL	g_autoref;		/* its store counts the links of its records itself */
static TS_UUID	g_hol, g_tpl;		/* 休日 and 年月日実身 */
static BOOL	g_has_hol, g_has_tpl;
static CTHOL	g_hols[HOL_MAX];
static INT	g_nhol;
static UW	g_paper = C_PAPER;	/* the 予定表 object's paper */

static INT	g_y, g_m;		/* the month shown */
static INT	g_ty, g_tm, g_td;	/* today */
static BOOL	g_has_month;
static TS_UUID	g_month;
static CLINK	g_l[LINK_MAX];
static INT	g_nl;
static INT	g_more[32];		/* links of each day not shown for want of room */
static BOOL	g_show_hidden;		/* 隠蔽仮身を表示 */
static SNAP	*g_undo[UNDO_MAX];
static INT	g_nundo;
static INT	g_pasted;		/* pastes since the last copy: each is put a step further */

static TS_UUID	g_win, g_ch;
static ID	g_kw, g_port;
static TS_UUID	g_lch;			/* where the desktop says what windows opened from its links did to them */
static ID	g_lport;
static INT	g_gid, g_w, g_h;
static UI	g_ui;
static BOOL	g_strip;
static BOOL	g_dirty, g_closing;
static BOOL	g_full;			/* 全画面表示, and where the window stood before */
static T_OBWPOS	g_restore;

static BOOL	g_band;			/* a rectangle being pulled out in a day */
static INT	g_band_day, g_bx0, g_by0, g_bx1, g_by1;
static INT	g_press_link = -1, g_press_day = 0;
static UD	g_press_at;

static INT	g_hold = HOLD_NONE;	/* what the hand holds, and where it went down */
static INT	g_hold_link;
static BOOL	g_hold_twice;		/* pressed twice: opens, unless carried out */
static BOOL	g_hold_right, g_hold_foot;
static BOOL	g_hold_moved;
static INT	g_hx0, g_hy0, g_hx, g_hy;

static UB	g_out[OUT_MAX];
static char	g_meta_none[1];
static char	*g_meta = g_meta_none;	/* the metadata meta_of read last ("" when none) */

static void layout( void );
static ER print_months( BOOL all );
static void left( void );
static void event( CONST T_OBNTM *m );

/* ---------------------------------------------------------------- saying things */

static void note( CONST char *s )
{
	char	line[300];

	snprintf(line, sizeof(line), "calendar: %s\n", s);
	tm_putstring((CONST UB *)line);
}

/* On the message line and the strip, and on the console */
static void say( CONST char *s )
{
	(void)wm_msg_put(s);
	if ( g_strip ) ui_set_text(&g_ui, P_STATUS, s);
}

static BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

/* ---------------------------------------------------------------- records */

/* A whole record of an object, 0 after it; NULL when it cannot be read */
static UB *rec_read( CONST TS_UUID *u, INT recno, INT *p_len )
{
	T_OBREC	r[16];
	INT	cnt = 0, i, at = 0;
	UD	size = 0;
	SZ	asz = 0;
	UB	*buf = NULL;
	BOOL	found = FALSE;
	ID	k;

	*p_len = 0;
	k = ob_opn_obj(u, OB_OP_R);
	if ( k <= 0 ) return NULL;
	if ( ob_lst_rec(k, r, 16, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 16; i++ ) {
			if ( r[i].recno == recno ) {
				size = r[i].size;
				found = TRUE;
			}
		}
	}
	if ( found && size < 16 * 1024 * 1024 ) buf = malloc((size_t)size + 1);
	while ( buf != NULL && (UD)at < size ) {
		SZ	n = ( size - at > CHUNK ) ? CHUNK : (SZ)( size - at );

		if ( ob_rea_rec(k, recno, (D)at, buf + at, n, &asz) < E_OK || asz <= 0 ) break;
		at += (INT)asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL && (UD)at != size ) {
		free(buf);
		buf = NULL;
	}
	if ( buf != NULL ) {
		buf[size] = 0;
		*p_len = (INT)size;
	}
	return buf;
}

static ER write_all( ID k, INT recno, CONST UB *data, INT len )
{
	INT	at = 0;
	SZ	asz = 0;
	ER	er;

	while ( at < len ) {
		SZ	n = ( len - at > CHUNK ) ? CHUNK : (SZ)( len - at );

		er = ob_wri_rec(k, recno, (D)at, data + at, n, &asz);
		if ( er < E_OK ) return er;
		if ( asz != n ) return E_IO;
		at += (INT)n;
	}
	return E_OK;
}

/* Record 0 of an object written whole */
static ER vol_of( CONST TS_UUID *obj, TS_UUID *p_vol, void *arg )
{
	return ob_ref_vid(obj, p_vol, NULL);
}

/*
 * Record 0 of an object written whole, its links first made to say on
 * which volume their objects are, as seen from the object's (design 8.3),
 * as the desktop does when it saves
 */
static ER rec_write( CONST TS_UUID *u, CONST UB *data, INT len )
{
	TS_UUID	hv, mine;
	UB	*fixed = NULL;
	INT	changed = 0;
	ID	k;
	ER	er;

	if ( ob_ref_vid(u, &hv, &mine) >= E_OK
	  && ( fixed = malloc((size_t)tad_lnk_vol_room(data, (SZ)len)) ) != NULL ) {
		SZ	n = tad_lnk_vol_fix(data, (SZ)len, fixed, &hv, &mine, vol_of, NULL, &changed);

		if ( changed > 0 ) {
			data = fixed;
			len = (INT)n;
		}
	}
	k = ob_opn_obj(u, OB_OP_R | OB_OP_W);
	if ( k <= 0 ) {
		free(fixed);
		return (ER)k;
	}
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(k, 0, data, (SZ)len);
	ob_cls_obj(k);
	free(fixed);
	return er;
}

static void name_of( CONST TS_UUID *u, UB *out )
{
	T_OBREF	ref;

	out[0] = 0;
	if ( ob_ref_obj(u, &ref) >= E_OK ) {
		memcpy(out, ref.name, OB_NAME_MAX);
		out[OB_NAME_MAX - 1] = 0;
	}
}

static BOOL name_is( CONST UB *a, CONST char *b )
{
	return (BOOL)( strcmp((CONST char *)a, b) == 0 );
}

/* An object's metadata, in a buffer of this program's; its length, 0 when it cannot be read */
static INT meta_of( CONST TS_UUID *u )
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

/* ---------------------------------------------------------------- the programs' templates */

/* The icon of the template of the program an applist names (id), or NULL; the caller frees it */
static UB *app_icon( CONST char *id, SZ *p_size )
{
	TS_UUID		box, *lk = NULL;
	INT		n = 0, i;
	UB		*ico = NULL;

	*p_size = 0;
	/* every program of the box, however many */
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) < E_OK || ( n = ui_lst_lnk(&box, &lk) ) < 0 ) {
		return NULL;
	}
	for ( i = 0; i < n && ico == NULL; i++ ) {
		T_JSON	root, tf, pg, base, it;
		UB	pid[48], bs[48];
		TS_UUID	bu;
		INT	mn = meta_of(&lk[i]);

		if ( mn > 0 && js_parse((CONST UB *)g_meta, mn, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "program", &pg) >= E_OK
		  && js_get_str(&pg, "id", pid, sizeof(pid)) > 0 && strcmp((char *)pid, id) == 0
		  && js_get(&pg, "base", &base) >= E_OK ) {
			it.s = NULL;
			if ( js_next(&base, &it) && js_str(&it, bs, sizeof(bs)) > 0 && ct_uuid_parse(bs, &bu) ) {
				ID	kb = ob_opn_obj(&bu, OB_OP_ATRRD);

				ico = ( kb > 0 ) ? malloc(OB_ICO_MAX) : NULL;
				if ( ico != NULL && ( ob_get_ico(kb, ico, OB_ICO_MAX, p_size) < E_OK || *p_size <= 0 ) ) {
					free(ico);
					ico = NULL;
					*p_size = 0;
				}
				if ( kb > 0 ) ob_cls_obj(kb);
			}
		}
	}
	free(lk);
	return ico;
}

/*
 * A new object beside the 予定表 object: its metadata, the icon of the
 * template of the program that opens it, and its record 0.
 */
static ER obj_make( CONST char *json, CONST char *app, CONST UB *rec, INT len, TS_UUID *p_u )
{
	T_OBCRE	c;
	UB	*ico;
	SZ	icosz = 0;
	INT	recno = -1;
	ER	er;
	ID	k;

	ico = app_icon(app, &icosz);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	c.near = g_cal;
	c.icon = ico;
	c.iconsz = ( ico != NULL ) ? icosz : 0;
	er = ob_cre_obj(&c, p_u);
	free(ico);
	if ( er < E_OK ) return er;
	k = ob_opn_obj(p_u, OB_OP_R | OB_OP_W | OB_O_EXCL);	/* a new object filled: not read half made */
	if ( k <= 0 ) {
		(void)ob_del_obj(p_u);
		return (ER)k;
	}
	er = ob_apd_rec(k, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = write_all(k, recno, rec, len);
	ob_cls_obj(k);
	if ( er < E_OK ) (void)ob_del_obj(p_u);
	return er;
}

static void new_vobjid( TS_UUID *u )
{
	static UB	seq;

	memset(u, 0, sizeof(*u));
	if ( ts_gen_uuid(u) < E_OK ) {
		/* no clock yet: unique among this program's own */
		*u = g_cal;
		u->b[15] ^= ++seq;
		u->b[14] ^= 0x5A;
	}
}

/* ---------------------------------------------------------------- the 予定表 object */

/*
 * The 予定表's links to the 休日 object and to the template of a day
 * (年月日実身) carry those words as their relationship (the link's own
 * 続柄, xmlTAD's relationship attribute), and are found by them: the
 * objects may be named anything. A 予定表 made before, or by the TADjs
 * Desktop, has links without them; those are found by their objects'
 * names, as the TADjs Desktop finds them, and given the words, so that
 * they are found so from then on.
 */
#define REL_HOL		"休日"
#define REL_TPL		"年月日実身"

/* Whether the link's relationship has word w among its words */
static BOOL rel_has( CONST UB *el, INT len, CONST char *w )
{
	UB	r[160];
	INT	n = ct_attr_text(el, len, "relationship", r, sizeof(r)), i = 0, a, wl = (INT)strlen(w);

	while ( n > 0 && i < n ) {
		while ( i < n && r[i] == ' ' ) i++;
		a = i;
		while ( i < n && r[i] != ' ' ) i++;
		if ( i - a == wl && memcmp(r + a, w, (size_t)wl) == 0 ) return TRUE;
	}
	return FALSE;
}

/* The link at el[i] of the 予定表's record given word w too, the record written again */
static void rel_give( CTEL *el, INT i, CONST char *w )
{
	UB	r[160], v[200];
	INT	n = ct_attr_text(g_calx + el[i].at, el[i].len, "relationship", r, sizeof(r));
	CTSET	set;
	CTBUF	b;

	if ( n < 0 ) n = 0;
	r[n] = 0;
	snprintf((char *)v, sizeof(v), "%s%s%s", (char *)r, ( n > 0 ) ? " " : "", w);
	set.name = "relationship";
	set.value = v;
	ct_buf(&b, g_out, OUT_MAX);
	ct_putn(&b, g_calx, el[i].at);
	ct_link_edit(&b, g_calx + el[i].at, el[i].len, &set, 1);
	ct_putn(&b, g_calx + el[i].at + el[i].len, g_caln - el[i].at - el[i].len);
	if ( b.fail || rec_write(&g_cal, b.p, b.n) < E_OK ) return;
	note("relationship given");
}

/* Its record read again, and the 休日 object and the template found by their links' relationships */
static void cal_read( void )
{
	CTEL	el[LINK_MAX];
	INT	n, i, hol_by_name = -1, tpl_by_name = -1;

	free(g_calx);
	g_calx = rec_read(&g_cal, 0, &g_caln);
	g_has_hol = g_has_tpl = FALSE;
	if ( g_calx == NULL ) return;
	n = ct_links(g_calx, g_caln, el, LINK_MAX);
	for ( i = 0; i < n; i++ ) {
		TS_UUID	u;

		if ( !ct_target(g_calx + el[i].at, el[i].len, &u) ) continue;
		if ( !g_has_hol && rel_has(g_calx + el[i].at, el[i].len, REL_HOL) ) {
			g_hol = u;
			g_has_hol = TRUE;
		} else if ( !g_has_tpl && rel_has(g_calx + el[i].at, el[i].len, REL_TPL) ) {
			g_tpl = u;
			g_has_tpl = TRUE;
		}
	}
	for ( i = 0; i < n && ( !g_has_hol || !g_has_tpl ); i++ ) {
		TS_UUID	u;
		UB	nm[OB_NAME_MAX];

		if ( !ct_target(g_calx + el[i].at, el[i].len, &u) ) continue;
		name_of(&u, nm);
		if ( !g_has_hol && name_is(nm, "休日") ) {
			g_hol = u;
			g_has_hol = TRUE;
			hol_by_name = i;
		} else if ( !g_has_tpl && name_is(nm, "年月日実身") ) {
			g_tpl = u;
			g_has_tpl = TRUE;
			tpl_by_name = i;
		}
	}
	/* found by their names: given the words, one after the other (the record is read again between) */
	if ( hol_by_name >= 0 ) {
		rel_give(el, hol_by_name, REL_HOL);
		if ( tpl_by_name >= 0 ) {
			free(g_calx);
			g_calx = rec_read(&g_cal, 0, &g_caln);
			n = ( g_calx != NULL ) ? ct_links(g_calx, g_caln, el, LINK_MAX) : 0;
			if ( tpl_by_name >= n ) tpl_by_name = -1;
		}
	}
	if ( tpl_by_name >= 0 && g_calx != NULL ) rel_give(el, tpl_by_name, REL_TPL);
	if ( hol_by_name >= 0 || tpl_by_name >= 0 ) {
		free(g_calx);
		g_calx = rec_read(&g_cal, 0, &g_caln);
	}
}

/* The paper the 予定表 object asks for (its window's backgroundColor) */
static void paper_read( void )
{
	INT	n = meta_of(&g_cal);

	g_paper = ( n > 0 ) ? ct_json_colour((CONST UB *)g_meta, n, "window", "backgroundColor", C_PAPER) : C_PAPER;
}

/* The month object of y/m among the 予定表's links */
static BOOL month_find( INT y, INT m, TS_UUID *p_u )
{
	CTEL	el[LINK_MAX];
	UB	want[8], nm[OB_NAME_MAX];
	INT	n, i;

	if ( g_calx == NULL ) return FALSE;
	ct_month_name(want, y, m);
	n = ct_links(g_calx, g_caln, el, LINK_MAX);
	for ( i = 0; i < n; i++ ) {
		if ( !ct_target(g_calx + el[i].at, el[i].len, p_u) ) continue;
		name_of(p_u, nm);
		if ( strcmp((char *)nm, (char *)want) == 0 ) return TRUE;
	}
	return FALSE;
}

static void hol_read( void )
{
	UB	*x;
	INT	n = 0;

	g_nhol = 0;
	if ( !g_has_hol ) return;
	x = rec_read(&g_hol, 0, &n);
	if ( x != NULL ) g_nhol = ct_hol_text(x, n, g_hols, HOL_MAX);
	free(x);
}

/* ---------------------------------------------------------------- today */

static void today( void )
{
	CONST TS_UUID	clk = OB_UUID_CLOCK_INIT;
	UB		t[48];
	SZ		asz = 0;
	ID		k = ob_opn_obj(&clk, OB_OP_READ);

	g_ty = 2026;
	g_tm = 1;
	g_td = 1;
	if ( k > 0 ) {
		/* "2026-09-25T18:30:12+09:00", the time where the person is */
		if ( ob_rea_rec(k, 1, 0, t, sizeof(t) - 1, &asz) >= E_OK && asz >= 10 ) {
			t[asz] = 0;
			g_ty = atoi((char *)t);
			g_tm = atoi((char *)t + 5);
			g_td = atoi((char *)t + 8);
			if ( g_ty < 1 || g_tm < 1 || g_tm > 12 || g_td < 1 || g_td > 31 ) {
				g_ty = 2026;
				g_tm = g_td = 1;
			}
		}
		ob_cls_obj(k);
	}
}

/* ---------------------------------------------------------------- the month */

static void links_free( void )
{
	INT	i;

	for ( i = 0; i < g_nl; i++ ) free(g_l[i].el);
	g_nl = 0;
}

/* A link added to the month shown: its element copied */
static BOOL link_add( CONST UB *el, INT len, CONST TS_UUID *target )
{
	CLINK	*l;

	if ( g_nl >= LINK_MAX ) return FALSE;
	l = &g_l[g_nl];
	l->el = malloc((size_t)len + 1);
	if ( l->el == NULL ) return FALSE;
	memcpy(l->el, el, (size_t)len);
	l->el[len] = 0;
	l->len = len;
	l->target = *target;
	name_of(target, l->name);
	l->shown = FALSE;
	l->day = FALSE;
	l->pick = FALSE;
	g_nl++;
	return TRUE;
}

/* A link's element replaced by what is in b */
static BOOL link_set( INT i, CONST CTBUF *b )
{
	UB	*el;

	if ( b->fail || i < 0 || i >= g_nl ) return FALSE;
	el = malloc((size_t)b->n + 1);
	if ( el == NULL ) return FALSE;
	memcpy(el, b->p, (size_t)b->n);
	el[b->n] = 0;
	free(g_l[i].el);
	g_l[i].el = el;
	g_l[i].len = b->n;
	return TRUE;
}

/* A link taken out of the list; its object's count is the caller's */
static void link_drop( INT i )
{
	free(g_l[i].el);
	memmove(&g_l[i], &g_l[i + 1], sizeof(CLINK) * (size_t)( g_nl - i - 1 ));
	g_nl--;
}

static INT today_in( INT y, INT m )
{
	return ( y == g_ty && m == g_tm ) ? g_td : 0;
}

/* The month's record written again from its links */
static ER month_save( void )
{
	CTLNK	l[LINK_MAX];
	CTBUF	b;
	INT	i;
	ER	er;

	if ( !g_has_month ) return E_OK;
	for ( i = 0; i < g_nl; i++ ) {
		l[i].el = g_l[i].el;
		l[i].len = g_l[i].len;
		l[i].name = g_l[i].name;
	}
	ct_buf(&b, g_out, OUT_MAX);
	er = ct_month_xml(&b, g_y, g_m, today_in(g_y, g_m), g_hols, g_nhol, l, g_nl);
	if ( er >= E_OK ) er = rec_write(&g_month, b.p, b.n);
	if ( er < E_OK ) {
		char	s[80];

		snprintf(s, sizeof(s), "年月実身を保存できません（%d）", (INT)er);
		say(s);
	}
	return er;
}

/*
 * The month's object made, when it has none yet: a figure of the grid,
 * opened by 仮身一覧, and a link to it put in the 予定表 under the
 * others.
 */
static ER month_make( void )
{
	UB	mn[8], vrec[512];
	char	json[640];
	CTBUF	b, v;
	CTEL	el[LINK_MAX];
	INT	box[4], n;
	TS_UUID	vid;
	ER	er;

	if ( g_has_month ) return E_OK;
	if ( g_calx == NULL ) return E_NOEXS;
	ct_month_name(mn, g_y, g_m);
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"editable\":true,\"deletable\":true,\"readable\":true,"
		 "\"window\":{\"width\":800,\"height\":600,\"minWidth\":400,\"minHeight\":300},"
		 "\"applist\":{\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":true},"
		 "\"base-calendar\":{\"name\":\"予定表\",\"defaultOpen\":false},"
		 "\"basic-text-editor\":{\"name\":\"基本テキストエディタ\",\"defaultOpen\":false}}}", mn);
	ct_buf(&b, g_out, OUT_MAX);
	er = ct_month_xml(&b, g_y, g_m, today_in(g_y, g_m), g_hols, g_nhol, NULL, 0);
	if ( er >= E_OK ) er = obj_make(json, "virtual-object-list", b.p, b.n, &g_month);
	if ( er < E_OK ) return er;

	/* the link in the 予定表: under the ones it has, one a row */
	n = ct_links(g_calx, g_caln, el, LINK_MAX);
	box[0] = 10;
	box[1] = n * 40 + 10;
	box[2] = 160;
	box[3] = box[1] + CT_LINK_H;
	new_vobjid(&vid);
	ct_buf(&v, vrec, sizeof(vrec));
	ct_link_new(&v, &g_month, &vid, box, FALSE);
	ct_buf(&b, g_out, OUT_MAX);
	er = ct_insert(&b, g_calx, g_caln, v.p, v.n);
	if ( er >= E_OK ) er = rec_write(&g_cal, b.p, b.n);
	if ( er >= E_OK && !g_autoref ) er = ob_lnk_obj(&g_month);
	if ( er < E_OK ) {
		(void)ob_del_obj(&g_month);
		return er;
	}
	g_has_month = TRUE;
	cal_read();
	{
		char	s[80];

		snprintf(s, sizeof(s), "made month %s", mn);
		note(s);
	}
	return E_OK;
}

/* ---------------------------------------------------------------- taking back (取消) */

static void snap_free( SNAP *s )
{
	if ( s != NULL ) {
		free(s->bytes);
		free(s);
	}
}

static void undo_clear( void )
{
	while ( g_nundo > 0 ) snap_free(g_undo[--g_nundo]);
}

/* The month's links as they are now, kept before a change; the oldest kept goes when there are too many */
static void undo_push( void )
{
	SNAP	*s = malloc(sizeof(SNAP));
	INT	i, total = 0, at = 0;

	if ( s == NULL ) return;
	for ( i = 0; i < g_nl; i++ ) total += g_l[i].len;
	s->bytes = malloc((size_t)total + 1);
	if ( s->bytes == NULL ) {
		free(s);
		return;
	}
	s->n = g_nl;
	for ( i = 0; i < g_nl; i++ ) {
		s->len[i] = g_l[i].len;
		memcpy(s->bytes + at, g_l[i].el, (size_t)g_l[i].len);
		at += g_l[i].len;
	}
	if ( g_nundo == UNDO_MAX ) {
		snap_free(g_undo[0]);
		memmove(&g_undo[0], &g_undo[1], sizeof(SNAP *) * ( UNDO_MAX - 1 ));
		g_nundo--;
	}
	g_undo[g_nundo++] = s;
}

/*
 * The last change taken back: the links as they were before it. An
 * object whose link comes back is counted once more, and one whose link
 * goes once less, as the change counted the other way.
 */
static void undo_do( void )
{
	SNAP	*s;
	TS_UUID	was[LINK_MAX];
	BOOL	used[LINK_MAX];
	INT	nwas = g_nl, i, k, at = 0;

	if ( g_nundo == 0 ) {
		say("取り消せる操作がありません");
		return;
	}
	s = g_undo[--g_nundo];
	for ( i = 0; i < g_nl; i++ ) {
		was[i] = g_l[i].target;
		used[i] = FALSE;
	}
	links_free();
	for ( i = 0; i < s->n; i++ ) {
		TS_UUID	u;

		if ( ct_target(s->bytes + at, s->len[i], &u) && link_add(s->bytes + at, s->len[i], &u) ) {
			for ( k = 0; k < nwas && ( used[k] || !same_uuid(&was[k], &u) ); k++ ) ;
			if ( k < nwas ) used[k] = TRUE;
			else if ( !g_autoref ) (void)ob_lnk_obj(&u);
		}
		at += s->len[i];
	}
	for ( k = 0; k < nwas; k++ ) {
		if ( !used[k] && !g_autoref ) (void)ob_unl_obj(&was[k]);
	}
	snap_free(s);
	(void)month_save();
	layout();
	note("undone");
	say("取り消しました");
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- showing a month */

static void month_show( INT y, INT m )
{
	UB	*x = NULL, mn[8];
	CTEL	*el;
	INT	n = 0, k, i;
	char	s[80];

	links_free();
	undo_clear();
	g_y = y;
	g_m = m;
	g_band = FALSE;
	g_hold = HOLD_NONE;
	g_pasted = 0;
	g_has_month = month_find(y, m, &g_month);
	el = malloc(sizeof(CTEL) * LINK_MAX);
	if ( g_has_month ) x = rec_read(&g_month, 0, &n);
	if ( x != NULL && el != NULL ) {
		k = ct_links(x, n, el, LINK_MAX);
		for ( i = 0; i < k; i++ ) {
			TS_UUID	u;

			if ( ct_target(x + el[i].at, el[i].len, &u) ) (void)link_add(x + el[i].at, el[i].len, &u);
		}
		/* with no 休日 object, the holidays the month itself names */
		if ( !g_has_hol ) g_nhol = ct_hol_month(x, n, y, m, g_hols, HOL_MAX);
	}
	free(x);
	free(el);
	ct_month_name(mn, y, m);
	if ( g_strip ) ui_set_text(&g_ui, P_MONTH, (CONST char *)mn);
	snprintf(s, sizeof(s), "month %s links %d%s", mn, g_nl, g_has_month ? "" : " (none yet)");
	note(s);
	layout();
	g_dirty = TRUE;
}

static void month_step( INT d )
{
	INT	y = g_y, m = g_m + d;

	if ( m < 1 ) {
		m = 12;
		y--;
	} else if ( m > 12 ) {
		m = 1;
		y++;
	}
	if ( y < 1 || y > 9999 ) return;
	month_show(y, m);
}

/* ---------------------------------------------------------------- the desktop */

/*
 * What came while the desktop was asked: the presses, moves and keys
 * were the desktop's own -- a panel it showed over this window, an
 * outline it carried -- and are passed over; the rest are done now.
 */
static void settle( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( !g_closing && ob_rea_rec(g_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( ( m.event & ( OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_ENTER
				 | OB_E_LEAVE | OB_E_WHEEL ) ) != 0
		  || ( g_strip && same_uuid(&m.uuid, &g_ui.pnl) ) ) {
			continue;		/* the strip's parts too: a key that answered the desktop reached them */
		}
		event(&m);
	}
}

/* A request to the desktop, its panels over this window; the answer waited for */
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

/* ---------------------------------------------------------------- 印刷 */

/* A month record made a page and put in the object of pages */
static ER print_put( T_UIPRN *up, CONST UB *x, INT n, UB *page, INT max )
{
	CTBUF	b;
	ER	er;

	ct_buf(&b, page, max);
	er = ct_print_page(&b, x, n);
	return ( er < E_OK ) ? er : ui_prn_page(up, b.p, b.n);
}

/*
 * 印刷: the month shown, or every month the 予定表 has an object for in
 * the order of the months, as pages handed to the desktop to print
 * (DT_RQ_PRINT). The desktop's panels say how; the file it writes goes
 * on the desktop.
 */
static ER print_months( BOOL all )
{
	T_UIPRN	up;
	UB	*page = malloc(OUT_MAX);
	ER	er;
	char	s[96];

	if ( page == NULL ) return E_NOMEM;
	er = ui_prn_open(&up, &g_cal);
	if ( er < E_OK ) {
		free(page);
		return er;
	}
	if ( !all ) {
		CTLNK	l[LINK_MAX];
		CTBUF	b;
		INT	i;

		for ( i = 0; i < g_nl; i++ ) {
			l[i].el = g_l[i].el;
			l[i].len = g_l[i].len;
			l[i].name = g_l[i].name;
		}
		ct_buf(&b, g_out, OUT_MAX);
		er = ct_month_xml(&b, g_y, g_m, 0, g_hols, g_nhol, l, g_nl);
		if ( er >= E_OK ) er = print_put(&up, b.p, b.n, page, OUT_MAX);
	} else if ( g_calx != NULL ) {
		CTEL	el[LINK_MAX];
		INT	key[LINK_MAX], n, i, k, cnt = 0;
		TS_UUID	us[LINK_MAX];

		/* the month objects among the links, in the order of their months */
		n = ct_links(g_calx, g_caln, el, LINK_MAX);
		for ( i = 0; i < n; i++ ) {
			TS_UUID	u;
			UB	nm[OB_NAME_MAX];
			INT	y, m;

			if ( !ct_target(g_calx + el[i].at, el[i].len, &u) ) continue;
			name_of(&u, nm);
			if ( !ct_is_month(nm, &y, &m) ) continue;
			for ( k = cnt; k > 0 && key[k - 1] > y * 12 + m; k-- ) {
				key[k] = key[k - 1];
				us[k] = us[k - 1];
			}
			key[k] = y * 12 + m;
			us[k] = u;
			cnt++;
		}
		for ( i = 0; i < cnt && er >= E_OK; i++ ) {
			INT	len = 0;
			UB	*x = rec_read(&us[i], 0, &len);

			if ( x == NULL ) continue;
			er = print_put(&up, x, len, page, OUT_MAX);
			free(x);
		}
	}
	if ( er >= E_OK ) er = ui_prn_ready(&up);
	snprintf(s, sizeof(s), "print %s pages %d (%d)", all ? "all" : "month", up.pages, (INT)er);
	note(s);
	if ( er == E_NOEXS ) {
		say("印刷する月がありません");
	} else if ( er >= E_OK ) {
		T_DTREQ	rq;
		T_DTANS	an;

		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_PRINT;
		rq.target = up.obj;
		name_of(&g_cal, rq.name);
		/* the person works the desktop's panels: as long as that takes */
		er = ask_desktop(&rq, &an, 60 * 60 * 1000);
		snprintf(s, sizeof(s), "print answered %d", (INT)er);
		note(s);
	}
	ui_prn_close(&up);
	free(page);
	return er;
}

/* A colour attribute "#rrggbb" of a link; 0xFFFFFFFF when it says none */
static UINT col_attr( CONST CLINK *l, CONST char *name )
{
	UB	col[16];

	if ( ct_attr(l->el, l->len, name, col, sizeof(col)) == 7 && col[0] == '#' ) {
		return (UINT)strtoul((char *)col + 1, NULL, 16);
	}
	return 0xFFFFFFFFU;
}

/* What a link's frame shows, as TAD_D_*: the name, frame and picture unless it says otherwise */
static CONST char *CONST disp_attr[6] = { "namedisp", "roledisp", "typedisp", "updatedisp", "framedisp", "pictdisp" };

static UINT disp_of( CONST CLINK *l )
{
	UINT	d = 0;
	INT	i;

	for ( i = 0; i < 6; i++ ) {
		if ( ct_attr_bool(l->el, l->len, disp_attr[i], (BOOL)( i == 0 || i == 4 || i == 5 )) ) d |= 1U << i;
	}
	return d;
}

/* How a link looks, for the desktop */
static void look_of( CONST CLINK *l, T_DTLOOK *k )
{
	memset(k, 0, sizeof(*k));
	k->w = l->r.right - l->r.left;
	k->h = l->r.bottom - l->r.top;
	k->chsz = ct_attr_int(l->el, l->len, "chsz", 0);
	k->frcol = col_attr(l, "frcol");
	k->chcol = col_attr(l, "chcol");
	k->tbcol = col_attr(l, "tbcol");
	k->bgcol = col_attr(l, "bgcol");
	k->disp = disp_of(l);
	k->autoopen = ct_attr_bool(l->el, l->len, "autoopen", FALSE) ? 1 : 0;
}

/* A request about the object of link i, its look said */
static void ask_of( INT i, UINT req, T_DTREQ *rq )
{
	memset(rq, 0, sizeof(*rq));
	rq->req = req;
	rq->target = g_l[i].target;
	rq->recno = -1;
	look_of(&g_l[i], &rq->look);
	if ( ct_attr_bool(g_l[i].el, g_l[i].len, "hidden", FALSE) ) rq->flags |= DT_F_HIDDEN;
}

/* What a link points at, opened as a double press opens it */
static void link_open( INT i )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[120];
	ER	er;

	if ( i < 0 || i >= g_nl ) return;
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.target = g_l[i].target;
	rq.recno = -1;
	/* what the window opened does to the link comes back to be put in the month (DT_F_LINK) */
	if ( g_lport > 0 && g_has_month && ct_link_from(&rq, &g_month, &g_lch, g_l[i].el, g_l[i].len) ) {
		T_DTLOOK	k;

		look_of(&g_l[i], &k);
		rq.look.frcol = k.frcol;
		rq.look.chcol = k.chcol;
		rq.look.tbcol = k.tbcol;
		rq.look.bgcol = k.bgcol;
	}
	er = ask_desktop(&rq, &an, 3000);
	snprintf(s, sizeof(s), "opened %s (%d)", g_l[i].name, (INT)er);
	note(s);
	if ( er < E_OK ) say("開けませんでした");
}

/* ---------------------------------------------------------------- what is taken */

static INT picked( void )
{
	INT	i, n = 0;

	for ( i = 0; i < g_nl; i++ ) {
		if ( g_l[i].pick ) n++;
	}
	return n;
}

static void pick_none( void )
{
	INT	i;

	for ( i = 0; i < g_nl; i++ ) g_l[i].pick = FALSE;
}

/* ---------------------------------------------------------------- a day's plans */

static INT link_named( CONST UB *name )
{
	INT	i;

	for ( i = 0; i < g_nl; i++ ) {
		if ( strcmp((char *)g_l[i].name, (char *)name) == 0 ) return i;
	}
	return -1;
}

/*
 * The object of day d of the month shown: opened when it has one, and
 * made from the template, with a link in its cell, when not.
 */
static void day_make( INT d )
{
	UB	day[11], *tpl = NULL, vrec[768];
	char	json[512], s[120];
	CTBUF	b, v;
	TS_UUID	u, vid;
	INT	n = 0, i, box[4];
	ER	er;

	if ( d < 1 || d > ct_days(g_y, g_m) ) return;
	ct_day_name(day, g_y, g_m, d);
	i = link_named(day);
	if ( i >= 0 ) {
		pick_none();
		g_l[i].pick = TRUE;
		g_dirty = TRUE;
		link_open(i);
		return;
	}
	if ( g_nl >= LINK_MAX ) {
		say("この月にはこれ以上置けません");
		return;
	}
	er = month_make();
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "年月実身を作れません（%d）", (INT)er);
		say(s);
		return;
	}

	/* its text: the template with the day put in, or the day alone */
	ct_buf(&b, g_out, OUT_MAX);
	if ( g_has_tpl ) tpl = rec_read(&g_tpl, 0, &n);
	if ( tpl != NULL ) ct_apply_date(&b, tpl, n, day);
	else ct_day_doc(&b, day);
	free(tpl);
	snprintf(json, sizeof(json),
		 "{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		 "\"editable\":true,\"deletable\":true,\"readable\":true,"
		 "\"applist\":{\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
		 "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false}}}", day);
	er = b.fail ? E_LIMIT : obj_make(json, "basic-text-editor", b.p, b.n, &u);
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "%s の実身を作れません（%d）", day, (INT)er);
		say(s);
		return;
	}

	/* its link, in its cell */
	undo_push();
	ct_day_box(g_y, g_m, d, 0, box);
	new_vobjid(&vid);
	ct_buf(&v, vrec, sizeof(vrec));
	ct_link_new(&v, &u, &vid, box, TRUE);
	if ( !link_add(v.p, v.n, &u) ) {
		(void)ob_del_obj(&u);
		return;
	}
	er = month_save();
	if ( er >= E_OK && !g_autoref ) er = ob_lnk_obj(&u);
	if ( er < E_OK ) {
		free(g_l[--g_nl].el);
		(void)ob_del_obj(&u);
		return;
	}
	pick_none();
	g_l[g_nl - 1].pick = TRUE;
	snprintf(s, sizeof(s), "made day %s", day);
	note(s);
	snprintf(s, sizeof(s), "%s の予定を作りました", day);
	say(s);
	g_dirty = TRUE;
}

/* The links taken, out of the month; their objects are left to the store's count */
static INT delete_picked( CONST char *what )
{
	INT	i, n = 0;
	char	s[120];

	if ( picked() == 0 ) return 0;
	undo_push();
	for ( i = g_nl - 1; i >= 0; i-- ) {
		TS_UUID	u;

		if ( !g_l[i].pick ) continue;
		u = g_l[i].target;
		snprintf(s, sizeof(s), "%s %s", what, g_l[i].name);
		note(s);
		link_drop(i);
		if ( !g_autoref ) (void)ob_unl_obj(&u);
		n++;
	}
	(void)month_save();
	layout();
	g_dirty = TRUE;
	return n;
}

/* ---------------------------------------------------------------- the layout */

/* The grid on the screen: its top, and the edges of a column and a row */
static INT grid_top( void )
{
	return STRIP_H + HEAD_H;
}

static INT col_x( INT c )
{
	return c * g_w / CT_COLS;
}

static INT row_y( INT r )
{
	return grid_top() + r * ( g_h - grid_top() ) / CT_ROWS;
}

/* A place of the month's record (800 by 600) on the screen, and back */
static INT to_sx( INT x ) { return x * g_w / CT_BASE_W; }
static INT to_sy( INT y ) { return y * g_h / CT_BASE_H; }
static INT to_rx( INT x ) { return ( g_w > 0 ) ? x * CT_BASE_W / g_w : x; }
static INT to_ry( INT y ) { return ( g_h > 0 ) ? y * CT_BASE_H / g_h : y; }

/* The day of the month shown under a place on the screen, 0 for none */
static INT day_at( INT x, INT y )
{
	INT	r, c, d;

	if ( y < grid_top() || y >= g_h || x < 0 || x >= g_w ) return 0;
	for ( r = 0; r < CT_ROWS - 1 && y >= row_y(r + 1); r++ ) ;
	for ( c = 0; c < CT_COLS - 1 && x >= col_x(c + 1); c++ ) ;
	d = r * CT_COLS + c - ct_wday(g_y, g_m, 1) + 1;
	return ( d >= 1 && d <= ct_days(g_y, g_m) ) ? d : 0;
}

/* A link's box in the record, as its element says */
static void box_of( CONST CLINK *l, INT *box )
{
	box[0] = ct_attr_int(l->el, l->len, "vobjleft", 0);
	box[1] = ct_attr_int(l->el, l->len, "vobjtop", 0);
	box[2] = ct_attr_int(l->el, l->len, "vobjright", box[0] + 150);
	box[3] = ct_attr_int(l->el, l->len, "vobjbottom", box[1] + CT_LINK_H);
	if ( box[2] - box[0] < 8 ) box[2] = box[0] + 150;
	if ( box[3] - box[1] < 8 ) box[3] = box[1] + CT_LINK_H;
}

/*
 * Where each link goes on the screen: a day's in its cell, the others
 * where they are, at the size they are. A hidden link is not shown
 * unless 隠蔽仮身を表示 is on.
 */
static void layout( void )
{
	INT	i, k, n[32];

	memset(n, 0, sizeof(n));
	memset(g_more, 0, sizeof(g_more));
	for ( i = 0; i < g_nl; i++ ) {
		CLINK	*l = &g_l[i];
		INT	y, m, d, row, col, top, box[4];

		l->shown = (BOOL)( g_show_hidden || !ct_attr_bool(l->el, l->len, "hidden", FALSE) );
		l->day = (BOOL)( ct_is_day(l->name, &y, &m, &d) && y == g_y && m == g_m && d <= ct_days(y, m) );
		if ( !l->shown ) {
			l->pick = FALSE;
			continue;
		}
		if ( l->day ) {
			ct_cell(y, m, d, &row, &col);
			k = n[d]++;
			top = row_y(row) + CT_DAYNO_H + 2 + k * ( CT_EV_H + CT_EV_GAP );
			l->r.left = col_x(col) + 1 + CT_EV_GAP;
			l->r.right = col_x(col + 1) - CT_EV_GAP;
			l->r.top = top;
			l->r.bottom = top + CT_EV_H;
			if ( l->r.bottom > row_y(row + 1) - 1 ) {
				l->shown = FALSE;
				l->pick = FALSE;
				g_more[d]++;
			}
		} else {
			box_of(l, box);
			l->r.left = to_sx(box[0]);
			l->r.top = to_sy(box[1]);
			if ( l->r.top < grid_top() ) l->r.top = grid_top();
			l->r.right = l->r.left + ( box[2] - box[0] );
			l->r.bottom = l->r.top + ( box[3] - box[1] );
		}
	}
}

/* The link under a place, the one drawn last first; -1 for none */
static INT link_at( INT x, INT y )
{
	INT	i;

	for ( i = g_nl - 1; i >= 0; i-- ) {
		CONST T_DPRECT	*r = &g_l[i].r;

		if ( g_l[i].shown && x >= r->left && x < r->right && y >= r->top && y < r->bottom ) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- drawing */

static void fill( INT l, INT t, INT r, INT b, UW c )
{
	T_DPRECT	rc;

	rc.left = l;
	rc.top = t;
	rc.right = r;
	rc.bottom = b;
	if ( r > l && b > t ) (void)dp_fill_rect(g_gid, &rc, c);
}

static void frame( INT l, INT t, INT r, INT b, UW c, INT w )
{
	T_DPRECT	f;

	f.left = l;
	f.top = t;
	f.right = r;
	f.bottom = b;
	if ( r > l && b > t ) (void)dp_frame_rect(g_gid, &f, c, w);
}

/* Words cut at a letter's edge to fit width, with "…" when they were cut */
static void text_fit( INT x, INT y, CONST UB *s, UW col, INT px, INT width )
{
	char	b[256];
	INT	n;

	if ( width <= 0 ) return;
	strncpy(b, (CONST char *)s, sizeof(b) - 4);
	b[sizeof(b) - 4] = 0;
	if ( dp_text_width((CONST UB *)b, px) <= width ) {
		(void)dp_text(g_gid, x, y, (CONST UB *)b, col, px);
		return;
	}
	n = (INT)strlen(b);
	while ( n > 0 ) {
		do {
			n--;
		} while ( n > 0 && ( b[n] & 0xC0 ) == 0x80 );
		strcpy(b + n, "…");
		if ( dp_text_width((CONST UB *)b, px) <= width ) break;
	}
	if ( n > 0 ) (void)dp_text(g_gid, x, y, (CONST UB *)b, col, px);
}

/* The links of the month as a figure, each in its place on the screen */
static void draw_links( void )
{
	CTBUF		b;
	T_DPRECT	clip;
	INT		i, box[4];

	ct_buf(&b, g_out, OUT_MAX);
	ct_put(&b, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n");
	for ( i = 0; i < g_nl; i++ ) {
		CLINK	*l = &g_l[i];

		if ( !l->shown ) continue;
		box[0] = l->r.left;
		box[1] = l->r.top;
		box[2] = l->r.right;
		box[3] = l->r.bottom;
		ct_link_put(&b, l->el, l->len, box, -1, TRUE);
		ct_put(&b, "\n");
	}
	ct_put(&b, "</figure>\n</tad>\n");
	if ( b.fail ) return;
	clip.left = 0;
	clip.top = grid_top();
	clip.right = g_w;
	clip.bottom = g_h;
	(void)dp_draw_tad(g_gid, &clip, 0, 0, b.p, b.n);
}

/* The size a link held by its edge comes to, where the hand is */
static void sized_box( CONST CLINK *l, INT *p_w, INT *p_h )
{
	INT	w = l->r.right - l->r.left, h = l->r.bottom - l->r.top;

	if ( g_hold_right ) w += g_hx - g_hx0;
	if ( g_hold_foot ) h += g_hy - g_hy0;
	if ( w < LINK_MIN_W ) w = LINK_MIN_W;
	*p_w = w;
	*p_h = ct_snap_h(ct_attr_int(l->el, l->len, "chsz", 14), h);
}

static void paint( void )
{
	static CONST char *CONST wd[7] = { "日", "月", "火", "水", "木", "金", "土" };
	INT	r, c, i, start = ct_wday(g_y, g_m, 1), days = ct_days(g_y, g_m);
	INT	pm = ( g_m == 1 ) ? 12 : g_m - 1, py = ( g_m == 1 ) ? g_y - 1 : g_y;
	INT	prev = ct_days(py, pm);
	UB	num[16];

	if ( g_gid < 0 ) return;
	fill(0, STRIP_H, g_w, g_h, g_paper);

	/* the days of the week, on the paper */
	fill(0, STRIP_H + HEAD_H - 1, g_w, STRIP_H + HEAD_H, C_LINE);
	for ( c = 0; c < CT_COLS; c++ ) {
		INT	x0 = col_x(c), x1 = col_x(c + 1);
		INT	tw = dp_text_width((CONST UB *)wd[c], 13);

		(void)dp_text(g_gid, x0 + ( x1 - x0 - tw ) / 2, STRIP_H + 17, (CONST UB *)wd[c],
			      ( c == 0 ) ? C_SUNDAY : ( c == 6 ) ? C_SATURDAY : C_TEXT, 13);
		if ( c > 0 ) fill(x0, STRIP_H, x0 + 1, STRIP_H + HEAD_H - 1, 0x00C0C0C0U);
	}

	/* the cells */
	fill(0, grid_top(), g_w, g_h, C_LINE);
	for ( r = 0; r < CT_ROWS; r++ ) {
		for ( c = 0; c < CT_COLS; c++ ) {
			INT		cell = r * CT_COLS + c, d = cell - start + 1, n;
			INT		x0 = col_x(c), x1 = col_x(c + 1), y0 = row_y(r), y1 = row_y(r + 1);
			UW		bg = C_CELL, fg = C_TEXT;
			CONST UB	*hol = NULL;

			if ( d < 1 ) {
				n = prev + d;
				bg = C_OTHER;
				fg = C_GREY;
			} else if ( d > days ) {
				n = d - days;
				bg = C_OTHER;
				fg = C_GREY;
			} else {
				n = d;
				hol = ct_hol_find(g_hols, g_nhol, g_y, g_m, d);
				if ( c == 0 || hol != NULL ) fg = C_SUNDAY;
				else if ( c == 6 ) fg = C_SATURDAY;
				if ( today_in(g_y, g_m) == d ) bg = C_TODAY;
			}
			fill(x0 + 1, y0 + 1, x1, y1, bg);
			snprintf((char *)num, sizeof(num), "%d", n);
			{
				INT	nx = x0 + 5 + dp_text(g_gid, x0 + 5, y0 + 15, num, fg, 13);

				if ( hol != NULL ) text_fit(nx + 6, y0 + 14, hol, C_SUNDAY, 11, x1 - nx - 10);
			}
			if ( d >= 1 && d <= days && g_more[d] > 0 ) {
				char	m[16];

				snprintf(m, sizeof(m), "他%d件", g_more[d]);
				(void)dp_text(g_gid, x1 - 4 - dp_text_width((CONST UB *)m, 11), y1 - 4,
					      (CONST UB *)m, C_GREY, 11);
			}
		}
	}

	draw_links();

	/* what is taken, where it is being carried or sized to, and the rectangle being pulled out */
	for ( i = 0; i < g_nl; i++ ) {
		CONST T_DPRECT	*f = &g_l[i].r;

		if ( !g_l[i].shown || !g_l[i].pick ) continue;
		frame(f->left - 2, f->top - 2, f->right + 2, f->bottom + 2, C_PICK, 2);
		if ( g_hold == HOLD_MOVE && g_hold_moved && !g_l[i].day ) {
			INT	dx = g_hx - g_hx0, dy = g_hy - g_hy0;

			if ( f->top + dy >= grid_top() ) {
				frame(f->left + dx, f->top + dy, f->right + dx, f->bottom + dy, C_TEXT, 1);
			}
		}
	}
	if ( g_hold == HOLD_SIZE && g_hold_moved && g_hold_link >= 0 && g_hold_link < g_nl ) {
		CONST CLINK	*l = &g_l[g_hold_link];
		INT		w, h;

		sized_box(l, &w, &h);
		frame(l->r.left, l->r.top, l->r.left + w, l->r.top + h, C_TEXT, 1);
	}
	if ( g_band ) {
		T_DPRECT	f;

		f.left = ( g_bx0 < g_bx1 ) ? g_bx0 : g_bx1;
		f.right = ( g_bx0 < g_bx1 ) ? g_bx1 : g_bx0;
		f.top = ( g_by0 < g_by1 ) ? g_by0 : g_by1;
		f.bottom = ( g_by0 < g_by1 ) ? g_by1 : g_by0;
		if ( f.right > f.left && f.bottom > f.top ) (void)dp_frame_rect(g_gid, &f, C_TEXT, 1);
	}
	(void)wm_obj_flush(g_kw, NULL);
	g_dirty = FALSE;
}

/* ---------------------------------------------------------------- the window */

static void work_size( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		g_w = wp.wright - wp.wleft;
		g_h = wp.wbottom - wp.wtop;
	}
	g_ui.w = g_w;
	g_ui.h = g_h;			/* what a panel that asks is put in the middle of */
}

/* The number after "key": in a part's metadata, 0 when there is none */
static INT json_int( CONST char *j, CONST char *key )
{
	char		k[32];
	CONST char	*p;
	INT		v = 0;

	snprintf(k, sizeof(k), "\"%s\":", key);
	p = strstr(j, k);
	if ( p == NULL ) return 0;
	p += strlen(k);
	while ( *p == ' ' ) p++;
	while ( *p >= '0' && *p <= '9' ) v = v * 10 + ( *p++ - '0' );
	return v;
}

/*
 * The strip: panel 1 of the data box, made, placed along the top of the
 * window and its parts opened by the links it keeps to them. The panel
 * is wider than any screen, so its ground reaches the window's right
 * edge however wide the window is made.
 */
static ER strip_open( void )
{
	T_OBCRE	c;
	T_OBMAP	mp;
	T_OBNTF	req;
	SZ	asz = 0;
	char	json[200];
	INT	i;

	snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"box\":\"%s\",\"num\":%d}}}",
		 SYSDEF_BOX_CALENDAR, PN_STRIP);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_ui.pnl) < E_OK ) return E_OBJ;
	g_ui.pk = ob_opn_obj(&g_ui.pnl, OB_OP_R | OB_OP_W | OB_OP_PROT);
	if ( g_ui.pk <= 0 ) {
		(void)ob_del_obj(&g_ui.pnl);
		g_ui.pk = 0;
		return E_OBJ;
	}
	memset(&mp, 0, sizeof(mp));
	if ( ob_map_rec(g_ui.pk, 0, g_kw, &mp) < E_OK ) {
		ob_cls_obj(g_ui.pk);
		(void)ob_del_obj(&g_ui.pnl);
		g_ui.pk = 0;
		return E_OBJ;
	}
	g_ui.np = 0;
	for ( i = 0; i < UI_PART_MAX; i++ ) {
		UIPART	*p = &g_ui.p[i];
		TS_UUID	pu;
		char	j[512];

		if ( ob_rea_rec(g_ui.pk, OB_WR_KIND + i, 0, &pu, sizeof(pu), &asz) < E_OK || asz < (SZ)sizeof(pu) ) break;
		p->key = ob_opn_obj(&pu, OB_OP_R | OB_OP_W | OB_OP_PROT);
		p->num = 0;
		p->kind = 0;
		if ( p->key > 0 && ob_get_atr(p->key, (UB *)j, sizeof(j) - 1, &asz) >= E_OK ) {
			j[( asz < (SZ)sizeof(j) ) ? asz : (SZ)sizeof(j) - 1] = 0;
			p->num = json_int(j, "part");
			p->kind = (UINT)json_int(j, "kind");
		}
		if ( p->key > 0 && p->num == 0 ) {
			ob_cls_obj(p->key);
			p->key = 0;
		}
		g_ui.np = i + 1;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 1;
	(void)ob_ntf_evt(g_ui.pk, OB_REC_ANY, &req, g_port);
	g_strip = TRUE;
	return E_OK;
}

static void strip_close( void )
{
	INT	i;

	for ( i = 0; i < g_ui.np; i++ ) {
		if ( g_ui.p[i].key > 0 ) ob_cls_obj(g_ui.p[i].key);
		g_ui.p[i].key = 0;
	}
	g_ui.np = 0;
	if ( g_ui.pk > 0 ) {
		ob_cls_obj(g_ui.pk);
		(void)ob_del_obj(&g_ui.pnl);
		g_ui.pk = 0;
	}
	g_strip = FALSE;
}

/* The window, where the 予定表 object last stood (its metadata), at least the size of the strip */
static ER win_open( CONST char *title )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	T_JSON		root, w, pos;
	SZ		asz = 0;
	char		json[260], shown[TS_UUID_STRLEN + 1];
	INT		x = 100, y = 80, ww = 800, wh = 600, mn = meta_of(&g_cal);

	if ( mn > 0 && js_parse((CONST UB *)g_meta, mn, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK ) {
		ww = (INT)js_get_num(&w, "width", ww);
		wh = (INT)js_get_num(&w, "height", wh);
		if ( js_get(&w, "pos", &pos) >= E_OK ) {
			x = (INT)js_get_num(&pos, "x", x);
			y = (INT)js_get_num(&pos, "y", y);
		}
	}
	if ( ww < MIN_W ) ww = MIN_W;
	if ( wh < MIN_H ) wh = MIN_H;
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	ts_uuid_text(&g_cal, shown);	/* what it shows, for ウインドウ一覧 */
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 x, y, x + ww + FRAME_W, y + wh + TITLE_H, SYSDEF_PROG_CALENDAR, shown);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) return E_OBJ;
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_MOVE | OB_E_KEY | OB_E_REDRAW
		   | OB_E_DELETE | OB_E_DROP | OB_E_LEAVE;
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( g_kw > 0 ) ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
		g_kw = 0;
		return E_OBJ;
	}
	/* the work area the size asked for, whatever the frame takes */
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + ww + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + wh + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	g_ui.kw = g_kw;
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_CALENDAR, &g_ui.box);
	work_size();
	return E_OK;
}

/* The window made again at its new size: no smaller than the least it may be */
static void resized( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	work_size();
	if ( ( g_w < MIN_W || g_h < MIN_H ) && ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		if ( g_w < MIN_W ) wp.right += MIN_W - g_w;
		if ( g_h < MIN_H ) wp.bottom += MIN_H - g_h;
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		work_size();
	}
	layout();
	g_dirty = TRUE;
}

/*
 * 全画面表示: the window made the size of the screen above the message
 * line by the desktop, or put back where it stood before.
 */
static void fullscreen( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	SZ	asz = 0;

	if ( g_full ) {
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz);
		g_full = FALSE;
		note("fullscreen off");
	} else if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &g_restore, sizeof(g_restore), &asz) >= E_OK ) {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_FULL;
		if ( ask_desktop(&rq, &an, 5000) >= E_OK ) {
			g_full = TRUE;
			note("fullscreen on");
		}
	}
	resized();
}

/* ---------------------------------------------------------------- asking for a month */

static void ask_month( void )
{
	UI	q;
	T_OBNTM	m;
	INT	v[2], num, ev, done = 0;

	if ( ui_dlg_open(&g_ui, PN_MONTH, &q) < E_OK ) return;
	v[0] = g_y;
	v[1] = g_m;
	ui_set_fields(&q, P_YM, v, 2);
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			done = ( num == P_YM_OK ) ? 1 : -1;
			break;
		case UI_EV_CLOSE:
			done = -1;
			break;
		case UI_EV_NONE:
			if ( m.id == 3 && m.event == OB_E_KEY && ( m.code == K_ENTER || m.code == K_KPENTER ) ) done = 1;
			break;
		default:
			break;
		}
	}
	if ( done > 0 ) (void)ui_fields(&q, P_YM, v, 2);
	ui_dlg_close(&q);
	if ( done > 0 && v[0] >= 1 && v[0] <= 9999 && v[1] >= 1 && v[1] <= 12 ) month_show(v[0], v[1]);
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- putting links in */

static void hex_col( char *out, UINT c, CONST char *dflt )
{
	if ( c == 0xFFFFFFFFU ) strcpy(out, dflt);
	else snprintf(out, 8, "#%06x", c & 0x00FFFFFFU);
}

/*
 * A link to target put in the month, made from an element it had
 * elsewhere (or NULL for a new one): with an identity of its own, in
 * box (the record's terms), kept whole inside the month, and its object
 * counted once more.
 */
static BOOL link_put( CONST UB *el, INT len, CONST TS_UUID *target, INT *box )
{
	char	vs[TS_UUID_STRLEN + 1], ids[TS_UUID_STRLEN + 8], num[5][12];
	static UB	tmp[16384];
	CTSET	set[7];
	CTBUF	b;
	TS_UUID	vid;
	INT	k;

	ct_fit_box(box);
	new_vobjid(&vid);
	ct_uuid_str(&vid, vs);
	ct_uuid_str(target, ids);
	strcat(ids, "_0.xtad");
	snprintf(num[0], 12, "%d", box[0]);
	snprintf(num[1], 12, "%d", box[1]);
	snprintf(num[2], 12, "%d", box[2]);
	snprintf(num[3], 12, "%d", box[3]);
	snprintf(num[4], 12, "%d", box[3] - box[1]);
	set[0].name = "id";		set[0].value = (CONST UB *)ids;
	set[1].name = "vobjid";		set[1].value = (CONST UB *)vs;
	for ( k = 0; k < 5; k++ ) {
		static CONST char *CONST bn[5] = { "vobjleft", "vobjtop", "vobjright", "vobjbottom", "height" };

		set[2 + k].name = bn[k];
		set[2 + k].value = (CONST UB *)num[k];
	}
	ct_buf(&b, tmp, sizeof(tmp));
	if ( el != NULL ) {
		ct_link_edit(&b, el, len, set, 7);
	} else {
		ct_link_new(&b, target, &vid, box, FALSE);
	}
	if ( b.fail || !link_add(b.p, b.n, target) ) return FALSE;
	if ( !g_autoref ) (void)ob_lnk_obj(target);
	return TRUE;
}

/* Links dropped on the window: put in the month where they were let go */
static void dropped( void )
{
	T_OBDROP	*d;
	char		el[700], fr[8], ch[8], tb[8], bg[8], vs[40], ts[40];
	INT		i, n = 0, x, y;
	ER		er;

	if ( ( d = ui_drop(&g_ui) ) == NULL ) return;
	if ( g_nl + d->n > LINK_MAX ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "この月にはこれ以上置けません");
		return;
	}
	if ( ( er = month_make() ) < E_OK ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "年月実身を作れません");
		return;
	}
	ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
	undo_push();
	x = to_rx(d->x);
	y = to_ry(d->y);
	pick_none();
	for ( i = 0; i < d->n; i++ ) {
		T_OBDROPV	*v = &d->v[i];
		INT		w = v->right - v->left, h = v->bottom - v->top, box[4];
		UINT		disp = ( v->disp != 0 ) ? v->disp : 0x31U;
		TS_UUID		vid;

		if ( w < LINK_MIN_W ) w = 150;
		if ( h < 12 ) h = CT_LINK_H;
		box[0] = x;
		box[1] = y + n * ( h + 4 );
		box[2] = box[0] + w;
		box[3] = box[1] + h;
		ct_fit_box(box);
		new_vobjid(&vid);
		ct_uuid_str(&v->target, ts);
		ct_uuid_str(&vid, vs);
		hex_col(fr, v->frcol, "#000000");
		hex_col(ch, v->chcol, "#000000");
		hex_col(tb, v->tbcol, "#ffffff");
		hex_col(bg, v->bgcol, "#ffffff");
		snprintf(el, sizeof(el),
			 "<link id=\"%s_0.xtad\" vobjid=\"%s\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\""
			 " vobjbottom=\"%d\" height=\"%d\" chsz=\"%d\" frcol=\"%s\" chcol=\"%s\" tbcol=\"%s\""
			 " bgcol=\"%s\" dlen=\"0\" pictdisp=\"%s\" namedisp=\"%s\" roledisp=\"%s\" typedisp=\"%s\""
			 " updatedisp=\"%s\" framedisp=\"%s\" autoopen=\"false\" hidden=\"false\""
			 " scrollx=\"0\" scrolly=\"0\" zoomratio=\"1\"/>",
			 ts, vs, box[0], box[1], box[2], box[3], h, ( v->chsz > 0 ) ? v->chsz : 14,
			 fr, ch, tb, bg,
			 ( disp & 0x20 ) ? "true" : "false", ( disp & 0x01 ) ? "true" : "false",
			 ( disp & 0x02 ) ? "true" : "false", ( disp & 0x04 ) ? "true" : "false",
			 ( disp & 0x08 ) ? "true" : "false", ( disp & 0x10 ) ? "true" : "false");
		if ( link_add((CONST UB *)el, (INT)strlen(el), &v->target) ) {
			if ( !g_autoref ) (void)ob_lnk_obj(&v->target);
			g_l[g_nl - 1].pick = TRUE;
			n++;
		}
	}
	if ( n > 0 ) {
		char	s[80];

		(void)month_save();
		layout();
		snprintf(s, sizeof(s), "dropped %d", n);
		note(s);
		g_dirty = TRUE;
	}
}

/* ---------------------------------------------------------------- the tray */

/* What is taken, to the tray as a figure of links: one set, named for how many */
static INT tray_copy( void )
{
	CTLNK	l[LINK_MAX];
	CTBUF	b;
	T_TRREC	r;
	UB	name[TR_NAME_MAX];
	INT	i, n = 0;

	for ( i = 0; i < g_nl; i++ ) {
		if ( !g_l[i].pick ) continue;
		l[n].el = g_l[i].el;
		l[n].len = g_l[i].len;
		l[n].name = g_l[i].name;
		n++;
	}
	if ( n == 0 ) return 0;
	ct_buf(&b, g_out, OUT_MAX);
	if ( ct_frag(&b, l, n) < E_OK ) return 0;
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = b.p;
	r.size = b.n;
	snprintf((char *)name, sizeof(name), "仮身%d個", n);
	if ( tr_psh_dat(&r, 1, name) < E_OK ) {
		say("トレーに入れられませんでした");
		return 0;
	}
	g_pasted = 0;
	return n;
}

/* The first xmlTAD fragment of the set in hand, read whole; NULL when it holds none */
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

/* Whether the set in hand holds a fragment to take links from */
static BOOL tray_has_links( void )
{
	T_TRSTS	*s = malloc(sizeof(T_TRSTS));
	BOOL	yes = FALSE;

	if ( s != NULL && tr_get_sts(s) >= E_OK && s->hand > 0 && s->hand <= TR_LEVELS ) {
		yes = (BOOL)( ( s->set[s->hand].kinds & ( 1U << TR_TAD ) ) != 0 );
	}
	free(s);
	return yes;
}

/*
 * The links of the set in hand put in the month, each a new link to
 * the same object, a step further than the last paste so that pasting
 * twice does not lay one on the other. A day's link goes to its cell.
 * 'take' takes the set out of the tray after: moving from the tray.
 */
static void tray_paste( BOOL take )
{
	UB	*x;
	CTEL	el[LINK_MAX];
	INT	n = 0, k, i, put = 0, off;
	char	s[80];

	x = tray_frag(&n);
	k = ( x != NULL ) ? ct_links(x, n, el, LINK_MAX) : 0;
	if ( k == 0 ) {
		free(x);
		say("トレーに仮身がありません");
		return;
	}
	if ( g_nl + k > LINK_MAX ) {
		free(x);
		say("この月にはこれ以上置けません");
		return;
	}
	if ( month_make() < E_OK ) {
		free(x);
		say("年月実身を作れません");
		return;
	}
	undo_push();
	pick_none();
	g_pasted++;
	off = STEP * g_pasted;
	for ( i = 0; i < k; i++ ) {
		CONST UB	*e = x + el[i].at;
		TS_UUID		u;
		INT		box[4], w, h;

		if ( !ct_target(e, el[i].len, &u) ) continue;
		box[0] = ct_attr_int(e, el[i].len, "vobjleft", -1);
		box[1] = ct_attr_int(e, el[i].len, "vobjtop", -1);
		w = ct_attr_int(e, el[i].len, "vobjright", -1) - box[0];
		h = ct_attr_int(e, el[i].len, "vobjbottom", -1) - box[1];
		if ( box[0] < 0 || box[1] < 0 || w < LINK_MIN_W || h < 8 ) {
			/* a link of a text: one under another at the left */
			w = ct_attr_int(e, el[i].len, "width", 150);
			h = ct_attr_int(e, el[i].len, "heightPx", CT_LINK_H);
			if ( w < LINK_MIN_W ) w = 150;
			if ( h < 8 ) h = CT_LINK_H;
			box[0] = 10;
			box[1] = CT_GRID_TOP + 10 + put * ( h + 4 );
		}
		box[0] += off;
		box[1] += off;
		box[2] = box[0] + w;
		box[3] = box[1] + h;
		if ( link_put(e, el[i].len, &u, box) ) {
			g_l[g_nl - 1].pick = TRUE;
			put++;
		}
	}
	free(x);
	if ( put > 0 ) {
		(void)month_save();
		if ( take ) (void)tr_del_dat(TR_HAND);
	}
	layout();
	snprintf(s, sizeof(s), "pasted %d", put);
	note(s);
	snprintf(s, sizeof(s), "%d個の仮身を%s", put, take ? "トレーから移動しました" : "トレーから複写しました");
	say(s);
	g_dirty = TRUE;
}

/* The トレー row of the edit menu: a row for each level, the one in hand ticked */
static void tray_menu( ID mid )
{
	static UB	rows[TR_LEVELS][WM_LABEL_MAX];
	TS_UUID		td;
	T_TRSTS		*s = malloc(sizeof(T_TRSTS));
	INT		i;

	if ( s == NULL ) return;
	if ( ui_uuid(SYSDEF_MENU_TRAY, &td) >= E_OK && tr_get_sts(s) >= E_OK ) {
		for ( i = 1; i <= s->nlevel && i <= TR_LEVELS; i++ ) {
			snprintf((char *)rows[i - 1], WM_LABEL_MAX, "%d %s", i, (char *)s->set[i].name);
		}
		(void)mn_set_lst(mid, &td, "tray.level", (CONST UB *)rows, WM_LABEL_MAX, s->nlevel);
		if ( s->hand > 0 ) (void)mn_chg_idx(mid, &td, "tray.level", s->hand - 1, MN_TICK);
		(void)mn_chg_atr(mid, &td, "tray.clear", ( s->nlevel == 0 ) ? MN_GREY : 0);
	}
	free(s);
}

/* ---------------------------------------------------------------- carried inside, out, sized */

/* The links taken that are not a day's, moved by where the hand went, and written */
static void move_picked( INT dx, INT dy )
{
	INT	i, n = 0;
	char	s[80];

	undo_push();
	for ( i = 0; i < g_nl; i++ ) {
		CLINK	*l = &g_l[i];
		INT	box[4];
		static UB	tmp[16384];
		CTBUF	b;

		if ( !l->pick || l->day || !l->shown ) continue;
		box[0] = to_rx(l->r.left + dx);
		box[1] = to_ry(l->r.top + dy);
		box[2] = box[0] + ( l->r.right - l->r.left );
		box[3] = box[1] + ( l->r.bottom - l->r.top );
		ct_fit_box(box);
		ct_buf(&b, tmp, sizeof(tmp));
		ct_link_put(&b, l->el, l->len, box, -1, FALSE);
		if ( link_set(i, &b) ) n++;
	}
	if ( n > 0 ) {
		(void)month_save();
		snprintf(s, sizeof(s), "moved %d", n);
		note(s);
	} else {
		snap_free(g_undo[--g_nundo]);
	}
	layout();
	g_dirty = TRUE;
}

/* A link held by its edge, made the size the hand let it go at: closed or open as that size is */
static void size_link( INT i )
{
	CLINK	*l = &g_l[i];
	INT	box[4], w, h;
	static UB	tmp[16384];
	CTBUF	b;
	char	s[120];

	sized_box(l, &w, &h);
	box[0] = to_rx(l->r.left);
	box[1] = to_ry(l->r.top);
	box[2] = box[0] + w;
	box[3] = box[1] + h;
	ct_fit_box(box);
	undo_push();
	ct_buf(&b, tmp, sizeof(tmp));
	ct_link_put(&b, l->el, l->len, box, -1, FALSE);
	if ( !link_set(i, &b) ) {
		snap_free(g_undo[--g_nundo]);
		return;
	}
	(void)month_save();
	layout();
	snprintf(s, sizeof(s), "resized %s %dx%d", l->name, w, h);
	note(s);
	g_dirty = TRUE;
}

/*
 * What is taken, carried out of the window: the desktop follows the
 * pointer with an outline round all of it and answers where it was let
 * go. Over a window of the desktop's that takes links, a link to each
 * object is put there, as they lay; the ones here are taken out of the
 * month (a move) unless the second button went down on the way (a
 * copy). Pressed twice before it was carried, each link put there is to
 * a copy of its object, and nothing here changes. Let go anywhere else,
 * nothing happens.
 */
static void carry_out( void )
{
	T_DTREQ		rq;
	T_DTANS		an;
	T_DPRECT	o;
	INT		i, k, n = 0, last = -1, done = 0;
	BOOL		twice = g_hold_twice, copy, given;
	ER		er;
	UINT		seq;
	char		s[120];

	memset(&o, 0, sizeof(o));
	if ( g_hold_link >= 0 && g_hold_link < g_nl && !g_l[g_hold_link].pick ) {
		pick_none();
		g_l[g_hold_link].pick = TRUE;
	}
	for ( i = 0; i < g_nl; i++ ) {
		if ( !g_l[i].pick || !g_l[i].shown ) continue;
		if ( n == 0 ) o = g_l[i].r;
		if ( g_l[i].r.left < o.left ) o.left = g_l[i].r.left;
		if ( g_l[i].r.top < o.top ) o.top = g_l[i].r.top;
		if ( g_l[i].r.right > o.right ) o.right = g_l[i].r.right;
		if ( g_l[i].r.bottom > o.bottom ) o.bottom = g_l[i].r.bottom;
		last = i;
		n++;
	}
	g_hold = HOLD_NONE;
	if ( n == 0 ) return;
	if ( n == 1 ) {
		ask_of(last, DT_RQ_CARRY, &rq);
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.req = DT_RQ_CARRY;
		rq.win = g_win;
		rq.look.frcol = rq.look.chcol = rq.look.tbcol = rq.look.bgcol = 0xFFFFFFFFU;
	}
	rq.flags |= DT_F_TO_PROG;
	rq.look.w = o.right - o.left;
	rq.look.h = o.bottom - o.top;
	rq.grab_x = g_hx0 - o.left;
	rq.grab_y = g_hy0 - o.top;
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er < E_OK ) {
		g_dirty = TRUE;
		return;
	}
	seq = rq.seq;
	copy = (BOOL)( an.how == DT_HOW_COPY );
	given = (BOOL)( ( an.flags & DT_F_TO_PROG ) != 0 );
	for ( i = 0, k = 0; i < g_nl; i++ ) {
		if ( !g_l[i].pick || !g_l[i].shown ) continue;
		ask_of(i, ( ++k < n ) ? DT_RQ_PLACE_MORE : DT_RQ_PLACE, &rq);
		rq.grab_x = g_l[i].r.left - o.left;
		rq.grab_y = g_l[i].r.top - o.top;
		rq.carry = seq;
		if ( twice ) rq.flags |= DT_F_DUPLICATE;
		memset(&an, 0, sizeof(an));
		er = ask_desktop(&rq, &an, 10 * 60 * 1000);
		snprintf(s, sizeof(s), "carried %s (%d)%s", g_l[i].name, (INT)er,
			 twice ? " as a new object" : copy ? " as a copy" : "");
		note(s);
		if ( er >= E_OK ) done++;
		else g_l[i].pick = FALSE;	/* not put there: it stays here */
	}
	if ( done > 0 && !twice && !copy ) {
		(void)delete_picked("moved out");
		snprintf(s, sizeof(s), "%d個の仮身を移動しました", done);
		say(s);
	} else if ( done > 0 && !given ) {
		snprintf(s, sizeof(s), "%d個の仮身を%s", done, twice ? "実身複製しました" : "複写しました");
		say(s);
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the desktop's 仮身操作 and 実身操作 */

/* A link's attributes set as the desktop's 属性変更 answered */
static void link_look( INT i, CONST T_DTANS *an )
{
	CLINK	*l = &g_l[i];
	char	col[4][8], cs[12], hs[12], bs[12];
	CTSET	set[16];
	static UB	tmp[16384];
	CTBUF	b;
	CONST UINT *c[4];
	static CONST char *CONST cn[4] = { "frcol", "chcol", "tbcol", "bgcol" };
	INT	n = 0, k, box[4];

	c[0] = &an->look.frcol;
	c[1] = &an->look.chcol;
	c[2] = &an->look.tbcol;
	c[3] = &an->look.bgcol;
	if ( an->look.chsz > 0 ) {
		snprintf(cs, sizeof(cs), "%d", an->look.chsz);
		set[n].name = "chsz";
		set[n++].value = (CONST UB *)cs;
	}
	for ( k = 0; k < 4; k++ ) {
		if ( *c[k] == 0xFFFFFFFFU ) continue;
		snprintf(col[k], sizeof(col[k]), "#%06x", *c[k] & 0x00FFFFFFU);
		set[n].name = cn[k];
		set[n++].value = (CONST UB *)col[k];
	}
	for ( k = 0; k < 6; k++ ) {
		set[n].name = disp_attr[k];
		set[n++].value = (CONST UB *)( ( ( an->look.disp >> k ) & 1 ) ? "true" : "false" );
	}
	set[n].name = "autoopen";
	set[n++].value = (CONST UB *)( an->look.autoopen ? "true" : "false" );
	set[n].name = "hidden";
	set[n++].value = (CONST UB *)( ( an->flags & DT_F_HIDDEN ) ? "true" : "false" );
	if ( !l->day ) {
		/* the band grows with the letters */
		box_of(l, box);
		k = box[3] - box[1];
		if ( k < ct_band_h(an->look.chsz) ) k = ct_band_h(an->look.chsz);
		snprintf(bs, sizeof(bs), "%d", box[1] + k);
		snprintf(hs, sizeof(hs), "%d", k);
		set[n].name = "vobjbottom";
		set[n++].value = (CONST UB *)bs;
		set[n].name = "height";
		set[n++].value = (CONST UB *)hs;
	}
	ct_buf(&b, tmp, sizeof(tmp));
	ct_link_edit(&b, l->el, l->len, set, n);
	undo_push();
	if ( !link_set(i, &b) ) {
		snap_free(g_undo[--g_nundo]);
		return;
	}
	(void)month_save();
	layout();
}

/* One attribute of a link set to text (NULL takes it off), and the month written */
static BOOL link_attr( INT i, CONST char *name, CONST UB *value )
{
	CTSET	set;
	UB	*tmp;
	CTBUF	b;
	BOOL	ok;
	INT	max = g_l[i].len + 64 + ( ( value != NULL ) ? 6 * (INT)strlen((CONST char *)value) : 0 );

	tmp = malloc((size_t)max);
	if ( tmp == NULL ) return FALSE;
	set.name = name;
	set.value = value;
	ct_buf(&b, tmp, max);
	ct_link_edit(&b, g_l[i].el, g_l[i].len, &set, 1);
	undo_push();
	ok = link_set(i, &b);
	free(tmp);
	if ( !ok ) {
		snap_free(g_undo[--g_nundo]);
		return FALSE;
	}
	(void)month_save();
	layout();
	return TRUE;
}

/*
 * A link to a new object (a copy, a new version) put a step down and
 * across from link i; kept to be taken back to when undo says so
 */
static void link_copy_put_as( INT i, CONST TS_UUID *nid, INT step, BOOL undo )
{
	INT	box[4];
	static UB	tmp[16384];
	CTSET	set[2];
	CTBUF	b;

	if ( g_nl >= LINK_MAX ) return;
	box[0] = to_rx(g_l[i].r.left) + step;
	box[1] = to_ry(g_l[i].r.top) + step;
	box[2] = box[0] + ( g_l[i].r.right - g_l[i].r.left );
	box[3] = box[1] + ( g_l[i].r.bottom - g_l[i].r.top );
	/* its own name and its object's data are the new object's */
	set[0].name = "name";
	set[0].value = NULL;
	set[1].name = "dlen";
	set[1].value = (CONST UB *)"0";
	ct_buf(&b, tmp, sizeof(tmp));
	ct_link_edit(&b, g_l[i].el, g_l[i].len, set, 2);
	if ( b.fail ) return;
	if ( undo ) undo_push();
	pick_none();
	if ( !link_put(b.p, b.n, nid, box) ) {
		if ( undo ) snap_free(g_undo[--g_nundo]);
		return;
	}
	g_l[g_nl - 1].pick = TRUE;
	(void)month_save();
	layout();
}

static void link_copy_put( INT i, CONST TS_UUID *nid, INT step )
{
	link_copy_put_as(i, nid, step, TRUE);
}

/* The link of the month shown with vobjid; -1 none */
static INT link_with( CONST TS_UUID *vobjid )
{
	UB	vs[TS_UUID_STRLEN + 1];
	TS_UUID	u;
	INT	i;

	for ( i = 0; i < g_nl; i++ ) {
		if ( ct_attr_text(g_l[i].el, g_l[i].len, "vobjid", vs, (INT)sizeof(vs)) == TS_UUID_STRLEN
		  && ct_uuid_parse(vs, &u) && same_uuid(&u, vobjid) ) return i;
	}
	return -1;
}

/*
 * A link of a month that is not shown, changed in the month's record as
 * it is kept: its element with set, or, with nid, a link to nid made like
 * it put in after the others (its object counted once more)
 */
static void month_link_told( CONST TS_UUID *month, CONST TS_UUID *vobjid, CONST CTSET *set, INT n,
			     CONST TS_UUID *nid )
{
	char	vs[TS_UUID_STRLEN + 1], ids[TS_UUID_STRLEN + 8];
	CTSET	add[4];
	CTEL	el;
	CTBUF	b, e;
	UB	*x, *y = NULL;
	INT	len = 0, max;
	TS_UUID	vid;

	x = rec_read(month, 0, &len);
	if ( x == NULL || !ct_link_of(x, len, vobjid, &el) ) {
		free(x);
		return;
	}
	max = len + el.len + 1024;
	y = malloc((size_t)max);
	if ( y == NULL ) {
		free(x);
		return;
	}
	if ( nid == NULL ) {
		ct_buf(&b, y, max);
		ct_putn(&b, x, el.at);
		ct_link_edit(&b, x + el.at, el.len, set, n);
		ct_putn(&b, x + el.at + el.len, len - el.at - el.len);
	} else {
		static UB	tmp[16384];

		new_vobjid(&vid);
		ct_uuid_str(&vid, vs);
		ct_uuid_str(nid, ids);
		strcat(ids, "_0.xtad");
		add[0].name = "id";	add[0].value = (CONST UB *)ids;
		add[1].name = "vobjid";	add[1].value = (CONST UB *)vs;
		add[2].name = "name";	add[2].value = NULL;
		add[3].name = "dlen";	add[3].value = (CONST UB *)"0";
		ct_buf(&e, tmp, sizeof(tmp));
		ct_link_edit(&e, x + el.at, el.len, add, 4);
		ct_buf(&b, y, max);
		if ( !e.fail ) (void)ct_insert(&b, x, len, e.p, e.n);
		else b.fail = TRUE;
	}
	if ( !b.fail && rec_write(month, b.p, b.n) >= E_OK && nid != NULL && !g_autoref ) (void)ob_lnk_obj(nid);
	free(y);
	free(x);
}

/*
 * What a window opened from a link of a month did to it, told by the
 * desktop (DT_F_LINK): the view it closed with written into the link, or
 * a link to the copy it was saved as put with it, as a window of the
 * desktop's changes its own; nothing to take back
 */
static void link_told( CONST T_DTANS *an )
{
	static UB	tmp[16384];
	CTVIEW	v;
	CTSET	set[CT_VIEW_SET];
	CTBUF	b;
	TS_UUID	month, vid;
	INT	n = ct_link_told(an, &month, &vid, &v, set), i;
	char	s[96];

	if ( n < 0 ) return;
	if ( !g_has_month || !same_uuid(&month, &g_month) ) {
		month_link_told(&month, &vid, set, n, ( an->how == DT_HOW_LINK_ADD ) ? &an->into : NULL);
	} else if ( ( i = link_with(&vid) ) >= 0 && an->how == DT_HOW_LINK_ADD ) {
		link_copy_put_as(i, &an->into, 0, FALSE);
	} else if ( i >= 0 ) {
		ct_buf(&b, tmp, sizeof(tmp));
		ct_link_edit(&b, g_l[i].el, g_l[i].len, set, n);
		if ( link_set(i, &b) ) (void)month_save();
	}
	if ( an->how == DT_HOW_LINK_VIEW ) snprintf(s, sizeof(s), "link view written back: %s,%s", v.sx, v.sy);
	else snprintf(s, sizeof(s), "a link to the copy saved put with its link");
	note(s);
	g_dirty = TRUE;
}

static void links_told( void )
{
	T_DTANS	an;
	SZ	asz = 0;

	while ( g_lport > 0 && ob_rea_tmo(g_lport, 0, 0, &an, sizeof(an), &asz, TMO_POL) >= E_OK ) {
		if ( asz == (SZ)sizeof(an) ) link_told(&an);
	}
}

/* The channel the desktop says on, and a notice of it at the window's channel */
static void links_channel( void )
{
	T_OBCRE	c;
	T_OBNTF	req;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_lch) < E_OK ) return;
	g_lport = ob_opn_obj(&g_lch, OB_OP_READ | OB_OP_ATRRD | OB_O_NOWAIT);
	if ( g_lport <= 0 ) {
		g_lport = 0;
		(void)ob_del_obj(&g_lch);
		return;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 4;
	(void)ob_ntf_evt(g_lport, OB_REC_ANY, &req, g_port);
}

/* The applist of a link: its own when it has one, its object's when not; its length */
static INT app_json( INT i, UB **p_out )
{
	T_JSON	root, al;
	INT	n;

	/* the link's own: no longer than the link it is written in */
	*p_out = malloc((size_t)g_l[i].len + 1);
	if ( *p_out == NULL ) return 0;
	n = ct_attr_text(g_l[i].el, g_l[i].len, "applist", *p_out, g_l[i].len + 1);
	if ( n > 0 ) return n;
	n = meta_of(&g_l[i].target);
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

/* The programs 実行 lists for link i: the list the desktop's own links show (DT_RQ_APPS) */
static INT g_napp;
static INT	g_appcap;			/* entries the three tables have room for */
static UB	(*g_app_id)[CT_APP_ID], (*g_app_name)[CT_APP_NAME];
static BOOL	*g_app_kind;			/* offered for the object's kind, not in its applist */

/* Room for 'want' programs, what is there kept */
static BOOL app_room( INT want )
{
	UB	(*id)[CT_APP_ID], (*name)[CT_APP_NAME];
	BOOL	*kind;

	if ( want <= g_appcap ) return TRUE;
	id = realloc(g_app_id, CT_APP_ID * (size_t)want);
	if ( id != NULL ) g_app_id = id;
	name = realloc(g_app_name, CT_APP_NAME * (size_t)want);
	if ( name != NULL ) g_app_name = name;
	kind = realloc(g_app_kind, sizeof(BOOL) * (size_t)want);
	if ( kind != NULL ) g_app_kind = kind;
	if ( id == NULL || name == NULL || kind == NULL ) return FALSE;
	g_appcap = want;
	return TRUE;
}


static void exec_menu( ID mid, INT i )
{
	UB		(*rows)[WM_LABEL_MAX];
	TS_UUID		ed;
	T_DTREQ		rq;
	T_DTANS		an;
	INT		n = 1, k;

	g_napp = 0;
	for ( k = 0; k < n; k++ ) {
		ask_of(i, DT_RQ_APPS, &rq);
		ct_link_req(&rq, g_l[i].el, g_l[i].len);
		rq.recno = k;
		memset(&an, 0, sizeof(an));
		if ( ask_desktop(&rq, &an, 3000) < E_OK ) break;
		n = (INT)an.how;
		if ( !app_room(n) ) break;
		if ( ct_app_entry(&an, g_app_id[g_napp], g_app_name[g_napp], &g_app_kind[g_napp]) ) g_napp++;
	}
	rows = malloc(WM_LABEL_MAX * (size_t)( g_napp + 1 ));
	if ( rows == NULL ) g_napp = 0;
	for ( k = 0; k < g_napp; k++ ) {
		snprintf((char *)rows[k], WM_LABEL_MAX, "%s", (char *)g_app_name[k]);
	}
	if ( rows != NULL && ui_uuid(SYSDEF_MENU_EXEC, &ed) >= E_OK ) {
		(void)mn_set_lst(mid, &ed, "exec.app", (CONST UB *)rows, WM_LABEL_MAX, g_napp);
	}
	free(rows);
}

/* Whether link i keeps to the program it opens with (起動アプリの固定) */
static BOOL app_kept( INT i )
{
	UB	*j = NULL;
	INT	n;
	BOOL	kept = FALSE;

	n = app_json(i, &j);
	kept = ( n > 0 ) ? ct_app_locked(j, n) : FALSE;
	free(j);
	return kept;
}

/* 起動アプリの固定 turned over: the link's own applist says whether it keeps to its program */
static void app_keep( INT i )
{
	UB	*j = NULL, *out = NULL;
	CTBUF	b;
	INT	n;
	BOOL	keep;
	char	s[120];

	n = app_json(i, &j);
	if ( j != NULL ) out = malloc((size_t)n + 64);	/* with "defaultOpenLock" put in */
	if ( j != NULL && out != NULL ) {
		keep = !ct_app_locked(j, n);
		ct_buf(&b, out, n + 64);
		if ( n > 0 && ct_app_lock(&b, j, n, keep) >= E_OK && link_attr(i, "applist", b.p) ) {
			snprintf(s, sizeof(s), "%s %s", keep ? "kept" : "let go", g_l[i].name);
			note(s);
			say(keep ? "起動アプリを固定しました" : "起動アプリを最新にしました");
		} else {
			say("起動アプリを持たない実身です");
		}
	}
	free(j);
	free(out);
	g_dirty = TRUE;
}

/*
 * One of the 仮身操作 and 実身操作 on link i, by its code: the desktop is
 * asked, and what it answers the link is to become is written into the
 * month. The object a link points at may change its name, and with it
 * whether it is a day's: every link's name is read again.
 */
static void link_op( CONST char *code, INT i, INT index )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[160];
	INT	k;
	ER	er;

	if ( i < 0 || i >= g_nl ) return;
	memset(&an, 0, sizeof(an));
	if ( strcmp(code, "vobj.open") == 0 ) {
		link_open(i);
	} else if ( strcmp(code, "vobj.close") == 0 ) {
		ask_of(i, DT_RQ_CLOSE, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "vobj.attr") == 0 ) {
		ask_of(i, DT_RQ_ATTR, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			link_look(i, &an);
			snprintf(s, sizeof(s), "attr %s", g_l[i].name);
			note(s);
			say("仮身属性を変更しました");
		}
	} else if ( strcmp(code, "vobj.relation") == 0 ) {
		ask_of(i, DT_RQ_RELATION, &rq);
		(void)ct_attr_text(g_l[i].el, g_l[i].len, "relationship", rq.name, sizeof(rq.name));
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			an.text[DT_TEXT_MAX - 1] = 0;
			(void)link_attr(i, "relationship", ( an.text[0] != 0 ) ? an.text : NULL);
			snprintf(s, sizeof(s), "relation %s [%s]", g_l[i].name, (char *)an.text);
			note(s);
			say("続柄を変更しました");
		}
	} else if ( strcmp(code, "vobj.reconnect") == 0 ) {
		T_OBREF	ref;

		if ( ob_ref_obj(&g_l[i].target, &ref) < E_OK ) {
			say("実身に接続できませんでした");
		}
		for ( k = 0; k < g_nl; k++ ) name_of(&g_l[k].target, g_l[k].name);
		layout();
	} else if ( strcmp(code, "real.rename") == 0 ) {
		ask_of(i, DT_RQ_RENAME, &rq);
		if ( ask_desktop(&rq, &an, 30 * 60 * 1000) >= E_OK ) {
			for ( k = 0; k < g_nl; k++ ) name_of(&g_l[k].target, g_l[k].name);
			an.text[DT_TEXT_MAX - 1] = 0;
			snprintf(s, sizeof(s), "renamed %s", (char *)an.text);
			note(s);
			(void)month_save();	/* a day's link that is a day's no more keeps its place */
			layout();
		}
	} else if ( strcmp(code, "real.duplicate") == 0 || strcmp(code, "real.newversion") == 0 ) {
		BOOL	dup = (BOOL)( code[5] == 'd' );

		ask_of(i, dup ? DT_RQ_DUPLICATE : DT_RQ_NEWVER, &rq);
		er = ask_desktop(&rq, &an, 30 * 60 * 1000);
		if ( er >= E_OK ) {
			link_copy_put(i, &an.into, dup ? STEP : STEP / 2);
			snprintf(s, sizeof(s), "%s %s", dup ? "duplicated" : "new version", g_l[i].name);
			note(s);
			say(dup ? "実身を複製しました" : "新版を作成しました");
		}
	} else if ( strcmp(code, "real.lock") == 0 || strcmp(code, "real.unlock") == 0 ) {
		app_keep(i);
	} else if ( strcmp(code, "real.info") == 0 ) {
		ask_of(i, DT_RQ_INFO, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.network") == 0 ) {
		ask_of(i, DT_RQ_NETWORK, &rq);
		(void)ask_desktop(&rq, &an, 5000);
	} else if ( strcmp(code, "real.search") == 0 ) {
		ask_of(i, DT_RQ_SEARCH, &rq);
		(void)ask_desktop(&rq, &an, 30 * 60 * 1000);
	} else if ( strcmp(code, "exec.app") == 0 ) {
		if ( index < 0 || index >= g_napp ) return;
		ask_of(i, DT_RQ_EXEC, &rq);
		snprintf((char *)rq.name, sizeof(rq.name), "%s", (char *)g_app_id[index]);
		if ( app_kept(i) ) rq.flags |= DT_F_KEEP;
		if ( g_app_kind[index] ) rq.flags |= DT_F_KIND;
		er = ask_desktop(&rq, &an, 5000);
		snprintf(s, sizeof(s), "exec %s %s (%d)", g_l[i].name, (char *)g_app_id[index], (INT)er);
		note(s);
	}
	g_dirty = TRUE;
}

/* 背景色変更: the paper of the 予定表 object, asked of the desktop and kept there */
static void paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[40];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PAPER;
	rq.target = g_cal;
	rq.look.bgcol = g_paper;
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) < E_OK ) return;
	g_paper = an.look.bgcol & 0x00FFFFFFU;
	snprintf(s, sizeof(s), "paper #%06x", (UINT)g_paper);
	note(s);
	say("背景色を変更しました");
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the person */

static void finish( void )
{
	g_closing = TRUE;
}

/* 元の実身に保存: the 予定表 object and the month written again */
static void save_all( void )
{
	ER	er = E_OK;

	if ( g_calx != NULL ) er = rec_write(&g_cal, g_calx, g_caln);
	if ( er >= E_OK ) er = month_save();
	if ( er >= E_OK ) {
		note("saved");
		say("保存しました");
	}
}

static void command( CONST char *code, INT on, INT index )
{
	INT	n;
	char	s[80];

	if ( strcmp(code, "close") == 0 ) finish();
	else if ( strcmp(code, "save") == 0 ) save_all();
	else if ( strcmp(code, "prev") == 0 ) month_step(-1);
	else if ( strcmp(code, "next") == 0 ) month_step(1);
	else if ( strcmp(code, "today") == 0 ) {
		today();
		month_show(g_ty, g_tm);
	} else if ( strcmp(code, "month") == 0 ) ask_month();
	else if ( strcmp(code, "refresh") == 0 ) {
		today();
		cal_read();
		hol_read();
		paper_read();
		month_show(g_y, g_m);
		say("再表示しました");
	} else if ( strcmp(code, "fullscreen") == 0 ) fullscreen();
	else if ( strcmp(code, "bgcolour") == 0 ) paper_ask();
	else if ( strcmp(code, "hidden") == 0 ) {
		g_show_hidden = (BOOL)!g_show_hidden;
		note(g_show_hidden ? "hidden shown" : "hidden not shown");
	} else if ( strcmp(code, "undo") == 0 ) undo_do();
	else if ( strcmp(code, "copy") == 0 || strcmp(code, "cut") == 0 ) {
		BOOL	cut = (BOOL)( code[1] == 'u' );

		n = tray_copy();
		if ( n > 0 && cut ) (void)delete_picked("cut");
		if ( n > 0 ) {
			snprintf(s, sizeof(s), "%s %d", cut ? "cut" : "copied", n);
			note(s);
			snprintf(s, sizeof(s), "%d個の仮身をトレーへ%s", n, cut ? "移動しました" : "複写しました");
			say(s);
		} else if ( picked() == 0 ) {
			say("仮身が選択されていません");
		}
	} else if ( strcmp(code, "paste") == 0 ) tray_paste(FALSE);
	else if ( strcmp(code, "moveback") == 0 ) tray_paste(TRUE);
	else if ( strcmp(code, "tray.level") == 0 ) (void)tr_sel_dat(index + 1);
	else if ( strcmp(code, "tray.clear") == 0 ) (void)tr_clr_tra();
	else if ( strcmp(code, "open") == 0 ) {
		for ( n = 0; n < g_nl; n++ ) {
			if ( g_l[n].pick ) link_open(n);
		}
	} else if ( strcmp(code, "print.month") == 0 ) (void)print_months(FALSE);
	else if ( strcmp(code, "print.all") == 0 ) (void)print_months(TRUE);
	else if ( strcmp(code, "delete") == 0 ) {
		n = delete_picked("deleted");
		if ( n > 0 ) say("仮身を削除しました");
	} else if ( on >= 0 ) {
		link_op(code, on, index);
	}
	layout();
	g_dirty = TRUE;
}

/*
 * The window's menu at a place: on a link, it is taken (alone, unless
 * it is one of what is taken) and the 仮身操作, 実身操作 and 実行 are
 * about it; elsewhere they are grey. What cannot be done now is grey.
 */
static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	s;
	ID	mid;
	BOOL	chosen, kept = FALSE, cut = FALSE;
	INT	i = link_at(m->x, m->y), npick;

	if ( i >= 0 && !g_l[i].pick ) {
		pick_none();
		g_l[i].pick = TRUE;
	}
	paint();
	if ( ui_uuid(SYSDEF_MENU_CALENDAR, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	npick = picked();
	(void)mn_chg_atr(mid, NULL, "save.new", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "trash", MN_HIDE);
	(void)mn_chg_atr(mid, NULL, "fullscreen", g_full ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "hidden", g_show_hidden ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "undo", ( g_nundo == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "copy", ( npick == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "cut", ( npick == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "delete", ( npick == 0 ) ? MN_GREY : 0);
	i = ( tray_has_links() ) ? 0 : MN_GREY;
	(void)mn_chg_atr(mid, NULL, "paste", (UINT)i);
	(void)mn_chg_atr(mid, NULL, "moveback", (UINT)i);
	tray_menu(mid);
	i = link_at(m->x, m->y);
	if ( i < 0 ) {
		(void)mn_chg_atr(mid, NULL, "vobj", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "real", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "exec", MN_GREY);
	} else {
		T_OBREF	ref;

		cut = (BOOL)( ob_ref_obj(&g_l[i].target, &ref) < E_OK );
		kept = app_kept(i);
		(void)mn_chg_atr(mid, NULL, "vobj.reconnect", cut ? 0 : MN_HIDE);
		(void)mn_chg_atr(mid, NULL, "real.lock", kept ? MN_HIDE : cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.unlock", !kept ? MN_HIDE : cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.rename", cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.duplicate", cut ? MN_GREY : 0);
		(void)mn_chg_atr(mid, NULL, "real.newversion", cut ? MN_GREY : 0);
		exec_menu(mid, i);
		(void)mn_chg_atr(mid, NULL, "exec", ( cut || g_napp == 0 ) ? MN_GREY : 0);
	}
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) command((CONST char *)s.code, i, s.index);
}

static void key( CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, 0);

	if ( ( m->mods & M_CTRL ) != 0 ) {
		switch ( c ) {
		case 'e': case 'E':	command("close", -1, 0);	break;
		case 's': case 'S':	command("save", -1, 0);		break;
		case 'o': case 'O':	command("open", -1, 0);		break;
		case 'l': case 'L':	command("fullscreen", -1, 0);	break;
		case 'c': case 'C':	command("copy", -1, 0);		break;
		case 'x': case 'X':	command("cut", -1, 0);		break;
		case 'v': case 'V':	command("paste", -1, 0);	break;
		case 'z': case 'Z':	command("moveback", -1, 0);	break;
		case 'p': case 'P':	command("print.month", -1, 0);	break;
		default:		break;
		}
		return;
	}
	switch ( m->code ) {
	case K_PGUP:	command("prev", -1, 0);		break;
	case K_PGDN:	command("next", -1, 0);		break;
	case K_HOME:	command("today", -1, 0);	break;
	case K_DEL:	command("delete", -1, 0);	break;
	case K_ENTER:
	case K_KPENTER:	command("open", -1, 0);		break;
	case K_ESC:
		pick_none();
		g_dirty = TRUE;
		break;
	default:
		break;
	}
}

/*
 * The one link picked when a place is just out of its right edge or its
 * foot, no farther than slop: what a finger's press there is to size.
 * -1 when it is not.
 */
static INT link_near( INT x, INT y, INT slop )
{
	INT	i, k = -1, n = 0;

	for ( i = 0; i < g_nl; i++ ) {
		if ( g_l[i].pick ) {
			k = i;
			n++;
		}
	}
	if ( n != 1 || !g_l[k].shown || g_l[k].day ) return -1;
	{
		CONST T_DPRECT	*r = &g_l[k].r;

		if ( x >= r->left && x < r->right + slop && y >= r->top && y < r->bottom + slop
		  && ( x >= r->right || y >= r->bottom ) ) return k;
	}
	return -1;
}

static void press( CONST T_OBNTM *m )
{
	INT	i, d, slop;
	BOOL	add = (BOOL)( ( m->mods & ( M_CTRL | M_SHIFT ) ) != 0 );

	if ( m->y < STRIP_H ) return;			/* the strip's: its parts tell their own */
	g_band = FALSE;
	g_hold = HOLD_NONE;
	slop = wm_hit_slop(m->when);		/* a finger reaches the edges farther */
	i = link_at(m->x, m->y);
	if ( i < 0 && slop > 0 && !add ) i = link_near(m->x, m->y, slop);
	if ( i >= 0 ) {
		CLINK	*l = &g_l[i];

		if ( !l->pick ) slop = 0;		/* farther in only on one picked before the press */
		g_hold_twice = (BOOL)( i == g_press_link && m->when - g_press_at < DBL_NS );
		g_press_link = g_hold_twice ? -1 : i;
		g_press_day = 0;
		g_press_at = m->when;
		if ( add ) {
			l->pick = (BOOL)!l->pick;
		} else if ( !l->pick ) {
			pick_none();
			l->pick = TRUE;
		}
		g_hold_link = i;
		g_hold_moved = FALSE;
		g_hx0 = g_hx = m->x;
		g_hy0 = g_hy = m->y;
		g_hold_right = (BOOL)( !l->day && m->x >= l->r.right - wm_hit_in(EDGE, slop, l->r.right - l->r.left) );
		g_hold_foot = (BOOL)( !l->day && m->y >= l->r.bottom - wm_hit_in(EDGE, slop, l->r.bottom - l->r.top) );
		if ( !l->pick ) {
			g_hold = HOLD_NONE;		/* let go of by Shift or Ctrl: nothing to carry */
		} else if ( ( g_hold_right || g_hold_foot ) && !g_hold_twice ) {
			pick_none();
			l->pick = TRUE;
			g_hold = HOLD_SIZE;
		} else {
			g_hold = l->day ? HOLD_DAY : HOLD_MOVE;
		}
		g_dirty = TRUE;
		return;
	}
	g_press_link = -1;
	if ( !add ) pick_none();
	d = day_at(m->x, m->y);
	if ( d > 0 && d == g_press_day && m->when - g_press_at < DBL_NS ) {
		g_press_day = 0;
		day_make(d);
		layout();
		g_dirty = TRUE;
		return;
	}
	g_press_day = d;
	g_press_at = m->when;
	if ( d > 0 ) {
		/* a rectangle begins in the day: pulled out far enough, it makes the day */
		g_band = TRUE;
		g_band_day = d;
		g_bx0 = g_bx1 = m->x;
		g_by0 = g_by1 = m->y;
	}
	g_dirty = TRUE;
}

static void moved( CONST T_OBNTM *m )
{
	if ( g_hold != HOLD_NONE ) {
		g_hx = m->x;
		g_hy = m->y;
		if ( g_hx - g_hx0 >= MOVE_MIN || g_hx0 - g_hx >= MOVE_MIN
		  || g_hy - g_hy0 >= MOVE_MIN || g_hy0 - g_hy >= MOVE_MIN ) {
			g_hold_moved = TRUE;
		}
		if ( g_hold_moved ) g_dirty = TRUE;
		/* the hand held carries on outside the work area: what is taken goes out */
		if ( ( g_hold == HOLD_MOVE || g_hold == HOLD_DAY )
		  && ( m->x < 0 || m->y < 0 || m->x >= g_w || m->y >= g_h ) ) {
			left();
		}
		return;
	}
	if ( !g_band ) return;
	g_bx1 = m->x;
	g_by1 = m->y;
	g_dirty = TRUE;
}

/* The pointer gone out of the window with what is taken held: carried out */
static void left( void )
{
	if ( g_hold == HOLD_MOVE || g_hold == HOLD_DAY ) {
		g_hold_moved = FALSE;		/* the outline inside is the desktop's now */
		paint();
		carry_out();
		layout();
		g_dirty = TRUE;
	}
}

static void released( CONST T_OBNTM *m )
{
	INT	dx, dy, hold = g_hold;

	if ( hold != HOLD_NONE ) {
		g_hold = HOLD_NONE;
		g_hx = m->x;
		g_hy = m->y;
		if ( hold == HOLD_SIZE && g_hold_moved ) {
			size_link(g_hold_link);
		} else if ( hold == HOLD_MOVE && g_hold_moved && !g_hold_twice ) {
			move_picked(g_hx - g_hx0, g_hy - g_hy0);
		} else if ( !g_hold_moved && g_hold_twice ) {
			link_open(g_hold_link);
		}
		g_dirty = TRUE;
		return;
	}
	if ( !g_band ) return;
	g_band = FALSE;
	dx = m->x - g_bx0;
	dy = m->y - g_by0;
	if ( dx < 0 ) dx = -dx;
	if ( dy < 0 ) dy = -dy;
	if ( dx >= BAND_MIN && dy >= BAND_MIN ) {
		g_press_day = 0;
		day_make(g_band_day);
		layout();
	}
	g_dirty = TRUE;
}

/* What the strip's parts were made to do */
static void strip_event( CONST T_OBNTM *m )
{
	INT	num = 0;

	if ( ui_event(&g_ui, m, &num) != UI_EV_BUTTON ) return;
	switch ( num ) {
	case P_PREV:	command("prev", -1, 0);		break;
	case P_NEXT:	command("next", -1, 0);		break;
	case P_TODAY:	command("today", -1, 0);	break;
	case P_MONTH:	command("month", -1, 0);	break;
	default:	break;
	}
}

static void event( CONST T_OBNTM *m )
{
	if ( g_strip && same_uuid(&m->uuid, &g_ui.pnl) ) {
		strip_event(m);
		return;
	}
	if ( g_lport > 0 && same_uuid(&m->uuid, &g_lch) ) {
		links_told();
		return;
	}
	if ( !same_uuid(&m->uuid, &g_win) ) return;
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		finish();
		break;
	case OB_E_PRESS:
		if ( m->code == 1 && g_hold == HOLD_NONE ) menu(m);
		else if ( m->code == 0 ) press(m);
		break;
	case OB_E_RELEASE:
		if ( m->code == 0 ) released(m);
		break;
	case OB_E_MOVE:
		moved(m);
		break;
	case OB_E_LEAVE:
		left();
		break;
	case OB_E_KEY:
		key(m);
		break;
	case OB_E_REDRAW:
		resized();
		break;
	case OB_E_DROP:
		dropped();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBCRE	c;
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0;
	char	title[OB_NAME_MAX + 8], s[120];

	if ( ts_get_arg(&g_cal, sizeof(g_cal)) < (SZ)sizeof(g_cal) || ob_ref_obj(&g_cal, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("予定表は、予定表の実身を開いて使います");
		return 1;
	}
	g_autoref = (BOOL)( ( ref.flags & OB_F_AUTOREF ) != 0 );
	snprintf(title, sizeof(title), "%s", (char *)ref.name);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port > 0 ) links_channel();
	if ( g_port <= 0 ) {
		note("no channel");
		return 2;
	}
	cal_read();
	if ( g_calx == NULL ) {
		note("the object cannot be read");
		(void)wm_msg_put("予定表の実身を読めません");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	hol_read();
	paper_read();
	today();
	if ( win_open(title) < E_OK ) {
		note("no window");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	if ( strip_open() < E_OK ) note("the strip cannot be placed");
	month_show(g_ty, g_tm);
	layout();
	snprintf(s, sizeof(s), "ready %s holidays %d%s", title, g_nhol, g_has_tpl ? " template" : "");
	note(s);
	say("日のセルで矩形を引くと、その日の予定の実身を作ります");
	paint();

	while ( !g_closing ) {
		for ( TMO tmo = TMO_FEVR; !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) event(&m);
		links_told();		/* what a panel's wait took the notice of is read too */
		if ( g_dirty && !g_closing ) paint();
	}
	if ( g_lport > 0 ) {
		ob_cls_obj(g_lport);
		(void)ob_del_obj(&g_lch);
	}
	links_free();
	undo_clear();
	strip_close();
	if ( g_kw > 0 ) ob_cls_obj(g_kw);
	(void)ob_del_obj(&g_win);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	free(g_calx);
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}
