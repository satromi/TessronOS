/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_igfx_psr.c (x64 PC)
 *	The panel's self refresh (PSR1) and the display's power states DC5
 *	and DC6 on Intel's integrated graphics of the 9th generation
 *	(Skylake, Kaby Lake, Amber Lake: design 6.10).
 *
 *	PSR. When the picture has not changed for some frames (the idle
 *	frames), the source stops sending it and the panel shows the last
 *	frame from its own memory; the main link stays up (link standby)
 *	or goes down as the VBT says. The display engine cannot see the
 *	processor's writes to the framebuffer, so the screen's driver says
 *	when it has written (knl_igfx_dirty, after each send of disp.c):
 *	PSR_CTL's enable is dropped, which makes the source leave PSR and
 *	send the frames again, and once PSR's state is idle the enable is
 *	set again, after which the hardware enters by itself when the idle
 *	frames have gone by. (A write to CURSURFLIVE alone, the hardware's
 *	tracking's exit, was seen on a Surface Go 2 not to make it leave.)
 *	A plane's new surface (a change of size)
 *	makes it leave as well, but PSR is turned off around every change
 *	of the plane all the same (knl_igfx_psr_hold).
 *
 *	PSR2 (selective update) is not used. On this generation it has no
 *	selective fetch and leans on the hardware's tracking alone.
 *
 *	DC5 and DC6. The display's microcontroller (DMC) runs a program
 *	Intel distributes (kbl_dmc_ver1_04.bin, SYSDEF_FW_DMC_KBL, loaded
 *	from its object on the system volume) that saves the display's
 *	state and turns its power wells off while the panel refreshes by
 *	itself, and brings them back when the display is written. DC5 also
 *	needs power well 2 off (pipes B and C, transcoders A to C, the
 *	external ports' AUX, audio, VGA): it is let go when only the eDP
 *	transcoder on pipe A is on and VGA is off, and asked for again
 *	whenever PSR is turned off (as it is while ext=1 looks at a port
 *	just plugged in, disp_ext.c). With no program, or one that is not whole, neither PSR nor
 *	the DC states are used, and the screen works as before.
 *
 *	The order. On: the program loaded (once, and again after a sleep
 *	that lost it), the panel's PSR configured over AUX (DPCD 170h),
 *	the source's PSR enabled, then the DC states allowed. Off: the DC
 *	states disallowed first (AUX and the plane's registers need the
 *	wells), power well 2 asked for, the source's PSR disabled and its
 *	state waited for to be idle (2 s at most), the panel's PSR off.
 *
 *	What turns it off for the rest of the start: an error the source
 *	reports in PSR (the AUX of its entry or exit), its state not idle
 *	within 2 s of being disabled, an exception (the screen taken back
 *	by the console, knl_igfx_psr_panic). While there is another pipe
 *	on (an external output), it stays off and comes back when the eDP
 *	pipe is alone again.
 *
 *	The start-up settings:
 *	  i915.enable_psr=1	PSR1 on (0 off, the default)
 *	  i915.enable_dc=N	0 no DC state, 1 up to DC5, 2 or -1 up to
 *				DC6 (-1, the default: DC6 where
 *				the backlight is not on the utility pin)
 *	  i915.psr_debug=1	PSR made to leave every second besides,
 *				to see whether a picture stands still
 *	  igfx.pwsync=1		the power wells' requests the firmware left
 *				in the BIOS's register taken over by the
 *				driver's (0, the default: only power well 2's)
 *
 *	For the records (idlelog=1): the entries and exits of PSR with why
 *	it left, the share of the time in PSR, and the DMC's counts of DC5
 *	and DC6.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include "sysdepend.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include <ts/pcie.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "../disp_hw.h"
#include "disp_pc.h"
#include <ts/board.h>

IMPORT ID	knl_obfile_first_vol( void );
IMPORT INT	knl_igfx_aux_ch( INT port );			/* disp_igfx_aux.c */
IMPORT INT	knl_igfx_dpcd_read( INT ch, UW addr, UB *buf, INT len );
IMPORT INT	knl_igfx_dpcd_write( INT ch, UW addr, CONST UB *buf, INT len );
IMPORT UBINT	knl_igfx_mmio( void );				/* disp_igfx.c */
IMPORT UD	knl_igfx_pixel_khz( void );
IMPORT void	(*knl_fbcon_wake)( void );			/* kernel/sysdepend/x64/fbcon.c */

/* ---------------------------------------------------------------- registers */

#define R_DC_STATE_EN		0x45504
#define   DC_UPTO_DC5		0x1U
#define   DC_UPTO_DC6		0x2U
#define   DC_MASK		0x3U
#define R_DC_STATE_DEBUG	0x45520
#define   DC_DEBUG_MEMORY_UP	0x2U
#define R_CHICKEN_DCPR_1	0x46430
#define   ALTERNATE_DC_EXIT	( 1U << 30 )
#define R_PWR_WELL_BIOS		0x45400
#define R_PWR_WELL_DRIVER	0x45404
#define R_PWR_WELL_KVMR		0x45408
#define R_PWR_WELL_DEBUG	0x4540c
#define   PW2_REQ		( 1U << 31 )
#define   PW2_STATE		( 1U << 30 )
#define R_FUSE_STATUS		0x42000
#define   FUSE_PG2_DIST		( 1U << 25 )
#define R_VGACNTRL		0x41000
#define   VGA_DISABLE		( 1U << 31 )
#define R_UTIL_PIN_CTL		0x48400
#define   UTIL_PIN_ENABLE	( 1U << 31 )
#define   UTIL_PIN_MODE_MASK	( 0xfU << 24 )
#define   UTIL_PIN_MODE_PWM	( 1U << 24 )

#define R_DMC_PROGRAM		0x80000
#define R_DMC_HTP		0x8f004
#define R_DMC_SSP_BASE		0x8f074
#define R_DMC_DC5_DC6		0x8002c
#define R_DMC_DC3_DC5		0x80030
#define DMC_MMIO_FIRST		0x80000U
#define DMC_MMIO_LAST		0x8ffffU
#define DMC_MAX_BYTES		0x3000
#define DMC_MAX_MMIO		8
#define DMC_FILE_MAX		( 64 * 1024 )

/* the eDP transcoder's PSR registers */
#define R_PSR_CTL		0x6f800
#define   PSR_ENABLE		( 1U << 31 )
#define   PSR_RESTORE_CTX	( 1U << 29 )	/* the hardware's own */
#define   PSR_LINK_STANDBY	( 1U << 27 )
#define   PSR_MAX_SLEEP(n)	( (UW)(n) << 20 )
#define   PSR_TP_TP1_TP3	( 1U << 11 )
#define   PSR_CRC_ENABLE	( 1U << 10 )
#define   PSR_TP23_TIME(n)	( (UW)(n) << 8 )
#define   PSR_TP1_TIME(n)	( (UW)(n) << 4 )
#define R_PSR_STATUS		0x6f840
#define   PSR_STATE(v)		( (UW)(v) >> 29 )
#define   PSR_ST_SRDENT		2
#define R_PSR_PERF_CNT		0x6f844
#define   PSR_PERF_MASK		0x00ffffffU
#define R_PSR_DEBUG		0x6f860
#define   DBG_MASK_HPD		( 1U << 31 )
#define   DBG_MASK_MAX_SLEEP	( 1U << 28 )
#define   DBG_MASK_LPSP		( 1U << 27 )
#define   DBG_MASK_MEMUP	( 1U << 26 )
#define   DBG_MASK_DISP_REG_WRITE ( 1U << 16 )
#define R_PSR_IMR		0x64834
#define R_PSR_IIR		0x64838
#define   PSR_IRQ_PRE_ENTRY	0x1U		/* the eDP transcoder's bits */
#define   PSR_IRQ_POST_EXIT	0x2U
#define   PSR_IRQ_ERROR		0x4U
#define   PSR_IRQ_ALL		0x7U
#define R_DE_MISC_IIR		0x44468
#define R_DE_MISC_IER		0x4446c
#define   DE_MISC_EDP_PSR	( 1U << 19 )

#define R_CURSURFLIVE_A		0x700ac
#define R_PIPE_FRMCOUNT_A	0x70040
#define R_DDI_BUF_CTL_A		0x64000
#define R_TRANS_DDI_EDP		0x6f400
#define R_PIPECONF_EDP		0x7f008
#define R_TRANSCONF(t)		( 0x70008 + 0x1000 * (t) )	/* transcoders A to C */
#define R_EDP_HTOTAL		0x6f000
#define R_EDP_VTOTAL		0x6f00c

/* the panel's DPCD */
#define DPCD_MAX_DOWNSPREAD	0x002
#define   DPCD_TPS3		0x40
#define DPCD_PSR_SUPPORT	0x070
#define DPCD_PSR_EN_CFG		0x170
#define   SINK_PSR_ENABLE	0x01
#define   SINK_MAIN_LINK_ACTIVE	0x02
#define   SINK_CRC_VERIFY	0x04
#define DPCD_SET_POWER		0x600
#define DPCD_SYNC_LATENCY	0x2009

/* the VBT */
#define VBT_OFF			0x400
#define BDB_PSR			9
#define BDB_DRIVER_FEATURES	12
#define BDB_LFP_OPTIONS		40
#define BDB_LFP_POWER		44

#define WAIT_IDLE_MS		2000
#define FILE_WAIT_MS		60000

/* why PSR left, for the records */
#define WHY_DIRTY		0
#define WHY_AUX			1
#define WHY_TIMEOUT		2
#define WHY_EXPLICIT		3
#define WHY_N			4

/* ---------------------------------------------------------------- state */

/*
 * The start-up's default of i915.enable_psr. 0: PSR and the DC states
 * are not used unless the setting asks for them. A build for trying it
 * on a machine whose settings are not rewritten may make this 1.
 */
LOCAL INT	psr_mode = 0;
LOCAL INT	dc_mode = -1;			/* i915.enable_dc */
LOCAL BOOL	psr_debug = FALSE;		/* i915.psr_debug=1 */
LOCAL BOOL	pw_sync = FALSE;		/* igfx.pwsync=1 */

LOCAL UBINT	mm = 0;
LOCAL ID	psr_mtx = 0;
LOCAL ID	psr_tsk = 0;
LOCAL INT	holds = 0;			/* explicit offs outstanding */
LOCAL volatile BOOL	active = FALSE;		/* the source's PSR enabled */
LOCAL volatile BOOL	broken = FALSE;		/* not again in this start */
LOCAL volatile BOOL	drawn = FALSE;		/* the screen's driver has sent once */
LOCAL volatile BOOL	err_seen = FALSE;	/* the source reported an error: the task turns it off */
LOCAL volatile BOOL	other_pipe = FALSE;	/* another pipe found on */
LOCAL BOOL	dmc_ok = FALSE;
LOCAL UW	dc_now = 0;			/* what DC_STATE_EN was set to allow */
LOCAL BOOL	pw2_let_go = FALSE;
LOCAL BOOL	ext_on = FALSE;			/* ext=1: the external outputs are looked for */
LOCAL BOOL	irq_count = FALSE;		/* the entries counted by the latch (else by the time) */
LOCAL CONST char *why_off = "not started";

/* the program */
LOCAL UW	*dmc_pay = NULL;
LOCAL INT	dmc_dw = 0;
LOCAL UW	dmc_ma[DMC_MAX_MMIO], dmc_md[DMC_MAX_MMIO];
LOCAL INT	dmc_mn = 0;
LOCAL UW	dmc_ver = 0;

/* the panel, from the VBT and the DPCD */
LOCAL BOOL	vbt_full_link = TRUE;
LOCAL INT	vbt_idle = 0, vbt_tp1_us = 2500, vbt_tp23_us = 2500;
LOCAL INT	vbt_psr_en = -1;		/* the VBT's own word; -1 not said */
LOCAL INT	idle_frames = 6;
LOCAL UD	frame_ns = 16666667ULL;

/* the counts */
LOCAL UD	t_last = 0;			/* the last dirty or enable, ns */
LOCAL UD	n_entry = 0, n_exit[WHY_N], in_ns = 0, n_dirty = 0;
LOCAL UD	n_latch = 0;			/* entries the latch (EDP_PSR_IIR) said */
LOCAL UD	st_at_send[8];			/* the state PSR_STATUS read before each send's exit */
LOCAL UD	n_left = 0, n_stayed = 0;	/* after an exit: idle within 50 ms, or not */
LOCAL UW	stayed_st = 0;
LOCAL UD	n_reenable = 0;
LOCAL volatile BOOL	sw_exit = FALSE;	/* PSR_CTL's enable dropped by a send, not yet set again */
LOCAL volatile UW	dirty_seq = 0;
LOCAL UW	psr_ctl_val = 0;		/* what PSR_CTL is set to when on */
LOCAL UD	pipes_at = 0;
LOCAL UD	txt_at = 0, txt_in = 0;
LOCAL UW	txt_dc5 = 0, txt_dc6 = 0, txt_perf = 0;
LOCAL BOOL	txt_first = TRUE;
LOCAL BOOL	frame_seen = FALSE;
LOCAL UW	dc_paused = 0;

/* who sent: the calling tasks, the four most seen, and the rest */
#define SENDERS			4
LOCAL ID	snd_tid[SENDERS];
LOCAL UD	snd_n[SENDERS], snd_rest = 0;
LOCAL T_DISPSTAT	txt_ds;

LOCAL UW rd( UW off )
{
	return *(volatile UW *)( mm + off );
}

LOCAL void wr( UW off, UW v )
{
	*(volatile UW *)( mm + off ) = v;
}

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UD now_ns( void )
{
	return knl_get_mono_ns();
}

/* ---------------------------------------------------------------- the DMC's program */

/*
 * The file checked before any of it is written: the CSS header (its length 128
 * bytes), the package header (version 1 with 20 entries, or 2 with 32;
 * its length in words that size), the entry for any stepping ('*' '*'),
 * the DMC header of version 1 (128 bytes: at most 8 register writes,
 * each in the DMC's range 80000h-8FFFFh), and the program's words (at
 * most 3000h bytes), all inside the file
 */
LOCAL BOOL dmc_parse( CONST UB *b, INT n )
{
	CONST UB	*p, *e, *h;
	INT		hver, max, psize, num, i, rem;
	UW		off, fsize;

	if ( n < 128 || le32(b + 4) * 4 != 128 ) {
		tm_printf((UB *)"igfx: psr: the DMC's CSS header is not 128 bytes\n");
		return FALSE;
	}
	dmc_ver = le32(b + 0x58);
	p = b + 128;
	rem = n - 128;
	if ( rem < 16 ) return FALSE;
	hver = p[1];
	max = ( hver == 1 ) ? 20 : ( hver == 2 ) ? 32 : 0;
	psize = 16 + max * 12;
	if ( max == 0 || rem < psize || p[0] * 4 != psize ) {
		tm_printf((UB *)"igfx: psr: the DMC's package header (version %d, %d words) not taken\n", hver, p[0]);
		return FALSE;
	}
	num = (INT)le32(p + 12);
	if ( num > max ) num = max;
	e = NULL;
	for ( i = 0; i < num; i++ ) {
		CONST UB	*f = p + 16 + 12 * i;

		if ( f[2] == '*' && f[3] == '*' ) {
			e = f;
			break;
		}
	}
	if ( e == NULL ) {
		tm_printf((UB *)"igfx: psr: the DMC's package has no program for any stepping (%d entries)\n", num);
		return FALSE;
	}
	off = le32(e + 4) * 4;
	if ( off > (UW)( rem - psize ) || rem - psize - (INT)off < 128 ) return FALSE;
	h = p + psize + off;
	rem = n - (INT)( h - b );
	if ( h[5] != 1 || h[4] != 128 ) {
		tm_printf((UB *)"igfx: psr: the DMC header of version %d, %d bytes, not taken\n", h[5], h[4]);
		return FALSE;
	}
	dmc_mn = (INT)le32(h + 20);
	if ( dmc_mn > DMC_MAX_MMIO ) {
		tm_printf((UB *)"igfx: psr: the DMC header has %d register writes\n", dmc_mn);
		return FALSE;
	}
	for ( i = 0; i < dmc_mn; i++ ) {
		dmc_ma[i] = le32(h + 24 + 4 * i);
		dmc_md[i] = le32(h + 56 + 4 * i);
		if ( dmc_ma[i] < DMC_MMIO_FIRST || dmc_ma[i] > DMC_MMIO_LAST ) {
			tm_printf((UB *)"igfx: psr: the DMC header writes %x, outside the DMC's range\n", dmc_ma[i]);
			return FALSE;
		}
	}
	fsize = le32(h + 12) * 4;
	if ( (INT)fsize > rem - 128 || fsize > DMC_MAX_BYTES || fsize == 0 ) {
		tm_printf((UB *)"igfx: psr: the DMC program of %d bytes not taken\n", (INT)fsize);
		return FALSE;
	}
	dmc_pay = (UW *)Kmalloc(fsize);
	if ( dmc_pay == NULL ) return FALSE;
	for ( i = 0; i < (INT)( fsize / 4 ); i++ ) dmc_pay[i] = le32(h + 128 + 4 * i);
	dmc_dw = (INT)( fsize / 4 );
	return TRUE;
}

/* The file from its object (record 1), once the system volume is there; FALSE when there is none */
LOCAL BOOL dmc_read( void )
{
	TS_UUID	u;
	UB	*b;
	SZ	asz = 0;
	ID	key = 0;
	INT	waited = 0;
	BOOL	ok;

	if ( ts_str_to_uuid(SYSDEF_FW_DMC_KBL, &u) < E_OK ) return FALSE;
	for ( ;; ) {
		key = ( knl_obfile_first_vol() > 0 ) ? ob_opn_obj(&u, OB_OP_R) : 0;
		if ( key > 0 || waited >= FILE_WAIT_MS ) break;
		tk_dly_tsk(500);
		waited += 500;
	}
	if ( key <= 0 ) {
		tm_printf((UB *)"igfx: psr: no object %s (kbl_dmc_ver1_04.bin): PSR and the DC states not used\n",
			  SYSDEF_FW_DMC_KBL);
		return FALSE;
	}
	b = (UB *)Kmalloc(DMC_FILE_MAX);
	ok = ( b != NULL && ob_rea_rec(key, 1, 0, b, DMC_FILE_MAX, &asz) >= E_OK );
	(void)ob_cls_obj(key);
	if ( ok ) ok = dmc_parse(b, (INT)asz);
	if ( b != NULL ) Kfree(b);
	if ( !ok ) {
		tm_printf((UB *)"igfx: psr: kbl_dmc_ver1_04.bin (%d bytes) not taken: PSR and the DC states not used\n",
			  (INT)asz);
	}
	return ok;
}

LOCAL BOOL dmc_there( void )
{
	return (BOOL)( rd(R_DMC_PROGRAM) != 0 && rd(R_DMC_SSP_BASE) != 0 && rd(R_DMC_HTP) != 0 );
}

/* The program written to the DMC, its register writes after it, the memory's wake masked; DC states off before */
LOCAL BOOL dmc_load( CONST char *when )
{
	INT	i;

	for ( i = 0; i < dmc_dw; i++ ) wr(R_DMC_PROGRAM + 4 * (UW)i, dmc_pay[i]);
	for ( i = 0; i < dmc_mn; i++ ) wr(dmc_ma[i], dmc_md[i]);
	wr(R_DC_STATE_DEBUG, rd(R_DC_STATE_DEBUG) | DC_DEBUG_MEMORY_UP);
	if ( !dmc_there() ) {
		tm_printf((UB *)"igfx: psr: the DMC program %s did not stay (SSP %08x HTP %08x)\n", when,
			  rd(R_DMC_SSP_BASE), rd(R_DMC_HTP));
		return FALSE;
	}
	tm_printf((UB *)"igfx: psr: the DMC program %d.%02d loaded %s (%d words, %d register writes)\n",
		  (INT)( dmc_ver >> 16 ), (INT)( dmc_ver & 0xffff ), when, dmc_dw, dmc_mn);
	return TRUE;
}

/* ---------------------------------------------------------------- the DC states */

/*
 * DC_STATE_EN written until it reads back as written six times in a
 * row, or 100 writes: the DMC has been seen not to take a write that
 * turns DC6 off at once
 */
LOCAL void dc_write( UW state )
{
	INT	rewrites = 0, rereads = 0;
	UW	v;

	wr(R_DC_STATE_EN, state);
	do {
		v = rd(R_DC_STATE_EN);
		if ( v != state ) {
			wr(R_DC_STATE_EN, state);
			rewrites++;
			rereads = 0;
		} else if ( rereads++ > 5 ) {
			break;
		}
	} while ( rewrites < 100 );
	if ( v != state ) {
		tm_printf((UB *)"igfx: psr: DC_STATE_EN %08x not taken after %d writes (reads %08x)\n", state, rewrites, v);
	}
}

LOCAL void dc_set( UW want )
{
	UW	v = rd(R_DC_STATE_EN);

	if ( ( v & DC_MASK ) != dc_now ) {
		tm_printf((UB *)"igfx: psr: DC_STATE_EN %08x, not as set (%x)\n", v, dc_now);
	}
	dc_write(( v & ~DC_MASK ) | want);
	dc_now = want;
}

/*
 * igfx.pwsync=1: each power well's request the firmware left in the
 * BIOS's register (PWR_WELL_CTL1) moved to the driver's (CTL2): MISC IO,
 * the DDIs' IO (A/E to D), power well 1 and 2. What the DMC turns off in
 * DC6 (power well 1, MISC IO) is then asked for by the driver alone.
 */
LOCAL void pw_take_over( void )
{
	static CONST UB	idx[7] = { 0, 1, 2, 3, 4, 14, 15 };
	UW		bios = rd(R_PWR_WELL_BIOS), drv = rd(R_PWR_WELL_DRIVER), b0 = bios, d0 = drv;
	INT		i;

	for ( i = 0; i < 7; i++ ) {
		UW	m = 1U << ( 2 * idx[i] + 1 );

		if ( bios & m ) {
			if ( ( drv & m ) == 0 ) {
				drv |= m;
				wr(R_PWR_WELL_DRIVER, drv);
			}
			bios &= ~m;
			wr(R_PWR_WELL_BIOS, bios);
		}
	}
	tm_printf((UB *)"igfx: psr: igfx.pwsync=1: the firmware's requests taken over: PWR_WELL_CTL1 %08x -> %08x, "
		  "CTL2 %08x -> %08x\n", b0, rd(R_PWR_WELL_BIOS), d0, rd(R_PWR_WELL_DRIVER));
}

/* Power well 2 asked for again (the driver's request), and waited for */
LOCAL void pw2_get( void )
{
	INT	ms;

	if ( !pw2_let_go ) return;
	wr(R_PWR_WELL_DRIVER, rd(R_PWR_WELL_DRIVER) | PW2_REQ);
	for ( ms = 0; ms < 20 && ( rd(R_PWR_WELL_DRIVER) & PW2_STATE ) == 0; ms++ ) tk_dly_tsk(1);
	for ( ; ms < 20 && ( rd(R_FUSE_STATUS) & FUSE_PG2_DIST ) == 0; ms++ ) tk_dly_tsk(1);
	if ( ( rd(R_VGACNTRL) & VGA_DISABLE ) == 0 ) wr(R_VGACNTRL, VGA_DISABLE);
	pw2_let_go = FALSE;
	if ( ms >= 20 ) {
		tm_printf((UB *)"igfx: psr: power well 2 not on again (PWR_WELL_CTL2 %08x, FUSE_STATUS %08x)\n",
			  rd(R_PWR_WELL_DRIVER), rd(R_FUSE_STATUS));
	}
}

/*
 * Power well 2 let go where nothing on it is used: only the eDP
 * transcoder on, VGA off. The
 * firmware's request taken over first, as the driver's. TRUE when it
 * went off.
 */
LOCAL BOOL pw2_release( void )
{
	UW	bios, drv;
	INT	t;

	for ( t = 0; t < 3; t++ ) {
		if ( rd(R_TRANSCONF(t)) & PSR_ENABLE ) return FALSE;
	}
	if ( ( rd(R_VGACNTRL) & VGA_DISABLE ) == 0 ) return FALSE;
	bios = rd(R_PWR_WELL_BIOS);
	if ( bios & PW2_REQ ) {
		drv = rd(R_PWR_WELL_DRIVER);
		if ( ( drv & PW2_REQ ) == 0 ) wr(R_PWR_WELL_DRIVER, drv | PW2_REQ);
		wr(R_PWR_WELL_BIOS, bios & ~PW2_REQ);
	}
	wr(R_PWR_WELL_DRIVER, rd(R_PWR_WELL_DRIVER) & ~PW2_REQ);
	pw2_let_go = TRUE;
	WaitUsec(1000);
	if ( rd(R_PWR_WELL_DRIVER) & PW2_STATE ) {
		tm_printf((UB *)"igfx: psr: power well 2 stays on (KVMR %08x, debug %08x ask for it): no DC5\n",
			  rd(R_PWR_WELL_KVMR), rd(R_PWR_WELL_DEBUG));
		return FALSE;
	}
	return TRUE;
}

/* The DC states allowed, up to DC6 or DC5 as set, once PSR is on */
LOCAL void dc_on( void )
{
	UW	u, want;
	BOOL	pw2;

	if ( dc_mode == 0 || !dmc_ok || !active ) return;
	pw2 = pw2_release();
	u = rd(R_UTIL_PIN_CTL);
	want = ( dc_mode == 1 || ( ( u & UTIL_PIN_ENABLE ) && ( u & UTIL_PIN_MODE_MASK ) == UTIL_PIN_MODE_PWM ) )
		? DC_UPTO_DC5 : DC_UPTO_DC6;
	wr(R_CHICKEN_DCPR_1, rd(R_CHICKEN_DCPR_1) | ALTERNATE_DC_EXIT);	/* Display WA #1183 */
	dc_set(want);
	tm_printf((UB *)"igfx: psr: DC states up to DC%d allowed, power well 2 %s (PWR_WELL_CTL1 %08x CTL2 %08x CTL3 %08x "
		  "CTL4 %08x)\n", ( want == DC_UPTO_DC6 ) ? 6 : 5, pw2 ? "off" : "on", rd(R_PWR_WELL_BIOS),
		  rd(R_PWR_WELL_DRIVER), rd(R_PWR_WELL_KVMR), rd(R_PWR_WELL_DEBUG));
}

LOCAL void dc_off( void )
{
	if ( dc_now != 0 ) dc_set(0);
	pw2_get();
}

/* ---------------------------------------------------------------- PSR on and off */

/* The VBT's PSR words for the panel: the link kept up or not, the idle frames, the training times */
LOCAL void vbt_read( void )
{
	UW			asls = ts_pcie_cfg_read(0, 2, 0, 0xfc, 4);
	CONST volatile UB	*v, *bdb, *blk, *end;
	CONST volatile UB	*psr = NULL, *drv = NULL, *pwr = NULL;
	UH			psr_sz = 0, drv_sz = 0, pwr_sz = 0, hsz, bsz, ver;
	UW			bdb_off;
	INT			pt = -1;

	if ( asls == 0 || asls == 0xffffffffU ) return;
	v = (CONST volatile UB *)( DEV_VA_BASE + (UD)asls + VBT_OFF );
	if ( v[0] != '$' || v[1] != 'V' || v[2] != 'B' || v[3] != 'T' ) return;
	bdb_off = (UW)v[0x1c] | ( (UW)v[0x1d] << 8 ) | ( (UW)v[0x1e] << 16 ) | ( (UW)v[0x1f] << 24 );
	if ( bdb_off < 0x20 || bdb_off > 0x1000 ) return;
	bdb = v + bdb_off;
	ver = (UH)( bdb[0x10] | ( bdb[0x11] << 8 ) );
	hsz = (UH)( bdb[0x12] | ( bdb[0x13] << 8 ) );
	bsz = (UH)( bdb[0x14] | ( bdb[0x15] << 8 ) );
	blk = bdb + hsz;
	end = bdb + bsz;
	while ( blk + 3 <= end ) {
		UB	id = blk[0];
		UH	sz = (UH)( blk[1] | ( blk[2] << 8 ) );

		if ( sz == 0 || blk + 3 + sz > end ) break;
		if ( id == BDB_PSR ) { psr = blk + 3; psr_sz = sz; }
		if ( id == BDB_DRIVER_FEATURES ) { drv = blk + 3; drv_sz = sz; }
		if ( id == BDB_LFP_POWER ) { pwr = blk + 3; pwr_sz = sz; }
		if ( id == BDB_LFP_OPTIONS && sz >= 1 ) pt = blk[3];
		blk += 3 + sz;
	}
	if ( pt < 0 || pt > 15 ) {
		tm_printf((UB *)"igfx: psr: the VBT's panel type %d: the first panel's entries used\n", pt);
		pt = 0;
	}
	if ( psr != NULL && psr_sz >= 6 * ( pt + 1 ) ) {
		CONST volatile UB	*e = psr + 6 * pt;
		INT			tp1 = e[2] | ( e[3] << 8 ), tp2 = e[4] | ( e[5] << 8 );
		static CONST INT	code_us[4] = { 500, 100, 2500, 0 };

		vbt_full_link = (BOOL)( e[0] & 1 );
		vbt_idle = e[1] & 0xf;
		if ( ver >= 205 ) {
			vbt_tp1_us = code_us[( tp1 <= 3 ) ? tp1 : 2];
			vbt_tp23_us = code_us[( tp2 <= 3 ) ? tp2 : 2];
		} else {
			vbt_tp1_us = tp1 * 100;
			vbt_tp23_us = tp2 * 100;
		}
	}
	if ( ver < 228 && drv != NULL && drv_sz >= 19 ) {
		vbt_psr_en = ( ( drv[17] | ( drv[18] << 8 ) ) >> 9 ) & 1;
	} else if ( ver >= 228 && pwr != NULL && pwr_sz >= 26 ) {
		vbt_psr_en = ( ( pwr[24] | ( pwr[25] << 8 ) ) >> pt ) & 1;
	}
	tm_printf((UB *)"igfx: psr: VBT %d, panel type %d: PSR %s, %s, idle frames %d, TP1 %d us, TP2/TP3 %d us\n",
		  ver, pt, ( vbt_psr_en < 0 ) ? "not said" : vbt_psr_en ? "on" : "off",
		  vbt_full_link ? "the link kept up" : "the link down", vbt_idle, vbt_tp1_us, vbt_tp23_us);
}

LOCAL UW tp_code( INT us )
{
	return ( us == 0 ) ? 3 : ( us <= 100 ) ? 1 : ( us <= 500 ) ? 0 : 2;
}

/* How many pipes are on besides the eDP transcoder's: transcoders A to C */
LOCAL INT pipes_other( void )
{
	INT	t, n = 0;

	for ( t = 0; t < 3; t++ ) {
		if ( rd(R_TRANSCONF(t)) & PSR_ENABLE ) n++;
	}
	return n;
}

/* Whether the panel is as PSR here needs: the eDP transcoder on pipe A over DDI A, no other pipe */
LOCAL CONST char *cannot( void )
{
	UW	ddi = rd(R_TRANS_DDI_EDP), sel = ( ddi >> 12 ) & 7;

	if ( ( ddi & PSR_ENABLE ) == 0 || ( rd(R_PIPECONF_EDP) & PSR_ENABLE ) == 0 ) return "the eDP transcoder is off";
	if ( sel != 0 && sel != 4 ) return "the eDP transcoder is not on pipe A";
	if ( ( rd(R_DDI_BUF_CTL_A) & PSR_ENABLE ) == 0 ) return "DDI A is off";
	if ( pipes_other() > 0 ) return "another pipe is on";
	return NULL;
}

/*
 * The frame's time: pipe A's frame counter over 200 ms, once (the
 * eDP transcoder's totals and the pixel clock worked out from the link
 * told beside it)
 */
LOCAL void frame_time( void )
{
	UD	khz = knl_igfx_pixel_khz(), calc = 0, t0, t1;
	UD	ht = ( ( rd(R_EDP_HTOTAL) >> 16 ) & 0x1fff ) + 1, vt = ( ( rd(R_EDP_VTOTAL) >> 16 ) & 0x1fff ) + 1;
	UW	f0, f1;

	if ( frame_seen ) return;
	if ( khz != 0 ) calc = ht * vt * 1000000ULL / khz;
	f0 = rd(R_PIPE_FRMCOUNT_A);
	t0 = now_ns();
	tk_dly_tsk(200);
	f1 = rd(R_PIPE_FRMCOUNT_A);
	t1 = now_ns();
	if ( f1 != f0 ) {
		frame_ns = ( t1 - t0 ) / (UD)( f1 - f0 );
	} else if ( calc != 0 ) {
		frame_ns = calc;
	}
	frame_seen = TRUE;
	tm_printf((UB *)"igfx: psr: the frame %d us by pipe A's frame counter (%d frames in %d ms); "
		  "%d us from the totals %d x %d and the pixel clock %d kHz\n", (INT)( frame_ns / 1000 ), (INT)( f1 - f0 ),
		  (INT)( ( t1 - t0 ) / 1000000 ), (INT)( calc / 1000 ), (INT)ht, (INT)vt, (INT)khz);
}

LOCAL void psr_on( void )
{
	CONST char	*why;
	INT		ch = knl_igfx_aux_ch(0), sync = 8;
	UB		b = 0, sink;
	UW		ctl, mask, imr;
	BOOL		tps3 = FALSE;

	if ( broken || holds > 0 || active || !dmc_ok || !drawn ) return;
	if ( vbt_psr_en == 0 ) {
		tm_printf((UB *)"igfx: psr: not on: the VBT says the panel has no PSR\n");
		why_off = "VBT: no PSR";
		broken = TRUE;
		return;
	}
	why = cannot();
	if ( why != NULL ) {
		if ( why_off != why ) tm_printf((UB *)"igfx: psr: not on: %s\n", why);
		why_off = why;
		return;
	}
	if ( knl_igfx_dpcd_read(ch, DPCD_PSR_SUPPORT, &b, 1) != 1 || b == 0 ) {
		tm_printf((UB *)"igfx: psr: the panel has no PSR (DPCD 070h %02x)\n", b);
		broken = TRUE;
		why_off = "the panel has no PSR";
		return;
	}
	if ( knl_igfx_dpcd_read(ch, DPCD_SYNC_LATENCY, &b, 1) == 1 ) sync = b & 0xf;
	if ( knl_igfx_dpcd_read(ch, DPCD_MAX_DOWNSPREAD, &b, 1) == 1 ) tps3 = (BOOL)( ( b & DPCD_TPS3 ) != 0 );
	idle_frames = ( vbt_idle > 6 ) ? vbt_idle : 6;
	if ( sync + 1 > idle_frames ) idle_frames = sync + 1;
	if ( idle_frames > 15 ) idle_frames = 15;
	frame_time();

	/* the panel: configured, then enabled */
	sink = SINK_CRC_VERIFY | ( vbt_full_link ? SINK_MAIN_LINK_ACTIVE : 0 );
	if ( knl_igfx_dpcd_write(ch, DPCD_PSR_EN_CFG, &sink, 1) != 1 ) {
		tm_printf((UB *)"igfx: psr: DPCD 170h not written\n");
		return;
	}
	sink |= SINK_PSR_ENABLE;
	(void)knl_igfx_dpcd_write(ch, DPCD_PSR_EN_CFG, &sink, 1);
	b = 1;
	(void)knl_igfx_dpcd_write(ch, DPCD_SET_POWER, &b, 1);

	/* the source: what is not to make it leave, the latch of its errors, then enabled */
	mask = DBG_MASK_HPD | DBG_MASK_MEMUP | DBG_MASK_LPSP | DBG_MASK_MAX_SLEEP | DBG_MASK_DISP_REG_WRITE;
	wr(R_PSR_DEBUG, mask);
	irq_count = (BOOL)( ( rd(R_DE_MISC_IER) & DE_MISC_EDP_PSR ) == 0 );	/* latched only, never an interrupt */
	wr(R_PSR_IIR, PSR_IRQ_ALL);
	imr = rd(R_PSR_IMR) | PSR_IRQ_ALL;
	wr(R_PSR_IMR, imr & ~( PSR_IRQ_ERROR | ( irq_count ? ( PSR_IRQ_PRE_ENTRY | PSR_IRQ_POST_EXIT ) : 0 ) ));
	ctl = PSR_ENABLE | (UW)idle_frames | PSR_MAX_SLEEP(0x1f) | ( vbt_full_link ? PSR_LINK_STANDBY : 0 )
		| PSR_TP1_TIME(tp_code(vbt_tp1_us)) | PSR_TP23_TIME(tp_code(vbt_tp23_us)) | PSR_CRC_ENABLE
		| ( tps3 ? PSR_TP_TP1_TP3 : 0 );
	if ( rd(R_PSR_CTL) & PSR_ENABLE ) tm_printf((UB *)"igfx: psr: PSR_CTL %08x already enabled\n", rd(R_PSR_CTL));
	wr(R_PSR_CTL, ( rd(R_PSR_CTL) & PSR_RESTORE_CTX ) | ctl);
	psr_ctl_val = ctl;
	sw_exit = FALSE;
	t_last = now_ns();
	active = TRUE;
	why_off = NULL;
	tm_printf((UB *)"igfx: psr: on: PSR_CTL %08x, idle frames %d (frame %d us), DPCD 170h %02x, sync latency %d, "
		  "TPS3 %s, entries counted %s\n", rd(R_PSR_CTL), idle_frames, (INT)( frame_ns / 1000 ), sink, sync,
		  tps3 ? "yes" : "no", irq_count ? "by the latch" : "by the time");
	dc_on();
}

/* Off, the DC states first: 'why' what made it go off, for the records; the panel's PSR off too */
LOCAL void psr_off( INT why, CONST char *what )
{
	INT	ms;
	UB	z = 0;
	UW	st = 0;

	dc_off();
	if ( !active ) return;
	wr(R_PSR_CTL, rd(R_PSR_CTL) & ~PSR_ENABLE);
	active = FALSE;
	sw_exit = FALSE;
	for ( ms = 0; ms < WAIT_IDLE_MS; ms++ ) {
		st = rd(R_PSR_STATUS);
		if ( PSR_STATE(st) == 0 ) break;
		tk_dly_tsk(1);
	}
	wr(R_PSR_IMR, rd(R_PSR_IMR) | PSR_IRQ_ALL);
	(void)knl_igfx_dpcd_write(knl_igfx_aux_ch(0), DPCD_PSR_EN_CFG, &z, 1);
	if ( ms >= WAIT_IDLE_MS ) {
		why = WHY_TIMEOUT;
		broken = TRUE;
		tm_printf((UB *)"igfx: psr: PSR_STATUS %08x not idle %d ms after PSR was turned off: not again\n", st, ms);
	}
	n_exit[why]++;
	why_off = what;
	tm_printf((UB *)"igfx: psr: off: %s\n", what);
}

/* ---------------------------------------------------------------- the task */

/*
 * The program read and loaded once the system volume is there; PSR on
 * once the screen's driver has drawn; then what the sends found (an
 * error, another pipe, a pipe alone again) dealt with, and with
 * i915.psr_debug=1 PSR made to leave every second
 */
LOCAL void psr_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	vbt_read();
	if ( !dmc_read() ) {
		why_off = "no DMC program";
		tk_exd_tsk();
	}
	tk_loc_mtx(psr_mtx, TMO_FEVR);
	dc_set(0);
	if ( pw_sync ) pw_take_over();
	dmc_ok = dmc_load("at the start");
	tk_unl_mtx(psr_mtx);
	if ( !dmc_ok ) {
		why_off = "the DMC program did not stay";
		tk_exd_tsk();
	}
	tk_loc_mtx(psr_mtx, TMO_FEVR);
	psr_on();
	tk_unl_mtx(psr_mtx);
	for ( ;; ) {
		ER	er = tk_slp_tsk(psr_debug ? 1000 : TMO_FEVR);

		tk_loc_mtx(psr_mtx, TMO_FEVR);
		if ( err_seen ) {
			err_seen = FALSE;
			if ( active ) {
				psr_off(WHY_AUX, "the source reported an error in PSR");
				broken = TRUE;
			}
		}
		if ( other_pipe && active ) {
			psr_off(WHY_EXPLICIT, "another pipe is on");
		}
		if ( !active ) psr_on();
		tk_unl_mtx(psr_mtx);
		/*
		 * After a send's exit: PSR's state waited for to be idle (50 ms
		 * at most), then PSR enabled again, when no other send came
		 * meanwhile; the hardware enters once the idle frames have gone
		 * by. Not idle in time: left off until the next send.
		 */
		tk_loc_mtx(psr_mtx, TMO_FEVR);
		while ( sw_exit && active && holds == 0 ) {
			UW	seq = dirty_seq, st = 0;
			INT	ms;

			for ( ms = 0; ms < 50; ms++ ) {
				st = rd(R_PSR_STATUS);
				if ( PSR_STATE(st) == 0 ) break;
				tk_dly_tsk(1);
			}
			if ( ms >= 50 ) {
				n_stayed++;
				stayed_st = st;
				break;
			}
			n_left++;
			sw_exit = FALSE;
			if ( seq != dirty_seq ) {
				sw_exit = TRUE;
				continue;
			}
			wr(R_PSR_CTL, ( rd(R_PSR_CTL) & PSR_RESTORE_CTX ) | psr_ctl_val);
			t_last = now_ns();
			n_reenable++;
		}
		tk_unl_mtx(psr_mtx);
		if ( er == E_TMOUT && psr_debug && active ) knl_igfx_dirty();
	}
}

/* ---------------------------------------------------------------- interface */

/* The console draws on the screen itself (an exception): PSR and the DC states off at once, for good */
EXPORT void knl_igfx_psr_panic( void )
{
	UW	v;
	INT	i;

	if ( mm == 0 || ( !active && dc_now == 0 ) ) return;
	broken = TRUE;
	v = rd(R_DC_STATE_EN) & ~DC_MASK;
	for ( i = 0; i < 100 && rd(R_DC_STATE_EN) != v; i++ ) wr(R_DC_STATE_EN, v);
	dc_now = 0;
	wr(R_PSR_CTL, rd(R_PSR_CTL) & ~PSR_ENABLE);
	active = FALSE;
}

/*
 * After each send to the framebuffer (disp.c, through T_DISPHW dirty):
 * the hardware's tracking does not see the processor's writes to the
 * memory scanned out, so PSR is left by dropping PSR_CTL's enable (and
 * CURSURFLIVE written besides), and the task enables it again once its
 * state is idle; the source's latch looked at (an error), and once a
 * second whether another pipe is on or the eDP pipe is alone again.
 * Called under the screen's lock; takes no lock of its own.
 */
EXPORT void knl_igfx_dirty( void )
{
	UD	t, gap, idle;
	UW	iir, st;
	ID	me;
	INT	i;

	if ( mm == 0 ) return;
	if ( !drawn ) {
		drawn = TRUE;
		if ( psr_tsk > 0 ) tk_wup_tsk(psr_tsk);
	}
	if ( !active ) {
		if ( !broken && dmc_ok && holds == 0 ) {
			t = now_ns();
			if ( t - pipes_at >= 1000000000ULL ) {
				pipes_at = t;
				if ( pipes_other() == 0 ) {
					other_pipe = FALSE;
					if ( psr_tsk > 0 ) tk_wup_tsk(psr_tsk);
				}
			}
		}
		return;
	}
	st = PSR_STATE(rd(R_PSR_STATUS));
	wr(R_PSR_CTL, rd(R_PSR_CTL) & ~PSR_ENABLE);
	wr(R_CURSURFLIVE_A, 0);
	t = now_ns();
	n_dirty++;
	st_at_send[st]++;
	gap = t - t_last;
	idle = frame_ns * (UD)idle_frames;
	iir = rd(R_PSR_IIR) & PSR_IRQ_ALL;
	if ( iir != 0 ) {
		wr(R_PSR_IIR, iir);
		wr(R_DE_MISC_IIR, DE_MISC_EDP_PSR);	/* the latch above it too, so that the next is latched */
	}
	if ( iir & PSR_IRQ_PRE_ENTRY ) n_latch++;
	if ( st != 0 && !sw_exit ) {
		/* enabled since the last exit and not idle when the send came: it had entered */
		n_entry++;
		n_exit[WHY_DIRTY]++;
		if ( gap > idle ) in_ns += gap - idle;
	}
	dirty_seq++;
	sw_exit = TRUE;
	if ( psr_tsk > 0 ) tk_wup_tsk(psr_tsk);
	me = tk_get_tid();
	for ( i = 0; i < SENDERS; i++ ) {
		if ( snd_tid[i] == me || snd_tid[i] == 0 ) {
			snd_tid[i] = me;
			snd_n[i]++;
			break;
		}
	}
	if ( i == SENDERS ) snd_rest++;
	if ( iir & PSR_IRQ_ERROR ) {
		err_seen = TRUE;
		if ( psr_tsk > 0 ) tk_wup_tsk(psr_tsk);
	}
	if ( !pw2_let_go && t - pipes_at >= 1000000000ULL ) {	/* no other pipe can be on without power well 2 */
		pipes_at = t;
		if ( pipes_other() > 0 ) {
			other_pipe = TRUE;
			if ( psr_tsk > 0 ) tk_wup_tsk(psr_tsk);
		}
	}
}

/*
 * PSR and the DC states off while the plane is changed, the panel is
 * dark or the machine sleeps (hold TRUE), and on again when the last
 * such is over (FALSE). After a sleep that lost the DMC's program, it
 * is loaded again first.
 */
EXPORT void knl_igfx_psr_hold( BOOL hold, CONST char *what )
{
	if ( psr_mtx <= 0 ) return;
	tk_loc_mtx(psr_mtx, TMO_FEVR);
	if ( hold ) {
		holds++;
		if ( active || dc_now != 0 ) psr_off(WHY_EXPLICIT, what);
	} else if ( holds > 0 && --holds == 0 ) {
		if ( dmc_ok && !dmc_there() ) {
			dc_now = 0;
			dmc_ok = dmc_load("again");
			if ( !dmc_ok ) broken = TRUE;
		}
		psr_on();
	}
	tk_unl_mtx(psr_mtx);
}

/*
 * The DC states held off while the records read the display's registers
 * (pause TRUE), and allowed again as they were (FALSE). PSR stays on.
 */
EXPORT void knl_igfx_dc_pause( BOOL pause )
{
	if ( psr_mtx <= 0 ) return;
	tk_loc_mtx(psr_mtx, TMO_FEVR);
	if ( pause ) {
		dc_paused = dc_now;
		if ( dc_now != 0 ) {
			dc_set(0);
			WaitUsec(1000);		/* the wells back from the DMC */
		}
	} else {
		if ( dc_paused != 0 && active && dc_now == 0 && holds == 0 ) dc_set(dc_paused);
		dc_paused = 0;
	}
	tk_unl_mtx(psr_mtx);
}

/* What DC_STATE_EN is set to allow (not what it reads while the records hold the DC states off) */
EXPORT UW knl_igfx_dc_setting( void )
{
	return dc_now;
}

/* Whether the DC states may be on now: the display's registers other than PSR's and the DMC's are not to be read */
EXPORT BOOL knl_igfx_dc_active( void )
{
	return (BOOL)( dc_now != 0 );
}

/*
 * For the records (idlelog=1): PSR on or off and why, the entries, the
 * exits by why, the share of the time in PSR since the last line (from
 * the latch's entries and the idle frames' time, or the time alone),
 * the source's own counter, and the DMC's counts of DC5 and DC6 since
 * the last line
 */
EXPORT INT knl_igfx_psr_text( char *t )
{
	UD	now, span, in;
	UW	dc5, dc6, perf;
	INT	n, pm, i;
	T_DISPSTAT	ds;

	if ( mm == 0 ) return tm_sprintf((UB *)t, (CONST UB *)"; PSR off: %s", ( why_off != NULL ) ? why_off : "not started");
	now = now_ns();
	in = in_ns;
	if ( active && !sw_exit && now - t_last > frame_ns * (UD)idle_frames ) {
		in += now - t_last - frame_ns * (UD)idle_frames;
	}
	dc5 = rd(R_DMC_DC3_DC5);
	dc6 = rd(R_DMC_DC5_DC6);
	perf = rd(R_PSR_PERF_CNT) & PSR_PERF_MASK;
	span = now - txt_at;
	pm = ( txt_first || span == 0 ) ? 0 : (INT)( ( in - txt_in ) * 1000 / span );
	if ( pm > 1000 ) pm = 1000;
	n = tm_sprintf((UB *)t, (CONST UB *)"; PSR %s%s%s, entries %ld (latch %ld), exits: dirty %ld (idle within 50 ms %ld, "
		       "not %ld, last %08x; enabled again %ld) AUX error %ld timeout %ld explicit %ld; STATUS %08x, states at sends",
		       active ? "on" : "off", ( !active && why_off != NULL ) ? ": " : "",
		       ( !active && why_off != NULL ) ? why_off : "", (D)n_entry, (D)n_latch, (D)n_exit[WHY_DIRTY],
		       (D)n_left, (D)n_stayed, stayed_st, (D)n_reenable, (D)n_exit[WHY_AUX], (D)n_exit[WHY_TIMEOUT],
		       (D)n_exit[WHY_EXPLICIT], rd(R_PSR_STATUS));
	for ( i = 0; i < 8; i++ ) {
		if ( st_at_send[i] != 0 ) n += tm_sprintf((UB *)t + n, (CONST UB *)" %d:%ld", i, (D)st_at_send[i]);
	}
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; sends %ld by task", (D)n_dirty);
	for ( i = 0; i < SENDERS && snd_tid[i] != 0; i++ ) {
		n += tm_sprintf((UB *)t + n, (CONST UB *)" %d:%ld", (INT)snd_tid[i], (D)snd_n[i]);
	}
	if ( snd_rest != 0 ) n += tm_sprintf((UB *)t + n, (CONST UB *)" others:%ld", (D)snd_rest);
	if ( ts_disp_stat(&ds) >= E_OK && !txt_first && ds.sends > txt_ds.sends ) {
		n += tm_sprintf((UB *)t + n, (CONST UB *)", in 10 s %ld sends of %ld rectangles, %ld rows, %ld pixels",
				(D)( ds.sends - txt_ds.sends ), (D)( ds.rects - txt_ds.rects ), (D)( ds.rows - txt_ds.rows ),
				(D)( ds.pixels - txt_ds.pixels ));
	}
	txt_ds = ds;
	n += tm_sprintf((UB *)t + n, (CONST UB *)"; in PSR %d.%d%%, PERF_CNT +%d; DC5 +%d DC6 +%d (DC_STATE_EN %x, "
			"power well 2 %s)", pm / 10, pm % 10, txt_first ? 0 : (INT)( ( perf - txt_perf ) & PSR_PERF_MASK ),
			txt_first ? 0 : (INT)( dc5 - txt_dc5 ), txt_first ? 0 : (INT)( dc6 - txt_dc6 ), dc_now,
			pw2_let_go ? "let go" : "asked for");
	txt_at = now;
	txt_in = in;
	txt_dc5 = dc5;
	txt_dc6 = dc6;
	txt_perf = perf;
	txt_first = FALSE;
	return n;
}

/*
 * From the display driver's start (disp_igfx.c), once the plane is the
 * driver's: the settings read and, with PSR asked for, the task that
 * loads the DMC's program and turns PSR on started
 */
EXPORT void knl_igfx_psr_start( void )
{
	UB	v[8];
	T_CMTX	cm;
	T_CTSK	ct;

	if ( knl_bootarg(knl_bootargs, "i915.enable_psr", v, sizeof(v)) ) psr_mode = ( v[0] == '1' ) ? 1 : 0;
	if ( knl_bootarg(knl_bootargs, "i915.enable_dc", v, sizeof(v)) ) {
		dc_mode = ( v[0] == '0' ) ? 0 : ( v[0] == '1' ) ? 1 : ( v[0] == '2' ) ? 2 : -1;
	}
	psr_debug = (BOOL)( knl_bootarg(knl_bootargs, "i915.psr_debug", v, sizeof(v)) && v[0] == '1' );
	pw_sync = (BOOL)( knl_bootarg(knl_bootargs, "igfx.pwsync", v, sizeof(v)) && v[0] == '1' );
	ext_on = (BOOL)( knl_bootarg(knl_bootargs, "ext", v, sizeof(v)) && v[0] == '1' );
	if ( psr_mode != 1 ) {
		tm_printf((UB *)"igfx: psr: off (i915.enable_psr=1 turns it on)\n");
		why_off = "i915.enable_psr=0";
		return;
	}
	mm = knl_igfx_mmio();
	if ( mm == 0 ) return;
	knl_memset(&cm, 0, sizeof(cm));
	cm.mtxatr = TA_INHERIT;
	psr_mtx = tk_cre_mtx(&cm);
	if ( psr_mtx <= 0 ) {
		mm = 0;
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)psr_task;
	ct.itskpri = 30;
	ct.stksz = 16 * 1024;
	psr_tsk = tk_cre_tsk(&ct);
	if ( psr_tsk > 0 ) tk_sta_tsk(psr_tsk, 0);
	knl_fbcon_wake = knl_igfx_psr_panic;
	tm_printf((UB *)"igfx: psr: asked for (i915.enable_psr=1), DC states %s%s%s\n",
		  ( dc_mode == 0 ) ? "none" : ( dc_mode == 1 ) ? "up to DC5" : "up to DC6",
		  psr_debug ? ", made to leave every second (i915.psr_debug=1)" : "",
		  ext_on ? ", power well 2 asked for only while ext=1 looks at a port" : "");
}

#endif /* X64 */
