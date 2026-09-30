/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obgpio.c
 *	The banks of general purpose pins as objects (design 18.7)
 *
 *	On the Raspberry Pi 5 each bank is a device object: the two always-on
 *	banks of the BCM2712 ("gpio-aon0", 17 pins, the activity LED among
 *	them, and "gpio-aon1", 6), and the three banks of the RP1
 *	("gpio-rp1-0" is the 40 pin header, GPIO 0 to 27; "gpio-rp1-1" 28 to
 *	33; "gpio-rp1-2" 34 to 53). The pins of the RP1 go by their GPIO
 *	number, the always-on ones by their place in the bank.
 *
 *	Record 1 is a line "pin direction value pull" for each pin, and
 *	written such lines set the pins (include/ts/ob.h, OB_GPIO_PINS). The
 *	system owns the banks and the administrators' group may write them
 *	(rw-rw-r--): everyone may look, the group and the administrators
 *	may set a pin. A pin a driver of the kernel uses -- the card detect
 *	of the SD socket, the reset of the Ethernet PHY, the header UART when
 *	it is set up, the I2S pins once the sound device has taken them --
 *	is shown as the kernel's and is not set. Setting pins to one of the
 *	chip's functions is the drivers' own business and stays in them.
 *
 *	A change of an input pin is told as OB_E_CHANGE on record 1, the
 *	pin in the notice's code and its value in x: a task looks at the
 *	banks someone watches ten times a second.
 *
 *	The kernel sets the activity LED through the object of its bank
 *	once the object layer is up (gpio_led); only the blinks that tell
 *	how far the start went, before there are objects, set it directly.
 *	QEMU has no pins, and so no such objects.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "obj.h"

#ifdef RPI5

#include <ts/gpio.h>

#define BANKS		5
#define GP_TEXT_MAX	2048
#define POLL_MS		100
#define POLL_PRI	30		/* below everything that does work */

#define RP1_FUNC_SW	5		/* the pin is driven from software */
#define RP1_FUNC_NONE	31		/* the pin is left alone */
#define RP1_FUNC_I2S0	2
#define RP1_PAD_PULLDOWN 0x00000004
#define RP1_PAD_PULLUP	0x00000008

typedef struct {
	CONST char *name;
	BOOL	rp1;			/* the RP1's, else the always-on block's */
	UINT	bank;			/* always-on: its bank */
	UINT	first;			/* RP1: its first GPIO */
	UINT	pins;
	TS_UUID	uuid;
	BOOL	there;
	UD	last;			/* the inputs as last looked at, bit per pin */
} GPBANK;

LOCAL GPBANK	gp_bank[BANKS] = {
	{ "gpio-aon0",  FALSE, 0, 0, 17, { { 0 } }, FALSE, 0 },
	{ "gpio-aon1",  FALSE, 1, 0, 6, { { 0 } }, FALSE, 0 },
	{ "gpio-rp1-0", TRUE,  0, 0, 28, { { 0 } }, FALSE, 0 },
	{ "gpio-rp1-1", TRUE,  0, 28, 6, { { 0 } }, FALSE, 0 },
	{ "gpio-rp1-2", TRUE,  0, 34, 20, { { 0 } }, FALSE, 0 },
};
LOCAL ID	gp_mtx = 0;
LOCAL ID	gp_led_key = 0;		/* the LED's bank, kept open by the kernel */

/* ---------------------------------------------------------------- one pin */

typedef struct {
	INT	dir;			/* GPIO_DIR_*, or 2 + the function it is given to */
	INT	value;
	INT	pull;			/* GPIO_PULL_*, -1 not known */
	BOOL	kernel;			/* a driver of the kernel uses it */
} GPPIN;

/* Whether a driver of the kernel has the pin */
LOCAL BOOL kernel_pin( CONST GPBANK *b, UINT pin, INT func )
{
	if ( !b->rp1 ) {
		return (BOOL)( b->bank == 0 && pin == GIO_AON_SD_CD );
	}
	if ( pin == 32 ) return TRUE;				/* the Ethernet PHY's reset */
#if CNF_RP1_UART0
	if ( pin == 14 || pin == 15 ) return TRUE;		/* the header UART */
#endif
	if ( pin >= 18 && pin <= 21 && func == RP1_FUNC_I2S0 ) return TRUE;	/* sound */
	return FALSE;
}

LOCAL ER pin_get( CONST GPBANK *b, UINT pin, GPPIN *p )
{
	UW	st[4];
	INT	v;

	if ( !b->rp1 ) {
		p->dir = gpio_get_dir(b->bank, pin);
		v = gpio_read(b->bank, pin);
		if ( p->dir < 0 || v < 0 ) return E_PAR;
		p->value = v;
		p->pull = -1;			/* the always-on block does not say */
		p->kernel = kernel_pin(b, pin, 0);
		return E_OK;
	}
	if ( rp1_gpio_state(pin, st) < E_OK || ( v = rp1_gpio_read(pin) ) < 0 ) {
		return E_PAR;
	}
	{
		INT	func = (INT)( st[0] & 0x1F );
		UW	bit = 1UL << ( ( pin < 28 ) ? pin : ( pin < 34 ) ? pin - 28 : pin - 34 );

		p->dir = ( func == RP1_FUNC_SW || func == RP1_FUNC_NONE )
		       ? ( ( st[3] & bit ) ? GPIO_DIR_OUT : GPIO_DIR_IN ) : 2 + func;
		p->pull = ( st[1] & RP1_PAD_PULLUP ) ? GPIO_PULL_UP
			: ( st[1] & RP1_PAD_PULLDOWN ) ? GPIO_PULL_DOWN : GPIO_PULL_NONE;
		p->kernel = kernel_pin(b, pin, func);
	}
	p->value = v;
	return E_OK;
}

LOCAL INT pins_text( CONST GPBANK *b, UB *t, INT max )
{
	GPPIN	p;
	UINT	i, pin;
	INT	n = 0;

	for ( i = 0; i < b->pins; i++ ) {
		pin = b->first + i;
		if ( pin_get(b, pin, &p) < E_OK ) continue;
		n = knl_oj_put_num(t, n, max, pin);
		if ( p.dir >= 2 ) {
			n = knl_oj_put(t, n, max, " alt");
			n = knl_oj_put_num(t, n, max, p.dir - 2);
		} else {
			n = knl_oj_put(t, n, max, ( p.dir == GPIO_DIR_OUT ) ? " out" : " in");
		}
		n = knl_oj_put(t, n, max, p.value ? " 1" : " 0");
		n = knl_oj_put(t, n, max, ( p.pull == GPIO_PULL_UP ) ? " up" : ( p.pull == GPIO_PULL_DOWN ) ? " down"
					  : ( p.pull == GPIO_PULL_NONE ) ? " none" : " -");
		n = knl_oj_put(t, n, max, p.kernel ? " kernel\n" : "\n");
	}
	return n;
}

/* One word of a line, and where the next begins */
LOCAL INT word( CONST UB *s, INT len, INT i, char *w, INT max )
{
	INT	k = 0;

	while ( i < len && ( s[i] == ' ' || s[i] == '\t' ) ) i++;
	while ( i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r' ) {
		if ( k < max - 1 ) w[k++] = (char)s[i];
		i++;
	}
	w[k] = 0;
	return i;
}

LOCAL BOOL same( CONST char *a, CONST char *b )
{
	while ( *a != 0 && *a == *b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == *b );
}

/* One line "pin direction value pull" applied */
LOCAL ER pin_set( CONST GPBANK *b, CONST UB *s, INT len )
{
	char	w[4][12];
	UINT	pin = 0;
	INT	i = 0, k, dir = -1, val = -1, pull = -1;
	GPPIN	p;
	ER	er = E_OK;

	for ( k = 0; k < 4; k++ ) {
		i = word(s, len, i, w[k], sizeof(w[k]));
	}
	if ( w[0][0] == 0 ) {
		return E_OK;			/* an empty line */
	}
	for ( k = 0; w[0][k] != 0; k++ ) {
		if ( w[0][k] < '0' || w[0][k] > '9' ) return E_PAR;
		pin = pin * 10 + (UINT)( w[0][k] - '0' );
	}
	if ( pin < b->first || pin >= b->first + b->pins ) {
		return E_PAR;
	}
	if ( same(w[1], "in") ) dir = GPIO_DIR_IN;
	else if ( same(w[1], "out") ) dir = GPIO_DIR_OUT;
	else if ( !same(w[1], "-") && w[1][0] != 0 ) return E_PAR;
	if ( same(w[2], "0") ) val = 0;
	else if ( same(w[2], "1") ) val = 1;
	else if ( !same(w[2], "-") && w[2][0] != 0 ) return E_PAR;
	if ( same(w[3], "up") ) pull = GPIO_PULL_UP;
	else if ( same(w[3], "down") ) pull = GPIO_PULL_DOWN;
	else if ( same(w[3], "none") ) pull = GPIO_PULL_NONE;
	else if ( !same(w[3], "-") && w[3][0] != 0 ) return E_PAR;

	if ( pin_get(b, pin, &p) < E_OK ) {
		return E_IO;
	}
	if ( p.kernel ) {
		return E_BUSY;			/* a driver of the kernel has it */
	}
	if ( !b->rp1 ) {
		if ( pull >= 0 ) return E_NOSPT;
		/* the value first, so that a pin made an output starts at it */
		if ( val >= 0 ) er = gpio_write(b->bank, pin, (UINT)val);
		if ( er >= E_OK && dir >= 0 ) er = gpio_set_dir(b->bank, pin, (UINT)dir);
		return er;
	}
	if ( val >= 0 ) er = rp1_gpio_write(pin, (UINT)val);
	if ( er >= E_OK && dir >= 0 ) er = rp1_gpio_set_dir(pin, (UINT)dir);
	if ( er >= E_OK && pull >= 0 ) er = rp1_gpio_set_pull(pin, (UINT)pull);
	return er;
}

/* ---------------------------------------------------------------- the records */

LOCAL INT gp_text( void *ctx, UB *t, INT n, INT max )
{
	CONST GPBANK	*b = (CONST GPBANK *)ctx;

	n = knl_oj_put(t, n, max, "<p>Record 1: a line \"pin direction value pull\" for each pin of "
			  "the bank; written such lines set the pins, - leaving a part as it is. "
			  "A pin with \"kernel\" after it is a driver's and is not set.</p>");
	n = knl_oj_put(t, n, max, b->rp1 ? "<p>The RP1's GPIO " : "<p>The BCM2712's always-on bank ");
	n = knl_oj_put_num(t, n, max, b->rp1 ? b->first : b->bank);
	if ( b->rp1 ) {
		n = knl_oj_put(t, n, max, " to ");
		n = knl_oj_put_num(t, n, max, b->first + b->pins - 1);
	}
	return knl_oj_put(t, n, max, "</p>");
}

LOCAL INT gp_attr( void *ctx, UB *j, INT n, INT max )
{
	CONST GPBANK	*b = (CONST GPBANK *)ctx;

	n = knl_oj_put(j, n, max, ",\"virtual\":false,\"chip\":");
	n = knl_oj_put(j, n, max, b->rp1 ? "\"RP1\"" : "\"BCM2712\"");
	n = knl_oj_put(j, n, max, ",\"first\":");
	n = knl_oj_put_num(j, n, max, b->first);
	n = knl_oj_put(j, n, max, ",\"pins\":");
	return knl_oj_put_num(j, n, max, b->pins);
}

LOCAL ER gp_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	UB	*t;
	INT	len;
	SZ	n = 0;

	(void)pos;
	if ( recno != OB_GPIO_PINS ) {
		return E_NOEXS;
	}
	if ( off < 0 || size < 0 ) {
		return E_PAR;
	}
	t = (UB *)Kmalloc(GP_TEXT_MAX);
	if ( t == NULL ) {
		return E_NOMEM;
	}
	tk_loc_mtx(gp_mtx, TMO_FEVR);
	len = pins_text((CONST GPBANK *)ctx, t, GP_TEXT_MAX);
	tk_unl_mtx(gp_mtx);
	if ( len >= 0 && off < len ) {
		n = (SZ)( len - off );
		if ( n > size ) n = size;
		knl_memcpy(buf, t + off, (INT)n);
	}
	Kfree(t);
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

LOCAL ER gp_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	CONST UB	*s = (CONST UB *)buf;
	INT		i = 0, e;
	ER		er = E_OK;

	(void)pos;
	if ( recno != OB_GPIO_PINS ) {
		return E_NOEXS;
	}
	if ( off != 0 || size < 0 ) {
		return E_PAR;
	}
	tk_loc_mtx(gp_mtx, TMO_FEVR);
	while ( i < (INT)size && er >= E_OK ) {
		for ( e = i; e < (INT)size && s[e] != '\n'; e++ ) ;
		er = pin_set((CONST GPBANK *)ctx, s + i, e - i);
		i = e + 1;
	}
	tk_unl_mtx(gp_mtx);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

LOCAL UD gp_size( void *ctx, INT recno )
{
	UB	*t;
	INT	len = 0;

	if ( recno != OB_GPIO_PINS ) {
		return 0;
	}
	t = (UB *)Kmalloc(GP_TEXT_MAX);
	if ( t != NULL ) {
		tk_loc_mtx(gp_mtx, TMO_FEVR);
		len = pins_text((CONST GPBANK *)ctx, t, GP_TEXT_MAX);
		tk_unl_mtx(gp_mtx);
		Kfree(t);
	}
	return ( len > 0 ) ? (UD)len : 0;
}

LOCAL CONST T_OBDVOPS gp_ops = {
	OB_S_GPIO, "gpio", OB_GPIO_NREC, 0664, 0, 0,
	gp_text, gp_attr, gp_rea, gp_wri, gp_size, NULL
};

/* ---------------------------------------------------------------- inputs that change */

LOCAL UD inputs( CONST GPBANK *b )
{
	GPPIN	p;
	UD	v = 0;
	UINT	i;

	for ( i = 0; i < b->pins; i++ ) {
		if ( pin_get(b, b->first + i, &p) >= E_OK && p.dir == GPIO_DIR_IN && p.value ) {
			v |= 1ULL << i;
		}
	}
	return v;
}

LOCAL void poll_task( INT stacd, void *exinf )
{
	T_OBNTM	m;
	UD	now, diff;
	INT	k;
	UINT	i;

	(void)stacd;
	(void)exinf;
	for (;;) {
		tk_dly_tsk(POLL_MS);
		for ( k = 0; k < BANKS; k++ ) {
			GPBANK	*b = &gp_bank[k];

			if ( !b->there ) continue;
			if ( !knl_ob_taker(&b->uuid, OB_E_CHANGE, NULL) ) {
				b->last = inputs(b);	/* nobody watching: only kept up to date */
				continue;
			}
			tk_loc_mtx(gp_mtx, TMO_FEVR);
			now = inputs(b);
			tk_unl_mtx(gp_mtx);
			diff = now ^ b->last;
			b->last = now;
			for ( i = 0; i < b->pins && diff != 0; i++ ) {
				if ( ( diff & ( 1ULL << i ) ) == 0 ) continue;
				knl_memset(&m, 0, sizeof(m));
				m.code = b->first + i;
				m.x = ( now >> i ) & 1;
				knl_ob_post(&b->uuid, OB_GPIO_PINS, OB_E_CHANGE, &m);
			}
		}
	}
}

/* ---------------------------------------------------------------- the LED, and the start */

/*
 * The activity LED (bank 0 of the always-on block, lit when low), set
 * through its bank's object with the key the kernel keeps. E_NOEXS
 * before there is one.
 */
EXPORT ER knl_obgpio_led( UINT pin, BOOL on )
{
	UB	line[24];
	INT	n;

	if ( gp_led_key <= 0 ) {
		return E_NOEXS;
	}
	n = knl_oj_put_num(line, 0, sizeof(line), pin);
	n = knl_oj_put(line, n, sizeof(line), on ? " out 0 -\n" : " out 1 -\n");
	return ob_wri_rec(gp_led_key, OB_GPIO_PINS, 0, line, n, NULL);
}

/* The object of a bank, for the checks on the board */
EXPORT ER knl_obgpio_uuid( INT k, TS_UUID *p_uuid )
{
	if ( k < 0 || k >= BANKS || !gp_bank[k].there ) {
		return E_NOEXS;
	}
	*p_uuid = gp_bank[k].uuid;
	return E_OK;
}

EXPORT void knl_obgpio_start( void )
{
	T_CMTX	cmtx;
	T_CTSK	ctsk;
	ID	tid;
	INT	k;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	gp_mtx = tk_cre_mtx(&cmtx);
	if ( gp_mtx <= 0 ) {
		return;
	}
	for ( k = 0; k < BANKS; k++ ) {
		GPBANK	*b = &gp_bank[k];

		if ( b->rp1 && rp1_gpio_read(b->first) < 0 ) {
			continue;		/* the RP1's window does not answer */
		}
		if ( knl_obdev_add((CONST UB *)b->name, NULL, &gp_ops, b, &b->uuid) >= E_OK ) {
			b->there = TRUE;
			b->last = inputs(b);
		}
	}
	if ( gp_bank[0].there ) {
		gp_led_key = ob_opn_obj(&gp_bank[0].uuid, OB_OP_READ | OB_OP_WRITE);
		if ( gp_led_key < E_OK ) gp_led_key = 0;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)poll_task;
	ctsk.itskpri = POLL_PRI;
	ctsk.stksz = 4096;
	tid = tk_cre_tsk(&ctsk);
	if ( tid > 0 ) {
		(void)tk_sta_tsk(tid, 0);
	}
}

#else /* RPI5 */

EXPORT void knl_obgpio_start( void )
{
}

EXPORT ER knl_obgpio_led( UINT pin, BOOL on )
{
	(void)pin;
	(void)on;
	return E_NOEXS;
}

EXPORT ER knl_obgpio_uuid( INT k, TS_UUID *p_uuid )
{
	(void)k;
	(void)p_uuid;
	return E_NOEXS;
}

#endif /* RPI5 */
