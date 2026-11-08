/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_gop.c
 *	The screen of a PC: the framebuffer the UEFI firmware set up
 *	(Graphics Output Protocol) before the loader left its boot services
 *	(design 16.5.1). Its size and its pixels' order are what the loader
 *	handed over (bootinfo.h). The pixels are mapped write-combining
 *	(VMAP_NOCACHE), and the layers above work as on the other machines.
 *	The screen console goes on drawing on them until the first pixels
 *	are sent or the size is first changed (knl_disp_hw_taken), so that
 *	the steps of the start-up after this one can still be read on the
 *	screen; each part of this one is named in its band.
 *
 *	Where the hardware under the framebuffer is known, the size can be
 *	changed while the system runs (ts_disp_setmode):
 *	  - Intel's integrated graphics of the 9th generation driving the
 *	    built-in panel: the plane, the pipe's size and its scaler
 *	    (x86_64/disp_igfx.c)
 *	  - QEMU's standard VGA (1234:1111), whose framebuffer is its first
 *	    BAR and whose mode registers (the bochs display's) are at 0x500
 *	    of its third
 *	On anything else the firmware's mode is the one there is.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/pm.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "sysdepend/sysdepend.h"
#include "disp_hw.h"
#include "x86_64/disp_pc.h"

#define HW_GOP		0
#define HW_IGFX		1
#define HW_STDVGA	2

LOCAL INT	hw_kind = HW_GOP;

/* ---------------------------------------------------------------- QEMU's standard VGA */

#define VBE_OFF		0x500
#define VBE_ID		0
#define VBE_XRES	1
#define VBE_YRES	2
#define VBE_BPP		3
#define VBE_ENABLE	4
#define VBE_VIRT_WIDTH	6
#define VBE_VIRT_HEIGHT	7
#define VBE_X_OFFSET	8
#define VBE_Y_OFFSET	9
#define VBE_ON		( 0x01 | 0x40 | 0x80 )	/* enabled, one flat run of pixels, not cleared */

LOCAL UBINT	vbe_regs = 0;
LOCAL UD	vbe_fb = 0, vbe_fb_size = 0;
LOCAL UBINT	vbe_va = 0;

LOCAL void vbe_write( UINT reg, UH v )
{
	*(volatile UH *)( vbe_regs + VBE_OFF + reg * 2 ) = v;
	DeviceBarrier();
}

LOCAL UH vbe_read( UINT reg )
{
	return *(volatile UH *)( vbe_regs + VBE_OFF + reg * 2 );
}

LOCAL BOOL vbe_probe( UD fb_pa )
{
	T_PCIDEV	d;
	INT		i;
	UH		id;

	for ( i = 0; ts_pcie_find(0x03, 0x00, 0x00, i, &d) >= E_OK; i++ ) {
		if ( d.vendor != 0x1234 || d.device != 0x1111 || d.bar[0] != fb_pa || d.bar[2] == 0 ) continue;
		vbe_regs = (UBINT)DEV_BASE(d.bar[2]);
		id = vbe_read(VBE_ID);
		if ( ( id & 0xfff0 ) != 0xb0c0 ) {
			vbe_regs = 0;
			return FALSE;
		}
		vbe_fb = d.bar[0];
		vbe_fb_size = d.bar_size[0];
		tm_printf((UB *)"disp: QEMU standard VGA, %ld MB, mode registers %04x: the size can be changed\n",
			  (D)( vbe_fb_size >> 20 ), (INT)id);
		return TRUE;
	}
	return FALSE;
}

LOCAL UINT	vbe_w = 0, vbe_h = 0;		/* the mode last set */

LOCAL void vbe_mode( UINT w, UINT h )
{
	vbe_w = w;
	vbe_h = h;
	vbe_write(VBE_ENABLE, 0);
	vbe_write(VBE_XRES, (UH)w);
	vbe_write(VBE_YRES, (UH)h);
	vbe_write(VBE_BPP, 32);
	vbe_write(VBE_VIRT_WIDTH, (UH)w);
	vbe_write(VBE_VIRT_HEIGHT, (UH)h);
	vbe_write(VBE_X_OFFSET, 0);
	vbe_write(VBE_Y_OFFSET, 0);
	vbe_write(VBE_ENABLE, VBE_ON);
}

LOCAL INT vbe_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	if ( w < 320 || h < 200 || w > 4096 || h > 4096 || (UD)w * h * 4 > vbe_fb_size ) {
		return 0;
	}
	if ( vbe_va == 0 ) {
		vbe_va = (UBINT)knl_vmap_pa(vbe_fb, ( vbe_fb_size + PAGE_SIZE - 1 ) / PAGE_SIZE, VMAP_NOCACHE);
		if ( vbe_va == 0 ) {
			return E_NOMEM;
		}
	}
	vbe_mode(w, h);
	if ( vbe_read(VBE_XRES) != w || vbe_read(VBE_YRES) != h || vbe_read(VBE_BPP) != 32 ) {
		vbe_mode(hw->width, hw->height);	/* back to what it was */
		return 0;
	}
	hw->width = w;
	hw->height = h;
	hw->pitch = w * 4;
	hw->fb_pa = vbe_fb;
	hw->fb_va = vbe_va;
	hw->pages = 1;
	hw->flip = NULL;
	hw->page2_va = 0;
	return 1;
}

/* ---------------------------------------------------------------- the screen */

/*
 * The machine asleep. An Intel panel goes dark and its pipe keeps running.
 * QEMU's adapter is reset by S3, and has its mode set again.
 */
IMPORT void	knl_igfx_dark( BOOL dark );	/* x86_64/disp_igfx.c */

LOCAL ER gop_pm_suspend( void *arg, INT how )
{
	knl_igfx_dark(TRUE);
	return E_OK;
}

LOCAL void gop_pm_resume( void *arg, INT how )
{
	if ( vbe_regs != 0 && how == PM_S3 ) {
		if ( vbe_w == 0 ) {
			vbe_w = knl_bootinfo->fb.width;
			vbe_h = knl_bootinfo->fb.height;
		}
		vbe_mode(vbe_w, vbe_h);
		tm_printf((UB *)"disp: QEMU standard VGA: %d x %d again after the sleep\n", vbe_w, vbe_h);
	}
	knl_igfx_dark(FALSE);
}

LOCAL CONST T_PMDEV	gop_pm = { "screen", PM_ORD_DISPLAY, gop_pm_suspend, gop_pm_resume, NULL };

/*
 * The start-up setting screens=N: the view shown as N screens side by side
 * on the one there is, so that what is laid out by screen can be tried
 * without a second one (10.4.8)
 */
LOCAL void screens_setting( void )
{
	CONST char	*c = knl_bootinfo->cmdline;
	CONST char	*w = "screens=";
	INT		k;

	for ( ; *c != 0; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; w[k] != 0 && c[k] == w[k]; k++ ) ;
		if ( w[k] == 0 ) knl_disp_screens(c[k] - '0');
	}
}

IMPORT void	knl_disp_ext_start( void );	/* x86_64/disp_ext.c */

/* The start-up setting ext=1: what is plugged into the external ports is looked at */
LOCAL BOOL ext_wanted( void )
{
	CONST char	*c = knl_bootinfo->cmdline;

	for ( ; *c != '\0'; c++ ) {
		if ( ( c == knl_bootinfo->cmdline || c[-1] == ' ' )
		  && c[0] == 'e' && c[1] == 'x' && c[2] == 't' && c[3] == '=' && c[4] == '1' ) return TRUE;
	}
	return FALSE;
}

EXPORT INT knl_disp_hw_init( T_DISPHW *hw )
{
	CONST T_BIFB	*fb;
	UD		base, npages, size;
	UBINT		va;

	if ( hw == NULL ) {
		return E_PAR;
	}
	knl_fbcon_stage("screen: the GOP's framebuffer");
	(void)ts_pm_register(&gop_pm);
	fb = &knl_bootinfo->fb;
	if ( fb->pa == 0 || fb->format == BI_FB_NONE ) {
		return 0;			/* the loader found no screen to draw on */
	}
	if ( fb->format == BI_FB_MASK && !( fb->mask_r == 0x00ff0000 && fb->mask_g == 0x0000ff00
					 && fb->mask_b == 0x000000ff ) ) {
		tm_printf((UB *)"disp: the screen's pixels are laid out %08x %08x %08x; not taken\n",
			  fb->mask_r, fb->mask_g, fb->mask_b);
		return 0;
	}

	hw->width  = fb->width;
	hw->height = fb->height;
	hw->bpp    = 32;
	hw->pitch  = fb->stride;
	hw->format = ( fb->format == BI_FB_RGBX ) ? DISP_FMT_XBGR8888 : DISP_FMT_XRGB8888;
	hw->fb_pa  = fb->pa;
	hw->alpha  = DISP_ALPHA_ANY;
	hw->via    = "UEFI GOP";

	size = (UD)hw->pitch * hw->height;
	base = hw->fb_pa & PAGE_MASK;
	npages = ( hw->fb_pa + size - base + PAGE_SIZE - 1 ) / PAGE_SIZE;
	va = (UBINT)knl_vmap_pa(base, npages, VMAP_NOCACHE);
	if ( va == 0 ) {
		return E_NOMEM;
	}
	hw->fb_va = va + (UBINT)( hw->fb_pa - base );
	hw->pages = 1;
	hw->flip = NULL;
	hw->vsync = NULL;
	hw->latch_us = 0;
	hw->flip_start = FALSE;
	hw->page2_va = 0;

	knl_fbcon_stage("screen: the display engine under it");
	if ( hw->format == DISP_FMT_XRGB8888 && knl_igfx_probe(hw->fb_pa) ) {
		hw_kind = HW_IGFX;
		if ( ext_wanted() ) knl_disp_ext_start();
	} else if ( hw->format == DISP_FMT_XRGB8888 && vbe_probe(hw->fb_pa) ) {
		hw_kind = HW_STDVGA;
	}
	screens_setting();
	knl_fbcon_stage("screen: the back buffer");
	return 1;
}

/* The screen is the driver's from now: the screen console stops drawing on it */
EXPORT void knl_disp_hw_taken( void )
{
	knl_fbcon_stop();
}

EXPORT INT knl_disp_hw_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	if ( hw == NULL ) {
		return E_PAR;
	}
	switch ( hw_kind ) {
	case HW_IGFX:	return knl_igfx_setmode(hw, w, h);
	case HW_STDVGA:	return vbe_setmode(hw, w, h);
	default:	return 0;		/* the firmware's mode is the one there is */
	}
}

/* ---------------------------------------------------------------- for the screen's object */

/* What drives the screen, for 画面's record 1 */
EXPORT CONST char *knl_disp_hw_driver( void )
{
	switch ( hw_kind ) {
	case HW_IGFX:	return "Intel display engine (eDP panel)";
	case HW_STDVGA:	return "QEMU standard VGA";
	default:	return "UEFI GOP";
	}
}

/* The sizes it can be set to, up to max: how many */
EXPORT INT knl_disp_hw_modes( UH (*m)[2], INT max )
{
	CONST UH	usual[][2] = {
		{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
		{ 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 1920, 1200 }
	};
	T_DISPSPEC	s;
	INT		i, n = 0;

	if ( hw_kind == HW_IGFX ) {
		return knl_igfx_modes(m, max);
	}
	if ( hw_kind == HW_STDVGA ) {
		for ( i = 0; i < (INT)( sizeof(usual) / sizeof(usual[0]) ) && n < max; i++ ) {
			if ( (UD)usual[i][0] * usual[i][1] * 4 > vbe_fb_size ) continue;
			m[n][0] = usual[i][0];
			m[n][1] = usual[i][1];
			n++;
		}
		return n;
	}
	if ( max > 0 && ts_disp_ref(&s) >= E_OK ) {
		m[0][0] = (UH)s.width;
		m[0][1] = (UH)s.height;
		return 1;
	}
	return 0;
}

/*
 * How a size of another shape is laid on the panel (DISP_FIT_*), laid
 * now or kept for the next size; < 0 asks. -1 when it is not chosen here
 */
EXPORT INT knl_disp_hw_fit( INT fit, BOOL now )
{
	return ( hw_kind == HW_IGFX ) ? knl_igfx_fit(fit, now) : -1;
}

#endif /* X64 */
