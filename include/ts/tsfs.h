/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfs.h
 *	Real objects (実身), stage one: TSFS-on-FAT (design 11.8.1, 11.9)
 *
 *	An object is identified by a UUID, not by a name in a directory.
 *	In this stage the store is a directory of a FAT volume holding the
 *	same files the TADjs Desktop writes, so a card can be moved between
 *	the two:
 *		{uuid}.json		metadata
 *		{uuid}_N.xtad		record N, XML TAD
 *		{uuid}_N.bin		record N, bytes (TessronOS extension)
 *		{uuid}.ico		the icon
 *		{uuid}_N_M.{ext}	resource M of record N
 */

#ifndef __TS_TSFS_H__
#define __TS_TSFS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

#define TSFS_MAX_REC	64		/* records of one object */
#define TSFS_NAME_MAX	64		/* bytes of the name held in the metadata */
#define TSFS_EXT_MAX	16		/* the file name extension of a resource */

/*
 * Which store a volume uses (design 11.9), given to ts_opn_vol as a
 * flag. The path of a native volume is the name of a block device, not a
 * directory.
 */
#define TSFS_STORE_FAT	0x0000		/* a directory of a FAT volume */
#define TSFS_STORE_BLK	0x0001		/* the native block format */

/* Record types */
#define TSFS_REC_XTAD	0
#define TSFS_REC_BIN	1

/* T_ROBJ.flags: the store counts the links of its records itself */
#define TSFS_F_AUTOREF		0x0800

/* Open modes */
#define TFO_READ	0x0001
#define TFO_WRITE	0x0002
#define TFO_EXCL	0x0004

/*
 * Creation information
 */
typedef struct {
	CONST UB *json;			/* metadata, UTF-8; NULL for a default */
	SZ	jsonsz;
} T_COBJ;

/*
 * One record
 */
typedef struct {
	INT	recno;
	UINT	rectype;		/* TSFS_REC_*: the kind of file it is written out as */
	UD	size;
	UINT	rt;			/* record type 0-31 (1 is xmlTAD) */
	UINT	sub;			/* subtype */
} T_RREC;

/*
 * The items of the metadata the store keeps at hand
 */
typedef struct {
	TS_UUID	uuid;
	UB	name[TSFS_NAME_MAX];
	INT	refcnt;
	INT	nrec;
	UD	size;			/* bytes of every record together */
	UINT	flags;			/* TSFSO_F_*; zero on the FAT store */
} T_ROBJ;

/*
 * Volume state
 */
typedef struct {
	INT	nobj;
	UD	blocks;
	UD	bfree;
	UD	bsize;
	BOOL	time_valid;		/* FALSE while the clock has not been set */
} T_RVOL;

/*
 * Volume: in this stage a directory of a mounted file system.
 */
IMPORT ID  ts_opn_vol( CONST char *path, UINT flags );
IMPORT ER  ts_cls_vol( ID vol );
IMPORT ER  ts_ref_vol( ID vol, T_RVOL *pk_rvol );

/*
 * Objects
 */
IMPORT ER  ts_cre_obj( ID vol, CONST T_COBJ *pk_cobj, TS_UUID *p_uuid );
IMPORT ER  ts_del_obj( ID vol, CONST TS_UUID *uuid );
IMPORT ID  ts_opn_obj( ID vol, CONST TS_UUID *uuid, UINT omode );
IMPORT ER  ts_cls_obj( ID od );
IMPORT ER  ts_od_uuid( ID od, TS_UUID *p_uuid );	/* what an open object is */
IMPORT ER  ts_ref_obj( ID vol, CONST TS_UUID *uuid, T_ROBJ *pk_robj );
IMPORT ER  ts_lst_obj( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt );
/*
 * A hold on an object that no link of the volume makes: the root, the
 * system box, a link from another volume. On the native store the links
 * of the object's own records are counted by the store itself (design
 * 11.7); these move only the holds, and letting go of one that is not
 * held answers E_OBJ. On the FAT store they move the count itself.
 */
IMPORT ER  ts_lnk_obj( ID vol, CONST TS_UUID *uuid );	/* reference count + 1 */
IMPORT ER  ts_unl_obj( ID vol, CONST TS_UUID *uuid );	/* reference count - 1 */
IMPORT ER  ts_set_flg_obj( ID od, UINT flags, UINT mask );

/*
 * The garbage list: the objects whose reference count has fallen to zero
 * (design 11.7). They stay where they are until they are collected, and
 * one that says it may not be deleted stays on the list for good.
 * Answers E_NOSPT on a store that keeps no list of its own.
 */
IMPORT ER  ts_lst_gc( ID vol, CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt );
IMPORT ER  ts_gc_obj( ID vol, CONST TS_UUID *uuid );
IMPORT ER  ts_gc_vol( ID vol, INT *p_cnt );

/*
 * Several changes as one transaction, so that a write and the reference
 * count it changes become real together.
 */
IMPORT ER  ts_beg_trx( ID vol );
IMPORT ER  ts_end_trx( ID vol, BOOL commit );

/*
 * The link table of an object: the <link> elements of its xmlTAD
 * records, which the native store reads itself when the object is
 * closed after a write (design 11.7). Answers E_NOSPT on FAT.
 */
#define TSFS_LNK_EXTERNAL	0x0001	/* points off the volume: not counted */

typedef struct {
	TS_UUID	vobjid;			/* the link's own identity; all 0 when it has none */
	TS_UUID	target;			/* the object it points at */
	INT	recno;			/* the record it is in */
	UINT	flags;			/* TSFS_LNK_* */
} T_RLNK;

IMPORT ER  ts_lst_lnk( ID od, T_RLNK *buf, INT n, INT *p_cnt );

/*
 * The patrol of a native volume (design 11.14): every management block
 * read from the medium and checked, the bytes of the records too when
 * `data` is set. What is found damaged stops the volume writing; a
 * superblock or a group head is written again from the good copy.
 * Answers E_NOSPT on FAT. Counts come back in *p_bad and *p_checked.
 */
IMPORT ER  ts_scr_vol( ID vol, BOOL data, UD *p_checked, UD *p_bad );

/*
 * Metadata
 */
IMPORT ER  ts_get_met( ID od, UB *json, SZ size, SZ *p_asize );
IMPORT ER  ts_set_met( ID od, CONST UB *json, SZ size );
IMPORT ER  ts_set_nam( ID od, CONST UB *name );

/*
 * Records
 */
IMPORT ER  ts_rea_rec( ID od, INT recno, D off, void *buf, SZ size, SZ *p_asize );
IMPORT ER  ts_wri_rec( ID od, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize );
IMPORT ER  ts_apd_rec( ID od, UINT rectype, INT *p_recno );
IMPORT ER  ts_trn_rec( ID od, INT recno, UD newsize );
/*
 * A record replaced whole. On the native store the old bytes are kept
 * until the change is real, so after a cut in the power the record is
 * all old or all new; on the FAT store it is a write and a cut.
 */
IMPORT ER  ts_rpl_rec( ID od, INT recno, CONST void *buf, SZ size );
IMPORT ER  ts_del_rec( ID od, INT recno );	/* any place; the last only on FAT */

/*
 * The order and types of the records (native store only; E_NOSPT on
 * FAT). A record put at `pos` (-1 for the end) takes its kind of file
 * from its type: RT 1 is xmlTAD, the others binary.
 */
IMPORT ER  ts_ins_rec( ID od, INT pos, UINT rt, UINT sub, INT *p_pos );
IMPORT ER  ts_mov_rec( ID od, INT from, INT to );
IMPORT ER  ts_set_rtp( ID od, INT recno, UINT rt, UINT sub );
IMPORT ER  ts_lst_rec( ID od, T_RREC *buf, INT n, INT *p_cnt );

/*
 * Resources: bytes named by (record, number) with a file name
 * extension. The icon is reached as the resource whose record number is
 * TSFS_ICON_REC as well as by its own calls below. These answer E_NOSPT on
 * the FAT store; the calls for the object layer (knl_tsfs_res_*) reach
 * its resources there.
 */
#define TSFS_ICON_REC	(-1)

/*
 * The icon: a part of the object beside its metadata (design 11.5), read
 * and replaced whole. *p_asize is how large it is; size 0 with buf NULL
 * asks only that. Replacing it with size 0 takes it away. E_NOEXS when
 * there is none.
 */
#define TSFS_ICON_MAX	65536		/* bytes of an icon */

IMPORT ER  ts_get_ico( ID od, UB *buf, SZ size, SZ *p_asize );
IMPORT ER  ts_set_ico( ID od, CONST UB *buf, SZ size );

IMPORT ER  ts_rea_res( ID od, INT recno, INT resno, void *buf, SZ size,
		       SZ *p_asize, UB *ext );
IMPORT ER  ts_wri_res( ID od, INT recno, INT resno, CONST UB *ext,
		       CONST void *buf, SZ size );
IMPORT ER  ts_del_res( ID od, INT recno, INT resno );

/*
 * For the program loader (kernel/sysman/proc.c)
 */
IMPORT ER  knl_tsfs_rec_path( ID vol, CONST TS_UUID *uuid, INT recno, UB *out, INT max );
IMPORT INT knl_tsfs_exec_rec( ID vol, CONST TS_UUID *uuid );

/*
 * For the object layer (design 18.6): whether an object is on a volume,
 * and its metadata without opening it
 */
IMPORT ER  knl_tsfs_exists( ID vol, CONST TS_UUID *uuid );
IMPORT ER  knl_tsfs_cre_as( ID vol, CONST TS_UUID *uuid, CONST UB *json, INT len );
IMPORT ER  knl_tsfs_get_meta( ID vol, CONST TS_UUID *uuid, UB *buf, INT max, INT *p_len );
IMPORT ER  knl_tsfs_set_meta( ID vol, CONST TS_UUID *uuid, CONST UB *buf, INT len );
/*
 * Whether an object may be changed and deleted, as the native store keeps
 * it beside the metadata (TSFSO_F_EDITABLE, TSFSO_F_DELETABLE); the FAT
 * store keeps no such flags and answers E_OK
 */
IMPORT ER  knl_tsfs_set_flags( ID vol, CONST TS_UUID *uuid, UINT flags, UINT mask );
IMPORT ER  knl_tsfs_get_icon( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize );
IMPORT ER  knl_tsfs_set_icon( ID vol, CONST TS_UUID *uuid, CONST UB *buf, SZ size );
IMPORT ER  knl_tsfs_cre_icon( ID vol, CONST TS_UUID *as, CONST UB *json, INT len,
			     CONST UB *icon, SZ iconlen, TS_UUID *p_uuid );
IMPORT ER  knl_tsfs_res_rea( ID vol, CONST TS_UUID *uuid, CONST UB *name, D off,
			    void *buf, SZ size, SZ *p_asize );
IMPORT ER  knl_tsfs_res_wri( ID vol, CONST TS_UUID *uuid, CONST UB *name,
			    CONST void *buf, SZ size );
IMPORT ER  knl_tsfs_res_del( ID vol, CONST TS_UUID *uuid, CONST UB *name );
IMPORT ER  knl_tsfs_res_lst( ID vol, CONST TS_UUID *uuid, UB *buf, SZ size, INT *p_cnt );

/*
 * Whether a path names one of the files a TSFS-on-FAT store keeps its
 * objects in: a name beginning with a UUID, straight in the store's
 * directory. What is there is reached through the objects and their
 * protection, not as files (design 18.9).
 */
IMPORT BOOL knl_tsfs_guards( CONST char *path );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFS_H__ */
