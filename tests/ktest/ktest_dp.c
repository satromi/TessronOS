/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_dp.c
 *	Drawing (design 16.4, stage 9c).
 *
 *	Every case here reads the pixels back out of the back buffer and
 *	compares them, so what is checked is the picture rather than the
 *	fact that a call returned. The cutting rules are what most of it is
 *	about: a drawing layer that writes one pixel outside its window is
 *	a drawing layer that corrupts another window.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/dp.h>
#include <ts/disp.h>

LOCAL BOOL		have_dp = FALSE;
LOCAL T_DISPSPEC	spec;
LOCAL INT		gid = 0;

/* What the back buffer holds at a place on the screen */
LOCAL UW at( INT x, INT y )
{
	UB	*p = (UB *)ts_disp_buffer();

	return *(UW *)(p + (UBINT)y * spec.pitch + (UBINT)x * 4) & 0x00FFFFFF;
}

/* Paint the whole screen one colour without going through an environment */
LOCAL void wipe( UW colour )
{
	ts_disp_clear(colour);
}

/* an environment opens with the whole screen to itself */
LOCAL void test_open( void )
{
	T_DPENV	env;

	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	gid = dp_open();
	if ( gid < 0 ) {
		KT_SKIP("drawing did not start");
	}
	have_dp = TRUE;
	KT_ASSERT(gid >= 1);

	KT_ASSERT_ER(dp_ref(gid, &env), E_OK);
	KT_ASSERT_EQ(env.origin.x, 0);
	KT_ASSERT_EQ(env.origin.y, 0);
	KT_ASSERT_EQ(env.frame.right, (INT)spec.width);
	KT_ASSERT_EQ(env.frame.bottom, (INT)spec.height);
	KT_ASSERT_EQ(env.mode, DP_MODE_COPY);

	/* an environment that was never opened, and one already closed */
	{
		INT	g2 = dp_open();

		KT_ASSERT(g2 >= 1);
		KT_ASSERT(g2 != gid);
		KT_ASSERT_ER(dp_close(g2), E_OK);
		KT_ASSERT_ER(dp_close(g2), E_ID);
		KT_ASSERT_ER(dp_ref(9999, &env), E_ID);
	}
}

/* a filled rectangle covers exactly what it says and nothing beside it */
LOCAL void test_fill( void )
{
	T_DPRECT	r;
	INT		x, y;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);
	r.left = 40; r.top = 40; r.right = 60; r.bottom = 50;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00112233), E_OK);

	for ( y = 39; y <= 50; y++ ) {
		for ( x = 39; x <= 60; x++ ) {
			UW	want = ( x >= 40 && x < 60 && y >= 40 && y < 50 )
				     ? 0x112233 : 0;

			KT_ASSERT_EQ(at(x, y), want);
		}
	}
}

/* the origin moves what the caller says into place on the screen */
LOCAL void test_origin( void )
{
	T_DPRECT	r;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);
	KT_ASSERT_ER(dp_set_origin(gid, 100, 200), E_OK);

	r.left = 0; r.top = 0; r.right = 10; r.bottom = 10;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00445566), E_OK);

	KT_ASSERT_EQ(at(100, 200), 0x445566);
	KT_ASSERT_EQ(at(109, 209), 0x445566);
	KT_ASSERT_EQ(at(99, 200), 0);
	KT_ASSERT_EQ(at(110, 209), 0);

	/* and one pixel goes where the origin puts it */
	KT_ASSERT_ER(dp_put_pixel(gid, 20, 20, 0x00778899), E_OK);
	KT_ASSERT_EQ(at(120, 220), 0x778899);
	KT_ASSERT_EQ(dp_get_pixel(gid, 20, 20), 0x778899);

	KT_ASSERT_ER(dp_set_origin(gid, 0, 0), E_OK);
}

/*
 * What lies outside the frame is not written. This is the rule that
 * keeps one window out of another, so it is checked from both sides:
 * the pixels outside stay as they were, and the call still succeeds.
 */
LOCAL void test_frame( void )
{
	T_DPRECT	r, frame;
	T_DPSTAT	before, after;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);
	frame.left = 300; frame.top = 300; frame.right = 340; frame.bottom = 320;
	KT_ASSERT_ER(dp_set_frame(gid, &frame), E_OK);

	/* a rectangle far larger than the frame */
	r.left = 200; r.top = 200; r.right = 500; r.bottom = 500;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00AABBCC), E_OK);

	KT_ASSERT_EQ(at(300, 300), 0xAABBCC);
	KT_ASSERT_EQ(at(339, 319), 0xAABBCC);
	KT_ASSERT_EQ(at(299, 300), 0);
	KT_ASSERT_EQ(at(340, 300), 0);
	KT_ASSERT_EQ(at(300, 299), 0);
	KT_ASSERT_EQ(at(300, 320), 0);

	/* one wholly outside is counted as such and changes nothing */
	KT_ASSERT_ER(dp_stat(&before), E_OK);
	r.left = 0; r.top = 0; r.right = 10; r.bottom = 10;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FF0000), E_OK);
	KT_ASSERT_ER(dp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.clipped, before.clipped + 1);
	KT_ASSERT_EQ(at(5, 5), 0);

	/* the program may narrow it further, but never widen it */
	r.left = 310; r.top = 305; r.right = 320; r.bottom = 315;
	KT_ASSERT_ER(dp_set_visible(gid, &r), E_OK);
	r.left = 0; r.top = 0; r.right = 1000; r.bottom = 1000;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00DDEEFF), E_OK);
	KT_ASSERT_EQ(at(310, 305), 0xDDEEFF);
	KT_ASSERT_EQ(at(319, 314), 0xDDEEFF);
	KT_ASSERT_EQ(at(309, 305), 0xAABBCC);	/* inside the frame, outside this */
	KT_ASSERT_EQ(at(320, 305), 0xAABBCC);

	/* put it back for what follows */
	frame.left = 0; frame.top = 0;
	frame.right = (INT)spec.width; frame.bottom = (INT)spec.height;
	KT_ASSERT_ER(dp_set_frame(gid, &frame), E_OK);
	KT_ASSERT_ER(dp_set_visible(gid, &frame), E_OK);
}

/* the outline of a rectangle is drawn on all four sides and hollow inside */
LOCAL void test_frame_rect( void )
{
	T_DPRECT	r;
	INT		x, y;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);
	r.left = 50; r.top = 50; r.right = 70; r.bottom = 65;
	KT_ASSERT_ER(dp_frame_rect(gid, &r, 0x00010101, 2), E_OK);

	for ( y = 50; y < 65; y++ ) {
		for ( x = 50; x < 70; x++ ) {
			BOOL	edge = ( x < 52 || x >= 68 || y < 52 || y >= 63 );

			KT_ASSERT_EQ(at(x, y), ( edge ) ? 0x010101 : 0);
		}
	}
	/* a width of nothing, and a rectangle with no area */
	KT_ASSERT_ER(dp_frame_rect(gid, &r, 0, 0), E_PAR);
	r.right = r.left;
	KT_ASSERT_ER(dp_frame_rect(gid, &r, 0, 1), E_PAR);
}

/* a line reaches both of its ends and stays between them */
LOCAL void test_line( void )
{
	INT	i;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);

	/* flat, upright, and at an angle */
	KT_ASSERT_ER(dp_line(gid, 10, 400, 30, 400, 0x00FF0000), E_OK);
	for ( i = 10; i <= 30; i++ ) {
		KT_ASSERT_EQ(at(i, 400), 0xFF0000);
	}
	KT_ASSERT_EQ(at(9, 400), 0);
	KT_ASSERT_EQ(at(31, 400), 0);

	KT_ASSERT_ER(dp_line(gid, 50, 400, 50, 420, 0x0000FF00), E_OK);
	for ( i = 400; i <= 420; i++ ) {
		KT_ASSERT_EQ(at(50, i), 0x00FF00);
	}

	KT_ASSERT_ER(dp_line(gid, 100, 400, 110, 410, 0x000000FF), E_OK);
	for ( i = 0; i <= 10; i++ ) {
		KT_ASSERT_EQ(at(100 + i, 400 + i), 0x0000FF);
	}

	/* one point is a line with both ends in the same place */
	KT_ASSERT_ER(dp_line(gid, 200, 400, 200, 400, 0x00ABCDEF), E_OK);
	KT_ASSERT_EQ(at(200, 400), 0xABCDEF);
}

/* drawing twice in the reversing mode puts back what was there */
LOCAL void test_mode( void )
{
	T_DPRECT	r;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00123456);
	KT_ASSERT_ER(dp_set_mode(gid, DP_MODE_XOR), E_OK);

	r.left = 500; r.top = 100; r.right = 520; r.bottom = 110;
	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FFFFFF), E_OK);
	KT_ASSERT(at(505, 105) != 0x123456);

	KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00FFFFFF), E_OK);
	KT_ASSERT_EQ(at(505, 105), 0x123456);

	KT_ASSERT_ER(dp_set_mode(gid, DP_MODE_COPY), E_OK);
	KT_ASSERT_ER(dp_set_mode(gid, 99), E_PAR);
}

/*
 * Moving a rectangle over itself. A window being dragged is this, so
 * both directions are tried: the one where the destination is ahead of
 * the source and the one where it is behind.
 */
LOCAL void test_copy( void )
{
	T_DPRECT	r;
	INT		x;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);

	/* a row of pixels that says where each one came from */
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_ER(dp_put_pixel(gid, 600 + x, 500,
					  (UW)(0x00010000 + x)), E_OK);
	}
	r.left = 600; r.top = 500; r.right = 620; r.bottom = 501;

	/* forwards, where the two overlap */
	KT_ASSERT_ER(dp_copy_rect(gid, &r, 605, 500), E_OK);
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_EQ(at(605 + x, 500), (UW)(0x010000 + x));
	}

	/* and backwards */
	wipe(0x00000000);
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_ER(dp_put_pixel(gid, 600 + x, 520,
					  (UW)(0x00020000 + x)), E_OK);
	}
	r.top = 520; r.bottom = 521;
	KT_ASSERT_ER(dp_copy_rect(gid, &r, 595, 520), E_OK);
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_EQ(at(595 + x, 520), (UW)(0x020000 + x));
	}

	/*
	 * Somewhere else entirely, where the two do not overlap. The
	 * row is laid down again first: the move above landed on top
	 * of part of where it came from, which is the point of it.
	 */
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_ER(dp_put_pixel(gid, 600 + x, 520,
					  (UW)(0x00030000 + x)), E_OK);
	}
	KT_ASSERT_ER(dp_copy_rect(gid, &r, 600, 540), E_OK);
	for ( x = 0; x < 20; x++ ) {
		KT_ASSERT_EQ(at(600 + x, 540), (UW)(0x030000 + x));
	}
}

/*
 * Holding covers a run of drawing calls, so a window redrawn from many
 * pieces reaches the screen once.
 */
LOCAL void test_hold( void )
{
	T_DISPSTAT	before, after;
	T_DPRECT	r;
	INT		i;

	if ( !have_dp ) KT_SKIP("no drawing");

	wipe(0x00000000);
	KT_ASSERT_ER(ts_disp_stat(&before), E_OK);

	KT_ASSERT_ER(dp_hold(), E_OK);
	for ( i = 0; i < 10; i++ ) {
		r.left = 700 + i * 2; r.top = 600;
		r.right = 702 + i * 2; r.bottom = 602;
		KT_ASSERT_ER(dp_fill_rect(gid, &r, 0x00909090), E_OK);
	}
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends);
	KT_ASSERT_ER(dp_release(), E_OK);

	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends + 1);
	KT_ASSERT_EQ(at(700, 600), 0x909090);
	KT_ASSERT_EQ(at(719, 601), 0x909090);
}

/* ---------------------------------------------------------------- regions */

LOCAL void rect_of( T_DPRECT *r, INT l, INT t, INT rr, INT b )
{
	r->left = l;  r->top = t;  r->right = rr;  r->bottom = b;
}

/*
 * A region of one rectangle is one band of one span, and the three set
 * operations keep the form normal: no empty band, nothing repeated,
 * spans merged where they touch, and the bounding rectangle the
 * smallest that covers what is there.
 */
LOCAL void test_region( void )
{
	T_DPRECT	a, b, bound;
	T_DPRGN		*ra = NULL, *rb = NULL, *rc = NULL;
	INT		bands = 0, spans = 0;

	rect_of(&a, 10, 10, 50, 50);
	KT_ASSERT_ER(dp_rgn_rect(&a, &ra), E_OK);
	if ( ra == NULL ) return;
	KT_ASSERT_ER(dp_rgn_size(ra, &bands, &spans), E_OK);
	KT_ASSERT_EQ(bands, 1);
	KT_ASSERT_EQ(spans, 1);
	KT_ASSERT(dp_rgn_has(ra, 20, 20));
	KT_ASSERT(!dp_rgn_has(ra, 60, 20));
	KT_ASSERT(!dp_rgn_empty(ra));

	/* a rectangle with a hole: three bands, the middle one two spans */
	rect_of(&b, 20, 20, 30, 40);
	KT_ASSERT_ER(dp_rgn_rect(&b, &rb), E_OK);
	KT_ASSERT_ER(dp_rgn_sub(ra, rb, &rc), E_OK);
	if ( rc == NULL ) return;
	KT_ASSERT_ER(dp_rgn_size(rc, &bands, &spans), E_OK);
	KT_ASSERT_EQ(bands, 3);
	KT_ASSERT_EQ(spans, 4);			/* 1 + 2 + 1 */
	KT_ASSERT(!dp_rgn_has(rc, 25, 30));	/* the hole */
	KT_ASSERT(dp_rgn_has(rc, 15, 30));
	KT_ASSERT(dp_rgn_has(rc, 35, 30));
	KT_ASSERT(dp_rgn_has(rc, 25, 15));
	KT_ASSERT_ER(dp_rgn_bound(rc, &bound), E_OK);
	KT_ASSERT_EQ(bound.left, 10);
	KT_ASSERT_EQ(bound.right, 50);
	dp_rgn_free(rc);  rc = NULL;

	/* what two rectangles have in common */
	KT_ASSERT_ER(dp_rgn_and(ra, rb, &rc), E_OK);
	KT_ASSERT_ER(dp_rgn_bound(rc, &bound), E_OK);
	KT_ASSERT_EQ(bound.left, 20);
	KT_ASSERT_EQ(bound.top, 20);
	KT_ASSERT_EQ(bound.right, 30);
	KT_ASSERT_EQ(bound.bottom, 40);
	KT_ASSERT_ER(dp_rgn_size(rc, &bands, &spans), E_OK);
	KT_ASSERT_EQ(bands, 1);			/* still one plain rectangle */
	dp_rgn_free(rc);  rc = NULL;

	/* one inside the other: the union is the larger one again */
	KT_ASSERT_ER(dp_rgn_or(ra, rb, &rc), E_OK);
	KT_ASSERT_ER(dp_rgn_size(rc, &bands, &spans), E_OK);
	KT_ASSERT_EQ(bands, 1);
	KT_ASSERT_EQ(spans, 1);
	dp_rgn_free(rc);  rc = NULL;

	/* two apart: two bands, and nothing between them */
	dp_rgn_free(rb);
	rect_of(&b, 10, 80, 50, 100);
	KT_ASSERT_ER(dp_rgn_rect(&b, &rb), E_OK);
	KT_ASSERT_ER(dp_rgn_or(ra, rb, &rc), E_OK);
	KT_ASSERT_ER(dp_rgn_size(rc, &bands, &spans), E_OK);
	KT_ASSERT_EQ(bands, 2);
	KT_ASSERT(!dp_rgn_has(rc, 20, 60));

	/* nothing taken from nothing is nothing */
	{
		T_DPRGN	*none = NULL;

		KT_ASSERT_ER(dp_rgn_sub(ra, ra, &none), E_OK);
		KT_ASSERT(dp_rgn_empty(none));
		dp_rgn_free(none);
	}

	dp_rgn_free(ra);
	dp_rgn_free(rb);
	dp_rgn_free(rc);
}

/* drawing through a shape reaches the shape and nothing else */
LOCAL void test_region_draw( void )
{
	T_DPRECT	a, b, all;
	T_DPRGN		*ra = NULL, *rb = NULL, *ring = NULL;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	rect_of(&a, 100, 100, 200, 200);
	rect_of(&b, 130, 130, 170, 170);
	KT_ASSERT_ER(dp_rgn_rect(&a, &ra), E_OK);
	KT_ASSERT_ER(dp_rgn_rect(&b, &rb), E_OK);
	KT_ASSERT_ER(dp_rgn_sub(ra, rb, &ring), E_OK);
	if ( ring == NULL ) return;

	KT_ASSERT_ER(dp_set_region(gid, ring), E_OK);
	rect_of(&all, 0, 0, 400, 400);
	KT_ASSERT_ER(dp_fill_rect(gid, &all, 0x00FF0000U), E_OK);

	KT_ASSERT_EQ((INT)at(110, 110), 0x00FF0000);	/* in the ring */
	KT_ASSERT_EQ((INT)at(150, 150), 0x00202020);	/* the hole */
	KT_ASSERT_EQ((INT)at(90, 150), 0x00202020);	/* outside it */
	KT_ASSERT_EQ((INT)at(199, 199), 0x00FF0000);

	/* a line is cut by the shape as well */
	KT_ASSERT_ER(dp_line(gid, 0, 150, 399, 150, 0x0000FF00U), E_OK);
	KT_ASSERT_EQ((INT)at(110, 150), 0x0000FF00);
	KT_ASSERT_EQ((INT)at(150, 150), 0x00202020);

	/* and taking the shape away opens it up again */
	KT_ASSERT_ER(dp_set_region(gid, NULL), E_OK);
	KT_ASSERT_ER(dp_fill_rect(gid, &all, 0x000000FFU), E_OK);
	KT_ASSERT_EQ((INT)at(150, 150), 0x000000FF);

	dp_rgn_free(ra);
	dp_rgn_free(rb);
	dp_rgn_free(ring);
	wipe(0x00000000U);
}

/* a rounded box is round: its corner is left alone, its middle is not */
LOCAL void test_round( void )
{
	T_DPRECT	r;
	T_DPPAT		pat;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	rect_of(&r, 100, 100, 200, 160);
	dp_pat_colour(&pat, 0x00FFFFFFU);
	KT_ASSERT_ER(dp_fill_round(gid, &r, 24, 24, &pat), E_OK);

	KT_ASSERT_EQ((INT)at(150, 130), 0x00FFFFFF);	/* the middle */
	KT_ASSERT_EQ((INT)at(101, 101), 0x00202020);	/* the corner is cut away */
	KT_ASSERT_EQ((INT)at(198, 101), 0x00202020);
	KT_ASSERT_EQ((INT)at(101, 158), 0x00202020);
	KT_ASSERT_EQ((INT)at(150, 101), 0x00FFFFFF);	/* but the top edge is there */
	KT_ASSERT_EQ((INT)at(101, 130), 0x00FFFFFF);

	/* an oval touches the middle of each side and misses every corner */
	wipe(0x00202020U);
	KT_ASSERT_ER(dp_fill_oval(gid, &r, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(150, 130), 0x00FFFFFF);
	KT_ASSERT_EQ((INT)at(150, 100), 0x00FFFFFF);
	KT_ASSERT_EQ((INT)at(102, 102), 0x00202020);

	/* the outline of one leaves its middle alone */
	wipe(0x00202020U);
	KT_ASSERT_ER(dp_frame_round(gid, &r, 24, 24, 3, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(150, 130), 0x00202020);
	KT_ASSERT_EQ((INT)at(150, 101), 0x00FFFFFF);
	KT_ASSERT_EQ((INT)at(101, 130), 0x00FFFFFF);
	wipe(0x00000000U);
}

/* a tone is the two colours in a dither, and it lies from the origin */
LOCAL void test_tone( void )
{
	T_DPRECT	r;
	T_DPPAT		pat;
	INT		x, fore = 0, back = 0;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	dp_pat_tone(&pat, 4, 0x00FFFFFFU, 0x00000000U);	/* half and half */
	rect_of(&r, 100, 100, 140, 120);
	KT_ASSERT_ER(dp_fill_rect_pat(gid, &r, &pat), E_OK);
	for ( x = 100; x < 140; x++ ) {
		if ( at(x, 100) == 0x00FFFFFF ) fore++;
		if ( at(x, 100) == 0x00000000 ) back++;
	}
	KT_ASSERT_EQ(fore, 20);
	KT_ASSERT_EQ(back, 20);

	/* the solid tone is the plain colour, which is what a colour is */
	dp_pat_tone(&pat, 7, 0x00FF00FFU, 0x00000000U);
	KT_ASSERT_ER(dp_fill_rect_pat(gid, &r, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(100, 100), 0x00FF00FF);
	KT_ASSERT_EQ((INT)at(139, 119), 0x00FF00FF);
	wipe(0x00000000U);
}

/* a polygon is filled inside its edges and nowhere else */
LOCAL void test_poly( void )
{
	T_DPPOINT	tri[3], sq[4];
	T_DPPAT		pat;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	dp_pat_colour(&pat, 0x00FFFF00U);
	tri[0].x = 100;  tri[0].y = 100;
	tri[1].x = 200;  tri[1].y = 100;
	tri[2].x = 150;  tri[2].y = 180;
	KT_ASSERT_ER(dp_fill_poly(gid, tri, 3, DP_POLY_ODD, &pat), E_OK);

	KT_ASSERT_EQ((INT)at(150, 120), 0x00FFFF00);	/* inside */
	KT_ASSERT_EQ((INT)at(105, 170), 0x00202020);	/* outside, under the slope */
	KT_ASSERT_EQ((INT)at(150, 190), 0x00202020);	/* past the point */
	KT_ASSERT_ER(dp_fill_poly(gid, tri, 2, DP_POLY_ODD, &pat), E_PAR);

	/* an outline is drawn where the fill stopped */
	wipe(0x00202020U);
	sq[0].x = 240;  sq[0].y = 100;
	sq[1].x = 320;  sq[1].y = 100;
	sq[2].x = 320;  sq[2].y = 160;
	sq[3].x = 240;  sq[3].y = 160;
	KT_ASSERT_ER(dp_draw_poly(gid, sq, 4, TRUE, 1, DP_LINE_SOLID, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(280, 100), 0x00FFFF00);
	KT_ASSERT_EQ((INT)at(280, 130), 0x00202020);	/* its middle is left alone */
	wipe(0x00000000U);
}

/* a piece of an ellipse is bounded by its two ends */
LOCAL void test_sector( void )
{
	T_DPRECT	r;
	T_DPPAT		pat;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	dp_pat_colour(&pat, 0x0000FFFFU);
	r.left = 100;  r.top = 100;  r.right = 200;  r.bottom = 200;

	/*
	 * The quarter from due east round to due north, the way a hand
	 * draws it: the piece above and to the right of the middle.
	 */
	KT_ASSERT_ER(dp_fill_sector(gid, &r, 50, 0, 0, -50, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(170, 140), 0x0000FFFF);	/* up and to the right */
	KT_ASSERT_EQ((INT)at(170, 160), 0x00202020);	/* down and to the right */
	KT_ASSERT_EQ((INT)at(130, 160), 0x00202020);	/* the far side */

	/* a chord cuts straight across instead */
	wipe(0x00202020U);
	KT_ASSERT_ER(dp_fill_chord(gid, &r, 50, 0, 0, -50, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(150, 150), 0x00202020);	/* the middle is outside it */

	/* an arc leaves the inside alone */
	wipe(0x00202020U);
	KT_ASSERT_ER(dp_draw_arc(gid, &r, 50, 0, 0, 50, 4, &pat), E_OK);
	KT_ASSERT_EQ((INT)at(150, 150), 0x00202020);
	wipe(0x00000000U);
}

/* a line may be wide, and it may be dotted */
LOCAL void test_wide_line( void )
{
	T_DPPAT	pat;
	INT	x, on = 0, off = 0;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	dp_pat_colour(&pat, 0x00FF00FFU);
	KT_ASSERT_ER(dp_line_wide(gid, 100, 300, 200, 300, 5, DP_LINE_SOLID, &pat),
		     E_OK);
	KT_ASSERT_EQ((INT)at(150, 300), 0x00FF00FF);
	KT_ASSERT_EQ((INT)at(150, 298), 0x00FF00FF);	/* five wide */
	KT_ASSERT_EQ((INT)at(150, 295), 0x00202020);

	wipe(0x00202020U);
	KT_ASSERT_ER(dp_line_wide(gid, 100, 320, 200, 320, 1, DP_LINE_DOT, &pat),
		     E_OK);
	for ( x = 100; x <= 200; x++ ) {
		if ( at(x, 320) == 0x00FF00FF ) on++;
		if ( at(x, 320) == 0x00202020 ) off++;
	}
	KT_ASSERT(on > 20);
	KT_ASSERT(off > 20);			/* dotted, not solid */
	wipe(0x00000000U);
}

/* a seeded fill spreads to the edge of what it started on */
LOCAL void test_seed( void )
{
	T_DPRECT	r;
	T_DPPAT		pat;

	if ( !have_dp ) KT_SKIP("no screen");

	wipe(0x00202020U);
	dp_pat_colour(&pat, 0x00FFFFFFU);
	r.left = 100;  r.top = 400;  r.right = 200;  r.bottom = 460;
	KT_ASSERT_ER(dp_frame_rect(gid, &r, 0x00FF0000U, 2), E_OK);

	dp_pat_colour(&pat, 0x0000FF00U);
	KT_ASSERT_ER(dp_fill_seed(gid, 150, 430, &pat), E_OK);

	KT_ASSERT_EQ((INT)at(150, 430), 0x0000FF00);	/* inside */
	KT_ASSERT_EQ((INT)at(105, 405), 0x0000FF00);
	KT_ASSERT_EQ((INT)at(100, 400), 0x00FF0000);	/* the wall stands */
	KT_ASSERT_EQ((INT)at(210, 430), 0x00202020);	/* it did not get out */
	wipe(0x00000000U);
}

EXPORT void ktest_dp( void )
{
	KT_RUN(test_open);
	KT_RUN(test_fill);
	KT_RUN(test_origin);
	KT_RUN(test_frame);
	KT_RUN(test_frame_rect);
	KT_RUN(test_line);
	KT_RUN(test_mode);
	KT_RUN(test_copy);
	KT_RUN(test_hold);
	KT_RUN(test_tone);
	KT_RUN(test_round);
	KT_RUN(test_region);
	KT_RUN(test_region_draw);
	KT_RUN(test_poly);
	KT_RUN(test_sector);
	KT_RUN(test_wide_line);
	KT_RUN(test_seed);
}
