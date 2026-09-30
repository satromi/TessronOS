# 上流 μT-Kernel 3.0 の取込み

TessronOS は TRON Forum の μT-Kernel 3.0 を出発点にしている。本書は「何を、どのバージョンから、どう取り込んだか」と、上流を更新する手順を記す。

## 取り込んだバージョン

| 項目 | 値 |
| --- | --- |
| リポジトリ | <https://github.com/tron-forum/mtkernel_3> |
| タグ | `v3.00.08`(2026-07-08 リリース) |
| コミット | `3a838a7` |
| 取込み日 | 2026-09-20 |
| ライセンス | T-License 2.2(`LICENSE`、正文はトロンフォーラムの TEF000-219-200401) |

## 取り込んだもの

| 上流のパス | TessronOS のパス | 備考 |
| --- | --- | --- |
| `kernel/tkernel/` | `kernel/tkernel/` | カーネル本体。64bit/SMP 化の改変対象 |
| `kernel/sysinit/`, `kernel/inittask/`, `kernel/usermain/`, `kernel/tstdlib/`, `kernel/knlinc/` | 同じ | |
| `kernel/sysdepend/*.h` | `kernel/sysdepend/` | 依存部の共通ヘッダ(`cpu_status.h`, `cpu_task.h`, `sys_msg.h`, `sys_timer.h`, `sysdepend.h`) |
| `include/tk/`, `include/sys/`, `include/tm/` | 同じ | `include/tk/typedef.h` は設計書 5.2 に沿って改訂する |
| `lib/libtk/*.c`, `lib/libtm/*.[ch]` | 同じ | |
| `config/*.h` | `config/` | ターゲット選択と機能スイッチを追加する |

## 取り込まなかったもの

- 他ボード・他 CPU の依存部(`kernel/sysdepend/{iote_*,cpu/rx*,cpu/stm32*,cpu/tx03_m367,cpu/rza2m}`、`include/{tk,sys}/sysdepend/{iote_*,ek_ra8m1,nucleo_*}`、`lib/{libtk,libtm}/sysdepend/`、`etc/linker/iote_*`、`build_make/iote_*.mk`)。TessronOS は AArch64(`armv8a`)の依存部を新規に書く。
- `kernel/sysdepend/cpu/core/armv7a/` は AArch64 版のファイル構成の参考にするが、リポジトリには入れない(上流のタグを参照する)。
- `device/`(ADC/I2C/シリアルのサンプルドライバ)、`app_sample/`。TessronOS のデバイスドライバは `device/` に新規に書く。
- Cortex-M 系の PendSV/NVIC 前提コード、`USE_STATIC_IVT` 等の MCU 固有オプション。

## 上流ファイルへの改変(2026-09-20 時点)

| ファイル | 改変 | 理由 |
| --- | --- | --- |
| `include/tk/typedef.h` | `BINT`/`UBINT` を追加、`SZ` を `BINT` に | 設計書 5.2 |
| `include/sys/machine.h` | ターゲット選択を `_RPI5_`/`_QEMU_VIRT_` に付け替え | 他ボードは持たない |
| `include/sys/inittask.h` | 初期タスクのスタックを 1KB → 8KB | AArch64 の例外フレームと C の呼出し深さ |
| `config/config.h` | ターゲット名の説明、`CNF_EXC_STACK_SIZE` 2048 → 8192 | 同上 |
| `kernel/knlinc/kernel.h` | `CallUserHandlerP1〜3` の `UW` を `UBINT` に | `exinf` ポインタの切詰め防止 |
| `kernel/tkernel/memory.c`, `memory.h`, `mempool.c` | ポインタを整数に入れる箇所の `UW` を `UBINT` に | LP64 でポインタが 64bit |
| `kernel/tstdlib/string.c` | `knl_memset` を 64bit 語で埋めるように書き直し | 元は `long` を 4 バイトと仮定し、LP64 では要求の 2 倍を書いていた |
| `lib/libtm/libtm_printf.c` | `%ld`/`%lx`/`%p` を 64bit で扱う(内部の値を `UD` に) | ポインタと `BINT`/`SZ` を出力するため |
| `include/tm/tmonitor.h` | `tm_com_set_base` を追加 | コンソールを別の PL011 に移すため(Pi 5 のヘッダ UART) |
| `kernel/tkernel/timer.c`, `timer.h` | ティックレス hrtimer に全面書換え(ns の単調時刻、期限順キュー、コンパレータは先頭期限のみ、`knl_timer_insert_u`/`_rel_ns`) | 設計書 7.4 |
| `kernel/tkernel/time_calls.c`, `time_calls.h` | 時刻を ns で計算(`knl_current_time` 廃止、`knl_real_time_ofs` は ns)、周期/アラームの時間を ns に、μs 版 `tk_*_u`、`ts_get_mono`、RTC 書戻し | 設計書 7.3, 7.6 |
| `kernel/tkernel/wait.c`, `wait.h` | `knl_make_wait_u`/`knl_make_wait_reltim_u`/`knl_gcb_make_wait_u` を追加 | μs 版の待ち |
| `kernel/tkernel/{semaphore,eventflag,mailbox,mutex,messagebuf,mempool,mempfix,task_sync}.c` | タイムアウト付きの呼出しは本体を `tk_*_u`(`TMO_U`)にし、ms 版は μs 版へ変換して呼ぶ薄いラッパ | μs 版 API を重複なく提供 |
| `kernel/tkernel/check.h` | `CHECK_TMOUT_U`/`CHECK_RELTIM_U` | 同上 |
| `kernel/knlinc/kernel.h` | `SYSCALL_U`(`USE_TIME_US_API` が 0 なら `LOCAL`) | 同上 |
| `include/tk/typedef.h` | `TMO_U`/`RELTIM_U` を追加 | 設計書 5.2 |
| `include/tk/syscall.h` | `T_CCYC_U`/`T_RCYC_U`/`T_RALM_U` と `tk_*_u` の宣言 | 設計書 7.6 |
| `include/tk/tkernel.h` | `<tk/smem.h>`, `<ts/time.h>` を含める | TessronOS の API |
| `config/config.h` | `CNF_TIMER_PERIOD` を 1 に(時間精度には影響しない)、`CNF_TIMER_SLACK_NS`/`USE_TIME_US_API`/`USE_RTC_WRITEBACK` を追加 | 設計書 7.4, 7.6 |
| `kernel/knlinc/kernel.h` | `knl_ctxtsk`/`knl_schedtsk`/`knl_dispatch_disabled` をグローバル変数から `knl_pcpu()->…` のマクロに、TCB に `runprc`/`assprc` を追加 | 設計書 8.2, 8.4 |
| `kernel/tkernel/task.c`, `task.h` | 上記グローバルの定義を削除、`knl_make_ready`/`knl_make_non_ready` は `knl_reschedule()` を呼ぶだけにし、`knl_reschedule` の実体を `smp.c` の全 CPU 割当てに差し替え | 設計書 8.3 |
| `kernel/tkernel/klock.c` | `knl_schedtsk` の直接代入を `knl_reschedule()` に | 同上 |
| `kernel/tkernel/task_manage.c` | `TA_ASSPRC`(`assprc` の検査と保存)、`tk_ref_tsk` の `prcid`、実行中判定を `tcb->runprc != 0` に、`tk_ter_tsk` は他 CPU で実行中のタスクに `E_OBJ` | 設計書 8.11 |
| `kernel/sysdepend/cpu/core/armv8a/cpu_status.h` | `BEGIN/END_CRITICAL_SECTION` をビッグカーネルロックの取得・解放に、`knl_taskindp` を per-CPU マクロに | 設計書 8.6.1 |
| `include/tk/syscall.h` | `TA_ASSPRC`、`T_CTSK.assprc`、`T_RTSK.prcid`、`tk_get_prc` | 設計書 8.11 |
| `include/tk/sysdepend/cpu/core/armv8a/syslib.h` | `T_SPLOCK` とスピンロック・アトミック・バリアの宣言 | 設計書 8.7 |
| `include/tk/syscall.h` | `T_DEVREQ` の `start` を `D`、`size`/`asize` を `SZ` に | 設計書 10.1(1TB 超のブロック番号と 4GB 超の転送) |
| `include/tk/tkernel.h` | `<ts/blk.h>`, `<ts/fs.h>`, `<ts/proc.h>` を含める | TessronOS の API |
| `config/config.h` | オブジェクト数の上限(タスク 48、セマフォ 24、イベントフラグ 24、mutex 16、メールボックス 12) | カーネル自身がファイルシステム・デバイス・プロセス管理で mutex とフラグを使うため |
| `kernel/sysdepend/cpu/core/armv8a/cpu_cntl.c`, `cpu_task.h` | EL0 タスクのユーザスタックのサイズとプロセス空間を生成時に指定できるように(`knl_ustack_size`, `knl_new_task_space`) | プロセスの主タスクはそのプロセスの空間にスタックを持つ(設計書 9.5) |

## NetBSD のネットワークスタック(取り込んでいない。`tools/netbsd/build.sh`が取得してビルドする)

TCP/IPスタック(docs/tessronos/09-network.md)は、NetBSDのカーネルのネットワーク部分を、rumpカーネルの部品としてTessronOSのカーネルの中で動かす。ソースはリポジトリに含めず、`tools/netbsd/build.sh`がNetBSDの配布物を取得し、スタックに必要なディレクトリだけをWSLのホームに展開して、TessronOSのツールチェーンでコンパイルする。ChromiumやMozcと同じく、リポジトリの外に置く。展開するのは約26MBで、スクリプトが検査値を確かめて取得し直せるので、リポジトリに入れる利点がない。

| 項目 | 値 |
| --- | --- |
| バージョン | NetBSD 10.1 |
| 取得元 | `https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/source/sets/syssrc.tgz` |
| 取得日 | 2026-09-30 |
| SHA512 | `766ac21f33cfe0e701dfedb894fa07f36d811da1a12e979181e8fca7af4e627852680ce42a7b29e97dd3e2e402ddf9ae7bfba60c8d7dc6b8a3354d8ce8c06926`(リリースの`source/sets/SHA512`と同じ) |
| 置き場 | `~/netbsd/netbsd-10.1`(`sys/`と`common/`のうち、スクリプトの`EXTRACT_DIRS`、`EXTRACT_TREES`にあるもの) |
| 生成物 | `~/tessronos-netbsd/rumpnet.o`、著作権表示の一覧`~/tessronos-netbsd/NOTICE-netbsd.txt` |
| ライセンス | BSD(2条項、3条項、一部4条項)ほか(`THIRD_PARTY_NOTICES.md`) |
| 変更 | ソースは変更しない |

コンパイルするのは、rumpカーネルのmakefile(`sys/rump/librump/rumpkern/Makefile.rumpkern`、`sys/rump/librump/rumpnet/Makefile.rumpnet`、`sys/rump/net/lib/libnet/Makefile`とそれが取り込む`libnetinet`、`libnetinet6`、`libnetmpls`の`Makefile.inc`)の`SRCS`と、`sys/lib/libkern`と`common/lib`のうちカーネルが使うものである。一覧はスクリプトに書いてある。オプションはrumpカーネルのデフォルト(`opt_rumpkernel.h`、`DIAGNOSTIC`あり)で、`KTRACE`とNetBSDの互換機能(`COMPAT_*`)は組み込まない。

NetBSDのmakefileとconfig(1)が生成するものは、スクリプトが代わりに作る。

| 生成するもの | 内容 |
| --- | --- |
| `vers.c` | NetBSDの版の名前と、`sys/conf/copyright`の著作権表示 |
| `ioconf.c` | rumpカーネルのルートバス(`mainbus0 at root`)の自動構成の表。`rump_autoconf.c`が取り込む |
| `ioconf.h` | 疑似デバイス`carp`と`mpls`の接続関数の宣言 |
| `<machine/...>`などのリンク | `sys/arch/evbarm/include`、`aarch64/include`、`arm/include`への参照 |

リンクでは、rumpカーネルのmakefileと同じ規則で、`rump`、`RUMP`、`__`で始まらないシンボルに`rumpns_`を付ける。自動生成されている`rump_syscalls.c`だけは、コンパイラが呼ぶ`memset`などを付け替えるだけにする。そのうえで`rump_`で始まる入口以外をローカルにし、リンクセット(`link_set_*`)の範囲も`.rodata.rump_link_sets`の中で決める。カーネルの側に必要なのは、ハイパーコールの実装だけである。

rumpカーネルの中に入るTessronOSの部品(`peripheral_kernel/network/netbsd/rumpcomp/`)は、NetBSDのヘッダでコンパイルするが、TessronOSのファイルであり、T-License 2.2に従う。

| ファイル | 内容 |
| --- | --- |
| `if_tsneta.c` | デバイス`neta`をEthernetのインタフェース`neta0`にする |
| `tsnet_conf.c` | アドレスと経路の設定、`FIONREAD`、呼び出したタスクに貸すスレッドを作る |
| `novfs_stub.c` | ファイルシステムを組み込まないため、rumpカーネルの基盤が参照するファイルシステムのシンボルを用意する |

rumpカーネルのビルドには、NetBSDの`build.sh`とnbmakeを使う`buildrump.sh`(`https://github.com/rumpkernel/buildrump.sh`)がある。しかし最後の更新は2017年で、2016年のNetBSDのソースを前提にしている。TessronOSは自立したaarch64のツールチェーンで浮動小数点のレジスタを使わずにコンパイルする必要があり、ファイルの一覧を直接持つほうが単純なので、使っていない。

## lwIP(`peripheral_kernel/network/lwip/`)

以前のネットワークスタックで、`make NETSTACK=lwip`のときに組み込む。lwIP をそのまま取り込む(設計書 12.6)。

| 項目 | 内容 |
| --- | --- |
| バージョン | STABLE-2_2_0_RELEASE(`0a0452b2c39bdd91e252aef045c115f88f6ca773`) |
| 取得元 | `https://git.savannah.nongnu.org/git/lwip.git` |
| ライセンス | 修正 BSD(3 条項)。`peripheral_kernel/network/lwip/COPYING` |
| 取り込んだもの | `src/core`(ipv4 含む)、`src/api`、`src/include`、`src/netif/ethernet.c` |
| 取り込まなかったもの | `contrib`、`doc`、`test`、PPP・6LoWPAN・SLIP・ZEP・bridge の実装(ヘッダは `init.c` が参照するため残す) |

**上流ファイルは改変しない。** 必要な調整はすべて `peripheral_kernel/network/port/` 側で行う。

| 移植層のファイル | 役割 |
| --- | --- |
| `port/include/lwipopts.h` | 構成。`NO_SYS=0`、IPv4 のみ、ソケットあり、DHCP は `CNF_NET_DHCP` |
| `port/include/arch/cc.h` | 型・バイト順・診断マクロ(`printf` ではなく T-Monitor へ) |
| `port/include/arch/sys_arch.h` | セマフォ・ミューテックス・メールボックスの型 |
| `port/include/{string.h,stdlib.h}` | C ライブラリが無いため必要な分だけ宣言。newlib 導入時に差し替える |
| `port/sys_arch.c` | 上の実体(`tk_cre_sem` ほか)。メールボックスは環状配列 + セマフォ 2 個 |
| `port/lwip_libc.c` | `errno`と乱数。`strlen`などの文字列の関数は`peripheral_kernel/libc/libc.c`にあり、`memcpy`系はカーネルが持っている |
| `port/netif_tf.c` | デバイス `neta` を lwIP の netif にする |
| `so_api.c` | T2EX の `so_*`。lwIP の -1/errno を ER に変換する。NetBSDのスタックでは`netbsd/so_api.c`がこれに代わる |

## FreeType(`outer_kernel/font/freetype/`)

フォントの描画は FreeType をそのまま取り込む(設計書 16.2.4、16.6)。独自のフォント管理層は作らない。

| 項目 | 内容 |
| --- | --- |
| バージョン | 2.13.3 |
| 取得元 | `https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz` |
| 取得日 | 2026-09-23 |
| SHA-256 | `5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747` |
| ライセンス | FTL(BSD 系)または GPLv2 の二者択一。`outer_kernel/font/freetype/LICENSE.TXT` と `docs/FTL.TXT` |
| 取り込んだもの | `include/` 全部と `src/` のうち `base`・`sfnt`・`truetype`・`smooth`・`raster`・`cff`・`psaux`・`pshinter`・`psnames` |
| 取り込まなかったもの | `docs`・`builds`・`tools`、および使わない形式のドライバ(`bdf`・`pcf`・`pfr`・`type1`・`type42`・`cid`・`winfonts`)、`autofit`、圧縮(`gzip`・`bzip2`・`lzw`)、`cache`・`sdf`・`svg`・`dlg`・検証(`gxvalid`・`otvalid`) |

**上流ファイルは改変しない。** 必要な調整はすべて `outer_kernel/font/port/` 側で行う。

| 移植層のファイル | 役割 |
| --- | --- |
| `port/ftoption.h` | 上流の設定をそのまま取り込んでから、カーネル内では意味のないものを無効にする(ファイル入出力・環境変数・圧縮・Mac のリソースフォーク) |
| `port/ftmodule.h` | 組み込むモジュール。TrueType と CFF の2つのアウトライン形式、グレースケールとモノクロの2つのラスタライザ |
| `port/ftsystem.c` | メモリはカーネルのアロケータから確保する。上流の `FT_Stream_Open`(ファイル名で開く)はエラーにする。ファイルから読むフォントは `fn.c` が独自のストリームを作って `FT_Open_Face` に渡す |
| `outer_kernel/font/fn.c` | TessronOS 側のインタフェース。フェイスを開き、サイズを設定し、UTF-8 の文字列を順に処理してグリフを配置する |

C ライブラリの不足分は `peripheral_kernel/libc/` にある(`qsort`・`atol`・`labs`・`strrchr`・メモリアロケータ・`setjmp`/`longjmp`)。lwIP 用に置いていた `string.h`/`stdlib.h` もここへ移した。newlib を導入したら差し替える。

## Mbed TLS(`lib/mbedtls/`)

プロセス用プログラムの TLS は Mbed TLS をそのまま取り込む。マイクロスクリプトの HTTPS が使う。

| 項目 | 内容 |
| --- | --- |
| バージョン | 3.6.7(LTS) |
| 取得元 | `https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2` |
| 取得日 | 2026-09-29 |
| SHA-256 | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` |
| ライセンス | Apache License 2.0 または GPL 2.0 以降の二者択一。Apache License 2.0 を選ぶ。`lib/mbedtls/LICENSE` |
| 取り込んだもの | `library/`(`Makefile`・`CMakeLists.txt` を除く)、`include/`、`LICENSE`、`README.md` |
| 取り込まなかったもの | `3rdparty`、`programs`、`tests`、`scripts`、`framework`、`docs`、`configs` |

**上流ファイルは改変しない。** 設定は `lib/libtls/ts_tls_config.h`(`MBEDTLS_CONFIG_FILE`)、乱数・時刻・ソケットとルート証明書の読み込みは `lib/libtls/tls.c` に置く。ビルドは `build_make/tls.mk`。

ルート証明書は curl が配布している Mozilla の CA 証明書リスト(`https://curl.se/ca/cacert.pem`、MPL 2.0)を、実身「ルート証明書」のレコード1(`etc/def/01a0d8c4-52d0-7a61-8b72-4c5d6e7f8091_1.bin`)として格納する。更新するときはこのファイルを差し替える。

## Chromium(取り込んでいない。別のリポジトリ`tessron-chromium`のスクリプトが読む)

ブラウザ(docs/tessronos/14-browser.md)は、Chromiumのソースをリポジトリに入れず、WSLのホームのチェックアウトを`tessron-chromium`の`tools/browser`がTessronOSのツールチェーンでコンパイルし直して作る。ブラウザの移植層とスクリプトは、TessronOSとは別のリポジトリ`tessron-chromium`にあり、`make BROWSER=1`のときに取り込む。

| 項目 | 内容 |
| --- | --- |
| バージョン | 153.0.8010.36(V8 15.3) |
| 置き場 | `~/cr6/src`(DEPSで固定した依存を含む)、ビルドの記述は`out/tessronos` |
| ライセンス | BSD 3条項ほか(`THIRD_PARTY_NOTICES.md`) |
| 変更 | ソースは書き換えない。`tessron-chromium/tools/browser/patches`のパッチを、コンパイルするときに写しに当てる |

| パッチ | 内容 |
| --- | --- |
| `abseil-cctz-libc.patch` | abseilの時刻帯を、TessronOSのCライブラリで扱う |
| `cookie_jar.patch` | `document.cookie`を、ブラウザのCookieの入れ物に向ける |
| `html_canvas_painter.patch` | canvasのレイヤーを合成に回さず、ページの描画の記録に描く |
| `html_media_element.patch`、`video_painter.patch` | メディアの操作部品がなくても動く。動画を常にソフトウェアで描く |
| `interface_object.patch` | コンパイルしていないWebのインタフェースを、グローバルのプロパティとして`undefined`と読ませる |
| `skia-png-frameinfo.patch` | libpngのコーデックが、1枚の画像の枠の情報を答える |
| `viewport_scrollbars.patch` | スクロールバーを隠す設定を、メインフレームのビューポートだけに効かせる(ページ全体はウインドウのスクロールバーで動かす) |

FFmpeg 4.4.6は、`~/ffbt/ffmpeg-4.4.6`のソースを`tessron-chromium/tools/browser/ffmpeg.sh`がビルドする(ソースは変更しない)。

## Mesa(生成したヘッダを1つ取り込む。ほかは`tools/mesa/`が取得して使う)

GPUのユーザ空間(docs/tessronos/13-gpu.md 13.6)はMesaを移植する予定である。現時点でリポジトリにあるMesa由来のものは、V3D 7.1の制御リストのパケットを詰めるヘッダ1つ(`device/gpu/v3d/mesa/v3d_packet_v71_pack.h`)だけである。これはMesaの`src/broadcom/cle/v3d_packet.xml`を`gen_pack_header.py`で変換したもので、`tools/mesa/probe.sh pack`が生成し、先頭にMITライセンスの著作権表示と許諾文(`tools/mesa/pack_notice.txt`)を付けて書き出す。手で編集せず、Mesaの版を上げるときは`MESA_VER`と`MESA_SHA256`を変えて作り直す。生成したヘッダが呼ぶ補助の関数(`device/gpu/v3d/mesa/cle/v3d_packet_helpers.h`)は、カーネルが浮動小数点のレジスタを使わずに済むように書いたTessronOSのファイルで、Mesaの同名のファイルの写しではない。それ以外は、`tools/mesa/probe.sh`が取得し、作業ディレクトリ(既定は`/tmp/tsmesa`)で生成とコンパイルを試すだけである。

| 項目 | 内容 |
| --- | --- |
| バージョン | 26.2.3 |
| 取得元 | `https://archive.mesa3d.org/mesa-26.2.3.tar.xz` |
| 取得日 | 2026-09-29(リリースは2026-09-16) |
| SHA-256 | `1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f` |
| ライセンス | MIT(ファイルごとの表示による。一部のファイルは他のライセンス。`docs/license.rst`を参照) |

TessronOSのコードがMesaから参照したものは次のとおりで、生成したヘッダを除き、いずれもファイルやコードの写しではない。

| TessronOSの場所 | 参照したMesaのファイル | 内容 |
| --- | --- | --- |
| `include/ts/gpu.h`の`T_GPUCL`、`T_GPUTFU`、`T_GPUCSD` | `include/drm-uapi/v3d_drm.h` | ジョブの要求の項目とその意味(ユーザ空間の変換を1対1にするため) |
| `device/gpu/v3d/v3d.c`の`v3d_identify` | `src/broadcom/common/v3d_device_info.c` | 識別ワードのフィールドの意味(バージョン、スライス数、QPU数、リビジョン) |
| `device/gpu/v3d/v3d.c`のジョブの実行 | `src/broadcom/simulator/v3dx_simulator.c` | キャッシュの無効化とクリーン、制御リストの開始、完了の判定の手順 |
| `tools/mesa/cle_probe.c` | `src/broadcom/cle/gen_pack_header.py`、`v3d_packet.xml` | 生成したヘッダをインクルードして使う(生成物は作業ディレクトリにだけ置く) |
| `device/gpu/v3d/v3d_test.c`のフレームの組み立て | `src/broadcom/vulkan/v3dvx_meta_common.c`、`v3dvx_cmd_buffer.c`、`src/broadcom/common/v3d_util.c`、`v3d_limits.h` | バッファを1色で埋める、またはコピーするフレームのパケットの並び、タイルとスーパータイルの大きさ、タイルの割当てメモリの大きさ |

Mesa本体を取り込む段階(13-gpu.mdのM1)では、この節を取り込んだファイルの一覧、改変、更新の手順を含むものに書き換える。

## フォント(`etc/font/`)

画面に文字を表示するにはフォントが必要である。フォントはリソースなので原則として取り込まないが、**再配布が認められているものを1つだけ**置く。これがないとフォントに関する試験がすべて SKIP になり、文字が表示されることを誰も確認できない。

| 項目 | 内容 |
| --- | --- |
| フォント | Noto Sans JP Regular(OpenType、PostScript アウトライン) |
| バージョン | Noto CJK Sans 2.004 の Subset OTF |
| 取得元 | `https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/05_NotoSansCJK-SubsetOTF.zip` |
| 取得日 | 2026-09-23 |
| ライセンス | SIL Open Font License 1.1。`etc/font/OFL.txt` |
| サイズ | 4,533,028 バイト |

`etc/font/` に置いた `.ttf`/`.otf` は試験用ディスクに格納され、`ktest_fn` が使う。**git で管理するのは上の1つだけ**で、ほかは置いても git には入らない(`.gitignore`)。

フルセットの Noto CJK(16.5MB)をファイル全体をメモリに読み込む方法で開くと、QEMU の試験が制限時間内に終わらない。そこで `fn_open_file()` を追加した。必要な部分だけを読むストリームを `FT_Open_Face` に渡すので、開くのに必要なメモリはフェイスの管理情報の分だけになる(→ 設計書 16.6)。

**CFF(PostScript アウトライン)のグリフを生成するタスクのスタックは、16KB なら足りるが 8KB では足りない。** インタプリタが作業領域をスタックに置くためである。不足するとスタックの下端を越えて、アロケータがその下に割り当てた領域を破壊する(エラーにならずに壊れる)。文字を描画するタスクには 32KB 以上のスタックを持たせること。

## 改変方針

- 上流ファイルのライセンスブロックは保つ。改変箇所は、その箇所が「何を・なぜ」するかの技術説明だけをコメントに書く。
- 上流と差分が出る変更(データ型、SMP 化、per-CPU 化)は設計書の該当節を参照できるようにする。
- 上流のバグ修正を見つけたら、TessronOS に取り込むと同時に上流へ報告する。

## 上流を更新する手順

1. `git -C <mtkernel_3> fetch --tags` で新しいタグを取り、`git worktree add <dir> <tag>` で展開する。
2. 前回のタグとの差分(`git diff <old> <new> -- kernel include lib config`)を読み、TessronOS が改変したファイルに重なるものを洗い出す。
3. 重ならないファイルは上書きコピー、重なるファイルは差分を手で当てる。
4. `make TARGET=_QEMU_VIRT_ test` が通ることを確認し、本書の「取り込んだバージョン」を更新してコミットする(コミットメッセージに上流のタグを書く)。
