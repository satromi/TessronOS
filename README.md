# TessronOS

TessronOSは、μT-Kernel 3.0をベースに、Raspberry Pi 5(Broadcom BCM2712、Arm Cortex-A76×4、AArch64)の上で動く、64bit、SMP、プロセス対応のTRON系OSです。

- 64bit化: AArch64(ARMv8.2-A)、48bitの仮想アドレス空間
- SMP: 4コアで1本のレディキューを共有し、スピンロックで排他する
- スケジューリング: 優先度を実時間の層(1〜15、厳密な優先度順)と時分割の層(16〜32、同じ優先度の間を20msで回り持ち)に分け、RTOSの決定性を保ったまま一般のアプリを公平に動かす
- プロセス: T2EX相当のメモリ保護とプログラムのロード、固有のアドレス空間を持つプロセス。パイプ、擬似端末、シグナルなどのPOSIX層も用意
- 実身/仮身、xmlTAD: ファイル、デバイス、プロセス、ウインドウを、UUIDで識別する実身として同じ操作で扱う。コマンドから見える道(`/`、`/bin`、`/dev`など)も実身の木である。実身を格納するファイルシステムTSFSは、TADjs Desktop形式のファイル群と相互に変換できる
- デスクトップ: BTRONの操作に合わせたデスクトップ、文章と図形の編集、かな漢字変換、小物(コンソール、音量ミキサーなど)
- デバイス: SDカード、USB、Ethernet、サウンド(I2S、HDMI、USBの出力先を選べ、プログラムごとの音量で混ぜる)、カメラ、GPU(V3D)、HEVCの復号器
- ブラウザ: Chromium 153のBlinkとV8を移植したブラウザ(別のリポジトリ[tessron-chromium](https://github.com/satromi/tessron-chromium))。YouTubeなどの動画のサイトも再生できる

仕様書は[docs/tessronos.md](docs/tessronos.md)にあります。

ビルド、Raspberry Pi 5への書き込み、WindowsのQEMUでの試し方は[docs/build-and-run.md](docs/build-and-run.md)にまとめています。

バージョンごとの変更は[CHANGELOG.md](CHANGELOG.md)にあります。最新のバージョンはR1.300です。

Raspberry Pi 5とQEMU用のイメージは、GitHubの[リリース](https://github.com/satromi/TessronOS/releases)で配布しています。

## 参考

- μT-Kernel 3.0: <https://github.com/tron-forum/mtkernel_3> / BSP2: <https://github.com/tron-forum/mtk3_bsp2>
- T-Kernel 2.0 Extension: <https://github.com/tron-forum/t2ex>
- SMP T-Kernel仕様書: <https://www.tron.org/ja/wp-content/uploads/sites/2/2015/03/TEF021-S002-01.00.01_ja.pdf>

## ライセンス

Copyright (C) 2026 satromi。μT-Kernel 3.0に由来する部分の著作権は、各ファイルのヘッダに記載のとおり坂村健氏にあります。

μT-Kernel 3.0に由来する部分はT-License 2.2に従います。新規部分(ソースコード、ビルド用ファイル、ツール、文書、リソース)もT-License 2.2に統一します。T2EXとはシステムコールの名称と引数を合わせていますが、T2EXのソースコードは含んでいません。

同梱している第三者のソフトウェアとリソースのライセンスは[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)にまとめています。

TessronOSは、μT-Kernel 3.0のソースコードを利用し改変した派生物として、トロンフォーラムのトレーサビリティサービスに登録しています。

- ディストリビューション ucode: 0007006A

![ディストリビューションマーク(ucode 0007006A)](distmark.png)

https://trace.tron.org/tk/00001C0000000000000000000007006A?lang=ja
