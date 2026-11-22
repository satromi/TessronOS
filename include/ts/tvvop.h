/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvvop.h
 *	The 仮身操作, 実身操作 and 実行 of a link in a program's window,
 *	the same in every program (lib/libtv/tv_vop.c, design 11.2.6)
 *
 *	The desktop does what is the system's: its panels, the store's
 *	names and copies, carrying a link out. What is the program's -- the
 *	link written back as the desktop answers, where a link to a new
 *	copy goes, and that the record is to be saved -- the program does
 *	through the callbacks of T_TVVOP. The items themselves are the
 *	system's menu of links (SYSDEF_MENU_OBJECT), which a program's menu
 *	takes in: tv_vop_menu sets how they stand for the link under the
 *	pointer and fills 実行's list, tv_vop_command does the one chosen.
 *	A program that holds its links as <link> elements of xmlTAD makes a
 *	T_TVLINK of one with ct_vop_link (application/calendar/cal_tad.h),
 *	one that holds T_VOBJ with tv_vop_vobj.
 *
 *	A request goes to the desktop through T_TVASK: how the program
 *	asks it (dq_tv_asker of application/dtask.h for one that keeps a
 *	DTASK, ta_tv_asker of ts/tvapp.h for one built on lib/libtv/tv_app.c).
 */

#ifndef __TS_TVVOP_H__
#define __TS_TVVOP_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>
#include <ts/dp.h>
#include <ts/dtreq.h>
#include <ts/tad.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- asking the desktop */

/*
 * ask sends rq with the parts added since the last one and waits for its
 * answer (the answer's er, or why there is none); part adds a part to the
 * next request's tail (len < 0: up to its 0), and a part that cannot be
 * kept makes that request fail with E_NOMEM; ans finds a part of the last
 * answer's tail, NULL when it has none.
 */
typedef struct {
	ER		(*ask)( void *arg, T_DTREQ *rq, T_DTANS *an, INT wait_ms );
	ER		(*part)( void *arg, UINT kind, CONST UB *s, SZ len );
	CONST UB	*(*ans)( void *arg, UINT kind, SZ *p_len );
	void		*arg;
} T_TVASK;

/* ---------------------------------------------------------------- a link */

/*
 * One link as a request about it says it. Its words are borrowed: the
 * program keeps them as long as the T_TVLINK is used, and none of them
 * is used after a callback that writes the link back.
 */
typedef struct {
	TS_UUID		target;		/* the object it points at */
	TS_UUID		vobjid;		/* the link's own identity; all zero none */
	T_DPRECT	box;		/* where it is drawn in the window */
	INT		was_left, was_top;	/* its box in the record (dt_req_was); */
	INT		was_right, was_bottom;	/* right not past left: none */
	T_DTLOOK	look;		/* chsz, colours, disp, autoopen; w and h are the box's */
	BOOL		hidden;
	INT		wordwrap;	/* TAD_WRAP_* */
	CONST UB	*rel;		/* its relationship, its applist and its viewMode, */
	SZ		rel_len;	/* each as long as it is; NULL or 0 none */
	CONST UB	*applist;
	SZ		applist_len;
	CONST UB	*view;
	SZ		view_len;
	void		*app;		/* the program's: which of its links this is */
} T_TVLINK;

/* A link of a T_VOBJ, drawn in box (NULL: the box the T_VOBJ holds); its words are v's */
IMPORT void tv_vop_vobj( T_TVLINK *l, CONST T_VOBJ *v, CONST T_DPRECT *box );

/* ---------------------------------------------------------------- the program's part */

/* What tv_vop_command writes back into a link (T_TVVOP put) */
#define TV_VOP_W_LOOK		1	/* 仮身属性変更: an->look and DT_F_HIDDEN of an->flags */
#define TV_VOP_W_REL		2	/* 続柄設定: text, len (0: none) */
#define TV_VOP_W_APPLIST	3	/* 起動アプリの固定: the applist, text, len */

typedef struct {
	CONST T_DTANS	*an;		/* the desktop's answer */
	CONST UB	*text;		/* the words, ending in 0 */
	SZ		len;
} T_TVPUT;

/* Why a link to a new object is to be put (T_TVVOP put_copy) */
#define TV_VOP_C_DUP		1	/* 実身複製 */
#define TV_VOP_C_NEWVER		2	/* 新版作成 */

/* Flags of T_TVVOP */
#define TV_VOP_F_STATE		0x0001	/* the menu asks the desktop how the link stands (DT_RQ_LINK_STATE) */
#define TV_VOP_F_DISK		0x0002	/* and sets ディスク操作's 切り離し and 拡張 as it answers */
/*
 * The size the desktop is told of a link is the one it has in the record
 * (was_*), not that of the box it is drawn in: for a window that draws
 * its record scaled
 */
#define TV_VOP_F_REC_SIZE	0x0004
/*
 * Links carried out are put there as copies and stay in the window:
 * for a list the program makes, or a copy asked for before the carrying
 */
#define TV_VOP_F_COPY		0x0008

typedef struct {
	T_TVASK		ask;
	UINT		flags;		/* TV_VOP_F_* */
	/* on the console, for the tests; on the desktop's message line */
	void		(*note)( CONST char *s );
	void		(*say)( CONST char *s );
	/*
	 * The object whose record holds the links and the channel where the
	 * desktop tells what a window opened from one did to it (DT_F_LINK):
	 * FALSE when there is none, and the links are not named.
	 */
	BOOL		(*named)( TS_UUID *p_parent, TS_UUID *p_chan );
	/* the link written back: FALSE when it could not be (no memory), the link as it was */
	BOOL		(*put)( T_TVLINK *l, UINT what, CONST T_TVPUT *p );
	/* a link to copy, the new object made from l's, put where the program puts one */
	void		(*put_copy)( T_TVLINK *l, CONST TS_UUID *copy, UINT why );
	/* the record changed: to be saved */
	void		(*changed)( void );
	/* optional, NULL none: what the program does when a link is opened (TRUE: done) */
	BOOL		(*open)( T_TVLINK *l );
	/* optional: what the program knows of an object forgotten (its name, after 名前変更) */
	void		(*forget)( CONST TS_UUID *target );
} T_TVVOP;

/* ---------------------------------------------------------------- the menu */

/*
 * The items 仮身操作, 実身操作 and 実行 of menu mid set for link l (NULL:
 * none under the pointer, all three grey), 実行's list filled from the
 * desktop (DT_RQ_APPS), and grey when the link reaches no object or the
 * list is empty. The list is kept for tv_vop_command until the next
 * tv_vop_menu. Answers how many programs 実行 offers, 0 when it is grey.
 */
IMPORT INT  tv_vop_menu( CONST T_TVVOP *h, ID mid, T_TVLINK *l );

/*
 * The item chosen of code (and index in 実行's list) done for link l:
 * TRUE when it was one of these (vobj.*, real.*, exec.app), FALSE when it
 * is the program's own.
 */
IMPORT BOOL tv_vop_command( CONST T_TVVOP *h, T_TVLINK *l, CONST char *code, INT index );

/*
 * An item of ディスク操作 (code "disk.*") chosen in a window: the program
 * ディスク操作 is started with it, on the object target when the menu was
 * opened on a link (NULL: on none). Answers FALSE for another code.
 */
IMPORT BOOL tv_vop_disk( CONST T_TVVOP *h, CONST char *code, CONST TS_UUID *target );

/* 開く: as a double press opens it (DT_RQ_OPEN, the link named when it can be) */
IMPORT ER   tv_vop_open( CONST T_TVVOP *h, T_TVLINK *l );

/* Whether link l keeps to the program it opens with (起動アプリの固定) */
IMPORT BOOL tv_vop_kept( CONST T_TVLINK *l );

/*
 * A request about link l for the desktop, its look said, its
 * relationship and applist in the tail of the next request
 */
IMPORT void tv_vop_req( CONST T_TVVOP *h, CONST T_TVLINK *l, UINT req, T_DTREQ *rq );

/* ---------------------------------------------------------------- carried out */

/* What became of links carried out (tv_vop_carry_out) */
typedef struct {
	INT	done;			/* how many were put where they were let go */
	BOOL	copy;			/* a copy was put (the second button on the way) */
	BOOL	twice;			/* a link to a copy of each object (実身複製ドラッグ) */
	BOOL	given;			/* let go over another program's window */
	BOOL	moved;			/* done, and neither: the program takes them away */
	/*
	 * The desktop's answer to the carrying itself, also when it did not
	 * follow the pointer: let go back over the window it came from, it is
	 * E_NOEXS with DT_F_BACK and where it was let go (dtreq.h), for a
	 * program that moves what it carried within its window then
	 */
	T_DTANS	an;
} T_TVCARRY;

/*
 * The n links of l carried out of the window, as one outline of the box
 * out (NULL: l's box, for one), held at x0, y0 in the window: the
 * desktop follows the pointer (DT_RQ_CARRY) and puts each where it is let
 * go (DT_RQ_PLACE, DT_RQ_PLACE_MORE for all but the last). twice: pressed
 * twice before it was carried, a link to a copy of each object is put.
 * placed (NULL: not wanted) says of each whether it was put. E_OK when
 * the desktop followed the pointer, its error when it did not.
 */
IMPORT ER   tv_vop_carry_out( CONST T_TVVOP *h, T_TVLINK *l, INT n, CONST T_DPRECT *out,
			      INT x0, INT y0, BOOL twice, BOOL *placed, T_TVCARRY *res );

/*
 * One link to target, named name with the applist appl (NULL: none),
 * carried out of the window while the button is down: the desktop
 * follows the pointer with an outline of out->w by out->h held at grab_x,
 * grab_y from its corner (DT_RQ_CARRY), and where it is let go over a
 * window that takes links the link is put looking as put says
 * (DT_RQ_PLACE), at the outline's corner. It may also go to another
 * program's window (DT_F_TO_PROG). The answer of the last request asked,
 * and that answer in *an: E_NOEXS or E_OBJ when it was let go where
 * nothing takes it, back over the window, or not carried at all (a
 * click).
 */
IMPORT ER   tv_vop_carry( CONST T_TVASK *a, CONST TS_UUID *target, CONST char *name, CONST char *appl,
			  CONST T_DTLOOK *out, INT grab_x, INT grab_y, CONST T_DTLOOK *put, T_DTANS *an );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVVOP_H__ */
