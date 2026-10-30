/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_tpanel.c
 *	A touch screen's controller on pins driven as an I2C bus
 *	(device/i2c/i2c_gpio.c, device/i2c/gt911.c, design 6.4.2).
 *
 *	The pins are made here: two open drain wires, each low when the
 *	controller's side (the code under test) or the device's pulls it,
 *	and a device on them that follows the wires a level at a time as a
 *	Goodix GT911 does -- it sees the start and the stop, takes its
 *	address and the bytes written, acknowledges, sends the bytes of its
 *	registers from where the address written left off, may hold the
 *	clock low a while, and may be left holding SDA in the middle of a
 *	byte. Its registers are the GT911's: the product at 0x8140, the
 *	configuration at 0x8047, the frame at 0x814E.
 *
 *	- bus: a write and a read of its registers; an address nobody has
 *	  is not acknowledged; the clock held low is waited for; a device
 *	  left holding SDA is clocked free.
 *	- touch: the controller found at 0x14 when nothing is at 0x5D, its
 *	  configuration read; frames told by its INT reach the input layer
 *	  as a touch screen's do: a tap is a click where it touched, a hold
 *	  of a second the second button, a move a drag, a second finger not
 *	  the pointer; two fingers moved together the wheel, a pinch
 *	  nothing. The frame's buffer is given back after each read.
 *	- turned: a controller of 480 by 640 under a screen wider than it is
 *	  tall has its X and Y swapped, its X reversed as the board says;
 *	  without an INT it is read every few milliseconds.
 */

#include "kernel.h"
#include "ktest.h"
#include <ts/hid.h>
#include <ts/disp.h>
#include <ts/time.h>
#include "../../device/i2c/i2cbus.h"
#include "../../device/i2c/gt911.h"

IMPORT ER	ts_disp_ref( T_DISPSPEC *spec ) __attribute__((weak));
IMPORT BOOL	knl_pt_holding( void ) __attribute__((weak));

/* ---------------------------------------------------------------- the device on the wires */

#define REG_BASE	0x8040
#define REG_SIZE	0x140

#define S_IDLE		0
#define S_ADDR		1
#define S_ADDR_ACK	2
#define S_WDATA		3
#define S_WACK		4
#define S_RDATA		5
#define S_RACK		6

typedef struct {
	BOOL	m_low[2];		/* pulled by the controller's side */
	BOOL	d_low[2];		/* pulled by the device */
	UH	addr;			/* the device's address; 0 none */
	INT	st, bit, nbyte;
	UB	cur;
	BOOL	rd, mack;
	UH	ptr;
	UB	reg[REG_SIZE];
	INT	stretch;		/* SCL held for this many looks at it */
	INT	stretch_each;		/* and again after each address acknowledged */
	UD	starts, stops, clears;	/* 0x814E written 0 */
	UD	stretched;
} FAKE;

LOCAL INT lvl( FAKE *f, INT w )
{
	return ( f->m_low[w] || f->d_low[w] ) ? 0 : 1;
}

LOCAL UB reg_get( FAKE *f, UH a )
{
	return ( a >= REG_BASE && a < REG_BASE + REG_SIZE ) ? f->reg[a - REG_BASE] : 0;
}

LOCAL void reg_put( FAKE *f, UH a, UB v )
{
	if ( a >= REG_BASE && a < REG_BASE + REG_SIZE ) f->reg[a - REG_BASE] = v;
	if ( a == 0x814E && v == 0 ) f->clears++;
}

LOCAL void drive_bit( FAKE *f )
{
	f->d_low[I2CGPIO_SDA] = ( ( f->cur >> ( 7 - f->bit ) ) & 1 ) == 0;
}

/* SCL went high: the bit on SDA is taken */
LOCAL void scl_rose( FAKE *f )
{
	INT	b = lvl(f, I2CGPIO_SDA);

	if ( f->st == S_ADDR || f->st == S_WDATA ) {
		f->cur = (UB)( ( f->cur << 1 ) | b );
		f->bit++;
	} else if ( f->st == S_RACK ) {
		f->mack = ( b == 0 );
	}
}

/* SCL went low: the device sets SDA for what comes next */
LOCAL void scl_fell( FAKE *f )
{
	switch ( f->st ) {
	case S_ADDR:
		if ( f->bit < 8 ) break;
		if ( f->addr != 0 && ( f->cur >> 1 ) == f->addr ) {
			f->rd = ( f->cur & 1 ) != 0;
			f->d_low[I2CGPIO_SDA] = TRUE;		/* acknowledged */
			f->st = S_ADDR_ACK;
			if ( f->stretch_each > 0 ) f->stretch = f->stretch_each;
		} else {
			f->st = S_IDLE;
		}
		break;
	case S_ADDR_ACK:
		f->d_low[I2CGPIO_SDA] = FALSE;
		f->bit = 0;
		if ( f->rd ) {
			f->cur = reg_get(f, f->ptr++);
			f->st = S_RDATA;
			drive_bit(f);
		} else {
			f->cur = 0;
			f->nbyte = 0;
			f->st = S_WDATA;
		}
		break;
	case S_WDATA:
		if ( f->bit < 8 ) break;
		if ( f->nbyte == 0 ) {
			f->ptr = (UH)( f->cur << 8 );
		} else if ( f->nbyte == 1 ) {
			f->ptr = (UH)( f->ptr | f->cur );
		} else {
			reg_put(f, f->ptr++, f->cur);
		}
		f->nbyte++;
		f->d_low[I2CGPIO_SDA] = TRUE;
		f->st = S_WACK;
		break;
	case S_WACK:
		f->d_low[I2CGPIO_SDA] = FALSE;
		f->bit = 0;
		f->cur = 0;
		f->st = S_WDATA;
		break;
	case S_RDATA:
		f->bit++;
		if ( f->bit < 8 ) {
			drive_bit(f);
		} else {
			f->d_low[I2CGPIO_SDA] = FALSE;
			f->st = S_RACK;
		}
		break;
	case S_RACK:
		if ( f->mack ) {
			f->cur = reg_get(f, f->ptr++);
			f->bit = 0;
			f->st = S_RDATA;
			drive_bit(f);
		} else {
			f->st = S_IDLE;
		}
		break;
	default:
		break;
	}
}

LOCAL void f_line( void *ctx, INT w, BOOL low )
{
	FAKE	*f = (FAKE *)ctx;
	INT	o_sda = lvl(f, I2CGPIO_SDA), o_scl = lvl(f, I2CGPIO_SCL), n;

	f->m_low[w] = low;
	if ( w == I2CGPIO_SCL ) {
		if ( !low && f->stretch > 0 ) {
			f->d_low[I2CGPIO_SCL] = TRUE;		/* held a while (f_sense lets it go) */
		}
		n = lvl(f, I2CGPIO_SCL);
		if ( o_scl == 1 && n == 0 ) scl_fell(f);
		if ( o_scl == 0 && n == 1 ) scl_rose(f);
	} else if ( o_scl == 1 ) {
		n = lvl(f, I2CGPIO_SDA);
		if ( o_sda == 1 && n == 0 ) {			/* a start */
			f->starts++;
			f->st = S_ADDR;
			f->bit = 0;
			f->cur = 0;
			f->d_low[I2CGPIO_SDA] = FALSE;
		} else if ( o_sda == 0 && n == 1 ) {		/* a stop */
			f->stops++;
			f->st = S_IDLE;
		}
	}
}

LOCAL INT f_sense( void *ctx, INT w )
{
	FAKE	*f = (FAKE *)ctx;

	if ( w == I2CGPIO_SCL && f->d_low[I2CGPIO_SCL] ) {
		f->stretched++;
		if ( --f->stretch <= 0 ) {
			f->stretch = 0;
			f->d_low[I2CGPIO_SCL] = FALSE;
			if ( lvl(f, I2CGPIO_SCL) == 1 ) scl_rose(f);
		}
	}
	return lvl(f, w);
}

/* A GT911 of the range, its INT set to the trigger; the product "911" */
LOCAL void fake_gt911( FAKE *f, UH addr, INT xmax, INT ymax, INT trig )
{
	knl_memset(f, 0, sizeof(*f));
	f->addr = addr;
	reg_put(f, 0x8140, '9');
	reg_put(f, 0x8141, '1');
	reg_put(f, 0x8142, '1');
	reg_put(f, 0x8143, 0);
	reg_put(f, 0x8144, 0x60);
	reg_put(f, 0x8145, 0x10);
	reg_put(f, 0x8047, 0x41);
	reg_put(f, 0x8048, (UB)xmax);
	reg_put(f, 0x8049, (UB)( xmax >> 8 ));
	reg_put(f, 0x804A, (UB)ymax);
	reg_put(f, 0x804B, (UB)( ymax >> 8 ));
	reg_put(f, 0x804C, 5);
	reg_put(f, 0x804D, (UB)trig);
	f->clears = 0;
}

/* A frame: n contacts (id, x, y), the state written last as the controller does */
LOCAL void frame( FAKE *f, INT n, CONST INT *c )
{
	INT	k;

	for ( k = 0; k < n; k++ ) {
		UH	a = (UH)( 0x814F + 8 * k );

		reg_put(f, a, (UB)c[3 * k]);
		reg_put(f, a + 1, (UB)c[3 * k + 1]);
		reg_put(f, a + 2, (UB)( c[3 * k + 1] >> 8 ));
		reg_put(f, a + 3, (UB)c[3 * k + 2]);
		reg_put(f, a + 4, (UB)( c[3 * k + 2] >> 8 ));
		reg_put(f, a + 5, 20);
		reg_put(f, a + 6, 0);
	}
	__sync_synchronize();
	f->reg[0x814E - REG_BASE] = (UB)( 0x80 | n );
}

LOCAL FAKE		fa, fb;
LOCAL T_I2CGPIO		g0, ga, gb;

LOCAL T_I2CBUS *bus_of( FAKE *f, T_I2CGPIO *g, CONST char *name )
{
	T_I2CGPIO_PINS	p;

	knl_memset(&p, 0, sizeof(p));
	p.ctx = f;
	p.line = f_line;
	p.sense = f_sense;
	p.half_us = 0;
	p.stretch_us = 200;
	return knl_i2cgpio_init(g, &p, (CONST UB *)name);
}

/* ---------------------------------------------------------------- bus */

LOCAL void test_tpanel_bus( void )
{
	T_I2CBUS	*bus;
	UB		w[3], r[6];
	UD		st;

	fake_gt911(&fa, 0x5D, 640, 480, GT911_TRIG_FALLING);
	/* left in the middle of a byte of 0 it was sending: SDA held low */
	fa.st = S_RDATA;
	fa.cur = 0x00;
	fa.bit = 1;
	fa.d_low[I2CGPIO_SDA] = TRUE;
	bus = bus_of(&fa, &g0, "i2c-test0");
	KT_ASSERT(bus != NULL);
	if ( bus == NULL ) return;
	KT_ASSERT_EQ(g0.stuck, 1);
	KT_ASSERT_EQ(lvl(&fa, I2CGPIO_SDA), 1);
	KT_ASSERT_EQ(fa.st, S_IDLE);

	/* the product, from where the address written left off */
	w[0] = 0x81;
	w[1] = 0x40;
	knl_memset(r, 0, sizeof(r));
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 2, r, 6), E_OK);
	KT_ASSERT_EQ(r[0], '9');
	KT_ASSERT_EQ(r[1], '1');
	KT_ASSERT_EQ(r[2], '1');
	KT_ASSERT_EQ(r[3], 0);
	KT_ASSERT_EQ(r[4] | ( r[5] << 8 ), 0x1060);
	KT_ASSERT_EQ(fa.st, S_IDLE);			/* the stop seen */
	KT_ASSERT_EQ(lvl(&fa, I2CGPIO_SDA) + lvl(&fa, I2CGPIO_SCL), 2);

	/* a write, read back */
	w[0] = 0x81;
	w[1] = 0x4E;
	w[2] = 0x5A;
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 3, NULL, 0), E_OK);
	KT_ASSERT_EQ(reg_get(&fa, 0x814E), 0x5A);
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 2, r, 1), E_OK);
	KT_ASSERT_EQ(r[0], 0x5A);

	/* nobody at 0x33 */
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x33, w, 2, r, 1), E_IO);
	KT_ASSERT_EQ(g0.nacks, 1);
	KT_ASSERT_EQ(lvl(&fa, I2CGPIO_SDA) + lvl(&fa, I2CGPIO_SCL), 2);

	/* the clock held after each address: waited for */
	fa.stretch_each = 20;
	st = fa.stretched;
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 2, r, 1), E_OK);
	KT_ASSERT_EQ(r[0], 0x5A);
	KT_ASSERT(fa.stretched > st);
	/*
	 * held longer than the bus waits: the transfer ends. Let go after,
	 * the device is left holding SDA for its acknowledge: the next
	 * transfer's start clocks it free
	 */
	fa.stretch_each = 1000;
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 2, r, 1), E_TMOUT);
	fa.stretch_each = 0;
	fa.stretch = 0;
	fa.d_low[I2CGPIO_SCL] = FALSE;
	r[0] = 0;
	KT_ASSERT_ER(bus->xfer(bus->ctx, 0x5D, w, 2, r, 1), E_OK);
	KT_ASSERT_EQ(r[0], 0x5A);
	tm_printf((UB *)"  i2c-test0: %d transfers, %d not acknowledged, %d cleared, %d starts, %d stops\n",
		  (INT)g0.xfers, (INT)g0.nacks, (INT)g0.stuck, (INT)fa.starts, (INT)fa.stops);
	(void)tk_del_mtx(g0.mtx);
}

/* ---------------------------------------------------------------- touch */

LOCAL INT	irq_gd = -1;
LOCAL INT	irq_trig = -1;
LOCAL INT	irq_arms = 0;

LOCAL ER fake_arm( void *ctx, INT trig, INT gd )
{
	(void)ctx;
	irq_gd = gd;
	irq_trig = trig;
	irq_arms++;
	return E_OK;
}

LOCAL INT	scr_w = 640, scr_h = 480;

LOCAL void screen( void )
{
	T_DISPSPEC	s;

	if ( ts_disp_ref != NULL && ts_disp_ref(&s) >= E_OK ) {
		scr_w = (INT)( ( s.view_w > 0 ) ? s.view_w : s.width );
		scr_h = (INT)( ( s.view_h > 0 ) ? s.view_h : s.height );
	}
}

LOCAL T_HIDEV want( UINT type, INT code, TMO tmo )
{
	T_HIDEV	ev;
	ER	er;

	knl_memset(&ev, 0, sizeof(ev));
	er = ts_hid_read(&ev, tmo);
	KT_ASSERT_ER(er, E_OK);
	KT_ASSERT_EQ(ev.type, type);
	if ( code >= 0 ) KT_ASSERT_EQ(ev.code, code);
	return ev;
}

LOCAL void none( TMO tmo )
{
	T_HIDEV	ev;
	ER	er = ts_hid_read(&ev, tmo);

	if ( er >= E_OK ) {
		tm_printf((UB *)"  unwanted event: type %d code %d at %d,%d\n", (INT)ev.type, (INT)ev.code, ev.x, ev.y);
	}
	KT_ASSERT_ER(er, E_TMOUT);
}

/* A frame on fa, told by its INT, and read: the buffer given back */
LOCAL void touch_a( INT n, CONST INT *c )
{
	UD	cl = fa.clears;
	INT	i;

	frame(&fa, n, c);
	knl_gt911_irq(irq_gd);
	for ( i = 0; i < 50 && fa.clears == cl; i++ ) tk_dly_tsk(2);
	KT_ASSERT(fa.clears > cl);
	KT_ASSERT_EQ(reg_get(&fa, 0x814E), 0);
}

/* The wheels' notches and the rest that came until nothing more came for quiet */
LOCAL void gather( TMO quiet, INT *p_v, INT *p_h, INT *p_rest )
{
	T_HIDEV	ev;

	*p_v = *p_h = *p_rest = 0;
	while ( ts_hid_read(&ev, quiet) >= E_OK ) {
		if ( ev.type == HID_EV_WHEEL && ev.code == HID_WHEEL_V ) *p_v += ev.dz;
		else if ( ev.type == HID_EV_WHEEL ) *p_h += ev.dz;
		else ( *p_rest )++;
	}
	tm_printf((UB *)"    wheel %d, across %d, others %d\n", *p_v, *p_h, *p_rest);
}

/*
 * Two fingers on the controller, taken together as by default: moved
 * up together, the wheel turned towards the person, a notch for each
 * 10 mm of the screen; the first one's touch no click; a pinch nothing.
 */
LOCAL void test_tpanel_two( void )
{
	INT	px = 0, step, dy, k, v, h, rest;
	INT	c[6];
	static CONST INT	q1[] = { 7, 200, 300 };

	if ( scr_h > scr_w ) {
		tm_printf((UB *)"  two fingers together: the controller turned, not tried\n");
		return;
	}
	none(600);
	touch_a(1, q1);
	(void)want(HID_EV_MOVE, -1, 500);
	KT_ASSERT(knl_hid_taken_touch(&px));
	step = (INT)( (D)100 * px / 1000 );
	dy = (INT)( (D)step * 5 / 2 * 480 / scr_h );	/* 2.5 notches' worth, in the controller's units */
	tm_printf((UB *)"  two fingers together: %d pixels in 100 mm, %d a notch, %d of the controller for 2.5\n",
		  px, step, dy);
	c[0] = 7; c[1] = 200; c[2] = 300; c[3] = 8; c[4] = 300; c[5] = 300;
	touch_a(2, c);
	none(1200);				/* no click, no hold */
	for ( k = 1; k <= 5; k++ ) {
		c[2] = c[5] = 300 - dy * k / 5;
		touch_a(2, c);
		tk_dly_tsk(30);
	}
	tk_dly_tsk(150);
	touch_a(2, c);
	tk_dly_tsk(20);
	touch_a(2, c);
	/* the first lets go, the second moves on its own: nothing */
	touch_a(1, c + 3);
	tk_dly_tsk(20);
	c[4] = 500; c[5] = 100;
	touch_a(1, c + 3);
	touch_a(0, NULL);
	gather(400, &v, &h, &rest);
	KT_ASSERT_EQ(v, -2);			/* up: what is shown goes up with the fingers */
	KT_ASSERT_EQ(h, 0);
	KT_ASSERT_EQ(rest, 0);

	/* a pinch: nothing */
	tm_printf((UB *)"  pinch\n");
	c[0] = 9; c[1] = 300; c[2] = 240; c[3] = 10; c[4] = 340; c[5] = 240;
	touch_a(2, c);
	for ( k = 1; k <= 5; k++ ) {
		c[1] = 300 - 30 * k;
		c[4] = 340 + 30 * k;
		touch_a(2, c);
		tk_dly_tsk(20);
	}
	touch_a(0, NULL);
	gather(400, &v, &h, &rest);
	KT_ASSERT_EQ(v + h + rest, 0);
}

LOCAL void test_tpanel_touch( void )
{
	T_GT911CONF	c;
	T_GT911INFO	in;
	T_HIDATTR	a, keep;
	T_HIDEV		ev, at;
	T_I2CBUS	*bus;
	INT		gd, ex, ey;
	UD		t0, t1;
	static CONST INT	p1[] = { 3, 320, 240 };
	static CONST INT	p1b[] = { 3, 324, 243 };
	static CONST INT	p2[] = { 4, 100, 400 };
	static CONST INT	p3[] = { 4, 500, 100 };
	static CONST INT	two[] = { 4, 500, 100, 6, 50, 50 };
	static CONST INT	p6[] = { 6, 50, 50 };

	screen();
	fake_gt911(&fa, 0x14, 640, 480, GT911_TRIG_FALLING);
	bus = bus_of(&fa, &ga, "i2c-test");
	KT_ASSERT(bus != NULL);
	if ( bus == NULL ) return;
	knl_memset(&c, 0, sizeof(c));
	c.bus = bus;
	c.irq_arm = fake_arm;
	c.name = (CONST UB *)"gt911-test";
	gd = knl_gt911_attach(&c);
	if ( gd == E_LIMIT ) KT_SKIP("both controllers' places in use (a panel on the board)");
	KT_ASSERT(gd >= 0);
	if ( gd < 0 ) return;
	KT_ASSERT_ER(knl_gt911_info(gd, &in), E_OK);
	tm_printf((UB *)"  gt911-test: GT%s at %02x, %d by %d, %d contacts, trigger %d, %s, screen %d by %d\n",
		  in.id, in.addr, in.x_max, in.y_max, in.touches, in.trig, in.polled ? "polled" : "interrupt",
		  scr_w, scr_h);
	KT_ASSERT_EQ(in.addr, 0x14);			/* 0x5D tried first: nobody */
	KT_ASSERT_EQ(in.id[0], '9');
	KT_ASSERT_EQ(in.id[2], '1');
	KT_ASSERT_EQ(in.id[3], 0);
	KT_ASSERT_EQ(in.fw, 0x1060);
	KT_ASSERT_EQ(in.x_max, 640);
	KT_ASSERT_EQ(in.y_max, 480);
	KT_ASSERT_EQ(in.touches, 5);
	KT_ASSERT_EQ(in.trig, GT911_TRIG_FALLING);
	KT_ASSERT(!in.polled);
	KT_ASSERT_EQ(irq_gd, gd);
	KT_ASSERT_EQ(irq_trig, GT911_TRIG_FALLING);
	KT_ASSERT_EQ(in.swapped, ( scr_h > scr_w ));

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.pd_on = a.pd_off = 0;
	a.main = 0;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);
	while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) ;

	/* a tap: the pointer there, a click of the main button when it lets go */
	tm_printf((UB *)"  tap\n");
	touch_a(1, p1);
	at = want(HID_EV_MOVE, -1, 500);
	ex = 320 * scr_w / 640;
	ey = 240 * scr_h / 480;
	if ( !in.swapped ) {
		KT_ASSERT_EQ(at.x, ex);
		KT_ASSERT_EQ(at.y, ey);
	}
	none(100);
	touch_a(0, NULL);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	(void)want(HID_EV_BTN_UP, 0, 500);
	none(600);

	/* held a second, wavering a little: the second button */
	tm_printf((UB *)"  hold\n");
	touch_a(1, p1);
	t0 = 0;
	(void)ts_get_mono(&t0);
	none(300);
	touch_a(1, p1b);
	ev = want(HID_EV_BTN_DOWN, 1, 1500);
	t1 = 0;
	(void)ts_get_mono(&t1);
	KT_ASSERT(t1 - t0 >= 900000000ULL);
	KT_ASSERT_EQ(ev.x, at.x);
	touch_a(0, NULL);
	(void)want(HID_EV_BTN_UP, 1, 500);
	none(100);

	/* moved: a drag from where it touched */
	tm_printf((UB *)"  drag\n");
	touch_a(1, p2);
	at = want(HID_EV_MOVE, -1, 500);
	touch_a(1, p3);
	ev = want(HID_EV_BTN_DOWN, 0, 500);
	KT_ASSERT_EQ(ev.x, at.x);
	KT_ASSERT_EQ(ev.y, at.y);
	ev = want(HID_EV_MOVE, -1, 500);
	KT_ASSERT_EQ(ev.code & 1, 1);
	KT_ASSERT(ev.x != at.x);
	/*
	 * a second finger comes while dragging: taken together, the drag
	 * is let go then, and what stays after the first lets go is not
	 * the pointer
	 */
	tm_printf((UB *)"  two fingers\n");
	touch_a(2, two);
	(void)want(HID_EV_BTN_UP, 0, 500);
	touch_a(1, p6);
	touch_a(1, p2);
	none(150);
	touch_a(0, NULL);
	none(1200);
	KT_ASSERT(knl_pt_holding == NULL || !knl_pt_holding());

	/* not taken together (二本指スクロール off): the drag goes on until the first lets go */
	tm_printf((UB *)"  two fingers, not together\n");
	a.tch_noscr = 1;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);
	touch_a(1, p2);
	at = want(HID_EV_MOVE, -1, 500);
	touch_a(1, p3);
	(void)want(HID_EV_BTN_DOWN, 0, 500);
	(void)want(HID_EV_MOVE, -1, 500);
	touch_a(2, two);
	none(100);
	touch_a(1, p6);
	(void)want(HID_EV_BTN_UP, 0, 500);
	none(150);
	touch_a(0, NULL);
	none(1200);
	a.tch_noscr = 0;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);

	test_tpanel_two();
	KT_ASSERT_ER(knl_gt911_info(gd, &in), E_OK);
	tm_printf((UB *)"  gt911-test: %d frames, %d interrupts, %d errors, armed %d times\n",
		  (INT)in.frames, (INT)in.ints, (INT)in.errors, irq_arms);
	KT_ASSERT_EQ(in.errors, 0);
	KT_ASSERT(in.frames >= 10);
	KT_ASSERT_ER(ts_hid_setattr(&keep), E_OK);
}

/* ---------------------------------------------------------------- turned */

LOCAL void test_tpanel_turned( void )
{
	T_GT911CONF	c;
	T_GT911INFO	in;
	T_HIDATTR	a, keep;
	T_HIDEV		ev;
	T_I2CBUS	*bus;
	INT		gd, ex, ey;
	static CONST INT	p[] = { 1, 120, 600 };

	screen();
	if ( scr_w <= scr_h ) KT_SKIP("the screen is not wider than it is tall");
	fake_gt911(&fb, 0x5D, 480, 640, GT911_TRIG_RISING);
	bus = bus_of(&fb, &gb, "i2c-test2");
	KT_ASSERT(bus != NULL);
	if ( bus == NULL ) return;
	knl_memset(&c, 0, sizeof(c));
	c.bus = bus;
	c.invert_x = TRUE;
	c.period = 5;
	c.name = (CONST UB *)"gt911-test2";
	gd = knl_gt911_attach(&c);
	if ( gd == E_LIMIT ) KT_SKIP("both controllers' places in use (a panel on the board)");
	KT_ASSERT(gd >= 0);
	if ( gd < 0 ) return;
	KT_ASSERT_ER(knl_gt911_info(gd, &in), E_OK);
	KT_ASSERT_EQ(in.addr, 0x5D);
	KT_ASSERT(in.swapped);
	KT_ASSERT(in.polled);
	KT_ASSERT_EQ(in.trig, GT911_TRIG_RISING);

	KT_ASSERT_ER(ts_hid_getattr(&keep), E_OK);
	a = keep;
	a.pd_on = a.pd_off = 0;
	a.main = 0;
	KT_ASSERT_ER(ts_hid_setattr(&a), E_OK);
	while ( ts_hid_read(&ev, TMO_POL) >= E_OK ) ;

	/* the controller's X 120 of 480, reversed: 359; its Y 600 of 640 across the screen */
	frame(&fb, 1, p);
	ev = want(HID_EV_MOVE, -1, 500);
	ex = 600 * scr_w / 640;
	ey = ( 479 - 120 ) * scr_h / 480;
	tm_printf((UB *)"  gt911-test2: 120,600 of 480 by 640 at %d,%d on the screen (%d,%d wanted)\n",
		  ev.x, ev.y, ex, ey);
	KT_ASSERT_EQ(ev.x, ex);
	KT_ASSERT_EQ(ev.y, ey);
	tk_dly_tsk(30);
	KT_ASSERT_EQ(reg_get(&fb, 0x814E), 0);	/* read without an INT */
	frame(&fb, 0, NULL);
	(void)want(HID_EV_BTN_DOWN, 0, 500);
	(void)want(HID_EV_BTN_UP, 0, 500);
	none(600);
	KT_ASSERT_ER(ts_hid_setattr(&keep), E_OK);
}

EXPORT void ktest_tpanel( void )
{
	KT_RUN(test_tpanel_bus);
	KT_RUN(test_tpanel_touch);
	KT_RUN(test_tpanel_turned);
}
