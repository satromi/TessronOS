/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio.h
 *	General purpose pins (design 4.10.1, 7)
 *
 *	The always-on block of the BCM2712 and the pins behind the RP1, where
 *	the 40 pin header goes; on the Raspberry Pi 4 the one block of the
 *	BCM2711. These are the drivers' own calls: a program,
 *	and the kernel once the object layer is up, set the pins through the
 *	objects of the banks (OB_S_GPIO, peripheral_kernel/obj/obgpio.c).
 */

#ifndef __TS_GPIO_H__
#define __TS_GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_DIR_IN	0
#define GPIO_DIR_OUT	1

#define GPIO_PULL_NONE	0
#define GPIO_PULL_UP	1
#define GPIO_PULL_DOWN	2

IMPORT ER  gpio_set_dir( UINT bank, UINT pin, UINT dir );
IMPORT ER  gpio_write( UINT bank, UINT pin, UINT value );
IMPORT INT gpio_read( UINT bank, UINT pin );		/* 0, 1 or an error */
IMPORT INT gpio_get_dir( UINT bank, UINT pin );		/* GPIO_DIR_* or an error */
IMPORT INT gpio_bank_pins( UINT bank );			/* 0: no such bank */

/*
 * The pulls, on the Raspberry Pi 4 (device/gpio/gpio_bcm2711.c), whose
 * one bank 0 holds all 58 pins by their GPIO number. There gpio_get_dir
 * answers 2 + the alternate function of a pin that has one.
 */
IMPORT ER  gpio_set_pull( UINT bank, UINT pin, UINT pull );
IMPORT INT gpio_get_pull( UINT bank, UINT pin );	/* GPIO_PULL_* or an error */

/*
 * An edge or a level of a pin of the header (0..27) told by the bank's
 * interrupt, on the Raspberry Pi 4 (device/gpio/gpio_bcm2711.c): fn is
 * called from the interrupt handler with the pin and ctx. A level is
 * told once: its detection is set again by calling once more after the
 * cause is dealt with. GPIO_IRQ_NONE turns it off.
 */
#define GPIO_IRQ_NONE		0
#define GPIO_IRQ_RISING		1
#define GPIO_IRQ_FALLING	2
#define GPIO_IRQ_LOW		3
#define GPIO_IRQ_HIGH		4
IMPORT ER  gpio_irq_set( UINT bank, UINT pin, INT mode, void (*fn)( UINT pin, void *ctx ), void *ctx );

/*
 * A pin a driver of the kernel takes for itself, which the objects of
 * the banks then show as the kernel's and do not set (Raspberry Pi 4)
 */
IMPORT ER   gpio_claim( UINT bank, UINT pin );
IMPORT BOOL gpio_claimed_by_kernel( UINT bank, UINT pin );

/* The activity LED of the board */
IMPORT ER   gpio_led( BOOL on );
IMPORT void gpio_led_blink( INT times, UINT ms );

IMPORT ER  knl_gpio_init( void );

/*
 * The pins of the 40 pin header, behind the RP1 (device/gpio/gpio_rp1.c)
 */
IMPORT ER  rp1_gpio_set_dir( UINT pin, UINT dir );
IMPORT ER  rp1_gpio_write( UINT pin, UINT value );
IMPORT INT rp1_gpio_read( UINT pin );
IMPORT ER  rp1_gpio_set_pull( UINT pin, UINT pull );
IMPORT ER  rp1_gpio_set_func( UINT pin, UINT func );
IMPORT ER  rp1_gpio_state( UINT pin, UW st[4] );	/* control, pad, bank output, bank output enable */
IMPORT ER  knl_rp1_gpio_init( void );

/* UART0 of the RP1, on the header pins (device/ser/uart_rp1.c) */
IMPORT ER  rp1_uart_init( UW baud );
IMPORT ER  rp1_uart_console( UW baud );

#ifdef __cplusplus
}
#endif

#endif /* __TS_GPIO_H__ */
