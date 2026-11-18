/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btaudio.h
 *	What the audio side of the Bluetooth driver shares among its files
 *	(design 6.8): bt_a2dp.c the stream to earphones and the channels of
 *	L2CAP the others ride on, bt_sdp.c this end's service records,
 *	bt_avrcp.c the remote control, bt_rfcomm.c the serial line under
 *	HFP, bt_hfp.c the hands-free profile, its voice link and the
 *	microphone. All of it runs in the controller's task, except what
 *	each says of the sound device's side.
 */

#ifndef __BTAUDIO_H__
#define __BTAUDIO_H__

/*
 * The ids of the outputs here: a word ("bt-", "le-"), the address's twelve
 * hex digits, its highest byte first, and perhaps a word after ("-hfp").
 * An id made fits in BTA_ID_ROOM, the longest of these forms with its 0;
 * an id asked is compared as long as it is (knl_bta_is_out_id).
 */
#define BTA_ID_ROOM		( 3 + 12 + 4 + 1 )
IMPORT void knl_bta_out_id( UB *id, CONST char *pre, CONST UB *addr, CONST char *post );
IMPORT BOOL knl_bta_is_out_id( CONST UB *id, CONST char *pre, CONST UB *addr, CONST char *post );

#define PSM_SDP_A		0x0001
#define PSM_RFCOMM		0x0003
#define PSM_AVCTP		0x0017
#define PSM_AVDTP_A		0x0019

#define L2_MTU_DEFAULT		672	/* what a channel takes when its configuration says nothing */

/* The index of a channel among the audio side's own */
#define XI(cid)			( (cid) - CID_X_FIRST )
#define XI_SDP_SRV		XI(CID_SDP_SRV)
#define XI_AVCTP		XI(CID_AVCTP)
#define XI_RFCOMM		XI(CID_RFCOMM)
#define XI_SDP_HF		XI(CID_SDP_HF)

/* bt_a2dp.c: the channels */
IMPORT void knl_bta_x_open( BTC *c, PEER *p, UH psm, UH cid );
IMPORT void knl_bta_x_close( BTC *c, PEER *p, UH cid );

/* bt_sdp.c */
#define RFC_CH_AG		1	/* this end's server channel of RFCOMM: the audio gateway */
IMPORT void knl_btsd_serve( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_btsd_hf_ask( BTC *c, PEER *p );
IMPORT void knl_btsd_hf_up( BTC *c, PEER *p );
IMPORT void knl_btsd_hf_in( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_btsd_hf_closed( BTC *c, PEER *p );
IMPORT void knl_btsd_tick( BTC *c );
IMPORT void knl_btsd_gone( BTC *c );

/* bt_avrcp.c */
IMPORT BOOL knl_btr_accept( BTC *c, PEER *p );
IMPORT void knl_btr_up( BTC *c, PEER *p );
IMPORT void knl_btr_in( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_btr_closed( BTC *c, PEER *p );
IMPORT void knl_btr_want( BTC *c, PEER *p );
IMPORT void knl_btr_down( BTC *c, PEER *p );
IMPORT void knl_btr_tick( BTC *c );
IMPORT void knl_btr_gone( BTC *c );
IMPORT BOOL knl_btr_absvol( void );
IMPORT void knl_btr_playing( BOOL on );

/* bt_rfcomm.c */
IMPORT BOOL knl_btf_accept( BTC *c, PEER *p );
IMPORT void knl_btf_up( BTC *c, PEER *p );
IMPORT void knl_btf_in( BTC *c, PEER *p, CONST UB *f, INT n );
IMPORT void knl_btf_closed( BTC *c, PEER *p );
IMPORT void knl_btf_open( BTC *c, PEER *p, UB ch );
#define RFC_TXQ_LIMIT		( 64 * 1024 )	/* what may wait for the earphones on RFCOMM (B) */
/*
 * Bytes to the earphones on the channel, kept until their credits and
 * their flow let them go: E_OBJ with no channel, E_BUSY while they take
 * nothing and RFC_TXQ_LIMIT bytes wait already, E_NOMEM with no room.
 * Nothing is let go of what was taken; it never waits.
 */
IMPORT ER   knl_btf_send( BTC *c, CONST UB *d, INT n );
IMPORT void knl_btf_down( BTC *c, PEER *p );
IMPORT void knl_btf_tick( BTC *c );
IMPORT void knl_btf_gone( BTC *c );
IMPORT BOOL knl_btf_busy( PEER *p );

/* bt_hfp.c */
IMPORT void knl_bth_hf_found( BTC *c, PEER *p, UB ch, UH features );
IMPORT void knl_bth_dlc_up( BTC *c, PEER *p );
IMPORT void knl_bth_at_in( BTC *c, PEER *p, CONST UB *d, INT n );
IMPORT void knl_bth_dlc_down( BTC *c, PEER *p );
IMPORT void knl_bth_event( BTC *c, UB code, CONST UB *d, INT len );
IMPORT void knl_bth_down( BTC *c, PEER *p );
IMPORT void knl_bth_gone( BTC *c );
IMPORT void knl_bth_tick( BTC *c );
IMPORT BOOL knl_bth_voice( void );		/* a voice link is up or being set up: A2DP stands aside */
IMPORT BOOL knl_bth_vol_outside( void );
/* the sound device's side, with the manager's lock held */
struct SNDOUTS;
IMPORT ER   knl_bth_outs( struct SNDOUTS *l );	/* its output added to l, if it has one */
IMPORT BOOL knl_bth_is_id( CONST UB *id );
IMPORT ER   knl_bth_play_start( CONST UB *id, UW *rate, INT *channels );
IMPORT void knl_bth_play_stop( void );
IMPORT BOOL knl_bth_play_running( void );
IMPORT BOOL knl_bth_can_record( void );
IMPORT ER   knl_bth_rec_start( UW rate, UW *dev_rate, INT *channels );
IMPORT void knl_bth_rec_stop( void );
IMPORT BOOL knl_bth_rec_running( void );
/* The earphones' microphone gain as a level of DN_SDVOLMIC: E_NOSPT when they take no remote volume, or have not said theirs */
IMPORT ER   knl_bth_mic_get( SDVol *v );
/* Set: told them by +VGM in the next round; E_NOSPT when they take no remote volume */
IMPORT ER   knl_bth_mic_set( CONST SDVol *v );

/* What the voice link has carried, for the tests */
typedef struct {
	UW	rx_frames;		/* samples of the microphone given to the sound device */
	UW	tx_packets;		/* SCO packets sent */
	UW	rx_packets;
	UB	alt;			/* the alternate setting of the voice interface */
	UH	handle;
	UB	codec;			/* of the last link up: 1 CVSD, 2 mSBC */
	UW	filled;			/* mSBC frames lost and made up */
	UW	bad;			/* mSBC frames whose check failed */
} T_BTHSTAT;
IMPORT void knl_bth_stat( T_BTHSTAT *s );

#endif /* __BTAUDIO_H__ */
