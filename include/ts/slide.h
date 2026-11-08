/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	slide.h
 *	スライド: what a document of slides says and how a slide is fitted
 *	to a screen (design 11.11), for the program of the slides and the
 *	kernel's tests (application/slide/sl_deck.c)
 */

#ifndef __TS_SLIDE_H__
#define __TS_SLIDE_H__

#include <ts/tad.h>
#include <ts/tadview.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_W		1920		/* the screen slides are made for, unless they say */
#define SL_H		1080
#define SL_ZOOM_MAX	4		/* times life size */

/*
 * The slides of a document: what its links point at, in the order they
 * stand, at most max of them; *p_w by *p_h the screen they are made for
 * ("%size:W:H" as the first paragraph, else SL_W by SL_H). Answers how
 * many.
 */
IMPORT INT  sl_pages( CONST T_TAD *rec, TS_UUID *ids, INT max, INT *p_w, INT *p_h );

/* Whether a record holds a <realtime> (it is played as it is shown) */
IMPORT BOOL sl_has_rt( CONST T_TAD *rec );

/* Whether a record is shown as slides: it links to some, or it is a program of its own */
IMPORT BOOL sl_takes( CONST T_TAD *rec );

/* The scene a record of 実時間制御TAD is made for: its figure's view, or SL_W by SL_H */
IMPORT void sl_scene_size( CONST T_TAD *rec, INT *p_w, INT *p_h );

/*
 * The magnification z / *p_den that makes a slide made for tw by th as
 * large as a window of ww by wh lets it be with its shape kept, and the
 * rectangle it takes in the middle of the window; answers z
 */
IMPORT INT  sl_fit( INT ww, INT wh, INT tw, INT th, T_DPRECT *out, INT *p_den );

/* A slide's model: its document or its figure (the other NULL); FALSE when it has neither */
IMPORT BOOL sl_model( CONST T_TAD *rec, T_TVDOC **p_d, T_TVFIG **p_f );

/* An element of that name */
IMPORT BOOL sl_is_elem( CONST T_TADNODE *nd, CONST char *name );

#ifdef __cplusplus
}
#endif

#endif /* __TS_SLIDE_H__ */
