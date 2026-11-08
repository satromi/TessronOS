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
 *	athx_fw.c (x64 PC, WIFI_NET80211=obsd)
 *	The QCA6174 brought up (design 9.6.9):
 *
 *	- the firmware file (firmware-6.bin, its elements: the image, the
 *	  OTP image that calibrates the chip from its one-time-programmable
 *	  memory, the features, the versions of WMI and HTT it speaks) and the
 *	  board file (board-2.bin, the board data of each board by the name of
 *	  its card: bus=pci,vendor=..,device=..,subsystem-vendor=..,
 *	  subsystem-device=..), both read from their objects on the system
 *	  volume;
 *	- the bootloader's commands (BMI): reading and writing the target's
 *	  memory and registers, running code there, the compressed stream the
 *	  images go in by; the chip's PLL set up for a faster download;
 *	- the order of it all: the board data, the OTP run, the firmware by the
 *	  diagnostic window, the bootloader told it is done, HTC's ready and
 *	  its services, WMI's service ready, the init command, WMI's ready
 *	  with the chip's address, HTT's version and receive ring;
 *	- HTC itself: its header on every message, the credits the target
 *	  gives for sending, its control endpoint and the connection of a
 *	  service to an endpoint.
 *
 *	Each step that fails says so on the console (KERNEL.TXT) with the
 *	stage reached (enum athx_stage).
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

/* The objects the firmware and board files are read from (wifi_user.c's wifi_fw[]) */
#define ATHX_FW_NAME	"athx-qca6174-firmware-6"
#define ATHX_BD_NAME	"athx-qca6174-board-2"

/* The PLL of each reference clock (indexed by enum ath10k_hw_refclk_speed) */
static const struct ath10k_hw_clk_params qca6174_clk[ATH10K_HW_REFCLK_COUNT] = {
	{ 48000000, 0xe, 0x2aaa8, 2400, 0, 1 },
	{ 19200000, 0x24, 0x2aaa8, 960, 0, 1 },
	{ 24000000, 0x1d, 0x15551, 1200, 0, 1 },
	{ 26000000, 0x1b, 0x4ec4, 1300, 0, 1 },
	{ 37400000, 0x12, 0x34b49, 1870, 0, 1 },
	{ 38400000, 0x12, 0x15551, 1920, 0, 1 },
	{ 40000000, 0x12, 0x26665, 2000, 0, 1 },
	{ 52000000, 0x1b, 0x4ec4, 2600, 0, 1 },
};

/* A wait for what the interrupt's handler sets: 0, or ETIMEDOUT, or ESHUTDOWN once the firmware has crashed */
int
athx_wait(struct athx_softc *sc, volatile int *flag, const char *what, int msec)
{
	uint64_t until = getnsecuptime() + (uint64_t)msec * 1000000ULL;
	uint64_t now;

	while (!*flag) {
		if (sc->wedged)
			return ESHUTDOWN;
		now = getnsecuptime();
		if (now >= until)
			return ETIMEDOUT;
		tsleep_nsec((void *)flag, 0, what, MIN(until - now, 100000000ULL));
	}
	return 0;
}

/* ---------------------------------------------------------------- the bootloader */

static uint32_t
hi_addr(uint32_t item)
{
	return QCA988X_HOST_INTEREST_ADDRESS + item;
}

static int
bmi_done(struct athx_softc *sc)
{
	struct bmi_cmd cmd;

	if (sc->bmi_done_sent)
		return 0;
	sc->bmi_done_sent = 1;
	cmd.id = htole32(BMI_DONE);
	return athx_bmi_exchange(sc, &cmd, sizeof(cmd.id), NULL, NULL);
}

static int
bmi_get_target_info(struct athx_softc *sc, uint32_t *version, uint32_t *type)
{
	struct bmi_cmd cmd;
	union bmi_resp resp;
	uint32_t resplen = sizeof(resp.get_target_info);
	int ret;

	cmd.id = htole32(BMI_GET_TARGET_INFO);
	ret = athx_bmi_exchange(sc, &cmd, sizeof(cmd.id), &resp, &resplen);
	if (ret)
		return ret;
	if (resplen < sizeof(resp.get_target_info))
		return EIO;
	*version = le32toh(resp.get_target_info.version);
	*type = le32toh(resp.get_target_info.type);
	return 0;
}

static int
bmi_read_memory(struct athx_softc *sc, uint32_t address, void *buffer, uint32_t length)
{
	struct bmi_cmd cmd;
	union bmi_resp resp;
	uint32_t cmdlen = sizeof(cmd.id) + sizeof(cmd.read_mem), rxlen;
	int ret;

	if (sc->bmi_done_sent)
		return EBUSY;
	while (length) {
		rxlen = MIN(length, BMI_MAX_DATA_SIZE);
		cmd.id = htole32(BMI_READ_MEMORY);
		cmd.read_mem.addr = htole32(address);
		cmd.read_mem.len = htole32(rxlen);
		ret = athx_bmi_exchange(sc, &cmd, cmdlen, &resp, &rxlen);
		if (ret)
			return ret;
		memcpy(buffer, resp.read_mem.payload, rxlen);
		address += rxlen;
		buffer = (uint8_t *)buffer + rxlen;
		length -= rxlen;
	}
	return 0;
}

static int
bmi_write_memory(struct athx_softc *sc, uint32_t address, const void *buffer, uint32_t length)
{
	struct bmi_cmd cmd;
	uint32_t hdrlen = sizeof(cmd.id) + sizeof(cmd.write_mem), txlen;
	int ret;

	if (sc->bmi_done_sent)
		return EBUSY;
	while (length) {
		txlen = MIN(length, BMI_MAX_DATA_SIZE - hdrlen);
		memset(cmd.write_mem.payload, 0, roundup(txlen, 4));
		memcpy(cmd.write_mem.payload, buffer, txlen);
		txlen = roundup(txlen, 4);
		cmd.id = htole32(BMI_WRITE_MEMORY);
		cmd.write_mem.addr = htole32(address);
		cmd.write_mem.len = htole32(txlen);
		ret = athx_bmi_exchange(sc, &cmd, hdrlen + txlen, NULL, NULL);
		if (ret)
			return ret;
		txlen = MIN(txlen, length);
		address += txlen;
		buffer = (const uint8_t *)buffer + txlen;
		length -= txlen;
	}
	return 0;
}

static int
bmi_read32(struct athx_softc *sc, uint32_t item, uint32_t *val)
{
	uint32_t tmp;
	int ret;

	ret = bmi_read_memory(sc, hi_addr(item), &tmp, 4);
	if (ret == 0)
		*val = le32toh(tmp);
	return ret;
}

static int
bmi_write32(struct athx_softc *sc, uint32_t item, uint32_t val)
{
	uint32_t v = htole32(val);

	return bmi_write_memory(sc, hi_addr(item), &v, sizeof(v));
}

static int
bmi_read_soc_reg(struct athx_softc *sc, uint32_t address, uint32_t *reg_val)
{
	struct bmi_cmd cmd;
	union bmi_resp resp;
	uint32_t resplen = sizeof(resp.read_soc_reg);
	int ret;

	if (sc->bmi_done_sent)
		return EBUSY;
	cmd.id = htole32(BMI_READ_SOC_REGISTER);
	cmd.read_soc_reg.addr = htole32(address);
	ret = athx_bmi_exchange(sc, &cmd, sizeof(cmd.id) + sizeof(cmd.read_soc_reg), &resp, &resplen);
	if (ret)
		return ret;
	*reg_val = le32toh(resp.read_soc_reg.value);
	return 0;
}

static int
bmi_write_soc_reg(struct athx_softc *sc, uint32_t address, uint32_t reg_val)
{
	struct bmi_cmd cmd;

	if (sc->bmi_done_sent)
		return EBUSY;
	cmd.id = htole32(BMI_WRITE_SOC_REGISTER);
	cmd.write_soc_reg.addr = htole32(address);
	cmd.write_soc_reg.value = htole32(reg_val);
	return athx_bmi_exchange(sc, &cmd, sizeof(cmd.id) + sizeof(cmd.write_soc_reg), NULL, NULL);
}

static int
bmi_execute(struct athx_softc *sc, uint32_t address, uint32_t param, uint32_t *result)
{
	struct bmi_cmd cmd;
	union bmi_resp resp;
	uint32_t resplen = sizeof(resp.execute);
	int ret;

	if (sc->bmi_done_sent)
		return EBUSY;
	cmd.id = htole32(BMI_EXECUTE);
	cmd.execute.addr = htole32(address);
	cmd.execute.param = htole32(param);
	ret = athx_bmi_exchange(sc, &cmd, sizeof(cmd.id) + sizeof(cmd.execute), &resp, &resplen);
	if (ret)
		return ret;
	if (resplen < sizeof(resp.execute))
		return EIO;
	*result = le32toh(resp.execute.result);
	return 0;
}

static int
bmi_lz_data(struct athx_softc *sc, const void *buffer, uint32_t length)
{
	struct bmi_cmd cmd;
	uint32_t hdrlen = sizeof(cmd.id) + sizeof(cmd.lz_data), txlen;
	int ret;

	while (length) {
		txlen = MIN(length, BMI_MAX_DATA_SIZE - hdrlen);
		cmd.id = htole32(BMI_LZ_DATA);
		cmd.lz_data.len = htole32(txlen);
		memcpy(cmd.lz_data.payload, buffer, txlen);
		ret = athx_bmi_exchange(sc, &cmd, hdrlen + txlen, NULL, NULL);
		if (ret)
			return ret;
		buffer = (const uint8_t *)buffer + txlen;
		length -= txlen;
	}
	return 0;
}

static int
bmi_lz_stream_start(struct athx_softc *sc, uint32_t address)
{
	struct bmi_cmd cmd;

	cmd.id = htole32(BMI_LZ_STREAM_START);
	cmd.lz_start.addr = htole32(address);
	return athx_bmi_exchange(sc, &cmd, sizeof(cmd.id) + sizeof(cmd.lz_start), NULL, NULL);
}

/* An image by the bootloader's compressed stream; a new stream started after to flush the target's caches */
static int
bmi_fast_download(struct athx_softc *sc, uint32_t address, const void *buffer, uint32_t length)
{
	uint8_t trailer[4] = { 0 };
	uint32_t head_len = rounddown(length, 4), trailer_len = length - head_len;
	int ret;

	ret = bmi_lz_stream_start(sc, address);
	if (ret)
		return ret;
	if (trailer_len > 0)
		memcpy(trailer, (const uint8_t *)buffer + head_len, trailer_len);
	ret = bmi_lz_data(sc, buffer, head_len);
	if (ret)
		return ret;
	if (trailer_len > 0)
		ret = bmi_lz_data(sc, trailer, 4);
	if (ret)
		return ret;
	return bmi_lz_stream_start(sc, 0x00);
}

static int
bmi_set_start(struct athx_softc *sc, uint32_t address)
{
	struct bmi_cmd cmd;

	cmd.id = htole32(BMI_SET_APP_START);
	cmd.set_app_start.addr = htole32(address);
	return athx_bmi_exchange(sc, &cmd, sizeof(cmd.id) + sizeof(cmd.set_app_start), NULL, NULL);
}

/* ---------------------------------------------------------------- the PLL */

/* The PLL set for the target's CPU clock, in the order the chip needs */
static int
athx_enable_pll_clock(struct athx_softc *sc)
{
	const struct ath10k_hw_clk_params *hw_clk;
	uint32_t addr, reg_val, mem_val, freq = QCA6174_HW_3_2_TARGET_CPU_FREQ;
	int wait_limit;

	addr = RTC_SOC_BASE_ADDRESS | EFUSE_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	if (MS(reg_val, EFUSE_XTAL_SEL) >= ATH10K_HW_REFCLK_COUNT)
		return EINVAL;
	hw_clk = &qca6174_clk[MS(reg_val, EFUSE_XTAL_SEL)];

	addr = RTC_SOC_BASE_ADDRESS | BB_PLL_CONFIG_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~(BB_PLL_CONFIG_FRAC_MASK | BB_PLL_CONFIG_OUTDIV_MASK);
	reg_val |= SM(hw_clk->rnfrac, BB_PLL_CONFIG_FRAC) | SM(hw_clk->outdiv, BB_PLL_CONFIG_OUTDIV);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	addr = RTC_WMAC_BASE_ADDRESS | WLAN_PLL_SETTLE_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~WLAN_PLL_SETTLE_TIME_MASK;
	reg_val |= SM(hw_clk->settle_time, WLAN_PLL_SETTLE_TIME);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	addr = RTC_SOC_BASE_ADDRESS | SOC_CORE_CLK_CTRL_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~SOC_CORE_CLK_CTRL_DIV_MASK;
	reg_val |= SM(1, SOC_CORE_CLK_CTRL_DIV);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	mem_val = htole32(1);
	if (bmi_write_memory(sc, CORE_CLK_DIV_ADDRESS, &mem_val, sizeof(mem_val)))
		return EINVAL;

	addr = RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val |= SM(hw_clk->refdiv, WLAN_PLL_CONTROL_REFDIV) | SM(hw_clk->div, WLAN_PLL_CONTROL_DIV) |
	    SM(1, WLAN_PLL_CONTROL_NOPWD);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	/* busy wait (at most 1 s) for the RTC sync status to say the PLL settled */
	wait_limit = 100000;
	addr = RTC_WMAC_BASE_ADDRESS | RTC_SYNC_STATUS_OFFSET;
	do {
		if (bmi_read_soc_reg(sc, addr, &reg_val))
			return EINVAL;
		if (!MS(reg_val, RTC_SYNC_STATUS_PLL_CHANGING))
			break;
		wait_limit--;
		delay(10);
	} while (wait_limit > 0);
	if (MS(reg_val, RTC_SYNC_STATUS_PLL_CHANGING))
		return EINVAL;

	addr = RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~WLAN_PLL_CONTROL_BYPASS_MASK;
	reg_val |= SM(0, WLAN_PLL_CONTROL_BYPASS);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	wait_limit = 100000;
	addr = RTC_WMAC_BASE_ADDRESS | RTC_SYNC_STATUS_OFFSET;
	do {
		if (bmi_read_soc_reg(sc, addr, &reg_val))
			return EINVAL;
		if (!MS(reg_val, RTC_SYNC_STATUS_PLL_CHANGING))
			break;
		wait_limit--;
		delay(10);
	} while (wait_limit > 0);
	if (MS(reg_val, RTC_SYNC_STATUS_PLL_CHANGING))
		return EINVAL;

	addr = RTC_SOC_BASE_ADDRESS | SOC_CPU_CLOCK_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~SOC_CPU_CLOCK_STANDARD_MASK;
	reg_val |= SM(1, SOC_CPU_CLOCK_STANDARD);
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	addr = RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET;
	if (bmi_read_soc_reg(sc, addr, &reg_val))
		return EINVAL;
	reg_val &= ~WLAN_PLL_CONTROL_NOPWD_MASK;
	if (bmi_write_soc_reg(sc, addr, reg_val))
		return EINVAL;

	mem_val = htole32(1);
	if (bmi_write_memory(sc, CPU_PLL_INIT_ADDRESS, &mem_val, sizeof(mem_val)))
		return EINVAL;
	freq = htole32(freq);
	if (bmi_write_memory(sc, CPU_SPEED_ADDRESS, &freq, sizeof(freq)))
		return EINVAL;
	return 0;
}

/* ---------------------------------------------------------------- the files */

void
athx_free_files(struct athx_softc *sc)
{
	if (sc->fw_file != NULL)
		free(sc->fw_file, M_DEVBUF, sc->fw_file_len);
	if (sc->bd_file != NULL)
		free(sc->bd_file, M_DEVBUF, sc->bd_file_len);
	sc->fw_file = sc->bd_file = NULL;
	sc->fw_image = sc->otp_image = sc->board_data = NULL;
}

static int
fw_feature(struct athx_softc *sc, int bit)
{
	return (bit / 8) < (int)sizeof(sc->fw_features) && (sc->fw_features[bit / 8] & (1 << (bit % 8)));
}

/* The firmware file read and its elements found: 0 or an errno value */
static int
athx_fetch_firmware(struct athx_softc *sc)
{
	const struct ath10k_fw_ie *hdr;
	const uint8_t *data;
	size_t len, magic_len, ie_len;
	uint32_t ie_id;
	int error;

	error = loadfirmware(ATHX_FW_NAME, &sc->fw_file, &sc->fw_file_len);
	if (error) {
		printf("%s: could not read the firmware %s (%d)\n", sc->sc_dev.dv_xname, ATHX_FW_NAME, error);
		return error;
	}
	data = sc->fw_file;
	len = sc->fw_file_len;
	magic_len = strlen(ATH10K_FIRMWARE_MAGIC) + 1;
	if (len < magic_len || memcmp(data, ATH10K_FIRMWARE_MAGIC, magic_len) != 0) {
		printf("%s: invalid firmware magic\n", sc->sc_dev.dv_xname);
		return EINVAL;
	}
	magic_len = roundup(magic_len, 4);
	len -= magic_len;
	data += magic_len;
	while (len > sizeof(struct ath10k_fw_ie)) {
		hdr = (const struct ath10k_fw_ie *)data;
		ie_id = le32toh(hdr->id);
		ie_len = le32toh(hdr->len);
		len -= sizeof(*hdr);
		data += sizeof(*hdr);
		if (len < ie_len) {
			printf("%s: invalid length for FW IE %u (%zu < %zu)\n", sc->sc_dev.dv_xname, ie_id,
			    len, ie_len);
			return EINVAL;
		}
		switch (ie_id) {
		case ATH10K_FW_IE_FW_VERSION:
			if (ie_len > sizeof(sc->fw_version) - 1)
				break;
			memcpy(sc->fw_version, data, ie_len);
			sc->fw_version[ie_len] = '\0';
			break;
		case ATH10K_FW_IE_FEATURES:
			memcpy(sc->fw_features, data, MIN(ie_len, sizeof(sc->fw_features)));
			break;
		case ATH10K_FW_IE_FW_IMAGE:
			sc->fw_image = data;
			sc->fw_image_len = ie_len;
			break;
		case ATH10K_FW_IE_OTP_IMAGE:
			sc->otp_image = data;
			sc->otp_image_len = ie_len;
			break;
		case ATH10K_FW_IE_WMI_OP_VERSION:
			if (ie_len == 4)
				sc->wmi_op_version = le32toh(*(const uint32_t *)data);
			break;
		case ATH10K_FW_IE_HTT_OP_VERSION:
			if (ie_len == 4)
				sc->htt_op_version = le32toh(*(const uint32_t *)data);
			break;
		case ATH10K_FW_IE_TIMESTAMP:
			break;
		case ATH10K_FW_IE_FW_CODE_SWAP_IMAGE:
			printf("%s: the firmware has a code swap image, which athx does not load\n",
			    sc->sc_dev.dv_xname);
			return EOPNOTSUPP;
		default:
			printf("%s: unknown FW IE: %u\n", sc->sc_dev.dv_xname, ie_id);
			break;
		}
		ie_len = roundup(ie_len, 4);
		if (ie_len > len)
			break;
		len -= ie_len;
		data += ie_len;
	}
	if (sc->fw_image == NULL || sc->fw_image_len == 0) {
		printf("%s: no firmware image in %s\n", sc->sc_dev.dv_xname, ATHX_FW_NAME);
		return ENOENT;
	}
	if (sc->wmi_op_version != ATH10K_FW_WMI_OP_VERSION_TLV ||
	    sc->htt_op_version != ATH10K_FW_HTT_OP_VERSION_TLV) {
		printf("%s: the firmware speaks WMI %u and HTT %u, not their TLV forms\n",
		    sc->sc_dev.dv_xname, sc->wmi_op_version, sc->htt_op_version);
		return EOPNOTSUPP;
	}
	printf("%s: firmware %s: image %zu bytes, OTP %zu bytes, features %02x%02x%02x%02x\n",
	    sc->sc_dev.dv_xname, sc->fw_version, sc->fw_image_len, sc->otp_image_len,
	    sc->fw_features[0], sc->fw_features[1], sc->fw_features[2], sc->fw_features[3]);
	return 0;
}

/* The board data of name in one ATH10K_BD_IE_BOARD element: 0, or ENOENT */
static int
parse_bd_ie_board(struct athx_softc *sc, const uint8_t *buf, size_t buf_len, const char *name)
{
	const struct ath10k_fw_ie *hdr;
	size_t ie_len;
	int match = 0;

	while (buf_len > sizeof(struct ath10k_fw_ie)) {
		hdr = (const struct ath10k_fw_ie *)buf;
		ie_len = le32toh(hdr->len);
		buf_len -= sizeof(*hdr);
		buf += sizeof(*hdr);
		if (buf_len < roundup(ie_len, 4))
			return EINVAL;
		switch (le32toh(hdr->id)) {
		case ATH10K_BD_IE_BOARD_NAME:
			if (ie_len == strlen(name) && memcmp(hdr->data, name, ie_len) == 0)
				match = 1;
			break;
		case ATH10K_BD_IE_BOARD_DATA:
			if (!match)
				break;
			sc->board_data = hdr->data;
			sc->board_len = ie_len;
			return 0;
		default:
			break;
		}
		ie_len = roundup(ie_len, 4);
		buf_len -= ie_len;
		buf += ie_len;
	}
	return ENOENT;
}

static int
search_bd(struct athx_softc *sc, const char *name, const uint8_t *data, size_t len)
{
	const struct ath10k_fw_ie *hdr;
	size_t ie_len;
	int ret;

	while (len > sizeof(struct ath10k_fw_ie)) {
		hdr = (const struct ath10k_fw_ie *)data;
		ie_len = le32toh(hdr->len);
		len -= sizeof(*hdr);
		data = hdr->data;
		if (len < roundup(ie_len, 4))
			return EINVAL;
		if (le32toh(hdr->id) == ATH10K_BD_IE_BOARD) {
			ret = parse_bd_ie_board(sc, data, ie_len, name);
			if (ret != ENOENT)
				return ret;
		}
		ie_len = roundup(ie_len, 4);
		len -= ie_len;
		data += ie_len;
	}
	return ENOENT;
}

/*
 * The board data chosen from board-2.bin: by the board and chip IDs the
 * OTP gave when it gave them, else by the card's PCI IDs and subsystem
 */
static int
athx_fetch_board(struct athx_softc *sc)
{
	const uint8_t *data;
	size_t len, magic_len;
	char bmi_name[100];
	int error;

	error = loadfirmware(ATHX_BD_NAME, &sc->bd_file, &sc->bd_file_len);
	if (error) {
		printf("%s: could not read the board file %s (%d)\n", sc->sc_dev.dv_xname, ATHX_BD_NAME, error);
		return error;
	}
	data = sc->bd_file;
	len = sc->bd_file_len;
	magic_len = strlen(ATH10K_BOARD_MAGIC) + 1;
	if (len < magic_len || memcmp(data, ATH10K_BOARD_MAGIC, magic_len) != 0) {
		printf("%s: found invalid board magic\n", sc->sc_dev.dv_xname);
		return EINVAL;
	}
	magic_len = roundup(magic_len, 4);
	if (len < magic_len)
		return EINVAL;
	data += magic_len;
	len -= magic_len;
	snprintf(sc->board_name, sizeof(sc->board_name),
	    "bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,subsystem-device=%04x",
	    sc->sc_vendor, sc->sc_device, sc->sc_subvendor, sc->sc_subdevice);
	error = ENOENT;
	if (sc->bmi_ids_valid) {
		snprintf(bmi_name, sizeof(bmi_name), "bus=pci,bmi-chip-id=%d,bmi-board-id=%d",
		    sc->bmi_chip_id, sc->bmi_board_id);
		error = search_bd(sc, bmi_name, data, len);
		if (error == 0)
			strlcpy(sc->board_name, bmi_name, sizeof(sc->board_name));
		else
			printf("%s: no board data for '%s', trying '%s'\n", sc->sc_dev.dv_xname, bmi_name,
			    sc->board_name);
	}
	if (error == ENOENT)
		error = search_bd(sc, sc->board_name, data, len);
	if (error) {
		printf("%s: no board data for '%s' in %s (%d)\n", sc->sc_dev.dv_xname, sc->board_name,
		    ATHX_BD_NAME, error);
		return error ? error : ENOENT;
	}
	printf("%s: board data '%s', %zu bytes\n", sc->sc_dev.dv_xname, sc->board_name, sc->board_len);
	return 0;
}

/* The board and chip IDs the OTP finds in the chip's EEPROM: 0, or EOPNOTSUPP when it has none */
static int
athx_board_id_from_otp(struct athx_softc *sc)
{
	uint32_t result, address = QCA6174_HW_3_0_PATCH_LOAD_ADDR;
	uint8_t board_id, chip_id;
	int ret;

	if (sc->otp_image == NULL || sc->otp_image_len == 0) {
		printf("%s: no OTP image to read the board id with\n", sc->sc_dev.dv_xname);
		return ENOENT;
	}
	ret = bmi_fast_download(sc, address, sc->otp_image, sc->otp_image_len);
	if (ret) {
		printf("%s: could not write otp for board id check: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_execute(sc, address, BMI_PARAM_GET_EEPROM_BOARD_ID, &result);
	if (ret) {
		printf("%s: could not execute otp for board id check: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	board_id = MS(result, ATH10K_BMI_BOARD_ID_FROM_OTP);
	chip_id = MS(result, ATH10K_BMI_CHIP_ID_FROM_OTP);
	sc->ext_bid_supported = (result & ATH10K_BMI_EXT_BOARD_ID_SUPPORT) != 0;
	printf("%s: OTP board id: result %08x, board %d chip %d%s\n", sc->sc_dev.dv_xname, result,
	    board_id, chip_id, sc->ext_bid_supported ? ", extended board id" : "");
	if ((result & ATH10K_BMI_BOARD_ID_STATUS_MASK) != 0 || board_id == 0)
		return EOPNOTSUPP;
	sc->bmi_ids_valid = 1;
	sc->bmi_board_id = board_id;
	sc->bmi_chip_id = chip_id;
	return 0;
}

/* ---------------------------------------------------------------- the download */

/* The target told the HTC version, its mode, the host's byte order, the device ID it checks */
static int
athx_init_configure_target(struct athx_softc *sc)
{
	uint32_t param_host;
	int ret;

	ret = bmi_write32(sc, HI_ITEM(hi_app_host_interest), HTC_PROTOCOL_VERSION);
	if (ret) {
		printf("%s: settings HTC version failed\n", sc->sc_dev.dv_xname);
		return ret;
	}
	ret = bmi_read32(sc, HI_ITEM(hi_option_flag), &param_host);
	if (ret)
		return ret;
	param_host |= (1 << HI_OPTION_NUM_DEV_SHIFT);
	param_host |= (HI_OPTION_FW_MODE_AP << HI_OPTION_FW_MODE_SHIFT);
	param_host |= (1 << HI_OPTION_MAC_ADDR_METHOD_SHIFT);
	param_host |= (0 << HI_OPTION_FW_BRIDGE_SHIFT);
	param_host |= (0 << HI_OPTION_FW_SUBMODE_SHIFT);
	ret = bmi_write32(sc, HI_ITEM(hi_option_flag), param_host);
	if (ret)
		return ret;
	ret = bmi_write32(sc, HI_ITEM(hi_be), 0);
	if (ret)
		return ret;
	ret = bmi_write32(sc, HI_ITEM(hi_fw_swap), 0);
	if (ret)
		return ret;
	return bmi_write32(sc, HI_ITEM(hi_hci_uart_pwr_mgmt_params_ext), sc->sc_device);
}

static int
athx_download_board_data(struct athx_softc *sc)
{
	uint32_t board_address, ext_addr;
	int ret;

	ret = bmi_read32(sc, HI_ITEM(hi_board_ext_data), &ext_addr);
	if (ret) {
		printf("%s: could not read board ext data addr (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	if (ext_addr != 0) {
		if (sc->board_len != QCA6174_BOARD_DATA_SZ + QCA6174_BOARD_EXT_DATA_SZ) {
			printf("%s: invalid board (ext) data sizes %zu != %d+%d\n", sc->sc_dev.dv_xname,
			    sc->board_len, QCA6174_BOARD_DATA_SZ, QCA6174_BOARD_EXT_DATA_SZ);
			return EINVAL;
		}
		ret = bmi_write32(sc, HI_ITEM(hi_board_ext_data_config), (QCA6174_BOARD_EXT_DATA_SZ << 16) | 1);
		if (ret)
			return ret;
	}
	ret = bmi_read32(sc, HI_ITEM(hi_board_data), &board_address);
	if (ret) {
		printf("%s: could not read board data addr (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_write_memory(sc, board_address, sc->board_data, MIN(QCA6174_BOARD_DATA_SZ, sc->board_len));
	if (ret) {
		printf("%s: could not write board data (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_write32(sc, HI_ITEM(hi_board_data_initialized), 1);
	if (ret)
		return ret;
	if (sc->ext_bid_supported)
		printf("%s: the OTP says the chip has an extended board id; its data is not written\n",
		    sc->sc_dev.dv_xname);
	return 0;
}

/* The board data in the target and the OTP run on it, which calibrates the chip */
static int
athx_download_and_run_otp(struct athx_softc *sc)
{
	uint32_t result, address = QCA6174_HW_3_0_PATCH_LOAD_ADDR;
	int ret;

	ret = athx_download_board_data(sc);
	if (ret) {
		printf("%s: failed to download board data: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	if (sc->otp_image == NULL || sc->otp_image_len == 0) {
		printf("%s: not running otp, calibration will be incorrect\n", sc->sc_dev.dv_xname);
		return 0;
	}
	ret = bmi_fast_download(sc, address, sc->otp_image, sc->otp_image_len);
	if (ret) {
		printf("%s: could not write otp (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_execute(sc, address, QCA6174_HW_3_2_OTP_EXE_PARAM, &result);
	if (ret) {
		printf("%s: could not execute otp (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	printf("%s: otp execute result %d\n", sc->sc_dev.dv_xname, result);
	if (!fw_feature(sc, ATH10K_FW_FEATURE_IGNORE_OTP_RESULT) && result != 0) {
		printf("%s: otp calibration failed: %d\n", sc->sc_dev.dv_xname, result);
		return EINVAL;
	}
	return 0;
}

/* The memory region of the chip's address space the diagnostic window reaches (CPU_ADDR_MSB) */
static void
athx_map_target_mem(struct athx_softc *sc, uint32_t msb)
{
	athx_write32(sc, SOC_CORE_BASE_ADDRESS + FW_RAM_CONFIG_ADDRESS, msb);
}

static int
athx_diag_segment_download(struct athx_softc *sc, const uint8_t *buffer, uint32_t address, uint32_t length)
{
	uint32_t addr = address & REGION_ACCESS_SIZE_MASK;
	int ret, size, remain;

	if (address < DRAM_BASE_ADDRESS + REGION_ACCESS_SIZE_LIMIT)
		return athx_diag_write_mem(sc, address, buffer, length);
	athx_map_target_mem(sc, CPU_ADDR_MSB_REGION_VAL(address));
	if (addr + length > REGION_ACCESS_SIZE_LIMIT) {
		size = REGION_ACCESS_SIZE_LIMIT - addr;
		remain = length - size;
		ret = athx_diag_write_mem(sc, address, buffer, size);
		if (ret == 0) {
			athx_map_target_mem(sc, CPU_ADDR_MSB_REGION_VAL(address) + 1);
			ret = athx_diag_write_mem(sc, address & ~REGION_ACCESS_SIZE_MASK, buffer + size, remain);
		}
	} else {
		ret = athx_diag_write_mem(sc, address, buffer, length);
	}
	athx_map_target_mem(sc, CPU_ADDR_MSB_REGION_VAL(DRAM_BASE_ADDRESS));
	return ret;
}

/* The segmented firmware image written by the diagnostic window: 0, or an errno value to fall back on BMI */
static int
athx_diag_fast_download(struct athx_softc *sc, const uint8_t *buf, uint32_t length)
{
	const struct bmi_segmented_file_header *hdr;
	const struct bmi_segmented_metadata *md;
	uint32_t left, base_addr, base_len;
	int ret = 0, end = 0;

	if (length < sizeof(*hdr))
		return EINVAL;
	hdr = (const struct bmi_segmented_file_header *)buf;
	if (le32toh(hdr->magic_num) != BMI_SGMTFILE_MAGIC_NUM || hdr->file_flags != 0)
		return EINVAL;
	md = (const struct bmi_segmented_metadata *)hdr->data;
	left = length - sizeof(*hdr);
	while (left > 0) {
		if (left < sizeof(*md)) {
			printf("%s: firmware segment is truncated: %d\n", sc->sc_dev.dv_xname, left);
			return EINVAL;
		}
		base_addr = le32toh(md->addr);
		base_len = le32toh(md->length);
		buf = md->data;
		left -= sizeof(*md);
		switch (base_len) {
		case BMI_SGMTFILE_BEGINADDR:
			ret = bmi_set_start(sc, base_addr);
			base_len = 0;
			break;
		case BMI_SGMTFILE_DONE:
			base_len = 0;
			end = 1;
			ret = 0;
			break;
		case BMI_SGMTFILE_BDDATA:
		case BMI_SGMTFILE_EXEC:
			printf("%s: firmware has unsupported segment: %d\n", sc->sc_dev.dv_xname, base_len);
			return EINVAL;
		default:
			if (base_len > left) {
				printf("%s: firmware has invalid segment length, %d > %d\n",
				    sc->sc_dev.dv_xname, base_len, left);
				return EINVAL;
			}
			ret = athx_diag_segment_download(sc, buf, base_addr, base_len);
			if (ret)
				printf("%s: failed to download firmware via diag interface: %d\n",
				    sc->sc_dev.dv_xname, ret);
			break;
		}
		if (ret || end)
			break;
		md = (const struct bmi_segmented_metadata *)(buf + base_len);
		left -= base_len;
	}
	return ret;
}

static int
athx_download_fw(struct athx_softc *sc)
{
	int ret;

	ret = athx_diag_fast_download(sc, sc->fw_image, sc->fw_image_len);
	if (ret == 0)
		return 0;
	printf("%s: failed to upload firmware via diag ce, trying BMI: %d\n", sc->sc_dev.dv_xname, ret);
	return bmi_fast_download(sc, QCA6174_HW_3_0_PATCH_LOAD_ADDR, sc->fw_image, sc->fw_image_len);
}

/* ---------------------------------------------------------------- HTC */

static void
htc_control_rx(struct athx_softc *sc, uint8_t *data, int len)
{
	const struct ath10k_htc_msg *msg = (const struct ath10k_htc_msg *)data;

	if (len < (int)sizeof(msg->hdr))
		return;
	switch (le16toh(msg->hdr.message_id)) {
	case ATH10K_HTC_MSG_READY_ID:
	case ATH10K_HTC_MSG_CONNECT_SERVICE_RESP_ID:
		if (sc->ctl_resp_done)
			printf("%s: HTC rx ctrl still processing\n", sc->sc_dev.dv_xname);
		sc->ctl_resp_len = MIN(len, ATH10K_HTC_MAX_CTRL_MSG_LEN);
		memcpy(sc->ctl_resp, data, sc->ctl_resp_len);
		sc->ctl_resp_done = 1;
		wakeup(&sc->ctl_resp_done);
		break;
	case ATH10K_HTC_MSG_SEND_SUSPEND_COMPLETE:
		break;
	default:
		printf("%s: ignoring unsolicited htc ep0 event\n", sc->sc_dev.dv_xname);
		break;
	}
}

/* The credits the target gave back, endpoint by endpoint */
static void
htc_process_trailer(struct athx_softc *sc, uint8_t *buf, int length)
{
	struct ath10k_ath10k_htc_record_hdr *rh;
	struct ath10k_htc_credit_report *cr;
	int i, n;

	while (length > 0) {
		if (length < (int)sizeof(*rh))
			return;
		rh = (struct ath10k_ath10k_htc_record_hdr *)buf;
		if (rh->len > length - (int)sizeof(*rh)) {
			printf("%s: invalid record length: %d\n", sc->sc_dev.dv_xname, rh->len);
			return;
		}
		if (rh->id == ATH10K_HTC_RECORD_CREDITS) {
			cr = (struct ath10k_htc_credit_report *)(rh + 1);
			n = rh->len / sizeof(*cr);
			for (i = 0; i < n; i++, cr++) {
				if (cr->eid >= ATHX_HTC_EP_COUNT)
					break;
				sc->ep[cr->eid].tx_credits += cr->credits;
				wakeup(&sc->ep[cr->eid].tx_credits);
			}
		}
		buf += sizeof(*rh) + rh->len;
		length -= sizeof(*rh) + rh->len;
	}
}

/* A message an engine brought up: its trailer's records taken, its payload to its endpoint */
void
athx_htc_rx(struct athx_softc *sc, struct athx_ce *ce, uint8_t *buf, int len)
{
	struct ath10k_htc_hdr *hdr = (struct ath10k_htc_hdr *)buf;
	struct athx_htc_ep *ep;
	int payload_len, trailer_len = 0;

	if (len < (int)sizeof(*hdr))
		return;
	if (hdr->eid >= ATHX_HTC_EP_COUNT) {
		printf("%s: HTC Rx: invalid eid %d\n", sc->sc_dev.dv_xname, hdr->eid);
		return;
	}
	ep = &sc->ep[hdr->eid];
	if (ep->service_id == 0) {
		printf("%s: htc rx endpoint %d is not connected\n", sc->sc_dev.dv_xname, hdr->eid);
		return;
	}
	payload_len = le16toh(hdr->len);
	if (payload_len + (int)sizeof(*hdr) > ATH10K_HTC_MAX_LEN || len - (int)sizeof(*hdr) < payload_len) {
		printf("%s: HTC rx frame of bad length %d (%d)\n", sc->sc_dev.dv_xname, payload_len, len);
		return;
	}
	if (hdr->flags & ATH10K_HTC_FLAG_TRAILER_PRESENT) {
		trailer_len = hdr->trailer_len;
		if (trailer_len < (int)sizeof(struct ath10k_ath10k_htc_record_hdr) || trailer_len > payload_len) {
			printf("%s: Invalid trailer length: %d\n", sc->sc_dev.dv_xname, trailer_len);
			return;
		}
		htc_process_trailer(sc, buf + sizeof(*hdr) + payload_len - trailer_len, trailer_len);
	}
	if (payload_len - trailer_len <= 0)
		return;
	if (ep->rx != NULL)
		(*ep->rx)(sc, buf + sizeof(*hdr), payload_len - trailer_len);
}

/*
 * A message to an endpoint: its HTC header put in front, the target's
 * credits taken (only WMI's endpoint has them), copied into a buffer the
 * chip reaches and sent on the endpoint's pipe. 0, EAGAIN when there are
 * no credits or no room
 */
int
athx_htc_send(struct athx_softc *sc, int eid, const void *data, int len)
{
	struct athx_htc_ep *ep = &sc->ep[eid];
	struct ath10k_htc_hdr *hdr;
	uint32_t paddr;
	uint16_t l;
	int credits = 0, slot = -1, ret, total = len + sizeof(*hdr);
	uint8_t *buf;

	if (sc->wedged)
		return EIO;
	if (eid >= ATHX_HTC_EP_COUNT || (ep->service_id == 0 && eid != 0))
		return ENOENT;
	if (ep->credit_flow) {
		credits = (total + ep->tx_credit_size - 1) / ep->tx_credit_size;
		if (ep->tx_credits < credits)
			return EAGAIN;
	}
	if (ep->ul_pipe == 4) {
		/* HTT's own messages: one of its buffers */
		for (slot = 0; slot < ATHX_HTT_CTL_N && sc->htt.ctl_busy[slot]; slot++)
			;
		if (slot == ATHX_HTT_CTL_N || total > ATHX_HTT_CTL_SZ)
			return EAGAIN;
		buf = athx_buf_va(&sc->htt.ctl, slot);
		paddr = athx_buf_pa(&sc->htt.ctl, slot);
	} else {
		buf = athx_ce_src_buf(sc, ep->ul_pipe, &paddr);
		if (buf == NULL || total > (int)sc->ce[ep->ul_pipe].src_sz_max)
			return EMSGSIZE;
	}
	if (athx_ce_free_src(sc, ep->ul_pipe) < 1)
		return EAGAIN;
	hdr = (struct ath10k_htc_hdr *)buf;
	memset(hdr, 0, sizeof(*hdr));
	hdr->eid = eid;
	hdr->len = htole16(len);
	if (ep->credit_flow)
		hdr->flags |= ATH10K_HTC_FLAG_NEED_CREDIT_UPDATE;
	hdr->seq_no = ep->seq_no++;
	memcpy(buf + sizeof(*hdr), data, len);
	l = total;
	ret = athx_ce_send_sg(sc, ep->ul_pipe, &paddr, &l, 1,
	    slot >= 0 ? (void *)&sc->htt.ctl_busy[slot] : NULL, eid);
	if (ret)
		return EAGAIN;
	if (slot >= 0)
		sc->htt.ctl_busy[slot] = 1;
	ep->tx_credits -= credits;
	return 0;
}

static void
htc_reset_endpoints(struct athx_softc *sc)
{
	int i;

	for (i = 0; i < ATHX_HTC_EP_COUNT; i++) {
		memset(&sc->ep[i], 0, sizeof(sc->ep[i]));
		sc->ep[i].credit_flow = 1;
	}
}

/*
 * A service connected to an endpoint the target assigns, with the pipes
 * the map gives it; the pseudo control service is endpoint 0 without
 * asking. 0 or an errno value
 */
int
athx_htc_connect(struct athx_softc *sc, uint16_t service_id, void (*rx)(struct athx_softc *, uint8_t *, int),
    int *eidp)
{
	struct {
		struct ath10k_ath10k_htc_msg_hdr hdr;
		struct ath10k_htc_conn_svc svc;
	} __packed req;
	const struct ath10k_htc_msg *msg;
	const struct ath10k_htc_conn_svc_response *resp;
	struct athx_htc_ep *ep;
	int eid, max_msg, ret, no_credit_flow = 0, tx_alloc = 0;
	uint16_t flags = 0;

	if (service_id == ATH10K_HTC_SVC_ID_RSVD_CTRL) {
		eid = ATH10K_HTC_EP_0;
		max_msg = ATH10K_HTC_MAX_CTRL_MSG_LEN;
		no_credit_flow = 1;
		goto setup;
	}
	/* WMI's control service is the only one with flow control: it has all the credits */
	if (service_id == ATH10K_HTC_SVC_ID_WMI_CONTROL)
		tx_alloc = sc->htc_total_credits;
	memset(&req, 0, sizeof(req));
	req.hdr.message_id = htole16(ATH10K_HTC_MSG_CONNECT_SERVICE_ID);
	flags |= SM(tx_alloc, ATH10K_HTC_CONN_FLAGS_RECV_ALLOC);
	if (service_id != ATH10K_HTC_SVC_ID_WMI_CONTROL) {
		flags |= ATH10K_HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL;
		no_credit_flow = 1;
	}
	req.svc.flags = htole16(flags);
	req.svc.service_id = htole16(service_id);
	sc->ctl_resp_done = 0;
	ret = athx_htc_send(sc, ATH10K_HTC_EP_0, &req, sizeof(req));
	if (ret)
		return ret;
	ret = athx_wait(sc, &sc->ctl_resp_done, "athxconn", 1000);
	if (ret) {
		printf("%s: Service connect timeout\n", sc->sc_dev.dv_xname);
		return ret;
	}
	msg = (const struct ath10k_htc_msg *)sc->ctl_resp;
	resp = &msg->connect_service_response;
	if (le16toh(msg->hdr.message_id) != ATH10K_HTC_MSG_CONNECT_SERVICE_RESP_ID ||
	    sc->ctl_resp_len < (int)(sizeof(msg->hdr) + sizeof(*resp))) {
		printf("%s: Invalid resp message ID 0x%x\n", sc->sc_dev.dv_xname,
		    le16toh(msg->hdr.message_id));
		return EPROTO;
	}
	if (resp->status != ATH10K_HTC_CONN_SVC_STATUS_SUCCESS) {
		printf("%s: HTC Service %04x connect request failed: 0x%x\n", sc->sc_dev.dv_xname,
		    service_id, resp->status);
		return EPROTO;
	}
	eid = resp->eid;
	max_msg = le16toh(resp->max_msg_size);
 setup:
	if (eid >= ATHX_HTC_EP_COUNT || max_msg == 0)
		return EPROTO;
	ep = &sc->ep[eid];
	if (ep->service_id != 0)
		return EPROTO;
	ep->service_id = service_id;
	ep->max_msg_len = max_msg;
	ep->tx_credits = tx_alloc;
	ep->tx_credit_size = sc->htc_credit_size ? sc->htc_credit_size : 1;
	if (service_id == ATH10K_HTC_SVC_ID_HTT_DATA_MSG && sc->htc_alt_data_credit_size != 0)
		ep->tx_credit_size = sc->htc_alt_data_credit_size;
	ep->rx = rx;
	ret = athx_map_service_to_pipe(sc, service_id, &ep->ul_pipe, &ep->dl_pipe);
	if (ret) {
		ep->service_id = 0;
		return ret;
	}
	if (no_credit_flow)
		ep->credit_flow = 0;
	if (eidp != NULL)
		*eidp = eid;
	printf("%s: HTC service %04x: endpoint %d, pipes %d out %d in, %d credits\n", sc->sc_dev.dv_xname,
	    service_id, eid, ep->ul_pipe, ep->dl_pipe, ep->tx_credits);
	return 0;
}

/* HTC's ready message waited for: the target's credits and their size */
static int
htc_wait_target(struct athx_softc *sc)
{
	const struct ath10k_htc_msg *msg = (const struct ath10k_htc_msg *)sc->ctl_resp;
	int ret;

	ret = athx_wait(sc, &sc->ctl_resp_done, "athxhtc", 1000);
	if (ret == ETIMEDOUT) {
		printf("%s: failed to receive control response completion, polling..\n", sc->sc_dev.dv_xname);
		athx_ce_poll_all(sc);
		ret = athx_wait(sc, &sc->ctl_resp_done, "athxhtc", 1000);
	}
	if (ret) {
		printf("%s: ctl_resp never came in (%d)\n", sc->sc_dev.dv_xname, ret);
		athx_say_regs(sc, "no HTC ready");
		return ret;
	}
	if (sc->ctl_resp_len < (int)(sizeof(msg->hdr) + sizeof(msg->ready)) ||
	    le16toh(msg->hdr.message_id) != ATH10K_HTC_MSG_READY_ID) {
		printf("%s: Invalid HTC ready msg (len %d id 0x%x)\n", sc->sc_dev.dv_xname, sc->ctl_resp_len,
		    le16toh(msg->hdr.message_id));
		return EIO;
	}
	sc->htc_total_credits = le16toh(msg->ready.credit_count);
	sc->htc_credit_size = le16toh(msg->ready.credit_size);
	if (sc->htc_total_credits == 0 || sc->htc_credit_size == 0) {
		printf("%s: Invalid credit size received\n", sc->sc_dev.dv_xname);
		return EIO;
	}
	if (sc->ctl_resp_len >= (int)(sizeof(msg->hdr) + sizeof(msg->ready_ext)))
		sc->htc_alt_data_credit_size = le16toh(msg->ready_ext.reserved) &
		    ATH10K_HTC_MSG_READY_EXT_ALT_DATA_MASK;
	printf("%s: HTC ready: %d credits of %d bytes\n", sc->sc_dev.dv_xname, sc->htc_total_credits,
	    sc->htc_credit_size);
	return 0;
}

/* HTC's setup completed to the target (after every service is connected) */
static int
htc_start(struct athx_softc *sc)
{
	struct {
		struct ath10k_ath10k_htc_msg_hdr hdr;
		struct ath10k_htc_setup_complete_extended ext;
	} __packed msg;

	memset(&msg, 0, sizeof(msg));
	msg.hdr.message_id = htole16(ATH10K_HTC_MSG_SETUP_COMPLETE_EX_ID);
	return athx_htc_send(sc, ATH10K_HTC_EP_0, &msg, sizeof(msg));
}

/* ---------------------------------------------------------------- the order */

/*
 * The chip from reset to running firmware with HTC, WMI and HTT up, and
 * its address read: 0 or an errno value, with sc->stage where it stopped
 */
int
athx_boot(struct athx_softc *sc)
{
	uint32_t type = 0;
	int ret;

	sc->wedged = 0;
	sc->bmi_done_sent = 0;
	ret = athx_pci_power_up(sc);
	if (ret)
		return ret;
	ret = bmi_get_target_info(sc, &sc->target_version, &type);
	if (ret) {
		printf("%s: could not get target info (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	sc->stage = ATHX_ST_BMI;
	printf("%s: chip id %08x (rev %d), target version %08x type %u\n", sc->sc_dev.dv_xname, sc->chip_id,
	    MS(sc->chip_id, SOC_CHIP_ID_REV), sc->target_version, type);
	if (sc->target_version != QCA6174_HW_3_0_VERSION && sc->target_version != QCA6174_HW_3_2_VERSION) {
		printf("%s: the target is not a QCA6174 hw3.0 to hw3.2 (version %08x)\n", sc->sc_dev.dv_xname,
		    sc->target_version);
		return ENODEV;
	}

	if (sc->fw_file == NULL) {
		ret = athx_fetch_firmware(sc);
		if (ret)
			return ret;
		ret = athx_board_id_from_otp(sc);
		if (ret && ret != EOPNOTSUPP) {
			printf("%s: failed to get board id from otp: %d\n", sc->sc_dev.dv_xname, ret);
			return ret;
		}
		ret = athx_fetch_board(sc);
		if (ret)
			return ret;
	}
	sc->stage = ATHX_ST_FILES;

	/* the bootloader again from the start of its commands */
	sc->bmi_done_sent = 0;
	ret = athx_enable_pll_clock(sc);
	printf("%s: boot enable pll ret %d\n", sc->sc_dev.dv_xname, ret);
	ret = athx_init_configure_target(sc);
	if (ret)
		return EINVAL;
	ret = athx_download_and_run_otp(sc);
	if (ret) {
		printf("%s: failed to run otp: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_download_fw(sc);
	if (ret) {
		printf("%s: failed to download the firmware: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_write32(sc, HI_ITEM(hi_serial_enable), 0);
	if (ret) {
		printf("%s: could not disable UART prints (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	sc->stage = ATHX_ST_DOWNLOAD;

	/* HTC's pseudo control service on endpoint 0, then the bootloader told it is done */
	htc_reset_endpoints(sc);
	sc->ctl_resp_done = 0;
	ret = athx_htc_connect(sc, ATH10K_HTC_SVC_ID_RSVD_CTRL, htc_control_rx, NULL);
	if (ret) {
		printf("%s: could not connect to htc service (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = bmi_done(sc);
	if (ret)
		return ret;
	ret = athx_htt_alloc(sc);
	if (ret) {
		printf("%s: failed to alloc htt: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_pci_start(sc);
	if (ret)
		return ret;
	ret = htc_wait_target(sc);
	if (ret) {
		printf("%s: failed to connect to HTC: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_htt_connect(sc);
	if (ret) {
		printf("%s: failed to connect htt (%d)\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_wmi_connect(sc);
	if (ret) {
		printf("%s: could not connect wmi: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = htc_start(sc);
	if (ret) {
		printf("%s: failed to start htc: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	sc->stage = ATHX_ST_HTC;

	ret = athx_wmi_init(sc);
	if (ret)
		return ret;
	sc->stage = ATHX_ST_WMI;

	ret = athx_htt_rx_ring_refill(sc);
	if (ret) {
		printf("%s: failed to refill htt rx ring: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	ret = athx_htt_setup(sc);
	if (ret) {
		printf("%s: failed to setup htt: %d\n", sc->sc_dev.dv_xname, ret);
		return ret;
	}
	sc->stage = ATHX_ST_HTT;
	return 0;
}
