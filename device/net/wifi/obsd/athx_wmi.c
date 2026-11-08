/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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
 *	athx_wmi.c (x64 PC, WIFI_NET80211=obsd)
 *	The firmware's control interface, WMI in its TLV form (design 9.6.9):
 *	every command is a WMI header (its ID) and a list of elements, each a
 *	tag and a length before its value; every event the same. Commands go
 *	out on HTC's WMI endpoint, the only one with credits: a command waits
 *	(up to 3 s) for the credits the target gives back in its trailers.
 *
 *	The events: the service ready event (the chip's capabilities, its
 *	regulatory domain, the services the firmware has, the memory it wants
 *	of the host), the ready event (the chip's address), the scan's events,
 *	the management frames received, the answers to vdev start and stop,
 *	the roam event that says a beacon was missed, and echo. What the host
 *	does not use is counted and dropped.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/endian.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include "athxreg.h"
#include "athxvar.h"

#define WMI_TLV_ABI_VER_NS0	0x5F414351
#define WMI_TLV_ABI_VER_NS1	0x00004C4D
#define WMI_TLV_ABI_VER_NS2	0x00000000
#define WMI_TLV_ABI_VER_NS3	0x00000000
#define WMI_TLV_ABI_VER0_MAJOR	1
#define WMI_TLV_ABI_VER0_MINOR	0
#define WMI_TLV_ABI_VER0	((((WMI_TLV_ABI_VER0_MAJOR) << 24) & 0xFF000000) | \
				 (((WMI_TLV_ABI_VER0_MINOR) << 0) & 0x00FFFFFF))
#define WMI_TLV_ABI_VER1	53

#define ATHX_WMI_BARRIER_ECHO_ID	0xBA991E9

/* The service ready event, kept as it came for the host to take apart outside the interrupt */
static uint8_t	*svc_rdy_buf;
static int	svc_rdy_len;
static uint8_t	svc_raw[256];		/* the service bitmap (four services in each word) */
static int	svc_raw_len;
static uint32_t	svc_ext[(WMI_TLV_MAX_EXT_SERVICE - WMI_TLV_MAX_SERVICE) / 32];
static int	svc_ext_valid;
static int	echo_done;

/* ---------------------------------------------------------------- elements */

/* An element put at *p: its value's start, *p moved past it */
static void *
tlv_put(uint8_t **p, uint16_t tag, uint16_t len)
{
	struct wmi_tlv *t = (struct wmi_tlv *)*p;

	t->tag = htole16(tag);
	t->len = htole16(len);
	*p += sizeof(*t) + len;
	return t->value;
}

/* The first element of tag in a list: its value and length, or NULL */
static const void *
tlv_find(const uint8_t *p, int len, uint16_t tag, int *vlen)
{
	const struct wmi_tlv *t;
	int l;

	while (len >= (int)sizeof(*t)) {
		t = (const struct wmi_tlv *)p;
		l = le16toh(t->len);
		p += sizeof(*t);
		len -= sizeof(*t);
		if (l > len)
			return NULL;
		if (le16toh(t->tag) == tag) {
			if (vlen != NULL)
				*vlen = l;
			return p;
		}
		p += l;
		len -= l;
	}
	return NULL;
}

/* The n-th element of tag (from 0) */
static const void *
tlv_find_nth(const uint8_t *p, int len, uint16_t tag, int n, int *vlen)
{
	const struct wmi_tlv *t;
	int l;

	while (len >= (int)sizeof(*t)) {
		t = (const struct wmi_tlv *)p;
		l = le16toh(t->len);
		p += sizeof(*t);
		len -= sizeof(*t);
		if (l > len)
			return NULL;
		if (le16toh(t->tag) == tag && n-- == 0) {
			if (vlen != NULL)
				*vlen = l;
			return p;
		}
		p += l;
		len -= l;
	}
	return NULL;
}

/* ---------------------------------------------------------------- sending */

/*
 * A command: the WMI header in front, sent on WMI's endpoint once there
 * are credits for it (waited for up to 3 s). 0 or an errno value
 */
static int
wmi_cmd_send(struct athx_softc *sc, uint32_t cmd_id, const void *body, int len)
{
	struct athx_htc_ep *ep = &sc->ep[sc->wmi_eid];
	struct wmi_cmd_hdr *hdr;
	uint64_t until = getnsecuptime() + 3000000000ULL;
	uint8_t *msg;
	int ret, total = sizeof(*hdr) + roundup(len, 4);

	if (sc->wedged)
		return ESHUTDOWN;
	msg = malloc(total, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (msg == NULL)
		return ENOMEM;
	hdr = (struct wmi_cmd_hdr *)msg;
	hdr->cmd_id = htole32(SM(cmd_id, WMI_CMD_HDR_CMD_ID));
	memcpy(msg + sizeof(*hdr), body, len);
	for (;;) {
		ret = athx_htc_send(sc, sc->wmi_eid, msg, total);
		if (ret != EAGAIN)
			break;
		if (getnsecuptime() >= until || sc->wedged)
			break;
		tsleep_nsec(&ep->tx_credits, 0, "athxcred", MSEC_TO_NSEC(10));
	}
	free(msg, M_DEVBUF, total);
	if (ret == EAGAIN) {
		printf("%s: wmi command %x timeout (credits %d)\n", sc->sc_dev.dv_xname, cmd_id, ep->tx_credits);
		athx_say_regs(sc, "WMI command not sent");
	}
	if (ret == 0)
		sc->n_wmi_cmd++;
	return ret;
}

/* A command of one element (tag) with len bytes of value */
static int
wmi_cmd1(struct athx_softc *sc, uint32_t cmd_id, uint16_t tag, const void *val, int len)
{
	uint8_t buf[256], *p = buf;
	void *v;

	if (len + (int)sizeof(struct wmi_tlv) > (int)sizeof(buf))
		return EMSGSIZE;
	memset(buf, 0, sizeof(buf));
	v = tlv_put(&p, tag, len);
	memcpy(v, val, len);
	return wmi_cmd_send(sc, cmd_id, buf, p - buf);
}

int
athx_wmi_svc(struct athx_softc *sc, int x)
{
	const uint32_t *w = (const uint32_t *)svc_raw;

	if (x < WMI_TLV_MAX_SERVICE)
		return x < svc_raw_len && (le32toh(w[x / 4]) & BIT(x % 4));
	x -= WMI_TLV_MAX_SERVICE;
	return svc_ext_valid && x / 32 < (int)nitems(svc_ext) && (le32toh(svc_ext[x / 32]) & BIT(x % 32));
}

int
athx_wmi_pdev_set_param(struct athx_softc *sc, uint32_t id, uint32_t value)
{
	struct wmi_tlv_pdev_set_param_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.param_id = htole32(id);
	cmd.param_value = htole32(value);
	return wmi_cmd1(sc, WMI_TLV_PDEV_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD, &cmd, sizeof(cmd));
}

int
athx_wmi_pdev_set_regdomain(struct athx_softc *sc, uint16_t rd, uint16_t rd2g, uint16_t rd5g, uint16_t ctl2g,
    uint16_t ctl5g)
{
	struct wmi_tlv_pdev_set_rd_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.regd = htole32(rd);
	cmd.regd_2ghz = htole32(rd2g);
	cmd.regd_5ghz = htole32(rd5g);
	cmd.conform_limit_2ghz = htole32(ctl2g);
	cmd.conform_limit_5ghz = htole32(ctl5g);
	return wmi_cmd1(sc, WMI_TLV_PDEV_SET_REGDOMAIN_CMDID, WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD, &cmd,
	    sizeof(cmd));
}

/* ---------------------------------------------------------------- channels */

/* Whether net80211's channel number c+4 exists (the HT40 secondary above c) */
static int
chan_has_above(struct ieee80211com *ic, struct ieee80211_channel *c)
{
	u_int n = ieee80211_chan2ieee(ic, c) + 4;

	if (n > IEEE80211_CHAN_MAX || ic->ic_channels[n].ic_flags == 0)
		return 0;
	return (IEEE80211_IS_CHAN_2GHZ(c) && IEEE80211_IS_CHAN_2GHZ(&ic->ic_channels[n])) ||
	    (IEEE80211_IS_CHAN_5GHZ(c) && IEEE80211_IS_CHAN_5GHZ(&ic->ic_channels[n]));
}

/* The radar band of 5 GHz (5260 to 5700 MHz) */
static int
chan_is_radar(struct ieee80211_channel *c)
{
	return c->ic_freq >= 5260 && c->ic_freq <= 5700;
}

/* A channel as the firmware takes it: its frequency, mode, flags and powers (0.5 dBm) */
static void
put_wmi_channel(struct athx_softc *sc, struct wmi_channel *ch, struct ieee80211_channel *c, uint32_t mode,
    uint32_t center1)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t flags = 0;
	int passive = (c->ic_flags & IEEE80211_CHAN_PASSIVE) != 0, radar = chan_is_radar(c);
	int max_power = IEEE80211_IS_CHAN_2GHZ(c) ? 20 : 30;

	memset(ch, 0, sizeof(*ch));
	if (passive || radar)
		flags |= WMI_CHAN_FLAG_PASSIVE;
	if (!passive)
		flags |= WMI_CHAN_FLAG_ADHOC_ALLOWED;
	flags |= WMI_CHAN_FLAG_ALLOW_HT | WMI_CHAN_FLAG_ALLOW_VHT;
	if (chan_has_above(ic, c))
		flags |= WMI_CHAN_FLAG_HT40_PLUS;
	if (radar)
		flags |= WMI_CHAN_FLAG_DFS;
	ch->mhz = htole32(c->ic_freq);
	ch->band_center_freq1 = htole32(center1 ? center1 : c->ic_freq);
	ch->band_center_freq2 = 0;
	ch->min_power = 0;
	ch->max_power = max_power * 2;
	ch->reg_power = max_power * 2;
	ch->antenna_max = 0;
	ch->max_tx_power = max_power * 2;
	ch->mode = mode;		/* mode and flags share storage */
	ch->flags |= htole32(flags);
}

/* Every channel net80211 has, for the firmware to scan and join on */
int
athx_wmi_scan_chan_list(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c;
	struct wmi_tlv_scan_chan_list_cmd *cmd;
	uint8_t *buf, *p, *chans;
	int n = 0, i, len, ret;

	for (i = 1; i <= IEEE80211_CHAN_MAX; i++)
		if (ic->ic_channels[i].ic_flags != 0)
			n++;
	len = sizeof(struct wmi_tlv) + sizeof(*cmd) + sizeof(struct wmi_tlv) +
	    n * (sizeof(struct wmi_tlv) + sizeof(struct wmi_channel));
	buf = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL)
		return ENOMEM;
	p = buf;
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD, sizeof(*cmd));
	cmd->num_scan_chans = htole32(n);
	chans = tlv_put(&p, WMI_TLV_TAG_ARRAY_STRUCT, n * (sizeof(struct wmi_tlv) + sizeof(struct wmi_channel)));
	for (i = 1; i <= IEEE80211_CHAN_MAX; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_flags == 0)
			continue;
		put_wmi_channel(sc, tlv_put(&chans, WMI_TLV_TAG_STRUCT_CHANNEL, sizeof(struct wmi_channel)), c,
		    IEEE80211_IS_CHAN_2GHZ(c) ? MODE_11G : MODE_11A, 0);
	}
	ret = wmi_cmd_send(sc, WMI_TLV_SCAN_CHAN_LIST_CMDID, buf, len);
	free(buf, M_DEVBUF, len);
	return ret;
}

/* ---------------------------------------------------------------- vdevs */

int
athx_wmi_vdev_create(struct athx_softc *sc, int vdev, uint32_t type, uint32_t subtype, const uint8_t *mac)
{
	struct wmi_vdev_create_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.vdev_type = htole32(type);
	cmd.vdev_subtype = htole32(subtype);
	memcpy(cmd.vdev_macaddr.addr, mac, ETH_ALEN);
	return wmi_cmd1(sc, WMI_TLV_VDEV_CREATE_CMDID, WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD, &cmd, sizeof(cmd));
}

int
athx_wmi_vdev_delete(struct athx_softc *sc, int vdev)
{
	struct wmi_vdev_delete_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	return wmi_cmd1(sc, WMI_TLV_VDEV_DELETE_CMDID, WMI_TLV_TAG_STRUCT_VDEV_DELETE_CMD, &cmd, sizeof(cmd));
}

int
athx_wmi_vdev_set_param(struct athx_softc *sc, int vdev, uint32_t id, uint32_t value)
{
	struct wmi_vdev_set_param_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.param_id = htole32(id);
	cmd.param_value = htole32(value);
	return wmi_cmd1(sc, WMI_TLV_VDEV_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD, &cmd, sizeof(cmd));
}

/*
 * The PHY mode of the BSS's channel, by the widest the AP and the station
 * both have (what the AP's beacons said, before and after the association)
 */
static uint32_t
chan_phymode(struct athx_softc *sc, struct ieee80211_node *ni, uint32_t *center1)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c = ni->ni_chan;
	int ht = (ic->ic_flags & IEEE80211_F_HTON) && ieee80211_node_supports_ht(ni);
	int vht = (ic->ic_flags & IEEE80211_F_VHTON) && IEEE80211_IS_CHAN_5GHZ(c) &&
	    ieee80211_node_supports_vht(ni);
	int ht40 = ht && ieee80211_node_supports_ht_chan40(ni) &&
	    ((ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK) >> IEEE80211_HTOP0_SCO_SHIFT) != IEEE80211_HTOP0_SCO_SCN;
	int vht80 = vht && ieee80211_node_supports_vht_chan80(ni);

	*center1 = c->ic_freq;
	if (ht40)
		*center1 = (((ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK) >> IEEE80211_HTOP0_SCO_SHIFT) == IEEE80211_HTOP0_SCO_SCA) ?
		    c->ic_freq + 10 : c->ic_freq - 10;
	if (IEEE80211_IS_CHAN_2GHZ(c)) {
		if (ht40)
			return MODE_11NG_HT40;
		return ht ? MODE_11NG_HT20 : MODE_11G;
	}
	if (vht80) {
		*center1 = ieee80211_ieee2mhz(ni->ni_vht_chan_center_freq_idx0, IEEE80211_CHAN_5GHZ);
		return MODE_11AC_VHT80;
	}
	if (vht)
		return ht40 ? MODE_11AC_VHT40 : MODE_11AC_VHT20;
	if (ht40)
		return MODE_11NA_HT40;
	return ht ? MODE_11NA_HT20 : MODE_11A;
}

/* The vdev started on the BSS's channel, the firmware's answer waited for (5 s) */
int
athx_wmi_vdev_start(struct athx_softc *sc, int vdev, struct ieee80211_node *ni)
{
	struct wmi_tlv_vdev_start_cmd *cmd;
	uint8_t buf[256], *p = buf;
	uint32_t mode, center1;
	int ret;

	memset(buf, 0, sizeof(buf));
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD, sizeof(*cmd));
	cmd->vdev_id = htole32(vdev);
	cmd->bcn_intval = htole32(ni->ni_intval);
	cmd->dtim_period = htole32(ni->ni_dtimperiod);
	cmd->flags = 0;
	mode = chan_phymode(sc, ni, &center1);
	put_wmi_channel(sc, tlv_put(&p, WMI_TLV_TAG_STRUCT_CHANNEL, sizeof(struct wmi_channel)), ni->ni_chan, mode,
	    center1);
	(void)tlv_put(&p, WMI_TLV_TAG_ARRAY_STRUCT, 0);
	sc->vdev_setup_done = 0;
	sc->last_vdev_start_status = 0;
	ret = wmi_cmd_send(sc, WMI_TLV_VDEV_START_REQUEST_CMDID, buf, p - buf);
	if (ret)
		return ret;
	ret = athx_wait(sc, &sc->vdev_setup_done, "athxvdev", 5000);
	if (ret) {
		printf("%s: no answer to vdev %d start\n", sc->sc_dev.dv_xname, vdev);
		return ret;
	}
	ATHX_DPRINTF(sc, "%s: vdev %d started on %d MHz, mode %u, center %u\n", sc->sc_dev.dv_xname, vdev,
	    ni->ni_chan->ic_freq, mode, center1);
	return sc->last_vdev_start_status;
}

int
athx_wmi_vdev_stop(struct athx_softc *sc, int vdev)
{
	struct wmi_vdev_stop_cmd cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	sc->vdev_setup_done = 0;
	ret = wmi_cmd1(sc, WMI_TLV_VDEV_STOP_CMDID, WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD, &cmd, sizeof(cmd));
	if (ret)
		return ret;
	return athx_wait(sc, &sc->vdev_setup_done, "athxvdst", 5000);
}

int
athx_wmi_vdev_up(struct athx_softc *sc, int vdev, uint32_t aid, const uint8_t *bssid)
{
	struct wmi_vdev_up_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.vdev_assoc_id = htole32(aid);
	memcpy(cmd.vdev_bssid.addr, bssid, ETH_ALEN);
	return wmi_cmd1(sc, WMI_TLV_VDEV_UP_CMDID, WMI_TLV_TAG_STRUCT_VDEV_UP_CMD, &cmd, sizeof(cmd));
}

int
athx_wmi_vdev_down(struct athx_softc *sc, int vdev)
{
	struct wmi_vdev_down_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	return wmi_cmd1(sc, WMI_TLV_VDEV_DOWN_CMDID, WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD, &cmd, sizeof(cmd));
}

/*
 * A key into the firmware (or taken out of it: cipher none, no data), the
 * HTT security indication that answers it waited for (3 s)
 */
int
athx_wmi_vdev_install_key(struct athx_softc *sc, int vdev, const uint8_t *mac, struct ieee80211_key *k,
    uint32_t flags, int del)
{
	struct wmi_vdev_install_key_cmd *cmd;
	uint8_t buf[256], *p = buf, *kd;
	uint32_t cipher, txmic = 0, rxmic = 0;
	int klen = del ? 0 : k->k_len, ret;

	if (del) {
		cipher = WMI_TLV_CIPHER_NONE;
	} else {
		switch (k->k_cipher) {
		case IEEE80211_CIPHER_CCMP:
			cipher = WMI_TLV_CIPHER_AES_CCM;
			break;
		case IEEE80211_CIPHER_TKIP:
			cipher = WMI_TLV_CIPHER_TKIP;
			txmic = rxmic = 8;
			break;
		default:
			return EOPNOTSUPP;
		}
	}
	memset(buf, 0, sizeof(buf));
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD, sizeof(*cmd));
	cmd->vdev_id = htole32(vdev);
	cmd->key_idx = htole32(k->k_id);
	cmd->key_flags = htole32(flags);
	cmd->key_cipher = htole32(cipher);
	cmd->key_len = htole32(klen);
	cmd->key_txmic_len = htole32(txmic);
	cmd->key_rxmic_len = htole32(rxmic);
	if (mac != NULL)
		memcpy(cmd->peer_macaddr.addr, mac, ETH_ALEN);
	kd = tlv_put(&p, WMI_TLV_TAG_ARRAY_BYTE, roundup(klen, 4));
	if (klen) {
		if (k->k_cipher == IEEE80211_CIPHER_TKIP) {
			/* the firmware's TKIP key: the temporal key, then the Tx MIC key, then the Rx MIC key */
			memcpy(kd, k->k_key, 16);
			memcpy(kd + 16, &k->k_key[16], 8);
			memcpy(kd + 24, &k->k_key[24], 8);
		} else {
			memcpy(kd, k->k_key, klen);
		}
	}
	sc->install_key_done = 0;
	ret = wmi_cmd_send(sc, WMI_TLV_VDEV_INSTALL_KEY_CMDID, buf, p - buf);
	if (ret)
		return ret;
	ret = athx_wait(sc, &sc->install_key_done, "athxkey", 3000);
	if (ret)
		printf("%s: no answer to the key %d (%s)\n", sc->sc_dev.dv_xname, k->k_id, del ? "deleted" : "set");
	return ret;
}

/* ---------------------------------------------------------------- peers */

/* A peer created, the HTT peer map that says the firmware made it waited for (3 s) */
int
athx_wmi_peer_create(struct athx_softc *sc, int vdev, const uint8_t *mac)
{
	struct wmi_tlv_peer_create_cmd cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.peer_type = htole32(WMI_PEER_TYPE_DEFAULT);
	memcpy(cmd.peer_addr.addr, mac, ETH_ALEN);
	sc->peer_mapped = 0;
	memcpy(sc->peer_wait_addr, mac, ETH_ALEN);
	ret = wmi_cmd1(sc, WMI_TLV_PEER_CREATE_CMDID, WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD, &cmd, sizeof(cmd));
	if (ret)
		return ret;
	ret = athx_wait(sc, &sc->peer_mapped, "athxpeer", 3000);
	if (ret)
		printf("%s: failed to wait for created wmi peer %s on vdev %d: %d\n", sc->sc_dev.dv_xname,
		    ether_sprintf((uint8_t *)mac), vdev, ret);
	return ret;
}

int
athx_wmi_peer_delete(struct athx_softc *sc, int vdev, const uint8_t *mac)
{
	struct wmi_peer_delete_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	memcpy(cmd.peer_macaddr.addr, mac, ETH_ALEN);
	return wmi_cmd1(sc, WMI_TLV_PEER_DELETE_CMDID, WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD, &cmd, sizeof(cmd));
}

int
athx_wmi_peer_set_param(struct athx_softc *sc, int vdev, const uint8_t *mac, uint32_t id, uint32_t value)
{
	struct wmi_peer_set_param_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.param_id = htole32(id);
	cmd.param_value = htole32(value);
	memcpy(cmd.peer_macaddr.addr, mac, ETH_ALEN);
	return wmi_cmd1(sc, WMI_TLV_PEER_SET_PARAM_CMDID, WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD, &cmd, sizeof(cmd));
}

/* The minimum MPDU start spacing of HT, in us, as the firmware takes it */
static uint32_t
parse_mpdudensity(uint8_t d)
{
	switch (d) {
	case 0:
		return 0;
	case 1:
	case 2:
	case 3:
		return 1;
	case 4:
		return 2;
	case 5:
		return 4;
	case 6:
		return 8;
	case 7:
		return 16;
	default:
		return 0;
	}
}

/*
 * The AP as a peer associated: its flags, legacy rates, HT and VHT
 * capabilities and the PHY mode of the BSS, for the firmware's rate
 * control
 */
int
athx_wmi_peer_assoc(struct athx_softc *sc, int vdev, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wmi_tlv_peer_assoc_cmd *cmd;
	struct wmi_vht_rate_set *vr;
	uint8_t buf[512], *p = buf, *rates;
	uint8_t legacy[IEEE80211_RATE_MAXSIZE], htr[MAX_SUPPORTED_RATES];
	uint32_t flags = WMI_TLV_PEER_AUTH, rate_caps = 0, ht_caps = 0, vht_caps = 0, max_mpdu = 0, density = 0;
	uint32_t center1, phymode, nss = 1;
	int nl = 0, nh = 0, i, max_nss = 0;

	for (i = 0; i < ni->ni_rates.rs_nrates && nl < (int)sizeof(legacy); i++) {
		uint8_t r = ni->ni_rates.rs_rates[i] & IEEE80211_RATE_VAL;

		/* in 500 kb/s, with bit 7 for a CCK rate */
		legacy[nl++] = r | ((r == 2 || r == 4 || r == 11 || r == 22) ? 0x80 : 0);
	}
	if (ic->ic_flags & IEEE80211_F_RSNON)
		flags |= WMI_TLV_PEER_NEED_PTK_4_WAY;
	if ((ic->ic_flags & IEEE80211_F_RSNON) && ni->ni_rsnprotos == IEEE80211_PROTO_WPA)
		flags |= WMI_TLV_PEER_NEED_GTK_2_WAY;
	if (ni->ni_flags & IEEE80211_NODE_QOS)
		flags |= WMI_TLV_PEER_QOS;
	if (ni->ni_flags & IEEE80211_NODE_HT) {
		flags |= WMI_TLV_PEER_HT;
		max_mpdu = (1 << (13 + (ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_LE))) - 1;
		density = parse_mpdudensity((ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_SS) >> 2);
		ht_caps = ni->ni_htcaps;
		rate_caps |= WMI_RC_HT_FLAG;
		if (ni->ni_htcaps & IEEE80211_HTCAP_LDPC)
			flags |= WMI_TLV_PEER_LDPC;
		if (ieee80211_node_supports_ht_chan40(ni)) {
			flags |= WMI_TLV_PEER_40MHZ;
			rate_caps |= WMI_RC_CW40_FLAG;
		}
		if (ni->ni_htcaps & (IEEE80211_HTCAP_SGI20 | IEEE80211_HTCAP_SGI40))
			rate_caps |= WMI_RC_SGI_FLAG;
		if (ni->ni_htcaps & IEEE80211_HTCAP_TXSTBC) {
			rate_caps |= WMI_RC_TX_STBC_FLAG;
			flags |= WMI_TLV_PEER_STBC;
		}
		if (ni->ni_htcaps & IEEE80211_HTCAP_RXSTBC_MASK) {
			rate_caps |= ((ni->ni_htcaps & IEEE80211_HTCAP_RXSTBC_MASK) >> IEEE80211_HTCAP_RXSTBC_SHIFT)
			    << WMI_RC_RX_STBC_FLAG_S;
			flags |= WMI_TLV_PEER_STBC;
		}
		if (ni->ni_rxmcs[1] && ni->ni_rxmcs[2])
			rate_caps |= WMI_RC_TS_FLAG;
		else if (ni->ni_rxmcs[1])
			rate_caps |= WMI_RC_DS_FLAG;
		for (i = 0; i < IEEE80211_HT_NUM_MCS && nh < (int)sizeof(htr); i++) {
			if (isset(ni->ni_rxmcs, i) && isset(ic->ic_sup_mcs, i)) {
				max_nss = (i / 8) + 1;
				htr[nh++] = i;
			}
		}
		if (nh == 0) {
			/* MCS 0 to 7 are mandatory at every HT station */
			for (i = 0; i < 8; i++)
				htr[nh++] = i;
		} else {
			nss = MIN((uint32_t)max_nss, sc->num_rf_chains ? sc->num_rf_chains : 1);
		}
	}
	if (ni->ni_flags & IEEE80211_NODE_VHT) {
		uint16_t rxmap = ni->ni_vht_rxmcs, txmap = ni->ni_vht_txmcs;

		flags |= WMI_TLV_PEER_VHT;
		vht_caps = ni->ni_vhtcaps;
		max_mpdu = MAX(max_mpdu, (1U << (13 + ((ni->ni_vhtcaps & IEEE80211_VHTCAP_MAX_AMPDU_LEN_MASK) >>
		    IEEE80211_VHTCAP_MAX_AMPDU_LEN_SHIFT))) - 1);
		if (ieee80211_node_supports_vht_chan80(ni))
			flags |= WMI_TLV_PEER_80MHZ;
		for (i = 0, max_nss = 0; i < 8; i++)
			if (((rxmap >> (2 * i)) & 3) != IEEE80211_VHT_MCS_SS_NOT_SUPP)
				max_nss = i + 1;
		nss = MIN((uint32_t)max_nss, sc->num_rf_chains ? sc->num_rf_chains : 1);
		(void)txmap;
	}
	phymode = chan_phymode(sc, ni, &center1);

	memset(buf, 0, sizeof(buf));
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD, sizeof(*cmd));
	cmd->vdev_id = htole32(vdev);
	cmd->new_assoc = htole32(1);
	cmd->assoc_id = htole32(ni->ni_associd & 0x3fff);
	cmd->flags = htole32(flags);
	cmd->caps = htole32(ni->ni_capinfo);
	cmd->listen_intval = htole32(1);
	cmd->ht_caps = htole32(ht_caps);
	cmd->max_mpdu = htole32(max_mpdu);
	cmd->mpdu_density = htole32(density);
	cmd->rate_caps = htole32(rate_caps);
	cmd->nss = htole32(nss);
	cmd->vht_caps = htole32(vht_caps);
	cmd->phy_mode = htole32(phymode);
	cmd->num_legacy_rates = htole32(nl);
	cmd->num_ht_rates = htole32(nh);
	memcpy(cmd->mac_addr.addr, ni->ni_macaddr, ETH_ALEN);
	rates = tlv_put(&p, WMI_TLV_TAG_ARRAY_BYTE, roundup(nl, 4));
	memcpy(rates, legacy, nl);
	rates = tlv_put(&p, WMI_TLV_TAG_ARRAY_BYTE, roundup(nh, 4));
	memcpy(rates, htr, nh);
	vr = tlv_put(&p, WMI_TLV_TAG_STRUCT_VHT_RATE_SET, sizeof(*vr));
	if (ni->ni_flags & IEEE80211_NODE_VHT) {
		vr->rx_max_rate = htole32(ni->ni_vht_rx_max_lgi_mbit_s);
		vr->rx_mcs_set = htole32(ni->ni_vht_rxmcs);
		vr->tx_max_rate = htole32(ni->ni_vht_tx_max_lgi_mbit_s);
		vr->tx_mcs_set = htole32(ni->ni_vht_txmcs);
	}
	printf("%s: peer %s assoc: aid %d, flags %08x, phymode %u, nss %u, %d legacy and %d HT rates, "
	    "rate caps %x\n", sc->sc_dev.dv_xname, ether_sprintf(ni->ni_macaddr), ni->ni_associd & 0x3fff, flags,
	    phymode, nss, nl, nh, rate_caps);
	return wmi_cmd_send(sc, WMI_TLV_PEER_ASSOC_CMDID, buf, p - buf);
}

/* ---------------------------------------------------------------- the station's power save */

int
athx_wmi_set_psmode(struct athx_softc *sc, int vdev, uint32_t mode)
{
	struct wmi_sta_powersave_mode_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.sta_ps_mode = htole32(mode);
	return wmi_cmd1(sc, WMI_TLV_STA_POWERSAVE_MODE_CMDID, WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD, &cmd,
	    sizeof(cmd));
}

int
athx_wmi_set_sta_ps_param(struct athx_softc *sc, int vdev, uint32_t id, uint32_t value)
{
	struct wmi_sta_powersave_param_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = htole32(vdev);
	cmd.param_id = htole32(id);
	cmd.param_value = htole32(value);
	return wmi_cmd1(sc, WMI_TLV_STA_POWERSAVE_PARAM_CMDID, WMI_TLV_TAG_STRUCT_STA_POWERSAVE_PARAM_CMD, &cmd,
	    sizeof(cmd));
}

/* The firmware's keepalive turned off by its interval (some firmware ignores enabled) */
int
athx_wmi_sta_keepalive_disable(struct athx_softc *sc, int vdev)
{
	struct wmi_tlv_sta_keepalive_cmd *cmd;
	uint8_t buf[128], *p = buf;

	if (!athx_wmi_svc(sc, WMI_TLV_SERVICE_STA_KEEP_ALIVE))
		return 0;
	memset(buf, 0, sizeof(buf));
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_STA_KEEPALIVE_CMD, sizeof(*cmd));
	cmd->vdev_id = htole32(vdev);
	cmd->enabled = htole32(1);
	cmd->method = htole32(WMI_STA_KEEPALIVE_METHOD_NULL_FRAME);
	cmd->interval = htole32(WMI_STA_KEEPALIVE_INTERVAL_DISABLE);
	(void)tlv_put(&p, WMI_TLV_TAG_STRUCT_STA_KEEPALVE_ARP_RESPONSE, sizeof(struct wmi_sta_keepalive_arp_resp));
	return wmi_cmd_send(sc, WMI_TLV_STA_KEEPALIVE_CMDID, buf, p - buf);
}

/* ---------------------------------------------------------------- scans */

/*
 * A scan of every channel net80211 has: active with the SSID asked for
 * when one is, passive otherwise; the probe requests carry the station's
 * rates and its HT and VHT capabilities. The firmware's start waited for
 * (1 s)
 */
int
athx_wmi_start_scan(struct athx_softc *sc, int vdev)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wmi_tlv_start_scan_cmd *cmd;
	struct wmi_ssid *ssid;
	struct wmi_mac_addr *bssid;
	uint32_t *chans, flags = WMI_SCAN_CHAN_STAT_EVENT;
	uint8_t *buf, *p, ie[128], *iep = ie;
	int n = 0, i, len, ie_len, nssid = ic->ic_des_esslen ? 1 : 0, ret;

	for (i = 1; i <= IEEE80211_CHAN_MAX; i++)
		if (ic->ic_channels[i].ic_flags != 0)
			n++;
	iep = ieee80211_add_rates(iep, &ic->ic_sup_rates[IEEE80211_MODE_11G]);
	iep = ieee80211_add_xrates(iep, &ic->ic_sup_rates[IEEE80211_MODE_11G]);
	if (ic->ic_flags & IEEE80211_F_HTON)
		iep = ieee80211_add_htcaps(iep, ic);
	if (ic->ic_flags & IEEE80211_F_VHTON)
		iep = ieee80211_add_vhtcaps(iep, ic);
	ie_len = iep - ie;
	len = sizeof(struct wmi_tlv) + sizeof(*cmd) + sizeof(struct wmi_tlv) + n * 4 +
	    sizeof(struct wmi_tlv) + nssid * sizeof(struct wmi_ssid) + sizeof(struct wmi_tlv) +
	    sizeof(struct wmi_mac_addr) + sizeof(struct wmi_tlv) + roundup(ie_len, 4);
	buf = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL)
		return ENOMEM;
	if (!nssid)
		flags |= WMI_SCAN_FLAG_PASSIVE;
	p = buf;
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_START_SCAN_CMD, sizeof(*cmd));
	cmd->common.scan_id = htole32(WMI_HOST_SCAN_REQ_ID_PREFIX | ATHX_SCAN_ID);
	cmd->common.scan_req_id = htole32(WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | 1);
	cmd->common.vdev_id = htole32(vdev);
	cmd->common.scan_priority = htole32(WMI_SCAN_PRIORITY_LOW);
	cmd->common.notify_scan_events = htole32(WMI_SCAN_EVENT_STARTED | WMI_SCAN_EVENT_COMPLETED |
	    WMI_SCAN_EVENT_BSS_CHANNEL | WMI_SCAN_EVENT_FOREIGN_CHANNEL | WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT |
	    WMI_SCAN_EVENT_DEQUEUED);
	cmd->common.dwell_time_active = htole32(50);
	cmd->common.dwell_time_passive = htole32(150);
	cmd->common.min_rest_time = htole32(50);
	cmd->common.max_rest_time = htole32(500);
	cmd->common.repeat_probe_time = 0;
	cmd->common.probe_spacing_time = 0;
	cmd->common.idle_time = 0;
	cmd->common.max_scan_time = htole32(20000);
	cmd->common.probe_delay = htole32(5);
	/* the TLV firmware takes this flag the other way round */
	cmd->common.scan_ctrl_flags = htole32(flags ^ WMI_SCAN_FILTER_PROBE_REQ);
	cmd->burst_duration_ms = 0;
	cmd->num_channels = htole32(n);
	cmd->num_ssids = htole32(nssid);
	cmd->num_bssids = htole32(1);
	cmd->ie_len = htole32(ie_len);
	cmd->num_probes = htole32(3);
	chans = tlv_put(&p, WMI_TLV_TAG_ARRAY_UINT32, n * 4);
	for (i = 1, n = 0; i <= IEEE80211_CHAN_MAX; i++)
		if (ic->ic_channels[i].ic_flags != 0)
			chans[n++] = htole32(ic->ic_channels[i].ic_freq);
	ssid = tlv_put(&p, WMI_TLV_TAG_ARRAY_FIXED_STRUCT, nssid * sizeof(struct wmi_ssid));
	if (nssid) {
		ssid->ssid_len = htole32(ic->ic_des_esslen);
		memcpy(ssid->ssid, ic->ic_des_essid, ic->ic_des_esslen);
	}
	bssid = tlv_put(&p, WMI_TLV_TAG_ARRAY_FIXED_STRUCT, sizeof(struct wmi_mac_addr));
	memset(bssid->addr, 0xff, ETH_ALEN);
	memcpy(tlv_put(&p, WMI_TLV_TAG_ARRAY_BYTE, roundup(ie_len, 4)), ie, ie_len);
	sc->scan_started_done = 0;
	ret = wmi_cmd_send(sc, WMI_TLV_START_SCAN_CMDID, buf, len);
	free(buf, M_DEVBUF, len);
	if (ret)
		return ret;
	ret = athx_wait(sc, &sc->scan_started_done, "athxscan", 1000);
	if (ret) {
		printf("%s: the firmware's scan did not start\n", sc->sc_dev.dv_xname);
		(void)athx_wmi_stop_scan(sc, vdev);
		return ETIMEDOUT;
	}
	return sc->scan_state == ATHX_SCAN_IDLE ? EINVAL : 0;
}

int
athx_wmi_stop_scan(struct athx_softc *sc, int vdev)
{
	struct wmi_stop_scan_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.req_type = htole32(WMI_SCAN_STOP_ONE);
	cmd.vdev_id = htole32(vdev);
	cmd.scan_id = htole32(WMI_HOST_SCAN_REQ_ID_PREFIX | ATHX_SCAN_ID);
	cmd.scan_req_id = htole32(WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | 1);
	return wmi_cmd1(sc, WMI_TLV_STOP_SCAN_CMDID, WMI_TLV_TAG_STRUCT_STOP_SCAN_CMD, &cmd, sizeof(cmd));
}

/* ---------------------------------------------------------------- management frames by WMI */

/*
 * A management frame by WMI (when the firmware has the service): its copy
 * in one of HTT's frame buffers, which the chip reads by its address,
 * until the firmware's completion gives the buffer back
 */
int
athx_wmi_mgmt_tx(struct athx_softc *sc, int vdev, struct mbuf *m)
{
	struct athx_htt *htt = &sc->htt;
	struct wmi_tlv_mgmt_tx_cmd *cmd;
	uint8_t *buf, *p;
	int id, flen = m->m_pkthdr.len, blen, len, ret;

	if (flen > ATHX_TX_BUFSZ)
		return EMSGSIZE;
	for (id = 0; id < ATHX_TX_NBUF && htt->tx_busy[id]; id++)
		;
	if (id == ATHX_TX_NBUF)
		return ENOBUFS;
	m_copydata(m, 0, flen, athx_buf_va(&htt->txdata, id));
	blen = roundup(MIN(flen, WMI_TLV_MGMT_TX_FRAME_MAX_LEN), 4);
	len = sizeof(struct wmi_tlv) * 2 + sizeof(*cmd) + blen;
	buf = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL)
		return ENOMEM;
	p = buf;
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_MGMT_TX_CMD, sizeof(*cmd));
	cmd->vdev_id = htole32(vdev);
	cmd->desc_id = htole32(id);
	cmd->chanfreq = 0;
	cmd->buf_len = htole32(blen);
	cmd->frame_len = htole32(flen);
	cmd->paddr = htole64(athx_buf_pa(&htt->txdata, id));
	m_copydata(m, 0, MIN(flen, blen), tlv_put(&p, WMI_TLV_TAG_ARRAY_BYTE, blen));
	htt->tx_busy[id] = 2;
	htt->tx_pending++;
	ret = wmi_cmd_send(sc, WMI_TLV_MGMT_TX_SEND_CMD, buf, len);
	free(buf, M_DEVBUF, len);
	if (ret) {
		htt->tx_busy[id] = 0;
		htt->tx_pending--;
	}
	return ret;
}

/* ---------------------------------------------------------------- events */

static void
ev_mgmt_rx(struct athx_softc *sc, const uint8_t *data, int len)
{
	const struct wmi_tlv_mgmt_rx_ev *ev;
	const uint8_t *frame;
	struct mbuf *m;
	uint32_t status, buf_len;
	int flen;

	ev = tlv_find(data, len, WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, NULL);
	frame = tlv_find(data, len, WMI_TLV_TAG_ARRAY_BYTE, &flen);
	if (ev == NULL || frame == NULL)
		return;
	buf_len = le32toh(ev->buf_len);
	status = le32toh(ev->status);
	if (buf_len > (uint32_t)flen || buf_len < sizeof(struct ieee80211_frame))
		return;
	if (status & (WMI_RX_STATUS_ERR_DECRYPT | WMI_RX_STATUS_ERR_KEY_CACHE_MISS | WMI_RX_STATUS_ERR_CRC))
		return;
	if (status & WMI_RX_STATUS_ERR_MIC) {
		sc->sc_ic.ic_stats.is_ccmp_dec_errs++;
		return;
	}
	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return;
	if (buf_len > MHLEN) {
		MCLGETL(m, M_DONTWAIT, buf_len);
		if (!(m->m_flags & M_EXT)) {
			m_freem(m);
			return;
		}
	}
	memcpy(mtod(m, void *), frame, buf_len);
	m->m_len = m->m_pkthdr.len = buf_len;
	sc->n_mgmt_rx++;
	athx_mgmt_rx(sc, m, le32toh(ev->channel), (int)le32toh(ev->snr) + ATHX_NOISE_FLOOR);
}

static void
ev_scan(struct athx_softc *sc, const uint8_t *data, int len)
{
	const struct wmi_scan_event *ev = tlv_find(data, len, WMI_TLV_TAG_STRUCT_SCAN_EVENT, NULL);

	if (ev == NULL)
		return;
	athx_scan_event(sc, le32toh(ev->event_type), le32toh(ev->reason), le32toh(ev->channel_freq));
}

static void
ev_ready(struct athx_softc *sc, const uint8_t *data, int len)
{
	const struct wmi_tlv_rdy_ev *ev = tlv_find(data, len, WMI_TLV_TAG_STRUCT_READY_EVENT, NULL);

	if (ev == NULL) {
		printf("%s: failed to parse ready event\n", sc->sc_dev.dv_xname);
		return;
	}
	printf("%s: wmi event ready sw_version 0x%08x abi_version %u mac_addr %s status %d\n",
	    sc->sc_dev.dv_xname, le32toh(ev->abi.abi_ver0), le32toh(ev->abi.abi_ver1),
	    ether_sprintf((uint8_t *)ev->mac_addr.addr), le32toh(ev->status));
	memcpy(sc->mac_addr, ev->mac_addr.addr, ETH_ALEN);
	sc->unified_ready = 1;
	wakeup(&sc->unified_ready);
}

static void
ev_vdev_start_resp(struct athx_softc *sc, const uint8_t *data, int len)
{
	const struct wmi_vdev_start_response_event *ev;

	ev = tlv_find(data, len, WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, NULL);
	sc->last_vdev_start_status = 0;
	if (ev == NULL) {
		sc->last_vdev_start_status = EPROTO;
	} else if (le32toh(ev->status) != 0) {
		printf("%s: vdev-start-response reports status error: %d (%s)\n", sc->sc_dev.dv_xname,
		    le32toh(ev->status), le32toh(ev->status) == WMI_VDEV_START_CHAN_INVALID ? "chan-invalid" :
		    "unknown");
		sc->last_vdev_start_status = EINVAL;
	}
	sc->vdev_setup_done = 1;
	wakeup(&sc->vdev_setup_done);
}

static void
ev_roam(struct athx_softc *sc, const uint8_t *data, int len)
{
	const struct wmi_tlv_roam_ev *ev = tlv_find(data, len, WMI_TLV_TAG_STRUCT_ROAM_EVENT, NULL);
	uint32_t reason;

	if (ev == NULL)
		return;
	reason = le32toh(ev->reason);
	printf("%s: wmi roam event vdev %u reason 0x%08x rssi %d\n", sc->sc_dev.dv_xname, le32toh(ev->vdev_id),
	    reason, (int)le32toh(ev->rssi));
	if (reason == WMI_ROAM_REASON_BEACON_MISS)
		athx_beacon_miss(sc, le32toh(ev->vdev_id));
}

static void
ev_mgmt_tx_compl(struct athx_softc *sc, uint32_t desc_id, uint32_t status)
{
	struct athx_htt *htt = &sc->htt;

	if (desc_id >= ATHX_TX_NBUF || htt->tx_busy[desc_id] != 2)
		return;
	htt->tx_busy[desc_id] = 0;
	htt->tx_pending--;
	if (status != 0)
		htt->n_tx_noack++;
	athx_tx_done(sc);
}

/* An event from the firmware, by its ID */
static void
athx_wmi_rx(struct athx_softc *sc, uint8_t *data, int len)
{
	const struct wmi_cmd_hdr *hdr = (const struct wmi_cmd_hdr *)data;
	uint32_t id;
	const void *v;

	if (len < (int)sizeof(*hdr))
		return;
	id = MS(le32toh(hdr->cmd_id), WMI_CMD_HDR_CMD_ID);
	data += sizeof(*hdr);
	len -= sizeof(*hdr);
	sc->n_wmi_ev++;
	switch (id) {
	case WMI_TLV_MGMT_RX_EVENTID:
		ev_mgmt_rx(sc, data, len);
		break;
	case WMI_TLV_SCAN_EVENTID:
		ev_scan(sc, data, len);
		break;
	case WMI_TLV_VDEV_START_RESP_EVENTID:
		ev_vdev_start_resp(sc, data, len);
		break;
	case WMI_TLV_VDEV_STOPPED_EVENTID:
		sc->vdev_setup_done = 1;
		wakeup(&sc->vdev_setup_done);
		break;
	case WMI_TLV_PEER_STA_KICKOUT_EVENTID:
		v = tlv_find(data, len, WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT, NULL);
		if (v != NULL)
			printf("%s: peer %s kicked out by the firmware\n", sc->sc_dev.dv_xname,
			    ether_sprintf((uint8_t *)((const struct wmi_peer_sta_kickout_event *)v)->peer_macaddr.addr));
		break;
	case WMI_TLV_ROAM_EVENTID:
		ev_roam(sc, data, len);
		break;
	case WMI_TLV_SERVICE_READY_EVENTID:
		if (svc_rdy_buf == NULL && len > 0) {
			svc_rdy_buf = malloc(len, M_DEVBUF, M_NOWAIT);
			if (svc_rdy_buf != NULL) {
				memcpy(svc_rdy_buf, data, len);
				svc_rdy_len = len;
			}
		}
		sc->service_ready = 1;
		wakeup(&sc->service_ready);
		break;
	case WMI_TLV_READY_EVENTID:
		ev_ready(sc, data, len);
		break;
	case WMI_TLV_SERVICE_AVAILABLE_EVENTID: {
		int l;

		v = tlv_find(data, len, WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT, &l);
		if (v != NULL && l >= 4) {
			memset(svc_ext, 0, sizeof(svc_ext));
			memcpy(svc_ext, (const uint8_t *)v + 4, MIN((size_t)l - 4, sizeof(svc_ext)));
			svc_ext_valid = 1;
		}
		break;
	}
	case WMI_TLV_ECHO_EVENTID:
		v = tlv_find(data, len, WMI_TLV_TAG_STRUCT_ECHO_EVENT, NULL);
		if (v != NULL && le32toh(((const struct wmi_echo_event *)v)->value) == ATHX_WMI_BARRIER_ECHO_ID) {
			echo_done = 1;
			wakeup(&echo_done);
		}
		break;
	case WMI_TLV_MGMT_TX_COMPLETION_EVENTID:
		v = tlv_find(data, len, WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT, NULL);
		if (v != NULL)
			ev_mgmt_tx_compl(sc, le32toh(((const struct wmi_tlv_mgmt_tx_compl_ev *)v)->desc_id),
			    le32toh(((const struct wmi_tlv_mgmt_tx_compl_ev *)v)->status));
		break;
	case WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID: {
		const uint32_t *num, *ids, *st;
		int i, n;

		num = tlv_find(data, len, WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT, NULL);
		ids = tlv_find_nth(data, len, WMI_TLV_TAG_ARRAY_UINT32, 0, NULL);
		st = tlv_find_nth(data, len, WMI_TLV_TAG_ARRAY_UINT32, 1, NULL);
		if (num == NULL || ids == NULL || st == NULL)
			break;
		n = le32toh(*num);
		for (i = 0; i < n && i < ATHX_TX_NBUF; i++)
			ev_mgmt_tx_compl(sc, le32toh(ids[i]), le32toh(st[i]));
		break;
	}
	default:
		break;
	}
}

/* WMI's control service connected (the endpoint with the target's credits) */
int
athx_wmi_connect(struct athx_softc *sc)
{
	memset(svc_raw, 0, sizeof(svc_raw));
	svc_raw_len = 0;
	svc_ext_valid = 0;
	sc->service_ready = sc->unified_ready = 0;
	return athx_htc_connect(sc, ATH10K_HTC_SVC_ID_WMI_CONTROL, athx_wmi_rx, &sc->wmi_eid);
}

/* ---------------------------------------------------------------- initialisation */

/* The memory the firmware asked for: whole pools, each as one piece the chip reaches */
static int
alloc_host_mem(struct athx_softc *sc, const uint8_t *reqs, int len)
{
	const struct wlan_host_mem_req *r;
	const struct wmi_tlv *t;
	uint32_t req_id, num_units, unit_size, info, pool;
	int l;

	while (len >= (int)sizeof(*t)) {
		t = (const struct wmi_tlv *)reqs;
		l = le16toh(t->len);
		reqs += sizeof(*t);
		len -= sizeof(*t);
		if (l > len || le16toh(t->tag) != WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ ||
		    l < (int)sizeof(*r))
			return EPROTO;
		r = (const struct wlan_host_mem_req *)reqs;
		reqs += l;
		len -= l;
		if (sc->num_mem_chunks >= WMI_MAX_MEM_REQS)
			return ENOMEM;
		req_id = le32toh(r->req_id);
		num_units = le32toh(r->num_units);
		unit_size = le32toh(r->unit_size);
		info = le32toh(r->num_unit_info);
		if (info & NUM_UNITS_IS_NUM_ACTIVE_PEERS)
			num_units = TARGET_TLV_NUM_PEERS + 1;
		else if (info & NUM_UNITS_IS_NUM_PEERS)
			num_units = TARGET_TLV_NUM_PEERS + 1;
		else if (info & NUM_UNITS_IS_NUM_VDEVS)
			num_units = TARGET_TLV_NUM_VDEVS + 1;
		pool = num_units * roundup(unit_size, 4);
		printf("%s: wmi mem_req_id %d num_units %d num_unit_info %d unit size %d: %u bytes\n",
		    sc->sc_dev.dv_xname, req_id, le32toh(r->num_units), info, unit_size, pool);
		if (pool == 0)
			continue;
		if (athx_dma_alloc(sc, &sc->mem_chunks[sc->num_mem_chunks].mem, pool, PAGE_SIZE) != 0) {
			printf("%s: no DMA memory of %u bytes for the firmware\n", sc->sc_dev.dv_xname, pool);
			return ENOMEM;
		}
		sc->mem_chunks[sc->num_mem_chunks].len = pool;
		sc->mem_chunks[sc->num_mem_chunks].req_id = req_id;
		sc->num_mem_chunks++;
	}
	return 0;
}

/* The service ready event taken apart: the chip's capabilities, its services, the memory it wants */
static int
parse_svc_rdy(struct athx_softc *sc)
{
	const struct wmi_tlv_svc_rdy_ev *ev;
	const struct hal_reg_capabilities *reg;
	const uint8_t *bmap, *reqs;
	int blen, rlen, i;

	if (svc_rdy_buf == NULL)
		return ENOMEM;
	ev = tlv_find(svc_rdy_buf, svc_rdy_len, WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, NULL);
	reg = tlv_find(svc_rdy_buf, svc_rdy_len, WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, NULL);
	bmap = tlv_find(svc_rdy_buf, svc_rdy_len, WMI_TLV_TAG_ARRAY_UINT32, &blen);
	reqs = tlv_find(svc_rdy_buf, svc_rdy_len, WMI_TLV_TAG_ARRAY_STRUCT, &rlen);
	if (ev == NULL || reg == NULL || bmap == NULL || reqs == NULL) {
		printf("%s: failed to parse service ready (%p %p %p %p)\n", sc->sc_dev.dv_xname, ev, reg,
		    bmap, reqs);
		return EPROTO;
	}
	if (le32toh(ev->abi.abi_ver0) != WMI_TLV_ABI_VER0 || le32toh(ev->abi.abi_ver_ns0) != WMI_TLV_ABI_VER_NS0 ||
	    le32toh(ev->abi.abi_ver_ns1) != WMI_TLV_ABI_VER_NS1 ||
	    le32toh(ev->abi.abi_ver_ns2) != WMI_TLV_ABI_VER_NS2 ||
	    le32toh(ev->abi.abi_ver_ns3) != WMI_TLV_ABI_VER_NS3) {
		printf("%s: the firmware's WMI ABI %08x is not the one athx speaks\n", sc->sc_dev.dv_xname,
		    le32toh(ev->abi.abi_ver0));
		return EOPNOTSUPP;
	}
	memcpy(svc_raw, bmap, MIN((size_t)blen, sizeof(svc_raw)));
	svc_raw_len = MIN(blen, (int)sizeof(svc_raw));
	sc->hw_min_tx_power = le32toh(ev->hw_min_tx_power);
	sc->hw_max_tx_power = le32toh(ev->hw_max_tx_power);
	sc->ht_cap_info = le32toh(ev->ht_cap_info);
	sc->vht_cap_info = le32toh(ev->vht_cap_info);
	sc->vht_supp_mcs = le32toh(ev->vht_supp_mcs);
	sc->sw_ver0 = le32toh(ev->abi.abi_ver0);
	sc->sw_ver1 = le32toh(ev->abi.abi_ver1);
	sc->phy_capability = le32toh(ev->phy_capability);
	sc->num_rf_chains = le32toh(ev->num_rf_chains);
	sc->sys_cap_info = le32toh(ev->sys_cap_info);
	sc->eeprom_rd = le32toh(reg->eeprom_rd);
	sc->low_2ghz_chan = le32toh(reg->low_2ghz_chan);
	sc->high_2ghz_chan = le32toh(reg->high_2ghz_chan);
	sc->low_5ghz_chan = le32toh(reg->low_5ghz_chan);
	sc->high_5ghz_chan = le32toh(reg->high_5ghz_chan);
	if (sc->num_rf_chains > 4)
		sc->num_rf_chains = 4;
	printf("%s: wmi service ready: fw %u.%u.%u.%u, ht_cap %08x vht_cap %08x vht_mcs %08x, %u rf chains, "
	    "eeprom_rd %04x, 2 GHz %u-%u, 5 GHz %u-%u, %u memory requests\n", sc->sc_dev.dv_xname,
	    (sc->sw_ver0 & 0xff000000) >> 24, sc->sw_ver0 & 0x00ffffff, (sc->sw_ver1 & 0xffff0000) >> 16,
	    sc->sw_ver1 & 0x0000ffff, sc->ht_cap_info, sc->vht_cap_info, sc->vht_supp_mcs, sc->num_rf_chains,
	    sc->eeprom_rd, sc->low_2ghz_chan, sc->high_2ghz_chan, sc->low_5ghz_chan, sc->high_5ghz_chan,
	    le32toh(ev->num_mem_reqs));
	if (le32toh(ev->num_mem_reqs) > WMI_MAX_MEM_REQS) {
		printf("%s: requested memory chunks number (%d) exceeds the limit\n", sc->sc_dev.dv_xname,
		    le32toh(ev->num_mem_reqs));
		return EPROTO;
	}
	for (i = 0; i < sc->num_mem_chunks; i++)
		athx_dma_free(sc, &sc->mem_chunks[i].mem);
	sc->num_mem_chunks = 0;
	return alloc_host_mem(sc, reqs, rlen);
}

/* The init command: the resources the firmware is to set up, and the memory given to it */
static int
send_init(struct athx_softc *sc)
{
	struct wmi_tlv_init_cmd *cmd;
	struct wmi_tlv_resource_config *cfg;
	struct host_memory_chunk_tlv *chunk;
	uint8_t *buf, *p, *chunks;
	int i, len, chunks_len, ret;

	chunks_len = sc->num_mem_chunks * (sizeof(struct host_memory_chunk_tlv) + sizeof(struct wmi_tlv));
	len = sizeof(struct wmi_tlv) + sizeof(*cmd) + sizeof(struct wmi_tlv) + sizeof(*cfg) +
	    sizeof(struct wmi_tlv) + chunks_len;
	buf = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL)
		return ENOMEM;
	p = buf;
	cmd = tlv_put(&p, WMI_TLV_TAG_STRUCT_INIT_CMD, sizeof(*cmd));
	cfg = tlv_put(&p, WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG, sizeof(*cfg));
	chunks = tlv_put(&p, WMI_TLV_TAG_ARRAY_STRUCT, chunks_len);

	cmd->abi.abi_ver0 = htole32(WMI_TLV_ABI_VER0);
	cmd->abi.abi_ver1 = htole32(WMI_TLV_ABI_VER1);
	cmd->abi.abi_ver_ns0 = htole32(WMI_TLV_ABI_VER_NS0);
	cmd->abi.abi_ver_ns1 = htole32(WMI_TLV_ABI_VER_NS1);
	cmd->abi.abi_ver_ns2 = htole32(WMI_TLV_ABI_VER_NS2);
	cmd->abi.abi_ver_ns3 = htole32(WMI_TLV_ABI_VER_NS3);
	cmd->num_host_mem_chunks = htole32(sc->num_mem_chunks);

	cfg->num_vdevs = htole32(TARGET_TLV_NUM_VDEVS);
	cfg->num_peers = htole32(TARGET_TLV_NUM_PEERS);
	cfg->ast_skid_limit = htole32(0x10);
	cfg->num_wds_entries = htole32(0x20);
	if (athx_wmi_svc(sc, WMI_TLV_SERVICE_RX_FULL_REORDER)) {
		cfg->num_offload_peers = htole32(TARGET_TLV_NUM_VDEVS);
		cfg->num_offload_reorder_bufs = htole32(TARGET_TLV_NUM_VDEVS);
	} else {
		cfg->num_offload_peers = 0;
		cfg->num_offload_reorder_bufs = 0;
	}
	cfg->num_peer_keys = htole32(2);
	cfg->num_tids = htole32(TARGET_TLV_NUM_TIDS);
	cfg->tx_chain_mask = htole32(0x7);
	cfg->rx_chain_mask = htole32(0x7);
	cfg->rx_timeout_pri[0] = htole32(0x64);
	cfg->rx_timeout_pri[1] = htole32(0x64);
	cfg->rx_timeout_pri[2] = htole32(0x64);
	cfg->rx_timeout_pri[3] = htole32(0x28);
	cfg->rx_decap_mode = htole32(ATH10K_HW_TXRX_NATIVE_WIFI);
	cfg->scan_max_pending_reqs = htole32(4);
	cfg->bmiss_offload_max_vdev = htole32(TARGET_TLV_NUM_VDEVS);
	cfg->roam_offload_max_vdev = htole32(TARGET_TLV_NUM_VDEVS);
	cfg->roam_offload_max_ap_profiles = htole32(8);
	cfg->num_mcast_groups = 0;
	cfg->num_mcast_table_elems = 0;
	cfg->mcast2ucast_mode = 0;
	cfg->tx_dbg_log_size = htole32(0x400);
	cfg->dma_burst_size = 0;
	cfg->mac_aggr_delim = 0;
	cfg->rx_skip_defrag_timeout_dup_detection_check = 0;
	cfg->vow_config = 0;
	cfg->gtk_offload_max_vdev = htole32(2);
	cfg->num_msdu_desc = htole32(TARGET_TLV_NUM_MSDU_DESC);
	cfg->max_frag_entries = htole32(2);
	cfg->num_tdls_vdevs = htole32(TARGET_TLV_NUM_TDLS_VDEVS);
	cfg->num_tdls_conn_table_entries = htole32(0x20);
	cfg->beacon_tx_offload_max_vdev = htole32(2);
	cfg->num_multicast_filter_entries = htole32(5);
	cfg->num_wow_filters = htole32(TARGET_TLV_NUM_WOW_PATTERNS);
	cfg->num_keep_alive_pattern = htole32(6);
	cfg->keep_alive_pattern_size = 0;
	cfg->max_tdls_concurrent_sleep_sta = htole32(1);
	cfg->max_tdls_concurrent_buffer_sta = htole32(1);
	cfg->wmi_send_separate = 0;
	cfg->num_ocb_vdevs = 0;
	cfg->num_ocb_channels = 0;
	cfg->num_ocb_schedules = 0;
	cfg->host_capab = htole32(WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL);
	if (athx_wmi_svc(sc, WMI_TLV_SERVICE_TX_DATA_MGMT_ACK_RSSI))
		cfg->host_capab |= htole32(WMI_RSRC_CFG_FLAG_TX_ACK_RSSI);

	for (i = 0; i < sc->num_mem_chunks; i++) {
		chunk = tlv_put(&chunks, WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK, sizeof(*chunk));
		chunk->ptr = htole32((uint32_t)sc->mem_chunks[i].mem.paddr);
		chunk->size = htole32(sc->mem_chunks[i].len);
		chunk->req_id = htole32(sc->mem_chunks[i].req_id);
	}
	ret = wmi_cmd_send(sc, WMI_TLV_INIT_CMDID, buf, len);
	free(buf, M_DEVBUF, len);
	return ret;
}

static int
wmi_barrier(struct athx_softc *sc)
{
	struct wmi_echo_cmd cmd;
	int ret;

	echo_done = 0;
	cmd.value = htole32(ATHX_WMI_BARRIER_ECHO_ID);
	ret = wmi_cmd1(sc, WMI_TLV_ECHO_CMDID, WMI_TLV_TAG_STRUCT_ECHO_CMD, &cmd, sizeof(cmd));
	if (ret)
		return ret;
	return athx_wait(sc, &echo_done, "athxecho", 3000);
}

/*
 * The firmware's services read, the init command sent and its ready
 * waited for; the receive filter of the MAC put right by a vdev made and
 * taken away again (some firmware leaves it open otherwise). 0 or an
 * errno value
 */
int
athx_wmi_init(struct athx_softc *sc)
{
	int ret;

	ret = athx_wait(sc, &sc->service_ready, "athxsvc", 5000);
	if (ret == ETIMEDOUT) {
		printf("%s: failed to receive service ready completion, polling..\n", sc->sc_dev.dv_xname);
		athx_ce_poll_all(sc);
		ret = athx_wait(sc, &sc->service_ready, "athxsvc", 5000);
	}
	if (ret) {
		printf("%s: wmi service ready event not received (%d)\n", sc->sc_dev.dv_xname, ret);
		athx_say_regs(sc, "no service ready");
		return ret;
	}
	ret = parse_svc_rdy(sc);
	free(svc_rdy_buf, M_DEVBUF, svc_rdy_len);
	svc_rdy_buf = NULL;
	if (ret)
		return ret;
	sc->mgmt_tx_wmi = athx_wmi_svc(sc, WMI_TLV_SERVICE_MGMT_TX_WMI);
	ret = send_init(sc);
	if (ret) {
		printf("%s: could not send WMI init command (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_wait(sc, &sc->unified_ready, "athxrdy", 5000);
	if (ret) {
		printf("%s: wmi unified ready event not received\n", sc->sc_dev.dv_xname);
		return ret;
	}
	ret = athx_wmi_vdev_create(sc, 0, WMI_VDEV_TYPE_STA, WMI_TLV_VDEV_SUBTYPE_NONE, sc->mac_addr);
	if (ret == 0)
		ret = athx_wmi_vdev_delete(sc, 0);
	if (ret == 0)
		ret = wmi_barrier(sc);
	if (ret)
		printf("%s: failed to reset rx filter: %d\n", sc->sc_dev.dv_xname, ret);
	printf("%s: management frames go by %s\n", sc->sc_dev.dv_xname, sc->mgmt_tx_wmi ? "WMI" : "HTT");
	return ret;
}
