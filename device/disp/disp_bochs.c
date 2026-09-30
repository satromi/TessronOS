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
LOCAL UD	vbe_fb_size = 0;		/* the pixels' window */
LOCAL UBINT	vbe_fb_pages = 0;		/* the pages mapped of it */

LOCAL void vbe_write( UINT reg, UH val )
{
	*(volatile UH *)(vbe_base + VBE_OFF + reg * 2) = val;
	Asm("dsb sy" ::: "memory");
}

LOCAL UH vbe_read( UINT reg )
{
	return *(volatile UH *)(vbe_base + VBE_OFF + reg * 2);
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
	return 1;
}

#endif /* PCIE_ECAM_PA */
