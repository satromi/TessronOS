/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_disp.c
 *	The screen (design 16.5, stage 9a).
 *
 *	The driver never reads the framebuffer, but a test may: reading the
 *	pixels back is the only way to prove that what was drawn is what
 *	arrived. That is what makes this stage worth doing on the emulated
 *	machine first.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/disp.h>
#include <tk/smem.h>

LOCAL BOOL		have_disp = FALSE;
LOCAL T_DISPSPEC	spec;
LOCAL UB		*fb = NULL;	/* the screen, mapped to be read */

/* One pixel of the back buffer */
LOCAL UW *back_at( INT x, INT y )
{
	UB	*p = (UB *)ts_disp_buffer();

	return (UW *)(p + (UBINT)y * spec.pitch + (UBINT)x * 4);
}

/*
 * The same pixel as the screen holds it, put back into the back
 * buffer's form: blue lowest, and nothing in the top byte, which the
 * send may have set for a screen that reads it as opacity.
 */
LOCAL UW screen_at( INT x, INT y )
{
	UB	*p = fb + (UBINT)y * spec.fb_pitch + (UBINT)x * ( spec.fb_bpp / 8 );
	UW	v;

	if ( spec.fb_format == DISP_FMT_RGB565 ) {
		UH	h = *(volatile UH *)p;

		/* the low bits are lost on the way; they are made up as the send's truncation leaves them */
		return ( (UW)( h & 0xF800 ) << 8 ) | ( (UW)( h & 0x07E0 ) << 5 ) | ( (UW)( h & 0x001F ) << 3 );
	}
	v = *(volatile UW *)p & 0x00FFFFFFU;
	if ( spec.fb_format == DISP_FMT_XBGR8888 ) {
		v = ( v & 0x0000FF00U ) | ( ( v >> 16 ) & 0xFFU ) | ( ( v & 0xFFU ) << 16 );
	}
	return v;
}

/* A colour as the screen can hold it */
LOCAL UW as_sent( UW c )
{
	return ( spec.fb_format == DISP_FMT_RGB565 ) ? ( c & 0x00F8FCF8U ) : ( c & 0x00FFFFFFU );
}

/* the screen is there and says what it is */
LOCAL void test_spec( void )
{
	if ( ts_disp_ref(&spec) < E_OK ) {
		KT_SKIP("the machine has no screen");
	}
	have_disp = TRUE;

	KT_ASSERT(spec.width >= 320);
	KT_ASSERT(spec.height >= 200);
	KT_ASSERT_EQ(spec.bpp, 32);
	KT_ASSERT_EQ(spec.format, DISP_FMT_XRGB8888);
	KT_ASSERT(spec.pitch >= spec.width * 4);
	KT_ASSERT(spec.fb_pa != 0);
	KT_ASSERT(spec.fb_format == DISP_FMT_XRGB8888 || spec.fb_format == DISP_FMT_XBGR8888
		  || spec.fb_format == DISP_FMT_RGB565);
	KT_ASSERT(spec.fb_pitch >= spec.width * ( spec.fb_bpp / 8 ));
	KT_ASSERT_EQ(spec.held, 0);
	KT_ASSERT(ts_disp_buffer() != NULL);

	tm_printf((UB*)"  screen %d x %d, %d bpp, pitch %d, at %lx\n",
		  (INT)spec.width, (INT)spec.height, (INT)spec.fb_bpp,
		  (INT)spec.fb_pitch, spec.fb_pa);

	/*
	 * A second mapping of the same pixels, so the test can read them.
	 * The driver's own mapping is write only by intent. It is not
	 * cached: on the board a line read once would stay in the cache
	 * and hide every later write that went past it to the memory.
	 */
	fb = (UB *)knl_vmap_pa(spec.fb_pa & ~(UD)4095,
			       ((spec.fb_pa & 4095) + (UD)spec.fb_pitch * spec.height + 4095) / 4096,
			       VMAP_NOCACHE);
	if ( fb != NULL ) {
		fb += spec.fb_pa & 4095;
	}
	KT_ASSERT(fb != NULL);
}

/* what is drawn into the back buffer reaches the screen */
LOCAL void test_send( void )
{
	T_DISPRECT	r;
	INT		x, y;

	if ( !have_disp ) KT_SKIP("no screen");

	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);

	/* a small block of a colour nothing else uses */
	for ( y = 10; y < 20; y++ ) {
		for ( x = 30; x < 50; x++ ) {
			*back_at(x, y) = 0x00123456;
		}
	}
	r.left = 30; r.top = 10; r.right = 50; r.bottom = 20;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);

	/* every pixel of it, and the corners just outside it */
	for ( y = 10; y < 20; y++ ) {
		for ( x = 30; x < 50; x++ ) {
			KT_ASSERT_EQ(screen_at(x, y), as_sent(0x123456));
		}
	}
	KT_ASSERT_EQ(screen_at(29, 10), as_sent(0));
	KT_ASSERT_EQ(screen_at(50, 10), as_sent(0));
	KT_ASSERT_EQ(screen_at(30, 9), as_sent(0));
	KT_ASSERT_EQ(screen_at(30, 20), as_sent(0));
}

/*
 * A rectangle that names only part of what changed sends only that
 * part. This is what makes holding worth doing, so it is worth proving
 * that the driver really is bounded by what it is told.
 */
LOCAL void test_partial( void )
{
	T_DISPRECT	r;
	INT		x;

	if ( !have_disp ) KT_SKIP("no screen");

	for ( x = 100; x < 120; x++ ) {
		*back_at(x, 60) = 0x00ABCDEF;
	}
	/* only the left half is declared */
	r.left = 100; r.top = 60; r.right = 110; r.bottom = 61;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);

	for ( x = 100; x < 110; x++ ) {
		KT_ASSERT_EQ(screen_at(x, 60), as_sent(0xABCDEF));
	}
	for ( x = 110; x < 120; x++ ) {
		KT_ASSERT_EQ(screen_at(x, 60), as_sent(0));
	}

	/* and declaring the rest sends the rest */
	r.left = 110; r.right = 120;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	for ( x = 110; x < 120; x++ ) {
		KT_ASSERT_EQ(screen_at(x, 60), as_sent(0xABCDEF));
	}
}

/*
 * Holding the screen gathers what would have been many sends into one.
 * The count is what the design is about, so the count is what is
 * checked, not only the pixels.
 */
LOCAL void test_hold( void )
{
	T_DISPSTAT	before, after;
	T_DISPRECT	r;
	INT		i;

	if ( !have_disp ) KT_SKIP("no screen");

	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&before), E_OK);

	/* twenty changes, each declared, while the screen is held */
	KT_ASSERT_ER(ts_disp_hold(), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	KT_ASSERT_EQ(spec.held, 1);

	for ( i = 0; i < 20; i++ ) {
		*back_at(200 + i, 100) = 0x00FF0000;
		r.left = 200 + i; r.top = 100;
		r.right = 201 + i; r.bottom = 101;
		KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	}

	/* nothing has gone across yet */
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends);
	KT_ASSERT_EQ(screen_at(200, 100), as_sent(0));

	KT_ASSERT_ER(ts_disp_release(), E_OK);

	/* one send covered all twenty */
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends + 1);
	for ( i = 0; i < 20; i++ ) {
		KT_ASSERT_EQ(screen_at(200 + i, 100), as_sent(0xFF0000));
	}

	/* holds nest: only the last release sends */
	KT_ASSERT_ER(ts_disp_stat(&before), E_OK);
	KT_ASSERT_ER(ts_disp_hold(), E_OK);
	KT_ASSERT_ER(ts_disp_hold(), E_OK);
	*back_at(300, 120) = 0x0000FF00;
	r.left = 300; r.top = 120; r.right = 301; r.bottom = 121;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	KT_ASSERT_ER(ts_disp_release(), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends);	/* still held */
	KT_ASSERT_ER(ts_disp_release(), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.sends, before.sends + 1);
	KT_ASSERT_EQ(screen_at(300, 120), as_sent(0x00FF00));
}

/* what is outside the screen is cut away rather than written past it */
LOCAL void test_clip( void )
{
	T_DISPRECT	r;

	if ( !have_disp ) KT_SKIP("no screen");

	/* a rectangle that runs off every edge still works */
	*back_at(0, 0) = 0x00010203;
	r.left = -50; r.top = -50;
	r.right = (INT)spec.width + 50;
	r.bottom = 1;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	KT_ASSERT_EQ(screen_at(0, 0), as_sent(0x010203));

	/* one entirely outside, and one with no area, are refused */
	r.left = 10; r.top = 10; r.right = 10; r.bottom = 20;
	KT_ASSERT_ER(ts_disp_damage(&r), E_PAR);
	r.left = 10; r.top = 20; r.right = 20; r.bottom = 10;
	KT_ASSERT_ER(ts_disp_damage(&r), E_PAR);
	KT_ASSERT_ER(ts_disp_damage(NULL), E_PAR);
}

/*
 * Red, green and blue, for a person to look at. Reading the pixels back
 * proves only that they arrived as this driver meant them; whether the
 * screen takes the bytes in the order the firmware said is seen on the
 * screen and nowhere else (make DISP_SWAP_RB=1 turns it round).
 */
LOCAL CONST UW	band[3] = { 0x00FF0000, 0x0000FF00, 0x000000FF };

LOCAL void test_colours( void )
{
	T_DISPRECT	r;
	INT		i, x, y;

	if ( !have_disp ) KT_SKIP("no screen");

	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
	for ( i = 0; i < 3; i++ ) {
		for ( y = 0; y < 96; y++ ) {
			for ( x = 0; x < 96; x++ ) {
				*back_at(i * 112 + x, y) = band[i];
			}
		}
	}
	r.left = 0; r.top = 0; r.right = 3 * 112; r.bottom = 96;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT_EQ(screen_at(i * 112 + 48, 48), as_sent(band[i]));
	}
	tm_printf((UB*)"  look: three squares at the top left, red, green, blue from the left\n");
#ifdef RPI5
	tk_dly_tsk(5000);
#endif
}

EXPORT void ktest_disp( void )
{
	KT_RUN(test_spec);
	KT_RUN(test_send);
	KT_RUN(test_partial);
	KT_RUN(test_hold);
	KT_RUN(test_clip);
	KT_RUN(test_colours);
}
