/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvsrc.h
 *	The source a program running as a process draws documents and
 *	figures from (lib/libtv/tv_src.c, design 11.11)
 *
 *	tv_src_init sets it as the source of the drawing (tv_source): what
 *	a link shows is asked of the kernel's store (ob_ref_vob), what it
 *	points at and the pictures a record names are read with the object
 *	calls, and both are kept until they are said to have changed.
 */

#ifndef __TS_TVSRC_H__
#define __TS_TVSRC_H__

#include <ts/tad.h>
#include <ts/tadview.h>

#ifdef __cplusplus
extern "C" {
#endif

IMPORT void tv_src_init( void );

/* The object being drawn: its pictures named "{realId}..." are its own (NULL none) */
IMPORT void tv_src_self( CONST TS_UUID *id );

/* The object of the window drawn in: links to other volumes are marked against it (NULL none) */
IMPORT void tv_src_host( CONST TS_UUID *id );

/*
 * The record of the object being edited, where the program keeps it
 * (*p_rec, which may be put in place of another): a link to that record
 * of it, opened, shows it as it is now, as the desktop shows a record
 * open in one of its windows, and not as it was last saved. NULL none.
 */
IMPORT void tv_src_live( CONST TS_UUID *id, INT recno, T_TAD *CONST *p_rec );

/* The looks of the links of a record asked for together, before it is drawn */
IMPORT void tv_src_prefetch( CONST T_TAD *rec );

/* What is kept of an object let go of, it having changed (NULL: everything) */
IMPORT void tv_src_forget( CONST TS_UUID *id );

/* A record of an object, read and parsed, kept by the source: not the caller's to free */
IMPORT T_TAD *tv_src_get( CONST TS_UUID *id, INT recno );

/* A record read whole, with a terminator after it; the caller frees it. NULL when it cannot be */
IMPORT UB *tv_src_read( CONST TS_UUID *id, INT recno, SZ *p_len );

/* The name of what a link points at and its paper, as the store has them; below nought when not found */
IMPORT INT tv_src_look( CONST T_VOBJ *v, UB *name, INT max, UW *p_paper );

/* What the source has asked of the system so far */
typedef struct {
	UD	asks;			/* calls of ob_ref_vob */
	UD	reads;			/* calls reading records and resources */
} T_TVSRCSTAT;

IMPORT void tv_src_stat( T_TVSRCSTAT *st );

/* How many times the letters have been drawn or measured through the kernel (lib/libtv/tv_fn.c) */
IMPORT UD tv_fn_calls( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TVSRC_H__ */
