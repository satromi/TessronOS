/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttrayv.c
 *	トレー: the accessory that shows everything in the tray
 *	(docs/tessronos/11-desktop.md 11.27)
 *
 *	One window of the desktop's (DTOWN). Its work area is a panel of
 *	tags standing one under another down its left side (P_DOWN), a tag
 *	for each of the tray's ten levels, those that hold nothing greyed
 *	and named by the number alone, the others by the level's number and
 *	the kind of what it holds: 文章, 図形, 仮身, 付せん (links to
 *	programs: 実行機能付せん), 画像 (pictures alone) or その他 (records
 *	of real objects). The sheet beside the tags says the
 *	kind and how many bytes the set is, and under that the set is
 *	drawn: a document laid out to the sheet's width, a figure from its
 *	corner, pictures one under another. The window's bar winds it.
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
 *	tray keeps, and the tray is left as it is.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
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
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include "desktop.h"

#define KEY_1		0x1E
#define KEY_0		0x27
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_DOWN	0x51
#define KEY_UP		0x52

#define N_TAGS		100
#define N_INFO		101
#define N_LIST		102

#define TV_WIN_W	560		/* the work area as it opens */
#define TV_WIN_H	380
#define TV_EDGE_IN	6		/* the sheet and its tags stand in this far from the work area's edge */
#define TV_TAG_FH	16		/* the letters of a tag (PD_FH) */
#define TV_TAG_ROOM	17		/* a tag's width besides its name: cut head, edges, and its gap to the sheet */
#define TV_LOOK_MS	300		/* how often the tray is looked at */
#define TV_WHEEL	48
#define TV_LINE		16		/* an arrow's step in a list of pictures */
#define TV_FRAG_MAX	( 4 * 1024 * 1024 )
#define TV_GAP		8		/* between pictures, and round them */
#define TV_PAPER	0x00FFFFFFU
#define TV_EDGE		0x00808080U
#define TV_ROW_MAX	40		/* letters of a row of the list */

/* What a level holds */
#define K_NONE		0
#define K_DOC		1		/* 文章 */
#define K_FIG		2		/* 図形 */
#define K_LINK		3		/* 仮身 */
#define K_TAG		4		/* 付せん: links to programs */
#define K_PIC		5		/* 画像 */
#define K_OTHER		6		/* その他 */

LOCAL CONST char *CONST kind_name[] = {
	"", "文章", "図形", "仮身", "付せん", "画像", "その他"
};

typedef struct {
	INT		pid;		/* the panel, -1 none */
	INT		w, h;		/* the work area the panel was built for */
	BOOL		list;		/* 一覧表示 */
	BOOL		tags;		/* 付せん表示 */
	INT		level;		/* the level shown, 0 when the tray is empty */
	INT		nlevel;
	UB		kind[TR_LEVELS + 1];
	UW		sig;		/* what the tray was, when the panel was built */
	UW		looked;		/* when, in ms */
	T_DPRECT	box;		/* where the set is drawn */
} TRV;

LOCAL CONST DTOWN	trv_own;
LOCAL DTWIN		*trv_win;

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

/* ---------------------------------------------------------------- the tray */

/* Everything that says the tray changed, in one word */
LOCAL UW tray_sig( CONST T_TRSTS *s )
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

LOCAL T_TRSTS *tray_now( void )
{
	T_TRSTS	*s = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));

	if ( s != NULL && tr_get_sts(s) < E_OK ) {
		Kfree(s);
		s = NULL;
	}
	return s;
}

/* One record of a level, read whole; NULL when it is not there */
LOCAL UB *rec_bytes( INT level, INT rec, T_TRINF *inf )
{
	UB	*buf;
	SZ	asz = 0;

	if ( tr_ref_rec(level, rec, inf) < E_OK || inf->size <= 0 || inf->size > TV_FRAG_MAX ) {
		return NULL;
	}
	buf = (UB *)Kmalloc(inf->size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	if ( tr_rea_rec(level, rec, buf, inf->size, &asz) < E_OK ) {
		Kfree(buf);
		return NULL;
	}
	buf[asz] = 0;
	inf->size = asz;
	return buf;
}

/* The first xmlTAD fragment of a level, read; the bytes are the caller's to free with it */
LOCAL T_TAD *level_frag( INT level, INT nrec, UB **p_bytes )
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
		Kfree(b);
	}
	return NULL;
}

/* Whether every link of a fragment points at a program */
LOCAL BOOL links_to_programs( CONST T_TAD *frag, INT n )
{
	T_VOBJ	v;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(frag, i, &v) < E_OK || dt_prog_of(&v.target) == NULL ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* Whether every shape of a figure is a picture the set carries ("tray:N") */
LOCAL BOOL only_tray_pictures( CONST T_TAD *frag )
{
	CONST T_TADNODE	*body = tad_body(frag), *c;
	CONST UB	*h;
	INT		n = 0;

	for ( c = ( body != NULL ) ? body->first : NULL; c != NULL; c = c->next ) {
		if ( c->kind != TAD_ND_ELEM ) {
			continue;
		}
		h = tad_attr(c, "href");
		if ( h == NULL || h[0] != 't' || h[1] != 'r' || h[2] != 'a' || h[3] != 'y' || h[4] != ':' ) {
			return FALSE;
		}
		n++;
	}
	return (BOOL)( n > 0 );
}

LOCAL UB kind_of( INT level, CONST T_TRSET *set )
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
		} else if ( links > 0 && dt_frag_shapes(frag) == 0 ) {
			k = links_to_programs(frag, links) ? K_TAG : K_LINK;
		} else {
			k = only_tray_pictures(frag) ? K_PIC : K_FIG;
		}
		tad_free(frag);
		Kfree(bytes);
	} else if ( ( set->kinds & ( 1U << TR_PNG ) ) != 0 ) {
		k = K_PIC;
	}
	return k;
}

/* How many bytes a level's records are */
LOCAL SZ level_size( INT level, INT nrec )
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

LOCAL INT put( UB *b, INT n, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = (UB)*s++;
	}
	b[n] = 0;
	return n;
}

LOCAL INT put_num( UB *b, INT n, INT max, SZ v )
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
LOCAL INT frag_words( CONST UB *x, SZ len, UB *out, INT max, INT letters )
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
LOCAL void row_of( INT level, CONST T_TRSET *set, UB kind, UB *out, INT max )
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
				Kfree(b);
				return;
			}
			Kfree(b);
		}
		break;
	}
	(void)put(out, n, max, (CONST char *)set->name);	/* nothing to read: the set's name */
}

/* ---------------------------------------------------------------- the panel */

LOCAL void work_size( DTWIN *d, INT *p_w, INT *p_h )
{
	T_WMWIN	w;

	*p_w = *p_h = 0;
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		*p_w = w.work.right - w.work.left;
		*p_h = w.work.bottom - w.work.top;
	}
}

/* The words over the set: its kind, its size and its name */
LOCAL void info_words( TRV *st, CONST T_TRSTS *s, UB *out, INT max )
{
	INT	n = 0;

	if ( st->level <= 0 ) {
		(void)put(out, 0, max, "トレーは空です");
		return;
	}
	n = put(out, n, max, "種別：");
	n = put(out, n, max, kind_name[st->kind[st->level]]);
	n = put(out, n, max, "　サイズ：");
	n = put_num(out, n, max, level_size(st->level, s->set[st->level].nrec));
	n = put(out, n, max, "バイト");
	if ( s->hand == st->level ) {
		n = put(out, n, max, "　（扱い中）");
	}
	(void)n;
}

/*
 * The panel built again from the tray: a tag for each of the ten levels,
 * those that hold nothing greyed and not to be chosen, the words over the sheet, and in 一覧表示 the list filling the
 * rest of it. Kept as large as the work area.
 */
LOCAL void build( DTWIN *d, TRV *st, CONST T_TRSTS *s )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_DPRECT	sheet;
	INT		w, h, i, at = -1, k;
	UB		words[WM_LABEL_MAX];

	work_size(d, &w, &h);
	if ( w <= 0 || h <= 0 ) {
		return;
	}
	if ( st->pid >= 0 ) {
		(void)wm_panel_close(st->pid);
		st->pid = -1;
	}
	st->w = w;
	st->h = h;
	st->nlevel = 0;
	for ( i = 1; i <= TR_LEVELS; i++ ) {
		st->kind[i] = kind_of(i, &s->set[i]);
		if ( st->kind[i] != K_NONE ) {
			st->nlevel = i;
		}
	}
	if ( st->level > st->nlevel ) {
		st->level = st->nlevel;
	}
	if ( st->level <= 0 && st->nlevel > 0 ) {
		st->level = 1;
	}

	def = dt_pn_new(w - 2 * DT_PN_IN, h - 2 * DT_PN_IN);
	if ( def == NULL ) {
		return;
	}
	def->kind = WM_PNL_BARE;
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = w;
	def->r.bottom = h;
	sheet.left = sheet.top = TV_EDGE_IN;
	sheet.right = w - TV_EDGE_IN;
	sheet.bottom = h - TV_EDGE_IN;

	pt = dt_pn_add(def, WM_PT_TAGS, N_TAGS, 0, 0, 0, 0, NULL);
	if ( pt != NULL ) {
		ID	fid = fn_system();
		INT	wide = 0;

		pt->type |= P_DOWN;
		pt->r.left = 0;
		pt->r.top = 0;
		pt->r.right = w;
		pt->r.bottom = h;
		if ( fid > 0 ) {
			(void)fn_set_size(fid, TV_TAG_FH);
		}
		for ( i = 1; i <= TR_LEVELS; i++ ) {
			UB	name[24];
			INT	n = put_num(name, 0, sizeof(name), (SZ)i);

			if ( st->kind[i] != K_NONE ) {
				n = put(name, n, sizeof(name), " ");
				(void)put(name, n, sizeof(name), kind_name[st->kind[i]]);
			} else {
				pt->inact |= 1U << ( i - 1 );
			}
			k = wm_panel_name(def, name);
			if ( at < 0 ) at = k;
			n = ( fid > 0 ) ? fn_width(fid, name) : TV_TAG_FH * 4;
			if ( n > wide ) wide = n;
		}
		pt->pool = ( at >= 0 ) ? at : 0;
		pt->count = TR_LEVELS;
		pt->value = st->level;
		pt->inner.left = TV_EDGE_IN + wide + TV_TAG_ROOM;
		pt->inner.top = TV_EDGE_IN;
		pt->inner.right = w - TV_EDGE_IN;
		pt->inner.bottom = h - TV_EDGE_IN;
		sheet = pt->inner;
	}

	info_words(st, s, words, sizeof(words));
	pt = dt_pn_add(def, WM_PT_LABEL, N_INFO, 0, 0, 0, 0, (CONST char *)words);
	if ( pt != NULL ) {
		pt->r.left = sheet.left + 8;
		pt->r.top = sheet.top + 6;
		pt->r.right = sheet.right - 8;
		pt->r.bottom = sheet.top + 28;
	}

	st->box.left = sheet.left + 6;
	st->box.top = sheet.top + 32;
	st->box.right = sheet.right - 6;
	st->box.bottom = sheet.bottom - 6;

	if ( st->list && st->nlevel > 0 ) {
		pt = dt_pn_add(def, WM_PT_LIST, N_LIST, 0, 0, 0, 0, NULL);
		if ( pt != NULL ) {
			at = -1;
			pt->r = st->box;
			for ( i = 1; i <= st->nlevel; i++ ) {
				UB	row[WM_LABEL_MAX];

				row_of(i, &s->set[i], st->kind[i], row, sizeof(row));
				k = wm_panel_name(def, row);
				if ( at < 0 ) at = k;
			}
			pt->pool = ( at >= 0 ) ? at : 0;
			pt->count = st->nlevel;
			pt->value = st->level;
			pt->top = 1;
		}
	}

	st->pid = wm_panel_open(d->wid, def);
	Kfree(def);
	st->sig = tray_sig(s);
	st->looked = now_ms();
}

LOCAL void rebuild( DTWIN *d )
{
	TRV	*st = (TRV *)d->self;
	T_TRSTS	*s = tray_now();

	if ( s == NULL ) {
		return;
	}
	build(d, st, s);
	Kfree(s);
}

/* ---------------------------------------------------------------- drawing the set */

/* Drawing kept inside a box; FALSE when nothing of it can be seen */
LOCAL BOOL narrow( INT gid, CONST T_DPRECT *r, T_DPRECT *p_was )
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
LOCAL INT draw_pictures( INT gid, INT level, INT nrec, CONST T_DPRECT *r, INT scroll )
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
			if ( gid >= 0 ) {
				(void)dp_put_argb(gid, r->left + TV_GAP, r->top + y - scroll, px, pw,
						  pw, ph, IMG_CLEAR);
			}
			y += ph + TV_GAP;
			Kfree(px);
		}
		Kfree(b);
	}
	return y;
}

/*
 * The level shown drawn into its box, wound by the window's scroll;
 * answers how tall all of it is.
 */
LOCAL INT draw_set( DTWIN *d, TRV *st, INT gid )
{
	T_TRSTS		*s;
	T_TAD		*frag;
	UB		*bytes = NULL;
	T_DPRECT	r = st->box, in, was;
	INT		nrec, tall = 0;

	dp_fill_rect(gid, &r, TV_PAPER);
	dp_frame_rect(gid, &r, TV_EDGE, 1);
	if ( st->level <= 0 ) {
		return 0;
	}
	s = tray_now();
	if ( s == NULL ) {
		return 0;
	}
	nrec = s->set[st->level].nrec;
	Kfree(s);
	in = r;
	in.left += 1;  in.top += 1;  in.right -= 1;  in.bottom -= 1;
	if ( !narrow(gid, &in, &was) ) {
		return 0;
	}
	frag = level_frag(st->level, nrec, &bytes);
	if ( frag != NULL && st->kind[st->level] != K_PIC ) {
		T_TADNODE	*body = tad_body(frag);

		if ( body != NULL && body->name != NULL && body->name[0] == 'd' ) {
			T_TVDOC	*doc = NULL;
			T_DPRECT	page = in;

			page.left += 4;
			page.right -= 4;
			if ( tv_doc(frag, &doc) >= E_OK && doc != NULL ) {
				doc->detail = st->tags;
				tall = tv_doc_draw(gid, doc, &page, d->scroll_y, frag, TV_PAPER);
				tv_doc_free(doc);
			}
		} else {
			T_TVFIG	*fig = NULL;
			INT	fw = 0, fh = 0;

			if ( tv_fig(frag, &fig) >= E_OK && fig != NULL ) {
				(void)tv_fig_draw(gid, fig, &in, 0, d->scroll_y, frag, TV_PAPER);
				if ( tv_fig_size(fig, &fw, &fh) >= E_OK ) {
					tall = fh;
				}
				tv_fig_free(fig);
			}
		}
	} else {
		tall = draw_pictures(gid, st->level, nrec, &in, d->scroll_y);
	}
	if ( frag != NULL ) {
		tad_free(frag);
	}
	if ( bytes != NULL ) {
		Kfree(bytes);
	}
	dp_set_visible(gid, &was);
	return tall;
}

/* The bar down the right: the set's height against what the box shows */
LOCAL void set_bar( DTWIN *d, TRV *st, INT tall )
{
	T_WMBAR	b;
	INT	view = st->box.bottom - st->box.top - 2;
	INT	extra = st->h - view;		/* what of the work area is not the set */

	if ( view < 1 ) view = 1;
	if ( st->list || tall < view ) {
		tall = view;
	}
	d->height = tall + extra;
	if ( d->scroll_y > tall - view ) {
		d->scroll_y = ( tall > view ) ? tall - view : 0;
	}
	if ( d->scroll_y < 0 ) {
		d->scroll_y = 0;
	}
	b.lo = 0;
	b.hi = d->height;
	b.clo = d->scroll_y;
	b.chi = d->scroll_y + st->h;
	if ( b.chi > b.hi ) b.chi = b.hi;
	wm_set_bar(d->wid, WM_BAR_R, &b);
}

/* ---------------------------------------------------------------- the window */

LOCAL void trv_draw( DTWIN *d )
{
	TRV	*st = (TRV *)d->self;
	INT	gid = wm_gid(d->wid), w, h, tall = 0, s0;

	if ( st == NULL || gid < 0 ) {
		return;
	}
	work_size(d, &w, &h);
	if ( st->pid < 0 || w != st->w || h != st->h ) {
		rebuild(d);
	}
	dt_own_ground(d);
	if ( st->pid >= 0 ) {
		wm_panel_draw(st->pid);
	}
	if ( !st->list || st->level <= 0 ) {
		s0 = d->scroll_y;
		tall = draw_set(d, st, gid);
		set_bar(d, st, tall);
		if ( d->scroll_y != s0 ) {
			(void)draw_set(d, st, gid);	/* wound back: the set was shorter than where it was */
		}
	} else {
		set_bar(d, st, 0);
	}
	wm_update();
}

LOCAL void show_level( DTWIN *d, INT level )
{
	TRV	*st = (TRV *)d->self;

	if ( level < 1 || level > st->nlevel || level == st->level ) {
		return;
	}
	st->level = level;
	d->scroll_y = 0;
	rebuild(d);
	tm_printf((UB *)"trayview: level %d %s\n", level, kind_name[st->kind[level]]);
	trv_draw(d);
}

LOCAL void wind( DTWIN *d, INT by )
{
	INT	was = d->scroll_y;

	d->scroll_y += by;
	if ( d->scroll_y < 0 ) {
		d->scroll_y = 0;
	}
	if ( d->scroll_y != was ) {
		trv_draw(d);		/* the bar keeps it within the set */
	}
}

LOCAL BOOL trv_input( DTWIN *d, CONST T_WMEV *ev )
{
	TRV	*st = (TRV *)d->self;
	UINT	ans = WM_ANS_NONE;
	INT	v;

	if ( st == NULL || st->pid < 0 ) {
		return FALSE;
	}
	if ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) {
		if ( ( ev->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
			return FALSE;		/* the menu's letters */
		}
		if ( ev->type == HID_EV_KEY_UP ) {
			return TRUE;
		}
		if ( ev->code == KEY_UP ) {
			show_level(d, st->level - 1);
		} else if ( ev->code == KEY_DOWN ) {
			show_level(d, st->level + 1);
		} else if ( ev->code >= KEY_1 && ev->code <= KEY_0 ) {
			show_level(d, ev->code - KEY_1 + 1);
		} else if ( ev->code == KEY_PGUP || ev->code == KEY_PGDN ) {
			INT	page = st->box.bottom - st->box.top - 2 * TV_LINE;

			if ( !st->list ) {
				wind(d, ( ev->code == KEY_PGUP ) ? -page : page);
			}
		}
		return TRUE;
	}
	if ( ev->type == HID_EV_WHEEL ) {
		if ( ev->code != HID_WHEEL_V ) {
			return TRUE;		/* the view and its list go only up and down */
		}
		if ( !st->list && ev->dz != 0 ) {
			wind(d, -ev->dz * TV_WHEEL);
			return TRUE;
		}
	} else if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return TRUE;			/* the second button: the desktop's menu */
	}
	if ( wm_panel_event(st->pid, ev, &ans) < E_OK ) {
		return TRUE;
	}
	if ( wm_panel_get(st->pid, N_TAGS, &v) >= E_OK && v != st->level && v >= 1 ) {
		show_level(d, v);
		return TRUE;
	}
	if ( st->list && wm_panel_get(st->pid, N_LIST, &v) >= E_OK && v != st->level && v >= 1 ) {
		show_level(d, v);
		return TRUE;
	}
	if ( ev->type != HID_EV_MOVE ) {
		trv_draw(d);
	}
	return TRUE;
}

/* Every few tenths of a second: the window built again when the tray changed */
LOCAL void trv_poll( DTWIN *d )
{
	TRV	*st = (TRV *)d->self;
	T_TRSTS	*s;
	UW	now = now_ms();

	if ( st == NULL || (UW)( now - st->looked ) < TV_LOOK_MS ) {
		return;
	}
	st->looked = now;
	s = tray_now();
	if ( s == NULL ) {
		return;
	}
	if ( tray_sig(s) != st->sig ) {
		/* what was put in goes in at level 1 and is in hand: the level in hand is shown */
		st->level = ( s->hand > 0 ) ? s->hand : 1;
		build(d, st, s);
		d->scroll_y = 0;
		tm_printf((UB *)"trayview: changed, %d levels, level %d %s\n", st->nlevel, st->level,
			  kind_name[( st->level > 0 ) ? st->kind[st->level] : K_NONE]);
		trv_draw(d);
	}
	Kfree(s);
}

LOCAL void trv_menu_set( DTWIN *d, ID mid )
{
	TRV	*st = (TRV *)d->self;

	(void)mn_chg_atr(mid, NULL, "tray.detail", st->list ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, NULL, "tray.list", st->list ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "tray.tags", st->tags ? MN_TICK : 0);
}

LOCAL void trv_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	TRV	*st = (TRV *)d->self;

	if ( mn_is(sel, "tray.detail") ) {
		st->list = FALSE;
	} else if ( mn_is(sel, "tray.list") ) {
		st->list = TRUE;
	} else if ( mn_is(sel, "tray.tags") ) {
		st->tags = !st->tags;
	} else {
		return;
	}
	tm_printf((UB *)"trayview: %s, tags %s\n", st->list ? "list" : "detail", st->tags ? "on" : "off");
	d->scroll_y = 0;
	rebuild(d);
	trv_draw(d);
}

LOCAL void trv_close( DTWIN *d )
{
	TRV	*st = (TRV *)d->self;

	if ( st != NULL ) {
		if ( st->pid >= 0 ) {
			(void)wm_panel_close(st->pid);
		}
		Kfree(st);
	}
	if ( trv_win == d ) {
		trv_win = NULL;
	}
	tm_printf((UB *)"trayview: closed\n");
}

LOCAL CONST DTOWN	trv_own = {
	SYSDEF_MENU_TRAYV,
	trv_draw,
	trv_input,
	NULL,
	trv_menu_set,
	trv_menu_do,
	NULL,
	trv_close,
	trv_poll
};

/* 小物's トレー: one window, brought to the front when it is open already */
EXPORT void dt_trayv_open( void )
{
	TRV		*st;
	DTWIN		*w;
	T_WMWIN		ww;
	T_DPRECT	o;

	if ( trv_win != NULL && trv_win->used && trv_win->own == &trv_own ) {
		wm_raise(trv_win->wid);
		wm_focus(trv_win->wid);
		wm_update();
		return;
	}
	st = (TRV *)Kmalloc(sizeof(TRV));
	if ( st == NULL ) {
		return;
	}
	knl_memset(st, 0, sizeof(*st));
	st->pid = -1;
	st->tags = TRUE;
	o.left = 160;
	o.top = 100;
	o.right = o.left + TV_WIN_W + 40;
	o.bottom = o.top + TV_WIN_H + 40;
	w = dt_own_open(&trv_own, st, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE | WM_ATTR_RBAR,
			"トレー", SYSDEF_PROG_TRAY);
	if ( w == NULL ) {
		Kfree(st);
		return;
	}
	if ( wm_ref(w->wid, &ww) >= E_OK ) {
		/* the frame measured: the work area made the size it opens at */
		o = ww.outer;
		o.right += TV_WIN_W - ( ww.work.right - ww.work.left );
		o.bottom += TV_WIN_H - ( ww.work.bottom - ww.work.top );
		(void)wm_move(w->wid, &o);
	}
	trv_win = w;
	{
		T_TRSTS	*s = tray_now();

		if ( s != NULL ) {
			st->level = ( s->hand > 0 ) ? s->hand : 1;
			build(w, st, s);
			Kfree(s);
		}
	}
	tm_printf((UB *)"trayview: opened, %d levels\n", st->nlevel);
	dt_draw(w);
}
