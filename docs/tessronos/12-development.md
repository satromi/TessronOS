# 第12章　開発と試験

開発環境の用意から、ビルド、Raspberry Pi 5への書き込み、WindowsのQEMUでの起動までの手順は、[ビルドと実行の手順](../build-and-run.md)に順を追ってまとめてある。この章は、その仕組みを説明する。

## 12.1 開発環境

ビルドはWSLのUbuntuで行う。ツールチェーンは`tools/setup_wsl.sh`が`~/.local/xPacks`以下にインストールする次のものである。

| ツール | バージョン |
| --- | --- |
| aarch64-none-elf-gcc(xPack) | 15.2.1-1.1 |
| qemu-system-aarch64(xPack) | 9.2.4-1 |
| Python | 3。イメージ作成ツールと試験の補助に使う |

`source tools/env.sh`でPATHが設定される。`TS_TOOLCHAIN`で別のツールチェーンを指定することもできる。

## 12.2 ビルド

makefileは`build_make/`にあり、再帰呼出しをしない1つのmakefileで全体をビルドする。対象マシンは`TARGET`で選ぶ。

| コマンド | 生成物 |
| --- | --- |
| `make TARGET=_QEMU_VIRT_` | `tessronos.elf` |
| `make TARGET=_QEMU_VIRT_ qemu` | ビルドしてQEMUで起動する(Ctrl-A Xで終了) |
| `make TARGET=_RPI5_` | `tessronos.elf`、`tessronos.img`、`tessronos.map` |
| `make TARGET=_RPI5_ sdimg` | SDカードのイメージ`sd.img`(12.5) |
| `make TARGET=_RPI5_ sd SD=<FATパーティション>` | FATパーティションに`tessronos.img`と`config.txt`をコピーする |
| `make pkg PKG=<id>` | アプリケーションのパッケージ`obj/pkg/<id>.tpk`(11.8) |
| `make clean` | 生成物を削除する |

変数で構成を切り替える。

| 変数 | 機能 |
| --- | --- |
| `DESKTOP=1` | 試験ではなく、デスクトップが起動する構成でビルドする |
| `UITEST=1` | `DESKTOP=1`と併せて、UIの試験用の操作機能を組み込む(12.4) |
| `KTEST=1` | カーネル内の試験(12.3)を組み込む |
| `MOZC=1` | Mozcの変換エンジンと辞書をリンクする(先に`tools/mozc/build.sh`でビルドしておく) |
| `NETSTACK=netbsd`、`NETSTACK=lwip` | TCP/IPスタックを選ぶ。デフォルトは`netbsd`で、NetBSDのスタックのオブジェクトがなければ、リンクの前に`tools/netbsd/build.sh`を実行して作る(12.7)。`lwip`は以前のlwIPである(第9章) |
| `RUTRACE=1` | NetBSDのスタックで、ロックを5秒以上待っているタスクをコンソールに出力する(9.1.2) |
| `BROWSER=1`、`BROWSER_BLINK=1` | 別のリポジトリ`tessron-chromium`(既定はTessronOSと同じディレクトリ。`CHROMIUM=<場所>`で変える)からブラウザを取り込む。事前にChromiumのコンパイルが要る(14.3) |
| `KT_V8=<試験名>,…`、`KT_SITE=<URL>` | 試験グループ`v8`のうち指定した試験だけを実行する、指定したサイトだけを開いて画面を写す(14.10) |
| `V3D=1`、`V3D=2` | Raspberry Pi 5のGPUを識別する、さらにMMUと割込みを設定して使えるようにし、起動の後で実機の試験をまとめて行う(13.3.1、13.8.2) |
| `TS_VERSION=R1.000` | システムのバージョン。システムの実身の情報に`VERSION`として出る |
| `BUILD_ID=<名前>` | 生成物の出力先を`obj/<マシン><名前>/`と`tessronos<名前>.*`に分ける。同じソースツリーで複数の構成を並行してビルドするときに使う |

システムボリュームは、`etc/def`(システムの実身)、`etc/xtad`(サンプルと原紙)、ビルドで生成したプログラムの実身から`tools/mktsfs`で作成する。

## 12.3 カーネル内の試験

試験はカーネルにリンクしてQEMU上で実行する(`tests/ktest/`)。試験はカーネルのタスクとして動き、1件ごとに`KTEST <名前> PASS`か`KTEST <名前> FAIL`をコンソールに出力する。最後に`KTEST SUMMARY <成功数>/<総数>`で集計を出す。

```sh
make TARGET=_QEMU_VIRT_ KTEST=1 test
make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=part,ob BUILD_ID=pt test
```

`KTONLY`で試験グループを名前で選ぶ(カンマ区切りで複数指定できる)。グループは`tests/ktest/ktest_<グループ>.c`のファイル単位で、`task`、`sync`、`time`、`mem`、`smp`、`proc`、`fat`、`tsfs`、`ob`、`om`、`dp`、`fn`、`wm`、`part`、`mn`、`cab`、`tv`、`usb`、`devob`、`snd`、`net`、`so`、`v8`、`gpu`などがある。`devob`は、デバイス管理に登録しないデバイスの実身(乱数、画面、入力、キーボードとポインタ、USBデバイス、システムのレコード3、4、9)の属性、読み書き、保護、仮身、接続と切断の通知を試験し、実身を通す経路と直接の関数の時間を出力する(8.10)。`fault`はデスクトップを起動し、Shift+Pauseによる強制終了と、例外で終了したプロセスのダイアログを試験する(5.5)。`KTONLY`はコンパイル時の定義なので、`BUILD_ID`で出力先を分けないと反映されない。

試験環境は次の構成のQEMUである。

| 項目 | 値 |
| --- | --- |
| マシン | `virt,gic-version=2`、`cortex-a76`、4コア、2GB |
| ディスク | virtio-blkのGPTの試験用ディスク。試験が作り直すTSFSのパーティション、FAT32のブートパーティション、システムボリューム、FAT16とFAT12のパーティション |
| ネットワーク | virtio-net。`NET=user`でホストへ接続できる。このとき、ホストで試験用のFTPサーバ(`tools/ftpd.py`)とHTTPサーバ(`tools/httpd_test.py`、2180番がJSON、2181番がServer-Sent Events)が起動し、マイクロスクリプトの試験は仮身サンプルの通信用スクリプトをそのまま実行する |
| USB | xHCIに、キーボード、タブレット、マスストレージ2台、オーディオ |

`tools/qemu_run.sh`がQEMUを起動してコンソール出力を監視し、`tools/qemu_qmp.py`が試験からの要求に応じて、USBデバイスの抜き差しやキー入力をQEMUに送る。

電源断の試験は`make KTEST=1 KTONLY=tsfscut BUILD_ID=cut powercut`である。TSFSのボリュームを変更し続けるマシンを任意の時点で強制終了し、`fsck.tsfs`でエラーがないことを確認する。

## 12.4 デスクトップのUIの試験

UIの試験は`tools/ui_run.py`で行う。`make TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1`でビルドしたカーネルは、2つ目のシリアルポートから1行ずつコマンドを受け取り、キーボードとポインタのイベントとして直接入力する(`application/desktop/dtuitest.c`)。ポインタは指定した座標へ一度に移動し、1行の処理が終わるごとにコンソールへ`@ut ok`と応答するので、固定時間の待ちは不要である。`ui_run.py`は最初の1回だけマシンを起動し、デスクトップが起動した時点のメモリとディスクの状態を一時ディレクトリの`ui_<tag>`に保存する。以後は、カーネルとディスクが同じである限りその状態から再開するので、1回の試験は起動を含めて数秒で終わる。

```text
hold 110 100 1
wait 500
move 140 240
wait 150
move 230 321
up 1
win TADjsについての管理情報
shot info_window
```

| コマンド | 機能 |
| --- | --- |
| `move X Y`、`click X Y [B]`、`dclick X Y`、`hold X Y [B]`、`down [B]`、`up [B]`、`drag X1 Y1 X2 Y2 [B]` | ポインタとボタンの操作(ボタン0が左、1が右) |
| `key CODE [MODS]`、`text 文字列`、`wheel N` | キー入力(HIDのUsage IDと修飾キー、16進数)、英数字の入力、ホイール |
| `open UUID` | 仮身をダブルクリックしたときと同じように実身を開く |
| `run PATH [N …]` | プログラムのファイルをプロセスとして起動する。数値(8個まで)が起動時の引数になり、プロセスIDを応答する。例外で終了するプロセスを起こして、例外のダイアログ(11.1)を写すときなどに使う |
| `win 名前`、`wins` | 指定した名前のウインドウの外形、表示中のウインドウの一覧 |
| `wait MS`、`idle` | 待つ |
| `lag MS` | MSミリ秒のあいだ、ポインタを50ミリ秒ごとに左右へ動かし、画面のポインタがそこへ描かれるまでの時間を測る。最長の時間(ミリ秒)、動かした回数、200ミリ秒を超えた回数を応答する。プログラムの起動中などに、デスクトップが止まらないことを確かめるのに使う |
| `shot NAME`、`expect 文字列` | 画面をPNGで保存する、コンソールに指定の文字列が出るのを待つ(`ui_run.py`が処理する) |

メニューはサブメニューが開くまでに少し時間がかかるので、項目の上を通過するたびに`wait 150`程度を入れる。

WSL版のQEMUはユーザモードネットワークを持たないので、外部のネットワークを使う試験(`--net user`)は、WindowsのPythonからWindows版のQEMUで実行する。

以前の`tools/desk_act.sh`(QEMUのモニタからマウスを少しずつ動かす方式)は1回に数分かかるので、新しい試験には使わない。

## 12.5 Raspberry Pi 5で動かす

`make TARGET=_RPI5_ sdimg`でSDカードのイメージを作る。パーティション1(FAT32)に`config.txt`、デバイスツリー、`tessronos.img`、プログラムを、パーティション2にシステムボリュームを置く。デバイスツリーはRaspberry Piのファームウェアの配布物から取得する。イメージはRaspberry Pi Imagerの「カスタムイメージを使う」などでSDカードに書き込む。

コンソールは3ピンのデバッグ用UARTに出力される。`RP1_CONSOLE=1`を指定するとコンソールを40ピンヘッダのUARTに移し、`RP1_UART=1`を指定するとヘッダのUARTを2つ目のシリアルポートとして使う。

## 12.6 ツール

| ツール | 機能 |
| --- | --- |
| `tools/mktsfs` | TSFSのボリュームを作成する |
| `tools/fsck.tsfs` | TSFSのボリュームを検査し、修復する |
| `tools/tsfs-import`、`tools/tsfs-export` | TADjs Desktop形式のファイル群からTSFSのボリュームを作成する、ボリュームの実身をファイル群に書き出す |
| `tools/mkdisk.py`、`tools/mksd.py`、`tools/mkfat.py` | 試験用ディスク、SDカードのイメージ、FATのボリュームを作成する |
| `tools/btbackup.py` | BTRONのBACKUP書庫を読み書きする |
| `tools/ftpd.py` | ネットワークの試験に使うFTPサーバ |
| `tools/ui_run.py` | UIの試験を、起動済みの状態から数秒で実行する(12.4) |
| `tools/mktpk.py` | アプリケーションのパッケージ(.tpk)を作成する。`--list`で内容を表示する |
| `tools/netbsd/build.sh` | NetBSDのTCP/IPスタックのソースを取得し、rumpカーネルの部品として1つのオブジェクトにビルドする(12.7) |
| `tools/mesa/probe.sh` | Mesaを取得し、その一部をプロセス用のツールチェーンでコンパイルしてみる。`pack`はV3Dのパケットを詰めるヘッダを生成してリポジトリに書く(13.6.4) |

## 12.7 NetBSDのスタックのビルド

TCP/IPスタック(第9章)は、NetBSD 10.1のカーネルのソースからビルドする。ソースはリポジトリに含めず、`tools/netbsd/build.sh`が次の順に処理する。

| 段階 | 処理 |
| --- | --- |
| `fetch` | NetBSDの配布物`syssrc.tgz`を取得してSHA512を確かめ、スタックのビルドに使うディレクトリだけを`~/netbsd/netbsd-10.1`(`NBSRC`で変える)に展開する。展開したソースは約26MBである |
| `gen` | NetBSDのmakefileとconfig(1)が生成するもの(`vers.c`、ルートバスの`ioconf.c`、疑似デバイスの`ioconf.h`)と、`<machine/...>`のリンクを`~/tessronos-netbsd/gen`に作る |
| `compile` | rumpカーネルの基盤(librump)、ソケット層(librumpnet)、インタフェースと経路とIPv4、IPv6(librumpnet_net)と、TessronOSの部品(`peripheral_kernel/network/netbsd/rumpcomp/`)を、カーネルと同じツールチェーンで浮動小数点のレジスタを使わずにコンパイルする |
| `link` | rumpカーネルの規則どおり、NetBSDのシンボルに接頭辞`rumpns_`を付けてから1つのオブジェクトにまとめ、`rump_`で始まる入口以外をローカルにする。リンクセットの範囲もこのオブジェクトの中で決める。結果は`~/tessronos-netbsd/rumpnet.o`(`OUT`で変える)で、外から必要とするのはハイパーコール(`rumpuser_`、`rumpcomp_`)だけである |

引数を付けなければすべての段階を実行する。ソースを取得するときだけネットワークを使う。コンパイルは12並列で10秒ほどで終わる。makefileは、このスクリプトか`rumpcomp/`のファイルが新しくなると、リンクの前にスクリプトを実行し直す。

コンパイルするファイルの一覧はスクリプトに書いてあり、NetBSDのmakefileの`SRCS`を写したものである。NetBSDの版を上げるときは、スクリプトの`NB_VER`、`NB_URL`、`NB_SHA512`を変え、新しい版のmakefileと突き合わせて一覧を直す。展開するディレクトリの一覧(`EXTRACT_DIRS`)は、コンパイルで生成される依存関係のファイル(`*.o.d`)から求められる。
