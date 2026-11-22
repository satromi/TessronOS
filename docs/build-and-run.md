# ビルドと実行の手順

TessronOSをソースからビルドし、Raspberry Pi 5とRaspberry Pi Compute Module 5(以下CM5)で起動する手順と、WindowsのQEMUで試す手順を説明する。設計の詳細は[TessronOS設計書](tessronos.md)の第12章(開発と試験)、第14章(ブラウザ)にある。

## 1. 必要なもの

| 用途 | 必要なもの |
| --- | --- |
| ビルド | Windows 11とWSL2のUbuntu(24.04で確認)。ディスクの空きは、TessronOSだけなら5GB、ブラウザも作るなら100GB以上。最初のビルドで、NetBSDのネットワークスタックのソース(約80MBの配布物)をインターネットから取得する |
| ブラウザのビルド | WSLに割り当てるメモリ16GB以上(24GBを推奨)。Chromiumのチェックアウト(約22GB) |
| Windowsで試す | QEMU for Windows(11.0で確認)、Python 3 for Windows(ネットワークの試験とUIの試験に使う) |
| 実機で動かす | Raspberry Pi 5(4GB以上)、またはCM5(4GB以上)とCM5 IO Board、microSDカード(1GB以上、ブラウザ入りなら2GB以上。eMMC付きのCM5では要らない)、Raspberry Pi Imager、HDMIのディスプレイ、USBのキーボードとマウス。eMMCに書き込むにはrpibootとUSB-Cのケーブル |
| 実機のコンソール | 3.3VのUSBシリアル変換(Raspberry Pi Debug Probeなど)。なくても動かせるが、起動の記録とエラーはシリアルにだけ出る |

リポジトリは次の2つで、同じディレクトリに置く。ブラウザを作らないなら`tessron-chromium`は要らない。

| リポジトリ | 置き場の例 | 内容 |
| --- | --- | --- |
| TessronOS | `C:\home\TessronOS`(WSLでは`/mnt/c/home/TessronOS`) | OS本体 |
| tessron-chromium | `C:\home\tessron-chromium` | ブラウザ(Chromiumの移植) |

## 2. 開発環境の用意

### 2.1 WSL2

PowerShellを管理者で開き、Ubuntuを入れる。

```powershell
wsl --install -d Ubuntu-24.04
```

ブラウザを作るときは、`C:\Users\<ユーザ名>\.wslconfig`でWSLのメモリを増やし、`wsl --shutdown`で再起動する。

```ini
[wsl2]
memory=24GB
processors=20
swap=16GB
```

### 2.2 ツールチェーンとQEMU

WSLのUbuntuで、補助のパッケージを入れてから、リポジトリの`tools/setup_wsl.sh`を実行する。xPackのGCC(aarch64-none-elf-gcc 15.2.1-1.1)とQEMU(9.2.4-1)が`~/.local/xPacks`に入る。root権限は要らない。

```sh
sudo apt install -y build-essential git curl python3 device-tree-compiler gdb-multiarch
cd /mnt/c/home/TessronOS
./tools/setup_wsl.sh
source tools/env.sh
```

`source tools/env.sh`は、ビルドする端末ごとに一度実行する(`~/.bashrc`に書いてもよい)。`aarch64-none-elf-gcc --version`で15.2.1と出れば準備ができている。

### 2.3 Windows側

- QEMU for Windowsを<https://qemu.weilnetz.de/w64/>から入手し、既定の`C:\Program Files\qemu`にインストールする。
- Python 3 for Windowsを<https://www.python.org/>から入手し、インストールのときに「Add python.exe to PATH」を選ぶ。WSLから`python.exe --version`で呼べることを確かめる。
- Raspberry Pi Imagerを<https://www.raspberrypi.com/software/>から入手してインストールする。

## 3. ビルド

makefileは`build_make/`にある。以下のコマンドはWSLで`build_make`に移動して実行する。生成物は`build_make/`の下にできる。

```sh
cd /mnt/c/home/TessronOS/build_make
```

### 3.1 主なmakeの変数

| 変数 | 意味 |
| --- | --- |
| `TARGET=_QEMU_VIRT_`、`TARGET=_RPI5_` | QEMU用、Raspberry Pi 5とCM5用。必須 |
| `DESKTOP=1` | デスクトップが起動する構成 |
| `KTEST=1` | カーネル内の試験を組み込む。試験用ディスクもこのときに作る |
| `MOZC=1` | かな漢字変換にMozcの変換エンジンと辞書を入れる(3.4) |
| `NETSTACK=lwip` | TCP/IPスタックを以前のlwIPにする。指定しなければNetBSDのスタックになる(3.7) |
| `BROWSER=1 BROWSER_BLINK=1` | ブラウザを入れる(3.5) |
| `GL=1` | OpenGL ES(Mesa)をビルドし、それを使う小物「歯車」と試験のプログラムを入れる。Mesaは`tools/mesa/build.sh`が`~/mesa`に取得し、`~/tsmesa`に静的ライブラリを作る。WSLにmeson(1.4以降)、ninja、Pythonのmakoとyamlが要る(第13章13.6.5) |
| `V3D=2` | Raspberry Pi 5のGPUを使えるようにし、起動の後でGPUの試験をまとめて行う(第13章) |
| `BUILD_ID=<名前>` | 生成物の置き場を分ける。変数の組合せを変えるときは、別の名前にする |
| `STKWATCH=1` | 走っていないタスクのスタックの戻りアドレスが書き換えられたら、ディスパッチャがそのタスクを走らせる前に見つけて止める。TCBが大きくなるので、試験で調べるときだけ使う(設計書4.7.6) |
| `KT_RUMP_S=<秒>` | 試験グループ`rump`がNetBSDのスタックに負荷を掛ける時間。既定は20秒(設計書9.1.2) |
| `KT_CHURN_N=<回数>` | プロセスやタスクを作っては終える試験(`proc`、`el0`、`prcobj`の群)の回数。既定は1000回(設計書4.6.2、5.5) |
| `-j<数>` | 並列にコンパイルする数。CPUのコア数にするとよい |

`BUILD_ID`を付けると、カーネルは`tessronos<名前>.elf`、中間ファイルとディスクのイメージは`obj/<TARGET><名前>/`にできる(`KTEST=1`のときは`obj/<TARGET>_ktest<名前>/`)。

### 3.2 QEMU用のカーネルと試験

```sh
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 test                          # 全部の試験(30分ほど)
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=wm,ob BUILD_ID=wo test # 試験グループを選ぶ
make TARGET=_QEMU_VIRT_ qemu                                       # 起動して、コンソールを端末に出す(Ctrl-A Xで終了)
```

試験は1件ごとに`KTEST <名前> PASS`か`FAIL`を出力し、最後に`KTEST SUMMARY <成功数>/<総数>`を出力する。試験グループの一覧は第12章にある。

`SD_EMMC=1`を付けると、試験用のSDカードのイメージをSDカードではなくeMMCとしてQEMUに挿し、CM5のeMMCと同じ初期化で`sd`の試験グループを通す(`make -j20 TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=sd SD_EMMC=1 BUILD_ID=em test`)。xPackのQEMU 9.2ではeMMCを挿せないので、このときはWindowsのQEMU(2.3)を使う。

### 3.3 デスクトップ

```sh
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 BUILD_ID=desk               # tessronosdesk.elf
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 obj/_QEMU_VIRT__ktest/test_disk.img   # デスクトップが使うディスク
```

デスクトップのディスクは、試験用ディスクと同じものである。システムボリューム(システムの実身)と、プログラムや試験のファイルを置いたFATの区画を持つ。起動は5章のとおり。

小物「文字検索」の辞書(漢字のよみ、部品、関連字)は、公開されているデータからビルドのときに作る。データは一度だけ次のように`~/chardic`に取得しておく(約14MB)。取得していなくてもビルドはでき、そのときの辞書はよみと部品を持たない(設計書11.19)。

```sh
python3 tools/gen_chardic.py fetch ~/chardic    # TessronOSの最上位で実行する
```

### 3.4 Mozc(かな漢字変換)

`MOZC=1`を付けないと、かな漢字変換はローマ字からかなへの変換だけになる。Mozcを入れるには、先に変換エンジンを1つのオブジェクトファイルにビルドしておく。

1. Mozcのソース(`~/mozc/src`)に、BTRON版の変換サーバのアダプタ(`src/btron`)を加えたものを用意し、ホストでビルドする。生成されるソース(protobufの出力)と辞書(`bazel-bin/data_manager/oss/mozc.data`)を使う。

   ```sh
   cd ~/mozc/src && bazelisk build --config oss_linux //btron:mozc_kserver
   ```

2. TessronOSのツールチェーンでビルドし直す。`~/tessronos-mozc/tsmozc.o`(約38MB)ができる。

   ```sh
   cd /mnt/c/home/TessronOS/tools/mozc
   JOBS=8 bash build.sh all
   ```

3. makeに`MOZC=1`を付ける。変換エンジンがカーネルにリンクされ、辞書は辞書の実身のレコード1としてディスクに入る。

### 3.5 ブラウザ

ブラウザは`tessron-chromium`のリポジトリにある。Chromium本体はどちらのリポジトリにも含めず、WSLのホームのChromiumのチェックアウトをTessronOSのツールチェーンでコンパイルし直す(第14章)。最初から全部をコンパイルすると、20コア・メモリ24GBで3時間ほどかかる。2回目からは、変わったファイルだけをコンパイルし直す。

前提として、次のものがWSLのホームに要る。場所は環境変数で変えられる。

| もの | 既定の場所 | 環境変数 |
| --- | --- | --- |
| Chromium 153.0.8010.36のチェックアウト(depot_toolsで取得し、DEPSの依存を含む) | `~/cr6/src` | `CR` |
| そのチェックアウトに合うgnとninja | `~/btron-upgrade/gnpkg153/` | `GN`、`NINJA` |
| ホストのライブラリの代わりのpkg-configの記述 | `~/btron-upgrade/pkgstub` | `PKGSTUB` |
| コンパイルし直した生成物の置き場 | `~/tessronos-obj` | `OBJ` |

手順は次のとおりである。`gngen`と`hostgen`は最初に一度だけ実行する。

```sh
cd /mnt/c/home/tessron-chromium/tools/browser
bash build.sh gngen                          # aarch64のビルドの記述(~/cr6/src/out/tessronos)
bash build.sh hostgen                        # ホストで作るもの(V8のスナップショット、ICUのデータなど)
JOBS=12 bash build.sh v8                     # V8(1ファイル1.5GB前後のメモリを使う)
JOBS=10 bash blink.sh all                    # Blinkとその依存(約16,000ファイル)
bash rust.sh build && bash rust.sh merge     # Rustで書かれた部分
cd /mnt/c/home/TessronOS/build_make
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 BUILD_ID=deskbr
```

並列の数は、メモリが足りる範囲で決める。WSLのメモリが24GBなら、V8は12、Blinkは10までにする。メモリが足りなくなるとスワップを使い始め、かえって遅くなる。

`tessron-chromium`を別の場所に置いたときは、makeに`CHROMIUM=<場所>`を、スクリプトを直接実行するときは`TESSRONOS=<TessronOSのリポジトリ>`を与える。

### 3.6 Raspberry Pi 5とCM5のイメージ

```sh
make -j20 TARGET=_RPI5_ DESKTOP=1 BUILD_ID=pi sdimg
```

`build_make/obj/_RPI5_pi/sd.img`ができる。区画1(FAT32、256MB)にファームウェアへの指示`config.txt`と`cmdline-cm5.txt`、デバイスツリー、カーネル`tessronos.img`、プログラムを、区画2にシステムボリューム(TSFS)を置く。デバイスツリー(Raspberry Pi 5の`bcm2712-rpi-5-b.dtb`、CM5の`bcm2712-rpi-cm5-cm5io.dtb`、CM5 Liteの`bcm2712-rpi-cm5l-cm5io.dtb`)は最初に一度だけRaspberry Piのファームウェアの配布物から取得し、`build_make/obj/boot/`に置く。

同じイメージがRaspberry Pi 5、CM5、CM5 Liteのどれでも起動する。ファームウェアが`config.txt`の`[cm5]`と`[board-type=0x1a]`の条件でボードに合ったデバイスツリーを選び、カーネルは起動時にボードを見分ける(設計書3.5)。Raspberry Pi 5ではSDカードに、CM5ではeMMCに(4.4)、CM5 LiteではIOボードのmicroSDカードに書き込む。

よく使う組合せは次のとおりである。

| 目的 | コマンド | イメージの大きさ |
| --- | --- | --- |
| デスクトップ | `make TARGET=_RPI5_ DESKTOP=1 BUILD_ID=pi sdimg` | 約330MB |
| デスクトップ、Mozc | `make TARGET=_RPI5_ DESKTOP=1 MOZC=1 BUILD_ID=pim sdimg` | 約350MB |
| デスクトップ、Mozc、ブラウザ、GPUの試験 | `make TARGET=_RPI5_ DESKTOP=1 MOZC=1 BROWSER=1 BROWSER_BLINK=1 V3D=2 BUILD_ID=pibr sdimg` | 約600MB |
| カーネル内の試験 | `make TARGET=_RPI5_ KTEST=1 BUILD_ID=pit sdimg` | 約330MB |

すでに書き込んだSDカードのカーネルだけを差し替えるときは、SDカードの区画1をWSLから見える場所にマウントして、`make TARGET=_RPI5_ sd SD=/mnt/e`とする(`tessronos.img`、`config.txt`、`cmdline-cm5.txt`、3つのデバイスツリーを上書きする)。

### 3.7 NetBSDのネットワークスタック

TCP/IPスタックは、デフォルトではNetBSD 11.0のカーネルのものである。ソースはリポジトリに含めず、最初のビルドでmakefileが`tools/netbsd/build.sh`を実行し、NetBSDの配布物を取得して、スタックに必要な部分だけを`~/netbsd/netbsd-11.0`に展開し、`~/tessronos-netbsd/rumpnet.o`にビルドする。取得には`curl`を使い、SHA512で配布物を確かめる。前もって、または作り直すときは、次のように実行する。

```sh
cd /mnt/c/home/TessronOS
bash tools/netbsd/build.sh all        # 取得、生成、コンパイル、リンク、著作権表示の一覧
bash tools/netbsd/build.sh clean      # 生成物を消す(ソースは残る)
```

ソースの置き場は`NBSRC=<ディレクトリ>`、生成物の置き場は`OUT=<ディレクトリ>`で変えられる。makeで`NETBSD_OBJ=<ファイル>`を指定すると、そのオブジェクトをリンクする。イメージを配布するときは、生成された`~/tessronos-netbsd/NOTICE-netbsd.txt`(NetBSDの著作権表示とライセンス条件の一覧)を添付する(`THIRD_PARTY_NOTICES.md`)。

### 3.8 Raspberry Pi 4、400、CM4のイメージ

Raspberry Pi 4 Model B、Raspberry Pi 400、Compute Module 4(CM4)は、CPUがARMv8.0-AのCortex-A72なので、Raspberry Pi 5とは別のカーネルのイメージを`TARGET=_RPI4_`で作る(設計書3.6)。NetBSDのスタックとMozcもARMv8.0-Aで別に作る。NetBSDのスタックは、なければmakeが作る。Mozcは前もって次のように作る(3.4の`~/mozc/src`のホストのビルドが済んでいること)。

```sh
cd /mnt/c/home/TessronOS
MARCH=armv8-a+crc OUT=~/tessronos-mozc-v80 JOBS=12 bash tools/mozc/build.sh all
MARCH=armv8-a+crc OUT=~/tessronos-netbsd-v80 bash tools/netbsd/build.sh all   # makeに任せてもよい
bash tools/chk_isa.sh ~/tessronos-mozc-v80/tsmozc.o ~/tessronos-netbsd-v80/rumpnet.o
```

`tools/chk_isa.sh`は、Cortex-A72にない命令(LSEのアトミック命令など)が残っていないかを調べる。見つからなければ何も出力しない(libgccのアトミック操作の関数は両方の形を持つので別に数える)。イメージは次のように作る。

```sh
make -j12 TARGET=_RPI4_ DESKTOP=1 MOZC=1 BUILD_ID=pi4 sdimg
```

`build_make/obj/_RPI4_pi4/sd.img`ができる。区画1にはRaspberry Pi 5のイメージと同じもののほか、Raspberry Pi 4系のファームウェア(`start4.elf`、`fixup4.dat`)、3つのデバイスツリー(`bcm2711-rpi-4-b.dtb`、`bcm2711-rpi-400.dtb`、`bcm2711-rpi-cm4.dtb`)、`overlays/disable-bt.dtbo`を置く。これらは最初に一度だけRaspberry Piのファームウェアの配布物から`build_make/obj/boot/`に取得する。同じイメージがRaspberry Pi 4 Model B、400、CM4のどれでも起動する。ブラウザ(`BROWSER=1`)は組み込めない。プログラムと実身のレコードは、中間の生成物の置き場の`v80/`(`~/tessronos-obj-ts/<ツリー>/user_<BUILD_ID>/v80/`、12章12.2.1)にできる。

| 目的 | コマンド |
| --- | --- |
| デスクトップ、Mozc | `make TARGET=_RPI4_ DESKTOP=1 MOZC=1 BUILD_ID=pi4 sdimg` |
| カーネル内の試験 | `make TARGET=_RPI4_ KTEST=1 BUILD_ID=pi4t sdimg` |
| QEMUの`raspi4b`で起動する | `make TARGET=_RPI4_ DESKTOP=1 MOZC=1 BUILD_ID=pi4 qemu-rpi4` |
| QEMUの`raspi4b`でカーネル内の試験 | `make TARGET=_RPI4_ KTEST=1 KTGROUP=kernel BUILD_ID=pi4t test-rpi4` |

すでに書き込んだSDカードのカーネルだけを差し替えるときは`make TARGET=_RPI4_ sd SD=/mnt/e`とする(`tessronos.img`、`config.txt`、ファームウェア、デバイスツリー、`overlays/disable-bt.dtbo`を上書きする)。

## 4. Raspberry Piで起動する

### 4.1 Raspberry Pi Imagerで書き込む

1. WSLで作った`sd.img`は、Windowsからは`C:\home\TessronOS\build_make\obj\_RPI5_pi\sd.img`のように見える(Raspberry Pi 4系のものは`obj\_RPI4_pi4\sd.img`)。
2. microSDカードをパソコンに挿し、Raspberry Pi Imagerを起動する。
3. 「Raspberry Piデバイス」で「Raspberry Pi 5」を選ぶ。
4. 「OS」で一覧のいちばん下の「カスタムイメージを使う」(Use custom)を選び、`sd.img`を指定する。
5. 「ストレージ」で書き込むmicroSDカードを選ぶ。ほかのディスクを選ばないよう、大きさを確かめる。
6. 「次へ」を押す。OSのカスタマイズを聞かれたら「いいえ」を選ぶ(TessronOSはこの設定を使わない)。
7. カードの内容を消してよいかを聞かれるので「はい」を選ぶ。書き込みと検証が終わるまで待つ。
8. 書き込みの後、Windowsが「ドライブ X: を使うにはフォーマットする必要があります」と表示することがある。これはWindowsが読めないTSFSの区画についての表示なので、必ず「キャンセル」を選ぶ。フォーマットすると、システムボリュームが消える。
9. カードを取り出す。

### 4.2 つなぐもの

| 端子 | つなぐもの |
| --- | --- |
| microSDスロット | 書き込んだカード |
| HDMI 0(USB-C端子に近い側) | ディスプレイ |
| USB | キーボードとマウス(USBメモリ、USBオーディオも使える) |
| Ethernet | ネットワーク(DHCPでアドレスを取得する) |
| UART(2つのHDMI端子の間の3ピン) | シリアルコンソール(4.3) |
| USB-C | 電源(5V 5Aを推奨) |

電源を入れると、ファームウェアが`config.txt`を読んでカーネルを起動し、デスクトップが表示される。

### 4.3 シリアルコンソール

起動の記録、エラー、試験の結果は、シリアルコンソールに出力する。3ピンのデバッグ用UARTに、Raspberry Pi Debug Probeなどの3.3VのUSBシリアル変換をつなぎ、Windowsの端末ソフト(Tera Term、PuTTYなど)で次の設定で開く。

| 項目 | 値 |
| --- | --- |
| 速度 | 115200bps |
| データ | 8bit、パリティなし、ストップビット1 |
| フロー制御 | なし |

40ピンヘッダのUART(GPIO14、15。8番がTXD、10番がRXD)に出したいときは、`RP1_CONSOLE=1`を付けてビルドする。ビルドし直さずに、区画1に`cmdline.txt`を作って`tessronos.console=rp1`と書き、`config.txt`の最後に`enable_rp1_uart=1`を足してもよい。この2つは必ず組にする。`enable_rp1_uart=1`がないとファームウェアがそのUARTにクロックを入れず、カーネルが触れたときに止まる。

### 4.4 CM5で起動する

CM5はCM5 IO Board(CM5IO)に挿して使う。イメージはRaspberry Pi 5と同じもの(3.6)である。

eMMC付きのCM5には、rpibootでeMMCをUSBの大容量記憶装置として見せてから書き込む。

1. CM5IOのジャンパJ2の`nRPI_BOOT`(Disable eMMC Boot)にジャンパを付ける。
2. CM5IOのUSB-C端子(USB 2.0。書込みとUSBデバイス用)をパソコンにつなぎ、CM5IOに電源を入れる。
3. Windowsではrpibootのインストーラ(<https://github.com/raspberrypi/usbboot/releases>)を入れ、スタートメニューの「rpiboot - Mass Storage Gadget」を実行する。Linuxでは`sudo rpiboot`(Ubuntu 24.04までのパッケージのバージョンは動かないことがあるので、そのときはソースからビルドする)。数秒でeMMCがディスクとして見える。
4. Raspberry Pi Imagerで4.1と同じく「カスタムイメージを使う」を選び、`sd.img`をそのディスクに書き込む。「Raspberry Piデバイス」は「Raspberry Pi 5」のままでよい。
5. 書込みが終わったら電源を切り、J2のジャンパを外し、USB-C端子のケーブルを外して電源を入れ直す。eMMCから起動する。

CM5 Lite(eMMCなし)では、4.1と同じ手順でmicroSDカードに書き込み、CM5IOのmicroSDスロットに挿す。eMMC付きのCM5はこのスロットを使わない。

CM5IOでつなぐものは次のとおりである。

| 端子 | つなぐもの |
| --- | --- |
| HDMI 0 | ディスプレイ |
| USB 3.0(Type-A、2つ) | キーボードとマウス(USBメモリ、USBオーディオも使える) |
| Ethernet | ネットワーク(DHCPでアドレスを取得する) |
| 40ピンヘッダの8番(TXD)、10番(RXD)、6番(GND) | シリアルコンソール。3.3VのUSBシリアル変換をつなぐ |
| USB-C(電源) | 電源(5V 5Aを推奨) |

CM5IOにはデバッグ用UARTの3ピンの端子がないので、`config.txt`の`[cm5]`の節と`cmdline-cm5.txt`で、コンソールを40ピンヘッダのUART0に移してある。設定は4.3と同じ115200bpsである。コンソールが移るのはRP1の初期化の後なので、それより前の起動の記録(ファームウェアの記録とカーネルの最初の数十行)は、CM5のモジュールのデバッグ用UART(コネクタを付ける場所とテストポイント)にだけ出る。起動の記録の`TessronOS: board`の行に、見分けたボードの名前(`Raspberry Pi Compute Module 5`か`Raspberry Pi Compute Module 5 Lite`)と、ファームウェアのボードのリビジョンが出る。システム環境設定の小物のバージョンの欄にもボードの名前が出る。

### 4.5 GPUの試験(`V3D=2`)

`V3D=2`を付けたイメージは、起動の3秒後にGPUの試験をまとめて行い、コンソールに`v3d test:`で始まる行を出力する。最後の`v3d test: summary <成功数>/<総数>`が結果である。試験が失敗しても、起動は止まらない(第13章13.8.2)。

### 4.6 デバイスの実身の確認(`objtest:`)

Raspberry Pi 5とCM5のイメージは、実身の層ができて4秒後に、QEMUでは試せないデバイスの実身を確かめ、コンソールに`objtest:`で始まる行を出力する(第8章8.4)。確かめるのは、GPIOの5つのバンクの実身とピンの行の数、基板のLEDをバンクの実身で3回点滅させて行の値が変わること、カーネルやボードのピン(常時オンのGPIO 5。Raspberry Pi 5ではSDの挿抜検出、CM5ではアンテナの選択。それとEthernetのPHYのリセット)への書込みが`E_BUSY`になること、GPIO26のプルを上げてから下げると入力の変化が`OB_E_CHANGE`で届くこと(GPIO26は入力のまま元のプルに戻す)、システムの実身のレコード9にSoCの温度とARMとV3Dのクロックがあること、画面の実身のモードを読め、今の解像度を書き直せて(`E_OK`)、xのあるキーでピクセルを読めること、USBデバイスとキーボード、ポインタのそれぞれに実身があること、乱数の実身を読めることである。最後の`objtest: summary <成功数>/<総数>`が結果である。どの確認にも時間の上限があり、失敗しても起動は止まらない。`OBJTEST=0`を付けてビルドすると行わない。

### 4.6.1 音の出力先の確認(`SNDTEST=1`、`sndtest:`)

`SNDTEST=1`を付けたイメージは、起動の25秒後に音の出力先を順に鳴らして確かめ、コンソールに`sndtest:`で始まる行を出力する(第6章6.5.4)。それぞれの出力先で440Hzの小さな音が2秒ほど鳴るので、聞こえ方も確かめられる。確かめるのは、出力先の一覧、I2Sのclk_i2sの実測(3072kHz)とリングバッファの進み(毎秒187.5ブロック)、HDMI0とHDMI1のリングバッファの進み、USBオーディオの機能が毎秒48000フレームを取ること、再生中に出力先を切り替えてもチャネルが止まらないことである。音を出せないHDMI端子とUSBの機能がないときは`skipped`になる。最後に結果を1行ずつ`sndtest: result`で並べ直し、`sndtest: summary <成功数>/<総数>`を出す。どの確認にも時間の上限があり、失敗しても次の確認に進み、起動も止まらない。確認の前に選ばれていた出力先は最後に選び直す。

### 4.7 Raspberry Pi 4、400、CM4で起動する

イメージ(3.8)は4.1と同じくRaspberry Pi Imagerの「カスタムイメージを使う」でmicroSDカードに書き込む。「Raspberry Piデバイス」は「Raspberry Pi 4」を選ぶ。パーティションテーブルはGPTなので、EEPROMのブートローダは2020年9月以降のバージョンにしておく(Raspberry Pi OSの`rpi-eeprom-update`か、Raspberry Pi Imagerの「Misc utility images」の「Bootloader」で更新する)。CM4はeMMC付きならCM5と同じくrpibootで(4.4)、LiteならIOボードのmicroSDスロットに書き込む。

| 端子 | つなぐもの |
| --- | --- |
| HDMI 0(USB-C端子に近い側) | ディスプレイ |
| USB 3.0、USB 2.0(Type-A) | キーボードとマウス(USBメモリ、USBオーディオも使える)。CM4ではIOボードのPCIeスロットに挿したxHCIのカードのポートだけが使える |
| Ethernet | ネットワーク(DHCPでアドレスを取得し、SNTPで時計を合わせる) |
| 40ピンヘッダの8番(TXD)、10番(RXD)、6番(GND) | シリアルコンソール。3.3VのUSBシリアル変換をつなぐ。設定は4.3と同じ115200bps |
| USB-C(Raspberry Pi 4)、電源端子 | 電源(5V 3Aを推奨) |

Raspberry Pi 4系には時計がないので、ネットワークにつながるまでは1985年から数える。起動の記録の`TessronOS: board`の行に見分けたボードの名前とリビジョンが出る。デスクトップのイメージでは、起動の後で`objtest:`の行(4.6の確認に、CPUの数、メモリ、SDカードの転送の方式、PCIeとVL805、Ethernetのリンクを加えたもの)が出る。実機で確かめる項目の一覧は設計書3.6.6にある。

## 5. WindowsのQEMUで試す

WSLのxPackのQEMUは画面を表示できず、ユーザモードのネットワークも持たない。画面とネットワークを使うときは、WindowsのQEMUを使う。

### 5.1 QEMU for Windows

2.3で入れたものを使う。`"C:\Program Files\qemu\qemu-system-aarch64.exe" --version`でバージョンを確かめる。

### 5.2 デスクトップを動かす

3.3の手順でカーネルとディスクを作り、PowerShellでリポジトリの先頭に移動して次を実行する。

```powershell
cd C:\home\TessronOS
powershell -ExecutionPolicy Bypass -File tools\run_desktop.ps1
```

QEMUのウインドウにデスクトップが表示され、PowerShellのウインドウにコンソールが出力される。マシンは試験と同じ構成(CPU 4つ、メモリ2GB)で、画面、USBのキーボードとタブレット、ユーザモードのネットワーク(DHCPでWindowsを経由してインターネットにつながる)、USBオーディオを持つ。

| オプション | 意味 |
| --- | --- |
| `-Fresh` | ディスクのコピーで動かす。デスクトップで保存した内容は、元のディスクに残らない |
| `-NoNet`、`-NoSound` | ネットワーク、音を使わない |
| `-Elf <ファイル>`、`-Disk <ファイル>` | カーネルとディスクを指定する。ブラウザ入りなら`-Elf build_make\tessronosdeskbr.elf -Disk build_make\obj\_QEMU_VIRT__ktestbr\test_disk.img`のようにする |
| `-Display sdl` | 画面の表示にSDLを使う(既定はGTK) |

操作の注意は次のとおりである。

- マウスはタブレットとしてつないでいるので、QEMUのウインドウに捕まえられない。
- 終了するときは、QEMUのウインドウを閉じる。デスクトップの状態は、閉じる前に保存したものだけがディスクに残る。
- 真新しいディスクで最初に起動したときは、書体と壁紙を取り込むので、デスクトップが表示されるまで数分かかる。

ブラウザ入りのデスクトップを動かすときは、ディスクも`BROWSER=1 BROWSER_BLINK=1`で作る(システムボリュームにブラウザのプログラムが入る)。

```sh
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 BUILD_ID=deskbr
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 BROWSER=1 BROWSER_BLINK=1 BUILD_ID=br obj/_QEMU_VIRT__ktestbr/test_disk.img
```

### 5.3 ネットワークを使う試験

`NET=user`を付けると、WSLのmakeが自動でWindowsのQEMUとWindowsのPythonを使い、試験用のFTPサーバとHTTPサーバをWindowsで起動する。

```sh
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=net,so,ms NET=user BUILD_ID=net test
```

ブラウザの試験で外部のサイトを開くときも`NET=user`を付ける(第14章14.10)。`NET=user`のとき、`ktest_so`はDHCP、インターネットの名前の解決、SNTPによる時刻合わせも確かめ、`ktest_so`と`ktest_svcio`はループバックとホストとの間のTCPの速さを測って出力する。

### 5.4 デスクトップのUIの試験

`tools/ui_run.py`は、デスクトップを起動済みの状態から、キーとポインタの操作を数秒で再生して、スクリーンショットを撮る。WindowsのPythonからWindowsのQEMUで実行すると、ユーザモードのネットワークも使える。カーネルは`UITEST=1`を付けてビルドする。

操作は1行に1つずつ手順のファイルに書く。

```text
click 110 100
wait 500
shot first
```

```sh
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 UITEST=1 BUILD_ID=desk
python.exe ../tools/ui_run.py --elf tessronosdesk.elf --net user --out shots first.steps
```

最初の1回だけマシンを起動し、デスクトップが起動した時点の状態を保存する。2回目からはその状態から再開するので、数秒で終わる。スクリーンショットは`--out`のディレクトリに`first.png`として保存される。コマンドの一覧は第12章12.4にある。

## 6. 困ったとき

| 症状 | 原因と対処 |
| --- | --- |
| Raspberry Pi 5の画面に何も出ない | シリアルコンソールで起動の記録を見る。`config.txt`とデバイスツリーが区画1にあるか、ディスプレイをHDMI 0につないでいるかを確かめる |
| CM5のシリアルコンソールに何も出ない | 40ピンヘッダの8番と10番につないでいるかを確かめる。区画1に`cmdline-cm5.txt`と`config.txt`の`[cm5]`の節があるかを確かめる。どちらかだけを消すとコンソールが移らないか、起動が止まる(4.4) |
| CM5がeMMCから起動しない | J2のジャンパ(`nRPI_BOOT`)を外したか、書込みの後に電源を入れ直したかを確かめる。CM5 LiteにはeMMCがないので、microSDカードに書き込む |
| 書き込んだカードをWindowsがフォーマットしようとする | TSFSの区画を読めないための表示である。「キャンセル」を選ぶ |
| Chromiumのコンパイルが極端に遅い | メモリが足りずスワップを使っている。`free -g`で確かめ、並列の数(`JOBS`)を下げる |
| `make BROWSER=1`が`no .../browser.mk`で止まる | `tessron-chromium`が隣にない。`CHROMIUM=<場所>`を指定する |
| `NET=user`の試験がQEMUを起動できない | WindowsのQEMUが`C:\Program Files\qemu`にない、またはWSLから`python.exe`が呼べない |
| 変数を変えたのに反映されない | `BUILD_ID`を分けていない。組合せごとに別の`BUILD_ID`にする |
| 最初のビルドが`tools/netbsd/build.sh`の`fetch`で止まる | NetBSDの配布物を取得できない。インターネットにつながるか、`curl`があるかを確かめる。スクリプトは通常の配布サーバ、保存用のサーバ、ftp.netbsd.orgの順に試し(新しいバージョンが出るとNetBSD 11.0は通常の配布サーバから外される)、どこからも取れなければTessronOSのリリースに添付した`netbsd-11.0-rumpsrc.tgz`を使う。それも取れないときは、その添付を手で`tools/netbsd/`に置くか、`NB_SUB_URL`で置き場を指定する。SHA512が合わないときは、取得した`~/netbsd/syssrc-11.0.tgz`を消してやり直す |
| ネットワークの処理が止まったように見える | `RUTRACE=1`を付けてビルドすると、NetBSDのスタックの中でロックを5秒以上待っているタスクがコンソールに出る |
