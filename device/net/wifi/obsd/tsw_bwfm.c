/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsw_bwfm.c (Raspberry Pi, WIFI_NET80211=obsd)
 *	The Wi-Fi interface's driver for the Pi's CYW43455 (design 9.6.8,
 *	9.6.9): OpenBSD's bwfm(4) on its sdmmc(4) and sdhc(4), as tswifi_obsd.c
 *	asks of it (tsw_drv.h).
 *
 *	What differs from the PCI cards as the interface sees it:
 *
 *	- there is no card on a bus to look for: the host is what the
 *	  hypercalls say (wifi_user_rpi5.c), and a machine without one has no
 *	  Wi-Fi. sdmmc finds the chip in a thread of its own, so the attach
 *	  waits for it;
 *	- the chip is FullMAC: no 802.11 frame of a join passes the layer, so
 *	  none of it can be stopped for the manager (tswifi_obsd.c's
 *	  tsw_send_mgmt). The firmware makes the handshakes, the protection
 *	  of management frames and SAE itself and roams by itself, when it is
 *	  set up for them just before the join: bwfm calls tswifi_bwfm_join
 *	  there, and tswifi_bwfm_event for each of the chip's events
 *	  (tools/openbsd/patches/bwfm-join-hooks.patch), and the manager asks
 *	  through rump_tswifi_fw_*.
 *
 *	The choices are taken from the access point's RSN element (the node
 *	of the scan): SAE when the access point offers it and the manager
 *	gave a password, else PSK (or PSK with SHA-256) with the PMK; the
 *	protection of management frames required when the access point
 *	requires it or the key management is SAE, offered when the access
 *	point offers it.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/queue.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdmmc/sdhcreg.h>
#include <dev/sdmmc/sdhcvar.h>
#include <dev/ic/bwfmreg.h>
#include <dev/ic/bwfmvar.h>

#include "obsd_glue.h"
#include "obsd_arm.h"
#include "tsw_drv.h"

extern struct bus_dma_tag obsd_dma_tag;

/* ---------------------------------------------------------------- the host */

static struct {
	struct sdhc_softc	sc;
	struct sdhc_host	*hosts[1];
	unsigned int		intr;
} host;

/* The chip is found in sdmmc's thread: how long it is waited for, and how often it is looked at */
#define CHIP_WAIT_MS	10000
#define CHIP_LOOK_MS	100

/* The task of the host's interrupt, masked by the hypercalls until the handler has run */
static void
host_intr_main(void *v)
{
	int depth;

	for (;;) {
		depth = obsd_lock_drop();
		(void)rumpcomp_tswifi_intr_wait(host.intr);
		obsd_lock_take(depth);
		(void)sdhc_intr(&host.sc);
		rumpcomp_tswifi_intr_done(host.intr);
	}
}

/* The host under sdhc; 0, or -1 when the machine has none. The lock is held */
static int
host_start(void)
{
	unsigned long long pa = 0, size = 0;
	unsigned int clk = 0;
	uint64_t mask, set;
	bus_space_handle_t h;
	int depth, er;

	depth = obsd_lock_drop();
	er = rumpcomp_tswifi_host(&pa, &size, &clk, &host.intr);
	obsd_lock_take(depth);
	if (er != 0)
		return -1;
	h = (bus_space_handle_t)rumpcomp_tswifi_map(pa, size);
	if (h == 0) {
		printf("obsd: the SDIO host's registers could not be mapped\n");
		return -1;
	}
	tsw_host_dev = &host.sc.sc_dev;
	strlcpy(host.sc.sc_dev.dv_xname, "sdhc0", sizeof(host.sc.sc_dev.dv_xname));
	host.sc.sc_host = host.hosts;
	host.sc.sc_dmat = &obsd_dma_tag;
	host.sc.sc_clkbase = clk;
	/*
	 * 32-bit accesses only, a card that is always there, the base clock
	 * the hypercalls give rather than the capabilities' field, the
	 * signals kept at 3.3V, and no DMA to begin with
	 */
	host.sc.sc_flags = SDHC_F_32BIT_ACCESS | SDHC_F_NONREMOVABLE;
	mask = (uint64_t)SDHC_VOLTAGE_SUPP_1_8V | ((uint64_t)SDHC_BASE_FREQ_MASK_V3 << SDHC_BASE_FREQ_SHIFT);
	set = 0;
	/*
	 * The BCM2711's older block: 3.3V and high speed as its wiring has
	 * them rather than its capabilities' registers, its power register
	 * never written 0, its speed bit left alone, each write to a register
	 * but the data port kept apart from the next, blocks of 1024 bytes
	 * at most
	 */
	if (rumpcomp_tswifi_host_arasan()) {
		host.sc.sc_flags |= SDHC_F_NOPWR0 | SDHC_F_NO_HS_BIT;
		mask = ~(uint64_t)0;
		set = SDHC_VOLTAGE_SUPP_3_3V | SDHC_HIGH_SPEED_SUPP |
		    (SDHC_MAX_BLK_LEN_1024 << SDHC_MAX_BLK_LEN_SHIFT);
		obsd_bus_write_gap = 1;
	}
	printf("obsd: SDIO host at %llx+%llx, %u kHz, %s\n", pa, size, clk,
	    obsd_bus_write_gap ? "the BCM2711's older block" : "the BCM2712's");
	if (obsd_task(host_intr_main, NULL, OBSD_PRI_INTR) != 0)
		return -1;
	er = sdhc_host_found(&host.sc, &obsd_host_space, h, size, 0, mask, set);
	if (er != 0) {
		printf("obsd: the SDIO host did not come up (%d)\n", er);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- the driver */

static struct ieee80211com	*bw_ic;
static int	caps_of(struct bwfm_softc *);

#define TSWIFI_FW_MFP		4

static struct ieee80211com *
bw_attach(struct pci_attach_args *pa, const char *xname)
{
	struct bwfm_softc *sc;
	struct ieee80211com *ic;
	static const uint8_t none[IEEE80211_ADDR_LEN];
	int waited;

	if (host_start() != 0)
		return NULL;
	for (waited = 0; waited < CHIP_WAIT_MS && !obsd_mountroot_pending(); waited += CHIP_LOOK_MS)
		tsleep_nsec(&host, 0, "chipwait", MSEC_TO_NSEC(CHIP_LOOK_MS));
	if (!obsd_mountroot_pending()) {
		printf("obsd: no chip on the SDIO host after %d ms%s\n", waited,
		    tsw_chip_dev != NULL ? " (it was found, and did not attach)" : "");
		return NULL;
	}
	sc = (struct bwfm_softc *)tsw_chip_dev;
	printf("obsd: %s: chip %d, revision %d: the firmware is read\n", xname,
	    sc->sc_chip.ch_chip, sc->sc_chip.ch_chiprev);
	obsd_mountroot_run();
	ic = &sc->sc_ic;
	if (ic->ic_if.if_softc == NULL || memcmp(ic->ic_myaddr, none, sizeof(none)) == 0) {
		printf("obsd: %s could not read its firmware and address\n", xname);
		return NULL;
	}
	printf("obsd: %s: the firmware runs, address %s\n", xname, ether_sprintf(ic->ic_myaddr));
	bw_ic = ic;
	/*
	 * net80211 turns away an access point that requires the protection of
	 * management frames from a card without it: the chip has it itself
	 */
	if (caps_of(sc) & TSWIFI_FW_MFP)
		ic->ic_caps |= IEEE80211_C_MFP;
	return ic;
}

/* The chip's firmware keeps it on the channel by itself */
static int
bw_te_on(void)
{
	return 0;
}

static int
bw_mgmt_queued(void)
{
	return 0;
}

/* A scan is IEEE80211_S_SCAN, which the interface looks at itself */
static int
bw_scanning(void)
{
	return 0;
}

static void
bw_card_stats(void)
{
}

static void
bw_debug(int level)
{
}

const struct tsw_drv tsw_bwfm_drv = {
	.name = "bwfm",
	.min_dbm = 0,
	.match = NULL,
	.attach = bw_attach,
	.te_on = bw_te_on,
	.tx_counts = NULL,
	.mgmt_queued = bw_mgmt_queued,
	.scanning = bw_scanning,
	.ptk_to_card = NULL,
	.rx_peek = NULL,
	.card_stats = bw_card_stats,
	.port_say = NULL,
	.debug = bw_debug,
};

/* ---------------------------------------------------------------- the chip's supplicant, PMF, SAE, roaming */

/* The chip's capabilities, as rump_tswifi_fw_caps tells them */
#define TSWIFI_FW_IDSUP		1
#define TSWIFI_FW_SAE		2

/* The firmware's words of a join: its "wpa_auth" and "auth" values, a band, the SAE password's room */
#define TSWIFI_WPA2_AUTH_PSK		0x00000080
#define TSWIFI_WPA2_AUTH_PSK_SHA256	0x00008000
#define TSWIFI_WPA3_AUTH_SAE_PSK	0x00040000
#define TSWIFI_AUTH_SAE			3
#define TSWIFI_BAND_ALL			3
#define TSWIFI_SAE_PW_MAX		128

/* The chip roams when the signal is below -75 dBm, to an access point 20 dB stronger */
#define TSWIFI_ROAM_TRIGGER_DBM		(-75)
#define TSWIFI_ROAM_DELTA_DB		20

/* RSN's suites (IEEE 802.11 9.4.2.24): 00-0F-AC and the type */
#define RSN_AKM_PSK		2
#define RSN_AKM_PSK_SHA256	6
#define RSN_AKM_SAE		8
#define RSN_CIPHER_TKIP		2
#define RSN_CIPHER_CCMP		4
#define RSN_CAP_MFPR		0x0040
#define RSN_CAP_MFPC		0x0080

/* What the chip says of the link (wifi.c has the same layout) */
struct tswifi_fwlink {
	int32_t		authorized;	/* the access point's port is open: the keys are in */
	int32_t		rssi;		/* dBm */
	int32_t		tx_kbps;	/* the rate of the last frame sent */
	int32_t		rx_kbps;	/* and of the last received */
	int32_t		mode;		/* 0 802.11a/b/g, 1 802.11n, 2 802.11ac */
	int32_t		freq;		/* MHz */
	int32_t		sup_status;	/* the chip's supplicant's last state (E_PSK_SUP), -1 none */
	int32_t		roams;		/* how many times the chip roamed */
	int32_t		pm;		/* BWFM_C_GET_PM: 0 awake, 1 PS-Poll, 2 fast; -1 not told */
	uint8_t		bssid[6];
	uint8_t		pad[2];
};

int	rump_tswifi_fw_caps(void);
int	rump_tswifi_fw_keys(int, const uint8_t *, const uint8_t *, int);
int	rump_tswifi_fw_link(struct tswifi_fwlink *);
void	tswifi_bwfm_join(struct bwfm_softc *);
void	tswifi_bwfm_event(struct bwfm_softc *, struct bwfm_event *);

int	bwfm_fwvar_cmd_get_data(struct bwfm_softc *, int, void *, size_t);
int	bwfm_fwvar_cmd_set_data(struct bwfm_softc *, int, void *, size_t);
int	bwfm_fwvar_cmd_get_int(struct bwfm_softc *, int, uint32_t *);
int	bwfm_fwvar_var_get_data(struct bwfm_softc *, const char *, void *, size_t);
int	bwfm_fwvar_var_set_data(struct bwfm_softc *, const char *, void *, size_t);
int	bwfm_fwvar_var_set_int(struct bwfm_softc *, const char *, uint32_t);

/*
 * What the manager asked for the joins of a network. The layer joins again by
 * itself when the network is lost, so the secrets are kept, as the layer
 * keeps its PMK, until the network is left (rump_tswifi_fw_keys(0))
 */
static struct {
	volatile int	use;		/* the chip's supplicant makes the handshakes */
	int		has_pmk;
	uint8_t		pmk[32];
	int		pw_len;
	uint8_t		pw[TSWIFI_SAE_PW_MAX];
} fwk;

static struct {
	int		known;		/* the capabilities have been read */
	int		caps;
	volatile int	sup_status;
	volatile int	roams;
} fws = { 0, 0, -1, 0 };

/* A word of the chip's capabilities, a list of words with blanks between */
static int
has_word(const char *list, const char *w)
{
	size_t n = strlen(w);
	const char *p;

	for (p = list; *p != '\0'; p++) {
		if (strncmp(p, w, n) == 0 && (p == list || p[-1] == ' ') &&
		    (p[n] == ' ' || p[n] == '\n' || p[n] == '\0'))
			return 1;
	}
	return 0;
}

/* The capabilities, read once and said on the console with the firmware's version */
static int
caps_of(struct bwfm_softc *sc)
{
	char buf[512];
	size_t n;

	if (fws.known)
		return fws.caps;
	memset(buf, 0, sizeof(buf));
	if (bwfm_fwvar_var_get_data(sc, "ver", buf, sizeof(buf) - 1) == 0) {
		buf[sizeof(buf) - 1] = '\0';
		n = strlen(buf);
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		printf("bwfm: firmware %s\n", buf);
	}
	memset(buf, 0, sizeof(buf));
	if (bwfm_fwvar_var_get_data(sc, "cap", buf, sizeof(buf) - 1) != 0) {
		printf("bwfm: the chip's capabilities could not be read\n");
		return 0;
	}
	buf[sizeof(buf) - 1] = '\0';
	fws.caps = (has_word(buf, "idsup") ? TSWIFI_FW_IDSUP : 0)
	    | (has_word(buf, "sae") ? TSWIFI_FW_SAE : 0)
	    | (has_word(buf, "mfp") ? TSWIFI_FW_MFP : 0);
	fws.known = 1;
	printf("bwfm: capabilities: %s\n", buf);
	printf("bwfm: the chip's supplicant %s, SAE %s, PMF %s\n",
	    (fws.caps & TSWIFI_FW_IDSUP) ? "yes" : "no",
	    (fws.caps & TSWIFI_FW_SAE) ? "yes" : "no",
	    (fws.caps & TSWIFI_FW_MFP) ? "yes" : "no");
	return fws.caps;
}

/* The chip's roaming on (off 0) or off, with the trigger and the step it roams by */
static void
roam_set(struct bwfm_softc *sc, int off)
{
	uint32_t v[2];
	int e1, e2 = 0, e3 = 0;

	e1 = bwfm_fwvar_var_set_int(sc, "roam_off", off);
	if (!off) {
		v[0] = htole32((uint32_t)TSWIFI_ROAM_TRIGGER_DBM);
		v[1] = htole32(TSWIFI_BAND_ALL);
		e2 = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_ROAM_TRIGGER, v, sizeof(v));
		v[0] = htole32(TSWIFI_ROAM_DELTA_DB);
		v[1] = htole32(TSWIFI_BAND_ALL);
		e3 = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_ROAM_DELTA, v, sizeof(v));
	}
	/* each setting's answer, 0 taken */
	if (off)
		printf("bwfm: the chip's roaming off (%d)\n", e1);
	else
		printf("bwfm: the chip's roaming on, below %d dBm to one %d dB stronger (%d %d %d)\n",
		    TSWIFI_ROAM_TRIGGER_DBM, TSWIFI_ROAM_DELTA_DB, e1, e2, e3);
}

/* The events of the supplicant and of a roam, added to those bwfm asked for */
static void
events_add(struct bwfm_softc *sc)
{
	uint8_t mask[BWFM_EVENT_MASK_LEN];

	memset(mask, 0, sizeof(mask));
	if (bwfm_fwvar_var_get_data(sc, "event_msgs", mask, sizeof(mask)) != 0)
		return;
	mask[BWFM_E_PSK_SUP / 8] |= 1 << (BWFM_E_PSK_SUP % 8);
	mask[BWFM_E_ROAM / 8] |= 1 << (BWFM_E_ROAM % 8);
	(void)bwfm_fwvar_var_set_data(sc, "event_msgs", mask, sizeof(mask));
}

/*
 * The access point's RSN element (the scan's node): the group cipher,
 * the key managements offered (a bit for each type) and the
 * capabilities. 0 when there is none to read.
 */
static int
ap_rsn(const uint8_t *ie, int *group, int *akms, int *caps)
{
	const uint8_t *p, *end;
	int n, i;

	if (ie == NULL || ie[0] != IEEE80211_ELEMID_RSN || ie[1] < 2 + 4)
		return 0;
	p = ie + 2;
	end = p + ie[1];
	p += 2;					/* the version */
	*group = p[3];
	p += 4;
	*akms = 0;
	*caps = 0;
	if (end - p < 2)
		return 1;
	n = p[0] | (p[1] << 8);			/* the pairwise ciphers, passed over */
	if (end - p < 2 + 4 * n)
		return 1;
	p += 2 + 4 * n;
	if (end - p < 2)
		return 1;
	n = p[0] | (p[1] << 8);
	p += 2;
	for (i = 0; i < n && end - p >= 4; i++, p += 4) {
		if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] < 32)
			*akms |= 1 << p[3];
	}
	if (end - p >= 2)
		*caps = p[0] | (p[1] << 8);
	return 1;
}

/* The station's RSN element for the chip's supplicant: its length */
static int
sta_rsn(uint8_t *ie, int group, int akm, int mfp)
{
	int n = 0, caps = (mfp >= BWFM_MFP_CAPABLE ? RSN_CAP_MFPC : 0)
	    | (mfp == BWFM_MFP_REQUIRED ? RSN_CAP_MFPR : 0);

	ie[n++] = IEEE80211_ELEMID_RSN;
	ie[n++] = 0;
	ie[n++] = 1;				/* version 1 */
	ie[n++] = 0;
	ie[n++] = 0x00;				/* the group cipher, the access point's */
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = group;
	ie[n++] = 1;				/* one pairwise cipher: CCMP */
	ie[n++] = 0;
	ie[n++] = 0x00;
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = RSN_CIPHER_CCMP;
	ie[n++] = 1;				/* one key management */
	ie[n++] = 0;
	ie[n++] = 0x00;
	ie[n++] = 0x0f;
	ie[n++] = 0xac;
	ie[n++] = akm;
	ie[n++] = caps & 0xff;
	ie[n++] = caps >> 8;
	ie[1] = n - 2;
	return n;
}

static const char *
akm_name(int akm)
{
	return akm == RSN_AKM_SAE ? "SAE (WPA3-Personal)"
	    : akm == RSN_AKM_PSK_SHA256 ? "PSK-SHA256" : "PSK (WPA2-Personal)";
}

/*
 * Just before bwfm joins (bwfm_connect, after it has written "wpa_auth",
 * "wsec", "auth" and "mfp"): the rest of the join's security written
 */
void
tswifi_bwfm_join(struct bwfm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct bwfm_wsec_pmk pmk;
	struct {
		uint16_t	len;
		uint8_t		key[TSWIFI_SAE_PW_MAX];
	} __packed sae;
	uint8_t ie[32];
	int caps, group = RSN_CIPHER_CCMP, akms = 1 << RSN_AKM_PSK, rcaps = 0;
	int akm, mfp, len, e[7], i, wpa = (ic->ic_flags & IEEE80211_F_RSNON) != 0;

	caps = caps_of(sc);
	events_add(sc);
	fws.sup_status = -1;
	if (!fwk.use || !wpa || !(caps & TSWIFI_FW_IDSUP)) {
		/* as bwfm makes it: the host's handshakes, and no roaming of the chip's under them */
		(void)bwfm_fwvar_var_set_int(sc, "sup_wpa", 0);
		printf("bwfm: join: %s\n", wpa ? "the handshakes by the host" : "no security");
		roam_set(sc, wpa);
		return;
	}
	if (ni == NULL || !ap_rsn(ni->ni_rsnie, &group, &akms, &rcaps))
		printf("bwfm: join: the access point's RSN element is not at hand: CCMP and PSK taken\n");
	if (fwk.pw_len > 0 && (akms & (1 << RSN_AKM_SAE)) && (caps & TSWIFI_FW_SAE))
		akm = RSN_AKM_SAE;
	else if (fwk.has_pmk && (akms & (1 << RSN_AKM_PSK)))
		akm = RSN_AKM_PSK;
	else if (fwk.has_pmk && (akms & (1 << RSN_AKM_PSK_SHA256)))
		akm = RSN_AKM_PSK_SHA256;
	else {
		printf("bwfm: join: the access point offers no key management there is a key for"
		    " (offered %x, a PMK %s, a password %s)\n", akms,
		    fwk.has_pmk ? "given" : "not given",
		    fwk.pw_len > 0 ? "given" : "not given");
		akm = RSN_AKM_PSK;
	}
	mfp = (akm == RSN_AKM_SAE || (rcaps & RSN_CAP_MFPR)) ? BWFM_MFP_REQUIRED
	    : (rcaps & RSN_CAP_MFPC) ? BWFM_MFP_CAPABLE : BWFM_MFP_NONE;
	if (mfp != BWFM_MFP_NONE && !(caps & TSWIFI_FW_MFP)) {
		printf("bwfm: join: the chip has no PMF\n");
		mfp = BWFM_MFP_NONE;
	}

	for (i = 0; i < 7; i++)
		e[i] = 0;
	len = sta_rsn(ie, group, akm, mfp);
	e[0] = bwfm_fwvar_var_set_data(sc, "wpaie", ie, len);
	e[1] = bwfm_fwvar_var_set_int(sc, "wpa_auth",
	    akm == RSN_AKM_SAE ? TSWIFI_WPA3_AUTH_SAE_PSK
	    : akm == RSN_AKM_PSK_SHA256 ? TSWIFI_WPA2_AUTH_PSK_SHA256 : TSWIFI_WPA2_AUTH_PSK);
	e[2] = bwfm_fwvar_var_set_int(sc, "wsec",
	    BWFM_WSEC_AES | (group == RSN_CIPHER_TKIP ? BWFM_WSEC_TKIP : 0));
	e[3] = bwfm_fwvar_var_set_int(sc, "auth",
	    akm == RSN_AKM_SAE ? TSWIFI_AUTH_SAE : BWFM_AUTH_OPEN);
	e[4] = bwfm_fwvar_var_set_int(sc, "mfp", mfp);
	if (akm == RSN_AKM_SAE) {
		memset(&sae, 0, sizeof(sae));
		sae.len = htole16(fwk.pw_len);
		memcpy(sae.key, fwk.pw, fwk.pw_len);
		e[5] = bwfm_fwvar_var_set_data(sc, "sae_password", &sae, sizeof(sae));
		explicit_bzero(&sae, sizeof(sae));
	} else {
		/* the PMK as it is, not a passphrase (no BWFM_WSEC_PASSPHRASE) */
		memset(&pmk, 0, sizeof(pmk));
		pmk.key_len = htole16(sizeof(fwk.pmk));
		pmk.flags = 0;
		memcpy(pmk.key, fwk.pmk, sizeof(fwk.pmk));
		e[5] = bwfm_fwvar_cmd_set_data(sc, BWFM_C_SET_WSEC_PMK, &pmk, sizeof(pmk));
		explicit_bzero(&pmk, sizeof(pmk));
	}
	e[6] = bwfm_fwvar_var_set_int(sc, "sup_wpa", 1);
	/* each setting's answer (0 taken), in the order written: wpaie wpa_auth wsec auth mfp key sup_wpa */
	printf("bwfm: join: the chip's supplicant, %s, group %s, PMF %s (%d %d %d %d %d %d %d)\n",
	    akm_name(akm), group == RSN_CIPHER_TKIP ? "TKIP" : "CCMP",
	    mfp == BWFM_MFP_REQUIRED ? "required" : mfp == BWFM_MFP_CAPABLE ? "offered" : "none",
	    e[0], e[1], e[2], e[3], e[4], e[5], e[6]);
	roam_set(sc, 0);
}

/* The chip's events: those of a join, of a roam and of the supplicant said, and noted */
void
tswifi_bwfm_event(struct bwfm_softc *sc, struct bwfm_event *e)
{
	static const struct {
		uint32_t	type;
		const char	*name;
	} names[] = {
		{ BWFM_E_SET_SSID, "set_ssid" }, { BWFM_E_AUTH, "auth" },
		{ BWFM_E_DEAUTH, "deauth" }, { BWFM_E_DEAUTH_IND, "deauth_ind" },
		{ BWFM_E_ASSOC, "assoc" }, { BWFM_E_REASSOC, "reassoc" },
		{ BWFM_E_DISASSOC, "disassoc" }, { BWFM_E_DISASSOC_IND, "disassoc_ind" },
		{ BWFM_E_LINK, "link" }, { BWFM_E_ROAM, "roam" },
		{ BWFM_E_PSK_SUP, "psk_sup" },
	};
	uint32_t type = ntohl(e->msg.event_type), status = ntohl(e->msg.status);
	const uint8_t *a = (const uint8_t *)&e->msg.addr;
	size_t i;

	for (i = 0; i < nitems(names) && names[i].type != type; i++)
		continue;
	if (i == nitems(names))
		return;
	/*
	 * The reason of a deauthentication or a disassociation is IEEE
	 * 802.11's; the status of psk_sup is the chip's supplicant's state
	 * (6 the keys are in)
	 */
	printf("bwfm: event %s, status %u, reason %u, flags %x, %02x:%02x:%02x:%02x:%02x:%02x\n",
	    names[i].name, status, ntohl(e->msg.reason), ntohs(e->msg.flags),
	    a[0], a[1], a[2], a[3], a[4], a[5]);
	if (type == BWFM_E_PSK_SUP)
		fws.sup_status = status;
	if (type == BWFM_E_ROAM && status == 0)
		fws.roams++;
}

static struct bwfm_softc *
bw_softc(void)
{
	return bw_ic != NULL ? (struct bwfm_softc *)tsw_chip_dev : NULL;
}

/* What the chip can do itself: TSWIFI_FW_*, 0 when there is no chip */
int
rump_tswifi_fw_caps(void)
{
	struct bwfm_softc *sc;
	int caps = 0;

	obsd_lock();
	sc = bw_softc();
	if (sc != NULL)
		caps = caps_of(sc);
	obsd_unlock();
	return caps;
}

/*
 * The next join's handshakes by the chip's supplicant (use 1), with the
 * network's PMK (32 bytes, for PSK) and its password (for SAE; NULL or
 * length 0 none), or by the host (use 0). 0 or an errno value.
 */
int
rump_tswifi_fw_keys(int use, const uint8_t *pmk, const uint8_t *pw, int pw_len)
{
	if (pw_len < 0 || pw_len > TSWIFI_SAE_PW_MAX)
		return EINVAL;
	obsd_lock();
	explicit_bzero(&fwk, sizeof(fwk));
	if (use) {
		if (pmk != NULL) {
			memcpy(fwk.pmk, pmk, sizeof(fwk.pmk));
			fwk.has_pmk = 1;
		}
		if (pw != NULL && pw_len > 0) {
			memcpy(fwk.pw, pw, pw_len);
			fwk.pw_len = pw_len;
		}
		fwk.use = 1;
	}
	obsd_unlock();
	return 0;
}

/*
 * The link as the chip has it. It asks the chip several things over
 * SDIO: not more than about once a second. 0 or an errno value.
 */
int
rump_tswifi_fw_link(struct tswifi_fwlink *l)
{
	struct bwfm_softc *sc;
	struct bwfm_sta_info sta;
	uint32_t v, ch[3];
	int c;

	memset(l, 0, sizeof(*l));
	l->sup_status = fws.sup_status;
	l->roams = fws.roams;
	obsd_lock();
	sc = bw_softc();
	if (sc == NULL) {
		obsd_unlock();
		return ENXIO;
	}
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_BSSID, l->bssid, sizeof(l->bssid)) != 0)
		memset(l->bssid, 0, sizeof(l->bssid));
	if (bwfm_fwvar_cmd_get_int(sc, BWFM_C_GET_RSSI, &v) == 0)
		l->rssi = (int32_t)v;
	l->pm = (bwfm_fwvar_cmd_get_int(sc, BWFM_C_GET_PM, &v) == 0) ? (int32_t)v : -1;
	if (bwfm_fwvar_cmd_get_data(sc, BWFM_C_GET_CHANNEL, ch, sizeof(ch)) == 0) {
		c = (int)letoh32(ch[1]);		/* the network's channel */
		l->freq = c <= 0 ? 0 : c == 14 ? 2484 : c < 14 ? 2407 + 5 * c : 5000 + 5 * c;
	}
	memset(&sta, 0, sizeof(sta));
	memcpy(&sta, l->bssid, sizeof(l->bssid));	/* whose: the access point's */
	if (bwfm_fwvar_var_get_data(sc, "sta_info", &sta, sizeof(sta)) == 0
	    && memcmp(sta.ea, l->bssid, sizeof(l->bssid)) == 0) {
		v = letoh32(sta.flags);
		l->authorized = (v & BWFM_STA_AUTHO) != 0;
		l->mode = (v & BWFM_STA_VHT_CAP) ? 2 : (v & BWFM_STA_N_CAP) ? 1 : 0;
		if (letoh16(sta.ver) >= 3) {
			l->tx_kbps = (int32_t)letoh32(sta.tx_rate);
			l->rx_kbps = (int32_t)letoh32(sta.rx_rate);
		}
	}
	obsd_unlock();
	return 0;
}
