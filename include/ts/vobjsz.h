/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vobjsz.h
 *	The size of a virtual object from the size of its letters (design 11.3)
 *
 *	One rule for every program that writes or lays out a link, the
 *	record layer (tad_closed_h) and the programs that read records of
 *	their own alike. Only arithmetic, so a program running as a
 *	process uses it without the record layer.
 *
 *	c is the letters' size (chsz, in pixels as in points; 14 when a
 *	record does not say), frame and pict whether the link shows its
 *	frame and its picture:
 *
 *	  closed height  with a frame          c + c/4 + 2
 *	                 no frame, a picture   c + c/4
 *	                 neither               c
 *	  least width    with a frame          2c
 *	                 no frame, a picture   c + c/4
 *	                 neither               c
 *
 *	A link that shows none of the picture, the name, the relationship,
 *	the kind and the date has no band; the closed height does not
 *	apply to it (tad_closed_h answers 0).
 */

#ifndef __TS_VOBJSZ_H__
#define __TS_VOBJSZ_H__

#define VOBJ_CHSZ_DEF		14

#define VOBJ_CH(c)		( ( (c) > 0 ) ? (c) : VOBJ_CHSZ_DEF )

#define VOBJ_CLOSED_H(c, frame, pict) \
	( ( frame ) ? VOBJ_CH(c) + VOBJ_CH(c) / 4 + 2 \
	  : ( pict ) ? VOBJ_CH(c) + VOBJ_CH(c) / 4 : VOBJ_CH(c) )

#define VOBJ_LEAST_W(c, frame, pict) \
	( ( frame ) ? VOBJ_CH(c) * 2 \
	  : ( pict ) ? VOBJ_CH(c) + VOBJ_CH(c) / 4 : VOBJ_CH(c) )

/*
 * The furniture across a closed box with a frame and the picture, as
 * it is drawn: half a letter of edge, the picture's square and half a
 * letter after it before the name, half a letter after the name.
 */
#define VOBJ_CLOSED_EDGE_W(c)	( ( VOBJ_CH(c) / 2 ) * 3 + VOBJ_CH(c) )

/* How far a box may be taller than closed and still be closed */
#define VOBJ_OPEN_SLACK		8

#endif /* __TS_VOBJSZ_H__ */
