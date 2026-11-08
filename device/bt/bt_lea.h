/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bt_lea.h
 *	What the parts of LE Audio share (design 6.8): bt_iso.c, the
 *	controller's isochronous channels; bt_bap.c, the device's services of
 *	LE Audio read and driven by GATT; bt_lea.c, the streams of a device
 *	or of the two of a pair of earphones, their LC3 frames and the output
 *	of the sound device. All of it runs in the controller's task but the
 *	encoding, which has a task of its own (bt_lea.c).
 */

#ifndef __BT_LEA_H__
#define __BT_LEA_H__

#define LEA_NASE	4		/* the sink ASEs of one device that are taken */
#define LEA_NSTREAM	2		/* the streams of the group, a CIS each */
#define LEA_OCT_MAX	155		/* the longest LC3 frame asked for, of 48 kHz in 10 ms */
#define LEA_SDU_MAX	( 2 * LEA_OCT_MAX )

/* The states of an ASE (ASCS 4.1) */
#define ASE_IDLE	0x00
#define ASE_CODEC	0x01
#define ASE_QOS		0x02
#define ASE_ENABLING	0x03
#define ASE_STREAMING	0x04
#define ASE_DISABLING	0x05
#define ASE_RELEASING	0x06

/* Audio locations (Assigned Numbers 6.12.1) */
#define LOC_FL		0x00000001U
#define LOC_FR		0x00000002U

/* A sink ASE of the device */
typedef struct {
	UB	id;
	UB	state;		/* ASE_*, 0xff not read yet */
	UH	val, cccd;
	/* what the server prefers, said in the Codec Configured state */
	UB	framing;	/* 0 unframed PDUs supported, 1 not */
	UB	phy;		/* the PHYs it prefers, as a bit field */
	UB	rtn;
	UH	latency;	/* ms */
	UW	pd_min, pd_max, ppd_min, ppd_max;	/* us */
	/* the QoS Configured state and after */
	UB	cig, cis;
} LEASE;

/* What is known of a device that serves LE Audio */
typedef struct {
	BOOL	ready;		/* its services read, its notifications on */
	BOOL	failed;		/* it refused what was asked of it: not asked again on this link */
	PEER	*p;
	/* PACS: its sink's LC3 record */
	BOOL	lc3;
	UH	freqs;		/* the sampling frequencies, a bit each */
	UB	durs;		/* the frame durations */
	UB	chans;		/* the channel counts: bit n for n + 1 */
	UH	oct_min, oct_max;
	UB	frames_max;	/* codec frames in one SDU */
	UW	sink_loc;	/* LOC_* */
	/* ASCS */
	INT	nase;
	LEASE	ase[LEA_NASE];
	UH	cp;
	/* CSIS: the set of earphones it is one of */
	BOOL	in_set;
	UB	sirk[16];
	UB	set_size, rank;
	/* VCS */
	UH	vol_cp;
	UB	vol, mute, vol_count;
} LEADEV;

/* bt_iso.c: the commands of a CIG and its CISes, and the data that goes on them */
IMPORT void knl_btiso_cig( BTC *c, UW interval, UB framing, UH latency, INT n,
			   CONST UB *cis_id, CONST UH *max_sdu, CONST UB *phy, CONST UB *rtn );
IMPORT void knl_btiso_cig_remove( BTC *c );
IMPORT void knl_btiso_cis_create( BTC *c, INT n, CONST UH *cis, CONST UH *acl );
IMPORT void knl_btiso_path( BTC *c, UH cis );
IMPORT void knl_btiso_drop( BTC *c, UH cis );
IMPORT BOOL knl_btiso_send( BTC *c, UH cis, UH seq, CONST UB *sdu, INT n );

/* bt_bap.c: the devices, and what is asked of them */
IMPORT LEADEV *knl_btbap_dev( BTC *c, INT i );	/* NPEER of them, NULL where none */
IMPORT void knl_btbap_codec( BTC *c, LEADEV *d, INT ai, UB freq, UW alloc, UH octets );
IMPORT void knl_btbap_qos( BTC *c, LEADEV *d, INT ai, UB cig, UB cis, UW interval, UB framing,
			   UB phy, UH max_sdu, UB rtn, UH latency, UW pd );
IMPORT void knl_btbap_enable( BTC *c, LEADEV *d, INT ai );
IMPORT void knl_btbap_disable( BTC *c, LEADEV *d, INT ai );
IMPORT void knl_btbap_volume( BTC *c, LEADEV *d, UB vol );
IMPORT void knl_btbap_sef( CONST UB *k, CONST UB *sirk, UB *out );
IMPORT void knl_btbap_tick( BTC *c );

/* bt_lea.c: what the other two tell */
IMPORT void knl_btlea_dev_ready( BTC *c, LEADEV *d );
IMPORT void knl_btlea_dev_down( BTC *c, LEADEV *d );
IMPORT void knl_btlea_ase( BTC *c, LEADEV *d, INT ai );
IMPORT void knl_btlea_cp_fail( BTC *c, LEADEV *d, INT ai, UB op, UB code, UB reason );
IMPORT void knl_btlea_cig_done( BTC *c, INT st, INT n, CONST UH *handles );
IMPORT void knl_btlea_cis_up( BTC *c, UH h, INT st );
IMPORT void knl_btlea_cis_failed( BTC *c, INT st );
IMPORT void knl_btlea_path_done( BTC *c, UH h, INT st );
IMPORT void knl_btlea_cis_down( BTC *c, UH h, INT reason );
IMPORT void knl_btlea_cig_removed( BTC *c, INT st );

#endif /* __BT_LEA_H__ */
