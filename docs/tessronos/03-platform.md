# 第3章　動作環境

## 3.1 対象のマシン

TessronOSが動くのは、Raspberry Pi 5とQEMUの`virt`マシンの2つである。どちらもAArch64のCPUを4つ持ち、同じ`kernel/sysdepend/cpu/core/armv8a/`のコードで動く。マシンごとに異なるのはアドレスと割込み番号のテーブル、起動手順、一部のドライバだけで、`kernel/sysdepend/rpi5/`と`kernel/sysdepend/qemu_virt/`に分けてある。

| 項目 | Raspberry Pi 5 | QEMU virt |
| --- | --- | --- |
| CPU | Cortex-A76 ×4(BCM2712) | `-cpu cortex-a76 -smp 4` |
| RAM | 物理0から。1GB直下にファームウェアの予約がある | `0x40000000`から連続 |
| カーネルの配置 | `0x200000`(フラットな`tessronos.img`) | `0x40200000`(ELFを`-kernel`で渡す) |
| 起動時のEL | EL2 | EL1(`virtualization=on`でEL2) |
| 割込み | GIC-400(GICv2) | GICv2(`gic-version=2`) |
| コンソール | デバッグ用UART(PL011) | PL011 |
| ディスク | SDカード、USBメモリ | virtio-blk、SDHCI、USBメモリ |
| ネットワーク | RP1のGigabit Ethernet | virtio-net |
| 画面 | ファームウェアが用意するフレームバッファ | `bochs-display` |
| USB | RP1のxHCI | `qemu-xhci` |
| 時計 | ファームウェアに問い合わせる(電池がなければ1970年から) | PL031 |
| 乱数 | ハードウェア乱数生成器とソフトウェアの疑似乱数生成器 | ソフトウェアの疑似乱数生成器 |

Raspberry Pi 5のUSB、Ethernet、GPIO、サウンドは、PCIeの先のRP1というチップにある。TessronOSはPCIeのルートコンプレックスを自前で初期化し、RP1のアドレスウインドウを通してこれらを扱う。QEMUにはRP1がないので、同じ役割をvirtioデバイス、`qemu-xhci`、`bochs-display`が担う。

## 3.2 起動

Raspberry Pi 5では、EEPROMのブートローダがSDカードの第1パーティションから`config.txt`、デバイスツリー、`tessronos.img`を読み込み、CPU0をEL2でイメージの先頭から実行する。`config.txt`は次のとおりである。

```text
kernel=tessronos.img
kernel_address=0x200000
device_tree=bcm2712-rpi-5-b.dtb
enable_uart=1
uart_2ndstage=1
pciex4_reset=0
framebuffer_depth=32
framebuffer_ignore_alpha=1
```

ファームウェアは、`kernel_address`の指定があっても、ヘッダのないイメージを`0x80000`に置くことがある。そのためカーネルのエントリは、実際にロードされたアドレスがリンクアドレスと異なれば、イメージを`0x200000`へコピーしてから処理を続ける。その後EL2からEL1へ移り、MMUを有効にして`main()`へ進む。2つめ以降のCPUはPSCIの`CPU_ON`で起動する。

起動の後半では、ドライバ、描画、ウインドウ、USB、ネットワーク、乱数、プロセス管理、実身マネージャの順に初期化し、最後に初期プログラム(`CNF_INIT_PATH`)があればそれを実行する。デスクトップ構成では、続いてデスクトップが起動する。

## 3.3 SDカードの構成

SDカードのイメージは`make TARGET=_RPI5_ BUILD_ID=<名前> sdimg`で作る。パーティションテーブルはGPTで、パーティションは次のとおりである。

| パーティション | 形式 | 内容 |
| --- | --- | --- |
| 1 | FAT32、256MB | `config.txt`、デバイスツリー、`tessronos.img`と、`/boot`に置くファイル |
| 2 | TSFS、64MB | システムボリューム。フォント、壁紙、定義実身、小物、辞書を実身として格納する |
| 3 | 空、16MB | カーネル内の試験の構成にだけある作業用パーティション |

デバイスツリーはファームウェアの配布物から取得して使う。再配布はしないので、リポジトリには含めていない。

## 3.4 動作確認の範囲

| 項目 | QEMU | Raspberry Pi 5 |
| --- | --- | --- |
| カーネル内の試験 | すべて(449件、前提条件を満たさない10件を除く) | 391件中390件(2026年9月28日) |
| 4つのCPU、SDカード | 確認済み | 確認済み |
| USBのキーボードとマウス、USBメモリ | 確認済み | マウスまで確認済み |
| Ethernet、DHCP、DNS | 確認済み | 確認済み |
| SNTPによる時計合わせ | 確認済み | 未確認 |
| デスクトップ | 確認済み | イメージは作成できるが、実機での確認はこれから |
| サウンド | USBオーディオで確認済み | 未確認 |

## 3.5 Raspberry Pi 4(計画)

Raspberry Pi 4(BCM2711)への対応は計画段階であり、この節に書くことはまだ実装していない。今の実装のうちRaspberry Pi 5に依存している部分と、Raspberry Pi 4との差異、移植の方針をまとめる。アドレスと割込み番号は、ファームウェアの配布物の`bcm2711-rpi-4-b.dtb`とBCM2711のペリフェラルの資料で確かめたものである。

### 3.5.1 差異

| 項目 | Raspberry Pi 5(今の実装) | Raspberry Pi 4 | 方針 |
| --- | --- | --- | --- |
| CPU | Cortex-A76 ×4。ARMv8.2-Aで、LSEのアトミック命令と暗号拡張がある | Cortex-A72 ×4。ARMv8.0-Aで、LSEのアトミック命令も暗号拡張もない(CRC32はある) | 命令セットの基準をARMv8.0-A(`-march=armv8-a+crc`)に下げる。カーネル、NetBSDのスタック、mozc、Blinkのすべてが対象 |
| 起動 | EEPROMのブートローダがカーネルを読み込む | EEPROMのブートローダがSDカードの`start4.elf`と`fixup4.dat`を読み込み、それがカーネルを読み込む | SDカードのFATパーティションにファームウェアのファイルを置く |
| 起動時のEL | EL2 | EL2(ファームウェアに組み込まれたarmstubが入れる) | 今の`boot.S`のまま |
| 2つめ以降のCPU | PSCIの`CPU_ON`(TF-A) | スピンテーブル。CPU1〜3がそれぞれ物理`0xd8`、`0xe0`、`0xe8`、`0xf0`を見て待つ。PSCIはない | デバイスツリーの`enable-method`でPSCIとスピンテーブルを選ぶ |
| 汎用タイマ | 54MHz | 54MHz | `CNTFRQ_EL0`を読む(今のまま) |
| RAM | 物理0から。1GB直下にファームウェアの領域 | 1、2、4、8GB。物理0からGPUの領域の手前まで、`0x40000000`から`0xFC000000`まで、8GBではさらに`0x1_00000000`から`0x2_00000000`まで。`0x0`〜`0x1000`はarmstubとスピンテーブル | 範囲はデバイスツリーの`/memory`と`/memreserve/`から取る |
| 周辺機器 | `0x10_0000_0000`以降と、PCIeの先のRP1の窓 | `0xFC000000`〜`0xFF7FFFFF`、ARMのローカル周辺機器は`0xFF800000`以降(low peripheralモード) | low peripheralモードのまま使う。RAMの最後の1GBの中に周辺機器があるので、リニアマップは1GB単位で作らない |
| DMAの届く範囲 | RP1の先のバスマスタは内向きの窓を通してメモリ全体に届く | VideoCore(メールボックス、フレームバッファ)とEMMC2の初期の版(B0)は1GBまで、PCIeの先のVL805は3GBまで、GENETはメモリ全体 | DMAに使うページを1GB未満から取る。バスアドレスへの変換はデバイスツリーの`dma-ranges`から求める |
| 割込み | GIC-400 | GIC-400。ディストリビュータ`0xFF841000`、CPUインタフェース`0xFF842000` | 今のGICのドライバのまま |
| コンソール | 3ピンのデバッグUART(PL011) | 40ピンヘッダの8番と10番のPL011(UART0、`0xFE201000`、SPI 121)。既定ではBluetoothが使っているので、`dtoverlay=disable-bt`で取り返す | USBシリアル変換器でつなぐ |
| SDカード | SDHCI(SPI 273) | EMMC2(`0xFE340000`、SPI 126) | `sdhci.c`を使う。基準クロックはメールボックスで問い合わせる |
| USB | RP1のxHCI 2つ | PCIeの先のVL805(xHCI、4口)。PCIeのルートコンプレックスは`0xFD500000`、INTAはSPI 143。USB-Cの口のDWC2は使わない | ルートコンプレックスをカーネルが初期化し、VL805のファームウェアの読込みをメールボックスで依頼する。xHCIから上のUSBスタックは共通 |
| Ethernet | RP1のCadence GEMとBCM54213PE | GENET v5(`0xFD580000`、SPI 157と158)とBCM54213PE(MDIOのアドレス1、RGMII) | GENETのドライバを新しく作る。PHYの扱いは共通にする |
| 画面 | ファームウェアのフレームバッファ | 同じ(KMSのオーバーレイは読み込ませない) | `disp_rpi5.c`をそのまま使う |
| サウンド | USBオーディオとRP1のI2S | USBオーディオ。ほかにI2S(`0xFE203000`)、PWMによるアナログ出力、HDMIの音声がある | 最初はUSBオーディオだけにする |
| GPIO | 常時オンのGPIOとRP1のGPIO | BCM2711のGPIO(`0xFE200000`、58本)。ACT LEDはGPIO 42 | GPIOのドライバとGPIOの実身のバンクの表を新しく作る |
| GPU | V3D 7.1 | V3D 4.2 | 対象外(`V3D`を指定しない) |
| 時計 | ファームウェアに問い合わせる | RTCはない | SNTPで合わせる |
| 乱数 | iproc-rng200 | iproc-rng200(`0xFE104000`) | 同じドライバを使う |
| 再起動と電源断 | PSCI | PSCIはない | 再起動はPMのウォッチドッグで行う。電源断はできないので停止する |

### 3.5.2 方針

カーネルの像はRaspberry Pi 5と分け、`make TARGET=_RPI4_`で作る。周辺機器のアドレスと割込み番号は`include/sys/sysdepend/cpu/bcm2711/sysdef.h`に置き、ボードに依存するコードは`kernel/sysdepend/rpi4/`に置く。メールボックス、ファームウェアの時計、乱数、フレームバッファ、SDHCIのように両方のボードで同じものは、`RPI5`の条件から外して両方でコンパイルする。1つのカーネルの像で両方のボードを動かすことはしない。周辺機器のアドレスと、どのドライバを組み込むかがコンパイル時に決まっているためである。ただし、SDカードの像は共通にできる。ファームウェアは`config.txt`の`[pi4]`と`[pi5]`の条件でボードごとに別の設定を読むので、2つのカーネルの像とデバイスツリーを同じFATパーティションに置き、システムボリュームを共有できる。

命令セットの基準はARMv8.0-Aに下げ、Raspberry Pi 5の像も同じ基準で作る。今は`-march=armv8.2-a -mno-outline-atomics`でコンパイルしているので、アトミック操作がLSEの命令(`ldadd`、`cas`など)になり、Cortex-A72では未定義命令の例外になる。基準を揃えると、NetBSDのスタック、mozc、Blinkのオブジェクトを2つのボードで共有できる。ブラウザが`getauxval`で答えるCPUの機能も、Cortex-A76の固定の値ではなく、カーネルが`ID_AA64ISAR0_EL1`から求めた値にする。Cortex-A72にはAESとSHAの命令がないためである。

起動の前半は今の`boot.S`がそのまま使える。ファームウェアはイメージを`0x80000`か`kernel_address`の番地に置き、EL2でCPU0だけを実行する。2つめ以降のCPUは、`smp.c`がデバイスツリーの`enable-method`を見て、`spin-table`なら`cpu-release-addr`に入口の物理アドレスを書き、キャッシュを掃き出してから`sev`で起こす。スピンテーブルでは引数が渡らないので、入口で`MPIDR_EL1`からプロセッサの番号を求める。

`config.txt`は次のようにする。

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
```

`start4.elf`、`fixup4.dat`、デバイスツリー、`overlays/disable-bt.dtbo`は、Raspberry Piのファームウェアの配布物から`make sdimg`のときに取得する。これらはBroadcomのライセンスのもとでバイナリとしてだけ再配布できるものなので、リポジトリには含めない。パーティションテーブルは今と同じGPTとし、GPTから起動できる版のEEPROMのブートローダを前提とする。

QEMUの`-M raspi4b`(11.0で確認。xPackの9.2にもある)は、GIC-400、PL011、メールボックスとフレームバッファ、EMMC2、DWC2をエミュレートするが、PCIe、VL805、GENET、乱数生成器はない。CPUはEL2で始まり、2つめ以降のCPUはスピンテーブルで待つので、起動、4つのCPU、タイマ、割込み、コンソール、SDカード、画面はQEMUで確かめられる。QEMUのCortex-A72はLSEの命令を持たないので、命令セットの基準の誤りもQEMUで見つかる。一方で暗号拡張は持つと答えるので、AESやSHAの命令を使っていないことは実機でしか確かめられない。USB、Ethernet、乱数も実機でしか確かめられない。
