/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wmobj.h
 *	Between the window layer and the window manager of the object
 *	layer (design 18.13): what the windows and panels tell it, and
 *	what it asks of them
 */

#ifndef __WMOBJ_H__
#define __WMOBJ_H__

#include <ts/wm.h>
#include <ts/part.h>

/*
 * Window numbers: there is no bound on how many windows are open at
 * once; a window's number is its place in a table that grows (wm.c), and
 * goes into its handle in 28 bits (wmobj.c).
 */
#define WM_WIN_LAST	0x0FFFFFFF

/* wm.c */
IMPORT ER   knl_wm_title( INT wid, char *buf, INT max );
/* The highest window number there is room for now: the windows are 1 to this */
IMPORT INT  knl_wm_nwin( void );

/* part.c: a panel's parts, copied out */
IMPORT INT  knl_pn_count( INT pid, BOOL *p_menu );
IMPORT ER   knl_pn_part( INT pid, INT i, T_WMPART *out );
IMPORT INT  knl_pn_add( INT pid, CONST T_WMPART *pt );
IMPORT ER   knl_pn_del( INT pid, INT i );
IMPORT ER   knl_pn_set_rect( INT pid, INT i, CONST T_DPRECT *r );

/*
 * wmobj.c. Each is called with no lock of the window layer held: a
 * window that opens or closes, a panel that opens or closes, something
 * that happened in a window or to one part of a panel.
 */
IMPORT void knl_wmobj_open( INT wid );
IMPORT void knl_wmobj_close( INT wid );
IMPORT void knl_wmobj_moved( INT wid, BOOL resized );
IMPORT void knl_wmobj_event( CONST T_WMEV *ev );
/*
 * A key from the keyboard as it came, before it is addressed to the
 * window with the keys: told to every window that never has the keys
 * (WM_ATTR_NOFOCUS) and is shown, to a request with OB_N_KEYSEEN
 */
IMPORT void knl_wmobj_key_seen( CONST T_WMEV *ev );
IMPORT void knl_wmobj_panel( INT pid );
IMPORT void knl_wmobj_panel_close( INT pid );
IMPORT void knl_wmobj_part( INT pid, INT i, UINT event, CONST T_WMEV *ev );

/* The process that made a window: 0 for one of the system's own */
IMPORT ID   knl_wmobj_maker( INT wid );

/*
 * A part about to be drawn, in the box r of the drawing environment
 * gid: TRUE when it has looks of its own and they were drawn, so the
 * standard drawing is left out. 'on' is whether it is pressed or set.
 */
IMPORT BOOL knl_wmobj_part_look( INT pid, INT i, INT gid, CONST T_DPRECT *r, BOOL on );

#endif /* __WMOBJ_H__ */
