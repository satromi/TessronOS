/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	gpio_rp1.c
 *	RP1 general purpose pins (design 4.6, 7): bank 0 is the 40 pin
 *	header (pins 0 to 27); banks 1 and 2 (28 to 33, 34 to 53) are wired
 *	on the board, among them the reset of the Ethernet PHY (pin 32).
 *
 *	The RP1 sits behind the x4 PCIe link. With `pciex4_reset=0` in
 *	config.txt the firmware leaves the link up and the peripheral
 *	window in place, so the registers are reached through the device
 *	window at RP1_WINDOW_PA without enumerating the bus (design 4.6).
 *
 *	Three blocks take part in one pin, one of each per bank, the banks
 *	0x4000 apart:
 *	  io_bankN	status and control, two words per pin. The control
 *			word picks the function; 5 is "software control",
 *			which is what a plain pin uses.
 *	  sys_rioN	the value, the direction and the input, with the
 *			atomic set, clear and flip aliases the RP1 offers.
 *	  pads_bankN	the pad itself: input enable, output disable, pull
 *			up and pull down, drive strength.
 *
 *	NOT VERIFIED ON HARDWARE (docs/private/checklists/phase7.md item 28).
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/gpio.h>
#include "sysdepend.h"

#define RP1_GPIO_PINS		54		/* banks 0, 1 and 2 */
#define RP1_BANK_STEP		0x4000		/* from one bank's block to the next */

/* which bank a pin is in, and its place in it */
#define PIN_BANK(n)		( ( (n) < 28 ) ? 0 : ( (n) < 34 ) ? 1 : 2 )
#define PIN_IDX(n)		( (n) - ( ( (n) < 28 ) ? 0 : ( (n) < 34 ) ? 28 : 34 ) )
#define BANK_OFS(n)		( (UBINT)PIN_BANK(n) * RP1_BANK_STEP )

/* io_bankN: two words per pin */
#define RP1_GPIO_STATUS(n)	(RP1_IO_BANK0_BASE + BANK_OFS(n) + (UBINT)PIN_IDX(n) * 8)
#define RP1_GPIO_CTRL(n)	(RP1_IO_BANK0_BASE + BANK_OFS(n) + (UBINT)PIN_IDX(n) * 8 + 4)

#define RP1_CTRL_FUNCSEL_MASK	0x0000001f
/*
 * Overrides of what the function drives: the output (bits 13:12), its
 * enable (15:14) and the input (17:16). 0 in each is "as the function
 * says"; anything else pins the value whatever the function does, which
 * is how a pin can be left held when the firmware hands it over.
 */
#define RP1_CTRL_OVERRIDES	0x0003f000
#define RP1_FUNC_SW		5		/* driven from sys_rio */
#define RP1_FUNC_NONE		31		/* the pin is left alone */

/*
 * sys_rioN. The RP1 gives every block three more views of itself:
 * +0x1000 flips the bits written, +0x2000 sets them, +0x3000 clears
 * them, each without a read first. (Taking the flip for the set leaves
 * a pin at the opposite of what it was told, seen on the board.)
 */
#define RP1_RIO_XOR		0x1000
#define RP1_RIO_SET		0x2000
#define RP1_RIO_CLR		0x3000

#define RP1_RIO_OUT(n)		(RP1_SYS_RIO0_BASE + BANK_OFS(n) + 0x00)
#define RP1_RIO_OE(n)		(RP1_SYS_RIO0_BASE + BANK_OFS(n) + 0x04)
#define RP1_RIO_IN(n)		(RP1_SYS_RIO0_BASE + BANK_OFS(n) + 0x08)
#define RP1_RIO_BIT(n)		(1UL << PIN_IDX(n))

/* pads_bankN: one word per pin, after a word of its own */
#define RP1_PAD(n)		(RP1_PADS_BANK0_BASE + BANK_OFS(n) + 4 + (UBINT)PIN_IDX(n) * 4)

#define RP1_PAD_SLEWFAST	0x00000001
#define RP1_PAD_SCHMITT		0x00000002
#define RP1_PAD_PULLDOWN	0x00000004
#define RP1_PAD_PULLUP		0x00000008
#define RP1_PAD_DRIVE_MASK	0x00000030
#define RP1_PAD_IN_ENABLE	0x00000040
#define RP1_PAD_OUT_DISABLE	0x00000080

LOCAL BOOL rp1_ready = FALSE;

LOCAL BOOL pin_valid( UINT pin )
{
	return ( rp1_ready && pin < RP1_GPIO_PINS );
}

/*
 * Put a pin under software control and open its pad in the direction
 * asked for.
 */
EXPORT ER rp1_gpio_set_dir( UINT pin, UINT dir )
{
	UW	v;

	if ( !pin_valid(pin) || dir > GPIO_DIR_OUT ) {
		return E_PAR;
	}
	v = in_w(RP1_GPIO_CTRL(pin));
	v = (v & ~(RP1_CTRL_FUNCSEL_MASK | RP1_CTRL_OVERRIDES)) | RP1_FUNC_SW;
	out_w(RP1_GPIO_CTRL(pin), v);

	v = in_w(RP1_PAD(pin));
	if ( dir == GPIO_DIR_IN ) {
		v |=  RP1_PAD_IN_ENABLE;
		v |=  RP1_PAD_OUT_DISABLE;
	} else {
		v &= ~RP1_PAD_OUT_DISABLE;
		v |=  RP1_PAD_IN_ENABLE;	/* so that a read tells what the pin is at */
	}
	out_w(RP1_PAD(pin), v);

	/* the direction itself lives in sys_rio */
	out_w((( dir == GPIO_DIR_OUT ) ? RP1_RIO_OE(pin) + RP1_RIO_SET
				       : RP1_RIO_OE(pin) + RP1_RIO_CLR), RP1_RIO_BIT(pin));

	return E_OK;
}

EXPORT ER rp1_gpio_write( UINT pin, UINT value )
{
	if ( !pin_valid(pin) ) {
		return E_PAR;
	}
	out_w((( value != 0 ) ? RP1_RIO_OUT(pin) + RP1_RIO_SET
			      : RP1_RIO_OUT(pin) + RP1_RIO_CLR), RP1_RIO_BIT(pin));

	return E_OK;
}

EXPORT INT rp1_gpio_read( UINT pin )
{
	if ( !pin_valid(pin) ) {
		return E_PAR;
	}
	return ( in_w(RP1_RIO_IN(pin)) & RP1_RIO_BIT(pin) ) ? 1 : 0;
}

/*
 * The pull the pin is held at when nothing drives it.
 */
EXPORT ER rp1_gpio_set_pull( UINT pin, UINT pull )
{
	UW	v;

	if ( !pin_valid(pin) || pull > GPIO_PULL_DOWN ) {
		return E_PAR;
	}
	v = in_w(RP1_PAD(pin)) & ~(RP1_PAD_PULLUP | RP1_PAD_PULLDOWN);
	if ( pull == GPIO_PULL_UP )   v |= RP1_PAD_PULLUP;
	if ( pull == GPIO_PULL_DOWN ) v |= RP1_PAD_PULLDOWN;
	out_w(RP1_PAD(pin), v);

	return E_OK;
}

/*
 * Hand a pin to one of the RP1's own functions (a UART, SPI and so on).
 * The numbers are per pin; the device tree of the board names them.
 */
EXPORT ER rp1_gpio_set_func( UINT pin, UINT func )
{
	UW	v;

	if ( !pin_valid(pin) || func > 31 ) {
		return E_PAR;
	}
	v = in_w(RP1_GPIO_CTRL(pin));
	out_w(RP1_GPIO_CTRL(pin), (v & ~RP1_CTRL_FUNCSEL_MASK) | func);

	return E_OK;
}

/*
 * What a pin is set to, for whoever has to find out why it does not do
 * what it was told: its control word, its pad, and the output and
 * output enable words of its bank.
 */
EXPORT ER rp1_gpio_state( UINT pin, UW st[4] )
{
	if ( !pin_valid(pin) ) {
		return E_PAR;
	}
	st[0] = in_w(RP1_GPIO_CTRL(pin));
	st[1] = in_w(RP1_PAD(pin));
	st[2] = in_w(RP1_RIO_OUT(pin));
	st[3] = in_w(RP1_RIO_OE(pin));
	return E_OK;
}

/*
 * The window is left in place by the firmware, so there is nothing to
 * bring up; the check is that the block answers at all. A read of a
 * control word of a pin that exists must not come back as all ones,
 * which is what an unmapped window gives.
 */
EXPORT ER knl_rp1_gpio_init( void )
{
	UW	v;

	rp1_ready = TRUE;
	v = in_w(RP1_GPIO_CTRL(0));
	if ( v == 0xFFFFFFFF ) {
		rp1_ready = FALSE;
		return E_NOEXS;			/* the link is down or not mapped */
	}

	return E_OK;
}

#endif /* RPI5 */
