/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_bcm.c
 *	A Broadcom/Cypress controller on a UART and its patch (design 6.8).
 *	The controller runs the firmware in its ROM from power on, at
 *	115200 baud; a patch for that ROM is given at every power on, into
 *	its RAM, and started. Without one it still works, as its ROM has it.
 *
 *	This runs in the controller's task before the ordinary start, and
 *	waits for its answers itself, as bt_intel.c does: nothing else is
 *	going on yet. The steps, each said on the console with its number
 *	("bt: bcm: 3 ..."), so that a log shows how far it came:
 *
 *	  1 Reset, at the speed after power on
 *	  2 its version: which chip, and so which patch
 *	  3 the speed raised (a vendor's command, then the host's end)
 *	  4 the patch read from its object and checked whole
 *	  5 Download Minidriver, then the patch's commands as they are,
 *	    the last of them Launch RAM
 *	  6 Reset again, at the speed after power on: the patch runs
 *	  7 its version again, and the speed raised again
 *	  8 its address: the chip's own placeholder replaced with the board's
 *
 *	Each command waits for its Command Complete; one with no answer or a
 *	status not 0 ends it there. A patch that is not there, or not whole,
 *	is not given, and the controller goes on as its ROM has it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "btint.h"
#include "bt_bcm.h"

#define C_RESET		0x0c03
#define C_READ_VER	0x1001
#define C_READ_ADDR	0x1009
#define C_BCM_ADDR	0xfc01		/* write the address */
#define C_BCM_BAUD	0xfc18		/* update the UART's speed: 0x0000, then the speed (32 bits) */
#define C_BCM_DOWNLOAD	0xfc2e		/* download minidriver */
#define C_BCM_CLOCK	0xfc45		/* the UART's clock: 1 is 48 MHz, needed above 3 Mbaud */
#define C_BCM_LAUNCH	0xfc4e		/* launch RAM */
#define OGF_VENDOR	0x3f

#define BCM_CLOCK_48MHZ	0x01
#define BAUD_NO_CLOCK	3000000		/* the most the default clock takes */

#define E_CMD_DONE	0x0e
#define E_CMD_STATUS	0x0f
#define E_VENDOR	0xff

#define CMD_TMO		2000
#define RESET_MS	100		/* after a Reset, before anything else */
#define DOWNLOAD_MS	50		/* after Download Minidriver */
#define LAUNCH_MS	250		/* after Launch RAM */
#define FW_MAX		( 256 * 1024 )

IMPORT ID knl_obfile_first_vol( void );

/* How long to wait for the volume with the patch's object, which may not be up yet */
EXPORT INT	knl_btb_fw_wait_ms = 30000;

/*
 * The chips by the LMP subversion their version answers, and the patch
 * the system has for each. The system has the CYW43455's (BCM4345C0),
 * the chip of the Raspberry Pi 4 and 5.
 */
LOCAL CONST struct {
	UH		subver;
	CONST char	*chip;
	CONST char	*uuid;		/* its patch's object, NULL none in the system */
} chips[] = {
	{ 0x6119, "BCM4345C0", SYSDEF_FW_BCM4345C0 },
	{ 0x6606, "BCM4345C5", NULL },
};

/* The addresses the chips answer before one is given: the board's is to be written */
LOCAL CONST UB	placeholder[][6] = {
	{ 0xac, 0x1f, 0x00, 0xc0, 0x45, 0x43 },		/* BCM4345C0 */
	{ 0xac, 0x1f, 0x00, 0xc5, 0x45, 0x43 },		/* BCM4345C5 */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

EXPORT INT knl_btb_hcd_check( CONST UB *d, INT n, INT *p_at )
{
	INT	at = 0, cmds = 0;
	UH	op = 0;

	*p_at = 0;
	if ( d == NULL || n <= 0 ) return BTB_HCD_EMPTY;
	while ( at < n ) {
		if ( n - at < 3 || n - at < 3 + d[at + 2] ) {
			*p_at = at;
			return BTB_HCD_SHORT;
		}
		op = (UH)( d[at] | ( d[at + 1] << 8 ) );
		if ( ( op >> 10 ) != OGF_VENDOR ) {
			*p_at = at;
			return BTB_HCD_NOTVENDOR;
		}
		at += 3 + d[at + 2];
		cmds++;
	}
	if ( op != C_BCM_LAUNCH ) {
		*p_at = at;
		return BTB_HCD_NOLAUNCH;
	}
	return cmds;
}

/* ---------------------------------------------------------------- commands */

/* The next event into c->ev; what else comes now is let go. FALSE when none came in time */
LOCAL BOOL next_event( BTC *c, INT tmo )
{
	UW	due = knl_btx_now() + (UW)tmo;

	for ( ;; ) {
		while ( c->head != c->tail ) {
			RINGITEM	*r = &c->ring[c->head];
			BOOL		ev = (BOOL)( r->kind == 0 && r->len >= 2 && r->len >= r->b[1] + 2 );

			if ( ev ) knl_memcpy(c->ev, r->b, r->b[1] + 2);
			c->head = ( c->head + 1 ) % RING_N;
			if ( ev ) return TRUE;
		}
		if ( c->gone || (W)( knl_btx_now() - due ) >= 0 ) return FALSE;
		(void)tk_wai_sem(c->sem, 1, 20);
	}
}

/*
 * A command and its answer: the return parameters after the status at
 * *p_ret, their length *p_len. FALSE when it was not taken, not answered,
 * or answered with a status not 0 (said on the console).
 */
LOCAL BOOL ask( BTC *c, UH op, CONST UB *p, INT n, CONST UB **p_ret, INT *p_len )
{
	UB	b[3 + 255];
	UW	due = knl_btx_now() + CMD_TMO;
	ER	er;

	b[0] = (UB)op;
	b[1] = (UB)( op >> 8 );
	b[2] = (UB)n;
	if ( n > 0 ) knl_memcpy(b + 3, p, n);
	er = c->gone ? E_IO : c->tr->cmd(c, b, n + 3);
	if ( er < E_OK ) {
		tm_printf((UB *)"bt: bcm: command %04x not sent (%d)\n", op, (INT)er);
		return FALSE;
	}
	while ( (W)( knl_btx_now() - due ) < 0 ) {
		if ( !next_event(c, CMD_TMO) ) break;
		if ( c->ev[0] == E_CMD_DONE && c->ev[1] >= 4 && (UH)( c->ev[3] | ( c->ev[4] << 8 ) ) == op ) {
			if ( c->ev[5] != 0 ) {
				tm_printf((UB *)"bt: bcm: command %04x answered status %02x\n", op, c->ev[5]);
				return FALSE;
			}
			if ( p_ret != NULL ) *p_ret = c->ev + 6;
			if ( p_len != NULL ) *p_len = c->ev[1] - 4;
			return TRUE;
		}
		if ( c->ev[0] == E_CMD_STATUS && c->ev[1] >= 4 && (UH)( c->ev[4] | ( c->ev[5] << 8 ) ) == op && c->ev[2] != 0 ) {
			tm_printf((UB *)"bt: bcm: command %04x refused, status %02x\n", op, c->ev[2]);
			return FALSE;
		}
		if ( c->ev[0] == E_VENDOR ) knl_bti_log_bytes("bcm: vendor event", c->ev + 2, c->ev[1]);
	}
	tm_printf((UB *)"bt: bcm: command %04x not answered in %d ms\n", op, CMD_TMO);
	return FALSE;
}

/* Why it stopped, kept for the settings and said on the console */
LOCAL BOOL fail( BTC *c, UW state, INT step, CONST char *why, CONST char *file )
{
	INT	i;

	tm_printf((UB *)"bt: bcm: stopped at step %d: %s%s%s\n", step, why,
		  ( file != NULL ) ? " " : "", ( file != NULL ) ? file : "");
	c->st.state = state;
	knl_memset(c->st.file, 0, sizeof(c->st.file));
	for ( i = 0; file != NULL && file[i] != 0 && i < (INT)sizeof(c->st.file) - 1; i++ ) c->st.file[i] = (UB)file[i];
	c->step = -1;
	return FALSE;
}

/* Reset, and the time a controller takes to be ready after it */
LOCAL BOOL reset( BTC *c )
{
	if ( !ask(c, C_RESET, NULL, 0, NULL, NULL) ) return FALSE;
	tk_dly_tsk(RESET_MS);
	return TRUE;
}

/* Its version: the LMP subversion, and the rest on the console */
LOCAL BOOL version( BTC *c, INT step, UH *p_subver )
{
	CONST UB	*r;
	INT		len = 0;
	UH		rev, sub;

	if ( !ask(c, C_READ_VER, NULL, 0, &r, &len) || len < 8 ) return FALSE;
	/* HCI version, its revision (16), LMP version, the maker (16), the LMP subversion (16) */
	rev = (UH)( r[1] | ( r[2] << 8 ) );
	sub = (UH)( r[6] | ( r[7] << 8 ) );
	tm_printf((UB *)"bt: bcm: %d version: HCI %d revision %04x, LMP %d subversion %04x, maker %04x: %03d.%03d.%03d build %04d\n",
		  step, r[0], rev, r[3], sub, r[4] | ( r[5] << 8 ),
		  ( sub & 0xe000 ) >> 13, ( sub & 0x1f00 ) >> 8, sub & 0xff, rev & 0x0fff);
	*p_subver = sub;
	return TRUE;
}

/* The controller's speed, then the host's end, then a question at the new speed */
LOCAL BOOL raise( BTC *c, CONST BTBCFG *cfg, INT step )
{
	UB	p[6];
	UH	sub;

	if ( cfg->oper_baud == 0 || cfg->oper_baud == cfg->init_baud || cfg->speed == NULL ) {
		tm_printf((UB *)"bt: bcm: %d speed stays %d baud\n", step, (INT)cfg->init_baud);
		return TRUE;
	}
	if ( cfg->oper_baud > BAUD_NO_CLOCK ) {
		p[0] = BCM_CLOCK_48MHZ;
		if ( !ask(c, C_BCM_CLOCK, p, 1, NULL, NULL) ) return FALSE;
	}
	p[0] = p[1] = 0;
	p[2] = (UB)cfg->oper_baud;
	p[3] = (UB)( cfg->oper_baud >> 8 );
	p[4] = (UB)( cfg->oper_baud >> 16 );
	p[5] = (UB)( cfg->oper_baud >> 24 );
	if ( !ask(c, C_BCM_BAUD, p, 6, NULL, NULL) ) return FALSE;
	if ( cfg->speed(c, cfg->oper_baud) < E_OK ) return FALSE;
	if ( !version(c, step, &sub) ) {
		tm_printf((UB *)"bt: bcm: %d no answer at %d baud\n", step, (INT)cfg->oper_baud);
		return FALSE;
	}
	tm_printf((UB *)"bt: bcm: %d speed %d baud\n", step, (INT)cfg->oper_baud);
	return TRUE;
}

/* A patch's bytes from its object into memory of its own (Kfree it); NULL when there is none */
LOCAL UB *fw_read( CONST char *uuid, INT *p_n )
{
	TS_UUID	u;
	UB	*b;
	SZ	asz = 0;
	ID	key;
	INT	waited = 0;

	if ( ts_str_to_uuid(uuid, &u) < E_OK ) return NULL;
	for ( ;; ) {
		/* the volume may come later than the controller: the object is not looked for before it */
		key = ( knl_obfile_first_vol() > 0 ) ? ob_opn_obj(&u, OB_OP_R) : 0;
		if ( key > 0 || waited >= knl_btb_fw_wait_ms ) break;
		tk_dly_tsk(500);
		waited += 500;
	}
	if ( key <= 0 ) return NULL;
	b = (UB *)Kmalloc(FW_MAX);
	if ( b != NULL && ob_rea_rec(key, 1, 0, b, FW_MAX, &asz) < E_OK ) {
		Kfree(b);
		b = NULL;
	}
	(void)ob_cls_obj(key);
	*p_n = (INT)asz;
	return b;
}

/* The patch given: Download Minidriver, then each of its commands as it is */
LOCAL BOOL give( BTC *c, CONST UB *fw, INT n, CONST char *file )
{
	INT	at = 0, cmds = 0;
	UW	t0 = knl_btx_now();

	if ( !ask(c, C_BCM_DOWNLOAD, NULL, 0, NULL, NULL) ) {
		return fail(c, BT_ST_FWFAILED, 5, "Download Minidriver not taken:", file);
	}
	tk_dly_tsk(DOWNLOAD_MS);
	while ( at < n ) {
		UH	op = (UH)( fw[at] | ( fw[at + 1] << 8 ) );
		INT	len = fw[at + 2];

		if ( !ask(c, op, fw + at + 3, len, NULL, NULL) ) {
			tm_printf((UB *)"bt: bcm: 5 the patch's command %d (%04x, at %d) not taken\n", cmds, op, at);
			return fail(c, BT_ST_FWFAILED, 5, "the patch not taken:", file);
		}
		at += 3 + len;
		cmds++;
	}
	tk_dly_tsk(LAUNCH_MS);
	tm_printf((UB *)"bt: bcm: 5 %s: %d commands, %d bytes taken in %d ms, launched\n",
		  file, cmds, n, (INT)( knl_btx_now() - t0 ));
	return TRUE;
}

/* The chip's placeholder address replaced with the board's */
LOCAL void address( BTC *c, CONST BTBCFG *cfg )
{
	CONST UB	*r;
	CONST UB	*a = cfg->bdaddr;
	INT		len = 0, i;
	BOOL		ph = FALSE;

	if ( !ask(c, C_READ_ADDR, NULL, 0, &r, &len) || len < 6 ) {
		tm_printf((UB *)"bt: bcm: 8 its address not answered\n");
		return;
	}
	for ( i = 0; i < (INT)( sizeof(placeholder) / sizeof(placeholder[0]) ); i++ ) {
		if ( same_bt_bytes(r, placeholder[i], 6) ) ph = TRUE;
	}
	tm_printf((UB *)"bt: bcm: 8 its address %02x:%02x:%02x:%02x:%02x:%02x%s\n",
		  r[5], r[4], r[3], r[2], r[1], r[0], ph ? ", the chip's placeholder" : "");
	if ( !ph ) return;
	if ( same_bt_bytes(a, placeholder[2], 6) ) {
		tm_printf((UB *)"bt: bcm: 8 the device tree gives no address: the placeholder stays\n");
		return;
	}
	if ( !ask(c, C_BCM_ADDR, a, 6, NULL, NULL) ) return;
	tm_printf((UB *)"bt: bcm: 8 the board's address %02x:%02x:%02x:%02x:%02x:%02x given\n",
		  a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* Going on without a patch: the address all the same */
LOCAL BOOL rom( BTC *c, CONST BTBCFG *cfg )
{
	address(c, cfg);
	tm_printf((UB *)"bt: bcm: done without a patch, on to the ordinary start\n");
	return TRUE;
}

LOCAL BOOL bcm_start( BTC *c, CONST BTBCFG *cfg, BOOL given, CONST UB *fw_given, INT n_given, CONST char *file_given )
{
	CONST char	*chip = NULL, *uuid = NULL, *file;
	char		name[24];
	UB		*fw = NULL;
	INT		n = 0, k, at = 0, i;
	UH		sub = 0;
	BOOL		ok;

	tm_printf((UB *)"bt: bcm: 1 reset at %d baud\n", (INT)cfg->init_baud);
	if ( !reset(c) ) {
		return fail(c, BT_ST_NOANSWER, 1, "no answer to Reset: the chip is not on, or the line is not as set", NULL);
	}
	if ( !version(c, 2, &sub) ) return fail(c, BT_ST_NOANSWER, 2, "its version not answered", NULL);
	for ( i = 0; i < (INT)( sizeof(chips) / sizeof(chips[0]) ); i++ ) {
		if ( chips[i].subver == sub ) {
			chip = chips[i].chip;
			uuid = chips[i].uuid;
		}
	}
	tm_printf((UB *)"bt: bcm: 2 the chip is %s\n", ( chip != NULL ) ? chip : "one not known here");
	if ( !raise(c, cfg, 3) ) return fail(c, BT_ST_NOANSWER, 3, "the speed not raised", NULL);

	/* the patch: given (the tests), or the chip's from its object */
	if ( given ) {
		fw = (UB *)fw_given;
		n = n_given;
		file = ( file_given != NULL ) ? file_given : "the patch";
	} else {
		tm_sprintf((UB *)name, (UB *)"%s.hcd", ( chip != NULL ) ? chip : "BCM");
		file = name;
		if ( uuid == NULL ) {
			tm_printf((UB *)"bt: bcm: 4 the system has no patch for %s: going on as its ROM has it\n",
				  ( chip != NULL ) ? chip : "this chip");
			return rom(c, cfg);
		}
		fw = fw_read(uuid, &n);
		if ( fw == NULL ) {
			tm_printf((UB *)"bt: bcm: 4 no object %s for %s: going on as its ROM has it\n", uuid, file);
			return rom(c, cfg);
		}
	}
	if ( fw == NULL ) {
		tm_printf((UB *)"bt: bcm: 4 no patch: going on as its ROM has it\n");
		return rom(c, cfg);
	}
	k = knl_btb_hcd_check(fw, n, &at);
	if ( k <= 0 ) {
		tm_printf((UB *)"bt: bcm: 4 %s (%d bytes) is not a whole patch (%d at %d): not given, going on as its ROM has it\n",
			  file, n, k, at);
		if ( !given ) Kfree(fw);
		return rom(c, cfg);
	}
	tm_printf((UB *)"bt: bcm: 4 %s: %d bytes, %d commands\n", file, n, k);
	ok = give(c, fw, n, file);
	if ( !given ) Kfree(fw);
	if ( !ok ) return FALSE;

	/* the patch runs, at the speed after power on */
	if ( cfg->speed != NULL && cfg->oper_baud != 0 && cfg->oper_baud != cfg->init_baud ) {
		if ( cfg->speed(c, cfg->init_baud) < E_OK ) return fail(c, BT_ST_FWFAILED, 6, "the host's speed not set:", file);
	}
	tm_printf((UB *)"bt: bcm: 6 reset at %d baud\n", (INT)cfg->init_baud);
	if ( !reset(c) ) return fail(c, BT_ST_FWFAILED, 6, "no answer to Reset after the patch:", file);
	if ( !version(c, 7, &sub) ) return fail(c, BT_ST_FWFAILED, 7, "its version not answered after the patch:", file);
	if ( !raise(c, cfg, 7) ) return fail(c, BT_ST_FWFAILED, 7, "the speed not raised after the patch:", file);
	address(c, cfg);
	tm_printf((UB *)"bt: bcm: done, on to the ordinary start\n");
	return TRUE;
}

EXPORT BOOL knl_btb_load( BTC *c, CONST BTBCFG *cfg )
{
	return bcm_start(c, cfg, FALSE, NULL, 0, NULL);
}

EXPORT BOOL knl_btb_run( BTC *c, CONST BTBCFG *cfg, CONST UB *fw, INT n, CONST char *file )
{
	return bcm_start(c, cfg, TRUE, fw, n, file);
}
