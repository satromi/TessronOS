/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_rpi5.c
 *	Screen of the Raspberry Pi 5, and of the Raspberry Pi 4 family, whose
 *	firmware hands over its screen the same way (design 16.5.1).
 *
 *	The display pipeline belongs to the VideoCore firmware. This kernel
 *	does not drive it; it is handed a buffer in memory that the firmware
 *	scans out, and learns where it is in one of two ways:
 *
 *	- the device tree. When the firmware brought a screen up before
 *	  starting the kernel, it describes it in a node compatible with
 *	  "simple-framebuffer" (address, size, stride, format). Taking it
 *	  as it stands changes nothing on the screen and needs no call.
 *	- the property interface of the firmware's mailbox. The kernel asks
 *	  for a size, a depth, a pixel order and whether the top byte is
 *	  read as opacity, and is told where the buffer went and its pitch.
 *
 *	The node is taken first when it is four bytes a pixel and no larger
 *	than CNF_DISP_MAX_W x CNF_DISP_MAX_H: it is what the firmware is
 *	showing already, and its format string says which byte is which,
 *	where the mailbox's pixel order is a number whose sense has to be
 *	taken on trust. Otherwise the mailbox is asked, for the display's
 *	size scaled down to that limit, and a node of two bytes a pixel or
 *	of a larger size is the fallback. With neither there is no screen,
 *	and everything else carries on without one.
 *
 *	The buffer lies in memory the page allocator does not hand out: the
 *	node's range is reserved from the start (pfalloc.c), and a buffer
 *	the mailbox names inside memory in use is refused rather than
 *	written over. It is mapped Normal Non-cacheable (design 16.5.2): the
 *	firmware reads it without looking at this processor's caches, and
 *	nothing here reads it back.
 *
 *	Two pages (the Raspberry Pi 5, CNF_DISP_FLIP). The firmware's buffer
 *	can be asked for twice as tall as the screen (its virtual size) and
 *	told from which row to show it (its virtual offset), so the screen
 *	can be switched between two pages (disp.c). The node's buffer is one
 *	page, so the mailbox is asked for a buffer of two at the node's size
 *	and the node's is left. The firmware is asked first without giving a
 *	buffer, but its answer is only written out: some firmware answers
 *	that test with the size it holds now, and only the buffer it gives
 *	says how many pages it will hold. The firmware is then made
 *	to show the second page and the first again, each read back from
 *	it, and its wait for the next vertical blank is called a few times
 *	and timed: the waits are used only when they come a refresh apart.
 *	A buffer that cannot be used sends the screen back to the node's,
 *	which the firmware may by then no longer show; make DISP_FLIP=0
 *	leaves the node as it is. The Raspberry Pi 4 keeps one page.
 *
 *	Not yet verified on hardware (HW-15, HW-17).
 */

#include <sys/machine.h>

#ifdef RPI_VCFW

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smem.h>
#include <ts/disp.h>
#include <ts/time.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "disp_hw.h"

#define TAG_ALLOCATE_BUFFER	0x00040001U
#define TAG_GET_PHYSICAL_WH	0x00040003U
#define TAG_GET_PITCH		0x00040008U
#define TAG_GET_VIRTUAL_OFFSET	0x00040009U
#define TAG_TEST_VIRTUAL_WH	0x00044004U
#define TAG_SET_PHYSICAL_WH	0x00048003U
#define TAG_SET_VIRTUAL_WH	0x00048004U
#define TAG_SET_DEPTH		0x00048005U
#define TAG_SET_PIXEL_ORDER	0x00048006U
#define TAG_SET_ALPHA_MODE	0x00048007U
#define TAG_SET_VIRTUAL_OFFSET	0x00048009U
#define TAG_WAIT_VSYNC		0x0004800EU	/* answers at the next vertical blank */
#define TAG_ANSWERED		0x80000000U	/* in a tag's size word */

/*
 * The firmware's own two pages (its virtual size and offset) are not
 * used: on the Raspberry Pi 5 it answers that it shows the second page
 * and goes on showing the first. Two pages are made through the
 * display's scaler instead (HVS_PAGES).
 */
#define TWO_PAGES		0
#if defined(RPI5) && CNF_DISP_FLIP
#define HVS_PAGES		1
#else
#define HVS_PAGES		0
#endif

#define ORDER_BGR		0		/* blue lowest */
#define ORDER_RGB		1		/* red lowest */
#define ALPHA_ENABLED		0		/* 0 is opaque */
#define ALPHA_REVERSED		1		/* 0xFF is opaque */
#define ALPHA_IGNORED		2

/* the VideoCore's bus addresses of ARM memory; the low 1GB is all it names */
#define VC_ADDR_MASK		0x3FFFFFFFU

/* when the firmware cannot say how large the display is */
#define DEFAULT_W		1280
#define DEFAULT_H		720

LOCAL UW	mb[64] __attribute__((aligned(64)));

/* ---------------------------------------------------------------- the message */

LOCAL INT	mb_at;

LOCAL void mb_begin( void )
{
	knl_memset(mb, 0, sizeof(mb));
	mb[1] = 0;			/* a request */
	mb_at = 2;
}

/* A tag with room for 'nval' words; answers where its values start */
LOCAL INT mb_tag( UW tag, INT nval, UW v0, UW v1 )
{
	INT	at;

	mb[mb_at++] = tag;
	mb[mb_at++] = (UW)nval * 4;	/* room for the answer */
	mb[mb_at++] = (UW)nval * 4;	/* what is sent */
	at = mb_at;
	mb[mb_at++] = v0;
	if ( nval > 1 ) {
		mb[mb_at++] = v1;
	}
	return at;
}

LOCAL ER mb_call( void )
{
	mb[mb_at++] = 0;		/* the end tag */
	mb[0] = (UW)mb_at * 4;
	return knl_mbox_property(mb);
}

LOCAL BOOL mb_answered( INT at )
{
	return (BOOL)( ( mb[at - 1] & TAG_ANSWERED ) != 0 );
}

/* ---------------------------------------------------------------- common */

/* Whether any page of [pa, pa + size) is memory this kernel hands out */
LOCAL BOOL in_use( UD pa, UD size )
{
	UD	a;

	for ( a = pa & PAGE_MASK; a < pa + size; a += PAGE_SIZE ) {
		PFRAME	*pf = knl_pa_to_pf(a);

		if ( pf != NULL && ( pf->flags & PF_RESERVED ) == 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Map the pixels, every page of them, so that writes may merge on their way out */
LOCAL INT map_fb( T_DISPHW *hw )
{
	UD	size = (UD)hw->pitch * hw->height * ( ( hw->pages > 1 ) ? hw->pages : 1 );
	UD	base = hw->fb_pa & PAGE_MASK;
	UD	npages = ( hw->fb_pa + size - base + PAGE_SIZE - 1 ) / PAGE_SIZE;
	UBINT	va;

	if ( in_use(hw->fb_pa, size) ) {
		tm_printf((UB *)"disp: the framebuffer at %lx lies in memory in use; not taken\n",
			  hw->fb_pa);
		return 0;
	}
	va = (UBINT)knl_vmap_pa(base, npages, VMAP_NOCACHE);
	if ( va == 0 ) {
		return E_NOMEM;
	}
	hw->fb_va = va + (UBINT)( hw->fb_pa - base );

	return 1;
}

/* The pixel order read the other way when the board was built to say so */
LOCAL UINT order_fmt( BOOL red_lowest )
{
	if ( CNF_DISP_SWAP_RB ) {
		red_lowest = (BOOL)!red_lowest;
	}
	return red_lowest ? DISP_FMT_XBGR8888 : DISP_FMT_XRGB8888;
}

/* ---------------------------------------------------------------- the device tree */

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	while ( *a != '\0' && *a == (UB)*b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == (UB)*b );
}

/*
 * The node the firmware left, as a screen. Answers 1 and fills 'hw'
 * without mapping it, or 0 when there is none this driver can send to.
 */
LOCAL INT from_dtb( T_DISPHW *hw )
{
	CONST T_DTBFB	*f = &knl_dtb_fb;

	if ( f->size == 0 ) {
		tm_printf((UB *)"disp: no simple-framebuffer in the device tree\n");
		return 0;
	}
	tm_printf((UB *)"disp: simple-framebuffer in the device tree: %d x %d, stride %d, %s, at %lx\n",
		  (INT)f->width, (INT)f->height, (INT)f->stride, f->format, f->pa);

	hw->width  = f->width;
	hw->height = f->height;
	hw->pitch  = f->stride;
	hw->fb_pa  = f->pa;
	hw->via    = "dtb";
	/*
	 * The names give the bytes from the highest down, of a pixel read
	 * as one little endian word: a8r8g8b8 has blue in the lowest byte.
	 * An 'a' says the top byte is opacity, 0xFF opaque.
	 */
	if ( same(f->format, "a8r8g8b8") || same(f->format, "x8r8g8b8") ) {
		hw->bpp = 32;
		hw->format = order_fmt(FALSE);
	} else if ( same(f->format, "a8b8g8r8") || same(f->format, "x8b8g8r8") ) {
		hw->bpp = 32;
		hw->format = order_fmt(TRUE);
	} else if ( same(f->format, "r5g6b5") ) {
		hw->bpp = 16;
		hw->format = DISP_FMT_RGB565;
	} else {
		tm_printf((UB *)"disp: format %s is not one this driver sends\n", f->format);
		return 0;
	}
	hw->alpha = ( f->format[0] == 'a' ) ? DISP_ALPHA_FF : DISP_ALPHA_ANY;
	if ( (UD)hw->pitch * hw->height > f->size
	  || hw->pitch < hw->width * ( hw->bpp / 8 ) ) {
		tm_printf((UB *)"disp: the node's size does not hold its rows\n");
		return 0;
	}

	return 1;
}

/* ---------------------------------------------------------------- the mailbox */

/*
 * How large a screen to ask for: the display's own size, halved until
 * it is within the limit (a 3840 x 2160 display is given 1920 x 1080,
 * which the firmware scales up to it).
 */
LOCAL UINT	force_w = 0, force_h = 0;	/* a size the machine's settings ask for */

LOCAL void want_size( UINT *p_w, UINT *p_h )
{
	UINT	w = 0, h = 0;
	INT	at;

	if ( force_w != 0 && force_h != 0 ) {
		*p_w = force_w & ~7U;
		*p_h = force_h;
		return;
	}

	mb_begin();
	at = mb_tag(TAG_GET_PHYSICAL_WH, 2, 0, 0);
	if ( mb_call() == E_OK && mb_answered(at) ) {
		w = mb[at];
		h = mb[at + 1];
	}
	if ( w == 0 || h == 0 ) {
		w = DEFAULT_W;
		h = DEFAULT_H;
	}
	while ( w > CNF_DISP_MAX_W || h > CNF_DISP_MAX_H ) {
		w /= 2;
		h /= 2;
	}
	*p_w = w & ~7U;			/* rows of a whole number of cache lines */
	*p_h = h;
}

/*
 * Ask the firmware for a buffer of 'pages' screens' worth of rows, the
 * screen showing the first. Answers 1 and fills 'hw' without mapping
 * it, or 0 when the firmware does not answer or gives none. A buffer
 * of fewer pages than asked for is one page.
 */
LOCAL INT from_mailbox( T_DISPHW *hw, UINT pages )
{
	UINT	w, h;
	INT	a_phys, a_virt, a_depth, a_order, a_alpha, a_alloc, a_pitch;
	ER	er;

	want_size(&w, &h);
	tm_printf((UB *)"disp: asking the firmware for a framebuffer of %d x %d, 32 bpp, %d page%s\n",
		  (INT)w, (INT)h, (INT)pages, ( pages > 1 ) ? "s" : "");

	mb_begin();
	a_phys  = mb_tag(TAG_SET_PHYSICAL_WH, 2, w, h);
	a_virt  = mb_tag(TAG_SET_VIRTUAL_WH, 2, w, h * pages);
	(void)mb_tag(TAG_SET_VIRTUAL_OFFSET, 2, 0, 0);
	a_depth = mb_tag(TAG_SET_DEPTH, 1, 32, 0);
	a_order = mb_tag(TAG_SET_PIXEL_ORDER, 1, ORDER_BGR, 0);
	a_alpha = mb_tag(TAG_SET_ALPHA_MODE, 1, ALPHA_IGNORED, 0);
	a_alloc = mb_tag(TAG_ALLOCATE_BUFFER, 2, PAGE_SIZE, 0);
	a_pitch = mb_tag(TAG_GET_PITCH, 1, 0, 0);
	er = mb_call();
	if ( er < E_OK ) {
		tm_printf((UB *)"disp: the firmware did not answer (%d)\n", (INT)er);
		return 0;
	}
	if ( !mb_answered(a_alloc) || ( mb[a_alloc] & VC_ADDR_MASK ) == 0
	  || !mb_answered(a_phys) || !mb_answered(a_depth) || !mb_answered(a_pitch) ) {
		tm_printf((UB *)"disp: the firmware gave no framebuffer "
			  "(allocate %x %x, size %x, depth %x, pitch %x)\n",
			  mb[a_alloc - 1], mb[a_alloc], mb[a_phys - 1],
			  mb[a_depth - 1], mb[a_pitch - 1]);
		return 0;
	}

	hw->width  = mb[a_phys];
	hw->height = mb[a_phys + 1];
	hw->bpp    = mb[a_depth];
	hw->pitch  = mb[a_pitch];
	hw->fb_pa  = (UD)( mb[a_alloc] & VC_ADDR_MASK );
	hw->via    = "mailbox";
	if ( hw->bpp == 16 ) {
		hw->format = DISP_FMT_RGB565;
	} else {
		/* an order the firmware did not answer is the one asked for */
		hw->format = order_fmt(mb_answered(a_order) && mb[a_order] == ORDER_RGB);
	}
	if ( !mb_answered(a_alpha) ) {
		hw->alpha = DISP_ALPHA_ZERO;	/* opacity, 0 opaque, is the firmware's default */
	} else if ( mb[a_alpha] == ALPHA_IGNORED ) {
		hw->alpha = DISP_ALPHA_ANY;
	} else {
		hw->alpha = ( mb[a_alpha] == ALPHA_REVERSED ) ? DISP_ALPHA_FF : DISP_ALPHA_ZERO;
	}
	if ( (UD)hw->pitch * hw->height > (UD)mb[a_alloc + 1] && mb[a_alloc + 1] != 0 ) {
		tm_printf((UB *)"disp: the buffer (%d bytes) does not hold %d rows of %d\n",
			  (INT)mb[a_alloc + 1], (INT)hw->height, (INT)hw->pitch);
		return 0;
	}
	hw->pages = 1;
	if ( pages > 1 ) {
		if ( mb_answered(a_virt) && mb[a_virt + 1] >= hw->height * pages ) {
			/*
			 * The virtual size is what the firmware shows from; the size
			 * it answers for the buffer is one page's on some firmware
			 * when it holds more. The whole of it is still kept out of
			 * memory in use (map_fb), and the flip is read back.
			 */
			if ( mb[a_alloc + 1] != 0 && (UD)hw->pitch * hw->height * pages > (UD)mb[a_alloc + 1] ) {
				tm_printf((UB *)"disp: the firmware holds %d rows but says the buffer is %d bytes; "
					  "taking its %d pages\n", (INT)mb[a_virt + 1], (INT)mb[a_alloc + 1],
					  (INT)pages);
			}
			hw->pages = pages;
		} else {
			tm_printf((UB *)"disp: the firmware gave one page (virtual size %x: %d x %d, "
				  "buffer %d bytes)\n", mb[a_virt - 1], (INT)mb[a_virt],
				  (INT)mb[a_virt + 1], (INT)mb[a_alloc + 1]);
		}
	}

	return 1;
}

/* ---------------------------------------------------------------- two pages */

#if TWO_PAGES

#define FL_SPIN_US		500		/* most answers come within this */
#define FL_TMO_MS		100		/* no answer: past several refreshes */
#define FL_REFRESH_US		16667		/* a refresh at 60 Hz, when the firmware cannot say */
#define FL_SELF_US		4000		/* a flip answered this late waited for the blank itself */
#define VS_CALLS		6		/* waits for the blank timed, after one to start from */

LOCAL UW	fl_mb[16] __attribute__((aligned(64)));	/* the message of a flip or a wait */
LOCAL UINT	fl_height;			/* rows of one page */

/*
 * One tag, 'nval' words of room for its answer and 'nask' of them sent,
 * answered within FL_TMO_MS with the task asleep while it is not.
 * E_NOSPT when the firmware did not answer the tag.
 */
LOCAL ER fl_call( UW tag, INT nval, INT nask, UW v0, UW v1, UW *a0, UW *a1 )
{
	ER	er;

	knl_memset(fl_mb, 0, sizeof(fl_mb));
	fl_mb[0] = (UW)( 6 + nval ) * 4;
	fl_mb[1] = 0;			/* a request */
	fl_mb[2] = tag;
	fl_mb[3] = (UW)nval * 4;
	fl_mb[4] = (UW)nask * 4;
	fl_mb[5] = v0;
	if ( nval > 1 ) {
		fl_mb[6] = v1;
	}
	fl_mb[5 + nval] = 0;		/* the end tag */
	er = knl_mbox_property_wait(fl_mb, FL_SPIN_US, FL_TMO_MS);
	if ( er < E_OK ) {
		return er;
	}
	if ( ( fl_mb[4] & TAG_ANSWERED ) == 0 ) {
		return E_NOSPT;
	}
	if ( a0 != NULL ) {
		*a0 = fl_mb[5];
	}
	if ( a1 != NULL && nval > 1 ) {
		*a1 = fl_mb[6];
	}
	return E_OK;
}

/* The screen to show page 0 or 1; E_IO when the firmware answers another place */
LOCAL ER rpi_flip( UINT page )
{
	UW	x = ~0U, y = ~0U;
	ER	er;

	er = fl_call(TAG_SET_VIRTUAL_OFFSET, 2, 2, 0, page * fl_height, &x, &y);
	if ( er < E_OK ) {
		return er;
	}
	return ( x == 0 && y == page * fl_height ) ? E_OK : E_IO;
}

/* Until the next vertical blank */
LOCAL ER rpi_vsync( void )
{
	return fl_call(TAG_WAIT_VSYNC, 1, 1, 0, 0, NULL, NULL);
}

/* Which row the firmware says it shows from */
LOCAL ER rpi_shown( UW *p_y )
{
	return fl_call(TAG_GET_VIRTUAL_OFFSET, 2, 0, 0, 0, NULL, p_y);
}

/* So many microseconds since t0 */
LOCAL UD us_since( UD t0 )
{
	UD	now;

	(void)ts_get_mono(&now);
	return ( now - t0 ) / 1000;
}

/*
 * A buffer of two pages tried: the second page shown and the first
 * again, each read back from the firmware, and its wait for the blank
 * timed. Fills in how the screen flips, or leaves 'hw' a page.
 */
LOCAL void flip_try( T_DISPHW *hw )
{
	UD	t0, set1, set0, d[VS_CALLS], t[VS_CALLS + 1], med;
	UW	y1 = ~0U, y0 = ~0U;
	ER	er;
	INT	i, j, n;
	BOOL	even;

	fl_height = hw->height;

	(void)ts_get_mono(&t0);
	er = rpi_flip(1);
	set1 = us_since(t0);
	if ( er >= E_OK ) {
		er = rpi_shown(&y1);
	}
	if ( er >= E_OK && y1 != fl_height ) {
		er = E_IO;
	}
	(void)ts_get_mono(&t0);
	if ( er >= E_OK ) {
		er = rpi_flip(0);
	}
	set0 = us_since(t0);
	if ( er >= E_OK ) {
		er = rpi_shown(&y0);
	}
	if ( er >= E_OK && y0 != 0 ) {
		er = E_IO;
	}
	if ( er < E_OK ) {
		(void)rpi_flip(0);
		tm_printf((UB *)"disp: flip: the firmware does not show the second page "
			  "(%d; read back from row %d, then %d); one page\n",
			  (INT)er, (INT)y1, (INT)y0);
		hw->pages = 1;
		return;
	}
	tm_printf((UB *)"disp: flip: two pages of %d x %d at %lx; the firmware showed the second "
		  "from row %d in %ld us and the first again in %ld us\n",
		  (INT)hw->width, (INT)hw->height, hw->fb_pa, (INT)y1, (D)set1, (D)set0);

	hw->flip = rpi_flip;
	hw->vsync = NULL;
	hw->flip_start = TRUE;
	if ( set1 >= FL_SELF_US && set0 >= FL_SELF_US ) {
		hw->latch_us = 0;
		tm_printf((UB *)"disp: vsync: the firmware's flip answers only at the blank; "
			  "a flip is taken once it is answered\n");
		return;
	}
	hw->latch_us = FL_REFRESH_US + 1000;

	/* the wait for the blank: a refresh apart, every time, or not used */
	for ( n = 0; n <= VS_CALLS; n++ ) {
		er = rpi_vsync();
		if ( er < E_OK ) {
			break;
		}
		(void)ts_get_mono(&t[n]);
	}
	if ( n <= VS_CALLS ) {
		tm_printf((UB *)"disp: vsync: no wait for the vertical blank (%s, %d); a flip is taken "
			  "to be taken %d us after it is made\n",
			  ( er == E_NOSPT ) ? "tag 0004800e not answered"
			  : ( er == E_TMOUT ) ? "no answer" : "error", (INT)er, (INT)hw->latch_us);
		return;
	}
	for ( i = 0; i < VS_CALLS; i++ ) {
		UD	v = ( t[i + 1] - t[i] ) / 1000;

		for ( j = i; j > 0 && d[j - 1] > v; j-- ) {
			d[j] = d[j - 1];
		}
		d[j] = v;
	}
	med = ( d[VS_CALLS / 2 - 1] + d[VS_CALLS / 2] ) / 2;
	even = (BOOL)( med >= 5000 && med <= 50000
		       && d[0] >= med - med / 4 && d[VS_CALLS - 1] <= med + med / 4 );
	if ( !even ) {
		tm_printf((UB *)"disp: vsync: the waits for the blank came %ld to %ld us apart, "
			  "not a refresh apart; not used, a flip is taken to be taken %d us after it is made\n",
			  (D)d[0], (D)d[VS_CALLS - 1], (INT)hw->latch_us);
		return;
	}
	hw->vsync = rpi_vsync;
	hw->latch_us = (UINT)( med + med / 8 );
	tm_printf((UB *)"disp: vsync: the waits for the blank came every %ld us (%ld to %ld, %d Hz); "
		  "a send within %d us of a flip waits for the next blank\n",
		  (D)med, (D)d[0], (D)d[VS_CALLS - 1], (INT)( ( 1000000 + med / 2 ) / med ),
		  (INT)hw->latch_us);
}

/*
 * A buffer of two pages in place of the node's one, at the node's size.
 * Answers 1 with 'hw' mapped (of two pages or, when the firmware gave
 * one, of one), or 0 to take the node.
 */
LOCAL INT two_pages( CONST T_DISPHW *node, T_DISPHW *hw )
{
	UW	tw = 0, th = 0;
	INT	at, r;

	mb_begin();
	at = mb_tag(TAG_TEST_VIRTUAL_WH, 2, node->width, node->height * 2);
	if ( mb_call() == E_OK && mb_answered(at) ) {
		tw = mb[at];
		th = mb[at + 1];
		if ( th < node->height * 2 ) {
			/*
			 * The test answers the size the firmware holds now on some
			 * firmware, whatever was asked: only a buffer actually asked
			 * for says how many pages there can be.
			 */
			tm_printf((UB *)"disp: flip: asked without a buffer, the firmware answers %d x %d "
				  "for two pages of %d x %d; asking for the buffer all the same\n",
				  (INT)tw, (INT)th, (INT)node->width, (INT)node->height);
		}
	}

	knl_memset(hw, 0, sizeof(*hw));
	force_w = node->width;
	force_h = node->height;
	r = from_mailbox(hw, 2);
	force_w = force_h = 0;
	if ( r <= 0 ) {
		tm_printf((UB *)"disp: flip: no buffer of two pages; the node's one page\n");
		return 0;
	}
	if ( hw->fb_pa == node->fb_pa ) {
		/*
		 * The firmware kept the buffer it had: the screen still reads it
		 * the way the node says, whatever order and alpha it answered
		 */
		hw->format = node->format;
		hw->alpha = node->alpha;
		hw->pitch = node->pitch;
		tm_printf((UB *)"disp: flip: the firmware kept its buffer; its pixels as the node says\n");
	}
	r = map_fb(hw);
	if ( r <= 0 ) {
		tm_printf((UB *)"disp: flip: the firmware's new buffer at %lx cannot be used; back to "
			  "the node's, which the firmware may no longer show (make DISP_FLIP=0)\n",
			  hw->fb_pa);
		return 0;
	}
	if ( hw->pages > 1 ) {
		flip_try(hw);
	}
	return 1;
}

#endif /* TWO_PAGES */

/* ---------------------------------------------------------------- the scaler */

#ifdef RPI5
/*
 * The display's scaler (HVS) reads, each frame, a list of the planes it
 * shows, each with the address of its pixels. The firmware made that
 * list for its framebuffer; showing another page is writing another
 * address there. Before anything is written, how the list holds the
 * framebuffer's address is looked for and written out: every word of
 * the lists' memory that holds it in one of the forms an address may
 * take, with the words round it. Only read.
 *
 * Only words the scaler has are read. Its registers come first, then
 * the lists' memory at HVS_DL_START, as many words as the register at
 * 4 says; the rest of the range the device tree gives has holes that
 * answer a read with an SError. Which registers there are depends on
 * the chip's stepping, told by the low byte of the first: 0x53 for C0
 * (the Raspberry Pi 5 of 4 and 8 GB), 0x54 for D0 (the 2 GB one, the
 * Raspberry Pi 500), whose registers lie elsewhere with gaps between.
 */
#define HVS_PATH	"/hvs@107c580000"
#define HVS_FORMS	6
#define HVS_PTRS	4		/* the lists' words found holding the address */
#define HVS_DL_START	0x4000		/* the lists' memory, from the registers' start */
#define HVS_DL_SIZE	1		/* the register that holds its size in words */
#define HVS_VER_C0	0x53

IMPORT UD	knl_fb_page2;		/* pfalloc.c */

LOCAL volatile UW	*hvs_ptr[HVS_PTRS];	/* the words of the lists that hold the address */
LOCAL INT		hvs_nptr = 0;
LOCAL UBINT		hvs_regs = 0;		/* the scaler's registers, virtually */
LOCAL UW		hvs_ver = 0;		/* the stepping, the first register's low byte */

/*
 * The registers of a D0 scaler (and of one not known), as byte ranges
 * [from, to): the only ones read at that stepping when the word that
 * changes each frame is looked for
 */
LOCAL CONST UH hvs_d0_regs[][2] = {
	{ 0x000, 0x018 }, { 0x020, 0x040 }, { 0x0d0, 0x0f4 }, { 0x0fc, 0x12c },
	{ 0x140, 0x16c }, { 0x180, 0x1ac }
};

/* Whether the register at a byte offset may be read */
LOCAL BOOL hvs_reg_ok( UW off )
{
	INT	i;

	if ( hvs_ver == HVS_VER_C0 ) {
		return (BOOL)( off < 0x200 );
	}
	for ( i = 0; i < (INT)( sizeof(hvs_d0_regs) / sizeof(hvs_d0_regs[0]) ); i++ ) {
		if ( off >= hvs_d0_regs[i][0] && off < hvs_d0_regs[i][1] ) return TRUE;
	}
	return FALSE;
}

LOCAL void hvs_probe( UD fb_pa )
{
	static CONST char *CONST form[HVS_FORMS] = { "as it is", "in 16ths", "in 256ths", "in pages",
						     "with the bus's top bits", "its top word" };
	CONST UB	*reg;
	INT		len, k, found = 0;
	UD		pa, size, i, from, to;
	UW		want[HVS_FORMS];
	volatile UW	*w;

	reg = knl_dtb_prop(HVS_PATH, "reg", &len);
	if ( reg == NULL || len < 16 ) {
		tm_printf((UB *)"hvs: no node %s\n", HVS_PATH);
		return;
	}
	pa = ( (UD)reg[0] << 56 ) | ( (UD)reg[1] << 48 ) | ( (UD)reg[2] << 40 ) | ( (UD)reg[3] << 32 )
	   | ( (UD)reg[4] << 24 ) | ( (UD)reg[5] << 16 ) | ( (UD)reg[6] << 8 ) | (UD)reg[7];
	size = ( (UD)reg[8] << 56 ) | ( (UD)reg[9] << 48 ) | ( (UD)reg[10] << 40 ) | ( (UD)reg[11] << 32 )
	     | ( (UD)reg[12] << 24 ) | ( (UD)reg[13] << 16 ) | ( (UD)reg[14] << 8 ) | (UD)reg[15];
	if ( size > 0x40000 ) size = 0x40000;
	want[0] = (UW)fb_pa;
	want[1] = (UW)( fb_pa >> 4 );
	want[2] = (UW)( fb_pa >> 8 );
	want[3] = (UW)( fb_pa >> 12 );
	want[4] = (UW)fb_pa | 0xC0000000U;
	want[5] = (UW)( fb_pa >> 32 );
	w = (volatile UW *)DEV_BASE(pa);
	hvs_regs = (UBINT)w;
	tm_printf((UB *)"hvs: at %lx, %lx bytes; looking for the framebuffer %lx\n", pa, size, fb_pa);
	hvs_ver = w[0] & 0xFF;
	tm_printf((UB *)"hvs: first words");
	for ( i = 0; i < 6; i++ ) tm_printf((UB *)" %08x", w[i]);
	tm_printf((UB *)"\n");
	from = HVS_DL_START / 4;
	to = from + w[HVS_DL_SIZE];
	if ( to > size / 4 ) to = size / 4;
	tm_printf((UB *)"hvs: stepping %s (%02x), lists at %lx..%lx\n",
		  ( hvs_ver == HVS_VER_C0 ) ? "C0" : ( hvs_ver == 0x54 ) ? "D0" : "not known",
		  hvs_ver, from * 4, to * 4);
	for ( i = from; i < to && found < 12; i++ ) {
		UW	v = w[i];

		for ( k = 0; k < HVS_FORMS - 1; k++ ) {
			if ( v == want[k] && v != 0 ) {
				UD	j;

				found++;
				if ( k == 0 && hvs_nptr < HVS_PTRS ) hvs_ptr[hvs_nptr++] = &w[i];
				tm_printf((UB *)"hvs: word %lx holds it %s:", i * 4, form[k]);
				for ( j = ( i >= from + 8 ) ? i - 8 : from; j < i + 8 && j < to; j++ ) {
					tm_printf((UB *)"%s%08x", ( j == i ) ? " [" : " ", w[j]);
					if ( j == i ) tm_printf((UB *)"]");
				}
				tm_printf((UB *)"\n");
			}
		}
	}
	if ( found == 0 ) tm_printf((UB *)"hvs: no word holds the framebuffer's address in any form looked for\n");
}

/*
 * Two pages through the scaler: the first is the firmware's buffer, the
 * second the one kept for it from the start (pfalloc.c), and a flip
 * writes the address of the one to show into each list's word that held
 * the first's. The scaler reads its list at the start of a frame, so a
 * flip is taken whole at the next one; there is no wait here for it.
 */
LOCAL UW	hvs_addr[2];

/*
 * When a flip has been taken. The scaler's registers hold a word that
 * changes once each frame, or twice (going into the vertical blank and
 * out of it); it is found at start-up by reading the first of them over
 * several frames and taking one that changed so often, no more and no
 * less. A flip is taken once a blank that began after it has ended: in
 * the blank the scaler reads its lists anew.
 *
 * The word is then watched over several frames more, each change timed,
 * for the refresh as a clock (disp.c): with two changes a frame, the
 * value it holds the longer is the one outside the blank, a change to it
 * is the end of a blank, and the time the other value lasts is the
 * blank's; with one, each change is taken for the end of a blank. The
 * period is the time from the first end seen to the last over their
 * number. Each wait for the blank sets the clock again by the end it
 * saw, so it does not drift.
 */
#define HVS_FC_WORDS	128		/* the registers looked at */
#define HVS_FC_US	120000		/* for so long: seven frames at 60 Hz */
#define HVS_CL_CHANGES	32		/* changes timed for the clock */
#define HVS_CL_US	150000		/* for at most so long */

LOCAL volatile UW	*hvs_fc = NULL;	/* the word that changes each frame */
LOCAL UW		hvs_fc_flip;	/* what it held at the last flip */
LOCAL INT		hvs_fc_edges;	/* how often it changes in a frame: 1 or 2 */
LOCAL UW		hvs_fc_idle;	/* with two: what it holds outside the blank */
LOCAL UD		hvs_frame_ns = 0;	/* the refresh, 0 when it could not be timed */
LOCAL UD		hvs_blank_ns = 0;
LOCAL UD		hvs_seen = 0;	/* when the end of a blank was last seen, ns */

LOCAL UD hvs_blank_seen( void )
{
	return hvs_seen;
}

/* The changes of the word timed, and the clock worked out from them */
LOCAL void hvs_clock_find( void )
{
	UD	at[HVS_CL_CHANGES], t0, now, held[2] = { 0, 0 }, first = 0, last = 0, d, f;
	UW	val[HVS_CL_CHANGES], v, was;
	INT	n = 0, i, ends = 0, nb = 0, bad = 0;

	was = *hvs_fc;
	(void)ts_get_mono(&t0);
	now = t0;
	while ( n < HVS_CL_CHANGES && now - t0 < (UD)HVS_CL_US * 1000 ) {
		v = *hvs_fc;
		(void)ts_get_mono(&now);
		if ( v != was ) {
			at[n] = now;
			val[n] = v;
			n++;
			was = v;
		}
	}
	if ( n < 6 ) {
		tm_printf((UB *)"hvs: clock: %d changes in %d ms; no clock\n", n, HVS_CL_US / 1000);
		return;
	}
	if ( hvs_fc_edges == 2 ) {
		for ( i = 2; i < n; i++ ) {
			if ( val[i] != val[i - 2] ) {
				tm_printf((UB *)"hvs: clock: the word takes more than two values; no clock\n");
				return;
			}
		}
		/* the value held the longer, over the whole changes, is the one outside the blank */
		for ( i = 0; i + 1 < n; i++ ) {
			held[i & 1] += at[i + 1] - at[i];
		}
		hvs_fc_idle = ( held[0] >= held[1] ) ? val[0] : val[1];
	}
	for ( i = 0; i < n; i++ ) {
		if ( hvs_fc_edges == 2 && val[i] != hvs_fc_idle ) {
			if ( i + 1 < n ) {
				hvs_blank_ns += at[i + 1] - at[i];
				nb++;
			}
			continue;
		}
		if ( ends == 0 ) first = at[i];
		last = at[i];
		ends++;
	}
	if ( ends < 3 ) {
		tm_printf((UB *)"hvs: clock: %d ends of a blank seen; no clock\n", ends);
		hvs_blank_ns = 0;
		return;
	}
	f = ( last - first ) / (UD)( ends - 1 );
	hvs_blank_ns = ( nb > 0 ) ? hvs_blank_ns / (UD)nb : 0;
	/* every end seen a whole number of periods from the first, within a fiftieth of one */
	for ( i = 0; i < n; i++ ) {
		if ( hvs_fc_edges == 2 && val[i] != hvs_fc_idle ) continue;
		d = ( at[i] - first ) % f;
		if ( d > f / 50 && f - d > f / 50 ) bad++;
	}
	if ( f < 5000000 || f > 50000000 || bad > 0 ) {
		tm_printf((UB *)"hvs: clock: ends of a blank %ld us apart, %d off it; no clock\n",
			  (D)( f / 1000 ), bad);
		hvs_blank_ns = 0;
		return;
	}
	hvs_frame_ns = f;
	hvs_seen = last;
	tm_printf((UB *)"hvs: clock: a frame every %ld.%02ld us (%d ends of a blank timed), its blank %ld us\n",
		  (D)( f / 1000 ), (D)( f % 1000 / 10 ), ends, (D)( hvs_blank_ns / 1000 ));
}

LOCAL void hvs_frame_find( void )
{
	volatile UW	*w = (volatile UW *)hvs_regs;
	UW		last[HVS_FC_WORDS];
	UH		changes[HVS_FC_WORDS];
	UD		t0, now;
	INT		i, best = -1;

	if ( hvs_regs == 0 ) return;
	for ( i = 0; i < HVS_FC_WORDS; i++ ) {
		last[i] = hvs_reg_ok((UW)i * 4) ? w[i] : 0;
		changes[i] = 0;
	}
	(void)ts_get_mono(&t0);
	do {
		for ( i = 0; i < HVS_FC_WORDS; i++ ) {
			UW	v;

			if ( !hvs_reg_ok((UW)i * 4) ) continue;
			v = w[i];

			if ( v != last[i] ) {
				last[i] = v;
				if ( changes[i] < 0xFFFF ) changes[i]++;
			}
		}
		(void)ts_get_mono(&now);
	} while ( now - t0 < (UD)HVS_FC_US * 1000 );
	for ( i = 0; i < HVS_FC_WORDS; i++ ) {
		if ( changes[i] != 0 ) {
			tm_printf((UB *)"hvs: register %x changed %d times in %d ms (now %08x)\n", i * 4,
				  (INT)changes[i], HVS_FC_US / 1000, last[i]);
		}
		/* 60 Hz gives 7 in 120 ms, 50 Hz 6: a frame each; twice that, a blank each */
		if ( best < 0 && changes[i] >= 5 && changes[i] <= 9 ) {
			best = i;
			hvs_fc_edges = 1;
		} else if ( best < 0 && changes[i] >= 11 && changes[i] <= 17 ) {
			best = i;
			hvs_fc_edges = 2;
		}
	}
	if ( best >= 0 ) {
		hvs_fc = &w[best];
		tm_printf((UB *)"hvs: the frames counted at register %x, %d change%s a frame\n", best * 4,
			  hvs_fc_edges, ( hvs_fc_edges > 1 ) ? "s" : "");
		hvs_clock_find();
	} else {
		tm_printf((UB *)"hvs: no register changes once a frame; a flip is waited for a frame's time\n");
	}
}

/*
 * Until the last flip has been taken: the end of a blank that began
 * after it. Where it is not known which value is the blank's, the word
 * changing as often as it does in a frame since the flip will do.
 */
LOCAL ER hvs_vsync( void )
{
	UD	t0, now;

	if ( hvs_fc == NULL ) {
		return E_NOSPT;
	}
	UW	last = hvs_fc_flip, v;
	INT	edges = 0;
	BOOL	began = FALSE;

	/*
	 * Read without sleeping, as the frames were counted: with two changes
	 * a frame the word holds its other value only through the blank, well
	 * under a millisecond, and a sleep of a tick would step over it. It
	 * is waited for only within a frame of the flip before; disp.c sleeps
	 * until shortly before the blank where its clock says when that is.
	 */
	(void)ts_get_mono(&t0);
	for ( ;; ) {
		v = *hvs_fc;
		if ( v != last ) {
			(void)ts_get_mono(&now);
			last = v;
			if ( hvs_fc_edges == 2 && hvs_frame_ns != 0 ) {
				if ( v != hvs_fc_idle ) {
					began = TRUE;		/* a blank begun after the flip */
					continue;
				}
				hvs_seen = now;
				if ( began ) {
					return E_OK;
				}
				continue;			/* the end of the blank the flip was made in */
			}
			if ( hvs_fc_edges == 1 ) {
				hvs_seen = now;
			}
			if ( ++edges >= hvs_fc_edges ) {
				return E_OK;
			}
			continue;
		}
		(void)ts_get_mono(&now);
		if ( now - t0 > 40000000ULL ) {
			return E_TMOUT;		/* past two frames: the counting is not what it seemed */
		}
	}
}

LOCAL ER hvs_flip( UINT page )
{
	INT	k;

	if ( page > 1 || hvs_nptr == 0 ) {
		return E_PAR;
	}
	if ( hvs_fc != NULL ) {
		hvs_fc_flip = *hvs_fc;
	}
	for ( k = 0; k < hvs_nptr; k++ ) {
		*hvs_ptr[k] = hvs_addr[page];
	}
	DeviceBarrier();
	return E_OK;
}

LOCAL INT hvs_pages( CONST T_DISPHW *node, T_DISPHW *hw )
{
	UD	size = (UD)node->pitch * node->height;
	UD	npages = ( size + PAGE_SIZE - 1 ) / PAGE_SIZE;
	UBINT	va2;

	if ( hvs_nptr == 0 || knl_fb_page2 == 0 || knl_fb_page2 + size > 0x100000000ULL ) {
		tm_printf((UB *)"hvs: no two pages (%d words, second page at %lx); one page\n", hvs_nptr,
			  knl_fb_page2);
		return 0;
	}
	*hw = *node;
	hw->pages = 1;
	if ( map_fb(hw) <= 0 ) {
		return 0;
	}
	va2 = (UBINT)knl_vmap_pa(knl_fb_page2, npages, VMAP_NOCACHE);
	if ( va2 == 0 ) {
		tm_printf((UB *)"hvs: the second page at %lx cannot be mapped; one page\n", knl_fb_page2);
		return 1;
	}
	hvs_addr[0] = (UW)node->fb_pa;
	hvs_addr[1] = (UW)knl_fb_page2;
	hw->page2_va = va2;
	hw->pages = 2;
	hw->flip = hvs_flip;
	hvs_frame_find();
	hw->vsync = ( hvs_fc != NULL ) ? hvs_vsync : NULL;
	hw->latch_us = 17667;		/* a frame at 60 Hz, the most a flip takes */
	if ( hvs_frame_ns != 0 ) {
		hw->frame_ns = hvs_frame_ns;
		hw->blank_ns = hvs_blank_ns;
		hw->blank_seen = hvs_blank_seen;
		hw->latch_us = (UINT)( hvs_frame_ns / 1000 + 1000 );
	}
	hw->flip_start = TRUE;
	tm_printf((UB *)"hvs: two pages, %lx and %lx, shown in turn through %d words of the scaler's lists\n",
		  node->fb_pa, knl_fb_page2, hvs_nptr);
	return 1;
}
#endif

/* ---------------------------------------------------------------- start-up */

EXPORT INT knl_disp_hw_init( T_DISPHW *hw )
{
	T_DISPHW	node;
	INT		r;
	BOOL		have_node, node_first;

	if ( hw == NULL ) {
		return E_PAR;
	}
	knl_memset(&node, 0, sizeof(node));
	have_node = (BOOL)( from_dtb(&node) > 0 );
#ifdef RPI5
	if ( have_node ) {
		hvs_probe(node.fb_pa);
	}
#endif
	node_first = (BOOL)( have_node && node.bpp == 32
			     && node.width <= CNF_DISP_MAX_W && node.height <= CNF_DISP_MAX_H );

	if ( node_first ) {
#if TWO_PAGES
		if ( two_pages(&node, hw) > 0 ) {
			return 1;
		}
#endif
#if HVS_PAGES
		if ( hvs_pages(&node, hw) > 0 ) {
			return 1;
		}
#endif
		*hw = node;
		hw->pages = 1;
		r = map_fb(hw);
		if ( r != 0 ) {
			return r;
		}
	}
	knl_memset(hw, 0, sizeof(*hw));
	if ( from_mailbox(hw, TWO_PAGES ? 2 : 1) > 0 ) {
		r = map_fb(hw);
#if TWO_PAGES
		if ( r > 0 && hw->pages > 1 ) {
			flip_try(hw);
		}
#endif
		if ( r != 0 ) {
			return r;
		}
	}
	if ( have_node && !node_first ) {
		*hw = node;
		hw->pages = 1;
		return map_fb(hw);
	}

	return 0;				/* no screen: serial only */
}

/*
 * Another size: a new buffer asked of the firmware at that size and
 * mapped, of two pages when the screen flips. A screen the firmware will
 * not give a buffer for keeps the one it has.
 */
EXPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	T_DISPHW	n;
	INT		r;
	BOOL		flips;

	if ( hw == NULL || w < 320 || h < 200 || w > CNF_DISP_MAX_W || h > CNF_DISP_MAX_H ) {
		return 0;
	}
	flips = (BOOL)( hw->flip != NULL && hw->pages > 1 );
	knl_memset(&n, 0, sizeof(n));
	force_w = w;
	force_h = h;
	r = from_mailbox(&n, flips ? 2 : 1);
	force_w = force_h = 0;
	if ( r <= 0 || n.width != ( w & ~7U ) || n.height != h ) {
		return 0;
	}
	r = map_fb(&n);
	if ( r <= 0 ) {
		return 0;
	}
#if TWO_PAGES
	if ( flips && n.pages > 1 ) {
		n.flip = hw->flip;
		n.vsync = hw->vsync;
		n.latch_us = hw->latch_us;
		n.flip_start = hw->flip_start;
		fl_height = n.height;
	} else {
		n.pages = 1;
	}
#else
	n.pages = 1;
#endif
	*hw = n;
	return 1;
}


/*
 * The displays the firmware drives besides the one shown (10.4.8, the
 * Raspberry Pi 5's second HDMI): read and said on the console only, for
 * the records the second output is to be made from; no further output is
 * given yet. The firmware counts its displays (GET_NUM_DISPLAYS) and
 * says how each is set (GET_DISPLAY_SETTINGS, the display's number given:
 * its size, depth, row stride and virtual size, and where its buffer is),
 * as the firmware's own framebuffer driver asks; config.txt's
 * max_framebuffers=2 is what makes it keep a second one. Nothing is set,
 * so the screen shown goes on as it was.
 */
#define TAG_GET_NUM_DISPLAYS	0x00040013U
#define TAG_GET_DISPLAY_SETTINGS 0x00040014U

LOCAL BOOL	more_told = FALSE;

EXPORT INT knl_disp_hw_more( INT i, UINT main_w, UINT main_h, T_DISPHW *hw, INT *p_side, INT *p_offset )
{
	INT	at, k, n;

	(void)main_w; (void)main_h; (void)hw; (void)p_side; (void)p_offset;
	if ( i != 0 || more_told ) {
		return 0;
	}
	more_told = TRUE;
	mb_begin();
	at = mb_tag(TAG_GET_NUM_DISPLAYS, 1, 0, 0);
	if ( mb_call() < E_OK || !mb_answered(at) ) {
		tm_printf((UB *)"disp: displays: the firmware does not say how many it drives\n");
		return 0;
	}
	n = (INT)mb[at];
	if ( n < 0 || n > 16 ) {
		tm_printf((UB *)"disp: displays: the firmware says %d, not taken\n", n);
		return 0;
	}
	tm_printf((UB *)"disp: displays: the firmware drives %d\n", n);
	for ( k = 0; k < n && k < 4; k++ ) {
		mb_begin();
		mb[mb_at++] = TAG_GET_DISPLAY_SETTINGS;
		mb[mb_at++] = 11 * 4;
		mb[mb_at++] = 4;
		at = mb_at;
		mb[mb_at++] = (UW)k;
		mb_at += 10;
		/* a call the firmware does not answer in time, or answers with an error or too short, is passed over */
		if ( mb_call() < E_OK || !mb_answered(at) || ( mb[at - 1] & ~TAG_ANSWERED ) < 11 * 4 ) {
			tm_printf((UB *)"disp: display %d: no settings answered\n", k);
			continue;
		}
		tm_printf((UB *)"disp: display %d: %d x %d, %d bpp, pitch %d, virtual %d x %d at %d, %d, buffer %08x%08x\n",
			  (INT)mb[at], (INT)mb[at + 1], (INT)mb[at + 2], (INT)mb[at + 3], (INT)mb[at + 4],
			  (INT)mb[at + 5], (INT)mb[at + 6], (INT)mb[at + 7], (INT)mb[at + 8], mb[at + 10], mb[at + 9]);
	}
	return 0;
}

#endif /* RPI_VCFW */
