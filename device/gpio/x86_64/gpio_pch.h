/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_pch.h (x64 PC)
 *	The GPIO pads of an Intel PCH (gpio_pch.c): a pin named the way the
 *	ACPI namespace names it (a GpioIo's or GpioInt's controller path and
 *	pin number), driven high or low, or taken as an interrupt.
 */

#ifndef _DEVICE_GPIO_X86_64_GPIO_PCH_
#define _DEVICE_GPIO_X86_64_GPIO_PCH_

/*
 * The pin of the controller ctrl made an output at the level high. E_NOEXS
 * when ctrl is no controller this knows or has no such pin, E_OBJ when
 * the pad is not a GPIO (its native function chosen) or is not the
 * host's, E_OK otherwise (a pad whose output the firmware locked is
 * said, and written all the same).
 */
IMPORT ER	knl_pchgpio_out( CONST char *ctrl, UINT pin, BOOL high );
/* The pad of the pin, said on the console after what: its configuration, ownership and locks */
IMPORT void	knl_pchgpio_say( CONST char *ctrl, UINT pin, CONST char *what );

/* How a pad's input interrupts: the values of a GpioInt as knl_acpica_gpioint gives them */
#define PCHGPIO_EDGE		1
#define PCHGPIO_LOW		2
#define PCHGPIO_BOTH		4

/*
 * The pin taken as an input that interrupts, masked for now. fn runs in the
 * interrupt handler. A pad whose event is its level comes masked: enable it
 * again when its cause is dealt with. NULL when this controller's interrupts
 * are not known, or the pad is not free for it (a native function, the ME's or
 * ACPI's, or locked in another setting); nothing has been changed on the pad then.
 */
IMPORT void	*knl_pchgpio_int_take( CONST char *ctrl, UINT pin, UINT mode, void (*fn)( void *arg ), void *arg );
/* A taken pad's interrupt enabled or masked */
IMPORT void	knl_pchgpio_int_on( void *pad, BOOL on );

#endif /* _DEVICE_GPIO_X86_64_GPIO_PCH_ */
