/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gt911.h
 *	The Goodix GT911 and the touch screen controllers of its family
 *	(gt911.c), as the board's driver finds and attaches one.
 */

#ifndef _DEVICE_I2C_GT911_
#define _DEVICE_I2C_GT911_

#include "i2cbus.h"

#define GT911_ADDR_A	0x5D		/* INT low while the reset ends (the controller's own default) */
#define GT911_ADDR_B	0x14		/* INT high while the reset ends */

/* The trigger of the INT line the controller is set to (its configuration, 0x804D bits 1-0) */
#define GT911_TRIG_RISING	0
#define GT911_TRIG_FALLING	1
#define GT911_TRIG_LOW		2
#define GT911_TRIG_HIGH		3

typedef struct {
	T_I2CBUS	*bus;
	UH		addr;		/* GT911_ADDR_*; 0 tries both */
	/*
	 * How the board mounts the panel: the controller's own X and Y
	 * reversed (before they are matched to the screen, which swaps
	 * them when the controller's longer side is not the screen's)
	 */
	BOOL		invert_x, invert_y;
	/*
	 * The INT line made to call knl_gt911_irq for the trigger, from the
	 * interrupt handler; again after each read, for a level is to be
	 * looked at once more only after the read. NULL, or an error from
	 * it: the controller is read every period ms instead.
	 */
	ER		(*irq_arm)( void *ctx, INT trig, INT gd );
	void		*irq_ctx;
	INT		period;		/* ms; 0 the default */
	CONST UB	*name;		/* its device object, "gt911" */
	INT		diag10;		/* the panel's diagonal, 0.1 mm; 0 not known */
} T_GT911CONF;

typedef struct {
	UH		addr;
	UB		id[5];		/* the product, "911" */
	UH		fw;		/* its firmware's version */
	UB		cfg_ver;	/* its configuration's version */
	INT		x_max, y_max;	/* the controller's own range, from its configuration */
	INT		touches;	/* the contacts it reports at most */
	INT		trig;		/* GT911_TRIG_* */
	BOOL		swapped;	/* its X is the screen's Y */
	BOOL		polled;		/* read every period ms */
	INT		hd;		/* the input layer's handle */
	UD		frames, ints, errors;
} T_GT911INFO;

/*
 * The controller of c found (its product read at 0x8140), its
 * configuration read and the touch screen given to the input layer,
 * read by a task of its own. Answers its number (from 0) for
 * knl_gt911_irq and knl_gt911_info, or an error: E_NOEXS when no
 * controller of the family answers.
 */
IMPORT INT	knl_gt911_attach( CONST T_GT911CONF *c );
IMPORT void	knl_gt911_irq( INT gd );		/* from the interrupt handler */
IMPORT ER	knl_gt911_info( INT gd, T_GT911INFO *info );

#endif /* _DEVICE_I2C_GT911_ */
