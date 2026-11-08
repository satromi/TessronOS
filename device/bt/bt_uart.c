/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_uart.c (Raspberry Pi 5)
 *	The board's own Bluetooth (design 6.8): the Bluetooth half of the
 *	CYW43455, whose Wi-Fi half is on SDIO (wifi_user_rpi5.c), on the
 *	BCM2712's UART "uarta" in H4.
 *
 *	All of it comes from the device tree, and a machine whose tree does
 *	not have it as this knows it is left alone:
 *
 *	- the UART, /soc@107c000000/serial@7d50c000 (brcm,bcm7271-uart): a
 *	  16550 with a 32-byte FIFO and its registers 4 bytes apart, its
 *	  clock the tree's clock-frequency, its interrupt a shared peripheral
 *	  one. Its lines have RTS and CTS (uart-has-rtscts), run by the UART
 *	  itself (MCR's AFE), so that a byte is not lost while the handler
 *	  is late.
 *	- the controller, its child "bluetooth" (brcm,bcm43438-bt): the
 *	  speed to run at (max-speed), its power (shutdown-gpios, BT_ON, GPIO
 *	  29 of gpio@7d508500, high on) and the board's address
 *	  (local-bd-address, which the firmware fills in).
 *	- the pins (uarta_24_pins and bt_shutdown_pins): GPIO 24 to 27 to the
 *	  UART (RTS, CTS, TXD, RXD; CTS and RXD pulled up), 29 a GPIO. The
 *	  places of their fields and the function numbers are those of the
 *	  BCM2712's two steppings, told apart by the pin control's compatible.
 *
 *	The pins and the power are set at the start of the system, before the
 *	Wi-Fi starts: the Wi-Fi's power (GPIO 28) and its pins share these
 *	registers, written without a lock of theirs, and this way the two
 *	are never written at the same time. The power goes off, then on with
 *	RTS not asserted, so that the controller starts as it does from
 *	power on rather than looking for the speed.
 *
 *	Then the controller's task (bt.c) runs this file's prep: the UART at
 *	115200 baud with its flow control, the interrupt, and the controller's
 *	start with its patch (bt_bcm.c). The interrupt handler takes the bytes
 *	out of the FIFO and the packets out of the bytes (bt_h4.c) into the
 *	task's ring; the task sends, waiting on the FIFO.
 *
 *	Every step says a line that starts with "bt: uart:" or "bt: bcm:".
 */

#include <sys/machine.h>

#ifdef RPI5

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/bt.h>
#include "sysdepend.h"
#include "../usb/usbdev.h"
#include "btint.h"
#include "bt_bcm.h"

#define BU_SOC_PATH	"/soc@107c000000"
#define BU_UART_PATH	"/soc@107c000000/serial@7d50c000"
#define BU_UART_COMPAT	"brcm,bcm7271-uart"
#define BU_BT_PATH	"/soc@107c000000/serial@7d50c000/bluetooth"
#define BU_BT_COMPAT	"brcm,bcm43438-bt"
#define BU_PINCTRL_PATH	"/soc@107c000000/pinctrl@7d504100"
#define BU_GPIO_PATH	"/soc@107c000000/gpio@7d508500"

#define BU_INIT_BAUD	115200		/* the controller's after power on */
#define BU_POWER_MS	100		/* off, and after on: what the controller takes to come out of reset */

/* The 16550's registers, each a 32-bit word */
#define U_RBR		0		/* receive; THR to send, DLL with DLAB */
#define U_IER		1		/* DLM with DLAB */
#define U_IIR		2		/* FCR to write */
#define U_LCR		3
#define U_MCR		4
#define U_LSR		5
#define U_MSR		6
#define U_SHIFT		2		/* reg-shift */
#define U_WIDTH		4		/* reg-io-width */
#define U_FIFO		32

#define IER_RDI		0x01		/* data received, and the FIFO's timeout */
#define IER_RLSI	0x04		/* the line's status */
#define FCR_ENABLE	0x01
#define FCR_CLR_RX	0x02
#define FCR_CLR_TX	0x04
#define FCR_TRIG_8	0x40		/* the interrupt at 8 bytes received */
#define LCR_8N1		0x03
#define LCR_DLAB	0x80
#define MCR_RTS		0x02
#define MCR_OUT2	0x08
#define MCR_AFE		0x20		/* RTS and CTS run by the UART */
#define LSR_DR		0x01
#define LSR_OE		0x02
#define LSR_PE		0x04
#define LSR_FE		0x08
#define LSR_BI		0x10
#define LSR_THRE	0x20
#define LSR_TEMT	0x40
#define MSR_CTS		0x10

/* The pin control's fields of a pin, as wifi_user_rpi5.c has them for its own */
#define PULL_NONE	0
#define PULL_UP		2
#define PULL_LEAVE	0xff

typedef struct {
	UB	pin;
	UB	mux_w, mux_b;		/* the function: word mux_w, field mux_b (4 bits each) */
	UB	pad_w, pad_b;		/* the pull: word pad_w, field pad_b (2 bits each) */
	UB	fsel;			/* the function number: 0 is GPIO */
	UB	pull;
} BU_PIN;

LOCAL CONST BU_PIN	bu_pins_c0[] = {
	{ 24, 3, 0, 9, 1, 3, PULL_NONE },	/* RTS, uart0 */
	{ 25, 3, 1, 9, 2, 4, PULL_UP },		/* CTS */
	{ 26, 3, 2, 9, 3, 4, PULL_NONE },	/* TXD */
	{ 27, 3, 3, 9, 4, 4, PULL_UP },		/* RXD */
	{ 29, 3, 5, 9, 6, 0, PULL_LEAVE },	/* BT_ON, a GPIO */
};

LOCAL CONST BU_PIN	bu_pins_d0[] = {
	{ 24, 2, 0, 5, 6, 4, PULL_NONE },
	{ 25, 2, 1, 5, 7, 4, PULL_UP },
	{ 26, 2, 2, 5, 8, 4, PULL_NONE },
	{ 27, 2, 3, 5, 9, 4, PULL_UP },
	{ 29, 2, 5, 5, 11, 0, PULL_LEAVE },
};

#define BU_NPINS	( sizeof(bu_pins_c0) / sizeof(bu_pins_c0[0]) )

/* A bank of the brcmstb GPIO block: the data at 4, the direction at 8 (1: input) */
#define GIO_DATA(b)	( (b) * 0x20 + 0x04 )
#define GIO_IODIR(b)	( (b) * 0x20 + 0x08 )

LOCAL struct {
	UD		pa;
	UBINT		base;
	UINT		intno;
	UW		clk_hz;
	UW		max_baud;
	UW		baud;
	UB		bdaddr[6];
	UW		on_ms;		/* when the power went on */
	BOOL		irq_on;
	BTC		*c;
	BTH4		h4;
	volatile UW	bytes, overrun, line_err;
} bu;

LOCAL UW bu_rd( INT r )
{
	return in_w(bu.base + ( (UBINT)r << U_SHIFT ));
}

LOCAL void bu_wr( INT r, UW v )
{
	out_w(bu.base + ( (UBINT)r << U_SHIFT ), v);
}

/* ---------------------------------------------------------------- the device tree */

LOCAL UW bu_be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

LOCAL BOOL bu_has( CONST char *path, CONST char *prop, CONST char *s )
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
LOCAL UD bu_soc_pa( CONST char *path )
{
	CONST UB	*v;
	INT		len;
	UD		child, parent, size;

	v = knl_dtb_prop(path, "reg", &len);
	if ( v == NULL || len < 8 ) return 0;
	if ( knl_dtb_ranges(BU_SOC_PATH, "ranges", 0, &child, &parent, &size) < E_OK ) return 0;
	if ( bu_be32(v) < child || bu_be32(v) - child >= size ) return 0;
	return parent + ( bu_be32(v) - child );
}

LOCAL BOOL bu_probe( void )
{
	CONST UB	*v;
	INT		len;

	if ( !knl_dtb_okay(BU_UART_PATH) || !bu_has(BU_UART_PATH, "compatible", BU_UART_COMPAT)
	  || !knl_dtb_okay(BU_BT_PATH) || !bu_has(BU_BT_PATH, "compatible", BU_BT_COMPAT) ) {
		tm_printf((UB *)"bt: uart: no built-in Bluetooth in the device tree (%s, %s)\n", BU_UART_PATH, BU_BT_PATH);
		return FALSE;
	}
	if ( knl_dtb_u32(BU_UART_PATH, "reg-shift", 0) != U_SHIFT || knl_dtb_u32(BU_UART_PATH, "reg-io-width", 0) != U_WIDTH ) {
		tm_printf((UB *)"bt: uart: %s: registers not 4 bytes apart in words: not one this knows\n", BU_UART_PATH);
		return FALSE;
	}
	bu.pa = bu_soc_pa(BU_UART_PATH);
	bu.clk_hz = knl_dtb_u32(BU_UART_PATH, "clock-frequency", 0);
	v = knl_dtb_prop(BU_UART_PATH, "interrupts", &len);
	if ( bu.pa == 0 || bu.clk_hz < 16 * BU_INIT_BAUD || v == NULL || len < 12 || bu_be32(v) != 0 ) {
		tm_printf((UB *)"bt: uart: %s: no address, clock or shared peripheral interrupt\n", BU_UART_PATH);
		return FALSE;
	}
	bu.intno = 32 + bu_be32(v + 4);
	bu.max_baud = knl_dtb_u32(BU_BT_PATH, "max-speed", BU_INIT_BAUD);
	if ( bu.max_baud > bu.clk_hz / 16 ) bu.max_baud = bu.clk_hz / 16;
	v = knl_dtb_prop(BU_BT_PATH, "local-bd-address", &len);
	if ( v != NULL && len == 6 ) knl_memcpy(bu.bdaddr, v, 6);
	bu.base = DEV_BASE(bu.pa);
	tm_printf((UB *)"bt: uart: %s at %08x%08x, interrupt %d, clock %d Hz, the controller up to %d baud, flow control %s, address %02x:%02x:%02x:%02x:%02x:%02x\n",
		  BU_UART_PATH, (UW)( bu.pa >> 32 ), (UW)bu.pa, (INT)bu.intno, (INT)bu.clk_hz, (INT)bu.max_baud,
		  ( knl_dtb_prop(BU_UART_PATH, "uart-has-rtscts", &len) != NULL ) ? "RTS/CTS" : "not said",
		  bu.bdaddr[5], bu.bdaddr[4], bu.bdaddr[3], bu.bdaddr[2], bu.bdaddr[1], bu.bdaddr[0]);
	return TRUE;
}

/* ---------------------------------------------------------------- the board */

LOCAL BOOL bu_pins( void )
{
	CONST BU_PIN	*t;
	CONST char	*step;
	UD		pa;
	UW		v;
	INT		i;

	if ( bu_has(BU_PINCTRL_PATH, "compatible", "brcm,bcm2712c0-pinctrl") ) {
		t = bu_pins_c0;
		step = "c0";
	} else if ( bu_has(BU_PINCTRL_PATH, "compatible", "brcm,bcm2712d0-pinctrl") ) {
		t = bu_pins_d0;
		step = "d0";
	} else {
		tm_printf((UB *)"bt: uart: the pin control %s is not one of the BCM2712's this knows\n", BU_PINCTRL_PATH);
		return FALSE;
	}
	pa = bu_soc_pa(BU_PINCTRL_PATH);
	if ( pa == 0 ) {
		tm_printf((UB *)"bt: uart: no address for the pin control\n");
		return FALSE;
	}
	for ( i = 0; i < (INT)BU_NPINS; i++ ) {
		UBINT	m = DEV_BASE(pa) + t[i].mux_w * 4;
		UBINT	p = DEV_BASE(pa) + t[i].pad_w * 4;

		v = in_w(m);
		out_w(m, ( v & ~( (UW)0xf << ( t[i].mux_b * 4 ) ) ) | ( (UW)t[i].fsel << ( t[i].mux_b * 4 ) ));
		if ( t[i].pull != PULL_LEAVE ) {
			v = in_w(p);
			out_w(p, ( v & ~( (UW)0x3 << ( t[i].pad_b * 2 ) ) ) | ( (UW)t[i].pull << ( t[i].pad_b * 2 ) ));
		}
	}
	tm_printf((UB *)"bt: uart: pins 24-27 to the UART, 29 a GPIO (pin control %s at %08x%08x): functions %08x, pulls %08x\n",
		  step, (UW)( pa >> 32 ), (UW)pa, in_w(DEV_BASE(pa) + t[0].mux_w * 4), in_w(DEV_BASE(pa) + t[0].pad_w * 4));
	return TRUE;
}

/* BT_ON as shutdown-gpios says it: a pin of the first bank, high on */
LOCAL BOOL bu_power( void )
{
	CONST UB	*v;
	INT		len;
	UW		pin;
	UD		pa;
	UBINT		g;

	v = knl_dtb_prop(BU_BT_PATH, "shutdown-gpios", &len);
	if ( v == NULL || len < 12 ) {
		tm_printf((UB *)"bt: uart: %s has no shutdown-gpios\n", BU_BT_PATH);
		return FALSE;
	}
	pin = bu_be32(v + 4);
	if ( bu_be32(v) != knl_dtb_u32(BU_GPIO_PATH, "phandle", 0) || pin >= 32 || ( bu_be32(v + 8) & 1 ) != 0 ) {
		tm_printf((UB *)"bt: uart: the controller's power is not a pin of the first bank of %s, high on\n", BU_GPIO_PATH);
		return FALSE;
	}
	pa = bu_soc_pa(BU_GPIO_PATH);
	if ( pa == 0 ) {
		tm_printf((UB *)"bt: uart: no address for %s\n", BU_GPIO_PATH);
		return FALSE;
	}
	g = DEV_BASE(pa);
	tm_printf((UB *)"bt: uart: BT_ON (GPIO %d) was %s, as %s; off %d ms, then on\n", (INT)pin,
		  ( in_w(g + GIO_DATA(0)) & ( 1U << pin ) ) ? "on" : "off",
		  ( in_w(g + GIO_IODIR(0)) & ( 1U << pin ) ) ? "an input" : "an output", BU_POWER_MS);
	out_w(g + GIO_DATA(0), in_w(g + GIO_DATA(0)) & ~( 1U << pin ));
	out_w(g + GIO_IODIR(0), in_w(g + GIO_IODIR(0)) & ~( 1U << pin ));
	tk_dly_tsk(BU_POWER_MS);
	out_w(g + GIO_DATA(0), in_w(g + GIO_DATA(0)) | ( 1U << pin ));
	bu.on_ms = knl_btx_now();
	return TRUE;
}

/* ---------------------------------------------------------------- the UART */

/* The host's end at a speed: the divisor nearest the clock gives */
LOCAL ER bu_speed( BTC *c, UW baud )
{
	UW	div = ( bu.clk_hz + 8 * baud ) / ( 16 * baud );
	UW	due = knl_btx_now() + 100;
	UW	lcr;

	(void)c;
	if ( div == 0 || div > 0xffff ) return E_PAR;
	while ( ( bu_rd(U_LSR) & LSR_TEMT ) == 0 ) {
		if ( (W)( knl_btx_now() - due ) >= 0 ) break;
	}
	/* with DLAB the handler's registers are the divisor's: it is kept out meanwhile */
	if ( bu.irq_on ) DisableInt(bu.intno);
	lcr = bu_rd(U_LCR);
	bu_wr(U_LCR, lcr | LCR_DLAB);
	bu_wr(U_RBR, div & 0xff);
	bu_wr(U_IER, div >> 8);
	bu_wr(U_LCR, lcr & ~LCR_DLAB);
	if ( bu.irq_on ) EnableInt(bu.intno, INTPRI_DEVICE);
	bu.baud = baud;
	tm_printf((UB *)"bt: uart: the host's end at %d baud (divisor %d, %d baud off)\n",
		  (INT)baud, (INT)div, (INT)( bu.clk_hz / ( 16 * div ) ) - (INT)baud);
	return E_OK;
}

/* A whole packet out of the bytes: into the controller's ring */
LOCAL void bu_packet( void *ctx, UB type, CONST UB *p, INT len )
{
	(void)ctx;
	if ( type == H4_EVENT ) knl_btx_ring_in(bu.c, 0, p, len);
	else if ( type == H4_ACL ) knl_btx_ring_in(bu.c, 1, p, len);
}

LOCAL void bu_inthdr( UINT intno )
{
	UB	b[U_FIFO];
	INT	n = 0, guard;
	UW	lsr;

	for ( guard = 0; guard < 4 * U_FIFO; guard++ ) {
		lsr = bu_rd(U_LSR);
		if ( lsr & LSR_OE ) bu.overrun++;
		if ( lsr & ( LSR_PE | LSR_FE | LSR_BI ) ) bu.line_err++;
		if ( ( lsr & LSR_DR ) == 0 ) break;
		b[n++] = (UB)bu_rd(U_RBR);
		if ( n == U_FIFO ) {
			knl_bth4_feed(&bu.h4, b, n, bu_packet, NULL);
			bu.bytes += n;
			n = 0;
		}
	}
	if ( n > 0 ) {
		knl_bth4_feed(&bu.h4, b, n, bu_packet, NULL);
		bu.bytes += n;
	}
	(void)bu_rd(U_IIR);
	EndOfInt(intno);
}

/* A packet out, its type before it, as fast as the FIFO takes it; the controller's CTS holds it back */
LOCAL ER bu_send( UB type, CONST UB *b, INT n )
{
	UB	out[1 + BTH4_MAX];
	INT	len = knl_bth4_pack(type, b, n, out, sizeof(out)), i = 0, k;
	UW	due = knl_btx_now() + 200 + (UW)n * 10000 / bu.baud;	/* ten bits a byte, in ms, and room */

	if ( len < 0 ) return E_PAR;
	while ( i < len ) {
		if ( ( bu_rd(U_LSR) & LSR_THRE ) == 0 ) {
			if ( (W)( knl_btx_now() - due ) >= 0 ) {
				tm_printf((UB *)"bt: uart: the line took %d of %d bytes: CTS %s\n",
					  i, len, ( bu_rd(U_MSR) & MSR_CTS ) ? "asserted" : "not asserted");
				return E_TMOUT;
			}
			/* a FIFO's worth takes a few milliseconds at the slow speed, a tenth of one at the fast */
			if ( bu.baud < 1000000 ) tk_dly_tsk(1);
			continue;
		}
		for ( k = 0; k < U_FIFO && i < len; k++ ) bu_wr(U_RBR, out[i++]);
	}
	return E_OK;
}

LOCAL ER bu_cmd( BTC *c, UB *b, INT n )
{
	(void)c;
	return bu_send(H4_CMD, b, n);
}

LOCAL ER bu_acl( BTC *c, UB *b, INT n )
{
	(void)c;
	return bu_send(H4_ACL, b, n);
}

LOCAL void bu_count( CONST char *when )
{
	tm_printf((UB *)"bt: uart: %s: %d bytes in, %d packets, %d bytes not a packet's type, %d too long, %d overruns, %d line errors\n",
		  when, (INT)bu.bytes, (INT)bu.h4.frames, (INT)bu.h4.bad_type, (INT)bu.h4.too_long,
		  (INT)bu.overrun, (INT)bu.line_err);
}

/* In the controller's task: the line, the interrupt, then the controller's start */
LOCAL BOOL bu_prep( BTC *c )
{
	BTBCFG	cfg;
	W	wait = BU_POWER_MS - (W)( knl_btx_now() - bu.on_ms );
	BOOL	ok;
	INT	i;

	bu.c = c;			/* before the interrupt: its packets go to this task's ring */
	if ( wait > 0 ) tk_dly_tsk((TMO)wait);
	bu_wr(U_IER, 0);
	bu_wr(U_LCR, LCR_8N1);
	(void)bu_speed(c, BU_INIT_BAUD);
	bu_wr(U_IIR, FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX | FCR_TRIG_8);
	for ( i = 0; i < 2 * U_FIFO && ( bu_rd(U_LSR) & LSR_DR ) != 0; i++ ) (void)bu_rd(U_RBR);
	(void)bu_rd(U_IIR);
	(void)bu_rd(U_MSR);
	knl_bth4_init(&bu.h4);
	bu_wr(U_MCR, MCR_RTS | MCR_OUT2 | MCR_AFE);
	knl_define_inthdr(bu.intno, TA_HLNG, (FP)bu_inthdr);
	SetIntMode(bu.intno, IM_LEVEL);
	EnableInt(bu.intno, INTPRI_DEVICE);
	bu.irq_on = TRUE;
	bu_wr(U_IER, IER_RDI | IER_RLSI);
	tm_printf((UB *)"bt: uart: the line up, interrupt %d on; LSR %02x, MSR %02x (CTS %s)\n",
		  (INT)bu.intno, bu_rd(U_LSR) & 0xff, bu_rd(U_MSR) & 0xff,
		  ( bu_rd(U_MSR) & MSR_CTS ) ? "asserted: the controller is up" : "not asserted");

	knl_memset(&cfg, 0, sizeof(cfg));
	cfg.init_baud = BU_INIT_BAUD;
	cfg.oper_baud = bu.max_baud;
	knl_memcpy(cfg.bdaddr, bu.bdaddr, 6);
	cfg.speed = bu_speed;
	ok = knl_btb_load(c, &cfg);
	bu_count(ok ? "after the controller's start" : "the controller's start stopped");
	return ok;
}

LOCAL CONST BTTRANS	bu_tr = { bu_cmd, bu_acl, bu_prep, NULL };	/* the chip has no isochronous channels */

/*
 * At the start of the system (devinit.c), before the Wi-Fi: the pins and
 * the power, then the controller's task, which goes on by itself.
 */
EXPORT void knl_btu_init( void )
{
	if ( !bu_probe() ) return;
	bu_wr(U_IER, 0);
	bu_wr(U_MCR, 0);		/* RTS not asserted while the controller comes up */
	if ( !bu_pins() || !bu_power() ) return;
	bu.baud = BU_INIT_BAUD;
	if ( knl_btx_open(&bu_tr, NULL) == NULL ) {
		tm_printf((UB *)"bt: uart: the built-in Bluetooth not started\n");
		return;
	}
	tm_printf((UB *)"bt: uart: the controller's task started\n");
}

#endif /* RPI5 */
