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

x64(`TARGET=_X64_`、3.10)には、別に次のものを使う。カーネル用のlibgccは`-mno-red-zone`の組(マルチライブ)で作ってある。`source tools/x64/env.sh`でPATHが設定され、makeは`TS_X64_TOOLCHAIN`、`TS_X64_EFI_TOOLCHAIN`で別の場所も指せる。

x64のビルドは、Wi-Fi(09章9.6)のために次の3つを使う。無ければ、makeがそれぞれの手順を走らせて作る。手で作るときや作り直すときは次を走らせる。

| 生成物 | 作る手順 | 取得するもの(初めてのときだけ) |
| --- | --- | --- |
| `~/tessronos-obsd-x64/obsdwifi.o` | `OUT=~/tessronos-obsd-x64 tools/openbsd/build.sh all` | OpenBSD 8.0の`sys.tar.gz`(`~/openbsd/dl`。SHA-256を確かめる) |
| `~/tessronos-wpa-x64/wpasupp.o`(Raspberry Pi 5は`~/tessronos-wpa-arm/wpasupp.o`) | `tools/wpa/build.sh all`(Pi 5のものは`ARCH=aarch64`。`test`でホストの試験) | NetBSD 11.0の`src.tgz`(`~/netbsd/dl`。SHA-512を確かめる) |
| `~/tessronos-wifi-fw`のファームウェアと許諾 | `tools/x64/wifi/fw.sh fetch` | linux-firmware 20230117の`iwlwifi-8265-36.ucode`、`LICENCE.iwlwifi_firmware`(SHA-256を確かめる) |

取得できないときは、それぞれの手順が何を取れなかったかと、場所を指す変数(`NB_URL`、`NB_DL`、`NBDL`、`FW_URL`)を言って止まる。Wi-Fiを含めないときは`make TARGET=_X64_ WIFI=0`とする。このときは`~/tessronos-netbsd-x64/rumpnet.o`を使い、上の3つは要らない。

| ツール | 用途 |
| --- | --- |
| x86_64-elf-gcc(`~/.local/xPacks/x86_64-elf-gcc`) | カーネルとプログラム、newlib |
| x86_64-w64-mingw32-gcc | UEFIのローダ(PE32+) |
| NASM 3.01(`~/.local/xPacks/nasm`) | x64のブラウザのコーデックのSIMDの部分(14.3.1)。`tools/x64/build_toolchain.sh nasm`で作る。NASMは署名もハッシュの表も公開していないので、リリースの`git.id`がChromiumのDEPSが固定するNASMと同じで、tarballのソースがそのチェックアウトと一致することを確かめた上で、tarballのSHA-256をスクリプトに固定して照合する |
| WindowsのQEMU(`C:\Program Files\qemu`)とOVMF(`edk2-x86_64-code.fd`) | 試験機。WSLのaptのQEMUは使わない |

## 12.2 ビルド

makefileは`build_make/`にあり、再帰呼出しをしない1つのmakefileで全体をビルドする。対象マシンは`TARGET`で選ぶ。`TARGET`からアーキテクチャ`ARCH`(`aarch64`、`_X64_`では`x86_64`)とコア依存の置き場の名前`ARCH_CORE`(`armv8a`、`x86_64`)が決まり、ツールチェーン、命令セットの基準、インクルードパスとソースの一覧のコア依存の部分がそれで切り替わる(4.8)。出力先は`ARCH`によらず`TARGET`と`BUILD_ID`で決まる。

| コマンド | 生成物 |
| --- | --- |
| `make TARGET=_QEMU_VIRT_` | `tessronos.elf` |
| `make TARGET=_QEMU_VIRT_ qemu` | ビルドしてQEMUで起動する(Ctrl-A Xで終了) |
| `make TARGET=_RPI5_` | `tessronos.elf`、`tessronos.img`、`tessronos.map` |
| `make TARGET=_RPI5_ sdimg` | SDカードのイメージ`sd.img`(12.5) |
| `make TARGET=_RPI5_ sd SD=<FATパーティション>` | FATパーティションに`tessronos.img`と`config.txt`をコピーする |
| `make TARGET=_RPI4_` | Raspberry Pi 4、400、CM4の`tessronos.img`。ARMv8.0-Aで作る(3.6.2) |
| `make TARGET=_RPI4_ sdimg` | Raspberry Pi 4系のSDカードのイメージ`sd.img`(12.5、3.6.5) |
| `make TARGET=_RPI4_ qemu-rpi4`、`test-rpi4` | QEMUの`raspi4b`で起動する、カーネル内の試験を走らせる(3.6.6) |
| `make TARGET=_X64_` | x64の`tessronos.elf`と、ESPの中身`obj/_X64_<BUILD_ID>/esp/`(`EFI/BOOT/BOOTX64.EFI`、`tessronos.elf`、`cmdline.txt`)。設定は`build_make/x64.mk`(3.10) |
| `make TARGET=_X64_ qemu-x64` | ビルドして、ESPのディレクトリをFATのディスクとしてWindowsのQEMUの`q35`で起動する |
| `make TARGET=_X64_ sdimg`、`qemu-x64-disk` | x64のディスクのイメージ`sd.img`(ESPとシステムのボリューム、3.10.4)、それをNVMeとして挿して起動する |
| `make TARGET=_RPI5_ WIFI_NET80211=obsd`、`make TARGET=_RPI4_ WIFI=1 WIFI_NET80211=obsd` | Raspberry PiのWi-FiをOpenBSDの802.11の層で動かす(既定は`nbsd`。`~/tessronos-obsd-arm/obsdwifi.o`、Pi 4の系統は`-v80`。09章9.6.8) |
| `make TARGET=_RPI4_ WIFI=1` | Pi 4、Pi 400、CM4のWi-Fiを含める(既定は含めない)。`~/tessronos-netbsd-v80-wifi/rumpnet.o`と`~/tessronos-wpa-arm-v80/wpasupp.o`を結び、CYW43455のファームウェアの実身を載せる(CYW43456のPi 400などはWi-Fiなしで動く)。ファームウェアの置き場は`WIFI_FW_DIR`で替えられる(09章9.6.8) |
| `make TARGET=_RPI5_ WIFI=0`、`make TARGET=_QEMU_VIRT_ WIFI=1` | Raspberry Pi 5のWi-Fiを除く(Pi 5は既定で含める)。QEMU virtの`WIFI=1`は、ホストの無い機械で何も壊れないことと、基板の設定の取り出し(KTESTの群`wifinv`)を確かめるためのもの(09章9.6.8) |
| `make TARGET=_X64_ WIFI=0` | Wi-Fiを除く(既定の`WIFI=1`は含める。09章9.6)。含めるときはOpenBSDの802.11の層とiwm(`~/tessronos-obsd-x64/obsdwifi.o`、9.6.9)と、WPA2の部分`~/tessronos-wpa-x64/wpasupp.o`を結び、ファームウェアの実身とWi-Fiの設定の実身をシステムボリュームに載せる |
| `make pkg PKG=<id>` | アプリケーションのパッケージ`obj/pkg/<id>.tpk`(11.8) |
| `make TARGET=<機種> sysup` | システムのパッケージ`obj/<TARGET><BUILD_ID>/tessronos-<機種>-<TS_VERSION>.tpk`。機種は`_RPI5_`、`_RPI4_`、`_X64_`(`_QEMU_VIRT_`は試験用)。システムの実身はイメージと同じ指定(`DESKTOP=1`、`MOZC=1`、`BROWSER=1`、`WIFI`など)で作ったものを集めるので、配るイメージと同じ変数で作る(11.8.1) |
| `make clean` | 生成物を削除する |

変数で構成を切り替える。

| 変数 | 機能 |
| --- | --- |
| `DESKTOP=1` | 試験ではなく、デスクトップが起動する構成でビルドする |
| `UITEST=1` | `DESKTOP=1`と併せて、UIの試験用の操作機能を組み込む(12.4) |
| `TESTAGENT=1` | `DESKTOP=1`と併せて、実機の試験用の遠隔操作を組み込む(12.4.1)。試験用イメージだけ |
| `KTEST=1` | カーネル内の試験(12.3)を組み込む |
| `MOZC=1` | Mozcの変換エンジンと辞書をリンクする(先に`tools/mozc/build.sh`でビルドしておく。x64は`tools/x64/mozc/build.sh`) |
| `NETSTACK=netbsd`、`NETSTACK=lwip` | TCP/IPスタックを選ぶ。デフォルトは`netbsd`で、NetBSDのスタックのオブジェクトがなければ、リンクの前に`tools/netbsd/build.sh`を実行して作る(12.7)。`lwip`は以前のlwIPである(第9章) |
| `RUTRACE=1` | NetBSDのスタックで、ロックを5秒以上待っているタスクをコンソールに出力する(9.1.2) |
| `BROWSER=1`、`BROWSER_BLINK=1` | 別のリポジトリ`tessron-chromium`(既定はTessronOSと同じディレクトリ。`CHROMIUM=<場所>`で変える)からブラウザを取り込む。事前にChromiumのコンパイルが要る(14.3)。`TARGET=_X64_`ではx64のもの(`TS_ARCH=x86_64`、目的物は`~/tessronos-obj-x64`、生成物は`obj/x64/`)を作る(14.3.1)。Raspberry Pi 4系(`TARGET=_RPI4_`)では使えない |
| `KT_V8=<試験名>,…`、`KT_SITE=<URL>`、`KT_SITE_WAIT=<ミリ秒>`、`KT_SITE_CLICK=<x>,<y>,…` | 試験グループ`v8`のうち指定した試験だけを実行する、指定したサイトだけを開いてスクリーンショットを撮る、2枚目を撮るまでの時間を決める、その後に画面の座標をクリックする(14.10) |
| `WV_TEST=<ディレクトリ>` | `BROWSER=1`の試験のディスクの`/boot`に、そのディレクトリの`wv_Packages`と`wv.deb`(開発用のマシンでRaspberry Piのリポジトリから取ったインデックスとWidevineのパッケージ)を載せる。`test_blink_wv`が導入した状態を試すときだけ使い、リポジトリにもシステムのボリュームにも入れない(14.6.3.1) |
| `V3D=1`、`V3D=2`、`V3D=3` | Raspberry Pi 5のGPUを識別する、さらにMMUと割込みを設定して使えるようにし、起動の後で実機の試験とウインドウの合成の確認をまとめて行う、試験をせずに合成の確認だけをしてGPUで合成する(13.3.1、13.8.2、13.10) |
| `GL=1` | MesaのEGLとOpenGL ESをプロセス用の静的ライブラリに作り、小物「歯車」と試験のプログラムに結ぶ(13.6。x64は`tools/x64/mesa/build.sh`、13.11.9) |
| `SNDTEST=1`、`HEVCTEST=1` | Raspberry Pi 5で、起動の後に音の出力先、HEVCの復号器を実機で確かめる(6.5.4、6.7.1) |
| `KLOCKSTAT=1`、`STKWATCH=1` | カーネルのロックの取得と待ちを場所ごとに数える、走っていないタスクのスタックの書換えを見張る(4.3.2、4.7.6) |
| `KLOCK=<0〜3>`、`KLOCKCHECK=1` | カーネルの排他の方式を替える(0は大きなロック、3はスケジューラのロックと物ごとのロック。既定はArmが0、x64が3)、ロックの順番の検査を試験のビルドでなくても入れる(4.3.3)。`-D`の定義なので`BUILD_ID`を分ける |
| `X64_CMDLINE=<設定>`、`SD_LOG_MB=<MB>` | x64のイメージのESPに置く`cmdline.txt`の中身、記録の区画TSLOGの大きさ(3.10.7) |
| `DWC2=0` | Raspberry PiのUSB-CのポートのDWC2を調べず、使わない(3.6.7) |
| `PCIE_POLL=1` | Raspberry Pi 4系で、PCIeの先のxHCIを割込みではなく一定の間隔で見る(3.6.4) |
| `OBJTEST=0` | Raspberry Pi 5と4系で、起動の後のデバイスの実身とボードの確認(`objtest:`)を行わない |
| `TS_VERSION=R1.300` | システムのバージョン。システムの実身の情報に`VERSION`として出る |
| `PKGS="a.tpk …"` | イメージに組み込むアプリケーションのパッケージ。既定は`etc/pkg/`の.tpk、x64では`etc/pkg/x64/`の.tpk、Raspberry Pi 4系では`etc/pkg/rpi4/`の.tpk(`build_make/rpi4.mk`、11.8) |
| `BUILD_ID=<名前>` | 生成物の出力先を`obj/<マシン><名前>/`と`tessronos<名前>.*`に分ける。同じソースツリーで複数の構成を並行してビルドするときに使う |

システムボリュームは、`etc/def`(システムの実身)、`etc/xtad`(サンプルと原紙)、ビルドで生成したプログラムの実身、組み込むパッケージの実身から`tools/mktsfs`で作成する。

実時間制御TAD(第15章)の実行系`lib/librtad/`と再生部`application/desktop/dtrt.c`、プロセスのウインドウで再生する`dtrthost.c`は、デスクトップと同じくカーネルに入る。音を鳴らす実時間メディア(`application/rtmedia/`)はnewlibとlibmを使うプロセスで、MP3の復号にminimp3(CC0、`application/rtmedia/minimp3.h`)を使う。ビルドでその実行ファイル`obj/rtmedia.elf`を作り、プログラムの実身(`01a0d8c4-52da-7c34-9d45-be6f708192a3`)のレコード1にする。実時間制御TADの実身を開くアプリと映像の復号は、別のリポジトリ`ffmpeg_btron`のMPEG Playerである(11.12)。MPEG Playerを変えたときは`wsl.exe bash ffmpeg_btron/tessron/build-tessron.sh`でパッケージ`mpegplay.tpk`を作り直し、`etc/pkg/`に置く。x64のものは`TS_ARCH=x86_64`で作って`etc/pkg/x64/`に、Raspberry Pi 4系のものは`TS_CPU=cortex-a72`で作って(FFmpegも`TS_CPU=cortex-a72`の`build-ffmpeg-ts.sh`で一度作っておく)`etc/pkg/rpi4/`に置く。

## 12.3 カーネル内の試験

試験はカーネルにリンクしてQEMU上で実行する(`tests/ktest/`)。試験はカーネルのタスクとして動き、1件ごとに`KTEST <名前> PASS`か`KTEST <名前> FAIL`をコンソールに出力する。最後に`KTEST SUMMARY <成功数>/<総数>`で集計を出す。

```sh
make TARGET=_QEMU_VIRT_ KTEST=1 test
make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=part,ob BUILD_ID=pt test
```

`KTONLY`で試験グループを名前で選ぶ(カンマ区切りで複数指定できる)。グループは`tests/ktest/ktest_<グループ>.c`のファイル単位で、`task`、`sync`、`time`、`mem`、`smp`、`board`、`sd`、`proc`、`fat`、`tsfs`、`ob`、`om`、`dp`、`fn`、`wm`、`part`、`mn`、`cab`、`tv`、`vt`、`usb`、`devob`、`snd`、`net`、`so`、`v8`、`gpu`、`cal`、`kana`、`slide`などがある。`cal`は予定表の実身の読み書き(11.10)、`kana`はかな入力練習の配列の表、問題文、判定、成績と、キーを離した通知(11.14)、`slide`はスライドの並びと画面への収め方(11.11)、`vols`は多くのUSBのディスクと区画の同時の取り付け、形式の見分け、ボリュームをまたぐ仮身とほかのボリュームからの参照の記録、抜き差しの繰り返しと4コアでの同時の使用、26台を超えるディスクと40区画のディスク(7.6、8.3)、`vt`は文章の縦書きの組版と原稿用紙(11.21)、`prn`は印刷のPDFの構造、字の位置、書体の部分集合、画像と各アプリのページ(11.22)、`scr`は画面印刷の取り込んだ画素と画面の一致、図形実身と保存先のディスク、トレーと図形への貼り込み、PNGの書き出し、PDF(11.24)、`dfix`はディスクの検査と修復、ツリーとビットマップが同時に壊れたときの作り直し、壊れた実身ブロックからの救出(7.5)、`rt`は実時間制御TADの実行系と二進TADへの変換(15.10)、`rump`は4つのCPUから同時に掛けたNetBSDのスタックへの負荷(9.1.2)、`smp`の`test_wait_result`、`test_del_ended`、`test_exd_stack`は別のCPUが解除した待ちの結果と、終了したタスクのスタックを返す時機(4.3)を試験する。`smp`の`test_int_target`は装置の割込みを受けるCPUの切り替え(4.5.1)を、`klk`はカーネルのロックの順番の検査と、物やタスクをまたぐ呼出しを全部のCPUで重ねたときの結果(4.3.3)を試験する。`devob`は、デバイス管理に登録しないデバイスの実身(乱数、画面、入力、キーボードとポインタ、USBデバイス、システムのレコード3、4、9)の属性、読み書き、保護、仮身、接続と切断の通知を試験し、実身を通す経路と直接の関数の時間を出力する(8.10)。`snd`は、サウンドデバイスのチャネル、変換、混合、録音と、出力先の一覧と選択、再生中の切替え、選んだUSBオーディオの機能を抜いたときに自動の先へ移り挿し直すと戻ること(QEMUのモニタで2つ目の機能を挿す)、システムの実身のレコード8と機器の設定の`SOUND_OUT`による選択、ミキサーの混ぜ合わせ、プログラムごとと全体の左右の音量、消音、ソフトリミッタ、その設定の保存と読み戻しを試験する(6.5.1、6.5.6)。`fault`はデスクトップを起動し、Shift+Pauseによる強制終了と、例外で終了したプロセスのダイアログを試験する(5.5)。`fq`はデスクトップを起動し、デスクトップを止めた状態でのShift+Pause、解放のイベントが失われた後のShift+Pause、強制終了のウインドウからのウインドウのないプロセスの終了、システムコールの中で止まったプロセスの2段の終了、デスクトップがプロセスの依頼で出したパネル、デスクトップの立ち上げ直しを試験する(5.5.6)。`cred`は、デスクトップと同じ方法で起動したプログラムが利用者の資格で動き、時刻、ネットワーク、ボリューム、時計とネットワーク設定の実身を断られ、タイムゾーン、キー配列、ユーザ環境設定の項目、ユーザ情報と機器の設定とシステムの実身を許されること、一時的に与えた管理者の権限で時刻を変えられ、返したあとと期限が過ぎたあとは断られること、ログインした利用者の資格から管理者のグループが除かれること、管理者のパスワードの最初の設定と照合、信頼するプログラムの見分け、デスクトップのタスクが作る実身の所有者、USBの媒体の保護を試験する(8.7)。`board`は、メモリ上に組み立てたデバイスツリーとボードのリビジョンから、Raspberry Pi 5、CM5、CM5 Lite、Raspberry Pi 4、400、CM4を見分けること、起動したマシンのデバイスツリーからノードのプロパティと`ranges`、`dma-ranges`を読めることを試験する(3.5.4)。`sysup`は、試験の中で作ったシステムのパッケージ(この機種の新しいバージョン)で、システムの実身と起動のファイルが新しくなり、利用者の実身、機器の設定、利用者が箱に足した仮身、利用者の起動の設定が残り、前の起動のファイルが`.old`に残ること、同じパッケージでもう一度と、電源が切れた後の起動のときのやり直しで同じ結果になること、別の機種とアーキテクチャのもの、新しくないもの、システムのものでないものを断ることを試験する(11.8.1)。続けて`/boot`の下に作ったシステム登録の形のESP(`\EFI\TESSRON\`にローダ、カーネル、設定、`INSTALL.TXT`、`\EFI\BOOT\BOOTX64.EFI`はWindowsのもの)で、起動のファイルが`\EFI\TESSRON\`に置かれ、Windowsのローダと根が変わらないこと、入れ替えの名前の変更のそれぞれの後で止めても、ローダの探し方でカーネルが読め、次の起動のやり直しで仕上がること、200MBのESPに69MBのカーネルの機械を1/64にした大きさで空きの見積りが通ることを試験する。`sd`は`SD_EMMC=1`を付けるとeMMCに対して行う。`KTONLY`はコンパイル時の定義なので、`BUILD_ID`で出力先を分けないと反映されない。

試験環境は次の構成のQEMUである。

| 項目 | 値 |
| --- | --- |
| マシン | `virt,gic-version=2`、`cortex-a76`、4コア、2GB |
| ディスク | virtio-blkのGPTの試験用ディスク。試験が作り直すTSFSのパーティション、FAT32のブートパーティション、システムボリューム、FAT16とFAT12のパーティション |
| ネットワーク | virtio-net。`NET=user`でホストへ接続できる。このとき、ホストで試験用のFTPサーバ(`tools/ftpd.py`)とHTTPサーバ(`tools/httpd_test.py`、2180番がJSON、2181番がServer-Sent Events)が起動し、マイクロスクリプトの試験は仮身サンプルの通信用スクリプトをそのまま実行する |
| USB | xHCIに、キーボード、タブレット、マスストレージ2台、オーディオ |

`tools/qemu_run.sh`がQEMUを起動してコンソール出力を監視し、`tools/qemu_qmp.py`が試験からの要求に応じて、USBデバイスの抜き差しやキー入力をQEMUに送る。

試験は目で見るための画面の写しを`/boot`にPPMで残す(`kt_shot`、`prn`の群のページの画像)。1枚が3MBほどあり、全部の群を走らせるとブートパーティションの空き(システムの実身のレコードを置いた後で約47MB)を超える。そのため、写しは書いた後の空きが16MB(`KT_SHOT_KEEP`)を切るときは書かず、コンソールに`KTEST <パス> not written`と出す。また、試験用ディスクは実行をまたいで残るので、試験の始めに前の実行の写し(`/boot`の`.PPM`)を消す。全部の群の実行では後の群の写しが残らないことがあるので、見たい写しは`KTONLY`でその群だけを走らせて撮る。

x64(3.10)では、`make TARGET=_X64_ KTEST=1 BUILD_ID=<名前> sdimg`で試験を組み込んだディスクのイメージ`obj/_X64__ktest<名前>/sd.img`を作り、Git Bashから`tools/x64/ktest64.sh obj/_X64__ktest<名前>/sd.img`でWindowsのQEMUの`q35`に挿して走らせる。`ACCEL=whpx`でWindowsのハイパーバイザを使い(全部の試験が約6分)、`NET=user`でホストの試験用のサーバとつなぐ。`KT_BOARD`はx64でも1で、ディスクは`nvmea`である。QEMUには`-rtc base=localtime`を付け、RTCをWindowsの入ったPCと同じローカル時刻にする(9.5.1)。`EXTRA`に書いた引数はQEMUにそのまま足す(Intel HDAの`-device intel-hda -device hda-duplex`、電池の試験のSSDTの`-acpitable`など)。`NVME4K=<イメージ>`は、論理ブロックが4096バイトの2台目のNVMe(`nvmeb`、QEMUの`nvme`の`logical_block_size=4096`)としてそのイメージを挿す(`pcdev`の群の`test_nvme_4k`が書き込む)。`NVME2=<イメージ>`、`NVME3=<イメージ>`、`NVME4=<イメージ>`は、そのイメージを2台目から4台目のNVMe(`nvmeb`、`nvmec`、`nvmed`)として挿し、`VARS=<ファイル>`はOVMFの変数の書ける写し(`edk2-i386-vars.fd`を写したもの)を2つ目のフラッシュとして与えて、起動の項目を次の起動に残す(システム登録の`sysinst`の群、3.10.10)。`OFFWAIT=1`は、`KTEST SUMMARY`の後もマシンが電源を切るまで待ち、電源を切る前の行(NVMeのシャットダウンの行など、06章6.2.1)をコンソールの出力に残す。`pcdev`の群(`tests/ktest/x86_64/ktest_pcdev.c`)は、PS/2、ACPIの電源の装置、SATA、IntelのLAN、HID over I2Cの実身と、そのUUIDが起動し直しても変わらないことを試験する。

x64のTCGでは、タイマーの待ちが何もしていないホストでも約2ms遅れる(`ACCEL=whpx`では同じイメージで約0.35ms)。ホストのプロセッサを全部ふさぐと、遅れは6〜8msになる。時間を見る試験は、次の形で書く。

- 早く終わる待ちは、ホストによらず誤りとする。
- 遅れは複数回測り、最良の値でカーネルを判定し、最悪の値には止まったプロセッサで説明できる範囲の上限を置く。カーネルが遅らせる待ちは毎回遅れ、失った待ちは終わらない。
- 仕事の完了は決まった時間で打ち切らず、進み(数の増加)が止まったときに失敗とする。

ページ数の照合では、プロセスの空間から外したページテーブルは、ほかのCPUがディスパッチャを通るまで返らない(`knl_pt_reclaim`)ことを見込む。

`USBQ=1`を付けると、USBの装置をQEMU virtの試験機と同じ並びにする(1つ目のxHCIのキーボード、タブレット、USB 3の口のディスク、ハブとその先のオーディオ、ディスク、マウス、USB 2だけの2つ目のxHCIのディスク3台とキーボード)。ディスクのイメージは`make TARGET=_X64_ KTEST=1 BUILD_ID=<名前> obj/_X64__ktest<名前>/usb_disk.img`などで、イメージの隣に作る(`usb_disk.img`、`usb_disk2.img`、`usb_disk3.img`、`usb_sf32.img`、`usb_fd.img`、`vols/`、`btcd.iso`)。QEMUにQMPの口を開き、`tools/qemu_qmp.py`(WindowsのPython)が試験からの抜き差しの要求をQEMUに送る。このときWSLのパス(`/mnt/c/...`)はWindowsのパスに直し、WindowsのQEMUが持たない「ファイルから読む文字デバイス」は、`/dev/zero`の代わりに、0を送り続けるループバックのTCPの口にする。WindowsのQEMUは、この口から0が届いている`usb-serial`を`device_del`で抜くと、3回に1回ほど異常終了する。そのため`device_del`を送る前にこの口の接続を切り(QEMUは文字デバイスの線が終わると機器を外す)、0.5秒待ってから`device_del`を送る。x64の試験は、2つのxHCIがあるとき(`kt_qemu_devs`)だけQEMUの機器を前提にする試験(`usb`、`devob`、`snd`の`KT_RUN_QEMU_DEVS`と、`vols`、`grow`、`btcd`の群)を走らせ、ないときは理由を言って飛ばす。オーディオの音を取るときは`USBQ_AUDIO=wav,path=<ファイル>`とする。WindowsのQEMUでは、USBのオーディオの等時転送が1秒あたり150〜210パケットしか進まない(QEMUのxHCIが等時転送のTDを仮想時計のタイマで1つずつ送り出し、Windowsのタイマが粗い)。そのため、再生の時間を見る`snd`の`test_play`、`test_mix`、`test_outputs`はx64でも、WindowsのQEMUで走らせたQEMU virtでも通らない。この3つは、LinuxのQEMUで走るQEMU virtの試験で確かめる。

Raspberry Pi 4系のイメージは、QEMUの`raspi4b`で`make TARGET=_RPI4_ KTEST=1 BUILD_ID=<名前> test-rpi4`として試験する(3.6.6)。試験の中の「QEMUではなくボード」を表す条件は`KT_BOARD`で、Raspberry Pi 5と4系で1になる。

電源断の試験は`make KTEST=1 KTONLY=tsfscut BUILD_ID=cut powercut`である。TSFSのボリュームを変更し続けるマシンを任意の時点で強制終了し、`fsck.tsfs`でエラーがないことを確認する。

## 12.4 デスクトップのUIの試験

UIの試験は`tools/ui_run.py`で行う。`make TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1`でビルドしたカーネルは、2つ目のシリアルポートから1行ずつコマンドを受け取り、キーボードとポインタのイベントとして直接入力する(`application/desktop/dtuitest.c`)。ポインタは指定した座標へ一度に移動し、1行の処理が終わるごとにコンソールへ`@ut ok`と応答するので、固定時間の待ちは不要である。`ui_run.py`は最初の1回だけマシンを起動し、デスクトップが起動した時点のメモリとディスクの状態を一時ディレクトリの`ui_<tag>`に保存する。以後は、カーネルとディスクが同じである限りその状態から再開するので、1回の試験は起動を含めて数秒で終わる。

```text
click 110 100 1
wait 500
move 140 240
wait 150
move 230 321
click 230 321
win TADjsについての管理情報
shot info_window
```

| コマンド | 機能 |
| --- | --- |
| `move X Y`、`click X Y [B]`、`dclick X Y`、`hold X Y [B]`、`down [B]`、`up [B]`、`drag X1 Y1 X2 Y2 [B]` | ポインタとボタンの操作(ボタン0が左、1が右) |
| `key CODE [MODS]`、`text 文字列`、`wheel N` | キー入力(HIDのUsage IDと修飾キー、16進数)、英数字と記号の入力(記号は日本語キーボードの配列で打つ)、ホイール |
| `keydown CODE [MODS]`、`keyup CODE [MODS]` | キーを押したままにする、離す。同時に押すキーや、押したままのシフトを試験するのに使う |
| `open UUID` | 仮身をダブルクリックしたときと同じように実身を開く |
| `run PATH [N …]` | プログラムのファイルをプロセスとして起動する。数値(8個まで)が起動時の引数になり、プロセスIDを応答する。例外で終了するプロセスを起こして、例外のダイアログ(11.1)のスクリーンショットを撮るときなどに使う |
| `win 名前`、`wins` | 指定した名前のウインドウの外形、表示中のウインドウの一覧 |
| `front TAG 名前1\|名前2…`、`hidden TAG 名前` | ウインドウが前面から指定の名前の順に並んでいること、指定した名前のウインドウが表示されていないことを確かめる。どちらも2秒まで待ち、`ok TAG: in front`、`ok TAG: hidden`と応答する |
| `waitwin 名前 [S]`、`mark`、`sync [S]` | `waitwin`は指定した名前のウインドウが出るまでS秒(既定60秒)待つ。名前は行の残り全部で、空白を含んでよい(PCM Player)。最後の語が数字ならそれを秒数とする。`sync`は、デスクトップのタスクが`sync`を受けたことをループの頭で知り、そのあと入力を読みに行って何も無かったとき(それまでに入れたキーやボタンを読み終え、その処理を終えたとき)まで、S秒まで待つ。ループが一周したことだけを見ると、段落を組む間に届いたキーをまだ読まないうちに応答してしまう(Ctrl+Eで閉じて保存する前に、保存の結果を確かめてしまう)。どちらも`mark`からの経過時間をミリ秒で応答するので、起動や処理にかかる時間を測るのに使う |
| `linkdrag ウインドウ名 実身名 X Y [ボタン]` | 指定した名前のウインドウ(原紙箱やキャビネットのように仮身を並べた図形)の中から、指定した名前の実身の仮身を探し、その中央から画面のX, Yへ運ぶ。仮身の並びが変わっても同じ仮身を運べる。見つからなければ`err no such link` |
| `ready` | デスクトップがプロセスからの依頼を受ける口(`DT_REQ_NAME`)を作り終えたら(最大50秒待って)`ok`と応答する。試験の始めに、コマンドを受け付け、`open`で実身を開ける状態になったことを確かめる。口は設定とプログラムを読んだ後に作るので、遅い起動では`uitest: ready`から数秒かかる |
| `wait MS`、`idle` | 待つ |
| `lag MS` | MSミリ秒のあいだ、ポインタを50ミリ秒ごとに左右へ動かし、画面のポインタがそこへ描かれるまでの時間を測る。最長の時間(ミリ秒)、動かした回数、200ミリ秒を超えた回数を応答する。プログラムの起動中などに、デスクトップが止まらないことを確かめるのに使う |
| `stat` | 前回の`stat`からの、ウインドウマネージャの合成の回数、描いた部品の数、合成した画素数(千画素単位)、ポインタの移動の回数、ポインタを描き直した回数と、画面へ送った矩形の数、画素数(千画素単位)を応答する。描画の量を比べるのに使う |
| `pnstat` | 前回の`pnstat`からの、パネルを全体で描いた回数と、パネルを描いている最中に画面を組み立てた回数を応答する。ウインドウを開いたときや部品を書き換えたときに、パネルを何回描き、途中の絵を画面に出していないかを見るのに使う。`UITEST=1`のビルドだけにある |
| `xml 行`、`xmlopen 名前` | `xml`でxmlTADを1行ずつためて、`xmlopen`でその文章を指定した名前のウインドウで開く。文章の実身を作らずに、表示や編集を試験するのに使う |
| `caret`、`shapes` | 前面の文章(図形の中で編集中の文章を含む)のカーソルと選択の端を`ok c 段落,位置 a 段落,位置`で、前面の図形の図形の数と選択中の数を応答する。`caret`は先に送ったキーが処理されて値が変わらなくなるまで待つ |
| `mkbig doc\|fig\|calc N [M]`、`sum [UUID]` | `mkbig`は原紙から大きな文章(N段落)、図形、表計算(N行M列)の実身を作り、そのUUIDとバイト数を応答する。`sum`は実身のレコード0のバイト数とハッシュ(FNV-1a、64ビット)を応答する。UUIDを省くと`mkbig`で作った実身を見る。大きな実身の表示と保存の試験に使う |
| `kslide UUID` | 実身を、プログラムの実身の設定によらず、デスクトップの中のスライド(`dtslide.c`)で開く。プロセスのスライドと同じ実行の中で速さを比べるため(`tools/tests/ui_slide_cmp.steps`、11.11)。`UITEST=1`のビルドだけにある |
| `svcstat`、`waits` | `svcstat`は前回からの、プロセスのシステムコールの回数、プロセスの表のロック(`prc_mtxid`)を取った回数とそのうち待った回数を応答する(5.6.1)。`waits`は今のタスクの状態(実行中、実行可能、時間待ち、起床待ち、セマフォ、イベントフラグ、ミューテックス、メッセージバッファ、その他の待ち、強制待ち)の数と、最も多く待たれている物を3つ(`m12x30`はミューテックス12を30タスクが待つ)応答する。どちらも`UITEST=1`のビルドだけにある |
| `stkuse` | 今あるタスクと、削除したタスクの開始アドレスとスタックの大きさの組ごとに、スタックの最も深い使用量をコンソールに`stkuse:`の行で出す。残りが4分の1を切るものには`LOW`を付ける(4.7.7) |
| `look 番号 値` | 見た目の表(`include/ts/look.h`)の項目を書き換えて、画面を合成し直す |
| `wifitest` | Wi-Fiのデバイスがないマシンに、決まったアクセスポイントの一覧を返す試験用のWi-Fiのデバイスを置く(`device/net/wifi/wifi.c`)。Wi-Fiを入れずにビルドしたカーネルでは`err no wifi`と応答する |
| `accpos ID`、`progrm UUID`、`proginst UUID`、`proginstq UUID`、`progres`、`adminpanels`、`refs UUID`、`mkpkg` | 小物の順番と表示、プログラムの削除とインストール、管理者のパネルが出た数、実身の参照数、試験用の小さなパッケージの実身の作成(11.2.1、11.8)。`proginstq`は権限のないプロセスとして置き換えを頼み、すぐに応答する |
| `apps UUID id id …` | 実身の機能付箋を書き直す。先頭が既定の起動になる。機能付箋の多い実身を付箋指定のパネルを通さずに作る(`tools/tests/ui_exec_many.steps`) |
| `shot NAME`、`expect 文字列` | 画面をPNGで保存する、コンソールに指定の文字列が出るのを待つ(`ui_run.py`が処理する) |
| `absent 文字列` | その実行でコンソールに指定の文字列がまだ出ていないことを確かめる(`ui_run.py`が処理する)。スクリプトの最後に`absent !OACV!`を置き、権限の不足で断られた呼出しがないことを見る |
| `hmp コマンド` | QEMUのモニタにコマンドを送る(`ui_run.py`が処理する)。`$VOLS`は`--vols`のイメージのコピーの置き場になる。`drive_add`と`device_add usb-storage`でディスクを挿し、`device_del`で抜く |
| `expectin S 文字列`、`logmark`、`expectsince 文字列`、`want 応答`、`keep NAME`、`same NAME`、`under NAME MS`、`hostmark`、`hosttime NAME MS`、`pixel X Y RRGGBB`、`pixelnot X Y RRGGBB` | `ui_run.py`が処理する。`pixel`はその時点の画面の(X, Y)の色が指定の色であることを、`pixelnot`は指定の色でないことを確かめる(QEMUのモニタの`screendump`で読む)。コンソールの出力だけでなく、実際に描かれたかを確かめるのに使う。`expectin`は待つ時間を秒で指定する`expect`。`want`は直前のコマンドの応答が指定のとおりであることを、`keep`と`same`は直前の応答を名前で残して後の応答と同じであることを確かめる。`under`は直前の応答(`ok MS`)がMSミリ秒以下であることを確かめ、値を名前とともに出す。`hostmark`からの経過時間をホストの時計で測り、`hosttime`がMSミリ秒以下であることを確かめる。`logmark`はコンソールのログのその時点の位置を覚え、`expectsince 文字列`はそれより後にその文字列が出ることを待つ(途中の状態で同じ行が前に出ていても数えない) |

メニューはサブメニューが開くまでに少し時間がかかるので、項目の上を通過するたびに`wait 150`程度を入れる。

`UITEST=1`でビルドしたカーネルは、プロセスの呼出しが権限の不足(`E_OACV`)で断られるたびに、コンソールへ`!OACV! pid=N "プログラム名" 操作 obj=UUID type=種類.副種類 "実身名" want=0x.. may=0x..`の1行を出す(`knl_ob_deny_note`、8.7.4)。`want`は求めた操作ビット、`may`は許されている操作ビットである。実身によらない呼出し(SVCゲートの権限の表、ボリュームの取り付けなど)では`obj`以下を省き、SVCゲートでは`want`が機能コード、`may`が表の権限になる。`UITEST=1`でないビルドも`make OACVLOG=1`で同じ行を出す。デスクトップから起動したプログラムは利用者の資格で動くので(8.7)、この行で、プログラムが要る以上の権限で実身を開いていないか、システムのものに書き込もうとしていないかを探す。`tools/tests/ui_user_apps.steps`は、プログラム箱のプログラム(パッケージで入るものを含む)を利用者の資格で一つずつ起動してウインドウが出ることを見て、システム環境設定での壁紙の変更と、バックアップの保存と復帰を行い、最後に断られた呼出しがないことを確かめる。

`--keep-disk ファイル`を付けると、実行を終えたディスクのイメージをそのファイルにコピーして残す。試験で作った実身を`tools/tsfs-export`で取り出して確かめるのに使う(11.22)。

`--cdrom イメージ`を付けると、そのイメージを入れたUSBのCD-ROMデバイス(`media=cdrom`のドライブを持つ`usb-storage`、ブロック2048バイト)をつないで起動する。イメージが変わると、保存した状態を使わずに起動し直す。BTRONのCD-ROMの試験(11.9、`tools/tests/ui_btcd.steps`)に使う。

WSL版のQEMUはユーザモードネットワークを持たないので、外部のネットワークを使う試験(`--net user`)は、WindowsのPythonからWindows版のQEMUで実行する。

`--vols ディレクトリ`を付けると、マシンに何もつないでいないUSBのハブを加え、そのディレクトリのディスクイメージ(`tools/tests/mkvoldisk.py`)を実行ごとに一時ディレクトリへコピーする。スクリプトは`hmp`でそれを挿して抜く。ディスク操作の試験(11.26、`tools/tests/ui_vols.steps`、`tools/tests/ui_disk_grow.steps`)に使う。`--keep-disk`と一緒に付けると、実行が書いたディスクイメージをそのディレクトリに書き戻すので、次の実行を`--disk`と`--fresh`で起動し直して、再起動をまたぐ振る舞いを試験できる(`tools/tests/ui_vols2.steps`)。

`--wav ファイル`を付けると、USBのサウンドデバイスを加え、その出力をQEMUがWAVEファイルに書く。音を出す試験(実時間制御TADの`tools/tests/ui_rt.steps`、`ui_rt_player.steps`など)で、鳴った音の大きさと周波数を後から確かめられる。

x64のイメージ(`make TARGET=_X64_ DESKTOP=1 UITEST=1 sdimg`)は`tools/x64/ui_run64.py --disk obj/_X64_<名前>/sd.img`で試験する。引数は`ui_run.py`と同じで、マシンの引数だけを`q35`、OVMF、NVMeのディスク、`virtio-net-pci`、2つ目のシリアルポート、ローカル時刻のRTC(`-rtc base=localtime`)に替える。`UI_ACCEL=whpx`でWindowsのハイパーバイザを使う。NVMeのデバイスは状態を保存できないので、毎回起動から始める(WHPXで約5秒)。`UI_USBBOOT=1`のときは、ディスクのイメージをNVMeでなくUSBの記憶装置(`id=sysdisk`、ルートポート)として挿し、カードから起動したPCと同じく、システムのボリュームを`uda1`にする(ファームウェアはハブの先のディスクから起動しないので、ルートポートに挿す)。台本の`hmp`の行では、`$DISK`が走らせているディスクの写しを指す。`tools/x64/tests/ui_sysgone.steps`はこれで、デスクトップが上がった後にシステムのディスクを`device_del`で抜き、挿し直す(11.26)。

ディスクの名前や並びがQEMU virtと違うために座標や名前が変わる台本は、x64のものを`tools/x64/tests/`に置く。`ui_xfconv`、`ui_scrprint`(ファイル変換の一覧で`/boot`が`nvmea0`の2行目)、`ui_btcd`、`ui_sysenv_font`(USBのディスク`uda`が`nvmea`、`nvmea0`、`nvmea1`、`nvmea2`の後の5行目。`ui_btcd`はファイル変換をプログラムの実身を開いて起こす。x64のイメージの小物のメニューは並びが違うため)、`ui_vols`(〈接続〉のパネルの行が`nvmea2`を含めて5つで、QEMU virtより26ドット低い)、`ui_diskfix`(壊したボリュームを`tools/tests/dfix_disk.py`で`mkvoldisk.py`の`volc.img`の第1区画に作り、USBのディスクとして挿す。一覧はシステムのボリューム`nvmea1`が先)、`ui_diskfix_boot`(`dfix_disk.py --pc`でx64のディスクのイメージに修復の依頼を書く)がある。`ui_vols2`はQEMU virtの台本のまま通る。

以前の`tools/desk_act.sh`(QEMUのモニタからマウスを少しずつ動かす方式)は1回に数分かかるので、新しい試験には使わない。

### 12.4.1 実機の試験用の遠隔操作(TESTAGENT)

Surface Go 2のようにシリアルポートのない実機でも、12.4のコマンドで操作し、画面とログを受け取れるようにする口である。OS側は`application/desktop/dttagent.c`と`ta_tls.c`、PC側は`tools/mcp/`のMCPサーバ(stdio)で、`tools/ui_steps.py`の.stepsの解釈を`tools/ui_run.py`と共有する。

安全のための決まりは次のとおりである。

| 項目 | 決まり |
| --- | --- |
| ビルド | `make DESKTOP=1 TESTAGENT=1`の試験用イメージだけに入る。既定のビルドとリリースのビルドにはコードが入らない(`USE_TESTAGENT`と`SRCS_DESKTOP`の追加、`testagent.mk`) |
| 開く条件 | 起動の設定`cmdline.txt`に`testagent=<鍵>`があるときだけポートを開く。鍵は16進で64〜128桁(32〜64バイト)。ないか短ければ開かず、その旨をログに出す |
| 認証と暗号 | Mbed TLSによるTLS 1.2のPSK(`TLS_PSK_WITH_AES_128_GCM_SHA256`)だけを受ける。鍵が違う相手と平文のTCPはハンドシェイクで失敗し、1行も読まない。失敗の後は1秒受け付けない |
| 範囲 | 同じLANの中だけの試験用である。IPv4のプライベートアドレス(10/8、172.16/12、192.168/16、169.254/16、127/8)以外からの接続は切り、PC側もそれ以外へ接続しない。PSKなので前方秘匿性はなく、鍵が漏れると記録された通信を復号できる。インターネットに出さない |
| 表示 | 待ち受け、接続、切断を、デスクトップのメッセージ行(`試験用の遠隔操作が有効: ...`)とログに出す。キーやボタンの操作でメッセージ行が消えると、3秒ごとの確認で出し直す。プログラムが出したメッセージがある間は上書きしない |
| 鍵の扱い | PCが`secrets`で作り、`~/.tessronos/testagent/<名前>.token`に置く(リポジトリの外。Windowsでは本人だけのアクセス権)。OSは読んだ後にメモリ上の設定から消し、起動のログとローダの記録(x64)では`*`に置き換える。ただし`cmdline.txt`自体には平文で残るので、SDやESPを読める人には見える |
| PC側 | 起動しても接続しない。`connect`で名前を指定したときだけ接続する |

シリアルの口`serb`は`UITEST=1`を併せたときだけ開く。`TESTAGENT=1`だけのイメージに認証のない入口はない。一方で`USE_UITEST`の試験用の機能(`OB_DENY_LOG`など)は入るので、このイメージを普段使いにはしない。

ポートはTCPの7878で、接続は同時に1つ、600秒無通信で切る。1行の形とその答えは12.4と同じで、答えの行に`@ut `が付かない。シリアルとネットワークの両方があるときも、1行ずつ順に処理する。次はネットワークの口だけのコマンドである。

| コマンド | 答え |
| --- | --- |
| `hello` | `ok tessronos 1 幅 高さ` |
| `shot` | `ok 幅 高さ バイト数`の後にRGBの画素(ポインタを含む)。PC側でPNGにする |
| `log 位置 [最大]` | `ok N 次の位置 終わり`の後にNバイト。コンソールのログ(`tm_log_read`)で、`KERNEL.TXT`と同じ起動からの内容を読む。`TESTAGENT=1`では保持を1MBに増やす |
| `put バイト数` | 行の後にそのバイト数を送る。レコード1に持つ実身を作り、`ok UUID`。4MBずつ書く。失敗すると作りかけの実身を消して`err put エラーコード`を返す。言ったバイト数が60秒届かないで終わったときは、その後の接続の中身を行と区別できないので、答えてから接続を閉じる。終わりにコンソールへ、かかった時間と書込みの時間、その間のTCPの受信の数え(順番どおり、重複、順番の飛び)を出す |
| `rmobj UUID` | その実身を消す |
| `reboot`、`poweroff` | `ok`の後、ウインドウを保存してセッションを終える |

インストールは`put`の後に、アプリケーションは`proginst`(`proginstf`はバージョンによらず置き換える)、システムは`sysup`で行う。`sysup`はデスクトップのループが1段階ずつ進める。

使い方は次のとおりである。Python 3.13以降が必要である(`ssl`のPSK)。

```text
python tools/mcp/tessron_mcp.py --new-target surface --host 192.168.1.50
  (表示された testagent=... を試験用イメージの起動の設定に足す)
make TARGET=_X64_ DESKTOP=1 TESTAGENT=1 BUILD_ID=ta sdimg
claude mcp add tessronos-test -- python C:\home\TessronOS\tools\mcp\tessron_mcp.py
```

起動の設定は、x64ではESPの`cmdline.txt`、Raspberry Pi 5では起動の区画の`cmdline.txt`(`make RPI_CMDLINE=testagent=<鍵>`でイメージに入る。システムの更新では置き換えない)、Compute Module 5では`config.txt`の`cmdline=`が指す`cmdline-cm5.txt`である。Raspberry Piのファームウェアは起動時に`cmdline.txt`の内容をシリアルに出すので、COM3のログには鍵が残る。接続先のアドレスは`targets.json`に書く。DHCPで変わるときは、ルータで固定するか、デスクトップのメッセージ行で確かめる。

MCPのツールは`targets`、`connect`、`disconnect`、`send_steps`、`step`、`screenshot`(MCPの画像で返す)、`log`、`key`、`pointer`、`win_list`、`win`、`focus`、`reboot`、`power_off`、`wait_up`、`install_tpk`、`serial_log`である。`.steps`の`hmp`はQEMUのモニタの命令なので、実機では失敗する。

自動にできる範囲には次の限界がある。

- OSとネットワークが動いていないと何もできない。電源が切れた機械、固まった機械、起動の途中で止まった機械は、ソフトでは起こせない。電源を入れ直すのは人の手である。`power_off`の後も同じである
- 無線のネットワークは、保存済みのWi-Fiの設定で起動時に接続できることが前提である。接続できなければ口には届かない。USBのLANのほうが確実である
- SDやESPの書換え、`cmdline.txt`の編集、ファームウェアの設定は人の手である。システムの更新(`install_tpk`)は動くシステムの上でしかできないので、更新で起動しなくなったら人の手で戻す
- `reboot`はデスクトップが応答しているときだけ働く。デスクトップが固まると`reboot`は効かない
- 起動が止まったときのログは、ネットワークでは読めない。x64は従来どおりUSBメモリの`KERNEL.TXT`を見る。Raspberry Pi 5はDebug Probe(COM3)のシリアルコンソールを`serial_log`で読める。読むだけで、コマンドは送らない。TeraTermと同時には開けない

システムの更新には、システムのボリュームに`.tpk`を受け取る空きが要る。x64の既定の`SYS_VOL_SIZE=auto`のイメージでは、117MBの`.tpk`の`put`が`err put -33`で失敗した。`SYS_VOL_SIZE=600M`のイメージでは通ったので、空きの不足とみている。

2026-11-02に確かめた範囲は次の表のとおりで、すべてQEMUである。実機では2026-11-03にRaspberry Pi 5で、Wi-Fi越しの接続、画面、`put`、`sysup`、`reboot`、更新後の鍵の引継ぎを確かめた。439MBの`put`は、チップのRAMの大きさを読み違えていた版で1565秒、直した版で267〜630秒だった。Wi-Fiの受信が数十秒から数十分止まることがあり、その間は送る側のTCPが再送を重ねる(9.6)。Surface Go 2、X1 Yoga 第3世代、X1 Carbon、USBのLAN、`serial_log`は未確認である。

| 項目 | x64(q35) | ARM(virt) |
| --- | --- | --- |
| MCPの`initialize`、`tools/list`、`connect`、`screenshot`(画像)、`log`、`key`、`pointer`、`win_list`、`step`、`send_steps` | 確認 | 確認 |
| 平文のTCP、違う鍵の拒否。PC側の短いトークン、トークンのファイルなし | 確認 | 確認 |
| 鍵がログとコンソールに出ない | 確認 | 確認 |
| `install_tpk`のアプリケーション(`put`、`proginst`、`proginstf`、`rmobj`) | 確認 | 確認 |
| `install_tpk`のシステム(`sysup`)と、その後の再起動、鍵の引継ぎ | 確認(ゲストの中のTCG、9分強) | 未確認 |
| `reboot`、`wait_up`、`power_off` | 確認(TCG) | 確認 |
| `testagent=`なしでポートを開かない | 確認(ログと、応答なし) | 未確認 |
| メッセージ行の表示と出し直し | 確認 | 未確認 |

既定のビルド(x64の`DESKTOP=1`)のカーネルには、`ta_tls_`、`dt_tagent`、`mbedtls_ssl_`のシンボルと`testagent`の文字列がないことを確かめた。ARMとリリースの構成では確かめていない。x64をWHPXで動かすと、`reboot`の後にQEMUのプロセスが終わる。TCGでは再起動するので、OSの問題ではなくWHPXのリセットの問題とみているが、原因は確定していない。

## 12.5 Raspberry Pi 5で動かす

`make TARGET=_RPI5_ sdimg`でSDカードのイメージを作る。パーティション1(FAT32)に`config.txt`、デバイスツリー、`tessronos.img`、プログラムを、パーティション2にシステムボリュームを置く。デバイスツリーはRaspberry Piのファームウェアの配布物から取得する。イメージはRaspberry Pi Imagerの「カスタムイメージを使う」などでSDカードに書き込む。

コンソールは3ピンのデバッグ用UARTに出力される。`RP1_CONSOLE=1`を指定するとコンソールを40ピンヘッダのUARTに移し、`RP1_UART=1`を指定するとヘッダのUARTを2つ目のシリアルポートとして使う。

Raspberry Pi 4、400、CM4では`make TARGET=_RPI4_ sdimg`でイメージを作る。パーティション1にファームウェア(`start4.elf`、`fixup4.dat`)、3つのデバイスツリー、`overlays/disable-bt.dtbo`も置く。コンソールは40ピンヘッダの8番と10番のUART0に出力される(3.6)。

## 12.6 ツール

| ツール | 機能 |
| --- | --- |
| `tools/mktsfs` | TSFSのボリュームを作成する |
| `tools/fsck.tsfs` | TSFSのボリュームを検査し、修復する |
| `tools/tsfs-import`、`tools/tsfs-export` | TADjs Desktop形式のファイル群からTSFSのボリュームを作成する、ボリュームの実身をファイル群に書き出す |
| `tools/mkdisk.py`、`tools/mksd.py`、`tools/mkfat.py` | 試験用ディスク、SDカードのイメージ、FATのボリュームを作成する。試験用ディスクの大きさ(`TEST_DISK_MB`)は最小の値で、システムボリュームが大きく、FAT32のボリューム(65525クラスタ以上)と載せるファイルが入らないときは、ディスクを大きくして作る |
| `tools/btbackup.py` | BTRONのBACKUP書庫を読み書きする |
| `tools/ftpd.py` | ネットワークの試験に使うFTPサーバ |
| `tools/ui_run.py` | UIの試験を、起動済みの状態から数秒で実行する(12.4) |
| `tools/mktpk.py` | アプリケーションのパッケージ(.tpk)を作成する。`--system`でシステムのパッケージを作る(11.8.1)。`--list`で内容を表示する |
| `tools/pkgstage.py` | イメージに組み込むパッケージを展開し、そのプログラムへの仮身を足したプログラム箱を作る(11.8) |
| `tools/netbsd/build.sh` | NetBSDのTCP/IPスタックのソースを取得し、rumpカーネルの部品として1つのオブジェクトにビルドする(12.7)。`WIFI=1`は、Raspberry Pi 5のWi-Fi(SDIOのホストのsdhcとsdmmc、bwfm、net80211)を足したものを、別のソースの置き場`~/netbsd/netbsd-11.0-wifi`から`~/tessronos-netbsd-wifi/rumpnet.o`に作る。Pi 5のビルド(既定の`WIFI=1`、`build_make/wifi_arm.mk`)はこれを結び、無ければmakeが作る(09章9.6.8) |
| `tools/rpi5/wifi/fw.sh` | Raspberry Pi 5とPi 4の系統のWi-Fiのチップのファームウェア、CLM、基板の設定(NVRAM)と許諾を、linux-firmware 20260810から取得してSHA-256を確かめ、`~/tessronos-rpi5-wifi-fw`に置く。`mkobj.py`がファームウェア、CLM、基板の設定の文章の実身を作り、`nvram.py`は文章から設定を取り出す規則(カーネルと同じ)、`test_nvram.py`はその試験(09章9.6.8) |
| `tools/x64/ktest64.sh`、`tools/x64/ui_run64.py` | x64のカーネル内の試験とUIの試験を、WindowsのQEMUの`q35`で走らせる(12.3、12.4) |
| `tools/x64/acpica/build.sh` | ACPIの解釈系ACPICAのリリースを取得し、OSの層と合わせてカーネルに結ぶオブジェクト`~/tessronos-acpica-x64/acpica.o`を作る(3.10.6)。x64のカーネルのビルドは、これがなければ先に作る |
| `tools/wpa/build.sh` | wpa_supplicantのWPA2の鍵の交換の部分を、NetBSDの`src.tgz`から取り出してカーネル向けに建て、`~/tessronos-wpa-x64/wpasupp.o`にする(`ARCH=aarch64`はRaspberry Pi 5の`~/tessronos-wpa-arm/wpasupp.o`)。`test`はホストでhostapdの認証側と鍵の交換を試す(09章9.6.7) |
| `tools/x64/wifi/fw.sh`、`mkobj.py` | Wi-Fiのファームウェアをlinux-firmwareから取得してSHA-256を確かめ、システムボリュームに載せる実身(ファームウェア、Wi-Fiの設定、それらを結ぶ実身「システム」)を作る(09章9.6.5) |
| `tools/x64/netbsd/build.sh`、`tools/x64/mozc/build.sh`、`tools/x64/mesa/build.sh` | NetBSDのスタック、Mozc、Mesaのx86_64用のもの(`~/tessronos-netbsd-x64`、`~/tessronos-mozc-x64`、`~/tsmesa-x64`)を作る。手順はそれぞれ`tools/netbsd`、`tools/mozc`、`tools/mesa`のものと同じで、ツールチェーンと命令セットの基準だけが違う |
| `tools/chk_isa.sh` | オブジェクトやイメージに、ARMv8.0-AにないLSEのアトミック命令や暗号拡張の命令が残っていないかを調べる(3.6.2) |
| `tools/mesa/probe.sh` | Mesaを取得し、その一部をプロセス用のツールチェーンでコンパイルしてみる。`pack`はV3Dのパケットを詰めるヘッダを生成してリポジトリに書く(13.6.4) |

## 12.7 NetBSDのスタックのビルド

TCP/IPスタック(第9章)は、NetBSD 11.0のカーネルのソースからビルドする。ソースはリポジトリに含めず、`tools/netbsd/build.sh`が次の順に処理する。

| 段階 | 処理 |
| --- | --- |
| `fetch` | NetBSDの配布物`syssrc.tgz`を取得してSHA512を確かめ、スタックのビルドに使うディレクトリだけを`~/netbsd/netbsd-11.0`(`NBSRC`で変える)に展開する。展開したソースは約27MB、2074ファイルである。取得先は通常の配布サーバ、保存用のサーバ、ftp.netbsd.orgの順に試し、途中で切れた転送は続きから取り直す。どこからも全体が取れないときは、展開するファイルだけを固めた`netbsd-11.0-rumpsrc.tgz`(`tools/netbsd/`にあるもの、無ければTessronOSのリリースの添付)を、その検査値を確かめて展開する(UPSTREAM.md) |
| `gen` | NetBSDのmakefileとconfig(1)が生成するもの(`vers.c`、ルートバスの`ioconf.c`、疑似デバイスの`ioconf.h`)と、`<machine/...>`のリンクを`~/tessronos-netbsd/gen`に作る |
| `compile` | rumpカーネルの基盤(librump)、ソケット層(librumpnet)、インタフェースと経路とIPv4、IPv6(librumpnet_net)と、TessronOSの部品(`peripheral_kernel/network/netbsd/rumpcomp/`)を、カーネルと同じツールチェーンで浮動小数点のレジスタを使わずにコンパイルする。言語の規格はNetBSD 11.0のカーネルと同じC11(`-std=gnu11`)である |
| `link` | rumpカーネルの規則どおり、NetBSDのシンボルに接頭辞`rumpns_`を付けてから1つのオブジェクトにまとめ、`rump_`で始まるエントリポイント以外をローカルにする。まとめるときに、NetBSDの`ip_savecontrol`の呼出しをTessronOSの部品の`__wrap_rumpns_ip_savecontrol`に向ける(`--wrap`。受け取ったIPv4のデータグラムのTOSを付随データに足す、9.2)。リンクセットの範囲もこのオブジェクトの中で決める。結果は`~/tessronos-netbsd/rumpnet.o`(`OUT`で変える)で、外から必要とするのはハイパーコール(`rumpuser_`、`rumpcomp_`)だけである |
| `notices` | コンパイラが読んだNetBSDのソースとヘッダ(依存関係のファイル`*.o.d`に載るもの)の著作権表示とライセンス条件を、重複を除いて`~/tessronos-netbsd/NOTICE-netbsd.txt`にまとめる(`THIRD_PARTY_NOTICES.md`) |

引数を付けなければすべての段階を実行する。ソースを取得するときだけネットワークを使う。命令セットの基準は`MARCH`で変える(既定は`armv8.2-a`)。Raspberry Pi 4系向けのビルドは`MARCH=armv8-a+crc OUT=~/tessronos-netbsd-v80`で作る(3.6.2)。コンパイルは12並列で10秒ほどで終わる。makefileは、このスクリプトか`rumpcomp/`のファイルが新しくなると、リンクの前にスクリプトを実行し直す。

コンパイルするファイルの一覧はスクリプトに書いてあり、NetBSDのmakefileの`SRCS`をコピーしたものである。NetBSDのバージョンを上げるときは、スクリプトの`NB_VER`と`NB_SHA512`を変え、新しいバージョンのmakefileと突き合わせて一覧を直す。展開するディレクトリの一覧(`EXTRACT_DIRS`)は、コンパイルで生成される依存関係のファイル(`*.o.d`)から求められる。展開したファイルを固め直したものを新しいバージョンのリリースに添付し、その検査値を`NB_SUB_SHA512`に書く。リンクと著作権表示の一覧には、一覧にあるソースのオブジェクトだけを使う。前のバージョンで作った置き場で作り直しても、新しいバージョンで無くなったソースのオブジェクトは取り込まない。
