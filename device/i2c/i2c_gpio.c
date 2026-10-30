/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	i2c_gpio.c
 *	An I2C bus on two general purpose pins, driven from software: the
 *	controller's side of the protocol, a bit at a time (design 6.4.2).
 *
 *	Both lines are open drain. A line is pulled low by making its pin an
 *	output at 0 and let go by making it an input, when its resistor pulls
 *	it up; the board's driver (T_I2CGPIO_PINS) does that. SDA changes only
 *	while SCL is low, but for a start (SDA falling while SCL is high) and
 *	a stop (SDA rising while SCL is high). After SCL is let go its level
 *	is read until it is high, for a device may hold it low until it is
 *	ready (clock stretching); one that holds it longer than stretch_us
 *	ends the transfer.
 *
 *	A transfer is a start, the address with the write bit and the bytes
 *	written, then a repeated start, the address with the read bit and the
 *	bytes read, each acknowledged by the controller but the last, and a
 *	stop. A byte the device does not acknowledge (its address, when
 *	nothing is there) ends the transfer with a stop and E_IO.
 *
 *	A device can be left in the middle of a byte it was sending (the
 *	controller restarted, or a transfer cut short), holding SDA low; no
 *	start can then be made. Before the first transfer, after one that
 *	failed, and when a start finds SDA held, SCL is clocked up to nine
 *	times while SDA is low, until the device lets it go, and a stop is
 *	made (I2C specification, 3.1.16).
 *
 *	One transfer at a time, under the bus's mutex. A task that is
 *	preempted in the middle only makes the clock slower, which the bus
 *	allows: its timing is the controller's.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "i2cbus.h"

#define BUS_CLEAR_CLOCKS	9

LOCAL void hold( T_I2CGPIO *g )
{
	if ( g->p.half_us > 0 ) {
		WaitUsec(g->p.half_us);
	}
}

LOCAL void sda( T_I2CGPIO *g, BOOL low )
{
	g->p.line(g->p.ctx, I2CGPIO_SDA, low);
}

LOCAL void scl_low( T_I2CGPIO *g )
{
	g->p.line(g->p.ctx, I2CGPIO_SCL, TRUE);
}

/* SCL let go and seen high: FALSE when a device holds it low too long */
LOCAL BOOL scl_high( T_I2CGPIO *g )
{
	UW	t;

	g->p.line(g->p.ctx, I2CGPIO_SCL, FALSE);
	for ( t = 0; g->p.sense(g->p.ctx, I2CGPIO_SCL) == 0; t++ ) {
		if ( t >= g->p.stretch_us ) {
			return FALSE;
		}
		if ( g->p.half_us > 0 ) {
			WaitUsec(1);
		}
	}
	return TRUE;
}

/* From both lines high: SDA falls, then SCL */
LOCAL BOOL start( T_I2CGPIO *g )
{
	sda(g, FALSE);
	if ( !scl_high(g) ) return FALSE;
	hold(g);
	if ( g->p.sense(g->p.ctx, I2CGPIO_SDA) == 0 ) {
		return FALSE;			/* a device holds SDA */
	}
	sda(g, TRUE);
	hold(g);
	scl_low(g);
	hold(g);
	return TRUE;
}

/* SDA low while SCL is low, then SCL up and SDA up */
LOCAL void stop( T_I2CGPIO *g )
{
	sda(g, TRUE);
	hold(g);
	(void)scl_high(g);
	hold(g);
	sda(g, FALSE);
	hold(g);
}

/* One bit out: SDA set while SCL is low, held while SCL is high */
LOCAL BOOL put_bit( T_I2CGPIO *g, BOOL one )
{
	sda(g, !one);
	hold(g);
	if ( !scl_high(g) ) return FALSE;
	hold(g);
	scl_low(g);
	return TRUE;
}

/* One bit in: SDA let go, read while SCL is high; -1 when SCL is held */
LOCAL INT get_bit( T_I2CGPIO *g )
{
	INT	b;

	sda(g, FALSE);
	hold(g);
	if ( !scl_high(g) ) return -1;
	hold(g);
	b = g->p.sense(g->p.ctx, I2CGPIO_SDA);
	scl_low(g);
	return b;
}

/* A byte out, the high bit first: E_OK acknowledged, E_IO not, E_TMOUT the clock held */
LOCAL ER put_byte( T_I2CGPIO *g, UB v )
{
	INT	i, ack;

	for ( i = 7; i >= 0; i-- ) {
		if ( !put_bit(g, ( v >> i ) & 1) ) return E_TMOUT;
	}
	ack = get_bit(g);
	if ( ack < 0 ) return E_TMOUT;
	return ( ack == 0 ) ? E_OK : E_IO;
}

/* A byte in, and the acknowledge after it (none after the last) */
LOCAL INT get_byte( T_I2CGPIO *g, BOOL ack )
{
	INT	i, b, v = 0;

	for ( i = 0; i < 8; i++ ) {
		b = get_bit(g);
		if ( b < 0 ) return -1;
		v = ( v << 1 ) | b;
	}
	if ( !put_bit(g, !ack) ) return -1;
	return v;
}

/*
 * A device left sending holds SDA low: SCL clocked until it lets go, and
 * a stop. Answers whether both lines are high after.
 */
LOCAL BOOL bus_clear( T_I2CGPIO *g )
{
	INT	i;

	sda(g, FALSE);
	(void)scl_high(g);
	hold(g);
	for ( i = 0; i < BUS_CLEAR_CLOCKS && g->p.sense(g->p.ctx, I2CGPIO_SDA) == 0; i++ ) {
		scl_low(g);
		hold(g);
		(void)scl_high(g);
		hold(g);
	}
	if ( i > 0 ) {
		g->stuck++;
		scl_low(g);
		hold(g);
		stop(g);
	}
	return ( g->p.sense(g->p.ctx, I2CGPIO_SDA) != 0 && g->p.sense(g->p.ctx, I2CGPIO_SCL) != 0 );
}

LOCAL ER xfer_locked( T_I2CGPIO *g, UH addr, CONST UB *w, INT wn, UB *r, INT rn )
{
	ER	er = E_OK;
	INT	i, v;

	if ( !start(g) && !( bus_clear(g) && start(g) ) ) {
		return E_TMOUT;
	}
	if ( wn > 0 || rn <= 0 ) {
		er = put_byte(g, (UB)( addr << 1 ));
		for ( i = 0; er == E_OK && i < wn; i++ ) {
			er = put_byte(g, w[i]);
		}
		if ( er != E_OK ) goto end;
		if ( rn > 0 ) {
			/* the repeated start: SDA up while SCL is low, then as a start */
			sda(g, FALSE);
			hold(g);
			if ( !start(g) ) {
				er = E_TMOUT;
				goto end;
			}
		}
	}
	if ( rn > 0 ) {
		er = put_byte(g, (UB)( ( addr << 1 ) | 1 ));
		for ( i = 0; er == E_OK && i < rn; i++ ) {
			v = get_byte(g, i + 1 < rn);
			if ( v < 0 ) {
				er = E_TMOUT;
			} else {
				r[i] = (UB)v;
			}
		}
	}
end:
	stop(g);
	return er;
}

LOCAL ER gpio_xfer( void *ctx, UH addr, CONST UB *w, INT wn, UB *r, INT rn )
{
	T_I2CGPIO	*g = (T_I2CGPIO *)ctx;
	ER		er;

	if ( addr > 0x7F || wn < 0 || rn < 0 || ( wn > 0 && w == NULL ) || ( rn > 0 && r == NULL ) ) {
		return E_PAR;
	}
	tk_loc_mtx(g->mtx, TMO_FEVR);
	g->xfers++;
	er = xfer_locked(g, addr, w, wn, r, rn);
	if ( er == E_IO ) {
		g->nacks++;
	} else if ( er < E_OK ) {
		(void)bus_clear(g);
	}
	tk_unl_mtx(g->mtx);
	return er;
}

EXPORT T_I2CBUS *knl_i2cgpio_init( T_I2CGPIO *g, CONST T_I2CGPIO_PINS *p, CONST UB *name )
{
	T_CMTX	cm;
	INT	i;

	knl_memset(g, 0, sizeof(*g));
	g->p = *p;
	if ( g->p.stretch_us == 0 ) {
		g->p.stretch_us = 1000;
	}
	knl_memset(&cm, 0, sizeof(cm));
	cm.mtxatr = TA_TFIFO;
	g->mtx = tk_cre_mtx(&cm);
	if ( g->mtx <= 0 ) {
		g->mtx = 0;
		return NULL;
	}
	for ( i = 0; i < 15 && name[i] != 0; i++ ) g->bus.name[i] = name[i];
	g->bus.ctx = g;
	g->bus.xfer = gpio_xfer;
	g->p.line(g->p.ctx, I2CGPIO_SDA, FALSE);
	g->p.line(g->p.ctx, I2CGPIO_SCL, FALSE);
	hold(g);
	if ( !bus_clear(g) ) {
		tm_printf((UB *)"i2c-gpio: %s: SDA %d, SCL %d with both let go: no pull-up, or a device holds them\n",
			  name, g->p.sense(g->p.ctx, I2CGPIO_SDA), g->p.sense(g->p.ctx, I2CGPIO_SCL));
		(void)tk_del_mtx(g->mtx);
		g->mtx = 0;
		return NULL;
	}
	return &g->bus;
}
