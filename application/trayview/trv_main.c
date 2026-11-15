/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	trv_main.c
 *	トレー: the accessory that shows everything in the tray, running as
 *	a process (docs/tessronos/11-desktop.md 11.27)
 *
 *	The window's work area is the panel of the data box (SYSDEF_BOX_TRAYV):
 *	tags standing one under another down its left side (down="true"), a
 *	tag for each of the tray's ten levels, those that hold nothing greyed
 *	and named by the number alone, the others by the level's number and
 *	the kind of what it holds: 文章, 図形, 仮身, 付せん (links to programs:
 *	実行機能付せん), 画像 (pictures alone) or その他 (records of real
 *	objects). The sheet beside the tags says the kind and how many bytes
 *	the set is, and under that the set is drawn by this program: a
 *	document laid out to the sheet's width, a figure from its corner,
 *	pictures one under another. The window's bar winds it. The panel and
 *	its parts are laid out again to the work area whenever the window
 *	changes its size, the tags' column as wide as their widest name; the
 *	sheet is the program's (view="true"), so that every time the panel is
 *	drawn the window is told to draw the set again.
 *
 *	詳細表示 is that. 一覧表示 puts a list in the sheet instead, a row
 *	for each level with the start of what it holds, and choosing a row
 *	shows that level as a tag does. 付せん表示 says whether a document
 *	is drawn with its 付せん -- the marks of its formats, notes, breaks
 *	and tabs -- or as its text alone.
 *
 *	A level is chosen with a press on its tag, with the arrow keys up
 *	and down, or with the digit of its number (0 for the tenth). The
 *	tray is looked at every few tenths of a second, and when anything
 *	in it changed -- a set put in by another window, taken out, the
 *	level in hand changed -- the window is built again from it, the
 *	same level kept shown while it still holds a set.
 *
 *	The accessory only looks: what it shows is drawn from the copy the
 *	tray keeps, and the tray is left as it is. Its window says it shows
 *	the program object, so that running the program again brings this
 *	window to the front. What it does is said on the console for the
 *	tests ("trayview: ...").
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/mn.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/tray.h>
#include <ts/fn.h>
#include <ts/img.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

#define KEY_1		0x1E
#define KEY_0		0x27
#define KEY_E		0x08		/* the command letter of 閉じる */
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_DOWN	0x51
#define KEY_UP		0x52

/* The parts of the data box's panel */
#define PN_MAIN		1
#define N_TAGS		1
#define N_INFO		2
#define N_LIST		3

#define TV_WIN_X	160		/* where the window opens */
#define TV_WIN_Y	100
#define TV_EDGE_IN	6		/* the sheet and its tags stand in this far from the work area's edge */
#define TV_TAG_FH	16		/* the letters of a tag */
#define TV_TAG_ROOM	17		/* a tag's width besides its name: cut head, edges, and its gap to the sheet */
#define TV_LOOK_MS	300		/* how often the tray is looked at */
#define TV_WHEEL	48
#define TV_LINE		16		/* an arrow's step in a list of pictures */
#define TV_FRAG_MAX	( 4 * 1024 * 1024 )
#define TV_GAP		8		/* between pictures, and round them */
#define TV_PAPER	0x00FFFFFFU
#define TV_EDGE		0x00808080U
#define TV_ROW_MAX	40		/* letters of a row of the list */
#define TV_TEXT		256		/* a tag's name, the words over the set, a row */

/* What a level holds */
#define K_NONE		0
#define K_DOC		1		/* 文章 */
#define K_FIG		2		/* 図形 */
#define K_LINK		3		/* 仮身 */
#define K_TAG		4		/* 付せん: links to programs */
#define K_PIC		5		/* 画像 */
#define K_OTHER		6		/* その他 */

static CONST char *CONST kind_name[] = {
	"", "文章", "図形", "仮身", "付せん", "画像", "その他"
};

static UI	ui;
static INT	g_gid = -1;
static BOOL	g_closing = FALSE;

static struct {
	INT		w, h;		/* the work area the panel was laid out for */
	INT		wide;		/* the widest tag's name it was laid out for */
	BOOL		list;		/* 一覧表示 */
	BOOL		tags;		/* 付せん表示 */
	INT		level;		/* the level shown, 0 when the tray is empty */
	INT		nlevel;
	UB		kind[TR_LEVELS + 1];
	UW		sig;		/* what the tray was, when the panel was built */
	UD		looked;		/* when, in ms */
	T_DPRECT	box;		/* where the set is drawn */
	INT		scroll_y;	/* how far the set is wound */
	INT		height;		/* all the window's bar runs over */
} st;

static void say( CONST char *s )
{
	char	line[TV_TEXT + 32];

	snprintf(line, sizeof(line), "trayview: %s\n", s);
	tm_putstring((CONST UB *)line);
}

/* ---------------------------------------------------------------- the tray */

/* Everything that says the tray changed, in one word */
static UW tray_sig( CONST T_TRSTS *s )
{
	UW	h = 2166136261U;
	INT	i, k;

	h = ( h ^ (UW)s->nlevel ) * 16777619U;
	h = ( h ^ (UW)s->hand ) * 16777619U;
	for ( i = 1; i <= TR_LEVELS; i++ ) {
		CONST T_TRSET	*t = &s->set[i];

		h = ( h ^ (UW)t->nrec ) * 16777619U;
		h = ( h ^ (UW)t->kinds ) * 16777619U;
		h = ( h ^ (UW)t->time ) * 16777619U;
		h = ( h ^ (UW)( t->time >> 32 ) ) * 16777619U;
		for ( k = 0; k < TR_NAME_MAX && t->name[k] != 0; k++ ) {
			h = ( h ^ t->name[k] ) * 16777619U;
		}
	}
	return h;
}

static T_TRSTS *tray_now( void )
{
	T_TRSTS	*s = (T_TRSTS *)malloc(sizeof(T_TRSTS));

	if ( s != NULL && tr_get_sts(s) < E_OK ) {
		free(s);
		s = NULL;
	}
	return s;
}

/* One record of a level, read whole; NULL when it is not there */
static UB *rec_bytes( INT level, INT rec, T_TRINF *inf )
{
	UB	*buf;
	SZ	asz = 0;

	if ( tr_ref_rec(level, rec, inf) < E_OK || inf->size <= 0 || inf->size > TV_FRAG_MAX ) {
		return NULL;
	}
	buf = (UB *)malloc((size_t)inf->size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tr_rea_rec(level, rec, buf, inf->size, &asz) < E_OK ) {
		free(buf);
		return NULL;
	}
	buf[asz] = 0;
	inf->size = asz;
	return buf;
}

/* The first xmlTAD fragment of a level, read; the bytes are the caller's to free with it */
static T_TAD *level_frag( INT level, INT nrec, UB **p_bytes )
{
	T_TRINF	inf;
	T_TAD	*doc = NULL;
	UB	*b;
	INT	i;

	*p_bytes = NULL;
	for ( i = 0; i < nrec; i++ ) {
		if ( tr_ref_rec(level, i, &inf) < E_OK || inf.kind != TR_TAD ) {
			continue;
		}
		b = rec_bytes(level, i, &inf);
		if ( b == NULL ) {
			continue;
		}
		if ( tad_parse(b, inf.size, NULL, &doc) >= E_OK ) {
			*p_bytes = b;
			return doc;
		}
		free(b);
	}
	return NULL;
}

/* Whether an object is a program: its metadata says what program it is */
static BOOL is_program( CONST TS_UUID *u )
{
	T_JSON	root, tf, g;
	UB	*m = NULL, id[64];
	INT	n;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);
	BOOL	is = FALSE;

	if ( k <= 0 ) {
		return FALSE;
	}
	n = ob_atr_read(k, 0, &m);
	ob_cls_obj(k);
	if ( n > 0 && js_parse(m, n, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "program", &g) >= E_OK && js_get_str(&g, "id", id, sizeof(id)) > 0 ) {
		is = TRUE;
	}
	free(m);
	return is;
}

/* Whether every link of a fragment points at a program */
static BOOL links_to_programs( CONST T_TAD *frag, INT n )
{
	T_VOBJ	v;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(frag, i, &v) < E_OK || !is_program(&v.target) ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* How many of a fragment's elements are shapes, its links not counted */
static INT frag_shapes( CONST T_TAD *frag )
{
	CONST T_TADNODE	*body = tad_body(frag), *c;
	INT		n = 0;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind == TAD_ND_ELEM && c->name != NULL && strcmp((CONST char *)c->name, "link") != 0 ) {
			n++;
		}
	}
	return n;
}

/* Whether every shape of a figure is a picture the set carries ("tray:N") */
static BOOL only_tray_pictures( CONST T_TAD *frag )
{
	CONST T_TADNODE	*body = tad_body(frag), *c;
	CONST UB	*h;
	INT		n = 0;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) {
			continue;
		}
		h = tad_attr(c, "href");
		if ( h == NULL || strncmp((CONST char *)h, "tray:", 5) != 0 ) {
			return FALSE;
		}
		n++;
	}
	return (BOOL)( n > 0 );
}

static UB kind_of( INT level, CONST T_TRSET *set )
{
	T_TAD		*frag;
	T_TADNODE	*body;
	UB		*bytes, k = K_OTHER;
	INT		links;

	if ( set->nrec <= 0 ) {
		return K_NONE;
	}
	frag = level_frag(level, set->nrec, &bytes);
	if ( frag != NULL ) {
		body = tad_body(frag);
		links = tad_lnk_count(frag);
		if ( body != NULL && body->name != NULL && body->name[0] == 'd' ) {
			k = K_DOC;
		} else if ( links > 0 && frag_shapes(frag) == 0 ) {
			k = links_to_programs(frag, links) ? K_TAG : K_LINK;
		} else {
			k = only_tray_pictures(frag) ? K_PIC : K_FIG;
		}
		tad_free(frag);
		free(bytes);
	} else if ( ( set->kinds & ( 1U << TR_PNG ) ) != 0 ) {
		k = K_PIC;
	}
	return k;
}

/* How many bytes a level's records are */
static SZ level_size( INT level, INT nrec )
{
	T_TRINF	inf;
	SZ	sz = 0;
	INT	i;

	for ( i = 0; i < nrec; i++ ) {
		if ( tr_ref_rec(level, i, &inf) >= E_OK ) {
			sz += inf.size;
		}
	}
	return sz;
}

/* ---------------------------------------------------------------- words */

static INT put( UB *b, INT n, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = (UB)*s++;
	}
	b[n] = 0;
	return n;
}

static INT put_num( UB *b, INT n, INT max, SZ v )
{
	char	t[32];
	INT	i = 0, g = 0;

	do {
		if ( g == 3 ) {
			t[i++] = ',';
			g = 0;
		}
		t[i++] = (char)( '0' + v % 10 );
		v /= 10;
		g++;
	} while ( v > 0 && i < 30 );
	while ( i > 0 && n < max - 1 ) {
		b[n++] = (UB)t[--i];
	}
	b[n] = 0;
	return n;
}

/*
 * The start of what a fragment says, as letters: the text between its
 * tags with the five escapes undone and runs of space made one, cut at a
 * letter's edge after so many letters.
 */
static INT frag_words( CONST UB *x, SZ len, UB *out, INT max, INT letters )
{
	INT	n = 0, got = 0;
	SZ	i = 0;
	BOOL	space = FALSE;

	while ( i < len && x[i] != 0 && got < letters && n < max - 5 ) {
		UB	c = x[i];

		if ( c == '<' ) {
			/* the name of the object a link stands for is not in the fragment: a break */
			while ( i < len && x[i] != '>' ) i++;
			i++;
			space = ( n > 0 );
			continue;
		}
		if ( c == '\r' || c == '\n' || c == '\t' || c == ' ' ) {
			space = ( n > 0 );
			i++;
			continue;
		}
		if ( space && n > 0 ) {
			out[n++] = ' ';
			got++;
		}
		space = FALSE;
		if ( c == '&' ) {
			CONST char	*e[5] = { "&lt;", "&gt;", "&amp;", "&quot;", "&apos;" };
			CONST char	r[5] = { '<', '>', '&', '"', '\'' };
			INT		k, j;

			for ( k = 0; k < 5; k++ ) {
				for ( j = 0; e[k][j] != 0 && i + j < len && x[i + j] == (UB)e[k][j]; j++ ) ;
				if ( e[k][j] == 0 ) {
					out[n++] = (UB)r[k];
					got++;
					i += j;
					break;
				}
			}
			if ( k < 5 ) {
				continue;
			}
		}
		/* one letter, all its bytes */
		do {
			out[n++] = x[i++];
		} while ( i < len && ( x[i] & 0xC0 ) == 0x80 && n < max - 1 );
		got++;
	}
	out[n] = 0;
	return n;
}

/* A row of the list: the level's number, its kind, and the start of it */
static void row_of( INT level, CONST T_TRSET *set, UB kind, UB *out, INT max )
{
	T_TRINF	inf;
	UB	*b;
	INT	n = 0, i;

	n = put_num(out, n, max, (SZ)level);
	n = put(out, n, max, " ");
	n = put(out, n, max, kind_name[kind]);
	n = put(out, n, max, "　");
	for ( i = 0; i < set->nrec; i++ ) {
		if ( tr_ref_rec(level, i, &inf) < E_OK || inf.kind != TR_TAD ) {
			continue;
		}
		b = rec_bytes(level, i, &inf);
		if ( b != NULL ) {
			UB	w[TV_ROW_MAX * 4 + 8];

			if ( frag_words(b, inf.size, w, sizeof(w), TV_ROW_MAX) > 0 ) {
				(void)put(out, n, max, (CONST char *)w);
				free(b);
				return;
			}
			free(b);
		}
		break;
	}
	(void)put(out, n, max, (CONST char *)set->name);	/* nothing to read: the set's name */
}

/* The words over the set: its kind, its size and whether it is in hand */
static void info_words( CONST T_TRSTS *s, UB *out, INT max )
{
	INT	n = 0;

	if ( st.level <= 0 ) {
		(void)put(out, 0, max, "トレーは空です");
		return;
	}
	n = put(out, n, max, "種別：");
	n = put(out, n, max, kind_name[st.kind[st.level]]);
	n = put(out, n, max, "　サイズ：");
	n = put_num(out, n, max, level_size(st.level, s->set[st.level].nrec));
	n = put(out, n, max, "バイト");
	if ( s->hand == st.level ) {
		n = put(out, n, max, "　（扱い中）");
	}
	(void)n;
}

/* ---------------------------------------------------------------- the panel */

static UIPART *part_of( INT num )
{
	INT	i;

	for ( i = 0; i < ui.np; i++ ) {
		if ( ui.p[i].num == num && ui.p[i].key > 0 ) return &ui.p[i];
	}
	return NULL;
}

/* A part put at r in the panel, and a panel of tags its sheet at in: what else it is kept */
static void part_place( INT num, CONST T_DPRECT *r, CONST T_DPRECT *in )
{
	UIPART		*p = part_of(num);
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( p == NULL || ob_rea_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	wp.left = r->left;
	wp.top = r->top;
	wp.right = r->right;
	wp.bottom = r->bottom;
	if ( in != NULL ) {
		wp.wleft = in->left;
		wp.wtop = in->top;
		wp.wright = in->right;
		wp.wbottom = in->bottom;
	}
	(void)ob_wri_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

/*
 * The panel laid out to a work area w by h: as large as it, the tags
 * filling it with their column as wide as the widest name, the words
 * over the sheet and the list filling the rest of it
 */
static void lay_out( INT w, INT h, INT wide )
{
	T_OBWPOS	wp;
	T_DPRECT	all, sheet, info;
	SZ		asz = 0;

	if ( ob_rea_rec(ui.pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + w;
		wp.bottom = wp.top + h;
		(void)ob_wri_rec(ui.pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	all.left = 0;
	all.top = 0;
	all.right = w;
	all.bottom = h;
	sheet.left = TV_EDGE_IN + wide + TV_TAG_ROOM;
	sheet.top = TV_EDGE_IN;
	sheet.right = w - TV_EDGE_IN;
	sheet.bottom = h - TV_EDGE_IN;
	if ( sheet.right <= sheet.left ) sheet.right = sheet.left + 1;
	if ( sheet.bottom <= sheet.top + 40 ) sheet.bottom = sheet.top + 41;
	part_place(N_TAGS, &all, &sheet);
	info.left = sheet.left + 8;
	info.top = sheet.top + 6;
	info.right = ( sheet.right - 8 > info.left ) ? sheet.right - 8 : info.left + 1;
	info.bottom = sheet.top + 28;
	part_place(N_INFO, &info, NULL);
	st.box.left = sheet.left + 6;
	st.box.top = sheet.top + 32;
	st.box.right = ( sheet.right - 6 > st.box.left ) ? sheet.right - 6 : st.box.left + 1;
	st.box.bottom = sheet.bottom - 6;
	part_place(N_LIST, &st.box, NULL);
	st.w = w;
	st.h = h;
	st.wide = wide;
}

/* The tags' state: the one shown, and those that hold nothing, which cannot be chosen */
static void tags_state( UW inact )
{
	UIPART	*p = part_of(N_TAGS);
	INT	v[2];
	SZ	asz = 0;

	if ( p == NULL ) return;
	v[0] = st.level;
	v[1] = (INT)inact;
	(void)ob_wri_rec(p->key, OB_WR_STATE, 0, v, sizeof(v), &asz);
}

/*
 * The panel built again from the tray: a tag for each of the ten levels,
 * those that hold nothing greyed and not to be chosen, the words over the
 * sheet, and in 一覧表示 the list filling the rest of it. Kept as large
 * as the work area.
 */
static void build( CONST T_TRSTS *s )
{
	UB		(*names)[24] = malloc(sizeof(*names) * TR_LEVELS);
	CONST char	*np[TR_LEVELS];
	UB		words[TV_TEXT];
	UW		inact = 0;
	ID		fid = fn_system();
	INT		i, n, w = 0, h = 0, wide = 0;

	if ( names == NULL ) {
		return;
	}
	ui_work(&ui, &w, &h);
	st.nlevel = 0;
	for ( i = 1; i <= TR_LEVELS; i++ ) {
		st.kind[i] = kind_of(i, &s->set[i]);
		if ( st.kind[i] != K_NONE ) {
			st.nlevel = i;
		}
	}
	if ( st.level > st.nlevel ) {
		st.level = st.nlevel;
	}
	if ( st.level <= 0 && st.nlevel > 0 ) {
		st.level = 1;
	}
	if ( fid > 0 ) {
		(void)fn_set_size(fid, TV_TAG_FH);
	}
	for ( i = 1; i <= TR_LEVELS; i++ ) {
		n = put_num(names[i - 1], 0, sizeof(names[0]), (SZ)i);
		if ( st.kind[i] != K_NONE ) {
			n = put(names[i - 1], n, sizeof(names[0]), " ");
			(void)put(names[i - 1], n, sizeof(names[0]), kind_name[st.kind[i]]);
		} else {
			inact |= 1U << ( i - 1 );
		}
		np[i - 1] = (CONST char *)names[i - 1];
		n = ( fid > 0 ) ? fn_width(fid, names[i - 1]) : TV_TAG_FH * 4;
		if ( n > wide ) wide = n;
	}
	if ( w > 0 && h > 0 && ( w != st.w || h != st.h || wide != st.wide ) ) {
		lay_out(w, h, wide);
	}
	ui_names(&ui, N_TAGS, np, TR_LEVELS);
	tags_state(inact);
	free(names);

	info_words(s, words, sizeof(words));
	ui_set_text(&ui, N_INFO, (CONST char *)words);

	if ( st.list && st.nlevel > 0 ) {
		UB		(*rows)[TV_TEXT] = malloc(sizeof(*rows) * (size_t)st.nlevel);
		CONST char	*rp[TR_LEVELS];

		if ( rows != NULL ) {
			for ( i = 1; i <= st.nlevel; i++ ) {
				row_of(i, &s->set[i], st.kind[i], rows[i - 1], TV_TEXT);
				rp[i - 1] = (CONST char *)rows[i - 1];
			}
			ui_names(&ui, N_LIST, rp, st.nlevel);
			free(rows);
		}
		ui_set_value(&ui, N_LIST, st.level);
		ui_show(&ui, N_LIST, TRUE);
	} else {
		ui_show(&ui, N_LIST, FALSE);
	}
	st.sig = tray_sig(s);
	st.looked = ui_now();
}

static void rebuild( void )
{
	T_TRSTS	*s = tray_now();

	if ( s == NULL ) {
		return;
	}
	build(s);
	free(s);
}

/* ---------------------------------------------------------------- drawing the set */

/* Drawing kept inside a box; FALSE when nothing of it can be seen */
static BOOL narrow( INT gid, CONST T_DPRECT *r, T_DPRECT *p_was )
{
	T_DPENV		env;
	T_DPRECT	cut;

	if ( dp_ref(gid, &env) < E_OK ) {
		return FALSE;
	}
	*p_was = env.visible;
	cut = env.visible;
	if ( cut.left < r->left )     cut.left = r->left;
	if ( cut.top < r->top )       cut.top = r->top;
	if ( cut.right > r->right )   cut.right = r->right;
	if ( cut.bottom > r->bottom ) cut.bottom = r->bottom;
	if ( cut.right <= cut.left || cut.bottom <= cut.top ) {
		return FALSE;
	}
	dp_set_visible(gid, &cut);
	return TRUE;
}

/* The pictures of a level one under another; how tall they came to */
static INT draw_pictures( INT gid, INT level, INT nrec, CONST T_DPRECT *r, INT scroll )
{
	T_TRINF	inf;
	UB	*b;
	UW	*px;
	INT	i, pw, ph, y = TV_GAP;

	for ( i = 0; i < nrec; i++ ) {
		if ( tr_ref_rec(level, i, &inf) < E_OK || inf.kind != TR_PNG ) {
			continue;
		}
		b = rec_bytes(level, i, &inf);
		if ( b == NULL ) {
			continue;
		}
		px = NULL;
		if ( img_png_decode(b, inf.size, &px, &pw, &ph) >= E_OK && px != NULL ) {
			(void)dp_put_argb(gid, r->left + TV_GAP, r->top + y - scroll, px, pw, pw, ph, IMG_CLEAR);
			y += ph + TV_GAP;
			free(px);
		}
		free(b);
	}
	return y;
}

/*
 * The level shown drawn into its box, wound by the window's scroll;
 * answers how tall all of it is.
 */
static INT draw_set( INT gid )
{
	T_TRSTS		*s;
	T_TAD		*frag;
	UB		*bytes = NULL;
	T_DPRECT	r = st.box, in, was;
	INT		nrec, tall = 0;

	dp_fill_rect(gid, &r, TV_PAPER);
	dp_frame_rect(gid, &r, TV_EDGE, 1);
	if ( st.level <= 0 ) {
		return 0;
	}
	s = tray_now();
	if ( s == NULL ) {
		return 0;
	}
	nrec = s->set[st.level].nrec;
	free(s);
	in = r;
	in.left += 1;  in.top += 1;  in.right -= 1;  in.bottom -= 1;
	if ( !narrow(gid, &in, &was) ) {
		return 0;
	}
	frag = level_frag(st.level, nrec, &bytes);
	if ( frag != NULL && st.kind[st.level] != K_PIC ) {
		T_TADNODE	*body = tad_body(frag);

		if ( body != NULL && body->name != NULL && body->name[0] == 'd' ) {
			T_TVDOC		*doc = NULL;
			T_DPRECT	page = in;

			page.left += 4;
			page.right -= 4;
			if ( tv_doc(frag, &doc) >= E_OK && doc != NULL ) {
				doc->detail = st.tags;
				tall = tv_doc_draw(gid, doc, &page, st.scroll_y, frag, TV_PAPER);
				tv_doc_free(doc);
			}
		} else {
			T_TVFIG	*fig = NULL;
			INT	fw = 0, fh = 0;

			if ( tv_fig(frag, &fig) >= E_OK && fig != NULL ) {
				(void)tv_fig_draw(gid, fig, &in, 0, st.scroll_y, frag, TV_PAPER);
				if ( tv_fig_size(fig, &fw, &fh) >= E_OK ) {
					tall = fh;
				}
				tv_fig_free(fig);
			}
		}
	} else {
		tall = draw_pictures(gid, st.level, nrec, &in, st.scroll_y);
	}
	if ( frag != NULL ) {
		tad_free(frag);
	}
	free(bytes);
	dp_set_visible(gid, &was);
	return tall;
}

/* The bar down the right: the set's height against what the box shows */
static void set_bar( INT tall )
{
	INT	view = st.box.bottom - st.box.top - 2;
	INT	extra = st.h - view;		/* what of the work area is not the set */
	INT	chi;

	if ( view < 1 ) view = 1;
	if ( st.list || tall < view ) {
		tall = view;
	}
	st.height = tall + extra;
	if ( st.scroll_y > tall - view ) {
		st.scroll_y = ( tall > view ) ? tall - view : 0;
	}
	if ( st.scroll_y < 0 ) {
		st.scroll_y = 0;
	}
	chi = st.scroll_y + st.h;
	if ( chi > st.height ) chi = st.height;
	ui_bar(&ui, 0, st.height, st.scroll_y, chi);
}

/* What the program draws over the panel: the set in the sheet, and the window's bar */
static void draw( void )
{
	INT	tall, s0;

	if ( g_gid < 0 ) {
		return;
	}
	if ( !st.list || st.level <= 0 ) {
		s0 = st.scroll_y;
		tall = draw_set(g_gid);
		set_bar(tall);
		if ( st.scroll_y != s0 ) {
			(void)draw_set(g_gid);	/* wound back: the set was shorter than where it was */
		}
	} else {
		set_bar(0);
	}
	(void)wm_obj_flush(ui.kw, NULL);
}

/* ---------------------------------------------------------------- what the person does */

static void show_level( INT level )
{
	char	s[64];

	if ( level < 1 || level > st.nlevel || level == st.level ) {
		return;
	}
	st.level = level;
	st.scroll_y = 0;
	rebuild();
	snprintf(s, sizeof(s), "level %d %s", level, kind_name[st.kind[level]]);
	say(s);
	draw();
}

static void wind( INT by )
{
	INT	was = st.scroll_y;

	st.scroll_y += by;
	if ( st.scroll_y < 0 ) {
		st.scroll_y = 0;
	}
	if ( st.scroll_y != was ) {
		draw();			/* the bar keeps it within the set */
	}
}

static void key( CONST T_OBNTM *m )
{
	if ( ( m->mods & OB_KEY_UP ) != 0 ) {
		return;
	}
	if ( ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
		if ( wm_key_cmd(m->code) == KEY_E ) {
			g_closing = TRUE;	/* 閉じる's letter */
		}
		return;
	}
	if ( m->code == KEY_UP ) {
		show_level(st.level - 1);
	} else if ( m->code == KEY_DOWN ) {
		show_level(st.level + 1);
	} else if ( m->code >= KEY_1 && m->code <= KEY_0 ) {
		show_level((INT)m->code - KEY_1 + 1);
	} else if ( ( m->code == KEY_PGUP || m->code == KEY_PGDN ) && !st.list ) {
		INT	page = st.box.bottom - st.box.top - 2 * TV_LINE;

		wind(( m->code == KEY_PGUP ) ? -page : page);
	}
}

static BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def, view;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;
	char	s[64];

	if ( ui_uuid(SYSDEF_MENU_TRAYV, &def) < E_OK || ui_uuid(SYSDEF_MENU_TRAYV_VIEW, &view) < E_OK
	  || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	(void)mn_chg_atr(mid, &view, "tray.detail", st.list ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, &view, "tray.list", st.list ? MN_TICK : 0);
	(void)mn_chg_atr(mid, &view, "tray.tags", st.tags ? MN_TICK : 0);
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) {
		return;
	}
	if ( is_code(&sel, "close") ) {
		g_closing = TRUE;
		return;
	}
	if ( is_code(&sel, "tray.detail") ) {
		st.list = FALSE;
	} else if ( is_code(&sel, "tray.list") ) {
		st.list = TRUE;
	} else if ( is_code(&sel, "tray.tags") ) {
		st.tags = !st.tags;
	} else {
		return;
	}
	snprintf(s, sizeof(s), "%s, tags %s", st.list ? "list" : "detail", st.tags ? "on" : "off");
	say(s);
	st.scroll_y = 0;
	rebuild();
	draw();
}

/* Every few tenths of a second: the window built again when the tray changed */
static void poll( void )
{
	T_TRSTS	*s;
	char	m[96];

	st.looked = ui_now();
	s = tray_now();
	if ( s == NULL ) {
		return;
	}
	if ( tray_sig(s) != st.sig ) {
		/* what was put in goes in at level 1 and is in hand: the level in hand is shown */
		st.level = ( s->hand > 0 ) ? s->hand : 1;
		build(s);
		st.scroll_y = 0;
		snprintf(m, sizeof(m), "changed, %d levels, level %d %s", st.nlevel, st.level,
			 kind_name[( st.level > 0 ) ? st.kind[st.level] : K_NONE]);
		say(m);
		draw();
	}
	free(s);
}

/* A notice of the window or its panel */
static BOOL event( CONST T_OBNTM *m )
{
	INT	num = 0, v;

	switch ( ui_event(&ui, m, &num) ) {
	case UI_EV_CLOSE:
		g_closing = TRUE;
		break;
	case UI_EV_MENU:
		menu(m);
		break;
	case UI_EV_KEY:
		key(m);
		break;
	case UI_EV_SHEET:
		if ( num >= 1 && num != st.level ) {
			show_level(num);
		}
		break;
	case UI_EV_CHANGE:
		if ( num == N_LIST && st.list && ( v = ui_value(&ui, N_LIST) ) >= 1 && v != st.level ) {
			show_level(v);
		}
		break;
	case UI_EV_WHEEL:
		if ( !st.list && num != 0 ) {
			wind(-num * TV_WHEEL);
		}
		break;
	case UI_EV_SCROLL:
		if ( !st.list ) {
			wind(num - st.scroll_y);
		}
		break;
	case UI_EV_SIZE:
		/* drawn again: the size changed, or the panel was drawn over the sheet */
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

int main( void )
{
	T_OBNTM	m;
	T_TRSTS	*s;
	char	line[64];
	ER	er;

	memset(&st, 0, sizeof(st));
	st.tags = TRUE;
	st.w = st.h = st.wide = -1;
	ui_icon(SYSDEF_PROG_TRAY);
	ui_shows(SYSDEF_PROG_TRAY);
	ui_attr(WM_ATTR_RESIZE | WM_ATTR_RBAR | UI_ATTR_HIDDEN);
	if ( ui_open(&ui, SYSDEF_BOX_TRAYV, PN_MAIN, wm_new_x(TV_WIN_X), wm_new_y(TV_WIN_Y)) < E_OK ) {
		say("no window");
		return 1;
	}
	g_gid = wm_obj_gid(ui.kw);
	s = tray_now();
	if ( s != NULL ) {
		st.level = ( s->hand > 0 ) ? s->hand : 1;
		build(s);
		free(s);
	}
	ui_reveal(&ui);
	snprintf(line, sizeof(line), "opened, %d levels", st.nlevel);
	say(line);
	draw();

	while ( !g_closing ) {
		UD	next = st.looked + TV_LOOK_MS;
		BOOL	again = FALSE;
		TMO	tmo;

		for ( tmo = ui_left(next); !g_closing && ( er = ui_wait(&ui, &m, tmo) ) >= E_OK; tmo = TMO_POL ) {
			if ( event(&m) ) {
				again = TRUE;
			}
		}
		if ( g_closing ) {
			break;
		}
		if ( again ) {
			INT	w = 0, h = 0;

			ui_work(&ui, &w, &h);
			if ( w > 0 && h > 0 && ( w != st.w || h != st.h ) ) {
				lay_out(w, h, st.wide);
			}
			draw();
		}
		if ( ui_now() >= next ) {
			poll();
		}
	}
	ui_close(&ui);
	say("closed");
	return 0;
}
