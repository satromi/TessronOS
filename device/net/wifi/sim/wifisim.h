/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifisim.h (the Wi-Fi simulator, WIFI_SIM=1)
 *	What the tests see of the simulated access point (wifisim_ap.c,
 *	design 9.6.12).
 */

#ifndef WIFISIM_H
#define WIFISIM_H

/* The network as the simulator starts it */
#define WIFISIM_SSID	"TessronOS-Sim"
#define WIFISIM_PASS	"wifisim-pass-2026"
#define WIFISIM_AP_IP	0xc0a84d01UL		/* 192.168.77.1: the router, the name server */
#define WIFISIM_POOL_IP	0xc0a84d64UL		/* 192.168.77.100: the first address DHCP gives */
#define WIFISIM_DOMAIN	"wifisim.test"

typedef struct {
	UW	frames_in, frames_out, send_err, rx_full;
	UW	eapol_in, eapol_out, ptk_set, handshakes, kicks;
	UW	joins, leaves;
	UW	arp, ip, ping, echo, echo_bytes, dns, other;
	UW	dhcp_discover, dhcp_offer, dhcp_request, dhcp_ack, dhcp_nak;
	UB	dhcp_chaddr[6];		/* the last DHCP message's client address (chaddr) */
	UB	dhcp_src[6];		/* and the address its frame came from */
	BOOL	ready;			/* the network is up */
	INT	open_stas;		/* stations whose port is open */
	UW	air[20];		/* the air's counts (tswifi_sim.c, obsd_sim.c: rump_tswifisim_stats) */
} WIFISIM_COUNTS;

/* The air's counts, by their place in air[] */
#define WSIM_AIR_STA_UNAUTH	9	/* the station's data frames dropped for its port not open */
#define WSIM_AIR_AP_UNAUTH	10
#define WSIM_AIR_BEACONS	6
#define WSIM_AIR_LOST_BGSCAN	14	/* lost while a background scan had the station away (OpenBSD's layer) */
#define WSIM_AIR_BGSCANS	15	/* background scans the station began (OpenBSD's layer) */
#define WSIM_AIR_TX_BA		16	/* block-ack agreements for what the station sends (OpenBSD's layer) */
#define WSIM_AIR_AP_RX_BA	17	/* the same, as the access point counts them */
#define WSIM_AIR_AP_OLD		18	/* the station's frames the access point dropped as old or duplicate */
#define WSIM_AIR_AP_SEQSHARE	19	/* dropped by its one record of the numbers ("seqshare on") */

IMPORT ER	knl_wifisim_ctl( CONST char *cmd );
IMPORT void	knl_wifisim_counts( WIFISIM_COUNTS *c );
IMPORT void	knl_wifisim_ap_start( void );

#endif /* WIFISIM_H */
