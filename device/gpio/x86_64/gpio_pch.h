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
 *	The GPIO pads of an Intel PCH as outputs (gpio_pch.c): a pin named
 *	the way the ACPI namespace names it (a GpioIo's controller path and
 *	pin number), driven high or low.
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

#endif /* _DEVICE_GPIO_X86_64_GPIO_PCH_ */
