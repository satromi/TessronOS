/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	objtest.c (Raspberry Pi 4)
 *	Checks on the board of what QEMU's raspi4b does not have or cannot
 *	show (docs/tessronos/08-object.md 8.4, 03-platform.md 3.6.4): the
 *	banks of pins, the activity LED set through its bank, the
 *	temperature and the clocks the firmware tells, the screen the
 *	firmware hands over, and with them the board as a whole -- the
 *	processors that came up, the memory found, how the SD card moves its
 *	data, the PCIe link and what is on it, the USB devices, the Ethernet
 *	link and the random generator.
 *
 *	They run once in a task of their own a few seconds after the object
 *	layer is up, below everything that starts the system, and write
 *	"objtest:" lines to the console: one a check, then a summary. Each
 *	check has a time limit and gives up when it runs out, so a check
 *	that fails lets the next one run and the system go on starting. The
 *	one card image is meant to answer as many questions about a board as
 *	it can in one start. Only inputs are touched on the header: GPIO 26
 *	has its pull turned up and down to see a change told, and is put
 *	back as it was. make OBJTEST=0 leaves them out.
 */

#include <sys/machine.h>

#ifdef RPI4

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/gpio.h>
#include <ts/usb.h>
#include <ts/hid.h>
#include <ts/disp.h>
#include <ts/net.h>
#include <ts/pcie.h>
#include <ts/board.h>
#include "sysman/pfalloc.h"
#include "sysdepend.h"

/*
 * Not in the in-kernel test build: its "FAIL" lines would be taken for
 * the tests' own, and the checks that need the board's hardware fail
 * under QEMU as a matter of course
 */
#ifndef CNF_OBJTEST
#ifdef USE_KTEST
#define CNF_OBJTEST	0
#else
#define CNF_OBJTEST	1
#endif
#endif

#define TEST_PRI	30
#define TEST_STKSZ	16384
#define TEST_DELAY_MS	4000
#define CHECK_MS	3000		/* the most one check may wait */
#define LINK_MS		6000		/* an Ethernet link takes a few seconds to come up */
#define TEXT_MAX	4096
#define PULL_PIN	26		/* on the header, pin 37 */
#define PULL_BANK	0		/* gpio-0 */
#define NBANKS		3

IMPORT ER  knl_obgpio_uuid( INT k, TS_UUID *p_uuid );
IMPORT INT knl_sd_dma_mode( INT mode );

LOCAL UB	txt[TEXT_MAX];
LOCAL INT	npass = 0, nrun = 0;

LOCAL void note( CONST char *name, BOOL ok, CONST char *what )
{
	nrun++;
	if ( ok ) npass++;
	tm_printf((UB *)"objtest: %s %s%s%s\n", name, ok ? "ok" : "FAIL", ( what != NULL ) ? ": " : "",
		  ( what != NULL ) ? what : "");
}

LOCAL BOOL has( CONST UB *t, INT len, CONST char *w )
{
	INT	i, k, n = (INT)knl_strlen(w);

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* The line of a text that begins with w, copied (up to its end) into out */
LOCAL BOOL line_of( CONST UB *t, INT len, CONST char *w, char *out, INT max )
{
	INT	i = 0, k, n = (INT)knl_strlen(w), m;

	while ( i < len ) {
		for ( k = 0; k < n && i + k < len && t[i + k] == (UB)w[k]; k++ ) ;
		if ( k == n ) {
			for ( m = 0; i < len && t[i] != '\n' && m < max - 1; ) out[m++] = (char)t[i++];
			out[m] = 0;
			return TRUE;
		}
		while ( i < len && t[i] != '\n' ) i++;
		i++;
	}
	out[0] = 0;
	return FALSE;
}

LOCAL INT rec( CONST TS_UUID *u, UINT ops, INT recno )
{
	SZ	asz = 0;
	ER	er;
	ID	k = ob_opn_obj(u, ops);

	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, recno, 0, txt, TEXT_MAX - 1, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) return (INT)er;
	txt[asz] = 0;
	return (INT)asz;
}

LOCAL ER put( CONST TS_UUID *u, INT recno, CONST char *s )
{
	ER	er;
	ID	k = ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE);

	if ( k <= 0 ) return (ER)k;
	er = ob_wri_rec(k, recno, 0, s, (SZ)knl_strlen(s), NULL);
	(void)ob_cls_obj(k);
	return er;
}

/* ---------------------------------------------------------------- the board */

/* The four processors, and the memory the device tree gave */
LOCAL void check_board( void )
{
	char	s[160];
	UD	mb = 0, dma = knl_pf_total_count(ZONE_DMA32) >> 8;
	INT	i;

	for ( i = 0; i < knl_mem_range_n; i++ ) {
		mb += knl_mem_range[i].size >> 20;
	}
	tm_sprintf((UB *)s, (UB *)"%s, revision %06x, %d of %d processors, %d MB in %d ranges, %d MB for DMA",
		   knl_board.name, knl_board.rev, knl_num_prc, knl_num_cpu, (INT)mb, knl_mem_range_n, (INT)dma);
	note("board", knl_board.kind != BOARD_UNKNOWN && knl_num_prc == 4 && mb >= 900, s);
}

/* The card the system came up from, and how its data moves */
LOCAL void check_sd( void )
{
	static CONST char *way[4] = { "the data port", "SDMA", "ADMA2 32 bit", "ADMA2 64 bit" };
	INT	m = knl_sd_dma_mode(-1);
	char	s[80];

	tm_sprintf((UB *)s, (UB *)"data moved by %s", ( m >= 0 && m < 4 ) ? way[m] : "?");
	note("sd", m > 0, s);
}

/* The PCIe link and what is on it: the VL805 on the Pi 4 and 400 */
LOCAL void check_pcie( void )
{
	T_PCIDEV	d;
	char		s[120];
	INT		i, n = ts_pcie_count(), xhci = 0;

	for ( i = 0; i < n; i++ ) {
		if ( ts_pcie_get(i, &d) < E_OK ) continue;
		tm_printf((UB *)"objtest:   pci %d:%d.%d %04x:%04x class %02x.%02x.%02x\n",
			  d.bus, d.dev, d.fn, d.vendor, d.device, d.class_code, d.subclass, d.prog_if);
		if ( d.class_code == 0x0c && d.subclass == 0x03 && d.prog_if == 0x30 ) xhci++;
	}
	tm_sprintf((UB *)s, (UB *)"%d functions, %d xHCI", n, xhci);
	if ( knl_board.kind == BOARD_CM4 ) {
		note("pcie", TRUE, s);		/* the slot may well be empty */
	} else {
		note("pcie", n >= 2 && xhci == 1, s);
	}
}

/* The Ethernet link, waited for a while */
LOCAL void check_ethernet( void )
{
	T_NETSTAT	st;
	UB		mac[6] = { 0, 0, 0, 0, 0, 0 };
	char		s[96];
	INT		waited;
	BOOL		up = FALSE, mac_ok = FALSE;

	if ( net_get_mac(mac) >= E_OK ) {
		mac_ok = ( ( mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5] ) != 0 && ( mac[0] & 1 ) == 0 );
	}
	for ( waited = 0; waited < LINK_MS; waited += 200 ) {
		if ( net_stat(&st) >= E_OK && st.up ) {
			up = TRUE;
			break;
		}
		tk_dly_tsk(200);
	}
	tm_sprintf((UB *)s, (UB *)"MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s",
		   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], up ? "up" : "down (no cable?)");
	note("ethernet", mac_ok && up, s);
}

/* ---------------------------------------------------------------- the objects */

/* Each bank an object of its pins, a line a pin */
LOCAL void check_banks( void )
{
	static CONST INT	pins[NBANKS] = { 28, 18, 12 };
	char	s[80];
	TS_UUID	u;
	T_OBREF	r;
	INT	k, n, lines, i, there = 0, right = 0;

	for ( k = 0; k < NBANKS; k++ ) {
		if ( knl_obgpio_uuid(k, &u) < E_OK || ob_ref_obj(&u, &r) < E_OK || r.sub != OB_S_GPIO ) {
			continue;
		}
		there++;
		n = rec(&u, OB_OP_R, OB_GPIO_PINS);
		for ( i = 0, lines = 0; i < n; i++ ) {
			if ( txt[i] == '\n' ) lines++;
		}
		if ( lines == pins[k] ) right++;
		{
			UB	*nm = knl_ob_name(&u, NULL, NULL);

			tm_printf((UB *)"objtest:   %s %d pins\n", ( nm != NULL ) ? nm : (UB *)"?", lines);
			if ( nm != NULL ) Kfree(nm);
		}
	}
	tm_sprintf((UB *)s, (UB *)"%d of %d banks, %d with every pin", there, NBANKS, right);
	note("gpio banks", there == NBANKS && right == NBANKS, s);
}

/* The activity LED through its bank: lit shows as GPIO 42 driven high */
LOCAL void check_led( void )
{
	TS_UUID	u;
	char	l[40];
	BOOL	ok = TRUE;
	INT	i, n;

	if ( knl_obgpio_uuid(1, &u) < E_OK ) {
		note("led", FALSE, "no bank");
		return;
	}
	for ( i = 0; i < 3 && ok; i++ ) {
		ok = ( gpio_led(TRUE) >= E_OK );
		n = rec(&u, OB_OP_R, OB_GPIO_PINS);
		ok = ok && line_of(txt, n, "42 ", l, sizeof(l)) && has((UB *)l, (INT)knl_strlen(l), "42 out 1");
		tk_dly_tsk(200);
		ok = ok && ( gpio_led(FALSE) >= E_OK );
		n = rec(&u, OB_OP_R, OB_GPIO_PINS);
		ok = ok && line_of(txt, n, "42 ", l, sizeof(l)) && has((UB *)l, (INT)knl_strlen(l), "42 out 0");
		tk_dly_tsk(200);
	}
	if ( knl_board.led_power ) {
		(void)gpio_led(TRUE);		/* the Pi 400's power LED, lit again */
	}
	note("led", ok, "blinked three times through gpio-1");
}

/* A pin the kernel or the board has is not set */
LOCAL void check_kernel_pins( void )
{
	TS_UUID	a, b;
	ER	e1 = E_NOEXS, e2 = E_NOEXS;
	char	s[64];

	if ( knl_obgpio_uuid(0, &a) >= E_OK ) e1 = put(&a, OB_GPIO_PINS, "14 out 1 -\n");
	if ( knl_obgpio_uuid(1, &b) >= E_OK ) e2 = put(&b, OB_GPIO_PINS, "28 out 0 -\n");
	tm_sprintf((UB *)s, (UB *)"console TXD %d, PHY MDIO %d", (INT)e1, (INT)e2);
	note("kernel pins", e1 == E_BUSY && e2 == E_BUSY, s);
}

/* An input turned from pulled up to pulled down: told on the bank */
LOCAL void check_events( void )
{
	T_OBCRE	c;
	T_OBNTF	req;
	T_OBNTM	m;
	TS_UUID	u, ch;
	char	was[40], s[64], w[4][12];
	SZ	asz;
	INT	n, waited, got = 0, val = -1, i, p, k;
	ID	port, kw;

	if ( knl_obgpio_uuid(PULL_BANK, &u) < E_OK ) {
		note("gpio events", FALSE, "no bank");
		return;
	}
	n = rec(&u, OB_OP_R, OB_GPIO_PINS);
	(void)line_of(txt, n, "26 ", was, sizeof(was));
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK ) {
		note("gpio events", FALSE, "no channel");
		return;
	}
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	kw = ob_opn_obj(&u, OB_OP_ATRRD);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	if ( port <= 0 || kw <= 0 || ob_ntf_evt(kw, OB_GPIO_PINS, &req, port) <= 0 ) {
		note("gpio events", FALSE, "no request");
	} else {
		(void)put(&u, OB_GPIO_PINS, "26 in - up\n");
		tk_dly_tsk(400);
		while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz > 0 ) asz = 0;
		(void)put(&u, OB_GPIO_PINS, "26 in - down\n");
		for ( waited = 0; waited < CHECK_MS && got == 0; waited += 50 ) {
			asz = 0;
			while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
				if ( m.event == OB_E_CHANGE && m.code == PULL_PIN ) {
					got++;
					val = m.x;
				}
				asz = 0;
			}
			if ( got == 0 ) tk_dly_tsk(50);
		}
		tm_sprintf((UB *)s, (UB *)"GPIO 26 pulled down told %d time(s), value %d", got, val);
		note("gpio events", got >= 1 && val == 0, s);
	}
	/* the pin as it was: its direction and pull from the line read first */
	for ( i = 0, p = 0; i < 4; i++ ) {
		while ( was[p] == ' ' ) p++;
		for ( k = 0; was[p] != 0 && was[p] != ' ' && k < 11; ) w[i][k++] = was[p++];
		w[i][k] = 0;
	}
	if ( ( knl_strcmp(w[1], "in") == 0 || knl_strcmp(w[1], "out") == 0 ) && w[3][0] != 0 ) {
		tm_sprintf((UB *)s, (UB *)"26 %s - %s\n", w[1], w[3]);
		(void)put(&u, OB_GPIO_PINS, s);
	}
	if ( kw > 0 ) (void)ob_cls_obj(kw);
	if ( port > 0 ) (void)ob_cls_obj(port);
	(void)ob_del_obj(&ch);
}

/* The temperature and the clocks, from the firmware, in the system's record 9 */
LOCAL void check_hw( void )
{
	char	t[40], a[40], v[40], s[140];
	INT	n = rec(&ob_uuid_system, OB_OP_R, OB_SYS_HW);
	BOOL	ok;

	ok = ( n > 0 && line_of(txt, n, "TEMPERATURE\t", t, sizeof(t))
	    && line_of(txt, n, "ARM_HZ\t", a, sizeof(a)) && line_of(txt, n, "V3D_HZ\t", v, sizeof(v)) );
	ok = ok && !has((UB *)t, (INT)knl_strlen(t), "unknown") && !has((UB *)a, (INT)knl_strlen(a), "unknown")
		&& a[7] >= '1' && a[7] <= '9';
	tm_sprintf((UB *)s, (UB *)"%s, %s, %s", t, a, v);
	note("temperature and clocks", ok, s);
}

/* The screen: its mode read, set again to its own size, its pixels given with x */
LOCAL void check_display( void )
{
	T_DISPSPEC	d;
	char		sz[40], cur[24], s[96];
	INT		n;
	ER		er;

	if ( ts_disp_ref(&d) < E_OK ) {
		note("display", TRUE, "no screen, and no object");
		return;
	}
	n = rec(&ob_uuid_display, OB_OP_R, OB_DSP_MODE);
	(void)line_of(txt, n, "SIZE\t", sz, sizeof(sz));
	tm_sprintf((UB *)cur, (UB *)"%dx%d", (INT)d.width, (INT)d.height);
	er = put(&ob_uuid_display, OB_DSP_MODE, cur);
	n = rec(&ob_uuid_display, OB_OP_R | OB_OP_EXEC, OB_DSP_PIXELS);
	tm_sprintf((UB *)s, (UB *)"%s, set to %s again %d, pixels %d", sz, cur, (INT)er, n);
	note("display", sz[0] != 0 && er == E_OK && n == TEXT_MAX - 1, s);
}

/* The USB devices and the keyboards and pointers, each an object */
LOCAL void check_usb( void )
{
	T_USBDEV	*d = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 32);
	T_HIDINFO	h[16];
	TS_UUID		u;
	char		s[64];
	INT		n = 0, nh, i, ok = 0, okh = 0;

	if ( d != NULL ) {
		n = ts_usb_lst_dev(d, 32);
		for ( i = 0; i < n && i < 32; i++ ) {
			if ( knl_obusb_uuid(d[i].dev, &u) >= E_OK ) ok++;
		}
		Kfree(d);
	}
	nh = knl_hid_list(h, 16);
	for ( i = 0; i < nh && i < 16; i++ ) {
		if ( knl_obinput_uuid(h[i].id, &u) >= E_OK ) okh++;
	}
	tm_sprintf((UB *)s, (UB *)"%d/%d USB devices, %d/%d keyboards and pointers", ok, n, okh, nh);
	note("usb objects", ok == n && okh == nh, s);
}

LOCAL void check_random( void )
{
	UB	a[32], b[32];
	INT	i, same = 0;
	SZ	asz = 0;
	UW	hw = 0;
	ID	k = ob_opn_obj(&ob_uuid_random, OB_OP_READ);
	BOOL	ok = ( k > 0 && ob_rea_rec(k, OB_RND_DATA, 0, a, 32, &asz) >= E_OK
		    && ob_rea_rec(k, OB_RND_DATA, 0, b, 32, &asz) >= E_OK );
	ER	er = knl_hwrng_read(&hw);

	for ( i = 0; i < 32; i++ ) {
		if ( a[i] == b[i] ) same++;
	}
	if ( k > 0 ) (void)ob_cls_obj(k);
	note("random", ok && same < 8 && er == E_OK, ( er == E_OK ) ? "with the rng200" : "without the rng200");
}

/* ---------------------------------------------------------------- the task */

LOCAL void test_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	tk_dly_tsk(TEST_DELAY_MS);
	tm_printf((UB *)"objtest: start\n");
	check_board();
	check_sd();
	check_pcie();
	check_banks();
	check_led();
	check_kernel_pins();
	check_events();
	check_hw();
	check_display();
	check_usb();
	check_ethernet();
	check_random();
	tm_printf((UB *)"objtest: summary %d/%d passed\n", npass, nrun);
	tk_ext_tsk();
}

EXPORT void knl_objtest_start( void )
{
	T_CTSK	ctsk;
	ID	tid;

	if ( !CNF_OBJTEST ) {
		return;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)test_task;
	ctsk.itskpri = TEST_PRI;
	ctsk.stksz = TEST_STKSZ;
	tid = tk_cre_tsk(&ctsk);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		tm_printf((UB *)"objtest: could not start (%d)\n", (INT)tid);
	}
}

#endif /* RPI4 */
