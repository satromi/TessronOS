# 第14章　ブラウザ

## 14.1 概要

小物「ブラウザ」は、Chromium 153(153.0.8010.36)のレンダリングエンジン一式、つまりBlink、V8 15.3、Skiaを、TessronOSのプロセスとして動くようにしたWebブラウザである。作者がBTRON(超漢字)向けに作ったChromiumの移植の移植層と画面の構成を引き継ぎ、システムの呼出しをTessronOSのもの(POSIXのソケット、実身、POSIXスレッド)に置き換えた。

ソースコードはTessronOSとは別のリポジトリ`tessron-chromium`にあり、`make BROWSER=1 BROWSER_BLINK=1`でイメージを作るときに取り込む(14.3)。取り込まないときは、プログラムの実身のレコード1に、ブラウザが入っていないことをメッセージ行に表示するだけの小さなプログラム(`application/browser/br_stub.c`)が入る。

| 項目 | 内容 |
| --- | --- |
| ページの表示 | HTML、CSS、JavaScript(V8。JITを使わない`--jitless`で動かす)、画像(PNG、JPEG、GIF、WebP、JPEG XL、AVIF、SVG)、Webフォント、canvasの2D、WebGL 1(CPUで描く) |
| 通信 | Chromiumのネットワークスタック。HTTP/1.1とHTTP/2、TLS 1.2と1.3、Cookie、HTTPキャッシュ、フォームの`POST` |
| 動画と音声 | `<video>`と`<audio>`。FFmpegで復号し、音はサウンドデバイスに出す |
| 入力 | キーボード、ポインタ、ホイール、かな漢字変換 |
| 保存 | 開いているページのアドレスと履歴はページの実身に、Cookie、`localStorage`、HTTPキャッシュはそれぞれの実身にしまう |

実際のサイトとして、`https://www.tron.org/ja/`、`https://www.wikipedia.org/`、`https://ja.wikipedia.org/wiki/BTRON`、`https://news.ycombinator.com/`、`https://html.duckduckgo.com/`、`https://www.gnu.org/`、`https://example.com/`をQEMUの上で読み、表示を確かめた(14.10)。

## 14.2 構成

| 場所 | 内容 |
| --- | --- |
| `application/browser/br_main.cc`、`br_main_blink.cc` | ウインドウ、ツールパネル、入力、ページの実身の読み書き。Blinkの版は`br_main_blink.cc`が`br_main.cc`を読み込んで作る |
| `application/browser/br_blink.cc`、`br_bindings.cc`、`br_loader.cc` | Blinkの起動、ページの組み立てと描画、V8のバインディングの登録、部品(スタイルシート、画像、スクリプト)の読み込み |
| `application/browser/br_http.cc`、`br_tls.cc`、`br_text.cc` | Blinkを使わない版のHTTP、TLS、文字だけの描画 |
| `application/browser/br_obj.cc` | ページの実身のメタデータとレコード0 |
| `application/browser/port/net/` | ネットワークスタックの下の層(ソケット、名前解決、ルート証明書、Cookie・保存域・キャッシュの保存) |
| `application/browser/port/base/`、`blink/`、`skia/`、`v8/` | Chromiumの`base`、Blink、Skia、V8がOSに求めるもの(メモリ、スレッド、時刻、`localStorage`、フォントマネージャ、PNGのコーデック) |
| `application/browser/port/media/`、`webgl/` | 動画と音声の復号と再生、WebGLのGLES2とGLSLの解釈 |
| `application/browser/port/shim/`、`compat/` | Linuxの口のうちChromiumが使うもの(`epoll`、`eventfd`、`signal`など)のヘッダと実体 |
| `tools/browser/` | ChromiumのソースをTessronOSのツールチェーンでコンパイルし直すスクリプト、除外の一覧、Chromiumのソースへのパッチ |
| `build_make/browser.mk` | TessronOSのmakefileが`make BROWSER=1`のときに読む規則 |
| `tests/uprog/v8prog.cc`、`skiaprog.cc` | V8とSkiaの試験プログラム |

以上は`tessron-chromium`のリポジトリの中の場所である。TessronOSの側には、プロセスのC++とPOSIXの実行環境(`lib/libcxxrt`、`lib/libpthread`、`include/tscxx`、`include/tsposix`)、プログラムと保存用の実身の定義(`etc/def`、`etc/xtad`)、試験`tests/ktest/ktest_v8.c`がある。

## 14.3 ビルド

Chromium本体はどちらのリポジトリにも含めない。WSLのホームにChromiumのチェックアウト(`~/cr6/src`、DEPSで固定した依存を含む)を置き、`tools/browser/translate.py`が、Chromiumのビルドシステム(gn)が出力するコンパイルのコマンドから定義とインクルードと言語の水準を取り出して、TessronOSのツールチェーン(aarch64-none-elfのgccとnewlib)でコンパイルし直す。生成物は`~/tessronos-obj/tessronos`に置き、ソースかコマンドが変わったものだけをコンパイルし直す。

```sh
cd tessron-chromium/tools/browser
bash build.sh gngen                        # aarch64のビルドの記述(out/tessronos)
bash build.sh hostgen                      # ホストで作るもの(Torqueの出力、スナップショット、ICUのデータ)
JOBS=14 bash build.sh v8                   # V8とその依存
JOBS=6 bash blink.sh all                   # Blink、base、net、ccなどの約16,000ファイル
bash rust.sh build && bash rust.sh merge   # Rustで書かれた部分
bash ffmpeg.sh                             # FFmpeg
cd ../../../TessronOS/build_make
make TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1
```

- ChromiumのソースはLinux向けとして読ませ(`__linux__`と`__TESSRONOS__`を定義する)、足りないLinuxとglibcの口は`application/browser/port/shim`とTessronOSの`include/tsposix`で補う。
- Chromiumのソースは書き換えない。変更が要るものは`tools/browser/patches`のパッチを、コンパイルするときに写しに当てる。
- 使わない部分(テスト、GPUのバックエンド、プロセス間のサンドボックス、QUICなど)は`tools/browser/exclude_blink.txt`で外す。
- 同じ目標の8ファイルを1つの翻訳単位にまとめてコンパイルする。Blinkの1つの翻訳単位は1GB以上のメモリを使うので、並列はメモリ16GBで6までにする。最初から全部をコンパイルすると数時間かかる。
- 結ぶときに解決できない記号には、`tools/browser/blink_stubs.py`がスタブを作る。関数は最初に呼ばれたときに名前を一度表示して0を返し、変数は0で埋めた領域になる。
- Rustで書かれた部分(JPEG XL、AVIF、JSON、XML、Webフォントの形式の判定、Skiaのフォントの読込み)は、Chromiumのビルド規則でaarch64向けにビルドし、1つのオブジェクトファイルにまとめて結ぶ。
- 実行ファイルは約118MBで、プログラムの実身のレコード1になる。`BROWSER_BLINK=1`のときは、システムボリュームを320MB、試験用ディスクを1GBにする。

`tessron-chromium`は、TessronOSのリポジトリと同じディレクトリに置く。別の場所に置くときは、makeに`CHROMIUM=<場所>`を、スクリプトを直接実行するときは環境変数`TESSRONOS=<TessronOSのリポジトリ>`を与える。

## 14.4 エンジンの起動

Chromiumの`content`層は使わない。ブラウザのプロセスの中で、`base`の終了時処理、コマンドライン、機能の一覧、スレッドプール、プロセス内のMojoを用意し、`blink::Platform`の実装(埋め込んだ`blink_resources.pak`の資源、フォント、既定の言語`ja`、ユーザエージェント)、主スレッドのスケジューラ、V8を順に起動する。ページは`Page`と`LocalFrame`で作り、描くときは文書のライフサイクルを進めて、描画の記録をSkiaの面に再生し、ウインドウに`dp_put_argb`で置く。

フォントは書体箱のNoto Sans JPの実身のレコード1を使う。どのファミリー名にもこの1つを返すフォントマネージャを置くが、アイコン用のフォント(Segoe Fluent Iconsなど)の名前には何も返さず、ページやテーマが自分で描くようにする。

## 14.5 ネットワーク

ネットワークスタック(`URLRequestContext`)は専用のスレッドで動き、ソケットはノンブロッキングで使う。名前解決は1件ごとのスレッドで`getaddrinfo`を呼ぶ。ページの部品は並行して取得し、ページを表示するのは部品がそろってから(最長30秒)である。

- 同時に持つ接続は32までに抑え、それを超える要求はソケットが空くのを待つ。システム全体のソケットの上限は64である(9.2)。
- TLSはChromiumのBoringSSLで、サーバの証明書はChromiumのルートストアの証明書(実行ファイルに埋め込む)で検証する。期限の切れた証明書のサイトは開かない。
- Cookie、`localStorage`、HTTPキャッシュは、学習箱にある実身「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」のレコード1にしまう(14.8)。Cookieと`localStorage`は変わってから2秒後、キャッシュは新しいものが来てから1分後に書き直し、終了時に残りを書く。
- フォームの`POST`は、本文と種類をアプリが受け取り、ネットワークスタックで送る。
- IPv6は使わない。IPv6に届くかを調べるUDPのソケットには、スタブが応答する。

## 14.6 画像、動画、音声、WebGL

画像はlibpng(Skiaのコーデック)、libjpeg-turbo、Wuffs(GIF)、libwebp、JPEG XLとAVIF(Rust、AVIFはdav1dを使う)で復号する。SVGとXHTMLはRustのXMLパーサで読む。

動画と音声はFFmpeg 4.4.6で復号する。FFmpegは、アセンブリとスレッドなし、OSなしのnewlibの構成でビルドし、MP4、Matroska、WebM、Ogg、MP3、WAV、AAC、FLACのデマルチプレクサと、H.264、VP8、VP9、Theora、MPEG-4、AAC、MP3、Vorbis、Opus、FLAC、PCMのデコーダだけを入れる。プレーヤは資源を丸ごと取得してから分けるので、要素の`preload`が`auto`(または`autoplay`)のときだけ読み込み時に取得し、そうでなければ再生が始まるまで取得しない。音は48kHz、16bit、ステレオでサウンドデバイスの実身に書く。

WebGLはGLES2をCPUで実行し、GLSL ES 1.00の一部を解釈する。GPU(第13章)を使うのは、ANGLEを有効にする段階である。

## 14.7 ウインドウと操作

ページはウインドウの作業領域の全体に描く。ウインドウのタイトルは、表示しているページの題(題がなければアドレス)である。状態は、システムのメッセージ行に表示する。

アドレスの欄と操作のボタンは、ページのウインドウとは別の小さなウインドウ(ツールパネル、外寸560×48)にあり、ページのウインドウの8ドット上に置く。ツールパネルは、ページのウインドウの従属ウインドウで、ウインドウマネージャが常にページのウインドウのすぐ前に保つ(10.4)。

| 部品 | 動作 |
| --- | --- |
| ← | 戻る |
| → | 進む |
| 再 | 再読込 |
| × | 読込みの中止。それまでに届いたものでページを表示する |
| アドレスの欄 | クリックで入力を始める。Ctrl+Aで全体を選び、Enterで開き、Escで元に戻す |

右ボタンでウインドウのメニュー(「ブラウザのメニュー」)を出す。戻る、進む、再読込、アドレス入力、最初のページと、ウインドウの共通のメニューがある。キーでは、Backspace(ページの入力欄の外)とAlt+←で戻り、Alt+→で進む。ホイールと矢印キーはBlinkの中でスクロールし、ページの中のスクロールできる領域にも効く。ページ全体のスクロールバーは、Blinkのものではなく、ウインドウの右と下にあるTessronOSのスクロールバーである。ブラウザは表示のたびに、ページの高さと幅、表示している範囲をスクロールバーのレコードに書き、つまみの前後を押したときやつまみをドラッグしたときの`OB_E_SCROLL`(10.4)を受けて、その位置へスクロールする。ドラッグ中の通知は続けて届くので、最後の位置だけを描く。Blinkのスクロールバーはメインフレームのものだけを隠し(設定の`HideScrollbars`を、パッチ`viewport_scrollbars.patch`でメインフレームのビューポートに限る)、ページの中のスクロールできる領域とiframeは自分のスクロールバーを持つ。ページの入力欄に入力するときは、キーを先にかな漢字変換に渡し、確定した文字を欄に入れる。変換中の文字は欄の中に下線付きで表示し、候補はブラウザが文字の位置の下に表示する。

## 14.8 実身

| 実身 | UUID | 内容 |
| --- | --- | --- |
| プログラム「ブラウザ」 | `01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57`(`SYSDEF_PROG_BROWSER`) | プログラム箱にある。レコード1が実行ファイル。idは`web-browser` |
| 原紙「ブラウザ」 | `01a0d8c4-5b22-7b13-9d54-8a6f7e8c9d01` | 原紙箱に並ぶ。複製するとページの実身になる。アドレスは`https://www.tron.org` |
| 「ブラウザのメニュー」 | `01a0d889-46bc-7e8b-8a9c-d2e3f4a5b678`(`SYSDEF_MENU_BROWSER`) | メニューの定義 |
| 「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」 | `SYSDEF_BROWSER_COOKIE`、`SYSDEF_BROWSER_STORAGE`、`SYSDEF_BROWSER_CACHE` | 学習箱にある。レコード1に、期限付きのCookie、起源ごとの`localStorage`、HTTPキャッシュを持つ |

ページの実身は、メタデータ`webBrowser`に開いているページのアドレス、題、戻る履歴と進む履歴を、`window`にウインドウの位置と大きさを持つ。レコード0は題とアドレスの文章で、基本文章編集でも開ける。ページの実身の仮身を開くと、デスクトップがプログラム「ブラウザ」をその実身のUUIDを引数にして起動し、ブラウザは実身のアドレスのページを開く。原紙のアドレスは`https://www.tron.org`なので、原紙から作ったページの実身は最初にトロンフォーラムのページを開く。アドレスを持たない実身は空白のページ(`about:blank`)を開く。実身を指定せずに起動したときは、ブラウザの説明のページ(`about:home`)を開く。同じページの実身をもう一度開くと、表示しているウインドウを前面に出す。

## 14.9 未対応のもの

- 移植していないWebのインタフェース(約490。`AudioContext`、`WebSocket`、`indexedDB`、`EventSource`、`Notification`など)は、その機能を持たないブラウザと同じく、グローバルのプロパティが`undefined`になる。
- `crypto.subtle`はダイジェスト(SHA-256、SHA-384、SHA-512)だけを計算し、ほかの操作は拒否する。
- JIT、WebAssembly、Service Worker、QUIC、IPv6、GPUを使った合成は使わない。
- 画像の復号器のうち、Rustで書かれたPNG、BMP、ICOは使わず、C++の復号器を使う。

## 14.10 試験

試験グループ`v8`(`tests/ktest/ktest_v8.c`)は、`make BROWSER=1 BROWSER_BLINK=1`でビルドしたときにブラウザを動かして確かめる。ブラウザが入っていないときは各試験を飛ばす。試験のタスクがループバックでHTTPのサーバになり、ブラウザのプロセスが求めたパスと、画面の画素と、ページの実身に書かれた値を見る。

| 試験 | 内容 |
| --- | --- |
| `test_script`、`test_skia` | V8でJavaScriptを実行する、Skiaで描いた図形と日本語をウインドウに出す |
| `test_browser`、`test_blink` | リンクと転送をたどる、Blinkでスタイルシートとスクリプトのあるページを組んで描く、画像4種類、Cookie、`localStorage`、canvasの2D |
| `test_blink_input` | 入力欄への入力とフォームの送信、ホイールのスクロール、かな漢字変換 |
| `test_blink_keep` | フォームの`POST`、次に起動したブラウザがCookieと`localStorage`を読み戻す |
| `test_blink_media` | WebGL、音声の長さ、`crypto`、`preload`のない動画を取得しないこと |
| `test_blink_menu`、`test_blink_tool` | ウインドウのメニューの再読込、ツールパネルでの移動とボタン、ツールパネルがページのウインドウのすぐ前にあること |
| `test_blink_mem` | 大きなページを読んだ後のメモリの使用量 |
| `test_desk` | デスクトップからページの実身を開く、同じ実身をもう一度開く |
| `test_https` | `NET=user`のときだけ。外部のサイトを開いて画面を写す。期限切れの証明書のサイトを拒否する |

`make ... KT_V8=test_blink_media,test_blink_tool`で、`v8`のうち指定した試験だけを実行する。`make ... NET=user KT_SITE=<URL>`では、そのサイトだけを開き、読み終えて1秒後と3秒後の画面を`/boot/SITE.PPM`、`/boot/SITE2.PPM`に写す。

2026年9月30日、QEMUの4コアで実際のサイトを開いた結果は次のとおりである。

| サイト | 部品がそろうまで | 同時に取得した最大数 |
| --- | --- | --- |
| `https://www.tron.org/ja/` | 18.6秒 | 68 |
| `https://www.wikipedia.org/` | 3.7秒 | 3 |
| `https://ja.wikipedia.org/wiki/BTRON` | 3.9秒 | 9 |
| `https://news.ycombinator.com/` | 1.4秒 | - |
| `https://html.duckduckgo.com/html/?q=TRON` | 1.9秒 | 13 |

## 14.11 ライセンス

`tessron-chromium`のファイルはT-License 2.2で配布する。`BROWSER=1`でビルドしたイメージには、Chromium(Blink、V8、Skia、`base`、`net`など)とその第三者コンポーネント、BoringSSL、FFmpeg、dav1d、Rustのクレートが含まれる。一覧と条件は`THIRD_PARTY_NOTICES.md`にある。
