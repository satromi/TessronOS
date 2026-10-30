/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cs_main.c
 *	文字検索: the accessory that finds a character and types it
 *	(design 11.19)
 *
 *	One window: above, the panel of the data box (SYSDEF_BOX_CHARSRCH)
 *	with four tags -- コード一覧, 漢字検索, 記号検索, 文字情報 -- and
 *	the boxes and buttons of each; below it, a strip the program draws
 *	itself: the characters of a row of a character set, or those a
 *	search found, in a grid of cells, each in the colour of its set, or
 *	the information of one character. The window is the panel's size
 *	with the strip added under it, so that the panel, which draws its
 *	whole rectangle each time a part changes, never paints over it.
 *
 *	A character is chosen with a press (the arrow keys move the
 *	choice); a double press, or the 文字情報 tag, shows its information.
 *	入力 types it into the window the keys went to before this one: the
 *	desktop is asked (DT_RQ_TYPE), and puts it into a text at its caret
 *	or into the box of a panel that has the keys. When there is no such
 *	window the character is put in the tray instead, to be pasted.
 *
 *	The dictionary (SYSDEF_DICT_CHAR) is read whole as the program
 *	starts. Started with an argument -- "kanji:<conditions>",
 *	"symbol:<name>", "code:<set>:<code>" or "info:<character>" -- it
 *	shows that first, which the tests use.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/tray.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "cs_dic.h"

#define TICK_MS		20

/* The panel's parts */
#define P_TAGS		100
#define P_SET		11		/* コード一覧 */
#define P_CODE		12
#define B_SHOW		13
#define B_CPREV		14
#define B_CNEXT		15
#define B_CTYPE		16
#define P_CSTAT		17
#define P_KQ		21		/* 漢字検索 */
#define B_KFIND		22
#define B_ELEMS		23
#define P_KJIS		24
#define P_KHOJO		25
#define P_KGB		26
#define P_KKS		27
#define B_KPREV		28
#define B_KNEXT		29
#define P_KSTAT		30
#define B_KTYPE		31
#define P_SNAME		41		/* 記号検索 */
#define P_SPART		42
#define B_SFIND		43
#define P_SSCRIPT	44
#define P_SCLASS	45
#define B_SPREV		46
#define B_SNEXT		47
#define P_SSTAT		48
#define B_STYPE		49
#define P_ICHAR		51		/* 文字情報 */
#define B_ILOOK		52
#define B_ITYPE		53
#define B_ITRAY		54
#define B_ICODE		55
#define P_ISTAT		56

#define PN_MAIN		1
#define PN_WARN		10

#define SH_CODE		1
#define SH_KANJI	2
#define SH_SYMBOL	3
#define SH_INFO		4

/* The strip under the panel */
#define WIN_W		640
#define PANEL_H		136
#define AREA_H		400
#define FOOT_H		26
#define LABEL_W		64
#define MARGIN		8

#define C_GROUND	0x00FFFFFFU
#define C_FRAME		0x00808080U
#define C_EMPTY		0x00F0F0F0U
#define C_PICK		0x00E00000U
#define C_TEXT		0x00000000U
#define C_DIM		0x00808080U
#define C_FOOT		0x00F4F4EEU

#define RES_MAX		16384
#define DBL_NS		( 450ULL * 1000000ULL )	/* a second press within this is a double one */

LOCAL UI	ui;
LOCAL BOOL	closing;
LOCAL INT	g_gid = -1;
LOCAL CDIC	dic;
LOCAL BOOL	have_dic;

/* The size cells are drawn at: 標準, 3/2倍, 2倍, 3倍, 4倍 */
LOCAL CONST INT	zoom_x2[] = { 2, 3, 4, 6, 8 };
LOCAL INT	zoom = 0;

/* What the strip shows */
#define V_ROW		1		/* a row of a set */
#define V_FOUND		2		/* what a search found */
#define V_ELEMS		3		/* the elements to choose from */
#define V_INFO		4
LOCAL INT	view = V_ROW;

/* The code list: the set, its plane and row, the first cell shown */
LOCAL INT	cl_set = CS_JIS, cl_plane = 1, cl_row = 16, cl_top = 0;

/* What was found, for each of the two searches */
typedef struct {
	UINT	*cp;
	INT	n, total, top, pick;
	BOOL	done;			/* searched at least once */
} FOUND;

LOCAL FOUND	fk, fs;

/* The character chosen, whichever sheet chose it; 0 none */
LOCAL UINT	sel_cp = 0;
LOCAL UINT	sel_cp2 = 0;		/* a mark that goes with it, for a place that holds two */
LOCAL INT	cl_pick = -1;		/* the cell chosen in the row */

/* The cells drawn last, for a press to find */
#define HIT_MAX		600
typedef struct {
	T_DPRECT	r;
	UINT		cp, cp2;
	INT		index;		/* in what it shows */
} HIT;
LOCAL HIT	hit[HIT_MAX];
LOCAL INT	nhit;

/* A double press: the cell and when */
LOCAL INT	last_index = -1;
LOCAL UD	last_when;

/* ---------------------------------------------------------------- words to the person */

LOCAL void say( CONST char *fmt, ... )
{
	char	s[256];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	tm_putstring((CONST UB *)s);
}

LOCAL void warn( CONST char *a, CONST char *b )
{
	CONST char	*l[3] = { a, ( b != NULL ) ? b : " ", " " };

	(void)ui_ask(&ui, PN_WARN, l, 3);
}

LOCAL INT status_part( void )
{
	switch ( ui_sheet(&ui) ) {
	case SH_KANJI:	return P_KSTAT;
	case SH_SYMBOL:	return P_SSTAT;
	case SH_INFO:	return P_ISTAT;
	default:	return P_CSTAT;
	}
}

LOCAL void status( CONST char *s )
{
	ui_set_text(&ui, status_part(), ( s[0] != 0 ) ? s : " ");
}

LOCAL INT one( UINT cp, char *s )
{
	INT	n = cs_utf8_put(cp, s);

	s[n] = 0;
	return n;
}

/* ---------------------------------------------------------------- the dictionary */

LOCAL void dic_read( void )
{
	TS_UUID	u;
	T_OBREC	r[4];
	ID	k;
	INT	cnt = 0, i;
	SZ	size = 0, asz = 0;
	UB	*b;

	have_dic = FALSE;
	if ( ui_uuid(SYSDEF_DICT_CHAR, &u) < E_OK ) return;
	k = ob_opn_obj(&u, OB_OP_R);
	if ( k < E_OK ) return;
	if ( ob_lst_rec(k, r, 4, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( r[i].recno == 1 ) size = (SZ)r[i].size;
		}
	}
	b = ( size > 0 ) ? malloc((size_t)size) : NULL;
	if ( b != NULL && ob_rea_rec(k, 1, 0, b, size, &asz) >= E_OK && cd_open(&dic, b, asz) >= E_OK ) {
		have_dic = TRUE;
	} else if ( b != NULL ) {
		free(b);
	}
	ob_cls_obj(k);
}

LOCAL CONST char *kind_name( INT kind )
{
	switch ( kind ) {
	case CS_K_JIS12:	return "日本基本（JIS第1/第2水準）";
	case CS_K_JIS3:		return "日本基本（JIS第3水準）";
	case CS_K_JIS4:		return "日本基本（JIS第4水準）";
	case CS_K_HOJO:		return "日本補助（JIS補助漢字）";
	case CS_K_GB:		return "中国（GB 2312）";
	case CS_K_KS:		return "韓国（KS X 1001）";
	case CS_K_UCS:		return "各国（Unicode）";
	default:		return "その他";
	}
}

LOCAL CONST char *set_name( INT set )
{
	switch ( set ) {
	case CS_JIS:	return "日本基本";
	case CS_HOJO:	return "日本補助";
	case CS_GB:	return "中国";
	case CS_KS:	return "韓国";
	case CS_UCS:	return "各国";
	case CS_TRON:	return "TRON";
	default:	return "";
	}
}

/* Each set's colour: black, 水色, 青緑, 青, 赤, 紫, 海松色; grey for the rest */
LOCAL UINT colour_of( INT kind )
{
	switch ( kind ) {
	case CS_K_JIS12:	return 0x00000000U;
	case CS_K_JIS3:		return 0x0000A0E0U;
	case CS_K_JIS4:		return 0x00008B8BU;
	case CS_K_HOJO:		return 0x000020D0U;
	case CS_K_GB:		return 0x00D00000U;
	case CS_K_KS:		return 0x008000A0U;
	case CS_K_UCS:		return 0x00606A2CU;
	default:		return 0x00909090U;
	}
}

/* Whether the system's face has a glyph for the character, as far as the dictionary says */
LOCAL BOOL drawable( UINT cp )
{
	CONST CDENT	*e;

	if ( !have_dic || ( dic.src & CD_SRC_FONT ) == 0 ) return TRUE;
	e = cd_find(&dic, cp);
	return (BOOL)( e == NULL || ( e->flags & CD_F_GLYPH ) != 0 );
}

/* ---------------------------------------------------------------- the strip */

LOCAL INT cell_px( void )
{
	return 32 * zoom_x2[zoom] / 2;
}

LOCAL void text_at( INT x, INT y, CONST char *s, UINT col, INT px )
{
	(void)dp_text(g_gid, x, y, (CONST UB *)s, col, px);
}

/* s cut so that it fits w at px, at a letter's edge */
LOCAL void text_fit( INT x, INT y, INT w, CONST char *s, UINT col, INT px )
{
	char	t[256];
	INT	n = (INT)strlen(s);

	if ( n >= (INT)sizeof(t) ) n = (INT)sizeof(t) - 1;
	memcpy(t, s, (size_t)n);
	t[n] = 0;
	while ( n > 0 && dp_text_width((CONST UB *)t, px) > w ) {
		do n--; while ( n > 0 && ( t[n] & 0xC0 ) == 0x80 );
		t[n] = 0;
	}
	text_at(x, y, t, col, px);
}

/* One cell: its character in its colour, framed when chosen, the code when there is no glyph */
LOCAL void draw_cell( CONST T_DPRECT *r, UINT cp, UINT cp2, INT kind, BOOL picked, INT index )
{
	INT	c = r->right - r->left, px = c * 3 / 4;
	char	s[16];

	dp_fill_rect(g_gid, r, ( cp == 0 ) ? C_EMPTY : C_GROUND);
	dp_frame_rect(g_gid, r, 0x00D8D8D8U, 1);
	if ( cp != 0 ) {
		if ( drawable(cp) ) {
			INT	n = one(cp, s), w;

			if ( cp2 != 0 ) one(cp2, s + n);
			w = dp_text_width((CONST UB *)s, px);
			text_at(r->left + ( c - w ) / 2, r->top + c * 4 / 5, s, colour_of(kind), px);
		} else {
			INT	w;

			snprintf(s, sizeof(s), "%lX", (unsigned long)cp);
			w = dp_text_width((CONST UB *)s, c / 4 + 2);
			text_at(r->left + ( c - w ) / 2, r->top + c * 3 / 5, s, C_DIM, c / 4 + 2);
		}
	}
	if ( picked ) {
		T_DPRECT	f = *r;

		dp_frame_rect(g_gid, &f, C_PICK, 2);
	}
	if ( nhit < HIT_MAX ) {
		hit[nhit].r = *r;
		hit[nhit].cp = cp;
		hit[nhit].cp2 = cp2;
		hit[nhit].index = index;
		nhit++;
	}
}

/* The line at the foot: the character chosen and its codes */
LOCAL void draw_foot( void )
{
	T_DPRECT	f;
	char		s[200], t[64], c[16];
	CSPLACE		p[1];

	f.left = 0;
	f.top = PANEL_H + AREA_H - FOOT_H;
	f.right = WIN_W;
	f.bottom = PANEL_H + AREA_H;
	dp_fill_rect(g_gid, &f, C_FOOT);
	dp_line(g_gid, 0, f.top, WIN_W, f.top, C_FRAME);
	if ( sel_cp == 0 ) return;
	one(sel_cp, c);
	if ( sel_cp2 != 0 ) one(sel_cp2, c + strlen(c));
	s[0] = 0;
	if ( have_dic && cs_places(&dic, sel_cp, p, 1) > 0 ) {
		cs_place_text(&p[0], t);
		snprintf(s, sizeof(s), "%s %s", set_name(p[0].set), t);
	}
	cs_tron_text(sel_cp, t);
	snprintf(s + strlen(s), sizeof(s) - strlen(s), "　%s　U+%04lX", t, (unsigned long)sel_cp);
	if ( drawable(sel_cp) ) text_at(MARGIN, f.bottom - 6, c, C_TEXT, 18);
	text_fit(MARGIN + 32, f.bottom - 8, WIN_W - MARGIN * 2 - 32, s, C_TEXT, 13);
}

LOCAL void strip_clear( void )
{
	T_DPRECT	a;

	a.left = 0;
	a.top = PANEL_H;
	a.right = WIN_W;
	a.bottom = PANEL_H + AREA_H;
	dp_fill_rect(g_gid, &a, C_GROUND);
	dp_line(g_gid, 0, PANEL_H, WIN_W, PANEL_H, C_FRAME);
	nhit = 0;
}

/* The columns and rows of cells the strip holds, with or without the labels' column */
LOCAL void grid( BOOL labels, INT *p_cols, INT *p_rows )
{
	INT	c = cell_px();

	*p_cols = ( WIN_W - MARGIN * 2 - ( labels ? LABEL_W : 0 ) ) / c;
	*p_rows = ( AREA_H - FOOT_H - MARGIN * 2 ) / c;
	if ( *p_cols < 1 ) *p_cols = 1;
	if ( *p_rows < 1 ) *p_rows = 1;
}

/* The label of a cell of the row, at the start of a line of the grid */
LOCAL void cell_label( INT cell, char *s )
{
	CSPLACE	p;

	memset(&p, 0, sizeof(p));
	p.set = cl_set;
	p.plane = cl_plane;
	p.row = cl_row;
	switch ( cl_set ) {
	case CS_UCS:
		p.cell = cl_row * 256 + cell;
		break;
	case CS_TRON:
		p.tplane = cl_plane;
		p.tcode = ( (UINT)cl_row << 8 ) | (UINT)( ( cell < 94 ) ? 0x21 + cell : 0x80 + cell - 94 );
		break;
	default:
		p.cell = cell + 1;
		break;
	}
	cs_place_text(&p, s);
}

LOCAL void draw_row( void )
{
	INT	cols, rows, c = cell_px(), n = cs_row_cells(cl_set, cl_plane, cl_row), i, k;
	INT	x0 = MARGIN + LABEL_W, y0 = PANEL_H + MARGIN;

	grid(TRUE, &cols, &rows);
	for ( k = 0; k < cols * rows && cl_top + k < n; k++ ) {
		T_DPRECT	r;
		UINT		u[2];
		INT		kind, m;

		i = cl_top + k;
		m = cs_cell(&dic, cl_set, cl_plane, cl_row, ( cl_set == CS_UCS || cl_set == CS_TRON ) ? i : i + 1, u, &kind);
		r.left = x0 + ( k % cols ) * c;
		r.top = y0 + ( k / cols ) * c;
		r.right = r.left + c;
		r.bottom = r.top + c;
		if ( k % cols == 0 ) {
			char	s[24];

			cell_label(i, s);
			text_at(MARGIN, r.top + c / 2 + 5, s, C_DIM, 12);
		}
		draw_cell(&r, ( m > 0 ) ? u[0] : 0, ( m == 2 ) ? u[1] : 0, kind, (BOOL)( i == cl_pick ), i);
	}
}

LOCAL void draw_found( CONST FOUND *f )
{
	INT	cols, rows, c = cell_px(), k;
	INT	x0 = MARGIN, y0 = PANEL_H + MARGIN;

	grid(FALSE, &cols, &rows);
	for ( k = 0; k < cols * rows && f->top + k < f->n; k++ ) {
		T_DPRECT	r;
		UINT		cp = f->cp[f->top + k];

		r.left = x0 + ( k % cols ) * c;
		r.top = y0 + ( k / cols ) * c;
		r.right = r.left + c;
		r.bottom = r.top + c;
		draw_cell(&r, cp, 0, cs_kind_of(&dic, cp), (BOOL)( f->top + k == f->pick ), f->top + k);
	}
	if ( f->n == 0 ) {
		text_at(MARGIN * 2, PANEL_H + 40, f->done ? "該当する文字はありません"
				: "条件を入れて〈検索〉を押してください", C_DIM, 14);
	}
}

/* The elements to choose from, by their strokes, each count marked before its first */
LOCAL void draw_elems( void )
{
	INT	cols, rows, c = 26, k = 0, i, last = 0;	/* 24 by 13: the 253 elements and their counts */
	INT	x0 = MARGIN, y0 = PANEL_H + MARGIN;

	cols = ( WIN_W - MARGIN * 2 ) / c;
	rows = ( AREA_H - FOOT_H - MARGIN * 2 ) / c;
	for ( i = 0; i < dic.nelem && k < cols * rows; i++ ) {
		T_DPRECT	r;
		UINT		shown;
		INT		strokes;
		UINT		cp = cd_elem(&dic, i, &shown, &strokes);

		if ( strokes != last ) {
			char	s[8];

			last = strokes;
			r.left = x0 + ( k % cols ) * c;
			r.top = y0 + ( k / cols ) * c;
			r.right = r.left + c;
			r.bottom = r.top + c;
			dp_fill_rect(g_gid, &r, 0x00E0E8F0U);
			snprintf(s, sizeof(s), "%d", strokes);
			text_at(r.left + 6, r.top + 19, s, 0x00406080U, 12);
			k++;
			if ( k >= cols * rows ) break;
		}
		r.left = x0 + ( k % cols ) * c;
		r.top = y0 + ( k / cols ) * c;
		r.right = r.left + c;
		r.bottom = r.top + c;
		draw_cell(&r, shown, 0, CS_K_JIS12, FALSE, i);
		hit[nhit - 1].cp = cp;		/* a press gives the element, whatever stands for it */
		k++;
	}
}

/* ---------------------------------------------------------------- 文字情報 */

LOCAL CONST char *var_name( INT kind )
{
	switch ( kind ) {
	case CD_V_ITAI:		return "異体字";
	case CD_V_SIMP:		return "中国漢字";
	case CD_V_TRAD:		return "台湾漢字";
	case CD_V_OLD:		return "旧字体";
	case CD_V_NEW:		return "新字体";
	case CD_V_UPPER:	return "大文字";
	case CD_V_LOWER:	return "小文字";
	default:		return "関連字";
	}
}

/* Hiragana as katakana, for 音読み */
LOCAL void to_kata( CONST UB *s, INT n, char *o, INT max )
{
	INT	i = 0, k = 0, m;
	UINT	c;

	while ( i < n && k + 5 < max ) {
		m = cs_utf8_next(s + i, &c);
		if ( m <= 0 ) break;
		if ( c >= 0x3041 && c <= 0x3096 ) c += 0x60;
		k += cs_utf8_put(c, o + k);
		i += m;
	}
	o[k] = 0;
}

LOCAL void copy_field( CONST UB *p, INT n, char *o, INT max )
{
	if ( n < 0 ) n = 0;
	if ( n >= max ) n = max - 1;
	memcpy(o, p, (size_t)n);
	o[n] = 0;
}

LOCAL INT info_line( INT y, CONST char *s )
{
	text_fit(MARGIN + 4, y, 470, s, C_TEXT, 14);
	return y + 22;
}

LOCAL void draw_info( void )
{
	CONST CDENT	*e = have_dic ? cd_find(&dic, sel_cp) : NULL;
	CSPLACE		pl[8];
	CONST UB	*p;
	char		s[320], t[80], c[16];
	INT		np, y = PANEL_H + 24, len, i;
	T_DPRECT	big;

	big.left = 496;
	big.top = PANEL_H + 10;
	big.right = 628;
	big.bottom = big.top + 132;
	dp_frame_rect(g_gid, &big, C_FRAME, 1);
	if ( sel_cp == 0 ) {
		text_at(MARGIN * 2, PANEL_H + 40, "文字を選ぶか、上の欄に入れて〈調べる〉を押してください", C_DIM, 14);
		return;
	}
	one(sel_cp, c);
	if ( drawable(sel_cp) ) {
		INT	w = dp_text_width((CONST UB *)c, 104);

		text_at(big.left + ( 132 - w ) / 2, big.top + 112, c, colour_of(cs_kind_of(&dic, sel_cp)), 104);
	}
	np = have_dic ? cs_places(&dic, sel_cp, pl, 8) : 0;
	if ( np > 0 ) {
		cs_place_text(&pl[0], t);
		snprintf(s, sizeof(s), "文字種：%s　%s", kind_name(pl[0].kind), t);
	} else {
		snprintf(s, sizeof(s), "文字種：各国（Unicode）");
	}
	y = info_line(y, s);
	cs_tron_text(sel_cp, t);
	{
		INT	tp = 0;
		UINT	tc = 0;

		(void)cs_tron(sel_cp, &tp, &tc);
		snprintf(s, sizeof(s), "TRONコード：第%d面 %04X　テキスト形式：%s", tp, tc, t);
	}
	y = info_line(y, s);
	snprintf(s, sizeof(s), "U：%04lX", (unsigned long)sel_cp);
	for ( i = 0; i < np; i++ ) {
		UINT	sj = cs_sjis(&pl[i]), eu = cs_euc(&pl[i]);

		if ( sj != 0 && strstr(s, "S：") == NULL ) snprintf(s + strlen(s), sizeof(s) - strlen(s), "　S：%04X", sj);
		if ( eu != 0 && strstr(s, "E：") == NULL ) snprintf(s + strlen(s), sizeof(s) - strlen(s), "　E：%X", eu);
	}
	len = cd_field(&dic, e, CD_T_BIG5, &p);
	if ( len == 2 ) snprintf(s + strlen(s), sizeof(s) - strlen(s), "　B：%02X%02X", p[0], p[1]);
	y = info_line(y, s);
	if ( e != NULL && ( e->flags & CD_F_KANJI ) != 0 ) {
		char	r[8];

		cd_radical(e->radical, r);
		snprintf(s, sizeof(s), "部首：%s（%d）　画数：%d", r, e->radical, e->strokes);
		y = info_line(y, s);
		len = cd_field(&dic, e, CD_T_ON, &p);
		if ( len > 0 ) {
			to_kata(p, len, t, sizeof(t));
			snprintf(s, sizeof(s), "音：%s", t);
			y = info_line(y, s);
		}
		len = cd_field(&dic, e, CD_T_KUN, &p);
		if ( len > 0 ) {
			char	k[200];

			copy_field(p, len, k, sizeof(k));
			snprintf(s, sizeof(s), "訓：%s", k);
			y = info_line(y, s);
		}
		s[0] = 0;
		if ( e->flags & CD_F_JOYO ) strcat(s, "常用漢字　");
		if ( e->grade >= 1 && e->grade <= 6 ) snprintf(s + strlen(s), sizeof(s) - strlen(s), "教育漢字（%d年）　", e->grade);
		if ( e->flags & CD_F_JINMEI ) strcat(s, "人名用漢字　");
		if ( s[0] != 0 ) {
			snprintf(t, sizeof(t), "属性：");
			memmove(s + strlen(t), s, strlen(s) + 1);
			memcpy(s, t, strlen(t));
			y = info_line(y, s);
		}
	} else if ( e != NULL ) {
		len = cd_field(&dic, e, CD_T_NAME, &p);
		if ( len > 0 ) {
			char	k[160];

			copy_field(p, len, k, sizeof(k));
			snprintf(s, sizeof(s), "名前：%s", k);
			y = info_line(y, s);
		}
	}
	/* 同定字: the places of the same character in the other sets */
	if ( np > 1 ) {
		strcpy(s, "同定字：");
		for ( i = 1; i < np; i++ ) {
			cs_place_text(&pl[i], t);
			snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s%s %s", ( i > 1 ) ? "、" : "", set_name(pl[i].set), t);
		}
		y = info_line(y, s);
	}
	/* 関連字: cells, each with what it is under it */
	len = cd_field(&dic, e, CD_T_VARS, &p);
	if ( len >= 4 ) {
		INT	x = MARGIN + 4 + 80, cc = 40;

		text_at(MARGIN + 4, y + 22, "関連字：", C_TEXT, 14);
		for ( i = 0; i + 4 <= len && x + cc <= WIN_W - MARGIN; i += 4 ) {
			UINT		v = (UINT)p[i] | ( (UINT)p[i + 1] << 8 ) | ( (UINT)p[i + 2] << 16 ) | ( (UINT)p[i + 3] << 24 );
			UINT		vc = v & 0xFFFFFF;
			T_DPRECT	r;
			CONST char	*nm = var_name((INT)( v >> 24 ));
			INT		w = dp_text_width((CONST UB *)nm, 10);

			r.left = x;
			r.top = y;
			r.right = x + cc;
			r.bottom = y + cc;
			draw_cell(&r, vc, 0, cs_kind_of(&dic, vc), FALSE, -1 - i / 4);
			text_at(x + ( cc - w ) / 2, y + cc + 12, nm, C_DIM, 10);
			x += cc + 12;
		}
	}
}

/* ---------------------------------------------------------------- drawing it all */

LOCAL void draw( void )
{
	T_DPRECT	a;

	if ( g_gid < 0 ) return;
	strip_clear();
	switch ( view ) {
	case V_ROW:	draw_row();		break;
	case V_FOUND:	draw_found(( ui_sheet(&ui) == SH_SYMBOL ) ? &fs : &fk);	break;
	case V_ELEMS:	draw_elems();		break;
	case V_INFO:	draw_info();		break;
	default:	break;
	}
	if ( view != V_INFO ) draw_foot();
	a.left = 0;
	a.top = PANEL_H;
	a.right = WIN_W;
	a.bottom = PANEL_H + AREA_H;
	(void)wm_obj_flush(ui.kw, &a);
}

/* ---------------------------------------------------------------- the character chosen */

LOCAL void pick( UINT cp, UINT cp2 )
{
	char	c[16];

	sel_cp = cp;
	sel_cp2 = cp2;
	if ( cp != 0 ) {
		one(cp, c);
		say("charsearch: picked %s U+%04lX\n", c, (unsigned long)cp);
	}
}

/* The text of the character chosen, with its mark; empty when none */
LOCAL INT chosen( char *s )
{
	INT	n;

	s[0] = 0;
	if ( sel_cp == 0 ) return 0;
	n = one(sel_cp, s);
	if ( sel_cp2 != 0 ) n += one(sel_cp2, s + n);
	return n;
}

/* Letters put in the tray as a text */
LOCAL BOOL to_tray( CONST char *s, CONST char *name )
{
	char	frag[512];
	T_TRREC	r;
	INT	i, k;

	k = snprintf(frag, sizeof(frag), "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>");
	for ( i = 0; s[i] != 0 && k < (INT)sizeof(frag) - 40; i++ ) {
		if ( s[i] == '<' ) k += snprintf(frag + k, sizeof(frag) - k, "&lt;");
		else if ( s[i] == '>' ) k += snprintf(frag + k, sizeof(frag) - k, "&gt;");
		else if ( s[i] == '&' ) k += snprintf(frag + k, sizeof(frag) - k, "&amp;");
		else frag[k++] = s[i];
	}
	if ( k > (INT)sizeof(frag) - 40 ) k = (INT)sizeof(frag) - 40;	/* the end of the text has its room */
	k += snprintf(frag + k, sizeof(frag) - k, "</p></document></tad>");
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = frag;
	r.size = k;
	return (BOOL)( tr_psh_dat(&r, 1, (CONST UB *)name) >= E_OK );
}

/* The desktop asked to type s into the window the keys went to before */
LOCAL ER ask_type( CONST char *s, UINT *p_how )
{
	static UINT	seq;
	T_DTREQ		rq;
	T_DTANS		an;
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
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_TYPE;
	rq.seq = ++seq;
	rq.reply = reply;
	rq.win = ui.win;
	strncpy((char *)rq.name, s, sizeof(rq.name) - 1);
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	/* the answer waited for on its channel, at most 3000 ms */
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, &an, sizeof(an), &asz, 3000);
		if ( er >= E_OK && asz == (SZ)sizeof(an) && an.seq == rq.seq ) {
			er = an.er;
			*p_how = an.how;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* 入力: the letters typed where the keys went before, or else put in the tray */
LOCAL void type_out( CONST char *s )
{
	UINT	how = 0;
	ER	er;
	char	m[96];

	if ( s[0] == 0 ) {
		warn("入力する文字を選んでください。", NULL);
		return;
	}
	er = ask_type(s, &how);
	if ( er >= E_OK ) {
		snprintf(m, sizeof(m), "「%s」を入力しました", s);
		status(m);
		say("charsearch: typed %s %s\n", s, ( how == DT_HOW_BOX ) ? "box" : "text");
		return;
	}
	if ( to_tray(s, s) ) {
		snprintf(m, sizeof(m), "入力先がないので「%s」をトレーに入れました", s);
		status(m);
		say("charsearch: tray %s (%d)\n", s, (INT)er);
	} else {
		warn("文字を入力できませんでした。", NULL);
	}
}

LOCAL void type_chosen( void )
{
	char	s[16];

	(void)chosen(s);
	type_out(s);
}

LOCAL void type_code( void )
{
	char	s[24];

	if ( sel_cp == 0 ) {
		warn("入力する文字を選んでください。", NULL);
		return;
	}
	cs_tron_text(sel_cp, s);
	type_out(s);
}

/* ---------------------------------------------------------------- コード一覧 */

LOCAL CONST INT	set_of_choice[] = { CS_JIS, CS_HOJO, CS_GB, CS_KS, CS_UCS, CS_TRON };

/* Whether a row of the set has any character */
LOCAL BOOL row_has( INT set, INT plane, INT row )
{
	INT	n = cs_row_cells(set, plane, row), i, kind;
	UINT	u[2];

	for ( i = 0; i < n; i++ ) {
		if ( cs_cell(&dic, set, plane, row, ( set == CS_UCS || set == CS_TRON ) ? i : i + 1, u, &kind) > 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL void row_said( void )
{
	char	s[120];

	if ( cl_set == CS_JIS ) {
		snprintf(s, sizeof(s), "日本基本　第%d面　%d区", cl_plane, cl_row);
	} else if ( cl_set == CS_UCS ) {
		snprintf(s, sizeof(s), "各国　U+%02X00〜U+%02XFF", cl_row, cl_row);
	} else if ( cl_set == CS_TRON ) {
		snprintf(s, sizeof(s), "TRONコード　第%d面　%02X21〜%02XFD", cl_plane, cl_row, cl_row);
	} else {
		snprintf(s, sizeof(s), "%s　%d区", set_name(cl_set), cl_row);
	}
	ui_set_text(&ui, P_CSTAT, s);
	say("charsearch: row %s %d-%d\n", set_name(cl_set), cl_plane, cl_row);
}

/* The row of a set shown, a cell of it chosen (-1 none) */
LOCAL void row_show( INT set, INT plane, INT row, INT cell )
{
	INT	cols, rows;

	cl_set = set;
	cl_plane = plane;
	cl_row = row;
	cl_pick = cell;
	cl_top = 0;
	grid(TRUE, &cols, &rows);
	if ( cell >= cols * rows ) cl_top = ( cell / cols - rows + 1 ) * cols;
	if ( cell >= 0 ) {
		UINT	u[2];
		INT	kind, m = cs_cell(&dic, set, plane, row, ( set == CS_UCS || set == CS_TRON ) ? cell : cell + 1, u, &kind);

		if ( m > 0 ) pick(u[0], ( m == 2 ) ? u[1] : 0);
	}
	view = V_ROW;
	ui_set_value(&ui, P_SET, ( set >= CS_JIS && set <= CS_TRON ) ? set : 1);
	row_said();
	draw();
}

/* 表示: the row of the code in the box */
LOCAL void code_show( void )
{
	char	s[48];
	INT	n, set = set_of_choice[( ui_value(&ui, P_SET) >= 1 && ui_value(&ui, P_SET) <= 6 ) ? ui_value(&ui, P_SET) - 1 : 0];
	INT	plane = 1, row = 1, cell = 1;

	n = ui_text(&ui, P_CODE, s, sizeof(s));
	if ( n < 0 ) n = 0;
	s[( n < (INT)sizeof(s) ) ? n : (INT)sizeof(s) - 1] = 0;
	if ( s[0] == 0 ) {
		/* no code: the set's first row that has characters */
		INT	first, rows = cs_rows(set, plane, &first), r;

		for ( r = first; r < first + rows && !row_has(set, set == CS_TRON ? 1 : ( set == CS_JIS ? 1 : 0 ), r); r++ ) ;
		row_show(set, ( set == CS_JIS || set == CS_TRON ) ? 1 : 0, r, -1);
		return;
	}
	if ( !cs_parse_code(set, s, &plane, &row, &cell) ) {
		warn("コードが読めません。", ( set == CS_JIS ) ? "例：16-01、1-16-01（面-区-点）"
				      : ( set == CS_UCS ) ? "例：4E9C、U+4E9C"
				      : ( set == CS_TRON ) ? "例：&T213021;、1 3021（面 コード）" : "例：16-01（区-点）");
		return;
	}
	row_show(set, plane, row, ( set == CS_UCS || set == CS_TRON ) ? cell : cell - 1);
}

/* 前頁 and 次頁 of the code list: the rest of the row, then the row before or after with characters */
LOCAL void row_step( INT d )
{
	INT	cols, rows, page, n = cs_row_cells(cl_set, cl_plane, cl_row), first, nr, r;

	grid(TRUE, &cols, &rows);
	page = cols * rows;
	if ( d > 0 && cl_top + page < n ) {
		cl_top += page;
		draw();
		return;
	}
	if ( d < 0 && cl_top > 0 ) {
		cl_top = ( cl_top > page ) ? cl_top - page : 0;
		draw();
		return;
	}
	nr = cs_rows(cl_set, cl_plane, &first);
	for ( r = cl_row + d; r >= first && r < first + nr; r += d ) {
		if ( row_has(cl_set, cl_plane, r) ) {
			row_show(cl_set, cl_plane, r, -1);
			return;
		}
	}
	status("ここが端です");
}

/* The code list where the character chosen is */
LOCAL void row_of_chosen( void )
{
	CSPLACE	p[1];

	if ( sel_cp == 0 || !have_dic || cs_places(&dic, sel_cp, p, 1) <= 0 ) {
		row_show(cl_set, cl_plane, cl_row, cl_pick);
		return;
	}
	switch ( p[0].set ) {
	case CS_UCS:
		row_show(CS_UCS, 0, p[0].cell >> 8, p[0].cell & 0xFF);
		break;
	case CS_JIS:
		row_show(CS_JIS, p[0].plane, p[0].row, p[0].cell - 1);
		break;
	default:
		row_show(p[0].set, 0, p[0].row, p[0].cell - 1);
		break;
	}
}

/* ---------------------------------------------------------------- the searches */

LOCAL UINT sets_chosen( void )
{
	UINT	s = 0;

	if ( ui_value(&ui, P_KJIS) ) s |= CS_SET_JIS;
	if ( ui_value(&ui, P_KHOJO) ) s |= CS_SET_HOJO;
	if ( ui_value(&ui, P_KGB) ) s |= CS_SET_GB;
	if ( ui_value(&ui, P_KKS) ) s |= CS_SET_KS;
	return ( s != 0 ) ? s : CS_SET_JIS;
}

LOCAL void found_said( CONST FOUND *f, INT part )
{
	char	s[96];
	INT	cols, rows;

	grid(FALSE, &cols, &rows);
	if ( f->n == 0 ) {
		snprintf(s, sizeof(s), "該当なし");
	} else {
		snprintf(s, sizeof(s), "%d 字（%d〜%d）%s", f->total, f->top + 1,
			 ( f->top + cols * rows < f->n ) ? f->top + cols * rows : f->n,
			 ( f->total > f->n ) ? "　多すぎるので一部だけ示します" : "");
	}
	ui_set_text(&ui, part, s);
}

LOCAL void found_pick_first( FOUND *f )
{
	f->top = 0;
	f->pick = ( f->n > 0 ) ? 0 : -1;
	if ( f->n > 0 ) pick(f->cp[0], 0);
}

LOCAL void kanji_find_q( CONST char *q )
{
	INT	n;

	if ( !have_dic ) {
		warn("文字の辞書がありません。", NULL);
		return;
	}
	n = cs_kanji(&dic, q, sets_chosen(), fk.cp, RES_MAX, &fk.total);
	if ( n == CS_E_EMPTY ) {
		warn("検索条件を入れてください。", "例：口 =9、木×2、倹-イ、こう");
		return;
	}
	if ( n == CS_E_SYNTAX ) {
		warn("検索条件が読めません。", "例：口 9、=8+2、虎 号 .");
		return;
	}
	fk.n = n;
	fk.done = TRUE;
	found_pick_first(&fk);
	view = V_FOUND;
	say("charsearch: found kanji %d for \"%s\"\n", fk.total, q);
	found_said(&fk, P_KSTAT);
	draw();
}

LOCAL void kanji_find( void )
{
	char	q[256];
	INT	n = ui_text(&ui, P_KQ, q, sizeof(q));

	if ( n < 0 ) n = 0;
	q[( n < (INT)sizeof(q) ) ? n : (INT)sizeof(q) - 1] = 0;
	kanji_find_q(q);
}

LOCAL void symbol_find_q( CONST char *name, CONST char *parts )
{
	INT	v[2] = { 0, 0 }, n;

	if ( !have_dic ) {
		warn("文字の辞書がありません。", NULL);
		return;
	}
	(void)ui_fields(&ui, P_SSCRIPT, v, 1);
	(void)ui_fields(&ui, P_SCLASS, v + 1, 1);
	n = cs_symbol(&dic, name, parts, v[0], v[1], CS_SET_ALL, fs.cp, RES_MAX, &fs.total);
	if ( n == CS_E_EMPTY ) {
		warn("名称、部品、字種、分類のどれかを指定してください。", "例：ハート、KA、チルド U、部品 ◎ 1");
		return;
	}
	fs.n = n;
	fs.done = TRUE;
	found_pick_first(&fs);
	view = V_FOUND;
	say("charsearch: found symbol %d for \"%s\" \"%s\" %d %d\n", fs.total, name, parts, v[0], v[1]);
	found_said(&fs, P_SSTAT);
	draw();
}

LOCAL void symbol_find( void )
{
	char	a[160], b[96];
	INT	n = ui_text(&ui, P_SNAME, a, sizeof(a)), m = ui_text(&ui, P_SPART, b, sizeof(b));

	if ( n < 0 ) n = 0;
	if ( m < 0 ) m = 0;
	a[( n < (INT)sizeof(a) ) ? n : (INT)sizeof(a) - 1] = 0;
	b[( m < (INT)sizeof(b) ) ? m : (INT)sizeof(b) - 1] = 0;
	symbol_find_q(a, b);
}

LOCAL void found_step( FOUND *f, INT part, INT d )
{
	INT	cols, rows, page;

	grid(FALSE, &cols, &rows);
	page = cols * rows;
	if ( d > 0 && f->top + page < f->n ) f->top += page;
	else if ( d < 0 && f->top > 0 ) f->top = ( f->top > page ) ? f->top - page : 0;
	else {
		status("ここが端です");
		return;
	}
	found_said(f, part);
	draw();
}

/* An element chosen from the list, added to the conditions */
LOCAL void elem_add( UINT cp )
{
	char	q[256], c[8];
	INT	n = ui_text(&ui, P_KQ, q, sizeof(q));

	if ( n < 0 ) n = 0;
	q[( n < (INT)sizeof(q) ) ? n : (INT)sizeof(q) - 1] = 0;
	n = (INT)strlen(q);
	one(cp, c);
	if ( n + (INT)strlen(c) + 2 >= (INT)sizeof(q) ) return;
	if ( n > 0 && q[n - 1] != ' ' ) strcat(q, " ");
	strcat(q, c);
	ui_set_text(&ui, P_KQ, q);
	say("charsearch: element %s\n", c);
}

/* ---------------------------------------------------------------- 文字情報 */

LOCAL void info_show( void )
{
	char	c[16];

	view = V_INFO;
	if ( sel_cp != 0 ) {
		one(sel_cp, c);
		ui_set_text(&ui, P_ICHAR, c);
		say("charsearch: info %s U+%04lX\n", c, (unsigned long)sel_cp);
	}
	ui_set_text(&ui, P_ISTAT, " ");
	draw();
}

LOCAL void info_look( void )
{
	char	s[32];
	INT	n = ui_text(&ui, P_ICHAR, s, sizeof(s));
	UINT	cp = 0;

	if ( n < 0 ) n = 0;
	s[( n < (INT)sizeof(s) ) ? n : (INT)sizeof(s) - 1] = 0;
	if ( cs_utf8_next((CONST UB *)s, &cp) <= 0 || cp == 0 ) {
		warn("調べる文字を入れてください。", NULL);
		return;
	}
	pick(cp, 0);
	info_show();
}

/* The first letter of the text in hand in the tray */
LOCAL void info_from_tray( void )
{
	T_TRINF	inf;
	UB	b[2048];
	SZ	asz = 0;
	INT	i = 0;
	UINT	cp = 0;

	if ( tr_ref_rec(TR_HAND, 0, &inf) < E_OK || inf.kind != TR_TAD || inf.size <= 0
	  || tr_rea_rec(TR_HAND, 0, b, sizeof(b) - 1, &asz) < E_OK ) {
		warn("トレーに文字がありません。", NULL);
		return;
	}
	b[asz] = 0;
	/* past the tags and what is between the tags of the head */
	while ( b[i] != 0 ) {
		if ( b[i] == '<' ) {
			while ( b[i] != 0 && b[i] != '>' ) i++;
			if ( b[i] == '>' ) i++;
			continue;
		}
		if ( b[i] == '\n' || b[i] == '\r' || b[i] == ' ' ) {
			i++;
			continue;
		}
		if ( b[i] == '&' ) {
			if ( strncmp((char *)b + i, "&lt;", 4) == 0 ) cp = '<';
			else if ( strncmp((char *)b + i, "&gt;", 4) == 0 ) cp = '>';
			else if ( strncmp((char *)b + i, "&amp;", 5) == 0 ) cp = '&';
			else cp = '&';
			break;
		}
		(void)cs_utf8_next(b + i, &cp);
		break;
	}
	if ( cp == 0 ) {
		warn("トレーに文字がありません。", NULL);
		return;
	}
	pick(cp, 0);
	ui_show_sheet(&ui, SH_INFO);
	info_show();
}

/* ---------------------------------------------------------------- トレーへ */

LOCAL void copy_chosen( void )
{
	char	s[16];

	if ( chosen(s) == 0 ) {
		warn("複写する文字を選んでください。", NULL);
		return;
	}
	if ( to_tray(s, s) ) {
		status("トレーに複写しました");
		say("charsearch: copied %s\n", s);
	}
}

/* What was found put in the tray as a text: what the strip shows, or all of it */
LOCAL void save_found( BOOL all )
{
	FOUND	*f = ( ui_sheet(&ui) == SH_SYMBOL ) ? &fs : &fk;
	INT	cols, rows, from, to, i, k = 0;
	char	*s;

	if ( view != V_FOUND || f->n == 0 ) {
		warn("保存する検索結果がありません。", NULL);
		return;
	}
	grid(FALSE, &cols, &rows);
	from = all ? 0 : f->top;
	to = all ? f->n : ( ( f->top + cols * rows < f->n ) ? f->top + cols * rows : f->n );
	s = malloc((size_t)( to - from ) * 4 + 1);
	if ( s == NULL ) return;
	for ( i = from; i < to; i++ ) k += cs_utf8_put(f->cp[i], s + k);
	s[k] = 0;
	{
		/* a text of the characters, a paragraph of them */
		char	*frag = malloc((size_t)k * 2 + 128);
		T_TRREC	r;
		INT	m = 0;

		if ( frag != NULL ) {
			m = snprintf(frag, (size_t)k * 2 + 128,
				     "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>%s</p></document></tad>", s);
			memset(&r, 0, sizeof(r));
			r.kind = TR_TAD;
			r.data = frag;
			r.size = m;
			if ( tr_psh_dat(&r, 1, (CONST UB *)"検索結果") >= E_OK ) {
				char	t[64];

				snprintf(t, sizeof(t), "%d 字をトレーに入れました", to - from);
				status(t);
				say("charsearch: saved %d\n", to - from);
			}
			free(frag);
		}
	}
	free(s);
}

/* ---------------------------------------------------------------- the menu */

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

LOCAL CONST char *zoom_code[] = { "z1", "z15", "z2", "z3", "z4" };

LOCAL void set_zoom( INT z )
{
	zoom = z;
	if ( view == V_FOUND ) {
		FOUND	*f = ( ui_sheet(&ui) == SH_SYMBOL ) ? &fs : &fk;
		INT	cols, rows;

		grid(FALSE, &cols, &rows);
		if ( f->pick >= 0 ) f->top = ( f->pick / ( cols * rows ) ) * cols * rows;
		found_said(f, ( ui_sheet(&ui) == SH_SYMBOL ) ? P_SSTAT : P_KSTAT);
	} else if ( view == V_ROW ) {
		INT	cols, rows;

		grid(TRUE, &cols, &rows);
		cl_top = ( cl_pick >= 0 ) ? ( cl_pick / ( cols * rows ) ) * cols * rows : 0;
	}
	say("charsearch: zoom %s\n", zoom_code[z]);
	draw();
}

LOCAL void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen_one;
	INT	z;

	if ( ui_uuid(SYSDEF_MENU_CHARSRCH, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	for ( z = 0; z < 5; z++ ) (void)mn_chg_atr(mid, NULL, zoom_code[z], ( z == zoom ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "save1", ( view == V_FOUND ) ? 0 : MN_GREY);
	(void)mn_chg_atr(mid, NULL, "saveall", ( view == V_FOUND ) ? 0 : MN_GREY);
	chosen_one = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen_one ) {
		draw();
		return;
	}
	if ( is_code(&sel, "close") ) {
		closing = TRUE;
		return;
	}
	if ( is_code(&sel, "input") ) type_chosen();
	else if ( is_code(&sel, "inputcode") ) type_code();
	else if ( is_code(&sel, "copy") ) copy_chosen();
	else if ( is_code(&sel, "trayinfo") ) info_from_tray();
	else if ( is_code(&sel, "save1") ) save_found(FALSE);
	else if ( is_code(&sel, "saveall") ) save_found(TRUE);
	else {
		for ( z = 0; z < 5; z++ ) {
			if ( is_code(&sel, zoom_code[z]) ) set_zoom(z);
		}
	}
	draw();
}

/* ---------------------------------------------------------------- presses and keys */

/* The sheet shown changed: what the strip shows follows it, with the character chosen */
LOCAL void sheet( INT tag )
{
	switch ( tag ) {
	case SH_CODE:
		row_of_chosen();
		break;
	case SH_KANJI:
		view = V_FOUND;
		ui_set_text(&ui, B_ELEMS, "部品一覧");
		if ( fk.done ) found_said(&fk, P_KSTAT);
		break;
	case SH_SYMBOL:
		view = V_FOUND;
		if ( fs.done ) found_said(&fs, P_SSTAT);
		break;
	case SH_INFO:
		info_show();
		break;
	default:
		break;
	}
	draw();
}

LOCAL void pressed_cell( CONST HIT *h, UD when )
{
	BOOL	twice = (BOOL)( h->index == last_index && when - last_when < DBL_NS );

	last_index = h->index;
	last_when = when;
	if ( view == V_ELEMS ) {
		elem_add(h->cp);
		return;
	}
	if ( h->cp == 0 ) return;
	if ( view == V_INFO ) {
		/* a related character: its own information, on a double press */
		if ( twice ) {
			pick(h->cp, 0);
			info_show();
		}
		return;
	}
	pick(h->cp, h->cp2);
	if ( view == V_ROW ) cl_pick = h->index;
	else if ( view == V_FOUND ) ( ( ui_sheet(&ui) == SH_SYMBOL ) ? &fs : &fk )->pick = h->index;
	if ( twice ) {
		ui_show_sheet(&ui, SH_INFO);
		info_show();
		return;
	}
	draw();
}

LOCAL void press( CONST T_OBNTM *m )
{
	INT	i;

	if ( m->y < PANEL_H ) return;
	for ( i = 0; i < nhit; i++ ) {
		CONST HIT	*h = &hit[i];

		if ( m->x >= h->r.left && m->x < h->r.right && m->y >= h->r.top && m->y < h->r.bottom ) {
			pressed_cell(h, m->when);
			return;
		}
	}
}

/* An arrow key: the choice moved along what the strip shows */
LOCAL void arrow( INT d )
{
	INT	cols, rows;

	if ( view == V_ROW ) {
		INT	n = cs_row_cells(cl_set, cl_plane, cl_row), at;

		grid(TRUE, &cols, &rows);
		if ( d == 2 ) d = cols;
		if ( d == -2 ) d = -cols;
		at = ( cl_pick < 0 ) ? 0 : cl_pick + d;
		if ( at < 0 || at >= n ) return;
		cl_pick = at;
		if ( at < cl_top ) cl_top = ( at / cols ) * cols;
		if ( at >= cl_top + cols * rows ) cl_top = ( at / cols - rows + 1 ) * cols;
		{
			UINT	u[2];
			INT	kind, k = cs_cell(&dic, cl_set, cl_plane, cl_row,
						  ( cl_set == CS_UCS || cl_set == CS_TRON ) ? at : at + 1, u, &kind);

			if ( k > 0 ) pick(u[0], ( k == 2 ) ? u[1] : 0);
			else pick(0, 0);
		}
	} else if ( view == V_FOUND ) {
		FOUND	*f = ( ui_sheet(&ui) == SH_SYMBOL ) ? &fs : &fk;
		INT	at;

		grid(FALSE, &cols, &rows);
		if ( d == 2 ) d = cols;
		if ( d == -2 ) d = -cols;
		at = f->pick + d;
		if ( f->n == 0 || at < 0 || at >= f->n ) return;
		f->pick = at;
		if ( at < f->top ) f->top = ( at / cols ) * cols;
		if ( at >= f->top + cols * rows ) f->top = ( at / cols - rows + 1 ) * cols;
		pick(f->cp[at], 0);
		found_said(f, ( ui_sheet(&ui) == SH_SYMBOL ) ? P_SSTAT : P_KSTAT);
	} else {
		return;
	}
	draw();
}

#define MOD_CTRL	( 0x01 | 0x10 )

LOCAL void typed( CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);

	if ( ( m->mods & MOD_CTRL ) != 0 ) {
		if ( c == 'e' || c == 'E' ) closing = TRUE;
		else if ( c == 'i' || c == 'I' ) type_chosen();
		else if ( c == 't' || c == 'T' ) type_code();
		else if ( c == 'c' || c == 'C' ) copy_chosen();
		else if ( c >= '1' && c <= '4' ) {
			ui_show_sheet(&ui, c - '0');
			sheet(c - '0');
		}
		return;
	}
	switch ( m->code ) {
	case 0x4F:	arrow(1);	break;		/* → */
	case 0x50:	arrow(-1);	break;		/* ← */
	case 0x51:	arrow(2);	break;		/* ↓ */
	case 0x52:	arrow(-2);	break;		/* ↑ */
	case 0x4B:					/* Page Up */
	case 0x4E:					/* Page Down */
		if ( view == V_ROW ) row_step(( m->code == 0x4B ) ? -1 : 1);
		else if ( view == V_FOUND ) {
			BOOL	sym = (BOOL)( ui_sheet(&ui) == SH_SYMBOL );

			found_step(sym ? &fs : &fk, sym ? P_SSTAT : P_KSTAT, ( m->code == 0x4B ) ? -1 : 1);
		}
		break;
	default:
		break;
	}
}

LOCAL void pressed( INT id )
{
	switch ( id ) {
	case B_SHOW:	code_show();				break;
	case B_CPREV:	row_step(-1);				break;
	case B_CNEXT:	row_step(1);				break;
	case B_KFIND:	kanji_find();				break;
	case B_ELEMS:
		view = ( view == V_ELEMS ) ? V_FOUND : V_ELEMS;
		ui_set_text(&ui, B_ELEMS, ( view == V_ELEMS ) ? "検索結果" : "部品一覧");
		draw();
		break;
	case B_KPREV:	found_step(&fk, P_KSTAT, -1);		break;
	case B_KNEXT:	found_step(&fk, P_KSTAT, 1);		break;
	case B_SFIND:	symbol_find();				break;
	case B_SPREV:	found_step(&fs, P_SSTAT, -1);		break;
	case B_SNEXT:	found_step(&fs, P_SSTAT, 1);		break;
	case B_ILOOK:	info_look();				break;
	case B_ITRAY:	info_from_tray();			break;
	case B_CTYPE:
	case B_KTYPE:
	case B_STYPE:
	case B_ITYPE:	type_chosen();				break;
	case B_ICODE:	type_code();				break;
	default:	break;
	}
}

/* ---------------------------------------------------------------- the window */

/* The window made taller than the panel by the strip */
LOCAL void open_strip( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.bottom += AREA_H;
		(void)ob_wri_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	g_gid = wm_obj_gid(ui.kw);
}

/* What the argument asks to be shown first */
LOCAL void start_with( CONST char *a )
{
	if ( strncmp(a, "kanji:", 6) == 0 ) {
		ui_show_sheet(&ui, SH_KANJI);
		sheet(SH_KANJI);
		if ( a[6] != 0 ) {
			ui_set_text(&ui, P_KQ, a + 6);
			kanji_find_q(a + 6);
		}
	} else if ( strncmp(a, "symbol:", 7) == 0 ) {
		ui_show_sheet(&ui, SH_SYMBOL);
		ui_set_text(&ui, P_SNAME, a + 7);
		symbol_find_q(a + 7, "");
	} else if ( strncmp(a, "code:", 5) == 0 && a[5] >= '1' && a[5] <= '6' && a[6] == ':' ) {
		ui_set_value(&ui, P_SET, a[5] - '0');
		ui_set_text(&ui, P_CODE, a + 7);
		code_show();
	} else if ( strncmp(a, "info:", 5) == 0 ) {
		UINT	cp = 0;

		if ( cs_utf8_next((CONST UB *)a + 5, &cp) > 0 ) {
			pick(cp, 0);
			ui_show_sheet(&ui, SH_INFO);
			info_show();
		}
	}
}

int main( void )
{
	T_OBNTM	m;
	SZ	argn;
	char	arg[256];
	INT	id, ev;

	memset(arg, 0, sizeof(arg));
	argn = ts_get_arg(arg, sizeof(arg) - 1);
	fk.cp = malloc(sizeof(UINT) * RES_MAX);
	fs.cp = malloc(sizeof(UINT) * RES_MAX);
	if ( fk.cp == NULL || fs.cp == NULL ) return 1;
	fk.pick = fs.pick = -1;
	dic_read();

	ui_icon(SYSDEF_PROG_CHARSRCH);
	if ( ui_open(&ui, SYSDEF_BOX_CHARSRCH, PN_MAIN, 180, 70) < E_OK ) {
		tm_putstring((CONST UB *)"charsearch: no window\n");
		return 1;
	}
	open_strip();
	if ( !have_dic ) {
		warn("文字の辞書が見つかりません。", "コード一覧だけを使えます。");
	}
	ui_set_value(&ui, P_KJIS, 1);
	row_show(CS_JIS, 1, 16, 0);
	say("charsearch: ready %d %x\n", have_dic ? dic.n : 0, have_dic ? dic.src : 0);
	if ( argn > 0 ) start_with(arg);

	while ( !closing ) {
		for ( TMO tmo = TMO_FEVR; !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			if ( m.event == OB_E_PRESS && m.recno < 0 && m.code == 0 ) {
				press(&m);
				continue;
			}
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				closing = TRUE;
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_KEY:
				typed(&m);
				break;
			case UI_EV_SHEET:
				sheet(id);
				break;
			case UI_EV_BUTTON:
				pressed(id);
				break;
			default:
				break;
			}
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"charsearch: closed\n");
	return 0;
}
