/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om_vdraw.c
 *	A virtual object drawn: its frame, its band with the picture and
 *	the words, and the room of an opened one (design 16.3)
 *
 *	Only drawing calls are made here, so the same code draws a link in
 *	the kernel's desktop and in a program running as a process
 *	(include/ts/tvport.h).
 */

#include <ts/tvport.h>
#include <ts/ob.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/img.h>

/* ---------------------------------------------------------------- drawing */

LOCAL UW colour_or( UW c, UW def )
{
	return ( c == TAD_COL_NONE ) ? def : c;
}

#define OM_GONE_INK	0x00A8A8A8U	/* a 虚身's frame and words */
#define OM_GONE_PAPER	0x00F4F4F4U	/* and what they are on */

/*
 * How tall the band along the top of a virtual object is: the height of
 * the object closed, the letters it holds and a little either side. A
 * link that shows none of the picture, the name, the relationship, the
 * kind and the date has no band at all: it is a plain box in tbcol.
 * The rule belongs to the records, so it is the record layer's
 * (tad_lnk_closed_h), and every program that sizes a link uses the same.
 */
EXPORT INT om_band_h( CONST T_VOBJ *vobj )
{
	return tad_lnk_closed_h(vobj);
}

/*
 * The picture that stands at the left of the band when the object has
 * no icon of its own: a leaf of paper with its corner turned. It is
 * drawn rather than left out because its absence would move the name.
 */
LOCAL void draw_leaf( INT gid, CONST T_DPRECT *box, UW ink, UW paper )
{
	T_DPRECT	q = *box;
	INT		fold;

	if ( q.right - q.left < 6 || q.bottom - q.top < 6 ) {
		return;
	}
	q.right = q.left + ( q.bottom - q.top ) * 3 / 4;
	fold = ( q.right - q.left ) / 3;
	dp_fill_rect(gid, &q, paper);
	dp_frame_rect(gid, &q, ink, 1);
	/* the turned corner */
	dp_line(gid, q.right - 1 - fold, q.top, q.right - 1, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top,
		q.right - 1 - fold, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top + fold, q.right - 1, q.top + fold,
		ink);
}

/*
 * The object's own icon in a square box, the nearest pixel of it for
 * each of the box's, its clear pixels left as they are.
 */
#define OM_ICON_MAX	64

LOCAL void draw_icon( INT gid, CONST T_DPRECT *box, CONST UW *px, INT w, INT h )
{
	UW	*sq;
	INT	side = box->bottom - box->top, x, y;

	if ( side > OM_ICON_MAX ) side = OM_ICON_MAX;
	if ( side <= 0 || w <= 0 || h <= 0 ) {
		return;
	}
	if ( dp_scale(gid) > 100 ) {
		T_DPRECT	b = *box;

		/* drawn at a scale: the icon taken again at the box's own pixels, from all of its detail */
		b.right = b.left + side;
		b.bottom = b.top + side;
		(void)dp_put_image(gid, &b, px, w, h, w, IMG_CLEAR);
		return;
	}
	/* from the heap: this may run on a process's small stack of calls */
	sq = (UW *)Kmalloc((SZ)side * side * sizeof(UW));
	if ( sq == NULL ) {
		return;
	}
	for ( y = 0; y < side; y++ ) {
		for ( x = 0; x < side; x++ ) {
			sq[y * side + x] = px[(SZ)( y * h / side ) * w + x * w / side];
		}
	}
	dp_put_argb(gid, box->left, box->top, sq, side, side, side, IMG_CLEAR);
	Kfree(sq);
}

/*
 * A virtual object drawn in a rectangle given, whether or not it is in
 * the register. Showing a document means drawing the links that stand
 * in it, and showing something is not a reason to register it.
 *
 * What is drawn is what the link's own attributes ask for and nothing
 * else: the box in tbcol, holding the picture and the words in chcol
 * at chsz, the frame in frcol. An opened one has the band in tbcol and
 * the room under it in bgcol. The words are what the caller made of
 * the link (om_draw_vob_as): its name, its relationship, the kind of
 * program it opens with and the date it was changed, each only when
 * the link asks; without them, the link's own name.
 */
LOCAL ER draw_vob_body( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			CONST T_OMLOOK *look, BOOL open, T_DPRECT *p_inner )
{
	T_DPRECT	r, band, pict;
	UW		frcol, tbcol, bgcol, chcol;
	INT		bh, ch, tx;
	CONST UB	*text = NULL;
	BOOL		gone;

	if ( rect == NULL || vobj == NULL ) {
		return E_PAR;
	}
	r = *rect;
	if ( r.right <= r.left || r.bottom <= r.top ) {
		return E_PAR;
	}
	band = r;
	frcol = colour_or(vobj->frcol, OM_DEF_FRCOL);
	tbcol = colour_or(vobj->tbcol, OM_DEF_TBCOL);
	bgcol = colour_or(vobj->bgcol, OM_DEF_BGCOL);
	chcol = colour_or(vobj->chcol, OM_DEF_CHCOL);
	gone = (BOOL)( look != NULL && ( look->state & OM_LOOK_GONE ) != 0 );
	if ( gone ) {
		/* a 虚身: what it points at is on a disk not attached, and it is drawn pale */
		frcol = chcol = OM_GONE_INK;
		tbcol = bgcol = OM_GONE_PAPER;
	}
	ch = ( vobj->chsz > 0 ) ? vobj->chsz : 14;
	bh = om_band_h(vobj);
	if ( look != NULL && look->text != NULL ) {
		text = look->text;
	} else if ( (vobj->disp & TAD_D_NAME) != 0 ) {
		text = vobj->name;
	}

	/* the whole of it first: a closed one is all band, an opened one the room below */
	dp_fill_rect(gid, &r, open ? bgcol : tbcol);

	if ( bh > 0 ) {
		BOOL	fr = (BOOL)( (vobj->disp & TAD_D_FRAME) != 0 );
		INT	half = ch / 2, quarter = ch / 4, ntop, limit;

		/*
		 * Everything in the band is measured from the box's corner and
		 * the letters' size, so a stretched box keeps its picture where
		 * it was and only the room for the name grows. With a frame the
		 * band is inside it, closed height less the two lines.
		 */
		band = r;
		if ( fr ) {
			band.left   = r.left + OM_FRAME_W;
			band.right  = r.right - OM_FRAME_W;
			band.top    = r.top + OM_FRAME_W;
			band.bottom = r.top + bh - OM_FRAME_W;
			if ( band.bottom > r.bottom - OM_FRAME_W ) {
				band.bottom = r.bottom - OM_FRAME_W;
			}
		} else if ( r.top + bh < r.bottom ) {
			band.bottom = r.top + bh;
		}
		if ( band.bottom > band.top && band.right > band.left ) {
			dp_fill_rect(gid, &band, tbcol);
		}
		/* the name: one letter tall, half a letter in from the frame */
		tx    = fr ? r.left + half : r.left;
		ntop  = fr ? r.top + OM_FRAME_W + ch / 8 : r.top;
		limit = fr ? r.right - half : r.right;
		if ( (vobj->disp & TAD_D_PICT) != 0 ) {
			/* the picture: a square of the letters and a quarter */
			pict.left   = tx;
			pict.top    = fr ? r.top + OM_FRAME_W : r.top;
			pict.bottom = pict.top + ch + quarter;
			if ( pict.bottom > band.bottom ) {
				pict.bottom = band.bottom;
			}
			pict.right  = pict.left + ( pict.bottom - pict.top );
			if ( pict.bottom > pict.top ) {
				if ( look != NULL && look->icon != NULL && !gone ) {
					draw_icon(gid, &pict, look->icon, look->icon_w, look->icon_h);
				} else {
					draw_leaf(gid, &pict, chcol, bgcol);
				}
			}
			if ( fr ) {
				tx = r.left + half + ch + half;
			} else {
				tx = r.left + ch + quarter;
				ntop = r.top + quarter;
			}
		}
		if ( text != NULL && text[0] != '\0' ) {
			ID	fid = fn_system();

			if ( fid > 0 ) {
				T_FNMET	met;
				INT	room = limit - tx;

				fn_set_size(fid, ch);
				if ( room > 0 && fn_metrics(fid, &met) >= E_OK ) {
					UB	cut[OM_LOOK_TEXT];
					INT	i, n;

					/* what does not fit is cut, not squeezed */
					n = fn_fit(fid, text, room);
					if ( n < 0 ) {
						n = 0;
					}
					for ( i = 0; i < n && i < OM_LOOK_TEXT - 1; i++ ) {
						cut[i] = text[i];
					}
					cut[i] = '\0';
					/* the letters centred in the name's one letter of height */
					fn_draw(gid, fid, tx,
						ntop + ( ch + met.ascent - met.descent ) / 2,
						cut, chcol);
				}
			}
		}
		if ( open && fr && r.top + bh - OM_FRAME_W < r.bottom - OM_FRAME_W ) {
			/* the line under an opened object's band, where a closed one's frame would be */
			dp_line(gid, r.left, r.top + bh - OM_FRAME_W, r.right - 1, r.top + bh - OM_FRAME_W, frcol);
		}
	}
	if ( (vobj->disp & TAD_D_FRAME) != 0 ) {
		dp_frame_rect(gid, &r, frcol, OM_FRAME_W);
	}
	if ( gone && open && bh > 0 && r.bottom - OM_FRAME_W > r.top + bh ) {
		/* an opened 虚身: lines across the room it has nothing to show in */
		T_DPRECT	room = r;
		T_DPENV		env;
		INT		x;

		room.left += OM_FRAME_W;
		room.right -= OM_FRAME_W;
		room.top = r.top + bh;
		room.bottom -= OM_FRAME_W;
		if ( dp_ref(gid, &env) >= E_OK ) {
			T_DPRECT	cut = env.visible;

			if ( cut.left < room.left ) cut.left = room.left;
			if ( cut.top < room.top ) cut.top = room.top;
			if ( cut.right > room.right ) cut.right = room.right;
			if ( cut.bottom > room.bottom ) cut.bottom = room.bottom;
			if ( cut.right > cut.left && cut.bottom > cut.top ) {
				dp_set_visible(gid, &cut);
				for ( x = room.left - ( room.bottom - room.top ); x < room.right; x += 8 ) {
					dp_line(gid, x, room.bottom - 1, x + ( room.bottom - room.top ), room.top,
						OM_GONE_INK);
				}
				dp_set_visible(gid, &env.visible);
			}
		}
	}
	if ( look != NULL && ( look->state & OM_LOOK_OTHER ) != 0 ) {
		/* on another volume than the object it is in: the top left corner filled */
		T_DPPOINT	tri[3];
		T_DPPAT		pat;
		INT		s = ( bh > 0 ) ? bh / 3 : 6;

		if ( s < 4 ) s = 4;
		if ( s > 10 ) s = 10;
		tri[0].x = r.left;  tri[0].y = r.top;
		tri[1].x = r.left + s;  tri[1].y = r.top;
		tri[2].x = r.left;  tri[2].y = r.top + s;
		dp_pat_colour(&pat, gone ? OM_GONE_INK : frcol);
		dp_fill_poly(gid, tri, 3, DP_POLY_ODD, &pat);
	}
	if ( p_inner != NULL ) {
		p_inner->left   = r.left + OM_FRAME_W;
		p_inner->right  = r.right - OM_FRAME_W;
		p_inner->top    = ( bh > 0 ) ? r.top + bh : r.top + OM_FRAME_W;
		p_inner->bottom = r.bottom - OM_FRAME_W;
		if ( (vobj->disp & TAD_D_FRAME) == 0 ) {
			/* without a frame the room keeps its distance from the edge */
			p_inner->left   += OM_OPEN_MARGIN;
			p_inner->right  -= OM_OPEN_MARGIN;
			p_inner->top    += OM_OPEN_MARGIN;
			p_inner->bottom -= OM_OPEN_MARGIN;
		}
	}

	return E_OK;
}

EXPORT ER om_draw_vob( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj )
{
	return draw_vob_body(gid, rect, vobj, NULL, FALSE, NULL);
}

EXPORT ER om_draw_open( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			T_DPRECT *p_inner )
{
	return draw_vob_body(gid, rect, vobj, NULL, TRUE, p_inner);
}

EXPORT ER om_draw_vob_as( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			  CONST T_OMLOOK *look, BOOL open, T_DPRECT *p_inner )
{
	return draw_vob_body(gid, rect, vobj, look, open, p_inner);
}

