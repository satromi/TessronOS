/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_pch_host.c
 *	device/gpio/x86_64/gpio_pch.c on the host, against communities of pads
 *	made of plain memory: which registers a pin of the namespace reaches
 *	on each layout, what taking a pad as an interrupt writes, what is
 *	refused, and how the handler acknowledges and masks. There is no
 *	QEMU machine with these pads; the real ones are for the machine itself.
 *
 *	  gcc -Wall -I tools/x64/tests/gpio_host -o /tmp/gph tools/x64/tests/gpio_pch_host.c && /tmp/gph
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define X64
#define _SYSDEPEND_ACPI_ACPI_		/* the ACPI calls are the ones below */

typedef int		INT;
typedef unsigned int	UINT;
typedef uint32_t	UW;
typedef uint16_t	UH;
typedef uint8_t		UB;
typedef unsigned long long UD;
typedef unsigned long	UBINT;
typedef int		BOOL;
typedef int		ER;
typedef int		T_SPLOCK;
typedef void		(*FP)( void );
#define TRUE		1
#define FALSE		0
#define E_OK		0
#define E_NOEXS		(-42)
#define E_OBJ		(-41)
#define LOCAL		static
#define EXPORT
#define IMPORT		extern
#define CONST		const
#define TA_HLNG		1
#define INTPRI_DEVICE	1
#define INTVEC_GSI(g)	( 0x30 + (g) )
#define INTVEC_GSI_NUM	32
#define DEV_BASE(b)	(b)
#define knl_memset	memset

static UW in_w( UBINT a ) { return *(volatile UW *)a; }
static void out_w( UBINT a, UW v );
#define InitSpinLock(l)		((void)(l))
#define ISpinLock(l, p)		((void)(l), (void)(p))
#define ISpinUnlock(l, p)	((void)(l), (void)(p))
#define tm_printf(...)		((void)0)

#define COMM_SIZE	0x4000
static unsigned char	comm[4][COMM_SIZE] __attribute__((aligned(16)));
static const char	*have_hid;
static int		line_gsi = 14, line_mode = 3;
static int		defined_vec, enabled_vec, eoi_count;
static void		(*handler)( UINT );

static void *knl_acpica_handle( const char *path ) { (void)path; return (void *)1; }
static void *knl_acpica_device( const char *hid, int k ) { return ( k == 0 && strcmp(hid, have_hid) == 0 ) ? (void *)1 : NULL; }
static int knl_acpica_mem( void *d, int k, unsigned long long *b, unsigned long long *len )
{
	(void)d;
	if ( k >= 4 ) return -1;
	*b = (unsigned long long)(uintptr_t)comm[k];
	*len = COMM_SIZE;
	return 0;
}
static int knl_acpica_irq( void *d, int *irq, int *mode ) { (void)d; *irq = line_gsi; *mode = line_mode; return ( line_gsi < 0 ) ? -1 : 0; }
static void knl_define_inthdr( INT vec, INT atr, FP f ) { (void)atr; defined_vec = vec; handler = (void (*)( UINT ))f; }
static void SetIntMode( UINT vec, UINT mode ) { (void)vec; (void)mode; }
static void EnableInt( UINT vec, INT pri ) { (void)pri; enabled_vec = (int)vec; }
static void EndOfInt( UINT vec ) { (void)vec; eoi_count++; }

/* GPI_IS is written 1 to clear */
static void out_w( UBINT a, UW v )
{
	int	k;

	for ( k = 0; k < 4; k++ ) {
		UBINT	o = a - (UBINT)(uintptr_t)comm[k];

		if ( o >= 0x100 && o < 0x120 ) {
			*(volatile UW *)a &= ~v;
			return;
		}
	}
	*(volatile UW *)a = v;
}

#include "../../../device/gpio/x86_64/gpio_pch.h"
#include "../../../device/gpio/x86_64/gpio_pch.c"

static int	fails, checks;
#define CHECK(c)	do { checks++; if ( !(c) ) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while ( 0 )

#define PADBAR_VAL	0x700
static UW *dw0( int k, int pad, int size ) { return (UW *)( comm[k] + PADBAR_VAL + pad * size ); }
static UW *reg( int k, int off ) { return (UW *)( comm[k] + off ); }

static void reset( const char *hid )
{
	int	k;

	memset(comm, 0, sizeof(comm));
	for ( k = 0; k < 4; k++ ) *reg(k, PADBAR) = PADBAR_VAL;
	memset(ctrls, 0, sizeof(ctrls));
	memset(padints, 0, sizeof(padints));
	nctrl = 0;
	npadint = 0;
	have_hid = hid;
	handler = NULL;
	defined_vec = enabled_vec = eoi_count = 0;
	line_gsi = 14;
	line_mode = 3;
}

static int	got_int;
static void *got_arg;
static void on_int( void *arg ) { got_int++; got_arg = arg; }

/* All pads of the group are the GPIO driver's */
static void all_hostsw( int k, int bank, int own_off ) { *reg(k, own_off + bank * 4) = 0xffffffff; }

static void test_map_tgl( void )
{
	CTRL	*c;
	PADREF	r;

	reset("INTC1055");
	c = ctrl_of("\\_SB.GPI0");
	CHECK(c != NULL);
	CHECK(c->lay->padsize == 16);

	/* the first of a group, in each community */
	CHECK(pad_of(c, 0, &r) && r.comm == 0 && r.pad == 0 && r.bit == 0 && r.g->bank == 0 && r.ownreg == 0);
	CHECK(pad_of(c, 25, &r) && r.comm == 0 && r.pad == 25 && r.bit == 25 && r.ownreg == 3);
	/* GPP_T: after the 4 PAD_OWN registers of the 26 pads of GPP_B */
	CHECK(pad_of(c, 32, &r) && r.comm == 0 && r.pad == 26 && r.bit == 0 && r.g->bank == 1 && r.ownreg == 4);
	/* GPP_A: GPP_B's 4, GPP_T's 2 */
	CHECK(pad_of(c, 64, &r) && r.comm == 0 && r.pad == 42 && r.bit == 0 && r.g->bank == 2 && r.ownreg == 6);
	CHECK(pad_of(c, 96, &r) && r.comm == 1 && r.pad == 0 && r.g->bank == 0);
	/* GPP_H3, GPP_D: the usual pads of a touch pad */
	CHECK(pad_of(c, 131, &r) && r.comm == 1 && r.pad == 11 && r.bit == 3 && r.g->bank == 1 && r.ownreg == 1);
	CHECK(pad_of(c, 160, &r) && r.comm == 1 && r.pad == 32 && r.g->bank == 2);
	/* community 2 is the fourth in the hardware's numbers and the third in _CRS */
	CHECK(pad_of(c, 256, &r) && r.comm == 2 && r.pad == 0 && r.g->bank == 0);
	/* GPP_E after HVCMOS, which has no pin: its bank and its pad count it all the same */
	CHECK(pad_of(c, 320, &r) && r.comm == 2 && r.pad == 55 && r.g->bank == 3 && r.ownreg == 3 + 4 + 1);
	CHECK(pad_of(c, 352, &r) && r.comm == 3 && r.pad == 0 && r.g->bank == 0);

	/* holes and the end */
	CHECK(!pad_of(c, 26, &r));
	CHECK(!pad_of(c, 31, &r));
	CHECK(!pad_of(c, 48, &r));
	CHECK(!pad_of(c, 360, &r));
}

static void test_map_spt( void )
{
	CTRL	*c;
	PADREF	r;
	UINT	pin;

	reset("INT344B");
	c = ctrl_of("\\_SB.PCI0.GPI0");
	CHECK(c != NULL && c->lay->padsize == 8 && !c->lay->has_int);
	/* a pin is a pad's number, the registers by groups of 24 and 8 owners a register, as always here */
	for ( pin = 0; pin <= 151; pin++ ) {
		UINT	first = ( pin < 48 ) ? 0 : ( pin < 120 ) ? 48 : 120;

		CHECK(pad_of(c, pin, &r));
		CHECK(r.pad == pin - first);
		CHECK(r.g->bank == r.pad / 24 && r.bit == r.pad % 24);
		CHECK(r.ownreg == r.pad / 8);
	}
	CHECK(!pad_of(c, 152, &r));
	/* no interrupt is taken on this one: the Surface Go 2's and X1 Yoga's stay as they were */
	CHECK(knl_pchgpio_int_take("\\_SB.PCI0.GPI0", 24, PCHGPIO_LOW, on_int, NULL) == NULL);
	CHECK(handler == NULL);
}

static void test_out_tgl( void )
{
	reset("INTC1055");
	all_hostsw(1, 1, 0x0b0);
	*dw0(1, 11, 16) = DW0_TXDIS;
	CHECK(knl_pchgpio_out("g", 131, TRUE) == E_OK);
	CHECK(*dw0(1, 11, 16) == DW0_TXSTATE);
	/* the ME's pad: bits 15:12 of the second owner register of GPP_H */
	CHECK(knl_pchgpio_out("g", 131, FALSE) == E_OK);
	*reg(1, PADOWN + 1 * 4) = 0x1 << 12;
	CHECK(knl_pchgpio_out("g", 131, TRUE) == E_OBJ);
}

static void test_int_level( void )
{
	void	*h;
	UW	*is, *ie;
	int	arg = 0;

	reset("INTC1055");
	all_hostsw(1, 1, 0x0b0);
	/* the firmware's pad: an output off, the input off, as an edge */
	*dw0(1, 11, 16) = DW0_RXDIS | DW0_RXEV_EDGE | DW0_TXDIS;
	h = knl_pchgpio_int_take("g", 131, PCHGPIO_LOW, on_int, &arg);
	CHECK(h != NULL);
	CHECK(*dw0(1, 11, 16) == ( DW0_TXDIS | DW0_RXINV ));
	CHECK(defined_vec == INTVEC_GSI(14) && enabled_vec == INTVEC_GSI(14));
	is = reg(1, GPI_IS + 4);
	ie = reg(1, GPI_IE + 4);
	CHECK(*ie == 0);			/* masked until asked */

	knl_pchgpio_int_on(h, TRUE);
	CHECK(*ie == ( 1U << 3 ));

	got_int = 0;
	*is = 1U << 3;
	handler(INTVEC_GSI(14));
	CHECK(got_int == 1 && got_arg == &arg);
	CHECK(*is == 0);			/* acknowledged */
	CHECK(*ie == 0);			/* a level masks until the cause is gone */
	CHECK(eoi_count == 1);

	/* status with the enable off is no interrupt of this pad */
	*is = 1U << 3;
	handler(INTVEC_GSI(14));
	CHECK(got_int == 1);
	*is = 0;

	/* another line, nothing */
	knl_pchgpio_int_on(h, TRUE);
	*is = 1U << 3;
	handler(INTVEC_GSI(15));
	CHECK(got_int == 1);
	*is = 0;
	knl_pchgpio_int_on(h, FALSE);
	CHECK(*ie == 0);
}

static void test_int_edge_stray( void )
{
	void	*h;
	UW	*is, *ie;

	reset("INTC1055");
	all_hostsw(2, 3, 0x0b0);
	*dw0(2, 55, 16) = DW0_TXDIS;
	h = knl_pchgpio_int_take("g", 320, PCHGPIO_EDGE | PCHGPIO_LOW, on_int, NULL);
	CHECK(h != NULL);
	CHECK(*dw0(2, 55, 16) == ( DW0_TXDIS | DW0_RXINV | DW0_RXEV_EDGE ));
	is = reg(2, GPI_IS + 3 * 4);
	ie = reg(2, GPI_IE + 3 * 4);
	knl_pchgpio_int_on(h, TRUE);

	/* its edge stays enabled; the stray the firmware left enabled is masked for good */
	*ie |= 1U << 7;
	got_int = 0;
	*is = ( 1U << 0 ) | ( 1U << 7 );
	handler(INTVEC_GSI(14));
	CHECK(got_int == 1);
	CHECK(*is == 0);
	CHECK(*ie == ( 1U << 0 ));

	/* both edges */
	reset("INTC1055");
	all_hostsw(2, 3, 0x0b0);
	*dw0(2, 55, 16) = DW0_TXDIS | DW0_RXINV;
	CHECK(knl_pchgpio_int_take("g", 320, PCHGPIO_EDGE | PCHGPIO_BOTH, on_int, NULL) != NULL);
	CHECK(*dw0(2, 55, 16) == ( DW0_TXDIS | DW0_RXEV_BOTH ));
}

/* Writes that do not stick, as on a pad the firmware locked */
static void test_refused( void )
{
	reset("INTC1055");
	/* ACPI's pad: HOSTSW_OWN 0 */
	*dw0(1, 11, 16) = DW0_TXDIS;
	CHECK(knl_pchgpio_int_take("g", 131, PCHGPIO_LOW, on_int, NULL) == NULL);
	CHECK(handler == NULL);

	/* a native function */
	all_hostsw(1, 1, 0x0b0);
	*dw0(1, 11, 16) = DW0_TXDIS | ( 1U << 10 );
	CHECK(knl_pchgpio_int_take("g", 131, PCHGPIO_LOW, on_int, NULL) == NULL);

	/* the ME's */
	*dw0(1, 11, 16) = DW0_TXDIS;
	*reg(1, PADOWN + 4) = 0x1 << 12;
	CHECK(knl_pchgpio_int_take("g", 131, PCHGPIO_LOW, on_int, NULL) == NULL);
	*reg(1, PADOWN + 4) = 0;

	/* no pad there, or a controller with no usable line (past the inputs this kernel routes) */
	CHECK(knl_pchgpio_int_take("g", 26, PCHGPIO_LOW, on_int, NULL) == NULL);
	line_gsi = 40;
	CHECK(knl_pchgpio_int_take("g", 131, PCHGPIO_LOW, on_int, NULL) == NULL);
	CHECK(handler == NULL);
}

int main( void )
{
	test_map_tgl();
	test_map_spt();
	test_out_tgl();
	test_int_level();
	test_int_edge_stray();
	test_refused();
	printf("gpio_pch_host: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
