/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_pch.c (x64 PC)
 *	The GPIO pads of an Intel PCH, driven as outputs (gpio_pch.h): what a
 *	camera's power controller of the discrete kind (device/cam/x86_64/
 *	int3472.c) needs of them, the sensor's reset, its power and its
 *	clock switched by pins of the chipset.
 *
 *	The controller is an ACPI device (INT344B, Sunrise Point-LP, which
 *	the Kaby Lake and Amber Lake notebooks have too) whose _CRS gives the
 *	memory of each community of pads in turn. The pins the namespace
 *	names are the pads counted across the communities, 24 to a group
 *	(GPP_A0 is 0, GPP_B0 24 ...), which on this PCH are the pads'
 *	numbers. In a community:
 *
 *	  0x00C	PADBAR: where the pads' configuration starts
 *	  0x020	PAD_OWN: 4 bits a pad, 0 when the host has it (else the ME)
 *	  0x0A0	PADCFGLOCK and PADCFGLOCKTX, 8 bytes a group: a bit a pad
 *	  0x0D0	HOSTSW_OWN, 4 bytes a group: 1 when the pad is the GPIO
 *		driver's, 0 when ACPI has it
 *	  PADBAR + 8 * pad: DW0 and DW1 of the pad
 *
 *	DW0: bit 0 the output's level (GPIOTXSTATE), bit 1 the input's
 *	(GPIORXSTATE), bit 8 the output off (GPIOTXDIS), bit 9 the input off
 *	(GPIORXDIS), bits 13:10 the function (PMODE, 0 for a GPIO). A pin is
 *	driven by clearing GPIOTXDIS and setting GPIOTXSTATE; the rest of
 *	the pad is left as the firmware set it. A pad of a native function or
 *	the ME's is not touched.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "../../../kernel/sysdepend/acpi/acpi.h"
#include "gpio_pch.h"

#define PADBAR			0x00c
#define PADOWN			0x020
#define PADCFGLOCK		0x0a0
#define HOSTSW_OWN		0x0d0
#define GPP_SIZE		24

#define DW0_TXSTATE		0x00000001
#define DW0_RXSTATE		0x00000002
#define DW0_TXDIS		0x00000100
#define DW0_RXDIS		0x00000200
#define DW0_PMODE(v)		( ( (v) >> 10 ) & 0xf )

#define MAX_COMM		4
#define MAX_CTRL		2

typedef struct {
	UH	first, last;		/* the pins of a community, by their numbers in the namespace */
} COMMUNITY;

typedef struct {
	CONST char	*hid;
	CONST char	*name;
	INT		ncomm;
	COMMUNITY	comm[MAX_COMM];		/* in the order of the memory of _CRS */
} LAYOUT;

LOCAL CONST LAYOUT	layouts[] = {
	{ "INT344B", "Sunrise Point-LP", 3, { { 0, 47 }, { 48, 119 }, { 120, 151 } } },
};

typedef struct {
	void		*dev;
	CONST LAYOUT	*lay;
	UBINT		base[MAX_COMM];
	UW		padbar[MAX_COMM];
} CTRL;

LOCAL CTRL	ctrls[MAX_CTRL];
LOCAL INT	nctrl = 0;
LOCAL T_SPLOCK	gp_lock;
LOCAL BOOL	gp_lock_made = FALSE;

/* The controller of the path, set up the first time it is asked for */
LOCAL CTRL *ctrl_of( CONST char *path )
{
	void	*h = knl_acpica_handle(path);
	CTRL	*c;
	INT	i, k;

	if ( h == NULL ) return NULL;
	for ( i = 0; i < nctrl; i++ ) {
		if ( ctrls[i].dev == h ) return &ctrls[i];
	}
	if ( nctrl >= MAX_CTRL ) return NULL;
	for ( i = 0; i < (INT)( sizeof(layouts) / sizeof(layouts[0]) ); i++ ) {
		for ( k = 0; k < 4; k++ ) {
			void	*d = knl_acpica_device(layouts[i].hid, k);

			if ( d == NULL ) break;
			if ( d != h ) continue;
			c = &ctrls[nctrl];
			knl_memset(c, 0, sizeof(*c));
			c->dev = h;
			c->lay = &layouts[i];
			for ( k = 0; k < c->lay->ncomm; k++ ) {
				unsigned long long	b = 0, len = 0;

				if ( knl_acpica_mem(h, k, &b, &len) != 0 || len < 0x1000 ) {
					tm_printf((UB *)"gpio: %s (%s): no memory for community %d\n", path, c->lay->name, k);
					return NULL;
				}
				c->base[k] = (UBINT)DEV_BASE(b);
				c->padbar[k] = in_w(c->base[k] + PADBAR);
				tm_printf((UB *)"gpio: %s %s community %d at %lx, pads %d-%d, PADBAR %x\n", path, c->lay->hid,
					  k, (UD)b, c->lay->comm[k].first, c->lay->comm[k].last, c->padbar[k]);
			}
			if ( !gp_lock_made ) {
				InitSpinLock(&gp_lock);
				gp_lock_made = TRUE;
			}
			nctrl++;
			return c;
		}
	}
	tm_printf((UB *)"gpio: %s is not a GPIO controller this knows\n", path);
	return NULL;
}

/* The community and the pad in it of a pin: FALSE when there is none */
LOCAL BOOL pad_of( CTRL *c, UINT pin, INT *comm, UINT *pad )
{
	INT	k;

	for ( k = 0; k < c->lay->ncomm; k++ ) {
		if ( pin >= c->lay->comm[k].first && pin <= c->lay->comm[k].last ) {
			*comm = k;
			*pad = pin - c->lay->comm[k].first;
			return TRUE;
		}
	}
	return FALSE;
}

typedef struct {
	UW	dw0, dw1, own, hostsw, lock, locktx;
} PADSTATE;

LOCAL void pad_read( CTRL *c, INT k, UINT pad, PADSTATE *p )
{
	UBINT	b = c->base[k];
	UINT	gpp = pad / GPP_SIZE, bit = pad % GPP_SIZE;

	p->dw0 = in_w(b + c->padbar[k] + pad * 8);
	p->dw1 = in_w(b + c->padbar[k] + pad * 8 + 4);
	p->own = ( in_w(b + PADOWN + ( pad / 8 ) * 4) >> ( ( pad % 8 ) * 4 ) ) & 0xf;
	p->hostsw = ( in_w(b + HOSTSW_OWN + gpp * 4) >> bit ) & 1;
	p->lock = ( in_w(b + PADCFGLOCK + gpp * 8) >> bit ) & 1;
	p->locktx = ( in_w(b + PADCFGLOCK + gpp * 8 + 4) >> bit ) & 1;
}

EXPORT void knl_pchgpio_say( CONST char *ctrl, UINT pin, CONST char *what )
{
	CTRL		*c = ctrl_of(ctrl);
	PADSTATE	p;
	INT		k;
	UINT		pad;

	if ( c == NULL || !pad_of(c, pin, &k, &pad) ) {
		tm_printf((UB *)"gpio: %s: pin %d of %s: no such pad\n", what, pin, ctrl);
		return;
	}
	pad_read(c, k, pad, &p);
	tm_printf((UB *)"gpio: %s: pin %d (community %d pad %d): DW0 %08x DW1 %08x, mode %d, output %s at %d, "
		  "input %d, owner %s, %s, lock %d/%d\n",
		  what, pin, k, pad, p.dw0, p.dw1, DW0_PMODE(p.dw0), ( p.dw0 & DW0_TXDIS ) ? "off" : "on",
		  ( p.dw0 & DW0_TXSTATE ) ? 1 : 0, ( p.dw0 & DW0_RXSTATE ) ? 1 : 0, ( p.own == 0 ) ? "host" : "ME",
		  p.hostsw ? "GPIO driver's" : "ACPI's", p.lock, p.locktx);
}

EXPORT ER knl_pchgpio_out( CONST char *ctrl, UINT pin, BOOL high )
{
	CTRL		*c = ctrl_of(ctrl);
	PADSTATE	p;
	INT		k;
	UINT		pad;
	UBINT		a;
	UW		v;
	UINT		im;

	if ( c == NULL || !pad_of(c, pin, &k, &pad) ) return E_NOEXS;
	pad_read(c, k, pad, &p);
	if ( DW0_PMODE(p.dw0) != 0 || p.own != 0 ) {
		tm_printf((UB *)"gpio: pin %d of %s is not a GPIO of the host's (DW0 %08x, owner %d)\n",
			  pin, ctrl, p.dw0, p.own);
		return E_OBJ;
	}
	if ( p.locktx ) {
		tm_printf((UB *)"gpio: pin %d of %s: its output is locked by the firmware; written all the same\n", pin, ctrl);
	}
	a = c->base[k] + c->padbar[k] + pad * 8;
	ISpinLock(&gp_lock, &im);
	v = in_w(a) & ~(UW)DW0_TXDIS;
	v = high ? ( v | DW0_TXSTATE ) : ( v & ~(UW)DW0_TXSTATE );
	out_w(a, v);
	ISpinUnlock(&gp_lock, &im);
	return E_OK;
}

#endif /* X64 */
