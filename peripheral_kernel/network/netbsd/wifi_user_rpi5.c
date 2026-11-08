/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifi_user_rpi5.c (Raspberry Pi 5, Raspberry Pi 4)
 *	The hypercalls of the Wi-Fi's SDIO host inside the rump kernel (design 9.6.8).
 *
 *	The Wi-Fi chip (CYW43455) is on the BCM2712's second SD host (sdio2,
 *	/axi/mmc@1100000), apart from the boot card's (device/sd/sdhci.c,
 *	which this does not touch). NetBSD's sdhc and sdmmc run that host in
 *	the rump kernel and bwfm(4) the chip (rumpcomp/wifi/tswifi_sdio.c);
 *	they reach the machine through these:
 *
 *	On the Raspberry Pi 4, the Pi 400 and the Compute Module 4 the chip
 *	(CYW43455) is on the BCM2711's older EMMC block
 *	(/soc/mmcnr@7e300000); the card slot is
 *	EMMC2. The block has no registers of its own to set; its pins are GPIO
 *	34 to 39, the chip's power a line of the firmware's GPIO expander and
 *	its clock the firmware's, so that the board's part below is another,
 *	and the rest is the same. The Pi 400 and some Compute Modules have
 *	the CYW43456, whose firmware the system does not include: such a
 *	machine has no Wi-Fi (knl_wifi5_no_firmware).
 *
 *	- the host, as the device tree says it: the registers, the base
 *	  clock (the fixed clock its "clocks" names), the interrupt. Before
 *	  the host is given to sdhc, the board is made ready as the device
 *	  tree describes it: GPIO 30 to 35 to the host (sdio2_30_pins), the
 *	  chip's power (wl-on-reg, GPIO 28 of gpio@7d508500) taken low and
 *	  then high with the delay the regulator asks for, and the BCM2712's
 *	  own registers of the block (cfg) set for a card that is always
 *	  there. A machine without that node has no Wi-Fi, and nothing of
 *	  it is touched.
 *	- the interrupt, a level one: taken by a handler that masks it and
 *	  wakes the thread that runs sdhc's handler, and unmasked again when
 *	  that is done.
 *	- memory a bus master may reach, and the cache work its
 *	  synchronisation asks for (the host does not snoop the caches).
 *	- the files bwfm asks for by name: the firmware and the regulatory
 *	  data from their objects (record 1), and the board's settings read
 *	  out of their document (record 0, wifi_nvram.c).
 *
 *	The Wi-Fi manager (device/net/wifi/wifi.c) starts the chip once the
 *	object layer is up, and is given the frames the chip receives. Each
 *	step of the bring-up says a line that starts with "wifi:", so that
 *	one start shows on the console how far the Wi-Fi got.
 */

#include <sys/machine.h>

#if defined(USE_WIFI) && !defined(X64)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "sysman/pfalloc.h"
#ifdef RPI4
#include <ts/gpio.h>
#include <ts/board.h>
#endif
#include "tstdlib.h"
#include "sysdepend.h"
#include "rumpglue.h"

/*
 * The rump kernel's virtual processor is let go while these wait; with
 * OpenBSD's layer they are called from outside the rump kernel
 */
#ifdef WIFI_OBSD
#define W_UNSCHED(n)	( (void)(n) )
#define W_SCHED(n)	( (void)(n) )
#else
#define W_UNSCHED(n)	knl_rump_unsched(&n)
#define W_SCHED(n)	knl_rump_sched(n)
#endif

/* NetBSD's errno values the hypercalls answer with */
#define NB_ENOENT	2
#define NB_ENXIO	6
#define NB_ENOMEM	12
#define NB_EINVAL	22

#ifdef RPI4
/* The node of the host, of its pins and of the firmware's GPIO expander */
#define W5_HOST_PATH	"/soc/mmcnr@7e300000"
#define W5_HOST_COMPAT	"brcm,bcm2835-mmc"
#define W5_SOC_PATH	"/soc"
#define W4_PINS_PATH	"/soc/gpio@7e200000/sdio_pins"
#define W4_FWGPIO_PATH	"/soc/firmware/gpio"
#else
/* The node of the host, of the pins and of the chip's power */
#define W5_HOST_PATH	"/axi/mmc@1100000"
#define W5_HOST_COMPAT	"brcm,bcm2712-sdhci"
#define W5_CLOCK_PATH	"/clocks/clk-emmc2"
#define W5_SOC_PATH	"/soc@107c000000"
#define W5_PINCTRL_PATH	"/soc@107c000000/pinctrl@7d504100"
#define W5_GPIO_PATH	"/soc@107c000000/gpio@7d508500"
#define W5_WLON_PATH	"/wl-on-reg"

/*
 * The BCM2712's registers of the block (cfg): the card's presence taken
 * from the register rather than a pin, the pads given the SD timing, and
 * the clock in MHz the controller counts its timer with
 */
#define CFG_CTRL		0x00
#define CFG_CTRL_SDCD_TEST_EN	0x80000000
#define CFG_CTRL_SDCD_TEST_LEV	0x40000000
#define CFG_SD_PIN_SEL		0x44
#define CFG_SD_PIN_SEL_MASK	0x3
#define CFG_SD_PIN_SEL_SD	0x2
#define CFG_CQ_CAPABILITY	0x4c
#define CFG_CQ_FMUL_SHIFT	12

/*
 * The pin control of the BCM2712 (brcm,bcm2712c0-pinctrl and -d0): each
 * pin's function a 4-bit field of a word, its pull a 2-bit one. The
 * places, and the function numbers of the host's lines, are those of the
 * two steppings, for the pins the device tree gives the host and the
 * chip's power.
 */
#define PULL_NONE	0
#define PULL_UP		2

typedef struct {
	UB	pin;
	UB	mux_w, mux_b;		/* the function: word mux_w, field mux_b (4 bits each) */
	UB	pad_w, pad_b;		/* the pull: word pad_w, field pad_b (2 bits each) */
	UB	fsel;			/* the function number: 0 is GPIO */
	UB	pull;
} W5_PIN;

LOCAL CONST W5_PIN	w5_pins_c0[] = {
	{ 28, 3, 4, 9,  5, 0, PULL_NONE },	/* WL_ON, a GPIO */
	{ 30, 3, 6, 9,  7, 4, PULL_NONE },	/* sd2 CLK */
	{ 31, 3, 7, 9,  8, 4, PULL_UP },	/* sd2 CMD */
	{ 32, 4, 0, 9,  9, 4, PULL_UP },	/* sd2 DAT0 */
	{ 33, 4, 1, 9, 10, 3, PULL_UP },	/* sd2 DAT1 */
	{ 34, 4, 2, 9, 11, 4, PULL_UP },	/* sd2 DAT2 */
	{ 35, 4, 3, 9, 12, 3, PULL_UP },	/* sd2 DAT3 */
};

LOCAL CONST W5_PIN	w5_pins_d0[] = {
	{ 28, 2, 4, 5, 10, 0, PULL_NONE },
	{ 30, 2, 6, 5, 12, 1, PULL_NONE },
	{ 31, 2, 7, 5, 13, 1, PULL_UP },
	{ 32, 3, 0, 5, 14, 1, PULL_UP },
	{ 33, 3, 1, 6,  0, 1, PULL_UP },
	{ 34, 3, 2, 6,  1, 1, PULL_UP },
	{ 35, 3, 3, 6,  2, 1, PULL_UP },
};

#define W5_NPINS	( sizeof(w5_pins_c0) / sizeof(w5_pins_c0[0]) )

/* A bank of the brcmstb GPIO block: 0x20 bytes, the data at 4, the direction at 8 (1: input) */
#define GIO_DATA(b)	( (b) * 0x20 + 0x04 )
#define GIO_IODIR(b)	( (b) * 0x20 + 0x08 )
#endif /* RPI4 */

/* The files bwfm asks for, and the object each comes from */
#define W5_FW_BIN	0
#define W5_FW_NVRAM	1

typedef struct {
	CONST char	*name;
	CONST char	*uuid;
	INT		kind;
} W5_FW;

LOCAL CONST W5_FW	w5_fw[] = {
	{ "brcmfmac43455-sdio.bin",		SYSDEF_FW_BWFM43455,	 W5_FW_BIN },
	{ "brcmfmac43455-sdio.clm_blob",	SYSDEF_FW_BWFM43455_CLM, W5_FW_BIN },
	{ "brcmfmac43455-sdio.txt",		SYSDEF_FW_BWFM43455_NV,	 W5_FW_NVRAM },
};

/* The Wi-Fi manager (device/net/wifi/wifi.c) */
IMPORT void	knl_wifi_rx( CONST UB *frame, INT len );

/* wifi_nvram.c */
IMPORT INT	knl_wifi_nvram( CONST UB *xml, SZ len, UB *out, SZ max, INT *p_drop );

LOCAL struct {
	BOOL		found;
	UD		host_pa, host_size;
	UD		cfg_pa;
	UINT		clk_khz;
	UINT		intno;
	ID		semid;
	volatile UW	ints;
	BOOL		probed;		/* the rump kernel asked for the host */
	BOOL		no_fw;		/* the driver asked for a chip's files the system does not have */
} w5;

/* ---------------------------------------------------------------- the device tree */

LOCAL UW w5_be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

/* Whether a string list property holds 's' */
LOCAL BOOL w5_has( CONST char *path, CONST char *prop, CONST char *s )
{
	CONST UB	*v;
	INT		len, i, k;

	v = knl_dtb_prop(path, prop, &len);
	for ( i = 0; v != NULL && i < len; i += k + 1 ) {
		for ( k = 0; i + k < len && v[i + k] != '\0'; k++ ) ;
		if ( knl_strcmp((CONST char *)&v[i], s) == 0 ) return TRUE;
	}
	return FALSE;
}

/* A node of the soc bus: its first register's physical address (through the bus's ranges) */
LOCAL UD w5_soc_pa( CONST char *path )
{
	CONST UB	*v;
	INT		len;
	UD		child, parent, size;

	v = knl_dtb_prop(path, "reg", &len);
	if ( v == NULL || len < 8 ) return 0;
	if ( knl_dtb_ranges(W5_SOC_PATH, "ranges", 0, &child, &parent, &size) < E_OK ) return 0;
	if ( w5_be32(v) < child || w5_be32(v) - child >= size ) return 0;
	return parent + ( w5_be32(v) - child );
}

/* ---------------------------------------------------------------- the board */

#ifdef RPI4

/*
 * The firmware's GPIO expander: its lines are numbered from 128 in the
 * property interface. A call's first word answers 0 when it was done.
 */
#define W4_TAG_GET_CLOCK_STATE	0x00030001U
#define W4_TAG_SET_CLOCK_STATE	0x00038001U
#define W4_TAG_SET_GPIO_STATE	0x00038041U
#define W4_TAG_GET_GPIO_CONFIG	0x00030043U
#define W4_TAG_SET_GPIO_CONFIG	0x00038043U
#define W4_EXP_GPIO_BASE	128
#define W4_GPIO_OUT		1

/* The device tree's numbers of a pin's pull (brcm,pull) */
#define W4_DT_PULL_DOWN		1
#define W4_DT_PULL_UP		2

/* The time the chip is given to start: the same chip's regulator delay on the Pi 5 */
#define W4_POWER_OFF_MS		20
#define W4_POWER_ON_MS		150

IMPORT void	knl_gpio_func( UINT pin, UW fsel );	/* gpio_bcm2711.c */

LOCAL UW	w4_mb[16] __attribute__((aligned(64)));

/* One tag of 'n' words, sent and answered in 'v' */
LOCAL ER w4_fw( UW tag, UW *v, INT n )
{
	INT	i;
	ER	er;

	knl_memset(w4_mb, 0, sizeof(w4_mb));
	w4_mb[0] = (UW)( 6 + n ) * 4;
	w4_mb[2] = tag;
	w4_mb[3] = (UW)n * 4;
	w4_mb[4] = (UW)n * 4;
	for ( i = 0; i < n; i++ ) w4_mb[5 + i] = v[i];
	er = knl_mbox_property(w4_mb);
	if ( er < E_OK ) return er;
	if ( ( w4_mb[4] & 0x80000000U ) == 0 ) return E_IO;
	for ( i = 0; i < n; i++ ) v[i] = w4_mb[5 + i];
	return E_OK;
}

/* A line of the expander: its state set, made an output first if it is not one */
LOCAL ER w4_line( UW line, UW state )
{
	UW	v[6];
	ER	er;

	v[0] = W4_EXP_GPIO_BASE + line;
	v[1] = v[2] = v[3] = v[4] = 0;
	er = w4_fw(W4_TAG_GET_GPIO_CONFIG, v, 5);
	if ( er < E_OK || v[0] != 0 ) {
		return ( er < E_OK ) ? er : E_IO;
	}
	if ( v[1] != W4_GPIO_OUT ) {
		/* an output of the polarity it has; no termination on an output */
		v[0] = W4_EXP_GPIO_BASE + line;
		v[1] = W4_GPIO_OUT;
		v[3] = v[4] = 0;
		v[5] = state;
		er = w4_fw(W4_TAG_SET_GPIO_CONFIG, v, 6);
	} else {
		v[0] = W4_EXP_GPIO_BASE + line;
		v[1] = state;
		er = w4_fw(W4_TAG_SET_GPIO_STATE, v, 2);
	}
	if ( er >= E_OK && v[0] != 0 ) er = E_IO;
	return er;
}

/* The number of a named line of the expander, -1 when it has no such line */
LOCAL INT w4_line_of( CONST char *name )
{
	CONST UB	*v;
	INT		len, i, k, line = 0;

	v = knl_dtb_prop(W4_FWGPIO_PATH, "gpio-line-names", &len);
	for ( i = 0; v != NULL && i < len; i += k + 1, line++ ) {
		for ( k = 0; i + k < len && v[i + k] != '\0'; k++ ) ;
		if ( knl_strcmp((CONST char *)&v[i], name) == 0 ) return line;
	}
	return -1;
}

/*
 * The lines the device tree holds at a level for the board (the Compute
 * Module 4's choice of antenna): a board without those nodes has none,
 * and its lines of the same numbers mean something else.
 */
LOCAL void w4_hog( CONST char *path )
{
	CONST UB	*v;
	INT		len, hlen, high;
	ER		er;

	v = knl_dtb_prop(path, "gpios", &len);
	if ( v == NULL || len < 8 ) return;
	high = ( knl_dtb_prop(path, "output-high", &hlen) != NULL ) ? 1 : 0;
	er = w4_line(w5_be32(v), (UW)high);
	tm_printf((UB *)"wifi: %s: line %d of the firmware's GPIO set to %d (%d)\n",
		  path, (INT)w5_be32(v), high, (INT)er);
}

/* The host's pins, as the node the host's pinctrl-0 names */
LOCAL BOOL w5_pins( void )
{
	CONST UB	*pins, *fn, *pull;
	INT		np, nf, nu, i;
	UW		pin, f, u;
	INT		was;

	if ( knl_dtb_u32(W5_HOST_PATH, "pinctrl-0", 0) != knl_dtb_u32(W4_PINS_PATH, "phandle", 1) ) {
		tm_printf((UB *)"wifi: the host's pins are not %s\n", W4_PINS_PATH);
		return FALSE;
	}
	pins = knl_dtb_prop(W4_PINS_PATH, "brcm,pins", &np);
	fn = knl_dtb_prop(W4_PINS_PATH, "brcm,function", &nf);
	pull = knl_dtb_prop(W4_PINS_PATH, "brcm,pull", &nu);
	if ( pins == NULL || fn == NULL || nf < 4 || pull == NULL || nu != np ) {
		tm_printf((UB *)"wifi: %s is not as this knows it\n", W4_PINS_PATH);
		return FALSE;
	}
	f = w5_be32(fn);
	for ( i = 0; i + 4 <= np; i += 4 ) {
		pin = w5_be32(pins + i);
		u = w5_be32(pull + i);
		was = gpio_get_dir(0, pin);
		knl_gpio_func(pin, f);
		(void)gpio_set_pull(0, pin, ( u == W4_DT_PULL_UP ) ? GPIO_PULL_UP
				    : ( u == W4_DT_PULL_DOWN ) ? GPIO_PULL_DOWN : GPIO_PULL_NONE);
		tm_printf((UB *)"wifi: pin %d: was %d, now %d, pull %d\n",
			  (INT)pin, was, gpio_get_dir(0, pin), gpio_get_pull(0, pin));
	}
	return TRUE;
}

/*
 * The chip's power: off a while, then on. Should the firmware not do it,
 * the start goes on: the firmware leaves the chip powered, and the rest
 * shows whether it answers.
 */
LOCAL BOOL w5_power( void )
{
	INT	line;
	ER	er;

	w4_hog(W4_FWGPIO_PATH "/ant1");
	w4_hog(W4_FWGPIO_PATH "/ant2");
	line = w4_line_of("WL_ON");
	if ( line < 0 ) {
		tm_printf((UB *)"wifi: %s names no line WL_ON: the chip's power left as it is\n", W4_FWGPIO_PATH);
		return TRUE;
	}
	er = w4_line((UW)line, 0);
	tm_printf((UB *)"wifi: the chip's power (line %d of the firmware's GPIO) off: %d\n", line, (INT)er);
	tk_dly_tsk(W4_POWER_OFF_MS);
	er = w4_line((UW)line, 1);
	tk_dly_tsk(W4_POWER_ON_MS);
	tm_printf((UB *)"wifi: the chip's power on: %d, %d ms waited\n", (INT)er, W4_POWER_ON_MS);
	return TRUE;
}

/*
 * Whether the node is there and the host free: the boot card's driver
 * takes this same host when asked to (tessronos.sd=emmc, as under QEMU)
 */
LOCAL BOOL w5_host_node( void )
{
	UB	v[8];

	if ( !knl_dtb_okay(W5_HOST_PATH) || !w5_has(W5_HOST_PATH, "compatible", W5_HOST_COMPAT) ) {
		return FALSE;
	}
	return !( knl_bootarg(knl_bootargs, "tessronos.sd", v, sizeof(v))
		  && knl_strcmp((CONST char *)v, "emmc") == 0 );
}

/* The host as the device tree says it, and the board made ready for it */
LOCAL BOOL w5_probe( void )
{
	CONST UB	*v;
	INT		len;
	UW		hz = 0, st[2];
	ER		er;

	if ( !w5_host_node() ) {
		tm_printf((UB *)"wifi: no Wi-Fi host (%s) in the device tree, or the boot card has it\n", W5_HOST_PATH);
		return FALSE;
	}
	w5.host_pa = w5_soc_pa(W5_HOST_PATH);
	v = knl_dtb_prop(W5_HOST_PATH, "reg", &len);
	if ( w5.host_pa == 0 || v == NULL || len < 8 ) {
		tm_printf((UB *)"wifi: %s has no registers\n", W5_HOST_PATH);
		return FALSE;
	}
	w5.host_size = w5_be32(v + 4);
	/*
	 * interrupts: <0 (SPI) n 4 (level, high)>. This line is also the card
	 * slot's (EMMC2), whose driver polls with its signals off, so that
	 * only this host raises it.
	 */
	v = knl_dtb_prop(W5_HOST_PATH, "interrupts", &len);
	if ( v == NULL || len < 12 || w5_be32(v) != 0 ) {
		tm_printf((UB *)"wifi: %s has no shared peripheral interrupt\n", W5_HOST_PATH);
		return FALSE;
	}
	w5.intno = 32 + w5_be32(v + 4);
	/* the base clock: the firmware keeps it; on, and its rate */
	st[0] = FW_CLOCK_EMMC;
	st[1] = 0;
	er = w4_fw(W4_TAG_GET_CLOCK_STATE, st, 2);
	tm_printf((UB *)"wifi: the host's clock was %s (%d)\n", ( st[1] & 1 ) ? "on" : "off", (INT)er);
	if ( er >= E_OK && ( st[1] & 1 ) == 0 ) {
		st[0] = FW_CLOCK_EMMC;
		st[1] = 1;
		er = w4_fw(W4_TAG_SET_CLOCK_STATE, st, 2);
		tm_printf((UB *)"wifi: the host's clock turned on: state %x (%d)\n", st[1], (INT)er);
	}
	if ( knl_fw_value(TAG_GET_CLOCK_RATE, FW_CLOCK_EMMC, &hz) < E_OK || hz < 1000000 ) {
		tm_printf((UB *)"wifi: the firmware does not tell the host's clock (%d Hz)\n", (INT)hz);
		return FALSE;
	}
	w5.clk_khz = hz / 1000;
	tm_printf((UB *)"wifi: SDIO host at %08x+%x, interrupt %d, clock %d kHz\n",
		  (UW)w5.host_pa, (UW)w5.host_size, (INT)w5.intno, (INT)w5.clk_khz);
	return w5_pins() && w5_power();
}

#else /* RPI4 */

LOCAL BOOL w5_host_node( void )
{
	return knl_dtb_okay(W5_HOST_PATH) && w5_has(W5_HOST_PATH, "compatible", W5_HOST_COMPAT);
}

/* The functions and pulls of the host's pins and of the chip's power */
LOCAL BOOL w5_pins( void )
{
	CONST W5_PIN	*t;
	CONST char	*step;
	UD		pa;
	UW		v, w;
	INT		i;

	if ( w5_has(W5_PINCTRL_PATH, "compatible", "brcm,bcm2712c0-pinctrl") ) {
		t = w5_pins_c0;
		step = "c0";
	} else if ( w5_has(W5_PINCTRL_PATH, "compatible", "brcm,bcm2712d0-pinctrl") ) {
		t = w5_pins_d0;
		step = "d0";
	} else {
		tm_printf((UB *)"wifi: the pin control %s is not one of the BCM2712's this knows\n", W5_PINCTRL_PATH);
		return FALSE;
	}
	pa = w5_soc_pa(W5_PINCTRL_PATH);
	if ( pa == 0 ) {
		tm_printf((UB *)"wifi: no address for the pin control\n");
		return FALSE;
	}
	for ( i = 0; i < (INT)W5_NPINS; i++ ) {
		UBINT	m = DEV_BASE(pa) + t[i].mux_w * 4;
		UBINT	p = DEV_BASE(pa) + t[i].pad_w * 4;

		v = in_w(m);
		w = ( v & ~( (UW)0xf << ( t[i].mux_b * 4 ) ) ) | ( (UW)t[i].fsel << ( t[i].mux_b * 4 ) );
		out_w(m, w);
		v = in_w(p);
		w = ( v & ~( (UW)0x3 << ( t[i].pad_b * 2 ) ) ) | ( (UW)t[i].pull << ( t[i].pad_b * 2 ) );
		out_w(p, w);
	}
	tm_printf((UB *)"wifi: pins 28 and 30-35 set (pin control %s at %08x%08x): functions %08x %08x, pulls %08x %08x\n",
		  step, (UW)( pa >> 32 ), (UW)pa,
		  in_w(DEV_BASE(pa) + t[1].mux_w * 4), in_w(DEV_BASE(pa) + t[3].mux_w * 4),
		  in_w(DEV_BASE(pa) + t[1].pad_w * 4), in_w(DEV_BASE(pa) + t[4].pad_w * 4));
	return TRUE;
}

/* The chip's power: off a while, then on with the delay its regulator asks for */
LOCAL BOOL w5_power( void )
{
	CONST UB	*v;
	INT		len;
	UW		pin, ms;
	UD		pa;
	UBINT		g;

	v = knl_dtb_prop(W5_WLON_PATH, "gpio", &len);
	if ( v == NULL || len < 12 || knl_dtb_prop(W5_WLON_PATH, "enable-active-high", &len) == NULL ) {
		tm_printf((UB *)"wifi: %s is not as this knows it\n", W5_WLON_PATH);
		return FALSE;
	}
	pin = w5_be32(v + 4);
	if ( w5_be32(v) != knl_dtb_u32(W5_GPIO_PATH, "phandle", 0) || pin >= 32 ) {
		tm_printf((UB *)"wifi: the chip's power is not a pin of the first bank of %s\n", W5_GPIO_PATH);
		return FALSE;
	}
	pa = w5_soc_pa(W5_GPIO_PATH);
	if ( pa == 0 ) {
		tm_printf((UB *)"wifi: no address for %s\n", W5_GPIO_PATH);
		return FALSE;
	}
	g = DEV_BASE(pa);
	ms = ( knl_dtb_u32(W5_WLON_PATH, "startup-delay-us", 150000) + 999 ) / 1000;
	tm_printf((UB *)"wifi: the chip's power (GPIO %d) was %s, as %s\n", (INT)pin,
		  ( in_w(g + GIO_DATA(0)) & ( 1U << pin ) ) ? "on" : "off",
		  ( in_w(g + GIO_IODIR(0)) & ( 1U << pin ) ) ? "an input" : "an output");
	out_w(g + GIO_DATA(0), in_w(g + GIO_DATA(0)) & ~( 1U << pin ));
	out_w(g + GIO_IODIR(0), in_w(g + GIO_IODIR(0)) & ~( 1U << pin ));
	tk_dly_tsk(20);
	out_w(g + GIO_DATA(0), in_w(g + GIO_DATA(0)) | ( 1U << pin ));
	tk_dly_tsk((TMO)ms);
	tm_printf((UB *)"wifi: the chip's power on, %d ms waited\n", (INT)ms);
	return TRUE;
}

/* The host as the device tree says it, and the board made ready for it */
LOCAL BOOL w5_probe( void )
{
	CONST UB	*v;
	INT		len;
	UW		ph, hz, mhz;
	UBINT		cfg;

	if ( !w5_host_node() ) {
		tm_printf((UB *)"wifi: no Wi-Fi host (%s) in the device tree\n", W5_HOST_PATH);
		return FALSE;
	}
	/* reg: the host's and the cfg's, two cells of address and two of size each */
	v = knl_dtb_prop(W5_HOST_PATH, "reg", &len);
	if ( v == NULL || len < 32 ) {
		tm_printf((UB *)"wifi: %s has no registers\n", W5_HOST_PATH);
		return FALSE;
	}
	w5.host_pa = ( (UD)w5_be32(v) << 32 ) | w5_be32(v + 4);
	w5.host_size = ( (UD)w5_be32(v + 8) << 32 ) | w5_be32(v + 12);
	w5.cfg_pa = ( (UD)w5_be32(v + 16) << 32 ) | w5_be32(v + 20);
	/* interrupts: <0 (SPI) n 4 (level, high)> */
	v = knl_dtb_prop(W5_HOST_PATH, "interrupts", &len);
	if ( v == NULL || len < 12 || w5_be32(v) != 0 ) {
		tm_printf((UB *)"wifi: %s has no shared peripheral interrupt\n", W5_HOST_PATH);
		return FALSE;
	}
	w5.intno = 32 + w5_be32(v + 4);
	/* the base clock: the fixed clock the node names */
	ph = knl_dtb_u32(W5_HOST_PATH, "clocks", 0);
	hz = knl_dtb_u32(W5_CLOCK_PATH, "clock-frequency", 0);
	if ( ph == 0 || ph != knl_dtb_u32(W5_CLOCK_PATH, "phandle", 0) || hz < 1000000 ) {
		tm_printf((UB *)"wifi: the host's clock is not %s\n", W5_CLOCK_PATH);
		return FALSE;
	}
	w5.clk_khz = hz / 1000;
	tm_printf((UB *)"wifi: SDIO host at %08x%08x+%x, cfg at %08x%08x, interrupt %d, clock %d kHz\n",
		  (UW)( w5.host_pa >> 32 ), (UW)w5.host_pa, (UW)w5.host_size,
		  (UW)( w5.cfg_pa >> 32 ), (UW)w5.cfg_pa, (INT)w5.intno, (INT)w5.clk_khz);

	if ( !w5_pins() || !w5_power() ) {
		return FALSE;
	}

	/* the card is always there; the SD timing; the timer's clock */
	cfg = DEV_BASE(w5.cfg_pa);
	mhz = hz / 1000000;
	tm_printf((UB *)"wifi: cfg was ctrl %08x, pin select %08x, CQ capability %08x\n",
		  in_w(cfg + CFG_CTRL), in_w(cfg + CFG_SD_PIN_SEL), in_w(cfg + CFG_CQ_CAPABILITY));
	out_w(cfg + CFG_CTRL, ( in_w(cfg + CFG_CTRL) & ~(UW)CFG_CTRL_SDCD_TEST_LEV ) | CFG_CTRL_SDCD_TEST_EN);
	out_w(cfg + CFG_SD_PIN_SEL, ( in_w(cfg + CFG_SD_PIN_SEL) & ~(UW)CFG_SD_PIN_SEL_MASK ) | CFG_SD_PIN_SEL_SD);
	out_w(cfg + CFG_CQ_CAPABILITY, ( 3U << CFG_CQ_FMUL_SHIFT ) | mhz);
	tm_printf((UB *)"wifi: cfg now ctrl %08x, pin select %08x, CQ capability %08x\n",
		  in_w(cfg + CFG_CTRL), in_w(cfg + CFG_SD_PIN_SEL), in_w(cfg + CFG_CQ_CAPABILITY));
	return TRUE;
}

#endif /* RPI4 */

/* ---------------------------------------------------------------- the interrupt */

LOCAL void w5_inthdr( UINT intno )
{
	DisableInt(intno);		/* until sdhc's handler has run */
	w5.ints++;
	EndOfInt(intno);
	tk_sig_sem(w5.semid, 1);
}

LOCAL BOOL w5_intr( void )
{
	T_CSEM	csem;

	csem.exinf   = NULL;
	csem.sematr  = TA_TFIFO | TA_FIRST;
	csem.isemcnt = 0;
	csem.maxsem  = 1;
	w5.semid = tk_cre_sem(&csem);
	if ( w5.semid <= 0 ) {
		tm_printf((UB *)"wifi: no semaphore for the host's interrupt (%d)\n", (INT)w5.semid);
		return FALSE;
	}
	knl_define_inthdr(w5.intno, TA_HLNG, (FP)w5_inthdr);
	SetIntMode(w5.intno, IM_LEVEL);
	EnableInt(w5.intno, INTPRI_DEVICE);
	return TRUE;
}

/* ---------------------------------------------------------------- the host */

/*
 * Whether there is a host: its registers, its base clock and the
 * cookie of its interrupt; the board made ready for it. 0, or ENXIO
 * when the machine has none (and nothing was touched).
 */
EXPORT int rumpcomp_tswifi_host( UD *p_pa, UD *p_size, UINT *p_clk_khz, UINT *p_intr )
{
	INT	n;
	BOOL	ok;

	W_UNSCHED(n);
	ok = w5_probe() && w5_intr();
	W_SCHED(n);
	w5.probed = TRUE;
	if ( !ok ) {
		return NB_ENXIO;
	}
	w5.found = TRUE;
	*p_pa = w5.host_pa;
	*p_size = w5.host_size;
	*p_clk_khz = w5.clk_khz;
	*p_intr = w5.intno;
	return 0;
}

/*
 * Whether the machine has the Wi-Fi (netsel.c, wifi.c): the host the
 * device tree describes, until the rump kernel has tried it, then
 * whether that went well
 */
EXPORT BOOL knl_wifi5_present( void )
{
	if ( w5.no_fw ) {
		return FALSE;
	}
	if ( w5.probed ) {
		return w5.found;
	}
	return w5_host_node();
}

/* Whether the chip turned out to be one whose firmware the system does not include */
EXPORT BOOL knl_wifi5_no_firmware( void )
{
	return w5.no_fw;
}

/*
 * Whether the host is the BCM2711's older block, which sdhc is to run
 * with the settings of that block (tswifi_sdio.c)
 */
EXPORT int rumpcomp_tswifi_host_arasan( void )
{
#ifdef RPI4
	return 1;
#else
	return 0;
#endif
}

/* The registers of the host: the device window of the kernel */
EXPORT void *rumpcomp_tswifi_map( UD pa, UD size )
{
	if ( !w5.found || pa != w5.host_pa || size > w5.host_size ) {
		return NULL;
	}
	return (void *)DEV_BASE(pa);
}

EXPORT int rumpcomp_tswifi_intr_wait( UINT cookie )
{
	INT	n;
	ER	er;

	(void)cookie;
	W_UNSCHED(n);
	er = tk_wai_sem(w5.semid, 1, TMO_FEVR);
	W_SCHED(n);
	return ( er >= E_OK ) ? 0 : NB_EINVAL;
}

EXPORT void rumpcomp_tswifi_intr_done( UINT cookie )
{
	(void)cookie;
	EnableInt(w5.intno, INTPRI_DEVICE);
}

/* ---------------------------------------------------------------- memory */

LOCAL INT w5_order( UD size )
{
	INT	order = 0;

	while ( ( (UD)PAGE_SIZE << order ) < size ) order++;
	return order;
}

/* Whole pages below 4 GB, zeroed, at their place in the linear map */
EXPORT void *rumpcomp_tswifi_dmalloc( UD size, UD align )
{
	PFRAME	*pf;

	(void)align;		/* the pages of an order are aligned to their size */
	pf = knl_alloc_pages((UINT)w5_order(size), ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	return ( pf != NULL ) ? knl_pf_to_va(pf) : NULL;
}

EXPORT void rumpcomp_tswifi_dmafree( void *va, UD size )
{
	if ( va != NULL ) {
		knl_free_pages(knl_pa_to_pf(VA2PA(va)), (UINT)w5_order(size));
	}
}

EXPORT UD rumpcomp_tswifi_va2pa( void *va )
{
	return (UD)VA2PA(va);
}

/* The cache lines of [va, va + len): 1 written out, 2 dropped, 3 both */
EXPORT void rumpcomp_tswifi_dmasync( void *va, UD len, INT how )
{
	UD	ctr, line;
	UBINT	a, e;

	Asm("mrs %0, ctr_el0" : "=r"(ctr));
	line = (UD)4 << ( ( ctr >> 16 ) & 0xf );	/* DminLine */
	a = (UBINT)va & ~( line - 1 );
	e = (UBINT)va + len;
	for ( ; a < e; a += line ) {
		if ( how == 1 ) {
			Asm("dc cvac, %0" :: "r"(a) : "memory");
		} else {
			Asm("dc civac, %0" :: "r"(a) : "memory");
		}
	}
	Asm("dsb sy" ::: "memory");
}

#ifdef WIFI_OBSD
/*
 * The layer's pools of large items (mbuf clusters, obsd_kern.c's
 * pool_get) and bus_dma's memory, under the names the PCI cards' have
 */
EXPORT int rumpcomp_pci_dmalloc( unsigned long size, unsigned long align, unsigned long *pap,
				 unsigned long *vap )
{
	void	*va = rumpcomp_tswifi_dmalloc((UD)size, (UD)align);

	if ( va == NULL ) return NB_ENOMEM;
	*vap = (unsigned long)va;
	*pap = (unsigned long)VA2PA(va);
	return 0;
}

EXPORT void rumpcomp_pci_dmafree( unsigned long va, unsigned long size )
{
	rumpcomp_tswifi_dmafree((void *)va, (UD)size);
}

EXPORT unsigned long rumpcomp_pci_virt_to_mach( void *va )
{
	return (unsigned long)VA2PA(va);
}
#endif /* WIFI_OBSD */

/* ---------------------------------------------------------------- the files */

/* Record 'rec' of the object 'uuid' read whole into a new buffer */
LOCAL ER w5_read( CONST char *uuid, INT rec, UB **p_buf, UD *p_size )
{
	TS_UUID		u;
	T_OBREC		r[4];
	INT		i, cnt = 0;
	ID		key;
	SZ		asz = 0;
	UD		sz = 0;
	UB		*buf = NULL;
	ER		er;

	if ( ts_str_to_uuid(uuid, &u) < E_OK ) return E_PAR;
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) return (ER)key;
	er = ob_lst_rec(key, r, 4, &cnt);
	for ( i = 0; er >= E_OK && i < cnt; i++ ) {
		if ( r[i].recno == rec ) sz = r[i].size;
	}
	if ( er >= E_OK ) {
		if ( sz == 0 || sz > 8 * 1024 * 1024 ) {
			er = E_NOEXS;
		} else if ( ( buf = (UB *)Kmalloc((size_t)sz + 1) ) == NULL ) {
			er = E_NOMEM;
		} else {
			er = ob_rea_rec(key, rec, 0, buf, (SZ)sz, &asz);
			if ( er >= E_OK && (UD)asz != sz ) er = E_IO;
		}
	}
	ob_cls_obj(key);
	if ( er < E_OK ) {
		if ( buf != NULL ) Kfree(buf);
		return er;
	}
	buf[sz] = '\0';
	*p_buf = buf;
	*p_size = sz;
	return E_OK;
}

/*
 * bwfm_nvram_convert works in place and needs room in the buffer for the
 * padding and the length token after the settings. The settings read out
 * of the document have no comment lines left to give it that room, so
 * empty lines, which the conversion drops, are added after them.
 */
#define W5_NV_ROOM	12

/*
 * The settings' macaddr= is the same for every board. The firmware of the
 * Raspberry Pi writes the board's own address in the device tree, so it
 * takes that line's place when it is there; all zero means it was not written.
 */
LOCAL void w5_nv_mac( UB *nv, INT len )
{
	static CONST char	key[] = "macaddr=", hex[] = "0123456789abcdef";
	CONST UB	*m;
	INT		n = 0, i, k;

	m = (CONST UB *)knl_dtb_prop(W5_HOST_PATH "/wifi@1", "local-mac-address", &n);
	if ( m == NULL || n < 6 || ( m[0] | m[1] | m[2] | m[3] | m[4] | m[5] ) == 0 ) {
		tm_printf((UB *)"wifi: no address of the board's own in the device tree: the settings' is used\n");
		return;
	}
	for ( i = 0; i + 25 <= len; i++ ) {
		if ( i != 0 && nv[i - 1] != '\n' ) continue;
		for ( k = 0; k < 8 && nv[i + k] == (UB)key[k]; k++ ) ;
		if ( k < 8 ) continue;
		for ( k = 0; k < 6; k++ ) {
			nv[i + 8 + k * 3] = (UB)hex[m[k] >> 4];
			nv[i + 9 + k * 3] = (UB)hex[m[k] & 15];
		}
		tm_printf((UB *)"wifi: the board's own address from the device tree: %02x:%02x:%02x:%02x:%02x:%02x\n",
			  m[0], m[1], m[2], m[3], m[4], m[5]);
		return;
	}
}

EXPORT int rumpcomp_tswifi_fw_open( CONST char *name, void **data, UD *size )
{
	UB	*buf = NULL, *nv;
	UD	sz = 0;
	INT	k, n, drop = 0, len;
	ER	er;

	for ( k = 0; k < (INT)( sizeof(w5_fw) / sizeof(w5_fw[0]) ); k++ ) {
		if ( knl_strcmp(name, w5_fw[k].name) == 0 ) break;
	}
	if ( k == (INT)( sizeof(w5_fw) / sizeof(w5_fw[0]) ) ) {
#ifdef RPI4
		if ( !w5.no_fw ) {
			tm_printf((UB *)"wifi: the system does not include the firmware of this machine's chip (%s): no Wi-Fi on this machine\n", name);
		}
		w5.no_fw = TRUE;
#else
		tm_printf((UB *)"wifi: file %s: not one the system has\n", name);
#endif
		return NB_ENOENT;
	}
	W_UNSCHED(n);
	er = w5_read(w5_fw[k].uuid, ( w5_fw[k].kind == W5_FW_NVRAM ) ? 0 : 1, &buf, &sz);
	W_SCHED(n);
	if ( er < E_OK ) {
		tm_printf((UB *)"wifi: file %s: the object %s could not be read (%d)\n", name, w5_fw[k].uuid, (INT)er);
		return ( er == E_NOMEM ) ? NB_ENOMEM : NB_ENOENT;
	}
	if ( w5_fw[k].kind == W5_FW_NVRAM ) {
		nv = (UB *)Kmalloc((size_t)sz + 1 + W5_NV_ROOM);
		len = ( nv != NULL ) ? knl_wifi_nvram(buf, (SZ)sz, nv, (SZ)sz + 1, &drop) : E_NOMEM;
		Kfree(buf);
		if ( len < 0 ) {
			if ( nv != NULL ) Kfree(nv);
			tm_printf((UB *)"wifi: file %s: the settings could not be read out of the document %s (%d)\n",
				  name, w5_fw[k].uuid, len);
			return NB_EINVAL;
		}
		tm_printf((UB *)"wifi: file %s: %d bytes of settings out of the document %s (%d bytes), %d lines left out\n",
			  name, len, w5_fw[k].uuid, (INT)sz, drop);
		w5_nv_mac(nv, len);
		knl_memset(nv + len, '\n', W5_NV_ROOM);
		buf = nv;
		sz = (UD)len + W5_NV_ROOM;
	} else {
		tm_printf((UB *)"wifi: file %s: %d bytes from the object %s\n", name, (INT)sz, w5_fw[k].uuid);
	}
	*data = buf;
	*size = sz;
	return 0;
}

EXPORT void rumpcomp_tswifi_fw_close( void *data, UD size )
{
	(void)size;
	if ( data != NULL ) Kfree(data);
}

/* ---------------------------------------------------------------- frames and words */

/* A frame bwfm0 received: to the manager, which does not wait */
EXPORT void rumpcomp_tswifi_input( CONST void *frame, UD len )
{
	knl_wifi_rx((CONST UB *)frame, (INT)len);
}

EXPORT void rumpcomp_tswifi_say( CONST char *s )
{
	tm_putstring((CONST UB *)s);
}

#endif /* USE_WIFI && !X64 */
