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

#define WM_MAX_WIN	1024		/* windows open at once */

/* wm.c */
IMPORT ER   knl_wm_title( INT wid, char *buf, INT max );

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
