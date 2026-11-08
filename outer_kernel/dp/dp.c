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

#define DP_ENV_FIRST	64		/* environments the table starts with; it doubles as more open */

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
	BOOL		watch;		/* what it touches is told (dp_watch) */
	CONST T_DPREC	*rec;		/* drawing kept as drawing (dp_open_rec), or NULL */
	void		*rec_arg;
	UINT		scale;		/* percent its target's pixels are to its coordinates (dp_set_scale) */
} DPENV;

/*
 * An environment drawn at a scale other than 100 (dp_set_scale) is one
 * whose coordinates are view pixels and whose target holds the pixels
 * of the screen: view pixel x of the screen lies at pixel
 * floor((x - corner) * scale / 100) of the target, and runs to where
 * the next begins. Only an environment with a target of its own is
 * drawn so; one drawing on the back buffer itself is drawn as it is.
 */
#define SCALED(e)	( (e)->scale != 100 && (e)->target != NULL )

/*
 * The environments open, one for each window and the desktop's own: a
 * table of pointers that doubles when it is full. Each environment is
 * allocated once and never freed, and a table is put in place whole
 * with one write of its pointer; the one it replaces is not freed, so
 * one looked up without the lock stays where it is.
 */
typedef struct {
	INT	n;
	DPENV	*e[];
} DPTBL;

LOCAL DPTBL		*dp_tbl = NULL;
LOCAL BOOL		dp_ready = FALSE;
LOCAL ID		dp_mtxid = 0;
LOCAL T_DPSTAT		dp_count;
LOCAL T_DISPSPEC	dp_spec;

/* ---------------------------------------------------------------- patterns */

/* The dithers and the patterns of one colour and of a tone (dp_pat_*) are in dp_pat.c */

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
	DPTBL	*t = dp_tbl;

	if ( t == NULL || gid < 1 || gid > t->n ) {
		return NULL;
	}
	if ( !t->e[gid - 1]->used ) {
		return NULL;
	}

	return t->e[gid - 1];
}

/* Twice as many environments, with the lock held: FALSE for no memory */
LOCAL BOOL env_grow( void )
{
	DPTBL	*t = dp_tbl, *nt;
	INT	n = ( t != NULL ) ? t->n * 2 : DP_ENV_FIRST, i;

	nt = (DPTBL *)Kmalloc((SZ)( sizeof(DPTBL) + sizeof(DPENV *) * (SZ)n ));
	if ( nt == NULL ) {
		return FALSE;
	}
	nt->n = n;
	for ( i = 0; i < n; i++ ) {
		if ( t != NULL && i < t->n ) {
			nt->e[i] = t->e[i];
			continue;
		}
		nt->e[i] = (DPENV *)Kmalloc((SZ)sizeof(DPENV));
		if ( nt->e[i] == NULL ) {
			while ( --i >= ( ( t != NULL ) ? t->n : 0 ) ) Kfree(nt->e[i]);
			Kfree(nt);
			return FALSE;
		}
		knl_memset(nt->e[i], 0, sizeof(DPENV));
	}
	__sync_synchronize();
	dp_tbl = nt;
	return TRUE;
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

	/*
	 * The screen bounds what is drawn into the screen. Pixels of an
	 * owner's own (a window's, a page's) are bounded by the frame the
	 * owner set, which may be larger than the screen: a sheet of A3 is
	 * taller than most screens.
	 */
	if ( e->target == NULL && e->rec == NULL ) {
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
	}

	return ( out->left < out->right && out->top < out->bottom );
}

/* ---------------------------------------------------------------- recording */

/*
 * What a call through an environment that records needs: whom to hand
 * it to, the origin, and what may be touched as one rectangle. FALSE
 * for an environment that draws pixels, which goes on the usual way.
 */
typedef struct {
	CONST T_DPREC	*ops;
	void		*arg;
	T_DPRECST	st;
	BOOL		copy;		/* drawn plainly: anything else is not kept */
} RECAT;

LOCAL BOOL rect_cut( T_DPRECT *r, CONST T_DPRECT *by );

LOCAL BOOL rec_at( INT gid, RECAT *ra )
{
	DPENV	*e;
	BOOL	r = FALSE;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL && e->rec != NULL ) {
		ra->ops = e->rec;
		ra->arg = e->rec_arg;
		ra->st.ox = e->origin.x;
		ra->st.oy = e->origin.y;
		ra->copy = (BOOL)( e->mode == DP_MODE_COPY );
		if ( !env_clip(e, &ra->st.clip) ) {
			ra->copy = FALSE;	/* nothing may be touched */
		} else if ( e->rgn != NULL ) {
			T_DPRECT	b;

			if ( dp_rgn_bound(e->rgn, &b) >= E_OK ) {
				b.left += e->origin.x;
				b.right += e->origin.x;
				b.top += e->origin.y;
				b.bottom += e->origin.y;
				if ( !rect_cut(&ra->st.clip, &b) ) {
					ra->copy = FALSE;
				}
			}
		}
		r = TRUE;
	}
	tk_unl_mtx(dp_mtxid);

	return r;
}

LOCAL void rec_op( T_DPRECOP *op, UINT kind )
{
	knl_memset(op, 0, sizeof(*op));
	op->kind = kind;
}

/*
 * A shape handed on when the environment records: TRUE when it did,
 * and the caller is done. Also what dp_shape.c asks before walking a
 * shape into runs of pixels.
 */
EXPORT BOOL knl_dp_rec_draw( INT gid, CONST T_DPRECOP *op, CONST T_DPPAT *pat )
{
	RECAT	ra;

	if ( !rec_at(gid, &ra) ) {
		return FALSE;
	}
	if ( ra.copy && ra.ops->draw != NULL ) {
		ra.ops->draw(ra.arg, &ra.st, op, pat);
	}
	return TRUE;
}

LOCAL BOOL rec_image( INT gid, CONST T_DPRECIMG *im )
{
	RECAT	ra;

	if ( !rec_at(gid, &ra) ) {
		return FALSE;
	}
	if ( ra.copy && ra.ops->image != NULL ) {
		ra.ops->image(ra.arg, &ra.st, im);
	}
	return TRUE;
}

EXPORT BOOL dp_rec_glyph( INT gid, CONST T_DPRECGLY *g )
{
	RECAT	ra;

	if ( g == NULL || !rec_at(gid, &ra) ) {
		return FALSE;
	}
	if ( ra.copy && ra.ops->glyph != NULL ) {
		ra.ops->glyph(ra.arg, &ra.st, g);
	}
	return TRUE;
}

EXPORT BOOL dp_is_rec( INT gid )
{
	RECAT	ra;

	return rec_at(gid, &ra);
}

EXPORT INT dp_open_rec( CONST T_DPREC *ops, void *arg, INT w, INT h )
{
	INT	gid;
	DPENV	*e;

	if ( ops == NULL || w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	gid = dp_open();
	if ( gid < 0 ) {
		return gid;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->rec = ops;
		e->rec_arg = arg;
		e->frame.left = 0;
		e->frame.top = 0;
		e->frame.right = w;
		e->frame.bottom = h;
		e->visible = e->frame;
	}
	tk_unl_mtx(dp_mtxid);

	return gid;
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

/* ---------------------------------------------------------------- drawn at a scale */

/* Where view coordinate x or y of the screen lies in a scaled target, in its pixels */
LOCAL INT dvx( CONST DPENV *e, INT x )
{
	return disp_dev_of(x - e->corner.x, e->scale);
}

LOCAL INT dvy( CONST DPENV *e, INT y )
{
	return disp_dev_of(y - e->corner.y, e->scale);
}

/* The view coordinate of the screen that pixel X or Y of a scaled target belongs to */
LOCAL INT vwx( CONST DPENV *e, INT X )
{
	return e->corner.x + disp_view_of(X, e->scale);
}

LOCAL INT vwy( CONST DPENV *e, INT Y )
{
	return e->corner.y + disp_view_of(Y, e->scale);
}

/* A pixel of a scaled target, by its own place */
LOCAL UW *dev_at( CONST DPENV *e, INT X, INT Y )
{
	return (UW *)( e->target + (UBINT)Y * e->pitch + (UBINT)X * 4 );
}

/* The pixels of a scaled target that view pixel (x, y) covers, put down in one colour */
LOCAL void dev_block( CONST DPENV *e, INT x, INT y, UW colour )
{
	INT	X0 = dvx(e, x), X1 = dvx(e, x + 1), Y = dvy(e, y), Y1 = dvy(e, y + 1), X;

	for ( ; Y < Y1; Y++ ) {
		UW	*p = dev_at(e, X0, Y);

		for ( X = X0; X < X1; X++, p++ ) {
			blend(p, colour, e->mode);
		}
	}
}

/* A view rectangle of the screen as the pixels of a scaled target */
LOCAL void dev_rect( CONST DPENV *e, CONST T_DPRECT *r, T_DPRECT *d )
{
	d->left = dvx(e, r->left);
	d->top = dvy(e, r->top);
	d->right = dvx(e, r->right);
	d->bottom = dvy(e, r->bottom);
}

/*
 * Tell the screen that a rectangle of the back buffer changed. An
 * environment drawing into something else says nothing: those pixels
 * are not on the screen yet, and whoever owns them decides when they
 * get there.
 */
LOCAL void	(*dp_watch_fn)( CONST T_DPRECT *r ) = NULL;

LOCAL void damaged( CONST DPENV *e, CONST T_DPRECT *r )
{
	T_DISPRECT	d;
	void		(*fn)( CONST T_DPRECT *r );

	if ( e->target != NULL ) {
		fn = dp_watch_fn;
		if ( e->watch && fn != NULL ) {
			fn(r);			/* a window's pixels: built onto the screen later */
		}
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
	DPENV	*e;
	INT	i;

	if ( !dp_ready ) {
		return E_NOEXS;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);

	for ( i = 0; dp_tbl != NULL && i < dp_tbl->n; i++ ) {
		if ( !dp_tbl->e[i]->used ) {
			break;
		}
	}
	if ( ( dp_tbl == NULL || i == dp_tbl->n ) && !env_grow() ) {
		tk_unl_mtx(dp_mtxid);
		return E_NOMEM;
	}
	e = dp_tbl->e[i];
	knl_memset(e, 0, sizeof(*e));
	e->used = TRUE;

	/* until told otherwise it may draw anywhere on the screen */
	e->frame.right  = (INT)dp_spec.width;
	e->frame.bottom = (INT)dp_spec.height;
	e->visible.right  = (INT)dp_spec.width;
	e->visible.bottom = (INT)dp_spec.height;
	e->fore = 0x00FFFFFF;
	e->back = 0x00000000;
	e->mode = DP_MODE_COPY;
	e->scale = 100;

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

EXPORT INT dp_open_mem( CONST T_DPBMP *bmp )
{
	T_DPRECT	all;
	INT		gid;

	if ( bmp == NULL || bmp->px == NULL || bmp->w <= 0 || bmp->h <= 0
	  || bmp->w > DP_MEM_MAX / bmp->h || bmp->pitch < bmp->w * 4 ) {
		return E_PAR;
	}
	gid = dp_open();
	if ( gid < 0 ) {
		return gid;
	}
	all.left = 0;  all.top = 0;  all.right = bmp->w;  all.bottom = bmp->h;
	(void)dp_set_target(gid, bmp->px, (UINT)bmp->pitch, 0, 0);
	(void)dp_set_origin(gid, 0, 0);
	(void)dp_set_frame(gid, &all);
	(void)dp_set_visible(gid, &all);

	return gid;
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

EXPORT ER dp_set_watch( INT gid, BOOL on )
{
	DPENV	*e;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->watch = on;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT void dp_watch( void (*fn)( CONST T_DPRECT *r ) )
{
	dp_watch_fn = fn;
}

EXPORT ER dp_set_scale( INT gid, UINT scale )
{
	DPENV	*e;

	if ( scale < 100 || scale > 400 ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e != NULL ) {
		e->scale = scale;
	}
	tk_unl_mtx(dp_mtxid);

	return ( e != NULL ) ? E_OK : E_ID;
}

EXPORT INT dp_scale( INT gid )
{
	DPENV	*e;
	INT	s;

	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	s = ( e == NULL ) ? E_ID : ( SCALED(e) ? (INT)e->scale : 100 );
	tk_unl_mtx(dp_mtxid);

	return s;
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
	T_DPRECOP	op;
	T_DPPAT		pat;

	rec_op(&op, DP_REC_RECT);
	op.r.left = x;
	op.r.top = y;
	op.r.right = x + 1;
	op.r.bottom = y + 1;
	dp_pat_colour(&pat, colour);
	if ( knl_dp_rec_draw(gid, &op, &pat) ) {
		return E_OK;
	}
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
	if ( SCALED(e) ) {
		dev_block(e, x, y, colour);
	} else {
		blend(pixel_at(e, x, y), colour, e->mode);
	}
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
	if ( e->rec != NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_PAR;			/* drawing kept as drawing has no pixels */
	}
	x += e->origin.x;
	y += e->origin.y;
	if ( x < 0 || x >= (INT)dp_spec.width
	  || y < 0 || y >= (INT)dp_spec.height ) {
		tk_unl_mtx(dp_mtxid);
		return E_PAR;
	}
	v = (INT)( *( SCALED(e) ? dev_at(e, dvx(e, x), dvy(e, y)) : pixel_at(e, x, y) ) & 0x00FFFFFF );
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
	{
		T_DPRECOP	op;

		rec_op(&op, DP_REC_RECT);
		op.r.left = x0;
		op.r.top = y;
		op.r.right = x1;
		op.r.bottom = y + 1;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
	if ( SCALED(e) ) {
		/* each pixel of the picture as large as a view pixel is */
		for ( ; x < x1; x++ ) {
			UW	c = src[x - sx];

			if ( c != clear ) {
				dev_block(e, x, y, c);
			}
		}
		x = x0;
		goto counted;
	}
	p = pixel_at(e, x, y);
	for ( ; x < x1; x++, p++ ) {
		UW	c = src[x - sx];

		if ( c != clear ) {
			blend(p, c, e->mode);
		}
	}
    counted:
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
	if ( e->rec != NULL || !env_clip(e, &clip) ) {
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
			pixels[(SZ)row * pitch + col] = SCALED(e) ? *dev_at(e, dvx(e, sx), dvy(e, sy))
							  : *pixel_at(e, sx, sy);
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
	T_DPRECIMG	im;

	if ( pixels == NULL || w <= 0 || h <= 0 || pitch < w ) {
		return E_PAR;
	}
	im.kind = DP_REC_ARGB;
	im.x = x;
	im.y = y;
	im.w = w;
	im.h = h;
	im.px = pixels;
	im.pitch = pitch;
	im.colour = clear;
	if ( rec_image(gid, &im) ) {
		return E_OK;
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
	{
		T_DPRECIMG	im;

		im.kind = DP_REC_MONO;
		im.x = x;
		im.y = y;
		im.w = w;
		im.h = h;
		im.px = bits;
		im.pitch = pitch;
		im.colour = colour;
		if ( rec_image(gid, &im) ) {
			return E_OK;
		}
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
 * Mixing in light, not in colour values.
 *
 * A value on the screen is not the light it gives: the light goes about
 * as the value to the power 2.2 (the sRGB curve). Mixing the values
 * themselves in proportion to how much of the pixel a letter covers
 * makes half a pixel of black on white far darker than half the light,
 * and half a pixel of white on black far dimmer -- the same letter comes
 * out heavy in one colour and thin in the other. So each of the three
 * values is turned into light by a table (dp_lin), the two lights mixed
 * in the share covered, and the result turned back into a value.
 *
 * The curve the mixing goes by is picked by how light the letter's own
 * colour is: the sRGB curve itself for black, a straighter one the
 * lighter the colour, a straight line (the values mixed as they are) for
 * white. Light letters on a dark ground look bolder than dark letters on
 * a light ground of the same shape, because a bright stroke spreads in
 * the eye; mixing them by a straighter curve takes back that spread, so
 * that a letter weighs the same whichever way round its colours are.
 *
 * Nothing pushes the shares themselves up or down (no contrast added):
 * the letters are made a little heavier in their outlines instead
 * (fn.c, the darkening), which keeps their edges as smooth as the
 * coverage says.
 *
 * Back from light to value: dp_inv gives, for each sixteenth of the light
 * scale, the value nearest its lower end, and dp_mid, the light halfway
 * between a value and the next, moves it on to the nearest. Both are
 * made from dp_lin the first time a curve is used.
 */
#include "dp_gamma.h"

LOCAL UB	dp_inv[DP_GAMMA_N][4096];
LOCAL UW	dp_mid[DP_GAMMA_N][256];
LOCAL BOOL	dp_gm_made[DP_GAMMA_N];

LOCAL void gamma_make( INT k )
{
	INT	v, b;

	for ( v = 0; v < 255; v++ ) {
		dp_mid[k][v] = ( (UW)dp_lin[k][v] + (UW)dp_lin[k][v + 1] + 1 ) / 2;
	}
	dp_mid[k][255] = 0x10000U;		/* past every light */
	v = 0;
	for ( b = 0; b < 4096; b++ ) {
		while ( v < 255 && (UW)( b << 4 ) >= dp_mid[k][v] ) {
			v++;
		}
		dp_inv[k][b] = (UB)v;
	}
	dp_gm_made[k] = TRUE;
}

/* The curve a colour's letters are mixed by: 0 straight .. DP_GAMMA_N - 1 sRGB */
LOCAL INT gamma_of( UW colour )
{
	UW	y = ( 54U * ( ( colour >> 16 ) & 0xFF ) + 183U * ( ( colour >> 8 ) & 0xFF )
		      + 19U * ( colour & 0xFF ) ) >> 8;

	return (INT)( ( ( 255U - y ) * (UW)( DP_GAMMA_N - 1 ) + 127U ) / 255U );
}

/* A light back to the nearest value */
Inline UW unlight( CONST UB *inv, CONST UW *mid, UW l )
{
	UW	v = inv[l >> 4];

	while ( l >= mid[v] ) {
		v++;
	}
	return v;
}

/* One value of what is there and the letter's light, mixed in share a of 255 */
Inline UW mix1( CONST UH *lin, CONST UB *inv, CONST UW *mid, UW d, INT lc, INT a )
{
	INT	ld = (INT)lin[d];

	if ( ld == lc ) {
		return d;
	}
	return unlight(inv, mid, (UW)( ld + ( ( lc - ld ) * a + ( ( lc > ld ) ? 127 : -127 ) ) / 255 ));
}

/* One pixel d with the letter's colour mixed into it in share a of 255, as gray_run mixes */
Inline UW gray_mix( UW d, UW a, UW colour, INT k, INT lr, INT lg, INT lb )
{
	UW	r, g, b;

	if ( a >= 255 ) {
		return ( d & 0xFF000000U ) | ( colour & 0x00FFFFFFU );
	}
	r = mix1(dp_lin[k], dp_inv[k], dp_mid[k], ( d >> 16 ) & 0xFF, lr, (INT)a);
	g = mix1(dp_lin[k], dp_inv[k], dp_mid[k], ( d >> 8 ) & 0xFF, lg, (INT)a);
	b = mix1(dp_lin[k], dp_inv[k], dp_mid[k], d & 0xFF, lb, (INT)a);
	return ( d & 0xFF000000U ) | ( r << 16 ) | ( g << 8 ) | b;
}

/*
 * A bitmap of how much of each pixel a shape covers (0 none .. 255 all),
 * put down in one colour: each pixel is the colour and what was there,
 * mixed in that share as light (above). Letters drawn with their edges
 * smoothed come this way. The pixel's top byte is kept as it was.
 */
LOCAL void gray_run( DPENV *e, CONST UB *cov, INT sx, INT x, INT x1, INT y,
		     UW colour, INT k, T_DPRECT *touched )
{
	UW		*p;
	INT		x0 = x;
	CONST UH	*lin = dp_lin[k];
	CONST UB	*inv = dp_inv[k];
	CONST UW	*mid = dp_mid[k];
	INT		lr = lin[( colour >> 16 ) & 0xFF];
	INT		lg = lin[( colour >> 8 ) & 0xFF];
	INT		lb = lin[colour & 0xFF];
	UW		last_d = 0, last_a = 0, last_out = 0;

	if ( x1 <= x ) {
		return;
	}
	if ( SCALED(e) ) {
		/* each share as large as a view pixel, mixed into each pixel under it */
		INT	Y0 = dvy(e, y), Y1 = dvy(e, y + 1), X, Y;

		for ( ; x < x1; x++ ) {
			UW	a = cov[x - sx];
			INT	X0 = dvx(e, x), X1 = dvx(e, x + 1);

			if ( a == 0 ) continue;
			for ( Y = Y0; Y < Y1; Y++ ) {
				for ( X = X0; X < X1; X++ ) {
					p = dev_at(e, X, Y);
					*p = gray_mix(*p, a, colour, k, lr, lg, lb);
				}
			}
		}
		x = x0;
		goto counted;
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
		if ( d == last_d && a == last_a ) {
			*p = last_out;		/* the same mix as the pixel before */
			continue;
		}
		r = mix1(lin, inv, mid, ( d >> 16 ) & 0xFF, lr, (INT)a);
		g = mix1(lin, inv, mid, ( d >> 8 ) & 0xFF, lg, (INT)a);
		b = mix1(lin, inv, mid, d & 0xFF, lb, (INT)a);
		last_d = d;
		last_a = a;
		last_out = ( d & 0xFF000000U ) | ( r << 16 ) | ( g << 8 ) | b;
		*p = last_out;
	}
    counted:
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
	INT		row, ox, oy, k = gamma_of(colour);

	if ( cov == NULL || w <= 0 || h <= 0 || pitch < w ) {
		return E_PAR;
	}
	{
		T_DPRECIMG	im;

		im.kind = DP_REC_GRAY;
		im.x = x;
		im.y = y;
		im.w = w;
		im.h = h;
		im.px = cov;
		im.pitch = pitch;
		im.colour = colour;
		if ( rec_image(gid, &im) ) {
			return E_OK;
		}
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
	if ( !dp_gm_made[k] ) {
		gamma_make(k);
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
				gray_run(e, line, sx, a, b, sy, colour, k, &touched);
			}
		} else {
			gray_run(e, line, sx, a0, a1, sy, colour, k, &touched);
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
	{
		T_DPRECOP	op;

		rec_op(&op, DP_REC_RECT);
		op.r = *r;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
	{
		T_DPRECOP	op;

		rec_op(&op, DP_REC_FRAME);
		op.r = *r;
		op.width = width;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
	{
		T_DPRECOP	op;
		T_DPPOINT	pt[2];

		rec_op(&op, DP_REC_LINE);
		pt[0].x = x0;
		pt[0].y = y0;
		pt[1].x = x1;
		pt[1].y = y1;
		op.p = pt;
		op.n = 2;
		op.width = 1;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
				if ( SCALED(e) ) {
					dev_block(e, x0, y0, c);
				} else {
					blend(pixel_at(e, x0, y0), c, e->mode);
				}
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
/*
 * A run of view pixels in a scaled target: each view pixel is the
 * rectangle of the target's pixels it covers, in the colour the pattern
 * gives it at its view place, so a pattern looks as it does unscaled. A
 * pattern of one colour is laid a row of the target at a time.
 */
LOCAL void run_scaled( DPENV *e, CONST T_DPPAT *pat, INT x, INT x1, INT y, INT ox, INT oy )
{
	INT	Y0 = dvy(e, y), Y1 = dvy(e, y + 1), Y, X, X1;
	UW	c;

	if ( pat->kind == DP_PAT_TONE
	  && ( pat->tone >= DP_TONE_MAX - 1 || pat->tone <= 1 || pat->fore == pat->back ) ) {
		c = ( pat->tone <= 1 ) ? pat->back : pat->fore;
		X = dvx(e, x);
		X1 = dvx(e, x1);
		for ( Y = Y0; Y < Y1; Y++ ) {
			UW	*p = dev_at(e, X, Y);
			INT	n;

			for ( n = X; n < X1; n++, p++ ) {
				blend(p, c, e->mode);
			}
		}
		return;
	}
	for ( ; x < x1; x++ ) {
		if ( pat_at(pat, x - ox, y - oy, &c) ) {
			dev_block(e, x, y, c);
		}
	}
}

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
	if ( SCALED(e) ) {
		run_scaled(e, pat, x, x1, y, ox, oy);
	} else {
		p = pixel_at(e, x, y);
		for ( ; x < x1; x++, p++ ) {
			UW	c;

			if ( pat_at(pat, x - ox, y - oy, &c) ) {
				blend(p, c, e->mode);
			}
		}
		x = x1;
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
	{
		T_DPRECOP	op;

		rec_op(&op, DP_REC_ROUND);
		op.r = *rect;
		op.rx = rx;
		op.ry = ry;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
	{
		T_DPRECOP	op;

		rec_op(&op, DP_REC_ROUNDF);
		op.r = *rect;
		op.rx = rx;
		op.ry = ry;
		op.width = width;
		if ( knl_dp_rec_draw(gid, &op, pat) ) {
			return E_OK;
		}
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
	if ( e == NULL || e->rec != NULL ) {
		tk_unl_mtx(dp_mtxid);
		return ( e == NULL ) ? E_ID : E_OK;	/* a recorded page has nothing to move */
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

	if ( SCALED(e) ) {
		/* the target's own pixels moved, as many as the smaller of the two ends covers */
		INT	SX = dvx(e, s.left), SY = dvy(e, s.top), DX = dvx(e, d.left), DY = dvy(e, d.top);
		INT	W = dvx(e, s.left + w) - SX, H = dvy(e, s.top + h) - SY;

		if ( dvx(e, d.left + w) - DX < W ) W = dvx(e, d.left + w) - DX;
		if ( dvy(e, d.top + h) - DY < H ) H = dvy(e, d.top + h) - DY;
		first = ( DY > SY ) ? H - 1 : 0;
		step = ( DY > SY ) ? -1 : 1;
		for ( y = 0; y < H; y++ ) {
			INT	row = first + y * step;
			UW	*sp = dev_at(e, SX, SY + row);
			UW	*dp = dev_at(e, DX, DY + row);
			INT	x;

			if ( DX > SX ) {
				for ( x = W - 1; x >= 0; x-- ) dp[x] = sp[x];
			} else {
				for ( x = 0; x < W; x++ ) dp[x] = sp[x];
			}
		}
		goto moved;
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
    moved:
	dp_count.calls++;
	dp_count.pixels += (UD)w * (UD)h;
	tk_unl_mtx(dp_mtxid);

	d.right = d.left + w;
	d.bottom = d.top + h;
	damaged(e, &d);

	return E_OK;
}

/* ---------------------------------------------------------------- the target's own pixels */

/*
 * What a scaled environment may write, as its target's pixels: the clip
 * and, row by row, the region turned into them. FALSE for nothing.
 */
LOCAL BOOL dev_clip( CONST DPENV *e, T_DPRECT *clip, T_DPRECT *dclip )
{
	if ( !env_clip(e, clip) ) {
		return FALSE;
	}
	dev_rect(e, clip, dclip);
	return ( dclip->left < dclip->right && dclip->top < dclip->bottom );
}

/*
 * The runs of target pixels of row Y that the environment may write,
 * cut to [X0, X1): up to 'max' of them into a[] (left, right pairs).
 * Without a region, the one run.
 */
LOCAL INT dev_runs( CONST DPENV *e, INT Y, INT X0, INT X1, INT *a, INT max )
{
	CONST INT	*sp = NULL;
	INT		ns, i, n = 0;

	if ( X0 >= X1 ) {
		return 0;
	}
	if ( e->rgn == NULL ) {
		a[0] = X0;
		a[1] = X1;
		return 1;
	}
	ns = dp_rgn_row(e->rgn, vwy(e, Y) - e->origin.y, &sp);
	for ( i = 0; i < ns && n < max; i++ ) {
		INT	l = dvx(e, sp[i * 2] + e->origin.x);
		INT	r = dvx(e, sp[i * 2 + 1] + e->origin.x);

		if ( l < X0 ) l = X0;
		if ( r > X1 ) r = X1;
		if ( l < r ) {
			a[n * 2] = l;
			a[n * 2 + 1] = r;
			n++;
		}
	}
	return n;
}

#define DEV_RUNS	32

/* The view rectangle of the screen a rectangle of target pixels lies in */
LOCAL void view_rect( CONST DPENV *e, CONST T_DPRECT *d, T_DPRECT *r )
{
	r->left = vwx(e, d->left);
	r->top = vwy(e, d->top);
	r->right = vwx(e, d->right - 1) + 1;
	r->bottom = vwy(e, d->bottom - 1) + 1;
}

/*
 * A letter's pixels made at the target's own size, put down with their
 * corner dx, dy target pixels from where view place (x, y) lies: a
 * share of each pixel covered (gray) or a bit a pixel (mono). In an
 * environment that is not scaled the place is (x + dx, y + dy).
 */
LOCAL ER put_cov_dev( INT gid, INT x, INT y, INT dx, INT dy, CONST UB *bits, INT pitch,
		      INT w, INT h, UW colour, BOOL gray )
{
	DPENV		*e;
	T_DPRECT	clip, dc, b, touched;
	INT		row, X0, Y0, k = gamma_of(colour), runs[DEV_RUNS * 2];
	INT		lr, lg, lb;

	if ( bits == NULL || w <= 0 || h <= 0 || ( gray && pitch < w ) ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	if ( !SCALED(e) || e->rec != NULL ) {
		tk_unl_mtx(dp_mtxid);
		return gray ? dp_put_gray(gid, x + dx, y + dy, bits, pitch, w, h, colour)
			    : dp_put_mono(gid, x + dx, y + dy, bits, pitch, w, h, colour);
	}
	if ( !dev_clip(e, &clip, &dc) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	if ( gray && !dp_gm_made[k] ) {
		gamma_make(k);
	}
	lr = dp_lin[k][( colour >> 16 ) & 0xFF];
	lg = dp_lin[k][( colour >> 8 ) & 0xFF];
	lb = dp_lin[k][colour & 0xFF];
	X0 = dvx(e, x + e->origin.x) + dx;
	Y0 = dvy(e, y + e->origin.y) + dy;
	b.left = X0;  b.top = Y0;  b.right = X0 + w;  b.bottom = Y0 + h;
	if ( !rect_cut(&b, &dc) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	for ( row = b.top; row < b.bottom; row++ ) {
		CONST UB	*line = bits + (SZ)( row - Y0 ) * pitch;
		INT		n = dev_runs(e, row, b.left, b.right, runs, DEV_RUNS), i, X;

		for ( i = 0; i < n; i++ ) {
			UW	*p = dev_at(e, runs[i * 2], row);

			for ( X = runs[i * 2]; X < runs[i * 2 + 1]; X++, p++ ) {
				INT	c = X - X0;

				if ( gray ) {
					UW	a = line[c];

					if ( a != 0 ) {
						*p = gray_mix(*p, a, colour, k, lr, lg, lb);
					}
				} else if ( ( ( line[c >> 3] >> ( 7 - ( c & 7 ) ) ) & 1 ) != 0 ) {
					blend(p, colour, e->mode);
				}
			}
		}
	}
	dp_count.calls++;
	dp_count.pixels += (UD)( b.right - b.left ) * (UD)( b.bottom - b.top );
	view_rect(e, &b, &touched);
	tk_unl_mtx(dp_mtxid);
	damaged(e, &touched);

	return E_OK;
}

EXPORT ER dp_put_gray_dev( INT gid, INT x, INT y, INT dx, INT dy, CONST UB *cov, INT pitch,
			   INT w, INT h, UW colour )
{
	return put_cov_dev(gid, x, y, dx, dy, cov, pitch, w, h, colour, TRUE);
}

EXPORT ER dp_put_mono_dev( INT gid, INT x, INT y, INT dx, INT dy, CONST UB *bits, INT pitch,
			   INT w, INT h, UW colour )
{
	return put_cov_dev(gid, x, y, dx, dy, bits, pitch, w, h, colour, FALSE);
}

/*
 * A picture of pw by ph pixels laid over the view rectangle 'box', taken
 * again for every pixel the box covers in the target, nearest pixel of
 * the picture to it: a picture made larger than the box (an icon of
 * thirty-two for a box of sixteen) comes out at the target's own detail
 * where the environment is scaled. A pixel equal to 'clear' is not laid.
 */
EXPORT ER dp_put_image( INT gid, CONST T_DPRECT *box, CONST UW *px, INT pw, INT ph,
			INT pitch, UW clear )
{
	DPENV		*e;
	T_DPRECT	clip, dc, d, b, touched;
	INT		X, Y, DW, DH, runs[DEV_RUNS * 2];

	if ( box == NULL || px == NULL || pw <= 0 || ph <= 0 || pitch < pw
	  || box->right <= box->left || box->bottom <= box->top ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	if ( !SCALED(e) || e->rec != NULL ) {
		INT	bw = box->right - box->left, bh = box->bottom - box->top;
		UW	*sq;
		ER	er;

		tk_unl_mtx(dp_mtxid);
		if ( bw == pw && bh == ph ) {
			return dp_put_argb(gid, box->left, box->top, px, pitch, pw, ph, clear);
		}
		sq = (UW *)Kmalloc((SZ)bw * bh * sizeof(UW));
		if ( sq == NULL ) {
			return E_NOMEM;
		}
		for ( Y = 0; Y < bh; Y++ ) {
			for ( X = 0; X < bw; X++ ) {
				sq[(SZ)Y * bw + X] = px[(SZ)( Y * ph / bh ) * pitch + X * pw / bw];
			}
		}
		er = dp_put_argb(gid, box->left, box->top, sq, bw, bw, bh, clear);
		Kfree(sq);
		return er;
	}
	if ( !dev_clip(e, &clip, &dc) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	b.left = box->left + e->origin.x;
	b.top = box->top + e->origin.y;
	b.right = box->right + e->origin.x;
	b.bottom = box->bottom + e->origin.y;
	dev_rect(e, &b, &d);
	DW = d.right - d.left;
	DH = d.bottom - d.top;
	b = d;
	if ( DW <= 0 || DH <= 0 || !rect_cut(&b, &dc) ) {
		dp_count.clipped++;
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	for ( Y = b.top; Y < b.bottom; Y++ ) {
		CONST UW	*line = px + (SZ)( ( Y - d.top ) * ph / DH ) * pitch;
		INT		n = dev_runs(e, Y, b.left, b.right, runs, DEV_RUNS), i;

		for ( i = 0; i < n; i++ ) {
			UW	*p = dev_at(e, runs[i * 2], Y);

			for ( X = runs[i * 2]; X < runs[i * 2 + 1]; X++, p++ ) {
				UW	c = line[( X - d.left ) * pw / DW];

				if ( c != clear ) {
					blend(p, c, e->mode);
				}
			}
		}
	}
	dp_count.calls++;
	dp_count.pixels += (UD)( b.right - b.left ) * (UD)( b.bottom - b.top );
	view_rect(e, &b, &touched);
	tk_unl_mtx(dp_mtxid);
	damaged(e, &touched);

	return E_OK;
}

/*
 * The pixels a view rectangle r covers, as the target holds them: in a
 * scaled environment the target's own pixels, more of them than the
 * rectangle has view pixels, so that what is put back (dp_put_dev) is
 * what was there and not the rectangle drawn larger. *p_px is taken
 * from the heap (Kfree it); pixels outside what the environment may
 * touch are left 0.
 */
EXPORT ER dp_get_dev( INT gid, CONST T_DPRECT *r, UW **p_px, INT *p_w, INT *p_h )
{
	DPENV		*e;
	T_DPRECT	clip, dc, d, b;
	INT		W, H, Y;
	UW		*px;

	if ( r == NULL || p_px == NULL || p_w == NULL || p_h == NULL
	  || r->right <= r->left || r->bottom <= r->top ) {
		return E_PAR;
	}
	*p_px = NULL;
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	if ( !SCALED(e) ) {
		tk_unl_mtx(dp_mtxid);
		W = r->right - r->left;
		H = r->bottom - r->top;
		px = (UW *)Kmalloc((SZ)W * H * sizeof(UW));
		if ( px == NULL ) {
			return E_NOMEM;
		}
		knl_memset(px, 0, (SZ)W * H * sizeof(UW));
		(void)dp_get_argb(gid, r->left, r->top, px, W, W, H);
		*p_px = px;
		*p_w = W;
		*p_h = H;
		return E_OK;
	}
	b.left = r->left + e->origin.x;
	b.top = r->top + e->origin.y;
	b.right = r->right + e->origin.x;
	b.bottom = r->bottom + e->origin.y;
	dev_rect(e, &b, &d);
	W = d.right - d.left;
	H = d.bottom - d.top;
	if ( W <= 0 || H <= 0 ) {
		tk_unl_mtx(dp_mtxid);
		return E_PAR;
	}
	px = (UW *)Kmalloc((SZ)W * H * sizeof(UW));
	if ( px == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_NOMEM;
	}
	knl_memset(px, 0, (SZ)W * H * sizeof(UW));
	b = d;
	if ( dev_clip(e, &clip, &dc) && rect_cut(&b, &dc) ) {
		for ( Y = b.top; Y < b.bottom; Y++ ) {
			knl_memcpy(px + (SZ)( Y - d.top ) * W + ( b.left - d.left ), dev_at(e, b.left, Y),
				   (SZ)( b.right - b.left ) * sizeof(UW));
		}
	}
	tk_unl_mtx(dp_mtxid);
	*p_px = px;
	*p_w = W;
	*p_h = H;

	return E_OK;
}

/* What dp_get_dev took, put back over the same view rectangle, every pixel as it was */
EXPORT ER dp_put_dev( INT gid, CONST T_DPRECT *r, CONST UW *px, INT w, INT h )
{
	DPENV		*e;
	T_DPRECT	clip, dc, d, b, touched;
	INT		Y;

	if ( r == NULL || px == NULL || w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	e = env_of(gid);
	if ( e == NULL ) {
		tk_unl_mtx(dp_mtxid);
		return E_ID;
	}
	if ( !SCALED(e) ) {
		tk_unl_mtx(dp_mtxid);
		return dp_put_argb(gid, r->left, r->top, px, w, w, h, 0xFF000000U);
	}
	b.left = r->left + e->origin.x;
	b.top = r->top + e->origin.y;
	b.right = r->right + e->origin.x;
	b.bottom = r->bottom + e->origin.y;
	dev_rect(e, &b, &d);
	if ( d.right - d.left != w || d.bottom - d.top != h ) {
		tk_unl_mtx(dp_mtxid);
		return E_PAR;			/* taken at another scale */
	}
	b = d;
	if ( !dev_clip(e, &clip, &dc) || !rect_cut(&b, &dc) ) {
		tk_unl_mtx(dp_mtxid);
		return E_OK;
	}
	for ( Y = b.top; Y < b.bottom; Y++ ) {
		knl_memcpy(dev_at(e, b.left, Y), px + (SZ)( Y - d.top ) * w + ( b.left - d.left ),
			   (SZ)( b.right - b.left ) * sizeof(UW));
	}
	dp_count.calls++;
	view_rect(e, &b, &touched);
	tk_unl_mtx(dp_mtxid);
	damaged(e, &touched);

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

/*
 * The screen came to another size (ts_disp_setmode): what drawing
 * straight onto it is kept inside taken again. The environments that
 * draw into windows were given their own targets and are moved with
 * them by the window manager.
 */
EXPORT void knl_dp_screen_changed( void )
{
	if ( !dp_ready ) {
		return;
	}
	tk_loc_mtx(dp_mtxid, TMO_FEVR);
	(void)ts_disp_ref(&dp_spec);
	tk_unl_mtx(dp_mtxid);
}
