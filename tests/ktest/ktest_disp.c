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
#include <ts/dispout.h>
#include <ts/wm.h>
#include <ts/time.h>
#include <tk/smem.h>
#include "../../device/disp/disp_hw.h"

LOCAL BOOL		have_disp = FALSE;
LOCAL T_DISPSPEC	spec;
LOCAL UB		*fb_all = NULL;	/* the framebuffer, every page of it, mapped to be read */
LOCAL UB		*fb = NULL;	/* the page shown */
LOCAL BOOL		flip_was = FALSE;	/* the screen flipped before the suite turned it off */

/* The first byte of a page */
LOCAL UB *page_base( UINT page )
{
	return fb_all + (UBINT)page * spec.fb_pitch * spec.main_h;
}

/* One pixel of the back buffer */
LOCAL UW *back_at( INT x, INT y )
{
	UB	*p = (UB *)ts_disp_buffer();

	return (UW *)(p + (UBINT)y * spec.pitch + (UBINT)x * 4);
}

/*
 * The same pixel as a page of the screen holds it, put back into the back
 * buffer's form: blue lowest, and nothing in the top byte, which the
 * send may have set for a screen that reads it as opacity.
 */
LOCAL UW page_at( UB *base, INT x, INT y )
{
	UB	*p = base + (UBINT)y * spec.fb_pitch + (UBINT)x * ( spec.fb_bpp / 8 );
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

/* The same, of the page the screen shows */
LOCAL UW screen_at( INT x, INT y )
{
	return page_at(fb, x, y);
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

	/* the suite reads the page sends go to, which with flipping is not the one shown */
	if ( spec.flipping ) {
		flip_was = TRUE;
		KT_ASSERT_ER(ts_disp_flip(FALSE), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
		KT_ASSERT(!spec.flipping);
	}
	KT_ASSERT(spec.fb_pages == 1 || spec.fb_pages == 2);
	KT_ASSERT(spec.fb_shown < spec.fb_pages);

	KT_ASSERT(spec.width >= 320);
	KT_ASSERT(spec.height >= 200);
	KT_ASSERT_EQ(spec.bpp, 32);
	KT_ASSERT_EQ(spec.format, DISP_FMT_XRGB8888);
	KT_ASSERT(spec.pitch >= spec.width * 4);
	KT_ASSERT(spec.fb_pa != 0);
	KT_ASSERT(spec.fb_format == DISP_FMT_XRGB8888 || spec.fb_format == DISP_FMT_XBGR8888
		  || spec.fb_format == DISP_FMT_RGB565);
	KT_ASSERT(spec.main_w >= 320 && spec.main_w <= spec.width);
	KT_ASSERT(spec.main_h >= 200 && spec.main_h <= spec.height);
	KT_ASSERT(spec.fb_pitch >= spec.main_w * ( spec.fb_bpp / 8 ));
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
	fb_all = (UB *)knl_vmap_pa(spec.fb_pa & ~(UD)4095,
				   ((spec.fb_pa & 4095) + (UD)spec.fb_pitch * spec.main_h * spec.fb_pages
				    + 4095) / 4096,
				   VMAP_NOCACHE);
	KT_ASSERT(fb_all != NULL);
	fb_all += spec.fb_pa & 4095;
	fb = page_base(spec.fb_shown);
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
#if KT_BOARD
	tk_dly_tsk(5000);
#endif
}

/*
 * Two pages. Each send goes to the page not shown, with what the send
 * before it wrote, and that page is then shown: after every send the
 * page shown holds what the back buffer does over what that send and
 * the one before it wrote, and along some whole rows. Two sends of two
 * rectangles far apart show the second page bringing in the first
 * rectangle, which only the other page was sent. A flip on this screen
 * is taken at once, so no send waits. Turned off again, sends go to the
 * page last shown, and the screen is left as the suite found it.
 */
#define FLIP_SENDS	20

LOCAL UW	flip_seed;

LOCAL UW flip_rand( void )
{
	flip_seed = flip_seed * 1103515245U + 12345U;
	return flip_seed >> 8;
}

/* How many pixels of r the page shown has otherwise than the back buffer */
LOCAL INT shown_differs( CONST T_DISPRECT *r )
{
	INT	x, y, n = 0;

	for ( y = r->top; y < r->bottom; y++ ) {
		for ( x = r->left; x < r->right; x++ ) {
			if ( screen_at(x, y) != as_sent(*back_at(x, y)) ) {
				if ( n == 0 ) {
					tm_printf((UB*)"  (%d,%d) on page %d: %06x, meant %06x\n", x, y,
						  (INT)spec.fb_shown, screen_at(x, y), as_sent(*back_at(x, y)));
				}
				n++;
			}
		}
	}
	return n;
}

LOCAL void block( CONST T_DISPRECT *r, UW c )
{
	INT	x, y;

	for ( y = r->top; y < r->bottom; y++ ) {
		for ( x = r->left; x < r->right; x++ ) {
			*back_at(x, y) = c;
		}
	}
}

LOCAL void test_flip( void )
{
	T_DISPSTAT	before, after;
	T_DISPRECT	r[3], was[3], a, b, row;
	INT		k, i, n, nw = 0, w, h;
	UINT		page;
	ER		er;

	if ( !have_disp ) KT_SKIP("no screen");
	er = ts_disp_flip(TRUE);
	if ( er == E_NOSPT ) KT_SKIP("the screen has one page");
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	KT_ASSERT(spec.flipping);
	KT_ASSERT_EQ(spec.fb_pages, 2);
	w = (INT)spec.main_w;
	h = (INT)spec.main_h;

	/* the whole screen once, to the other page, which is then shown */
	page = spec.fb_shown;
	KT_ASSERT_ER(ts_disp_stat(&before), E_OK);
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	KT_ASSERT_EQ(spec.fb_shown, 1 - page);
	fb = page_base(spec.fb_shown);
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.flips, before.flips + 1);

	/* a rectangle sent to one page reaches the other with the next send */
	a.left = 10;  a.top = 10;  a.right = 40;  a.bottom = 30;
	b.left = w - 40;  b.top = h - 30;  b.right = w - 10;  b.bottom = h - 10;
	block(&a, 0x00336699);
	KT_ASSERT_ER(ts_disp_damage(&a), E_OK);
	block(&b, 0x00996633);
	KT_ASSERT_ER(ts_disp_damage(&b), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	fb = page_base(spec.fb_shown);
	KT_ASSERT_EQ(spec.fb_shown, 1 - page);	/* two sends: back where it was */
	KT_ASSERT_EQ(shown_differs(&a), 0);
	KT_ASSERT_EQ(shown_differs(&b), 0);
	KT_ASSERT_EQ(page_at(page_base(page), 20, 20), as_sent(0x336699));
	KT_ASSERT_EQ(page_at(page_base(page), w - 20, h - 20), as_sent(0));

	/* sends of one to three rectangles, of new colours, each under one hold */
	flip_seed = 0x2545F491U;
	KT_ASSERT_ER(ts_disp_stat(&before), E_OK);
	for ( k = 0; k < FLIP_SENDS; k++ ) {
		page = spec.fb_shown;
		n = 1 + (INT)( flip_rand() % 3 );
		KT_ASSERT_ER(ts_disp_hold(), E_OK);
		for ( i = 0; i < n; i++ ) {
			INT	rw = 1 + (INT)( flip_rand() % (UW)( w / 8 ) );
			INT	rh = 1 + (INT)( flip_rand() % (UW)( h / 8 ) );

			r[i].left = (INT)( flip_rand() % (UW)( w - rw + 1 ) );
			r[i].top = (INT)( flip_rand() % (UW)( h - rh + 1 ) );
			r[i].right = r[i].left + rw;
			r[i].bottom = r[i].top + rh;
			block(&r[i], flip_rand() & 0x00FFFFFFU);
			KT_ASSERT_ER(ts_disp_damage(&r[i]), E_OK);
		}
		KT_ASSERT_ER(ts_disp_release(), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
		KT_ASSERT_EQ(spec.fb_shown, 1 - page);
		fb = page_base(spec.fb_shown);
		for ( i = 0; i < n; i++ ) {
			KT_ASSERT_EQ(shown_differs(&r[i]), 0);
		}
		for ( i = 0; i < nw; i++ ) {
			KT_ASSERT_EQ(shown_differs(&was[i]), 0);
		}
		row.left = 0;  row.right = w;
		row.top = (INT)( flip_rand() % (UW)h );  row.bottom = row.top + 1;
		KT_ASSERT_EQ(shown_differs(&row), 0);
		for ( i = 0; i < n; i++ ) {
			was[i] = r[i];
		}
		nw = n;
	}
	KT_ASSERT_ER(ts_disp_stat(&after), E_OK);
	KT_ASSERT_EQ(after.flips, before.flips + FLIP_SENDS);
	KT_ASSERT_EQ(after.sends, before.sends + FLIP_SENDS);
#if !KT_BOARD
	KT_ASSERT_EQ(after.waits, before.waits);	/* taken at once */
#endif
	tm_printf((UB*)"  %d sends to the page not shown, %ld kpx copied (with what the page lacked), "
		  "%ld waited for a flip, %ld us all told\n",
		  FLIP_SENDS, (D)( ( after.pixels - before.pixels ) / 1000 ),
		  (D)( after.waits - before.waits ), (D)( after.wait_us - before.wait_us ));

	/* off: sends go to the page last shown */
	page = spec.fb_shown;
	KT_ASSERT_ER(ts_disp_flip(FALSE), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	KT_ASSERT(!spec.flipping);
	KT_ASSERT_EQ(spec.fb_shown, page);
	block(&a, 0x00123456);
	KT_ASSERT_ER(ts_disp_damage(&a), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&spec), E_OK);
	KT_ASSERT_EQ(spec.fb_shown, page);
	KT_ASSERT_EQ(screen_at(20, 20), as_sent(0x123456));

	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
	if ( flip_was ) {
		KT_ASSERT_ER(ts_disp_flip(TRUE), E_OK);
	}
}

/*
 * The blanks as a clock (design 10.4.5), on a refresh made up for a
 * screen that has none (knl_disp_sim_blank): a send right after a flip
 * waits for the blank that takes it and no longer, and its flip comes
 * in the first quarter of the refresh; a send that comes after the
 * blank that took the flip before, though within a refresh of it, goes
 * on without waiting, where watching the screen would wait a whole
 * refresh more; and sends in a row come one a refresh, asleep until
 * shortly before each blank.
 */
#define BL_US		16667
#define BL_ROW		30

LOCAL void bl_send( INT k )
{
	T_DISPRECT	r;

	r.left = 20 + k % 40;  r.top = 20;  r.right = r.left + 16;  r.bottom = 36;
	block(&r, 0x00102030U + (UW)k);
	(void)ts_disp_damage(&r);
}

LOCAL void test_blank( void )
{
	T_DISPFRAME	f0, f1;
	T_DISPSTAT	s0, s1;
	UD		t0, t1, tc;
	D		since;
	INT		k;
	ER		er;

	if ( !have_disp ) KT_SKIP("no screen");
	er = ts_disp_flip(TRUE);
	if ( er == E_NOSPT ) KT_SKIP("the screen has one page");
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_ER(knl_disp_sim_blank(BL_US), E_OK);
	KT_ASSERT_EQ(ts_disp_paced(), BL_US);
	bl_send(0);

	/* right after a flip: until the blank that takes it */
	KT_ASSERT_ER(ts_disp_frames(&f0), E_OK);
	KT_ASSERT_EQ(f0.frame_ns, (UD)BL_US * 1000);
	(void)ts_get_mono(&t0);
	bl_send(1);
	(void)ts_get_mono(&t1);
	KT_ASSERT_ER(ts_disp_frames(&f1), E_OK);
	KT_ASSERT_EQ(f1.waited, f0.waited + 1);
	KT_ASSERT_EQ(f1.taken, f0.taken);
	KT_ASSERT(( t1 - t0 ) / 1000 <= (UD)BL_US + 3000);
	KT_ASSERT_EQ(f1.phase[0], f0.phase[0] + 1);
	KT_ASSERT_EQ(f1.off, f0.off);
	tm_printf((UB *)"  a send right after a flip: %ld us, the flip in quarter %d of the refresh\n",
		  (D)( ( t1 - t0 ) / 1000 ), ( f1.phase[0] > f0.phase[0] ) ? 1 : ( f1.phase[1] > f0.phase[1] ) ? 2 : 3);

	/*
	 * After the blank that took the flip before: the last send's flip
	 * came just after a blank, and the next send's waits for the end of
	 * the next blank, a refresh on; a send 17.8 ms after that flip is
	 * past the blank that took it and still within a refresh and an
	 * eighth of it, where the flip would otherwise be taken to be due
	 */
	bl_send(2);
	(void)ts_get_mono(&tc);
	(void)tk_dly_tsk_u(17800);
	KT_ASSERT_ER(ts_disp_frames(&f0), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&s0), E_OK);
	(void)ts_get_mono(&t0);
	bl_send(3);
	(void)ts_get_mono(&t1);
	KT_ASSERT_ER(ts_disp_frames(&f1), E_OK);
	KT_ASSERT_ER(ts_disp_stat(&s1), E_OK);
	since = (D)( ( t0 - tc ) / 1000 );
	if ( since >= 17100 && since <= 18500 ) {
		KT_ASSERT_EQ(f1.taken, f0.taken + 1);
		KT_ASSERT_EQ(f1.waited, f0.waited);
		KT_ASSERT_EQ(s1.waits, s0.waits);
	}
	tm_printf((UB *)"  a send %ld us after a flip, past its blank: %ld us, %s\n", since,
		  (D)( ( t1 - t0 ) / 1000 ), ( f1.taken > f0.taken ) ? "taken by the clock" : "waited");

	/* in a row: one a refresh, asleep before each blank */
	KT_ASSERT_ER(ts_disp_frames(&f0), E_OK);
	(void)ts_get_mono(&t0);
	for ( k = 0; k < BL_ROW; k++ ) {
		bl_send(4 + k);
	}
	(void)ts_get_mono(&t1);
	KT_ASSERT_ER(ts_disp_frames(&f1), E_OK);
	KT_ASSERT(f1.apart[1] - f0.apart[1] >= BL_ROW - 2);
	KT_ASSERT_EQ(f1.apart[0], f0.apart[0]);
	KT_ASSERT(f1.sleep_us > f0.sleep_us);
	KT_ASSERT(( t1 - t0 ) / 1000 >= (UD)( BL_ROW - 1 ) * BL_US);
	KT_ASSERT(( t1 - t0 ) / 1000 <= (UD)( BL_ROW + 2 ) * BL_US);
	KT_ASSERT_EQ(f1.off, f0.off);
	tm_printf((UB *)"  %d sends in a row: %ld us each; refreshes apart 1:%ld 2:%ld 3+:%ld; "
		  "asleep %ld us, watching %ld us each\n", BL_ROW, (D)( ( t1 - t0 ) / 1000 / BL_ROW ),
		  (D)( f1.apart[1] - f0.apart[1] ), (D)( f1.apart[2] - f0.apart[2] ),
		  (D)( f1.apart[3] + f1.apart[4] - f0.apart[3] - f0.apart[4] ),
		  (D)( ( f1.sleep_us - f0.sleep_us ) / BL_ROW ), (D)( ( f1.spin_us - f0.spin_us ) / BL_ROW ));

	KT_ASSERT_ER(knl_disp_sim_blank(0), E_OK);
	if ( !flip_was ) {
		KT_ASSERT_ER(ts_disp_flip(FALSE), E_OK);
		KT_ASSERT_EQ(ts_disp_paced(), 0U);
	}
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * Screens of their own sizes (10.4.8): framebuffers in memory put right
 * of and below the screens there are, as an output a driver found would
 * be. The back buffer grows round them, each screen's rectangle is its
 * own, and what changes on a screen reaches its framebuffer at its place
 * in it, a rectangle across two screens to both.
 */
#define SC_W		320
#define SC_H		240
#define SC_DOWN		16		/* the screen right of the others starts this far down */


/* A framebuffer in memory, w x h, as an output; NULL when there is no room */
LOCAL UW *mem_out( T_DISPHW *hw, UINT w, UINT h )
{
	UBINT	pages = ( (UBINT)w * h * 4 + 4095 ) / 4096;
	UW	*m = (UW *)knl_vmap(pages, 0);

	if ( m == NULL ) return NULL;
	knl_memset(m, 0, (SZ)( pages * 4096 ));
	knl_memset(hw, 0, sizeof(*hw));
	hw->width = w;
	hw->height = h;
	hw->bpp = 32;
	hw->pitch = w * 4;
	hw->format = DISP_FMT_XRGB8888;
	hw->alpha = DISP_ALPHA_ANY;
	hw->fb_va = (UBINT)m;
	hw->via = "memory (test)";
	hw->pages = 1;
	return m;
}

LOCAL void test_screens( void )
{
	T_DISPHW	hw;
	T_DISPSPEC	was, s;
	T_DISPRECT	r;
	T_DPRECT	on;
	UW		*m1, *m2;
	INT		n0, sc1, sc2, x, y, w, h, bad;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&was), E_OK);
	n0 = ts_disp_screens();
	if ( n0 > 1 && was.width == was.main_w && was.height == was.main_h ) {
		KT_SKIP("the screen is shown as columns (screens=N)");
	}
	m1 = mem_out(&hw, SC_W, SC_H);
	KT_ASSERT(m1 != NULL);
	if ( m1 == NULL ) return;

	/* a place left of or above the main screen, a side not known, a framebuffer that cannot be sent to */
	KT_ASSERT_ER(knl_disp_out_attach(&hw, DISP_SIDE_RIGHT, -1), E_NOSPT);
	KT_ASSERT_ER(knl_disp_out_attach(&hw, 7, 0), E_PAR);
	hw.bpp = 24;
	KT_ASSERT_ER(knl_disp_out_attach(&hw, DISP_SIDE_RIGHT, 0), E_PAR);
	hw.bpp = 32;
	KT_ASSERT_EQ(ts_disp_screens(), n0);

	/* right of the others, a little down */
	sc1 = knl_disp_out_attach(&hw, DISP_SIDE_RIGHT, SC_DOWN);
	KT_ASSERT_EQ(sc1, n0);
	KT_ASSERT_EQ(ts_disp_screens(), n0 + 1);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.width, was.width + SC_W);
	KT_ASSERT_EQ(s.height, ( was.height > SC_DOWN + SC_H ) ? was.height : SC_DOWN + SC_H);
	KT_ASSERT_EQ(s.main_w, was.main_w);
	KT_ASSERT_EQ(s.main_h, was.main_h);
	KT_ASSERT(s.pitch >= s.width * 4);
	KT_ASSERT_ER(ts_disp_screen(sc1, &x, &y, &w, &h), E_OK);
	if ( s.scale == 100 ) {
		KT_ASSERT_EQ(x, (INT)was.width);
		KT_ASSERT_EQ(y, SC_DOWN);
		KT_ASSERT_EQ(w, SC_W);
		KT_ASSERT_EQ(h, SC_H);
	}
	KT_ASSERT_ER(ts_disp_screen(sc1 + 1, &x, &y, &w, &h), E_NOEXS);
	tm_printf((UB *)"  a screen of %d x %d put right: the back buffer %d x %d (was %d x %d), screen %d at %d,%d %dx%d\n",
		  SC_W, SC_H, (INT)s.width, (INT)s.height, (INT)was.width, (INT)was.height, sc1, x, y, w, h);

	/* the window manager's screen of a point: by both its coordinates */
	(void)ts_disp_screen(sc1, &x, &y, &w, &h);
	wm_screen_at(x + w / 2, y + h / 2, &on);
	KT_ASSERT_EQ(on.left, x);
	KT_ASSERT_EQ(on.top, y);
	KT_ASSERT_EQ(on.right - on.left, w);
	wm_screen_at(x + w + 100, y + 2, &on);		/* past the right edge: the nearest */
	KT_ASSERT_EQ(on.left, x);
	wm_screen_at(10, 10, &on);
	KT_ASSERT_EQ(on.left, 0);
	KT_ASSERT_EQ(on.top, 0);

	/* a rectangle across the main screen's right edge and into the new one */
	(void)wm_flush();
	for ( y = SC_DOWN + 5; y < SC_DOWN + 15; y++ ) {
		for ( x = (INT)was.width - 10; x < (INT)was.width + 20; x++ ) {
			*(UW *)( (UB *)ts_disp_buffer() + (UBINT)y * s.pitch + (UBINT)x * 4 ) = 0x00ABCDEF;
		}
	}
	spec = s;
	r.left = (INT)was.width - 10; r.top = SC_DOWN + 5; r.right = (INT)was.width + 20; r.bottom = SC_DOWN + 15;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	bad = 0;
	for ( y = 5; y < 15; y++ ) {
		for ( x = 0; x < 20; x++ ) {
			if ( ( m1[y * SC_W + x] & 0x00FFFFFF ) != 0x00ABCDEF ) bad++;
		}
	}
	KT_ASSERT_EQ(bad, 0);
	/* round it, the screen holds what the back buffer does (the desk the windows were laid on) */
	KT_ASSERT_EQ(m1[5 * SC_W + 20] & 0x00FFFFFF,
		     *(UW *)( (UB *)ts_disp_buffer() + (UBINT)( SC_DOWN + 5 ) * s.pitch + (UBINT)( was.width + 20 ) * 4 ) & 0x00FFFFFF);
	KT_ASSERT_EQ(m1[4 * SC_W + 0] & 0x00FFFFFF,
		     *(UW *)( (UB *)ts_disp_buffer() + (UBINT)( SC_DOWN + 4 ) * s.pitch + (UBINT)was.width * 4 ) & 0x00FFFFFF);
	KT_ASSERT(( m1[5 * SC_W + 20] & 0x00FFFFFF ) != 0x00ABCDEF);
	if ( n0 == 1 ) {
		KT_ASSERT_EQ(screen_at((INT)was.main_w - 5, SC_DOWN + 6), as_sent(0xABCDEF));
	}

	/* below them all */
	m2 = mem_out(&hw, SC_W, SC_H);
	KT_ASSERT(m2 != NULL);
	if ( m2 != NULL ) {
		sc2 = knl_disp_out_attach(&hw, DISP_SIDE_BELOW, 8);
		KT_ASSERT_EQ(sc2, n0 + 1);
		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		KT_ASSERT_EQ(s.height, ( ( was.height > SC_DOWN + SC_H ) ? was.height : SC_DOWN + SC_H ) + SC_H);
		KT_ASSERT_ER(ts_disp_screen(sc2, &x, &y, &w, &h), E_OK);
		wm_screen_at(x + 1, y + 1, &on);
		KT_ASSERT_EQ(on.top, y);
		KT_ASSERT_EQ(on.left, x);
		if ( s.scale == 100 ) {
			KT_ASSERT_EQ(x, 8);
			KT_ASSERT_EQ(w, SC_W);
		}
		(void)wm_flush();
		{
			INT	px = disp_dev_of(x, s.scale), py = disp_dev_of(y, s.scale);

			*(UW *)( (UB *)ts_disp_buffer() + (UBINT)( py + 3 ) * s.pitch + (UBINT)( px + 4 ) * 4 ) = 0x00135791;
			r.left = px + 4; r.top = py + 3; r.right = px + 5; r.bottom = py + 4;
			KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
			KT_ASSERT_EQ(m2[3 * SC_W + 4] & 0x00FFFFFF, 0x00135791U);
		}
		KT_ASSERT_ER(knl_disp_out_detach(sc2), E_OK);
		knl_vunmap((UB *)m2, ( SC_W * SC_H * 4 + 4095 ) / 4096);
	}

	/* taken away again: the back buffer as it was */
	KT_ASSERT_ER(knl_disp_out_detach(n0 + 2), E_NOEXS);
	KT_ASSERT_ER(knl_disp_out_detach(sc1), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.width, was.width);
	KT_ASSERT_EQ(s.height, was.height);
	KT_ASSERT_EQ(s.pitch, was.pitch);
	spec = s;
	knl_vunmap((UB *)m1, ( SC_W * SC_H * 4 + 4095 ) / 4096);
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * Screens left of and above the main one: the view's corner is the top
 * left of them all, so the main screen moves away from it and shows its
 * own rectangle of the back buffer from there; each other screen shows
 * its part; a point of the view goes with the screen it was on, and to
 * the main screen when its own is taken away; taken away, the main screen
 * is at the corner again.
 */
LOCAL void test_screens_left( void )
{
	T_DISPHW	hw;
	T_DISPSPEC	was, s;
	T_DISPRECT	r, on;
	T_DPRECT	m;
	UW		*m1, *m2;
	INT		n0, sc1, sc2, x, y, w, h, mx, my, ox = 0, oy = 0, ow, oh, px, py, ax = 0, ay = 0;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&was), E_OK);
	n0 = ts_disp_screens();
	if ( n0 > 1 && was.width == was.main_w && was.height == was.main_h ) {
		KT_SKIP("the screen is shown as columns (screens=N)");
	}
	if ( n0 + 2 > DISP_SCREENS_MAX ) KT_SKIP("no room for two more screens");
	KT_ASSERT_EQ(was.main_x, 0);
	KT_ASSERT_EQ(was.main_y, 0);
	if ( n0 > 1 ) (void)ts_disp_screen(1, &ox, &oy, &ow, &oh);	/* one there already, right of the main one */
	m1 = mem_out(&hw, SC_W, SC_H);
	KT_ASSERT(m1 != NULL);
	if ( m1 == NULL ) return;

	/* before the main screen's corner along the edge: not taken */
	KT_ASSERT_ER(knl_disp_out_attach(&hw, DISP_SIDE_LEFT, -1), E_NOSPT);
	KT_ASSERT_ER(knl_disp_out_attach(&hw, DISP_SIDE_ABOVE, -1), E_NOSPT);

	/* left of the main screen, a little down */
	sc1 = knl_disp_out_attach(&hw, DISP_SIDE_LEFT, SC_DOWN);
	KT_ASSERT_EQ(sc1, n0);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.width, was.width + SC_W);
	KT_ASSERT_EQ(s.main_x, SC_W);
	KT_ASSERT_EQ(s.main_y, 0);
	KT_ASSERT_EQ(s.main_w, was.main_w);
	KT_ASSERT_ER(ts_disp_screen(0, &mx, &my, &w, &h), E_OK);
	KT_ASSERT_ER(ts_disp_screen(sc1, &x, &y, &w, &h), E_OK);
	KT_ASSERT_EQ(x, 0);
	if ( s.scale == 100 ) {
		KT_ASSERT_EQ(mx, SC_W);
		KT_ASSERT_EQ(my, 0);
		KT_ASSERT_EQ(y, SC_DOWN);
		KT_ASSERT_EQ(w, SC_W);
	}
	if ( n0 > 1 ) {
		/* the one right of the main screen went with it */
		KT_ASSERT_ER(ts_disp_screen(1, &px, &py, &w, &h), E_OK);
		KT_ASSERT_EQ(px, ox + mx);
		KT_ASSERT_EQ(py, oy);
	}
	wm_screen_main_rect(&m);
	KT_ASSERT_EQ(m.left, mx);
	KT_ASSERT_EQ(m.top, my);
	KT_ASSERT(wm_screen_is_main(&m));
	wm_screen_at(x + 2, y + 2, &m);
	KT_ASSERT_EQ(m.left, x);
	KT_ASSERT(!wm_screen_is_main(&m));
	tm_printf((UB *)"  a screen of %d x %d put left: the back buffer %d x %d, the main screen at %d,%d, screen %d at %d,%d\n",
		  SC_W, SC_H, (INT)s.width, (INT)s.height, mx, my, sc1, x, y);

	/* a point of the main screen goes with it */
	px = 10;
	py = 10;
	KT_ASSERT(!knl_disp_follow(&px, &py, &on));
	KT_ASSERT_EQ(px, mx + 10);
	KT_ASSERT_EQ(py, my + 10);
	KT_ASSERT_EQ(on.left, mx);

	/* the main screen shows its own rectangle of the back buffer, the left one its part */
	spec = s;
	(void)wm_flush();
	{
		INT	dmx = disp_dev_of(mx, s.scale), dx = disp_dev_of(x, s.scale), dy = disp_dev_of(y, s.scale);

		*back_at(dmx + 3, 4) = 0x00246801;
		r.left = dmx + 3; r.top = 4; r.right = dmx + 4; r.bottom = 5;
		KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
		KT_ASSERT_EQ(screen_at(3, 4), as_sent(0x246801));
		*back_at(dx + 5, dy + 6) = 0x00135791;
		r.left = dx + 5; r.top = dy + 6; r.right = dx + 6; r.bottom = dy + 7;
		KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
		KT_ASSERT_EQ(m1[6 * SC_W + 5] & 0x00FFFFFF, 0x00135791U);
		KT_ASSERT(screen_at(5, dy + 6) != as_sent(0x135791));
		ax = x + 5;
		ay = y + 6;
	}

	/* above them, after the left one: the main screen moves down as well */
	m2 = mem_out(&hw, SC_W, SC_H);
	KT_ASSERT(m2 != NULL);
	if ( m2 != NULL ) {
		INT	lx = x, ly = y, lx2 = 0, ly2 = 0;

		sc2 = knl_disp_out_attach(&hw, DISP_SIDE_ABOVE, 8);
		KT_ASSERT_EQ(sc2, n0 + 1);
		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		KT_ASSERT_EQ(s.main_x, SC_W);
		KT_ASSERT_EQ(s.main_y, SC_H);
		KT_ASSERT_ER(ts_disp_screen(0, &mx, &my, &w, &h), E_OK);
		KT_ASSERT_ER(ts_disp_screen(sc2, &x, &y, &w, &h), E_OK);
		KT_ASSERT_EQ(y, 0);
		if ( s.scale == 100 ) {
			KT_ASSERT_EQ(x, SC_W + 8);
			KT_ASSERT_EQ(my, SC_H);
		}
		/* the left one went down with the main one; a point on it with it */
		KT_ASSERT_ER(ts_disp_screen(sc1, &lx2, &ly2, &w, &h), E_OK);
		KT_ASSERT_EQ(lx2, lx);
		if ( s.scale == 100 ) KT_ASSERT_EQ(ly2, ly + my);
		px = ax;
		py = ay;
		KT_ASSERT(!knl_disp_follow(&px, &py, &on));
		KT_ASSERT_EQ(px, ax - lx + lx2);
		KT_ASSERT_EQ(py, ay - ly + ly2);
		KT_ASSERT_EQ(on.top, ly2);

		spec = s;
		(void)wm_flush();
		{
			INT	dx = disp_dev_of(x, s.scale), dmx = disp_dev_of(mx, s.scale), dmy = disp_dev_of(my, s.scale);

			*back_at(dx + 4, 3) = 0x00102030;
			r.left = dx + 4; r.top = 3; r.right = dx + 5; r.bottom = 4;
			KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
			KT_ASSERT_EQ(m2[3 * SC_W + 4] & 0x00FFFFFF, 0x00102030U);
			*back_at(dmx + 2, dmy + 2) = 0x00405060;
			r.left = dmx + 2; r.top = dmy + 2; r.right = dmx + 3; r.bottom = dmy + 3;
			KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
			KT_ASSERT_EQ(screen_at(2, 2), as_sent(0x405060));
		}

		/* the one above taken away: a point on it goes to the main screen, as far from its corner */
		ax = x + 4;
		ay = y + 3;
		KT_ASSERT_ER(knl_disp_out_detach(sc2), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		KT_ASSERT_EQ(s.main_y, 0);
		KT_ASSERT_EQ(s.main_x, SC_W);
		KT_ASSERT_ER(ts_disp_screen(0, &mx, &my, &w, &h), E_OK);
		px = ax;
		py = ay;
		KT_ASSERT(knl_disp_follow(&px, &py, &on));
		KT_ASSERT_EQ(px, mx + 4);
		KT_ASSERT_EQ(py, my + 3);
		KT_ASSERT_EQ(on.left, mx);
		KT_ASSERT_EQ(on.top, my);
		knl_vunmap((UB *)m2, ( SC_W * SC_H * 4 + 4095 ) / 4096);
	}

	/* taken away again: the main screen at the corner, the back buffer as it was */
	KT_ASSERT_ER(knl_disp_out_detach(sc1), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.main_x, 0);
	KT_ASSERT_EQ(s.main_y, 0);
	KT_ASSERT_EQ(s.width, was.width);
	KT_ASSERT_EQ(s.height, was.height);
	KT_ASSERT_EQ(s.pitch, was.pitch);
	if ( n0 > 1 ) {
		KT_ASSERT_ER(ts_disp_screen(1, &px, &py, &w, &h), E_OK);
		KT_ASSERT_EQ(px, ox);
		KT_ASSERT_EQ(py, oy);
	}
	spec = s;
	knl_vunmap((UB *)m1, ( SC_W * SC_H * 4 + 4095 ) / 4096);
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * An output that mirrors the main screen (DISP_SIDE_MIRROR): no screen of
 * its own, the view and the back buffer as they were; of the same size it
 * is sent the main screen's pixels as they are, of another it is sent
 * them scaled with the ratio kept, in the middle, black round them
 */
LOCAL void test_mirror( void )
{
	T_DISPHW	hw;
	T_DISPSPEC	was, s;
	T_DISPRECT	r;
	UW		*m;
	INT		n0, out, x, y, mw, mh, dmx, dmy;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&was), E_OK);
	n0 = ts_disp_screens();
	if ( n0 > 1 && was.width == was.main_w && was.height == was.main_h ) {
		KT_SKIP("the screen is shown as columns (screens=N)");
	}
	if ( knl_disp_out_count() + 1 >= DISP_SCREENS_MAX ) KT_SKIP("no room for another output");

	/* the rectangle a picture is shown in: as large as it goes, its ratio kept, in the middle */
	knl_disp_fit(1280, 800, 1920, 1080, &r);
	KT_ASSERT(r.left == 96 && r.top == 0 && r.right == 96 + 1728 && r.bottom == 1080);
	knl_disp_fit(1920, 1080, 1280, 1024, &r);
	KT_ASSERT(r.left == 0 && r.top == 152 && r.right == 1280 && r.bottom == 152 + 720);
	knl_disp_fit(1024, 768, 2048, 1536, &r);
	KT_ASSERT(r.left == 0 && r.top == 0 && r.right == 2048 && r.bottom == 1536);

	mw = (INT)was.main_w;
	mh = (INT)was.main_h;
	dmx = disp_dev_of(was.main_x, was.scale);
	dmy = disp_dev_of(was.main_y, was.scale);

	/* of the main screen's size: no screen of its own, a copy of the main screen */
	m = mem_out(&hw, (UINT)mw, (UINT)mh);
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	out = knl_disp_out_attach(&hw, DISP_SIDE_MIRROR, 0);
	KT_ASSERT_EQ(out, knl_disp_out_count());
	KT_ASSERT_EQ(knl_disp_out_screen(out), -1);
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.width, was.width);
	KT_ASSERT_EQ(s.height, was.height);
	KT_ASSERT_EQ(s.main_x, was.main_x);
	KT_ASSERT_ER(ts_disp_screen(n0, &x, &y, &mw, &mh), E_NOEXS);
	mw = (INT)was.main_w;
	mh = (INT)was.main_h;
	spec = s;
	(void)wm_flush();
	*back_at(dmx + 7, dmy + 9) = 0x00123456;
	r.left = dmx + 7; r.top = dmy + 9; r.right = dmx + 8; r.bottom = dmy + 10;
	KT_ASSERT_ER(ts_disp_damage(&r), E_OK);
	KT_ASSERT_EQ(m[9 * mw + 7] & 0x00FFFFFF, 0x00123456U);
	KT_ASSERT_ER(knl_disp_out_detach(out), E_OK);
	knl_vunmap((UB *)m, ( (UBINT)mw * mh * 4 + 4095 ) / 4096);

	/* of another size, 320 x 240: the main screen scaled into the middle, black above and below */
	m = mem_out(&hw, 320, 240);
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	out = knl_disp_out_attach(&hw, DISP_SIDE_MIRROR, 0);
	KT_ASSERT(out >= 1);
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	knl_disp_fit((UINT)mw, (UINT)mh, 320, 240, &r);
	tm_printf((UB *)"  the main screen of %d x %d mirrored in 320 x 240: in %d,%d-%d,%d\n", mw, mh,
		  r.left, r.top, r.right, r.bottom);
	(void)wm_flush();
	for ( y = 0; y < mh; y++ ) {
		UW	*p = back_at(dmx, dmy + y);

		for ( x = 0; x < mw; x++ ) p[x] = 0x00ABCDEF;
	}
	{
		T_DISPRECT	all;

		all.left = dmx; all.top = dmy; all.right = dmx + mw; all.bottom = dmy + mh;
		KT_ASSERT_ER(ts_disp_damage(&all), E_OK);
	}
	KT_ASSERT_EQ(m[( ( r.top + r.bottom ) / 2 ) * 320 + ( r.left + r.right ) / 2] & 0x00FFFFFF, 0x00ABCDEFU);
	KT_ASSERT_EQ(m[r.top * 320 + r.left] & 0x00FFFFFF, 0x00ABCDEFU);
	KT_ASSERT_EQ(m[( r.bottom - 1 ) * 320 + r.right - 1] & 0x00FFFFFF, 0x00ABCDEFU);
	if ( r.top > 0 ) {
		KT_ASSERT_EQ(m[0], 0U);					/* the bar above */
		KT_ASSERT_EQ(m[239 * 320 + 319], 0U);			/* and below */
	}
	if ( r.left > 0 ) {
		KT_ASSERT_EQ(m[120 * 320], 0U);
	}
	KT_ASSERT_ER(knl_disp_out_detach(out), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	knl_vunmap((UB *)m, ( 320 * 240 * 4 + 4095 ) / 4096);

	/*
	 * The main screen set to another size while mirrored into 320 x 200:
	 * what is scaled from and the rectangle it is scaled into made anew
	 * for the new size (800 x 600 into 320 x 200: bars left and right,
	 * black), and a pixel there the one the new size's sampling takes,
	 * scaled once
	 */
	m = mem_out(&hw, 320, 200);
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	out = knl_disp_out_attach(&hw, DISP_SIDE_MIRROR, 0);
	KT_ASSERT(out >= 1);
	if ( n0 == 1 && ts_disp_setmode(800, 600) >= E_OK ) {
		T_DISPRECT	f, all;
		UD		stx, sty;
		INT		cx, cy, fx, fy;

		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		spec = s;
		knl_disp_fit(800, 600, 320, 200, &f);
		KT_ASSERT(f.left > 0 && f.top == 0);
		(void)wm_flush();
		for ( y = 0; y < 600; y++ ) {
			UW	*p = back_at(0, y);

			for ( x = 0; x < 800; x++ ) p[x] = ( (UW)x << 12 ) ^ (UW)y;	/* every pixel its own */
		}
		all.left = 0; all.top = 0; all.right = 800; all.bottom = 600;
		KT_ASSERT_ER(ts_disp_damage(&all), E_OK);
		stx = ( 800ULL << 16 ) / (UD)( f.right - f.left );
		sty = ( 600ULL << 16 ) / (UD)( f.bottom - f.top );
		cx = ( f.left + f.right ) / 2 + 7;
		cy = ( f.top + f.bottom ) / 2 + 5;
		fx = (INT)( ( (UD)( cx - f.left ) * stx + ( stx >> 1 ) ) >> 16 );
		fy = (INT)( ( (UD)( cy - f.top ) * sty + ( sty >> 1 ) ) >> 16 );
		KT_ASSERT_EQ(m[cy * 320 + cx] & 0x00FFFFFF, ( ( (UW)fx << 12 ) ^ (UW)fy ) & 0x00FFFFFF);
		KT_ASSERT_EQ(m[100 * 320 + f.left - 1], 0U);		/* the bar left */
		KT_ASSERT_EQ(m[100 * 320 + f.right], 0U);		/* and right */
		tm_printf((UB *)"  the main screen set to 800 x 600 while mirrored into 320 x 200: in %d,%d-%d,%d\n",
			  f.left, f.top, f.right, f.bottom);
		KT_ASSERT_ER(ts_disp_setmode(was.main_w, was.main_h), E_OK);
	} else {
		tm_printf((UB *)"  the main screen's size not changed here (two screens, or a screen that keeps its size)\n");
	}
	KT_ASSERT_ER(knl_disp_out_detach(out), E_OK);
	knl_vunmap((UB *)m, ( 320 * 200 * 4 + 4095 ) / 4096);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	spec = s;
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * An output with two pages keeps its own frames: a send goes to its page
 * not shown, which is then shown; a send that comes before that flip has
 * been taken (latch_us here, made long) does not wait for it, and what it
 * brought is sent by the output task once the flip is taken, with what
 * the other page lacked, so that the page shown holds both.
 */
#define OF_LATCH_US	40000

LOCAL UINT	of_page[16];
LOCAL INT	of_n = 0;

LOCAL ER of_flip( UINT page )
{
	if ( of_n < 16 ) of_page[of_n] = page;
	of_n++;
	return E_OK;
}

LOCAL void of_fill( UINT pitch, INT x0, INT y0, INT x1, INT y1, UW c )
{
	INT		x, y;
	T_DISPRECT	r;

	for ( y = y0; y < y1; y++ ) {
		for ( x = x0; x < x1; x++ ) {
			*(UW *)( (UB *)ts_disp_buffer() + (UBINT)y * pitch + (UBINT)x * 4 ) = c;
		}
	}
	r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
	(void)ts_disp_damage(&r);
}

LOCAL void test_out_flip( void )
{
	T_DISPHW	hw;
	T_DISPSPEC	was, s;
	UW		*m, *pg;
	UBINT		pages = ( (UBINT)SC_W * SC_H * 4 * 2 + 4095 ) / 4096;
	INT		sc, n0, x0, last;
	UD		t0, t1;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&was), E_OK);
	if ( ts_disp_screens() > 1 && was.width == was.main_w ) KT_SKIP("the screen is shown as columns (screens=N)");
	m = (UW *)knl_vmap(pages, 0);
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	knl_memset(m, 0, (SZ)( pages * 4096 ));
	knl_memset(&hw, 0, sizeof(hw));
	hw.width = SC_W;
	hw.height = SC_H;
	hw.bpp = 32;
	hw.pitch = SC_W * 4;
	hw.format = DISP_FMT_XRGB8888;
	hw.alpha = DISP_ALPHA_ANY;
	hw.fb_va = (UBINT)m;
	hw.via = "memory, two pages (test)";
	hw.pages = 2;
	hw.flip = of_flip;
	hw.latch_us = OF_LATCH_US;
	of_n = 0;
	sc = knl_disp_out_attach(&hw, DISP_SIDE_RIGHT, 0);
	KT_ASSERT(sc >= 1);
	if ( sc < 1 ) {
		knl_vunmap((UB *)m, pages);
		return;
	}
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	x0 = (INT)was.width;
	(void)wm_flush();
	tk_dly_tsk(200);			/* what the windows laid again sent, flipped and settled */
	n0 = of_n;
	last = ( n0 > 0 ) ? (INT)of_page[( n0 - 1 ) % 16] : 0;

	/* the first send: to the page not shown, then shown, without waiting */
	(void)ts_get_mono(&t0);
	of_fill(s.pitch, x0 + 10, 10, x0 + 30, 20, 0x00A1B2C3);
	(void)ts_get_mono(&t1);
	KT_ASSERT_EQ(of_n, n0 + 1);
	KT_ASSERT_EQ((INT)of_page[n0 % 16], 1 - last);
	pg = m + (SZ)( 1 - last ) * SC_W * SC_H;
	KT_ASSERT_EQ(pg[15 * SC_W + 20] & 0x00FFFFFF, 0x00A1B2C3U);
	KT_ASSERT(( t1 - t0 ) / 1000 < OF_LATCH_US / 2);

	/* the next, before that flip is taken: it does not wait, nor flip */
	(void)ts_get_mono(&t0);
	of_fill(s.pitch, x0 + 50, 10, x0 + 60, 20, 0x00C3B2A1);
	(void)ts_get_mono(&t1);
	KT_ASSERT_EQ(of_n, n0 + 1);
	KT_ASSERT(( t1 - t0 ) / 1000 < OF_LATCH_US / 2);

	/* the output task sends it once the flip is taken: the page shown has both */
	tk_dly_tsk(OF_LATCH_US / 1000 * 3);
	KT_ASSERT_EQ(of_n, n0 + 2);
	KT_ASSERT_EQ((INT)of_page[( n0 + 1 ) % 16], last);
	pg = m + (SZ)last * SC_W * SC_H;
	KT_ASSERT_EQ(pg[15 * SC_W + 55] & 0x00FFFFFF, 0x00C3B2A1U);
	KT_ASSERT_EQ(pg[15 * SC_W + 20] & 0x00FFFFFF, 0x00A1B2C3U);
	tm_printf((UB *)"  an output of two pages: %d flips; a send while its flip was not taken came back in %ld us\n",
		  of_n, (D)( ( t1 - t0 ) / 1000 ));

	KT_ASSERT_ER(knl_disp_out_detach(sc), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	spec = s;
	knl_vunmap((UB *)m, pages);
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * The outputs (10.4.9): each display listed with what its EDID says and
 * the modes it offers, the display's preferred first; a state checked
 * without touching anything, refused when it does not make sense;
 * committed, and put back as it was when an output does not take its
 * part; an output turned off stays listed without a screen.
 */
LOCAL T_DISPHW	fo_hw;

LOCAL INT fo_modes( void *ctx, T_DISPMODE *m, INT max )
{
	INT	k;

	(void)ctx;
	if ( max < 2 ) return 0;
	knl_memset(m, 0, 2 * sizeof(*m));
	for ( k = 0; k < 2; k++ ) {
		m[k].hdisp = m[k].hss = m[k].hse = m[k].htotal = (UH)( k == 0 ? SC_W : 640 );
		m[k].vdisp = m[k].vss = m[k].vse = m[k].vtotal = (UH)( k == 0 ? SC_H : 480 );
		m[k].clock_khz = (UW)m[k].hdisp * m[k].vdisp * 60 / 1000;
		m[k].vrefresh = 60;
		m[k].flags = DISP_MODE_DRIVER | ( k == 0 ? DISP_MODE_PREFERRED : 0 );
	}
	return 2;
}

/* Every mode of output k, in a list taken for them (Kfree it): how many */
LOCAL INT modes_all( INT k, T_DISPMODE **p_m )
{
	INT	n = ts_disp_out_modes(k, NULL, 0), got;

	*p_m = NULL;
	if ( n <= 0 ) return n;
	*p_m = (T_DISPMODE *)Kmalloc(sizeof(T_DISPMODE) * (SZ)n);
	if ( *p_m == NULL ) return E_NOMEM;
	got = ts_disp_out_modes(k, *p_m, n);
	return ( got < n ) ? got : n;
}

/* to made into a copy of from, the entries it held before given back */
LOCAL void st_set( T_DISPSTATE *to, CONST T_DISPSTATE *from )
{
	ts_disp_state_free(to);
	KT_ASSERT_ER(ts_disp_state_copy(to, from), E_OK);
}

/* It takes only its first mode: another is refused, as hardware that cannot drive it would */
LOCAL ER fo_commit( void *ctx, CONST T_DISPMODE *m, T_DISPHW *hw )
{
	(void)ctx;
	if ( m == NULL ) return E_OK;
	if ( m->hdisp != SC_W || m->vdisp != SC_H ) return E_IO;
	*hw = fo_hw;
	return E_OK;
}

LOCAL CONST T_DISPOUTHW	fo_out = {
	DISP_CON_USB, "Test-1", NULL, NULL, fo_modes, NULL, fo_commit,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

LOCAL void test_outputs( void )
{
	T_DISPOUTINFO	oi;
	T_DISPMODE	*md = NULL;
	T_DISPSTATE	st = { 0, NULL }, st2 = { 0, NULL };
	T_DISPSPEC	s;
	UB		*edid;
	INT		n, nm, k, sc, fo;
	UW		*m;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	if ( ts_disp_screens() > 1 && s.width == s.main_w ) KT_SKIP("the screen is shown as columns (screens=N)");
	n = ts_disp_outputs();
	KT_ASSERT(n >= 1);
	for ( k = 0; k < n; k++ ) {
		KT_ASSERT_ER(ts_disp_output(k, &oi), E_OK);
		if ( md != NULL ) Kfree(md);
		nm = modes_all(k, &md);
		tm_printf((UB *)"  output %d: %s, kind %d, screen %d, EDID %d bytes, %s %s %04x, %d x %d mm, %d modes%s%s\n",
			  k, oi.name, (INT)oi.kind, oi.screen, (INT)oi.edid_len, oi.maker, oi.model, (INT)oi.product,
			  (INT)oi.mm_w, (INT)oi.mm_h, nm, oi.hdmi ? ", HDMI" : "", oi.audio ? ", sound" : "");
		KT_ASSERT(oi.name[0] != 0);
		KT_ASSERT(nm >= 1);
		KT_ASSERT_EQ(nm, oi.nmodes);
		if ( nm >= 1 ) {
			INT	j;

			for ( j = 0; j < nm && j < 6; j++ ) {
				tm_printf((UB *)"    %d x %d %d Hz, %d kHz, flags %04x\n", (INT)md[j].hdisp, (INT)md[j].vdisp,
					  (INT)md[j].vrefresh, (INT)md[j].clock_khz, (INT)md[j].flags);
			}
		}
		if ( oi.edid_len >= 128 ) {
			KT_ASSERT_EQ(ts_disp_out_edid(k, NULL, 0), (INT)oi.edid_len);
			edid = (UB *)Kmalloc(oi.edid_len);
			KT_ASSERT(edid != NULL);
			if ( edid != NULL ) {
				KT_ASSERT_EQ(ts_disp_out_edid(k, edid, (INT)oi.edid_len), (INT)oi.edid_len);
				KT_ASSERT(edid[0] == 0x00 && edid[1] == 0xFF);
				Kfree(edid);
			}
			KT_ASSERT(oi.maker[0] >= 'A' && oi.maker[0] <= 'Z');
			KT_ASSERT(( md[0].flags & DISP_MODE_PREFERRED ) != 0);
		}
	}
	KT_ASSERT_ER(ts_disp_output(n, &oi), E_NOEXS);
	KT_ASSERT_ER(ts_disp_output(0, &oi), E_OK);
	KT_ASSERT_EQ(oi.screen, 0);
	KT_ASSERT_EQ((UINT)oi.mode.hdisp, s.main_w);

	/* checked without touching anything */
	KT_ASSERT_ER(ts_disp_state(&st), E_OK);
	KT_ASSERT_EQ(st.n, n);
	KT_ASSERT(st.out[0].on);
	KT_ASSERT_ER(ts_disp_check(&st), E_OK);
	st_set(&st2, &st);
	st2.out[0].on = FALSE;
	KT_ASSERT_ER(ts_disp_check(&st2), E_NOSPT);	/* the main screen stays on */
	st_set(&st2, &st);
	st2.n = n + 1;
	KT_ASSERT_ER(ts_disp_check(&st2), E_PAR);
	st2.n = n;
	st_set(&st2, &st);
	st2.out[0].w = 123;
	KT_ASSERT_ER(ts_disp_check(&st2), E_NOSPT);	/* not a mode it offers */
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.main_w, (UINT)st.out[0].w);
	KT_ASSERT_ER(ts_disp_commit(&st), E_OK);	/* as it is */

	/* the main screen to another of its modes, and back */
	if ( md != NULL ) Kfree(md);
	nm = modes_all(0, &md);
	for ( k = 0; k < nm; k++ ) {
		if ( md[k].hdisp >= 800 && md[k].vdisp >= 600 && md[k].hdisp <= 1600 && md[k].vdisp <= 1200
		  && ( md[k].hdisp != st.out[0].w || md[k].vdisp != st.out[0].h ) ) break;
	}
	if ( k < nm ) {
		st_set(&st2, &st);
		st2.out[0].w = md[k].hdisp;
		st2.out[0].h = md[k].vdisp;
		KT_ASSERT_ER(ts_disp_commit(&st2), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		KT_ASSERT_EQ(s.main_w, (UINT)md[k].hdisp);
		KT_ASSERT_EQ(s.main_h, (UINT)md[k].vdisp);
		KT_ASSERT_ER(ts_disp_commit(&st), E_OK);
		KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
		KT_ASSERT_EQ(s.main_w, (UINT)st.out[0].w);
		tm_printf((UB *)"  the main output to %d x %d and back\n", (INT)md[k].hdisp, (INT)md[k].vdisp);
	}

	/* an output that refuses a mode: the state put back as it was */
	if ( md != NULL ) Kfree(md);
	m = mem_out(&fo_hw, SC_W, SC_H);		/* kept: the output stays listed after the test */
	KT_ASSERT(m != NULL);
	if ( m == NULL ) goto out;
	fo_hw.out = &fo_out;
	sc = knl_disp_out_attach(&fo_hw, DISP_SIDE_RIGHT, 0);
	KT_ASSERT(sc >= 1);
	if ( sc < 1 ) goto out;
	KT_ASSERT_EQ(ts_disp_outputs(), n + 1);
	fo = n;
	KT_ASSERT_ER(ts_disp_output(fo, &oi), E_OK);
	KT_ASSERT_EQ(oi.screen, sc);
	KT_ASSERT_EQ(oi.nmodes, 2);
	ts_disp_state_free(&st);
	KT_ASSERT_ER(ts_disp_state(&st), E_OK);
	KT_ASSERT(st.out[fo].on && st.out[fo].w == SC_W);
	st_set(&st2, &st);
	st2.out[fo].w = 640;
	st2.out[fo].h = 480;
	KT_ASSERT_ER(ts_disp_check(&st2), E_OK);	/* it offers it */
	KT_ASSERT_ER(ts_disp_commit(&st2), E_IO);	/* and does not take it */
	ts_disp_state_free(&st2);
	KT_ASSERT_ER(ts_disp_state(&st2), E_OK);
	KT_ASSERT(st2.out[fo].on && st2.out[fo].w == SC_W && st2.out[fo].h == SC_H);
	KT_ASSERT_EQ(ts_disp_screens(), sc + 1);
	/* off: no screen, still listed */
	st_set(&st2, &st);
	st2.out[fo].on = FALSE;
	KT_ASSERT_ER(ts_disp_commit(&st2), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), sc);
	KT_ASSERT_EQ(ts_disp_outputs(), n + 1);
	KT_ASSERT_ER(ts_disp_output(fo, &oi), E_OK);
	KT_ASSERT_EQ(oi.screen, -1);
	/* on again */
	KT_ASSERT_ER(ts_disp_commit(&st), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), sc + 1);
	KT_ASSERT_ER(ts_disp_commit(&st2), E_OK);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	spec = s;
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
out:
	ts_disp_state_free(&st);
	ts_disp_state_free(&st2);
}

/*
 * More outputs, screens, modes and EDID blocks than the tables of fixed
 * size there were held (4 outputs, 4 screens, 48 modes, 4 EDID blocks):
 * none is cut off. Six screens of memory are put right of the others,
 * each an output of its own, all on in one state that passes the check;
 * an output offers 60 modes of its own and another an EDID of six
 * blocks, whose last extension's modes are among those it offers.
 */
#define MN_SCREENS	6
#define MN_MODES	60
#define MN_BLOCKS	6

LOCAL INT mn_modes( void *ctx, T_DISPMODE *m, INT max )
{
	INT	k;

	(void)ctx;
	for ( k = 0; k < MN_MODES && k < max; k++ ) {
		knl_memset(&m[k], 0, sizeof(m[k]));
		m[k].hdisp = m[k].hss = m[k].hse = m[k].htotal = (UH)( k == 0 ? SC_W : 1000 + k );
		m[k].vdisp = m[k].vss = m[k].vse = m[k].vtotal = (UH)( k == 0 ? SC_H : 700 );
		m[k].clock_khz = (UW)m[k].hdisp * m[k].vdisp * 60 / 1000;
		m[k].vrefresh = 60;
		m[k].flags = DISP_MODE_DRIVER | ( k == 0 ? DISP_MODE_PREFERRED : 0 );
	}
	return MN_MODES;
}

LOCAL ER mn_commit( void *ctx, CONST T_DISPMODE *m, T_DISPHW *hw )
{
	(void)ctx; (void)hw;
	return ( m == NULL || ( m->hdisp == SC_W && m->vdisp == SC_H ) ) ? E_OK : E_NOSPT;
}

LOCAL INT mn_off( void *ctx )
{
	(void)ctx;
	return DISP_ST_OFF;
}

LOCAL CONST T_DISPOUTHW	mn_out[MN_SCREENS] = {
	{ DISP_CON_USB, "Test-M1", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ DISP_CON_USB, "Test-M2", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ DISP_CON_USB, "Test-M3", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ DISP_CON_USB, "Test-M4", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ DISP_CON_USB, "Test-M5", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ DISP_CON_USB, "Test-M6", NULL, NULL, mn_modes, NULL, mn_commit, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
};

/* A detailed timing descriptor of w x 600 at about 60 Hz */
LOCAL void mn_dtd( UB *d, UINT w )
{
	UINT	ht = w + 160, vt = 600 + 30, clk = ht * vt * 60 / 10000;

	knl_memset(d, 0, 18);
	d[0] = (UB)clk;
	d[1] = (UB)( clk >> 8 );
	d[2] = (UB)w;
	d[3] = 160;
	d[4] = (UB)( ( ( w >> 8 ) << 4 ) | 0 );
	d[5] = (UB)600;
	d[6] = 30;
	d[7] = (UB)( ( 600 >> 8 ) << 4 );
	d[8] = 48;
	d[9] = 32;
	d[10] = ( 3 << 4 ) | 5;
	d[17] = 0x1e;
}

LOCAL void mn_sum( UB *b )
{
	UINT	k, sum = 0;

	for ( k = 0; k < 127; k++ ) sum += b[k];
	b[127] = (UB)( 256 - ( sum & 0xFF ) );
}

/* An EDID of MN_BLOCKS blocks: a mode in the base block, six in each extension, all of other widths */
LOCAL UB	mn_edid[DISP_EDID_BLOCK * MN_BLOCKS];

LOCAL void mn_edid_make( void )
{
	CONST UB	hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	INT		b, k, w = 800;

	knl_memset(mn_edid, 0, sizeof(mn_edid));
	knl_memcpy(mn_edid, hdr, 8);
	mn_edid[8] = 0x50;			/* "TES" */
	mn_edid[9] = 0xB3;
	mn_edid[10] = 0x34;
	mn_edid[18] = 1;
	mn_edid[19] = 4;
	for ( k = 0; k < 8; k++ ) {
		mn_edid[38 + 2 * k] = 0x01;
		mn_edid[39 + 2 * k] = 0x01;
	}
	mn_dtd(mn_edid + 54, (UINT)w);
	w += 16;
	for ( k = 1; k < 4; k++ ) {
		mn_edid[54 + 18 * k + 3] = 0x10;	/* a descriptor of nothing */
	}
	mn_edid[126] = MN_BLOCKS - 1;
	mn_sum(mn_edid);
	for ( b = 1; b < MN_BLOCKS; b++ ) {
		UB	*x = mn_edid + DISP_EDID_BLOCK * b;

		x[0] = 0x02;
		x[1] = 3;
		x[2] = 4;
		for ( k = 0; k < 6; k++ ) {
			mn_dtd(x + 4 + 18 * k, (UINT)w);
			w += 16;
		}
		mn_sum(x);
	}
}

LOCAL INT mn_get_edid( void *ctx, UB *buf, INT max )
{
	INT	n = ( max < (INT)sizeof(mn_edid) ) ? max : (INT)sizeof(mn_edid);

	(void)ctx;
	knl_memcpy(buf, mn_edid, n);
	return n;
}

LOCAL CONST T_DISPOUTHW	mn_edid_out = {
	DISP_CON_HDMI, "Test-E1", mn_off, mn_get_edid, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

LOCAL CONST T_DISPOUTHW	mn_modes_out = {
	DISP_CON_USB, "Test-L1", mn_off, NULL, mn_modes, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

LOCAL void test_many_outputs( void )
{
	T_DISPHW	hw;
	T_DISPSPEC	was, s;
	T_DISPOUTINFO	oi;
	T_DISPSTATE	st = { 0, NULL };
	T_DISPMODE	*md = NULL;
	UW		*mem[MN_SCREENS];
	INT		n0, no0, k, i, sc, nm, put = 0, x, y, w, h;
	UB		*e;
	BOOL		last_seen;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&was), E_OK);
	n0 = ts_disp_screens();
	if ( n0 > 1 && was.width == was.main_w ) KT_SKIP("the screen is shown as columns (screens=N)");
	no0 = ts_disp_outputs();

	/* six screens more, beyond the four there once were room for */
	for ( k = 0; k < MN_SCREENS; k++ ) {
		mem[k] = mem_out(&hw, SC_W, SC_H);
		KT_ASSERT(mem[k] != NULL);
		if ( mem[k] == NULL ) break;
		hw.out = &mn_out[k];
		sc = knl_disp_out_attach(&hw, DISP_SIDE_RIGHT, 0);
		KT_ASSERT_EQ(sc, n0 + k);
		if ( sc < 1 ) {
			knl_vunmap((UB *)mem[k], ( SC_W * SC_H * 4 + 4095 ) / 4096);
			break;
		}
		put++;
	}
	KT_ASSERT_EQ(put, MN_SCREENS);
	KT_ASSERT_EQ(ts_disp_screens(), n0 + put);
	KT_ASSERT(n0 + put > DISP_SCREENS_MAX);
	KT_ASSERT_ER(ts_disp_screen(n0 + put - 1, &x, &y, &w, &h), E_OK);
	if ( was.scale == 100 ) {
		KT_ASSERT_EQ(w, SC_W);
	}
	KT_ASSERT_ER(ts_disp_screen(n0 + put, &x, &y, &w, &h), E_NOEXS);
	KT_ASSERT(ts_disp_outputs() >= no0 + put);
	KT_ASSERT(ts_disp_outputs() > 4);

	/* the state: an entry for every output, all of them on passing the check, set again as it is */
	KT_ASSERT_ER(ts_disp_state(&st), E_OK);
	KT_ASSERT_EQ(st.n, ts_disp_outputs());
	for ( i = 0, k = 0; i < st.n; i++ ) {
		if ( st.out[i].on ) k++;
	}
	KT_ASSERT(k >= put + 1);
	KT_ASSERT_ER(ts_disp_check(&st), E_OK);
	KT_ASSERT_ER(ts_disp_commit(&st), E_OK);
	KT_ASSERT_EQ(ts_disp_screens(), n0 + put);
	ts_disp_state_free(&st);
	tm_printf((UB *)"  %d screens on %d outputs, all of them on in one state\n", ts_disp_screens(), ts_disp_outputs());

	/* every one of them taken away again, the last first */
	for ( k = put; k >= 1; k-- ) {
		KT_ASSERT_ER(knl_disp_out_detach(n0 + k - 1), E_OK);
		knl_vunmap((UB *)mem[k - 1], ( SC_W * SC_H * 4 + 4095 ) / 4096);
	}
	KT_ASSERT_EQ(ts_disp_screens(), n0);
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	KT_ASSERT_EQ(s.width, was.width);
	spec = s;

	/* an output that offers 60 modes of its own: all of them */
	KT_ASSERT_ER(knl_disp_out_add(&mn_modes_out), E_OK);
	for ( i = 0; i < ts_disp_outputs() && ts_disp_output(i, &oi) >= E_OK; i++ ) {
		if ( knl_strcmp(oi.name, "Test-L1") == 0 ) break;
	}
	KT_ASSERT(i < ts_disp_outputs());
	if ( i < ts_disp_outputs() ) {
		KT_ASSERT_EQ(oi.nmodes, MN_MODES);
		KT_ASSERT_EQ(ts_disp_out_modes(i, NULL, 0), MN_MODES);
		nm = modes_all(i, &md);
		KT_ASSERT_EQ(nm, MN_MODES);
		if ( nm == MN_MODES ) {
			KT_ASSERT_EQ((INT)md[MN_MODES - 1].hdisp, 1000 + MN_MODES - 1);
		}
		if ( md != NULL ) Kfree(md);
		md = NULL;
	}

	/* an output whose EDID is six blocks: all of it, and the modes of its last extension */
	mn_edid_make();
	KT_ASSERT_ER(knl_disp_out_add(&mn_edid_out), E_OK);
	for ( i = 0; i < ts_disp_outputs() && ts_disp_output(i, &oi) >= E_OK; i++ ) {
		if ( knl_strcmp(oi.name, "Test-E1") == 0 ) break;
	}
	KT_ASSERT(i < ts_disp_outputs());
	if ( i < ts_disp_outputs() ) {
		KT_ASSERT_EQ((INT)oi.edid_len, DISP_EDID_BLOCK * MN_BLOCKS);
		KT_ASSERT_EQ(ts_disp_out_edid(i, NULL, 0), DISP_EDID_BLOCK * MN_BLOCKS);
		e = (UB *)Kmalloc(DISP_EDID_BLOCK * MN_BLOCKS);
		KT_ASSERT(e != NULL);
		if ( e != NULL ) {
			KT_ASSERT_EQ(ts_disp_out_edid(i, e, DISP_EDID_BLOCK * MN_BLOCKS), DISP_EDID_BLOCK * MN_BLOCKS);
			KT_ASSERT_EQ(e[DISP_EDID_BLOCK * ( MN_BLOCKS - 1 )], 0x02);
			Kfree(e);
		}
		KT_ASSERT_EQ(oi.nmodes, 1 + 6 * ( MN_BLOCKS - 1 ));
		nm = modes_all(i, &md);
		last_seen = FALSE;
		for ( k = 0; k < nm; k++ ) {
			if ( md[k].hdisp == 800 + 16 * ( 6 * ( MN_BLOCKS - 1 ) ) ) last_seen = TRUE;
		}
		KT_ASSERT(last_seen);
		tm_printf((UB *)"  an EDID of %d bytes read whole: %d modes\n", (INT)oi.edid_len, nm);
		if ( md != NULL ) Kfree(md);
	}
	(void)wm_flush();
	KT_ASSERT_ER(ts_disp_clear(0x00000000), E_OK);
}

/*
 * A second display the machine was started with (QEMU's -device
 * bochs-display twice, or the standard VGA and a secondary-vga): it is a
 * screen right of the main one, and what is drawn there reaches it
 */
LOCAL void test_second( void )
{
	T_DISPSPEC	s;
	INT		x, y, w, h;

	if ( !have_disp ) KT_SKIP("no screen");
	KT_ASSERT_ER(ts_disp_ref(&s), E_OK);
	if ( ts_disp_screens() < 2 || s.width == s.main_w ) KT_SKIP("started with one display");
	KT_ASSERT_ER(ts_disp_screen(1, &x, &y, &w, &h), E_OK);
	tm_printf((UB *)"  a second display: screen 1 at %d,%d %dx%d; the back buffer %d x %d, the main screen %d x %d\n",
		  x, y, w, h, (INT)s.width, (INT)s.height, (INT)s.main_w, (INT)s.main_h);
	KT_ASSERT_EQ(y, 0);
	KT_ASSERT(x > 0 && w > 0 && h > 0);
	if ( s.scale == 100 ) {
		KT_ASSERT_EQ((INT)s.width, (INT)s.main_w + w);
		KT_ASSERT_EQ(x, (INT)s.main_w);
	}

	/*
	 * A window across the two screens, for a person to look at: QEMU is
	 * asked to write what each display shows (the second display is the
	 * one given the id disp2), into obj/ beside where it runs
	 */
	{
		T_DPRECT	o, r;
		INT		wid;

		o.left = x - 300;  o.top = 150;  o.right = x + 300;  o.bottom = 450;
		wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "two screens");
		if ( wid >= 1 ) {
			(void)wm_move(wid, &o);		/* across the edge, as one dragged there */
			r.left = 0;  r.top = 0;  r.right = 600;  r.bottom = 300;
			(void)dp_fill_rect(wm_gid(wid), &r, 0x003060C0);
			(void)wm_raise(wid);
			KT_ASSERT_ER(wm_composite(), E_OK);
			tm_printf((UB *)"KTEST QMP {\"execute\": \"screendump\", \"arguments\": {\"filename\": \"obj/kt_screens_head0.ppm\"}}\n");
			tm_printf((UB *)"KTEST QMP {\"execute\": \"screendump\", \"arguments\": {\"filename\": \"obj/kt_screens_head1.ppm\", \"device\": \"disp2\"}}\n");
			tk_dly_tsk(3000);
			KT_ASSERT_ER(wm_close(wid), E_OK);
		}
	}
}

#ifdef X64
#include "../../device/disp/x86_64/disp_pc.h"

IMPORT BOOL knl_dp_link_fits( UW pixel_khz, UINT bpp, UW link_khz, INT lanes );
IMPORT BOOL knl_dp_link_choose( UW pixel_khz, UINT bpp, UW max_khz, INT max_lanes, UW *p_khz, INT *p_lanes );
IMPORT void knl_dp_m_n( UINT bpp, INT lanes, UW pixel_khz, UW link_khz, UW *p_dm, UW *p_dn, UW *p_lm, UW *p_ln );
IMPORT BOOL knl_dp_fallback( UW *p_khz, INT *p_lanes, UW max_khz );
IMPORT UW   knl_igfx_split( UINT wa, UINT wb );

/*
 * What the external output of Intel's 9th generation works out without
 * the hardware (06-device.md 6.9.1), against values found by hand:
 *
 * Watermarks, the PRM's methods 1 and 2 (06-device.md 6.10), latencies
 * 3 5 16 27 42 71 101 126 us. A plane 1920 wide, htotal 2050, 99138 kHz:
 * a line 4 blocks (7680 bytes / 512 = 15, + 1 = 16 a line: 16.0 in 16.16);
 * level 0: method 1 = 3 us x 99138 kHz x 4 / 512 000 = 2.32, 3 + 1 = 4
 * blocks, no lines (80000004); level 1: 3.87 -> 5 < 16 so a line's 16,
 * +1 = 17, 1 line (80004011); and so on to level 7: 126 us, 97.59 against
 * method 2's 7 lines x 16 = 112, the smaller 97.59 -> 99 + 1 = 100, 7
 * lines (8001c064). The cursor 256 wide: its deepest level needs 26
 * blocks, so 32; the plane 892 - 32 = 860 (BUF_CFG 035b0000, CUR 037b035c)
 * -- the table of 6.10. At 165231 kHz (the panel's clock read on the
 * machine) with the same latencies: level 0 3.87 -> 5 blocks, level 7
 * 162.65 -> 165 blocks, 11 lines; the cursor's deepest needs 36, the plane
 * 856. With a second pipe of the same width the buffer is shared at 446
 * (892 x 1920 / 3840); pipe A 0-445, its cursor 36, the plane 410; pipe B
 * of 1920 at 148500 kHz, htotal 2200, 446-891: cursor 30 (more than 8),
 * plane 416. Worked out apart from the C code (scratch Python of the same
 * steps written from the PRM's description), matching the 6.10 table.
 *
 * The link: 1920 x 1080 at 148500 kHz, 24 bits a pixel, 445 500 kB/s; at
 * RBR (162 000 kHz a lane) one lane 162 000, two 324 000, four 648 000:
 * the least is RBR x 4; with two lanes at most, HBR x 2 (540 000). 3840 x
 * 2160 at 297000 kHz (891 000): HBR x 4. M/N of 1080p60 on HBR x 2: data
 * 24 x 148500 = 3 564 000 over 270 000 x 2 x 8 = 4 320 000, N the next
 * power of two 8 388 608 (the most, 0x800000), M = 3 564 000 x 8 388 608 /
 * 4 320 000 = 6 920 601.6 -> 0x699999; link 148 500 / 270 000, N 524 288
 * (0x80000), M 288 358.4 -> 0x46666. The fallback from HBR2 x 4: HBR x 4,
 * RBR x 4, HBR2 x 2, HBR x 2, RBR x 2, HBR2 x 1, HBR x 1, RBR x 1, then none.
 */
LOCAL void test_igfx_calc( void )
{
	CONST UH	lat[8] = { 3, 5, 16, 27, 42, 71, 101, 126 };
	CONST UW	wm1[8] = { 0x80000004, 0x80004011, 0x80004011, 0x80008017, 0x8000c023, 0x80010039, 0x80014051, 0x8001c064 };
	CONST UW	wm2[8] = { 0x80000005, 0x80004011, 0x80008017, 0x8000c025, 0x80010039, 0x8001805e, 0x80024085, 0x8002c0a5 };
	CONST UW	wmb[8] = { 0x80000005, 0x80004011, 0x80008015, 0x80008022, 0x8000c032, 0x80014052, 0x8001c072, 0x80024092 };
	CONST UW	fb_khz[9] = { 270000, 162000, 540000, 270000, 162000, 540000, 270000, 162000, 0 };
	CONST INT	fb_lanes[9] = { 4, 4, 2, 2, 2, 1, 1, 1, 0 };
	T_IGWM		o;
	UW		khz, dm, dn, lm, ln;
	INT		lanes, l, k;

	/* one pipe: the table of 6.10, the plane 860 blocks */
	KT_ASSERT(knl_igfx_wm_calc(lat, 1920, 2050, 99138, 0, IGFX_DBUF_BLOCKS, 1, &o));
	for ( l = 0; l < 8; l++ ) KT_ASSERT_EQ(o.wm[l], wm1[l]);
	KT_ASSERT_EQ(o.cur, 32U);
	KT_ASSERT_EQ(o.share, 860U);
	KT_ASSERT_EQ(o.buf_cfg, 0x035b0000U);
	KT_ASSERT_EQ(o.cur_buf, 0x037b035cU);
	KT_ASSERT(o.sagv);
	KT_ASSERT(knl_igfx_wm_calc(lat, 1920, 2050, 165231, 0, IGFX_DBUF_BLOCKS, 1, &o));
	for ( l = 0; l < 8; l++ ) KT_ASSERT_EQ(o.wm[l], wm2[l]);
	KT_ASSERT_EQ(o.cur, 36U);
	KT_ASSERT_EQ(o.buf_cfg, 0x03570000U);
	KT_ASSERT_EQ(o.cur_buf, 0x037b0358U);

	/* two pipes: the buffer shared by width, SAGV off */
	KT_ASSERT_EQ(knl_igfx_split(1920, 1920), 446U);
	KT_ASSERT_EQ(knl_igfx_split(1920, 2560), 382U);
	KT_ASSERT_EQ(knl_igfx_split(1920, 0), (UW)IGFX_DBUF_BLOCKS);
	KT_ASSERT(knl_igfx_wm_calc(lat, 1920, 2050, 165231, 0, 446, 2, &o));
	KT_ASSERT_EQ(o.cur, 36U);
	KT_ASSERT_EQ(o.share, 410U);
	KT_ASSERT_EQ(o.buf_cfg, 0x01990000U);
	KT_ASSERT_EQ(o.cur_buf, 0x01bd019aU);
	KT_ASSERT(!o.sagv);
	KT_ASSERT(knl_igfx_wm_calc(lat, 1920, 2200, 148500, 446, IGFX_DBUF_BLOCKS, 2, &o));
	for ( l = 0; l < 8; l++ ) KT_ASSERT_EQ(o.wm[l], wmb[l]);
	KT_ASSERT_EQ(o.cur, 30U);
	KT_ASSERT_EQ(o.share, 416U);
	KT_ASSERT_EQ(o.buf_cfg, 0x035d01beU);
	KT_ASSERT_EQ(o.cur_buf, 0x037b035eU);

	/* the link */
	KT_ASSERT(!knl_dp_link_fits(148500, 24, 162000, 2));
	KT_ASSERT(knl_dp_link_fits(148500, 24, 162000, 4));
	KT_ASSERT(knl_dp_link_choose(148500, 24, 540000, 4, &khz, &lanes));
	KT_ASSERT_EQ(khz, 162000U);
	KT_ASSERT_EQ(lanes, 4);
	KT_ASSERT(knl_dp_link_choose(148500, 24, 540000, 2, &khz, &lanes));
	KT_ASSERT_EQ(khz, 270000U);
	KT_ASSERT_EQ(lanes, 2);
	KT_ASSERT(knl_dp_link_choose(297000, 24, 540000, 4, &khz, &lanes));
	KT_ASSERT_EQ(khz, 270000U);
	KT_ASSERT_EQ(lanes, 4);
	KT_ASSERT(!knl_dp_link_choose(594000, 24, 270000, 4, &khz, &lanes));
	knl_dp_m_n(24, 2, 148500, 270000, &dm, &dn, &lm, &ln);
	KT_ASSERT_EQ(dm, 0x699999U);
	KT_ASSERT_EQ(dn, 0x800000U);
	KT_ASSERT_EQ(lm, 0x46666U);
	KT_ASSERT_EQ(ln, 0x80000U);
	khz = 540000;
	lanes = 4;
	for ( k = 0; k < 9; k++ ) {
		BOOL	more = knl_dp_fallback(&khz, &lanes, 540000);

		KT_ASSERT_EQ(more, (BOOL)( fb_khz[k] != 0 ));
		if ( !more ) break;
		KT_ASSERT_EQ(khz, fb_khz[k]);
		KT_ASSERT_EQ(lanes, fb_lanes[k]);
	}
	/*
	 * The DPLL's dividers for TMDS (i915's skl_ddi_calculate_wrpll): 148500 kHz,
	 * the AFE clock 742.5 MHz; the even dividers: 12 gives 8910 MHz, 1% under
	 * 9000 (a deviation of 100, under the 600 allowed below), and none comes
	 * nearer (10: 7425, 14: 10395, and 8910 is 6.1% over 8400, past the 100
	 * allowed above): P 12 = 2 x 3 x 2 (p0 2, q 3, k 2), the DCO 8910 MHz =
	 * 371.25 x 24 MHz: integer 371 (173h), fraction 0.25 x 8000h = 2000h;
	 * CFGCR1 80000000h | 2000h << 9 | 173h = 80400173h; CFGCR2 q 3 << 8 | q
	 * mode 80h | k 1 << 5 | p 1 << 2 | 9000 MHz 1 = 3A5h. 74250 kHz: AFE
	 * 371.25 MHz, divider 24 to 8910 MHz again, 24 = 2 x 6 x 2: CFGCR2 6A5h.
	 */
	{
		IMPORT BOOL knl_skl_wrpll( UW tmds_khz, UW *p_c1, UW *p_c2 );
		UW	c1 = 0, c2 = 0;

		KT_ASSERT(knl_skl_wrpll(148500, &c1, &c2));
		KT_ASSERT_EQ(c1, 0x80400173U);
		KT_ASSERT_EQ(c2, 0x3a5U);
		KT_ASSERT(knl_skl_wrpll(74250, &c1, &c2));
		KT_ASSERT_EQ(c1, 0x80400173U);
		KT_ASSERT_EQ(c2, 0x6a5U);
	}
	/*
	 * The pipe scaler for a mirrored picture of another size: the window
	 * (ratio kept, in the middle), Gen9's limits (8 to 4096 each way, a
	 * downscale under 3, the pixel rate as the scaler reads it within
	 * CDCLK) and that rate, as worked out by hand; the initial phase as
	 * skl_scaler_calc_phase makes it
	 */
	{
		IMPORT ER knl_skl_scaler_check( UINT src_w, UINT src_h, UINT dst_w, UINT dst_h, UW mode_khz, UW cdclk, UW *p_khz );
		IMPORT UW knl_skl_scaler_phase( UW scale );
		UW	khz = 0;

		/* 1920 x 1080 into 1280 x 1024: a window of 1280 x 720, 1.5 down each way: the rate 2.25 times */
		KT_ASSERT_ER(knl_skl_scaler_check(1920, 1080, 1280, 1024, 108000, 675000, &khz), E_OK);
		KT_ASSERT_EQ(khz, 243000U);
		/* 1280 x 800 up into 1920 x 1080: a window of 1728 x 1080, the rate as the mode's */
		KT_ASSERT_ER(knl_skl_scaler_check(1280, 800, 1920, 1080, 148500, 675000, &khz), E_OK);
		KT_ASSERT_EQ(khz, 148500U);
		/* a downscale of 3 is past it, and a source wider than 4096 */
		KT_ASSERT_ER(knl_skl_scaler_check(3840, 2160, 1280, 720, 74250, 675000, &khz), E_NOSPT);
		KT_ASSERT_ER(knl_skl_scaler_check(5120, 1440, 1920, 1080, 148500, 675000, &khz), E_NOSPT);
		/* 2560 x 1600 into 1280 x 800, 2 down each way: 4 times 71 MHz, within 337.5 MHz and not 270 MHz */
		KT_ASSERT_ER(knl_skl_scaler_check(2560, 1600, 1280, 800, 71000, 337500, &khz), E_OK);
		KT_ASSERT_EQ(khz, 284000U);
		KT_ASSERT_ER(knl_skl_scaler_check(2560, 1600, 1280, 800, 71000, 270000, &khz), E_NOSPT);
		KT_ASSERT_EQ(knl_skl_scaler_phase(0x18000), 0x1001U);	/* 1.5 down */
		KT_ASSERT_EQ(knl_skl_scaler_phase(0x10000), 0x0001U);	/* none */
		KT_ASSERT_EQ(knl_skl_scaler_phase(0xAAAA), 0x3554U);	/* 1.5 up */
	}
	tm_printf((UB *)"  the external output's sums: watermarks, the buffer's shares, the link, M/N, the fallback and the TMDS DPLL as worked out by hand\n");
}
#endif

EXPORT void ktest_disp( void )
{
	KT_RUN(test_spec);
	KT_RUN(test_send);
	KT_RUN(test_partial);
	KT_RUN(test_hold);
	KT_RUN(test_clip);
	KT_RUN(test_colours);
	KT_RUN(test_flip);
	KT_RUN(test_blank);
	KT_RUN(test_second);
	KT_RUN(test_screens);
	KT_RUN(test_screens_left);
	KT_RUN(test_mirror);
	KT_RUN(test_out_flip);
	KT_RUN(test_outputs);
	KT_RUN(test_many_outputs);
#ifdef X64
	KT_RUN(test_igfx_calc);
#endif
}
