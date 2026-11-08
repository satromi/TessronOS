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
 *	The controller is an ACPI device whose _CRS gives the memory of each
 *	community of pads in turn, and which _HID says the layout of:
 *
 *	  INT344B   Sunrise Point-LP, which the Kaby Lake and Amber Lake
 *		    notebooks have too. Groups of 24, outputs only.
 *	  INTC1055  Alder Lake-P and Raptor Lake-P, whose pads are laid out
 *		    as Tiger Lake-LP's; INT34C5 is Tiger Lake-LP itself. Outputs,
 *		    and inputs that interrupt.
 *
 *	A pin in the namespace is not always a pad's number: each group has a
 *	base of its own in the namespace, with holes between them (GPP_T0 of
 *	Tiger Lake-LP is 32, and the 26th pad of GPP_B leaves 26 to 31 unused).
 *	The table of groups turns one into the other. Groups the namespace
 *	cannot name (HVCMOS, JTAG, SPI) are in the table all the same, for
 *	the registers of a community are counted through them. In a community:
 *
 *	  0x00C	PADBAR: where the pads' configuration starts
 *	  0x020	PAD_OWN: 4 bits a pad, 8 to a register. A group starts a
 *		register of its own, so a group of 26 takes 4
 *	  lock	PADCFGLOCK and PADCFGLOCKTX, 8 bytes a group: a bit a pad
 *	  own	HOSTSW_OWN, 4 bytes a group: 1 when the pad is the GPIO
 *		driver's, 0 when ACPI has it
 *	  0x100	GPI_IS, 4 bytes a group: a pad's interrupt status, written 1 to clear
 *	  0x120	GPI_IE, 4 bytes a group: its enable
 *	  PADBAR + size * pad: the pad's configuration, DW0 first
 *
 *	DW0: bit 0 the output's level (GPIOTXSTATE), bit 1 the input's
 *	(GPIORXSTATE), bit 8 the output off (GPIOTXDIS), bit 9 the input off
 *	(GPIORXDIS), bits 13:10 the function (PMODE, 0 for a GPIO), bit 23
 *	the input inverted (RXINV), bits 26:25 what is an event (RXEVCFG: 0
 *	the level, 1 an edge, 3 both edges). A pin is driven by clearing
 *	GPIOTXDIS and setting GPIOTXSTATE; the rest of the pad is left as the
 *	firmware set it. A pad of a native function or the ME's is not touched.
 *
 *	The pads' interrupts share one line, the Interrupt of the controller's
 *	own _CRS, which is the OR of GPI_IS and GPI_IE over all the groups.
 *	The handler acknowledges each pad and calls who took it. A pad whose
 *	event is its level would interrupt again at once while it stays
 *	asserted, so it is masked as it comes, until its taker has dealt with
 *	the cause. A pad nobody took (the firmware may leave one enabled) is
 *	masked for good, or it would hold the line.
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
#define GPI_IS			0x100
#define GPI_IE			0x120

#define DW0_TXSTATE		0x00000001
#define DW0_RXSTATE		0x00000002
#define DW0_TXDIS		0x00000100
#define DW0_RXDIS		0x00000200
#define DW0_PMODE(v)		( ( (v) >> 10 ) & 0xf )
#define DW0_RXINV		0x00800000
#define DW0_RXEV_EDGE		0x02000000
#define DW0_RXEV_BOTH		0x06000000
#define DW0_RXEV_MASK		0x06000000

#define MAX_COMM		4
#define MAX_CTRL		2
#define MAX_PADINT		4
#define NOMAP			0xffff		/* a group the namespace cannot name */

typedef struct {
	UB	comm;		/* its community: its place in the memory of _CRS */
	UB	bank;		/* its place in the community's registers */
	UH	first, last;	/* its pads, in the numbering of all the pads */
	UH	base;		/* the pin of its first pad in the namespace */
} GROUP;

typedef struct {
	CONST char	*hid;
	CONST char	*name;
	INT		ncomm;
	UH		first[MAX_COMM];	/* the first pad of each community */
	UH		padsize;
	UH		lock, own;		/* the offsets of PADCFGLOCK and HOSTSW_OWN */
	BOOL		has_int;		/* its inputs may interrupt */
	CONST GROUP	*group;
	INT		ngroup;
} LAYOUT;

LOCAL CONST GROUP	spt_lp_group[] = {
	{ 0, 0,   0,  23,   0 },	/* GPP_A */
	{ 0, 1,  24,  47,  24 },	/* GPP_B */
	{ 1, 0,  48,  71,  48 },	/* GPP_C */
	{ 1, 1,  72,  95,  72 },	/* GPP_D */
	{ 1, 2,  96, 119,  96 },	/* GPP_E */
	{ 2, 0, 120, 143, 120 },	/* GPP_F */
	{ 2, 1, 144, 151, 144 },	/* GPP_G */
};

LOCAL CONST GROUP	tgl_lp_group[] = {
	{ 0, 0,   0,  25,   0 },	/* GPP_B */
	{ 0, 1,  26,  41,  32 },	/* GPP_T */
	{ 0, 2,  42,  66,  64 },	/* GPP_A */
	{ 1, 0,  67,  74,  96 },	/* GPP_S */
	{ 1, 1,  75,  98, 128 },	/* GPP_H */
	{ 1, 2,  99, 119, 160 },	/* GPP_D */
	{ 1, 3, 120, 143, 192 },	/* GPP_U */
	{ 1, 4, 144, 170, 224 },	/* vGPIO */
	{ 2, 0, 171, 194, 256 },	/* GPP_C */
	{ 2, 1, 195, 219, 288 },	/* GPP_F */
	{ 2, 2, 220, 225, NOMAP },	/* HVCMOS */
	{ 2, 3, 226, 250, 320 },	/* GPP_E */
	{ 2, 4, 251, 259, NOMAP },	/* JTAG */
	{ 3, 0, 260, 267, 352 },	/* GPP_R */
	{ 3, 1, 268, 276, NOMAP },	/* SPI */
};

#define NITEMS(a)	( (INT)( sizeof(a) / sizeof((a)[0]) ) )

LOCAL CONST LAYOUT	layouts[] = {
	{ "INT344B", "Sunrise Point-LP", 3, { 0, 48, 120 }, 8, 0x0a0, 0x0d0, FALSE, spt_lp_group, NITEMS(spt_lp_group) },
	{ "INTC1055", "Alder Lake-P", 4, { 0, 67, 171, 260 }, 16, 0x080, 0x0b0, TRUE, tgl_lp_group, NITEMS(tgl_lp_group) },
	{ "INT34C5", "Tiger Lake-LP", 4, { 0, 67, 171, 260 }, 16, 0x080, 0x0b0, TRUE, tgl_lp_group, NITEMS(tgl_lp_group) },
};

typedef struct {
	void		*dev;
	CONST LAYOUT	*lay;
	UBINT		base[MAX_COMM];
	UW		padbar[MAX_COMM];
	INT		gsi;			/* the line of its pads' interrupts once taken, else -1 */
} CTRL;

/* A pad taken as an interrupt */
typedef struct pch_padint {
	CTRL		*c;
	CONST GROUP	*g;
	UINT		bit;
	BOOL		level;
	void		(*fn)( void *arg );
	void		*arg;
} PADINT;

LOCAL CTRL	ctrls[MAX_CTRL];
LOCAL INT	nctrl = 0;
LOCAL PADINT	padints[MAX_PADINT];
LOCAL INT	npadint = 0;
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
			c->gsi = -1;
			for ( k = 0; k < c->lay->ncomm; k++ ) {
				unsigned long long	b = 0, len = 0;

				if ( knl_acpica_mem(h, k, &b, &len) != 0 || len < 0x1000 ) {
					tm_printf((UB *)"gpio: %s (%s): no memory for community %d\n", path, c->lay->name, k);
					return NULL;
				}
				c->base[k] = (UBINT)DEV_BASE(b);
				c->padbar[k] = in_w(c->base[k] + PADBAR);
				tm_printf((UB *)"gpio: %s %s (%s) community %d at %lx, first pad %d, PADBAR %x\n", path, c->lay->hid,
					  c->lay->name, k, (UD)b, c->lay->first[k], c->padbar[k]);
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

/* What a pin of the namespace is. A hole between groups, or past the last, is no pad */
typedef struct {
	CONST GROUP	*g;
	INT		comm;
	UINT		pad;		/* its place in the community's pads */
	UINT		bit;		/* its place in the group's registers */
	UINT		ownreg;		/* the register of PAD_OWN, counted through the groups before */
} PADREF;

LOCAL BOOL pad_of( CTRL *c, UINT pin, PADREF *r )
{
	CONST LAYOUT	*l = c->lay;
	INT		i, j;

	for ( i = 0; i < l->ngroup; i++ ) {
		CONST GROUP	*g = &l->group[i];

		if ( g->base == NOMAP || pin < g->base || pin > (UINT)( g->base + g->last - g->first ) ) continue;
		r->g = g;
		r->comm = g->comm;
		r->bit = pin - g->base;
		r->pad = g->first + r->bit - l->first[g->comm];
		r->ownreg = r->bit / 8;
		for ( j = 0; j < i; j++ ) {
			if ( l->group[j].comm == g->comm ) {
				r->ownreg += ( l->group[j].last - l->group[j].first + 1 + 7 ) / 8;
			}
		}
		return TRUE;
	}
	return FALSE;
}

typedef struct {
	UW	dw0, dw1, own, hostsw, lock, locktx;
} PADSTATE;

LOCAL void pad_read( CTRL *c, CONST PADREF *r, PADSTATE *p )
{
	CONST LAYOUT	*l = c->lay;
	UBINT		b = c->base[r->comm];
	UBINT		a = b + c->padbar[r->comm] + r->pad * l->padsize;
	UINT		bank = r->g->bank;

	p->dw0 = in_w(a);
	p->dw1 = in_w(a + 4);
	p->own = ( in_w(b + PADOWN + r->ownreg * 4) >> ( ( r->bit % 8 ) * 4 ) ) & 0xf;
	p->hostsw = ( in_w(b + l->own + bank * 4) >> r->bit ) & 1;
	p->lock = ( in_w(b + l->lock + bank * 8) >> r->bit ) & 1;
	p->locktx = ( in_w(b + l->lock + bank * 8 + 4) >> r->bit ) & 1;
}

EXPORT void knl_pchgpio_say( CONST char *ctrl, UINT pin, CONST char *what )
{
	CTRL		*c = ctrl_of(ctrl);
	PADSTATE	p;
	PADREF		r;

	if ( c == NULL || !pad_of(c, pin, &r) ) {
		tm_printf((UB *)"gpio: %s: pin %d of %s: no such pad\n", what, pin, ctrl);
		return;
	}
	pad_read(c, &r, &p);
	tm_printf((UB *)"gpio: %s: pin %d (community %d pad %d): DW0 %08x DW1 %08x, mode %d, output %s at %d, "
		  "input %d, owner %s, %s, lock %d/%d\n",
		  what, pin, r.comm, r.pad, p.dw0, p.dw1, DW0_PMODE(p.dw0), ( p.dw0 & DW0_TXDIS ) ? "off" : "on",
		  ( p.dw0 & DW0_TXSTATE ) ? 1 : 0, ( p.dw0 & DW0_RXSTATE ) ? 1 : 0, ( p.own == 0 ) ? "host" : "ME",
		  p.hostsw ? "GPIO driver's" : "ACPI's", p.lock, p.locktx);
}

EXPORT ER knl_pchgpio_out( CONST char *ctrl, UINT pin, BOOL high )
{
	CTRL		*c = ctrl_of(ctrl);
	PADSTATE	p;
	PADREF		r;
	UBINT		a;
	UW		v;
	UINT		im;

	if ( c == NULL || !pad_of(c, pin, &r) ) return E_NOEXS;
	pad_read(c, &r, &p);
	if ( DW0_PMODE(p.dw0) != 0 || p.own != 0 ) {
		tm_printf((UB *)"gpio: pin %d of %s is not a GPIO of the host's (DW0 %08x, owner %d)\n",
			  pin, ctrl, p.dw0, p.own);
		return E_OBJ;
	}
	if ( p.locktx ) {
		tm_printf((UB *)"gpio: pin %d of %s: its output is locked by the firmware; written all the same\n", pin, ctrl);
	}
	a = c->base[r.comm] + c->padbar[r.comm] + r.pad * c->lay->padsize;
	ISpinLock(&gp_lock, &im);
	v = in_w(a) & ~(UW)DW0_TXDIS;
	v = high ? ( v | DW0_TXSTATE ) : ( v & ~(UW)DW0_TXSTATE );
	out_w(a, v);
	ISpinUnlock(&gp_lock, &im);
	return E_OK;
}

/* ---------------------------------------------------------------- inputs that interrupt */

/* The line of the controller's pads: every pad with its status and its enable set */
LOCAL void pads_inthdr( UINT intno )
{
	INT	n, i, k;

	for ( n = 0; n < nctrl; n++ ) {
		CTRL		*c = &ctrls[n];
		CONST LAYOUT	*l = c->lay;

		if ( c->gsi < 0 || (UINT)INTVEC_GSI(c->gsi) != intno ) continue;
		for ( i = 0; i < l->ngroup; i++ ) {
			CONST GROUP	*g = &l->group[i];
			UBINT		b = c->base[g->comm];
			UINT		im;
			UW		st, mask;

			ISpinLock(&gp_lock, &im);
			st = in_w(b + GPI_IS + g->bank * 4) & in_w(b + GPI_IE + g->bank * 4);
			if ( st == 0 ) {
				ISpinUnlock(&gp_lock, &im);
				continue;
			}
			/* masked first: a level still asserted sets its status again as soon as it is cleared */
			mask = st;
			for ( k = 0; k < npadint; k++ ) {
				if ( padints[k].g == g && padints[k].c == c && !padints[k].level ) {
					mask &= ~( 1U << padints[k].bit );
				}
			}
			if ( mask != 0 ) {
				out_w(b + GPI_IE + g->bank * 4, in_w(b + GPI_IE + g->bank * 4) & ~mask);
			}
			out_w(b + GPI_IS + g->bank * 4, st);
			ISpinUnlock(&gp_lock, &im);
			for ( k = 0; k < npadint; k++ ) {
				if ( padints[k].g == g && padints[k].c == c && ( st & ( 1U << padints[k].bit ) ) ) {
					st &= ~( 1U << padints[k].bit );
					padints[k].fn(padints[k].arg);
				}
			}
			if ( st != 0 ) {
				tm_printf((UB *)"gpio: community %d group %d: interrupts of pads nobody took (%x), masked\n",
					  g->comm, g->bank, st);
			}
		}
	}
	EndOfInt(intno);
}

EXPORT void *knl_pchgpio_int_take( CONST char *ctrl, UINT pin, UINT mode, void (*fn)( void *arg ), void *arg )
{
	CTRL		*c = ctrl_of(ctrl);
	PADSTATE	p;
	PADREF		r;
	PADINT		*h;
	UBINT		a;
	UW		v, want, keep;
	UINT		im;

	if ( c == NULL || !c->lay->has_int || npadint >= MAX_PADINT || !pad_of(c, pin, &r) ) {
		tm_printf((UB *)"gpio: pin %d of %s: no interrupt on this controller or no such pad\n", pin, ctrl);
		return NULL;
	}
	pad_read(c, &r, &p);
	if ( DW0_PMODE(p.dw0) != 0 || p.own != 0 || !p.hostsw ) {
		tm_printf((UB *)"gpio: pin %d of %s is not an input for the GPIO driver (DW0 %08x, owner %d, HOSTSW %d)\n",
			  pin, ctrl, p.dw0, p.own, p.hostsw);
		return NULL;
	}

	/* what the event needs, and only that, so that a pad the firmware locked in this state is usable */
	keep = DW0_RXINV | DW0_RXEV_MASK | DW0_RXDIS | DW0_TXDIS;
	want = DW0_TXDIS;
	if ( mode & PCHGPIO_BOTH ) {
		want |= DW0_RXEV_BOTH;
	} else {
		if ( mode & PCHGPIO_EDGE ) want |= DW0_RXEV_EDGE;
		if ( mode & PCHGPIO_LOW ) want |= DW0_RXINV;
	}
	a = c->base[r.comm] + c->padbar[r.comm] + r.pad * c->lay->padsize;

	if ( c->gsi < 0 ) {
		int	gsi = -1, gmode = 0;

		if ( knl_acpica_irq(knl_acpica_handle(ctrl), &gsi, &gmode) != 0 || gsi < 0 || gsi >= INTVEC_GSI_NUM ) {
			tm_printf((UB *)"gpio: %s: no interrupt line it can use (%d)\n", ctrl, gsi);
			return NULL;
		}
		c->gsi = gsi;
		knl_define_inthdr((INT)INTVEC_GSI(gsi), TA_HLNG, (FP)pads_inthdr);
		SetIntMode(INTVEC_GSI(gsi), (UINT)gmode);
		EnableInt(INTVEC_GSI(gsi), INTPRI_DEVICE);
		tm_printf((UB *)"gpio: %s: the pads' interrupts on line %d, mode %d\n", ctrl, gsi, gmode);
	}

	ISpinLock(&gp_lock, &im);
	v = in_w(a);
	if ( ( v & keep ) != want ) {
		out_w(a, ( v & ~keep ) | want);
		v = in_w(a);
	}
	ISpinUnlock(&gp_lock, &im);
	if ( ( v & keep ) != want ) {
		tm_printf((UB *)"gpio: pin %d of %s: the pad did not take the setting (DW0 %08x, lock %d)\n", pin, ctrl, v, p.lock);
		return NULL;
	}

	h = &padints[npadint];
	h->c = c;
	h->g = r.g;
	h->bit = r.bit;
	h->level = (BOOL)( ( mode & ( PCHGPIO_EDGE | PCHGPIO_BOTH ) ) == 0 );
	h->fn = fn;
	h->arg = arg;
	npadint++;
	tm_printf((UB *)"gpio: pin %d of %s: community %d group %d bit %d, %s, DW0 %08x, input %d\n", pin, ctrl, r.comm,
		  r.g->bank, r.bit, h->level ? "level" : "edge", v, ( v & DW0_RXSTATE ) ? 1 : 0);
	return h;
}

EXPORT void knl_pchgpio_int_on( void *pad, BOOL on )
{
	PADINT	*h = (PADINT *)pad;
	UBINT	b = h->c->base[h->g->comm];
	UBINT	ie = b + GPI_IE + h->g->bank * 4;
	UW	bit = 1U << h->bit;
	UINT	im;

	ISpinLock(&gp_lock, &im);
	if ( on ) {
		out_w(ie, in_w(ie) | bit);
	} else {
		out_w(ie, in_w(ie) & ~bit);
	}
	ISpinUnlock(&gp_lock, &im);
}

#endif /* X64 */
