/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uapp.h
 *	What a program running as a process draws its windows with
 *	(design 16.4.3, 18.13)
 *
 *	A program makes its window as a real object (ob_cre_obj,
 *	OB_T_WINDOW), and with the window's key takes the window's drawing
 *	environment (wm_obj_gid). It draws there with the dp_ calls of
 *	include/ts/dp.h that a process may make -- dp_fill_rect,
 *	dp_frame_rect, dp_line, dp_put_argb -- and the letters below, and
 *	shows what it drew with wm_obj_flush. A program that draws its
 *	pixels itself draws them in a surface of the work area mapped into
 *	its memory (wm_obj_surf) and shows the rectangles it changed with
 *	wm_obj_put. What happens to the window it is told through notices
 *	(ob_ntf_evt) at a channel of its own.
 *
 *	Its menu is made from a menu definition and shown on its window with
 *	the calls of include/ts/mn.h, mn_pop_men waiting for the answer; a key
 *	is turned into its letter with wm_key_char.
 *
 *	Files are reached with the fs_ calls of include/ts/fs.h (fs_open to
 *	fs_sync, fs_utime, fs_attach_dev, fs_detach_dev, fs_mounts): a descriptor is
 *	the process's that opened it and is closed when it ends. A volume
 *	is mounted through the key of its disk's device object, at
 *	FS_MEDIA_DIR "/" and the device's name ("/media/uda0"), as far as
 *	the key's rights on the blocks go; FS_MNT_RDONLY writes nothing.
 *	Sockets are in include/ts/soapp.h.
 */

#ifndef __TS_UAPP_H__
#define __TS_UAPP_H__

#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/dp.h>
#include <ts/mn.h>
#include <ts/kconv.h>
#include <ts/fs.h>
#include <ts/soapp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The drawing environment of a window the key may write; the process's own */
IMPORT INT wm_obj_gid( ID key );

/* What was drawn in the window shown: a rectangle of its work area, or NULL for all */
IMPORT ER  wm_obj_flush( ID key, CONST T_DPRECT *r );

/*
 * A surface of the window's work area in the process's own memory
 * (T_WMSURF, include/ts/wm.h), drawn in directly; and n rectangles of
 * it (work area coordinates, at most WM_PUT_MAX) put in the window and
 * shown, as wm_obj_flush shows what was drawn. The surface is as large
 * as the work area was when it was asked for; after the window changes
 * size it is asked for again (wm_obj_surf answers the same surface while
 * the size is the same). One asked for anew, and one whose window has
 * closed, is no longer the window's: what is written there goes nowhere,
 * and wm_obj_put answers E_OBJ or E_ID.
 */
IMPORT ER  wm_obj_surf( ID key, T_WMSURF *s );
IMPORT ER  wm_obj_put( ID key, CONST T_DPRECT *r, INT n );

/*
 * A screen keyboard: its window made with WM_ATTR_NOFOCUS in its "attr"
 * never has the keys, so the window that had them keeps them. A key
 * pressed on its picture is typed there with wm_key_put (the key of the
 * window, the key's usage, the modifiers held with it), as wm_obj_key_put
 * of include/ts/wm.h says: only just after a press on the window, and
 * with WM_KEY_PUT_DOWN and WM_KEY_PUT_UP held for as long as the picture's
 * key is. The modifiers may carry the screen keyboard's own layouts
 * (KM_MOD_LAYOUT), which the key is read in instead of the machine's. The
 * keys pressed on the keyboard itself are told to the window when its
 * request has OB_N_KEYSEEN (include/ts/ob.h). The machine's layouts and
 * the keys given others are read and set with km_get, km_set and km_label
 * of include/ts/keymap.h.
 */
IMPORT ER  wm_key_put( ID key, UINT code, UINT mods );

/*
 * The places of the window where a finger presses the main button as it
 * touches, a hold there being no second button: wm_obj_press_area of
 * include/ts/wm.h. The window's program gives them again when its work
 * area changes.
 */
IMPORT ER  wm_press_area( ID key, CONST T_DPRECT *r, INT n );

/*
 * Letters in the system's face, px pixels tall (0: as it is), with the
 * baseline at y. Answers how far to the right they reached.
 */
IMPORT INT dp_text( INT gid, INT x, INT y, CONST UB *utf8, UW colour, INT px );
IMPORT INT dp_text_width( CONST UB *utf8, INT px );

/*
 * A figure in xmlTAD (<tad><figure>...</figure></tad>) drawn over what is
 * there: a point (x, y) of the figure lands on (ox + x, oy + y), and
 * nothing goes outside `clip`.
 */
IMPORT ER  dp_draw_tad( INT gid, CONST T_DPRECT *clip, INT ox, INT oy,
			CONST UB *xml, SZ len );

/*
 * A picture file -- PNG, JPEG, BMP, GIF or ICO -- as 0x00rrggbb pixels, row after
 * row, into px (room for maxpx of them). With px NULL only its size is
 * said; with too little room, E_LIMIT and the size.
 */
IMPORT ER  dp_img_decode( CONST UB *data, SZ size, UW *px, SZ maxpx,
			  INT *p_w, INT *p_h );

/*
 * かな漢字変換 in a window of one's own: a session opened with kc_open
 * (closed with kc_close), and a key as the keyboard gave it (the usage
 * and its modifiers) handed to it with the input mode it came in -- the
 * notice's imode (T_OBNTM), never kc_mode, which keys typed after it
 * may have changed already. What comes back is as kc_key's in
 * include/ts/kconv.h: KC_NOTMINE for a key that is not the converter's
 * -- every key in 英語 -- or its TSMOZC_* flags, the composition in out.
 * A key in 英語 that comes while something is being composed commits it
 * first: the program hands the converter Shift+Enter in the mode the
 * composition was typed in, then takes the key as 英語's. kc_choose and
 * kc_list are as there. kc_mode is the input mode the message line shows
 * now (WM_MODE_* of include/ts/wm.h), for showing it.
 */
IMPORT INT  kc_hid_key( INT kid, UINT key, UINT mods, UINT mode, T_KCOUT *out );
IMPORT UINT kc_mode( void );

/*
 * A message on the system's message line, the band at the foot of the
 * screen (one line of UTF-8, to its first newline; NULL takes it away).
 * It stays until the next press or key. Declared in include/ts/wm.h as
 * the kernel's own call of the same name.
 */

/* The C library functions lib/libts gives (the compiler calls these itself) */
extern void *memset( void *d, int c, unsigned long n );
extern void *memcpy( void *d, CONST void *s, unsigned long n );

/* Processes, time and the console */
IMPORT void ts_ext_prc( INT exitcd );
IMPORT ID   ts_get_pid( void );
IMPORT SZ   ts_get_arg( void *buf, SZ size );	/* the start-up argument; its size */
IMPORT ER   tk_dly_tsk( RELTIM dlytim );
IMPORT ER   tm_putstring( CONST UB *s );

/*
 * Also for a process, declared where the kernel's are: ts_gen_uuid and
 * ts_get_random (include/ts/uuid.h; in a process it reads the random
 * source object 乱数, lib/libts/ts_random.c), ob_att_vol and ob_det_vol
 * (include/ts/ob.h), and the tray's tr_ calls (include/ts/tray.h).
 */

/*
 * The administrator's rights for one operation (lib/libts/ts_admin.c):
 * ts_admin_ask asks the desktop, which asks the person for the
 * administrator's password (DT_RQ_ADMIN of include/ts/dtreq.h); what
 * says what is to be done, win is the program's window (NULL: none).
 * E_OK when the process acts as an administrator until ob_end_adm,
 * E_ABORT when the person put the panel away, E_OACV when the password
 * was typed wrong, E_NOEXS when there is no desktop. ts_no_right tells
 * the person on the message line that what could not be done for want
 * of the rights ("時刻を設定" says 時刻を設定する権限がありません).
 */
IMPORT ER   ts_admin_ask( CONST char *what, CONST TS_UUID *win );
IMPORT void ts_no_right( CONST char *what );

/*
 * What went to the console, from *p_pos on (0: from the oldest kept),
 * and *p_pos moved past it; how many bytes were given.
 */
IMPORT INT  tm_log_read( UD *p_pos, UB *buf, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UAPP_H__ */
