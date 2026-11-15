/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_bochs.c
 *	Screen of the emulated machine (design 16.5.1).
 *
 *	A display function on the bus with two windows: the first is the
 *	pixels, laid out one row after another, and the second holds a
 *	handful of sixteen bit registers that set the mode. Asking for a
 *	size and a depth and then turning it on is all there is to it.
 *
 *	It is the same shape as the route the board takes, which asks its
 *	firmware for a buffer and is told where it went, so the layers
 *	above this one are written once and work on both.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "disp_hw.h"

#ifdef PCIE_ECAM_PA

#define PCI_CLASS_DISPLAY	0x03

/* the registers, sixteen bits each, in the second window */
#define VBE_OFF			0x500
#define VBE_ID			0
#define VBE_XRES		1
#define VBE_YRES		2
#define VBE_BPP			3
#define VBE_ENABLE		4
#define VBE_BANK		5
#define VBE_VIRT_WIDTH		6
#define VBE_VIRT_HEIGHT		7
#define VBE_X_OFFSET		8
#define VBE_Y_OFFSET		9

#define VBE_DISABLED		0x00
#define VBE_ENABLED		0x01
#define VBE_LFB_ENABLED		0x40	/* the pixels are one flat run */
#define VBE_NOCLEARMEM		0x80

/*
 * What to ask for. The back buffer and the screen together come to
 * eight megabytes at this size, which the card has room for, and it is
 * the size the desktop's own ground was drawn at.
 */
#ifdef CNF_BOCHS_W			/* another size, to try what a board's display would give */
#define WANT_WIDTH		CNF_BOCHS_W
#define WANT_HEIGHT		CNF_BOCHS_H
#else
#define WANT_WIDTH		1280
#define WANT_HEIGHT		800
#endif
#define WANT_BPP		32

LOCAL UBINT	vbe_base = 0;
LOCAL CONST T_DISPOUTHW	main_out;		/* the main display as an output, below */
LOCAL UD	vbe_fb_size = 0;		/* the pixels' window */
LOCAL UBINT	vbe_fb_pages = 0;		/* the pages mapped of it */

LOCAL void vbe_write( UINT reg, UH val )
{
	*(volatile UH *)(vbe_base + VBE_OFF + reg * 2) = val;
	DeviceBarrier();
}

LOCAL UH vbe_read( UINT reg )
{
	return *(volatile UH *)(vbe_base + VBE_OFF + reg * 2);
}

/*
 * The page shown, by the row the screen starts from. The card reads
 * the pixels afresh at each refresh from wherever that says, so a flip
 * is taken at once; it is used only when asked for (ts_disp_flip), and
 * the screen keeps its first page otherwise.
 */
LOCAL UH	vbe_rows;			/* rows of one page */

LOCAL ER vbe_flip( UINT page )
{
	vbe_write(VBE_Y_OFFSET, (UH)( page * vbe_rows ));
	return ( vbe_read(VBE_Y_OFFSET) == (UH)( page * vbe_rows ) ) ? E_OK : E_IO;
}

/* Whether the mode holds two pages, as the card says and its window has room */
LOCAL UINT vbe_pages( UINT pitch, UINT h )
{
	return ( vbe_read(VBE_VIRT_HEIGHT) >= h * 2 && (UD)pitch * h * 2 <= vbe_fb_size ) ? 2 : 1;
}

EXPORT INT knl_disp_hw_init( T_DISPHW *hw )
{
	T_PCIDEV	d;
	UBINT		fb_va;
	UD		fb_size;
	UH		id;

	if ( hw == NULL ) {
		return E_PAR;
	}
	if ( ts_pcie_find(PCI_CLASS_DISPLAY, 0xff, 0xff, 0, &d) < E_OK ) {
		return 0;			/* the machine has no screen */
	}
	if ( d.bar[0] == 0 || d.bar[2] == 0 ) {
		return 0;			/* it was given no windows */
	}
	ts_pcie_enable(&d);

	/*
	 * The registers are reached through the device window, because
	 * they are registers: every write has to arrive on its own and in
	 * order. The pixels are the opposite and are mapped below.
	 */
	vbe_base = ts_pcie_bar_base(&d, 2);
	if ( vbe_base == 0 ) {
		return 0;
	}
	id = vbe_read(VBE_ID);
	if ( (id & 0xfff0) != 0xb0c0 ) {
		tm_printf((UB *)"disp: the display answers %04x, not a mode setter\n",
			  (INT)id);
		return 0;
	}

	/* the mode is set while the output is off, then turned back on */
	vbe_write(VBE_ENABLE, VBE_DISABLED);
	vbe_write(VBE_XRES, WANT_WIDTH);
	vbe_write(VBE_YRES, WANT_HEIGHT);
	vbe_write(VBE_BPP, WANT_BPP);
	vbe_write(VBE_VIRT_WIDTH, WANT_WIDTH);
	/* room for two screens, so a later stage can switch between them */
	vbe_write(VBE_VIRT_HEIGHT, WANT_HEIGHT * 2);
	vbe_write(VBE_X_OFFSET, 0);
	vbe_write(VBE_Y_OFFSET, 0);
	vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);

	if ( vbe_read(VBE_XRES) != WANT_WIDTH
	  || vbe_read(VBE_YRES) != WANT_HEIGHT
	  || vbe_read(VBE_BPP) != WANT_BPP ) {
		tm_printf((UB *)"disp: the mode did not take (%d x %d, %d bpp)\n",
			  (INT)vbe_read(VBE_XRES), (INT)vbe_read(VBE_YRES),
			  (INT)vbe_read(VBE_BPP));
		vbe_write(VBE_ENABLE, VBE_DISABLED);
		return 0;
	}

	hw->width  = WANT_WIDTH;
	hw->height = WANT_HEIGHT;
	hw->bpp    = WANT_BPP;
	hw->pitch  = WANT_WIDTH * (WANT_BPP / 8);
	hw->format = DISP_FMT_XRGB8888;
	hw->fb_pa  = d.bar[0];
	hw->alpha  = DISP_ALPHA_ANY;
	hw->via    = "bochs-display";

	/*
	 * The pixels are mapped so that writes may merge and reorder on
	 * their way out, which is what makes filling a rectangle cost
	 * anything like what the memory can do. Nothing reads them back.
	 */
	fb_size = (UD)hw->pitch * hw->height;
	if ( fb_size > d.bar_size[0] ) {
		return 0;			/* the window is too small for it */
	}
	vbe_fb_size = d.bar_size[0];
	/* the whole window is mapped, so that another size needs no new mapping */
	vbe_fb_pages = (UBINT)( ( vbe_fb_size + PAGE_SIZE - 1 ) / PAGE_SIZE );
	fb_va = (UBINT)knl_vmap_pa(hw->fb_pa, vbe_fb_pages, VMAP_NOCACHE);
	if ( fb_va == 0 ) {
		return E_NOMEM;
	}
	hw->fb_va = fb_va;
	vbe_rows = WANT_HEIGHT;
	hw->pages = vbe_pages(hw->pitch, hw->height);
	hw->flip = ( hw->pages > 1 ) ? vbe_flip : NULL;
	hw->vsync = NULL;
	hw->latch_us = 0;
	hw->flip_start = FALSE;
	hw->out = &main_out;

	return 1;
}

EXPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	if ( hw == NULL || vbe_base == 0 || w < 320 || h < 200 || w > 4096 || h > 4096 ) {
		return 0;
	}
	w &= ~7U;
	if ( (UD)w * h * ( WANT_BPP / 8 ) * 2 > vbe_fb_size ) {
		return 0;			/* two screens of it do not fit the window */
	}
	vbe_write(VBE_ENABLE, VBE_DISABLED);
	vbe_write(VBE_XRES, (UH)w);
	vbe_write(VBE_YRES, (UH)h);
	vbe_write(VBE_BPP, WANT_BPP);
	vbe_write(VBE_VIRT_WIDTH, (UH)w);
	vbe_write(VBE_VIRT_HEIGHT, (UH)( h * 2 ));
	vbe_write(VBE_X_OFFSET, 0);
	vbe_write(VBE_Y_OFFSET, 0);
	vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);
	if ( vbe_read(VBE_XRES) != w || vbe_read(VBE_YRES) != h ) {
		/* back to what it was */
		vbe_write(VBE_ENABLE, VBE_DISABLED);
		vbe_write(VBE_XRES, (UH)hw->width);
		vbe_write(VBE_YRES, (UH)hw->height);
		vbe_write(VBE_VIRT_WIDTH, (UH)hw->width);
		vbe_write(VBE_VIRT_HEIGHT, (UH)( hw->height * 2 ));
		vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);
		return 0;
	}
	hw->width = w;
	hw->height = h;
	hw->pitch = w * ( WANT_BPP / 8 );
	vbe_rows = (UH)h;
	hw->pages = vbe_pages(hw->pitch, h);
	hw->flip = ( hw->pages > 1 ) ? vbe_flip : NULL;
	return 1;
}

/*
 * The EDID QEMU gives a display (its edid property, on by default): in the
 * first bytes of the register window, the base block and its extensions.
 * The number of bytes read, 0 when no EDID is there.
 */
#define EDID_WIN		1024

LOCAL INT edid_read( UBINT regs, UB *buf, INT max )
{
	INT	i, n = ( max < EDID_WIN ) ? max : EDID_WIN;

	if ( regs == 0 ) {
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		buf[i] = *(volatile UB *)( regs + i );
	}
	return ( n >= 128 && buf[0] == 0x00 && buf[1] == 0xFF && buf[7] == 0x00 ) ? n : 0;
}

LOCAL INT main_edid( void *ctx, UB *buf, INT max )
{
	(void)ctx;
	return edid_read(vbe_base, buf, max);
}

/* The main display, as the layer of outputs sees it: its size goes by knl_disp_hw_setmode */
LOCAL CONST T_DISPOUTHW	main_out = {
	DISP_CON_VIRTUAL, "Virtual-1", NULL, main_edid, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/*
 * A second bochs-display (QEMU's -device bochs-display given twice): a
 * further screen right of the main one (10.4.8), MORE_WIDTH wide and as
 * tall as the main one at start-up, so that the pointer, which moves in
 * the rectangle round the screens, finds a screen everywhere. Two pages
 * where its window has room, flipped between. Its mode is set again by
 * the layer of outputs (more_commit), and it is turned off when off.
 */
#define MORE_WIDTH		1024

LOCAL UBINT	more_regs = 0;
LOCAL UD	more_fb = 0, more_fb_size = 0;
LOCAL UBINT	more_va = 0;
LOCAL UH	more_rows = 0;

#define MORE_RD(reg)		( *(volatile UH *)( more_regs + VBE_OFF + (reg) * 2 ) )
#define MORE_WR(reg, v)		do { *(volatile UH *)( more_regs + VBE_OFF + (reg) * 2 ) = (UH)(v); DeviceBarrier(); } while ( 0 )

/* The page the second display shows, by the row it starts from; taken at once, as the first's */
LOCAL ER more_flip( UINT page )
{
	MORE_WR(VBE_Y_OFFSET, page * more_rows);
	return ( MORE_RD(VBE_Y_OFFSET) == (UH)( page * more_rows ) ) ? E_OK : E_IO;
}

LOCAL INT more_edid( void *ctx, UB *buf, INT max )
{
	(void)ctx;
	return edid_read(more_regs, buf, max);
}

LOCAL ER more_check( void *ctx, CONST T_DISPMODE *m )
{
	(void)ctx;
	if ( m->hdisp < 320 || m->vdisp < 200 || m->hdisp > 4096 || m->vdisp > 4096 || ( m->hdisp & 7 ) != 0
	  || (UD)m->hdisp * m->vdisp * ( WANT_BPP / 8 ) > more_fb_size ) {
		return E_NOSPT;
	}
	return E_OK;
}

/* The second display set to w x h, two pages where the window has room: hw made what it then is */
LOCAL ER more_mode( UINT w, UINT h, T_DISPHW *hw )
{
	UINT	pages = ( (UD)w * h * ( WANT_BPP / 8 ) * 2 <= more_fb_size ) ? 2 : 1;

	MORE_WR(VBE_ENABLE, VBE_DISABLED);
	MORE_WR(VBE_XRES, w);
	MORE_WR(VBE_YRES, h);
	MORE_WR(VBE_BPP, WANT_BPP);
	MORE_WR(VBE_VIRT_WIDTH, w);
	MORE_WR(VBE_VIRT_HEIGHT, h * pages);
	MORE_WR(VBE_X_OFFSET, 0);
	MORE_WR(VBE_Y_OFFSET, 0);
	MORE_WR(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);
	if ( MORE_RD(VBE_XRES) != w || MORE_RD(VBE_YRES) != h ) {
		tm_printf((UB *)"disp: the second bochs-display did not take %d x %d\n", (INT)w, (INT)h);
		return E_IO;
	}
	if ( pages == 2 && MORE_RD(VBE_VIRT_HEIGHT) < h * 2 ) {
		pages = 1;
	}
	hw->width  = w;
	hw->height = h;
	hw->bpp    = WANT_BPP;
	hw->pitch  = w * ( WANT_BPP / 8 );
	hw->format = DISP_FMT_XRGB8888;
	hw->fb_pa  = more_fb;
	hw->fb_va  = more_va;
	hw->alpha  = DISP_ALPHA_ANY;
	hw->via    = "bochs-display (second)";
	hw->pages  = pages;
	hw->page2_va = 0;
	hw->flip   = ( pages == 2 ) ? more_flip : NULL;
	hw->vsync  = NULL;
	hw->latch_us = 0;
	more_rows = (UH)h;
	return E_OK;
}

LOCAL ER more_commit( void *ctx, CONST T_DISPMODE *m, T_DISPHW *hw )
{
	(void)ctx;
	if ( m == NULL ) {
		MORE_WR(VBE_ENABLE, VBE_DISABLED);	/* off */
		return E_OK;
	}
	if ( more_check(ctx, m) < E_OK ) {
		return E_NOSPT;
	}
	return more_mode(m->hdisp, m->vdisp, hw);
}

LOCAL CONST T_DISPOUTHW	more_out = {
	DISP_CON_VIRTUAL, "Virtual-2", NULL, more_edid, NULL, more_check, more_commit,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

EXPORT INT knl_disp_hw_more( INT i, UINT main_w, UINT main_h, T_DISPHW *hw, INT *p_side, INT *p_offset )
{
	T_PCIDEV	d;
	UBINT		regs;
	UINT		w = MORE_WIDTH, h = main_h;

	(void)main_w;
	if ( i != 0 || hw == NULL || h < 200 || h > 4096 ) {
		return 0;
	}
	if ( ts_pcie_find(PCI_CLASS_DISPLAY, 0xff, 0xff, 1, &d) < E_OK || d.bar[0] == 0 || d.bar[2] == 0 ) {
		return 0;
	}
	ts_pcie_enable(&d);
	regs = ts_pcie_bar_base(&d, 2);
	if ( regs == 0 || ( *(volatile UH *)( regs + VBE_OFF + VBE_ID * 2 ) & 0xfff0 ) != 0xb0c0 ) {
		return 0;
	}
	if ( (UD)w * h * ( WANT_BPP / 8 ) > d.bar_size[0] ) {
		return 0;
	}
	/* the whole window mapped, so that another mode needs no new mapping */
	more_va = (UBINT)knl_vmap_pa(d.bar[0], ( d.bar_size[0] + PAGE_SIZE - 1 ) / PAGE_SIZE, VMAP_NOCACHE);
	if ( more_va == 0 ) {
		return 0;
	}
	more_regs = regs;
	more_fb = d.bar[0];
	more_fb_size = d.bar_size[0];
	knl_memset(hw, 0, sizeof(*hw));
	if ( more_mode(w, h, hw) < E_OK ) {
		return 0;
	}
	hw->out = &more_out;
	*p_side = DISP_SIDE_RIGHT;
	*p_offset = 0;
	return 1;
}

#endif /* PCIE_ECAM_PA */
