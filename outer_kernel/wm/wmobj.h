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
/* Something is being carried, its outlines on the screen (wm_set_drag) */
IMPORT BOOL knl_wm_carrying( void );

/* part.c: a panel's parts, copied out */
IMPORT INT  knl_pn_count( INT pid, BOOL *p_menu );
IMPORT ER   knl_pn_part( INT pid, INT i, T_WMPART *out );
IMPORT INT  knl_pn_add( INT pid, CONST T_WMPART *pt );
IMPORT ER   knl_pn_del( INT pid, INT i );
IMPORT ER   knl_pn_set_rect( INT pid, INT i, CONST T_DPRECT *r );
/* A panel of tags' sheet moved within the panel, and a panel made another size where it stands */
IMPORT ER   knl_pn_set_inner( INT pid, INT i, CONST T_DPRECT *r );
IMPORT ER   knl_pn_set_size( INT pid, INT w, INT h );

/*
 * wmobj.c. Each is called with no lock of the window layer held: a
 * window that opens or closes, a panel that opens or closes, something
 * that happened in a window or to one part of a panel.
 */
IMPORT void knl_wmobj_open( INT wid );
IMPORT void knl_wmobj_close( INT wid );
IMPORT void knl_wmobj_gsurf_shown( INT wid, INT idx, UW serial );	/* OB_E_CHANGE on OB_WR_GSURF */
IMPORT void knl_wmobj_moved( INT wid, BOOL resized );
/* A panel whose tags' sheet the program draws in (P_VIEW) was drawn: the window told OB_E_REDRAW */
IMPORT void knl_wmobj_view_drawn( INT wid );
IMPORT void knl_wmobj_event( CONST T_WMEV *ev );
/*
 * A window was given the keys (wm_focus): when a question of its
 * program's (a pop-up of the same process) is up without them, the
 * question takes them, and gives them back to this window when it goes
 */
IMPORT void knl_wmobj_focused( INT wid );
/*
 * A key from the keyboard as it came, before it is addressed to the
 * window with the keys: told to every window that never has the keys
 * (WM_ATTR_NOFOCUS) and is shown, to a request with OB_N_KEYSEEN
 */
IMPORT void knl_wmobj_key_seen( CONST T_WMEV *ev );
/*
 * A button or a key pressed, as the desktop reads it, wherever it goes:
 * OB_E_PRESSED told to every window that asked for it
 */
IMPORT void knl_wmobj_press_seen( CONST T_WMEV *ev );
/* A panel opened, made an object with its parts: E_NOMEM when their UUIDs could not be kept */
IMPORT ER   knl_wmobj_panel( INT pid );
IMPORT void knl_wmobj_panel_close( INT pid );
IMPORT void knl_wmobj_part( INT pid, INT i, UINT event, CONST T_WMEV *ev );
/* Whether a program hears a tag of part i of panel pid pulled out (OB_E_PULL) */
IMPORT BOOL knl_wmobj_pulls( INT pid, INT i );
/* A panel of tags: the tags that can be seen (OB_WR_TAGS), at most max; 0 for a part of another kind */
IMPORT INT  knl_pn_tags( INT pid, INT i, T_OBTAG *out, INT max );

/* The process that made a window: 0 for one of the system's own */
IMPORT ID   knl_wmobj_maker( INT wid );

/*
 * A part about to be drawn, in the box r of the drawing environment
 * gid: TRUE when it has looks of its own and they were drawn, so the
 * standard drawing is left out. 'on' is whether it is pressed or set.
 */
IMPORT BOOL knl_wmobj_part_look( INT pid, INT i, INT gid, CONST T_DPRECT *r, BOOL on );

#endif /* __WMOBJ_H__ */
