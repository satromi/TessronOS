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
 *	Screen of the Raspberry Pi 5 (design 16.5.1).
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
 *	Not yet verified on hardware (HW-15, HW-17).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <tk/smem.h>
#include <ts/disp.h>
#include "sysdepend/sysdepend.h"
#include "sysman/pfalloc.h"
#include "disp_hw.h"

#define TAG_ALLOCATE_BUFFER	0x00040001U
#define TAG_GET_PHYSICAL_WH	0x00040003U
#define TAG_GET_PITCH		0x00040008U
#define TAG_SET_PHYSICAL_WH	0x00048003U
#define TAG_SET_VIRTUAL_WH	0x00048004U
#define TAG_SET_DEPTH		0x00048005U
#define TAG_SET_PIXEL_ORDER	0x00048006U
#define TAG_SET_ALPHA_MODE	0x00048007U
#define TAG_SET_VIRTUAL_OFFSET	0x00048009U
#define TAG_ANSWERED		0x80000000U	/* in a tag's size word */

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

/* Map the pixels so that writes may merge on their way out */
LOCAL INT map_fb( T_DISPHW *hw )
{
	UD	size = (UD)hw->pitch * hw->height;
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
 * Ask the firmware for a buffer. Answers 1 and fills 'hw' without
 * mapping it, or 0 when the firmware does not answer or gives none.
 */
LOCAL INT from_mailbox( T_DISPHW *hw )
{
	UINT	w, h;
	INT	a_phys, a_depth, a_order, a_alpha, a_alloc, a_pitch;
	ER	er;

	want_size(&w, &h);
	tm_printf((UB *)"disp: asking the firmware for a framebuffer of %d x %d, 32 bpp\n",
		  (INT)w, (INT)h);

	mb_begin();
	a_phys  = mb_tag(TAG_SET_PHYSICAL_WH, 2, w, h);
	(void)mb_tag(TAG_SET_VIRTUAL_WH, 2, w, h);
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

	return 1;
}

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
	node_first = (BOOL)( have_node && node.bpp == 32
			     && node.width <= CNF_DISP_MAX_W && node.height <= CNF_DISP_MAX_H );

	if ( node_first ) {
		*hw = node;
		r = map_fb(hw);
		if ( r != 0 ) {
			return r;
		}
	}
	knl_memset(hw, 0, sizeof(*hw));
	if ( from_mailbox(hw) > 0 ) {
		r = map_fb(hw);
		if ( r != 0 ) {
			return r;
		}
	}
	if ( have_node && !node_first ) {
		*hw = node;
		return map_fb(hw);
	}

	return 0;				/* no screen: serial only */
}

/*
 * Another size: a new buffer asked of the firmware at that size and
 * mapped. A screen the firmware will not give a buffer for keeps the
 * one it has.
 */
EXPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	T_DISPHW	n;
	INT		r;

	if ( hw == NULL || w < 320 || h < 200 || w > CNF_DISP_MAX_W || h > CNF_DISP_MAX_H ) {
		return 0;
	}
	knl_memset(&n, 0, sizeof(n));
	force_w = w;
	force_h = h;
	r = from_mailbox(&n);
	force_w = force_h = 0;
	if ( r <= 0 || n.width != ( w & ~7U ) || n.height != h ) {
		return 0;
	}
	r = map_fb(&n);
	if ( r <= 0 ) {
		return 0;
	}
	*hw = n;
	return 1;
}

#endif /* RPI5 */
