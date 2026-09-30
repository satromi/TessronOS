/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	look.c
 *	The numbered look table, and the schemes (design 16.2.2, 16.5.7)
 *
 *	One table, found by number. An entry holds a value and, when it is
 *	a pattern, a tone and a second colour; what an entry means is the
 *	reader's business, which is why the same call answers a colour, a
 *	length and a setting.
 *
 *	The numbers are in <ts/look.h>; definitions name entries by them.
 *
 *	A scheme is a list of numbers and what to put in them. Putting one
 *	on writes those numbers, clears the tones they had, and leaves
 *	everything it does not mention alone -- which is what lets a scheme
 *	be a short list rather than a copy of the whole table.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>
#include <ts/look.h>
#include <ts/wm.h>
#include <ts/conf.h>

#define LOOK_MAX	192		/* entries the table holds */
#define LOOK_STRIPE	0x80		/* a tone that is the warning's stripes */

typedef struct {
	UH	num;
	UB	tone;			/* 0: a plain value */
	UB	used;
	UW	fore;
	UW	back;
} LOOKENT;

LOCAL LOOKENT		look_tab[LOOK_MAX];
LOCAL UINT		look_scheme = WM_SCHEME_LIGHT;
LOCAL BOOL		look_hand = FALSE;

typedef struct {
	UH	num;
	UW	value;			/* the colour, or the first colour of a pattern */
	UB	tone;			/* 0: plain; 1..6: a half-tone of the two;
					   LOOK_STRIPE: the warning's stripes */
	UW	back;			/* the second colour of a pattern */
} LOOKDEF;

/*
 * What every entry starts as, and what the scheme "akarui" is: a mid
 * grey band for the window that has the input and a pale one for the
 * others, white menus framed in black, pale grey parts with green
 * lamps, a green panel frame, and a title's and a menu's letters
 * sixteen high, a scroll bar sixteen wide. A pattern is its two
 * colours through one of the eight masks.
 *
 * A number not listed answers nought, which is what a caller that asks
 * for something that has no meaning should get.
 */
LOCAL CONST LOOKDEF	look_default[] = {
	{ LK_GROUND,		0x00204050U, 0, 0 },
	{ LK_TITLE_H,		16, 0, 0 },
	{ LK_MENU_H,		16, 0, 0 },	/* the menus' letters */
	{ LK_BAR_W,		16, 0, 0 },
	{ LK_DRAG_W,		2, 0, 0 },
	{ LK_DBLTIME,		400, 0, 0 },
	{ LK_BLINK,		800, 0, 0 },
	{ LK_CARET_W,		1, 0, 0 },
	{ LK_MARCH,		800, 0, 0 },
	{ LK_SEL_W,		1, 0, 0 },
	{ LK_PART_RAD,		12, 0, 0 },

	{ LK_GROUND_1ST,	0x00000000U, 3, 0x00FFFFFFU },
	{ LK_PNLGROUND,		0x00E0E0E0U, 0, 0 },
	{ LK_OUTLINE,		0x00000000U, 0, 0 },
	{ LK_LIGHT,		0x00FFFFFFU, 0, 0 },
	{ LK_SHADOW,		0x00000000U, 0, 0 },
	{ LK_MSGGREY,		0x00808080U, 0, 0 },
	{ LK_ACTFRAME,		0x00808080U, 0, 0 },
	{ LK_INACTFRAME,	0x00E0E0E0U, 0, 0 },
	{ LK_SUBFRAME,		0x00808080U, 0, 0 },
	{ LK_TOPFRAME,		0x00808080U, 0, 0 },
	{ LK_MENUGROUND,	0x00FFFFFFU, 0, 0 },
	{ LK_MENUFRAME,		0x00000000U, 0, 0 },
	{ LK_SUBMGROUND,	0x00FFFFFFU, 0, 0 },
	{ LK_SUBMFRAME,		0x00000000U, 0, 0 },
	{ LK_ACTPARTS,		0x00FFFFFFU, 0, 0 },
	{ LK_INACTPARTS,	0x00E0E0E0U, 0, 0 },
	{ LK_VOLKNOB,		0x00E0E0E0U, 0, 0 },
	{ LK_VOLBACK,		0x00808080U, 0, 0 },
	{ LK_LAMPON_IN,		0x0000FF00U, 4, 0x008080A0U },
	{ LK_LAMPON_LT,		0x0000FF00U, 4, 0x00000000U },
	{ LK_LAMPOFF_IN,	0x00E0E0E0U, 0, 0 },
	{ LK_LAMPOFF_LT,	0x00D0FFFFU, 4, 0x00000000U },
	{ LK_PEMPHAS,		0x008080A0U, 0, 0 },
	{ LK_VOLTOMBO,		0x00E0E000U, 0, 0 },
	{ LK_SBARKNOB,		0x00E0E0E0U, 0, 0 },
	{ LK_SBARBACK,		0x00808080U, 0, 0 },
	{ LK_SBARTOMBO,		0x00E0E000U, 0, 0 },
	{ LK_MARKON_IN,		0x0000FF00U, 4, 0x008080A0U },
	{ LK_MARKON_LT,		0x0000FF00U, 4, 0x00000000U },
	{ LK_MARKOFF_IN,	0x00E0E0E0U, 0, 0 },
	{ LK_MARKOFF_LT,	0x00D0FFFFU, 4, 0x00000000U },
	{ LK_PNLFRAME,		0x0000EF00U, 4, 0x007F7F9FU },
	{ LK_POPFRAME,		0x00808080U, 0, 0 },
	{ LK_MSGWHITE,		0x00FFFFFFU, 0, 0 },
	{ LK_WRNFRAME,		0x00000000U, LOOK_STRIPE, 0x00F0F000U },
	{ LK_FATFRAME,		0x00FF0000U, 0, 0 },

	{ LK_ACTWTFCOL,		0x00FFFFFFU, 0, 0 },
	{ LK_ACTWTBCOL,		0x00000000U, 0, 0 },
	{ LK_INWTFCOL,		0x00FFFFFFU, 0, 0 },
	{ LK_INWTBCOL,		0x00000000U, 0, 0 },
	{ LK_ACTMODECOL,	0x00808080U, 0, 0 },
	{ LK_ACTMODEBCOL,	0x00E0E0E0U, 0, 0 },
	{ LK_INMODECOL,		0x00E0E0E0U, 0, 0 },
	{ LK_INMODEBCOL,	0x00808080U, 0, 0 },
	{ LK_MENUCHCOL,		0x00000000U, 0, 0 },
	{ LK_MENUINACT,		0x00E0E0E0U, 0, 0 },
	{ LK_MENUKEYCOL,	0x00000000U, 0, 0 },
	{ LK_MENUKEYINACT,	0x00E0E0E0U, 0, 0 },
	{ LK_MSGCOL,		0x00FFFFFFU, 0, 0 },
	{ LK_ACTPARTSCOL,	0x00000000U, 0, 0 },
	{ LK_INACTPARTSCOL,	0x00808080U, 0, 0 },
	{ LK_LIGHTPARTSCOL,	0x00FFFFFFU, 0, 0 },

	{ LK_LOOK,		1, 0, 0 },
	{ LK_OPACITY,		255, 0, 0 },
	{ LK_WASH,		0x00202838U, 0, 0 },
	{ LK_WASH_STR,		0, 0, 0 },
	{ LK_ROUND,		0, 0, 0 },
	{ LK_SHD_DX,		4, 0, 0 },
	{ LK_SHD_DY,		4, 0, 0 },
	{ LK_SHD_SIZE,		2, 0, 0 },
	{ LK_SHD_DARK,		0, 0, 0 },
	{ LK_BAR_WASH,		0x00405878U, 0, 0 },
	{ LK_BAR_STR,		0, 0, 0 },
	{ LK_MOTION,		0, 0, 0 },
	{ LK_BLUR,		0, 0, 0 },
	{ LK_FROM,		1, 0, 0 },
	{ LK_LIVE,		1, 0, 0 },
};

/*
 * The schemes. Each says only the entries it changes; everything else
 * keeps what it had, which is why a scheme is a short list.
 */
LOCAL CONST LOOKDEF	scheme_dark[] = {
	{ LK_GROUND,		0x00101418U, 0, 0 },
	{ LK_OUTLINE,		0x00000000U, 0, 0 },
	{ LK_LIGHT,		0x00707070U, 0, 0 },
	{ LK_SHADOW,		0x00101010U, 0, 0 },
	{ LK_ACTFRAME,		0x00404858U, 0, 0 },
	{ LK_INACTFRAME,	0x00303030U, 0, 0 },
	{ LK_SUBFRAME,		0x00282828U, 0, 0 },
	{ LK_TOPFRAME,		0x00485060U, 0, 0 },
	{ LK_GROUND_1ST,	0x00202020U, 0, 0 },
	{ LK_ACTPARTS,		0x00404040U, 0, 0 },
	{ LK_INACTPARTS,	0x00383838U, 0, 0 },
	{ LK_LAMPON_IN,		0x0080C0FFU, 0, 0 },
	{ LK_LAMPON_LT,		0x00FFFFFFU, 0, 0 },
	{ LK_LAMPOFF_IN,	0x00303030U, 0, 0 },
	{ LK_LAMPOFF_LT,	0x00101010U, 0, 0 },
	{ LK_PEMPHAS,		0x00C0C0E0U, 0, 0 },
	{ LK_SBARKNOB,		0x00585858U, 0, 0 },
	{ LK_SBARBACK,		0x00383838U, 0, 0 },
	{ LK_SBARTOMBO,		0x00A0A0A0U, 0, 0 },
	{ LK_ACTWTFCOL,		0x00E8E8E8U, 0, 0 },
	{ LK_ACTWTBCOL,		0x00000000U, 0, 0 },
	{ LK_ACTPARTSCOL,	0x00E8E8E8U, 0, 0 },
	{ LK_INACTPARTSCOL,	0x00808080U, 0, 0 },
};

LOCAL CONST LOOKDEF	scheme_contrast[] = {
	{ LK_GROUND,		0x00000000U, 0, 0 },
	{ LK_OUTLINE,		0x00000000U, 0, 0 },
	{ LK_LIGHT,		0x00FFFFFFU, 0, 0 },
	{ LK_SHADOW,		0x00000000U, 0, 0 },
	{ LK_ACTFRAME,		0x00000000U, 0, 0 },
	{ LK_INACTFRAME,	0x00FFFFFFU, 0, 0 },
	{ LK_SUBFRAME,		0x00FFFFFFU, 0, 0 },
	{ LK_TOPFRAME,		0x00000000U, 0, 0 },
	{ LK_GROUND_1ST,	0x00FFFFFFU, 0, 0 },
	{ LK_ACTPARTS,		0x00FFFFFFU, 0, 0 },
	{ LK_INACTPARTS,	0x00FFFFFFU, 0, 0 },
	{ LK_LAMPON_IN,		0x00000000U, 0, 0 },
	{ LK_LAMPON_LT,		0x00000000U, 0, 0 },
	{ LK_LAMPOFF_IN,	0x00FFFFFFU, 0, 0 },
	{ LK_LAMPOFF_LT,	0x00000000U, 0, 0 },
	{ LK_PEMPHAS,		0x00000000U, 0, 0 },
	{ LK_SBARKNOB,		0x00000000U, 0, 0 },
	{ LK_SBARBACK,		0x00FFFFFFU, 0, 0 },
	{ LK_SBARTOMBO,		0x00FFFFFFU, 0, 0 },
	{ LK_ACTWTFCOL,		0x00FFFFFFU, 0, 0 },
	{ LK_ACTWTBCOL,		0x00000000U, 0, 0 },
	{ LK_ACTPARTSCOL,	0x00000000U, 0, 0 },
	{ LK_INACTPARTSCOL,	0x00000000U, 0, 0 },
};

LOCAL CONST LOOKDEF	scheme_blue[] = {
	{ LK_GROUND,		0x00183048U, 0, 0 },
	{ LK_OUTLINE,		0x00000000U, 0, 0 },
	{ LK_LIGHT,		0x00E0E8F0U, 0, 0 },
	{ LK_SHADOW,		0x00405060U, 0, 0 },
	{ LK_ACTFRAME,		0x00405878U, 0, 0 },
	{ LK_INACTFRAME,	0x00808890U, 0, 0 },
	{ LK_SUBFRAME,		0x00708090U, 0, 0 },
	{ LK_TOPFRAME,		0x00506888U, 0, 0 },
	{ LK_GROUND_1ST,	0x00FFFFFFU, 0, 0 },
	{ LK_ACTPARTS,		0x00C8D0DCU, 0, 0 },
	{ LK_INACTPARTS,	0x00C0C4C8U, 0, 0 },
	{ LK_LAMPON_IN,		0x00204060U, 0, 0 },
	{ LK_LAMPON_LT,		0x00FFFFFFU, 0, 0 },
	{ LK_LAMPOFF_IN,	0x00D8DCE0U, 0, 0 },
	{ LK_LAMPOFF_LT,	0x00808890U, 0, 0 },
	{ LK_PEMPHAS,		0x00405878U, 0, 0 },
	{ LK_SBARKNOB,		0x00A8B8CCU, 0, 0 },
	{ LK_SBARBACK,		0x00C8D0DCU, 0, 0 },
	{ LK_SBARTOMBO,		0x00405878U, 0, 0 },
	{ LK_ACTWTFCOL,		0x00FFFFFFU, 0, 0 },
	{ LK_ACTWTBCOL,		0x00203040U, 0, 0 },
	{ LK_ACTPARTSCOL,	0x00102030U, 0, 0 },
	{ LK_INACTPARTSCOL,	0x00708090U, 0, 0 },
};

typedef struct {
	CONST LOOKDEF	*def;
	INT		n;
	CONST char	*name;
} SCHEME;

#define NDEF(a)		( (INT)(sizeof(a) / sizeof((a)[0])) )

LOCAL CONST SCHEME	scheme[WM_SCHEME_MAX] = {
	{ look_default,    NDEF(look_default),    "akarui" },
	{ scheme_dark,     NDEF(scheme_dark),     "kurai" },
	{ scheme_contrast, NDEF(scheme_contrast), "kousa" },
	{ scheme_blue,     NDEF(scheme_blue),     "ao" },
};

/* ---------------------------------------------------------------- store */

LOCAL LOOKENT *ent_of( UINT num, BOOL make )
{
	INT	i, free_at = -1;

	for ( i = 0; i < LOOK_MAX; i++ ) {
		if ( look_tab[i].used && look_tab[i].num == (UH)num ) {
			return &look_tab[i];
		}
		if ( !look_tab[i].used && free_at < 0 ) {
			free_at = i;
		}
	}
	if ( !make || free_at < 0 ) {
		return NULL;
	}
	look_tab[free_at].used = TRUE;
	look_tab[free_at].num  = (UH)num;
	look_tab[free_at].tone = 0;
	look_tab[free_at].fore = 0;
	look_tab[free_at].back = 0;

	return &look_tab[free_at];
}

EXPORT INT knl_look_init( void )
{
	INT	i;

	for ( i = 0; i < LOOK_MAX; i++ ) {
		look_tab[i].used = FALSE;
	}
	for ( i = 0; i < NDEF(look_default); i++ ) {
		LOOKENT	*e = ent_of(look_default[i].num, TRUE);

		if ( e != NULL ) {
			e->fore = look_default[i].value;
			e->tone = look_default[i].tone;
			e->back = look_default[i].back;
		}
	}
	/*
	 * What a person sets starts as ユーザ環境設定 has it when nothing is
	 * said (lib/libconf); the person's record, read once the store is
	 * there, sets it as it was left (wm_conf_apply).
	 */
	for ( i = 0; i < cf_user_nitem; i++ ) {
		LOOKENT	*e = ent_of(cf_user_items[i].look, TRUE);

		if ( e != NULL ) {
			e->fore = (UW)cf_user_items[i].dflt;
			e->tone = 0;
		}
	}
	look_scheme = WM_SCHEME_LIGHT;
	look_hand = FALSE;

	return 1;
}

/*
 * A pattern's own colour: its two colours mixed in the share of dots
 * its mask sets, eighths out of eight for the half-tones. What draws a
 * line or a letter in an entry that is a pattern gets this.
 */
LOCAL UW mix_of( UW fore, UW back, UINT eighths )
{
	UW	r, g, b;

	r = ( ( ( fore >> 16 ) & 0xFF ) * eighths
	    + ( ( back >> 16 ) & 0xFF ) * ( 8 - eighths ) ) / 8;
	g = ( ( ( fore >> 8 ) & 0xFF ) * eighths
	    + ( ( back >> 8 ) & 0xFF ) * ( 8 - eighths ) ) / 8;
	b = ( ( fore & 0xFF ) * eighths + ( back & 0xFF ) * ( 8 - eighths ) ) / 8;

	return ( r << 16 ) | ( g << 8 ) | b;
}

EXPORT UW wm_look( UINT num )
{
	CONST LOOKENT	*e = ent_of(num, FALSE);
	CONST UB	eighths[8] = { 0, 0, 1, 2, 4, 6, 7, 8 };

	if ( e == NULL ) {
		return 0;
	}
	if ( e->tone == LOOK_STRIPE ) {
		return mix_of(e->fore, e->back, 4);
	}
	if ( e->tone > 0 && e->tone < 8 ) {
		return mix_of(e->fore, e->back, eighths[e->tone]);
	}

	return e->fore;
}

EXPORT INT wm_num( UINT num, INT dflt )
{
	CONST LOOKENT	*e = ent_of(num, FALSE);

	return ( e != NULL ) ? (INT)e->fore : dflt;
}

EXPORT ER wm_set_look( UINT num, UW value )
{
	LOOKENT	*e = ent_of(num, TRUE);

	if ( e == NULL ) {
		return E_LIMIT;
	}
	e->fore = value;
	e->tone = 0;
	look_hand = TRUE;

	return E_OK;
}

/*
 * One of what a person sets (the 9000s, and the lengths and times
 * ユーザ環境設定 shares with the windows): written without marking the
 * scheme as changed by hand, since it is not a colour of the scheme.
 */
EXPORT ER knl_look_setting( UINT num, UW value )
{
	LOOKENT	*e = ent_of(num, TRUE);
	BOOL	changed;

	if ( e == NULL ) {
		return E_LIMIT;
	}
	changed = (BOOL)( e->fore != value || e->tone != 0 );
	e->fore = value;
	e->tone = 0;
	if ( num >= LK_KEY_ON && num <= LK_DBL_W ) {
		knl_input_sync();		/* the keyboard's and the pointer's */
	}
	if ( changed && ( num == LK_TITLE_H || num == LK_BAR_W ) ) {
		wm_relayout();			/* every frame measured again */
	}
	return E_OK;
}

EXPORT ER wm_set_look_tone( UINT num, INT tone, UW fore, UW back )
{
	LOOKENT	*e;

	if ( tone < 0 || tone >= DP_TONE_MAX ) {
		return E_PAR;
	}
	e = ent_of(num, TRUE);
	if ( e == NULL ) {
		return E_LIMIT;
	}
	e->fore = fore;
	e->back = back;
	e->tone = (UB)tone;
	look_hand = TRUE;

	return E_OK;
}

/*
 * The warning's stripes: thirty-two square, each row fifteen of the
 * first colour and seventeen of the second, each row one further on,
 * so that they run corner to corner.
 */
#define STRIPE		32

LOCAL UW	stripe_tile[STRIPE * STRIPE];

LOCAL void stripes( T_DPPAT *pat, UW fore, UW back )
{
	INT	x, y;

	for ( y = 0; y < STRIPE; y++ ) {
		for ( x = 0; x < STRIPE; x++ ) {
			stripe_tile[y * STRIPE + x] =
				( ( x + y ) % STRIPE < 15 ) ? fore : back;
		}
	}
	knl_memset(pat, 0, sizeof(*pat));
	pat->kind = DP_PAT_TILE;
	pat->tile = stripe_tile;
	pat->hs = STRIPE;
	pat->vs = STRIPE;
	pat->mask = NULL;
	pat->fore = fore;
	pat->back = back;
}

EXPORT ER wm_look_pat( UINT num, T_DPPAT *pat )
{
	CONST LOOKENT	*e = ent_of(num, FALSE);

	if ( pat == NULL ) {
		return E_PAR;
	}
	if ( e == NULL ) {
		return E_NOEXS;
	}
	if ( e->tone == 0 ) {
		dp_pat_colour(pat, e->fore);
	} else if ( e->tone == LOOK_STRIPE ) {
		stripes(pat, e->fore, e->back);
	} else {
		dp_pat_tone(pat, (INT)e->tone, e->fore, e->back);
	}

	return E_OK;
}

/* ---------------------------------------------------------------- schemes */

EXPORT UINT wm_scheme( void )
{
	return look_scheme;
}

EXPORT BOOL wm_scheme_changed( void )
{
	return look_hand;
}

EXPORT CONST char *wm_scheme_name( UINT n )
{
	return ( n < WM_SCHEME_MAX ) ? scheme[n].name : NULL;
}

/* The colours written; the windows are drawn again by the caller */
EXPORT ER knl_look_scheme( UINT n )
{
	INT	i;

	if ( n >= WM_SCHEME_MAX ) {
		return E_PAR;
	}
	for ( i = 0; i < scheme[n].n; i++ ) {
		LOOKENT	*e = ent_of(scheme[n].def[i].num, TRUE);

		if ( e != NULL ) {
			e->fore = scheme[n].def[i].value;
			e->tone = scheme[n].def[i].tone;
			e->back = scheme[n].def[i].back;
		}
	}
	look_scheme = n;
	look_hand = FALSE;

	return E_OK;
}
