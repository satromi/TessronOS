/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsfmt.h
 *	Where the fields of an object block and of a node of a B+tree are
 *	(design 11.6.5, 11.6.6), for the code that writes them
 *	(tsfsobj.c, tsfsbtree.c) and the code that checks a whole volume
 *	(tsfsck.c). Everything is little endian.
 *
 *	The object block, 4096 bytes:
 *
 *	   0	the common head: kind "TFOB", its own block number, the
 *		volume, the object (low 64 bits of its UUID) as owner, the
 *		transaction, the CRC
 *	  64	UUID of the object
 *	  80	flags (TSFSO_F_*)
 *	  84	reference count
 *	  88	entries of the placement table, of the resource table, of
 *		the link table (32 bits each), and the next rid
 *	 104	made, updated, read	64 bit ns since 1985-01-01 UTC
 *	 128	where the metadata text is (a place, 32 bytes)
 *	 160	where the placement table is
 *	 192	where the resource table is
 *	 224	where the link table is
 *	 256	which units of the inline area are taken (56 bits)
 *	 264	holds: the part of the count no link of the volume makes
 *	 288	where the icon is (a place, 32 bytes); all 0 when it has none
 *	 320	version 5: the spare place (a place, 32 bytes), runs the
 *		object owns that no record and no management part does;
 *		all 0 when it is empty (TSFSO_F_SPARE when it is not)
 *	 512	the inline area, 56 units of 64 bytes
 *
 *	A place (32 bytes) is one of
 *	   0	form: none, inline, a run, an extent tree, a tree of two
 *		levels, a run tree (version 5), the runs to give back
 *		(version 5, the spare place only)
 *	   8	bytes
 *	  16	inline: offset (16 bits) and units (16 bits)
 *		a run: first block; its length at 24
 *		an extent tree: the block of its leaf; extents at 24
 *		a run tree: the block of its root node; runs at 24
 *		the runs to give back: the rid of the record whose runs
 *		past `bytes` go
 *
 *	A node of a run tree (version 5), one block with the common head
 *	(kind "TFTB"):
 *	  64	entries
 *	  68	level, 0 at the leaves
 *	 128	up to 165 entries of 24 bytes: at a leaf the logical
 *		block, the block and the blocks of a run; at an index the
 *		first logical block of the child, the child's block and the
 *		blocks below it
 *
 *	The metadata text, the icon and the tables are management parts:
 *	inline, or a run of blocks each with the common head (kind "TFTB")
 *	that goes through the journal. A volume written before the icon had
 *	a place of its own kept it as a resource of the owner all ones; it
 *	is read from there until it is next written. The bytes of a record
 *	or a resource are data: inline while they are small, else a run of
 *	plain blocks, else an extent tree, whose one leaf (kind "TFTB")
 *	lists up to 165 runs.
 *
 *	A placement table entry, 48 bytes, in the order of the rids:
 *	   0	rid
 *	   4	kind (TSFS_REC_XTAD or TSFS_REC_BIN)
 *	   8	bytes
 *	  16	where the bytes are (a place)
 *
 *	A resource table entry, 64 bytes:
 *	   0	rid of the record it belongs to; all ones for the icon; the
 *		top bit and a place for one written before its record was
 *	   4	its number within that record
 *	   8	file name extension, 16 bytes padded with NUL; on a version
 *		5 volume, when the first byte is 0xFF, the offset (+4) and
 *		the length (+8, 32 bits each) of the extension in the
 *		string area behind the entries of the table
 *	  24	bytes
 *	  32	where the bytes are (a place)
 *
 *	A link table entry, 48 bytes, in the order of the whole entry:
 *	   0	the link's own identity (vobjid), all 0 when it has none
 *	  16	the object it points at
 *	  32	rid of the record it is in
 *	  36	flags: 1 it points off the volume and is not counted
 *	  40	reserved
 *
 *	A node of a B+tree, one block:
 *
 *	   0	the common head: kind "TFBT", its own block number, the
 *		volume, the tree as owner, the transaction, the CRC
 *	  64	type (leaf or node)
 *	  65	level (0 at the leaves)
 *	  66	number of keys
 *	  72	next leaf, 0 at the last one
 *	  80	leftmost child (internal nodes)
 *	  88	the tree (TSFSBT_OBJ, TSFSBT_GC, TSFSBT_ORPHAN)
 *	  96	the keys and their values, 24 bytes each
 */

#ifndef __TSFSFMT_H__
#define __TSFSFMT_H__

#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsobj.h>

/* the object block */
#define OB_UUID		64
#define OB_FLAGS	80
#define OB_REFCNT	84
#define OB_NPLACE	88
#define OB_NRES		92
#define OB_NLINK	96
#define OB_NEXTRID	100
#define OB_MADE		104
#define OB_UPDATED	112
#define OB_READ		120
#define OB_META		128
#define OB_PLACE	160
#define OB_RES		192
#define OB_LINK		224
#define OB_INLMAP	256
#define OB_PINS		264
#define OB_ICON		288
#define OB_SPARE	320		/* version 5 */
#define OB_INLINE	TSFSO_HEAD_SIZE
#define INL_UNITS	( TSFSO_INLINE_BYTES / TSFSO_INLINE_UNIT )

/* a place */
#define PL_SIZE		32
#define PL_FORM		0
#define PL_BYTES	8
#define PL_A		16
#define PL_B		24
#define F_NONE		0
#define F_INLINE	1
#define F_RUN		2
#define F_TREE		3
#define F_TREE2		4		/* an index of leaves (TSFSBLK_INCOMPAT_XT2) */
#define F_TREEN		5		/* a run tree of any depth (version 5) */
#define F_TRIM		6		/* the spare place: the runs of record PL_A past PL_BYTES (version 5) */

/* a placement table entry */
#define PE_SIZE		48
#define PE_RID		0
#define PE_KIND		4
#define PE_BYTES	8
#define PE_PLACE	16

/* a resource table entry */
#define RS_SIZE		64
#define RS_OWNER	0
#define RS_RESNO	4
#define RS_EXT		8
#define RS_BYTES	24
#define RS_PLACE	32
#define RID_ICON	0xFFFFFFFFU
#define RID_PLACE	0x80000000U	/* owner by place: its record was not there yet */
#define RSX_MARK	0xFF		/* RS_EXT's first byte: the extension is in the string area */
#define RSX_OFF		4		/* from RS_EXT: its offset in the string area */
#define RSX_LEN		8		/* from RS_EXT: its length */

/* a link table entry */
#define LK_SIZE		48
#define LK_VOBJ		0
#define LK_TARGET	16
#define LK_RID		32
#define LK_FLAGS	36
#define LK_KEY		40		/* the bytes entries are ordered by */
#define LK_F_EXTERNAL	1

/* a block of a management part: the common head, then the bytes */
#define MB_PAY		( TSFSBLK_BLOCK_SIZE - TSFSBLK_HDR_SIZE )

/* the leaf of an extent tree */
#define XL_N		64
#define XL_ENT		128
#define XL_ESZ		24

/* a node of a run tree (version 5): entries at XL_N, the entries from XL_ENT */
#define XN_LEVEL	68

/* a node of a B+tree */
#define OFF_TYPE	64
#define OFF_LEVEL	65
#define OFF_NKEYS	66
#define OFF_NEXT	72
#define OFF_CHILD0	80
#define OFF_TREE	88

#define ENT(buf, i)	((buf) + TSFSBT_HDR_SIZE + (i) * TSFSBT_ENT_SIZE)
#define ENT_KEY(buf, i)	(ENT(buf, i))
#define ENT_VAL(buf, i)	(ENT(buf, i) + 16)

#endif /* __TSFSFMT_H__ */
