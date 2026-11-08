/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Copyright 2023 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2026 satromi
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 *	athxvar.h (x64 PC, WIFI_NET80211=obsd)
 *	The state of athx, the driver of Qualcomm's QCA6174 hw3.x on PCI
 *	(design 9.6.9), shared by its files:
 *
 *	- athx_pci.c: the window of registers, the copy engines (CE) that
 *	  carry everything between the host and the chip, the diagnostic
 *	  window into the chip's memory, the bootloader's message exchange,
 *	  the chip's resets and its interrupt;
 *	- athx_fw.c: the firmware and board files, the bootloader's commands
 *	  (BMI), the firmware's download and the order the chip is brought up
 *	  in, and the host-target communication (HTC) over the copy engines;
 *	- athx_wmi.c: the commands and events of the firmware's control
 *	  interface (WMI, in its TLV form);
 *	- athx_htt.c: the transport of the data frames (HTT): the receive ring
 *	  of the chip's MAC, the frames sent and their completions;
 *	- athx.c: OpenBSD's net80211 above it all: the interface, its states,
 *	  scans, joins, keys and frames, and the channels the regulatory domain
 *	  of the chip allows.
 *
 *	Everything runs under the layer's one lock (the kernel lock of OpenBSD
 *	as the layer gives it); a wait for the chip sleeps with tsleep_nsec,
 *	which lets the lock go, so that the interrupt's thread can run.
 *	Every buffer the chip reads or writes is memory from bus_dmamem_alloc,
 *	whole pages below 4GB (the chip reaches 32 bits of address): frames
 *	are copied in and out of it, no mbuf is given to the chip.
 */

#ifndef __ATHXVAR_H__
#define __ATHXVAR_H__

/* ---------------------------------------------------------------- memory the chip reaches */

struct athx_dma {
	bus_dmamap_t		map;
	bus_dma_segment_t	seg;
	caddr_t			kva;
	bus_size_t		size;
	bus_addr_t		paddr;
};

/* A number of equal buffers, in pieces of ATHX_CHUNK bytes */
#define ATHX_CHUNK	(64 * 1024)

struct athx_bufs {
	int		n;		/* how many */
	int		size;		/* each, a power of 2 no larger than ATHX_CHUNK */
	int		per;		/* in each piece */
	int		npieces;
	struct athx_dma	*piece;
};

static inline void *
athx_buf_va(struct athx_bufs *b, int i)
{
	return b->piece[i / b->per].kva + (i % b->per) * b->size;
}

static inline uint32_t
athx_buf_pa(struct athx_bufs *b, int i)
{
	return (uint32_t)b->piece[i / b->per].paddr + (i % b->per) * b->size;
}

/* ---------------------------------------------------------------- the copy engines */

struct athx_softc;
struct athx_ce;

struct athx_ce_ring {
	unsigned int		nentries;
	unsigned int		nentries_mask;
	unsigned int		sw_index;	/* the next to be completed (src) or read (dest) by the host */
	unsigned int		write_index;	/* the next the host writes */
	unsigned int		hw_index;	/* the chip's read index, as last read */
	struct athx_dma		mem;		/* the descriptors */
	struct ce_desc		*base;		/* aligned to CE_DESC_RING_ALIGN */
	uint32_t		base_paddr;
	void			**ctx;		/* what each entry carries */
	struct athx_bufs	bufs;		/* src: a buffer per entry; dest: the buffers posted */
};

/* The host's configuration of one engine */
struct athx_ce_attr {
	uint32_t	flags;
	unsigned int	src_nentries;
	unsigned int	src_sz_max;
	unsigned int	dest_nentries;
	void		(*send_cb)(struct athx_softc *, struct athx_ce *);
	void		(*recv_cb)(struct athx_softc *, struct athx_ce *);
};

struct athx_ce {
	int			id;
	uint32_t		ctrl_addr;
	uint32_t		attr_flags;
	unsigned int		src_sz_max;
	struct athx_ce_ring	*src;
	struct athx_ce_ring	*dest;
	unsigned int		buf_sz;		/* of the receive buffers */
	void			(*send_cb)(struct athx_softc *, struct athx_ce *);
	void			(*recv_cb)(struct athx_softc *, struct athx_ce *);
};

/* A message the chip sent to the host, as an engine received it */
struct athx_rxbuf {
	uint8_t		*data;
	int		len;
};

/* ---------------------------------------------------------------- HTC */

#define ATHX_HTC_EP_COUNT	9

struct athx_htc_ep {
	uint16_t	service_id;	/* 0: not connected */
	int		ul_pipe, dl_pipe;
	int		tx_credits;
	int		tx_credit_size;
	int		credit_flow;	/* sending takes credits */
	uint8_t		seq_no;
	int		max_msg_len;
	void		(*rx)(struct athx_softc *, uint8_t *, int);
};

/* ---------------------------------------------------------------- WMI */

#define ATHX_WMI_SVC_WORDS	32	/* the TLV service bitmap: up to 1024 services */

struct athx_mem_chunk {
	struct athx_dma	mem;
	uint32_t	len;
	uint32_t	req_id;
};

/* ---------------------------------------------------------------- HTT */

#define ATHX_TX_NBUF	256		/* frames sent and not yet completed, at most */
#define ATHX_TX_BUFSZ	2048		/* the copy of one frame */
#define ATHX_HTT_CTL_N	32		/* HTT's own messages in flight */
#define ATHX_HTT_CTL_SZ	256

struct athx_htt {
	int		eid;
	int		ver_major, ver_minor;
	int		ver_received;

	/* the MAC's receive ring: an address per entry, the index the host fills */
	struct athx_dma	ring;		/* HTT_RX_RING_SIZE addresses */
	struct athx_dma	alloc_idx;	/* the host's write index, read by the chip */
	struct athx_bufs bufs;		/* HTT_RX_RING_SIZE buffers of HTT_RX_BUF_SIZE */
	int		size, size_mask;
	int		fill_level, fill_cnt;
	int		sw_rd_idx;
	int		*ring_buf;	/* the buffer each entry holds, -1 none */
	int		*free_buf;	/* buffers not on the ring */
	int		nfree;
	int		mpdus_ready;
	int		confused;	/* the ring went out of step: nothing more is read */

	/* frames sent: the HTC/HTT header and fragment list, and the frame's copy */
	struct athx_dma	txbuf;		/* ATHX_TX_NBUF struct ath10k_htt_txbuf_32 */
	struct athx_bufs txdata;	/* ATHX_TX_NBUF buffers of ATHX_TX_BUFSZ */
	uint8_t		tx_busy[ATHX_TX_NBUF];
	int		tx_pending;

	/* HTT's own messages (version, ring setup, aggregation) */
	struct athx_bufs ctl;
	uint8_t		ctl_busy[ATHX_HTT_CTL_N];

	/* what came, for the record */
	uint32_t	n_rx_ind, n_rx_msdu, n_rx_drop, n_tx_compl, n_tx_noack, n_tx_discard;
	uint32_t	n_addba, n_delba, n_peer_map;
};

/* ---------------------------------------------------------------- the driver */

/* The scan of the firmware, as the host sees it */
enum athx_scan_state {
	ATHX_SCAN_IDLE,
	ATHX_SCAN_STARTING,
	ATHX_SCAN_RUNNING,
	ATHX_SCAN_ABORTING,
};

/* How far the chip has come up: what KERNEL.TXT says when it stops */
enum athx_stage {
	ATHX_ST_NONE,
	ATHX_ST_PCI,		/* the window mapped, the card claimed */
	ATHX_ST_RESET,		/* the cold and warm resets done, the target initialised */
	ATHX_ST_PIPES,		/* the copy engines set up, their configuration in the target */
	ATHX_ST_BMI,		/* the bootloader answers: the target's version */
	ATHX_ST_FILES,		/* the firmware and board files read and chosen */
	ATHX_ST_DOWNLOAD,	/* board data, OTP and the firmware in the target */
	ATHX_ST_HTC,		/* HTC ready, its services connected */
	ATHX_ST_WMI,		/* the firmware's service ready and ready events */
	ATHX_ST_HTT,		/* HTT's version agreed, the receive ring given */
	ATHX_ST_UP,		/* running: its address known */
};

struct athx_softc {
	struct device		sc_dev;
	struct ieee80211com	sc_ic;

	int			(*sc_newstate)(struct ieee80211com *, enum ieee80211_state, int);

	/* the bus */
	bus_dma_tag_t		sc_dmat;
	bus_space_tag_t		sc_st;
	bus_space_handle_t	sc_sh;
	bus_size_t		sc_sz;
	pci_chipset_tag_t	sc_pc;
	pcitag_t		sc_tag;
	void			*sc_ih;
	uint16_t		sc_vendor, sc_device, sc_subvendor, sc_subdevice;
	uint32_t		chip_id;
	uint32_t		target_version;
	enum athx_stage		stage;
	int			wedged;		/* the firmware crashed or the device is gone */

	/* the copy engines */
	struct athx_ce		ce[CE_COUNT];
	struct athx_ce_attr	attr[CE_COUNT];
	struct ce_pipe_config	pipe_config[NUM_TARGET_CE_CONFIG_WLAN];
	struct ce_service_to_pipe serv_to_pipe[18];
	int			nserv_to_pipe;
	struct athx_dma		diag_buf;	/* the bounce buffer of the diagnostic window */

	/* the bootloader */
	int			bmi_done_sent;
	struct athx_dma		bmi_req, bmi_resp;

	/* the files */
	u_char			*fw_file;	/* firmware-6.bin */
	size_t			fw_file_len;
	const uint8_t		*fw_image, *otp_image;
	size_t			fw_image_len, otp_image_len;
	char			fw_version[64];
	uint8_t			fw_features[4];
	uint32_t		wmi_op_version, htt_op_version;
	u_char			*bd_file;	/* board-2.bin */
	size_t			bd_file_len;
	const uint8_t		*board_data;
	size_t			board_len;
	char			board_name[100];
	int			bmi_ids_valid;
	uint32_t		bmi_board_id, bmi_chip_id;
	int			ext_bid_supported;

	/* HTC */
	struct athx_htc_ep	ep[ATHX_HTC_EP_COUNT];
	int			htc_total_credits;
	int			htc_credit_size;
	int			htc_alt_data_credit_size;
	uint8_t			ctl_resp[ATH10K_HTC_MAX_CTRL_MSG_LEN];
	int			ctl_resp_len;
	int			ctl_resp_done;

	/* WMI */
	int			wmi_eid;
	uint32_t		svc_map[ATHX_WMI_SVC_WORDS];
	int			service_ready, unified_ready;
	uint32_t		hw_min_tx_power, hw_max_tx_power;
	uint32_t		ht_cap_info, vht_cap_info, vht_supp_mcs;
	uint32_t		phy_capability, num_rf_chains, sys_cap_info;
	uint32_t		eeprom_rd;
	uint32_t		low_2ghz_chan, high_2ghz_chan, low_5ghz_chan, high_5ghz_chan;
	uint32_t		sw_ver0, sw_ver1;
	struct athx_mem_chunk	mem_chunks[WMI_MAX_MEM_REQS];
	int			num_mem_chunks;
	uint8_t			mac_addr[ETH_ALEN];
	int			vdev_setup_done;
	int			last_vdev_start_status;
	int			install_key_done;
	int			peer_mapped;
	uint8_t			peer_wait_addr[ETH_ALEN];
	int			mgmt_tx_wmi;	/* management frames by WMI, not by HTT */
	uint32_t		n_wmi_ev, n_wmi_cmd, n_mgmt_rx;

	/* HTT */
	struct athx_htt		htt;

	/* the regulatory domain */
	uint16_t		reg_domain, reg_5ghz_ctl, reg_2ghz_ctl;
	char			alpha2[3];

	/* the interface */
	int			vdev_id;
	int			vdev_created, vdev_started, vdev_up, bss_peer;
	uint8_t			bss_addr[ETH_ALEN];
	enum athx_scan_state	scan_state;
	int			scan_started_done;
	struct timeout		scan_to;

	/* the tasks that do what may sleep, out of net80211's way */
	struct task		newstate_task;
	struct task		setkey_task;
	struct task		bmiss_task;
	enum ieee80211_state	ns_nstate;
	int			ns_arg;
	int			ns_pending;
	struct {
		struct ieee80211_node	*ni;
		struct ieee80211_key	*k;
		int			add;
	}			setkey_arg[8];
	int			setkey_cur, setkey_tail, setkey_nkeys;
	int			sc_tx_timer;
	int			debug;
};

/* The firmware's scan ID, and the noise floor its signals are given above */
#define ATHX_SCAN_ID		0
#define ATHX_NOISE_FLOOR	(-95)

/* The firmware keeps the native Wi-Fi header unpadded to 4 bytes (its feature bit) */
#define fw_feature_no_nwifi_pad(sc) 	((sc)->fw_features[ATH10K_FW_FEATURE_NO_NWIFI_DECAP_4ADDR_PADDING / 8] & 	 (1 << (ATH10K_FW_FEATURE_NO_NWIFI_DECAP_4ADDR_PADDING % 8)))

#define ATHX_DPRINTF(sc, ...)	do { if ((sc)->debug) printf(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- athx_pci.c */

uint32_t athx_read32(struct athx_softc *, uint32_t);
void	athx_write32(struct athx_softc *, uint32_t, uint32_t);
uint32_t athx_soc_read32(struct athx_softc *, uint32_t);
void	athx_soc_write32(struct athx_softc *, uint32_t, uint32_t);
int	athx_dma_alloc(struct athx_softc *, struct athx_dma *, bus_size_t, bus_size_t);
void	athx_dma_free(struct athx_softc *, struct athx_dma *);
int	athx_bufs_alloc(struct athx_softc *, struct athx_bufs *, int n, int size);
void	athx_bufs_free(struct athx_softc *, struct athx_bufs *);
int	athx_pci_attach(struct athx_softc *, struct pci_attach_args *);
int	athx_pci_power_up(struct athx_softc *);
int	athx_pci_start(struct athx_softc *);
void	athx_pci_stop(struct athx_softc *);
int	athx_diag_read_mem(struct athx_softc *, uint32_t, void *, int);
int	athx_diag_write_mem(struct athx_softc *, uint32_t, const void *, int);
int	athx_diag_read32(struct athx_softc *, uint32_t, uint32_t *);
int	athx_bmi_exchange(struct athx_softc *, const void *, uint32_t, void *, uint32_t *);
int	athx_ce_send_sg(struct athx_softc *, int pipe, const uint32_t *paddr, const uint16_t *len,
	    int n, void *ctx, unsigned int transfer_id);
void	*athx_ce_src_buf(struct athx_softc *, int pipe, uint32_t *paddr);
int	athx_ce_free_src(struct athx_softc *, int pipe);
void	athx_ce_service(struct athx_softc *, int pipe);
void	athx_ce_poll_all(struct athx_softc *);
int	athx_map_service_to_pipe(struct athx_softc *, uint16_t, int *ul, int *dl);
int	athx_intr(void *);
void	athx_say_regs(struct athx_softc *, const char *why);

/* ---------------------------------------------------------------- athx_fw.c */

int	athx_boot(struct athx_softc *);
int	athx_htc_send(struct athx_softc *, int eid, const void *, int len);
void	athx_htc_rx(struct athx_softc *, struct athx_ce *, uint8_t *, int);
int	athx_htc_connect(struct athx_softc *, uint16_t service_id,
	    void (*rx)(struct athx_softc *, uint8_t *, int), int *eid);
int	athx_wait(struct athx_softc *, volatile int *, const char *, int msec);
void	athx_free_files(struct athx_softc *);

/* ---------------------------------------------------------------- athx_wmi.c */

int	athx_wmi_connect(struct athx_softc *);
int	athx_wmi_init(struct athx_softc *);
int	athx_wmi_svc(struct athx_softc *, int);
int	athx_wmi_pdev_set_param(struct athx_softc *, uint32_t, uint32_t);
int	athx_wmi_pdev_set_regdomain(struct athx_softc *, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t);
int	athx_wmi_scan_chan_list(struct athx_softc *);
int	athx_wmi_vdev_create(struct athx_softc *, int vdev, uint32_t type, uint32_t subtype, const uint8_t *);
int	athx_wmi_vdev_delete(struct athx_softc *, int vdev);
int	athx_wmi_vdev_set_param(struct athx_softc *, int vdev, uint32_t, uint32_t);
int	athx_wmi_vdev_start(struct athx_softc *, int vdev, struct ieee80211_node *);
int	athx_wmi_vdev_stop(struct athx_softc *, int vdev);
int	athx_wmi_vdev_up(struct athx_softc *, int vdev, uint32_t aid, const uint8_t *bssid);
int	athx_wmi_vdev_down(struct athx_softc *, int vdev);
int	athx_wmi_vdev_install_key(struct athx_softc *, int vdev, const uint8_t *mac,
	    struct ieee80211_key *, uint32_t flags, int del);
int	athx_wmi_peer_create(struct athx_softc *, int vdev, const uint8_t *);
int	athx_wmi_peer_delete(struct athx_softc *, int vdev, const uint8_t *);
int	athx_wmi_peer_set_param(struct athx_softc *, int vdev, const uint8_t *, uint32_t, uint32_t);
int	athx_wmi_peer_assoc(struct athx_softc *, int vdev, struct ieee80211_node *);
int	athx_wmi_set_psmode(struct athx_softc *, int vdev, uint32_t);
int	athx_wmi_set_sta_ps_param(struct athx_softc *, int vdev, uint32_t, uint32_t);
int	athx_wmi_sta_keepalive_disable(struct athx_softc *, int vdev);
int	athx_wmi_start_scan(struct athx_softc *, int vdev);
int	athx_wmi_stop_scan(struct athx_softc *, int vdev);
int	athx_wmi_mgmt_tx(struct athx_softc *, int vdev, struct mbuf *);

/* ---------------------------------------------------------------- athx_htt.c */

int	athx_htt_connect(struct athx_softc *);
int	athx_htt_alloc(struct athx_softc *);
int	athx_htt_rx_ring_refill(struct athx_softc *);
int	athx_htt_setup(struct athx_softc *);
int	athx_htt_tx(struct athx_softc *, int vdev, struct mbuf *, int txmode, int no_encrypt);
void	athx_htt_txrx_compl_task(struct athx_softc *);
void	athx_htt_tx_cb(struct athx_softc *, struct athx_ce *);

/* ---------------------------------------------------------------- athx.c */

void	athx_scan_event(struct athx_softc *, uint32_t type, uint32_t reason, uint32_t freq);
void	athx_mgmt_rx(struct athx_softc *, struct mbuf *, uint32_t channel, int rssi);
void	athx_data_rx(struct athx_softc *, struct mbuf *, int rssi, uint32_t rxi_flags);
void	athx_beacon_miss(struct athx_softc *, uint32_t vdev);
void	athx_tx_done(struct athx_softc *);
struct ieee80211com *athx_attach(struct pci_attach_args *, const char *xname);
int	athx_match(struct pci_attach_args *);

extern struct athx_softc *athx_sc;

#endif /* __ATHXVAR_H__ */
