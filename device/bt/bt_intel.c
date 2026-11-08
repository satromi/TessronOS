/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_intel.c
 *	An Intel controller's firmware (design 6.8). The controller comes up
 *	in its boot loader, which answers only its vendor's commands, and
 *	takes its firmware at every power on.
 *
 *	This runs in the controller's task before the ordinary start, and
 *	waits for its answers itself: nothing else is going on yet. Everything
 *	the controller says of itself goes to the console.
 *
 *	The steps: the version says what it runs. In its boot loader, the boot
 *	parameters give the rest of the file's name; the file comes from its
 *	object (record 1). The secure send commands go on the bulk endpoint,
 *	and in the boot loader events may come back there too: the header
 *	with its key and signature, then the file's commands in pieces that
 *	end on a four byte boundary. A vendor's event says the signature was
 *	good; a reset with the boot address the file named starts the firmware,
 *	and another event says it is up. Then the .ddc file's settings.
 *
 *	A failure leaves the controller as it is, and the reason in its state
 *	(T_BTSTATE), for the settings to show. The adapter is then not ready.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "btint.h"

#define C_INTEL_RESET	0xfc01
#define C_INTEL_VER	0xfc05
#define C_SECURE_SEND	0xfc09
#define C_INTEL_BOOT	0xfc0d
#define C_BOOT_ADDR	0xfc0e		/* in the file: where to start */
#define C_WRITE_DDC	0xfc8b

#define E_CMD_DONE	0x0e
#define E_VENDOR	0xff
#define EV_BOOTED	0x02
#define EV_LOADED	0x06

#define FW_VARIANT_BOOT	0x06
#define FW_VARIANT_OPER	0x23
#define TLV_IMAGE	0x1c
#define IMAGE_BOOT	0x01

#define FRAG_INIT	0x00
#define FRAG_DATA	0x01
#define FRAG_SIGN	0x02
#define FRAG_PKEY	0x03
#define FRAG_MAX	252

#define RSA_HEADER	644		/* the header with its key and signature, in the older files */
#define CSS_VERSION_AT	8
#define CSS_RSA		0x00010000

#define FW_MAX		( 1200 * 1024 )
#define CMD_TMO		2000
#define FRAG_TMO	2000
#define LOAD_TMO	5000
#define BOOT_TMO	5000

IMPORT ID knl_obfile_first_vol( void );

/* How long to wait for the volume with the firmware's object, which may not be up yet */
EXPORT INT	knl_bti_fw_wait_ms = 30000;

/*
 * The files there are objects for (tools/x64/wifi/mkobj.py). These are the
 * same bytes as ibt-20-1-3 under another name, as the maker publishes them.
 */
LOCAL CONST struct { CONST char *name, *uuid; } fw_objs[] = {
	{ "ibt-12-16.sfi",  "01a0d8c4-54f0-7b01-8c10-01f0a2b354f0" },
	{ "ibt-12-16.ddc",  "01a0d8c4-54f2-7b03-8c12-03f2a2b354f2" },
	{ "ibt-20-0-3.sfi", "01a0d8c4-54f4-7b05-8c14-05f4a2b354f4" },
	{ "ibt-20-0-3.ddc", "01a0d8c4-54f6-7b07-8c16-07f6a2b354f6" },
	{ "ibt-20-1-3.sfi", "01a0d8c4-54f8-7b09-8c18-09f8a2b354f8" },
	{ "ibt-20-1-3.ddc", "01a0d8c4-54fa-7b0b-8c1a-0bfaa2b354fa" },
	{ "ibt-20-1-4.sfi", "01a0d8c4-54fc-7b0d-8c1c-0dfca2b354fc" },
	{ "ibt-20-1-4.ddc", "01a0d8c4-54fe-7b0f-8c1e-0ffea2b354fe" }
};

/* Bytes on the console in lines of 32, as they are: what a machine's log has to tell later */
EXPORT void knl_bti_log_bytes( CONST char *what, CONST UB *b, INT n )
{
	INT	i, at;

	tm_printf((UB *)"bt: %s, %d bytes\n", what, n);
	for ( at = 0; at < n; at += 32 ) {
		char	t[3 * 32 + 1];
		INT	m = 0;

		for ( i = at; i < n && i < at + 32; i++ ) m += tm_sprintf((UB *)t + m, (UB *)" %02x", b[i]);
		tm_printf((UB *)"bt:  %s\n", t);
	}
}

/* The next whole event from either endpoint, into c->ev; FALSE when none came in time */
LOCAL BOOL next_event( BTC *c, INT tmo )
{
	UW	due = knl_btx_now() + (UW)tmo;

	for ( ;; ) {
		while ( c->head != c->tail ) {
			RINGITEM	*r = &c->ring[c->head];
			INT		n = r->len;
			BOOL		whole = FALSE;

			if ( c->ev_have + n <= (INT)sizeof(c->ev) ) {
				knl_memcpy(c->ev + c->ev_have, r->b, n);
				c->ev_have += n;
			} else {
				c->ev_have = 0;
			}
			c->head = ( c->head + 1 ) % RING_N;
			whole = (BOOL)( c->ev_have >= 2 && c->ev_have >= c->ev[1] + 2 );
			if ( whole ) {
				c->ev_have = 0;
				return TRUE;
			}
		}
		if ( c->gone || (W)( knl_btx_now() - due ) >= 0 ) return FALSE;
		(void)tk_wai_sem(c->sem, 1, 50);
	}
}

LOCAL ER send_control( BTC *c, UH op, CONST UB *p, INT n )
{
	UB	b[3 + 255];
	ER	er;

	b[0] = (UB)op;
	b[1] = (UB)( op >> 8 );
	b[2] = (UB)n;
	if ( n > 0 ) knl_memcpy(b + 3, p, n);
	knl_usb_lock();
	er = c->gone ? E_IO : knl_usb_control(c->dev, USB_RT_CLASS, 0x00, 0, 0, b, (UH)( n + 3 ), NULL);
	knl_usb_unlock();
	return er;
}

/* A command and its answer: c->ev has it after, the returns from *p_ret; FALSE when it did not come */
LOCAL BOOL ask( BTC *c, UH op, CONST UB *p, INT n, CONST UB **p_ret, INT *p_len )
{
	UW	due = knl_btx_now() + CMD_TMO;

	if ( send_control(c, op, p, n) < E_OK ) return FALSE;
	while ( (W)( knl_btx_now() - due ) < 0 ) {
		if ( !next_event(c, CMD_TMO) ) return FALSE;
		if ( c->ev[0] == E_CMD_DONE && c->ev[1] >= 3 && (UH)( c->ev[3] | ( c->ev[4] << 8 ) ) == op ) {
			*p_ret = c->ev + 5;
			*p_len = c->ev[1] - 3;
			return TRUE;
		}
		if ( c->ev[0] == E_VENDOR ) knl_bti_log_bytes("Intel event", c->ev + 2, c->ev[1]);
	}
	return FALSE;
}

/* Why it stopped, kept for the settings and said on the console */
LOCAL BOOL fail( BTC *c, UW state, CONST char *why, CONST char *file )
{
	INT	i;

	tm_printf((UB *)"bt: Intel firmware not loaded: %s%s%s\n", why, ( file != NULL ) ? " " : "", ( file != NULL ) ? file : "");
	c->st.state = state;
	knl_memset(c->st.file, 0, sizeof(c->st.file));
	for ( i = 0; file != NULL && file[i] != 0 && i < (INT)sizeof(c->st.file) - 1; i++ ) c->st.file[i] = (UB)file[i];
	c->step = -1;
	return FALSE;
}

/* A file's bytes from its object into memory of its own (Kfree it); NULL when there is none */
LOCAL UB *fw_read( CONST char *name, INT *p_n )
{
	TS_UUID	u;
	UB	*b;
	SZ	asz = 0;
	ID	key;
	INT	i, waited = 0;
	CONST char *id = NULL;

	for ( i = 0; i < (INT)( sizeof(fw_objs) / sizeof(fw_objs[0]) ); i++ ) {
		if ( knl_strcmp(fw_objs[i].name, name) == 0 ) id = fw_objs[i].uuid;
	}
	if ( id == NULL || ts_str_to_uuid(id, &u) < E_OK ) return NULL;
	for ( ;; ) {
		/* the volume may come later than the controller: the object is not looked for before it */
		key = ( knl_obfile_first_vol() > 0 ) ? ob_opn_obj(&u, OB_OP_R) : 0;
		if ( key > 0 || waited >= knl_bti_fw_wait_ms ) break;
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

/* One secure send on the bulk endpoint, and its answer on either; a load's end seen on the way is kept */
LOCAL BOOL send_frag( BTC *c, UB type, CONST UB *d, INT n, BOOL *p_loaded, UB *p_result )
{
	UB	b[4 + FRAG_MAX];
	INT	got = 0;
	ER	er;

	b[0] = (UB)C_SECURE_SEND;
	b[1] = (UB)( C_SECURE_SEND >> 8 );
	b[2] = (UB)( n + 1 );
	b[3] = type;
	knl_memcpy(b + 4, d, n);
	knl_usb_lock();
	er = c->gone ? E_IO : c->dev->hc->ops->bulk(c->dev->hc, c->dev, &c->ep_out, b, n + 4, &got, 2000);
	knl_usb_unlock();
	if ( er < E_OK ) {
		tm_printf((UB *)"bt: Intel secure send not taken (%d)\n", (INT)er);
		return FALSE;
	}
	for ( ;; ) {
		if ( !next_event(c, FRAG_TMO) ) {
			tm_printf((UB *)"bt: Intel secure send not answered\n");
			return FALSE;
		}
		if ( c->ev[0] == E_CMD_DONE ) {
			if ( c->ev[5] != 0 ) {
				tm_printf((UB *)"bt: Intel secure send answered status %02x\n", c->ev[5]);
				return FALSE;
			}
			return TRUE;
		}
		if ( c->ev[0] == E_VENDOR && c->ev[2] == EV_LOADED ) {
			*p_loaded = TRUE;
			*p_result = c->ev[3];
			return TRUE;
		}
	}
}

/* Pieces of up to FRAG_MAX of one kind */
LOCAL BOOL send_part( BTC *c, UB type, CONST UB *d, INT n, BOOL *p_loaded, UB *p_result )
{
	while ( n > 0 ) {
		INT	k = ( n > FRAG_MAX ) ? FRAG_MAX : n;

		if ( !send_frag(c, type, d, k, p_loaded, p_result) ) return FALSE;
		d += k;
		n -= k;
	}
	return TRUE;
}

/* The file: its header, then its commands, the boot address noted among them */
LOCAL BOOL send_file( BTC *c, CONST UB *fw, INT n, UW *p_boot, CONST char *name )
{
	BOOL	loaded = FALSE;
	UB	result = 0;
	INT	sent = RSA_HEADER, ready = 0;
	UW	css;

	if ( n < RSA_HEADER ) return fail(c, BT_ST_FWFAILED, "a file too short:", name);
	css = (UW)fw[CSS_VERSION_AT] | ( (UW)fw[CSS_VERSION_AT + 1] << 8 ) | ( (UW)fw[CSS_VERSION_AT + 2] << 16 ) | ( (UW)fw[CSS_VERSION_AT + 3] << 24 );
	if ( css != CSS_RSA ) return fail(c, BT_ST_FWFAILED, "a header not of the form known:", name);
	/* the header, its key (the four bytes of the exponent are not sent) and its signature */
	if ( !send_part(c, FRAG_INIT, fw, 128, &loaded, &result)
	  || !send_part(c, FRAG_PKEY, fw + 128, 256, &loaded, &result)
	  || !send_part(c, FRAG_SIGN, fw + 388, 256, &loaded, &result) ) {
		return fail(c, BT_ST_FWFAILED, "its header was not taken:", name);
	}
	while ( n - sent - ready >= 3 ) {
		CONST UB	*h = fw + sent + ready;
		UH		op = (UH)( h[0] | ( h[1] << 8 ) );

		if ( op == C_BOOT_ADDR && h[2] >= 4 ) {
			*p_boot = (UW)h[3] | ( (UW)h[4] << 8 ) | ( (UW)h[5] << 16 ) | ( (UW)h[6] << 24 );
		}
		ready += 3 + h[2];
		while ( ready >= FRAG_MAX ) {
			if ( !send_frag(c, FRAG_DATA, fw + sent, FRAG_MAX, &loaded, &result) ) {
				return fail(c, BT_ST_FWFAILED, "its commands were not taken:", name);
			}
			sent += FRAG_MAX;
			ready -= FRAG_MAX;
		}
		if ( ready > 0 && ( ready & 3 ) == 0 ) {
			if ( !send_frag(c, FRAG_DATA, fw + sent, ready, &loaded, &result) ) {
				return fail(c, BT_ST_FWFAILED, "its commands were not taken:", name);
			}
			sent += ready;
			ready = 0;
		}
	}
	while ( !loaded ) {
		if ( !next_event(c, LOAD_TMO) ) return fail(c, BT_ST_FWFAILED, "no word that the load ended:", name);
		if ( c->ev[0] == E_VENDOR && c->ev[2] == EV_LOADED ) {
			loaded = TRUE;
			result = c->ev[3];
		}
	}
	if ( result != 0 ) {
		tm_printf((UB *)"bt: Intel load result %02x\n", result);
		return fail(c, BT_ST_FWFAILED, "the controller refused", name);
	}
	return TRUE;
}

/* The settings of the .ddc file, each (length, its bytes) a command; a machine may have none */
LOCAL void send_ddc( BTC *c, CONST char *name )
{
	CONST UB	*ret;
	UB		*d;
	INT		n = 0, at = 0, len, saved = knl_bti_fw_wait_ms;

	knl_bti_fw_wait_ms = 0;
	d = fw_read(name, &n);
	knl_bti_fw_wait_ms = saved;
	if ( d == NULL ) {
		tm_printf((UB *)"bt: no %s: its settings left\n", name);
		return;
	}
	while ( at < n && at + 1 + d[at] <= n ) {
		INT	l = d[at] + 1;

		if ( !ask(c, C_WRITE_DDC, d + at, l, &ret, &len) || len < 1 || ret[0] != 0 ) {
			tm_printf((UB *)"bt: %s: a setting not taken at %d\n", name, at);
			break;
		}
		at += l;
	}
	tm_printf((UB *)"bt: %s: %d of %d bytes given\n", name, at, n);
	Kfree(d);
}

/* The name of the older kinds' file, without its suffix; FALSE for a variant of no known form */
LOCAL BOOL fw_name( CONST UB *ver, UH revid, char *name )
{
	switch ( ver[2] ) {
	case 0x0b: case 0x0c:
		tm_sprintf((UB *)name, (UB *)"ibt-%d-%d", ver[2], revid);
		return TRUE;
	case 0x11: case 0x12: case 0x13: case 0x14:
		tm_sprintf((UB *)name, (UB *)"ibt-%d-%d-%d", ver[2], ver[3], ver[5]);
		return TRUE;
	default:
		return FALSE;
	}
}

/*
 * Before the ordinary start. TRUE when that may go on: the controller runs its
 * firmware, or did not say it does not; FALSE when it is left, with the reason.
 */
EXPORT BOOL knl_bti_load( BTC *c )
{
	CONST UB	*r;
	UB		ver[10], p[8];
	UH		revid = 0;
	char		base[24], file[28];
	UB		*fw;
	UW		boot = 0;
	INT		n = 0, len = 0, at;
	UW		t0;
	UB		arg = 0xff;

	/* a parameter of 0xff asks for the newer form; the older kinds answer in their own */
	if ( !ask(c, C_INTEL_VER, &arg, 1, &r, &len) ) {
		tm_printf((UB *)"bt: Intel version: no answer; going on with the ordinary start\n");
		return TRUE;
	}
	knl_bti_log_bytes("Intel version", r, len);
	if ( len < 1 || r[0] != 0 ) {
		tm_printf((UB *)"bt: Intel version: status %02x; going on with the ordinary start\n", ( len > 0 ) ? r[0] : 0xff);
		return TRUE;
	}
	if ( len != 10 ) {
		/* the newer form: every field said; no firmware of this form is known here yet */
		BOOL	in_boot = FALSE;

		for ( at = 1; at + 2 <= len && at + 2 + r[at + 1] <= len; at += 2 + r[at + 1] ) {
			INT	i, m = 0, l = r[at + 1];
			char	v[3 * 32 + 1];

			for ( i = 0; i < l && i < 32; i++ ) m += tm_sprintf((UB *)v + m, (UB *)" %02x", r[at + 2 + i]);
			tm_printf((UB *)"bt: Intel, type %02x:%s\n", r[at], v);
			if ( r[at] == TLV_IMAGE && l >= 1 && r[at + 2] == IMAGE_BOOT ) in_boot = TRUE;
		}
		if ( in_boot ) return fail(c, BT_ST_NOFIRMWARE, "the newer form of version has no file here yet", NULL);
		return TRUE;
	}
	knl_memcpy(ver, r, 10);
	tm_printf((UB *)"bt: Intel, older form: hw platform %02x variant %02x revision %02x, fw variant %02x revision %02x build %d week %d year %d patch %d\n",
		  ver[1], ver[2], ver[3], ver[4], ver[5], ver[6], ver[7], ver[8], ver[9]);
	if ( ver[4] == FW_VARIANT_OPER ) return TRUE;
	if ( ver[4] != FW_VARIANT_BOOT ) return fail(c, BT_ST_FWFAILED, "a firmware variant not known", NULL);

	if ( !ask(c, C_INTEL_BOOT, NULL, 0, &r, &len) || len < 1 || r[0] != 0 ) {
		return fail(c, BT_ST_FWFAILED, "its boot parameters did not come", NULL);
	}
	knl_bti_log_bytes("Intel boot parameters", r, len);
	/* status, three of its OTP, the revision, the secure boot's settings, then the address and the rest */
	if ( len >= 6 ) revid = (UH)( r[4] | ( r[5] << 8 ) );
	if ( len >= 23 && r[22] != 0 ) return fail(c, BT_ST_FWFAILED, "a loading method not known (limited CCE)", NULL);
	if ( !fw_name(ver, revid, base) ) return fail(c, BT_ST_FWFAILED, "a hardware variant not known", NULL);
	tm_sprintf((UB *)file, (UB *)"%s.sfi", base);
	tm_printf((UB *)"bt: Intel: the file for it is %s\n", file);
	fw = fw_read(file, &n);
	if ( fw == NULL ) return fail(c, BT_ST_NOFIRMWARE, "no object of", file);
	t0 = knl_btx_now();
	if ( !send_file(c, fw, n, &boot, file) ) {
		Kfree(fw);
		return FALSE;
	}
	Kfree(fw);
	tm_printf((UB *)"bt: %s: %d bytes taken in %d ms, boot address %08x\n", file, n, (INT)( knl_btx_now() - t0 ), boot);

	/* the firmware started: this command has no answer but the event when it is up */
	p[0] = 0x00;		/* the reset's kind */
	p[1] = 0x01;		/* with the patch */
	p[2] = 0x00;		/* the settings not reloaded */
	p[3] = 0x01;		/* from the address given */
	p[4] = (UB)boot;
	p[5] = (UB)( boot >> 8 );
	p[6] = (UB)( boot >> 16 );
	p[7] = (UB)( boot >> 24 );
	if ( send_control(c, C_INTEL_RESET, p, 8) < E_OK ) return fail(c, BT_ST_FWFAILED, "its reset was not taken:", file);
	for ( ;; ) {
		if ( !next_event(c, BOOT_TMO) ) return fail(c, BT_ST_FWFAILED, "no word that it started:", file);
		if ( c->ev[0] == E_VENDOR && c->ev[2] == EV_BOOTED ) break;
	}
	tm_printf((UB *)"bt: Intel firmware started\n");

	if ( !ask(c, C_INTEL_VER, &arg, 1, &r, &len) ) return fail(c, BT_ST_FWFAILED, "no version after the start:", file);
	knl_bti_log_bytes("Intel version", r, len);
	if ( len == 10 && r[4] != FW_VARIANT_OPER ) return fail(c, BT_ST_FWFAILED, "still not running after the start:", file);
	tm_sprintf((UB *)file, (UB *)"%s.ddc", base);
	send_ddc(c, file);
	return TRUE;
}
