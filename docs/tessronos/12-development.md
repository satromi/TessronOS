# 第12章　開発とテスト

開発環境の用意から、ビルド、Raspberry Pi 5への書き込み、WindowsのQEMUでの起動までの手順は、[ビルドと実行の手順](../build-and-run.md)に順を追ってまとめてある。この章はその仕組みを説明する。

## 12.1 開発環境

ビルドはWSLのUbuntuで行う。ツールチェーンは`tools/setup_wsl.sh`が`~/.local/xPacks`以下にインストールする次のものである。

| ツール | バージョン |
| --- | --- |
| aarch64-none-elf-gcc(xPack) | 15.2.1-1.1 |
| qemu-system-aarch64(xPack) | 9.2.4-1 |
| Python | 3。イメージ作成ツールとテストの補助に使う |

`source tools/env.sh`でPATHが設定される。`TS_TOOLCHAIN`で別のツールチェーンを指定することもできる。

x64(`TARGET=_X64_`、3.10)には、別に次のものを使う。カーネル用のlibgccは`-mno-red-zone`のマルチライブラリで作ってある。`source tools/x64/env.sh`でPATHが設定され、makeは`TS_X64_TOOLCHAIN`、`TS_X64_EFI_TOOLCHAIN`で別の場所も指定できる。

x64のビルドはWi-Fi(09章9.6)のために次の3つを使う。なければ、makeがそれぞれのスクリプトを実行して作る。手で作るときや作り直すときは次を実行する。

| 生成物 | 作るコマンド | ダウンロードするもの(初回だけ) |
| --- | --- | --- |
| `~/tessronos-obsd-x64/obsdwifi.o` | `OUT=~/tessronos-obsd-x64 tools/openbsd/build.sh all` | OpenBSD 8.0の`sys.tar.gz`(`~/openbsd/dl`。SHA-256を検証する) |
| `~/tessronos-wpa-x64/wpasupp.o`(Raspberry Pi 5は`~/tessronos-wpa-arm/wpasupp.o`) | `tools/wpa/build.sh all`(Pi 5のものは`ARCH=aarch64`。`test`でホスト上のテスト) | NetBSD 11.0の`src.tgz`(`~/netbsd/dl`。SHA-512を検証する) |
| `~/tessronos-wifi-fw`のファームウェアとライセンス | `tools/x64/wifi/fw.sh fetch` | linux-firmwareの決まったタグ(20230117、20240312、20251021、20260309、20261019)のIntel、MediaTek、Qualcommのファームウェアとライセンス(`iwlwifi-8265-36.ucode`、`LICENCE.iwlwifi_firmware`など。SHA-256を検証する) |

ダウンロードできないときは、それぞれのスクリプトが何をダウンロードできなかったかと、場所を指定する変数(`OB_URL`、`OB_DL`、`NB_URL`、`NBDL`、`FW_URL`)を表示して止まる。Wi-Fiを含めないときは`make TARGET=_X64_ WIFI=0`とする。このときは`~/tessronos-netbsd-x64/rumpnet.o`を使い、上の3つは不要である。

| ツール | 用途 |
| --- | --- |
| x86_64-elf-gcc(`~/.local/xPacks/x86_64-elf-gcc`) | カーネルとプログラム、newlib |
| x86_64-w64-mingw32-gcc | UEFIのローダ(PE32+) |
| NASM 3.01(`~/.local/xPacks/nasm`) | x64のブラウザのコーデックのSIMDの部分(14.3.1)。`tools/x64/build_toolchain.sh nasm`で作る。NASMは署名もハッシュの一覧も公開していないので、リリースの`git.id`がChromiumのDEPSで固定しているNASMと同じで、tarballのソースがそのチェックアウトと一致することを確認した上で、tarballのSHA-256をスクリプトに固定して照合する |
| WindowsのQEMU(`C:\Program Files\qemu`)とOVMF(`edk2-x86_64-code.fd`) | テスト用のマシン。WSLのaptのQEMUは使わない |

## 12.2 ビルド

makefileは`build_make/`にあり、再帰呼出しをしない1つのmakefileで全体をビルドする。対象マシンは`TARGET`で選ぶ。`TARGET`からアーキテクチャ`ARCH`(`aarch64`、`_X64_`では`x86_64`)とコア依存のディレクトリの名前`ARCH_CORE`(`armv8a`、`x86_64`)が決まり、ツールチェーン、命令セットの基準、インクルードパスとソースの一覧のコア依存の部分がそれで切り替わる(4.8)。出力先は`ARCH`によらず`TARGET`と`BUILD_ID`で決まる。

| コマンド | 生成物 |
| --- | --- |
| `make TARGET=_QEMU_VIRT_` | `tessronos.elf` |
| `make TARGET=_QEMU_VIRT_ qemu` | ビルドしてQEMUで起動する(Ctrl-A Xで終了) |
| `make TARGET=_RPI5_` | `tessronos.elf`、`tessronos.img`、`tessronos.map` |
| `make TARGET=_RPI5_ sdimg` | SDカードのイメージ`sd.img`(12.5) |
| `make TARGET=_RPI5_ sd SD=<FATパーティション>` | FATパーティションに`tessronos.img`と`config.txt`をコピーする |
| `make TARGET=_RPI4_` | Raspberry Pi 4、400、CM4の`tessronos.img`。ARMv8.0-Aで作る(3.6.2) |
| `make TARGET=_RPI4_ sdimg` | Raspberry Pi 4系のSDカードのイメージ`sd.img`(12.5、3.6.5) |
| `make TARGET=_RPI4_ qemu-rpi4`、`test-rpi4` | QEMUの`raspi4b`で起動する、カーネル内のテストを実行する(3.6.6) |
| `make TARGET=_X64_` | x64の`tessronos.elf`と、ESPの中身`obj/_X64_<BUILD_ID>/esp/`(`EFI/BOOT/BOOTX64.EFI`、`tessronos.elf`、`cmdline.txt`)。設定は`build_make/x64.mk`(3.10) |
| `make TARGET=_X64_ qemu-x64` | ビルドして、ESPのディレクトリをFATのディスクとしてWindowsのQEMUの`q35`で起動する |
| `make TARGET=_X64_ sdimg`、`qemu-x64-disk` | x64のディスクのイメージ`sd.img`(ESPとシステムのボリューム、3.10.4)、それをNVMeとして接続して起動する |
| `make TARGET=_RPI5_ WIFI_NET80211=obsd`、`make TARGET=_RPI4_ WIFI=1 WIFI_NET80211=obsd` | Raspberry PiのWi-FiをOpenBSDの802.11のレイヤで動かす(既定は`nbsd`。`~/tessronos-obsd-arm/obsdwifi.o`、Pi 4系は`-v80`。09章9.6.8) |
| `make TARGET=_RPI4_ WIFI=1` | Pi 4、Pi 400、CM4のWi-Fiを含める(既定は含めない)。`~/tessronos-netbsd-v80-wifi/rumpnet.o`と`~/tessronos-wpa-arm-v80/wpasupp.o`をリンクし、CYW43455のファームウェアの実身を載せる(CYW43456のPi 400などはWi-Fiなしで動く)。ファームウェアの置き場は`WIFI_FW_DIR`で変えられる(09章9.6.8) |
| `make TARGET=_RPI5_ WIFI=0`、`make TARGET=_QEMU_VIRT_ WIFI=1` | Raspberry Pi 5のWi-Fiを除く(Pi 5は既定で含める)。QEMU virtの`WIFI=1`は、ホストのないマシンで異常が起きないことと、基板の設定の取り出し(KTESTのグループ`wifinv`)を確認するためのもの(09章9.6.8) |
| `make TARGET=_X64_ WIFI=0` | Wi-Fiを除く(既定の`WIFI=1`は含める。09章9.6)。含めるときはOpenBSDの802.11のレイヤとiwm(`~/tessronos-obsd-x64/obsdwifi.o`、9.6.9)と、WPA2の部分`~/tessronos-wpa-x64/wpasupp.o`をリンクし、ファームウェアの実身とWi-Fiの設定の実身をシステムボリュームに載せる |
| `make pkg PKG=<id>` | アプリケーションのパッケージ`obj/pkg/<id>.tpk`(11.8) |
| `make TARGET=<機種> sysup` | システムのパッケージ`obj/<TARGET><BUILD_ID>/TessronOS-<機種の名前>-<TS_VERSION>.tpk`(機種の名前は`Pi5`、`Pi4`、`x64`、`QEMU`。例:`TessronOS-Pi4-R1.500.tpk`)。機種は`_RPI5_`、`_RPI4_`、`_X64_`(`_QEMU_VIRT_`はテスト用)。システムの実身はイメージと同じ指定(`DESKTOP=1`、`MOZC=1`、`BROWSER=1`、`WIFI`など)で作ったものを集めるので、配布するイメージと同じ変数で作る(11.8.1) |
| `make TARGET=<機種> sdzip` | 配布用に圧縮したイメージ`obj/<TARGET><BUILD_ID>/TessronOS-<機種の名前>-<TS_VERSION>.zip`。中身は`TessronOS-<機種の名前>-<TS_VERSION>.img`の1つで、`sd.img`と同じ内容 |
| `make clean` | 生成物を削除する |

変数で構成を切り替える。

| 変数 | 機能 |
| --- | --- |
| `DESKTOP=1` | テストではなく、デスクトップが起動する構成でビルドする |
| `UITEST=1` | `DESKTOP=1`と併せて、UIテスト用の操作機能を組み込む(12.4) |
| `TESTAGENT=1` | `DESKTOP=1`と併せて、実機テスト用のリモート操作を組み込む(12.4.1)。テスト用イメージだけ |
| `KTEST=1` | カーネル内のテスト(12.3)を組み込む |
| `MOZC=1` | Mozcの変換エンジンと辞書をリンクする(先に`tools/mozc/build.sh`でビルドしておく。x64は`tools/x64/mozc/build.sh`) |
| `NETSTACK=netbsd`、`NETSTACK=lwip` | TCP/IPスタックを選ぶ。デフォルトは`netbsd`で、NetBSDのスタックのオブジェクトがなければ、リンクの前に`tools/netbsd/build.sh`を実行して作る(12.7)。`lwip`は以前のlwIPである(第9章) |
| `RUTRACE=1` | NetBSDのスタックで、ロックを5秒以上待っているタスクをコンソールに出力する(9.1.2) |
| `BROWSER=1`、`BROWSER_BLINK=1` | 別のリポジトリ`tessron-chromium`(既定はTessronOSと同じディレクトリ。`CHROMIUM=<場所>`で変える)からブラウザを取り込む。事前にChromiumのコンパイルが必要である(14.3)。`TARGET=_X64_`ではx64のもの(`TS_ARCH=x86_64`、オブジェクトの置き場は`~/tessronos-obj-x64`、生成物はx64の`UOBJ`、12.2.1)を作る(14.3.1)。Raspberry Pi 4系(`TARGET=_RPI4_`)では使えない |
| `KT_V8=<テスト名>,…`、`KT_SITE=<URL>`、`KT_SITE_WAIT=<ミリ秒>`、`KT_SITE_CLICK=<x>,<y>,…` | テストグループ`v8`のうち指定したテストだけを実行する、指定したサイトだけを開いてスクリーンショットを撮る、2枚目を撮るまでの時間を決める、その後に画面の座標をクリックする(14.10) |
| `WV_TEST=<ディレクトリ>` | `BROWSER=1`のテスト用ディスクの`/boot`に、そのディレクトリの`wv_Packages`と`wv.deb`(開発用のマシンでRaspberry Piのリポジトリから取ったインデックスとWidevineのパッケージ)を載せる。x64では`wv_update.json`と`wv.crx3`(Googleのコンポーネント更新サービスの応答のアドレスをテスト用サーバの`http://127.0.0.1:18170/gup/crx/`にしたものと、Widevine CDMのコンポーネントのファイル)を載せる。`test_blink_wv`がインストールした状態をテストするときだけ使い、リポジトリにもシステムのボリュームにも入れない(14.6.3.1) |
| `V3D=1`、`V3D=2`、`V3D=3` | Raspberry Pi 5のGPUを識別する、さらにMMUと割込みを設定して使えるようにし、起動の後で実機でのテストとウインドウの合成の確認をまとめて行う、テストをせずに合成の確認だけをしてGPUで合成する(13.3.1、13.8.2、13.10) |
| `GL=1` | MesaのEGLとOpenGL ESをプロセス用の静的ライブラリに作り、小物「歯車」とテスト用のプログラムにリンクする(13.6。x64は`tools/x64/mesa/build.sh`、13.11.9) |
| `SNDTEST=1`、`HEVCTEST=1` | Raspberry Pi 5で、起動の後に音の出力先、HEVCのデコーダを実機で確認する(6.5.4、6.7.1) |
| `KLOCKSTAT=1`、`STKWATCH=1` | カーネルのロックの取得と待ちを場所ごとに数える、実行中でないタスクのスタックの書き換えを監視する(4.3.2、4.7.6) |
| `KLOCK=<0〜3>`、`KLOCKCHECK=1` | カーネルの排他の方式を変える(0は大きなロック、3はスケジューラのロックとオブジェクトごとのロック。既定はArmが0、x64が3)、ロックの順番の検査をテスト用のビルドでなくても入れる(4.3.3)。`-D`の定義なので`BUILD_ID`を分ける |
| `X64_CMDLINE=<設定>`、`SD_LOG_MB=<MB>` | x64のイメージのESPに置く`cmdline.txt`の中身、ログ用のパーティションTSLOGの大きさ(3.10.7) |
| `DWC2=0` | Raspberry PiのUSB-CのポートのDWC2を調べず、使わない(3.6.7) |
| `PCIE_POLL=1` | Raspberry Pi 4系で、PCIeの先のxHCIを割込みではなく一定の間隔のポーリングで見る(3.6.4) |
| `OBJTEST=0` | Raspberry Pi 5と4系で、起動の後のデバイスの実身とボードの確認(`objtest:`)を行わない |
| `TS_VERSION=R1.500` | システムのバージョン。システムの実身の情報に`VERSION`として出る |
| `PKGS="a.tpk …"` | イメージに組み込むアプリケーションのパッケージ。既定は`etc/pkg/`の.tpk、x64では`etc/pkg/x64/`の.tpk、Raspberry Pi 4系では`etc/pkg/rpi4/`の.tpk(`build_make/rpi4.mk`、11.8) |
| `BUILD_ID=<名前>` | 出力先を分ける。中間の生成物は`$(OBJ_ROOT)/<マシン><名前>/`と`$(OBJ_ROOT)/user_<名前>/`に、イメージ、パッケージ、テスト用ディスクは`obj/<マシン><名前>/`に、カーネルは`tessronos<名前>.*`になる(12.2.1)。BUILD_IDの違うビルドは同時に実行できる |
| `OBJ_ROOT=<ディレクトリ>` | 中間の生成物の置き場。既定は`~/tessronos-obj-ts/<ソースツリーの場所>`(WSLのext4)。`OBJ_ROOT=obj`で、前と同じ`build_make/obj`の下に置く(12.2.1) |

システムボリュームは、`etc/def`(システムの実身)、`etc/xtad`(サンプルと原紙)、ビルドで生成したプログラムの実身、組み込むパッケージの実身から`tools/mktsfs`で作成する。

実時間制御TAD(第15章)の実行系`lib/librtad/`と再生部`lib/librtplay/`は、再生するアプリ(スライドとMPEG Player)に、文章と図形の表示部(`TV_LIB_SRCS`)とともに`TV_USER`でリンクする。newlibだけのスライドは`lib/librtplay/rtp_mlock.c`もリンクする。実行系はカーネルのテストのためにカーネルにも入り、デスクトップは`<realtime>`だけのレコードを見分けるのに`rt_find`を使う。音を鳴らす実時間メディア(`application/rtmedia/`)はnewlibとlibmを使うプロセスで、MP3のデコードにminimp3(CC0、`application/rtmedia/minimp3.h`)を使う。ビルドでその実行ファイル`rtmedia.elf`(`UOBJ`、12.2.1)を作り、プログラムの実身(`01a0d8c4-52da-7c34-9d45-be6f708192a3`)のレコード1にする。実時間制御TADの実身を開くアプリと映像のデコードは、別のリポジトリ`ffmpeg_btron`のMPEG Playerである(11.12)。MPEG Playerは再生部と表示部をTessronOSの作業ツリーのソースからコンパイルしてリンクするので、それらか`include/ts/rtplay.h`、`rtmedia.h`を変えたときと、MPEG Playerを変えたときは、`wsl.exe bash ffmpeg_btron/tessron/build-tessron.sh`でパッケージ`mpegplay.tpk`を作り直し、`etc/pkg/`に置く。AArch64のランタイムライブラリ(`~/ffbt/ts-rt`)は、プロセスのシステムコールの入口(`lib/libts/svc_stub.S`)を含むので、システムコールが増えたときは`build-ffmpeg-ts.sh rt`(Raspberry Pi 4系は`TS_CPU=cortex-a72`も)で作り直す。x64のものは`TS_ARCH=x86_64`で作って`etc/pkg/x64/`に、Raspberry Pi 4系のものは`TS_CPU=cortex-a72`で作って(FFmpegも`TS_CPU=cortex-a72`の`build-ffmpeg-ts.sh`で一度作っておく)`etc/pkg/rpi4/`に置く。

### 12.2.1 出力の置き場とビルドのロック

ビルドの出力は中間の生成物と、Windowsから使うものに分けて置く。中間の生成物は、`/mnt/c`(WSLから見たWindowsのドライブ)よりファイルの読み書きが速いWSLのext4に置く。

| 置き場 | 置くもの |
| --- | --- |
| `$(OBJ_ROOT)/<TARGET>[_ktest]<BUILD_ID>/`(makefileの`OBJDIR`) | カーネルのオブジェクトと依存関係のファイル、`offset.h`、構成を記録するファイル(`*.stamp`)、システムのボリューム`sysvol.img`、x64のUEFIのローダ`BOOTX64.EFI`、イメージとパッケージを組むときの一時ファイル |
| `$(OBJ_ROOT)/user_<BUILD_ID>/`(`UOBJ`。BUILD_IDがなければ`user/`。Raspberry Pi 4系はその下の`v80/`、x64は`x64/`) | 利用者のプログラム(`*.elf`)と実身のレコード(`rec/`)、それらがリンクするライブラリ(`lib/`、`psx/`)、ブラウザ(`browser.elf`) |
| `build_make/obj/<TARGET>[_ktest]<BUILD_ID>/`(`OUTDIR`) | SDカードとディスクのイメージ`sd.img`、システムのパッケージ(`.tpk`)と圧縮したイメージ(`.zip`)、x64のESP(`esp/`)、テスト用ディスク(`test_disk.img`、`usb_*.img`、`sd_card.img`、`vols/`、`btcd.iso`)、テスト用のFTPのファイル(`ftp/`) |
| `build_make/obj/pkg/`(x64は`obj/x64/pkg/`、Raspberry Pi 4系は`obj/v80/pkg/`) | `make pkg`、`make befcpkg`などで作るアプリケーションのパッケージ |
| `build_make/tessronos<BUILD_ID>.*` | カーネル(`.elf`、`.img`、`.map`、`.lst`) |

`OBJ_ROOT`の既定は`~/tessronos-obj-ts/<ソースツリーの場所>`で、場所はツリーのパスの`/`を`_`にしたもの(`/mnt/c/home/TessronOS`なら`~/tessronos-obj-ts/home_TessronOS`)である。ソースツリー(gitのworktreeを含む)ごとに分かれる。Windowsから使うもの(SDカードに書くイメージ、WindowsのQEMUが読むディスクとESP、WindowsのPythonのFTPサーバが提供するファイル)は`OUTDIR`にあり、パスは前と変わらない。`make OBJ_ROOT=obj`とすると、中間の生成物も前と同じ`build_make/obj`の下に置く。前の置き場のパスでターゲットを指定したとき(`make obj/_QEMU_VIRT__ktestx/lib/libftp/ftp.o`、`make obj/docedit.elf`など)は、今の置き場のものを作る。

中間の生成物とプログラムはBUILD_IDごとに分かれるので、BUILD_IDの違うビルドは同時に実行できる。同じBUILD_IDのビルドを同時に実行してはいけない(カーネルの`tessronos<BUILD_ID>.elf`も同じファイルになる)。アーキテクチャや`-D`で効く構成(`KTONLY`など)が違うときも、BUILD_IDを分ける。新しいBUILD_IDでは初めのビルドでプログラムもすべてビルドする。

ビルドのロックは`/tmp/tessronos-make.lock`で、`tools/mklock.sh`で取る。

| 操作 | コマンド | ロック |
| --- | --- | --- |
| ビルド | `tools/mklock.sh build make -j16 TARGET=... BUILD_ID=...` | 共有。ほかのビルドと同時に実行される。実行中に読むファイルが変わったら、同じコマンドをもう一度実行する |
| すべてのビルドが読むファイルの変更(`include/`などのヘッダ、`build_make/`のmakefileと`*.mk`、ビルドが実行する`tools/`のスクリプト) | `tools/mklock.sh edit cp ...` | 取らない。変更はビルドを待たない。`tools/mklock.sh edit`はコマンドをすぐ実行し、前後で世代のファイル(`/tmp/tessronos-src.gen`)に1行ずつ追加する。`tools/mklock.sh build`は共有のロック(`flock -s /tmp/tessronos-make.lock`と同じ)を取って`make`を実行し、始める前に世代を記録する。`make`が終わったら、その間に世代が進んでいないか、`include/`などの`*.h`、`build_make/`のmakefileと`*.mk`、`tools/`の`*.sh`と`*.py`に`make`を始めた時刻より新しいものがないかを調べる(`edit`を通さない変更はタイムスタンプで見つける)。あれば古いヘッダと新しいヘッダが混ざっているおそれがあるので、同じコマンドをもう一度実行し、`mklock: changed during the build (...)`の行を出す。再実行の前に、そのBUILD_IDの`.o`のうち、`make`を始めた後に書かれ、`.d`で変わったヘッダに依存しているものを消す(コンパイル中に変更された`.o`は、変更より後のタイムスタンプで古い内容のまま残るため)。それより前の`.o`はヘッダより古いので、`make`が作り直す。作り直されるのは、変わったヘッダに依存する`.o`だけである。`make`が失敗したときも、その間に変更があればもう一度実行し、なければそのまま失敗で終わる。繰り返しは5回までで(`TS_MKLOCK_RERUN`で変える)、それでも変更が続くときは混ざっているおそれがあることを出して0以外で終わる。ブラウザなど、`mklock.sh`が探す場所(`OBJ_ROOT`と`build_make/obj`)の外にある生成物は、この処理の対象にならない。共有のロックは、前と同じ`flock /tmp/tessronos-make.lock make ...`(排他のロック)がビルドの終わりを待ち、ほかのビルドと混ざらないために残している。前のようにビルドが終わるまで変更を待たせる仕組みはない。QEMUのテスト、`ui_run.py`、`ktest64.sh`は、ロックの外で実行する。 |

ホームの下にあって、ビルドの間で共有する生成物(NetBSDのスタック、Mesaの`libtsgl.a`、ACPICA、OpenBSDのWi-Fiのレイヤ、wpa_supplicant、コマンド、Wi-Fiのファームウェア)は、makefileが作り直すときに`~/.tessronos-shared.lock`の排他のロックを取り、それをリンクするとき(カーネルとプログラムのリンク)に同じファイルの共有のロックを取る。作りかけのものを別のビルドがリンクしないようにするためである。ブラウザのオブジェクトの置き場(`~/tessronos-obj`、`~/tessronos-obj-x64`)は、tessron-chromiumの`tools/browser/build.sh`が段階ごとにロックを取る。Raspberry Piのファームウェアとデバイスツリー(`build_make/obj/boot`)は、すべてのビルドが共有する。

## 12.3 カーネル内のテスト

テストはカーネルにリンクしてQEMU上で実行する(`tests/ktest/`)。テストはカーネルのタスクとして動き、1件ごとに`KTEST <名前> PASS`か`KTEST <名前> FAIL`をコンソールに出力する。最後に`KTEST SUMMARY <成功数>/<総数>`で集計を出す。

```sh
make TARGET=_QEMU_VIRT_ KTEST=1 test
make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=part,ob BUILD_ID=pt test
```

`KTONLY`でテストグループを名前で選ぶ(カンマ区切りで複数指定できる)。グループは`tests/ktest/ktest_<グループ>.c`のファイル単位で、`task`、`sync`、`time`、`mem`、`smp`、`board`、`sd`、`proc`、`fat`、`tsfs`、`ob`、`om`、`dp`、`fn`、`wm`、`part`、`mn`、`cab`、`tv`、`vt`、`usb`、`devob`、`snd`、`net`、`so`、`v8`、`gpu`、`cal`、`kana`、`slide`などがある。`cal`は予定表の実身の読み書き(11.10)、`kana`はかな入力練習の配列の表、問題文、判定、成績と、キーを離した通知(11.14)、`slide`はスライドの並びと画面への収め方(11.11)、`vols`は多くのUSBのディスクとパーティションの同時のマウント、形式の識別、ボリュームをまたぐ仮身とほかのボリュームからの参照の記録、抜き差しの繰り返しと4コアでの同時の使用、26台を超えるディスクと40パーティションのディスク(7.6、8.3)、`vt`は文章の縦書きの組版と原稿用紙(11.21)、`prn`は印刷のPDFの構造、文字の位置、フォントのサブセット、画像と各アプリのページ(11.22)、`scr`は画面印刷で取り込んだ画素と画面の一致、図形実身と保存先のディスク、トレーと図形への貼り込み、PNGの書き出し、PDF(11.24)、`dfix`はディスクの検査と修復、ツリーとビットマップが同時に壊れたときの再構築、壊れた実身ブロックからの救出(7.5)、`rt`は実時間制御TADの実行系と二進TADへの変換(15.10)、`rump`は4つのCPUから同時にかけたNetBSDのスタックへの負荷(9.1.2)、`smp`の`test_wait_result`、`test_del_ended`、`test_exd_stack`は別のCPUが解除した待ちの結果と、終了したタスクのスタックを返すタイミング(4.3)をテストする。`smp`の`test_int_target`はデバイスの割込みを受けるCPUの切り替え(4.5.1)を、`klk`はカーネルのロックの順番の検査と、オブジェクトやタスクをまたぐ呼出しを全部のCPUで重ねたときの結果(4.3.3)をテストする。`devob`は、デバイス管理に登録しないデバイスの実身(乱数、画面、入力、キーボードとポインタ、USBデバイス、システムのレコード3、4、9)の属性、読み書き、保護、仮身、接続と切断の通知をテストし、実身を通す経路と直接の関数呼出しの時間を出力する(8.10)。`snd`は、サウンドデバイスのチャネル、変換、ミキシング、録音と、出力先の一覧と選択、再生中の切り替え、選んだUSBオーディオの機能を抜いたときに自動の出力先へ移り、挿し直すと戻ること(QEMUのモニタで2つ目の機能を接続する)、システムの実身のレコード8と機器の設定の`SOUND_OUT`による選択、ミキサーの混ぜ合わせ、プログラムごとと全体の左右の音量、ミュート、ソフトリミッタ、その設定の保存と読み戻しをテストする(6.5.1、6.5.6)。`fault`はデスクトップを起動し、Shift+Pauseによる強制終了と、例外で終了したプロセスのダイアログをテストする(5.5)。`fq`はデスクトップを起動し、デスクトップを止めた状態でのShift+Pause、解放のイベントが失われた後のShift+Pause、強制終了のウインドウからのウインドウのないプロセスの終了、システムコールの中で止まったプロセスの2段階の終了、デスクトップがプロセスの依頼で出したパネル、デスクトップの再起動をテストする(5.5.6)。`cred`は、デスクトップと同じ方法で起動したプログラムが利用者の資格で動き、時刻、ネットワーク、ボリューム、時計とネットワーク設定の実身へのアクセスを拒否され、タイムゾーン、キー配列、ユーザ環境設定の項目、ユーザ情報と機器の設定とシステムの実身へのアクセスを許可されること、一時的に与えた管理者の権限で時刻を変えられ、返したあとと期限が過ぎたあとは拒否されること、ログインした利用者の資格から管理者のグループが除かれること、管理者のパスワードの最初の設定と照合、信頼するプログラムの識別、デスクトップのタスクが作る実身の所有者、USBの媒体の保護をテストする(8.7)。`board`は、メモリ上に組み立てたデバイスツリーとボードのリビジョンから、Raspberry Pi 5、CM5、CM5 Lite、Raspberry Pi 4、400、CM4を識別すること、起動したマシンのデバイスツリーからノードのプロパティと`ranges`、`dma-ranges`を読めることをテストする(3.5.4)。`sysup`は、テストの中で作ったシステムのパッケージ(この機種の新しいバージョン)で、システムの実身と起動用のファイルが新しくなり、利用者の実身、機器の設定、利用者が箱に足した仮身、利用者の起動の設定が残り、前の起動用のファイルが`.old`に残ること、同じパッケージでもう一度行ったときと、電源が切れた後の起動のときのやり直しで同じ結果になること、別の機種とアーキテクチャのもの、新しくないもの、システムのものでないものを拒否することをテストする(11.8.1)。続けて`/boot`の下に作ったシステム登録の形のESP(`\EFI\TESSRON\`にローダ、カーネル、設定、`INSTALL.TXT`、`\EFI\BOOT\BOOTX64.EFI`はWindowsのもの)で、起動用のファイルが`\EFI\TESSRON\`に置かれ、Windowsのローダとルートディレクトリが変わらないこと、入れ替えの名前の変更のそれぞれの後で止めても、ローダの探し方でカーネルが読め、次の起動時のやり直しで完了すること、200MBのESPに69MBのカーネルのマシンを1/64にした大きさで空き容量の見積りが通ることをテストする。`sd`は`SD_EMMC=1`を付けるとeMMCに対して行う。`KTONLY`はコンパイル時の定義なので、`BUILD_ID`で出力先を分けないと反映されない。

テスト環境は次の構成のQEMUである。

| 項目 | 値 |
| --- | --- |
| マシン | `virt,gic-version=2`、`cortex-a76`、4コア、2GB |
| ディスク | virtio-blkのGPTのテスト用ディスク。テストが作り直すTSFSのパーティション、FAT32のブートパーティション、システムボリューム、FAT16とFAT12のパーティション |
| ネットワーク | virtio-net。`NET=user`でホストへ接続できる。このとき、ホストでテスト用のFTPサーバ(`tools/ftpd.py`)とHTTPサーバ(`tools/httpd_test.py`、2180番がJSON、2181番がServer-Sent Events)が起動し、マイクロスクリプトのテストは仮身サンプルの通信用スクリプトをそのまま実行する |
| USB | xHCIに、キーボード、タブレット、マスストレージ2台、オーディオ |

`tools/qemu_run.sh`がQEMUを起動してコンソール出力を監視し、`tools/qemu_qmp.py`がテストからの要求に応じて、USBデバイスの抜き差しやキー入力をQEMUに送る。

テストは目で見るための画面のイメージを`/boot`にPPMで残す(`kt_shot`、`prn`のグループのページの画像)。1枚が3MBほどあり、全部のグループを実行するとブートパーティションの空き容量(システムの実身のレコードを置いた後で約47MB)を超える。そのため、画面のイメージは書いた後の空き容量が16MB(`KT_SHOT_KEEP`)を切るときは書かず、コンソールに`KTEST <パス> not written`と出す。また、テスト用ディスクは実行をまたいで残るので、テストの始めに前の実行のイメージ(`/boot`の`.PPM`)を消す。全部のグループの実行では後のグループのイメージが残らないことがあるので、見たいイメージは`KTONLY`でそのグループだけを実行して撮る。

x64(3.10)では、`make TARGET=_X64_ KTEST=1 BUILD_ID=<名前> sdimg`でテストを組み込んだディスクのイメージ`obj/_X64__ktest<名前>/sd.img`を作り、Git Bashから`tools/x64/ktest64.sh obj/_X64__ktest<名前>/sd.img`でWindowsのQEMUの`q35`に接続して実行する。`ACCEL=whpx`でWindowsのハイパーバイザを使い(全部のテストで約6分)、`NET=user`でホストのテスト用サーバとつなぐ。`KT_BOARD`はx64でも1で、ディスクは`nvmea`である。QEMUには`-rtc base=localtime`を付け、RTCをWindowsの入ったPCと同じローカル時刻にする(9.5.1)。`EXTRA`に書いた引数はQEMUにそのまま追加する(Intel HDAの`-device intel-hda -device hda-duplex`、電池のテストのSSDTの`-acpitable`など)。`NVME4K=<イメージ>`は、論理ブロックが4096バイトの2台目のNVMe(`nvmeb`、QEMUの`nvme`の`logical_block_size=4096`)としてそのイメージを接続する(`pcdev`のグループの`test_nvme_4k`が書き込む)。`NVME2=<イメージ>`、`NVME3=<イメージ>`、`NVME4=<イメージ>`、`NVME5=<イメージ>`は、そのイメージを2台目から5台目のNVMe(`nvmeb`、`nvmec`、`nvmed`、`nvmee`)として接続し、`VARS=<ファイル>`はOVMFの変数の書き込めるコピー(`edk2-i386-vars.fd`をコピーしたもの)を2つ目のフラッシュとして与えて、ブートエントリを次の起動に残す(システム登録の`sysinst`のグループ、3.10.10)。`NVME5BOOT=1`は`NVME5`を起動の最初の候補にする(1台目を空のイメージにして、起動したディスクが5台目でも起動できることを確認する。3.10.10)。`VARS`がないときは、システム登録のテストが起動したディスクのESPに残すブートエントリの依頼(`\EFI\TESSRON\BOOTREG.TXT`)を、起動の前と終わった後にイメージから消す(`tools/tests/mkwindisk.py rm-esp`)。依頼が残っているとローダがエントリを書いて再起動し、`-no-reboot`のQEMUがそこで終わるためである。`OFFWAIT=1`は、`KTEST SUMMARY`の後もマシンが電源を切るまで待ち、電源を切る前の行(NVMeのシャットダウンの行など、06章6.2.1)をコンソールの出力に残す。`pcdev`のグループ(`tests/ktest/x86_64/ktest_pcdev.c`)は、PS/2、ACPIの電源デバイス、SATA、IntelのLAN、HID over I2Cの実身と、そのUUIDが再起動しても変わらないことをテストする。

x64のTCGでは、タイマーの待ちが何もしていないホストでも約2ms遅れる(`ACCEL=whpx`では同じイメージで約0.35ms)。ホストのプロセッサを全部使い切ると、遅れは6〜8msになる。時間を見るテストは次の形で書く。

- 早く終わる待ちはホストによらずエラーとする。
- 遅れは複数回測り、最良の値でカーネルを判定し、最悪の値には止まったプロセッサで説明できる範囲の上限を置く。カーネルが遅らせる待ちは毎回遅れ、失われた待ちは終わらない。
- 処理の完了は決まった時間で打ち切らず、進み(数の増加)が止まったときに失敗とする。

ページ数の照合では、プロセスの空間から外したページテーブルは、ほかのCPUがディスパッチャを通るまで返らない(`knl_pt_reclaim`)ことを見込む。

`USBQ=1`を付けると、USBのデバイスをQEMU virtのテスト用マシンと同じ構成にする(1つ目のxHCIのキーボード、タブレット、USB 3のポートのディスク、ハブとその先のオーディオ、ディスク、マウス、USB 2だけの2つ目のxHCIのディスク3台とキーボード)。ディスクのイメージは`make TARGET=_X64_ KTEST=1 BUILD_ID=<名前> obj/_X64__ktest<名前>/usb_disk.img`などで、イメージと同じディレクトリに作る(`usb_disk.img`、`usb_disk2.img`、`usb_disk3.img`、`usb_sf32.img`、`usb_fd.img`、`vols/`、`btcd.iso`)。QEMUにQMPのポートを開き、`tools/qemu_qmp.py`(WindowsのPython)がテストからの抜き差しの要求をQEMUに送る。このときWSLのパス(`/mnt/c/...`)はWindowsのパスに変換し、WindowsのQEMUが持たない「ファイルから読む文字デバイス」は、`/dev/zero`の代わりに、0を送り続けるループバックのTCPのポートにする。WindowsのQEMUは、このポートから0が届いている`usb-serial`を`device_del`で抜くと、3回に1回ほど異常終了する。そのため`device_del`を送る前にこのポートの接続を切り(QEMUは文字デバイスの接続が終わるとデバイスを外す)、0.5秒待ってから`device_del`を送る。x64のテストは、2つのxHCIがあるとき(`kt_qemu_devs`)だけQEMUのデバイスを前提にするテスト(`usb`、`devob`、`snd`の`KT_RUN_QEMU_DEVS`と、`vols`、`grow`、`btcd`のグループ)を実行し、ないときは理由を表示してスキップする。オーディオの音を取るときは`USBQ_AUDIO=wav,path=<ファイル>`とする。WindowsのQEMUでは、USBのオーディオのアイソクロナス転送が1秒あたり150〜210パケットしか進まない(QEMUのxHCIがアイソクロナス転送のTDを仮想時計のタイマで1つずつ送り出し、Windowsのタイマが粗い)。そのため、再生の時間を見る`snd`の`test_play`、`test_mix`、`test_outputs`はx64でも、WindowsのQEMUで実行したQEMU virtでも通らない。この3つは、LinuxのQEMUで動くQEMU virtのテストで確認する。

Raspberry Pi 4系のイメージは、QEMUの`raspi4b`で`make TARGET=_RPI4_ KTEST=1 BUILD_ID=<名前> test-rpi4`としてテストする(3.6.6)。テストの中の「QEMUではなくボード」を表す条件は`KT_BOARD`で、Raspberry Pi 5と4系で1になる。

電源断のテストは`make KTEST=1 KTONLY=tsfscut BUILD_ID=cut powercut`である。TSFSのボリュームを変更し続けるマシンを任意の時点で強制終了し、`fsck.tsfs`でエラーがないことを確認する。

## 12.4 デスクトップのUIのテスト

UIのテストは`tools/ui_run.py`で行う。`make TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1`でビルドしたカーネルは、2つ目のシリアルポートから1行ずつコマンドを受け取り、キーボードとポインタのイベントとして直接入力する(`application/desktop/dtuitest.c`)。ポインタは指定した座標へ一度に移動し、1行の処理が終わるごとにコンソールへ`@ut ok`と応答するので、固定時間の待ちは不要である。`ui_run.py`は最初の1回だけマシンを起動し、デスクトップの立ち上げが終わった時点のメモリとディスクの状態を一時ディレクトリの`ui_<tag>`に保存する。立ち上げの終わりは、キャビネットが自動で開いたプロセス(原紙箱など)がウインドウを描き終えたときにデスクトップが出す`TessronOS desktop: settled`の行で知り(4章)、その後`sync`でデスクトップのループが一回りしてから保存する。前は起動の通知の1.5秒後に保存していたので、原紙箱のプロセスが開き終わる前の状態を保存することがあった。`not settled`の行が出たとき、または300秒たっても出ないときは、保存せずに終了コード2で終わる。以後は、カーネルとディスクが同じである限りその状態から再開するので、1回のテストは起動を含めて数秒で終わる。

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
| `keydown CODE [MODS]`、`keyup CODE [MODS]` | キーを押したままにする、離す。同時に押すキーや、押したままのシフトをテストするのに使う |
| `open UUID` | 仮身をダブルクリックしたときと同じように実身を開く |
| `run PATH [N …]` | プログラムのファイルをプロセスとして起動する。数値(8個まで)が起動時の引数になり、プロセスIDを応答する。例外で終了するプロセスを起動して、例外のダイアログ(11.1)のスクリーンショットを撮るときなどに使う |
| `win 名前`、`wins` | 指定した名前のウインドウの外形、表示中のウインドウの一覧 |
| `front TAG 名前1\|名前2…`、`hidden TAG 名前` | ウインドウが前面から指定の名前の順に並んでいること、指定した名前のウインドウが表示されていないことを確認する。どちらも2秒まで待ち、`ok TAG: in front`、`ok TAG: hidden`と応答する |
| `waitwin 名前 [S]`、`mark`、`sync [S]` | `waitwin`は指定した名前のウインドウが出るまでS秒(既定60秒)待つ。名前は行の残り全部で、空白を含んでよい(PCM Player)。最後の語が数字ならそれを秒数とする。`sync`は、デスクトップのタスクが`sync`を受けたことをループの先頭で知り、そのあと入力を読みに行って何もなかったとき(それまでに入れたキーやボタンを読み終え、その処理を終えたとき)まで、S秒まで待つ。ループが一周したことだけを見ると、段落を組む間に届いたキーをまだ読まないうちに応答してしまう(Ctrl+Eで閉じて保存する前に、保存の結果を確認してしまう)。どちらも`mark`からの経過時間をミリ秒で応答するので、起動や処理にかかる時間を測るのに使う |
| `linkdrag ウインドウ名 実身名 X Y [ボタン]` | 指定した名前のウインドウ(原紙箱やキャビネットのように仮身を並べた図形)の中から、指定した名前の実身の仮身を探し、その中央から画面のX, Yへドラッグする。仮身の並びが変わっても同じ仮身をドラッグできる。見つからなければ`err no such link` |
| `ready` | デスクトップがプロセスからの依頼を受けるチャネル(`DT_REQ_NAME`)を作り終えたら(最大50秒待って)`ok`と応答する。テストの始めに、コマンドを受け付け、`open`で実身を開ける状態になったことを確認する。チャネルは設定とプログラムを読んだ後に作るので、遅い起動では`uitest: ready`から数秒かかる |
| `wait MS`、`idle` | 待つ |
| `lag MS` | MSミリ秒のあいだ、ポインタを50ミリ秒ごとに左右へ動かし、画面のポインタがそこへ描かれるまでの時間を測る。最長の時間(ミリ秒)、動かした回数、200ミリ秒を超えた回数を応答する。プログラムの起動中などに、デスクトップが止まらないことを確認するのに使う |
| `stat` | 前回の`stat`からの、ウインドウマネージャの合成の回数、描いた部品の数、合成した画素数(千画素単位)、ポインタの移動の回数、ポインタを描き直した回数と、画面へ送った矩形の数、画素数(千画素単位)を応答する。描画の量を比べるのに使う |
| `pnstat` | 前回の`pnstat`からの、パネルを全体で描いた回数と、パネルを描いている最中に画面を組み立てた回数を応答する。ウインドウを開いたときや部品を書き換えたときに、パネルを何回描き、途中の絵を画面に出していないかを見るのに使う。`UITEST=1`のビルドだけにある |
| `xml 行`、`xmlopen 名前`、`xmlbig` | `xml`でxmlTADを1行ずつためる。`xmlopen`はためた図形を、指定した名前の新しい実身にし、仮身一覧で開く(ウインドウは100,60、500×400)。文章は受け付けず`err not a figure`と応答する。`xmlbig`はためたものを`mkbig`で作った実身のレコード0に書く。その中の`ffffffff-ffff-ffff-ffff-ffffffffffff`はその実身自身に、`eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee`はその1つ前に作った実身(`big1`)に置き換える(自分を指す仮身、テスト用に作った別の実身を指す仮身)。文章や図形をプロセスのアプリで開いてテストするのに使う |
| `mkbig doc\|fig\|calc N [M]`、`sum [UUID]` | `mkbig`は原紙から大きな文章(N段落)、図形、表計算(N行M列)の実身を作り、そのUUIDとバイト数を応答する。`sum`は実身のレコード0のバイト数とハッシュ(FNV-1a、64ビット)を応答する。UUIDを省くと`mkbig`で作った実身を見る。大きな実身の表示と保存のテストに使う |
| `svcstat`、`waits` | `svcstat`は前回からの、プロセスのシステムコールの回数、プロセスの表のロック(`prc_mtxid`)を取った回数とそのうち待った回数を応答する(5.6.1)。`waits`は今のタスクの状態(実行中、実行可能、時間待ち、起床待ち、セマフォ、イベントフラグ、ミューテックス、メッセージバッファ、その他の待ち、強制待ち)の数と、最も多く待たれているオブジェクトを3つ(`m12x30`はミューテックス12を30タスクが待つ)応答する。どちらも`UITEST=1`のビルドだけにある |
| `stkuse` | 今あるタスクと、削除したタスクの開始アドレスとスタックの大きさの組ごとに、スタックの最も深い使用量をコンソールに`stkuse:`の行で出す。残りが4分の1を切るものには`LOW`を付ける(4.7.7) |
| `look 番号 値` | 見た目の表(`include/ts/look.h`)の項目を書き換えて、画面を合成し直す |
| `wifitest` | Wi-Fiのデバイスがないマシンに、決まったアクセスポイントの一覧を返すテスト用のWi-Fiのデバイスを置く(`device/net/wifi/wifi.c`)。Wi-Fiを入れずにビルドしたカーネルでは`err no wifi`と応答する |
| `accpos ID`、`progrm UUID`、`proginst UUID`、`proginstq UUID`、`progres`、`adminpanels`、`refs UUID`、`mkpkg` | 小物の順番と表示、プログラムの削除とインストール、管理者のパネルが出た数、実身の参照数、テスト用の小さなパッケージの実身の作成(11.2.1、11.8)。`proginstq`は権限のないプロセスとして置き換えを依頼し、すぐに応答する |
| `apps UUID id id …` | 実身の機能付箋を書き直す。先頭が既定の起動になる。機能付箋の多い実身を付箋指定のパネルを通さずに作る(`tools/tests/ui_exec_many.steps`) |
| `shot NAME`、`expect 文字列` | 画面をPNGで保存する、コンソールに指定の文字列が出るのを待つ(`ui_run.py`が処理する) |
| `absent 文字列` | その実行でコンソールに指定の文字列がまだ出ていないことを確認する(`ui_run.py`が処理する)。スクリプトの最後に`absent !OACV!`を置き、権限の不足で拒否された呼出しがないことを見る |
| `hmp コマンド` | QEMUのモニタにコマンドを送る(`ui_run.py`が処理する)。`$VOLS`は`--vols`のイメージのコピーの置き場になる。`drive_add`と`device_add usb-storage`でディスクを接続し、`device_del`で外す |
| `expectin S 文字列`、`logmark`、`expectsince 文字列`、`want 応答`、`keep NAME`、`same NAME`、`under NAME MS`、`hostmark`、`hosttime NAME MS`、`pixel X Y RRGGBB`、`pixelnot X Y RRGGBB`、`shotsame A B X0 Y0 X1 Y1`、`shotsum 名前 X0 Y0 X1 Y1` | `ui_run.py`が処理する。`shotsame`は撮った2枚のその矩形を画素で比べ、`shotsum`は撮った1枚のその矩形の画素のハッシュ(SHA-1の先頭16桁)を`ok ハッシュ`として応答し、後の`want`で前に記録した画面と比べる。`pixel`はその時点の画面の(X, Y)の色が指定の色であることを、`pixelnot`は指定の色でないことを確認する(QEMUのモニタの`screendump`で読む)。コンソールの出力だけでなく、実際に描かれたかを確認するのに使う。`expectin`は待つ時間を秒で指定する`expect`。`want`は直前のコマンドの応答が指定のとおりであることを、`keep`と`same`は直前の応答を名前で残して後の応答と同じであることを確認する。`under`は直前の応答(`ok MS`)がMSミリ秒以下であることを確認し、値を名前とともに出す。`hostmark`からの経過時間をホストの時計で測り、`hosttime`がMSミリ秒以下であることを確認する。`logmark`はコンソールのログのその時点の位置を記録し、`expectsince 文字列`はそれより後にその文字列が出ることを待つ(途中の状態で同じ行が前に出ていても数えない) |

メニューはサブメニューが開くまでに少し時間がかかるので、項目の上を通過するたびに`wait 150`程度を入れる。

`UITEST=1`でビルドしたカーネルは、プロセスの呼出しが権限の不足(`E_OACV`)で拒否されるたびに、コンソールへ`!OACV! pid=N "プログラム名" 操作 obj=UUID type=種類.副種類 "実身名" want=0x.. may=0x..`の1行を出す(`knl_ob_deny_note`、8.7.4)。`want`は求めた操作ビット、`may`は許されている操作ビットである。実身によらない呼出し(SVCゲートの権限の表、ボリュームのマウントなど)では`obj`以下を省き、SVCゲートでは`want`が機能コード、`may`が表の権限になる。`UITEST=1`でないビルドも`make OACVLOG=1`で同じ行を出す。デスクトップから起動したプログラムは利用者の資格で動くので(8.7)、この行で、プログラムが必要以上の権限で実身を開いていないか、システムのものに書き込もうとしていないかを探す。`tools/tests/ui_user_apps.steps`は、プログラム箱のプログラム(パッケージで入るものを含む)を利用者の資格で一つずつ起動してウインドウが出ることを見て、システム環境設定での壁紙の変更と、バックアップの保存と復帰を行い、最後に拒否された呼出しがないことを確認する。

`make PGCHECK=1`(`-DPF_FREE_CHECK=1`)でビルドしたカーネルは、解放した後に書き込まれたページを見つける(`peripheral_kernel/sysman/pfalloc.c`)。`knl_free_pages`は解放するページ全部にフレーム番号から作ったパターンを書き、どこから解放されたか(呼出し元のアドレス)をページごとの表に残す。`knl_alloc_pages`は渡す前にページをパターンと比べる。違っていれば、解放した後に誰かが書き込んだことになる(残ったマッピングや変換、デバイス、まだ持っていたコード)。空いたままのページをもう一度解放したときは、二重解放としてリストに入れない。表(ページごとに24バイト。解放した場所、所有者、解放した時刻で、4MBのブロックを3つずつ、最大16組で32GB分)はプロセスの回収のタスクが始まるときに作り、`pgcheck: on, N of M pages watched`を出す。所有者は、プロセスが手放したページ(umemの`pages_drop`)と空間を破棄するときのページ(`free_level`)について、そのプロセスとページのアドレスを書く(`knl_pf_check_owner`)。見つけたものは、プロセスが終わるたびに回収のタスクが出す(最初の24ページまで1ページずつ)。`pgcheck: N pages written while free`の後の行は、ページごとに、物理アドレス、崩れた最初の位置、崩れたワードの数、解放した場所のアドレスと何マイクロ秒前に解放したか、解放したpidとそのアドレスとプロセッサ、確保した場所のアドレスとそのpidとプロセッサ、その場所の64バイトを示す(アドレスはカーネルのelfの`nm`で引く)。`pgcheck: N pages given back while free`の行は最初に解放した場所と2度目に解放した場所を示す。オーバーヘッドは解放するときのページの書き込みと渡すときの読み出しだけなので、実機でも使える。このフラグは全部のコンパイルに効くので、`BUILD_ID`を分けて作る。

x86-64では、フラグに関わらず、変換を破棄する依頼(`shoot_cpus`、4.5.2)に上限を過ぎてから応答したプロセッサの回数を数え、0でないときに、回収のタスクが`TessronOS x64: N requests to drop translations were answered only past the bound`の行で、最後のプロセッサとともに出す。依頼する側は待つのをやめず、応答が揃うまでページを解放しない。テストは`cxx`のグループの`test_tlb_stale`で、プログラムの`tlbre`の2つのプロセスが、ほかのスレッドが書いて読み直すのを待っているページを手放し(`MADV_DONTNEED`)、その間にすぐ終わるプロセスを次々に起動して、古い中身が見えないこと、`PGCHECK=1`では空きのページへの書き込みと二重解放がないことを見る。

`--keep-disk ファイル`を付けると、実行を終えたディスクのイメージをそのファイルにコピーして残す。テストで作った実身を`tools/tsfs-export`で取り出して確認するのに使う(11.22)。

`--cdrom イメージ`を付けると、そのイメージを入れたUSBのCD-ROMデバイス(`media=cdrom`のドライブを持つ`usb-storage`、ブロック2048バイト)をつないで起動する。イメージが変わると、保存した状態を使わずに起動し直す。BTRONのCD-ROMのテスト(11.9、`tools/tests/ui_btcd.steps`)に使う。

WSL版のQEMUはユーザモードネットワークを持たないので、外部のネットワークを使うテスト(`--net user`)は、WindowsのPythonからWindows版のQEMUで実行する。

`--vols ディレクトリ`を付けると、マシンに何もつないでいないUSBのハブを加え、そのディレクトリのディスクイメージ(`tools/tests/mkvoldisk.py`)を実行ごとに一時ディレクトリへコピーする。スクリプトは`hmp`でそれを接続して外す。ディスク操作のテスト(11.26、`tools/tests/ui_vols.steps`、`tools/tests/ui_disk_grow.steps`)に使う。`--keep-disk`と一緒に付けると、実行が書き込んだディスクイメージをそのディレクトリに書き戻すので、次の実行を`--disk`と`--fresh`で起動し直して、再起動をまたぐ動作をテストできる(`tools/tests/ui_vols2.steps`)。

`--wav ファイル`を付けると、USBのサウンドデバイスを加え、その出力をQEMUがWAVEファイルに書く。音を出すテスト(実時間制御TADの`tools/tests/ui_rt.steps`、`ui_rt_player.steps`など)で、鳴った音の大きさと周波数を後から確認できる。

x64のイメージ(`make TARGET=_X64_ DESKTOP=1 UITEST=1 sdimg`)は`tools/x64/ui_run64.py --disk obj/_X64_<名前>/sd.img`でテストする。引数は`ui_run.py`と同じで、マシンの引数だけを`q35`、OVMF、NVMeのディスク、`virtio-net-pci`、2つ目のシリアルポート、ローカル時刻のRTC(`-rtc base=localtime`)に替える。`UI_ACCEL=whpx`でWindowsのハイパーバイザを使う。NVMeのデバイスは状態を保存できないので、毎回起動から始める(WHPXで約5秒)。`UI_USBBOOT=1`のときは、ディスクのイメージをNVMeでなくUSBのストレージ(`id=sysdisk`、ルートポート)として接続し、カードから起動したPCと同じく、システムのボリュームを`uda1`にする(ファームウェアはハブの先のディスクから起動しないので、ルートポートに接続する)。スクリプトの`hmp`の行では、`$DISK`が実行しているディスクのコピーを指す。`tools/x64/tests/ui_sysgone.steps`はこれで、デスクトップが起動した後にシステムのディスクを`device_del`で外し、接続し直す(11.26)。

ディスクの名前や並びがQEMU virtと違うために座標や名前が変わるスクリプトは、x64のものを`tools/x64/tests/`に置く。`ui_xfconv`、`ui_scrprint`(ファイル変換の一覧で`/boot`が`nvmea0`の2行目)、`ui_xfc_carry`(`nvmea0`のウインドウから仮身をドラッグする)、`ui_btcd`、`ui_sysenv_font`(USBのディスク`uda`が`nvmea`、`nvmea0`、`nvmea1`、`nvmea2`の後の5行目。`--vols`の置き場に`tools/tests/mkfontdisk.py`で`font.img`を作っておく。`ui_btcd`はファイル変換をプログラムの実身を開いて起動する。x64のイメージの小物のメニューは並びが違うため)、`ui_vols`(ディスク操作の〈接続〉の一覧で`uda0`の前が`nvmea0`と`nvmea2`の2行で、QEMU virtより2行上。ほかはQEMU virtのスクリプトと同じ)、`ui_diskfix`(壊したボリュームを`tools/tests/dfix_disk.py`で`mkvoldisk.py`の`volc.img`の第1パーティションに作り、USBのディスクとして接続する。一覧はシステムのボリューム`nvmea1`が先)、`ui_diskfix_boot`(`dfix_disk.py --pc`でx64のディスクのイメージに修復の依頼を書く)がある。`ui_vols2`はQEMU virtのスクリプトのまま通る。

以前の`tools/desk_act.sh`(QEMUのモニタからマウスを少しずつ動かす方式)は1回に数分かかるので、新しいテストには使わない。

### 12.4.1 実機テスト用のリモート操作(TESTAGENT)

Surface Go 2のようにシリアルポートのない実機でも、12.4のコマンドで操作し、画面とログを受け取れるようにするためのインタフェースである。OS側は`application/desktop/dttagent.c`と`ta_tls.c`、PC側は`tools/mcp/`のMCPサーバ(stdio)で、`tools/ui_steps.py`の.stepsの解釈を`tools/ui_run.py`と共有する。

安全のための決まりは次のとおりである。

| 項目 | 決まり |
| --- | --- |
| ビルド | `make DESKTOP=1 TESTAGENT=1`のテスト用イメージだけに入る。既定のビルドとリリースのビルドにはコードが入らない(`USE_TESTAGENT`と`SRCS_DESKTOP`の追加、`testagent.mk`) |
| 開く条件 | 起動の設定`cmdline.txt`に`testagent=<鍵>`があるときだけポートを開く。鍵は16進で64〜128桁(32〜64バイト)。ないか短ければ開かず、その旨をログに出す |
| 認証と暗号 | Mbed TLSによるTLS 1.2のPSK(`TLS_PSK_WITH_AES_128_GCM_SHA256`)だけを受け付ける。鍵が違う相手と平文のTCPはハンドシェイクで失敗し、1行も読まない。失敗の後は1秒受け付けない |
| 範囲 | 同じLANの中だけのテスト用である。IPv4のプライベートアドレス(10/8、172.16/12、192.168/16、169.254/16、127/8)以外からの接続は切り、PC側もそれ以外へ接続しない。PSKなので前方秘匿性はなく、鍵が漏れると記録された通信を復号できる。インターネットに公開しない |
| 表示 | 待ち受け、接続、切断を、デスクトップのメッセージ行とログに出す。接続中は`試験用の遠隔操作が有効: 接続中 この機械 192.168.0.143:7878、相手 192.168.0.150`のように、このマシンのアドレスと相手(PC)のアドレスの両方を出す。キーやボタンの操作でメッセージ行が消えると、3秒ごとの確認で出し直す。プログラムが出したメッセージがある間は上書きしない |
| 鍵の扱い | PCが`secrets`で作り、`~/.tessronos/testagent/<名前>.token`に置く(リポジトリの外。Windowsでは本人だけのアクセス権)。OSは読んだ後にメモリ上の設定から消し、起動のログとローダのログ(x64)では`*`に置き換える。ただし`cmdline.txt`自体には平文で残るので、SDやESPを読める人には見える |
| PC側 | 起動しても接続しない。`connect`で名前を指定したときだけ接続する |

シリアルポート`serb`は`UITEST=1`を併せたときだけ開く。`TESTAGENT=1`だけのイメージには、認証なしで操作を受け付けるインタフェースはない。一方で`USE_UITEST`のテスト用の機能(`OB_DENY_LOG`など)は入るので、このイメージを普段使いにはしない。

ポートはTCPの7878で、接続は同時に1つ、600秒無通信で切る。1行の形とその応答は12.4と同じで、応答の行に`@ut `が付かない。シリアルとネットワークの両方があるときも、1行ずつ順に処理する。

接続を1つずつ扱うので、相手が何も言わずに消えた接続(PCのWi-Fiの切り替え、スリープなど)を保持したままでいると、次の接続はlistenの待ち行列で待たされ、TLSのハンドシェイクに応答しない。実機では、画面を撮る手順の途中にWi-Fiを切り替えたところ、画面の送信がTCPの再送の打ち切りまで約8分止まり、その間に待ち行列にたまった接続はPCがあきらめた後に読まれて、ハンドシェイクの失敗(`-0x7280`、相手が閉じた)で次々に拒否された。消えた相手は次の4つの方法で早く検出して切る。

| 方法 | 動作 |
| --- | --- |
| TCPのkeepalive | 受け付けた接続に`SO_KEEPALIVE`を付け、15秒無通信で5秒おきに3回プローブを送り、応答がなければスタックが接続を落とす(`TCP_KEEPIDLE`、`TCP_KEEPINTVL`、`TCP_KEEPCNT`)。行を待つ間と`put`の受信中に効く。sshdの`ClientAliveInterval`に当たる |
| 送信の進み | 送信は`MSG_DONTWAIT`で送れるだけ送り、送信バッファの空きを`poll`で待つ。30秒空きが出なければ、相手が受け取っていないとして接続をあきらめる(LinuxのTCP_USER_TIMEOUTに当たる)。スタックの再送の打ち切り(数分)は待たない。あきらめた接続にはTLSの終了の通知を送らず、`SO_LINGER`を0にしてRSTで閉じる |
| 自分のアドレスの変化 | 行を待つ間、1秒ごとに接続の自分側のアドレスとインタフェースのアドレスを比べ、別のアドレスになっていれば(別のネットワークにつないだ)セッションを終える。アドレスがないだけのときは、同じアドレスに戻ることもあるのでkeepaliveに任せる |
| 新しい接続との入れ替え | 行を待つ間はlistenのソケットも監視し、新しい接続が来たらその場で受け付けて、ハンドシェイクをする。鍵が合ってハンドシェイクが終わってから、無通信の古いセッションをRSTで閉じ、新しい方に入れ替える。鍵のない相手は古いセッションを切れない。コマンドの処理中は入れ替えない |

待ち行列から受け付けた接続が、読む前にすでに相手から閉じられていたとき(FINやRSTが届いている)は、ハンドシェイクをせずに閉じる。ハンドシェイクの途中で相手が閉じたとき(`-0x7280`)も同じで、どちらも失敗の後の1秒の待ちを置かない。この待ちは鍵が合わなかったときのためのもので、PCがあきらめた接続が並んでいても、その後ろで待つ接続をPCの待ち時間を越えて待たせない。listenのbacklogは1のままで、入れ替えがあるので待ち行列は長くならない。

次はネットワークのインタフェースだけのコマンドである。

| コマンド | 応答 |
| --- | --- |
| `hello` | `ok tessronos 1 幅 高さ` |
| `shot` | `ok 幅 高さ バイト数`の後にRGBの画素(ポインタを含む)。PC側でPNGにする |
| `log 位置 [最大]` | `ok N 次の位置 終わり`の後にNバイト。コンソールのログ(`tm_log_read`)で、`KERNEL.TXT`と同じ起動からの内容を読む。`TESTAGENT=1`では保持する量を1MBに増やす |
| `put バイト数` | 行の後にそのバイト数を送る。レコード1に持つ実身を作り、`ok UUID`。4MBずつ書く。失敗すると作りかけの実身を消して`err put エラーコード`を返す。指定したバイト数が60秒届かないで終わったときは、その後の接続の中身を行と区別できないので、応答してから接続を閉じる。終わりにコンソールへ、かかった時間と書き込みの時間、その間のTCPの受信の数(順番どおり、重複、順番の飛び)を出す |
| `rmobj UUID` | その実身を消す |
| `reboot`、`poweroff` | `ok`の後、ウインドウを保存してセッションを終える |

インストールは`put`の後に、アプリケーションは`proginst`(`proginstf`はバージョンによらず置き換える)、システムは`sysup`で行う。`sysup`はデスクトップのループが1段階ずつ進める。

使い方は次のとおりである。Python 3.13以降が必要である(`ssl`のPSK)。

```text
python tools/mcp/tessron_mcp.py --new-target surface --host 192.168.1.50
  (表示された testagent=... をテスト用イメージの起動の設定に足す)
make TARGET=_X64_ DESKTOP=1 TESTAGENT=1 BUILD_ID=ta sdimg
claude mcp add tessronos-test -- python C:\home\TessronOS\tools\mcp\tessron_mcp.py
```

起動の設定は、x64ではESPの`cmdline.txt`、Raspberry Pi 5では起動のパーティションの`cmdline.txt`(`make RPI_CMDLINE=testagent=<鍵>`でイメージに入る。システムの更新では置き換えない)、Compute Module 5では`config.txt`の`cmdline=`が指す`cmdline-cm5.txt`である。Raspberry Piのファームウェアは起動時に`cmdline.txt`の内容をシリアルに出すので、COM3のログには鍵が残る。接続先のアドレスは`targets.json`に書く。DHCPで変わるときは、ルータで固定するか、デスクトップのメッセージ行で確認する。

MCPのツールは`targets`、`connect`、`disconnect`、`send_steps`、`step`、`screenshot`(MCPの画像で返す)、`log`、`key`、`pointer`、`win_list`、`win`、`focus`、`reboot`、`power_off`、`wait_up`、`install_tpk`、`serial_log`である。`.steps`の`hmp`はQEMUのモニタのコマンドなので、実機では失敗する。

自動化できる範囲には次の限界がある。

- OSとネットワークが動いていないと何もできない。電源が切れたマシン、フリーズしたマシン、起動の途中で止まったマシンは、ソフトウェアでは起動できない。電源を入れ直すのは人の手である。`power_off`の後も同じである
- 無線のネットワークは、保存済みのWi-Fiの設定で起動時に接続できることが前提である。接続できなければポートには届かない。USBのLANのほうが確実である
- SDやESPの書き換え、`cmdline.txt`の編集、ファームウェアの設定は人の手である。システムの更新(`install_tpk`)は動くシステムの上でしかできないので、更新で起動しなくなったら人の手で戻す
- `reboot`はデスクトップが応答しているときだけ働く。デスクトップがフリーズすると`reboot`は効かない
- 起動が止まったときのログは、ネットワークでは読めない。x64は従来どおりUSBメモリの`KERNEL.TXT`を見る。Raspberry Pi 5はDebug Probe(COM3)のシリアルコンソールを`serial_log`で読める。読むだけで、コマンドは送らない。TeraTermと同時には開けない

システムの更新には、システムのボリュームに`.tpk`を受け取る空き容量が必要である。x64の既定の`SYS_VOL_SIZE=auto`のイメージでは、117MBの`.tpk`の`put`が`err put -33`で失敗した。`SYS_VOL_SIZE=600M`のイメージでは通ったので、空き容量の不足とみている。

2026-11-02に確認した範囲は次の表のとおりで、すべてQEMUである。実機では2026-11-03にRaspberry Pi 5で、Wi-Fi越しの接続、画面、`put`、`sysup`、`reboot`、更新後の鍵の引き継ぎを確認した。439MBの`put`はチップのRAMの大きさを読み違えていたバージョンで1565秒、修正したバージョンで267〜630秒だった。Wi-Fiの受信が数十秒から数十分止まることがあり、その間は送る側のTCPが再送を重ねる(9.6)。Surface Go 2、X1 Yoga 第3世代、X1 Carbon、USBのLAN、`serial_log`は未確認である。

| 項目 | x64(q35) | ARM(virt) |
| --- | --- | --- |
| MCPの`initialize`、`tools/list`、`connect`、`screenshot`(画像)、`log`、`key`、`pointer`、`win_list`、`step`、`send_steps` | 確認 | 確認 |
| 平文のTCP、違う鍵の拒否。PC側の短いトークン、トークンのファイルなし | 確認 | 確認 |
| 鍵がログとコンソールに出ない | 確認 | 確認 |
| `install_tpk`のアプリケーション(`put`、`proginst`、`proginstf`、`rmobj`) | 確認 | 確認 |
| `install_tpk`のシステム(`sysup`)と、その後の再起動、鍵の引き継ぎ | 確認(ゲストの中のTCG、9分強) | 未確認 |
| `reboot`、`wait_up`、`power_off` | 確認(TCG) | 確認 |
| `testagent=`なしでポートを開かない | 確認(ログと、応答なし) | 未確認 |
| メッセージ行の表示と出し直し | 確認 | 未確認 |

既定のビルド(x64の`DESKTOP=1`)のカーネルには、`ta_tls_`、`dt_tagent`、`mbedtls_ssl_`のシンボルと`testagent`の文字列がないことを確認した。ARMとリリースの構成では確認していない。x64をWHPXで動かすと、`reboot`の後にQEMUのプロセスが終わる。TCGでは再起動するので、OSの問題ではなくWHPXのリセットの問題とみているが、原因は確定していない。

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
| `tools/mkdisk.py`、`tools/mksd.py`、`tools/mkfat.py` | テスト用ディスク、SDカードのイメージ、FATのボリュームを作成する。テスト用ディスクの大きさ(`TEST_DISK_MB`)は最小の値で、システムボリュームが大きく、FAT32のボリューム(65525クラスタ以上)と載せるファイルが入らないときは、ディスクを大きくして作る |
| `tools/btbackup.py` | BTRONのBACKUP書庫を読み書きする |
| `tools/ftpd.py` | ネットワークのテストに使うFTPサーバ |
| `tools/ui_run.py` | UIのテストを、起動済みの状態から数秒で実行する(12.4) |
| `tools/mktpk.py` | アプリケーションのパッケージ(.tpk)を作成する。`--system`でシステムのパッケージを作る(11.8.1)。`--system --manifest <ディレクトリ>`で、システムボリュームが持つシステムの実身の一覧(実身「システム」のレコード1)を同じ集め方で作る。`--list`で内容を表示する |
| `tools/pkgstage.py` | イメージに組み込むパッケージを展開し、そのプログラムへの仮身を足したプログラム箱を作る(11.8) |
| `tools/netbsd/build.sh` | NetBSDのTCP/IPスタックのソースを取得し、rumpカーネルの部品として1つのオブジェクトにビルドする(12.7)。`WIFI=1`は、Raspberry Pi 5のWi-Fi(SDIOのホストのsdhcとsdmmc、bwfm、net80211)を足したものを、別のソースの置き場`~/netbsd/netbsd-11.0-wifi`から`~/tessronos-netbsd-wifi/rumpnet.o`に作る。Pi 5のビルド(既定の`WIFI=1`、`build_make/wifi_arm.mk`)はこれをリンクし、なければmakeが作る(09章9.6.8) |
| `tools/rpi5/wifi/fw.sh` | Raspberry Pi 5とPi 4系のWi-Fiのチップのファームウェア、CLM、基板の設定(NVRAM)とライセンスを、linux-firmware 20260810から取得してSHA-256を検証し、`~/tessronos-rpi5-wifi-fw`に置く。`mkobj.py`がファームウェア、CLM、基板の設定の文章の実身を作り、`nvram.py`は文章から設定を取り出す規則(カーネルと同じ)、`test_nvram.py`はそのテスト(09章9.6.8) |
| `tools/x64/ktest64.sh`、`tools/x64/ui_run64.py` | x64のカーネル内のテストとUIのテストを、WindowsのQEMUの`q35`で実行する(12.3、12.4) |
| `tools/x64/acpica/build.sh` | ACPIのインタプリタACPICAのリリースを取得し、OS依存のレイヤと合わせてカーネルにリンクするオブジェクト`~/tessronos-acpica-x64/acpica.o`を作る(3.10.6)。x64のカーネルのビルドは、これがなければ先に作る |
| `tools/wpa/build.sh` | wpa_supplicantのWPA2の鍵交換の部分を、NetBSDの`src.tgz`から取り出してカーネル向けにビルドし、`~/tessronos-wpa-x64/wpasupp.o`にする(`ARCH=aarch64`はRaspberry Pi 5の`~/tessronos-wpa-arm/wpasupp.o`)。`test`はホストでhostapdの認証側と鍵交換をテストする(09章9.6.7) |
| `tools/x64/wifi/fw.sh`、`mkobj.py` | Wi-Fiのファームウェアをlinux-firmwareから取得してSHA-256を検証し、システムボリュームに載せる実身(ファームウェア、Wi-Fiの設定、それらへの仮身を持つ実身「システム」)を作る(09章9.6.5) |
| `tools/x64/netbsd/build.sh`、`tools/x64/mozc/build.sh`、`tools/x64/mesa/build.sh` | NetBSDのスタック、Mozc、Mesaのx86_64用のもの(`~/tessronos-netbsd-x64`、`~/tessronos-mozc-x64`、`~/tsmesa-x64`)を作る。手順はそれぞれ`tools/netbsd`、`tools/mozc`、`tools/mesa`のものと同じで、ツールチェーンと命令セットの基準だけが違う |
| `tools/chk_isa.sh` | オブジェクトやイメージに、ARMv8.0-AにないLSEのアトミック命令や暗号拡張の命令が残っていないかを調べる(3.6.2) |
| `tools/mesa/probe.sh` | Mesaを取得し、その一部をプロセス用のツールチェーンで試しにコンパイルする。`pack`はV3Dのパケットを詰めるヘッダを生成してリポジトリに書く(13.6.4) |

## 12.7 NetBSDのスタックのビルド

TCP/IPスタック(第9章)は、NetBSD 11.0のカーネルのソースからビルドする。ソースはリポジトリに含めず、`tools/netbsd/build.sh`が次の順に処理する。

| 段階 | 処理 |
| --- | --- |
| `fetch` | NetBSDの配布物`syssrc.tgz`を取得してSHA512を検証し、スタックのビルドに使うディレクトリだけを`~/netbsd/netbsd-11.0`(`NBSRC`で変える)に展開する。展開したソースは約27MB、2074ファイルである。取得先は通常の配布サーバ、アーカイブ用のサーバ、ftp.netbsd.orgの順に試し、途中で切れた転送は続きから取り直す。どこからも全体を取得できないときは、展開するファイルだけをまとめた`netbsd-11.0-rumpsrc.tgz`(`tools/netbsd/`にあるもの、なければTessronOSのリリースの添付ファイル)を、そのチェックサムを検証して展開する(UPSTREAM.md) |
| `gen` | NetBSDのmakefileとconfig(1)が生成するもの(`vers.c`、ルートバスの`ioconf.c`、疑似デバイスの`ioconf.h`)と、`<machine/...>`のリンクを`~/tessronos-netbsd/gen`に作る |
| `compile` | rumpカーネルの基盤(librump)、ソケット層(librumpnet)、インタフェースと経路とIPv4、IPv6(librumpnet_net)と、TessronOSの部品(`peripheral_kernel/network/netbsd/rumpcomp/`)を、カーネルと同じツールチェーンで浮動小数点のレジスタを使わずにコンパイルする。言語の規格はNetBSD 11.0のカーネルと同じC11(`-std=gnu11`)である |
| `link` | rumpカーネルの規則どおり、NetBSDのシンボルに接頭辞`rumpns_`を付けてから1つのオブジェクトにまとめ、`rump_`で始まるエントリポイント以外をローカルにする。まとめるときに、NetBSDの`ip_savecontrol`の呼出しをTessronOSの部品の`__wrap_rumpns_ip_savecontrol`に向ける(`--wrap`。受け取ったIPv4のデータグラムのTOSを付随データに足す、9.2)。リンクセットの範囲もこのオブジェクトの中で決める。結果は`~/tessronos-netbsd/rumpnet.o`(`OUT`で変える)で、外から必要とするのはハイパーコール(`rumpuser_`、`rumpcomp_`)だけである |
| `notices` | コンパイラが読んだNetBSDのソースとヘッダ(依存関係のファイル`*.o.d`に載るもの)の著作権表示とライセンス条件を、重複を除いて`~/tessronos-netbsd/NOTICE-netbsd.txt`にまとめる(`THIRD_PARTY_NOTICES.md`) |

引数を付けなければすべての段階を実行する。ソースを取得するときだけネットワークを使う。命令セットの基準は`MARCH`で変える(既定は`armv8.2-a`)。Raspberry Pi 4系向けのビルドは`MARCH=armv8-a+crc OUT=~/tessronos-netbsd-v80`で作る(3.6.2)。コンパイルは12並列で10秒ほどで終わる。makefileは、このスクリプトか`rumpcomp/`のファイルが新しくなると、リンクの前にスクリプトを実行し直す。

コンパイルするファイルの一覧はスクリプトに書いてあり、NetBSDのmakefileの`SRCS`をコピーしたものである。NetBSDのバージョンを上げるときは、スクリプトの`NB_VER`と`NB_SHA512`を変え、新しいバージョンのmakefileと突き合わせて一覧を修正する。展開するディレクトリの一覧(`EXTRACT_DIRS`)は、コンパイルで生成される依存関係のファイル(`*.o.d`)から求められる。展開したファイルをまとめ直したものを新しいバージョンのリリースに添付し、そのチェックサムを`NB_SUB_SHA512`に書く。リンクと著作権表示の一覧には、一覧にあるソースのオブジェクトだけを使う。前のバージョンで作った置き場で作り直しても、新しいバージョンでなくなったソースのオブジェクトは取り込まない。
