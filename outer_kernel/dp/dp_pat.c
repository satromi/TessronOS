/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dp_pat.c
 *	The patterns a drawing call puts down: a plain colour and a tone of
 *	two (design 16.5)
 *
 *	These only fill in a pattern, so the same code serves the kernel and
 *	a program running as a process that draws through the system calls
 *	(include/ts/tvport.h).
 */

#include <ts/tvport.h>
#include <ts/dp.h>

/*
 * The eight dithers. Four rows of thirty-two columns, the leftmost
 * column in the top bit; a bit set means the first colour. Tone 1 is
 * all of the second colour and tone 7 all of the first, with the
 * quarters, halves and three-quarters between them. The same rows serve
 * as the standard masks of tile patterns.
 */
EXPORT CONST UW dp_tone_rows[DP_TONE_MAX][4] = {
	{ 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 0x88888888U, 0, 0x22222222U, 0 },
	{ 0xAAAAAAAAU, 0, 0x55555555U, 0 },
	{ 0xAAAAAAAAU, 0x55555555U, 0xAAAAAAAAU, 0x55555555U },
	{ 0x55555555U, 0xFFFFFFFFU, 0xAAAAAAAAU, 0xFFFFFFFFU },
	{ 0x77777777U, 0xFFFFFFFFU, 0xDDDDDDDDU, 0xFFFFFFFFU },
	{ 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU },
};

EXPORT void dp_pat_colour( T_DPPAT *pat, UW colour )
{
	if ( pat == NULL ) {
		return;
	}
	pat->kind = DP_PAT_TONE;
	pat->tone = 7;				/* all of the first colour */
	pat->fore = colour;
	pat->back = colour;
	pat->hs = pat->vs = 0;
	pat->tile = NULL;
	pat->mask = NULL;
}

EXPORT void dp_pat_tone( T_DPPAT *pat, INT tone, UW fore, UW back )
{
	if ( pat == NULL ) {
		return;
	}
	if ( tone < 0 ) tone = 0;
	if ( tone >= DP_TONE_MAX ) tone = DP_TONE_MAX - 1;
	pat->kind = DP_PAT_TONE;
	pat->tone = tone;
	pat->fore = fore;
	pat->back = back;
	pat->hs = pat->vs = 0;
	pat->tile = NULL;
	pat->mask = NULL;
}

