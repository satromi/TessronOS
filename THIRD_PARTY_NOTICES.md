# 第三者のソフトウェアとリソース

TessronOSに同梱している、またはビルドしたイメージに含まれる、TessronOS以外の著作物とそのライセンスの一覧である。TessronOSのソースコード自体のライセンスは`LICENSE`(T-License 2.2)による。

## リポジトリに含まれるもの

| 名称 | 場所 | バージョン | ライセンス | ライセンス文 |
| --- | --- | --- | --- | --- |
| μT-Kernel 3.0(TRON Forum) | `kernel/`、`include/tk`、`include/sys`、`include/tm`、`lib/libtk`、`lib/libtm`、`config/`の由来部分 | v3.00.08 | T-License 2.2 | `LICENSE` |
| lwIP(`make NETSTACK=lwip`のときだけイメージに入る) | `peripheral_kernel/network/lwip/` | 2.2.0 | 修正BSD(3条項) | `peripheral_kernel/network/lwip/COPYING` |
| FreeType | `outer_kernel/font/freetype/` | 2.13.3 | FreeType License(FTL)を選択(GPLv2とのデュアルライセンス) | `outer_kernel/font/freetype/LICENSE.TXT`、`outer_kernel/font/freetype/docs/FTL.TXT` |
| Noto Sans JP | `etc/font/NotoSansJP-Regular.otf` | Noto CJK Sans 2.004 | SIL Open Font License 1.1 | `etc/font/OFL.txt` |
| Noto Serif JP | `etc/font/NotoSerifJP-Regular.otf` | Noto CJK Serif 2.003 | SIL Open Font License 1.1 | `etc/font/OFL.txt` |
| Noto Sans Mono CJK JP(公式の配布物から、Noto Sans JPと同じ文字に絞ったもの) | `etc/font/NotoSansMonoCJKjp-Regular.otf` | 元はNoto CJK Sans 2.004の`Sans/Mono/NotoSansMonoCJKjp-Regular.otf`(入手元 <https://github.com/notofonts/noto-cjk>、16424156バイト) | SIL Open Font License 1.1 | `etc/font/OFL.txt` |
| Mbed TLS | `lib/mbedtls/` | 3.6.7 | Apache License 2.0を選択(GPL 2.0以降とのデュアルライセンス) | `lib/mbedtls/LICENSE` |
| liblc3(Google。LE AudioのLC3の符号化器と復号器) | `lib/liblc3/`(`include/lc3.h`、`include/lc3_private.h`、`src/`の`.c`と`.h`。符号化器をカーネルに結ぶ。復号器はカーネル内テストだけが呼ぶ) | 1.1.3(タグ`v1.1.3`、コミット`96a3af0beb5487aca3b98a4b992a539a1f6d80d1`) | Apache License 2.0 | `lib/liblc3/LICENSE` |
| Mesa(V3D 7.1の制御リストのパケットを詰めるコード) | `device/gpu/v3d/mesa/v3d_packet_v71_pack.h`(Mesaの`v3d_packet.xml`から`gen_pack_header.py`で生成したもの) | 26.2.3 | MIT | ファイルの先頭 |
| Mesa(ウインドウの合成のシェーダ) | `device/gpu/v3d/mesa/v3d_cp_shaders.h`(MesaのNIRとV3Dのシェーダコンパイラ、パケットの詰め込みが出力したQPUのコードとレコード) | 26.2.3 | MIT | ファイルの先頭 |
| Mesa(IntelのGPUのウインドウの合成のシェーダ) | `device/gpu/intel/mesa/igt_cp_shaders.h`(MesaのNIRとIntelのシェーダコンパイラ、genxmlの詰め込みが出力した第9世代のコード、インタフェース記述子、コマンド) | 26.2.3 | MIT | ファイルの先頭 |
| MozillaのCA証明書リスト | `etc/def/01a0d8c4-52d0-7a61-8b72-4c5d6e7f8091_1.bin`(実身「ルート証明書」のレコード1) | curlのcacert.pem(2026-09-25時点のMozillaのデータ) | Mozilla Public License 2.0 | <https://mozilla.org/MPL/2.0/>(入手元 <https://curl.se/docs/caextract.html>) |
| BeFC(satromi、MIT) | `application/befc/` | - | MIT | `application/befc/LICENSE` |
| emu2413 v1.5.9(BeFCのVRC7の音源の表と式) | `application/befc/cart.c`、`opll_tables.h` | 1.5.9 | MIT | `application/befc/LICENSE` |
| minimp3 | `application/rtmedia/minimp3.h`(実時間メディアのMP3の復号) | 版番号なし(<https://github.com/lieff/minimp3>) | CC0 1.0(パブリックドメインへの献呈) | ファイルの先頭 |
| Formula.js(@formulajs/formulajs) | `application/calc/js/formula.min.js`(基本表計算の関数。V8を結んだ基本表計算に埋め込む) | 4.3.0 | MIT | `application/calc/js/LICENSE-formulajs` |
| jStat | `application/calc/js/jstat.min.js`(Formula.jsが使う統計の関数) | 1.9系 | MIT | `application/calc/js/LICENSE-jstat` |
| Chromiumのメディアの試験データ(`media/test/data`の`bear-av1-480x360.webm`と`bear-av1.mp4`) | `tests/ktest/ktest_v8.c`の`vid_av1`、`vid_av1mp4`(ブラウザのAV1の試験が配るバイト列。イメージの試験のプログラムにだけ入る) | Chromium 153.0.8010.36のチェックアウトのもの | BSD 3条項(Chromium) | Chromiumの`LICENSE` |
| OpenBSD 8.0の有線LANのドライバ | `device/net/x86_64/rge_hw.h`(`if_rge.c`、`if_rgereg.h`からの表と手順。ISC)、`device/usb/x86_64/ure_hw.h`(`if_ure.c`、`if_urereg.h`からの表と手順。2条項BSD)、`device/usb/x86_64/axen_reg.h`(`if_axenreg.h`のレジスタ番号。作者はYojiro UO氏で、`if_axen.c`はISC)、`device/usb/x86_64/axe_reg.h`(`if_axereg.h`のレジスタ番号と命令。Bill Paul氏の4条項BSDで宣伝条項あり。`if_axe.c`の`axe_devs`のうちAX88772の系統のIDを`usbdevs`の番号にしたもの。`if_axe.c`はJonathan Gray氏のISCとBill Paul氏の4条項BSD)。いずれも`tools/x64/nic/gen_hw.py`が取り出し、ライセンス文をファイルの先頭に残す。`device/net/x86_64/rge.c`のMACのMCUの書込み手順は`if_rge.c`(ISC)を、`device/gpio/x86_64/gpio_pch.c`のパッドの表とレジスタの配置は`sys/dev/acpi/pchgpio.c`(ISC)を、`device/net/x86_64/re.c`のリビジョンの表、EEPROMの読み方とレジスタ番号は`sys/dev/ic/re.c`、`rtl81x9reg.h`(Bill Paul氏の4条項BSD。宣伝条項あり)を、`igc.c`のレジスタ番号と手順は`sys/dev/pci/igc_*`(IntelとRubicon CommunicationsのBSD 3条項)を、`device/usb/x86_64/usbven.c`のAX88772の初期化、PHYの選び方と電源、受信と送信の枠、リンクの読み方は`sys/dev/usb/if_axe.c`(ISC、BSD 4条項)を読んで書いた | OpenBSD 8.0 | ISC、BSD 2条項、BSD 3条項、BSD 4条項 | 各ファイルの先頭、OpenBSDの`sys.tar.gz`の各ファイル |
| QualcommのQCA6174のWi-Fiのドライバathx | `device/net/wifi/obsd/athx.c`、`athx_pci.c`、`athx_fw.c`、`athx_wmi.c`、`athx_htt.c`、`athxreg.h`、`athxvar.h`(Linux 7.3.1のath10k`drivers/net/wireless/ath/ath10k`の`pci.c`、`ce.c`、`bmi.c`、`htc.c`、`core.c`、`hw.c`、`wmi.c`、`wmi-tlv.c`、`htt.c`、`htt_rx.c`、`htt_tx.c`、`mac.c`のうちPCIとQCA6174に要る部分をOpenBSDのnet80211の上に書き直したもの。`athxreg.h`はath10kのヘッダから宣言を抜き出した。net80211との結び方はOpenBSD 8.0のqwx(`sys/dev/ic/qwx.c`)の形に合わせた)、`athxregd.h`(Linuxの`drivers/net/wireless/ath/regd.h`、`regd_common.h`、`ath.h`の規制領域の表)。層とのつなぎの`tsw_athx.c`はT-License | Linux 7.3.1、OpenBSD 8.0 | ISC(Atheros Communications、Qualcomm Atheros、The Linux Foundation、Qualcomm Technologies、Stefan Sperling)。ath10kの各ファイルのSPDXがISCであることを確かめて写した。GPLのコードは含まない | 各ファイルの先頭 |

Noto Sans Mono CJK JPのほかは、改変せずに同梱している。Noto Sans Mono CJK JPは、配布物(16MB)をそのまま入れると大きいので、fontToolsの`subset`で、文字をNoto Sans JP(`NotoSansJP-Regular.otf`)の文字表にある16732字に、組版の機能を日本語の組版に使うもの(`ccmp`、`liga`、`dlig`、`fwid`、`hwid`、`pwid`、`jp78`、`jp83`、`jp90`、`nlck`、`ruby`、`vert`、`vrt2`、`kern`、`palt`、`halt`、`vpal`、`vhal`、`vkrn`、`mark`、`mkmk`)に絞り、名前の表と字形の形はそのまま残した(4.4MB)。配布物に添えられたOFLの本文(`Sans/LICENSE`。`etc/font/OFL.txt`と同じもの)には、著作権表示の後に予約された書体名(Reserved Font Name)の指定がなく、名前の表の著作権表示(`© 2014-2021 Adobe`)にもない。改変した版の名前の制限(OFLの第3項)にあたる名前はないので、名前の表は元のまま(`Noto Sans Mono CJK JP`、`NotoSansMonoCJKjp-Regular`)にした。予約名`Source`はAdobeのSource Han Sansのもので、この書体の名前には含まれない。Mesaから生成したヘッダは、生成したものの先頭に著作権表示と許諾文を付けただけで、`tools/mesa/probe.sh pack`(パケット)と`tools/mesa/shaders.sh`(合成のシェーダ)、`tools/x64/mesa/igt_shaders.sh`(IntelのGPUの合成のシェーダ)で作り直せる。lwIP、FreeType、Mbed TLSは上流のファイルを変更せず、移植層を`peripheral_kernel/network/port/`、`outer_kernel/font/port/`、`lib/libtls/`に置く(`UPSTREAM.md`)。Mbed TLSをリンクしたプログラム(マイクロスクリプト)を配布するときは、Apache License 2.0の要求に従いライセンス文を添付する(Mbed TLSにはNOTICEファイルはない)。CA証明書リストはMPL 2.0の「対象ソフトウェア」を改変せずにソースコードの形で同梱するもので、その条件は上記URLの本文による。Noto Sans JP、Noto Serif JP、Noto Sans Mono CJK JPは、フォント単体で販売しない限り、OFLに従い他のソフトウェアと一緒に配布できる。

TADjs Desktopから持ってきたサンプルの実身とアイコン(`etc/xtad`、`etc/def`の`.ico`)、壁紙`TESSRON.PIC`は、TessronOSの作者自身の著作物であり、TessronOSの一部としてT-License 2.2で配布する。

## ビルドしたイメージに含まれるもの

リポジトリには含まれないが、ビルド時にリンクされるもの、またはイメージに格納されるものである。イメージを配布するときは、それぞれの条件に従う。

| 名称 | 含まれ方 | ライセンス |
| --- | --- | --- |
| ACPICA 20260930(ACPIの解釈系。`dispatcher`、`events`、`executer`、`hardware`、`namespace`、`parser`、`resources`、`tables`、`utilities`) | x64(`TARGET=_X64_`)のカーネルに、`tools/x64/acpica/build.sh`がビルドしたオブジェクトとしてリンクする。ソースはリポジトリに含めない | BSD 3条項とGPL 2.0のデュアルで、BSD 3条項を選択する(リリースの`LICENSE.BSD-3-Clause`。ビルドの出力先に`LICENSE-acpica.txt`として写す) |
| NetBSD 11.0のカーネルのネットワーク部分(rumpカーネルの基盤、ソケット、インタフェース、経路、IPv4、IPv6、TCP、UDPと、それが使う`libkern`、`common/lib`) | デフォルト(`NETSTACK=netbsd`)のすべてのイメージのカーネルに、`tools/netbsd/build.sh`がビルドしたオブジェクトとしてリンクする。ソースはリポジトリに含めない | 大半がBSD 2条項と3条項。一部はBSD 4条項(宣伝の条項がある)、RSA Data SecurityのMD4とMD5のライセンス、パブリックドメイン。`sys/net80211`のヘッダはBSDとGPL 2のデュアルで、BSDを選択する。GPLだけのファイルは含まない |
| wpa_supplicant 2.11の一部(`WPA_MODE=proc`では`src/eapol_supp`、`src/eap_peer`のPEAP、TLS、MSCHAPv2、`src/eap_common`、`src/crypto/tls_openssl.c`、`ms_funcs.c`も。`src/rsn_supp`の`wpa.c`、`wpa_ie.c`、`pmksa_cache.c`、`src/common`の`wpa_common.c`、`ieee802_11_common.c`、`sae.c`、`dragonfly.c`、`src/utils`の`common.c`、`wpabuf.c`、`src/crypto`の`dh_groups.c`。NetBSD 11.0の`src.tgz`の`external/bsd/wpa/dist`) | x64とRaspberry Pi 5の`WIFI=1`のビルドのカーネルに、`tools/wpa/build.sh`が移植層(`device/net/wifi/wpa/`)とともにビルドしたオブジェクトとしてリンクする。ソースはリポジトリに含めない | BSD 3条項(Copyright (c) 2002-2022, Jouni Malinen and contributors。配布物の`COPYING`。ビルドの出力先に`COPYING-wpa_supplicant.txt`として写す)。イメージを配布するときは著作権表示と条件を添付する |
| hostapdの認証側(wpa_supplicant 2.11と同じ配布物の`src/ap`の`wpa_auth.c`、`wpa_auth_ie.c`、`pmksa_cache_auth.c`。NetBSD 11.0の`src.tgz`の`external/bsd/wpa/dist`) | テストのためのWi-Fiのシミュレータ(`WIFI_SIM=1`、既定は0)のビルドのカーネルにだけ、`tools/wpa/build.sh ap`が仮想のAPの移植層(`device/net/wifi/sim/wsap_glue.c`)、上の行のwpa_supplicantの一部、`crypto_mbedtls.c`、Mbed TLSの写しとともにビルドしたオブジェクト(`wpaap.o`)としてリンクする。既定のビルドのイメージには入らない。ソースはリポジトリに含めず、変更しない | BSD 3条項(wpa_supplicantと同じ`COPYING`。ビルドの出力先に`COPYING-wpa_supplicant.txt`として写す) |
| wpa_supplicantの暗号の層`src/crypto/crypto_mbedtls.c`(Copyright (c) 2022, Glenn Strauss。OpenWrt v24.10.0の`package/network/services/hostapd/patches/110-mbedtls-TLS-crypto-option-initial-port.patch`が新しく足すファイル) | 同じオブジェクトに入る。`tools/wpa/build.sh`が当てを取得してSHA-256を検査し、当ての中のこのファイルの行だけを取り出して(取り出したもののSHA-256も検査する)、変更せずにコンパイルする。ソースはリポジトリに含めない | BSD 3条項(wpa_supplicantと同じ条件。ファイルの先頭の表示) |
| libssh2 1.11.1(https://libssh2.org/download/libssh2-1.11.1.tar.gz) | `SSH=1`のビルドで、ファイル変換(プログラム実身のレコード1)とコマンド`ssh`に、Mbed TLSとともに静的にリンクする。`tools/ssh/build.sh`が取得してSHA-256を検査し、変更せずにコンパイルする。ソースはリポジトリに含めない | BSD 3条項(配布物の`COPYING`。ビルドの出力先に`COPYING-libssh2.txt`として写す)。イメージを配布するときは著作権表示と条件を添付する |
| OpenSSL 3.5.9のlibcryptoとlibssl(`no-asm`などで使わない部分を外した構成。https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz) | `WIFI=1 WPA_MODE=proc`のビルドで、プロセスwpasupp(プログラム実身のレコード1)に、wpa_supplicantの同じソースと本流の`src/crypto/crypto_openssl.c`、移植層(`device/net/wifi/wpa/wpa_glue.c`、`application/wpasupp/`)とともにリンクする。`tools/wpa/proc.sh`が取得してSHA-256を検査し、変更せずにコンパイルする。ソースはリポジトリに含めない | Apache License 2.0(Copyright (c) 1998-2025 The OpenSSL Project Authors。配布物の`LICENSE.txt`。ビルドの出力先に`LICENSE-openssl.txt`として写す。NOTICEファイルはない)。イメージを配布するときはライセンス文を添付する |
| Mbed TLS 3.6.7の一部(`lib/mbedtls/library`の37のファイル。大きい整数、楕円曲線、ECDH、ECDSA、DH、MD5、SHA-1、SHA-2、AES、CMAC、鍵の包み、HKDF、PKCS#5、CTR_DRBGほか) | x64とRaspberry Pi 5の`WIFI=1`のビルドのカーネルに、wpa_supplicantと同じオブジェクトとしてリンクする。構成は`device/net/wifi/wpa/ts_wpa_mbedtls_config.h`で、上流のファイルは変更しない | Apache License 2.0(上の表のMbed TLSと同じ。ビルドの出力先に`LICENSE-mbedtls.txt`として写す) |
| IntelのWi-Fiのファームウェア`iwlwifi-8265-36.ucode`(Dual Band Wireless-AC 8265用、APIの版36) | x64の`WIFI=1`のビルドが、`tools/x64/wifi/fw.sh`で取得したファイルをシステムボリュームの実身(`SYSDEF_FW_IWM8265_36`)のレコード1に変更せずに格納する。リポジトリには含めない | Intelの再配布の許諾(linux-firmwareの`LICENCE.iwlwifi_firmware`。OpenBSDのパッケージの`iwm-license`と同じ文。実身のレコード0に全文を入れる) |
| IntelのWi-Fiのファームウェア`iwlwifi-cc-a0-77.ucode`(Wi-Fi 6 AX200用、APIのバージョン77) | x64の`WIFI=1`のビルドが、`tools/x64/wifi/fw.sh`で取得したファイルをシステムボリュームの実身(`SYSDEF_FW_IWXCC_77`)のレコード1に変更せずに格納する。リポジトリには含めない | Intelの再配布の許諾(上と同じ`LICENCE.iwlwifi_firmware`。OpenBSDのパッケージの`iwx-license`と同じ文。実身のレコード0に全文を入れる) |
| IntelのWi-Fiのファームウェア`iwlwifi-Qu-b0-hr-b0-77.ucode`、`iwlwifi-Qu-c0-hr-b0-77.ucode`、`iwlwifi-QuZ-a0-hr-b0-77.ucode`(AX201用)、`iwlwifi-ty-a0-gf-a0-77.ucode`、`iwlwifi-ty-a0-gf-a0.pnvm`、`iwlwifi-so-a0-gf-a0-77.ucode`、`iwlwifi-so-a0-gf-a0.pnvm`(AX210、AX211用) | 同じ。実身は`SYSDEF_FW_IWXQUB_77`など(`tools/x64/wifi/mkobj.py`の`MORE`)。リポジトリには含めない | Intelの再配布の許諾(上と同じ。実身のレコード0に全文を入れる) |
| IntelのBluetoothのファームウェア`ibt-12-16.sfi`、`.ddc`(8265用)、`ibt-20-0-3`、`ibt-20-1-3`、`ibt-20-1-4`の`.sfi`、`.ddc`(AX200用) | x64の`WIFI=1`のビルドが、`tools/x64/wifi/fw.sh`で取得(linux-firmwareのタグ20251021、SHA-256はそのファイルにある)したファイルをシステムボリュームの実身のレコード1に変更せずに格納する。リポジトリには含めない | Intelの再配布の許諾(linux-firmwareの`LICENCE.ibt_firmware`。実身のレコード0に全文) |
| IntelのWi-Fiのファームウェア`iwlwifi-Qu-b0-jf-b0-77.ucode`、`iwlwifi-Qu-c0-jf-b0-77.ucode`、`iwlwifi-QuZ-a0-jf-b0-77.ucode`(AX201のPCIのIDを持つ9462、9560用)、`iwlwifi-3160-17.ucode`、`iwlwifi-3168-29.ucode`、`iwlwifi-7260-17.ucode`、`iwlwifi-7265-17.ucode`、`iwlwifi-7265D-29.ucode`、`iwlwifi-8000C-36.ucode`、`iwlwifi-9000-pu-b0-jf-b0-46.ucode`、`iwlwifi-9260-th-b0-jf-b0-46.ucode`(iwmの8265以外のカード用) | 同じ。実身は`SYSDEF_FW_IWXQUBJF_77`、`SYSDEF_FW_IWM3160_17`など(`tools/x64/wifi/mkobj.py`の`MORE`)。リポジトリには含めない | Intelの再配布の許諾(上と同じ`LICENCE.iwlwifi_firmware`。OpenBSDのパッケージの`iwm-license`、`iwx-license`と同じ文。実身のレコード0に全文を入れる) |
| MediaTekのWi-Fiのファームウェア`WIFI_MT7961_patch_mcu_1_2_hdr.bin`、`WIFI_RAM_CODE_MT7961_1.bin`、`WIFI_MT7961_patch_mcu_1a_2_hdr.bin`、`WIFI_RAM_CODE_MT7961_1a.bin`(MT7921、MT7920用)、`WIFI_MT7922_patch_mcu_1_1_hdr.bin`、`WIFI_RAM_CODE_MT7922_1.bin`(MT7922用) | 同じ。実身は`SYSDEF_FW_MWX*`。リポジトリには含めない | MediaTekの許諾(`LICENCE.mediatek`。OpenBSDのパッケージの`mwx-license`と同じ文。MediaTekのチップを持つ機器のための利用と再配布を認め、ファームウェア自身がGPLやLGPLの対象になる形を禁じる。カーネルにはリンクせず、独立した実身に置く。実身のレコード0に全文を入れる) |
| QualcommのWi-Fiのファームウェア`firmware-6.bin`(QCA6174 hw3.0〜hw3.2用、WLAN.RM.4.4.1-00309)と基板の較正のデータ`board-2.bin` | x64の`WIFI=1`のビルドが、`tools/x64/wifi/fw.sh`で取得したファイルをシステムボリュームの実身(`SYSDEF_FW_ATHX6174_FW`、`SYSDEF_FW_ATHX6174_BD`)のレコード1に変更せずに格納する。リポジトリには含めない | Qualcomm Atherosの許諾(linux-firmwareの`LICENSES/LICENSE.QualcommAtheros_ath10k`。Qualcomm Atherosのチップと一緒に使うためのバイナリの形での複製と再配布を、変更しないこと、著作権表示と免責の文を添えることなどの条件で認める)と、ファームウェアが含むソフトウェアの告知(`ath10k/QCA6174/hw3.0/notice_ath10k_firmware-6.txt`)。カーネルにはリンクせず、独立した実身に置く。実身のレコード0に、許諾と告知の全文を入れる |
| NetBSD 11.0のRaspberry Pi 5とRaspberry Pi 4の系統のWi-Fiの部分(rumpカーネルのデバイスの層`librumpdev`と`kern_pmf.c`、SD/SDIOの層`sys/dev/sdmmc`の`sdmmc.c`、`sdmmc_cis.c`、`sdmmc_io.c`、`sdmmc_mem.c`、SD Host Controllerのドライバ`sdhc.c`、BroadcomのFullMACのドライバbwfm(4)(`sys/dev/ic/bwfm.c`、`sys/dev/sdmmc/if_bwfm_sdio.c`)、IEEE 802.11の層`sys/net80211`、AES(`sys/crypto/aes`)) | Raspberry Pi 5の`WIFI=1`(既定)とRaspberry Pi 4の系統の`WIFI=1`のビルドのカーネルに、`tools/netbsd/build.sh`(`WIFI=1`)がネットワーク部分と一つにビルドしたオブジェクトとしてリンクする。ソースはリポジトリに含めない | sdmmcとsdhcはISC(Uwe Stuehler)と、NetBSDで足された部分のBSD 2条項(NONAKA Kimihiro)。bwfmはISC(Broadcom Corporation、Patrick Wildt)。net80211、AES、rumpの部分は上のx64のWi-Fiの行と同じ。`tools/netbsd/build.sh notices`が書く`NOTICE-netbsd.txt`に、コンパイルしたファイルの著作権表示をまとめる |
| OpenBSD 8.0のRaspberry Piの系統のWi-Fiの部分(SD/SDIOの層`sys/dev/sdmmc`の`sdmmc.c`、`sdmmc_cis.c`、`sdmmc_io.c`、`sdmmc_mem.c`、SD Host Controllerのドライバ`sdhc.c`、BroadcomのFullMACのドライバbwfm(4)(`sys/dev/ic/bwfm.c`、`sys/dev/sdmmc/if_bwfm_sdio.c`)) | `WIFI_NET80211=obsd`を指定したRaspberry Pi 5とRaspberry Pi 4の系統のビルドのカーネルに、`tools/openbsd/build.sh`(`ARCH=aarch64`)が層と一つにビルドしたオブジェクトとしてリンクする。既定の`nbsd`には入らない。ソースはリポジトリに含めない | ISC。`tools/openbsd/build.sh notices`が書く`NOTICE-openbsd.txt`に、コンパイルしたファイルの著作権表示をまとめる |
| OpenBSD 8.0のWi-Fiの部分(IEEE 802.11の層`sys/net80211`の全部、Intelの無線のドライバiwm(4)(`sys/dev/pci/if_iwm.c`)とiwx(4)(`sys/dev/pci/if_iwx.c`)、MediaTekの無線のドライバmwx(4)(`sys/dev/pci/if_mwx.c`)、OpenBSDのカーネルの`kern/uipc_mbuf.c`、`kern/uipc_mbuf2.c`、`kern/subr_tree.c`、`net/if_media.c`、`lib/libkern/fls.c`、`sys/crypto`の`aes.c`、`cmac.c`、`hmac.c`、`md5.c`、`sha1.c`、`sha2.c`、`key_wrap.c`、`michael.c`、`arc4.c`と、それらのヘッダ) | x64の`WIFI_NET80211=obsd`のビルドのカーネルに、`tools/openbsd/build.sh`が層(`device/net/wifi/obsd/`)とともにビルドしたオブジェクト(`~/tessronos-obsd-x64/obsdwifi.o`)としてリンクする。TessronOSのathx(上の「リポジトリに含まれるもの」、ISC)も同じオブジェクトに入る。既定のビルドには入らない。ソースはリポジトリに含めない | iwmとiwxはISC(genua gmbh、Fixup Software、Stefan Sperling、Damien Bergamini ほか)と、IntelのiwlwifiのBSDとGPL 2のデュアルの部分(BSDを選択する)。mwxはISC(Claudio Jeker、MediaTek Inc.、Lorenzo Bianconi、Stefan Sperling、Felix Fietkau、Damien Bergamini)。net80211はBSD 2条項(Atsushi Onoe、Sam Leffler)とISC。mbuf、木、媒体とカーネルのヘッダはBSD 2条項、3条項とISCで、一部はBSD 4条項(宣伝の条項がある。下の「表示」)。暗号は、AESがMIT(Thomas Pornin)、SHA-2がBSD 3条項(Aaron D. Gifford)、CMACとMichaelと鍵の包みがISC、MD5とSHA-1がパブリックドメイン。GPLだけのファイルは含まない。`tools/openbsd/build.sh notices`が書く`NOTICE-openbsd.txt`に、コンパイルしたファイルの著作権表示をまとめる |
| Raspberry Pi 5とRaspberry Pi 4の系統のWi-Fiのチップ(CYW43455)のファームウェア`cyfmac43455-sdio.bin`と地域の規則のデータ`cyfmac43455-sdio.clm_blob` | Raspberry Pi 5と、Raspberry Pi 4の系統の`WIFI=1`のビルドが、`tools/rpi5/wifi/fw.sh`で取得したファイルを、システムボリュームの実身(`SYSDEF_FW_BWFM43455`、`SYSDEF_FW_BWFM43455_CLM`)のレコード1に変更せずに格納する。リポジトリには含めない | Cypressの許諾(linux-firmwareの`LICENSES/LICENCE.cypress`)。Cypressの集積回路の製品と使う目的に限り、オブジェクトコードの形で複製と配布ができる。改変と逆アセンブルなどの解析は許されない(実身のレコード0に許諾の全文を入れる) |
| Raspberry Pi 5の内蔵Bluetooth(CYW43455のBluetooth、BCM4345C0)のパッチ`BCM4345C0.hcd` | Raspberry Pi 5の`WIFI=1`のビルドが、`tools/rpi5/wifi/fw.sh`で取得したファイルを、システムボリュームの実身(`SYSDEF_FW_BCM4345C0`)のレコード1に変更せずに格納する。リポジトリには含めない | 上と同じCypressの許諾。Raspberry Piがbluez-firmwareの`debian/copyright`で、linux-firmwareの`LICENCE.cypress`が当たると述べている(実身のレコード0に許諾の全文を入れる) |
| Raspberry Pi 5とRaspberry Pi 4の系統のWi-Fiの基板の設定(NVRAM、`brcmfmac43455-sdio.raspberrypi,4-model-b.txt`。linux-firmwareでRaspberry Pi 5、Pi 500、Compute Module 5の名前がこのファイルへの別名になっているもの) | Raspberry Pi 5と、Raspberry Pi 4の系統の`WIFI=1`のビルドが、`tools/rpi5/wifi/mkobj.py`で文章の実身(`SYSDEF_FW_BWFM43455_NV`、「Ｗｉ－Ｆｉの基板の設定（Ｒａｓｐｂｅｒｒｙ　Ｐｉ　５）」、Pi 4の系統は末尾が「４」)にする。行ごとに1つの段落で、内容は変えない。出典とライセンスを冒頭の注記の段落とメタデータ(`tessronos.firmware`)に書き、GPL-2.0の全文をレコード1に入れる。リポジトリには含めない | GPL-2.0以降(ファイルの`SPDX-License-Identifier: GPL-2.0+`。Copyright (C) 2018 Raspberry Pi (Trading) Ltd.)。文章そのものがソースなので、イメージを配布するときはこの実身を含めればソースを添えたことになる |
| newlib(C標準ライブラリと数学関数) | xPackツールチェーン付属のものをプロセス用プログラムにリンクする | BSD系ライセンスの集合(newlibの`COPYING.NEWLIB`)。イメージを配布するときは著作権表示を添付する |
| libstdc++、libgcc | 同上。C++のプログラムにリンクする | GPLv3とGCC Runtime Library Exception。この例外により、リンクしたプログラムの配布条件は制約されない |
| Mozc | `make MOZC=1`のときだけ、`tools/mozc/build.sh`でビルドした変換エンジンをリンクする | 修正BSD(3条項) |
| Mozcの辞書データ | `make MOZC=1`のときだけ、辞書の実身のレコードに格納される | Mozcのデータのライセンス(IPAdic由来の部分を含む)。Mozcの`LICENSE`と`data/`以下の各ファイルを参照 |
| 文字検索の辞書(KANJIDIC2、KRADFILE、RADKFILE、KanjiVG、Unihan、Unicode Character Databaseから作ったデータ) | `tools/gen_chardic.py`がビルドのときに作り、実身「文字検索の辞書」(`SYSDEF_DICT_CHAR`)のレコード1に格納される。取得したデータがないときは、Pythonの文字コードの表と`unicodedata`だけから作る | 辞書はKANJIDIC2とKRADFILE(Electronic Dictionary Research and Development Group、CC BY-SA 4.0)、KanjiVG(Copyright (C) 2009-2013 Ulrich Apel、CC BY-SA 3.0)の派生物なので、CC BY-SA 4.0で配布する。UnihanとUnicode Character DatabaseはUnicode License v3 |
| tessron-chromium(ブラウザのアプリケーションと、ChromiumをTessronOSに載せる移植層) | `make BROWSER=1`のときだけ、ブラウザの実行ファイルにリンクし、プログラム「ブラウザ」の実身のレコード1に格納される | MIT(`tessron-chromium/LICENSE`) |
| Chromium 153.0.8010.36(Blink、V8、Skia、`base`、`net`、`cc`、`mojo`など)と、その`third_party`のうちコンパイルしたもの(ICU、HarfBuzz、FreeType、libpng、libjpeg-turbo、libwebp、Wuffs、zlib、Brotli、BoringSSL、QUICHE(QUICとHTTP/3)、abseil、Perfetto、Highway、simdutf、dav1d、libyuv、LZMA SDKのXZの復号器など) | `make BROWSER=1`のときだけ、`tessron-chromium`のスクリプトがコンパイルし直してブラウザの実行ファイルに静的にリンクし、プログラム「ブラウザ」の実身のレコード1に格納される | Chromium、V8、Skia、QUICHEはBSD 3条項。Blinkの一部のファイル(WebKitとKHTMLに由来するもの)はLGPL 2.1(またはLGPL 2)とBSDのデュアル。`third_party`はそれぞれのライセンス(ICUはUnicode License、BoringSSLはOpenSSL・ISC・Apache 2.0の組合せ、abseilとPerfettoはApache 2.0、dav1dはBSD 2条項、LZMA SDKはパブリックドメインなど) |
| Chromiumが使うRustのクレート(Rustの標準ライブラリ、jxl-rs、CrabbyAvif、Fontations、serde_json_lenient、RustFFTなど) | `make BROWSER=1`のときだけ、aarch64向けにビルドしてブラウザの実行ファイルにリンクする | 多くはMITとApache 2.0のデュアル。クレートごとの`LICENSE`による |
| ChromiumのFFmpeg(チェックアウトの`third_party/ffmpeg`。libavcodec、libavformat、libavutil。ChromiumがChromeの構成(`ffmpeg_branding = "Chrome"`)として選んだ分離器と復号器で、H.264とAACを含む) | `make BROWSER=1 BROWSER_BLINK=1`のときだけ、`tessron-chromium`のスクリプトがコンパイルし直し(NEONのアセンブリを含む)、ブラウザの実行ファイルに静的にリンクする。GPLの部分は含まない | LGPL 2.1以降 |
| libvpx、libopus(チェックアウトの`third_party/libvpx`、`third_party/opus`) | 同上。VP8とVP9、Opusの復号に使う | libvpxはBSD 3条項、libopusはBSD 3条項 |
| PDFium(チェックアウトの`third_party/pdfium`。その`third_party`のAGG、Little CMS、OpenJPEG、LibTIFF、bigintを含む) | 同上。ページに表示するPDFを描くのに使う | PDFiumはBSD 3条項とApache 2.0(`LICENSE`)。AGGは独自の許諾(著作権表示を残す条件)、Little CMSはMIT、OpenJPEGはBSD 2条項、LibTIFFはLibTIFFの許諾(BSD型)、bigintはパブリックドメイン |
| libaom(チェックアウトの`third_party/libaom`) | 同上。WebCodecsとMediaRecorderのAV1の符号化に使う(復号はdav1d) | BSD 2条項(`LICENSE`)と、Alliance for Open Mediaの特許の許諾(`PATENTS`) |
| CLD3とProtocol Buffersの軽量版(チェックアウトの`third_party/cld_3`、`third_party/protobuf`の`protobuf_lite`) | 同上。組み込みのAIの`LanguageDetector`が文の言語を推定するのに使う(CLD3のニューラルネットワークの重みはソースに含まれる) | CLD3はApache 2.0、Protocol BuffersはBSD 3条項 |
| 音声の合成と認識: Open JTalk 1.11(その中のMeCab 0.996を含む)、hts_engine API 1.10、Open JTalkの辞書(naist-jdic 1.11、UTF-8)、HTSの音声`nitech_jp_atr503_m001` 1.05、Flite 2.2とその英語の音声`cmu_us_slt`、whisper.cpp 1.7.6とggml、Whisperのtinyのモデル(`ggml-tiny-q5_1.bin`) | `make BROWSER=1 BROWSER_BLINK=1`のときだけ、`tessron-chromium/tools/browser/speech.sh`が取得したソースをコンパイルし直してブラウザの実行ファイルにリンクし、辞書、音声、モデルをプログラム「ブラウザ」の実身のレコード2に入れる。`speechSynthesis`と`SpeechRecognition`に使う。ソースとデータはリポジトリに含めない | Open JTalk、hts_engine APIは修正BSD(名古屋工業大学)。MeCabはOpen JTalkが収めたもので修正BSD(工藤拓、日本電信電話)。naist-jdicは修正BSD(奈良先端科学技術大学院大学)。nitech_jp_atr503_m001はCC BY 3.0(名古屋工業大学。帰属の表示が要る)。FliteはBSD型の許諾(カーネギーメロン大学。Festival由来のファイルはCMUとエディンバラ大学の共同の著作権で同じ許諾、G.72xの符号化はSun Microsystemsの無制限の使用の許諾)、cmu_us_sltも同じ許諾。whisper.cppとggmlはMIT(The ggml authors)。WhisperのモデルはOpenAIがMITで公開したもの |
| hunspell(チェックアウトの`third_party/hunspell`)と英語の辞書(`en_US`、SCOWLの単語の表から作ったもの。Debianの`hunspell-en-us` 2020.12.07の`en_US.aff`、`en_US.dic`を`tessron-chromium/tools/browser/spell`に置く) | `make BROWSER=1 BROWSER_BLINK=1`のときだけ、ブラウザの綴りの検査に使い、実行ファイルにリンクする | hunspellはMPL 1.1、GPL 2.0、LGPL 2.1のトリプル(MPL 1.1で使う)。辞書はSCOWLの許諾(著作権の表示と許諾の表示を残せば、使用、複製、改変、配布、販売を許す。`tools/browser/spell/COPYING`) |
| Mesa 26.2.3のOpenGL ES(EGL、OpenGL ESの状態管理、GLSLコンパイラ、NIR、Gallium3Dの補助、softpipeとv3dのドライバ、V3Dのシェーダコンパイラ、`src/util`) | `make GL=1`のときだけ、`tools/mesa/build.sh`が取得したソースの写しにパッチ(`tools/mesa/patches`)を当ててコンパイルし直し、移植層(`lib/libgl`)とともに静的ライブラリ`libtsgl.a`にして、OpenGL ESを使うプログラム(小物「歯車」の実身のレコード1、試験の`GLPROG.ELF`、`BROWSER=1 BROWSER_BLINK=1`のときはブラウザ)に静的にリンクする | 大半はMIT。Khronosのヘッダ(`include/EGL`、`include/GLES*`の`*platform.h`、`include/vulkan`、`include/vk_video`)はApache 2.0、`src/c11/impl/threads_posix.c`はBoost Software License 1.0、`src/util/softfloat.c`はBSD 3条項(Berkeley SoftFloat)、`src/util/xxhash.h`はBSD 2条項、`src/util/blake3`はCC0 1.0とApache 2.0(LLVMの例外付きを含む)から選べるもの。GLSLとARBのプログラムの構文解析器(`glsl_parser.cpp`、`glcpp-parse.c`、`program_parse.tab.c`)はビルドのときにホストのBisonが生成したもので、GPL 3.0以降にBisonの例外2.2が付き、例外により生成物は任意の条件で配布できる。GPLだけのファイルは含まない |
| ANGLE(Chromiumのチェックアウトの`third_party/angle`。EGLとOpenGL ESの入口(`GL_`、`EGL_`の名前のもの)、`libANGLE`、シェーダの変換器(GLSL、ESSL、WGSLの出力)と前処理器、OpenGL ESのバックエンド、WebGPUのバックエンド(WebGL on WebGPU)、`src/common`、画像とGPU情報の補助)と、Chromiumの`ui/gl`(GLとEGLの関数表、ANGLEのプラットフォーム) | `make GL=1 BROWSER=1 BROWSER_BLINK=1`のときだけ、`tessron-chromium`のスクリプト(`tools/browser/build.sh`の`angle_lib`)がコンパイルし直し、Mesaとともにブラウザの実行ファイルに静的にリンクする | BSD 3条項(ANGLEの`LICENSE`)。`src/common/third_party/xxhash`はBSD 2条項、`src/common/base/anglebase`はChromiumのBSD 3条項。シェーダの構文解析器(`glslang_tab_autogen.cpp`)はANGLEがBisonで生成して収めたもので、Bisonの例外2.2により任意の条件で配布できる |
| Raspberry Pi 5のデバイスツリー(`bcm2712-rpi-5-b.dtb`) | `make sdimg`がRaspberry Piのファームウェア配布物から取得し、SDカードイメージに格納する。リポジトリには含めない | 元のソースコード(Raspberry Pi版Linuxの`bcm2712-rpi-5-b.dts`ほか)はGPL-2.0とMITのデュアルライセンス。MITを選択し、イメージを配布するときはその表示を添付する |
| Raspberry Pi 4系のファームウェア(`start4.elf`、`fixup4.dat`)とデバイスツリー(`bcm2711-rpi-4-b.dtb`、`bcm2711-rpi-400.dtb`、`bcm2711-rpi-cm4.dtb`、`overlays/disable-bt.dtbo`) | `make TARGET=_RPI4_ sdimg`がRaspberry Piのファームウェア配布物から取得し、SDカードイメージに格納する。リポジトリには含めない | ファームウェアはBroadcomのライセンス(配布物の`LICENCE.broadcom`。バイナリの形でだけ再配布でき、イメージを配布するときはその表示を添付する)。デバイスツリーは上と同じ |
| FFmpeg 4.4.6(libavcodec、libavformat、libavutil、libswresample)とzlib | `etc/pkg`(x64は`etc/pkg/x64`)に置いたパッケージ`mpegplay.tpk`をイメージに組み込むとき、プログラム「MPEG Player」の実行ファイルに静的にリンクされて入る。FFmpegはGPLの部分とnonfreeの部分を有効にせず、復号器・分離器・パーサだけを有効にしてビルドしている | FFmpegはLGPL 2.1以降。zlibはzlib License |
| minimp3、miniz | `etc/pkg`(x64は`etc/pkg/x64`)に置いたパッケージ`pcmplay.tpk`をイメージに組み込むとき、プログラム「PCM Player」の実行ファイルにリンクされて入る | minimp3はCC0 1.0、minizはMIT |

## 取得して使うもの(リポジトリにもイメージにも含まれない)

| 名称 | 使い方 | バージョン | ライセンス |
| --- | --- | --- | --- |
| ACPICA | `tools/x64/acpica/build.sh`がリリースの`acpica-unix-20260930.tar.gz`を`~/acpica`に取得し(SHA-256を検査する)、ヘッダの写しの`acenv.h`の1行だけを変えてコンパイルする。取得したソースは書き換えない | 20260930 | BSD 3条項(GPL 2.0とのデュアル) |
| NetBSD | `tools/netbsd/build.sh`が`syssrc.tgz`を取得し、ネットワークスタックに必要なディレクトリだけを`~/netbsd/netbsd-11.0`に展開してビルドする。ソースは変更しない(`UPSTREAM.md`) | 11.0 | BSDほか(上記) |
| Chromium | WSLのホームのチェックアウト(`~/cr6/src`、DEPSで固定した依存を含む)を、`tessron-chromium`のスクリプトが読んでコンパイルし直す。ソースは書き換えず、変更は`tessron-chromium/tools/browser/patches`のパッチを写しに当てる(`UPSTREAM.md`) | 153.0.8010.36 | BSD 3条項ほか(上記) |
| 音声の合成と認識のソース | `tessron-chromium/tools/browser/speech.sh`がSourceForge(Open JTalk、hts_engine API、辞書、音声)、GitHub(Flite、whisper.cpp)、Hugging Face(モデル)から取得し、`~/speech`に展開してコンパイルする。辞書と音声のファイルを実行ファイルのデータから読む部分だけ、写しを書き換える | Open JTalk 1.11、hts_engine API 1.10、Flite 2.2、whisper.cpp 1.7.6 | 修正BSDほか(上記) |
| OpenBSD | `tools/openbsd/build.sh`がOpenBSD 8.0の`sys.tar.gz`を新しい空のディレクトリに取得し、SHA-256(リリースの`SHA256`の値)を検査してから`~/openbsd/dl/8.0`に置き、要るディレクトリだけを`~/openbsd/src-8.0`に展開してビルドする。ソースは変更しない | 8.0 | ISC、BSDほか(上記) |
| wpa_supplicant | `tools/wpa/build.sh`が、NetBSDの`src.tgz`(`~/netbsd/dl`。SHA-512を検査する)から`external/bsd/wpa/dist`を`~/netbsd/wpa-11.0`に展開してコンパイルする。ソースは変更しない。`ap`は同じソースとhostapdの認証側をWi-Fiのシミュレータ(`WIFI_SIM=1`)の仮想のAPのためにカーネルと同じ形で建て、`test`は同じソースとhostapdの認証側をホストのコンパイラで建てて試験し、`modtest`は上流のmodule testsを同じソースで走らせる(どちらもイメージに入らない) | 2.11 | BSD 3条項 |
| `crypto_mbedtls.c` | `tools/wpa/build.sh`が、OpenWrtのタグv24.10.0の`package/network/services/hostapd/patches/110-mbedtls-TLS-crypto-option-initial-port.patch`(SHA-256 `93d3f88cdc5f72be4be74e0f0dbff412e803a1427abbcdf4d323645dcc29883a`)を`~/netbsd/dl`に取得し、当てが新しく作る`src/crypto/crypto_mbedtls.c`の行だけを`~/netbsd/wpa-11.0/crypto_mbedtls.c`に取り出す(4043行、SHA-256 `3ad41315f6502721fb520d42885338d637c368dcab3fcc20d3d9201007e4287d`)。当ての残り(hostapdの作り方の変更)は使わない。ファイルは変更しない | OpenWrt v24.10.0 | BSD 3条項 |
| IntelのWi-Fiのファームウェア | `tools/x64/wifi/fw.sh`がlinux-firmwareのタグ20230117から`iwlwifi-8265-36.ucode`(OpenBSD 8.0の`iwm-firmware-20240410`の`iwm-8265-36`と同じバイト列)と`LICENCE.iwlwifi_firmware`を、タグ20251021から`intel/iwlwifi/iwlwifi-cc-a0-77.ucode`(OpenBSD 8.0の`iwx-firmware-20251021p0`の`iwx-cc-a0-77`と同じバイト列)とAX201、AX210、AX201のIDの9462と9560のファイル(同じパッケージの同じ名前のファイルと同じバイト列)を、iwmの8265以外のカードのファイルをタグ20230117と20240312(9000と9260)から(`iwm-firmware-20240410`と同じバイト列)、新しい空のディレクトリに取得し、SHA-256と大きさを検査してから`~/tessronos-wifi-fw`に置く | linux-firmware 20230117、20240312、20251021 | Intelの再配布の許諾(上記) |
| MediaTekのWi-Fiのファームウェア | `tools/x64/wifi/fw.sh`がlinux-firmwareのタグ20260309から`mediatek/`のファイルと`LICENCE.mediatek`(OpenBSD 8.0の`mwx-firmware-20260309p0`と同じバイト列)を、同じ手順で取得する | linux-firmware 20260309 | MediaTekの許諾(上記) |
| QualcommのWi-Fiのファームウェア | `tools/x64/wifi/fw.sh`がlinux-firmwareのタグ20261019から`ath10k/QCA6174/hw3.0/firmware-6.bin`、`board-2.bin`、`notice_ath10k_firmware-6.txt`と`LICENSES/LICENSE.QualcommAtheros_ath10k`を、同じ手順で取得する | linux-firmware 20261019 | Qualcomm Atherosの許諾(上記) |
| Linuxのath10k(athxを書いた元) | 開発の時に、git.kernel.orgの`linux.git`(stable)のタグv7.3.1から`drivers/net/wireless/ath/ath10k`の要るファイルと`drivers/net/wireless/ath/regd.c`、`regd.h`、`regd_common.h`、`ath.h`を取得して読み、写した。ビルドの時には取得しない | v7.3.1 | ISC |
| Raspberry Pi 5とRaspberry Pi 4の系統のWi-Fiのファームウェアと基板の設定 | `tools/rpi5/wifi/fw.sh`がlinux-firmwareのタグ20260810から`cypress/cyfmac43455-sdio.bin`、`cypress/cyfmac43455-sdio.clm_blob`、`brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.txt`、`LICENSES/LICENCE.cypress`、`LICENSES/GPL-2.0`を新しい空のディレクトリに取得し、SHA-256と大きさを検査してから`~/tessronos-rpi5-wifi-fw`に置く | linux-firmware 20260810 | Cypressの許諾、GPL-2.0以降(上記) |
| Raspberry Pi 5の内蔵Bluetoothのパッチ | `tools/rpi5/wifi/fw.sh`がRaspberry Piのbluez-firmware(github.com/RPi-Distro/bluez-firmware)のコミットcdf61dc691a49ff01a124752bd04194907f0f9cdから`debian/firmware/broadcom/BCM4345C0.hcd`を同じ手順で取得し、`~/tessronos-rpi5-wifi-fw`に置く | bluez-firmware cdf61dc6 | Cypressの許諾(上記) |
| 文字検索の辞書の元データ | `tools/gen_chardic.py fetch ~/chardic`が、Unihan(<https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip>)、KANJIDIC2(<http://www.edrdg.org/kanjidic/kanjidic2.xml.gz>)、KRADFILE(<http://ftp.edrdg.org/pub/Nihongo/kradzip.zip>)、KanjiVG(<https://github.com/KanjiVG/kanjivg/releases>)を変更せずに取得する。`make`が`tools/gen_chardic.py make`で辞書のレコードに変換する | Unihanは最新版、KanjiVGはr20260714 | Unicode License v3、CC BY-SA 4.0、CC BY-SA 4.0、CC BY-SA 3.0 |
| Mesa | `tools/mesa/build.sh`が`~/mesa`に取得し(SHA-256を検査する)、ファイルをハードリンクした写し(`~/tsmesa/src`)にパッチを当て、ホストのmesonが書いたコンパイルのコマンドをプロセス用に書き換えてOpenGL ESの静的ライブラリにする(`make GL=1`、上記)。取得したソースは書き換えない。`tools/mesa/probe.sh`が取得し、一部をプロセス用のツールチェーンでコンパイルできるかを試す。`tools/mesa/shaders.sh`が`~/mesa`に取得してNIR、V3Dのシェーダコンパイラ、`src/util`をホスト向けに建て、合成のシェーダを生成するホストのプログラム(`tools/mesa/gen_cp_shaders.c`、`test_cp_shaders.c`)にリンクする。これらのプログラムはイメージに入らない。GPUのドライバの設計ではジョブの要求の項目と識別ワードの意味を、実機の試験ではフレームの組み立て方を、合成ではレコードと状態のパケットの設定を参照した(`UPSTREAM.md`) | 26.2.3 | MIT(一部のファイルは他のライセンス) |
| WidevineのCDM(Googleの`libwidevinecdm.so`と`manifest.json`。Raspberry Piの公式のリポジトリ<https://archive.raspberrypi.com/debian>のパッケージ`libwidevinecdm0`、arm64) | TessronOSにもブラウザにも同梱せず、リポジトリにもイメージにも入れない。利用者がブラウザのメニューの〈Widevineを導入...〉を選び、Googleの独自のソフトウェアで同梱していないこと、入手元、利用の条件は利用者が確かめることを示すパネルで〈導入〉を選んだときだけ、ブラウザがそのリポジトリからHTTPSで取得し、リポジトリの索引のSHA-256と照らしてから、その利用者の実身に入れる(設計書14.6.3.1)。CDMは変更も解析もせず、Chromiumの正規のインタフェース(`media::CdmModule`、`media::CdmAdapter`)で読み込んで使う。〈Widevineを外す〉で消える。開発では、試験のために開発の機械で取得したものを試験のディスクにだけ載せ、試験が済んだら消す | 利用者が導入するときにリポジトリにあるバージョン(2026年10月は4.10.2662.3+1) | Googleの独自のライセンス(プロプライエタリ)。利用の条件は導入する利用者が確かめる |

Mesaをリンクしたプログラムを配布するときは、MITライセンスの著作権表示と許諾文を添付する。`V3D=2`と`V3D=3`でビルドしたカーネルには、上記の生成した2つのヘッダから作られたコードとデータが含まれる。x64のカーネル(`TARGET=_X64_`)には、`device/gpu/intel/mesa/igt_cp_shaders.h`から作られたコードとデータが含まれる。`make GL=1`でビルドしたイメージを配布するときは、さらにMesaのファイルの著作権表示とMITの許諾文、Apache License 2.0の全文(Khronosのヘッダ)、Boost Software License 1.0の全文、Berkeley SoftFloatのBSD 3条項とxxHashのBSD 2条項の著作権表示と条件を添付する。リンクしたファイルのライセンスは、`~/tsmesa/obj`の依存の記録(`.d`)に挙がったソースとヘッダの先頭を読んで確かめた。

BeSFC(snes9xに由来するコードとLakeSnes(MIT)に由来するコードを含む)のコアと拡張チップは、このリポジトリにも、システムのイメージにも、システムの.tpkにも含まれない。`make besfcpkg`が別のリポジトリから取り込み、BeSFCの.tpkにだけ入る。そのパッケージは全体としてsnes9xのライセンスに従い、非商用に限る(そのリポジトリの`LICENSES/`、パッケージの実身「BeSFCのライセンス」)。このリポジトリの`application/befc/emu_main.c`のBeSFCの部分と`emu_pad.h`はTessronOSのために書いたコードで、T-Licenseである。

## 表示

FreeTypeのライセンス(FTL)は、FreeTypeを使ったソフトウェアのドキュメントに次の一文を記載することを求めている。

Portions of this software are copyright © 2024 The FreeType Project (<https://freetype.org>). All rights reserved.

文字検索の辞書の実身はCC BY-SA 4.0の派生物である。イメージとともに配布するときは、元のデータとその著作権者(KANJIDIC2とKRADFILEはElectronic Dictionary Research and Development Group、<http://www.edrdg.org/edrdg/licence.html>。KanjiVGはUlrich Apel、<http://kanjivg.tagaini.net>)、ライセンス(<https://creativecommons.org/licenses/by-sa/4.0/>)、データを変換したこと(`tools/gen_chardic.py`)を表示し、受け取った人が同じ条件で再配布できるようにする。この表示は辞書の実身のレコード0の文章にも書いてある。辞書を使うプログラム(文字検索)と変換のスクリプトは辞書を含まないので、この条件は及ばない。

lwIPのライセンスは、バイナリを配布するときに著作権表示、ライセンス条件、免責事項をドキュメントに記載することを求めている。`NETSTACK=lwip`でビルドしたイメージを配布するときは`peripheral_kernel/network/lwip/COPYING`の全文を添付する。

NetBSDのファイルのBSDライセンスも、バイナリを配布するときに著作権表示、ライセンス条件、免責事項をドキュメントに記載することを求めている。`tools/netbsd/build.sh`は、スタックにコンパイルしたNetBSDのすべてのソースとヘッダの著作権表示とライセンス条件を、重複を除いて`~/tessronos-netbsd/NOTICE-netbsd.txt`にまとめる(NetBSD全体の著作権表示`sys/conf/copyright`を含む)。イメージを配布するときはこのファイルを添付する。MD4とMD5のコード(`common/lib/libc/md`)は、RSA Data Securityのライセンスに従い、「RSA Data Security, Inc. MD4 Message-Digest Algorithm」「RSA Data Security, Inc. MD5 Message-Digest Algorithm」に由来することを表示する。

`WIFI_NET80211=obsd`でビルドしたイメージでは、`tools/openbsd/build.sh notices`が、そのオブジェクトにコンパイルしたOpenBSDのすべてのソースとヘッダの著作権表示とライセンス条件を、重複を除いて`~/tessronos-obsd-x64/NOTICE-openbsd.txt`にまとめる。イメージを配布するときはこのファイルも添付する。

BSD 4条項のファイルは、その機能や使用に触れる宣伝の資料に次の表示を求めている。

- This product includes software developed by the University of California, Berkeley and its contributors.
- This product includes software developed by the University of California, Lawrence Berkeley Laboratory and its contributors.
- This product includes software developed by the NetBSD Foundation, Inc. and its contributors.
- This product includes software developed for the NetBSD Project. See http://www.NetBSD.org/ for information about NetBSD.
- This product includes software developed for the NetBSD Project by Wasabi Systems, Inc.
- This product includes software developed by Christopher G. Demetriou for the NetBSD Project.
- This product includes software developed by Jonathan Stone and Jason R. Thorpe for the NetBSD Project.
- This product includes software developed by Jonathan Stone for the NetBSD Project.
- This product includes software developed at the Information Technology Division, US Naval Research Laboratory.
- This product includes software developed by the Alice Group.
- This product includes software developed by Brini.
- This product includes software developed by Emmanuel Dreyfus.
- This product includes software developed by Charles Hannum.
- This product includes software developed by Jason L. Wright.
- This product includes software developed by Rolf Grossmann.
- This product includes software developed by Paul Mackerras <paulus@samba.org>.
- This product includes software developed by QUALCOMM Incorporated.
- This product includes software developed by Bill Paul.

イメージ(`tessronos.img`、`sd.img`)を配布するときに添付するものは、T-License 2.2(`LICENSE`)、NetBSDの著作権表示とライセンス条件の一覧(`NOTICE-netbsd.txt`。`NETSTACK=lwip`ならlwIPの`COPYING`)、FreeTypeの上記の一文、Mbed TLSの`LICENSE`、liblc3の`LICENSE`(`lib/liblc3/LICENSE`)、Noto Sans JP、Noto Serif JP、Noto Sans Mono CJK JPの`OFL.txt`、CA証明書リストのMPL 2.0の入手先、newlibの`COPYING.NEWLIB`の表示、デバイスツリーのMITの表示、`MOZC=1`でビルドしたときはMozcとその辞書データのライセンス、文字検索の辞書のCC BY-SA 4.0の表示(上記)、`V3D=2`か`V3D=3`でビルドしたときは`device/gpu/v3d/mesa/v3d_packet_v71_pack.h`と`device/gpu/v3d/mesa/v3d_cp_shaders.h`の先頭にあるMesaの著作権表示と許諾文、x64のイメージでは`device/gpu/intel/mesa/igt_cp_shaders.h`の先頭にあるMesaの著作権表示と許諾文である。

`BROWSER=1`でビルドしたイメージを配布するときは、さらに次のものが要る。

- tessron-chromiumの`LICENSE`(MITライセンスの著作権表示と許諾文)。
- Chromiumと、コンパイルした`third_party`とRustのクレートの著作権表示とライセンス文。Chromiumの`tools/licenses/licenses.py`(`credits`)が、ビルドの記述(`out/tessronos`)から一覧を作る。
- LGPLの部分(FFmpegと、Blinkのファイルのうち、LGPLのもの)について、LGPL 2.1の第6節の条件。ブラウザは静的にリンクしているので、利用者が改変したライブラリとリンクし直せるように、ブラウザの実行ファイルのオブジェクトファイル(またはソースコード)と、リンクの手順を提供する。FFmpeg(Chromiumのチェックアウトの`third_party/ffmpeg`)とChromiumの対応するソースコードの入手先も示す。ブラウザのFFmpegはH.264とAACの復号器を含むので、その特許の扱いは配布する者が確かめる。

`etc/pkg`のパッケージを組み込んだイメージを配布するときは、さらに次のものが要る。

- MPEG Playerについて、FFmpegのLGPL 2.1の第6節の条件。静的にリンクしているので、利用者が改変したFFmpegとリンクし直せるように、MPEG Playerのオブジェクトファイル(またはソースコード)とリンクの手順を提供し、FFmpeg 4.4.6の対応するソースコードの入手先を示す。zlibの著作権表示も添付する。
- PCM Playerについて、minizのMITライセンスの著作権表示と許諾文。

## μT-Kernel 3.0の派生物を配布するときの手続き

T-License 2.2は、改変したソースコードの派生物を第三者へ再配布するとき、トロンフォーラムのトレーサビリティサービスに登録してディストリビューションucodeを取得すること、本ライセンス契約を添付すること(第3条第2項第6号)、ソースコードを利用した旨を表示すること(第3条第6項)を求めている。TessronOSは登録済みで、ディストリビューションucodeは0007006Aである。ucodeとソースコードを利用した旨はREADME.mdに表示し、本ライセンス契約は`LICENSE`として添付する。

## USBのDWC2のドライバについて

`device/usb/dwc2.c`はTessronOSのために書いたもので、ほかのソースのコードは含まない。初期化の順は、次のBSDライセンスのドライバで確かめた。GPLのコードは写していない。NetBSDのものはLinuxと同じSynopsysのコードで、BSDとGPLの二重のライセンスであり、BSDの条件で参照した。

| 参照したもの | 著作権 | ライセンス |
| --- | --- | --- |
| NetBSD 11.0 `sys/external/bsd/dwc2/dist/dwc2_core.c` | Copyright (C) 2004-2013 Synopsys, Inc. | BSD-3-Clause(GPLとの選択) |
| FreeBSD `sys/dev/usb/controller/dwc_otg.c` | Copyright (c) 2015 Daisuke Aoyama、Copyright (c) 2012-2015 Hans Petter Selasky、Copyright (c) 2010-2011 Aleksandr Rybalko | BSD-2-Clause |

## `WG=1`のビルドを配布するとき

`make WG=1`で作ったイメージには、次のものの著作権表示と許諾文が要る。

- NetBSDの`sys/net/if_wg.c`とrumpの`libwg`(BSDライセンス)。
- NetBSDが同梱するlibsodiumの`sys/external/isc/libsodium`(ISCライセンス)。
