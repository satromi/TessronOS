/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_drv.h (x64 PC, WIFI_NET80211=obsd)
 *	What the Wi-Fi interface (tswifi_obsd.c) asks of the driver of the
 *	card (design 9.6.9): iwm(4) for Intel's 7260 to 8265 and 9260/9560
 *	(tsw_iwm.c), iwx(4) for the Wi-Fi 6 AX200 and later (tsw_iwx.c).
 *	The driver is chosen by the card's PCI ID, each driver's own match
 *	saying whether it knows the card. Each of the files is compiled
 *	with OpenBSD's kernel headers and its driver's own; this one with
 *	OpenBSD's alone.
 */

#ifndef __TSW_DRV_H__
#define __TSW_DRV_H__

struct pci_attach_args;
struct ieee80211com;
struct ieee80211_node;
struct ieee80211_key;
struct ieee80211_frame;

struct tsw_drv {
	const char	*name;		/* the driver's: the interface is <name>0 */
	int		min_dbm;	/* the signal the driver gives net80211 is dBm less this */

	/* The card known by the driver (its PCI IDs) */
	int		(*match)(struct pci_attach_args *);

	/*
	 * The driver attached to the card as xname, and what it left until
	 * the root file system was there run (the firmware read, the card
	 * started once to read its NVM): its 802.11 interface, or NULL
	 */
	struct ieee80211com *(*attach)(struct pci_attach_args *, const char *xname);

	/* The time event that keeps the card on the channel while it joins is on */
	int		(*te_on)(void);

	/*
	 * Of the frame sent last, as the driver counts the card's answers:
	 * the tries its rate control saw (both NULL when the card's own
	 * rate control has them), and what waits in the queue of the
	 * management frames
	 */
	void		(*tx_counts)(u_int *tx, u_int *retry);
	int		(*mgmt_queued)(void);

	/* The card scans (a scan of its own or a background one) */
	int		(*scanning)(void);

	/*
	 * The pairwise CCMP key into the card, waiting for its answer, or
	 * NULL for the driver's own ic_set_key
	 */
	int		(*ptk_to_card)(struct ieee80211com *, struct ieee80211_node *,
			    struct ieee80211_key *);

	/* What the card wrote to a receive buffer, as the driver begins to read it (obsd_pci.c's dm_sync) */
	void		(*rx_peek)(bus_dmamap_t, bus_addr_t, bus_size_t, int);

	/* The card's own counts said, as its last statistics notification gave them */
	void		(*card_stats)(void);

	/* The keys the card holds said, as the port opens */
	void		(*port_say)(void);

	/* How much the driver says on the console */
	void		(*debug)(int);
};

extern const struct tsw_drv	tsw_iwm_drv;	/* tsw_iwm.c */
extern const struct tsw_drv	tsw_iwx_drv;	/* tsw_iwx.c */

/* ---------------------------------------------------------------- what tswifi_obsd.c gives the drivers */

/* The card's 802.11 interface once attached */
extern struct ieee80211com	*tsw_ic;

/* When the port opened (ns of getnsecuptime); 0 while it is not */
uint64_t	tsw_port_ns(void);

/*
 * The status the card writes after each frame it received (the
 * firmware's RX_MPDU_RES_STATUS, the same bits for iwm and iwx)
 */
#define TSW_RXS_CRC_OK		0x00000001
#define TSW_RXS_OVERRUN_OK	0x00000002
#define TSW_RXS_SRC_STA_FOUND	0x00000004
#define TSW_RXS_KEY_VALID	0x00000008
#define TSW_RXS_KEY_PARAM_OK	0x00000010
#define TSW_RXS_ICV_OK		0x00000020
#define TSW_RXS_MIC_OK		0x00000040
#define TSW_RXS_SEC_MSK		0x00000700
#define TSW_RXS_SEC_CCM		0x00000200
#define TSW_RXS_SEC_ERR		0x00000700
#define TSW_RXS_DEC_DONE	0x00000800
#define TSW_RXS_STA_ID_MSK	0x1f000000

/*
 * One MPDU the card wrote, its 802.11 header and length, its status and
 * the rate it came at (rate_n_flags): counted, the replays the driver
 * will drop seen ahead, the first protected ones to the station after
 * the port opened said
 */
void	tsw_rx_mpdu(const struct ieee80211_frame *, uint32_t flen, uint32_t st, uint32_t rate);

/* The card's answers to what was sent: 1 while the first of them after the port opened are to be said */
int	tsw_txr_say(void);

/* The card's answers to what was sent: 1 while the first of them during the handshake (associated, the port not yet open) are to be said */
int	tsw_txr_hs_say(void);

/* The size of a notification of the card's statistics against what the driver reads it as, said once */
void	tsw_stats_size(const char *drv, unsigned int got, unsigned int want, const char *api);

#endif /* __TSW_DRV_H__ */
