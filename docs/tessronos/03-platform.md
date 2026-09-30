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
