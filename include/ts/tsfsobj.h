/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsobj.h
 *	Real objects on a native volume (design 11.6.6, 11.7)
 *
 *	One object is one object block: a fixed head of 512 bytes, which
 *	says where each part of the object is, and an inline area of 3584
 *	bytes that holds the parts small enough to fit. The parts are the
 *	metadata text, the placement table (one entry per record: its rid,
 *	its kind, its size and where its bytes are), the resource table and
 *	the link table. What does not fit inline goes into blocks of its
 *	own: management parts into blocks with the common head, which go
 *	through the journal; the bytes of records and resources into plain
 *	blocks, as one run or as an extent tree.
 *
 *	The object index (tsfsbtree.c) carries the UUID to the object block;
 *	everything else an object has is reached from that block.
 */

#ifndef __TS_TSFSOBJ_H__
#define __TS_TSFSOBJ_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>

#define TSFSO_HEAD_SIZE		512		/* the fixed head */
#define TSFSO_INLINE_BYTES	3584		/* the inline area behind it */
#define TSFSO_INLINE_UNIT	64		/* the inline area is handed out in these */

#define TSFSO_MAX_REC	TSFS_MAX_REC	/* records of one object */
#define TSFSO_MAX_RES	64		/* resources of one object */
#define TSFSO_EXT_LEN	16		/* the file name extension of a resource */
#define TSFSO_MAX_EXTENTS 165		/* runs one record's bytes may lie in */
#define TSFSO_MAX_LINK	1024		/* links of one object */

/* Flags of the head */
#define TSFSO_F_EDITABLE	0x0001
#define TSFSO_F_DELETABLE 0x0002
#define TSFSO_F_READABLE	0x0004
#define TSFSO_F_GARBAGE	0x0100		/* it is on the garbage list */
#define TSFSO_F_ORPHAN	0x0200		/* it goes when it is last closed */
#define TSFSO_F_RELINK	0x0400		/* its link table has to be made again */
#define TSFSO_F_AUTOREF	TSFS_F_AUTOREF	/* given out by ts_obj_ref only: never stored */

/* The record number of the icon, which belongs to no record */
#define TSFSO_ICON_REC	(-1)

/*
 * A volume of the native store. The identifier is the one the block
 * layer hands out, so ts_read_blk and the rest take it as they are.
 */
IMPORT ID ts_obj_mount( CONST char *devnm );
IMPORT ER ts_obj_unmount( ID vol );
IMPORT ER ts_obj_ref_vol( ID vol, T_RVOL *pk_rvol );

/*
 * Objects. Making one takes an object block, writes it and puts the UUID
 * in the index, all as one journal transaction.
 */
IMPORT ER ts_obj_create( ID vol, CONST UB *json, INT jsonlen, TS_UUID *p_uuid );

/* One made with an identity chosen for it; E_OBJ when there is one already */
IMPORT ER ts_obj_create_as( ID vol, CONST TS_UUID *uuid, CONST UB *json, INT jsonlen );
IMPORT ER ts_obj_delete( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_exists( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_ref( ID vol, CONST TS_UUID *uuid, T_ROBJ *pk_robj );
IMPORT ER ts_obj_list( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n,
		       INT *p_cnt );

/*
 * Reference counts and the garbage list (design 11.7). An object whose
 * count falls to zero goes on the list; one that is linked again comes
 * off it. Nothing is freed until it is collected, and an object that is
 * not deletable stays on the list however often it is collected.
 */
IMPORT ER ts_obj_link( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_unlink( ID vol, CONST TS_UUID *uuid );

/*
 * The count is the links to the object from the link tables of the
 * volume, and the holds ts_obj_link takes. A link table is made from the
 * xmlTAD records when the object is marked for it: a change to such a
 * record marks it in the same transaction, and ts_obj_relink does the
 * work -- when the object is closed, and at mount for what a cut in the
 * power left behind. Work that names many objects is done a part at a
 * time, each part a transaction that leaves the counts true.
 */
IMPORT ER ts_obj_relink( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_lst_lnk( ID vol, CONST TS_UUID *uuid, T_RLNK *buf, INT n, INT *p_cnt );

/*
 * An object deleted while it is open: out of the index at once, and on
 * the orphan tree until ts_obj_reap takes it when the last one closes,
 * or the next mount does. ts_obj_tidy finishes every object on the
 * orphan tree; mount and unmount call it.
 */
IMPORT ER ts_obj_orphan( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_reap( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_tidy( ID vol, INT *p_cnt );

/*
 * The patrol of a whole volume (design 11.14): the block layer's blocks,
 * every node of the three trees, and every object's block, management
 * blocks and extent leaves, read from the medium and checked. With
 * `data` the bytes of the records and resources are read as well, which
 * finds what the device can no longer read (they carry no checksum).
 */
IMPORT ER ts_obj_scrub( ID vol, BOOL data, T_TSFSSCRUB *pk_scrub );
IMPORT ER ts_obj_gc_list( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n,
			  INT *p_cnt );
IMPORT ER ts_obj_gc_one( ID vol, CONST TS_UUID *uuid );
IMPORT ER ts_obj_gc_all( ID vol, INT *p_cnt );

/* Metadata: the JSON text */
IMPORT ER ts_obj_get_meta( ID vol, CONST TS_UUID *uuid, UB *json, SZ size,
			   SZ *p_asize );
IMPORT ER ts_obj_set_meta( ID vol, CONST TS_UUID *uuid, CONST UB *json,
			   SZ size );
IMPORT ER ts_obj_set_name( ID vol, CONST TS_UUID *uuid, CONST UB *name );

/*
 * The icon, a management part beside the metadata: read (how large in
 * *p_asize; size 0 and buf NULL to ask only that), replaced whole (size
 * 0 takes it away), and given when the object is made.
 */
IMPORT ER ts_obj_get_icon( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize );
IMPORT ER ts_obj_get_icon_at( ID vol, CONST TS_UUID *uuid, D off, UB *buf, SZ size,
			      SZ *p_asize );
IMPORT ER ts_obj_set_icon( ID vol, CONST TS_UUID *uuid, CONST UB *icon, SZ size );
IMPORT ER ts_obj_create_icon( ID vol, CONST TS_UUID *as, CONST UB *json, INT jsonlen,
			      CONST UB *icon, SZ iconlen, TS_UUID *p_uuid );
IMPORT ER ts_obj_set_flags( ID vol, CONST TS_UUID *uuid, UINT flags,
			    UINT mask );

/* Records, by their place (0 on) */
IMPORT ER ts_obj_rea_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  void *buf, SZ size, SZ *p_asize );
IMPORT ER ts_obj_wri_rec( ID vol, CONST TS_UUID *uuid, INT recno, D off,
			  CONST void *buf, SZ size, SZ *p_asize );
IMPORT ER ts_obj_apd_rec( ID vol, CONST TS_UUID *uuid, UINT rectype,
			  INT *p_recno );
IMPORT ER ts_obj_trn_rec( ID vol, CONST TS_UUID *uuid, INT recno, UD newsize );
IMPORT ER ts_obj_del_rec( ID vol, CONST TS_UUID *uuid, INT recno );
IMPORT ER ts_obj_lst_rec( ID vol, CONST TS_UUID *uuid, T_RREC *buf, INT n,
			  INT *p_cnt );

/*
 * The order of the records is the member "records" of the metadata
 * (design 11.5): a record put in or moved changes that only, never the
 * placement table.
 */
IMPORT ER ts_obj_ins_rec( ID vol, CONST TS_UUID *uuid, INT pos, UINT rt, UINT sub,
			  INT *p_pos );
IMPORT ER ts_obj_mov_rec( ID vol, CONST TS_UUID *uuid, INT from, INT to );
IMPORT ER ts_obj_set_rtp( ID vol, CONST TS_UUID *uuid, INT recno, UINT rt, UINT sub );

/*
 * A record's bytes replaced whole. The new bytes go to a new place, the
 * placement table is changed in the same transaction, and the old place
 * is given back once that transaction is real: after a cut in the power
 * the record is either all old or all new (design 11.14.3).
 */
IMPORT ER ts_obj_rpl_rec( ID vol, CONST TS_UUID *uuid, INT recno, CONST void *buf,
			  SZ size );

/*
 * Resources: bytes named by (record, number) with a file name extension,
 * the icon being the one whose record number is TSFSO_ICON_REC.
 */
IMPORT ER ts_obj_rea_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  void *buf, SZ size, SZ *p_asize, UB *ext );
IMPORT ER ts_obj_wri_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			  CONST UB *ext, CONST void *buf, SZ size );
IMPORT ER ts_obj_del_res( ID vol, CONST TS_UUID *uuid, INT recno, INT resno );

/* A resource read from a place in it on; the rest as ts_obj_rea_res */
IMPORT ER ts_obj_rea_res_at( ID vol, CONST TS_UUID *uuid, INT recno, INT resno,
			     D off, void *buf, SZ size, SZ *p_asize, UB *ext );

/* What resources an object has */
typedef struct {
	INT	recno;
	INT	resno;
	UB	ext[TSFSO_EXT_LEN];
	UD	size;
} T_TSFSRES;

IMPORT ER ts_obj_lst_res( ID vol, CONST TS_UUID *uuid, T_TSFSRES *buf, INT n, INT *p_cnt );

/*
 * Several changes as one transaction. Calls between a begin and its end
 * go into one journal transaction, so that a write and the reference
 * count it changes become real together.
 */
IMPORT ER ts_obj_begin( ID vol );
IMPORT ER ts_obj_end( ID vol, BOOL commit );

/*
 * The metadata scanner, which lives in kernel/fs/tsfs.c and is used by
 * both stores. Only the keys of the outer object are looked at, so a key
 * of the same name inside a nested object is not taken by mistake.
 */
IMPORT INT knl_json_find( CONST UB *j, INT len, CONST char *key );
IMPORT INT knl_json_get_num( CONST UB *j, INT len, CONST char *key, INT dflt );
IMPORT void knl_json_get_str( CONST UB *j, INT len, CONST char *key, UB *out,
			      INT max );
IMPORT INT knl_json_set_num( UB *j, INT len, INT max, CONST char *key,
			     INT value );
IMPORT INT knl_json_set_str( UB *j, INT len, INT max, CONST char *key,
			     CONST UB *value );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSOBJ_H__ */
