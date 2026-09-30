# 第2章　構成

## 2.1 レイヤ構成

TessronOSはBTRONのレイヤ構成を採用している。下から、モニタ層、中心核、周辺核、外核、アプリケーションの5つである。レイヤの名前はそのままソースツリーの最上位のディレクトリ名になっており、どのファイルがどのレイヤに属するかはディレクトリで分かる。

```text
 アプリケーション   デスクトップ(カーネル内) / プロセスの小物とアプリ       application/  lib/
 ─────────────── SVCゲートウェイ(機能コード表) ───────────────
 外核              描画 dp_  ウインドウ wm_  フォント fn_  仮身 om_  xmlTAD tad_  outer_kernel/
                   画像 img_  ファイル変換 xf_
 周辺核            システム管理  プロセス  ファイル fs_  TSFS  カレンダ dt_  peripheral_kernel/
                   ネットワーク so_  実身 ob_
 中心核            μT-Kernel 3.0(64bit・SMP化)                          kernel/tkernel/
 モニタ層          機種依存部  デバイスドライバ  T-Monitor互換          kernel/sysdepend/  device/  lib/libtm/
 ハードウェア      Raspberry Pi 5 / QEMU virt
```

呼び出しは上位から下位への一方向である。下位のレイヤが上位のレイヤを直接呼ぶことはない。ただし、上位のレイヤが登録した関数を下位のレイヤがコールバックとして呼ぶことはある。たとえば実身マネージャは、ウインドウの実身を扱う関数をウインドウマネージャから登録してもらい、ウインドウに対する実身の操作が来たときにそれを呼ぶ。

## 2.2 保護レベルと例外レベル

| 保護レベル | EL | 空間 | 主な実行主体 | 下位レイヤの呼び出し方 |
| --- | --- | --- | --- | --- |
| 0 | EL1 | 共有空間 | カーネル、ドライバ、システムのタスク | 関数呼出し |
| 1 | EL1 | 共有空間 | カーネル内で動くアプリケーション(デスクトップなど) | 関数呼出し |
| 2 | EL0 | 共有空間 | プロセスに属さないユーザレベルのタスク | SVC |
| 3 | EL0 | 固有空間 | プロセスのタスク | SVC |

AArch64にはEL1が1つしかないので、保護レベル0と1は同じ権限で動く。0と1の区別は用途の区別として残している。保護レベル2と3の違いは固有空間を持つかどうかだけで、3のタスクは必ずどれかのプロセスに属する。

EL0からカーネルを呼ぶ手段はSVC命令だけである。SVCの即値に機能コードを置き、カーネルは機能コードからクラスと番号を取り出してテーブルを参照する(第5章)。

## 2.3 主なコンポーネント

| レイヤ | コンポーネント | 役割 | ソース |
| --- | --- | --- | --- |
| 中心核 | μT-Kernel 3.0 | タスク、同期と通信、メモリプール、時間管理、割込み、スケジューラ、プロセッサ間の排他 | `kernel/tkernel/` |
| 周辺核 | システム管理 | 物理ページの割当て、アドレス空間とMMU、共有メモリ、パーティションテーブル、PCIe、乱数、SVCゲートウェイ | `peripheral_kernel/sysman/` |
| 周辺核 | プロセス管理 | プロセスの生成と終了、ELFのロード、プロセス間メッセージ、終了時のリソースの回収 | `peripheral_kernel/sysman/proc.c`、`elf.c` |
| 周辺核 | ファイル管理 | T2EX互換の`fs_`とFAT(FAT12/16/32)、実身を格納するTSFS | `peripheral_kernel/fs/` |
| 周辺核 | 実身マネージャ | 実身の基本操作`ob_`。名前の解決、実身キー、保護、ユーザと資格情報、種類ごとのマネージャ | `peripheral_kernel/obj/` |
| 周辺核 | ネットワーク | NetBSDのTCP/IP(rumpカーネル)とソケット`so_`、DHCP、名前解決、設定の反映、SNTP。`NETSTACK=lwip`ではlwIP | `peripheral_kernel/network/` |
| 周辺核 | カレンダ | 時刻と暦`dt_` | `peripheral_kernel/sysman/datetime.c` |
| 外核 | ウインドウマネージャ | 描画`dp_`、フォント`fn_`(FreeType)、ウインドウ、部品、パネル、メニュー、データボックス、外観設定テーブル、ポインタ、メッセージ行 | `outer_kernel/dp/`、`font/`、`wm/` |
| 外核 | 実身/仮身マネージャ | 仮身の描画と登録テーブル`om_`、xmlTADの解析・編集・表示`tad_` | `outer_kernel/om/`、`tad/` |
| 外核 | 画像、ファイル変換 | PNG、JPEG、BMP、GIF、ICOのデコード`img_`、ファイルと実身の相互変換`xf_` | `outer_kernel/img/`、`xf/` |
| モニタ層 | 機種依存部 | 起動、例外ベクタ、ディスパッチャ、MMUとキャッシュの操作、GIC、Generic Timer、PSCI | `kernel/sysdepend/` |
| モニタ層 | ドライバ | シリアル、SD、GPIO、virtio、Ethernet、画面、USB、サウンド | `device/` |
| アプリ | デスクトップ | 操作環境、仮身一覧、基本文章編集、基本図形編集、かな漢字変換の入力処理(カーネル内で動く) | `application/desktop/`ほか |
| アプリ | プロセスのプログラム | 小物、書庫解凍、バックアップ、ファイル変換など | `application/` |
| ライブラリ | ユーザ側 | SVCのスタブ`libts`、JSON、書庫、文字コード、C++ランタイム、スレッドなど | `lib/` |

## 2.4 名前の接頭辞

関数の接頭辞は、その関数が属するレイヤと仕様の出典を表す。

| 接頭辞 | 出典 | 例 |
| --- | --- | --- |
| `tk_` | T-Kernel標準とSMP T-Kernelの追加 | `tk_cre_tsk`、`tk_get_prc` |
| `ts_` | TessronOSで追加したもの | `ts_cre_prc`、`ts_map_mem` |
| `fs_` `dt_` `pm_` `so_` | T2EX互換 | `fs_open`、`dt_localtime`、`so_socket` |
| `ob_` | 実身の基本操作 | `ob_opn_obj`、`ob_rea_rec` |
| `dp_` `wm_` `fn_` `om_` `tad_` `mn_` | 外核 | `dp_fill_rect`、`wm_open`、`mn_pop_men` |
| `tm_` | T-Monitor互換 | `tm_printf` |
| `knl_` | カーネルの内部 | `knl_dispatch` |

## 2.5 ソースツリー

```text
tessronos/
├── config/              上限値と機能の切替え(config.h)
├── include/             tk/ sys/ tm/(上流由来) ts/(TessronOS) tsposix/ tscxx/(POSIXとC++の互換)
├── kernel/              中心核とモニタ層の機種依存部
│   ├── tkernel/         μT-Kernel 3.0の本体(64bit・SMP化)
│   └── sysdepend/       cpu/core/armv8a/(AArch64共通) rpi5/ qemu_virt/(マシンごと)
├── peripheral_kernel/   sysman/ fs/ obj/ network/ libc/
│   └── network/         so_prc.c so_sntp.c so_conf.c(両方のスタックで共通) netbsd/(NetBSDのスタックの受け、
│                        rumpcomp/はrumpカーネルの中に入る部品) lwip/ port/ so_api.c(NETSTACK=lwip)
├── outer_kernel/        dp/ wm/ font/ om/ tad/ img/ xf/
├── device/              disp/ gpio/ net/ sd/ ser/ snd/ usb/ vblk/ vnet/
├── application/         desktop/ cab/ doc/ fig/ kconv/ と各プログラム
├── lib/                 libts/ libjson/ libconf/ libui/ libbpk/ libbtbk/ libtxc/ libxfu/ libftp/
│                        libcxxrt/ libpthread/ libtk/ libtm/
├── etc/                 def/(定義実身) xtad/(サンプルの実身) font/ wall/(フォントと壁紙) linker/ boot/(config.txt)
├── tests/               ktest/(カーネル内の試験) uprog/(試験用のプロセス)
├── tools/               イメージの作成、TSFSのツール、試験の実行、netbsd/(NetBSDのスタックのビルド)
└── build_make/          makefile とマシンごとの定義
```

## 2.6 構成の切替え

構成は`config/config.h`の定数とmakeの変数で決める。主なmakeの変数を次に示す。

| 変数 | 意味 |
| --- | --- |
| `TARGET` | `_RPI5_`か`_QEMU_VIRT_`。必須 |
| `DESKTOP=1` | デスクトップが起動する構成でビルドする |
| `KTEST=1` | カーネル内の試験を組み込む。`KTONLY`、`KTGROUP`で実行する試験を絞り込む |
| `MOZC=1` | かな漢字変換にMozcの変換エンジンをリンクする。指定しなければローマ字からかなへの変換だけになる |
| `NETSTACK` | TCP/IPスタック。`netbsd`(デフォルト)はNetBSDのスタック、`lwip`は以前のlwIP(第9章) |
| `BUILD_ID` | 生成物と中間ファイルの出力先を分ける名前。`KTONLY`などを変えるときは必ず別の名前にする |
| `BROWSER=1`、`BROWSER_BLINK=1` | 別のリポジトリ`tessron-chromium`からブラウザを取り込む(第14章) |
