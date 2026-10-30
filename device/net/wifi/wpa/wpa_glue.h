/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpa_glue.h (x64 PC)
 *	Between the Wi-Fi manager (wifi.c) and the WPA2 handshakes of
 *	wpa_supplicant (wpa_glue.c, tools/wpa/build.sh).
 *
 *	wpa_supplicant's headers cannot meet T-Kernel's, so what crosses is
 *	in plain C types: bytes, ints. An address is 6 bytes, an SSID up to
 *	32, a pairwise master key 32.
 */

#ifndef __WPA_GLUE_H__
#define __WPA_GLUE_H__

/* ---------------------------------------------------------------- what the manager calls */

/* The security a network offers, from its elements and its capability field (privacy) */
#define WPAG_OPEN	0		/* none */
#define WPAG_WEP	1		/* WEP (not joined) */
#define WPAG_WPA	2		/* WPA without RSN (not joined) */
#define WPAG_WPA2	3		/* WPA2-Personal (PSK) without SAE beside it */
#define WPAG_WPA3	4		/* WPA3-Personal only (SAE: not joined) */
#define WPAG_8021X	5		/* WPA2-Enterprise (not joined) */

extern int	knl_wpa_classify( const unsigned char *ies, int ies_len, int privacy );

/*
 * The pairwise master key of a passphrase (8 to 63 characters, PBKDF2
 * with the SSID) or of 64 hexadecimal digits; 0, or -1 for one that
 * is neither
 */
extern int	knl_wpa_psk( const char *pass, int pass_len, const unsigned char *ssid,
			     int ssid_len, unsigned char *pmk );

/*
 * Ready for a network of WPA2-Personal: our address, the access point's,
 * the SSID, the RSN element the access point sent, the key. The RSN
 * element to put in the association request is written to ie (up to
 * 64 bytes) and its length into *ie_len. 0, or -1 when the network
 * offers nothing of WPA2-Personal this can join.
 */
extern int	knl_wpa_start( const unsigned char *own, const unsigned char *bssid,
			       const unsigned char *ssid, int ssid_len,
			       const unsigned char *ap_ie, int ap_ie_len,
			       const unsigned char *pmk, unsigned char *ie, int *ie_len );

/* ---------------------------------------------------------------- WPA3, PMF (design 9.6.9) */

/*
 * The security a saved network is joined with (the settings' word):
 * WPA2-Personal, WPA3-Personal (SAE), or either (the access point
 * decides: SAE when it offers SAE, else PSK).
 */
#define WPAG_SAVED_WPA2		1
#define WPAG_SAVED_WPA3		2
#define WPAG_SAVED_WPA2WPA3	3

/* WPAG_* of knl_wpa_classify for an access point offering both PSK and SAE (WPA3 transition mode) */
#define WPAG_WPA2WPA3	6

/* The key management of a join */
#define WPAG_AKM_PSK		2	/* 00-0F-AC:2 */
#define WPAG_AKM_PSK_SHA256	6	/* 00-0F-AC:6 */
#define WPAG_AKM_SAE		8	/* 00-0F-AC:8 */

/* The protection of management frames (802.11w) asked for */
#define WPAG_PMF_OFF		0
#define WPAG_PMF_OPTIONAL	1	/* MFPC: used when the access point can */
#define WPAG_PMF_REQUIRED	2	/* MFPC and MFPR: WPA3 always */

/*
 * How a saved network is joined through an access point: from the
 * saved security (WPAG_SAVED_*), the access point's RSN element and
 * RSNXE (NULL when it sent none), whether the card can protect
 * management frames (card_pmf) and whether an H2E password element is
 * kept (have_pt). Answers 0 with the key management (*akm), the
 * protection (*pmf) and whether SAE goes by H2E (*h2e), or -1 when the
 * two have nothing in common (an access point of WPA3 only for a network
 * saved as WPA2, one that requires PMF of a card without it).
 */
extern int	knl_wpa_choose( int saved, const unsigned char *ap_rsn, int ap_rsn_len,
				const unsigned char *ap_rsnxe, int ap_rsnxe_len, int card_pmf,
				int have_pt, int *akm, int *pmf, int *h2e );

/*
 * Ready for the 4-way handshake of a join as knl_wpa_choose decided,
 * with the access point's RSN element and RSNXE (NULL, 0 when it sent
 * none: message 3 must carry the same ones): the key management
 * (WPAG_AKM_*), the protection (WPAG_PMF_*), the
 * PMK (32 bytes: of the passphrase, or SAE's) and, for SAE, its PMKID
 * (16 bytes, put in our RSN element), and h2e (our RSNXE says
 * SAE-H2E). Our RSN element (and RSNXE after it, for H2E) for the
 * association request is written to ie (up to WPAG_IE_MAX bytes) and
 * its length into *ie_len. 0, or -1.
 */
#define WPAG_IE_MAX	96

/*
 * The element knl_wpa_start2 writes is the only RSN element of a join:
 * the manager puts it (and the RSNXE after it, for H2E) in the
 * association request as it is -- on the x64 PC the layer of OpenBSD's
 * net80211 puts it in place of its own (ic_send_mgmt), on the Raspberry
 * Pi 5 the chip is given it -- and message 2 of the 4-way handshake
 * carries the same bytes, which the access point compares with the
 * request's. The access point's RSN element and RSNXE given to
 * knl_wpa_choose and here are those of its beacon or probe response.
 *
 * After SAE: knl_wpa_sae_rx answered WPAG_SAE_DONE, knl_wpa_sae_pmk
 * gives the PMK and PMKID, and knl_wpa_start2 is called with
 * WPAG_AKM_SAE, WPAG_PMF_REQUIRED, SAE's PMK and PMKID: the element it
 * writes names SAE (00-0F-AC:8), MFPC and MFPR, and the PMKID.
 */
extern int	knl_wpa_start2( const unsigned char *own, const unsigned char *bssid,
				const unsigned char *ssid, int ssid_len,
				const unsigned char *ap_rsn, int ap_rsn_len,
				const unsigned char *ap_rsnxe, int ap_rsnxe_len, int akm, int pmf,
				const unsigned char *pmk, const unsigned char *pmkid, int h2e,
				unsigned char *ie, int *ie_len );

/*
 * SAE (WPA3-Personal): the authentication frames before the
 * association, with wpa_supplicant's src/common/sae.c. The manager
 * sends and receives the frames (authentication algorithm 3); what
 * crosses here is the frame's body after its algorithm, transaction
 * sequence number and status code: the SAE fields (group, scalar,
 * element, token; send-confirm and confirm).
 *
 * knl_wpa_sae_pt: H2E's password element of a network (group 19, the
 * elliptic curve P-256) from its SSID and password: written to pt (up
 * to WPAG_SAE_PT_MAX bytes, x and y), its length into *pt_len. Kept
 * instead of the password when the access points of the network use
 * H2E. 0, or -1.
 *
 * knl_wpa_sae_start: SAE begun with bssid: by H2E from pt, or by
 * hunting and pecking from the password. The first Commit is written to
 * out (up to WPAG_SAE_MAX bytes) with its length (*out_len), the status
 * code to send with it (*out_status: 0, or 126 for H2E) and its
 * sequence number 1. 0, or -1.
 *
 * knl_wpa_sae_rx: an authentication frame of SAE from the access point
 * (seq 1 Commit, 2 Confirm, its status code and body). Answers
 * WPAG_SAE_SEND with a frame to send back in out (*out_len, *out_seq,
 * *out_status) -- our Confirm, or our Commit again with the
 * anti-clogging token the access point asked for, or with the next
 * group --, WPAG_SAE_WAIT with nothing to send, WPAG_SAE_DONE when the
 * access point's Confirm is right (the PMK and PMKID are then ready),
 * WPAG_SAE_FAIL when it is given up (its Confirm is wrong -- the password
 * --, no group in common, a frame that cannot be read).
 *
 * knl_wpa_sae_resend: the last frame sent, again (no answer in time):
 * as knl_wpa_sae_rx's out; WPAG_SAE_SEND, or WPAG_SAE_FAIL when there
 * is none or it was sent too often.
 *
 * knl_wpa_sae_pmk: the PMK (32 bytes) and PMKID (16) of the SAE just
 * accepted, for knl_wpa_start2. 0, or -1.
 *
 * knl_wpa_sae_abort: what SAE holds let go (the password's element and
 * the keys cleared).
 */
#define WPAG_SAE_MAX		512
#define WPAG_SAE_PT_MAX		132
#define WPAG_SAE_SEND		1
#define WPAG_SAE_WAIT		0
#define WPAG_SAE_DONE		2
#define WPAG_SAE_FAIL		(-1)
#define WPAG_SAE_STATUS_H2E	126	/* WLAN_STATUS_SAE_HASH_TO_ELEMENT */

extern int	knl_wpa_sae_pt( const unsigned char *ssid, int ssid_len, const char *pass, int pass_len,
				unsigned char *pt, int *pt_len );
extern int	knl_wpa_sae_start( const unsigned char *own, const unsigned char *bssid, int h2e,
				   const char *pass, int pass_len, const unsigned char *pt, int pt_len,
				   unsigned char *out, int *out_len, int *out_status );
extern int	knl_wpa_sae_rx( int seq, int status, const unsigned char *body, int len,
				unsigned char *out, int *out_len, int *out_seq, int *out_status );
extern int	knl_wpa_sae_resend( unsigned char *out, int *out_len, int *out_seq, int *out_status );
extern int	knl_wpa_sae_pmk( unsigned char *pmk, unsigned char *pmkid );
extern void	knl_wpa_sae_abort( void );

/* The card is associated with bssid: the handshake may begin */
extern void	knl_wpa_assoc( const unsigned char *bssid );

/* An EAPOL frame from src (its body, after the Ethernet header) */
extern void	knl_wpa_rx( const unsigned char *src, const unsigned char *body, int len );

/* Where the handshake is */
#define WPAG_NONE	0		/* nothing begun */
#define WPAG_HANDSHAKE	1		/* the 4-way or the group key handshake */
#define WPAG_DONE	2		/* the keys are set: data may pass */
extern int	knl_wpa_state( void );

/* The association is gone */
extern void	knl_wpa_disassoc( void );

/* The timers of the state machine, run from the manager's loop */
extern void	knl_wpa_tick( void );

/* ---------------------------------------------------------------- what the glue calls (wifi.c) */

/* An EAPOL frame body out to dst (the Ethernet header is put on by the manager) */
extern int	knl_wifi_eapol_tx( const unsigned char *dst, const unsigned char *body, int len );

/*
 * A key set: cipher 1 TKIP, 3 CCMP, 6 BIP-CMAC-128; addr all ones for
 * the group's; 0 or -1. The integrity group key of protected
 * management frames (IGTK, from the IGTK KDE of message 3 or of the
 * group key handshake) comes as cipher 6, idx 4 or 5, addr all ones,
 * set_tx 0, seq the IPN (6 bytes, least significant first, as the KDE
 * carries it) and key its 16 bytes.
 */
#define WPAG_CIPHER_TKIP	1
#define WPAG_CIPHER_CCMP	3
#define WPAG_CIPHER_BIP		6
extern int	knl_wifi_setkey( int cipher, const unsigned char *addr, int idx, int set_tx,
				 const unsigned char *seq, int seq_len,
				 const unsigned char *key, int key_len );

/* A key taken away: the pairwise one of addr, or the group's of idx (addr NULL) */
extern int	knl_wifi_delkey( const unsigned char *addr, int idx );

/* Leave the network, telling the access point why */
extern void	knl_wifi_deauth( int reason );

/* One line of what the handshake says, for the console */
extern void	knl_wifi_say( const char *line );

/* The calling task stopped for good (what wpa_supplicant's abort comes to) */
extern void	knl_wifi_halt( void );

/* Milliseconds since the start, and random bytes */
extern unsigned long long knl_wifi_ms( void );
extern void	knl_wifi_random( void *buf, int len );

#endif /* __WPA_GLUE_H__ */
