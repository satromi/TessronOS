/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sk.h
 *	ソフトウェアキーボード: the tables of its layouts (design 11.31)
 */

#ifndef SK_H
#define SK_H

#include <tk/typedef.h>
#include <ts/dp.h>

/* What a key does */
#define SK_KEY		0		/* typed: code (with Fn held, fn when it has one) */
#define SK_MOD		1		/* a modifier (0xE0..0xE7): held for the next key, or until pressed again */
#define SK_IME		2		/* 日本語 on and off: ひらがなカタカナ or 英数 typed, as the mode stands */
#define SK_FN		3		/* Fn: the next key types its fn, or every key until pressed again */
#define SK_ROW		0xFF		/* the end of a row */

/* Its side, for a keyboard split in two windows at the screen's edges */
#define SKF_L		0x00
#define SKF_R		0x01

/* A key of a layout laid out in rows of keys of widths in quarters of a key */
typedef struct {
	UB		code;		/* the key's number (HID usage) */
	UB		w;		/* its width in quarters of a key */
	UB		act;		/* SK_* */
	UB		flags;		/* SKF_* */
	CONST char	*name;		/* NULL: the letter it types */
} SKKEY;

/*
 * A key of a layout laid out by its shape: the corners of its outline,
 * eight of them (the four sides and the four corners cut where they are
 * rounded), in tenths of a unit of the layout's plane, round the key in
 * order. A key may be turned to any angle.
 */
#define SK_NPT		8
typedef struct {
	UB		code;		/* the key's number (HID usage); 0 for Fn */
	UB		fn;		/* what it types with Fn held, 0 none */
	UB		act;		/* SK_* */
	UB		flags;		/* SKF_* */
	CONST char	*name;		/* NULL: the letter it types */
	H		pt[SK_NPT][2];
} SKPLACE;

/* The TRON keyboard (sk_tron.c): its keys, and the plane they are laid out on */
IMPORT CONST SKPLACE	sk_tron[];
IMPORT CONST INT	sk_ntron;
#define SK_TRON_W	8176		/* tenths of a unit */
#define SK_TRON_H	3254
/*
 * and each key's outline as it is drawn: the four sides and the four
 * rounded corners, each corner a curve in six pieces, in the order of
 * sk_tron[]
 */
#define SK_TRON_NL	28
IMPORT CONST H		sk_tron_line[][SK_TRON_NL][2];

/*
 * A key of a layout drawn as its chart draws it (sk_utron.c): what it
 * types and its outline. The letters of its faces are typed as whatever
 * usage types them in the latin layout set; in 日本語 with kana typed,
 * a key with TRON かな on its faces types the usage kan, whose kana the
 * layout gives.
 */
typedef struct {
	UB		code;		/* the usage it types when no letter of it is typed; 0 none */
	UB		fn;		/* what it types with Fn held, 0 none */
	UB		act;		/* SK_* */
	UB		flags;		/* SKF_*: the half it is in */
	CONST char	*name;		/* a key that types no letter: what it is */
	UB		kan;		/* the usage whose TRON かな it types, 0 none */
	char		pl, red, blue;	/* the letter of its plain face, with the left (red) and the right
					   (blue) Shift; 0 none */
	UH		off, n;		/* its outline: n points from sk_ut_pt[off] */
} SKVKEY;

/* A mark drawn on a key: outlines filled (even-odd) in a colour */
typedef struct {
	UH		key;		/* sk_ut_key[] */
	UINT		rgb;
	UH		off;		/* its points from sk_ut_pt[off] */
	UH		cn, ncn;	/* the points of each of its ncn outlines: sk_ut_cn[cn...] */
} SKVMARK;

/* A half of the keyboard: the box round it, and its outline */
typedef struct {
	H		l, t, r, b;
	UH		off, n;
} SKVHALF;

/* The μTRON keyboard (sk_utron.c), on a plane of 842 x 595 units, in tenths */
IMPORT CONST H		sk_ut_pt[];	/* x, y pairs */
IMPORT CONST UH		sk_ut_cn[];
IMPORT CONST SKVMARK	sk_ut_mark[];
IMPORT CONST INT	sk_ut_nmark;
IMPORT CONST SKVKEY	sk_ut_key[];
IMPORT CONST INT	sk_ut_nkey;
IMPORT CONST SKVHALF	sk_ut_half[2];
#define SK_UT_W		8420
#define SK_UT_H		5952
#define SK_UT_GROUND	0x00D5D9E3U	/* the chart's ground round the halves */
#define SK_UT_EDGE	0x00231F20U	/* and the keys' outlines */

/* Drawing with the edges smoothed, in a surface of pixels 0x00rrggbb (sk_draw.c) */
typedef struct {
	UINT		*px;
	INT		w, h;		/* in pixels */
	INT		pitch;		/* in pixels */
} SKSURF;

#define SK_FIX		16		/* a pixel's parts in the points given */

/*
 * Outlines filled (ncn of them, cn[i] points each, x, y in sixteenths of
 * a pixel), even-odd or by winding, each pixel covered as much as the
 * shape covers it, inside clip
 */
IMPORT void sk_fill( SKSURF *s, CONST INT *xy, CONST INT *cn, INT ncn, UINT rgb, BOOL evenodd,
		     CONST T_DPRECT *clip );
IMPORT void sk_rect( SKSURF *s, CONST T_DPRECT *r, UINT rgb );
/* An outline moved in by d sixteenths (its corners mitred), into out */
IMPORT void sk_inset( CONST INT *xy, INT n, INT d, INT *out );

#endif /* SK_H */
