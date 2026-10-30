/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpi.h
 *	What the kernel reads of the ACPI tables as it starts (acpi.c): the
 *	processors and the interrupt controllers (MADT), the PCI Express
 *	configuration space (MCFG), the fixed hardware (FADT) and the HPET.
 *	No AML is run; the one value of the DSDT read is the sleep type of
 *	\_S5, for turning the machine off.
 */

#ifndef _SYSDEPEND_ACPI_ACPI_
#define _SYSDEPEND_ACPI_ACPI_

#define ACPI_MAX_CPU		64	/* more than are started (CNF_MAX_PRCID), to count what is left */
#define ACPI_MAX_IOAPIC		4
#define ACPI_MAX_ISO		16
#define ACPI_MAX_MCFG		4

typedef struct {
	UW	apic_id;
	UW	uid;		/* the processor's UID in the namespace */
} T_ACPICPU;

typedef struct {
	UW	id;
	UW	addr;		/* physical address of its registers */
	UW	gsi_base;	/* the first global system interrupt it takes */
	UW	ninput;		/* how many (read from the IOAPIC itself) */
} T_ACPIIOAPIC;

typedef struct {
	UB	irq;		/* the ISA interrupt */
	UW	gsi;		/* the input it is wired to */
	UH	flags;		/* MPS INTI flags: bits 1:0 polarity, 3:2 trigger */
} T_ACPIISO;

typedef struct {
	UD	base;		/* physical address of the configuration space of bus 'start' ... */
	UH	seg;
	UB	start, end;	/* ... to 'end' */
} T_ACPIMCFG;

/* A register of the fixed hardware (Generic Address Structure) */
typedef struct {
	UB	space;		/* 0 memory, 1 I/O port */
	UB	width;		/* bits */
	UB	offset;
	UB	access;
	UD	addr;
} T_ACPIGAS;

#define ACPI_GAS_MEM	0
#define ACPI_GAS_IO	1

typedef struct {
	BOOL		present;	/* the tables were there and good */
	UB		oem[7];		/* the OEM ID of the RSDP */
	INT		ncpu;			/* the enabled processors, threads included */
	T_ACPICPU	cpu[ACPI_MAX_CPU];	/* the boot processor first */
	INT		ncpu_later;		/* listed as able to come online later, not started */
	INT		ncpu_over;		/* enabled ones beyond ACPI_MAX_CPU */
	UD		lapic_addr;
	INT		nioapic;
	T_ACPIIOAPIC	ioapic[ACPI_MAX_IOAPIC];
	INT		niso;
	T_ACPIISO	iso[ACPI_MAX_ISO];
	INT		lint_nmi;	/* the LINT input wired to NMI (0 or 1), -1 none */
	INT		nmcfg;
	T_ACPIMCFG	mcfg[ACPI_MAX_MCFG];
	UD		hpet_addr;	/* 0 when there is none */
	/* FADT */
	UH		sci_irq;
	UW		smi_cmd;
	UB		acpi_enable;
	UW		pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt;
	UW		pm_tmr;		/* the PM timer's I/O port, 0 when there is none */
	BOOL		pm_tmr32;	/* 32 bits wide (else 24) */
	UB		century;	/* the CMOS's century register, 0 when there is none */
	UH		iapc_boot;	/* IA-PC boot architecture flags */
	UW		flags;		/* FADT flags */
	T_ACPIGAS	reset_reg;
	UB		reset_value;
	UD		dsdt;
	INT		s5_typa, s5_typb;	/* \_S5's SLP_TYP, -1 when not found */
} T_ACPIINFO;

#define ACPI_FADT_RESET_REG	( 1U << 10 )
#define ACPI_FADT_HW_REDUCED	( 1U << 20 )
#define ACPI_FADT_LOW_POWER_S0	( 1U << 21 )

IMPORT T_ACPIINFO	knl_acpi;

/* The tables at rsdp_pa read (reset_main.c, before the interrupt controllers are set up) */
IMPORT ER	knl_acpi_init( UD rsdp_pa );
/* The index'th table with the signature, NULL when there is none (linear map) */
IMPORT CONST void *knl_acpi_table( CONST char *sig, INT index );
/* The input an ISA interrupt is wired to, and its MPS flags */
IMPORT UW	knl_acpi_isa_gsi( UB irq, UH *p_flags );
/* Off (ACPI S5), or a reset through the FADT's reset register: FALSE when it did not happen */
IMPORT BOOL	knl_acpi_poweroff( void );
IMPORT BOOL	knl_acpi_reset( void );

/* The ACPI interpreter (ACPICA, built apart and linked in) and what the kernel gives it */
#include "acpica/tsacpi_os.h"

#endif /* _SYSDEPEND_ACPI_ACPI_ */
