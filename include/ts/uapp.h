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
 *	shows what it drew with wm_obj_flush. What happens to the window it
 *	is told through notices (ob_ntf_evt) at a channel of its own.
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
 * and its modifiers) handed to it. What comes back is as kc_key's in
 * include/ts/kconv.h: KC_NOTMINE for a key that is not the converter's
 * -- every key in 英語 -- or its TSMOZC_* flags, the composition in out.
 * kc_choose and kc_list are as there. kc_mode is the input mode the
 * message line shows (WM_MODE_* of include/ts/wm.h).
 */
IMPORT INT  kc_hid_key( INT kid, UINT key, UINT mods, T_KCOUT *out );
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
 * (include/ts/ob.h).
 */

/*
 * What went to the console, from *p_pos on (0: from the oldest kept),
 * and *p_pos moved past it; how many bytes were given.
 */
IMPORT INT  tm_log_read( UD *p_pos, UB *buf, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UAPP_H__ */
