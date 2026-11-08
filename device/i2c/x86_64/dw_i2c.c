/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dw_i2c.c (x64 PC)
 *	The I2C controllers of an Intel chipset's LPSS (Low Power Subsystem):
 *	Synopsys DesignWare I2C masters, one PCI function each, behind which
 *	a notebook PC has its touch screen, its touch pad or its pen (HID
 *	over I2C, hid_i2c.c) and its camera's power controller.
 *
 *	The functions are Intel's of class 0C.80 (Serial bus, other) or
 *	11.80 (Signal processing, other): Sunrise Point and Kaby Lake (the
 *	100 and 200 series, 9D60h and on) give the latter, later chipsets
 *	the former, and the LPSS's UARTs, SPI controllers and the thermal
 *	functions share both classes. A function is touched only when an I2C
 *	device of the ACPI namespace names it as its bus (by its _ADR), and
 *	only when the LPSS's own registers after the controller's (0x200 on)
 *	say it is an I2C controller (their capabilities at 0x2FC, type 0).
 *
 *	It is set up as follows: its ACPI device's _PS0 run and the PCI
 *	function put into D0 (the firmware may leave it in D3hot, where it
 *	answers nothing); its memory window and bus mastering on; the LPSS's
 *	reset of it put on and taken off again (0x204: the function and its
 *	DMA); then made a master at the speed of the devices on it, with the
 *	clock counts its ACPI device gives for that speed (SSCN for the
 *	standard mode, FMCN for the fast, FPCN for the fast plus: SCL's high
 *	and low counts and SDA's hold), which match the input clock of the
 *	chipset. Without them the counts the firmware had in it before the
 *	reset are put back. A transfer is done by looking at the FIFOs,
 *	without the interrupt: the devices behind it are read a few bytes at
 *	a time.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/pm.h>
#include <ts/pcie.h>
#include "../../../kernel/sysdepend/acpi/acpi.h"
#include "i2c.h"

#define IC_CON			0x00
#define IC_TAR			0x04
#define IC_SS_SCL_HCNT		0x14
#define IC_SS_SCL_LCNT		0x18
#define IC_FS_SCL_HCNT		0x1c
#define IC_FS_SCL_LCNT		0x20
#define IC_DATA_CMD		0x10
#define IC_INTR_MASK		0x30
#define IC_RAW_INTR_STAT	0x34
#define IC_CLR_INTR		0x40
#define IC_CLR_TX_ABRT		0x54
#define IC_ENABLE		0x6c
#define IC_STATUS		0x70
#define IC_TXFLR		0x74
#define IC_RXFLR		0x78
#define IC_SDA_HOLD		0x7c
#define IC_TX_ABRT_SOURCE	0x80
#define IC_ENABLE_STATUS	0x9c
#define IC_COMP_PARAM_1		0xf4
#define IC_COMP_VERSION		0xf8
#define IC_COMP_TYPE		0xfc

/* The LPSS's own registers, after the controller's */
#define LPSS_RESETS		0x204
#define LPSS_CAPS		0x2fc
#define LPSS_RESETS_FUNC	0x3		/* the controller out of reset */
#define LPSS_RESETS_IDMA	0x4		/* and its DMA */
#define LPSS_CAPS_TYPE(c)	( ( (c) >> 4 ) & 0xf )
#define LPSS_TYPE_I2C		0

#define COMP_TYPE_DW		0x44570140
#define COMP_VER_SDA_HOLD	0x3131312a	/* 1.11a: IC_SDA_HOLD there, its RX hold too */
#define SDA_HOLD_RX_MASK	0x00ff0000
#define SDA_HOLD_RX_SHIFT	16

#define CON_MASTER		0x01
#define CON_SPEED_STD		0x02
#define CON_SPEED_FAST		0x04		/* fast and fast plus */
#define CON_RESTART		0x20
#define CON_SLAVE_OFF		0x40
#define CMD_READ		0x100
#define CMD_STOP		0x200
#define RAW_TX_ABRT		0x40
#define STATUS_ACTIVITY		0x01
#define STATUS_TFE		0x04

#define PCI_CAP_PM		0x01
#define PM_CTRL			4		/* PMCSR, after the capability's header */
#define PM_STATE		0x0003
#define PM_PME_STATUS		0x8000		/* write 1 to clear: kept as it is */

#define DW_MAX			8

typedef struct {
	T_I2CBUS	bus;
	UBINT		base;
	UW		adr;
	ID		mtx;
	UW		fifo;
	UW		hz;		/* the speed it runs at */
	UW		ver;
	void		*acpi;		/* its ACPI device */
	UW		fw[5];		/* the firmware's counts: SS high, low, FS high, low, SDA hold */
} DWI2C;

LOCAL DWI2C	dw[DW_MAX];
LOCAL INT	ndw = 0;

LOCAL UW  rd( DWI2C *d, UW o ) { return in_w(d->base + o); }
LOCAL void wr( DWI2C *d, UW o, UW v ) { out_w(d->base + o, v); }

LOCAL BOOL enable( DWI2C *d, BOOL on )
{
	INT	i;

	wr(d, IC_ENABLE, on ? 1 : 0);
	for ( i = 0; i < 100; i++ ) {
		if ( ( rd(d, IC_ENABLE_STATUS) & 1 ) == ( on ? 1U : 0U ) ) return TRUE;
		WaitUsec(100);
	}
	return FALSE;
}

LOCAL ER dw_xfer( void *ctx, UH addr, CONST UB *w, INT wn, UB *r, INT rn )
{
	DWI2C	*d = (DWI2C *)ctx;
	INT	i, sent, got = 0, t;
	ER	er = E_OK;

	if ( wn + rn <= 0 ) return E_PAR;
	tk_loc_mtx(d->mtx, TMO_FEVR);
	(void)enable(d, FALSE);
	wr(d, IC_TAR, addr & 0x3ff);
	(void)enable(d, TRUE);
	(void)rd(d, IC_CLR_INTR);
	/* the bytes written, then a read command a byte; the last with the stop */
	for ( i = 0, sent = 0; i < wn + rn && er == E_OK; i++ ) {
		UW	c = ( i < wn ) ? w[i] : CMD_READ;

		if ( i == wn + rn - 1 ) c |= CMD_STOP;
		for ( t = 0; rd(d, IC_TXFLR) >= d->fifo && t < 10000; t++ ) WaitUsec(10);
		wr(d, IC_DATA_CMD, c);
		sent++;
		while ( rd(d, IC_RXFLR) > 0 && got < rn ) r[got++] = (UB)rd(d, IC_DATA_CMD);
		if ( rd(d, IC_RAW_INTR_STAT) & RAW_TX_ABRT ) er = E_IO;
	}
	for ( t = 0; er == E_OK && got < rn && t < 20000; t++ ) {
		while ( rd(d, IC_RXFLR) > 0 && got < rn ) r[got++] = (UB)rd(d, IC_DATA_CMD);
		if ( rd(d, IC_RAW_INTR_STAT) & RAW_TX_ABRT ) er = E_IO;
		if ( got < rn ) WaitUsec(10);
	}
	for ( t = 0; t < 10000 && ( rd(d, IC_STATUS) & STATUS_ACTIVITY ); t++ ) WaitUsec(10);
	if ( rd(d, IC_RAW_INTR_STAT) & RAW_TX_ABRT ) {
		(void)rd(d, IC_TX_ABRT_SOURCE);
		(void)rd(d, IC_CLR_TX_ABRT);
		er = E_IO;
	}
	if ( er == E_OK && got < rn ) er = E_TMOUT;
	tk_unl_mtx(d->mtx);
	return er;
}

/* The PCI function in D0: the state it was found in (0..3), -1 when it has no power management */
LOCAL INT to_d0( CONST T_PCIDEV *p )
{
	UINT	pm = ts_pcie_cap_find(p, PCI_CAP_PM);
	UW	cs;

	if ( pm == 0 ) return -1;
	cs = ts_pcie_cfg_read(p->bus, p->dev, p->fn, pm + PM_CTRL, 2);
	if ( ( cs & PM_STATE ) != 0 ) {
		ts_pcie_cfg_write(p->bus, p->dev, p->fn, pm + PM_CTRL, cs & ~(UW)( PM_STATE | PM_PME_STATUS ), 2);
		tk_dly_tsk(10);			/* D3hot to D0: 10 ms before it is touched */
	}
	return (INT)( cs & PM_STATE );
}

/* Three counts of a speed from the ACPI device (SCL's high and low, SDA's hold): whether it has them */
LOCAL BOOL acpi_counts( void *acpi, CONST char *method, UW *hcnt, UW *lcnt, UW *hold )
{
	unsigned long long	v[3];
	char			s[3][32];
	INT			n;

	if ( acpi == NULL ) return FALSE;
	n = knl_acpica_pkg(acpi, method, v, s, 3);
	if ( n < 2 || v[0] == 0 || v[1] == 0 || v[0] > 0xffff || v[1] > 0xffff ) return FALSE;
	*hcnt = (UW)v[0];
	*lcnt = (UW)v[1];
	*hold = ( n >= 3 && v[2] <= 0xffffff ) ? (UW)v[2] : 0;
	return TRUE;
}

/*
 * The master's speed and clock counts for devices of hz: standard mode
 * below 400kHz, fast mode from it, the fast plus's counts from 1MHz.
 * The counts of the standard mode are put in whatever the speed, so
 * that a switch to it later finds them right.
 */
LOCAL void set_speed( DWI2C *d, UW hz )
{
	UW	sh = 0, sl = 0, shold = 0, fh = 0, fl = 0, fhold = 0, hold, con;
	BOOL	ss, fs;
	CONST char *how;

	ss = acpi_counts(d->acpi, "SSCN", &sh, &sl, &shold);
	how = "FMCN";
	if ( hz >= 1000000 && acpi_counts(d->acpi, "FPCN", &fh, &fl, &fhold) ) {
		fs = TRUE;
		how = "FPCN";
	} else {
		fs = acpi_counts(d->acpi, "FMCN", &fh, &fl, &fhold);
	}
	if ( !ss ) {
		sh = d->fw[0];
		sl = d->fw[1];
		shold = d->fw[4];
	}
	if ( !fs ) {
		fh = d->fw[2];
		fl = d->fw[3];
		fhold = d->fw[4];
	}
	(void)enable(d, FALSE);
	if ( sh != 0 && sl != 0 ) {
		wr(d, IC_SS_SCL_HCNT, sh);
		wr(d, IC_SS_SCL_LCNT, sl);
	}
	if ( fh != 0 && fl != 0 ) {
		wr(d, IC_FS_SCL_HCNT, fh);
		wr(d, IC_FS_SCL_LCNT, fl);
	}
	con = CON_MASTER | CON_RESTART | CON_SLAVE_OFF | ( hz >= 400000 ? CON_SPEED_FAST : CON_SPEED_STD );
	hold = ( hz >= 400000 ) ? fhold : shold;
	if ( d->ver >= COMP_VER_SDA_HOLD ) {
		if ( hold == 0 ) hold = d->fw[4];
		/* a hold of the transmit side only: the receive side's one clock, as the hardware's least */
		if ( hold != 0 && ( hold & SDA_HOLD_RX_MASK ) == 0 ) hold |= 1U << SDA_HOLD_RX_SHIFT;
		if ( hold != 0 ) wr(d, IC_SDA_HOLD, hold);
	}
	wr(d, IC_CON, con);
	wr(d, IC_INTR_MASK, 0);
	d->hz = hz;
	tm_printf((UB *)"i2c: %s at %d Hz: %s mode, SCL counts %d/%d (%s), fast %d/%d (%s), SDA hold %x\n",
		  d->bus.name, hz, ( hz >= 400000 ) ? "fast" : "standard",
		  rd(d, IC_SS_SCL_HCNT), rd(d, IC_SS_SCL_LCNT), ss ? "SSCN" : "as it was",
		  rd(d, IC_FS_SCL_HCNT), rd(d, IC_FS_SCL_LCNT), fs ? how : "as it was",
		  ( d->ver >= COMP_VER_SDA_HOLD ) ? rd(d, IC_SDA_HOLD) : 0);
}

/* The LPSS I2C function at a PCI device and function of bus 0 */
LOCAL BOOL lpss_find( UW adr, T_PCIDEV *p )
{
	INT	i, n = ts_pcie_count();

	for ( i = 0; i < n; i++ ) {
		if ( ts_pcie_get(i, p) != E_OK ) continue;
		if ( p->vendor != 0x8086 || p->bus != 0 || ( ( (UW)p->dev << 16 ) | p->fn ) != adr ) continue;
		return (BOOL)( p->subclass == 0x80 && ( p->class_code == 0x0c || p->class_code == 0x11 ) );
	}
	return FALSE;
}

/*
 * The controller at a PCI device and function, set up the first time it
 * is asked for, at the speed hz of the device that asks; a device that
 * asks for less later slows it down, as the slowest device on a bus
 * sets its speed
 */
EXPORT T_I2CBUS *knl_dwi2c_bus( UW adr, void *acpi, UW hz )
{
	T_PCIDEV	p;
	T_CMTX		cm;
	DWI2C		*d;
	INT		i, d0, ps0;
	UW		caps, was;

	if ( hz == 0 ) hz = 100000;
	for ( i = 0; i < ndw; i++ ) {
		if ( dw[i].adr == adr ) {
			d = &dw[i];
			if ( hz < d->hz ) {
				tk_loc_mtx(d->mtx, TMO_FEVR);
				set_speed(d, hz);
				tk_unl_mtx(d->mtx);
			}
			return &d->bus;
		}
	}
	if ( ndw >= DW_MAX ) return NULL;
	if ( !lpss_find(adr, &p) ) {
		tm_printf((UB *)"i2c: no LPSS function at %d.%d\n", adr >> 16, adr & 0xffff);
		return NULL;
	}
	d = &dw[ndw];
	knl_memset(d, 0, sizeof(*d));
	d->acpi = acpi;
	(void)tm_sprintf(d->bus.name, (UB *)"i2c%d.%d", p.dev, p.fn);
	ps0 = ( acpi != NULL ) ? knl_acpica_call(acpi, "_PS0") : -1;
	d0 = to_d0(&p);
	ts_pcie_enable(&p);
	d->base = ts_pcie_bar_base(&p, 0);
	if ( d->base == 0 || p.bar_size[0] < 0x1000 ) {
		tm_printf((UB *)"i2c: %s %04x: no memory window\n", d->bus.name, p.device);
		return NULL;
	}
	caps = rd(d, LPSS_CAPS);
	was = rd(d, LPSS_RESETS);
	tm_printf((UB *)"i2c: %s %04x class %02x.%02x: _PS0 %s, found in D%d, LPSS caps %08x, resets %x\n",
		  d->bus.name, p.device, p.class_code, p.subclass, ( ps0 == 0 ) ? "run" : ( ps0 == -1 ? "none" : "failed" ),
		  d0 < 0 ? 0 : d0, caps, was);
	if ( caps == 0xffffffff || LPSS_CAPS_TYPE(caps) != LPSS_TYPE_I2C ) {
		tm_printf((UB *)"i2c: %s is not an I2C controller of the LPSS\n", d->bus.name);
		return NULL;
	}
	/* the firmware's counts, when it had the controller running */
	if ( ( was & LPSS_RESETS_FUNC ) == LPSS_RESETS_FUNC && rd(d, IC_COMP_TYPE) == COMP_TYPE_DW ) {
		d->fw[0] = rd(d, IC_SS_SCL_HCNT);
		d->fw[1] = rd(d, IC_SS_SCL_LCNT);
		d->fw[2] = rd(d, IC_FS_SCL_HCNT);
		d->fw[3] = rd(d, IC_FS_SCL_LCNT);
		d->fw[4] = ( rd(d, IC_COMP_VERSION) >= COMP_VER_SDA_HOLD ) ? rd(d, IC_SDA_HOLD) : 0;
	}
	/* in reset, then out of it with its DMA */
	wr(d, LPSS_RESETS, 0);
	WaitUsec(10);
	wr(d, LPSS_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);
	WaitUsec(100);
	if ( rd(d, IC_COMP_TYPE) != COMP_TYPE_DW ) {
		tm_printf((UB *)"i2c: %s is not a DesignWare controller (%08x)\n", d->bus.name, rd(d, IC_COMP_TYPE));
		return NULL;
	}
	d->adr = adr;
	d->ver = rd(d, IC_COMP_VERSION);
	d->fifo = ( ( rd(d, IC_COMP_PARAM_1) >> 16 ) & 0xff ) + 1;
	if ( d->fifo < 2 || d->fifo > 256 ) d->fifo = 16;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	d->mtx = tk_cre_mtx(&cm);
	if ( d->mtx <= 0 ) return NULL;
	set_speed(d, hz);
	d->bus.ctx = d;
	d->bus.xfer = dw_xfer;
	tm_printf((UB *)"TessronOS: i2c %04x:%04x at %d:%d.%d, version %08x, FIFO %d\n", p.vendor, p.device,
		  p.bus, p.dev, p.fn, d->ver, d->fifo);
	ndw++;
	return &d->bus;
}

/*
 * After S3 each controller is as from reset: in D0 by its ACPI method, out
 * of the LPSS's reset, its speed set again. Nothing to do when power stayed
 * on (S0 idle): these functions are left out of D3hot.
 */
LOCAL ER dw_pm_suspend( void *arg, INT how )
{
	INT	i;

	for ( i = 0; i < ndw; i++ ) tk_loc_mtx(dw[i].mtx, TMO_FEVR);
	return E_OK;
}

LOCAL void dw_pm_resume( void *arg, INT how )
{
	INT	i;

	for ( i = 0; i < ndw; i++ ) {
		DWI2C	*d = &dw[i];

		if ( how == PM_S3 ) {
			if ( d->acpi != NULL ) (void)knl_acpica_call(d->acpi, "_PS0");
			wr(d, LPSS_RESETS, 0);
			WaitUsec(10);
			wr(d, LPSS_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);
			WaitUsec(100);
			set_speed(d, d->hz);
		}
		tk_unl_mtx(d->mtx);
	}
}

LOCAL CONST T_PMDEV	dw_pm = { "i2c", PM_ORD_INPUT, dw_pm_suspend, dw_pm_resume, NULL };

/* The functions that may be LPSS I2C controllers (what is used of them is set up when an I2C device asks) */
EXPORT INT knl_dwi2c_init( void )
{
	T_PCIDEV	p;
	INT		i, n = 0, all = ts_pcie_count();

	(void)ts_pm_register(&dw_pm);
	for ( i = 0; i < all; i++ ) {
		if ( ts_pcie_get(i, &p) == E_OK && p.vendor == 0x8086 && p.bus == 0 && p.subclass == 0x80
		  && ( p.class_code == 0x0c || p.class_code == 0x11 ) ) {
			n++;
		}
	}
	return n;
}

#endif /* X64 */
