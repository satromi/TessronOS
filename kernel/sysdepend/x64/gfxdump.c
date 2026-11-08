/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gfxdump.c (x64 PC)
 *	The display engine of Intel's integrated graphics of the 9th
 *	generation (Skylake, Kaby Lake, Amber Lake: UHD 620 8086:5917, UHD
 *	615 8086:591C and their kin), read as the firmware left it and told
 *	as text for DISPLAY.TXT (bootlog.c), before a driver of its own is
 *	written: which pipe and transcoder drive the panel at what timing,
 *	whether a pipe scaler stretches the plane, where the plane's surface
 *	is in the graphics translation table, the panel's power and
 *	backlight, the stolen memory and the OpRegion with its VBT.
 *
 *	Nothing is written: every register is only read, through the
 *	device window over the first BAR (the registers in its first 8MB,
 *	the global GTT from 8MB on).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "igt_gen.h"

IMPORT INT knl_igfx_log( char *buf, INT max );		/* disp_igfx.c */

typedef struct {
	CONST char	*name;
	UW		off;
} GREG;

/* The registers read, as the 9th generation's display engine places them */
LOCAL CONST GREG	gregs[] = {
	{ "CDCLK_CTL", 0x46000 }, { "LCPLL1_CTL", 0x46010 }, { "LCPLL2_CTL", 0x46014 },
	{ "DPLL_CTRL1", 0x6c058 }, { "DPLL_CTRL2", 0x6c05c }, { "DPLL_STATUS", 0x6c060 },
	{ "DPLL1_CFGCR1", 0x6c040 }, { "DPLL1_CFGCR2", 0x6c044 },
	{ "DPLL2_CFGCR1", 0x6c048 }, { "DPLL2_CFGCR2", 0x6c04c },
	{ "DPLL3_CFGCR1", 0x6c050 }, { "DPLL3_CFGCR2", 0x6c054 },
	{ "DDI_BUF_CTL_A", 0x64000 }, { "DDI_BUF_CTL_B", 0x64100 }, { "DDI_BUF_CTL_C", 0x64200 },
	{ "DDI_BUF_CTL_D", 0x64300 }, { "DDI_BUF_CTL_E", 0x64400 },
	{ "TRANS_DDI_FUNC_CTL_A", 0x60400 }, { "TRANS_DDI_FUNC_CTL_B", 0x61400 },
	{ "TRANS_DDI_FUNC_CTL_C", 0x62400 }, { "TRANS_DDI_FUNC_CTL_EDP", 0x6f400 },
	{ "PIPECONF_A", 0x70008 }, { "PIPECONF_B", 0x71008 }, { "PIPECONF_C", 0x72008 },
	{ "PIPECONF_EDP", 0x7f008 },
	{ "PIPE_SRC_A", 0x6001c }, { "PIPE_SRC_B", 0x6101c }, { "PIPE_SRC_C", 0x6201c },
	{ "PS_CTRL_1_A", 0x68180 }, { "PS_WIN_POS_1_A", 0x68170 }, { "PS_WIN_SZ_1_A", 0x68174 },
	{ "PS_CTRL_2_A", 0x68280 }, { "PS_WIN_POS_2_A", 0x68270 }, { "PS_WIN_SZ_2_A", 0x68274 },
	{ "PS_CTRL_1_B", 0x68980 }, { "PS_WIN_POS_1_B", 0x68970 }, { "PS_WIN_SZ_1_B", 0x68974 },
	{ "PS_CTRL_2_B", 0x68a80 }, { "PS_WIN_POS_2_B", 0x68a70 }, { "PS_WIN_SZ_2_B", 0x68a74 },
	{ "PS_CTRL_1_C", 0x69180 }, { "PS_WIN_POS_1_C", 0x69170 }, { "PS_WIN_SZ_1_C", 0x69174 },
	{ "PLANE_CTL_1_A", 0x70180 }, { "PLANE_STRIDE_1_A", 0x70188 }, { "PLANE_POS_1_A", 0x7018c },
	{ "PLANE_SIZE_1_A", 0x70190 }, { "PLANE_SURF_1_A", 0x7019c }, { "PLANE_OFFSET_1_A", 0x701a4 },
	{ "PLANE_CTL_1_B", 0x71180 }, { "PLANE_STRIDE_1_B", 0x71188 }, { "PLANE_POS_1_B", 0x7118c },
	{ "PLANE_SIZE_1_B", 0x71190 }, { "PLANE_SURF_1_B", 0x7119c }, { "PLANE_OFFSET_1_B", 0x711a4 },
	{ "PLANE_CTL_1_C", 0x72180 }, { "PLANE_STRIDE_1_C", 0x72188 }, { "PLANE_POS_1_C", 0x7218c },
	{ "PLANE_SIZE_1_C", 0x72190 }, { "PLANE_SURF_1_C", 0x7219c }, { "PLANE_OFFSET_1_C", 0x721a4 },
	{ "PP_STATUS", 0xc7200 }, { "PP_CONTROL", 0xc7204 }, { "PP_ON_DELAYS", 0xc7208 },
	{ "PP_OFF_DELAYS", 0xc720c },
	{ "BLC_PWM_CTL", 0x48250 }, { "BLC_PWM_DATA", 0x48254 },
	{ "BLC_PWM_PCH_CTL1", 0xc8250 }, { "BLC_PWM_PCH_CTL2", 0xc8254 },
	{ "PWR_WELL_CTL2", 0x45404 }, { "FUSE_STATUS", 0x42000 }, { "SFUSE_STRAP", 0xc2014 },
	{ "DBUF_CTL", 0x45008 },
	{ "PLANE_BUF_CFG_1_A", 0x7027c }, { "PLANE_WM_1_A_0", 0x70240 }, { "PLANE_WM_1_A_1", 0x70244 },
	{ "PLANE_WM_TRANS_1_A", 0x70268 }, { "CUR_BUF_CFG_A", 0x7017c },
	{ "PLANE_BUF_CFG_1_B", 0x7127c }, { "PLANE_BUF_CFG_1_C", 0x7227c },
	{ "PLANE_WM_1_A_2", 0x70248 }, { "PLANE_WM_1_A_3", 0x7024c }, { "PLANE_WM_1_A_4", 0x70250 },
	{ "PLANE_WM_1_A_5", 0x70254 }, { "PLANE_WM_1_A_6", 0x70258 }, { "PLANE_WM_1_A_7", 0x7025c },
	{ "PLANE_SURFLIVE_1_A", 0x701ac }, { "PIPEDSL_A", 0x70000 },
	{ "TRANS_DATA_M1_EDP", 0x6f030 }, { "TRANS_DATA_N1_EDP", 0x6f034 },
	{ "TRANS_LINK_M1_EDP", 0x6f040 }, { "TRANS_LINK_N1_EDP", 0x6f044 },
	{ "DE_PIPE_IMR_A", 0x44404 }, { "DE_PIPE_IIR_A", 0x44408 }, { "DE_PIPE_IER_A", 0x4440c },
	{ "MASTER_IRQ", 0x44200 }, { "CHICKEN_PAR1_1", 0x42080 }, { "PIPE_BOTTOM_COLOR_A", 0x70034 },
};
#define N_GREGS		( sizeof(gregs) / sizeof(gregs[0]) )

/* Each transcoder's timing: HTOTAL, HBLANK, HSYNC, VTOTAL, VBLANK, VSYNC from its base */
LOCAL CONST GREG	trans[] = {
	{ "A", 0x60000 }, { "B", 0x61000 }, { "C", 0x62000 }, { "EDP", 0x6f000 },
};

typedef struct {
	char	*p;
	INT	len, cap;
} GBUF;

LOCAL void gput( GBUF *b, CONST char *s )
{
	while ( *s != '\0' && b->len < b->cap - 1 ) b->p[b->len++] = *s++;
	b->p[b->len] = '\0';
}

#define gfmt(b, ...)	do { UB t_[200]; tm_sprintf(t_, (CONST UB *)__VA_ARGS__); gput((b), (CONST char *)t_); } while (0)

LOCAL UW rd( UBINT mmio, UW off )
{
	return *(volatile UW *)( mmio + off );
}

/* The two halves of a timing register: (value & 0x1fff) + 1 and (value >> 16 & 0x1fff) + 1 */
LOCAL void timing( GBUF *b, CONST char *name, UW v )
{
	gfmt(b, "    %-7s %08x  active/start %d, total/end %d\n", name, v, ( v & 0x1fff ) + 1,
	     ( ( v >> 16 ) & 0x1fff ) + 1);
}

EXPORT INT knl_gfx_report( char *buf, INT max )
{
	GBUF		b;
	T_PCIDEV	d;
	UD		bar0, bar2, gtt_va, surf;
	UBINT		mmio;
	UW		v, bdsm, asls, ggc, cmd;
	INT		i, k;

	b.p = buf;
	b.len = 0;
	b.cap = max;
	buf[0] = '\0';
	for ( i = 0; ; i++ ) {
		if ( ts_pcie_find(0x03, 0x00, 0x00, i, &d) < E_OK ) {
			return 0;		/* no Intel display: no file */
		}
		if ( d.vendor == 0x8086 ) break;
	}
	bar0 = d.bar[0];
	bar2 = d.bar[2];
	cmd = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x04, 2);
	ggc = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x50, 4);
	bdsm = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x5c, 4);
	asls = ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0xfc, 4);
	gfmt(&b, "TessronOS x64: the display engine of %04x:%04x at %02x:%02x.%d, as the firmware left it (only read)\n",
	     d.vendor, d.device, d.bus, d.dev, d.fn);
	gfmt(&b, "  GTTMMADR (BAR0) %lx, %ld bytes; GMADR (BAR2) %lx, %ld bytes\n", bar0, (D)d.bar_size[0],
	     bar2, (D)d.bar_size[2]);
	gfmt(&b, "  command %04x, GGC %08x (GGMS %d, GMS %d), BDSM %08x, ASLS %08x\n", cmd, ggc, ( ggc >> 6 ) & 3,
	     ( ggc >> 8 ) & 0xff, bdsm, asls);
	if ( bar0 == 0 || d.bar_size[0] < 16 * 1024 * 1024 ) {
		gfmt(&b, "  the first BAR is not of the registers and the GTT: nothing more read\n");
		return b.len;
	}
	/*
	 * The places below are the 9th generation's: on a later one they are
	 * other registers or none. What such a machine tells is igt12.c's
	 */
	if ( knl_igt_gen(d.device) != NULL ) {
		gfmt(&b, "  graphics of the 11th generation or later: what is told of it is in the lines \"igt12:\"\n");
		return b.len;
	}
	mmio = (UBINT)DEV_BASE(bar0);
	gtt_va = (UD)DEV_BASE(bar0 + 8 * 1024 * 1024);

	gfmt(&b, "\n[registers]\n");
	for ( k = 0; k < (INT)N_GREGS; k++ ) {
		v = rd(mmio, gregs[k].off);
		gfmt(&b, "  %-24s %05x  %08x\n", gregs[k].name, gregs[k].off, v);
	}
	gfmt(&b, "\n[transcoder timings]\n");
	for ( k = 0; k < (INT)( sizeof(trans) / sizeof(trans[0]) ); k++ ) {
		gfmt(&b, "  transcoder %s\n", trans[k].name);
		timing(&b, "HTOTAL", rd(mmio, trans[k].off + 0x00));
		timing(&b, "HBLANK", rd(mmio, trans[k].off + 0x04));
		timing(&b, "HSYNC", rd(mmio, trans[k].off + 0x08));
		timing(&b, "VTOTAL", rd(mmio, trans[k].off + 0x0c));
		timing(&b, "VBLANK", rd(mmio, trans[k].off + 0x10));
		timing(&b, "VSYNC", rd(mmio, trans[k].off + 0x14));
	}
	gfmt(&b, "\n[pipe sources and planes]\n");
	for ( k = 0; k < 3; k++ ) {
		v = rd(mmio, 0x6001c + k * 0x1000);
		gfmt(&b, "  pipe %c: source %d x %d", 'A' + k, ( ( v >> 16 ) & 0x1fff ) + 1, ( v & 0x1fff ) + 1);
		v = rd(mmio, 0x70190 + k * 0x1000);
		gfmt(&b, ", plane 1 %d x %d", ( v & 0x1fff ) + 1, ( ( v >> 16 ) & 0x1fff ) + 1);
		v = rd(mmio, 0x70180 + k * 0x1000);
		gfmt(&b, ", plane 1 %s, format %d, tiling %d\n", ( v & 0x80000000U ) ? "on" : "off",
		     ( v >> 24 ) & 0xf, ( v >> 10 ) & 7);
	}

	gfmt(&b, "\n[global GTT]\n");
	for ( k = 0; k < 8; k++ ) {
		UD	e = *(volatile UD *)( gtt_va + (UD)k * 8 );

		gfmt(&b, "  entry %d: %016lx\n", k, e);
	}
	for ( k = 0; k < 3; k++ ) {
		surf = rd(mmio, 0x7019c + k * 0x1000) & ~0xfffU;
		if ( surf == 0 ) continue;
		gfmt(&b, "  pipe %c's plane 1 surface at GTT offset %lx:", 'A' + k, surf);
		for ( i = 0; i < 4; i++ ) {
			UD	e = *(volatile UD *)( gtt_va + ( ( surf >> 12 ) + (UD)i ) * 8 );

			gfmt(&b, " %016lx", e);
		}
		gfmt(&b, "\n");
	}

	gfmt(&b, "\n[OpRegion]\n");
	if ( asls == 0 || asls == 0xffffffffU ) {
		gfmt(&b, "  none (ASLS 0)\n");
	} else {
		CONST volatile UB	*op = (CONST volatile UB *)( DEV_VA_BASE + (UD)asls );	/* below 4GB */
		char		sig[17];

		for ( i = 0; i < 16; i++ ) sig[i] = ( op[i] >= 0x20 && op[i] < 0x7f ) ? (char)op[i] : '.';
		sig[16] = '\0';
		gfmt(&b, "  at %08x: signature \"%s\", size %d KB, version %08x, mailboxes %08x\n", asls, sig,
		     *(CONST volatile UW *)( op + 0x10 ), *(CONST volatile UW *)( op + 0x14 ),
		     *(CONST volatile UW *)( op + 0x58 ));
		gfmt(&b, "  mailbox 4 starts \"%c%c%c%c\"%s\n", op[0x400], op[0x401], op[0x402], op[0x403],
		     ( op[0x400] == '$' && op[0x401] == 'V' && op[0x402] == 'B' && op[0x403] == 'T' )
		     ? ": the VBT is there" : "");
	}
	gfmt(&b, "\n[what the display driver did]\n");
	if ( b.len < b.cap - 1 ) {
		b.len += knl_igfx_log(b.p + b.len, b.cap - b.len);
	}
	return b.len;
}

#endif /* X64 */
