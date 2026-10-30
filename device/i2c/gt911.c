/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gt911.c
 *	The Goodix GT911 touch screen controller and those of its family
 *	(GT9110, GT912, GT927, GT928, GT967 ...) on an I2C bus (design 6.4.2).
 *
 *	The controller's registers have addresses of 16 bits, sent high byte
 *	first, and are read from where a write of the address left off. It
 *	answers at 0x5D or 0x14, as its INT line was when its reset ended;
 *	a board that does not drive its reset leaves it at the one the board
 *	strapped, so both are tried.
 *
 *	   0x8140	the product, four characters ("911" and a 0), then the
 *			firmware's version (two bytes, low first)
 *	   0x8047	the configuration: its version, then the range of X
 *			and of Y (two bytes each, low first), the contacts
 *			reported at most (bits 3-0), and Module_Switch1, whose
 *			bits 1-0 are the trigger of INT (rising, falling, low,
 *			high)
 *	   0x814E	the state of the coordinates: bit 7 a frame is ready,
 *			bits 3-0 how many contacts it has; then the contacts,
 *			eight bytes each from 0x814F: its tracking number
 *			(bits 3-0), X and Y (two bytes each, low first), its
 *			size (two bytes) and one reserved. Writing 0 to 0x814E
 *			gives the buffer back to the controller for the next
 *			frame.
 *
 *	A frame is read when INT says there is one: the state and the first
 *	contact in one read, the other contacts in a second when there are
 *	more. A frame the INT told of that is not ready yet is looked at
 *	again every millisecond for up to 20. Whatever was read, 0 is written
 *	back. Each contact is handed to the input layer by its tracking
 *	number, and the frame is ended (knl_hid_mt_contact, knl_hid_mt_sync):
 *	a contact the frame leaves out has let go. While it is touched the
 *	controller gives a frame each time it scans the panel, and one with
 *	no contact when the last finger lets go.
 *
 *	The INT line is the board's to deliver (T_GT911CONF.irq_arm). Without
 *	it, or when frames are found waiting by the read made after a second
 *	without one while no interrupt came, three times in a row, the
 *	controller is read every period ms instead.
 *
 *	The controller's X runs 0..x_max-1 and its Y 0..y_max-1 as its
 *	configuration says. They are matched to the screen: reversed as the
 *	board says it mounts the panel, then swapped when the controller's
 *	longer side is not the screen's (a panel of 480 by 640 under a
 *	screen of 640 by 480).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/disp.h>
#include "gt911.h"

IMPORT INT	knl_hid_mt_attach( CONST UB *name, INT xmax, INT ymax );
IMPORT ER	knl_hid_mt_size( INT hd, INT diag10 );
IMPORT void	knl_hid_mt_contact( INT hd, INT id, INT x, INT y );
IMPORT void	knl_hid_mt_sync( INT hd );
IMPORT void	knl_usb_lock( void );
IMPORT void	knl_usb_unlock( void );
IMPORT void	knl_obinput_sync( void ) __attribute__((weak));
IMPORT ER	ts_disp_ref( T_DISPSPEC *spec ) __attribute__((weak));

#define GT_MAX		2
#define GT_CONTACTS	10		/* the most the family reports */
#define GT_CONTACT_SIZE	8
#define GT_RANGE_MAX	4096		/* a range larger than this is not the configuration's */
#define GT_IDLE_MS	1000		/* with the interrupt: a read this long after the last one came, all the same */
#define GT_MISSED	3		/* frames found by those reads in a row: the interrupt is not arriving */
#define GT_READY_MS	20		/* a frame INT told of, waited for */
#define GT_PERIOD_MS	10

#define REG_ID		0x8140
#define REG_CONFIG	0x8047
#define REG_COORD	0x814E

#define ST_READY	0x80
#define ST_COUNT	0x0F

typedef struct {
	BOOL		used;
	T_GT911CONF	c;
	T_GT911INFO	i;
	INT		xr, yr;		/* the range across the screen and down it */
	ID		sem;
	INT		missed;
	UB		name[16];
} GT911;

LOCAL GT911	gt[GT_MAX];

/* n bytes of the registers from reg */
LOCAL ER reg_read( GT911 *g, UH addr, UH reg, UB *buf, INT n )
{
	UB	w[2];

	w[0] = (UB)( reg >> 8 );
	w[1] = (UB)reg;
	return g->c.bus->xfer(g->c.bus->ctx, addr, w, 2, buf, n);
}

LOCAL ER reg_write1( GT911 *g, UH reg, UB v )
{
	UB	w[3];

	w[0] = (UB)( reg >> 8 );
	w[1] = (UB)reg;
	w[2] = v;
	return g->c.bus->xfer(g->c.bus->ctx, g->i.addr, w, 3, NULL, 0);
}

/* The product at addr, when it is one of the family: four characters starting with '9' */
LOCAL BOOL probe( GT911 *g, UH addr )
{
	UB	b[6];
	INT	k;

	if ( reg_read(g, addr, REG_ID, b, 6) < E_OK ) {
		return FALSE;
	}
	if ( b[0] != '9' ) {
		tm_printf((UB *)"gt911: %s %02x: product %02x %02x %02x %02x, not of the family\n",
			  g->c.bus->name, addr, b[0], b[1], b[2], b[3]);
		return FALSE;
	}
	for ( k = 0; k < 4 && b[k] >= '0' && b[k] <= 'Z'; k++ ) g->i.id[k] = b[k];
	g->i.id[k] = 0;
	g->i.fw = (UH)( b[4] | ( b[5] << 8 ) );
	g->i.addr = addr;
	return TRUE;
}

/* The controller's range and contacts from its configuration; the screen's when it gives none */
LOCAL void read_config( GT911 *g )
{
	UB		b[7];
	T_DISPSPEC	spec;
	INT		sw = 0, sh = 0;

	if ( ts_disp_ref != NULL && ts_disp_ref(&spec) >= E_OK ) {
		sw = (INT)( ( spec.view_w > 0 ) ? spec.view_w : spec.width );
		sh = (INT)( ( spec.view_h > 0 ) ? spec.view_h : spec.height );
	}
	knl_memset(b, 0, sizeof(b));
	(void)reg_read(g, g->i.addr, REG_CONFIG, b, 7);
	g->i.cfg_ver = b[0];
	g->i.x_max = b[1] | ( b[2] << 8 );
	g->i.y_max = b[3] | ( b[4] << 8 );
	g->i.touches = b[5] & 0x0F;
	g->i.trig = b[6] & 0x03;
	if ( g->i.touches == 0 || g->i.touches > GT_CONTACTS ) {
		g->i.touches = GT_CONTACTS;
	}
	if ( g->i.x_max <= 0 || g->i.x_max > GT_RANGE_MAX || g->i.y_max <= 0 || g->i.y_max > GT_RANGE_MAX ) {
		tm_printf((UB *)"gt911: %s: no range in the configuration (%d by %d): the screen's\n",
			  g->name, g->i.x_max, g->i.y_max);
		g->i.x_max = ( sw > 0 ) ? sw : 640;
		g->i.y_max = ( sh > 0 ) ? sh : 480;
	}
	g->i.swapped = ( sw > 0 && sh > 0 && g->i.x_max != g->i.y_max && sw != sh
		      && ( g->i.x_max > g->i.y_max ) != ( sw > sh ) );
	g->xr = g->i.swapped ? g->i.y_max : g->i.x_max;
	g->yr = g->i.swapped ? g->i.x_max : g->i.y_max;
}

/* A contact's place on the screen's axes */
LOCAL void place( GT911 *g, INT *px, INT *py )
{
	INT	x = *px, y = *py, t;

	if ( x >= g->i.x_max ) x = g->i.x_max - 1;
	if ( y >= g->i.y_max ) y = g->i.y_max - 1;
	if ( g->c.invert_x ) x = g->i.x_max - 1 - x;
	if ( g->c.invert_y ) y = g->i.y_max - 1 - y;
	if ( g->i.swapped ) {
		t = x;
		x = y;
		y = t;
	}
	*px = x;
	*py = y;
}

/*
 * One frame read and handed on: 1 when there was one, 0 when none was
 * ready, -1 when the bus failed. told: INT said there is one, which is
 * waited for when it is not ready yet.
 */
LOCAL INT read_frame( GT911 *g, BOOL told )
{
	UB	b[1 + GT_CONTACT_SIZE * GT_CONTACTS];
	INT	n, k, t, x, y;
	ER	er;

	for ( t = 0; ; t++ ) {
		er = reg_read(g, g->i.addr, REG_COORD, b, 1 + GT_CONTACT_SIZE);
		if ( er < E_OK ) {
			g->i.errors++;
			return -1;
		}
		if ( ( b[0] & ST_READY ) || !told || t >= GT_READY_MS ) break;
		tk_dly_tsk(1);
	}
	if ( !( b[0] & ST_READY ) ) {
		if ( told ) (void)reg_write1(g, REG_COORD, 0);
		return 0;
	}
	n = b[0] & ST_COUNT;
	if ( n > g->i.touches ) {
		g->i.errors++;			/* not a frame of this controller's */
		(void)reg_write1(g, REG_COORD, 0);
		return 1;
	}
	if ( n > 1 && reg_read(g, g->i.addr, REG_COORD + 1 + GT_CONTACT_SIZE,
			       b + 1 + GT_CONTACT_SIZE, ( n - 1 ) * GT_CONTACT_SIZE) < E_OK ) {
		g->i.errors++;
		(void)reg_write1(g, REG_COORD, 0);
		return -1;
	}
	(void)reg_write1(g, REG_COORD, 0);
	g->i.frames++;
	knl_usb_lock();
	for ( k = 0; k < n; k++ ) {
		CONST UB	*p = b + 1 + k * GT_CONTACT_SIZE;

		x = p[1] | ( p[2] << 8 );
		y = p[3] | ( p[4] << 8 );
		place(g, &x, &y);
		knl_hid_mt_contact(g->i.hd, p[0] & 0x0F, x, y);
	}
	knl_hid_mt_sync(g->i.hd);
	knl_usb_unlock();
	return 1;
}

LOCAL BOOL arm( GT911 *g )
{
	return ( g->c.irq_arm != NULL && g->c.irq_arm(g->c.irq_ctx, g->i.trig, (INT)( g - gt )) >= E_OK );
}

EXPORT void knl_gt911_irq( INT gd )
{
	if ( gd >= 0 && gd < GT_MAX && gt[gd].used && gt[gd].sem > 0 ) {
		gt[gd].i.ints++;
		(void)tk_sig_sem(gt[gd].sem, 1);
	}
}

LOCAL void gt_task( INT stacd, void *exinf )
{
	GT911	*g = (GT911 *)exinf;
	ER	er;
	INT	got;

	(void)stacd;
	for ( ;; ) {
		if ( g->i.polled ) {
			tk_dly_tsk(g->c.period);
			(void)read_frame(g, FALSE);
			continue;
		}
		er = tk_wai_sem(g->sem, 1, GT_IDLE_MS);
		got = read_frame(g, er >= E_OK);
		g->missed = ( er == E_TMOUT && got > 0 ) ? g->missed + 1 : 0;
		if ( g->missed >= GT_MISSED || !arm(g) ) {
			g->i.polled = TRUE;
			tm_printf((UB *)"gt911: %s: frames wait with no interrupt coming (%d so far): read every %d ms\n",
				  g->name, (INT)g->i.ints, g->c.period);
		}
	}
}

EXPORT INT knl_gt911_attach( CONST T_GT911CONF *c )
{
	GT911	*g = NULL;
	T_CSEM	cs;
	T_CTSK	ct;
	ID	t;
	INT	k;

	for ( k = 0; k < GT_MAX; k++ ) {
		if ( !gt[k].used && gt[k].sem == 0 ) {
			g = &gt[k];
			break;
		}
	}
	if ( g == NULL ) return E_LIMIT;
	if ( c == NULL || c->bus == NULL ) return E_PAR;
	knl_memset(g, 0, sizeof(*g));
	g->c = *c;
	if ( g->c.period <= 0 ) g->c.period = GT_PERIOD_MS;
	for ( k = 0; k < 15 && c->name != NULL && c->name[k] != 0; k++ ) g->name[k] = c->name[k];
	g->name[k] = 0;
	if ( !( ( c->addr != GT911_ADDR_B && probe(g, GT911_ADDR_A) )
	     || ( c->addr != GT911_ADDR_A && probe(g, GT911_ADDR_B) ) ) ) {
		return E_NOEXS;
	}
	read_config(g);

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 0x7fffffff;
	g->sem = tk_cre_sem(&cs);
	if ( g->sem <= 0 ) {
		g->sem = 0;
		return E_LIMIT;
	}
	knl_usb_lock();
	g->i.hd = knl_hid_mt_attach(g->name, g->xr - 1, g->yr - 1);
	if ( g->i.hd > 0 && c->diag10 > 0 ) (void)knl_hid_mt_size(g->i.hd, c->diag10);
	knl_usb_unlock();
	if ( g->i.hd <= 0 ) {
		return g->i.hd;
	}
	(void)reg_write1(g, REG_COORD, 0);		/* a frame left from before: the buffer given back */
	g->used = TRUE;
	g->i.polled = !arm(g);
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = g;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)gt_task;
	ct.itskpri = 12;
	ct.stksz = 4 * 1024;
	t = tk_cre_tsk(&ct);
	if ( t <= 0 ) {
		g->used = FALSE;
		return E_LIMIT;
	}
	(void)tk_sta_tsk(t, 0);
	if ( knl_obinput_sync != NULL ) knl_obinput_sync();	/* its object, when the object layer is up already */
	tm_printf((UB *)"TessronOS: %s: Goodix GT%s (firmware %04x, configuration %d) on %s at %02x, %d by %d%s%s%s, "
		  "%d contacts, INT %s, %s %d%s\n",
		  g->name, g->i.id, g->i.fw, g->i.cfg_ver, c->bus->name, g->i.addr, g->i.x_max, g->i.y_max,
		  c->invert_x ? ", X reversed" : "", c->invert_y ? ", Y reversed" : "",
		  g->i.swapped ? ", X and Y swapped" : "", g->i.touches,
		  ( g->i.trig == GT911_TRIG_RISING ) ? "rising" : ( g->i.trig == GT911_TRIG_FALLING ) ? "falling"
		  : ( g->i.trig == GT911_TRIG_LOW ) ? "low" : "high",
		  g->i.polled ? "read every" : "interrupt, else read after", g->i.polled ? g->c.period : GT_IDLE_MS, " ms");
	return (INT)( g - gt );
}

EXPORT ER knl_gt911_info( INT gd, T_GT911INFO *info )
{
	if ( gd < 0 || gd >= GT_MAX || !gt[gd].used ) return E_PAR;
	*info = gt[gd].i;
	return E_OK;
}
