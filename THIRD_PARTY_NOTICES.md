# 第三者のソフトウェアとリソース

TessronOSに同梱している、またはビルドしたイメージに含まれる、TessronOS以外の著作物とそのライセンスの一覧である。TessronOSのソースコード自体のライセンスは`LICENSE`(T-License 2.2)による。

## リポジトリに含まれるもの

| 名称 | 場所 | バージョン | ライセンス | ライセンス文 |
| --- | --- | --- | --- | --- |
| μT-Kernel 3.0(TRON Forum) | `kernel/`、`include/tk`、`include/sys`、`include/tm`、`lib/libtk`、`lib/libtm`、`config/`の由来部分 | v3.00.08 | T-License 2.2 | `LICENSE` |
| lwIP(`make NETSTACK=lwip`のときだけイメージに入る) | `peripheral_kernel/network/lwip/` | 2.2.0 | 修正BSD(3条項) | `peripheral_kernel/network/lwip/COPYING` |
| FreeType | `outer_kernel/font/freetype/` | 2.13.3 | FreeType License(FTL)を選択(GPLv2とのデュアルライセンス) | `outer_kernel/font/freetype/LICENSE.TXT`、`outer_kernel/font/freetype/docs/FTL.TXT` |
| Noto Sans JP | `etc/font/NotoSansJP-Regular.otf` | Noto CJK Sans 2.004 | SIL Open Font License 1.1 | `etc/font/OFL.txt` |
| Mbed TLS | `lib/mbedtls/` | 3.6.7 | Apache License 2.0を選択(GPL 2.0以降とのデュアルライセンス) | `lib/mbedtls/LICENSE` |
| Mesa(V3D 7.1の制御リストのパケットを詰めるコード) | `device/gpu/v3d/mesa/v3d_packet_v71_pack.h`(Mesaの`v3d_packet.xml`から`gen_pack_header.py`で生成したもの) | 26.2.3 | MIT | ファイルの先頭 |
| MozillaのCA証明書リスト | `etc/def/01a0d8c4-52d0-7a61-8b72-4c5d6e7f8091_1.bin`(実身「ルート証明書」のレコード1) | curlのcacert.pem(2026-09-25時点のMozillaのデータ) | Mozilla Public License 2.0 | <https://mozilla.org/MPL/2.0/>(入手元 <https://curl.se/docs/caextract.html>) |

いずれも改変せずに同梱している。Mesaから生成したヘッダは、生成したものの先頭に著作権表示と許諾文を付けただけで、`tools/mesa/probe.sh pack`で作り直せる。lwIP、FreeType、Mbed TLSは上流のファイルを変更せず、移植層を`peripheral_kernel/network/port/`、`outer_kernel/font/port/`、`lib/libtls/`に置く(`UPSTREAM.md`)。Mbed TLSをリンクしたプログラム(マイクロスクリプト)を配布するときは、Apache License 2.0の要求に従いライセンス文を添付する(Mbed TLSにはNOTICEファイルはない)。CA証明書リストはMPL 2.0の「対象ソフトウェア」を改変せずにソースコードの形で同梱するもので、その条件は上記URLの本文による。Noto Sans JPは、フォント単体で販売しない限り、OFLに従い他のソフトウェアと一緒に配布できる。

TADjs Desktopから持ってきたサンプルの実身とアイコン(`etc/xtad`、`etc/def`の`.ico`)、壁紙`TESSRON.PIC`は、TessronOSの作者自身の著作物であり、TessronOSの一部としてT-License 2.2で配布する。

## ビルドしたイメージに含まれるもの

リポジトリには含まれないが、ビルド時にリンクされるもの、またはイメージに格納されるものである。イメージを配布するときは、それぞれの条件に従う。

| 名称 | 含まれ方 | ライセンス |
| --- | --- | --- |
| NetBSD 10.1のカーネルのネットワーク部分(rumpカーネルの基盤、ソケット、インタフェース、経路、IPv4、IPv6、TCP、UDPと、それが使う`libkern`、`common/lib`) | デフォルト(`NETSTACK=netbsd`)のすべてのイメージのカーネルに、`tools/netbsd/build.sh`がビルドしたオブジェクトとしてリンクする。ソースはリポジトリに含めない | 大半がBSD 2条項と3条項。一部はBSD 4条項(宣伝の条項がある)、RSA Data SecurityのMD4とMD5のライセンス、パブリックドメイン。`sys/net80211`のヘッダはBSDとGPL 2のデュアルで、BSDを選択する。GPLだけのファイルは含まない |
| newlib(C標準ライブラリと数学関数) | xPackツールチェーン付属のものをプロセス用プログラムにリンクする | BSD系ライセンスの集合(newlibの`COPYING.NEWLIB`)。イメージを配布するときは著作権表示を添付する |
| libstdc++、libgcc | 同上。C++のプログラムにリンクする | GPLv3とGCC Runtime Library Exception。この例外により、リンクしたプログラムの配布条件は制約されない |
| Mozc | `make MOZC=1`のときだけ、`tools/mozc/build.sh`でビルドした変換エンジンをリンクする | 修正BSD(3条項) |
| Mozcの辞書データ | `make MOZC=1`のときだけ、辞書の実身のレコードに格納される | Mozcのデータのライセンス(IPAdic由来の部分を含む)。Mozcの`LICENSE`と`data/`以下の各ファイルを参照 |
| Chromium 153.0.8010.36(Blink、V8、Skia、`base`、`net`、`cc`、`mojo`など)と、その`third_party`のうちコンパイルしたもの(ICU、HarfBuzz、FreeType、libpng、libjpeg-turbo、libwebp、Wuffs、zlib、Brotli、BoringSSL、abseil、Perfetto、Highway、simdutf、dav1d、libyuvなど) | `make BROWSER=1`のときだけ、`tessron-chromium`のスクリプトがコンパイルし直してブラウザの実行ファイルに静的にリンクし、プログラム「ブラウザ」の実身のレコード1に格納される | Chromium、V8、SkiaはBSD 3条項。Blinkの一部のファイル(WebKitとKHTMLに由来するもの)はLGPL 2.1(またはLGPL 2)とBSDのデュアル。`third_party`はそれぞれのライセンス(ICUはUnicode License、BoringSSLはOpenSSL・ISC・Apache 2.0の組合せ、abseilとPerfettoはApache 2.0、dav1dはBSD 2条項など) |
| Chromiumが使うRustのクレート(Rustの標準ライブラリ、jxl-rs、CrabbyAvif、Fontations、serde_json_lenientなど) | `make BROWSER=1`のときだけ、aarch64向けにビルドしてブラウザの実行ファイルにリンクする | 多くはMITとApache 2.0のデュアル。クレートごとの`LICENSE`による |
| FFmpeg 4.4.6(libavcodec、libavformat、libavutil、libswresample) | `make BROWSER=1`のときだけ、`tessron-chromium/tools/browser/ffmpeg.sh`がGPLの部分を有効にせずにビルドし、ブラウザの実行ファイルに静的にリンクする | LGPL 2.1以降 |
| Raspberry Pi 5のデバイスツリー(`bcm2712-rpi-5-b.dtb`) | `make sdimg`がRaspberry Piのファームウェア配布物から取得し、SDカードイメージに格納する。リポジトリには含めない | 元のソースコード(Raspberry Pi版Linuxの`bcm2712-rpi-5-b.dts`ほか)はGPL-2.0とMITのデュアルライセンス。MITを選択し、イメージを配布するときはその表示を添付する |

## 取得して使うもの(リポジトリにもイメージにも含まれない)

| 名称 | 使い方 | バージョン | ライセンス |
| --- | --- | --- | --- |
| NetBSD | `tools/netbsd/build.sh`が`syssrc.tgz`を取得し、ネットワークスタックに必要なディレクトリだけを`~/netbsd/netbsd-10.1`に展開してビルドする。ソースは変更しない(`UPSTREAM.md`) | 10.1 | BSDほか(上記) |
| Chromium | WSLのホームのチェックアウト(`~/cr6/src`、DEPSで固定した依存を含む)を、`tessron-chromium`のスクリプトが読んでコンパイルし直す。ソースは書き換えず、変更は`tessron-chromium/tools/browser/patches`のパッチを写しに当てる(`UPSTREAM.md`) | 153.0.8010.36 | BSD 3条項ほか(上記) |
| FFmpeg | `~/ffbt/ffmpeg-4.4.6`のソースを`tessron-chromium/tools/browser/ffmpeg.sh`がビルドする | 4.4.6 | LGPL 2.1以降 |
| Mesa | `tools/mesa/probe.sh`が取得し、一部をプロセス用のツールチェーンでコンパイルできるかを試す。GPUのドライバの設計ではジョブの要求の項目と識別ワードの意味を、実機の試験ではフレームの組み立て方を参照した(`UPSTREAM.md`) | 26.2.3 | MIT(一部のファイルは他のライセンス) |

Mesaをリンクしたプログラムを配布するときは、MITライセンスの著作権表示と許諾文を添付する。`V3D=2`でビルドしたカーネルには、上記の生成したヘッダから作られたコードが含まれる。

## 表示

FreeTypeのライセンス(FTL)は、FreeTypeを使ったソフトウェアのドキュメントに次の一文を記載することを求めている。

Portions of this software are copyright © 2024 The FreeType Project (<https://freetype.org>). All rights reserved.

lwIPのライセンスは、バイナリを配布するときに著作権表示、ライセンス条件、免責事項をドキュメントに記載することを求めている。`NETSTACK=lwip`でビルドしたイメージを配布するときは`peripheral_kernel/network/lwip/COPYING`の全文を添付する。

NetBSDのファイルのBSDライセンスも、バイナリを配布するときに著作権表示、ライセンス条件、免責事項をドキュメントに記載することを求めている。`tools/netbsd/build.sh`は、スタックにコンパイルしたNetBSDのすべてのソースとヘッダの著作権表示とライセンス条件を、重複を除いて`~/tessronos-netbsd/NOTICE-netbsd.txt`にまとめる(NetBSD全体の著作権表示`sys/conf/copyright`を含む)。イメージを配布するときはこのファイルを添付する。MD4とMD5のコード(`common/lib/libc/md`)は、RSA Data Securityのライセンスに従い、「RSA Data Security, Inc. MD4 Message-Digest Algorithm」「RSA Data Security, Inc. MD5 Message-Digest Algorithm」に由来することを表示する。

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
- This product includes software developed by Jason L. Wright.
- This product includes software developed by Rolf Grossmann.
- This product includes software developed by Paul Mackerras <paulus@samba.org>.
- This product includes software developed by QUALCOMM Incorporated.

イメージ(`tessronos.img`、`sd.img`)を配布するときに添付するものは、T-License 2.2(`LICENSE`)、NetBSDの著作権表示とライセンス条件の一覧(`NOTICE-netbsd.txt`。`NETSTACK=lwip`ならlwIPの`COPYING`)、FreeTypeの上記の一文、Mbed TLSの`LICENSE`、Noto Sans JPの`OFL.txt`、CA証明書リストのMPL 2.0の入手先、newlibの`COPYING.NEWLIB`の表示、デバイスツリーのMITの表示、`MOZC=1`でビルドしたときはMozcとその辞書データのライセンス、`V3D=2`でビルドしたときは`device/gpu/v3d/mesa/v3d_packet_v71_pack.h`の先頭にあるMesaの著作権表示と許諾文である。

`BROWSER=1`でビルドしたイメージを配布するときは、さらに次のものが要る。

- Chromiumと、コンパイルした`third_party`とRustのクレートの著作権表示とライセンス文。Chromiumの`tools/licenses/licenses.py`(`credits`)が、ビルドの記述(`out/tessronos`)から一覧を作る。
- LGPLの部分(FFmpegと、Blinkのファイルのうち、LGPLのもの)について、LGPL 2.1の第6節の条件。ブラウザは静的にリンクしているので、利用者が改変したライブラリとリンクし直せるように、ブラウザの実行ファイルのオブジェクトファイル(またはソースコード)と、リンクの手順を提供する。FFmpegとChromiumの対応するソースコードの入手先も示す。

## μT-Kernel 3.0の派生物を配布するときの手続き

T-License 2.2は、改変したソースコードの派生物を第三者へ再配布するとき、トロンフォーラムのトレーサビリティサービスに登録してディストリビューションucodeを取得すること、本ライセンス契約を添付すること(第3条第2項第6号)、ソースコードを利用した旨を表示すること(第3条第6項)を求めている。TessronOSは登録済みで、ディストリビューションucodeは0007006Aである。ucodeとソースコードを利用した旨はREADME.mdに表示し、本ライセンス契約は`LICENSE`として添付する。
