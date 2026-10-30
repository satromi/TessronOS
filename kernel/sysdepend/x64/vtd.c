/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	vtd.c (x64 PC)
 *	Intel VT-d, the IOMMU of the chipset, as the firmware hands it over
 *	(docs/tessronos/03-platform.md 3.10.9).
 *
 *	TessronOS does not use VT-d: every driver gives its devices physical
 *	addresses, and the messages of MSI and MSI-X are written in the
 *	compatibility format (cpu_msi.h). A firmware that protects the
 *	machine from DMA before the boot may leave a remapping unit with
 *	its translation, its interrupt remapping or its protected memory
 *	regions on. A device behind such a unit then has its DMA and its
 *	messages stopped or sent elsewhere, while devices behind a unit left
 *	off work.
 *
 *	So, before any device is started: the DMAR table is told whole on
 *	the console (KERNEL.TXT): its flags, each remapping unit (DRHD) with
 *	its registers and the devices it covers, the reserved memory
 *	(RMRR) and the rest by kind. Of each unit the version, the
 *	capabilities, the status, the root table, the interrupt remapping
 *	table, the invalidation queue, the protected memory regions and the
 *	fault records are read. What of translation (TES), interrupt
 *	remapping (IRES), the invalidation queue (QIES) and the protected
 *	memory (PMEN.EPM) is found on is turned off, in that order, each
 *	waited for in the status register, as the specification's
 *	programming of the global command register asks: the one-shot bits
 *	of GSTS masked off, the bit to change cleared, the rest written back
 *	as it was. The queue is turned off only once its head has come to
 *	its tail. The start-up setting vtd=keep only tells and leaves all of
 *	it as it was.
 *
 *	The fault records are told again later on demand
 *	(knl_vtd_faults), so that a device that does not answer can be
 *	looked at for what the unit stopped.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "../acpi/acpi.h"

IMPORT CONST void	*knl_acpi_table( CONST char *sig, INT index );	/* acpi.c */

/* The registers of a remapping unit */
#define R_VER		0x00
#define R_CAP		0x08
#define R_ECAP		0x10
#define R_GCMD		0x18
#define R_GSTS		0x1c
#define R_RTADDR	0x20
#define R_FSTS		0x34
#define R_FECTL		0x38
#define R_PMEN		0x64
#define R_PLMBASE	0x68
#define R_PLMLIMIT	0x6c
#define R_PHMBASE	0x70
#define R_PHMLIMIT	0x78
#define R_IQH		0x80
#define R_IQT		0x88
#define R_IQA		0x90
#define R_IRTA		0xb8

/* GSTS, and the same bits of GCMD */
#define ST_TE		0x80000000U	/* translation */
#define ST_RTP		0x40000000U	/* one shot: root table pointer set */
#define ST_FL		0x20000000U	/* one shot */
#define ST_AFL		0x10000000U
#define ST_WBF		0x08000000U	/* one shot */
#define ST_QIE		0x04000000U	/* the invalidation queue */
#define ST_IRE		0x02000000U	/* interrupt remapping */
#define ST_IRTP		0x01000000U	/* one shot */
#define ST_CFI		0x00800000U	/* compatibility format interrupts let through */
#define ST_KEEP		( ST_TE | ST_AFL | ST_QIE | ST_IRE | ST_CFI )	/* what is not one shot */

#define PMEN_EPM	0x80000000U
#define PMEN_PRS	0x00000001U

#define VTD_MAX		8
#define WAIT_US		100000		/* each change, at most */

LOCAL UBINT	vtd_base[VTD_MAX];
LOCAL UD	vtd_pa[VTD_MAX];
LOCAL INT	vtd_n = 0;

Inline UB rb( CONST UB *p, INT o ) { return p[o]; }
Inline UH rh( CONST UB *p, INT o ) { return (UH)( p[o] | ( p[o + 1] << 8 ) ); }
Inline UW rw( CONST UB *p, INT o ) { return (UW)rh(p, o) | ( (UW)rh(p, o + 2) << 16 ); }
Inline UD rd( CONST UB *p, INT o ) { return (UD)rw(p, o) | ( (UD)rw(p, o + 4) << 32 ); }

Inline UW r32( UBINT b, UW off ) { return *(volatile UW *)( b + off ); }
Inline UD r64( UBINT b, UW off ) { return *(volatile UD *)( b + off ); }
Inline void w32( UBINT b, UW off, UW v ) { *(volatile UW *)( b + off ) = v; }

/* The devices a structure names, from 'at' to its end */
LOCAL void say_scopes( CONST UB *s, INT at, INT len )
{
	CONST char *CONST kind[] = { "?", "device", "bridge and below", "IOAPIC", "HPET", "ACPI device" };

	while ( at + 6 <= len ) {
		INT	sl = rb(s, at + 1), k;
		UB	t = rb(s, at);

		if ( sl < 6 || at + sl > len ) break;
		tm_printf((UB *)"vtd:     scope %s, id %d, bus %d", kind[( t < 6 ) ? t : 0], rb(s, at + 4), rb(s, at + 5));
		for ( k = at + 6; k + 1 < at + sl; k += 2 ) {
			tm_printf((UB *)" / %d.%d", rb(s, k), rb(s, k + 1));
		}
		tm_printf((UB *)"\n");
		at += sl;
	}
}

/* Why a fault record says the unit stopped a request */
LOCAL CONST char *fault_why( UW fr )
{
	switch ( fr ) {
	case 0x01: return "no root entry";
	case 0x02: return "no context entry";
	case 0x05: return "write not allowed by the page";
	case 0x06: return "read not allowed by the page";
	case 0x20: return "interrupt: reserved field in the request";
	case 0x21: return "interrupt: index past the table";
	case 0x22: return "interrupt: entry not present";
	case 0x25: return "interrupt: compatibility format blocked";
	case 0x26: return "interrupt: remappable blocked";
	default:   return ( fr >= 0x20 ) ? "interrupt" : "DMA";
	}
}

/* The fault records of unit i that hold a fault */
LOCAL INT say_faults( INT i )
{
	UBINT	b = vtd_base[i];
	UD	cap = r64(b, R_CAP);
	UW	fro = (UW)( ( cap >> 24 ) & 0x3ff ) * 16, nfr = (UW)( ( cap >> 40 ) & 0xff ) + 1, k;
	INT	n = 0;

	for ( k = 0; k < nfr && k < 256; k++ ) {
		UD	lo = r64(b, fro + k * 16), hi = r64(b, fro + k * 16 + 8);
		UW	sid = (UW)( hi & 0xffff ), fr = (UW)( ( hi >> 32 ) & 0xff );

		if ( ( hi & ( 1ULL << 63 ) ) == 0 ) continue;
		tm_printf((UB *)"vtd:   fault %d: from %d:%d.%d, reason %02x (%s), %s, address %lx\n", k, sid >> 8,
			  ( sid >> 3 ) & 0x1f, sid & 7, fr, fault_why(fr),
			  ( fr >= 0x20 ) ? "interrupt" : ( ( hi >> 62 ) & 1 ) ? "read" : "write", lo & ~0xfffULL);
		n++;
	}
	return n;
}

/* A bit of GCMD changed, and waited for in GSTS: TRUE when GSTS shows it */
LOCAL BOOL gcmd_clear( UBINT b, UW bit )
{
	UW	st = r32(b, R_GSTS), t;

	w32(b, R_GCMD, ( st & ST_KEEP ) & ~bit);
	for ( t = 0; t < WAIT_US; t += 10 ) {
		if ( ( r32(b, R_GSTS) & bit ) == 0 ) return TRUE;
		WaitUsec(10);
	}
	tm_printf((UB *)"vtd:   GCMD %08x written over GSTS %08x; GSTS %08x after %d ms\n", ( st & ST_KEEP ) & ~bit, st,
		  r32(b, R_GSTS), WAIT_US / 1000);
	return FALSE;
}

LOCAL void say_unit( INT i, CONST char *when )
{
	UBINT	b = vtd_base[i];

	tm_printf((UB *)"vtd:   unit %d at %lx %s: VER %08x CAP %016lx ECAP %016lx GSTS %08x RTADDR %016lx\n", i,
		  vtd_pa[i], when, r32(b, R_VER), r64(b, R_CAP), r64(b, R_ECAP), r32(b, R_GSTS), r64(b, R_RTADDR));
	tm_printf((UB *)"vtd:   unit %d: FSTS %08x FECTL %08x PMEN %08x low %08x-%08x high %016lx-%016lx; "
		  "IQA %016lx IQH %lx IQT %lx; IRTA %016lx\n", i, r32(b, R_FSTS), r32(b, R_FECTL), r32(b, R_PMEN),
		  r32(b, R_PLMBASE), r32(b, R_PLMLIMIT), r64(b, R_PHMBASE), r64(b, R_PHMLIMIT), r64(b, R_IQA),
		  r64(b, R_IQH), r64(b, R_IQT), r64(b, R_IRTA));
}

/* Unit i's translation, interrupt remapping, queue and protected memory off, those that are on */
LOCAL void turn_off( INT i )
{
	UBINT	b = vtd_base[i];
	UW	st = r32(b, R_GSTS), t;

	if ( st & ST_TE ) {
		tm_printf((UB *)"vtd:   unit %d: translation was on: %s\n", i,
			  gcmd_clear(b, ST_TE) ? "turned off" : "NOT turned off");
	}
	if ( st & ST_IRE ) {
		tm_printf((UB *)"vtd:   unit %d: interrupt remapping was on (compatibility format %s): %s\n", i,
			  ( st & ST_CFI ) ? "let through" : "blocked", gcmd_clear(b, ST_IRE) ? "turned off" : "NOT turned off");
	}
	if ( st & ST_QIE ) {
		for ( t = 0; t < WAIT_US && r64(b, R_IQH) != r64(b, R_IQT); t += 10 ) {
			WaitUsec(10);
		}
		tm_printf((UB *)"vtd:   unit %d: the invalidation queue was on (head %lx, tail %lx): %s\n", i,
			  r64(b, R_IQH), r64(b, R_IQT), gcmd_clear(b, ST_QIE) ? "turned off" : "NOT turned off");
	}
	if ( r32(b, R_PMEN) & PMEN_EPM ) {
		w32(b, R_PMEN, 0);
		for ( t = 0; t < WAIT_US && ( r32(b, R_PMEN) & PMEN_PRS ) != 0; t += 10 ) {
			WaitUsec(10);
		}
		tm_printf((UB *)"vtd:   unit %d: the protected memory regions were on: %s\n", i,
			  ( r32(b, R_PMEN) & ( PMEN_EPM | PMEN_PRS ) ) == 0 ? "turned off" : "NOT turned off");
	}
}

/*
 * The DMAR table told, and what the firmware left on turned off unless
 * keep. Called once, before the devices are started.
 */
EXPORT void knl_vtd_init( BOOL keep )
{
	CONST UB	*t = (CONST UB *)knl_acpi_table("DMAR", 0);
	INT		len, at, i;
	BOOL		any_on = FALSE;

	if ( t == NULL ) {
		tm_printf((UB *)"vtd: no DMAR table: no VT-d\n");
		return;
	}
	len = (INT)rw(t, 4);
	tm_printf((UB *)"vtd: DMAR %d bytes, address width %d bits, flags %02x (interrupt remapping %s, x2APIC opt out %s, "
		  "DMA protection asked for %s)\n", len, rb(t, 36) + 1, rb(t, 37), ( rb(t, 37) & 1 ) ? "yes" : "no",
		  ( rb(t, 37) & 2 ) ? "yes" : "no", ( rb(t, 37) & 4 ) ? "yes" : "no");
	for ( at = 48; at + 4 <= len; ) {
		CONST UB	*s = t + at;
		UH		type = rh(s, 0), sl = rh(s, 2);

		if ( sl < 4 || at + sl > len ) break;
		switch ( type ) {
		case 0:
			tm_printf((UB *)"vtd:   DRHD unit %d: registers %lx, segment %d, flags %02x%s\n", vtd_n, rd(s, 8),
				  rh(s, 6), rb(s, 4), ( rb(s, 4) & 1 ) ? " (every device not named elsewhere)" : "");
			say_scopes(s, 16, sl);
			if ( vtd_n < VTD_MAX ) {
				vtd_pa[vtd_n] = rd(s, 8);
				vtd_base[vtd_n] = (UBINT)DEV_BASE(rd(s, 8));
				vtd_n++;
			}
			break;
		case 1:
			tm_printf((UB *)"vtd:   RMRR: memory %lx-%lx kept for DMA, segment %d\n", rd(s, 8), rd(s, 16), rh(s, 6));
			say_scopes(s, 24, sl);
			break;
		case 2:
			tm_printf((UB *)"vtd:   ATSR: flags %02x, segment %d\n", rb(s, 4), rh(s, 6));
			say_scopes(s, 8, sl);
			break;
		case 3:
			tm_printf((UB *)"vtd:   RHSA: unit at %lx in proximity domain %d\n", rd(s, 8), rw(s, 16));
			break;
		case 4:
			tm_printf((UB *)"vtd:   ANDD: ACPI device %d\n", rb(s, 7));
			break;
		default:
			tm_printf((UB *)"vtd:   structure %d, %d bytes\n", type, sl);
			break;
		}
		at += sl;
	}
	for ( i = 0; i < vtd_n; i++ ) {
		UW	st = r32(vtd_base[i], R_GSTS);

		say_unit(i, "as the firmware left it");
		if ( say_faults(i) == 0 ) {
			tm_printf((UB *)"vtd:   unit %d: no fault recorded\n", i);
		}
		if ( ( st & ( ST_TE | ST_IRE | ST_QIE ) ) != 0 || ( r32(vtd_base[i], R_PMEN) & PMEN_EPM ) != 0 ) {
			any_on = TRUE;
			if ( keep ) {
				tm_printf((UB *)"vtd:   unit %d: left on (vtd=keep)\n", i);
			} else {
				turn_off(i);
				say_unit(i, "now");
			}
		}
	}
	tm_printf((UB *)"vtd: %d unit(s); %s\n", vtd_n, !any_on ? "the firmware left them all off"
		  : keep ? "what the firmware left on is kept (vtd=keep)" : "what the firmware left on is turned off");
}

/* The units' status and fault records again, for a device that does not answer */
EXPORT void knl_vtd_faults( CONST char *why )
{
	INT	i;

	for ( i = 0; i < vtd_n; i++ ) {
		tm_printf((UB *)"vtd: (%s) unit %d: GSTS %08x FSTS %08x PMEN %08x\n", why, i, r32(vtd_base[i], R_GSTS),
			  r32(vtd_base[i], R_FSTS), r32(vtd_base[i], R_PMEN));
		if ( say_faults(i) == 0 ) {
			tm_printf((UB *)"vtd:   unit %d: no fault recorded\n", i);
		}
	}
}

#if USE_KTEST
#include "sysman/pfalloc.h"

/* How many remapping units the table names */
EXPORT INT knl_vtd_count( void )
{
	return vtd_n;
}

/*
 * For the tests on QEMU's intel-iommu, which no firmware turns on: unit
 * 0's invalidation queue turned on over an empty queue, as a firmware
 * would leave it, then turned off as at the start. E_OK when it went on
 * and came off, E_NOSPT when the unit has no queue.
 */
EXPORT ER knl_vtd_test_qi( void )
{
	UBINT	b;
	PFRAME	*pf;
	UD	pa;
	UW	t;
	ER	er = E_OK;

	if ( vtd_n == 0 ) return E_NOEXS;
	b = vtd_base[0];
	if ( ( r64(b, R_ECAP) & 0x2 ) == 0 ) return E_NOSPT;
	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) return E_NOMEM;
	pa = knl_pf_to_pa(pf);
	*(volatile UD *)( b + R_IQT ) = 0;
	*(volatile UD *)( b + R_IQA ) = pa;		/* 256 entries of 16 bytes */
	w32(b, R_GCMD, ( r32(b, R_GSTS) & ST_KEEP ) | ST_QIE);
	for ( t = 0; t < WAIT_US && ( r32(b, R_GSTS) & ST_QIE ) == 0; t += 10 ) WaitUsec(10);
	if ( ( r32(b, R_GSTS) & ST_QIE ) == 0 ) {
		er = E_IO;			/* did not go on */
	} else {
		/* used as a firmware uses it: an invalidation wait that writes a word, done */
		volatile UD	*q = (volatile UD *)PA2VA(pa);
		volatile UW	*done = (volatile UW *)PA2VA(pa + 2048);

		q[0] = 0x5 | 0x20 | ( 0x1ULL << 32 );	/* wait, status write of 1 */
		q[1] = pa + 2048;
		*(volatile UD *)( b + R_IQT ) = 16;
		for ( t = 0; t < WAIT_US && *done != 1; t += 10 ) WaitUsec(10);
		if ( *done != 1 ) tm_printf((UB *)"vtd: the test's invalidation wait did not end\n");
		turn_off(0);
		if ( r32(b, R_GSTS) & ST_QIE ) er = E_OBJ;	/* did not come off */
	}
	knl_free_pages(pf, 0);
	return er;
}
#endif

#endif /* X64 */
