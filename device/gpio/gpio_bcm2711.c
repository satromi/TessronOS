/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_bcm2711.c
 *	The general purpose pins of the BCM2711 (design 4.10.1, 7;
 *	docs/tessronos/03-platform.md 3.6).
 *
 *	One block holds all 58 pins. Each has three bits of function
 *	select (input, output or one of six alternate functions), a bit in
 *	the set, clear and level registers, and two bits of pull (none, up,
 *	down) that, unlike the older chips, can be read back. They are
 *	addressed here as bank 0, pin = the GPIO number:
 *
 *	   0..27	the 40 pin header
 *	  28..57	the board's own wiring: the Ethernet PHY's MDIO and
 *			RGMII, the Bluetooth UART, the Wi-Fi's SDIO, the
 *			audio PWM, the activity LED (42)
 *
 *	The activity LED is lit when its pin is high. On the Raspberry Pi
 *	400 the same pin is the power LED, which is left lit once the blinks
 *	that tell how far the start went are over (include/ts/board.h).
 *
 *	Read-modify-write of the function select and pull registers is done
 *	under a spin lock with interrupts off; the set and clear registers
 *	take a write of the one bit and need no lock.
 *
 *	A pin of the header (0..27) can tell of an edge or a level on it
 *	(gpio_irq_set): its detection is enabled (GPREN, GPFEN, GPHEN, GPLEN)
 *	and its event latched in GPEDS raises the block's first interrupt
 *	(SPI 113, level high). The handler clears each event and calls the
 *	pin's function. A level would latch again at once while it holds, so
 *	its detection is turned off as it is taken, and the driver sets it
 *	again once it has dealt with the cause; an edge stays enabled, and
 *	setting the same again leaves an event latched meanwhile in place.
 *
 *	A pin a driver of the kernel takes for itself (gpio_claim: a touch
 *	screen's bus) is shown as the kernel's in the objects of the banks,
 *	which do not set it (peripheral_kernel/obj/obgpio.c).
 */

#include <sys/machine.h>

#ifdef RPI4

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/gpio.h>
#include <ts/board.h>
#include "sysdepend.h"

LOCAL T_SPLOCK	gpio_lock;

#define GPFSEL(n)		(GPIO_BASE + 0x00 + (UBINT)(n) * 4)	/* 10 pins each */
#define GPSET(n)		(GPIO_BASE + 0x1c + (UBINT)(n) * 4)	/* 32 pins each */
#define GPCLR(n)		(GPIO_BASE + 0x28 + (UBINT)(n) * 4)
#define GPLEV(n)		(GPIO_BASE + 0x34 + (UBINT)(n) * 4)
#define GPPUPPDN(n)		(GPIO_BASE + 0xe4 + (UBINT)(n) * 4)	/* 16 pins each */
#define GPEDS(n)		(GPIO_BASE + 0x40 + (UBINT)(n) * 4)	/* events latched; 1 written clears */
#define GPREN(n)		(GPIO_BASE + 0x4c + (UBINT)(n) * 4)	/* rising edge detection */
#define GPFEN(n)		(GPIO_BASE + 0x58 + (UBINT)(n) * 4)	/* falling edge */
#define GPHEN(n)		(GPIO_BASE + 0x64 + (UBINT)(n) * 4)	/* high level */
#define GPLEN(n)		(GPIO_BASE + 0x70 + (UBINT)(n) * 4)	/* low level */
#define GPAREN(n)		(GPIO_BASE + 0x7c + (UBINT)(n) * 4)	/* rising edge, not synchronised */
#define GPAFEN(n)		(GPIO_BASE + 0x88 + (UBINT)(n) * 4)	/* falling edge, not synchronised */

#define IRQ_PINS		28			/* the header's: the block's first interrupt */

/* Function select codes: 0 input, 1 output, then the alternates */
#define FSEL_IN			0
#define FSEL_OUT		1
#define FSEL_MASK		7

/* Pull codes */
#define PULL_NONE		0
#define PULL_UP			1
#define PULL_DOWN		2

LOCAL BOOL gpio_valid( UINT bank, UINT pin )
{
	return ( bank == 0 && pin < GPIO_NPINS );
}

LOCAL UW fsel_get( UINT pin )
{
	return ( in_w(GPFSEL(pin / 10)) >> ( ( pin % 10 ) * 3 ) ) & FSEL_MASK;
}

LOCAL void fsel_set( UINT pin, UW code )
{
	UINT	imask;
	UW	v;
	UINT	sh = ( pin % 10 ) * 3;

	ISpinLock(&gpio_lock, &imask);
	v = in_w(GPFSEL(pin / 10));
	v = ( v & ~( (UW)FSEL_MASK << sh ) ) | ( ( code & FSEL_MASK ) << sh );
	out_w(GPFSEL(pin / 10), v);
	ISpinUnlock(&gpio_lock, &imask);
}

/*
 * A pin given to a block of the chip by a driver of the kernel (the
 * Wi-Fi's SDIO): the value of its function select field, as the device
 * tree's brcm,function has it.
 */
EXPORT void knl_gpio_func( UINT pin, UW fsel )
{
	if ( pin < GPIO_NPINS ) {
		fsel_set(pin, fsel);
	}
}

EXPORT ER gpio_set_dir( UINT bank, UINT pin, UINT dir )
{
	if ( !gpio_valid(bank, pin) || dir > GPIO_DIR_OUT ) {
		return E_PAR;
	}
	fsel_set(pin, ( dir == GPIO_DIR_OUT ) ? FSEL_OUT : FSEL_IN);
	return E_OK;
}

EXPORT ER gpio_write( UINT bank, UINT pin, UINT value )
{
	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	if ( value != 0 ) {
		out_w(GPSET(pin / 32), 1UL << ( pin % 32 ));
	} else {
		out_w(GPCLR(pin / 32), 1UL << ( pin % 32 ));
	}
	return E_OK;
}

EXPORT INT gpio_read( UINT bank, UINT pin )
{
	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	return ( in_w(GPLEV(pin / 32)) & ( 1UL << ( pin % 32 ) ) ) ? 1 : 0;
}

/* Which way a pin goes: GPIO_DIR_IN or GPIO_DIR_OUT, or 2 + the alternate function it has */
EXPORT INT gpio_get_dir( UINT bank, UINT pin )
{
	UW	f;

	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	f = fsel_get(pin);
	switch ( f ) {
	case FSEL_IN:	return GPIO_DIR_IN;
	case FSEL_OUT:	return GPIO_DIR_OUT;
	case 4:		return 2 + 0;		/* alternate 0 .. 3 are 4 .. 7 */
	case 5:		return 2 + 1;
	case 6:		return 2 + 2;
	case 7:		return 2 + 3;
	case 3:		return 2 + 4;		/* alternate 4 */
	default:	return 2 + 5;		/* 2: alternate 5 */
	}
}

EXPORT ER gpio_set_pull( UINT bank, UINT pin, UINT pull )
{
	UINT	imask, sh;
	UW	v, code;

	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	switch ( pull ) {
	case GPIO_PULL_NONE:	code = PULL_NONE;	break;
	case GPIO_PULL_UP:	code = PULL_UP;		break;
	case GPIO_PULL_DOWN:	code = PULL_DOWN;	break;
	default:		return E_PAR;
	}
	sh = ( pin % 16 ) * 2;
	ISpinLock(&gpio_lock, &imask);
	v = in_w(GPPUPPDN(pin / 16));
	v = ( v & ~( 3UL << sh ) ) | ( code << sh );
	out_w(GPPUPPDN(pin / 16), v);
	ISpinUnlock(&gpio_lock, &imask);
	return E_OK;
}

EXPORT INT gpio_get_pull( UINT bank, UINT pin )
{
	UW	code;

	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	code = ( in_w(GPPUPPDN(pin / 16)) >> ( ( pin % 16 ) * 2 ) ) & 3;
	return ( code == PULL_UP ) ? GPIO_PULL_UP : ( code == PULL_DOWN ) ? GPIO_PULL_DOWN : GPIO_PULL_NONE;
}

/* How many pins a bank has, 0 for one there is not */
EXPORT INT gpio_bank_pins( UINT bank )
{
	return ( bank == 0 ) ? GPIO_NPINS : 0;
}

/* ---------------------------------------------------------------- events of the pins */

typedef struct {
	INT	mode;			/* GPIO_IRQ_* */
	void	(*fn)( UINT pin, void *ctx );
	void	*ctx;
} PINIRQ;

LOCAL PINIRQ	pin_irq[IRQ_PINS];
LOCAL BOOL	irq_defined = FALSE;
LOCAL UD	gpio_claimed = 0;		/* pins of bank 0 a driver took, bit per pin */

/* The detection register of a mode */
LOCAL UBINT detect_reg( INT mode )
{
	switch ( mode ) {
	case GPIO_IRQ_RISING:	return GPREN(0);
	case GPIO_IRQ_FALLING:	return GPFEN(0);
	case GPIO_IRQ_HIGH:	return GPHEN(0);
	default:		return GPLEN(0);
	}
}

LOCAL void detect_off( UINT pin )
{
	UW	bit = 1UL << pin;

	out_w(GPREN(0), in_w(GPREN(0)) & ~bit);
	out_w(GPFEN(0), in_w(GPFEN(0)) & ~bit);
	out_w(GPHEN(0), in_w(GPHEN(0)) & ~bit);
	out_w(GPLEN(0), in_w(GPLEN(0)) & ~bit);
	out_w(GPAREN(0), in_w(GPAREN(0)) & ~bit);
	out_w(GPAFEN(0), in_w(GPAFEN(0)) & ~bit);
}

LOCAL void gpio_inthdr( UINT intno )
{
	UW	ev, bit;
	UINT	pin, imask;
	PINIRQ	pi;

	ev = in_w(GPEDS(0)) & ( ( 1UL << IRQ_PINS ) - 1 );
	for ( pin = 0; ev != 0 && pin < IRQ_PINS; pin++ ) {
		bit = 1UL << pin;
		if ( !( ev & bit ) ) continue;
		ev &= ~bit;
		ISpinLock(&gpio_lock, &imask);
		pi = pin_irq[pin];
		if ( pi.fn == NULL ) {
			detect_off(pin);		/* nobody's: not again */
		} else if ( pi.mode == GPIO_IRQ_LOW || pi.mode == GPIO_IRQ_HIGH ) {
			out_w(detect_reg(pi.mode), in_w(detect_reg(pi.mode)) & ~bit);
		}
		out_w(GPEDS(0), bit);
		ISpinUnlock(&gpio_lock, &imask);
		if ( pi.fn != NULL ) {
			pi.fn(pin, pi.ctx);
		}
	}
	EndOfInt(intno);
}

EXPORT ER gpio_irq_set( UINT bank, UINT pin, INT mode, void (*fn)( UINT pin, void *ctx ), void *ctx )
{
	UINT	imask;
	UW	bit = 1UL << pin;
	BOOL	same;

	if ( bank != 0 || pin >= IRQ_PINS || mode < GPIO_IRQ_NONE || mode > GPIO_IRQ_HIGH
	  || ( mode != GPIO_IRQ_NONE && fn == NULL ) ) {
		return E_PAR;
	}
	if ( !irq_defined && mode != GPIO_IRQ_NONE ) {
		knl_define_inthdr(GPIO_INTNO0, TA_HLNG, (FP)gpio_inthdr);
		SetIntMode(GPIO_INTNO0, IM_LEVEL);
		EnableInt(GPIO_INTNO0, INTPRI_DEVICE);
		irq_defined = TRUE;
	}
	ISpinLock(&gpio_lock, &imask);
	same = ( mode != GPIO_IRQ_NONE && pin_irq[pin].fn == fn && pin_irq[pin].ctx == ctx
	      && pin_irq[pin].mode == mode && ( in_w(detect_reg(mode)) & bit ) != 0 );
	if ( !same ) {
		detect_off(pin);
		out_w(GPEDS(0), bit);
		pin_irq[pin].mode = mode;
		pin_irq[pin].fn = ( mode != GPIO_IRQ_NONE ) ? fn : NULL;
		pin_irq[pin].ctx = ctx;
		if ( mode != GPIO_IRQ_NONE ) {
			out_w(detect_reg(mode), in_w(detect_reg(mode)) | bit);
		}
	}
	ISpinUnlock(&gpio_lock, &imask);
	return E_OK;
}

EXPORT ER gpio_claim( UINT bank, UINT pin )
{
	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	__atomic_or_fetch(&gpio_claimed, 1ULL << pin, __ATOMIC_SEQ_CST);
	return E_OK;
}

EXPORT BOOL gpio_claimed_by_kernel( UINT bank, UINT pin )
{
	return ( gpio_valid(bank, pin) && ( __atomic_load_n(&gpio_claimed, __ATOMIC_SEQ_CST) & ( 1ULL << pin ) ) != 0 );
}

/*
 * The activity LED, lit when high. Once the object layer is up it is set
 * through the object of its bank (knl_obgpio_led); before that, while
 * the system starts, directly.
 */
IMPORT ER knl_obgpio_led( UINT pin, BOOL on );

EXPORT ER gpio_led( BOOL on )
{
	ER	er = knl_obgpio_led(GPIO_LED_ACT, on);

	if ( er != E_NOEXS ) {
		return er;
	}
	(void)gpio_write(0, GPIO_LED_ACT, on ? 1 : 0);
	return gpio_set_dir(0, GPIO_LED_ACT, GPIO_DIR_OUT);
}

/*
 * Blink the LED a given number of times. Used during bring-up to say
 * how far the kernel reached without a console: the count is the signal.
 * A board whose one LED is its power LED has it lit again afterwards.
 */
EXPORT void gpio_led_blink( INT times, UINT ms )
{
	INT	i;

	for ( i = 0; i < times; i++ ) {
		gpio_led(TRUE);
		tk_dly_tsk(ms);
		gpio_led(FALSE);
		tk_dly_tsk(ms);
	}
	if ( knl_board.led_power ) {
		gpio_led(TRUE);
	}
}

EXPORT ER knl_gpio_init( void )
{
	/* the LED pin an output, whatever the firmware left it as */
	(void)gpio_write(0, GPIO_LED_ACT, knl_board.led_power ? 1 : 0);
	return gpio_set_dir(0, GPIO_LED_ACT, GPIO_DIR_OUT);
}

#endif /* RPI4 */
