/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_ser.c
 *	The serial ports as devices, and the console's record (design 10.5)
 *
 *	The machine the tests run on has two PL011s: the console's, and a
 *	second one with nothing on the far end. The second is turned round
 *	on itself (the UART's own loopback) so that what is written is what
 *	is read.
 */

#include "ktest.h"
#include <ts/ser.h>

#define PL_CR		0x30
#define CR_LBE		0x0080

#ifdef QEMU_VIRT
#define SECOND_CR()	( DEV_BASE(0x09040000) + PL_CR )	/* the machine's second PL011 */
#endif
#ifdef RPI5
/*
 * The port that is not the console, a PL011 as well: UART0 of the RP1 on
 * header pins 8 and 10, or the debug UART when the console moved there
 * (CNF_RP1_CONSOLE). Turned back on itself, nothing leaves by its pins.
 */
IMPORT UBINT	tm_uart_base;
#define SECOND_CR()	( ( ( tm_uart_base == RP1_UART0_BASE ) ? DEV_BASE(PL011_UART10_PA) \
					: RP1_UART0_BASE ) + PL_CR )
#endif

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

IMPORT INT	(*tm_getc_hook)( void );

/* the ports are there, the console's first and saying so */
LOCAL void test_ports( void )
{
	T_LDEV		ld[16];
	T_RDEV		rd;
	T_SERINFO	info;
	SZ		asz;
	ID		dd;
	INT		n, i, ports = 0;

	n = tk_lst_dev(ld, 0, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( tk_ref_dev(ld[i].devnm, &rd) >= E_OK && ( rd.devatr & 0xFF ) == TDK_SERIAL ) {
			ports++;
		}
	}
	KT_ASSERT(ports >= 1);
	tm_printf((UB *)"  %d serial ports\n", ports);

	dd = tk_opn_dev((UB *)"sera", TD_READ);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_SER_INFO, &info, sizeof(info), &asz), E_OK);
	KT_ASSERT(info.console);
	KT_ASSERT(info.speed > 0);
	/* the monitor reads the console through the driver, which has its input */
	KT_ASSERT(tm_getc_hook != NULL);
	if ( tm_getc_hook != NULL ) {
		KT_ASSERT_EQ(tm_getc_hook(), -1);	/* nobody has typed */
	}
	tm_printf((UB *)"  sera: %s\n", info.label);
	tk_cls_dev(dd, 0);
}

/* what is written on the second port comes back on it */
LOCAL void test_loop( void )
{
	CONST char	*msg = "TessronOS serial 0123456789\n";
	UB		buf[64];
	T_SERINFO	info;
	TMO		tmo = 1000;
	SZ		asz;
	UW		speed;
	INT		n = s_len(msg), got = 0, i, tries;
	ID		dd;

	dd = tk_opn_dev((UB *)"serb", TD_UPDATE);
	if ( dd <= 0 ) KT_SKIP("no second port");
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_SER_INFO, &info, sizeof(info), &asz), E_OK);
	KT_ASSERT(!info.console);

	/* the speed is the port's to set; the console's is not */
	speed = 9600;
	KT_ASSERT_ER(tk_swri_dev(dd, TDN_SER_SPEED, &speed, sizeof(speed), &asz), E_OK);
	speed = 0;
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_SER_SPEED, &speed, sizeof(speed), &asz), E_OK);
	KT_ASSERT_EQ(speed, 9600);
	speed = 115200;
	KT_ASSERT_ER(tk_swri_dev(dd, TDN_SER_SPEED, &speed, sizeof(speed), &asz), E_OK);

#ifdef SECOND_CR
	out_w(SECOND_CR(), in_w(SECOND_CR()) | CR_LBE);
#endif
	/* nothing waiting: a read with no time-out answers at once */
	KT_ASSERT_ER(tk_srea_dev(dd, 0, buf, sizeof(buf), &asz), E_OK);
	KT_ASSERT_ER(tk_swri_dev(dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz), E_OK);
	KT_ASSERT_ER(tk_swri_dev(dd, 0, msg, n, &asz), E_OK);
	KT_ASSERT_EQ((INT)asz, n);
	for ( tries = 0; got < n && tries < 20; tries++ ) {
		asz = 0;
		if ( tk_srea_dev(dd, 0, buf + got, sizeof(buf) - got, &asz) < E_OK || asz == 0 ) {
			break;
		}
		got += (INT)asz;
	}
#ifdef SECOND_CR
	out_w(SECOND_CR(), in_w(SECOND_CR()) & ~(UW)CR_LBE);
#endif
	tm_printf((UB *)"  %d of %d bytes came back\n", got, n);
	KT_ASSERT_EQ(got, n);
	for ( i = 0; i < got && i < n; i++ ) {
		if ( buf[i] != (UB)msg[i] ) break;
	}
	KT_ASSERT_EQ(i, n);
	tmo = TMO_POL;
	(void)tk_swri_dev(dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz);
	tk_cls_dev(dd, 0);
}

/* what goes to the console is kept, to be read back from where one was */
LOCAL void test_console_log( void )
{
	UB	*buf = (UB *)Kmalloc(4096);
	UD	pos = ~(UD)0 >> 1;
	INT	n, i, k, found = 0;
	CONST char *mark = "console-log-mark-4711";

	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	(void)tm_log_read(&pos, buf, 1);		/* to the end: an old place is moved on */
	tm_printf((UB *)"  %s\n", mark);
	n = tm_log_read(&pos, buf, 4096);
	KT_ASSERT(n > s_len(mark));
	for ( i = 0; i + s_len(mark) <= n && !found; i++ ) {
		for ( k = 0; mark[k] != 0 && buf[i + k] == (UB)mark[k]; k++ ) ;
		found = ( mark[k] == 0 );
	}
	KT_ASSERT(found);
	/* and nothing more until more is written */
	KT_ASSERT_EQ(tm_log_read(&pos, buf, 4096), 0);
	Kfree(buf);
}

EXPORT void ktest_ser( void )
{
	KT_RUN(test_ports);
	KT_RUN(test_loop);
	KT_RUN(test_console_log);
}
