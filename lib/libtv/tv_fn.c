/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_fn.c
 *	The letters of a program running as a process, through the faces of
 *	the kernel (design 10.3, 11.11)
 *
 *	A face's size, look and angle are the face's own in the kernel, and
 *	the kernel's desktop draws with the same faces. A process that set
 *	them there and left them would change how the desktop's letters
 *	come out. So the calls that set them (fn_set_size, fn_set_style,
 *	fn_set_angle) only change what this process keeps for the face, as
 *	the kernel's would check them, and every call that draws or
 *	measures hands that to the kernel with it (fn_dra_txt), which sets
 *	the face so while it draws and puts it back after.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/fn.h>
#include <ts/dp.h>
#include <string.h>
#include <stdlib.h>

#define TVF_PX_MAX	512		/* as fn_set_size of the kernel */
#define TVF_XPX_MAX	1024

/*
 * What this process keeps for each face, by its ID less one; as long as
 * the highest ID it has set. A face it has set nothing for is drawn as
 * the kernel has it (st.px 0).
 */
LOCAL T_FNSTATE	*tvf;
LOCAL INT	tvf_n;
LOCAL UD	tvf_calls;		/* calls of fn_dra_txt made, for a measurement */

EXPORT UD tv_fn_calls( void )
{
	return tvf_calls;
}

LOCAL T_FNSTATE *face_of( ID fid, BOOL make )
{
	if ( fid <= 0 ) return NULL;
	if ( fid > tvf_n ) {
		T_FNSTATE	*t;
		INT		n = ( tvf_n > 0 ) ? tvf_n : 16;

		if ( !make ) return NULL;
		while ( n < fid ) n *= 2;
		t = (T_FNSTATE *)realloc(tvf, sizeof(T_FNSTATE) * (size_t)n);
		if ( t == NULL ) return NULL;
		memset(t + tvf_n, 0, sizeof(T_FNSTATE) * (size_t)( n - tvf_n ));
		tvf = t;
		tvf_n = n;
	}
	return &tvf[fid - 1];
}

EXPORT ER fn_set_size( ID fid, INT px )
{
	T_FNSTATE	*f;

	if ( fid <= 0 ) return E_ID;
	if ( px <= 0 || px > TVF_PX_MAX ) return E_PAR;
	f = face_of(fid, TRUE);
	if ( f == NULL ) return E_NOMEM;
	f->px = px;
	f->style = 0;
	f->xpx = 0;
	f->angle = 0;
	return E_OK;
}

EXPORT ER fn_set_angle( ID fid, INT deg )
{
	T_FNSTATE	*f = face_of(fid, FALSE);

	if ( fid <= 0 ) return E_ID;
	if ( f == NULL || f->px == 0 ) return E_PAR;
	f->angle = ( deg % 360 + 360 ) % 360;
	return E_OK;
}

EXPORT ER fn_set_style( ID fid, UINT style, INT xpx )
{
	T_FNSTATE	*f = face_of(fid, FALSE);

	if ( fid <= 0 ) return E_ID;
	if ( f == NULL || f->px == 0 || xpx < 0 || xpx > TVF_XPX_MAX ) return E_PAR;
	if ( xpx == f->px ) xpx = 0;
	f->style = style;
	f->xpx = xpx;
	return E_OK;
}

/* One call of the kernel's letters, with what this process keeps for the face */
LOCAL INT call( UINT op, INT gid, ID fid, INT x, INT y, CONST UB *s, UW colour, T_FNTXT *pk )
{
	T_FNSTATE	*f = face_of(fid, FALSE);

	memset(pk, 0, sizeof(*pk));
	pk->op = op;
	pk->gid = gid;
	pk->fid = fid;
	pk->x = x;
	pk->y = y;
	pk->text = s;
	pk->len = ( s != NULL ) ? (INT)strlen((CONST char *)s) : 0;
	pk->colour = colour;
	if ( f != NULL ) pk->st = *f;		/* st.px 0: the face as the kernel has it */
	tvf_calls++;
	return fn_dra_txt(pk);
}

EXPORT INT fn_draw( INT gid, ID fid, INT x, INT y, CONST UB *utf8, UW colour )
{
	T_FNTXT	pk;

	return call(FN_OP_DRAW, gid, fid, x, y, utf8, colour, &pk);
}

EXPORT INT fn_width( ID fid, CONST UB *utf8 )
{
	T_FNTXT	pk;

	return call(FN_OP_WIDTH, -1, fid, 0, 0, utf8, 0, &pk);
}

EXPORT INT fn_vdraw( INT gid, ID fid, INT cx, INT y, CONST UB *utf8, UW colour )
{
	T_FNTXT	pk;

	return call(FN_OP_VDRAW, gid, fid, cx, y, utf8, colour, &pk);
}

EXPORT INT fn_vwidth( ID fid, CONST UB *utf8 )
{
	T_FNTXT	pk;

	return call(FN_OP_VWIDTH, -1, fid, 0, 0, utf8, 0, &pk);
}

EXPORT ER fn_extent( ID fid, CONST UB *utf8, INT *p_above, INT *p_below )
{
	T_FNTXT	pk;
	ER	er = call(FN_OP_EXTENT, -1, fid, 0, 0, utf8, 0, &pk);

	if ( er >= E_OK ) {
		if ( p_above != NULL ) *p_above = pk.above;
		if ( p_below != NULL ) *p_below = pk.below;
	}
	return er;
}

EXPORT INT fn_fit( ID fid, CONST UB *utf8, INT width )
{
	T_FNTXT	pk;
	T_FNSTATE	*f = face_of(fid, FALSE);

	memset(&pk, 0, sizeof(pk));
	pk.op = FN_OP_FIT;
	pk.gid = -1;
	pk.fid = fid;
	pk.text = utf8;
	pk.len = ( utf8 != NULL ) ? (INT)strlen((CONST char *)utf8) : 0;
	pk.width = width;
	if ( f != NULL ) pk.st = *f;
	tvf_calls++;
	return fn_dra_txt(&pk);
}

EXPORT ER fn_metrics( ID fid, T_FNMET *met )
{
	T_FNTXT	pk;
	ER	er;

	if ( met == NULL ) return E_PAR;
	er = call(FN_OP_METRICS, -1, fid, 0, 0, NULL, 0, &pk);
	if ( er >= E_OK ) *met = pk.met;
	return er;
}

EXPORT BOOL fn_has_vert( ID fid, UW cp )
{
	T_FNTXT	pk;
	T_FNSTATE	*f = face_of(fid, FALSE);

	memset(&pk, 0, sizeof(pk));
	pk.op = FN_OP_HAS_VERT;
	pk.gid = -1;
	pk.fid = fid;
	pk.cp = cp;
	if ( f != NULL ) pk.st = *f;
	tvf_calls++;
	return (BOOL)( fn_dra_txt(&pk) > 0 );
}
