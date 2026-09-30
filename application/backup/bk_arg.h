/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_arg.h
 *	バックアップ: the start-up argument that asks for work without the
 *	window (design 17.18)
 *
 *	The accessory is started with nothing (the window opens), with the
 *	UUID of an object (the window opens on it: an archive object to
 *	restore from, anything else to save), or with this: save or restore
 *	at once, say how it went on the console and end, 0 when it went well.
 */

#ifndef __BK_ARG_H__
#define __BK_ARG_H__

#define BK_ARG_MAGIC	0x504B4B42	/* "BKKP" */
#define BK_OP_SAVE	1
#define BK_OP_RESTORE	2
#define BK_ARG_ROOTS	8

typedef struct {
	UINT	magic;
	UINT	op;			/* BK_OP_* */
	TS_UUID	obj;			/* save: the root. restore: the cabinet the root is
					   linked into, 0 for the first one */
	UB	path[256];		/* save: the directory the volumes go to, "" for
					   objects. restore: the file of the first volume */
	UD	capacity;		/* save: bytes a volume holds, 0 as much as there is */
	UB	base[64];		/* save: the volumes' name after "N・", "" the root's */
	TS_UUID	into;			/* save to objects: the cabinet the volumes are linked
					   into and made on the volume of, 0 the first */
	INT	nroot;			/* save: more roots after obj, in root[] */
	TS_UUID	root[BK_ARG_ROOTS];
	TS_UUID	parent[BK_ARG_ROOTS + 1];	/* where obj, then each of root[], is linked
					   from, for a restore to put it back; 0 not known */
	INT	nexcl;			/* save: objects left out */
	TS_UUID	excl[BK_ARG_ROOTS];
} T_BKARG;

#endif /* __BK_ARG_H__ */
