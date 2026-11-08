/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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
 *	athxreg.h (x64 PC, WIFI_NET80211=obsd)
 *	What the host and Qualcomm's QCA6174 (hw3.x, PCI 168c:003e) share: the
 *	SoC's registers and their values on this chip, the copy engines'
 *	descriptors and configuration, the bootloader's messages (BMI), the
 *	firmware and board files' elements, and the messages of the three
 *	layers above the copy engines (HTC, WMI in its TLV form, HTT) with the
 *	receive descriptor the chip writes in front of each frame. All the
 *	values are the chip's and its firmware's; every field is little-endian.
 *
 *	The declarations after the chip's own register values below are kept
 *	as the firmware's interface documents them, field for field, with the
 *	kernel types they are written in given here.
 */

#ifndef __ATHXREG_H__
#define __ATHXREG_H__

#include <sys/types.h>

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int8_t		s8;
typedef int16_t		s16;
typedef int32_t		s32;
typedef uint16_t	__le16;
typedef uint32_t	__le32;
typedef uint64_t	__le64;
typedef uint16_t	__be16;
typedef uint32_t	__be32;

#ifndef BIT
#define BIT(n)			(1U << (n))
#endif
#define GENMASK(h, l)		(((~0U) << (l)) & (~0U >> (31 - (h))))
#define GENMASK_ULL(h, l)	(((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define FIELD_GET(m, v)		(((v) & (m)) >> __builtin_ctz(m))
#define FIELD_PREP(m, v)	(((v) << __builtin_ctz(m)) & (m))
#define DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { struct { } __empty_ ## NAME; TYPE NAME[]; }
#ifndef offsetof
#define offsetof(t, m)		__builtin_offsetof(t, m)
#endif
#define ETH_ALEN		6
#ifndef rounddown
#define rounddown(x, y)		(((x) / (y)) * (y))
#endif

/* A field of a register or a word, by its mask and its lowest bit */
#define MS(_v, _f)		(((_v) & _f##_MASK) >> _f##_LSB)
#define SM(_v, _f)		(((_v) << _f##_LSB) & _f##_MASK)

/* ---------------------------------------------------------------- QCA6174's own values */

/* Where the SoC's blocks are in the window of BAR 0 */
#define RTC_SOC_BASE_ADDRESS			0x00000800
#define RTC_WMAC_BASE_ADDRESS			0x00001000
#define SOC_CORE_BASE_ADDRESS			0x0003a000
#define WLAN_MAC_BASE_ADDRESS			0x00010000
#define CE_WRAPPER_BASE_ADDRESS			0x00034000
#define CE0_BASE_ADDRESS			0x00034400
#define CE1_BASE_ADDRESS			0x00034800
#define SOC_RESET_CONTROL_SI0_RST_MASK		0x00000000
#define SOC_RESET_CONTROL_CE_RST_MASK		0x00000001
#define SOC_CHIP_ID_ADDRESS			0x000000f0
#define SCRATCH_3_ADDRESS			0x00000028
#define FW_INDICATOR_ADDRESS			0x0003a028
#define PCIE_LOCAL_BASE_ADDRESS			0x00080000
#define CE_WRAPPER_INTERRUPT_SUMMARY_HOST_MSI_LSB	0x00000008
#define CE_WRAPPER_INTERRUPT_SUMMARY_HOST_MSI_MASK	0x0000ff00
#define PCIE_INTR_FIRMWARE_MASK			0x00000400
#define PCIE_INTR_CE_MASK_ALL			0x0007f800
#define PCIE_INTR_CLR_ADDRESS			0x00000014
#define CPU_PLL_INIT_ADDRESS			0x00404020
#define CPU_SPEED_ADDRESS			0x00404024
#define CORE_CLK_DIV_ADDRESS			0x00404028

#define RTC_STATE_V_ON				3
#define CE_COUNT				8
#define NUM_TARGET_CE_CONFIG_WLAN		7
#define CE_DESC_FLAGS_META_DATA_MASK		0xFFFC
#define CE_DESC_FLAGS_META_DATA_LSB		2

/* The registers of one copy engine, from its base */
#define CE_SR_BASE_ADDR_LO		0x00000000
#define CE_SR_SIZE			0x00000004
#define CE_DR_BASE_ADDR_LO		0x00000008
#define CE_DR_SIZE			0x0000000c
#define CE_CTRL1			0x00000010
#define CE_CTRL1_DMAX_MASK		GENMASK(15, 0)
#define CE_CTRL1_DMAX_LSB		0
#define CE_CTRL1_SRC_RING_BYTE_SWAP_MASK	GENMASK(16, 16)
#define CE_CTRL1_SRC_RING_BYTE_SWAP_LSB	16
#define CE_CTRL1_DST_RING_BYTE_SWAP_MASK	GENMASK(17, 17)
#define CE_CTRL1_DST_RING_BYTE_SWAP_LSB	17
#define CE_CMD				0x00000018
#define CE_HOST_IE			0x0000002c
#define CE_HOST_IE_COPY_COMPLETE_MASK	GENMASK(0, 0)
#define CE_HOST_IS			0x00000030
#define CE_HOST_IS_CC_MASK		0x00000001
#define CE_HOST_IS_WM_MASK		0x0000001E
#define CE_MISC_IE			0x00000034
#define CE_MISC_IS_ERR_MASK		0x000007E0
#define CE_SR_WR_INDEX			0x0000003c
#define CE_DST_WR_INDEX			0x00000040
#define CE_CURRENT_SRRI			0x00000044
#define CE_CURRENT_DRRI			0x00000048
#define CE_SRC_WATERMARK		0x0000004c
#define CE_SRC_WATERMARK_LOW_MASK	GENMASK(31, 16)
#define CE_SRC_WATERMARK_LOW_LSB	16
#define CE_SRC_WATERMARK_HIGH_MASK	GENMASK(15, 0)
#define CE_SRC_WATERMARK_HIGH_LSB	0
#define CE_DST_WATERMARK		0x00000050
#define CE_DST_WATERMARK_LOW_MASK	GENMASK(31, 16)
#define CE_DST_WATERMARK_LOW_LSB	16
#define CE_DST_WATERMARK_HIGH_MASK	GENMASK(15, 0)
#define CE_DST_WATERMARK_HIGH_LSB	0

/* The copy engine of the diagnostic window, and how long it is waited for */
#define CE_DIAG_PIPE			7

/* The banks of IRAM hw3.0 to hw3.2 switch to (the early allocation) */
#define QCA6174_HW_3_IRAM_BANKS		9

/* The chip of hw3.2 on PCI: what its firmware is told and loaded with */
#define QCA6174_HW_3_2_TARGET_CPU_FREQ	176000000
#define QCA6174_HW_3_2_OTP_EXE_PARAM	0

/* The PLL of each reference clock (the efuse's XTAL_SEL picks one) */
struct ath10k_hw_clk_params {
	u32 refclk;
	u32 div;
	u32 rnfrac;
	u32 settle_time;
	u32 refdiv;
	u32 outdiv;
};

/* ---------------------------------------------------------------- the firmware's interfaces */

/* The chip and its firmware files (hw.h) */

#define QCA6174_2_1_DEVICE_ID   (0x003e)
#define QCA6174_HW_3_0_VERSION		0x05020000
#define QCA6174_HW_3_2_VERSION		0x05030000
#define QCA6174_HW_3_0_PATCH_LOAD_ADDR	0x1234
#define ATH10K_FIRMWARE_MAGIC               "QCA-ATH10K"
#define ATH10K_BOARD_MAGIC                  "QCA-ATH10K-BOARD"

enum qca6174_chip_id_rev {
	QCA6174_HW_1_0_CHIP_ID_REV = 0,
	QCA6174_HW_1_1_CHIP_ID_REV = 1,
	QCA6174_HW_1_3_CHIP_ID_REV = 2,
	QCA6174_HW_2_1_CHIP_ID_REV = 4,
	QCA6174_HW_2_2_CHIP_ID_REV = 5,
	QCA6174_HW_3_0_CHIP_ID_REV = 8,
	QCA6174_HW_3_1_CHIP_ID_REV = 9,
	QCA6174_HW_3_2_CHIP_ID_REV = 10,
};

struct ath10k_fw_ie {
	__le32 id;
	__le32 len;
	u8 data[];
};

enum ath10k_fw_ie_type {
	ATH10K_FW_IE_FW_VERSION = 0,
	ATH10K_FW_IE_TIMESTAMP = 1,
	ATH10K_FW_IE_FEATURES = 2,
	ATH10K_FW_IE_FW_IMAGE = 3,
	ATH10K_FW_IE_OTP_IMAGE = 4,

	/* WMI "operations" interface version, 32 bit value. Supported from
	 * FW API 4 and above.
	 */
	ATH10K_FW_IE_WMI_OP_VERSION = 5,

	/* HTT "operations" interface version, 32 bit value. Supported from
	 * FW API 5 and above.
	 */
	ATH10K_FW_IE_HTT_OP_VERSION = 6,

	/* Code swap image for firmware binary */
	ATH10K_FW_IE_FW_CODE_SWAP_IMAGE = 7,
};

enum ath10k_fw_wmi_op_version {
	ATH10K_FW_WMI_OP_VERSION_UNSET = 0,

	ATH10K_FW_WMI_OP_VERSION_MAIN = 1,
	ATH10K_FW_WMI_OP_VERSION_10_1 = 2,
	ATH10K_FW_WMI_OP_VERSION_10_2 = 3,
	ATH10K_FW_WMI_OP_VERSION_TLV = 4,
	ATH10K_FW_WMI_OP_VERSION_10_2_4 = 5,
	ATH10K_FW_WMI_OP_VERSION_10_4 = 6,

	/* keep last */
	ATH10K_FW_WMI_OP_VERSION_MAX,
};

enum ath10k_fw_htt_op_version {
	ATH10K_FW_HTT_OP_VERSION_UNSET = 0,

	ATH10K_FW_HTT_OP_VERSION_MAIN = 1,

	/* also used in 10.2 and 10.2.4 branches */
	ATH10K_FW_HTT_OP_VERSION_10_1 = 2,

	ATH10K_FW_HTT_OP_VERSION_TLV = 3,

	ATH10K_FW_HTT_OP_VERSION_10_4 = 4,

	/* keep last */
	ATH10K_FW_HTT_OP_VERSION_MAX,
};

enum ath10k_bd_ie_type {
	/* contains sub IEs of enum ath10k_bd_ie_board_type */
	ATH10K_BD_IE_BOARD = 0,
	ATH10K_BD_IE_BOARD_EXT = 1,
};

enum ath10k_bd_ie_board_type {
	ATH10K_BD_IE_BOARD_NAME = 0,
	ATH10K_BD_IE_BOARD_DATA = 1,
};

enum ath10k_hw_txrx_mode {
	ATH10K_HW_TXRX_RAW = 0,

	/* Native Wifi decap mode is used to align IP frames to 4-byte
	 * boundaries and avoid a very expensive re-alignment in the host's
	 * 802.11 layer.
	 */
	ATH10K_HW_TXRX_NATIVE_WIFI = 1,
	ATH10K_HW_TXRX_ETHERNET = 2,

	/* Valid for HTT >= 3.0. Used for management frames in TX_FRM. */
	ATH10K_HW_TXRX_MGMT = 3,
};

enum ath10k_fw_features {
	/* wmi_mgmt_rx_hdr contains extra RSSI information */
	ATH10K_FW_FEATURE_EXT_WMI_MGMT_RX = 0,

	/* Firmware from 10X branch. Deprecated, don't use in new code. */
	ATH10K_FW_FEATURE_WMI_10X = 1,

	/* firmware support tx frame management over WMI, otherwise it's HTT */
	ATH10K_FW_FEATURE_HAS_WMI_MGMT_TX = 2,

	/* Firmware does not support P2P */
	ATH10K_FW_FEATURE_NO_P2P = 3,

	/* Firmware 10.2 feature bit. The ATH10K_FW_FEATURE_WMI_10X feature
	 * bit is required to be set as well. Deprecated, don't use in new
	 * code.
	 */
	ATH10K_FW_FEATURE_WMI_10_2 = 4,

	/* Some firmware revisions lack proper multi-interface client powersave
	 * implementation. Enabling PS could result in connection drops,
	 * traffic stalls, etc.
	 */
	ATH10K_FW_FEATURE_MULTI_VIF_PS_SUPPORT = 5,

	/* Some firmware revisions have an incomplete WoWLAN implementation
	 * despite WMI service bit being advertised. This feature flag is used
	 * to distinguish whether WoWLAN is really supported or not.
	 */
	ATH10K_FW_FEATURE_WOWLAN_SUPPORT = 6,

	/* Don't trust error code from otp.bin */
	ATH10K_FW_FEATURE_IGNORE_OTP_RESULT = 7,

	/* Some firmware revisions pad 4th hw address to 4 byte boundary making
	 * it 8 bytes long in Native Wifi Rx decap.
	 */
	ATH10K_FW_FEATURE_NO_NWIFI_DECAP_4ADDR_PADDING = 8,

	/* Firmware supports bypassing PLL setting on init. */
	ATH10K_FW_FEATURE_SUPPORTS_SKIP_CLOCK_INIT = 9,

	/* Raw mode support. If supported, FW supports receiving and transmitting
	 * frames in raw mode.
	 */
	ATH10K_FW_FEATURE_RAW_MODE_SUPPORT = 10,

	/* Firmware Supports Adaptive CCA*/
	ATH10K_FW_FEATURE_SUPPORTS_ADAPTIVE_CCA = 11,

	/* Firmware supports management frame protection */
	ATH10K_FW_FEATURE_MFP_SUPPORT = 12,

	/* Firmware supports pull-push model where host shares it's software
	 * queue state with firmware and firmware generates fetch requests
	 * telling host which queues to dequeue tx from.
	 *
	 * Primary function of this is improved MU-MIMO performance with
	 * multiple clients.
	 */
	ATH10K_FW_FEATURE_PEER_FLOW_CONTROL = 13,

	/* Firmware supports BT-Coex without reloading firmware via pdev param.
	 * To support Bluetooth coexistence pdev param, WMI_COEX_GPIO_SUPPORT of
	 * extended resource config should be enabled always. This firmware IE
	 * is used to configure WMI_COEX_GPIO_SUPPORT.
	 */
	ATH10K_FW_FEATURE_BTCOEX_PARAM = 14,

	/* Unused flag and proven to be not working, enable this if you want
	 * to experiment sending NULL func data frames in HTT TX
	 */
	ATH10K_FW_FEATURE_SKIP_NULL_FUNC_WAR = 15,

	/* Firmware allow other BSS mesh broadcast/multicast frames without
	 * creating monitor interface. Appropriate rxfilters are programmed for
	 * mesh vdev by firmware itself. This feature flags will be used for
	 * not creating monitor vdev while configuring mesh node.
	 */
	ATH10K_FW_FEATURE_ALLOWS_MESH_BCAST = 16,

	/* Firmware does not support power save in station mode. */
	ATH10K_FW_FEATURE_NO_PS = 17,

	/* Firmware allows management tx by reference instead of by value. */
	ATH10K_FW_FEATURE_MGMT_TX_BY_REF = 18,

	/* Firmware load is done externally, not by bmi */
	ATH10K_FW_FEATURE_NON_BMI = 19,

	/* Firmware sends only one chan_info event per channel */
	ATH10K_FW_FEATURE_SINGLE_CHAN_INFO_PER_CHANNEL = 20,

	/* Firmware allows setting peer fixed rate */
	ATH10K_FW_FEATURE_PEER_FIXED_RATE = 21,

	/* Firmware support IRAM recovery */
	ATH10K_FW_FEATURE_IRAM_RECOVERY = 22,

	/* keep last */
	ATH10K_FW_FEATURE_COUNT,
};

#define TARGET_TLV_NUM_VDEVS			4
#define TARGET_TLV_NUM_STATIONS			32
#define TARGET_TLV_NUM_PEERS			33
#define TARGET_TLV_NUM_TDLS_VDEVS		1
#define TARGET_TLV_NUM_TIDS			((TARGET_TLV_NUM_PEERS) * 2)
#define TARGET_TLV_NUM_MSDU_DESC		(1024 + 32)
#define TARGET_TLV_NUM_WOW_PATTERNS		22
#define TARGET_TLV_MGMT_NUM_MSDU_DESC		(50)


/* The registers of the SoC (hw.h, its fixed ones) */

#define CE_COUNT_MAX 12
#define RTC_STATE_V_LSB				0
#define RTC_STATE_V_MASK			0x00000007
#define RTC_STATE_ADDRESS			0x0000
#define PCIE_SOC_WAKE_V_MASK			0x00000001
#define PCIE_SOC_WAKE_ADDRESS			0x0004
#define PCIE_SOC_WAKE_RESET			0x00000000
#define SOC_GLOBAL_RESET_ADDRESS		0x0008

#define SOC_RESET_CONTROL_ADDRESS		0x00000000
#define SOC_RESET_CONTROL_CPU_WARM_RST_MASK	0x00000040
#define SOC_CPU_CLOCK_OFFSET			0x00000020
#define SOC_CPU_CLOCK_STANDARD_LSB		0
#define SOC_CPU_CLOCK_STANDARD_MASK		0x00000003
#define SOC_LF_TIMER_CONTROL0_ADDRESS		0x00000050
#define SOC_LF_TIMER_CONTROL0_ENABLE_MASK	0x00000004
#define SOC_CHIP_ID_REV_LSB			8
#define SOC_CHIP_ID_REV_MASK			0x00000f00

#define CORE_CTRL_CPU_INTR_MASK			0x00002000
#define CORE_CTRL_PCIE_REG_31_MASK		0x00000800
#define CORE_CTRL_ADDRESS			0x0000
#define PCIE_INTR_ENABLE_ADDRESS		0x0008
#define PCIE_INTR_CAUSE_ADDRESS			0x000c
#define FW_RAM_CONFIG_ADDRESS			0x0018
#define FW_IND_EVENT_PENDING			1
#define FW_IND_INITIALIZED			2
#define FW_IND_HOST_READY			0x80000000
#define DRAM_BASE_ADDRESS			0x00400000

#define SOC_CORE_CLK_CTRL_OFFSET		0x00000114
#define SOC_CORE_CLK_CTRL_DIV_LSB		0
#define SOC_CORE_CLK_CTRL_DIV_MASK		0x00000007
#define EFUSE_OFFSET				0x0000032c
#define EFUSE_XTAL_SEL_LSB			8
#define EFUSE_XTAL_SEL_MASK			0x00000700
#define BB_PLL_CONFIG_OFFSET			0x000002f4
#define BB_PLL_CONFIG_FRAC_LSB			0
#define BB_PLL_CONFIG_FRAC_MASK			0x0003ffff
#define BB_PLL_CONFIG_OUTDIV_LSB		18
#define BB_PLL_CONFIG_OUTDIV_MASK		0x001c0000

#define WLAN_PLL_SETTLE_OFFSET			0x0018
#define WLAN_PLL_SETTLE_TIME_LSB		0
#define WLAN_PLL_SETTLE_TIME_MASK		0x000007ff
#define WLAN_PLL_CONTROL_OFFSET			0x0014
#define WLAN_PLL_CONTROL_DIV_LSB		0
#define WLAN_PLL_CONTROL_DIV_MASK		0x000003ff
#define WLAN_PLL_CONTROL_REFDIV_LSB		10
#define WLAN_PLL_CONTROL_REFDIV_MASK		0x00003c00
#define WLAN_PLL_CONTROL_BYPASS_LSB		16
#define WLAN_PLL_CONTROL_BYPASS_MASK		0x00010000
#define WLAN_PLL_CONTROL_NOPWD_LSB		18
#define WLAN_PLL_CONTROL_NOPWD_MASK		0x00040000

#define RTC_SYNC_STATUS_OFFSET			0x0244
#define RTC_SYNC_STATUS_PLL_CHANGING_LSB	5
#define RTC_SYNC_STATUS_PLL_CHANGING_MASK	0x00000020
#define CPU_ADDR_MSB_REGION_MASK	GENMASK(23, 20)
#define CPU_ADDR_MSB_REGION_VAL(X)	FIELD_GET(CPU_ADDR_MSB_REGION_MASK, X)
#define REGION_ACCESS_SIZE_LIMIT	0x100000
#define REGION_ACCESS_SIZE_MASK		(REGION_ACCESS_SIZE_LIMIT - 1)

enum ath10k_hw_refclk_speed {
	ATH10K_HW_REFCLK_UNKNOWN = -1,
	ATH10K_HW_REFCLK_48_MHZ = 0,
	ATH10K_HW_REFCLK_19_2_MHZ = 1,
	ATH10K_HW_REFCLK_24_MHZ = 2,
	ATH10K_HW_REFCLK_26_MHZ = 3,
	ATH10K_HW_REFCLK_37_4_MHZ = 4,
	ATH10K_HW_REFCLK_38_4_MHZ = 5,
	ATH10K_HW_REFCLK_40_MHZ = 6,
	ATH10K_HW_REFCLK_52_MHZ = 7,

	/* must be the last one */
	ATH10K_HW_REFCLK_COUNT,
};


/* The copy engines (ce.h) */

#define CE_HTT_H2T_MSG_SRC_NENTRIES 8192
#define CE_DESC_RING_ALIGN	8
#define CE_SEND_FLAG_GATHER	0x00010000
#define CE_DESC_FLAGS_GATHER         (1 << 0)
#define CE_DESC_FLAGS_BYTE_SWAP      (1 << 1)

struct ce_desc {
	__le32 addr;
	__le16 nbytes;
	__le16 flags; /* %CE_DESC_FLAGS_ */
};

#define CE_ATTR_NO_SNOOP		BIT(0)
#define CE_ATTR_BYTE_SWAP_DATA		BIT(1)
#define CE_ATTR_SWIZZLE_DESCRIPTORS	BIT(2)
#define CE_ATTR_DIS_INTR		BIT(3)
#define CE_ATTR_POLL			BIT(4)
#define CE_ATTR_FLAGS 0

#define CE_RING_DELTA(nentries_mask, fromidx, toidx) \
	(((int)(toidx) - (int)(fromidx)) & (nentries_mask))
#define CE_RING_IDX_INCR(nentries_mask, idx) (((idx) + 1) & (nentries_mask))
#define CE_RING_IDX_ADD(nentries_mask, idx, num) \
		(((idx) + (num)) & (nentries_mask))
#define CE_WRAPPER_INTERRUPT_SUMMARY_ADDRESS			0x0000

struct ce_pipe_config {
	__le32 pipenum;
	__le32 pipedir;
	__le32 nentries;
	__le32 nbytes_max;
	__le32 flags;
	__le32 reserved;
};

#define PIPEDIR_NONE    0
#define PIPEDIR_IN      1  /* Target-->Host, WiFi Rx direction */
#define PIPEDIR_OUT     2  /* Host->Target, WiFi Tx direction */
#define PIPEDIR_INOUT   3  /* bidirectional */

struct ce_service_to_pipe {
	__le32 service_id;
	__le32 pipedir;
	__le32 pipenum;
};


/* The PCI interconnect (pci.h) */

#define DIAG_TRANSFER_LIMIT 2048
#define PCIE_CONFIG_FLAG_ENABLE_L1  0x0000001
#define PCIE_WAKE_TIMEOUT 30000	/* 30ms */
#define PCIE_WAKE_LATE_US 10000	/* 10ms */
#define DIAG_ACCESS_CE_TIMEOUT_US 10000 /* 10 ms */
#define DIAG_ACCESS_CE_WAIT_US	50

struct pcie_state {
	/* Pipe configuration Target address */
	/* NB: ce_pipe_config[CE_COUNT] */
	u32 pipe_cfg_addr;

	/* Service to pipe map Target address */
	/* NB: service_to_pipe[PIPE_TO_CE_MAP_CN] */
	u32 svc_to_pipe_map;

	/* number of MSI interrupts requested */
	u32 msi_requested;

	/* number of MSI interrupts granted */
	u32 msi_granted;

	/* Message Signalled Interrupt address */
	u32 msi_addr;

	/* Base data */
	u32 msi_data;

	/*
	 * Data for firmware interrupt;
	 * MSI data for other interrupts are
	 * in various SoC registers
	 */
	u32 msi_fw_intr_data;

	/* PCIE_PWR_METHOD_* */
	u32 power_mgmt_method;

	/* PCIE_CONFIG_FLAG_* */
	u32 config_flags;
};


/* The target's host interest area (targaddrs.h) */

#define QCA988X_HOST_INTEREST_ADDRESS    0x00400800
#define HOST_INTEREST_MAX_SIZE          0x200

struct host_interest {
	/*
	 * Pointer to application-defined area, if any.
	 * Set by Target application during startup.
	 */
	u32 hi_app_host_interest;			/* 0x00 */

	/* Pointer to register dump area, valid after Target crash. */
	u32 hi_failure_state;				/* 0x04 */

	/* Pointer to debug logging header */
	u32 hi_dbglog_hdr;				/* 0x08 */

	u32 hi_unused0c;				/* 0x0c */

	/*
	 * General-purpose flag bits, similar to SOC_OPTION_* flags.
	 * Can be used by application rather than by OS.
	 */
	u32 hi_option_flag;				/* 0x10 */

	/*
	 * Boolean that determines whether or not to
	 * display messages on the serial port.
	 */
	u32 hi_serial_enable;				/* 0x14 */

	/* Start address of DataSet index, if any */
	u32 hi_dset_list_head;				/* 0x18 */

	/* Override Target application start address */
	u32 hi_app_start;				/* 0x1c */

	/* Clock and voltage tuning */
	u32 hi_skip_clock_init;				/* 0x20 */
	u32 hi_core_clock_setting;			/* 0x24 */
	u32 hi_cpu_clock_setting;			/* 0x28 */
	u32 hi_system_sleep_setting;			/* 0x2c */
	u32 hi_xtal_control_setting;			/* 0x30 */
	u32 hi_pll_ctrl_setting_24ghz;			/* 0x34 */
	u32 hi_pll_ctrl_setting_5ghz;			/* 0x38 */
	u32 hi_ref_voltage_trim_setting;		/* 0x3c */
	u32 hi_clock_info;				/* 0x40 */

	/* Host uses BE CPU or not */
	u32 hi_be;					/* 0x44 */

	u32 hi_stack;	/* normal stack */			/* 0x48 */
	u32 hi_err_stack; /* error stack */		/* 0x4c */
	u32 hi_desired_cpu_speed_hz;			/* 0x50 */

	/* Pointer to Board Data  */
	u32 hi_board_data;				/* 0x54 */

	/*
	 * Indication of Board Data state:
	 *    0: board data is not yet initialized.
	 *    1: board data is initialized; unknown size
	 *   >1: number of bytes of initialized board data
	 */
	u32 hi_board_data_initialized;			/* 0x58 */

	u32 hi_dset_ram_index_table;			/* 0x5c */

	u32 hi_desired_baud_rate;			/* 0x60 */
	u32 hi_dbglog_config;				/* 0x64 */
	u32 hi_end_ram_reserve_sz;			/* 0x68 */
	u32 hi_mbox_io_block_sz;			/* 0x6c */

	u32 hi_num_bpatch_streams;			/* 0x70 -- unused */
	u32 hi_mbox_isr_yield_limit;			/* 0x74 */

	u32 hi_refclk_hz;				/* 0x78 */
	u32 hi_ext_clk_detected;			/* 0x7c */
	u32 hi_dbg_uart_txpin;				/* 0x80 */
	u32 hi_dbg_uart_rxpin;				/* 0x84 */
	u32 hi_hci_uart_baud;				/* 0x88 */
	u32 hi_hci_uart_pin_assignments;		/* 0x8C */

	u32 hi_hci_uart_baud_scale_val;			/* 0x90 */
	u32 hi_hci_uart_baud_step_val;			/* 0x94 */

	u32 hi_allocram_start;				/* 0x98 */
	u32 hi_allocram_sz;				/* 0x9c */
	u32 hi_hci_bridge_flags;			/* 0xa0 */
	u32 hi_hci_uart_support_pins;			/* 0xa4 */

	u32 hi_hci_uart_pwr_mgmt_params;		/* 0xa8 */

	/*
	 * 0xa8 - [1]: 0 = UART FC active low, 1 = UART FC active high
	 *        [31:16]: wakeup timeout in ms
	 */
	/* Pointer to extended board Data  */
	u32 hi_board_ext_data;				/* 0xac */
	u32 hi_board_ext_data_config;			/* 0xb0 */
	/*
	 * Bit [0]  :   valid
	 * Bit[31:16:   size
	 */
	/*
	 * hi_reset_flag is used to do some stuff when target reset.
	 * such as restore app_start after warm reset or
	 * preserve host Interest area, or preserve ROM data, literals etc.
	 */
	u32  hi_reset_flag;				/* 0xb4 */
	/* indicate hi_reset_flag is valid */
	u32  hi_reset_flag_valid;			/* 0xb8 */
	u32 hi_hci_uart_pwr_mgmt_params_ext;		/* 0xbc */
	/* 0xbc - [31:0]: idle timeout in ms */
	/* ACS flags */
	u32 hi_acs_flags;				/* 0xc0 */
	u32 hi_console_flags;				/* 0xc4 */
	u32 hi_nvram_state;				/* 0xc8 */
	u32 hi_option_flag2;				/* 0xcc */

	/* If non-zero, override values sent to Host in WMI_READY event. */
	u32 hi_sw_version_override;			/* 0xd0 */
	u32 hi_abi_version_override;			/* 0xd4 */

	/*
	 * Percentage of high priority RX traffic to total expected RX traffic
	 * applicable only to ar6004
	 */
	u32 hi_hp_rx_traffic_ratio;			/* 0xd8 */

	/* test applications flags */
	u32 hi_test_apps_related;			/* 0xdc */
	/* location of test script */
	u32 hi_ota_testscript;				/* 0xe0 */
	/* location of CAL data */
	u32 hi_cal_data;				/* 0xe4 */

	/* Number of packet log buffers */
	u32 hi_pktlog_num_buffers;			/* 0xe8 */

	/* wow extension configuration */
	u32 hi_wow_ext_config;				/* 0xec */
	u32 hi_pwr_save_flags;				/* 0xf0 */

	/* Spatial Multiplexing Power Save (SMPS) options */
	u32 hi_smps_options;				/* 0xf4 */

	/* Interconnect-specific state */
	u32 hi_interconnect_state;			/* 0xf8 */

	/* Coex configuration flags */
	u32 hi_coex_config;				/* 0xfc */

	/* Early allocation support */
	u32 hi_early_alloc;				/* 0x100 */
	/* FW swap field */
	/*
	 * Bits of this 32bit word will be used to pass specific swap
	 * instruction to FW
	 */
	/*
	 * Bit 0 -- AP Nart descriptor no swap. When this bit is set
	 * FW will not swap TX descriptor. Meaning packets are formed
	 * on the target processor.
	 */
	/* Bit 1 - unused */
	u32 hi_fw_swap;					/* 0x104 */

	/* global arenas pointer address, used by host driver debug */
	u32 hi_dynamic_mem_arenas_addr;			/* 0x108 */

	/* allocated bytes of DRAM use by allocated */
	u32 hi_dynamic_mem_allocated;			/* 0x10C */

	/* remaining bytes of DRAM */
	u32 hi_dynamic_mem_remaining;			/* 0x110 */

	/* memory track count, configured by host */
	u32 hi_dynamic_mem_track_max;			/* 0x114 */

	/* minidump buffer */
	u32 hi_minidump;				/* 0x118 */

	/* bdata's sig and key addr */
	u32 hi_bd_sig_key;				/* 0x11c */
} __packed;

#define HI_ITEM(item)  offsetof(struct host_interest, item)
#define HI_OPTION_MAC_ADDR_METHOD_SHIFT 3
#define HI_OPTION_FW_MODE_AP      0x2 /* AP Mode */
#define HI_OPTION_NUM_DEV_SHIFT   0x9
#define HI_OPTION_FW_BRIDGE_SHIFT 0x04
#define HI_OPTION_FW_MODE_SHIFT        0xC
#define HI_OPTION_FW_SUBMODE_SHIFT     0x14
#define HI_OPTION_EARLY_CFG_DONE    0x10 /* Early configuration is complete */

#define HI_EARLY_ALLOC_MAGIC		0x6d8a
#define HI_EARLY_ALLOC_MAGIC_MASK	0xffff0000
#define HI_EARLY_ALLOC_MAGIC_SHIFT	16
#define HI_EARLY_ALLOC_IRAM_BANKS_MASK	0x0000000f
#define HI_EARLY_ALLOC_IRAM_BANKS_SHIFT	0

#define QCA6174_BOARD_DATA_SZ     8192
#define QCA6174_BOARD_EXT_DATA_SZ 0


/* The bootloader's messages (bmi.h) */

#define BMI_MAX_DATA_SIZE	256
#define BMI_MAX_CMDBUF_SIZE (BMI_MAX_DATA_SIZE + \
			sizeof(u32) + \
			sizeof(u32) + \
			sizeof(u32))

enum bmi_cmd_id {
	BMI_NO_COMMAND          = 0,
	BMI_DONE                = 1,
	BMI_READ_MEMORY         = 2,
	BMI_WRITE_MEMORY        = 3,
	BMI_EXECUTE             = 4,
	BMI_SET_APP_START       = 5,
	BMI_READ_SOC_REGISTER   = 6,
	BMI_READ_SOC_WORD       = 6,
	BMI_WRITE_SOC_REGISTER  = 7,
	BMI_WRITE_SOC_WORD      = 7,
	BMI_GET_TARGET_ID       = 8,
	BMI_GET_TARGET_INFO     = 8,
	BMI_ROMPATCH_INSTALL    = 9,
	BMI_ROMPATCH_UNINSTALL  = 10,
	BMI_ROMPATCH_ACTIVATE   = 11,
	BMI_ROMPATCH_DEACTIVATE = 12,
	BMI_LZ_STREAM_START     = 13, /* should be followed by LZ_DATA */
	BMI_LZ_DATA             = 14,
	BMI_NVRAM_PROCESS       = 15,
};

#define BMI_NVRAM_SEG_NAME_SZ 16
#define BMI_PARAM_GET_EEPROM_BOARD_ID 0x10
#define BMI_PARAM_GET_FLASH_BOARD_ID 0x8000
#define BMI_PARAM_FLASH_SECTION_ALL 0x10000
#define ATH10K_BMI_EXT_BOARD_ID_SUPPORT 0x40000
#define ATH10K_BMI_BOARD_ID_FROM_OTP_MASK   0x7c00
#define ATH10K_BMI_BOARD_ID_FROM_OTP_LSB    10
#define ATH10K_BMI_CHIP_ID_FROM_OTP_MASK    0x18000
#define ATH10K_BMI_CHIP_ID_FROM_OTP_LSB     15
#define ATH10K_BMI_BOARD_ID_STATUS_MASK 0xff

struct bmi_cmd {
	__le32 id; /* enum bmi_cmd_id */
	union {
		struct {
		} done;
		struct {
			__le32 addr;
			__le32 len;
		} read_mem;
		struct {
			__le32 addr;
			__le32 len;
			u8 payload[];
		} write_mem;
		struct {
			__le32 addr;
			__le32 param;
		} execute;
		struct {
			__le32 addr;
		} set_app_start;
		struct {
			__le32 addr;
		} read_soc_reg;
		struct {
			__le32 addr;
			__le32 value;
		} write_soc_reg;
		struct {
		} get_target_info;
		struct {
			__le32 rom_addr;
			__le32 ram_addr; /* or value */
			__le32 size;
			__le32 activate; /* 0=install, but dont activate */
		} rompatch_install;
		struct {
			__le32 patch_id;
		} rompatch_uninstall;
		struct {
			__le32 count;
			__le32 patch_ids[]; /* length of @count */
		} rompatch_activate;
		struct {
			__le32 count;
			__le32 patch_ids[]; /* length of @count */
		} rompatch_deactivate;
		struct {
			__le32 addr;
		} lz_start;
		struct {
			__le32 len; /* max BMI_MAX_DATA_SIZE */
			u8 payload[]; /* length of @len */
		} lz_data;
		struct {
			u8 name[BMI_NVRAM_SEG_NAME_SZ];
		} nvram_process;
		u8 payload[BMI_MAX_CMDBUF_SIZE];
	};
} __packed;

union bmi_resp {
	struct {
		DECLARE_FLEX_ARRAY(u8, payload);
	} read_mem;
	struct {
		__le32 result;
	} execute;
	struct {
		__le32 value;
	} read_soc_reg;
	struct {
		__le32 len;
		__le32 version;
		__le32 type;
	} get_target_info;
	struct {
		__le32 patch_id;
	} rompatch_install;
	struct {
		__le32 patch_id;
	} rompatch_uninstall;
	struct {
		/* 0 = nothing executed
		 * otherwise = NVRAM segment return value
		 */
		__le32 result;
	} nvram_process;
	u8 payload[BMI_MAX_CMDBUF_SIZE];
} __packed;

struct bmi_segmented_file_header {
	__le32 magic_num;
	__le32 file_flags;
	u8 data[];
};

struct bmi_segmented_metadata {
	__le32 addr;
	__le32 length;
	u8 data[];
};

#define BMI_SGMTFILE_MAGIC_NUM          0x544d4753 /* "SGMT" */
#define BMI_SGMTFILE_FLAG_COMPRESS      1
#define BMI_SGMTFILE_DONE               0xffffffff
#define BMI_SGMTFILE_BDDATA             0xfffffffe
#define BMI_SGMTFILE_BEGINADDR          0xfffffffd
#define BMI_SGMTFILE_EXEC               0xfffffffc
#define BMI_CE_NUM_TO_TARG 0
#define BMI_CE_NUM_TO_HOST 1


/* The host-target communication (htc.h) */

enum ath10k_htc_tx_flags {
	ATH10K_HTC_FLAG_NEED_CREDIT_UPDATE = 0x01,
	ATH10K_HTC_FLAG_SEND_BUNDLE        = 0x02
};

enum ath10k_htc_rx_flags {
	ATH10K_HTC_FLAGS_RECV_1MORE_BLOCK = 0x01,
	ATH10K_HTC_FLAG_TRAILER_PRESENT = 0x02,
};

struct ath10k_htc_hdr {
	u8 eid; /* @enum ath10k_htc_ep_id */
	u8 flags; /* @enum ath10k_htc_tx_flags, ath10k_htc_rx_flags */
	__le16 len;
	union {
		u8 trailer_len; /* for rx */
		u8 control_byte0;
	} __packed;
	union {
		u8 seq_no; /* for tx */
		u8 control_byte1;
	} __packed;
	union {
		__le16 pad_len;
		struct {
			u8 pad0;
			u8 pad1;
		} __packed;
	} __packed;

} __packed __aligned(4);

enum ath10k_ath10k_htc_msg_id {
	ATH10K_HTC_MSG_READY_ID                = 1,
	ATH10K_HTC_MSG_CONNECT_SERVICE_ID      = 2,
	ATH10K_HTC_MSG_CONNECT_SERVICE_RESP_ID = 3,
	ATH10K_HTC_MSG_SETUP_COMPLETE_ID       = 4,
	ATH10K_HTC_MSG_SETUP_COMPLETE_EX_ID    = 5,
	ATH10K_HTC_MSG_SEND_SUSPEND_COMPLETE   = 6
};

enum ath10k_htc_conn_flags {
	ATH10K_HTC_CONN_FLAGS_THRESHOLD_LEVEL_ONE_FOURTH    = 0x0,
	ATH10K_HTC_CONN_FLAGS_THRESHOLD_LEVEL_ONE_HALF      = 0x1,
	ATH10K_HTC_CONN_FLAGS_THRESHOLD_LEVEL_THREE_FOURTHS = 0x2,
	ATH10K_HTC_CONN_FLAGS_THRESHOLD_LEVEL_UNITY         = 0x3,
#define ATH10K_HTC_CONN_FLAGS_THRESHOLD_LEVEL_MASK 0x3
	ATH10K_HTC_CONN_FLAGS_REDUCE_CREDIT_DRIBBLE    = 1 << 2,
	ATH10K_HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL = 1 << 3
#define ATH10K_HTC_CONN_FLAGS_RECV_ALLOC_MASK 0xFF00
#define ATH10K_HTC_CONN_FLAGS_RECV_ALLOC_LSB  8
};

enum ath10k_htc_conn_svc_status {
	ATH10K_HTC_CONN_SVC_STATUS_SUCCESS      = 0,
	ATH10K_HTC_CONN_SVC_STATUS_NOT_FOUND    = 1,
	ATH10K_HTC_CONN_SVC_STATUS_FAILED       = 2,
	ATH10K_HTC_CONN_SVC_STATUS_NO_RESOURCES = 3,
	ATH10K_HTC_CONN_SVC_STATUS_NO_MORE_EP   = 4
};

struct ath10k_ath10k_htc_msg_hdr {
	__le16 message_id; /* @enum htc_message_id */
} __packed;

struct ath10k_htc_unknown {
	u8 pad0;
	u8 pad1;
} __packed;

struct ath10k_htc_ready {
	__le16 credit_count;
	__le16 credit_size;
	u8 max_endpoints;
	u8 pad0;
} __packed;

struct ath10k_htc_ready_extended {
	struct ath10k_htc_ready base;
	u8 htc_version; /* @enum ath10k_htc_version */
	u8 max_msgs_per_htc_bundle;
	union {
		__le16 reserved;
		struct {
			u8 pad0;
			u8 pad1;
		} __packed;
	} __packed;

} __packed;

struct ath10k_htc_conn_svc {
	__le16 service_id;
	__le16 flags; /* @enum ath10k_htc_conn_flags */
	u8 pad0;
	u8 pad1;
} __packed;

struct ath10k_htc_conn_svc_response {
	__le16 service_id;
	u8 status; /* @enum ath10k_htc_conn_svc_status */
	u8 eid;
	__le16 max_msg_size;
} __packed;

struct ath10k_htc_setup_complete_extended {
	u8 pad0;
	u8 pad1;
	__le32 flags; /* @enum htc_setup_complete_flags */
	u8 max_msgs_per_bundled_recv;
	u8 pad2;
	u8 pad3;
	u8 pad4;
} __packed;

struct ath10k_htc_msg {
	struct ath10k_ath10k_htc_msg_hdr hdr;
	union {
		/* host-to-target */
		struct ath10k_htc_conn_svc connect_service;
		struct ath10k_htc_ready ready;
		struct ath10k_htc_ready_extended ready_ext;
		struct ath10k_htc_unknown unknown;
		struct ath10k_htc_setup_complete_extended setup_complete_ext;

		/* target-to-host */
		struct ath10k_htc_conn_svc_response connect_service_response;
	};
} __packed __aligned(4);

enum ath10k_ath10k_htc_record_id {
	ATH10K_HTC_RECORD_NULL             = 0,
	ATH10K_HTC_RECORD_CREDITS          = 1,
	ATH10K_HTC_RECORD_LOOKAHEAD        = 2,
	ATH10K_HTC_RECORD_LOOKAHEAD_BUNDLE = 3,
};

struct ath10k_ath10k_htc_record_hdr {
	u8 id; /* @enum ath10k_ath10k_htc_record_id */
	u8 len;
	u8 pad0;
	u8 pad1;
} __packed;

struct ath10k_htc_credit_report {
	u8 eid; /* @enum ath10k_htc_ep_id */
	u8 credits;
	u8 pad0;
	u8 pad1;
} __packed;

enum ath10k_htc_svc_gid {
	ATH10K_HTC_SVC_GRP_RSVD = 0,
	ATH10K_HTC_SVC_GRP_WMI = 1,
	ATH10K_HTC_SVC_GRP_NMI = 2,
	ATH10K_HTC_SVC_GRP_HTT = 3,
	ATH10K_LOG_SERVICE_GROUP = 6,

	ATH10K_HTC_SVC_GRP_TEST = 254,
	ATH10K_HTC_SVC_GRP_LAST = 255,
};

#define SVC(group, idx) \
	(int)(((int)(group) << 8) | (int)(idx))

enum ath10k_htc_svc_id {
	/* NOTE: service ID of 0x0000 is reserved and should never be used */
	ATH10K_HTC_SVC_ID_RESERVED	= 0x0000,
	ATH10K_HTC_SVC_ID_UNUSED	= ATH10K_HTC_SVC_ID_RESERVED,

	ATH10K_HTC_SVC_ID_RSVD_CTRL	= SVC(ATH10K_HTC_SVC_GRP_RSVD, 1),
	ATH10K_HTC_SVC_ID_WMI_CONTROL	= SVC(ATH10K_HTC_SVC_GRP_WMI, 0),
	ATH10K_HTC_SVC_ID_WMI_DATA_BE	= SVC(ATH10K_HTC_SVC_GRP_WMI, 1),
	ATH10K_HTC_SVC_ID_WMI_DATA_BK	= SVC(ATH10K_HTC_SVC_GRP_WMI, 2),
	ATH10K_HTC_SVC_ID_WMI_DATA_VI	= SVC(ATH10K_HTC_SVC_GRP_WMI, 3),
	ATH10K_HTC_SVC_ID_WMI_DATA_VO	= SVC(ATH10K_HTC_SVC_GRP_WMI, 4),

	ATH10K_HTC_SVC_ID_NMI_CONTROL	= SVC(ATH10K_HTC_SVC_GRP_NMI, 0),
	ATH10K_HTC_SVC_ID_NMI_DATA	= SVC(ATH10K_HTC_SVC_GRP_NMI, 1),

	ATH10K_HTC_SVC_ID_HTT_DATA_MSG	= SVC(ATH10K_HTC_SVC_GRP_HTT, 0),

	ATH10K_HTC_SVC_ID_HTT_DATA2_MSG = SVC(ATH10K_HTC_SVC_GRP_HTT, 1),
	ATH10K_HTC_SVC_ID_HTT_DATA3_MSG = SVC(ATH10K_HTC_SVC_GRP_HTT, 2),
	ATH10K_HTC_SVC_ID_HTT_LOG_MSG = SVC(ATH10K_LOG_SERVICE_GROUP, 0),
	/* raw stream service (i.e. flash, tcmd, calibration apps) */
	ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS = SVC(ATH10K_HTC_SVC_GRP_TEST, 0),
};

#define ATH10K_HTC_MSG_READY_EXT_ALT_DATA_MASK 0xFFF
#define ATH10K_HTC_MAX_LEN 4096
#define ATH10K_HTC_MAX_CTRL_MSG_LEN 256

enum ath10k_htc_ep_id {
	ATH10K_HTC_EP_UNUSED = -1,
	ATH10K_HTC_EP_0 = 0,
	ATH10K_HTC_EP_1 = 1,
	ATH10K_HTC_EP_2,
	ATH10K_HTC_EP_3,
	ATH10K_HTC_EP_4,
	ATH10K_HTC_EP_5,
	ATH10K_HTC_EP_6,
	ATH10K_HTC_EP_7,
	ATH10K_HTC_EP_8,
	ATH10K_HTC_EP_COUNT,
};


/* The wireless module interface: the common structures (wmi.h) */

#define HTC_PROTOCOL_VERSION    0x0002

struct wmi_cmd_hdr {
	__le32 cmd_id;
} __packed;

#define WMI_CMD_HDR_CMD_ID_MASK   0x00FFFFFF
#define WMI_CMD_HDR_CMD_ID_LSB    0

struct wmi_mac_addr {
	union {
		u8 addr[6];
		struct {
			u32 word0;
			u32 word1;
		} __packed;
	} __packed;
} __packed;

enum wmi_phy_mode {
	MODE_11A        = 0,   /* 11a Mode */
	MODE_11G        = 1,   /* 11b/g Mode */
	MODE_11B        = 2,   /* 11b Mode */
	MODE_11GONLY    = 3,   /* 11g only Mode */
	MODE_11NA_HT20   = 4,  /* 11a HT20 mode */
	MODE_11NG_HT20   = 5,  /* 11g HT20 mode */
	MODE_11NA_HT40   = 6,  /* 11a HT40 mode */
	MODE_11NG_HT40   = 7,  /* 11g HT40 mode */
	MODE_11AC_VHT20 = 8,
	MODE_11AC_VHT40 = 9,
	MODE_11AC_VHT80 = 10,
	/*    MODE_11AC_VHT160 = 11, */
	MODE_11AC_VHT20_2G = 11,
	MODE_11AC_VHT40_2G = 12,
	MODE_11AC_VHT80_2G = 13,
	MODE_11AC_VHT80_80 = 14,
	MODE_11AC_VHT160 = 15,
	MODE_UNKNOWN    = 16,
	MODE_MAX        = 16
};

struct wmi_channel {
	__le32 mhz;
	__le32 band_center_freq1;
	__le32 band_center_freq2; /* valid for 11ac, 80plus80 */
	union {
		__le32 flags; /* WMI_CHAN_FLAG_ */
		struct {
			u8 mode; /* only 6 LSBs */
		} __packed;
	} __packed;
	union {
		__le32 reginfo0;
		struct {
			/* note: power unit is 0.5 dBm */
			u8 min_power;
			u8 max_power;
			u8 reg_power;
			u8 reg_classid;
		} __packed;
	} __packed;
	union {
		__le32 reginfo1;
		struct {
			/* note: power unit is 1 dBm */
			u8 antenna_max;
			/* note: power unit is 0.5 dBm */
			u8 max_tx_power;
		} __packed;
	} __packed;
} __packed;

#define WMI_CHAN_FLAG_HT40_PLUS      (1 << 6)
#define WMI_CHAN_FLAG_PASSIVE        (1 << 7)
#define WMI_CHAN_FLAG_ADHOC_ALLOWED  (1 << 8)
#define WMI_CHAN_FLAG_AP_DISABLED    (1 << 9)
#define WMI_CHAN_FLAG_DFS            (1 << 10)
#define WMI_CHAN_FLAG_ALLOW_HT       (1 << 11)
#define WMI_CHAN_FLAG_ALLOW_VHT      (1 << 12)

#define WMI_HT_CAP_ENABLED                0x0001   /* HT Enabled/ disabled */
#define WMI_HT_CAP_HT20_SGI       0x0002   /* Short Guard Interval with HT20 */
#define WMI_HT_CAP_DYNAMIC_SMPS           0x0004   /* Dynamic MIMO powersave */
#define WMI_HT_CAP_TX_STBC                0x0008   /* B3 TX STBC */
#define WMI_HT_CAP_TX_STBC_MASK_SHIFT     3
#define WMI_HT_CAP_RX_STBC                0x0030   /* B4-B5 RX STBC */
#define WMI_HT_CAP_RX_STBC_MASK_SHIFT     4
#define WMI_HT_CAP_LDPC                   0x0040   /* LDPC supported */
#define WMI_HT_CAP_L_SIG_TXOP_PROT        0x0080   /* L-SIG TXOP Protection */
#define WMI_HT_CAP_MPDU_DENSITY           0x0700   /* MPDU Density */
#define WMI_HT_CAP_MPDU_DENSITY_MASK_SHIFT 8
#define WMI_HT_CAP_HT40_SGI               0x0800
#define WMI_HT_CAP_RX_LDPC                0x1000   /* LDPC RX support */
#define WMI_HT_CAP_TX_LDPC                0x2000   /* LDPC TX support */
#define WMI_VHT_CAP_MAX_MPDU_LEN_MASK            0x00000003

struct hal_reg_capabilities {
	/* regdomain value specified in EEPROM */
	__le32 eeprom_rd;
	/*regdomain */
	__le32 eeprom_rd_ext;
	/* CAP1 capabilities bit map. */
	__le32 regcap1;
	/* REGDMN EEPROM CAP. */
	__le32 regcap2;
	/* REGDMN MODE */
	__le32 wireless_modes;
	__le32 low_2ghz_chan;
	__le32 high_2ghz_chan;
	__le32 low_5ghz_chan;
	__le32 high_5ghz_chan;
} __packed;

struct wlan_host_mem_req {
	/* ID of the request */
	__le32 req_id;
	/* size of the  of each unit */
	__le32 unit_size;
	/* flags to  indicate that
	 * the number units is dependent
	 * on number of resources(num vdevs num peers .. etc)
	 */
	__le32 num_unit_info;
	/*
	 * actual number of units to allocate . if flags in the num_unit_info
	 * indicate that number of units is tied to number of a particular
	 * resource to allocate then  num_units filed is set to 0 and host
	 * will derive the number units from number of the resources it is
	 * requesting.
	 */
	__le32 num_units;
} __packed;

#define NUM_UNITS_IS_NUM_VDEVS         BIT(0)
#define NUM_UNITS_IS_NUM_PEERS         BIT(1)
#define NUM_UNITS_IS_NUM_ACTIVE_PEERS  BIT(2)
#define WMI_MAX_MEM_REQS 16

struct wmi_ssid {
	__le32 ssid_len;
	u8 ssid[32];
} __packed;

#define WMI_HOST_SCAN_REQUESTOR_ID_PREFIX 0xA000
#define WMI_HOST_SCAN_REQ_ID_PREFIX 0xA000

enum wmi_scan_priority {
	WMI_SCAN_PRIORITY_VERY_LOW = 0,
	WMI_SCAN_PRIORITY_LOW,
	WMI_SCAN_PRIORITY_MEDIUM,
	WMI_SCAN_PRIORITY_HIGH,
	WMI_SCAN_PRIORITY_VERY_HIGH,
	WMI_SCAN_PRIORITY_COUNT   /* number of priorities supported */
};

struct wmi_start_scan_common {
	/* Scan ID */
	__le32 scan_id;
	/* Scan requestor ID */
	__le32 scan_req_id;
	/* VDEV id(interface) that is requesting scan */
	__le32 vdev_id;
	/* Scan Priority, input to scan scheduler */
	__le32 scan_priority;
	/* Scan events subscription */
	__le32 notify_scan_events;
	/* dwell time in msec on active channels */
	__le32 dwell_time_active;
	/* dwell time in msec on passive channels */
	__le32 dwell_time_passive;
	/*
	 * min time in msec on the BSS channel,only valid if at least one
	 * VDEV is active
	 */
	__le32 min_rest_time;
	/*
	 * max rest time in msec on the BSS channel,only valid if at least
	 * one VDEV is active
	 */
	/*
	 * the scanner will rest on the bss channel at least min_rest_time
	 * after min_rest_time the scanner will start checking for tx/rx
	 * activity on all VDEVs. if there is no activity the scanner will
	 * switch to off channel. if there is activity the scanner will let
	 * the radio on the bss channel until max_rest_time expires.at
	 * max_rest_time scanner will switch to off channel irrespective of
	 * activity. activity is determined by the idle_time parameter.
	 */
	__le32 max_rest_time;
	/*
	 * time before sending next set of probe requests.
	 * The scanner keeps repeating probe requests transmission with
	 * period specified by repeat_probe_time.
	 * The number of probe requests specified depends on the ssid_list
	 * and bssid_list
	 */
	__le32 repeat_probe_time;
	/* time in msec between 2 consecutive probe requests with in a set. */
	__le32 probe_spacing_time;
	/*
	 * data inactivity time in msec on bss channel that will be used by
	 * scanner for measuring the inactivity.
	 */
	__le32 idle_time;
	/* maximum time in msec allowed for scan  */
	__le32 max_scan_time;
	/*
	 * delay in msec before sending first probe request after switching
	 * to a channel
	 */
	__le32 probe_delay;
	/* Scan control flags */
	__le32 scan_ctrl_flags;
} __packed;

#define WMI_SCAN_FLAG_PASSIVE        0x1
#define WMI_SCAN_ADD_BCAST_PROBE_REQ 0x2
#define WMI_SCAN_ADD_CCK_RATES 0x4
#define WMI_SCAN_ADD_OFDM_RATES 0x8
#define WMI_SCAN_CHAN_STAT_EVENT 0x10
#define WMI_SCAN_FILTER_PROBE_REQ 0x20

enum wmi_stop_scan_type {
	WMI_SCAN_STOP_ONE	= 0x00000000, /* stop by scan_id */
	WMI_SCAN_STOP_VDEV_ALL	= 0x01000000, /* stop by vdev_id */
	WMI_SCAN_STOP_ALL	= 0x04000000, /* stop all scans */
};

struct wmi_stop_scan_cmd {
	__le32 scan_req_id;
	__le32 scan_id;
	__le32 req_type;
	__le32 vdev_id;
} __packed;

enum wmi_scan_event_type {
	WMI_SCAN_EVENT_STARTED              = BIT(0),
	WMI_SCAN_EVENT_COMPLETED            = BIT(1),
	WMI_SCAN_EVENT_BSS_CHANNEL          = BIT(2),
	WMI_SCAN_EVENT_FOREIGN_CHANNEL      = BIT(3),
	WMI_SCAN_EVENT_DEQUEUED             = BIT(4),
	/* possibly by high-prio scan */
	WMI_SCAN_EVENT_PREEMPTED            = BIT(5),
	WMI_SCAN_EVENT_START_FAILED         = BIT(6),
	WMI_SCAN_EVENT_RESTARTED            = BIT(7),
	WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT = BIT(8),
	WMI_SCAN_EVENT_MAX                  = BIT(15),
};

enum wmi_scan_completion_reason {
	WMI_SCAN_REASON_COMPLETED,
	WMI_SCAN_REASON_CANCELLED,
	WMI_SCAN_REASON_PREEMPTED,
	WMI_SCAN_REASON_TIMEDOUT,
	WMI_SCAN_REASON_INTERNAL_FAILURE,
	WMI_SCAN_REASON_MAX,
};

struct wmi_scan_event {
	__le32 event_type; /* %WMI_SCAN_EVENT_ */
	__le32 reason; /* %WMI_SCAN_REASON_ */
	__le32 channel_freq; /* only valid for WMI_SCAN_EVENT_FOREIGN_CHANNEL */
	__le32 scan_req_id;
	__le32 scan_id;
	__le32 vdev_id;
} __packed;

#define WMI_RX_STATUS_OK			0x00
#define WMI_RX_STATUS_ERR_CRC			0x01
#define WMI_RX_STATUS_ERR_DECRYPT		0x08
#define WMI_RX_STATUS_ERR_MIC			0x10
#define WMI_RX_STATUS_ERR_KEY_CACHE_MISS	0x20
#define WMI_RX_STATUS_EXT_INFO		0x40

enum wmi_dfs_region {
	/* Uninitialized dfs domain */
	WMI_UNINIT_DFS_DOMAIN = 0,

	/* FCC3 dfs domain */
	WMI_FCC_DFS_DOMAIN = 1,

	/* ETSI dfs domain */
	WMI_ETSI_DFS_DOMAIN = 2,

	/*Japan dfs domain */
	WMI_MKK4_DFS_DOMAIN = 3,
};

struct wmi_vdev_create_cmd {
	__le32 vdev_id;
	__le32 vdev_type;
	__le32 vdev_subtype;
	struct wmi_mac_addr vdev_macaddr;
} __packed;

enum wmi_vdev_type {
	WMI_VDEV_TYPE_AP      = 1,
	WMI_VDEV_TYPE_STA     = 2,
	WMI_VDEV_TYPE_IBSS    = 3,
	WMI_VDEV_TYPE_MONITOR = 4,
};

#define WMI_VDEV_START_HIDDEN_SSID  (1 << 0)
#define WMI_VDEV_START_PMF_ENABLED  (1 << 1)

struct wmi_vdev_delete_cmd {
	/* unique id identifying the VDEV, generated by the caller */
	__le32 vdev_id;
} __packed;

struct wmi_vdev_up_cmd {
	__le32 vdev_id;
	__le32 vdev_assoc_id;
	struct wmi_mac_addr vdev_bssid;
} __packed;

struct wmi_vdev_stop_cmd {
	__le32 vdev_id;
} __packed;

struct wmi_vdev_down_cmd {
	__le32 vdev_id;
} __packed;

struct wmi_vdev_set_param_cmd {
	__le32 vdev_id;
	__le32 param_id;
	__le32 param_value;
} __packed;

#define WMI_MAX_KEY_INDEX   3
#define WMI_KEY_PAIRWISE 0x00
#define WMI_KEY_GROUP    0x01
#define WMI_KEY_TX_USAGE 0x02 /* default tx key - static wep */

struct wmi_key_seq_counter {
	__le32 key_seq_counter_l;
	__le32 key_seq_counter_h;
} __packed;

enum wmi_cipher_suites {
	WMI_CIPHER_NONE,
	WMI_CIPHER_WEP,
	WMI_CIPHER_TKIP,
	WMI_CIPHER_AES_OCB,
	WMI_CIPHER_AES_CCM,
	WMI_CIPHER_WAPI,
	WMI_CIPHER_CKIP,
	WMI_CIPHER_AES_CMAC,
	WMI_CIPHER_AES_GCM,
};

enum wmi_tlv_cipher_suites {
	WMI_TLV_CIPHER_NONE,
	WMI_TLV_CIPHER_WEP,
	WMI_TLV_CIPHER_TKIP,
	WMI_TLV_CIPHER_AES_OCB,
	WMI_TLV_CIPHER_AES_CCM,
	WMI_TLV_CIPHER_WAPI,
	WMI_TLV_CIPHER_CKIP,
	WMI_TLV_CIPHER_AES_CMAC,
	WMI_TLV_CIPHER_ANY,
	WMI_TLV_CIPHER_AES_GCM,
};

struct wmi_vdev_install_key_cmd {
	__le32 vdev_id;
	struct wmi_mac_addr peer_macaddr;
	__le32 key_idx;
	__le32 key_flags;
	__le32 key_cipher; /* %WMI_CIPHER_ */
	struct wmi_key_seq_counter key_rsc_counter;
	struct wmi_key_seq_counter key_global_rsc_counter;
	struct wmi_key_seq_counter key_tsc_counter;
	u8 wpi_key_rsc_counter[16];
	u8 wpi_key_tsc_counter[16];
	__le32 key_len;
	__le32 key_txmic_len;
	__le32 key_rxmic_len;

	/* contains key followed by tx mic followed by rx mic */
	u8 key_data[];
} __packed;

#define WMI_VDEV_DISABLE_4_ADDR_SRC_LRN 1
#define WMI_VDEV_SLOT_TIME_LONG		0x1
#define WMI_VDEV_SLOT_TIME_SHORT	0x2
#define WMI_VDEV_PREAMBLE_LONG		0x1
#define WMI_VDEV_PREAMBLE_SHORT		0x2

struct wmi_vdev_start_response_event {
	__le32 vdev_id;
	__le32 req_id;
	__le32 resp_type; /* %WMI_VDEV_RESP_ */
	__le32 status;
} __packed;

enum wmi_sta_ps_mode {
	/* enable power save for the given STA VDEV */
	WMI_STA_PS_MODE_DISABLED = 0,
	/* disable power save  for a given STA VDEV */
	WMI_STA_PS_MODE_ENABLED = 1,
};

struct wmi_sta_powersave_mode_cmd {
	/* unique id identifying the VDEV, generated by the caller */
	__le32 vdev_id;

	/*
	 * Power save mode
	 * (see enum wmi_sta_ps_mode)
	 */
	__le32 sta_ps_mode;
} __packed;

enum wmi_sta_ps_param_rx_wake_policy {
	/*
	 * Wake up when ever there is an  RX activity on the VDEV. In this mode
	 * the Power save SM(state machine) will come out of sleep by either
	 * sending null frame (or) a data frame (with PS==0) in response to TIM
	 * bit set in the received beacon frame from AP.
	 */
	WMI_STA_PS_RX_WAKE_POLICY_WAKE = 0,

	/*
	 * Here the power save state machine will not wakeup in response to TIM
	 * bit, instead it will send a PSPOLL (or) UASPD trigger based on UAPSD
	 * configuration setup by WMISET_PS_SET_UAPSD  WMI command.  When all
	 * access categories are delivery-enabled, the station will send a
	 * UAPSD trigger frame, otherwise it will send a PS-Poll.
	 */
	WMI_STA_PS_RX_WAKE_POLICY_POLL_UAPSD = 1,
};

enum wmi_sta_ps_param_tx_wake_threshold {
	WMI_STA_PS_TX_WAKE_THRESHOLD_NEVER = 0,
	WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS = 1,

	/*
	 * Values greater than one indicate that many TX attempts per beacon
	 * interval before the STA will wake up
	 */
};

enum wmi_sta_ps_param_pspoll_count {
	WMI_STA_PS_PSPOLL_COUNT_NO_MAX = 0,
	/*
	 * Values greater than 0 indicate the maximum number of PS-Poll frames
	 * FW will send before waking up.
	 */

	/* When u-APSD is enabled the firmware will be very reluctant to exit
	 * STA PS. This could result in very poor Rx performance with STA doing
	 * PS-Poll for each and every buffered frame. This value is a bit
	 * arbitrary.
	 */
	WMI_STA_PS_PSPOLL_COUNT_UAPSD = 3,
};

enum wmi_sta_powersave_param {
	/*
	 * Controls how frames are retrievd from AP while STA is sleeping
	 *
	 * (see enum wmi_sta_ps_param_rx_wake_policy)
	 */
	WMI_STA_PS_PARAM_RX_WAKE_POLICY = 0,

	/*
	 * The STA will go active after this many TX
	 *
	 * (see enum wmi_sta_ps_param_tx_wake_threshold)
	 */
	WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD = 1,

	/*
	 * Number of PS-Poll to send before STA wakes up
	 *
	 * (see enum wmi_sta_ps_param_pspoll_count)
	 *
	 */
	WMI_STA_PS_PARAM_PSPOLL_COUNT = 2,

	/*
	 * TX/RX inactivity time in msec before going to sleep.
	 *
	 * The power save SM will monitor tx/rx activity on the VDEV, if no
	 * activity for the specified msec of the parameter the Power save
	 * SM will go to sleep.
	 */
	WMI_STA_PS_PARAM_INACTIVITY_TIME = 3,

	/*
	 * Set uapsd configuration.
	 *
	 * (see enum wmi_sta_ps_param_uapsd)
	 */
	WMI_STA_PS_PARAM_UAPSD = 4,
};

struct wmi_sta_powersave_param_cmd {
	__le32 vdev_id;
	__le32 param_id; /* %WMI_STA_PS_PARAM_ */
	__le32 param_value;
} __packed;

enum wmi_peer_type {
	WMI_PEER_TYPE_DEFAULT = 0,
	WMI_PEER_TYPE_BSS = 1,
	WMI_PEER_TYPE_TDLS = 2,
};

struct wmi_peer_delete_cmd {
	__le32 vdev_id;
	struct wmi_mac_addr peer_macaddr;
} __packed;

enum wmi_peer_smps_state {
	WMI_PEER_SMPS_PS_NONE = 0x0,
	WMI_PEER_SMPS_STATIC  = 0x1,
	WMI_PEER_SMPS_DYNAMIC = 0x2
};

struct wmi_peer_set_param_cmd {
	__le32 vdev_id;
	struct wmi_mac_addr peer_macaddr;
	__le32 param_id;
	__le32 param_value;
} __packed;

#define MAX_SUPPORTED_RATES 128

struct wmi_vht_rate_set {
	__le32 rx_max_rate; /* Max Rx data rate */
	__le32 rx_mcs_set;  /* Negotiated RX VHT rates */
	__le32 tx_max_rate; /* Max Tx data rate */
	__le32 tx_mcs_set;  /* Negotiated TX VHT rates */
} __packed;

#define WMI_RC_DS_FLAG          0x01
#define WMI_RC_CW40_FLAG        0x02
#define WMI_RC_SGI_FLAG         0x04
#define WMI_RC_HT_FLAG          0x08
#define WMI_RC_RTSCTS_FLAG      0x10
#define WMI_RC_TX_STBC_FLAG     0x20
#define WMI_RC_RX_STBC_FLAG     0xC0
#define WMI_RC_RX_STBC_FLAG_S   6
#define WMI_RC_WEP_TKIP_FLAG    0x100
#define WMI_RC_TS_FLAG          0x200
#define WMI_RC_UAPSD_FLAG       0x400

struct wmi_peer_sta_kickout_event {
	struct wmi_mac_addr peer_macaddr;
} __packed;

enum wmi_sta_keepalive_method {
	WMI_STA_KEEPALIVE_METHOD_NULL_FRAME = 1,
	WMI_STA_KEEPALIVE_METHOD_UNSOLICITATED_ARP_RESPONSE = 2,
};

#define WMI_STA_KEEPALIVE_INTERVAL_DISABLE 0

struct wmi_sta_keepalive_arp_resp {
	__be32 src_ip4_addr;
	__be32 dest_ip4_addr;
	struct wmi_mac_addr dest_mac_addr;
} __packed;

enum wmi_roam_reason {
	WMI_ROAM_REASON_BETTER_AP = 1,
	WMI_ROAM_REASON_BEACON_MISS = 2,
	WMI_ROAM_REASON_LOW_RSSI = 3,
	WMI_ROAM_REASON_SUITABLE_AP_FOUND = 4,
	WMI_ROAM_REASON_HO_FAILED = 5,

	/* keep last */
	WMI_ROAM_REASON_MAX,
};

enum wmi_vdev_start_status {
	WMI_VDEV_START_OK = 0,
	WMI_VDEV_START_CHAN_INVALID,
};

struct wmi_mgmt_tx_hdr {
	__le32 vdev_id;
	struct wmi_mac_addr peer_macaddr;
	__le32 tx_rate;
	__le32 tx_power;
	__le32 buf_len;
} __packed;

struct wmi_echo_event {
	__le32 value;
} __packed;

struct wmi_echo_cmd {
	__le32 value;
} __packed;


/* The wireless module interface in its TLV form (wmi-tlv.h) */

#define WMI_TLV_CMD(grp_id) (((grp_id) << 12) | 0x1)
#define WMI_TLV_EV(grp_id) (((grp_id) << 12) | 0x1)
#define WMI_TLV_CMD_UNSUPPORTED 0

enum wmi_tlv_grp_id {
	WMI_TLV_GRP_START = 0x3,
	WMI_TLV_GRP_SCAN = WMI_TLV_GRP_START,
	WMI_TLV_GRP_PDEV,
	WMI_TLV_GRP_VDEV,
	WMI_TLV_GRP_PEER,
	WMI_TLV_GRP_MGMT,
	WMI_TLV_GRP_BA_NEG,
	WMI_TLV_GRP_STA_PS,
	WMI_TLV_GRP_DFS,
	WMI_TLV_GRP_ROAM,
	WMI_TLV_GRP_OFL_SCAN,
	WMI_TLV_GRP_P2P,
	WMI_TLV_GRP_AP_PS,
	WMI_TLV_GRP_RATECTL,
	WMI_TLV_GRP_PROFILE,
	WMI_TLV_GRP_SUSPEND,
	WMI_TLV_GRP_BCN_FILTER,
	WMI_TLV_GRP_WOW,
	WMI_TLV_GRP_RTT,
	WMI_TLV_GRP_SPECTRAL,
	WMI_TLV_GRP_STATS,
	WMI_TLV_GRP_ARP_NS_OFL,
	WMI_TLV_GRP_NLO_OFL,
	WMI_TLV_GRP_GTK_OFL,
	WMI_TLV_GRP_CSA_OFL,
	WMI_TLV_GRP_CHATTER,
	WMI_TLV_GRP_TID_ADDBA,
	WMI_TLV_GRP_MISC,
	WMI_TLV_GRP_GPIO,
	WMI_TLV_GRP_FWTEST,
	WMI_TLV_GRP_TDLS,
	WMI_TLV_GRP_RESMGR,
	WMI_TLV_GRP_STA_SMPS,
	WMI_TLV_GRP_WLAN_HB,
	WMI_TLV_GRP_RMC,
	WMI_TLV_GRP_MHF_OFL,
	WMI_TLV_GRP_LOCATION_SCAN,
	WMI_TLV_GRP_OEM,
	WMI_TLV_GRP_NAN,
	WMI_TLV_GRP_COEX,
	WMI_TLV_GRP_OBSS_OFL,
	WMI_TLV_GRP_LPI,
	WMI_TLV_GRP_EXTSCAN,
	WMI_TLV_GRP_DHCP_OFL,
	WMI_TLV_GRP_IPA,
	WMI_TLV_GRP_MDNS_OFL,
	WMI_TLV_GRP_SAP_OFL,
};

enum wmi_tlv_cmd_id {
	WMI_TLV_INIT_CMDID = 0x1,
	WMI_TLV_START_SCAN_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_SCAN),
	WMI_TLV_STOP_SCAN_CMDID,
	WMI_TLV_SCAN_CHAN_LIST_CMDID,
	WMI_TLV_SCAN_SCH_PRIO_TBL_CMDID,
	WMI_TLV_SCAN_UPDATE_REQUEST_CMDID,
	WMI_TLV_SCAN_PROB_REQ_OUI_CMDID,
	WMI_TLV_PDEV_SET_REGDOMAIN_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_PDEV),
	WMI_TLV_PDEV_SET_CHANNEL_CMDID,
	WMI_TLV_PDEV_SET_PARAM_CMDID,
	WMI_TLV_PDEV_PKTLOG_ENABLE_CMDID,
	WMI_TLV_PDEV_PKTLOG_DISABLE_CMDID,
	WMI_TLV_PDEV_SET_WMM_PARAMS_CMDID,
	WMI_TLV_PDEV_SET_HT_CAP_IE_CMDID,
	WMI_TLV_PDEV_SET_VHT_CAP_IE_CMDID,
	WMI_TLV_PDEV_SET_DSCP_TID_MAP_CMDID,
	WMI_TLV_PDEV_SET_QUIET_MODE_CMDID,
	WMI_TLV_PDEV_GREEN_AP_PS_ENABLE_CMDID,
	WMI_TLV_PDEV_GET_TPC_CONFIG_CMDID,
	WMI_TLV_PDEV_SET_BASE_MACADDR_CMDID,
	WMI_TLV_PDEV_DUMP_CMDID,
	WMI_TLV_PDEV_SET_LED_CONFIG_CMDID,
	WMI_TLV_PDEV_GET_TEMPERATURE_CMDID,
	WMI_TLV_PDEV_SET_LED_FLASHING_CMDID,
	WMI_TLV_VDEV_CREATE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_VDEV),
	WMI_TLV_VDEV_DELETE_CMDID,
	WMI_TLV_VDEV_START_REQUEST_CMDID,
	WMI_TLV_VDEV_RESTART_REQUEST_CMDID,
	WMI_TLV_VDEV_UP_CMDID,
	WMI_TLV_VDEV_STOP_CMDID,
	WMI_TLV_VDEV_DOWN_CMDID,
	WMI_TLV_VDEV_SET_PARAM_CMDID,
	WMI_TLV_VDEV_INSTALL_KEY_CMDID,
	WMI_TLV_VDEV_WNM_SLEEPMODE_CMDID,
	WMI_TLV_VDEV_WMM_ADDTS_CMDID,
	WMI_TLV_VDEV_WMM_DELTS_CMDID,
	WMI_TLV_VDEV_SET_WMM_PARAMS_CMDID,
	WMI_TLV_VDEV_SET_GTX_PARAMS_CMDID,
	WMI_TLV_VDEV_IPSEC_NATKEEPALIVE_FILTER_CMDID,
	WMI_TLV_VDEV_PLMREQ_START_CMDID,
	WMI_TLV_VDEV_PLMREQ_STOP_CMDID,
	WMI_TLV_PEER_CREATE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_PEER),
	WMI_TLV_PEER_DELETE_CMDID,
	WMI_TLV_PEER_FLUSH_TIDS_CMDID,
	WMI_TLV_PEER_SET_PARAM_CMDID,
	WMI_TLV_PEER_ASSOC_CMDID,
	WMI_TLV_PEER_ADD_WDS_ENTRY_CMDID,
	WMI_TLV_PEER_REMOVE_WDS_ENTRY_CMDID,
	WMI_TLV_PEER_MCAST_GROUP_CMDID,
	WMI_TLV_PEER_INFO_REQ_CMDID,
	WMI_TLV_PEER_GET_ESTIMATED_LINKSPEED_CMDID,
	WMI_TLV_BCN_TX_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_MGMT),
	WMI_TLV_PDEV_SEND_BCN_CMDID,
	WMI_TLV_BCN_TMPL_CMDID,
	WMI_TLV_BCN_FILTER_RX_CMDID,
	WMI_TLV_PRB_REQ_FILTER_RX_CMDID,
	WMI_TLV_MGMT_TX_CMDID,
	WMI_TLV_PRB_TMPL_CMDID,
	WMI_TLV_MGMT_TX_SEND_CMD,
	WMI_TLV_ADDBA_CLEAR_RESP_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_BA_NEG),
	WMI_TLV_ADDBA_SEND_CMDID,
	WMI_TLV_ADDBA_STATUS_CMDID,
	WMI_TLV_DELBA_SEND_CMDID,
	WMI_TLV_ADDBA_SET_RESP_CMDID,
	WMI_TLV_SEND_SINGLEAMSDU_CMDID,
	WMI_TLV_STA_POWERSAVE_MODE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_STA_PS),
	WMI_TLV_STA_POWERSAVE_PARAM_CMDID,
	WMI_TLV_STA_MIMO_PS_MODE_CMDID,
	WMI_TLV_PDEV_DFS_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_DFS),
	WMI_TLV_PDEV_DFS_DISABLE_CMDID,
	WMI_TLV_DFS_PHYERR_FILTER_ENA_CMDID,
	WMI_TLV_DFS_PHYERR_FILTER_DIS_CMDID,
	WMI_TLV_ROAM_SCAN_MODE = WMI_TLV_CMD(WMI_TLV_GRP_ROAM),
	WMI_TLV_ROAM_SCAN_RSSI_THRESHOLD,
	WMI_TLV_ROAM_SCAN_PERIOD,
	WMI_TLV_ROAM_SCAN_RSSI_CHANGE_THRESHOLD,
	WMI_TLV_ROAM_AP_PROFILE,
	WMI_TLV_ROAM_CHAN_LIST,
	WMI_TLV_ROAM_SCAN_CMD,
	WMI_TLV_ROAM_SYNCH_COMPLETE,
	WMI_TLV_ROAM_SET_RIC_REQUEST_CMDID,
	WMI_TLV_ROAM_INVOKE_CMDID,
	WMI_TLV_OFL_SCAN_ADD_AP_PROFILE = WMI_TLV_CMD(WMI_TLV_GRP_OFL_SCAN),
	WMI_TLV_OFL_SCAN_REMOVE_AP_PROFILE,
	WMI_TLV_OFL_SCAN_PERIOD,
	WMI_TLV_P2P_DEV_SET_DEVICE_INFO = WMI_TLV_CMD(WMI_TLV_GRP_P2P),
	WMI_TLV_P2P_DEV_SET_DISCOVERABILITY,
	WMI_TLV_P2P_GO_SET_BEACON_IE,
	WMI_TLV_P2P_GO_SET_PROBE_RESP_IE,
	WMI_TLV_P2P_SET_VENDOR_IE_DATA_CMDID,
	WMI_TLV_P2P_DISC_OFFLOAD_CONFIG_CMDID,
	WMI_TLV_P2P_DISC_OFFLOAD_APPIE_CMDID,
	WMI_TLV_P2P_DISC_OFFLOAD_PATTERN_CMDID,
	WMI_TLV_P2P_SET_OPPPS_PARAM_CMDID,
	WMI_TLV_AP_PS_PEER_PARAM_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_AP_PS),
	WMI_TLV_AP_PS_PEER_UAPSD_COEX_CMDID,
	WMI_TLV_PEER_RATE_RETRY_SCHED_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_RATECTL),
	WMI_TLV_WLAN_PROFILE_TRIGGER_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_PROFILE),
	WMI_TLV_WLAN_PROFILE_SET_HIST_INTVL_CMDID,
	WMI_TLV_WLAN_PROFILE_GET_PROFILE_DATA_CMDID,
	WMI_TLV_WLAN_PROFILE_ENABLE_PROFILE_ID_CMDID,
	WMI_TLV_WLAN_PROFILE_LIST_PROFILE_ID_CMDID,
	WMI_TLV_PDEV_SUSPEND_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_SUSPEND),
	WMI_TLV_PDEV_RESUME_CMDID,
	WMI_TLV_ADD_BCN_FILTER_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_BCN_FILTER),
	WMI_TLV_RMV_BCN_FILTER_CMDID,
	WMI_TLV_WOW_ADD_WAKE_PATTERN_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_WOW),
	WMI_TLV_WOW_DEL_WAKE_PATTERN_CMDID,
	WMI_TLV_WOW_ENABLE_DISABLE_WAKE_EVENT_CMDID,
	WMI_TLV_WOW_ENABLE_CMDID,
	WMI_TLV_WOW_HOSTWAKEUP_FROM_SLEEP_CMDID,
	WMI_TLV_WOW_ACER_IOAC_ADD_KEEPALIVE_CMDID,
	WMI_TLV_WOW_ACER_IOAC_DEL_KEEPALIVE_CMDID,
	WMI_TLV_WOW_ACER_IOAC_ADD_WAKE_PATTERN_CMDID,
	WMI_TLV_WOW_ACER_IOAC_DEL_WAKE_PATTERN_CMDID,
	WMI_TLV_D0_WOW_ENABLE_DISABLE_CMDID,
	WMI_TLV_EXTWOW_ENABLE_CMDID,
	WMI_TLV_EXTWOW_SET_APP_TYPE1_PARAMS_CMDID,
	WMI_TLV_EXTWOW_SET_APP_TYPE2_PARAMS_CMDID,
	WMI_TLV_RTT_MEASREQ_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_RTT),
	WMI_TLV_RTT_TSF_CMDID,
	WMI_TLV_SPECTRAL_SCAN_CONF_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_SPECTRAL),
	WMI_TLV_SPECTRAL_SCAN_ENABLE_CMDID,
	WMI_TLV_REQUEST_STATS_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_STATS),
	WMI_TLV_MCC_SCHED_TRAFFIC_STATS_CMDID,
	WMI_TLV_REQUEST_STATS_EXT_CMDID,
	WMI_TLV_REQUEST_LINK_STATS_CMDID,
	WMI_TLV_START_LINK_STATS_CMDID,
	WMI_TLV_CLEAR_LINK_STATS_CMDID,
	WMI_TLV_CGET_FW_MEM_DUMP_CMDID,
	WMI_TLV_CDEBUG_MESG_FLUSH_CMDID,
	WMI_TLV_CDIAG_EVENT_LOG_CONFIG_CMDID,
	WMI_TLV_CREQUEST_WLAN_STATS_CMDID,
	WMI_TLV_CREQUEST_RCPI_CMDID,
	WMI_TLV_REQUEST_PEER_STATS_INFO_CMDID,
	WMI_TLV_SET_ARP_NS_OFFLOAD_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_ARP_NS_OFL),
	WMI_TLV_ADD_PROACTIVE_ARP_RSP_PATTERN_CMDID,
	WMI_TLV_DEL_PROACTIVE_ARP_RSP_PATTERN_CMDID,
	WMI_TLV_NETWORK_LIST_OFFLOAD_CONFIG_CMDID =
			WMI_TLV_CMD(WMI_TLV_GRP_NLO_OFL),
	WMI_TLV_APFIND_CMDID,
	WMI_TLV_GTK_OFFLOAD_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_GTK_OFL),
	WMI_TLV_CSA_OFFLOAD_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_CSA_OFL),
	WMI_TLV_CSA_OFFLOAD_CHANSWITCH_CMDID,
	WMI_TLV_CHATTER_SET_MODE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_CHATTER),
	WMI_TLV_CHATTER_ADD_COALESCING_FILTER_CMDID,
	WMI_TLV_CHATTER_DELETE_COALESCING_FILTER_CMDID,
	WMI_TLV_CHATTER_COALESCING_QUERY_CMDID,
	WMI_TLV_PEER_TID_ADDBA_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_TID_ADDBA),
	WMI_TLV_PEER_TID_DELBA_CMDID,
	WMI_TLV_STA_DTIM_PS_METHOD_CMDID,
	WMI_TLV_STA_UAPSD_AUTO_TRIG_CMDID,
	WMI_TLV_STA_KEEPALIVE_CMDID,
	WMI_TLV_BA_REQ_SSN_CMDID,
	WMI_TLV_ECHO_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_MISC),
	WMI_TLV_PDEV_UTF_CMDID,
	WMI_TLV_DBGLOG_CFG_CMDID,
	WMI_TLV_PDEV_QVIT_CMDID,
	WMI_TLV_PDEV_FTM_INTG_CMDID,
	WMI_TLV_VDEV_SET_KEEPALIVE_CMDID,
	WMI_TLV_VDEV_GET_KEEPALIVE_CMDID,
	WMI_TLV_FORCE_FW_HANG_CMDID,
	WMI_TLV_SET_MCASTBCAST_FILTER_CMDID,
	WMI_TLV_THERMAL_MGMT_CMDID,
	WMI_TLV_HOST_AUTO_SHUTDOWN_CFG_CMDID,
	WMI_TLV_TPC_CHAINMASK_CONFIG_CMDID,
	WMI_TLV_SET_ANTENNA_DIVERSITY_CMDID,
	WMI_TLV_GPIO_CONFIG_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_GPIO),
	WMI_TLV_GPIO_OUTPUT_CMDID,
	WMI_TLV_TXBF_CMDID,
	WMI_TLV_FWTEST_VDEV_MCC_SET_TBTT_MODE_CMDID =
			WMI_TLV_CMD(WMI_TLV_GRP_FWTEST),
	WMI_TLV_FWTEST_P2P_SET_NOA_PARAM_CMDID,
	WMI_TLV_UNIT_TEST_CMDID,
	WMI_TLV_TDLS_SET_STATE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_TDLS),
	WMI_TLV_TDLS_PEER_UPDATE_CMDID,
	WMI_TLV_TDLS_SET_OFFCHAN_MODE_CMDID,
	WMI_TLV_RESMGR_ADAPTIVE_OCS_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_RESMGR),
	WMI_TLV_RESMGR_SET_CHAN_TIME_QUOTA_CMDID,
	WMI_TLV_RESMGR_SET_CHAN_LATENCY_CMDID,
	WMI_TLV_STA_SMPS_FORCE_MODE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_STA_SMPS),
	WMI_TLV_STA_SMPS_PARAM_CMDID,
	WMI_TLV_HB_SET_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_WLAN_HB),
	WMI_TLV_HB_SET_TCP_PARAMS_CMDID,
	WMI_TLV_HB_SET_TCP_PKT_FILTER_CMDID,
	WMI_TLV_HB_SET_UDP_PARAMS_CMDID,
	WMI_TLV_HB_SET_UDP_PKT_FILTER_CMDID,
	WMI_TLV_RMC_SET_MODE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_RMC),
	WMI_TLV_RMC_SET_ACTION_PERIOD_CMDID,
	WMI_TLV_RMC_CONFIG_CMDID,
	WMI_TLV_MHF_OFFLOAD_SET_MODE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_MHF_OFL),
	WMI_TLV_MHF_OFFLOAD_PLUMB_ROUTING_TBL_CMDID,
	WMI_TLV_BATCH_SCAN_ENABLE_CMDID =
			WMI_TLV_CMD(WMI_TLV_GRP_LOCATION_SCAN),
	WMI_TLV_BATCH_SCAN_DISABLE_CMDID,
	WMI_TLV_BATCH_SCAN_TRIGGER_RESULT_CMDID,
	WMI_TLV_OEM_REQ_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_OEM),
	WMI_TLV_NAN_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_NAN),
	WMI_TLV_MODEM_POWER_STATE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_COEX),
	WMI_TLV_CHAN_AVOID_UPDATE_CMDID,
	WMI_TLV_OBSS_SCAN_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_OBSS_OFL),
	WMI_TLV_OBSS_SCAN_DISABLE_CMDID,
	WMI_TLV_LPI_MGMT_SNOOPING_CONFIG_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_LPI),
	WMI_TLV_LPI_START_SCAN_CMDID,
	WMI_TLV_LPI_STOP_SCAN_CMDID,
	WMI_TLV_EXTSCAN_START_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_EXTSCAN),
	WMI_TLV_EXTSCAN_STOP_CMDID,
	WMI_TLV_EXTSCAN_CONFIGURE_WLAN_CHANGE_MONITOR_CMDID,
	WMI_TLV_EXTSCAN_CONFIGURE_HOTLIST_MONITOR_CMDID,
	WMI_TLV_EXTSCAN_GET_CACHED_RESULTS_CMDID,
	WMI_TLV_EXTSCAN_GET_WLAN_CHANGE_RESULTS_CMDID,
	WMI_TLV_EXTSCAN_SET_CAPABILITIES_CMDID,
	WMI_TLV_EXTSCAN_GET_CAPABILITIES_CMDID,
	WMI_TLV_SET_DHCP_SERVER_OFFLOAD_CMDID =
			WMI_TLV_CMD(WMI_TLV_GRP_DHCP_OFL),
	WMI_TLV_IPA_OFFLOAD_ENABLE_DISABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_IPA),
	WMI_TLV_MDNS_OFFLOAD_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_MDNS_OFL),
	WMI_TLV_MDNS_SET_FQDN_CMDID,
	WMI_TLV_MDNS_SET_RESPONSE_CMDID,
	WMI_TLV_MDNS_GET_STATS_CMDID,
	WMI_TLV_SAP_OFL_ENABLE_CMDID = WMI_TLV_CMD(WMI_TLV_GRP_SAP_OFL),
};

enum wmi_tlv_event_id {
	WMI_TLV_SERVICE_READY_EVENTID = 0x1,
	WMI_TLV_READY_EVENTID,
	WMI_TLV_SERVICE_AVAILABLE_EVENTID,
	WMI_TLV_SCAN_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_SCAN),
	WMI_TLV_PDEV_TPC_CONFIG_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_PDEV),
	WMI_TLV_CHAN_INFO_EVENTID,
	WMI_TLV_PHYERR_EVENTID,
	WMI_TLV_PDEV_DUMP_EVENTID,
	WMI_TLV_TX_PAUSE_EVENTID,
	WMI_TLV_DFS_RADAR_EVENTID,
	WMI_TLV_PDEV_L1SS_TRACK_EVENTID,
	WMI_TLV_PDEV_TEMPERATURE_EVENTID,
	WMI_TLV_VDEV_START_RESP_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_VDEV),
	WMI_TLV_VDEV_STOPPED_EVENTID,
	WMI_TLV_VDEV_INSTALL_KEY_COMPLETE_EVENTID,
	WMI_TLV_VDEV_MCC_BCN_INTERVAL_CHANGE_REQ_EVENTID,
	WMI_TLV_VDEV_TSF_REPORT_EVENTID,
	WMI_TLV_VDEV_DELETE_RESP_EVENTID,
	WMI_TLV_PEER_STA_KICKOUT_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_PEER),
	WMI_TLV_PEER_INFO_EVENTID,
	WMI_TLV_PEER_TX_FAIL_CNT_THR_EVENTID,
	WMI_TLV_PEER_ESTIMATED_LINKSPEED_EVENTID,
	WMI_TLV_PEER_STATE_EVENTID,
	WMI_TLV_PEER_ASSOC_CONF_EVENTID,
	WMI_TLV_PEER_DELETE_RESP_EVENTID,
	WMI_TLV_MGMT_RX_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_MGMT),
	WMI_TLV_HOST_SWBA_EVENTID,
	WMI_TLV_TBTTOFFSET_UPDATE_EVENTID,
	WMI_TLV_OFFLOAD_BCN_TX_STATUS_EVENTID,
	WMI_TLV_OFFLOAD_PROB_RESP_TX_STATUS_EVENTID,
	WMI_TLV_MGMT_TX_COMPLETION_EVENTID,
	WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID,
	WMI_TLV_TX_DELBA_COMPLETE_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_BA_NEG),
	WMI_TLV_TX_ADDBA_COMPLETE_EVENTID,
	WMI_TLV_BA_RSP_SSN_EVENTID,
	WMI_TLV_AGGR_STATE_TRIG_EVENTID,
	WMI_TLV_ROAM_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_ROAM),
	WMI_TLV_PROFILE_MATCH,
	WMI_TLV_ROAM_SYNCH_EVENTID,
	WMI_TLV_P2P_DISC_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_P2P),
	WMI_TLV_P2P_NOA_EVENTID,
	WMI_TLV_PDEV_RESUME_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_SUSPEND),
	WMI_TLV_WOW_WAKEUP_HOST_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_WOW),
	WMI_TLV_D0_WOW_DISABLE_ACK_EVENTID,
	WMI_TLV_RTT_MEASUREMENT_REPORT_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_RTT),
	WMI_TLV_TSF_MEASUREMENT_REPORT_EVENTID,
	WMI_TLV_RTT_ERROR_REPORT_EVENTID,
	WMI_TLV_STATS_EXT_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_STATS),
	WMI_TLV_IFACE_LINK_STATS_EVENTID,
	WMI_TLV_PEER_LINK_STATS_EVENTID,
	WMI_TLV_RADIO_LINK_STATS_EVENTID,
	WMI_TLV_UPDATE_FW_MEM_DUMP_EVENTID,
	WMI_TLV_DIAG_EVENT_LOG_SUPPORTED_EVENTID,
	WMI_TLV_INST_RSSI_STATS_EVENTID,
	WMI_TLV_RADIO_TX_POWER_LEVEL_STATS_EVENTID,
	WMI_TLV_REPORT_STATS_EVENTID,
	WMI_TLV_UPDATE_RCPI_EVENTID,
	WMI_TLV_PEER_STATS_INFO_EVENTID,
	WMI_TLV_NLO_MATCH_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_NLO_OFL),
	WMI_TLV_NLO_SCAN_COMPLETE_EVENTID,
	WMI_TLV_APFIND_EVENTID,
	WMI_TLV_GTK_OFFLOAD_STATUS_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_GTK_OFL),
	WMI_TLV_GTK_REKEY_FAIL_EVENTID,
	WMI_TLV_CSA_HANDLING_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_CSA_OFL),
	WMI_TLV_CHATTER_PC_QUERY_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_CHATTER),
	WMI_TLV_ECHO_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_MISC),
	WMI_TLV_PDEV_UTF_EVENTID,
	WMI_TLV_DEBUG_MESG_EVENTID,
	WMI_TLV_UPDATE_STATS_EVENTID,
	WMI_TLV_DEBUG_PRINT_EVENTID,
	WMI_TLV_DCS_INTERFERENCE_EVENTID,
	WMI_TLV_PDEV_QVIT_EVENTID,
	WMI_TLV_WLAN_PROFILE_DATA_EVENTID,
	WMI_TLV_PDEV_FTM_INTG_EVENTID,
	WMI_TLV_WLAN_FREQ_AVOID_EVENTID,
	WMI_TLV_VDEV_GET_KEEPALIVE_EVENTID,
	WMI_TLV_THERMAL_MGMT_EVENTID,
	WMI_TLV_DIAG_DATA_CONTAINER_EVENTID,
	WMI_TLV_HOST_AUTO_SHUTDOWN_EVENTID,
	WMI_TLV_UPDATE_WHAL_MIB_STATS_EVENTID,
	WMI_TLV_UPDATE_VDEV_RATE_STATS_EVENTID,
	WMI_TLV_DIAG_EVENTID,
	WMI_TLV_GPIO_INPUT_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_GPIO),
	WMI_TLV_UPLOADH_EVENTID,
	WMI_TLV_CAPTUREH_EVENTID,
	WMI_TLV_RFKILL_STATE_CHANGE_EVENTID,
	WMI_TLV_TDLS_PEER_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_TDLS),
	WMI_TLV_BATCH_SCAN_ENABLED_EVENTID =
			WMI_TLV_EV(WMI_TLV_GRP_LOCATION_SCAN),
	WMI_TLV_BATCH_SCAN_RESULT_EVENTID,
	WMI_TLV_OEM_CAPABILITY_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_OEM),
	WMI_TLV_OEM_MEASUREMENT_REPORT_EVENTID,
	WMI_TLV_OEM_ERROR_REPORT_EVENTID,
	WMI_TLV_NAN_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_NAN),
	WMI_TLV_LPI_RESULT_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_LPI),
	WMI_TLV_LPI_STATUS_EVENTID,
	WMI_TLV_LPI_HANDOFF_EVENTID,
	WMI_TLV_EXTSCAN_START_STOP_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_EXTSCAN),
	WMI_TLV_EXTSCAN_OPERATION_EVENTID,
	WMI_TLV_EXTSCAN_TABLE_USAGE_EVENTID,
	WMI_TLV_EXTSCAN_CACHED_RESULTS_EVENTID,
	WMI_TLV_EXTSCAN_WLAN_CHANGE_RESULTS_EVENTID,
	WMI_TLV_EXTSCAN_HOTLIST_MATCH_EVENTID,
	WMI_TLV_EXTSCAN_CAPABILITIES_EVENTID,
	WMI_TLV_MDNS_STATS_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_MDNS_OFL),
	WMI_TLV_SAP_OFL_ADD_STA_EVENTID = WMI_TLV_EV(WMI_TLV_GRP_SAP_OFL),
	WMI_TLV_SAP_OFL_DEL_STA_EVENTID,
};

enum wmi_tlv_pdev_param {
	WMI_TLV_PDEV_PARAM_TX_CHAIN_MASK = 0x1,
	WMI_TLV_PDEV_PARAM_RX_CHAIN_MASK,
	WMI_TLV_PDEV_PARAM_TXPOWER_LIMIT2G,
	WMI_TLV_PDEV_PARAM_TXPOWER_LIMIT5G,
	WMI_TLV_PDEV_PARAM_TXPOWER_SCALE,
	WMI_TLV_PDEV_PARAM_BEACON_GEN_MODE,
	WMI_TLV_PDEV_PARAM_BEACON_TX_MODE,
	WMI_TLV_PDEV_PARAM_RESMGR_OFFCHAN_MODE,
	WMI_TLV_PDEV_PARAM_PROTECTION_MODE,
	WMI_TLV_PDEV_PARAM_DYNAMIC_BW,
	WMI_TLV_PDEV_PARAM_NON_AGG_SW_RETRY_TH,
	WMI_TLV_PDEV_PARAM_AGG_SW_RETRY_TH,
	WMI_TLV_PDEV_PARAM_STA_KICKOUT_TH,
	WMI_TLV_PDEV_PARAM_AC_AGGRSIZE_SCALING,
	WMI_TLV_PDEV_PARAM_LTR_ENABLE,
	WMI_TLV_PDEV_PARAM_LTR_AC_LATENCY_BE,
	WMI_TLV_PDEV_PARAM_LTR_AC_LATENCY_BK,
	WMI_TLV_PDEV_PARAM_LTR_AC_LATENCY_VI,
	WMI_TLV_PDEV_PARAM_LTR_AC_LATENCY_VO,
	WMI_TLV_PDEV_PARAM_LTR_AC_LATENCY_TIMEOUT,
	WMI_TLV_PDEV_PARAM_LTR_SLEEP_OVERRIDE,
	WMI_TLV_PDEV_PARAM_LTR_RX_OVERRIDE,
	WMI_TLV_PDEV_PARAM_LTR_TX_ACTIVITY_TIMEOUT,
	WMI_TLV_PDEV_PARAM_L1SS_ENABLE,
	WMI_TLV_PDEV_PARAM_DSLEEP_ENABLE,
	WMI_TLV_PDEV_PARAM_PCIELP_TXBUF_FLUSH,
	WMI_TLV_PDEV_PARAM_PCIELP_TXBUF_WATERMARK,
	WMI_TLV_PDEV_PARAM_PCIELP_TXBUF_TMO_EN,
	WMI_TLV_PDEV_PARAM_PCIELP_TXBUF_TMO_VALUE,
	WMI_TLV_PDEV_PARAM_PDEV_STATS_UPDATE_PERIOD,
	WMI_TLV_PDEV_PARAM_VDEV_STATS_UPDATE_PERIOD,
	WMI_TLV_PDEV_PARAM_PEER_STATS_UPDATE_PERIOD,
	WMI_TLV_PDEV_PARAM_BCNFLT_STATS_UPDATE_PERIOD,
	WMI_TLV_PDEV_PARAM_PMF_QOS,
	WMI_TLV_PDEV_PARAM_ARP_AC_OVERRIDE,
	WMI_TLV_PDEV_PARAM_DCS,
	WMI_TLV_PDEV_PARAM_ANI_ENABLE,
	WMI_TLV_PDEV_PARAM_ANI_POLL_PERIOD,
	WMI_TLV_PDEV_PARAM_ANI_LISTEN_PERIOD,
	WMI_TLV_PDEV_PARAM_ANI_OFDM_LEVEL,
	WMI_TLV_PDEV_PARAM_ANI_CCK_LEVEL,
	WMI_TLV_PDEV_PARAM_DYNTXCHAIN,
	WMI_TLV_PDEV_PARAM_PROXY_STA,
	WMI_TLV_PDEV_PARAM_IDLE_PS_CONFIG,
	WMI_TLV_PDEV_PARAM_POWER_GATING_SLEEP,
	WMI_TLV_PDEV_PARAM_RFKILL_ENABLE,
	WMI_TLV_PDEV_PARAM_BURST_DUR,
	WMI_TLV_PDEV_PARAM_BURST_ENABLE,
	WMI_TLV_PDEV_PARAM_HW_RFKILL_CONFIG,
	WMI_TLV_PDEV_PARAM_LOW_POWER_RF_ENABLE,
	WMI_TLV_PDEV_PARAM_L1SS_TRACK,
	WMI_TLV_PDEV_PARAM_HYST_EN,
	WMI_TLV_PDEV_PARAM_POWER_COLLAPSE_ENABLE,
	WMI_TLV_PDEV_PARAM_LED_SYS_STATE,
	WMI_TLV_PDEV_PARAM_LED_ENABLE,
	WMI_TLV_PDEV_PARAM_AUDIO_OVER_WLAN_LATENCY,
	WMI_TLV_PDEV_PARAM_AUDIO_OVER_WLAN_ENABLE,
	WMI_TLV_PDEV_PARAM_WHAL_MIB_STATS_UPDATE_ENABLE,
	WMI_TLV_PDEV_PARAM_VDEV_RATE_STATS_UPDATE_PERIOD,
	WMI_TLV_PDEV_PARAM_TXPOWER_REASON_NONE,
	WMI_TLV_PDEV_PARAM_TXPOWER_REASON_SAR,
	WMI_TLV_PDEV_PARAM_PEER_STATS_INFO_ENABLE = 0x8b,
	WMI_TLV_PDEV_PARAM_TXPOWER_REASON_MAX,
};

enum wmi_tlv_vdev_param {
	WMI_TLV_VDEV_PARAM_RTS_THRESHOLD = 0x1,
	WMI_TLV_VDEV_PARAM_FRAGMENTATION_THRESHOLD,
	WMI_TLV_VDEV_PARAM_BEACON_INTERVAL,
	WMI_TLV_VDEV_PARAM_LISTEN_INTERVAL,
	WMI_TLV_VDEV_PARAM_MULTICAST_RATE,
	WMI_TLV_VDEV_PARAM_MGMT_TX_RATE,
	WMI_TLV_VDEV_PARAM_SLOT_TIME,
	WMI_TLV_VDEV_PARAM_PREAMBLE,
	WMI_TLV_VDEV_PARAM_SWBA_TIME,
	WMI_TLV_VDEV_STATS_UPDATE_PERIOD,
	WMI_TLV_VDEV_PWRSAVE_AGEOUT_TIME,
	WMI_TLV_VDEV_HOST_SWBA_INTERVAL,
	WMI_TLV_VDEV_PARAM_DTIM_PERIOD,
	WMI_TLV_VDEV_OC_SCHEDULER_AIR_TIME_LIMIT,
	WMI_TLV_VDEV_PARAM_WDS,
	WMI_TLV_VDEV_PARAM_ATIM_WINDOW,
	WMI_TLV_VDEV_PARAM_BMISS_COUNT_MAX,
	WMI_TLV_VDEV_PARAM_BMISS_FIRST_BCNT,
	WMI_TLV_VDEV_PARAM_BMISS_FINAL_BCNT,
	WMI_TLV_VDEV_PARAM_FEATURE_WMM,
	WMI_TLV_VDEV_PARAM_CHWIDTH,
	WMI_TLV_VDEV_PARAM_CHEXTOFFSET,
	WMI_TLV_VDEV_PARAM_DISABLE_HTPROTECTION,
	WMI_TLV_VDEV_PARAM_STA_QUICKKICKOUT,
	WMI_TLV_VDEV_PARAM_MGMT_RATE,
	WMI_TLV_VDEV_PARAM_PROTECTION_MODE,
	WMI_TLV_VDEV_PARAM_FIXED_RATE,
	WMI_TLV_VDEV_PARAM_SGI,
	WMI_TLV_VDEV_PARAM_LDPC,
	WMI_TLV_VDEV_PARAM_TX_STBC,
	WMI_TLV_VDEV_PARAM_RX_STBC,
	WMI_TLV_VDEV_PARAM_INTRA_BSS_FWD,
	WMI_TLV_VDEV_PARAM_DEF_KEYID,
	WMI_TLV_VDEV_PARAM_NSS,
	WMI_TLV_VDEV_PARAM_BCAST_DATA_RATE,
	WMI_TLV_VDEV_PARAM_MCAST_DATA_RATE,
	WMI_TLV_VDEV_PARAM_MCAST_INDICATE,
	WMI_TLV_VDEV_PARAM_DHCP_INDICATE,
	WMI_TLV_VDEV_PARAM_UNKNOWN_DEST_INDICATE,
	WMI_TLV_VDEV_PARAM_AP_KEEPALIVE_MIN_IDLE_INACTIVE_TIME_SECS,
	WMI_TLV_VDEV_PARAM_AP_KEEPALIVE_MAX_IDLE_INACTIVE_TIME_SECS,
	WMI_TLV_VDEV_PARAM_AP_KEEPALIVE_MAX_UNRESPONSIVE_TIME_SECS,
	WMI_TLV_VDEV_PARAM_AP_ENABLE_NAWDS,
	WMI_TLV_VDEV_PARAM_ENABLE_RTSCTS,
	WMI_TLV_VDEV_PARAM_TXBF,
	WMI_TLV_VDEV_PARAM_PACKET_POWERSAVE,
	WMI_TLV_VDEV_PARAM_DROP_UNENCRY,
	WMI_TLV_VDEV_PARAM_TX_ENCAP_TYPE,
	WMI_TLV_VDEV_PARAM_AP_DETECT_OUT_OF_SYNC_SLEEPING_STA_TIME_SECS,
	WMI_TLV_VDEV_PARAM_EARLY_RX_ADJUST_ENABLE,
	WMI_TLV_VDEV_PARAM_EARLY_RX_TGT_BMISS_NUM,
	WMI_TLV_VDEV_PARAM_EARLY_RX_BMISS_SAMPLE_CYCLE,
	WMI_TLV_VDEV_PARAM_EARLY_RX_SLOP_STEP,
	WMI_TLV_VDEV_PARAM_EARLY_RX_INIT_SLOP,
	WMI_TLV_VDEV_PARAM_EARLY_RX_ADJUST_PAUSE,
	WMI_TLV_VDEV_PARAM_TX_PWRLIMIT,
	WMI_TLV_VDEV_PARAM_SNR_NUM_FOR_CAL,
	WMI_TLV_VDEV_PARAM_ROAM_FW_OFFLOAD,
	WMI_TLV_VDEV_PARAM_ENABLE_RMC,
	WMI_TLV_VDEV_PARAM_IBSS_MAX_BCN_LOST_MS,
	WMI_TLV_VDEV_PARAM_MAX_RATE,
	WMI_TLV_VDEV_PARAM_EARLY_RX_DRIFT_SAMPLE,
	WMI_TLV_VDEV_PARAM_SET_IBSS_TX_FAIL_CNT_THR,
	WMI_TLV_VDEV_PARAM_EBT_RESYNC_TIMEOUT,
	WMI_TLV_VDEV_PARAM_AGGR_TRIG_EVENT_ENABLE,
	WMI_TLV_VDEV_PARAM_IS_IBSS_POWER_SAVE_ALLOWED,
	WMI_TLV_VDEV_PARAM_IS_POWER_COLLAPSE_ALLOWED,
	WMI_TLV_VDEV_PARAM_IS_AWAKE_ON_TXRX_ENABLED,
	WMI_TLV_VDEV_PARAM_INACTIVITY_CNT,
	WMI_TLV_VDEV_PARAM_TXSP_END_INACTIVITY_TIME_MS,
	WMI_TLV_VDEV_PARAM_DTIM_POLICY,
	WMI_TLV_VDEV_PARAM_IBSS_PS_WARMUP_TIME_SECS,
	WMI_TLV_VDEV_PARAM_IBSS_PS_1RX_CHAIN_IN_ATIM_WINDOW_ENABLE,
};

enum wmi_tlv_peer_param {
	WMI_TLV_PEER_SMPS_STATE = 0x1, /* see %wmi_peer_smps_state */
	WMI_TLV_PEER_AMPDU      = 0x2,
	WMI_TLV_PEER_AUTHORIZE  = 0x3,
	WMI_TLV_PEER_CHAN_WIDTH = 0x4,
	WMI_TLV_PEER_NSS        = 0x5,
	WMI_TLV_PEER_USE_4ADDR  = 0x6,
	WMI_TLV_PEER_MEMBERSHIP = 0x7,
	WMI_TLV_PEER_USERPOS = 0x8,
	WMI_TLV_PEER_CRIT_PROTO_HINT_ENABLED = 0x9,
	WMI_TLV_PEER_TX_FAIL_CNT_THR = 0xa,
	WMI_TLV_PEER_SET_HW_RETRY_CTS2S = 0xb,
	WMI_TLV_PEER_IBSS_ATIM_WINDOW_LENGTH = 0xc,
	WMI_TLV_PEER_PHYMODE = 0xd,
	WMI_TLV_PEER_USE_FIXED_PWR = 0xe,
	WMI_TLV_PEER_DUMMY_VAR = 0xff,
};

enum wmi_tlv_peer_flags {
	WMI_TLV_PEER_AUTH = 0x00000001,
	WMI_TLV_PEER_QOS = 0x00000002,
	WMI_TLV_PEER_NEED_PTK_4_WAY = 0x00000004,
	WMI_TLV_PEER_NEED_GTK_2_WAY = 0x00000010,
	WMI_TLV_PEER_APSD = 0x00000800,
	WMI_TLV_PEER_HT = 0x00001000,
	WMI_TLV_PEER_40MHZ = 0x00002000,
	WMI_TLV_PEER_STBC = 0x00008000,
	WMI_TLV_PEER_LDPC = 0x00010000,
	WMI_TLV_PEER_DYN_MIMOPS = 0x00020000,
	WMI_TLV_PEER_STATIC_MIMOPS = 0x00040000,
	WMI_TLV_PEER_SPATIAL_MUX = 0x00200000,
	WMI_TLV_PEER_VHT = 0x02000000,
	WMI_TLV_PEER_80MHZ = 0x04000000,
	WMI_TLV_PEER_PMF = 0x08000000,
	WMI_TLV_PEER_160MHZ = 0x20000000,
};

enum wmi_tlv_tag {
	WMI_TLV_TAG_LAST_RESERVED = 15,

	WMI_TLV_TAG_FIRST_ARRAY_ENUM,
	WMI_TLV_TAG_ARRAY_UINT32 = WMI_TLV_TAG_FIRST_ARRAY_ENUM,
	WMI_TLV_TAG_ARRAY_BYTE,
	WMI_TLV_TAG_ARRAY_STRUCT,
	WMI_TLV_TAG_ARRAY_FIXED_STRUCT,
	WMI_TLV_TAG_LAST_ARRAY_ENUM = 31,

	WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT,
	WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ,
	WMI_TLV_TAG_STRUCT_READY_EVENT,
	WMI_TLV_TAG_STRUCT_SCAN_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_TPC_CONFIG_EVENT,
	WMI_TLV_TAG_STRUCT_CHAN_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_COMB_PHYERR_RX_HDR,
	WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_STOPPED_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT,
	WMI_TLV_TAG_STRUCT_MGMT_RX_HDR,
	WMI_TLV_TAG_STRUCT_TBTT_OFFSET_EVENT,
	WMI_TLV_TAG_STRUCT_TX_DELBA_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_TX_ADDBA_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_EVENT,
	WMI_TLV_TAG_STRUCT_WOW_EVENT_INFO,
	WMI_TLV_TAG_STRUCT_WOW_EVENT_INFO_SECTION_BITMAP,
	WMI_TLV_TAG_STRUCT_RTT_EVENT_HEADER,
	WMI_TLV_TAG_STRUCT_RTT_ERROR_REPORT_EVENT,
	WMI_TLV_TAG_STRUCT_RTT_MEAS_EVENT,
	WMI_TLV_TAG_STRUCT_ECHO_EVENT,
	WMI_TLV_TAG_STRUCT_FTM_INTG_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_GET_KEEPALIVE_EVENT,
	WMI_TLV_TAG_STRUCT_GPIO_INPUT_EVENT,
	WMI_TLV_TAG_STRUCT_CSA_EVENT,
	WMI_TLV_TAG_STRUCT_GTK_OFFLOAD_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_IGTK_INFO,
	WMI_TLV_TAG_STRUCT_DCS_INTERFERENCE_EVENT,
	WMI_TLV_TAG_STRUCT_ATH_DCS_CW_INT,
	WMI_TLV_TAG_STRUCT_ATH_DCS_WLAN_INT_STAT,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_CTX_T,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_T,
	WMI_TLV_TAG_STRUCT_PDEV_QVIT_EVENT,
	WMI_TLV_TAG_STRUCT_HOST_SWBA_EVENT,
	WMI_TLV_TAG_STRUCT_TIM_INFO,
	WMI_TLV_TAG_STRUCT_P2P_NOA_INFO,
	WMI_TLV_TAG_STRUCT_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_AVOID_FREQ_RANGES_EVENT,
	WMI_TLV_TAG_STRUCT_AVOID_FREQ_RANGE_DESC,
	WMI_TLV_TAG_STRUCT_GTK_REKEY_FAIL_EVENT,
	WMI_TLV_TAG_STRUCT_INIT_CMD,
	WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG,
	WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK,
	WMI_TLV_TAG_STRUCT_START_SCAN_CMD,
	WMI_TLV_TAG_STRUCT_STOP_SCAN_CMD,
	WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD,
	WMI_TLV_TAG_STRUCT_CHANNEL,
	WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_WMM_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_WMM_PARAMS,
	WMI_TLV_TAG_STRUCT_PDEV_SET_QUIET_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_DELETE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD,
	WMI_TLV_TAG_STRUCT_P2P_NOA_DESCRIPTOR,
	WMI_TLV_TAG_STRUCT_P2P_GO_SET_BEACON_IE,
	WMI_TLV_TAG_STRUCT_GTK_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_UP_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD,
	WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD,
	WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD,
	WMI_TLV_TAG_STRUCT_PEER_FLUSH_TIDS_CMD,
	WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD,
	WMI_TLV_TAG_STRUCT_VHT_RATE_SET,
	WMI_TLV_TAG_STRUCT_BCN_TMPL_CMD,
	WMI_TLV_TAG_STRUCT_PRB_TMPL_CMD,
	WMI_TLV_TAG_STRUCT_BCN_PRB_INFO,
	WMI_TLV_TAG_STRUCT_PEER_TID_ADDBA_CMD,
	WMI_TLV_TAG_STRUCT_PEER_TID_DELBA_CMD,
	WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD,
	WMI_TLV_TAG_STRUCT_STA_POWERSAVE_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_STA_DTIM_PS_METHOD_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_MODE,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_RSSI_THRESHOLD,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_PERIOD,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_RSSI_CHANGE_THRESHOLD,
	WMI_TLV_TAG_STRUCT_PDEV_SUSPEND_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_RESUME_CMD,
	WMI_TLV_TAG_STRUCT_ADD_BCN_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_RMV_BCN_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_WOW_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_WOW_HOSTWAKEUP_FROM_SLEEP_CMD,
	WMI_TLV_TAG_STRUCT_STA_UAPSD_AUTO_TRIG_CMD,
	WMI_TLV_TAG_STRUCT_STA_UAPSD_AUTO_TRIG_PARAM,
	WMI_TLV_TAG_STRUCT_SET_ARP_NS_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_ARP_OFFLOAD_TUPLE,
	WMI_TLV_TAG_STRUCT_NS_OFFLOAD_TUPLE,
	WMI_TLV_TAG_STRUCT_FTM_INTG_CMD,
	WMI_TLV_TAG_STRUCT_STA_KEEPALIVE_CMD,
	WMI_TLV_TAG_STRUCT_STA_KEEPALVE_ARP_RESPONSE,
	WMI_TLV_TAG_STRUCT_P2P_SET_VENDOR_IE_DATA_CMD,
	WMI_TLV_TAG_STRUCT_AP_PS_PEER_CMD,
	WMI_TLV_TAG_STRUCT_PEER_RATE_RETRY_SCHED_CMD,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_TRIGGER_CMD,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_SET_HIST_INTVL_CMD,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_GET_PROF_DATA_CMD,
	WMI_TLV_TAG_STRUCT_WLAN_PROFILE_ENABLE_PROFILE_ID_CMD,
	WMI_TLV_TAG_STRUCT_WOW_DEL_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_WOW_ADD_DEL_EVT_CMD,
	WMI_TLV_TAG_STRUCT_RTT_MEASREQ_HEAD,
	WMI_TLV_TAG_STRUCT_RTT_MEASREQ_BODY,
	WMI_TLV_TAG_STRUCT_RTT_TSF_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SPECTRAL_CONFIGURE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SPECTRAL_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_REQUEST_STATS_CMD,
	WMI_TLV_TAG_STRUCT_NLO_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_NLO_CONFIGURED_PARAMETERS,
	WMI_TLV_TAG_STRUCT_CSA_OFFLOAD_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_CSA_OFFLOAD_CHANSWITCH_CMD,
	WMI_TLV_TAG_STRUCT_CHATTER_SET_MODE_CMD,
	WMI_TLV_TAG_STRUCT_ECHO_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SET_KEEPALIVE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_GET_KEEPALIVE_CMD,
	WMI_TLV_TAG_STRUCT_FORCE_FW_HANG_CMD,
	WMI_TLV_TAG_STRUCT_GPIO_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_GPIO_OUTPUT_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ADD_WDS_ENTRY_CMD,
	WMI_TLV_TAG_STRUCT_PEER_REMOVE_WDS_ENTRY_CMD,
	WMI_TLV_TAG_STRUCT_BCN_TX_HDR,
	WMI_TLV_TAG_STRUCT_BCN_SEND_FROM_HOST_CMD,
	WMI_TLV_TAG_STRUCT_MGMT_TX_HDR,
	WMI_TLV_TAG_STRUCT_ADDBA_CLEAR_RESP_CMD,
	WMI_TLV_TAG_STRUCT_ADDBA_SEND_CMD,
	WMI_TLV_TAG_STRUCT_DELBA_SEND_CMD,
	WMI_TLV_TAG_STRUCT_ADDBA_SETRESPONSE_CMD,
	WMI_TLV_TAG_STRUCT_SEND_SINGLEAMSDU_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_PKTLOG_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_PKTLOG_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_HT_IE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_VHT_IE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_DSCP_TID_MAP_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_GREEN_AP_PS_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_GET_TPC_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_BASE_MACADDR_CMD,
	WMI_TLV_TAG_STRUCT_PEER_MCAST_GROUP_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_AP_PROFILE,
	WMI_TLV_TAG_STRUCT_AP_PROFILE,
	WMI_TLV_TAG_STRUCT_SCAN_SCH_PRIORITY_TABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_DFS_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_DFS_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_WOW_ADD_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_WOW_BITMAP_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_IPV4_SYNC_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_IPV6_SYNC_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_MAGIC_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_SCAN_UPDATE_REQUEST_CMD,
	WMI_TLV_TAG_STRUCT_CHATTER_PKT_COALESCING_FILTER,
	WMI_TLV_TAG_STRUCT_CHATTER_COALESCING_ADD_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_CHATTER_COALESCING_DELETE_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_CHATTER_COALESCING_QUERY_CMD,
	WMI_TLV_TAG_STRUCT_TXBF_CMD,
	WMI_TLV_TAG_STRUCT_DEBUG_LOG_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_NLO_EVENT,
	WMI_TLV_TAG_STRUCT_CHATTER_QUERY_REPLY_EVENT,
	WMI_TLV_TAG_STRUCT_UPLOAD_H_HDR,
	WMI_TLV_TAG_STRUCT_CAPTURE_H_EVENT_HDR,
	WMI_TLV_TAG_STRUCT_VDEV_WNM_SLEEPMODE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_IPSEC_NATKEEPALIVE_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_WMM_ADDTS_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_WMM_DELTS_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SET_WMM_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_TDLS_SET_STATE_CMD,
	WMI_TLV_TAG_STRUCT_TDLS_PEER_UPDATE_CMD,
	WMI_TLV_TAG_STRUCT_TDLS_PEER_EVENT,
	WMI_TLV_TAG_STRUCT_TDLS_PEER_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_VDEV_MCC_SET_TBTT_MODE_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_CHAN_LIST,
	WMI_TLV_TAG_STRUCT_VDEV_MCC_BCN_INTVL_CHANGE_EVENT,
	WMI_TLV_TAG_STRUCT_RESMGR_ADAPTIVE_OCS_CMD,
	WMI_TLV_TAG_STRUCT_RESMGR_SET_CHAN_TIME_QUOTA_CMD,
	WMI_TLV_TAG_STRUCT_RESMGR_SET_CHAN_LATENCY_CMD,
	WMI_TLV_TAG_STRUCT_BA_REQ_SSN_CMD,
	WMI_TLV_TAG_STRUCT_BA_RSP_SSN_EVENT,
	WMI_TLV_TAG_STRUCT_STA_SMPS_FORCE_MODE_CMD,
	WMI_TLV_TAG_STRUCT_SET_MCASTBCAST_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_P2P_SET_OPPPS_CMD,
	WMI_TLV_TAG_STRUCT_P2P_SET_NOA_CMD,
	WMI_TLV_TAG_STRUCT_BA_REQ_SSN_CMD_SUB_STRUCT_PARAM,
	WMI_TLV_TAG_STRUCT_BA_REQ_SSN_EVENT_SUB_STRUCT_PARAM,
	WMI_TLV_TAG_STRUCT_STA_SMPS_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_SET_GTX_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_MCC_SCHED_TRAFFIC_STATS_CMD,
	WMI_TLV_TAG_STRUCT_MCC_SCHED_STA_TRAFFIC_STATS,
	WMI_TLV_TAG_STRUCT_OFFLOAD_BCN_TX_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_P2P_NOA_EVENT,
	WMI_TLV_TAG_STRUCT_HB_SET_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_HB_SET_TCP_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_HB_SET_TCP_PKT_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_HB_SET_UDP_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_HB_SET_UDP_PKT_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_HB_IND_EVENT,
	WMI_TLV_TAG_STRUCT_TX_PAUSE_EVENT,
	WMI_TLV_TAG_STRUCT_RFKILL_EVENT,
	WMI_TLV_TAG_STRUCT_DFS_RADAR_EVENT,
	WMI_TLV_TAG_STRUCT_DFS_PHYERR_FILTER_ENA_CMD,
	WMI_TLV_TAG_STRUCT_DFS_PHYERR_FILTER_DIS_CMD,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_RESULT_SCAN_LIST,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_RESULT_NETWORK_INFO,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_TRIGGER_RESULT_CMD,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_ENABLED_EVENT,
	WMI_TLV_TAG_STRUCT_BATCH_SCAN_RESULT_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_PLMREQ_START_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_PLMREQ_STOP_CMD,
	WMI_TLV_TAG_STRUCT_THERMAL_MGMT_CMD,
	WMI_TLV_TAG_STRUCT_THERMAL_MGMT_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_INFO_REQ_CMD,
	WMI_TLV_TAG_STRUCT_PEER_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_INFO,
	WMI_TLV_TAG_STRUCT_PEER_TX_FAIL_CNT_THR_EVENT,
	WMI_TLV_TAG_STRUCT_RMC_SET_MODE_CMD,
	WMI_TLV_TAG_STRUCT_RMC_SET_ACTION_PERIOD_CMD,
	WMI_TLV_TAG_STRUCT_RMC_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_MHF_OFFLOAD_SET_MODE_CMD,
	WMI_TLV_TAG_STRUCT_MHF_OFFLOAD_PLUMB_ROUTING_TABLE_CMD,
	WMI_TLV_TAG_STRUCT_ADD_PROACTIVE_ARP_RSP_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_DEL_PROACTIVE_ARP_RSP_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_NAN_CMD_PARAM,
	WMI_TLV_TAG_STRUCT_NAN_EVENT_HDR,
	WMI_TLV_TAG_STRUCT_PDEV_L1SS_TRACK_EVENT,
	WMI_TLV_TAG_STRUCT_DIAG_DATA_CONTAINER_EVENT,
	WMI_TLV_TAG_STRUCT_MODEM_POWER_STATE_CMD_PARAM,
	WMI_TLV_TAG_STRUCT_PEER_GET_ESTIMATED_LINKSPEED_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ESTIMATED_LINKSPEED_EVENT,
	WMI_TLV_TAG_STRUCT_AGGR_STATE_TRIG_EVENT,
	WMI_TLV_TAG_STRUCT_MHF_OFFLOAD_ROUTING_TABLE_ENTRY,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_CMD,
	WMI_TLV_TAG_STRUCT_REQ_STATS_EXT_CMD,
	WMI_TLV_TAG_STRUCT_STATS_EXT_EVENT,
	WMI_TLV_TAG_STRUCT_OBSS_SCAN_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_OBSS_SCAN_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_OFFLOAD_PRB_RSP_TX_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_SET_LED_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_HOST_AUTO_SHUTDOWN_CFG_CMD,
	WMI_TLV_TAG_STRUCT_HOST_AUTO_SHUTDOWN_EVENT,
	WMI_TLV_TAG_STRUCT_UPDATE_WHAL_MIB_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_CHAN_AVOID_UPDATE_CMD_PARAM,
	WMI_TLV_TAG_STRUCT_WOW_ACER_IOAC_PKT_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_ACER_IOAC_TMR_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_IOAC_ADD_KEEPALIVE_CMD,
	WMI_TLV_TAG_STRUCT_WOW_IOAC_DEL_KEEPALIVE_CMD,
	WMI_TLV_TAG_STRUCT_WOW_IOAC_KEEPALIVE_T,
	WMI_TLV_TAG_STRUCT_WOW_ACER_IOAC_ADD_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_WOW_ACER_IOAC_DEL_PATTERN_CMD,
	WMI_TLV_TAG_STRUCT_START_LINK_STATS_CMD,
	WMI_TLV_TAG_STRUCT_CLEAR_LINK_STATS_CMD,
	WMI_TLV_TAG_STRUCT_REQUEST_LINK_STATS_CMD,
	WMI_TLV_TAG_STRUCT_IFACE_LINK_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_RADIO_LINK_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_CHANNEL_STATS,
	WMI_TLV_TAG_STRUCT_RADIO_LINK_STATS,
	WMI_TLV_TAG_STRUCT_RATE_STATS,
	WMI_TLV_TAG_STRUCT_PEER_LINK_STATS,
	WMI_TLV_TAG_STRUCT_WMM_AC_STATS,
	WMI_TLV_TAG_STRUCT_IFACE_LINK_STATS,
	WMI_TLV_TAG_STRUCT_LPI_MGMT_SNOOPING_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_LPI_START_SCAN_CMD,
	WMI_TLV_TAG_STRUCT_LPI_STOP_SCAN_CMD,
	WMI_TLV_TAG_STRUCT_LPI_RESULT_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_STATE_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_BUCKET_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_BUCKET_CHANNEL_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_START_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_STOP_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CONFIGURE_WLAN_CHANGE_MONITOR_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_WLAN_CHANGE_BSSID_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CONFIGURE_HOTLIST_MONITOR_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_GET_CACHED_RESULTS_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_GET_WLAN_CHANGE_RESULTS_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_SET_CAPABILITIES_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_GET_CAPABILITIES_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_OPERATION_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_START_STOP_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_TABLE_USAGE_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_WLAN_DESCRIPTOR_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_RSSI_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CACHED_RESULTS_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_WLAN_CHANGE_RESULTS_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_WLAN_CHANGE_RESULT_BSSID_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_HOTLIST_MATCH_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CAPABILITIES_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CACHE_CAPABILITIES_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_WLAN_CHANGE_MONITOR_CAPABILITIES_EVENT,
	WMI_TLV_TAG_STRUCT_EXTSCAN_HOTLIST_MONITOR_CAPABILITIES_EVENT,
	WMI_TLV_TAG_STRUCT_D0_WOW_ENABLE_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_D0_WOW_DISABLE_ACK_EVENT,
	WMI_TLV_TAG_STRUCT_UNIT_TEST_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_OFFLOAD_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_11I_OFFLOAD_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_11R_OFFLOAD_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_ESE_OFFLOAD_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_SYNCH_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_SYNCH_COMPLETE,
	WMI_TLV_TAG_STRUCT_EXTWOW_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_EXTWOW_SET_APP_TYPE1_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_EXTWOW_SET_APP_TYPE2_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_LPI_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_LPI_HANDOFF_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_RATE_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_RATE_HT_INFO,
	WMI_TLV_TAG_STRUCT_RIC_REQUEST,
	WMI_TLV_TAG_STRUCT_PDEV_GET_TEMPERATURE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_TEMPERATURE_EVENT,
	WMI_TLV_TAG_STRUCT_SET_DHCP_SERVER_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_TPC_CHAINMASK_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_RIC_TSPEC,
	WMI_TLV_TAG_STRUCT_TPC_CHAINMASK_CONFIG,
	WMI_TLV_TAG_STRUCT_IPA_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_SCAN_PROB_REQ_OUI_CMD,
	WMI_TLV_TAG_STRUCT_KEY_MATERIAL,
	WMI_TLV_TAG_STRUCT_TDLS_SET_OFFCHAN_MODE_CMD,
	WMI_TLV_TAG_STRUCT_SET_LED_FLASHING_CMD,
	WMI_TLV_TAG_STRUCT_MDNS_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_MDNS_SET_FQDN_CMD,
	WMI_TLV_TAG_STRUCT_MDNS_SET_RESP_CMD,
	WMI_TLV_TAG_STRUCT_MDNS_GET_STATS_CMD,
	WMI_TLV_TAG_STRUCT_MDNS_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_INVOKE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_RESUME_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_SET_ANTENNA_DIVERSITY_CMD,
	WMI_TLV_TAG_STRUCT_SAP_OFL_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_SAP_OFL_ADD_STA_EVENT,
	WMI_TLV_TAG_STRUCT_SAP_OFL_DEL_STA_EVENT,
	WMI_TLV_TAG_STRUCT_APFIND_CMD_PARAM,
	WMI_TLV_TAG_STRUCT_APFIND_EVENT_HDR,
	WMI_TLV_TAG_STRUCT_OCB_SET_SCHED_CMD,
	WMI_TLV_TAG_STRUCT_OCB_SET_SCHED_EVENT,
	WMI_TLV_TAG_STRUCT_OCB_SET_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_OCB_SET_CONFIG_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_OCB_SET_UTC_TIME_CMD,
	WMI_TLV_TAG_STRUCT_OCB_START_TIMING_ADVERT_CMD,
	WMI_TLV_TAG_STRUCT_OCB_STOP_TIMING_ADVERT_CMD,
	WMI_TLV_TAG_STRUCT_OCB_GET_TSF_TIMER_CMD,
	WMI_TLV_TAG_STRUCT_OCB_GET_TSF_TIMER_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_DCC_GET_STATS_CMD,
	WMI_TLV_TAG_STRUCT_DCC_CHANNEL_STATS_REQUEST,
	WMI_TLV_TAG_STRUCT_DCC_GET_STATS_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_DCC_CLEAR_STATS_CMD,
	WMI_TLV_TAG_STRUCT_DCC_UPDATE_NDL_CMD,
	WMI_TLV_TAG_STRUCT_DCC_UPDATE_NDL_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_DCC_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_OCB_CHANNEL,
	WMI_TLV_TAG_STRUCT_OCB_SCHEDULE_ELEMENT,
	WMI_TLV_TAG_STRUCT_DCC_NDL_STATS_PER_CHANNEL,
	WMI_TLV_TAG_STRUCT_DCC_NDL_CHAN,
	WMI_TLV_TAG_STRUCT_QOS_PARAMETER,
	WMI_TLV_TAG_STRUCT_DCC_NDL_ACTIVE_STATE_CONFIG,
	WMI_TLV_TAG_STRUCT_ROAM_SCAN_EXTENDED_THRESHOLD_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_FILTER_FIXED_PARAM,
	WMI_TLV_TAG_STRUCT_PASSPOINT_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_PASSPOINT_EVENT_HDR,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CONFIGURE_HOTLIST_SSID_MONITOR_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_HOTLIST_SSID_MATCH_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_TSF_TSTAMP_ACTION_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_TSF_REPORT_EVENT,
	WMI_TLV_TAG_STRUCT_GET_FW_MEM_DUMP,
	WMI_TLV_TAG_STRUCT_UPDATE_FW_MEM_DUMP,
	WMI_TLV_TAG_STRUCT_FW_MEM_DUMP_PARAMS,
	WMI_TLV_TAG_STRUCT_DEBUG_MESG_FLUSH,
	WMI_TLV_TAG_STRUCT_DEBUG_MESG_FLUSH_COMPLETE,
	WMI_TLV_TAG_STRUCT_PEER_SET_RATE_REPORT_CONDITION,
	WMI_TLV_TAG_STRUCT_ROAM_SUBNET_CHANGE_CONFIG,
	WMI_TLV_TAG_STRUCT_VDEV_SET_IE_CMD,
	WMI_TLV_TAG_STRUCT_RSSI_BREACH_MONITOR_CONFIG,
	WMI_TLV_TAG_STRUCT_RSSI_BREACH_EVENT,
	WMI_TLV_TAG_STRUCT_EVENT_INITIAL_WAKEUP,
	WMI_TLV_TAG_STRUCT_SOC_SET_PCL_CMD,
	WMI_TLV_TAG_STRUCT_SOC_SET_HW_MODE_CMD,
	WMI_TLV_TAG_STRUCT_SOC_SET_HW_MODE_RESPONSE_EVENT,
	WMI_TLV_TAG_STRUCT_SOC_HW_MODE_TRANSITION_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_TXRX_STREAMS,
	WMI_TLV_TAG_STRUCT_SOC_SET_HW_MODE_RESPONSE_VDEV_MAC_ENTRY,
	WMI_TLV_TAG_STRUCT_SOC_SET_DUAL_MAC_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_SOC_SET_DUAL_MAC_CONFIG_RESPONSE_EVENT,
	WMI_TLV_TAG_STRUCT_IOAC_SOCK_PATTERN_T,
	WMI_TLV_TAG_STRUCT_WOW_ENABLE_ICMPV6_NA_FLT_CMD,
	WMI_TLV_TAG_STRUCT_DIAG_EVENT_LOG_CONFIG,
	WMI_TLV_TAG_STRUCT_DIAG_EVENT_LOG_SUPPORTED_EVENT,
	WMI_TLV_TAG_STRUCT_PACKET_FILTER_CONFIG,
	WMI_TLV_TAG_STRUCT_PACKET_FILTER_ENABLE,
	WMI_TLV_TAG_STRUCT_SAP_SET_BLACKLIST_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_MGMT_TX_CMD,
	WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT,
	WMI_TLV_TAG_STRUCT_SOC_SET_ANTENNA_MODE_CMD,
	WMI_TLV_TAG_STRUCT_WOW_UDP_SVC_OFLD_CMD,
	WMI_TLV_TAG_STRUCT_LRO_INFO_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_EARLYSTOP_RSSI_THRES_PARAM,
	WMI_TLV_TAG_STRUCT_SERVICE_READY_EXT_EVENT,
	WMI_TLV_TAG_STRUCT_MAWC_SENSOR_REPORT_IND_CMD,
	WMI_TLV_TAG_STRUCT_MAWC_ENABLE_SENSOR_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_CONFIGURE_MAWC_CMD,
	WMI_TLV_TAG_STRUCT_NLO_CONFIGURE_MAWC_CMD,
	WMI_TLV_TAG_STRUCT_EXTSCAN_CONFIGURE_MAWC_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ASSOC_CONF_EVENT,
	WMI_TLV_TAG_STRUCT_WOW_HOSTWAKEUP_GPIO_PIN_PATTERN_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_AP_PS_EGAP_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_AP_PS_EGAP_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_PMF_OFFLOAD_SET_SA_QUERY_CMD,
	WMI_TLV_TAG_STRUCT_TRANSFER_DATA_TO_FLASH_CMD,
	WMI_TLV_TAG_STRUCT_TRANSFER_DATA_TO_FLASH_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_SCPC_EVENT,
	WMI_TLV_TAG_STRUCT_AP_PS_EGAP_INFO_CHAINMASK_LIST,
	WMI_TLV_TAG_STRUCT_STA_SMPS_FORCE_MODE_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_BPF_GET_CAPABILITY_CMD,
	WMI_TLV_TAG_STRUCT_BPF_CAPABILITY_INFO_EVT,
	WMI_TLV_TAG_STRUCT_BPF_GET_VDEV_STATS_CMD,
	WMI_TLV_TAG_STRUCT_BPF_VDEV_STATS_INFO_EVT,
	WMI_TLV_TAG_STRUCT_BPF_SET_VDEV_INSTRUCTIONS_CMD,
	WMI_TLV_TAG_STRUCT_BPF_DEL_VDEV_INSTRUCTIONS_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_DELETE_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_DELETE_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_DENSE_THRES_PARAM,
	WMI_TLV_TAG_STRUCT_ENLO_CANDIDATE_SCORE_PARAM,
	WMI_TLV_TAG_STRUCT_PEER_UPDATE_WDS_ENTRY_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_CONFIG_RATEMASK,
	WMI_TLV_TAG_STRUCT_PDEV_FIPS_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SMART_ANT_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SMART_ANT_SET_RX_ANTENNA_CMD,
	WMI_TLV_TAG_STRUCT_PEER_SMART_ANT_SET_TX_ANTENNA_CMD,
	WMI_TLV_TAG_STRUCT_PEER_SMART_ANT_SET_TRAIN_ANTENNA_CMD,
	WMI_TLV_TAG_STRUCT_PEER_SMART_ANT_SET_NODE_CONFIG_OPS_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_ANT_SWITCH_TBL_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_CTL_TABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_MIMOGAIN_TABLE_CMD,
	WMI_TLV_TAG_STRUCT_FWTEST_SET_PARAM_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ATF_REQUEST,
	WMI_TLV_TAG_STRUCT_VDEV_ATF_REQUEST,
	WMI_TLV_TAG_STRUCT_PDEV_GET_ANI_CCK_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_GET_ANI_OFDM_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_INST_RSSI_STATS_RESP,
	WMI_TLV_TAG_STRUCT_MED_UTIL_REPORT_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_STA_PS_STATECHANGE_EVENT,
	WMI_TLV_TAG_STRUCT_WDS_ADDR_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_RATECODE_LIST_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_NFCAL_POWER_ALL_CHANNELS_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_TPC_EVENT,
	WMI_TLV_TAG_STRUCT_ANI_OFDM_EVENT,
	WMI_TLV_TAG_STRUCT_ANI_CCK_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_CHANNEL_HOPPING_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_FIPS_EVENT,
	WMI_TLV_TAG_STRUCT_ATF_PEER_INFO,
	WMI_TLV_TAG_STRUCT_PDEV_GET_TPC_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_FILTER_NRP_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_QBOOST_CFG_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SMART_ANT_GPIO_HANDLE,
	WMI_TLV_TAG_STRUCT_PEER_SMART_ANT_SET_TX_ANTENNA_SERIES,
	WMI_TLV_TAG_STRUCT_PEER_SMART_ANT_SET_TRAIN_ANTENNA_PARAM,
	WMI_TLV_TAG_STRUCT_PDEV_SET_ANT_CTRL_CHAIN,
	WMI_TLV_TAG_STRUCT_PEER_CCK_OFDM_RATE_INFO,
	WMI_TLV_TAG_STRUCT_PEER_MCS_RATE_INFO,
	WMI_TLV_TAG_STRUCT_PDEV_NFCAL_POWER_ALL_CHANNELS_NFDBR,
	WMI_TLV_TAG_STRUCT_PDEV_NFCAL_POWER_ALL_CHANNELS_NFDBM,
	WMI_TLV_TAG_STRUCT_PDEV_NFCAL_POWER_ALL_CHANNELS_FREQNUM,
	WMI_TLV_TAG_STRUCT_MU_REPORT_TOTAL_MU,
	WMI_TLV_TAG_STRUCT_VDEV_SET_DSCP_TID_MAP_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_SET_MBO,
	WMI_TLV_TAG_STRUCT_MIB_STATS_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_NAN_DISC_IFACE_CREATED_EVENT,
	WMI_TLV_TAG_STRUCT_NAN_DISC_IFACE_DELETED_EVENT,
	WMI_TLV_TAG_STRUCT_NAN_STARTED_CLUSTER_EVENT,
	WMI_TLV_TAG_STRUCT_NAN_JOINED_CLUSTER_EVENT,
	WMI_TLV_TAG_STRUCT_NDI_GET_CAP_REQ,
	WMI_TLV_TAG_STRUCT_NDP_INITIATOR_REQ,
	WMI_TLV_TAG_STRUCT_NDP_RESPONDER_REQ,
	WMI_TLV_TAG_STRUCT_NDP_END_REQ,
	WMI_TLV_TAG_STRUCT_NDI_CAP_RSP_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_INITIATOR_RSP_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_RESPONDER_RSP_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_END_RSP_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_INDICATION_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_CONFIRM_EVENT,
	WMI_TLV_TAG_STRUCT_NDP_END_INDICATION_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_SET_QUIET_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_PCL_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_HW_MODE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_MAC_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_ANTENNA_MODE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_HW_MODE_RESPONSE_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_HW_MODE_TRANSITION_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_SET_HW_MODE_RESPONSE_VDEV_MAC_ENTRY,
	WMI_TLV_TAG_STRUCT_PDEV_SET_MAC_CONFIG_RESPONSE_EVENT,
	WMI_TLV_TAG_STRUCT_COEX_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_CONFIG_ENHANCED_MCAST_FILTER,
	WMI_TLV_TAG_STRUCT_CHAN_AVOID_RPT_ALLOW_CMD,
	WMI_TLV_TAG_STRUCT_SET_PERIODIC_CHANNEL_STATS_CONFIG,
	WMI_TLV_TAG_STRUCT_VDEV_SET_CUSTOM_AGGR_SIZE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_WAL_POWER_DEBUG_CMD,
	WMI_TLV_TAG_STRUCT_MAC_PHY_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_HW_MODE_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_SOC_MAC_PHY_HW_MODE_CAPS,
	WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES_EXT,
	WMI_TLV_TAG_STRUCT_SOC_HAL_REG_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_VDEV_WISA_CMD,
	WMI_TLV_TAG_STRUCT_TX_POWER_LEVEL_STATS_EVT,
	WMI_TLV_TAG_STRUCT_SCAN_ADAPTIVE_DWELL_PARAMETERS_TLV,
	WMI_TLV_TAG_STRUCT_SCAN_ADAPTIVE_DWELL_CONFIG,
	WMI_TLV_TAG_STRUCT_WOW_SET_ACTION_WAKE_UP_CMD,
	WMI_TLV_TAG_STRUCT_NDP_END_RSP_PER_NDI,
	WMI_TLV_TAG_STRUCT_PEER_BWF_REQUEST,
	WMI_TLV_TAG_STRUCT_BWF_PEER_INFO,
	WMI_TLV_TAG_STRUCT_DBGLOG_TIME_STAMP_SYNC_CMD,
	WMI_TLV_TAG_STRUCT_RMC_SET_LEADER_CMD,
	WMI_TLV_TAG_STRUCT_RMC_MANUAL_LEADER_EVENT,
	WMI_TLV_TAG_STRUCT_PER_CHAIN_RSSI_STATS,
	WMI_TLV_TAG_STRUCT_RSSI_STATS,
	WMI_TLV_TAG_STRUCT_P2P_LO_START_CMD,
	WMI_TLV_TAG_STRUCT_P2P_LO_STOP_CMD,
	WMI_TLV_TAG_STRUCT_P2P_LO_STOPPED_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_REORDER_QUEUE_SETUP_CMD,
	WMI_TLV_TAG_STRUCT_PEER_REORDER_QUEUE_REMOVE_CMD,
	WMI_TLV_TAG_STRUCT_SET_MULTIPLE_MCAST_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT,
	WMI_TLV_TAG_STRUCT_READ_DATA_FROM_FLASH_CMD,
	WMI_TLV_TAG_STRUCT_READ_DATA_FROM_FLASH_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_SET_REORDER_TIMEOUT_VAL_CMD,
	WMI_TLV_TAG_STRUCT_PEER_SET_RX_BLOCKSIZE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_SET_WAKEUP_CONFIG_CMDID,
	WMI_TLV_TAG_STRUCT_TLV_BUF_LEN_PARAM,
	WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_ANTDIV_INFO_REQ_CMD,
	WMI_TLV_TAG_STRUCT_PEER_ANTDIV_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_PEER_ANTDIV_INFO,
	WMI_TLV_TAG_STRUCT_PDEV_GET_ANTDIV_STATUS_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_ANTDIV_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_MNT_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_GET_CHIP_POWER_STATS_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_CHIP_POWER_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_COEX_GET_ANTENNA_ISOLATION_CMD,
	WMI_TLV_TAG_STRUCT_COEX_REPORT_ISOLATION_EVENT,
	WMI_TLV_TAG_STRUCT_CHAN_CCA_STATS,
	WMI_TLV_TAG_STRUCT_PEER_SIGNAL_STATS,
	WMI_TLV_TAG_STRUCT_TX_STATS,
	WMI_TLV_TAG_STRUCT_PEER_AC_TX_STATS,
	WMI_TLV_TAG_STRUCT_RX_STATS,
	WMI_TLV_TAG_STRUCT_PEER_AC_RX_STATS,
	WMI_TLV_TAG_STRUCT_REPORT_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_CHAN_CCA_STATS_THRESH,
	WMI_TLV_TAG_STRUCT_PEER_SIGNAL_STATS_THRESH,
	WMI_TLV_TAG_STRUCT_TX_STATS_THRESH,
	WMI_TLV_TAG_STRUCT_RX_STATS_THRESH,
	WMI_TLV_TAG_STRUCT_PDEV_SET_STATS_THRESHOLD_CMD,
	WMI_TLV_TAG_STRUCT_REQUEST_WLAN_STATS_CMD,
	WMI_TLV_TAG_STRUCT_RX_AGGR_FAILURE_EVENT,
	WMI_TLV_TAG_STRUCT_RX_AGGR_FAILURE_INFO,
	WMI_TLV_TAG_STRUCT_VDEV_ENCRYPT_DECRYPT_DATA_REQ_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_ENCRYPT_DECRYPT_DATA_RESP_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_BAND_TO_MAC,
	WMI_TLV_TAG_STRUCT_TBTT_OFFSET_INFO,
	WMI_TLV_TAG_STRUCT_TBTT_OFFSET_EXT_EVENT,
	WMI_TLV_TAG_STRUCT_SAR_LIMITS_CMD,
	WMI_TLV_TAG_STRUCT_SAR_LIMIT_CMD_ROW,
	WMI_TLV_TAG_STRUCT_PDEV_DFS_PHYERR_OFFLOAD_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_DFS_PHYERR_OFFLOAD_DISABLE_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_ADFS_CH_CFG_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_ADFS_OCAC_ABORT_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_DFS_RADAR_DETECTION_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_ADFS_OCAC_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_DFS_CAC_COMPLETE_EVENT,
	WMI_TLV_TAG_STRUCT_VENDOR_OUI,
	WMI_TLV_TAG_STRUCT_REQUEST_RCPI_CMD,
	WMI_TLV_TAG_STRUCT_UPDATE_RCPI_EVENT,
	WMI_TLV_TAG_STRUCT_REQUEST_PEER_STATS_INFO_CMD,
	WMI_TLV_TAG_STRUCT_PEER_STATS_INFO,
	WMI_TLV_TAG_STRUCT_PEER_STATS_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_PKGID_EVENT,
	WMI_TLV_TAG_STRUCT_CONNECTED_NLO_RSSI_PARAMS,
	WMI_TLV_TAG_STRUCT_SET_CURRENT_COUNTRY_CMD,
	WMI_TLV_TAG_STRUCT_REGULATORY_RULE_STRUCT,
	WMI_TLV_TAG_STRUCT_REG_CHAN_LIST_CC_EVENT,
	WMI_TLV_TAG_STRUCT_11D_SCAN_START_CMD,
	WMI_TLV_TAG_STRUCT_11D_SCAN_STOP_CMD,
	WMI_TLV_TAG_STRUCT_11D_NEW_COUNTRY_EVENT,
	WMI_TLV_TAG_STRUCT_REQUEST_RADIO_CHAN_STATS_CMD,
	WMI_TLV_TAG_STRUCT_RADIO_CHAN_STATS,
	WMI_TLV_TAG_STRUCT_RADIO_CHAN_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_PER_CONFIG,
	WMI_TLV_TAG_STRUCT_VDEV_ADD_MAC_ADDR_TO_RX_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_ADD_MAC_ADDR_TO_RX_FILTER_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_BPF_SET_VDEV_ACTIVE_MODE_CMD,
	WMI_TLV_TAG_STRUCT_HW_DATA_FILTER_CMD,
	WMI_TLV_TAG_STRUCT_CONNECTED_NLO_BSS_BAND_RSSI_PREF,
	WMI_TLV_TAG_STRUCT_PEER_OPER_MODE_CHANGE_EVENT,
	WMI_TLV_TAG_STRUCT_CHIP_POWER_SAVE_FAILURE_DETECTED,
	WMI_TLV_TAG_STRUCT_PDEV_MULTIPLE_VDEV_RESTART_REQUEST_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_CSA_SWITCH_COUNT_STATUS_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_UPDATE_PKT_ROUTING_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_CHECK_CAL_VERSION_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_CHECK_CAL_VERSION_EVENT,
	WMI_TLV_TAG_STRUCT_PDEV_SET_DIVERSITY_GAIN_CMD,
	WMI_TLV_TAG_STRUCT_MAC_PHY_CHAINMASK_COMBO,
	WMI_TLV_TAG_STRUCT_MAC_PHY_CHAINMASK_CAPABILITY,
	WMI_TLV_TAG_STRUCT_VDEV_SET_ARP_STATS_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_GET_ARP_STATS_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_GET_ARP_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_IFACE_OFFLOAD_STATS,
	WMI_TLV_TAG_STRUCT_REQUEST_STATS_CMD_SUB_STRUCT_PARAM,
	WMI_TLV_TAG_STRUCT_RSSI_CTL_EXT,
	WMI_TLV_TAG_STRUCT_SINGLE_PHYERR_EXT_RX_HDR,
	WMI_TLV_TAG_STRUCT_COEX_BT_ACTIVITY_EVENT,
	WMI_TLV_TAG_STRUCT_VDEV_GET_TX_POWER_CMD,
	WMI_TLV_TAG_STRUCT_VDEV_TX_POWER_EVENT,
	WMI_TLV_TAG_STRUCT_OFFCHAN_DATA_TX_COMPL_EVENT,
	WMI_TLV_TAG_STRUCT_OFFCHAN_DATA_TX_SEND_CMD,
	WMI_TLV_TAG_STRUCT_TX_SEND_PARAMS,
	WMI_TLV_TAG_STRUCT_HE_RATE_SET,
	WMI_TLV_TAG_STRUCT_CONGESTION_STATS,
	WMI_TLV_TAG_STRUCT_SET_INIT_COUNTRY_CMD,
	WMI_TLV_TAG_STRUCT_SCAN_DBS_DUTY_CYCLE,
	WMI_TLV_TAG_STRUCT_SCAN_DBS_DUTY_CYCLE_PARAM_TLV,
	WMI_TLV_TAG_STRUCT_PDEV_DIV_GET_RSSI_ANTID,
	WMI_TLV_TAG_STRUCT_THERM_THROT_CONFIG_REQUEST,
	WMI_TLV_TAG_STRUCT_THERM_THROT_LEVEL_CONFIG_INFO,
	WMI_TLV_TAG_STRUCT_THERM_THROT_STATS_EVENT,
	WMI_TLV_TAG_STRUCT_THERM_THROT_LEVEL_STATS_INFO,
	WMI_TLV_TAG_STRUCT_PDEV_DIV_RSSI_ANTID_EVENT,
	WMI_TLV_TAG_STRUCT_OEM_DMA_RING_CAPABILITIES,
	WMI_TLV_TAG_STRUCT_OEM_DMA_RING_CFG_REQ,
	WMI_TLV_TAG_STRUCT_OEM_DMA_RING_CFG_RSP,
	WMI_TLV_TAG_STRUCT_OEM_INDIRECT_DATA,
	WMI_TLV_TAG_STRUCT_OEM_DMA_BUF_RELEASE,
	WMI_TLV_TAG_STRUCT_OEM_DMA_BUF_RELEASE_ENTRY,
	WMI_TLV_TAG_STRUCT_PDEV_BSS_CHAN_INFO_REQUEST,
	WMI_TLV_TAG_STRUCT_PDEV_BSS_CHAN_INFO_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_LCA_DISALLOW_CONFIG_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_VDEV_LIMIT_OFFCHAN_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_RSSI_REJECTION_OCE_CONFIG_PARAM,
	WMI_TLV_TAG_STRUCT_UNIT_TEST_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_FILS_OFFLOAD_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_PDEV_UPDATE_PMK_CACHE_CMD,
	WMI_TLV_TAG_STRUCT_PMK_CACHE,
	WMI_TLV_TAG_STRUCT_PDEV_UPDATE_FILS_HLP_PKT_CMD,
	WMI_TLV_TAG_STRUCT_ROAM_FILS_SYNCH_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_GTK_OFFLOAD_EXTENDED_TLV_PARAM,
	WMI_TLV_TAG_STRUCT_ROAM_BG_SCAN_ROAMING_PARAM,
	WMI_TLV_TAG_STRUCT_OIC_PING_OFFLOAD_PARAMS_CMD,
	WMI_TLV_TAG_STRUCT_OIC_PING_OFFLOAD_SET_ENABLE_CMD,
	WMI_TLV_TAG_STRUCT_OIC_PING_HANDOFF_EVENT,
	WMI_TLV_TAG_STRUCT_DHCP_LEASE_RENEW_OFFLOAD_CMD,
	WMI_TLV_TAG_STRUCT_DHCP_LEASE_RENEW_EVENT,
	WMI_TLV_TAG_STRUCT_BTM_CONFIG,
	WMI_TLV_TAG_STRUCT_DEBUG_MESG_FW_DATA_STALL_PARAM,
	WMI_TLV_TAG_STRUCT_WLM_CONFIG_CMD,
	WMI_TLV_TAG_STRUCT_PDEV_UPDATE_CTLTABLE_REQUEST,
	WMI_TLV_TAG_STRUCT_PDEV_UPDATE_CTLTABLE_EVENT,
	WMI_TLV_TAG_STRUCT_ROAM_CND_SCORING_PARAM,
	WMI_TLV_TAG_STRUCT_PDEV_CONFIG_VENDOR_OUI_ACTION,
	WMI_TLV_TAG_STRUCT_VENDOR_OUI_EXT,
	WMI_TLV_TAG_STRUCT_ROAM_SYNCH_FRAME_EVENT,
	WMI_TLV_TAG_STRUCT_FD_SEND_FROM_HOST_CMD,
	WMI_TLV_TAG_STRUCT_ENABLE_FILS_CMD,
	WMI_TLV_TAG_STRUCT_HOST_SWFDA_EVENT,

	WMI_TLV_TAG_MAX
};

enum wmi_tlv_service {
	WMI_TLV_SERVICE_BEACON_OFFLOAD = 0,
	WMI_TLV_SERVICE_SCAN_OFFLOAD,
	WMI_TLV_SERVICE_ROAM_SCAN_OFFLOAD,
	WMI_TLV_SERVICE_BCN_MISS_OFFLOAD,
	WMI_TLV_SERVICE_STA_PWRSAVE,
	WMI_TLV_SERVICE_STA_ADVANCED_PWRSAVE,
	WMI_TLV_SERVICE_AP_UAPSD,
	WMI_TLV_SERVICE_AP_DFS,
	WMI_TLV_SERVICE_11AC,
	WMI_TLV_SERVICE_BLOCKACK,
	WMI_TLV_SERVICE_PHYERR,
	WMI_TLV_SERVICE_BCN_FILTER,
	WMI_TLV_SERVICE_RTT,
	WMI_TLV_SERVICE_WOW,
	WMI_TLV_SERVICE_RATECTRL_CACHE,
	WMI_TLV_SERVICE_IRAM_TIDS,
	WMI_TLV_SERVICE_ARPNS_OFFLOAD,
	WMI_TLV_SERVICE_NLO,
	WMI_TLV_SERVICE_GTK_OFFLOAD,
	WMI_TLV_SERVICE_SCAN_SCH,
	WMI_TLV_SERVICE_CSA_OFFLOAD,
	WMI_TLV_SERVICE_CHATTER,
	WMI_TLV_SERVICE_COEX_FREQAVOID,
	WMI_TLV_SERVICE_PACKET_POWER_SAVE,
	WMI_TLV_SERVICE_FORCE_FW_HANG,
	WMI_TLV_SERVICE_GPIO,
	WMI_TLV_SERVICE_STA_DTIM_PS_MODULATED_DTIM,
	WMI_TLV_SERVICE_STA_UAPSD_BASIC_AUTO_TRIG,
	WMI_TLV_SERVICE_STA_UAPSD_VAR_AUTO_TRIG,
	WMI_TLV_SERVICE_STA_KEEP_ALIVE,
	WMI_TLV_SERVICE_TX_ENCAP,
	WMI_TLV_SERVICE_AP_PS_DETECT_OUT_OF_SYNC,
	WMI_TLV_SERVICE_EARLY_RX,
	WMI_TLV_SERVICE_STA_SMPS,
	WMI_TLV_SERVICE_FWTEST,
	WMI_TLV_SERVICE_STA_WMMAC,
	WMI_TLV_SERVICE_TDLS,
	WMI_TLV_SERVICE_BURST,
	WMI_TLV_SERVICE_MCC_BCN_INTERVAL_CHANGE,
	WMI_TLV_SERVICE_ADAPTIVE_OCS,
	WMI_TLV_SERVICE_BA_SSN_SUPPORT,
	WMI_TLV_SERVICE_FILTER_IPSEC_NATKEEPALIVE,
	WMI_TLV_SERVICE_WLAN_HB,
	WMI_TLV_SERVICE_LTE_ANT_SHARE_SUPPORT,
	WMI_TLV_SERVICE_BATCH_SCAN,
	WMI_TLV_SERVICE_QPOWER,
	WMI_TLV_SERVICE_PLMREQ,
	WMI_TLV_SERVICE_THERMAL_MGMT,
	WMI_TLV_SERVICE_RMC,
	WMI_TLV_SERVICE_MHF_OFFLOAD,
	WMI_TLV_SERVICE_COEX_SAR,
	WMI_TLV_SERVICE_BCN_TXRATE_OVERRIDE,
	WMI_TLV_SERVICE_NAN,
	WMI_TLV_SERVICE_L1SS_STAT,
	WMI_TLV_SERVICE_ESTIMATE_LINKSPEED,
	WMI_TLV_SERVICE_OBSS_SCAN,
	WMI_TLV_SERVICE_TDLS_OFFCHAN,
	WMI_TLV_SERVICE_TDLS_UAPSD_BUFFER_STA,
	WMI_TLV_SERVICE_TDLS_UAPSD_SLEEP_STA,
	WMI_TLV_SERVICE_IBSS_PWRSAVE,
	WMI_TLV_SERVICE_LPASS,
	WMI_TLV_SERVICE_EXTSCAN,
	WMI_TLV_SERVICE_D0WOW,
	WMI_TLV_SERVICE_HSOFFLOAD,
	WMI_TLV_SERVICE_ROAM_HO_OFFLOAD,
	WMI_TLV_SERVICE_RX_FULL_REORDER,
	WMI_TLV_SERVICE_DHCP_OFFLOAD,
	WMI_TLV_SERVICE_STA_RX_IPA_OFFLOAD_SUPPORT,
	WMI_TLV_SERVICE_MDNS_OFFLOAD,
	WMI_TLV_SERVICE_SAP_AUTH_OFFLOAD,
	WMI_TLV_SERVICE_DUAL_BAND_SIMULTANEOUS_SUPPORT,
	WMI_TLV_SERVICE_OCB,
	WMI_TLV_SERVICE_AP_ARPNS_OFFLOAD,
	WMI_TLV_SERVICE_PER_BAND_CHAINMASK_SUPPORT,
	WMI_TLV_SERVICE_PACKET_FILTER_OFFLOAD,
	WMI_TLV_SERVICE_MGMT_TX_HTT,
	WMI_TLV_SERVICE_MGMT_TX_WMI,
	WMI_TLV_SERVICE_EXT_MSG,
	WMI_TLV_SERVICE_MAWC,
	WMI_TLV_SERVICE_PEER_ASSOC_CONF,
	WMI_TLV_SERVICE_EGAP,
	WMI_TLV_SERVICE_STA_PMF_OFFLOAD,
	WMI_TLV_SERVICE_UNIFIED_WOW_CAPABILITY,
	WMI_TLV_SERVICE_ENHANCED_PROXY_STA,
	WMI_TLV_SERVICE_ATF,
	WMI_TLV_SERVICE_COEX_GPIO,
	WMI_TLV_SERVICE_AUX_SPECTRAL_INTF,
	WMI_TLV_SERVICE_AUX_CHAN_LOAD_INTF,
	WMI_TLV_SERVICE_BSS_CHANNEL_INFO_64,
	WMI_TLV_SERVICE_ENTERPRISE_MESH,
	WMI_TLV_SERVICE_RESTRT_CHNL_SUPPORT,
	WMI_TLV_SERVICE_BPF_OFFLOAD,
	WMI_TLV_SERVICE_SYNC_DELETE_CMDS,
	WMI_TLV_SERVICE_SMART_ANTENNA_SW_SUPPORT,
	WMI_TLV_SERVICE_SMART_ANTENNA_HW_SUPPORT,
	WMI_TLV_SERVICE_RATECTRL_LIMIT_MAX_MIN_RATES,
	WMI_TLV_SERVICE_NAN_DATA,
	WMI_TLV_SERVICE_NAN_RTT,
	WMI_TLV_SERVICE_11AX,
	WMI_TLV_SERVICE_DEPRECATED_REPLACE,
	WMI_TLV_SERVICE_TDLS_CONN_TRACKER_IN_HOST_MODE,
	WMI_TLV_SERVICE_ENHANCED_MCAST_FILTER,
	WMI_TLV_SERVICE_PERIODIC_CHAN_STAT_SUPPORT,
	WMI_TLV_SERVICE_MESH_11S,
	WMI_TLV_SERVICE_HALF_RATE_QUARTER_RATE_SUPPORT,
	WMI_TLV_SERVICE_VDEV_RX_FILTER,
	WMI_TLV_SERVICE_P2P_LISTEN_OFFLOAD_SUPPORT,
	WMI_TLV_SERVICE_MARK_FIRST_WAKEUP_PACKET,
	WMI_TLV_SERVICE_MULTIPLE_MCAST_FILTER_SET,
	WMI_TLV_SERVICE_HOST_MANAGED_RX_REORDER,
	WMI_TLV_SERVICE_FLASH_RDWR_SUPPORT,
	WMI_TLV_SERVICE_WLAN_STATS_REPORT,
	WMI_TLV_SERVICE_TX_MSDU_ID_NEW_PARTITION_SUPPORT,
	WMI_TLV_SERVICE_DFS_PHYERR_OFFLOAD,
	WMI_TLV_SERVICE_RCPI_SUPPORT,
	WMI_TLV_SERVICE_FW_MEM_DUMP_SUPPORT,
	WMI_TLV_SERVICE_PEER_STATS_INFO,
	WMI_TLV_SERVICE_REGULATORY_DB,
	WMI_TLV_SERVICE_11D_OFFLOAD,
	WMI_TLV_SERVICE_HW_DATA_FILTERING,
	WMI_TLV_SERVICE_MULTIPLE_VDEV_RESTART,
	WMI_TLV_SERVICE_PKT_ROUTING,
	WMI_TLV_SERVICE_CHECK_CAL_VERSION,
	WMI_TLV_SERVICE_OFFCHAN_TX_WMI,
	WMI_TLV_SERVICE_8SS_TX_BFEE,
	WMI_TLV_SERVICE_EXTENDED_NSS_SUPPORT,
	WMI_TLV_SERVICE_ACK_TIMEOUT,
	WMI_TLV_SERVICE_PDEV_BSS_CHANNEL_INFO_64,
	WMI_TLV_MAX_SERVICE = 128,

/* NOTE:
 * The above service flags are delivered in the wmi_service_bitmap field
 * of the WMI_TLV_SERVICE_READY_EVENT message.
 * The below service flags are delivered in a WMI_TLV_SERVICE_AVAILABLE_EVENT
 * message rather than in the WMI_TLV_SERVICE_READY_EVENT message's
 * wmi_service_bitmap field.
 * The WMI_TLV_SERVICE_AVAILABLE_EVENT message immediately precedes the
 * WMI_TLV_SERVICE_READY_EVENT message.
 */

	WMI_TLV_SERVICE_CHAN_LOAD_INFO = 128,
	WMI_TLV_SERVICE_TX_PPDU_INFO_STATS_SUPPORT,
	WMI_TLV_SERVICE_VDEV_LIMIT_OFFCHAN_SUPPORT,
	WMI_TLV_SERVICE_FILS_SUPPORT,
	WMI_TLV_SERVICE_WLAN_OIC_PING_OFFLOAD,
	WMI_TLV_SERVICE_WLAN_DHCP_RENEW,
	WMI_TLV_SERVICE_MAWC_SUPPORT,
	WMI_TLV_SERVICE_VDEV_LATENCY_CONFIG,
	WMI_TLV_SERVICE_PDEV_UPDATE_CTLTABLE_SUPPORT,
	WMI_TLV_SERVICE_PKTLOG_SUPPORT_OVER_HTT,
	WMI_TLV_SERVICE_VDEV_MULTI_GROUP_KEY_SUPPORT,
	WMI_TLV_SERVICE_SCAN_PHYMODE_SUPPORT,
	WMI_TLV_SERVICE_THERM_THROT,
	WMI_TLV_SERVICE_BCN_OFFLOAD_START_STOP_SUPPORT,
	WMI_TLV_SERVICE_WOW_WAKEUP_BY_TIMER_PATTERN,
	WMI_TLV_SERVICE_PEER_MAP_UNMAP_V2_SUPPORT = 143,
	WMI_TLV_SERVICE_OFFCHAN_DATA_TID_SUPPORT = 144,
	WMI_TLV_SERVICE_RX_PROMISC_ENABLE_SUPPORT = 145,
	WMI_TLV_SERVICE_SUPPORT_DIRECT_DMA = 146,
	WMI_TLV_SERVICE_AP_OBSS_DETECTION_OFFLOAD = 147,
	WMI_TLV_SERVICE_11K_NEIGHBOUR_REPORT_SUPPORT = 148,
	WMI_TLV_SERVICE_LISTEN_INTERVAL_OFFLOAD_SUPPORT = 149,
	WMI_TLV_SERVICE_BSS_COLOR_OFFLOAD = 150,
	WMI_TLV_SERVICE_RUNTIME_DPD_RECAL = 151,
	WMI_TLV_SERVICE_STA_TWT = 152,
	WMI_TLV_SERVICE_AP_TWT = 153,
	WMI_TLV_SERVICE_GMAC_OFFLOAD_SUPPORT = 154,
	WMI_TLV_SERVICE_SPOOF_MAC_SUPPORT = 155,
	WMI_TLV_SERVICE_PEER_TID_CONFIGS_SUPPORT = 156,
	WMI_TLV_SERVICE_VDEV_SWRETRY_PER_AC_CONFIG_SUPPORT = 157,
	WMI_TLV_SERVICE_DUAL_BEACON_ON_SINGLE_MAC_SCC_SUPPORT = 158,
	WMI_TLV_SERVICE_DUAL_BEACON_ON_SINGLE_MAC_MCC_SUPPORT = 159,
	WMI_TLV_SERVICE_MOTION_DET = 160,
	WMI_TLV_SERVICE_INFRA_MBSSID = 161,
	WMI_TLV_SERVICE_OBSS_SPATIAL_REUSE = 162,
	WMI_TLV_SERVICE_VDEV_DIFFERENT_BEACON_INTERVAL_SUPPORT = 163,
	WMI_TLV_SERVICE_NAN_DBS_SUPPORT = 164,
	WMI_TLV_SERVICE_NDI_DBS_SUPPORT = 165,
	WMI_TLV_SERVICE_NAN_SAP_SUPPORT = 166,
	WMI_TLV_SERVICE_NDI_SAP_SUPPORT = 167,
	WMI_TLV_SERVICE_CFR_CAPTURE_SUPPORT = 168,
	WMI_TLV_SERVICE_CFR_CAPTURE_IND_MSG_TYPE_1 = 169,
	WMI_TLV_SERVICE_ESP_SUPPORT = 170,
	WMI_TLV_SERVICE_PEER_CHWIDTH_CHANGE = 171,
	WMI_TLV_SERVICE_WLAN_HPCS_PULSE = 172,
	WMI_TLV_SERVICE_PER_VDEV_CHAINMASK_CONFIG_SUPPORT = 173,
	WMI_TLV_SERVICE_TX_DATA_MGMT_ACK_RSSI = 174,
	WMI_TLV_SERVICE_NAN_DISABLE_SUPPORT = 175,
	WMI_TLV_SERVICE_HTT_H2T_NO_HTC_HDR_LEN_IN_MSG_LEN = 176,
	WMI_TLV_SERVICE_COEX_SUPPORT_UNEQUAL_ISOLATION = 177,
	WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT = 178,
	WMI_TLV_SERVICE_SUPPORT_EXTEND_ADDRESS = 179,

	WMI_TLV_MAX_EXT_SERVICE = 256,
};

struct wmi_tlv {
	__le16 len;
	__le16 tag;
	u8 value[];
} __packed;

#define WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL	BIT(9)

struct wmi_tlv_mgmt_tx_compl_ev {
	__le32 desc_id;
	__le32 status;
	__le32 pdev_id;
	__le32 ppdu_id;
	__le32 ack_rssi;
};

#define WMI_TLV_MGMT_RX_NUM_RSSI 4

struct wmi_tlv_mgmt_rx_ev {
	__le32 channel;
	__le32 snr;
	__le32 rate;
	__le32 phy_mode;
	__le32 buf_len;
	__le32 status;
	__le32 rssi[WMI_TLV_MGMT_RX_NUM_RSSI];
} __packed;

struct wmi_tlv_abi_version {
	__le32 abi_ver0;
	__le32 abi_ver1;
	__le32 abi_ver_ns0;
	__le32 abi_ver_ns1;
	__le32 abi_ver_ns2;
	__le32 abi_ver_ns3;
} __packed;

struct wmi_tlv_hw_bd_info {
	u8 rev;
	u8 project_id;
	u8 custom_id;
	u8 reference_design_id;
} __packed;

struct wmi_tlv_svc_rdy_ev {
	__le32 fw_build_vers;
	struct wmi_tlv_abi_version abi;
	__le32 phy_capability;
	__le32 max_frag_entry;
	__le32 num_rf_chains;
	__le32 ht_cap_info;
	__le32 vht_cap_info;
	__le32 vht_supp_mcs;
	__le32 hw_min_tx_power;
	__le32 hw_max_tx_power;
	__le32 sys_cap_info;
	__le32 min_pkt_size_enable;
	__le32 max_bcn_ie_size;
	__le32 num_mem_reqs;
	__le32 max_num_scan_chans;
	__le32 hw_bd_id; /* 0 means hw_bd_info is invalid */
	struct wmi_tlv_hw_bd_info hw_bd_info[5];
} __packed;

struct wmi_tlv_rdy_ev {
	struct wmi_tlv_abi_version abi;
	struct wmi_mac_addr mac_addr;
	__le32 status;
} __packed;

struct wmi_tlv_resource_config {
	__le32 num_vdevs;
	__le32 num_peers;
	__le32 num_offload_peers;
	__le32 num_offload_reorder_bufs;
	__le32 num_peer_keys;
	__le32 num_tids;
	__le32 ast_skid_limit;
	__le32 tx_chain_mask;
	__le32 rx_chain_mask;
	__le32 rx_timeout_pri[4];
	__le32 rx_decap_mode;
	__le32 scan_max_pending_reqs;
	__le32 bmiss_offload_max_vdev;
	__le32 roam_offload_max_vdev;
	__le32 roam_offload_max_ap_profiles;
	__le32 num_mcast_groups;
	__le32 num_mcast_table_elems;
	__le32 mcast2ucast_mode;
	__le32 tx_dbg_log_size;
	__le32 num_wds_entries;
	__le32 dma_burst_size;
	__le32 mac_aggr_delim;
	__le32 rx_skip_defrag_timeout_dup_detection_check;
	__le32 vow_config;
	__le32 gtk_offload_max_vdev;
	__le32 num_msdu_desc;
	__le32 max_frag_entries;
	__le32 num_tdls_vdevs;
	__le32 num_tdls_conn_table_entries;
	__le32 beacon_tx_offload_max_vdev;
	__le32 num_multicast_filter_entries;
	__le32 num_wow_filters;
	__le32 num_keep_alive_pattern;
	__le32 keep_alive_pattern_size;
	__le32 max_tdls_concurrent_sleep_sta;
	__le32 max_tdls_concurrent_buffer_sta;
	__le32 wmi_send_separate;
	__le32 num_ocb_vdevs;
	__le32 num_ocb_channels;
	__le32 num_ocb_schedules;
	__le32 host_capab;
} __packed;

struct host_memory_chunk_tlv {
	/* id of the request that is passed up in service ready */
	__le32 req_id;

	/* the physical address the memory chunk */
	__le32 ptr;

	/* size of the chunk */
	__le32 size;

	/* the upper 32 bit address valid only for more than 32 bit target */
	__le32 ptr_high;
} __packed;

struct wmi_tlv_init_cmd {
	struct wmi_tlv_abi_version abi;
	__le32 num_host_mem_chunks;
} __packed;

struct wmi_tlv_pdev_set_param_cmd {
	__le32 pdev_id; /* not used yet */
	__le32 param_id;
	__le32 param_value;
} __packed;

struct wmi_tlv_pdev_set_rd_cmd {
	__le32 pdev_id; /* not used yet */
	__le32 regd;
	__le32 regd_2ghz;
	__le32 regd_5ghz;
	__le32 conform_limit_2ghz;
	__le32 conform_limit_5ghz;
} __packed;

struct wmi_tlv_scan_chan_list_cmd {
	__le32 num_scan_chans;
} __packed;

struct wmi_tlv_start_scan_cmd {
	struct wmi_start_scan_common common;
	__le32 burst_duration_ms;
	__le32 num_channels;
	__le32 num_bssids;
	__le32 num_ssids;
	__le32 ie_len;
	__le32 num_probes;
	struct wmi_mac_addr mac_addr;
	struct wmi_mac_addr mac_mask;
} __packed;

enum wmi_tlv_vdev_subtype {
	WMI_TLV_VDEV_SUBTYPE_NONE	= 0,
	WMI_TLV_VDEV_SUBTYPE_P2P_DEV	= 1,
	WMI_TLV_VDEV_SUBTYPE_P2P_CLI	= 2,
	WMI_TLV_VDEV_SUBTYPE_P2P_GO	= 3,
	WMI_TLV_VDEV_SUBTYPE_PROXY_STA	= 4,
	WMI_TLV_VDEV_SUBTYPE_MESH	= 5,
	WMI_TLV_VDEV_SUBTYPE_MESH_11S	= 6,
};

struct wmi_tlv_vdev_start_cmd {
	__le32 vdev_id;
	__le32 requestor_id;
	__le32 bcn_intval;
	__le32 dtim_period;
	__le32 flags;
	struct wmi_ssid ssid;
	__le32 bcn_tx_rate;
	__le32 bcn_tx_power;
	__le32 num_noa_descr;
	__le32 disable_hw_ack;
} __packed;

struct wmi_tlv_peer_create_cmd {
	__le32 vdev_id;
	struct wmi_mac_addr peer_addr;
	__le32 peer_type;
} __packed;

struct wmi_tlv_peer_assoc_cmd {
	struct wmi_mac_addr mac_addr;
	__le32 vdev_id;
	__le32 new_assoc;
	__le32 assoc_id;
	__le32 flags;
	__le32 caps;
	__le32 listen_intval;
	__le32 ht_caps;
	__le32 max_mpdu;
	__le32 mpdu_density;
	__le32 rate_caps;
	__le32 nss;
	__le32 vht_caps;
	__le32 phy_mode;
	__le32 ht_info[2];
	__le32 num_legacy_rates;
	__le32 num_ht_rates;
} __packed;

struct wmi_tlv_roam_ev {
	__le32 vdev_id;
	__le32 reason;
	__le32 rssi;
} __packed;

struct wmi_tlv_sta_keepalive_cmd {
	__le32 vdev_id;
	__le32 enabled;
	__le32 method; /* WMI_STA_KEEPALIVE_METHOD_ */
	__le32 interval; /* in seconds */
} __packed;

struct wmi_tlv_mgmt_tx_cmd {
	__le32 vdev_id;
	__le32 desc_id;
	__le32 chanfreq;
	__le64 paddr;
	__le32 frame_len;
	__le32 buf_len;
} __packed;

#define WMI_TLV_MGMT_TX_FRAME_MAX_LEN	64
#define WMI_RSRC_CFG_FLAG_TX_ACK_RSSI		BIT(18)


/* The host-target transport of the data (htt.h, rx_desc.h) */

enum htt_h2t_msg_type { /* host-to-target */
	HTT_H2T_MSG_TYPE_VERSION_REQ        = 0,
	HTT_H2T_MSG_TYPE_TX_FRM             = 1,
	HTT_H2T_MSG_TYPE_RX_RING_CFG        = 2,
	HTT_H2T_MSG_TYPE_STATS_REQ          = 3,
	HTT_H2T_MSG_TYPE_SYNC               = 4,
	HTT_H2T_MSG_TYPE_AGGR_CFG           = 5,
	HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG = 6,

	/* This command is used for sending management frames in HTT < 3.0.
	 * HTT >= 3.0 uses TX_FRM for everything.
	 */
	HTT_H2T_MSG_TYPE_MGMT_TX            = 7,
	HTT_H2T_MSG_TYPE_TX_FETCH_RESP      = 11,

	HTT_H2T_NUM_MSGS /* keep this last */
};

struct htt_cmd_hdr {
	u8 msg_type;
} __packed;

struct htt_ver_req {
	u8 pad[sizeof(u32) - sizeof(struct htt_cmd_hdr)];
} __packed;

struct htt_data_tx_desc_frag {
	union {
		struct double_word_addr {
			__le32 paddr;
			__le32 len;
		} __packed dword_addr;
		struct triple_word_addr {
			__le32 paddr_lo;
			__le16 paddr_hi;
			__le16 len_16;
		} __packed tword_addr;
	} __packed;
} __packed;

enum htt_data_tx_desc_flags0 {
	HTT_DATA_TX_DESC_FLAGS0_MAC_HDR_PRESENT = 1 << 0,
	HTT_DATA_TX_DESC_FLAGS0_NO_AGGR         = 1 << 1,
	HTT_DATA_TX_DESC_FLAGS0_NO_ENCRYPT      = 1 << 2,
	HTT_DATA_TX_DESC_FLAGS0_NO_CLASSIFY     = 1 << 3,
	HTT_DATA_TX_DESC_FLAGS0_RSVD0           = 1 << 4
#define HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE_MASK 0xE0
#define HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE_LSB 5
};

enum htt_data_tx_desc_flags1 {
#define HTT_DATA_TX_DESC_FLAGS1_VDEV_ID_BITS 6
#define HTT_DATA_TX_DESC_FLAGS1_VDEV_ID_MASK 0x003F
#define HTT_DATA_TX_DESC_FLAGS1_VDEV_ID_LSB  0
#define HTT_DATA_TX_DESC_FLAGS1_EXT_TID_BITS 5
#define HTT_DATA_TX_DESC_FLAGS1_EXT_TID_MASK 0x07C0
#define HTT_DATA_TX_DESC_FLAGS1_EXT_TID_LSB  6
	HTT_DATA_TX_DESC_FLAGS1_POSTPONED        = 1 << 11,
	HTT_DATA_TX_DESC_FLAGS1_MORE_IN_BATCH    = 1 << 12,
	HTT_DATA_TX_DESC_FLAGS1_CKSUM_L3_OFFLOAD = 1 << 13,
	HTT_DATA_TX_DESC_FLAGS1_CKSUM_L4_OFFLOAD = 1 << 14,
	HTT_DATA_TX_DESC_FLAGS1_TX_COMPLETE      = 1 << 15
};

enum htt_data_tx_ext_tid {
	HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST = 16,
	HTT_DATA_TX_EXT_TID_MGMT                = 17,
	HTT_DATA_TX_EXT_TID_INVALID             = 31
};

#define HTT_INVALID_PEERID 0xFFFF

struct htt_data_tx_desc {
	u8 flags0; /* %HTT_DATA_TX_DESC_FLAGS0_ */
	__le16 flags1; /* %HTT_DATA_TX_DESC_FLAGS1_ */
	__le16 len;
	__le16 id;
	__le32 frags_paddr;
	union {
		__le32 peerid;
		struct {
			__le16 peerid;
			__le16 freq;
		} __packed offchan_tx;
	} __packed;
	u8 prefetch[0]; /* start of frame, for FW classification engine */
} __packed;

enum htt_rx_ring_flags {
	HTT_RX_RING_FLAGS_MAC80211_HDR = 1 << 0,
	HTT_RX_RING_FLAGS_MSDU_PAYLOAD = 1 << 1,
	HTT_RX_RING_FLAGS_PPDU_START   = 1 << 2,
	HTT_RX_RING_FLAGS_PPDU_END     = 1 << 3,
	HTT_RX_RING_FLAGS_MPDU_START   = 1 << 4,
	HTT_RX_RING_FLAGS_MPDU_END     = 1 << 5,
	HTT_RX_RING_FLAGS_MSDU_START   = 1 << 6,
	HTT_RX_RING_FLAGS_MSDU_END     = 1 << 7,
	HTT_RX_RING_FLAGS_RX_ATTENTION = 1 << 8,
	HTT_RX_RING_FLAGS_FRAG_INFO    = 1 << 9,
	HTT_RX_RING_FLAGS_UNICAST_RX   = 1 << 10,
	HTT_RX_RING_FLAGS_MULTICAST_RX = 1 << 11,
	HTT_RX_RING_FLAGS_CTRL_RX      = 1 << 12,
	HTT_RX_RING_FLAGS_MGMT_RX      = 1 << 13,
	HTT_RX_RING_FLAGS_NULL_RX      = 1 << 14,
	HTT_RX_RING_FLAGS_PHY_DATA_RX  = 1 << 15
};

#define HTT_RX_RING_SIZE_MIN 128
#define HTT_RX_RING_SIZE_MAX 2048
#define HTT_RX_RING_SIZE HTT_RX_RING_SIZE_MAX
#define HTT_RX_RING_FILL_LEVEL (((HTT_RX_RING_SIZE) / 2) - 1)

struct htt_rx_ring_rx_desc_offsets {
	/* the following offsets are in 4-byte units */
	__le16 mac80211_hdr_offset;
	__le16 msdu_payload_offset;
	__le16 ppdu_start_offset;
	__le16 ppdu_end_offset;
	__le16 mpdu_start_offset;
	__le16 mpdu_end_offset;
	__le16 msdu_start_offset;
	__le16 msdu_end_offset;
	__le16 rx_attention_offset;
	__le16 frag_info_offset;
} __packed;

struct htt_rx_ring_setup_ring32 {
	__le32 fw_idx_shadow_reg_paddr;
	__le32 rx_ring_base_paddr;
	__le16 rx_ring_len; /* in 4-byte words */
	__le16 rx_ring_bufsize; /* rx skb size - in bytes */
	__le16 flags; /* %HTT_RX_RING_FLAGS_ */
	__le16 fw_idx_init_val;

	struct htt_rx_ring_rx_desc_offsets offsets;
} __packed;

struct htt_rx_ring_setup_hdr {
	u8 num_rings; /* supported values: 1, 2 */
	__le16 rsvd0;
} __packed;

struct htt_rx_ring_setup_32 {
	struct htt_rx_ring_setup_hdr hdr;
	struct htt_rx_ring_setup_ring32 rings[];
} __packed;

struct htt_aggr_conf {
	u8 max_num_ampdu_subframes;
	/* amsdu_subframes is limited by 0x1F mask */
	u8 max_num_amsdu_subframes;
} __packed;

enum htt_mgmt_tx_status {
	HTT_MGMT_TX_STATUS_OK    = 0,
	HTT_MGMT_TX_STATUS_RETRY = 1,
	HTT_MGMT_TX_STATUS_DROP  = 2
};

enum htt_tlv_t2h_msg_type {
	HTT_TLV_T2H_MSG_TYPE_VERSION_CONF              = 0x0,
	HTT_TLV_T2H_MSG_TYPE_RX_IND                    = 0x1,
	HTT_TLV_T2H_MSG_TYPE_RX_FLUSH                  = 0x2,
	HTT_TLV_T2H_MSG_TYPE_PEER_MAP                  = 0x3,
	HTT_TLV_T2H_MSG_TYPE_PEER_UNMAP                = 0x4,
	HTT_TLV_T2H_MSG_TYPE_RX_ADDBA                  = 0x5,
	HTT_TLV_T2H_MSG_TYPE_RX_DELBA                  = 0x6,
	HTT_TLV_T2H_MSG_TYPE_TX_COMPL_IND              = 0x7,
	HTT_TLV_T2H_MSG_TYPE_PKTLOG                    = 0x8,
	HTT_TLV_T2H_MSG_TYPE_STATS_CONF                = 0x9,
	HTT_TLV_T2H_MSG_TYPE_RX_FRAG_IND               = 0xa,
	HTT_TLV_T2H_MSG_TYPE_SEC_IND                   = 0xb,
	HTT_TLV_T2H_MSG_TYPE_RC_UPDATE_IND             = 0xc, /* deprecated */
	HTT_TLV_T2H_MSG_TYPE_TX_INSPECT_IND            = 0xd,
	HTT_TLV_T2H_MSG_TYPE_MGMT_TX_COMPL_IND         = 0xe,
	HTT_TLV_T2H_MSG_TYPE_TX_CREDIT_UPDATE_IND      = 0xf,
	HTT_TLV_T2H_MSG_TYPE_RX_PN_IND                 = 0x10,
	HTT_TLV_T2H_MSG_TYPE_RX_OFFLOAD_DELIVER_IND    = 0x11,
	HTT_TLV_T2H_MSG_TYPE_RX_IN_ORD_PADDR_IND       = 0x12,
	/* 0x13 reservd */
	HTT_TLV_T2H_MSG_TYPE_WDI_IPA_OP_RESPONSE       = 0x14,
	HTT_TLV_T2H_MSG_TYPE_CHAN_CHANGE               = 0x15,
	HTT_TLV_T2H_MSG_TYPE_RX_OFLD_PKT_ERR           = 0x16,
	HTT_TLV_T2H_MSG_TYPE_TEST,
	/* keep this last */
	HTT_TLV_T2H_NUM_MSGS
};

struct htt_resp_hdr {
	u8 msg_type;
} __packed;

struct htt_ver_resp {
	u8 minor;
	u8 major;
	u8 rsvd0;
} __packed;

struct htt_mgmt_tx_completion {
	u8 rsvd0;
	u8 rsvd1;
	u8 flags;
	__le32 desc_id;
	__le32 status;
	__le32 ppdu_id;
	__le32 info;
} __packed;

#define HTT_RX_INDICATION_INFO0_EXT_TID_MASK  (0x1F)
#define HTT_RX_INDICATION_INFO0_EXT_TID_LSB   (0)
#define HTT_RX_INDICATION_INFO1_NUM_MPDU_RANGES_MASK     0xFF000000
#define HTT_RX_INDICATION_INFO1_NUM_MPDU_RANGES_LSB      24

struct htt_rx_indication_hdr {
	u8 info0; /* %HTT_RX_INDICATION_INFO0_ */
	__le16 peer_id;
	__le32 info1; /* %HTT_RX_INDICATION_INFO1_ */
} __packed;

struct htt_rx_indication_ppdu {
	u8 combined_rssi;
	u8 sub_usec_timestamp;
	u8 phy_err_code;
	u8 info0; /* HTT_RX_INDICATION_INFO0_ */
	struct {
		u8 pri20_db;
		u8 ext20_db;
		u8 ext40_db;
		u8 ext80_db;
	} __packed rssi_chains[4];
	__le32 tsf;
	__le32 usec_timestamp;
	__le32 info1; /* HTT_RX_INDICATION_INFO1_ */
	__le32 info2; /* HTT_RX_INDICATION_INFO2_ */
} __packed;

struct htt_rx_indication_mpdu_range {
	u8 mpdu_count;
	u8 mpdu_range_status; /* %htt_rx_mpdu_status */
	u8 pad0;
	u8 pad1;
} __packed;

struct htt_rx_indication_prefix {
	__le16 fw_rx_desc_bytes;
	u8 pad0;
	u8 pad1;
} __packed;

struct fw_rx_desc_base {
	u8 info0;
} __packed;

struct htt_rx_indication {
	struct htt_rx_indication_hdr hdr;
	struct htt_rx_indication_ppdu ppdu;
	struct htt_rx_indication_prefix prefix;

	/*
	 * the following fields are both dynamically sized, so
	 * take care addressing them
	 */

	/* the size of this is %fw_rx_desc_bytes */
	struct fw_rx_desc_base fw_desc;

	/*
	 * %mpdu_ranges starts after &%prefix + roundup(%fw_rx_desc_bytes, 4)
	 * and has %num_mpdu_ranges elements.
	 */
	struct htt_rx_indication_mpdu_range mpdu_ranges[];
} __packed;

struct htt_rx_peer_map {
	u8 vdev_id;
	__le16 peer_id;
	u8 addr[6];
	u8 rsvd0;
	u8 rsvd1;
} __packed;

struct htt_rx_peer_unmap {
	u8 rsvd0;
	__le16 peer_id;
} __packed;

enum htt_security_types {
	HTT_SECURITY_NONE,
	HTT_SECURITY_WEP128,
	HTT_SECURITY_WEP104,
	HTT_SECURITY_WEP40,
	HTT_SECURITY_TKIP,
	HTT_SECURITY_TKIP_NOMIC,
	HTT_SECURITY_AES_CCMP,
	HTT_SECURITY_WAPI,

	HTT_NUM_SECURITY_TYPES /* keep this last! */
};

enum htt_security_flags {
#define HTT_SECURITY_TYPE_MASK 0x7F
#define HTT_SECURITY_TYPE_LSB  0
	HTT_SECURITY_IS_UNICAST = 1 << 7
};

struct htt_security_indication {
	union {
		/* dont use bitfields; undefined behaviour */
		u8 flags; /* %htt_security_flags */
		struct {
			u8 security_type:7, /* %htt_security_types */
			   is_unicast:1;
		} __packed;
	} __packed;
	__le16 peer_id;
	u8 michael_key[8];
	u8 wapi_rsc[16];
} __packed;

struct htt_rx_addba {
	u8 window_size;
	__le16 info0; /* %HTT_RX_BA_INFO0_ */
} __packed;

struct htt_rx_delba {
	u8 rsvd0;
	__le16 info0; /* %HTT_RX_BA_INFO0_ */
} __packed;

#define HTT_RX_BA_INFO0_TID_MASK     0x000F
#define HTT_RX_BA_INFO0_TID_LSB      0
#define HTT_RX_BA_INFO0_PEER_ID_MASK 0xFFF0
#define HTT_RX_BA_INFO0_PEER_ID_LSB  4

enum htt_data_tx_status {
	HTT_DATA_TX_STATUS_OK            = 0,
	HTT_DATA_TX_STATUS_DISCARD       = 1,
	HTT_DATA_TX_STATUS_NO_ACK        = 2,
	HTT_DATA_TX_STATUS_POSTPONE      = 3 /* HL only */
};

enum htt_data_tx_flags {
#define HTT_DATA_TX_STATUS_MASK 0x07
#define HTT_DATA_TX_STATUS_LSB  0
#define HTT_DATA_TX_TID_MASK    0x78
#define HTT_DATA_TX_TID_LSB     3
	HTT_DATA_TX_TID_INVALID = 1 << 7
};

#define HTT_TX_COMPL_INV_MSDU_ID 0xFFFF

struct htt_data_tx_completion {
	union {
		u8 flags;
		struct {
			u8 status:3,
			   tid:4,
			   tid_invalid:1;
		} __packed;
	} __packed;
	u8 num_msdus;
	u8 flags2; /* HTT_TX_CMPL_FLAG_DATA_RSSI */
	__le16 msdus[]; /* variable length based on %num_msdus */
} __packed;

struct ath10k_htt_txbuf_32 {
	struct htt_data_tx_desc_frag frags[2];
	struct ath10k_htc_hdr htc_hdr;
	struct htt_cmd_hdr cmd_hdr;
	struct htt_data_tx_desc cmd_tx;
} __packed __aligned(4);

#define RX_HTT_HDR_STATUS_LEN 64

enum rx_attention_flags {
	RX_ATTENTION_FLAGS_FIRST_MPDU          = BIT(0),
	RX_ATTENTION_FLAGS_LAST_MPDU           = BIT(1),
	RX_ATTENTION_FLAGS_MCAST_BCAST         = BIT(2),
	RX_ATTENTION_FLAGS_PEER_IDX_INVALID    = BIT(3),
	RX_ATTENTION_FLAGS_PEER_IDX_TIMEOUT    = BIT(4),
	RX_ATTENTION_FLAGS_POWER_MGMT          = BIT(5),
	RX_ATTENTION_FLAGS_NON_QOS             = BIT(6),
	RX_ATTENTION_FLAGS_NULL_DATA           = BIT(7),
	RX_ATTENTION_FLAGS_MGMT_TYPE           = BIT(8),
	RX_ATTENTION_FLAGS_CTRL_TYPE           = BIT(9),
	RX_ATTENTION_FLAGS_MORE_DATA           = BIT(10),
	RX_ATTENTION_FLAGS_EOSP                = BIT(11),
	RX_ATTENTION_FLAGS_U_APSD_TRIGGER      = BIT(12),
	RX_ATTENTION_FLAGS_FRAGMENT            = BIT(13),
	RX_ATTENTION_FLAGS_ORDER               = BIT(14),
	RX_ATTENTION_FLAGS_CLASSIFICATION      = BIT(15),
	RX_ATTENTION_FLAGS_OVERFLOW_ERR        = BIT(16),
	RX_ATTENTION_FLAGS_MSDU_LENGTH_ERR     = BIT(17),
	RX_ATTENTION_FLAGS_TCP_UDP_CHKSUM_FAIL = BIT(18),
	RX_ATTENTION_FLAGS_IP_CHKSUM_FAIL      = BIT(19),
	RX_ATTENTION_FLAGS_SA_IDX_INVALID      = BIT(20),
	RX_ATTENTION_FLAGS_DA_IDX_INVALID      = BIT(21),
	RX_ATTENTION_FLAGS_SA_IDX_TIMEOUT      = BIT(22),
	RX_ATTENTION_FLAGS_DA_IDX_TIMEOUT      = BIT(23),
	RX_ATTENTION_FLAGS_ENCRYPT_REQUIRED    = BIT(24),
	RX_ATTENTION_FLAGS_DIRECTED            = BIT(25),
	RX_ATTENTION_FLAGS_BUFFER_FRAGMENT     = BIT(26),
	RX_ATTENTION_FLAGS_MPDU_LENGTH_ERR     = BIT(27),
	RX_ATTENTION_FLAGS_TKIP_MIC_ERR        = BIT(28),
	RX_ATTENTION_FLAGS_DECRYPT_ERR         = BIT(29),
	RX_ATTENTION_FLAGS_FCS_ERR             = BIT(30),
	RX_ATTENTION_FLAGS_MSDU_DONE           = BIT(31),
};

struct rx_attention {
	__le32 flags; /* %RX_ATTENTION_FLAGS_ */
} __packed;

struct rx_frag_info_common {
	u8 ring0_more_count;
	u8 ring1_more_count;
	u8 ring2_more_count;
	u8 ring3_more_count;
} __packed;

struct rx_frag_info_v1 {
	struct rx_frag_info_common common;
} __packed;

enum htt_rx_mpdu_encrypt_type {
	HTT_RX_MPDU_ENCRYPT_WEP40            = 0,
	HTT_RX_MPDU_ENCRYPT_WEP104           = 1,
	HTT_RX_MPDU_ENCRYPT_TKIP_WITHOUT_MIC = 2,
	HTT_RX_MPDU_ENCRYPT_WEP128           = 3,
	HTT_RX_MPDU_ENCRYPT_TKIP_WPA         = 4,
	HTT_RX_MPDU_ENCRYPT_WAPI             = 5,
	HTT_RX_MPDU_ENCRYPT_AES_CCM_WPA2     = 6,
	HTT_RX_MPDU_ENCRYPT_NONE             = 7,
	HTT_RX_MPDU_ENCRYPT_AES_CCM256_WPA2  = 8,
	HTT_RX_MPDU_ENCRYPT_AES_GCMP_WPA2    = 9,
	HTT_RX_MPDU_ENCRYPT_AES_GCMP256_WPA2 = 10,
};

#define RX_MPDU_START_INFO0_PEER_IDX_MASK     0x000007ff
#define RX_MPDU_START_INFO0_PEER_IDX_LSB      0
#define RX_MPDU_START_INFO0_SEQ_NUM_MASK      0x0fff0000
#define RX_MPDU_START_INFO0_SEQ_NUM_LSB       16
#define RX_MPDU_START_INFO0_ENCRYPT_TYPE_MASK 0xf0000000
#define RX_MPDU_START_INFO0_ENCRYPT_TYPE_LSB  28
#define RX_MPDU_START_INFO1_TID_MASK 0xf0000000
#define RX_MPDU_START_INFO1_TID_LSB  28

struct rx_mpdu_start {
	__le32 info0;
	union {
		struct {
			__le32 pn31_0;
			__le32 info1; /* %RX_MPDU_START_INFO1_ */
		} __packed;
		struct {
			u8 pn[6];
		} __packed;
	} __packed;
} __packed;

struct rx_mpdu_end {
	__le32 info0;
} __packed;

enum rx_msdu_decap_format {
	RX_MSDU_DECAP_RAW = 0,

	/* Note: QoS frames are reported as non-QoS. The rx_hdr_status in
	 * htt_rx_desc contains the original decapped 802.11 header.
	 */
	RX_MSDU_DECAP_NATIVE_WIFI = 1,

	/* Payload contains an ethernet header (struct ethhdr). */
	RX_MSDU_DECAP_ETHERNET2_DIX = 2,

	/* Payload contains two 48-bit addresses and 2-byte length (14 bytes
	 * total), followed by an RFC1042 header (8 bytes).
	 */
	RX_MSDU_DECAP_8023_SNAP_LLC = 3
};

#define RX_MSDU_START_INFO0_MSDU_LENGTH_MASK    0x00003fff
#define RX_MSDU_START_INFO0_MSDU_LENGTH_LSB     0
#define RX_MSDU_START_INFO1_DECAP_FORMAT_MASK   0x00000300
#define RX_MSDU_START_INFO1_DECAP_FORMAT_LSB    8

struct rx_msdu_start_common {
	__le32 info0; /* %RX_MSDU_START_INFO0_ */
	__le32 flow_id_crc;
	__le32 info1; /* %RX_MSDU_START_INFO1_ */
} __packed;

struct rx_msdu_start_qca99x0 {
	__le32 info2; /* %RX_MSDU_START_INFO2_ */
} __packed;

struct rx_msdu_start_v1 {
	struct rx_msdu_start_common common;
	union {
		struct rx_msdu_start_qca99x0 qca99x0;
	} __packed;
} __packed;

#define RX_MSDU_END_INFO0_FIRST_MSDU                BIT(14)
#define RX_MSDU_END_INFO0_LAST_MSDU                 BIT(15)

struct rx_msdu_end_common {
	__le16 ip_hdr_cksum;
	__le16 tcp_hdr_cksum;
	u8 key_id_octet;
	u8 classification_filter;
	u8 wapi_pn[10];
	__le32 info0;
} __packed;

struct rx_msdu_end_qca99x0 {
	__le32 ipv6_crc;
	__le32 tcp_seq_no;
	__le32 tcp_ack_no;
	__le32 info1;
	__le32 info2;
} __packed;

struct rx_msdu_end_v1 {
	struct rx_msdu_end_common common;
	union {
		struct rx_msdu_end_qca99x0 qca99x0;
	} __packed;
} __packed;

struct rx_ppdu_start {
	struct {
		u8 pri20_mhz;
		u8 ext20_mhz;
		u8 ext40_mhz;
		u8 ext80_mhz;
	} rssi_chains[4];
	u8 rssi_comb;
	__le16 rsvd0;
	u8 info0; /* %RX_PPDU_START_INFO0_ */
	__le32 info1; /* %RX_PPDU_START_INFO1_ */
	__le32 info2; /* %RX_PPDU_START_INFO2_ */
	__le32 info3; /* %RX_PPDU_START_INFO3_ */
	__le32 info4; /* %RX_PPDU_START_INFO4_ */
	__le32 info5; /* %RX_PPDU_START_INFO5_ */
} __packed;

struct rx_ppdu_end_common {
	__le32 evm_p0;
	__le32 evm_p1;
	__le32 evm_p2;
	__le32 evm_p3;
	__le32 evm_p4;
	__le32 evm_p5;
	__le32 evm_p6;
	__le32 evm_p7;
	__le32 evm_p8;
	__le32 evm_p9;
	__le32 evm_p10;
	__le32 evm_p11;
	__le32 evm_p12;
	__le32 evm_p13;
	__le32 evm_p14;
	__le32 evm_p15;
	__le32 tsf_timestamp;
	__le32 wb_timestamp;
} __packed;

struct rx_ppdu_end_qca988x {
	u8 locationing_timestamp;
	u8 phy_err_code;
	__le16 flags; /* %RX_PPDU_END_FLAGS_ */
	__le32 info0; /* %RX_PPDU_END_INFO0_ */
	__le16 bb_length;
	__le16 info1; /* %RX_PPDU_END_INFO1_ */
} __packed;

struct rx_ppdu_end_qca6174 {
	u8 locationing_timestamp;
	u8 phy_err_code;
	__le16 flags; /* %RX_PPDU_END_FLAGS_ */
	__le32 info0; /* %RX_PPDU_END_INFO0_ */
	__le32 rtt; /* %RX_PPDU_END_RTT_ */
	__le16 bb_length;
	__le16 info1; /* %RX_PPDU_END_INFO1_ */
} __packed;

struct rx_pkt_end {
	__le32 info0; /* %RX_PKT_END_INFO0_ */
	__le32 phy_timestamp_1;
	__le32 phy_timestamp_2;
} __packed;

struct rx_phy_ppdu_end {
	__le32 info0; /* %RX_PHY_PPDU_END_INFO0_ */
	__le32 info1; /* %RX_PHY_PPDU_END_INFO1_ */
} __packed;

struct rx_location_info {
	__le32 rx_location_info0; /* %RX_LOCATION_INFO0_ */
	__le32 rx_location_info1; /* %RX_LOCATION_INFO1_ */
} __packed;

struct rx_ppdu_end_qca99x0 {
	struct rx_pkt_end rx_pkt_end;
	__le32 rx_location_info; /* %RX_LOCATION_INFO_ */
	struct rx_phy_ppdu_end rx_phy_ppdu_end;
	__le32 rx_timing_offset; /* %RX_PPDU_END_RX_TIMING_OFFSET_ */
	__le32 rx_info; /* %RX_PPDU_END_RX_INFO_ */
	__le16 bb_length;
	__le16 info1; /* %RX_PPDU_END_INFO1_ */
} __packed;

struct rx_ppdu_end_qca9984 {
	struct rx_pkt_end rx_pkt_end;
	struct rx_location_info rx_location_info;
	struct rx_phy_ppdu_end rx_phy_ppdu_end;
	__le32 rx_timing_offset; /* %RX_PPDU_END_RX_TIMING_OFFSET_ */
	__le32 rx_info; /* %RX_PPDU_END_RX_INFO_ */
	__le16 bb_length;
	__le16 info1; /* %RX_PPDU_END_INFO1_ */
} __packed;

struct rx_ppdu_end_v1 {
	struct rx_ppdu_end_common common;
	union {
		struct rx_ppdu_end_qca988x qca988x;
		struct rx_ppdu_end_qca6174 qca6174;
		struct rx_ppdu_end_qca99x0 qca99x0;
		struct rx_ppdu_end_qca9984 qca9984;
	} __packed;
} __packed;

struct htt_rx_desc {
	union {
		/* This field is filled on the host using the msdu buffer
		 * from htt_rx_indication
		 */
		struct fw_rx_desc_base fw_desc;
		u32 pad;
	} __packed;
} __packed;

struct htt_rx_desc_v1 {
	struct htt_rx_desc base;
	struct {
		struct rx_attention attention;
		struct rx_frag_info_v1 frag_info;
		struct rx_mpdu_start mpdu_start;
		struct rx_msdu_start_v1 msdu_start;
		struct rx_msdu_end_v1 msdu_end;
		struct rx_mpdu_end mpdu_end;
		struct rx_ppdu_start ppdu_start;
		struct rx_ppdu_end_v1 ppdu_end;
	} __packed;
	u8 rx_hdr_status[RX_HTT_HDR_STATUS_LEN];
	u8 msdu_payload[];
};

#define HTT_RX_DESC_ALIGN 8
#define HTT_RX_BUF_SIZE 2048
#define ATH10K_HTT_MAX_NUM_REFILL 100
#define ATH10K_HTT_MAX_NUM_AMSDU_DEFAULT 3
#define ATH10K_HTT_MAX_NUM_AMPDU_DEFAULT 64

#endif /* __ATHXREG_H__ */
