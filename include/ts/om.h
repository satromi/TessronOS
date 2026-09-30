/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om.h
 *	Real objects and virtual objects for a program (design 16.3, 16.4.1)
 *
 *	Protection level 3: a library the desktop and every editor links,
 *	not a manager in the kernel. Two things live here.
 *
 *	The register of virtual objects a program has on the screen. A
 *	program puts one in when it has drawn a <link> of its document and
 *	takes it out again when it stops drawing it. The register is not
 *	filled by reading a document: what is on the screen is what the
 *	program says is on the screen (design 16.3.1).
 *
 *	The round trip of the reference count. A <link> added to a document
 *	is a reference to another real object, so writing the document and
 *	raising the count of what it points at belong together; om_wri_doc
 *	does both in one transaction (design 16.3.4, 11.7).
 */

#ifndef __TS_OM_H__
#define __TS_OM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/tad.h>
#include <ts/dp.h>

#define OM_MAX_VOBJ	256		/* virtual objects one program registers */

/* What the body of a new object is */
#define OM_BODY_DOC	0		/* <document>: text */
#define OM_BODY_FIG	1		/* <figure>: drawing, and a cabinet */

/*
 * Where a registered virtual object is and what belongs to it. The
 * rectangle is in the coordinates of the window it is drawn in. `image`
 * is the place a program keeps what it read out of the object itself,
 * which the format does not hold and the register does (design 16.3.2).
 */
typedef struct {
	INT	left, top, right, bottom;
	ID	wid;			/* the window, 0 while there is none */
	ID	gid;			/* the drawing environment, 0 while none */
	void	*image;			/* the program keeps this, the register does not */
	SZ	imagesz;
} T_OMREG;

typedef struct {
	TS_UUID	vobjid;			/* identity of the link */
	TS_UUID	target;			/* the real object it refers to */
	T_OMREG	reg;
	T_VOBJ	vobj;			/* how it is drawn, as the document holds it */
} T_OMVOBJ;

/*
 * The register. om_ini makes it and om_fin gives it back; the calls
 * between them answer E_OBJ until it is there.
 */
IMPORT ER om_ini( void );
IMPORT ER om_fin( void );
IMPORT ER om_reg_vob( CONST T_VOBJ *vobj, CONST T_OMREG *reg, ID *p_vid );
IMPORT ER om_del_vob( ID vid );
IMPORT ER om_set_vob( ID vid, CONST T_OMREG *reg );

/*
 * Say that what the link holds has changed: its colours, its name, what
 * it shows. The identity and what it points at cannot change this way,
 * because those make it the link it is.
 */
IMPORT ER om_upd_vob( ID vid, CONST T_VOBJ *vobj );
IMPORT ER om_ref_vob( ID vid, T_OMVOBJ *pk_vobj );
IMPORT ER om_fnd_vob( CONST TS_UUID *vobjid, ID *p_vid );
IMPORT INT om_cnt_vob( void );

/*
 * Documents. om_rea_doc reads a record and parses it; om_wri_doc writes
 * it back and settles what the links added and removed since it was read
 * owe the reference counts, both inside one transaction, so that a power
 * cut cannot leave the count disagreeing with the text.
 *
 * The counts are changed before the record is written: a link to an
 * object that is not there has to stop the write, and the bytes of a
 * record are not taken back by an abort while the header is.
 *
 * On a store with no log (TSFS-on-FAT) the two are done one after the
 * other, which is all that store can do.
 */
/* ---------------------------------------------------------------- drawing */

/*
 * What a virtual object looks like. Every measurement of the frame is
 * here and nowhere else, so that the look can be changed in one place
 * (the same rule the work area of a window follows, design 16.4).
 */
#define OM_FRAME_W	1		/* the line around it */
#define OM_TITLE_H	16		/* the band the name sits in */
#define OM_ICON_W	16		/* the picture at the left of the band */

/* What a colour that the document did not give falls back to */
#define OM_DEF_FRCOL	0x00000000U
#define OM_DEF_CHCOL	0x00000000U
#define OM_DEF_TBCOL	0x00E1F2F9U
#define OM_DEF_BGCOL	0x00FFFFFFU

/*
 * Draw a registered virtual object through a drawing environment, in the
 * rectangle the register holds for it. What is drawn is decided by the
 * link's own display flags: the frame, the band, the picture.
 *
 * The name is not drawn yet. There is no font layer (design 16.6, stage
 * 9c), and drawing something else in its place would put a mark on the
 * screen that no document asked for.
 */
IMPORT ER om_dsp_vob( INT gid, ID vid );

/*
 * The same, for a link that is not in the register: what shows a
 * document draws the links standing in it, and showing something is not
 * a reason to keep it.
 */
IMPORT ER om_draw_vob( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj );

/*
 * The band along the top of one: where the picture sits, where the name
 * sits, and how tall it is. A virtual object big enough to show what it
 * points at -- an open one -- keeps the band and gives the rest to the
 * thing it points at, so whoever draws that needs to know where the
 * band ends.
 */
/* ----------------------------------------------------- the store of objects */

/*
 * The real objects a program shows and edits, read and written through
 * the basic operations of the object layer (ts/ob.h, design 18): the
 * metadata, the records, and the resources beside them. om_store_files
 * attaches the volume they are on and installs the store as what a link
 * is drawn from, with the name and the content of the object it points
 * at.
 *
 * What is read is kept until the store is emptied, because a link is
 * drawn again on every screen and reading it again each time is the
 * whole of what makes drawing slow.
 */
IMPORT ER    om_store_files( CONST char *dir );
IMPORT void  om_store_empty( void );

/* One record, and one name, by identity. What comes back is the store's */
IMPORT T_TAD *om_store_get( CONST TS_UUID *id, INT recno );
IMPORT INT   om_store_name( CONST TS_UUID *id, UB *buf, INT max );

/*
 * A record written back to its file, as it now stands: what was changed,
 * and everything else exactly as it was read.
 */
IMPORT ER    om_store_save( CONST TS_UUID *id, INT recno );

/*
 * A record as text, and a record put back from text. Taking back a
 * change is putting back the record as it was before it; everything
 * drawn from the record is then drawn again from the one put back.
 */
/*
 * Where a window showing the object last stood, from its metadata, and
 * where it stands now, written back. Answers E_NOEXS when the metadata
 * says nothing of a window.
 */
IMPORT ER    om_store_window( CONST TS_UUID *id, T_DPRECT *r );
IMPORT ER    om_store_set_window( CONST TS_UUID *id, CONST T_DPRECT *r );

/* Whether the object's applist makes this program the one it opens with */
IMPORT BOOL  om_store_opens_with( CONST TS_UUID *id, CONST char *app );

/*
 * The metadata changed: the name (and the record's own name for itself),
 * what the object is written on, and its relationship tags, given and
 * read as "[tag] [tag]".
 */
IMPORT ER    om_store_rename( CONST TS_UUID *id, CONST UB *name );
IMPORT ER    om_store_set_paper( CONST TS_UUID *id, UW colour );
IMPORT INT   om_store_relationship( CONST TS_UUID *id, UB *buf, INT max );
IMPORT ER    om_store_set_relationship( CONST TS_UUID *id, CONST UB *tags );

/* The programs the applist names, by the names they are shown under */
#define OM_APP_NAME	64
IMPORT INT   om_store_apps( CONST TS_UUID *id, UB (*names)[OM_APP_NAME],
			    INT max );

/*
 * The identity of the program at a place of the applist (the order of
 * om_store_apps), and of the one the object opens with. The length, or
 * -1 when there is none.
 */
IMPORT INT   om_store_app_id( CONST TS_UUID *id, INT index, UB *buf, INT max );
IMPORT INT   om_store_default_app( CONST TS_UUID *id, UB *buf, INT max );

/*
 * The applist written anew: n programs by identity and shown name, the
 * one at 'dflt' (0 up, -1 none) the one the object opens with.
 */
#define OM_APP_ID	40		/* bytes of a program's identity */
IMPORT ER    om_store_set_apps( CONST TS_UUID *id, CONST UB *CONST *ids,
				CONST UB *CONST *names, INT n, INT dflt );

/*
 * The program the object opens with made one the applist already names
 * (its identity): E_NOEXS when the list does not name it.
 */
IMPORT ER    om_store_set_default_app( CONST TS_UUID *id, CONST UB *app );

/*
 * The icon of the template of a program the program box holds, by the
 * program's identity: what an object that opens in it is given when made.
 * NULL when there is none; the caller's to Kfree.
 */
IMPORT UB   *om_store_app_icon( CONST UB *app, SZ *p_size );

/* What the metadata says of an object, for 管理情報 */
#define OM_INFO_TEXT	40
typedef struct {
	UB	name[TAD_NAME_MAX];
	UB	maker[OM_INFO_TEXT];
	UB	made[OM_INFO_TEXT];
	UB	updated[OM_INFO_TEXT];
	UB	accessed[OM_INFO_TEXT];
	INT	refs;
	INT	records;
	INT	bytes;			/* of record 0 */
} T_OMINFO;
IMPORT ER    om_store_info( CONST TS_UUID *id, T_OMINFO *info );

/*
 * The programs a link opens with: those of the object's applist, or,
 * when the object has none, those of the applist the link carries.
 */
IMPORT INT   om_link_apps( CONST T_VOBJ *v, UB (*names)[OM_APP_NAME], INT max );
IMPORT INT   om_link_app_id( CONST T_VOBJ *v, INT index, UB *buf, INT max );
IMPORT INT   om_link_default_app( CONST T_VOBJ *v, UB *buf, INT max );

/*
 * What a link's band says, as the link asks (the name, " : " and the
 * relationship, " (" the program ")", the date), and the object's icon,
 * kept by the store.
 */
IMPORT INT   om_store_label( CONST T_VOBJ *v, UB *buf, INT max );
IMPORT ER    om_store_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h );

/* The icon of an object replaced (an ICO or a PNG; 0 bytes: none), and shown afresh */
IMPORT ER    om_store_set_icon( CONST TS_UUID *id, CONST UB *buf, SZ size );

/* The maker of an object set to the user the caller acts for */
IMPORT ER    om_store_set_maker( CONST TS_UUID *id );

/*
 * One object read or written whole through the object layer (om_obj.c):
 * the metadata, a record, a resource. What is read belongs to the caller
 * and ends in a nought. A record written whole replaces the one there,
 * or is added when it is the next number.
 */
#define OM_REC_MAX	64
IMPORT UB   *om_obj_meta( CONST TS_UUID *id, SZ *p_size );
IMPORT ER    om_obj_meta_put( CONST TS_UUID *id, CONST UB *buf, SZ size );
IMPORT INT   om_obj_records( CONST TS_UUID *id, T_OBREC *buf, INT max );
IMPORT UB   *om_obj_record( CONST TS_UUID *id, INT recno, SZ *p_size );
IMPORT ER    om_obj_record_put( CONST TS_UUID *id, INT recno, UINT rt, UINT sub,
				CONST UB *buf, SZ size );
IMPORT BOOL  om_obj_href( CONST UB *href, TS_UUID *id, CONST UB **p_name );
IMPORT UB   *om_obj_res( CONST TS_UUID *id, CONST UB *name, SZ *p_size );
IMPORT UB   *om_obj_icon( CONST TS_UUID *id, SZ *p_size );	/* the icon, the caller's to Kfree */
IMPORT ER    om_obj_icon_put( CONST TS_UUID *id, CONST UB *buf, SZ size );
IMPORT ER    om_obj_res_put( CONST TS_UUID *id, CONST UB *name, CONST UB *buf, SZ size );
IMPORT INT   om_obj_res_list( CONST TS_UUID *id, UB *buf, SZ size );

/* A link to an object added to a box (a figure of links), when it has none to it */
IMPORT ER    om_obj_link_add( CONST TS_UUID *box, CONST TS_UUID *uuid );

/* Every link of a box to an object taken out */
IMPORT ER    om_obj_link_del( CONST TS_UUID *box, CONST TS_UUID *uuid );

/*
 * A real object copied under a new identity and given a name. 'deep'
 * copies what it links to as well; otherwise the copy's links point at
 * the same objects as the original's, and those objects are counted as
 * linked to once more. Answers the copy's
 * identity. The copy is written to the store's directory; nothing of it
 * is read until something opens it.
 */
IMPORT ER    om_store_copy( CONST TS_UUID *src, CONST UB *name,
			    BOOL deep, TS_UUID *p_new );

IMPORT ER    om_store_snapshot( CONST TS_UUID *id, INT recno, UB **p_xml,
				SZ *p_size );
IMPORT T_TAD *om_store_restore( CONST TS_UUID *id, INT recno,
				CONST UB *xml, SZ size );

/*
 * How many links point at each object, kept in its metadata. Saving a
 * record moves the counts of what it links to by what changed; an
 * object no link reaches is a 屑実身.
 *
 *   om_store_bump	one object's count moved (never below nought)
 *   om_store_refs	one object's count; -1 when the metadata has none
 *   om_store_links	what the links of a record's text point at
 *   om_store_relink	counts moved from one text of a record to another
 *   om_store_objects	every object in the store
 *   om_store_delete	an object thrown away for good, with its files
 *   om_store_recount	every count made again from the records, with
 *			'roots' -- what the system itself holds -- once each
 *   om_store_forget	what is held of an object in memory, let go
 *   om_store_counts	whether the store of an object counts the links of
 *			its records itself (the native store, design 11.7):
 *			then none of the above moves counts for its links
 */
IMPORT BOOL  om_store_counts( CONST TS_UUID *id );
IMPORT void  om_store_bump( CONST TS_UUID *id, INT more );
IMPORT INT   om_store_refs( CONST TS_UUID *id );
IMPORT INT   om_store_links( CONST UB *buf, SZ len, TS_UUID *ids, INT max );
IMPORT void  om_store_relink( CONST UB *was, SZ was_len, CONST UB *now,
			      SZ now_len );
IMPORT INT   om_store_objects( TS_UUID *ids, INT max );
IMPORT ER    om_store_delete( CONST TS_UUID *id );
IMPORT ER    om_store_recount( CONST TS_UUID *roots, INT nroot,
			       INT *p_total, INT *p_changed );
IMPORT void  om_store_forget( CONST TS_UUID *id );

/* A record as last written, whole; the caller frees it */
IMPORT UB   *om_store_read( CONST TS_UUID *id, INT recno, SZ *p_len );

/* What an object is written on, out of its metadata */
IMPORT ER    om_store_paper( CONST TS_UUID *id, UW *p_colour );

/*
 * A picture beside the records, by the name a record gives it: its
 * pixels as they are kept (not to be freed), and the same written back
 * as a PNG.
 */
IMPORT ER    om_store_picture( CONST UB *href, CONST UW **p_px, INT *p_w,
			       INT *p_h );
IMPORT ER    om_store_put_picture( CONST UB *href, CONST UW *px, INT w, INT h );

IMPORT INT om_band_h( CONST T_VOBJ *vobj );

/*
 * How much taller than its band a virtual object's box has to be before
 * it counts as opened. A closed one is a pixel or two taller than its
 * band from the rounding of whatever laid it out; this is the slack
 * that allows for that and nothing more.
 */
#define OM_OPEN_SLACK	8

/*
 * Draw one that is open: the same band, and the rectangle under it left
 * empty for the caller to draw the real object into. Answers that inner
 * rectangle.
 */
IMPORT ER om_draw_open( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			T_DPRECT *p_inner );

/*
 * What the caller made of a link to draw it with: the words of its band
 * (the name, then " : " and the relationship, " (" the kind ")" and the
 * date, as the link asks) and the object's own icon, 0x00rrggbb with
 * IMG_CLEAR where it is clear. NULL for either draws the link's own
 * name, or the turned leaf.
 */
#define OM_LOOK_TEXT	( TAD_NAME_MAX + TAD_REL_MAX * 2 + 96 )
typedef struct {
	CONST UB	*text;
	CONST UW	*icon;
	INT		icon_w, icon_h;
} T_OMLOOK;

/*
 * The room an opened object keeps from its edge when it has no frame,
 * and the same drawing with the caller's words and icon; 'open' says
 * whether it is an opened one.
 */
#define OM_OPEN_MARGIN	10
IMPORT ER om_draw_vob_as( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			  CONST T_OMLOOK *look, BOOL open, T_DPRECT *p_inner );

/*
 * Show or take back the mark that says a virtual object is picked. It is
 * drawn by inverting, so the same call twice leaves the screen as it
 * was, and nothing needs to be remembered to undo it.
 */
IMPORT ER om_sel_vob( INT gid, ID vid, BOOL on );

/*
 * A document object made (on the volume attached with 'vol', NULL for
 * the first), a record of one read as a document the caller frees, and
 * a document written back with the counts its links owe settled.
 */
IMPORT ER om_cre_obj( CONST char *vol, CONST UB *name, UINT body, TS_UUID *p_uuid );
IMPORT ER om_rea_doc( CONST TS_UUID *uuid, INT recno, CONST T_TADLIM *lim,
		      T_TAD **p_doc );
IMPORT ER om_wri_doc( CONST TS_UUID *uuid, INT recno, T_TAD *doc );

#ifdef __cplusplus
}
#endif

#endif /* __TS_OM_H__ */
