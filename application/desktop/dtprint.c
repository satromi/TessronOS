/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtprint.c
 *	印刷 as the desktop does it (design 11.22): the panels that say how
 *	(印刷設定, 印刷詳細設定), the pages shown before they are written
 *	(画面表示), and the file put on the desktop as a real object
 *
 *	What is printed is pages of a layout paper drawn by whatever shows
 *	them: a document's pages by its own drawing (tv_doc_draw_page), a
 *	figure as the sheets of its paper laid side by side from its top
 *	left corner, left to right and then down, and the pages a process
 *	hands over as records of xmlTAD in an object made for the purpose
 *	(DT_RQ_PRINT). The layer below (prn_) lays them on the printing
 *	paper and writes the file.
 *
 *	The output is a PDF file. It is made a real object the way a file
 *	from outside is, and a link to it is put in the desktop's first
 *	window, from which it can be given out as a file (ファイル変換) like
 *	any file taken in.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/fn.h>
#include <ts/print.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESC		0x29
#define KEY_BS		0x2A
#define KEY_F10		0x43
#define KEY_PGUP	0x4B
#define KEY_PGDN	0x4E
#define KEY_RIGHT	0x4F
#define KEY_LEFT	0x50
#define KEY_DOWN	0x51
#define KEY_UP		0x52

/* ---------------------------------------------------------------- the pages */

/*
 * A document without its links, for 仮身: 印刷 off: the runs that are
 * links left out, so that the text closes up where they stood. The
 * copy shares everything else with the document it was made from.
 */
LOCAL T_TVDOC *doc_without_links( DTPRSRC *s, CONST T_TVDOC *d )
{
	T_TVDOC	*c = (T_TVDOC *)Kmalloc(sizeof(T_TVDOC));
	INT	p, i, n = 0;

	if ( c == NULL ) {
		return NULL;
	}
	*c = *d;
	s->own_run = (T_TVRUN *)Kmalloc(sizeof(T_TVRUN) * (SZ)( d->nrun + 1 ));
	s->own_para = (T_TVPARA *)Kmalloc(sizeof(T_TVPARA) * (SZ)( d->npara + 1 ));
	if ( s->own_run == NULL || s->own_para == NULL ) {
		Kfree(c);
		return NULL;
	}
	for ( p = 0; p < d->npara; p++ ) {
		s->own_para[p] = d->para[p];
		s->own_para[p].first = n;
		for ( i = d->para[p].first; i < d->para[p].first + d->para[p].n; i++ ) {
			if ( d->run[i].kind != TV_RUN_LINK ) {
				s->own_run[n++] = d->run[i];
			}
		}
		s->own_para[p].n = n - s->own_para[p].first;
	}
	c->run = s->own_run;
	c->nrun = n;
	c->para = s->own_para;

	return c;
}

LOCAL T_TVFIG *fig_without_links( DTPRSRC *s, CONST T_TVFIG *f )
{
	T_TVFIG	*c = (T_TVFIG *)Kmalloc(sizeof(T_TVFIG));
	INT	i, n = 0;

	if ( c == NULL ) {
		return NULL;
	}
	*c = *f;
	s->own_sh = (T_TVSHAPE *)Kmalloc(sizeof(T_TVSHAPE) * (SZ)( f->nsh + 1 ));
	if ( s->own_sh == NULL ) {
		Kfree(c);
		return NULL;
	}
	for ( i = 0; i < f->nsh; i++ ) {
		if ( f->sh[i].kind != TV_SH_LINK ) {
			s->own_sh[n++] = f->sh[i];
		}
	}
	c->sh = s->own_sh;
	c->nsh = n;

	return c;
}

/*
 * How deep the open links of what is printed show what they point at
 * (pr_open_set): as on the screen, or with 仮身: 展開 as many open
 * objects deep as 深さ says -- 1 the open links of the page, 2 those
 * inside them as well, and so on. Each page is drawn so and the
 * screen's depth put back.
 */
LOCAL INT	pr_open = TV_MAX_DEPTH;

LOCAL void pr_open_set( CONST T_PRNSET *set )
{
	pr_open = ( set != NULL && set->expand ) ? set->depth : TV_MAX_DEPTH;
}

LOCAL ER draw_doc( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	DTPRSRC	*s = (DTPRSRC *)arg;
	ER	er;

	INT	kz = s->doc->zoom, kd = s->doc->zoom_den, kdep = s->doc->depth, kmax = s->doc->open_max;
	BOOL	kbare = s->doc->bare;

	dp_fill_rect(gid, sheet, ( s->paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : s->paper);

	/* printed at life size, on its paper, as deep as 仮身: 展開 says */
	s->doc->zoom = s->doc->zoom_den = 8;
	s->doc->bare = FALSE;
	s->doc->depth = 0;
	s->doc->open_max = pr_open;
	er = tv_doc_draw_page(gid, s->doc, page, sheet, s->src, s->paper);
	s->doc->zoom = kz;
	s->doc->zoom_den = kd;
	s->doc->bare = kbare;
	s->doc->depth = kdep;
	s->doc->open_max = kmax;
	return er;
}

/* How far a figure's shapes reach, in its own pixels */
LOCAL void fig_reach( CONST T_TVFIG *f, INT *p_w, INT *p_h )
{
	INT	i, w = 0, h = 0;

	for ( i = 0; i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*s = &f->sh[i];

		if ( s->kind == TV_SH_GROUP ) {
			continue;
		}
		if ( s->r.right > w ) w = s->r.right;
		if ( s->r.bottom > h ) h = s->r.bottom;
	}
	*p_w = w;
	*p_h = h;
}

LOCAL ER draw_fig( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	DTPRSRC	*s = (DTPRSRC *)arg;
	INT	col = page % s->cols, row = page / s->cols;
	UW	ground = ( s->paper == TV_PAPER_LOOK ) ? wm_look(WM_LOOK_WORK) : s->paper;

	ER	er;

	dp_fill_rect(gid, sheet, ground);
	s->fig->zoom = 8;
	s->fig->open_max = pr_open;
	er = tv_fig_draw_at(gid, s->fig, sheet, col * s->pw, row * s->ph, s->src,
			    TAD_COL_NONE, 0);
	s->fig->open_max = 0;
	return er;
}

EXPORT void dt_print_src_doc( DTPRSRC *s, T_PRNJOB *job, T_TVDOC *d, CONST T_TAD *src,
			      UW paper, CONST UB *name, BOOL links )
{
	knl_memset(s, 0, sizeof(*s));
	knl_memset(job, 0, sizeof(*job));
	s->kind = TV_KIND_DOC;
	s->doc = d;
	s->src = src;
	s->paper = paper;
	if ( !links ) {
		s->own_doc = doc_without_links(s, d);
		if ( s->own_doc != NULL ) {
			s->doc = s->own_doc;
		}
	}
	job->npage = tv_doc_pages(s->doc, src);
	job->lw = ( d->paper_w > 0 ) ? d->paper_w : DT_PAPER_W;
	job->lh = ( d->paper_h > 0 ) ? d->paper_h : DT_PAPER_H;
	job->unit = 1000;			/* a document's pixel is a point */
	job->name = name;
	job->draw = draw_doc;
	job->arg = s;
}

EXPORT void dt_print_src_fig( DTPRSRC *s, T_PRNJOB *job, T_TVFIG *f, CONST T_TAD *src,
			      UW paper, CONST UB *name, BOOL links )
{
	INT	w, h;

	knl_memset(s, 0, sizeof(*s));
	knl_memset(job, 0, sizeof(*job));
	s->kind = TV_KIND_FIG;
	s->fig = f;
	s->src = src;
	s->paper = paper;
	if ( !links ) {
		s->own_fig = fig_without_links(s, f);
		if ( s->own_fig != NULL ) {
			s->fig = s->own_fig;
		}
	}
	/* the paper is in points; the figure's own pixels are 96 to the inch */
	s->pw = ( ( f->paper_w > 0 ) ? f->paper_w : DT_PAPER_W ) * 4 / 3;
	s->ph = ( ( f->paper_h > 0 ) ? f->paper_h : DT_PAPER_H ) * 4 / 3;
	fig_reach(s->fig, &w, &h);
	s->cols = ( w + s->pw - 1 ) / s->pw;
	s->rows = ( h + s->ph - 1 ) / s->ph;
	if ( s->cols < 1 ) s->cols = 1;
	if ( s->rows < 1 ) s->rows = 1;
	job->npage = s->cols * s->rows;
	job->lw = s->pw;
	job->lh = s->ph;
	job->unit = 750;
	job->name = name;
	job->draw = draw_fig;
	job->arg = s;
}

EXPORT void dt_print_src_free( DTPRSRC *s )
{
	if ( s->own_doc != NULL ) Kfree(s->own_doc);
	if ( s->own_fig != NULL ) Kfree(s->own_fig);
	if ( s->own_run != NULL ) Kfree(s->own_run);
	if ( s->own_para != NULL ) Kfree(s->own_para);
	if ( s->own_sh != NULL ) Kfree(s->own_sh);
	knl_memset(s, 0, sizeof(*s));
}

/* ---------------------------------------------------------------- pages a process hands over */

/*
 * The object a process makes for printing (DT_RQ_PRINT) holds a page
 * in each of its records, as xmlTAD: a document, which may run to
 * several pages of its paper, or a figure, which is one sheet of its
 * paper. The layout paper is the first record's.
 *
 * Only one record is made into a model at a time, when one of its
 * pages is drawn: a page of a table is a figure of a hundred pieces of
 * text, and a hundred pages of them held at once is more memory than
 * the drawing needs.
 *
 * The pages are drawn as a window of the desktop's draws its own for
 * 印刷: on the paper the request names (white when it names none), a
 * document as 詳細 and the hidden letters show it when it says so, and
 * without its links when the setting last made says 仮身: 印刷 off.
 */
#define SPOOL_MAX	PRN_PAGE_MAX

typedef struct {
	TS_UUID		obj;
	INT		nrec;
	INT		recno[SPOOL_MAX];
	SZ		size[SPOOL_MAX];
	INT		first[SPOOL_MAX + 1];	/* the first page of each record */
	UW		paper;			/* what the pages lie on */
	UINT		flags;			/* DT_F_PR_DETAIL, DT_F_PR_HIDDEN */
	BOOL		links;			/* FALSE: drawn without them */

	/* the record made into a model just now */
	INT		cur;
	T_TAD		*t;
	T_TVDOC		*doc;
	T_TVFIG		*fig;
	DTPRSRC		own;			/* its copy without the links */
} SPOOL;

LOCAL void spool_drop( SPOOL *sp )
{
	dt_print_src_free(&sp->own);
	if ( sp->doc != NULL ) tv_doc_free(sp->doc);
	if ( sp->fig != NULL ) tv_fig_free(sp->fig);
	if ( sp->t != NULL ) tad_free(sp->t);
	sp->doc = NULL;
	sp->fig = NULL;
	sp->t = NULL;
	sp->cur = -1;
}

/* What is drawn of the model made just now: its copy without the links, or itself */
LOCAL T_TVDOC *spool_doc( SPOOL *sp )
{
	return ( sp->own.own_doc != NULL ) ? sp->own.own_doc : sp->doc;
}

LOCAL T_TVFIG *spool_fig( SPOOL *sp )
{
	return ( sp->own.own_fig != NULL ) ? sp->own.own_fig : sp->fig;
}

/* A record read whole and made into its model: TRUE when it is a page */
LOCAL BOOL spool_load( SPOOL *sp, ID key, INT recno, SZ size )
{
	UB	*buf;
	SZ	got = 0, n = 0;
	ER	er = E_OK;

	spool_drop(sp);
	if ( size <= 0 || size > 0x4000000 ) {
		return FALSE;
	}
	buf = (UB *)Kmalloc(size + 1);
	if ( buf == NULL ) {
		return FALSE;
	}
	/* a read may give less than was asked */
	while ( got < size ) {
		er = ob_rea_rec(key, recno, (D)got, buf + got, size - got, &n);
		if ( er < E_OK || n <= 0 ) {
			break;
		}
		got += n;
	}
	if ( got <= 0 || tad_parse(buf, got, NULL, &sp->t) < E_OK || sp->t == NULL ) {
		Kfree(buf);
		sp->t = NULL;
		return FALSE;
	}
	Kfree(buf);
	if ( tv_kind(sp->t) == TV_KIND_FIG ) {
		if ( tv_fig(sp->t, &sp->fig) < E_OK ) {
			sp->fig = NULL;
		}
	} else if ( tv_doc(sp->t, &sp->doc) < E_OK ) {
		sp->doc = NULL;
	}
	if ( sp->fig == NULL && sp->doc == NULL ) {
		spool_drop(sp);
		return FALSE;
	}
	if ( sp->doc != NULL ) {
		sp->doc->detail = (BOOL)( ( sp->flags & DT_F_PR_DETAIL ) != 0 );
		sp->doc->show_hidden = (BOOL)( ( sp->flags & DT_F_PR_HIDDEN ) != 0 );
	}
	if ( sp->fig != NULL ) {
		sp->fig->show_hidden = (BOOL)( ( sp->flags & DT_F_PR_HIDDEN ) != 0 );
	}
	if ( !sp->links && sp->doc != NULL ) {
		sp->own.own_doc = doc_without_links(&sp->own, sp->doc);
	} else if ( !sp->links ) {
		sp->own.own_fig = fig_without_links(&sp->own, sp->fig);
	}
	return TRUE;
}

/* The record r made the model, read again when it is not the one made last */
LOCAL BOOL spool_at( SPOOL *sp, INT r )
{
	ID	key;
	BOOL	ok;

	if ( sp->cur == r && sp->t != NULL ) {
		return TRUE;
	}
	key = ob_opn_obj(&sp->obj, OB_OP_R);
	if ( key <= 0 ) {
		return FALSE;
	}
	ok = spool_load(sp, key, sp->recno[r], sp->size[r]);
	ob_cls_obj(key);
	if ( ok ) {
		sp->cur = r;
	}
	return ok;
}

LOCAL ER draw_spool( void *arg, INT gid, INT page, CONST T_DPRECT *sheet )
{
	SPOOL	*sp = (SPOOL *)arg;
	INT	r;
	ER	er;

	for ( r = 0; r + 1 < sp->nrec && sp->first[r + 1] <= page; r++ ) ;
	dp_fill_rect(gid, sheet, sp->paper);
	if ( !spool_at(sp, r) ) {
		return E_NOEXS;
	}
	if ( sp->doc != NULL ) {
		T_TVDOC	*d = spool_doc(sp);

		d->open_max = pr_open;
		er = tv_doc_draw_page(gid, d, page - sp->first[r], sheet, sp->t, sp->paper);
		d->open_max = 0;
	} else {
		T_TVFIG	*f = spool_fig(sp);

		f->zoom = 8;
		f->open_max = pr_open;
		er = tv_fig_draw_at(gid, f, sheet, 0, 0, sp->t, TAD_COL_NONE, 0);
		f->open_max = 0;
	}
	return er;
}

LOCAL void spool_free( SPOOL *sp )
{
	spool_drop(sp);
	Kfree(sp);
}

/*
 * The records of the object counted into pages; NULL when none is a
 * page. paper and flags as DT_RQ_PRINT says (DT_F_PR_*), links as
 * 仮身: 印刷 says.
 */
LOCAL SPOOL *spool_read( CONST TS_UUID *obj, T_PRNJOB *job, CONST UB *name,
			 UW paper, UINT flags, BOOL links )
{
	SPOOL	*sp = (SPOOL *)Kmalloc(sizeof(SPOOL));
	T_OBREC	*rl;
	ID	key;
	INT	i, nrec = 0, pages = 0;

	if ( sp == NULL ) {
		return NULL;
	}
	knl_memset(sp, 0, sizeof(*sp));
	knl_memset(job, 0, sizeof(*job));
	sp->obj = *obj;
	sp->cur = -1;
	sp->paper = paper;
	sp->flags = flags;
	sp->links = links;
	rl = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * SPOOL_MAX);
	key = ( rl != NULL ) ? ob_opn_obj(obj, OB_OP_R) : E_NOMEM;
	if ( key <= 0 ) {
		if ( rl != NULL ) Kfree(rl);
		Kfree(sp);
		return NULL;
	}
	if ( ob_lst_rec(key, rl, SPOOL_MAX, &nrec) < E_OK ) {
		nrec = 0;
	}
	if ( nrec > SPOOL_MAX ) {
		nrec = SPOOL_MAX;
	}
	/* each page record made a model once, to know its paper and how many pages */
	for ( i = 0; i < nrec && sp->nrec < SPOOL_MAX; i++ ) {
		INT	k = sp->nrec;

		if ( rl[i].rt != OB_RT_TAD || !spool_load(sp, key, rl[i].recno, (SZ)rl[i].size) ) {
			continue;
		}
		if ( k == 0 ) {
			if ( sp->fig != NULL ) {
				job->lw = ( ( sp->fig->paper_w > 0 ) ? sp->fig->paper_w : DT_PAPER_W ) * 4 / 3;
				job->lh = ( ( sp->fig->paper_h > 0 ) ? sp->fig->paper_h : DT_PAPER_H ) * 4 / 3;
				job->unit = 750;
			} else {
				job->lw = ( sp->doc->paper_w > 0 ) ? sp->doc->paper_w : DT_PAPER_W;
				job->lh = ( sp->doc->paper_h > 0 ) ? sp->doc->paper_h : DT_PAPER_H;
				job->unit = 1000;
			}
		}
		sp->recno[k] = rl[i].recno;
		sp->size[k] = (SZ)rl[i].size;
		sp->first[k] = pages;
		pages += ( sp->fig != NULL ) ? 1 : tv_doc_pages(spool_doc(sp), sp->t);
		sp->cur = k;
		sp->nrec++;
	}
	ob_cls_obj(key);
	Kfree(rl);
	if ( sp->nrec == 0 || pages == 0 ) {
		spool_free(sp);
		return NULL;
	}
	sp->first[sp->nrec] = pages;
	job->npage = pages;
	job->name = name;
	job->draw = draw_spool;
	job->arg = sp;

	return sp;
}

/* ---------------------------------------------------------------- 印刷詳細設定 */

typedef struct {
	CONST char	*name;
	INT		w, h;		/* tenths of a millimetre, as it lies */
} PRPAPER;

LOCAL CONST PRPAPER pr_papers[] = {
	{ "レイアウト用紙と同じ", 0, 0 },
	{ "A3用紙:縦", 2970, 4200 },	{ "A3用紙:横", 4200, 2970 },
	{ "A4用紙:縦", 2100, 2970 },	{ "A4用紙:横", 2970, 2100 },
	{ "A5用紙:縦", 1480, 2100 },	{ "A5用紙:横", 2100, 1480 },
	{ "B4用紙:縦", 2570, 3640 },	{ "B4用紙:横", 3640, 2570 },
	{ "B5用紙:縦", 1820, 2570 },	{ "B5用紙:横", 2570, 1820 },
	{ "はがき:縦", 1000, 1480 },	{ "はがき:横", 1480, 1000 },
	{ "レター:縦", 2159, 2794 },	{ "レター:横", 2794, 2159 },
	{ "その他の用紙", -1, -1 }
};
#define N_PRPAPER	( (INT)( sizeof(pr_papers) / sizeof(pr_papers[0]) ) )

#define PD_LIST		1
#define PD_H		2
#define PD_W		3
#define PD_ROTATE	4
#define PD_AUTO		5
#define PD_GIVEN	6
#define PD_PCT		7
#define PD_LINKS	8
#define PD_EXPAND	9
#define PD_DEPTH	10

/* A number as its digits */
LOCAL void num_text( INT v, UB *out )
{
	char	t[12];
	INT	n = 0, k = 0;

	if ( v < 0 ) {
		out[k++] = '-';
		v = -v;
	}
	do {
		t[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < 11 );
	while ( n > 0 ) {
		out[k++] = (UB)t[--n];
	}
	out[k] = 0;
}

/* Digits as a number, -1 when they are not */
LOCAL INT text_num( CONST UB *s )
{
	INT	v = 0;
	BOOL	any = FALSE;

	while ( *s == ' ' ) s++;
	for ( ; *s >= '0' && *s <= '9'; s++ ) {
		v = v * 10 + ( *s - '0' );
		any = TRUE;
		if ( v > 100000 ) return -1;
	}
	return any ? v : -1;
}

LOCAL void put_words( UB *to, INT max, CONST char *a, CONST UB *b, CONST char *c )
{
	INT	k = 0, i;

	for ( i = 0; a != NULL && a[i] != 0 && k < max - 1; i++ ) to[k++] = (UB)a[i];
	for ( i = 0; b != NULL && b[i] != 0 && k < max - 1; i++ ) to[k++] = b[i];
	for ( i = 0; c != NULL && c[i] != 0 && k < max - 1; i++ ) to[k++] = (UB)c[i];
	while ( k > 0 && k == max - 1 && ( to[k] & 0xC0 ) == 0x80 ) k--;
	to[k] = 0;
}

/* A box of a number, with the words in front of and after it */
LOCAL T_WMPART *num_box( T_WMPANEL *p, INT num, INT x, INT y, CONST char *before, INT w,
			 INT v, CONST char *after, BOOL grey )
{
	T_WMPART	*pt;
	INT		k;

	if ( before != NULL ) {
		dt_pn_add(p, WM_PT_LABEL, 0, x, y + 2, 60, 22, before);
		x += 60;
	}
	pt = dt_pn_add(p, WM_PT_NUMBOX | ( grey ? P_DISABLE : 0 ), num, x, y, w, 26, NULL);
	if ( pt != NULL ) {
		num_text(v, pt->text);
		for ( k = 0; pt->text[k] != 0; k++ ) ;
		pt->caret = k;
		pt->value = v;
	}
	if ( after != NULL ) {
		dt_pn_add(p, WM_PT_LABEL, 0, x + w + 4, y + 2, 60, 22, after);
	}
	return pt;
}

LOCAL INT box_num( INT pid, INT num, INT dflt )
{
	UB	t[WM_LABEL_MAX];
	INT	v;

	if ( wm_panel_text(pid, num, t, WM_LABEL_MAX) < E_OK ) {
		return dflt;
	}
	v = text_num(t);
	return ( v >= 0 ) ? v : dflt;
}

/* The paper of the list the setting is, or the last: その他 */
LOCAL INT paper_index( CONST T_PRNSET *set )
{
	INT	i;

	if ( set->paper_w <= 0 ) {
		return 0;
	}
	for ( i = 1; i < N_PRPAPER - 1; i++ ) {
		if ( pr_papers[i].w == set->paper_w && pr_papers[i].h == set->paper_h ) {
			return i;
		}
	}
	return N_PRPAPER - 1;
}

/* "A4用紙:縦 (210×297mm)" for the paper the setting prints on */
LOCAL void paper_words( CONST T_PRNJOB *job, CONST T_PRNSET *set, UB *out, INT max )
{
	INT	i = paper_index(set), w, h, pct;
	UB	a[16], b[16];
	UB	t[WM_LABEL_MAX];

	prn_paper(job, set, &w, &h, &pct);
	num_text(( w * 254 + 360000 ) / 720000, a);		/* thousandths of a point as millimetres */
	num_text(( h * 254 + 360000 ) / 720000, b);
	put_words(t, WM_LABEL_MAX, ( i == 0 ) ? "レイアウト用紙と同じ (" : pr_papers[i].name,
		  ( i == 0 ) ? a : (CONST UB *)"", ( i == 0 ) ? "×" : " (");
	if ( i != 0 ) {
		put_words(t + knl_strlen((CONST char *)t), WM_LABEL_MAX - (INT)knl_strlen((CONST char *)t),
			  NULL, a, "×");
	}
	put_words(out, max, (CONST char *)t, b, "mm)");
}

/* 印刷詳細設定: TRUE when set */
LOCAL BOOL detail_panel( DTWIN *host, CONST T_PRNJOB *job, T_PRNSET *set )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, i, at = -1, cnt = 0, w = 560, h = 344, pct;
	INT		pw, ph, cur = paper_index(set);
	UINT		ans;
	UB		t[WM_LABEL_MAX], a[16], b[16];

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return FALSE;
	}
	prn_paper(job, set, &pw, &ph, &pct);
	num_text(( job->lw * job->unit * 254 + 360000 ) / 720000, a);
	num_text(( job->lh * job->unit * 254 + 360000 ) / 720000, b);
	put_words(t, WM_LABEL_MAX, "レイアウト用紙:  ", a, "×");
	put_words(t + knl_strlen((CONST char *)t), WM_LABEL_MAX - (INT)knl_strlen((CONST char *)t), NULL, b, "mm");
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, w - 28, 22, (CONST char *)t);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 38, 90, 22, "印刷用紙:");
	pt = dt_pn_add(def, WM_PT_LIST, PD_LIST, 104, 36, 220, 170, NULL);
	for ( i = 0; i < N_PRPAPER; i++ ) {
		INT	k = wm_panel_name(def, (CONST UB *)pr_papers[i].name);

		if ( k < 0 ) {
			break;
		}
		if ( at < 0 ) {
			at = k;
		}
		cnt++;
	}
	if ( pt != NULL ) {
		pt->pool = ( at >= 0 ) ? at : 0;
		pt->count = cnt;
		pt->top = 1;
		pt->value = cur + 1;
	}
	num_box(def, PD_H, 340, 36, "縦:", 80, ( ph * 254 + 360000 ) / 720000, "mm", FALSE);
	num_box(def, PD_W, 340, 70, "横:", 80, ( pw * 254 + 360000 ) / 720000, "mm", FALSE);
	pt = dt_pn_add(def, WM_PT_CHECK, PD_ROTATE, 340, 110, 180, 22, "用紙回転");
	if ( pt != NULL ) pt->value = set->rotate ? 1 : 0;

	dt_pn_add(def, WM_PT_LABEL, 0, 14, 222, 90, 22, "倍率:");
	num_text(pct / 100, a);
	put_words(t, WM_LABEL_MAX, "自動 (", a, "%)");
	pt = dt_pn_add(def, WM_PT_CHOICE, PD_AUTO, 104, 220, 150, 22, (CONST char *)t);
	if ( pt != NULL ) {
		pt->group = PD_AUTO;
		pt->value = ( set->scale == PRN_SCALE_AUTO ) ? 1 : 0;
	}
	pt = dt_pn_add(def, WM_PT_CHOICE, PD_GIVEN, 260, 220, 70, 22, "指定");
	if ( pt != NULL ) {
		pt->group = PD_AUTO;
		pt->value = ( set->scale != PRN_SCALE_AUTO ) ? 1 : 0;
	}
	num_box(def, PD_PCT, 330, 218, NULL, 70, ( set->scale != PRN_SCALE_AUTO ) ? set->scale : pct / 100,
		"%", FALSE);

	dt_pn_add(def, WM_PT_LABEL, 0, 14, 256, 90, 22, "仮身:");
	pt = dt_pn_add(def, WM_PT_CHECK, PD_LINKS, 104, 254, 90, 22, "印刷");
	if ( pt != NULL ) pt->value = set->links ? 1 : 0;
	pt = dt_pn_add(def, WM_PT_CHECK, PD_EXPAND, 200, 254, 90, 22, "展開");
	if ( pt != NULL ) pt->value = set->expand ? 1 : 0;
	num_box(def, PD_DEPTH, 300, 252, "深さ:", 60, set->depth, NULL, FALSE);
	dt_pn_buttons(def, "設定");

	ans = dt_pn_run(host, def, &pid);
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		INT	v = dt_pn_value(pid, PD_LIST);

		if ( v >= 1 && v <= N_PRPAPER ) {
			if ( v == 1 ) {
				set->paper_w = set->paper_h = 0;
			} else if ( v < N_PRPAPER ) {
				set->paper_w = pr_papers[v - 1].w;
				set->paper_h = pr_papers[v - 1].h;
			} else {
				/* その他の用紙: the size typed in */
				INT	hh = box_num(pid, PD_H, 0), ww = box_num(pid, PD_W, 0);

				if ( hh >= 30 && hh <= 1000 && ww >= 30 && ww <= 1000 ) {
					set->paper_w = ww * 10;
					set->paper_h = hh * 10;
				}
			}
		}
		set->rotate = (BOOL)( dt_pn_value(pid, PD_ROTATE) != 0 );
		if ( dt_pn_value(pid, PD_GIVEN) != 0 ) {
			INT	p = box_num(pid, PD_PCT, 100);

			set->scale = ( p < 10 ) ? 10 : ( p > 400 ) ? 400 : p;
		} else {
			set->scale = PRN_SCALE_AUTO;
		}
		set->links = (BOOL)( dt_pn_value(pid, PD_LINKS) != 0 );
		set->expand = (BOOL)( set->links && dt_pn_value(pid, PD_EXPAND) != 0 );
		set->depth = box_num(pid, PD_DEPTH, 1);
		if ( set->depth < 1 ) set->depth = 1;
		if ( set->depth > PRN_DEPTH_MAX ) set->depth = PRN_DEPTH_MAX;
	}
	dt_pn_done(host, pid);

	return (BOOL)( ans == WM_ANS_OK );
}

/* ---------------------------------------------------------------- 画面表示 */

#define PV_BAND		40		/* the band of buttons at the head */
#define PV_PAD		16		/* grey round the sheet */

typedef struct {
	CONST T_PRNJOB	*job;
	CONST T_PRNSET	*set;
	INT		wid, gid;
	INT		ww, wh;		/* the work area */
	INT		page, pages;
	BOOL		zoom;		/* 拡大 */
	INT		cx, cy;		/* the middle of what is shown when magnified, sheet pixels */
	UW		*px;		/* the sheet as shown */
	INT		sw, sh;
	INT		typed;		/* a page number being typed, 0 none */
} PV;

typedef struct {
	CONST char	*label;
	INT		x, w;
} PVBTN;

LOCAL CONST PVBTN pv_btn[] = {
	{ "前頁", 8, 56 }, { "次頁", 68, 56 }, { "標準", 136, 56 }, { "拡大", 196, 56 },
	{ "終了", 0, 64 }		/* at the right */
};
#define PV_NBTN		5
#define PV_B_PREV	0
#define PV_B_NEXT	1
#define PV_B_STD	2
#define PV_B_ZOOM	3
#define PV_B_END	4

LOCAL INT btn_x( CONST PV *v, INT i )
{
	return ( i == PV_B_END ) ? v->ww - pv_btn[i].w - 8 : pv_btn[i].x;
}

LOCAL void pv_words( CONST PV *v, INT x, INT y, CONST UB *s, UW col )
{
	ID	fid = fn_system();

	if ( fid > 0 ) {
		fn_set_size(fid, 14);
		(void)fn_draw(v->gid, fid, x, y, s, col);
	}
}

LOCAL INT pv_width( CONST UB *s )
{
	ID	fid = fn_system();

	if ( fid <= 0 ) {
		return 0;
	}
	fn_set_size(fid, 14);
	return fn_width(fid, s);
}

/* The sheet as shown made again: the whole of it, or twice as large */
LOCAL void pv_render( PV *v )
{
	INT	pw, ph, pct, aw = v->ww - 2 * PV_PAD, ah = v->wh - PV_BAND - 2 * PV_PAD;

	prn_paper(v->job, v->set, &pw, &ph, &pct);
	if ( pw <= 0 || ph <= 0 || aw <= 0 || ah <= 0 ) {
		return;
	}
	/* the whole sheet in the area, its sides kept */
	v->sh = ah;
	v->sw = (INT)( (D)ah * pw / ph );
	if ( v->sw > aw ) {
		v->sw = aw;
		v->sh = (INT)( (D)aw * ph / pw );
	}
	if ( v->zoom ) {
		v->sw *= 2;
		v->sh *= 2;
	}
	if ( v->px != NULL ) {
		Kfree(v->px);
	}
	v->px = (UW *)Kmalloc(sizeof(UW) * (SZ)v->sw * v->sh);
	if ( v->px != NULL ) {
		(void)prn_sheet(v->job, v->set, v->page, v->px, v->sw, v->sh);
	}
}

LOCAL void pv_draw( PV *v )
{
	T_DPRECT	r;
	INT		i, x, y, sx = 0, sy = 0, w, h;
	UB		t[48], a[12], b[12];
	UW		grey = 0x00808080U;

	r.left = 0;
	r.top = 0;
	r.right = v->ww;
	r.bottom = PV_BAND;
	dp_fill_rect(v->gid, &r, wm_look(WM_LOOK_PART_FACE));
	for ( i = 0; i < PV_NBTN; i++ ) {
		BOOL	on = (BOOL)( ( i == PV_B_STD && !v->zoom ) || ( i == PV_B_ZOOM && v->zoom ) );

		r.left = btn_x(v, i);
		r.top = 7;
		r.right = r.left + pv_btn[i].w;
		r.bottom = r.top + 26;
		dp_fill_rect(v->gid, &r, on ? 0x00D0E8FFU : 0x00FFFFFFU);
		dp_frame_rect(v->gid, &r, 0x00404040U, 1);
		pv_words(v, r.left + ( pv_btn[i].w - pv_width((CONST UB *)pv_btn[i].label) ) / 2, r.top + 19,
			 (CONST UB *)pv_btn[i].label, 0x00000000U);
	}
	/* the page shown, of how many, between the buttons and 終了 */
	num_text(( v->typed > 0 ) ? v->typed : v->page + 1, a);
	num_text(v->pages, b);
	put_words(t, sizeof(t), NULL, a, " / ");
	put_words(t + knl_strlen((CONST char *)t), sizeof(t) - (INT)knl_strlen((CONST char *)t), NULL, b, NULL);
	x = btn_x(v, PV_B_ZOOM) + pv_btn[PV_B_ZOOM].w;
	x += ( btn_x(v, PV_B_END) - x - pv_width(t) ) / 2;
	pv_words(v, x, 26, t, ( v->typed > 0 ) ? 0x00C00000U : 0x00000000U);

	r.left = 0;
	r.top = PV_BAND;
	r.right = v->ww;
	r.bottom = v->wh;
	dp_fill_rect(v->gid, &r, grey);
	if ( v->px == NULL ) {
		return;
	}
	/* where the sheet goes: in the middle, or around the place magnified */
	w = v->ww - 2 * PV_PAD;
	h = v->wh - PV_BAND - 2 * PV_PAD;
	if ( v->sw <= w ) {
		x = PV_PAD + ( w - v->sw ) / 2;
	} else {
		sx = v->cx - w / 2;
		if ( sx < 0 ) sx = 0;
		if ( sx > v->sw - w ) sx = v->sw - w;
		x = PV_PAD;
	}
	if ( v->sh <= h ) {
		y = PV_BAND + PV_PAD + ( h - v->sh ) / 2;
	} else {
		sy = v->cy - h / 2;
		if ( sy < 0 ) sy = 0;
		if ( sy > v->sh - h ) sy = v->sh - h;
		y = PV_BAND + PV_PAD;
	}
	dp_put_argb(v->gid, x, y, v->px + (SZ)sy * v->sw + sx, v->sw,
		    ( v->sw - sx < w ) ? v->sw - sx : w, ( v->sh - sy < h ) ? v->sh - sy : h, 0xFFFFFFFFU);
	r.left = x - 1;
	r.top = y - 1;
	r.right = x + ( ( v->sw - sx < w ) ? v->sw - sx : w ) + 1;
	r.bottom = y + ( ( v->sh - sy < h ) ? v->sh - sy : h ) + 1;
	dp_frame_rect(v->gid, &r, 0x00303030U, 1);
	wm_update();
}

LOCAL void pv_go( PV *v, INT page )
{
	if ( page < 0 ) page = 0;
	if ( page >= v->pages ) page = v->pages - 1;
	v->typed = 0;
	if ( page != v->page ) {
		v->page = page;
		pv_render(v);
	}
	pv_draw(v);
}

/* 画面表示: the sheets shown one at a time until 終了 */
LOCAL void preview( DTWIN *host, CONST T_PRNJOB *job, CONST T_PRNSET *set )
{
	PV		v;
	T_DPRECT	o;
	T_WMWIN		w;
	T_WMEV		ev;
	INT		sw = 1024, shh = 768, pw, ph, pct, h, wd;
	BOOL		done = FALSE;

	pr_open_set(set);

	knl_memset(&v, 0, sizeof(v));
	v.job = job;
	v.set = set;
	v.pages = prn_sheets(job, set);
	if ( v.pages <= 0 ) {
		dt_tell(host, "印刷するページがありません", NULL);
		return;
	}
	wm_screen_main(&sw, &shh);
	prn_paper(job, set, &pw, &ph, &pct);
	/* as tall as the screen allows, as wide as the sheet's sides say */
	h = shh - 120;
	if ( h > 900 ) h = 900;
	wd = (INT)( (D)( h - PV_BAND - 2 * PV_PAD ) * pw / ( ph > 0 ? ph : 1 ) ) + 2 * PV_PAD;
	if ( wd < 420 ) wd = 420;
	if ( wd > sw - 40 ) wd = sw - 40;
	o.left = ( sw - wd ) / 2;
	o.top = 30;
	o.right = o.left + wd;
	o.bottom = o.top + h;
	v.wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "画面表示");
	if ( v.wid <= 0 ) {
		return;
	}
	v.gid = wm_gid(v.wid);
	if ( wm_ref(v.wid, &w) >= E_OK ) {
		v.ww = w.work.right - w.work.left;
		v.wh = w.work.bottom - w.work.top;
	}
	wm_raise(v.wid);
	wm_focus(v.wid);
	v.page = 0;
	pv_render(&v);
	pv_draw(&v);
	tm_printf((UB *)"print: preview of %d sheets, %d x %d shown\n", v.pages, v.sw, v.sh);

	while ( !done ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.wid != v.wid ) {
			continue;		/* the rest of the desktop waits for this */
		}
		if ( ev.type == HID_EV_BTN_DOWN && ev.code == 0 ) {
			INT	i, hit = -1;

			if ( ev.y < PV_BAND ) {
				for ( i = 0; i < PV_NBTN; i++ ) {
					if ( ev.x >= btn_x(&v, i) && ev.x < btn_x(&v, i) + pv_btn[i].w ) {
						hit = i;
					}
				}
				switch ( hit ) {
				case PV_B_PREV:	pv_go(&v, v.page - 1);	break;
				case PV_B_NEXT:	pv_go(&v, v.page + 1);	break;
				case PV_B_STD:
					v.zoom = FALSE;
					pv_render(&v);
					pv_draw(&v);
					break;
				case PV_B_ZOOM:
					v.zoom = TRUE;
					v.cx = v.sw;		/* the middle of a sheet twice as large */
					v.cy = v.sh;
					pv_render(&v);
					pv_draw(&v);
					break;
				case PV_B_END:	done = TRUE;		break;
				default:	break;
				}
				continue;
			}
			if ( ( ev.mods & ( HID_MOD_LALT | HID_MOD_RALT ) ) != 0 ) {
				/* Alt and a press: magnified about where it was pressed */
				INT	aw = v.ww - 2 * PV_PAD, x0 = PV_PAD + ( aw - v.sw ) / 2;
				INT	y0 = PV_BAND + PV_PAD + ( v.wh - PV_BAND - 2 * PV_PAD - v.sh ) / 2;

				if ( !v.zoom ) {
					v.cx = ( ev.x - x0 ) * 2;
					v.cy = ( ev.y - y0 ) * 2;
					v.zoom = TRUE;
					pv_render(&v);
				}
				pv_draw(&v);
				continue;
			}
			/* a press on the sheet: the next, or with Shift the one before */
			if ( ( ev.mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 ) {
				pv_go(&v, v.page - 1);
			} else {
				pv_go(&v, v.page + 1);
			}
			continue;
		}
		if ( ev.type != HID_EV_KEY_DOWN ) {
			continue;
		}
		if ( ev.code >= 0x1E && ev.code <= 0x27 ) {
			/* digits: a page number, gone to by right Ctrl, F10 or Enter */
			INT	d = ( ev.code == 0x27 ) ? 0 : ev.code - 0x1D;

			v.typed = v.typed * 10 + d;
			if ( v.typed > PRN_PAGE_MAX ) v.typed = d;
			pv_draw(&v);
		} else if ( ev.code == KEY_BS ) {
			v.typed /= 10;
			pv_draw(&v);
		} else if ( ( ev.code == KEY_F10 || ev.code == KEY_ENTER || ev.code == 0xE4 ) && v.typed > 0 ) {
			pv_go(&v, v.typed - 1);
		} else if ( ev.code == KEY_PGDN || ev.code == KEY_RIGHT || ev.code == KEY_DOWN ) {
			pv_go(&v, v.page + 1);
		} else if ( ev.code == KEY_PGUP || ev.code == KEY_LEFT || ev.code == KEY_UP ) {
			pv_go(&v, v.page - 1);
		} else if ( ev.code == KEY_ESC || ev.code == KEY_ENTER ) {
			done = TRUE;
		}
	}
	if ( v.px != NULL ) {
		Kfree(v.px);
	}
	wm_close(v.wid);
	if ( host != NULL ) {
		wm_focus(host->wid);
	}
	wm_update();
}

/* ---------------------------------------------------------------- 印刷設定 */

#define PS_ALL		1
#define PS_PART		2
#define PS_FROM		3
#define PS_TO		4
#define PS_COPIES	5
#define PS_NORMAL	6
#define PS_FAST		7
#define PS_FINE		8
#define PS_COLOUR	9
#define PS_DETAIL	92
#define PS_SHOW		93

#define ANS_DETAIL	( WM_ANS_ACT + 1 )
#define ANS_SHOW	( WM_ANS_ACT + 2 )

/* What was set last time is where the panel starts next time */
LOCAL T_PRNSET	pr_last;
LOCAL BOOL	pr_last_ok = FALSE;

LOCAL UINT print_panel( DTWIN *host, CONST T_PRNJOB *job, T_PRNSET *set, BOOL show )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	INT		pid, w = 560, h = 268, i;
	UINT		ans;
	UB		t[WM_LABEL_MAX], u[WM_LABEL_MAX];

	def = dt_pn_new(w, h);
	if ( def == NULL ) {
		return WM_ANS_CANCEL;
	}
	paper_words(job, set, u, WM_LABEL_MAX);
	put_words(t, WM_LABEL_MAX, "印刷用紙:  ", u, NULL);
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, w - 28, 22, (CONST char *)t);

	dt_pn_add(def, WM_PT_LABEL, 0, 14, 40, 70, 22, "ページ:");
	pt = dt_pn_add(def, WM_PT_CHOICE, PS_ALL, 84, 38, 110, 22, "全ページ");
	if ( pt != NULL ) {
		pt->group = PS_ALL;
		pt->value = set->all ? 1 : 0;
	}
	pt = dt_pn_add(def, WM_PT_CHOICE, PS_PART, 196, 38, 110, 22, "部分ページ");
	if ( pt != NULL ) {
		pt->group = PS_ALL;
		pt->value = set->all ? 0 : 1;
	}
	num_box(def, PS_FROM, 310, 36, "開始:", 60, set->from, NULL, FALSE);
	num_box(def, PS_TO, 434, 36, "終了:", 60, ( set->to > job->npage ) ? job->npage : set->to, NULL,
		FALSE);
	num_box(def, PS_COPIES, 14, 74, "部数:", 60, set->copies, "部", FALSE);

	dt_pn_add(def, WM_PT_LABEL, 0, 14, 112, 70, 22, "品質:");
	for ( i = 0; i < 3; i++ ) {
		LOCAL CONST char *CONST q[3] = { "通常", "高速", "高密度" };

		pt = dt_pn_add(def, WM_PT_CHOICE, PS_NORMAL + i, 84 + i * 90, 110, 86, 22, q[i]);
		if ( pt != NULL ) {
			pt->group = PS_NORMAL;
			pt->value = ( set->quality == i ) ? 1 : 0;
		}
	}
	pt = dt_pn_add(def, WM_PT_CHECK, PS_COLOUR, 370, 110, 100, 22, "カラー");
	if ( pt != NULL ) pt->value = set->colour ? 1 : 0;

	num_text(job->npage, u);
	put_words(t, WM_LABEL_MAX, "出力先:  PDF ファイル (デスクトップに置く)  全 ", u, " ページ");
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 150, w - 28, 22, (CONST char *)t);

	dt_pn_buttons(def, "印刷");
	pt = dt_pn_add(def, WM_PT_BUTTON, PS_DETAIL, w - 432, h - 36, 110, 26, "詳細設定");
	if ( pt != NULL ) pt->answer = ANS_DETAIL;
	if ( show ) {
		pt = dt_pn_add(def, WM_PT_BUTTON, PS_SHOW, w - 316, h - 36, 110, 26, "画面表示");
		if ( pt != NULL ) pt->answer = ANS_SHOW;
	}

	ans = dt_pn_run(host, def, &pid);
	if ( ans != WM_ANS_CANCEL && pid >= 0 ) {
		INT	c = box_num(pid, PS_COPIES, 1);

		set->all = (BOOL)( dt_pn_value(pid, PS_ALL) != 0 );
		set->from = box_num(pid, PS_FROM, 1);
		set->to = box_num(pid, PS_TO, PRN_PAGE_MAX);
		if ( set->from < 1 ) set->from = 1;
		if ( set->from > PRN_PAGE_MAX ) set->from = PRN_PAGE_MAX;
		if ( set->to < set->from ) set->to = set->from;
		if ( set->to > PRN_PAGE_MAX ) set->to = PRN_PAGE_MAX;
		set->copies = ( c < 1 ) ? 1 : ( c > PRN_COPY_MAX ) ? PRN_COPY_MAX : c;
		set->quality = ( dt_pn_value(pid, PS_FAST) != 0 ) ? PRN_Q_FAST
			     : ( dt_pn_value(pid, PS_FINE) != 0 ) ? PRN_Q_FINE : PRN_Q_NORMAL;
		set->colour = (BOOL)( dt_pn_value(pid, PS_COLOUR) != 0 );
	}
	dt_pn_done(host, pid);

	return ans;
}

/* ---------------------------------------------------------------- the file on the desktop */

/* A link to an object as a figure keeps one: its band, at the system's size */
LOCAL void link_of( T_VOBJ *v, CONST TS_UUID *u, CONST UB *name )
{
	INT	k;

	knl_memset(v, 0, sizeof(*v));
	v->target = *u;
	for ( k = 0; name[k] != 0 && k < TAD_NAME_MAX - 1; k++ ) {
		v->name[k] = name[k];
	}
	v->name[k] = 0;
	v->chsz = 14;
	v->frcol = 0x000000;
	v->chcol = 0x000000;
	v->tbcol = 0xFFFFFF;
	v->bgcol = 0xFFFFFF;
	v->disp = TAD_D_DEFAULT;
	v->zoom = 100;
	v->right = 200;
	v->bottom = 24;
	v->height = 24;
}

/* A link to an object put in a window of a figure, under what is there */
LOCAL BOOL place_in_window( DTWIN *t, CONST TS_UUID *u, CONST UB *name )
{
	T_WMWIN		wt;
	T_DPRECT	to;
	T_VOBJ		v;
	INT		i, bottom = 8;

	if ( t == NULL || t->fig == NULL || t->sealed || wm_ref(t->wid, &wt) < E_OK ) {
		return FALSE;
	}
	for ( i = 0; i < t->fig->nsh; i++ ) {
		if ( t->fig->sh[i].kind != TV_SH_GROUP && t->fig->sh[i].r.bottom + 8 > bottom ) {
			bottom = t->fig->sh[i].r.bottom + 8;
		}
	}
	link_of(&v, u, name);
	dt_work_rect(t, &to);
	if ( ed_place_link(t, wt.work.left + to.left - t->scroll_x + 16,
			   wt.work.top + to.top - t->scroll_y + bottom, &v) ) {
		(void)dt_save(t);
	}
	return TRUE;
}

/* A link to an object put in the desktop's first window, under what is there */
EXPORT void dt_place_on_desktop( CONST TS_UUID *u, CONST UB *name )
{
	(void)place_in_window(dt_first_win(), u, name);
}

/*
 * A link to an object put in the figure 'box' (a volume's top object),
 * under what is there: in its window when it is open, else written into
 * its record directly.
 */
EXPORT ER dt_place_in( CONST TS_UUID *box, CONST TS_UUID *u, CONST UB *name )
{
	T_TAD	*doc;
	T_VOBJ	*v, *e;
	TS_UUID	id;
	INT	i, n, bottom = 8;
	ER	er;

	if ( place_in_window(dt_showing(box), u, name) ) {
		return E_OK;
	}
	doc = om_store_get(box, 0);
	if ( doc == NULL ) {
		return E_NOEXS;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ) * 2);
	if ( v == NULL ) {
		return E_NOMEM;
	}
	e = v + 1;
	n = tad_lnk_count(doc);
	for ( i = 0; i < n; i++ ) {
		if ( tad_lnk_get(doc, i, e) >= E_OK && e->bottom + 8 > bottom ) {
			bottom = e->bottom + 8;
		}
	}
	link_of(v, u, name);
	v->left = 16;
	v->right = v->left + 200;
	v->top = bottom;
	v->bottom = bottom + 24;
	er = tad_lnk_add(doc, v, &id);
	if ( er >= E_OK ) {
		er = om_store_save(box, 0);
	}
	Kfree(v);
	return er;
}

/*
 * The panels of 印刷 (印刷設定, and 印刷詳細設定 and 画面表示 from it)
 * worked for a job, the setting they leave in 'set': E_OK when 印刷 was
 * pressed, E_ABORT when the panel was put away. Without 'show' the
 * panel has no 画面表示, for pages that are not there yet.
 */
EXPORT ER dt_print_ask( DTWIN *host, CONST T_PRNJOB *job, T_PRNSET *set, BOOL show )
{
	UINT	ans;

	if ( pr_last_ok ) {
		*set = pr_last;
		set->all = TRUE;
	} else {
		prn_default(set);
	}
	for ( ;; ) {
		ans = print_panel(host, job, set, show);
		if ( ans == ANS_DETAIL ) {
			(void)detail_panel(host, job, set);
			continue;
		}
		if ( ans == ANS_SHOW ) {
			preview(host, job, set);
			continue;
		}
		break;
	}
	pr_last = *set;
	pr_last_ok = TRUE;

	return ( ans == WM_ANS_OK ) ? E_OK : E_ABORT;
}

/*
 * 印刷 of a job: the panels, the pages shown when asked, and the file
 * written and put on the desktop. Answers E_OK when it was written,
 * E_ABORT when the panel was put away.
 */
EXPORT ER dt_print( DTWIN *host, T_PRNJOB *job )
{
	T_PRNSET	set;

	if ( job == NULL || job->npage <= 0 ) {
		dt_tell(host, "印刷するページがありません", NULL);
		return E_NOEXS;
	}
	if ( dt_print_ask(host, job, &set, TRUE) < E_OK ) {
		return E_ABORT;
	}
	return dt_print_out(host, job, &set);
}

/* The file of a job written as 'set' says and put on the desktop */
EXPORT ER dt_print_out( DTWIN *host, CONST T_PRNJOB *job, CONST T_PRNSET *set )
{
	TS_UUID		u;
	ER		er;
	T_PRNSTAT	st;
	UB		msg[WM_LABEL_MAX], a[12];
	UB		fname[TAD_NAME_MAX];
	INT		k, i;

	if ( prn_sheets(job, set) <= 0 ) {
		dt_tell(host, "印刷するページがありません", "ページの範囲を確かめてください");
		return E_NOEXS;
	}
	(void)wm_msg_put("印刷しています");
	wm_update();
	pr_open_set(set);
	er = prn_object(job, set, NULL, &u);
	prn_stat(&st);
	tm_printf((UB *)"print: %d sheets, %d faces (%d glyphs), %d pictures, %d patterns, %d bytes (%d)\n",
		  st.pages, st.faces, st.glyphs, st.images, st.patterns, (INT)st.bytes, (INT)er);
	if ( er < E_OK ) {
		(void)wm_msg_put("印刷できませんでした");
		return er;
	}
	/* the name the object was given: the job's and ".pdf" */
	for ( k = 0, i = 0; job->name != NULL && job->name[i] != 0 && k < TAD_NAME_MAX - 6; i++ ) {
		fname[k++] = job->name[i];
	}
	fname[k] = 0;
	if ( om_store_name(&u, fname, TAD_NAME_MAX) < 0 ) {
		put_words(fname, TAD_NAME_MAX, NULL, job->name, ".pdf");
	}
	dt_place_on_desktop(&u, fname);
	num_text(st.pages, a);
	put_words(msg, WM_LABEL_MAX, "「", fname, "」を作りました (");
	put_words(msg + knl_strlen((CONST char *)msg), WM_LABEL_MAX - (INT)knl_strlen((CONST char *)msg),
		  NULL, a, "ページ)");
	(void)wm_msg_put((CONST char *)msg);
	wm_update();

	return E_OK;
}

/* 印刷 of a window of the desktop's: its document or its figure */
EXPORT void dt_print_window( DTWIN *d )
{
	DTPRSRC		s;
	T_PRNJOB	job;
	UB		name[TAD_NAME_MAX];
	INT		k;

	if ( d == NULL || ( d->doc == NULL && d->fig == NULL ) ) {
		return;
	}
	for ( k = 0; d->name[k] != 0 && k < TAD_NAME_MAX - 1; k++ ) {
		name[k] = d->name[k];
	}
	name[k] = 0;
	if ( k == 0 ) {
		(void)om_store_name(&d->id, name, TAD_NAME_MAX);
	}
	/* the links are drawn or not as the setting last made says; the panel can change it */
	if ( d->doc != NULL ) {
		dt_print_src_doc(&s, &job, d->doc, d->rec, d->paper, name,
				 pr_last_ok ? pr_last.links : TRUE);
	} else {
		dt_print_src_fig(&s, &job, d->fig, d->rec, d->paper, name,
				 pr_last_ok ? pr_last.links : TRUE);
	}
	(void)dt_print(d, &job);
	dt_print_src_free(&s);
	dt_draw(d);
	wm_update();
}

/*
 * 基本印刷 (the 実行 menu of a link): the object the link points at
 * printed without being opened in a window -- its document, or its
 * figure -- with the panels over the window the link is in.
 */
EXPORT void dt_print_vobj( DTWIN *from, CONST T_VOBJ *v )
{
	DTWIN		*host = ( from != NULL ) ? from : dt_first_win();
	T_TAD		*rec = om_store_get(&v->target, ( v->recno > 0 ) ? v->recno : 0);
	T_TVDOC		*d = NULL;
	T_TVFIG		*f = NULL;
	DTPRSRC		s;
	T_PRNJOB	job;
	UB		name[TAD_NAME_MAX];
	UW		paper = TV_PAPER_LOOK;

	if ( rec == NULL || host == NULL ) {
		(void)wm_msg_put("実身を読めませんでした");
		return;
	}
	if ( om_store_name(&v->target, name, TAD_NAME_MAX) < 0 ) {
		name[0] = 0;
	}
	(void)om_store_paper(&v->target, &paper);
	if ( tv_kind(rec) == TV_KIND_DOC && tv_doc(rec, &d) >= E_OK && d != NULL ) {
		dt_print_src_doc(&s, &job, d, rec, paper, name, pr_last_ok ? pr_last.links : TRUE);
	} else if ( tv_kind(rec) == TV_KIND_FIG && tv_fig(rec, &f) >= E_OK && f != NULL ) {
		dt_print_src_fig(&s, &job, f, rec, paper, name, pr_last_ok ? pr_last.links : TRUE);
	} else {
		dt_tell(host, "この実身は基本印刷できません", "文章か図形の実身を選んでください");
		return;
	}
	(void)dt_print(host, &job);
	dt_print_src_free(&s);
	if ( d != NULL ) tv_doc_free(d);
	if ( f != NULL ) tv_fig_free(f);
	dt_draw_all();
	wm_update();
}

/*
 * DT_RQ_PRINT: the pages a process made, in the object 'target' it made
 * for them, printed as a window of the desktop's is, the panels in the
 * middle of the process's window. The object is the process's to throw
 * away afterwards.
 */
EXPORT ER dt_print_spool( CONST T_DTREQ *rq, T_DTANS *an )
{
	DTWIN		*host = dt_host_of(&rq->win);
	T_PRNJOB	job;
	SPOOL		*sp;
	UB		name[sizeof(rq->name) + 1];
	UW		paper;
	INT		k;
	ER		er;

	for ( k = 0; k < (INT)sizeof(rq->name) && rq->name[k] != 0; k++ ) {
		name[k] = rq->name[k];
	}
	name[k] = 0;
	paper = ( ( rq->flags & DT_F_PR_PAPER ) != 0 ) ? (UW)rq->look.bgcol : 0x00FFFFFFU;
	if ( paper == TV_PAPER_LOOK ) {
		paper = wm_look(WM_LOOK_WORK);		/* the paper of the scheme, as the window shows it */
	}
	sp = spool_read(&rq->target, &job, name, paper,
			rq->flags & ( DT_F_PR_DETAIL | DT_F_PR_HIDDEN ), pr_last_ok ? pr_last.links : TRUE);
	if ( sp == NULL ) {
		return E_NOEXS;
	}
	wm_obj_quiet(TRUE);		/* the keys that answer the panels are theirs, not the program's */
	er = dt_print(host, &job);
	wm_obj_quiet(FALSE);
	spool_free(sp);
	(void)an;

	return er;
}

/* The same pages as a job, without the panels: what the tests print */
EXPORT void *dt_spool_open( CONST TS_UUID *obj, T_PRNJOB *job, CONST UB *name )
{
	return spool_read(obj, job, name, 0x00FFFFFFU, 0, TRUE);
}

EXPORT void dt_spool_close( void *sp )
{
	if ( sp != NULL ) {
		spool_free((SPOOL *)sp);
	}
}
