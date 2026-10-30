/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kp_main.c
 *	かな入力練習: typing kana, the key to press shown on a keyboard
 *	(design 11.14)
 *
 *	The program is started on a かな入力練習 object. Its record 0 holds
 *	the questions, one a paragraph; beside it, as resources named as a
 *	TADjs Desktop names the files it keeps them in, are the keyboards
 *	("_1_N.json"), the kana layouts ("_2_N.json") and the translations
 *	of a keyboard that changes its own layout ("_3_N.json"). The window
 *	shows the question with the readings over its words and what has
 *	been typed of the phrase in hand, the keyboard drawn from its layout
 *	with the kana on each key, and the scores along the foot.
 *
 *	Enter begins a practice of ten questions taken at random and Esc
 *	stops it. Each key pressed is judged by the kana layout chosen
 *	(kp_eng.c): the keys that come next are shown in reds that fade the
 *	further ahead they are, the shift to hold is framed, and a key
 *	flashes green when right and red when wrong. A chord is judged by
 *	the keys held, so the window asks to hear keys let go as well as
 *	pressed (OB_N_KEYUP). When the last question is done the scores are
 *	shown in a panel of the program's data box.
 *
 *	A layout's JSON carried onto the window (a link to an object that
 *	holds it, as ファイル変換 brings a file in) is added to the object
 *	as the next resource of its kind and chosen. A keyboard or a kana
 *	layout the object lacks is made from the system's (ts/kbmap.h) and
 *	written there too. JISかな and ローマ字 are the system's own and
 *	always there besides the object's kana layouts.
 *
 *	What was done is said on the message line and, for the tests, on the
 *	console ("kanatype: ..."). Started with "selftest" instead of an
 *	object, the program checks itself against the template and ends.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/dtreq.h>
#include <ts/hid.h>
#include <ts/uuid.h>
#include <ts/kbmap.h>
#include "kp_eng.h"

IMPORT ER tk_get_otm( SYSTIM *pk_tim );

#define TICK_MS		20
#define FRAME_W		16
#define TITLE_H		34
#define MIN_W		600
#define MIN_H		400

#define TEMPLATE	"019c403c-bb89-7133-acc4-ddd109fb8182"	/* the template: the selftest reads it */

#define FILES_MAX	100		/* resources of one kind looked for */
#define KB_ITEMS	16
#define KANA_ITEMS	18		/* the object's, and the system's two */
#define TRANS_ITEMS	16
#define JSON_MAX	( 256 * 1024 )

/* The data box's panels */
#define PN_RESULT	1
#define PN_LOOK		2
#define P_RES_RESTART	101
#define P_RES_CLOSE	102
#define P_LOOK_N	11
#define P_LOOK_OK	102

/* Keys: the keyboard's usages */
#define K_E		0x08
#define K_L		0x0F
#define K_KPENTER	0x58
#define K_RBRACKET_ALT	0x31		/* the ] key, as some keyboards report it */
#define K_RBRACKET	0x32
#define M_CTRL		( HID_MOD_LCTRL | HID_MOD_RCTRL )

/* The look */
#define C_GROUND	0x00DEDEDEU	/* the keyboard's and the scores' */
#define C_FRAME		0x00808080U
#define C_TEXT_BG	0x00FFFFFFU
#define C_KEY		0x00F8F8F8U
#define C_KEY_EDGE	0x00999999U
#define C_KANA		0x00333333U
#define C_LABEL		0x00666666U
#define C_SUB		0x00999999U
#define C_LEFT		0x00CC0000U
#define C_RIGHT		0x000000CCU
#define C_NEXT1		0x00FD3E3EU	/* red three quarters over the key */
#define C_NEXT1_EDGE	0x00CC0000U
#define C_NEXT2		0x00FB8888U
#define C_NEXT2_EDGE	0x00DD6666U
#define C_NEXT3		0x00FABABAU
#define C_NEXT3_EDGE	0x00DD9999U
#define C_NEXT4		0x00F9DADAU
#define C_SHIFT		0x00F9D3D3U
#define C_RIGHT_FLASH	0x007CD67CU
#define C_WRONG_FLASH	0x00FF0000U
#define C_PRESSED	0x00C4D9FAU
#define C_PRESSED_EDGE	0x004488CCU
#define C_DONE		0x00AAAAAAU
#define C_NOW		0x00FFFDE0U
#define C_PENDING	0x00333333U
#define C_RUBY		0x00888888U
#define C_RUBY_NOW	0x00555555U
#define C_IN_RIGHT	0x0000AA00U
#define C_IN_LATER	0x00CCCCCCU
#define C_RULE		0x00E0E0E0U
#define C_BLACK		0x00000000U
#define C_WHITE		0x00FFFFFFU

#define PAD		8
#define GAP		6
#define KEY_UNIT	46		/* a key, and the gap after it, before it is scaled */
#define KEY_GAP		2
#define FLASH_RIGHT_MS	200
#define FLASH_WRONG_MS	300

typedef struct {
	UB		name[KB_NAME];
	INT		file;			/* N of _1_N.json */
	KBLAYOUT	l;
} KBITEM;

typedef struct {
	UB		name[KB_NAME];
	INT		file;			/* N of _2_N.json, -1 the system's */
	INT		mode;			/* KP_M_* */
	KBKANAMAP	m;
} KANAITEM;

typedef struct {
	UB		name[KB_NAME];
	INT		file;
	KBTRANS		t;
} TRANSITEM;

/* ---------------------------------------------------------------- state */

static TS_UUID	g_obj;			/* the かな入力練習 object */
static UB	g_name[OB_NAME_MAX];
static UB	*g_xtad;		/* its record 0 */
static INT	g_xtadn;

static KBITEM	g_kb[KB_ITEMS];
static INT	g_nkb, g_akb;
static KANAITEM	g_kana[KANA_ITEMS];
static INT	g_nkana, g_akana;
static TRANSITEM g_tr[TRANS_ITEMS];
static INT	g_ntr, g_atr = -1;	/* -1: none */
static KBKANAMAP g_jis;			/* the hands romaji is measured with */

static KPENG	g_e;
static INT	g_look = 4;		/* keys shown ahead */
static KBSET	g_held;			/* keys held, translated */
static BOOL	g_lsh, g_rsh;
static UD	g_flash_until[256];	/* ms */
static UB	g_flash_kind[256];	/* 1 right, 2 wrong */
static BOOL	g_scored;		/* the scores shown at least once */
static KPSCORE	g_score;		/* as they were at the last key */

static TS_UUID	g_win, g_ch;
static ID	g_kw, g_port;
static INT	g_gid = -1, g_w, g_h;
static UI	g_ui;
static BOOL	g_dirty, g_closing;
static BOOL	g_full;
static T_OBWPOS	g_restore;
static UW	g_paper = C_GROUND;	/* the object's window.backgroundColor */

static UB	g_json[JSON_MAX + 1];
static UB	g_out[JSON_MAX];
static char	g_meta_none[1];
static char	*g_meta = g_meta_none;	/* the metadata meta_of read last ("" when none) */

static void event( CONST T_OBNTM *m );

/* ---------------------------------------------------------------- saying things */

static void note( CONST char *s )
{
	char	line[300];

	snprintf(line, sizeof(line), "kanatype: %s\n", s);
	tm_putstring((CONST UB *)line);
}

static void say( CONST char *s )
{
	(void)wm_msg_put(s);
}

/*
 * The time in ms. The kernel's SYSTIM is two 32-bit words, which a
 * program's tk/typedef.h does not say (its W and UW are as wide as a
 * long), so the two words are taken as such.
 */
static UD now_ms( void )
{
	struct {
		INT	hi;
		UINT	lo;
	} t;

	if ( tk_get_otm((SYSTIM *)(void *)&t) < E_OK ) return 0;
	return ( (UD)(UINT)t.hi << 32 ) | (UD)t.lo;
}

static UINT rnd( void )
{
	UINT	v = 0;

	(void)ts_get_random(&v, sizeof(v));
	return v;
}

static BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

static CONST char *key_name( UINT u )
{
	CONST char	*s = kb_key_name(u);

	return ( s != NULL ) ? s : "?";
}

/* ---------------------------------------------------------------- the object */

/* A whole record, 0 after it; NULL when it cannot be read */
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
		SZ	n = ( size - at > 65536 ) ? 65536 : (SZ)( size - at );

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

/* A resource of the object into g_json, 0 after it: its length, or an error */
static INT res_read( ID k, CONST char *name )
{
	SZ	asz = 0;
	INT	at = 0;

	for ( ;; ) {
		ER	er = ob_rea_res(k, (CONST UB *)name, (D)at, g_json + at, JSON_MAX - at, &asz);

		if ( er < E_OK ) return ( at > 0 ) ? at : er;
		if ( asz <= 0 ) break;
		at += (INT)asz;
		if ( at >= JSON_MAX ) return E_LIMIT;
	}
	g_json[at] = 0;
	return at;
}

/* JSON written as a resource of the object as JSON.stringify writes it */
static ER res_write( CONST char *name, CONST UB *json, INT len )
{
	INT	n = kb_json_compact(json, len, g_out, JSON_MAX);
	ID	k;
	ER	er;

	if ( n < 0 ) return n;
	k = ob_opn_obj(&g_obj, OB_OP_R | OB_OP_WRITE);
	if ( k <= 0 ) return (ER)k;
	er = ob_wri_res(k, (CONST UB *)name, g_out, n);
	ob_cls_obj(k);
	return er;
}

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

/* The paper the object asks for: window.backgroundColor, "#rrggbb" */
static void paper_read( void )
{
	T_JSON	root, w;
	UB	c[16];
	INT	n = meta_of(&g_obj);

	g_paper = C_GROUND;
	if ( n > 0 && js_parse((CONST UB *)g_meta, n, &root) >= E_OK && js_get(&root, "window", &w) >= E_OK
	  && js_get_str(&w, "backgroundColor", c, sizeof(c)) == 7 && c[0] == '#' ) {
		g_paper = (UW)strtoul((char *)c + 1, NULL, 16) & 0x00FFFFFFU;
	}
}

static void name_or( UB *name, CONST UB *meta, CONST char *dflt, INT n )
{
	if ( meta[0] != 0 ) snprintf((char *)name, KB_NAME, "%s", (CONST char *)meta);
	else snprintf((char *)name, KB_NAME, "%s %d", dflt, n);
}

/* Every keyboard, kana layout and translation the object keeps, and the system's where it lacks them */
static void layouts_read( void )
{
	char	rn[24];
	UB	nm[KB_NAME];
	INT	i, n;
	ID	k = ob_opn_obj(&g_obj, OB_OP_R);

	g_nkb = g_nkana = g_ntr = 0;
	for ( i = 0; k > 0 && i < FILES_MAX && g_nkb < KB_ITEMS; i++ ) {
		snprintf(rn, sizeof(rn), "_1_%d.json", i);
		n = res_read(k, rn);
		if ( n <= 0 ) break;
		if ( kb_read_keyboard(g_json, n, &g_kb[g_nkb].l) < E_OK ) break;
		(void)kb_meta_name(g_json, n, nm, sizeof(nm));
		name_or(g_kb[g_nkb].name, nm, "レイアウト", i);
		g_kb[g_nkb++].file = i;
	}
	for ( i = 0; k > 0 && i < FILES_MAX && g_nkana < KANA_ITEMS - 2; i++ ) {
		snprintf(rn, sizeof(rn), "_2_%d.json", i);
		n = res_read(k, rn);
		if ( n <= 0 ) break;
		if ( kb_read_kana(g_json, n, &g_kana[g_nkana].m) < E_OK ) break;
		(void)kb_meta_name(g_json, n, nm, sizeof(nm));
		name_or(g_kana[g_nkana].name, nm, "配列", i);
		g_kana[g_nkana].mode = KP_M_KANA;
		g_kana[g_nkana++].file = i;
	}
	for ( i = 0; k > 0 && i < FILES_MAX && g_ntr < TRANS_ITEMS; i++ ) {
		snprintf(rn, sizeof(rn), "_3_%d.json", i);
		n = res_read(k, rn);
		if ( n <= 0 ) break;
		if ( kb_read_trans(g_json, n, &g_tr[g_ntr].t) < E_OK ) break;
		(void)kb_meta_name(g_json, n, nm, sizeof(nm));
		name_or(g_tr[g_ntr].name, nm, "変換", i);
		g_tr[g_ntr++].file = i;
	}
	if ( k > 0 ) ob_cls_obj(k);

	/* none: the system's, written in the object as well */
	if ( g_nkb == 0 ) {
		n = (INT)strlen(kb_jp109_json);
		(void)res_write("_1_0.json", (CONST UB *)kb_jp109_json, n);
		(void)kb_read_keyboard((CONST UB *)kb_jp109_json, n, &g_kb[0].l);
		snprintf((char *)g_kb[0].name, KB_NAME, "%s", ( g_kb[0].l.name[0] != 0 ) ? (char *)g_kb[0].l.name : "日本語109キー");
		g_kb[0].file = 0;
		g_nkb = 1;
	}
	if ( g_nkana == 0 ) {
		n = (INT)strlen(kb_tron_json);
		(void)res_write("_2_0.json", (CONST UB *)kb_tron_json, n);
		(void)kb_read_kana((CONST UB *)kb_tron_json, n, &g_kana[0].m);
		snprintf((char *)g_kana[0].name, KB_NAME, "%s", ( g_kana[0].m.name[0] != 0 ) ? (char *)g_kana[0].m.name : "TRON配列");
		g_kana[0].file = 0;
		g_kana[0].mode = KP_M_KANA;
		g_nkana = 1;
	}

	/* the system's two, after the object's */
	(void)kb_read_kana((CONST UB *)kb_jis_json, (INT)strlen(kb_jis_json), &g_jis);
	g_kana[g_nkana].m = g_jis;
	snprintf((char *)g_kana[g_nkana].name, KB_NAME, "JISかな");
	g_kana[g_nkana].file = -1;
	g_kana[g_nkana++].mode = KP_M_KANA;
	g_kana[g_nkana].m = g_jis;
	snprintf((char *)g_kana[g_nkana].name, KB_NAME, "ローマ字");
	g_kana[g_nkana].file = -1;
	g_kana[g_nkana++].mode = KP_M_ROMAJI;

	g_akb = 0;
	g_akana = 0;
	g_atr = -1;
}

static void kana_apply( void )
{
	KANAITEM	*a = &g_kana[g_akana];

	kp_set_map(&g_e, a->mode, &a->m);
}

/* ---------------------------------------------------------------- the window's parts */

typedef struct {
	INT	x, y, w, h;
} BOX;

static INT	g_rows;			/* rows the question takes */
static BOX	g_text, g_keys, g_stats;

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

static INT text( INT x, INT y, CONST char *s, UW c, INT px )
{
	return dp_text(g_gid, x, y, (CONST UB *)s, c, px);
}

static INT width( CONST char *s, INT px )
{
	return dp_text_width((CONST UB *)s, px);
}

/* Bold: drawn twice, a pixel apart */
static INT text_bold( INT x, INT y, CONST char *s, UW c, INT px )
{
	(void)text(x + 1, y, s, c, px);
	return text(x, y, s, c, px) + 1;
}

/* Code points of the question, as UTF-8 */
static void q_str( INT a, INT b, char *out, INT max )
{
	(void)kp_utf8(g_e.q + a, b - a, (UB *)out, max);
}

/* ---------------------------------------------------------------- the question */

#define Q_PX		18
#define RUBY_PX		10
#define Q_ROW		36
#define IN_PX		16

/* A word's width: the wider of it and its reading */
static INT seg_width( CONST KPSEG *s )
{
	char	b[256];
	INT	w, rw = 0;

	q_str(s->d0, s->d1, b, sizeof(b));
	w = width(b, Q_PX);
	if ( s->r1 > s->r0 ) {
		q_str(s->r0, s->r1, b, sizeof(b));
		rw = width(b, RUBY_PX);
	}
	return ( rw > w ) ? rw : w;
}

static INT phrase_width( CONST KPPHRASE *p )
{
	INT	i, w = 0;

	for ( i = 0; i < p->nseg; i++ ) w += seg_width(&g_e.seg[p->seg0 + i]);
	return w;
}

/* The phrases laid out in rows within width: the rows they take; drawn when draw */
static INT question_lay( INT x0, INT y0, INT wmax, BOOL draw )
{
	INT	sp = width(" ", Q_PX), x = x0, row = 0, i, k;

	for ( i = 0; i < g_e.nph; i++ ) {
		CONST KPPHRASE	*p = &g_e.ph[i];
		INT		pw = phrase_width(p);
		UW		fg = ( i < g_e.cph ) ? C_DONE : ( i == g_e.cph ) ? C_BLACK : C_PENDING;
		UW		rc = ( i == g_e.cph ) ? C_RUBY_NOW : ( i < g_e.cph ) ? C_DONE : C_RUBY;
		INT		top;

		if ( i > 0 ) x += sp;
		if ( x > x0 && x + pw > x0 + wmax ) {
			x = x0;
			row++;
		}
		top = y0 + row * Q_ROW;
		if ( draw ) {
			if ( i == g_e.cph ) fill(x, top + 2, x + pw, top + Q_ROW - 2, C_NOW);
			for ( k = 0; k < p->nseg; k++ ) {
				CONST KPSEG	*s = &g_e.seg[p->seg0 + k];
				INT		sw = seg_width(s);
				char		b[256];
				INT		dw;

				q_str(s->d0, s->d1, b, sizeof(b));
				dw = width(b, Q_PX);
				(void)text(x + ( sw - dw ) / 2, top + 28, b, fg, Q_PX);
				if ( s->r1 > s->r0 ) {
					INT	rw;

					q_str(s->r0, s->r1, b, sizeof(b));
					rw = width(b, RUBY_PX);
					(void)text(x + ( sw - rw ) / 2, top + 11, b, rc, RUBY_PX);
				}
				x += sw;
			}
		} else {
			x += pw;
		}
	}
	return row + 1;
}

/* What is typed of the phrase in hand: done green, the next underlined, the rest grey */
static void input_draw( INT x, INT y )
{
	CONST KPPHRASE	*p;
	INT		i, len;

	if ( !g_e.practising && !g_e.completed ) return;
	if ( g_e.cph >= g_e.nph ) return;
	p = &g_e.ph[g_e.cph];
	len = kp_expected_len(&g_e);
	for ( i = 0; i < p->nin; i++ ) {
		char	b[8];
		INT	w, cw;
		UW	c;
		BOOL	now = (BOOL)( i >= g_e.cch && i < g_e.cch + len );

		(void)kp_utf8(g_e.in + p->in0 + i, 1, (UB *)b, sizeof(b));
		w = width(b, IN_PX);
		cw = ( w > IN_PX ) ? w : IN_PX;
		c = ( i < g_e.cch ) ? C_IN_RIGHT : now ? C_PENDING : C_IN_LATER;
		if ( now ) {
			(void)text_bold(x + ( cw - w ) / 2, y + 17, b, c, IN_PX);
			fill(x, y + 21, x + cw, y + 23, C_PENDING);
		} else {
			(void)text(x + ( cw - w ) / 2, y + 17, b, c, IN_PX);
		}
		x += cw;
	}
}

/* ---------------------------------------------------------------- the scores */

#define ST_LABEL_PX	11
#define ST_VALUE_PX	13
#define ST_ROW		22

static INT stats_items( char lab[7][16], char val[7][40] )
{
	KPSCORE	s;
	INT	cur, tot;
	char	a[16];

	kp_progress(&g_e, &cur, &tot);
	strcpy(lab[0], "速度");
	strcpy(lab[1], "正誤率");
	strcpy(lab[2], "左右率");
	strcpy(lab[3], "シフト率");
	strcpy(lab[4], "移動距離");
	strcpy(lab[5], "総移動距離");
	strcpy(lab[6], "問題");
	if ( !g_scored ) {
		strcpy(val[0], "0 文字/分");
		strcpy(val[1], "100%");
		strcpy(val[2], "0:0");
		strcpy(val[3], "0%");
		strcpy(val[4], "0 u/字");
		strcpy(val[5], "0 u");
	} else {
		s = g_score;
		snprintf(val[0], 40, "%d 文字/分", s.speed);
		kp_fixed(s.accuracy, 1, a);
		snprintf(val[1], 40, "%s%%", a);
		snprintf(val[2], 40, "%d:%d", s.left, s.right);
		kp_fixed(s.shift_ratio, 1, a);
		snprintf(val[3], 40, "%s%%", a);
		kp_fixed(s.distance, 2, a);
		snprintf(val[4], 40, "%s u/字", a);
		kp_fixed(s.total, 1, a);
		snprintf(val[5], 40, "%s u", a);
	}
	snprintf(val[6], 40, "%d / %d", cur, tot);
	return 7;
}

/* The scores laid out in rows within width: the rows they take; drawn when draw */
static INT stats_lay( INT x0, INT y0, INT wmax, BOOL draw )
{
	char	lab[7][16], val[7][40];
	INT	n = stats_items(lab, val), i, x = x0, row = 0;

	for ( i = 0; i < n; i++ ) {
		INT	lw = width(lab[i], ST_LABEL_PX), vw = width(val[i], ST_VALUE_PX) + 1, w = lw + 4 + vw;

		if ( x > x0 && x + w > x0 + wmax ) {
			x = x0;
			row++;
		}
		if ( draw ) {
			INT	y = y0 + row * ST_ROW;

			(void)text(x, y + 15, lab[i], C_LABEL, ST_LABEL_PX);
			(void)text_bold(x + lw + 4, y + 16, val[i], C_KANA, ST_VALUE_PX);
		}
		x += w + 16;
	}
	return row + 1;
}

/* ---------------------------------------------------------------- the keyboard */

static D	g_u16;			/* a key's pitch on the screen, 2^16 being a pixel */
static INT	g_kx, g_ky;		/* where the keyboard's corner is */
static INT	g_scale16;

static INT kpx( INT v )
{
	return (INT)( (D)v * g_u16 / ( (D)KB_UNIT << 16 ) );
}

static INT spx( INT px )
{
	INT	v = (INT)( ( (D)px * g_scale16 + 32768 ) >> 16 );

	return ( v < 6 ) ? 6 : v;
}

/* The keyboard fitted to its box, no larger than its own size */
static void keys_fit( CONST KBLAYOUT *l, CONST BOX *b )
{
	INT	unit = KEY_UNIT + KEY_GAP, cw = b->w - 2 * 8 - 2, ch = b->h - 2 * 8 - 2;
	D	kw = (D)l->w * unit / KB_UNIT, kh = (D)l->h * unit / KB_UNIT;
	D	sx, sy, s;

	if ( kw <= 0 || kh <= 0 || cw <= 0 || ch <= 0 ) {
		g_scale16 = 65536;
	} else {
		sx = ( (D)cw << 16 ) / kw;
		sy = ( (D)ch << 16 ) / kh;
		s = ( sx < sy ) ? sx : sy;
		g_scale16 = (INT)( ( s > 65536 ) ? 65536 : s );
	}
	g_u16 = (D)unit * g_scale16;
	g_kx = b->x + 1 + 8 + ( cw - (INT)( kw * g_scale16 >> 16 ) ) / 2;
	g_ky = b->y + 1 + 8 + ( ch - (INT)( kh * g_scale16 >> 16 ) ) / 2;
	if ( g_kx < b->x + 9 ) g_kx = b->x + 9;
	if ( g_ky < b->y + 9 ) g_ky = b->y + 9;
}

/* A turned key: filled scan line by scan line between its edges, and framed */
static void quad( CONST INT *xs, CONST INT *ys, UW bg, UW edge )
{
	INT	y, i, top = ys[0], bot = ys[0];

	for ( i = 1; i < 4; i++ ) {
		if ( ys[i] < top ) top = ys[i];
		if ( ys[i] > bot ) bot = ys[i];
	}
	for ( y = top; y < bot; y++ ) {
		INT	lo = 0x7FFFFFFF, hi = -0x7FFFFFFF, yy = 2 * y + 1;

		for ( i = 0; i < 4; i++ ) {
			INT	x0 = xs[i], y0 = ys[i], x1 = xs[( i + 1 ) & 3], y1 = ys[( i + 1 ) & 3];

			if ( ( 2 * y0 <= yy && yy < 2 * y1 ) || ( 2 * y1 <= yy && yy < 2 * y0 ) ) {
				INT	x = x0 + ( yy - 2 * y0 ) * ( x1 - x0 ) / ( 2 * ( y1 - y0 ) );

				if ( x < lo ) lo = x;
				if ( x > hi ) hi = x;
			}
		}
		if ( hi > lo ) fill(lo, y, hi, y + 1, bg);
	}
	for ( i = 0; i < 4; i++ ) {
		(void)dp_line(g_gid, xs[i], ys[i], xs[( i + 1 ) & 3], ys[( i + 1 ) & 3], edge);
	}
}

/* How strongly each key is shown to be next (1 the first .. 4), and the shift to hold */
static UB	g_next[256];
static BOOL	g_shift_req[2];

static void next_mark( void )
{
	KPNEXT	nx[KP_LOOK_MAX * 2];
	INT	n, i, k;

	memset(g_next, 0, sizeof(g_next));
	g_shift_req[0] = g_shift_req[1] = FALSE;
	if ( !g_e.practising ) return;
	n = kp_next(&g_e, g_look, nx);
	for ( i = 0; i < n && i < 4; i++ ) {
		if ( nx[i].nkey > 0 ) {
			for ( k = 0; k < nx[i].nkey; k++ ) {
				UINT	u = nx[i].keys[k];

				if ( u < 256 && g_next[u] == 0 ) g_next[u] = (UB)( i + 1 );
			}
		} else {
			UINT	u = nx[i].usage;

			if ( u < 256 && g_next[u] == 0 ) g_next[u] = (UB)( i + 1 );
			if ( i == 0 && nx[i].shift == KP_SH_LEFT ) g_shift_req[0] = TRUE;
			if ( i == 0 && nx[i].shift == KP_SH_RIGHT ) g_shift_req[1] = TRUE;
		}
	}
}

static void key_draw( CONST KBKEY *k, CONST KANAITEM *ka, UD now )
{
	UINT		u = k->usage;
	INT		x = g_kx + kpx(k->x), y = g_ky + kpx(k->y);
	INT		w = kpx(k->w) - spx(KEY_GAP), h = kpx(k->h) - spx(KEY_GAP);
	UW		bg = C_KEY, edge = C_KEY_EDGE;
	INT		lvl = ( u < 256 ) ? g_next[u] : 0, ew = 1;
	BOOL		white = (BOOL)( lvl == 1 || lvl == 2 );
	BOOL		shift = (BOOL)( ( u == KB_K_LSHIFT && g_shift_req[0] ) || ( u == KB_K_RSHIFT && g_shift_req[1] ) );
	CONST UH	*kn = NULL, *kl = NULL, *kr = NULL;
	INT		cx = x + w / 2, cy = y + h / 2, lines = 0, ty;
	char		b[48], c[48];

	if ( w < 2 || h < 2 ) return;
	switch ( lvl ) {
	case 1:	bg = C_NEXT1;	edge = C_NEXT1_EDGE;	break;
	case 2:	bg = C_NEXT2;	edge = C_NEXT2_EDGE;	break;
	case 3:	bg = C_NEXT3;	edge = C_NEXT3_EDGE;	break;
	case 4:	bg = C_NEXT4;				break;
	default:					break;
	}
	if ( shift ) {
		bg = C_SHIFT;
		edge = C_NEXT1_EDGE;
		ew = 2;
	}
	if ( u != 0 && KB_IN(&g_held, u) ) {
		bg = C_PRESSED;
		edge = C_PRESSED_EDGE;
	}
	if ( u != 0 && u < 256 && g_flash_until[u] > now ) {
		bg = ( g_flash_kind[u] == 2 ) ? C_WRONG_FLASH : C_RIGHT_FLASH;
	}

	if ( k->r != 0 ) {
		INT	cs = kb_cos(k->r), sn = kb_sin(k->r);
		INT	xs[4], ys[4], i;
		static CONST INT	sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };

		for ( i = 0; i < 4; i++ ) {
			D	dx = (D)sx[i] * w / 2, dy = (D)sy[i] * h / 2;

			xs[i] = cx + (INT)( ( dx * cs - dy * sn ) >> 30 );
			ys[i] = cy + (INT)( ( dx * sn + dy * cs ) >> 30 );
		}
		quad(xs, ys, bg, edge);
	} else {
		fill(x, y, x + w, y + h, bg);
		frame(x, y, x + w, y + h, edge, ew);
	}

	/* what is on it: the kana, small the shifted ones, else the label */
	if ( ka != NULL && ka->mode == KP_M_KANA && u != 0 ) {
		kn = kb_kana_of(&ka->m, u, KB_PLAIN);
		kl = kb_kana_of(&ka->m, u, KB_LEFT);
		kr = kb_kana_of(&ka->m, u, KB_RIGHT);
	}
	if ( kn != NULL ) lines += spx(16);
	if ( kl != NULL || kr != NULL ) lines += spx(8);
	if ( kn == NULL && k->label[0] != 0 ) lines += spx(11);
	ty = cy - lines / 2;
	if ( kn != NULL ) {
		INT	px = spx(13);

		(void)kp_utf8(kn, KB_CP, (UB *)b, sizeof(b));
		(void)text_bold(cx - width(b, px) / 2, ty + px, b, white ? C_WHITE : C_KANA, px);
		ty += spx(16);
	}
	if ( kl != NULL || kr != NULL ) {
		INT	px = spx(7), wl = 0, wr = 0, gap = spx(3), sx0;

		b[0] = c[0] = 0;
		if ( kl != NULL ) (void)kp_utf8(kl, KB_CP, (UB *)b, sizeof(b));
		if ( kr != NULL ) (void)kp_utf8(kr, KB_CP, (UB *)c, sizeof(c));
		if ( kl != NULL && kr != NULL && strcmp(b, c) == 0 ) {
			/* the same with either shift: shown once */
			wl = width(b, px);
			(void)text(cx - wl / 2, ty + px, b, C_SUB, px);
		} else {
			if ( kl != NULL ) wl = width(b, px);
			if ( kr != NULL ) wr = width(c, px);
			sx0 = cx - ( wl + wr + ( ( kl != NULL && kr != NULL ) ? gap : 0 ) ) / 2;
			if ( kl != NULL ) {
				(void)text(sx0, ty + px, b, C_LEFT, px);
				sx0 += wl + gap;
			}
			if ( kr != NULL ) (void)text(sx0, ty + px, c, C_RIGHT, px);
		}
		ty += spx(8);
	}
	if ( kn == NULL && k->label[0] != 0 ) {
		INT	px = spx(9);

		(void)text(cx - width((CONST char *)k->label, px) / 2, ty + px, (CONST char *)k->label,
			   white ? C_WHITE : C_LABEL, px);
	}
}

static void keys_draw( UD now )
{
	CONST KBLAYOUT	*l = &g_kb[g_akb].l;
	INT		i;

	fill(g_keys.x, g_keys.y, g_keys.x + g_keys.w, g_keys.y + g_keys.h, C_GROUND);
	frame(g_keys.x, g_keys.y, g_keys.x + g_keys.w, g_keys.y + g_keys.h, C_FRAME, 1);
	keys_fit(l, &g_keys);
	next_mark();
	for ( i = 0; i < l->n; i++ ) key_draw(&l->k[i], &g_kana[g_akana], now);
}

/* ---------------------------------------------------------------- the whole */

static void layout( void )
{
	INT	srows, th, sh;

	g_rows = question_lay(0, 0, g_w - 2 * PAD - 28, FALSE);
	th = 10 + g_rows * Q_ROW + 4 + 4 + 1 + 4 + 24 + 10;
	g_text.x = PAD;
	g_text.y = PAD;
	g_text.w = g_w - 2 * PAD;
	g_text.h = th;
	srows = stats_lay(0, 0, g_w - 2 * PAD - 20, FALSE);
	sh = srows * ST_ROW + 12;
	g_stats.x = PAD;
	g_stats.w = g_w - 2 * PAD;
	g_stats.h = sh;
	g_stats.y = g_h - PAD - sh;
	g_keys.x = PAD;
	g_keys.w = g_w - 2 * PAD;
	g_keys.y = g_text.y + th + GAP;
	g_keys.h = g_stats.y - GAP - g_keys.y;
	if ( g_keys.h < 40 ) g_keys.h = 40;
}

static void paint( void )
{
	UD	now = now_ms();
	INT	iy;

	if ( g_gid < 0 ) return;
	layout();
	fill(0, 0, g_w, g_h, g_paper);

	/* the question and what is typed */
	fill(g_text.x, g_text.y, g_text.x + g_text.w, g_text.y + g_text.h, C_TEXT_BG);
	frame(g_text.x, g_text.y, g_text.x + g_text.w, g_text.y + g_text.h, C_FRAME, 1);
	(void)question_lay(g_text.x + 14, g_text.y + 10, g_text.w - 28, TRUE);
	iy = g_text.y + 10 + g_rows * Q_ROW + 4 + 4;
	fill(g_text.x + 14, iy, g_text.x + g_text.w - 14, iy + 1, C_RULE);
	input_draw(g_text.x + 14, iy + 5);

	keys_draw(now);

	fill(g_stats.x, g_stats.y, g_stats.x + g_stats.w, g_stats.y + g_stats.h, C_GROUND);
	frame(g_stats.x, g_stats.y, g_stats.x + g_stats.w, g_stats.y + g_stats.h, C_FRAME, 1);
	(void)stats_lay(g_stats.x + 10, g_stats.y + 6, g_stats.w - 20, TRUE);

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
	g_ui.h = g_h;
}

/* The window, where the object last stood, and asking to hear keys let go */
static ER win_open( CONST char *title )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	T_JSON		root, w, pos;
	SZ		asz = 0;
	char		json[260];
	char		shown[TS_UUID_STRLEN + 1];
	INT		x = 100, y = 80, ww = 850, wh = 650, mn = meta_of(&g_obj);

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
	ts_uuid_text(&g_obj, shown);	/* what it shows, for ウインドウ一覧 */
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7,\"icon\":\"%s\",\"shows\":\"%s\"}",
		 x, y, x + ww + FRAME_W, y + wh + TITLE_H, SYSDEF_PROG_KANATYPE, shown);
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
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE | OB_E_DROP;
	req.id = 5;
	req.flags = OB_N_KEYUP;
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) {
		if ( g_kw > 0 ) ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
		g_kw = 0;
		return E_OBJ;
	}
	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + ww + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + wh + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	g_ui.kw = g_kw;
	g_ui.port = g_port;
	(void)ui_uuid(SYSDEF_BOX_KANATYPE, &g_ui.box);
	work_size();
	return E_OK;
}

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
	g_dirty = TRUE;
}

/*
 * What came while the desktop was asked: presses and keys were the
 * desktop's own panel's; what is held is forgotten, its letting go
 * having gone there too.
 */
static void settle( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( !g_closing && ob_rea_rec(g_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( ( m.event & ( OB_E_PRESS | OB_E_KEY ) ) != 0 ) continue;
		event(&m);
	}
	memset(&g_held, 0, sizeof(g_held));
	g_lsh = g_rsh = FALSE;
}

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

/* 全画面表示: the size of the screen above the message line, or back where it stood */
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

/* 背景色変更: asked of the desktop, which keeps it as the object's paper */
static void paper_ask( void )
{
	T_DTREQ	rq;
	T_DTANS	an;
	char	s[40];

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PAPER;
	rq.target = g_obj;
	rq.look.bgcol = g_paper;
	if ( ask_desktop(&rq, &an, 30 * 60 * 1000) < E_OK ) return;
	g_paper = an.look.bgcol & 0x00FFFFFFU;
	snprintf(s, sizeof(s), "paper #%06x", (UINT)g_paper);
	note(s);
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- the practice */

static void load_question( INT i )
{
	kp_load(&g_e, i);
	memset(g_flash_until, 0, sizeof(g_flash_until));
	g_dirty = TRUE;
}

static void practice_start( void )
{
	char	s[80];
	INT	cur, tot;

	memset(&g_held, 0, sizeof(g_held));
	kp_start(&g_e, rnd, now_ms());
	g_scored = TRUE;
	kp_score(&g_e, now_ms(), &g_score);
	kp_progress(&g_e, &cur, &tot);
	snprintf(s, sizeof(s), "started %d questions, %s", tot, (char *)g_kana[g_akana].name);
	note(s);
	say("練習中... (Escで中断)");
	g_dirty = TRUE;
}

static void practice_stop( CONST char *msg )
{
	memset(&g_held, 0, sizeof(g_held));
	kp_stop(&g_e);
	note("stopped");
	if ( msg != NULL ) say(msg);
	g_dirty = TRUE;
}

/* All done: the scores in the panel, and back to the first question */
static void practice_done( void )
{
	static char	lines[8][80];
	CONST char	*lp[8];
	KPSCORE		s;
	UI		q;
	T_OBNTM		m;
	char		a[16], b[16], c[16], d[16];
	INT		cur, tot, num, ev, done = 0, i;

	kp_score(&g_e, g_e.end, &s);
	kp_progress(&g_e, &cur, &tot);
	kp_fixed(s.accuracy, 1, a);
	kp_fixed(s.shift_ratio, 1, b);
	kp_fixed(s.distance, 2, c);
	kp_fixed(s.total, 1, d);
	snprintf(lines[0], 80, "全問完了!");
	snprintf(lines[1], 80, "問題数: %d 問", tot);
	snprintf(lines[2], 80, "速度: %d 文字/分", s.speed);
	snprintf(lines[3], 80, "正誤率: %s%%", a);
	snprintf(lines[4], 80, "左右率: %d:%d", s.left, s.right);
	snprintf(lines[5], 80, "シフト率: %s%%", b);
	snprintf(lines[6], 80, "移動距離: %s u/字 (総 %s u)", c, d);
	snprintf(lines[7], 80, "経過時間: %d 秒", s.seconds);
	{
		char	t[200];

		snprintf(t, sizeof(t), "done speed %d accuracy %s keys %d seconds %d", s.speed, a, s.keys, s.seconds);
		note(t);
	}
	paint();
	if ( ui_dlg_open(&g_ui, PN_RESULT, &q) >= E_OK ) {
		for ( i = 0; i < 8; i++ ) {
			lp[i] = lines[i];
			ui_set_text(&q, i + 1, lp[i]);
		}
		while ( done == 0 ) {
			ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
			switch ( ev ) {
			case UI_EV_BUTTON:
				done = ( num == P_RES_RESTART ) ? 1 : 2;
				break;
			case UI_EV_CLOSE:
				done = 2;
				break;
			case UI_EV_NONE:
				if ( m.id == 3 && m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0
				  && ( m.code == KB_K_ENTER || m.code == K_KPENTER ) ) {
					done = 1;
				}
				break;
			default:
				break;
			}
		}
		ui_dlg_close(&q);
	}
	memset(&g_held, 0, sizeof(g_held));
	load_question(0);
	note(( done == 1 ) ? "result: restart" : "result: closed");
	say(( done == 1 ) ? "Enterキーで練習開始" : "全問完了。Enterキーで再開");
}

/* 先読みキー数 (1〜8) */
static void ask_look( void )
{
	UI	q;
	T_OBNTM	m;
	INT	v = g_look, num, ev, done = 0;
	char	s[40];

	if ( ui_dlg_open(&g_ui, PN_LOOK, &q) < E_OK ) return;
	ui_set_fields(&q, P_LOOK_N, &v, 1);
	while ( done == 0 ) {
		ev = ( ui_wait(&g_ui, &m, TMO_FEVR) >= E_OK ) ? ui_dlg_take(&q, &m, &num) : UI_EV_CLOSE;
		switch ( ev ) {
		case UI_EV_BUTTON:
			done = ( num == P_LOOK_OK ) ? 1 : -1;
			break;
		case UI_EV_CLOSE:
			done = -1;
			break;
		case UI_EV_NONE:
			if ( m.id == 3 && m.event == OB_E_KEY && ( m.mods & OB_KEY_UP ) == 0
			  && ( m.code == KB_K_ENTER || m.code == K_KPENTER ) ) {
				done = 1;
			}
			break;
		default:
			break;
		}
	}
	if ( done > 0 ) (void)ui_fields(&q, P_LOOK_N, &v, 1);
	ui_dlg_close(&q);
	memset(&g_held, 0, sizeof(g_held));
	if ( done > 0 && v >= 1 && v <= KP_LOOK_MAX ) {
		g_look = v;
		snprintf(s, sizeof(s), "lookahead %d", v);
		note(s);
	}
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- choosing */

static void choose_keyboard( INT i )
{
	char	s[120];

	if ( i < 0 || i >= g_nkb || i == g_akb ) return;
	g_akb = i;
	snprintf(s, sizeof(s), "keyboard %s", (char *)g_kb[i].name);
	note(s);
	snprintf(s, sizeof(s), "キーボードレイアウト: %s", (char *)g_kb[i].name);
	say(s);
	g_dirty = TRUE;
}

static void choose_kana( INT i )
{
	char	s[120];

	if ( i < 0 || i >= g_nkana || i == g_akana ) return;
	g_akana = i;
	kana_apply();
	snprintf(s, sizeof(s), "kana %s", (char *)g_kana[i].name);
	note(s);
	snprintf(s, sizeof(s), "かな入力方式: %s", (char *)g_kana[i].name);
	say(s);
	g_dirty = TRUE;
}

static void choose_trans( INT i )
{
	char	s[120];
	CONST char *name;

	if ( i == g_atr || i < -1 || i >= g_ntr ) return;
	if ( g_e.practising ) practice_stop(NULL);
	memset(&g_held, 0, sizeof(g_held));
	g_atr = i;
	name = ( i < 0 ) ? "なし" : (CONST char *)g_tr[i].name;
	snprintf(s, sizeof(s), "trans %s", name);
	note(s);
	snprintf(s, sizeof(s), "キーコード変換: %s", name);
	say(s);
	g_dirty = TRUE;
}

/* ---------------------------------------------------------------- what is dropped */

/* The next number of a kind's resources: one past the largest */
static INT next_file( INT kind )
{
	INT	i, n = 0;

	if ( kind == KB_KIND_KEYBOARD ) {
		for ( i = 0; i < g_nkb; i++ ) if ( g_kb[i].file + 1 > n ) n = g_kb[i].file + 1;
	} else if ( kind == KB_KIND_KANA ) {
		for ( i = 0; i < g_nkana; i++ ) if ( g_kana[i].file + 1 > n ) n = g_kana[i].file + 1;
	} else {
		for ( i = 0; i < g_ntr; i++ ) if ( g_tr[i].file + 1 > n ) n = g_tr[i].file + 1;
	}
	return n;
}

/* The JSON an object holds: in record 1 as a file is brought in, else record 0 */
static INT json_of( CONST TS_UUID *u )
{
	UB	*b;
	INT	n = 0, rec;

	for ( rec = 1; rec >= 0; rec-- ) {
		b = rec_read(u, rec, &n);
		if ( b == NULL ) continue;
		if ( n > 0 && n <= JSON_MAX ) {
			memcpy(g_json, b, (size_t)n);
			g_json[n] = 0;
			free(b);
			return n;
		}
		free(b);
	}
	return 0;
}

/* A layout's JSON added to the object as the next of its kind, and chosen */
static ER layout_add( CONST UB *json, INT n, CONST UB *fallback )
{
	INT	kind = kb_kind(json, n), f;
	UB	nm[KB_NAME];
	char	rn[24], s[160];
	ER	er = E_OK;

	s[0] = 0;
	(void)kb_meta_name(json, n, nm, sizeof(nm));
	if ( nm[0] == 0 ) snprintf((char *)nm, sizeof(nm), "%s", (CONST char *)fallback);
	f = next_file(kind);
	switch ( kind ) {
	case KB_KIND_KEYBOARD:
		if ( g_nkb >= KB_ITEMS ) return E_LIMIT;
		snprintf(rn, sizeof(rn), "_1_%d.json", f);
		if ( kb_read_keyboard(json, n, &g_kb[g_nkb].l) < E_OK || ( er = res_write(rn, json, n) ) < E_OK ) break;
		snprintf((char *)g_kb[g_nkb].name, KB_NAME, "%s", (char *)nm);
		g_kb[g_nkb].file = f;
		g_akb = g_nkb++;
		snprintf(s, sizeof(s), "キーボードレイアウト「%s」を追加しました", (char *)nm);
		break;
	case KB_KIND_TRANS:
		if ( g_ntr >= TRANS_ITEMS ) return E_LIMIT;
		snprintf(rn, sizeof(rn), "_3_%d.json", f);
		if ( kb_read_trans(json, n, &g_tr[g_ntr].t) < E_OK || ( er = res_write(rn, json, n) ) < E_OK ) break;
		snprintf((char *)g_tr[g_ntr].name, KB_NAME, "%s", (char *)nm);
		g_tr[g_ntr].file = f;
		g_atr = g_ntr++;
		if ( g_e.practising ) practice_stop(NULL);
		memset(&g_held, 0, sizeof(g_held));
		snprintf(s, sizeof(s), "キーコード変換「%s」を追加・適用しました", (char *)nm);
		break;
	case KB_KIND_KANA: {
		INT	at = g_nkana - 2, i;		/* before the system's two */

		if ( g_nkana >= KANA_ITEMS ) return E_LIMIT;
		snprintf(rn, sizeof(rn), "_2_%d.json", f);
		for ( i = g_nkana; i > at; i-- ) g_kana[i] = g_kana[i - 1];
		if ( kb_read_kana(json, n, &g_kana[at].m) < E_OK || ( er = res_write(rn, json, n) ) < E_OK ) {
			for ( i = at; i < g_nkana; i++ ) g_kana[i] = g_kana[i + 1];
			break;
		}
		snprintf((char *)g_kana[at].name, KB_NAME, "%s", (char *)nm);
		g_kana[at].file = f;
		g_kana[at].mode = KP_M_KANA;
		g_nkana++;
		g_akana = at;
		kana_apply();
		snprintf(s, sizeof(s), "かな入力方式「%s」を追加しました", (char *)nm);
		break;
	}
	default:
		say("不明なJSONファイル形式です");
		note("dropped: not a layout");
		return E_PAR;
	}
	if ( er < E_OK || s[0] == 0 ) {
		say("JSONファイルの読み込みに失敗しました");
		return ( er < E_OK ) ? er : E_PAR;
	}
	say(s);
	{
		char	t[200];

		snprintf(t, sizeof(t), "added %s %s as %s", ( kind == KB_KIND_KEYBOARD ) ? "keyboard"
			 : ( kind == KB_KIND_KANA ) ? "kana" : "trans", (char *)nm, rn);
		note(t);
	}
	g_dirty = TRUE;
	return E_OK;
}

static void dropped( void )
{
	T_OBDROP	*d;
	UB		fb[OB_DROP_NAME];
	INT		n, k;

	if ( ( d = ui_drop(&g_ui) ) == NULL ) return;
	if ( d->n < 1 ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, "");
		return;
	}
	n = json_of(&d->v[0].target);
	if ( n <= 0 || kb_kind(g_json, n) == KB_KIND_NONE ) {
		ui_drop_answer(&g_ui, d, OB_DR_REFUSE, ( n <= 0 ) ? "JSONファイルの読み込みに失敗しました"
								 : "不明なJSONファイル形式です");
		note("dropped: not a layout");
		return;
	}
	ui_drop_answer(&g_ui, d, OB_DR_ACCEPT, "");
	/* the name the link shows, without .json, when the layout gives none */
	snprintf((char *)fb, sizeof(fb), "%s", (char *)d->v[0].name);
	k = (INT)strlen((char *)fb);
	if ( k > 5 && strcmp((char *)fb + k - 5, ".json") == 0 ) fb[k - 5] = 0;
	(void)layout_add(g_json, n, fb);
}

/* ---------------------------------------------------------------- the person */

static void finish( void )
{
	g_closing = TRUE;
}

static void command( CONST char *code, INT index )
{
	if ( strcmp(code, "close") == 0 ) finish();
	else if ( strcmp(code, "fullscreen") == 0 ) fullscreen();
	else if ( strcmp(code, "refresh") == 0 ) {
		note("refreshed");
		g_dirty = TRUE;
	} else if ( strcmp(code, "bgcolour") == 0 ) paper_ask();
	else if ( strcmp(code, "kb.item") == 0 ) choose_keyboard(index);
	else if ( strcmp(code, "kana.item") == 0 ) choose_kana(index);
	else if ( strcmp(code, "trans.item") == 0 ) choose_trans(index - 1);
	else if ( strcmp(code, "start") == 0 ) {
		if ( !g_e.practising ) practice_start();
	} else if ( strcmp(code, "reset") == 0 ) {
		practice_stop(NULL);
		load_question(0);
		note("reset");
		say("リセットしました。Enterキーで練習開始");
	} else if ( strcmp(code, "lookahead") == 0 ) ask_look();
	g_dirty = TRUE;
}

static void menu( CONST T_OBNTM *m )
{
	static UB	kb[KB_ITEMS][WM_LABEL_MAX], ka[KANA_ITEMS][WM_LABEL_MAX], tr[TRANS_ITEMS + 1][WM_LABEL_MAX];
	TS_UUID		def, d;
	T_MNSEL		s;
	ID		mid;
	INT		i;
	BOOL		chosen;

	if ( ui_uuid(SYSDEF_MENU_KANATYPE, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "fullscreen", g_full ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "start", g_e.practising ? MN_GREY : 0);
	for ( i = 0; i < g_nkb; i++ ) snprintf((char *)kb[i], WM_LABEL_MAX, "%s", (char *)g_kb[i].name);
	for ( i = 0; i < g_nkana; i++ ) snprintf((char *)ka[i], WM_LABEL_MAX, "%s", (char *)g_kana[i].name);
	snprintf((char *)tr[0], WM_LABEL_MAX, "なし");
	for ( i = 0; i < g_ntr; i++ ) snprintf((char *)tr[i + 1], WM_LABEL_MAX, "%s", (char *)g_tr[i].name);
	if ( ui_uuid(SYSDEF_MENU_KT_KEYBOARD, &d) >= E_OK ) {
		(void)mn_set_lst(mid, &d, "kb.item", (CONST UB *)kb, WM_LABEL_MAX, g_nkb);
		(void)mn_chg_idx(mid, &d, "kb.item", g_akb, MN_TICK);
	}
	if ( ui_uuid(SYSDEF_MENU_KT_KANA, &d) >= E_OK ) {
		(void)mn_set_lst(mid, &d, "kana.item", (CONST UB *)ka, WM_LABEL_MAX, g_nkana);
		(void)mn_chg_idx(mid, &d, "kana.item", g_akana, MN_TICK);
	}
	if ( ui_uuid(SYSDEF_MENU_KT_TRANS, &d) >= E_OK ) {
		(void)mn_set_lst(mid, &d, "trans.item", (CONST UB *)tr, WM_LABEL_MAX, g_ntr + 1);
		(void)mn_chg_idx(mid, &d, "trans.item", g_atr + 1, MN_TICK);
	}
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &s) >= E_OK );
	(void)mn_del_men(mid);
	memset(&g_held, 0, sizeof(g_held));
	if ( chosen ) command((CONST char *)s.code, s.index);
}

static void flash( UINT u, INT kind )
{
	if ( u >= 256 ) return;
	g_flash_kind[u] = (UB)kind;
	g_flash_until[u] = now_ms() + ( ( kind == 2 ) ? FLASH_WRONG_MS : FLASH_RIGHT_MS );
}

static BOOL modifier_only( UINT u )
{
	return (BOOL)( u == KB_K_LCTRL || u == KB_K_RCTRL || u == KB_K_LALT || u == KB_K_RALT
		    || u == KB_K_LMETA || u == KB_K_RMETA || u == KB_K_EISU );
}

static void key_up( UINT u )
{
	KB_DEL(&g_held, u);
	if ( u == KB_K_LSHIFT ) g_lsh = FALSE;
	if ( u == KB_K_RSHIFT ) g_rsh = FALSE;
	g_dirty = TRUE;
}

static void key( CONST T_OBNTM *m )
{
	UINT	code = m->code, u;
	KPRES	r;
	char	s[120], ch[32];
	BOOL	again;
	INT	k;
	UD	now;

	if ( code == K_RBRACKET_ALT ) code = K_RBRACKET;
	u = kb_trans(( g_atr >= 0 ) ? &g_tr[g_atr].t : NULL, code);
	if ( ( m->mods & OB_KEY_UP ) != 0 ) {
		key_up(u);
		return;
	}
	if ( u == KB_K_LSHIFT || u == KB_K_RSHIFT ) {
		if ( u == KB_K_LSHIFT ) g_lsh = TRUE;
		else g_rsh = TRUE;
		KB_ADD(&g_held, u);
		g_dirty = TRUE;
		return;
	}
	if ( modifier_only(u) ) return;
	if ( wm_key_cmd(code) == K_L && ( m->mods & M_CTRL ) != 0 && !g_e.practising ) {
		fullscreen();
		return;
	}
	if ( wm_key_cmd(code) == K_E && ( m->mods & M_CTRL ) != 0 ) {
		finish();
		return;
	}
	if ( ( code == KB_K_ENTER || code == K_KPENTER ) && !g_e.practising ) {
		practice_start();
		return;
	}
	if ( code == KB_K_ESC ) {
		if ( g_e.practising ) practice_stop("中断しました。Enterキーで再開");
		return;
	}
	/* a key held repeats: only its first press is a key typed */
	again = (BOOL)( KB_IN(&g_held, u) );
	KB_ADD(&g_held, u);
	g_dirty = TRUE;
	if ( !g_e.practising || again ) return;

	now = now_ms();
	if ( g_kana[g_akana].mode == KP_M_ROMAJI && g_atr < 0 ) {
		/* romaji: the letter the system's layout makes of the key */
		UB	c = wm_key_char(code, 0);
		UINT	ru;

		if ( c >= 'A' && c <= 'Z' ) c = (UB)( c - 'A' + 'a' );
		ru = kb_roma_key((char)c);
		if ( ru != 0 ) u = ru;
	}
	kp_key(&g_e, u, (BOOL)( g_lsh || ( m->mods & HID_MOD_LSHIFT ) != 0 ),
	       (BOOL)( g_rsh || ( m->mods & HID_MOD_RSHIFT ) != 0 ), &g_held, &g_kb[g_akb].l, now, &r);
	kp_score(&g_e, now, &g_score);
	if ( r.has_char ) (void)kp_utf8(r.cp, KB_CP, (UB *)ch, sizeof(ch));
	else strcpy(ch, "-");
	if ( r.partial ) {
		snprintf(s, sizeof(s), "key %s wait %s", key_name(u), ch);
		note(s);
		return;
	}
	if ( r.correct == 1 ) {
		if ( r.ncombo > 0 ) {
			for ( k = 0; k < r.ncombo; k++ ) flash(r.combo[k], 1);
		} else {
			flash(u, 1);
		}
	} else if ( r.correct == 0 && ( r.has_char || r.combo_wrong ) ) {
		flash(u, 2);
	}
	snprintf(s, sizeof(s), "key %s %s %s", key_name(u), ( r.correct == 1 ) ? "right" : ( r.correct == 0
		 && ( r.has_char || r.combo_wrong ) ) ? "wrong" : "none", ch);
	note(s);
	if ( r.question_done ) {
		INT	cur, tot;

		kp_progress(&g_e, &cur, &tot);
		snprintf(s, sizeof(s), "question %d/%d", cur, tot);
		note(s);
	}
	if ( r.finished ) practice_done();
}

static void event( CONST T_OBNTM *m )
{
	if ( !same_uuid(&m->uuid, &g_win) ) return;
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		finish();
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(m);
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

/* Whether a flash ended: the key is drawn again without it */
/* ms until the first key lit up is to go out, TMO_FEVR when none is lit */
static TMO flash_left( UD now )
{
	UD	first = 0;
	INT	i;

	for ( i = 0; i < 256; i++ ) {
		if ( g_flash_until[i] != 0 && ( first == 0 || g_flash_until[i] < first ) ) first = g_flash_until[i];
	}
	if ( first == 0 ) return TMO_FEVR;
	return ( first > now ) ? (TMO)( first - now ) : 0;
}

static BOOL flash_ended( UD now )
{
	INT	i;
	BOOL	ended = FALSE;

	for ( i = 0; i < 256; i++ ) {
		if ( g_flash_until[i] != 0 && g_flash_until[i] <= now ) {
			g_flash_until[i] = 0;
			ended = TRUE;
		}
	}
	return ended;
}

/* ---------------------------------------------------------------- the selftest */

static INT st_fail;

static void st_check( BOOL ok, CONST char *what )
{
	char	s[160];

	if ( ok ) return;
	st_fail++;
	snprintf(s, sizeof(s), "selftest: failed: %s", what);
	note(s);
}

/* Every question of the object typed with a kana layout, each key the one shown next */
static BOOL st_type_all( INT mode, CONST KBKANAMAP *map, CONST KBLAYOUT *kb )
{
	KBSET	held;
	INT	guard = 0, i;

	kp_set_map(&g_e, mode, map);
	g_e.pick_count = KP_Q_MAX;
	kp_start(&g_e, NULL, 1000);
	while ( g_e.practising && guard++ < 20000 ) {
		KPNEXT	nx[2];
		KPRES	r;

		if ( kp_next(&g_e, 1, nx) < 1 ) return FALSE;
		memset(&held, 0, sizeof(held));
		if ( nx[0].nkey > 0 ) {
			for ( i = 0; i < nx[0].nkey; i++ ) {
				KB_ADD(&held, nx[0].keys[i]);
				kp_key(&g_e, nx[0].keys[i], FALSE, FALSE, &held, kb, 1000 + guard * 100, &r);
			}
		} else {
			kp_key(&g_e, nx[0].usage, nx[0].shift == KP_SH_LEFT, nx[0].shift == KP_SH_RIGHT, &held, kb,
			       1000 + guard * 100, &r);
		}
		if ( r.correct != 1 && !r.partial ) return FALSE;	/* a kana awaiting its mark goes on */
	}
	return (BOOL)( g_e.completed && g_e.wrong == 0 && g_e.cq + 1 == g_e.nq );
}

static INT selftest( void )
{
	TS_UUID	tpl, cp;
	INT	n, i;
	char	s[160];

	note("selftest");
	st_check(ui_uuid(TEMPLATE, &tpl) >= E_OK, "the template's name");
	g_obj = tpl;
	g_xtad = rec_read(&tpl, 0, &g_xtadn);
	st_check(g_xtad != NULL, "the template's record 0");
	if ( g_xtad == NULL ) return 1;
	kp_init(&g_e);
	n = kp_questions(&g_e, g_xtad, g_xtadn);
	st_check(n == 30, "30 questions");

	/* a copy of the template: what is read, and what is written there and read again */
	if ( ob_cpy_obj(&tpl, NULL, &cp) < E_OK ) {
		st_check(FALSE, "a copy of the template");
		return 1;
	}
	g_obj = cp;
	layouts_read();
	snprintf(s, sizeof(s), "selftest: keyboards %d kana %d trans %d", g_nkb, g_nkana, g_ntr);
	note(s);
	st_check(g_nkb == 2 && g_nkana == 4 && g_ntr == 1, "two keyboards, two kana layouts and the system's two, a translation");
	st_check(strcmp((char *)g_kb[0].name, "日本語109キー") == 0, "the first keyboard's name");
	st_check(strcmp((char *)g_kana[1].name, "薙刀式v17") == 0, "the second kana layout's name");
	st_check(kb_trans(&g_tr[0].t, 0x33) == 0x14, "Dvorak: ; is reported for q");
	for ( i = 0; i < g_nkana; i++ ) {
		snprintf(s, sizeof(s), "every question typed with %s", (char *)g_kana[i].name);
		st_check(st_type_all(g_kana[i].mode, &g_kana[i].m, &g_kb[i % g_nkb].l), s);
	}

	/* a keyboard added: written as JSON.stringify writes it, and read back as the third */
	n = (INT)strlen(kb_jp109_json);
	st_check(layout_add((CONST UB *)kb_jp109_json, n, (CONST UB *)"x") >= E_OK, "a keyboard added");
	{
		ID	k = ob_opn_obj(&cp, OB_OP_R);
		INT	m = ( k > 0 ) ? res_read(k, "_1_2.json") : -1;

		st_check(m == n && memcmp(g_json, kb_jp109_json, (size_t)n) == 0, "_1_2.json as it was written");
		if ( k > 0 ) ob_cls_obj(k);
	}
	layouts_read();
	st_check(g_nkb == 3, "three keyboards read again");
	(void)ob_del_obj(&cp);
	free(g_xtad);
	snprintf(s, sizeof(s), "selftest %s (%d failed)", ( st_fail == 0 ) ? "ok" : "failed", st_fail);
	note(s);
	return ( st_fail == 0 ) ? 0 : 1;
}

/* ---------------------------------------------------------------- start and end */

int main( void )
{
	T_OBCRE	c;
	T_OBNTM	m;
	T_OBREF	ref;
	SZ	asz = 0, an;
	char	title[OB_NAME_MAX + 8], s[200], arg[16];

	memset(arg, 0, sizeof(arg));
	an = ts_get_arg(arg, sizeof(arg) - 1);
	if ( an >= 8 && memcmp(arg, "selftest", 8) == 0 ) return selftest();
	if ( ts_get_arg(&g_obj, sizeof(g_obj)) < (SZ)sizeof(g_obj) || ob_ref_obj(&g_obj, &ref) < E_OK ) {
		note("no object to open");
		(void)wm_msg_put("かな入力練習は、かな入力練習の実身を開いて使います");
		return 1;
	}
	snprintf(title, sizeof(title), "%s", (char *)ref.name);
	snprintf((char *)g_name, sizeof(g_name), "%s", (char *)ref.name);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	g_port = ( ob_cre_obj(&c, &g_ch) >= E_OK ) ? ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	if ( g_port <= 0 ) {
		note("no channel");
		return 2;
	}
	kp_init(&g_e);
	g_xtad = rec_read(&g_obj, 0, &g_xtadn);
	(void)kp_questions(&g_e, g_xtad, g_xtadn);
	layouts_read();
	kana_apply();
	paper_read();
	if ( win_open(title) < E_OK ) {
		note("no window");
		ob_cls_obj(g_port);
		(void)ob_del_obj(&g_ch);
		return 3;
	}
	load_question(0);
	snprintf(s, sizeof(s), "ready %s questions %d keyboards %d kana %d trans %d", title, g_e.nq, g_nkb, g_nkana, g_ntr);
	note(s);
	say(( g_xtad != NULL ) ? "Enterキーで練習開始" : "データの読み込みに失敗しました");
	paint();

	while ( !g_closing ) {
		for ( TMO tmo = flash_left(now_ms());
		      !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK; tmo = TMO_POL ) {
			event(&m);
		}
		if ( !g_closing && flash_ended(now_ms()) ) g_dirty = TRUE;
		if ( g_dirty && !g_closing ) paint();
	}
	if ( g_kw > 0 ) ob_cls_obj(g_kw);
	(void)ob_del_obj(&g_win);
	ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	free(g_xtad);
	(void)wm_msg_put(NULL);
	note("closed");
	return 0;
}
