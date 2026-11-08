/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_om.c
 *	Virtual objects over the file system (design 16.3.4, 16.3.5).
 *
 *	The round trip: a <link> put into a document raises the reference
 *	count of the object it points at when the document is written, and
 *	taking it out again lowers it. The count is the one the file system
 *	holds, so it is read back with ob_ref_obj and has to survive the
 *	volume being detached and attached again.
 *
 *	The volume is the native one the earlier tests leave behind; it is
 *	made afresh only when there is none. The objects made here are left
 *	on it, so that the host tools have a volume with real reference
 *	counts to check.
 *
 *	The test task has a small stack, so a T_VOBJ and every buffer come
 *	from the heap.
 */

#include "ktest.h"
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsobj.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/fn.h>
#include "../../peripheral_kernel/obj/obj.h"

#define OMDEV		kt_scratch()

LOCAL ID	vol = 0;		/* above 0 once the volume is attached */
LOCAL TS_UUID	cabinet;		/* the document the links live in */
LOCAL TS_UUID	target;			/* what they point at */
LOCAL TS_UUID	first_vid;		/* the identity of the first link */
LOCAL BOOL	have_objs = FALSE;

LOCAL INT refcnt_of( CONST TS_UUID *uuid )
{
	T_OBREF	robj;

	if ( ob_ref_obj(uuid, &robj) < E_OK ) {
		return -1;
	}

	return robj.refcnt;
}

/* How many links the record on the volume really holds */
LOCAL INT links_on_disk( CONST TS_UUID *uuid )
{
	T_TAD	*doc = NULL;
	INT	n;

	if ( om_rea_doc(uuid, 0, NULL, &doc) < E_OK ) {
		return -1;
	}
	n = tad_lnk_count(doc);
	tad_free(doc);

	return n;
}

/* ---------------------------------------------------------------- */

/* the volume opens, and two objects with an empty document each are made */
LOCAL void test_objects( void )
{
	T_TAD	*doc = NULL;

	if ( OMDEV == NULL ) KT_SKIP(KT_NO_SCRATCH);
	vol = ( ob_att_vol(OMDEV, TSFS_STORE_BLK) >= E_OK ) ? 1 : 0;
	if ( vol <= 0 ) {
		/* nothing has made one yet: make one here */
		if ( ts_format_blk(OMDEV, "OBJECTS") < E_OK ) {
			KT_SKIP("no block device to make a volume on");
		}
		vol = ( ob_att_vol(OMDEV, TSFS_STORE_BLK) >= E_OK ) ? 1 : 0;
	}
	if ( vol <= 0 ) {
		KT_SKIP("the volume would not open");
	}

	KT_ASSERT_ER(om_cre_obj(OMDEV, (CONST UB *)"cabinet", OM_BODY_FIG,
				&cabinet), E_OK);
	KT_ASSERT_ER(om_cre_obj(OMDEV, (CONST UB *)"kiroku", OM_BODY_DOC,
				&target), E_OK);
	have_objs = TRUE;

	/* both start with nothing pointing at them */
	KT_ASSERT_EQ(refcnt_of(&cabinet), 0);
	KT_ASSERT_EQ(refcnt_of(&target), 0);

	/* and the document that was written parses */
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_EQ(tad_lnk_count(doc), 0);
		KT_ASSERT(tad_body(doc) != NULL);
		tad_free(doc);
	}

	/* a record that is not there is not a document */
	doc = NULL;
	KT_ASSERT_ER(om_rea_doc(&cabinet, 3, NULL, &doc), E_NOEXS);
}

/* a link written into the document raises the count of what it points at */
LOCAL void test_link( void )
{
	T_TAD	*doc = NULL;
	T_VOBJ	*v;
	INT	i;

	if ( !have_objs ) KT_SKIP("no objects");

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;

	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }

	v->target = target;
	v->left   = 4;
	v->top    = 9;
	v->right  = 102;
	v->bottom = 40;
	v->height = 31;
	v->chsz   = 14;
	v->disp   = TAD_D_DEFAULT;
	v->zoom   = 100;

	KT_ASSERT_ER(tad_lnk_add(doc, v, &first_vid), E_OK);
	KT_ASSERT_EQ(tad_lnk_ndelta(doc), 1);

	/* nothing has happened to the count until the document is written */
	KT_ASSERT_EQ(refcnt_of(&target), 0);

	KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
	KT_ASSERT_EQ(tad_lnk_ndelta(doc), 0);	/* what was owed is settled */
	tad_free(doc);

	KT_ASSERT_EQ(refcnt_of(&target), 1);
	KT_ASSERT_EQ(links_on_disk(&cabinet), 1);

	/* and what is on the volume says what was put there */
	doc = NULL;
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_find(doc, &first_vid, v), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &target), 0);
		KT_ASSERT_EQ(v->left, 4);
		KT_ASSERT_EQ(v->bottom, 40);
		tad_free(doc);
	}
	Kfree(v);
}

/*
 * A link to something that is not there. The native store counts the
 * links itself and takes one it cannot find for a link off the volume:
 * it is written, and counts nothing. It is taken out again, so that the
 * tests after this one find the document as it was.
 */
LOCAL void test_missing( void )
{
	T_TAD	*doc = NULL;
	T_VOBJ	*v;
	TS_UUID	ghost, gvid;
	INT	i;

	if ( !have_objs ) KT_SKIP("no objects");

	KT_ASSERT_ER(ts_gen_uuid(&ghost), E_OK);

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;

	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }

	v->target = ghost;
	v->width  = 150;
	v->heightpx = 30;
	v->disp   = TAD_D_DEFAULT;
	v->zoom   = 100;
	KT_ASSERT_ER(tad_lnk_add(doc, v, &gvid), E_OK);
	KT_ASSERT(om_store_counts(&cabinet));
	KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
	tad_free(doc);
	KT_ASSERT_EQ(refcnt_of(&target), 1);
	KT_ASSERT_EQ(links_on_disk(&cabinet), 2);

	doc = NULL;
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_del(doc, &gvid), E_OK);
		KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
		tad_free(doc);
	}
	KT_ASSERT_EQ(refcnt_of(&target), 1);
	KT_ASSERT_EQ(links_on_disk(&cabinet), 1);
	Kfree(v);
}

/* taking the link out again lowers the count, and zero is garbage */
LOCAL void test_unlink( void )
{
	T_TAD	*doc = NULL;
	TS_UUID	buf[8];
	INT	cnt = 0, i, found = 0;

	if ( !have_objs ) KT_SKIP("no objects");

	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc == NULL ) return;

	KT_ASSERT_ER(tad_lnk_del(doc, &first_vid), E_OK);
	KT_ASSERT_EQ(tad_lnk_count(doc), 0);
	KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
	tad_free(doc);

	KT_ASSERT_EQ(refcnt_of(&target), 0);
	KT_ASSERT_EQ(links_on_disk(&cabinet), 0);

	/* the count fell to zero, so it is on the garbage list */
	KT_ASSERT_ER(ts_lst_gc(knl_obfile_vol_of(&target), NULL, buf, 8, &cnt), E_OK);
	for ( i = 0; i < cnt; i++ ) {
		if ( ts_uuid_cmp(&buf[i], &target) == 0 ) found++;
	}
	KT_ASSERT_EQ(found, 1);
}

/* the count is on the volume, so it is still there after a remount */
LOCAL void test_remount( void )
{
	T_TAD	*doc = NULL;
	T_VOBJ	*v;
	INT	i;

	if ( !have_objs ) KT_SKIP("no objects");

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	if ( v == NULL ) return;
	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;

	/* put it back, so that the volume is left with a reference on it */
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc == NULL ) { Kfree(v); return; }
	v->target = target;
	v->left   = 4;
	v->top    = 9;
	v->right  = 102;
	v->bottom = 40;
	v->height = 31;
	v->chsz   = 14;
	v->disp   = TAD_D_DEFAULT;
	v->zoom   = 100;
	KT_ASSERT_ER(tad_lnk_add(doc, v, &first_vid), E_OK);
	KT_ASSERT_ER(om_wri_doc(&cabinet, 0, doc), E_OK);
	tad_free(doc);
	KT_ASSERT_EQ(refcnt_of(&target), 1);

	KT_ASSERT_ER(ob_det_vol(OMDEV), E_OK);
	vol = ( ob_att_vol(OMDEV, TSFS_STORE_BLK) >= E_OK ) ? 1 : 0;
	KT_ASSERT(vol > 0);
	if ( vol <= 0 ) { Kfree(v); return; }

	KT_ASSERT_EQ(refcnt_of(&target), 1);
	KT_ASSERT_EQ(links_on_disk(&cabinet), 1);

	/* the link is the one that was written, down to where it sits */
	doc = NULL;
	KT_ASSERT_ER(om_rea_doc(&cabinet, 0, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_find(doc, &first_vid, v), E_OK);
		KT_ASSERT_EQ(v->right, 102);
		tad_free(doc);
	}
	Kfree(v);
}

/*
 * The register of what a program has on the screen. It is filled one
 * virtual object at a time by whoever draws them, not by reading a
 * document (design 16.3.1).
 */
LOCAL void test_register( void )
{
	T_OMVOBJ	*out;
	T_OMREG		reg;
	T_VOBJ		*v;
	TS_UUID		id1, id2;
	ID		vid1 = 0, vid2 = 0, got = 0;
	INT		i;

	v   = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	out = (T_OMVOBJ *)Kmalloc(sizeof(T_OMVOBJ));
	KT_ASSERT(v != NULL && out != NULL);
	if ( v == NULL || out == NULL ) {
		if ( v != NULL ) Kfree(v);
		if ( out != NULL ) Kfree(out);
		return;
	}
	KT_ASSERT_ER(om_ini(), E_OK);
	KT_ASSERT_ER(om_ini(), E_OK);		/* twice is no error */
	KT_ASSERT_EQ(om_cnt_vob(), 0);

	for ( i = 0; i < (INT)sizeof(T_VOBJ); i++ ) ((UB *)v)[i] = 0;
	KT_ASSERT_ER(ts_gen_uuid(&id1), E_OK);
	KT_ASSERT_ER(ts_gen_uuid(&id2), E_OK);

	reg.left = 10; reg.top = 20; reg.right = 110; reg.bottom = 51;
	reg.wid = 3; reg.gid = 4; reg.image = NULL; reg.imagesz = 0;

	v->vobjid = id1;
	v->target = target;
	v->chsz   = 14;
	KT_ASSERT_ER(om_reg_vob(v, &reg, &vid1), E_OK);
	KT_ASSERT_EQ(om_cnt_vob(), 1);

	/* the same identity twice would be two views with one name */
	KT_ASSERT_ER(om_reg_vob(v, &reg, &vid2), E_OBJ);

	v->vobjid = id2;
	KT_ASSERT_ER(om_reg_vob(v, NULL, &vid2), E_OK);
	KT_ASSERT_EQ(om_cnt_vob(), 2);
	KT_ASSERT(vid1 != vid2);

	KT_ASSERT_ER(om_fnd_vob(&id1, &got), E_OK);
	KT_ASSERT_EQ(got, vid1);
	KT_ASSERT_ER(om_ref_vob(vid1, out), E_OK);
	KT_ASSERT_EQ(out->reg.right, 110);
	KT_ASSERT_EQ(out->reg.wid, 3);
	KT_ASSERT_EQ(out->vobj.chsz, 14);
	KT_ASSERT_EQ(ts_uuid_cmp(&out->target, &target), 0);

	/* it moved */
	reg.left = 40; reg.right = 140;
	KT_ASSERT_ER(om_set_vob(vid1, &reg), E_OK);
	KT_ASSERT_ER(om_ref_vob(vid1, out), E_OK);
	KT_ASSERT_EQ(out->reg.left, 40);

	/* and it is no longer drawn */
	KT_ASSERT_ER(om_del_vob(vid1), E_OK);
	KT_ASSERT_EQ(om_cnt_vob(), 1);
	KT_ASSERT_ER(om_ref_vob(vid1, out), E_NOEXS);
	KT_ASSERT_ER(om_fnd_vob(&id1, &got), E_NOEXS);
	KT_ASSERT_ER(om_del_vob(vid1), E_NOEXS);
	KT_ASSERT_ER(om_ref_vob(OM_MAX_VOBJ + 1, out), E_NOEXS);

	KT_ASSERT_ER(om_fin(), E_OK);
	KT_ASSERT_ER(om_reg_vob(v, &reg, &vid1), E_OBJ);	/* it is gone */

	Kfree(out);
	Kfree(v);
}

/*
 * How wide a closed link at 14 is made to hold the name: the name
 * measured with the system's face at 14, or six letters with no face
 */
LOCAL INT name_box_w( CONST UB *name )
{
	T_FNSTATE	st;
	ID		fid;
	INT		w = 14 * 6;

	fn_hold();
	fid = fn_system();
	if ( fid > 0 && fn_state_get(fid, &st) >= E_OK ) {
		if ( fn_set_size(fid, 14) >= E_OK ) {
			INT	m = fn_width(fid, name);

			if ( m >= 0 ) w = m;
		}
		(void)fn_state_set(fid, &st);
	}
	fn_release();

	return tad_closed_w(14, TAD_D_PICT | TAD_D_NAME | TAD_D_FRAME, w);
}

/*
 * Links a box is given while the system runs (om_obj_link_add): closed,
 * as wide as the name of the object each points at, the first 8 from
 * the box's left and top and the next 8 below it
 */
LOCAL void test_box_add( void )
{
	static CONST UB	short_nm[] = "仮身の幅";
	static CONST UB	long_nm[] = "a-rather-longer-name-of-an-object";
	TS_UUID		box, a, b;
	T_TAD		*doc = NULL;
	T_VOBJ		*v;
	UINT		disp = TAD_D_PICT | TAD_D_NAME | TAD_D_FRAME;
	INT		wa, wb;
	ID		was = fn_system(), face;

	if ( vol <= 0 ) KT_SKIP("no volume");
	face = kt_face();		/* the system's face, put back after */

	KT_ASSERT_ER(om_cre_obj(OMDEV, (CONST UB *)"box", OM_BODY_FIG, &box), E_OK);
	KT_ASSERT_ER(om_cre_obj(OMDEV, short_nm, OM_BODY_DOC, &a), E_OK);
	KT_ASSERT_ER(om_cre_obj(OMDEV, long_nm, OM_BODY_DOC, &b), E_OK);
	KT_ASSERT_ER(om_obj_link_add(&box, &a), E_OK);
	KT_ASSERT_ER(om_obj_link_add(&box, &b), E_OK);
	KT_ASSERT_ER(om_obj_link_add(&box, &a), E_OK);	/* linked already: left */

	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	KT_ASSERT(v != NULL);
	KT_ASSERT_ER(om_rea_doc(&box, 0, NULL, &doc), E_OK);
	if ( v == NULL || doc == NULL ) {
		if ( v != NULL ) Kfree(v);
		if ( doc != NULL ) tad_free(doc);
		(void)fn_set_system(was);
		return;
	}
	KT_ASSERT_EQ(tad_lnk_count(doc), 2);
	wa = name_box_w(short_nm);
	wb = name_box_w(long_nm);
	tm_printf((UB *)"  closed links added: %d and %d wide, %d high\n", wa, wb,
		  tad_closed_h(14, disp));

	if ( tad_lnk_get(doc, 0, v) >= E_OK ) {
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &a), 0);
		KT_ASSERT_EQ(v->left, 8);
		KT_ASSERT_EQ(v->top, 8);
		KT_ASSERT_EQ(v->bottom - v->top, tad_closed_h(14, disp));
		KT_ASSERT_EQ(v->right - v->left, wa);
		KT_ASSERT_EQ(v->chsz, 14);
	} else {
		KT_ASSERT(FALSE);
	}
	if ( tad_lnk_get(doc, 1, v) >= E_OK ) {
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &b), 0);
		KT_ASSERT_EQ(v->left, 8);
		KT_ASSERT_EQ(v->top, 8 + tad_closed_h(14, disp) + 8);
		KT_ASSERT_EQ(v->bottom - v->top, tad_closed_h(14, disp));
		KT_ASSERT_EQ(v->right - v->left, wb);
	} else {
		KT_ASSERT(FALSE);
	}
	/* with the system's face, each as wide as its own name */
	if ( face > 0 ) {
		KT_ASSERT(wa < wb);
		KT_ASSERT(wa >= tad_least_w(14, disp));
	} else {
		KT_ASSERT_EQ(wa, tad_closed_w(14, disp, 14 * 6));	/* no face: six letters */
	}
	tad_free(doc);
	Kfree(v);
	(void)fn_set_system(was);
}

/* leave the volume where the host tools can read it */
LOCAL void test_leave( void )
{
	if ( vol <= 0 ) KT_SKIP("no volume");

	KT_ASSERT_EQ(refcnt_of(&target), 1);
	KT_ASSERT_ER(ob_det_vol(OMDEV), E_OK);
	vol = 0;
}

EXPORT void ktest_om( void )
{
	KT_RUN(test_objects);
	KT_RUN(test_link);
	KT_RUN(test_missing);
	KT_RUN(test_unlink);
	KT_RUN(test_remount);
	KT_RUN(test_box_add);
	KT_RUN(test_register);
	KT_RUN(test_leave);
}
