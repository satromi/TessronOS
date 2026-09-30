/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	look.h
 *	The numbered look table (design 16.2.2, 16.5.7)
 *
 *	Everything the system draws its own furniture with is an entry in
 *	one table, found by number. A number answers a colour, a pattern, a
 *	length or a setting depending on what the entry is for; the table
 *	does not care, and whoever reads an entry knows which it wants.
 *
 *	**The numbers are fixed.** The windows, panels, parts and menus of the
 *	programs being brought over are drawn from these entries, and a
 *	program that asks for 472 wants the knob of a scroll bar. Renumbering
 *	them would mean rewriting every definition that arrives, and a
 *	definition that arrives is data, not code: it cannot be rewritten.
 *
 *	The ranges are:
 *
 *	    250..319    lengths and times
 *	    400..499    patterns: grounds, edges, bands, lamps, knobs
 *	    500..549    where things were left
 *	    550..579    colours
 *	    580..599    fonts
 *	    9000..9199  settings
 */

#ifndef __TS_LOOK_H__
#define __TS_LOOK_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ lengths */

#define LK_GROUND	250	/* the ground the windows stand on */
#define LK_MENUNAMES	240	/* what renames a menu's items */
#define LK_TITLE_H	300	/* how tall a window's name is */
#define LK_MENU_H	301	/* how tall one row of a menu is */
#define LK_BAR_W	302	/* how wide a scroll bar is */
#define LK_DRAG_W	303	/* how far a press moves before it is a drag */
#define LK_DBLTIME	304	/* the time between the presses of a double press */
#define LK_BLINK	306	/* how fast a caret blinks */
#define LK_CARET_W	307	/* the caret's stem */
#define LK_MARCH	308	/* how fast a chosen edge marches */
#define LK_SEL_W	309	/* what a selection's frame width is multiplied by */
#define LK_PART_RAD	310	/* the corner of a part */

/* ------------------------------------------------------------ patterns */

#define LK_GROUND_1ST	400	/* the grounds a work area may be cleared with */
#define LK_GROUND_LAST	499

#define LK_LIGHT	451	/* the lit edge of a raised thing */
#define LK_SHADOW	452	/* its shaded edge */
#define LK_MSGGREY	453
#define LK_ACTFRAME	454	/* the band of the window with the input */
#define LK_INACTFRAME	455	/* of one without it, and of a bar that cannot be worked */
#define LK_SUBFRAME	456	/* of one that belongs to another */
#define LK_TOPFRAME	457	/* of one kept in front */
#define LK_PNLGROUND	401	/* the ground of a panel */
#define LK_MENUGROUND	458	/* the ground of a menu's list */
#define LK_MENUFRAME	459	/* and its frame */
#define LK_SUBMGROUND	460	/* the same for a list opened from a list */
#define LK_SUBMFRAME	461
#define LK_ACTPARTS	462	/* the ground of a part that can be worked */
#define LK_INACTPARTS	463	/* and of one that cannot */
#define LK_VOLKNOB	464	/* the knob of a volume */
#define LK_VOLBACK	465	/* what it slides along */
#define LK_LAMPON_IN	466	/* the inside of a lamp that is lit */
#define LK_LAMPON_LT	467	/* and its edge */
#define LK_LAMPOFF_IN	468	/* the inside of one that is out */
#define LK_LAMPOFF_LT	469	/* and its edge */
#define LK_PEMPHAS	470	/* the ring round the part that answers */
#define LK_VOLTOMBO	471	/* the mark in a volume's knob */
#define LK_SBARKNOB	472	/* the knob of a scroll bar */
#define LK_SBARBACK	473	/* the track it runs along */
#define LK_SBARTOMBO	474	/* the mark across the knob's middle */
#define LK_MARKON_IN	475	/* a menu row's mark, set: inside */
#define LK_MARKON_LT	476	/* and edge */
#define LK_MARKOFF_IN	477	/* not set */
#define LK_MARKOFF_LT	478
#define LK_PNLFRAME	479	/* the frame round a plain panel */
#define LK_POPFRAME	480	/* the band of a pop-up */
#define LK_MSGWHITE	481
#define LK_WRNFRAME	482	/* round a panel that warns */
#define LK_FATFRAME	483	/* round one that cannot be carried on from */

/*
 * The line round a window and round its work area. The system this
 * comes from draws it in black and keeps no entry for it; one is kept
 * here so that a scheme can say otherwise, and 450 is free there.
 */
#define LK_OUTLINE	450

/* ------------------------------------------------------------ colours */

#define LK_ACTWTFCOL	550	/* the name of the window with the input */
#define LK_ACTWTBCOL	551	/* and the stroke under it */
#define LK_INWTFCOL	552	/* the name of one without it */
#define LK_INWTBCOL	553
#define LK_ACTMODECOL	554	/* the same two while it is being worked */
#define LK_ACTMODEBCOL	555
#define LK_INMODECOL	556
#define LK_INMODEBCOL	557
#define LK_MENUCHCOL	558	/* the words of a menu */
#define LK_MENUINACT	559	/* and of a row that cannot be chosen */
#define LK_MENUKEYCOL	560	/* a row's key letters */
#define LK_MENUKEYINACT	561
#define LK_MSGCOL	562
#define LK_ACTPARTSCOL	563	/* the words on a part that can be worked */
#define LK_INACTPARTSCOL 564	/* and on one that cannot */
#define LK_LIGHTPARTSCOL 565

/* ------------------------------------------------------------ fonts */

#define LK_PARTSFONT	582
#define LK_MSGFONT	583
#define LK_PNLFONT	584
#define LK_WNDFONT	585

/* ------------------------------------------------------------ settings */

#define LK_LOOK		9100	/* on or off */
#define LK_OPACITY	9101	/* how solid a frame is, 0..255 */
#define LK_WASH		9102	/* a colour washed over the frame */
#define LK_WASH_STR	9103	/* and how far towards it, 0..255 */
#define LK_ROUND	9104	/* how far the corners are cut */
#define LK_SHD_DX	9105
#define LK_SHD_DY	9106
#define LK_SHD_SIZE	9107	/* how far the shadow reaches past the window */
#define LK_SHD_DARK	9108	/* how dark it is, 0..255 */
#define LK_BAR_WASH	9109	/* a colour washed over the title band */
#define LK_BAR_STR	9110
#define LK_MOTION	9111	/* how long an opening takes, in ms */
#define LK_BLUR		9112	/* how far what shows through is blurred */
#define LK_FROM		9113	/* bit 0: the frame's colours come from the scheme */
#define LK_LIVE		9114	/* a window dragged is moved as it goes */

/*
 * What a person sets for themselves (ユーザ環境設定, design 16.5.23).
 * Times are in milliseconds. The window layer, the keyboard and the
 * pointer read these as they go, so a value set takes at once; the
 * user information object keeps them from one start to the next.
 */

/* the keyboard */
#define LK_KEY_ON	9120	/* how long a key is held before it counts */
#define LK_KEY_OFF	9121	/* after a release, how long a press is not taken */
#define LK_KEY_SIM	9122	/* how long a shift may follow the key it goes with */
#define LK_KRP		9123	/* keys repeat: 1 */
#define LK_KRP_START	9124	/* before the first repeat */
#define LK_KRP_INT	9125	/* between repeats */
#define LK_SCLK		9126	/* between the presses of a shift that make it one-shot; 0 never */
#define LK_TSHIFT	9127	/* 1: each press of a shift goes 通常, 一時シフト, 簡易ロック */

/* the pointer (LK_DBLTIME and LK_DRAG_W are its too) */
#define LK_PD_ON	9130	/* how long a button is held before it counts */
#define LK_PD_OFF	9131	/* after a release, how long a press is not taken */
#define LK_PD_SPEED	9132	/* 1 (the most sensitive) .. 15 */
#define LK_PD_ACCEL	9133	/* where speeding up starts: 0 (soonest) .. 7 (never) */
#define LK_PD_KEYSPD	9134	/* the pointer keys: 1 (fastest) .. 15 */
#define LK_PD_MAIN	9135	/* the main button: 0 left, 1 right */
#define LK_PD_ABS	9136	/* a pen or tablet: 1 absolute, 0 relative */
#define LK_PD_MIDDBL	9137	/* the middle button is a double press: 1 */
#define LK_PD_WHEEL	9138	/* the wheel scrolls: 1 */
#define LK_DBL_W	9139	/* how far apart the presses of a double press may be */

/* the screen (LK_TITLE_H, LK_MENU_H, LK_BAR_W, LK_BLINK, LK_MARCH, LK_CARET_W, LK_SEL_W too) */
#define LK_MENU_DLY	9140	/* before a list opened from a row opens */
#define LK_PTR_SIZE	9141	/* the pointer: 0 small, 1 middle, 2 large */
#define LK_SCR_LIVE	9142	/* the page follows a scroll bar's knob as it is dragged: 1 */

/*
 * sound: whether keys and buttons click, and the speaker. The sounds
 * themselves are objects of the sound box (SYSDEF_SND_*, beep.c).
 */
#define LK_CLK_KEY	9154	/* a key clicks: 1 */
#define LK_CLK_BTN	9155	/* a button clicks: 1 */
#define LK_SPEAKER	9156	/* 0 silences everything */

/* keys */
#define LK_EDITKEYS	9160	/* the edit menu's keys: 0 BTRON, 1 Windows */
#define LK_KANA		9161	/* かな入力: 1, ローマ字入力: 0 */

/* What the machine's settings say of the keyboard (システム環境設定) */
#define LK_KBD_US	9170	/* an English keyboard: 1; a Japanese one: 0 */
#define LK_KBD_MODE	9171	/* the input mode the desktop starts in: 0 かな, 1 英数 */

#ifdef __cplusplus
}
#endif

#endif /* __TS_LOOK_H__ */
