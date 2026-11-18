/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_ext.c (x64 PC)
 *	The external outputs of Intel's integrated graphics of the 9th
 *	generation (docs/tessronos/06-device.md 6.9.1): which DDI ports have a
 *	connector, as the VBT's child devices say (their DVO port, AUX
 *	channel and DDC pin); what is plugged into each, found by the PCH's
 *	hot plug detection; and what it is, read when it is plugged in: its
 *	DPCD (the receiver's capabilities, the link's state, a branch
 *	device's downstream ports) and its EDID with its extensions, through
 *	I2C over AUX, or a dual mode adaptor's identifier and the EDID over
 *	GMBUS where the port is DP++ and AUX does not answer. Each port is an
 *	output of the layer of outputs (dispout.c). A DisplayPort port is lit
 *	on pipe B when the layer turns it on, as i915 does on this generation:
 *	a free DPLL at the link's rate, the port's buffer levels, the link
 *	trained, the transcoder's timing and M/N, the pipe and its plane on
 *	pages of its own in the GGTT; pipe A's part of the display buffer is
 *	made room for first, SAGV off, and the panel's self refresh held off
 *	while pipe B is on. The panel's link and its DPLL are not touched.
 *
 *	While a port is read the panel's self refresh is held off
 *	(knl_igfx_psr_hold, which asks for power well 2 again: the AUX
 *	channels of B to D and GMBUS are under it) and the DDI's IO power well
 *	is asked for; both are let go when it is read.
 *
 *	At the start, read only: what the firmware left on pipes B and C
 *	(their pipe, transcoder, DDI, PLL, plane and link M/N), for whether a
 *	pipe it lit could be taken over; and who turns a Type-C port to
 *	DisplayPort (an ACPI UCSI device, a PD controller on I2C), as far as
 *	the namespace says.
 *
 *	The hot plug interrupts come through the drawing driver's (igt.c, the
 *	master interrupt's PCH bit): a long pulse is a plug or an unplug, a
 *	short one (IRQ_HPD) on a lit port has its link looked at. The state is
 *	also looked at every EXT_POLL_MS, for a machine whose interrupt does
 *	not come. The start-up setting ext=0 leaves all of it out.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <tk/tkernel.h>
#include <ts/disp.h>
#include <ts/dispout.h>
#include "sysdepend.h"
#include "sysman/pfalloc.h"
#include "../disp_hw.h"
#include "disp_pc.h"
#include "sysdepend/x64/igt_ggtt.h"
#include "sysdepend/acpi/acpica/tsacpi_os.h"

IMPORT UBINT	knl_igfx_mmio( void );					/* disp_igfx.c */
IMPORT INT	knl_igfx_vbt_child( INT i, UB *buf, INT max );		/* disp_igfx.c */
IMPORT void	knl_igfx_psr_hold( BOOL hold, CONST char *what );	/* disp_igfx_psr.c */
IMPORT INT	knl_igfx_aux_xfer( INT ch, CONST UB *tx, INT ntx, UB *rx, INT max );	/* disp_igfx_aux.c */
IMPORT INT	knl_igfx_dpcd_read( INT ch, UW addr, UB *buf, INT len );
IMPORT INT	knl_igfx_dpcd_write( INT ch, UW addr, CONST UB *buf, INT len );

#define EXT_POLL_MS		10000

/* ---------------------------------------------------------------- registers */

#define SDEISR			0xc4000
#define SDE_HPD(port)		( 1U << ( 20 + (port) ) )	/* port 1 B, 2 C, 3 D */
#define PCH_PORT_HOTPLUG	0xc4030
#define HP_ENABLE(port)		( 1U << ( 4 + ( (port) - 1 ) * 8 ) )

#define PWR_WELL_CTL2		0x45404
#define PW_REQ(i)		( 1U << ( 2 * (i) + 1 ) )
#define PW_STATE(i)		( 1U << ( 2 * (i) ) )
#define PW_DDI_IO(port)		PW_REQ(1 + (port))	/* the DDI's IO well: B 2, C 3, D 4 */
#define PW_2			15			/* power well 2: pipes B and C, the transcoders, AUX B-D */

#define GMBUS0			0xc5100
#define GMBUS1			0xc5104
#define GMBUS2			0xc5108
#define GMBUS3			0xc510c
#define GM_SW_RDY		( 1U << 30 )
#define GM_CYCLE_WAIT		( 1U << 25 )
#define GM_CYCLE_STOP		( 4U << 25 )
#define GM_READ			1U
#define GM_HW_RDY		( 1U << 11 )
#define GM_SATOER		( 1U << 10 )

/* what a pipe is set to (pipe and transcoder 1 B, 2 C) */
#define R_TRANS(t, off)		( 0x60000 + 0x1000 * (t) + (off) )
#define R_PIPECONF(p)		( 0x70008 + 0x1000 * (p) )
#define R_TRANS_DDI(t)		( 0x60400 + 0x1000 * (t) )
#define R_TRANS_CLK_SEL(t)	( 0x46140 + 4 * (t) )
#define R_PLANE(p, off)		( 0x70180 + 0x1000 * (p) + (off) )
#define R_DDI_BUF_CTL(port)	( 0x64000 + 0x100 * (port) )
#define R_DP_TP_CTL(port)	( 0x64040 + 0x100 * (port) )
#define R_DPLL_CTRL1		0x6c058
#define R_DPLL_CTRL2		0x6c05c
#define R_DPLL_STATUS		0x6c060
#define R_LCPLL2_CTL		0x46014
#define R_WRPLL_CTL(i)		( 0x46040 + 0x20 * (i) )
#define R_DPLL_CFGCR1(i)	( 0x6c040 + 8 * (i) )
#define R_DPLL_CFGCR2(i)	( 0x6c044 + 8 * (i) )

/* I2C over AUX */
#define AUX_I2C_WRITE		0x0
#define AUX_I2C_READ		0x1
#define AUX_I2C_MOT		0x4
#define AUX_REPLY_MASK		0x30
#define AUX_I2C_REPLY_MASK	0xc0
#define AUX_ACK			0x00
#define AUX_I2C_DEFER		0x80
#define AUX_DEFER		0x20

#define DDC_EDID		0x50
#define DDC_SEGMENT		0x30
#define DDC_ADAPTOR		0x40		/* a dual mode adaptor's identifier (DP++) */

/* the VBT's child devices: the device types and DVO ports of i915's intel_vbt_defs.h */
#define VBT_DP			0x0004		/* DEVICE_TYPE_DISPLAYPORT_OUTPUT */
#define VBT_TMDS		0x0010		/* DEVICE_TYPE_TMDS_DVI_SIGNALING */
#define VBT_NOT_HDMI		0x0800		/* DEVICE_TYPE_NOT_HDMI_OUTPUT */
#define VBT_INTERNAL		0x1000		/* DEVICE_TYPE_INTERNAL_CONNECTOR */

/* ---------------------------------------------------------------- the ports */

#define KIND_DP			0
#define KIND_DPPP		1		/* DisplayPort that also gives TMDS (DP++) */
#define KIND_HDMI		2
#define EXT_PORTS		3

typedef struct {
	INT		port;			/* the DDI: 1 B, 2 C, 3 D */
	INT		kind;			/* KIND_* */
	INT		aux;			/* its AUX channel, 0 to 3 */
	INT		ddc;			/* its GMBUS pin */
	BOOL		plugged;
	BOOL		dp;			/* what is plugged in answers on AUX */
	UB		caps[16];		/* its DPCD 000h-00Fh */
	T_DISPMODE	mode;			/* while lit: the mode */
	T_DISPHW	hw;			/* and the framebuffer */
	UINT		src_w, src_h;		/* the framebuffer's size the pipe scaler shows in the mode (fit); 0: the mode's */
	UW		link_khz;
	INT		lanes;
	INT		dpll;			/* the DPLL it uses, -1 none */
	INT		adaptor_type;		/* a DP++ adaptor's type: 1, 2; 0 none seen */
	UW		adaptor_tmds;		/* a type 2 adaptor's TMDS clock limit, kHz */
	UW		hdmi_level;		/* the VBT's HDMI level (hdmi_level_shifter) */
	UB		*edid;			/* as long as the EDID read, NULL none */
	INT		edid_len;
	char		name[12];
	T_DISPOUTHW	out;
} EXTPORT;

LOCAL EXTPORT	ext_port[EXT_PORTS];
LOCAL INT	ext_nport = 0;
LOCAL UBINT	ex_mmio = 0;
LOCAL ID	ext_mtx = 0;			/* the ports' EDIDs while they are read */

LOCAL UW ex_rd( UW off )
{
	return in_w(ex_mmio + off);
}

LOCAL void ex_wr( UW off, UW v )
{
	out_w(ex_mmio + off, v);
}

/* ---------------------------------------------------------------- what the firmware left */

/* A pipe as the firmware left it, every register of it, when power well 2 is on */
LOCAL void pipe_say( INT p )
{
	UW	ddi = ex_rd(R_TRANS_DDI(p)), conf = ex_rd(R_PIPECONF(p));
	INT	port = (INT)( ( ddi >> 28 ) & 7 );

	tm_printf((UB *)"ext: firmware: pipe %c PIPECONF %08x TRANS_DDI_FUNC_CTL %08x TRANS_CLK_SEL %08x\n",
		  'A' + p, conf, ddi, ex_rd(R_TRANS_CLK_SEL(p)));
	tm_printf((UB *)"ext: firmware: pipe %c timing H %08x %08x %08x V %08x %08x %08x PIPE_SRC %08x\n", 'A' + p,
		  ex_rd(R_TRANS(p, 0x00)), ex_rd(R_TRANS(p, 0x04)), ex_rd(R_TRANS(p, 0x08)),
		  ex_rd(R_TRANS(p, 0x0c)), ex_rd(R_TRANS(p, 0x10)), ex_rd(R_TRANS(p, 0x14)), ex_rd(R_TRANS(p, 0x1c)));
	tm_printf((UB *)"ext: firmware: pipe %c DATA_M1 %08x DATA_N1 %08x LINK_M1 %08x LINK_N1 %08x\n", 'A' + p,
		  ex_rd(R_TRANS(p, 0x30)), ex_rd(R_TRANS(p, 0x34)), ex_rd(R_TRANS(p, 0x40)), ex_rd(R_TRANS(p, 0x44)));
	tm_printf((UB *)"ext: firmware: pipe %c PLANE_CTL %08x STRIDE %08x POS %08x SIZE %08x SURF %08x OFFSET %08x BUF_CFG %08x\n",
		  'A' + p, ex_rd(R_PLANE(p, 0x00)), ex_rd(R_PLANE(p, 0x08)), ex_rd(R_PLANE(p, 0x0c)),
		  ex_rd(R_PLANE(p, 0x10)), ex_rd(R_PLANE(p, 0x1c)), ex_rd(R_PLANE(p, 0x24)), ex_rd(R_PLANE(p, 0x7c)));
	if ( ( ddi & 0x80000000U ) != 0 && port >= 1 && port <= 4 ) {
		tm_printf((UB *)"ext: firmware: pipe %c drives DDI %c: DDI_BUF_CTL %08x DP_TP_CTL %08x\n", 'A' + p,
			  'A' + port, ex_rd(R_DDI_BUF_CTL(port)), ex_rd(R_DP_TP_CTL(port)));
	}
}

LOCAL void firmware_say( void )
{
	UW	pw = ex_rd(PWR_WELL_CTL2);
	INT	i;

	tm_printf((UB *)"ext: firmware: power wells %08x (power well 2 %s); DPLL_CTRL1 %08x DPLL_CTRL2 %08x DPLL_STATUS %08x\n",
		  pw, ( pw & PW_STATE(PW_2) ) ? "on" : "off", ex_rd(R_DPLL_CTRL1), ex_rd(R_DPLL_CTRL2), ex_rd(R_DPLL_STATUS));
	tm_printf((UB *)"ext: firmware: LCPLL2_CTL %08x WRPLL_CTL1 %08x WRPLL_CTL2 %08x", ex_rd(R_LCPLL2_CTL),
		  ex_rd(R_WRPLL_CTL(0)), ex_rd(R_WRPLL_CTL(1)));
	for ( i = 0; i < 3; i++ ) {
		tm_printf((UB *)" CFGCR%d %08x %08x", i + 1, ex_rd(R_DPLL_CFGCR1(i)), ex_rd(R_DPLL_CFGCR2(i)));
	}
	tm_printf((UB *)"\n");
	if ( ( pw & PW_STATE(PW_2) ) == 0 ) {
		tm_printf((UB *)"ext: firmware: pipes B and C are off (their power well is)\n");
		return;
	}
	for ( i = 1; i <= 2; i++ ) {
		pipe_say(i);
	}
}

/* ---------------------------------------------------------------- Type-C */

/*
 * Who turns a Type-C port to DisplayPort, as far as the ACPI namespace
 * says: a UCSI device (PNP0CA0, USBC000), or a PD controller on I2C by
 * the IDs the platforms give them. Read only; said for the records.
 */
LOCAL void typec_say( void )
{
	CONST char *CONST ids[] = { "PNP0CA0", "USBC000", "INT3515", "TPS6598", "CYP0002", "CYPS0001", "MSHW0145" };
	INT	k, i, any = 0, t;
	void	*dev;
	char	path[64];

	for ( t = 0; t < 30 && !knl_acpica_ready(); t++ ) {
		tk_dly_tsk(1000);
	}
	if ( !knl_acpica_ready() ) {
		tm_printf((UB *)"ext: Type-C: no ACPI namespace to look in\n");
		return;
	}
	for ( k = 0; k < (INT)( sizeof(ids) / sizeof(ids[0]) ); k++ ) {
		for ( i = 0; i < 4 && ( dev = knl_acpica_device(ids[k], i) ) != NULL; i++ ) {
			unsigned int	addr = 0, hz = 0;
			int		pin = -1, irq = -1, mode = 0;
			char		ctrl[64], gctrl[64];

			path[0] = 0;
			(void)knl_acpica_path(dev, path, sizeof(path));
			if ( knl_acpica_i2c(dev, &addr, &hz, ctrl, sizeof(ctrl), &pin, gctrl, sizeof(gctrl), &irq, &mode) == 0 ) {
				tm_printf((UB *)"ext: Type-C: %s at %s, on I2C %s address %02x\n", ids[k], path, ctrl, addr);
			} else {
				tm_printf((UB *)"ext: Type-C: %s at %s\n", ids[k], path);
			}
			any++;
		}
	}
	if ( any == 0 ) {
		tm_printf((UB *)"ext: Type-C: no UCSI device nor a known PD controller in the namespace: "
			  "the firmware and the PD controller turn it by themselves\n");
	}
}

/*
 * The UCSI's state, read only (USB Type-C Connector System Software
 * Interface, its ACPI form as Linux's ucsi_acpi): the mailbox is the
 * memory the device's _CRS names (VERSION, CCI at 4, CONTROL at 8,
 * MESSAGE_IN at 10h); a command is written to CONTROL and sent with the
 * _DSM's function 1, its answer fetched with function 2 until the CCI
 * says it completed, then acknowledged. Only commands that ask are given
 * (GET_CAPABILITY, GET_CONNECTOR_STATUS, GET_CURRENT_CAM,
 * GET_ALTERNATE_MODES of the partner): nothing is set, so who turns a port
 * to DisplayPort (the PD controller's firmware, by the design) can be
 * seen from the records. Before them, as Linux's ucsi_init does at every
 * start, the PPM is reset (PPM_RESET, the CCI's reset bit waited for) and
 * told to say when a command completes or fails
 * (SET_NOTIFICATION_ENABLE): a PPM idle with its notifications off takes
 * no other command. The completion is waited for by reading the CCI, not
 * by the ACPI notice.
 */
#define UCSI_CCI		0x04
#define UCSI_CONTROL		0x08
#define UCSI_MSG_IN		0x10
#define CCI_COMPLETE		0x80000000U
#define CCI_ERROR		0x40000000U
#define CCI_ACK_DONE		0x20000000U
#define CCI_BUSY		0x10000000U
#define UCSI_ACK_CC_CI		0x04
#define UCSI_ACK_COMMAND	( 1ULL << 17 )
#define UCSI_PPM_RESET		0x01
#define UCSI_SET_NTFY_ENABLE	0x05
#define UCSI_NTFY_CMD_COMPLETE	( 1ULL << 16 )
#define UCSI_NTFY_ERROR		( 1ULL << 31 )
#define CCI_RESET_DONE		0x08000000U
#define UCSI_GET_CAPABILITY	0x06
#define UCSI_GET_ALT_MODES	0x0c
#define UCSI_GET_CURRENT_CAM	0x0e
#define UCSI_GET_CONN_STATUS	0x12

LOCAL CONST UB	ucsi_uuid[16] = {	/* 6f8398c2-7ca4-11e4-ad36-631042b5008f, as ACPI lays it */
	0xc2, 0x98, 0x83, 0x6f, 0xa4, 0x7c, 0xe4, 0x11, 0xad, 0x36, 0x63, 0x10, 0x42, 0xb5, 0x00, 0x8f
};

LOCAL void		*ucsi_dev = NULL;
LOCAL volatile UB	*ucsi_mb = NULL;
LOCAL INT		ucsi_nconn = 0;

LOCAL UW mb_rd( INT off )
{
	return (UW)ucsi_mb[off] | ( (UW)ucsi_mb[off + 1] << 8 ) | ( (UW)ucsi_mb[off + 2] << 16 ) | ( (UW)ucsi_mb[off + 3] << 24 );
}

LOCAL void mb_wr( INT off, UW v )
{
	INT	i;

	for ( i = 0; i < 4; i++ ) ucsi_mb[off + i] = (UB)( v >> ( 8 * i ) );
}

LOCAL void ucsi_dsm( UINT func )
{
	unsigned long long	v = 0;

	(void)knl_acpica_dsm_int(ucsi_dev, ucsi_uuid, 1, func, &v);
}

/* One command given and its answer read, then acknowledged: the CCI, 0 when nothing completed in time */
LOCAL UW ucsi_cmd( UD control, UB *in, INT len )
{
	UW	cci = 0;
	INT	t;

	mb_wr(UCSI_CONTROL, (UW)control);
	mb_wr(UCSI_CONTROL + 4, (UW)( control >> 32 ));
	ucsi_dsm(1);
	for ( t = 0; t < 50; t++ ) {
		ucsi_dsm(2);
		cci = mb_rd(UCSI_CCI);
		if ( cci & ( CCI_COMPLETE | CCI_ERROR ) ) break;
		tk_dly_tsk(2);
	}
	if ( ( cci & ( CCI_COMPLETE | CCI_ERROR ) ) == 0 ) return 0;
	for ( t = 0; t < len && t < 16; t++ ) in[t] = ucsi_mb[UCSI_MSG_IN + t];
	mb_wr(UCSI_CONTROL, (UW)( UCSI_ACK_CC_CI | UCSI_ACK_COMMAND ));
	mb_wr(UCSI_CONTROL + 4, 0);
	ucsi_dsm(1);
	for ( t = 0; t < 50; t++ ) {
		ucsi_dsm(2);
		if ( mb_rd(UCSI_CCI) & CCI_ACK_DONE ) break;
		tk_dly_tsk(2);
	}
	return cci;
}

LOCAL void ucsi_bytes( CONST char *what, INT conn, UW cci, CONST UB *b, INT n )
{
	char	t[160];
	INT	i, k;

	k = tm_sprintf((UB *)t, (CONST UB *)"ext: UCSI %s", what);
	if ( conn > 0 ) k += tm_sprintf((UB *)t + k, (CONST UB *)" of connector %d", conn);
	k += tm_sprintf((UB *)t + k, (CONST UB *)": CCI %08x%s:", cci, ( cci & CCI_ERROR ) ? " (error)" : "");
	for ( i = 0; i < n; i++ ) k += tm_sprintf((UB *)t + k, (CONST UB *)" %02x", b[i]);
	tm_sprintf((UB *)t + k, (CONST UB *)"\n");
	tm_printf((UB *)"%s", t);
}

/* The UCSI's mailbox found from the device's _CRS (a Memory32Fixed), and its capability read */
LOCAL void ucsi_start( void )
{
	UB	crs[128], in[16];
	INT	n, i;
	UD	pa = 0;
	UW	cci;
	UBINT	va;

	ucsi_dev = knl_acpica_device("PNP0CA0", 0);
	if ( ucsi_dev == NULL ) ucsi_dev = knl_acpica_device("USBC000", 0);
	if ( ucsi_dev == NULL ) return;
	n = knl_acpica_buf(ucsi_dev, "_CRS", crs, sizeof(crs));
	for ( i = 0; i + 12 <= n; ) {
		if ( crs[i] == 0x86 ) {				/* Memory32Fixed: info, base, length */
			pa = (UD)crs[i + 4] | ( (UD)crs[i + 5] << 8 ) | ( (UD)crs[i + 6] << 16 ) | ( (UD)crs[i + 7] << 24 );
			break;
		}
		if ( crs[i] & 0x80 ) {
			i += 3 + ( crs[i + 1] | ( crs[i + 2] << 8 ) );
		} else {
			if ( crs[i] == 0x79 ) break;		/* the end tag */
			i += 1 + ( crs[i] & 7 );
		}
	}
	if ( pa == 0 ) {
		tm_printf((UB *)"ext: UCSI: its mailbox not found in _CRS (%d bytes)\n", n);
		return;
	}
	va = (UBINT)knl_vmap_pa(pa & ~(UD)4095, 1, VMAP_NOCACHE);
	if ( va == 0 ) return;
	ucsi_mb = (volatile UB *)( va + ( pa & 4095 ) );
	tm_printf((UB *)"ext: UCSI: mailbox at %lx, version %02x%02x\n", pa, ucsi_mb[1], ucsi_mb[0]);
	/* the PPM reset, asked again while it says it is busy with something else */
	ucsi_dsm(2);
	tm_printf((UB *)"ext: UCSI: CCI %08x before the PPM's reset\n", mb_rd(UCSI_CCI));
	mb_wr(UCSI_CONTROL, UCSI_PPM_RESET);
	mb_wr(UCSI_CONTROL + 4, 0);
	ucsi_dsm(1);
	cci = 0;
	for ( i = 0; i < 100; i++ ) {
		tk_dly_tsk(20);
		ucsi_dsm(2);
		cci = mb_rd(UCSI_CCI);
		if ( cci & CCI_RESET_DONE ) break;
		if ( cci != 0 ) ucsi_dsm(1);
	}
	tm_printf((UB *)"ext: UCSI: PPM reset %s after %d ms (CCI %08x)\n", ( cci & CCI_RESET_DONE ) ? "done" : "NOT done",
		  20 * ( i + 1 ), cci);
	if ( ( cci & CCI_RESET_DONE ) == 0 ) return;
	knl_memset(in, 0, sizeof(in));
	cci = ucsi_cmd(UCSI_SET_NTFY_ENABLE | UCSI_NTFY_CMD_COMPLETE | UCSI_NTFY_ERROR, in, 0);
	tm_printf((UB *)"ext: UCSI: notifications of completions and errors on (CCI %08x)\n", cci);
	knl_memset(in, 0, sizeof(in));
	cci = ucsi_cmd(UCSI_GET_CAPABILITY, in, 16);
	ucsi_bytes("capability", 0, cci, in, 16);
	if ( cci & CCI_COMPLETE ) ucsi_nconn = in[4];
}

/*
 * Each connector's state: connected or not and the partner (status), the
 * alternate mode now (current CAM), and the partner's first alternate
 * mode (its SVID, FF01h being DisplayPort's)
 */
LOCAL void ucsi_say( CONST char *when )
{
	UB	in[16];
	INT	c;
	UW	cci;

	if ( ucsi_mb == NULL ) return;
	tm_printf((UB *)"ext: UCSI: %s, %d connector(s)\n", when, ucsi_nconn);
	for ( c = 1; c <= ucsi_nconn && c <= 4; c++ ) {
		knl_memset(in, 0, sizeof(in));
		cci = ucsi_cmd(UCSI_GET_CONN_STATUS | ( (UD)c << 16 ), in, 16);
		ucsi_bytes("status", c, cci, in, 16);
		knl_memset(in, 0, sizeof(in));
		cci = ucsi_cmd(UCSI_GET_CURRENT_CAM | ( (UD)c << 16 ), in, 4);
		ucsi_bytes("current alternate mode", c, cci, in, 4);
		knl_memset(in, 0, sizeof(in));
		cci = ucsi_cmd(UCSI_GET_ALT_MODES | ( 1ULL << 16 ) | ( (UD)c << 24 ), in, 12);
		ucsi_bytes("the partner's alternate modes", c, cci, in, 12);
	}
}

/* ---------------------------------------------------------------- I2C over AUX */

/* One I2C over AUX request, asked again on a DEFER: the reply's data bytes, or an error */
LOCAL INT i2c_aux( INT ch, UB cmd, UB addr, CONST UB *data, INT n, UB *rx, INT rmax )
{
	UB	tx[20], r[20];
	INT	k, got, ntx;

	tx[0] = (UB)( cmd << 4 );
	tx[1] = 0;
	tx[2] = addr;
	ntx = 3;
	if ( n > 0 || ( cmd & AUX_I2C_READ ) ) {
		tx[3] = (UB)( n - 1 );
		ntx = 4;
	}
	if ( ( cmd & AUX_I2C_READ ) == 0 && n > 0 ) {
		knl_memcpy(tx + 4, data, n);
		ntx += n;
	}
	if ( n == 0 ) ntx = 3;			/* an address only request: the start or the stop */
	for ( k = 0; k < 32; k++ ) {
		got = knl_igfx_aux_xfer(ch, tx, ntx, r, sizeof(r));
		if ( got < E_OK ) return got;
		if ( ( r[0] & AUX_REPLY_MASK ) == AUX_DEFER || ( r[0] & AUX_I2C_REPLY_MASK ) == AUX_I2C_DEFER ) {
			WaitUsec(500);
			continue;
		}
		if ( ( r[0] & ( AUX_REPLY_MASK | AUX_I2C_REPLY_MASK ) ) != AUX_ACK ) return E_IO;
		got--;
		if ( got > rmax ) got = rmax;
		if ( got > 0 && rx != NULL ) knl_memcpy(rx, r + 1, got);
		return got;
	}
	return E_TMOUT;
}

/* len bytes from offset off of the I2C device at addr (segment seg for the EDID's later blocks) */
LOCAL INT i2c_read_aux( INT ch, UB addr, INT seg, UB off, UB *buf, INT len )
{
	INT	got = 0, n, tries = 0;

	if ( seg > 0 ) {
		UB	s = (UB)seg;

		if ( i2c_aux(ch, AUX_I2C_WRITE | AUX_I2C_MOT, DDC_SEGMENT, &s, 1, NULL, 0) < E_OK ) return E_IO;
	}
	if ( i2c_aux(ch, AUX_I2C_WRITE | AUX_I2C_MOT, addr, &off, 1, NULL, 0) < E_OK ) {
		(void)i2c_aux(ch, AUX_I2C_READ, addr, NULL, 0, NULL, 0);
		return E_IO;
	}
	while ( got < len && tries++ < 64 ) {
		n = i2c_aux(ch, AUX_I2C_READ | AUX_I2C_MOT, addr, NULL, ( len - got > 16 ) ? 16 : len - got,
			    buf + got, len - got);
		if ( n < E_OK ) break;
		got += n;
	}
	(void)i2c_aux(ch, AUX_I2C_READ, addr, NULL, 0, NULL, 0);	/* the stop */
	return ( got == len ) ? got : E_IO;
}

/*
 * The EDID through I2C over AUX, every extension its base block names
 * (two blocks a segment of E-DDC): how many bytes, the blocks in *p_e
 * (Kfree it); 0 when there is none, E_NOMEM
 */
LOCAL INT edid_aux( INT ch, UB **p_e )
{
	UB	b0[DISP_EDID_BLOCK], *e;
	INT	b, nb;

	*p_e = NULL;
	if ( i2c_read_aux(ch, DDC_EDID, 0, 0, b0, DISP_EDID_BLOCK) < E_OK ) return 0;
	nb = 1 + b0[126];
	e = (UB *)Kmalloc((SZ)( DISP_EDID_BLOCK * nb ));
	if ( e == NULL ) return E_NOMEM;
	knl_memcpy(e, b0, DISP_EDID_BLOCK);
	for ( b = 1; b < nb; b++ ) {
		if ( i2c_read_aux(ch, DDC_EDID, b / 2, (UB)( ( b % 2 ) * 128 ), e + DISP_EDID_BLOCK * b,
				  DISP_EDID_BLOCK) < E_OK ) break;
	}
	*p_e = e;
	return DISP_EDID_BLOCK * b;
}

/* ---------------------------------------------------------------- GMBUS */

/* len bytes from offset idx of the I2C device at addr on pin's GMBUS (an index cycle): TRUE when they came */
LOCAL BOOL gmbus_read( INT pin, UB addr, UB idx, UB *e, INT len )
{
	INT	got = 0, i, t;
	UW	st = 0, w;

	ex_wr(GMBUS0, (UW)pin);
	ex_wr(GMBUS1, GM_SW_RDY | GM_CYCLE_WAIT | GM_CYCLE_STOP | ( 1U << 26 ) | ( (UW)len << 16 ) | ( (UW)idx << 8 )
	      | ( (UW)addr << 1 ) | GM_READ);
	while ( got < len ) {
		for ( t = 0; t < 500; t++ ) {
			st = ex_rd(GMBUS2);
			if ( st & ( GM_HW_RDY | GM_SATOER ) ) break;
			WaitUsec(100);
		}
		if ( ( st & GM_HW_RDY ) == 0 || ( st & GM_SATOER ) != 0 ) {
			ex_wr(GMBUS1, 1U << 31);
			ex_wr(GMBUS1, 0);
			ex_wr(GMBUS0, 0);
			return FALSE;
		}
		w = ex_rd(GMBUS3);
		for ( i = 0; i < 4 && got < len; i++ ) {
			e[got++] = (UB)( w >> ( 8 * i ) );
		}
	}
	ex_wr(GMBUS0, 0);
	return TRUE;
}

LOCAL BOOL gmbus_write2( INT pin, UB addr, UB reg, UB val );

/* ---------------------------------------------------------------- a port read */

LOCAL void bytes_say( CONST char *what, INT port, UW addr, CONST UB *b, INT n )
{
	char	t[120];
	INT	i, k;

	k = tm_sprintf((UB *)t, (CONST UB *)"ext: port %c: %s (%04x):", 'A' + port, what, addr);
	for ( i = 0; i < n && i < 16; i++ ) k += tm_sprintf((UB *)t + k, (CONST UB *)" %02x", b[i]);
	tm_sprintf((UB *)t + k, (CONST UB *)"\n");
	tm_printf((UB *)"%s", t);
}

LOCAL void edid_say( INT port, CONST UB *e, INT len, CONST char *how )
{
	INT	i, k, sum = 0;
	char	name[16];

	for ( i = 0; i < 128; i++ ) sum += e[i];
	name[0] = '\0';
	for ( i = 54; i < 126; i += 18 ) {
		if ( e[i] == 0 && e[i + 1] == 0 && e[i + 3] == 0xfc ) {
			for ( k = 0; k < 13 && e[i + 5 + k] != '\n'; k++ ) name[k] = (char)e[i + 5 + k];
			name[k] = '\0';
		}
	}
	tm_printf((UB *)"ext: port %c: EDID by %s, %d bytes, sum %s, \"%s\", maker %02x%02x product %02x%02x, "
		  "preferred %d x %d at %d kHz pixel clock, %d extension(s)\n", 'A' + port, how, len,
		  ( sum & 0xff ) == 0 ? "good" : "BAD", name, e[8], e[9], e[11], e[10],
		  e[56] | ( ( e[58] & 0xf0 ) << 4 ), e[59] | ( ( e[61] & 0xf0 ) << 4 ),
		  ( e[54] | ( e[55] << 8 ) ) * 10, e[126]);
}

/*
 * What is plugged into a port, read: the DPCD's receiver capabilities
 * (000h-00Fh), the link's state (200h-205h) and, for a branch device,
 * its downstream ports (080h-08Fh), then the EDID through I2C over AUX;
 * where AUX does not answer and the port is DP++ or HDMI, a dual mode
 * adaptor's identifier (I2C 40h) and the EDID over GMBUS. The caller
 * holds the wells.
 */
LOCAL void port_read( EXTPORT *x )
{
	UB	b[16], *e = NULL, *was;
	INT	n, len = 0;

	n = knl_igfx_dpcd_read(x->aux, 0x000, b, 16);
	x->dp = (BOOL)( n >= 8 );
	knl_memset(x->caps, 0, sizeof(x->caps));
	if ( n >= 8 ) {
		knl_memcpy(x->caps, b, n);
		bytes_say("DPCD receiver capabilities", x->port, 0x000, b, n);
		if ( b[5] & 0x01 ) {		/* DOWNSTREAMPORT_PRESENT: a branch device */
			UB	d[16];
			INT	m = knl_igfx_dpcd_read(x->aux, 0x080, d, 16);

			if ( m > 0 ) bytes_say("DPCD downstream ports", x->port, 0x080, d, m);
		}
		n = knl_igfx_dpcd_read(x->aux, 0x200, b, 6);
		if ( n > 0 ) bytes_say("DPCD link state", x->port, 0x200, b, n);
		len = edid_aux(x->aux, &e);
		if ( len == E_NOMEM ) {
			tm_printf((UB *)"ext: port %c: no room for its EDID\n", 'A' + x->port);
		} else if ( len >= 128 ) {
			edid_say(x->port, e, len, "AUX");
		} else {
			tm_printf((UB *)"ext: port %c: DisplayPort, no EDID through AUX\n", 'A' + x->port);
		}
	} else if ( x->kind != KIND_DP ) {
		UB	ad[32];
		CONST char *id = "DP-HDMI ADAPTOR";
		INT	k;

		x->adaptor_type = 1;			/* a type 1 adaptor need not answer */
		x->adaptor_tmds = 0;
		if ( gmbus_read(x->ddc, DDC_ADAPTOR, 0x00, ad, 32) ) {
			bytes_say("dual mode adaptor", x->port, 0x00, ad, 16);
			bytes_say("dual mode adaptor", x->port, 0x10, ad + 16, 16);
			for ( k = 0; id[k] != 0 && ad[k] == (UB)id[k]; k++ ) ;
			if ( id[k] == 0 && ad[0x10] == 0xa0 ) {
				x->adaptor_type = 2;
				x->adaptor_tmds = (UW)ad[0x1d] * 2500;
			}
		}
		tm_printf((UB *)"ext: port %c: DP++ adaptor of type %d, TMDS up to %d kHz\n", 'A' + x->port, x->adaptor_type,
			  (INT)( ( x->adaptor_type == 2 && x->adaptor_tmds != 0 ) ? x->adaptor_tmds : 165000 ));
		/* GMBUS reads the first segment of E-DDC: the base block and one extension */
		e = (UB *)Kmalloc(2 * DISP_EDID_BLOCK);
		if ( e == NULL ) {
			tm_printf((UB *)"ext: port %c: no room for its EDID\n", 'A' + x->port);
		} else if ( gmbus_read(x->ddc, DDC_EDID, 0x00, e, 2 * DISP_EDID_BLOCK) ) {
			len = 128 * ( 1 + ( e[126] > 0 ) );
			edid_say(x->port, e, len, "GMBUS");
		} else {
			tm_printf((UB *)"ext: port %c: something is plugged in, neither AUX nor GMBUS answers\n", 'A' + x->port);
		}
	} else {
		tm_printf((UB *)"ext: port %c: something is plugged in, AUX does not answer\n", 'A' + x->port);
	}
	if ( len < 128 && e != NULL ) {
		Kfree(e);
		e = NULL;
	}
	tk_loc_mtx(ext_mtx, TMO_FEVR);
	was = x->edid;
	x->edid = e;
	x->edid_len = ( e != NULL ) ? len : 0;
	tk_unl_mtx(ext_mtx);
	if ( was != NULL ) Kfree(was);
}

/* A port just plugged in, read with the wells it needs, and let go of them */
LOCAL void port_look( EXTPORT *x )
{
	knl_igfx_psr_hold(TRUE, "ext: a port looked at");
	ex_wr(PWR_WELL_CTL2, ex_rd(PWR_WELL_CTL2) | PW_DDI_IO(x->port));
	tk_dly_tsk(20);
	port_read(x);
	ex_wr(PWR_WELL_CTL2, ex_rd(PWR_WELL_CTL2) & ~PW_DDI_IO(x->port));
	knl_igfx_psr_hold(FALSE, "ext: a port looked at");
}

/* ---------------------------------------------------------------- the ports as outputs */

LOCAL INT out_detect( void *ctx )
{
	return ( (EXTPORT *)ctx )->plugged ? DISP_ST_ON : DISP_ST_OFF;
}

LOCAL INT out_edid( void *ctx, UB *buf, INT max )
{
	EXTPORT	*x = (EXTPORT *)ctx;
	INT	n;

	tk_loc_mtx(ext_mtx, TMO_FEVR);
	n = ( x->edid_len < max ) ? x->edid_len : max;
	if ( n > 0 ) knl_memcpy(buf, x->edid, n);
	tk_unl_mtx(ext_mtx);
	return n;
}

/* ---------------------------------------------------------------- the link, worked out (no hardware) */

/*
 * The DisplayPort link's rates by their symbol clock (kHz): RBR, HBR,
 * HBR2; their codes in the DPCD (LINK_BW_SET) and in DPLL_CTRL1 (half
 * the symbol clock in 100 kHz: 810, 1350, 2700, as 2, 1, 0)
 */
LOCAL CONST UW	dp_rates[3] = { 162000, 270000, 540000 };
LOCAL CONST UB	dp_bw_code[3] = { 0x06, 0x0a, 0x14 };
LOCAL CONST UW	dp_dpll_code[3] = { 2, 1, 0 };

LOCAL INT rate_index( UW khz )
{
	INT	i;

	for ( i = 0; i < 3 && dp_rates[i] != khz; i++ ) ;
	return ( i < 3 ) ? i : -1;
}

/* The highest rate a receiver takes, from its DPCD 001h (MAX_LINK_RATE): 0 none known */
LOCAL UW rate_of_code( UB code )
{
	return ( code >= 0x14 ) ? 540000 : ( code >= 0x0a ) ? 270000 : ( code >= 0x06 ) ? 162000 : 0;
}

/*
 * Whether a mode of pixel_khz at bpp bits a pixel fits a link of lanes
 * lanes at link_khz: the mode's bytes a second, rounded up, against the
 * link's (8b/10b: a symbol carries a byte), as i915's intel_dp_link_required
 * and intel_dp_max_data_rate
 */
EXPORT BOOL knl_dp_link_fits( UW pixel_khz, UINT bpp, UW link_khz, INT lanes )
{
	UD	need = ( (UD)pixel_khz * bpp + 7 ) / 8;

	return (BOOL)( need <= (UD)link_khz * (UD)lanes );
}

/*
 * The least link a mode fits, the rate first and then the lanes (1, 2,
 * 4), up to what the receiver takes, as i915's
 * intel_dp_compute_link_config_wide: FALSE when none does
 */
EXPORT BOOL knl_dp_link_choose( UW pixel_khz, UINT bpp, UW max_khz, INT max_lanes, UW *p_khz, INT *p_lanes )
{
	INT	r, l;

	for ( r = 0; r < 3 && dp_rates[r] <= max_khz; r++ ) {
		for ( l = 1; l <= max_lanes && l <= 4; l <<= 1 ) {
			if ( knl_dp_link_fits(pixel_khz, bpp, dp_rates[r], l) ) {
				*p_khz = dp_rates[r];
				*p_lanes = l;
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* m/n with n a power of two up to 0x800000, both brought within 24 bits, as i915's compute_m_n */
LOCAL void m_n_of( UD m, UD n, UW *p_m, UW *p_n )
{
	UD	nn = 1, mm;

	while ( nn < n && nn < 0x800000ULL ) nn <<= 1;
	if ( nn > 0x800000ULL ) nn = 0x800000ULL;
	mm = m * nn / n;
	while ( mm > 0xffffffULL || nn > 0xffffffULL ) {
		mm >>= 1;
		nn >>= 1;
	}
	*p_m = (UW)mm;
	*p_n = (UW)nn;
}

/*
 * The data and link M/N of a mode on a link, as i915's
 * intel_link_compute_m_n (TU 64, the N not held constant): data M/N the
 * mode's bits a second over the link's, link M/N the pixel clock over the
 * link's symbol clock
 */
EXPORT void knl_dp_m_n( UINT bpp, INT lanes, UW pixel_khz, UW link_khz, UW *p_dm, UW *p_dn, UW *p_lm, UW *p_ln )
{
	m_n_of((UD)bpp * pixel_khz, (UD)link_khz * lanes * 8, p_dm, p_dn);
	m_n_of((UD)pixel_khz, (UD)link_khz, p_lm, p_ln);
}

/*
 * The link to try after training failed, as i915's
 * intel_dp_get_link_train_fallback_values: the next lower rate with the
 * same lanes; at the lowest rate, the receiver's highest rate with half
 * the lanes; FALSE after one lane at the lowest rate
 */
EXPORT BOOL knl_dp_fallback( UW *p_khz, INT *p_lanes, UW max_khz )
{
	INT	i = rate_index(*p_khz);

	if ( i > 0 ) {
		*p_khz = dp_rates[i - 1];
		return TRUE;
	}
	if ( *p_lanes > 1 ) {
		*p_khz = max_khz;
		*p_lanes >>= 1;
		return TRUE;
	}
	return FALSE;
}

/*
 * Pipe A's part of the display buffer when pipe B is on beside it: the
 * buffer shared by the pipes' widths, pipe A first, as i915's
 * skl_ddb_get_pipe_allocation_limits
 */
EXPORT UW knl_igfx_split( UINT wa, UINT wb )
{
	if ( wa + wb == 0 ) return IGFX_DBUF_BLOCKS;
	return (UW)( (UD)IGFX_DBUF_BLOCKS * wa / ( wa + wb ) );
}

/* ---------------------------------------------------------------- the registers of a lit port */

#define R_DDI_BUF_TRANS_LO(p, i)	( 0x64e00 + 0x60 * (p) + 8 * (i) )
#define R_DDI_BUF_TRANS_HI(p, i)	( 0x64e04 + 0x60 * (p) + 8 * (i) )
#define R_DISPIO_CR_TX_BMU_CR0	0x6c00c
#define R_DP_TP_STATUS(port)	( 0x64044 + 0x100 * (port) )
#define R_PIPEMISC(p)		( 0x70030 + 0x1000 * (p) )
#define R_PS_CTRL(p, s)		( 0x68180 + 0x800 * (p) + 0x100 * (s) )
#define R_PS_WIN_POS(p, s)	( 0x68170 + 0x800 * (p) + 0x100 * (s) )
#define R_PS_WIN_SZ(p, s)	( 0x68174 + 0x800 * (p) + 0x100 * (s) )
#define R_PS_VPHASE(p, s)	( 0x68188 + 0x800 * (p) + 0x100 * (s) )
#define R_PS_HPHASE(p, s)	( 0x68194 + 0x800 * (p) + 0x100 * (s) )
#define PS_SCALER_EN		0x80000000U
#define PS_SCALER_MODE_DYN	0		/* SKL: either scaler of a pipe, for a plane or the pipe */
#define PS_FILTER_MEDIUM	0
#define PS_PHASE_TRIP		0x0001U
#define R_PLANE_WM(p, l)	( 0x70240 + 0x1000 * (p) + 4 * (l) )
#define R_PLANE_WM_TRANS(p)	( 0x70268 + 0x1000 * (p) )
#define R_PLANE_BUF_CFG(p)	( 0x7027c + 0x1000 * (p) )
#define R_CUR_CTL(p)		( 0x70080 + 0x1000 * (p) )
#define R_CUR_BASE(p)		( 0x70084 + 0x1000 * (p) )
#define R_CUR_WM(p, l)		( 0x70140 + 0x1000 * (p) + 4 * (l) )
#define R_CUR_WM_TRANS(p)	( 0x70168 + 0x1000 * (p) )
#define R_CUR_BUF_CFG(p)	( 0x7017c + 0x1000 * (p) )
#define R_FRMCOUNT(p)		( 0x70040 + 0x1000 * (p) )

#define DDI_BUF_ENABLE		0x80000000U
#define DDI_BUF_IS_IDLE		0x00000080U
#define DP_TP_ENABLE		0x80000000U
#define DP_TP_ENHANCED		0x00040000U
#define DP_TP_PAT(n)		( (UW)(n) << 8 )	/* 0 TPS1, 1 TPS2, 2 idle, 3 normal, 4 TPS3 */
#define DP_TP_IDLE_DONE		0x02000000U
#define TRANS_DDI_ENABLE	0x80000000U
#define TRANS_DDI_MODE_DP_SST	( 2U << 24 )
#define TRANS_DDI_MODE_HDMI	( 0U << 24 )
#define TRANS_DDI_MODE_DVI	( 1U << 24 )
#define TRANS_DDI_PVSYNC	0x00020000U
#define TRANS_DDI_PHSYNC	0x00010000U
#define PIPECONF_ENABLE		0x80000000U
#define PIPECONF_STATE		0x40000000U
#define PLANE_ON_XRGB		0x84000000U	/* enabled, XRGB8888, linear */
#define LCPLL_ENABLE		0x80000000U
#define DPLL_LOCK(id)		( 1U << ( 8 * (id) ) )

#define PIPE_B			1

/*
 * The 9th generation's voltage swing and pre-emphasis for DisplayPort on
 * the Kaby Lake and Amber Lake Y parts (DDI_BUF_TRANS's pairs and the
 * balance leg's I_boost), the table i915 keeps as kbl_y DP; the level a
 * swing and a pre-emphasis select, as its intel_ddi_dp_level (swing 0:
 * 0-3, swing 1: 4-6, swing 2: 7-8). The VBT's own table is not asked for
 * (use_vbt_vswing 0 on the machines read).
 */
typedef struct {
	UW	lo, hi;
	UB	iboost;
} BUFTRANS;

LOCAL CONST BUFTRANS	trans_dp[9] = {
	{ 0x00001017, 0x000000a1, 0 }, { 0x00005012, 0x00000088, 0 }, { 0x80007011, 0x000000cd, 3 },
	{ 0x8000800f, 0x000000c0, 3 }, { 0x00001017, 0x0000009d, 0 }, { 0x80005012, 0x000000c0, 3 },
	{ 0x80007011, 0x000000c0, 3 }, { 0x00001017, 0x0000004c, 0 }, { 0x80005012, 0x000000c0, 3 },
};

#define SWING_MAX		2
LOCAL CONST UB	level_of[3][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 0xff }, { 7, 8, 0xff, 0xff } };
LOCAL CONST UB	preemph_max[3] = { 3, 2, 1 };

/* ---------------------------------------------------------------- the framebuffer of pipe B */

#define EXT_FB_SIZE		( 32 * 1024 * 1024 )	/* two pages of up to 2560 x 1600 */
#define EXT_FB_BLOCK_ORDER	10			/* 4MB at a time */
#define EXT_FB_BLOCKS		( EXT_FB_SIZE / ( 4096 << EXT_FB_BLOCK_ORDER ) )
#define EXT_PAGE_ALIGN		( 256 * 1024 )

LOCAL UD	fb_ggtt = 0;
LOCAL UB	*fb_ap = NULL;
LOCAL UD	fb_ram[EXT_FB_BLOCKS];
LOCAL UD	fb_page = 0;			/* bytes of a page, aligned */

/* The GGTT range of pipe B's pages and their memory, taken once and kept */
LOCAL ER fb_take( void )
{
	UINT	k, per = 1U << EXT_FB_BLOCK_ORDER;
	ER	er;

	if ( fb_ap != NULL ) return E_OK;
	er = knl_igt_ggtt_alloc(EXT_FB_SIZE, EXT_PAGE_ALIGN, IGT_GGTT_MAPPABLE, IGT_GGTT_DISPLAY, "display ext", &fb_ggtt);
	if ( er < E_OK ) {
		tm_printf((UB *)"ext: no room in the GGTT for the external screen (%d)\n", (INT)er);
		return er;
	}
	for ( k = 0; k < EXT_FB_BLOCKS; k++ ) {
		PFRAME	*pf = knl_alloc_pages(EXT_FB_BLOCK_ORDER, ZONE_NORMAL, 0);

		if ( pf == NULL ) pf = knl_alloc_pages(EXT_FB_BLOCK_ORDER, ZONE_DMA32, 0);
		if ( pf == NULL ) {
			tm_printf((UB *)"ext: no memory for the external screen\n");
			return E_NOMEM;
		}
		fb_ram[k] = knl_pf_to_pa(pf);
		knl_igt_ggtt_set(fb_ggtt + (UD)k * per * 4096, &fb_ram[k], per, TRUE);
	}
	fb_ap = (UB *)knl_vmap_pa(knl_igfx_gmadr() + fb_ggtt, EXT_FB_SIZE / 4096, VMAP_NOCACHE);
	if ( fb_ap == NULL ) {
		tm_printf((UB *)"ext: the external screen's pages could not be mapped\n");
		return E_NOMEM;
	}
	knl_memset(fb_ap, 0, EXT_FB_SIZE);
	tm_printf((UB *)"ext: the external screen's pages at GGTT %lx, 32MB of system memory\n", fb_ggtt);
	return E_OK;
}

/* The page shown: its address written to the plane, taken at the next vertical blank */
LOCAL ER b_flip( UINT page )
{
	ex_wr(R_PLANE(PIPE_B, 0x1c), (UW)( fb_ggtt + (UD)page * fb_page ));
	(void)ex_rd(R_PLANE(PIPE_B, 0x1c));
	return E_OK;
}

/* ---------------------------------------------------------------- link training */

/* A train set byte of a swing and a pre-emphasis, the maximum marks set where reached */
LOCAL UB train_byte( INT sw, INT pe )
{
	return (UB)( sw | ( ( sw >= SWING_MAX ) ? 0x04 : 0 ) | ( pe << 3 ) | ( ( pe >= preemph_max[sw] ) ? 0x20 : 0 ) );
}

/* The port's buffer at the level of sw and pe, and its balance leg */
LOCAL void buf_level( EXTPORT *x, INT sw, INT pe, INT lanes )
{
	UW	level = level_of[sw][pe], cr0;
	UB	ib = trans_dp[level].iboost;

	cr0 = ex_rd(R_DISPIO_CR_TX_BMU_CR0);
	cr0 &= ~( ( 7U << ( 8 + 3 * x->port ) ) | ( 1U << ( 23 + x->port ) ) );
	cr0 |= ( ib != 0 ) ? ( (UW)ib << ( 8 + 3 * x->port ) ) : ( 1U << ( 23 + x->port ) );
	ex_wr(R_DISPIO_CR_TX_BMU_CR0, cr0);
	ex_wr(R_DDI_BUF_CTL(x->port), DDI_BUF_ENABLE | ( level << 24 ) | ( (UW)( lanes - 1 ) << 1 ));
}

/* The lane status and the adjust request (DPCD 202h-207h): FALSE when not read */
LOCAL BOOL lane_status( EXTPORT *x, UB *st )
{
	return (BOOL)( knl_igfx_dpcd_read(x->aux, 0x202, st, 6) == 6 );
}

LOCAL BOOL lanes_have( CONST UB *st, INT lanes, UB bits )
{
	INT	l;

	for ( l = 0; l < lanes; l++ ) {
		if ( ( ( st[l / 2] >> ( 4 * ( l % 2 ) ) ) & bits ) != bits ) return FALSE;
	}
	return TRUE;
}

/* The swing and pre-emphasis asked for: the most of any lane, kept within what the port gives */
LOCAL void adjust( CONST UB *st, INT lanes, INT *p_sw, INT *p_pe )
{
	INT	l, sw = 0, pe = 0;

	for ( l = 0; l < lanes; l++ ) {
		UB	a = (UB)( st[4 + l / 2] >> ( 4 * ( l % 2 ) ) );

		if ( ( a & 3 ) > sw ) sw = a & 3;
		if ( ( ( a >> 2 ) & 3 ) > pe ) pe = ( a >> 2 ) & 3;
	}
	if ( sw > SWING_MAX ) sw = SWING_MAX;
	if ( pe > preemph_max[sw] ) pe = preemph_max[sw];
	*p_sw = sw;
	*p_pe = pe;
}

LOCAL void train_set( EXTPORT *x, INT sw, INT pe, INT lanes )
{
	UB	t[4];
	INT	l;

	buf_level(x, sw, pe, lanes);
	for ( l = 0; l < 4; l++ ) t[l] = train_byte(sw, pe);
	(void)knl_igfx_dpcd_write(x->aux, 0x103, t, lanes);
}

/* The receiver's wait between a pattern and its status: 100 us for the clock recovery, 400 for the rest, or what 00Eh asks */
LOCAL void train_wait( EXTPORT *x, BOOL eq )
{
	UINT	r = x->caps[0x0e] & 0x7f;

	if ( r != 0 ) {
		tk_dly_tsk((RELTIM)( 4 * r ));
	} else {
		WaitUsec(eq ? 400 : 100);
	}
}

/*
 * The link trained at link_khz with lanes lanes, as i915's
 * intel_dp_link_training.c: the clock recovery on TPS1 (the swing and
 * pre-emphasis the receiver asks for, five tries at one swing, given up
 * at the highest swing), then the channel equalization on TPS3 where the
 * receiver takes it or TPS2 (five tries, given up when the clock is
 * lost), then the idle pattern. TRUE when trained.
 */
LOCAL BOOL link_train( EXTPORT *x, UW link_khz, INT lanes )
{
	UB	b[2], st[6], pat = ( x->caps[0x02] & 0x40 ) ? 3 : 2;
	INT	sw = 0, pe = 0, tries, same = 0, last_sw = -1, i = rate_index(link_khz);
	BOOL	enh = (BOOL)( ( x->caps[0x02] & 0x80 ) != 0 );

	b[0] = dp_bw_code[i];
	b[1] = (UB)( lanes | ( enh ? 0x80 : 0 ) );
	if ( knl_igfx_dpcd_write(x->aux, 0x100, b, 2) != 2 ) {
		tm_printf((UB *)"ext: %s: the link's rate and lanes not written\n", x->name);
		return FALSE;
	}
	ex_wr(R_DP_TP_CTL(x->port), DP_TP_ENABLE | ( enh ? DP_TP_ENHANCED : 0 ) | DP_TP_PAT(0));
	buf_level(x, 0, 0, lanes);
	WaitUsec(600);				/* the buffer out of idle */
	b[0] = 0x21;				/* TPS1, scrambling off */
	(void)knl_igfx_dpcd_write(x->aux, 0x102, b, 1);
	train_set(x, 0, 0, lanes);
	for ( tries = 0; tries < 10; tries++ ) {
		train_wait(x, FALSE);
		if ( !lane_status(x, st) ) return FALSE;
		if ( lanes_have(st, lanes, 0x1) ) break;		/* CR_DONE */
		adjust(st, lanes, &sw, &pe);
		if ( sw == last_sw ) {
			if ( ++same >= 5 ) break;
		} else {
			same = 0;
			last_sw = sw;
		}
		train_set(x, sw, pe, lanes);
	}
	if ( !lanes_have(st, lanes, 0x1) ) {
		tm_printf((UB *)"ext: %s: clock recovery failed at %d kHz x %d (status %02x %02x, swing %d pre-emphasis %d)\n",
			  x->name, (INT)link_khz, lanes, st[0], st[1], sw, pe);
		return FALSE;
	}
	ex_wr(R_DP_TP_CTL(x->port), DP_TP_ENABLE | ( enh ? DP_TP_ENHANCED : 0 ) | DP_TP_PAT(( pat == 3 ) ? 4 : 1));
	b[0] = (UB)( pat | 0x20 );
	(void)knl_igfx_dpcd_write(x->aux, 0x102, b, 1);
	for ( tries = 0; tries < 5; tries++ ) {
		train_wait(x, TRUE);
		if ( !lane_status(x, st) ) return FALSE;
		if ( !lanes_have(st, lanes, 0x1) ) break;		/* the clock lost */
		if ( lanes_have(st, lanes, 0x7) && ( st[2] & 0x01 ) ) {	/* CR, EQ, symbols locked; lanes aligned */
			tm_printf((UB *)"ext: %s: link trained at %d kHz x %d, swing %d pre-emphasis %d, TPS%d\n",
				  x->name, (INT)link_khz, lanes, sw, pe, pat);
			ex_wr(R_DP_TP_CTL(x->port), DP_TP_ENABLE | ( enh ? DP_TP_ENHANCED : 0 ) | DP_TP_PAT(2));
			for ( i = 0; i < 20 && ( ex_rd(R_DP_TP_STATUS(x->port)) & DP_TP_IDLE_DONE ) == 0; i++ ) WaitUsec(100);
			return TRUE;
		}
		adjust(st, lanes, &sw, &pe);
		train_set(x, sw, pe, lanes);
	}
	tm_printf((UB *)"ext: %s: channel equalization failed at %d kHz x %d (status %02x %02x %02x)\n",
		  x->name, (INT)link_khz, lanes, st[0], st[1], st[2]);
	return FALSE;
}

/* ---------------------------------------------------------------- pipe B on and off */

/* A DPLL not in use, 2 or 3 (DPLL0 is the core display clock's, DPLL1 the panel's): -1 none */
LOCAL INT dpll_free( void )
{
	if ( ( ex_rd(R_WRPLL_CTL(0)) & LCPLL_ENABLE ) == 0 ) return 2;
	if ( ( ex_rd(R_WRPLL_CTL(1)) & LCPLL_ENABLE ) == 0 ) return 3;
	return -1;
}

/* The core display clock, kHz (CDCLK_CTL's decimal), which a pipe's pixel rate may not pass */
LOCAL UW cdclk_khz( void )
{
	return ( ex_rd(0x46000) & 0x7ff ) * 500 + 1000;
}

/* The DPLL id set to the link's rate, enabled and locked; the port's clock taken from it */
LOCAL ER dpll_on( EXTPORT *x, INT id, UW link_khz )
{
	UW	c1 = ex_rd(R_DPLL_CTRL1), c2;
	INT	i;

	c1 &= ~( 0x3fU << ( 6 * id ) );
	c1 |= ( 1U | ( dp_dpll_code[rate_index(link_khz)] << 1 ) ) << ( 6 * id );	/* override, the rate; no SSC */
	ex_wr(R_DPLL_CTRL1, c1);
	(void)ex_rd(R_DPLL_CTRL1);
	ex_wr(R_WRPLL_CTL(id - 2), ex_rd(R_WRPLL_CTL(id - 2)) | LCPLL_ENABLE);
	for ( i = 0; i < 50 && ( ex_rd(R_DPLL_STATUS) & DPLL_LOCK(id) ) == 0; i++ ) WaitUsec(100);
	if ( ( ex_rd(R_DPLL_STATUS) & DPLL_LOCK(id) ) == 0 ) {
		tm_printf((UB *)"ext: %s: DPLL%d did not lock (status %08x)\n", x->name, id, ex_rd(R_DPLL_STATUS));
		return E_IO;
	}
	c2 = ex_rd(R_DPLL_CTRL2);
	c2 &= ~( ( 3U << ( 3 * x->port + 1 ) ) | ( 1U << ( x->port + 15 ) ) );
	c2 |= ( (UW)id << ( 3 * x->port + 1 ) ) | ( 1U << ( 3 * x->port ) );
	ex_wr(R_DPLL_CTRL2, c2);
	(void)ex_rd(R_DPLL_CTRL2);
	return E_OK;
}

LOCAL void dpll_off( EXTPORT *x, INT id )
{
	ex_wr(R_DPLL_CTRL2, ex_rd(R_DPLL_CTRL2) | ( 1U << ( x->port + 15 ) ));	/* the port's clock off */
	if ( id >= 2 ) ex_wr(R_WRPLL_CTL(id - 2), ex_rd(R_WRPLL_CTL(id - 2)) & ~LCPLL_ENABLE);
}

/* Pipe B, its transcoder and the port off, in the reverse of the order they went on */
LOCAL void pipe_b_off( EXTPORT *x )
{
	INT	i;
	UB	d3 = 2;

	ex_wr(R_PLANE(PIPE_B, 0x00), 0);
	ex_wr(R_PLANE(PIPE_B, 0x1c), 0);
	ex_wr(R_PIPECONF(PIPE_B), ex_rd(R_PIPECONF(PIPE_B)) & ~PIPECONF_ENABLE);
	for ( i = 0; i < 100 && ( ex_rd(R_PIPECONF(PIPE_B)) & PIPECONF_STATE ) != 0; i++ ) tk_dly_tsk(1);
	ex_wr(R_TRANS_DDI(PIPE_B), ex_rd(R_TRANS_DDI(PIPE_B)) & ~TRANS_DDI_ENABLE);
	ex_wr(R_DP_TP_CTL(x->port), 0);
	ex_wr(R_DDI_BUF_CTL(x->port), ex_rd(R_DDI_BUF_CTL(x->port)) & ~DDI_BUF_ENABLE);
	for ( i = 0; i < 20 && ( ex_rd(R_DDI_BUF_CTL(x->port)) & DDI_BUF_IS_IDLE ) == 0; i++ ) WaitUsec(100);
	if ( x->dp ) {
		(void)knl_igfx_dpcd_write(x->aux, 0x600, &d3, 1);	/* the receiver to sleep */
	} else if ( x->adaptor_type == 2 ) {
		(void)gmbus_write2(x->ddc, DDC_ADAPTOR, 0x20, 0x01);	/* the adaptor's TMDS output off */
	}
	ex_wr(R_TRANS_CLK_SEL(PIPE_B), 0);
	dpll_off(x, x->dpll);
	x->dpll = -1;
}

/*
 * The pipe scaler (the panel fitter of Gen9: one of pipe B's two
 * scalers, given to the pipe rather than a plane), as i915's skl_pfit and
 * skl_update_scaler: a framebuffer of src_w x src_h shown in a mode of
 * dst_w x dst_h, in the window knl_disp_fit gives (ratio kept, in the
 * middle; the pipe shows black round it). Gen9's limits: the source and
 * the window 8 to 4096 each way (SKL_MIN_SRC_W, SKL_MAX_SRC_W, ...
 * SKL_MAX_DST_H), a downscale of less than 3 each way (the scaler's
 * 16.16 ratio below 0x30000), and the pipe's pixel rate as the scaler
 * reads it -- the mode's clock times the downscale each way, as
 * skl_scaler's adjusted pixel rate -- within CDCLK (one pixel a clock).
 * E_OK with that rate (the watermarks are reckoned with it) into
 * *p_khz; E_NOSPT past a limit.
 */
EXPORT ER knl_skl_scaler_check( UINT src_w, UINT src_h, UINT dst_w, UINT dst_h, UW mode_khz, UW cdclk,
				UW *p_khz )
{
	T_DISPRECT	r;
	UINT		ww, wh;
	UD		khz;

	if ( src_w < 8 || src_h < 8 || src_w > 4096 || src_h > 4096 || dst_w < 8 || dst_h < 8 || dst_w > 4096 || dst_h > 4096 ) {
		return E_NOSPT;
	}
	knl_disp_fit(src_w, src_h, dst_w, dst_h, &r);
	ww = (UINT)( r.right - r.left );
	wh = (UINT)( r.bottom - r.top );
	if ( ww < 8 || wh < 8 ) return E_NOSPT;
	if ( src_w >= 3 * ww || src_h >= 3 * wh ) return E_NOSPT;
	khz = mode_khz;
	if ( src_w > ww ) khz = khz * src_w / ww;
	if ( src_h > wh ) khz = khz * src_h / wh;
	if ( khz > cdclk ) return E_NOSPT;
	if ( p_khz != NULL ) *p_khz = (UW)khz;
	return E_OK;
}

/*
 * The scaler's initial phase for a ratio (16.16) on an RGB picture, as
 * i915's skl_scaler_calc_phase: half a source pixel less half a
 * destination one, kept within the -0.5 to 1.5 the hardware takes, the
 * trip bit set for one at or past 0
 */
EXPORT UW knl_skl_scaler_phase( UW scale )
{
	INT	phase = -0x8000 + (INT)( scale / 2 );
	UW	trip = 0;

	if ( phase > 0x18000 ) phase = 0x18000;
	if ( phase < 0 ) {
		phase = 0x10000 + phase;
	} else {
		trip = PS_PHASE_TRIP;
	}
	return ( ( (UW)phase >> 2 ) & 0xfffeU ) | trip;
}

/* The size of framebuffer pipe B shows in m: the one fit asked for, else the mode's */
#define SRC_W(x, m)	( ( (x)->src_w != 0 ) ? (x)->src_w : (UINT)(m)->hdisp )
#define SRC_H(x, m)	( ( (x)->src_h != 0 ) ? (x)->src_h : (UINT)(m)->vdisp )

/*
 * Pipe B's source size and its scaler: the framebuffer's size as the
 * pipe's source; the pipe scaler (scaler 0) on with its window where the
 * framebuffer is of another size than the mode, off otherwise, as
 * skl_pfit_enable writes it. Scaler 1 is not used.
 */
LOCAL void pipe_b_scaler( EXTPORT *x, CONST T_DISPMODE *m )
{
	UINT		sw = SRC_W(x, m), sh = SRC_H(x, m);
	T_DISPRECT	r;
	UW		hs, vs;

	ex_wr(R_TRANS(PIPE_B, 0x1c), ( (UW)( sw - 1 ) << 16 ) | ( sh - 1 ));	/* PIPESRC */
	ex_wr(R_PS_CTRL(PIPE_B, 1), 0);
	if ( sw == m->hdisp && sh == m->vdisp ) {
		ex_wr(R_PS_CTRL(PIPE_B, 0), 0);			/* no scaler, no panel fitter */
		ex_wr(R_PS_WIN_POS(PIPE_B, 0), 0);
		ex_wr(R_PS_WIN_SZ(PIPE_B, 0), 0);
		return;
	}
	knl_disp_fit(sw, sh, m->hdisp, m->vdisp, &r);
	hs = (UW)( ( (UD)sw << 16 ) / (UD)( r.right - r.left ) );
	vs = (UW)( ( (UD)sh << 16 ) / (UD)( r.bottom - r.top ) );
	ex_wr(R_PS_CTRL(PIPE_B, 0), PS_SCALER_EN | PS_FILTER_MEDIUM | PS_SCALER_MODE_DYN);
	ex_wr(R_PS_VPHASE(PIPE_B, 0), knl_skl_scaler_phase(vs));	/* the Y phase 0, the RGB phase */
	ex_wr(R_PS_HPHASE(PIPE_B, 0), knl_skl_scaler_phase(hs));
	ex_wr(R_PS_WIN_POS(PIPE_B, 0), ( (UW)r.left << 16 ) | (UW)r.top);
	ex_wr(R_PS_WIN_SZ(PIPE_B, 0), ( (UW)( r.right - r.left ) << 16 ) | (UW)( r.bottom - r.top ));
	tm_printf((UB *)"ext: %s: pipe scaler on: %d x %d into %d x %d at %d, %d of %d x %d\n", x->name, (INT)sw, (INT)sh,
		  r.right - r.left, r.bottom - r.top, r.left, r.top, (INT)m->hdisp, (INT)m->vdisp);
}

/* The watermarks and shares of pipe B's plane and cursor written, from its part of the buffer */
LOCAL BOOL pipe_b_wm( EXTPORT *x, CONST T_DISPMODE *m, UW start )
{
	UH	lat[IGFX_WM_LEVELS];
	T_IGWM	o;
	INT	l;

	UW	khz = m->clock_khz;

	if ( !knl_igfx_latencies(lat) ) return FALSE;
	/* the plane's width, and the pixel rate the scaler reads it at where it shrinks the picture */
	if ( x->src_w != 0 && knl_skl_scaler_check(SRC_W(x, m), SRC_H(x, m), m->hdisp, m->vdisp, m->clock_khz, 0xffffffffU,
						    &khz) < E_OK ) return FALSE;
	if ( !knl_igfx_wm_calc(lat, SRC_W(x, m), m->htotal, khz, start, IGFX_DBUF_BLOCKS, 2, &o) ) return FALSE;
	for ( l = 0; l < IGFX_WM_LEVELS; l++ ) {
		ex_wr(R_PLANE_WM(PIPE_B, l), o.wm[l]);
		ex_wr(R_CUR_WM(PIPE_B, l), 0);
	}
	ex_wr(R_PLANE_WM_TRANS(PIPE_B), 0);
	ex_wr(R_CUR_WM_TRANS(PIPE_B), 0);
	ex_wr(R_PLANE_BUF_CFG(PIPE_B), o.buf_cfg);
	ex_wr(R_CUR_BUF_CFG(PIPE_B), o.cur_buf);
	tm_printf((UB *)"ext: %s: pipe B's part of the display buffer %d to %d: plane %08x cursor %08x, WM %08x %08x %08x %08x %08x %08x %08x %08x\n",
		  x->name, (INT)start, IGFX_DBUF_BLOCKS, o.buf_cfg, o.cur_buf, o.wm[0], o.wm[1], o.wm[2], o.wm[3],
		  o.wm[4], o.wm[5], o.wm[6], o.wm[7]);
	return TRUE;
}

/*
 * Pipe B lit on the port in mode m over DisplayPort, as i915's
 * hsw_crtc_enable with intel_ddi_pre_enable_dp: the DPLL, the port's
 * clock and the transcoder's, the receiver woken, the buffer's levels,
 * the link trained (and tried again lower as it fails), the M/N and the
 * timing, the transcoder's DDI function, the pipe, the plane on its page,
 * and the link's normal pattern. The caller holds the panel's self
 * refresh off and pipe A's part of the buffer made room. E_OK, or the
 * error with everything put back off.
 */
LOCAL ER pipe_b_on( EXTPORT *x, CONST T_DISPMODE *m, UW start, UINT pitch )
{
	UW	max_khz = rate_of_code(x->caps[1]), khz, dm, dn, lm, ln, ddi;
	INT	max_lanes = x->caps[2] & 0x1f, lanes, id, i;
	UB	d0 = 1, b0 = 0;
	BOOL	trained = FALSE;

	if ( !knl_dp_link_choose(m->clock_khz, 24, max_khz, max_lanes, &khz, &lanes) ) return E_NOSPT;
	id = dpll_free();
	if ( id < 0 ) return E_LIMIT;
	(void)knl_igfx_dpcd_write(x->aux, 0x600, &d0, 1);	/* the receiver woken */
	tk_dly_tsk(1);
	for ( ;; ) {
		if ( dpll_on(x, id, khz) < E_OK ) {
			dpll_off(x, id);
			return E_IO;
		}
		x->dpll = id;
		ex_wr(R_TRANS_CLK_SEL(PIPE_B), (UW)( x->port + 1 ) << 29);
		for ( i = 0; i < 9; i++ ) {
			ex_wr(R_DDI_BUF_TRANS_LO(x->port, i), trans_dp[i].lo);
			ex_wr(R_DDI_BUF_TRANS_HI(x->port, i), trans_dp[i].hi);
		}
		if ( link_train(x, khz, lanes) ) {
			trained = TRUE;
			break;
		}
		pipe_b_off(x);
		if ( !knl_dp_fallback(&khz, &lanes, max_khz) || !knl_dp_link_fits(m->clock_khz, 24, khz, lanes) ) break;
		tm_printf((UB *)"ext: %s: trying again at %d kHz x %d\n", x->name, (INT)khz, lanes);
		(void)knl_igfx_dpcd_write(x->aux, 0x600, &d0, 1);
	}
	if ( !trained ) {
		b0 = 0;
		(void)knl_igfx_dpcd_write(x->aux, 0x102, &b0, 1);
		return E_IO;
	}
	knl_dp_m_n(24, lanes, m->clock_khz, khz, &dm, &dn, &lm, &ln);
	ex_wr(R_TRANS(PIPE_B, 0x30), ( 63U << 25 ) | dm);	/* TU 64 */
	ex_wr(R_TRANS(PIPE_B, 0x34), dn);
	ex_wr(R_TRANS(PIPE_B, 0x40), lm);
	ex_wr(R_TRANS(PIPE_B, 0x44), ln);
	ex_wr(R_TRANS(PIPE_B, 0x00), ( (UW)( m->htotal - 1 ) << 16 ) | ( m->hdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x04), ( (UW)( m->htotal - 1 ) << 16 ) | ( m->hdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x08), ( (UW)( m->hse - 1 ) << 16 ) | ( m->hss - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x0c), ( (UW)( m->vtotal - 1 ) << 16 ) | ( m->vdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x10), ( (UW)( m->vtotal - 1 ) << 16 ) | ( m->vdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x14), ( (UW)( m->vse - 1 ) << 16 ) | ( m->vss - 1 ));
	pipe_b_scaler(x, m);
	ex_wr(R_PIPEMISC(PIPE_B), 0);				/* 8 bits a colour, no dithering */
	ddi = TRANS_DDI_ENABLE | ( (UW)x->port << 28 ) | TRANS_DDI_MODE_DP_SST | ( (UW)( lanes - 1 ) << 1 )
	    | ( ( m->flags & DISP_MODE_PVSYNC ) ? TRANS_DDI_PVSYNC : 0 ) | ( ( m->flags & DISP_MODE_PHSYNC ) ? TRANS_DDI_PHSYNC : 0 );
	ex_wr(R_TRANS_DDI(PIPE_B), ddi);
	ex_wr(R_PIPECONF(PIPE_B), PIPECONF_ENABLE);
	for ( i = 0; i < 100 && ( ex_rd(R_PIPECONF(PIPE_B)) & PIPECONF_STATE ) == 0; i++ ) tk_dly_tsk(1);
	if ( ( ex_rd(R_PIPECONF(PIPE_B)) & PIPECONF_STATE ) == 0 ) {
		tm_printf((UB *)"ext: %s: pipe B did not come on\n", x->name);
		pipe_b_off(x);
		return E_IO;
	}
	if ( !pipe_b_wm(x, m, start) ) {
		pipe_b_off(x);
		return E_NOSPT;
	}
	ex_wr(R_CUR_CTL(PIPE_B), 0);
	ex_wr(R_CUR_BASE(PIPE_B), 0);
	ex_wr(R_PLANE(PIPE_B, 0x08), pitch / 64);		/* the stride in 64 bytes */
	ex_wr(R_PLANE(PIPE_B, 0x0c), 0);
	ex_wr(R_PLANE(PIPE_B, 0x10), ( (UW)( SRC_H(x, m) - 1 ) << 16 ) | ( SRC_W(x, m) - 1 ));
	ex_wr(R_PLANE(PIPE_B, 0x24), 0);
	ex_wr(R_PLANE(PIPE_B, 0x00), PLANE_ON_XRGB);
	ex_wr(R_PLANE(PIPE_B, 0x1c), (UW)fb_ggtt);
	/* the link's normal pattern, training over (intel_dp_stop_link_train) */
	ex_wr(R_DP_TP_CTL(x->port), ( ex_rd(R_DP_TP_CTL(x->port)) & ~DP_TP_PAT(7) ) | DP_TP_PAT(3));
	(void)knl_igfx_dpcd_write(x->aux, 0x102, &b0, 1);
	x->link_khz = khz;
	x->lanes = lanes;
	tm_printf((UB *)"ext: %s: pipe B on: %d x %d at %d kHz, link %d kHz x %d, M/N %08x/%08x %08x/%08x, DPLL%d, frame counter %d\n",
		  x->name, (INT)m->hdisp, (INT)m->vdisp, (INT)m->clock_khz, (INT)khz, lanes, dm, dn, lm, ln, x->dpll,
		  (INT)ex_rd(R_FRMCOUNT(PIPE_B)));
	return E_OK;
}

/* ---------------------------------------------------------------- HDMI (TMDS) on a DP++ port */

/*
 * The DPLL's dividers for a TMDS clock, as i915's skl_ddi_calculate_wrpll:
 * the AFE clock five times the TMDS clock; the DCO at one of 8400, 9000
 * and 9600 MHz within +1% or -6%, the divider of least deviation (an even
 * one when there is any); the divider split into P, Q and K; the DCO's
 * integer and fraction (of 24 MHz, the fraction in 1/0x8000). DPLL_CFGCR1
 * and DPLL_CFGCR2 into *p_c1, *p_c2; FALSE when no divider fits.
 */
EXPORT BOOL knl_skl_wrpll( UW tmds_khz, UW *p_c1, UW *p_c2 )
{
	CONST UD	central[3] = { 8400000000ULL, 9000000000ULL, 9600000000ULL };
	CONST UB	even[36] = { 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54,
				     56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90, 92, 96, 98 };
	CONST UB	odd[7] = { 3, 5, 7, 9, 15, 21, 35 };
	UD		afe = (UD)tmds_khz * 1000 * 5, best_dev = ~0ULL, best_central = 0, dco, dev;
	UINT		best_p = 0, p, p0 = 0, p1 = 0, p2 = 0, half, d, c, i, n;
	UW		cf, pdiv, kdiv, dint, dfrac;

	for ( d = 0; d < 2; d++ ) {
		n = ( d == 0 ) ? 36 : 7;
		for ( c = 0; c < 3; c++ ) {
			for ( i = 0; i < n; i++ ) {
				p = ( d == 0 ) ? even[i] : odd[i];
				dco = p * afe;
				dev = 10000ULL * ( ( dco > central[c] ) ? dco - central[c] : central[c] - dco ) / central[c];
				if ( ( dco >= central[c] ) ? dev < 100 : dev < 600 ) {
					if ( dev < best_dev ) {
						best_dev = dev;
						best_central = central[c];
						best_p = p;
					}
				}
				if ( best_dev == 0 ) break;
			}
			if ( best_dev == 0 ) break;
		}
		if ( d == 0 && best_p != 0 ) break;	/* an even divider found is kept */
	}
	if ( best_p == 0 ) return FALSE;
	p = best_p;
	if ( p % 2 == 0 ) {
		half = p / 2;
		if ( half == 1 || half == 2 || half == 3 || half == 5 ) { p0 = 2; p1 = 1; p2 = half; }
		else if ( half % 2 == 0 ) { p0 = 2; p1 = half / 2; p2 = 2; }
		else if ( half % 3 == 0 ) { p0 = 3; p1 = half / 3; p2 = 2; }
		else if ( half % 7 == 0 ) { p0 = 7; p1 = half / 7; p2 = 2; }
	} else if ( p == 3 || p == 9 ) { p0 = 3; p1 = 1; p2 = p / 3; }
	else if ( p == 5 || p == 7 ) { p0 = p; p1 = 1; p2 = 1; }
	else if ( p == 15 ) { p0 = 3; p1 = 1; p2 = 5; }
	else if ( p == 21 ) { p0 = 7; p1 = 1; p2 = 3; }
	else if ( p == 35 ) { p0 = 7; p1 = 1; p2 = 5; }
	if ( p0 == 0 ) return FALSE;
	cf = ( best_central == 9600000000ULL ) ? 0 : ( best_central == 9000000000ULL ) ? 1 : 3;
	pdiv = ( p0 == 1 ) ? 0 : ( p0 == 2 ) ? 1 : ( p0 == 3 ) ? 2 : 4;
	kdiv = ( p2 == 5 ) ? 0 : ( p2 == 2 ) ? 1 : ( p2 == 3 ) ? 2 : 3;
	dco = (UD)p0 * p1 * p2 * afe;
	dint = (UW)( dco / 24000000ULL );
	dfrac = (UW)( ( dco / 24 - (UD)dint * 1000000ULL ) * 0x8000 / 1000000ULL );
	*p_c1 = 0x80000000U | ( dfrac << 9 ) | dint;
	*p_c2 = ( (UW)p1 << 8 ) | ( ( p1 == 1 ) ? 0 : 0x80U ) | ( kdiv << 5 ) | ( pdiv << 2 ) | cf;
	return TRUE;
}

/*
 * The Skylake and Kaby Lake Y parts' HDMI buffer levels (DDI_BUF_TRANS
 * and I_boost), the default the ninth; the level the VBT's
 * hdmi_level_shifter names is written to the port's tenth entry, which
 * HDMI uses
 */
LOCAL CONST BUFTRANS	trans_hdmi[11] = {
	{ 0x00000018, 0x000000a1, 0 }, { 0x00005012, 0x000000df, 0 }, { 0x80007011, 0x000000cb, 3 },
	{ 0x00000018, 0x000000a4, 0 }, { 0x00000018, 0x0000009d, 0 }, { 0x00004013, 0x00000080, 0 },
	{ 0x80006013, 0x000000c0, 3 }, { 0x00000018, 0x0000008a, 0 }, { 0x80003015, 0x000000c0, 3 },
	{ 0x80003015, 0x000000c0, 3 }, { 0x80000018, 0x000000c0, 3 },
};
#define HDMI_LEVEL_DEFAULT	8
#define HDMI_TMDS_MAX		300000		/* the 9th generation's own HDMI, kHz */
#define DP_DUAL_TYPE1_MAX	165000		/* a type 1 adaptor's TMDS clock, kHz */

/* Two bytes written to the I2C device at addr on pin's GMBUS (a register and its value): TRUE when written */
LOCAL BOOL gmbus_write2( INT pin, UB addr, UB reg, UB val )
{
	INT	t;
	UW	st = 0;

	ex_wr(GMBUS0, (UW)pin);
	ex_wr(GMBUS3, (UW)reg | ( (UW)val << 8 ));
	ex_wr(GMBUS1, GM_SW_RDY | GM_CYCLE_WAIT | GM_CYCLE_STOP | ( 2U << 16 ) | ( (UW)addr << 1 ));
	for ( t = 0; t < 500; t++ ) {
		st = ex_rd(GMBUS2);
		if ( ( st & GM_SATOER ) != 0 || ( st & ( 1U << 14 ) ) != 0 ) break;	/* NAK, or the wait phase: done */
		WaitUsec(100);
	}
	ex_wr(GMBUS0, 0);
	return (BOOL)( ( st & GM_SATOER ) == 0 && t < 500 );
}

/* An HDMI port's TMDS clock limit, from what a DP++ adaptor says of itself */
LOCAL UW tmds_max( EXTPORT *x )
{
	UW	lim = HDMI_TMDS_MAX;

	if ( x->kind == KIND_DPPP ) {
		UW	a = ( x->adaptor_type == 2 && x->adaptor_tmds != 0 ) ? x->adaptor_tmds : DP_DUAL_TYPE1_MAX;

		if ( a < lim ) lim = a;
	}
	return lim;
}

/* The DPLL id set to the TMDS clock in HDMI mode, enabled and locked; the port's clock taken from it */
LOCAL ER dpll_on_hdmi( EXTPORT *x, INT id, UW tmds_khz )
{
	UW	c1, c2, cfg1, cfg2;
	INT	i;

	if ( !knl_skl_wrpll(tmds_khz, &cfg1, &cfg2) ) return E_NOSPT;
	c1 = ex_rd(R_DPLL_CTRL1);
	c1 &= ~( 0x3fU << ( 6 * id ) );
	c1 |= ( 1U | 0x20U ) << ( 6 * id );			/* override, HDMI mode */
	ex_wr(R_DPLL_CTRL1, c1);
	ex_wr(R_DPLL_CFGCR1(id - 1), cfg1);
	ex_wr(R_DPLL_CFGCR2(id - 1), cfg2);
	(void)ex_rd(R_DPLL_CFGCR2(id - 1));
	ex_wr(R_WRPLL_CTL(id - 2), ex_rd(R_WRPLL_CTL(id - 2)) | LCPLL_ENABLE);
	for ( i = 0; i < 50 && ( ex_rd(R_DPLL_STATUS) & DPLL_LOCK(id) ) == 0; i++ ) WaitUsec(100);
	if ( ( ex_rd(R_DPLL_STATUS) & DPLL_LOCK(id) ) == 0 ) {
		tm_printf((UB *)"ext: %s: DPLL%d did not lock for HDMI (status %08x)\n", x->name, id, ex_rd(R_DPLL_STATUS));
		return E_IO;
	}
	c2 = ex_rd(R_DPLL_CTRL2);
	c2 &= ~( ( 3U << ( 3 * x->port + 1 ) ) | ( 1U << ( x->port + 15 ) ) );
	c2 |= ( (UW)id << ( 3 * x->port + 1 ) ) | ( 1U << ( 3 * x->port ) );
	ex_wr(R_DPLL_CTRL2, c2);
	tm_printf((UB *)"ext: %s: DPLL%d for TMDS %d kHz: CFGCR1 %08x CFGCR2 %08x\n", x->name, id, (INT)tmds_khz, cfg1, cfg2);
	return E_OK;
}

/*
 * Pipe B lit on the port in mode m over TMDS (a DP++ adaptor), as i915's
 * intel_ddi_pre_enable_hdmi and intel_enable_ddi_hdmi: the DPLL in HDMI
 * mode, the port's clock and the transcoder's, the buffer's HDMI level and
 * I_boost, a type 2 adaptor's TMDS output on, the timing, the
 * transcoder's DDI function in DVI mode (no data islands: neither
 * InfoFrames nor sound yet), the pipe, the plane, then the port's buffer
 * on. The 9th generation drives no scrambling: up to 300 MHz.
 */
LOCAL ER pipe_b_on_hdmi( EXTPORT *x, CONST T_DISPMODE *m, UW start, UINT pitch )
{
	INT	id = dpll_free(), i;
	UW	level = ( x->hdmi_level < 11 ) ? x->hdmi_level : HDMI_LEVEL_DEFAULT, cr0;
	UB	ib;

	if ( id < 0 ) return E_LIMIT;
	if ( dpll_on_hdmi(x, id, m->clock_khz) < E_OK ) {
		dpll_off(x, id);
		return E_IO;
	}
	x->dpll = id;
	ex_wr(R_TRANS_CLK_SEL(PIPE_B), (UW)( x->port + 1 ) << 29);
	ex_wr(R_DDI_BUF_TRANS_LO(x->port, 9), trans_hdmi[level].lo);
	ex_wr(R_DDI_BUF_TRANS_HI(x->port, 9), trans_hdmi[level].hi);
	ib = trans_hdmi[level].iboost;
	cr0 = ex_rd(R_DISPIO_CR_TX_BMU_CR0);
	cr0 &= ~( ( 7U << ( 8 + 3 * x->port ) ) | ( 1U << ( 23 + x->port ) ) );
	cr0 |= ( ib != 0 ) ? ( (UW)ib << ( 8 + 3 * x->port ) ) : ( 1U << ( 23 + x->port ) );
	ex_wr(R_DISPIO_CR_TX_BMU_CR0, cr0);
	if ( x->adaptor_type == 2 && !gmbus_write2(x->ddc, DDC_ADAPTOR, 0x20, 0x00) ) {
		tm_printf((UB *)"ext: %s: the adaptor's TMDS output not turned on\n", x->name);
	}
	ex_wr(R_TRANS(PIPE_B, 0x00), ( (UW)( m->htotal - 1 ) << 16 ) | ( m->hdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x04), ( (UW)( m->htotal - 1 ) << 16 ) | ( m->hdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x08), ( (UW)( m->hse - 1 ) << 16 ) | ( m->hss - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x0c), ( (UW)( m->vtotal - 1 ) << 16 ) | ( m->vdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x10), ( (UW)( m->vtotal - 1 ) << 16 ) | ( m->vdisp - 1 ));
	ex_wr(R_TRANS(PIPE_B, 0x14), ( (UW)( m->vse - 1 ) << 16 ) | ( m->vss - 1 ));
	pipe_b_scaler(x, m);
	ex_wr(R_PIPEMISC(PIPE_B), 0);
	ex_wr(R_TRANS_DDI(PIPE_B), TRANS_DDI_ENABLE | ( (UW)x->port << 28 ) | TRANS_DDI_MODE_DVI
	      | ( ( m->flags & DISP_MODE_PVSYNC ) ? TRANS_DDI_PVSYNC : 0 ) | ( ( m->flags & DISP_MODE_PHSYNC ) ? TRANS_DDI_PHSYNC : 0 ));
	ex_wr(R_PIPECONF(PIPE_B), PIPECONF_ENABLE);
	for ( i = 0; i < 100 && ( ex_rd(R_PIPECONF(PIPE_B)) & PIPECONF_STATE ) == 0; i++ ) tk_dly_tsk(1);
	if ( ( ex_rd(R_PIPECONF(PIPE_B)) & PIPECONF_STATE ) == 0 || !pipe_b_wm(x, m, start) ) {
		tm_printf((UB *)"ext: %s: pipe B did not come on for HDMI\n", x->name);
		pipe_b_off(x);
		return E_IO;
	}
	ex_wr(R_CUR_CTL(PIPE_B), 0);
	ex_wr(R_CUR_BASE(PIPE_B), 0);
	ex_wr(R_PLANE(PIPE_B, 0x08), pitch / 64);
	ex_wr(R_PLANE(PIPE_B, 0x0c), 0);
	ex_wr(R_PLANE(PIPE_B, 0x10), ( (UW)( SRC_H(x, m) - 1 ) << 16 ) | ( SRC_W(x, m) - 1 ));
	ex_wr(R_PLANE(PIPE_B, 0x24), 0);
	ex_wr(R_PLANE(PIPE_B, 0x00), PLANE_ON_XRGB);
	ex_wr(R_PLANE(PIPE_B, 0x1c), (UW)fb_ggtt);
	ex_wr(R_DDI_BUF_CTL(x->port), DDI_BUF_ENABLE);		/* the port's buffer on last */
	tm_printf((UB *)"ext: %s: pipe B on over TMDS (DVI): %d x %d at %d kHz, level %d, DPLL%d\n", x->name,
		  (INT)m->hdisp, (INT)m->vdisp, (INT)m->clock_khz, (INT)level, x->dpll);
	return E_OK;
}

/* ---------------------------------------------------------------- the ports as outputs, lit */

LOCAL EXTPORT	*ext_lit = NULL;		/* the port pipe B drives, NULL none */
LOCAL UW	ext_a_end = 0;

/* Whether mode m can be lit on the port: the link takes it, a DPLL is free, the core clock and the pages hold it */
LOCAL ER mode_check( EXTPORT *x, CONST T_DISPMODE *m )
{
	UW	khz;
	INT	lanes;

	if ( !x->plugged ) return E_NOSPT;
	if ( m->flags & DISP_MODE_INTERLACE ) return E_NOSPT;
	if ( m->hdisp > 2560 || m->vdisp > 1600 || m->hdisp < 640 || m->vdisp < 480 ) return E_NOSPT;
	if ( m->clock_khz > cdclk_khz() ) return E_NOSPT;
	if ( x->dp ) {
		if ( !knl_dp_link_choose(m->clock_khz, 24, rate_of_code(x->caps[1]), x->caps[2] & 0x1f, &khz, &lanes) ) return E_NOSPT;
	} else {
		/* TMDS through a DP++ adaptor (or an HDMI port): within the port's and the adaptor's clock, no scrambling */
		if ( x->kind == KIND_DP || x->edid_len == 0 ) return E_NOSPT;
		if ( m->clock_khz > tmds_max(x) ) return E_NOSPT;
	}
	if ( ext_lit != NULL && ext_lit != x ) return E_LIMIT;	/* one pipe beside the panel */
	if ( ext_lit == NULL && dpll_free() < 0 ) return E_LIMIT;
	return E_OK;
}

LOCAL ER out_check( void *ctx, CONST T_DISPMODE *m )
{
	return mode_check((EXTPORT *)ctx, m);
}

/*
 * A framebuffer of src_w x src_h shown in m by the pipe scaler (the main
 * screen mirrored in a mode of another size): whether it can be
 * (knl_skl_scaler_check, an interlaced mode not, the pages within the
 * GGTT range), and with set, made so at the next commit of m
 */
LOCAL ER out_fit( void *ctx, CONST T_DISPMODE *m, UINT src_w, UINT src_h, BOOL set )
{
	EXTPORT	*x = (EXTPORT *)ctx;
	BOOL	same;
	ER	er;

	if ( m == NULL ) return E_PAR;
	same = (BOOL)( src_w == m->hdisp && src_h == m->vdisp );
	if ( !same ) {
		if ( m->flags & DISP_MODE_INTERLACE ) return E_NOSPT;
		er = knl_skl_scaler_check(src_w, src_h, m->hdisp, m->vdisp, m->clock_khz, cdclk_khz(), NULL);
		if ( er < E_OK ) return er;
		if ( (UD)( ( src_w * 4 + 63 ) & ~63U ) * src_h > EXT_FB_SIZE ) return E_NOSPT;
	}
	if ( set ) {
		x->src_w = same ? 0 : src_w;
		x->src_h = same ? 0 : src_h;
	}
	return E_OK;
}

/* The port lit in m with its framebuffer described into hw, or off (m NULL); the panel kept as it was when it fails */
LOCAL ER out_commit( void *ctx, CONST T_DISPMODE *m, T_DISPHW *hw )
{
	EXTPORT	*x = (EXTPORT *)ctx;
	UINT	pitch;
	ER	er;

	if ( m == NULL ) {
		if ( ext_lit != x ) return E_OK;
		pipe_b_off(x);
		ex_wr(PWR_WELL_CTL2, ex_rd(PWR_WELL_CTL2) & ~PW_DDI_IO(x->port));
		(void)knl_igfx_pipes(1, IGFX_DBUF_BLOCKS);
		knl_igfx_psr_hold(FALSE, "pipe B");
		ext_lit = NULL;
		tm_printf((UB *)"ext: %s: pipe B off, the panel's buffer and SAGV as before\n", x->name);
		return E_OK;
	}
	if ( ext_lit == x && x->mode.hdisp == m->hdisp && x->mode.vdisp == m->vdisp && x->mode.clock_khz == m->clock_khz
	  && x->hw.width == SRC_W(x, m) && x->hw.height == SRC_H(x, m) ) {
		*hw = x->hw;				/* as it is */
		return E_OK;
	}
	if ( ext_lit == x ) (void)out_commit(ctx, NULL, hw);
	er = mode_check(x, m);
	if ( er < E_OK ) return er;
	er = fb_take();
	if ( er < E_OK ) return er;
	pitch = ( SRC_W(x, m) * 4 + 63 ) & ~63U;		/* the framebuffer's, which the scaler may show larger or smaller */
	fb_page = ( (UD)pitch * SRC_H(x, m) + EXT_PAGE_ALIGN - 1 ) & ~(UD)( EXT_PAGE_ALIGN - 1 );
	knl_igfx_psr_hold(TRUE, "pipe B");			/* PSR and the DC states off while a second pipe is on */
	ext_a_end = knl_igfx_split(knl_igfx_panel_w(), SRC_W(x, m));
	er = knl_igfx_pipes(2, ext_a_end);			/* pipe A's part made smaller first, SAGV off */
	if ( er >= E_OK ) {
		ex_wr(PWR_WELL_CTL2, ex_rd(PWR_WELL_CTL2) | PW_DDI_IO(x->port));
		tk_dly_tsk(2);
		er = x->dp ? pipe_b_on(x, m, ext_a_end, pitch) : pipe_b_on_hdmi(x, m, ext_a_end, pitch);
		if ( er < E_OK ) {
			ex_wr(PWR_WELL_CTL2, ex_rd(PWR_WELL_CTL2) & ~PW_DDI_IO(x->port));
			(void)knl_igfx_pipes(1, IGFX_DBUF_BLOCKS);
		}
	}
	if ( er < E_OK ) {
		knl_igfx_psr_hold(FALSE, "pipe B");
		tm_printf((UB *)"ext: %s: not lit (%d): the panel's buffer, watermarks and SAGV put back\n", x->name, (INT)er);
		return er;
	}
	ext_lit = x;
	x->mode = *m;
	knl_memset(&x->hw, 0, sizeof(x->hw));
	x->hw.width = SRC_W(x, m);
	x->hw.height = SRC_H(x, m);
	x->hw.bpp = 32;
	x->hw.pitch = pitch;
	x->hw.format = DISP_FMT_XRGB8888;
	x->hw.fb_pa = knl_igfx_gmadr() + fb_ggtt;
	x->hw.fb_va = (UBINT)fb_ap;
	x->hw.alpha = DISP_ALPHA_ANY;
	x->hw.via = x->name;
	x->hw.pages = ( 2 * fb_page <= EXT_FB_SIZE ) ? 2 : 1;
	x->hw.page2_va = ( x->hw.pages == 2 ) ? (UBINT)( fb_ap + fb_page ) : 0;
	x->hw.flip = ( x->hw.pages == 2 ) ? b_flip : NULL;
	/* a flip is taken at the next vertical blank: surely within a refresh and a millisecond */
	x->hw.latch_us = (UINT)( (UD)m->htotal * m->vtotal * 1000 / m->clock_khz + 1000 );
	x->hw.out = &x->out;
	*hw = x->hw;
	return E_OK;
}

/* ---------------------------------------------------------------- hot plug interrupts */

#define R_SDEIMR		0xc4004
#define R_SDEIIR		0xc4008
#define R_SDEIER		0xc400c
#define HP_LONG(port)		( 2U << ( 8 * ( (port) - 1 ) ) )
#define HP_SHORT(port)		( 1U << ( 8 * ( (port) - 1 ) ) )
#define HP_STATUS(port)		( 3U << ( 8 * ( (port) - 1 ) ) )

IMPORT void	knl_igt_irq_display( void (*fn)( UW master ) );	/* igt.c */

LOCAL ID	ext_tsk = 0;
LOCAL UW	ext_hp_seen = 0;		/* the hot plug status bits latched by the interrupt, for the task */
LOCAL UW	ext_irqs = 0;

/*
 * The display's interrupt, through the drawing driver's (the master's
 * PCH bit): the PCH's hot plug interrupts acknowledged, their long and
 * short pulses kept for the task and the task woken, as i915's
 * spt_irq_handler
 */
LOCAL void ext_irq( UW master )
{
	UW	iir, hp;

	if ( ( master & ( 1U << 23 ) ) == 0 || ex_mmio == 0 ) return;
	iir = ex_rd(R_SDEIIR);
	if ( iir == 0 ) return;
	ex_wr(R_SDEIIR, iir);
	hp = ex_rd(PCH_PORT_HOTPLUG);
	ex_wr(PCH_PORT_HOTPLUG, hp);		/* the status bits cleared by writing them back */
	ext_hp_seen |= hp;
	ext_irqs++;
	if ( ext_tsk > 0 ) (void)tk_wup_tsk(ext_tsk);
}

LOCAL void irq_start( void )
{
	UW	bits = 0;
	INT	i;

	for ( i = 0; i < ext_nport; i++ ) bits |= SDE_HPD(ext_port[i].port);
	knl_igt_irq_display(ext_irq);
	ex_wr(R_SDEIIR, ex_rd(R_SDEIIR));
	ex_wr(R_SDEIER, ex_rd(R_SDEIER) | bits);
	ex_wr(R_SDEIMR, ex_rd(R_SDEIMR) & ~bits);
	tm_printf((UB *)"ext: hot plug interrupts asked for: SDEIER %08x SDEIMR %08x\n", ex_rd(R_SDEIER), ex_rd(R_SDEIMR));
}

/* ---------------------------------------------------------------- the machine asleep */

LOCAL EXTPORT	*ext_slept = NULL;		/* the port lit before the sleep */
LOCAL T_DISPMODE ext_slept_mode;

/*
 * Before the machine sleeps: pipe B and its port put off, as for an
 * output turned off, the panel's buffer and SAGV as with it alone. The
 * screen it showed is kept, and lit again after the sleep.
 */
EXPORT void knl_disp_ext_suspend( void )
{
	T_DISPHW	hw;

	if ( ex_mmio == 0 || ext_lit == NULL ) return;
	ext_slept = ext_lit;
	ext_slept_mode = ext_lit->mode;
	tm_printf((UB *)"ext: %s: off for the sleep\n", ext_lit->name);
	(void)out_commit(ext_lit, NULL, &hw);
}

/*
 * After the sleep: the hot plug interrupts asked for again (the sleep may
 * have reset them), and the port lit before it lit again in the same mode
 * on the same pages, where it is still plugged in; the task looks at the
 * ports again either way
 */
EXPORT void knl_disp_ext_resume( void )
{
	EXTPORT	*x = ext_slept;
	T_DISPHW hw;
	UW	bits = 0;
	INT	i;
	ER	er;

	if ( ex_mmio == 0 ) return;
	for ( i = 0; i < ext_nport; i++ ) bits |= SDE_HPD(ext_port[i].port);
	if ( bits != 0 ) {
		ex_wr(R_SDEIER, ex_rd(R_SDEIER) | bits);
		ex_wr(R_SDEIMR, ex_rd(R_SDEIMR) & ~bits);
	}
	ext_slept = NULL;
	if ( x != NULL && ( ex_rd(SDEISR) & SDE_HPD(x->port) ) != 0 ) {
		er = out_commit(x, &ext_slept_mode, &hw);
		tm_printf((UB *)"ext: %s: lit again after the sleep (%d)\n", x->name, (INT)er);
	}
	if ( ext_tsk > 0 ) (void)tk_wup_tsk(ext_tsk);
}

/* A short pulse on a lit port (IRQ_HPD): the link looked at, and lit again when it fell, as intel_dp_check_link_status */
LOCAL void short_pulse( EXTPORT *x )
{
	UB	st[6];
	T_DISPHW hw;
	T_DISPMODE m;

	if ( ext_lit != x || !lane_status(x, st) ) return;
	if ( lanes_have(st, x->lanes, 0x7) && ( st[2] & 0x01 ) ) return;
	tm_printf((UB *)"ext: %s: the link fell (status %02x %02x %02x): lit again\n", x->name, st[0], st[1], st[2]);
	m = x->mode;
	(void)out_commit(x, NULL, &hw);
	(void)out_commit(x, &m, &hw);
}

/* The ports with a connector, from the VBT's child devices; B, C and D as DisplayPort when there is no VBT */
LOCAL void ports_find( void )
{
	UB	c[64];
	INT	i, n, ndp = 0, nhdmi = 0;

	for ( i = 0; i < 8 && ext_nport < EXT_PORTS; i++ ) {
		INT	port, aux;
		UW	type;

		n = knl_igfx_vbt_child(i, c, sizeof(c));
		if ( n < 0 ) break;
		if ( n < 26 ) continue;
		type = (UW)( c[2] | ( c[3] << 8 ) );
		/* DVO ports: 1-3 HDMI B-D, 7-9 DP B-D (i915's DVO_PORT_HDMIB.. and DVO_PORT_DPB..) */
		port = ( c[16] >= 1 && c[16] <= 3 ) ? c[16] : ( c[16] >= 7 && c[16] <= 9 ) ? c[16] - 6 : 0;
		if ( port == 0 || ( type & VBT_INTERNAL ) != 0 ) continue;
		aux = ( c[25] == 0x10 ) ? 1 : ( c[25] == 0x20 ) ? 2 : ( c[25] == 0x30 ) ? 3 : ( c[25] == 0x40 ) ? 0 : port;
		ext_port[ext_nport].port = port;
		ext_port[ext_nport].kind = ( type & VBT_DP ) == 0 ? KIND_HDMI
					 : ( ( type & VBT_TMDS ) != 0 && ( type & VBT_NOT_HDMI ) == 0 ) ? KIND_DPPP : KIND_DP;
		ext_port[ext_nport].aux = aux;
		ext_port[ext_nport].ddc = c[19];
		ext_port[ext_nport].hdmi_level = c[7] & 0x1f;
		ext_nport++;
	}
	if ( ext_nport == 0 ) {
		for ( i = 1; i <= 3; i++ ) {
			ext_port[ext_nport].port = i;
			ext_port[ext_nport].kind = KIND_DP;
			ext_port[ext_nport].aux = i;
			ext_port[ext_nport].ddc = ( i == 1 ) ? 5 : ( i == 2 ) ? 4 : 6;
			ext_nport++;
		}
	}
	for ( i = 0; i < ext_nport; i++ ) {
		EXTPORT	*x = &ext_port[i];

		if ( x->kind == KIND_HDMI ) {
			tm_sprintf((UB *)x->name, (CONST UB *)"HDMI-A-%d", ++nhdmi);
		} else {
			tm_sprintf((UB *)x->name, (CONST UB *)"DP-%d", ++ndp);
		}
		x->out.kind = ( x->kind == KIND_HDMI ) ? DISP_CON_HDMI : DISP_CON_DP;
		x->out.name = x->name;
		x->out.detect = out_detect;
		x->out.get_edid = out_edid;
		x->out.check = out_check;
		x->out.commit = out_commit;
		x->out.fit = out_fit;
		x->out.ctx = x;
		x->dpll = -1;
		tm_printf((UB *)"ext: %s: DDI %c, %s, AUX %c, DDC pin %d\n", x->name, 'A' + x->port,
			  ( x->kind == KIND_DP ) ? "DisplayPort" : ( x->kind == KIND_DPPP ) ? "DisplayPort and TMDS (DP++)" : "HDMI",
			  'A' + x->aux, x->ddc);
	}
}

LOCAL void ext_task( INT stacd, void *exinf )
{
	UW	now;
	INT	i;

	(void)stacd;
	(void)exinf;
	/* the main screen is there before the ports are put beside it, as outputs after it */
	for ( i = 0; i < 100; i++ ) {
		T_DISPHW	hw;

		if ( knl_disp_screen_hw(0, &hw, NULL, NULL) >= E_OK ) break;
		tk_dly_tsk(100);
	}
	firmware_say();
	ports_find();
	for ( i = 0; i < ext_nport; i++ ) {
		knl_disp_out_add(&ext_port[i].out);
		ex_wr(PCH_PORT_HOTPLUG, ex_rd(PCH_PORT_HOTPLUG) | HP_ENABLE(ext_port[i].port));
	}
	tm_printf((UB *)"ext: hot plug control %08x, SDEISR %08x\n", ex_rd(PCH_PORT_HOTPLUG), ex_rd(SDEISR));
	irq_start();
	typec_say();
	ucsi_start();
	ucsi_say("at the start");
	for ( ;; ) {
		UW	hp;
		UINT	s;

		DI(s);
		hp = ext_hp_seen;
		ext_hp_seen = 0;
		EI(s);
		now = ex_rd(SDEISR);
		for ( i = 0; i < ext_nport; i++ ) {
			EXTPORT	*x = &ext_port[i];
			BOOL	in = (BOOL)( ( now & SDE_HPD(x->port) ) != 0 );

			if ( in == x->plugged && in && ( hp & HP_LONG(x->port) ) != 0 ) {
				tm_printf((UB *)"ext: %s: a long pulse, still plugged in: read again\n", x->name);
				port_look(x);
				knl_disp_out_hotplug(&x->out);
				continue;
			}
			if ( in == x->plugged ) {
				if ( in && ( hp & HP_SHORT(x->port) ) != 0 ) short_pulse(x);
				continue;
			}
			tm_printf((UB *)"ext: %s (DDI %c) %s (SDEISR %08x)\n", x->name, 'A' + x->port,
				  in ? "plugged in" : "taken out", now);
			if ( in ) {
				tk_dly_tsk(200);		/* the plug settled */
				port_look(x);
				ucsi_say("a port plugged in");
			} else {
				tk_loc_mtx(ext_mtx, TMO_FEVR);
				x->edid_len = 0;
				tk_unl_mtx(ext_mtx);
			}
			x->plugged = in;
			knl_disp_out_hotplug(&x->out);
		}
		(void)tk_slp_tsk(EXT_POLL_MS);		/* woken by the interrupt, or looked at again */
	}
}

/* From the screen's start-up (disp_gop.c), on an Intel display of the 9th generation */
EXPORT void knl_disp_ext_start( void )
{
	T_CTSK	ct;
	T_CMTX	cm;
	ID	t;

	ex_mmio = knl_igfx_mmio();
	if ( ex_mmio == 0 ) return;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	ext_mtx = tk_cre_mtx(&cm);
	if ( ext_mtx <= 0 ) return;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ext_task;
	ct.itskpri = 28;
	ct.stksz = 16384;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) {
		ext_tsk = t;
		(void)tk_sta_tsk(t, 0);
	}
}

#endif /* X64 */
