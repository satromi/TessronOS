/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_sdp.c
 *	This end's service records (design 6.8), and the one question this
 *	end asks of earphones' records besides bt.c's: where their hands-free
 *	service is.
 *
 *	Earphones that connect by themselves ask first what this end offers
 *	and on which channels: a source of A2DP, the target and the
 *	controller of AVRCP, the audio gateway of HFP. Each record is made
 *	once, as the protocol's data elements, its attributes in the order of
 *	their IDs; a question is answered from them: the handles of the
 *	records that have every UUID asked (ServiceSearch), the attributes
 *	asked of one record (ServiceAttribute) or of every record that has
 *	them (ServiceSearchAttribute). An answer longer than the device
 *	takes, or than one frame of this end, goes in pieces: the
 *	continuation is where the next piece starts, two bytes.
 *
 *	The question of the device's own records rides on a channel of its
 *	own, after bt.c's question about its sink has ended: the hands-free
 *	service's protocols, which name its server channel of RFCOMM, and
 *	its features.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/bt.h>
#include "../usb/usbdev.h"
#include "../snd/sndhw.h"
#include "btint.h"
#include "btaudio.h"

/* PDUs */
#define SDP_ERROR_RSP		0x01
#define SDP_SS_REQ		0x02
#define SDP_SS_RSP		0x03
#define SDP_SA_REQ		0x04
#define SDP_SA_RSP		0x05
#define SDP_SSA_REQ		0x06
#define SDP_SSA_RSP		0x07

/* What an error response says */
#define SDPE_BAD_HANDLE		0x0002
#define SDPE_SYNTAX		0x0003
#define SDPE_PDU_SIZE		0x0004
#define SDPE_CONT		0x0005

/* Data elements: the type in the top five bits of the head, the size below */
#define DE_UINT8		0x08
#define DE_UINT16		0x09
#define DE_UINT32		0x0a
#define DE_UUID16		0x19
#define DE_UUID32		0x1a
#define DE_UUID128		0x1c
#define DE_STR8			0x25
#define DE_SEQ8			0x35
#define DE_SEQ16		0x36
#define DET_UINT		1
#define DET_UUID		3
#define DET_SEQ			6

/* Attributes */
#define AT_HANDLE		0x0000
#define AT_CLASSES		0x0001
#define AT_PROTOCOLS		0x0004
#define AT_BROWSE		0x0005
#define AT_LANG			0x0006
#define AT_PROFILES		0x0009
#define AT_NAME			0x0100
#define AT_NETWORK		0x0301
#define AT_FEATURES		0x0311

/* UUIDs */
#define U_SDP			0x0001
#define U_RFCOMM		0x0003
#define U_AVCTP			0x0017
#define U_AVDTP			0x0019
#define U_L2CAP			0x0100
#define U_PUBLIC_BROWSE		0x1002
#define U_AUDIO_SOURCE		0x110a
#define U_AV_TARGET		0x110c
#define U_A2DP			0x110d
#define U_AV_REMOTE		0x110e
#define U_AV_CONTROLLER		0x110f
#define U_HANDSFREE		0x111e
#define U_HANDSFREE_AG		0x111f
#define U_GENERIC_AUDIO		0x1203

#define NREC			4
#define REC_MAX			200
#define REC_UUIDS		12
#define FIRST_HANDLE		0x00010001

#define PATTERN_MAX		12	/* the UUIDs a search may name */
#define RANGES_MAX		16	/* and the attributes or their ranges a question may */
#define ANSWER_MAX		( NREC * ( REC_MAX + 3 ) + 3 )
#define PIECE_ROOM		600	/* the most of an answer in one frame of this end */
#define HF_BUF			512
#define HF_TMO_MS		5000

typedef struct {
	UW	handle;
	UB	b[REC_MAX];		/* (attribute ID, value) pairs, in the order of their IDs */
	INT	n;
	UH	uuid[REC_UUIDS];	/* every UUID in it, for the searches */
	INT	nuuid;
} REC;

LOCAL REC	rec[NREC];
LOCAL BOOL	rec_made = FALSE;
LOCAL UB	answer[ANSWER_MAX];

/* ---------------------------------------------------------------- making the records */

LOCAL REC	*mk;			/* the record being made */

LOCAL void put1( UB v )
{
	if ( mk->n < REC_MAX ) mk->b[mk->n++] = v;
}

LOCAL void put_u16( UH v )
{
	put1(DE_UINT16);
	put1((UB)( v >> 8 ));
	put1((UB)v);
}

LOCAL void put_u8( UB v )
{
	put1(DE_UINT8);
	put1(v);
}

LOCAL void put_uuid( UH u )
{
	put1(DE_UUID16);
	put1((UB)( u >> 8 ));
	put1((UB)u);
	if ( mk->nuuid < REC_UUIDS ) mk->uuid[mk->nuuid++] = u;
}

LOCAL void put_attr( UH id )
{
	put_u16(id);
}

/* A sequence begun: where its length goes; the sequences here are all shorter than 256 bytes */
LOCAL INT seq_open( void )
{
	put1(DE_SEQ8);
	put1(0);
	return mk->n - 1;
}

LOCAL void seq_close( INT at )
{
	mk->b[at] = (UB)( mk->n - at - 1 );
}

LOCAL void put_str( CONST char *s )
{
	INT	n = (INT)knl_strlen(s);

	put1(DE_STR8);
	put1((UB)n);
	while ( *s != 0 ) put1((UB)*s++);
}

/* What every record has: its handle, its classes, the public browse group, the language and its name */
LOCAL void rec_begin( INT i, UH cls1, UH cls2 )
{
	INT	s;

	mk = &rec[i];
	knl_memset(mk, 0, sizeof(*mk));
	mk->handle = (UW)( FIRST_HANDLE + i );
	put_attr(AT_HANDLE);
	put1(DE_UINT32);
	put1((UB)( mk->handle >> 24 ));
	put1((UB)( mk->handle >> 16 ));
	put1((UB)( mk->handle >> 8 ));
	put1((UB)mk->handle);
	put_attr(AT_CLASSES);
	s = seq_open();
	put_uuid(cls1);
	if ( cls2 != 0 ) put_uuid(cls2);
	seq_close(s);
}

/* The protocols: L2CAP on a PSM, and above it the protocol of that PSM with its version */
LOCAL void put_l2_proto( UH psm, UH proto, UH ver )
{
	INT	s, s1;

	put_attr(AT_PROTOCOLS);
	s = seq_open();
	s1 = seq_open();
	put_uuid(U_L2CAP);
	put_u16(psm);
	seq_close(s1);
	s1 = seq_open();
	put_uuid(proto);
	put_u16(ver);
	seq_close(s1);
	seq_close(s);
}

LOCAL void put_browse_lang( void )
{
	INT	s;

	put_attr(AT_BROWSE);
	s = seq_open();
	put_uuid(U_PUBLIC_BROWSE);
	seq_close(s);
	/* English, UTF-8 (the MIBenum 106), the names from 0x0100 */
	put_attr(AT_LANG);
	s = seq_open();
	put_u16(0x656e);
	put_u16(106);
	put_u16(AT_NAME);
	seq_close(s);
}

LOCAL void put_profile( UH profile, UH ver )
{
	INT	s, s1;

	put_attr(AT_PROFILES);
	s = seq_open();
	s1 = seq_open();
	put_uuid(profile);
	put_u16(ver);
	seq_close(s1);
	seq_close(s);
}

LOCAL void rec_make( void )
{
	INT	s, s1;

	/* A2DP 1.3, its source: AVDTP 1.3, a player */
	rec_begin(0, U_AUDIO_SOURCE, 0);
	put_l2_proto(PSM_AVDTP_A, U_AVDTP, 0x0103);
	put_browse_lang();
	put_profile(U_A2DP, 0x0103);
	put_attr(AT_NAME);
	put_str("Audio Source");
	put_attr(AT_FEATURES);
	put_u16(0x0001);

	/* AVRCP 1.4, its target: category 1, the buttons of a player */
	rec_begin(1, U_AV_TARGET, 0);
	put_l2_proto(PSM_AVCTP, U_AVCTP, 0x0104);
	put_browse_lang();
	put_profile(U_AV_REMOTE, 0x0104);
	put_attr(AT_NAME);
	put_str("AVRCP Target");
	put_attr(AT_FEATURES);
	put_u16(0x0001);

	/* AVRCP 1.4, its controller: category 2, the volume of the earphones */
	rec_begin(2, U_AV_REMOTE, U_AV_CONTROLLER);
	put_l2_proto(PSM_AVCTP, U_AVCTP, 0x0104);
	put_browse_lang();
	put_profile(U_AV_REMOTE, 0x0104);
	put_attr(AT_NAME);
	put_str("AVRCP Controller");
	put_attr(AT_FEATURES);
	put_u16(0x0002);

	/* HFP 1.8, its audio gateway: RFCOMM on the server channel, no network to reject a call on */
	rec_begin(3, U_HANDSFREE_AG, U_GENERIC_AUDIO);
	put_attr(AT_PROTOCOLS);
	s = seq_open();
	s1 = seq_open();
	put_uuid(U_L2CAP);
	seq_close(s1);
	s1 = seq_open();
	put_uuid(U_RFCOMM);
	put_u8(RFC_CH_AG);
	seq_close(s1);
	seq_close(s);
	put_browse_lang();
	put_profile(U_HANDSFREE, 0x0108);
	put_attr(AT_NAME);
	put_str("Voice Gateway");
	put_attr(AT_NETWORK);
	put_u8(0x00);
	put_attr(AT_FEATURES);
	put_u16(0x0000);
	rec_made = TRUE;
}

/* ---------------------------------------------------------------- reading a question */

/* The head of a data element: its type, the bytes of its head and of its content */
LOCAL BOOL de_at( CONST UB *b, INT n, INT *p_type, INT *p_hdr, INT *p_len )
{
	static CONST UB	fixed[5] = { 1, 2, 4, 8, 16 };
	INT		sz;

	if ( n < 1 ) return FALSE;
	*p_type = b[0] >> 3;
	sz = b[0] & 7;
	if ( *p_type == 0 ) {
		*p_hdr = 1;
		*p_len = 0;
	} else if ( sz < 5 ) {
		*p_hdr = 1;
		*p_len = fixed[sz];
	} else if ( sz == 5 ) {
		if ( n < 2 ) return FALSE;
		*p_hdr = 2;
		*p_len = b[1];
	} else if ( sz == 6 ) {
		if ( n < 3 ) return FALSE;
		*p_hdr = 3;
		*p_len = ( b[1] << 8 ) | b[2];
	} else {
		if ( n < 5 || b[1] != 0 || b[2] != 0 ) return FALSE;
		*p_hdr = 5;
		*p_len = ( b[3] << 8 ) | b[4];
	}
	return (BOOL)( *p_hdr + *p_len <= n );
}

/*
 * A UUID as the 16 bits of the base UUID it is made of: 0 for one of
 * another form, which no record here has.
 */
LOCAL UH uuid16_of( CONST UB *v, INT len )
{
	static CONST UB	base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };

	if ( len == 2 ) return (UH)( ( v[0] << 8 ) | v[1] );
	if ( len == 4 && v[0] == 0 && v[1] == 0 ) return (UH)( ( v[2] << 8 ) | v[3] );
	if ( len == 16 && v[0] == 0 && v[1] == 0 ) {
		INT	i;

		for ( i = 0; i < 12; i++ ) {
			if ( v[4 + i] != base[i] ) return 0;
		}
		return (UH)( ( v[2] << 8 ) | v[3] );
	}
	return 0;
}

/* The search pattern: a sequence of UUIDs. How many there are, -1 one not well formed */
LOCAL INT pattern_of( CONST UB *b, INT n, UH *u, INT *p_used )
{
	INT	t, h, l, at, k = 0, t2, h2, l2;

	if ( !de_at(b, n, &t, &h, &l) || t != DET_SEQ ) return -1;
	for ( at = h; at < h + l; at += h2 + l2 ) {
		if ( !de_at(b + at, h + l - at, &t2, &h2, &l2) || t2 != DET_UUID || k >= PATTERN_MAX ) return -1;
		u[k++] = uuid16_of(b + at + h2, l2);
	}
	*p_used = h + l;
	return ( k > 0 ) ? k : -1;
}

/* The attributes asked: IDs and ranges, as pairs from and to. How many, -1 not well formed */
LOCAL INT ranges_of( CONST UB *b, INT n, UH (*r)[2], INT *p_used )
{
	INT	t, h, l, at, k = 0, t2, h2, l2;

	if ( !de_at(b, n, &t, &h, &l) || t != DET_SEQ ) return -1;
	for ( at = h; at < h + l; at += h2 + l2 ) {
		CONST UB *v;

		if ( !de_at(b + at, h + l - at, &t2, &h2, &l2) || t2 != DET_UINT || k >= RANGES_MAX ) return -1;
		v = b + at + h2;
		if ( l2 == 2 ) {
			r[k][0] = r[k][1] = (UH)( ( v[0] << 8 ) | v[1] );
		} else if ( l2 == 4 ) {
			r[k][0] = (UH)( ( v[0] << 8 ) | v[1] );
			r[k][1] = (UH)( ( v[2] << 8 ) | v[3] );
		} else {
			return -1;
		}
		k++;
	}
	*p_used = h + l;
	return ( k > 0 ) ? k : -1;
}

/* Whether a record has every UUID of the pattern */
LOCAL BOOL rec_matches( CONST REC *r, CONST UH *u, INT nu )
{
	INT	i, k;

	for ( i = 0; i < nu; i++ ) {
		BOOL	has = FALSE;

		for ( k = 0; k < r->nuuid && !has; k++ ) has = (BOOL)( u[i] != 0 && r->uuid[k] == u[i] );
		if ( !has ) return FALSE;
	}
	return TRUE;
}

/* The record's attributes asked, as a sequence, at out: how many bytes */
LOCAL INT rec_attrs( CONST REC *r, UH (*rg)[2], INT nr, UB *out )
{
	INT	at = 0, n = 3, t, h, l;

	while ( at < r->n ) {
		UH	id = (UH)( ( r->b[at + 1] << 8 ) | r->b[at + 2] );
		INT	whole, i;
		BOOL	want = FALSE;

		if ( !de_at(r->b + at + 3, r->n - at - 3, &t, &h, &l) ) break;
		whole = 3 + h + l;
		for ( i = 0; i < nr && !want; i++ ) want = (BOOL)( id >= rg[i][0] && id <= rg[i][1] );
		if ( want ) {
			knl_memcpy(out + n, r->b + at, whole);
			n += whole;
		}
		at += whole;
	}
	out[0] = DE_SEQ16;
	out[1] = (UB)( ( n - 3 ) >> 8 );
	out[2] = (UB)( n - 3 );
	return n;
}

/* ---------------------------------------------------------------- answering */

LOCAL void sdp_send( BTC *c, PEER *p, UB pdu, CONST UB *q, CONST UB *d, INT n )
{
	UB	b[5 + PIECE_ROOM + 8];

	b[0] = pdu;
	b[1] = q[1];
	b[2] = q[2];
	b[3] = (UB)( n >> 8 );
	b[4] = (UB)n;
	knl_memcpy(b + 5, d, n);
	knl_btx_l2_send(c, p, p->rcid_x[XI_SDP_SRV], b, n + 5);
}

LOCAL void sdp_error( BTC *c, PEER *p, CONST UB *q, UH code )
{
	UB	d[2];

	d[0] = (UB)( code >> 8 );
	d[1] = (UB)code;
	sdp_send(c, p, SDP_ERROR_RSP, q, d, 2);
	tm_printf((UB *)"bt: \"%s\": a question of the service records here (%02x) answered error %d\n", p->name, q[0], code);
}

/*
 * A piece of an answer of n bytes in all, from where the continuation
 * says, no longer than max: the count, the bytes, the continuation.
 */
LOCAL void sdp_piece( BTC *c, PEER *p, UB pdu, CONST UB *q, CONST UB *all, INT n, INT max, CONST UB *cont )
{
	UB	d[2 + PIECE_ROOM + 3];
	INT	from = 0, k, room = (INT)p->mtu_x[XI_SDP_SRV] - 5 - 2 - 3;

	if ( cont[0] == 2 ) {
		from = ( cont[1] << 8 ) | cont[2];
	} else if ( cont[0] != 0 ) {
		sdp_error(c, p, q, SDPE_CONT);
		return;
	}
	if ( from > n ) {
		sdp_error(c, p, q, SDPE_CONT);
		return;
	}
	if ( room > PIECE_ROOM ) room = PIECE_ROOM;
	if ( max > room ) max = room;
	k = n - from;
	if ( k > max ) k = max;
	d[0] = (UB)( k >> 8 );
	d[1] = (UB)k;
	knl_memcpy(d + 2, all + from, k);
	if ( from + k < n ) {
		d[2 + k] = 2;
		d[3 + k] = (UB)( ( from + k ) >> 8 );
		d[4 + k] = (UB)( from + k );
		sdp_send(c, p, pdu, q, d, k + 5);
	} else {
		d[2 + k] = 0;
		sdp_send(c, p, pdu, q, d, k + 3);
	}
}

/* The continuation: the byte of its length and what it has, which must be in the question */
LOCAL BOOL cont_of( CONST UB *b, INT n, CONST UB **p_cont )
{
	if ( n < 1 || b[0] > 16 || 1 + b[0] != n ) return FALSE;
	*p_cont = b;
	return TRUE;
}

/* A question to this end's records, on the channel a device opened for it */
EXPORT void knl_btsd_serve( BTC *c, PEER *p, CONST UB *f, INT n )
{
	UH		u[PATTERN_MAX], rg[RANGES_MAX][2];
	INT		nu, nr, used, used2, at, i, max, len = 0;
	CONST UB	*cont;

	if ( !rec_made ) rec_make();
	if ( n < 5 ) return;
	if ( 5 + ( ( f[3] << 8 ) | f[4] ) != n ) {
		sdp_error(c, p, f, SDPE_PDU_SIZE);
		return;
	}
	switch ( f[0] ) {
	case SDP_SS_REQ: {
		UB	d[4 + 4 * NREC + 1];
		INT	cnt = 0;

		at = 5;
		if ( ( nu = pattern_of(f + at, n - at, u, &used) ) < 0 || at + used + 2 > n
		  || !cont_of(f + at + used + 2, n - at - used - 2, &cont) ) {
			sdp_error(c, p, f, SDPE_SYNTAX);
			return;
		}
		max = ( f[at + used] << 8 ) | f[at + used + 1];
		for ( i = 0; i < NREC && cnt < max; i++ ) {
			if ( !rec_matches(&rec[i], u, nu) ) continue;
			d[4 + 4 * cnt] = (UB)( rec[i].handle >> 24 );
			d[5 + 4 * cnt] = (UB)( rec[i].handle >> 16 );
			d[6 + 4 * cnt] = (UB)( rec[i].handle >> 8 );
			d[7 + 4 * cnt] = (UB)rec[i].handle;
			cnt++;
		}
		d[0] = d[2] = 0;
		d[1] = d[3] = (UB)cnt;
		d[4 + 4 * cnt] = 0;
		sdp_send(c, p, SDP_SS_RSP, f, d, 5 + 4 * cnt);
		break;
	}
	case SDP_SA_REQ: {
		UW	h;
		REC	*r = NULL;

		at = 5;
		if ( n < at + 6 ) {
			sdp_error(c, p, f, SDPE_SYNTAX);
			return;
		}
		h = ( (UW)f[at] << 24 ) | ( (UW)f[at + 1] << 16 ) | ( (UW)f[at + 2] << 8 ) | f[at + 3];
		max = ( f[at + 4] << 8 ) | f[at + 5];
		at += 6;
		if ( ( nr = ranges_of(f + at, n - at, rg, &used) ) < 0 || !cont_of(f + at + used, n - at - used, &cont) || max < 7 ) {
			sdp_error(c, p, f, SDPE_SYNTAX);
			return;
		}
		for ( i = 0; i < NREC; i++ ) {
			if ( rec[i].handle == h ) r = &rec[i];
		}
		if ( r == NULL ) {
			sdp_error(c, p, f, SDPE_BAD_HANDLE);
			return;
		}
		len = rec_attrs(r, rg, nr, answer);
		sdp_piece(c, p, SDP_SA_RSP, f, answer, len, max, cont);
		break;
	}
	case SDP_SSA_REQ:
		at = 5;
		if ( ( nu = pattern_of(f + at, n - at, u, &used) ) < 0 || at + used + 2 > n ) {
			sdp_error(c, p, f, SDPE_SYNTAX);
			return;
		}
		max = ( f[at + used] << 8 ) | f[at + used + 1];
		at += used + 2;
		if ( ( nr = ranges_of(f + at, n - at, rg, &used2) ) < 0 || !cont_of(f + at + used2, n - at - used2, &cont) || max < 7 ) {
			sdp_error(c, p, f, SDPE_SYNTAX);
			return;
		}
		len = 3;
		for ( i = 0; i < NREC; i++ ) {
			if ( rec_matches(&rec[i], u, nu) ) len += rec_attrs(&rec[i], rg, nr, answer + len);
		}
		answer[0] = DE_SEQ16;
		answer[1] = (UB)( ( len - 3 ) >> 8 );
		answer[2] = (UB)( len - 3 );
		sdp_piece(c, p, SDP_SSA_RSP, f, answer, len, max, cont);
		break;
	default:
		sdp_error(c, p, f, SDPE_SYNTAX);
		break;
	}
}

/* ---------------------------------------------------------------- the device's hands-free service */

typedef struct {
	BTC	*c;
	PEER	*p;
	BOOL	asked;			/* the question is out */
	UH	tid;
	UW	due;
	UB	got[HF_BUF];
	INT	n;
} HFQ;

LOCAL HFQ	hfq;

/* After bt.c's question: a channel of its own for this one */
EXPORT void knl_btsd_hf_ask( BTC *c, PEER *p )
{
	if ( hfq.p != NULL || p->rcid_x[XI_SDP_HF] != 0 ) return;
	knl_memset(&hfq, 0, sizeof(hfq));
	hfq.c = c;
	hfq.p = p;
	hfq.due = knl_btx_now() + HF_TMO_MS;
	knl_bta_x_open(c, p, PSM_SDP_A, CID_SDP_HF);
}

LOCAL void hf_ask( CONST UB *cont, INT nc )
{
	UB	b[32];
	INT	k = 5;

	hfq.tid++;
	hfq.asked = TRUE;
	b[0] = SDP_SSA_REQ;
	b[1] = (UB)( hfq.tid >> 8 );
	b[2] = (UB)hfq.tid;
	b[k++] = DE_SEQ8;
	b[k++] = 3;
	b[k++] = DE_UUID16;
	b[k++] = (UB)( U_HANDSFREE >> 8 );
	b[k++] = (UB)U_HANDSFREE;
	b[k++] = (UB)( HF_BUF >> 8 );
	b[k++] = (UB)HF_BUF;
	b[k++] = DE_SEQ8;
	b[k++] = 6;
	b[k++] = DE_UINT16;
	b[k++] = (UB)( AT_PROTOCOLS >> 8 );
	b[k++] = (UB)AT_PROTOCOLS;
	b[k++] = DE_UINT16;
	b[k++] = (UB)( AT_FEATURES >> 8 );
	b[k++] = (UB)AT_FEATURES;
	b[k++] = (UB)nc;
	if ( nc > 0 ) knl_memcpy(b + k, cont, nc);
	k += nc;
	b[3] = 0;
	b[4] = (UB)( k - 5 );
	knl_btx_l2_send(hfq.c, hfq.p, hfq.p->rcid_x[XI_SDP_HF], b, k);
}

EXPORT void knl_btsd_hf_up( BTC *c, PEER *p )
{
	(void)c;
	if ( hfq.p == p && !hfq.asked ) hf_ask(NULL, 0);
}

/* The server channel in a list of protocols: the one after RFCOMM's UUID */
LOCAL UB hf_channel( CONST UB *b, INT n )
{
	INT	t, h, l, at, t2, h2, l2, t3, h3, l3;

	if ( !de_at(b, n, &t, &h, &l) || t != DET_SEQ ) return 0;
	for ( at = h; at < h + l; at += h2 + l2 ) {
		CONST UB *s;

		if ( !de_at(b + at, h + l - at, &t2, &h2, &l2) ) return 0;
		if ( t2 != DET_SEQ ) continue;
		s = b + at + h2;
		if ( !de_at(s, l2, &t3, &h3, &l3) || t3 != DET_UUID || uuid16_of(s + h3, l3) != U_RFCOMM ) continue;
		s += h3 + l3;
		if ( !de_at(s, l2 - h3 - l3, &t3, &h3, &l3) || t3 != DET_UINT || l3 != 1 ) return 0;
		return s[h3];
	}
	return 0;
}

/* What came: its first record's channel and features, and the question is over */
LOCAL void hf_done( BOOL ok )
{
	BTC	*c = hfq.c;
	PEER	*p = hfq.p;
	UB	ch = 0;
	UH	feat = 0;
	INT	t, h, l, rt, rh, rl;

	if ( ok && de_at(hfq.got, hfq.n, &t, &h, &l) && t == DET_SEQ && de_at(hfq.got + h, l, &rt, &rh, &rl) && rt == DET_SEQ ) {
		CONST UB	*r = hfq.got + h + rh;
		INT		at = 0;

		while ( at < rl && de_at(r + at, rl - at, &t, &h, &l) && t == DET_UINT && l == 2 ) {
			UH	id = (UH)( ( r[at + h] << 8 ) | r[at + h + 1] );
			INT	vt, vh, vl;

			at += h + l;
			if ( !de_at(r + at, rl - at, &vt, &vh, &vl) ) break;
			if ( id == AT_PROTOCOLS ) ch = hf_channel(r + at, vh + vl);
			if ( id == AT_FEATURES && vt == DET_UINT && vl == 2 ) feat = (UH)( ( r[at + vh] << 8 ) | r[at + vh + 1] );
			at += vh + vl;
		}
	}
	if ( p->rcid_x[XI_SDP_HF] != 0 ) knl_bta_x_close(c, p, CID_SDP_HF);
	knl_memset(&hfq, 0, sizeof(hfq));
	tm_printf((UB *)"bt: \"%s\": its hands-free service: %s %d, features %04x\n", p->name,
		  ( ch != 0 ) ? "server channel" : "none", ch, feat);
	knl_bth_hf_found(c, p, ch, feat);
}

EXPORT void knl_btsd_hf_in( BTC *c, PEER *p, CONST UB *f, INT n )
{
	INT	cnt, nc;

	(void)c;
	if ( hfq.p != p || n < 7 ) return;
	if ( f[0] != SDP_SSA_RSP || f[1] != (UB)( hfq.tid >> 8 ) || f[2] != (UB)hfq.tid ) {
		hf_done(FALSE);
		return;
	}
	cnt = ( f[5] << 8 ) | f[6];
	if ( 7 + cnt >= n || hfq.n + cnt > HF_BUF ) {
		hf_done(FALSE);
		return;
	}
	knl_memcpy(hfq.got + hfq.n, f + 7, cnt);
	hfq.n += cnt;
	nc = f[7 + cnt];
	if ( nc > 0 && nc <= 16 && 8 + cnt + nc <= n ) {
		hf_ask(f + 8 + cnt, nc);
		return;
	}
	hf_done(TRUE);
}

/* The channel refused or closed before the answer: no hands-free service known */
EXPORT void knl_btsd_hf_closed( BTC *c, PEER *p )
{
	(void)c;
	if ( hfq.p == p ) {
		p->rcid_x[XI_SDP_HF] = 0;
		hf_done(FALSE);
	}
}

/* The controller gone: the question with it */
EXPORT void knl_btsd_gone( BTC *c )
{
	if ( hfq.c == c ) knl_memset(&hfq, 0, sizeof(hfq));
}

EXPORT void knl_btsd_tick( BTC *c )
{
	if ( hfq.p != NULL && hfq.c == c && (W)( knl_btx_now() - hfq.due ) >= 0 ) {
		tm_printf((UB *)"bt: \"%s\": its records did not answer about hands-free\n", hfq.p->name);
		hf_done(FALSE);
	}
}
