/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_le.c
 *	LE input devices (design 6.8): found by what they advertise, paired by
 *	SMP, their reports read by GATT as HID over GATT says. This end is
 *	always the central.
 *
 *	Pairing asks for no number, as on the classic link: this end says it
 *	has neither a display nor a keyboard, which makes it Just Works,
 *	whether the older pairing (the key from a TK of zero) or Secure
 *	Connections (a key agreed on P-256) as the device can. Neither guards
 *	against a man in the middle while pairing; Secure Connections does
 *	against one who only listens.
 *
 *	A device paired is kept like any other (knl_btx_keep), with the keys
 *	it gave. While one of those is not connected, this end listens for it
 *	(a passive scan) and connects when it advertises, resolving a private
 *	address with the device's identity key. The services are found again
 *	on each connection: nothing of them is kept, so a device whose firmware
 *	changed cannot be read wrongly.
 *
 *	One ATT request is outstanding at a time, the answer to one sending
 *	the next, as the protocol wants.
 *
 *	A device with no HID service but the services of LE Audio, on a
 *	controller that can be the central of CISes, is not let go: its ATT is
 *	then bt_bap.c's. A controller with extended advertising is asked to
 *	scan and connect by the extended commands, whose reports are taken as
 *	the legacy ones are.
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
#include "bt_crypto.h"

IMPORT INT  knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );

/* LE events */
#define LE_CONN_DONE		0x01
#define LE_ADV_REPORT		0x02
#define LE_UPDATE_DONE		0x03
#define LE_EXT_ADV_REPORT	0x0d
#define LE_CIS_DONE		0x19

/* What advertises */
#define HCI_REMOTE_USER_ENDED	0x13	/* the reason a link is let go for */
#define ADV_IND			0x00
#define ADV_DIRECT		0x01
#define ADV_SCAN_IND		0x02
#define ADV_NONCONN		0x03
#define ADV_SCAN_RSP		0x04
#define AD_UUID16_MORE		0x02
#define AD_UUID16_ALL		0x03
#define AD_NAME_SHORT		0x08
#define AD_NAME_FULL		0x09
#define AD_APPEARANCE		0x19
#define APPEAR_HID_FIRST	0x03c0
#define APPEAR_HID_LAST		0x03cf
#define CONN_TMO_MS		10000

/* SMP */
#define SMP_PAIR_REQ		0x01
#define SMP_PAIR_RSP		0x02
#define SMP_CONFIRM		0x03
#define SMP_RANDOM		0x04
#define SMP_FAILED		0x05
#define SMP_ENC_INFO		0x06
#define SMP_CENTRAL_ID		0x07
#define SMP_ID_INFO		0x08
#define SMP_ID_ADDR		0x09
#define SMP_SEC_REQ		0x0b
#define SMP_PUBKEY		0x0c
#define SMP_DHKEY_CHECK		0x0d

#define SMP_IO_NONE		0x03
#define SMP_AUTH_BOND		0x01
#define SMP_AUTH_SC		0x08
#define SMP_DIST_ENC		0x01
#define SMP_DIST_ID		0x02

#define SMP_ERR_CONFIRM		0x04
#define SMP_ERR_UNSPEC		0x08
#define SMP_ERR_KEY_SIZE	0x06
#define SMP_ERR_DHKEY		0x0b
#define SMP_ERR_PARAM		0x0a
#define SMP_KEY_MIN		7
#define SMP_TMO_MS		30000

enum {
	S_IDLE, S_RSP, S_LEG_CONFIRM, S_LEG_RANDOM, S_SC_PUB, S_SC_CONFIRM, S_SC_RANDOM, S_SC_CHECK,
	S_ENC, S_DIST, S_DONE
};

/* ATT */
#define ATT_ERROR		0x01
#define ATT_MTU_REQ		0x02
#define ATT_MTU_RSP		0x03
#define ATT_FIND_REQ		0x04
#define ATT_FIND_RSP		0x05
#define ATT_TYPE_REQ		0x08
#define ATT_TYPE_RSP		0x09
#define ATT_READ_REQ		0x0a
#define ATT_READ_RSP		0x0b
#define ATT_BLOB_REQ		0x0c
#define ATT_BLOB_RSP		0x0d
#define ATT_GROUP_REQ		0x10
#define ATT_GROUP_RSP		0x11
#define ATT_WRITE_REQ		0x12
#define ATT_WRITE_RSP		0x13
#define ATT_WRITE_CMD		0x52
#define ATT_NOTIFY		0x1b
#define ATT_INDICATE		0x1d
#define ATT_CONFIRM		0x1e

#define ATT_E_NOT_LONG		0x0b
#define ATT_E_NOT_FOUND		0x0a
#define ATT_E_NOT_SUPPORTED	0x06
#define ATT_MTU_DEFAULT		23
#define ATT_MTU_ASK		185
#define ATT_TMO_MS		30000

#define UUID_PRIMARY		0x2800
#define UUID_CHAR		0x2803
#define UUID_CCCD		0x2902
#define UUID_REPORT_REF		0x2908
#define UUID_HID_SERVICE	0x1812
#define UUID_REPORT_MAP		0x2a4b
#define UUID_REPORT		0x2a4d
#define UUID_PROTO_MODE		0x2a4e

#define REPORT_INPUT		1
#define NREPORT			12

enum {
	A_NONE, A_MTU, A_SVC, A_CHR, A_DESC, A_REF, A_MAP, A_CCC, A_READY,
	A_LEA				/* an LE Audio device: its ATT is bt_bap.c's */
};

/* L2CAP on LE */
#define LS_REJECT		0x01
#define LS_UPDATE_REQ		0x12
#define LS_UPDATE_RSP		0x13

typedef struct {
	UH	val;			/* its value's handle */
	UH	last;			/* the last handle that can be a descriptor of it */
	UH	cccd, ref;
	UB	id, type;
} LEREP;

typedef struct {
	/* pairing */
	UB	st;
	BOOL	sc;
	UW	due;			/* when it gives up */
	UB	want;			/* the keys it is still to give: SMP_DIST_* */
	UB	preq[7], pres[7];
	UB	ia[6], ra[6];
	UB	iat, rat;
	UB	mrand[16], sconf[16], stk[16];
	UB	priv[32], pub[64], ppub[64], dh[32];
	UB	na[16], nb[16], cb[16], eb[16], mackey[16], ltk[16];
	BOOL	got_cb;
	/* the services */
	UB	at;
	UW	at_due;
	UH	mtu;
	UH	at_next;
	UH	svc_first, svc_last;
	UH	map, mode;
	INT	nrep, cur;
	LEREP	rep[NREPORT];
	BOOL	map_big;
} LES;

/* What f6 takes of a pairing request or answer: the capability lowest, out of band, the requirements */
LOCAL void iocap_of( CONST UB *pdu, UB *t )
{
	t[0] = pdu[1];
	t[1] = pdu[2];
	t[2] = pdu[3];
}

LOCAL void smp_send( BTC *c, PEER *p, CONST UB *b, INT n )
{
	knl_btx_l2_send(c, p, CID_SMP, b, n);
}

LOCAL void le_drop( BTC *c, PEER *p, UB reason )
{
	UB	r[3];

	r[0] = (UB)p->handle;
	r[1] = (UB)( p->handle >> 8 );
	r[2] = reason;
	knl_btx_cmd(c, C_DISCONNECT, r, 3);
}

LOCAL void smp_fail( BTC *c, PEER *p, LES *s, UB why )
{
	UB	b[2];

	b[0] = SMP_FAILED;
	b[1] = why;
	smp_send(c, p, b, 2);
	tm_printf((UB *)"bt: \"%s\": LE pairing given up, reason %02x\n", p->name, why);
	s->st = S_IDLE;
	le_drop(c, p, 0x05);
}

LOCAL void le_start_enc( BTC *c, PEER *p, CONST UB *key, CONST UB *rnd, UH ediv )
{
	UB	r[28];

	r[0] = (UB)p->handle;
	r[1] = (UB)( p->handle >> 8 );
	knl_memcpy(r + 2, rnd, 8);
	r[10] = (UB)ediv;
	r[11] = (UB)( ediv >> 8 );
	knl_memcpy(r + 12, key, 16);
	knl_btx_cmd(c, C_LE_START_ENC, r, 28);
}

LOCAL void gatt_start( BTC *c, PEER *p );

LOCAL void smp_start( BTC *c, PEER *p, LES *s )
{
	s->preq[0] = SMP_PAIR_REQ;
	s->preq[1] = SMP_IO_NONE;
	s->preq[2] = 0;				/* no data out of band */
	s->preq[3] = SMP_AUTH_BOND | SMP_AUTH_SC;
	s->preq[4] = 16;
	s->preq[5] = 0;				/* this end's address is public and it gives no keys */
	s->preq[6] = SMP_DIST_ENC | SMP_DIST_ID;
	s->st = S_RSP;
	s->due = knl_btx_now() + SMP_TMO_MS;
	smp_send(c, p, s->preq, 7);
}

/* The older pairing: this end's confirm of a random number of its own, with a TK of zero */
LOCAL void legacy_confirm( BTC *c, PEER *p, LES *s )
{
	static CONST UB	tk[16] = { 0 };
	UB		b[17];

	(void)ts_get_random(s->mrand, 16);
	b[0] = SMP_CONFIRM;
	knl_bts_c1(tk, s->mrand, s->preq, s->pres, s->iat, s->ia, s->rat, s->ra, b + 1);
	s->st = S_LEG_CONFIRM;
	smp_send(c, p, b, 17);
}

LOCAL void sc_pubkey( BTC *c, PEER *p, LES *s )
{
	UB	b[65];

	if ( knl_bts_p256_key(s->priv, s->pub, TRUE) < E_OK ) {
		smp_fail(c, p, s, SMP_ERR_UNSPEC);
		return;
	}
	b[0] = SMP_PUBKEY;
	knl_memcpy(b + 1, s->pub, 64);
	s->st = S_SC_PUB;
	smp_send(c, p, b, 65);
}

/* Secure Connections: the nonces are seen to fit the confirm, then the keys and the checks */
LOCAL void sc_checks( BTC *c, PEER *p, LES *s )
{
	static CONST UB	zero[16] = { 0 };
	UB		a[7], b[7], io[3], m[17], want[16];

	knl_bts_f4(s->ppub, s->pub, s->nb, 0, want);
	if ( !same_bt_bytes(want, s->cb, 16) ) {
		smp_fail(c, p, s, SMP_ERR_CONFIRM);
		return;
	}
	knl_memcpy(a, s->ia, 6);
	a[6] = s->iat;
	knl_memcpy(b, s->ra, 6);
	b[6] = s->rat;
	knl_bts_f5(s->dh, s->na, s->nb, a, b, s->mackey, s->ltk);
	iocap_of(s->preq, io);
	m[0] = SMP_DHKEY_CHECK;
	knl_bts_f6(s->mackey, s->na, s->nb, zero, io, a, b, m + 1);
	s->st = S_SC_CHECK;
	smp_send(c, p, m, 17);
}

/* The bond is whole: what it gave kept, and on to its services */
LOCAL void smp_done( BTC *c, PEER *p, LES *s )
{
	ER	er;

	s->st = S_DONE;
	p->have_key = TRUE;
	er = knl_btx_keep(p);
	tm_printf((UB *)"bt: \"%s\": LE pairing done, %s, key of %d bytes; kept (%d)\n", p->name,
		  p->le_sc ? "Secure Connections" : "the older pairing", p->key_sz, (INT)er);
	gatt_start(c, p);
}

LOCAL void smp_in( BTC *c, PEER *p, LES *s, CONST UB *f, INT n )
{
	UB	b[33];
	INT	i;

	if ( n < 1 ) return;
	switch ( f[0] ) {
	case SMP_SEC_REQ:
		/* the device asks: what it was already given, or a pairing of its asking */
		if ( s->st == S_IDLE && !p->have_key ) smp_start(c, p, s);
		break;
	case SMP_FAILED:
		tm_printf((UB *)"bt: \"%s\": the device gave up the pairing, reason %02x\n", p->name, ( n > 1 ) ? f[1] : 0);
		s->st = S_IDLE;
		break;
	case SMP_PAIR_RSP:
		if ( s->st != S_RSP || n < 7 ) break;
		knl_memcpy(s->pres, f, 7);
		p->key_sz = ( f[4] < s->preq[4] ) ? f[4] : s->preq[4];
		if ( p->key_sz < SMP_KEY_MIN ) {
			smp_fail(c, p, s, SMP_ERR_KEY_SIZE);
			break;
		}
		s->sc = (BOOL)( ( f[3] & SMP_AUTH_SC ) != 0 );
		p->le_sc = s->sc;
		/* in Secure Connections the key is not given but agreed */
		s->want = (UB)( f[6] & ( s->sc ? SMP_DIST_ID : ( SMP_DIST_ENC | SMP_DIST_ID ) ) );
		tm_printf((UB *)"bt: \"%s\": it can do %s, input and output %d, requirements %02x\n", p->name,
			  s->sc ? "Secure Connections" : "the older pairing", f[1], f[3]);
		if ( s->sc ) sc_pubkey(c, p, s);
		else legacy_confirm(c, p, s);
		break;
	case SMP_CONFIRM:
		if ( n < 17 ) break;
		if ( s->st == S_LEG_CONFIRM ) {
			knl_memcpy(s->sconf, f + 1, 16);
			b[0] = SMP_RANDOM;
			knl_memcpy(b + 1, s->mrand, 16);
			s->st = S_LEG_RANDOM;
			smp_send(c, p, b, 17);
		} else if ( s->st == S_SC_PUB || s->st == S_SC_CONFIRM ) {
			knl_memcpy(s->cb, f + 1, 16);
			s->got_cb = TRUE;
			if ( s->st == S_SC_CONFIRM ) {
				(void)ts_get_random(s->na, 16);
				b[0] = SMP_RANDOM;
				knl_memcpy(b + 1, s->na, 16);
				s->st = S_SC_RANDOM;
				smp_send(c, p, b, 17);
			}
		}
		break;
	case SMP_RANDOM:
		if ( n < 17 ) break;
		if ( s->st == S_LEG_RANDOM ) {
			static CONST UB	tk[16] = { 0 };
			UB		want[16];
			static CONST UB	zero8[8] = { 0 };

			knl_bts_c1(tk, f + 1, s->preq, s->pres, s->iat, s->ia, s->rat, s->ra, want);
			if ( !same_bt_bytes(want, s->sconf, 16) ) {
				smp_fail(c, p, s, SMP_ERR_CONFIRM);
				break;
			}
			knl_bts_s1(tk, f + 1, s->mrand, s->stk);
			/* a shorter key than 16 bytes is the same with its upper bytes zero */
			for ( i = p->key_sz; i < 16; i++ ) s->stk[i] = 0;
			s->st = S_ENC;
			le_start_enc(c, p, s->stk, zero8, 0);
		} else if ( s->st == S_SC_RANDOM ) {
			knl_memcpy(s->nb, f + 1, 16);
			sc_checks(c, p, s);
		}
		break;
	case SMP_PUBKEY:
		if ( n < 65 || s->st != S_SC_PUB ) break;
		knl_memcpy(s->ppub, f + 1, 64);
		/* its X the same as this end's is this end's own key sent back */
		if ( same_bt_bytes(s->ppub, s->pub, 32) ) {
			smp_fail(c, p, s, SMP_ERR_PARAM);
			break;
		}
		if ( knl_bts_p256_dh(s->priv, s->ppub, s->dh) < E_OK ) {
			tm_printf((UB *)"bt: \"%s\": a public key not on the curve\n", p->name);
			smp_fail(c, p, s, SMP_ERR_PARAM);
			break;
		}
		s->st = S_SC_CONFIRM;
		if ( s->got_cb ) {
			(void)ts_get_random(s->na, 16);
			b[0] = SMP_RANDOM;
			knl_memcpy(b + 1, s->na, 16);
			s->st = S_SC_RANDOM;
			smp_send(c, p, b, 17);
		}
		break;
	case SMP_DHKEY_CHECK: {
		static CONST UB	zero[16] = { 0 };
		static CONST UB	zero8[8] = { 0 };
		UB		a[7], rb[7], io[3], want[16];

		if ( n < 17 || s->st != S_SC_CHECK ) break;
		knl_memcpy(a, s->ia, 6);
		a[6] = s->iat;
		knl_memcpy(rb, s->ra, 6);
		rb[6] = s->rat;
		iocap_of(s->pres, io);
		knl_bts_f6(s->mackey, s->nb, s->na, zero, io, rb, a, want);
		if ( !same_bt_bytes(want, f + 1, 16) ) {
			smp_fail(c, p, s, SMP_ERR_DHKEY);
			break;
		}
		knl_memcpy(p->ltk, s->ltk, 16);
		knl_memset(p->rnd, 0, 8);
		p->ediv = 0;
		for ( i = p->key_sz; i < 16; i++ ) p->ltk[i] = 0;
		s->st = S_ENC;
		le_start_enc(c, p, p->ltk, zero8, 0);
		break;
	}
	case SMP_ENC_INFO:
		if ( n >= 17 && s->st == S_DIST ) knl_memcpy(p->ltk, f + 1, 16);
		break;
	case SMP_CENTRAL_ID:
		if ( n >= 11 && s->st == S_DIST ) {
			p->ediv = (UH)( f[1] | ( f[2] << 8 ) );
			knl_memcpy(p->rnd, f + 3, 8);
			s->want &= (UB)~SMP_DIST_ENC;
			if ( s->want == 0 ) smp_done(c, p, s);
		}
		break;
	case SMP_ID_INFO:
		if ( n >= 17 && s->st == S_DIST ) {
			knl_memcpy(p->irk, f + 1, 16);
			p->has_irk = TRUE;
		}
		break;
	case SMP_ID_ADDR:
		if ( n >= 8 && s->st == S_DIST ) {
			static CONST UB	none[6] = { 0 };

			/* from now on it is known by this address; an all zero one is none */
			if ( !same_bt_bytes(f + 2, none, 6) ) {
				p->atype = f[1];
				knl_memcpy(p->addr, f + 2, 6);
			}
			s->want &= (UB)~SMP_DIST_ID;
			if ( s->want == 0 ) smp_done(c, p, s);
		}
		break;
	default:
		break;
	}
}

/* The link is encrypted, or was refused */
EXPORT void knl_btl_enc_change( BTC *c, PEER *p, INT status, INT on )
{
	LES	*s = (LES *)p->les;

	if ( s == NULL ) return;
	if ( status != 0 || on == 0 ) {
		tm_printf((UB *)"bt: \"%s\": LE encryption refused (%02x); pair it again if its keys were lost\n", p->name, status);
		le_drop(c, p, 0x05);
		return;
	}
	if ( s->st == S_ENC ) {
		if ( s->want == 0 ) {
			smp_done(c, p, s);
		} else {
			s->st = S_DIST;
			s->due = knl_btx_now() + SMP_TMO_MS;
		}
	} else if ( s->st == S_IDLE ) {
		gatt_start(c, p);		/* a kept device, by the keys it gave */
	}
}

/* ---------------------------------------------------------------- GATT */

LOCAL void att_send( BTC *c, PEER *p, LES *s, CONST UB *b, INT n )
{
	s->at_due = knl_btx_now() + ATT_TMO_MS;
	knl_btx_l2_send(c, p, CID_ATT, b, n);
}

LOCAL void ask_range( BTC *c, PEER *p, LES *s, UB op, UH first, UH last, UH uuid )
{
	UB	b[7];
	INT	n = 5;

	b[0] = op;
	b[1] = (UB)first;
	b[2] = (UB)( first >> 8 );
	b[3] = (UB)last;
	b[4] = (UB)( last >> 8 );
	if ( op != ATT_FIND_REQ ) {
		b[5] = (UB)uuid;
		b[6] = (UB)( uuid >> 8 );
		n = 7;
	}
	att_send(c, p, s, b, n);
}

LOCAL void ask_handle( BTC *c, PEER *p, LES *s, UB op, UH h, CONST UB *v, INT n )
{
	UB	b[5];

	b[0] = op;
	b[1] = (UB)h;
	b[2] = (UB)( h >> 8 );
	if ( n > 0 ) knl_memcpy(b + 3, v, n);
	att_send(c, p, s, b, n + 3);
}

/* The next report's descriptors, the references, the map, the notifications, each in turn */
LOCAL void next_desc( BTC *c, PEER *p, LES *s );
LOCAL void next_ref( BTC *c, PEER *p, LES *s );
LOCAL void next_ccc( BTC *c, PEER *p, LES *s );

LOCAL void gatt_ready( BTC *c, PEER *p, LES *s )
{
	s->at = A_READY;
	s->at_due = 0;
	knl_btx_rd_layout(p->rd, p->rdlen, &p->lay);
	p->report_proto = TRUE;
	if ( !p->hid_on ) {
		p->hid_on = TRUE;
		knl_usb_lock();
		if ( p->lay.has_kbd ) p->hd = knl_hid_ext_attach(TRUE, NULL, 0, (CONST UB *)"btkbd");
		if ( p->lay.has_ptr || p->lay.has_cc ) {
			p->hd2 = knl_hid_ext_attach(FALSE, p->rd, p->rdlen, (CONST UB *)"bthid");
		}
		knl_usb_unlock();
	}
	tm_printf((UB *)"bt: \"%s\": HID over GATT: %d reports, a map of %d bytes: keyboard slot %d, ID %d; other slot %d\n",
		  p->name, s->nrep, p->rdlen, p->hd, p->lay.rid, p->hd2);
}

LOCAL void next_ccc( BTC *c, PEER *p, LES *s )
{
	static CONST UB	on[2] = { 0x01, 0x00 };

	s->at = A_CCC;
	while ( s->cur < s->nrep ) {
		LEREP	*r = &s->rep[s->cur++];

		if ( r->type == REPORT_INPUT && r->cccd != 0 ) {
			ask_handle(c, p, s, ATT_WRITE_REQ, r->cccd, on, 2);
			return;
		}
	}
	gatt_ready(c, p, s);
}

LOCAL void read_map( BTC *c, PEER *p, LES *s )
{
	s->at = A_MAP;
	p->rdlen = 0;
	if ( s->map == 0 ) {
		tm_printf((UB *)"bt: \"%s\": its HID service has no report map\n", p->name);
		s->at = A_NONE;
		return;
	}
	ask_handle(c, p, s, ATT_READ_REQ, s->map, NULL, 0);
}

LOCAL void map_done( BTC *c, PEER *p, LES *s )
{
	/* the report protocol, the device's own default perhaps being the boot one */
	if ( s->mode != 0 ) {
		UB	b[4];

		b[0] = ATT_WRITE_CMD;
		b[1] = (UB)s->mode;
		b[2] = (UB)( s->mode >> 8 );
		b[3] = 0x01;
		knl_btx_l2_send(c, p, CID_ATT, b, 4);
	}
	s->cur = 0;
	next_ccc(c, p, s);
}

LOCAL void next_ref( BTC *c, PEER *p, LES *s )
{
	s->at = A_REF;
	while ( s->cur < s->nrep ) {
		if ( s->rep[s->cur].ref != 0 ) {
			ask_handle(c, p, s, ATT_READ_REQ, s->rep[s->cur].ref, NULL, 0);
			return;
		}
		s->cur++;
	}
	read_map(c, p, s);
}

LOCAL void next_desc( BTC *c, PEER *p, LES *s )
{
	s->at = A_DESC;
	while ( s->cur < s->nrep ) {
		LEREP	*r = &s->rep[s->cur];

		if ( s->at_next == 0 ) s->at_next = (UH)( r->val + 1 );
		if ( s->at_next <= r->last ) {
			ask_range(c, p, s, ATT_FIND_REQ, s->at_next, r->last, 0);
			return;
		}
		s->cur++;
		s->at_next = 0;
	}
	s->cur = 0;
	next_ref(c, p, s);
}

/* A characteristic of the HID service as it is listed */
LOCAL void note_char( LES *s, UH decl, UH val, UH uuid )
{
	if ( s->nrep > 0 && s->rep[s->nrep - 1].last == 0xffff ) s->rep[s->nrep - 1].last = (UH)( decl - 1 );
	if ( uuid == UUID_REPORT_MAP ) {
		s->map = val;
	} else if ( uuid == UUID_PROTO_MODE ) {
		s->mode = val;
	} else if ( uuid == UUID_REPORT && s->nrep < NREPORT ) {
		LEREP	*r = &s->rep[s->nrep++];

		knl_memset(r, 0, sizeof(*r));
		r->val = val;
		r->last = 0xffff;
		r->type = REPORT_INPUT;		/* a report with no reference is taken as input, with no ID */
	}
}

/* An answer of the server, which sends on the next request */
LOCAL void gatt_answer( BTC *c, PEER *p, LES *s, CONST UB *f, INT n )
{
	UB	op = f[0];
	INT	i, l;

	switch ( s->at ) {
	case A_MTU:
		if ( op == ATT_MTU_RSP && n >= 3 ) {
			UH	m = (UH)( f[1] | ( f[2] << 8 ) );

			s->mtu = ( m < ATT_MTU_DEFAULT ) ? ATT_MTU_DEFAULT : ( m > ATT_MTU_ASK ) ? ATT_MTU_ASK : m;
		}
		s->at = A_SVC;
		ask_range(c, p, s, ATT_GROUP_REQ, 1, 0xffff, UUID_PRIMARY);
		break;
	case A_SVC:
		if ( op == ATT_GROUP_RSP && n >= 2 && f[1] >= 6 ) {
			UH	last = 0;

			l = f[1];
			for ( i = 2; i + l <= n; i += l ) {
				last = (UH)( f[i + 2] | ( f[i + 3] << 8 ) );
				if ( l == 6 ) {
					knl_btbap_svc(c, p, (UH)( f[i + 4] | ( f[i + 5] << 8 ) ), (UH)( f[i] | ( f[i + 1] << 8 ) ), last);
				}
				if ( l == 6 && ( f[i + 4] | ( f[i + 5] << 8 ) ) == UUID_HID_SERVICE && s->svc_first == 0 ) {
					s->svc_first = (UH)( f[i] | ( f[i + 1] << 8 ) );
					s->svc_last = last;
				}
			}
			if ( last != 0xffff && last != 0 ) {
				ask_range(c, p, s, ATT_GROUP_REQ, (UH)( last + 1 ), 0xffff, UUID_PRIMARY);
				break;
			}
		}
		if ( s->svc_first == 0 && knl_btbap_start(c, p, s->mtu) ) {
			/* no input, but sound: LE Audio (bt_bap.c) */
			s->at = A_LEA;
			s->at_due = 0;
			break;
		}
		if ( s->svc_first == 0 ) {
			/* listed by what it advertised alone, and not for input after all: let go */
			tm_printf((UB *)"bt: \"%s\": no HID service: disconnected\n", p->name);
			s->at = A_NONE;
			s->at_due = 0;
			le_drop(c, p, HCI_REMOTE_USER_ENDED);
			break;
		}
		s->at = A_CHR;
		ask_range(c, p, s, ATT_TYPE_REQ, s->svc_first, s->svc_last, UUID_CHAR);
		break;
	case A_CHR:
		if ( op == ATT_TYPE_RSP && n >= 2 && f[1] >= 7 ) {
			UH	decl = 0;

			l = f[1];
			for ( i = 2; i + l <= n; i += l ) {
				decl = (UH)( f[i] | ( f[i + 1] << 8 ) );
				note_char(s, decl, (UH)( f[i + 3] | ( f[i + 4] << 8 ) ),
					  ( l == 7 ) ? (UH)( f[i + 5] | ( f[i + 6] << 8 ) ) : 0);
			}
			if ( decl != 0 && decl < s->svc_last ) {
				ask_range(c, p, s, ATT_TYPE_REQ, (UH)( decl + 1 ), s->svc_last, UUID_CHAR);
				break;
			}
		}
		if ( s->nrep > 0 && s->rep[s->nrep - 1].last == 0xffff ) s->rep[s->nrep - 1].last = s->svc_last;
		s->cur = 0;
		s->at_next = 0;
		next_desc(c, p, s);
		break;
	case A_DESC:
		if ( op == ATT_FIND_RSP && n >= 2 ) {
			LEREP	*r = &s->rep[s->cur];
			UH	h = 0;

			l = ( f[1] == 1 ) ? 4 : 18;
			for ( i = 2; i + l <= n; i += l ) {
				h = (UH)( f[i] | ( f[i + 1] << 8 ) );
				if ( l == 4 ) {
					UH	u = (UH)( f[i + 2] | ( f[i + 3] << 8 ) );

					if ( u == UUID_CCCD ) r->cccd = h;
					if ( u == UUID_REPORT_REF ) r->ref = h;
				}
			}
			if ( h != 0 && h < r->last ) {
				s->at_next = (UH)( h + 1 );
			} else {
				s->cur++;
				s->at_next = 0;
			}
		} else {
			s->cur++;
			s->at_next = 0;
		}
		next_desc(c, p, s);
		break;
	case A_REF:
		if ( op == ATT_READ_RSP && n >= 3 ) {
			s->rep[s->cur].id = f[1];
			s->rep[s->cur].type = f[2];
		}
		s->cur++;
		next_ref(c, p, s);
		break;
	case A_MAP:
		if ( op == ATT_READ_RSP || op == ATT_BLOB_RSP ) {
			INT	m = n - 1;

			if ( p->rdlen + m > HID_RD_MAX ) {
				tm_printf((UB *)"bt: \"%s\": a report map longer than %d bytes\n", p->name, HID_RD_MAX);
				map_done(c, p, s);
				break;
			}
			knl_memcpy(p->rd + p->rdlen, f + 1, m);
			p->rdlen += m;
			if ( m == s->mtu - 1 ) {
				UB	off[2];

				off[0] = (UB)p->rdlen;
				off[1] = (UB)( p->rdlen >> 8 );
				ask_handle(c, p, s, ATT_BLOB_REQ, s->map, off, 2);
				break;
			}
		} else if ( op == ATT_ERROR && n >= 5 && f[4] != ATT_E_NOT_LONG && p->rdlen == 0 ) {
			tm_printf((UB *)"bt: \"%s\": its report map not read, error %02x\n", p->name, f[4]);
			s->at = A_NONE;
			s->at_due = 0;
			break;
		}
		map_done(c, p, s);
		break;
	case A_CCC:
		if ( op == ATT_ERROR && n >= 5 ) {
			tm_printf((UB *)"bt: \"%s\": a report's notifications refused, error %02x\n", p->name, f[4]);
		}
		next_ccc(c, p, s);
		break;
	default:
		break;
	}
}

/* A report as it comes, handed on as HID has it: with its ID in front if it has one */
LOCAL void report_note( PEER *p, LES *s, CONST UB *f, INT n )
{
	UH	h;
	UB	r[ATT_MTU_ASK + 1];
	INT	i, m;

	if ( n < 3 || s->at != A_READY ) return;
	h = (UH)( f[1] | ( f[2] << 8 ) );
	for ( i = 0; i < s->nrep; i++ ) {
		LEREP	*rp = &s->rep[i];

		if ( rp->val != h ) continue;
		m = n - 3;
		if ( m > ATT_MTU_ASK ) return;
		if ( rp->id != 0 ) {
			r[0] = rp->id;
			knl_memcpy(r + 1, f + 3, m);
			knl_btx_report_in(p, r, m + 1);
		} else {
			knl_btx_report_in(p, f + 3, m);
		}
		return;
	}
}

LOCAL void att_in( BTC *c, PEER *p, LES *s, CONST UB *f, INT n )
{
	UB	b[5];

	if ( n < 1 ) return;
	if ( s->at == A_LEA && knl_btbap_att_in(c, p, f, n) ) return;
	switch ( f[0] ) {
	case ATT_NOTIFY:
		report_note(p, s, f, n);
		return;
	case ATT_INDICATE:
		b[0] = ATT_CONFIRM;
		knl_btx_l2_send(c, p, CID_ATT, b, 1);
		return;
	case ATT_MTU_REQ:
		/* the device asks too: its answer stands for both */
		b[0] = ATT_MTU_RSP;
		b[1] = (UB)ATT_MTU_ASK;
		b[2] = (UB)( ATT_MTU_ASK >> 8 );
		knl_btx_l2_send(c, p, CID_ATT, b, 3);
		return;
	case ATT_ERROR: case ATT_MTU_RSP: case ATT_FIND_RSP: case ATT_TYPE_RSP: case ATT_READ_RSP:
	case ATT_BLOB_RSP: case ATT_GROUP_RSP: case ATT_WRITE_RSP:
		gatt_answer(c, p, s, f, n);
		return;
	default:
		break;
	}
	/* a request to this end, which serves nothing: odd opcodes are answers, handled above */
	if ( ( f[0] & 0x40 ) != 0 || ( f[0] & 1 ) != 0 ) return;
	b[0] = ATT_ERROR;
	b[1] = f[0];
	b[2] = ( n >= 3 ) ? f[1] : 0;
	b[3] = ( n >= 3 ) ? f[2] : 0;
	b[4] = ( f[0] == ATT_GROUP_REQ || f[0] == ATT_TYPE_REQ || f[0] == ATT_FIND_REQ ) ? ATT_E_NOT_FOUND : ATT_E_NOT_SUPPORTED;
	knl_btx_l2_send(c, p, CID_ATT, b, 5);
}

LOCAL void gatt_start( BTC *c, PEER *p )
{
	LES	*s = (LES *)p->les;
	UB	b[3];

	if ( s == NULL || s->at != A_NONE ) return;
	knl_btbap_reset(c, p);
	s->mtu = ATT_MTU_DEFAULT;
	s->at = A_MTU;
	s->nrep = 0;
	s->svc_first = 0;
	s->map = s->mode = 0;
	b[0] = ATT_MTU_REQ;
	b[1] = (UB)ATT_MTU_ASK;
	b[2] = (UB)( ATT_MTU_ASK >> 8 );
	att_send(c, p, s, b, 3);
}

/* ---------------------------------------------------------------- L2CAP on LE */

LOCAL void le_sig_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	UB	r[6];

	if ( n < 4 ) return;
	if ( f[0] == LS_UPDATE_REQ && n >= 12 ) {
		UH	lo = (UH)( f[4] | ( f[5] << 8 ) ), hi = (UH)( f[6] | ( f[7] << 8 ) );
		UH	lat = (UH)( f[8] | ( f[9] << 8 ) ), to = (UH)( f[10] | ( f[11] << 8 ) );
		BOOL	ok = (BOOL)( lo >= 6 && lo <= hi && hi <= 3200 && lat <= 499 && to >= 10 && to <= 3200 );

		r[0] = LS_UPDATE_RSP;
		r[1] = f[1];
		r[2] = 2;
		r[3] = 0;
		r[4] = ok ? 0 : 1;
		r[5] = 0;
		knl_btx_l2_send(c, p, CID_LE_SIG, r, 6);
		if ( ok ) {
			UB	u[14];

			u[0] = (UB)p->handle;
			u[1] = (UB)( p->handle >> 8 );
			knl_memcpy(u + 2, f + 4, 8);
			knl_memset(u + 10, 0, 4);
			knl_btx_cmd(c, C_LE_CONN_UPDATE, u, 14);
		}
	} else if ( f[0] != LS_UPDATE_RSP && f[0] != LS_REJECT ) {
		r[0] = LS_REJECT;
		r[1] = f[1];
		r[2] = 2;
		r[3] = 0;
		r[4] = 0;		/* not understood */
		r[5] = 0;
		knl_btx_l2_send(c, p, CID_LE_SIG, r, 6);
	}
}

EXPORT void knl_btl_l2_in( BTC *c, PEER *p, INT cid, CONST UB *f, INT len )
{
	LES	*s = (LES *)p->les;

	if ( s == NULL ) return;
	if ( cid == CID_SMP ) smp_in(c, p, s, f, len);
	else if ( cid == CID_ATT ) att_in(c, p, s, f, len);
	else le_sig_in(c, p, f, len);
}

/* ---------------------------------------------------------------- finding and connecting */

/*
 * A controller with extended advertising is asked by the extended
 * commands: the devices of LE Audio advertise so, and a controller that
 * has been given one of them refuses the legacy ones until it is reset.
 */
LOCAL void scan_enable( BTC *c, BOOL on )
{
	UB	r[6];

	knl_memset(r, 0, sizeof(r));
	r[0] = on ? 1 : 0;
	r[1] = on ? 1 : 0;		/* each one once; for as long as it is on */
	knl_btx_cmd(c, c->has_ext_adv ? C_LE_EXT_SCAN_ENABLE : C_LE_SCAN_ENABLE, r, c->has_ext_adv ? 6 : 2);
}

LOCAL void scan_set( BTC *c, BOOL on, BOOL active )
{
	UB	r[8];

	if ( on && ( !c->le_scan_on || c->le_scan_active != active ) ) {
		if ( c->le_scan_on ) scan_enable(c, FALSE);
		if ( c->has_ext_adv ) {
			r[0] = 0;		/* this end's public address */
			r[1] = 0;		/* all that advertise */
			r[2] = 0x01;		/* on the 1M PHY */
			r[3] = active ? 1 : 0;
			r[4] = 0x60;		/* every 60 ms, for 30 */
			r[5] = 0;
			r[6] = 0x30;
			r[7] = 0;
			knl_btx_cmd(c, C_LE_EXT_SCAN_PARAM, r, 8);
			scan_enable(c, TRUE);
			c->le_scan_on = TRUE;
			c->le_scan_active = active;
			return;
		}
		r[0] = active ? 1 : 0;
		r[1] = 0x60;		/* every 60 ms, for 30 */
		r[2] = 0;
		r[3] = 0x30;
		r[4] = 0;
		r[5] = 0;		/* this end's public address */
		r[6] = 0;		/* all that advertise */
		knl_btx_cmd(c, C_LE_SCAN_PARAM, r, 7);
		r[0] = 1;
		r[1] = 1;		/* each one once */
		knl_btx_cmd(c, C_LE_SCAN_ENABLE, r, 2);
		c->le_scan_on = TRUE;
		c->le_scan_active = active;
	} else if ( !on && c->le_scan_on ) {
		scan_enable(c, FALSE);
		c->le_scan_on = FALSE;
	}
}

EXPORT void knl_btl_scan_start( BTC *c )
{
	if ( !c->has_le ) return;
	c->le_other = 0;
	c->le_user_scan = TRUE;
	scan_set(c, TRUE, TRUE);
}

EXPORT void knl_btl_scan_end( BTC *c )
{
	if ( !c->has_le || !c->le_user_scan ) return;
	c->le_user_scan = FALSE;
	scan_set(c, FALSE, FALSE);
	if ( c->le_other > 0 ) {
		tm_printf((UB *)"bt: LE scan: %d that take a connection and do not say they are input devices, listed\n", c->le_other);
	}
}

EXPORT void knl_btl_connect( BTC *c, PEER *p )
{
	UB	r[25];

	if ( c->le_conn != NULL || !c->has_le ) return;
	scan_set(c, FALSE, FALSE);
	r[0] = 0x60;			/* scan every 60 ms, for 30 */
	r[1] = 0;
	r[2] = 0x30;
	r[3] = 0;
	r[4] = 0;			/* this address, not a list */
	r[5] = p->cur_type;
	knl_memcpy(r + 6, p->cur_addr, 6);
	r[12] = 0;			/* this end's public address */
	r[13] = 24;			/* events every 30 to 50 ms, for input that does not wait */
	r[14] = 0;
	r[15] = 40;
	r[16] = 0;
	r[17] = r[18] = 0;		/* the device need not skip */
	r[19] = 200;			/* lost after 2 s */
	r[20] = 0;
	r[21] = r[22] = r[23] = r[24] = 0;
	c->le_conn = p;
	c->le_conn_due = knl_btx_now() + CONN_TMO_MS;
	c->le_conn_cancelled = FALSE;
	if ( c->has_ext_adv ) {
		UB	x[26];

		/* the same on the 1M PHY alone; the event that comes is the same as well */
		x[0] = 0;
		x[1] = 0;
		x[2] = p->cur_type;
		knl_memcpy(x + 3, p->cur_addr, 6);
		x[9] = 0x01;
		knl_memcpy(x + 10, r, 4);
		knl_memcpy(x + 14, r + 13, 12);
		knl_btx_cmd(c, C_LE_EXT_CREATE_CONN, x, 26);
		return;
	}
	knl_btx_cmd(c, C_LE_CREATE_CONN, r, 25);
}

/* What the advertising data says: its name, and whether it is for input */
LOCAL BOOL ad_look( CONST UB *ad, INT n, CONST UB **p_name, INT *p_nlen )
{
	BOOL	input = FALSE;
	INT	at = 0;

	while ( at < n && ad[at] != 0 && at + 1 + ad[at] <= n ) {
		INT	l = ad[at] - 1, t = ad[at + 1];
		CONST UB *v = ad + at + 2;

		if ( t == AD_NAME_FULL || ( t == AD_NAME_SHORT && *p_name == NULL ) ) {
			*p_name = v;
			*p_nlen = l;
		} else if ( t == AD_UUID16_MORE || t == AD_UUID16_ALL ) {
			INT	i;

			for ( i = 0; i + 2 <= l; i += 2 ) {
				if ( ( v[i] | ( v[i + 1] << 8 ) ) == UUID_HID_SERVICE ) input = TRUE;
			}
		} else if ( t == AD_APPEARANCE && l >= 2 ) {
			INT	a = v[0] | ( v[1] << 8 );

			if ( a >= APPEAR_HID_FIRST && a <= APPEAR_HID_LAST ) input = TRUE;
		}
		at += 1 + ad[at];
	}
	return input;
}

/* The kept device this address is: by itself, or a private one its identity key gives */
LOCAL PEER *le_kept_of( BTC *c, CONST UB *a, INT type )
{
	INT	i;

	for ( i = 0; i < NPEER; i++ ) {
		PEER	*p = &c->peer[i];

		if ( !p->used || !p->le || !p->have_key ) continue;
		if ( same_bt_bytes(p->addr, a, 6) ) return p;
		if ( type == 1 && p->has_irk && ( a[5] & 0xc0 ) == 0x40 ) {
			UB	h[3];

			knl_bts_ah(p->irk, a + 3, h);
			if ( same_bt_bytes(h, a, 3) ) return p;
		}
	}
	return NULL;
}

/*
 * A device seen in a scan the person asked for and not listed, said with
 * what it advertised, for a device that does not come up in the list:
 * the kind of report (0 connectable, 2 scannable, 3 not connectable, 4
 * a scan response), its name, its appearance and its 16 bit services.
 */
LOCAL void ad_say( CONST UB *a, INT type, INT evt, CONST UB *ad, INT n )
{
	char	name[33], svc[64], raw[2 * 31 + 1];
	INT	at = 0, appear = -1, sn = 0, nl = 0, i;

	name[0] = svc[0] = '\0';
	while ( at < n && ad[at] != 0 && at + 1 + ad[at] <= n ) {
		INT	l = ad[at] - 1, t = ad[at + 1], i;
		CONST UB *v = ad + at + 2;

		if ( ( t == AD_NAME_FULL || t == AD_NAME_SHORT ) && nl == 0 ) {
			nl = ( l < (INT)sizeof(name) - 1 ) ? l : (INT)sizeof(name) - 1;
			knl_memcpy(name, v, nl);
			name[nl] = '\0';
		} else if ( t == AD_APPEARANCE && l >= 2 ) {
			appear = v[0] | ( v[1] << 8 );
		} else if ( t == AD_UUID16_MORE || t == AD_UUID16_ALL ) {
			for ( i = 0; i + 2 <= l && sn + 6 < (INT)sizeof(svc); i += 2 ) {
				sn += tm_sprintf((UB *)svc + sn, (UB *)" %04x", v[i] | ( v[i + 1] << 8 ));
			}
		}
		at += 1 + ad[at];
	}
	for ( i = 0; i < n && i < 31; i++ ) {
		(void)tm_sprintf((UB *)raw + 2 * i, (UB *)"%02x", ad[i]);
	}
	raw[2 * i] = '\0';
	tm_printf((UB *)"bt: LE scan: %02x:%02x:%02x:%02x:%02x:%02x (address type %d, report %d): name \"%s\", appearance %d, services%s; %s\n",
		  a[5], a[4], a[3], a[2], a[1], a[0], type, evt, name, appear, ( sn > 0 ) ? svc : " none", raw);
}

/* One device's report: a kept one to connect, one a scan lists, the other of a pair of earphones */
LOCAL void adv_one( BTC *c, INT evt, INT type, CONST UB *a, CONST UB *ad, INT n )
{
	PEER		*p = le_kept_of(c, a, type);
	CONST UB	*name = NULL;
	INT		nlen = 0;

	if ( p != NULL && !p->up && c->le_conn == NULL && !c->le_user_scan
	  && ( evt == ADV_IND || evt == ADV_DIRECT ) ) {
		knl_memcpy(p->cur_addr, a, 6);
		p->cur_type = (UB)type;
		tm_printf((UB *)"bt: \"%s\" is advertising: connecting\n", p->name);
		knl_btl_connect(c, p);
	} else if ( c->le_user_scan && p == NULL ) {
		BOOL		input = ad_look(ad, n, &name, &nlen);
		T_BTFOUND	*f = NULL;
		INT		i;

		for ( i = 0; i < c->nfound; i++ ) {
			if ( same_bt_bytes(c->found[i].addr, a, 6) ) f = &c->found[i];
		}
		/*
		 * Every device that takes a connection is listed, whatever it
		 * says of itself: many input devices put nothing for input in
		 * what they advertise, and the pairing finds out from the
		 * device's services. What one not marked for input advertised
		 * is said, to tell the devices apart.
		 */
		if ( !input && f == NULL ) {
			if ( evt == ADV_IND ) c->le_other++;
			ad_say(a, type, evt, ad, n);
		}
		if ( evt == ADV_IND && f == NULL ) {
			f = knl_btx_found_add(c, a, 0, NULL, 0);
		}
		if ( f != NULL ) {
			f->le = 1;
			f->atype = (UB)type;
			if ( name != NULL ) (void)knl_btx_found_add(c, a, 0, name, nlen);
		}
	} else if ( p == NULL && evt == ADV_IND && c->le_conn == NULL && !c->le_user_scan
		 && knl_btbap_rsi(c, ad, n) && ( p = knl_btx_peer_new(c, a) ) != NULL && !p->up ) {
		/* its set identifier resolves with the SIRK of one here: the other earphone, paired as it comes */
		(void)ad_look(ad, n, &name, &nlen);
		if ( name != NULL && p->name[0] == 0 ) {
			if ( nlen > (INT)sizeof(p->name) - 1 ) nlen = (INT)sizeof(p->name) - 1;
			knl_memcpy(p->name, name, nlen);
			p->name[nlen] = 0;
		}
		p->le = TRUE;
		p->atype = (UB)type;
		knl_memcpy(p->cur_addr, a, 6);
		p->cur_type = (UB)type;
		tm_printf((UB *)"bt: \"%s\": a member of a set of earphones here: connecting\n", p->name);
		knl_btl_connect(c, p);
	}
}

LOCAL void adv_report( BTC *c, CONST UB *d, INT len )
{
	INT	at = 1, k;

	for ( k = 0; k < d[0]; k++ ) {
		INT	n;

		if ( at + 8 > len ) return;
		n = d[at + 8];
		if ( at + 9 + n + 1 > len ) return;
		adv_one(c, d[at], d[at + 1], d + at + 2, d + at + 9, n);
		at += 9 + n + 1;
	}
}

/*
 * The extended reports, each as the legacy kind it comes to: its event
 * type's bits are connectable 0x01, scannable 0x02, directed 0x04, a
 * scan response 0x08, and a legacy PDU 0x10. Its address type may say
 * an identity the controller resolved (2, 3): the type of that address.
 * A report whose data was cut short is taken as far as it goes.
 */
LOCAL void ext_adv_report( BTC *c, CONST UB *d, INT len )
{
	INT	at = 1, k;

	for ( k = 0; k < d[0]; k++ ) {
		INT	et, type, n, evt;

		if ( at + 24 > len ) return;
		et = d[at] | ( d[at + 1] << 8 );
		type = d[at + 2];
		n = d[at + 23];
		if ( at + 24 + n > len ) return;
		evt = ( et & 0x08 ) ? ADV_SCAN_RSP : ( et & 0x04 ) ? ADV_DIRECT : ( et & 0x01 ) ? ADV_IND
		    : ( et & 0x02 ) ? ADV_SCAN_IND : ADV_NONCONN;
		if ( type != 0xff ) adv_one(c, evt, type & 1, d + at + 3, d + at + 24, n);
		at += 24 + n;
	}
}

LOCAL void le_conn_done( BTC *c, CONST UB *d, INT len )
{
	PEER	*p = c->le_conn;
	LES	*s;
	UH	h;
	INT	st;

	if ( len < 13 || p == NULL ) return;
	st = d[0];
	c->le_conn = NULL;
	h = (UH)( d[1] | ( d[2] << 8 ) );
	tm_printf((UB *)"bt: LE connection %d to \"%s\": status %02x, interval %d\n", h, p->name, st, d[11] | ( d[12] << 8 ));
	if ( st != 0 ) return;
	if ( p->les == NULL ) p->les = Kcalloc(1, sizeof(LES));
	s = (LES *)p->les;
	if ( s == NULL ) return;
	knl_memset(s, 0, sizeof(*s));
	p->handle = h;
	p->up = TRUE;
	p->hd = p->hd2 = 0;
	p->hid_on = FALSE;
	s->iat = 0;			/* this end's public address */
	knl_memcpy(s->ia, c->addr, 6);
	s->rat = d[4];
	knl_memcpy(s->ra, d + 5, 6);
	if ( p->have_key && p->key_sz > 0 ) {
		le_start_enc(c, p, p->ltk, p->rnd, p->ediv);
	} else {
		smp_start(c, p, s);
	}
}

EXPORT void knl_btl_event( BTC *c, CONST UB *d, INT len )
{
	if ( len < 1 ) return;
	switch ( d[0] ) {
	case LE_CONN_DONE:
		le_conn_done(c, d + 1, len - 1);
		break;
	case LE_ADV_REPORT:
		adv_report(c, d + 1, len - 1);
		break;
	case LE_EXT_ADV_REPORT:
		ext_adv_report(c, d + 1, len - 1);
		break;
	case LE_CIS_DONE:
		knl_btiso_cis_est(c, d + 1, len - 1);
		break;
	case LE_UPDATE_DONE:
		if ( len >= 4 ) tm_printf((UB *)"bt: LE connection %d: parameters changed, status %02x\n", d[2] | ( d[3] << 8 ), d[1]);
		break;
	default:
		break;
	}
}

/* The connection went */
EXPORT void knl_btl_down( BTC *c, PEER *p )
{
	knl_btbap_down(c, p);
	if ( p->les != NULL ) {
		knl_memset(p->les, 0, sizeof(LES));	/* the keys of a pairing not finished go */
		Kfree(p->les);
		p->les = NULL;
	}
}

/* Each round of the task: what has waited too long, and listening for the devices kept */
EXPORT void knl_btl_tick( BTC *c )
{
	UW	now = knl_btx_now();
	INT	i;
	BOOL	absent = FALSE;

	if ( !c->has_le || !c->ready ) return;
	if ( c->le_conn != NULL && !c->le_conn_cancelled && (W)( now - c->le_conn_due ) >= 0 ) {
		c->le_conn_cancelled = TRUE;
		knl_btx_cmd(c, C_LE_CONN_CANCEL, NULL, 0);
	}
	for ( i = 0; i < NPEER; i++ ) {
		PEER	*p = &c->peer[i];
		LES	*s = (LES *)p->les;

		if ( p->used && p->le && p->have_key && !p->up ) absent = TRUE;
		if ( s == NULL || !p->up ) continue;
		if ( s->st != S_IDLE && s->st != S_DONE && (W)( now - s->due ) >= 0 ) {
			tm_printf((UB *)"bt: \"%s\": LE pairing timed out at step %d\n", p->name, s->st);
			s->st = S_IDLE;
			le_drop(c, p, 0x13);
		}
		if ( s->at_due != 0 && (W)( now - s->at_due ) >= 0 ) {
			tm_printf((UB *)"bt: \"%s\": its services did not answer, step %d\n", p->name, s->at);
			s->at_due = 0;
			le_drop(c, p, 0x13);
		}
	}
	/* the other of a pair of earphones is listened for as well */
	if ( !c->le_user_scan && c->le_conn == NULL ) scan_set(c, (BOOL)( absent || knl_btbap_seek(c) ), FALSE);
	knl_btlea_tick(c);
}
