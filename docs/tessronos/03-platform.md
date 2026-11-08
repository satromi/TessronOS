# 第3章　動作環境

## 3.1 対象のマシン

TessronOSが動くのは、Raspberry Pi 5、Raspberry Pi Compute Module 5(以下CM5)、Raspberry Pi 4 Model B、Raspberry Pi 400、Raspberry Pi Compute Module 4(以下CM4)、QEMUの`virt`マシンである。いずれもAArch64のCPUを4つ持ち、同じ`kernel/sysdepend/cpu/core/armv8a/`のコードで動く。マシンごとに異なるのはアドレスと割込み番号のテーブル、起動手順、一部のドライバだけで、`kernel/sysdepend/rpi5/`、`kernel/sysdepend/rpi4/`、`kernel/sysdepend/qemu_virt/`に分けてある。2つのRaspberry Piに共通のもの(メールボックス、ファームウェアの時計、乱数)は`kernel/sysdepend/rpi/`にある。CM5はRaspberry Pi 5と同じBCM2712とRP1を載せたモジュールで、Raspberry Pi 5と同じカーネルのイメージ(`TARGET=_RPI5_`)で動く。両者の違いは起動時にボードを見分けて吸収する(3.5)。Raspberry Pi 4、400、CM4はBCM2711を載せたボードで、命令セットの基準が違うので別のカーネルのイメージ(`TARGET=_RPI4_`)で動く(3.6)。このほか、UEFIで起動するx64のPCで動く。x64は別のイメージ(`TARGET=_X64_`)で、QEMUの`q35`マシンとLenovo ThinkPad X1 Yoga 第3世代の実機でデスクトップまで起動することを確かめ、Microsoft Surface Go 2への対応を進めている(3.10)。

| 項目 | Raspberry Pi 5 | Raspberry Pi 4系 | QEMU virt |
| --- | --- | --- | --- |
| CPU | Cortex-A76 ×4(BCM2712) | Cortex-A72 ×4(BCM2711) | `-cpu cortex-a76 -smp 4` |
| RAM | 物理0から。1GB直下にファームウェアの予約がある | 物理0から。1GB未満、1GBから`0xFC000000`まで、8GBのモデルでは4GBから8GBまでの範囲(3.6.1) | `0x40000000`から連続 |
| カーネルの配置 | `0x200000`(フラットな`tessronos.img`) | `0x200000`(フラットな`tessronos.img`) | `0x40200000`(ELFを`-kernel`で渡す) |
| 起動時のEL | EL2 | EL2 | EL1(`virtualization=on`でEL2) |
| 割込み | GIC-400(GICv2) | GIC-400(GICv2) | GICv2(`gic-version=2`) |
| コンソール | デバッグ用UART(PL011) | 40ピンヘッダのUART0(PL011) | PL011 |
| ディスク | SDカード、USBメモリ | SDカードかeMMC(EMMC2)、USBメモリ | virtio-blk、SDHCI、USBメモリ |
| ネットワーク | RP1のGigabit Ethernet | GENETのGigabit Ethernet | virtio-net |
| 画面 | ファームウェアが用意するフレームバッファ | ファームウェアが用意するフレームバッファ | `bochs-display` |
| USB | RP1のxHCI | PCIeの先のVL805(xHCI) | `qemu-xhci` |
| 時計 | ファームウェアに問い合わせる(電池がなければ1970年から) | ない。SNTPで合わせる | PL031 |
| 乱数 | ハードウェア乱数生成器とソフトウェアの疑似乱数生成器 | 同じ | ソフトウェアの疑似乱数生成器 |

Raspberry Pi 5のUSB、Ethernet、GPIO、サウンドは、PCIeの先のRP1というチップにある。TessronOSはPCIeのルートコンプレックスを自前で初期化し、RP1のアドレスウインドウを通してこれらを扱う。QEMUにはRP1がないので、同じ役割をvirtioデバイス、`qemu-xhci`、`bochs-display`が担う。

## 3.2 起動

Raspberry Pi 5では、EEPROMのブートローダがSDカードの第1パーティションから`config.txt`、デバイスツリー、`tessronos.img`を読み込み、CPU0をEL2でイメージの先頭から実行する。CM5では同じことを、eMMCかCM5 LiteのIOボードのSDカードから行う。`config.txt`は次のとおりである。

```text
kernel=tessronos.img
kernel_address=0x200000
enable_uart=1
uart_2ndstage=1
pciex4_reset=0
framebuffer_depth=32
framebuffer_ignore_alpha=1

[cm5]
device_tree=bcm2712-rpi-cm5-cm5io.dtb
enable_rp1_uart=1
cmdline=cmdline-cm5.txt
[all]
[board-type=0x1a]
device_tree=bcm2712-rpi-cm5l-cm5io.dtb
[all]
```

`[cm5]`の節はCM5とCM5 Liteの両方が読み、`[board-type=0x1a]`の節はCM5 Liteだけが読む(0x1aはリビジョンコードのボードの種類)。後から読んだ`device_tree`が前の指定を置き換えるので、ボードごとに合ったデバイスツリーが読み込まれる。`[pi5]`の条件はCM5にも当てはまるので使わない。`enable_rp1_uart`と`cmdline`はCM5のコンソールを40ピンヘッダに移すための指定である(3.5.2)。

ファームウェアは、`kernel_address`の指定があっても、ヘッダのないイメージを`0x80000`に置くことがある。そのためカーネルのエントリは、実際にロードされたアドレスがリンクアドレスと異なれば、イメージを`0x200000`へコピーしてから処理を続ける。その後EL2からEL1へ移り、MMUを有効にして`main()`へ進む。2つめ以降のCPUはPSCIの`CPU_ON`で起動する。

起動の後半では、ドライバ、描画、ウインドウ、USB、ネットワーク、乱数、プロセス管理、実身マネージャの順に初期化し、最後に初期プログラム(`CNF_INIT_PATH`)があればそれを実行する。デスクトップ構成では、続いてデスクトップが起動する。

## 3.3 SDカードの構成

SDカードのイメージは`make TARGET=_RPI5_ BUILD_ID=<名前> sdimg`で作る。パーティションテーブルはGPTで、パーティションは次のとおりである。

| パーティション | 形式 | 内容 |
| --- | --- | --- |
| 1 | FAT32、512MB | `config.txt`、`cmdline-cm5.txt`、3つのデバイスツリー、`tessronos.img`と、`/boot`に置くファイル |
| 2 | TSFS、2GB | システムボリューム。フォント、壁紙、定義実身、小物、辞書を実身として格納し、利用者の実身も置く |
| 3 | 空、16MB | カーネル内の試験の構成にだけある作業用パーティション |

システムボリュームの大きさは`build_make/makefile`の`SYS_VOL_SIZE`で決まり、Raspberry Piのイメージでは2GBにしてある(`BROWSER_BLINK=1`のときの`browser.mk`の値より先に決める)。`tools/mksd.py`はシステムボリュームを64MBずつイメージにコピーするので、イメージを作るときに2GBをメモリに置かない。区画表はイメージの大きさで作るので、バックアップの区画表はイメージの終わりにあり、カードに書くとカードの途中に来る。最初の起動で、カーネルが区画2とシステムボリュームを8GBまで(カードがそれより小さければカードの終わりまで)広げ、バックアップの区画表をカードの終わりへ移す(7.7)。コンソールには`sysvol: grown 2048 MB -> 8192 MB`のように出る。

Raspberry Pi 5とRaspberry Pi 500のデバイスツリーは`config.txt`で指定せず、ファームウェアがボードとチップのステッピングから選ぶ。C1のRaspberry Pi 5は`bcm2712-rpi-5-b.dtb`、D0のRaspberry Pi 5は`bcm2712d0-rpi-5-b.dtb`、Raspberry Pi 500は`bcm2712-rpi-500.dtb`である。以前は`bcm2712-rpi-5-b.dtb`に固定していたので、D0のボードにC1の値が渡り、Wi-Fiの初期化がC1のピンの表で範囲外のレジスタ(ピンコントローラの0x24。D0のレジスタは0x20バイト)を読んでSErrorで止まった。デバイスツリーはCM5の`bcm2712-rpi-cm5-cm5io.dtb`、CM5 Liteの`bcm2712-rpi-cm5l-cm5io.dtb`で、ファームウェアの配布物から取得して使う。再配布はしないので、リポジトリには含めていない。1つのイメージがRaspberry Pi 5とCM5の両方で起動するので、同じイメージをSDカードにもCM5のeMMCにも書き込める。

Raspberry Pi 4系のイメージは`make TARGET=_RPI4_ BUILD_ID=<名前> sdimg`で作る。構成は同じで、ファイルが違う(3.6.5)。

## 3.4 動作確認の範囲

Raspberry Pi 4系は実機ではまだ確かめておらず、QEMUの`raspi4b`で確かめられる範囲を確かめた(3.6.6)。

| 項目 | QEMU | Raspberry Pi 5 |
| --- | --- | --- |
| カーネル内の試験 | すべて(449件、前提条件を満たさない10件を除く) | 391件中390件(2026年9月28日) |
| 4つのCPU、SDカード | 確認済み | 確認済み |
| USBのキーボードとマウス、USBメモリ | 確認済み | マウスまで確認済み |
| Ethernet、DHCP、DNS | 確認済み | 確認済み |
| IPv6(ルータ広告による自動設定、AAAA、IPv6のTCP) | 確認済み(QEMUのユーザモードのネットワーク) | 未確認 |
| SNTPによる時計合わせ | 確認済み | 未確認 |
| デスクトップ | 確認済み | イメージは作成できるが、実機での確認はこれから |
| サウンド | USBオーディオで確認済み(出力先の選択と抜き差しによる切替えを含む) | 未確認(I2SとHDMIの音は実機の試験をこれから行う) |

CM5は実機ではまだ確認していない。ボードの見分けとボードごとの値はカーネル内の試験(`ktest_board`)で、eMMCの初期化と読み書きはQEMUのeMMC(`make SD_EMMC=1`)で確かめた(3.5.4)。

x64のPCで確かめた範囲は3.10.5に、実機での起動は3.10.7と3.10.8に記す。

## 3.5 Raspberry Pi Compute Module 5

CM5はRaspberry Pi 5と同じBCM2712とRP1を載せたモジュールで、IOボード(CM5IO)に挿して使う。eMMC(16GB、32GB、64GB)を載せたものと、載せていないCM5 Liteがある。周辺機器のアドレスと割込み番号はRaspberry Pi 5と同じなので、カーネルのイメージは分けず、起動時にボードを見分けて、違う配線だけをボードの表で切り替える。この節の値は、ファームウェアの配布物の`bcm2712-rpi-5-b.dtb`、`bcm2712-rpi-cm5-cm5io.dtb`、`bcm2712-rpi-cm5l-cm5io.dtb`と、CM5とCM5IOのデータシートで確かめたものである。

### 3.5.1 差異

| 項目 | Raspberry Pi 5 | CM5(eMMCあり) | CM5 Lite | 扱い |
| --- | --- | --- | --- | --- |
| ルートノードの`compatible` | `raspberrypi,5-model-b` | `raspberrypi,5-compute-module` | CM5と同じ(`model`の末尾が`Lite`) | ボードの見分けに使う |
| リビジョンコードのボードの種類 | 0x17 | 0x18 | 0x1a | 見分けを確定する |
| デバイスツリー | ファームウェアが選ぶ(`bcm2712-rpi-5-b.dtb`、`bcm2712d0-rpi-5-b.dtb`、`bcm2712-rpi-500.dtb`) | `bcm2712-rpi-cm5-cm5io.dtb` | `bcm2712-rpi-cm5l-cm5io.dtb` | `config.txt`の条件で選ぶ(3.2) |
| SDHCI(`0x10_00ff_f000`、SPI 273)の先 | SDカードのスロット、4ビット | モジュール上のeMMC、8ビット、HS200/HS400対応、取外し不可 | IOボードのmicroSDスロット、4ビット | eMMCはMMCとして初期化する(3.5.3) |
| カード検出 | 常時オンのGPIO 5(負論理) | なし(`broken-cd`) | なし(`broken-cd`) | CM5では検出線を見ず、カードを初期化してみる |
| SDの信号電圧 | 3.3Vと1.8Vを常時オンのGPIO 3で切り替える。カードの電源は常時オンのGPIO 4 | 1.8V固定 | Raspberry Pi 5と同じ | Raspberry Pi 5とCM5 LiteではUHS-Iのとき1.8Vにする(3.5.5)。CM5では1.8Vの信号であることをコントローラに設定する |
| 常時オンのGPIO 5、6 | 5はカード検出、6はSDの電源の異常 | Wi-Fiのアンテナの選択(5は出力H、6は出力L) | CM5と同じ | CM5では5と6をボードのものとし、GPIOの実身から変更させない |
| ACT LED | 常時オンのGPIO 9(負論理) | 同じ | 同じ | 変更なし |
| 電源LED | RP1のGPIO 44 | 同じ | 同じ | 変更なし |
| デバッグ用UART(`0x10_7d00_1000`) | 3ピンのコネクタ | モジュール上にコネクタを付ける場所とテストポイントだけがあり、CM5IOにはコネクタがない | CM5と同じ | 起動の前半の出力はここに出る |
| 40ピンヘッダのUART0(GPIO 14、15) | RP1のUART0 | 同じ | 同じ | CM5ではコンソールをここに移す(3.5.2) |
| Ethernet | RP1のGEM、BCM54213PE、MDIOのアドレス1 | RP1のGEM、BCM54210PE、MDIOのアドレス0。PHYの割込みがRP1のGPIO 37 | CM5と同じ | PHYは標準のレジスタだけで扱う。ボードの表のアドレスを先に試し、応答がなければ全アドレスを探す |
| USB | RP1のxHCI 2つ | 同じ(CM5IOのType-Aのポート2つ。Type-CのポートはrpibootとUSBデバイス用) | 同じ | 変更なし。CM5には過電流の検出線(RP1のGPIO 43)がない |
| PCIeの外部コネクタ | FFCのコネクタ(PCIe 2.0 x1) | CM5IOのM.2 Mキー(PCIe 2.0 x1) | 同じ | どちらもまだ使わない |
| ファン | RP1のGPIO 45(PWM)とGPIO 29(回転数) | 同じ(CM5IOの4ピンのコネクタ) | 同じ | どちらもまだ制御しない |
| 電源ボタン | GPIO 20 | 同じ(CM5IOのボタン) | 同じ | 変更なし |
| RTC | ファームウェアに問い合わせる。電池は別売 | 同じ。CM5IOにCR2032のソケットがある | 同じ | 変更なし。充電はしない(デバイスツリーの`trickle-charge-microvolt`は0) |

ボードは次の順に見分ける。起動の最初に、デバイスツリーのルートノードの`compatible`と`model`からボードを決める(`kernel/sysdepend/cpu/core/armv8a/board.c`)。続いてデバイスの初期化の最初に、ファームウェアのプロパティインタフェースでボードのリビジョン(タグ`0x00010002`)を問い合わせ、そのボードの種類が表にあればそれで確定する。デバイスツリーは`config.txt`で選んだファイルにすぎないが、リビジョンはボードそのものを表すためである。どちらでも分からないボードはRaspberry Pi 5として扱い、名前にはデバイスツリーの`model`を使う。見分けた結果は`knl_board`(`include/ts/board.h`)にあり、SDHCI、GPIOの実身、Ethernet、システムの実身(`BOARD`と`BOARD_REV`)が参照する。

ボードを見分けた直後に、ファームウェアにCPUのクロックの最大値(タグ`0x00030004`)と最小値(タグ`0x00030007`)を問い合わせ、最大値と既定の上限1800MHzの低いほうに設定する(タグ`0x00038002`)。ファームウェアはCPUを定格より低いクロックで起動し(Raspberry Pi 5では1.5GHz)、上げるのはOSに任せるためである。設定の実身が読めるようになると、マシンの設定の上限`CPU_MHZ`に合わせ直す(3.9)。設定した結果はコンソールに`TessronOS: processors at ... MHz`と出る。温度が上がりすぎたときや電源の電圧が下がったときにクロックを下げるのは、引き続きファームウェアが行う。Raspberry Pi 4、400、CM4でも同じ処理を行う。

BCM2712にはC1とD0の2つのステッピングがあり、ファームウェアの配布物の`bcm2712d0-rpi-5-b.dtb`によれば、D0ではデバッグ用UARTの割込みがSPI 120(C1は121)、常時オンのGPIOのバンク0が15本(C1は17本)である。カーネルはC1の値を使っているので、D0のステッピングのボードではデバッグ用UARTの受信の割込みが届かない(受信したバイトは読出しのときにFIFOから取り出すので、入力は遅れるが失われない)。これはRaspberry Pi 5の2GBモデルにも共通する未対応の点である。

### 3.5.2 コンソール

CM5IOにはデバッグ用UARTのコネクタがないので、CM5ではコンソールを40ピンヘッダの8番(TXD)と10番(RXD)、つまりRP1のUART0に移す。ファームウェアがRP1のUART0にクロックを入れるのは`config.txt`に`enable_rp1_uart=1`があるときだけで、クロックのないUART0に触れるとSErrorになる。そこで、移すかどうかをカーネルのイメージには埋め込まず、`config.txt`の`[cm5]`の節で`enable_rp1_uart=1`と`cmdline=cmdline-cm5.txt`を組にして指定する。`cmdline-cm5.txt`の`tessronos.console=rp1`は、ファームウェアがデバイスツリーの`/chosen/bootargs`に入れて渡す。カーネルはこれを見て、RP1のGPIOを初期化した直後にコンソールを移す。それまでの出力はデバッグ用UARTに出る。Raspberry Pi 5でも、`cmdline.txt`に`tessronos.console=rp1`を書き、`config.txt`に`enable_rp1_uart=1`を足せば、ビルドし直さずに同じことができる(`make RP1_CONSOLE=1`と同じ結果になる)。

### 3.5.3 eMMC

`device/sd/sdhci.c`は、SDカードとして応答しないデバイスをMMC(JEDECのeMMC)として初期化する。eMMCはアイドル状態でCMD8に応答しないので、SDカードとの区別はCMD8の無応答で行う。初期化の手順は次のとおりである。

1. CMD0でアイドル状態にし、CMD1(引数`0x40FF8080`。セクタアドレスと、2.7〜3.6Vおよび1.70〜1.95V)をビジーが解けるまで繰り返す。応答のアクセスモードがセクタなら、2GBを超えるデバイスとしてセクタ番号で読み書きする。
2. CMD2でCIDを読み、CMD3で相対アドレス(1)を与え、CMD9でCSDを読む。書込み禁止のビットはSDカードと同じ位置にある。
3. CMD7で選択し、クロックを25MHz(後方互換のタイミングの上限26MHzの内側)に上げ、CMD8で拡張CSDを読む。2GBを超えるデバイスの大きさは拡張CSDの`SEC_COUNT`(212〜215バイト目)から求める。
4. CMD6で拡張CSDの`BUS_WIDTH`(183バイト目)を書き換えてバス幅を広げる。ボードが8本を配線し(CM5)、コントローラの能力レジスタのビット18が8ビットに対応していれば8ビット、そうでなければ4ビットを試す。切り替えた後に拡張CSDをもう一度読み、デバイスを記述する192バイト目以降が最初に読んだものと一致すれば、その幅を使う。一致しなければ1ビットに戻して次の幅を試す。

データの読み書き(CMD17、18、24、25と自動CMD12)、DMAの方式の選択と検査、パーティションの読み取りはSDカードと共通である。eMMCは取外しできないディスクとして`sda`に登録する。HS200とHS400への切り替え、ブートパーティションとRPMBへのアクセス、eMMCのキャッシュの利用はしていない。

### 3.5.4 試験

QEMUにはCM5の機種がないので、次の2つで確かめる。

- `ktest_board`は、Raspberry Pi 5、CM5、CM5 Lite、QEMU、未知のボードのルートノードを持つデバイスツリーをメモリ上に組み立て、ルートノードの読み取り、`compatible`と`model`とリビジョンからの見分け、ボードごとの値、コマンドラインの解析を確かめる。
- `ktest_sd`の`test_speed`は、QEMUのsdhci-pciとsd-card(CMD6のHigh Speedを持つ)で、カードがHigh Speedに切り替わり、クロックが既定の速さより上がっていることと、SCRがCMD23を示さないのでCMD12で止めることを確かめる。後に続く読み書きの試験は、その速さで行う。
- `ktest_sd`の`test_cmd23`は、試験用の区画に、複数ブロックの転送をCMD12とCMD23(コマンドとして送る)で止めて書き、データポートと全体のディスクから読み直して確かめる。Auto-CMD23は実機でだけ試す(7.3)。
- `ktest_sd`の`test_uhs_back`は、1.8Vへの切り替えが失敗したときの戻り方を、カードを初期化し直す`knl_sd_reinit`で試す。QEMUでは、UHS-Iを申し出てもS18Aが返らずHigh Speedになること、S18Aが返ったことにするとCMD11に応答がなく電源を入れ直してHigh Speedになること、CMD11なしで切り替わったことにするとSDR25になること、SDR25に切り替えた後の読み直しをわざと食い違わせると電源を入れ直してHigh Speedになることを確かめ、どの場合も先頭の32KBが前と同じに読めることを見る。実機では、読み直しを食い違わせて3.3Vに戻ること、もう一度初期化して起動時と同じ速さに戻ることを確かめる。
- `make TARGET=_QEMU_VIRT_ KTEST=1 KTONLY=sd SD_EMMC=1 test`は、試験用のSDカードのイメージをQEMUの`emmc`としてsdhci-pciに挿し、能力レジスタに8ビットのビットを足して、`ktest_sd`をそのまま通す。xPackのQEMU 9.2の`emmc`は`-device`で挿せないので、WindowsのQEMU(11.0で確認)を使う。

### 3.5.5 SDカードのバスの速さ

SDカードは、UHS-I(1.8Vの信号でSDR104、DDR50、SDR50)、High Speed(3.3Vで50MHz)、既定の速さ(Default Speed、3.3Vで25MHz)の順に、使えるいちばん速いものにする。手順はRaspberry Pi OSのカーネル(github.com/raspberrypi/linuxのrpi-6.12.y)の`drivers/mmc/core`と`drivers/mmc/host`、SDの物理層の仕様、SDHCIの仕様に合わせた。全体の流れは`sdhci.c`の`sd_bringup`にあり、どこで失敗してもカードの電源を入れ直して3.3Vからやり直し、必ず使える状態で終わる。

#### スロットの能力と電源

Raspberry Pi 5で使う値と能力は、Raspberry Pi OSが同じカードスロットで使うものと同じにする。どれも起動時にデバイスツリーから読む(`sd_slot_probe`)。

| 項目 | 値 | 出典 |
| --- | --- | --- |
| UHS-Iのバスの速さ | SDR50、DDR50、SDR104。`no-1-8-v`があれば使わない | `bcm2712-rpi-5-b.dts`の`&sdio1`の`sd-uhs-sdr50`、`sd-uhs-ddr50`、`sd-uhs-sdr104`。SDR104があればSDR50もある(`sdhci.c`の`sdhci_setup_host`) |
| 基準クロックと上限 | 200MHz(`clk_emmc2`)。`max-frequency`があればそれを上限にする | `bcm2712.dtsi`、`sdhci_setup_host`(`f_max`) |
| 信号の電圧 | `sd-io-1v8-reg`(`regulator-gpio`)。常時オンのGPIOの3番が1で1.8V、0で3.3V。切り替えた後5ms待つ(`regulator-settling-time-us`) | `bcm2712-rpi-5-b.dts`、`drivers/regulator/gpio-regulator.c` |
| カードの電源 | `sd-vcc-reg`(`regulator-fixed`)。常時オンのGPIOの4番が1で入る(`enable-active-high`) | 同上 |
| クロックの経路 | UHS-Iを使うスロットでは、コントローラのリセットの前に`SDIO_CFG_MAX_50MHZ_MODE`(0x10_00ff_f400+0x1ac)のビット0を下ろしビット31を立て、チューニングで動かす遅延線を通す | `sdhci-brcmstb.c`の`sdhci_brcmstb_cfginit_2712` |
| ピンの選択 | クロックを書くたびに`SDIO_CFG_SD_PIN_SEL`(+0x44)をSDにする | `sdhci_bcm2712_set_clock` |
| ドライバの強さ | タイプB(ホスト制御2のビット5〜4を0)。カードへCMD6の関数グループ3は送らない | brcmstbは`select_drive_strength`を持たない(`core.c`の`mmc_select_drive_strength`、`sd.c`の`sd_select_driver_type`) |
| 電流の上限 | SDR50、SDR104、DDR50でだけ、ホストの最大電流のレジスタ(0x48のビット7〜0、4mA単位)とカードが共に持つ800、600、400、200mAの大きいものをCMD6の関数グループ4で設定する。レジスタが0なら送らない | `sd.c`の`sd_set_current_limit`、`sdhci_setup_host` |
| XPC | ホストの最大電流が150mAを超えればACMD41でXPCを立てる | `sd.c`の`mmc_sd_get_cid` |
| クロックの値 | UHS-Iでは、そのバスの速さのプリセット値のレジスタ(0x66〜0x6E)の分周を使い、プリセット値を有効にする(ホスト制御2のビット15)。プリセット値が示すクロックがバスの速さの上限(SDR104は208MHz、SDR50は100MHz、DDR50は50MHz)を超えるときは使わず、上限以下になる分周を自分で求める | `sdhci.c`の`sdhci_set_ios`、`sdhci_calc_clk`。brcmstbは`SDHCI_QUIRK2_PRESET_VALUE_BROKEN`を立てない。上限の確認はTessronOSが足したもの |
| Auto-CMD23 | 7.3 | `sdhci_setup_host` |

CM5 Liteのデバイスツリーも同じ2つのレギュレータを持つので、同じ手順になる(実機では未確認)。CM5のeMMCは1.8V固定で、UHS-Iの手順は使わない。Pi 4系とQEMUはUHS-Iを試みない(試験だけは`knl_sd_reinit`で試みさせる)。

カードの電源の入れ直し(`sd_power_cycle`)は、Linuxの`mmc_power_cycle`(`mmc_power_off`、1ms、`mmc_power_up`)の順にする。

1. 切る。割込みの信号を止め、コントローラをリセットし、クロックを止め、4番を0にしてカードの電源を切り、コントローラのバスの電源も切る。1ms待つ。
2. さらに1ms待つ。
3. 入れる。4番を1にし、バスの電源を3.3Vで入れ、信号を3.3Vにする(ホスト制御2の1.8Vのビットを下ろし、3番を0にし、変わったなら5ms、さらに5ms待って、ビットが下りたままであることを確かめる)。10ms待つ。
4. 初期化(`sd_init_card`)が400kHzのクロックを出してから10ms待ち、CMD0から始める。

#### 1.8Vへの切り替え

UHS-Iを使えるスロットでは、ACMD41でS18R(ビット24)を立てる。カードの応答にS18Aがあれば、CMD2の前に切り替える(`sd_uhs_voltage`、Linuxの`mmc_set_uhs_voltage`と`mmc_host_set_uhs_voltage`、`sdhci_start_signal_voltage_switch`)。

1. CMD11を送る。応答がなければ電源を入れ直して、1.8Vを求めずにやり直す。応答のR1にエラーがあれば、カードは3.3Vのままなので、電源は入れ直さずに1.8Vを求めずにやり直す。
2. 1ms後にDAT[3:0]がすべてLowであることを確かめる(PRESENT_STATEのビット23〜20)。
3. クロックを止め、3番を1にして5ms待ち、ホスト制御2の1.8Vのビットを立てて、立ったままであることを確かめる。
4. クロックを止めたまま10ms待ち(仕様は5ms)、400kHzのクロックを出し直す。
5. 1ms後にDAT[3:0]がすべてHighであることを確かめる。

2、3、5で失敗したら、電源を入れ直して1.8Vを求めてやり直す。これを10回まで繰り返し、それを超えたら1.8Vを求めない(`mmc_sd_get_cid`の`retries`)。LinuxはDAT0だけを見る(`sdhci_card_busy`)が、TessronOSはSDの物理層の仕様とSDHCIの仕様のとおり4本とも見る。食い違ったときは`TessronOS: sd after CMD11 DAT[3:0] ...`か`TessronOS: sd at 1.8V DAT[3:0] ...`と4本の値を出す。

前にカードを使っていた側が1.8Vのまま電源を切らずに渡したカードは、3.3VのままCMD6の照会でUHS-Iのバスの速さを答える(3.3Vではビット2〜4が0)。そのときはホストの側だけを1.8Vにして次の手順を一度だけ試し、失敗すれば電源を入れ直して最初からやり直す(`sd.c`の`mmc_sd_init_card`の`mmc_sd_card_using_v18`)。

#### UHS-Iのバスの速さ

1.8Vに切り替えたカードは、SDR12(25MHz)で4ビットのバスにした後、次の順にする(`sd_uhs_card`、`sd.c`の`mmc_sd_init_uhs_card`)。LinuxはSCRとCMD6の照会を400kHzの1ビットで読むが、TessronOSは3.3Vのときと同じく25MHzの4ビットにしてから読む。どちらもSDR12の範囲である。

1. ACMD51でSCRを、CMD6の照会でスイッチの状態を読む(`sd_switch_info`)。
2. バスの速さを選ぶ。ホストとカード(SD_SPEC3のカードのCMD6の13バイト目)が共に持つものから、SDR104、DDR50、SDR50、SDR25の順にとる(`sd_update_bus_speed_mode`)。SDR12しかないカードは、3.3VのHigh Speedより遅いので失敗とする。
3. カードの先頭の32KBをデータポートで読んでおく。
4. 電流の上限を設定する(上の表)。
5. CMD6の切り替えのモードで関数グループ1をバスの速さにし、状態の16バイト目の下位4ビットがそれであることを確かめる。Linuxは違っていても警告だけでSDR12に留まるが、TessronOSは失敗とする。
6. ホストを合わせる(`sd_host_timing`、`sdhci_set_ios`の順)。カードへのクロックを止め、25MHzを超えるバスの速さではHigh Speed Enableを立て、ホスト制御2のドライバの強さをタイプBに、UHSのモード(SDR50は2、SDR104は3、DDR50は4、SDR25は1)を設定し、プリセット値を有効にしてクロックを出す(上の表)。
7. チューニング(`sd_execute_tuning`、`sdhci.c`の`sdhci_execute_tuning`と`__sdhci_execute_tuning`)。SDR104とDDR50、およびコントローラの能力のレジスタ(0x44)のビット13が立つときのSDR50で行う。ホスト制御2のExecute Tuning(ビット6)を立て、割込みの許可をBuffer Read Readyだけにして、ブロックの大きさ64バイトのCMD19を送り、Buffer Read Readyを50msまで待つ。データは読み出さない。1ms待ってExecute Tuningが下りていれば、Sampling Clock Select(ビット7)が立っていれば成功、下りていれば失敗とする。40回までに終わらなければ失敗とし、両方のビットを下ろす。ブロックが来なければCMDとDATをリセットして打ち切る。LinuxはSDR104の失敗でも固定のサンプリング点で続けるが、TessronOSは失敗とする。DDR50の失敗はLinuxと同じく続ける。
8. 3と同じ32KBを読み直して一致することと、CMD13でカードが転送の状態にあり、データを受け付けることを確かめる。

2〜8のどこかで失敗したら、電源を入れ直し、1.8Vを求めずに3.3Vで初期化して、次の節のHigh Speedへ進む。

使っている間は、Linuxと同じく2つの場合にチューニングをやり直す(`core.c`の`mmc_request_done`、`host.c`の`mmc_retune`)。1つはCRCかエンドビットかインデックスの誤りで転送が失敗したとき(その転送を一度だけ送り直す)、もう1つはコントローラの能力のレジスタの再チューニングのタイマ(ビット11〜8が0でなく、モードのビット15〜14が0)が、2^(n-1)秒(nはビット11〜8の値)たったときである。チューニングし直しても失敗した転送と、チューニングそのものの失敗は、電源を入れ直して3.3Vに戻し(`sd_give_up_uhs`)、その転送を送り直す。

#### High Speed

3.3Vのカードは、既定の速さで4ビットのバスにし、続けてHigh Speedへの切り替えを試みる(`sd_high_speed`)。手順はSDの物理層の仕様のSwitch Functionに沿い、LinuxとU-Bootのドライバと同じ順にしてある。

1. ACMD51でSCR(8バイト)を読む。SD_SPEC(先頭のバイトの下位4ビット)が1以上で、CSDのコマンドクラス(CCC、ビット95〜84)にクラス10(switch)があれば、CMD6を使える。
2. CMD6の照会のモード(引数`0x00FFFFF0`)で64バイトの状態を読み、関数グループ1が使えるものを見る。13バイト目のビット1がHigh Speedである。関数が使用中(29バイト目のビット1)なら4回まで聞き直す。
3. 既定の速さのまま、カードの先頭の32KBをデータポートで読んでおく。
4. CMD6の切り替えのモード(引数`0x80FFFFF1`)で関数グループ1をHigh Speedにする。応答のR1にエラーのビットがなく、状態の16バイト目の下位4ビットが1であることを確かめる。
5. ホストの側を合わせる。カードへのクロックを止めてから、ホスト制御のHigh Speed Enable(ビット2)を立て、仕様のバージョンが3.00以上のコントローラではホスト制御2のUHSのモードを0に、ドライバの強さをタイプB(0)にする。クロックを25MHzで出し直してから50MHzにする(Linuxの`sdhci_set_ios`の順)。
6. 3と同じ32KBを読み直して一致することと、CMD13でカードが転送の状態にあり、データを受け付けることを確かめる。

どれかの段で失敗したら、ホストのHigh Speed Enableを下ろしてクロックを25MHzに戻し、4まで進んでいればCMD6で関数グループ1を既定(関数0)に戻して、既定の速さのまま使う。そのときはR1、PRESENT_STATE、INT_STATUSをコンソールに出す。

#### コンソールの表示と終わり方

決まった速さは1行で出す。クロックの値は基準クロックを分周した実際の値である(QEMUのsdhci-pciは基準クロックが52MHzなので、High Speedは26MHz)。

- `sda: SD card, UHS-I SDR104 200 MHz, tuned`
- `sda: SD card, High Speed 50 MHz; UHS-I not used: <理由>`(理由の例: `the card did not accept 1.8V (no S18A)`、`the tuning (CMD19) failed`)
- `sda: SD card, default speed 25 MHz: <High Speedでない理由>; UHS-I not used: <理由>`

UHS-Iを試みるスロットでは、その前に`TessronOS: sd UHS-I on the slot: ...`の行に、デバイスツリーから読んだバスの速さとピン、能力のレジスタ、最大電流、バージョン、`SDIO_CFG_MAX_50MHZ_MODE`の前後の値を出す。プリセット値でクロックを出したときは`TessronOS: sd the clock from the preset value ...`も出す。

シャットダウンと再起動の前には、1.8Vのカードの選択を外し(CMD7、引数0)、電源を切って信号を3.3Vに戻す(`knl_sd_finish`、`devinit.c`の`knl_finish_device`から。Linuxの`mmc_sd_suspend`)。ファームウェアが次に起動するとき、カードを電源を入れた直後と同じ状態で見つけるためである。Linuxは3.3Vのカードも電源を切るが、TessronOSは3.3Vのカードを、これまで実機で確かめたとおりファームウェアが渡したまま電源を入れておく。

DMAの方式の検査(`sd_dma_check`)は切り替えた後の速さで行う。QEMUのsd-cardは電圧の切り替えとチューニングの実体を持たないので、QEMUで試せるのは戻り方だけである(3.5.4)。1.8Vへの切り替え、チューニング、プリセット値、Auto-CMD23の本当の動作は実機でしか確かめられない(実機で未確認)。

## 3.6 Raspberry Pi 4、Raspberry Pi 400、Compute Module 4

Raspberry Pi 4 Model B、Raspberry Pi 400、Compute Module 4(以下CM4)は、BCM2711を載せたボードである。CPUがARMv8.0-AのCortex-A72なので、ARMv8.2-Aで作るRaspberry Pi 5のイメージとは別に、`make TARGET=_RPI4_`でカーネルのイメージを作る。3つのボードは同じイメージで動き、ボードの違いはCM5と同じ仕組み(3.5.1)で起動時に見分けて吸収する。アドレスと割込み番号は、ファームウェアの配布物の`bcm2711-rpi-4-b.dtb`、`bcm2711-rpi-400.dtb`、`bcm2711-rpi-cm4.dtb`とBCM2711のペリフェラルの資料で確かめたもので、`include/sys/sysdepend/cpu/bcm2711/sysdef.h`にある。ボードに依存するコードは`kernel/sysdepend/rpi4/`に、Raspberry Pi 5と共通のメールボックス、時計、乱数は`kernel/sysdepend/rpi/`にある。

実機では、Raspberry Pi 400で、起動、ボードの見分け、SDカード(EMMC2のHigh Speed)、画面(ファームウェアのフレームバッファ)、サウンドの初期化までが進むことを確かめた(2026年10月)。その先のPCIeの初期化で止まった件は3.6.4で直したが、その先はまだ確かめていない。QEMUの`raspi4b`で確かめられる範囲は確かめ(3.6.6)、PCIe、VL805、GENET、GPIOのように実機でしか確かめられないものは、資料とデバイスツリーの値に基づいて書いた。

### 3.6.1 差異

| 項目 | Raspberry Pi 5 | Raspberry Pi 4系 | 扱い |
| --- | --- | --- | --- |
| CPU | Cortex-A76 ×4。ARMv8.2-Aで、LSEのアトミック命令と暗号拡張がある | Cortex-A72 ×4。ARMv8.0-Aで、LSEのアトミック命令も暗号拡張もない(CRC32はある) | 命令セットの基準を`-march=armv8-a+crc`にした別のイメージを作る(3.6.2) |
| 起動 | EEPROMのブートローダがカーネルを読み込む | EEPROMのブートローダがSDカードの`start4.elf`と`fixup4.dat`を読み込み、それがカーネルを読み込む | SDカードのFATパーティションにファームウェアのファイルを置く(3.6.5) |
| 起動時のEL | EL2 | EL2(ファームウェアに組み込まれたarmstubが入れる) | `boot.S`のまま |
| 2つめ以降のCPU | PSCIの`CPU_ON`(TF-A) | スピンテーブル。CPU1〜3がそれぞれ物理`0xe0`、`0xe8`、`0xf0`を見て待つ。PSCIはない | デバイスツリーの`enable-method`でPSCIとスピンテーブルを選ぶ(3.6.3) |
| 汎用タイマ | 54MHz | 54MHz | `CNTFRQ_EL0`を読む |
| RAM | 物理0から。1GB直下にファームウェアの領域 | 1、2、4、8GB。物理0からGPUの領域の手前まで、`0x40000000`から`0xFC000000`まで、8GBではさらに`0x1_00000000`から`0x2_00000000`まで。`0x0`〜`0x1000`はarmstubとスピンテーブル | 範囲はデバイスツリーの`/memory`と`/memreserve/`から取る |
| 周辺機器 | `0x10_0000_0000`以降と、PCIeの先のRP1のアドレスウインドウ | `0xFC000000`〜`0xFF7FFFFF`、ARMのローカル周辺機器は`0xFF800000`以降(low peripheralモード) | RAMの4つめの1GBに周辺機器があるので、RAMが1GBを埋めないところは2MB単位でリニアマップに載せる |
| DMAの届く範囲 | RP1の先のバスマスタは内向きのアドレスウインドウを通してメモリ全体に届く | VideoCore(メールボックス、フレームバッファ)とEMMC2の初期のリビジョン(B0)は1GBまで、PCIeの先は3GBまで、GENETはメモリ全体 | DMAに使うページを1GB未満から取る(`DMA_ZONE_LIMIT`)。EMMC2のバスアドレスはデバイスツリーの`dma-ranges`から求める |
| 割込み | GIC-400 | GIC-400。ディストリビュータ`0xFF841000`、CPUインタフェース`0xFF842000` | GICのドライバのまま |
| コンソール | 3ピンのデバッグUART(PL011) | 40ピンヘッダの8番と10番のPL011(UART0、`0xFE201000`、SPI 121)。既定ではBluetoothが使っているので、`dtoverlay=disable-bt`で取り返す | USBシリアル変換器でつなぐ |
| SDカード | SDHCI(SPI 273) | EMMC2(`0xFE340000`、SPI 126) | `sdhci.c`を使う。基準クロックはメールボックスで問い合わせる |
| USB | RP1のxHCI 2つ | PCIeの先のVL805(xHCI、4ポート)。ルートコンプレックスは`0xFD500000`、INTAはSPI 143。USB-CのポートのDWC2は、機器がつながっているときだけ使う(3.6.7) | ルートコンプレックスをカーネルが初期化し、VL805のプログラムの読込みをメールボックスで依頼する。xHCIから上のUSBスタックは共通 |
| Ethernet | RP1のCadence GEMとBCM54213PE | GENET v5(`0xFD580000`、SPI 157と158)とBCM54213PE(RGMII) | GENETのドライバ`eth_genet.c`を新しく作った。PHYは標準のレジスタとBroadcomのクロック遅延のレジスタだけで扱う |
| 画面 | ファームウェアのフレームバッファ | 同じ(KMSのオーバーレイは読み込ませない) | `disp_rpi5.c`を両方で使う |
| サウンド | USBオーディオ、RP1のI2S、HDMIの音声(6.5) | USBオーディオ。ほかにI2S、PWMによるアナログ出力、HDMIの音声がある | USBオーディオだけにする |
| GPIO | 常時オンのGPIOとRP1のGPIO | BCM2711のGPIO(`0xFE200000`、58本)。ACT LEDはGPIO 42 | `gpio_bcm2711.c`と、GPIOの実身のバンクの表(3.6.4) |
| GPU | V3D 7.1 | V3D 4.2 | 対象外 |
| 時計 | ファームウェアに問い合わせる | RTCはない | SNTPで合わせる |
| 乱数 | iproc-rng200 | iproc-rng200(`0xFE104000`) | 同じドライバを使う |
| 再起動と電源断 | PSCI | PSCIはない | 再起動はPMのウォッチドッグで行う。電源断の代わりにファームウェアの停止状態に入る(3.6.3) |

ボードは次のとおり見分ける。どれもSDHCIの先にカード検出線がない(デバイスツリーの`broken-cd`)ので、カードを初期化してみて見つける。

| ボード | ルートノードの`compatible` | リビジョンコードのボードの種類 | ボードの表の値 |
| --- | --- | --- | --- |
| Raspberry Pi 4 Model B | `raspberrypi,4-model-b` | 0x11 | SDカード4ビット、PHYのMDIOのアドレス1 |
| Raspberry Pi 400 | `raspberrypi,400` | 0x13 | 4Bと同じ。GPIO 42のLEDは電源LEDなので、点滅の後は点けたままにする |
| CM4(eMMCあり、Lite) | `raspberrypi,4-compute-module` | 0x14 | eMMCは8ビット(LiteではIOボードのSDカード)、PHYのMDIOのアドレス0 |

### 3.6.2 命令セットと外部の生成物

`TARGET=_RPI4_`では、カーネル、プログラム、ライブラリのすべてを`-march=armv8-a+crc -mno-outline-atomics`でコンパイルする。ARMv8.0-AではGCCのアトミック操作の既定がlibgccの関数(`__aarch64_cas4_acq_rel`など)の呼出しになり、libgccをリンクしない小物がリンクできないので、排他ロードとストアの命令で直に作らせる。プログラム、実身のレコード、それらがリンクするライブラリはRaspberry Pi 5のものと混ざらないように`obj/v80/`に置く(`build_make/common.mk`の`UOBJ`)。

外部で作るオブジェクトも同じ基準で別に作る。NetBSDのスタックは`MARCH=armv8-a+crc OUT=~/tessronos-netbsd-v80 bash tools/netbsd/build.sh all`で、Mozcは`MARCH=armv8-a+crc OUT=~/tessronos-mozc-v80 bash tools/mozc/build.sh all`で作る。`TARGET=_RPI4_`のときmakeはこの2つの置き場を見る。NetBSDのスタックはリンクの前になければmakeが作る。ブラウザ(Blink)はARMv8.2-Aでしか作っていないので、`BROWSER=1`はエラーにする。

ARMv8.0-Aにない命令が残っていないことは`tools/chk_isa.sh`で確かめる。オブジェクトやイメージの命令を一つずつ調べ、LSEのアトミック命令、`ldapr`、内積、暗号拡張の命令を探す。libgccのアトミック操作の関数(`__aarch64_cas4_acq`など)は両方の形を持って実行時に選び、このシステムでは常にLSEでない方を選ぶので、別に数える。NetBSDのスタックとMozcのARMv8.0-A向けのビルド、Raspberry Pi 4のイメージには見つからない(ARMv8.2-AのMozcには352個ある)。QEMUのCortex-A72はLSEの命令を持たないので、残っていればQEMUでも未定義命令の例外になる。暗号拡張はQEMUのCortex-A72が持つと答えるので、確かめられるのはこの検査だけである。

### 3.6.3 起動とメモリ

ファームウェアはイメージを`0x80000`に置き、EL2でCPU0だけを実行する。`boot.S`は、イメージをリンクアドレス`0x200000`へコピーする前に、`MIDR_EL1`の部品番号がそのイメージの対象のCPU(Raspberry Pi 4のイメージではCortex-A72の0xD08、Raspberry Pi 5のイメージではCortex-A76の0xD0B)であることを確かめる。違えば、もう一方のボードのコンソール(Raspberry Pi 4のイメージはRaspberry Pi 5のデバッグ用UART、Raspberry Pi 5のイメージはRaspberry Pi 4のUART0)に、このイメージがどのボード向けかを1行書いて止まる。続いて`board.c`が、デバイスツリーのルートノードの`compatible`にそのイメージのSoC(`brcm,bcm2711`か`brcm,bcm2712`)があることを確かめ、なければ同じように止まる。

イメージは`.data`までがフラットなイメージの範囲で、`boot.S`はその範囲をコピーする。リンカが`.data`の後ろに置いていたGOT(NetBSDのスタックがリンクセットの終わりを参照する)がコピーされず、コピーした先でゼロのままになって、NetBSDのスタックの初期化がNULLを参照して止まっていた。リンカスクリプトでGOTを`.data`に入れて直した。イメージが`0x80000`に置かれるRaspberry Pi 5でも同じことが起きていた。

2つめ以降のCPUは、デバイスツリーの`/cpus/cpu@*`の`enable-method`が`spin-table`なら、`smp.c`がその`cpu-release-addr`にエントリの物理アドレスを書き、キャッシュをフラッシュして`sev`で起こす。ファームウェアのスピンテーブルは引数を渡さないので、エントリ(`knl_secondary_spin_entry`)で`MPIDR_EL1`のAff0をプロセッサの番号とし、Aff0がデバイスツリーの並びと一致しないCPUは起こさない。

メモリの範囲はデバイスツリーの`/memory`から取り、ヘッダの`/memreserve/`の範囲(`0x0`〜`0x1000`)はページアロケータに渡さない。リニアマップは1GBのブロックで作っていたが、RAMが1GBを埋めないところは2MBのブロックの表(`mmu.c`の`edge_l2`)で、RAMのある部分だけを載せる。`0xC0000000`〜`0xFC000000`のRAMの後ろの周辺機器を通常のメモリとして載せると、CPUが先読みでレジスタを読むことがあるためである。DMAに使うページのゾーンは1GB未満とし(`DMA_ZONE_LIMIT`)、VideoCore、EMMC2、PCIeのどれもが届くページを`KAF_DMA32`で取る。

再起動は、PMのウォッチドッグに数ティック後の全体リセットを設定して行う。電源を切る手段はないので、システムの終了では、リセットの状態のレジスタにブートパーティション63を書いてからリセットする。ファームウェアはそれを停止の指示として受け取り、CPUを止めたまま電源の入れ直しを待つ。

### 3.6.4 デバイス

| デバイス | ドライバ | 内容 |
| --- | --- | --- |
| コンソール | `device/ser/ser.c` | UART0(PL011)。クロックはメールボックスで問い合わせ、答えがなければ48MHz |
| メールボックス、時計、乱数 | `kernel/sysdepend/rpi/` | Raspberry Pi 5と共通。時計のタグには答えがないのでSNTPで合わせる。乱数生成器はデバイスツリーのノードが無効なら使わない |
| 画面 | `device/disp/disp_rpi5.c` | ファームウェアが用意するフレームバッファ。3.5インチの液晶は3.6.7 |
| SDカード、eMMC | `device/sd/sdhci.c` | EMMC2。下を参照 |
| GPIO | `device/gpio/gpio_bcm2711.c` | 58本を1つのバンク0にGPIOの番号で並べる。プルの設定は読み返せる |
| PCIe | `kernel/sysdepend/rpi4/pcie_rc.c` | ルートコンプレックスを初期化し、`kernel/sysman/pcie.c`の走査に設定空間の読み書きを渡す。下を参照 |
| USB | `device/usb/` | PCIeの先のxHCI。INTA(SPI 143)で割込みを受ける。`make PCIE_POLL=1`で一定の間隔で見る。USB-CのポートのDWC2(`device/usb/dwc2.c`)は、起動時に機器が見つかったときだけ使う(3.6.7) |
| Ethernet | `device/net/eth_genet.c` | GENET v5。下を参照 |

EMMC2の基準クロックは能力レジスタに書かれていないので、メールボックスでEMMC2のクロック(番号12)の周波数を問い合わせる。EMMC2が見るメモリは、デバイスツリーの`/emmc2bus`の`dma-ranges`で求める。初期のリビジョン(B0)では物理アドレスの1GBまでがバスアドレス`0xC0000000`からに見え、後のリビジョン(C0)ではファームウェアがこれを書き換え、周辺機器の手前までがそのままのアドレスで見える。`dma-ranges`がなければB0の値を使う。届かないバッファはバウンスバッファ(256KB、DMAのゾーンから取る)を通す。ADMA2の記述子の表もバスアドレスで渡す。CM4ではボードの表の8本のデータ線を使ってeMMCを8ビットのバスに切り替える。信号電圧はファームウェアが3.3Vのまま渡すので、どのボードでも1.8Vには切り替えない。

GPIOの実身は、BCM2711のピンを3つのバンクにして見せる。`gpio-0`は40ピンヘッダのGPIO 0〜27、`gpio-1`はGPIO 28〜45、`gpio-2`はGPIO 46〜57である。ピンはGPIOの番号で書く。コンソールのUART0のピン(14、15)と、ボードの配線(EthernetのPHYのMDIOとRGMII、BluetoothのUART、Wi-FiのSDIO、オーディオのPWM、SPIフラッシュ)に使うGPIO 28〜57は、ACT LEDのGPIO 42を除いてカーネルのものとし、実身から変更しない。ACT LEDは高で点く。

PCIeのルートコンプレックスは、ファームウェアがブリッジをリセットしたまま渡す。EEPROMのブートローダがUSBからの起動を試したときは、VL805を使った後でもう一度リセットする(起動時のコンソールの`XHCI-STOP`と`PCI0 reset`)。ブリッジがリセットされている間にルートポートの設定空間を読むと、非同期の外部アボート(SError)になる。そこで、リセットのレジスタ(`0x9210`)のほかは、ブリッジをリセットから出してSerDesの電源を入れるまで触らないように、`kernel/sysdepend/rpi4/pcie_rc.c`は次の順に初期化する。

1. デバイスツリーの`/scb/pcie@7d500000`が有効なことを確かめ、`ranges`と`dma-ranges`からアドレスウインドウの値を取る。レジスタはまだ読まない。
2. リセットのレジスタを読み、ブリッジ(ビット1)と先のデバイスのPERST#(ビット0)をリセットに入れ、1ミリ秒待ってブリッジだけを出す。
3. SerDesの電源を入れ(`0x4204`のIDDQのビットを落とす)、1ミリ秒待つ。
4. ここで初めてルートポートの設定空間の先頭(ベンダとデバイスの番号)を読み、全ビット0か1ならルートコンプレックスがないとして終える。状態のレジスタ(`0x4068`)でルートコンプレックスとして動いていることも確かめる。
5. `0x4008`でブリッジからメモリへのアクセスを許し、存在しない機能の設定空間の読出しにエラーではなく全ビット1を返させ(`CFG_READ_UR_MODE`)、バーストを128バイトにし、内向きのアドレスウインドウ(PCIeの0から物理0へ。レジスタが2のべき乗しか取らないので4GBとし、外向きのアドレスウインドウと重なる上の1GBを除いた3GBが届く)を設定する。ブリッジ自身のほかの2つのウインドウは閉じ、コントローラ自身の割込みは止めて消す。
6. 外向きのアドレスウインドウ(物理`0x6_0000_0000`からPCIeの`0xC000_0000`へ1GB)、ルートポートのクラスコード(PCIのブリッジ)、内向きのアドレスウインドウのバイト順(リトルエンディアン)を設定する。
7. PERST#を解き、100ミリ秒待ってから、さらに100ミリ秒まで5ミリ秒ごとにリンク(`PHYLINKUP`と`DL_ACTIVE`)を見る。上がらなければUSBなしで続ける。
8. リンクが上がってから、ルートポートのブリッジの範囲を設定し、`kernel/sysman/pcie.c`の共通の走査がバスに番号を付けてBARを外向きのアドレスウインドウに割り当てる。リンクが上がる前と、ルートポートの下のバスのデバイス0以外は、設定空間を読まずに全ビット1を返す。
9. Raspberry Pi 4 Model BとRaspberry Pi 400では、VL805(1106:3483)の設定空間のバージョンのワード(0x50)が0なら、プロパティのタグ`NOTIFY_XHCI_RESET`(0x00030058)にVL805の設定空間のアドレス(バス1のデバイス0なら`0x100000`)を渡してファームウェアにプログラムを読み込ませ、10ミリ秒待つ。xHCIのドライバがBARの先のレジスタを触るのはこの後である。

設定空間は、ルートポートはレジスタの先頭に、その下のバスの機能は`0x8000`の4KBのウインドウに、`0x9000`の索引のレジスタに番号を書いて見る。Raspberry Pi 4 Model B(ボードのリビジョン1.4以降)とRaspberry Pi 400はVL805のプログラムをVL805自身ではなくボードのEEPROMに持つので、PCIeのリセットの後は手順9をしないとVL805のxHCIが動かない。CM4ではスロットに挿したカードが何かは分からないので、これをしない。汎用のxHCIのカードなら、VL805と同じ経路でUSBが使える。CM4のDWC2も、ほかのボードと同じく起動時に調べる(3.6.7。CM4では確かめていない)。

起動時のコンソールには、手順ごとにリセットのレジスタと状態のレジスタの値を出す(`pcie as found`、`pcie bridge out of reset`、`pcie serdes on`、`pcie root port id`、`pcie windows set`、`pcie far end out of reset`、`pcie link up`、`pcie found`、`pcie VL805`)。止まったときは最後の行で手順が分かる。

R1.100とR1.110のイメージは、手順1の直後、ブリッジをリセットから出す前にルートポートの設定空間を読んでいたため、Raspberry Pi 400で`> pcie`の直後にSError(ESR `0xbf000002`)で止まった。ELRとLRをそのビルドの関数に直すと、`knl_pcie_rc_init`の中の、2回目のデバイスツリーの読出しから戻った直後の`0xFD500000`の読出しの4命令後である。この読み方はボードによらないので、Raspberry Pi 4 Model BとCM4でも同じく止まる。

BCM2711のPCIeの初期化の手順を、Raspberry Pi OSのカーネル(rpi-6.6.y)の手順と比べると次のとおりである。

| 項目 | Raspberry Pi OS | TessronOS | 扱い |
| --- | --- | --- | --- |
| 最初のレジスタの読出し | ブリッジをリセットから出してSerDesの電源を入れるまで、リセットのレジスタのほかは触らない。リビジョンのレジスタは初期化の後で読む | R1.110までは、リセットの前にルートポートの設定空間を読んでいた | 手順4に移した(SErrorの原因) |
| リセット | ブリッジをリセットに入れ、PERST#も確かにリセットに入れ、100〜200マイクロ秒の後ブリッジだけを出す | 同じ。待ちは1ミリ秒 | そのまま |
| SerDes | IDDQを落とし、100〜200マイクロ秒待つ | 同じ。待ちは1ミリ秒 | そのまま |
| `MISC_CTRL` | `SCB_ACCESS_EN`、`CFG_READ_UR_MODE`、バースト128バイト(BCM2711の値0)、SCB0の大きさ。`RCB_MPS_MODE`はデバイスツリーに`brcm,enable-mps-rcb`があるときだけ | 同じ。Pi 4系のデバイスツリーには`brcm,enable-mps-rcb`がない | そのまま |
| 読み失敗の応答と待ちの時間 | UBUSの応答のエラーを止めるビット、UBUSの待ちの時間、設定空間の再試行の待ちの時間(`0x40a4`、`0x40a8`、`0x405c`)はBCM2712だけで設定する | BCM2711では設定しない(Pi 5の側`kernel/sysdepend/rpi5/pcie_rc.c`だけが設定する) | そのまま。BCM2711では`CFG_READ_UR_MODE`とリンクの確認で避ける |
| ルートコンプレックスとしての動作の確認 | リンクの前に状態のレジスタで確かめる | リンクの後で確かめていた | 手順4に移した |
| 外向きのアドレスウインドウ、クラスコード、バイト順 | リンクの前に設定する | リンクの後で設定していた | 手順6に移した |
| リンクの待ち | PERST#を解いて100ミリ秒、さらに5ミリ秒ごとに100ミリ秒まで | 同じ | そのまま |
| リンクの速さとSSC | デバイスツリーの`max-link-speed`があれば速さを制限し、`brcm,enable-ssc`があればSerDesのMDIOでSSCを有効にする(失敗しても続ける) | どちらもしない | 未対応。Pi 4系のデバイスツリーには`max-link-speed`がない |
| 設定空間の割当て | ルートバスはデバイス0だけ。リンクが上がっていなければ下のバスは読まない。下のバスは索引のレジスタ(`0x9000`)に書いて`0x8000`から読む | 同じ。さらにルートポートの直下のバスはデバイス0だけ | そのまま |
| 割込み | コントローラ自身のMSIの仕組みを使う | MSIは使わずINTA(SPI 143)をレベルトリガで受ける | そのまま |
| VL805の読込み | xHCIのドライバが動き出す前に、デバイスツリーのリセットの指定(`usb@0,0`の`resets`)を通して必ず`NOTIFY_XHCI_RESET`に`0x100000`を渡し、200〜1000マイクロ秒待つ。BIOSからの引継ぎの処理はPi 4では行わない | 4Bと400で、バージョンのワードが0のときだけ、走査の後、xHCIのドライバの前に依頼し、10ミリ秒待つ | そのまま |

GENETは、システム、RGMII、割込み、受信バッファ、UniMAC、受信と送信のDMAの部分からなる。DMAはそれぞれ256個の記述子(状態と長さ、アドレスの下位と上位の3ワード)と17本のリングを持ち、既定のリング(16番)だけを、受信と送信に128個ずつの記述子で使う。リングは、作る側と使う側の16ビットのカウンタの組で、受信ではコントローラがバッファを埋めて数え、ドライバがフレームを取り出して数え返す。受信したフレームはバッファの2バイト目から始まる。PHYのBCM54213PEはGENET自身のMDIO(UniMACの`0x614`、デバイスツリーの`mdio@e14`)にあり、アドレスはボードの表の値から試し、答えがなければ全アドレスを探す。デバイスツリーの`phy-mode`が`rgmii-rxid`なので、受信のクロックはPHYで、送信のクロックはGENETで遅らせる。PHYの受信の遅延はBroadcomの補助制御レジスタ(0x18)のシャドウ7、送信の遅延はシャドウレジスタ(0x1c)のシャドウ3で設定する。MACアドレスは、ファームウェアがデバイスツリーに書く`local-mac-address`、メールボックスのボードのMACアドレスの順に取る。割込みは使わず、Raspberry Pi 5のドライバと同じく、読み手が受信のリングを見る。すべてのフレームを受け取り、要らないものはプロトコルスタックが捨てる。

### 3.6.5 SDカードのイメージ

`make TARGET=_RPI4_ BUILD_ID=<名前> sdimg`で`build_make/obj/_RPI4_<名前>/sd.img`を作る。デスクトップとMozcを含むイメージは`make TARGET=_RPI4_ DESKTOP=1 MOZC=1 BUILD_ID=pi4 sdimg`で作る。パーティションの構成はRaspberry Pi 5のイメージ(3.3)と同じで、パーティション1のFAT32に次のファイルを置く。

| ファイル | 内容 |
| --- | --- |
| `start4.elf`、`fixup4.dat` | Raspberry Pi 4系のファームウェア |
| `bcm2711-rpi-4-b.dtb`、`bcm2711-rpi-400.dtb`、`bcm2711-rpi-cm4.dtb` | デバイスツリー |
| `overlays/disable-bt.dtbo` | UART0をBluetoothから取り返すオーバーレイ |
| `config.txt` | `etc/boot/config-rpi4.txt` |
| `tessronos.img` | カーネル |

ファームウェア、デバイスツリー、オーバーレイは`make sdimg`のときにRaspberry Piのファームウェアの配布物から`build_make/obj/boot/`に取得する。これらはBroadcomのライセンスのもとでバイナリとしてだけ再配布できるものなので、リポジトリには含めない。`RPI4_FW=<ディレクトリ>`で手元のファームウェアの`boot`ディレクトリを指すこともできる。`tools/mksd.py`と`tools/mkfat.py`は、`overlays/disable-bt.dtbo=<ファイル>`のように`名前=ファイル`で名前を変え、名前の`/`の前をルートの下のディレクトリとして置く。

`config.txt`は次のとおりである。

```text
[pi4]
arm_64bit=1
kernel=tessronos.img
kernel_address=0x200000
device_tree=bcm2711-rpi-4-b.dtb
dtoverlay=disable-bt
enable_uart=1
uart_2ndstage=1
framebuffer_depth=32
framebuffer_ignore_alpha=1

[pi400]
device_tree=bcm2711-rpi-400.dtb

[cm4]
device_tree=bcm2711-rpi-cm4.dtb
[all]
```

`[pi4]`の条件はRaspberry Pi 4 Model B、400、CM4のすべてに当てはまり、Raspberry Pi 5には当てはまらない。このカードをRaspberry Pi 5に挿すと、ファームウェアはカーネルを見つけられずに止まる。後から読んだ`device_tree`が前の指定を置き換えるので、ボードごとに合ったデバイスツリーが読み込まれる。カーネルとデバイスツリーと`config.txt`だけを書き込んだカードの上で差し替えるには`make TARGET=_RPI4_ sd SD=<FATパーティション>`を使う。パーティションテーブルはGPTなので、GPTから起動できるバージョンのEEPROMのブートローダ(2020年9月以降)が要る。

### 3.6.6 試験

QEMUの`-M raspi4b`(xPackの9.2で確認)は、GIC-400、PL011、メールボックスとフレームバッファ、EMMC2と古いEMMC、PMのウォッチドッグをエミュレートし、PCIe、GENET、乱数生成器を持たない。QEMUはデバイスツリーのそれらのノードを`disabled`にして渡すので、カーネルはそれを見て使わない(`knl_dtb_okay`)。CPUはEL2で始まり、2つめ以降のCPUはQEMUのスピンテーブルで待つ。QEMUはSDカードを古いEMMC(`0xFE300000`)に繋ぎ、EMMC2を空のままにするので、QEMUではコマンドラインの`tessronos.sd=emmc`でSDカードのドライバを古いEMMCに向ける。実機では古いEMMCはWi-Fiに繋がっているので、この指定はしない。Wi-Fi(`WIFI=1`。09章9.6.8)は、この指定があるときはホストに触らない。

| makeのターゲット | 内容 |
| --- | --- |
| `make TARGET=_RPI4_ BUILD_ID=<名前> qemu-rpi4` | SDカードのイメージを作り、QEMUの`raspi4b`で起動する(Ctrl-A Xで終了) |
| `make TARGET=_RPI4_ KTEST=1 BUILD_ID=<名前> test-rpi4` | カーネル内の試験を走らせ、`KTEST SUMMARY`を待つ |

QEMUにはカーネルのイメージ`tessronos.img`を`-kernel`で、デバイスツリーを`-dtb`で、SDカードのイメージを2のべき乗の大きさに伸ばしたコピーを`-drive if=sd`で渡す。ファームウェアと同じく、イメージは`0x80000`に置かれてから`0x200000`へコピーされる。QEMUの`raspi4b`では、起動、4つのCPU、タイマ、割込み、コンソール、SDカード(ただし古いEMMCで、PIOでの転送)、画面、ウォッチドッグによるリセット、誤ったボードのイメージの検出(Raspberry Pi 5のイメージを`raspi4b`で起動すると、UART0にその旨の1行が出て止まる)を確かめた。RAMが1GBの途中で終わるときのリニアマップは、QEMU virtを`-m 1100M`で起動して確かめた。デスクトップとMozcの構成も起動し、かな漢字変換が始まるところまで確かめた。カーネル内の試験は、`kernel`の群が68件すべて、`fs`の群が134件中133件通った(2026年10月1日)。通らなかった1件(`test_calendar`)は、試験の前にシステムの時差が設定されていると失敗するもので、QEMU virtでも同じ条件で失敗する。試験の`#ifdef RPI5`は、「QEMUではなくボード」の意味のものを`KT_BOARD`(Raspberry Pi 5か4のとき1)に置き換えた。

デスクトップの構成で起動すると、起動の後でデバイスの実身とボードの確認(`kernel/sysdepend/rpi4/objtest.c`、`objtest:`の行)が走る。1枚のカードで多くを確かめられるように、Raspberry Pi 5の確認(バンク、LED、カーネルのピン、入力の変化、温度とクロック、画面、USBの実身、乱数)に、ボード(4つのCPU、メモリの範囲、DMAのゾーン)、SDカードの転送の方式、PCIeのリンクとその先の機能、Ethernetのリンクを加えた。カーネル内の試験の構成では走らせない。実機では次の項目を確かめる。

| 項目 | 確かめ方 | 期待する結果 |
| --- | --- | --- |
| 起動、ボードの見分け | シリアルコンソール | `board Raspberry Pi 4 Model B`(400、CM4ではそれぞれの名前)、リビジョンが表示される |
| メモリ | 起動時の`RAM[n]`の行と`objtest: board` | 8GBでは3つか4つの範囲。`0xFC000000`より上を読み書きして止まらない |
| 4つのCPU | `4 of 4 CPUs online` | スピンテーブルから4つとも起きる |
| SDカード | `sda`の行と`objtest: sd` | ADMA2かSDMAで転送している(`data moved by`がデータポートでない)。B0とC0の両方の基板で`sd EMMC2 sees memory`の値がデバイスツリーと合う |
| CM4のeMMC | `sda eMMC ... 8 data lines` | 8ビットに切り替わる |
| 画面 | HDMI | デスクトップが出る |
| PCIeとVL805 | `pcie link up`、`pcie VL805 at ...`、`objtest: pcie` | リンクが上がり、VL805(1106:3483)が見つかり、プログラムの読込みの依頼が0を返す |
| USB | キーボードとマウスを挿す | xHCIが4ポートで上がり、入力が届く。割込みの数が増える(INTAが届く) |
| Ethernet | `genet: ... phy 1 up`、`objtest: ethernet` | PHYが見つかり、リンクが上がり、DHCPでアドレスを得て、SNTPで時計が合う |
| GPIO | `objtest: gpio banks`、`gpio events` | 3つのバンク、GPIO 26のプルの変化が通知される |
| LED | 目で見る | 起動中に2回と3回点滅する。400では最後に点いたままになる |
| 温度とクロック | `objtest: temperature and clocks` | 温度とARMのクロックが表示される |
| 乱数 | `objtest: random` | rng200から値が取れる |
| 再起動と停止 | システムの終了と再起動の操作 | 再起動でファームウェアから起動し直す。終了ではファームウェアが停止状態に入る |
| 命令セット | 全体の動作 | 未定義命令の例外が出ない(`tools/chk_isa.sh`で検査済み) |
| 3.5インチの液晶とタッチパネル | 普通のPi 4のカード(PocketTerm35)か`make LCD=dpi35`のカード、`tpanel:`と`gt911`の行 | 640×480で出る。`Goodix GT911 ... at 14`の行が出て、指でポインタが動き、タップがクリック、長押しがメニューになる(3.6.7) |
| PocketTerm35の本体のキーボード | 普通のカード、`dwc2: probing`と`usb: hid:`の行 | `dwc2: probing: a device at ... ms`のあとキーが打てる。機器がなければ`nothing in 3000 ms: put back`(3.6.7) |

### 3.6.7 3.5インチの液晶とタッチパネル

Raspberry Pi 4に載せるWaveshareの3.5インチの液晶(640×480)を使えるようにした。実機では確かめていない。仕様はWaveshareのwikiと、Waveshareが配るデバイスツリーのオーバーレイ(dtbo)を逆コンパイルした値による。

| 製品 | 画面 | タッチ | オーバーレイの値 |
| --- | --- | --- | --- |
| PocketTerm35(Pi 4とPi 5の手持ちの端末)と、それに載る3.5inch HDMI LCD (E) | HDMI。キットにmicro HDMIのアダプタが付く | GT911。I2C1のピン(GPIO 2がSDA、3がSCL)、INTはGPIO 4(立下り)、アドレス0x14。リセットのピンはない | `3.5HDMI_E_DTBO.zip`の`waveshare-35dpi-4b.dtbo`。Pi 5用の`waveshare-35dpi-5b.dtbo`は0x14と0x5Dの両方 |
| 3.5inch DPI LCD | DPI(RGB666)。GPIO 0〜9、12〜17、20〜25をALT2にする | GT911。ピンを動かすI2C(`i2c-gpio`)で、GPIO 10がSDA、11がSCL、待ち4マイクロ秒。INTはGPIO 27(立下り)。アドレスは0x14か0x5D。X軸を反転し、XとYを入れ替える | `3.5DPI-dtbo.zip`の`waveshare-touch-35dpi.dtbo`。バックライトはGPIO 18 |

PocketTerm35のwikiの設定(`dtparam=i2c_arm=on`、`dtoverlay=waveshare-35dpi-4b`)の名前には「dpi」とあるが、中身はHDMIの液晶のタッチの設定で、DPIの液晶の同じ名前のオーバーレイ(GPIO 27、`i2c_gpio`)とは別物である。PocketTerm35の画面はHDMIなので、DPIの設定はしない。

画面は、Raspberry Pi 4ではファームウェアのフレームバッファ(`device/disp/disp_rpi5.c`)を使うので、ファームウェアのconfig.txtの設定で出す。PocketTerm35の画面はHDMIなので、普通のPi 4のカードで動く。液晶のEDIDが640×480を返せば、ファームウェアはその大きさで出す。映らないときのために、`etc/boot/config-rpi4.txt`の`[pi4]`の節に、下の表の`pt35`の4行をコメントにして置いてある。先頭の`#`を消せば使えるが、ほかのHDMIのモニタも640×480に固定されるので、カードを分けずに利用者が選ぶ形にした。DPIの液晶は設定がないと映らず、シリアルのコンソールも使えなくなるので、`make LCD=dpi35`で`etc/boot/config-rpi4-lcd-dpi35.txt`を`config.txt`の後ろに足す(`$(OBJDIR)/config-rpi4.txt`を作ってカードに置く)。

| LCD | 足す設定 | 由来 |
| --- | --- | --- |
| `pt35`(config.txtのコメント) | `hdmi_group=2`、`hdmi_mode=87`、`hdmi_cvt=640 480 75 6 0 0 0`、`hdmi_drive=2` | 3.5inch HDMI LCD (E)のwiki。wikiの`hdmi_cvt`の行は「=」が抜けているので足した。液晶のEDIDが640×480を返すなら、この設定が無くてもファームウェアはその大きさで出す |
| `dpi35` | `gpio=0-9=a2`、`gpio=12-17=a2`、`gpio=20-25=a2`、`enable_dpi_lcd=1`、`display_default_lcd=1`、`extra_transpose_buffer=2`、`dpi_group=2`、`dpi_mode=87`、`dpi_output_format=0x6f006`、`dpi_timings=640 0 20 10 10 480 0 10 5 5 0 0 0 60 0 60000000 1`、`gpio=18=op,dh`、`enable_uart=0`、`uart_2ndstage=0` | 3.5inch DPI LCDのwikiのBusterの設定。wikiの`hdmi_timings`は、今のファームウェアでDPIに使う`dpi_timings`にした。バックライトはwikiの`pinctrl set 18 op dh`。DPIがGPIO 14、15(UART0)も使うので、シリアルのコンソールは使えなくなる |

`dpi_output_format=0x6f006`は、下位4ビットが出力の形式6(RGB666)で、Waveshareの値をそのまま使う。上位のビット(同期の信号の有無と極性)の意味は資料によって位置が食い違うので、ここでは読み解かない。WaveshareのKMSのオーバーレイ(`waveshare-35dpi.dtbo`)は、25MHz、水平のフロントポーチ80・同期34・バックポーチ80、垂直8・4・8、同期は正の極性、RGB666(`MEDIA_BUS_FMT_RGB666_1X24_CPADHI`)で、wikiのBusterの値と違う。Busterの値で映らないときは、こちらを`dpi_timings=640 1 80 34 80 480 1 8 4 8 0 0 0 60 0 25000000 1`として試す。

タッチパネルは、起動時にカーネルが見つける(`kernel/sysdepend/rpi4/tpanel.c`、6.4.2)。2つの配線を順に試し、その配線のピンがすべて入力か、そのピンのバスの機能(GPIO 2、3ならI2C1、10、11ならSPI0のALT0)のときだけ触る。DPIの画面ではGPIO 2、3がALT2なので、PocketTerm35の配線は試さない。どちらの配線でも、0x5Dと0x14でGT911の製品の番号(0x8140)を読み、答えたものを取る。見つけたら3本のピンをカーネルのものとし、GPIOの実身からは変更できなくする。`make TPANEL=0`でピンに触らない。

PocketTerm35の本体のキーボードはRP2040が動かし、WaveshareのFAQのとおりRaspberry PiのUSB OTGのポート、つまりUSB-Cのポートにつながる。TessronOSでは設定なしで使える。内部の配線はType-Cのオスと4ピンのPH1.25の変換と4ピンのケーブルである。wikiの設定はPi 4とPi 5で共通の`dtoverlay=dwc2,dr_mode=host`で、このポートのUSBのコントローラはどちらもDWC2である。Pi 5版はRP1のxHCIではなくBCM2712のDWC2につながると判断したが、これは設定とデバイスツリーからの判断で、回路図は公開されていない。

| 項目 | 分かっていること | 根拠 |
| --- | --- | --- |
| Pi 4系のコントローラ | `/soc/usb@7e980000`、物理`0xFE980000`、割込みはSPI 73(`usb`)。SPI 40の`soft`は使わない。メモリは先頭の1GBがバス`0xC0000000`から見える | ファームウェアの`bcm2711-rpi-4-b.dtb`、`-400`、`-cm4` |
| Pi 5のコントローラ | `/axi/usb@480000`、物理`0x10_0048_0000`、割込みはSPI 73。バスアドレスは物理アドレスと同じ | `bcm2712-rpi-5-b.dtb`、`bcm2712d0-rpi-5-b.dtb` |
| 電源のドメイン | どちらのノードも`power-domains`はUSB。ファームウェアのタグ`0x00028001`、装置3で入れる | デバイスツリーと、Raspberry Pi OSのカーネルの電源のドライバの説明 |
| RP2040のUSB | チップのUSBはフルスピードまで | RP2040のデータシート |
| キーボードのクラス | 未確認。HIDのブートプロトコルかレポート形式か、VID:PIDは公開の資料にない。`device/usb/hid.c`はどちらも読む | 実機のログで確かめる |

設定はいらない。使うかどうかは、ポートに機器がつながっているかで決める。カーネルは機種を見分けない。最初のポートの走査が終わった後、USBの管理タスクが、デバイスツリーのノードからレジスタの番地を取り(`status`が`disabled`のままでも`reg`はある。手元の8つのdtbのすべてで確かめた)、ファームウェアのタグで電源を入れ、ホストを強制し、ポートの電源のビットを立てて、接続のビットを50ミリ秒ごとに最大3秒見る(`knl_dwc2_probe_step`)。起動はこの間待たない。

3秒は次の考えによる。機器は電源が入ってから100ミリ秒以内に信号線を引き上げる(USB 2.0の7.1.7.3)。本体のRP2040は、調べ始めるより数秒前のファームウェアの起動中に電源が入っている。残りは、USBを遅れて始めるプログラムのための余裕である。RP2040のプログラムが実際にいつ引き上げるかは未確認で、3秒より遅い機器は見つからない。

100ミリ秒つながったままならば、バッファ、割込み、DMAを用意してコントローラの1台に加え、`dwc2: probing: a device at ... ms ...: used`と出す。D+とD-がともに高い状態(線の状態3)は機器として扱わない。分圧で線を持ち上げる充電器があるためである。3秒でつながらなければ、ポートの電源のビットを落とし、コアをリセットし、`GUSBCFG`を元の値に戻し、電源が切れていたなら切り直して、`dwc2: probing: nothing in 3000 ms ...: put back, not used`と出す。このときは割込みもメモリもコントローラの登録も残らない。調べるのは起動時だけなので、起動の後で挿した機器は見つからない。

| 条件 | 動作 | コンソール |
| --- | --- | --- |
| `make DWC2=0` | 調べない | なし |
| コマンド行の`tessronos.dwc2=off` | 調べない | `dwc2: tessronos.dwc2=off: not used` |
| コマンド行の`tessronos.dwc2=host` | すぐ使う | `dwc2: tessronos.dwc2=host: used` |
| ノードが有効で`dr_mode`が`host` | すぐ使う。抜き差しもできる | `dwc2: ... dr_mode host: used` |
| `dr_mode`が`host`以外(`peripheral`、`otg`) | 触らない | `dwc2: ... dr_mode is otg: left to that` |
| それ以外(普通のカード) | 調べる | `dwc2: probing the USB-C port ...` |

ホストにしても害がないことは、ボードごとに次のように確かめた。

| ボード | 確かめたこと | 根拠と確度 |
| --- | --- | --- |
| Pi 4 Model B | VBUSは5Vの電源の線に直結で、切り替える回路がない。CC1とCC2はそれぞれ5.1kΩでGNDに落ちた受け側で、ソフトでは変えられない。データ線はUSB2だけでSoCに行く。ホストにしても電源の向きは変わらない。PCにつないだ場合は両方がホストで、どちらも線を引き上げないので接続にならず、何も送らない | 公開の縮小回路図 |
| Pi 400 | 公開の回路図を見つけられなかった。電源の回路はPi 4と同じ配置という公式の説明がある。データ線の有無は未確認。線がなければ接続にならない | 未確認の部分あり |
| Pi 5、Pi 500、CM5 | 回路図は公開されていない。USB-CのPDは電源の制御の側で扱う。ホストの強制がVBUSやPDに影響する回路の有無は確かめられない。ノードはすべて同じ番地で`disabled`。Pi 500のデータ線の有無は未確認 | 未確認。実機で確かめる |
| CM4、CM5のIOボード | CM4のIOボードでは、DWC2がボードのハブの先のUSB-Aにつながるので、ハブが見つかれば使う | CM4 IOの資料。実機では未確認 |

ファームウェアとの関係は次のとおりである。Pi 4系のオーバーレイの中身はデバイスツリーのプロパティだけで、ファームウェアがオーバーレイの有無で電源やクロックを変えるかは資料にない。このため電源はタグで自分で入れ、元の状態を読んで戻す。Pi 5のDWCのPHYの初期化は、ブートローダの2023-12-06以降で行われる。Pi 5のそれより前のブートローダでは、Linuxのドライバがコアのリセットのタイムアウトで止まった(読み書き自体は例外にならない)。TessronOSでも同じくタイムアウトになり、`dwc2: probing: the block did not come up`と出して元に戻す。条件によらずPHYを初期化するかは資料にない。

ドライバは`device/usb/dwc2.c`で、xHCIと同じ`USBHC_OPS`の裏にあり、管理タスクとクラスドライバには、ルートポートが1つのコントローラの1台として見える。xHCIと同時に使える。xHCIの道は変えていない。電源を入れ、IDの`GSNPSID`が`0x4F54xxxx`であることと内蔵のDMAがあることを確かめ、コアをリセットし、`GUSBCFG`でホストを強制する。FIFOは受信774、非周期の送信256、周期の送信512ワードに分け、ポートの電源を入れる。

転送はチャネルのバッファDMAで行う。バッファはuncachedのページで、Pi 4系では`dma-ranges`の範囲に入るDMAのゾーン(1GB未満)から取り、範囲を外れたら使わない。呼び出し側のメモリは直接渡さず、64KBのバッファを通す。チャネル0を制御とバルクの同期の転送に使い、ほかのチャネルをストリーム(割込みとバルクのIN)に使う。データのトグルは装置とエンドポイントごとに持ち、止まったチャネルの`HCTSIZ`から次の値を取る。制御とバルクのNAKはコントローラが自分で繰り返す。割込みのエンドポイントのNAKではチャネルが止まるので、管理タスクが次の間隔でまた頼む。コントローラにスケジュールの機能がないためで、`USBHC`の`poll_too`で管理タスクを割込みがあっても8ミリ秒ごとに動かす。割込みのエンドポイントはこの刻みより速くは読まれないが、キーボードには足りる。

割込みのハンドラは、チャネルかポートのマスクを外して信号を下げ、管理タスクと待っているタスクを起こすだけで、原因のビットは消さない。原因はレジスタに残り、管理のロックを持つタスクだけが読むので、別のプロセッサのハンドラと共有する状態がない。待つ側は2ミリ秒ごとにレジスタも見るので、割込みが届かなくても遅くなるだけで止まらない。

対応するのは、ルートポートに直結のハイ、フル、ロースピードの機器のコントロール、割込み、バルクの転送と、同じ速度のハブの先の機器である。ハイスピードのハブの先のフルスピードとロースピードの機器(分割トランザクション)、フルスピードのハブの先のロースピードの機器(プリアンブル)は`dev_init`で断り、`dwc2: a ... speed device behind a hub is not supported`と出す。アイソクロナスのエンドポイント(USBオーディオ、カメラ)は`E_NOSPT`を返す。

試験はQEMUの`raspi4b`のDWC2のモデル(`hcd-dwc2`)で行う。`make TARGET=_RPI4_ KTEST=1 KTONLY=usb BUILD_ID=<名前> test-rpi4`は、デバイスツリーを変えず、`RPI4_QEMU_USB`の`usb-kbd`を挿して起動する。`RPI4_QEMU_USB=`にすると機器なしになる。`test_dwc2`が、制御の転送、STALL、キーの押下と解放、抜き差しを確かめる。このモデルの物理のポートは1つで、`bus=usb-bus.0,port=1`を付けないとQEMUが自動でフルスピードのハブを間に挟む。機器なし、直結のハイスピード、`usb_version=1`のフルスピード、ハブの先、デバイスツリーのプロパティを書き換えた判定(`host`と`peripheral`)、`tessronos.dwc2=off`を、それぞれ通した。

実機では確かめていない。普通のカードで機器がなければ、起動の時間は変わらず、起動から数秒の間だけブロックに電源が入り、`dwc2:`の行が3行増える。消費電力の増え方は測っていない。実機で見るものは、`dwc2:`の行(`probing`の結果、電源の前の状態、ID、`hwcfg`、`HPRT`)、`dwc2: port connected`、`dwc2: port reset`の速度、`usb: ... speed ...`のVID:PIDとクラス、`usb: hid:`のプロトコル、`usb: controller N: ... interrupts arrived`である。USB-Aのポートのキーボードは、これまでどおりxHCIで使える。

## 3.7 Raspberry Pi 5のファン

Raspberry Pi 5のファンの端子のPWMの線は、RP1のGPIO 45からトランジスタを通して反転して駆動される。ピンがLowのときファンは全速で回り、ピンのプルダウンにより、駆動を始めるまでは全速である。ボードのデバイスツリーによれば、信号はRP1の2つめのPWMのブロック(`0x4009c000`)のチャネル3で、周期は41566ns、出力は反転である。デバイスツリーはCPUの温度に50℃、60℃、67.5℃、75℃の4つの段階を置き、ファンの強さを255のうち75、125、175、250としている。これはCPUを起動時のクロックのまま使う場合の値である。TessronOSはCPUを起動時より高いクロック(既定で1800MHz、設定によりファームウェアが許す最大の2.4GHzまで、3.9)で動かすので、カーネル(`kernel/sysdepend/rpi5/fan.c`)は早めに強く回す。段階は40℃、50℃、57.5℃、65℃で、それぞれ3℃下がると1つ前の段階に戻る。各段階でのファンの強さは255のうち120、170、220、255で、40℃未満では止まる。2026年10月5日の実機では、ブラウザで動画のページを開いたあとに全体が高温になり、デバイスツリーの値では足りなかった。ファンで足りないときは、ファームウェアが自分で下げる前にCPUのクロックを下げる。78℃で1800MHz、82℃で1500MHzにし、70℃を下回ると下げをやめる(`knl_cpu_heat`、タグ`0x00038002`)。下げも戻しもマシンの設定の上限(3.9)を超えない。戻すときは最大ではなく上限に戻り、上限が1800MHz以下なら78℃の段ではクロックは上限のまま変わらない。下げたときと戻したときは、コンソールに`fan: ... C, processors at ... MHz`と出る。ファンの制御のタスクは、ファームウェアが答える電源とクロックの状態(タグ`0x00030046`)も毎秒読み、変わるたびにコンソールに`power:`の行を出す。電源の電圧が下がった(`the voltage is low`、起動後にあれば`low voltage`)ことはここで分かる。

GPIO 45のどの機能がPWMの出力か、ブロックのクロックをどの源から作るかはRP1の公開資料の表にないので、起動時に調べる。チャネルを50%のデューティで動かし、クロックの源を水晶(50MHz)から順に、その下でピンの機能を1つずつ切り替えて、ピンを4000回読む。ピンが半分ほどHighで、何度も変化する組合せがPWMである。電源投入時のクロックは動いていない源を選んでいて、そのままではPWMの機能のピンがHighのまま変化しない(Raspberry Pi 5で確かめた)。同じ読取りからPWMの周波数が分かるので、ブロックのクロックが何であっても25kHzになるように範囲を決める。続けて75%のデューティでピンがHighである割合を読み、出力の反転の向きを確かめる。どの組合せでもPWMが見えないときは、クロックを元の設定に戻し、ピンを普通の出力として使い、最初の段階からは全速、それ未満では停止の入切だけで制御する。

温度は1秒ごとにファームウェアに問い合わせる(タグ`0x00030006`)。3回続けて読めなかったときは全速にする。制御のタスクは優先度6で、プログラムが負荷をかけていても動く。段階が変わるとコンソールに`fan: ... C, step ...`と出る。起動時には、PWMのクロックのレジスタの値、試した機能ごとの読取りの結果、決まった機能と範囲を出す。ファンの状態はシステムの実身のレコード9の`FAN_LEVEL`(255のうちの強さ)、`FAN_DRIVE`(`pwm`か`onoff`)、`FAN_SPEED`(デバイスツリーがファンの回転を読むとしているワードの値)で読める。CM5のIOボードのファンの端子は別の方式なので、制御はRaspberry Pi 5だけで行う。

## 3.8 ウォッチドッグ

BCM2712とBCM2711のPM(電源管理)のブロックはウォッチドッグを持つ。`PM_WDOG`(オフセット`0x24`)は16マイクロ秒の単位で20ビット、つまり17秒弱まで数え下ろし、0になると`PM_RSTC`(`0x1c`)のWRCFGの欄が全体リセットを指していればチップ全体をリセットする。書込みはどれも上位8ビットにパスワード`0x5a`を持たなければ無視される。ブロックのアドレスは、BCM2712では周辺機器の空間の`0x7d200000`(CPUからは`0x10_7d20_0000`)、BCM2711では`0x7e100000`(`0xfe100000`)で、ボードのデバイスツリーの`watchdog`のノード(`brcm,bcm2712-pm`、`brcm,bcm2835-pm`)と一致する。CM5はRaspberry Pi 5と同じBCM2712なので同じように使え、Raspberry Pi 4、400、CM4はRaspberry Pi 4系のイメージの再起動にもともと同じウォッチドッグを使っている。カーネルはこれを停止の検出(4.7.3)に使い、10秒後のリセットを設定し直し続ける(`kernel/sysdepend/rpi/wdog.c`)。止めるときは`PM_RSTC`に`0x102`を書く。QEMU virtにはウォッチドッグがない。

止まったときのリセットまでの時間は、マシンの設定の行`WATCHDOG`(秒、既定30、0で使わない、上限3600)で決まる。行がないとき、値が数でないときは既定の30秒にする。起動時に使い始めると、コンソールに`watchdog on`と出る。設定はシステム環境設定の〈機器〉の〈電源管理〉で、使うかどうかと秒数(5〜3600)を選んで〈設定〉を押すと書き込まれ、カーネルが設定の実身の変更を見てすぐに反映する(8.9)。今の値はシステムの実身のレコード9の`WATCHDOG_S`(秒、0は使わない)と`WATCHDOG_HW`(ボードのウォッチドッグを動かしているか)で読める。

CM5ではコンソールをRP1のUART0に移す(3.5.2)ので、RP1との間のPCIeが止まると、停止の報告もコンソールに出られない。その場合も、ウォッチドッグによるリセットは働く。

## 3.9 CPUのクロックの上限

CPUのクロックの上限は、マシンの設定の行`CPU_MHZ`(MHz)で決める。0はファームウェアが許す最大(Raspberry Pi 5とCM5では2.4GHz)である。行がないとき、値が数でないときは1800MHzとし、配布するシステムのボリュームの設定の実身(`etc/def`)の初期値も`CPU_MHZ 1800`にしてある。すでに配ったマシンで行がない場合も1800MHzになる。2.4GHzで4つのCPUを回すと消費電流が増え、電源が足りないと電圧が下がってボードが止まりやすいためである。2026年10月の実機で、起動時に2.4GHzへ上げるようにしてから落ちやすくなった。電圧が下がったかどうかはコンソールの`power:`の行(3.7)と、システムの実身のレコード9の`UNDERVOLT`で確かめられる。十分な電源を使う人は、システム環境設定で〈最大〉などに上げられる。

`config.txt`の`arm_freq`では上限を決めない。ファームウェアがその値を最大として扱い、OSから上げられなくなるからである。上限はカーネルがファームウェアに求めるクロックで決める(`kernel/sysdepend/cpu/core/armv8a/cpuclk.c`、`include/ts/cpuclk.h`)。

求めるクロックは、ファームウェアの最大(タグ`0x00030004`)、上限、ファンによる下げ(3.7)のうち最も低いもので、ファームウェアの最小(タグ`0x00030007`)を下回らない(`knl_cpu_want`)。上限がファームウェアの範囲の外にあれば、範囲の端に丸める。ファームウェアには、求めるクロックが変わったときだけ設定を求める(タグ`0x00038002`、`knl_cpu_hw_set`)。マシンの設定は、どの行を書き換えても全体を読み直すからである。

| 時点 | 処理 | 上限 |
| --- | --- | --- |
| ボードを見分けた直後(`knl_cpu_start`) | ファームウェアの範囲を問い合わせ、クロックを上げる | 1800MHz(設定はまだ読めない) |
| 実身マネージャの初期化の直後(`wm_cpu_conf_apply`) | 設定の実身の`CPU_MHZ`を読む | 設定の値 |
| デスクトップの起動時(`wm_dev_conf_apply`) | マシンの設定をすべて読む | 設定の値 |
| 設定の実身が書き換えられたとき | カーネルの見張りのタスクが読み直す(8.9) | 設定の値 |

起動時のクロックが1800MHzを超えることはないので、設定で上限を低くしてあるマシンでも、起動の途中で2.4GHzになることはない。上限が変わると、コンソールに`cpu: limit ... MHz; processors at ... MHz (the firmware allows ... to ...)`と出る。`processors at`はファームウェアが答えた設定後のクロックである。起動時は`TessronOS: processors at ... MHz (were ..., the firmware allows ... to ..., limit 1800 until the settings are read)`と出る。

今の状態はシステムの実身のレコード9で読める(8.4)。

| キー | 内容 |
| --- | --- |
| `ARM_HZ` | 今のクロック(ファームウェアの答え) |
| `ARM_LIMIT_MHZ` | 上限(0は最大) |
| `ARM_MIN_HZ`、`ARM_MAX_HZ` | ファームウェアが許す範囲 |
| `ARM_HEAT_HZ` | ファンによる下げ(0は下げていない) |
| `THROTTLED` | ファームウェアが答えた電源とクロックの状態(タグ`0x00030046`)。ビット0は今の低電圧、1は最大の引下げ、2はクロックの低下、3は温度のソフトリミット、ビット16〜19はそれぞれが起動後にあったこと |
| `UNDERVOLT` | `now`(今低電圧)、`before`(起動後に低電圧があった)、`no`、`unknown` |

Raspberry Pi 4、400、CM4でも同じ設定が効く。ファームウェアとのやり取り(`kernel/sysdepend/rpi/mbox.c`)は共通である。Raspberry Pi 4の最大はボードのリビジョンにより1500MHzか1800MHzなので、既定の1800MHzは最大と同じになり、2000MHzや2200MHzを選んでも最大に丸められる。Raspberry Pi 4にはファンの制御がないので、ファンによる下げはない。CM5はRaspberry Pi 5と同じで、ファンの制御だけがない(3.7)。QEMUにはファームウェアがないので、上限を覚えるだけでクロックは変えない(`knl_cpu_hw_*`の弱い定義)。

カーネル内の試験`ktest_devob`の`test_cpu_limit`は、求めるクロックの計算をRaspberry Pi 5とRaspberry Pi 4の範囲で確かめ、マシンの設定に`CPU_MHZ`と`WATCHDOG`を書いて読み直させ、上限とウォッチドッグの秒数がカーネルとレコード9に出ること、値がないときと数でないときに既定に戻ることを確かめる。システム環境設定の操作は`tools/tests/ui_sysenv.steps`で試験する(11章)。

## 3.10 x64のPC

UEFIで起動するx64のPCは、1つのカーネルのイメージ(`TARGET=_X64_`、`ARCH=x86_64`)で扱う。QEMUの`q35`マシン(ファームウェアはOVMF)と、Microsoft Surface Go 2(モデル1927)を同じイメージで動かすのが目標である。実機の試験にはLenovo ThinkPad X1 Yoga 第3世代(Kaby Lake R、Core i7-8650U、Intel UHD 620、機種の型番20LDから20LG)も使う。機種はSMBIOSの製造元と製品名で見分け(`BOARD_SURFACE_GO2`、`BOARD_X1YOGA3`)、機種ごとの差はそこで分ける。X1 Yogaの固有の部分(Wacomのペンとタッチ、ThinkPadのECのホットキー、2560×1440の画面)は、実機のHW.TXTとDSDTを見てから扱う。段階1でQEMUの`q35`の上のデスクトップまでを作り、段階2(実機での起動)では、画面とファイルによる記録(3.10.7)、起動したディスクの見分け、ACPIの解釈系(3.10.6)、USBのLANを入れた。ThinkPad X1 Yoga 第3世代はデスクトップまで起動する(3.10.7)。GPU、Wi-Fi、解像度の変更、PS/2、電池、音などの実機での確認と、Surface Go 2での起動はこれからである(3.10.8)。コア依存のコードは`kernel/sysdepend/cpu/core/x86_64/`に、マシンに依存するものは`kernel/sysdepend/x64/`に、ACPIの表の解析は`kernel/sysdepend/acpi/`に、UEFIのローダは`tools/x64/uefi/`にある。

| 項目 | x64のPC(QEMU q35) |
| --- | --- |
| CPU | `-cpu max -smp 4`。命令セットの基準はx86-64-v2で、AVX2は実行時に調べる。ACPIのMADTで有効とされた論理CPUをすべて使う(上限1024、4.3.2) |
| RAM | UEFIのメモリマップのうち、使ってよい種類の範囲 |
| カーネルの配置 | 物理アドレスはローダが決める。仮想アドレスは上位2GB(`0xFFFFFFFF80200000`から) |
| 起動 | UEFIのローダ`\EFI\BOOT\BOOTX64.EFI`。Secure Bootは使わない |
| 割込み | Local APIC(x2APICかxAPIC)とIOAPIC。PCIeのデバイスはMSIかMSI-X。ハンドラの無いベクタに来た割込みは、Linuxと同じく、Local APICで終えて数えて捨て、ベクタごとの最初と10倍ごとにコンソールに出す(IOAPICの入力ならその入力を止める)。プロセッサは止めない。起動の設定`msiearly`は、試験のために、メッセージの割込みを有効にした直後に、ハンドラの定義の前にその割込みを1回起こす |
| タイマ | Local APICのタイマ(TSC deadlineか単発)。周波数はCPUID 15h、HPET、ACPIのPMタイマ、PITの順で測ったTSC |
| MMU | 4段のページテーブル。プロセッサにPCIDがあれば、プロセスの空間ごとの番号で切り替える |
| コンソール | COM1(16550)と、フレームバッファに出す起動時の文字。COM2は2つ目のシリアルポート(`serb`) |
| ディスク | NVMe。実機ではSATA(AHCI)とUSBメモリも |
| ネットワーク | virtio-net(PCI)。実機ではIntelのLAN、USBのLAN、Wi-Fi(3.10.3) |
| 画面 | UEFIのGOPが用意するフレームバッファ。Intelの内蔵グラフィックス(第9世代)では解像度を変えられる(6.9) |
| USB | `qemu-xhci` |
| 時計 | CMOSのRTC。ローカル時刻(既定、Windowsと同じ)かUTCかを選べる(9.5.1) |
| 乱数 | `RDSEED`、`RDRAND`とソフトウェアの疑似乱数生成器 |

### 3.10.1 起動

UEFIのファームウェアは、ESP(EFIシステムパーティション)の`\EFI\BOOT\BOOTX64.EFI`を実行する。これはmingw-w64のGCCで作るPE32+のEFIアプリケーションで(`tools/x64/uefi/loader.c`)、次のことを行う。

1. 自分を読み込んだボリュームから、起動の設定`cmdline.txt`の1行とカーネル`tessronos.elf`を読む。探す順は、ローダ自身のファイルのあるディレクトリ(装置パスのファイルパスの節から求める。内蔵のディスクに登録した`\EFI\TESSRON\BOOTX64.EFI`なら`\EFI\TESSRON\`)、`\EFI\TESSRON\`、根である(3.10.10)。それぞれの場所で、その名前のファイルが無ければ`.new`を付けたもの、それも無ければ`.old`を付けたものを読む。システムのバージョンアップは、この2つのファイルを`<名前>`を`<名前>.old`に、`<名前>.new`を`<名前>`に名前を変えて入れ替えるので(11.8.1)、2回の変更の間で電源が切れても、同期済みの新しいファイルで起動する。設定の`video=WxH`で、GOPの画面モードを選ぶ。書かなければ、SMBIOSの機種で決めたモード(ThinkPad X1 Yoga 第3世代は1600×1200)、それも無ければ1920×1280以下で最も大きいモードにする。
2. ELFの`PT_LOAD`を物理アドレス(`p_paddr`)の並びのまま、ファームウェアに求めたページ(`EfiLoaderData`。3.10.7)に置く。イメージの先頭の`.khdr`の見出し(`T_KHDR`)がイメージの後ろに続けて欲しいヒープの大きさを言うので、それも合わせて取る。
3. メモリマップ、ACPIとSMBIOSの表のアドレス、画面、乱数の種、コマンド行を`T_BOOTINFO`(`kernel/sysdepend/cpu/core/x86_64/bootinfo.h`)にまとめる。
4. カーネルのページテーブルを作る。全物理メモリの直接の写し、デバイスの領域の先頭4GB、上位2GBのカーネルのイメージである。カーネルの半分の256個のPML4のエントリはすべてここで埋め、プロセスの空間はそれを写す。
5. ブートサービスを抜け(`ExitBootServices`)、CR3とPATを設定し、`T_BOOTINFO`の物理アドレスを`rdi`に入れてカーネルの入口に飛ぶ。

ローダは画面のモードを決めた後、自分の出す文字をGOPのフレームバッファに直接描く。幅が1600画素以上で高さが900画素以上の画面では文字を縦横2倍にし、最上行の帯に今の段階を出す。カーネルのコンソールはローダの最後の行の下から続く(3.10.7)。ブートサービスを抜ける前に、自分が読み込まれたボリュームの`\EFI\TESSRON\`に、出した文字(`LOADER.TXT`)と機械の報告(`HW.TXT`ほか)を書く(3.10.7)。ESPの区画のGPTの固有GUIDを、自分の装置パスのハードドライブの節から読んで`T_BOOTINFO`で渡す。

ランタイムサービスはその後使わない。カーネルはランタイムサービスの領域を写さず、`SetVirtualAddressMap`も呼ばないので、カーネルからUEFIの変数は書けない。ファームウェアの起動の項目は、システム登録の依頼があるときにローダがブートサービスの間に書く(3.10.10)。ブートサービスを抜ける前に`GetTime`を1回呼び、ファームウェアが時計にタイムゾーンを与えていればそれを渡す(9.5.1)。時計はCMOSから読み、電源はACPIで切る。ローダの出力は、ファームウェアのコンソールと、あればCOM1に出る。カーネルは`boot.S`で自分のGDTに移り、`reset_main.c`がGSのベースをプロセッサごとの領域に向け、IDT、フレームバッファのコンソール、メモリの範囲、ACPI、ボードの判別、TSCの周波数、MMUの順に初期化して、`knl_main`に入る。ボードはSMBIOSのtype 1(製造者と製品名)で見分ける(`BOARD_QEMU_Q35`、`BOARD_SURFACE_GO2`、`BOARD_X1YOGA3`、それ以外は`BOARD_PC`)。

### 3.10.2 仮想アドレスの配置

| 範囲 | 中身 |
| --- | --- |
| `0x0000000000400000`〜`0x00007FFFF0000000` | プロセスの空間。ユーザスタックの上端は`0x00007FFFF0000000` |
| `0xFFFF800000000000`〜 | 全物理メモリの直接の写し(`PA2VA`) |
| `0xFFFFC00000000000`〜 | デバイスの領域(キャッシュしない)。物理アドレスの15TB未満はそのまま足した所に見え、先頭4GBはローダが写し、それより上(64ビットのBAR)は最初に触ったときにページフォールトの処理で1GBずつ写す。15TB以上のアドレス(ハイパーバイザが64ビットのBARを物理アドレスの上端近くに置くとき)は、領域の最後の1TBの1GBの枠に、求められた順に割り当てる(`knl_dev_far`) |
| `0xFFFFD00000000000`〜 | `vmap`の領域 |
| `0xFFFFE00000000000`〜 | プロセッサごとの領域 |
| `0xFFFFFFFF80000000`〜 | カーネルのイメージ(`-mcmodel=kernel`) |

PATは`0x0007010600070106`で、PWTのビットを立てたページが書込み結合(WC)になる。フレームバッファはWCで写す。

### 3.10.3 デバイス

ACPIの表は、段階1では自前の小さな解析(`kernel/sysdepend/acpi/acpi.c`)で読む。MADT(Local APICとIOAPIC、割込みの付け替え、NMIの線)、MCFG(PCIeの構成空間のECAM)、FADT(PMタイマ、リセットの方法、DSDT)、HPETである。電源を切るときは、DSDTの`\_S5`の値をPM1の制御のレジスタに書く。

PCIeは、MCFGが言うECAMで構成空間を読む。バス番号とBARはファームウェアが割り当てたものをそのまま使い(`PCIE_KEEP_FW_BARS`)、ARMの`pcie.c`の割当てはしない。MSIとMSI-Xは、Local APICの宛先`0xFEE00000`とベクタ番号で送らせる(`cpu_msi.h`)。

| デバイス | ドライバ | 内容 |
| --- | --- | --- |
| NVMe | `device/nvme/nvme.c` | 管理のキューと入出力のキュー1つ、割込みはMSI-X(なければMSI)。`nvmea`、区画は`nvmea0`、`nvmea1`…。電源を切るときと再起動のときに、記録を書いた後でFlushとシャットダウンの通知(CC.SHN)を行う。答えの無いコマンドは制御器をリセットしてから`E_IO`で失敗にする。HMPREを言う制御器にはホストメモリバッファを渡す(06章6.2.1) |
| virtio-net | `device/vnet/vnet.c`と`device/virtio/vio_pci.c` | virtioのPCIの転送。レジスタの読み書きをvirtio-mmioの並びに読み替える |
| xHCI | `device/usb/xhci.c`、`device/usb/x86_64/xhci_pc.c` | ARMと同じドライバ。Intelのコントローラのための手順(ファームウェアからの引き継ぎの待ち、BEIを使わない、Type-Cの口の役割の記録、USB 3のリンクの温かいリセット)をx64だけで足す(06章 6.3.3) |
| 画面 | `device/disp/disp_gop.c` | GOPのフレームバッファ。画素の並び(RGBかBGR)はGOPの答えから決める |
| 時計 | `kernel/sysdepend/x64/rtc.c` | CMOSのRTC。レジスタの時刻をローカル時刻かUTCとして読み書きし、その差は暦の設定に従う(9.5.1) |
| 乱数 | `kernel/sysdepend/x64/rng_hw.c` | `RDSEED`、なければ`RDRAND` |
| SATA | `device/ahci/ahci.c` | AHCIの先のディスク。`sataa`、区画は`sataa0`…(06章) |
| PS/2 | `device/ps2/ps2.c` | i8042のキーボードとマウス(ThinkPadの内蔵のキーボードとTrackPoint)。実身「ps2kbd」「ps2aux」 |
| 音 | `device/snd/x86_64/snd_hda.c` | Intel HDA。コーデックを仕様どおりに歩いて道筋を組む(06章) |
| LAN(Intel) | `device/net/x86_64/e1k.c` | e1000eの系統。チップセットのI219の手順(06章、ULPの解除、PHYの電源、SkylakeのPCHの回避) |
| I2C、HID over I2C | `device/i2c/x86_64/` | LPSSのI2C(DesignWare)と、その先のタッチとペン(ACPIのPNP0C50) |
| PCIeとUSBの有線LAN | `device/net/x86_64/re.c`、`rge.c`、`igc.c`、`lanpci.c`、`device/usb/x86_64/usbven.c` | RTL8111/8168(8168Bを含む)、RTL8125(8125B、8125Dを含む)、I225/I226、USBのRTL8152/8153、AX88179、AX88772/AX88772A/AX88772B(9.6.13)。実機では未確認 |
| USBのLAN | `device/usb/x86_64/cdcnet.c`、`device/net/netsel.c` | 通信クラスのCDC-ECMとCDC-NCMのアダプタ。どの構成にその機能があるかを読み、なければ別の構成を選ぶ(RTL8153などはメーカー独自の構成で立ち上がる)。virtio-netがないときに、プロトコルスタックのカード(`neta`)になる |
| ネットワークアダプタの選択 | `device/net/netsel.c`、`netadp.c` | プロトコルスタックのカード(`neta`)にするアダプタを、利用者の選択(機器の設定の`NET_ADAPTER`)か、自動ならリンクが上がっている有線、無ければ接続しているWi-Fiから選ぶ。替えるときは`neta0`のアドレスを新しいカードのものにし、DHCPを問い合わせ直す。実身「netadp」(09章9.6.1) |
| Wi-Fi | `peripheral_kernel/network/netbsd/rumpcomp/wifi/`、`wifi_user.c`、`device/net/wifi/wifi.c`、`wpa/` | 既定で入る(`make WIFI=0`で除く)。IntelのWireless-AC 8265などをNetBSDのiwm(4)とnet80211でrumpカーネルの中で動かし、WPA2-Personalの鍵の交換をwpa_supplicantの一部で行う。ファームウェアはシステムボリュームの実身。実身「wlan0」(09章9.6) |
| GPU(描画) | `device/gpu/intel/`、`kernel/sysdepend/x64/igt_ggtt.c` | Intelの第9世代の内蔵GPUのレンダーとコピーのエンジン。forcewake、RC6と負荷に応じた周波数、PPGTT、論理リングコンテキストとexeclistsでの投入、エンジン単位のリセット、ウインドウの合成(レンダーのエンジンの計算シェーダ、BLTのエンジンでの写し)、プロセスのOpenGL ES(Mesaのiris)。GGTTの割当ては表示の係と、pcodeは表示の係の共有の部品で行う。実身「gpu0」。起動の設定`igt=`、`igtrc6=`、`igtrps=`、`igtrc=`(13章13.11) |
| 内蔵カメラ | `device/cam/x86_64/`、`device/gpio/x86_64/gpio_pch.c`、`device/cam/cam_isp.c` | Surface Go 2の前面のカメラ(OV5693)。IPU3のCSI-2の受け口CIO2、ACPIのSSDBと`_DEP`からのセンサーと電源の組立て、INT3472のdiscreteの電源(PCHのGPIO)、I2Cのセンサー、ソフトウェアの現像でNV12とYUY2を作り、カメラデバイスの1台にする。TPS68470の電源の先のセンサー(背面、赤外線)は使わない。起動の設定`cam=off`、`cam=bars`、`cam_bayer=`(06章6.6.1) |
| シリアル | `device/ser/x86_64/ser.c` | 16550のCOM1〜COM4(`sera`〜`serd`)。割込みの線はACPIのMADTの付け替えに従ってIOAPICから受ける。受信は割込みでリングに溜める。COM1はコンソールも兼ねる |

デスクトップのウインドウの合成の内側のループ(`outer_kernel/wm/x86_64/compose_simd.c`)は、x86-64-v2に必ずあるSSEで4画素ずつ処理し、プロセッサにAVX2があってXCR0がそのレジスタを保つときはAVX2で8画素ずつ処理する。どちらを使うかは最初の呼出しで1度だけ調べる。どちらも`compose.c`の普通のループと同じ16ビットの幅で同じように丸めるので、結果はビット単位で同じになる(`ktest_wm`の`test_simd_same`)。このファイルだけはSIMDのレジスタを使ってよいように翻訳し(`build_make/x64.mk`)、呼ぶのは`TA_FPU`の合成のタスクだけである。

プロセッサのクロックは、ファームウェアとプロセッサに任せる(`kernel/sysdepend/cpu/core/x86_64/cpuclk.c`)。カーネルは、クロックや電力に関わるレジスタ(IA32_PERF_CTL、IA32_PM_ENABLE、HWPの要求、IA32_ENERGY_PERF_BIAS、IA32_MISC_ENABLEのSpeedStepとターボのビット、パッケージの電力の上限)をどれも書かない。ACPIの`\_SB._OSC`でも何も求めない。機器の設定の`CPU_MHZ`は、保存されていても使わない(システム環境設定も選ばせない)。最初に設定を読んだときに、仮想マシンでなく(CPUID.1:ECXのビット31)、HWPがある(CPUID.6:EAXのビット7)機械では、IA32_PM_ENABLE、HWPの能力と要求を読んでコンソールに出す(`cpu: HWP on: ...`)。読むだけである。i7-8650UのようにファームウェアがHWPを有効にした機械では、プロセッサが自分でクロックを選ぶ。

今のクロックは、Linuxの`/proc/cpuinfo`の「cpu MHz」(`arch/x86/kernel/cpu/aperfmperf.c`)と同じ考え方で、APERFとMPERFから求める(`knl_cpu_freq_tick`、`knl_cpu_x64_clock`)。APERFは実際のクロックで、MPERFはTSCと同じ一定の速さで数え、どちらもプロセッサが動いている間(C0)だけ進む。各プロセッサは自分のタイマの割込み(停止の検出の周期、500ミリ秒ごと)で2つを読み、前回からの増分の比にTSCの周波数を掛けて、その周期に動いていた間のクロックを覚える。読む側は待たずに、1.5秒より新しい値を持つプロセッサの平均と最も速いものを取る。システムの実身のレコード9の`CLOCK_MHZ`(平均)と`CLOCK_MOST_MHZ`がそれで、32ビットのHzでは4.29GHzを超えられないのでMHzで書く。プロセッサが名乗る基本と最大のクロック(CPUID.16h)は`BASE_MHZ`と`TURBO_MHZ`、温度は`TEMPERATURE`で、Linuxのcoretempと同じく、TjMax(MSR_TEMPERATURE_TARGETのビット23:16、0なら100度)からパッケージの温度の状態(パッケージのセンサがなければそのコアの状態)のビット22:16を引く。ビット31が立っていないときは読めないとする。温度はIntelのプロセッサだけで読む。どれもMSRを読むだけで、仮想マシンでは読まない(QEMUでは出ない)。

プロセッサがPCIDを持つときは、プロセスの空間ごとに番号を付けてCR3に載せ、切り替えのたびにTLBを捨てずに済ませる。`INVLPG`は今のPCIDの分しか無効にしないので、ほかのプロセッサに頼むTLBの無効化(NMI)は、PCIDを使うときは常に全体の無効化にする。起動時のコンソールに`process-context identifiers on`か`off`が出る。

### 3.10.4 ディスクのイメージ

`make TARGET=_X64_ DESKTOP=1 BUILD_ID=<名前> sdimg`で`obj/_X64_<名前>/sd.img`ができる。GPTで、区画1がESP(FAT32、512MB)、区画2がシステムのボリュームである。カーネルは、ローダが渡したESPの固有GUIDを持つディスクを探し(NVMeとUSBメモリ、最大15秒待つ)、そのディスクのESPを`/boot`に、TessronOSの区画をシステムのボリューム(`CNF_OB_SYSVOL`、`kernel/sysdepend/x64/bootdisk.c`)にする。見つからなければどちらも使わない。PCの内蔵のSSDにほかのOSがあっても、起動したディスク以外には書かない。ESPには`EFI/BOOT/BOOTX64.EFI`、`tessronos.elf`、`cmdline.txt`と、Raspberry PiのSDカードの第1パーティションと同じ試験用のファイル(`init.elf`など)が入る。USBメモリかNVMeのSSDにそのまま書けば起動できる形にしてある。

### 3.10.5 試験

CPUの数の試験は、`tools/x64/ktest64.sh`の環境変数`SMP`でQEMUの`-smp`を与えて行う。`SMP="8,sockets=1,cores=4,threads=2"`(ThinkPad X1 Yoga 第3世代と同じ4コア8スレッド)、`SMP=16`、`SMP="64,sockets=2,cores=16,threads=2"`。起動すると「N of M CPUs online (P packages, C cores, T threads)」と出る。コアとスレッドは、CPUID 0x1F(なければ0x0B)のSMTとコアの段のビット幅と、各CPUのx2APICのIDから求める。WHPXではQEMUがCPUの段を伝えないので、どのCPUも別のパッケージに見える。QEMUは255を超えるCPUにIOMMUの割込みの付け替えを求めるので、それを超える数は試していない。ホストが混んでいるとき(他のビルドが走っているとき)のTCGの8 CPU以上やWHPXの16 CPUでは、時間を測る試験(`test_cross_wakeup`、`test_timer_other_prc`、`test_stress`、`test_many_tasks`)が時間内に終わらないことがある。ホストが空いているときは通る。

試験機はWindowsのQEMU(`C:\Program Files\qemu`)と、それに付いてくるOVMF(`edk2-x86_64-code.fd`)である。`make TARGET=_X64_ BUILD_ID=<名前> qemu-x64`はESPのディレクトリをそのままFATのディスクとして見せて起動し、`qemu-x64-disk`はディスクのイメージをNVMeとして挿して起動する。x64のRTCは既定でローカル時刻として扱う(9.5.1)ので、QEMUの既定のUTCではなく`-rtc base=localtime`を付け、Windowsの入ったPCと同じ形にする(`x64.mk`、`ktest64.sh`、`ui_run64.py`はどれも付ける)。

```text
qemu-system-x86_64.exe -M q35 -cpu max -smp 4 -m 2G -rtc base=localtime
  -drive if=pflash,format=raw,readonly=on,file=edk2-x86_64-code.fd
  -drive file=sd.img,if=none,format=raw,id=nvd -device nvme,serial=tessronos,drive=nvd
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -serial stdio
```

段階1で確かめたこと(2026-10-18):

- 4つのCPUが起動し、PCIeの列挙、NVMe(MSI-X)、virtio-net(MSI-X)、xHCIとUSBのキーボードとタブレット、GOPの1280×800の画面が動く。
- NVMeのシステムのボリュームから実身マネージャが立ち上がり、`/boot/INIT.ELF`がプロセスとして動き、デスクトップが出る。
- ポインタのダブルクリックで実身を開き、メニューから小物の電卓(newlibとlibmを使うプロセス)を起動して、0.1+0.2、√2、1÷7を計算できる。
- 停止の検出(4.7)が4つのCPUで働く。

カーネル内の試験(12.3)は、x64のディスクのイメージに組み込み、`tools/x64/ktest64.sh`で走らせる。AArch64のレジスタを直接見ていた試験は、x86-64ではそれに当たるもの(`syscall`の引数、SMAPとSMEP、CR2とエラーコード、Local APIC、XMMのレジスタなど)を見るように書き分けた。デバイスツリー、SDのホストコントローラ、V3Dの表とGPUで描く試験(`board`、`sd`、`gpu`、`gl`)はArmの機器だけのものなので、x64では組み込まず、飛ばしたことを出す。

2026-10-19に確かめたこと:

- カーネル内の試験の全部(Windowsのハイパーバイザで約6分)は、以前は698件中695〜696件が成功していた。下の直しの後は702件中701件が成功し、残る1件は外のNTPのサーバに届かなかった`test_sntp`で、`so`の群だけをもう一度走らせると成功した。失敗するのは、生きているプロセスがあることを前提にする`test_processes`と、時間に依存して時々失敗する試験(`test_perf`、`test_figtext`、`test_menu_look`)の1〜2件だった。この4つの群(`tsfsobj`、`drop`、`part`、`bt2`)をx64とQEMU virtで3回ずつ走らせて原因を見て、試験を直した。`test_perf`は、Windowsのハイパーバイザの上の仮想のNVMeでは、2MBの書込みが素の書込みの37〜54%の速さで回ごとに揺れるので、判定の下限を40%から25%にした(1ブロックずつの要求なら17%を下回る)。`test_figtext`は、実身マネージャの表示用の表(96項目の固定)が前の群の実身の名前とアイコンで埋まり、新しい実身を開けなかった(x64でもArmでも、R1.110でも起きる)。表をメモリに合わせた大きさにし、使われていない項目を手放すようにして直した(11.1)。`test_processes`は、`init`が挨拶だけをしてすぐ終わる機械(QEMU virt)ではプロセスが1つもないので、そのときは終わるまで待つプロセスを試験が自分で起こして確かめる。直した後は、x64で52件、QEMU virtで53件が、それぞれ3回とも成功した。`test_menu_look`は6回のうち一度も失敗しなかった。`NET=user`のネットワークの試験は55件すべて成功する(DHCP、IPv6の自動設定、AAAAの名前解決、SNTP、HTTPS、FTP)。
- UIの試験(12.4)は`tools/x64/ui_run64.py`で走らせ、`ui_glgears`を含む33の台本が成功する。ディスクの並びやデバイスの名前を前提にする台本は、x64のものを`tools/x64/tests/`に分け(`ui_xfconv`、`ui_scrprint`、`ui_btcd`、`ui_sysenv_font`、`ui_vols`、`ui_diskfix`、`ui_diskfix_boot`)、すべて成功する。`ui_vols2`はそのままで成功する。パッケージ(PCM/MPEGプレーヤ)を前提にする`ui_rt`と`ui_user_apps`も、x64のイメージに既定で入れてから成功する。成功しないのは、日付で座標が変わるもの(`ui_calendar_ops`)である。
- USBを、QEMU virtの試験機と同じ装置(1つ目のxHCIのSuperSpeedのディスク、ハブとその先のオーディオ、ディスク、マウス、USB 2だけの2つ目のxHCIのディスク3台とキーボード)をつないで、`tools/x64/ktest64.sh`の`USBQ=1`で試験した(12.3)。`usb`、`devob`、`snd`、`vols`、`grow`、`btcd`の61件中59件が成功する(USB 3とUSB 2、ハブ越し、USBのディスクの抜き差しとマウント、実身の出し入れ、26台を超えるディスク、CD-ROM、HIDの抜き差し、プログラムのUSBデバイス、オーディオの再生と出力先の切替え)。残りの2件は、WindowsのQEMUではUSBのオーディオの再生が実時間の数分の1しか進まないためで、同じQEMUのArmでも同じに失敗する。USBのカメラはQEMUに装置が無いので、実機で確かめる。
- x64のパッケージ(.tpk)を作れるようにした。表計算は`make TARGET=_X64_ pkg PKG=basic-calc-editor`、PCM Playerは`ARCH=x86_64 build-tessron.sh`、MPEG Playerは`TS_ARCH=x86_64`の`build-ffmpeg-ts.sh`と`build-tessron.sh`(どちらもそのリポジトリ)である。x64のPCM PlayerとMPEG Playerのパッケージは`etc/pkg/x64/`に置き、x64のイメージには既定で入れる(`PKGS`の既定。Armの既定と`etc/pkg/`のパッケージは変えていない)。表計算はx64のイメージに初めから入っているので、パッケージは入れない(同じプログラムを古いパッケージで上書きしないため)。MPEG Playerのメニューの「NEONを使う」は、x64ではFFmpegのSIMDを切り替えるので「SIMDを使う」とする。イメージに入れて、PCM PlayerはWAV、MP3、ZIPを、MPEG PlayerはH.264/AAC、VP8/Vorbis、MPEG-4/MP3、HEVC/AACを4つのスレッドで復号して再生した(QEMUの変換)。
- 4つのCPUのデスクトップで、「歯車」を描かせ続けたまま、プロセスの起動と終了(すぐ終わるもの5つ、4つのCPUを同時に使うもの、タスクを200回作って消すもの)と電卓の起動と終了を300回繰り返した(約17分、プロセス2400個)。失敗した操作、例外、権限の不足はなく、「歯車」は最後まで毎秒59〜60フレームで描いた。
- システム環境設定の〈電源を切る〉でACPIの`\_S5`に入って電源が切れ、〈再起動〉でFADTのリセットのレジスタからリセットされ、もう一度デスクトップまで起動する(QEMUの変換で確かめた。Windowsのハイパーバイザの上では、QEMUのモニタの`system_reset`でもリセットの後のプロセッサが動かなくなるので、リセットはQEMUの変換で試験する)。
- `make GL=1`で、Mesaをx86_64-elfのプロセス用に作り(`tools/x64/mesa/build.sh`、softpipeだけ)、小物「歯車」がsoftpipeで描く(1280×800の画面で320×280のウインドウに60フレーム/秒)。2026-10-19からはIntelのGPUのドライバ(iris)も同じライブラリに入る。GPUの無い機械ではsoftpipeで描く(13章13.11.9)。
- NetBSDのTCP/IPスタック(`tools/x64/netbsd/build.sh`、`~/tessronos-netbsd-x64/rumpnet.o`)とMozc(`tools/x64/mozc/build.sh`、`MOZC=1`)もx86_64で作って動く。
- ブラウザ(Chromium 153の移植、14章)も`make TARGET=_X64_ BROWSER=1 BROWSER_BLINK=1 GL=1`でx64のものを作る(14.3.1)。ARMの移植と同じソース、パッチ、移植層から、gnの`target_cpu = "x64"`、x86_64-elfのツールチェーン、NASM(12.1)で作り、V8はx64のJITとWebAssemblyで動く。WebAssemblyの範囲外の参照は、ページフォールトをプログラムに返す`ts_def_flt`(`T_TSFLT`のx86-64の形)からV8のトラップハンドラに渡る。2026-10-21(2026-10-20から作業)に、QEMUの`q35`で`https://www.tron.org/ja/`の表示と、試験グループ`v8`の86件中79件(残りはQEMU virtと同じに失敗するものと、音の装置の構成によるもの)を確かめた。IntelのGPUではirisで描く見込みで、実機ではまだ確かめていない。
- Windowsのハイパーバイザ(`-accel whpx -cpu host,vmx=off,lmce=off,sgx=off,sgxlc=off`。OVMFが読むMSRの機能は切る)の上では、TSC deadlineのタイマとPCIDを使って動く。

Windowsのハイパーバイザで走らせるときは、QEMUの引数の`-cpu max`を上の`-accel whpx -cpu host,...`に替える。デバイスの64ビットのBARは物理アドレスの上端近く(約56TB)に置かれるので、デバイスの領域の遠い枠を使う(3.10.2)。
### 3.10.6 ACPIの解釈系

AMLを実行するACPIの解釈系にはACPICAを使う。`tools/x64/acpica/build.sh`がリリース(acpica-unix-20260930、SHA-256で確かめる)を取得し、カーネルと同じ条件(x86_64-elf、浮動小数点のレジスタなし、`-mno-red-zone -mcmodel=kernel`)でコンパイルし、OSの層とカーネルの使う部分(`kernel/sysdepend/acpi/acpica/`)と合わせて`~/tessronos-acpica-x64/acpica.o`にする。デバッガ、逆アセンブラ、ASLのコンパイラは入れない。ソースは変えず、`include/platform/acenv.h`の写しで`__TESSRONOS_KERNEL__`のときに`actessronos.h`を選ばせる。カーネルが呼ぶ関数(`knl_acpica_*`)とOSの層の関数(`AcpiOs*`)のほかは局所の記号にするので、ACPICAの小さなCライブラリ(`memcpy`など)はカーネルのものとぶつからない。

| 層 | ファイル | 内容 |
| --- | --- | --- |
| OSの層 | `kernel/sysdepend/acpi/acpica/osl.c` | メモリの写し、PCIの構成空間(ECAM)、ポート、SCI、セマフォ、スピンロック、時間、遅らせて行う処理(通知用とそれ以外の2つのタスク)、FACSのグローバルロック |
| カーネルの側 | `kernel/sysdepend/acpi/acpi_os.c` | 上をカーネルの機能で作る。RAMは直接の写し、それ以外はデバイスの領域(キャッシュしない)。SCIはMADTの付け替えに従い、レベルトリガ |
| 使う所 | `kernel/sysdepend/acpi/acpica/acpica.c` | 起動(表、名前空間、ACPIモード、SCI、`_INI`、メソッドのあるGPE)、電源ボタン、再起動、名前空間の装置の一覧 |
| 使う所 | `kernel/sysdepend/acpi/acpica/acpica_off.c` | 電源断(`\_TTS`、最後の段階の予定、`\_PTS`、S5)と、起動時に出すファームウェアが残した起こしの状態 |

起動の段階「ACPI interpreter」(PCI Expressの後)で名前空間を読み込む。電源ボタンは、FADTの固定のボタンと、通知で知らせるもの(`PNP0C0C`の0x80、SurfaceのボタンのデバイスMSHW0028とMSHW0040の0xC6)を受け、デスクトップの〈電源を切る〉と同じようにセッションを終えて電源を切る。すべての通知は、どの装置の何番かをコンソールに出す(実機のボタンが何を送るかを調べるため)。再起動はFADTのリセットのレジスタで行う。電源断(`kernel/sysdepend/acpi/acpica/acpica_off.c`)は次の順で行う。

1. システムがまだ動いているうちに(`knl_finish_device`)、`\_TTS(5)`を実行する(再起動のときも)。続いてWi-Fiの接続を切る。プロセスの終了とファイルシステムの書き出しはこれより前に済んでいる(4.9)。
2. どのPCIの機能もS5から起こさないように、すべての機能の電源管理の能力(PMCSR)のPME_Enを切り、立っているPME_Statusを消す。起こす設定が残っていた機能はコンソールに出す。S5から起こすことは何にも頼んでいないので、コーデックのジャックやUSBの接続、ファームウェアが起こす用にしたままのWi-FiのPMEが、電源を切った直後にPCHのPMEのGPEやPCIeのWAKE#(PM1の有効のレジスタでは止まらない)で機械を起こしてしまうのを防ぐ。
3. 最後の段階で何をするか(PM1aとPM1bの制御のブロックか、ハードウェア縮小のときはFADTのスリープ制御のレジスタ、`\_S5`の値、`\_PTS`の有無、いま有効なGPEとS5から起こす用に残るGPE)をコンソールに出し、実身のボリュームに印を付けて外せるものを外し(`knl_fs_finish`)、そのあとで記録を最後に書いて`/boot`と`/tslog`を外す。これより後はディスクに書けないので、記録に残るのはここまでである。
4. 割込みを止め、他のプロセッサをすべて止める(SGI_STOP)。そのうえで`knl_acpi_os_alone`にする。プロセッサが止まるのは割込みが開いているところだけで、スピンロックの中では止まらないので、解釈系が待つものは止めたタスクが持っていたセマフォだけになり、それはすぐに取れたものとしてよい。
5. `AcpiEnterSleepStatePrep`で`\_PTS(5)`と`\_SST`を実行し、すべてのGPEを止め、`AcpiEnterSleepState`でS5に入る。ACPICAはPM1の状態を消し、起こす用のGPEだけを有効にし、PM1aとPM1bにそれぞれのSLP_TYPを、まずSLP_ENなしで、次にSLP_ENを付けて書く(ハードウェア縮小のときはスリープ制御のレジスタ)。各段階は画面に出す。10秒たっても切れなければACPICAがSLP_ENをもう一度書き、そのあとはWAK_STSを待ち続けるので、画面に最後の行が残ったまま止まっていれば、SLP_ENが受け付けられなかったことになる。

解釈系が動いていないときは、起動時に読んだ表だけで切る(acpi.c)。PM1の状態を消し、PM1の制御のレジスタのほかのビット(SCI_ENなど)を残したまま、SLP_TYP、SLP_ENの順に書く。ハードウェア縮小の機械はこの道では切れない。

起動のときは、表を読んだあと、ACPIモードにしてGPEの状態が消える前に、ファームウェアが残したPM1の状態とGPEの状態をコンソールに出す(`acpi: as the firmware left it`)。電源を切ったはずの機械がすぐ起き直したときは、WAK_STSと並んで、起こしたもの(PWRBTN、PCIEXP_WAKE、RTC、GPEの番号)がここに出る。

解釈系は寛容なモード(`AcpiGbl_EnableInterpreterSlack`)で動かす。Linuxが`acpi=strict`でないときの既定と同じで、ファームウェアのAMLが確かめられている振る舞いに合わせる。Surface Go 2のDSDTは、0x1003バイトのOperationRegion `OGNS`の0x1000に、DWordAccのフィールド(ECON、ECOK、LTES)を置いている。厳密なモードでは、アクセスの幅に切り上げた分が領域の外に出るのでAE_AML_REGION_LIMITになり、`EC0._REG`がECOKを立てられず、`ACAD._INI`、`ACAD._PSR`、`BAT1._STA`、`PWRB._STA`、`QCQU._STA`などが途中で止まっていた。寛容なモードでは、幅に切り上げた長さに収まるアクセスは通す。

`cmdline.txt`に`acpi=0`と書くと解釈系を起動しない。

初期化は、表の読込み、ECDTがあればその埋め込みコントローラ(EC)、ACPIモードとSCI、名前空間のオブジェクトの初期化(_STAと_INI)、`\_SB._OSC`(何も引き受けないことを伝える)、`\_PIC(1)`(割込みはIOAPIC)、ECDTがなければDSDTのEC(PNP0C09)、起こす装置のGPE(_PRW)を起こす用にとっておき、メソッドのある残りのGPEを有効にする、ボタンと通知、の順である。`_OSI`にはACPICAの既定のとおり「Windows 2000」から「Windows 2022」までのすべてに真を返す(ファームウェアはWindowsで確かめられている道筋が最も確かなので、Windowsとして振る舞う)。

ECは2つのポート(データとコマンド/状態)で読み書きし、IBFが空くのとOBFが満ちるのを1回最大500ms待つ。2バイト以上はバーストモード(0x82と0x83)で囲み、ECの装置の`_GLK`が1ならファームウェアのグローバルロックを持つ。1つの読み書きは長くかかりうるので、セマフォで1つずつにする。ECの事象はGPEで来て、SCI_EVTが立っている間クエリ(0x84)で番号を得て`_Qxx`を実行する。始めに残っている事象もくみ出す。

電池(PNP0C0A)、ACアダプタ(ACPI0003)、蓋(PNP0C0D)、電源ボタンはデバイスの実身「battery0」「acpower」「lid」「powerbutton」になる(`kernel/sysdepend/acpi/acpi_power.c`、`OB_S_POWER`)。レコード1が状態の「キー 値」の行で、電池は`_BIX`(なければ`_BIF`)と`_BST`から、在否(`_STA`のビット4)、単位(mWhかmAh)、設計容量、最後の満充電量、残量、率、電圧、残りの百分率(満充電量が分からなければ設計容量で割る)、型番などを作る。状態の`STATE`はLinuxの電池のドライバと同じに決め、放電のビットが立てば`discharging`、充電のビットなら`charging`、どちらでもなく残量が最後の満充電量(または設計容量)に届いていれば`full`、届いていなければ`notcharging`(充電のしきい値や温度で充電器が止めている)とする。残量が危ういというビットは`CRITICAL 1`の行で別に示す。

読み直す契機もLinuxに合わせた。ACアダプタは通知(0x80など)が来ると`_PSR`を読む(Linuxの`acpi_ac_notify`)。電池は通知が来ると`_STA`、`_BIX`、`_BST`を読む(`acpi_battery_notify`。0x81は容量などの情報の変化、0x80は状態の変化)。電池の0x80ではACアダプタも読み直す。抜き差しを電池の通知でしか知らせないファームウェアがあるためで、Linuxの`acpi_ac_battery_notify`と同じである。ACアダプタの通知では、upowerと同じく電池も読み直す。充電が始まったり止まったりするからである。通知を出さない機械のために、電池はupowerと同じく30秒ごとにも読む。変わればレコード1に`OB_E_CHANGE`を通知する。ACPICAの通知は、SCIの割込みの中でなく、解釈系の通知用のタスク(`osl.c`の`AcpiOsExecute`)で呼ばれるので、メソッドの実行と通知はそのタスクで行う。システム環境設定の〈電源管理〉はこの実身を読み(11.6)、変化の通知で表示を読み直す。

Surface Go 2の音量の上げる・下げるボタンと電源ボタンは、GPIOの割込みではなく、埋め込みコントローラ(EC)のクエリ(`_QD1`から`_QD6`)を通して、IntelのHIDイベントフィルタの装置(`\_SB.HIDD`、`_HID`は`INT33D5`)への通知として来る。ファームウェアは、その装置の`_DSM`(UUIDは`eeec56b3-4442-408f-a792-4edd4d758054`、リビジョン1)の機能1で「5ボタンの配列」が読み込まれるまで、どのボタンも通知しない。読み込むと、ファームウェアは`PNP0C0C`の電源ボタン(`\_SB.PWRB`)の`_STA`を0にして、電源ボタンも配列の通知で知らせる。通知の値は次のとおりで、押したときと離したときが組になる(どちらも`_OSI`の答えで決まる`OSYS`がWindows 10以上のときだけ送られる)。

| 通知 | ボタン |
| --- | --- |
| 0xC4、0xC5 | 音量を上げる(押した、離した) |
| 0xC6、0xC7 | 音量を下げる |
| 0xCE、0xCF | 電源 |

`acpi_power.c`は、起動の最後(電源ボタンの実身を作った後)に`INT33D5`を探し、`_DSM`の機能0で機能1があることを確かめてから、機能1で配列を読み込む。音量のボタンは、入力の層(6.4)には「acpibtn」という名前のキーボードとして渡す。キーはキーボードのページの使用法のVolume Up(0x80)とVolume Down(0x81)で、普通のキーボードの音量のキーと同じに、押している間はキーリピートの設定のとおりに繰り返し、ウインドウマネージャが全体の音量を変える(6.5.6)。離した通知がないまま押した通知が来たら、離したことにしてから押し直す。電源ボタンの0xCEは、固定の電源ボタンと同じくセッションを終えて電源を切る。同じ押し下げを固定のイベントと配列の両方で知らせるファームウェアのために、別の経路で1秒以内に来た押し下げは同じ押し下げとして1回だけ扱う(`acpi: the power button: the same press, told the other way too`)。配列のほかの通知(Windowsボタン、回転のロック、タブレットとノートの形)は、コンソールに出すだけにする。

コンソールには、配列を読み込んだこと(`acpi: buttons (\_SB_.HIDD, INT33D5): the 5-button array loaded ...`)と、最初の16回の押した・離した(`acpi: buttons: Volume Up pressed (c4)`など)を出す。すべての通知を出す行(`acpi: notification c4 for \_SB_.HIDD`)もそのまま出る。QEMUにはこの装置がないので、実機で初めて動く。

QEMUの`q35`で、名前空間の読込み、SCI、QEMUのモニタの`system_powerdown`(固定の電源ボタン)によるデスクトップからの電源断を確かめた。電池、ACアダプタ、蓋は、`tools/x64/tests/acpi_battery.asl`(ECを使わずに値を返し、ときどきACアダプタの通知を送る手作りのSSDT)を`iasl`(`tools/x64/acpica/build.sh`と同じリリースのもの)でAMLにし、QEMUの`-acpitable`で読ませて確かめた。ECの手順はQEMUにECがないので、実機で初めて動く。

### 3.10.7 画面とファイルによる記録

シリアルポートのない実機(Surface Go 2)では、画面の写真と記録のファイルで、どこまで進んでどこで止まったかを調べる。

画面のコンソール(`kernel/sysdepend/x64/fbcon.c`)は、ローダと同じ倍率の文字で、ローダの最後の行の下から続ける。GOPのフレームバッファが4GBより上にある機械(ThinkPad X1 Yoga 第3世代では約190GBの所)のために、ローダはその範囲もデバイスの窓に書込み結合で写しておく。カーネルは、最初にカーネルのPML4を覚えてから(`reset_main`)画面に触れ、デバイスの窓が画面の最初と最後のバイトを写していると確かめられたときだけ画面に書く。写せなければ、記録のファイルにだけ出す。最上行の青い帯には、起動の段階の名前(`knl_fbcon_stage`)を出す。段階は、the processor、ACPI tables、the processor's clock、memory management、the kernel、PCI Express、VT-d、ACPI interpreter、serial ports、NVMe、network card、screen, drawing, windows、keyboard, sound, camera、USB、network、the disk started from、processes, objects、the first programの順である。screen, drawing, windowsの中は、screen: the GOP's framebuffer、screen: the display engine under it、screen: the GGTT as the firmware left it、screen: the panel and the plane, whether the size can change、screen: the two places in the GGTT(この3つはIntelの表示エンジンのとき)、screen: the back buffer、drawing、windows、windows: doneに分けて出す。画面のコンソールは、表示のドライバが最初に画素を送るとき、または最初に大きさを変えるとき(`knl_disp_hw_taken`から`knl_fbcon_stop`)まで、帯と行を画面に出し続ける。前はドライバの初期化(`knl_disp_hw_init`)で止めていたので、デスクトップが描くまでのあいだ帯がscreen, drawing, windowsのまま残り、その後の段階で止まっても画面からは分からなかった(ThinkPad X1 Yoga 第3世代の1回目の起動で、システムボリュームの拡張まで進んでいたのに、この段で止まったように見えた)。画面の最下行は起動の見張りに取っておく。各プロセッサのLocal APICタイマの割込み(`lockup.c`、0.5秒ごと)が、今の段階が5秒を超えて続くと、その秒数、直前の1秒にタイマの割込みを受けたプロセッサの数と全体の数、書いたプロセッサ、段階の名前を赤地で出し、1秒ごとに書き直す(`knl_fbcon_watch`)。秒数が2回の撮影のあいだで増えていなければ、どのプロセッサも割込みを受けていない。数が全体より少なければ、割込みを止めたまま動かないか、例外で止まったプロセッサがある。そのときは、そのうち最初のプロセッサの番号、例外で止まったか(stopped by an exception)割込みを受けていないだけか(silent)、最後にタイマの割込みを受けてからの秒数、そのときのRIPとタスクを出し、ほかにもあればその数を(+N)で添える。見張りは、ドライバが画面を受け取ると終わる。止まる例外のときは画面を消し、赤い帯に最後の段階を出して、例外の種類とベクタ、エラーコード、RIP、CR2、全レジスタ、タスク、呼出し元をたどったアドレスと、その直前のコンソールの行(画面に入るだけ)を出す。デスクトップが画面を使っている間に止まったときも同じである。例外の要約(種類、RIP、CR2、エラーコード、タスク、RSP、RBP、段階)の2行は、赤い帯のすぐ下に黄色で残す(`knl_fbcon_pin`)。コンソールの行は、画面の下まで来るとその要約の下から続くので、例外の後にほかのプロセッサがロックアップの報告などを長く出しても、要約は消えない。前は要約も普通の行で、画面の上に戻ってきた行に上書きされていた。

止まったときは、記録のファイル(3.10.7)をその場で書く。システムのタイマと機器の割込みは最初のプロセッサが受けるので、例外を起こしたプロセッサをすぐ止めると、ほかのプロセッサも時間切れや入出力を待てなくなり、記録を書けない。そこで、タスクの中で、割込みが許され、ディスパッチが禁止されておらず、カーネルのロックを持たずに起きた例外では、報告の後にそのタスクを眠らせたままにし、プロセッサは止めない。記録の係のタスク(`bootlog.c`、0.25秒ごとに見る)がすぐに`KERNEL.TXT`などを書き、書き終えたら全プロセッサを止めて(`knl_exc_stop_all`)、画面を赤い帯、要約、コンソールの最後の行で描き直す。15秒で書き終わらなければ(ディスクが答えない)、眠らせたタスクが自分で止める。割込みの処理中の例外、二重フォールト、マシンチェック、スタックのあふれ(`!STACK!`)はその場で止め、書けるなら書く(`knl_bootlog_stopped`)。ロックアップの報告(プロセッサが割込みを受けない、カーネルのロックが離されない、ウォッチドッグが蹴られない)のときも記録を書かせる。デスクトップのループが回らないときは、戻ることもあるので2秒後に書く。

ローダの最後の段階からカーネルの画面のコンソールが始まるまで(ExitBootServicesの前後、メモリマップの変換、カーネルへの飛び込み、カーネルの入口、例外の表、文字の部品)は、帯も文字も出ない。そこで段階ごとに、画面の左下に色の違う四角を左から順に描く(`bootinfo.h`の`BI_MK_*`、色と段階の対応は[x64の実機で試す手順](../x64-hw-test.md)の7章)。カーネルはローダの写したデバイスの窓を通して直接描く。画面のコンソールの前に例外が起きたときは、赤い四角に続けて、例外の番号、RIP、CR2、エラーコードを16進の点の数字で描く。段階の番号と例外は、ローダが物理アドレス0x8F000(取れなければ無し)に確保したページにも書く。温かい再起動の後、次のローダが「the last start got to mark 番号」と前回の段階を記録に出す。起動の設定は、TSLOGの`cmdline.txt`があればESPのものの代わりに使う(Windowsで書き換えられるように)。

ローダは、カーネルのイメージとヒープ、T_BOOTINFO、ページテーブル、1MB未満のページ、段階の記録のページを、すべてUEFIの標準の種類EfiLoaderDataで確保する。確保した範囲を覚えておき、メモリマップを変換するときにその範囲だけをカーネルのもの(`BI_MEM_KERNEL`)として渡す。以前はOSのローダ用の独自の種類0x80000001で確保していた。その種類では、ThinkPad X1 Yoga 第3世代(LenovoのファームウェアN25ET72W)でExitBootServicesから戻らず、段階の四角が3つで止まった。EfiLoaderDataに替えると、デスクトップまで起動した。同じイメージで`memtype=custom`にすると再び止まり、原因はメモリの種類と確定した。QEMUのOVMFはどちらの種類でも動く。比べるときは、起動の設定`memtype=custom`で以前の種類に戻せる(検討書5.12)。

コンソールに出たものは512KBの環に保ち(`kernel/sysdepend/x64/bootlog.c`)、起動の20秒後と、その後は1分ごとに、ESPの`/boot/EFI/TESSRON/`に書く。USBの記憶装置を詰まらせないよう、前に書いてからコンソールに何も増えていなければ書かず、`KERNEL.TXT`と`USB.TXT`はまとめて書き、`ACPI.TXT`は最初の1回だけ、`DISPLAY.TXT`は中身が変わったときだけ書く。解像度の変更やWi-Fiの接続の失敗など、残したい出来事のあとは2秒後に書く(`knl_bootlog_soon`)。電源を切るときと再起動するときは、`knl_finish_device`から、ディスクと割込みがまだ動いているうちに最後に書き(`knl_bootlog_final`)、`/tslog`と`/boot`をアンマウントする(`knl_bootlog_unmount`)。アンマウントすると使用中の印が消えるので、次の起動でこれらのFATの見直し(`fat_check`)を省ける。前は印が残り、SDカードからの起動のたびに約2万8千回の読取りで約20秒かかっていた。`/boot`がマウントされていないとき(ディスクが外れて戻った後)は、スリープと電源断の道では書かない。見直しの間はファイルの呼出しがすべて待たされ、電源断が20秒以上延びるためである。その場合は最後の記録が残らない。FATの見直しをしたときは、かかった時間をコンソールに出す(`fat: uda0 was not unmounted: looked over in ... ms`)。

起動したディスクが抜かれると、`/boot`と`/tslog`も去り、書けなくなる。コンソールの環はメモリにあるので、抜かれている間に出た行も残っている。同じディスクが挿し直されると、書く前に、ローダが渡したESPの区画のGUIDでディスクを探し直し(`bootdisk.c`の`knl_bootdisk_again`。挿し直すと`uda`が`udb`になるなど名前が変わることがある)、`/boot`と`/tslog`を付け直して書く(`bootlog.c`の`boot_again`、コンソールに`bootlog: /boot on udb0 again (0)`)。抜かれている間は、探しても何も出さない。カードが戻らないときのために、デスクトップは、システムのボリュームが去ったときに、コンソールの最後の40行を新しい行を上にしてウインドウに出す(11.26)。UEFIの変数と内蔵のSSDには書かない決まりなので、次の起動まで持ち越す形(メモリに残して次の起動で書く)は、ファームウェアが再起動でメモリを消さないと確かめられるまで採らない。

同じ記録を、記録の区画TSLOGの`TESSRON/`にも書く。TSLOGは`tools/mksd.py --log-mb`(x64のイメージでは`SD_LOG_MB`、既定は64MB)が作るFAT32の区画である。種類は基本データ(EBD0A0A2-B9E5-4433-87C0-68B6B72699C7)、名前とボリュームのラベルはTSLOGである。

- **置き場所**: Windowsは、取り外しできる装置ではESPにドライブ文字を付けない。ESPの記録はdiskpartで区画を選ばないと取り出せず、その操作も「リムーバブル メディアではサポートされていません」で断られることがある。Windows 10 バージョン1703以降は、取り外しできる装置の2つ目以降の区画にもドライブ文字を付ける。そこでTSLOGは、エクスプローラにTSLOGのドライブとして出るようにした。
- **区画の位置と表の順**: 区画の位置はESPのすぐ後で、システムのボリュームが最後の区画のまま最初の起動で広がれるようにしてある(7.7)。GPTの表では最後の項目なので、ほかの区画の番号(`nvmea0`が`/boot`、`nvmea1`がシステムのボリューム)は変わらない。
- **ローダの書き方**: ローダは、自分の来たディスク(デバイスパスのハードディスクの節より前が同じもの)の中から、UEFIのファイルシステムでボリュームのラベルがTSLOGのものを探して書く。見つからなければESPにだけ書く。
- **カーネルの書き方**: カーネルは、起動したディスクのGPTから種類と名前でTSLOGを見つけ、`/tslog`に取り付けて書く(`kernel/sysdepend/x64/bootdisk.c`)。

| ファイル | 書くもの | 内容 |
| --- | --- | --- |
| `LOADER.TXT` | ローダ | ローダが出した文字 |
| `HW.TXT` | ローダ | ファームウェア、CPUIDの全リーフと主な機能、GOPのモードの一覧、UEFIのメモリマップ、SMBIOS(機種、ボード、メモリ)、ACPIの表の一覧とFADTの主な値、PCIの全ファンクション(ID、クラス、BAR、IRQ、ケーパビリティ)、UEFIが列挙したUSBの装置(VID、PID、文字列、全部の構成の記述子とその解析) |
| `ACPI/*.BIN` | ローダ | ACPIの表そのもの(`DSDT.BIN`、`SSDT1.BIN`…、`FACS.BIN`) |
| `SMBIOS.BIN` | ローダ | SMBIOSの構造の表そのもの |
| `KERNEL.TXT` | カーネル | カーネルのコンソール(起動したディスク、各段階、デバイスの発見) |
| `USB.TXT` | カーネル | カーネルが立ち上げたUSBの装置(VID、PID、速度、つながり方、全部の構成の記述子とその解析)。USBのディスクごとの読み、書き、キャッシュの書き出し(SYNCHRONIZE CACHE)の回数、合計の時間、一番長くかかった時間 |
| `ACPI.TXT` | カーネル | 名前空間の装置(パス、`_HID`、`_CID`、`_UID`、`_ADR`、`_STA`、`_CRS`のメモリ、ポート、割込み、GPIO、I2C、SPI、UART) |
| `DISPLAY.TXT` | カーネル | Intelの内蔵グラフィックス(第9世代)の表示エンジンを、ファームウェアが設定したまま読んだもの。CDCLK、DPLL、DDI、トランスコーダとそのタイミング、パイプの大きさ、パイプのスケーラ、プライマリプレーン、パネルの電源とバックライト、GGTTの先頭とプレーンの面のエントリ、BAR、スチールンメモリ、OpRegionとVBTの有無。読むだけで書かない(`kernel/sysdepend/x64/gfxdump.c`)。最後の節に、表示のドライバが解像度を変えたときの手順(計算した値、書いた値、反映、アンダーラン、書き戻し。6.9)を並べる。解像度を変えた2秒後にも書き直す。Intelの画面が無ければ作らない |

`cmdline.txt`(ESPの根、1行)には、次の設定を空白で区切って書ける。ほかのPCで書き換えられるので、実機を作り直したイメージなしで試せる。

| 設定 | 意味 |
| --- | --- |
| `video=WxH` | GOPの画面のモード。書かなければ機種で決めたモード(ThinkPad X1 Yoga 第3世代は1600×1200)、それも無ければ1920×1280以下で最も大きいもの |
| `hwreport=0` | ローダの報告(`HW.TXT`など)を作らない |
| `connect=0` | 報告の前にUEFIの全部のコントローラをつなぎ直さない(USBの一覧はファームウェアがつないだ分だけになる) |
| `keepmode=1` | GOPの画面のモードを変えない |
| `memtype=custom` | カーネルの領域を、OSのローダ用の独自のメモリの種類(0x80000001)で確保する(比べるため) |
| `lastlog=0` | ブートサービスを抜ける直前に`LOADER.TXT`を書き直さない |
| `tslog=0` | ローダはTSLOGに記録を書かない(TSLOGの`cmdline.txt`は読む。カーネルは書く) |
| `acpi=0` | ACPIの解釈系を起動しない |
| `screens=N` | ビューをN個の画面に分ける(10.4.8) |
| `vmd=off` | Intel VMDに触らない(裏のNVMeは見えない。06-device 6.2.2) |
| `vtd=keep` | VT-dの表とレジスタを記録に出すだけで、ファームウェアが有効のまま渡した変換、割込みの再割当て、無効化のキュー、保護範囲を切らない(3.10.9) |
| `stopat=名前` | 名前で始まる段階で止まり、コンソールを画面に残す(例: `stopat=USB`) |
| `faultat=名前` | その段階で無効命令の例外を起こし、止まった画面を確かめる |

### 3.10.8 実機(Surface Go 2、ThinkPad X1 Yoga)で起動する

実機の試験用のイメージは`make TARGET=_X64_ DESKTOP=1 BUILD_ID=hw2 X64_CMDLINE= sdimg`で作る(`build_make/obj/_X64_hw2/sd.img`、Windowsからは`C:\home\TessronOS\build_make\obj\_X64_hw2\sd.img`。作るスクリプトは`C:\tmp\mkimg_x64hw2.sh`)。`X64_CMDLINE=`を空にして、画面のモードはローダに自動で選ばせる(ThinkPad X1 Yoga 第3世代は1600×1200、Surface Go 2は1920×1280以下で最も大きいもの)。ThinkPad X1 Yoga(第3世代)の2560×1440で試すときは、`cmdline.txt`に`video=2560x1440`と書く。両方の機種での試し方は、[x64の実機で試す手順](../x64-hw-test.md)にまとめてある。

USBメモリ(1GB以上。中身は消える)への書込みは、Windowsで次のどちらかで行う。

- Rufus: 「デバイス」でUSBメモリを選び、「ブートの種類」の「選択」で`sd.img`を選んで「スタート」。書込みの方式を聞かれたら「DDイメージモード」を選ぶ。
- Raspberry Pi Imager: 「OS」で「カスタムイメージを使う」を選んで`sd.img`を指定し、「ストレージ」でUSBメモリを選んで書く。OSの設定の変更は使わない。

Surface Go 2の準備と起動は次のとおりである。

1. Windowsでデバイスの暗号化(BitLocker)を使っているときは、回復キーを手元に控えるか、BitLockerを一時停止しておく。セキュアブートを変えると、次のWindowsの起動で回復キーを求められることがある。
2. Surfaceの電源を切る。音量の上げるボタンを押したまま電源ボタンを押して離し、Surfaceのロゴが出たら音量のボタンを離すと、Surface UEFIの画面になる。
3. 「Security」の「Secure Boot」を無効にする(「Disabled」、または構成の選択で「None」)。「Boot configuration」で「USB Storage」が有効であることを確かめる。「Exit」から「Restart now」で抜け、Windowsが起動したらシャットダウンする。
4. USBメモリを挿す(USB-Cの変換やハブ経由でもよい)。Type Coverと、あればUSBのLANのアダプタもつないでおく。
5. 音量の下げるボタンを押したまま電源ボタンを押して離し、ロゴが出たら音量のボタンを離すと、USBメモリから起動する。

起動の途中で止まったときは、画面の写真を撮る。帯の段階の名前と、赤い帯の画面ならその全体が手がかりになる。動いたときも止まったときも、電源を切った後にUSBメモリをWindowsのPCに挿し、エクスプローラでTSLOGのドライブの`TESSRON`のフォルダを丸ごとコピーする(3.10.7)。

TessronOSは起動したUSBメモリにだけ書く。内蔵のSSDは読んで装置として見せるが、利用者が取り付けるか、小物「システム登録」で登録する(3.10.10)のでない限り書かない。

### 3.10.9 VT-d(IOMMU)

TessronOSはVT-dを使わない。ドライバは装置に物理アドレスをそのまま渡し、MSIとMSI-Xのメッセージは互換の形式(アドレス0xFEExxxxxに宛先のAPIC ID、データにベクタ)で書く。ところが、起動前のDMAの保護を行うファームウェアは、VT-dの再割当ての単位(DRHD)の変換、割込みの再割当て、保護範囲(PMR)を有効のまま渡すことがある。そうした単位の下の装置は、DMAが止まるか別の所へ行き、互換の形式のメッセージは割込みの再割当てで止められる(GSTS.CFISが0のとき)。有効のまま渡されなかった単位の下の装置は普通に動く。

そこで起動の段階「VT-d」(PCI Expressの走査の直後、どの装置も始める前)で、`kernel/sysdepend/x64/vtd.c`が次のことを行う。

1. ACPIのDMARの表を全部コンソール(KERNEL.TXT)に出す。表のフラグ(割込みの再割当て、x2APICのオプトアウト、DMAの保護の要求)、DRHDごとのレジスタの基底と対象の装置(デバイススコープ)、RMRR(DMA用に取っておくメモリの範囲と装置)、ATSR、RHSA、ANDD。
2. DRHDごとに、VER、CAP、ECAP、GSTS、RTADDR、FSTS、FECTL、PMENと低い保護範囲と高い保護範囲、IQA、IQH、IQT、IRTAを読んで出す。障害記録(CAPのFROとNFRの示す場所)のうち印(F)の立ったものを、要求元の装置(バス、デバイス、ファンクション)、理由の番号と意味、読みか書きか割込みか、アドレスで出す。
3. 有効のまま渡されたものを、変換(TES)、割込みの再割当て(IRES)、無効化のキュー(QIES)、保護範囲(PMEN.EPM)の順に切る。GCMDは書くだけのレジスタなので、VT-dの仕様の手順に従い、GSTSを読んで一度きりの印(RTPS、FLS、WBFS、IRTPS)を落とし、変える印だけを落として残りをそのまま書き戻し、GSTSの印が落ちるのを最大100ms待つ。キューは頭(IQH)が尾(IQT)に追いつくのを待ってから切る。保護範囲はPMENのEPMを落とし、PRSが落ちるのを待つ。切った後のレジスタをもう一度出す。

切ってよい根拠は次のとおりである。TessronOSは再割当ての表を作らず、変換のための表も持たないので、変換が有効のままではファームウェアの表のとおりにしか装置のDMAが届かず、ドライバの渡す物理アドレスとは合わない。割込みの再割当てが有効のままでは、互換の形式のメッセージは仕様上止められるか(CFIS=0)、ファームウェアの表に従って別の所へ送られる。どちらもTessronOSのドライバの前提と合わない。ExitBootServicesの後はファームウェアの装置のドライバは動いていないので、切ってもファームウェアの側で困るものはない。表示のエンジンはGGTTを通してstolenメモリを読むが、その物理アドレスは変換を切っても変わらない。

`cmdline.txt`に`vtd=keep`と書くと、1と2だけを行い、何も切らない(比べるため)。後で装置が答えないときに、障害記録を出し直す関数`knl_vtd_faults`がある(Wi-Fiのカードが起動しなかったときに使う)。

### 3.10.9.1 AMDの機種

AMDのプロセッサの実機は手元にない。TCGの`-cpu max`もベンダはAuthenticAMDと答える。ここに書くことは、QEMUの`-cpu EPYC-Rome`などで起動とテストを通したか、コードを読んで直したものであり、実機では未確認である。Intelだけの前提を洗い出した結果は次のとおりである。

- SYSRET: AMDのSYSRETはSSのセレクタだけを読み込み、記述子のキャッシュを更新しない。割込みで入ったタスクから切り替わるとSSがヌルのままなので、`vector.S`でSYSRETの前にSSを読み込み直す。
- TSCとタイマ: CPUIDの15hはIntelだけなので、HPET、PMタイマの順で測る。AMDのチップセットのHPETは32ビットのカウンタなので、下位のワードだけを読み、桁あふれを引き算で扱う。ZenにはTSCデッドラインがない機種があり、そのときはローカルAPICのワンショットを使う(`-cpu EPYC-Rome`で通過)。
- CPUの判定: 起動の記録に、ベンダ、シグネチャ、CPUIDの機能のワードを出す。HWPと温度のMSRはIntelの判定の中にある。`MSR_TSC_ADJUST`はCPUIDで確かめてから使う。
- 乱数: RDSEEDとRDRANDが成功と言いながら全部0や全部1を返す不具合がAMDの一部の世代にあるので、その値を捨てる。RDSEEDが値を返さなければRDRANDに替える。
- IOMMU: VT-dの表の代わりにIVRSを読み、各ユニットの制御レジスタの`IommuEn`が立っていれば切る(`vtd.c`、`vtd=keep`で切らない)。割込みの再割当てが残ると、MSIの互換形式が届かないからである。S3の後も同じ。
- スリープ: `PNP0D80`の`_DSM`がIntelのUUIDに答えないとき、AMDのUUID(e3f32452-febc-43ce-9039-932122d37721)を試す。関数の番号が違うので読み替える。深い状態にはSMUへの指示が要るが、それは作っていないので、既定では低電力S0アイドルを選ばない。`\_S3`があればS3、なければスリープしない。`sleep=s0`で試せる。LPITはないので、カウンタの記録は0になる。
- PCIの表: 機能の数は機種で違い、AMDは32を超えることが多いので、固定の表をやめて足りなくなると広げる。
- 機器: xHCI、NVMe、HD Audio、AHCIのドライバはクラスで探し、Intelだけの処理はベンダIDの判定の中にある。起動の設定`pcifake=amd`(`KTEST`のビルドだけ)で、表のベンダとデバイスのIDをAMDのものに替えて`pcdev`の群を通す。GPUはGOPのままで、amdgpuは作らない。

AMDの実機で試すときに見る記録は、`KERNEL.TXT`の`TessronOS x64: CPU`、`TSC`、`Local APIC`、`iommu:`、`pm:`、`acpi: sleep:`の各行である。

### 3.10.10 内蔵のディスクへの登録(システム登録)

microSDカードやUSBメモリから起動したTessronOSを、PCの内蔵のディスク(NVMeかSATA)に写して、そこから起動できるようにする。対象はx64のPC(Surface Go 2、ThinkPad X1 Yoga 第3世代)だけである。デスクトップの中で動く小物「システム登録」(`application/sysinst/sysinst.c`、11.32)が画面を受け持ち、書き方の中心は`application/sysinst/si_core.c`にまとめてある。写すものは、いま動いているシステムボリュームの全体(利用者の実身、設定、入れたアプリ)と、起動したESPのTessronOSのファイル(ローダ、カーネル、起動の設定)である。写し先には、起動したディスクとUSBのディスクを出さない(候補は`nvmea`〜`nvmed`と`sataa`〜`satad`のうち、`/boot`のディスクでないもの)。セクタが512バイトのディスクだけを扱う。

方法は3つある。

| 方法 | 区画表 | TessronOSの区画 | ESP |
| --- | --- | --- | --- |
| Windowsと共存する(既定) | `knl_gpt_add`で、使っている項目の後に1項目を足す(7.7)。主と予備の表をCRCとともに書き直し、既存の項目と区画の中身は変えない | Windowsで縮めて作った未割り当ての領域のうち最も大きいもの(1MiB境界)。前に登録した区画(TessronOSの型の最初のもの)があれば、それを書き直す | 既存のESPに、FATのファイルシステムを通して`\EFI\TESSRON\`だけを足す。`\EFI\Microsoft\`と`\EFI\Boot\bootx64.efi`は変えない |
| 区画を1つ消して使う | `knl_gpt_retype`で、選んだ区画の項目の型をTessronOSのものに、名前を「TessronOS」に、固有GUIDを新しいものに書き換え、属性を消す。項目の番号と位置は変えない。すぐ後ろに未割り当ての領域があれば、`knl_gpt_grow`でそこまで広げる。ほかの項目と区画の中身は変えない | 選んだ区画(Windowsのボリューム、Linux、TessronOSのもの) | 共存と同じ |
| ディスク全体を消して使う | 新しいGPT(保護MBR、128項目、主と予備)を書く | ESPの後ろ、ディスクの終わりまで | ESP(ディスクが32GB未満なら260MB、それ以上なら512MB)をFAT32で作り、`\EFI\BOOT\BOOTX64.EFI`と`\EFI\TESSRON\`を置く |

ESPに置くファイルは次のとおりである。ローダは、自分のファイルのあるディレクトリ、`\EFI\TESSRON\`、根の順にカーネルと設定を探す(3.10.1)ので、どちらの方法でも同じ置き方で起動できる。

| ファイル | 中身 |
| --- | --- |
| `\EFI\TESSRON\BOOTX64.EFI` | 起動したESPのローダ(`\EFI\TESSRON\`か`\EFI\BOOT\`のうち、文字列「TessronOS loader」を含むもの。WindowsのESPの同じ名前のファイルと取り違えないため) |
| `\EFI\TESSRON\tessronos.elf` | カーネル |
| `\EFI\TESSRON\cmdline.txt` | 起動の設定(あれば) |
| `\EFI\TESSRON\INSTALL.TXT` | 登録の記録(方法`coexist`・`replace`・`erase`、写し元、写し先、日時) |
| `\EFI\BOOT\BOOTX64.EFI` | ディスク全体を使うときだけ。ローダの写し(ファームウェアの既定の起動) |

システムのバージョンアップ(11.8.1)は、`INSTALL.TXT`があるESPでは、ローダ、カーネル、起動の設定を`\EFI\TESSRON\`の下で入れ替え、ESPの根には何も足さない。`\EFI\BOOT\BOOTX64.EFI`は、文字列「TessronOS loader」を含むとき(ディスク全体を使ったとき)だけ更新し、Windowsのものには書かない。

区画を1つ消して使うときに選べる区画は、型が基本データ、Linux、TessronOSのいずれかで、属性のビット0(プラットフォームに要る)が立っていないものである。基本データの区画でも、属性のビット63(ドライブ文字を付けない)が立っているものは、メーカーの回復イメージのことがあるので選べない。ESP、Microsoft予約、Windows回復の区画、型の分からない区画は残す。Linuxのインストーラが既存の区画を選んで使うときと同じく、選んだ区画の場所と番号はそのままにして中身だけを書き換えるので、後ろのWindows回復の区画の番号も変わらない。Windowsのボリュームを消したときは、ESPの`\EFI\Microsoft\`は残るが、その起動の項目は起動できなくなる。

ローダの記録(`LOADER.TXT`、`HW.TXT`、`ACPI\`)は、内蔵のディスクから起動したときも同じ`\EFI\TESSRON\`に書く。共存のときにESPの空きを確かめる量は、置くファイルの大きさに記録の分の4MBを足したものである。WindowsのESPが100MBで空きが足りなければ、そう示して何も書かない。

#### システムボリュームの写し方

動いているボリュームをそのまま写すと、写している間の書込みで中身が食い違う。Linuxのファイルシステムの凍結(`fsfreeze`)と、使っているブロックだけを写す`dump`や`xfs_copy`と同じ考え方で、次のように写す(`si_vol_copy`)。

1. ジャーナルを凍結する(`knl_jrnl_freeze`。容量の拡張と同じ仕組み、7.3)。新しいトランザクションは始まらず、走っているものの終わりを最大5秒待ち、ジャーナルの中身を本来の場所に書き終える。TSFSは実身のレコードのデータもトランザクションの中で書く(順序付けの方式)ので、凍結の間はボリュームのどのブロックも変わらない。終わらないトランザクションがあるときは、0.5秒おいて6回まで試す。
2. `ts_sync_blk`で割当て群の見出しとスーパーブロックを書き、装置のキャッシュを空にする。媒体の上のボリュームが、ある一瞬のものになる。
3. 最初の割当て群の前(スーパーブロック2つとジャーナル)、各群の見出しと空きビットマップ、ビットマップで使用中のブロックだけを、区画の装置から読んで写し先に書く(`ts_blk_group`、`ts_blk_map`)。続いたブロックは1つの要求(装置の`TDN_IOMAX`まで、最大1MB)にまとめ、要求ごとのCRC-32を覚える。空きのブロックは読まない。
4. 凍結を解き、写し先を読み返して、覚えたCRCと比べる。

凍結の間は、システムボリュームに書く操作(ウインドウの位置の保存など)が待たされる。読むだけの操作と、写しの進み具合の表示は止まらない。写したボリュームは元と同じボリュームのUUIDを持つ。TSFSのブロックの見出しがボリュームのUUIDを持つので、変えると全ブロックを書き直すことになるためである。同じUUIDのボリュームを同時に取り付けることは実身の管理が断る(`E_OBJ`)ので、内蔵のディスクから起動したときは、元のmicroSDのボリュームは取り付けられない。スーパーブロックは使用中の印のまま写るので、次に開くときはジャーナルを読み直して(空である)開く。

写し先の区画はボリュームより大きいので、ボリュームを区画の大きさに広げる(`knl_tsfs_part_grow`、7.7)。共存のときは写した直後に広げる。ディスク全体を使うときは、ドライバが起動のときに読んだ区画を持ち続ける(`knl_part_rescan`は使用中かもしれない区画が動くことを許さない)ので、新しい区画の装置はその起動の間には無い。そのときは、内蔵のディスクから最初に起動したときに、システムボリュームを区画の大きさに合わせる処理(`knl_sysvol_grow`)が広げる。ボリュームは元より小さくできないので、写し先の区画は元のボリューム(USBメモリで起動した最初に8GBまで広がっていることがある)以上の大きさが要る。

#### ファームウェアの起動の項目

カーネルはUEFIのランタイムサービスを使わない(3.10.1)。ランタイムサービスを使えるようにするには、その領域をカーネルの空間に写し、`SetVirtualAddressMap`を呼び、Microsoftの呼出し規約と排他を用意する必要があり、大きい。そこで、起動の項目はローダがブートサービスの間に書く。システム登録は、起動したESPに依頼のファイル`\EFI\TESSRON\BOOTREG.TXT`を置く。中身は、写し先のESPの区画の番号(表の項目の番号、1から)、開始のセクタ、セクタの数、固有GUID(表のままのバイト列の16進)、ローダのパス`\EFI\TESSRON\BOOTX64.EFI`、BootOrderの先頭に置くか(ディスク全体を使ったときと、ディスクにWindowsのボリューム(基本データの区画)が1つも残らないとき1)である。

次にそのmicroSD(USBメモリ)から起動したとき、ローダは画面の準備の後で依頼を読み、すぐに消してから(ファームウェアが断っても毎回繰り返さないため)、次のことを行う(`boot_entry_request`、`tools/x64/uefi/loader.c`)。

1. 起動の項目(`EFI_LOAD_OPTION`)を作る。属性は`LOAD_OPTION_ACTIVE`、名前は「TessronOS」、装置パスはハードドライブの節(GPTの署名、区画の番号、開始、大きさ)とファイルパスの節である。ハードドライブの節から始まる短い装置パスは、ファームウェアが区画のGUIDで探す(UEFIの仕様の3.1.2)。
2. BootOrderの項目のうち同じ装置パスを持つものがあればその番号、無ければ使われていない最も小さい番号の`Boot####`に書く。
3. BootOrderに入れる。依頼が先頭を求めるとき(ディスクにWindowsが残らないとき)は先頭に置く。そうでないときは、すでにBootOrderにある項目はその位置のままにし(前の登録や利用者が先頭にした項目を後ろへ下げない)、新しい項目だけを最後に足す(既定はWindowsのまま)。Linuxのブートローダを入れ直したときと同じく、入れ直しで起動の順が変わらないようにするためである。
4. `BootNext`をその番号にし、記録を書いてから`ResetSystem`で起動し直す。ファームウェアは1回だけ内蔵のディスクのTessronOSを起動する。

ファームウェアが変数を書けなかったときはその旨を記録に書き、そのままmicroSDのTessronOSを起動する。

#### 手順と注意

1. Windowsで、ディスクの管理の「ボリュームの縮小」でCドライブを縮め、未割り当ての領域を作る(共存のとき)。区画を1つ消して使うときは要らない。大きさは、TessronOSのシステムボリューム(USBメモリなら最大8GB)より大きくする。
2. BitLockerやデバイスの暗号化を使っているときは、回復キーを控えておく。ESPにファイルを足すことや起動の項目を足すことは差し支えないが、Secure Bootを切ると(TessronOSのローダは署名が無いので、microSDから起動するにも切る必要がある)、次のWindowsの起動で回復キーを求められる。
3. microSD(USBメモリ)から起動する(Surface Go 2は音量の上げるボタンを押したまま電源ボタンでSurface UEFIに入り、起動の設定で選ぶ。ThinkPad X1 Yogaは起動のときにF12で起動のメニュー、3.10.8)。
4. 小物「システム登録」を開き、写し先のディスクと方法を選んで[開始]を押す。区画を1つ消して使うときは、〈消す区画〉の一覧から区画を選ぶ。区画を消すときとディスク全体を使うときは、消える区画の一覧(番号、種類、大きさ、名前、BitLockerの印)が出るので確かめる。管理者のパスワードを入れると始まる。
5. 終わったら[再起動]を押し、もう一度microSDから起動する(3と同じ操作)。ローダが起動の項目を登録して、内蔵のディスクのTessronOSで起動し直す。
6. その後は、ディスク全体を使ったときと、ディスクにWindowsのボリュームが残らないときは、microSDを抜けば内蔵のディスクから起動する。共存のときは、Surface Go 2はSurface UEFIの「Boot configuration」で、ThinkPad X1 YogaはF12の起動のメニューで「TessronOS」を選ぶ。

起動の項目の登録が使えない機械では、ファームウェアの起動のメニューの「ファイルから起動」などで`\EFI\TESSRON\BOOTX64.EFI`を選ぶ。ディスク全体を使ったときは`\EFI\BOOT\BOOTX64.EFI`があるので、ディスクを選ぶだけで起動する。Windowsを消したとき、ファームウェアに残る「Windows Boot Manager」の項目は、その実体が無いので選んでも起動しない。

内蔵のディスクから起動したカーネルは、ローダが渡すESPの固有GUIDでそのディスクを選び、そのディスクのESPを`/boot`に、最初のTessronOSの区画をシステムボリュームにする(`bootdisk.c`)。共存のときは、ESPはWindowsのもの、システムボリュームは足した区画である。microSDを挿したまま内蔵のディスクから起動しても、microSDには書かない。

#### 試験

`tests/ktest/x86_64/ktest_sysinst.c`(群`sysinst`)が、`tools/tests/mkwindisk.py`で作るWindowsの形のディスク(ESP 200MBにWindowsの起動のファイル、MSR 16MB、基本データ 96MB、未割り当て、回復 64MBを最後に。基本データの区画の名前`KTWIN-C`が共存、`KTWIN-E`が全消去、`KTWIN-R`が区画を消して使う写し先)を、`tools/x64/ktest64.sh`の`NVME2`、`NVME3`、`NVME4`で2台目から4台目のNVMeとして挿して確かめる。

- 起動したディスクは候補に出ず、その上の計画は`SI_R_BOOTDISK`になる。
- 全消去では、元の4区画が消えるものとして計画に並び、書いた後の表はESPとTessronOSの2項目で、主と予備の表がCRCを通り、ESPがFAT32で、TessronOSの区画のスーパーブロックがシステムボリュームのUUIDを持つ。
- 共存では、元の4項目が同じで、MSR、基本データ、回復の区画の中身のCRCが書く前と同じであり、ESPのWindowsのファイルのCRCも同じで、5つ目の項目「TessronOS」が未割り当ての領域にでき、`\EFI\TESSRON\`のローダとカーネルが`/boot`のものと同じである。写したボリュームはシステムボリュームと同じUUIDで、区画の大きさに広がり、ディスク修復の検査(通常)で障害が無い。`/boot`に依頼のファイルがある。
- 区画を消して使うときは、ESP、MSR、回復の区画が選べず(計画は`SI_R_KEEP`)、基本データの区画を選んだ計画は後ろの未割り当ての領域の終わりまでになる。書いた後の表は元と同じ4項目で、3つ目が同じ位置と番号のままTessronOSの型と名前、新しい固有GUIDになり、未割り当ての領域の終わりまで広がる。ほかの項目と、MSRと回復の区画の中身のCRC、ESPのWindowsのファイルのCRCは書く前と同じで、写したボリュームはディスク修復の検査で障害が無い。
- 登録したディスクから起動したとき(ESPに`INSTALL.TXT`がある)は、`/boot`とシステムボリュームがそのディスクにあり、ボリュームが区画の大きさになっていることだけを確かめる。

ホストでは`mkwindisk.py check`(共存の後のイメージ。元の区画のSHA-256、Windowsのファイル、新しい区画とTSFSの印、`\EFI\TESSRON\`)、`mkwindisk.py check-replace`(区画を消した後のイメージ)と`mkwindisk.py check-erase`で、カーネルとは別の読み手が確かめる。`ktest64.sh`の`VARS`にOVMFの変数のファイル(`edk2-i386-vars.fd`の写し)を与えると、起動の項目が次の起動に残るので、登録の依頼のあるイメージで起動し(ローダが項目を書いて起動し直し、`-no-reboot`でQEMUが終わる)、もう一度起動すると`BootNext`で共存のディスクから起動する。全消去のディスクは、そのイメージだけを挿して起動できる。

手順は次のとおりである。

```sh
make TARGET=_X64_ KTEST=1 KTONLY=sysinst BUILD_ID=<名前> sdimg
python tools/tests/mkwindisk.py make <dir>/winc.img C --free-mb 1200
python tools/tests/mkwindisk.py make <dir>/wine.img E --free-mb 1200
python tools/tests/mkwindisk.py make <dir>/winr.img R --free-mb 1200
cp "C:/Program Files/qemu/share/edk2-i386-vars.fd" <dir>/vars.fd
NVME2=<dir>/winc.img NVME3=<dir>/wine.img VARS=<dir>/vars.fd \
  bash tools/x64/ktest64.sh build_make/obj/_X64__ktest<名前>/sd.img 900 <dir>/a.log
python tools/tests/mkwindisk.py check <dir>/winc.img
python tools/tests/mkwindisk.py check-erase <dir>/wine.img
NVME2=<dir>/winc.img VARS=<dir>/vars.fd bash tools/x64/ktest64.sh build_make/obj/_X64__ktest<名前>/sd.img 300 <dir>/b.log
NVME2=<dir>/winc.img VARS=<dir>/vars.fd bash tools/x64/ktest64.sh build_make/obj/_X64__ktest<名前>/sd.img 600 <dir>/c.log
bash tools/x64/ktest64.sh <dir>/wine.img 600 <dir>/d.log
```

`--free-mb`は試験のイメージのシステムボリュームより大きくする。

#### まだ無いもの

- TessronOSの区画の大きさを選ぶこと。共存では未割り当ての領域の全体を使う。
- 論理セクタが4096バイトのディスク(GPTの書き方が512バイトのセクタだけを扱う)。
- 登録したTessronOSを内蔵のディスクから取り除くこと。
- 内蔵のディスクのTSLOGの区画(Windowsから記録を取り出すには、Windowsで`\EFI\TESSRON\`を見る)。
- Secure Bootでの起動(ローダに署名が無い)。


### 3.10.11 スリープ

スリープは`kernel/sysdepend/x64/pm.c`が1つのタスクで最初から最後まで行う。タスクは凍結しない。ドライバが休止の中で要求を止めるロックを持ち、デバイスに頼むタスクは復帰まで待つ。同じタスクが休止と復帰を呼ぶので、休止で取ったミューテックスを復帰で放せる。

方法は実行時に決める。FADTの`LOW_POWER_S0`が立ち、`PNP0D80`のデバイスがあれば低電力S0アイドル(S0ix)を使う。そうでなく`\_S3`があればS3を使う。起動の設定`sleep=off`、`sleep=s3`、`sleep=s0`で変えられる。Surface Go 2(BIOS 1.0.31)のACPIの表は、FADTのフラグが`0020c4b5`で`LOW_POWER_S0`が立ち、LPIT(パッケージのC10の`MSR 0x632`とPMCの0xFE00013C)と`\_SB.PEPD`(INT33A1、`_DSM`のUUIDは`c4eb40a0-6cd2-11e2-bcfd-0800200c9a66`)があるのでS0ixである。DSDTには`\_S3`(SLP_TYP 5)もあるが、ファームウェアのS3の復帰の道が使えるかは確かめていない。ThinkPad X1 Yoga 第3世代はACPIの表のコピーがまだ手元になく、起動時の`pm: the machine sleeps by`の行で分かる。

順序は次のとおりである。

1. 記録を書き、ファイルシステムを`fs_sync`で書き出し、`\_TTS`を実行する。
2. 登録されたドライバ(`ts_pm_register`、`include/ts/pm.h`)の休止を順に呼ぶ。カメラ、音、ネットワーク、GPU、画面、入力、USB、ディスクの順である。失敗があれば眠らずに復帰する。
3. 全PCIファンクションの構成(ヘッダ、MSI、MSI-Xの表、Expressの制御)を保存する。S0ixでは、ストレージ、ネットワーク、マルチメディア、USBのエンドポイントをD3hotにし、`_DSM`の制約の一覧を記録に出す。表示のファンクションはD0のままで、パネルのバックライトだけ消す。eDPのリンクをカーネルが張り直せないので、パイプは動いたままであり、パッケージの深いC状態には入らないことがある。
4. 監視とタイマを止め、他のプロセッサをIPI(`SGI_CALL`)の中で止める。割込みが許可されていた所なのでスピンロックを持っていない。
5. S3: `\_PTS(3)`、FACSの起こすベクタ(トランポリンのページ)、SLP_EN。起きるとブートプロセッサがトランポリン(`tramp.S`)から入り、GDT、IDT、TSS、MSR、Local APIC、IOAPIC、CR3、FPUのレジスタを戻して、止めた場所(`pm_ctx.S`)から続ける。他のプロセッサはINITとSIPIで同じ道を通る。各プロセッサはトランポリンから、この用に取った別のスタックで入る。一時スタックは、アイドルの間に割り込まれたフレームが残っているので使わない。TSCは止めたときの値から続くように足し直す(`IA32_TSC_ADJUST`があればそれで)。
6. S0ix: `_DSM`の3(画面オフ)と5(入る)を呼び、CPUID 5の最も深いMWAITで割込みを待つ。割込みが来ると400msの間システムを動かし、ファームウェアの通知がタスクまで届くのを待つ。起きる理由(電源ボタン、蓋が開いた、RTCのアラーム)が無ければ戻って待つ。止まっていた時間は単調時計から除く。
7. 単調時計は眠っている間進まない(各タイマの残り時間は変わらない)。システムの時刻にはRTCで測った時間を足す。
8. ファンクションをD0にして構成を戻し、ドライバの復帰を逆順に呼び、`\_TTS(0)`を実行する。

デバイスごとの休止と復帰は次のとおりである。

| デバイス | 休止 | 復帰 |
| --- | --- | --- |
| NVMe | シャットダウンの通知まで行い、3つのロックを持ち続ける | リセットの後と同じ手順でキューとHMBを作り直す |
| xHCI | Run/Stopを止め、CSSで状態を保存 | レジスタを戻してCRS。SREが立たなければ、リンクがU3のルートポートを起こし(USB 2は20ミリ秒のResumeの後にU0、USB 3はU0、それから10ミリ秒待つ。U3でないポートには何もしない)、ドアベルを鳴らして続ける。Surface Go 2のS0ixでは、起きた後のルートポートがU3のままで、起こさないと転送がすべてTransaction Errorになり、ハブごと切れていた。立てば(状態が失われた)リセットから始め、機器を見つけ直す。システムのディスクがUSBなら、差し直したディスクと同じ扱い(11.26)で戻る |
| AHCI | キャッシュを書き出してロックを持つ | ポートを始め直す |
| HD Audio | ストリームを止める | コントローラをリセットから始め、コーデックに電源を入れ、動いていたストリームを始め直す |
| e1k、virtio-net | 送信のロックを持って止める | リセットから始める |
| Wi-Fi | 接続を離れインタフェースを下げる | インタフェースを上げ(ファームウェアを読み直す)、保存したネットワークに入り直す |
| GPU(`igt`) | ジョブの入口のロックを持ち、動いているジョブを待つ | S0ixは何もしない。S3はGGTTを比べて書き戻し、フォースウェイク、ワークアラウンド、エンジン、割込み、RC6とRPSを設定し直す |
| 画面 | バックライトを消す | 点ける。QEMUの標準VGAはS3の後にモードを設定し直す |
| カメラ | ストリームをソースで止める | 始め直す |
| I2C | ロックを持つ | S3の後に`_PS0`、LPSSのリセット、速さの設定。HID over I2Cの機器自体の再設定は入っていない |
| PS/2 | なし | S3の後に構成とスキャンを設定し直す |
| シリアル | なし | S3の後に形式、速さ、受信の割込みを設定し直す |

Intelの表示はS3の後にファームウェアが設定し直さないので、eDPを点け直す処理(6.9)が入るまで、S3で眠ると画面は戻らない。

電源ボタンと蓋の動作はシステム環境設定の〈電源管理〉で選ぶ(機器の設定の`BUTTON_SLEEP`、`LID_SLEEP`)。既定では、電源ボタンは電源を切り、蓋のある機械は蓋を閉じるとスリープする。起動の設定`pwrbtn=sleep`、`pwrbtn=off`、`lid=sleep`、`lid=ignore`でも変えられる。起こした押下が通知で届いても、起きて2秒の間はもう一度眠らない。

QEMUのq35では、起動の設定`sleep=s0`で低電力S0アイドルに2回入り、`system_powerdown`(固定の電源ボタン)で起きて、NVMe、xHCI(状態が失われる側の道)、e1000e、HD Audioの復帰まで通ることを確かめた。QEMUは動いている間はRTCのアラームでSCIを出さないので、この道はRTCでは起きない。S3は、RTCのアラームで2回続けて眠って起き、全プロセッサとデバイスの復帰を経てデスクトップが動き続けることを確かめた。実機ではどちらも確かめていない。

確かめるために、起動の設定`sleeptest=N`(`sleeptest_after=秒`、`sleeptest_for=秒`)で、起動後に自動でN回眠り、RTCのアラームで起きる。各段階は`pm:`の行で`KERNEL.TXT`に残る。眠る前に記録を書くので、起きなかったときも、そこまでの行が残る。起きた行にはLPITの2つのカウンタの増分が出るので、低電力の状態に実際に入ったかが分かる。


### 3.10.12 ThinkPad X1 Carbon(型番21CBCTO1WW、Alder Lake-P)

12th Gen Intel Core i7-1280Pの機種で、機種の見分けはSMBIOSの製品名の先頭`21CB`(`BOARD_X1C_21CB`)である。名前を出すだけで、機種で道を分けてはいない。2026-11-01の時点で実機は手元になく、ここに書いたことはコードとQEMUで確かめた範囲である。下の表の「機種の構成」の値はどれも記憶によるもので、実機のHW.TXTで確かめるまで確定しない。

| 項目 | 見込み(未確認) | コードの扱い |
| --- | --- | --- |
| CPU | Pコア6、Eコア8、20スレッド | MADTのとおりに起こす |
| GPU | Iris Xe(8086:46A6) | GOPの画面。`igt12:`のログだけ |
| Wi-Fi | AX211(CNVi、8086:51F0) | iwx。無線部で選ぶファームウェアは全部載せた |
| 音 | ALC287系、デジタルマイク | HDAの道。下の「音」 |
| ストレージ | NVMe | クラスで見つける |
| 入力 | PS/2のキーボード、I2CのHIDのタッチパッド、TrackPoint | 下の「入力」 |
| USB | PCHのxHCIとThunderbolt側のxHCI | クラスで見つける。USB4のホスト(NHI)は扱わない |

#### コードとQEMUで確かめて直したこと

- PCIの表: `pcie.c`の表は32ファンクションで、あふれた分は黙って捨てていた。走査はデバイス番号の順なので、捨てられるのは後ろの00:1Cから00:1F(NVMeの下のルートポート、LPC、HDA、SMBus)である。この世代のチップセットはファンクションの数が32に近いか超えるので、起動ディスクも音も見えなくなりうる。表はあふれると倍の大きさに取り直す形にし、取れないときはその数をコンソールに出す。`pcdev`の群の`test_pci_all`が、バス0を直に読んだ数と表の数を比べる。
- TSCの速さ: CPUIDの15hが完全に答えれば測らない。答えないときのHPET、PMタイマ、8254の測定は、カウンタが動かないと永久に回っていた(画面の見張りは後の段階で始まるので何も出ない)。約1秒(TSCで2^32)であきらめて次を試し、全部だめならCPUIDの16hの基本クロックを使う。どれを使ったかは`TessronOS x64: TSC`の行に出る。
- XSAVE: 各プロセッサが`knl_xcr0`と`knl_xsave_size`を上書きしていた。タスクの保存領域は最初の値で作るので、後のプロセッサが違う値を答えると保存があふれる。今は起動プロセッサが決め、他は同じ成分が同じ大きさに入ることを確かめる。入らないプロセッサは理由を出して止まり、オンラインにならない。カーネルが許す成分はx87、SSE、AVXだけなので、AVX-512の有無の差は影響しない。
- MADT: IDが0xFF(x2APICの項目では0xFFFFFFFF)の項目はプロセッサでないものとして飛ばす。同じプロセッサのLocal APICとx2APICの2項目は1つに数え、起動プロセッサを先頭に置くことは前からそうなっている。
- プロセッサごとの記録: CPUIDは実行したプロセッサの値を返すので、HW.TXT(起動プロセッサのみ)ではPとEの差が分からない。`cpu: PRC`の行にコアの種類、シグネチャ、リーフ7、アドレスのビット数、XSAVEの大きさを1プロセッサ1行で出し、`a hybrid processor:`の行に種類ごとの数を出す。
- 起動中のプロセッサの記録: 起こす前に、段階の帯を`the kernel: starting CPU of APIC 番号`にする。三重フォールトで機械がリセットしても分かるよう、番号を印のページ(`T_BIMARK.starting`)にも書き、次のローダが`while starting the processor of APIC 番号`と出す。

#### 変えずに足りると確かめたこと

- CPUの数とAPIC IDの飛び: `CNF_MAX_PRCID`は1024、`ACPI_MAX_CPU`は64で、IDは順番に使わず表から引く。下のQEMUの試験で飛びのある構成を動かした。
- x2APIC: CPUIDにあれば常にx2APICで使い、ファームウェアがすでに有効にしていても同じ道を通る。装置の割込みの宛先は8ビットのIDなので、0xFEを超えるプロセッサには装置の割込みを送らない。
- MWAIT: 待機は`hlt`で、MWAITはスリープの中でCPUIDのリーフ5を確かめて使うだけである。パッケージの深い省電力に入らないので電池の持ちはよくないはずで、これは未測定である。
- IOAPIC: 入力が32を超える分(PCHは多い)は使わないベクタを指して隠したままにする。入力が32以上の割込みを`_CRS`で渡すHIDは、一定の間隔で読む道に落ちる。
- PCIの決め打ち: xHCI、NVMe、HDA、I2CはどれもクラスとACPIで見つけ、IDの表で止まる所はない。IDを見るのはSunrise Pointの役割の切替え(`is_spt`)と第9世代のGPUの表だけで、どちらもこの機種を除く。`knl_igfx_probe`と`gfxdump.c`も第9世代の表の外ではレジスタを読まない。Thunderboltのルートポートの下は、ファームウェアの付けたバス番号のまま走査するだけで、何も割り当てない。
- 画面: GOPのまま(6.9)で、解像度は変えられない。`igt_gen.c`の表で46A6はAlder Lake-P(graphics 12.0、`IGT_READ_SAME`)なので、`igt12.c`は起動の30秒後に読むだけのログを出す。BAR0が4GBより上でもデバイスの窓に入る。レジスタの場所はTiger Lakeと同じという前提で、実機で値を見て確かめる(13.11.16)。

#### 入力

キーボードとTrackPointはi8042の道である。`AcpiGetDevices`は`_HID`と`_CID`の両方を見るので、ThinkPad独自の`_HID`でも`_CID`が`PNP0303`、`PNP0F13`なら見つかる。キーボードとポインタのどちらのIDもないときは、FADTが否定しない限りコントローラを直接試す。

タッチパッドは`hid_i2c.c`の道である。`dw_i2c.c`にPCIのIDの表はなく、クラス0C.80か11.80とACPIの`_ADR`、LPSSのケーパビリティで見分けるので、Alder LakeのIDを足す必要はない。クロックの値は`SSCN`、`FMCN`、なければファームウェアの残した値を使う。実機で未確認である。

タッチパッドの割込みが`GpioInt`のとき、`gpio_pch.c`がAlder Lake-Pのピン制御(`_HID`は`INTC1055`、パッドの配置はTiger Lake-LPと同じ)で受ける。起動の直後は今までどおり8ミリ秒ごとに読む道で動き、パッドの割込みも有効にしておく。割込みと一緒に読めたレポートが8回になったら割込みの道に替える(`hidi2c: ...: GPIO interrupt confirmed`)。割込みが一度も来ないままレポートが64回読めたとき、読むものがない割込みが200回来たとき、替えた後で割込みが来ないのにレポートが読めることが3回続いたときは、パッドを止めて読む道に戻す(`GPIO interrupt given up`)。パッドが取れないとき(ACPIが持つ、MEが持つ、ネイティブの機能、違う設定でロック済み)は最初から読む道である。実機では未確認で、表はOpenBSDの`pchgpio.c`とLinuxの`pinctrl-tigerlake.c`の2つで同じことを確かめ、レジスタの計算は`tools/x64/tests/gpio_pch_host.c`で確かめた。

#### 音

HDAの道で動かす。DSPが有効(クラス04.01)でも、00:1F.3のHDAのレジスタをそのまま使う(`ctrl_find`)。スピーカが出るかどうかは次の3点にかかり、3つともログを見るまで分からない。

1. スピーカのアンプがSoundWireかI2Sの先ならDSPが要るので鳴らない。`hda: built-in sound`の行のnoteとNHLTの行で分かる。SOF本体は作らない。
2. ピンの既定の構成にある固定のスピーカはDACまでたどるが、`hda_quirks()`は空である。低音用のスピーカの接続の直し、Realtekの係数、GPIOが要る機種では、音が小さいか出ない。`TessronOS: hda:`の行とコーデックのIDを見て、要るものだけ足す。
3. I2Cの先の外付けのアンプがあると、その初期化も要る。`ACPI/*.BIN`のDSDTとSSDTで、アンプの`_HID`があるかを確かめる。

デジタルマイクはDSPの先なので録音できない見込みである(`SND_NOTE_MIC_DSP`)。

#### Wi-Fi

OpenBSD 8.0の`if_iwx.c`では、8086:51F0は`WL_22500_11`で、既定は`iwx-so-a0-gf-a0-77`とそのPNVMである。カードの報告するMACと無線部で`iwx_dev_info_table`から選び直すので、同じIDでも`so-a0-gf4-a0`、`so-a0-hr-b0`、`so-a0-jf-b0`を求めることがある。この3つ(gf4はPNVMも)を`fw.sh`と`mkobj.py`に足した(9.6.5)。バイト列はOpenBSDの`iwx-firmware-20251021p0`のものとSHA-256で一致する。BZ世代(8086:7740)は別のAPIで、対象の外である。

OpenBSDの表は51F0を`sc_integrated = 0`とし、51F1は1とする。CNViなので、カードが起動しないときはまずこの違いを疑う。層はOpenBSDのソースのまま使っている。

#### 内蔵のストレージ

ファームウェアの設定でIntel VMDが有効だと、NVMeはPCIのバスに直接見えず、VMDのファンクションの下に隠れる。VMDのドライバは無いので内蔵のNVMeは見えない。USBからの起動と記録には影響しない。HW.TXTのPCIの一覧で確かめる。

#### 試験

`tools/x64/ktest64.sh`の`SMP`と`EXTRA`で次を走らせた。結果は下のとおりである。

| 構成 | 群 | 結果 |
| --- | --- | --- |
| `SMP="20,sockets=1,cores=10,threads=2"`、x2APIC、ルートポートを32個足す(バス0に40ファンクション) | `pcdev` | 7件成功(4件はその装置が無く飛ばす)。`test_pci_all`が40と40で一致 |
| 同じCPUの数、`-cpu max,-x2apic`(xAPIC) | `pcdev` | 同じ。20 of 20 CPUs online |
| `SMP="18,sockets=1,cores=6,threads=3"`(APIC IDに飛びがあり、最後のPRC 18がAPIC 22)、`-machine hpet=off`(PMタイマで測る) | `smp` | `test_del_ended`までの10件が成功 |
| 既定(4 CPU) | `obsdwifi` | 2件成功。`test_firmware`が足した4つを含む名前を読む |

表の18 CPUの構成は、`test_exd_stack`が試験の誤りでページフォールトになっていた時点の結果で、`test_int_target`から後ろは走っていない。その試験は、スタックの先頭の前にあるガードページを読まない形に直した(4.3)。

QEMUにはPとEの混在が無く、MADTのx2APICの項目も255未満のIDでは作られないので、ハイブリッドの差とその項目の読みは実機で初めて通る。

#### 起動の手順

[x64の実機で試す手順](../x64-hw-test.md)の10章に書いた。Secure Bootは無効にする前提である(ローダに署名が無い)。
