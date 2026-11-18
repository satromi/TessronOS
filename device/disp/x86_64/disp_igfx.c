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
#include <ts/board.h>

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
#define R_CUR_BASE_A		0x70084
#define R_CUR_WM_A(l)		( 0x70140 + 4 * (l) )
#define R_CUR_WM_TRANS_A	0x70168
#define R_CUR_BUF_CFG_A		0x7017c
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
	UW	cur_wm[8], cur_trans, cur_buf;
	UW	stride, pos, size, offset, ctl;
	UW	surf;
} IGREGS;

LOCAL BOOL	ig_found = FALSE;		/* an Intel display function */
LOCAL BOOL	ig_ok = FALSE;			/* set up as this driver can change */
LOCAL BOOL	ig_wm_on = TRUE;		/* the start-up setting igfx.wm=0: the old way, level 0 only */
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

/*
 * Everything told, from the start: the record begins in LOG_FIRST bytes
 * of its own and is taken anew twice as large when it is full. The
 * record before is not given back, since knl_igfx_log may be reading it.
 * Where there is no room (or the call comes from an interrupt, which may
 * not take memory) what does not fit is counted in ig_loglost, and the
 * record says how much.
 */
#define LOG_FIRST	8192
LOCAL char	ig_log0[LOG_FIRST];
LOCAL char	*ig_log = ig_log0;
LOCAL INT	ig_logcap = LOG_FIRST;
LOCAL INT	ig_loglen = 0;
LOCAL INT	ig_loglost = 0;

LOCAL void wm_apply( CONST char *what );

/* Room for n more bytes and the nought after them */
LOCAL BOOL log_room( INT n )
{
	char	*b;
	INT	cap = ig_logcap;

	if ( ig_loglen + n < ig_logcap ) {
		return TRUE;
	}
	if ( in_indp() ) {
		return FALSE;
	}
	while ( ig_loglen + n >= cap ) cap *= 2;
	b = (char *)Kmalloc((SZ)cap);
	if ( b == NULL ) {
		return FALSE;
	}
	knl_memcpy(b, ig_log, ig_loglen + 1);
	ig_log = b;
	ig_logcap = cap;
	return TRUE;
}

LOCAL void say( CONST char *s )
{
	INT	i, n = (INT)knl_strlen(s);

	tm_putstring((CONST UB *)s);
	if ( !log_room(n) ) {
		ig_loglost += n;
		return;
	}
	for ( i = 0; i < n; i++ ) ig_log[ig_loglen++] = s[i];
	ig_log[ig_loglen] = '\0';
}

#define sayf(...)	do { UB t_[240]; tm_sprintf(t_, (CONST UB *)__VA_ARGS__); say((CONST char *)t_); } while (0)

/*
 * The record into buf, up to max - 1 bytes and a nought: how many were
 * put; with a line at the end of how many bytes the record could not keep
 */
EXPORT INT knl_igfx_log( char *buf, INT max )
{
	INT	n = ( ig_loglen < max - 1 ) ? ig_loglen : max - 1;

	if ( max <= 0 ) return 0;
	knl_memcpy(buf, ig_log, n);
	buf[n] = '\0';
	if ( ig_loglost > 0 && n < max - 1 ) {
		UB	t[64];
		INT	k;

		(void)tm_sprintf(t, (CONST UB *)"(%d bytes told were not kept: no memory)\n", ig_loglost);
		for ( k = 0; t[k] != 0 && n < max - 1; k++ ) buf[n++] = (char)t[k];
		buf[n] = '\0';
	}
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
	for ( l = 0; l < 8; l++ ) r->cur_wm[l] = rd(R_CUR_WM_A(l));
	r->cur_trans = rd(R_CUR_WM_TRANS_A);
	r->cur_buf = rd(R_CUR_BUF_CFG_A);
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
	sayf("igfx:   CUR_WM0-7 %08x %08x %08x %08x %08x %08x %08x %08x TRANS %08x CUR_BUF_CFG %08x\n",
	     r->cur_wm[0], r->cur_wm[1], r->cur_wm[2], r->cur_wm[3], r->cur_wm[4], r->cur_wm[5], r->cur_wm[6],
	     r->cur_wm[7], r->cur_trans, r->cur_buf);
	sayf("igfx:   PLANE STRIDE %08x POS %08x SIZE %08x OFFSET %08x CTL %08x SURF %08x\n", r->stride,
	     r->pos, r->size, r->offset, r->ctl, r->surf);
}

/* ---------------------------------------------------------------- finding it */

/* The DP link's symbol clock DPLL0 gives, kHz, from DPLL_CTRL1's rate of it; 0 not known */
/*
 * DDI A's link symbol clock in kHz: the DPLL DPLL_CTRL2 selects for it
 * (DPLL0 when not overridden), and that DPLL's rate in DPLL_CTRL1 (six
 * bits a DPLL: override, the rate's code, SSC, HDMI). The code names
 * half the symbol clock in 100 kHz: 810 for 1.62 Gbps, 1350 for 2.7, 2700
 * for 5.4, 1620, 1080 and 2160 for eDP 1.4's 3.24, 2.16 and 4.32. 0 when
 * the DPLL's rate is not given there.
 */
#define R_DPLL_CTRL2		0x6c05c

LOCAL INT ddi_a_dpll( UW ctrl2 )
{
	return ( ctrl2 & 1 ) ? (INT)( ( ctrl2 >> 1 ) & 3 ) : 0;
}

LOCAL UD link_khz( UW ctrl1, UW ctrl2 )
{
	CONST UW	code[8] = { 2700, 1350, 810, 1620, 1080, 2160, 0, 0 };
	UW		f = ctrl1 >> ( 6 * ddi_a_dpll(ctrl2) );

	if ( ( f & 1 ) == 0 ) return 0;
	return (UD)code[( f >> 1 ) & 7] * 200;
}

/* ---------------------------------------------------------------- what the outputs are, for the records */

IMPORT INT	knl_igfx_aux_ch( INT port );		/* disp_igfx_aux.c */
IMPORT INT	knl_igfx_dpcd_read( INT ch, UW addr, UB *buf, INT len );

#define R_DDI_BUF_CTL(p)	( 0x64000 + 0x100 * (p) )
#define R_TRANS_DDI(t)		( 0x60400 + 0x1000 * (t) )
#define R_SDEISR		0xc4000
#define R_SHOTPLUG_CTL		0xc4030
#define R_SHOTPLUG_CTL2		0xc403c
#define VBT_OFF			0x400		/* the VBT in the OpRegion's fourth mailbox */
#define BDB_GENERAL_DEFS	2

/*
 * Once at the start, read only: each DDI's buffer control (A to E), the
 * PCH's hot plug state and controls, pipes B's and C's PIPECONF and the
 * transcoders' DDI function controls (A, B, C, eDP); and the VBT's
 * child devices (the connectors and how they are wired: each one's
 * type, DVO port, DDC pin and AUX channel as its bytes say), where the
 * OpRegion (ASLS) holds a VBT
 */
LOCAL void outputs_say( void )
{
	UW	asls;
	INT	p;
	char	t[300];
	INT	n;

	n = tm_sprintf((UB *)t, (CONST UB *)"igfx: outputs: DDI_BUF_CTL");
	for ( p = 0; p < 5; p++ ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %c %08x", 'A' + p, rd(R_DDI_BUF_CTL(p)));
	tm_sprintf((UB *)t + n, (CONST UB *)"; SDEISR %08x SHOTPLUG_CTL %08x SHOTPLUG_CTL2 %08x\n", rd(R_SDEISR),
		   rd(R_SHOTPLUG_CTL), rd(R_SHOTPLUG_CTL2));
	say(t);
	sayf("igfx: outputs: PIPECONF B %08x C %08x eDP %08x; TRANS_DDI_FUNC_CTL A %08x B %08x C %08x eDP %08x\n",
	     rd(R_PIPECONF_B), rd(R_PIPECONF_C), rd(R_PIPECONF_EDP), rd(R_TRANS_DDI(0)), rd(R_TRANS_DDI(1)),
	     rd(R_TRANS_DDI(2)), rd(R_TRANS_DDI_EDP));

	asls = ts_pcie_cfg_read(0, 2, 0, 0xfc, 4);
	if ( asls == 0 || asls == 0xffffffffU ) {
		say("igfx: VBT: no OpRegion\n");
		return;
	}
	{
		CONST volatile UB	*v = (CONST volatile UB *)( DEV_VA_BASE + (UD)asls + VBT_OFF );
		CONST volatile UB	*bdb, *blk, *end;
		UW			bdb_off;
		UH			hsz, bsz;

		if ( v[0] != '$' || v[1] != 'V' || v[2] != 'B' || v[3] != 'T' ) {
			say("igfx: VBT: none in the OpRegion\n");
			return;
		}
		bdb_off = (UW)v[0x1c] | ( (UW)v[0x1d] << 8 ) | ( (UW)v[0x1e] << 16 ) | ( (UW)v[0x1f] << 24 );
		if ( bdb_off < 0x20 || bdb_off > 0x1000 ) {
			sayf("igfx: VBT: the data block's offset %x not taken\n", bdb_off);
			return;
		}
		bdb = v + bdb_off;
		hsz = (UH)( bdb[0x12] | ( bdb[0x13] << 8 ) );
		bsz = (UH)( bdb[0x14] | ( bdb[0x15] << 8 ) );
		sayf("igfx: VBT: version %d, data block version %d, %d bytes\n", v[0x14] | ( v[0x15] << 8 ),
		     bdb[0x10] | ( bdb[0x11] << 8 ), bsz);
		blk = bdb + hsz;
		end = bdb + bsz;
		while ( blk + 3 <= end ) {
			UB	id = blk[0];
			UH	sz = (UH)( blk[1] | ( blk[2] << 8 ) );

			if ( sz == 0 || blk + 3 + sz > end ) break;
			if ( id == BDB_GENERAL_DEFS && sz > 5 && blk[3 + 4] != 0 ) {
				UB			csz = blk[3 + 4];
				INT			i, k, cnt = ( sz - 5 ) / csz;
				CONST volatile UB	*c;

				for ( i = 0; i < cnt; i++ ) {
					c = blk + 3 + 5 + i * csz;
					if ( ( c[2] | c[3] ) == 0 ) continue;		/* no device in the slot */
					n = tm_sprintf((UB *)t, (CONST UB *)"igfx: VBT child %d: handle %04x type %04x DVO port %d "
						       "DDC pin %d; bytes", i, c[0] | ( c[1] << 8 ), c[2] | ( c[3] << 8 ), c[16], c[19]);
					for ( k = 0; k < csz && k < 40; k++ ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %02x", c[k]);
					tm_sprintf((UB *)t + n, (CONST UB *)"\n");
					say(t);
				}
			}
			blk += 3 + sz;
		}
	}
}

/*
 * The VBT's i-th child device slot (the General Definitions block), its
 * bytes into buf, up to max: how many (0 for an empty slot), or E_NOEXS
 * past the last one or where there is no VBT. Read only; for the external
 * ports (disp_ext.c), which go by its DVO port, AUX channel and DDC pin.
 */
EXPORT INT knl_igfx_vbt_child( INT i, UB *buf, INT max )
{
	UW	asls;
	CONST volatile UB	*v, *bdb, *blk, *end;
	UW	bdb_off;
	UH	hsz, bsz;

	asls = ts_pcie_cfg_read(0, 2, 0, 0xfc, 4);
	if ( asls == 0 || asls == 0xffffffffU || i < 0 ) return E_NOEXS;
	v = (CONST volatile UB *)( DEV_VA_BASE + (UD)asls + VBT_OFF );
	if ( v[0] != '$' || v[1] != 'V' || v[2] != 'B' || v[3] != 'T' ) return E_NOEXS;
	bdb_off = (UW)v[0x1c] | ( (UW)v[0x1d] << 8 ) | ( (UW)v[0x1e] << 16 ) | ( (UW)v[0x1f] << 24 );
	if ( bdb_off < 0x20 || bdb_off > 0x1000 ) return E_NOEXS;
	bdb = v + bdb_off;
	hsz = (UH)( bdb[0x12] | ( bdb[0x13] << 8 ) );
	bsz = (UH)( bdb[0x14] | ( bdb[0x15] << 8 ) );
	blk = bdb + hsz;
	end = bdb + bsz;
	while ( blk + 3 <= end ) {
		UB	id = blk[0];
		UH	sz = (UH)( blk[1] | ( blk[2] << 8 ) );

		if ( sz == 0 || blk + 3 + sz > end ) break;
		if ( id == BDB_GENERAL_DEFS && sz > 5 && blk[3 + 4] != 0 ) {
			UB			csz = blk[3 + 4];
			INT			cnt = ( sz - 5 ) / csz, k;
			CONST volatile UB	*c;

			if ( i >= cnt ) return E_NOEXS;
			c = blk + 3 + 5 + i * csz;
			if ( ( c[2] | c[3] ) == 0 ) return 0;		/* no device in the slot */
			for ( k = 0; k < csz && k < max; k++ ) buf[k] = c[k];
			return k;
		}
		blk += 3 + sz;
	}
	return E_NOEXS;
}

/*
 * Once at the start: the panel's DPCD over DDI A's AUX channel, read
 * only: 000h-00Fh (revision, the most link rate and lanes), 02Eh (ALPM),
 * 070h-074h (PSR: its version, capabilities, PSR2's selective update
 * granularity), 2200h-220Fh (the extended receiver capabilities) when
 * 00Eh says they are there, 700h-701h (the eDP revision and general
 * capabilities). What failed is told, and the start goes on.
 */
LOCAL void dpcd_bytes( CONST char *what, UW addr, INT len )
{
	UB	b[16];
	char	t[160];
	INT	n, i, k;

	n = knl_igfx_dpcd_read(knl_igfx_aux_ch(0), addr, b, len);
	if ( n < E_OK ) {
		sayf("igfx: DPCD %s (%04x): not read (%d)\n", what, addr, n);
		return;
	}
	k = tm_sprintf((UB *)t, (CONST UB *)"igfx: DPCD %s (%04x):", what, addr);
	for ( i = 0; i < n; i++ ) k += tm_sprintf((UB *)t + k, (CONST UB *)" %02x", b[i]);
	tm_sprintf((UB *)t + k, (CONST UB *)"\n");
	say(t);
}

LOCAL void dpcd_say( void )
{
	UB	ext = 0;

	dpcd_bytes("receiver capabilities", 0x000, 16);
	dpcd_bytes("link set (LINK_BW_SET, LANE_COUNT_SET)", 0x100, 2);
	dpcd_bytes("ALPM", 0x02e, 1);
	dpcd_bytes("PSR", 0x070, 5);
	if ( knl_igfx_dpcd_read(knl_igfx_aux_ch(0), 0x00e, &ext, 1) == 1 && ( ext & 0x80 ) != 0 ) {
		dpcd_bytes("extended receiver capabilities", 0x2200, 16);
	}
	dpcd_bytes("eDP", 0x700, 2);
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
	outputs_say();
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
	dpcd_say();
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
	{
		UW	c1 = rd(R_DPLL_CTRL1), c2 = rd(R_DPLL_CTRL2);
		UD	lk = link_khz(c1, c2);

		ig_pixel_khz = ( n != 0 && lk != 0 ) ? lk * m / n : 0;
		sayf("igfx: DDI A on DPLL%d (DPLL_CTRL1 %08x CTRL2 %08x): link symbol clock %d kHz, link M/N %x/%x, "
		     "pixel clock %d kHz\n", ddi_a_dpll(c2), c1, c2, (INT)lk, m, n, (INT)ig_pixel_khz);
	}
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
	{
		UB	v[4];

		if ( knl_bootarg(knl_bootargs, "igfx.wm", v, sizeof(v)) && v[0] == '0' ) {
			ig_wm_on = FALSE;
			say("igfx: igfx.wm=0: the watermarks as the firmware left them (level 0 only on a change)\n");
		}
	}
	wm_apply("the watermarks at the start");
	knl_igfx_psr_start();
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
	for ( l = 0; l < 8; l++ ) wr(R_CUR_WM_A(l), r->cur_wm[l]);
	wr(R_CUR_WM_TRANS_A, r->cur_trans);
	wr(R_CUR_BUF_CFG_A, r->cur_buf);
	wr(R_CUR_BASE_A, rd(R_CUR_BASE_A));	/* the cursor's registers take with its base */
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
LOCAL ER commit_now( CONST IGREGS *r, CONST char *what );

/* PSR and the DC states off while the plane changes, on again after */
LOCAL ER commit( CONST IGREGS *r, CONST char *what )
{
	ER	er;

	knl_igfx_psr_hold(TRUE, what);
	er = commit_now(r, what);
	knl_igfx_psr_hold(FALSE, what);
	return er;
}

LOCAL ER commit_now( CONST IGREGS *r, CONST char *what )
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

/*
 * All eight levels, as the PRM's watermark program asks for them and as
 * worked out for this generation (9th):
 *
 * The latencies: the power controller's for levels 0-3 and 4-7 (two
 * requests); a level of 0 after level 0 turns it and those above off,
 * each level at least the one below; when level 0 reads 0 the memory's
 * read latency (2 us) is added to every level there is; and 1 us more,
 * as for memory of 16Gb chips, since what memory is fitted is not read.
 * IPC (the isochronous priority control) is not turned on without
 * knowing the memory is symmetric, so its 4 us is not added.
 *
 * A level for a plane w pixels wide of cpp bytes, linear, not rotated,
 * at the pipe's pixel rate (kHz): method 1 the bytes fetched in the
 * latency, latency * rate * cpp / 1000 in blocks of 512 bytes; method 2
 * the lines of the latency, rounded up, times the blocks of a line
 * (the line's bytes in blocks rounded up, and one more); the smaller of
 * the two when the latency is a line's time or more, else method 1.
 * Blocks: that rounded up and one more, at least a line's blocks above
 * level 0 and one block more there; lines: the result in lines rounded
 * up (none for level 0); each at least the level below; more than 31
 * lines is not a level. A level needs its blocks and one more of the
 * plane's share of the display buffer (DDB), or it is off. The
 * transition watermark stays off on this generation.
 *
 * The buffer: 892 blocks for the one pipe (896, 4 kept for the bypass
 * path); the cursor's share at its end, 32 blocks or what its levels
 * need for a cursor 256 pixels wide, and the rest the plane's.
 *
 * SAGV (the system agent's changes of voltage and clock, which stop the
 * display's reads for up to 30 us): left on only when the deepest level
 * the plane has on is for 30 us or more, asked of the power controller
 * before the change is written to be off, and after it to be on.
 */
#define DBUF_BLOCKS		892
#define DBUF_BLOCK_BYTES	512
#define WM_LEVELS		8
#define WM_MAX_LINES		31
#define CURSOR_W		256
#define CURSOR_MIN_ALLOC	32
#define SAGV_BLOCK_US		30
#define PCODE_SAGV_CONTROL	0x21
#define SAGV_DISABLE		0x0
#define SAGV_ENABLE		0x3
#define SAGV_IS_DISABLED	0x1
#define FX_ONE			( 1ULL << 16 )

typedef struct {
	BOOL	on;
	UW	blocks, lines, min_ddb;
} WMLEVEL;

LOCAL INT	ig_lat_state = 0;		/* 0 not read, 1 read, -1 could not */
LOCAL UH	ig_lat[WM_LEVELS];
LOCAL INT	ig_sagv = -1;			/* -1 as the firmware left it, 0 off, 1 on */
LOCAL UW	ig_underruns = 0;

LOCAL UD fx_div_up( UD a, UD d )		/* a / d as 16.16, rounded up */
{
	return ( ( a << 16 ) + d - 1 ) / d;
}

LOCAL UW fx_up( UD f )				/* 16.16 rounded up to an integer */
{
	return (UW)( ( f + FX_ONE - 1 ) >> 16 );
}

LOCAL BOOL latencies( void )
{
	UW	v0 = 0, v1 = 0;
	INT	l, i;

	if ( ig_lat_state != 0 ) return (BOOL)( ig_lat_state > 0 );
	if ( knl_igt_pcode_rw(PCODE_MEM_LATENCY, 0, &v0) < E_OK || knl_igt_pcode_rw(PCODE_MEM_LATENCY, 1, &v1) < E_OK ) {
		ig_lat_state = -1;
		say("igfx: the memory latencies not read from the power controller: the deeper watermarks not set\n");
		return FALSE;
	}
	for ( l = 0; l < 4; l++ ) {
		ig_lat[l] = (UH)( ( v0 >> ( 8 * l ) ) & 0xff );
		ig_lat[l + 4] = (UH)( ( v1 >> ( 8 * l ) ) & 0xff );
	}
	sayf("igfx: memory latencies %d %d %d %d %d %d %d %d us (power controller)\n", ig_lat[0], ig_lat[1], ig_lat[2],
	     ig_lat[3], ig_lat[4], ig_lat[5], ig_lat[6], ig_lat[7]);
	for ( l = 1; l < WM_LEVELS && ig_lat[l] != 0; l++ ) ;
	for ( i = l + 1; i < WM_LEVELS; i++ ) ig_lat[i] = 0;
	for ( l = 1; l < WM_LEVELS && ig_lat[l] != 0; l++ ) {
		if ( ig_lat[l] < ig_lat[l - 1] ) ig_lat[l] = ig_lat[l - 1];
	}
	if ( ig_lat[0] == 0 ) {
		ig_lat[0] += 2;
		for ( l = 1; l < WM_LEVELS && ig_lat[l] != 0; l++ ) ig_lat[l] += 2;
	}
	ig_lat[0] += 1;
	for ( l = 1; l < WM_LEVELS && ig_lat[l] != 0; l++ ) ig_lat[l] += 1;
	sayf("igfx: latencies taken %d %d %d %d %d %d %d %d us\n", ig_lat[0], ig_lat[1], ig_lat[2], ig_lat[3],
	     ig_lat[4], ig_lat[5], ig_lat[6], ig_lat[7]);
	ig_lat_state = 1;
	return TRUE;
}

/* The levels of a linear plane w pixels wide of cpp bytes, on a pipe of htotal at khz, for the latencies lat */
LOCAL void levels_calc( CONST UH *lat_of, UINT w, UINT cpp, UINT htotal, UD khz, WMLEVEL *lv )
{
	UD	pbpl, m1, m2, sel;
	UW	linetime_us, blocks, lines, prev_b = 0, prev_l = 0;
	INT	l;

	pbpl = (UD)( ( w * cpp + DBUF_BLOCK_BYTES - 1 ) / DBUF_BLOCK_BYTES + 1 ) << 16;
	linetime_us = (UW)( ( (UD)htotal * 1000 + khz - 1 ) / khz );
	for ( l = 0; l < WM_LEVELS; l++ ) {
		UW	lat = lat_of[l];

		lv[l].on = FALSE;
		lv[l].blocks = lv[l].lines = 0;
		lv[l].min_ddb = 0xffff;
		if ( lat == 0 ) continue;
		m1 = fx_div_up((UD)lat * khz * cpp, 1000ULL * DBUF_BLOCK_BYTES);
		m2 = ( ( (UD)lat * khz + (UD)htotal * 1000 - 1 ) / ( (UD)htotal * 1000 ) ) * pbpl;
		if ( (UD)cpp * htotal / DBUF_BLOCK_BYTES < 1 && (UD)w * cpp / DBUF_BLOCK_BYTES < 1 ) {
			sel = m2;
		} else if ( lat >= linetime_us ) {
			sel = ( m1 < m2 ) ? m1 : m2;
		} else {
			sel = m1;
		}
		blocks = fx_up(sel) + 1;
		if ( l > 0 && blocks < fx_up(pbpl) ) blocks = fx_up(pbpl);
		lines = (UW)( ( sel + pbpl - 1 ) / pbpl );	/* both 16.16: the quotient is the lines, rounded up */
		if ( l >= 1 ) blocks++;
		if ( blocks < prev_b ) blocks = prev_b;
		if ( lines < prev_l ) lines = prev_l;
		if ( l == 0 ) lines = 0;		/* level 0 has no lines */
		if ( lines > WM_MAX_LINES ) {
			prev_b = prev_l = 0;		/* not a level: the next is held to nothing below */
			continue;
		}
		prev_b = blocks;
		prev_l = lines;
		lv[l].on = TRUE;
		lv[l].blocks = blocks;
		lv[l].lines = lines;
		lv[l].min_ddb = blocks + 1;
	}
}

/* The levels of pipe A's plane w pixels wide */
LOCAL void levels_of( UINT w, UINT cpp, WMLEVEL *lv )
{
	levels_calc(ig_lat, w, cpp, ig_htotal, ig_pixel_khz, lv);
}

/* The register of a level, off when it does not fit the share */
LOCAL UW wm_reg( CONST WMLEVEL *w, UW share )
{
	if ( !w->on || w->min_ddb > share ) return 0;
	return 0x80000000U | ( w->lines << 14 ) | ( w->blocks & 0x7ff );
}

/*
 * The cursor's share: least blocks (32 with one pipe on, 8 with more, as
 * i915's skl_cursor_allocation), or what the deepest level it can have
 * needs
 */
LOCAL UW cursor_calc( CONST UH *lat_of, UINT htotal, UD khz, UW least )
{
	WMLEVEL	lv[WM_LEVELS];
	UW	need = 0;
	INT	l;

	levels_calc(lat_of, CURSOR_W, 4, htotal, khz, lv);
	for ( l = 0; l < WM_LEVELS && lv[l].on; l++ ) need = lv[l].min_ddb;
	return ( need > least ) ? need : least;
}

#define CURSOR_MIN_ALLOC_MANY	8	/* with more than one pipe on */

EXPORT BOOL knl_igfx_wm_calc( CONST UH *lat, UINT w, UINT htotal, UD khz, UW start, UW end, INT npipes, T_IGWM *o )
{
	WMLEVEL	lv[WM_LEVELS];
	INT	l, deepest = -1;

	if ( lat == NULL || o == NULL || khz == 0 || htotal == 0 || end <= start ) return FALSE;
	o->cur = cursor_calc(lat, htotal, khz, ( npipes <= 1 ) ? CURSOR_MIN_ALLOC : CURSOR_MIN_ALLOC_MANY);
	if ( o->cur >= end - start ) return FALSE;
	o->share = end - start - o->cur;
	levels_calc(lat, w, CPP, htotal, khz, lv);
	for ( l = 0; l < WM_LEVELS; l++ ) {
		o->wm[l] = wm_reg(&lv[l], o->share);
		if ( o->wm[l] != 0 ) deepest = l;
	}
	o->buf_cfg = ( ( start + o->share - 1 ) << 16 ) | start;
	o->cur_buf = ( ( end - 1 ) << 16 ) | ( start + o->share );
	o->sagv = (BOOL)( npipes <= 1 && deepest >= 0 && lat[deepest] >= SAGV_BLOCK_US );
	return TRUE;
}

/* Pipe A's part of the DDB, [0, ig_a_end), and the pipes on (knl_igfx_pipes) */
LOCAL UW	ig_a_end = DBUF_BLOCKS;
LOCAL INT	ig_npipes = 1;

/*
 * The watermarks and shares of the buffer into r for a plane w pixels
 * wide: the plane's eight levels, the cursor's (off: no cursor plane is
 * shown), both shares; FALSE when they cannot be worked out. *p_sagv
 * says whether SAGV may stay on.
 */
LOCAL BOOL wm_fill( IGREGS *r, UINT w, BOOL *p_sagv )
{
	WMLEVEL	lv[WM_LEVELS];
	T_IGWM	o;
	UW	cur, share;
	INT	l;

	if ( ig_pixel_khz == 0 || ig_htotal == 0 || !latencies() ) return FALSE;
	if ( !knl_igfx_wm_calc(ig_lat, w, ig_htotal, ig_pixel_khz, 0, ig_a_end, ig_npipes, &o) ) return FALSE;
	cur = o.cur;
	share = o.share;
	levels_of(w, CPP, lv);			/* for the record below */
	for ( l = 0; l < WM_LEVELS; l++ ) {
		r->wm[l] = o.wm[l];
		r->cur_wm[l] = 0;
	}
	r->wm_trans = 0;
	r->cur_trans = 0;
	r->buf_cfg = o.buf_cfg;
	r->cur_buf = o.cur_buf;
	*p_sagv = o.sagv;
	{
		char	t[400];
		INT	n;

		n = tm_sprintf((UB *)t, (CONST UB *)"igfx: watermarks for %d pixels at %ld kHz (htotal %d): plane share %d blocks, "
			       "cursor %d; levels (blocks/lines)", w, (D)ig_pixel_khz, ig_htotal, share, cur);
		for ( l = 0; l < WM_LEVELS; l++ ) {
			n += tm_sprintf((UB *)t + n, (CONST UB *)" %d:%s%d/%d", l, ( r->wm[l] != 0 ) ? "" : "off ",
					lv[l].blocks, lv[l].lines);
		}
		tm_sprintf((UB *)t + n, (CONST UB *)"; SAGV %s\n", *p_sagv ? "may stay on" : "to be off");
		say(t);
	}
	return TRUE;
}

/* SAGV asked on or off of the power controller, as needed */
LOCAL void sagv_set( BOOL on )
{
	UW	r = 0;
	INT	i;
	ER	er = E_OK;

	if ( ig_sagv == ( on ? 1 : 0 ) ) return;
	if ( on ) {
		er = knl_igt_pcode_rw(PCODE_SAGV_CONTROL, SAGV_ENABLE, &r);
	} else {
		/* asked again until it says off, for a millisecond at least */
		for ( i = 0; i < 50; i++ ) {
			er = knl_igt_pcode_rw(PCODE_SAGV_CONTROL, SAGV_DISABLE, &r);
			if ( er < E_OK || ( r & SAGV_IS_DISABLED ) != 0 ) break;
			WaitUsec(100);
		}
	}
	sayf("igfx: SAGV asked %s: %d (answer %08x)\n", on ? "on" : "off", (INT)er, r);
	if ( er >= E_OK ) ig_sagv = on ? 1 : 0;
}

/*
 * The watermarks of the plane as it is now, worked out and written
 * within a frame (at the start, and after a sleep); put back when the
 * plane did not take them
 */
LOCAL void wm_apply( CONST char *what )
{
	IGREGS	now, want;
	BOOL	sagv = FALSE;

	if ( !ig_ok || !ig_wm_on ) return;
	read_regs(&now);
	want = now;
	if ( !wm_fill(&want, ( now.size & 0x1fff ) + 1, &sagv) ) return;
	if ( !sagv ) sagv_set(FALSE);
	if ( commit(&want, what) < E_OK ) {
		(void)commit(&now, "put back");
		return;
	}
	if ( sagv ) sagv_set(TRUE);
}

EXPORT BOOL knl_igfx_latencies( UH *lat )
{
	if ( lat == NULL || !latencies() ) return FALSE;
	knl_memcpy(lat, ig_lat, sizeof(ig_lat));
	return TRUE;
}

EXPORT ER knl_igfx_pipes( INT npipes, UW a_end )
{
	IGREGS	now, want;
	BOOL	sagv = FALSE;
	UW	was_end = ig_a_end;
	INT	was_n = ig_npipes;

	if ( !ig_ok || !ig_wm_on ) return E_NOSPT;
	if ( npipes < 1 || a_end == 0 || a_end > DBUF_BLOCKS ) return E_PAR;
	if ( npipes > 1 ) sagv_set(FALSE);	/* off before a second pipe reads the memory */
	ig_npipes = npipes;
	ig_a_end = a_end;
	read_regs(&now);
	want = now;
	if ( !wm_fill(&want, ( now.size & 0x1fff ) + 1, &sagv) ) {
		ig_npipes = was_n;
		ig_a_end = was_end;
		return E_NOSPT;
	}
	sayf("igfx: pipe A's part of the display buffer 0 to %d, %d pipe(s) on\n", (INT)a_end, npipes);
	if ( commit(&want, ( npipes > 1 ) ? "pipe A's part for another pipe" : "pipe A's part whole again") < E_OK ) {
		(void)commit(&now, "put back");
		ig_npipes = was_n;
		ig_a_end = was_end;
		return E_IO;
	}
	if ( npipes == 1 && sagv ) sagv_set(TRUE);
	return E_OK;
}

EXPORT UINT knl_igfx_panel_w( void )
{
	return ig_nw;
}

EXPORT UD knl_igfx_gmadr( void )
{
	return ig_gmadr;
}

EXPORT void knl_igfx_wm_again( void )
{
	wm_apply("the watermarks after the sleep");
}

/*
 * For the records (cpuidle.c, idlelog=1): the watermarks as they read
 * now, the shares, SAGV as asked, and the pipe's FIFO underruns counted
 * since the start (the interrupt's latch, cleared as it is read)
 */
EXPORT INT knl_igfx_wm_text( char *t )
{
	INT	l, n;

	if ( !ig_found || ig_mmio == 0 ) return 0;
	knl_igfx_dc_pause(TRUE);		/* the plane's registers read with the power wells on */
	if ( rd(R_DE_PIPE_IIR_A) & PIPE_UNDERRUN ) {
		ig_underruns++;
		wr(R_DE_PIPE_IIR_A, PIPE_UNDERRUN);
	}
	n = tm_sprintf((UB *)t, (CONST UB *)"; display WM%s", ig_wm_on ? "" : " (igfx.wm=0)");
	for ( l = 0; l < WM_LEVELS; l++ ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %08x", rd(R_PLANE_WM_1A(l)));
	n += tm_sprintf((UB *)t + n, (CONST UB *)" trans %08x DDB %08x cursor DDB %08x, SAGV %s, underruns %d",
			rd(R_PLANE_WM_TRANS_1A), rd(R_PLANE_BUF_CFG_1A), rd(R_CUR_BUF_CFG_A),
			( ig_sagv < 0 ) ? "as left" : ig_sagv ? "on" : "off", (INT)ig_underruns);
	knl_igfx_dc_pause(FALSE);
	n += knl_igfx_psr_text(t + n);
	return n;
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
	if ( ig_wm_on ) {
		BOOL	sagv = FALSE;

		if ( wm_fill(r, w, &sagv) ) {
			if ( !sagv ) sagv_set(FALSE);
			goto plane;
		}
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
plane:
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
	if ( ig_wm_on && ig_lat_state > 0 ) {
		INT	l, deepest = -1;

		for ( l = 0; l < WM_LEVELS; l++ ) if ( want.wm[l] & 0x80000000U ) deepest = l;
		if ( deepest >= 0 && ig_lat[deepest] >= SAGV_BLOCK_US ) sagv_set(TRUE);
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

/*
 * The sizes offered: the usual ones up to the panel's own, and the one
 * now. How many there are, of which up to max are put in m.
 */
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
	for ( i = 0; i < (INT)( sizeof(usual) / sizeof(usual[0]) ); i++ ) {
		if ( usual[i][0] > ig_nw || usual[i][1] > ig_nh ) continue;
		if ( n < max ) {
			m[n][0] = usual[i][0];
			m[n][1] = usual[i][1];
		}
		if ( usual[i][0] == ig_nw && usual[i][1] == ig_nh ) panel = TRUE;
		if ( usual[i][0] == ig_w && usual[i][1] == ig_h ) cur = TRUE;
		n++;
	}
	if ( !panel ) {
		if ( n < max ) {
			m[n][0] = (UH)ig_nw;
			m[n][1] = (UH)ig_nh;
		}
		n++;
	}
	if ( !cur && !( ig_w == ig_nw && ig_h == ig_nh ) ) {
		if ( n < max ) {
			m[n][0] = (UH)ig_w;
			m[n][1] = (UH)ig_h;
		}
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
	if ( dark ) knl_igfx_psr_hold(TRUE, "the backlight off");
	v = rd(PP_CONTROL);
	wr(PP_CONTROL, dark ? ( v & ~PP_BL_ENABLE ) : ( v | PP_BL_ENABLE ));
	tm_printf((UB *)"igfx: the backlight %s: PP_CONTROL %08x -> %08x\n", dark ? "off" : "on", v, rd(PP_CONTROL));
	if ( !dark ) knl_igfx_psr_hold(FALSE, "the backlight on");
}

/* The pipe's pixel clock in kHz; 0 when it is not known */
EXPORT UD knl_igfx_pixel_khz( void )
{
	return ig_pixel_khz;
}

#endif /* X64 */
