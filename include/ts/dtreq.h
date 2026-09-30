/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtreq.h
 *
 *	What a process asks of the desktop. The desktop keeps a channel
 *	named DT_REQ_NAME; a process opens it for writing and writes one
 *	T_DTREQ. The desktop does what a double click does and, when the
 *	request names a channel to answer at, writes one T_DTANS there.
 */

#ifndef __TS_DTREQ_H__
#define __TS_DTREQ_H__

#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_REQ_NAME	"tessronos.desktop"

#define DT_RQ_OPEN	1		/* the object opened as a double click opens it */
#define DT_RQ_TOOL	2		/* the accessory of this name (小物) started */

/*
 * A link carried out of the asking program's window: asked while the
 * hand is down on what the program shows as a link, the desktop carries
 * its outline until the hand lets go, and answers where. Let go over a
 * window of the desktop's that takes links (a figure, a cabinet, a
 * text), after the question in ask (when there is one) is answered
 * with yes, the answer is E_OK and names that window's object in into;
 * anywhere else, or answered no, it is E_NOEXS or E_ABORT. The program
 * then makes what the link is to point at and asks DT_RQ_PLACE with
 * the carry's seq: the link is put where the hand let go.
 */
#define DT_RQ_CARRY	3
#define DT_RQ_PLACE	4		/* a link to target put where a carry (carry) was let go */

/*
 * Several links carried as one outline: each but the last put with
 * DT_RQ_PLACE_MORE, which leaves the carry open for the next, and the
 * last with DT_RQ_PLACE. With either, grab_x and grab_y say where the
 * link's corner is from the outline's corner where it was let go (0, 0
 * the corner itself).
 */
#define DT_RQ_PLACE_MORE 7

/*
 * An application installed from a package (.tpk) and deleted
 * (システム環境設定's バージョン sheet). DT_RQ_INSTALL: target is the
 * package, an object whose record 1 is the file; recno 1 installs it
 * even when the system has the same version or a newer one. The answer
 * is E_OK with the program in into and how (DT_HOW_*), E_LIMIT when the
 * package is not newer than what the system has, E_OBJ when it is not
 * a package, E_PAR for a program of the desktop's own. DT_RQ_REMOVE:
 * target is the program object.
 */
#define DT_RQ_INSTALL	5
#define DT_RQ_REMOVE	6
#define DT_HOW_NEW	1		/* a program the system did not have */
#define DT_HOW_UPDATED	2		/* one it had, now the package's */

/* How a link is to look: its outline while carried, and the link put */
typedef struct {
	INT	w, h;			/* the frame, in screen pixels */
	INT	chsz;			/* the letters, in points; 0 the usual */
	UINT	frcol, chcol, tbcol, bgcol;	/* 0x00rrggbb; 0xFFFFFFFF not said */
	UINT	disp;			/* what the frame shows (TAD_D_* of ts/tad.h); 0 the usual */
	UINT	autoopen;		/* it opens by itself when shown */
} T_DTLOOK;

typedef struct {
	UINT	req;			/* DT_RQ_* */
	UINT	seq;			/* given back in the answer */
	TS_UUID	target;			/* DT_RQ_OPEN: the object; DT_RQ_PLACE: what the link points at */
	INT	recno;			/* and its record, -1 the first */
	TS_UUID	reply;			/* a channel to answer at; 0 none */
	UB	name[64];		/* DT_RQ_TOOL: the accessory's name, UTF-8 */
	T_DTLOOK look;			/* DT_RQ_CARRY, DT_RQ_PLACE */
	INT	grab_x, grab_y;		/* DT_RQ_CARRY: where the hand holds the outline, from its corner */
	UB	ask[2][96];		/* DT_RQ_CARRY: the question where it is let go, UTF-8; empty: none */
	UB	yes[16];		/* and the word on the button that goes ahead; empty: 「実行」 */
	UINT	carry;			/* DT_RQ_PLACE: the seq of the DT_RQ_CARRY it ends */
} T_DTREQ;

typedef struct {
	UINT	seq;
	ER	er;			/* E_OK, or why not */
	TS_UUID	proc;			/* the process it started; 0 when a window of the desktop's own opened */
	TS_UUID	into;			/* DT_RQ_CARRY: the object of the window it was let go over;
					   DT_RQ_INSTALL: the program */
	UINT	how;			/* DT_RQ_INSTALL: DT_HOW_* */
} T_DTANS;

#ifdef __cplusplus
}
#endif
#endif /* __TS_DTREQ_H__ */
