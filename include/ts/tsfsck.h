/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsck.h
 *	The check and repair of a native TSFS volume, on the machine itself
 *	(docs/tessronos/07-filesystem.md 7.5, the accessory ディスク修復 in
 *	11-desktop.md 11.25)
 *
 *	The check walks the volume by its structure: the superblocks and the
 *	groups, the three trees, every object block with its management
 *	parts and its records' places, the reference counts against the link
 *	tables, the free maps against what the objects use, and the objects
 *	no link from a live object reaches. Each thing found is told as an
 *	event while the walk goes on.
 *
 *	The repair does only what cannot lose anything that is reachable:
 *	free maps and trees that do not pass made again (the maps from the
 *	blocks found in use, the trees from the object blocks), link tables
 *	made again from the records, counts and the garbage list made to agree
 *	with the link tables, blocks nothing uses given back, blocks in use
 *	marked as such, the free counts and the copies of the superblock
 *	written again, and the objects only reached from one another
 *	(隠れ屑実身) gathered into a new cabinet TRASH, itself a 屑実身. A
 *	volume with damage the repair does not put right (an object block or
 *	part that does not pass its check, a block used twice, a tree and a
 *	map both bad) is not written at all.
 */

#ifndef __TS_TSFSCK_H__
#define __TS_TSFSCK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/tsfsblk.h>

/* How: 処理方式 */
#define TSFSCK_QUICK	0x0001		/* 簡易: no places of the records, no free maps */
#define TSFSCK_FULL	0x0002		/* 完全: the free blocks and the records' blocks read too */
#define TSFSCK_REPAIR	0x0010		/* 修復; without it the volume is only looked at */
#define TSFSCK_ALONE	0x0100		/* nothing else has the volume open (set by ob_chk_run) */

/* What is being looked at: the phase an event belongs to */
#define TSFSCK_PH_SUPER		1	/* the superblocks and the groups */
#define TSFSCK_PH_INDEX		2	/* リンクインデックス: the three trees */
#define TSFSCK_PH_HEAD		3	/* ファイルヘッダー: the object blocks */
#define TSFSCK_PH_TABLE		4	/* 通常インデックスと接続インデックス: the tables */
#define TSFSCK_PH_DATA		5	/* フラグメント・区切り・レコードサイズ */
#define TSFSCK_PH_INFO		6	/* ファイルヘッダー情報: counts, the garbage list */
#define TSFSCK_PH_SPACE		7	/* the free maps against what is used */
#define TSFSCK_PH_FREE		8	/* the free blocks read (完全) */
#define TSFSCK_PH_HIDDEN	9	/* objects no live object reaches */
#define TSFSCK_PH_REPAIR	10	/* putting right */
#define TSFSCK_PH_VERIFY	11	/* the check again after the repair */
#define TSFSCK_NPHASE		12

/* What was found. K_PHASE says only that a phase begins. */
#define TSFSCK_K_PHASE		0
#define TSFSCK_K_SUPER		1	/* a superblock copy bad (b 0), an error recorded (b 1, a its kind) */
#define TSFSCK_K_GROUP		2	/* a group head on the medium bad (b 0), a map bad (b 1) */
#define TSFSCK_K_FREECNT	3	/* groups whose free count is not their map's (a groups) */
#define TSFSCK_K_NODE		4	/* a node of a tree bad or out of order (a the tree) */
#define TSFSCK_K_INDEX		5	/* an entry of a tree points at no object block */
#define TSFSCK_K_HEAD		6	/* an object block bad, or not the object's */
#define TSFSCK_K_PART		7	/* a management part (metadata, icon, a table) cannot be read */
#define TSFSCK_K_TABLE		8	/* a table does not agree with itself or the object block */
#define TSFSCK_K_DATA		9	/* a record's place does not hold its bytes */
#define TSFSCK_K_META		10	/* the metadata is not a JSON object, or its records do not agree */
#define TSFSCK_K_REFCNT		11	/* a count (a) not what the links and holds make (b) */
#define TSFSCK_K_GCLIST		12	/* the garbage list and the objects disagree */
#define TSFSCK_K_ORPHAN		13	/* the orphan tree and the objects disagree */
#define TSFSCK_K_LINK		14	/* a link counted to an object that is not on the volume */
#define TSFSCK_K_DOUBLE		15	/* a block two things use (blk) */
#define TSFSCK_K_LOST		16	/* blocks in use but free in the map (blk, a of them) */
#define TSFSCK_K_LEAK		17	/* blocks taken in the map that nothing uses (blk, a of them) */
#define TSFSCK_K_UNREAD		18	/* a block the device cannot read (blk) */
#define TSFSCK_K_HIDDEN		19	/* 隠れ屑実身: reached only from other such objects */
#define TSFSCK_K_RELINK		20	/* a link table marked to be made again, with nothing to do it */
#define TSFSCK_K_XREF		21	/* the record of links from other volumes wrong: b why
					   (TSFSCK_X_*), a the count recorded, blk the count found */
#define TSFSCK_NKIND		22

/* Why the record of links from other volumes was found wrong (T_TSFSCKEV.b of K_XREF) */
#define TSFSCK_X_TARGET		0	/* the object linked to is not on the volume */
#define TSFSCK_X_SOURCE		1	/* the object linking is not on its volume, attached */
#define TSFSCK_X_COUNT		2	/* it links a number of times the record does not say */
#define TSFSCK_X_MISSING	3	/* a link no record says */
#define TSFSCK_X_OBJECT		4	/* the object of the record is not there */

/*
 * Why a block, a part, a table or a place was found wrong (T_TSFSCKEV.b
 * of K_NODE, K_INDEX, K_HEAD, K_PART, K_TABLE, K_DATA, K_META)
 */
#define TSFSCK_W_DAMAGED	0	/* it does not pass the check of its head */
#define TSFSCK_W_OUTSIDE	1	/* it points outside the blocks objects may use */
#define TSFSCK_W_WRONG		2	/* it is another's, or says another tree or level */
#define TSFSCK_W_SIZE		3	/* its bytes and the room it has disagree */
#define TSFSCK_W_FORM		4	/* a form, a type or a kind that is not known */
#define TSFSCK_W_COUNT		5	/* its entries are not as many as the object block says */
#define TSFSCK_W_ORDER		6	/* out of order, or one named twice */
#define TSFSCK_W_SHARED		7	/* two parts share units of the inline area */

typedef struct {
	INT	phase;			/* TSFSCK_PH_* */
	INT	kind;			/* TSFSCK_K_* */
	BOOL	fixed;			/* told by the repair: it was put right */
	BOOL	has_uuid;
	TS_UUID	uuid;			/* the object it is about */
	BOOL	has_other;
	TS_UUID	other;			/* the object a link points at */
	UD	blk;			/* the block it is about, 0 for none */
	D	a, b;			/* numbers the kind says the meaning of */
} T_TSFSCKEV;

/*
 * Events come in the order of the walk: a K_PHASE as each phase begins,
 * then what it finds. The repair tells each thing it puts right (fixed
 * set, phase TSFSCK_PH_REPAIR); the look after it tells, under
 * TSFSCK_PH_VERIFY, one event for each kind still found, with `a` how
 * many.
 */
typedef void (*FP_TSFSCK)( void *ctx, CONST T_TSFSCKEV *ev );

typedef struct {
	UINT	mode;
	INT	found[TSFSCK_NKIND];	/* the first look: blocks for LOST and LEAK, else each thing */
	INT	fixed[TSFSCK_NKIND];	/* what the repair put right */
	INT	left[TSFSCK_NKIND];	/* what the last look still found */
	INT	problems;		/* found, every kind together */
	INT	unfixable;		/* of them, what the repair does not put right */
	INT	remaining;		/* left, every kind together */
	BOOL	written;		/* the repair changed the volume */
	BOOL	refused;		/* the repair was not tried: damage it does not put right */
	BOOL	detached;		/* a volume in use, taken off for the repair and put back */
	BOOL	inuse;			/* a volume in use that could not be taken off: an object of it is open */
	BOOL	has_trash;
	TS_UUID	trash;			/* the cabinet the 隠れ屑実身 went into */
	UD	objects, records, links;
	UD	blocks, free;		/* of the volume */
	UD	checked;		/* blocks read */
} T_TSFSCK;

/*
 * A volume of the block layer (ts_obj_mount) checked, and repaired when
 * the mode says so. The caller keeps every writer away meanwhile; ts_chk_vol
 * does that for a volume opened with ts_opn_vol.
 */
IMPORT ER ts_obj_check( ID bvol, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx );
IMPORT ER ts_chk_vol( ID vol, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx );

/*
 * When a TRASH has gone (the store calls this as it collects one): of the
 * objects it linked to, those no live object reaches now, and those only
 * they reach -- groups linking only to one another among them -- go too.
 * Answers how many went.
 */
IMPORT INT ts_obj_drop_cycles( ID bvol, CONST TS_UUID *from, INT nfrom );

/*
 * The native volumes the machine has, attached or not, for the file
 * manager and the accessory: by device name ("vblka0"). ob_chk_run checks
 * one of them: an attached one in place, the others opened for the
 * check alone. An attached volume that is to be repaired is taken off
 * first and put back afterwards, so that it is repaired alone as well;
 * while an object of it is open it stays, and is repaired in place when
 * it can be (T_TSFSCK.inuse). The volume the system runs on is not repaired while it
 * runs (E_BUSY): ob_chk_request asks for it to be repaired at the next
 * start, before anything uses it, and ob_chk_boot_result tells what that
 * repair found and did.
 */
#define TSFSCK_DEVNM_MAX	16

typedef struct {
	UB	devnm[TSFSCK_DEVNM_MAX];
	UB	label[TSFSBLK_LABEL_MAX];
	UD	blocks;
	BOOL	attached;		/* its objects are in use */
	BOOL	system;			/* the volume the system runs on */
} T_TSFSCKDEV;

/* Every volume there is: up to max of them into buf, and how many there are (ask again with more room when more) */
IMPORT INT ob_chk_list( T_TSFSCKDEV *buf, INT max );
IMPORT ER  ob_chk_run( CONST char *devnm, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx );
IMPORT ER  ob_chk_request( CONST char *devnm, UINT mode );
IMPORT INT ob_chk_boot_result( UB *devnm, INT nmax, T_TSFSCK *res, T_TSFSCKEV *ev, INT max );

/* Where the request waits for the next start, on the boot volume */
#define TSFSCK_REQUEST	"/boot/DISKFIX.REQ"

/* At start, before the system volume is attached: the request carried out */
IMPORT ER  knl_ob_chk_boot( CONST char *sysvol );

/* Whether a device is being checked alone, and must not be attached meanwhile */
IMPORT BOOL knl_ob_chk_busy( CONST char *devnm );

/*
 * The record of links from other volumes of a volume (TSFS's number)
 * checked against the volumes attached (design 8.3, 7.5): an entry whose
 * object linked to is not there, or whose object linking is not on its
 * volume, attached; a count that is not what that object's link table
 * says; a link of an attached volume no entry says. A volume not
 * attached cannot be asked, and what it is said to link stays as it is.
 * With 'fix' the record is made to say what was found. Events as
 * ts_obj_check tells them (kind TSFSCK_K_XREF); found and fixed counted
 * into res.
 */
IMPORT ER   knl_obxref_check( ID vol, BOOL fix, T_TSFSCK *res, FP_TSFSCK fn, void *ctx );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSCK_H__ */
