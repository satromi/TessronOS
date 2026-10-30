/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tpanel.c (rpi4)
 *	A touch screen on the 40 pin header: a Goodix GT911 on two pins
 *	driven as an I2C bus (device/i2c/i2c_gpio.c, gt911.c), its INT on a
 *	third (design 6.4.2; docs/tessronos/03-platform.md 3.6.7).
 *
 *	The panels this is for are wired in one of two ways, as their
 *	makers' device tree overlays say:
 *
 *	- the PocketTerm35's 3.5inch HDMI LCD (E): the screen is on HDMI,
 *	  the touch on the pins of I2C1, GPIO 2 (SDA, pin 3) and GPIO 3
 *	  (SCL, pin 5), INT on GPIO 4 (pin 7), at 0x14.
 *	- the 3.5inch DPI LCD: the screen on the DPI pins, the touch on
 *	  GPIO 10 (SDA, pin 19) and GPIO 11 (SCL, pin 23), INT on GPIO 27
 *	  (pin 13), at 0x14 or 0x5D, and its X reversed.
 *
 *	A wiring is tried only when each of its pins is an input, or has the
 *	bus function of those pins (I2C1 on 2 and 3, SPI0 on 10 and 11): a
 *	pin the firmware gave to the screen (DPI is alternate 2) or made an
 *	output is another's, and setting it would take the screen away. The
 *	DPI screen takes GPIO 2 and 3, so with it the first wiring is never
 *	tried; without it, the pins of the second are idle and are tried
 *	after the first. A controller is looked for at both addresses; one
 *	found has its three pins taken for the kernel (gpio_claim). Where none
 *	answers, the pins are left inputs, with the pulls they had.
 *
 *	The pins are open drain: a line is pulled low by making its pin an
 *	output (its latch is 0) and let go by making it an input; the
 *	board's resistors, and the pin's own pull-up set here, pull it high.
 *	The clock is about 100 kHz.
 *
 *	make TPANEL=0 leaves the pins alone.
 */

#include <sys/machine.h>

#ifdef RPI4

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/gpio.h>
#include "../../../device/i2c/i2cbus.h"
#include "../../../device/i2c/gt911.h"

#ifndef CNF_RPI_TPANEL
#define CNF_RPI_TPANEL	1
#endif

#define HALF_US		4		/* half a period of the clock */
#define STRETCH_US	2000

typedef struct {
	CONST char	*what;
	UINT		sda, scl, irq;
	UINT		bus_alt;	/* the alternate function of the bus pins that is their bus's */
	BOOL		invert_x;
	UB		name[16];
	/* while it is used */
	T_I2CGPIO	g;
	INT		gd;
	INT		pull[2];
} WIRING;

LOCAL WIRING	wiring[] = {
	{ .what = "3.5inch HDMI LCD (E), PocketTerm35", .sda = 2, .scl = 3, .irq = 4, .bus_alt = 0,
	  .invert_x = FALSE, .name = "gt911", .gd = -1 },
	{ .what = "3.5inch DPI LCD", .sda = 10, .scl = 11, .irq = 27, .bus_alt = 0,
	  .invert_x = TRUE, .name = "gt911", .gd = -1 },
};

#define TP_DIAG10	889		/* both are 3.5 inch panels: their diagonal, 0.1 mm */

#define NWIRING		( (INT)( sizeof(wiring) / sizeof(wiring[0]) ) )

LOCAL void pin_line( void *ctx, INT which, BOOL low )
{
	WIRING	*w = (WIRING *)ctx;
	UINT	pin = ( which == I2CGPIO_SDA ) ? w->sda : w->scl;

	(void)gpio_set_dir(0, pin, low ? GPIO_DIR_OUT : GPIO_DIR_IN);
}

LOCAL INT pin_sense( void *ctx, INT which )
{
	WIRING	*w = (WIRING *)ctx;

	return gpio_read(0, ( which == I2CGPIO_SDA ) ? w->sda : w->scl) > 0 ? 1 : 0;
}

LOCAL void int_came( UINT pin, void *ctx )
{
	(void)pin;
	knl_gt911_irq(((WIRING *)ctx)->gd);
}

/* The controller's trigger of INT as the pin's detection */
LOCAL ER int_arm( void *ctx, INT trig, INT gd )
{
	WIRING	*w = (WIRING *)ctx;
	INT	mode = ( trig == GT911_TRIG_RISING ) ? GPIO_IRQ_RISING
		     : ( trig == GT911_TRIG_FALLING ) ? GPIO_IRQ_FALLING
		     : ( trig == GT911_TRIG_LOW ) ? GPIO_IRQ_LOW : GPIO_IRQ_HIGH;

	w->gd = gd;
	return gpio_irq_set(0, w->irq, mode, int_came, w);
}

/* Whether a pin is free for the wiring: an input, or alt its bus function (-1 none) */
LOCAL BOOL pin_free( UINT pin, INT alt, INT *p_dir )
{
	INT	d = gpio_get_dir(0, pin);

	*p_dir = d;
	return ( d == GPIO_DIR_IN || ( alt >= 0 && d == 2 + alt ) );
}

LOCAL BOOL try_wiring( WIRING *w )
{
	T_I2CGPIO_PINS	p;
	T_GT911CONF	c;
	T_I2CBUS	*bus;
	INT		d_sda, d_scl, d_irq, gd;

	if ( !pin_free(w->sda, (INT)w->bus_alt, &d_sda) || !pin_free(w->scl, (INT)w->bus_alt, &d_scl)
	  || !pin_free(w->irq, -1, &d_irq) ) {
		tm_printf((UB *)"tpanel: %s: GPIO %d, %d, %d are %d, %d, %d (0 input, 1 output, 2+ alternate): in use, not tried\n",
			  w->what, w->sda, w->scl, w->irq, d_sda, d_scl, d_irq);
		return FALSE;
	}
	w->pull[0] = gpio_get_pull(0, w->sda);
	w->pull[1] = gpio_get_pull(0, w->scl);
	(void)gpio_set_dir(0, w->sda, GPIO_DIR_IN);
	(void)gpio_set_dir(0, w->scl, GPIO_DIR_IN);
	(void)gpio_set_dir(0, w->irq, GPIO_DIR_IN);
	(void)gpio_set_pull(0, w->sda, GPIO_PULL_UP);
	(void)gpio_set_pull(0, w->scl, GPIO_PULL_UP);
	(void)gpio_write(0, w->sda, 0);			/* the latch an output pulls low with */
	(void)gpio_write(0, w->scl, 0);

	knl_memset(&p, 0, sizeof(p));
	p.ctx = w;
	p.line = pin_line;
	p.sense = pin_sense;
	p.half_us = HALF_US;
	p.stretch_us = STRETCH_US;
	bus = knl_i2cgpio_init(&w->g, &p, (CONST UB *)( ( w->sda == 2 ) ? "i2c-gpio2" : "i2c-gpio10" ));
	gd = E_NOEXS;
	if ( bus != NULL ) {
		knl_memset(&c, 0, sizeof(c));
		c.bus = bus;
		c.addr = 0;
		c.invert_x = w->invert_x;
		c.diag10 = TP_DIAG10;
		c.irq_arm = int_arm;
		c.irq_ctx = w;
		c.name = w->name;
		gd = knl_gt911_attach(&c);
	}
	if ( gd < 0 ) {
		tm_printf((UB *)"tpanel: %s: no controller on GPIO %d and %d (%d)\n", w->what, w->sda, w->scl, gd);
		if ( w->pull[0] >= 0 ) (void)gpio_set_pull(0, w->sda, (UINT)w->pull[0]);
		if ( w->pull[1] >= 0 ) (void)gpio_set_pull(0, w->scl, (UINT)w->pull[1]);
		if ( w->g.mtx > 0 ) {
			(void)tk_del_mtx(w->g.mtx);
			w->g.mtx = 0;
		}
		return FALSE;
	}
	w->gd = gd;
	(void)gpio_claim(0, w->sda);
	(void)gpio_claim(0, w->scl);
	(void)gpio_claim(0, w->irq);
	tm_printf((UB *)"tpanel: %s: GPIO %d (SDA), %d (SCL), %d (INT)\n", w->what, w->sda, w->scl, w->irq);
	return TRUE;
}

/* The touch screen found and attached: how many (0 or 1) */
EXPORT INT knl_tpanel_init( void )
{
	INT	i;

	if ( !CNF_RPI_TPANEL ) {
		return 0;
	}
	for ( i = 0; i < NWIRING; i++ ) {
		if ( try_wiring(&wiring[i]) ) {
			return 1;
		}
	}
	return 0;
}

#endif /* RPI4 */
