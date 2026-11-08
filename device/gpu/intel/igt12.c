/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt12.c (x64 PC)
 *	The state of Intel's graphics of the 11th generation and later, as
 *	the firmware left it, told on the console in lines that start
 *	"igt12:" (docs/tessronos/13-gpu.md 13.11.16). No such machine was at
 *	hand when this was written, so these lines are what the next step is
 *	to be decided on. Of those whose register places were not confirmed,
 *	among them the whole 11th generation, only what the PCI configuration
 *	space says is told.
 *
 *	Nothing is written to the device, neither its registers nor its
 *	configuration space. The screen stays the one the UEFI's GOP set up.
 *
 *	What is read, and why reading it is safe:
 *	  - the fuses and the display's registers answer without forcewake;
 *	    a register in a display power well that is off completes and
 *	    reads as zeroes
 *	  - the engines' and the GuC's registers lie in wells of the GT, which
 *	    this does not wake: they are read only when the well's forcewake
 *	    acknowledge says someone holds it awake, and are otherwise told
 *	    as not read
 *	  - nothing is polled: each register is read once
 *
 *	Should a read stop the machine after all, the lines before it are in
 *	KERNEL.TXT: the records are written first, as for the 9th generation's
 *	first start, and each group is announced before it is read.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "igt12.h"

IMPORT void	knl_bootlog_soon( void );	/* bootlog.c */

#define R_GGC		0x108040
#define R_DSMBASE	0x1080c0
#define R_EU_DISABLE	0x09134
#define R_SLICE_EN	0x09138
#define R_DSS_EN	0x0913c
#define R_MEDIA_DISABLE	0x09140
#define R_ACK_RENDER	0x00d84
#define R_ACK_GT	0x130044
#define R_ACK_MEDIA	0x00d88
#define R_RCS_BASE	0x02000
#define R_BCS_BASE	0x22000
#define R_RING_MODE	0x029c
#define R_EXEC_ST_LO	0x0234
#define R_EXEC_ST_HI	0x0238
#define R_GUC_STATUS	0x0c000
#define R_GUC_WOPCM	0x0c340
#define R_PWR_BIOS	0x45400
#define R_PWR_DRIVER	0x45404
#define R_TRANS_CONF(p)	( 0x70008 + (p) * 0x1000 )
#define R_TRANS_DDI(p)	( 0x60400 + (p) * 0x1000 )
#define R_PLANE_CTL(p)	( 0x70180 + (p) * 0x1000 )
#define R_PLANE_SURF(p)	( 0x7019c + (p) * 0x1000 )

#define PCI_COMMAND	0x04
#define CMD_MEMORY	0x0002

Inline UW rd( UBINT mmio, UW off )
{
	return *(volatile UW *)( mmio + off );
}

EXPORT void igt12_read( UBINT mmio, T_IGT12STATE *s )
{
	INT	p;

	s->dead = FALSE;
	s->rcs_read = s->gt_read = FALSE;
	s->slice_en = rd(mmio, R_SLICE_EN);
	if ( s->slice_en == 0xffffffffU ) {
		s->dead = TRUE;
		return;
	}
	s->dss_en = rd(mmio, R_DSS_EN);
	s->eu_dis = rd(mmio, R_EU_DISABLE);
	s->media_dis = rd(mmio, R_MEDIA_DISABLE);
	s->ggc = rd(mmio, R_GGC);
	s->dsm_lo = rd(mmio, R_DSMBASE);
	s->dsm_hi = rd(mmio, R_DSMBASE + 4);

	s->ack_render = rd(mmio, R_ACK_RENDER);
	s->ack_gt = rd(mmio, R_ACK_GT);
	s->ack_media = rd(mmio, R_ACK_MEDIA);
	if ( ( s->ack_render & 0xffff ) != 0 ) {
		s->rcs_read = TRUE;
		s->rcs_mode = rd(mmio, R_RCS_BASE + R_RING_MODE);
		s->rcs_st_lo = rd(mmio, R_RCS_BASE + R_EXEC_ST_LO);
		s->rcs_st_hi = rd(mmio, R_RCS_BASE + R_EXEC_ST_HI);
	}
	if ( ( s->ack_gt & 0xffff ) != 0 ) {
		s->gt_read = TRUE;
		s->bcs_mode = rd(mmio, R_BCS_BASE + R_RING_MODE);
		s->bcs_st_lo = rd(mmio, R_BCS_BASE + R_EXEC_ST_LO);
		s->bcs_st_hi = rd(mmio, R_BCS_BASE + R_EXEC_ST_HI);
		s->guc_status = rd(mmio, R_GUC_STATUS);
		s->guc_wopcm = rd(mmio, R_GUC_WOPCM);
	}

	s->pwr_bios = rd(mmio, R_PWR_BIOS);
	s->pwr_driver = rd(mmio, R_PWR_DRIVER);
	for ( p = 0; p < IGT12_NPIPE; p++ ) {
		s->pipe[p].conf = rd(mmio, R_TRANS_CONF(p));
		s->pipe[p].ddi = rd(mmio, R_TRANS_DDI(p));
		s->pipe[p].plane_ctl = rd(mmio, R_PLANE_CTL(p));
		s->pipe[p].plane_surf = rd(mmio, R_PLANE_SURF(p));
	}
}

LOCAL INT bits( UW v )
{
	INT	n = 0;

	for ( ; v != 0; v &= v - 1 ) n++;
	return n;
}

LOCAL void say_uc( CONST T_IGTGEN *g )
{
	tm_printf((UB *)"igt12: GuC, i915's default: %s\n",
		  g->uc == IGT_UC_NONE ? "not used; execlists from the host" :
		  g->uc == IGT_UC_HUC ? "only to authenticate the HuC; execlists from the host" :
		  "submission through it; execlists from the host are what is to be tried, not confirmed here");
}

LOCAL void say_state( CONST T_IGT12STATE *s )
{
	INT	p;

	tm_printf((UB *)"igt12: GT: slices %02x, DSS %08x (%d), EU disabled %02x; GGC %08x, stolen memory %08x%08x\n",
		  s->slice_en, s->dss_en, bits(s->dss_en), s->eu_dis, s->ggc, s->dsm_hi, s->dsm_lo);
	tm_printf((UB *)"igt12: forcewake acknowledges: render %08x, GT %08x, media %08x\n", s->ack_render, s->ack_gt,
		  s->ack_media);
	tm_printf((UB *)"igt12: engines: render and copy; video decode %02x and enhancement %01x are fused off of 8 and 4\n",
		  s->media_dis & 0xff, ( s->media_dis >> 16 ) & 0xf);
	if ( s->rcs_read ) {
		tm_printf((UB *)"igt12: render: mode %08x, execlist status %08x %08x\n", s->rcs_mode, s->rcs_st_hi,
			  s->rcs_st_lo);
	} else {
		tm_printf((UB *)"igt12: render: asleep, not read\n");
	}
	if ( s->gt_read ) {
		tm_printf((UB *)"igt12: copy: mode %08x, execlist status %08x %08x\n", s->bcs_mode, s->bcs_st_hi,
			  s->bcs_st_lo);
		tm_printf((UB *)"igt12: GuC: status %08x, WOPCM offset %08x\n", s->guc_status, s->guc_wopcm);
	} else {
		tm_printf((UB *)"igt12: copy and GuC: asleep, not read\n");
	}
	tm_printf((UB *)"igt12: display power wells: firmware's %08x, driver's %08x\n", s->pwr_bios, s->pwr_driver);
	for ( p = 0; p < IGT12_NPIPE; p++ ) {
		tm_printf((UB *)"igt12: pipe %c: transcoder %08x (%s), DDI %08x, plane 1 %08x at %08x\n", 'A' + p,
			  s->pipe[p].conf, ( s->pipe[p].conf & 0x40000000U ) ? "on" : "off", s->pipe[p].ddi,
			  s->pipe[p].plane_ctl, s->pipe[p].plane_surf);
	}
}

/*
 * The first BAR is reached only when its size is that of the registers and
 * the table together and the firmware left the device answering on it: the
 * enable is the device's own state, and turning it on would be a write.
 */
LOCAL void look_task( INT stacd, void *exinf )
{
	T_PCIDEV	d = *(T_PCIDEV *)exinf;
	T_IGT12STATE	s;

	(void)stacd;
	tk_dly_tsk(30000);
	knl_bootlog_soon();
	tk_dly_tsk(4000);
	tm_printf((UB *)"igt12: - reading the registers, nothing written\n");
	igt12_read((UBINT)DEV_BASE(d.bar[0]), &s);
	if ( s.dead ) {
		tm_printf((UB *)"igt12: the device reads all ones: nothing more read\n");
	} else {
		say_state(&s);
	}
	tm_printf((UB *)"igt12: done; the screen stays the GOP's\n");
	knl_bootlog_soon();
	tk_ext_tsk();
}

/* Found on the bus, told, and read later by a task of its own: none of it writes to the device */
EXPORT void knl_igt12_look( void )
{
	LOCAL T_PCIDEV	d;			/* the task's, for as long as it runs */
	CONST T_IGTGEN	*g = NULL;
	T_CTSK		ct;
	ID		tid;
	UW		cmd;
	INT		i;

	for ( i = 0; ts_pcie_find(0x03, 0xff, 0xff, i, &d) >= E_OK; i++ ) {
		if ( d.vendor != 0x8086 ) continue;
		g = knl_igt_gen(d.device);
		if ( g != NULL ) break;
	}
	if ( g == NULL ) return;
	cmd = ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_COMMAND, 2);
	tm_printf((UB *)"igt12: %04x:%04x rev %02x at %02x:%02x.%d: %s, graphics %d.%d, %s; command %04x\n", d.vendor,
		  d.device, ts_pcie_cfg_read(d.bus, d.dev, d.fn, 0x08, 1), d.bus, d.dev, d.fn, g->name,
		  g->verx10 / 10, g->verx10 % 10, g->discrete ? "discrete" : "integrated", cmd);
	tm_printf((UB *)"igt12: BAR0 %lx, %ld bytes; BAR2 %lx, %ld bytes\n", (UD)d.bar[0], (D)d.bar_size[0], (UD)d.bar[2],
		  (D)d.bar_size[2]);
	say_uc(g);
#ifdef IGT12
	tm_printf((UB *)"igt12: built with IGT12=1: only the formats of the execlists; the engines are not started\n");
#endif
	if ( g->read == IGT_READ_NO ) {
		tm_printf((UB *)"igt12: its registers are not read: their places are not confirmed for it\n");
		return;
	}
	if ( d.bar[0] == 0 || d.bar_size[0] < 16UL * 1024 * 1024 || ( cmd & CMD_MEMORY ) == 0 ) {
		tm_printf((UB *)"igt12: its registers cannot be reached as they are: not read\n");
		return;
	}
	if ( g->read == IGT_READ_SAME ) {
		tm_printf((UB *)"igt12: registers read at Tiger Lake's places (the same graphics version)\n");
	}
	ct.exinf = &d;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)look_task;
	ct.itskpri = 30;
	ct.stksz = 8192;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		tm_printf((UB *)"igt12: no task to read the registers: not read\n");
	}
}

#endif /* X64 */
