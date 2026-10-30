/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	i2cbus.h
 *	An I2C bus as the devices on it see it: one transfer, bytes written
 *	to the device at addr and then, after a repeated start, bytes read
 *	from it (either may be none). A bus is a controller of the chip (the
 *	PC's LPSS controllers, device/i2c/x86_64/dw_i2c.c) or two pins driven
 *	from software (i2c_gpio.c); a test gives one of its own.
 */

#ifndef _DEVICE_I2C_I2CBUS_
#define _DEVICE_I2C_I2CBUS_

typedef struct {
	void		*ctx;
	ER		(*xfer)( void *ctx, UH addr, CONST UB *w, INT wn, UB *r, INT rn );
	UB		name[16];
} T_I2CBUS;

/*
 * A bus on two pins driven from software. Each line is open drain: it is
 * pulled low, or let go and pulled up by its resistor, and its level is
 * read back, for a device holds SDA low to answer and may hold SCL low to
 * make the controller wait (clock stretching). half_us is half a clock
 * period; stretch_us the longest SCL is waited for.
 */
#define I2CGPIO_SDA	0
#define I2CGPIO_SCL	1

typedef struct {
	void		*ctx;
	void		(*line)( void *ctx, INT which, BOOL low );	/* pull it low, or let it go */
	INT		(*sense)( void *ctx, INT which );		/* its level: 0 or 1 */
	UW		half_us;
	UW		stretch_us;
} T_I2CGPIO_PINS;

typedef struct {
	T_I2CBUS	bus;
	T_I2CGPIO_PINS	p;
	ID		mtx;		/* one transfer at a time */
	UD		xfers, nacks, stuck;
} T_I2CGPIO;

/*
 * The bus of the pins p, called name, in g (kept by the caller): both
 * lines let go and the bus freed of a device left in the middle of a
 * byte. NULL when it cannot be made.
 */
IMPORT T_I2CBUS	*knl_i2cgpio_init( T_I2CGPIO *g, CONST T_I2CGPIO_PINS *p, CONST UB *name );

#endif /* _DEVICE_I2C_I2CBUS_ */
