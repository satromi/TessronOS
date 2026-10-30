/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xhci_pc.c (x64 PC)
 *	What a PC's xHCI needs besides the controller of the standard
 *	(device/usb/xhci.c), around knl_xhci_attach in usbman.c's find_pci:
 *
 *	before it	the controller's PCI identity and, on an Intel
 *			chipset, the port routing registers in its
 *			configuration space (0xD0 XUSB2PR, 0xD4 XUSB2PRM,
 *			0xD8 USB3_PSSEN, 0xDC USB3PRM). They hand the ports
 *			between an EHCI and the xHCI and matter only where
 *			the chipset still has an Intel EHCI (up to the 8 and
 *			9 series); then the ports the masks allow are given
 *			to the xHCI. On Skylake and later there is no EHCI,
 *			and the registers are only read and recorded.
 *	after it	on an Intel controller, every isochronous TD raises
 *			its interrupt (no Block Event Interrupt: on these
 *			controllers the events held back by it can come
 *			late). On the Sunrise Point PCH (Skylake, Kaby Lake
 *			and Amber Lake, the X1 Yoga and the Surface Go 2) the
 *			dual role port (a Type-C port that can also be a
 *			device) is switched by the registers of the Intel
 *			vendor capability (192): DUAL_ROLE_CFG0 at 0x68
 *			(bit 20 the ID pin by software, bit 21 the software
 *			ID pin used, bit 24 VBUS valid by software, bit 16
 *			the software switch, bits 0-1 the static role) and
 *			DUAL_ROLE_CFG1 at 0x6C (bit 29 in the host role).
 *			The firmware sets the role; it is recorded, and only
 *			"usbrole=host" in the start-up settings switches the
 *			port to the host role (the ID pin low, no VBUS),
 *			waiting up to a second for CFG1 to say so.
 *
 *	Each step is said on the console (and so in KERNEL.TXT), and the
 *	values are kept for USB.TXT (knl_xhci_pc_regs).
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pcie.h>
#include "sysdepend/sysdepend.h"
#include "../usbdev.h"

IMPORT void  knl_xhci_pc_no_bei( USBHC *hc );
IMPORT UBINT knl_xhci_pc_xcap( USBHC *hc, INT id );

#define VID_INTEL		0x8086

#define CFG_XUSB2PR		0xd0
#define CFG_XUSB2PRM		0xd4
#define CFG_USB3_PSSEN		0xd8
#define CFG_USB3PRM		0xdc

#define XCAP_INTEL		192
#define DRCFG0			0x68
#define DRCFG0_STATIC_MASK	0x00000003
#define DRCFG0_SW_SWITCH	0x00010000
#define DRCFG0_IDPIN		0x00100000
#define DRCFG0_IDPIN_EN		0x00200000
#define DRCFG0_VBUS		0x01000000
#define DRCFG1			0x6c
#define DRCFG1_HOST		0x20000000

/* the record of each controller, by its index among them */
#define PC_MAX			4
typedef struct {
	BOOL	used;
	UH	vendor, device;
	UB	bus, dev, fn;
	BOOL	routing;	/* the routing registers were read */
	UW	xusb2pr, xusb2prm, pssen, usb3prm;
	BOOL	routed;		/* they were written (an EHCI is there) */
	BOOL	dual_role;	/* the Intel capability was found */
	UBINT	drcap;		/* its registers, 0 none */
	UW	cfg0, cfg1;	/* as found */
	UW	cfg0_now, cfg1_now;
	BOOL	switched;
	BOOL	no_bei;
} XPC;

LOCAL XPC	pc_tbl[PC_MAX];
LOCAL XPC	pc_next;	/* filled before the attach, kept after it */

LOCAL UW rd_w( UBINT a )
{
	return *(volatile UW *)a;
}

LOCAL void wr_w( UBINT a, UW v )
{
	*(volatile UW *)a = v;
	DeviceBarrier();
}

/* the Sunrise Point PCH's xHCI: the LP (U and Y processors) and the H */
LOCAL BOOL is_spt( UH device )
{
	return ( device == 0x9d2f || device == 0xa12f );
}

LOCAL BOOL setting( CONST char *w )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; w[k] != '\0' && c[k] == w[k]; k++ ) ;
		if ( w[k] == '\0' && ( c[k] == '\0' || c[k] == ' ' ) ) return TRUE;
	}
	return FALSE;
}

/* Is there an Intel EHCI on the bus? */
LOCAL BOOL intel_ehci( void )
{
	T_PCIDEV	d;
	INT		i;

	for ( i = 0; ts_pcie_find(0x0c, 0x03, 0x20, i, &d) >= E_OK; i++ ) {
		if ( d.vendor == VID_INTEL ) {
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * Before knl_xhci_attach: the identity, and the port routing of an Intel
 * chipset.
 */
EXPORT void knl_xhci_pc_before( CONST T_PCIDEV *d )
{
	XPC	*p = &pc_next;
	UINT	b = d->bus, dv = d->dev, f = d->fn;

	knl_memset(p, 0, sizeof(*p));
	p->used = TRUE;
	p->vendor = d->vendor;
	p->device = d->device;
	p->bus = d->bus; p->dev = d->dev; p->fn = d->fn;
	tm_printf((UB *)"usb: xhci %04x:%04x at %02x:%02x.%d\n", d->vendor, d->device,
		  d->bus, d->dev, d->fn);
	if ( d->vendor != VID_INTEL ) {
		return;
	}
	p->routing = TRUE;
	p->xusb2pr  = ts_pcie_cfg_read(b, dv, f, CFG_XUSB2PR, 4);
	p->xusb2prm = ts_pcie_cfg_read(b, dv, f, CFG_XUSB2PRM, 4);
	p->pssen    = ts_pcie_cfg_read(b, dv, f, CFG_USB3_PSSEN, 4);
	p->usb3prm  = ts_pcie_cfg_read(b, dv, f, CFG_USB3PRM, 4);
	tm_printf((UB *)"usb: xhci intel: XUSB2PR %08x XUSB2PRM %08x USB3_PSSEN %08x USB3PRM %08x\n",
		  p->xusb2pr, p->xusb2prm, p->pssen, p->usb3prm);
	if ( intel_ehci() ) {
		/* the SuperSpeed terminations first, then the USB 2 lines */
		ts_pcie_cfg_write(b, dv, f, CFG_USB3_PSSEN, p->usb3prm, 4);
		ts_pcie_cfg_write(b, dv, f, CFG_XUSB2PR, p->xusb2prm, 4);
		p->routed = TRUE;
		tm_printf((UB *)"usb: xhci intel: ports taken from the EHCI: USB3_PSSEN %08x XUSB2PR %08x\n",
			  ts_pcie_cfg_read(b, dv, f, CFG_USB3_PSSEN, 4),
			  ts_pcie_cfg_read(b, dv, f, CFG_XUSB2PR, 4));
	}
}

/* Bring the dual role port to the host role; TRUE once CFG1 says so */
LOCAL BOOL to_host( UBINT c )
{
	UW	v = rd_w(c + DRCFG0);
	INT	i;

	v &= ~(DRCFG0_IDPIN | DRCFG0_VBUS);
	v |= DRCFG0_IDPIN_EN;
	wr_w(c + DRCFG0, v);
	for ( i = 0; i < 100; i++ ) {
		if ( (rd_w(c + DRCFG1) & DRCFG1_HOST) != 0 ) {
			return TRUE;
		}
		tk_dly_tsk(10);
	}
	return FALSE;
}

/*
 * After knl_xhci_attach succeeded and the controller has its index:
 * the Intel controller's own settings.
 */
EXPORT void knl_xhci_pc_after( USBHC *hc )
{
	XPC	*p = &pc_next;
	UBINT	c;

	if ( hc->index < PC_MAX ) {
		pc_tbl[hc->index] = *p;
		p = &pc_tbl[hc->index];
	}
	if ( p->vendor != VID_INTEL ) {
		return;
	}
	knl_xhci_pc_no_bei(hc);
	p->no_bei = TRUE;
	if ( !is_spt(p->device) ) {
		return;
	}
	c = knl_xhci_pc_xcap(hc, XCAP_INTEL);
	if ( c == 0 ) {
		tm_printf((UB *)"usb: xhci intel: no dual role capability\n");
		return;
	}
	p->dual_role = TRUE;
	p->drcap = c;
	p->cfg0 = p->cfg0_now = rd_w(c + DRCFG0);
	p->cfg1 = p->cfg1_now = rd_w(c + DRCFG1);
	tm_printf((UB *)"usb: xhci intel: dual role CFG0 %08x CFG1 %08x (%s role, %s)\n",
		  p->cfg0, p->cfg1, ( (p->cfg1 & DRCFG1_HOST) != 0 ) ? "host" : "device",
		  ( (p->cfg0 & DRCFG0_SW_SWITCH) != 0 ) ? "switched by software" : "as the firmware set");
	if ( (p->cfg1 & DRCFG1_HOST) == 0 && setting("usbrole=host") ) {
		p->switched = to_host(c);
		p->cfg0_now = rd_w(c + DRCFG0);
		p->cfg1_now = rd_w(c + DRCFG1);
		tm_printf((UB *)"usb: xhci intel: to the host role (usbrole=host): %s, CFG0 %08x CFG1 %08x\n",
			  p->switched ? "done" : "not in a second", p->cfg0_now, p->cfg1_now);
	}
}

/*
 * The dual role registers as they are now and as they were once the
 * start-up was done with them, for the record of a root port let go
 * (xhci.c, knl_xhci_port_say): a role switched away from the host by
 * the firmware disconnects the Type-C port. FALSE when the controller
 * has no such capability.
 */
EXPORT BOOL knl_xhci_pc_role( USBHC *hc, UW *cfg0, UW *cfg1, UW *cfg0_was, UW *cfg1_was, BOOL *host )
{
	XPC	*p;

	if ( hc->index < 0 || hc->index >= PC_MAX ) {
		return FALSE;
	}
	p = &pc_tbl[hc->index];
	if ( !p->used || !p->dual_role || p->drcap == 0 ) {
		return FALSE;
	}
	*cfg0 = rd_w(p->drcap + DRCFG0);
	*cfg1 = rd_w(p->drcap + DRCFG1);
	*cfg0_was = p->cfg0_now;
	*cfg1_was = p->cfg1_now;
	*host = ( (*cfg1 & DRCFG1_HOST) != 0 );
	return TRUE;
}

/*
 * For USB.TXT: what was found of controller hcidx, as words: [0] vendor
 * and device, [1] bus, device and function, [2] flags (1 routing read, 2
 * routed, 4 dual role, 8 switched, 16 no BEI), [3..6] XUSB2PR, XUSB2PRM,
 * USB3_PSSEN, USB3PRM, [7..10] CFG0 and CFG1 as found and now. Answers
 * how many, 0 when nothing is known.
 */
EXPORT INT knl_xhci_pc_regs( INT hcidx, UW *w, INT max )
{
	XPC	*p;

	if ( hcidx < 0 || hcidx >= PC_MAX || !pc_tbl[hcidx].used || max < 11 ) {
		return 0;
	}
	p = &pc_tbl[hcidx];
	w[0] = (UW)p->vendor << 16 | p->device;
	w[1] = (UW)p->bus << 16 | (UW)p->dev << 8 | p->fn;
	w[2] = ( p->routing ? 1 : 0 ) | ( p->routed ? 2 : 0 ) | ( p->dual_role ? 4 : 0 )
	     | ( p->switched ? 8 : 0 ) | ( p->no_bei ? 16 : 0 );
	w[3] = p->xusb2pr; w[4] = p->xusb2prm; w[5] = p->pssen; w[6] = p->usb3prm;
	w[7] = p->cfg0; w[8] = p->cfg1; w[9] = p->cfg0_now; w[10] = p->cfg1_now;
	return 11;
}

#endif /* X64 */
