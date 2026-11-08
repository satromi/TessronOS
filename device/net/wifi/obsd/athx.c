/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Copyright (c) 2008-2009 Atheros Communications Inc.
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
 *	athx.c (x64 PC, WIFI_NET80211=obsd)
 *	athx, the driver of Qualcomm's QCA6174 hw3.x on PCI (168c:003e), as
 *	OpenBSD's net80211 sees it (design 9.6.9):
 *
 *	- the chip is brought up once, when the driver attaches (athx_boot):
 *	  its firmware runs from then on and its address is read; the
 *	  interface's up makes the station's vdev and scans, its down takes
 *	  the vdev away again;
 *	- the channels are those the regulatory domain of the chip (its
 *	  EEPROM's word in the service ready event) allows by the chip
 *	  maker's world rules: a world SKU's own, else the most restrictive
 *	  of them; 5 GHz is scanned passively, and its radar band too;
 *	- the states: SCAN starts the firmware's scan (beacons and probe
 *	  responses come up by WMI's management frames, its end ends
 *	  net80211's scan); AUTH starts the vdev on the AP's channel and makes
 *	  the AP a peer, before net80211 sends its authentication; RUN gives
 *	  the firmware the AP's capabilities (peer assoc) and puts the vdev
 *	  up; going back takes each away in turn. What may sleep runs in a
 *	  task, out of net80211's way. There is no background scan (and so
 *	  no roaming to a better AP while associated): the firmware's beacon
 *	  miss takes the station back to SCAN;
 *	- frames: the firmware's own rate control and aggregation (net80211
 *	  sends no ADDBA of its own); data in native Wi-Fi form by HTT,
 *	  management frames by HTT or, when the firmware has the service, by
 *	  WMI; received MPDUs are given to net80211 in the order the
 *	  firmware gives them (no reordering of A-MPDUs on the host);
 *	- keys: CCMP and TKIP in the firmware (the pairwise key authorises the
 *	  peer), BIP and WEP in net80211's software.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/device.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/endian.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#include "athxreg.h"
#include "athxregd.h"
#include "athxvar.h"

struct athx_softc *athx_sc;

/* ---------------------------------------------------------------- the regulatory domain */

/* A rule of the world regulatory domains: its band's edges (MHz), and no initiating radiation */
struct athx_rule {
	uint16_t	start, end;
	uint8_t		no_ir;
	uint8_t		no_ofdm;
};

/* Every world domain allows 1 to 11 actively; the rest by the domain */
#define R_2GHZ_CH01_11	{ 2412 - 10, 2462 + 10, 0, 0 }
#define R_2GHZ_CH12_13	{ 2467 - 10, 2472 + 10, 1, 0 }
#define R_2GHZ_CH14	{ 2484 - 10, 2484 + 10, 1, 1 }
#define R_5GHZ_5150_5350 { 5150 - 10, 5350 + 10, 1, 0 }
#define R_5GHZ_5470_5850 { 5470 - 10, 5850 + 10, 1, 0 }
#define R_5GHZ_5725_5850 { 5725 - 10, 5850 + 10, 1, 0 }

static const struct athx_rule world_60_61_62[] = {
	R_2GHZ_CH01_11, R_2GHZ_CH12_13, R_2GHZ_CH14, R_5GHZ_5150_5350, R_5GHZ_5470_5850,
};
static const struct athx_rule world_63_65[] = {
	R_2GHZ_CH01_11, R_2GHZ_CH12_13, R_5GHZ_5150_5350, R_5GHZ_5725_5850,
};
static const struct athx_rule world_64[] = {
	R_2GHZ_CH01_11, R_5GHZ_5150_5350, R_5GHZ_5725_5850,
};
static const struct athx_rule world_66_69[] = {
	R_2GHZ_CH01_11, R_5GHZ_5150_5350, R_5GHZ_5470_5850,
};
static const struct athx_rule world_67_68_6A_6C[] = {
	R_2GHZ_CH01_11, R_2GHZ_CH12_13, R_5GHZ_5150_5350, R_5GHZ_5470_5850,
};

/* The channels the chip has */
static const uint8_t chans_2ghz[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const uint8_t chans_5ghz[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165, 169, 173,
};

static const struct country_code_to_enum_rd *
find_country(uint16_t cc)
{
	int i;

	for (i = 0; i < (int)nitems(allCountries); i++)
		if (allCountries[i].countryCode == cc)
			return &allCountries[i];
	return NULL;
}

static const struct country_code_to_enum_rd *
find_country_by_rd(int rd)
{
	int i;

	for (i = 0; i < (int)nitems(allCountries); i++)
		if (allCountries[i].regDmnEnum == rd)
			return &allCountries[i];
	return NULL;
}

static const struct reg_dmn_pair_mapping *
get_regpair(int rd)
{
	int i;

	if (rd == NO_ENUMRD)
		return NULL;
	for (i = 0; i < (int)nitems(regDomainPairs); i++)
		if (regDomainPairs[i].reg_domain == rd)
			return &regDomainPairs[i];
	return NULL;
}

static int
is_wwr_sku(uint16_t rd)
{
	return ((rd & COUNTRY_ERD_FLAG) != COUNTRY_ERD_FLAG) &&
	    (((rd & WORLD_SKU_MASK) == WORLD_SKU_PREFIX) || rd == WORLD);
}

/*
 * The regulatory domain pair of the EEPROM's word (a country code or a
 * pair), its CTLs, and the rules of the world domain the channels follow
 */
static void
athx_regd_init(struct athx_softc *sc, const struct athx_rule **rules, int *nrules)
{
	const struct country_code_to_enum_rd *country = NULL;
	const struct reg_dmn_pair_mapping *pair;
	uint16_t cur = sc->eeprom_rd, rd, cc = CTRY_DEFAULT;
	int valid = 0, i;

	/* 0x8000 alone is not a domain: the chip maker's world domain 0x64 in its place */
	if (cur == COUNTRY_ERD_FLAG)
		cur = 0x64;
	rd = cur & ~WORLDWIDE_ROAMING_FLAG;
	if (rd & COUNTRY_ERD_FLAG) {
		valid = find_country(rd & ~COUNTRY_ERD_FLAG) != NULL;
		if (valid)
			cc = rd & ~COUNTRY_ERD_FLAG;
	} else {
		for (i = 0; i < (int)nitems(regDomainPairs); i++)
			if (regDomainPairs[i].reg_domain == rd)
				valid = 1;
	}
	pair = NULL;
	if (valid) {
		if (cc == CTRY_DEFAULT && rd == CTRY_DEFAULT)
			cc = CTRY_UNITED_STATES;
		if (cc != CTRY_DEFAULT) {
			country = find_country(cc);
			if (country != NULL)
				rd = country->regDmnEnum;
		}
		pair = get_regpair(rd);
		if (country == NULL)
			country = find_country_by_rd(rd);
	}
	if (pair == NULL) {
		printf("%s: invalid regulatory domain/country code 0x%x: the world domain 0x64\n",
		    sc->sc_dev.dv_xname, sc->eeprom_rd);
		pair = get_regpair(0x64);
		country = NULL;
	}
	sc->reg_domain = pair->reg_domain;
	sc->reg_5ghz_ctl = pair->reg_5ghz_ctl;
	sc->reg_2ghz_ctl = pair->reg_2ghz_ctl;
	if (country != NULL) {
		sc->alpha2[0] = country->isoName[0];
		sc->alpha2[1] = country->isoName[1];
	} else {
		sc->alpha2[0] = sc->alpha2[1] = '0';
	}
	sc->alpha2[2] = '\0';
	*rules = world_64;
	*nrules = nitems(world_64);
	if (is_wwr_sku(cur & ~WORLDWIDE_ROAMING_FLAG)) {
		switch (sc->reg_domain) {
		case 0x60: case 0x61: case 0x62:
			*rules = world_60_61_62;
			*nrules = nitems(world_60_61_62);
			break;
		case 0x63: case 0x65:
			*rules = world_63_65;
			*nrules = nitems(world_63_65);
			break;
		case 0x66: case 0x69:
			*rules = world_66_69;
			*nrules = nitems(world_66_69);
			break;
		case 0x67: case 0x68: case 0x6A: case 0x6C:
			*rules = world_67_68_6A_6C;
			*nrules = nitems(world_67_68_6A_6C);
			break;
		default:
			break;
		}
	}
	printf("%s: EEPROM regdomain 0x%x: regpair 0x%x (CTL 2 GHz 0x%x, 5 GHz 0x%x), country %s, %s rules\n",
	    sc->sc_dev.dv_xname, sc->eeprom_rd, sc->reg_domain, sc->reg_2ghz_ctl, sc->reg_5ghz_ctl, sc->alpha2,
	    is_wwr_sku(cur & ~WORLDWIDE_ROAMING_FLAG) ? "its world domain's" : "the most restrictive world");
}

static const struct athx_rule *
rule_of(const struct athx_rule *rules, int n, u_int freq)
{
	int i;

	for (i = 0; i < n; i++)
		if (freq - 10 >= rules[i].start && freq + 10 <= rules[i].end)
			return &rules[i];
	return NULL;
}

/* net80211's channels: those the rules allow, passive where they say so and in the radar band */
static void
athx_setup_channels(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct athx_rule *rules, *r;
	struct ieee80211_channel *c;
	int nrules, i, n2 = 0, n5 = 0;
	u_int freq;

	athx_regd_init(sc, &rules, &nrules);
	memset(ic->ic_channels, 0, sizeof(ic->ic_channels));
	for (i = 0; i < (int)nitems(chans_2ghz); i++) {
		freq = ieee80211_ieee2mhz(chans_2ghz[i], IEEE80211_CHAN_2GHZ);
		r = rule_of(rules, nrules, freq);
		if (r == NULL)
			continue;
		c = &ic->ic_channels[chans_2ghz[i]];
		c->ic_freq = freq;
		c->ic_flags = IEEE80211_CHAN_CCK | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
		if (!r->no_ofdm)
			c->ic_flags |= IEEE80211_CHAN_OFDM | IEEE80211_CHAN_HT;
		if (r->no_ir)
			c->ic_flags |= IEEE80211_CHAN_PASSIVE;
		n2++;
	}
	for (i = 0; i < (int)nitems(chans_5ghz); i++) {
		freq = ieee80211_ieee2mhz(chans_5ghz[i], IEEE80211_CHAN_5GHZ);
		/* the 5 GHz channels outside what the chip says it has */
		if (sc->low_5ghz_chan && sc->high_5ghz_chan &&
		    (freq < sc->low_5ghz_chan || freq > sc->high_5ghz_chan))
			continue;
		r = rule_of(rules, nrules, freq);
		if (r == NULL)
			continue;
		c = &ic->ic_channels[chans_5ghz[i]];
		c->ic_freq = freq;
		c->ic_flags = IEEE80211_CHAN_A | IEEE80211_CHAN_HT | IEEE80211_CHAN_VHT;
		if (r->no_ir || (freq >= 5260 && freq <= 5700))
			c->ic_flags |= IEEE80211_CHAN_PASSIVE;
		n5++;
	}
	printf("%s: %d channels of 2 GHz, %d of 5 GHz\n", sc->sc_dev.dv_xname, n2, n5);
}

/* ---------------------------------------------------------------- the chip's settings */

/* The pdev's settings, the channels and the regulatory domain given to the firmware */
static int
athx_pdev_setup(struct athx_softc *sc)
{
	uint32_t chains = (1 << sc->num_rf_chains) - 1;
	int ret;

	ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_PMF_QOS, 1);
	if (ret == 0)
		ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_DYNAMIC_BW, 1);
	if (ret == 0 && athx_wmi_svc(sc, WMI_TLV_SERVICE_BURST))
		ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_BURST_ENABLE, 0);
	if (ret == 0)
		(void)athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_IDLE_PS_CONFIG, 1);
	if (ret == 0 && chains != 0) {
		ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_TX_CHAIN_MASK, chains);
		if (ret == 0)
			ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_RX_CHAIN_MASK, chains);
	}
	/* ARP frames in best effort, not in voice where the firmware puts them */
	if (ret == 0)
		ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_ARP_AC_OVERRIDE, 0);
	if (ret == 0)
		ret = athx_wmi_pdev_set_param(sc, WMI_TLV_PDEV_PARAM_ANI_ENABLE, 1);
	if (ret) {
		printf("%s: failed to set the pdev's parameters: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_wmi_scan_chan_list(sc);
	if (ret)
		printf("%s: failed to update channel list: %d\n", sc->sc_dev.dv_xname, ret);
	ret = athx_wmi_pdev_set_regdomain(sc, sc->reg_domain, sc->reg_domain, sc->reg_domain, sc->reg_2ghz_ctl,
	    sc->reg_5ghz_ctl);
	if (ret)
		printf("%s: failed to set pdev regdomain: %d\n", sc->sc_dev.dv_xname, ret);
	return ret;
}

/* The station's vdev: made with the chip's address, its power save off, the firmware's keepalive off */
static int
athx_vdev_create(struct athx_softc *sc)
{
	int ret;

	if (sc->vdev_created)
		return 0;
	sc->vdev_id = 0;
	ret = athx_wmi_vdev_create(sc, sc->vdev_id, WMI_VDEV_TYPE_STA, WMI_TLV_VDEV_SUBTYPE_NONE, sc->mac_addr);
	if (ret) {
		printf("%s: failed to create WMI vdev %d: %d\n", sc->sc_dev.dv_xname, sc->vdev_id, ret);
		return ret;
	}
	sc->vdev_created = 1;
	ret = athx_wmi_sta_keepalive_disable(sc, sc->vdev_id);
	if (ret == 0 && sc->num_rf_chains)
		ret = athx_wmi_vdev_set_param(sc, sc->vdev_id, WMI_TLV_VDEV_PARAM_NSS, sc->num_rf_chains);
	if (ret == 0)
		ret = athx_wmi_set_sta_ps_param(sc, sc->vdev_id, WMI_STA_PS_PARAM_RX_WAKE_POLICY,
		    WMI_STA_PS_RX_WAKE_POLICY_WAKE);
	if (ret == 0)
		ret = athx_wmi_set_sta_ps_param(sc, sc->vdev_id, WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD,
		    WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS);
	if (ret == 0)
		ret = athx_wmi_set_sta_ps_param(sc, sc->vdev_id, WMI_STA_PS_PARAM_PSPOLL_COUNT,
		    WMI_STA_PS_PSPOLL_COUNT_NO_MAX);
	if (ret == 0)
		ret = athx_wmi_set_psmode(sc, sc->vdev_id, WMI_STA_PS_MODE_DISABLED);
	if (ret)
		printf("%s: failed to set up vdev %d: %d\n", sc->sc_dev.dv_xname, sc->vdev_id, ret);
	return ret;
}

static void
athx_vdev_delete(struct athx_softc *sc)
{
	if (!sc->vdev_created)
		return;
	(void)athx_wmi_vdev_delete(sc, sc->vdev_id);
	sc->vdev_created = 0;
}

/* ---------------------------------------------------------------- scans */

static void
athx_scan_finish(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;

	if (sc->scan_state == ATHX_SCAN_IDLE)
		return;
	timeout_del(&sc->scan_to);
	sc->scan_state = ATHX_SCAN_IDLE;
	wakeup(&sc->scan_state);
	if (ic->ic_state == IEEE80211_S_SCAN && (ifp->if_flags & IFF_RUNNING))
		ieee80211_end_scan(ifp);
}

/* The scan of the firmware, as its events go */
void
athx_scan_event(struct athx_softc *sc, uint32_t type, uint32_t reason, uint32_t freq)
{
	ATHX_DPRINTF(sc, "%s: scan event %u reason %u freq %u state %d\n", sc->sc_dev.dv_xname, type, reason,
	    freq, sc->scan_state);
	switch (type) {
	case WMI_SCAN_EVENT_STARTED:
		if (sc->scan_state == ATHX_SCAN_STARTING)
			sc->scan_state = ATHX_SCAN_RUNNING;
		sc->scan_started_done = 1;
		wakeup(&sc->scan_started_done);
		break;
	case WMI_SCAN_EVENT_START_FAILED:
		printf("%s: received scan start failure event\n", sc->sc_dev.dv_xname);
		if (sc->scan_state == ATHX_SCAN_STARTING) {
			sc->scan_started_done = 1;
			wakeup(&sc->scan_started_done);
			athx_scan_finish(sc);
		}
		break;
	case WMI_SCAN_EVENT_COMPLETED:
		if (sc->scan_state == ATHX_SCAN_RUNNING || sc->scan_state == ATHX_SCAN_ABORTING)
			athx_scan_finish(sc);
		break;
	default:
		break;
	}
}

/* A scan the firmware never ended: given up on */
static void
athx_scan_timeout(void *arg)
{
	struct athx_softc *sc = arg;
	int s = splnet();

	printf("%s: the firmware's scan did not end; given up\n", sc->sc_dev.dv_xname);
	athx_scan_finish(sc);
	splx(s);
}

static int
athx_scan(struct athx_softc *sc)
{
	int ret;

	if (sc->scan_state != ATHX_SCAN_IDLE)
		return EBUSY;
	sc->scan_state = ATHX_SCAN_STARTING;
	ret = athx_wmi_start_scan(sc, sc->vdev_id);
	if (ret) {
		printf("%s: failed to start hw scan: %d\n", sc->sc_dev.dv_xname, ret);
		sc->scan_state = ATHX_SCAN_IDLE;
		return ret;
	}
	/* the firmware's longest scan (20 s), and its events' time */
	timeout_add_msec(&sc->scan_to, 20000 + 200);
	return 0;
}

static void
athx_scan_abort(struct athx_softc *sc)
{
	int i;

	if (sc->scan_state != ATHX_SCAN_RUNNING)
		return;
	sc->scan_state = ATHX_SCAN_ABORTING;
	if (athx_wmi_stop_scan(sc, sc->vdev_id) != 0) {
		athx_scan_finish(sc);
		return;
	}
	/* its completed event ends it; at most 3 s */
	for (i = 0; i < 30 && sc->scan_state != ATHX_SCAN_IDLE && !sc->wedged; i++)
		(void)tsleep_nsec(&sc->scan_state, 0, "athxabrt", MSEC_TO_NSEC(100));
	if (sc->scan_state != ATHX_SCAN_IDLE) {
		timeout_del(&sc->scan_to);
		sc->scan_state = ATHX_SCAN_IDLE;
	}
}

/* ---------------------------------------------------------------- the states */

/* Before net80211 authenticates: the vdev on the AP's channel, the AP made a peer */
static int
athx_auth(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int ret;

	ret = athx_wmi_vdev_set_param(sc, sc->vdev_id, WMI_TLV_VDEV_PARAM_BEACON_INTERVAL, ni->ni_intval);
	if (ret)
		return ret;
	if (sc->vdev_started) {
		(void)athx_wmi_vdev_stop(sc, sc->vdev_id);
		sc->vdev_started = 0;
	}
	ret = athx_wmi_vdev_start(sc, sc->vdev_id, ni);
	if (ret) {
		printf("%s: failed to start vdev %d on %d MHz: %d\n", sc->sc_dev.dv_xname, sc->vdev_id,
		    ni->ni_chan->ic_freq, ret);
		return ret;
	}
	sc->vdev_started = 1;
	if (sc->bss_peer && memcmp(sc->bss_addr, ni->ni_macaddr, ETH_ALEN) != 0) {
		(void)athx_wmi_peer_delete(sc, sc->vdev_id, sc->bss_addr);
		sc->bss_peer = 0;
	}
	if (!sc->bss_peer) {
		ret = athx_wmi_peer_create(sc, sc->vdev_id, ni->ni_macaddr);
		if (ret)
			return ret;
		memcpy(sc->bss_addr, ni->ni_macaddr, ETH_ALEN);
		sc->bss_peer = 1;
	}
	printf("%s: joining %s on %d MHz\n", sc->sc_dev.dv_xname, ether_sprintf(ni->ni_macaddr),
	    ni->ni_chan->ic_freq);
	return 0;
}

/* Back from AUTH or ASSOC: the AP's peer and the vdev taken away */
static int
athx_deauth(struct athx_softc *sc)
{
	if (sc->bss_peer) {
		(void)athx_wmi_peer_delete(sc, sc->vdev_id, sc->bss_addr);
		sc->bss_peer = 0;
	}
	if (sc->vdev_started) {
		(void)athx_wmi_vdev_stop(sc, sc->vdev_id);
		sc->vdev_started = 0;
	}
	return 0;
}

/* Associated: the AP's capabilities to the firmware, the vdev up */
static int
athx_run(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	static const uint32_t smps_map[4] = {
		WMI_PEER_SMPS_STATIC, WMI_PEER_SMPS_DYNAMIC, WMI_PEER_SMPS_PS_NONE, WMI_PEER_SMPS_PS_NONE
	};
	int ret;

	ret = athx_wmi_peer_assoc(sc, sc->vdev_id, ni);
	if (ret) {
		printf("%s: failed to run peer assoc for %s: %d\n", sc->sc_dev.dv_xname,
		    ether_sprintf(ni->ni_macaddr), ret);
		return ret;
	}
	if (ni->ni_flags & IEEE80211_NODE_HT)
		(void)athx_wmi_peer_set_param(sc, sc->vdev_id, ni->ni_macaddr, WMI_TLV_PEER_SMPS_STATE,
		    smps_map[(ni->ni_htcaps & IEEE80211_HTCAP_SMPS_MASK) >> IEEE80211_HTCAP_SMPS_SHIFT]);
	(void)athx_wmi_vdev_set_param(sc, sc->vdev_id, WMI_TLV_VDEV_PARAM_SLOT_TIME,
	    (ic->ic_flags & IEEE80211_F_SHSLOT) ? WMI_VDEV_SLOT_TIME_SHORT : WMI_VDEV_SLOT_TIME_LONG);
	(void)athx_wmi_vdev_set_param(sc, sc->vdev_id, WMI_TLV_VDEV_PARAM_PREAMBLE,
	    (ic->ic_flags & IEEE80211_F_SHPREAMBLE) ? WMI_VDEV_PREAMBLE_SHORT : WMI_VDEV_PREAMBLE_LONG);
	if (ni->ni_dtimperiod)
		(void)athx_wmi_vdev_set_param(sc, sc->vdev_id, WMI_TLV_VDEV_PARAM_DTIM_PERIOD, ni->ni_dtimperiod);
	ret = athx_wmi_vdev_up(sc, sc->vdev_id, ni->ni_associd & 0x3fff, ni->ni_bssid);
	if (ret) {
		printf("%s: failed to set vdev %d up: %d\n", sc->sc_dev.dv_xname, sc->vdev_id, ret);
		return ret;
	}
	sc->vdev_up = 1;
	/* the firmware's power save state machine needs a peer command after the vdev is up */
	(void)athx_wmi_peer_set_param(sc, sc->vdev_id, ni->ni_bssid, WMI_TLV_PEER_DUMMY_VAR, 1);
	/* an open network's data passes at once; a protected one's once the pairwise key is in */
	if (!(ic->ic_flags & IEEE80211_F_RSNON))
		(void)athx_wmi_peer_set_param(sc, sc->vdev_id, ni->ni_macaddr, WMI_TLV_PEER_AUTHORIZE, 1);
	printf("%s: vdev %d up: bssid %s aid %d, %s%s\n", sc->sc_dev.dv_xname, sc->vdev_id,
	    ether_sprintf(ni->ni_bssid), ni->ni_associd & 0x3fff,
	    (ni->ni_flags & IEEE80211_NODE_VHT) ? "VHT" : (ni->ni_flags & IEEE80211_NODE_HT) ? "HT" : "legacy",
	    (ic->ic_flags & IEEE80211_F_RSNON) ? ", protected" : "");
	return 0;
}

static int
athx_run_stop(struct athx_softc *sc)
{
	if (sc->bss_peer)
		(void)athx_wmi_peer_set_param(sc, sc->vdev_id, sc->bss_addr, WMI_TLV_PEER_AUTHORIZE, 0);
	if (sc->vdev_up) {
		(void)athx_wmi_vdev_down(sc, sc->vdev_id);
		sc->vdev_up = 0;
	}
	return 0;
}

static void
athx_setkey_clear(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	while (sc->setkey_nkeys > 0) {
		ieee80211_release_node(ic, sc->setkey_arg[sc->setkey_tail].ni);
		sc->setkey_arg[sc->setkey_tail].ni = NULL;
		sc->setkey_tail = (sc->setkey_tail + 1) % nitems(sc->setkey_arg);
		sc->setkey_nkeys--;
	}
	sc->setkey_cur = sc->setkey_tail = 0;
}

static int
athx_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct ifnet *ifp = &ic->ic_if;
	struct athx_softc *sc = ifp->if_softc;

	if (!(ifp->if_flags & IFF_RUNNING) || sc->wedged)
		return 0;
	if (sc->ns_pending && sc->ns_nstate == nstate && nstate != IEEE80211_S_SCAN &&
	    nstate != IEEE80211_S_AUTH)
		return 0;
	if (ic->ic_state == IEEE80211_S_RUN) {
		task_del(systq, &sc->setkey_task);
		athx_setkey_clear(sc);
	}
	sc->ns_nstate = nstate;
	sc->ns_arg = arg;
	sc->ns_pending = 1;
	task_add(systq, &sc->newstate_task);
	return 0;
}

static void
athx_newstate_task(void *arg)
{
	struct athx_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	enum ieee80211_state nstate = sc->ns_nstate, ostate = ic->ic_state;
	int err = 0, s = splnet();

	sc->ns_pending = 0;
	if (!(ifp->if_flags & IFF_RUNNING) || sc->wedged) {
		splx(s);
		return;
	}
	if (ostate == IEEE80211_S_SCAN && nstate == ostate) {
		if (sc->scan_state != ATHX_SCAN_IDLE) {
			splx(s);
			return;
		}
		goto next_scan;
	}
	if (nstate <= ostate) {
		switch (ostate) {
		case IEEE80211_S_RUN:
			athx_run_stop(sc);
			/* FALLTHROUGH */
		case IEEE80211_S_ASSOC:
		case IEEE80211_S_AUTH:
			if (nstate <= IEEE80211_S_AUTH)
				athx_deauth(sc);
			/* FALLTHROUGH */
		case IEEE80211_S_SCAN:
			if (sc->scan_state == ATHX_SCAN_RUNNING)
				athx_scan_abort(sc);
			if (nstate == IEEE80211_S_SCAN)
				ieee80211_free_allnodes(ic, 0);
			break;
		case IEEE80211_S_INIT:
			break;
		}
	}
	switch (nstate) {
	case IEEE80211_S_INIT:
		break;
	case IEEE80211_S_SCAN:
 next_scan:
		err = athx_scan(sc);
		if (err)
			break;
		if (ifp->if_flags & IFF_DEBUG)
			printf("%s: %s -> %s\n", ifp->if_xname, ieee80211_state_name[ic->ic_state],
			    ieee80211_state_name[IEEE80211_S_SCAN]);
		ieee80211_set_link_state(ic, LINK_STATE_DOWN);
		ieee80211_node_cleanup(ic, ic->ic_bss);
		ic->ic_state = IEEE80211_S_SCAN;
		splx(s);
		return;
	case IEEE80211_S_AUTH:
		err = athx_auth(sc);
		break;
	case IEEE80211_S_ASSOC:
		break;
	case IEEE80211_S_RUN:
		err = athx_run(sc);
		break;
	}
	if (err)
		printf("%s: the change to %s failed: %d\n", sc->sc_dev.dv_xname, ieee80211_state_name[nstate], err);
	if (err && nstate != IEEE80211_S_INIT && (ifp->if_flags & IFF_RUNNING)) {
		/* back to scanning: the firmware keeps nothing of the failed join */
		athx_deauth(sc);
		sc->sc_newstate(ic, IEEE80211_S_INIT, -1);
		ieee80211_begin_scan(ifp);
	} else {
		sc->sc_newstate(ic, nstate, sc->ns_arg);
	}
	splx(s);
}

/* The firmware missed the AP's beacons: net80211 told, which probes it or scans again */
static void
athx_bmiss_task(void *arg)
{
	struct athx_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	int s = splnet();

	if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL) {
		printf("%s: beacon miss: the link to %s is lost\n", sc->sc_dev.dv_xname,
		    ether_sprintf(ic->ic_bss->ni_bssid));
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	}
	splx(s);
}

void
athx_beacon_miss(struct athx_softc *sc, uint32_t vdev)
{
	if (vdev != (uint32_t)sc->vdev_id || !sc->vdev_up)
		return;
	task_add(systq, &sc->bmiss_task);
}

/* ---------------------------------------------------------------- keys */

static int
athx_queue_setkey(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k, int add)
{
	struct athx_softc *sc = ic->ic_softc;

	if (sc->setkey_nkeys >= (int)nitems(sc->setkey_arg) || k->k_id > WMI_MAX_KEY_INDEX)
		return ENOSPC;
	sc->setkey_arg[sc->setkey_cur].ni = ieee80211_ref_node(ni);
	sc->setkey_arg[sc->setkey_cur].k = k;
	sc->setkey_arg[sc->setkey_cur].add = add;
	sc->setkey_cur = (sc->setkey_cur + 1) % nitems(sc->setkey_arg);
	sc->setkey_nkeys++;
	task_add(systq, &sc->setkey_task);
	return EBUSY;
}

static int
athx_set_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	if (k->k_cipher != IEEE80211_CIPHER_CCMP && k->k_cipher != IEEE80211_CIPHER_TKIP)
		return ieee80211_set_key(ic, ni, k);
	if (ni == NULL)
		ni = ic->ic_bss;
	return athx_queue_setkey(ic, ni, k, 1);
}

static void
athx_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni, struct ieee80211_key *k)
{
	if (k->k_cipher != IEEE80211_CIPHER_CCMP && k->k_cipher != IEEE80211_CIPHER_TKIP) {
		ieee80211_delete_key(ic, ni, k);
		return;
	}
	if (ic->ic_state != IEEE80211_S_RUN)
		return;		/* the firmware's keys go with its peer */
	if (ni == NULL)
		ni = ic->ic_bss;
	(void)athx_queue_setkey(ic, ni, k, 0);
}

static void
athx_setkey_task(void *arg)
{
	struct athx_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ieee80211_key *k;
	uint32_t flags;
	int s = splnet(), add, err;

	while (sc->setkey_nkeys > 0) {
		ni = sc->setkey_arg[sc->setkey_tail].ni;
		k = sc->setkey_arg[sc->setkey_tail].k;
		add = sc->setkey_arg[sc->setkey_tail].add;
		if (ic->ic_state == IEEE80211_S_RUN && sc->bss_peer && !sc->wedged) {
			flags = (k->k_flags & IEEE80211_KEY_GROUP) ? WMI_KEY_GROUP : WMI_KEY_PAIRWISE;
			err = athx_wmi_vdev_install_key(sc, sc->vdev_id, sc->bss_addr, k, flags, !add);
			printf("%s: %s key %d (%s, %s): %d\n", sc->sc_dev.dv_xname, add ? "installed" : "deleted",
			    k->k_id, (k->k_flags & IEEE80211_KEY_GROUP) ? "group" : "pairwise",
			    k->k_cipher == IEEE80211_CIPHER_CCMP ? "CCMP" : "TKIP", err);
			if (err == 0 && add && !(k->k_flags & IEEE80211_KEY_GROUP))
				(void)athx_wmi_peer_set_param(sc, sc->vdev_id, sc->bss_addr, WMI_TLV_PEER_AUTHORIZE, 1);
		}
		ieee80211_release_node(ic, ni);
		sc->setkey_arg[sc->setkey_tail].ni = NULL;
		sc->setkey_tail = (sc->setkey_tail + 1) % nitems(sc->setkey_arg);
		sc->setkey_nkeys--;
	}
	splx(s);
}

/* ---------------------------------------------------------------- frames */

/* A management frame by WMI: its channel, its signal in dBm */
void
athx_mgmt_rx(struct athx_softc *sc, struct mbuf *m, uint32_t channel, int rssi)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_rxinfo rxi;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;

	if (!(ifp->if_flags & IFF_RUNNING) || m->m_pkthdr.len < (int)sizeof(*wh)) {
		m_freem(m);
		return;
	}
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_chan = channel;
	rxi.rxi_rssi = rssi;
	wh = mtod(m, struct ieee80211_frame *);
	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) && !IEEE80211_IS_MULTICAST(wh->i_addr1))
		rxi.rxi_flags |= IEEE80211_RXI_HWDEC;
	ni = ieee80211_find_rxnode(ic, wh);
	ieee80211_input(ifp, m, ni, &rxi);
	ieee80211_release_node(ic, ni);
}

/* A data frame put back together by HTT */
void
athx_data_rx(struct athx_softc *sc, struct mbuf *m, int rssi, uint32_t rxi_flags)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_rxinfo rxi;
	struct ieee80211_node *ni;

	if (!(ifp->if_flags & IFF_RUNNING) || ic->ic_state < IEEE80211_S_ASSOC) {
		m_freem(m);
		return;
	}
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = rssi;
	rxi.rxi_chan = ic->ic_bss != NULL ? ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) : 0;
	rxi.rxi_flags = rxi_flags;
	ni = ieee80211_find_rxnode(ic, mtod(m, struct ieee80211_frame *));
	ieee80211_input(ifp, m, ni, &rxi);
	ieee80211_release_node(ic, ni);
}

/* A frame sent: what waits goes on */
void
athx_tx_done(struct athx_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	sc->sc_tx_timer = 0;
	if (ifq_is_oactive(&ifp->if_snd) && sc->htt.tx_pending < ATHX_TX_NBUF - 8) {
		ifq_clr_oactive(&ifp->if_snd);
		(*ifp->if_start)(ifp);
	}
}

static int
athx_tx(struct athx_softc *sc, struct mbuf *m, struct ieee80211_node *ni)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	if (type == IEEE80211_FC0_TYPE_MGT) {
		if (sc->mgmt_tx_wmi)
			return athx_wmi_mgmt_tx(sc, sc->vdev_id, m);
		if (sc->htt.ver_major >= 3)
			return athx_htt_tx(sc, sc->vdev_id, m, ATH10K_HW_TXRX_MGMT,
			    !(wh->i_fc[1] & IEEE80211_FC1_PROTECTED));
		return EOPNOTSUPP;
	}
	/* a frame without the protected bit goes out as it is (EAPOL before the keys, an open network) */
	return athx_htt_tx(sc, sc->vdev_id, m, ATH10K_HW_TXRX_NATIVE_WIFI,
	    !(wh->i_fc[1] & IEEE80211_FC1_PROTECTED));
}

static void
athx_start(struct ifnet *ifp)
{
	struct athx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ether_header *eh;
	struct mbuf *m;
	int err;

	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd) || sc->wedged)
		return;
	for (;;) {
		if (sc->htt.tx_pending >= ATHX_TX_NBUF - 2) {
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		m = mq_dequeue(&ic->ic_mgtq);
		if (m != NULL) {
			ni = m->m_pkthdr.ph_cookie;
			goto sendit;
		}
		if (ic->ic_state != IEEE80211_S_RUN || (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY))
			break;
		m = ifq_dequeue(&ifp->if_snd);
		if (m == NULL)
			break;
		if (m->m_len < (int)sizeof(*eh) && (m = m_pullup(m, sizeof(*eh))) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
 sendit:
		err = athx_tx(sc, m, ni);
		m_freem(m);
		if (ni != NULL)
			ieee80211_release_node(ic, ni);
		if (err) {
			ifp->if_oerrors++;
			if (err == ENOBUFS) {
				ifq_set_oactive(&ifp->if_snd);
				break;
			}
			continue;
		}
		if (ifp->if_flags & IFF_UP) {
			sc->sc_tx_timer = 15;
			ifp->if_timer = 1;
		}
	}
}

static void
athx_watchdog(struct ifnet *ifp)
{
	struct athx_softc *sc = ifp->if_softc;

	ifp->if_timer = 0;
	if (sc->sc_tx_timer > 0) {
		if (--sc->sc_tx_timer == 0) {
			printf("%s: device timeout: %d frames in flight, %u completed, %u not acknowledged, "
			    "%u discarded\n", sc->sc_dev.dv_xname, sc->htt.tx_pending, sc->htt.n_tx_compl,
			    sc->htt.n_tx_noack, sc->htt.n_tx_discard);
			athx_say_regs(sc, "device timeout");
			ifp->if_oerrors++;
			return;
		}
		ifp->if_timer = 1;
	}
	ieee80211_watchdog(ifp);
}

/* ---------------------------------------------------------------- the interface */

static int
athx_init(struct ifnet *ifp)
{
	struct athx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int err;

	if (sc->wedged || sc->stage < ATHX_ST_UP) {
		printf("%s: the chip is not running (stage %d%s)\n", sc->sc_dev.dv_xname, sc->stage,
		    sc->wedged ? ", firmware crashed" : "");
		return EIO;
	}
	err = athx_vdev_create(sc);
	if (err)
		return err;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ic->ic_state = IEEE80211_S_INIT;
	sc->ns_nstate = IEEE80211_S_INIT;
	sc->scan_state = ATHX_SCAN_IDLE;
	ieee80211_begin_scan(ifp);
	return 0;
}

static void
athx_stop(struct ifnet *ifp)
{
	struct athx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int was_running = (ifp->if_flags & IFF_RUNNING) != 0;

	task_del(systq, &sc->newstate_task);
	task_del(systq, &sc->setkey_task);
	task_del(systq, &sc->bmiss_task);
	athx_setkey_clear(sc);
	ifp->if_timer = sc->sc_tx_timer = 0;
	if (was_running && !sc->wedged) {
		if (sc->scan_state == ATHX_SCAN_RUNNING)
			athx_scan_abort(sc);
		athx_run_stop(sc);
		athx_deauth(sc);
		athx_vdev_delete(sc);
	}
	timeout_del(&sc->scan_to);
	sc->scan_state = ATHX_SCAN_IDLE;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	if (ic->ic_state != IEEE80211_S_INIT)
		sc->sc_newstate(ic, IEEE80211_S_INIT, -1);
}

static int
athx_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int s, err = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING)) {
				err = athx_init(ifp);
				if (err)
					athx_stop(ifp);
			}
		} else if (ifp->if_flags & IFF_RUNNING) {
			athx_stop(ifp);
		}
		break;
	default:
		err = ieee80211_ioctl(ifp, cmd, data);
		break;
	}
	if (err == ENETRESET) {
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) {
			athx_stop(ifp);
			err = athx_init(ifp);
		}
	}
	splx(s);
	return err;
}

static int
athx_media_change(struct ifnet *ifp)
{
	int err;

	err = ieee80211_media_change(ifp);
	if (err != ENETRESET)
		return err;
	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING)) {
		athx_stop(ifp);
		err = athx_init(ifp);
	}
	return err;
}

/* ---------------------------------------------------------------- attaching */

int
athx_match(struct pci_attach_args *pa)
{
	return PCI_VENDOR(pa->pa_id) == 0x168c && PCI_PRODUCT(pa->pa_id) == QCA6174_2_1_DEVICE_ID;
}

/* What the chip can do, as net80211 takes it, from the firmware's service ready event */
static void
athx_caps(struct athx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	int i, nss = sc->num_rf_chains ? sc->num_rf_chains : 1;

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_QOS | IEEE80211_C_RSN | IEEE80211_C_SCANALL | IEEE80211_C_SCANALLBAND |
	    IEEE80211_C_SHSLOT | IEEE80211_C_SHPREAMBLE;
	ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;

	if (sc->ht_cap_info & WMI_HT_CAP_ENABLED) {
		ic->ic_htcaps = IEEE80211_HTCAP_CBW20_40 | IEEE80211_HTCAP_DSSSCCK40 |
		    (IEEE80211_HTCAP_SMPS_DIS << IEEE80211_HTCAP_SMPS_SHIFT);
		if (sc->ht_cap_info & WMI_HT_CAP_HT20_SGI)
			ic->ic_htcaps |= IEEE80211_HTCAP_SGI20;
		if (sc->ht_cap_info & WMI_HT_CAP_HT40_SGI)
			ic->ic_htcaps |= IEEE80211_HTCAP_SGI40;
		if ((sc->ht_cap_info & WMI_HT_CAP_TX_STBC) && nss > 1)
			ic->ic_htcaps |= IEEE80211_HTCAP_TXSTBC;
		if (sc->ht_cap_info & WMI_HT_CAP_RX_STBC)
			ic->ic_htcaps |= (((sc->ht_cap_info & WMI_HT_CAP_RX_STBC) >> WMI_HT_CAP_RX_STBC_MASK_SHIFT) <<
			    IEEE80211_HTCAP_RXSTBC_SHIFT) & IEEE80211_HTCAP_RXSTBC_MASK;
		if ((sc->ht_cap_info & WMI_HT_CAP_LDPC) ||
		    ((sc->ht_cap_info & WMI_HT_CAP_RX_LDPC) && (sc->ht_cap_info & WMI_HT_CAP_TX_LDPC)))
			ic->ic_htcaps |= IEEE80211_HTCAP_LDPC;
		if (sc->ht_cap_info & WMI_HT_CAP_L_SIG_TXOP_PROT)
			ic->ic_htcaps |= IEEE80211_HTCAP_LSIGTXOPPROT;
		if (sc->vht_cap_info & WMI_VHT_CAP_MAX_MPDU_LEN_MASK)
			ic->ic_htcaps |= IEEE80211_HTCAP_AMSDU7935;
		ic->ic_htxcaps = 0;
		ic->ic_txbfcaps = 0;
		ic->ic_aselcaps = 0;
		/* A-MPDUs of up to 64 KB, 8 us between their MPDUs at least */
		ic->ic_ampdu_params = IEEE80211_AMPDU_PARAM_SS_8 | 0x3;
		memset(ic->ic_sup_mcs, 0, sizeof(ic->ic_sup_mcs));
		for (i = 0; i < nss && i < 4; i++)
			ic->ic_sup_mcs[i] = 0xff;
		ic->ic_tx_mcs_set = IEEE80211_TX_MCS_SET_DEFINED;
		ic->ic_vhtcaps = sc->vht_cap_info;
		if (nss <= 1)
			ic->ic_vhtcaps &= ~IEEE80211_VHTCAP_TX_STBC;
		ic->ic_vht_rxmcs = 0;
		for (i = 0; i < 8; i++)
			ic->ic_vht_rxmcs |= (i < nss ? IEEE80211_VHT_MCS_0_9 : IEEE80211_VHT_MCS_SS_NOT_SUPP) << (2 * i);
		ic->ic_vht_txmcs = ic->ic_vht_rxmcs;
		ic->ic_vht_rx_max_lgi_mbit_s = 0;
		ic->ic_vht_tx_max_lgi_mbit_s = 0;
	}
	ic->ic_max_rssi = 0;
}

/*
 * The card attached as xname: claimed, its chip brought up with its
 * firmware, its channels set, its interface attached to net80211. The
 * 802.11 interface, or NULL with KERNEL.TXT saying the stage it stopped at
 */
struct ieee80211com *
athx_attach(struct pci_attach_args *pa, const char *xname)
{
	struct athx_softc *sc;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int err;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		return NULL;
	strlcpy(sc->sc_dev.dv_xname, xname, sizeof(sc->sc_dev.dv_xname));
	printf("%s at pci%d dev %d function %d", xname, pa->pa_bus, pa->pa_device, pa->pa_function);
	err = athx_pci_attach(sc, pa);
	if (err) {
		printf("%s: the card could not be claimed (%d)\n", xname, err);
		return NULL;
	}
	printf("%s: QCA6174 %04x:%04x, subsystem %04x:%04x, window %lu bytes\n", xname, sc->sc_vendor,
	    sc->sc_device, sc->sc_subvendor, sc->sc_subdevice, (unsigned long)sc->sc_sz);
	athx_sc = sc;
	err = athx_boot(sc);
	if (err) {
		printf("%s: STOPPED at stage %d (%s): %d\n", xname, sc->stage,
		    sc->stage < ATHX_ST_RESET ? "reset" : sc->stage < ATHX_ST_PIPES ? "copy engines" :
		    sc->stage < ATHX_ST_BMI ? "bootloader" : sc->stage < ATHX_ST_FILES ? "files" :
		    sc->stage < ATHX_ST_DOWNLOAD ? "download" : sc->stage < ATHX_ST_HTC ? "HTC" :
		    sc->stage < ATHX_ST_WMI ? "WMI" : "HTT", err);
		athx_say_regs(sc, "stopped");
		return NULL;
	}

	ic = &sc->sc_ic;
	ifp = &ic->ic_if;
	athx_caps(sc);
	athx_setup_channels(sc);
	err = athx_pdev_setup(sc);
	if (err) {
		printf("%s: STOPPED: the pdev's setup failed: %d\n", xname, err);
		return NULL;
	}
	sc->stage = ATHX_ST_UP;
	memcpy(ic->ic_myaddr, sc->mac_addr, ETH_ALEN);
	ic->ic_ibss_chan = &ic->ic_channels[1];

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = athx_ioctl;
	ifp->if_start = athx_start;
	ifp->if_watchdog = athx_watchdog;
	memcpy(ifp->if_xname, xname, IFNAMSIZ);
	if_attach(ifp);
	ieee80211_ifattach(ifp);
	ieee80211_media_init(ifp, athx_media_change, ieee80211_media_status);

	task_set(&sc->newstate_task, athx_newstate_task, sc);
	task_set(&sc->setkey_task, athx_setkey_task, sc);
	task_set(&sc->bmiss_task, athx_bmiss_task, sc);
	timeout_set_proc(&sc->scan_to, athx_scan_timeout, sc);
	ic->ic_set_key = athx_set_key;
	ic->ic_delete_key = athx_delete_key;
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = athx_newstate;
	printf("%s: firmware %s running, address %s, HTT %d.%d, %u rf chains, management frames by %s\n",
	    xname, sc->fw_version, ether_sprintf(ic->ic_myaddr), sc->htt.ver_major, sc->htt.ver_minor,
	    sc->num_rf_chains, sc->mgmt_tx_wmi ? "WMI" : "HTT");
	return ic;
}
