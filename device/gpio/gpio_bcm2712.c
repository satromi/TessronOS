/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_bcm2712.c
 *	Always-on GPIO of the BCM2712 (design 4.10.1, 7).
 *
 *	The gio_aon block holds the pins that stay powered: bank 0 has 17
 *	of them and bank 1 has 6. The activity LED of the board is bank 0
 *	bit 9 and lights when the pin is driven low. This is the first
 *	thing to reach for when bringing the board up, because it needs no
 *	console.
 *
 *	NOT VERIFIED ON HARDWARE. The addresses and the bit come from the
 *	device tree (docs/private/tessronos-design/04, HW-06);
 *	whether the firmware leaves the pin in GPIO mode has to be checked
 *	on the board itself.
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/gpio.h>
#include "sysdepend.h"

/*
 * Taken with interrupts disabled around the read-modify-write of the GPIO registers: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	gpio_lock;

/* Registers of one bank, 0x20 bytes apart */
#define GIO_BANK(b)		(GIO_AON_BASE + (UBINT)(b) * 0x20)
#define GIO_ODEN(b)		(GIO_BANK(b) + 0x00)	/* open drain */
#define GIO_DATA(b)		(GIO_BANK(b) + 0x04)
#define GIO_IODIR(b)		(GIO_BANK(b) + 0x08)	/* 1: input, 0: output */
#define GIO_EC(b)		(GIO_BANK(b) + 0x0c)
#define GIO_EI(b)		(GIO_BANK(b) + 0x10)
#define GIO_MASK(b)		(GIO_BANK(b) + 0x14)
#define GIO_LEVEL(b)		(GIO_BANK(b) + 0x18)
#define GIO_STAT(b)		(GIO_BANK(b) + 0x1c)

#define GIO_BANK_NUM		2
#define GIO_BANK0_PINS		17
#define GIO_BANK1_PINS		6

/* The activity LED: bank 0, bit 9, lit when low */
#define LED_BANK		0
#define LED_BIT			9

LOCAL BOOL gpio_valid( UINT bank, UINT pin )
{
	if ( bank >= GIO_BANK_NUM ) return FALSE;
	return ( pin < (( bank == 0 ) ? GIO_BANK0_PINS : GIO_BANK1_PINS) );
}

EXPORT ER gpio_set_dir( UINT bank, UINT pin, UINT dir )
{
	UW	v;
	UINT	imask;

	if ( !gpio_valid(bank, pin) || dir > GPIO_DIR_OUT ) {
		return E_PAR;
	}
	ISpinLock(&gpio_lock, &imask);
	v = in_w(GIO_IODIR(bank));
	if ( dir == GPIO_DIR_IN ) {
		v |= (1UL << pin);
	} else {
		v &= ~(1UL << pin);
	}
	out_w(GIO_IODIR(bank), v);
	ISpinUnlock(&gpio_lock, &imask);

	return E_OK;
}

EXPORT ER gpio_write( UINT bank, UINT pin, UINT value )
{
	UW	v;
	UINT	imask;

	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	ISpinLock(&gpio_lock, &imask);
	v = in_w(GIO_DATA(bank));
	if ( value != 0 ) {
		v |= (1UL << pin);
	} else {
		v &= ~(1UL << pin);
	}
	out_w(GIO_DATA(bank), v);
	ISpinUnlock(&gpio_lock, &imask);

	return E_OK;
}

EXPORT INT gpio_read( UINT bank, UINT pin )
{
	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	return ( in_w(GIO_DATA(bank)) & (1UL << pin) ) ? 1 : 0;
}

/* Which way a pin goes: GPIO_DIR_IN or GPIO_DIR_OUT */
EXPORT INT gpio_get_dir( UINT bank, UINT pin )
{
	if ( !gpio_valid(bank, pin) ) {
		return E_PAR;
	}
	return ( in_w(GIO_IODIR(bank)) & (1UL << pin) ) ? GPIO_DIR_IN : GPIO_DIR_OUT;
}

/* How many pins a bank has, 0 for one there is not */
EXPORT INT gpio_bank_pins( UINT bank )
{
	return ( bank >= GIO_BANK_NUM ) ? 0 : ( bank == 0 ) ? GIO_BANK0_PINS : GIO_BANK1_PINS;
}

/*
 * The activity LED. It is wired the other way round: low lights it.
 * Once the object layer is up it is set through the object of its bank
 * (knl_obgpio_led); before that, while the system starts, directly.
 */
IMPORT ER knl_obgpio_led( UINT pin, BOOL on );

EXPORT ER gpio_led( BOOL on )
{
	ER	er = knl_obgpio_led(LED_BIT, on);

	if ( er != E_NOEXS ) {
		return er;
	}
	er = gpio_set_dir(LED_BANK, LED_BIT, GPIO_DIR_OUT);
	if ( er < E_OK ) {
		return er;
	}
	return gpio_write(LED_BANK, LED_BIT, on ? 0 : 1);
}

/*
 * Blink the LED a given number of times. Used during bring-up to say
 * how far the kernel reached without a console: the count is the signal.
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
}

EXPORT ER knl_gpio_init( void )
{
	/* The firmware leaves the LED pin as an output; nothing to set up
	   beyond that until the pin multiplexer is handled (HW-06). */
	return E_OK;
}

#endif /* RPI5 */
