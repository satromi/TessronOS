/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	print.c
 *	印刷: a program's pages laid on the printing paper and written as a
 *	PDF file (design 11.22)
 *
 *	The layout paper (レイアウト用紙) is what the program lays its pages
 *	out on; the printing paper (印刷用紙) is what they are printed on.
 *	Unless the printing paper is set, it is the layout paper and nothing
 *	is magnified. When it is set, 自動 magnifies each page by as much as
 *	lets the whole page fit on one sheet and puts it in the middle; a
 *	magnification given in percent is used as it is from the sheet's
 *	top left corner, and what goes past the sheet's edge is not printed.
 *
 *	Each page is drawn once, however many copies are asked for: the
 *	sheets of the later copies name the same drawing.
 *
 *	A file is made in memory and then taken in as a real object the way
 *	a file from outside is (xf_import), its bytes in record 1.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>
#include <ts/dt.h>
#include <ts/img.h>
#include <ts/xf.h>
#include <ts/print.h>
#include "prn_local.h"

LOCAL T_PRNSTAT	prn_last;

EXPORT void prn_default( T_PRNSET *set )
{
	knl_memset(set, 0, sizeof(*set));
	set->all = TRUE;
	set->from = 1;
	set->to = PRN_PAGE_MAX;
	set->copies = 1;
	set->quality = PRN_Q_NORMAL;
	set->colour = TRUE;
	set->scale = PRN_SCALE_AUTO;
	set->links = TRUE;
	set->expand = FALSE;
	set->depth = 1;
}

/* The pages in range: the first, from 0, and how many */
LOCAL INT page_range( CONST T_PRNJOB *job, CONST T_PRNSET *set, INT *p_first )
{
	INT	a = 1, b = job->npage;

	if ( !set->all ) {
		a = ( set->from > 1 ) ? set->from : 1;
		if ( set->to < b ) {
			b = set->to;
		}
	}
	*p_first = a - 1;

	return ( b >= a ) ? b - a + 1 : 0;
}

LOCAL INT copies_of( CONST T_PRNSET *set )
{
	return ( set->copies < 1 ) ? 1 : ( set->copies > PRN_COPY_MAX ) ? PRN_COPY_MAX : set->copies;
}

EXPORT INT prn_sheets( CONST T_PRNJOB *job, CONST T_PRNSET *set )
{
	INT	first;

	return page_range(job, set, &first) * copies_of(set);
}

EXPORT INT prn_sheet_page( CONST T_PRNJOB *job, CONST T_PRNSET *set, INT k )
{
	INT	first, n = page_range(job, set, &first);

	return ( n > 0 ) ? first + k % n : 0;
}

EXPORT void prn_paper( CONST T_PRNJOB *job, CONST T_PRNSET *set, INT *p_w, INT *p_h,
		       INT *p_pct100 )
{
	D	lw = (D)job->lw * job->unit, lh = (D)job->lh * job->unit;
	D	pw = lw, ph = lh, pct;

	if ( set->paper_w > 0 && set->paper_h > 0 ) {
		pw = (D)set->paper_w * 72000 / 254;
		ph = (D)set->paper_h * 72000 / 254;
	}
	if ( set->scale != PRN_SCALE_AUTO ) {
		pct = (D)set->scale * 100;
	} else if ( lw <= 0 || lh <= 0 ) {
		pct = 10000;
	} else {
		D	a = pw * 10000 / lw, b = ph * 10000 / lh;

		pct = ( a < b ) ? a : b;
	}
	*p_w = (INT)pw;
	*p_h = (INT)ph;
	*p_pct100 = (INT)pct;
}

/*
 * Where the layout page lies on the sheet: x' = k x + e, y' = f - k y
 * in millionths of a point, the sheet's own space going up from its
 * foot as PDF has it.
 */
LOCAL void placing( CONST T_PRNJOB *job, CONST T_PRNSET *set, D *p_k, D *p_e, D *p_f,
		    INT *p_pw, INT *p_ph )
{
	INT	pw, ph, pct;
	D	k, sw, sh, ex = 0, ey = 0;

	prn_paper(job, set, &pw, &ph, &pct);
	k = (D)job->unit * pct / 10;			/* millionths of a point to a pixel */
	sw = (D)job->lw * k / 1000;			/* thousandths of a point */
	sh = (D)job->lh * k / 1000;
	if ( set->scale == PRN_SCALE_AUTO ) {
		ex = ( (D)pw - sw ) / 2;
		ey = ( (D)ph - sh ) / 2;
		if ( ex < 0 ) ex = 0;
		if ( ey < 0 ) ey = 0;
	}
	*p_k = k;
	*p_e = ex * 1000;
	*p_f = ( (D)ph - ey ) * 1000;
	*p_pw = pw;
	*p_ph = ph;
}

/* ---------------------------------------------------------------- the file */

LOCAL void put_date( PBUF *b )
{
	TS_TIME	t = 0;
	TS_TM	tm;
	INT	v[6], i;

	if ( dt_gettime(&t) < E_OK || dt_localtime(&t, &tm) < E_OK ) {
		return;
	}
	v[0] = tm.tm_year + 1900;
	v[1] = tm.tm_mon + 1;
	v[2] = tm.tm_mday;
	v[3] = tm.tm_hour;
	v[4] = tm.tm_min;
	v[5] = tm.tm_sec;
	pb_str(b, " /CreationDate (D:");
	for ( i = 0; i < 6; i++ ) {
		if ( i > 0 && v[i] < 10 ) {
			pb_str(b, "0");
		}
		pb_int(b, v[i]);
	}
	pb_str(b, ")");
}

LOCAL ER build( CONST T_PRNJOB *job, CONST T_PRNSET *set, PRNDOC *doc )
{
	INT		first, n = page_range(job, set, &first), copies = copies_of(set);
	INT		*cont = NULL, *pages = NULL, root, ptree, info, i, c;
	INT		pw, ph, nsheet = n * copies;
	PBUF		b;
	PDFFACE		*ff;
	PRNRES		*r;
	ER		er;

	knl_memset(doc, 0, sizeof(*doc));
	knl_memset(&prn_last, 0, sizeof(prn_last));
	if ( job == NULL || job->draw == NULL || job->lw <= 0 || job->lh <= 0 || job->unit <= 0 ) {
		return E_PAR;
	}
	if ( n <= 0 ) {
		return E_NOEXS;			/* no page in the range asked for */
	}
	er = pdf_open(&doc->pdf);
	if ( er < E_OK ) {
		return er;
	}
	doc->gray = (BOOL)!set->colour;
	doc->quality = set->quality;
	doc->level = ( set->quality == PRN_Q_FAST ) ? 1 : ( set->quality == PRN_Q_FINE ) ? 9 : 6;
	placing(job, set, &doc->k, &doc->e, &doc->f, &pw, &ph);
	root = pdf_new_obj(&doc->pdf);
	ptree = pdf_new_obj(&doc->pdf);
	info = pdf_new_obj(&doc->pdf);
	doc->res_obj = pdf_new_obj(&doc->pdf);
	cont = (INT *)Kmalloc(sizeof(INT) * (SZ)n);
	pages = (INT *)Kmalloc(sizeof(INT) * (SZ)nsheet);
	if ( cont == NULL || pages == NULL ) {
		er = E_NOMEM;
		goto done;
	}

	/* each page drawn once */
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		PRNPAGE		pg;
		T_DPRECT	sheet;
		INT		gid;

		prn_page_begin(&pg, doc, job->lw, job->lh);
		pb_str(&pg.c, "q ");
		prn_micro(&pg.c, doc->k);
		pb_str(&pg.c, " 0 0 ");
		prn_micro(&pg.c, -doc->k);
		pb_str(&pg.c, " ");
		prn_micro(&pg.c, doc->e);
		pb_str(&pg.c, " ");
		prn_micro(&pg.c, doc->f);
		pb_str(&pg.c, " cm\n");
		gid = dp_open_rec(&prn_rec_ops, &pg, job->lw, job->lh);
		if ( gid < 0 ) {
			pb_free(&pg.c);
			er = (ER)gid;
			break;
		}
		sheet.left = 0;
		sheet.top = 0;
		sheet.right = job->lw;
		sheet.bottom = job->lh;
		(void)job->draw(job->arg, gid, first + i, &sheet);
		dp_close(gid);
		prn_page_end(&pg);
		pb_str(&pg.c, "Q\n");
		cont[i] = pdf_new_obj(&doc->pdf);
		er = pg.c.fail ? E_NOMEM : pdf_stream(&doc->pdf, cont[i], NULL, pg.c.p, pg.c.n, TRUE,
						    doc->level);
		pb_free(&pg.c);
	}
	if ( er < E_OK ) {
		goto done;
	}

	/* the sheets: every copy names the same drawings */
	for ( c = 0; c < copies; c++ ) {
		for ( i = 0; i < n; i++ ) {
			INT	o = pdf_new_obj(&doc->pdf);

			pages[c * n + i] = o;
			pdf_begin_obj(&doc->pdf, o);
			pb_init(&b);
			pb_str(&b, "<< /Type /Page /Parent ");
			pb_int(&b, ptree);
			pb_str(&b, " 0 R /MediaBox [ 0 0 ");
			pb_milli(&b, pw);
			pb_str(&b, " ");
			pb_milli(&b, ph);
			pb_str(&b, " ] /Resources ");
			pb_int(&b, doc->res_obj);
			pb_str(&b, " 0 R /Contents ");
			pb_int(&b, cont[i]);
			pb_str(&b, " 0 R");
			if ( set->rotate ) {
				pb_str(&b, " /Rotate 90");
			}
			pb_str(&b, " >>");
			pdf_buf(&doc->pdf, &b);
			pb_free(&b);
			pdf_end_obj(&doc->pdf);
		}
	}
	pdf_begin_obj(&doc->pdf, ptree);
	pb_init(&b);
	pb_str(&b, "<< /Type /Pages /Count ");
	pb_int(&b, nsheet);
	pb_str(&b, " /Kids [");
	for ( i = 0; i < nsheet; i++ ) {
		pb_str(&b, " ");
		pb_int(&b, pages[i]);
		pb_str(&b, " 0 R");
	}
	pb_str(&b, " ] >>");
	pdf_buf(&doc->pdf, &b);
	pb_free(&b);
	pdf_end_obj(&doc->pdf);

	/* the faces, now that it is known which glyphs of them were used */
	for ( ff = doc->faces; ff != NULL && er >= E_OK; ff = ff->next ) {
		INT	g;

		if ( ff->used == NULL ) {
			continue;
		}
		er = pdf_face_write(&doc->pdf, ff);
		prn_last.faces++;
		for ( g = 0; g < ff->nglyph; g++ ) {
			if ( ( ff->used[g >> 3] & ( 1 << ( g & 7 ) ) ) != 0 ) {
				prn_last.glyphs++;
			}
		}
	}
	if ( er < E_OK ) {
		goto done;
	}

	/* the one dictionary of what the pages name */
	pdf_begin_obj(&doc->pdf, doc->res_obj);
	pb_init(&b);
	pb_str(&b, "<< /ProcSet [ /PDF /Text /ImageB /ImageC ]");
	if ( doc->nface > 0 ) {
		pb_str(&b, " /Font <<");
		for ( ff = doc->faces; ff != NULL; ff = ff->next ) {
			if ( ff->used == NULL ) continue;
			pb_str(&b, " /F");
			pb_int(&b, ff->res);
			pb_str(&b, " ");
			pb_int(&b, ff->obj);
			pb_str(&b, " 0 R");
		}
		pb_str(&b, " >>");
	}
	if ( doc->nimg > 0 ) {
		pb_str(&b, " /XObject <<");
		for ( r = doc->imgs; r != NULL; r = r->next ) {
			pb_str(&b, " /Im");
			pb_int(&b, r->res);
			pb_str(&b, " ");
			pb_int(&b, r->obj);
			pb_str(&b, " 0 R");
		}
		pb_str(&b, " >>");
	}
	if ( doc->npat > 0 ) {
		pb_str(&b, " /Pattern <<");
		for ( r = doc->pats; r != NULL; r = r->next ) {
			pb_str(&b, " /P");
			pb_int(&b, r->res);
			pb_str(&b, " ");
			pb_int(&b, r->obj);
			pb_str(&b, " 0 R");
		}
		pb_str(&b, " >>");
	}
	pb_str(&b, " >>");
	pdf_buf(&doc->pdf, &b);
	pb_free(&b);
	pdf_end_obj(&doc->pdf);

	pdf_begin_obj(&doc->pdf, info);
	pb_init(&b);
	pb_str(&b, "<< /Producer (TessronOS)");
	if ( job->name != NULL && job->name[0] != 0 ) {
		pb_str(&b, " /Title ");
		pb_text16(&b, job->name);
	}
	put_date(&b);
	pb_str(&b, " >>");
	pdf_buf(&doc->pdf, &b);
	pb_free(&b);
	pdf_end_obj(&doc->pdf);

	pdf_begin_obj(&doc->pdf, root);
	pb_init(&b);
	pb_str(&b, "<< /Type /Catalog /Pages ");
	pb_int(&b, ptree);
	pb_str(&b, " 0 R /ViewerPreferences << /DisplayDocTitle true >> >>");
	pdf_buf(&doc->pdf, &b);
	pb_free(&b);
	pdf_end_obj(&doc->pdf);

	er = pdf_close(&doc->pdf, root, info);
	prn_last.pages = nsheet;
	prn_last.images = doc->nimg;
	prn_last.patterns = doc->npat;
	prn_last.bytes = doc->pdf.size;
done:
	if ( cont != NULL ) Kfree(cont);
	if ( pages != NULL ) Kfree(pages);

	return er;
}

EXPORT ER prn_pdf( CONST T_PRNJOB *job, CONST T_PRNSET *set, UB **p_bytes, SZ *p_len )
{
	PRNDOC	*doc;
	ER	er;
	UB	*out;

	if ( p_bytes == NULL || p_len == NULL || set == NULL ) {
		return E_PAR;
	}
	doc = (PRNDOC *)Kmalloc(sizeof(PRNDOC));
	if ( doc == NULL ) {
		return E_NOMEM;
	}
	er = build(job, set, doc);
	if ( er >= E_OK ) {
		out = (UB *)Kmalloc(doc->pdf.size + 1);
		if ( out == NULL ) {
			er = E_NOMEM;
		} else {
			pdf_rewind(&doc->pdf);
			(void)pdf_read(&doc->pdf, out, doc->pdf.size);
			*p_bytes = out;
			*p_len = doc->pdf.size;
		}
	}
	prn_doc_free(doc);
	Kfree(doc);

	return er;
}

LOCAL INT doc_read( void *ctx, void *buf, SZ n )
{
	return pdf_read(&((PRNDOC *)ctx)->pdf, buf, n);
}

EXPORT ER prn_object( CONST T_PRNJOB *job, CONST T_PRNSET *set, CONST TS_UUID *box,
		      TS_UUID *p_uuid )
{
	PRNDOC		*doc;
	T_XFSRC		src;
	UB		*name = NULL;
	INT		k = 0, i;
	TS_TIME		now = 0;
	ER		er;

	if ( p_uuid == NULL || set == NULL || job == NULL ) {
		return E_PAR;
	}
	doc = (PRNDOC *)Kmalloc(sizeof(PRNDOC));
	if ( doc == NULL ) {
		return E_NOMEM;
	}
	er = build(job, set, doc);
	if ( er >= E_OK ) {
		/* the job's name whole, and the ending that says what it is */
		INT	jl = 0;

		while ( job->name != NULL && job->name[jl] != 0 ) jl++;
		name = (UB *)Kmalloc((SZ)jl + 16);
		if ( name == NULL ) {
			er = E_NOMEM;
		}
	}
	if ( er >= E_OK ) {
		for ( i = 0; job->name != NULL && job->name[i] != 0; i++ ) {
			name[k++] = job->name[i];
		}
		if ( k == 0 ) {
			CONST char	*d = "印刷";

			for ( i = 0; d[i] != 0; i++ ) name[k++] = (UB)d[i];
		}
		name[k++] = '.';
		name[k++] = 'p';
		name[k++] = 'd';
		name[k++] = 'f';
		name[k] = 0;
		(void)dt_gettime(&now);
		knl_memset(&src, 0, sizeof(src));
		src.filename = name;
		src.source = "print";
		src.mtime = (D)now;
		src.read = doc_read;
		src.ctx = doc;
		pdf_rewind(&doc->pdf);
		er = xf_import(&src, box, p_uuid);
	}
	if ( name != NULL ) Kfree(name);
	prn_doc_free(doc);
	Kfree(doc);

	return er;
}

/* ---------------------------------------------------------------- 画面表示 */

/*
 * A sheet into pixels: the layout page drawn at its own size into
 * pixels of its own, then laid on the sheet at the size the sheet is
 * shown, each pixel of the sheet the mean of the page's pixels it
 * covers -- the letters read better that way than taken a pixel in
 * so many.
 */
EXPORT ER prn_sheet( CONST T_PRNJOB *job, CONST T_PRNSET *set, INT k, UW *px,
		     INT w, INT h )
{
	D		kk, e, f, sx, sy;
	INT		pw, ph, gid, x, y, lw = job->lw, lh = job->lh;
	UW		*page;
	T_DPRECT	all;

	if ( job == NULL || set == NULL || px == NULL || w <= 0 || h <= 0 || lw <= 0 || lh <= 0 ) {
		return E_PAR;
	}
	for ( x = 0; x < w * h; x++ ) {
		px[x] = 0x00FFFFFFU;
	}
	placing(job, set, &kk, &e, &f, &pw, &ph);
	if ( pw <= 0 || ph <= 0 ) {
		return E_PAR;
	}
	page = (UW *)Kmalloc(sizeof(UW) * (SZ)lw * lh);
	if ( page == NULL ) {
		return E_NOMEM;
	}
	for ( x = 0; x < lw * lh; x++ ) {
		page[x] = 0x00FFFFFFU;
	}
	gid = dp_open();
	if ( gid < 0 ) {
		Kfree(page);
		return (ER)gid;
	}
	all.left = 0;
	all.top = 0;
	all.right = lw;
	all.bottom = lh;
	dp_set_target(gid, page, (UINT)( lw * 4 ), 0, 0);
	dp_set_origin(gid, 0, 0);
	dp_set_frame(gid, &all);
	dp_set_visible(gid, &all);
	(void)job->draw(job->arg, gid, prn_sheet_page(job, set, k), &all);
	dp_close(gid);

	/*
	 * Each pixel of the sheet shown: where it is on the sheet in points,
	 * then where that is on the page; sx and sy are the page's pixels
	 * to one shown pixel, in 1/65536.
	 */
	sx = (D)pw * 1000 * 65536 / ( (D)w * kk );	/* page pixels per shown pixel */
	sy = (D)ph * 1000 * 65536 / ( (D)h * kk );
	for ( y = 0; y < h; y++ ) {
		/* the sheet's y from its top, in millionths of a point */
		D	ty = (D)y * ph * 1000 / h;
		D	py = ( f - (D)ph * 1000 + ty ) * 65536 / kk;	/* page y, 1/65536 */
		INT	y0 = (INT)( py >> 16 ), y1 = (INT)( ( py + sy ) >> 16 );

		if ( y1 <= y0 ) y1 = y0 + 1;
		for ( x = 0; x < w; x++ ) {
			D	tx = (D)x * pw * 1000 / w;
			D	pxx = ( tx - e ) * 65536 / kk;
			INT	x0 = (INT)( pxx >> 16 ), x1 = (INT)( ( pxx + sx ) >> 16 );
			UW	rs = 0, gs = 0, bs = 0, cnt = 0;
			INT	a, bb;

			if ( pxx < 0 || py < 0 || x0 >= lw || y0 >= lh ) {
				continue;
			}
			if ( x1 <= x0 ) x1 = x0 + 1;
			for ( bb = y0; bb < y1 && bb < lh; bb++ ) {
				for ( a = x0; a < x1 && a < lw; a++ ) {
					UW	c = page[(SZ)bb * lw + a];

					rs += ( c >> 16 ) & 0xFF;
					gs += ( c >> 8 ) & 0xFF;
					bs += c & 0xFF;
					cnt++;
				}
			}
			if ( cnt > 0 ) {
				UW	c = ( ( rs / cnt ) << 16 ) | ( ( gs / cnt ) << 8 ) | ( bs / cnt );

				px[(SZ)y * w + x] = set->colour ? c
					: ( ( ( ( ( c >> 16 ) & 0xFF ) * 299 + ( ( c >> 8 ) & 0xFF ) * 587
					      + ( c & 0xFF ) * 114 ) / 1000 ) * 0x010101U );
			}
		}
	}
	Kfree(page);

	return E_OK;
}

EXPORT void prn_stat( T_PRNSTAT *st )
{
	if ( st != NULL ) {
		*st = prn_last;
	}
}
