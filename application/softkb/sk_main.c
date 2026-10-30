/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sk_main.c
 *	ソフトウェアキーボード: a keyboard on the screen to type with the
 *	pointer or a touch panel (design 11.31)
 *
 *	A window that never has the keys (WM_ATTR_NOFOCUS) and floats in
 *	front of the others (WM_ATTR_FLOAT), filled with the keys of a
 *	layout: a key pressed on it is typed into the window that has the
 *	keys (wm_key_put), so that a text, a part's text box or a process's
 *	window takes it as a key of the keyboard, かな漢字変換 included: the
 *	converter works where the key arrives, in the input mode the reader
 *	of the input sets as the key passes it. The keys grow and shrink with
 *	the window, which can be sized as the person likes, so that they are
 *	as big as a finger wants.
 *
 *	The layouts are tables (SKPAGE):
 *	- the English letters (QWERTY): rows of keys, each with its width in
 *	  quarters of a key, drawn with the drawing environment's boxes and
 *	  words;
 *	- the TRON keyboard (sk_tron.c): keys with the outlines they have on
 *	  the keyboard, turned and set off as its halves are, written with
 *	  what they type (km_label);
 *	- the μTRON keyboard (sk_utron.c): its layout chart as it is, every
 *	  mark on the keys an outline, in two windows, one at each edge of
 *	  the screen, a half of the keyboard in each.
 *	The keys by shape are drawn in the window's surface with their edges
 *	smoothed (sk_draw.c), and put in the window.
 *
 *	The modifiers and Fn pressed on the picture hold for the next key;
 *	pressed twice, until pressed again. The input mode key turns 日本語
 *	on (ひらがなカタカナ) and off (英数), and is written with the mode as
 *	it stands (kc_mode, and the mode each key seen carries). The keys
 *	held on the keyboard itself are lit (OB_N_KEYSEEN). The layout chosen
 *	is kept in the user information object (SYSDEF_CONF_USER), as the
 *	line SOFTKB_LAYOUT. Choosing a TRON keyboard switches the machine's
 *	layouts to Dvorak and TRON かな配列, as キー配列 does, and the English
 *	letters put back what there was (tron_on, tron_off).
 *
 *	What is done is said on the console ("softkb: ..."), which the tests
 *	read. Started with "selftest", the program checks its tables and the
 *	calls it is made of, and ends.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/keymap.h>
#include <ts/conf.h>
#include "sk.h"

#define LOOK_MS		500		/* the input mode looked at again, for a change made elsewhere */
#define FLASH_MS	150		/* how long a key pressed on the picture stays lit */

/* ---------------------------------------------------------------- the layouts */

#define SK_ROWQ		60		/* every row is this many quarters wide */
#define SK_NROW		5

#define K(c, w)		{ (c), (w), SK_KEY, SKF_L, NULL }
#define KR(c, w)	{ (c), (w), SK_KEY, SKF_R, NULL }
#define N(c, w, f, s)	{ (c), (w), SK_KEY, (f), (s) }
#define MOD(c, w, f, s)	{ (c), (w), SK_MOD, (f), (s) }
#define ROW		{ 0, 0, SK_ROW, 0, NULL }

/* The modifiers, the thumbs' row and the arrows: the same on both keyboards */
#define SK_BOTTOM \
	MOD(0xE0, 5, SKF_L, "Ctrl"), MOD(0xE2, 4, SKF_L, "Alt"), N(0x8B, 5, SKF_L, "無変換"), \
	N(0x2C, 19, SKF_L, ""), N(0x8A, 5, SKF_R, "変換"), { 0x88, 6, SK_IME, SKF_R, NULL }, \
	N(0x50, 4, SKF_R, "←"), N(0x52, 4, SKF_R, "↑"), N(0x51, 4, SKF_R, "↓"), \
	N(0x4F, 4, SKF_R, "→"), ROW

/* The Japanese keyboard's letters and signs, in the places Windows 11's touch keyboard has them */
static CONST SKKEY	sk_qwerty_jp[] = {
	N(0x29, 4, SKF_L, "Esc"), K(0x1E, 4), K(0x1F, 4), K(0x20, 4), K(0x21, 4), K(0x22, 4),
	KR(0x23, 4), KR(0x24, 4), KR(0x25, 4), KR(0x26, 4), KR(0x27, 4), KR(0x2D, 4), KR(0x2E, 4),
	KR(0x89, 4), N(0x2A, 4, SKF_R, "BS"), ROW,
	N(0x2B, 6, SKF_L, "Tab"), K(0x14, 4), K(0x1A, 4), K(0x08, 4), K(0x15, 4), K(0x17, 4),
	KR(0x1C, 4), KR(0x18, 4), KR(0x0C, 4), KR(0x12, 4), KR(0x13, 4), KR(0x2F, 4), KR(0x30, 4),
	KR(0x32, 6), ROW,
	N(0x39, 7, SKF_L, "英数"), K(0x04, 4), K(0x16, 4), K(0x07, 4), K(0x09, 4), K(0x0A, 4),
	KR(0x0B, 4), KR(0x0D, 4), KR(0x0E, 4), KR(0x0F, 4), KR(0x33, 4), KR(0x34, 4),
	N(0x28, 9, SKF_R, "Enter"), ROW,
	MOD(0xE1, 9, SKF_L, "Shift"), K(0x1D, 4), K(0x1B, 4), K(0x06, 4), K(0x19, 4), K(0x05, 4),
	KR(0x11, 4), KR(0x10, 4), KR(0x36, 4), KR(0x37, 4), KR(0x38, 4), KR(0x87, 4),
	MOD(0xE5, 7, SKF_R, "Shift"), ROW,
	SK_BOTTOM
};

/* The English keyboard's */
static CONST SKKEY	sk_qwerty_us[] = {
	N(0x29, 4, SKF_L, "Esc"), K(0x35, 4), K(0x1E, 4), K(0x1F, 4), K(0x20, 4), K(0x21, 4),
	K(0x22, 4), KR(0x23, 4), KR(0x24, 4), KR(0x25, 4), KR(0x26, 4), KR(0x27, 4), KR(0x2D, 4),
	KR(0x2E, 4), N(0x2A, 4, SKF_R, "BS"), ROW,
	N(0x2B, 6, SKF_L, "Tab"), K(0x14, 4), K(0x1A, 4), K(0x08, 4), K(0x15, 4), K(0x17, 4),
	KR(0x1C, 4), KR(0x18, 4), KR(0x0C, 4), KR(0x12, 4), KR(0x13, 4), KR(0x2F, 4), KR(0x30, 4),
	KR(0x31, 6), ROW,
	N(0x39, 7, SKF_L, "英数"), K(0x04, 4), K(0x16, 4), K(0x07, 4), K(0x09, 4), K(0x0A, 4),
	KR(0x0B, 4), KR(0x0D, 4), KR(0x0E, 4), KR(0x0F, 4), KR(0x33, 4), KR(0x34, 4),
	N(0x28, 9, SKF_R, "Enter"), ROW,
	MOD(0xE1, 9, SKF_L, "Shift"), K(0x1D, 4), K(0x1B, 4), K(0x06, 4), K(0x19, 4), K(0x05, 4),
	KR(0x11, 4), KR(0x10, 4), KR(0x36, 4), KR(0x37, 4), KR(0x38, 4),
	MOD(0xE5, 11, SKF_R, "Shift"), ROW,
	SK_BOTTOM
};

/* How a page's keys are laid out */
#define PK_ROWS		0		/* in rows */
#define PK_SHAPE	1		/* by shape, written with what they type (SKPLACE) */
#define PK_CHART	2		/* as a chart draws them, every mark an outline (SKVKEY) */

/*
 * A page of keys. The layouts to come -- the kana, the numbers and signs
 * -- are pages of their own, chosen by name.
 */
typedef struct {
	CONST char	*code;		/* the menu's code that chooses it */
	CONST char	*key;		/* its word in the line SOFTKB_LAYOUT */
	INT		kind;		/* PK_* */
	BOOL		split;		/* its halves in two windows at the screen's edges */
	BOOL		tron;		/* typed in Dvorak and TRON かな配列, switched to when chosen */
	INT		w0, h0;		/* the work area it opens with (each window's, when split) */
} SKPAGE;

#define PG_QWERTY	0
#define PG_TRON		1
#define PG_UTRON	2
static CONST SKPAGE	sk_pages[] = {
	{ "layout.qwerty", "qwerty", PK_ROWS, FALSE, FALSE, 640, 232 },
	{ "layout.tron", "tron", PK_SHAPE, FALSE, TRUE, 760, 310 },
	{ "layout.utron", "utron", PK_CHART, TRUE, TRUE, 380, 290 },
};
#define SK_NPAGE	( (INT)( sizeof(sk_pages) / sizeof(sk_pages[0]) ) )

#define SK_KEYU		400		/* a key of a page by shape, roughly, in its plane's tenths */
#define UT_KEYU		375		/* and of the μTRON keyboard */
#define UT_ROOM		60		/* the chart's ground left round a half, in tenths */
#define CONF_KEY	"SOFTKB_LAYOUT"

/* ---------------------------------------------------------------- the windows */

#define SK_M		4		/* the margin round the keys */
#define SK_WMIN		360		/* the least the work area may be */
#define SK_HMIN		140
#define SK_WMIN_HALF	200		/* and a half's */
#define SK_HMIN_HALF	150

#define C_GROUND	0x00E8E8E8U
#define C_KEY		0x00FFFFFFU
#define C_PLATE		SK_UT_GROUND	/* a μTRON half under its keys: the same light grey as round the halves */
#define C_FUNC		0x00F4F4F4U	/* a key that types no letter */
#define C_EDGE		0x00808080U
#define C_TEXT		0x00000000U
#define C_HELD		0x0090C8FFU
#define C_ONCE		0x00FFE070U	/* a modifier held for the next key */
#define C_LOCK		0x00FFB060U	/* and held until pressed again */
#define C_JP		0x00C00000U	/* the input mode key in 日本語 */
#define C_MODE		0x00FFE0E0U	/* the face of the key of the input mode in use, on the chart */
#define C_RED		0x00D00000U	/* the face of the left Shift */
#define C_BLUE		0x000040D0U	/* and of the right */

#define M_SHIFT		( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
/* the modifiers that make a key a command: Ctrl, Alt, the Windows keys */
#define M_CMD		( HID_MOD_LCTRL | HID_MOD_RCTRL | HID_MOD_LALT | HID_MOD_RALT | HID_MOD_LMETA | HID_MOD_RMETA )

/* What a window shows: the whole keyboard, or one of its halves */
#define PART_ALL	0
#define PART_L		1
#define PART_R		2

typedef struct {
	BOOL		open;
	TS_UUID		win;
	ID		kw;
	INT		gid;
	INT		w, h;		/* its work area */
	INT		part;		/* PART_* */
	BOOL		edge;		/* a half held to its edge of the screen (not moved away by the person) */
	INT		lastx, lasty;	/* where its frame stood when last placed or looked at */
} SKVIEW;

static TS_UUID		g_ch;
static ID		g_port = 0;
static SKVIEW		g_v[2];
static INT		g_nv = 0;
static BOOL		g_closing = FALSE;
static BOOL		g_dirty = TRUE;

static T_KMSTATE	g_st;			/* the keyboard of the machine's settings */
static INT		g_page = 0;		/* sk_pages[] */
static UB		g_down[32];		/* the keys held on the keyboard, a bit each */
static UINT		g_mods = 0;		/* the modifiers held there, as last told */
static UINT		g_once = 0;		/* modifiers pressed on the picture, for the next key */
static UINT		g_lock = 0;		/* and those pressed twice, until pressed again */
static INT		g_fn = 0;		/* Fn on the picture: 0 off, 1 the next key, 2 until pressed again */
static UINT		g_mode = 0;		/* the input mode, WM_MODE_* */
static UINT		g_flash = 0;		/* the key just pressed on the picture, lit a moment */
static UD		g_flash_end = 0;

/* ---------------------------------------------------------------- words */

static void say( CONST char *fmt, ... )
{
	char	s[160];
	va_list	ap;
	INT	n;

	n = snprintf(s, sizeof(s), "softkb: ");
	va_start(ap, fmt);
	vsnprintf(s + n, sizeof(s) - (size_t)n, fmt, ap);
	va_end(ap);
	strncat(s, "\n", sizeof(s) - strlen(s) - 1);
	tm_putstring((CONST UB *)s);
}

static BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(*a)) == 0 );
}

static CONST SKPAGE *page( void )
{
	return &sk_pages[g_page];
}

static BOOL held( UINT code )
{
	return (BOOL)( code < 256 && ( g_down[code >> 3] & ( 1U << ( code & 7 ) ) ) != 0 );
}

static BOOL mode_jp( void )
{
	return (BOOL)( ( g_mode & WM_MODE_ALPH ) == 0 );
}

/* The input mode as the message line shows it, read again */
static void mode_read( void )
{
	UINT	m = kc_mode();

	if ( m != g_mode ) {
		g_mode = m;
		g_dirty = TRUE;
	}
}

/* ---------------------------------------------------------------- the keys' places */

#define SK_MAXPT	48

/*
 * A key as it stands in a window: what it does, its outline in pixels
 * (four corners in rows, eight for the TRON keyboard, its chart's for the
 * μTRON keyboard) and the box round it
 */
typedef struct {
	INT		idx;			/* its place in the page's table */
	UB		code, fn, act, flags;
	CONST char	*name;
	UB		kan;
	char		pl, red, blue;
	INT		n;
	INT		x[SK_MAXPT], y[SK_MAXPT];
	T_DPRECT	box;
	INT		cx, cy;			/* its middle */
} SKONE;

/* Where quarter q of a row starts, and where row r */
static INT at_x( CONST SKVIEW *v, INT q )
{
	return SK_M + q * ( v->w - 2 * SK_M ) / SK_ROWQ;
}

static INT at_y( CONST SKVIEW *v, INT r )
{
	return SK_M + r * ( v->h - 2 * SK_M ) / SK_NROW;
}

/*
 * A page by shape: the part of its plane the window shows (rx, ry, rw,
 * rh, in tenths), as large as the work area lets it be, in the middle. A
 * point (x, y) of the plane is at ox + ( x - rx ) * num / den.
 */
typedef struct {
	INT	rx, ry, rw, rh;
	INT	num, den, ox, oy;
} SKSCALE;

static void region_of( CONST SKVIEW *v, SKSCALE *s )
{
	if ( page()->kind == PK_SHAPE ) {
		s->rx = 0;
		s->ry = 0;
		s->rw = SK_TRON_W;
		s->rh = SK_TRON_H;
		return;
	}
	if ( v->part == PART_R ) {
		s->rx = sk_ut_half[1].l - UT_ROOM;
	} else {
		s->rx = sk_ut_half[0].l - UT_ROOM;
	}
	s->ry = sk_ut_half[0].t - UT_ROOM;
	s->rw = ( v->part == PART_ALL ) ? sk_ut_half[1].r - sk_ut_half[0].l + 2 * UT_ROOM
		: sk_ut_half[0].r - sk_ut_half[0].l + 2 * UT_ROOM;
	s->rh = sk_ut_half[0].b - sk_ut_half[0].t + 2 * UT_ROOM;
}

static void scale_of( CONST SKVIEW *v, SKSCALE *s )
{
	INT	aw = v->w - 2 * SK_M, ah = v->h - 2 * SK_M;

	region_of(v, s);
	if ( aw < 1 ) aw = 1;
	if ( ah < 1 ) ah = 1;
	if ( aw * s->rh <= ah * s->rw ) {
		s->num = aw;
		s->den = s->rw;
	} else {
		s->num = ah;
		s->den = s->rh;
	}
	s->ox = SK_M + ( aw - s->rw * s->num / s->den ) / 2;
	s->oy = SK_M + ( ah - s->rh * s->num / s->den ) / 2;
}

/* The size of a key in pixels: a row's height in rows, a key's of the plane by shape */
static INT key_px( CONST SKVIEW *v )
{
	SKSCALE	s;

	if ( page()->kind == PK_ROWS ) return ( v->h - 2 * SK_M ) / SK_NROW;
	scale_of(v, &s);
	return ( ( page()->kind == PK_CHART ) ? UT_KEYU : SK_KEYU ) * s.num / s.den;
}

/* A point of the plane in the window, in sixteenths of a pixel */
static INT fx( CONST SKSCALE *s, INT x )
{
	return s->ox * SK_FIX + (INT)( (D)( x - s->rx ) * s->num * SK_FIX / s->den );
}

static INT fy( CONST SKSCALE *s, INT y )
{
	return s->oy * SK_FIX + (INT)( (D)( y - s->ry ) * s->num * SK_FIX / s->den );
}

/* Whether a key of the chart is in the half the window shows */
static BOOL in_view( CONST SKVIEW *v, UB flags )
{
	if ( v->part == PART_L ) return (BOOL)( ( flags & SKF_R ) == 0 );
	if ( v->part == PART_R ) return (BOOL)( ( flags & SKF_R ) != 0 );
	return TRUE;
}

static void one_box( SKONE *o )
{
	INT	j;

	o->cx = o->cy = 0;
	for ( j = 0; j < o->n; j++ ) {
		if ( j == 0 || o->x[j] < o->box.left ) o->box.left = o->x[j];
		if ( j == 0 || o->x[j] > o->box.right ) o->box.right = o->x[j];
		if ( j == 0 || o->y[j] < o->box.top ) o->box.top = o->y[j];
		if ( j == 0 || o->y[j] > o->box.bottom ) o->box.bottom = o->y[j];
		o->cx += o->x[j];
		o->cy += o->y[j];
	}
	o->cx /= o->n;
	o->cy /= o->n;
}

typedef void (*SKEACH)( CONST SKVIEW *v, CONST SKONE *k, void *arg );

static void sk_each( CONST SKVIEW *v, SKEACH f, void *arg )
{
	CONST SKPAGE	*p = page();
	SKONE		o;
	SKSCALE		s;
	INT		i, j;

	memset(&o, 0, sizeof(o));
	if ( p->kind == PK_SHAPE ) {
		scale_of(v, &s);
		for ( i = 0; i < sk_ntron; i++ ) {
			CONST SKPLACE	*k = &sk_tron[i];

			o.idx = i;
			o.code = k->code;
			o.fn = k->fn;
			o.act = k->act;
			o.flags = k->flags;
			o.name = k->name;
			o.n = SK_NPT;
			for ( j = 0; j < SK_NPT; j++ ) {
				o.x[j] = fx(&s, k->pt[j][0]) / SK_FIX;
				o.y[j] = fy(&s, k->pt[j][1]) / SK_FIX;
			}
			one_box(&o);
			f(v, &o, arg);
		}
		return;
	}
	if ( p->kind == PK_CHART ) {
		scale_of(v, &s);
		for ( i = 0; i < sk_ut_nkey; i++ ) {
			CONST SKVKEY	*k = &sk_ut_key[i];

			if ( !in_view(v, k->flags) ) continue;
			o.idx = i;
			o.code = k->code;
			o.fn = k->fn;
			o.act = k->act;
			o.flags = k->flags;
			o.name = k->name;
			o.kan = k->kan;
			o.pl = k->pl;
			o.red = k->red;
			o.blue = k->blue;
			o.n = ( k->n > SK_MAXPT ) ? SK_MAXPT : k->n;
			for ( j = 0; j < o.n; j++ ) {
				o.x[j] = fx(&s, sk_ut_pt[( k->off + j ) * 2]) / SK_FIX;
				o.y[j] = fy(&s, sk_ut_pt[( k->off + j ) * 2 + 1]) / SK_FIX;
			}
			one_box(&o);
			f(v, &o, arg);
		}
		return;
	}
	{
		CONST SKKEY	*k = g_st.us ? sk_qwerty_us : sk_qwerty_jp;
		INT		q = 0, row = 0;

		for ( i = 0; row < SK_NROW; k++ ) {
			if ( k->act == SK_ROW ) {
				q = 0;
				row++;
				continue;
			}
			o.idx = i++;
			o.code = k->code;
			o.fn = 0;
			o.act = k->act;
			o.flags = k->flags;
			o.name = k->name;
			o.n = 4;
			o.box.left = at_x(v, q) + 1;
			o.box.right = at_x(v, q + k->w) - 2;
			o.box.top = at_y(v, row) + 1;
			o.box.bottom = at_y(v, row + 1) - 2;
			o.x[0] = o.x[3] = o.box.left;
			o.x[1] = o.x[2] = o.box.right;
			o.y[0] = o.y[1] = o.box.top;
			o.y[2] = o.y[3] = o.box.bottom;
			o.cx = ( o.box.left + o.box.right ) / 2;
			o.cy = ( o.box.top + o.box.bottom ) / 2;
			f(v, &o, arg);
			q += k->w;
		}
	}
}

/* ---------------------------------------------------------------- the letters */

/* The letter a key types as it is in the latin layout set, 0 none */
static char key_letter( UINT code )
{
	char	b[8];

	if ( code == 0 || km_label(KM_L_CHAR, code, 0, b, sizeof(b)) != 1 ) return 0;
	return b[0];
}

/*
 * The logical layout (論理配列): which letter each place of the keys
 * types, the latin layout of the machine's settings (km_set), the same
 * for the keyboard itself and for every page of this one.
 */
static BOOL logical_dvorak( void )
{
	return (BOOL)( g_st.latin == KM_LATIN_DVORAK );
}

/* The place of a key of the μTRON chart: the usage of the key under it on the keyboard of the keys as written */
static UINT chart_place( CONST SKONE *k )
{
	return ( k->kan != 0 ) ? k->kan : k->code;
}

/*
 * Whether a key of the μTRON chart is written and typed by its place:
 * a key with a letter on the chart, when the logical layout is QWERTY
 * and the key's place types a letter there. In Dvorak the chart's own
 * letters are written, and typed as they are written.
 */
static BOOL chart_by_place( CONST SKONE *k )
{
	return (BOOL)( k->pl != 0 && !logical_dvorak() && key_letter(chart_place(k)) != 0 );
}

/* ---------------------------------------------------------------- drawing */

static void text_at( CONST SKVIEW *v, INT x, INT y, CONST char *s, UW c, INT px )
{
	(void)dp_text(v->gid, x, y, (CONST UB *)s, c, px);
}

/* Words with their middle at (x, y) */
static void centre_at( CONST SKVIEW *v, INT x, INT y, CONST char *s, UW c, INT px )
{
	text_at(v, x - dp_text_width((CONST UB *)s, px) / 2, y + px / 3, s, c, px);
}

/* The key's face, as what is held and pressed makes it */
static UW key_ground( CONST SKONE *k )
{
	UW	c = ( k->name != NULL || k->act != SK_KEY ) ? C_FUNC : C_KEY;

	if ( page()->kind == PK_CHART ) {
		c = C_KEY;
		if ( k->act == SK_KEY && k->name != NULL
		  && ( ( k->code == 0x88 && mode_jp() ) || ( k->code == 0x39 && !mode_jp() ) ) ) c = C_MODE;
	}
	if ( k->act == SK_MOD ) {
		UINT	bit = 1U << ( k->code - 0xE0 );

		if ( ( g_lock & bit ) != 0 ) c = C_LOCK;
		else if ( ( g_once & bit ) != 0 ) c = C_ONCE;
	} else if ( k->act == SK_FN ) {
		c = ( g_fn == 2 ) ? C_LOCK : ( g_fn == 1 ) ? C_ONCE : c;
	}
	if ( ( k->code != 0 && held(k->code) ) || ( g_flash != 0 && g_flash == k->code ) ) c = C_HELD;
	return c;
}

/* What Fn makes a key type, as it is written */
static CONST char *fn_label( UINT fn, char *buf, INT max )
{
	switch ( fn ) {
	case 0x35:	return "半/全";
	case 0x46:	return "PrtSc";
	case 0x47:	return "ScrLk";
	case 0x48:	return "Pause";
	case 0x9A:	return "SysRq";
	default:	break;
	}
	if ( km_label(KM_L_NAME, fn, 0, buf, max) <= 0 ) buf[0] = 0;
	return buf;
}

/* What a key is written with */
static CONST char *key_label( CONST SKONE *k, char *buf, INT max )
{
	BOOL	shift = (BOOL)( ( ( g_mods | g_once | g_lock ) & M_SHIFT ) != 0 );

	if ( k->act == SK_IME ) return mode_jp() ? "あ" : "A";
	if ( g_fn != 0 && k->fn != 0 ) return fn_label(k->fn, buf, max);
	if ( k->name != NULL ) return k->name;
	if ( km_label(KM_L_CHAR, k->code, shift ? HID_MOD_LSHIFT : 0, buf, max) > 0 ) return buf;
	if ( km_label(KM_L_NAME, k->code, 0, buf, max) <= 0 ) buf[0] = 0;
	return buf;
}

/* The ink of a key's words: the key of the input mode in use red */
static UW key_ink( CONST SKONE *k )
{
	if ( k->act == SK_IME ) return mode_jp() ? C_JP : C_TEXT;
	if ( page()->kind != PK_ROWS && k->act == SK_KEY && k->name != NULL
	  && ( ( k->code == 0x88 && mode_jp() ) || ( k->code == 0x39 && !mode_jp() ) ) ) return C_JP;
	return C_TEXT;
}

/* A key of the rows: its face and its words, as large as it lets them be */
static void draw_row_key( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	char		b[32];
	CONST char	*s;
	INT		px, rw = k->box.right - k->box.left, rh = k->box.bottom - k->box.top;

	(void)arg;
	(void)dp_fill_rect(v->gid, &k->box, key_ground(k));
	(void)dp_frame_rect(v->gid, &k->box, C_EDGE, 1);
	s = key_label(k, b, sizeof(b));
	if ( s[0] == 0 ) return;
	/* a letter as big as the key lets it be; a name smaller, and smaller still when it is wide */
	px = rh * ( ( ( (UB)s[0] < 0x80 && s[1] == 0 ) || k->act == SK_IME ) ? 11 : 8 ) / 20;
	if ( px > 32 ) px = 32;
	while ( px > 8 && dp_text_width((CONST UB *)s, px) > rw - 4 ) px--;
	centre_at(v, k->cx, k->cy, s, key_ink(k), px);
}

/*
 * The words of a key of the TRON keyboard. A key with a name has it in
 * the middle; a key with a letter has the letter at the top left, and
 * under it the kana it types: the faces of the left and the right Shift
 * side by side in red and blue, the plain face at the foot, as the TRON
 * keyboard is written (or the JIS kana alone, in JIS かな).
 */
static void draw_shape_words( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	char		b[32], f0[16], fl[16], fr[16];
	CONST char	*s;
	INT		kh = *(CONST INT *)arg, px;

	s = key_label(k, b, sizeof(b));
	if ( s[0] == 0 ) return;
	if ( k->name != NULL || ( g_fn != 0 && k->fn != 0 ) || k->act != SK_KEY ) {
		px = kh * 30 / 100;
		if ( px > 28 ) px = 28;
		while ( px > 7 && dp_text_width((CONST UB *)s, px) > kh * 85 / 100 ) px--;
		centre_at(v, k->cx, k->cy, s, ( k->code == 0xE1 ) ? C_RED : ( k->code == 0xE5 ) ? C_BLUE : key_ink(k),
			  px);
		return;
	}
	px = kh * 34 / 100;
	if ( px < 8 ) px = 8;
	text_at(v, k->cx - kh * 36 / 100, k->cy - kh * 8 / 100, s, C_TEXT, px);
	px = kh * 24 / 100;
	if ( px < 7 ) px = 7;
	if ( km_label(KM_L_KANA, k->code, 0, f0, sizeof(f0)) > 0 ) {
		if ( km_label(KM_L_KANA, k->code, KM_MOD_THUMB_L, fl, sizeof(fl)) > 0 ) {
			centre_at(v, k->cx - kh * 18 / 100, k->cy + kh * 6 / 100, fl, C_RED, px);
		}
		if ( km_label(KM_L_KANA, k->code, KM_MOD_THUMB_R, fr, sizeof(fr)) > 0 ) {
			centre_at(v, k->cx + kh * 18 / 100, k->cy + kh * 6 / 100, fr, C_BLUE, px);
		}
		centre_at(v, k->cx, k->cy + kh * 30 / 100, f0, C_TEXT, px);
	} else if ( km_label(KM_L_JIS, k->code, 0, f0, sizeof(f0)) > 0 ) {
		centre_at(v, k->cx + kh * 14 / 100, k->cy + kh * 26 / 100, f0, C_TEXT, px);
	}
}

/* ---- the keys by shape, in the surface */

#define XY_MAX		1024
#define CN_MAX		128

static INT	g_xy[XY_MAX * 2], g_in[XY_MAX * 2], g_cn[CN_MAX];

typedef struct {
	SKSURF		s;
	SKSCALE		sc;
	T_DPRECT	all;
	INT		line;		/* the keys' outline, in sixteenths of a pixel */
} SKPAINT;

/* An outline of the plane (n points from pt, x and y pairs) into g_xy, in sixteenths of a pixel */
static INT plane_pts( CONST SKSCALE *sc, CONST H *pt, INT n )
{
	INT	j;

	if ( n > XY_MAX ) n = XY_MAX;
	for ( j = 0; j < n; j++ ) {
		g_xy[j * 2] = fx(sc, pt[j * 2]);
		g_xy[j * 2 + 1] = fy(sc, pt[j * 2 + 1]);
	}
	return n;
}

/* A key's outline drawn: the line round it, and its face inside the line */
static void key_shape( SKPAINT *pp, INT n, UW edge, UW face )
{
	g_cn[0] = n;
	sk_fill(&pp->s, g_xy, g_cn, 1, edge, TRUE, &pp->all);
	sk_inset(g_xy, n, pp->line, g_in);
	sk_fill(&pp->s, g_in, g_cn, 1, face, TRUE, &pp->all);
}

static void paint_shape_key( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	SKPAINT	*pp = (SKPAINT *)arg;
	INT	n;

	(void)v;
	if ( page()->kind == PK_SHAPE ) {
		n = plane_pts(&pp->sc, &sk_tron_line[k->idx][0][0], SK_TRON_NL);
		key_shape(pp, n, C_EDGE, key_ground(k));
		return;
	}
	{
		CONST SKVKEY	*vk = &sk_ut_key[k->idx];
		INT		i, j, at;

		n = plane_pts(&pp->sc, &sk_ut_pt[vk->off * 2], vk->n);
		key_shape(pp, n, SK_UT_EDGE, key_ground(k));
		/* the marks on it, as the chart draws them */
		for ( i = 0; i < sk_ut_nmark; i++ ) {
			CONST SKVMARK	*m = &sk_ut_mark[i];
			INT		total = 0, nc = ( m->ncn > CN_MAX ) ? CN_MAX : m->ncn;

			if ( m->key != k->idx || chart_by_place(k) ) continue;
			for ( j = 0; j < nc; j++ ) {
				g_cn[j] = sk_ut_cn[m->cn + j];
				total += g_cn[j];
			}
			if ( total > XY_MAX ) continue;
			for ( at = 0; at < total; at++ ) {
				g_xy[at * 2] = fx(&pp->sc, sk_ut_pt[( m->off + at ) * 2]);
				g_xy[at * 2 + 1] = fy(&pp->sc, sk_ut_pt[( m->off + at ) * 2 + 1]);
			}
			sk_fill(&pp->s, g_xy, g_cn, nc, m->rgb, TRUE, &pp->all);
		}
	}
}

/* A key of the μTRON chart written by its place: its letter and kana as the TRON keyboard's are written */
static void draw_chart_words( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	SKONE	c;

	if ( !chart_by_place(k) ) return;
	c = *k;
	c.code = (UB)chart_place(k);
	c.name = NULL;
	draw_shape_words(v, &c, arg);
}

static void paint_shapes( SKVIEW *v )
{
	T_WMSURF	ws;
	SKPAINT		pp;
	INT		i, lw;

	if ( wm_obj_surf(v->kw, &ws) < E_OK || ws.addr == NULL ) return;
	pp.s.px = (UINT *)ws.addr;
	pp.s.w = ( ws.width < v->w ) ? ws.width : v->w;
	pp.s.h = ( ws.height < v->h ) ? ws.height : v->h;
	pp.s.pitch = ws.pitch / (INT)sizeof(UINT);
	pp.all.left = 0;
	pp.all.top = 0;
	pp.all.right = pp.s.w;
	pp.all.bottom = pp.s.h;
	scale_of(v, &pp.sc);
	if ( page()->kind == PK_SHAPE ) {
		sk_rect(&pp.s, &pp.all, C_GROUND);
		lw = 57;			/* the chart's line, in hundredths of a unit */
	} else {
		sk_rect(&pp.s, &pp.all, SK_UT_GROUND);
		for ( i = 0; i < 2; i++ ) {
			if ( ( v->part == PART_L && i == 1 ) || ( v->part == PART_R && i == 0 ) ) continue;
			g_cn[0] = plane_pts(&pp.sc, &sk_ut_pt[sk_ut_half[i].off * 2], sk_ut_half[i].n);
			sk_fill(&pp.s, g_xy, g_cn, 1, C_PLATE, TRUE, &pp.all);
		}
		lw = 50;
	}
	/* the line as thick as the chart's, never thinner than a pixel */
	pp.line = (INT)( (D)lw * 10 * pp.sc.num * SK_FIX / pp.sc.den / 100 );
	if ( pp.line < SK_FIX ) pp.line = SK_FIX;
	sk_each(v, paint_shape_key, &pp);
	(void)wm_obj_put(v->kw, &pp.all, 1);
}

static void paint_view( SKVIEW *v )
{
	T_DPRECT	all;

	if ( !v->open || v->gid < 0 ) return;
	if ( page()->kind != PK_ROWS ) {
		INT	kh;

		paint_shapes(v);
		kh = key_px(v);
		if ( page()->kind == PK_SHAPE ) {
			sk_each(v, draw_shape_words, &kh);
			(void)wm_obj_flush(v->kw, NULL);
		} else if ( !logical_dvorak() ) {
			sk_each(v, draw_chart_words, &kh);
			(void)wm_obj_flush(v->kw, NULL);
		}
		return;
	}
	all.left = 0;
	all.top = 0;
	all.right = v->w;
	all.bottom = v->h;
	(void)dp_fill_rect(v->gid, &all, C_GROUND);
	sk_each(v, draw_row_key, NULL);
	(void)wm_obj_flush(v->kw, NULL);
}

static void paint( void )
{
	INT	i;

	for ( i = 0; i < g_nv; i++ ) paint_view(&g_v[i]);
	g_dirty = FALSE;
}

/* ---------------------------------------------------------------- what is pressed */

typedef struct {
	INT		x, y;
	BOOL		got, inside;
	INT		best;			/* the nearest middle so far, squared */
	SKONE		k;
} SKHIT;

static BOOL in_outline( CONST SKONE *k, INT x, INT y )
{
	INT	i, j;
	BOOL	in = FALSE;

	for ( i = 0, j = k->n - 1; i < k->n; j = i++ ) {
		if ( ( k->y[i] > y ) != ( k->y[j] > y )
		  && x < k->x[i] + ( y - k->y[i] ) * ( k->x[j] - k->x[i] ) / ( k->y[j] - k->y[i] ) ) {
			in = (BOOL)!in;
		}
	}
	return in;
}

static void hit_key( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	SKHIT	*h = (SKHIT *)arg;
	INT	dx = h->x - k->cx, dy = h->y - k->cy, d = dx * dx + dy * dy;

	(void)v;
	if ( h->inside ) return;
	/* the gap between keys in rows belongs to the key on its left and above */
	if ( page()->kind == PK_ROWS ) {
		if ( h->x >= k->box.left - 1 && h->x <= k->box.right + 1
		  && h->y >= k->box.top - 1 && h->y <= k->box.bottom + 1 ) {
			h->k = *k;
			h->got = TRUE;
		}
		return;
	}
	if ( in_outline(k, h->x, h->y) ) {
		h->k = *k;
		h->got = h->inside = TRUE;
		return;
	}
	/* between keys by shape: the nearest, when it is near */
	if ( d < h->best ) {
		h->best = d;
		h->k = *k;
		h->got = TRUE;
	}
}

static BOOL key_at( CONST SKVIEW *v, INT x, INT y, SKONE *k )
{
	static SKHIT	h;
	INT		r = key_px(v) * 6 / 10;

	h.x = x;
	h.y = y;
	h.got = h.inside = FALSE;
	h.best = r * r;
	sk_each(v, hit_key, &h);
	if ( h.got ) *k = h.k;
	return h.got;
}

/* A modifier pressed on the picture: off, for the next key, until pressed again, off */
static void modifier( UINT code )
{
	UINT	bit = 1U << ( code - 0xE0 );

	if ( ( g_lock & bit ) != 0 ) {
		g_lock &= ~bit;
	} else if ( ( g_once & bit ) != 0 ) {
		g_once &= ~bit;
		g_lock |= bit;
	} else {
		g_once |= bit;
	}
	say("mods once %x lock %x", g_once, g_lock);
}

static void put( CONST SKVIEW *v, UINT code, UINT mods )
{
	ER	er = wm_key_put(v->kw, code, mods);

	if ( er < E_OK ) say("not typed %02x (%d)", code, (INT)er);
	else say("typed %02x mods %x", code, mods);
	g_flash = code;
	g_flash_end = ui_now() + FLASH_MS;
}

/*
 * The usage that types a letter in the latin layout set, and whether
 * with Shift: FALSE when no key types it
 */
static BOOL latin_of( char ch, UINT *p_code, BOOL *p_shift )
{
	char	b[8];
	UINT	c;
	INT	s;

	for ( s = 0; s < 2; s++ ) {
		for ( c = 0x04; c <= 0x89; c++ ) {
			if ( c > 0x38 && c != 0x87 && c != 0x89 ) continue;
			if ( km_label(KM_L_CHAR, c, s ? HID_MOD_LSHIFT : 0, b, sizeof(b)) == 1 && b[0] == ch ) {
				*p_code = c;
				*p_shift = (BOOL)( s != 0 );
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* The letter a key is written with (its plain face), 0 none */
static char shown_letter( CONST SKONE *k )
{
	if ( k->name != NULL || k->act != SK_KEY ) return 0;
	if ( page()->kind == PK_CHART ) return chart_by_place(k) ? key_letter(chart_place(k)) : k->pl;
	return key_letter(k->code);
}

/*
 * What a key of the μTRON chart types. In 日本語 with kana typed, a key
 * with TRON かな on its faces types the usage of its kana, the Shift held
 * choosing the face. A key written by its place types its place. Else
 * the letter of the face the Shift held chooses -- the plain one, the red
 * with the left Shift, the blue with the right, a capital with either --
 * is typed as whatever key types it in the latin layout set; a key with
 * no letter there types its usage.
 */
static BOOL chart_send( CONST SKONE *k, UINT mods, UINT *p_code, UINT *p_mods )
{
	char	b[16], ch = k->pl;
	UINT	code;
	BOOL	shift;

	*p_mods = mods;
	if ( mode_jp() && ( g_mode & WM_MODE_ROMAN ) == 0 && k->kan != 0
	  && km_label(KM_L_KANA, k->kan, 0, b, sizeof(b)) > 0 ) {
		*p_code = k->kan;
		return TRUE;
	}
	if ( chart_by_place(k) ) {
		*p_code = chart_place(k);
		return TRUE;
	}
	if ( ( mods & HID_MOD_LSHIFT ) != 0 ) {
		ch = ( k->red != 0 ) ? k->red : ( k->pl >= 'a' && k->pl <= 'z' ) ? (char)( k->pl - 'a' + 'A' ) : 0;
	} else if ( ( mods & HID_MOD_RSHIFT ) != 0 ) {
		ch = ( k->blue != 0 ) ? k->blue : ( k->pl >= 'a' && k->pl <= 'z' ) ? (char)( k->pl - 'a' + 'A' ) : 0;
	}
	if ( ch != 0 && latin_of(ch, &code, &shift) ) {
		*p_code = code;
		*p_mods = ( mods & ~M_SHIFT ) | ( shift ? HID_MOD_LSHIFT : 0 );
		return TRUE;
	}
	*p_code = k->code;
	return (BOOL)( k->code != 0 );
}

/*
 * What a key that types types, with the modifiers held, as the keyboard
 * sends it: the usage of its place with the modifiers, Ctrl and Alt too
 * (the programs take a command by the letter the key types, wm_key_cmd);
 * with Fn its Fn usage; a key of the μTRON chart as chart_send says
 */
static BOOL send_of( CONST SKONE *k, UINT mods, UINT *p_code, UINT *p_mods )
{
	*p_mods = mods;
	if ( g_fn != 0 && k->fn != 0 ) {
		*p_code = k->fn;
		return TRUE;
	}
	if ( page()->kind == PK_CHART && k->name == NULL ) return chart_send(k, mods, p_code, p_mods);
	*p_code = k->code;
	return (BOOL)( k->code != 0 );
}

static void press( CONST SKVIEW *v, INT x, INT y )
{
	SKONE	k;
	UINT	code, mods;

	if ( !key_at(v, x, y, &k) ) return;
	switch ( k.act ) {
	case SK_MOD:
		modifier(k.code);
		break;
	case SK_FN:
		g_fn = ( g_fn + 1 ) % 3;
		say("fn %d", g_fn);
		break;
	case SK_IME:
		/*
		 * The reader of the input sets the mode as the key passes it
		 * (knl_msg_mode_key): ひらがなカタカナ to 日本語, 英数 to 英語.
		 * Typed without the modifiers held, which would make them
		 * katakana, capitals or the other input.
		 */
		mode_read();
		put(v, mode_jp() ? 0x39 : 0x88, 0);
		say("ime %s", mode_jp() ? "off" : "on");
		break;
	default:
		if ( send_of(&k, g_once | g_lock, &code, &mods) ) put(v, code, mods);
		else say("nothing to type");
		g_once = 0;
		if ( g_fn == 1 ) g_fn = 0;
		break;
	}
	g_dirty = TRUE;
}

/* A key of the keyboard, as it came: the keys held lit, the input mode shown as it stands */
static void seen( CONST T_OBNTM *m )
{
	UINT	c = m->code;

	if ( m->lost != 0 ) memset(g_down, 0, sizeof(g_down));
	if ( c < 256 ) {
		UB	bit = (UB)( 1U << ( c & 7 ) );

		if ( ( m->mods & OB_KEY_UP ) != 0 ) g_down[c >> 3] &= (UB)~bit;
		else g_down[c >> 3] |= bit;
	}
	g_mods = m->mods & ~( OB_KEY_UP | OB_KEY_SEEN );
	/*
	 * A key is seen before the reader of the input works it as a mode
	 * key, so its press carries the mode as it was; its let go, the
	 * mode it left.
	 */
	if ( m->imode != g_mode ) {
		g_mode = m->imode;
		say("mode %s", mode_jp() ? "jp" : "en");
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the layout kept */

/*
 * The layout chosen, as the line SOFTKB_LAYOUT of the user information
 * object (record 1, lines of settings): read at the start, written when
 * another is chosen. The other lines are kept as they are. While a TRON
 * keyboard is chosen, two lines beside it say how the machine's layouts
 * were switched for it: SOFTKB_WAS the latin and the kana layouts there
 * were before, SOFTKB_SET those the switch set ("1 1", Dvorak and TRON
 * かな配列); both empty when nothing was switched.
 */
static UB	g_conf[CF_TEXT_MAX];
static INT	g_conf_len;

static INT conf_read( void )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	g_conf_len = 0;
	if ( ob_fnd_sys(SYSROLE_CONF_USER, SYSDEF_CONF_USER, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) return (INT)key;
	er = ob_rea_rec(key, 1, 0, g_conf, sizeof(g_conf), &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) return (INT)er;
	g_conf_len = (INT)( ( asz > (SZ)sizeof(g_conf) ) ? (SZ)sizeof(g_conf) : asz );
	return g_conf_len;
}

/* A line of what conf_read read put to a value, in the text only */
static BOOL conf_put( CONST char *key, CONST char *val )
{
	INT	len = cf_put(g_conf, g_conf_len, (INT)sizeof(g_conf), key, 0, val);

	if ( len < 0 ) return FALSE;
	g_conf_len = len;
	return TRUE;
}

/* The text written back, replaced in one change: a cut in the power leaves the old record or the new one */
static ER conf_write( void )
{
	TS_UUID	u;
	ID	key;
	ER	er;

	if ( ob_fnd_sys(SYSROLE_CONF_USER, SYSDEF_CONF_USER, &u) < E_OK ) return E_SYS;
	key = ui_conf_open(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	er = ob_rpl_rec(key, 1, g_conf, (SZ)g_conf_len);
	ob_cls_obj(key);
	return er;
}

/* A pair of layouts of a line ("latin kana"): FALSE when the line is not there or empty */
static BOOL conf_pair( CONST char *key, INT *p_latin, INT *p_kana )
{
	char	v[32], w[16];

	if ( !cf_get(g_conf, g_conf_len, key, 0, v, sizeof(v)) || v[0] == 0 ) return FALSE;
	if ( !cf_word(v, 0, w, sizeof(w)) ) return FALSE;
	*p_latin = cf_num(w, -1);
	if ( !cf_word(v, 1, w, sizeof(w)) ) return FALSE;
	*p_kana = cf_num(w, -1);
	return (BOOL)( *p_latin >= 0 && *p_latin < KM_LATIN_N && *p_kana >= 0 && *p_kana < KM_KANA_N );
}

static INT page_kept( void )
{
	char	v[32];
	INT	len = conf_read(), i;

	if ( len < 0 || !cf_get(g_conf, len, CONF_KEY, 0, v, sizeof(v)) ) return 0;
	for ( i = 0; i < SK_NPAGE; i++ ) {
		if ( strcmp(v, sk_pages[i].key) == 0 ) return i;
	}
	return 0;
}

/*
 * The switch's line SOFTKB_SET read: its latin layout -1 when the person
 * has since chosen the logical layout, which is then the person's
 */
static BOOL conf_set_pair( INT *p_latin, INT *p_kana )
{
	char	v[32], w[16];

	if ( !cf_get(g_conf, g_conf_len, "SOFTKB_SET", 0, v, sizeof(v)) || v[0] == 0 ) return FALSE;
	if ( !cf_word(v, 0, w, sizeof(w)) ) return FALSE;
	*p_latin = cf_num(w, -1);
	if ( !cf_word(v, 1, w, sizeof(w)) ) return FALSE;
	*p_kana = cf_num(w, -1);
	return TRUE;
}

/*
 * The latin layout a TRON keyboard is typed in: Dvorak, its letters'
 * own, at first; the logical layout the person last chose on a TRON
 * keyboard after that (SOFTKB_TRONLATIN)
 */
static INT tron_latin( void )
{
	char	v[16];
	INT	n;

	if ( !cf_get(g_conf, g_conf_len, "SOFTKB_TRONLATIN", 0, v, sizeof(v)) ) return KM_LATIN_DVORAK;
	n = cf_num(v, KM_LATIN_DVORAK);
	return ( n >= 0 && n < KM_LATIN_N ) ? n : KM_LATIN_DVORAK;
}

/*
 * The logical layout chosen by the person: the machine's latin layout
 * set (km_set), as キー配列 sets it. It is the person's: the English
 * letters chosen again later do not put back the one there was before a
 * TRON keyboard (SOFTKB_SET's latin made -1), and chosen on a TRON
 * keyboard, it is the one a TRON keyboard is typed in from then on.
 */
static void logical_set( INT latin )
{
	T_KMSTATE	s;
	INT		sl, sk;
	char		v[16];
	ER		er;

	if ( km_get(&s) < E_OK ) return;
	s.latin = latin;
	er = km_set(&s, KM_SET_LATIN);
	if ( er < E_OK ) {
		say("latin not set (%d)", (INT)er);
		return;
	}
	if ( conf_read() >= 0 ) {
		if ( conf_set_pair(&sl, &sk) ) {
			snprintf(v, sizeof(v), "%d %d", -1, sk);
			(void)conf_put("SOFTKB_SET", v);
		}
		if ( page()->tron ) {
			snprintf(v, sizeof(v), "%d", latin);
			(void)conf_put("SOFTKB_TRONLATIN", v);
		}
		(void)conf_write();
	}
	(void)km_get(&g_st);
	say("latin %d", latin);
	g_dirty = TRUE;
}

/*
 * A TRON keyboard chosen: the machine's layouts switched to Dvorak and
 * TRON かな配列, as キー配列 sets them (km_set), so that its keys type
 * what they are written with, the keyboard's own keys too. What there
 * was is kept, to be put back -- but not again when it is kept already:
 * a TRON keyboard chosen a second time before the English letters keeps
 * what there was the first time.
 */
static void tron_on( void )
{
	T_KMSTATE	s, ns;
	INT		wl, wk;
	char		v[16];
	ER		er;

	if ( km_get(&s) < E_OK ) return;
	ns = s;
	ns.latin = tron_latin();
	ns.kana = KM_KANA_TRON;
	er = km_set(&ns, KM_SET_LATIN | KM_SET_KANA);
	if ( er < E_OK ) {
		say("layouts not set (%d)", (INT)er);	/* not the person's to set: the picture only */
		return;
	}
	if ( !conf_pair("SOFTKB_WAS", &wl, &wk) ) {
		wl = s.latin;
		wk = s.kana;
		snprintf(v, sizeof(v), "%d %d", wl, wk);
		(void)conf_put("SOFTKB_WAS", v);
	}
	snprintf(v, sizeof(v), "%d %d", ns.latin, KM_KANA_TRON);
	(void)conf_put("SOFTKB_SET", v);
	say("layouts set latin %d kana %d, were %d %d", ns.latin, KM_KANA_TRON, wl, wk);
}

/*
 * The English letters chosen again: each of the two layouts put back as
 * it was before the TRON keyboard, if it is still what the switch set.
 * One changed since (by キー配列, システム環境設定) is the person's
 * choice, and stays.
 */
static void tron_off( void )
{
	T_KMSTATE	s, ns;
	INT		wl, wk, sl, sk;
	UINT		what = 0;
	ER		er;

	if ( !conf_pair("SOFTKB_WAS", &wl, &wk) ) return;
	if ( !conf_set_pair(&sl, &sk) ) {
		sl = KM_LATIN_DVORAK;
		sk = KM_KANA_TRON;
	}
	(void)conf_put("SOFTKB_WAS", "");
	(void)conf_put("SOFTKB_SET", "");
	if ( km_get(&s) < E_OK ) return;
	ns = s;
	if ( s.latin == sl ) {
		ns.latin = wl;
		what |= KM_SET_LATIN;
	}
	if ( s.kana == sk ) {
		ns.kana = wk;
		what |= KM_SET_KANA;
	}
	er = ( what != 0 ) ? km_set(&ns, what) : E_OK;
	if ( er < E_OK ) say("layouts not put back (%d)", (INT)er);
	else say("layouts back latin %d kana %d (%x put back)", ns.latin, ns.kana, what);
}

/* ---------------------------------------------------------------- the windows */

#define MSG_ROOM	24		/* the message line at the foot of the screen, kept clear */

static BOOL place_get( CONST SKVIEW *v, T_OBWPOS *wp )
{
	SZ	asz = 0;

	return (BOOL)( v->kw > 0 && ob_rea_rec(v->kw, OB_WR_PLACE, 0, wp, sizeof(*wp), &asz) >= E_OK
		       && asz >= (SZ)sizeof(*wp) );
}

static void place_set( CONST SKVIEW *v, T_OBWPOS *wp )
{
	SZ	asz = 0;

	(void)ob_wri_rec(v->kw, OB_WR_PLACE, 0, wp, sizeof(*wp), &asz);
}

/* Where the window stands now, noted: a later place not this one was the person's moving it */
static void place_note( SKVIEW *v )
{
	T_OBWPOS	wp;

	if ( !place_get(v, &wp) ) return;
	v->lastx = wp.left;
	v->lasty = wp.top;
}

/* Moved by the person since it was last noted: no longer held to the screen's edge */
static void place_moved( SKVIEW *v )
{
	T_OBWPOS	wp;

	if ( !place_get(v, &wp) ) return;
	if ( wp.left != v->lastx || wp.top != v->lasty ) {
		v->edge = FALSE;
		v->lastx = wp.left;
		v->lasty = wp.top;
	}
}

static void place_shift( CONST SKVIEW *v, T_OBWPOS *wp, INT dx, INT dy )
{
	wp->left += dx;
	wp->right += dx;
	wp->wleft += dx;
	wp->wright += dx;
	wp->top += dy;
	wp->bottom += dy;
	wp->wtop += dy;
	wp->wbottom += dy;
	place_set(v, wp);
}

/*
 * A half held to its edge of the screen put there again (the right half
 * grows to the left), and any window kept on the screen: moved in where
 * it stands out, the message line kept clear
 */
static void place_fit( SKVIEW *v )
{
	T_OBWPOS	wp;
	INT		sw = 0, sh = 0, dx = 0, dy = 0;

	if ( !place_get(v, &wp) ) return;
	wm_screen(&sw, &sh);
	if ( v->edge && v->part == PART_R ) dx = sw - wp.right;
	else if ( v->edge && v->part == PART_L ) dx = -wp.left;
	else if ( wp.right > sw ) dx = sw - wp.right;
	if ( wp.left + dx < 0 ) dx = -wp.left;
	if ( wp.bottom > sh - MSG_ROOM ) dy = sh - MSG_ROOM - wp.bottom;
	if ( wp.top + dy < 0 ) dy = -wp.top;
	if ( dx != 0 || dy != 0 ) place_shift(v, &wp, dx, dy);
	place_note(v);
}

static void work_size( SKVIEW *v )
{
	T_OBWPOS	wp;

	if ( place_get(v, &wp) ) {
		v->w = wp.wright - wp.wleft;
		v->h = wp.wbottom - wp.wtop;
	}
}

static INT w_min( CONST SKVIEW *v )
{
	return ( v->part == PART_ALL ) ? SK_WMIN : SK_WMIN_HALF;
}

static INT h_min( CONST SKVIEW *v )
{
	return ( v->part == PART_ALL ) ? SK_HMIN : SK_HMIN_HALF;
}

/* The work area sized to (w, h), the frame round it as it is, never larger than the screen */
static void work_set( SKVIEW *v, INT w, INT h )
{
	T_OBWPOS	wp;
	INT		sw = 0, sh = 0, fw, fh;

	if ( place_get(v, &wp) ) {
		wm_screen(&sw, &sh);
		fw = ( wp.right - wp.left ) - ( wp.wright - wp.wleft );
		fh = ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop );
		if ( sw > 0 && w > sw - fw ) w = sw - fw;
		if ( sh > 0 && h > sh - MSG_ROOM - fh ) h = sh - MSG_ROOM - fh;
		if ( w < w_min(v) ) w = w_min(v);
		if ( h < h_min(v) ) h = h_min(v);
		wp.right = wp.left + w + fw;
		wp.bottom = wp.top + h + fh;
		place_set(v, &wp);
	}
	work_size(v);
}

/*
 * A window sized again by the person: the keys to its size, never
 * smaller than they can be pressed. The halves are one keyboard: the
 * other half is given the same size, each staying at its edge of the
 * screen unless the person has moved it away. The other's own notice of
 * its new size finds the two alike, and goes no further.
 */
static void resized( SKVIEW *v )
{
	INT	w = v->w, h = v->h;

	work_size(v);
	if ( v->w < w_min(v) || v->h < h_min(v) ) work_set(v, v->w, v->h);
	if ( v->w != w || v->h != h ) say("size %d %d", v->w, v->h);
	if ( g_nv == 2 ) {
		SKVIEW	*o = ( v == &g_v[0] ) ? &g_v[1] : &g_v[0];

		place_moved(v);
		place_fit(v);
		if ( o->open && ( o->w != v->w || o->h != v->h ) ) {
			place_moved(o);
			work_set(o, v->w, v->h);
			place_fit(o);
			say("other half size %d %d", o->w, o->h);
		}
	}
	g_dirty = TRUE;
}

/*
 * Where a page's window was last, as a line of the user information
 * object: SOFTKB_WIN_<page>[L|R] with the frame's corner, the work area's
 * size and whether it was held to its edge of the screen
 */
static void win_key( INT pg, INT part, char *buf, INT max )
{
	snprintf(buf, (size_t)max, "SOFTKB_WIN_%s%s", sk_pages[pg].key,
		 ( part == PART_L ) ? "L" : ( part == PART_R ) ? "R" : "");
}

static BOOL win_kept( INT pg, INT part, INT *p )
{
	char	key[40], v[64], w[16];
	INT	i;

	win_key(pg, part, key, sizeof(key));
	if ( !cf_get(g_conf, g_conf_len, key, 0, v, sizeof(v)) || v[0] == 0 ) return FALSE;
	for ( i = 0; i < 5; i++ ) {
		if ( !cf_word(v, i, w, sizeof(w)) ) return FALSE;
		p[i] = cf_num(w, -1);
	}
	return (BOOL)( p[2] > 0 && p[3] > 0 );
}

/* The windows' places put in the lines read (conf_read), to be written with them */
static void wins_put( void )
{
	T_OBWPOS	wp;
	char		key[40], v[64];
	INT		i;

	for ( i = 0; i < g_nv; i++ ) {
		SKVIEW	*s = &g_v[i];

		if ( !s->open || !place_get(s, &wp) ) continue;
		place_moved(s);
		win_key(g_page, s->part, key, sizeof(key));
		snprintf(v, sizeof(v), "%d %d %d %d %d", wp.left, wp.top, wp.wright - wp.wleft,
			 wp.wbottom - wp.wtop, s->edge ? 1 : 0);
		(void)conf_put(key, v);
		say("window %s %s", key, v);
	}
}

/*
 * A window, in front of the windows worked in and never having the keys,
 * its size the person's to change, and asking to be told every key of
 * the keyboard: where the page's window was last (the lines read by
 * conf_read), or the whole keyboard at the foot of the screen in the
 * middle, a half at the screen's left or right edge; on the screen in
 * any case
 */
static ER win_open( SKVIEW *v, INT part, BOOL plain )
{
	T_OBCRE		c;
	T_OBNTF		req;
	INT		sw = 0, sh = 0, x, y, w0 = page()->w0, h0 = page()->h0, kept[5];
	char		json[200];

	memset(v, 0, sizeof(*v));
	v->part = part;
	v->gid = -1;
	v->edge = (BOOL)( part != PART_ALL );
	wm_screen(&sw, &sh);
	if ( part == PART_L ) {
		x = 0;
	} else if ( part == PART_R ) {
		x = ( sw > w0 + 16 ) ? sw - w0 - 16 : 0;
	} else {
		x = ( sw > w0 + 16 ) ? ( sw - w0 - 16 ) / 2 : 0;
	}
	y = ( sh > h0 + 120 ) ? sh - h0 - 120 : 0;
	if ( !plain && win_kept(g_page, part, kept) ) {
		x = kept[0];
		y = kept[1];
		w0 = kept[2];
		h0 = kept[3];
		v->edge = (BOOL)( part != PART_ALL && kept[4] != 0 );
	}
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%u,\"icon\":\"%s\"}",
		 x, y, x + w0 + 16, y + h0 + 34,
		 (UINT)( WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			 | ( plain ? 0 : ( WM_ATTR_NOFOCUS | WM_ATTR_FLOAT ) ) ),
		 SYSDEF_PROG_SOFTKB);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)( ( part == PART_L ) ? "ソフトウェアキーボード(左手)"
			     : ( part == PART_R ) ? "ソフトウェアキーボード(右手)" : "ソフトウェアキーボード" );
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &v->win) < E_OK ) return E_OBJ;
	v->kw = ob_opn_obj(&v->win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	v->gid = ( v->kw > 0 ) ? wm_obj_gid(v->kw) : E_ID;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE;
	req.id = 7;
	req.flags = OB_N_KEYSEEN;
	if ( v->kw <= 0 || v->gid < 0 || ob_ntf_evt(v->kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( v->kw > 0 ) ob_cls_obj(v->kw);
		(void)ob_del_obj(&v->win);
		v->kw = 0;
		return E_OBJ;
	}
	v->open = TRUE;
	work_set(v, w0, h0);			/* the frame as it came out round the work area, on the screen */
	place_fit(v);
	return E_OK;
}

static void win_close( SKVIEW *v )
{
	if ( !v->open ) return;
	if ( v->kw > 0 ) ob_cls_obj(v->kw);
	v->kw = 0;
	(void)ob_del_obj(&v->win);
	v->open = FALSE;
}

/* The windows of the page: one, or a half at each edge of the screen; the halves of one size */
static ER views_open( void )
{
	g_nv = 0;
	if ( page()->split ) {
		if ( win_open(&g_v[0], PART_L, FALSE) < E_OK ) return E_OBJ;
		g_nv = 1;
		if ( win_open(&g_v[1], PART_R, FALSE) < E_OK ) return E_OBJ;
		g_nv = 2;
		if ( g_v[1].w != g_v[0].w || g_v[1].h != g_v[0].h ) {
			work_set(&g_v[1], g_v[0].w, g_v[0].h);
			place_fit(&g_v[1]);
		}
		return E_OK;
	}
	if ( win_open(&g_v[0], PART_ALL, FALSE) < E_OK ) return E_OBJ;
	g_nv = 1;
	return E_OK;
}

static void views_close( void )
{
	INT	i;

	for ( i = 0; i < g_nv; i++ ) win_close(&g_v[i]);
	g_nv = 0;
}

/*
 * Another layout: where the windows of this one stand kept, the windows
 * of the other opened where they were last, the machine's layouts
 * switched for a TRON keyboard or put back from it, and the layout kept.
 * Closing the window puts nothing back: it opens again as it was left,
 * the layouts with it.
 */
static void page_set( INT pg )
{
	CONST SKPAGE	*p = &sk_pages[pg];
	ER		er;

	if ( pg == g_page ) return;
	(void)conf_read();
	wins_put();
	if ( p->tron && !page()->tron ) tron_on();
	else if ( !p->tron && page()->tron ) tron_off();
	g_page = pg;
	g_fn = 0;
	if ( g_nv > 0 ) {
		views_close();
		if ( views_open() < E_OK ) {
			say("no window");
			g_closing = TRUE;
		}
	}
	say("page %s size %d %d", p->key, g_v[0].w, g_v[0].h);
	er = conf_put(CONF_KEY, p->key) ? conf_write() : E_LIMIT;
	if ( er < E_OK ) say("not kept (%d)", (INT)er);
	else say("kept %s", p->key);
	(void)km_get(&g_st);
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the menu */

static void command( CONST char *code )
{
	INT	i;

	if ( strcmp(code, "close") == 0 ) {
		g_closing = TRUE;
		return;
	}
	for ( i = 0; i < SK_NPAGE; i++ ) {
		if ( strcmp(code, sk_pages[i].code) == 0 ) page_set(i);
	}
	if ( strcmp(code, "latin.qwerty") == 0 ) logical_set(KM_LATIN_KEYS);
	else if ( strcmp(code, "latin.dvorak") == 0 ) logical_set(KM_LATIN_DVORAK);
	g_dirty = TRUE;
}

/* The menu, in whichever window it was asked for: the same for both halves */
static void menu( CONST SKVIEW *v, CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	INT	i;
	BOOL	chosen;

	if ( ui_uuid(SYSDEF_MENU_SOFTKB, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	for ( i = 0; i < SK_NPAGE; i++ ) {
		(void)mn_chg_atr(mid, NULL, sk_pages[i].code, ( i == g_page ) ? MN_TICK : 0);
	}
	(void)km_get(&g_st);
	(void)mn_chg_atr(mid, NULL, "latin.qwerty", logical_dvorak() ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, NULL, "latin.dvorak", logical_dvorak() ? MN_TICK : 0);
	say("menu");
	chosen = (BOOL)( mn_pop_men(mid, v->kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) {
		say("chosen %s", (CONST char *)sel.code);
		command((CONST char *)sel.code);
	}
}

static void event( CONST T_OBNTM *m )
{
	SKVIEW	*v = NULL;
	INT	i;

	for ( i = 0; i < g_nv; i++ ) {
		if ( g_v[i].open && same_uuid(&m->uuid, &g_v[i].win) ) v = &g_v[i];
	}
	if ( v == NULL ) return;
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		g_closing = TRUE;		/* a half closed closes the other with it */
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) {
			menu(v, m);
		} else if ( m->code == 0 && m->x >= 0 && m->y >= 0 && m->x < v->w && m->y < v->h ) {
			press(v, m->x, m->y);
		}
		break;
	case OB_E_KEY:
		/* both halves are told of every key: one is enough */
		if ( ( m->mods & OB_KEY_SEEN ) != 0 && v == &g_v[0] ) seen(m);
		break;
	case OB_E_REDRAW:
		resized(v);
		break;
	default:
		break;
	}
}

static ER port_open( void )
{
	T_OBCRE	c;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_ch) < E_OK ) return E_NOMEM;
	g_port = ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( g_port <= 0 ) {
		(void)ob_del_obj(&g_ch);
		return E_OBJ;
	}
	return E_OK;
}

static void port_close( void )
{
	if ( g_port > 0 ) ob_cls_obj(g_port);
	g_port = 0;
	(void)ob_del_obj(&g_ch);
}

/* ---------------------------------------------------------------- the selftest */

static INT st_fail;

static void st_check( BOOL ok, CONST char *what )
{
	if ( ok ) return;
	st_fail++;
	say("selftest: failed: %s", what);
}

/* A table in rows as it must be: five rows of SK_ROWQ quarters, each key with something to be written with */
static BOOL st_table( CONST SKKEY *t, BOOL letters )
{
	INT	q = 0, row = 0;
	char	b[32];

	for ( ; row < SK_NROW; t++ ) {
		if ( t->act == SK_ROW ) {
			if ( q != SK_ROWQ ) return FALSE;
			q = 0;
			row++;
			continue;
		}
		if ( t->w == 0 || t->code == 0 ) return FALSE;
		if ( t->act == SK_MOD && ( t->code < 0xE0 || t->code > 0xE7 ) ) return FALSE;
		if ( letters && t->act == SK_KEY && t->name == NULL
		  && km_label(KM_L_CHAR, t->code, 0, b, sizeof(b)) <= 0 ) return FALSE;
		q += t->w;
	}
	return TRUE;
}

/* Each key found at its middle, in the window it is in */
static INT	st_hit_i;
static BOOL	st_hit_ok;

static void st_hit( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	SKONE	got;

	(void)arg;
	if ( !key_at(v, k->cx, k->cy, &got) || got.idx != k->idx ) {
		st_hit_ok = FALSE;
		say("selftest: key %d not found at its middle", k->idx);
	}
	st_hit_i++;
}

static INT st_hits( SKVIEW *v, INT part, INT w, INT h )
{
	v->part = part;
	v->w = w;
	v->h = h;
	st_hit_i = 0;
	sk_each(v, st_hit, NULL);
	return st_hit_i;
}

/*
 * The TRON keyboard's table as it must be: 72 keys on the plane, each
 * typing something (Fn alone typing nothing), each found where it is,
 * its outline as drawn round the same place as the one pressed
 */
static void st_tron( void )
{
	SKVIEW	v;
	INT	i, j, fns = 0, was_pg = g_page;
	BOOL	ok = TRUE, letters = TRUE, lines = TRUE;
	char	b[32];

	memset(&v, 0, sizeof(v));
	st_check(sk_ntron == 72, "72 keys of the TRON keyboard");
	for ( i = 0; i < sk_ntron; i++ ) {
		CONST SKPLACE	*k = &sk_tron[i];
		INT		cx = 0, cy = 0, lx = 0, ly = 0;

		for ( j = 0; j < SK_NPT; j++ ) {
			if ( k->pt[j][0] < 0 || k->pt[j][0] > SK_TRON_W || k->pt[j][1] < 0 || k->pt[j][1] > SK_TRON_H ) {
				ok = FALSE;
			}
			cx += k->pt[j][0];
			cy += k->pt[j][1];
		}
		for ( j = 0; j < SK_TRON_NL; j++ ) {
			lx += sk_tron_line[i][j][0];
			ly += sk_tron_line[i][j][1];
		}
		cx /= SK_NPT;
		cy /= SK_NPT;
		lx /= SK_TRON_NL;
		ly /= SK_TRON_NL;
		if ( cx - lx > 10 || lx - cx > 10 || cy - ly > 10 || ly - cy > 10 ) lines = FALSE;
		if ( k->act == SK_FN ) fns++;
		else if ( k->code == 0 ) ok = FALSE;
		if ( k->act == SK_MOD && ( k->code < 0xE0 || k->code > 0xE7 ) ) ok = FALSE;
		/* its letters are the Japanese keyboard's usages: written when the machine's keyboard is one */
		if ( !g_st.us && k->act == SK_KEY && k->name == NULL
		  && km_label(KM_L_CHAR, k->code, 0, b, sizeof(b)) <= 0 ) letters = FALSE;
	}
	st_check(ok, "the TRON keyboard's keys on its plane, each typing something");
	st_check(fns == 1, "one Fn");
	st_check(letters, "every letter key of the TRON keyboard written with its letter");
	st_check(lines, "each outline drawn where its key is pressed");

	g_page = PG_TRON;
	st_hit_ok = TRUE;
	st_check(st_hits(&v, PART_ALL, 760, 310) == 72 && st_hit_ok, "each key of the TRON keyboard found at its middle");
	st_hit_ok = TRUE;
	(void)st_hits(&v, PART_ALL, 400, 300);		/* narrower than its shape: smaller, in the middle */
	st_check(st_hit_ok, "each key found in a narrow window");
	g_page = was_pg;
}

/*
 * The μTRON keyboard's table as it must be: 76 keys on the plane, 38 in
 * each half, the marks of each inside it, each
 * key found at its middle in its half's window and in one of the whole,
 * and the letters of its faces typed by some key of the latin layout set
 */
static void st_utron( void )
{
	SKVIEW	v;
	INT	i, j, nl = 0, was_pg = g_page;
	BOOL	ok = TRUE, marks = TRUE, letters = TRUE;
	UINT	code;
	BOOL	shift;

	memset(&v, 0, sizeof(v));
	st_check(sk_ut_nkey == 76, "76 keys of the μTRON keyboard");
	for ( i = 0; i < sk_ut_nkey; i++ ) {
		CONST SKVKEY	*k = &sk_ut_key[i];

		if ( ( k->flags & SKF_R ) == 0 ) nl++;
		if ( k->act == SK_MOD && ( k->code < 0xE0 || k->code > 0xE7 ) ) ok = FALSE;
		if ( k->n < 3 || k->n > SK_MAXPT ) ok = FALSE;
		if ( k->pl != 0 && !latin_of(k->pl, &code, &shift) ) letters = FALSE;
	}
	for ( i = 0; i < sk_ut_nmark; i++ ) {
		CONST SKVMARK	*m = &sk_ut_mark[i];
		CONST SKVKEY	*k = &sk_ut_key[m->key];
		INT		l = 0x7FFF, t = 0x7FFF, r = 0, b = 0, total = 0;

		for ( j = 0; j < k->n; j++ ) {
			INT	x = sk_ut_pt[( k->off + j ) * 2], y = sk_ut_pt[( k->off + j ) * 2 + 1];

			if ( x < l ) l = x;
			if ( x > r ) r = x;
			if ( y < t ) t = y;
			if ( y > b ) b = y;
		}
		for ( j = 0; j < m->ncn; j++ ) total += sk_ut_cn[m->cn + j];
		for ( j = 0; j < total; j++ ) {
			INT	x = sk_ut_pt[( m->off + j ) * 2], y = sk_ut_pt[( m->off + j ) * 2 + 1];

			if ( x < l - 20 || x > r + 20 || y < t - 20 || y > b + 20 ) marks = FALSE;
		}
	}
	st_check(ok && nl == 38, "the μTRON keyboard's keys, 38 of them in the left half");
	st_check(marks, "the marks of each key inside it");
	st_check(letters, "every letter of the μTRON keyboard typed by a key of the latin layout");

	g_page = PG_UTRON;
	st_hit_ok = TRUE;
	st_check(st_hits(&v, PART_L, 380, 290) == 38 && st_hit_ok, "each key of the left half found");
	st_hit_ok = TRUE;
	st_check(st_hits(&v, PART_R, 380, 290) == 38 && st_hit_ok, "each key of the right half found");
	st_hit_ok = TRUE;
	st_check(st_hits(&v, PART_ALL, 760, 300) == 76 && st_hit_ok, "each key found on the whole keyboard");
	g_page = was_pg;
}

/* The layout chosen is kept in the user information object, and read back; then as it was */
static void st_keep( void )
{
	INT	was = page_kept();

	g_page = ( was == 0 ) ? PG_TRON : 0;
	command(sk_pages[was].code);
	st_check(page_kept() == was && g_page == was, "the layout chosen kept");
	command(sk_pages[( was == 0 ) ? PG_TRON : 0].code);
	st_check(page_kept() == ( ( was == 0 ) ? PG_TRON : 0 ), "another layout chosen kept");
	command(sk_pages[was].code);
	st_check(page_kept() == was, "the layout put back");
}

/*
 * The machine's layouts switched with a TRON keyboard: set to Dvorak and
 * TRON かな配列, put back with the English letters, and a layout the
 * person changed in between left as the person set it; between the TRON
 * keyboard and the μTRON keyboard nothing switched; then all as it was
 */
static void st_switch( void )
{
	T_KMSTATE	was, base, s;
	INT		kept = page_kept();

	if ( km_get(&was) < E_OK ) {
		st_check(FALSE, "km_get");
		return;
	}
	base = was;
	base.latin = KM_LATIN_KEYS;
	base.kana = KM_KANA_JIS;
	st_check(km_set(&base, KM_SET_LATIN | KM_SET_KANA) == E_OK, "the layouts set to start from");
	g_page = 0;
	command("layout.tron");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_DVORAK && s.kana == KM_KANA_TRON,
		 "Dvorak and TRON かな配列 with the TRON keyboard");
	command("layout.qwerty");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_KEYS && s.kana == KM_KANA_JIS,
		 "the layouts put back with the English letters");
	command("layout.utron");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_DVORAK && s.kana == KM_KANA_TRON,
		 "Dvorak and TRON かな配列 with the μTRON keyboard");
	command("layout.tron");
	s.latin = KM_LATIN_DVORAK;
	s.kana = KM_KANA_NICOLA;
	st_check(km_set(&s, KM_SET_KANA) == E_OK, "the kana layout changed by the person");
	command("layout.utron");			/* from one TRON keyboard to the other: nothing done */
	command("layout.qwerty");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_KEYS && s.kana == KM_KANA_NICOLA,
		 "the latin layout put back, the person's kana layout left");
	st_check(km_set(&was, KM_SET_LATIN | KM_SET_KANA) == E_OK, "the layouts as they were");
	(void)conf_read();				/* the line as it was, nothing switched */
	(void)conf_put(CONF_KEY, sk_pages[kept].key);
	(void)conf_put("SOFTKB_WAS", "");
	(void)conf_put("SOFTKB_SET", "");
	(void)conf_write();
	g_page = kept;
}

/*
 * Every key that is written with a letter types that letter, on every
 * page, in QWERTY and in Dvorak; with Ctrl, the command of that letter
 */
static INT	st_bad;

static void st_same( CONST SKVIEW *v, CONST SKONE *k, void *arg )
{
	char	ch = shown_letter(k), b[8];
	UINT	code, mods;

	(void)v;
	(void)arg;
	if ( ch == 0 ) return;
	if ( !send_of(k, 0, &code, &mods) || km_label(KM_L_CHAR, code, mods & M_SHIFT, b, sizeof(b)) != 1 || b[0] != ch ) {
		st_bad++;
		say("selftest: page %d key %d written %c types %02x", g_page, k->idx, ch, code);
	}
	/* with Ctrl, the same key with Ctrl: the command of the letter it types (wm_key_cmd) */
	if ( ch >= 'a' && ch <= 'z' && ( !send_of(k, HID_MOD_LCTRL, &code, &mods) || ( mods & HID_MOD_LCTRL ) == 0
	  || wm_key_cmd(code) != 0x04 + (UINT)( ch - 'a' ) ) ) {
		st_bad++;
		say("selftest: page %d key %d written %c commands %02x", g_page, k->idx, ch, code);
	}
}

static void st_logical( void )
{
	T_KMSTATE	was, s;
	SKVIEW		v;
	INT		la, pg, was_pg = g_page, kept = page_kept();
	UINT		was_mode = g_mode;

	if ( km_get(&was) < E_OK ) return;
	command("latin.dvorak");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_DVORAK && g_st.latin == KM_LATIN_DVORAK,
		 "the logical layout Dvorak, the machine's latin layout");
	command("latin.qwerty");
	st_check(km_get(&s) >= E_OK && s.latin == KM_LATIN_KEYS && g_st.latin == KM_LATIN_KEYS,
		 "the logical layout QWERTY, the machine's latin layout");
	memset(&v, 0, sizeof(v));
	g_mode = WM_MODE_ALPH | WM_MODE_ROMAN;
	for ( la = 0; la < KM_LATIN_N; la++ ) {
		s = was;
		s.latin = la;
		(void)km_set(&s, KM_SET_LATIN);
		(void)km_get(&g_st);
		for ( pg = 0; pg < SK_NPAGE; pg++ ) {
			g_page = pg;
			st_bad = 0;
			v.part = PART_ALL;
			v.w = 760;
			v.h = 300;
			sk_each(&v, st_same, NULL);
			st_check(st_bad == 0, "every key typing the letter it is written with");
		}
	}
	g_mode = was_mode;
	g_page = was_pg;
	(void)km_set(&was, KM_SET_LATIN);
	(void)km_get(&g_st);
	(void)conf_read();				/* the lines as they were */
	(void)conf_put("SOFTKB_TRONLATIN", "");
	(void)conf_put(CONF_KEY, sk_pages[kept].key);
	(void)conf_write();
}

/* A shape filled with its edges smoothed: a pixel half covered, half coloured */
static void st_fill( void )
{
	static UINT	px[8 * 8];
	SKSURF		s;
	T_DPRECT	all;
	INT		xy[8], cn[1];

	s.px = px;
	s.w = s.h = s.pitch = 8;
	all.left = all.top = 0;
	all.right = all.bottom = 8;
	sk_rect(&s, &all, 0x00FFFFFFU);
	/* x 2 to 4.5, y 2 to 6: pixels 2 and 3 all black, pixel 4 half */
	xy[0] = 2 * SK_FIX;		xy[1] = 2 * SK_FIX;
	xy[2] = 4 * SK_FIX + SK_FIX / 2;	xy[3] = 2 * SK_FIX;
	xy[4] = 4 * SK_FIX + SK_FIX / 2;	xy[5] = 6 * SK_FIX;
	xy[6] = 2 * SK_FIX;		xy[7] = 6 * SK_FIX;
	cn[0] = 4;
	sk_fill(&s, xy, cn, 1, 0x00000000U, TRUE, &all);
	st_check(px[3 * 8 + 2] == 0 && px[3 * 8 + 3] == 0, "the pixels covered, coloured");
	st_check(( px[3 * 8 + 4] & 0xFF ) > 0x70 && ( px[3 * 8 + 4] & 0xFF ) < 0x90, "a pixel half covered, half coloured");
	st_check(px[3 * 8 + 5] == 0x00FFFFFFU && px[1 * 8 + 3] == 0x00FFFFFFU, "the pixels outside left");
}

static INT selftest( void )
{
	SKONE	ime;
	SKONE	one;
	char	b[32];
	INT	i;

	say("selftest");
	st_check(km_get(&g_st) >= E_OK, "km_get");
	for ( i = 0; i < SK_NPAGE; i++ ) {
		if ( sk_pages[i].kind != PK_ROWS ) continue;
		/* the letters only of the one the machine's keyboard types them on */
		st_check(st_table(sk_qwerty_jp, (BOOL)!g_st.us), "the Japanese keyboard's table");
		st_check(st_table(sk_qwerty_us, g_st.us), "the English keyboard's table");
	}
	st_tron();
	st_utron();
	st_fill();

	memset(&one, 0, sizeof(one));
	one.code = 0x1E;
	one.act = SK_KEY;
	one.fn = 0x3A;
	memset(&ime, 0, sizeof(ime));
	ime.code = 0x88;
	ime.act = SK_IME;
	g_mode = WM_MODE_ALPH;
	st_check(strcmp(key_label(&one, b, sizeof(b)), "1") == 0, "the key 1 written 1");
	g_once = HID_MOD_LSHIFT;
	st_check(strcmp(key_label(&one, b, sizeof(b)), "!") == 0, "the key 1 with Shift written !");
	g_once = 0;
	g_fn = 1;
	st_check(strcmp(key_label(&one, b, sizeof(b)), "F1") == 0, "the key 1 with Fn written F1");
	g_fn = 0;
	st_check(strcmp(key_label(&ime, b, sizeof(b)), "A") == 0, "the input mode key in 英語");
	g_mode = 0;
	st_check(strcmp(key_label(&ime, b, sizeof(b)), "あ") == 0, "the input mode key in 日本語");
	modifier(0xE1);
	st_check(g_once == HID_MOD_LSHIFT && g_lock == 0, "Shift once");
	modifier(0xE1);
	st_check(g_once == 0 && g_lock == HID_MOD_LSHIFT, "Shift twice");
	modifier(0xE1);
	st_check(g_once == 0 && g_lock == 0, "Shift a third time");
	st_keep();
	st_switch();
	st_logical();

	/* a window that may have the keys types nothing; this one, only after a press */
	g_page = 0;
	if ( port_open() >= E_OK ) {
		if ( win_open(&g_v[0], PART_ALL, TRUE) >= E_OK ) {
			st_check(wm_key_put(g_v[0].kw, 0x04, 0) == E_OBJ, "a window that may have the keys");
			win_close(&g_v[0]);
		} else {
			st_check(FALSE, "a plain window");
		}
		if ( win_open(&g_v[0], PART_ALL, FALSE) >= E_OK ) {
			st_check(g_v[0].w == sk_pages[0].w0 && g_v[0].h == sk_pages[0].h0, "the work area's size");
			st_check(wm_key_put(g_v[0].kw, 0x88, 0) == E_OACV, "a key before any press");
			win_close(&g_v[0]);
		} else {
			st_check(FALSE, "a window that never has the keys");
		}
		port_close();
	}
	say("selftest %s (%d failed)", ( st_fail == 0 ) ? "ok" : "failed", st_fail);
	return ( st_fail == 0 ) ? 0 : 1;
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	char	arg[16];
	UD	look_at, until;

	if ( km_get(&g_st) < E_OK ) memset(&g_st, 0, sizeof(g_st));
	memset(arg, 0, sizeof(arg));
	if ( ts_get_arg(arg, sizeof(arg) - 1) >= 8 && memcmp(arg, "selftest", 8) == 0 ) return selftest();

	if ( port_open() < E_OK ) {
		say("no channel");
		return 2;
	}
	g_page = page_kept();
	g_mode = kc_mode();
	if ( views_open() < E_OK ) {
		say("no window");
		views_close();
		port_close();
		return 3;
	}
	paint();
	say("ready %s %s size %d %d mode %s", page()->key, g_st.us ? "us" : "jp", g_v[0].w, g_v[0].h,
	    mode_jp() ? "jp" : "en");

	look_at = ui_now() + LOOK_MS;
	while ( !g_closing ) {
		until = ( g_flash != 0 && g_flash_end < look_at ) ? g_flash_end : look_at;
		for ( TMO tmo = ui_left(until);
		      !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) {
			event(&m);
		}
		if ( g_closing ) break;
		if ( g_flash != 0 && ui_now() >= g_flash_end ) {
			g_flash = 0;
			g_dirty = TRUE;
		}
		if ( ui_now() >= look_at ) {
			T_KMSTATE	s;

			look_at = ui_now() + LOOK_MS;
			mode_read();				/* changed by a key of the keyboard while no notice came */
			if ( km_get(&s) >= E_OK && memcmp(&s, &g_st, sizeof(s)) != 0 ) {
				if ( s.latin != g_st.latin ) say("latin %d", s.latin);
				g_st = s;			/* the keyboard or a layout changed: the letters and kana written anew */
				g_dirty = TRUE;
			}
		}
		if ( g_dirty ) paint();
	}
	/* where the windows stand kept, to open there again */
	if ( conf_read() >= 0 ) {
		wins_put();
		(void)conf_write();
	}
	views_close();
	port_close();
	say("closed");
	return 0;
}
