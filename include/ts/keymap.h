/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	keymap.h
 *	The keyboard's layouts and the keys given other meanings
 *	(outer_kernel/wm/keymap.c, docs/tessronos/11-desktop.md キー配列)
 *
 *	The keyboard sends key numbers (the USB usages, 0x04 the key where
 *	'a' is on most keyboards); what a number types is the layout's. The
 *	latin layout is the keys as they are written on the keyboard
 *	(the Japanese one or the English one, LK_KBD_US) or Dvorak laid on
 *	the same places. A key may also be given another key's number before
 *	anything else sees it: CapsLock made a Ctrl, say. A key made a
 *	modifier holds that modifier while it is down, for every program.
 */

#ifndef __TS_KEYMAP_H__
#define __TS_KEYMAP_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The latin layouts (LK_KBD_LAYOUT) */
#define KM_LATIN_KEYS	0	/* as the keys are written */
#define KM_LATIN_DVORAK	1	/* Dvorak, on the English keyboard's places */
#define KM_LATIN_N	2

/* The kana layouts (LK_KBD_KANA) */
#define KM_KANA_JIS	0	/* JIS かな, as the Japanese keyboard's keys are written */
#define KM_KANA_TRON	1	/* TRON かな配列 */
#define KM_KANA_NICOLA	2	/* 親指シフト (NICOLA) */
#define KM_KANA_N	3

#define KM_MAP_MAX	24	/* keys given another number at most: a line of the settings holds so many */

/*
 * The thumbs of the thumb-shift kana layouts: 無変換 under the left
 * thumb, 変換 under the right. While TRON かな or 親指シフト is chosen a
 * thumb key held marks the keys pressed meanwhile with these bits in
 * their modifiers, and is itself let through only when it was pressed
 * and let go alone (device/usb/hid.c).
 */
#define KM_THUMB_L_KEY	0x8B
#define KM_THUMB_R_KEY	0x8A
#define KM_MOD_THUMB_L	0x100
#define KM_MOD_THUMB_R	0x200

/*
 * A key's own layouts. A key put in by a screen keyboard (wm_obj_key_put)
 * carries in its modifiers the latin and the kana layout it is to be read
 * in: KM_MOD_OWN with the two numbers (KM_MOD_LAYOUT). The marks go with
 * the key wherever it goes, in the events and the notices' mods, and
 * everything that makes a letter or a kana of a key from its mods
 * (km_char, km_kana_text, km_label, wm_key_char, kc_kana_key_m) reads the
 * key in them. A key of the keyboard plugged in carries none, and is read
 * in the machine's layouts. So the screen keyboard's layouts are its own
 * and never the keyboard's.
 */
#define KM_MOD_OWN		0x00040000U
#define KM_MOD_LATIN_SHIFT	20		/* 2 bits: KM_LATIN_* */
#define KM_MOD_KANA_SHIFT	22		/* 2 bits: KM_KANA_* */
#define KM_MOD_LAYOUTS		( KM_MOD_OWN | 0x00F00000U )
#define KM_MOD_LAYOUT(latin, kana) \
	( KM_MOD_OWN | ( (UINT)(latin) << KM_MOD_LATIN_SHIFT ) | ( (UINT)(kana) << KM_MOD_KANA_SHIFT ) )

/* The layouts a key is read in: its own when its mods carry them, else the machine's */
IMPORT INT  km_latin_of( UINT mods );
IMPORT INT  km_kana_of( UINT mods );

/* The kana a key types in a thumb-shift layout, UTF-8, by the thumbs held; NULL none or JIS かな */
IMPORT CONST char *km_kana_text( UINT code, UINT mods );

/* Whether a key is under the left hand (the thumb on its own side is the same-hand shift) */
IMPORT BOOL km_left_hand( UINT code );

/* The letter a key types in the latin layout (km_latin_of), with what is held; 0 none */
IMPORT UB   km_char( UINT code, UINT mods );

/* The latin layout and the kana one in use */
IMPORT INT  km_latin( void );
IMPORT INT  km_kana( void );

/* A key's number as the keys given others make it */
IMPORT UINT km_remap( UINT code );

/* The modifier bits (HID_MOD_*) of modifier keys given another number */
IMPORT UINT km_mods_away( void );

/* The keys given other numbers: from[i] types as to[i]; how many */
IMPORT INT  km_map_get( UB *from, UB *to, INT max );
IMPORT ER   km_map_set( CONST UB *from, CONST UB *to, INT n );

/* The same as a line of the machine's settings ("39E0,E039"), and back */
IMPORT INT  km_map_text( char *buf, INT max );
IMPORT void km_map_parse( CONST char *s );

/* The layouts set and kept in the machine's settings */
IMPORT ER   km_set_latin( INT layout );
IMPORT ER   km_set_kana( INT layout );
IMPORT ER   km_map_save( void );

/* A key's name for the keyboard's picture and the list of keys given others */
IMPORT CONST char *km_key_name( UINT code );

/*
 * The same for a program running as a process, as system calls of the
 * window layer (lib/libts), and for the kernel alike: the layouts in
 * use and the keys given other numbers, read and set together, and the
 * words a key is written with.
 */
typedef struct {
	INT	latin;			/* KM_LATIN_* */
	INT	kana;			/* KM_KANA_* */
	BOOL	us;			/* the English keyboard (LK_KBD_US), else the Japanese */
	INT	nmap;			/* keys given another number */
	UB	from[KM_MAP_MAX];	/* from[i] types as to[i] */
	UB	to[KM_MAP_MAX];
} T_KMSTATE;

IMPORT ER   km_get( T_KMSTATE *st );

/*
 * What of st is set and kept in the machine's settings: the latin
 * layout, the kana layout, the keys given others (all of them, as st
 * lists them; none takes every key back). Nothing is set unless all
 * of it is right. From a process, only one that may write the machine's
 * settings (SYSDEF_CONF_DEV) may set them; E_OACV otherwise.
 */
#define KM_SET_LATIN	0x01
#define KM_SET_KANA	0x02
#define KM_SET_MAP	0x04
IMPORT ER   km_set( CONST T_KMSTATE *st, UINT what );

/*
 * The words a key is written with, UTF-8 into buf (at most max bytes
 * with the 0 after them); how many bytes, 0 when there are none. The
 * letter and the kana in the layouts mods carries (KM_MOD_LAYOUT), else
 * in the machine's.
 */
#define KM_L_NAME	0		/* its name (km_key_name) */
#define KM_L_CHAR	1		/* the letter it types in the latin layout, with mods (km_char) */
#define KM_L_KANA	2		/* the kana it types in a thumb-shift layout, with mods (km_kana_text) */
#define KM_L_JIS	3		/* its JIS かな, Shift in mods taking the small one */
IMPORT INT  km_label( UINT what, UINT code, UINT mods, char *buf, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_KEYMAP_H__ */
