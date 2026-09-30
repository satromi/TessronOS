# 変更履歴

TessronOSの版ごとの変更を、新しいものから順に記載しています。

バージョン表記はRn.nnnの形で表します。Raspberry Pi 5とQEMU用のイメージは、GitHubのリリースで配布しています。

## R1.000 (2026-09-30)

最初の公開版です。

### 動作環境

- Raspberry Pi 5(Broadcom BCM2712、Cortex-A76 ×4)
- QEMUの`virt`マシン(`cortex-a76`、4コア)

ビルド、Raspberry Pi Imagerでの書き込み、WindowsのQEMUでの試し方は[docs/build-and-run.md](docs/build-and-run.md)に、設計は[docs/tessronos.md](docs/tessronos.md)にあります。

### 主な機能

- カーネル: μT-Kernel 3.0をAArch64の64bitに移し、4つのCPUのSMPで動かしています。カーネル内の排他はスピンロックで行います。
- プロセス: EL0で動くプロセスが固有のアドレス空間を持ち、ELF形式のプログラムをロードして実行します。システムコールはT2EXに合わせました。Shift+Pauseで最前面のプロセスを強制終了でき、プロセッサ例外で終了したプロセスはダイアログで知らせます。
- 実身: ファイル、メモリ、デバイス(ディスク、時計、入力、画面、GPIO、USB、乱数)、プロセス、通信路、ソケット、ウインドウ、部品、書体を、UUIDで識別する実身として同じ操作で扱えます。
- ファイルシステム: 実身をそのまま格納するTSFSと、FAT12/16/32。TADjs Desktop形式のファイル群と相互に変換できます。
- デバイス: SDカード、USB(xHCI、ハブ、キーボード、マウス、USBメモリ、USBオーディオ)、Ethernet(Raspberry Pi 5のRP1、QEMUのvirtio)、サウンド、GPIO、RTC。
- ネットワーク: NetBSD 10.1のTCP/IPスタックをrumpカーネルとして組み込んだ。DHCP、DNS、SNTP、TLS(Mbed TLS)を使えます。`make NETSTACK=lwip`でlwIPに切り替えられます。
- 画面: ウインドウシステムと、BTRONの操作に合わせたデスクトップ環境、HMI環境です。仮身一覧、基本文章編集、基本図形編集、かな漢字変換(Mozc)などのアプリがあります。
- 小物: 時計、コンソール、シリアル通信、書庫解凍、バックアップ、ファイル変換、ネットワーク設定、システム環境設定、ユーザ環境設定、マイクロスクリプトなどあります。。
- ブラウザ: Chromium 153のBlinkとV8を移植したブラウザで、HTTPSのWebサイトを表示できます。ソースは別のリポジトリ[tessron-chromium](https://github.com/satromi/tessron-chromium)にあり、`make BROWSER=1`でイメージに取り込みます。ページ全体のスクロールはウインドウのスクロールバーで行う。原紙「ブラウザ」から作ったページの実身は、最初に`https://www.tron.org`を開きます。

### 既知の制限

- GPU(V3D 7.1)のドライバは試験中です。Raspberry Pi 5の実機では、描画のジョブの多くが終わりません。
- IPv6はリンクローカルアドレスだけで、ルータ広告による自動設定はしません。名前解決はIPv4だけです。
- NetBSDのスタックのタイマが1秒に100回刻むので、NetBSDの構成ではカーネルのティックレス動作が働きません。
- ブラウザは、移植していないWebのインタフェース(`WebSocket`、`indexedDB`、`AudioContext`など)を使うページを正しく表示できないことがあります。JITとWebAssemblyは使えません。
- USBのマウスとキーボードは、機種によって動作を確かめていないものがあります。起動時にはシリアルに、入力機器の報告の読み方を表示します。

### ライセンス

TessronOSのソースコードはT-License 2.2で配布する。μT-Kernel 3.0の派生物としてトロンフォーラムのトレーサビリティサービスに登録してあり、ディストリビューション番号は0007006Aです。

第三者のソフトウェアとリソース、イメージを配布するときに添付するライセンスの表示は[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)にまとめています。
