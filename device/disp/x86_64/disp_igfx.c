/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_igfx.c (x64 PC)
 *	The size of the screen changed while the system runs, on Intel's
 *	integrated graphics of the 9th generation (Skylake, Kaby Lake,
 *	Amber Lake, Coffee Lake: UHD 620 8086:5917, UHD 615 8086:591C)
 *	driving the built-in panel over eDP (design 16.5.1, the study's
 *	5.14).
 *
 *	The firmware leaves the panel lit at its own size by the eDP
 *	transcoder from pipe A, with the primary plane of pipe A showing its
 *	framebuffer (the UEFI's GOP) from the start of the graphics address
 *	space, and pipe A's scaler stretching it when its mode is smaller
 *	than the panel. None of the link is touched here: the transcoder,
 *	the DDI and the PLL go on as the firmware set them, and the panel
 *	stays at its own size. What changes is the plane (its size, row
 *	length and surface), the pipe's source size, and the scaler's window
 *	on the panel; so a size that does not take never leaves the panel
 *	dark.
 *
 *	The surfaces. Two places in the global GTT take turns, both taken
 *	from the part of it the processor can reach (mappable) through the
 *	one keeper of the table both drivers of the device share
 *	(igt_ggtt.c): place 0 is its first 16MB, where the firmware maps the
 *	start of stolen memory and the GOP's framebuffer lies, taken over
 *	from the firmware as the system starts; place 1 is 16MB more
 *	wherever the keeper finds room, mapped onto pages of system memory
 *	taken once and kept. A new size is made ready in the place not
 *	shown, filled black, then shown; the processor reaches both through
 *	the aperture (GMADR, the second BAR) mapped write-combining.
 *
 *	The change is made inside one frame: the registers are written
 *	while the pipe's scan line is well before the vertical blank, with
 *	interrupts off, the plane's surface last, so that the pipe's source,
 *	the scaler, the plane's buffer and watermarks and the plane itself
 *	all take at the same blank. Then the surface the plane is showing is
 *	read until it is the new one, and after three frames the pipe's FIFO
 *	underrun is looked at; either failing puts back the registers as
 *	they were before, the same way.
 *
 *	The plane's share of the display buffer (DDB) and its watermarks
 *	are worked out for the new width by the PRM's two methods, with the
 *	memory latency the power controller reports (igt_pcode.c).
 *
 *	Every step is told on the console (KERNEL.TXT) and kept for
 *	DISPLAY.TXT (gfxdump.c); the records are written again soon after a
 *	change (bootlog.c).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/pcie.h>
#include "sysman/pfalloc.h"
#include "../disp_hw.h"
#include "disp_pc.h"
#include "sysdepend/x64/igt_ggtt.h"

IMPORT ER   knl_igt_pcode_rw( UW cmd, UW in, UW *out );	/* igt_pcode.c */
IMPORT void knl_bootlog_soon( void );			/* bootlog.c */

/* ---------------------------------------------------------------- registers */

#define R_PIPEDSL_A		0x70000		/* the scan line of pipe A */
#define R_PIPE_SRC_A		0x6001c
#define R_PS_CTRL_1A		0x68180
#define R_PS_WIN_POS_1A		0x68170
#define R_PS_WIN_SZ_1A		0x68174
#define R_PLANE_CTL_1A		0x70180
#define R_PLANE_STRIDE_1A	0x70188
#define R_PLANE_POS_1A		0x7018c
#define R_PLANE_SIZE_1A		0x70190
#define R_PLANE_SURF_1A		0x7019c
#define R_PLANE_OFFSET_1A	0x701a4
#define R_PLANE_SURFLIVE_1A	0x701ac
#define R_PLANE_WM_1A(l)	( 0x70240 + 4 * (l) )
#define R_PLANE_WM_TRANS_1A	0x70268
#define R_PLANE_BUF_CFG_1A	0x7027c
#define R_PIPECONF_B		0x71008
#define R_PIPECONF_C		0x72008
#define R_PIPECONF_EDP		0x7f008
#define R_TRANS_DDI_EDP		0x6f400
#define R_EDP_HTOTAL		0x6f000
#define R_EDP_VTOTAL		0x6f00c
#define R_EDP_VBLANK		0x6f010
#define R_EDP_LINK_M1		0x6f040
#define R_EDP_LINK_N1		0x6f044
#define R_DPLL_CTRL1		0x6c058
#define R_DE_PIPE_IMR_A		0x44404
#define R_DE_PIPE_IIR_A		0x44408
#define R_CHICKEN_PAR1_1	0x42080

#define PIPE_UNDERRUN		0x80000000U
#define SURF_ADDR		0xfffff000U	/* PLANE_SURF and PLANE_SURFLIVE: the surface's address */
#define HASH_MODE		0x00008000U	/* CHICKEN_PAR1_1: the newer hash of compressed surfaces */
#define PS_ON_MEDIUM		0x90000000U	/* enabled, high quality, the medium filter (no table) */
#define PCODE_MEM_LATENCY	0x06		/* GEN9: the memory latency, levels 0 to 3 */

#define PLACE_SIZE		( 16 * 1024 * 1024 )
#define PLACE_PAGES		( PLACE_SIZE / 4096 )
#define PLACE_ALIGN		( 256 * 1024 )		/* a linear surface's start */
#define RAM_BLOCK_ORDER		10			/* 4MB at a time */
#define RAM_BLOCKS		( PLACE_SIZE / ( 4096 << RAM_BLOCK_ORDER ) )
#define EVADE_LINES		64		/* written only this far before the blank, or more */
#define CPP			4

/* What is written to change the size, in the order it is written */
typedef struct {
	UW	pipe_src;
	UW	ps_ctrl, ps_pos, ps_sz;
	UW	wm[8], wm_trans, buf_cfg;
	UW	stride, pos, size, offset, ctl;
	UW	surf;
} IGREGS;

LOCAL BOOL	ig_found = FALSE;		/* an Intel display function */
LOCAL BOOL	ig_ok = FALSE;			/* set up as this driver can change */
LOCAL UBINT	ig_mmio = 0;
LOCAL UD	ig_gmadr = 0, ig_stolen = 0, ig_stolen_size = 0;
LOCAL UD	ig_place_off[2];		/* the places' GGTT addresses */
LOCAL BOOL	ig_place_have[2];
LOCAL UB	*ig_ap[2];			/* and where the processor writes them, through the aperture */
LOCAL UH	ig_dev = 0;
LOCAL UINT	ig_nw, ig_nh;			/* the panel */
LOCAL UINT	ig_htotal, ig_vstart;
LOCAL UD	ig_pixel_khz = 0;		/* 0: not known */
LOCAL INT	ig_lat0 = -1;			/* µs; < 0 not read yet */
LOCAL UW	ig_fw_wm0 = 0, ig_ps_on = 0;
LOCAL UINT	ig_place = 0;			/* the place shown */
LOCAL UD	ig_ram[RAM_BLOCKS * 2];		/* the system memory blocks of a place, by place */
LOCAL INT	ig_fit = DISP_FIT_ASPECT;
LOCAL UINT	ig_w, ig_h;			/* the size now */
LOCAL IGREGS	ig_fw;				/* as the firmware left them */

/* ---------------------------------------------------------------- the record */

#define LOG_MAX		8192
LOCAL char	ig_log[LOG_MAX];
LOCAL INT	ig_loglen = 0;

LOCAL void say( CONST char *s )
{
	INT	i;

	tm_putstring((CONST UB *)s);
	for ( i = 0; s[i] != '\0' && ig_loglen < LOG_MAX - 1; i++ ) ig_log[ig_loglen++] = s[i];
	ig_log[ig_loglen] = '\0';
}

#define sayf(...)	do { UB t_[240]; tm_sprintf(t_, (CONST UB *)__VA_ARGS__); say((CONST char *)t_); } while (0)

EXPORT INT knl_igfx_log( char *buf, INT max )
{
	INT	n = ( ig_loglen < max - 1 ) ? ig_loglen : max - 1;

	if ( max <= 0 ) return 0;
	knl_memcpy(buf, ig_log, n);
	buf[n] = '\0';
	return n;
}

/* ---------------------------------------------------------------- access */

LOCAL UW rd( UW off )
{
	return *(volatile UW *)( ig_mmio + off );
}

LOCAL void wr( UW off, UW v )
{
	*(volatile UW *)( ig_mmio + off ) = v;
}

LOCAL void read_regs( IGREGS *r )
{
	INT	l;

	r->pipe_src = rd(R_PIPE_SRC_A);
	r->ps_ctrl = rd(R_PS_CTRL_1A);
	r->ps_pos = rd(R_PS_WIN_POS_1A);
	r->ps_sz = rd(R_PS_WIN_SZ_1A);
	for ( l = 0; l < 8; l++ ) r->wm[l] = rd(R_PLANE_WM_1A(l));
	r->wm_trans = rd(R_PLANE_WM_TRANS_1A);
	r->buf_cfg = rd(R_PLANE_BUF_CFG_1A);
	r->stride = rd(R_PLANE_STRIDE_1A);
	r->pos = rd(R_PLANE_POS_1A);
	r->size = rd(R_PLANE_SIZE_1A);
	r->offset = rd(R_PLANE_OFFSET_1A);
	r->ctl = rd(R_PLANE_CTL_1A);
	r->surf = rd(R_PLANE_SURF_1A);
}

LOCAL void say_regs( CONST char *what, CONST IGREGS *r )
{
	sayf("igfx: %s: PIPE_SRC %08x PS_CTRL %08x PS_WIN_POS %08x PS_WIN_SZ %08x\n", what, r->pipe_src,
	     r->ps_ctrl, r->ps_pos, r->ps_sz);
	sayf("igfx:   PLANE_WM0-7 %08x %08x %08x %08x %08x %08x %08x %08x TRANS %08x BUF_CFG %08x\n",
	     r->wm[0], r->wm[1], r->wm[2], r->wm[3], r->wm[4], r->wm[5], r->wm[6], r->wm[7], r->wm_trans,
	     r->buf_cfg);
	sayf("igfx:   PLANE STRIDE %08x POS %08x SIZE %08x OFFSET %08x CTL %08x SURF %08x\n", r->stride,
	     r->pos, r->size, r->offset, r->ctl, r->surf);
}

/* ---------------------------------------------------------------- finding it */

/* The DP link's symbol clock DPLL0 gives, kHz, from DPLL_CTRL1's rate of it; 0 not known */
LOCAL UD link_khz( UW ctrl1 )
{
	CONST UW	mhz[8] = { 2700, 1350, 810, 1620, 1080, 2160, 0, 0 };

	if ( ( ctrl1 & 1 ) == 0 ) return 0;		/* DPLL0 not overridden: the rate is the default */
	return (UD)mhz[( ctrl1 >> 1 ) & 7] * 200;	/* the DPLL's MHz is half the link rate; a symbol is 10 bits */
}

EXPORT BOOL knl_igfx_probe( UD fb_pa )
{
	T_IGTDEV	dev;
	UW		v, ddi;
	UD		m, n;
	ER		er;

	knl_fbcon_stage("screen: the GGTT as the firmware left it");
	if ( knl_igt_dev(&dev) < E_OK ) {
		return FALSE;			/* no Intel graphics of the 9th generation */
	}
	knl_fbcon_stage("screen: the panel and the plane, whether the size can change");
	ig_found = TRUE;
	ig_dev = dev.pci.device;
	ig_mmio = dev.mmio;
	ig_gmadr = dev.gmadr_pa;
	ig_stolen = dev.stolen_pa;
	ig_stolen_size = dev.stolen_size;
	sayf("igfx: %04x:%04x, registers %lx, aperture %lx (%ld MB reachable), stolen %lx %ld MB, GGC %08x\n",
	     dev.pci.vendor, dev.pci.device, dev.pci.bar[0], ig_gmadr, (D)( dev.mappable >> 20 ), ig_stolen,
	     (D)( ig_stolen_size >> 20 ), dev.ggc);
	if ( fb_pa != ig_gmadr ) {
		sayf("igfx: the screen is at %lx, not the aperture: it keeps its size\n", fb_pa);
		return FALSE;
	}
	ddi = rd(R_TRANS_DDI_EDP);
	v = ( ddi >> 12 ) & 7;
	if ( ( ddi & 0x80000000U ) == 0 || ( v != 0 && v != 4 ) || ( rd(R_PIPECONF_EDP) & 0x80000000U ) == 0 ) {
		sayf("igfx: the eDP transcoder is not on from pipe A (%08x %08x): the screen keeps its size\n", ddi,
		     rd(R_PIPECONF_EDP));
		return FALSE;
	}
	if ( ( rd(R_PIPECONF_B) | rd(R_PIPECONF_C) ) & 0x80000000U ) {
		say("igfx: another pipe is on: the screen keeps its size\n");
		return FALSE;
	}
	read_regs(&ig_fw);
	if ( ( ig_fw.ctl & 0x80000000U ) == 0 || ( ( ig_fw.ctl >> 24 ) & 0xf ) != 4 || ( ( ig_fw.ctl >> 10 ) & 7 ) != 0
	  || ig_fw.surf != 0 ) {
		sayf("igfx: the plane is not linear XRGB8888 at 0 (%08x %08x): the screen keeps its size\n", ig_fw.ctl,
		     ig_fw.surf);
		return FALSE;
	}
	ig_nw = ( rd(R_EDP_HTOTAL) & 0x1fff ) + 1;
	ig_nh = ( rd(R_EDP_VTOTAL) & 0x1fff ) + 1;
	ig_htotal = ( ( rd(R_EDP_HTOTAL) >> 16 ) & 0x1fff ) + 1;
	ig_vstart = ( rd(R_EDP_VBLANK) & 0x1fff ) + 1;
	m = rd(R_EDP_LINK_M1) & 0xffffff;
	n = rd(R_EDP_LINK_N1) & 0xffffff;
	ig_pixel_khz = ( n != 0 && link_khz(rd(R_DPLL_CTRL1)) != 0 ) ? link_khz(rd(R_DPLL_CTRL1)) * m / n : 0;
	ig_w = ( ig_fw.size & 0x1fff ) + 1;
	ig_h = ( ( ig_fw.size >> 16 ) & 0x1fff ) + 1;
	ig_fw_wm0 = ig_fw.wm[0];
	ig_ps_on = ( ig_fw.ps_ctrl & 0x80000000U ) ? ig_fw.ps_ctrl : PS_ON_MEDIUM;
	/* the shape as the firmware lays it: over the whole panel whatever its shape is stretching */
	if ( ( ig_fw.ps_ctrl & 0x80000000U ) != 0 && ig_fw.ps_sz == ( ( ig_nw << 16 ) | ig_nh )
	  && (UD)ig_w * ig_nh != (UD)ig_h * ig_nw ) {
		ig_fit = DISP_FIT_STRETCH;
	}
	sayf("igfx: panel %d x %d (htotal %d, blank from line %d), pixel clock %ld kHz (link M/N %ld/%ld), plane %d x %d\n",
	     ig_nw, ig_nh, ig_htotal, ig_vstart, (D)ig_pixel_khz, (D)m, (D)n, ig_w, ig_h);
	say_regs("as the firmware left them", &ig_fw);
	if ( ig_nw < 640 || ig_nh < 480 || ig_vstart <= EVADE_LINES + 16 ) {
		say("igfx: the panel's timing is not usable: the screen keeps its size\n");
		return FALSE;
	}
	/*
	 * The two places taken now, before the drawing driver takes ranges
	 * of the mappable part: the GOP's screen with the rest of its 16MB,
	 * and another 16MB
	 */
	knl_fbcon_stage("screen: the two places in the GGTT");
	er = knl_igt_ggtt_claim(0, PLACE_SIZE, IGT_GGTT_DISPLAY, "display 0");
	if ( er < E_OK ) {
		sayf("igfx: the GOP's screen could not be taken over in the GGTT (%d): the screen keeps its size\n",
		     (INT)er);
		return FALSE;
	}
	ig_place_off[0] = 0;
	ig_place_have[0] = TRUE;
	er = knl_igt_ggtt_alloc(PLACE_SIZE, PLACE_ALIGN, IGT_GGTT_MAPPABLE, IGT_GGTT_DISPLAY, "display 1",
				&ig_place_off[1]);
	if ( er < E_OK ) {
		sayf("igfx: no second place in the GGTT (%d): the screen keeps its size\n", (INT)er);
		return FALSE;
	}
	ig_place_have[1] = TRUE;
	sayf("igfx: places at GGTT %lx and %lx\n", ig_place_off[0], ig_place_off[1]);
	ig_place = 0;
	ig_ok = TRUE;
	say("igfx: the size can be changed while the system runs\n");
	return TRUE;
}

/* ---------------------------------------------------------------- the surfaces */

/*
 * The entries of a place made to map its memory, up to 'pages' of it,
 * through the table's keeper (igt_ggtt.c), which flushes the GPU's
 * cache of them: place 0 stolen memory where its first 16MB keep clear
 * of the part the firmware may reserve at its top (8MB at most), system
 * memory otherwise. The place's processor mapping is made the first time.
 */
LOCAL ER place_map( UINT p, UINT pages )
{
	BOOL	stolen = (BOOL)( p == 0 && ig_stolen != 0 && ig_stolen_size >= PLACE_SIZE + 8 * 1024 * 1024 );
	UINT	k, n, per = 1U << RAM_BLOCK_ORDER;
	UD	pa;

	if ( !ig_place_have[p] ) {
		return E_NOEXS;
	}
	if ( ig_ap[p] == NULL ) {
		ig_ap[p] = (UB *)knl_vmap_pa(ig_gmadr + ig_place_off[p], PLACE_PAGES, VMAP_NOCACHE);
		if ( ig_ap[p] == NULL ) {
			say("igfx: the aperture could not be mapped\n");
			return E_NOMEM;
		}
	}
	if ( stolen ) {
		pa = ig_stolen;
		knl_igt_ggtt_set(ig_place_off[p], &pa, pages, TRUE);
	} else {
		for ( k = 0; k < RAM_BLOCKS && k * per < pages; k++ ) {
			if ( ig_ram[p * RAM_BLOCKS + k] == 0 ) {
				PFRAME	*pf = knl_alloc_pages(RAM_BLOCK_ORDER, ZONE_NORMAL, 0);

				if ( pf == NULL ) pf = knl_alloc_pages(RAM_BLOCK_ORDER, ZONE_DMA32, 0);
				if ( pf == NULL ) {
					sayf("igfx: no memory for place %d\n", p);
					return E_NOMEM;
				}
				ig_ram[p * RAM_BLOCKS + k] = knl_pf_to_pa(pf);
			}
			n = ( pages - k * per < per ) ? pages - k * per : per;
			knl_igt_ggtt_set(ig_place_off[p] + (UD)k * per * 4096, &ig_ram[p * RAM_BLOCKS + k], n, TRUE);
		}
	}
	sayf("igfx: place %d (GGTT %lx): %d pages of %s, first entry %016lx\n", p, ig_place_off[p], pages,
	     stolen ? "stolen memory" : "system memory", knl_igt_ggtt_pte(ig_place_off[p]));
	return E_OK;
}

/* ---------------------------------------------------------------- the watermarks */

/* The memory latency of level 0, µs: as the power controller says, or what the firmware's watermark allows */
LOCAL INT latency0( void )
{
	UW	v = 0;
	ER	er;

	if ( ig_lat0 >= 0 ) {
		return ig_lat0;
	}
	er = knl_igt_pcode_rw(PCODE_MEM_LATENCY, 0, &v);
	if ( er >= E_OK ) {
		ig_lat0 = (INT)( v & 0xff );
		sayf("igfx: memory latency of levels 0-3 %d %d %d %d us (pcode)\n", v & 0xff, ( v >> 8 ) & 0xff,
		     ( v >> 16 ) & 0xff, ( v >> 24 ) & 0xff);
		if ( ig_lat0 == 0 ) {
			ig_lat0 = 2;			/* level 0 read as 0: 2µs added, as the PRM's note says */
			say("igfx: level 0 read as 0: 2 us taken\n");
		}
	} else if ( ig_pixel_khz != 0 && ( ig_fw_wm0 & 0x3ff ) > 1 ) {
		/* method 1 gave the firmware's blocks - 1 at most */
		ig_lat0 = (INT)( (UD)( ( ig_fw_wm0 & 0x3ff ) - 1 ) * 512 * 1000 / ( ig_pixel_khz * CPP ) );
		sayf("igfx: pcode not read (%d): latency at most %d us from the firmware's watermark\n", (INT)er,
		     ig_lat0);
	} else {
		ig_lat0 = 0;
	}
	return ig_lat0;
}

/*
 * Level 0's watermark for a linear plane w pixels wide, as
 * enable | lines << 14 | blocks; 0 when it cannot be worked out or does
 * not fit the plane's share of the buffer (alloc blocks)
 */
LOCAL UW wm0_of( UINT w, UINT alloc, INT lat )
{
	UD	m1, m2, sel, pbpl, line_ns, blocks, lines;

	if ( ig_pixel_khz == 0 || lat <= 0 ) {
		return 0;
	}
	m1 = (UD)lat * ig_pixel_khz * CPP / 512;			/* blocks, in thousandths */
	pbpl = ( (UD)w * CPP + 511 ) / 512;
	line_ns = (UD)ig_htotal * 1000000 / ig_pixel_khz;
	m2 = ( ( (UD)lat * ig_pixel_khz + (UD)ig_htotal * 1000 - 1 ) / ( (UD)ig_htotal * 1000 ) ) * pbpl * 1000;
	sel = ( (UD)lat * 1000 >= line_ns && m2 < m1 ) ? m2 : m1;
	blocks = ( sel + 999 ) / 1000 + 1;
	lines = ( sel + pbpl * 1000 - 1 ) / ( pbpl * 1000 );
	if ( blocks > alloc || lines > 31 ) {
		return 0;
	}
	return 0x80000000U | (UW)( lines << 14 ) | (UW)blocks;
}

/* ---------------------------------------------------------------- the change */

/* The scan line well before the blank: TRUE with interrupts off (*p_s to put them back) */
LOCAL BOOL evade( UINT *p_s )
{
	UINT	line, s, tries;

	for ( tries = 0; tries < 500; tries++ ) {
		line = rd(R_PIPEDSL_A) & 0x1fff;
		if ( line + EVADE_LINES < ig_vstart ) {
			DI(s);
			line = rd(R_PIPEDSL_A) & 0x1fff;
			if ( line + EVADE_LINES < ig_vstart ) {
				*p_s = s;
				return TRUE;
			}
			EI(s);
		}
		WaitUsec(100);
	}
	return FALSE;
}

LOCAL void write_regs( CONST IGREGS *r )
{
	INT	l;

	wr(R_PIPE_SRC_A, r->pipe_src);
	wr(R_PS_CTRL_1A, r->ps_ctrl);
	wr(R_PS_WIN_POS_1A, r->ps_pos);
	wr(R_PS_WIN_SZ_1A, r->ps_sz);		/* the scaler's registers take at the next blank */
	for ( l = 0; l < 8; l++ ) wr(R_PLANE_WM_1A(l), r->wm[l]);
	wr(R_PLANE_WM_TRANS_1A, r->wm_trans);
	wr(R_PLANE_BUF_CFG_1A, r->buf_cfg);
	wr(R_PLANE_STRIDE_1A, r->stride);
	wr(R_PLANE_POS_1A, r->pos);
	wr(R_PLANE_SIZE_1A, r->size);
	wr(R_PLANE_OFFSET_1A, r->offset);
	wr(R_PLANE_CTL_1A, r->ctl);
	wr(R_PLANE_SURF_1A, r->surf);		/* last: the plane's registers take with it */
	(void)rd(R_PLANE_SURF_1A);
}

/*
 * The registers written within one frame and checked: E_OK when the
 * plane shows the new surface and no underrun came, else E_IO (what
 * was written stays; the caller puts back)
 */
LOCAL ER commit( CONST IGREGS *r, CONST char *what )
{
	UINT	s, line, ms;
	UW	imr, iir;

	imr = rd(R_DE_PIPE_IMR_A);
	if ( imr & PIPE_UNDERRUN ) {
		wr(R_DE_PIPE_IMR_A, imr & ~PIPE_UNDERRUN);	/* latched in IIR; nothing is enabled to interrupt */
	}
	wr(R_DE_PIPE_IIR_A, PIPE_UNDERRUN);
	if ( !evade(&s) ) {
		sayf("igfx: %s: the scan line never came before the blank (%d)\n", what,
		     rd(R_PIPEDSL_A) & 0x1fff);
		wr(R_DE_PIPE_IMR_A, imr);
		return E_TMOUT;
	}
	line = rd(R_PIPEDSL_A) & 0x1fff;
	write_regs(r);
	EI(s);
	/* the live address is in bits 31:12; the bits below carry the plane's state (0x20 seen) */
	for ( ms = 0; ms < 100; ms++ ) {
		if ( ( rd(R_PLANE_SURFLIVE_1A) & SURF_ADDR ) == ( r->surf & SURF_ADDR ) ) break;
		tk_dly_tsk(1);
	}
	tk_dly_tsk(50);					/* three frames */
	iir = rd(R_DE_PIPE_IIR_A);
	wr(R_DE_PIPE_IMR_A, imr);
	sayf("igfx: %s: written at line %d, the plane shows %08x after %d ms, underrun %s\n", what, line,
	     rd(R_PLANE_SURFLIVE_1A), ms, ( iir & PIPE_UNDERRUN ) ? "YES" : "none");
	if ( ms >= 100 || ( iir & PIPE_UNDERRUN ) != 0 ) {
		return E_IO;
	}
	return E_OK;
}

/* Where the scaler lays w x h on the panel, for the shape kept */
LOCAL void window_of( UINT w, UINT h, UINT *x, UINT *y, UINT *ww, UINT *wh )
{
	UD	a = (UD)w * ig_nh, b = (UD)h * ig_nw;

	*x = *y = 0;
	*ww = ig_nw;
	*wh = ig_nh;
	if ( ig_fit == DISP_FIT_STRETCH || a == b ) {
		return;
	}
	if ( a > b ) {					/* wider than the panel: bands above and below */
		*wh = (UINT)( ( (UD)h * ig_nw + w / 2 ) / w ) & ~1U;
		*y = ( ig_nh - *wh ) / 2;
	} else {					/* narrower: bands at the sides */
		*ww = (UINT)( ( (UD)w * ig_nh + h / 2 ) / h ) & ~1U;
		*x = ( ig_nw - *ww ) / 2;
	}
}

/* The registers for w x h shown from place p */
LOCAL ER regs_for( UINT w, UINT h, UINT p, CONST IGREGS *now, IGREGS *r )
{
	UINT	x, y, ww, wh, pitch = ( w * CPP + 63 ) & ~63U, alloc, l;
	UW	wm, wm_old;
	INT	lat;

	*r = *now;
	r->pipe_src = ( ( w - 1 ) << 16 ) | ( h - 1 );
	if ( w == ig_nw && h == ig_nh ) {
		r->ps_ctrl = 0;
		r->ps_pos = 0;
		r->ps_sz = 0;
	} else {
		window_of(w, h, &x, &y, &ww, &wh);
		r->ps_ctrl = ig_ps_on;
		r->ps_pos = ( x << 16 ) | y;
		r->ps_sz = ( ww << 16 ) | wh;
	}
	alloc = ( ( now->buf_cfg >> 16 ) & 0x3ff ) - ( now->buf_cfg & 0x3ff ) + 1;
	lat = latency0();
	wm_old = wm0_of(( ( now->size ) & 0x1fff ) + 1, alloc, lat);
	wm = wm0_of(w, alloc, lat);
	if ( wm_old != 0 && ( wm_old & 0x7ffffff ) != ( ig_fw_wm0 & 0x7ffffff ) ) {
		sayf("igfx: level 0 worked out for the plane now %08x, the firmware's %08x: the larger kept\n",
		     wm_old, ig_fw_wm0);
	}
	if ( wm == 0 ) {
		if ( ig_pixel_khz == 0 || lat <= 0 ) {
			wm = ig_fw_wm0;			/* nothing to work it out from: as the firmware had it */
			say("igfx: watermark not worked out: the firmware's level 0 kept\n");
		} else {
			sayf("igfx: %d x %d does not fit the plane's %d blocks of the buffer\n", w, h, alloc);
			return E_NOSPT;
		}
	}
	if ( ( ig_fw_wm0 & 0x80000000U ) != 0 && ( ig_fw_wm0 & 0x3ff ) > ( wm & 0x3ff ) ) {
		wm = ( wm & ~0x3ffU ) | ( ig_fw_wm0 & 0x3ff );
	}
	if ( ( ig_fw_wm0 & 0x80000000U ) != 0 && ( ( ig_fw_wm0 >> 14 ) & 0x1f ) > ( ( wm >> 14 ) & 0x1f ) ) {
		wm = ( wm & ~( 0x1fU << 14 ) ) | ( ig_fw_wm0 & ( 0x1fU << 14 ) );
	}
	r->wm[0] = wm;
	for ( l = 1; l < 8; l++ ) r->wm[l] = now->wm[l] & ~0x80000000U;	/* the deeper levels off */
	r->wm_trans = now->wm_trans & ~0x80000000U;
	sayf("igfx: %d x %d: latency %d us, %d blocks of the buffer, level 0 %d blocks %d lines\n", w, h, lat,
	     alloc, wm & 0x3ff, ( wm >> 14 ) & 0x1f);
	r->stride = pitch / 64;
	r->pos = 0;
	r->size = ( ( h - 1 ) << 16 ) | ( w - 1 );
	r->offset = 0;
	r->surf = (UW)ig_place_off[p];
	return E_OK;
}

EXPORT INT knl_igfx_setmode( T_DISPHW *hw, UINT w, UINT h )
{
	IGREGS	now, want;
	UINT	p, pitch, pages;
	ER	er;

	if ( !ig_ok || hw == NULL ) {
		return 0;
	}
	if ( w < 320 || h < 200 || w > ig_nw || h > ig_nh ) {
		sayf("igfx: %d x %d: outside 320 x 200 to %d x %d\n", w, h, ig_nw, ig_nh);
		return 0;
	}
	pitch = ( w * CPP + 63 ) & ~63U;
	pages = (UINT)( ( (UD)pitch * h + 4095 ) / 4096 );
	if ( pages > PLACE_PAGES ) {
		return 0;
	}
	sayf("igfx: to %d x %d (%s)\n", w, h, ( ig_fit == DISP_FIT_STRETCH ) ? "stretched" : "keeping its shape");
	p = 1 - ig_place;
	er = place_map(p, pages);
	if ( er < E_OK ) {
		return er;
	}
	knl_memset(ig_ap[p], 0, (SZ)pitch * h);
	DeviceBarrier();				/* out of the write-combining buffers */

	read_regs(&now);
	er = regs_for(w, h, p, &now, &want);
	if ( er < E_OK ) {
		return 0;
	}
	say_regs("before", &now);
	say_regs("to write", &want);
	er = commit(&want, "the new size");
	if ( er < E_OK ) {
		(void)commit(&now, "put back");
		knl_bootlog_soon();
		return 0;
	}
	ig_place = p;
	ig_w = w;
	ig_h = h;
	hw->width = w;
	hw->height = h;
	hw->pitch = pitch;
	hw->fb_pa = ig_gmadr + ig_place_off[p];
	hw->fb_va = (UBINT)ig_ap[p];
	hw->pages = 1;
	hw->flip = NULL;
	hw->page2_va = 0;
	sayf("igfx: now %d x %d from place %d\n", w, h, p);
	knl_bootlog_soon();
	return 1;
}

EXPORT INT knl_igfx_fit( INT fit, BOOL at_once )
{
	IGREGS	cur, want;

	if ( !ig_ok ) {
		return -1;
	}
	if ( fit < 0 || fit == ig_fit ) {
		return ig_fit;
	}
	ig_fit = ( fit == DISP_FIT_STRETCH ) ? DISP_FIT_STRETCH : DISP_FIT_ASPECT;
	if ( !at_once || ( ig_w == ig_nw && ig_h == ig_nh ) ) {
		return ig_fit;				/* nothing scaled: taken with the next size */
	}
	read_regs(&cur);
	want = cur;
	{
		UINT	x, y, ww, wh;

		window_of(ig_w, ig_h, &x, &y, &ww, &wh);
		want.ps_ctrl = ig_ps_on;
		want.ps_pos = ( x << 16 ) | y;
		want.ps_sz = ( ww << 16 ) | wh;
	}
	sayf("igfx: %d x %d %s\n", ig_w, ig_h, ( ig_fit == DISP_FIT_STRETCH ) ? "stretched" : "keeping its shape");
	if ( commit(&want, "the shape") < E_OK ) {
		(void)commit(&cur, "put back");
	}
	knl_bootlog_soon();
	return ig_fit;
}

/* The sizes offered: the usual ones up to the panel's own, and the one now */
EXPORT INT knl_igfx_modes( UH (*m)[2], INT max )
{
	CONST UH	usual[][2] = {
		{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
		{ 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 },
		{ 1920, 1200 }, { 2048, 1152 }, { 2560, 1440 }, { 2560, 1600 }, { 3200, 1800 }, { 3840, 2160 }
	};
	INT	i, n = 0;
	BOOL	panel = FALSE, cur = FALSE;

	if ( !ig_ok ) {
		return 0;
	}
	for ( i = 0; i < (INT)( sizeof(usual) / sizeof(usual[0]) ) && n < max; i++ ) {
		if ( usual[i][0] > ig_nw || usual[i][1] > ig_nh ) continue;
		m[n][0] = usual[i][0];
		m[n][1] = usual[i][1];
		if ( usual[i][0] == ig_nw && usual[i][1] == ig_nh ) panel = TRUE;
		if ( usual[i][0] == ig_w && usual[i][1] == ig_h ) cur = TRUE;
		n++;
	}
	if ( !panel && n < max ) {
		m[n][0] = (UH)ig_nw;
		m[n][1] = (UH)ig_nh;
		n++;
	}
	if ( !cur && n < max && !( ig_w == ig_nw && ig_h == ig_nh ) ) {
		m[n][0] = (UH)ig_w;
		m[n][1] = (UH)ig_h;
		n++;
	}
	return n;
}

EXPORT void knl_idisp_hash_mode( BOOL new_hash )
{
	UW	v;

	if ( !ig_found ) {
		return;
	}
	v = rd(R_CHICKEN_PAR1_1);
	wr(R_CHICKEN_PAR1_1, new_hash ? ( v | HASH_MODE ) : ( v & ~HASH_MODE ));
	sayf("igfx: CHICKEN_PAR1_1 %08x -> %08x\n", v, rd(R_CHICKEN_PAR1_1));
}

/*
 * The panel dark or lit for the machine's sleep: the backlight's enable in
 * the PCH's panel power control. The link and the pipe keep running.
 */
#define PP_CONTROL	0xc7204
#define PP_BL_ENABLE	( 1U << 2 )

/* The registers, for what else is on the display engine (disp_ext.c); 0 without an Intel display */
EXPORT UBINT knl_igfx_mmio( void )
{
	return ig_found ? ig_mmio : 0;
}

EXPORT void knl_igfx_dark( BOOL dark )
{
	UW	v;

	if ( !ig_found || ig_mmio == 0 ) return;
	v = rd(PP_CONTROL);
	wr(PP_CONTROL, dark ? ( v & ~PP_BL_ENABLE ) : ( v | PP_BL_ENABLE ));
	tm_printf((UB *)"igfx: the backlight %s: PP_CONTROL %08x -> %08x\n", dark ? "off" : "on", v, rd(PP_CONTROL));
}

#endif /* X64 */
