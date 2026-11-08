/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_bap.c
 *	The services of an LE Audio device, read and driven as their client
 *	(design 6.8): what its sink can take (PACS), its stream ends and
 *	their states (ASCS), the set of earphones it is one of (CSIS), and its
 *	volume (VCS). What is done with them is bt_lea.c's.
 *
 *	A device whose GATT services have no HID service but have those of
 *	LE Audio comes here from bt_le.c, which has found its services. Its
 *	characteristics are listed service by service, the descriptors that
 *	turn on their notifications found, the values read, and the
 *	notifications turned on, one ATT request at a time as bt_le.c does.
 *	Then the device is ready, and from then on the states of its ASEs come
 *	as notifications, with those of the ASE control point that say what an
 *	operation came to.
 *
 *	The operations on the control point go as writes without an answer,
 *	one ASE each, which needs an ATT MTU of 33 at the least; LE Audio
 *	devices take 64.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "btint.h"
#include "bt_crypto.h"
#include "bt_lea.h"

/* ATT */
#define ATT_ERROR		0x01
#define ATT_FIND_REQ		0x04
#define ATT_FIND_RSP		0x05
#define ATT_TYPE_REQ		0x08
#define ATT_TYPE_RSP		0x09
#define ATT_READ_REQ		0x0a
#define ATT_READ_RSP		0x0b
#define ATT_BLOB_REQ		0x0c
#define ATT_BLOB_RSP		0x0d
#define ATT_WRITE_REQ		0x12
#define ATT_WRITE_RSP		0x13
#define ATT_WRITE_CMD		0x52
#define ATT_NOTIFY		0x1b
#define ATT_E_NOT_LONG		0x0b

#define PROP_READ		0x02
#define PROP_NOTIFY		0x10

#define UUID_CHAR		0x2803
#define UUID_CCCD		0x2902

/* The services and their characteristics (Assigned Numbers 3.4 and 3.8) */
#define SVC_VCS			0x1844
#define SVC_CSIS		0x1846
#define SVC_ASCS		0x184e
#define SVC_PACS		0x1850
#define CH_VOL_STATE		0x2b7d
#define CH_VOL_CP		0x2b7e
#define CH_SIRK			0x2b84
#define CH_SET_SIZE		0x2b85
#define CH_RANK			0x2b87
#define CH_SINK_ASE		0x2bc4
#define CH_ASE_CP		0x2bc6
#define CH_SINK_PAC		0x2bc9
#define CH_SINK_LOC		0x2bca

#define CODING_LC3		0x06

/* The LTV structures of a PAC record's capabilities, and of a codec's configuration */
#define CAP_FREQS		0x01
#define CAP_DURS		0x02
#define CAP_CHANS		0x03
#define CAP_OCTETS		0x04
#define CAP_FRAMES		0x05
#define CFG_FREQ		0x01
#define CFG_DUR			0x02
#define CFG_ALLOC		0x03
#define CFG_OCTETS		0x04
#define CFG_BLOCKS		0x05
#define CFG_DUR_10MS		0x01
#define META_STREAMING		0x02
#define CTX_MEDIA		0x0004

/* The ASE control point */
#define OP_CONFIG_CODEC		0x01
#define OP_CONFIG_QOS		0x02
#define OP_ENABLE		0x03
#define OP_DISABLE		0x05
#define TARGET_BALANCED		0x02
#define TARGET_PHY_2M		0x02
#define CP_LEN_MAX		30		/* the longest operation written: Config Codec */

/* The volume's control point */
#define VOP_SET_ABSOLUTE	0x04
#define VCS_E_COUNTER		0x80

#define SIRK_ENCRYPTED		0x00
#define SIRK_PLAIN		0x01

#define HCI_REMOTE_USER_ENDED	0x13
#define NCHR			16
#define VAL_MAX			256
#define BAP_TMO_MS		30000

enum { NS_PACS, NS_ASCS, NS_CSIS, NS_VCS, NSVC };

enum { B_NONE, B_CHR, B_DESC, B_READ, B_CCC, B_READY, B_VOL, B_VOL_READ };

typedef struct {
	UH	uuid;
	UB	props;
	UH	val, end, cccd;
} CHR;

typedef struct {
	LEADEV	d;
	BTC	*c;
	BOOL	used;
	UH	mtu;
	UH	svc_first[NSVC], svc_last[NSVC];
	CHR	chr[NCHR];
	INT	nchr;
	UB	step;
	INT	si, cur;
	UH	next;
	UW	due;			/* when the request out is given up, 0 none */
	UB	val[VAL_MAX];		/* a value being read, perhaps in pieces */
	INT	vlen;
	BOOL	vol_want;		/* a volume to set once nothing else is out */
	UB	vol_set;
	BOOL	vol_again;
} BAP;

LOCAL BAP	bap_tab[NPEER];

LOCAL UH get16( CONST UB *p )
{
	return (UH)( p[0] | ( p[1] << 8 ) );
}

LOCAL UW get24( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 );
}

LOCAL void put16( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
}

LOCAL void put24( UB *p, UW v )
{
	p[0] = (UB)v;
	p[1] = (UB)( v >> 8 );
	p[2] = (UB)( v >> 16 );
}

LOCAL BAP *bap_of( BTC *c, PEER *p )
{
	INT	i = (INT)( p - c->peer );

	return ( i >= 0 && i < NPEER ) ? &bap_tab[i] : NULL;
}

EXPORT LEADEV *knl_btbap_dev( BTC *c, INT i )
{
	BAP	*b = &bap_tab[i];

	return ( i >= 0 && i < NPEER && b->used && b->c == c ) ? &b->d : NULL;
}

/*
 * The SIRK as the server encrypts it with the LTK (CSIS 4.5):
 * k1(K, s1("SIRKenc"), "csis") XOR SIRK, the same both ways. K and the
 * SIRK go the lowest byte first; s1 and k1 work on the values written the
 * other way round.
 */
EXPORT void knl_btbap_sef( CONST UB *k, CONST UB *sirk, UB *out )
{
	static CONST UB	zero[16] = { 0 };
	static CONST UB	m[7] = { 'S', 'I', 'R', 'K', 'e', 'n', 'c' };
	static CONST UB	pp[4] = { 'c', 's', 'i', 's' };
	UB		salt[16], kb[16], t[16], r[16];
	INT		i;

	knl_bts_cmac(zero, m, 7, salt);
	for ( i = 0; i < 16; i++ ) kb[i] = k[15 - i];
	knl_bts_cmac(salt, kb, 16, t);
	knl_bts_cmac(t, pp, 4, r);
	for ( i = 0; i < 16; i++ ) out[i] = (UB)( r[15 - i] ^ sirk[i] );
}

/* ---------------------------------------------------------------- requests */

LOCAL void att_send( BAP *b, CONST UB *f, INT n )
{
	b->due = knl_btx_now() + BAP_TMO_MS;
	knl_btx_l2_send(b->c, b->d.p, CID_ATT, f, n);
}

LOCAL void ask_range( BAP *b, UB op, UH first, UH last, UH uuid )
{
	UB	f[7];
	INT	n = 5;

	f[0] = op;
	put16(f + 1, first);
	put16(f + 3, last);
	if ( op == ATT_TYPE_REQ ) {
		put16(f + 5, uuid);
		n = 7;
	}
	att_send(b, f, n);
}

LOCAL void ask_handle( BAP *b, UB op, UH h, CONST UB *v, INT n )
{
	UB	f[8];

	f[0] = op;
	put16(f + 1, h);
	if ( n > 0 ) knl_memcpy(f + 3, v, n);
	att_send(b, f, n + 3);
}

/* An operation on the ASE control point, which answers by a notification */
LOCAL void cp_write( BAP *b, CONST UB *op, INT n )
{
	UB	f[3 + CP_LEN_MAX];

	if ( b->d.cp == 0 || n > CP_LEN_MAX || n + 3 > b->mtu ) return;
	f[0] = ATT_WRITE_CMD;
	put16(f + 1, b->d.cp);
	knl_memcpy(f + 3, op, n);
	knl_btx_l2_send(b->c, b->d.p, CID_ATT, f, n + 3);
}

/* ---------------------------------------------------------------- the values */

/* What is subscribed to where it can notify: the records and locations too, which change with the device's mode */
LOCAL BOOL want_cccd( UH u )
{
	return (BOOL)( u == CH_SINK_PAC || u == CH_SINK_LOC || u == CH_SINK_ASE || u == CH_ASE_CP
		    || u == CH_VOL_STATE );
}

LOCAL BOOL want_read( UH u )
{
	return (BOOL)( u == CH_SINK_PAC || u == CH_SINK_LOC || u == CH_SINK_ASE || u == CH_SIRK
		    || u == CH_SET_SIZE || u == CH_RANK || u == CH_VOL_STATE );
}

LOCAL BOOL want_chr( UH u )
{
	return (BOOL)( want_read(u) || u == CH_ASE_CP || u == CH_VOL_CP );
}

/* The first PAC record of LC3 among the sink's: what it can take */
LOCAL void pac_in( LEADEV *d, CONST UB *v, INT n )
{
	INT	k, at = 1;

	for ( k = 0; n > 0 && k < v[0]; k++ ) {
		INT	cl, ml;
		CONST UB *cap;

		if ( at + 6 > n ) return;
		cl = v[at + 5];
		cap = v + at + 6;
		if ( at + 6 + cl + 1 > n ) return;
		ml = v[at + 6 + cl];
		if ( v[at] == CODING_LC3 && !d->lc3 ) {
			INT	i = 0;

			d->lc3 = TRUE;
			d->chans = 0x01;		/* one channel, unless it says */
			d->frames_max = 1;
			while ( i < cl && cap[i] != 0 && i + 1 + cap[i] <= cl ) {
				INT	l = cap[i] - 1;
				CONST UB *x = cap + i + 2;

				switch ( cap[i + 1] ) {
				case CAP_FREQS:  if ( l >= 2 ) d->freqs = get16(x); break;
				case CAP_DURS:   if ( l >= 1 ) d->durs = x[0]; break;
				case CAP_CHANS:  if ( l >= 1 ) d->chans = x[0]; break;
				case CAP_OCTETS:
					if ( l >= 4 ) {
						d->oct_min = get16(x);
						d->oct_max = get16(x + 2);
					}
					break;
				case CAP_FRAMES: if ( l >= 1 ) d->frames_max = x[0]; break;
				default: break;
				}
				i += 1 + cap[i];
			}
		}
		at += 6 + cl + 1 + ml;
	}
}

/* An ASE's value: its state, and what the state brings */
LOCAL void ase_in( LEASE *a, CONST UB *v, INT n )
{
	if ( n < 2 ) return;
	a->id = v[0];
	a->state = v[1];
	switch ( v[1] ) {
	case ASE_CODEC:
		if ( n < 25 ) break;
		a->framing = v[2];
		a->phy = v[3];
		a->rtn = v[4];
		a->latency = get16(v + 5);
		a->pd_min = get24(v + 7);
		a->pd_max = get24(v + 10);
		a->ppd_min = get24(v + 13);
		a->ppd_max = get24(v + 16);
		break;
	case ASE_QOS: case ASE_ENABLING: case ASE_STREAMING: case ASE_DISABLING:
		if ( n < 4 ) break;
		a->cig = v[2];
		a->cis = v[3];
		break;
	default:
		break;
	}
}

LOCAL INT ase_of_handle( LEADEV *d, UH h )
{
	INT	i;

	for ( i = 0; i < d->nase; i++ ) {
		if ( d->ase[i].val == h ) return i;
	}
	return -1;
}

LOCAL void value_in( BAP *b, CHR *h, CONST UB *v, INT n )
{
	LEADEV	*d = &b->d;

	switch ( h->uuid ) {
	case CH_SINK_PAC:
		pac_in(d, v, n);
		break;
	case CH_SINK_LOC:
		if ( n >= 4 ) d->sink_loc = (UW)get16(v) | ( (UW)get16(v + 2) << 16 );
		break;
	case CH_SINK_ASE:
		if ( d->nase < LEA_NASE && n >= 2 ) {
			LEASE	*a = &d->ase[d->nase++];

			knl_memset(a, 0, sizeof(*a));
			a->val = h->val;
			a->cccd = h->cccd;
			ase_in(a, v, n);
		}
		break;
	case CH_SIRK:
		if ( n >= 17 && ( v[0] == SIRK_PLAIN || v[0] == SIRK_ENCRYPTED ) ) {
			if ( v[0] == SIRK_PLAIN ) knl_memcpy(d->sirk, v + 1, 16);
			else knl_btbap_sef(d->p->ltk, v + 1, d->sirk);
			d->in_set = TRUE;
		}
		break;
	case CH_SET_SIZE:
		if ( n >= 1 ) d->set_size = v[0];
		break;
	case CH_RANK:
		if ( n >= 1 ) d->rank = v[0];
		break;
	case CH_VOL_STATE:
		if ( n >= 3 ) {
			d->vol = v[0];
			d->mute = v[1];
			d->vol_count = v[2];
		}
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the steps */

LOCAL void next_chr( BAP *b );
LOCAL void next_desc( BAP *b );
LOCAL void next_read( BAP *b );
LOCAL void next_ccc( BAP *b );
LOCAL void vol_go( BAP *b );

/* The characteristic still open takes the handles up to the next declaration */
LOCAL void chr_close( BAP *b, UH end )
{
	if ( b->nchr > 0 && b->chr[b->nchr - 1].end == 0xffff ) b->chr[b->nchr - 1].end = end;
}

LOCAL void next_chr( BAP *b )
{
	b->step = B_CHR;
	while ( b->si < NSVC ) {
		if ( b->svc_first[b->si] != 0 && b->svc_first[b->si] <= b->svc_last[b->si] ) {
			if ( b->next == 0 ) b->next = b->svc_first[b->si];
			ask_range(b, ATT_TYPE_REQ, b->next, b->svc_last[b->si], UUID_CHAR);
			return;
		}
		b->si++;
		b->next = 0;
	}
	b->cur = 0;
	b->next = 0;
	next_desc(b);
}

LOCAL void next_desc( BAP *b )
{
	b->step = B_DESC;
	while ( b->cur < b->nchr ) {
		CHR	*h = &b->chr[b->cur];

		if ( want_cccd(h->uuid) && ( h->props & PROP_NOTIFY ) != 0 ) {
			if ( b->next == 0 ) b->next = (UH)( h->val + 1 );
			if ( b->next <= h->end && b->next != 0 ) {
				ask_range(b, ATT_FIND_REQ, b->next, h->end, 0);
				return;
			}
		}
		b->cur++;
		b->next = 0;
	}
	b->cur = 0;
	next_read(b);
}

LOCAL void next_read( BAP *b )
{
	b->step = B_READ;
	while ( b->cur < b->nchr ) {
		CHR	*h = &b->chr[b->cur];

		if ( want_read(h->uuid) && ( h->props & PROP_READ ) != 0 ) {
			b->vlen = 0;
			ask_handle(b, ATT_READ_REQ, h->val, NULL, 0);
			return;
		}
		b->cur++;
	}
	b->cur = 0;
	next_ccc(b);
}

LOCAL void bap_ready( BAP *b )
{
	LEADEV	*d = &b->d;

	b->step = B_READY;
	b->due = 0;
	d->ready = (BOOL)( d->lc3 && d->nase > 0 && d->cp != 0 && ( d->durs & 0x02 ) != 0 );
	tm_printf((UB *)"bt: \"%s\": LE Audio sink: LC3 %d, frequencies %04x, durations %02x, channels %02x, %d to %d bytes, %d a SDU; locations %08x; %d ASEs; set %d of %d; volume %d\n",
		  d->p->name, d->lc3, d->freqs, d->durs, d->chans, d->oct_min, d->oct_max, d->frames_max,
		  d->sink_loc, d->nase, d->rank, d->in_set ? d->set_size : 1, d->vol);
	if ( !d->ready ) {
		tm_printf((UB *)"bt: \"%s\": no sink of LC3 in 10 ms frames this end can drive\n", d->p->name);
		return;
	}
	knl_btlea_dev_ready(b->c, d);
	if ( b->vol_want ) vol_go(b);
}

LOCAL void next_ccc( BAP *b )
{
	static CONST UB	on[2] = { 0x01, 0x00 };

	b->step = B_CCC;
	while ( b->cur < b->nchr ) {
		if ( b->chr[b->cur].cccd != 0 ) {
			ask_handle(b, ATT_WRITE_REQ, b->chr[b->cur].cccd, on, 2);
			return;
		}
		b->cur++;
	}
	bap_ready(b);
}

/* The volume set, with the change counter last seen */
LOCAL void vol_go( BAP *b )
{
	UB	v[3];

	b->vol_want = FALSE;
	if ( b->d.vol_cp == 0 ) return;
	v[0] = VOP_SET_ABSOLUTE;
	v[1] = b->d.vol_count;
	v[2] = b->vol_set;
	b->step = B_VOL;
	ask_handle(b, ATT_WRITE_REQ, b->d.vol_cp, v, 3);
}

LOCAL CHR *chr_of_val( BAP *b, UH h )
{
	INT	i;

	for ( i = 0; i < b->nchr; i++ ) {
		if ( b->chr[i].val == h ) return &b->chr[i];
	}
	return NULL;
}

/* An answer to the request out */
LOCAL void answer( BAP *b, CONST UB *f, INT n )
{
	UB	op = f[0];
	INT	i, l;

	b->due = 0;
	switch ( b->step ) {
	case B_CHR: {
		BOOL	more = FALSE;

		if ( op == ATT_TYPE_RSP && n >= 2 && f[1] >= 7 ) {
			UH	decl = 0;

			l = f[1];
			for ( i = 2; i + l <= n; i += l ) {
				UH	u = ( l == 7 ) ? get16(f + i + 5) : 0;

				decl = get16(f + i);
				chr_close(b, (UH)( decl - 1 ));
				if ( want_chr(u) && b->nchr < NCHR ) {
					CHR	*h = &b->chr[b->nchr++];

					h->uuid = u;
					h->props = f[i + 2];
					h->val = get16(f + i + 3);
					h->end = 0xffff;
					h->cccd = 0;
					if ( u == CH_ASE_CP ) b->d.cp = h->val;
					if ( u == CH_VOL_CP ) b->d.vol_cp = h->val;
				}
			}
			if ( decl != 0 && decl < b->svc_last[b->si] ) {
				b->next = (UH)( decl + 1 );
				more = TRUE;
			}
		}
		if ( !more ) {
			chr_close(b, b->svc_last[b->si]);
			b->si++;
			b->next = 0;
		}
		next_chr(b);
		break;
	}
	case B_DESC: {
		CHR	*h = &b->chr[b->cur];

		if ( op == ATT_FIND_RSP && n >= 2 ) {
			UH	last = 0;

			l = ( f[1] == 1 ) ? 4 : 18;
			for ( i = 2; i + l <= n; i += l ) {
				last = get16(f + i);
				if ( l == 4 && get16(f + i + 2) == UUID_CCCD ) h->cccd = last;
			}
			if ( last != 0 && last < h->end ) {
				b->next = (UH)( last + 1 );
			} else {
				b->cur++;
				b->next = 0;
			}
		} else {
			b->cur++;
			b->next = 0;
		}
		next_desc(b);
		break;
	}
	case B_READ: {
		CHR	*h = &b->chr[b->cur];

		if ( op == ATT_READ_RSP || op == ATT_BLOB_RSP ) {
			INT	m = n - 1;

			if ( b->vlen + m > VAL_MAX ) m = VAL_MAX - b->vlen;
			knl_memcpy(b->val + b->vlen, f + 1, m);
			b->vlen += m;
			if ( n - 1 == b->mtu - 1 && b->vlen < VAL_MAX ) {
				UB	off[2];

				put16(off, (UW)b->vlen);
				ask_handle(b, ATT_BLOB_REQ, h->val, off, 2);
				break;
			}
			value_in(b, h, b->val, b->vlen);
		} else if ( op == ATT_ERROR && n >= 5 && f[4] == ATT_E_NOT_LONG && b->vlen > 0 ) {
			value_in(b, h, b->val, b->vlen);
		} else if ( op == ATT_ERROR && n >= 5 ) {
			tm_printf((UB *)"bt: \"%s\": characteristic %04x not read, error %02x\n", b->d.p->name, h->uuid, f[4]);
		}
		b->cur++;
		next_read(b);
		break;
	}
	case B_CCC:
		if ( op == ATT_ERROR && n >= 5 ) {
			tm_printf((UB *)"bt: \"%s\": notifications of %04x refused, error %02x\n", b->d.p->name, b->chr[b->cur].uuid, f[4]);
		}
		b->cur++;
		next_ccc(b);
		break;
	case B_VOL:
		b->step = B_READY;
		if ( op == ATT_ERROR && n >= 5 && f[4] == VCS_E_COUNTER && !b->vol_again ) {
			CHR	*h;

			/* its counter has moved on: read it, and once again */
			for ( i = 0, h = NULL; i < b->nchr; i++ ) {
				if ( b->chr[i].uuid == CH_VOL_STATE ) h = &b->chr[i];
			}
			if ( h != NULL ) {
				b->vol_again = TRUE;
				b->step = B_VOL_READ;
				ask_handle(b, ATT_READ_REQ, h->val, NULL, 0);
			}
		} else if ( op == ATT_ERROR && n >= 5 ) {
			tm_printf((UB *)"bt: \"%s\": volume not set, error %02x\n", b->d.p->name, f[4]);
		} else {
			b->vol_again = FALSE;
		}
		break;
	case B_VOL_READ:
		b->step = B_READY;
		if ( op == ATT_READ_RSP && n >= 4 ) {
			b->d.vol = f[1];
			b->d.mute = f[2];
			b->d.vol_count = f[3];
			vol_go(b);
		}
		break;
	default:
		break;
	}
}

/* A notification: an ASE's new state, what an operation came to, the volume */
LOCAL void notified( BAP *b, CONST UB *f, INT n )
{
	LEADEV	*d = &b->d;
	UH	h;
	INT	ai, i;
	CHR	*ch;

	if ( n < 3 ) return;
	h = get16(f + 1);
	f += 3;
	n -= 3;
	ai = ase_of_handle(d, h);
	if ( ai >= 0 ) {
		ase_in(&d->ase[ai], f, n);
		if ( d->ready ) knl_btlea_ase(b->c, d, ai);
		return;
	}
	if ( h == d->cp && n >= 2 ) {
		for ( i = 0; i < f[1] && 2 + 3 * i + 3 <= n; i++ ) {
			CONST UB *e = f + 2 + 3 * i;
			INT	k;

			if ( e[1] == 0 ) continue;
			for ( k = 0; k < d->nase; k++ ) {
				if ( d->ase[k].id == e[0] ) knl_btlea_cp_fail(b->c, d, k, f[0], e[1], e[2]);
			}
		}
		return;
	}
	ch = chr_of_val(b, h);
	if ( ch != NULL && ch->uuid == CH_VOL_STATE && n >= 3 ) {
		d->vol = f[0];
		d->mute = f[1];
		d->vol_count = f[2];
	}
}

/* ---------------------------------------------------------------- from bt_le.c */

/* A new link's services are being found: nothing is kept of the last one */
EXPORT void knl_btbap_reset( BTC *c, PEER *p )
{
	BAP	*b = bap_of(c, p);

	if ( b == NULL ) return;
	if ( b->used && b->d.ready ) knl_btlea_dev_down(c, &b->d);
	knl_memset(b, 0, sizeof(*b));
}

/* A primary service of the device, as bt_le.c lists them */
EXPORT void knl_btbap_svc( BTC *c, PEER *p, UH uuid, UH first, UH last )
{
	BAP	*b = bap_of(c, p);
	INT	s = ( uuid == SVC_PACS ) ? NS_PACS : ( uuid == SVC_ASCS ) ? NS_ASCS
		  : ( uuid == SVC_CSIS ) ? NS_CSIS : ( uuid == SVC_VCS ) ? NS_VCS : -1;

	if ( b == NULL || s < 0 || b->svc_first[s] != 0 ) return;
	b->svc_first[s] = first;
	b->svc_last[s] = last;
}

/* Whether it is taken as an LE Audio device: then its ATT is this file's */
EXPORT BOOL knl_btbap_start( BTC *c, PEER *p, UH mtu )
{
	BAP	*b = bap_of(c, p);

	if ( b == NULL || !c->has_cis || b->svc_first[NS_PACS] == 0 || b->svc_first[NS_ASCS] == 0 ) return FALSE;
	if ( mtu < CP_LEN_MAX + 3 ) {
		tm_printf((UB *)"bt: \"%s\": LE Audio, but an ATT MTU of %d is too short for it\n", p->name, mtu);
		return FALSE;
	}
	b->used = TRUE;
	b->c = c;
	b->mtu = mtu;
	b->d.p = p;
	tm_printf((UB *)"bt: \"%s\": LE Audio: PACS, ASCS%s%s\n", p->name,
		  b->svc_first[NS_CSIS] != 0 ? ", CSIS" : "", b->svc_first[NS_VCS] != 0 ? ", VCS" : "");
	b->si = 0;
	b->next = 0;
	next_chr(b);
	return TRUE;
}

/* What ATT brings of a device taken here: TRUE when this file has dealt with it */
EXPORT BOOL knl_btbap_att_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	BAP	*b = bap_of(c, p);

	if ( b == NULL || !b->used || n < 1 ) return FALSE;
	switch ( f[0] ) {
	case ATT_NOTIFY:
		notified(b, f, n);
		return TRUE;
	case ATT_ERROR: case ATT_FIND_RSP: case ATT_TYPE_RSP: case ATT_READ_RSP: case ATT_BLOB_RSP: case ATT_WRITE_RSP:
		answer(b, f, n);
		return TRUE;
	default:
		return FALSE;
	}
}

EXPORT void knl_btbap_down( BTC *c, PEER *p )
{
	knl_btbap_reset(c, p);
}

/*
 * Whether what advertises has a resolvable set identifier of a set one of
 * whose members is here and the others not yet: the hash its SIRK gives
 * of the random part (CSIS 4.8).
 */
EXPORT BOOL knl_btbap_rsi( BTC *c, CONST UB *ad, INT n )
{
	INT	at = 0, i;

	while ( at < n && ad[at] != 0 && at + 1 + ad[at] <= n ) {
		if ( ad[at + 1] == 0x2e && ad[at] == 7 ) {
			CONST UB *rsi = ad + at + 2;

			for ( i = 0; i < NPEER; i++ ) {
				LEADEV	*d = knl_btbap_dev(c, i);
				UB	h[3];

				if ( d == NULL || !d->ready || !d->in_set || d->set_size < 2 ) continue;
				knl_bts_ah(d->sirk, rsi + 3, h);
				if ( same_bt_bytes(h, rsi, 3) ) return TRUE;
			}
		}
		at += 1 + ad[at];
	}
	return FALSE;
}

/* Whether a member of a set is not here: what advertises is then listened to */
EXPORT BOOL knl_btbap_seek( BTC *c )
{
	INT	i, k;

	for ( i = 0; i < NPEER; i++ ) {
		LEADEV	*d = knl_btbap_dev(c, i);
		INT	here = 0;

		if ( d == NULL || !d->ready || !d->in_set || d->set_size < 2 ) continue;
		for ( k = 0; k < NPEER; k++ ) {
			LEADEV	*e = knl_btbap_dev(c, k);

			if ( e != NULL && e->ready && e->in_set && same_bt_bytes(e->sirk, d->sirk, 16) ) here++;
		}
		if ( here < d->set_size ) return TRUE;
	}
	return FALSE;
}

/* Each round of the task: a device that does not answer is let go */
EXPORT void knl_btbap_tick( BTC *c )
{
	UW	now = knl_btx_now();
	INT	i;

	for ( i = 0; i < NPEER; i++ ) {
		BAP	*b = &bap_tab[i];
		UB	r[3];

		if ( !b->used || b->c != c || b->due == 0 || (W)( now - b->due ) < 0 ) continue;
		tm_printf((UB *)"bt: \"%s\": its LE Audio services did not answer, step %d\n", b->d.p->name, b->step);
		b->due = 0;
		put16(r, b->d.p->handle);
		r[2] = HCI_REMOTE_USER_ENDED;
		knl_btx_cmd(c, C_DISCONNECT, r, 3);
	}
}

/* ---------------------------------------------------------------- what bt_lea.c asks */

LOCAL BAP *bap_dev( LEADEV *d )
{
	return (BAP *)d;		/* the device is the first of its BAP */
}

/* Config Codec: LC3 at a frequency, in 10 ms frames of octets bytes, for the locations alloc */
EXPORT void knl_btbap_codec( BTC *c, LEADEV *d, INT ai, UB freq, UW alloc, UH octets )
{
	UB	u[CP_LEN_MAX];
	INT	k = 11;

	(void)c;
	u[0] = OP_CONFIG_CODEC;
	u[1] = 1;
	u[2] = d->ase[ai].id;
	u[3] = TARGET_BALANCED;
	u[4] = TARGET_PHY_2M;
	u[5] = CODING_LC3;
	u[6] = u[7] = u[8] = u[9] = 0;
	u[10] = 19;
	u[k++] = 2;
	u[k++] = CFG_FREQ;
	u[k++] = freq;
	u[k++] = 2;
	u[k++] = CFG_DUR;
	u[k++] = CFG_DUR_10MS;
	u[k++] = 5;
	u[k++] = CFG_ALLOC;
	put16(u + k, alloc);
	put16(u + k + 2, alloc >> 16);
	k += 4;
	u[k++] = 3;
	u[k++] = CFG_OCTETS;
	put16(u + k, octets);
	k += 2;
	u[k++] = 2;
	u[k++] = CFG_BLOCKS;
	u[k++] = 1;
	cp_write(bap_dev(d), u, k);
}

EXPORT void knl_btbap_qos( BTC *c, LEADEV *d, INT ai, UB cig, UB cis, UW interval, UB framing,
			   UB phy, UH max_sdu, UB rtn, UH latency, UW pd )
{
	UB	u[18];

	(void)c;
	u[0] = OP_CONFIG_QOS;
	u[1] = 1;
	u[2] = d->ase[ai].id;
	u[3] = cig;
	u[4] = cis;
	put24(u + 5, interval);
	u[8] = framing;
	u[9] = phy;
	put16(u + 10, max_sdu);
	u[12] = rtn;
	put16(u + 13, latency);
	put24(u + 15, pd);
	cp_write(bap_dev(d), u, 18);
}

/* Enable, the stream being for media */
EXPORT void knl_btbap_enable( BTC *c, LEADEV *d, INT ai )
{
	UB	u[8];

	(void)c;
	u[0] = OP_ENABLE;
	u[1] = 1;
	u[2] = d->ase[ai].id;
	u[3] = 4;
	u[4] = 3;
	u[5] = META_STREAMING;
	put16(u + 6, CTX_MEDIA);
	cp_write(bap_dev(d), u, 8);
}

EXPORT void knl_btbap_disable( BTC *c, LEADEV *d, INT ai )
{
	UB	u[3];

	(void)c;
	u[0] = OP_DISABLE;
	u[1] = 1;
	u[2] = d->ase[ai].id;
	cp_write(bap_dev(d), u, 3);
}

/* The device's volume, 0 to 255, when nothing else is being asked of it */
EXPORT void knl_btbap_volume( BTC *c, LEADEV *d, UB vol )
{
	BAP	*b = bap_dev(d);

	(void)c;
	b->vol_set = vol;
	b->vol_want = TRUE;
	if ( b->step == B_READY ) vol_go(b);
}
