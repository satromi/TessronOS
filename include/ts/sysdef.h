/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysdef.h
 *	The system's definition objects known to the code (design 18.15)
 *
 *	The definitions are objects in the store, as the files in etc/def
 *	(metadata and record 0 in xmlTAD); the code knows them only by these
 *	identities.
 */

#ifndef __TS_SYSDEF_H__
#define __TS_SYSDEF_H__

/* システム箱: the one object the system's boxes are linked from */
#define SYSDEF_SYSTEM_BOX	"01a0d8c4-5211-7c52-9e0a-3b61d27f4c85"

/*
 * The relationships of the links of システム (ob_fnd_sys): what the code
 * finds each of the system's objects by, the identity given here for it
 * being the default when the link is not there.
 */
#define SYSROLE_DEFS		"system.definitions"	/* SYSDEF_BOX */
#define SYSROLE_PROGS		"system.programs"	/* SYSDEF_PROG_BOX */
#define SYSROLE_ACCS		"system.accessories"	/* SYSDEF_ACC_BOX */
#define SYSROLE_FONTS		"system.fonts"		/* SYSDEF_FONT_BOX */
#define SYSROLE_WALLS		"system.wallpapers"	/* SYSDEF_WALL_BOX */
#define SYSROLE_LEARN		"system.learning"	/* SYSDEF_LEARN_BOX */
#define SYSROLE_DICTS		"system.dictionaries"	/* SYSDEF_DICT_BOX */
#define SYSROLE_XFCONV		"system.xfconv"		/* SYSDEF_XFCONV_BOX */
#define SYSROLE_CALENDAR	"system.calendar"	/* SYSDEF_CALENDAR */
#define SYSROLE_CONF_NET	"system.conf.net"	/* SYSDEF_CONF_NET */
#define SYSROLE_CONF_DEV	"system.conf.dev"	/* SYSDEF_CONF_DEV */
#define SYSROLE_CONF_USER	"system.conf.user"	/* SYSDEF_CONF_USER */
#define SYSROLE_CONF_ACC	"system.conf.accessories"	/* SYSDEF_CONF_ACC */
#define SYSROLE_SOUNDS		"system.sounds"		/* SYSDEF_SND_BOX */
#define SYSROLE_CERTS		"system.certs"		/* SYSDEF_CA_CERTS */
#define SYSROLE_ROOT		"system.root"		/* SYSDEF_ROOT */

/*
 * The system's 原紙箱: the one an installed program's templates are
 * linked from. It is one of the person's objects, with no identity
 * fixed here: when システム has no link of this role, there is none.
 */
#define SYSROLE_BASES		"system.templates"

/* The cabinet the desktop opens with (「BTRON」): what the user has starts there */
#define SYSDEF_CABINET		"0199ce83-6e83-7d42-8962-d2ad8c721654"

/* 定義箱: the boxes of definitions */
#define SYSDEF_BOX		"01a0d889-460b-7872-859f-505e29b0e9ce"

/* The boxes of what the system carries (design 18.12, 18.20) */
#define SYSDEF_PROG_BOX		"01a0d6d1-08cf-750a-8479-73bdcfc4823c"	/* プログラム箱 */
#define SYSDEF_ACC_BOX		"01a0d6d1-08d0-7fbe-b385-b6d0c36f2308"	/* 小物箱 */
#define SYSDEF_FONT_BOX		"01a0d6d1-08e3-78ea-97a3-532747a3f4d8"	/* 書体箱 */
#define SYSDEF_WALL_BOX		"01a0d6d1-08e4-74f4-ade8-3fe38cd87982"	/* 壁紙箱 */
#define SYSDEF_LEARN_BOX	"01a0d8c4-5210-7a20-9d31-6b0f2c4e8a17"	/* 学習箱 */
#define SYSDEF_DICT_BOX		"01a0d8c4-5215-7d08-b3f1-6a2e9c4d7b19"	/* 辞書箱 */

/*
 * bin: a plain object whose record 0 links to the commands -- programs
 * that run without a window of their own ("kind": "command"), on what
 * the console gives them (design 16.5.19). The console starts in it and
 * looks for a command there. Opened from the desktop, a command is run
 * in a console. Linked from the システム箱 for now: the object the
 * paths start from is still to be settled.
 */
#define SYSDEF_BIN		"01a0d8c4-5460-7a71-8c82-9fab0c1d5460"
#define SYSDEF_PROG_HELLO	"01a0d8c4-5462-7c93-8ea4-bc1d2e3f5462"	/* hello */
#define SYSDEF_PROG_V8PROG	"01a0d8c4-5464-7eb5-80c6-de3f405f5464"	/* v8prog (make BROWSER=1) */

/*
 * The tree of paths a POSIX program sees (design 5.8.1): plain objects
 * whose record 0 links to what is below them. "/" links to bin, dev,
 * home and tmp and is linked from the システム箱. dev links to the
 * device objects, made again by the kernel as devices come and go; home
 * to the cabinet the desktop opens with; tmp is emptied at each start.
 */
#define SYSDEF_ROOT		"01a0d8c4-5470-7a81-8c92-9fab0c1d5470"	/* / */
#define SYSDEF_DEV		"01a0d8c4-5471-7a82-8c93-9fab0c1d5471"	/* dev */
#define SYSDEF_HOME		"01a0d8c4-5472-7a83-8c94-9fab0c1d5472"	/* home */
#define SYSDEF_TMP		"01a0d8c4-5473-7a84-8c95-9fab0c1d5473"	/* tmp */

/* かな漢字変換の辞書: mozc's data set in record 1 (RT 12) */
#define SYSDEF_DICT_MOZC	"01a0d8c4-5216-7f4a-8c27-1d5b3e9a6c02"

/* ユーザ辞書: the words the person registered, as lines of text in record 1 (ts/udic.h) */
#define SYSDEF_DICT_USER	"01a0d8c4-5310-7a01-8b12-3c4d5e6f7b10"

/* 文字検索の辞書: readings, elements, codes and names of the characters, in record 1 (tools/gen_chardic.py) */
#define SYSDEF_DICT_CHAR	"01a0d8c4-5354-7d94-8a05-cd2e3f405454"

/* The clock: a program object whose record 1 is its executable */
#define SYSDEF_PROG_CLOCK	"01a0d8c4-5212-7a19-8d63-e04b5c9f2a71"
#define SYSDEF_PROG_CONSOLE	"01a0d8c4-5219-7c3e-8a41-2f6b9d0e5c11"	/* コンソール */
#define SYSDEF_PROG_SERIAL	"01a0d8c4-521a-7d52-9b17-4e8c2a6f0d23"	/* シリアル通信 */
#define SYSDEF_PROG_MICROSCRIPT	"01a0d8c4-521b-7e63-8c28-5f9d3b7a1e34"	/* マイクロスクリプト */
#define SYSDEF_PROG_XFCONV	"01a0d8c4-52b1-7c21-9a31-4f5e6d7c8b91"	/* ファイル変換 */
#define SYSDEF_XFCONV_BOX	"01a0d8c4-52b2-7d32-8b42-5a6f7e8d9ca2"	/* ファイル変換の箱: 取り込んだ箱の置き場 */
#define SYSDEF_PROG_BACKUP	"01a0d8c4-52b0-7d15-9a3c-4e6f8b2d1c07"	/* バックアップ */
#define SYSDEF_PROG_NETENV	"01a0d8c4-52b5-7a41-8c61-2d7e9f0a1b31"	/* ネットワーク設定 */
#define SYSDEF_PROG_SYSENV	"01a0d8c4-52b6-7b52-9d72-3e8fa01b2c42"	/* システム環境設定 */
#define SYSDEF_PROG_SNDMIX	"01a0d8c4-5440-7a51-8c62-7e8f90a15440"	/* 音量ミキサー */
#define SYSDEF_PROG_USERENV	"01a0d8c4-52b7-7c63-8e83-4f9a012c3d53"	/* ユーザ環境設定 */
#define SYSDEF_PROG_CALENDAR	"01a0d8c4-52d4-7b31-9c42-5d6e7f809a12"	/* 予定表 */
#define SYSDEF_PROG_CALC	"01a0d8c4-52e5-7e86-8f97-0a1b2c3d4e51"	/* 基本表計算 */
#define SYSDEF_PROG_KANATYPE	"01a0d8c4-52f0-7a01-8b12-3c4d5e6f7a90"	/* かな入力練習 */
#define SYSDEF_PROG_BEFC	"01a0d8c4-5500-7a01-8b12-3c4d5e6f7b50"	/* BeFC */
#define SYSDEF_PROG_BESFC	"01a0d8c4-5501-7a01-8b12-3c4d5e6f7b60"	/* BeSFC */
#define SYSDEF_PROG_KEYMAP	"01a0d8c4-52e8-7b19-9c2a-3d4e5f6a7b84"	/* キー配列 */
#define SYSDEF_PROG_SOFTKB	"01a0d8c4-54b0-7ac1-8cd2-9fab0c1d54b0"	/* ソフトウェアキーボード */
#define SYSDEF_PROG_WORDDIC	"01a0d8c4-5312-7c23-8d34-5e6f7b8c9d12"	/* 単語登録 */
#define SYSDEF_PROG_CALCU	"01a0d8c4-5320-7a41-8b52-6c7d8e9fa020"	/* 電卓 */
#define SYSDEF_PROG_SCRPRINT	"01a0d8c4-53d0-7a21-8b32-4c5d6e7f53d0"	/* 画面印刷 */
#define SYSDEF_PROG_TRAY	"01a0d8c4-5430-7a31-8b42-5c6d7e8f5430"	/* トレー */
#define SYSDEF_PROG_BASECOL	"01a0d8c4-5b92-7c53-8d64-9fab0c1d5b92"	/* 原紙集め */
#define SYSDEF_PROG_DISKFIX	"01a0d8c4-53e0-7a31-8c42-5d6e7f8053e0"	/* ディスク修復 */
#define SYSDEF_PROG_MCARD	"01a0d8c4-5330-7a51-8c62-7d8e9fab3030"	/* マイクロカード */
#define SYSDEF_PROG_CHARSRCH	"01a0d8c4-5350-7a61-8d72-9fab0c1d5050"	/* 文字検索 */
#define SYSDEF_PROG_ARCHIVE	"01a0d8c4-5370-7a71-8e82-9fab0c1d7070"	/* 書庫管理 */
#define SYSDEF_PROG_FQUIT	"01a0d8c4-5420-7a10-8b21-6c3d4e5f5420"	/* 強制終了 */
#define SYSDEF_PROG_SYSINST	"01a0d8c4-54c0-7a11-8c22-4d5e6f7054c0"	/* システム登録 (x64) */
#define SYSDEF_TMPL_ARCHIVE	"01a0d8c4-5372-7c93-8aa4-bc1d2e3f7272"	/* 書庫管理の原紙「空書庫」 */
#define SYSDEF_PROG_TECHO	"01a0d8c4-53c0-7a11-8b22-3c4d5e6f53c0"	/* 電子手帳 */
#define SYSDEF_TMPL_TECHO	"01a0d8c4-53c4-7e55-8f66-7a8b9cad53c4"	/* 電子手帳の原紙「手帳用紙」 */
#define SYSDEF_PROG_SLIDE	"01a0d8c4-52d5-7c42-8d53-6e7f8091ab23"	/* スライド (a process, application/slide) */
#define SYSDEF_PROG_RTMEDIA	"01a0d8c4-52da-7c34-9d45-be6f708192a3"	/* 実時間メディア: what the player of 実時間制御TAD sounds with */
#define SYSDEF_PROG_WPASUPP	"01a0d8c4-5a61-7b02-8c13-5e3f70819a61"	/* wpasupp: wpa_supplicant's process (WPA_MODE=proc) */

/* The system itself: a virtual device whose records say and change what the machine is (OB_SYS_*) */
#define SYSDEF_SYSTEM		"01a0d680-3aa6-7c52-9d6f-3e72a0b18f14"

/* The settings those three write, as lines of text in record 1 (include/ts/conf.h) */
#define SYSDEF_CONF_NET		"01a0d8c4-52c0-7a10-8b21-5e6f7a8b9c01"	/* ネットワークの設定 */
#define SYSDEF_CONF_DEV		"01a0d8c4-52c1-7b21-9c32-6f7a8b9cad12"	/* 機器の設定 */
#define SYSDEF_CONF_USER	"01a0d8c4-52c2-7c32-8d43-7a8b9cadbe23"	/* ユーザ情報 */
#define SYSDEF_CONF_ACC		"01a0d8c4-52cc-7c55-8d65-ae1f2a3b4c56"	/* 小物の設定 */

/*
 * The Wi-Fi of the x64 PC (design 9.6): its networks and their passwords,
 * which only the administrators read and write (the x64 build puts it on
 * the system volume, linked from the system box), and the firmware of
 * Intel's 8265 that iwm(4) loads, of the Wi-Fi 6 AX200, AX201 and AX210 that
 * iwx(4) loads, of MediaTek's MT7921 and MT7922 that mwx(4) loads, and of
 * Qualcomm's QCA6174 that athx loads with its boards' data (record 1 the file
 * as its maker publishes it)
 */
#define SYSDEF_CONF_WIFI	"01a0d8c4-5481-7b92-9db3-0abc1d2e5481"	/* Ｗｉ－Ｆｉの設定 */
#define SYSDEF_FW_IWM8265_36	"01a0d8c4-548a-7fab-8c2d-5e6f7081548a"	/* iwlwifi-8265-36.ucode, for OpenBSD's iwm (design 9.6.9) */
#define SYSDEF_FW_IWXCC_77	"01a0d8c4-548c-71cd-ae4f-708192a3548c"	/* iwlwifi-cc-a0-77.ucode, for OpenBSD's iwx (design 9.6.9) */
#define SYSDEF_FW_IWXQUB_77	"01a0d8c4-5490-7a01-8b10-0190a2b35490"	/* iwlwifi-Qu-b0-hr-b0-77.ucode */
#define SYSDEF_FW_IWXQUC_77	"01a0d8c4-5492-7a03-8b12-0392a2b35492"	/* iwlwifi-Qu-c0-hr-b0-77.ucode */
#define SYSDEF_FW_IWXQUZ_77	"01a0d8c4-5494-7a05-8b14-0594a2b35494"	/* iwlwifi-QuZ-a0-hr-b0-77.ucode */
#define SYSDEF_FW_IWXTY_77	"01a0d8c4-5496-7a07-8b16-0796a2b35496"	/* iwlwifi-ty-a0-gf-a0-77.ucode */
#define SYSDEF_FW_IWXTY_PNVM	"01a0d8c4-5498-7a09-8b18-0998a2b35498"	/* iwlwifi-ty-a0-gf-a0.pnvm */
#define SYSDEF_FW_IWXSO_77	"01a0d8c4-549a-7a0b-8b1a-0b9aa2b3549a"	/* iwlwifi-so-a0-gf-a0-77.ucode */
#define SYSDEF_FW_IWXSO_PNVM	"01a0d8c4-549c-7a0d-8b1c-0d9ca2b3549c"	/* iwlwifi-so-a0-gf-a0.pnvm */
#define SYSDEF_FW_IWXSOGF4_77	"01a0d8c4-54e0-7a1b-8b2a-1bc0a2b354e0"	/* iwlwifi-so-a0-gf4-a0-77.ucode */
#define SYSDEF_FW_IWXSOGF4_PNVM	"01a0d8c4-54e2-7a1d-8b2c-1dc2a2b354e2"	/* iwlwifi-so-a0-gf4-a0.pnvm */
#define SYSDEF_FW_IWXSOHR_77	"01a0d8c4-54e4-7a1f-8b2e-1fc4a2b354e4"	/* iwlwifi-so-a0-hr-b0-77.ucode */
#define SYSDEF_FW_IWXSOJF_77	"01a0d8c4-54e6-7a21-8b30-21c6a2b354e6"	/* iwlwifi-so-a0-jf-b0-77.ucode */
#define SYSDEF_FW_IWXQUBJF_77	"01a0d8c4-54e8-7a23-8b32-23c8a2b354e8"	/* iwlwifi-Qu-b0-jf-b0-77.ucode */
#define SYSDEF_FW_IWXQUCJF_77	"01a0d8c4-54ea-7a25-8b34-25caa2b354ea"	/* iwlwifi-Qu-c0-jf-b0-77.ucode */
#define SYSDEF_FW_IWXQUZJF_77	"01a0d8c4-54ec-7a27-8b36-27cca2b354ec"	/* iwlwifi-QuZ-a0-jf-b0-77.ucode */
#define SYSDEF_FW_IWM3160_17	"01a0d8c4-54ee-7a29-8b38-29cea2b354ee"	/* iwlwifi-3160-17.ucode */
#define SYSDEF_FW_IWM3168_29	"01a0d8c4-54f0-7a2b-8b3a-2bc0a2b354f0"	/* iwlwifi-3168-29.ucode */
#define SYSDEF_FW_IWM7260_17	"01a0d8c4-54f2-7a2d-8b3c-2dc2a2b354f2"	/* iwlwifi-7260-17.ucode */
#define SYSDEF_FW_IWM7265_17	"01a0d8c4-54f4-7a2f-8b3e-2fc4a2b354f4"	/* iwlwifi-7265-17.ucode */
#define SYSDEF_FW_IWM7265D_29	"01a0d8c4-54f6-7a31-8b40-31c6a2b354f6"	/* iwlwifi-7265D-29.ucode */
#define SYSDEF_FW_IWM8000C_36	"01a0d8c4-54f8-7a33-8b42-33c8a2b354f8"	/* iwlwifi-8000C-36.ucode */
#define SYSDEF_FW_IWM9000_46	"01a0d8c4-54fa-7a35-8b44-35caa2b354fa"	/* iwlwifi-9000-pu-b0-jf-b0-46.ucode */
#define SYSDEF_FW_IWM9260_46	"01a0d8c4-54fc-7a37-8b46-37cca2b354fc"	/* iwlwifi-9260-th-b0-jf-b0-46.ucode */
#define SYSDEF_FW_MWX7961_PATCH	"01a0d8c4-549e-7a0f-8b1e-0f9ea2b3549e"	/* WIFI_MT7961_patch_mcu_1_2_hdr.bin, for OpenBSD's mwx */
#define SYSDEF_FW_MWX7961_RAM	"01a0d8c4-54a0-7a11-8b20-11a0a2b354a0"	/* WIFI_RAM_CODE_MT7961_1.bin */
#define SYSDEF_FW_MWX7920_PATCH	"01a0d8c4-54a2-7a13-8b22-13a2a2b354a2"	/* WIFI_MT7961_patch_mcu_1a_2_hdr.bin */
#define SYSDEF_FW_MWX7920_RAM	"01a0d8c4-54a4-7a15-8b24-15a4a2b354a4"	/* WIFI_RAM_CODE_MT7961_1a.bin */
#define SYSDEF_FW_MWX7922_PATCH	"01a0d8c4-54a6-7a17-8b26-17a6a2b354a6"	/* WIFI_MT7922_patch_mcu_1_1_hdr.bin */
#define SYSDEF_FW_MWX7922_RAM	"01a0d8c4-54a8-7a19-8b28-19a8a2b354a8"	/* WIFI_RAM_CODE_MT7922_1.bin */
#define SYSDEF_FW_ATHX6174_FW	"01a0d8c4-55a0-7c01-8d10-01a0b2c355a0"	/* QCA6174 hw3.x firmware-6.bin, for TessronOS's athx */
#define SYSDEF_FW_ATHX6174_BD	"01a0d8c4-55a2-7c03-8d12-03a2b2c355a2"	/* QCA6174 hw3.x board-2.bin */

/*
 * The Wi-Fi of the Raspberry Pi 5 (CYW43455 on SDIO, design 9.6.8): the
 * firmware and the regulatory data (CLM) that bwfm(4) loads into the
 * chip (record 1 the files as linux-firmware publishes them), and the
 * settings of the board (NVRAM), a document whose paragraphs are the
 * lines of the text (record 0, xmlTAD). The Pi 5 build puts them on the
 * system volume, linked from the system box.
 */
#define SYSDEF_FW_BWFM43455	"01a0d8c4-5484-7e95-8a06-3def40515484"	/* cyfmac43455-sdio.bin */
#define SYSDEF_FW_BWFM43455_CLM	"01a0d8c4-5485-7fa6-9b17-4ef051625485"	/* cyfmac43455-sdio.clm_blob */
#define SYSDEF_FW_BWFM43455_NV	"01a0d8c4-5486-70b7-ac28-5f0162735486"	/* Ｗｉ－Ｆｉの基板の設定 */

/*
 * The patch of the same chip's Bluetooth (BCM4345C0 on the BCM2712's UART,
 * design 6.8), given at every power on (device/bt/bt_bcm.c; record 1 the
 * file as Raspberry Pi's bluez-firmware publishes it). The Pi 5 build puts
 * it on the system volume with the Wi-Fi's, linked from the system box.
 */
#define SYSDEF_FW_BCM4345C0	"01a0d8c4-5610-7d01-8e10-01a0c3d45610"	/* BCM4345C0.hcd */

/*
 * The system's sounds: the box they are in, and the buzzer and the
 * clicks. Each is played through the sound device's object; its record
 * 1 holds the sound as a WAV file, or else its metadata says the tone
 * (tessronos.sound.tone: hz, ms) the system makes for it (beep.c).
 */
#define SYSDEF_SND_BOX		"01a0d8c4-52c3-7d43-9e54-8b9cadbecf34"	/* 音箱 */
#define SYSDEF_CA_CERTS		"01a0d8c4-52d0-7a61-8b72-4c5d6e7f8091"	/* ルート証明書: HTTPSで信頼する認証局(レコード1にPEM) */
#define SYSDEF_SND_BUZZER	"01a0d8c4-52c4-7e54-8f65-9cadbecfd045"	/* ブザー音 */
#define SYSDEF_SND_PRESS	"01a0d8c4-52c5-7f65-9a76-adbecfd0e156"	/* 押下音 */
#define SYSDEF_SND_RELEASE	"01a0d8c4-52c6-7076-8b87-becfd0e1f267"	/* 離し音 */
#define SYSDEF_PROG_BROWSER	"01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57"	/* ブラウザ */

/*
 * What the browser keeps between runs, linked from the learning box: its
 * record 1 holds the bytes of one store of the engine (application/
 * browser/port/net/br_store.cc)
 */
#define SYSDEF_BROWSER_COOKIE	"01a0d8c4-5b23-7c14-8e65-9b7f8a9dae12"	/* ブラウザのCookie */
#define SYSDEF_BROWSER_STORAGE	"01a0d8c4-5b24-7d15-8f76-ac8f9baebf23"	/* ブラウザの保存域 */
#define SYSDEF_BROWSER_CACHE	"01a0d8c4-5b25-7e16-8a87-bd9aacbfc034"	/* ブラウザの蓄え */
#define SYSDEF_BROWSER_EXT	"01a0d8c4-5b29-7d32-8e43-0f1e2d3c4b5a"	/* ブラウザの拡張機能: 拡張機能の実身への仮身と、その状態 */
#define SYSDEF_BROWSER_SAVE	"01a0d8c4-5b26-7f17-8b98-ceabbdc0d145"	/* ブラウザの保存箱: 頁から保存したもの(システム箱) */

/* The menus every window has (SYSMENU.DEF) */
#define SYSDEF_MENU_WINDOW	"01a0d889-45e7-7a0c-adb9-ddf2d690ef56"	/* 閉じる */
#define SYSDEF_MENU_OBJECT	"01a0d889-45ec-766a-986d-5fad49d48e06"	/* 仮身操作 … 実行 */
#define SYSDEF_MENU_VOBJ	"01a0d889-45e8-730d-9735-a0a4cd451326"	/* 仮身操作 */
#define SYSDEF_MENU_REAL	"01a0d889-45e9-7bcc-9400-508a2870f95b"	/* 実身操作 */
#define SYSDEF_MENU_ACC		"01a0d889-45ea-781b-8701-66f15a81b862"	/* 小物 */
#define SYSDEF_MENU_ACCBOX	"01a0d889-4820-7a01-8b12-3c4d5e6f4820"	/* 小物箱のメニュー */
#define SYSDEF_MENU_ACCMGR	"01a0d889-4821-7a12-8b23-3c4d5e6f4821"	/* 小物管理 */
#define SYSDEF_MENU_EXEC	"01a0d889-45eb-7624-bbda-d9863883da4b"	/* 実行 */
#define SYSDEF_MENU_TRAY	"01a0d889-460c-7eda-8b47-5745d175681c"	/* トレーの履歴 */
#define SYSDEF_MENU_DISK	"01a0d889-46c0-7d2a-8c3b-4e5f6a7b8c91"	/* ディスク操作 */

/* 暦: the system's time zone, kept in its metadata (tessronos.calendar.tz, design 12.3.1) */
#define SYSDEF_CALENDAR		"01a0d8c4-52b3-7e43-9c53-6b7f8e9dab13"

/* トレー実身 (design 18.16): on the volatile volume, one while there is one user */
#define SYSDEF_TRAY		"01a0d8c4-5210-713b-9447-234cbe7a7ed5"

/* The menus of the programs built into the desktop */
#define SYSDEF_MENU_CAB		"01a0d889-45f1-79f8-a7bc-5cec735dc1ae"	/* 仮身一覧 */
#define SYSDEF_MENU_DOC		"01a0d889-45fa-718c-8cf5-55990db2cd7d"	/* 基本文章編集 */
#define SYSDEF_MENU_FIG		"01a0d889-4602-7aa0-8bef-fcfcae5c6e00"	/* 基本図形編集 */
#define SYSDEF_MENU_TRASH	"01a0d889-4605-7e09-8386-522403b11551"	/* 屑実身操作 */
#define SYSDEF_MENU_SEARCH	"01a0d889-4607-7ed4-802e-9525098ad383"	/* 実身/仮身検索 */
#define SYSDEF_MENU_NETWORK	"01a0d889-460a-7456-bd41-4034928caffb"	/* 仮身ネットワーク */
#define SYSDEF_MENU_CONSOLE	"01a0d889-460d-7a3b-8c52-1e9f4d7b2a60"	/* コンソール */
#define SYSDEF_MENU_SERIAL	"01a0d889-460e-7b41-9d63-2fa05e8c3b71"	/* シリアル通信 */
#define SYSDEF_MENU_SER_PORT	"01a0d889-460f-7c52-8e74-3ab16f9d4c82"	/* ポート */
#define SYSDEF_MENU_SER_SPEED	"01a0d889-4610-7d63-9f85-4bc27a0e5d93"	/* 通信速度 */
#define SYSDEF_MENU_SER_NL	"01a0d889-4611-7e74-8a96-5cd38b1f6ea4"	/* 送信する改行 */
#define SYSDEF_MENU_MICROSCRIPT	"01a0d889-4612-7f85-9ba7-6de49c2f7fb5"	/* マイクロスクリプト */
#define SYSDEF_MENU_MS_OPS	"01a0d889-4613-708a-8cb8-7ef5ad3a80c6"	/* 操作 */
#define SYSDEF_MENU_XFCONV	"01a0d889-46b1-7e21-8d31-6a5b4c3d2e11"	/* ファイル変換 */
#define SYSDEF_MENU_XFC_DIR	"01a0d889-46b2-7f32-8e42-7b6c5d4e3f22"	/* ファイル変換のディレクトリ */
#define SYSDEF_MENU_BACKUP	"01a0d889-46b0-7e21-8c4d-2a5f9e3b6d18"	/* バックアップ */
#define SYSDEF_MENU_NETENV	"01a0d889-46b5-7d14-9a25-6b7c8d9eaf01"	/* ネットワーク設定 */
#define SYSDEF_MENU_SYSENV	"01a0d889-46b6-7e25-8b36-7c8d9eaf0b12"	/* システム環境設定 */
#define SYSDEF_MENU_SNDMIX	"01a0d889-46c0-7d64-8f85-a1b2c3d4e5f6"	/* 音量ミキサー */
#define SYSDEF_MENU_BROWSER	"01a0d889-46bc-7e8b-8a9c-d2e3f4a5b678"	/* ブラウザ */
#define SYSDEF_MENU_SYSENV_OP	"01a0d889-46bb-7d7a-9f8b-c1d2e3f4a567"	/* システム環境設定の操作 */
#define SYSDEF_MENU_FONTSET	"01a0d889-4780-7a01-8b12-3c4d5e6f4780"	/* フォント設定 */
#define SYSDEF_MENU_FONTSET_VIEW "01a0d889-4781-7a12-8b23-3c4d5e6f4781"	/* フォント設定の表示 */
#define SYSDEF_MENU_USERENV	"01a0d889-46b7-7f36-9c47-8d9eaf0b1c23"	/* ユーザ環境設定 */
#define SYSDEF_MENU_INFO	"01a0d889-46b8-7a47-8d58-9eaf0b1c2d34"	/* 管理情報 */
#define SYSDEF_MENU_WINLIST	"01a0d889-46ba-7c69-8f7a-b0c1d2e3f456"	/* ウインドウ */
#define SYSDEF_MENU_TAIL	"01a0d889-46bd-7f9c-8bad-e3f4a5b6c789"	/* ウインドウ、小物、実行 */
#define SYSDEF_MENU_CLOCK	"01a0d889-46be-7a0d-9bbe-f4a5b6c7d89a"	/* 時計のメニュー */
#define SYSDEF_MENU_BASECOL	"01a0d8c4-5b93-7c54-8d65-9fab0c1d5b93"	/* 原紙集めのメニュー */
#define SYSDEF_MENU_FQUIT	"01a0d889-5423-7d43-8e54-9f6071b2c423"	/* 強制終了 */
#define SYSDEF_MENU_CALENDAR	"01a0d889-46c0-7a11-8b22-3c4d5e6f7a81"	/* 予定表 */
#define SYSDEF_MENU_SLIDE	"01a0d889-46c1-7b22-9c33-4d5e6f7a8b92"	/* スライド */
#define SYSDEF_MENU_CAL_VIEW	"01a0d889-46c5-7f66-9a77-8b9cadbecfd6"	/* 予定表の表示 */
#define SYSDEF_MENU_CAL_EDIT	"01a0d889-46c6-7077-8b88-9cadbecfd0e7"	/* 予定表の編集 */
#define SYSDEF_MENU_CALC	"01a0d889-46d0-7a01-8b12-3c4d5e6f7a01"	/* 基本表計算 */
/* 基本表計算's menus that its 操作パネル opens under its buttons */
#define SYSDEF_MENU_CALC_FACE	"01a0d889-46d3-7d34-9e45-6f7a8b9cad34"	/* 書体 */
#define SYSDEF_MENU_CALC_SIZE	"01a0d889-46d5-7f56-9a67-8b9cadbecf56"	/* 文字サイズ */
#define SYSDEF_MENU_CALC_COLOUR	"01a0d889-46d6-7067-8b78-9cadbecfd067"	/* 文字色 */
#define SYSDEF_MENU_CALC_GROUND	"01a0d889-46da-74ab-8fbc-d0e1f20314ab"	/* セルの背景色 */
#define SYSDEF_MENU_CALC_LCOLOUR "01a0d889-46dc-76cd-8bde-f203142536cd"	/* 線の色 */
#define SYSDEF_MENU_CALC_ALIGN	"01a0d889-46f0-7a01-8b12-3c4d5e6f7c01"	/* 横の配置 */
#define SYSDEF_MENU_CALC_VALIGN	"01a0d889-46f1-7b12-9c23-4d5e6f7c8d12"	/* 縦の配置 */
#define SYSDEF_MENU_CALC_LINE	"01a0d889-46f2-7c23-8d34-5e6f7c8d9e23"	/* 線種 */
#define SYSDEF_MENU_CALC_LWIDTH	"01a0d889-46f3-7d34-9e45-6f7c8d9eaf34"	/* 線幅 */
#define SYSDEF_MENU_KANATYPE	"01a0d889-46e0-7a01-8b12-3c4d5e6f7b01"	/* かな入力練習 */
#define SYSDEF_MENU_BEFC	"01a0d889-5500-7a01-8b12-3c4d5e6f7b52"	/* BeFC */
#define SYSDEF_MENU_BESFC	"01a0d889-5501-7a01-8b12-3c4d5e6f7b62"	/* BeSFC */
#define SYSDEF_MENU_WORDDIC	"01a0d889-4710-7a01-8b12-3c4d5e6f7c10"	/* 単語登録 */
#define SYSDEF_MENU_CALCU	"01a0d889-4720-7a01-8b12-3c4d5e6f7d20"	/* 電卓 */
#define SYSDEF_MENU_KEYMAP	"01a0d889-4740-7a01-8b12-3c4d5e6f7e40"	/* キー配列 */
#define SYSDEF_MENU_KM_LATIN	"01a0d889-4741-7a12-8b23-3c4d5e6f7e41"	/* キー配列の英字配列 */
#define SYSDEF_MENU_KM_KANA	"01a0d889-4742-7a23-8b34-3c4d5e6f7e42"	/* キー配列のかな配列 */
#define SYSDEF_MENU_KM_SHAPE	"01a0d889-4743-7a34-8b45-3c4d5e6f7e43"	/* キー配列のキーボードの形 */
#define SYSDEF_MENU_SOFTKB	"01a0d889-47b0-7a01-8b12-3c4d5e6f47b0"	/* ソフトウェアキーボード */
#define SYSDEF_MENU_SK_LAYOUT	"01a0d889-47b1-7a12-8b23-3c4d5e6f47b1"	/* ソフトウェアキーボードの配列 */
#define SYSDEF_MENU_KM_VIEW	"01a0d889-4744-7a45-8b56-3c4d5e6f7e44"	/* キー配列の表示 */
#define SYSDEF_MENU_DISKFIX	"01a0d889-47e0-7a01-8b12-3c4d5e6f47e0"	/* ディスク修復 */
#define SYSDEF_MENU_DFIX_INFO	"01a0d889-47e1-7a12-8b23-3c4d5e6f47e1"	/* ディスク修復の障害情報 */
#define SYSDEF_MENU_SYSINST	"01a0d889-4810-7a11-8b22-3c4d5e6f4810"	/* システム登録 */
#define SYSDEF_MENU_TRAYV	"01a0d889-47f0-7a01-8b12-3c4d5e6f47f0"	/* トレー */
#define SYSDEF_MENU_TRAYV_VIEW	"01a0d889-47f1-7a12-8b23-3c4d5e6f47f1"	/* トレーの表示 */
#define SYSDEF_MENU_NOTE	"01a0d889-47f2-7a23-8b34-3c4d5e6f47f2"	/* 通知 */
#define SYSDEF_MENU_FPAT	"01a0d889-4800-7a01-8b12-3c4d5e6f4800"	/* パターン編集 */
#define SYSDEF_MENU_MCARD	"01a0d889-4730-7a01-8b12-3c4d5e6f7e30"	/* マイクロカード */
#define SYSDEF_MENU_CHARSRCH	"01a0d889-4750-7a01-8b12-3c4d5e6f7f50"	/* 文字検索 */
#define SYSDEF_MENU_ARCHIVE	"01a0d889-4770-7a01-8b12-3c4d5e6f7f70"	/* 書庫管理 */
#define SYSDEF_MENU_TECHO	"01a0d889-47c0-7a01-8b12-3c4d5e6f47c0"	/* 電子手帳 */
#define SYSDEF_MENU_MC_KIND	"01a0d889-4737-7a78-8b89-3c4d5e6f7e37"	/* マイクロカードの項目の種類 */
#define SYSDEF_MENU_MC_FACE	"01a0d889-4738-7a89-8b9a-3c4d5e6f7e38"	/* マイクロカードの項目の書体 */
#define SYSDEF_MENU_KT_KEYBOARD	"01a0d889-46e2-7c23-8d34-5e6f7b8c9d23"	/* かな入力練習のキーボードレイアウト */
#define SYSDEF_MENU_KT_KANA	"01a0d889-46e3-7d34-9e45-6f7b8c9dae34"	/* かな入力練習のかな入力方式 */
#define SYSDEF_MENU_KT_TRANS	"01a0d889-46e4-7e45-8f56-7b8c9daebf45"	/* かな入力練習のキーコード変換 */

/* The data boxes of the settings accessories: their panels (design 18.15) */
#define SYSDEF_BOX_NETENV	"01a0d8c4-52c8-7d11-8e21-6f7a8b9c0d01"	/* ネットワーク設定 */
#define SYSDEF_BOX_SYSENV	"01a0d8c4-52c9-7e22-9f32-7a8b9c0d1e12"	/* システム環境設定 */
#define SYSDEF_BOX_USERENV	"01a0d8c4-52ca-7f33-8a43-8b9c0d1e2f23"	/* ユーザ環境設定 */
#define SYSDEF_BOX_SNDMIX	"01a0d8c4-5442-7c53-8e74-90a1b2c35442"	/* 音量ミキサー */
#define SYSDEF_BOX_XFCONV	"01a0d8c4-52cb-7b44-9c54-9d0e1f2a3b45"	/* ファイル変換: 尋ねるパネル */
#define SYSDEF_BOX_CALENDAR	"01a0d8c4-52d6-7d53-9e64-7f8091a2bc34"	/* 予定表: 上の帯と年月を尋ねるパネル */
#define SYSDEF_BOX_SLIDE	"01a0d8c4-52db-7d64-8e75-9f8091a2b3db"	/* スライド: ジャンプと背景色変更の一覧のパネル */
#define SYSDEF_BOX_CALC		"01a0d8c4-52e7-7a08-8b19-2c3d4e5f6a73"	/* 基本表計算: 検索/置換、用紙設定、保存の確認 */
#define SYSDEF_BOX_KANATYPE	"01a0d8c4-52f2-7c23-8d34-5e6f7a8b9c12"	/* かな入力練習: 結果、先読みキー数 */
#define SYSDEF_BOX_WORDDIC	"01a0d8c4-5314-7e45-8f56-7b8c9daebf14"	/* 単語登録: 窓のパネルと尋ねるパネル */
#define SYSDEF_BOX_CALCU	"01a0d8c4-5322-7c63-8d74-8e9fabc21022"	/* 電卓: 標準と関数電卓のパネル */
#define SYSDEF_BOX_MCARD	"01a0d8c4-5332-7c73-8e84-9fabc4d53232"	/* マイクロカード: 表示設定、並べ替え、印刷などのパネル */
#define SYSDEF_BOX_KEYMAP	"01a0d8c4-52ea-7d3b-8e4c-5f6a7b8c9d96"	/* キー配列: ウインドウの上の帯と下の一覧 */
#define SYSDEF_BOX_FIG		"01a0d8c4-52ec-7f5d-9a6e-7b8c9daebf08"	/* 基本図形編集: 道具パネルのボタンの絵 */
#define SYSDEF_BOX_DOC		"01a0d8c4-5448-7d61-8e72-9f8091a25448"	/* 基本文章編集: 知らせ、検索/置換、仮身化などのパネル */
#define SYSDEF_BOX_CAB		"01a0d8c4-5450-7c84-8d95-a6b7c8d95450"	/* 仮身一覧: 整頓のパネル */
#define SYSDEF_BOX_CHARSRCH	"01a0d8c4-5352-7c83-8f94-bc1d2e3f5252"	/* 文字検索: 窓のパネルと尋ねるパネル */
#define SYSDEF_BOX_ARCHIVE	"01a0d8c4-5376-7d95-8ea6-bc1d2e3f7676"	/* 書庫管理: 書籍の作成、書籍情報、実身名一覧のパネル */
#define SYSDEF_BOX_MICROSCRIPT	"01a0d8c4-521c-7f74-8d39-6fae4c8b2f45"	/* マイクロスクリプト: 接続の許可を尋ねるパネル */
#define SYSDEF_BOX_BACKUP	"01a0d8c4-52b4-7f54-8d64-7c8f9fa0beb4"	/* バックアップ: 窓のパネル(保存と復帰のタブ)と次の媒体を尋ねるパネル */
#define SYSDEF_BOX_FQUIT	"01a0d8c4-5422-7c32-9d43-8e5f6071a422"	/* 強制終了: 窓のパネルと立ち上げ直しを尋ねるパネル */
#define SYSDEF_BOX_TECHO	"01a0d8c4-53c2-7c33-8d44-5e6f7a8b53c2"	/* 電子手帳: 検索パネル、用紙設定、印刷と閉じるときに尋ねるパネル */
#define SYSDEF_BOX_BROWSER	"01a0d8c4-5442-7c94-8a05-1b2c3d4e5442"	/* ブラウザ: 頁の alert・confirm・prompt と select の一覧のパネル */

#endif /* __TS_SYSDEF_H__ */
