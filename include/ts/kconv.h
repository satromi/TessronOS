/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kconv.h
 *	かな漢字変換: the converter the text input port uses (design 17, 12d)
 *
 *	Keys go in; what comes out is the composition -- the clauses the
 *	converter is still working on, with the committed ones at its head
 *	once -- and, while candidates are being chosen, the list of them.
 *	Drawing the composition and the list, and putting the committed
 *	text into the document, are the port's (the desktop's TIP).
 *
 *	Which converter answers is decided when the system is built: mozc
 *	(make MOZC=1, the engine from tools/mozc) converts to kanji; without
 *	it a converter of romaji to kana stands in, with the same calls, so
 *	that the port is the same either way. mozc runs in a task of its own
 *	and the calls wait for it.
 *
 *	The composition is tsmozc_out (ts/tsmozc.h): UTF-8, the clauses as
 *	byte offsets. Keys are BTRON key codes, TSMOZC_K_* for those that
 *	are not characters.
 */

#ifndef __TS_KCONV_H__
#define __TS_KCONV_H__

#include <ts/tsmozc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef tsmozc_out	T_KCOUT;

#define KC_NOTMINE	TSMOZC_NOTMINE

/* The converter started: mozc is made in the background */
IMPORT ER   kc_start( void );

/* Whether it converts to kanji (mozc is there and running) */
IMPORT BOOL kc_kanji( void );

/* A session for one input port: its number, or an error */
IMPORT INT  kc_open( void );
IMPORT void kc_close( INT kid );

/*
 * How the session's letters are typed: TSMOZC_M_ROMAN for romaji (keys
 * as ASCII letters), 0 for kana keys (keys as the kana's TRON codes).
 */
IMPORT ER   kc_input( INT kid, INT mode );

/*
 * A key: the TSMOZC_* flags of what it did, KC_NOTMINE when the key is
 * not the converter's (no composition to act on), or an error. stat is
 * the keyboard's TSMOZC_S_* state: shift, and katakana for kana typed
 * as katakana.
 */
IMPORT INT  kc_key( INT kid, UINT code, UINT stat, T_KCOUT *out );

/*
 * What a key of the keyboard (a HID usage, with its modifiers) is to the
 * converter in 日本語: one of the TSMOZC_K_* keys, or 0 for a letter's
 * key; and a letter's key as the kana on it (kana keys), or 0.
 */
IMPORT UINT kc_code_key( UINT key, UINT mods );
IMPORT UINT kc_kana_key( UINT key, BOOL shift );

/* Candidate n (from 1) chosen from the list shown */
IMPORT INT  kc_choose( INT kid, INT n, T_KCOUT *out );

/*
 * The candidates shown, UTF-8 each ending in 0: how many are in buf,
 * the number of the first (from 1) and the total. Answers the chosen
 * one, or an error when there is no list.
 */
IMPORT INT  kc_list( INT kid, UB *buf, INT size, INT *p_count, INT *p_first,
		     INT *p_total );

#ifdef __cplusplus
}
#endif

#endif /* __TS_KCONV_H__ */
