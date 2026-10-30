/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wsap_glue.h (the Wi-Fi simulator, WIFI_SIM=1)
 *	The access point's side of the WPA2-Personal handshakes (design
 *	9.6.12): what the simulated access point (wifisim_ap.c) calls of
 *	the authenticator's object (tools/wpa/build.sh ap, wpaap.o), and
 *	what that object calls back.
 *
 *	Plain C types only: the kernel's side includes this with the
 *	kernel's headers, the authenticator's side with wpa_supplicant's.
 */

#ifndef WSAP_GLUE_H
#define WSAP_GLUE_H

/* The ciphers, as net80211 numbers them (IEEE80211_CIPHER_*) */
#define WSAP_CIPHER_TKIP	1
#define WSAP_CIPHER_CCMP	3

/* ---------------------------------------------------------------- what the access point calls */

/*
 * The authenticator started for the network ssid with the passphrase
 * pass (8 to 63 characters) and the group cipher group (WSAP_CIPHER_*;
 * the pairwise one is CCMP), the access point's address own. The group
 * key is set at once (knl_wsap_setkey). group_rekey_s seconds between
 * new group keys, 0 none. 0, or -1 when it could not be started.
 */
int	knl_wsap_start(const unsigned char *own, const unsigned char *ssid, int ssid_len,
		       const char *pass, int pass_len, int group, int group_rekey_s);
void	knl_wsap_stop(void);

/* The access point's RSN element, as the authenticator makes it: its length, 0 none */
int	knl_wsap_ie(unsigned char *buf, int max);

/*
 * A station associated with the element ie (its RSN element of the
 * association request): its handshake begun. 0, or the reason code the
 * element is refused with.
 */
int	knl_wsap_join(const unsigned char *sta, const unsigned char *ie, int ie_len);

/* A station gone: its state machine let go */
void	knl_wsap_leave(const unsigned char *sta);

/* An EAPOL frame's body (the header of 802.1X on) from the station sta */
void	knl_wsap_rx(const unsigned char *sta, const unsigned char *body, int len);

/* The authenticator's timers (every few tens of ms) */
void	knl_wsap_tick(void);

/* Whether the station's port is open (the 4-way handshake done) */
int	knl_wsap_done(const unsigned char *sta);

/* ---------------------------------------------------------------- what it calls back */

/* An EAPOL frame's body to the station dst; 0 or -1 */
int	knl_wsap_eapol_tx(const unsigned char *dst, const unsigned char *body, int len);

/*
 * A key into the access point's net80211: for the station addr (the
 * pairwise key) or for the group (addr all ones, its index idx), with
 * its cipher (WSAP_CIPHER_*). 0 or -1
 */
int	knl_wsap_setkey(int cipher, const unsigned char *addr, int idx,
			const unsigned char *key, int key_len);

/* The station's port opened (1) or closed (0) */
void	knl_wsap_port(const unsigned char *sta, int authorized);

/* The authenticator sends the station away with the reason */
void	knl_wsap_kick(const unsigned char *sta, int reason);

/* One line of the authenticator's, the time in ms, random bytes, a stop on a fault */
void	knl_wsap_say(const char *line);
unsigned long long knl_wsap_ms(void);
void	knl_wsap_random(void *buf, int len);
void	knl_wsap_halt(void);

#endif /* WSAP_GLUE_H */
