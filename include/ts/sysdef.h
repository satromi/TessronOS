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

/* かな漢字変換の辞書: mozc's data set in record 1 (RT 12) */
#define SYSDEF_DICT_MOZC	"01a0d8c4-5216-7f4a-8c27-1d5b3e9a6c02"

/* The clock: a program object whose record 1 is its executable */
#define SYSDEF_PROG_CLOCK	"01a0d8c4-5212-7a19-8d63-e04b5c9f2a71"
#define SYSDEF_PROG_CONSOLE	"01a0d8c4-5219-7c3e-8a41-2f6b9d0e5c11"	/* コンソール */
#define SYSDEF_PROG_SERIAL	"01a0d8c4-521a-7d52-9b17-4e8c2a6f0d23"	/* シリアル通信 */
#define SYSDEF_PROG_MICROSCRIPT	"01a0d8c4-521b-7e63-8c28-5f9d3b7a1e34"	/* マイクロスクリプト */
#define SYSDEF_PROG_UNPACK	"01a0d8c4-52a7-7c31-9b5e-3f1d8a6c2e90"	/* 書庫解凍 */
#define SYSDEF_PROG_XFCONV	"01a0d8c4-52b1-7c21-9a31-4f5e6d7c8b91"	/* ファイル変換 */
#define SYSDEF_XFCONV_BOX	"01a0d8c4-52b2-7d32-8b42-5a6f7e8d9ca2"	/* ファイル変換の箱: 取り込んだ箱の置き場 */
#define SYSDEF_PROG_BACKUP	"01a0d8c4-52b0-7d15-9a3c-4e6f8b2d1c07"	/* バックアップ */
#define SYSDEF_PROG_NETENV	"01a0d8c4-52b5-7a41-8c61-2d7e9f0a1b31"	/* ネットワーク設定 */
#define SYSDEF_PROG_SYSENV	"01a0d8c4-52b6-7b52-9d72-3e8fa01b2c42"	/* システム環境設定 */
#define SYSDEF_PROG_USERENV	"01a0d8c4-52b7-7c63-8e83-4f9a012c3d53"	/* ユーザ環境設定 */

/* The system itself: a virtual device whose records say and change what the machine is (OB_SYS_*) */
#define SYSDEF_SYSTEM		"01a0d680-3aa6-7c52-9d6f-3e72a0b18f14"

/* The settings those three write, as lines of text in record 1 (include/ts/conf.h) */
#define SYSDEF_CONF_NET		"01a0d8c4-52c0-7a10-8b21-5e6f7a8b9c01"	/* ネットワークの設定 */
#define SYSDEF_CONF_DEV		"01a0d8c4-52c1-7b21-9c32-6f7a8b9cad12"	/* 機器の設定 */
#define SYSDEF_CONF_USER	"01a0d8c4-52c2-7c32-8d43-7a8b9cadbe23"	/* ユーザ情報 */

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

/* The menus every window has (SYSMENU.DEF) */
#define SYSDEF_MENU_WINDOW	"01a0d889-45e7-7a0c-adb9-ddf2d690ef56"	/* 閉じる */
#define SYSDEF_MENU_OBJECT	"01a0d889-45ec-766a-986d-5fad49d48e06"	/* 仮身操作 … 実行 */
#define SYSDEF_MENU_VOBJ	"01a0d889-45e8-730d-9735-a0a4cd451326"	/* 仮身操作 */
#define SYSDEF_MENU_REAL	"01a0d889-45e9-7bcc-9400-508a2870f95b"	/* 実身操作 */
#define SYSDEF_MENU_ACC		"01a0d889-45ea-781b-8701-66f15a81b862"	/* 小物 */
#define SYSDEF_MENU_EXEC	"01a0d889-45eb-7624-bbda-d9863883da4b"	/* 実行 */
#define SYSDEF_MENU_TRAY	"01a0d889-460c-7eda-8b47-5745d175681c"	/* トレーの履歴 */

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
#define SYSDEF_MENU_UNPACK	"01a0d889-46a7-7e53-9d70-5b3fac8e4b02"	/* 書庫解凍 */
#define SYSDEF_MENU_XFCONV	"01a0d889-46b1-7e21-8d31-6a5b4c3d2e11"	/* ファイル変換 */
#define SYSDEF_MENU_XFC_DIR	"01a0d889-46b2-7f32-8e42-7b6c5d4e3f22"	/* ファイル変換のディレクトリ */
#define SYSDEF_MENU_BACKUP	"01a0d889-46b0-7e21-8c4d-2a5f9e3b6d18"	/* バックアップ */
#define SYSDEF_MENU_NETENV	"01a0d889-46b5-7d14-9a25-6b7c8d9eaf01"	/* ネットワーク設定 */
#define SYSDEF_MENU_SYSENV	"01a0d889-46b6-7e25-8b36-7c8d9eaf0b12"	/* システム環境設定 */
#define SYSDEF_MENU_BROWSER	"01a0d889-46bc-7e8b-8a9c-d2e3f4a5b678"	/* ブラウザ */
#define SYSDEF_MENU_SYSENV_OP	"01a0d889-46bb-7d7a-9f8b-c1d2e3f4a567"	/* システム環境設定の操作 */
#define SYSDEF_MENU_USERENV	"01a0d889-46b7-7f36-9c47-8d9eaf0b1c23"	/* ユーザ環境設定 */
#define SYSDEF_MENU_INFO	"01a0d889-46b8-7a47-8d58-9eaf0b1c2d34"	/* 管理情報 */
#define SYSDEF_MENU_WINLIST	"01a0d889-46ba-7c69-8f7a-b0c1d2e3f456"	/* ウインドウ */

/* The data boxes of the settings accessories: their panels (design 18.15) */
#define SYSDEF_BOX_NETENV	"01a0d8c4-52c8-7d11-8e21-6f7a8b9c0d01"	/* ネットワーク設定 */
#define SYSDEF_BOX_SYSENV	"01a0d8c4-52c9-7e22-9f32-7a8b9c0d1e12"	/* システム環境設定 */
#define SYSDEF_BOX_USERENV	"01a0d8c4-52ca-7f33-8a43-8b9c0d1e2f23"	/* ユーザ環境設定 */
#define SYSDEF_BOX_XFCONV	"01a0d8c4-52cb-7b44-9c54-9d0e1f2a3b45"	/* ファイル変換: 尋ねるパネル */

#endif /* __TS_SYSDEF_H__ */
