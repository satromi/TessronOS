# ビルドと実行の手順

TessronOSをソースからビルドし、Raspberry Pi 5で起動する手順と、WindowsのQEMUで試す手順を説明する。設計の詳細は[TessronOS設計書](tessronos.md)の第12章(開発と試験)、第14章(ブラウザ)にある。

## 1. 必要なもの

| 用途 | 必要なもの |
| --- | --- |
| ビルド | Windows 11とWSL2のUbuntu(24.04で確認)。ディスクの空きは、TessronOSだけなら5GB、ブラウザも作るなら100GB以上。最初のビルドで、NetBSDのネットワークスタックのソース(約80MBの配布物)をインターネットから取得する |
| ブラウザのビルド | WSLに割り当てるメモリ16GB以上(24GBを推奨)。Chromiumのチェックアウト(約22GB) |
| Windowsで試す | QEMU for Windows(11.0で確認)、Python 3 for Windows(ネットワークの試験とUIの試験に使う) |
| 実機で動かす | Raspberry Pi 5(4GB以上)、microSDカード(1GB以上、ブラウザ入りなら2GB以上)、Raspberry Pi Imager、HDMIのディスプレイ、USBのキーボードとマウス |
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
| `TARGET=_QEMU_VIRT_`、`TARGET=_RPI5_` | QEMU用、Raspberry Pi 5用。必須 |
| `DESKTOP=1` | デスクトップが起動する構成 |
| `KTEST=1` | カーネル内の試験を組み込む。試験用ディスクもこのときに作る |
| `MOZC=1` | かな漢字変換にMozcの変換エンジンと辞書を入れる(3.4) |
| `NETSTACK=lwip` | TCP/IPスタックを以前のlwIPにする。指定しなければNetBSDのスタックになる(3.7) |
| `BROWSER=1 BROWSER_BLINK=1` | ブラウザを入れる(3.5) |
| `V3D=2` | Raspberry Pi 5のGPUを使えるようにし、起動の後でGPUの試験をまとめて行う(第13章) |
| `BUILD_ID=<名前>` | 生成物の置き場を分ける。変数の組合せを変えるときは、別の名前にする |
| `-j<数>` | 並列にコンパイルする数。CPUのコア数にするとよい |

`BUILD_ID`を付けると、カーネルは`tessronos<名前>.elf`、中間ファイルとディスクのイメージは`obj/<TARGET><名前>/`にできる(`KTEST=1`のときは`obj/<TARGET>_ktest<名前>/`)。

### 3.2 QEMU用のカーネルと試験

```sh
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 test                          # 全部の試験(30分ほど)
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=wm,ob BUILD_ID=wo test # 試験グループを選ぶ
make TARGET=_QEMU_VIRT_ qemu                                       # 起動して、コンソールを端末に出す(Ctrl-A Xで終了)
```

試験は1件ごとに`KTEST <名前> PASS`か`FAIL`を出力し、最後に`KTEST SUMMARY <成功数>/<総数>`を出力する。試験グループの一覧は第12章にある。

### 3.3 デスクトップ

```sh
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 BUILD_ID=desk               # tessronosdesk.elf
make -j20 TARGET=_QEMU_VIRT_ KTEST=1 obj/_QEMU_VIRT__ktest/test_disk.img   # デスクトップが使うディスク
```

デスクトップのディスクは、試験用ディスクと同じものである。システムボリューム(システムの実身)と、プログラムや試験のファイルを置いたFATの区画を持つ。起動は5章のとおり。

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
| FFmpeg 4.4.6のソース | `~/ffbt/ffmpeg-4.4.6` | `FFMPEG` |
| コンパイルし直した生成物の置き場 | `~/tessronos-obj` | `OBJ` |

手順は次のとおりである。`gngen`と`hostgen`は最初に一度だけ実行する。

```sh
cd /mnt/c/home/tessron-chromium/tools/browser
bash build.sh gngen                          # aarch64のビルドの記述(~/cr6/src/out/tessronos)
bash build.sh hostgen                        # ホストで作るもの(V8のスナップショット、ICUのデータなど)
JOBS=12 bash build.sh v8                     # V8(1ファイル1.5GB前後のメモリを使う)
JOBS=10 bash blink.sh all                    # Blinkとその依存(約16,000ファイル)
bash rust.sh build && bash rust.sh merge     # Rustで書かれた部分
bash ffmpeg.sh                               # FFmpeg
cd /mnt/c/home/TessronOS/build_make
make -j20 TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 BUILD_ID=deskbr
```

並列の数は、メモリが足りる範囲で決める。WSLのメモリが24GBなら、V8は12、Blinkは10までにする。メモリが足りなくなるとスワップを使い始め、かえって遅くなる。

`tessron-chromium`を別の場所に置いたときは、makeに`CHROMIUM=<場所>`を、スクリプトを直接実行するときは`TESSRONOS=<TessronOSのリポジトリ>`を与える。

### 3.6 Raspberry Pi 5のSDカードのイメージ

```sh
make -j20 TARGET=_RPI5_ DESKTOP=1 BUILD_ID=pi sdimg
```

`build_make/obj/_RPI5_pi/sd.img`ができる。区画1(FAT32、256MB)にファームウェアへの指示`config.txt`、デバイスツリー、カーネル`tessronos.img`、プログラムを、区画2にシステムボリューム(TSFS)を置く。デバイスツリー(`bcm2712-rpi-5-b.dtb`)は最初に一度だけRaspberry Piのファームウェアの配布物から取得する。

よく使う組合せは次のとおりである。

| 目的 | コマンド | イメージの大きさ |
| --- | --- | --- |
| デスクトップ | `make TARGET=_RPI5_ DESKTOP=1 BUILD_ID=pi sdimg` | 約330MB |
| デスクトップ、Mozc | `make TARGET=_RPI5_ DESKTOP=1 MOZC=1 BUILD_ID=pim sdimg` | 約350MB |
| デスクトップ、Mozc、ブラウザ、GPUの試験 | `make TARGET=_RPI5_ DESKTOP=1 MOZC=1 BROWSER=1 BROWSER_BLINK=1 V3D=2 BUILD_ID=pibr sdimg` | 約600MB |
| カーネル内の試験 | `make TARGET=_RPI5_ KTEST=1 BUILD_ID=pit sdimg` | 約330MB |

すでに書き込んだSDカードのカーネルだけを差し替えるときは、SDカードの区画1をWSLから見える場所にマウントして、`make TARGET=_RPI5_ sd SD=/mnt/e`とする(`tessronos.img`と`config.txt`を上書きする)。


### 3.7 NetBSDのネットワークスタック

TCP/IPスタックは、デフォルトではNetBSD 10.1のカーネルのものである。ソースはリポジトリに含めず、最初のビルドでmakefileが`tools/netbsd/build.sh`を実行し、NetBSDの配布物を取得して、スタックに必要な部分だけを`~/netbsd/netbsd-10.1`に展開し、`~/tessronos-netbsd/rumpnet.o`にビルドする。取得には`curl`を使い、SHA512で配布物を確かめる。前もって、または作り直すときは、次のように実行する。

```sh
cd /mnt/c/home/TessronOS
bash tools/netbsd/build.sh all        # 取得、生成、コンパイル、リンク、著作権表示の一覧
bash tools/netbsd/build.sh clean      # 生成物を消す(ソースは残る)
```

ソースの置き場は`NBSRC=<ディレクトリ>`、生成物の置き場は`OUT=<ディレクトリ>`で変えられる。makeで`NETBSD_OBJ=<ファイル>`を指定すると、そのオブジェクトをリンクする。イメージを配布するときは、生成された`~/tessronos-netbsd/NOTICE-netbsd.txt`(NetBSDの著作権表示とライセンス条件の一覧)を添付する(`THIRD_PARTY_NOTICES.md`)。

## 4. Raspberry Pi 5で起動する

### 4.1 Raspberry Pi Imagerで書き込む

1. WSLで作った`sd.img`は、Windowsからは`C:\home\TessronOS\build_make\obj\_RPI5_pi\sd.img`のように見える。
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

40ピンヘッダのUART(GPIO14、15)に出したいときは、`RP1_CONSOLE=1`を付けてビルドする。

### 4.4 GPUの試験(`V3D=2`)

`V3D=2`を付けたイメージは、起動の3秒後にGPUの試験をまとめて行い、コンソールに`v3d test:`で始まる行を出力する。最後の`v3d test: summary <成功数>/<総数>`が結果である。試験が失敗しても、起動は止まらない(第13章13.8.2)。

### 4.5 デバイスの実身の確認(`objtest:`)

Raspberry Pi 5のイメージは、実身の層ができて4秒後に、QEMUでは試せないデバイスの実身を確かめ、コンソールに`objtest:`で始まる行を出力する(第8章8.4)。確かめるのは、GPIOの5つのバンクの実身とピンの行の数、基板のLEDをバンクの実身で3回点滅させて行の値が変わること、カーネルのドライバのピン(SDの挿抜検出、EthernetのPHYのリセット)への書込みが`E_BUSY`になること、GPIO26のプルを上げてから下げると入力の変化が`OB_E_CHANGE`で届くこと(GPIO26は入力のまま元のプルに戻す)、システムの実身のレコード9にSoCの温度とARMとV3Dのクロックがあること、画面の実身のモードを読め、今の解像度を書き直せて(`E_OK`)、xのあるキーでピクセルを読めること、USBデバイスとキーボード、ポインタのそれぞれに実身があること、乱数の実身を読めることである。最後の`objtest: summary <成功数>/<総数>`が結果である。どの確認にも時間の上限があり、失敗しても起動は止まらない。`OBJTEST=0`を付けてビルドすると行わない。

## 5. WindowsのQEMUで試す

WSLのxPackのQEMUは画面を表示できず、ユーザモードのネットワークも持たない。画面とネットワークを使うときは、WindowsのQEMUを使う。

### 5.1 QEMU for Windows

2.3で入れたものを使う。`"C:\Program Files\qemu\qemu-system-aarch64.exe" --version`で版を確かめる。

### 5.2 デスクトップを動かす

3.3の手順でカーネルとディスクを作り、PowerShellでリポジトリの先頭に移動して次を実行する。

```powershell
cd C:\home\TessronOS
powershell -ExecutionPolicy Bypass -File tools\run_desktop.ps1
```

QEMUのウインドウにデスクトップが表示され、PowerShellのウインドウにコンソールが出力される。マシンは試験と同じ構成(CPU 4つ、メモリ2GB)で、画面、USBのキーボードとタブレット、ユーザモードのネットワーク(DHCPでWindowsを経由してインターネットにつながる)、USBオーディオを持つ。

| オプション | 意味 |
| --- | --- |
| `-Fresh` | ディスクの写しで動かす。デスクトップで保存した内容は、元のディスクに残らない |
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

`tools/ui_run.py`は、デスクトップを起動済みの状態から、キーとポインタの操作を数秒で再生して、画面を写す。WindowsのPythonからWindowsのQEMUで実行すると、ユーザモードのネットワークも使える。カーネルは`UITEST=1`を付けてビルドする。

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

最初の1回だけマシンを起動し、デスクトップが起動した時点の状態を保存する。2回目からはその状態から再開するので、数秒で終わる。写した画面は`--out`のディレクトリに`first.png`として保存される。コマンドの一覧は第12章12.4にある。

## 6. 困ったとき

| 症状 | 原因と対処 |
| --- | --- |
| Raspberry Pi 5の画面に何も出ない | シリアルコンソールで起動の記録を見る。`config.txt`とデバイスツリーが区画1にあるか、ディスプレイをHDMI 0につないでいるかを確かめる |
| 書き込んだカードをWindowsがフォーマットしようとする | TSFSの区画を読めないための表示である。「キャンセル」を選ぶ |
| Chromiumのコンパイルが極端に遅い | メモリが足りずスワップを使っている。`free -g`で確かめ、並列の数(`JOBS`)を下げる |
| `make BROWSER=1`が`no .../browser.mk`で止まる | `tessron-chromium`が隣にない。`CHROMIUM=<場所>`を指定する |
| `NET=user`の試験がQEMUを起動できない | WindowsのQEMUが`C:\Program Files\qemu`にない、またはWSLから`python.exe`が呼べない |
| 変数を変えたのに反映されない | `BUILD_ID`を分けていない。組合せごとに別の`BUILD_ID`にする |
| 最初のビルドが`tools/netbsd/build.sh`の`fetch`で止まる | NetBSDの配布物を取得できない。インターネットにつながるか、`curl`があるかを確かめる。SHA512が合わないときは、取得した`~/netbsd/syssrc-10.1.tgz`を消してやり直す |
| ネットワークの処理が止まったように見える | `RUTRACE=1`を付けてビルドすると、NetBSDのスタックの中でロックを5秒以上待っているタスクがコンソールに出る |
