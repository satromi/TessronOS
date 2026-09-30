/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tray.c
 *	The tray (design 18.16)
 *
 *	A set of several kinds of tray record put in and read back byte for
 *	byte; the levels kept newest first and the oldest falling off; the
 *	level in hand chosen, added to, moved and taken out; the place of
 *	its own; and the tray as a real object whose record 0 says what it
 *	holds.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/tray.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/sysdef.h>
#include <ts/tad.h>
#include "../../application/desktop/desktop.h"

LOCAL CONST char	tad[] = "<p>トレーの文字<link id=\"0199ce60-532a-7670-b8bc-84c970de11dc_0.xtad\"/></p>";
LOCAL CONST UB		png[] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13 };
LOCAL CONST UB		prog[] = { 0x7F, 'E', 'L', 'F', 2, 1, 1, 0 };

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL same_name( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

/* A set of one xmlTAD record, named by a number */
LOCAL ER push_one( INT k )
{
	T_TRREC	r;
	UB	name[8];

	name[0] = (UB)( '0' + k / 10 );
	name[1] = (UB)( '0' + k % 10 );
	name[2] = 0;
	r.kind = TR_TAD;
	r.rt = r.sub = 0;
	r.data = tad;
	r.size = sizeof(tad) - 1;
	return tr_psh_dat(&r, 1, name);
}

/* several kinds in one set, and each read back as it went in */
LOCAL void test_set( void )
{
	T_TRREC	r[3];
	T_TRINF	inf;
	T_TRSTS	*sts;
	UB	buf[128];
	SZ	asz = 0;

	KT_ASSERT_ER(tr_clr_tra(), E_OK);
	r[0].kind = TR_TAD;  r[0].rt = 0;  r[0].sub = 0;  r[0].data = tad;  r[0].size = sizeof(tad) - 1;
	r[1].kind = TR_PNG;  r[1].rt = 0;  r[1].sub = 0;  r[1].data = png;  r[1].size = sizeof(png);
	r[2].kind = TR_REC;  r[2].rt = OB_RT_PROG;  r[2].sub = 5;  r[2].data = prog;  r[2].size = sizeof(prog);
	KT_ASSERT_EQ(tr_psh_dat(r, 3, (CONST UB *)"三つ"), 3);

	sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	KT_ASSERT(sts != NULL);
	if ( sts == NULL ) return;
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT_EQ(sts->nlevel, 1);
	KT_ASSERT_EQ(sts->hand, 1);
	KT_ASSERT_EQ(sts->set[1].nrec, 3);
	KT_ASSERT_EQ(sts->set[1].kinds, ( 1U << TR_TAD ) | ( 1U << TR_PNG ) | ( 1U << TR_REC ));
	KT_ASSERT(same_name(sts->set[1].name, "三つ"));
	Kfree(sts);

	KT_ASSERT_ER(tr_ref_rec(TR_HAND, 2, &inf), E_OK);
	KT_ASSERT_EQ(inf.kind, TR_REC);
	KT_ASSERT_EQ(inf.rt, OB_RT_PROG);
	KT_ASSERT_EQ(inf.sub, 5);
	KT_ASSERT_EQ(inf.size, sizeof(prog));
	KT_ASSERT_ER(tr_rea_rec(1, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(tad) - 1);
	KT_ASSERT(same_bytes(buf, (CONST UB *)tad, sizeof(tad) - 1));
	KT_ASSERT_ER(tr_rea_rec(1, 1, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT(same_bytes(buf, png, sizeof(png)));
	KT_ASSERT_ER(tr_rea_rec(1, 2, buf, 4, &asz), E_OK);	/* too little room: what fits */
	KT_ASSERT_EQ(asz, sizeof(prog));
	KT_ASSERT(same_bytes(buf, prog, 4));
	KT_ASSERT_ER(tr_ref_rec(1, 3, &inf), E_NOEXS);
}

/* the levels: newest first, the oldest falling off below the last */
LOCAL void test_levels( void )
{
	T_TRSTS	*sts;
	INT	k;

	KT_ASSERT_ER(tr_clr_tra(), E_OK);
	for ( k = 1; k <= TR_LEVELS + 2; k++ ) {
		KT_ASSERT_EQ(push_one(k), 1);
	}
	sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	KT_ASSERT(sts != NULL);
	if ( sts == NULL ) return;
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT_EQ(sts->nlevel, TR_LEVELS);
	KT_ASSERT(same_name(sts->set[1].name, "12"));
	KT_ASSERT(same_name(sts->set[TR_LEVELS].name, "03"));	/* 01 and 02 fell off */

	/* another level in hand; what is added goes to it */
	KT_ASSERT_ER(tr_sel_dat(3), E_OK);
	KT_ASSERT_EQ(push_one(99) >= E_OK, TRUE);		/* a new set takes the hand */
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT_EQ(sts->hand, 1);
	KT_ASSERT_ER(tr_sel_dat(3), E_OK);
	{
		T_TRREC	r;

		r.kind = TR_PNG;  r.rt = 0;  r.sub = 0;  r.data = png;  r.size = sizeof(png);
		KT_ASSERT_EQ(tr_add_dat(&r, 1), 1);
	}
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT_EQ(sts->set[3].nrec, 2);
	KT_ASSERT(same_name(sts->set[3].name, "11"));		/* the name it had */
	KT_ASSERT_EQ(sts->set[3].kinds, ( 1U << TR_TAD ) | ( 1U << TR_PNG ));

	/* moved to the top, the hand going with it */
	KT_ASSERT_ER(tr_mov_dat(3, 1), E_OK);
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT(same_name(sts->set[1].name, "11"));
	KT_ASSERT(same_name(sts->set[2].name, "99"));
	KT_ASSERT_EQ(sts->hand, 1);

	/* taken out: the one after comes into the hand */
	KT_ASSERT_ER(tr_del_dat(TR_HAND), E_OK);
	KT_ASSERT_ER(tr_get_sts(sts), E_OK);
	KT_ASSERT_EQ(sts->nlevel, TR_LEVELS - 1);
	KT_ASSERT(same_name(sts->set[1].name, "99"));
	KT_ASSERT_EQ(sts->hand, 1);

	/* what cannot be */
	KT_ASSERT_ER(tr_sel_dat(0), E_PAR);
	KT_ASSERT_ER(tr_sel_dat(TR_LEVELS), E_PAR);
	KT_ASSERT_ER(tr_psh_dat(NULL, 1, NULL), E_PAR);
	{
		T_TRREC	r;

		r.kind = 9;  r.rt = 0;  r.sub = 0;  r.data = png;  r.size = sizeof(png);
		KT_ASSERT_ER(tr_psh_dat(&r, 1, NULL), E_PAR);
	}
	Kfree(sts);
}

/* the place of its own, apart from the levels */
LOCAL void test_put( void )
{
	T_TRREC	r;
	T_TRSTS	*sts;
	UB	buf[16];
	SZ	asz = 0;

	r.kind = TR_PNG;  r.rt = 0;  r.sub = 0;  r.data = png;  r.size = sizeof(png);
	KT_ASSERT_ER(tr_set_dat(&r, 1), E_OK);
	KT_ASSERT_ER(tr_rea_rec(TR_PUT, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(png));
	sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	if ( sts != NULL ) {
		KT_ASSERT_ER(tr_get_sts(sts), E_OK);
		KT_ASSERT_EQ(sts->set[TR_PUT].nrec, 1);
		KT_ASSERT_EQ(sts->nlevel, TR_LEVELS - 1);	/* the levels are as they were */
		Kfree(sts);
	}
	KT_ASSERT_ER(tr_set_dat(NULL, 0), E_OK);
	KT_ASSERT_ER(tr_rea_rec(TR_PUT, 0, buf, sizeof(buf), &asz), E_NOEXS);
}

/* the tray is a real object: record 0 lists what it holds */
LOCAL void test_object( void )
{
	TS_UUID	u;
	T_OBREF	r;
	T_TRSTS	*sts;
	UB	*rec;
	SZ	size = 0;
	INT	i;
	BOOL	hand = FALSE;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_TRAY, &u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_MEMORY);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL);
	for ( i = 0; rec != NULL && i + 3 <= (INT)size && !hand; i++ ) {
		hand = same_bytes(rec + i, (CONST UB *)"▶", 3);
	}
	KT_ASSERT(hand);
	if ( rec != NULL ) Kfree(rec);

	KT_ASSERT_ER(tr_clr_tra(), E_OK);
	sts = (T_TRSTS *)Kmalloc(sizeof(T_TRSTS));
	if ( sts != NULL ) {
		KT_ASSERT_ER(tr_get_sts(sts), E_OK);
		KT_ASSERT_EQ(sts->nlevel, 0);
		KT_ASSERT_EQ(sts->hand, 0);
		Kfree(sts);
	}
	KT_ASSERT_ER(tr_rea_rec(TR_HAND, 0, NULL, 0, &size), E_NOEXS);
}

/*
 * What the desktop's windows put in the tray: a <figure> fragment of a
 * shape and a link, read back as a figure with its shapes and links
 * counted; a <document> fragment read back as text.
 */
LOCAL void test_frag( void )
{
	CONST char	*xml = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
			       "<rect l=\"1\" t=\"2\" r=\"30\" b=\"40\"><p>中</p></rect>"
			       "</figure></tad>";
	T_TAD		*src = NULL, *frag, *back;
	T_TADNODE	*body, *copy;
	T_VOBJ		v;
	TS_UUID		id;
	INT		links = -1, n = 0;

	KT_ASSERT_ER(tr_clr_tra(), E_OK);
	while ( xml[n] != 0 ) n++;
	KT_ASSERT_ER(tad_parse((CONST UB *)xml, (SZ)n, NULL, &src), E_OK);
	frag = dt_frag_new("figure");
	KT_ASSERT(frag != NULL && src != NULL);
	if ( frag == NULL || src == NULL ) return;

	/* a shape copied whole, with what it holds, from one document to another */
	body = tad_body(frag);
	copy = tad_node_copy(frag, body, NULL, tad_body(src)->first);
	KT_ASSERT(copy != NULL && copy->first != NULL && copy->first->first != NULL);
	knl_memset(&v, 0, sizeof(v));
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_MENU_WINDOW, &v.target), E_OK);
	v.right = 100;
	v.bottom = 20;
	KT_ASSERT_ER(tad_lnk_add(frag, &v, &id), E_OK);
	KT_ASSERT_ER(dt_tray_put(frag, "図形と仮身2個"), 1);
	tad_free(frag);
	tad_free(src);

	KT_ASSERT_EQ(dt_tray_kind(&links), DT_TRAY_FIG);
	KT_ASSERT_EQ(links, 1);
	back = dt_tray_frag();
	KT_ASSERT(back != NULL);
	if ( back != NULL ) {
		KT_ASSERT_EQ(dt_frag_shapes(back), 1);
		KT_ASSERT_EQ(tad_lnk_count(back), 1);
		tad_free(back);
	}

	/* a text on top of it: the hand moves to it, and the figure is still there */
	frag = dt_frag_new("document");
	KT_ASSERT(frag != NULL);
	if ( frag == NULL ) return;
	body = tad_body(frag);
	KT_ASSERT(tad_text_new(frag, tad_elem_new(frag, "p", body, NULL, FALSE), NULL,
			       (CONST UB *)"文字", 6) != NULL);
	KT_ASSERT_ER(dt_tray_put(frag, "文字列"), 1);
	tad_free(frag);
	KT_ASSERT_EQ(dt_tray_kind(NULL), DT_TRAY_DOC);
	KT_ASSERT_ER(tr_sel_dat(2), E_OK);
	KT_ASSERT_EQ(dt_tray_kind(NULL), DT_TRAY_FIG);

	/* moving from the tray takes the set out */
	dt_tray_taken();
	KT_ASSERT_EQ(dt_tray_kind(NULL), DT_TRAY_DOC);
	dt_tray_taken();
	KT_ASSERT_EQ(dt_tray_kind(NULL), DT_TRAY_NONE);
}

EXPORT void ktest_tray( void )
{
	KT_RUN(test_set);
	KT_RUN(test_levels);
	KT_RUN(test_put);
	KT_RUN(test_object);
	KT_RUN_SCREEN(test_frag);
}
