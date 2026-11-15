/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	edid.h
 *	What a display's EDID says (edid.c)
 */

#ifndef __TS_EDID_H__
#define __TS_EDID_H__

typedef struct {
	char	maker[4];		/* three letters */
	char	model[16];		/* the display's name, "" when it gives none */
	UH	product;
	UW	serial;
	UW	ident;			/* maker, product and serial in one number */
	UH	mm_w, mm_h;		/* its size, mm; 0 not known */
	INT	blocks;			/* blocks of 128 bytes read: the base and its extensions */
	BOOL	hdmi;			/* a CTA extension with HDMI's identifier */
	BOOL	audio;			/* it plays sound */
} T_EDIDINFO;

/*
 * The EDID of len bytes read into info, and its modes into modes, up to
 * max: how many, or E_PAR when it is not an EDID (its header or the base
 * block's sum wrong)
 */
IMPORT INT knl_edid_parse( CONST UB *e, INT len, T_EDIDINFO *info, T_DISPMODE *modes, INT max );

#endif /* __TS_EDID_H__ */
