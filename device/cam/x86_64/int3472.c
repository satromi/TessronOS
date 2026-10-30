/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	int3472.c (x64 PC)
 *	A camera sensor's power controller, the ACPI device INT3472 a
 *	sensor's _DEP names (camx.h).
 *
 *	Its CLDB (a buffer of 32 bytes) says what it is: byte 1 is the kind,
 *	1 for discrete (lines of the chipset's GPIO switch the sensor's
 *	reset, its power, its clock and the light beside it), 2 and above for
 *	a TPS68470 PMIC on I2C (regulators and a clock of its own); byte 2 is
 *	the controller's number, which the sensor's SSDB names too.
 *
 *	A discrete one has a GpioIo in its _CRS for each line, and a _DSM
 *	(79234640-9E10-4FEA-A5C1-B5AA8B19756F) that tells, for the line of
 *	index i, at function i + 2: its kind in bits 7:0 (0 reset, 1
 *	powerdown, 2 strobe, 8 digital power, 0x0B power enable (analogue),
 *	0x0C clock enable, 0x0D privacy LED, 0x10 I/O power, 0x12
 *	handshake), its pin again in bits 15:8 and the level that has
 *	the sensor on in bits 31:24 (non-zero high, else low): out of reset,
 *	out of powerdown, powered, clocked, lit. Function 1 gives how many
 *	lines there are. The polarity of the GpioIo itself is not used.
 *
 *	The sensor is powered in this order: reset and powerdown held (their
 *	level for off), the clock on, the supplies on in the sensor's order
 *	(analogue, I/O, digital) with 2 ms for each to rise (200 ms for a
 *	handshake line, behind which a controller powers the sensor), the
 *	light on,
 *	reset and powerdown let go, and 7.5 ms before the sensor is spoken
 *	to (its data sheet asks for at least 5 ms after the reset ends, with
 *	the clock running). Off is the other way: reset and powerdown held,
 *	the power, the clock and the light off.
 *
 *	A TPS68470 is found and its revision read, and said; its regulators
 *	and clock are not driven here, so a sensor behind one is not powered.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "../../../kernel/sysdepend/acpi/acpi.h"
#include "../../gpio/x86_64/gpio_pch.h"
#include "camx.h"

#define TPS68470_REVID		0xff

/* 79234640-9E10-4FEA-A5C1-B5AA8B19756F, as ACPI lays out a UUID */
LOCAL CONST UB	gpio_dsm[16] = {
	0x40, 0x46, 0x23, 0x79, 0x10, 0x9e, 0xea, 0x4f, 0xa5, 0xc1, 0xb5, 0xaa, 0x8b, 0x19, 0x75, 0x6f
};

LOCAL CONST char *kind_name( UB t )
{
	switch ( t ) {
	case CAMGP_RESET:		return "reset";
	case CAMGP_POWERDOWN:		return "powerdown";
	case CAMGP_POWER_ENABLE:	return "power enable";
	case CAMGP_CLK_ENABLE:		return "clock enable";
	case CAMGP_PRIVACY_LED:		return "privacy LED";
	case CAMGP_HANDSHAKE:		return "handshake";
	case CAMGP_STROBE:		return "strobe";
	case CAMGP_POWER1:		return "power (digital)";
	case CAMGP_DOVDD:		return "power (I/O)";
	default:			return "unknown";
	}
}

/* The TPS68470 behind the controller: its bus from _CRS, its revision read */
LOCAL ER find_pmic( T_CAMPWR *p )
{
	unsigned int		addr = 0, hz = 0;
	unsigned long long	adr = 0;
	char			ctrl[64];
	UB			reg = TPS68470_REVID, rev = 0;
	ER			er;

	if ( knl_acpica_i2c(p->dev, &addr, &hz, ctrl, sizeof(ctrl), NULL, NULL, 0, NULL, NULL) != 0
	  || knl_acpica_int(knl_acpica_handle(ctrl), "_ADR", &adr) != 0 ) {
		tm_printf((UB *)"campwr: %s: a TPS68470 with no I2C connection\n", p->path);
		return E_NOEXS;
	}
	p->pmic = knl_dwi2c_bus((UW)adr, knl_acpica_handle(ctrl), hz);
	p->pmic_addr = (UH)addr;
	if ( p->pmic == NULL ) return E_NOEXS;
	er = p->pmic->xfer(p->pmic->ctx, p->pmic_addr, &reg, 1, &rev, 1);
	tm_printf((UB *)"campwr: %s: TPS68470 at %02x on %s (%d Hz): revision %02x (%d); "
		  "its regulators and clock are not driven: the sensor behind it is not powered\n",
		  p->path, addr, ctrl, hz, rev, (INT)er);
	return E_NOSPT;
}

EXPORT ER knl_campwr_find( T_CAMPWR *p, CONST char *path )
{
	unsigned long long	n = 0, v;
	char			line[3 * 32 + 1];
	INT			i, len, m;

	knl_memset(p, 0, sizeof(*p));
	knl_strncpy(p->path, path, sizeof(p->path) - 1);
	p->dev = knl_acpica_handle(path);
	if ( p->dev == NULL ) return E_NOEXS;
	len = knl_acpica_buf(p->dev, "CLDB", p->cldb, sizeof(p->cldb));
	for ( i = 0, m = 0, line[0] = '\0'; i < len && i < (INT)sizeof(p->cldb); i++ ) {
		m += tm_sprintf((UB *)line + m, (UB *)" %02x", p->cldb[i]);
	}
	tm_printf((UB *)"campwr: %s: CLDB (%d bytes):%s\n", path, len, line);
	if ( len < 3 ) return E_NOEXS;
	tm_printf((UB *)"campwr: %s: version %d, kind %d (%s), controller %d, sensor card %d\n", path,
		  p->cldb[0], p->cldb[1], ( p->cldb[1] == CAMPWR_DISCRETE ) ? "discrete" : ( p->cldb[1] >= 2 ? "TPS68470" : "?" ),
		  p->cldb[2], p->cldb[3]);
	if ( p->cldb[1] >= CAMPWR_TPS68470 ) {
		p->kind = CAMPWR_TPS68470;
		return find_pmic(p);
	}
	if ( p->cldb[1] != CAMPWR_DISCRETE ) return E_NOSPT;
	p->kind = CAMPWR_DISCRETE;

	if ( knl_acpica_dsm_int(p->dev, gpio_dsm, 0, 1, &n) != 0 ) n = 0;
	for ( i = 0; i < CAMPWR_GPIO_MAX; i++ ) {
		T_CAMGPIO	*g = &p->gpio[i];
		unsigned int	pin = 0;
		int		flags = 0;

		if ( knl_acpica_gpio(p->dev, i, &pin, g->ctrl, sizeof(g->ctrl), &flags) != 0 ) break;
		v = 0;
		if ( knl_acpica_dsm_int(p->dev, gpio_dsm, 0, (unsigned int)( i + 2 ), &v) != 0 ) {
			tm_printf((UB *)"campwr: %s: line %d (pin %d) has no _DSM entry\n", path, i, pin);
			break;
		}
		g->pin = (UH)pin;
		g->dsm = (UW)v;
		g->type = (UB)( v & 0xff );
		g->on = ( ( v >> 24 ) & 0xff ) != 0 ? 1 : 0;
		tm_printf((UB *)"campwr: %s: line %d: pin %d on %s (_CRS: %s%s), _DSM %08x: %s, on at %s%s\n",
			  path, i, pin, g->ctrl, ( flags & 1 ) ? "active low" : "active high",
			  ( flags & 2 ) ? ", output" : "", g->dsm, kind_name(g->type), g->on ? "high" : "low",
			  ( ( v >> 8 ) & 0xff ) != ( pin & 0xff ) ? " (the _DSM's pin differs)" : "");
		p->ngpio++;
	}
	tm_printf((UB *)"campwr: %s: %d lines, the _DSM says %d\n", path, p->ngpio, (INT)n);
	return ( p->ngpio > 0 ) ? E_OK : E_NOEXS;
}

/* A line of the kind to the level for on (or off), when there is one; whether there was */
LOCAL BOOL drive( T_CAMPWR *p, UB type, BOOL on )
{
	INT	i;
	BOOL	had = FALSE;

	for ( i = 0; i < p->ngpio; i++ ) {
		T_CAMGPIO	*g = &p->gpio[i];
		BOOL		lv;

		if ( g->type != type ) continue;
		lv = on ? ( g->on != 0 ) : ( g->on == 0 );
		if ( p->swap_rst && ( type == CAMGP_RESET || type == CAMGP_POWERDOWN ) ) lv = !lv;
		(void)knl_pchgpio_out(g->ctrl, g->pin, lv);
		had = TRUE;
	}
	return had;
}

EXPORT ER knl_campwr_on( T_CAMPWR *p )
{
	if ( p->kind != CAMPWR_DISCRETE ) return E_NOSPT;
	(void)drive(p, CAMGP_RESET, FALSE);
	(void)drive(p, CAMGP_POWERDOWN, FALSE);
	(void)drive(p, CAMGP_CLK_ENABLE, TRUE);
	/* the supplies in the sensor's order (analogue, I/O, digital), each given 2 ms to rise */
	if ( drive(p, CAMGP_POWER_ENABLE, TRUE) ) tk_dly_tsk(2);
	if ( drive(p, CAMGP_DOVDD, TRUE) ) tk_dly_tsk(2);
	if ( drive(p, CAMGP_POWER1, TRUE) ) tk_dly_tsk(2);
	if ( drive(p, CAMGP_HANDSHAKE, TRUE) ) tk_dly_tsk(200);
	(void)drive(p, CAMGP_PRIVACY_LED, TRUE);
	tk_dly_tsk(1);
	(void)drive(p, CAMGP_RESET, TRUE);
	(void)drive(p, CAMGP_POWERDOWN, TRUE);
	WaitUsec(7500);
	p->on = TRUE;
	return E_OK;
}

EXPORT void knl_campwr_off( T_CAMPWR *p )
{
	if ( p->kind != CAMPWR_DISCRETE ) return;
	(void)drive(p, CAMGP_RESET, FALSE);
	(void)drive(p, CAMGP_POWERDOWN, FALSE);
	(void)drive(p, CAMGP_HANDSHAKE, FALSE);
	(void)drive(p, CAMGP_POWER1, FALSE);
	(void)drive(p, CAMGP_DOVDD, FALSE);
	(void)drive(p, CAMGP_POWER_ENABLE, FALSE);
	(void)drive(p, CAMGP_CLK_ENABLE, FALSE);
	(void)drive(p, CAMGP_PRIVACY_LED, FALSE);
	p->on = FALSE;
}

EXPORT void knl_campwr_swap( T_CAMPWR *p )
{
	p->swap_rst = !p->swap_rst;
	tm_printf((UB *)"campwr: %s: reset and powerdown now driven %s the _DSM's levels\n", p->path,
		  p->swap_rst ? "against" : "as");
}

EXPORT void knl_campwr_say( T_CAMPWR *p, CONST char *when )
{
	INT	i;
	char	what[64];

	for ( i = 0; i < p->ngpio; i++ ) {
		(void)tm_sprintf((UB *)what, (UB *)"%s, %s", when, kind_name(p->gpio[i].type));
		knl_pchgpio_say(p->gpio[i].ctrl, p->gpio[i].pin, what);
	}
}

#endif /* X64 */
