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
 *	印刷 as the desktop serves it (design 11.22): the pages of what is
 *	printed, the jobs 基本印刷 works, and the file put on the desktop as
 *	a real object
 *
 *	What is printed is pages of a layout paper drawn by whatever shows
 *	them: a document's pages by its own drawing (tv_doc_draw_page), a
 *	figure as the sheets of its paper laid side by side from its top
 *	left corner, left to right and then down, and the pages a process
 *	hands over as records of xmlTAD in an object made for the purpose
 *	(DT_RQ_PRINT). The layer below (prn_) lays them on the printing
 *	paper and writes the file.
 *
 *	The panels that say how (印刷設定, 印刷詳細設定) and the pages shown
 *	before they are written (画面表示) are 基本印刷's, a process
 *	(application/bprint) the desktop starts for each job; it asks the
 *	desktop (DT_RQ_PRJOB) for what only the desktop can do with the
 *	pages: lay them on the paper, draw a sheet in its window, write the
 *	file.
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
#include <ts/time.h>
#include <ts/proc.h>
#include <ts/fquit.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include "desktop.h"

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

/* ---------------------------------------------------------------- the file on the desktop */

/* A link to an object as a figure keeps one: its band, at the system's size; v made empty first */
LOCAL ER link_of( T_VOBJ *v, CONST TS_UUID *u, CONST UB *name )
{
	tad_vobj_clear(v);
	v->target = *u;
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
	return tad_vobj_set(v, TAD_W_NAME, name, -1);
}

/*
 * A link to an object put in the desktop's first window, under what is
 * there: by the program of the window, which saves it (DT_HOW_LINK_PUT);
 * written into the cabinet's record directly while the window is not up
 */
EXPORT void dt_place_on_desktop( CONST TS_UUID *u, CONST UB *name )
{
	(void)dt_place_on_desktop_appl(u, name, NULL);
}

/*
 * The same, the link given an applist of its own (applist; NULL or empty
 * none): told to the program of the window with the name and the applist
 * as parts of the answer's tail (DT_TAIL_NAME, DT_TAIL_APPLIST), each as
 * long as it is (dt_prog_tell_link)
 */
EXPORT ER dt_place_on_desktop_appl( CONST TS_UUID *u, CONST UB *name, CONST UB *applist )
{
	TS_UUID	cab;

	if ( dt_prog_tell_link(DT_TELL_FIRST, DT_HOW_LINK_PUT, u, name, applist) ) {
		return E_OK;
	}
	if ( ts_str_to_uuid(SYSDEF_CABINET, &cab) >= E_OK ) {
		return dt_place_in_appl(&cab, u, name, applist);
	}
	return E_NOEXS;
}

/*
 * A link to an object put in the figure 'box' (a volume's top object),
 * under what is there, written into its record directly: the first
 * window's by its program, as dt_place_on_desktop puts it.
 */
EXPORT ER dt_place_in( CONST TS_UUID *box, CONST TS_UUID *u, CONST UB *name )
{
	return dt_place_in_appl(box, u, name, NULL);
}

EXPORT ER dt_place_in_appl( CONST TS_UUID *box, CONST TS_UUID *u, CONST UB *name, CONST UB *applist )
{
	T_TAD	*doc;
	T_VOBJ	*v, *e;
	TS_UUID	id;
	INT	i, n, bottom = 8;
	ER	er;

	{
		TS_UUID	cab;

		if ( ts_str_to_uuid(SYSDEF_CABINET, &cab) >= E_OK && ts_uuid_cmp(&cab, box) == 0
		  && ( applist == NULL || applist[0] == 0 )
		  && dt_prog_tell_text(DT_TELL_FIRST, DT_HOW_LINK_PUT, u, name) ) {
			return E_OK;
		}
	}
	doc = om_store_get(box, 0);
	if ( doc == NULL ) {
		return E_NOEXS;
	}
	v = (T_VOBJ *)Kcalloc(2, sizeof(T_VOBJ));
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
	er = link_of(v, u, name);
	if ( er >= E_OK ) {
		er = tad_vobj_set(v, TAD_W_APPLIST, applist, -1);
	}
	v->left = 16;
	v->right = v->left + 200;
	v->top = bottom;
	v->bottom = bottom + 24;
	if ( er >= E_OK ) {
		er = tad_lnk_add(doc, v, &id);
	}
	if ( er >= E_OK ) {
		er = om_store_save(box, 0);
	}
	tad_vobj_clear(v);
	tad_vobj_clear(e);
	Kfree(v);
	return er;
}

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && at + 1 < max ) {
		b[at++] = (UB)*s++;
	}
	b[at] = 0;
	return at;
}

LOCAL INT put_num( UB *b, INT at, INT max, INT v )
{
	char	t[12];
	INT	n = 0;

	if ( v < 0 ) {
		at = put(b, at, max, "-");
		v = -v;
	}
	do {
		t[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && n < 11 );
	while ( n > 0 && at + 1 < max ) {
		b[at++] = (UB)t[--n];
	}
	b[at] = 0;
	return at;
}

/*
 * 印刷の指定 as the text DT_RQ_PRJOB carries it: thirteen numbers apart
 * by spaces, in the order of T_PRNSET
 */
LOCAL INT set_text( CONST T_PRNSET *s, UB *out, INT max )
{
	INT	v[13], i, at = 0;

	v[0] = s->all ? 1 : 0;
	v[1] = s->from;
	v[2] = s->to;
	v[3] = s->copies;
	v[4] = s->quality;
	v[5] = s->colour ? 1 : 0;
	v[6] = s->paper_w;
	v[7] = s->paper_h;
	v[8] = s->rotate ? 1 : 0;
	v[9] = s->scale;
	v[10] = s->links ? 1 : 0;
	v[11] = s->expand ? 1 : 0;
	v[12] = s->depth;
	out[0] = 0;
	for ( i = 0; i < 13; i++ ) {
		if ( i > 0 ) at = put(out, at, max, " ");
		at = put_num(out, at, max, v[i]);
	}
	return at;
}

/* The text read back, every number kept within what the panels allow; FALSE when it is not one */
LOCAL BOOL text_set( CONST UB *t, T_PRNSET *s )
{
	INT	v[13], k = 0, i = 0;

	while ( k < 13 ) {
		INT	x = 0;
		BOOL	neg = FALSE, any = FALSE;

		while ( t[i] == ' ' ) i++;
		if ( t[i] == '-' ) {
			neg = TRUE;
			i++;
		}
		while ( t[i] >= '0' && t[i] <= '9' && x < 10000000 ) {
			x = x * 10 + ( t[i++] - '0' );
			any = TRUE;
		}
		if ( !any ) {
			return FALSE;
		}
		v[k++] = neg ? -x : x;
	}
	s->all = (BOOL)( v[0] != 0 );
	s->from = ( v[1] < 1 ) ? 1 : ( v[1] > PRN_PAGE_MAX ) ? PRN_PAGE_MAX : v[1];
	s->to = ( v[2] < s->from ) ? s->from : ( v[2] > PRN_PAGE_MAX ) ? PRN_PAGE_MAX : v[2];
	s->copies = ( v[3] < 1 ) ? 1 : ( v[3] > PRN_COPY_MAX ) ? PRN_COPY_MAX : v[3];
	s->quality = ( v[4] == PRN_Q_FAST || v[4] == PRN_Q_FINE ) ? v[4] : PRN_Q_NORMAL;
	s->colour = (BOOL)( v[5] != 0 );
	if ( v[6] >= 300 && v[6] <= 10000 && v[7] >= 300 && v[7] <= 10000 ) {
		s->paper_w = v[6];
		s->paper_h = v[7];
	} else {
		s->paper_w = s->paper_h = 0;
	}
	s->rotate = (BOOL)( v[8] != 0 );
	s->scale = ( v[9] == PRN_SCALE_AUTO ) ? PRN_SCALE_AUTO : ( v[9] < 10 ) ? 10 : ( v[9] > 400 ) ? 400 : v[9];
	s->links = (BOOL)( v[10] != 0 );
	s->expand = (BOOL)( s->links && v[11] != 0 );
	s->depth = ( v[12] < 1 ) ? 1 : ( v[12] > PRN_DEPTH_MAX ) ? PRN_DEPTH_MAX : v[12];
	return TRUE;
}

/* ---------------------------------------------------------------- the file written */

/* What was set last time is where the panels start next time */
LOCAL T_PRNSET	pr_last;
LOCAL BOOL	pr_last_ok = FALSE;

/* The file of a job written as 'set' says and put on the desktop: some sheets are known to be there */
LOCAL ER print_write( CONST T_PRNJOB *job, CONST T_PRNSET *set )
{
	TS_UUID		u;
	ER		er;
	T_PRNSTAT	st;
	UB		*fname, *msg, num[12];

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
	/* the name the object was given, whole; else the job's and ".pdf" */
	fname = om_store_name_dup(&u, NULL);
	if ( fname == NULL ) {
		fname = dt_cat(( job->name != NULL ) ? (CONST char *)job->name : "", ".pdf", NULL);
	}
	if ( fname == NULL ) {
		(void)wm_msg_put("メモリが足りません");
		return E_NOMEM;
	}
	dt_place_on_desktop(&u, fname);
	(void)put_num(num, 0, sizeof(num), st.pages);
	msg = dt_cat("「", (CONST char *)fname, "」を作りました (", (CONST char *)num, "ページ)", NULL);
	(void)wm_msg_put(( msg != NULL ) ? (CONST char *)msg : "印刷しました");	/* the line shows what fits */
	if ( msg != NULL ) Kfree(msg);
	Kfree(fname);
	wm_update();

	return E_OK;
}

/* The file of a job written as 'set' says and put on the desktop */
EXPORT ER dt_print_out( DTWIN *host, CONST T_PRNJOB *job, CONST T_PRNSET *set )
{
	if ( prn_sheets(job, set) <= 0 ) {
		dt_tell(host, "印刷するページがありません", "ページの範囲を確かめてください");
		return E_NOEXS;
	}
	return print_write(job, set);
}

/* ---------------------------------------------------------------- the jobs 基本印刷 works */

/*
 * 基本印刷 is a process (application/bprint). What it prints is a job
 * the desktop holds: the pages a process made (DT_RQ_PRINT), an object
 * printed from the 実行 menu of a link (dt_print_vobj), or pages of the
 * desktop's own (a slide, 画面印刷: dt_print_start). The desktop starts
 * 基本印刷 for the job, and the process works the panels -- 印刷設定,
 * 印刷詳細設定, 画面表示 -- asking the desktop (DT_RQ_PRJOB) for what
 * only the desktop can do: how a setting lays the pages on the paper,
 * a sheet drawn into its window, and the file written and put on the
 * desktop. One job is worked at a time; one asked for meanwhile waits
 * its turn, as a program that asked waited while the panels were the
 * desktop's own.
 */
#define PJ_MAX		8
#define PJ_START_MS	60000		/* 基本印刷 has not asked in this long: it did not start */
#define PJ_POLL_MS	500		/* how often the process working the job is looked for */

#define PJ_SPOOL	1		/* the pages a process made */
#define PJ_OBJ		2		/* an object, printed without being opened */
#define PJ_OWN		3		/* the desktop's own pages */

#define PV_PAD		16		/* grey round the sheet in 画面表示 */

typedef struct {
	BOOL		used;
	BOOL		active;		/* the one 基本印刷 is started for */
	UINT		serial;		/* the order they were asked in */
	UINT		kind;		/* PJ_* */
	UINT		how;		/* DT_PRINT_SHOW, DT_PRINT_SET */
	T_PRNJOB	job;		/* PJ_SPOOL, PJ_OBJ: made here */
	T_PRNJOB	*own;		/* PJ_OWN: the caller's */
	void		(*done)( void *arg, ER er, CONST T_PRNSET *set );
	void		*arg;
	SPOOL		*sp;		/* PJ_SPOOL */
	TS_UUID		target;		/* PJ_SPOOL: the object of the pages; PJ_OBJ: the object */
	UW		paper;
	UINT		flags;		/* PJ_SPOOL: DT_F_PR_DETAIL, DT_F_PR_HIDDEN */
	DTPRSRC		src;		/* PJ_OBJ */
	CONST T_TAD	*rec;		/* as the store held it when the pages were made */
	INT		recno;
	T_TVDOC		*doc;
	T_TVFIG		*fig;
	BOOL		links;		/* the pages as made: with their links */
	UB		*name;		/* the job's, whole: pj_clear lets it go */
	TS_UUID		reply;		/* PJ_SPOOL: answered when the job ends */
	UINT		seq;
	SZ		anssz;
	ID		pid;		/* the 基本印刷 working it; 0 none yet */
	TS_UUID		puuid;
	UD		since;		/* when 基本印刷 was started for it, ms */
	UW		*px;		/* 画面表示: the sheet as last drawn */
	INT		pxk, pxw, pxh;
	T_PRNSET	pxset;
} PJOB;

LOCAL PJOB	pj[PJ_MAX];
LOCAL UINT	pj_serial = 0;
LOCAL UD	pj_looked = 0;

LOCAL UD ms_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000ULL;
}

LOCAL PJOB *pj_active( void )
{
	INT	i;

	for ( i = 0; i < PJ_MAX; i++ ) {
		if ( pj[i].used && pj[i].active ) return &pj[i];
	}
	return NULL;
}

LOCAL T_PRNJOB *pj_job( PJOB *J )
{
	return ( J->own != NULL ) ? J->own : &J->job;
}

/* A job's place made free */
LOCAL void pj_clear( PJOB *J )
{
	if ( J->name != NULL ) {
		Kfree(J->name);
	}
	knl_memset(J, 0, sizeof(*J));
}

LOCAL PJOB *pj_take( UINT kind, UINT how, CONST UB *name )
{
	UB	*own;
	INT	i;

	for ( i = 0; i < PJ_MAX; i++ ) {
		if ( !pj[i].used ) break;
	}
	if ( i == PJ_MAX ) {
		return NULL;
	}
	own = dt_cat(( name != NULL ) ? (CONST char *)name : "", NULL);
	if ( own == NULL ) {
		return NULL;
	}
	knl_memset(&pj[i], 0, sizeof(pj[i]));
	pj[i].name = own;
	pj[i].used = TRUE;
	pj[i].serial = ++pj_serial;
	pj[i].kind = kind;
	pj[i].how = how;
	pj[i].links = TRUE;
	return &pj[i];
}

/* An object's pages made from its record, with their links or without them */
LOCAL void pj_obj_pages( PJOB *J )
{
	dt_print_src_free(&J->src);
	if ( J->doc != NULL ) tv_doc_free(J->doc);
	if ( J->fig != NULL ) tv_fig_free(J->fig);
	J->doc = NULL;
	J->fig = NULL;
	knl_memset(&J->job, 0, sizeof(J->job));
	if ( J->rec == NULL ) {
		return;
	}
	if ( tv_kind(J->rec) == TV_KIND_DOC && tv_doc(J->rec, &J->doc) >= E_OK && J->doc != NULL ) {
		dt_print_src_doc(&J->src, &J->job, J->doc, J->rec, J->paper, J->name, J->links);
	} else if ( tv_kind(J->rec) == TV_KIND_FIG && tv_fig(J->rec, &J->fig) >= E_OK && J->fig != NULL ) {
		dt_print_src_fig(&J->src, &J->job, J->fig, J->rec, J->paper, J->name, J->links);
	} else {
		J->doc = NULL;
		J->fig = NULL;
	}
}

/*
 * An object's record as the store holds it now: the store lets a record
 * go when the object changes (om_store_forget), and the job, which may
 * wait its turn and be worked for a while, makes its pages again from
 * the record read anew rather than keep drawing from the one let go
 */
LOCAL void pj_fresh( PJOB *J )
{
	CONST T_TAD	*rec;

	if ( J->kind != PJ_OBJ ) {
		return;
	}
	rec = om_store_get(&J->target, J->recno);
	if ( rec == J->rec ) {
		return;
	}
	J->rec = rec;
	if ( J->px != NULL ) {
		Kfree(J->px);
		J->px = NULL;
	}
	pj_obj_pages(J);
}

/* The pages made again with their links or without them, as 仮身: 印刷 now says */
LOCAL void pj_links( PJOB *J, BOOL links )
{
	pj_fresh(J);
	if ( J->links == links || J->kind == PJ_OWN ) {
		return;
	}
	J->links = links;
	if ( J->px != NULL ) {
		Kfree(J->px);
		J->px = NULL;
	}
	if ( J->kind == PJ_SPOOL ) {
		SPOOL	*sp = spool_read(&J->target, &J->job, J->name, J->paper, J->flags, links);

		if ( sp != NULL ) {
			spool_free(J->sp);
			J->sp = sp;
		}
	} else {
		pj_obj_pages(J);
	}
}

/* Whether the process working the job is still there */
LOCAL BOOL pj_alive( CONST PJOB *J )
{
	TS_UUID	u;

	return (BOOL)( J->pid > 0 && knl_prc_uuid(J->pid, &u, NULL) >= E_OK && ts_uuid_cmp(&u, &J->puuid) == 0 );
}

LOCAL void pj_next( void );

/*
 * A job ended: the setting kept for the next time, the one who asked
 * told, and the next job begun. er E_OK: written (the setting taken,
 * for DT_PRINT_SET); E_ABORT: put away.
 */
LOCAL void pj_end( PJOB *J, ER er, CONST T_PRNSET *set )
{
	T_DTANS	an;
	UINT	serial = J->serial;

	if ( set != NULL ) {
		pr_last = *set;
		pr_last_ok = TRUE;
	}
	if ( J->sp != NULL ) {
		spool_free(J->sp);
		J->sp = NULL;
	}
	dt_print_src_free(&J->src);
	if ( J->doc != NULL ) tv_doc_free(J->doc);
	if ( J->fig != NULL ) tv_fig_free(J->fig);
	if ( J->px != NULL ) Kfree(J->px);
	if ( J->kind == PJ_SPOOL ) {
		/* the program that asked, which throws the object of its pages away now */
		knl_memset(&an, 0, sizeof(an));
		an.seq = J->seq;
		an.er = er;
		dt_answer(&J->reply, &an, J->anssz);
	}
	if ( J->done != NULL ) {
		J->done(J->arg, er, set);
	}
	pj_clear(J);
	tm_printf((UB *)"print: job %u ended (%d)\n", serial, (INT)er);
	wm_update();
	pj_next();
}

/* The job that has waited longest begun, when none is under way: 基本印刷 started for it */
LOCAL void pj_next( void )
{
	CONST DTPROG	*p;
	PJOB		*J = NULL;
	INT		i;
	ER		er;

	if ( pj_active() != NULL ) {
		return;
	}
	for ( i = 0; i < PJ_MAX; i++ ) {
		if ( pj[i].used && ( J == NULL || pj[i].serial < J->serial ) ) J = &pj[i];
	}
	if ( J == NULL ) {
		return;
	}
	J->active = TRUE;
	J->pid = 0;
	J->since = ms_now();
	p = dt_prog_find((CONST UB *)"basic-print");
	er = ( p != NULL && !p->builtin ) ? dt_prog_run(NULL, p, NULL) : E_NOEXS;
	tm_printf((UB *)"print: job %u of %d pages, 基本印刷 started (%d)\n", J->serial,
		  pj_job(J)->npage, (INT)er);
	if ( er < E_OK ) {
		(void)wm_msg_put("基本印刷を起動できませんでした");
		pj_end(J, er, NULL);
	}
}

/*
 * The windows of a process brought in front of the others, in the
 * order they stand, the nearest given the keys: a program asked for
 * again while it is up
 */
EXPORT void dt_raise_pid( ID pid )
{
	INT	room = 64, n, i, top = 0;
	INT	*w;

	for ( ;; ) {
		w = (INT *)Kmalloc(sizeof(INT) * (SZ)room);
		if ( w == NULL ) {
			return;
		}
		n = wm_list(w, room);
		if ( n < room ) break;
		Kfree(w);
		room *= 2;
	}
	for ( i = n - 1; i >= 0; i-- ) {
		if ( pid > 0 && wm_obj_maker(w[i]) == pid ) {
			wm_raise(w[i]);
			top = w[i];
		}
	}
	Kfree(w);
	if ( top > 0 ) {
		(void)wm_focus(top);
	}
	wm_update();
}

/*
 * A job of the desktop's own: job, and whatever it draws from, stay the
 * caller's until done is called -- E_OK when the file was written (with
 * DT_PRINT_SET: the setting chosen, in set, nothing written), E_ABORT
 * when the panel was put away, or why it could not be worked. done may
 * be called before this returns.
 */
EXPORT ER dt_print_start( T_PRNJOB *job, CONST TS_UUID *win, UINT how,
			  void (*done)( void *arg, ER er, CONST T_PRNSET *set ), void *arg )
{
	PJOB	*J;

	if ( job == NULL || job->npage <= 0 ) {
		dt_tell(dt_host_of(win), "印刷するページがありません", NULL);
		return E_NOEXS;
	}
	J = pj_take(PJ_OWN, how, job->name);
	if ( J == NULL ) {
		return E_LIMIT;
	}
	J->own = job;
	J->done = done;
	J->arg = arg;
	pj_next();
	return E_OK;
}

/*
 * 基本印刷 (the 実行 menu of a link): the object the link points at
 * printed without being opened in a window -- its document, or its
 * figure
 */
EXPORT void dt_print_vobj( DTWIN *from, CONST T_VOBJ *v )
{
	DTWIN		*host = ( from != NULL ) ? from : dt_first_win();
	CONST T_TAD	*rec = om_store_get(&v->target, ( v->recno > 0 ) ? v->recno : 0);
	PJOB		*J;
	UB		*name;
	UW		paper = TV_PAPER_LOOK;
	BOOL		links = pr_last_ok ? pr_last.links : TRUE;

	if ( rec == NULL || host == NULL ) {
		(void)wm_msg_put("実身を読めませんでした");
		return;
	}
	(void)om_store_paper(&v->target, &paper);
	if ( tv_kind(rec) != TV_KIND_DOC && tv_kind(rec) != TV_KIND_FIG ) {
		dt_tell(host, "この実身は基本印刷できません", "文章か図形の実身を選んでください");
		return;
	}
	name = om_store_name_dup(&v->target, NULL);	/* the job keeps its own copy */
	J = pj_take(PJ_OBJ, DT_PRINT_SHOW, name);
	if ( name != NULL ) Kfree(name);
	if ( J == NULL ) {
		(void)wm_msg_put("印刷を待っているものが多すぎます");
		return;
	}
	J->target = v->target;
	J->recno = ( v->recno > 0 ) ? v->recno : 0;
	J->paper = paper;
	J->rec = rec;
	J->links = links;
	pj_obj_pages(J);
	if ( J->job.npage <= 0 ) {
		dt_tell(host, ( J->doc == NULL && J->fig == NULL ) ? "この実身は基本印刷できません"
								  : "印刷するページがありません", NULL);
		dt_print_src_free(&J->src);
		if ( J->doc != NULL ) tv_doc_free(J->doc);
		if ( J->fig != NULL ) tv_fig_free(J->fig);
		pj_clear(J);
		return;
	}
	pj_next();
}

/*
 * DT_RQ_PRINT: the pages a process made, in the object 'target' it made
 * for them, printed as a window of the desktop's printed its own. The
 * answer is the job's, written when it ends (DT_ANS_LATER: not here);
 * the object is the process's to throw away after it.
 */
EXPORT ER dt_print_spool( CONST T_DTREQ *rq, SZ anssz )
{
	PJOB		*J;
	SPOOL		*sp;
	CONST UB	*name = dt_rq_word(rq, DT_TAIL_NAME, NULL);
	UW		paper;
	BOOL		links = pr_last_ok ? pr_last.links : TRUE;

	paper = ( ( rq->flags & DT_F_PR_PAPER ) != 0 ) ? (UW)rq->look.bgcol : 0x00FFFFFFU;
	if ( paper == TV_PAPER_LOOK ) {
		paper = wm_look(WM_LOOK_WORK);		/* the paper of the scheme, as the window shows it */
	}
	J = pj_take(PJ_SPOOL, DT_PRINT_SHOW, name);
	if ( J == NULL ) {
		return E_LIMIT;
	}
	sp = spool_read(&rq->target, &J->job, J->name, paper, rq->flags & ( DT_F_PR_DETAIL | DT_F_PR_HIDDEN ), links);
	if ( sp == NULL ) {
		pj_clear(J);
		return E_NOEXS;
	}
	J->sp = sp;
	J->target = rq->target;
	J->paper = paper;
	J->flags = rq->flags & ( DT_F_PR_DETAIL | DT_F_PR_HIDDEN );
	J->links = links;
	J->reply = rq->reply;
	J->seq = rq->seq;
	J->anssz = anssz;
	pj_next();

	return DT_ANS_LATER;
}

/* Whether two settings are one */
LOCAL BOOL same_set( CONST T_PRNSET *a, CONST T_PRNSET *b )
{
	UB	x[DT_TEXT_MAX], y[DT_TEXT_MAX];
	INT	i;

	(void)set_text(a, x, sizeof(x));
	(void)set_text(b, y, sizeof(y));
	for ( i = 0; x[i] != 0 && x[i] == y[i]; i++ ) ;
	return (BOOL)( x[i] == y[i] );
}

/* Whether the process is a 基本印刷, and which */
LOCAL BOOL is_bprint( ID pid, TS_UUID *p_uuid )
{
	TS_UUID	prog, bp;

	return (BOOL)( pid > 0 && knl_prc_uuid(pid, p_uuid, &prog) >= E_OK
		       && ts_str_to_uuid(SYSDEF_PROG_BPRINT, &bp) >= E_OK && ts_uuid_cmp(&prog, &bp) == 0 );
}

/*
 * 画面表示: sheet k drawn in the work area of the process's window
 * below 'top', the whole of it in the middle, or (zoom) twice as large
 * about (cx, cy) of it, on grey; the sheet drawn last kept for the next
 * time it is asked with the same setting
 */
LOCAL ER pj_show( PJOB *J, CONST T_DTREQ *rq, T_DTANS *an, ID pid, CONST T_PRNSET *set )
{
	CONST T_PRNJOB	*job = pj_job(J);
	T_WMWIN		w;
	T_DPRECT	r;
	ID		maker = 0, forp = 0;
	INT		wid = wm_obj_wid(&rq->win), gid, ww, wh, top = rq->grab_y, k = rq->grab_x;
	INT		pw, ph, pct, aw, ah, sw, sh, x, y, sx = 0, sy = 0, cw, ch, sheets;
	BOOL		zoom = (BOOL)( ( rq->flags & DT_F_PJ_ZOOM ) != 0 );

	if ( wid <= 0 || wm_ref(wid, &w) < E_OK ) {
		return E_NOEXS;
	}
	/* the window has to be the asking process's own */
	if ( knl_wmobj_who(wid, TMO_FEVR, &maker, &forp) < E_OK || maker != pid ) {
		return E_OACV;
	}
	sheets = prn_sheets(job, set);
	if ( k < 0 || k >= sheets ) {
		return E_PAR;
	}
	ww = w.work.right - w.work.left;
	wh = w.work.bottom - w.work.top;
	aw = ww - 2 * PV_PAD;
	ah = wh - top - 2 * PV_PAD;
	prn_paper(job, set, &pw, &ph, &pct);
	if ( pw <= 0 || ph <= 0 || aw <= 0 || ah <= 0 || top < 0 ) {
		return E_PAR;
	}
	/* the whole sheet in the area, its sides kept */
	sh = ah;
	sw = (INT)( (D)ah * pw / ph );
	if ( sw > aw ) {
		sw = aw;
		sh = (INT)( (D)aw * ph / pw );
	}
	if ( zoom ) {
		sw *= 2;
		sh *= 2;
	}
	if ( J->px == NULL || J->pxk != k || J->pxw != sw || J->pxh != sh
	  || !same_set(&J->pxset, set) ) {
		if ( J->px != NULL ) Kfree(J->px);
		J->px = (UW *)Kmalloc(sizeof(UW) * (SZ)sw * sh);
		if ( J->px == NULL ) {
			return E_NOMEM;
		}
		pr_open_set(set);
		(void)prn_sheet(job, set, k, J->px, sw, sh);
		J->pxk = k;
		J->pxw = sw;
		J->pxh = sh;
		J->pxset = *set;
		tm_printf((UB *)"print: preview of %d sheets, sheet %d, %d x %d shown\n", sheets, k + 1, sw, sh);
	}
	gid = wm_gid(wid);
	if ( gid < 0 ) {
		return E_NOEXS;
	}
	r.left = 0;
	r.top = top;
	r.right = ww;
	r.bottom = wh;
	dp_fill_rect(gid, &r, 0x00808080U);
	/* where the sheet goes: in the middle, or around the place magnified */
	if ( sw <= aw ) {
		x = PV_PAD + ( aw - sw ) / 2;
	} else {
		sx = rq->look.w - aw / 2;
		if ( sx < 0 ) sx = 0;
		if ( sx > sw - aw ) sx = sw - aw;
		x = PV_PAD;
	}
	if ( sh <= ah ) {
		y = top + PV_PAD + ( ah - sh ) / 2;
	} else {
		sy = rq->look.h - ah / 2;
		if ( sy < 0 ) sy = 0;
		if ( sy > sh - ah ) sy = sh - ah;
		y = top + PV_PAD;
	}
	cw = ( sw - sx < aw ) ? sw - sx : aw;
	ch = ( sh - sy < ah ) ? sh - sy : ah;
	dp_put_argb(gid, x, y, J->px + (SZ)sy * sw + sx, sw, cw, ch, 0xFFFFFFFFU);
	r.left = x - 1;
	r.top = y - 1;
	r.right = x + cw + 1;
	r.bottom = y + ch + 1;
	dp_frame_rect(gid, &r, 0x00303030U, 1);
	wm_update();
	an->look.w = sw;
	an->look.h = sh;
	an->how = (UINT)sheets;
	return E_OK;
}

/* DT_RQ_PRJOB: what 基本印刷 asks of the desktop for the job it works */
EXPORT ER dt_print_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	PJOB		*J = pj_active();
	T_PRNJOB	*job;
	T_PRNSET	set;
	TS_UUID		pu;
	UB		t[DT_TEXT_MAX];
	INT		w, h, pct;
	ER		er;

	if ( rq->recno == DT_PJ_JOB ) {
		if ( J == NULL ) {
			return E_NOEXS;
		}
		if ( J->pid != 0 && J->pid != pid && pj_alive(J) ) {
			dt_raise_pid(J->pid);	/* asked again: the one working it in front */
			return E_BUSY;
		}
		if ( !is_bprint(pid, &pu) ) {
			return E_OACV;
		}
		J->pid = pid;
		J->puuid = pu;
		if ( pr_last_ok ) {
			set = pr_last;
			set.all = TRUE;
		} else {
			prn_default(&set);
		}
		pj_links(J, set.links);
		job = pj_job(J);
		/* the setting, a tab, the job's name */
		(void)set_text(&set, t, sizeof(t));
		(void)dt_ans_cat(an, t, -1);
		(void)dt_ans_cat(an, (CONST UB *)"\t", 1);
		(void)dt_ans_cat(an, J->name, -1);
		an->how = (UINT)job->npage;
		an->flags = ( ( J->how & DT_PRINT_SHOW ) ? DT_F_PJ_SHOW : 0 ) | ( ( J->how & DT_PRINT_SET ) ? DT_F_PJ_SET : 0 );
		tm_printf((UB *)"print: job %u taken by process %d\n", J->serial, (INT)pid);
		return E_OK;
	}
	if ( J == NULL || J->pid != pid ) {
		return E_NOEXS;
	}
	if ( !text_set(dt_rq_word(rq, DT_TAIL_TEXT, NULL), &set) ) {
		return E_PAR;
	}
	pj_links(J, set.links);
	job = pj_job(J);
	switch ( rq->recno ) {
	case DT_PJ_PAPER:
		prn_paper(job, &set, &w, &h, &pct);
		(void)dt_ans_num(an, w);
		(void)dt_ans_cat(an, (CONST UB *)" ", 1);
		(void)dt_ans_num(an, h);
		(void)dt_ans_cat(an, (CONST UB *)" ", 1);
		(void)dt_ans_num(an, pct);
		(void)dt_ans_cat(an, (CONST UB *)" ", 1);
		(void)dt_ans_num(an, prn_sheets(job, &set));
		(void)dt_ans_cat(an, (CONST UB *)" ", 1);
		(void)dt_ans_num(an, job->lw * job->unit);
		(void)dt_ans_cat(an, (CONST UB *)" ", 1);
		(void)dt_ans_num(an, job->lh * job->unit);
		an->how = (UINT)job->npage;
		return E_OK;
	case DT_PJ_SHOW:
		return pj_show(J, rq, an, pid, &set);
	case DT_PJ_OUT:
		if ( J->how & DT_PRINT_SET ) {
			pj_end(J, E_OK, &set);
			return E_OK;
		}
		if ( prn_sheets(job, &set) <= 0 ) {
			return E_NOEXS;
		}
		er = print_write(job, &set);
		pj_end(J, ( er < E_OK ) ? er : E_OK, &set);
		return er;
	case DT_PJ_END:
		pj_end(J, E_ABORT, &set);
		return E_OK;
	default:
		return E_PAR;
	}
}

/*
 * The job under way followed: one whose 基本印刷 did not start, or went
 * (強制終了) before it was done, put away
 */
EXPORT void dt_print_poll( void )
{
	PJOB	*J = pj_active();
	UD	now;

	if ( J == NULL ) {
		return;
	}
	now = ms_now();
	if ( now - pj_looked < PJ_POLL_MS ) {
		return;
	}
	pj_looked = now;
	if ( J->pid == 0 ) {
		if ( now - J->since > PJ_START_MS ) {
			tm_printf((UB *)"print: 基本印刷 did not come for job %u\n", J->serial);
			pj_end(J, E_TMOUT, NULL);
		}
		return;
	}
	if ( !pj_alive(J) ) {
		tm_printf((UB *)"print: 基本印刷 gone, job %u put away\n", J->serial);
		pj_end(J, E_ABORT, NULL);
	}
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
