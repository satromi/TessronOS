/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tray.h
 *	The tray: what is taken from one place until it is put into another
 *	(design 18.16)
 *
 *	What is put in the tray goes in as a set: a name, the time, and one
 *	or more tray records -- an xmlTAD fragment, a picture, a record of a
 *	real object as it is. Sets are kept in levels, the newest at level
 *	1; the oldest falls off below level TR_LEVELS. The level in hand is
 *	the one copying and moving from the tray take; it is level 1 after
 *	something is put in, and any level can be chosen instead. Level 0
 *	is a place of its own for a set a program puts down and picks up
 *	again without it going into the levels.
 *
 *	The tray is a real object (SYSDEF_TRAY, on the volatile volume):
 *	record 0 lists the levels in words, and each set is one record of
 *	its own. What goes in is copied in, what comes out is copied out.
 */

#ifndef __TS_TRAY_H__
#define __TS_TRAY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

#define TR_LEVELS	10		/* sets kept */
#define TR_NAME_MAX	64		/* bytes of a set's name, with its terminator */
#define TR_REC_MAX	32		/* tray records in one set */

/* Which set: a level 1..TR_LEVELS, the place of its own, or the one in hand */
#define TR_PUT		0
#define TR_HAND		( -1 )

/* What a tray record holds */
#define TR_TAD		1		/* an xmlTAD fragment: text, figures, <link>s */
#define TR_PNG		2		/* a picture, PNG */
#define TR_REC		3		/* a record of a real object, as it is */

/* A tray record going in */
typedef struct {
	UINT	kind;			/* TR_* */
	UINT	rt, sub;		/* TR_REC: the record's type and subtype */
	CONST void *data;
	SZ	size;
} T_TRREC;

/* A tray record in a set */
typedef struct {
	UINT	kind;
	UINT	rt, sub;
	SZ	size;
} T_TRINF;

/* A set */
typedef struct {
	INT	nrec;			/* 0: nothing there */
	D	time;			/* when it went in, TS_TIME */
	UINT	kinds;			/* ( 1 << TR_* ) of each kind it holds */
	UB	name[TR_NAME_MAX];
} T_TRSET;

typedef struct {
	INT	nlevel;			/* levels that hold a set */
	INT	hand;			/* the level in hand, 0 when the tray is empty */
	T_TRSET	set[TR_LEVELS + 1];	/* [TR_PUT], then levels 1..TR_LEVELS */
} T_TRSTS;

/* A set put in at level 1; the others move down one. It becomes the one in hand */
IMPORT ER  tr_psh_dat( CONST T_TRREC *recs, INT n, CONST UB *name );

/* Records added to the set in hand, so that several takings make one set */
IMPORT ER  tr_add_dat( CONST T_TRREC *recs, INT n );

/* A set put down in the place of its own; n of 0 empties it */
IMPORT ER  tr_set_dat( CONST T_TRREC *recs, INT n );

IMPORT ER  tr_get_sts( T_TRSTS *sts );

/* One tray record of a set: what it is, and its bytes */
IMPORT ER  tr_ref_rec( INT level, INT rec, T_TRINF *inf );
IMPORT ER  tr_rea_rec( INT level, INT rec, void *buf, SZ size, SZ *p_asize );

IMPORT ER  tr_sel_dat( INT level );		/* the level in hand */
IMPORT ER  tr_mov_dat( INT level, INT to );	/* a set moved to another level */
IMPORT ER  tr_del_dat( INT level );		/* a set taken out */
IMPORT ER  tr_clr_tra( void );			/* everything taken out */

#ifdef __cplusplus
}
#endif

#endif /* __TS_TRAY_H__ */
