/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpi.c
 *	The ACPI tables the kernel reads as it starts (acpi.h): the RSDP the
 *	UEFI loader found, the XSDT (or the RSDT of ACPI 1.0) and in it the
 *	MADT, the MCFG, the FADT and the HPET table. Every table is checked
 *	by its sum before it is used. The tables are reached through the
 *	linear map, which covers the firmware's memory below the top of RAM.
 *
 *	Nothing here runs AML. Of the DSDT only the package \_S5 is looked
 *	for, the way it is laid out by every compiler of ASL: its name, a
 *	PackageOp, the length, the count, and the two sleep types as
 *	constants. A machine whose \_S5 is made otherwise is turned off
 *	some other way once the interpreter is there.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include "acpi.h"

EXPORT T_ACPIINFO	knl_acpi;

typedef struct {
	UB	sig[4];
	UW	length;
	UB	revision;
	UB	checksum;
	UB	oemid[6];
	UB	oem_table_id[8];
	UW	oem_revision;
	UW	creator_id;
	UW	creator_revision;
} __attribute__((packed)) T_SDTHDR;

typedef struct {
	UB	sig[8];
	UB	checksum;
	UB	oemid[6];
	UB	revision;
	UW	rsdt;
	UW	length;
	UD	xsdt;
	UB	xchecksum;
	UB	reserved[3];
} __attribute__((packed)) T_RSDP;

#define ACPI_MAX_TABLES	64

LOCAL CONST T_SDTHDR	*tables[ACPI_MAX_TABLES];
LOCAL INT		ntables = 0;

Inline UB rd8( CONST void *p, INT off )
{
	return ((CONST UB *)p)[off];
}

Inline UH rd16( CONST void *p, INT off )
{
	CONST UB	*b = (CONST UB *)p + off;

	return (UH)( b[0] | ( b[1] << 8 ) );
}

Inline UW rd32( CONST void *p, INT off )
{
	CONST UB	*b = (CONST UB *)p + off;

	return (UW)b[0] | ( (UW)b[1] << 8 ) | ( (UW)b[2] << 16 ) | ( (UW)b[3] << 24 );
}

Inline UD rd64( CONST void *p, INT off )
{
	return (UD)rd32(p, off) | ( (UD)rd32(p, off + 4) << 32 );
}

LOCAL BOOL sum_ok( CONST void *p, UW len )
{
	CONST UB	*b = p;
	UB		s = 0;
	UW		i;

	for ( i = 0; i < len; i++ ) {
		s += b[i];
	}
	return ( s == 0 );
}

LOCAL BOOL sig_is( CONST UB *s, CONST char *want, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( s[i] != (UB)want[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL CONST T_SDTHDR *table_at( UD pa )
{
	CONST T_SDTHDR	*h;

	if ( pa == 0 ) {
		return NULL;
	}
	h = (CONST T_SDTHDR *)PA2VA(pa);
	if ( h->length < sizeof(T_SDTHDR) || h->length > 0x1000000 || !sum_ok(h, h->length) ) {
		return NULL;
	}
	return h;
}

EXPORT CONST void *knl_acpi_table( CONST char *sig, INT index )
{
	INT	i;

	for ( i = 0; i < ntables; i++ ) {
		if ( sig_is(tables[i]->sig, sig, 4) && index-- == 0 ) {
			return tables[i];
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */

LOCAL void read_madt( CONST T_SDTHDR *h )
{
	CONST UB	*p = (CONST UB *)h, *e = p + h->length;
	UW		a, b, c, d, boot;

	knl_acpi.lapic_addr = rd32(h, 36);
	knl_acpi.lint_nmi = -1;
	cpuid(0, 0, &a, &b, &c, &d);
	if ( a >= 0xb ) {
		cpuid(0xb, 0, &a, &b, &c, &d);
	} else {
		a = b = 0;
	}
	boot = d;				/* the boot processor's x2APIC ID */
	if ( a == 0 && b == 0 ) {
		cpuid(1, 0, &a, &b, &c, &d);
		boot = b >> 24;			/* no leaf 0Bh: the initial APIC ID */
	}

	for ( p += 44; p + 2 <= e && p[1] >= 2 && p + p[1] <= e; p += p[1] ) {
		UW	id, flags;

		switch ( p[0] ) {
		case 0:				/* Processor Local APIC */
		case 9:				/* Processor Local x2APIC */
			if ( p[0] == 0 ) {
				id = p[3];
				flags = rd32(p, 4);
			} else {
				id = rd32(p, 4);
				flags = rd32(p, 8);
			}
			/*
			 * Enabled (bit 0): started. Able to come online later
			 * (bit 1 alone): counted and left, as the processors a
			 * PC's firmware brings in later are. A processor listed
			 * twice (as a local APIC and as an x2APIC) once.
			 */
			if ( ( flags & 1 ) == 0 ) {
				if ( ( flags & 2 ) != 0 ) knl_acpi.ncpu_later++;
				break;
			}
			{
				INT	k;

				for ( k = 0; k < knl_acpi.ncpu && knl_acpi.cpu[k].apic_id != id; k++ ) ;
				if ( k < knl_acpi.ncpu ) break;
			}
			if ( knl_acpi.ncpu >= ACPI_MAX_CPU ) {
				knl_acpi.ncpu_over++;
				break;
			}
			if ( id == boot && knl_acpi.ncpu > 0 ) {
				knl_acpi.cpu[knl_acpi.ncpu] = knl_acpi.cpu[0];
				knl_acpi.cpu[0].apic_id = id;
				knl_acpi.cpu[0].uid = ( p[0] == 0 ) ? p[2] : rd32(p, 12);
			} else {
				knl_acpi.cpu[knl_acpi.ncpu].apic_id = id;
				knl_acpi.cpu[knl_acpi.ncpu].uid = ( p[0] == 0 ) ? p[2] : rd32(p, 12);
			}
			knl_acpi.ncpu++;
			break;
		case 1:				/* I/O APIC */
			if ( knl_acpi.nioapic < ACPI_MAX_IOAPIC ) {
				T_ACPIIOAPIC	*a = &knl_acpi.ioapic[knl_acpi.nioapic++];

				a->id = p[2];
				a->addr = rd32(p, 4);
				a->gsi_base = rd32(p, 8);
			}
			break;
		case 2:				/* Interrupt Source Override */
			if ( knl_acpi.niso < ACPI_MAX_ISO ) {
				T_ACPIISO	*o = &knl_acpi.iso[knl_acpi.niso++];

				o->irq = p[3];
				o->gsi = rd32(p, 4);
				o->flags = rd16(p, 8);
			}
			break;
		case 4:				/* Local APIC NMI */
			knl_acpi.lint_nmi = p[5] & 1;
			break;
		case 0x0a:			/* Local x2APIC NMI */
			knl_acpi.lint_nmi = p[8] & 1;
			break;
		case 5:				/* Local APIC Address Override */
			knl_acpi.lapic_addr = rd64(p, 4);
			break;
		default:
			break;
		}
	}
}

LOCAL void read_mcfg( CONST T_SDTHDR *h )
{
	CONST UB	*p = (CONST UB *)h + 44, *e = (CONST UB *)h + h->length;

	for ( ; p + 16 <= e && knl_acpi.nmcfg < ACPI_MAX_MCFG; p += 16 ) {
		T_ACPIMCFG	*m = &knl_acpi.mcfg[knl_acpi.nmcfg++];

		m->base = rd64(p, 0);
		m->seg = rd16(p, 8);
		m->start = p[10];
		m->end = p[11];
	}
}

LOCAL void read_gas( T_ACPIGAS *g, CONST void *p )
{
	g->space = rd8(p, 0);
	g->width = rd8(p, 1);
	g->offset = rd8(p, 2);
	g->access = rd8(p, 3);
	g->addr = rd64(p, 4);
}

/*
 * The sleep types of \_S5 in the DSDT: Name (_S5, Package () { a, b, ... })
 * as the compilers lay it out
 */
LOCAL void find_s5( CONST T_SDTHDR *h )
{
	CONST UB	*p = (CONST UB *)h + sizeof(T_SDTHDR), *e = (CONST UB *)h + h->length;
	INT		v[2], k;

	for ( ; p + 8 < e; p++ ) {
		if ( !sig_is(p, "_S5_", 4) || p[4] != 0x12 ) {
			continue;
		}
		if ( !( p[-1] == 0x08 || ( p[-2] == 0x08 && p[-1] == '\\' ) ) ) {
			continue;		/* not the name of an object */
		}
		p += 5;
		p += 1 + ( *p >> 6 );		/* PkgLength: its lead byte says how many follow */
		p++;				/* NumElements */
		for ( k = 0; k < 2 && p < e; k++ ) {
			if ( *p == 0x0a ) {		/* BytePrefix */
				v[k] = p[1];
				p += 2;
			} else if ( *p == 0x00 || *p == 0x01 ) {	/* Zero, One */
				v[k] = *p;
				p++;
			} else {
				return;
			}
		}
		if ( k == 2 ) {
			knl_acpi.s5_typa = v[0];
			knl_acpi.s5_typb = v[1];
		}
		return;
	}
}

LOCAL void read_fadt( CONST T_SDTHDR *h )
{
	UW	len = h->length;
	UD	dsdt;

	knl_acpi.sci_irq = rd16(h, 46);
	knl_acpi.smi_cmd = rd32(h, 48);
	knl_acpi.acpi_enable = rd8(h, 52);
	knl_acpi.pm1a_evt = rd32(h, 56);
	knl_acpi.pm1b_evt = rd32(h, 60);
	knl_acpi.pm1a_cnt = rd32(h, 64);
	knl_acpi.pm1b_cnt = rd32(h, 68);
	knl_acpi.pm_tmr = rd32(h, 76);
	if ( len >= 116 ) {
		knl_acpi.century = rd8(h, 108);
		knl_acpi.iapc_boot = rd16(h, 109);
		knl_acpi.flags = rd32(h, 112);
		knl_acpi.pm_tmr32 = ( knl_acpi.flags & ( 1U << 8 ) ) != 0;
	}
	if ( len >= 129 ) {
		read_gas(&knl_acpi.reset_reg, (CONST UB *)h + 116);
		knl_acpi.reset_value = rd8(h, 128);
	}
	dsdt = rd32(h, 40);
	if ( len >= 148 && rd64(h, 140) != 0 ) {
		dsdt = rd64(h, 140);
	}
	knl_acpi.dsdt = dsdt;
	{
		CONST T_SDTHDR	*d = table_at(dsdt);

		if ( d != NULL && sig_is(d->sig, "DSDT", 4) ) {
			find_s5(d);
		}
	}
}

EXPORT ER knl_acpi_init( UD rsdp_pa )
{
	CONST T_RSDP	*r;
	CONST T_SDTHDR	*x;
	INT		i, n, w;
	CONST void	*t;

	knl_memset(&knl_acpi, 0, sizeof(knl_acpi));
	knl_acpi.s5_typa = knl_acpi.s5_typb = -1;
	knl_acpi.lint_nmi = -1;
	if ( rsdp_pa == 0 ) {
		return E_NOEXS;
	}
	r = (CONST T_RSDP *)PA2VA(rsdp_pa);
	if ( !sig_is(r->sig, "RSD PTR ", 8) || !sum_ok(r, 20) ) {
		return E_OBJ;
	}
	knl_memcpy(knl_acpi.oem, r->oemid, 6);
	if ( r->revision >= 2 && r->xsdt != 0 && sum_ok(r, r->length) ) {
		x = table_at(r->xsdt);
		w = 8;
	} else {
		x = table_at(r->rsdt);
		w = 4;
	}
	if ( x == NULL ) {
		return E_OBJ;
	}
	n = (INT)( ( x->length - sizeof(T_SDTHDR) ) / w );
	for ( i = 0; i < n && ntables < ACPI_MAX_TABLES; i++ ) {
		UD	pa = ( w == 8 ) ? rd64(x, sizeof(T_SDTHDR) + i * 8) : rd32(x, sizeof(T_SDTHDR) + i * 4);
		CONST T_SDTHDR	*h = table_at(pa);

		if ( h != NULL ) {
			tables[ntables++] = h;
		}
	}

	if ( ( t = knl_acpi_table("APIC", 0) ) != NULL ) read_madt(t);
	if ( ( t = knl_acpi_table("MCFG", 0) ) != NULL ) read_mcfg(t);
	if ( ( t = knl_acpi_table("FACP", 0) ) != NULL ) read_fadt(t);
	if ( ( t = knl_acpi_table("HPET", 0) ) != NULL ) {
		T_ACPIGAS	g;

		read_gas(&g, (CONST UB *)t + 40);
		if ( g.space == ACPI_GAS_MEM ) knl_acpi.hpet_addr = g.addr;
	}
	knl_acpi.present = TRUE;

	tm_printf((UB *)"TessronOS x64: ACPI %s: %d tables, %d CPUs, %d IOAPICs, %d overrides, "
		  "MCFG %d, HPET %lx, PM timer %x, S5 %d/%d\n",
		  knl_acpi.oem, ntables, knl_acpi.ncpu, knl_acpi.nioapic, knl_acpi.niso,
		  knl_acpi.nmcfg, knl_acpi.hpet_addr, knl_acpi.pm_tmr,
		  knl_acpi.s5_typa, knl_acpi.s5_typb);
	return E_OK;
}

EXPORT UW knl_acpi_isa_gsi( UB irq, UH *p_flags )
{
	INT	i;

	for ( i = 0; i < knl_acpi.niso; i++ ) {
		if ( knl_acpi.iso[i].irq == irq ) {
			if ( p_flags != NULL ) *p_flags = knl_acpi.iso[i].flags;
			return knl_acpi.iso[i].gsi;
		}
	}
	if ( p_flags != NULL ) *p_flags = 0;	/* as the bus says: ISA is edge, active high */
	return irq;
}

/* ------------------------------------------------------------------------ */

#define SLP_EN		( 1U << 13 )
#define SLP_TYP_MASK	( 7U << 10 )
#define PM1_STS_ALL	0x8731U		/* the fixed status bits of PM1 event: written 1 to clear */

/*
 * Off by the tables alone (no interpreter): the PM1 status cleared (a
 * wake already latched, the power button's among them, would start the
 * machine again at once), then each PM1 control block written with its
 * own SLP_TYP, the other bits (SCI_EN among them) kept, first without
 * SLP_EN and then with it. A hardware-reduced machine, which has no PM1
 * blocks, is not turned off this way.
 */
EXPORT BOOL knl_acpi_poweroff( void )
{
	UH	a, b = 0;

	if ( knl_acpi.pm1a_cnt == 0 || knl_acpi.s5_typa < 0 ) {
		return FALSE;
	}
	if ( knl_acpi.pm1a_evt != 0 ) out_port_h((UH)knl_acpi.pm1a_evt, (UH)PM1_STS_ALL);
	if ( knl_acpi.pm1b_evt != 0 ) out_port_h((UH)knl_acpi.pm1b_evt, (UH)PM1_STS_ALL);
	a = (UH)( ( in_port_h((UH)knl_acpi.pm1a_cnt) & ~( SLP_TYP_MASK | SLP_EN ) ) | ( (UW)knl_acpi.s5_typa << 10 ) );
	if ( knl_acpi.pm1b_cnt != 0 ) {
		b = (UH)( ( in_port_h((UH)knl_acpi.pm1b_cnt) & ~( SLP_TYP_MASK | SLP_EN ) ) | ( (UW)knl_acpi.s5_typb << 10 ) );
	}
	out_port_h((UH)knl_acpi.pm1a_cnt, a);
	if ( knl_acpi.pm1b_cnt != 0 ) out_port_h((UH)knl_acpi.pm1b_cnt, b);
	out_port_h((UH)knl_acpi.pm1a_cnt, (UH)( a | SLP_EN ));
	if ( knl_acpi.pm1b_cnt != 0 ) out_port_h((UH)knl_acpi.pm1b_cnt, (UH)( b | SLP_EN ));
	WaitUsec(100000);
	return FALSE;
}

EXPORT BOOL knl_acpi_reset( void )
{
	T_ACPIGAS	*g = &knl_acpi.reset_reg;

	if ( ( knl_acpi.flags & ACPI_FADT_RESET_REG ) != 0 && g->addr != 0 ) {
		if ( g->space == ACPI_GAS_IO ) {
			out_port_b((UH)g->addr, knl_acpi.reset_value);
		} else if ( g->space == ACPI_GAS_MEM ) {
			out_b(DEV_BASE(g->addr), knl_acpi.reset_value);
		}
		WaitUsec(100000);
	}
	out_port_b(0xcf9, 0x02);		/* the PCH's reset control: a hard reset */
	out_port_b(0xcf9, 0x06);
	WaitUsec(100000);
	out_port_b(0x64, 0xfe);			/* the keyboard controller's reset line */
	WaitUsec(100000);
	return FALSE;
}

#endif /* X64 */
