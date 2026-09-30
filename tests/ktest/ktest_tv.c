/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tv.c
 *	Documents and figures shown (design 17.5, phases 14 and 15)
 *
 *	The records here are real ones, byte for byte: an opening cabinet
 *	and sample documents as they are distributed. Reading anything else
 *	would test the reader against what this system expects rather than
 *	against what it will be given.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tad.h>
#include <ts/uuid.h>
#include <ts/tadview.h>
#include <ts/rx.h>
#include <ts/kconv.h>
#include <ts/img.h>
#include <ts/fs.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/docmenu.h>
#include <ts/mn.h>
#include "../../application/desktop/desktop.h"
#include <ts/hid.h>
#include <ts/fn.h>
#include <ts/om.h>
#include <ts/disp.h>
#include <ts/xf.h>
#include <ts/sysdef.h>

#define XT_DOC		"/boot/019a6c96-e262-7dfd-a3bc-1e85d495d60d_0.xtad"
#define XT_FIG		"/boot/019aaa4e-de25-72ad-8518-ab392b7ea301_0.xtad"
#define XT_CAB		"/boot/019a4370-3819-79a2-9758-881e1e0de90a_0.xtad"

LOCAL SZ s_len( CONST char *t )
{
	SZ	n = 0;

	while ( t[n] != 0 ) {
		n++;
	}

	return n;
}

LOCAL INT	wid = 0;
LOCAL BOOL	ready = FALSE;

/* A record read whole. NULL when it is not on the disk. */
LOCAL UB *slurp( CONST char *path, SZ *p_size )
{
	T_FSTAT	st;
	UB	*buf;
	INT	fd, n;
	SZ	got = 0;

	if ( fs_stat(path, &st) < EX_OK || st.size == 0 ) {
		return NULL;
	}
	buf = (UB *)Kmalloc((SZ)st.size + 1);
	if ( buf == NULL ) {
		return NULL;
	}
	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		Kfree(buf);
		return NULL;
	}
	while ( got < (SZ)st.size ) {
		n = fs_read(fd, buf + got, (INT)((SZ)st.size - got));
		if ( n <= 0 ) {
			break;
		}
		got += (SZ)n;
	}
	fs_close(fd);
	buf[got] = 0;
	if ( p_size != NULL ) {
		*p_size = got;
	}

	return buf;
}

/* ---------------------------------------------------------------- the store */

/*
 * What a link points at, out of the records on the disk. A real object
 * is a file named by its identity, with its metadata beside it: the
 * name is in the metadata, the content in the record.
 *
 * This is what the store does for a program in the finished system; it
 * is written out here so that the drawing can be tried against the real
 * records before the store is wired up to it.
 */
/*
 * What has been read already.
 *
 * A link is drawn every time the document it stands in is drawn, and
 * drawing it means knowing what the object is called and, if it is
 * open, what is in it. Going to the disk for that each time is a file
 * opened, a directory walked and a record parsed for every link on
 * every redraw, which on a page of twenty links came to some twenty
 * seconds -- more than everything else the drawing does together.
 *
 * So what is read is kept, under the identity it was read for. The
 * store owns it until it is emptied; a record that is open has its
 * bytes kept with it, because what was parsed points into them.
 */
#define OPEN_MAX	32

LOCAL struct {
	BOOL	used;
	TS_UUID	id;
	INT	recno;
	T_TAD	*doc;
	UB	*xml;
	UB	name[TAD_NAME_MAX];
	BOOL	named;			/* the name has been looked for */
	BOOL	has_name;		/* and was found */
} tv_open_tab[OPEN_MAX];

LOCAL BOOL same_id( CONST TS_UUID *a, CONST TS_UUID *b )
{
	CONST UB	*p = (CONST UB *)a;
	CONST UB	*q = (CONST UB *)b;
	UINT		i;

	for ( i = 0; i < sizeof(TS_UUID); i++ ) {
		if ( p[i] != q[i] ) {
			return FALSE;
		}
	}

	return TRUE;
}

/* The entry for one object, made if there is none and there is room */
LOCAL INT tv_slot( CONST TS_UUID *id, INT recno )
{
	INT	i, free = -1;

	for ( i = 0; i < OPEN_MAX; i++ ) {
		if ( !tv_open_tab[i].used ) {
			if ( free < 0 ) {
				free = i;
			}
			continue;
		}
		if ( same_id(&tv_open_tab[i].id, id)
		  && tv_open_tab[i].recno == recno ) {
			return i;
		}
	}
	if ( free < 0 ) {
		return -1;
	}
	tv_open_tab[free].used     = TRUE;
	tv_open_tab[free].id       = *id;
	tv_open_tab[free].recno    = recno;
	tv_open_tab[free].doc      = NULL;
	tv_open_tab[free].xml      = NULL;
	tv_open_tab[free].named    = FALSE;
	tv_open_tab[free].has_name = FALSE;

	return free;
}

/* Everything the store has read, given back */
LOCAL void tv_store_empty( void )
{
	INT	i;

	for ( i = 0; i < OPEN_MAX; i++ ) {
		if ( !tv_open_tab[i].used ) {
			continue;
		}
		if ( tv_open_tab[i].doc != NULL ) {
			tad_free(tv_open_tab[i].doc);
		}
		if ( tv_open_tab[i].xml != NULL ) {
			Kfree(tv_open_tab[i].xml);
		}
		tv_open_tab[i].used = FALSE;
		tv_open_tab[i].doc = NULL;
		tv_open_tab[i].xml = NULL;
	}
}

LOCAL void uuid_path( CONST T_VOBJ *v, CONST char *tail, INT recno, char *out )
{
	char	id[40];
	INT	i = 0, k;

	if ( ts_uuid_to_str(&v->target, id, sizeof(id)) < E_OK ) {
		out[0] = 0;
		return;
	}
	out[i++] = '/';  out[i++] = 'b';  out[i++] = 'o';  out[i++] = 'o';
	out[i++] = 't';  out[i++] = '/';
	for ( k = 0; id[k] != 0; k++ ) {
		out[i++] = id[k];
	}
	if ( recno >= 0 ) {
		out[i++] = '_';
		out[i++] = (char)('0' + (recno % 10));
	}
	for ( k = 0; tail[k] != 0; k++ ) {
		out[i++] = tail[k];
	}
	out[i] = 0;
}

/* The name out of the metadata: "name" is the first thing it says */
LOCAL INT tv_name_of( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	char	path[96];
	UB	*meta;
	SZ	size = 0;
	INT	i, n = 0, slot;

	slot = tv_slot(&v->target, -2);		/* -2: the metadata, not a record */
	if ( slot >= 0 && tv_open_tab[slot].named ) {
		if ( !tv_open_tab[slot].has_name ) {
			return -1;
		}
		for ( n = 0; n < max - 1 && tv_open_tab[slot].name[n] != 0; n++ ) {
			buf[n] = tv_open_tab[slot].name[n];
		}
		buf[n] = 0;
		return n;
	}
	uuid_path(v, ".json", -1, path);
	if ( path[0] == 0 ) {
		return -1;
	}
	meta = slurp(path, &size);
	if ( meta == NULL ) {
		if ( slot >= 0 ) {
			tv_open_tab[slot].named = TRUE;
		}
		return -1;
	}
	for ( i = 0; i + 8 < (INT)size; i++ ) {
		if ( meta[i] == '"' && meta[i + 1] == 'n' && meta[i + 2] == 'a'
		  && meta[i + 3] == 'm' && meta[i + 4] == 'e' && meta[i + 5] == '"' ) {
			INT	j = i + 6;

			while ( j < (INT)size && meta[j] != '"' ) {
				j++;		/* past the colon and the space */
			}
			j++;
			while ( j < (INT)size && meta[j] != '"' && n < max - 1 ) {
				buf[n++] = meta[j++];
			}
			break;
		}
	}
	buf[n] = 0;
	Kfree(meta);
	if ( slot >= 0 ) {
		INT	k;

		for ( k = 0; k < n && k < TAD_NAME_MAX - 1; k++ ) {
			tv_open_tab[slot].name[k] = buf[k];
		}
		tv_open_tab[slot].name[k] = 0;
		tv_open_tab[slot].named = TRUE;
		tv_open_tab[slot].has_name = (BOOL)( n > 0 );
	}

	return ( n > 0 ) ? n : -1;
}

LOCAL T_TAD *tv_open_of( CONST T_VOBJ *v, void *arg )
{
	char	path[96];
	UB	*xml;
	SZ	size = 0;
	T_TAD	*doc = NULL;
	INT	recno = ( v->recno >= 0 ) ? v->recno : 0;
	INT	slot;

	slot = tv_slot(&v->target, recno);
	if ( slot >= 0 && tv_open_tab[slot].doc != NULL ) {
		return tv_open_tab[slot].doc;		/* read already */
	}
	uuid_path(v, ".xtad", recno, path);
	if ( path[0] == 0 ) {
		return NULL;
	}
	xml = slurp(path, &size);
	if ( xml == NULL ) {
		return NULL;
	}
	if ( tad_parse(xml, size, NULL, &doc) < E_OK || doc == NULL ) {
		Kfree(xml);
		return NULL;
	}
	if ( slot < 0 ) {
		/* no room to remember it: it is read for this drawing alone,
		   and the bytes go with the tree that points into them */
		tad_free(doc);
		Kfree(xml);
		return NULL;
	}
	tv_open_tab[slot].doc = doc;
	tv_open_tab[slot].xml = xml;

	return doc;
}

/*
 * Done with for now. What was read stays in the store: the same link
 * will be drawn again on the next screen, and reading it again is the
 * whole of what made drawing slow.
 */
LOCAL void tv_shut_of( T_TAD *doc, void *arg )
{
	(void)doc;
	(void)arg;
}

LOCAL CONST T_TVSRC	tv_store = { tv_name_of, tv_open_of, tv_shut_of,
				     NULL, NULL };

LOCAL void start( void )
{
	T_DPRECT	o;

	if ( ready ) {
		return;
	}
	/*
	 * A face to draw with. The suite that sets the system's own runs
	 * only when the whole book is run, and a document drawn without
	 * letters is not a document; so one is opened here when there is
	 * none.
	 */
	if ( fn_system() <= 0 ) {
		ID	fid = 0;
		TS_UUID	u;
		ER	er;

		er = kt_sysobj(SYSDEF_FONT_BOX, "NotoSansJP-Regular.otf", &u);
		if ( er >= E_OK ) {
			er = fn_open_obj(&u, xf_data_rec(&u), 0, &fid);
		}
		tm_printf((UB *)"  face %d er %d\n", (INT)fid, (INT)er);
		if ( er >= E_OK && fid > 0 ) {
			fn_set_size(fid, 14);
			fn_set_system(fid);
		}
	}
	/*
	 * The ground. A window drawn over a plain colour and a window drawn
	 * over a picture are not the same window: what shows through a
	 * translucent frame is part of what is being looked at.
	 */
	{
		TS_UUID	u;
		ER	er = kt_sysobj(SYSDEF_WALL_BOX, "TESSRON.PIC", &u);

		if ( er >= E_OK ) {
			er = wm_load_wall_obj(&u, xf_data_rec(&u), WM_WALL_FIT);
		}
		tm_printf((UB *)"  ground %d\n", (INT)er);
	}

	o.left = 60;  o.top = 50;  o.right = 1060;  o.bottom = 760;
	wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE
			  | WM_ATTR_RBAR, "TADjs");
	if ( wid > 0 ) {
		ready = TRUE;
		/*
		 * The window holds the input. A window that does not is
		 * drawn with a flat bar and a pale band, which is right --
		 * and is not what a picture of the system should show.
		 */
		wm_focus(wid);
	}
	/* these pictures are of the system as a person sees it */
	wm_show_pointer(TRUE);
	tv_source(&tv_store, NULL);
}

/*
 * The introduction: a document of paragraphs, sizes, colours and
 * virtual objects standing in the text.
 */
LOCAL void test_doc( void )
{
	UB		*xml;
	SZ		size = 0;
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_DPRECT	r;
	INT		gid, h;

	xml = slurp(XT_DOC, &size);
	if ( xml == NULL ) KT_SKIP("no record on the disk (put one in etc/xtad/)");

	KT_ASSERT_ER(tad_parse(xml, size, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(xml); return; }

	KT_ASSERT_EQ((INT)tv_kind(doc), TV_KIND_DOC);
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); Kfree(xml); return; }

	/* what the record says it is */
	KT_ASSERT(model->npara > 10);
	KT_ASSERT(model->nrun > 20);
	KT_ASSERT_EQ(model->hunit, -72);

	/* the first paragraph is the heading: large, coloured, and heavy */
	{
		CONST T_TVRUN	*run = &model->run[model->para[0].first];

		KT_ASSERT(model->para[0].n > 0);
		KT_ASSERT_EQ(run->size, 28);
		KT_ASSERT_EQ((INT)run->colour, 0x0051AEFB);
		KT_ASSERT((run->style & TV_ST_BOLD) != 0);
	}

	/* the links in it are the ones the record holds */
	KT_ASSERT(tad_lnk_count(doc) > 20);
	{
		INT	i, links = 0;

		for ( i = 0; i < model->nrun; i++ ) {
			if ( model->run[i].kind == TV_RUN_LINK ) {
				links++;
			}
		}
		KT_ASSERT_EQ(links, tad_lnk_count(doc));
	}

	start();
	if ( ready ) {
		gid = wm_gid(wid);
		KT_ASSERT(gid >= 0);
		r.left = 10;  r.top = 10;  r.right = 780;  r.bottom = 620;
		{
			SYSTIM	t0, t1, t2, t3;
			INT	h2;

			/*
			 * Measuring and drawing, timed apart. Both walk the
			 * whole document; only the second puts pixels down,
			 * so the difference is what drawing itself costs and
			 * the first is what asking for widths costs.
			 */
			/*
			 * One line of letters, measured over and over with
			 * everything already made. What this costs says
			 * whether the time goes on making letters or on
			 * asking for ones that are already there.
			 */
			{
				CONST UB	*line = (CONST UB *)
					"ããã"
					"abcdefghijæå­";
				SYSTIM		a, b;
				INT		k, w = 0;

				(void)fn_width(fn_system(), line);
				tk_get_tim(&a);
				for ( k = 0; k < 100; k++ ) {
					w += fn_width(fn_system(), line);
				}
				tk_get_tim(&b);
				tm_printf((UB *)"  100 widths of 16 letters: %d ms (w %d)\n",
					  (INT)(b.lo - a.lo), (INT)w);
			}
			tk_get_tim(&t0);
			h2 = tv_doc_height(model, r.right - r.left, doc);
			tk_get_tim(&t1);
			h = tv_doc_draw(gid, model, &r, 0, doc, TV_PAPER_LOOK);
			tk_get_tim(&t2);
			(void)tv_doc_draw(gid, model, &r, 0, doc, TV_PAPER_LOOK);
			tk_get_tim(&t3);
			tm_printf((UB *)"  measure %d ms, draw %d ms, again %d ms\n",
				  (INT)(t1.lo - t0.lo), (INT)(t2.lo - t1.lo),
				  (INT)(t3.lo - t2.lo));
			KT_ASSERT_EQ(h2, h);
			fn_report();
		}
		KT_ASSERT(h > 100);			/* it came to a page of text */

		/*
		 * The bar says what part of the document is being shown.
		 * Without it the knob fills the track, which is what a bar
		 * means when there is nothing to scroll -- and says nothing
		 * about whether the arithmetic behind it is right.
		 */
		{
			T_WMBAR	b;

			b.lo  = 0;
			b.hi  = h;
			b.clo = 0;
			b.chi = r.bottom - r.top;
			wm_set_bar(wid, WM_BAR_R, &b);
		}
		KT_ASSERT_ER(wm_composite(), E_OK);
		kt_shot("/boot/DOC.PPM");
	}

	tv_doc_free(model);
	tad_free(doc);
	Kfree(xml);
}

/* A figure: shapes, a line with an arrow, pieces of text, and links */
LOCAL void test_fig( void )
{
	UB		*xml;
	SZ		size = 0;
	T_TAD		*doc = NULL;
	T_TVFIG		*model = NULL;
	T_DPRECT	r;
	INT		gid, i, lines = 0, docs = 0, links = 0, images = 0;

	xml = slurp(XT_FIG, &size);
	if ( xml == NULL ) KT_SKIP("no record on the disk");

	KT_ASSERT_ER(tad_parse(xml, size, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(xml); return; }
	KT_ASSERT_EQ((INT)tv_kind(doc), TV_KIND_FIG);
	KT_ASSERT_ER(tv_fig(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); Kfree(xml); return; }

	KT_ASSERT_EQ(model->view.right, 800);
	KT_ASSERT_EQ(model->view.bottom, 600);
	KT_ASSERT_EQ(model->hunit, -96);

	for ( i = 0; i < model->nsh; i++ ) {
		switch ( model->sh[i].kind ) {
		case TV_SH_LINE:	lines++;	break;
		case TV_SH_DOC:		docs++;		break;
		case TV_SH_LINK:	links++;	break;
		case TV_SH_IMAGE:	images++;	break;
		default:				break;
		}
	}
	KT_ASSERT_EQ(lines, 1);
	KT_ASSERT_EQ(images, 1);
	KT_ASSERT_EQ(docs, 2);
	KT_ASSERT_EQ(links, 2);

	/* the shapes are in the order the record numbers them */
	for ( i = 1; i < model->nsh; i++ ) {
		KT_ASSERT(model->sh[i].z >= model->sh[i - 1].z);
	}

	start();
	if ( ready ) {
		gid = wm_gid(wid);
		r.left = 10;  r.top = 10;  r.right = 780;  r.bottom = 620;
		KT_ASSERT_ER(tv_fig_draw(gid, model, &r, 0, 0, doc, TV_PAPER_LOOK), E_OK);
		KT_ASSERT_ER(wm_composite(), E_OK);
		kt_shot("/boot/FIG.PPM");
	}

	tv_fig_free(model);
	tad_free(doc);
	Kfree(xml);
}

/* The cabinet: a figure whose shapes are all virtual objects */
LOCAL void test_cabinet( void )
{
	UB		*xml;
	SZ		size = 0;
	T_TAD		*doc = NULL;
	T_TVFIG		*model = NULL;
	T_DPRECT	r;
	INT		gid, i, links = 0;

	xml = slurp(XT_CAB, &size);
	if ( xml == NULL ) KT_SKIP("no record on the disk");

	KT_ASSERT_ER(tad_parse(xml, size, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(xml); return; }
	KT_ASSERT_EQ((INT)tv_kind(doc), TV_KIND_FIG);
	KT_ASSERT_ER(tv_fig(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); Kfree(xml); return; }

	for ( i = 0; i < model->nsh; i++ ) {
		if ( model->sh[i].kind == TV_SH_LINK ) {
			links++;
			KT_ASSERT(model->sh[i].r.right > model->sh[i].r.left);
			KT_ASSERT(model->sh[i].r.bottom > model->sh[i].r.top);
		}
	}
	KT_ASSERT_EQ(links, tad_lnk_count(doc));
	KT_ASSERT(links > 10);

	start();
	if ( ready ) {
		gid = wm_gid(wid);
		r.left = 10;  r.top = 10;  r.right = 780;  r.bottom = 620;
		KT_ASSERT_ER(tv_fig_draw(gid, model, &r, 0, 0, doc, TV_PAPER_LOOK), E_OK);
		KT_ASSERT_ER(wm_composite(), E_OK);
		kt_shot("/boot/CAB.PPM");
	}

	tv_fig_free(model);
	tad_free(doc);
	Kfree(xml);
}

/*
 * The menu the second button brings up over a document.
 *
 * It is opened where the button came down, the pointer is walked down
 * it to a heading that holds a list of its own, and that list opens
 * beside it. Nothing here presses anything: what is being checked is
 * that the tree comes up, that it is laid out to its own words, and
 * that a heading opens what it holds.
 */
LOCAL void test_menu( void )
{
	T_DOCMENU	st;
	T_WMEV		ev;
	INT		pid, cmd = 0, i;
	ID		mid;

	start();
	if ( !ready ) KT_SKIP("no window");

	st.view = DM_VIEW_CLEAN;
	st.wrap = TRUE;
	st.show_hidden = FALSE;
	st.paper_frame = FALSE;
	st.has_pick = FALSE;
	st.is_root = FALSE;
	st.nrecent = 0;

	if ( doc_menu_make(&st, &mid) < E_OK ) {
		KT_SKIP("no definitions");
	}
	pid = mn_opn_men(mid, wid, 120, 90);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) {
		(void)mn_del_men(mid);
		return;
	}
	KT_ASSERT_ER(wm_panel_draw(pid), E_OK);

	/*
	 * Down the headings one row at a time, as a pointer would go. The
	 * list stands in a popup window of its own, and its rows are
	 * measured from that window.
	 */
	ev.type = HID_EV_MOVE;
	ev.code = 0;
	ev.mods = 0;
	ev.when = 0;
	ev.wid  = wm_panel_wid(pid);
	{
		T_DPRECT	mr;
		INT		row = wm_menu_row_h();

		KT_ASSERT_ER(wm_panel_rect(pid, &mr), E_OK);
		for ( i = 0; i <= 5; i++ ) {
			ev.x = mr.left + 20;
			ev.y = mr.top + i * row + row / 2;
			KT_ASSERT_ER(wm_menu_event(pid, &ev, &cmd), E_OK);
			KT_ASSERT_EQ(cmd, 0);	/* moving over it chooses nothing */
		}
	}
	KT_ASSERT_ER(wm_composite(), E_OK);
	kt_shot("/boot/MENU.PPM");

	/* and the whole chain goes when the menu does */
	KT_ASSERT_ER(wm_panel_close(pid), E_OK);
	(void)mn_del_men(mid);
	KT_ASSERT_EQ(wm_panel_self_check(), 0);
}


/*
 * Places in a text: where each is drawn and which is under a point,
 * the one the answer of the other; a paragraph set to the middle or the
 * right laid over by what is left of its line; an empty paragraph a
 * line the caret can stand in.
 */
LOCAL CONST char caret_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p>abcdef</p><p align=\"center\">mid</p><p align=\"right\">end</p><p></p>"
"</document></tad>";

LOCAL void test_caret( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_DPRECT	r, b0, b3, bc, br, be;
	INT		pi = -1, pos = -1;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	KT_ASSERT_ER(tad_parse((CONST UB *)caret_doc, s_len(caret_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(model->npara, 4);
	r.left = 0;  r.top = 0;  r.right = 400;  r.bottom = 300;

	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 0, &b0), E_OK);
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 3, &b3), E_OK);
	KT_ASSERT_EQ(b0.left, 0);
	KT_ASSERT(b3.left > b0.left + 10);
	KT_ASSERT(b3.bottom > b3.top);

	/* the place under a point is the place drawn there */
	KT_ASSERT_ER(tv_doc_caret_at(model, &r, 0, doc, b3.left + 1,
				     b3.top + 2, &pi, &pos), E_OK);
	KT_ASSERT_EQ(pi, 0);
	KT_ASSERT_EQ(pos, 3);

	/* the middle, and the right */
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 1, 0, &bc), E_OK);
	KT_ASSERT(bc.left > 150 && bc.left < 200);
	KT_ASSERT(bc.top > b0.top);
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 2, 3, &br), E_OK);
	KT_ASSERT(br.left > 390 && br.left <= 400);

	/* an empty paragraph, and a point below everything */
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 3, 0, &be), E_OK);
	KT_ASSERT_EQ(be.left, 0);
	KT_ASSERT(be.bottom > be.top);
	KT_ASSERT_ER(tv_doc_caret_at(model, &r, 0, doc, 5, 290, &pi, &pos), E_OK);
	KT_ASSERT_EQ(pi, 3);

	/* a place past the end of a paragraph is not there */
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 7, &b3), E_NOEXS);

	tv_doc_free(model);
	tad_free(doc);
}


/*
 * Every kind of shape a figure may hold is read into the model and
 * comes out as an outline: rounded corners as the record writes them,
 * a sector, a chord and an arc from the points that give their ends,
 * a curve, a line joined to two shapes, and a shape turned round.
 */
LOCAL CONST char shapes_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
"<rect round=\"1\" cornerRadius=\"8\" figRH=\"16\" figRV=\"10\" l_pat=\"1\" f_pat=\"0\" left=\"10\" top=\"10\" right=\"110\" bottom=\"60\"/>"
"<ellipse l_pat=\"1\" f_pat=\"4\" frameLeft=\"200\" frameTop=\"10\" frameRight=\"300\" frameBottom=\"60\"/>"
"<arc l_pat=\"1\" f_pat=\"4\" frameLeft=\"0\" frameTop=\"0\" frameRight=\"100\" frameBottom=\"100\" startX=\"101\" startY=\"50\" endX=\"50\" endY=\"101\"/>"
"<chord l_pat=\"1\" frameLeft=\"0\" frameTop=\"0\" frameRight=\"100\" frameBottom=\"100\" startX=\"101\" startY=\"50\" endX=\"1\" endY=\"50\"/>"
"<elliptical_arc l_pat=\"1\" f_pat=\"4\" frameLeft=\"0\" frameTop=\"0\" frameRight=\"100\" frameBottom=\"100\" startX=\"50\" startY=\"1\" endX=\"101\" endY=\"50\" end_arrow=\"1\" arrow_type=\"filled\"/>"
"<curve l_pat=\"1\" f_pat=\"0\" closed=\"0\" points=\"0,0 50,50 100,0 150,50\"/>"
"<line l_pat=\"1\" lineType=\"1\" lineConnectionType=\"curve\" start_conn=\"0,2\" end_conn=\"1,4\" points=\"0,0 5,5\"/>"
"<polygon l_pat=\"1\" f_pat=\"4\" rotation=\"90\" points=\"0,0 40,0 40,20 0,20\"/>"
"<polyline l_pat=\"1\" points=\"1.5,2.4 3.6,4\"/>"
"</figure></tad>";

LOCAL void test_shapes( void )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;
	T_DPPOINT	*out;
	INT		i, n;
	BOOL		closed;

	KT_ASSERT_ER(tad_parse((CONST UB *)shapes_doc, s_len(shapes_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(f->nsh, 9);
	out = (T_DPPOINT *)Kmalloc(sizeof(T_DPPOINT) * 4096);
	if ( out == NULL ) { tv_fig_free(f); tad_free(doc); return; }

	/* the angles: east is nought, south a quarter, the screen way round */
	KT_ASSERT_EQ(tv_atan2(0, 10), 0);
	KT_ASSERT_EQ(tv_atan2(10, 0), 1024);
	KT_ASSERT_EQ(tv_atan2(0, -10), 2048);
	KT_ASSERT_EQ(tv_atan2(-10, 0), 3072);
	KT_ASSERT_EQ(tv_sin(1024), 16384);
	KT_ASSERT_EQ(tv_cos(2048), -16384);

	for ( i = 0; i < f->nsh; i++ ) {
		CONST T_TVSHAPE	*sh = &f->sh[i];

		switch ( sh->kind ) {
		case TV_SH_RECT:
			KT_ASSERT_EQ(sh->rad_h, 8);
			KT_ASSERT_EQ(sh->rad_v, 5);
			n = tv_shape_outline(sh, out, 4096, &closed);
			KT_ASSERT(n > 8 && closed);
			break;
		case TV_SH_ARC:
			/* east to south: a quarter, the sector's middle first */
			KT_ASSERT_EQ(sh->a0, 0);
			KT_ASSERT_EQ(sh->a1, 1024);
			n = tv_shape_outline(sh, out, 4096, &closed);
			KT_ASSERT(n > 3 && closed);
			KT_ASSERT_EQ(out[0].x, 50);
			KT_ASSERT_EQ(out[0].y, 50);
			break;
		case TV_SH_CHORD:
			KT_ASSERT_EQ(sh->a1, 2048);
			break;
		case TV_SH_EARC:
			KT_ASSERT_EQ(sh->a0, 3072);
			KT_ASSERT_EQ((INT)sh->fill_col, (INT)TAD_COL_NONE);
			KT_ASSERT_EQ(sh->arrow, 1);
			KT_ASSERT_EQ(sh->arrow_type, 1);
			n = tv_shape_outline(sh, out, 4096, &closed);
			KT_ASSERT(n > 3 && !closed);
			break;
		case TV_SH_CURVE:
			KT_ASSERT_EQ(sh->npt, 4);
			n = tv_shape_outline(sh, out, 4096, &closed);
			KT_ASSERT(n > 8 && !closed);
			KT_ASSERT_EQ(out[n - 1].x, 150);
			break;
		case TV_SH_LINE:
			if ( sh->npt == 2 && sh->c0_shape >= 0 ) {
				/* from the rectangle's right to the ellipse's left */
				KT_ASSERT_EQ(sh->conn, TV_CONN_CURVE);
				KT_ASSERT_EQ(sh->line_type, 1);
				n = tv_line_path(f, sh, out, 4096);
				KT_ASSERT(n > 2);
				KT_ASSERT_EQ(out[0].x, 110);
				KT_ASSERT_EQ(out[0].y, 35);
				KT_ASSERT_EQ(out[n - 1].x, 200);
				KT_ASSERT_EQ(out[n - 1].y, 35);
			} else if ( sh->npt == 2 ) {
				/* numbers with fractions, to the nearest */
				KT_ASSERT_EQ(sh->pt[0].x, 2);
				KT_ASSERT_EQ(sh->pt[0].y, 2);
				KT_ASSERT_EQ(sh->pt[1].x, 4);
			}
			break;
		case TV_SH_POLY:
			/* turned a quarter about its middle: wide becomes tall */
			KT_ASSERT_EQ(sh->rot, 90);
			n = tv_shape_outline(sh, out, 4096, &closed);
			KT_ASSERT_EQ(n, 4);
			{
				INT	k, minx = out[0].x, maxx = out[0].x;

				for ( k = 1; k < n; k++ ) {
					if ( out[k].x < minx ) minx = out[k].x;
					if ( out[k].x > maxx ) maxx = out[k].x;
				}
				KT_ASSERT(maxx - minx >= 19 && maxx - minx <= 22);
			}
			break;
		default:
			break;
		}
	}
	Kfree(out);

	/* magnified twice, a place is half as far into the figure */
	{
		T_DPRECT	r;
		INT		which = -1;

		r.left = 0;  r.top = 0;  r.right = 800;  r.bottom = 600;
		KT_ASSERT_EQ(f->zoom, 0);
		KT_ASSERT_ER(tv_fig_shape_at(f, &r, 0, 0, 410, 70, &which), E_NOEXS);
		f->zoom = 16;
		KT_ASSERT_ER(tv_fig_shape_at(f, &r, 0, 0, 410, 70, &which), E_OK);
		KT_ASSERT(which >= 0 && f->sh[which].kind == TV_SH_ELLIPSE);
	}
	tv_fig_free(f);
	tad_free(doc);
}

/*
 * A document on paper: the paper and its margins, and the tab format's
 * margin, read in the record's own units and kept as pixels; a ruby
 * written under its letters; and, laid on pages, text kept inside the
 * margins and a page break going on to the head of the next page.
 * Shown in one length, the same text uses the whole box.
 */
LOCAL CONST char pages_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<docScale hunit=\"-144\" vunit=\"-144\"/>"
"<paper width=\"1190\" length=\"400\"/>"
"<docmargin top=\"40\" bottom=\"40\" left=\"40\" right=\"40\"/>"
"<p><tab-format left=\"56\" right=\"0\" indent=\"0\"/>AB</p>"
"<p><ruby position=\"1\" text=\"\xe3\x82\x88\xe3\x81\xbf\">\xe6\xbc\xa2\xe5\xad\x97</ruby></p>"
"<pagebreak/>"
"<p>CD</p>"
"</document></tad>";

LOCAL void test_pages( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_DPRECT	r, b0, b2;
	INT		i;
	BOOL		below = FALSE;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	KT_ASSERT_ER(tad_parse((CONST UB *)pages_doc, s_len(pages_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(model->npara, 3);
	KT_ASSERT_EQ(model->paper_w, 595);
	KT_ASSERT_EQ(model->paper_h, 200);
	KT_ASSERT_EQ(model->margin[0], 20);
	KT_ASSERT_EQ(model->margin[1], 20);
	KT_ASSERT_EQ(model->para[0].left, 28);
	for ( i = 0; i < model->nrun; i++ ) {
		if ( model->run[i].ruby != NULL ) {
			below = model->run[i].ruby_below;
		}
	}
	KT_ASSERT(below);

	r.left = 0;  r.top = 0;  r.right = 595;  r.bottom = 600;

	/* in one length: no margins */
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 0, &b0), E_OK);
	KT_ASSERT_EQ(b0.left, 28);
	KT_ASSERT(b0.top < 20);

	/* on pages: inside the margins, and the break goes to the next page */
	model->page_h = model->paper_h;
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 0, &b0), E_OK);
	KT_ASSERT_EQ(b0.left, 48);
	KT_ASSERT(b0.top >= 20);
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 2, 0, &b2), E_OK);
	KT_ASSERT(b2.top >= 220);
	KT_ASSERT(b2.top < 260);
	KT_ASSERT(tv_doc_height(model, 595, doc) > 220);

	/* in two columns the pages are whole pages, and a column starts at the margin */
	model->columns = 2;
	model->colsp = 20;
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 0, &b0), E_OK);
	KT_ASSERT_EQ(b0.left, 48);
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 2, 0, &b2), E_OK);
	KT_ASSERT(b2.top >= 220);
	KT_ASSERT_EQ(tv_doc_height(model, 595, doc) % 200, 0);

	tv_doc_free(model);
	tad_free(doc);
}

/*
 * A picture written as a PNG reads back as the same pixels, the clear
 * ones clear -- large enough that its data runs to more than one of
 * the blocks it is stored in.
 */
LOCAL void test_png( void )
{
	INT	w = 200, h = 120, x, y, bad = 0, rw = 0, rh = 0;
	UW	*px, *back = NULL;
	UB	*png = NULL;
	SZ	len = 0;

	px = (UW *)Kmalloc(sizeof(UW) * (SZ)w * h);
	KT_ASSERT(px != NULL);
	if ( px == NULL ) return;
	for ( y = 0; y < h; y++ ) {
		for ( x = 0; x < w; x++ ) {
			px[y * w + x] = ( ( x + y ) % 7 == 0 ) ? IMG_CLEAR
				      : (UW)( ( x << 16 ) | ( y << 8 ) | ( ( x ^ y ) & 0xFF ) );
		}
	}
	KT_ASSERT_ER(img_png_encode(px, w, h, &png, &len), E_OK);
	KT_ASSERT(len > (SZ)w * h * 4);
	KT_ASSERT_ER(img_png_decode(png, len, &back, &rw, &rh), E_OK);
	KT_ASSERT_EQ(rw, w);
	KT_ASSERT_EQ(rh, h);
	if ( back != NULL ) {
		for ( y = 0; y < h; y++ ) {
			for ( x = 0; x < w; x++ ) {
				if ( back[y * w + x] != px[y * w + x] ) {
					bad++;
				}
			}
		}
		Kfree(back);
	}
	KT_ASSERT_EQ(bad, 0);
	if ( png != NULL ) Kfree(png);
	Kfree(px);
}

/*
 * A figure's own patterns: colours each drawn through a mask, a mask
 * narrower than sixteen laid again beside itself, and one of the fixed
 * numbers given a colour of the figure's own.
 */
LOCAL CONST char pats_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
"<patterns>"
"<pattern id=\"130\" type=\"0\" width=\"16\" height=\"16\" ncol=\"2\" fgcolors=\"#ff0000,#0000ff\" bgcolor=\"transparent\" masks=\"200,201\"/>"
"<mask id=\"200\" type=\"0\" width=\"16\" height=\"16\" data=\"8000,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\"/>"
"<mask id=\"201\" type=\"0\" width=\"8\" height=\"1\" data=\"0100\"/>"
"<pattern id=\"5\" ncol=\"1\" fgcolors=\"#123456\" masks=\"\"/>"
"</patterns>"
"<rect l_pat=\"1\" f_pat=\"130\" left=\"0\" top=\"0\" right=\"10\" bottom=\"10\"/>"
"<rect l_pat=\"5\" f_pat=\"5\" left=\"20\" top=\"0\" right=\"30\" bottom=\"10\"/>"
"</figure></tad>";

LOCAL void test_patterns( void )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;
	CONST T_TVPAT	*p;

	KT_ASSERT_ER(tad_parse((CONST UB *)pats_doc, s_len(pats_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(f->nsh, 2);
	p = f->sh[0].fill_pat;
	KT_ASSERT(p != NULL);
	KT_ASSERT(tv_fig_pattern(f, 130) == p);
	if ( p != NULL ) {
		KT_ASSERT_EQ(p->tile[0], 0x00FF0000U);
		KT_ASSERT(( p->mask[0] & 0x80000000U ) != 0);
		KT_ASSERT_EQ(p->tile[7], 0x000000FFU);
		KT_ASSERT_EQ(p->tile[15], 0x000000FFU);
		KT_ASSERT_EQ(p->tile[3 * 16 + 7], 0x000000FFU);
		KT_ASSERT(( p->mask[3] & ( 0x80000000U >> 1 ) ) == 0);
	}
	KT_ASSERT(f->sh[1].fill_pat == NULL);
	KT_ASSERT_EQ(f->sh[1].fill_col, 0x00123456U);
	KT_ASSERT_EQ(f->sh[1].line_col, 0x00123456U);
	tv_fig_free(f);
	tad_free(doc);
}

/*
 * What a figure says of the shapes after it: a <transform> moving the
 * next one, a figure inside the figure drawn from its drawing area into
 * its view, marks at points, the bands of a <freefig>, and arrows given
 * by a <figmodifier>.
 */
LOCAL CONST char elems_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
"<transform dh=\"100\" dv=\"50\" hangle=\"0\" vangle=\"0\"/>"
"<rect l_pat=\"1\" left=\"0\" top=\"0\" right=\"10\" bottom=\"10\"/>"
"<figure><figView left=\"200\" top=\"0\" right=\"300\" bottom=\"100\"/>"
"<figDraw left=\"0\" top=\"0\" right=\"50\" bottom=\"50\"/>"
"<rect l_pat=\"1\" left=\"10\" top=\"10\" right=\"20\" bottom=\"20\"/></figure>"
"<markerDefine type=\"0\" id=\"3\" size=\"10\" fgCol=\"#ff0000\"/>"
"<marker markerId=\"3\" points=\"5,5 15,5\"/>"
"<freefig f_pat=\"1\" sy=\"30\" nr=\"5\" bx=\"0\" nh=\"4\" h=\"10,20,30,40\"/>"
"<figmodifier arrow=\"both\"/>"
"<line l_pat=\"1\" points=\"0,0 10,10\"/>"
"</figure></tad>";

LOCAL void test_elements( void )
{
	T_TAD		*doc = NULL;
	T_TVFIG		*f = NULL;

	KT_ASSERT_ER(tad_parse((CONST UB *)elems_doc, s_len(elems_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(f->nsh, 6);
	if ( f->nsh == 6 ) {
		KT_ASSERT_EQ(f->sh[0].r.left, 100);
		KT_ASSERT_EQ(f->sh[0].r.top, 50);
		KT_ASSERT_EQ(f->sh[0].r.right, 110);
		KT_ASSERT_EQ(f->sh[1].r.left, 220);
		KT_ASSERT_EQ(f->sh[1].r.top, 20);
		KT_ASSERT_EQ(f->sh[1].r.right, 240);
		KT_ASSERT_EQ(f->sh[1].r.bottom, 40);
		KT_ASSERT_EQ(f->sh[2].kind, TV_SH_MARKER);
		KT_ASSERT_EQ(f->sh[2].npt, 2);
		KT_ASSERT_EQ(f->sh[2].rad_h, 10);
		KT_ASSERT_EQ(f->sh[2].rad_v, 3);
		KT_ASSERT_EQ(f->sh[2].line_col, 0x00FF0000U);
		KT_ASSERT_EQ(f->sh[3].kind, TV_SH_RECT);
		KT_ASSERT_EQ(f->sh[3].r.left, 10);
		KT_ASSERT_EQ(f->sh[3].r.bottom, 35);
		KT_ASSERT_EQ(f->sh[4].r.left, 30);
		KT_ASSERT_EQ(f->sh[4].r.right, 40);
		KT_ASSERT_EQ(f->sh[5].kind, TV_SH_LINE);
		KT_ASSERT_EQ(f->sh[5].arrow, 3);
	}
	tv_fig_free(f);
	tad_free(doc);
}

/* A figure standing in a text: a thing of the text, as large as its view */
LOCAL CONST char infig_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p>A<figure><figView left=\"10\" top=\"10\" right=\"110\" bottom=\"50\"/>"
"<figDraw left=\"10\" top=\"10\" right=\"110\" bottom=\"50\"/>"
"<rect l_pat=\"1\" left=\"10\" top=\"10\" right=\"110\" bottom=\"50\"/></figure>B</p>"
"</document></tad>";

LOCAL void test_infigure( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_DPRECT	r, box;
	INT		i, run = -1;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	KT_ASSERT_ER(tad_parse((CONST UB *)infig_doc, s_len(infig_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(model->nfig, 1);
	for ( i = 0; i < model->nrun; i++ ) {
		if ( model->run[i].kind == TV_RUN_FIGURE ) {
			run = i;
		}
	}
	KT_ASSERT(run >= 0);
	KT_ASSERT_EQ(model->figs[0]->nsh, 1);
	r.left = 0;  r.top = 0;  r.right = 400;  r.bottom = 300;
	KT_ASSERT_EQ(tv_doc_thing(model, &r, 0, doc, 0, 0, run, &box), run);
	KT_ASSERT_EQ(box.right - box.left, 100);
	KT_ASSERT_EQ(box.bottom - box.top, 40);
	/* the place after it is one further on than the place before it */
	KT_ASSERT_EQ(tv_doc_thing(model, &r, 0, doc, box.left + 5, box.top + 5, -1, NULL), run);
	tv_doc_free(model);
	tad_free(doc);
}

/* 詳細: a tab format shows as a ruler with its markers, settings as flags */
LOCAL CONST char marks_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p><tab-format left=\"28\" indent=\"14\" ntabs=\"1\" tabs=\"56\"/>abc"
"<text align=\"center\"/></p>"
"<p><docmemo text=\"note\"/>def</p>"
"</document></tad>";

#define FLAG_MID	8		/* inside a flag at the head of a line */

LOCAL void test_marks( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_DPRECT	r;
	T_TVMARK	m;
	T_DPRECT	box;
	INT		x, y, found = 0;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	KT_ASSERT_ER(tad_parse((CONST UB *)marks_doc, s_len(marks_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT(model->npara >= 2);
	KT_ASSERT(model->para[0].fmt != NULL);
	KT_ASSERT(model->para[0].align_at != NULL);
	KT_ASSERT(model->para[1].fmt == NULL);
	r.left = 0;  r.top = 0;  r.right = 400;  r.bottom = 300;

	/* not in detail: nothing */
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 28, 12, &m), TV_MARK_NONE);

	model->detail = TRUE;
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 28, 12, &m), TV_MARK_LHEAD);
	KT_ASSERT(m.node == model->para[0].fmt);
	KT_ASSERT_EQ(m.zero, 0);
	KT_ASSERT_EQ(m.head, 28);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 42, 12, &m), TV_MARK_FHEAD);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 84, 12, &m), TV_MARK_TAB);
	KT_ASSERT_EQ(m.index, 0);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 398, 12, &m), TV_MARK_LEND);
	KT_ASSERT_EQ(m.end, 400);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 6, 12, &m), TV_MARK_ORIGIN);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 200, 12, &m), TV_MARK_BAR);
	KT_ASSERT_EQ(tv_doc_mark(model, &r, 0, doc, 386, 40, &m), TV_MARK_ALIGN);
	KT_ASSERT(m.node == model->para[0].align_at);

	/* the note's flag, where the second paragraph begins */
	KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 1, 0, &box), E_OK);
	x = box.left + FLAG_MID;
	for ( y = box.top; y < box.bottom + 4 && !found; y++ ) {
		if ( tv_doc_mark(model, &r, 0, doc, x, y, &m) == TV_MARK_RUN ) {
			found = 1;
		}
	}
	KT_ASSERT_EQ(found, 1);
	KT_ASSERT(box.left > 28);		/* centred: the alignment carries on */
	KT_ASSERT(m.node != NULL && tad_attr(m.node, "text") != NULL);
	tv_doc_free(model);
	tad_free(doc);
}

/* 用紙オーバーレイ: a header and a footer defined, and put on */
LOCAL CONST char over_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<paper type=\"doc\" length=\"842\" width=\"595\" top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<paper-overlay-define N=\"0\" P=\"0\"><p><text align=\"right\"/>Head</p></paper-overlay-define>"
"<paper-overlay-define N=\"1\" P=\"2\"><tab-format height=\"abs:20\"/>"
"<p><text align=\"center\"/>- <page-number step=\"1\" num=\"3\"/> -</p></paper-overlay-define>"
"<docoverlay active=\"0, 1\"/>"
"<p>text</p>"
"</document></tad>";

LOCAL void test_overlays( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	INT		i, pn = -1;

	start();
	KT_ASSERT_ER(tad_parse((CONST UB *)over_doc, s_len(over_doc), NULL, &doc),
		     E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	/* the text holds only its own paragraph */
	KT_ASSERT_EQ(model->npara, 1);
	KT_ASSERT(model->over[0] != NULL);
	KT_ASSERT(model->over[1] != NULL);
	KT_ASSERT(model->over[2] == NULL);
	KT_ASSERT_EQ(model->over_on, 3);
	KT_ASSERT_EQ(model->over_pages[1], 2);
	KT_ASSERT_EQ(model->over[0]->npara, 1);
	KT_ASSERT_EQ(model->over[0]->para[0].align, TV_ALIGN_RIGHT);
	KT_ASSERT_EQ(model->over[1]->para[0].align, TV_ALIGN_CENTRE);
	KT_ASSERT(model->over[1]->para[0].gap_abs);
	KT_ASSERT_EQ(model->over[1]->para[0].gap, 20);
	for ( i = 0; i < model->over[1]->nrun; i++ ) {
		if ( model->over[1]->run[i].kind == TV_RUN_PAGENO ) {
			pn = i;
		}
	}
	KT_ASSERT(pn >= 0);
	tv_doc_free(model);
	tad_free(doc);
}

/*
 * What 書庫解凍 makes of a BTRON text (design 17.16): an overlay whose
 * text begins with a fill line, and so stands at the foot, overlay 3 put
 * on, the document's own 禁則, variables, a character kept as <tchar>,
 * and a figure's own kind of line.
 */
LOCAL CONST char bpk_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"Name\"><document>"
"<paper type=\"doc\" length=\"842\" width=\"595\" top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<docmargin top=\"57\" bottom=\"57\" left=\"57\" right=\"28\"/>"
"<paper-overlay-define N=\"3\" P=\"0\"><p><fill-line />"
"<text align=\"center\"/>-<page-number num=\"1\" step=\"1\"/>-</p></paper-overlay-define>"
"<docoverlay active=\"3\"/>"
"<line-head-kinsoku kind=\"0x00\" ch=\"x\"/>"
"<line-tail-kinsoku kind=\"0x11\" ch=\"(\"/>"
"<p>a<variable id=\"0\"/>b<variable id=\"252\"/><tchar plane=\"9\" code=\"9830\">\xE3\x80\x93</tchar>"
"<docappl appl=\"8000-0003-8000\" data=\"0002\"/><tadseg id=\"ffe8\" data=\"00\"/></p>"
"</document></tad>";

LOCAL CONST char bpk_fig[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
"<figView left=\"0\" top=\"0\" right=\"100\" bottom=\"100\"/>"
"<lineTypeDefine id=\"9\" nb=\"2\" mask=\"0ff0\" />"
"<figappl appl=\"8000-0000-8000\" data=\"0000\"/>"
"<line lineType=\"9\" lineWidth=\"1\" l_pat=\"0\" f_pat=\"0\" points=\"0,0 90,90\" zIndex=\"1\" />"
"</figure></tad>";

LOCAL void test_bpk_forms( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_TVFIG		*f = NULL;
	CONST UB	*dash;
	INT		i, nvar = 0, h;
	BOOL		geta = FALSE;

	start();
	KT_ASSERT_ER(tad_parse((CONST UB *)bpk_doc, s_len(bpk_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT(model->over[3] != NULL);
	KT_ASSERT(model->over[3] != NULL && model->over[3]->foot);
	KT_ASSERT(!model->foot);
	KT_ASSERT_EQ(model->over_on, 8);
	KT_ASSERT_EQ(model->npara, 1);
	KT_ASSERT_EQ(model->para[0].khead_kind, 0);
	KT_ASSERT_EQ(model->para[0].ktail_kind, 0x11);
	KT_ASSERT_EQ(model->para[0].ktail_len, 1);
	for ( i = 0; i < model->nrun; i++ ) {
		if ( model->run[i].kind == TV_RUN_VAR ) nvar++;
		if ( model->run[i].kind == TV_RUN_TEXT && model->run[i].len == 3
		  && model->run[i].text[0] == 0xE3 && model->run[i].text[2] == 0x93 ) geta = TRUE;
	}
	KT_ASSERT_EQ(nvar, 2);
	KT_ASSERT(geta);
	/* laid on pages, the variables filled in and the overlay at the foot */
	model->page_h = model->paper_h;
	h = tv_doc_height(model, 595, doc);
	KT_ASSERT(h > 0);
	if ( ready ) {
		T_DPRECT	r;
		INT		gid = wm_gid(wid);

		r.left = 10;  r.top = 10;  r.right = 605;  r.bottom = 620;
		/* scrolled so that the foot of the first page, and its number, shows */
		KT_ASSERT(tv_doc_draw(gid, model, &r, 300, doc, TV_PAPER_LOOK) > 0);
		KT_ASSERT_ER(wm_composite(), E_OK);
		kt_shot("/boot/BPKF.PPM");
	}
	tv_doc_free(model);
	tad_free(doc);

	doc = NULL;
	KT_ASSERT_ER(tad_parse((CONST UB *)bpk_fig, s_len(bpk_fig), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f != NULL ) {
		/* 0000 1111 1111 0000: eight on, eight off, from the first drawn */
		dash = tv_fig_dash(f, 9);
		KT_ASSERT(dash != NULL);
		if ( dash != NULL ) {
			KT_ASSERT_EQ(dash[0], 8);
			KT_ASSERT_EQ(dash[1], 8);
			KT_ASSERT_EQ(dash[2], 0);
		}
		KT_ASSERT(tv_fig_dash(f, 1) == NULL);
		KT_ASSERT_EQ(f->nsh, 1);
		if ( ready ) {
			T_DPRECT	r;

			r.left = 10;  r.top = 10;  r.right = 200;  r.bottom = 200;
			KT_ASSERT_ER(tv_fig_draw(wm_gid(wid), f, &r, 0, 0, doc, TV_PAPER_LOOK), E_OK);
		}
		tv_fig_free(f);
	}
	tad_free(doc);
}

/*
 * The forms 書庫解凍 writes that change how things are drawn: letters
 * turned (文字回転) and moved off the baseline (文字基準位置移動), a
 * figure's paper overlay, and a link sized the way a binary TAD sizes
 * it. Each is drawn into pixels of the test's own and looked at.
 */
#define PX_W		400
#define PX_H		240
#define PX_PAPER	0x00FFFFFFU

LOCAL CONST char turn_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<p><font size=\"48\"/>I<font rotation=\"90\" rotabs=\"0\"/>I<font rotation=\"0\"/></p>"
"<p><font size=\"40\"/>I<font baseshift=\"abs:10\" baseattr=\"0\"/>I"
"<font baseshift=\"0.25\" baseattr=\"1\"/>I</p>"
"</document></tad>";

LOCAL CONST char over_fig[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
"<figView left=\"0\" top=\"0\" right=\"200\" bottom=\"100\"/>"
"<figoverlay number=\"1\" even=\"true\" odd=\"true\" overlayData=\"0\">"
"<rect round=\"0\" lineWidth=\"0\" fillColor=\"#ff0000\" left=\"10\" top=\"10\" right=\"40\" bottom=\"40\" zIndex=\"1\"/>"
"</figoverlay>"
"<figoverlay number=\"2\" even=\"true\" odd=\"true\" overlayData=\"0\">"
"<rect round=\"0\" lineWidth=\"0\" fillColor=\"#00ff00\" left=\"50\" top=\"10\" right=\"80\" bottom=\"40\" zIndex=\"1\"/>"
"</figoverlay>"
"<figoverlay number=\"3\" even=\"true\" odd=\"false\" overlayData=\"0\">"
"<rect round=\"0\" lineWidth=\"0\" fillColor=\"#0000ff\" left=\"90\" top=\"10\" right=\"120\" bottom=\"40\" zIndex=\"1\"/>"
"</figoverlay>"
"<figoverlay active=\"1, 3\"/>"
"<rect round=\"0\" lineWidth=\"0\" fillColor=\"#000000\" left=\"30\" top=\"30\" right=\"60\" bottom=\"60\" zIndex=\"1\"/>"
"</figure></tad>";

/* a link as a binary TAD of 120 units to the inch gives it: its rectangle, or that in points */
LOCAL CONST char vobj_doc[] =
"<tad version=\"1.0\" encoding=\"UTF-8\"><document>"
"<docScale hunit=\"-120\" vunit=\"-120\"/>"
"<p><link id=\"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_0.xtad\" name=\"\xE5\x89\x8D\xE5\x9B\x9E\xE3\x81\xBE\xE3\x81\xA7\xE3\x81\xAE\xE6\x8C\xAF\xE3\x82\x8A\xE8\xBF\x94\xE3\x82\x8A\""
" vobjleft=\"5\" vobjtop=\"88\" vobjright=\"278\" vobjbottom=\"120\" height=\"32\" chsz=\"14\"/></p>"
"<p><link id=\"01a0d8c4-52a8-7d42-8c6f-4a2e9b7d3fa1_0.xtad\" name=\"\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAB\""
" vobjleft=\"5\" vobjtop=\"48\" vobjright=\"164\" vobjbottom=\"80\" height=\"32\" width=\"95\" heightpx=\"19\" chsz=\"14\"/></p>"
"</document></tad>";

/* Pixels of the test's own, and a drawing environment on them */
LOCAL UW *px_open( INT *p_gid )
{
	UW		*px = (UW *)Kmalloc(sizeof(UW) * PX_W * PX_H);
	T_DPRECT	all;
	INT		gid, i;

	*p_gid = -1;
	if ( px == NULL ) {
		return NULL;
	}
	for ( i = 0; i < PX_W * PX_H; i++ ) {
		px[i] = PX_PAPER;
	}
	gid = dp_open();
	if ( gid < 0 ) {
		Kfree(px);
		return NULL;
	}
	all.left = 0;  all.top = 0;  all.right = PX_W;  all.bottom = PX_H;
	dp_set_target(gid, px, PX_W * 4, 0, 0);
	dp_set_origin(gid, 0, 0);
	dp_set_frame(gid, &all);
	dp_set_visible(gid, &all);
	*p_gid = gid;

	return px;
}

/* The box of the pixels of a colour in a stretch of columns and rows; FALSE for none */
LOCAL BOOL ink_box( CONST UW *px, UW col, INT x0, INT x1, INT y0, INT y1, T_DPRECT *b )
{
	INT	x, y;
	BOOL	any = FALSE;

	for ( y = ( y0 > 0 ) ? y0 : 0; y < y1 && y < PX_H; y++ ) {
		for ( x = ( x0 > 0 ) ? x0 : 0; x < x1 && x < PX_W; x++ ) {
			if ( ( px[y * PX_W + x] & 0x00FFFFFFU ) != col ) {
				continue;
			}
			if ( !any ) {
				b->left = b->right = x;
				b->top = b->bottom = y;
				any = TRUE;
			}
			if ( x < b->left ) b->left = x;
			if ( x > b->right ) b->right = x;
			if ( y < b->top ) b->top = y;
			if ( y > b->bottom ) b->bottom = y;
		}
	}

	return any;
}

LOCAL void test_bpk_draw( void )
{
	T_TAD		*doc = NULL;
	T_TVDOC		*model = NULL;
	T_TVFIG		*f = NULL;
	T_DPRECT	r, c1, c2, a, b, c;
	UW		*px;
	INT		gid, i;

	start();
	if ( fn_system() <= 0 ) KT_SKIP("no face");
	r.left = 0;  r.top = 0;  r.right = PX_W;  r.bottom = PX_H;

	/* turned and moved letters */
	KT_ASSERT_ER(tad_parse((CONST UB *)turn_doc, s_len(turn_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model == NULL ) { tad_free(doc); return; }
	KT_ASSERT_EQ(model->npara, 2);
	{
		INT	turned = 0, up = 0, down = 0;

		for ( i = 0; i < model->nrun; i++ ) {
			if ( model->run[i].turn == 90 ) turned++;
			if ( model->run[i].lift == 10 && model->run[i].lift_abs ) up++;
			if ( model->run[i].lift == -250 && !model->run[i].lift_abs ) down++;
		}
		KT_ASSERT_EQ(turned, 1);
		KT_ASSERT_EQ(up, 1);
		KT_ASSERT_EQ(down, 1);
	}
	px = px_open(&gid);
	KT_ASSERT(px != NULL);
	if ( px != NULL ) {
		KT_ASSERT(tv_doc_draw(gid, model, &r, 0, doc, PX_PAPER) > 0);

		/* the upright I stands tall; the one turned a quarter lies along the line */
		KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 1, &c1), E_OK);
		KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 0, 2, &c2), E_OK);
		KT_ASSERT(ink_box(px, 0, 0, c1.left, c1.top, c1.bottom, &a));
		KT_ASSERT(ink_box(px, 0, c1.left, c2.left + 1, c1.top, c1.bottom, &b));
		tm_printf((UB *)"  upright %d,%d-%d,%d turned %d,%d-%d,%d cells %d %d\n",
			  a.left, a.top, a.right, a.bottom, b.left, b.top, b.right, b.bottom,
			  c1.left, c2.left);
		KT_ASSERT(a.bottom - a.top > 3 * ( a.right - a.left + 1 ));
		KT_ASSERT(b.right - b.left > 3 * ( b.bottom - b.top + 1 ));
		/* as long across as the upright one is tall, inside its own cell */
		KT_ASSERT(b.right - b.left + 3 >= a.bottom - a.top);
		KT_ASSERT(b.right - b.left <= a.bottom - a.top + 3);
		KT_ASSERT(b.left >= c1.left && b.right < c2.left + 1);
		/* its cell as wide as the letters are tall */
		KT_ASSERT(c2.left - c1.left >= 44 && c2.left - c1.left <= 52);
		/* its middle as high as the upright box's middle */
		KT_ASSERT(b.top < a.bottom && b.bottom > a.top);

		/* the second line: raised ten pixels, then lowered a quarter of 40 */
		KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 1, 1, &c1), E_OK);
		KT_ASSERT_ER(tv_doc_caret_box(model, &r, 0, doc, 1, 2, &c2), E_OK);
		{
			/* only the second line's rows */
			KT_ASSERT(ink_box(px, 0, 0, c1.left, c1.top, c1.bottom, &a));
			KT_ASSERT(ink_box(px, 0, c1.left, c2.left, c1.top, c1.bottom, &b));
			KT_ASSERT(ink_box(px, 0, c2.left, PX_W, c1.top, c1.bottom, &c));
			tm_printf((UB *)"  base %d-%d raised %d-%d lowered %d-%d line %d-%d\n",
				  a.top, a.bottom, b.top, b.bottom, c.top, c.bottom,
				  c1.top, c1.bottom);
			KT_ASSERT_EQ(a.top - b.top, 10);
			KT_ASSERT_EQ(a.bottom - b.bottom, 10);
			KT_ASSERT_EQ(c.top - a.top, 10);
			KT_ASSERT_EQ(c.bottom - a.bottom, 10);
		}
		dp_close(gid);
		Kfree(px);
	}
	if ( ready ) {
		T_DPRECT	w;

		w.left = 10;  w.top = 10;  w.right = 10 + PX_W;  w.bottom = 10 + PX_H;
		KT_ASSERT(tv_doc_draw(wm_gid(wid), model, &w, 0, doc, TV_PAPER_LOOK) > 0);
		KT_ASSERT_ER(wm_composite(), E_OK);
		kt_shot("/boot/BPKD.PPM");
	}
	tv_doc_free(model);
	tad_free(doc);

	/* a figure's overlays: the one put on, not the one left off nor the even pages' */
	doc = NULL;
	KT_ASSERT_ER(tad_parse((CONST UB *)over_fig, s_len(over_fig), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_fig(doc, &f), E_OK);
	if ( f != NULL ) {
		KT_ASSERT_EQ(f->nsh, 1);		/* the overlays' shapes are their own */
		KT_ASSERT_EQ(f->over_on, 0x0A);
		KT_ASSERT(f->over[1] != NULL && f->over[2] != NULL && f->over[3] != NULL);
		KT_ASSERT_EQ(f->over_pages[3], 2);
		KT_ASSERT_EQ(f->sh[0].place, 0);
		px = px_open(&gid);
		KT_ASSERT(px != NULL);
		if ( px != NULL ) {
			KT_ASSERT_ER(tv_fig_draw(gid, f, &r, 0, 0, doc, PX_PAPER), E_OK);
			KT_ASSERT_EQ(px[20 * PX_W + 20] & 0x00FFFFFFU, 0x00FF0000U);
			/* under the figure's own shape where they meet */
			KT_ASSERT_EQ(px[35 * PX_W + 35] & 0x00FFFFFFU, 0x00000000U);
			KT_ASSERT_EQ(px[20 * PX_W + 60] & 0x00FFFFFFU, PX_PAPER);
			KT_ASSERT_EQ(px[20 * PX_W + 100] & 0x00FFFFFFU, PX_PAPER);
			dp_close(gid);
			Kfree(px);
		}
		tv_fig_free(f);
	}
	tad_free(doc);

	/* links sized by a binary TAD's rectangle: their names fit */
	doc = NULL;
	KT_ASSERT_ER(tad_parse((CONST UB *)vobj_doc, s_len(vobj_doc), NULL, &doc), E_OK);
	if ( doc == NULL ) return;
	KT_ASSERT_ER(tv_doc(doc, &model), E_OK);
	if ( model != NULL ) {
		INT	k = 0;

		for ( i = 0; i < model->nrun; i++ ) {
			T_VOBJ		v;
			T_DPRECT	box;
			INT		band, tw, w, h;
			ID		fid = fn_system();

			if ( model->run[i].kind != TV_RUN_LINK ) {
				continue;
			}
			KT_ASSERT_EQ(tv_doc_thing(model, &r, 0, doc, 0, 0, i, &box), i);
			KT_ASSERT_ER(tad_lnk_get(doc, model->run[i].link, &v), E_OK);
			band = om_band_h(&v);
			(void)fn_set_size(fid, v.chsz);
			tw = fn_width(fid, (CONST UB *)v.name);
			w = box.right - box.left;
			h = box.bottom - box.top;
			tm_printf((UB *)"  link %d: %dx%d band %d name %d\n", k, w, h, band, tw);
			KT_ASSERT_EQ(w, ( k == 0 ) ? ( 278 - 5 ) * 72 / 120 : 95);
			/* the band and its frame, and the picture beside the name */
			KT_ASSERT(h >= band + 2 * OM_FRAME_W);
			KT_ASSERT(w - band - 7 >= tw);
			k++;
		}
		KT_ASSERT_EQ(k, 2);
		tv_doc_free(model);
	}
	tad_free(doc);
}

/* 検索の正規表現: where a pattern matches, its groups, and what replaces it */
LOCAL BOOL str_is( CONST char *a, CONST char *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}

	return (BOOL)( *a == *b );
}

LOCAL BOOL rx_at( CONST char *pat, CONST char *text, INT *p_a, INT *p_b,
		  INT *groups )
{
	T_RX	*rx = NULL;
	BOOL	ok;

	if ( rx_compile((CONST UB *)pat, &rx) < E_OK ) {
		return FALSE;
	}
	ok = rx_find(rx, (CONST UB *)text, (INT)s_len(text), 0, p_a, p_b, groups);
	rx_free(rx);

	return ok;
}

LOCAL void test_regex( void )
{
	T_RX	*rx = NULL;
	INT	a = -1, b = -1, g[RX_GROUPS * 2];
	UB	out[64];
	char	*big;
	INT	i;

	start();
	KT_ASSERT(rx_at("a+b", "xaaab", &a, &b, NULL));
	KT_ASSERT_EQ(a, 1);  KT_ASSERT_EQ(b, 5);

	KT_ASSERT(rx_at("(\\d+)-(\\d+)", "tel 03-1234", &a, &b, g));
	KT_ASSERT_EQ(a, 4);  KT_ASSERT_EQ(b, 11);
	KT_ASSERT_EQ(g[2], 4);  KT_ASSERT_EQ(g[3], 6);
	KT_ASSERT_EQ(g[4], 7);  KT_ASSERT_EQ(g[5], 11);
	KT_ASSERT_EQ(rx_expand((CONST UB *)"$2/$1 [$&] $$", (CONST UB *)"tel 03-1234",
			       g, out, sizeof(out)), 19);
	KT_ASSERT(str_is((CONST char *)out, "1234/03 [03-1234] $"));

	KT_ASSERT(rx_at("^abc$", "abc", &a, &b, NULL));
	KT_ASSERT(!rx_at("^abc$", "abcd", &a, &b, NULL));
	KT_ASSERT(rx_at("colou?r", "color", &a, &b, NULL));
	KT_ASSERT(rx_at("colou?r", "a colour", &a, &b, NULL));
	KT_ASSERT_EQ(a, 2);

	KT_ASSERT(rx_at("[^a-c]+", "abcxyzabc", &a, &b, NULL));
	KT_ASSERT_EQ(a, 3);  KT_ASSERT_EQ(b, 6);
	KT_ASSERT(rx_at("(?:ab|cd){2}", "xxcdab", &a, &b, NULL));
	KT_ASSERT_EQ(a, 2);  KT_ASSERT_EQ(b, 6);
	KT_ASSERT(rx_at(".*?b", "aabab", &a, &b, NULL));
	KT_ASSERT_EQ(b, 3);
	KT_ASSERT(rx_at(".*b", "aabab", &a, &b, NULL));
	KT_ASSERT_EQ(b, 5);
	KT_ASSERT(rx_at("a{2,3}", "aaaa", &a, &b, NULL));
	KT_ASSERT_EQ(b, 3);
	KT_ASSERT(rx_at("a\\.b", "a.b", &a, &b, NULL));
	KT_ASSERT(!rx_at("a\\.b", "axb", &a, &b, NULL));
	KT_ASSERT(rx_at("(a|b)*c", "ababc", &a, &b, g));
	KT_ASSERT_EQ(a, 0);  KT_ASSERT_EQ(b, 5);
	KT_ASSERT_EQ(g[2], 3);			/* the last time round */

	/* letters of more than one byte, one at a time */
	KT_ASSERT(rx_at("漢字", "かな漢字です", &a, &b, NULL));
	KT_ASSERT_EQ(a, 6);  KT_ASSERT_EQ(b, 12);
	KT_ASSERT(rx_at("[ぁ-ん]+", "かな漢字", &a, &b, NULL));
	KT_ASSERT_EQ(a, 0);  KT_ASSERT_EQ(b, 6);
	KT_ASSERT(rx_at("な.で", "かな漢字です", &a, &b, NULL) == FALSE);
	KT_ASSERT(rx_at("な..で", "かな漢字です", &a, &b, NULL));

	/* not patterns */
	KT_ASSERT_ER(rx_compile((CONST UB *)"(", &rx), E_PAR);
	KT_ASSERT_ER(rx_compile((CONST UB *)"a)", &rx), E_PAR);
	KT_ASSERT_ER(rx_compile((CONST UB *)"*a", &rx), E_PAR);
	KT_ASSERT_ER(rx_compile((CONST UB *)"[z-a]", &rx), E_PAR);

	/* a long run of one letter does not go deep */
	big = (char *)Kmalloc(5001);
	if ( big != NULL ) {
		for ( i = 0; i < 5000; i++ ) {
			big[i] = 'a';
		}
		big[5000] = 0;
		KT_ASSERT(rx_at("a.*a$", big, &a, &b, NULL));
		KT_ASSERT_EQ(b, 5000);
		Kfree(big);
	}
}

/*
 * かな漢字変換: romaji typed, converted and committed. With mozc
 * (make MOZC=1) にほんご becomes 日本語; without it the romaji converter
 * gives the kana and commits them as they are.
 */
LOCAL INT kc_type( INT kid, CONST char *keys, T_KCOUT *out )
{
	INT	r = 0;

	for ( ; *keys != 0; keys++ ) {
		r = kc_key(kid, (UINT)(UB)*keys, 0, out);
		if ( r < 0 ) {
			return r;
		}
	}
	return r;
}

LOCAL void test_kconv( void )
{
	T_KCOUT	*out = (T_KCOUT *)Kmalloc(sizeof(T_KCOUT));
	INT	kid, r;
	BOOL	kanji;

	start();
	KT_ASSERT(out != NULL);
	if ( out == NULL ) return;
	KT_ASSERT_ER(kc_start(), E_OK);
	kid = kc_open();
	KT_ASSERT(kid > 0);
	kanji = kc_kanji();
	tm_printf((UB *)"  converter: %s\n", kanji ? "mozc" : "romaji to kana");

	/* nothing composed: the keys are the application's */
	KT_ASSERT_EQ(kc_key(kid, TSMOZC_K_CR, 0, out), KC_NOTMINE);

	r = kc_type(kid, "nihongo", out);
	KT_ASSERT(r >= 0);
	KT_ASSERT(str_is(out->text, "にほんご"));
	KT_ASSERT_EQ(out->n_out, 0);
	KT_ASSERT_EQ(out->n_cl, 1);

	if ( kanji ) {
		r = kc_key(kid, TSMOZC_K_SPACE, 0, out);
		KT_ASSERT(r >= 0);
		tm_printf((UB *)"  converted: %s\n", out->text);
		KT_ASSERT(str_is(out->text, "日本語"));
	}
	r = kc_key(kid, TSMOZC_K_CR, 0, out);
	KT_ASSERT(r >= 0 && ( r & TSMOZC_OUT ) != 0);
	KT_ASSERT_EQ(out->n_out, 1);
	KT_ASSERT(str_is(out->text, kanji ? "日本語" : "にほんご"));

	if ( kanji ) {
		INT	count = 0, first = 0, total = 0;
		UB	*list = (UB *)Kmalloc(4096);

		/* かんじ, converted twice: the candidates are listed */
		(void)kc_type(kid, "kanji", out);
		(void)kc_key(kid, TSMOZC_K_SPACE, 0, out);
		r = kc_key(kid, TSMOZC_K_SPACE, 0, out);
		KT_ASSERT(r >= 0 && ( r & TSMOZC_LIST ) != 0);
		if ( list != NULL ) {
			r = kc_list(kid, list, 4096, &count, &first, &total);
			KT_ASSERT(r >= 1);
			KT_ASSERT(count >= 2);
			KT_ASSERT(total >= count);
			tm_printf((UB *)"  candidates: %d of %d, first %s\n", count, total, list);
			/* the second one chosen: it is committed */
			r = kc_choose(kid, first + 1, out);
			KT_ASSERT(r >= 0);
			Kfree(list);
		}
		(void)kc_key(kid, TSMOZC_K_CR, 0, out);
	}
	kc_close(kid);
	Kfree(out);
}

LOCAL void test_done( void )
{
	if ( wid > 0 ) {
		wm_close(wid);
		wid = 0;
		ready = FALSE;
	}
	/* the ground by itself, with nothing standing on it */
	KT_ASSERT_ER(wm_composite(), E_OK);
	kt_shot("/boot/DESK.PPM");
	KT_ASSERT_EQ(wm_self_check(), 0);
}

EXPORT void ktest_tv( void )
{
	KT_RUN(test_doc);
	KT_RUN(test_fig);
	KT_RUN(test_cabinet);
	KT_RUN(test_menu);
	KT_RUN(test_caret);
	KT_RUN(test_shapes);
	KT_RUN(test_pages);
	KT_RUN(test_png);
	KT_RUN(test_patterns);
	KT_RUN(test_elements);
	KT_RUN(test_infigure);
	KT_RUN(test_marks);
	KT_RUN(test_overlays);
	KT_RUN(test_bpk_forms);
	KT_RUN(test_bpk_draw);
	KT_RUN(test_regex);
	KT_RUN(test_kconv);
	KT_RUN(test_done);
}
