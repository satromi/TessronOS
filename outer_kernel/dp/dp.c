/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dp.c
 *	Drawing (design 16.4).
 *
 *	Every call goes the same way: turn what the caller said into screen
 *	coordinates, cut it down to what the environment may touch, do the
 *	work in the back buffer, and say what changed. Nothing writes to
 *	the screen here and nothing reads from it.
 *
 *	Two rectangles do the cutting. The frame is set by whoever owns the
 *	window and says which part of the screen is that window's at all;
 *	the visible region is the program's own and can only narrow it
 *	further. Keeping them apart is what lets a window manager reshape a
 *	window without touching what the program inside it decided.
 *
 *	The whole of the screen is one resource, so one lock covers it.
 *	Calls are short, and the expensive part of drawing is not here but
 *	in the sending, which happens outside this file.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/dp.h>
#include <ts/disp.h>

#define DP_MAX_ENV	1088		/* environments open at once: one for each window, and the desktop's own */

typedef struct {
	BOOL		used;
	T_DPPOINT	origin;
	T_DPRECT	frame;		/* screen coordinates */
	T_DPRECT	visible;	/* from the origin */
	UW		fore, back;
	UINT		mode;
	UB		*target;	/* NULL: the back buffer */
	UINT		pitch;
	T_DPPOINT	corner;	/* where the target sits on the screen */
	T_DPRGN		*rgn;		/* the shape drawn through, this one's own */
} DPENV;

LOCAL DPENV		dp_env[DP_MAX_ENV];
LOCAL BOOL		dp_ready = FALSE;
LOCAL ID		dp_mtxid = 0;
LOCAL T_DPSTAT		dp_count;
LOCAL T_DISPSPEC	dp_spec;

/* ---------------------------------------------------------------- patterns */

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

/* A pattern is laid from the origin, so the place asked for is measured
 * from there; two fills of neighbouring rectangles then meet with no
 * seam. FALSE where the pattern's mask leaves the pixel alone. */
LOCAL BOOL pat_at( CONST T_DPPAT *pat, INT x, INT y, UW *out )
{
	INT	tx, ty, stride;
	UW	bit;

	if ( pat->kind == DP_PAT_TILE && pat->tile != NULL
	  && pat->hs > 0 && pat->vs > 0 ) {
		tx = x % pat->hs;  if ( tx < 0 ) tx += pat->hs;
		ty = y % pat->vs;  if ( ty < 0 ) ty += pat->vs;
		if ( pat->mask != NULL ) {
			stride = (pat->hs + 31) / 32;
			bit = (pat->mask[ty * stride + tx / 32] >> (31 - (tx & 31))) & 1U;
			if ( bit == 0 ) {
				return FALSE;	/* the mask keeps this one out */
			}
		}
		*out = pat->tile[ty * pat->hs + tx];
		return TRUE;
	}
	bit = (dp_tone_rows[pat->tone & (DP_TONE_MAX - 1)][y & 3]
	       >> (31 - (x & 31))) & 1U;
	*out = bit ? pat->fore : pat->back;

	return TRUE;
}

/* A run of pixels, cut to everything, defined with the curves below */
LOCAL void span( DPENV *e, CONST T_DPPAT *pat, CONST T_DPRECT *clip,
		 INT x, INT y, INT n, INT ox, INT oy, T_DPRECT *touched );

/* ---------------------------------------------------------------- helpers */

LOCAL DPENV *env_of( INT gid )
{
	if ( gid < 1 || gid > DP_MAX_ENV ) {
		return NULL;
	}
	if ( !dp_env[gid - 1].used ) {
		return NULL;
	}

	return &dp_env[gid - 1];
}

/*
 * What this environment may write to, as a rectangle on the screen: the
 * frame, and the visible region brought into screen coordinates, cut
 * against each other and against the screen itself.
 */
LOCAL BOOL env_clip( CONST DPENV *e, T_DPRECT *out )
{
	T_DPRECT	v;

	v.left   = e->visible.left   + e->origin.x;
	v.top    = e->visible.top    + e->origin.y;
	v.right  = e->visible.right  + e->origin.x;
	v.bottom = e->visible.bottom + e->origin.y;

	out->left   = ( e->frame.left   > v.left )   ? e->frame.left   : v.left;
	out->top    = ( e->frame.top    > v.top )    ? e->frame.top    : v.top;
	out->right  = ( e->frame.right  < v.right )  ? e->frame.right  : v.right;
	out->bottom = ( e->frame.bottom < v.bottom ) ? e->frame.bottom : v.bottom;

	if ( out->left < 0 ) {
		out->left = 0;
	}
	if ( out->top < 0 ) {
		out->top = 0;
	}
	if ( out->right > (INT)dp_spec.width ) {
		out->right = (INT)dp_spec.width;
	}
	if ( out->bottom > (INT)dp_spec.height ) {
		out->bottom = (INT)dp_spec.height;
	}

	return ( out->left < out->right && out->top < out->bottom );
}

/* Cut one rectangle against another; FALSE when nothing is left */
LOCAL BOOL rect_cut( T_DPRECT *r, CONST T_DPRECT *by )
{
	if ( by->left   > r->left )   r->left   = by->left;
	if ( by->top    > r->top )    r->top    = by->top;
	if ( by->right  < r->right )  r->right  = by->right;
	if ( by->bottom < r->bottom ) r->bottom = by->bottom;

	return ( r->left < r->right && r->top < r->bottom );
}

/*
 * Where a screen coordinate lands in whatever this environment draws
 * into. The offset comes from the target's own corner.
 */
LOCAL UW *pixel_at( CONST DPENV *e, INT x, INT y )
{
	UB	*base;
	UINT	pitch;

	if ( e->target != NULL ) {
		base = e->target;
		pitch = e->pitch;
		x -= e->corner.x;
		y -= e->corner.y;
	} else {
		base = (UB *)ts_disp_buffer();
		pitch = dp_spec.pitch;
	}

	return (UW *)(base + (UBINT)y * pitch + (UBINT)x * 4);
}

/* Put one pixel down the way the environment says to meet what is there */
LOCAL void blend( UW *p, UW colour, UINT mode )
{
	switch ( mode ) {
	case DP_MODE_XOR:	*p ^= colour;	break;
	case DP_MODE_AND:	*p &= colour;	break;
	default:		*p  = colour;	break;
	}
}

/*
 * Tell the screen that a rectangle of the back buffer changed. An
 * environment drawing into something else says nothing: those pixels
 * are not on the screen yet, and whoever owns them decides when they
 * get there.
 */
LOCAL void damaged( CONST DPENV *e, CONST T_DPRECT *r )
{
	T_DISPRECT	d;

	if ( e->target != NULL ) {
		return;
	}
	d.left = r->left;
	d.top = r->top;
	d.right = r->right;
	d.bottom = r->bottom;
	ts_disp_damage(&d);
}

/* ---------------------------------------------------------------- environments */

EXPORT INT dp_open( void )
{
	INT	i;

	if ( !dp_ready ) {
		return E_NOEXS;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);

	for ( i = 0; i < DP_MAX_ENV; i++ ) {
		if ( !dp_env[i].used ) {
			break;
		}
	}
	if ( i == DP_MAX_ENV ) {
		tk_unl_mtx(dp_mtxid);
		return E_LIMIT;
	}
	knl_memset(&dp_env[i], 0, sizeof(dp_env[i]));
	dp_env[i].used = TRUE;

	/* until told otherwise it may draw anywhere on the screen */
	dp_env[i].frame.right  = (INT)dp_spec.width;
	dp_env[i].frame.bottom = (INT)dp_spec.height;
	dp_env[i].visible.right  = (INT)dp_spec.width;
	dp_env[i].visible.bottom = (INT)dp_spec.height;
	dp_env[i].fore = 0x00FFFFFF;
	dp_env[i].back = 0x00000000;
	dp_env[i].mode = DP_MODE_COPY;

	tk_unl_mtx(dp_mtxid);

	return i + 1;
}

EXPORT ER dp_close( INT gid )
{
	DPENV	*e;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		dp_rgn_free(e->rgn);
	e->rgn = NULL;
	e->used = FALSE;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_origin( INT gid, INT x, INT y )
{
	DPENV	*e;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->origin.x = x;
		e->origin.y = y;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_frame( INT gid, CONST T_DPRECT *r )
{
	DPENV	*e;

	if ( r == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->frame = *r;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_visible( INT gid, CONST T_DPRECT *r )
{
	DPENV	*e;

	if ( r == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->visible = *r;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_target( INT gid, void *pixels, UINT pitch,
			 INT corner_x, INT corner_y )
{
	DPENV	*e;

	if ( pixels != NULL && pitch == 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->target = (UB *)pixels;
		e->pitch = pitch;
		e->corner.x = ( pixels != NULL ) ? corner_x : 0;
		e->corner.y = ( pixels != NULL ) ? corner_y : 0;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_place( INT gid, void *pixels, UINT pitch, INT corner_x, INT corner_y,
			CONST T_DPRECT *work )
{
	DPENV	*e;

	if ( work == NULL || ( pixels != NULL && pitch == 0 ) ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->target = (UB *)pixels;
		e->pitch = pitch;
		e->corner.x = ( pixels != NULL ) ? corner_x : 0;
		e->corner.y = ( pixels != NULL ) ? corner_y : 0;
		e->origin.x = work->left;
		e->origin.y = work->top;
		e->frame = *work;
		e->visible.left = 0;
		e->visible.top = 0;
		e->visible.right = work->right - work->left;
		e->visible.bottom = work->bottom - work->top;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_region( INT gid, CONST T_DPRGN *rgn )
{
	DPENV	*e;
	T_DPRGN	*copy = NULL;
	ER	er = E_OK;

	if ( rgn != NULL ) {
		er = dp_rgn_copy(rgn, &copy);
		if ( er < E_OK ) {
			return er;
		}
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		dp_rgn_free(copy);
		return E_ID;
	}
	dp_rgn_free(e->rgn);
	e->rgn = copy;
	tk_unl_mtx(dp_mtxid);

	return E_OK;
}

EXPORT ER dp_set_colour( INT gid, UW fore, UW back )
{
	DPENV	*e;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->fore = fore;
		e->back = back;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_set_mode( INT gid, UINT mode )
{
	DPENV	*e;

	if ( mode > DP_MODE_AND ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->mode = mode;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT ER dp_ref( INT gid, T_DPENV *env )
{
	DPENV	*e;

	if ( env == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		env->origin  = e->origin;
		env->frame   = e->frame;
		env->visible = e->visible;
		env->fore    = e->fore;
		env->back    = e->back;
		env->mode    = e->mode;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

/* ---------------------------------------------------------------- drawing */

EXPORT ER dp_put_pixel( INT gid, INT x, INT y, UW colour )
{
	DPENV		*e;
	T_DPRECT	clip, r;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	x += e->origin.x;
	y += e->origin.y;

	if ( !env_clip(e, &clip) || x < clip.left || x >= clip.right
	  || y < clip.top || y >= clip.bottom ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;			/* outside: nothing to do */
	}
	blend(pixel_at(e, x, y), colour, e->mode);
	dp_count.calls++;
	dp_count.pixels++;
	tk_unl_mtx(dp_mtxid);

	r.left = x; r.top = y; r.right = x + 1; r.bottom = y + 1;
	damaged(e, &r);

	return E_OK;
}

EXPORT INT dp_get_pixel( INT gid, INT x, INT y )
{
	DPENV	*e;
	INT	v;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	x += e->origin.x;
	y += e->origin.y;
	if ( x < 0 || x >= (INT)dp_spec.width
	  || y < 0 || y >= (INT)dp_spec.height ) {
		tk_unl_mtx(dp_mtxid);
		return E_PAR;
	}
	v = (INT)(*pixel_at(e, x, y) & 0x00FFFFFF);
	tk_unl_mtx(dp_mtxid);

	return v;
}

/*
 * One run of pixels on one row, in a pattern: the smallest thing the
 * layer puts down, and what every shape drawn elsewhere comes down to.
 * The coordinates are the caller's, as everywhere else.
 */
EXPORT ER dp_fill_run( INT gid, INT x0, INT x1, INT y, CONST T_DPPAT *pat )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	INT		ox, oy;

	if ( pat == NULL || x1 <= x0 ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = x1 + ox;  touched.right = x0 + ox;
	touched.top = y + oy + 1;  touched.bottom = y + oy;
	span(e, pat, &clip, x0 + ox, y + oy, x1 - x0, ox, oy, &touched);
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

/*
 * A bitmap of one bit per pixel put down in one colour: a letter.
 *
 * This exists because a letter is a hundred or so pixels and a page is
 * thousands of letters, and going through the layer once per pixel
 * means taking the lock and working out the address a million times for
 * one page. Here the lock is taken once and the rows are walked as
 * runs.
 */
/*
 * A row of pixels that carry their own colours, cut to the clip and to
 * the region, and laid through the same blending as everything else.
 * 'src' points at the pixel that belongs at 'x'.
 */
LOCAL void run_px( DPENV *e, CONST UW *src, INT sx, INT x, INT x1, INT y,
		   UW clear, T_DPRECT *touched )
{
	UW	*p;
	INT	x0 = x;

	if ( x1 <= x ) {
		return;
	}
	p = pixel_at(e, x, y);
	for ( ; x < x1; x++, p++ ) {
		UW	c = src[x - sx];

		if ( c != clear ) {
			blend(p, c, e->mode);
		}
	}
	dp_count.pixels += (UD)(x1 - x0);
	if ( x0 < touched->left )      touched->left = x0;
	if ( x1 > touched->right )     touched->right = x1;
	if ( y < touched->top )        touched->top = y;
	if ( y + 1 > touched->bottom ) touched->bottom = y + 1;
}

LOCAL void span_px( DPENV *e, CONST UW *src, CONST T_DPRECT *clip,
		    INT x, INT y, INT n, INT ox, INT oy, UW clear,
		    T_DPRECT *touched )
{
	INT	x1 = x + n, sx = x;

	if ( n <= 0 || y < clip->top || y >= clip->bottom ) {
		return;
	}
	if ( x < clip->left )    x = clip->left;
	if ( x1 > clip->right )  x1 = clip->right;
	if ( x >= x1 ) {
		return;
	}
	if ( e->rgn != NULL ) {
		CONST INT	*sp = NULL;
		INT		ns = dp_rgn_row(e->rgn, y - oy, &sp);
		INT		i;

		for ( i = 0; i < ns; i++ ) {
			INT	a = sp[i * 2] + ox;
			INT	b = sp[i * 2 + 1] + ox;

			if ( a < x )  a = x;
			if ( b > x1 ) b = x1;
			run_px(e, src, sx, a, b, y, clear, touched);
		}
		return;
	}
	run_px(e, src, sx, x, x1, y, clear, touched);
}

/*
 * A picture of true-colour pixels laid down at a place. 'pitch' is in
 * pixels, not bytes, because that is how a picture is held. A pixel
 * equal to 'clear' is not laid, which is what gives a picture a shape
 * other than its own rectangle; a 'clear' no pixel can equal lays all
 * of it.
 *
 * It goes through the clip and the region a row at a time. Laying a
 * picture pixel by pixel through dp_put_pixel would cut each pixel
 * against the region by itself, which costs more in the cutting than
 * in the pixels.
 */
/*
 * A rectangle of pixels read back into the caller's own memory.
 *
 * This is how anything that covers part of the screen for a while --
 * a menu, a dragged outline, a panel -- puts back what was under it
 * without asking the program that owns the window to draw itself
 * again. The program may be busy, or waiting for the very menu that
 * is covering it.
 *
 * What is read is what is in the environment's target, cut to the
 * clip: pixels outside it are left as they were in the buffer given,
 * so a caller that clears its buffer first can tell them apart.
 */
EXPORT ER dp_get_argb( INT gid, INT x, INT y, UW *pixels, INT pitch,
		       INT w, INT h )
{
	DPENV		*e;
	T_DPRECT	clip;
	INT		row, col, ox, oy;

	if ( pixels == NULL || w <= 0 || h <= 0 || pitch < w ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	if ( !env_clip(e, &clip) ) {
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	for ( row = 0; row < h; row++ ) {
		INT	sy = y + oy + row;

		if ( sy < clip.top || sy >= clip.bottom ) {
			continue;
		}
		for ( col = 0; col < w; col++ ) {
			INT	sx = x + ox + col;

			if ( sx < clip.left || sx >= clip.right ) {
				continue;
			}
			pixels[(SZ)row * pitch + col] = *pixel_at(e, sx, sy);
		}
	}
	tk_unl_mtx(dp_mtxid);

	return E_OK;
}

EXPORT ER dp_put_argb( INT gid, INT x, INT y, CONST UW *pixels, INT pitch,
		       INT w, INT h, UW clear )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	INT		row, ox, oy;

	if ( pixels == NULL || w <= 0 || h <= 0 || pitch < w ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = x + ox + w;  touched.right = x + ox;
	touched.top = y + oy + h;   touched.bottom = y + oy;

	for ( row = 0; row < h; row++ ) {
		span_px(e, pixels + (SZ)row * pitch, &clip,
			x + ox, y + oy + row, w, ox, oy, clear, &touched);
	}
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

EXPORT ER dp_put_mono( INT gid, INT x, INT y, CONST UB *bits, INT pitch,
		       INT w, INT h, UW colour )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	T_DPPAT		pat;
	INT		row, col, ox, oy;

	if ( bits == NULL || w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	dp_pat_colour(&pat, colour);
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = x + ox + w;  touched.right = x + ox;
	touched.top = y + oy + h;   touched.bottom = y + oy;

	for ( row = 0; row < h; row++ ) {
		CONST UB	*line = bits + (SZ)row * pitch;
		INT		run0 = -1;

		for ( col = 0; col <= w; col++ ) {
			BOOL	on = FALSE;

			if ( col < w ) {
				on = ( ( line[col >> 3] >> (7 - (col & 7)) ) & 1 ) != 0;
			}
			if ( on ) {
				if ( run0 < 0 ) {
					run0 = col;
				}
			} else if ( run0 >= 0 ) {
				span(e, &pat, &clip, x + ox + run0, y + oy + row,
				     col - run0, ox, oy, &touched);
				run0 = -1;
			}
		}
	}
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

/*
 * A bitmap of how much of each pixel a shape covers (0 none .. 255 all),
 * put down in one colour: each pixel is the colour and what was there,
 * mixed in that share. Letters drawn with their edges smoothed come this
 * way. The pixel's top byte is kept as it was.
 */
LOCAL void gray_run( DPENV *e, CONST UB *cov, INT sx, INT x, INT x1, INT y,
		     UW colour, T_DPRECT *touched )
{
	UW	*p;
	INT	x0 = x;
	UW	cr = ( colour >> 16 ) & 0xFF, cg = ( colour >> 8 ) & 0xFF, cb = colour & 0xFF;

	if ( x1 <= x ) {
		return;
	}
	p = pixel_at(e, x, y);
	for ( ; x < x1; x++, p++ ) {
		UW	a = cov[x - sx], d, r, g, b;

		if ( a == 0 ) continue;
		d = *p;
		if ( a >= 255 ) {
			*p = ( d & 0xFF000000U ) | ( colour & 0x00FFFFFFU );
			continue;
		}
		r = ( cr * a + ( ( d >> 16 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
		g = ( cg * a + ( ( d >> 8 ) & 0xFF ) * ( 255 - a ) + 127 ) / 255;
		b = ( cb * a + ( d & 0xFF ) * ( 255 - a ) + 127 ) / 255;
		*p = ( d & 0xFF000000U ) | ( r << 16 ) | ( g << 8 ) | b;
	}
	dp_count.pixels += (UD)(x1 - x0);
	if ( x0 < touched->left )      touched->left = x0;
	if ( x1 > touched->right )     touched->right = x1;
	if ( y < touched->top )        touched->top = y;
	if ( y + 1 > touched->bottom ) touched->bottom = y + 1;
}

EXPORT ER dp_put_gray( INT gid, INT x, INT y, CONST UB *cov, INT pitch,
		       INT w, INT h, UW colour )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	INT		row, ox, oy;

	if ( cov == NULL || w <= 0 || h <= 0 || pitch < w ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = x + ox + w;  touched.right = x + ox;
	touched.top = y + oy + h;   touched.bottom = y + oy;

	for ( row = 0; row < h; row++ ) {
		CONST UB	*line = cov + (SZ)row * pitch;
		INT		sy = y + oy + row, sx = x + ox;
		INT		a0 = sx, a1 = sx + w;

		if ( sy < clip.top || sy >= clip.bottom ) continue;
		if ( a0 < clip.left )  a0 = clip.left;
		if ( a1 > clip.right ) a1 = clip.right;
		if ( a0 >= a1 ) continue;
		if ( e->rgn != NULL ) {
			CONST INT	*sp = NULL;
			INT		ns = dp_rgn_row(e->rgn, sy - oy, &sp);
			INT		i;

			for ( i = 0; i < ns; i++ ) {
				INT	a = sp[i * 2] + ox;
				INT	b = sp[i * 2 + 1] + ox;

				if ( a < a0 ) a = a0;
				if ( b > a1 ) b = a1;
				gray_run(e, line, sx, a, b, sy, colour, &touched);
			}
		} else {
			gray_run(e, line, sx, a0, a1, sy, colour, &touched);
		}
	}
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

/* What this environment may touch, in the coordinates the caller draws
 * in: what a shape that walks pixel by pixel needs so as not to walk
 * the whole screen. */
EXPORT ER dp_clip_rect( INT gid, T_DPRECT *out )
{
	DPENV		*e;
	T_DPRECT	clip;
	BOOL		any;

	if ( out == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	any = env_clip(e, &clip);
	out->left   = clip.left - e->origin.x;
	out->top    = clip.top - e->origin.y;
	out->right  = clip.right - e->origin.x;
	out->bottom = clip.bottom - e->origin.y;
	tk_unl_mtx(dp_mtxid);

	return any ? E_OK : E_NOEXS;
}

EXPORT ER dp_fill_rect_pat( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat )
{
	DPENV		*e;
	T_DPRECT	clip, a;
	INT		y, ox, oy;

	if ( r == NULL || pat == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	a.left   = r->left   + ox;
	a.top    = r->top    + oy;
	a.right  = r->right  + ox;
	a.bottom = r->bottom + oy;

	if ( !env_clip(e, &clip) || !rect_cut(&a, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	{
		T_DPRECT	touched;

		touched.left = a.right;  touched.top = a.bottom;
		touched.right = a.left;  touched.bottom = a.top;
		for ( y = a.top; y < a.bottom; y++ ) {
			span(e, pat, &a, a.left, y, a.right - a.left,
			     ox, oy, &touched);
		}
		dp_count.calls++;
		tk_unl_mtx(dp_mtxid);

		if ( touched.right > touched.left
		  && touched.bottom > touched.top ) {
			damaged(e, &touched);
		}
	}

	return E_OK;
}

EXPORT ER dp_fill_rect( INT gid, CONST T_DPRECT *r, UW colour )
{
	T_DPPAT	pat;

	dp_pat_colour(&pat, colour);

	return dp_fill_rect_pat(gid, r, &pat);
}

EXPORT ER dp_frame_rect_pat( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat,
			     INT width )
{
	T_DPRECT	side;
	ER		er;

	if ( r == NULL || pat == NULL || width <= 0 ) {
		return E_PAR;
	}
	if ( r->right - r->left <= 0 || r->bottom - r->top <= 0 ) {
		return E_PAR;
	}
	/* the four sides, drawn as rectangles so the corners are covered once */
	er = dp_hold();
	if ( er < E_OK ) {
		return er;
	}
	side = *r;  side.bottom = r->top + width;
	dp_fill_rect_pat(gid, &side, pat);
	side = *r;  side.top = r->bottom - width;
	dp_fill_rect_pat(gid, &side, pat);
	side = *r;  side.top = r->top + width;  side.bottom = r->bottom - width;
	side.right = r->left + width;
	dp_fill_rect_pat(gid, &side, pat);
	side.left = r->right - width;  side.right = r->right;
	dp_fill_rect_pat(gid, &side, pat);

	return dp_release();
}

EXPORT ER dp_frame_rect( INT gid, CONST T_DPRECT *r, UW colour, INT width )
{
	T_DPPAT	pat;

	dp_pat_colour(&pat, colour);

	return dp_frame_rect_pat(gid, r, &pat, width);
}

/*
 * A straight line, stepping along whichever axis it covers more of and
 * carrying the error of the other. Both ends are drawn.
 */
EXPORT ER dp_line_pat( INT gid, INT x0, INT y0, INT x1, INT y1,
		       CONST T_DPPAT *pat )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	INT		dx, dy, sx, sy, err, e2, ox, oy;
	BOOL		any = FALSE;

	if ( pat == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	x0 += ox;  y0 += oy;
	x1 += ox;  y1 += oy;

	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = x0;  touched.right = x0 + 1;
	touched.top = y0;   touched.bottom = y0 + 1;

	dx = ( x1 > x0 ) ? x1 - x0 : x0 - x1;
	dy = ( y1 > y0 ) ? y1 - y0 : y0 - y1;
	sx = ( x0 < x1 ) ? 1 : -1;
	sy = ( y0 < y1 ) ? 1 : -1;
	err = dx - dy;

	for (;;) {
		if ( x0 >= clip.left && x0 < clip.right
		  && y0 >= clip.top && y0 < clip.bottom
		  && ( e->rgn == NULL
		    || dp_rgn_has(e->rgn, x0 - ox, y0 - oy) ) ) {
			UW	c;

			if ( pat_at(pat, x0 - ox, y0 - oy, &c) ) {
				blend(pixel_at(e, x0, y0), c, e->mode);
			}
			dp_count.pixels++;
			any = TRUE;
			if ( x0 < touched.left )       touched.left = x0;
			if ( x0 + 1 > touched.right )  touched.right = x0 + 1;
			if ( y0 < touched.top )        touched.top = y0;
			if ( y0 + 1 > touched.bottom ) touched.bottom = y0 + 1;
		}
		if ( x0 == x1 && y0 == y1 ) {
			break;
		}
		e2 = err * 2;
		if ( e2 > -dy ) {
			err -= dy;
			x0 += sx;
		}
		if ( e2 < dx ) {
			err += dx;
			y0 += sy;
		}
	}
	if ( any ) {
		dp_count.calls++;
	} else {
		dp_count.clipped++;
	}
	tk_unl_mtx(dp_mtxid);

	if ( any ) {
		damaged(e, &touched);
	}

	return E_OK;
}

EXPORT ER dp_line( INT gid, INT x0, INT y0, INT x1, INT y1, UW colour )
{
	T_DPPAT	pat;

	dp_pat_colour(&pat, colour);

	return dp_line_pat(gid, x0, y0, x1, y1, &pat);
}

/*
 * Move pixels within the back buffer. The rows are copied from whichever
 * end keeps the source ahead of the destination, so a rectangle sliding
 * over itself does not eat its own tail.
 */
/* ------------------------------------------------- ovals and rounded boxes */

/*
 * A level oval is a rounded box whose corner radii reach past its
 * sides, so one routine draws both. They are drawn row by row from the
 * vertical middle outward with a midpoint ellipse tracker: the decision
 * variable is kept in the weighted form 4(Bx^2 + Ay^2 - AB) with
 * A = a^2 and B = b^2, so a step sideways costs 4B and a step down 4A.
 *
 * A frame tracks a second, smaller ellipse for the hole. The hole's
 * tracker takes its steep-region steps one row early, which is what
 * keeps the ring from thinning where the curve turns.
 */
typedef struct {
	D	X, Y, d;		/* 4Bx, 4Ay, and the decision */
	D	c6A, c6A4B, c6B, c4A6B, cBmA, c4B, c4A;
} OVAL;

/* An ellipse with half axes a and b, starting at (a, 0). A circle is
 * tracked with unit weights, which keeps the numbers small. */
LOCAL void oval_init( OVAL *o, INT a, INT b, BOOL unit )
{
	D	A = unit ? 1 : (D)a * a;
	D	B = unit ? 1 : (D)b * b;

	o->X = 4 * (D)a * B;
	o->Y = 0;
	o->d = (1 - 2 * (D)a) * B + 2 * A;
	o->c6A   = 6 * A;
	o->c6A4B = 6 * A + 4 * B;
	o->c6B   = 6 * B;
	o->c4A6B = 4 * A + 6 * B;
	o->cBmA  = B - A;
	o->c4B   = 4 * B;
	o->c4A   = 4 * A;
}

/* One row outward on the outer ellipse: a step sideways moves the left
 * edge right and the right edge left. */
LOCAL void oval_step( OVAL *o, INT *xl, INT *xr )
{
	if ( o->Y < o->X ) {
		if ( o->d < 0 ) {
			o->d += o->Y + o->c6A;
		} else {
			o->d += o->Y - o->X + o->c6A4B;
			o->X -= o->c4B;
			(*xl)++;
			(*xr)--;
		}
		o->Y += o->c4A;
		if ( o->Y >= o->X ) {
			o->d -= (o->X + o->Y) >> 1;
			o->d += o->cBmA;
		}
	} else if ( o->X > 0 ) {
		(*xl)++;
		(*xr)--;
		if ( o->d >= 0 ) {
			do {
				o->d -= o->X;
				o->X -= o->c4B;
				(*xl)++;
				(*xr)--;
				o->d += o->c6B;
			} while ( o->d >= 0 && o->X > 0 );
		}
		o->d += o->Y - o->X + o->c4A6B;
		o->X -= o->c4B;
		o->Y += o->c4A;
	}
}

/* The hole's steep-region steps, taken as soon as they are due. */
LOCAL void hole_drain( OVAL *h, INT *xl, INT *xr )
{
	do {
		(*xl)++;
		(*xr)--;
		h->d -= h->X;
		h->X -= h->c4B;
		h->d += h->c6B;
		if ( h->d < 0 ) {
			break;
		}
	} while ( h->X > 0 );
}

/* One row outward on the hole. */
LOCAL void hole_step( OVAL *h, INT *xl, INT *xr )
{
	if ( h->Y < h->X ) {
		if ( h->d < 0 ) {
			h->d += h->Y + h->c6A;
		} else {
			h->d += h->Y - h->X + h->c6A4B;
			h->X -= h->c4B;
			(*xl)++;
			(*xr)--;
		}
		h->Y += h->c4A;
		if ( h->Y >= h->X ) {
			h->d -= (h->X + h->Y) >> 1;
			h->d += h->cBmA;
			if ( h->d >= 0 && h->X > 0 ) {
				hole_drain(h, xl, xr);
			}
		}
	} else if ( h->X > 0 ) {
		(*xl)++;
		(*xr)--;
		h->d += h->Y - h->X + h->c4A6B;
		h->X -= h->c4B;
		h->Y += h->c4A;
		if ( h->d >= 0 && h->X > 0 ) {
			hole_drain(h, xl, xr);
		}
	}
}

/*
 * A run of pixels at (x, y), n of them, cut to what the environment may
 * touch and put down in the pattern. Every curve here is drawn as runs,
 * so this is the one place that writes.
 */
/* One run of pixels, already cut to everything: this is the only place
 * in the drawing layer that writes. */
LOCAL void run( DPENV *e, CONST T_DPPAT *pat, INT x, INT x1, INT y,
		INT ox, INT oy, T_DPRECT *touched )
{
	UW	*p;
	INT	x0 = x;

	if ( x1 <= x ) {
		return;
	}
	p = pixel_at(e, x, y);
	for ( ; x < x1; x++, p++ ) {
		UW	c;

		if ( pat_at(pat, x - ox, y - oy, &c) ) {
			blend(p, c, e->mode);
		}
	}
	dp_count.pixels += (UD)(x1 - x0);
	if ( x0 < touched->left )      touched->left = x0;
	if ( x1 > touched->right )     touched->right = x1;
	if ( y < touched->top )        touched->top = y;
	if ( y + 1 > touched->bottom ) touched->bottom = y + 1;
}

/*
 * A run of pixels at (x, y), n of them, cut to what the environment may
 * touch and, where one is set, to the shape it draws through. Every
 * curve and every fill here is drawn as runs, so cutting once here
 * covers all of them.
 */
LOCAL void span( DPENV *e, CONST T_DPPAT *pat, CONST T_DPRECT *clip,
		 INT x, INT y, INT n, INT ox, INT oy, T_DPRECT *touched )
{
	INT	x1 = x + n;

	if ( n <= 0 || y < clip->top || y >= clip->bottom ) {
		return;
	}
	if ( x < clip->left )    x = clip->left;
	if ( x1 > clip->right )  x1 = clip->right;
	if ( x >= x1 ) {
		return;
	}
	if ( e->rgn != NULL ) {
		CONST INT	*sp = NULL;
		INT		ns = dp_rgn_row(e->rgn, y - oy, &sp);
		INT		i;

		for ( i = 0; i < ns; i++ ) {
			INT	a = sp[i * 2] + ox;
			INT	b = sp[i * 2 + 1] + ox;

			if ( a < x )  a = x;
			if ( b > x1 ) b = x1;
			run(e, pat, a, b, y, ox, oy, touched);
		}
		return;
	}
	run(e, pat, x, x1, y, ox, oy, touched);
}

LOCAL void block( DPENV *e, CONST T_DPPAT *pat, CONST T_DPRECT *clip,
		  INT x, INT y, INT w, INT h, INT ox, INT oy, T_DPRECT *touched )
{
	INT	i;

	for ( i = 0; i < h; i++ ) {
		span(e, pat, clip, x, y + i, w, ox, oy, touched);
	}
}

/* Both radii are lengths, so the smaller of the two is wanted */
LOCAL INT lmin( INT a, INT b )
{
	return ( a < b ) ? a : b;
}

/*
 * The rows of a rounded box: where each row starts and stops, measured
 * from the box's own corner. The fill below puts these down and the
 * region built from them is the same shape, which is what keeps a
 * window's corner and the shape it is laid down in from disagreeing by
 * a pixel.
 */
LOCAL void round_rows( INT w, INT h, INT rx, INT ry, INT *x0, INT *x1 )
{
	OVAL	o;
	INT	a, bh, cyt, cyb, x, xr, i;

	for ( i = 0; i < h; i++ ) {
		x0[i] = 0;
		x1[i] = 0;
	}
	a  = (lmin(w, rx) - 1) / 2;
	bh = (lmin(h, ry) - 1) / 2;
	cyt = bh;
	cyb = h - bh;
	x = 0;  xr = w - 1;
	for ( i = cyt; i < cyb; i++ ) {
		x0[i] = 0;
		x1[i] = w;
	}
	if ( bh <= 0 ) {
		return;
	}
	oval_init(&o, a, bh, (BOOL)(a == bh));
	while ( o.Y < o.X ) {
		if ( o.d < 0 ) {
			o.d += o.Y + o.c6A;
		} else {
			o.d += o.Y - o.X + o.c6A4B;
			o.X -= o.c4B;
			x++;
			xr--;
		}
		cyt--;
		if ( cyt >= 0 && cyt < h ) {
			x0[cyt] = x;  x1[cyt] = xr + 1;
		}
		if ( cyb >= 0 && cyb < h ) {
			x0[cyb] = x;  x1[cyb] = xr + 1;
		}
		cyb++;
		o.Y += o.c4A;
	}
	if ( cyt <= 0 ) {
		return;
	}
	o.d -= (o.X + o.Y) >> 1;
	o.d += o.cBmA;
	do {
		INT	nt = cyt - 1, nb = cyb + 1;

		if ( o.d >= 0 && o.X > 0 ) {
			do {
				o.d -= o.X;
				o.X -= o.c4B;
				x++;
				xr--;
				o.d += o.c6B;
			} while ( o.d >= 0 && o.X > 0 );
		}
		if ( o.X > 0 ) {
			x++;
			xr--;
			o.d += o.Y - o.X + o.c4A6B;
			o.X -= o.c4B;
			o.Y += o.c4A;
		}
		if ( nt >= 0 && nt < h ) {
			x0[nt] = x;  x1[nt] = xr + 1;
		}
		if ( cyb >= 0 && cyb < h ) {
			x0[cyb] = x;  x1[cyb] = xr + 1;
		}
		cyt = nt;
		cyb = nb;
	} while ( cyt > 0 );
}

/*
 * The region of a rounded box, in the coordinates the rectangle is
 * given in. This is how a window whose corners are round is laid down:
 * one shape worked out when its size changes, rather than a test at
 * every pixel of every screen.
 */
EXPORT ER dp_rgn_round( CONST T_DPRECT *r, INT rx, INT ry, T_DPRGN **p_out )
{
	INT	w, h, i, *x0, *x1;
	ER	er;

	if ( r == NULL || p_out == NULL || rx < 0 || ry < 0 ) {
		return E_PAR;
	}
	w = r->right - r->left;
	h = r->bottom - r->top;
	if ( w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	x0 = (INT *)Kmalloc((SZ)h * 2 * sizeof(INT));
	if ( x0 == NULL ) {
		return E_NOMEM;
	}
	x1 = x0 + h;
	round_rows(w, h, rx, ry, x0, x1);
	for ( i = 0; i < h; i++ ) {
		if ( x1[i] > x0[i] ) {
			x0[i] += r->left;
			x1[i] += r->left;
		}
	}
	er = dp_rgn_from_rows(r->top, h, x0, x1, p_out);
	Kfree(x0);

	return er;
}

/*
 * A filled rounded box: the middle band as one block, then rows outward
 * on both sides, each as wide as the ellipse says.
 */
EXPORT ER dp_fill_round( INT gid, CONST T_DPRECT *rect, INT rx, INT ry,
			 CONST T_DPPAT *pat )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	OVAL		o;
	INT		l, t, r, b, a, bh, cyt, cyb, x, xr, ox, oy;

	if ( rect == NULL || pat == NULL || rx < 0 || ry < 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	l = rect->left + ox;    t = rect->top + oy;
	r = rect->right + ox;   b = rect->bottom + oy;
	if ( l >= r || t >= b || !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return ( l >= r || t >= b ) ? E_PAR : E_OK;
	}
	touched.left = r;  touched.top = b;
	touched.right = l; touched.bottom = t;

	a  = (lmin(r - l, rx) - 1) / 2;
	bh = (lmin(b - t, ry) - 1) / 2;
	cyt = t + bh;
	cyb = b - bh;
	x = l;  xr = r - 1;
	block(e, pat, &clip, l, cyt, r - l, cyb - cyt, ox, oy, &touched);
	if ( bh > 0 ) {
		oval_init(&o, a, bh, (BOOL)(a == bh));
		while ( o.Y < o.X ) {
			if ( o.d < 0 ) {
				o.d += o.Y + o.c6A;
			} else {
				o.d += o.Y - o.X + o.c6A4B;
				o.X -= o.c4B;
				x++;
				xr--;
			}
			span(e, pat, &clip, x, --cyt, xr - x + 1, ox, oy, &touched);
			span(e, pat, &clip, x, cyb++, xr - x + 1, ox, oy, &touched);
			o.Y += o.c4A;
		}
		if ( cyt > t ) {
			o.d -= (o.X + o.Y) >> 1;
			o.d += o.cBmA;
			do {
				INT	nt = cyt - 1, nb = cyb + 1;

				if ( o.d >= 0 && o.X > 0 ) {
					do {
						o.d -= o.X;
						o.X -= o.c4B;
						x++;
						xr--;
						o.d += o.c6B;
					} while ( o.d >= 0 && o.X > 0 );
				}
				if ( o.X > 0 ) {
					x++;
					xr--;
					o.d += o.Y - o.X + o.c4A6B;
					o.X -= o.c4B;
					o.Y += o.c4A;
				}
				span(e, pat, &clip, x, nt, xr - x + 1, ox, oy, &touched);
				span(e, pat, &clip, x, cyb, xr - x + 1, ox, oy, &touched);
				cyt = nt;
				cyb = nb;
			} while ( cyt > t );
		}
	}
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

/*
 * The outline of a rounded box, w wide: two upright bars over the middle
 * band, then rows outward where the ring is the outer ellipse less the
 * hole.
 */
EXPORT ER dp_frame_round( INT gid, CONST T_DPRECT *rect, INT rx, INT ry,
			  INT width, CONST T_DPPAT *pat )
{
	DPENV		*e;
	T_DPRECT	clip, touched;
	OVAL		o, h;
	INT		l, t, r, b, a, bh, ia, ib, cyt, cyb, tw, il, ir;
	INT		x, xr, ox, oy;
	BOOL		unit;

	if ( rect == NULL || pat == NULL || width <= 0 || rx < 0 || ry < 0 ) {
		return E_PAR;
	}
	if ( rect->right - rect->left <= 2 * width
	  || rect->bottom - rect->top <= 2 * width ) {
		return dp_fill_round(gid, rect, rx, ry, pat);
	}
	if ( rx == 0 || ry == 0 ) {
		return dp_frame_rect_pat(gid, rect, pat, width);
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	ox = e->origin.x;
	oy = e->origin.y;
	l = rect->left + ox;    t = rect->top + oy;
	r = rect->right + ox;   b = rect->bottom + oy;
	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	touched.left = r;  touched.top = b;
	touched.right = l; touched.bottom = t;

	a  = (lmin(r - l, rx) - 1) >> 1;
	bh = (lmin(b - t, ry) - 1) >> 1;
	cyt = t + bh;
	cyb = b - bh;
	ia = a - (width - 1);
	ib = bh - (width - 1);
	tw = t + width;
	il = l + width;
	ir = r - width;
	x = l;  xr = r - 1;
	if ( ib <= 0 ) {
		il = ir;
		cyt = t + width;
		cyb = b - width;
	}
	block(e, pat, &clip, l, cyt, width, cyb - cyt, ox, oy, &touched);
	block(e, pat, &clip, r - width, cyt, width, cyb - cyt, ox, oy, &touched);
	while ( ib <= 0 ) {
		span(e, pat, &clip, l, --cyt, r - l, ox, oy, &touched);
		span(e, pat, &clip, l, cyb++, r - l, ox, oy, &touched);
		ib++;
	}
	if ( bh > 0 ) {
		unit = (BOOL)(a == bh);
		oval_init(&o, a, bh, unit);
		oval_init(&h, ia, ib, unit);
		do {
			INT	nt = cyt - 1, nb = cyb + 1;

			oval_step(&o, &x, &xr);
			if ( cyt > tw ) {
				hole_step(&h, &il, &ir);
			} else if ( cyt == tw ) {
				il = ir;
			}
			cyt = nt;
			if ( ir > il ) {
				INT	side = ((xr - x + 1) - (ir - il)) >> 1;

				span(e, pat, &clip, x, cyt, side, ox, oy, &touched);
				span(e, pat, &clip, ir, cyt, side, ox, oy, &touched);
				span(e, pat, &clip, ir, cyb, side, ox, oy, &touched);
				span(e, pat, &clip, x, cyb, side, ox, oy, &touched);
			} else {
				span(e, pat, &clip, x, cyt, xr - x + 1, ox, oy, &touched);
				span(e, pat, &clip, x, cyb, xr - x + 1, ox, oy, &touched);
			}
			cyb = nb;
		} while ( cyt > t );
	}
	dp_count.calls++;
	tk_unl_mtx(dp_mtxid);

	if ( touched.right > touched.left && touched.bottom > touched.top ) {
		damaged(e, &touched);
	}

	return E_OK;
}

/*
 * An oval is the rounded box whose corner radii reach past its sides,
 * so both fall out of the two above.
 */
EXPORT ER dp_fill_oval( INT gid, CONST T_DPRECT *r, CONST T_DPPAT *pat )
{
	return dp_fill_round(gid, r, DP_ROUND_FULL, DP_ROUND_FULL, pat);
}

EXPORT ER dp_frame_oval( INT gid, CONST T_DPRECT *r, INT width,
			 CONST T_DPPAT *pat )
{
	return dp_frame_round(gid, r, DP_ROUND_FULL, DP_ROUND_FULL, width, pat);
}

EXPORT ER dp_copy_rect( INT gid, CONST T_DPRECT *src, INT to_x, INT to_y )
{
	DPENV		*e;
	T_DPRECT	clip, s, d;
	INT		w, h, y, step, first;

	if ( src == NULL ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	s.left   = src->left   + e->origin.x;
	s.top    = src->top    + e->origin.y;
	s.right  = src->right  + e->origin.x;
	s.bottom = src->bottom + e->origin.y;
	d.left = to_x + e->origin.x;
	d.top  = to_y + e->origin.y;

	if ( !env_clip(e, &clip) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	/*
	 * Both ends are cut, and by the same amount, so that the pixels
	 * that do move keep their places relative to one another.
	 */
	if ( s.left < clip.left ) {
		d.left += clip.left - s.left;
		s.left = clip.left;
	}
	if ( s.top < clip.top ) {
		d.top += clip.top - s.top;
		s.top = clip.top;
	}
	if ( s.right > clip.right ) {
		s.right = clip.right;
	}
	if ( s.bottom > clip.bottom ) {
		s.bottom = clip.bottom;
	}
	w = s.right - s.left;
	h = s.bottom - s.top;
	if ( d.left < clip.left ) {
		w -= clip.left - d.left;
		s.left += clip.left - d.left;
		d.left = clip.left;
	}
	if ( d.top < clip.top ) {
		h -= clip.top - d.top;
		s.top += clip.top - d.top;
		d.top = clip.top;
	}
	if ( d.left + w > clip.right ) {
		w = clip.right - d.left;
	}
	if ( d.top + h > clip.bottom ) {
		h = clip.bottom - d.top;
	}
	if ( w <= 0 || h <= 0 ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}

	if ( d.top > s.top ) {
		first = h - 1;
		step = -1;
	} else {
		first = 0;
		step = 1;
	}
	for ( y = 0; y < h; y++ ) {
		INT	row = first + y * step;
		UW	*sp = pixel_at(e, s.left, s.top + row);
		UW	*dp = pixel_at(e, d.left, d.top + row);
		INT	x;

		if ( d.left > s.left ) {
			for ( x = w - 1; x >= 0; x-- ) {
				dp[x] = sp[x];
			}
		} else {
			for ( x = 0; x < w; x++ ) {
				dp[x] = sp[x];
			}
		}
	}
	dp_count.calls++;
	dp_count.pixels += (UD)w * (UD)h;
	tk_unl_mtx(dp_mtxid);

	d.right = d.left + w;
	d.bottom = d.top + h;
	damaged(e, &d);

	return E_OK;
}

EXPORT ER dp_hold( void )
{
	return ts_disp_hold();
}

EXPORT ER dp_release( void )
{
	return ts_disp_release();
}

EXPORT ER dp_stat( T_DPSTAT *st )
{
	if ( !dp_ready || st == NULL ) {
		return E_NOEXS;
	}
	*st = dp_count;

	return E_OK;
}

/* ---------------------------------------------------------------- start-up */

EXPORT INT knl_dp_init( void )
{
	T_CMTX	cmtx;

	if ( dp_ready ) {
		return 1;
	}
	if ( ts_disp_ref(&dp_spec) < E_OK ) {
		return 0;			/* no screen to draw on */
	}
	if ( dp_spec.bpp != 32 ) {
		return E_NOSPT;			/* only four bytes a pixel so far */
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	dp_mtxid = tk_cre_mtx(&cmtx);
	if ( dp_mtxid <= 0 ) {
		return E_LIMIT;
	}
	dp_ready = TRUE;

	return 1;
}
