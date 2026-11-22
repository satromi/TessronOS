# 第14章　ブラウザ

## 14.1 概要

小物「ブラウザ」は、Chromium 153(153.0.8010.36)のレンダリングエンジン一式、つまりBlink、V8 15.3、Skiaを、TessronOSのプロセスとして動くようにしたWebブラウザである。作者がBTRON(超漢字)向けに作ったChromiumの移植から、移植レイヤと画面の構成を引き継ぎ、システムコールをTessronOSのもの(POSIXのソケット、実身、POSIXスレッド)に置き換えた。

ソースコードはTessronOSとは別のリポジトリ`tessron-chromium`にあり、`make BROWSER=1 BROWSER_BLINK=1`でイメージを作るときに取り込む(14.3)。取り込まないときは、プログラムの実身のレコード1に、ブラウザが入っていないことをメッセージ行に表示するだけの小さなプログラム(`application/browser/br_stub.c`)が入る。

| 項目 | 内容 |
| --- | --- |
| ページの表示 | HTML、CSS、JavaScriptとWebAssembly(V8。どちらも機械語にコンパイルして実行する)、画像(PNG、JPEG、GIF、WebP、JPEG XL、AVIF、SVG)、Webフォント、canvasの2D、WebGL 1と2(`make GL=1`のときはANGLEとMesaで描画する。そうでなければWebGL 1だけをCPUで描画する)。`make GL=1`のブラウザは、GPUのあるマシンでページとcanvasの2DをSkiaのGaneshでGPUラスタライズする(14.7.2)。iframe、専用ワーカー(`blob:`のURLのものも)、画像のURLを直接開いたときの画像文書(14.6) |
| ページの機能 | `crypto.subtle`、`CompressionStream`、`BroadcastChannel`、Web Locks、Permissions、`Notification`(デスクトップの通知で表示する。14.4.10)、`navigator.storage`、`storage`イベント、`createImageBitmap`と`OffscreenCanvas`の2D、`navigator.sendBeacon`、`navigator.clipboard`、Cache Storage(14.4.4、14.4.5) |
| 通信 | Chromiumのネットワークスタック。HTTP/1.1、HTTP/2、HTTP/3(QUIC)、TLS 1.2と1.3、Cookie、HTTPキャッシュ、フォームの`POST`、`fetch`と`XMLHttpRequest`、`WebSocket`、`EventSource` |
| 動画と音声 | `<video>`と`<audio>`、Media Source Extensions(`MediaSource`。YouTubeなどの動画サイトが使う)、Encrypted Media Extensions(Clear Keyだけ)、MediaCapabilities。Chromiumのメディアパイプラインで再生し、音はサウンドデバイスに出す。Web Audio(`AudioContext`)の音もサウンドデバイスに出す |
| 入力 | キーボード、ポインタ、ホイール、かな漢字変換。ページの`alert`、`confirm`、`prompt`、`<select>`の一覧、許可の確認は、ブラウザのデータボックスのパネルで尋ねる(14.7.3) |
| 保存 | 開いているページのアドレスと履歴はページの実身に、Cookie、`localStorage`、`indexedDB`、HTTPキャッシュ、Cache Storageはそれぞれの実身に保存する(保存域のクォータは14.4.5.1)。ページの複写と貼り込みはシステムのトレーを使う |

実際のサイトとして、`https://www.tron.org/ja/`、`https://www.wikipedia.org/`、`https://ja.wikipedia.org/wiki/BTRON`、`https://news.ycombinator.com/`、`https://html.duckduckgo.com/`、`https://www.gnu.org/`、`https://example.com/`をQEMU上で読み込み、表示を確認した(14.10)。

## 14.2 構成

| 場所 | 内容 |
| --- | --- |
| `application/browser/br_main.cc`、`br_main_blink.cc` | ウインドウ、ツールパネル、入力、ページの実身の読み書き。Blinkを使うビルドは、`br_main_blink.cc`が`br_main.cc`をインクルードして作る |
| `application/browser/br_blink.cc`、`br_bindings.cc`、`br_loader.cc` | Blinkの起動、ページの組み立てと描画、V8のバインディングの登録、サブリソース(スタイルシート、画像、スクリプト)の読み込み |
| `application/browser/br_http.cc`、`br_tls.cc`、`br_text.cc` | Blinkを使わないビルドのHTTP、TLS、テキストだけの描画 |
| `application/browser/br_obj.cc` | ページの実身のメタデータとレコード0 |
| `application/browser/br_proc.cc`、`br_rproc.cc`、`br_proc.h`、`mojom/br_process.mojom` | ブラウザプロセスとレンダラプロセス(14.13)。レンダラの起動と予備と上限、レンダラのページを表す`RemotePage`、レンダラ側のループとページ、両者の間のMojoのインタフェース |
| `application/browser/port/net/` | ネットワークスタックの下位レイヤ(TCPとUDPのソケット、名前解決、ルート証明書、Cookie・保存域・キャッシュの保存)、ページのクリップボードとして使うトレー(`br_tray.cc`) |
| `application/browser/port/base/`、`blink/`、`skia/`、`v8/` | Chromiumの`base`、Blink、Skia、V8がOSに求める機能(メモリ、スレッド、時刻、`localStorage`、`WebSocket`のチャネル、IndexedDBのバックエンド、フォントマネージャ、PNGのコーデック)。`blink/`には、Blobのバックエンド(`blob_tessronos.cc`)、専用ワーカーのホスト(`worker_tessronos.cc`)、ブラウザ側のサービス(`services_tessronos.cc`、`cache_storage_tessronos.cc`)もある |
| `application/browser/port/media/`、`webgl/` | メディア要素のプレーヤ(Chromiumのメディアパイプラインとの接続、音と映像の出力、EMEのエントリポイント)、Web Audioの出力、WebGLのGLES2(ANGLEに渡すものと、CPUで実行するもの)とGLSLのインタプリタ |
| `application/browser/port/gpu/` | SkiaのGaneshによる、ページとcanvasの2DのGPUラスタライズ(GPUのスレッド、14.7.2) |
| `application/browser/port/shim/`、`compat/` | LinuxのインタフェースのうちChromiumが使うもの(`epoll`、`eventfd`、`signal`など)のヘッダと実装 |
| `tools/browser/` | ChromiumのソースをTessronOSのツールチェーンでコンパイルし直すスクリプト、除外リスト、Chromiumのソースへのパッチ |
| `build_make/browser.mk` | TessronOSのmakefileが`make BROWSER=1`のときに読み込むルール |
| `tests/uprog/v8prog.cc`、`skiaprog.cc` | V8とSkiaのテストプログラム |

以上は`tessron-chromium`のリポジトリの中の場所である。TessronOSの側には、プロセス用のC++とPOSIXのランタイム(`lib/libcxxrt`、`lib/libpthread`、`include/tscxx`、`include/tsposix`)、プログラムと保存用の実身とデータボックスの定義(`etc/def`、`etc/xtad`)、テスト`tests/ktest/ktest_v8.c`がある。ブラウザはパネルを`lib/libui`で出す。`build_make/cxxrt.mk`はそれを`libtsui.a`にし、`build.sh`がブラウザにリンクする。`libcxxrt.a`はC++のプログラムのすべてにまるごとリンクするので、`lib/libui`を自分でリンクするCのプログラム(基本表計算)と重複しないように、別のライブラリにしている。

## 14.3 ビルド

Chromium本体はどちらのリポジトリにも含めない。WSLのホームにChromiumのチェックアウト(`~/cr6/src`、DEPSで固定した依存を含む)を置き、`tools/browser/translate.py`が、Chromiumのビルドシステム(gn)が出力するコンパイルのコマンドから定義とインクルードと言語の規格を取り出して、TessronOSのツールチェーン(aarch64-none-elfのgccとnewlib)でコンパイルし直す。生成物は`~/tessronos-obj/tessronos`に置き、ソースかコマンドが変わったものだけをコンパイルし直す。

```sh
cd tessron-chromium/tools/browser
bash build.sh gngen                        # aarch64のビルドの記述(out/tessronos)
bash build.sh hostgen                      # ホストで作るもの(Torqueの出力、スナップショット、ICUのデータ)
JOBS=14 bash build.sh v8                   # V8とその依存
JOBS=6 bash blink.sh all                   # Blink、base、net、ccなどの約16,000ファイル
bash rust.sh build && bash rust.sh merge   # Rustで書かれた部分
cd ../../../TessronOS/build_make
make TARGET=_QEMU_VIRT_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 GL=1
```

`GL=1`を付けると、`build.sh`はANGLE(約310ファイル)をコンパイルして、Mesaの静的ライブラリ(`libtsgl.a`、13.6.5)とともにブラウザにリンクし、WebGLをANGLEで描画する(14.6.1)。GPUラスタライズ(`port/gpu/br_gpu.cc`、14.7.2)も一緒にリンクする。SkiaのGanesh(GLのバックエンドを含む)はBlinkの依存としていつもコンパイルしてあり、`br_gpu.cc`が呼ぶときだけリンクされる。付けなければANGLEもMesaもリンクしない。

- ChromiumのソースはLinux向けとしてコンパイルし(`__linux__`と`__TESSRONOS__`を定義する)、足りないLinuxとglibcのインタフェースは`application/browser/port/shim`とTessronOSの`include/tsposix`で補う。
- Chromiumのソースは書き換えない。変更が要るものは`tools/browser/patches`のパッチを、コンパイルするときにコピーに適用する。ソースのコピーは`~/tessronos-obj/_patched`に置いてそのソースのコンパイルに使う。ヘッダのコピーは`~/tessronos-obj/_patched_inc`に置き、元のヘッダが見つかるインクルードディレクトリ(チェックアウトの最上位を除く)の前に、コピーの同じディレクトリを加える。
- gnの引数(`tools/browser/args.gn`)で、V8を機械語のコンパイラ(Sparkplug、Maglev、Turbofan、WebAssemblyのLiftoff)とともにビルドする(`v8_jitless = false`)。WebAssemblyのインタプリタDrumBrakeはビルドしない(`v8_enable_drumbrake = false`)。DrumBrakeをgccでビルドするためのパッチ(`wasm_interpreter.patch`、`wasm_interpreter_h.patch`)は、作り直すときのために残している。パッチを当てたヘッダのコピーのディレクトリはV8のすべてのソースのインクルードディレクトリに加わるので、パッチを外すとV8のすべてのソースがコンパイルし直しになる。ポインタ圧縮を有効にし、その4GBのケージはプロセスの中で1つを共有する。この設定はホストの`mksnapshot`にも効き、スナップショットに入る組込み関数の一覧がプログラムのものと一致する。設定を変えたときは`build.sh gngen`と`build.sh hostgen`をやり直す。
- 使わない部分(テスト、GPUのバックエンド、プロセス間のサンドボックスなど)は`tools/browser/exclude_blink.txt`で外す。ネットワークスタックはQUIC(`net/quic`、`third_party/quiche`、BoringSSLのQUICの部分)も含めてコンパイルし、システムに問い合わせる部分(TCPとUDPのソケット、名前解決など)だけを外して、移植レイヤの`port/net`のものを使う。
- 同じターゲットの8ファイルを1つの翻訳単位にまとめてコンパイルする。Blinkの1つの翻訳単位は1GBから3GBのメモリを使うので、並列数はメモリ16GBで6までにする。最初から全部をコンパイルすると数時間かかる。WSLが使うメモリはWindowsから見るとコミット済みのメモリになり、Windowsのページファイル(C:にある)がそれに合わせて大きくなる。C:の空きが少ないときは、並列数を減らし、WSLのページキャッシュをこまめに破棄する。WSLのメモリが12GBのときは4並列にし、`TRANSLATE_TUNE="--param=ggc-min-expand=20 --param=ggc-min-heapsize=262144"`でgccの使うメモリを抑える(7並列ではWSLのメモリとスワップを使い切った)。構造体のパッキングを`#pragma pack(<数>)`で変えて戻さないソース(WebRTCの`wav_header.cc`)は、後ろのソースの構造体の配置を変えてしまうので、翻訳単位から外して単独でコンパイルする(翻訳単位の区切りはそのまま)。
- リンク時に解決できないシンボルには、`tools/browser/blink_stubs.py`がスタブを作る。関数は最初に呼ばれたときに名前を一度表示して0を返し、変数は0で埋めた領域になる。移植レイヤに足した関数も、定義を無名の名前空間に置くなどしてリンクされないと気づかないうちにスタブになり、`std::string`のように値を返す関数では呼び出した側のメモリが破壊される。新しいインタフェースを足したら、実行ログに`browser: stand-in called:`でその名前が出ないことを確認する。Cの名前は関数か変数かを名前から見分けられないので、移植レイヤの変数がリンクされないと関数のスタブ(`.text`の数バイト)が割り当てられ、そこに書き込んだときに書き込み保護違反で異常終了する(2026年10月、`br_net_quic`で起きた)。そのため、移植レイヤの名前が解決できないときは、スタブを作らずにビルドを止める。無名の名前空間の中の名前(nmの名前に`_GLOBAL__N_`を含むもの)は、エンジンのものも含めて、解決できないときは常に止める。内部リンケージなので、それを参照するオブジェクトファイルの中でしか定義できず、ほかのオブジェクトファイルが定義を持つことはない。参照する側の宣言を無名の名前空間の中に置いた誤りである(2026年11月、`br_proc.cc`の`br_quota_volume_line`で起きた)。移植レイヤの名前とは、名前空間のないCの名前とC++の関数・変数のうち`br_`、`ts_`、`knl_`、`tessronos`、TessronOSのAPIの接頭辞(`tk_`、`wm_`、`ob_`、`so_`、`dp_`、`ui_`、`fs_`、`om_`、`gp_`、`ns_`、`usb_`)で始まるものと、`tessronos::`、`br::`、`brgl::`の中や`Br`で始まるクラスのC++の名前である。`blink::`、`gpu::`、`base::`、`net::`などエンジンの名前空間の中の名前は、これまでどおりスタブにする。止めるときは、名前ごとに、その名前を参照している移植レイヤのオブジェクトファイル(`port/`と`compat/`の`.o`)と、同じ名前の定義を無名の名前空間や`static`の中に持つオブジェクトファイルを出力し、推定される原因(無名の名前空間の中への定義、宣言と定義の食い違い、ビルドされていないファイル)を添える。わざとスタブにする名前は、`tools/browser/blink_stubs_allow.txt`に1行に1つ、nmが出力する名前と`#`のあとに理由を書いて許可する。今は空である。この検査は`blink_stubs.py`だけで動くので、未定義の名前を持つオブジェクトファイルを`ld -r`でまとめて渡せば、ブラウザ全体をリンクせずにテストできる。
- `blink.sh all`はBlinkのcore、`base`、`net`などをコンパイルし、makeはそれをコンパイルし直さない。coreのソースに当てるパッチ(`loader_factory_for_frame.patch`など)を足したり変えたりしたときは、`blink.sh all`をやり直す(変わった翻訳単位だけをコンパイルするので数分で済む)。modulesのうちリンクするもの(`blink_modules_src.txt`)とバインディング(`blink_modules.txt`)は、makeのたびに変わったものをコンパイルする。
- メディアパイプラインのデコーダのために、gnの引数で`proprietary_codecs = true`、`ffmpeg_branding = "Chrome"`、`enable_hls_demuxer = true`とし、ChromiumのFFmpeg(H.264とAACを含む構成)、libvpx、libopusをBlinkと一緒にコンパイルする。FFmpegのNEONのアセンブリ(16ファイル)はコンパイルのコマンドに含まれないので、`build.sh`が`ffmpeg_internal.ninja`の定義とインクルードディレクトリでアセンブルする。libvpxのうちドット積、行列積、SVEの命令を使うファイルは、`flags.txt`でその命令を許可してコンパイルする。これらは実行時にプロセッサがその命令を持つと答えたときだけ呼ばれ、TessronOSでは呼ばれない。
- Rustで書かれた部分(JPEG XL、AVIF、JSON、XML、Webフォントの形式の判定、Skiaのフォントの読込み)は、Chromiumのビルドルールでaarch64向けにビルドし、1つのオブジェクトファイルにまとめてリンクする。
- パッチ`canvas_gpu.patch`は`canvas_2d_bitmap_provider.cc`だけを変え、ヘッダは変えないので、当てたときにコンパイルし直すのはその翻訳単位だけである(`blink.sh all`)。
- 実行ファイルは約122MB(JITを入れる前は約106MB、`GL=1`でANGLEとMesaをリンクすると約142MB)で、プログラムの実身のレコード1になる。`BROWSER_BLINK=1`のときは、システムボリュームを512MB、テスト用ディスクを1GB(中身が増えれば自動で広がる)にする。Raspberry PiのSDカードのイメージでは、システムボリュームはそれより先に2GBと決めてある(3.3)。

`tessron-chromium`は、TessronOSのリポジトリと同じディレクトリに置く。別の場所に置くときは、makeに`CHROMIUM=<場所>`を、スクリプトを直接実行するときは環境変数`TESSRONOS=<TessronOSのリポジトリ>`を与える。

### 14.3.1 x64

x64のブラウザ(3.10)は、ARMの移植をそのまま土台にし、同じChromiumのチェックアウト、同じパッチ、同じ移植レイヤから作る。上流のLinux版から移植し直してはいない。x64で別に持つのは、gnの引数、ツールチェーン、CPUに依存するシムとNASMのアセンブリだけで、移植レイヤのコードはARMと共通である。違いは`#if defined(__x86_64__)`の分岐にまとめた。

スクリプト(`build.sh`、`translate.py`、`gngen.sh`、`hostgen.sh`、`blink.sh`、`rust.sh`、`speech.sh`)は、環境変数`TS_ARCH=x86_64`でx64向けにビルドする。付けないときはARMで、コンパイルのコマンドは以前と一字一句同じなので、ARMのオブジェクトファイルは作り直しにならない。`make TARGET=_X64_ BROWSER=1 BROWSER_BLINK=1`は、`cxxrt.mk`の`BROWSER_ARCH_ENV`で`TS_ARCH=x86_64`を渡す。

```sh
cd tessron-chromium/tools/browser
TS_ARCH=x86_64 bash gngen.sh               # out/tessronos_x64(args_x64.gn)
TS_ARCH=x86_64 bash hostgen.sh             # mksnapshot、Torque、ICUのデータ
DAWN=1 TS_ARCH=x86_64 bash gngen.sh        # out/tessronos_x64_dawn
TS_ARCH=x86_64 bash build.sh v8            # V8とその依存
TS_ARCH=x86_64 bash blink.sh labels && TS_ARCH=x86_64 bash blink.sh gen
TS_ARCH=x86_64 JOBS=4 bash blink.sh all    # Blinkの全体
TS_ARCH=x86_64 bash rust.sh build && TS_ARCH=x86_64 bash rust.sh merge
cd ../../../TessronOS/build_make
make TARGET=_X64_ DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 GL=1 BUILD_ID=<名前> sdimg
```

| 項目 | ARM | x64 |
| --- | --- | --- |
| gnの引数 | `tools/browser/args.gn`(`target_cpu = "arm64"`) | `tools/browser/args_x64.gn`(`target_cpu = "x64"`) |
| gnの出力 | `~/cr6/src/out/tessronos`、`tessronos_dawn` | `~/cr6/src/out/tessronos_x64`、`tessronos_x64_dawn` |
| オブジェクトファイル | `~/tessronos-obj/tessronos` | `~/tessronos-obj-x64/tessronos_x64` |
| コンパイラ | `aarch64-none-elf-gcc`、`-march=armv8.2-a` | `x86_64-elf-gcc`、`-march=x86-64-v2` |
| リンカスクリプト、crti・crtn | `etc/linker/user/program_cxx.ld`、ツールチェーンのもの | `etc/linker/x64/program_cxx.ld`、`lib/libcxxrt/x86_64`のもの |
| Rust | `aarch64-unknown-linux-gnu` | `x86_64-unknown-linux-gnu` |
| Mesa | `~/tsmesa/libtsgl.a` | `~/tsmesa-x64/libtsgl.a`(softpipeとiris) |
| 生成物 | `UOBJ`(`~/tessronos-obj-ts/<ツリー>/user_<BUILD_ID>/`)の`browser.elf`など(`BROWSER_OUT`、12章12.2.1) | x64の`UOBJ`(`…/user_<BUILD_ID>/x64/`)の`browser.elf`など |

- gnの引数は、ARMのものに対して`target_cpu`と、`host_toolchain`・`v8_snapshot_toolchain`を`//build/toolchain/linux:clang_x64`にしたことだけが違う。ホストもターゲットもx64なので、gnの既定ではターゲットのgccのツールチェーンがホストのツールとmksnapshotもビルドすることになる。ターゲットのコマンドは`translate.py`が読むだけなので、ホストのツールとmksnapshotはChromiumのclangでビルドする。mksnapshotはx64のホストでそのまま動き、`embedded.S`はx86-64のアセンブリになる。`hostgen.sh`は、ターゲットのgccのツールチェーンが呼ぶ`gcc`、`g++`などを、出力ファイルを作るだけのダミーに置き換える(ARMの`aarch64-linux-gnu-gcc`と同じ扱い)。
- 命令セットの基準はTessronOSのx64と同じx86-64-v2である。x64では、gnがターゲットごとに付ける`-march=`、`-mavx2`などを落とさず、基準の`-march=x86-64-v2`の後ろに残す。Skiaの`SkOpts_hsw`、skcmsの`TransformHsw`と`TransformSkx`、libyuv、dav1dなどのAVX2とAVX-512の部分はその命令でコンパイルされ、実行時にCPUIDとXGETBVで選ばれる。カーネルはAVXまでを有効にするので(3.10、XCR0)、AVX-512の部分は選ばれない。
- dav1d、FFmpeg、libaom、libjpeg-turbo、libvpx、OpenH264のx86のSIMDの部分はNASMのソースで、gnはそれを独自のアクションでアセンブルする。compile_commands.jsonに出ないので、`tools/browser/nasm_objs.py`が`toolchain.ninja`のアクションからフラグとソースを読み、ツールチェーンのNASM(12.1)でアセンブルして`libnasm.a`にする。リンクでは呼ばれるものだけを取り込む。BoringSSLはARMと同じくアセンブリを使わない(`OPENSSL_NO_ASM`)。
- `port/shim_x64`(`sys/ucontext.h`、`sys/syscall.h`)を`port/shim`より前にインクルードする。`sys/ucontext.h`はx86-64のLinuxの`mcontext_t`(`gregs`と`REG_RIP`などの番号)、`sys/syscall.h`はx86-64のLinuxのシステムコールの番号である(x86_64-unknown-linux-gnuのRustのコードも同じ番号を渡すため)。`port/shim`のファイルは変えていない。
- x64だけのフラグと外すソースは`tools/browser/flags_x64.txt`と`exclude_x64.txt`に書く。`x86_64-elf`のnewlibにないLinuxの名前(`PROT_GROWSDOWN`、`SI_KERNEL`、`SYS_rt_sigaction`)を、それを使うソースにだけ与える。
- `blink.sh gen`は、x64のgnの出力で作れない生成物を、ARMの出力(`out/tessronos/gen`)からコピーする。BlinkとMojoの生成物はアーキテクチャに依存しない。作れないのは、ホストのclangとシステムのlibstdc++(gcc 11)では通らないcryptoのソース1つに依存する生成物と、チェックアウトにないesbuildが要るDevToolsのinspector overlayに依存する`components/optimization_guide/core:features`である。
- Rustのコードは`x86_64-unknown-linux-gnu`向けにビルドする。RustのTLSはELFのTLSで、`lib/libpthread`のTLSのVariant II(FSのベース)が受ける。Cの名前の`open64`、`stat64`などに渡るLinuxのフラグと`struct stat`のレイアウトは、x86-64のLinuxのものに変換する(`port/compat/ts_libc.c`)。
- 処理時間を積算する箇所(エンジンのループの1回、タスク、ANGLEの呼出し)は、ARMでは汎用タイマのカウンタ、x64ではTSC(`rdtsc`)を読む(`port/base/counter_tessronos.h`)。TSCの1秒あたりのカウント数は、最初に単調時計と20ミリ秒の間で比べて求める(`br_counter_hz`)。
- 音声の合成と認識の部品はx64でも同じものをビルドする。whisper.cppのggmlは、x86向けのコード(`arch/x86`)をx86-64-v2でコンパイルする。
- zstdのHuffmanの復号はx86-64のアセンブリのループを呼ぶが、gnはそのソースをコンパイルのコマンドに出さないので、Cのループにする(`ZSTD_DISABLE_ASM`)。Web Audioの`OscillatorNode`のSSE2の部分(`cpu/x86/oscillator_kernel_sse2.cc`)を`blink_modules_src.txt`に加えた。
- Widevine(14.6.3.1)は、x64ではGoogleのコンポーネントアップデートのサービスが配布するLinux x64のCDMを、利用者が導入したときだけ使う。`port/compat/ts_so.c`はx86-64のglibcの共有オブジェクトを読み込み、`port/compat/ts_glibc.c`はx86-64のglibcの型の大きさとLinuxの番号でglibcの代わりを実装する。スレッドの制御ブロックは、glibcのものと同じく64バイトにして、0x28にスタック保護の値を置く(`lib/libpthread/sysdepend/x86_64/pth_cpu.h`)。
- HEVCのハードウェアデコーダ(14.6.7)はRaspberry Pi 5のものなので、x64ではQEMUと同じくHEVCを扱えないと答える。IntelのGPUの動画エンジンを持つマシンでは、デバイスの実身「動画の復号器」(`device.kind`が`"h264"`)でH.264をデコードし、扱えないストリームはFFmpegに戻す(14.6.7.1)。
- x64のイメージではテストプログラムがESP(パーティション1)に載るので、`BROWSER_BLINK=1`のときはESPを1GB、システムボリュームを768MBにする(`browser.mk`の`SD_BOOT_MB`と`SYS_VOL_SIZE`、指定すれば変えられる)。実行ファイルは約225MB(`GL=1`)である。

GPUは13章のとおりで、ANGLEはMesaのEGLのプラットフォーム`tessronos`の上で動き、IntelのGPUのマシンではiris、そうでなければsoftpipeで描画する。WebGPUはDawnのOpenGL ESのバックエンドで、システムのOpenGL ESが3.1を提供するときに使う(14.6.5)。実機で1回起動すれば分かるように、x64のブラウザは起動時に`browser: gpu: the machine's GPU is Intel's (kind 3, version …)`か`browser: gpu: the machine has no GPU the system drives (softpipe)`の行を出力する。続く`egl:`の行(13.8.3)がirisかsoftpipeかを、WebGLのコンテキストを作ったときの`browser: webgl: WebGL 2 on ANGLE, ANGLE (…)`の括弧がレンダラ名とOpenGL ESのバージョンを示す。WebGPUを使えるかどうかは、WebGPUを使ったときの`browser: webgpu: adapter …`の行か、その前の`browser: webgpu: the system's OpenGL ES makes no context of 3.1`の行で分かる。

x64で確認したこと(2026-10-21、QEMU q35、Windowsのハイパーバイザ、`GL=1`でsoftpipe):

- 2026年10月30日、会議のサイトに要るもの(14.5.2、14.4.8、14.4.12.1、14.6.2)を入れて、`test_blink_rtc`、`test_blink_meet`、`test_blink_coi`が成功した(`KT_V8`、サウンドデバイスは`intel-hda`)。`test_blink_meet`では、640×360、毎秒30フレームのcanvasを、VP8、VP9、H.264、AV1のどれでも毎秒28フレームでエンコードしてデコードした(Windowsのハイパーバイザ。実機のm3-8100Yではこれより遅い)。受信した音声は約33万サンプル再生され、画面の共有は1280×800の`monitor`で読めた。`tools/browser/ice_tcp_server.py`の相手には、TCPのactiveの候補で接続した(`ice-tcp connected pair=tcp,active`)。`KT_SITE`で、`https://meet.google.com/<コード>`は対応外のブラウザと表示されずに会議の画面(存在しないコードなので「ミーティング コードを確認」)まで、`https://app.zoom.us/wc/join`は「ミーティングに参加」の入力の画面まで進んだ。

- `https://www.tron.org/ja/`を読み込み(`KT_SITE`、`NET=user`)、タイトル「トロンフォーラム」で画像と日本語の本文を描画した。
- テストグループ`v8`の全体(`make TARGET=_X64_ KTEST=1 KTONLY=v8 DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 GL=1 NET=user sdimg`、`tools/x64/ktest64.sh`に`USBQ=1 NET=user`)は91件(V3Dを模擬する`test_blink_v3dfall`はスキップしていた。2026年10月23日からはx64ではIntelのGPUを模擬して実行する)で、2026年10月21日に下の修正を入れて実行したときは、`test_blink_gpu`がcanvasの画素の1点だけ違って失敗したほかは成功した(`test_blink_gpu`はその前の2回では成功した)。QEMU virtでは92件中、`test_blink_dialog`のほかは成功し、それを修正したあとの`test_blink_dialog`も成功した。以前はx64とQEMU virtの両方で失敗していたテストの原因と修正は次のとおりである。`test_jit`は、プログラムとV8の起動の時間が支配的なので、スクリプトが自分で測ったループの時間で比べるようにした。`test_blink_file`は、ページの3つの保存が同時に終わって保存箱を開き合い、1つが`E_BUSY`で失敗していた(保存を1つずつにした、14.7.4)。`test_blink_csp`は、Cookieを確認するパス`/csk`が、`test_blink_cachekeep`の同じ名前のパスに先に取られていた(`/csq`に分けた)。`test_blink_api5`は、保存域の許可のハンドルが作った`blob:`のURLを、登録が届く前に引いていた(表の「保存域の許可のハンドルの`createObjectURL`」)。`test_blink_menu`は、押す行が再読込の1つ上(灰色の「進む」)だった。メニューの区切り線も1行の高さを持つ。`test_blink_speech`の最初の1件は、前の`test_blink_iwa`で印刷のパネルに応答したEnterがページにも届いてボタンをもう一度押し、2つ目の印刷のパネルが応答のないまま残って、デスクトップが終了しなかった(パネルの表示中はキーをアプリに渡さないようにした、11章)。QEMU virtだけで失敗していた`test_blink_dialog`は、遅いマシンでページがまだ`prompt`の応答を受け取る前に`<select>`を押していた(タイトルとパネルを待つようにした)。`test_blink_capture`は、ブラウザと同じ方法でマシンにマイクがあるかを調べて判定を選び、`test_blink_speech`と`test_blink_audio`は、音声の出力デバイスがないマシンではスキップする。V8のJIT、WebAssemblyとそのトラップ、ワーカー、Service Worker、WebGL(ANGLEとsoftpipe)、WebGPU、WebRTC、メディア、PDF、音声の認識と合成、ゲームパッドとMIDI(ソフトウェアのUSBコントローラ)はx64でも成功した。
- `test_blink_rtc`は最初、送信する映像のエンコードで`std::bad_alloc`で終了した。WebRTCの`common_audio/wav_header.cc`が`#pragma pack(2)`を戻さずに終わり、8ファイルの翻訳単位の中でその後ろの`generic_frame_info.cc`の構造体がパックされた配置になって、`GenericFrameInfo`のコンストラクタが`part_of_chain`の中に書き込んでいた。`translate.py`は、`#pragma pack(<数>)`があって`pop`のないソースを、翻訳単位から外して単独でコンパイルする(翻訳単位の区切りは変えない。ARMとx64の両方)。

## 14.4 エンジンの起動

Chromiumの`content`レイヤは使わない。ブラウザプロセスの中で、`base`の終了時処理、コマンドライン、機能の一覧、スレッドプール、プロセス内のMojoを用意し、`blink::Platform`の実装(埋め込んだ`blink_resources.pak`のリソース、フォント、既定の言語`ja`、ユーザエージェント)、メインスレッドのスケジューラ、V8を順に起動する。ページは`Page`と`LocalFrame`で作る。描画するときは文書のライフサイクルを進めて、描画の記録を、システムがプロセスにマップしたウインドウの作業領域の面にSkiaで再生し、変わった矩形だけをウインドウに置く(14.7.1)。ページは常にアクティブで入力フォーカスを持つものとし(`FocusController`)、入力欄のキャレットを表示して点滅させる。

V8はJavaScriptをバイトコードのインタプリタ(Ignition)で実行し始め、よく実行される関数をSparkplug、Maglev、Turbofanで機械語にコンパイルする。WebAssemblyはLiftoffで機械語にしてから実行し、よく実行される関数をTurbofanでコンパイルし直す。`WebAssembly.instantiateStreaming`はBlinkの応答の読み込みのインタフェースで動く。

フォントは実身「書体」の実身のレコード1を使う(10.3)。ゴシック体のNoto Sans JPを既定と`sans-serif`に、明朝体のNoto Serif JPを`serif`に、等幅のNoto Sans Mono CJK JPを`monospace`に割り当てる。日本語のNoto CJKのフォントを入れたLinuxのデスクトップのChromeと同じ割り当てである。フォントマネージャは、明朝体と等幅にはそれぞれのファミリー名(`Noto Serif JP`、`Noto Sans Mono CJK JP`)で、ほかのどのファミリー名にもゴシック体を返す。実身「書体」に明朝体や等幅がなければ、そのファミリーもゴシック体になる。アイコン用のフォント(Segoe Fluent Iconsなど)の名前には何も返さず、ページやテーマが自分で描画するようにする。文書が文字エンコーディングを示さないときは、日本語のChromeと同じくShift_JISとして読む。

### 14.4.1 JITとメモリ

V8 15.3は、W^Xを守るためのページの保護属性の切り替え(以前の`--write-protect-code-memory`)を持たない。LinuxのAArch64では、コード領域(ポインタ圧縮のケージの中の64MBから128MB)を読み書きと実行の両方の属性で要求し(`kReadWriteExecute`)、そこに書き込んだコードを`FlushInstructionCache`の後にそのまま実行する。スレッドごとに書込みと実行を切り替える方式は、AppleのMAP_JIT(`pthread_jit_write_protect_np`)とx64のメモリ保護キー(PKU)にしかない。

TessronOSは書込みと実行を同時に許すページを作らない(5.3)ので、移植レイヤの`OS::SetPermissions`(`port/v8/platform-tessronos.cc`)は`kReadWriteExecute`を`TS_MEM_JIT`に対応させる。この範囲のページは、カーネルが例外を受けて書込みと実行を切り替える。V8がコードを書き込むとページは読み書き可能になり、それを実行すると、カーネルがデータキャッシュの書き戻しと命令キャッシュの無効化を行って読出しと実行に切り替え、同じプロセスを実行中のほかのプロセッサにコンテキスト同期をさせる。V8から見ると、ページは書き込めて実行できるメモリとして振る舞う。`kReadExecute`は読出しと実行のページに、`kNoAccessWillJitLater`は予約だけに対応させる。V8が自分で行うキャッシュの操作(`FlushInstructionCache`はLinuxとして`__builtin___clear_cache`を呼び、`CTR_EL0`を読んで`dc cvau`と`ic ivau`を実行する)は、EL0に許可してある。

ページは触れたときに作るので、コード領域の全体を一度に要求しても、使った分のメモリしか使わない。`DiscardSystemPages`は`ts_ctl_mem`の`TS_MEM_DISCARD`を呼び、破棄したページはシステムに返る(5.3)。読み書きのページも触れたときに作るので、`OS::HasLazyCommits`は真を返す。

ブラウザがメモリをシステムに返す方法は次のとおりである。`malloc`はnewlibのもので、ヒープは`_sbrk`の1つの予約(64GB)を1MBずつ読み書き可能にする(`lib/libcxxrt/ts_syscalls.c`)。ページは触れたときに作られる(5.3)。newlibがヒープの末尾を縮めると(`malloc_trim`)、新しい末尾より上のページを`TS_MEM_DISCARD`で返す(Linuxでbrkを下げるとページが返るのに当たる)。ヒープの途中の空きは、newlibが`mmap`を使わないので返らない。V8とpartition_allocは、使わなくなった範囲を`DecommitPages`(`ts_ctl_mem`の`TS_MEM_NONE`)、`mprotect`の`PROT_NONE`、`munmap`、`MAP_FIXED`の`mmap`で外すか、`madvise`の`MADV_DONTNEED`(partition_allocの空いたスロットスパン)と`DiscardSystemPages`で破棄する。どちらでもページはシステムに返る。

以前は、読み書きのページを予約やコミットの時点で全部作り、`MADV_DONTNEED`はその場で0で埋めるだけ、`DiscardSystemPages`は何もせず、ヒープを縮めてもページを返さなかった。2026年11月9日のSurface Go 2で、YouTubeを見ている間に`asked`が30秒で1.2GB増えて戻らなかった(5.3.1)のは、このため一度でも使ったメモリが全部残っていたからである。YouTubeは1ページのアプリなので、別の動画へ移っても文書は替わらない。その操作の前後で`asked`はほとんど増えておらず、前の文書が残ったためではない。

メモリの内訳は、30秒ごとと、文書を入れ替えるたびにコンソールに出力する(`BlinkEngine::memory`)。前の文書が解放されたかは`documents`と`contexts`で、何が増えたかはほかの項目で見分ける。

```text
browser: memory: malloc 210000 KB in use (arena 380000 KB), V8 70000 KB used of 90000 KB (external 120000 KB), Oilpan 30000 KB, partitions 150000 KB, memory objects 40 (20000 KB); documents 3, frames 3, contexts 3, detached 0, array buffers 900, media keys 0
browser: page https://example.com/ committed: malloc ...
```

| 項目 | 内容 |
| --- | --- |
| `malloc`、`arena` | newlibの`mallinfo`の使用中の量と、ヒープの大きさ |
| `V8` | JSのヒープの使用量と大きさ、`external`はArrayBufferなどV8の外のメモリ |
| `Oilpan` | Blinkのオブジェクトのヒープ |
| `partitions` | Blinkのpartition_allocのパーティションのコミットした量 |
| `memory objects` | このプロセスがマッピングしている`memfd_create`のメモリの実身(Mojoのデータパイプと共有メモリ)の数と、マッピングの大きさ(`ts_memfd_bytes`) |
| `documents`、`frames`、`contexts`、`detached` | BlinkのInstanceCountersの文書、フレーム、V8のコンテキスト、切り離されたScriptStateの数 |
| `array buffers`、`media keys` | ArrayBufferの中身とMediaKeysの数 |

ページを移ってもGCが実行されるまでは前の文書が残る。`documents`が移るたびに増え続けるなら、前の文書を何かが参照している。

2026年11月9日、Surface Go 2(メモリ8GB)でYouTubeを約20分見ていると、V8がZoneのメモリを確保できず(`V8 process OOM (Zone)`)、続いて64KBの確保(`base::AlignedAlloc`、newlibの`posix_memalign`)がNULLを返して、ブラウザが`IMMEDIATE_CRASH`の`int3`で停止した。JSのヒープは70MBほどだった。newlibの`malloc`は`mmap`を使わず、`_sbrk`のコミット(`ts_ctl_mem`)が拒否されたときだけNULLを返す。カーネルが拒否するのは、マシンの空きページが、要求した分と残しておく16MB(`UMEM_FLOOR`)の和より少ないときである。つまり、マシンの空きページが尽きていた。当時のカーネルは、プロセスが外したページを、そのプロセスのどれかのタスクがシステムコールの中にいる間は返さなかった(5.6.1)。ブラウザでは、ネットワークのスレッドが`poll`で、描画やGPUの待ちのスレッドがそれぞれのシステムコールで待ち、ほぼいつも誰かがシステムコールの中にいる。V8とpartition_allocが外したページが返るのは、全部のタスクが同時にシステムコールの外にいる瞬間だけだった。今は、デバイスにページを貸しているシステムコールの間だけ保留する。実機でこれが尽きた原因だったか、ほかに増えていたもの(GPUのBO、ヒープ)があったかは、30秒ごとのメモリの行(5.3.1の`held`、`asked`、`gpu`、`free`)で確認する。パネルに「一般保護例外(エラーコード0x1a)」と出ていたのは、IDTのベクタ3のゲートがDPL 0だったためで、今は「ブレークポイント」と出る(5.5.2)。

WebAssemblyのメモリの範囲外の参照は、V8のトラップハンドラがモジュールのトラップに変える(`EnableWebAssemblyTrapHandler`)。V8はモジュールのメモリの前後にアクセスできないページを置き、生成するコードは範囲を検査する命令を持たない。範囲外のロードとストアはそこでデータアボートになり、カーネルがそれをプロセスのエントリポイントに返す(`ts_def_flt`、5.5.2)。移植レイヤはそれを`SIGSEGV`として`sigaction`のハンドラ、つまりV8のトラップハンドラに渡し(`port/compat/ts_signal.c`)、ハンドラは戻り先をトラップのランディングパッドに書き換える。`siginfo_t`の`si_code`はLinuxの値にしてある(`port/shim/sys/signal.h`)。newlibでは`SI_USER`が1で`SEGV_MAPERR`と重なり、V8がカーネルの出したシグナルと認めないためである。WebAssemblyのものでない例外は、ハンドラが外れて同じアクセスがやり直され、これまでどおりプロセスが終了する。x64では、カーネルが状態をタスクのrspから大きさを引いただけの、8の倍数の位置に置いていた時期があった。`T_TSFLT`は16バイトのアライメントを持つので、コンパイラは`ts_flt_handle`の中で状態の`sp`と`pc`をまとめてSSEの`movdqa`で読む。rspが16の倍数でない所で起きたデータアボートでは、この読出しが一般保護例外になり、元の例外ではなく`ts_flt_handle`のアドレスでブラウザが異常終了していた(2026年11月9日、Surface Go 2でYouTubeを見ているとき)。今はカーネルが状態を16バイト境界にそろえ、レッドゾーンの下に置く(5.5.2)。元の例外はカーネルのコンソールの`fault handed to the program`の行で分かる。

テスト(14.10)のために、ブラウザの起動引数の行`v8=<フラグ>`でV8にフラグを与えられる。`v8=--jitless`では機械語を生成せず、DrumBrakeをビルドしないのでWebAssemblyは使えない。

### 14.4.2 フレームとアイドル時間

ブラウザはコンポジタを使わないので、コンポジタがフレームの始まりにメインスレッドに行わせる処理をブラウザ自身が行う。ページが新しいフレームを要求すると(`ChromeClient::ScheduleAnimation`)、ブラウザは画面のリフレッシュの周期(60Hz、16.667ミリ秒)のタイミングでフレームを始める。フレームの始めには、HTMLのイベントループのレンダリングの更新のうちライフサイクルより前の段階、つまりresizeとscrollのイベント、メディアクエリのリスナ、CSSアニメーションとWeb Animationsの更新とそのイベント、`requestAnimationFrame`のコールバックを、ウィジェットと同じ`PageAnimator::ServiceScriptedAnimations`で実行する。続いてライフサイクル(スタイル、レイアウト、描画)を進め、そのあとで`IntersectionObserver`と`ResizeObserver`に通知する。フレームを要求するフラグはフレームを始める前に消すので、コールバックの中で次のフレームを要求すれば次のフレームが来る。ページのサブリソースがそろうのを待つ間も、100ミリ秒に1回フレームを始めてライフサイクルを進め、表示だけをサブリソースがそろうまで待つ。そのため、`loading="lazy"`の画像や、スクリプトが`IntersectionObserver`で見えたことを知ってから読み込ませる画像も、待つ間に取得が始まる。

次のフレームは、ブラウザの起動から数えたリフレッシュのタイミングのうち、前のフレームを始めた時刻にそのフレームにかかった時間の2倍(リフレッシュ1回分より短ければ1回分)を足した時刻以後の最初のものにする。フレームにかかった時間は、フレームを始めてから、コールバック、ライフサイクル、描画、ウインドウへの表示を終えるまでのメインスレッドの時間である。そのため、軽いページのアニメーションと動画は毎秒60回まで描き直し、1回の表示に10ミリ秒かかるページは毎秒30回、20ミリ秒かかるページは毎秒20回ほどになって、スクリプトとネットワークの処理にスレッドの半分以上が残る。アプリケーションのループは、何も届いていないときにスリープする時間を次のフレームの時刻までに縮める(最長10ミリ秒)。画面のリフレッシュの時刻そのもの(垂直帰線の時刻)はプロセスに通知するインタフェースがないので、位相は合わせず、周期だけを合わせる。ウインドウマネージャはリフレッシュ1回につき合成を1回にまとめるので(10.4)、周期が合っていれば、表示の取りこぼしと二重の合成は起きにくい。以前は長くても100ミリ秒に1回だったので、`requestAnimationFrame`と動画は毎秒10回までしか描かれなかった。

スムーズスクロール(`scroll-behavior: smooth`、スクリプトの`behavior: 'smooth'`、キーやホイールの1段)は、Blinkのスクロールのアニメータ(`SetScrollAnimatorEnabled`)が、このフレームの始めにメインスレッドで少しずつ進める。以前はアニメータを無効にしていたので、すぐに行き先へジャンプした。

専用ワーカーの`requestAnimationFrame`は、Chromiumでは、ワーカーがviz(`EmbeddedFrameSinkProvider`が作る`CompositorFrameSink`)からフレームの始まりを受けて呼ぶ。このブラウザにはそれがないので、パッチ`begin_frame_provider.patch`で、ワーカーにフレームのソースがないときはエンベッダに頼むようにし、ブラウザが60Hzのタイミングでワーカーのスレッドにフレームを始めさせる(`worker_begin_frame`)。1回頼まれるごとに1フレームで、コールバックの中で次を要求すれば次のフレームが来る。以前は`requestAnimationFrame`が`NotSupportedError`を投げた。

以前はライフサイクルだけを進めていたので、`requestAnimationFrame`のコールバック、scrollとresizeのイベント、CSSアニメーションは一度も実行されなかった。`requestAnimationFrame`で中身を組み立てるサイト(YouTubeのカスタム要素など)は、そのため画像もアイコンも描画しなかった。

メインスレッドのスケジューラは、コンポジタからフレームの始まりと、しばらくフレームが来ないことを通知され、フレームの間のアイドル時間に`requestIdleCallback`のコールバックなどのアイドルタスクを実行する。ブラウザは起動時に一度、フレームはしばらく来ないとスケジューラに通知する(`BeginFrameNotExpectedSoon`)。スケジューラは、ほかに実行するタスクがないときに長いアイドル時間(1回が最大50ミリ秒で、次の遅延タスクの時刻までのもの)を始めて、アイドルタスクを実行する。

### 14.4.3 移植していない機能の扱い

`blink_stubs.py`のスタブ(14.3)は、属性ならnullを、操作なら0を返す。ページから呼べるインタフェースはスタブにしない。以前は、移植していないモジュールの属性(`navigator.ml`、`navigator.managed`など)をページのスクリプトより先に消すスクリプトを各ウインドウで実行していたが、消した跡が残り、ページからの見え方がChromeと違うのでやめた。それらの多くは実行時の機能で無効になっていて、Chromeと同じく初めからない。`navigator.managed`はChromeと同じく常にあり、モジュールをリンクしてある。管理されたマシンではないので、問い合わせはChromeと同じくサービスにつながらず拒否される。

EME(`requestMediaKeySystemAccess`、`setMediaKeys`)とMediaCapabilities(14.6.2、14.6.3)、14.4.4から14.4.9に挙げるものと、画面の向き(固定は`NotSupportedError`)、`navigator.connection`(有線のネットワーク、品質は固定の見積もりの`4g`)、`navigator.devicePosture`(`continuous`)、`navigator.virtualKeyboard`、`navigator.windowControlsOverlay`、`registerProtocolHandler`、`launchQueue`、`navigator.ink`、ストレージのバケットと古いFile System API(14.4.5)、FedCMのログインの状態(`navigator.login`)、CSS Paint API(`CSS.paintWorklet`。コンポジタのスレッドがないので、ワークレットはページのスレッドで描画する)、`navigator.plugins`と`mimeTypes`と`pdfViewerEnabled`、Storage Access(`document.hasStorageAccess`、`requestStorageAccess`)、メディア要素の出力先(`sinkId`、`setSinkId`。出力は既定の1つで、`""`と`"default"`のほかは`NotFoundError`。プレーヤのない要素のときにフレームのクライアントに問い合わせる箇所は、パッチ`audio_output_device.patch`で既定の出力を返す)、`DeviceOrientationEvent`と`DeviceMotionEvent`(センサーがないのでイベントは起きない)、`MediaStreamTrackProcessor`と`MediaStreamTrackGenerator`は、モジュールをコンパイルして本来の実装で応答する。除外する一覧は、機能を実装するたびに減らす(14.9)。

### 14.4.4 ワーカー、Blob、iframe

Chromiumでは、Blobの中身とその公開URLはブラウザプロセスが持ち(`BlobRegistry`、`BlobURLStore`)、専用ワーカーのスクリプトの取得と起動もブラウザプロセスのホストが行う。このブラウザでは、それらを`port/blink/blob_tessronos.cc`と`worker_tessronos.cc`がプロセスの中に持つ。

- Blobのバックエンドは、Blobの中身(バイト列、ほかのBlobの範囲、ストリームから作ったもの)を専用のスレッド(Blobのスレッド)で持ち、読み出し、範囲の読み出し、データパイプへの書き出しに応答する。`URL.createObjectURL`のURLは`BlobURLStore`に登録し、`fetch`、`XMLHttpRequest`、ワーカーの起動が、そのURLを`blob:`の`URLLoaderFactory`(`Range`の要求も扱う)か、トークン(`BlobURLToken`)に解決する。トークンが生きている間は、`revokeObjectURL`の後でも解決できる。ウインドウの`BlobURLStore`はフレームのassociated interfaceとしてページのスレッドで受け、来た順にBlobのスレッドへ渡す。
- `blob:`のURLの要求と、すでにファクトリを持つ要求は、エンベッダのローダでなくそのファクトリで読む(パッチ`loader_factory_for_frame.patch`)。この経路のローダは読み込みの通知の受信側(`ResourceLoadInfoNotifierWrapper`)を必ず使うので、フレームのクライアントは通知をどこにも渡さない受信側を作って渡す。
- 専用ワーカー(`new Worker`)は、Blinkの`WebDedicatedWorkerHostFactoryClient`をブラウザが実装して起動する。スクリプトはワーカー自身がワーカーのスレッドで取得する(`WebWorkerFetchContext`)。`blob:`のURLのワーカーは、URLをトークンに解決してから起動するので、作った直後にURLを`revokeObjectURL`してよい。ワーカーにも14.4.3の置き換えのスクリプトを実行する。ワーカーのブローカは、Blobのバックエンドと14.4.5のサービスをバインドする。
- 共有ワーカー(`new SharedWorker`)は、ブラウザプロセスが持つ`SharedWorkerConnector`とワーカーのホストを`port/blink/shared_worker_tessronos.cc`がページのスレッドで持つ。アドレス、名前、ページのオリジンが同じ接続は1つのワーカーにつなぎ、最初の接続でBlinkの`WebSharedWorker`を起動する。ワーカーは自分のスレッドでスクリプトを取得し(専用ワーカーと同じ`WebWorkerFetchContext`)、接続ごとにメッセージポート付きの`connect`イベントを受ける。スクリプトが読めなかったこと、例外、接続できたことは接続したページのすべてに通知し、接続したページがすべてなくなったワーカーは終了させる。ワーカーのポリシー(リファラポリシーとCSP)のコンテナはワーカー自身が持ち、ホストには通知しない。DevToolsは使わないので、DevToolsの受信側は作らない。
- iframeは、同じプロセスの`LocalFrame`として作る(`CreateFrame`)。子フレームの読み込みはブラウザのフレームのクライアント(`BrChildFrameClient`)が取得してコミットし、`about:blank`、`srcdoc`、`data:`も扱う。子フレームは親のページの中に描画し、`postMessage`と同じオリジンの`contentDocument`が使える。

### 14.4.5 ブラウザ側のサービス

ブラウザプロセスがページに提供するMojoのサービスのうち、次のものを`port/blink/services_tessronos.cc`と`cache_storage_tessronos.cc`がBlobのスレッドで持つ。ウインドウは空のブローカ(binderを与えたもの)で、ワーカーは自分のブローカで要求する。BroadcastChannelは、ウインドウではフレームのassociated interfaceとしてページのスレッドで受ける。このブラウザは一度に1つのページしか表示しないので、ロックとチャネルはプロセスの全体で名前ごとに持つ。

| サービス | 応答 |
| --- | --- |
| Permissions(`navigator.permissions`、ほかの機能が求める許可) | ブラウザが確認せずに与えるもの(クリップボードへの書込み、自分のフレームのストレージアクセス)は許可。クリップボードの読み取りは、利用者にパネルで尋ね(14.7.3)、応答をオリジンごとに記憶する。利用者の応答は「ブラウザの利用者データ」のレコード10(ストア`perms`、14.8)に入れ、ブラウザを終了しても残る。ほかは問い合わせに`prompt`と応答し、要求には拒否で応答する |
| Notification | 許可は利用者にパネルで尋ね、応答をオリジンごとに記憶する(Permissionsと同じく残る)。通知はデスクトップの通知(11.28)で表示する(14.4.10) |
| Push(`pushManager`) | プッシュサービスに購読を登録し、届いたメッセージを復号してService Workerに送る(14.4.11) |
| ストレージの割り当て(`navigator.storage.estimate`、`persist`) | オリジンのCache Storage、IndexedDB、オリジンごとのファイルシステムの使用量と、オリジンの上限を返す。`persist()`は通知を許可されたオリジンと導入したアプリに確認せずに許可する(14.4.5.1) |
| Web Locks(`navigator.locks`) | 名前ごとのロックを共有か排他で持ち、要求された順に与える。持ち主がハンドルを閉じると次に渡す。`steal`は持っているロックを奪う |
| BroadcastChannel | 同じ名前のほかの接続のすべてに、それぞれのスレッドで渡す |
| クリップボード(`ClipboardHost`。ページの中の複写と貼り込み、`execCommand`、`navigator.clipboard`が使う) | システムのトレー(18.16)を使う(`port/net/br_tray.cc`)。書き込みが確定すると、文字列を1行1段落のxmlTADの断片として、画像をPNGとして、1つの組でトレーに入れる。読むときは、手にある組の最初のxmlTADの記録の文字(タグを除き、段落の終わりと改行を改行に、実体参照を文字にしたもの)と最初の画像を返す。ほかのアプリが入れた組も読める |
| Cache Storage(`caches`) | オリジンごとに、名前の付いたキャッシュを作った順に持つ。キャッシュは要求(アドレス、メソッド、ヘッダ)と応答の組の並びで、応答の本文はモジュールが作ったBlobのまま持つ。照合は仕様のとおり、フラグメントを除いたアドレス(`ignoreSearch`ならクエリも除く)、`ignoreMethod`でなければGETだけ、`ignoreVary`でなければ応答の`Vary`が挙げる要求ヘッダを比べる。`put`は同じ要求の項目を置き換える。返す応答は本文を共有するコピーである。キャッシュはブラウザを終了しても残る。`put`された応答の本文はそのBlobから読み取って持ち、キャッシュが変わってから2秒後に、前の保存から作られたキャッシュと項目、消えたキャッシュと項目だけを「ブラウザの利用者データ」のレコード9(ストア`caches`)に書く(エントリ単位の保存、14.8.1)。起動時に読み戻し、本文をBlobに作り直す。使用量はオリジンのクォータで数え、オリジンの上限を超える`put`は`QuotaExceededError`で拒否し、全体のプールを超える`put`はほかのオリジンを追い出してから行う(14.4.5.1)。項目は本文を読み終えてから本文ごと書き、本文を読めなかった項目は書かない。本文を残さずに書いた以前のバージョンの項目(本文のフラグが0)は、読み戻すときに除き、次の保存で消す。フレームのブローカが空のブローカのときにモジュールが拒否するのは、パッチ`global_cache_storage.patch`で外す |

| `cookieStore` | ネットワークスタックのCookieストアを使う(`port/blink/cookie_store_tessronos.cc`)。`get`と`getAll`はページのアドレスのCookieのうちスクリプトが見られるもの(HttpOnlyでないもの)を返し、`set`と`delete`はスクリプトが与えたものから作ったCookie(削除は期限の切れたもの)を入れる。`change`イベントはCookieストアの変更の通知から作るので、応答、`document.cookie`、`cookieStore`のどれで変わっても届く。HttpOnlyのCookieは見せず、通知しない。フレームのブローカが空のブローカのときにモジュールが`cookieStore`をnullにするのは、パッチ`global_cookie_store.patch`で外す |
| Payment Request(`new PaymentRequest`) | システムに支払いのアプリも手段もないので、`show()`は`NotSupportedError`、`canMakePayment()`と`hasEnrolledInstrument()`はfalseを返す。BlinkではPayment Requestは試験中の扱いなので、ブラウザが起動時に有効にする(`WebRuntimeFeatures::EnablePaymentRequest`) |

Protected Audience(`joinAdInterestGroup`、`runAdAuction`など)、Topics(`document.browsingTopics`)、Shared Storage(`sharedStorage`)は、Chromium 153自身がそれぞれ「何もしない」、「取り除いた」(`NotSupportedError`)、「無効」(`OperationError`)と応答する実装になっているので、そのモジュールをそのまま入れて同じに応答する。

ストレージのバケット(`navigator.storageBuckets`、`port/blink/buckets_tessronos.cc`)は、オリジンの中の名前の付いた保存場所である。バケットのIndexedDBとCache Storageは、オリジンのキーの後ろにバケットの名前を付けたものとして持つので、既定のバケットやほかのバケットのものと分かれ、データベースはほかのものと同じくブラウザを終了しても残る。ロックはオリジンのものを使う。バケットのディレクトリ(`getDirectory`)は、バケットのキーで分けたオリジンごとのファイルシステム(下)である。`persisted`と期限はブラウザが動く間だけ記憶する。バケットの使用量はオリジンのクォータに含め(14.4.5.1)、バケットの`estimate()`はそのバケットの使用量とオリジンの上限を返す。バケットを消すと、そのデータベースとキャッシュも消える。Chromium 153のバケットには`durability()`がない。

古いFile System API(`webkitRequestFileSystem`、`webkitResolveLocalFileSystemURL`、`port/blink/filesystem_tessronos.cc`)は、オリジンごとの一時(`TEMPORARY`)と永続(`PERSISTENT`)のファイルシステムを、ブラウザが動く間メモリの中に持つディレクトリとファイルのツリーとして扱う。アドレスは`filesystem:<オリジン>/temporary/a/b`の形である。ディレクトリの作成、一覧、移動、複写、削除、ファイルへのBlobの書込みと切り詰め、ファイルを`File`として読むこと(スナップショットは大きさと時刻だけを返し、中身はバイト列のBlobにする)ができる。サービスは専用のスレッドで動く。ページがファイルのBlobを同期呼出しで要求し、それをBlobのバックエンドのスレッドに登録するためである。どちらの種類も、ブラウザを終了すると中身は消える。

オリジンごとのファイルシステム(Origin Private File System、`navigator.storage.getDirectory()`、`port/blink/opfs_tessronos.cc`)は、オリジンごとのディレクトリとファイルのツリーをプロセスの中に持つ。ディレクトリは名前ごとの項目を、ファイルはバイト列と書いた時刻を持つ。ページが持つハンドルはツリーのノードへのパイプで、ツリーから外されたノードは`NotFoundError`を返す。

- `getFileHandle`と`getDirectoryHandle`は項目を探し、`create`なら作る。`entries()`、`removeEntry`、`remove`、`move`(名前、別のディレクトリ、その両方)、`resolve`がツリーを扱う。
- `createWritable`のストリームに書いたものは、閉じたときにファイルになる。
- 専用ワーカーの`createSyncAccessHandle`には、Chromeがシークレットウインドウで使うファイルの代理(`FileSystemAccessFileDelegateHost`の`Read`、`Write`、`GetLength`、`SetLength`)で応答し、その呼出しがファイルのバイト列を直接扱う。ハンドルを閉じるまでファイルをロックし、その間は書き込みのストリームを作らせない(逆も同じ)。
- ほかのコンテキストに送ったハンドル(`postMessage`)は、ノードを指す転送のトークンとして届く。
- ツリーは、最後に変わってから2秒後に全体を「ブラウザの利用者データ」のレコード11(ストア`opfs`)に書き、起動時に読み戻す。

`localStorage`と`sessionStorage`の変更は、同じページの同じオリジンのほかのウインドウ(iframe)に`storage`イベントで通知する(`port/blink/storage_tessronos.cc`)。変更したウインドウ自身には通知しない。

#### 14.4.5.1 保存域のクォータと追い出し

オリジンのCache Storage、IndexedDB、オリジンごとのファイルシステムは、バケットのものも含めて1つのクォータで数える(`port/blink/cache_storage_tessronos.cc`)。Chromeのクォータの管理と同じく、ブラウザ全体のプールとオリジンごとの上限を持ち、オリジンの上限を超える`put`は拒否し、プールを超えそうになったら、使われていないオリジンをまるごと追い出して空きを作る。

大きさは、ストアの実身があるボリュームの容量と空き(`ob_ref_vol`)と、マシンのメモリから決める。

| 値 | 決め方 | Chromeの値 |
| --- | --- | --- |
| プール | ボリュームの80%と、メモリの1/16の小さい方 | ディスクの80% |
| オリジンの上限 | プールの75%。ただし、追い出せないほかのオリジン(永続化したもの、開いているもの)が持つ分をプールから引いた残りを超えない | プールの75%(ディスクの60%) |
| 必ず残す空き | 1GBとボリュームの1%の小さい方。`put`の後に空きがこれを下回るなら拒否する | 同じ |
| 残したい空き | 2GBとボリュームの10%の小さい方。空きがこれを下回るなら、プールを超えたときと同じく追い出す | 同じ |

割合は、ChromeのQuotaSettings(`CalculateNominalDynamicSettings`)の既定の値をそのまま使う。全体を80%、オリジンをその75%とするのは、1つのサイトがプールを使い切らず、ほかのサイトの分が残るようにするためである。2つの空きの値は、ボリュームが大きいときに10%や1%では残しすぎになるので、固定の値で上限を抑えている。

メモリの1/16を加えたのは、このブラウザがCache Storage、IndexedDB、ファイルシステムの中身をプロセスのメモリにも持つためである。Cache Storageの本文はBlobとストアに書くコピーの2つをメモリに持ち、レンダラはそれぞれ起動時にすべてのオリジンのものを読み戻す。そのため、ボリュームが大きくてもプールはメモリの量で抑える。Raspberry Pi 5の4GBではおよそ256MB(オリジンは192MB)、8GBでは512MB(384MB)、x64の16GBでは1GBになる。2GBのシステムボリュームなら80%の1.6GBなので、メモリで決まる。

Chromeの`estimate()`が返す`quota`はオリジンの上限の固定の値で、空きの量では変わらない。ここでも同じく空きの量では変えず、追い出せないほかのオリジンの分だけを引く。

使用量は、Cache Storageでは項目の本文とアドレスとヘッダの合計、IndexedDBではレコードのキーと値とインデックスの項目に、値が指すBlobとFileの大きさを足したもの、ファイルシステムではストアに書く形式での大きさである。localStorageは、Chromeと同じく数えず、追い出さない(オリジンごとに5MBの上限を別に持つ)。Service Workerの登録も数えず、追い出さない。Chromeは登録も追い出すが、ここでは登録のストアが別にあって小さく、追い出すと動いているワーカーを止めることになるからである。

規則は次のとおりである。

- `put`(`Cache.put`、`add`、`addAll`の1回の呼出し)は、置き換える項目の分を引いた増える量で判断する。オリジンの使用量にそれを足すとオリジンの上限を超えるときは、呼出しの全体を`QuotaExceededError`で拒否する。
- 全体がプールを超えるとき、または空きが残したい空きを下回るときは、ほかのオリジンを最後に使った時刻の古い順に1つずつ、オリジン単位でまるごと(Cache Storage、IndexedDB、ファイルシステムと、そのバケットのもの)追い出し、足りたところで`put`を行う。追い出さないのは、`put`したオリジン、永続化したオリジン、このプロセスで開いているオリジン(表示しているページのもの、IndexedDBの接続があるもの)である。
- 追い出せるものがなくなってもプールを超えるとき、または空きが必ず残す空きを下回るときは拒否する。オリジンの上限は追い出せないほかのオリジンの分を引いてあるので、前者は永続化したオリジンや開いているオリジンでプールが埋まっているときにだけ起きる。
- 追い出すたびに、コンソールに`browser: storage: evicted <オリジン> (<KB> KB) for room`の1行を出力する。出力するのはオリジンだけで、パスやクエリは出力しない。

最後に使った時刻は、Cache Storageを開いたとき、照合したとき、`put`したとき、IndexedDBのファクトリを作ったとき、ファイルシステムのルートを要求したときに、マイクロ秒で更新する。この時刻と、永続化したかどうかと、使用量を、ストア`sites`(14.8)のオリジンの行に書く。行の先頭の8バイトは消去(14.7.10)が使う1時間単位の時刻で、その後ろに、最後に使った時刻(8バイト)、使用量(8バイト)、フラグ(4バイト、1が永続化)を置く。書くのは、使用量かフラグが変わったときと、時刻が前に書いたものから30秒以上進んだときである。起動し直しても、時刻、永続化のフラグ、使用量が残る。

レンダラは別々のプロセスなので、ほかのレンダラが変えたオリジンの使用量は`sites`の行で知る(自分が変えたオリジンは自分の数えた値を使う)。追い出すオリジンを選ぶ前には`sites`を読み直す。追い出したプロセスは、自分が読み戻していない分も含めてそのオリジンの項目をストアから消し、`sites`に通知(消去の通知と同じ形式で、`evict`にオリジンを入れたもの)を書く。ほかのブラウザとレンダラは3秒ごとのポーリングでそれを見つけ、自分が持つそのオリジンのものを消す。そのオリジンを開いているプロセスは消さず、Cache Storageの項目をストアに書き直す。

ボリュームの容量と空きは、1つのプロセスで動くブラウザでは自分で`ob_ref_vol`で問い合わせる。レンダラはストアの実身を開かないので、ストアの実身を持つネットワークのプロセスに、ストアのパイプのメソッド`Store.Volume`(`mojom/br_process.mojom`)で問い合わせ、ネットワークのプロセスが同じ関数`br_store_volume`(`port/net/br_store.cc`)で応答する。どちらも、問い合わせた後に増えた使用量を空きから引き、60秒ごとに問い合わせ直す。ストアを読み戻すと、問い合わせた後に増えたように見えるので、起動時の行を出力した後の最初の使用で問い合わせ直す。問い合わせられない間(ストアのパイプがまだない、利用者の実身がない)は、プールをメモリだけで決め、空きを見ず、使うたびに問い合わせ直す。

エンジンが起動するときに、プロセスごとにコンソールに`browser: storage: a pool of <KB> KB, <KB> KB an origin (volume <容量> KB, <空き> KB free; memory <MB> MB)`を出力する。そのときにボリュームが分からなければ`(volume not known yet; memory <MB> MB)`と出力し、分かった時点で`browser: storage: the volume now known: a pool of ...`をもう一度出力する。

以前は、ブラウザがレンダラを起動するときに起動引数の行`storagevol=<容量>,<空き>`で渡していた。その行を作る関数の宣言が`br_proc.cc`の無名の名前空間の中にあったため、リンクで定義と結び付かずに`blink_stubs.py`のスタブ(14.3)になり、レンダラは容量を知らないままだった(2026年11月、Raspberry Pi 5で`volume 0 KB`と出て、ディスクの空きを見ていなかった)。テストはどれもテスト用のプール(`quotapool=`)を使っていて、容量を見ていなかった。今は`test_blink_csvolume`が、レンダラの起動時の行に容量が出ることを確認する。

Cache Storageの項目とキャッシュの番号は、マイクロ秒の時刻を4ビット左シフトし、下位4ビットにプロセスの番号の下位4ビットを入れたものである(前に与えた番号より必ず大きくする)。以前は1からの通し番号で、同時に動くレンダラがそれぞれ起動時に読んだ最大の番号から数えたため、別々のレンダラが同じ番号の項目を書いて上書きし合うことがあった。

`navigator.storage.persist()`は確認せずに決める。オリジンが通知を許可されている、または隔離されたWebアプリとして許可されている(導入したアプリにあたる)なら、許可して永続化し、そのほかは拒否する。Chromeのデスクトップ版は、これに加えてブックマークとサイトのエンゲージメントの高さでも許可するが、ここにはその2つがない。`persisted()`と`permissions.query({name: 'persistent-storage'})`は永続化したかどうかを返す。以前はどちらも、拒否と`prompt`だった。永続化したオリジンは追い出さないので、その使用量はほかのオリジンの上限から引かれる。

`navigator.storage.estimate()`は、`usage`にオリジンの使用量(バケットのものを含む)を、`quota`にオリジンの上限を、`usageDetails`に`caches`、`indexedDB`、`fileSystem`ごとの量を返す。

テスト用の起動引数の行`quotapool=<KB>`は、プールをその大きさにし、ボリュームの空きを見ない。また起動時に、各オリジンの使用量、最後に使ってからの秒数、永続化したかを`browser: storage: <オリジン> <KB> KB, used <秒> s ago, persisted`の行で出力する。`persist=<オリジン>`は、そのオリジンを導入したアプリとして扱い、`persist()`を許可する。

以前は、すべてのオリジンのCache Storageの本文の合計を64MBに固定していた。64MBを一度埋めると、どのサイトも`put`できないままになった。さらにその前は、64MBを超えた項目を本文なしで書いていたので、次の起動でその項目が本文のない応答になり、Service Workerがそれをページのスクリプトやスタイルシートの応答にしていた(Huluのページの表示がおかしくなった原因)。

### 14.4.6 Service Worker

Chromiumでは、Service Workerの登録とその保存、ワーカーの起動、どのページを制御するか、ページの取得をワーカーに回すことは、ブラウザプロセスとネットワークサービスが受け持つ。このブラウザでは、それらを`port/blink/service_worker_tessronos.cc`がページのスレッドで持つ。

- `navigator.serviceWorker.register()`は、スコープごとの登録をブラウザが動く間持ち、ワーカー(Blinkの`WebEmbeddedWorker`)をこのプロセスの専用のスレッドで起動する。スクリプトはワーカー自身が取得する。スクリプトが動くと`installing`にして`install`イベントを送り、完了すれば待機中にし、アクティブなワーカーがなければ(または`skipWaiting()`なら)`activate`イベントを送ってアクティブなワーカーにする。`ready`、`getRegistration`、`getRegistrations`、`unregister`に応答する。
- フレームに文書ができるたびに(メインフレームでもiframeでも)、その文書をワーカーのクライアントとして、UUIDを付けて記憶する。フレームのクライアント(`BrFrameClientBase`)がそのUUIDを持ち、次の文書ができたときとフレームが外れたときに破棄する。文書のアドレスを含むスコープにアクティブなワーカーがあれば、その文書はワーカーに制御される。iframeの`about:blank`と`about:srcdoc`の文書は、親の文書を制御するワーカーに制御される。`clients.claim()`は、スコープの中のすべての文書を制御する。新しいワーカーがアクティブになると、前のワーカーが制御していた文書はそのワーカーのものになる(`controllerchange`)。
- 制御された文書の取得(`br_loader.cc`)は、フレームのローダが持つ文書のUUIDで、まずその文書を制御するワーカーに`fetch`イベントとして送り(`clientId`はその文書。ワーカーの`ControllerServiceWorker`を複製した受信側に送る)、`respondWith()`の応答(Blobかストリーム)を文書に渡す。応答しなければネットワークから取得する。
- スコープの中のアドレスへ移るとき、メインフレーム(`br_main.cc`の`br_nav_fetch_load`)では、まずワーカーに`fetch`イベントとして問い合わせ、30秒まで待つ。応答があればそれをページとして表示し、応答のヘッダも文書に渡す。iframeのGETの移動(`BrChildFrameClient`)も、ネストした文書の移動(`destination`が`iframe`、`<frame>`なら`frame`。`frameType`が`nested`)の`fetch`イベントとして問い合わせ、その`resultingClientId`をできる文書のUUIDにする。待たずに先へ進み、応答が来たらそれを文書にし、応答しなければネットワークから取得する。
- 移動の要求のredirect modeは`manual`である。そのため、ワーカーが`fetch(event.request)`で応答すると、サーバのリダイレクトは`opaqueredirect`の応答になる。この応答は、隠しているリダイレクトのステータスと`Location`をそのまま持つ。ワーカーが`respondWith`で返したリダイレクト(`opaqueredirect`、または`Response.redirect()`などの301、302、303、307、308で`Location`のあるもの)は、文書として表示せず、移動がそれに従う。行き先は、もとのアドレスを基準に`Location`を解決して決め、行き先にフラグメントがなければもとのフラグメントを付ける。行き先のアドレスは、そのスコープのワーカーにあらためて`fetch`イベントで問い合わせ、ワーカーがなければ、またはワーカーが応答しなければネットワークから取得する(`RedirectOf`、`br_sw_navigate`、iframeでは`FrameStep`)。ワーカーが応答したリダイレクトは20回までで、超えたときは`ERR_TOO_MANY_REDIRECTS`、`Location`がアドレスにならないときは`ERR_INVALID_REDIRECT`で移動が失敗する(メインフレームはエラーのページを表示し、iframeには何もコミットしない)。2026年11月20日まではこのリダイレクトの応答をそのままページとして表示していたので、Raspberry Pi 5でserwistのService Workerを登録したサイトのログインのページを開くと、本文0バイトの302が白いページになっていた。
- ページの`fetch`などのサブリソースの要求(redirect modeは`follow`)をワーカーが`fetch(event.request)`で取得するときは、ワーカーの取得がネットワークのリダイレクトに従い、最後の応答をページに渡す。これは以前と同じである。ワーカーがサブリソースの要求にリダイレクトの応答で応答したときにそれに従うことは、まだ扱っていない。
- 文書とワーカーの間の`postMessage`、ワーカーの`clients.matchAll()`(制御する文書。`includeUncontrolled`なら同じオリジンのすべての文書。作られた順)、`clients.get()`、`client.postMessage()`、`focus()`に応答する。クライアントの`frameType`は、メインフレームの文書が`top-level`、iframeの文書が`nested`である。ワーカーへのメッセージの送信元は、そのワーカーのオブジェクトを渡した文書である。文書やワーカーに渡す登録とワーカーのオブジェクト(本来はブラウザとのassociated interface)は、それぞれ専用のパイプにする。
- アクティブなワーカーのある登録は、スコープ、登録の番号、ワーカーのスクリプトのアドレスを「ブラウザの利用者データ」のレコード7(ストア`sw`)に入れ(変わってから0.5秒後)、次の起動の初めに読み戻す。読み戻した登録のワーカーは、最初に必要になったとき(そのスコープのページを表示するとき、ワーカーへのイベントやメッセージが来たとき)に起動し、スクリプトを取得し直して動かす。`install`と`activate`のイベントは送らない。`unregister`で保存からも消える。
- 通知のクリックと閉じたこと(14.4.10)、プッシュ(14.4.11)のイベントはワーカーに送る。ナビゲーションのプリロードは扱わない。
- 起動したワーカーは、ブラウザが動く間停止しない。ワーカーは、イベントのない時間がアイドルの遅延(30秒)を過ぎるとホストに停止を要求し(`RequestTermination`)、応答が来るまで、その後に来たイベントを始めずにキューに置く。ホストはページのスレッドで停止しないと応答し、ワーカーのアイドルの遅延を既定の30秒に戻す(イベントが5分でタイムアウトすると、ワーカーは遅延を0にし、キューが空くたびにすぐまた要求するため)。応答しないと、キューに置かれたイベントは、メインフレームの移動のイベントが来るか、それぞれが5分でタイムアウトするまで始まらない(14.10の2026年10月28日の記録)。`test_blink_swidle`が、40秒何もしなかった後のページの取得にワーカーがすぐ応答することを確認する。

ワーカーがバックグラウンドで行う処理は、`port/blink/background_tessronos.cc`がサービスのスレッドで受け持つ。

- 一度だけの同期(`registration.sync.register`)は、ページのpromiseが解決したときに、ワーカーに`sync`イベントとして送る。マシンは常にネットワークにつながっているとみなす。イベントが失敗すると5分後、さらに15分後に送り直し、3回目を最後の機会(`lastChance`)とする。
- 定期的な同期(`registration.periodicSync.register`)は、登録してから、または最後に送ってから間隔が過ぎるたびに、ワーカーに`periodicsync`イベントとして送る。間隔はページが求めた`minInterval`と12時間の長いほうである。テストでは起動引数の行`periodicsync=`で12時間を短くする。
- Content Index(`registration.index`)は、サイトがオフラインで読めるとする記事などの説明を登録ごとに持つ。ChromeではAndroidだけの機能なので、Chromeと同じく既定では出さず、起動引数の行`features=ContentIndex`で出す。説明を表示する場所はないので、アイコンは要求せず(大きさの一覧を空で返す)、`contentdelete`イベントは送らない。
- バックグラウンドフェッチ(`registration.backgroundFetch.fetch`、`port/blink/background_fetch_tessronos.cc`)は、要求されたものを専用のスレッドで1つずつ取得し(`br_fetch`。Cookieを送る)、状態、アドレス、種類、本文を取得とともに持つ。見ているページとワーカーには進み具合と、1つ完了するごとにその要求と応答を伝える。最後のものが完了すると、すべての応答が2xxならワーカーに`backgroundfetchsuccess`、そうでなければ`backgroundfetchfail`を送る。ワーカーは、そのイベントが終わるまで、登録の`matchAll`で応答をBlobとして読める。`abort()`は`backgroundfetchabort`を送る。取得が完了したことはデスクトップの通知で伝え、通知の〈開く〉はワーカーへの`backgroundfetchclick`になる。アイコンを表示する場所はないので、アイコンは要求しない。取得はブラウザが動く間のもので、終了したときに完了していなかったものは次の起動で続けない。
- 許可の問い合わせには、`background-sync`、`periodic-background-sync`、`background-fetch`を許可と応答する。
- 待っている一度だけの同期、定期的な同期、Content Indexの説明は、Service Workerの登録の番号ごとに「ブラウザの利用者データ」のレコード8(ストア`background`)に入れ、次の起動で読み戻す。待っていた一度だけの同期は起動の3秒後に送る。登録を`unregister`すると、その登録のものも消える。

### 14.4.7 資格情報

`navigator.credentials`(`port/blink/credentials_tessronos.cc`)は次のように応答する。

| 種類 | 応答 |
| --- | --- |
| パスキー(WebAuthn) | ブラウザ自身がソフトウェアの認証器(プラットフォーム認証器)を持つ。`create()`は、利用者にパネル5で尋ねてから、依拠するサイト(RP ID)ごとにP-256の鍵を作り、`fmt`が`none`のアテステーションと認証器のデータ(RP IDのハッシュ、フラグUP・UV・AT、カウンタ、AAGUID、資格情報のID、COSEの公開鍵)をCBORで作って返す。`get()`は、許可された資格情報か、なければそのサイトの資格情報(複数ならパネル4で選ぶ)で、利用者に尋ねてから、認証器のデータとクライアントのデータのハッシュにECDSA(SHA-256)で署名して返す。鍵は「ブラウザの保存域」のレコード9に、パスワードとともに暗号化して書き(PKCS#8の秘密鍵、ID、RP ID、利用者のIDと名前、カウンタ。14.4.7.1)、次に起動したブラウザでも使える。RP IDはページのホストか、それを含むドメインでなければならない |
| FedCM(`identity`) | IDプロバイダの設定(`configURL`)を取得し、その`accounts_endpoint`から利用者のアカウントを(IDプロバイダのCookieとともに)取得する。アカウントをパネル4で選ばせ、`id_assertion_endpoint`に`account_id`、`client_id`、`nonce`を送ってトークンを受け取り、ページに渡す |
| パスワード(`PasswordCredential`) | `store()`は、利用者にパネル5で尋ね、許可されればページのオリジンの資格情報として保存する。`get({password: true})`は、そのオリジンの保存したものを、パネル(1つなら確認、複数ならパネル4の一覧で、最後に保存したものを選んだ状態から)で選ばせてから返す。`mediation: 'silent'`では、1つだけあってページが`preventSilentAccess()`を呼んでいないときだけ確認せずに返す |
| ログインのフォーム | Chromeのパスワードマネージャと同じく、パスワードの欄に値のあるフォームを送信するとき(`submit`のイベントの前、`DispatchWillSendSubmitEvent`)、その利用者名(パスワードの欄の前の文字・メール・電話の欄)とパスワードを保存するかをパネル5で尋ねる。保存したパスワードのあるオリジンのログインのフォームの欄にフォーカスが移ると(`FocusedElementChanged`、文書ごとに1度)、入力するかをパネルで尋ね、選んだものを欄に入れる(`input`と`change`のイベントが出る) |
| フェデレーテッドの資格情報 | 保存しない |
| WebOTP | SMSを受信する手段がないと応答する |

#### 14.4.7.1 パスワードとパスキーの保存

パスワードとパスキーは、平文では実身に置かない。ChromeがLinuxで暗号鍵をOSの鍵の保管(libsecret、KWallet)に預け、保存するパスワードをAESで暗号化するのと同じ考えで、鍵を利用者ごとの実身に置き、レコード9をAES-256-GCMで暗号化する(`port/net/br_store.cc`の「sealed stores」)。

鍵は、TessronOSの資格情報と実身の保護(8.6、8.7)に預ける。ブラウザは、自分が代理するユーザ(`ob_get_crd`)ごとに、初めて保存するときに実身「ブラウザの鍵」を「ブラウザの保存域」と同じボリュームに作る。所有者はそのユーザ、モードは`rw-------`、ACLはなく、レコード1に乱数(BoringSSLの`RAND_bytes`)の32バイトの鍵を持つ。どの仮身からも指されないので、固定(`ob_lnk_obj`)を1つ付けて屑実身にならないようにする。ログインのパスワードから鍵を導出する方法は採らない。ブラウザはログインのパスワードを受け取らず(8.7.2のとおりデスクトップの中だけで検証する)、ログインしない利用者はパスワードを持たないからである。

レコード9は、12バイトのヘッダ(14.8)の後に`BRK1`、部分の数、ユーザごとの部分を並べる。部分は、ユーザのUUID、鍵の実身のUUID、12バイトのナンス、暗号文と16バイトのタグである。タグは`BRK1`、ユーザ、鍵の実身も対象にするので、部分を別のユーザの位置に移すと開けない。ブラウザは自分のユーザの部分だけを開き、書くときは自分の部分だけを新しいナンスで作り直し、ほかのユーザの部分はそのまま残す。鍵を読むときは、鍵の実身の所有者がそのユーザで、グループとほかの人に何も許さず、ACLがないことを確認し、満たさない鍵は使わない。書くときは、前の中身をその場所で0で上書きしてから新しい中身を書く。

この形式より前のブラウザが平文で書いたレコード9は、起動したときに読み込み、その場で暗号化して書き直す。前の中身はその場所で0で上書きし、TSFSのジャーナルに残る前のコピーも`ob_wip_vol`で0で上書きする(7.3)。

守れるものと守れないものは次のとおりである。

| 場面 | 守れるか | 理由 |
| --- | --- | --- |
| 同じマシンのほかのユーザ、ほかのユーザとして動くプログラムが「ブラウザの保存域」を読む | 守れる | 「ブラウザの保存域」は誰でも読めるが、中身は暗号文で、鍵の実身はそのユーザだけが開ける |
| 部分をほかのユーザの部分と入れ替える、書き換える | 守れる | タグが合わず開けない。開けない部分は読まなかったものとして扱う |
| 管理者(管理者のグループの資格で動くもの) | 守れない | 管理者はすべての実身を開ける。LinuxのChromeでもrootは鍵の保管を読める |
| 同じユーザとして動くほかのプログラム | 守れない | 鍵の実身を開ける。Chromeでも、同じユーザのプログラムはlibsecretから鍵を取り出せる |
| ログインしない利用者(`利用者`)どうし | 守れない | マシンの前でログインしない人は、みな同じユーザである |
| ディスクを外して別のマシンで読む、ボリュームのイメージを読む | 守れない | 鍵と暗号文が同じディスクにあり、鍵をラップするログインのパスワードやハードウェアの鍵がない。ChromeがLinuxで鍵の保管を使えないときの`basic`と同じ程度である |
| 動いているブラウザのメモリ | 守れない | 開いたパスワードと鍵はプロセスのメモリにある。使い終えたバッファは`OPENSSL_cleanse`で消去する |
| 平文の形式からの移行の前に書かれたブロック | 守れる(このボリュームの中では) | 移行では、同じ場所とジャーナルを0で上書きする。前の形式のブラウザが書いたものを別のボリュームに複製したり、バックアップに取ったりしていれば、そこには残る |

住所とカード(レコード12)も同じ形式と同じ鍵で暗号化する。

パスワードとパスキーは、ページを動かすプロセスのメモリに置かない。読み書きは操作ごとに行う(`port/net/br_cred_core.cc`)。「このオリジンのパスワード」「このRP IDのパスキーのIDと利用者名」「パスキーを作る」「このパスキーで署名する」「このオリジンのパスワードを保存する」の操作ごとに、レコード9を開いて読み、変えたときはその場で書き戻す。メモリに残すものはない。ほかのプロセス(別に起動したブラウザ、〈設定〉)が変えた内容も、次の操作で読む。そのため、以前のプロセス間の通知(`BR_RELOAD_CREDS`)は不要になった。パスキーの秘密鍵は署名する操作の中でだけ使い、操作の外へは出さない。マルチプロセスでは、レンダラはネットワークのプロセスに操作を依頼する(14.13.8)。〈設定〉の一覧、変更、削除は、ブラウザプロセスが同じ操作で直接行う。1つのプロセスのブラウザは、自分のプロセスで行う。

住所とカードは、ブラウザプロセスが読む。フォームの欄にフォーカスが移ると、レンダラはブラウザに`Host.AutofillChoose`を送る。ブラウザが実身を読み、パネル4で選ばせ、選んだ1件の欄だけをレンダラに返す。乗っ取られたレンダラでも、利用者がパネルで選んでいない住所やカードは受け取れない。

鍵の実身を消すと、そのユーザの保存したパスワードとパスキーは開けなくなる。次に保存するときに新しい鍵を作り、そのユーザの部分を作り直す。

### 14.4.8 機器のないもの

機器に関わるWebのインタフェースは、Blinkのモジュールをそのまま使い、ブラウザプロセスの側は`port/blink/devices_tessronos.cc`がこのマシンにあるものを返す。

| インタフェース | 応答 |
| --- | --- |
| 電池(`navigator.getBattery()`) | 電池がないので、電源につながったマシンと同じく、充電中で満充電と応答する |
| 画面の消灯の防止(`navigator.wakeLock`) | 許可する。システムはページを表示している間、画面を消さない |
| センサー(`Accelerometer`など) | センサーがないと応答する(`NotReadableError`)。センサーの読み取り値の共有バッファは作らない。種類ごとの最大の頻度はChromiumの表(`sensor_traits.cc`)による |
| アイドルの検出(`IdleDetector`) | 利用者は操作中で、画面はロックされていないと応答する |
| 共有(`navigator.share()`) | タイトル、文、アドレスを1組にしてトレーに入れる(コピーと同じ)。ChromeはLinuxでは出さないので、既定では出さず、起動引数の行`features=WebShare`で出す |
| キーボード(`navigator.keyboard`) | キー配列の表は、システムがキーコードを読むUS配列。キーのロックは許可する |
| MIDI(`requestMIDIAccess`) | マシンのUSBのMIDIの機器(オーディオクラスのMIDIストリーミングのサブクラス)を接続する(`port/blink/midi_tessronos.cc`)。システムのオーディオのドライバはオーディオストリーミングのインタフェースしか取らないので、MIDIの機器はプログラムの機器(6.3.1)として残り、ブラウザが開いてUSB MIDIをバルクのエンドポイントでやり取りする。エンドポイントのクラス固有の記述子が挙げる埋め込みのジャックがケーブル1本ずつで、OUTのエンドポイントのものがページの出力、INのものがページの入力になる。名前は機器の名前(ケーブルが複数ならその番号を付ける)。入ってくる4バイトのイベントのパケットは、コードインデックスが示すバイト数を取り出し、ケーブルごとに完全なメッセージにまとめて(`midi::MidiMessageQueue`)ページに渡す。ページが送るメッセージはイベントのパケットに分け(システムエクスクルーシブは3バイトずつ)、機器ごとのスレッドで書き、送れたバイト数をページに通知する。機器はセッションを始めるときに探す。許可は14.4.5の許可の扱いによる |
| アプリのバッジ(`navigator.setAppBadge`) | ブラウザのウインドウのタイトルの前に括弧で出す(`(3) 題`、数のないバッジは`(●)`)。関連アプリはないと応答する |
| プレゼンテーション(`PresentationRequest`) | 2つめの画面はないと応答する |
| Bluetooth(`navigator.bluetooth`) | アダプタがないと応答する(`getAvailability()`はfalse) |
| HIDとUSBの機器(`navigator.hid`、`navigator.usb`) | WebUSBは、システムのクラスドライバが取らなかった機器(6.3.1)を扱う(`port/blink/usb_hid_tessronos.cc`、`port/net/br_usb.cc`)。`requestDevice()`はフィルタに合う機器をパネル(`br_choose`)に並べ、選んだ機器をそのオリジンのものにする(ブラウザが動く間)。機器の情報(`UsbDeviceInfo`)は、機器、構成、文字列の記述子から作る。開く、構成を選ぶ(システムが設定した構成だけ)、インタフェースを取る、代替設定、ストールの解除、リセット、コントロール転送、バルク転送とインタラプト転送、アイソクロナス転送(1サービス間隔に1パケット)ができる。ブロックする呼出しはそれぞれのスレッドで行い、サービスのスレッドで応答する。WebHIDは、同じ機器のうちHIDのインタフェースを扱う(キーボードとポインタはシステムのHIDのドライバのもので、一覧に出ない)。レポート記述子はインタフェースへの`GET_DESCRIPTOR`で読み、Chromiumのパーサ(`services/device/public/cpp/hid`)でコレクションとレポートの大きさにし、mojomの通常の型からBlinkの型へ、シリアライズしたバイト列で移す。開くと、インタラプトのINのエンドポイントを専用のスレッドで読み続け、届いたレポートをページに渡す。出力レポートはOUTのエンドポイント(なければ`SET_REPORT`)、Featureレポートは`SET_REPORT`と`GET_REPORT`で送受信する。Chromeはキーボードとマウスのレポートをページに渡さないが、ここに出る機器はシステムが入力として読んでいないので、そうしない |
| XR(`navigator.xr`) | ヘッドセットはなく、没入型のセッションは扱えないと応答する(`isSessionSupported`はfalse、`requestSession`は失敗)。インラインのセッションはページの中で動く |
| 画面の共有(`getDisplayMedia()`) | 要求されるたびに、パネルで共有するものを選ばせる(応答は記憶しない)。選べるものと音は14.4.18にある。キャプチャは`port/blink/screen_tessronos.cc`のキャプチャのホストで、カメラのキャプチャのホストが、その機器のセッションのクライアントを渡す(`camera_tessronos.cc`の`Hosts`)。セッションごとにキャプチャを1つ持つ。ホストのスレッドは、要求されたフレームレート(30まで)で、ウインドウマネージャが最後に合成した画面のうち、選んだ画面の矩形をポインタごとコピーし(SVC`wm_grab_screen`、10.4.3、`port/media/br_screen.cc`)、libyuvで`I420`にして、要求された大きさより大きければ縦横の比を保って縮小する。4つの共有メモリのバッファの扱いはカメラと同じである。共有している間、ブラウザのウインドウのタイトルの前に`[画面を共有中]`と出す(`br_capture_text`)。最後のトラックが停止するとキャプチャを止め、この表示も消える |
| 画面(`getScreenDetails()`) | 許可をパネルで尋ねてから、システムの1つの画面を返す |
| マシンのフォント(`queryLocalFonts()`) | 許可をパネルで尋ねてから、ブラウザが描画に使うフォント(Skiaのフォント管理が持つもの)の一覧を返す |
| リモート再生(メディア要素の`remote`) | 再生先の機器はなく、`watchAvailability`は`NotSupportedError`で拒否する |
| ゲームパッド(`navigator.getGamepads()`) | プログラムの機器のHIDのインタフェースのうち、コレクションがジョイスティックかゲームパッド(汎用デスクトップの4か5)のものを、接続されたときに開き(1秒ごとに機器を確認する)、1つずつのスレッドで入力レポートを読む(`port/blink/usb_hid_tessronos.cc`)。レポート記述子はChromiumのパーサで読み、ボタン(ボタンのページ)、軸(汎用デスクトップのXからWheelまでを-1から1に)、ハットスイッチ(軸2つ)を用途の順に並べる。Chromiumがベンダとプロダクトに標準の配置を持つものはそれを当て、`mapping`を`standard`にする。名前はChromeのLinux版と同じく「製品名 (STANDARD GAMEPAD Vendor: vvvv Product: pppp)」。Chromeと同じく、ボタンが一度押されるまではページに見せない。ページのウインドウができるたびにゲームパッドの補助(`NavigatorGamepad`)を作るので、`getGamepads`の前に付けた`gamepadconnected`のリスナーにも届く。振動は扱わないと応答する |
| 振動(`navigator.vibrate`) | 振動する機器はないので、振動はすぐ終わったと応答する(`vibrate`はtrueを返す。Chromeと同じく、利用者がページを操作する前はfalse) |
| Compute Pressure(`PressureObserver`) | プロセッサがタスクを実行した時間の割合(カーネルの`ts_ref_cpu`、`port/net/br_cpu.cc`)を1秒ごとに測り、0.6未満を`nominal`、0.75未満を`fair`、0.9未満を`serious`、それ以上を`critical`と伝える |
| スポイト(`EyeDropper`) | ページのウインドウのメッセージの欄に点の指定を求める案内を出し、利用者が次にページの上で押した点の色を返す(`br_pick_color`)。Escでやめる |
| カメラとマイク(`navigator.mediaDevices`) | マイクは、サウンドデバイスが録音できるとき(`OB_SND_INFO`の48kHzの録音)に、入力の機器「マイク」として一覧に出る。`getUserMedia`は利用者にパネルで尋ね(応答はオリジンごとにブラウザが動く間記憶する)、マイクの機器を返す。ストリームの音は、Blinkのローカルの音のソース(`ProcessedLocalAudioSource`)がプラットフォームから得る録音のソース(`br_audio_capturer`、`port/media/br_audio_sink.cc`)が取る。録音のソースは、サウンドデバイスの再生と別のユニット(`snda0`など。ユニットごとに経路が別になる)を48kHz、16bit、ステレオの録音にし、専用のスレッドで読んで、頼まれた大きさの区切りで渡す(`br_mic_open`、`br_mic_read`、`port/media/br_snd.cc`)。カメラは、カメラデバイスのユニットの実身(6.6)ごとに入力の機器として一覧に出て(名前は製品名)、モードを`I420`の形式の一覧として示す。`getUserMedia`で映像を要求すると、カメラ、マイク、両方のうち、まだ応答のないものを利用者に尋ね、カメラの機器を返す。カメラとマイクの応答はオリジンとそれぞれについてブラウザが動く間記憶し、どちらかが拒否されていれば尋ねずに拒否する。この応答は`navigator.permissions.query({name: 'camera'})`と`'microphone'`の応答にもなり(尋ねる前は`prompt`、許可すると`granted`、拒否すると`denied`)、ページの状態オブジェクトの`change`のイベントにも伝える(会議のサイトは、この応答でカメラとマイクを始めるか、許可を求める案内を出すかを決める)。ほかの許可の応答が変わったときも、その状態オブジェクトに伝える。映像は、Blinkの映像のキャプチャ(`VideoCaptureImpl`、プラットフォームの`GetVideoCaptureImplManager`)が通信するキャプチャのホスト(`VideoCaptureHost`、`port/blink/camera_tessronos.cc`)が送る。ホストは要求された大きさに一番近いモード(同じならMJPEGより無圧縮)でカメラからストリーミングし(`br_cam_open`、`port/media/br_cam.cc`)、読み取りのスレッドが新しいフレームを8ミリ秒ごとに確認して`I420`にする。YUY2、UYVY、NV12はlibyuvで、MJPEGはSkiaのJPEGのデコーダでデコードしてからlibyuvで変換する。サービスのスレッドで、それを4つの共有メモリのバッファの空いたものにコピーし、停止していないクライアントに通知する。バッファは、渡したクライアントがすべて返すと次に使う。ホストのクライアントはすべて1つのストリームを共有し、最後のクライアントが停止するとカメラを閉じる。カメラが取り外されると、トラックは終了する。カメラの機器のIDは、ページのオリジンとユニットの実身のUUID(v7)を連結したもののSHA-256を64桁の小文字の16進にしたものである。既定の機器以外のIDはこの形式でなければメッセージの検査で破棄される。同じカメラは同じオリジンに対して起動をまたいで同じIDになり、オリジンが違えば違うIDになるので、サイトをまたいで利用者を識別する手がかりにならない。機器の型のシリアライズ(`media/capture`の`mojom`と型)は、Blinkの全体から除いている`media/capture`のうち、それだけを組み込む |
| 音声の合成(`speechSynthesis`)と認識(`SpeechRecognition`) | ブラウザに入れた合成器と認識器で動く(14.4.15)。マイクがないマシンでは、認識はすぐ`audio-capture`のエラーで終わる |

`MediaRecorder`と、canvasとメディア要素の`captureStream()`は、Blinkのモジュールをそのまま使う。機器がなくても、canvasや`AudioContext`から作ったストリームを、メディアライブラリのソフトウェアのエンコーダ(VP8、Opus)でWebMに記録する。

### 14.4.9 Web Serial

`navigator.serial`(`port/blink/serial_tessronos.cc`)は、システムのシリアルポートをSVCのクラス11(6.2、`port/net/br_serial.cc`)で使う。

- `requestPort()`は、ポートの一覧(デバイスの名前と説明。コンソールのポートには「(コンソール)」を付ける)をパネル4で選ばせる。選ばれたポートは、ブラウザが動く間、そのオリジンのページが開け、`getPorts()`に現れる。`forget()`で外す。システムのポートはUSBの機器ではないので、ベンダやプロダクトのIDで絞り込む`filters`には何も当たらない。
- `open()`は、ポートを開いて要求された速度にする。ページが`readable`を読む間、ポートを専用のスレッドで読み(1回の待ちは0.1秒)、届いたバイトをサービスのスレッドでページのデータパイプに入れる。パイプに入りきらないものは64KBまで持ち、それを超えると読むのを待つ。ページが`writable`に書いたものはサービスのスレッドでパイプから取り出し、専用のスレッドがポートに送る。遅いポートでも、サービスのスレッドとページは待たない。
- システムのポートにはデータの線しかないので、制御線の設定は受け付けるだけで、読むとすべて偽を返す。データの形式は8ビット、パリティなし、ストップビット1である。

### 14.4.10 通知

Web通知(`new Notification`と、Service Workerの`registration.showNotification`)は、デスクトップの通知(11.28)で表示する。ブラウザ側の`NotificationService`(`port/blink/services_tessronos.cc`)が通知に番号を付けて`DT_RQ_NOTIFY`を書き(`port/net/br_notify.cc`)、デスクトップの応答を専用のスレッドが1つの応答用のチャネルで読んで、番号で通知に渡す。

- 表示の許可(`Notification.requestPermission`)は、利用者にパネルで尋ねる。通知元にはページのオリジンを出す。
- ウインドウの通知は、表示するとリスナーに`show`を伝え、〈開く〉で`click`、〈閉じる〉で`close`を伝える。`notification.close()`は通知を閉じて`close`を伝える。同じタグの通知は前のものと入れ替わる。
- Service Workerの通知は、〈開く〉で`notificationclick`(`action`は空)、通知のボタンで`notificationclick`(`action`はそのボタンの名前)、〈閉じる〉で`notificationclose`をワーカーに送る(`br_sw_dispatch`)。`requireInteraction`の通知は時間がたっても消えない。時間がたって消えた通知も、ブラウザの通知の一覧に残り、`getNotifications`に応答する。

### 14.4.11 プッシュ

Web Push(Service Workerの`registration.pushManager`)は、プッシュサービスにWebSocketで常時接続し、届いたメッセージをワーカーの`push`イベントにする(`port/net/push_tessronos.cc`、`port/blink/services_tessronos.cc`の`PushMessagingImpl`)。

- プッシュサービスは既定でMozillaのautopush(`wss://push.services.mozilla.com/`)である。autopushは、送信者の鍵(VAPID)を持つどのサイトにも購読を与え、ブラウザはサービスの側にアカウントを必要としない。起動引数の行`push=`で別のサービス(autopushと同じプロトコルのもの)を指定できる。
- 接続はネットワークスタックのスレッドの`WebSocketChannel`で張り、サブプロトコルは`push-notification`である。`hello`でユーザエージェントの識別子(`uaid`)を受け取り、以後はそれを使う。切れると間隔を延ばしながら再接続し、25分ごとに`ping`を送る。
- `subscribe()`は、`userVisibleOnly`がtrueで送信者の鍵(`applicationServerKey`)があるものだけを受け付ける。通知の許可がまだなければ利用者にパネルで尋ねる。スコープごとにチャネルの番号を作って`register`を送り、サービスが返した`pushEndpoint`を購読の宛先にする。購読ごとにP-256の鍵と16バイトの認証のシークレットを作り、ページに`p256dh`と`auth`として渡す。サービスが20秒応答しなければ`AbortError`で終わる。
- `getSubscription()`はそのスコープの購読を返し、`unsubscribe()`は`unregister`を送って購読を消す。購読(宛先、鍵、シークレット、`uaid`)は「ブラウザの利用者データ」のレコード6(ストア`push`)に入れ、ブラウザを終了しても残る。購読があれば、ブラウザは起動するとサービスに接続し、前の`uaid`を使う。ブラウザが動いていない間に送られたメッセージは、サービスが保持していて、そのときに届く。
- サービスの`notification`は、まず`ack`で受信確認を返し、本文をRFC 8291の`aes128gcm`として復号する(ECDH P-256、HKDF-SHA-256、AES-128-GCM。BoringSSLを使う)。復号できたものを、そのスコープのワーカーに`push`イベント(`data`は本文)として送る(`br_sw_dispatch_scope`)。ワーカーが動いていなければ、残してある登録(14.4.6)から起動する。そのスコープの登録がなければ、メッセージは破棄する(受信確認は返してある)。
- 本文のない`notification`は、データのない`push`イベントにする。古いエンコーディングの`aesgcm`(RFC 8291の前のドラフトのもの。ソルトと送信者の鍵は`Encryption`と`Crypto-Key`のヘッダで来る)も復号する。
- テスト用の起動引数の行`pushkey=`は、購読の秘密鍵と認証のシークレットを与える。テストは、それに向けて暗号化したメッセージを送るサービスを使う。

### 14.4.12 実行時の機能、タイムゾーン、画面、ユーザエージェント

Blinkの実行時の機能(`RuntimeEnabledFeatures`)のうち、`base::Feature`に従うものは、Chromeではレンダラプロセスが起動時に`WebRuntimeFeatures::UpdateStatusFromBaseFeatures`で`base::Feature`の状態を反映する。このブラウザも、Blinkの初期化の直後に同じ呼出しをする。以前は呼んでいなかったので、Chromeで既定で有効な機能のうち、Blinkの一覧で安定と書かれていないものが無効のままだった。YouTubeの再生の検証のスクリプト(BotGuard)は、その差のために例外で止まり、検証の結果(`GenerateIT`への要求)にエラーの文字列を入れていた。呼ぶようにすると、検証は最後まで動き、サーバから検証のトークン(integrity token)が返る。

スクリプトの日時と`Intl`は、システムのタイムゾーン(ユーザ環境設定で選ぶ地域、9.5)で動く。ブラウザは起動時に`dt_getsyszone`で地域のIANAの名前を読み、ICUの既定のタイムゾーンにする(`br_zone_name`、`follow_zone`)。名前のないときは時差から`GMT+09:00`の形式の名前を作る。3秒ごとに確認し、変わっていればICUの既定を替えて、Chromeがマシンのタイムゾーンが変わったときにするのと同じく、V8に日時の設定が変わったことを通知する(`DateTimeConfigurationChangeNotification`)。以後に作る日時は新しいタイムゾーンになる。夏時間とその地域の過去のルールはICUのデータが扱う。テストは`test_blink_zone`である。

`screen.width`、`screen.height`、`screen.colorDepth`、画面の大きさのメディアクエリは、`ChromeClient`の`GetScreenInfos`が返す。画面の大きさはウインドウマネージャに問い合わせ(`wm_screen`、SVC 9-36)、1ピクセルを1 CSSピクセル、色深度を24ビットとする。以前は基底の`ChromeClient`の空の値のままで、幅と高さと色深度が0だった。

ユーザエージェントの文字列は、Chrome 153のLinuxのデスクトップの縮小したユーザエージェントと同じ`Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36`である。Chromeは縮小したユーザエージェントでは、Linuxならプロセッサによらず`X11; Linux x86_64`と名乗り、`navigator.platform`も`Linux x86_64`を返す。以前は`X11; TessronOS aarch64`と名乗っていたが、`navigator.platform`(`Linux x86_64`)、クライアントヒントのプラットフォームと食い違い、ページから見て一貫していなかった。同じことをクライアントヒントでも示す(`br_user_agent_metadata`)。`navigator.userAgentData`のブランドは`Chromium` 153と、Chromiumがどのバージョンでも足す意味のないブランド(バージョン番号から選ぶ`Not_A Brand` 8)で、この順に並ぶ(並びもバージョン番号から決まり、153ではChromiumが先)。詳しいバージョン(`fullVersionList`、`uaFullVersion`、`Sec-CH-UA-Full-Version-List`)は、コンパイルしたソースのバージョン`153.0.8010.36`である。プラットフォームは`Linux`、アーキテクチャは`arm`の64ビットである(プロセッサはクライアントヒントだけが示す)。フレーム、専用ワーカー、共有ワーカー、Service Workerが同じ値を持つ。安全なオリジンへの要求には、ネットワークの取得が`Sec-CH-UA`、`Sec-CH-UA-Mobile`、`Sec-CH-UA-Platform`のヘッダを付ける(ページが自分で付けたものは替えない)。`navigator.platform`は、ChromeがLinuxのどのアーキテクチャでも返す固定の値`Linux x86_64`である(Blinkのユーザエージェントの縮小の値)。

ChromeのLinux版のウインドウにあるインタフェースのうち、このブラウザになかったのは`BeforeInstallPromptEvent`だけだった(ウインドウのすべてのインタフェースの名前とメンバをページから書き出し、ChromiumのIDLと照らし合わせて確認した。Android、ChromeOS、隔離されたWebアプリだけのもの、端末上の言語モデルを要するAIのAPIを除く)。そのモジュールをコンパイルして加えた。Webアプリのインストールはないので、`beforeinstallprompt`のイベントは起きない。

ページから見えるブラウザの振る舞いは、Chrome 153のLinuxのデスクトップと次の点をそろえている(テスト`test_blink_face`が値を書き出す)。

| 項目 | 値 |
| --- | --- |
| `window.chrome` | `loadTimes()`、`csi()`、`app`(`isInstalled`は偽、`getDetails`、`getIsInstalled`、`installState`、`runningState`、`InstallState`、`RunningState`)。関数はChromeと同じくネイティブの関数で、`loadTimes()`と`csi()`の戻り値はネイティブのデータプロパティを持つ。値はウインドウのパフォーマンスのタイミングと応答から作る(`install_chrome`) |
| `navigator.languages`、`Intl`の既定のロケール、`Date`の文字列 | `ja`、`en-US`、`en`。ICUの既定のロケールを`ja`にする(以前は`en-US`で、`navigator.language`と食い違っていた) |
| `(pointer: fine)`、`(hover: hover)` | 真(マウスのあるマシン)。以前は偽で、ページがタッチの画面と判断していた |
| `outerWidth`、`outerHeight`、`screenX`、`screenY` | ブラウザのウインドウの外寸と位置(`ChromeClient::RootWindowRect`)。以前は0だった。位置は、ウインドウがある画面の左上からの値である(14.7) |
| `screen.width`、`screen.height` | ブラウザのウインドウがある画面(ウインドウの中央がある画面。`wm_screen_of`)の大きさ。ページに見せる画面はこの1つで、左上を(0, 0)とする(14.7) |
| 文書の応答 | ステータスコード、ヘッダの行、プロトコルを文書に渡す(`BrEngine::response`、`apply_response`)。`Content-Security-Policy`と`Content-Security-Policy-Report-Only`は、`Platform::AppendContentSecurityPolicy`で文書のCSPに入れる。ネットワークから(または`data:`のURLで)メインフレームにコミットする文書には、新しい既定のポリシーのコンテナを渡す(`main_policies`)。渡さないとBlinkはウインドウの前の文書のコンテナを引き継ぎ、新しい文書がそれまでの文書のCSPにも制約される(2026年10月15日まで、Googleのログインの画面から移った`workspace.google.com`のページが、ログインの画面のnonce付きのCSPで自分のスクリプトをブロックされていた)。`about:blank`の文書は前の文書のものを引き継ぐ。`Permissions-Policy`、`Refresh`、`Link`などもBlinkが応答から読む。`performance.getEntriesByType('navigation')`の`nextHopProtocol`、`responseStatus`、`transferSize`と`chrome.loadTimes()`の`connectionInfo`はこれで実際の値になる。iframeの文書も同じ |

`navigator.plugins`と`navigator.mimeTypes`は、PDFを表示するChromeと同じく5つ(`PDF Viewer`、`Chrome PDF Viewer`、`Chromium PDF Viewer`、`Microsoft Edge PDF Viewer`、`WebKit built-in PDF`)と2つ(`application/pdf`、`text/pdf`)で、`pdfViewerEnabled`は真である(14.6.6)。WebGLの`UNMASKED_RENDERER_WEBGL`は、`make GL=1`ではANGLEの文字列、そうでなければ`TessronOS software GL`である。

テスト用の起動引数の行には次のものがある。`script=js:<スクリプト>`は、ページを表示した後にそのスクリプトをメインワールドで実行する(`script=<URL>`は取得したスクリプトを実行する)。`features=<名前>,-<名前>`は、Blinkの実行時の機能を有効にする(`-`を付けると無効にする。Chromeが試験中として無効にしているものを試すため)。`netlog=<語>,<語>`は、URLにその語を含む要求について、要求のヘッダと本文、応答の本文(テキストは16KBまで、そのほかは2KBまでを16進で)をコンソールに出力する。`quic=off`は、HTTP/3(QUIC)を使わず、すべての要求をTCPで送る(HTTP/3の転送の速さと比べるため)。`trace=exceptions`は、ブラウザのAPIがページのスクリプトに投げた例外を、インタフェース、メンバ、例外のメッセージの組ごとに1度コンソールに出力する(V8の`SetExceptionPropagationCallback`で、Blinkがメッセージを整形した後に取得する)。ページの実身の本文に`trace=exceptions`の段落を書き足しておくと、そのページの実身から開いたブラウザは、これを起動引数の行として受け取る(コンソールに`browser: trace=exceptions, from the page object`と出る)。実機で利用者がサイトの振る舞いを調べられるようにするためである。ほかの起動引数の行はページの実身からは受け取らない(`script=`はページで任意のスクリプトを動かし、`netlog=`は送信したパスワードもコンソールに出力するため)。

### 14.4.12.1 クロスオリジンの分離

最上位の文書の応答が`Cross-Origin-Opener-Policy: same-origin`と、分離を許す`Cross-Origin-Embedder-Policy`(`require-corp`か`credentialless`)を持ち、安全なコンテキストであるとき、Chromeはそのブラウジングコンテキストグループを分離し、ページは`crossOriginIsolated`が真になって`SharedArrayBuffer`と共有の`WebAssembly.Memory`を使える(ZoomのWeb版は、WebAssemblyのスレッドのためにこれを使う)。以前のこのブラウザは分離をせず、`crossOriginIsolated`はいつも偽で、`SharedArrayBuffer`もなかった。

- 文書をコミットするとき(`set_agent_cluster`、`br_blink.cc`)、応答のヘッダをChromiumのパーサ(`network::ParseCrossOriginOpenerPolicy`、`ParseCrossOriginEmbedderPolicy`)で読む。最上位の文書が上の条件を満たせば、そのページは最上位の文書のオリジンで分離される。ページのすべての文書(iframeも)は、エージェントクラスタのキーにその分離(`WebCrossOriginIsolationKey`、オリジンは最上位の文書のもの、モードは`kConcrete`)を持つ。Chromiumでも、分離のキーは最上位の文書のオリジンで、ページのすべての文書に当たる。Blinkはこのキーから`crossOriginIsolated`を決め、iframeは権限ポリシー`cross-origin-isolated`が許すときだけそれを得る。専用ワーカーは作成元のコンテキストの分離を受け継ぐ(Blinkの`DedicatedWorker`)。分離したページでは`Origin-Agent-Cluster: ?0`は効かない。
- 文書の`Cross-Origin-Embedder-Policy`は文書のポリシー(`policy_container`)に入れる。フレームのローダはそれを持ち、サブリソースの応答をChromiumの`network::CrossOriginResourcePolicy::IsBlockedByHeaderValue`で検査する(`br_loader.cc`の`corp_allows`)。別のオリジン(サイト)へのno-corsの要求の応答が`Cross-Origin-Resource-Policy`で`same-origin`(`same-site`)を指定していれば、COEPによらずブロックする。COEPが`require-corp`なら、CORPを持たない応答もブロックする。ブロックした要求は`net::ERR_BLOCKED_BY_RESPONSE`で失敗し、コンソールに出力する。COEPが`credentialless`のときは、別のオリジンへのno-corsの要求を資格情報なしで送り、CORPのない応答も受け取る。以前はCORPを検査していなかった。
- iframeの文書は、親の文書のCOEPが`require-corp`か`credentialless`のとき、自分も分離を許すCOEPを持たなければならず、ネストしたナビゲーションのCORPの検査(`network::CrossOriginResourcePolicy::IsNavigationBlocked`)も通らなければならない(`coep_blocks_frame`)。通らなければ、Chromeと同じく、そのフレームには文書の代わりにブラウザのエラーの文書(`chrome-error://chromewebdata/`、`net::ERR_BLOCKED_BY_RESPONSE`、オリジンは不透明)をコミットし、コンソールに出力する。親はその中を見られない(`contentDocument`は`null`)。
- 分離していないページでは、`SharedArrayBuffer`はChromeと同じくない(V8の`--enable-sharedarraybuffer-per-context`で、コンテキストごとにBlinkに問い合わせる)。

### 14.4.13 組み込みのAI

Chrome 153がLinuxで出す組み込みのAIのAPI(`LanguageDetector`、`Translator`、`Summarizer`、`LanguageModel`)を出す(`port/blink/ai_tessronos.cc`)。`Writer`、`Rewriter`、`Proofreader`はChromeでもオリジントライアルなので出さない。

- `LanguageDetector`は実際に判定する。Chromeはブラウザがダウンロードした言語判定のモデル(TFLite)をファイルで渡すが、このブラウザはChromiumのソースツリーのCLD3(ニューラルネットワークの重みをソースに持つ)で判定する。言語判定のドライバ(`ContentLanguageDetectionDriver`)はモデルがある(`kReadily`)と応答し、Blinkの判定のモデルにはパッチ`language_detection_model.patch`で判定の関数を渡す。CLD3が見つけた言語(3つまで)に、文のうちその言語の部分の割合と確からしさを掛けたものを確信度とし、残りを判定できないもの(`und`)にする。CLD3の古い言語コード(`iw`、`jw`、`tl`)は今のもの(`he`、`jv`、`fil`)にする。
- 翻訳の言語パック、要約や言語モデルのモデル(Gemini Nano)はマシンになく、取得もできないので、Chromeがモデルを持てないマシンで応答するのと同じく、`availability()`は`unavailable`を返し、`create()`は拒否する。`LanguageModel.params()`はnullである。

### 14.4.14 隔離されたWebアプリだけのAPI

Chromeは、Direct Sockets、ウインドウの操作(`window.maximize`、`minimize`、`restore`、`setResizable`)、Web Printingなどを、隔離されたWebアプリ(IWA)にだけ出し、普通のページには出さない。このブラウザにはIWAの仕組みがないので、利用者が許可したオリジンをIWAの代わりにする。ウインドウのメニューの〈このサイトを隔離されたWebアプリとして扱う〉を選ぶと、そのページのオリジンが許可され(チェックが付く)、ページを再読み込みする。もう一度選ぶと外れる。許可したオリジンは許可の応答とともに「ブラウザの利用者データ」のレコード10(ストア`perms`)に残る。テストは起動引数の行`isolated=<オリジン>`でその実行の間だけ許可する。

- 許可したオリジンのウインドウは、Blinkにとって隔離されたコンテキスト(`IsIsolatedContext`)であり、クロスオリジン分離(`crossOriginIsolated`)でもある(パッチ`isolated_context.patch`)。IWAはいつもクロスオリジン分離されているからである。
- Direct Sockets(`TCPSocket`、`UDPSocket`、`TCPServerSocket`)は、システムのソケットで動く(`port/blink/direct_sockets_tessronos.cc`)。TCPは名前解決をして接続し、受信したバイトをページが読むデータパイプへ、ページが書いたバイトをソケットへ、それぞれのスレッドで運ぶ。TCPのサーバは接続を受け付けるたびに同じ形の接続を渡す。UDPには相手に接続したものと、自分のアドレスにバインドして誰にでも送るものがあり、ページが求める数だけ受信したデータグラムを渡す。マルチキャストへの参加とTLSへの切り替えは扱わない。
- ウインドウの操作は、Chromeの試験中の機能(`DesktopPWAsAdditionalWindowingControls`)を、許可したオリジンのウインドウでだけ有効にする。ページはアプリのスコープ(`Settings::SetWebAppScope`、そのオリジン)にあるものとし、ウインドウの管理の許可をパネルで尋ねてから行う。最大化は外枠をウインドウがある画面の矩形にし、元に戻すと前の位置に戻して前面に出す。ウインドウシステムには最小化の状態がないので、最小化はほかのすべてのウインドウの後ろに回す。`setResizable(false)`の後に利用者が大きさを変えると、元の大きさに戻す。
- Web Printing(`window.printing`)は、デスクトップの印刷を1台のプリンタ「TessronOSの印刷」として見せる(`port/blink/printing_tessronos.cc`)。属性はA4、1部、カラー、縦と横、96dpiである。ChromeがCUPSを持つChromeOSでだけ使う印刷のインタフェースは、パッチ`web_printing_manager.patch`でこのブラウザでも使う。印刷の文書(PDF)は、PDFiumでページごとに用紙の印刷範囲に描画し、ページの印刷と同じくデスクトップに渡す(14.7.5)。ジョブは処理中、完了(印刷されなかったときは中止)と通知する。
- 裏付けのないもの(Smart Card、Web NFC、Contacts、Digital Goods、SubApps、Web Install)は、ChromeのLinux版と同じく出さない。スマートカードのリーダのドライバ、NFC、アドレス帳、アプリの導入の仕組みがないからである。

### 14.4.15 音声の合成と認識

Web Speech API(`speechSynthesis`、`SpeechRecognition`)は、ブラウザに入れた合成器と認識器で動く(`port/blink/speech_tessronos.cc`、`port/speech/speech_glue.c`)。合成器と認識器は`tools/browser/speech.sh`がコンパイルしたもの(`libspeech.a`)で、使う辞書、音声、モデル(約57MB)は、プログラム「ブラウザ」の実身のレコード2に入れる(`speech.sh`が1つのレコードにまとめ、ビルドがプログラムの横に置く)。プログラムは起動のときに丸ごとメモリに読み込まれるので、実行ファイルに入れると、ウインドウを別のブラウザで開くたびに57MBを余計に読み込み、時間とメモリを使う。レコード2は合成か認識をするときに初めて読み込む(`port/speech/br_speech_rec.cc`)。ネットワークのサービスは使わない。

| 用途 | 使うもの |
| --- | --- |
| 日本語の合成 | Open JTalk 1.11(MeCabと辞書naist-jdicで形態素に分け、読みとアクセントを付け、HTSのラベルにする)とhts_engine API 1.10、音声`nitech_jp_atr503_m001`(48kHz) |
| 英語の合成 | Flite 2.2、音声`cmu_us_slt`(16kHz) |
| 認識 | whisper.cpp 1.7.6、Whisperのtinyのモデル(5bitに量子化したもの、約31MB)。CPU(NEON)の4つのスレッドで認識する |

- 声は2つで、`getVoices()`は「Open JTalk 日本語」(`ja-JP`、既定)と「Flite English」(`en-US`)を返す。どちらも`localService`である。発話は、名指しされた声、なければ言語(`ja`で始まるか)、言語もなければ本文に仮名か漢字があるかで声を選ぶ。`rate`は話す速さ、`pitch`は高さ(Open JTalkでは半音の数、Fliteでは基本周波数の平均)、`volume`は大きさになる。
- Blinkは発話を1つずつ渡す。合成は1つのスレッドで順に行い、作った音を48kHz、ステレオにして、メディア要素と同じ音のリングバッファ(14.6.2)に空いた分ずつ書く。書き始めるときに`start`を、リングバッファに書いた音が鳴り終わったときに`end`をページに通知する。`pause()`は書くのを止め、`resume()`で続ける。`cancel()`は話している発話を止めて`interrupted`のエラーで終わらせる。まだ渡されていない発話はBlinkが破棄する。
- 日本語の辞書のファイル(`sys.dic`など)は圧縮して持ち、初めて日本語を話すときにレコードから読んで展開する(`br_speech_file`)。展開した辞書(約107MB)はブラウザが終了するまで残す。音声とモデルは圧縮せずにそのまま読む。
- 認識は、マイクがなければすぐ`audio-capture`のエラーで終わる。あればマイクの使用をパネルで尋ね(応答はオリジンごとにブラウザが動く間記憶する)、拒否されれば`not-allowed`で終わる。許可されると録音を始め(`br_mic_open`、14.4.8)、48kHzのステレオを16kHzのモノラルにしてバッファに貯める。音の大きさ(二乗平均)がしきい値を超えると`soundstart`を通知し、音が出た後に1秒の無音が続くか、10秒たつか、ページが`stop()`を呼ぶと録音を終える。貯めた音をwhisper.cppに要求した言語(`lang`の`-`の前、なければ自動)で認識させ、聞き取った文を確定した結果(`isFinal`が真)として1つ渡す。エンコーダは30秒のウインドウの全部でなく録音の長さに少し足した分だけを見る(`audio_ctx`)ので、数秒の音は短い時間で認識できる。`abort()`は`aborted`、音がなければ`no-speech`、何も聞き取れなければ`no-match`で終わる。途中の結果(`interimResults`)と、連続した認識(`continuous`)の区切りは扱わず、どちらも1つの確定した結果になる。
- マシン上での認識のAPI(`SpeechRecognition.available()`、`install()`)は、Chromeの試験中の機能なので出さない。
- MeCabは設定ファイルを読まない(Open JTalkが値をコードで与える)が、そのために開くだけのファイルストリームを作る。ツールチェーンのC++のライブラリはスレッドなしでビルドされていて、そのファイルストリームの大きさが、スレッドありとしたヘッダ(`include/tscxx`)で並べたものと合わず、生成すると異常終了する。そこでその1行をコピーから除く(`speech.sh`)。文字列ストリームは大きさが合うのでそのまま使える。

### 14.4.16 WebNN

WebNN(`navigator.ml`、`MLContext`、`MLGraphBuilder`、`MLGraph`、`MLTensor`)は、Chrome 153でも試験中の機能(`MachineLearningNeuralNetwork`、親の機能`WebMachineLearningNeuralNetwork`は既定で無効)なので、Chromeと同じく既定では出さず、起動引数の行`features=MachineLearningNeuralNetwork`で出す。

- ページが作ったグラフの検査、定数の並べ替え、テンソルの管理は、ChromiumのWebNNのサービス(`services/webnn`のコンテキスト、グラフの構築、テンソル)が行う。ChromeはGPUのプロセスがコンテキストを作れないときにこのサービスをレンダラプロセスで動かし(`WebNNContextProviderInRenderer`)、LiteRTで計算する。このブラウザもサービスを自分のプロセスで動かし、計算はLiteRTの代わりに移植レイヤ(`port/blink/webnn_tessronos.cc`)が行う。そのためのインタフェースをパッチ`webnn_context_provider.patch`で足す。
- 計算は、ページが組み立てた順に演算を1つずつ、テンソル全体について倍精度浮動小数点数で行い、結果をそのオペランドの型(float32、float16、各整数)に丸めてから次の演算に渡す。WebNNの仕様の演算のすべて(要素ごとの演算と比較と論理演算、型の変換、`matmul`、`gemm`、`conv2d`と`convTranspose2d`(グループと拡張を含む)、プーリング、正規化、`softmax`、リダクション、`gather`と`scatter`の各種、`pad`、`slice`、`split`、`concat`、`transpose`、`tile`、`resample2d`、`triangular`、`where`、量子化と逆量子化、`gru`、`gruCell`、`lstm`、`lstmCell`など)を扱う。
- コンテキストは、ページが求める機器(`cpu`、`gpu`、`npu`)によらずプロセッサで計算する。オペランドのレイアウトはNCHW(`conv2d`の重みはOIHW、`convTranspose2d`はIOHW)で、型は4bitの整数を除くすべて、次元は8までを扱う。テンソルはブラウザのメモリにあり、WebGPUのテンソルとの共有は扱わない。

### 14.4.17 Chromeとの比較

ページから見える振る舞いとネットワークへの要求の形を、実際のChrome 153と同じページで比べてそろえた。基準は、WSLの中でXvfbの画面に出したGoogle Chrome 153.0.8010.36(Linux版の配布物)と、Chrome for Testingの同じバージョンである。Chrome for Testingはテスト用のビルドで、XSLT、Topics、Protected Audience、Attribution Reportingを無効にし、WebMCPを有効にしているので、比べる基準はGoogle Chromeにした。

比べるページは、ウインドウと各インタフェースの静的な名前とプロトタイプの名前(`Reflect.ownKeys`をたどり、各プロパティの記述子、関数の`length`、`name`、`Function.prototype.toString`の形)、`navigator`、`screen`、`window`、`document`の値、`userAgentData.getHighEntropyValues`、メディアクエリ、`CSS.supports`、計算されたスタイル、`Intl`と`Date`、数学関数の値、時計の分解能と進み、エラーのスタックとメッセージ、権限の問い合わせ、WebGLの値と拡張、Web Audioの出力、canvasの描画、フォントの幅、メディアの型、WebCodecs、WebAuthnの能力、Wasmの機能、ワーカー(専用と共有)とiframeの中の同じ値、CORSの振る舞い、`httpbin.org`が返す要求のヘッダを集め、JSONでサーバに送る。差分ツールがそれを「Chromeにだけある」「こちらにだけある」「値が違う」「並びが違う」に分け、画面の大きさ、プロセッサの数、GPU、時計の細かい値のような、実際のLinuxのマシンどうしでも違うものは別に分ける。

違っていて修正したものは次のとおりである。

| 項目 | 以前 | 修正後(Chromeと同じ) |
| --- | --- | --- |
| CORS | 移植したフェッチのレイヤ(`br_loader.cc`)が検査をせず、ほかのオリジンへの`fetch`とXMLHttpRequestが読め、画像でcanvasが汚染されず、ほかのオリジンのスクリプトの例外のメッセージが見えた | 要求のモードとオリジンから、応答をCORSで検査する(`network::cors::CheckAccess`)。安全でないメソッドとヘッダにはプリフライトリクエスト(`OPTIONS`、`Access-Control-Max-Age`の間はキャッシュする)を送る。`no-cors`のほかのオリジンへの応答は不透明(`opaque`)、CORSの応答は`cors`の型にし、Blinkが公開するヘッダを絞る。`same-origin`のモードのほかのオリジンへの要求は送らない |
| 要求の`Origin`、`Sec-Fetch-Site`、`Sec-Fetch-Mode`、`Sec-Fetch-Dest`、`Sec-Fetch-Storage-Access` | 付けていなかった | ネットワークサービスと同じルールで付ける(CORSの要求とGET、HEAD以外の`Origin`、安全な宛先への`Sec-Fetch-*`) |
| 資格情報 | 常に送った | `credentials`が`omit`、または`same-origin`でほかのオリジンのときは、Cookieと認証を送らない |
| ウインドウの移動のCookie | 移動の要求のCookieの基準のサイト(site for cookies)が最初のアドレスのままで、ほかのサイトへのリダイレクトの先はクロスサイトの要求として扱われた。`SameSite`のない(既定でLax)Cookieを、リダイレクトの応答が付けても保存せず、送りもしなかった。`www.gmail.com`から`accounts.google.com`へ転送されたログインの画面は、最初の応答が付けた`__Host-GAPS`を持たず、画面の要求(`batchexecute`、`browserinfo`)がHTTP 400で拒否された(2026年10月15日まで) | ブラウザの移動と同じく、基準のサイトをリダイレクトごとにその先のアドレスにし、メインフレームの移動としてCookieを扱う。移動を求めたページのオリジンを要求のイニシエータ(initiator)にし、利用者が求めた移動はイニシエータなしにする(`br_navigation_headers`の行`X-TessronOS-Navigation`、`X-TessronOS-Initiator`をネットワークスタックが取り除いて使う) |
| 優先度 | 全部が既定の優先度で、HTTP/2とHTTP/3の`priority`が`i`だけだった | Blinkが決めた優先度と`incremental`をネットワークのレイヤに渡し、`u=1, i`などになる |
| `Accept-Encoding` | `gzip, deflate` | `gzip, deflate, br, zstd`(BrotliとZstandardの復号を入れた) |
| 高エントロピーのクライアントヒント | 送らなかった | ウインドウの文書の応答の`Accept-CH`をオリジンごとに記憶し、文書をその指示でコミットする(`enabled_client_hints`)。Blinkがサブリソースの要求に`Sec-CH-UA-Arch`、`-Full-Version-List`、`Sec-CH-DPR`、`Sec-CH-Viewport-Width`、`Device-Memory`などを付ける。ウインドウの移動の要求にもChromeと同じ`Accept`、`Upgrade-Insecure-Requests`、`Sec-Fetch-*`、記憶したヒント、最高の優先度を付ける(`br_navigation_headers`) |
| ブランドの並びと詳しいバージョン | `Not_A Brand`が先、詳しいバージョンは`153.0.0.0` | `Chromium`が先、`153.0.8010.36` |
| `SharedArrayBuffer` | どのコンテキストにもあった | クロスオリジン分離のコンテキストだけ(V8の`--enable-sharedarraybuffer-per-context`) |
| タッチのイベントのハンドラ(`ontouchstart`など) | あった | タッチの画面のないマシンなのでない |
| Web Bluetooth | あった | ChromeのLinux版と同じくない |
| 決済のアプリ(`PaymentManager`)、`a.hrefTranslate`、`SerialPort.connected`、`Request.targetAddressSpace`、`navigator.deprecatedURNToURL`など | なかった | ある |
| `AbstractRange`の`startContainer`と`endContainer` | あった | `OpaqueRange`が入っているときはない(Chromeと同じく、引数の解決の後に決める) |
| `window.originAgentCluster` | 偽 | 真。文書をオリジンで分けたエージェントクラスタに入れる(`Origin-Agent-Cluster: ?0`のときを除く)。Blinkの既定の設定が許していたファイルのページの全体へのアクセスをChromeの設定と同じく無効にした |
| `chrome.loadTimes`、`chrome.csi`、`chrome.app` | 関数に`prototype`がなく、`app`のメンバの順が違った | Chromeと同じ |
| WebGLの`VERSION`、`SHADING_LANGUAGE_VERSION` | ANGLEの文字列(`OpenGL ES 2.0 (ANGLE 2.1.1 ...)`) | GPUのコマンドバッファの文字列(`WebGL 1.0 (OpenGL ES 2.0 Chromium)`) |
| 権限の既定 | `payment-handler`、`pointer-lock`、`keyboard-lock`が尋ねる、`periodic-background-sync`、`top-level-storage-access`が許す | 前の3つは許す、`periodic-background-sync`はインストールしたアプリだけ(テストの`periodicsync=`の行のとき)、`top-level-storage-access`は尋ねる |
| `enumerateDevices` | 許可の前から機器の名前とIDが見えた | 許可の前は種類ごとに1つ、ID、グループ、名前を空にする |
| 文書の`PerformanceNavigationTiming.contentType` | 空 | 文書の型 |
| `navigator.cpuPerformance` | 0 | コアの数からの段階(4コアは2) |
| キーボードの配列の一覧 | 47 | 48(`IntlBackslash`) |
| `(transform-3d)`、`(-webkit-transform-3d)` | 偽 | 真(パッチ`media_values_3d.patch`) |
| `getClientCapabilities` | 空 | このマシンの認証器(パスキーの保存、利用者の確認あり)と、条件付きの仲介を持たないことを名前で返す |
| ワーカーのIndexedDB | ファクトリがなく、`indexedDB.open`が応答しなかった | ウインドウと同じオリジンのデータベースを、ウインドウのスレッドで扱う |
| 文書のタイミング | 移動の開始がコミットの時刻で、`performance.timing`の`fetchStart`、`requestStart`、`responseStart`、`connectStart`などが0だった(YouTubeの計測のマークが負の時刻になる) | ウインドウが移動を告げられた時刻、要求を送った時刻、応答が来た時刻を文書のタイミングと応答の`LoadTiming`に渡す(`br_navigation_timing`、`apply_nav_timing`) |

その後、比べ方を2つ加えた。1つ目はビルドの設定の突き合わせである。公式ビルドのgnの引数(`is_official_build`、x64)を別の出力のディレクトリで`gn args --list`に一覧を出力させ、`out/tessronos`の一覧と比べた。ページから見える振る舞いに関わる違いは、`v8_enable_temporal_support`、`enable_hls_demuxer`、`enable_mse_mpeg2ts_stream_parser`、`use_v8_context_snapshot`の4つだった。content層がbase::Featureの状態からBlinkの実行時の機能を決める表(`content/child/runtime_features.cc`)も既定の状態で突き合わせたが、このブラウザの設定と違うものはなかった。2つ目はWeb Platform Testsである。チェックアウトの`third_party/blink/web_tests/external/wpt`をwptserveで提供し、テストのページを1つずつiframeで開くページ(runner)が、testharness.jsが親のウインドウに送る完了の通知を集める。同じテストをChromeとこのブラウザで実行し、サブテストごとに結果を比べた。

これらで見つかり、修正したものは次のとおりである。

| 項目 | 以前 | 修正後(Chromeと同じ) |
| --- | --- | --- |
| `Temporal` | なかった | V8を`v8_enable_temporal_support`でビルドし、temporal_capiとICU4XのRustのクレートをリンクする(ブラウザはRustの部分のオブジェクトファイルごと、V8だけをリンクするプログラムは同じもののアーカイブから要るものだけをリンクする) |
| HLSとMPEG-2 TS | `canPlayType`がHLSに空を返し、MSEが`video/mp2t`を受け付けなかった | メディアライブラリのHLSのデマルチプレクサで再生する(`port/media/hls_tessronos.cc`がプレイリストとセグメントを取得して渡す)。MPEG-2 TSのストリームパーサを入れる |
| ウインドウのグローバルプロパティの並び | 実行時の機能で入るインタフェースが`console`の直後に逆順で並んだ | コンテキストのスナップショットを使うChromeと同じ並び。スナップショットに入る型(Window、WindowProperties、HTMLDocument、Document、Node、EventTarget)のテンプレートを、すべての実行時の機能を無効にして作り、実行時の機能のものはコンテキストごとに後から入れる(`V8ContextSnapshotImpl`とパッチ`context_snapshot_emulation.patch`、`local_window_proxy.patch`) |
| `window.chrome`の位置 | コンテキストの準備が済んだ後に入れていた | コンテキストを作った直後(`DidCreateScriptContext`)に入れる |
| `Referer` | ネットワークのレイヤが要求のヘッダの`Referer`を捨て、どの要求にも付かなかった | Blinkが決めたリファラとリファラポリシーを、スタックの要求そのものに渡す(`br_loader.cc`、`net_fetch_tessronos.cc`) |
| iframeの中の`SharedWorker` | ページの最上位のオリジンでworkerを作り、ほかのオリジンのiframeではスクリプトを読めなかった | workerを作った文書のオリジンで作る |
| iframeを`about:blank`に移したとき | 移した文書のオリジンを渡さず、別のオリジン(不透明なオリジン)になった | 移した文書のオリジンを受け継ぐ |
| Paint Timing、Largest Contentful Paint | 項目が出なかった | 描画したフレームを画面に出した時刻を`NotifyPresentationTime`の応答にする。ウィジェットのないフレームでも計測を続ける(パッチ`paint_timing.patch`) |
| `longtask` | 項目が出なかった | ページごとにタスクの長さを監視し(`AnimationFrameTimingMonitor`)、50ミリ秒以上のタスクをページのフレームのタイムラインに入れる |
| `long-animation-frame` | 項目が出なかった | フレームの始め(`requestAnimationFrame`のコールバックの前)、スタイルとレイアウトの始め、描画を終えた時刻を、ウィジェットが通知するのと同じく同じモニタに通知する。フレームのタスクの終わりと描画を終えた時刻は、ウィジェットのないフレームのためのフック(`g_frame_timing_monitor_hook`、パッチ`local_frame.patch`、`paint_timing.patch`)でモニタに届く。50ミリ秒以上のフレームと、何も描画せずに50ミリ秒以上かかったタスクが、画面に出た時刻とともに項目になる |
| `keyboard.getLayoutMap()` | ほかのオリジンのiframeでも応答した | 最上位のフレームか、権限ポリシーが`keyboard-map`を許すフレームだけに応答する(パッチ`keyboard_layout.patch`) |
| メモリオブジェクト(`memfd_create`)の数 | 128まで。Blobの読み出しやfetchの本文の多いページで、Mojoのデータパイプが作れなくなった | 1024まで(`lib/libcxxrt/ts_mman.c`) |

実際のLinuxのマシンどうしでも違うので、そのままにしたものがある。画面とウインドウの大きさ、プロセッサの数、`deviceMemory`、GPUの名前と限界の値とWebGLの拡張、Web Audioの`sampleRate`、時計の値の端数、再帰の深さ、音声の合成の声、`AudioContext`の初めの状態(ChromeはYouTubeのような利用の多いサイトで自動再生を許し、ほかでは利用者の操作まで`suspended`にする)である。

2026年10月10日に、次のものも修正した。

| 項目 | 以前 | 修正後(Chromeと同じ) |
| --- | --- | --- |
| 明朝体と等幅のフォント | 実身「書体」にゴシック体しかなく、`serif`と`monospace`もゴシック体だった | 実身「書体」にNoto Serif JPとNoto Sans Mono CJK JPを入れ、`serif`と`monospace`をそれに割り当てる(14.4) |
| ワーカーの`OffscreenCanvas`のWebGL | `getContext('webgl')`がnullだった | ANGLEのコンテキストを、それを使うスレッドでカレントにする(14.6.1) |
| 長いアニメーションフレーム | 項目が出なかった | 上の表の`long-animation-frame` |
| iframeの文書とService Worker | 制御するのはページの文書だけで、iframeの移動もワーカーに問い合わせなかった | 文書ごとにクライアントにし、iframeの文書も制御し、iframeの移動も`fetch`イベントにする(14.4.6) |

同じ日に、上の修正を入れたブラウザでWeb Platform Testsを再実行した。比べたのは、idlharnessの158本を含む2つの一覧(4188本、重なりを除く)で、Chromeの結果は実行済みのものを使った。QEMUのマシンを2台並べ、テストを1つずつ配るキュー(`runsrv3.py`)から両方が取り、停止したマシンは再起動する。テストのページは`10.0.2.2`から開くので、起動引数の行`secure=`でそのオリジンを安全なコンテキストとして扱う(Chromeは`127.0.0.1`から開くので、もとから安全なコンテキストになる)。サブテストの名前にあるホストとポートは、比べる前にそろえる。1回目は4188本のうち280本、1480のサブテストが違った。違いの多い分野から修正し、違ったテストだけを再実行して、166本、721のサブテストになった(同じ数え方。そのうちChromeだけが成功するものが594、こちらだけが成功するものが95)。1回目に成功せず今は成功するサブテストは745ある。修正したものは次のとおりである。

| 項目 | 以前 | 修正後(Chromeと同じ) |
| --- | --- | --- |
| Blobを本文にした`fetch`とXMLHttpRequest | 本文が空で送られた(データパイプの部分を読まなかった) | 本文のデータパイプを読み切ってから送る(`br_loader.cc`の`BodyReader`) |
| iframeへのフォームの送信 | ファイルを含む本文が空で、ASCII以外の本文はLatin-1からUTF-8に文字化けした | ファイル(Blob)の中身を読んでから送り、本文はバイトのまま送る。ページのフォームも同じくバイトのまま |
| `blob:`のURLの応答 | 型が空のBlobに`Content-Type`を付けなかった | 空でも付ける(`Content-Length`、`Content-Range`、`Content-Type`の順) |
| 応答のヘッダの値 | UTF-8として読み、ASCII以外を含む値が消えた | 1バイトを1文字として読む |
| 何度も現れるヘッダ(CORSの検査) | 最初の1つだけを見た | コンマと空白でつないだ1つの値として見る |
| XMLHttpRequestの`upload`のイベント | 起きなかった | 本文を送った後、応答の前に送った量を通知する |
| 進捗のイベントの`lengthComputable` | いつも真 | 応答に`Content-Length`があるときだけ真 |
| `statusText` | 200なら`OK`、ほかは空 | ステータス行の理由句(HTTP/2とHTTP/3では空)。同期の読み込み(同期のXMLHttpRequest)も同じ(`br_net_fetch_sync_status_text`) |
| リダイレクト | ネットワークスタックがBlinkに伝えずにたどり、`Response.redirected`、`Response.url`が元のままで、`redirect`の`manual`と`error`が効かなかった | たどったリダイレクトをBlinkに1つずつ伝える(`WillFollowRedirect`)。`error`は失敗にし、`manual`は最初のリダイレクトの応答を不透明なリダイレクトにする。`manual`と`error`の要求は、ネットワークスタックが最初のリダイレクトで止まり(読み込みのレイヤのヘッダ行`X-TessronOS-Redirect: stop`)、その先を要求しない。CORSの要求は、リダイレクトの応答もCORSで検査する(2026年10月15日まで、`manual`の要求がその先までたどられ、その先の応答をCORSで検査して拒否していた。YouTubeのページが`https://youtube.com/`へ送るリダイレクトをたどらない`fetch`が、その先の`www.youtube.com`の応答のCORSのエラーになった) |
| `fetch()`の`cache`の指定 | スタックに伝えなかった | 読み込みのフラグ(検証する、キャッシュを通らない、など)を伝える |
| 読み込みの失敗の理由 | どの失敗も`ERR_FAILED`としてBlinkに伝えた。Webフォント(`@font-face`)は最初にHTTPキャッシュだけから読み(キャッシュを見た読み込み)、`ERR_CACHE_MISS`ならネットワークから読み直すが、そのフラグが伝わらず、キャッシュにないフォントはどれも読まれなかった(Google マップのボタンのアイコンが、記号のフォントが読まれず、四角に×の形で表示された) | ネットワークスタックのエラー(`br_net_fetch_error`)をそのまま伝える(`br_loader.cc`の`Fail`) |
| 許可されないポート | 接続しに行った | ネットワークサービスと同じく直ちに拒否する(`net::IsPortAllowedForScheme`) |
| URLのスキーム | `chrome-extension:`、`isolated-app:`などを特別なスキームとして登録していなかった | Linux版のChromeと同じく登録する(ホストを持ち、オリジンを持つ、安全、アクセスなし、など) |
| `document.createEvent()` | モジュールのイベント(`StorageEvent`、`DeviceMotionEvent`など)を作れなかった | モジュールのイベントのファクトリを登録する |
| 文字エンコーディングを示さない文書 | windows-1252として読んだ | 日本語のChromeと同じくShift_JISとして読む |
| ローカル時刻(`document.lastModified`) | Cのライブラリが協定世界時のまま、9時間ずれた | システムのタイムゾーンの時差を`TZ`にしてCのライブラリに渡す |
| iframeの`blob:`の文書 | 開かなかった | Blobの中身で文書にする |
| iframeの`about:blank`と`about:srcdoc` | ベースURLを親から受け継がなかった。新しいiframeの`load`のイベントが後から起きた | 開いた文書(`srcdoc`は親)のベースURLを受け継ぐ。新しいiframeの最初の`about:blank`はその場でコミットし、`load`は挿入した時に起きる |
| iframeの`sandbox`と`allow` | 文書に効かなかった | 親の文書とiframeの属性のサンドボックスのフラグを文書のポリシー(policy container)に入れ、オリジンを切り離すときは不透明なオリジンにする。`allow`の権限ポリシーも渡す |
| ウインドウの`blob:`のURLのストア | フレームのassociated interfaceを通したので、ウインドウのスレッドが同期のXMLHttpRequestで待つと、URLの登録が届かずに止まった | ストアのスレッドへの専用のパイプにする(パッチ`public_url_manager.patch`) |
| 保存域の許可のハンドルの`createObjectURL` | 登録はハンドルのストアのパイプで、読み込みはフレームのストアのパイプで届くので、すぐに`fetch()`すると登録より先に検索され、「登録されていない」で失敗した | 解決の時に見つからないURLは、読み込みの時に検索し直し、さらに10ミリ秒ずつ50回まで登録を待つ(`blob_tessronos.cc`の`UrlLoaderFactory`) |
| `BroadcastChannel`のBlob | 受信側でnullになった | Blobのハンドルもメッセージと一緒に渡す |
| ワーカーのストレージのバケット | ワーカーに与えていなかった | ワーカーのオリジンのものを与える |
| Element Timingの矩形 | 0だった | ウィジェットのないフレームでは見える矩形をそのまま使う(パッチ`element_timing_utils.patch`) |
| サブリソースのタイミング | タイミングがなく、`Timing-Allow-Origin`の判断もなかった | 要求を渡した時刻と応答の時刻、通ったオリジンの判断を応答に付ける。プロトコル(`nextHopProtocol`)も付ける |
| `<frame>`の移動の`fetch`イベント | `destination`が`iframe`だった | `frame` |

続けて、残っていた次のものを修正した。

| 項目 | 以前 | 修正後(Chromeと同じ) |
| --- | --- | --- |
| `fetch()`とXMLHttpRequestの本文 | 全部がそろってから渡した。終わらない応答の`fetch()`は応答せず、本文を届いた分ずつ読めなかった | 届いた分ずつデータパイプに書く(14.5) |
| 同期のXMLHttpRequestの`statusText` | 空 | ステータス行の理由句 |
| iframeのセッション履歴 | iframeの移動は項目にならず、`history.length`が増えず、`history.back()`でiframeが戻らなかった | 結合セッション履歴(14.7) |
| iframeの文書のタイミング | 読み込みの種類がいつも`navigate`、`redirectCount`が0、`fetchStart`が0、`document.referrer`が空 | 移動の種類、リダイレクト、時刻、リファラをコミットに渡す(14.7) |
| ワーカーの`requestAnimationFrame` | `NotSupportedError` | 60Hzのタイミングで呼ぶ(14.4.2) |
| スムーズスクロール | すぐに行き先へジャンプした | フレームごとに進む(14.4.2) |
| ページのスクリプトの連続した`history.go()` | 前の移動が終わる前の位置から数えた | 前の移動が終わるのを待ち、そこから数える |
| 終わらないスクリプトのワーカーの`terminate()` | マシンが応答しなくなった | ワーカーが終了する(カーネルのラウンドロビン) |

これらを入れて、関係する分野(`/fetch/api/basic`、`response`、`abort`、`/xhr`、`/html/browsers/history`、`/navigation-timing`、`/resource-timing`、`/css/cssom-view`のスクロール、ワーカーの`requestAnimationFrame`の881本)を再実行した。カーネルに同じ優先度の間のラウンドロビン(タイムシェアリングの優先度16〜32を20ミリ秒ごとに切り替える)が入った後に、`Worker-terminate-forever-during-evaluation.html`(7つのサブテスト)も実行した。前の回より失敗するようになったサブテストはなく、163のサブテストが新たに成功した。4188本全体では、Chromeと違うのは129本、563のサブテストになった(Chromeだけが成功するものが434、こちらだけが成功するものが98)。1回目からは、成功せず今は成功するサブテストが908ある。

残った違いには、環境によるものがある。Chromeは`127.0.0.1`を安全なオリジンとするので、そこでは`Secure`のCookieが付くが、`10.0.2.2`のテストでは付かない。閉じたポートへの接続は、Windowsのループバックではすぐに拒否されるが、QEMUのユーザモードのネットワークでは待たされる。`www1.`のような別名のホストは、どちらでも名前解決できない。フォントが違うので、文字の幅を測るテストの値が違う。QEMUのマシンは遅いので、HTMLのパーサがタイムスライス(10ミリ秒)を使い切って途中で処理を譲り、そこでマイクロタスクが実行されて、`MutationObserver`の記録の分かれ方が違う(`MutationObserver-document.html`など。本文の渡し方によるものではない)。終わらないスクリプトを実行しているワーカーは、以前は停止できなかった。同じ優先度のタスクの間にラウンドロビンがなく、ビジーループのワーカーが4つあると4つのプロセッサを占有し、`terminate()`を2秒後に強制するページのスレッドのタイマも実行されず、QEMUではマシンの出力が止まった。カーネルに同じ優先度の間のラウンドロビンが入って、Blinkの強制終了(`TerminateExecution`)が機能するようになった。

まだ違うもので、修正していないものは次のとおりである。Widevine、ブランドの名前(`Google Chrome`はなく、`Chromium`を名乗る)、WebGLのアンチエイリアスと一部の拡張、上に挙げたWeb Platform Testsの残り。

YouTubeの再生の検証(`GenerateIT`)は、2026年10月9日の時点では応答の先頭のトークンがnullだった。上の表の修正を入れた後の2026年10月10日には、応答の先頭にトークンが返り、その後のSABRの応答の`STREAM_PROTECTION_STATUS`は1のまま本編が再生された。`GenerateIT`の要求の本文も、Chromeの1967バイトに対して1978バイトになった。検証のスクリプトの中身は調べておらず、どの修正によるものかは切り分けていない。

### 14.4.18 画面、ウインドウ、音の共有

`getDisplayMedia`は、呼ばれるたびにデータボックスのパネル20「画面の共有」を出す。パネルには、要求しているオリジン、画面全体とウインドウの一覧(手前のウインドウから順。部品11)、スイッチ「音も共有する」(部品12)、〈取消〉と〈共有〉のボタンがある。既定のボタンは安全な側の〈取消〉である。一覧は`wm_share_list`(SVC 9-39)で得る。名前のバーがあって表示されていて、ほかのウインドウに従属していないウインドウが並ぶ。ページの指定は次のように扱う。`selfBrowserSurface: 'exclude'`ならページのウインドウを並べず、`monitorTypeSurfaces: 'exclude'`なら画面全体を並べない。`displaySurface`は最初に選ぶ行を決める。

| 選んだもの | `displaySurface` | 映像 | 音 |
| --- | --- | --- | --- |
| 画面(画面が1つなら「画面全体」、2つ以上なら画面ごとに「画面 1(主画面 1920×1080)」「画面 2(DP-1 1920×1080)」) | `monitor` | `wm_grab_screen`。ビューのうち、その画面の矩形と、その上のポインタ | すべてのプログラムの音。`restrictOwnAudio`なら自分のプロセスを除く。`systemAudio: 'exclude'`なら音はない |
| 同じプログラム(ブラウザ)のウインドウ。Chromeのタブに当たる | `browser` | `wm_grab_window`。そのウインドウの作業領域 | そのプロセスの音 |
| ほかのウインドウ | `window` | 同上 | そのプロセスの音。カーネルのタスクが開いたウインドウ(デスクトップなど)には音がない |

ウインドウは、ウインドウマネージャが持つウインドウ自身のピクセルから読む(X11のCompositeの名前付きピクスマップやPipeWireのウインドウのノードに当たる)。そのため、ほかのウインドウに隠れていても正しく読める。読むのは作業領域だけで、枠、タイトルバー、ポインタは入らない(`cursor`は`never`)。ウインドウ番号は再利用されるので、番号と開いたときの通し番号の組で指定する。閉じたウインドウには`E_NOEXS`が返り、ページのトラックは終了する(`ended`)。大きさが変わるとフレームの大きさも変わる。コピーの途中で大きさが変わったときは、そのフレームを破棄する。カーネルの権限での確認はない。プログラムは利用者が選んだものだけを読むというルールで、`wm_grab_screen`と同じである。

音は、サウンドデバイスのモニタ(`DN_SDMONITOR`、レコード`OB_SND_MONITOR`、6.5)でキャプチャする。ミキサが足し合わせた音を録音のチャンネルに渡す(sndioの`mon`やPulseAudioのモニタソースに当たる)。プログラムの音量の後で全体の音量の前を取得するので、利用者が自分のスピーカーを消しても共有の音は消えない。プロセスを指定でき(`SND_MON_ONLY`、`SND_MON_EXCEPT`)、これがタブやウインドウの音になる。ハードウェアのストリームが動いているときだけモニタにフレームが来るので、音を鳴らす出力がない間、ブラウザはクロックに合わせて無音を渡す(`br_audio_sink.cc`)。ブラウザはプレイヤーのチャンネルをユニット`snda`、マイクを`snda0`で使い、モニタは`snda1`から順に使う。読むのが遅いときは新しいフレームを破棄し、録音は止めない。

画面が2つ以上あるときは、Chromeの画面の選択と同じく、画面ごとに1行を出す(主画面を先頭に、〈識別〉と同じ番号。10.4.8)。名前は、番号と、主画面か画面を映す出力の名前と、大きさである。出力が1つで画面を列に分けているとき(テストの`screens=N`)は名前を出さない。すべての画面を合わせたものは、Chromeにないので選択肢に出さない。主画面の複製を映す出力は画面に数えないので、複製のときは1行(「画面全体」)になる。画面の一覧はブラウザプロセスが、画面の実身のレコード1の`SCREENi`の行(ビューでの位置と大きさ、その画面を映す出力の名前とEDIDの識別、18.7)から作る。レンダラは一覧を読まない。選んだ画面の機器のIDは`screen:<番号>:0`で、画面ごとに違う。

キャプチャのスレッドは、選んだ画面の矩形を0.5秒ごとにブラウザに問い合わせる(`Host.ShareScreenRect`、`br_share_screen_rect`)。ブラウザは、同じ出力(名前と識別が同じもの)の画面を探し、その矩形を返す。配置や解像度が変わったときは、Chromeと同じく、新しい矩形を新しい大きさのフレームでキャプチャし続ける。ページが求めた大きさ(制約がなければBlinkの既定の2880×1800)を超えるときだけ、縦横の比を保って縮小する。フレームが共有のバッファより大きくなったときは、新しい大きさのバッファを作り直してクライアントに通知する(`OnNewBuffer`)。古いバッファは、どのクライアントも持っていなければすぐに、持っていれば返されたときに消して通知する(`OnBufferDestroyed`)。ページの`track.getSettings()`の`width`と`height`と、ビデオ要素の大きさも新しい大きさになる。共有している画面が外されたか、主画面の複製に切り替わったときは、閉じたウインドウと同じくキャプチャを終え、ページのトラックは終了する(`ended`)。出力のレイヤは構成を変えるとき、主画面のほかの出力をいったん全部外して付け直すので(`apply_locked`)、その間の問い合わせでは画面が1つに見えることがある。そのため、2回続けて(0.5秒以上の間を置いて)見つからないときに外されたとみなし、1回だけ見つからない間はフレームを送らない。ポインタは、ウインドウマネージャがビューの座標で合成した画面にあるので、コピーした矩形の中の位置に出る。ウインドウの共有は、ウインドウ自身のピクセルを読むので、ウインドウがどの画面にあっても同じである。

共有中はウインドウのタイトルの前に「画面を共有中」と出す。`CaptureController`の操作(フォーカス、拡大縮小、ホイールの転送)は扱わないと応答する。`suppressLocalAudioPlayback`は効かない。

## 14.5 ネットワーク

ネットワークスタック(`URLRequestContext`)は専用のスレッドで動き、ソケットはノンブロッキングで使う。スレッドは、スタックが読み書きを待っているすべてのソケットと、自分をウェイクアップするためのデータグラムソケット(ループバックにバインドする)を1回の`poll`で待つ。どれかのソケットの準備ができたとき、ほかのスレッドからタスクを渡されたとき(そのソケットに1バイトを送る。続けて渡されても、スレッドが受け取るまでは1回だけ送る)、スタックの次のタイマーの時刻になったときにウェイクアップし、準備のできたソケットの処理を進めて、時刻になったタスクを実行する。その回の処理の間は、ソケットの準備ができたかどうかを`poll`の結果で応答し、システムに問い合わせ直さない。待ち時間は最長0.1秒である。以前は10ミリ秒ごとにすべてのソケットを1つずつ調べていたので、届いたデータグラムを最大10ミリ秒待たせていた。

名前解決は、スタックのDNSクライアントが、システムのネームサーバにデータグラムソケットで問い合わせる。ネームサーバは、`so_getdns`で得るIPv4のものと、システムの実身のレコード7の`DNS6`の行(設定したもの、次にルータが通知したもの、9.3.4)のIPv6のもので、この順に問い合わせる(`port/net/br_sysnet.cc`)。ネームサーバは、Chromiumのシステムの設定を読む仕組み(`DnsConfigService`)の移植(`port/net/net_platform_tessronos.cc`の`DnsConfigServiceTessronOS`)が読み、ネットワークが変わるたびに読み直す(14.5.3)。アドレス(AとAAAA)と同時に、`https`のURLのホスト名ではHTTPSレコード(RFC 9460)も問い合わせる。ネームサーバがないとき、またはDNSクライアントで解決できないときは、1件ごとに別のスレッドで`getaddrinfo`を呼び、IPv4とIPv6のアドレスをすべて、システムが返す順に受け取る(9.3.2)。ページのサブリソースは並行して取得し、ページを表示するのはサブリソースがそろってから(最長30秒)である。

- TCPの接続のプールの上限はChromiumの既定のまま(ソフトの上限256と、乱数による同じ数までの上乗せ)で、それを超える要求はソケットが空くのを待つ。ソケットの数の上限は、システム全体の上限(このビルドで4096個、最後の32個はカーネル用)と、プロセスごとの上限(ソフトの上限1024)の2段である(9.2.2、14.13.8)。
- TLSはChromiumのBoringSSLで、サーバの証明書はChromiumのルートストアの証明書(実行ファイルに埋め込む)で検証する。期限の切れた証明書のサイトは開かない。
- Cookie、`localStorage`、HTTPキャッシュは、実身「かな漢字変換の学習」にある実身「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」のレコード1に格納する(14.8)。Cookieと`localStorage`は変わってから2秒後、キャッシュは新しいエントリが来てから1分後に保存し、終了時に残りを保存する。HTTPキャッシュはエントリ単位で、新しいエントリと変わったエントリ(ストリームの大きさか、ヘッダとメタデータのハッシュが変わったもの)、消えたエントリだけを書く。書込みはストアのスレッドが行う(14.8.1)。
- フォームの`POST`は、本文とその種類をアプリが受け取り、ネットワークスタックで送る。
- スクリプトの`fetch`と`XMLHttpRequest`の応答の本文は、Mojoのデータパイプで渡す。`fetch()`の`Response`は本文をストリームとしてしか読まないためである。応答は100ミリ秒だけ保留し、その間に本文が終われば、応答と本文を1つのタスクでまとめて渡す(すぐに終わる応答が、それまでと同じ順でタスクに入る。長いタスクの後のXMLHttpRequestの`timeout`の判定など)。終わらなければ、その時点で応答とパイプを渡し、本文はネットワークスタックから届いた分ずつパイプに書く(`BrURLLoader`の`PipedPart`と`Release`、パイプは512KB)。パイプが一杯のときは、書けなかった分を保持しておき、パイプが書けるようになったら書く。そのため、ページは本文を届いた分ずつ読め(`ReadableStream`、XMLHttpRequestの`progress`)、動画を`fetch()`で少しずつ受け取るページ(YouTubeのSABR)も、全部が届く前に再生に回せる。終わらない応答でも、応答が来た時点で`fetch()`が結果を返す。ページが本文の読み取りをやめたとき(`reader.cancel()`)と、ローダが破棄されたとき(`AbortController`)は、スタックに取得を中止させる。こうした要求は、応答が来た時点でサブリソースの待ちから外す。届いた分ごとにページのスレッドのタスクが1つ増えるので、動画を再生し続けるページではタスクキューが空にならない。そこで、エンジンのスレッドがタスクを実行するのはキューが空になるまでか、長くても100ミリ秒までとし(`run_tasks`)、残りは次の回に回す。そうしないと、スレッドがフレームと入力とサブリソースの待ちに戻らず、YouTubeのページの表示が4分ほど遅れた。データパイプの共有メモリは、`lib/libcxxrt`の`memfd_create`が作る名前のないメモリの実身で(5.7、8.3)、パッチ`shared_memory_region.patch`と`shared_memory_tracker.patch`でそれを使う。同じ領域のマッピングはどれも同じアドレスになる。
- `indexedDB`は、BlinkのIndexedDBのページ側(要求、トランザクションの進行、値からのキーの取出し、値のラップ)をそのまま使い、Chromiumではブラウザがプロセスの外に持つバックエンド(Mojoの`IDBFactory`、`IDBDatabase`、`IDBTransaction`、`IDBCursor`)を、`port/blink/indexeddb_tessronos.cc`がプロセスの中に持つ。ページが空のブローカに`IDBFactory`を求めると、そのページのオリジンのバックエンドがバインドされる。データはメモリにあり、オリジンごとのデータベース、データベースごとのオブジェクトストア、ストアごとのレコードとインデックスからなる。キーは、並びがIndexedDBのキーの順(数、日付、文字列、バイナリ、配列)になるバイト列にして持つので、レコードとインデックスの並びがそのままカーソルの順になる。書込みのトランザクションは最初に書く前にデータベースのコピーを取り、中断されるとコピーに戻す。トランザクションは互いを待たず、要求が来た順に応答する。データベースを開く処理と削除する処理は、Chromiumのconnection coordinatorと同じく、データベースごとに要求された順に1つずつ行う。開く要求は、そのデータベースのバージョン変更のトランザクションが動いている間は待ち、終わってから、アップグレードが作ったオブジェクトストアを含む状態で応答する。アップグレードと削除は、ほかの開いている接続に`versionchange`を通知し、それらが閉じるまで待つ。ページがその接続を閉じずにおくと(`VersionChangeIgnored`)、要求した側に`blocked`を通知する。以前は、アップグレードの最中に来た同じバージョンを開く要求に、まだストアのない状態で応答していた。ページの別々の部分が同じデータベースを続けて開くZoomのWeb版では、2つ目の接続の`transaction()`が`NotFoundError`(One of the specified object stores was not found)で失敗していた。削除も、開いている接続を強制的に閉じて(`ForcedClose`)すぐに消していた。何かを変えたトランザクションがコミットされると、2秒後に全体を「ブラウザの保存域」のレコード2に書き、起動時に読み戻す。BlobやFileを含む値(大きな値もBlinkがBlobにラップして渡す)は、値のバイト列の前にその外部オブジェクトの番号を付けて持つ。外部オブジェクトは番号ごとに、ページが渡したBlob(保持し続け、値を返すたびに複製して渡す)、種類、大きさ、Fileなら名前と時刻、Blobのバックエンドから後で読み取った中身を持つ。データベースを書くときは中身も書き、読み戻した中身は、それを含む値を最初に返すときにBlobに作り直す。File System Accessのハンドルを含む値は受け付けない。
- `WebSocket`は、BlinkのチャネルをMojoでネットワークサービスに接続する代わりに、`port/blink/websocket_tessronos.cc`のチャネル(パッチ`dom_websocket.patch`で`DOMWebSocket`が作る)が、ネットワークスレッドでネットワークスタックの`net::WebSocketChannel`を動かす。開くときのハンドシェイク、フレームの組立てとマスク、閉じるときのハンドシェイクはスタックが行い、TLS、証明書の検証、Cookieは通常の要求と同じものを使う。メッセージはネットワークスレッドでフレームから組み立て、まとめてページのスレッドに渡す。Blobを送るときは、Blobのバックエンドからその中身を読み取ってからバイナリのメッセージとして送る。読み取る間にページが送ったものは、そのBlobの後ろで待たせ、送った順を保つ。`WebSocketStream`も同じチャネルで動く(パッチ`websocket_stream.patch`)。
- `URL.createObjectURL(blob)`は、ChromiumではBlobの公開URLを`BlobURLStore`に同期のMojo呼出し(`Register`)で登録する。このブラウザではレシーバをバインドする処理がページのスレッドの後のタスクとして残るので、同期の呼出しはそのタスクを待って永久に戻らず、Googleマップの読込みが止まった(14.7のエンジンのスレッドの待ちの記録で分かった)。パッチ`public_url_manager.patch`で、`Register`を応答を待たずに送り、URLはそのまま作る。Blobの中身とURLの解決は14.4.4のバックエンドが持つ。
- 画像の`decode()`は、`ChromeClient`の`RequestDecode`にデコードを要求し、応答を待つ。基底の`ChromeClient`はその場で「できない」と応答するが、画像のローダは要求のキューを処理しながら要求するので、その場の応答によって処理中のキューから要求が消え、プロセスが落ちた(Googleマップ)。このブラウザの`ChromeClient`は、画像を描くときにデコードするので「できる」と応答し、その応答を後のタスクとして渡す(`br_blink.cc`)。
- canvasの描画の時間を計る`CanvasPerformanceMonitor`は、描画のあったタスクの終わりで計測を終えることを前提にし、次のタスクの始めまで計測が残っていると止まる(`NOTREACHED`)。このブラウザは、フレームの始め(`requestAnimationFrame`のコールバック)と、入力をページに渡す処理を、スケジューラのタスクの外で行うので、そこでcanvasに描くページ(Googleマップ)で止まった。パッチ`canvas_performance_monitor.patch`で、タスクの外の描画の計測は捨てて次のタスクを始める。
- `EventSource`の要求(`Accept: text/event-stream`)は、応答の本文を全部そろえてから渡す通常の取得ではなく、応答が来た時点で応答を、そのあと本文を届いた分ずつBlinkに渡す(`br_net_fetch_stream`)。開いたままのストリームがページの表示を待たせないように、応答が来た時点で未完了の要求の数から外す。`EventSource`を閉じると接続を切る。
- IPv6のアドレスにも接続する。ソケット(`port/net/tcp_socket_tessronos.cc`、`br_stream.cc`)は接続先のアドレスファミリで開き、IPv6のソケットは`IPV6_V6ONLY`にする。スタックがホスト名のIPv4とIPv6の両方を求めるとき、`getaddrinfo`には`AI_ADDRCONFIG`を付けるので、外部に到達できるIPv6のアドレス(リンクローカルでもサイトローカルでもないもの)を持たないマシンではAAAAを問い合わせず、IPv4だけになる。両方のアドレスがあれば、Chromiumの接続の手順(`TransportConnectJob`)がIPv6のアドレスに先に接続を始め、300ミリ秒たっても接続できなければIPv4のアドレスにも接続を始めて、先に接続できた方を使う(Happy Eyeballs)。IPv6の接続がすぐに失敗したときは、待たずにIPv4に移る。`http://[::1]:8080/`のようなIPv6のアドレスのURLも開ける。スタックがIPv6に到達できるかを調べるときは、UDPのソケットを外部のIPv6のアドレスに接続して、そのソケットのアドレスを見る。DNSクライアントで問い合わせるときも、この調査でIPv6に到達できないと分かったマシンではAAAAを問い合わせない。
- HTTP/3(QUIC)を使う。サーバがHTTP/3を使えることは、応答の`Alt-Svc`か、ホスト名のHTTPSレコードの`alpn`で知る。HTTPSレコードで知ったときは最初の要求から、`Alt-Svc`で知ったときはそれ以後の要求で、スタックはQUICの接続をTCPの接続と競わせ、先に確立した方で送る。TCPの接続は、QUICの名前解決が済んでから、そのサーバの往復時間の1.5倍(分からなければ0.3秒)だけ遅れて始める。QUICがこのマシンのアドレスで動いた記録がなければ遅らせない。QUICの接続ができなければ、そのサーバのHTTP/3はしばらく使えないものとして記録され、要求はTCP(HTTP/2かHTTP/1.1)のままで送る。この切替えと復帰はChromiumの仕組み(`HttpStreamFactory`の要求の競合と`HttpServerProperties`の記録)がそのまま行う。HTTPSレコードにECHの設定があれば、TLSのClient Helloを暗号化する(ECH)。QUICの接続は開いたソケットを使い続け(ポートマイグレーションはしない)、ハンドシェイクを15秒まで待つ。
- サーバについて知ったこと(HTTP/3を使えるサーバと`Alt-Svc`の期限、サーバごとの往復時間、QUICが最後に動いたときのマシンのアドレス、使えないと記録したQUIC、HTTP/2を使えるサーバ)と、QUICの接続を再開するためのセッションチケット(TLSのセッション、サーバのトランスポートパラメータ、HTTP/3の設定、アドレス検証のトークン)は、「ブラウザの蓄え」のレコード2に格納し、次に起動したブラウザが読み戻す。前者は、`HttpServerProperties`が書き出す形式(JSON)のまま持つ。チケットはQUICのセッションプールのセッションキャッシュにあり、キャッシュはチケットを渡すときに消す(チケットは1回だけ使う)ので、HTTP/3で応答したサーバと読み戻したサーバのチケットを1つずつ取り出してコピーし、すぐに戻す。予約された番号のトランスポートパラメータ(GREASE)は書き出さない。セッションキャッシュは、プールがfriendとして内部へのアクセスを許しているクラス(`net::test::QuicSessionPoolPeer`)を通して取り出すので、Chromiumのソースは変えていない。ネットワークから新しい応答が来てから10秒後と終了時に書き直す。読み戻したブラウザは、最初の要求からQUICの接続を競わせ、チケットで接続を再開して完全なハンドシェイクを省く。
- QUICのUDPのソケット(`port/net/udp_socket_tessronos.cc`)は、Chromiumの`UDPSocketPosix`を、システムのノンブロッキングのデータグラムソケット(`socket`、`connect`、`recvfrom`、`sendto`)で実装したものである。すぐに読み書きできないときは、要求をソケットの待ちとして残し、ネットワークのスレッドがTCPのソケットと一緒に待つ。QUICのパケットリーダには、待っているデータグラムを1回の読出し(`ReadMultiple`、機能`QuicUseReadMultiple`を有効にする)でまとめて渡す。システムには複数のデータグラムを1回で受け取る呼出し(`recvmmsg`)がないので、移植レイヤが`recvfrom`を続けて呼び、64KBのバッファに詰める。セグメンテーションのオフロード(GSO)、受信のまとめ(GRO)とフラグメント化禁止のフラグは使わない。ECNは扱う。送るデータグラムのマークはQUICが求めるとおりに`IP_TOS`(IPv6は`IPV6_TCLASS`)で付け、QUICが受信したマークを求める(`SetRecvTos`)と、ソケットは`IP_RECVTOS`(`IPV6_RECVTCLASS`)を1にして、データグラムを`recvmsg`で読み、補助データのTOSをQUICのパケットリーダに渡す(9.2)。QUICはそれを数えてACKのフレームでサーバに伝える。受信バッファは、QUICが求める1MBをシステムが受け付けなければ、受け付けるまで半分にして求め直す。スタックのバッファが一時的に足りないときの送信の失敗は`ENOBUFS`になり、QUICはしばらく待って送り直す。

### 14.5.1 WebTransport

`new WebTransport(url)`は、ネットワークスタックのHTTP/3(QUIC)の上のWebTransportのクライアント(`net::CreateWebTransportClient`)を使う。Chromiumではネットワークサービスがその接続とページの側(セッション、ストリーム、データグラムのMojoのインタフェース)を接続するが、このブラウザでは`port/blink/web_transport_tessronos.cc`がネットワークスレッドで接続する。

- ストリームのバイトは、ページのデータパイプとQUICのストリームの間で、双方が受け取れるだけ渡す。ストリームの終わり(FIN)、リセット、送信の停止の要求は両方向に伝える。ページが作った双方向と単方向のストリーム、サーバが開いたストリームのどちらも扱う。
- データグラムは、セッションが送れるときに送り、処理されたことをページに通知する。
- サーバの証明書は、ページがSHA-256のハッシュで指定したもの(`serverCertificateHashes`)でもよく、その照合はスタックが行う。
- 応答のヘッダは、Cookieを除いてページに渡す。
- ネットワークスレッドでMojoのメッセージを受けられるように、そのスレッドはシーケンスごとのストレージの表(`SequenceLocalStorageMap`)を持つ。

テストは、ホストで動かすWebTransportのエコーサーバ(`tools/browser/wt_echo_server.py`、aioquicを使う)と`NET=user`で行う(14.10)。

### 14.5.2 WebRTC

`RTCPeerConnection`はChromiumのWebRTC(`third_party/webrtc`)とBlinkのモジュールをそのまま使う。WebRTCのスレッドはエンジンのスレッドの上で動く(`components/webrtc`の`ThreadWrapper`)。ネットワークとの間の送受信は、Chromiumではネットワークサービスの`P2PSocketManager`であるが、このブラウザでは`port/blink/p2p_tessronos.cc`が担当する。

- マシンのインタフェース(`GetNetworkList`、システムの`so_getifaddr`が返すIPv4のアドレスとマスク)を、ページが求めたときに通知する。ネットワークがなくてもマシンの中の接続ができるように、ループバックの候補も集める(`Platform::AllowsLoopbackInPeerConnection`)。
- ソケットはネットワークスタックのデータグラムソケットで、求められたアドレスと範囲のポートにバインドし、ネットワークスレッドで読み書きする。受信したものは届いた順にページに渡し、送ったパケットはそれぞれ送信済みと通知する。ICE、STUN、TURN、DTLS、SRTP、SCTP(データチャネル)はすべて、このソケットの上でWebRTCが行う。
- TCPのソケット(TCPの上のICE、TCPとTLSの上のTURN、Googleの`ssltcp`の候補が使う擬似TLS)は、Chromiumのネットワークサービスのもの(`services/network/p2p`の`P2PSocketTcp`と`P2PSocketStunTcp`)をそのままコンパイルして使う。ストリームの上のパケットを、RFC 4571の長さのプレフィックスか、STUNとTURNの区切りで分けるのはそれらである。ページの側のインタフェース(Mojoの`P2PSocket`と`P2PSocketClient`)は同じパイプのまま、ネットワークのスレッドで、Blinkの型でない`network::mojom`の型にバインドする。接続はネットワークスタックの接続の手順(`network::ProxyResolvingClientSocketFactory`。プロキシの設定、TCPのソケット、TLSとブラウザの証明書の検証)で作る。UDPが通らないネットワーク(社内のネットワークなど)では、MeetのようなサイトはTCPかTLSの443番のTURNに切り替えて接続する。
- ページが自分のアドレスを隠すための名前(mDNSの`<uuid>.local`、`MdnsResponder`)は、ブラウザがプロセス全体で保持し、その名前の名前解決にはそれで応答する。同じマシンの2つの接続は、これで互いを見つける。名前をネットワークに通知することはしない。ほかの名前はネットワークスタックの名前解決で引く。
- このブラウザは`WebLocalFrame`を使わずにBlinkのフレームを動かすので、接続を作るとき、フレームの`LocalFrame`は実行コンテキストから得て、接続を記録する仕組み(`PeerConnectionTracker`)は使わない(パッチ`rtc_peer_connection_handler.patch`)。フレームがなくても候補はこのソケットで集める(パッチ`peer_connection_dependency_factory.patch`。Chromium 153のこのファイルの宣言と定義の型の食い違いも修正する)。
- 映像のエンコードとデコードはソフトウェアで行う。WebRTCの内蔵のエンコーダとデコーダで、VP8とVP9はlibvpx、H.264のエンコードはOpenH264、デコードはFFmpeg、AV1のエンコードはlibaom、デコードはdav1dである(gnの引数の`proprietary_codecs`から`rtc_use_h264`が、`enable_av1_decoder`からWebRTCのdav1dが有効になる)。音声はOpusである。`RTCRtpSender.getCapabilities`はこの4つの映像のコーデックとOpusを返す。
- 受信した音声は、WebRTCに要求したときにだけデコードされる。ウインドウのWebRTCの音声デバイス(`blink::WebRtcAudioDeviceImpl`)から10ミリ秒ずつ取り出す(`RenderData`)と、WebRTCのミキサがそのウインドウのすべての接続の受信ストリームからその分を取り出し、受信のトラックのシンクにも渡す。Chromiumでは、要素が相手のストリームを再生するときに作るWebRTCの音声のレンダラ(`blink::WebRtcAudioRenderer`)が、オーディオサービスの出力ストリームの上で取り出す。このブラウザは`WebLocalFrame`を使わないのでそれを作れず、取り出す側がいなかった。そのため、相手の音声は聞こえず、受信ストリームのジッタバッファはあふれ、マイクのエコーキャンセラは再生した音声の参照信号を受け取らなかった(デバイスは取り出された音声を再生のシンク、つまりマイクの音声の処理にも渡す)。`port/media/rtc_audio_tessronos.cc`が、ウインドウごとのデバイスに1つ、レンダラに当たるスレッドを持つ。要素が相手のストリームを`srcObject`にすると、プレーヤは最初の音声のトラックが相手のもの(`PeerConnectionRemoteAudioTrack`)ならそのレンダラの受け手になり(Chromiumと同じ選び方)、自分ではその音声を出力しない。スレッドは、ブラウザの音声のミキシング(14.6.2)の自分のストリームとリングバッファに残る分が0.1秒より減るたびに取り出し、残る分が聞こえるまでの時間を再生の遅延として渡す(エコーキャンセラはこの遅延で参照信号を合わせる)。要素が一時停止していても取り出し続け(受信ストリームがあふれないように)、どの要素も再生していなければ無音を出力する。音量はChromiumと同じく、相手の音源ごとに、それを持って再生している要素の音量の和(10まで、なければ0)を、WebRTCのシグナリングのスレッドで音源に設定し、WebRTCがミキシングの前に掛ける。

HSTS(`Strict-Transport-Security`)で知ったことは、`BrHstsKeeper`が`TransportSecurityState`の変更を監視し、利用者ごとのストア`hsts`に10秒後に保存し、次の起動で読み戻す。ホスト名はそのSHA-256のハッシュだけを持つ。

### 14.5.3 ネットワークの切り替え

Wi-Fiで別のアクセスポイントに接続し直し、アドレス、ゲートウェイ、DNSサーバが変わったときは、ChromiumがLinuxでnetlinkの通知を受けたときと同じく、接続のプールとセッションを閉じ、名前解決の結果のキャッシュを捨て、DNSの設定を読み直す。

- `port/net/net_platform_tessronos.cc`の`NetworkChangeNotifierTessronOS`が、TessronOSの`NetworkChangeNotifier`である。ネットワークのスレッドで、スタックを組む前(`Context()`の初め)に1つだけ作り、プログラムの終わりまで持つ。作るときに今の構成を読み、専用のスレッド(スタック64KB)を起動する。スレッドはSVCの`so_netwait`(9.3.5)で、システムの構成の変更の回数が変わるまで1回あたり最長60秒待ち、変わればネットワークのスレッドにタスクを渡す。ポーリングはせず、変わらない間はブラウザのスレッドはウェイクアップしない(カーネルの中で0.2秒ごとにタイムアウトを調べるだけである)。
- ネットワークのスレッドは、IPv4のアドレス、マスク、ゲートウェイ(`so_getifaddr`)と使っているIPv6のアドレス(システムの実身のレコード7の`ADDRESS6`)、DNSサーバ(`so_getdns`とレコード7の`DNS6`)を読み直して前のものと比べる。アドレスが変わっていれば`NotifyObserversOfIPAddressChange`で通知する。これを受けて、TCPのソケットのプール(`TransportClientSocketPool`)は待ち状態の接続と使っていない接続を`ERR_NETWORK_CHANGED`で捨て、HTTP/2のセッション(`SpdySessionPool`、Linuxと同じく閉じる形)とQUICのセッション(`QuicSessionPool`)は閉じ、名前解決(`HostResolverManager`)は結果のキャッシュを捨て、進行中の問い合わせをやり直させる。外部に到達できるアドレス(IPv4のアドレス、またはリンクローカルでないIPv6のアドレス)が無くなったときと戻ったときは、接続の種類(`CONNECTION_NONE`と`CONNECTION_ETHERNET`)の変化も通知する。通知の遅延は、Linuxの`NetworkChangeNotifierLinux`と同じ値(アドレスの変化で2秒、接続の種類で0.5秒と1.5秒)にして、アドレスが外れて新しいアドレスが付く一連の変化が、`OnNetworkChanged`の1回の通知になるようにする。
- アドレスかDNSサーバが変わっていれば、`SystemDnsConfigChangeNotifier`に読み直させる(`RefreshConfig`)。DNSの設定は`DnsConfigServiceTessronOS`(Chromiumの`DnsConfigService`の移植)が、DNSサーバだけを持つ`DnsConfig`として読む。hostsファイルは無い。新しい設定は`HostResolverManager`のDNSクライアントに渡り、古いサーバで進んでいた問い合わせはやり直される。DNSサーバが無いときは、名前はシステムの名前解決で引く。前は、DNSサーバをネットワークのコンテキストを作るときに一度だけ読み、`DnsConfigOverrides`で固定していた。
- `dns=system`の行のときは、DNSクライアントを使わない名前解決(`insecure_dns_mode`が`kDisabled`)のままである。

前は`NetworkChangeNotifier`が無く、Chromiumはネットワークが変わったことを知らなかった。QEMUで切替えを再現すると(下の`test_netswitch`)、新しいアドレス10.1.2.15、ゲートウェイ10.1.2.2、DNSサーバ10.1.2.3が付いた後も、ブラウザは前のネットワークで開いたQUICのセッション(IPv6の`[2606:4700:10::ac42:93f3]:443`)で`https://example.com/`への要求を送り、要求は応答待ち(`LOAD_STATE_WAITING_FOR_RESPONSE`)のまま60秒で打ち切られた。初めて引く名前(`www.wikipedia.org`)は、DNSクライアントが前のサーバ(10.0.2.3と`[fec0::3]`)に問い合わせて応答が無く、システムの名前解決に回ってから約4.9秒で届いた。Surface Go 2で家のWi-Fiからスマートフォンのテザリングに切り替えたとき、ブラウザが「接続中」のまま進まなくなったのは、この状態である。修正した後は、ブラウザがアドレスとDNSサーバの変化を知り(`browser: net: the network changed: addresses name servers`)、新しいサーバ10.1.2.3に問い合わせ、2つの要求はそれぞれ約1.3秒と約1.1秒で届いた。

## 14.6 画像、動画、音声、WebGL

画像はlibpng(Skiaのコーデック)、libjpeg-turbo、Wuffs(GIF)、libwebp、JPEG XLとAVIF(Rust、AVIFはdav1dを使う)でデコードする。SVGとXHTMLはRustのXMLパーサで読む。

画像のアドレスを直接開いたとき(画像へのリンク)は、Blinkの画像のドキュメント(`ImageDocument`)になり、画像を暗い背景の中央に表示する。ウインドウより大きな画像はウインドウに収まるように縮め、画像をクリックすると等倍にして、クリックした所をウインドウの中央に置く。もう一度クリックすると縮めた表示に戻る。タイトルは「ファイル名 (幅×高さ)」になる。

動画と音声は、Chromiumのメディアのパイプライン(`media/`)で再生する(14.6.2)。デマルチプレクサは、アドレスで指定されたリソースにはFFmpegのもの(`media::FFmpegDemuxer`)、`MediaSource`にはChromiumのもの(`media::ChunkDemuxer`)を使う。デコーダは、H.264、AAC、MP3、Vorbis、FLAC、TheoraなどがChromiumのFFmpeg、VP8とVP9がlibvpx、Opusがlibopus、AV1がdav1dである。映像と音声の同期はChromiumの仕組みのままで、音声のレンダラが音声出力の遅延からクロックを進め、映像のレンダラがそのクロックに合わせてフレームを選び、間に合わないフレームをドロップする。アドレスで指定されたリソースは丸ごと取得してメモリから読むので、要素の`preload`が`auto`(または`autoplay`)のときだけ読込み時に取得し、そうでなければ再生が始まるまで取得しない。音声は48kHz、16bit、ステレオでサウンドデバイスの実身に書く。

Web Audio(`AudioContext`)は、Blinkのノードとグラフをそのまま使い、出力デバイス(`blink::WebAudioDevice`)を`port/media/web_audio_tessronos.cc`が持つ。デバイスのスレッドが、メディア要素と共用の音声のリングバッファ(`br_snd.cc`)をサウンドデバイスより約0.1秒先まで満たすように、10ミリ秒(480フレーム)ずつグラフをレンダリングさせ、48kHz、16bit、ステレオにして書く。コンテキストのサンプリング周波数はデバイスの48kHzで、ページが別の周波数を求めると、Blinkの出力先がそれに変換する。オシレータの波形テーブル(`PeriodicWave`)、畳み込み、アナライザに使うFFTはRustのRustFFTで、`rust.sh`がビルドする。メディア要素とWeb Audioが同時に鳴ると、同じリングバッファに交互に書かれ、音声はミキシングされない。サウンドデバイスがないとき、またはデバイスがサンプルを持ったまま、再生済みの量(`OB_SND_CNT`)が0.4秒進まないとき(ハードウェアがサンプルを取り込んでいない)は、リングバッファを送り出すスレッドが48kHzの時刻どおりにリングバッファの中身を捨てて、音を出さずに進める。リングバッファに残っている量でメディア要素のクロックが進むので、こうしないと、出力の止まったデバイスでは動画の時刻と映像も止まる。捨てている間も再生済みの量を監視し続け、進めばまたデバイスに渡す。止まったこと、戻ったことはコンソールに出す(`browser: snd:`)。マイクと出力先の選択は扱わない(許可を求める相手がなく、問合せには応答できないまま残る)。

CPUの実装(`port/webgl/gl_tessronos.cc`、`br_glsl.cc`)は、マルチサンプルのレンダーバッファ(4サンプル、回転グリッドの位置)を持つ。三角形はその内側のサンプルを覆い、フラグメントシェーダは画素ごとに1度だけ実行し(画素の中心が外ならば最初に覆ったサンプルの位置で)、深度テストとブレンドはサンプルごとに行う。1サンプルのフレームバッファへのブリットと読出しで、サンプルを平均する。描画バッファは`antialias`(既定は真)のとき、この明示的なリゾルブ(`GL_CHROMIUM_framebuffer_multisample`)で描くので、三角形の縁がなめらかになる。`make GL=1`のとき、Mesaのsoftpipeはマルチサンプルを持つと報告するだけで、縁はなめらかにならない。V3Dは本物のマルチサンプルを持つ。WEBGL_debug_renderer_infoの`UNMASKED_VENDOR_WEBGL`と`UNMASKED_RENDERER_WEBGL`は、Chromeが返す形式(「Google Inc. (ベンダ)」、「ANGLE (ベンダ, デバイス, API)」)で、CPUの実装は「Google Inc. (TessronOS)」、「ANGLE (TessronOS, TessronOS software GL, OpenGL ES 2.0)」である。

WebGLは、`make GL=1`でビルドしたときはChromiumのANGLEで描き、ANGLEはOpenGL ESのバックエンドでTessronOSのMesa(第13章 13.6)の上で動く(14.6.1)。WebGL 1と2を扱い、MesaはV3Dがあればv3d、なければsoftpipeで描く。`GL=1`でないとき、またはANGLEが起動しなかったときは、以前からのCPUの実装がWebGL 1だけを描く(`port/webgl/gl_tessronos.cc`、`br_glsl.cc`。GLES2をCPUで実行し、GLSL ES 1.00の一部を解釈する)。SwiftShader(CPUで動くVulkanとGLES)は使わない。SwiftShaderは、シェーダと描画の処理を実行時に機械語に変換して実行する(ReactorのLLVMかSubzero)。書込みと実行を交互に行うメモリ(5.3)を使えば動かせるが、LLVMかSubzeroを移植することになるので、使っていない。

### 14.6.1 WebGLとANGLE

ChromiumのWebGLは、普通はGPUプロセスの中で動く。レンダラのBlinkは`gpu::gles2::GLES2Interface`にGLの呼出しを書き、その実装(`GLES2Implementation`)が呼出しをコマンドバッファに積む。GPUプロセスのデコーダ(パススルーのデコーダ)がそれを取り出してANGLEのOpenGL ESに渡す。ANGLEはWebGLのための検査とシェーダの変換を行い、OSのグラフィックスAPIで描く。このブラウザはcontentもGPUプロセスも使わないので、コマンドバッファとデコーダを持ち込まず、Blinkが呼ぶ`GLES2Interface`を、ANGLEを同じプロセスで直接呼ぶものとして実装した(`port/webgl/gl_angle.cc`)。差し込む場所は、BlinkがWebGLのコンテキストを作るときにプラットフォームに求める`WebGraphicsContext3DProvider`で、CPUの実装もここに差し込んでいる。コマンドバッファのクライアント、`ui/gl`、GPUのサービスのレイヤ(共有イメージの管理、同期ポイントの管理、スケジューラ)はどれも要らない。

| レイヤ | 内容 |
| --- | --- |
| Blink | WebGLのコンテキスト、描画バッファ(`DrawingBuffer`)、拡張の扱い。変更しない |
| `gl_angle.cc`の`GLES2Interface` | 呼出しを1つずつANGLEのエントリポイントに渡す。コンテキストを切り替えるときだけ`EGL_MakeCurrent`を呼ぶ |
| ANGLE(`libGLESv2_thin_static`) | `GL_`と`EGL_`で始まる名前のエントリポイント、`libANGLE`、シェーダのトランスレータ(ESSLの出力)、OpenGL ESのバックエンド(`DisplayEGL`) |
| Mesa(`libtsgl.a`) | EGLのプラットフォーム`tessronos`と、OpenGL ES 3.1。softpipeかv3d |

- ANGLEのエントリポイントは`GL_ActiveTexture`、`EGL_Initialize`のように名前の先頭が`GL_`と`EGL_`なので、同じプログラムにリンクするMesaの`glActiveTexture`、`eglInitialize`と衝突しない。`gl`と`egl`で始まる名前のシンボルを定義するANGLEのファイル(`libGLESv2_autogen.cpp`、`libEGL`)はコンパイルしない。
- `GLES2Interface`の約330のメソッドのうち、ANGLEに同じ関数がある約290は、`tools/browser/gen_gl_forward.py`がChromiumのヘッダ(`gles2_interface_autogen.h`)とANGLEのエントリポイントのヘッダから転送のコードを生成する(`gl_angle_forward.inc`、`gl_angle_entry.inc`)。Chromium独自の拡張の名前のメソッドは、ANGLE、EXT、OESなどの接尾辞の同じ関数に渡す。マルチドローの要素の位置は、バイト数の配列からポインタの配列に直す。ただし、渡す先の拡張の関数がOpenGL ES 3.0のコアにもある場合は、OpenGL ES 3のコンテキスト(WebGL 2)ではコアの関数に、OpenGL ES 2のコンテキストでは拡張の関数に渡す。インスタンス描画(`DrawArraysInstancedANGLE`、`DrawElementsInstancedANGLE`、`VertexAttribDivisorANGLE`)、頂点配列オブジェクト(`BindVertexArrayOES`など)、問合せ(`GenQueriesEXT`、`BeginQueryEXT`など)、`TexStorage2DEXT`、`UnmapBufferCHROMIUM`は生成するコードで(`gen_gl_forward.py`の`CORE_IN_ES3`)、`DrawBuffersEXT`、`BlitFramebufferCHROMIUM`、`RenderbufferStorageMultisampleCHROMIUM`と問合せの開始と終了は`gl_angle.cc`の手で書いた部分で切り替える。GPUプロセスのデコーダが使うGLの関数の表も、OpenGL ES 3ではこれらの名前をコアの関数に対応付ける。`RenderbufferStorageMultisampleEXT`は`EXT_multisampled_render_to_texture`の別の関数なので、切り替えない。ANGLEは、拡張を有効にしていないコンテキストで拡張の関数が呼ばれると`GL_INVALID_OPERATION`(`Command requires an extension.`)にする。OpenGL ES 3のコンテキストでは`EXT_draw_buffers`も`ANGLE_instanced_arrays`も有効にならないので、以前はWebGL 2の`drawBuffers`とインスタンス描画が呼ばれるたびにこのエラーが残り、描画もされなかった(14.8.1)。残りのうちコマンドバッファのクライアントが自身で処理するもの(共有イメージ、拡張の要求、フラッシュ、同期トークン、WebGL 2の`getBufferSubData`)は手で書いた。どこにも渡せないもの(コマンドバッファのトレースやGPUフェンスなど)は、最初に呼ばれたときにコンソールに名前を出し、何もしない。
- コマンドバッファ独自の問合せの対象は、ANGLEに渡さずに`gl_angle.cc`が応答する。WebGL 2の`fenceSync`は、Blinkの中では`GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM`の問合せで、`clientWaitSync`と`getSyncParameter`は、その結果が出たかを問い合わせる。`KHR_parallel_shader_compile`の`COMPLETION_STATUS_KHR`も、`linkProgram`を挟んだ`GL_PROGRAM_COMPLETION_QUERY_CHROMIUM`の問合せである。以前はこれらもANGLEに渡していたが、ANGLEはこの対象を知らないので問合せは始まらず、結果はいつまでも出ず、問い合わせるたびに`getError`に`INVALID_OPERATION`が残った。ページのフェンスはシグナル状態にならず、リンクは終わらなかった。Googleマップの地図のコンポーネント(`mapcore`)は、描いた画素を`PIXEL_PACK_BUFFER`に読み出してフェンスを置き、`clientWaitSync(sync, 0, 0)`をタイマで繰り返して読み出しの終わりを待つ。その待ちが終わらず、地図のフレームの完了と表示の安定を待つ処理(マップが`ActionFlow error: hung, waiting for done(). Branches: stableViewport flowType frame_complete_event`と報告する)が止まり、続く初期化と、左上のメニューのパネルを出す処理も止まった。いまは、フェンスの問合せは終わるときにANGLEのフェンス(`glFenceSync`)を置き、それがシグナル状態になったら結果が出たと応答する(OpenGL ES 2のコンテキストにはWebGLのフェンスがないので、フラッシュした時点で出たとする)。リンクの問合せは終えた時点で出たとし、結果はそのプログラムのリンクの状態にする(ANGLEはリンクの状態を問い合わせるとリンクの終わりを待つ)。
- ANGLEのディスプレイは、OpenGL ESのバックエンド(`EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE`)を、システムのEGLをデバイスとして(`EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE`)使う形で、プロセスに1つ作る。ANGLEはシステムのEGLを`dlopen`で開いて`eglGetProcAddress`を探す。ブラウザの`dlopen`(`port/compat/ts_linux.c`)は、名前に`EGL`を含むものを開くと、リンクしたMesaを指す値を返し、`dlsym`はMesaの`eglGetProcAddress`を返す。以降のEGLとOpenGL ESの関数はすべてそれで得る。MesaのEGLは既定のディスプレイでプラットフォーム`tessronos`になり、ピクセルバッファとサーフェスなしのコンテキストを持つ。Mesaの`dlopen`など(`lib/libgl/gl_os.c`)は弱いシンボルなので、ブラウザのものが使われる。
- コンテキストはプロバイダ(`WebGraphicsContext3DProvider`)ごとに1つで、WebGL 1はOpenGL ES 2.0、WebGL 2は3.0のコンテキストである。WebGL向けの属性(`EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE`)を付けて作るので、ANGLEがWebGLの規則で呼出しを検査し、シェーダを変換する。初期化されていないリソースは0で埋める(`EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE`)。描画は1×1のピクセルバッファを現在のサーフェスにして行い、描画先はいつもフレームバッファオブジェクトである。コンテキストを作ったとき、コマンドバッファのクライアントが自分のために使う拡張(フレームバッファのブリット、テクスチャのストレージ、テクスチャのコピー、`OES_packed_depth_stencil`など。GPUプロセスのデコーダが要求するものと同じ一覧)のうち、ANGLEが出せるものを要求する。OpenGL ES 3のコンテキストでは`OES_packed_depth_stencil`はコアの機能なので、拡張の文字列にないときは追加して返す。BlinkはWebGLのコンテキストを作るときにこの名前を確認する。
- ANGLEがエラーを記録したときの説明(どの関数か、なぜか)は、コンテキストを作るときに`KHR_debug`のコールバック(`GL_DebugMessageCallbackKHR`)で受け取り、`[.WebGL-<通し番号>]GL_INVALID_OPERATION: glDrawBuffersEXT: Command requires an extension.`の形にしてコンテキストのエラーメッセージのコールバック(`SetErrorMessageCallback`)に渡す。Blinkはそれをページのコンソールに警告として出す(コンテキストごとに256件まで)。同じ説明はシステムのコンソールにも、違う説明ごとに最初の1回だけ`browser: webgl:`の行で出す。ページが`getError`で受け取ったエラーを、どの呼出しが残したかをこれで確認する。
- `EXT_disjoint_timer_query`と`EXT_disjoint_timer_query_webgl2`はページに出さない。Blinkは、GPUの機能の情報(`GpuFeatureInfo`)で`ENABLE_WEBGL_TIMER_QUERY_EXTENSIONS`が有効なときだけこれを承認済みの拡張とし、このブラウザのプロバイダはそれを有効にしていないので、開発者向けの拡張のまま隠れる。LinuxのChromeはこれを有効にしている(Androidを除く)ので、Chromeでは出る。出すと、時間の問合せはANGLEを通ってMesaの`EXT_disjoint_timer_query`に渡る。softpipeは持つが、v3dでは確認していないので、実機で確認してから有効にする。
- コンテキストは、それを描くcanvasのスレッドで使う。ページのcanvasならページのスレッド、ワーカーの`OffscreenCanvas`ならワーカーのスレッドである。ワーカーのコンテキストは、Blinkがページのスレッドに作らせて待つので、作ったスレッドと使うスレッドが違う。EGLのコンテキストは同時に1つのスレッドでしかカレントにできないので、コンテキストは作る間だけカレントにし、作り終えたら解除する。使うスレッドが最初の呼出しでカレントにする。どのコンテキストがカレントかはスレッドごとに、コンテキストの通し番号で記録する(アドレスで記録すると、別のスレッドで破棄されたコンテキストのアドレスに新しいコンテキストが作られたとき、取り違える)。以前はプロセスで1つしか記録せず、ワーカーのスレッドはページのスレッドでカレントになったコンテキストをそのまま使えるとみなしていたので、ワーカーの`getContext('webgl')`はnullを返した。
- `transferControlToOffscreen`でワーカーに渡したcanvasのフレームは、ワーカーの`OffscreenCanvas`が描くたびに、コンテキストのスナップショットとしてページに渡す(パッチ`offscreen_canvas_frame.patch`と`html_canvas_placeholder.patch`)。Chromiumでは、`OffscreenCanvas`はフレームをGPUプロセスの共有イメージ(GPUで合成しないときはソフトウェアのコンポジタの共有イメージ)としてコンポジタに渡し、ページのcanvas(プレースホルダ)はそれをレイヤーとして表示する。このブラウザはコンポジタもその共有イメージのインタフェースも持たないので、WebGLはフレームを作れず(`GetResourceForPushFrame`がいつもnull)、作れたWebGPUのフレームもページの描画では描かれなかった(`HTMLCanvasElement::Paint`はプレースホルダを描かない)。いまは、`OffscreenCanvas`のフレームごとに、WebGLは描画バッファの読み戻し、WebGPUは今のテクスチャの画素(14.6.5)、ほかはコンテキストの画像を、ラスタの`SkImage`にしてプレースホルダの番号で保持し、ページのスレッドに通知する。WebGLは、フレームを渡した後でコンポジタに渡したときと同じく合成済みとし(`MarkLayerComposited`)、描画バッファを保持しない設定なら次のフレームの描画の前に消す。WebGLの読み戻しは毎回描画バッファから読む(`GetRGBAUnacceleratedStaticBitmapImage`)。WebGLのコンテキストがリソースプロバイダのないときのために持つスナップショット(`PaintRenderingResultsToSnapshot`の`cached_snapshot_`)は、1度作ると大きさが変わるまで作り直さないので、それを使った最初のバージョンでは、2枚目からのフレームがすべて1枚目のままになり、実機のGoogleマップは地図を描いても、ドラッグやズームが画面に出なかった。ページのスレッドはそのcanvasの描き直しを求め、ページを描くときにその画像をcanvasの大きさに描く。Googleマップは地図をこの形で描く(14.8.1)。
- コンテキストごとに、5秒の間の呼出しの数、描画と消去の数、描画バッファから読んだフレームの数、`readPixels`の数(メモリとピクセルのパックのバッファ。メモリなら先頭の4バイト)、`getBufferSubData`の数と先頭の8バイト、フェンスの数と、その問合せに「終わった」「まだ」と応答した数、ページに返したエラーの数、カラーバッファの大きさを数え、使われたコンテキストについて5秒ごとに1行コンソールに出す。最初のWebGL 1と2のコンテキストを作ったときは、上限値(テクスチャの大きさ、uniformとvaryingの数、サンプル数、描画バッファの数、ユニフォームブロックの大きさなど)とフラグメントシェーダの`highp`の精度、拡張の一覧を1回ずつ出す。実機のV3DとQEMUのsoftpipeでページが選ぶ描き方が違うとき、どの値が違うかをこの行で比べる。
- ANGLEのOpenGL ESのバックエンドは、仮想化のグループ(`EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE`)が同じコンテキストを、システムのEGLの1つのコンテキストの上で切り替えて動かす。指定しなければディスプレイの全部のコンテキストが1つのグループになる。システムのコンテキストも同時に1つのスレッドでしかカレントにできないので、ワーカーとページが同時にWebGLを使うと、互いにシステムのコンテキストを奪い合い、システムの`eglMakeCurrent`が失敗して、呼出しが別のコンテキストの状態の上で動いた。新しく作ったコンテキストが返す文字列が不正なアドレスになり、ブラウザが落ちた(ワーカーの`OffscreenCanvas`とページのcanvasで同時にWebGLを使うテストページで再現した。Google マップを`gpu=on`で開いたときも、同じ所で落ちていた)。そこで、ページのスレッドのコンテキストはディスプレイのグループに、ほかのスレッドのコンテキストはスレッドごとのグループに入れる。ワーカーのコンテキストはページのスレッドで作られるので、ワーカーのスレッドで最初にカレントにするときに、そのスレッドのグループで作り直す(まだ何も描いていないので、要求した拡張を求め直すだけでよい)。システムのコンテキストは、ページのスレッドに1つ、WebGLを使うワーカーのスレッドごとに1つになる。softpipeではシステムのコンテキスト1つが約200MBを使うので、QEMUの2GBのマシンで`gpu=on`(Skiaのコンテキストがもう1つ)のままWebGLを使うワーカーのあるサイトを開くと、メモリが足りなくなることがある(4GBでは足りる)。`EGL_MakeCurrent`が失敗したときは、コンソールに1度`browser: webgl: a context could not be made current`を出し、どのコンテキストもカレントにしないでおく(呼出しにはコンテキストのないものとしてANGLEが応答する)。
- 描画バッファのカラーバッファは共有イメージとして作られる。共有イメージのインタフェースは作られたときの大きさをメールボックスごとに記録し、`CreateAndTexStorage2DSharedImageCHROMIUM`がその大きさのRGBA8のテクスチャをANGLEに作る(`ColorBuffer`)。共有イメージへのアクセスの開始と終了、同期トークンは、呼出しが返ったときには実行が済んでいるので、何もしない。
- 描いた結果は、合成をCPUで行う設定(`IsGpuCompositingDisabled`)により、描画バッファが`ReadPixels`で読み戻してcanvasのビットマップにし、Skiaがページに描く。ページをGPUでラスタ化するとき(14.7.2)は、Ganeshがそのビットマップをテクスチャにして描く。GPUから読み戻すので、v3dではキャッシュしないマッピングのBOを読むことになる。描画バッファ全体を原点から読む`ReadPixels`は表示のためのフレームとして数え、コンソールの`browser: draw: webgl`の行(14.7.2)に出す。ページのcanvasは、コンポジタのレイヤーで見せる前提(`IsComposited`)なので、Blinkは描いてもページの描画の描き直しを求めない。このブラウザはcanvasをページの記録に描くので(`html_canvas_painter.patch`)、パッチ`webgl_snapshot.patch`で、描画バッファが前のフレームの後で初めて変わったとき(`MarkContextChanged`)に、canvasのレイアウトオブジェクトに描き直しを求める。また、リソースプロバイダ(`CanvasNon2DResourceProvider`)が作れないとき、Blinkは読み戻したスナップショット(`cached_snapshot_`)を1度作ると大きさが変わるまで作り直さないので、同じパッチで、描画バッファが変わった後に描くたびに読み直す(読めなかったときは前のものを使う)。以前はどちらもGPUのプロセスの経路(14.13.10)にしかなかったので、`gpuproc=off`では、ページのWebGLのcanvasは最初に描かれた1枚のまま画面に残った。実機のSurface Go 2で、webglsamplesのAquariumのデモの間ずっと`browser: draw: webgl 1 frames`で、テクスチャの画像を読み込む前のフレームが出たままになり、テクスチャが黒く見えた。three.jsの例も同じで、テクスチャそのものは正しかった(`test_blink_gltex`)。後で、カラーバッファをMesaのBOにしてウインドウシステムに渡し、GPUで合成する(13.10)ときは、カラーバッファを作る1か所(`ColorBuffer`。BOのEGLイメージをテクスチャにする)と、描画バッファの出力(読み戻しをやめて共有イメージのまま渡す)を変える。転送の部分は変わらない。
- softpipeはマルチサンプリングを持たず、`GL_MAX_SAMPLES`を1としたままOpenGL ES 3.1を出す(Mesaのソフトウェアのマルチサンプリングの扱い)。ANGLEは、OpenGL ES 3.0の必須の形式が4サンプルを持たないとOpenGL ES 2.0に制限するので、そのままではsoftpipeでWebGL 2ができない。パッチ(`angle_es3_samples.patch`)で、システムのドライバ自身がOpenGL ES 3以上を出しているときはこの制限をしないようにした。v3dは4サンプルを持つので、この変更は影響しない。softpipeのWebGL 2で`renderbufferStorageMultisample`に2以上のサンプル数を与えるとエラーになる。描画バッファのアンチエイリアスは使わない(`kAntialiasingModeNone`)。
- ANGLEのソースのうち、gccでコンパイルできない1か所(`IntermNode.cpp`の配列の大きさ)はパッチ(`angle_intermnode.patch`)で修正した。X11、Vulkan、WebGPU、NULLのバックエンド、libpci、HLSL・MSL・WGSL・SPIR-Vの出力はコンパイルせず(`tools/browser/exclude_angle.txt`)、そこを指す名前は`blink_stubs.py`のスタブになる。どれも呼ばれない。ANGLEがGPUの情報を集めるときにPCIのデバイスを探す関数は、`gl_angle.cc`が「PCIのデバイスはない」と返す。ANGLEがDRMの`drmGetVersion`を呼ぶ箇所(デバイスファイルを開いて名前を見る)は、ファイルが開けないので実行されない。そのヘッダは`port/shim/xf86drm.h`である。
- 古いCPUの実装は残す。`GL=1`でないブラウザ(Mesaをリンクしない)では、WebGL 1をCPUで描くのに使い、WebGL 2はない。`GL=1`でも、ANGLEのディスプレイができなかったとき(システムのEGLが初期化できないときなど)は、WebGL 1をこれで描く。ANGLEのコンテキストのプロバイダは弱いシンボルで参照し(`br_blink.cc`の`BrWebGLProvider`)、リンクされていないときは呼ばない。

| ファイル | 内容 |
| --- | --- |
| `application/browser/port/webgl/gl_angle.cc` | ANGLEの上の`GLES2Interface`、共有イメージ、コンテキストのプロバイダ |
| `application/browser/port/webgl/gl_angle_forward.inc`、`gl_angle_entry.inc` | 生成した転送のコードと、ANGLEの拡張のエントリポイントの宣言 |
| `application/browser/port/webgl/gl_cmdbuf.cc` | `gpuproc=on`のときの、GPUのプロセスのコマンドバッファのWebGLのプロバイダ(14.13.10) |
| `tools/browser/gen_gl_forward.py` | 上の2つを生成する |
| `tools/browser/labels_angle.txt`、`exclude_angle.txt` | コンパイルするANGLEのgnのラベルと、外すソース |
| `tools/browser/patches/angle_intermnode.patch`、`angle_es3_samples.patch` | ANGLEへのパッチ |
| `application/browser/port/compat/ts_linux.c` | システムのEGLを返す`dlopen`と`dlsym` |
| `application/browser/port/compat/ts_libc.c` | Cライブラリの浮動小数点数と文字列の変換を一度に1つだけ動かすラッパ(下) |

ワーカーとページのスレッドが同時にWebGLを使うと、両方のスレッドで数値と文字の変換(`printf`の`%g`や`strtod`。ANGLEのシェーダの変換もこれを使う)が同時に動く。プログラムのCライブラリ(newlib)は、スレッドごとの作業領域を持たないビルドで、変換に使う多倍長整数のフリーリストとdtoaの直前の結果をプログラム全体で1つ持つので、2つのスレッドが同時に変換するとフリーリストの中身がおかしくなり、`_Balloc`で落ちた。ブラウザは、変換のエントリ(`_dtoa_r`、`_ldtoa_r`、`_strtod_r`、`_strtod_l`、`strtod`、`strtod_l`、`strtof`、`strtof_l`、`_strtodg_l`)をリンカの`--wrap`でラップしてリンクする(`tools/browser/build.sh`)。ラップした関数は1つのロックを取って本物を呼び、dtoaの結果は、ロックを解放する前に呼出し元のスレッドの領域にコピーして返す(`printf`はその後ろに精度の分まで0を書き足すので、その分の大きさを確保する)。ラップせずにリンクするプログラム(`v8prog`など)では、ラップした関数は呼ばれず、`--gc-sections`で捨てられる。

ブラウザを起動してWebGLのコンテキストを最初に作ったとき、コンソールに次の行が出る。

| 行 | 意味 |
| --- | --- |
| `browser: webgl: ANGLE 1.5 (ANGLE 2.1.1 ...) on Google Inc. (Mesa)` | ANGLEのディスプレイがMesaのEGLの上にできた |
| `browser: webgl: WebGL 1 on ANGLE, ANGLE (Mesa, softpipe, OpenGL ES 3.1)` | WebGL 1のコンテキストができた。括弧の中はMesaのドライバで、V3Dで描くときは`softpipe`の代わりにv3dのレンダラ名(`V3D 7.1`で始まる)になる |
| `browser: webgl: WebGL 2 on ANGLE, ...` | WebGL 2のコンテキストができた |
| `browser: webgl: WebGL N not on ANGLE (理由)` | コンテキストができなかった。WebGL 1ならCPUの実装に戻る |
| `browser: webgl: no WebGL 2 (it needs ANGLE: make GL=1)` | WebGL 2を求められたが、ANGLEがない |
| `browser: webgl: <名前> is not given to ANGLE` | ANGLEに渡せない`GLES2Interface`のメソッドが呼ばれた(一度だけ) |
| `browser: webgl: [.WebGL-N]GL_INVALID_OPERATION: <関数>: <理由>` | ANGLEがコンテキストNにエラーを記録した。説明ごとに最初の1回だけ出す |
| `browser: webgl: context N (WebGL 2, worker) in 5 s: 509 calls, 0 draws, 410 clears, 1 frames, ...` | コンテキストNの5秒間の数(上)。`page`はページのスレッド、`worker`はワーカーのスレッドのコンテキスト |
| `browser: webgl: limits of WebGL 2: texture 16384, ...` と `browser: webgl: extensions of WebGL 2: ...` | 最初のコンテキストの限界値と拡張の一覧 |

ANGLEのディスプレイの前に、Mesaのプラットフォームが`egl:`の行でV3Dを使うかどうかを出力する(第13章13.8.3)。`egl: the V3D (v3d), ...`なら、WebGLの行の括弧の中は`Broadcom, V3D 7.1.7.0, OpenGL ES 3.0`になる。V3Dの確認が通らなかったときと、V3Dのないマシンでは、softpipeで描き、ページは同じように表示される。起動時の行`gpufake=<パス>`は、DRMシムに実機のV3D(x64ではKaby Lake GT2のIntelのGPU)をエミュレートさせ(ジョブは描かずに終わる)、ローダからv3dドライバ(x64ではiris)のscreenの作成までをQEMUで実行してから、確認の失敗でsoftpipeに戻す。終わりにプラットフォームの選んだドライバと進んだ段階(`driver=v3d step=6`、x64では`driver=iris step=6`)をそのパスに書く(`test_blink_v3dfall`)。irisはi915の要求をCライブラリの`ioctl`で出すので、ブラウザの`ioctl`(`port/compat/ts_linux.c`)はGPUのディスクリプタへの要求をDRMシムの`ts_gpu_fd_ioctl`に渡す(第13章13.11.9)。

GLを使うプロセス(`gpuproc=off`ではレンダラ、`gpuproc=on`ではGPUのプロセス)は、GLを初期化した直後に`browser: gpu: OpenGL ES:`(GPUのプロセスでは`browser: gpu process: OpenGL ES:`)の行で、プラットフォームが選んだドライバと、softpipeに戻ったときの段階と理由を出力する。`egl:`の行と同じ内容で、実機のログでブラウザの行だけを抜き出したときにも理由が分かるようにした。softpipeにフォールバックする条件と、v3dドライバの要求をシステムがどこまで受け付けるかの表は第13章13.8.4にある。

`GLES2Interface`のANGLEへの呼出しは、どれも時間を計測し(`gl_angle.cc`の`Timed`)、スレッドごとに累計する。エンジンのスレッドのループの1周の内訳(14.7)の`WebGL`がこの時間である。

### 14.6.2 メディア要素のプレーヤとMedia Source Extensions

Chromiumでは、メディア要素のプレーヤ(`blink::WebMediaPlayerImpl`)が`content`レイヤのコンポーネント(フレームのクライアント、コンポジタ、mojoのサービス)の上でパイプラインを動かす。このブラウザは`content`レイヤを使わないので、要素とパイプラインをつなぐ部分を`port/media`に書いた。パイプラインとそのコンポーネントはChromiumのものをそのまま使う。

| コンポーネント | 内容 |
| --- | --- |
| `BrWebMediaPlayer`(`media_player.cc`) | `blink::WebMediaPlayer`の実装。`media::PipelineController`と`media::PipelineImpl`を作り、再生、一時停止、シーク、再生の速さ、音量を渡し、準備の状態、時刻、長さ、大きさ、持っている時刻の範囲、デコードしたフレームとドロップしたフレームの数を要素に返す |
| メディアのスレッド | パイプライン(デマルチプレクサ、レンダラ、デコーダの呼出し)は、すべてのプレーヤが共有する1つのスレッド(`base::Thread`の`media`)で動く。デマルチプレクサ、レンダラが使うコンポーネント、ログは、パイプラインが止まった後にこのスレッドで破棄する |
| レンダラ | `media::RendererImplFactory`が作る`media::RendererImpl`と、`media::DefaultDecoderFactory`のデコーダ。CDMが付くと、暗号化されたストリームの前に`DecryptingDemuxerStream`が入る |
| 音声の出力(`br_audio_sink.cc`) | 音声のレンダラの出力先(`media::AudioRendererSink`)。48kHz、ステレオのデバイスとして応答し、自分のスレッドで、ブラウザの音声ミキサ(`br_mix.cc`)の自分のストリームとサウンドデバイスのリングバッファ(`br_snd.cc`)に残る分が0.1秒分より減るたびに1回分をレンダラに求め、16bitにして書く。求めるときに、残っている分が聞こえるまでの時間を遅延として渡し、レンダラはそれでクロックを合わせる。音を出せないときはリングバッファの中身が時刻どおりに捨てられるので(14.6)、クロックはそのまま進む |
| 映像の出力(`BrVideoSink`) | 映像のレンダラの出力先(`media::VideoRendererSink`)。アプリケーションのループのたびに、その時刻に表示するフレームをレンダラに求め、変わっていれば要素を描き直させる。描くときは、そのフレーム(`media::VideoFrame`)を、YUVの各プレーンを返すジェネレータ(`BrFrameGenerator`)を持つ画像として要素の矩形に描く記録を作る(`PaintFrame`)。ページをGPUでラスタ化するときは、GaneshがプレーンをテクスチャにしてシェーダでRGBにし(14.7.2)、プロセッサでラスタ化するときは、ジェネレータがlibyuvでRGBにする。プレーンを返せるのは8bitと10bitのI420、I422、I444で(10bitの扱いは14.6.7)、回転か反転のあるフレームとほかの形式は、今までどおり`media::PaintCanvasVideoRenderer`がプロセッサでRGBにする。同じフレームを描き直すときは同じ画像を使うので、GPUのテクスチャも使い回す。canvasやWebGLが要素を描くときは、フレームを`PaintCanvasVideoRenderer`に渡す |
| コントロール | `controls`の要素には、Blinkの標準のコントロール(`MediaControlsImpl`、`modules/media_controls`)をシャドウツリーとして付ける。再生と一時停止、シークバー、音量、ピクチャインピクチャなどのボタンは、ページの要素として描かれ、押した操作もページのイベントとして届く |
| デマルチプレクサ(`media_demuxer.cc`) | アドレスで指定されたリソースは、ネットワークスタックで丸ごと取得し、`media::MemoryDataSource`から`media::FFmpegDemuxer`で読む。FFmpegのヘッダに届くメディアのライブラリのフラグでコンパイルするため、別のファイルにした |
| ログ | パイプラインの`media::MediaLog`のエラーと警告をコンソールに出す。プレーヤは作られた順の番号を持ち、パイプラインの開始、再生、一時停止、シーク、レンダラの`BUFFERING_HAVE_ENOUGH`と`BUFFERING_HAVE_NOTHING`、終わりと、再生中または再生を待つ間は5秒ごとに、時刻、要素の準備の状態、パイプラインの状態(止まっている、シーク中、休止、安定)、持っている時刻の範囲、表示しているフレームの時刻、表示したフレームの数、デコードしたフレームとドロップしたフレームの数、音声のリングバッファの状態(デバイス、デバイスなし、止まっている、リングバッファとデバイスに残る量、デバイスが取り込んだ量、捨てた量)を1行でコンソールに出す(`browser: media <番号>:`)。実機で再生が止まったときは、この行で、時刻と範囲、フレーム、音声の出力のどこが止まったかを見る |

YouTubeなどの動画のサイトのプレーヤは、動画を数秒ずつの断片に分けて取得し、Media Source Extensions(MSE)の`MediaSource`と`SourceBuffer`で要素に渡す。`MediaSource`が要素に付くと、プレーヤは`media::ChunkDemuxer`を作ってパイプラインを始め、デマルチプレクサが開いた時点で、Chromiumの`blink::WebMediaSourceImpl`を要素に渡す。これで`sourceopen`が来る。BlinkのMSEの部分(`modules/mediasource`の18のソースと11のバインディング)と`platform/media`の`WebMediaSourceImpl`、`WebSourceBufferImpl`は、そのままコンパイルする(`blink_modules_src.txt`、`blink_modules.txt`)。バイト列の解析(MP4とWebMの`StreamParser`)、時刻の範囲ごとのフレームの管理と隙間のあるシーク(`SourceBufferStream`)、`remove`、`abort`、`timestampOffset`、追加のウインドウ、`sequence`のモード、`changeType`、画質の切替えで追加される新しい初期化セグメント、`endOfStream`は`ChunkDemuxer`の処理で、Chromiumと同じように動く。`URL.createObjectURL(mediaSource)`のレジストリ(`MediaSourceRegistryImpl`)は、modulesの初期化(`ModulesInitializer`)を通らないので、バインディングの準備(`br_bindings.cc`)で作る。

`MediaSource.isTypeSupported`、`canPlayType`、MediaCapabilitiesの`decodingInfo`は、Chromiumのメディアの判定(メディアのクライアントがないときの既定)のままに返す。`encodingInfo`は、`webrtc`の型をWebRTCのエンコーダの判定で返し(なめらかさは、ブラウザの側の`WebrtcVideoPerfHistory`が、プロセッサで間に合う大きさと速さか(854×480、毎秒31フレームまで)で返す)、`record`の型はChromeと同じく実験的な機能として受け付けない(`TypeError`)。H.264、AAC、VP8、VP9、Opus、AV1などは受け付け、HEVCはデコーダを持つマシン(Raspberry Pi 5)でだけ受け付ける(14.6.7)。AV1は、LinuxのChromeと同じく、ビルドの記述でデコーダを持つ設定(`enable_av1_decoder = true`。dav1dの`ENABLE_DAV1D_DECODER`は、AVIFの画像にも使う)にする。これで、AV1を扱えると返す箇所(`media/base/supported_types.cc`、`media/base/mime_util_internal.cc`、`media/filters/stream_parser_factory.cc`)と、AV1を読む箇所(MSEのMP4のパーサが`av01`のサンプルの記述と`av1C`を読む`media/formats/mp4/box_definitions.cc`、WebMのパーサの`media/formats/webm/webm_video_client.cc`、FFmpegのデマルチプレクサのプロファイルの`media/ffmpeg/ffmpeg_common.cc`)が、同じフラグに従う。扱えると返す箇所だけをdav1dに従わせると、YouTubeはMSEでAV1のMP4を選び、パーサが`av01`を読めずに追加が失敗して再生が止まる。メディア要素、MSE、WebCodecsの`VideoDecoder`は`DefaultDecoderFactory`のdav1dのデコーダ(`OffloadingDav1dVideoDecoder`)でデコードする。なめらかさは下の範囲で返すので、1080pのAV1は`smooth`が偽になり、YouTubeのようなサイトは大きいものにはほかのコーデックを選ぶ。AV1のエンコードは、Chromiumのソースツリーのlibaomをコンパイルしてリンクし、WebCodecsの`VideoEncoder`とMediaRecorder(`video/webm;codecs=av01`)が使う。libaomの内積命令と行列積命令を使う実装(`_neon_dotprod.c`、`_neon_i8mm.c`)は、その命令を許す`-march`でコンパイルする(`flags.txt`。libaomはプロセッサがその命令を持つと報告したときだけ呼び、Cortex-A76は内積命令を持つ)。SVEの実装はコンパイルせず、プロセッサがSVEを持たないので呼ばれない。映像はCPUでデコードするので、パッチ`media_source.patch`で、`isTypeSupported`の型の引数に大きさとフレームレート(`width=`、`height=`、`framerate=`)があるとき、854×480の画素数と毎秒31フレームを超えるものを拒否する(`br_media_source_size_ok`)。ただし、`codecs`の映像のコーデックがすべて、そのマシンのハードウェアデコーダが扱うもの(IntelのGPUのマシンのH.264の`avc1`・`avc3`、Raspberry Pi 5のHEVCの`hvc1`・`hev1`)のときは、デコーダの実身が受け付ける大きさ(幅1920、高さ1088まで)と毎秒60フレームまで受け付ける(14.6.7)。音声のコーデックは数えず、VP9やAV1などプロセッサでデコードするコーデックが混じれば今までの制限になる。実身はフレームレートの上限を返さないので、60は決めた値である。パッチ`media_capabilities.patch`で、`decodingInfo`の`smooth`をその範囲で真、`powerEfficient`を偽にする。判定は`isTypeSupported`と同じ関数(`br_media_type_decodes_in_time`)で、ハードウェアのデコーダが扱うH.264とHEVCでは、`smooth`をデコーダが受け付ける大きさと毎秒60フレームまで真にし、`powerEfficient`も真にする。Chromiumは`decodingInfo`の結果をブラウザのプロセスのデコードの記録(`VideoDecodePerfHistory`)に問い合わせるが、そのサービスはないので、問い合わせずに返す。専用ワーカーの中で作った`MediaSource`も扱う(`canConstructInDedicatedWorker`は真)。ページはその`handle`(`MediaSourceHandle`)をウインドウに送って要素の`srcObject`にし、Blinkのスレッドをまたぐ接続(`CrossThreadMediaSourceAttachment`)が、ワーカーの`SourceBuffer`をウインドウの側のプレーヤの`ChunkDemuxer`につなぐ。

Raspberry Pi 5の実機でYouTubeの動画を開くと、広告の1本目の映像が数秒動いて止まり、2本目(実は止まった1本目をプレーヤが新しい`MediaSource`で読み直したもの)は最初のフレームのまま動かなかった。YouTubeが送る再生の統計(`watchtime`の`cmt`)では、要素の時刻は最初から0.277秒で止まり、15秒分のデータを持ったまま待ちになっていた。デバイスがサンプルを取り込まなくなり、音声のリングバッファが満杯のまま、音声のレンダラのクロックが書いた分の終わりで止まったためである(0.277秒は、デバイスに渡せる7200フレームと出力側が最初に取り込む4096フレーム、合わせて0.235秒に、リングバッファに残る分を足した量に当たる)。出力のRP1のI2Sは、リンクされたブロックの有効フラグをコントローラが書き戻しでクリアするのに対して、フラグを設定し直していなかったので、DMAが1周(85ミリ秒)で止まったと考えられる。映像のレンダラは最後に聞こえた時刻から先を推定して数秒進み、そこで止まった。読み直したプレーヤは、満杯のリングバッファのために音声を一度も求められず、時刻0のままだった。修正は、I2Sの出力で済んだブロックに有効フラグを設定し直し、止まったDMAを再開させること(状態のレジスタをコンソールに出す)と、ブラウザで出力の止まったデバイスをリングバッファの中身を捨てて扱うこと(14.6)の2つである。QEMUでは音声の出力(USBオーディオ)がサンプルを取り込み続けるので、広告2本と本編が再生されていた。

動画のピクチャインピクチャ(`requestPictureInPicture`)は、ブラウザの別のウインドウに動画のフレームを描いて扱う(14.7.8)。

`requestVideoFrameCallback`は、映像の出力が新しいフレームを要素に渡したときに、待っているページに通知する。コールバックに渡す情報は、そのフレームを出した時刻(表示する時刻はその16ミリ秒後とする)、それまでに出したフレームの数、フレームの大きさとメディアの時刻、フレームの間隔の平均である。

要素の`srcObject`に`MediaStream`(canvasの`captureStream`、WebRTCの相手のストリームなど)を与えると、プレーヤはパイプラインを使わない。最初の映像のトラックにシンク(`blink::MediaStreamVideoSink`)を付け、来たフレームを映像の出力にそのまま渡す。最初の音声のトラックのシンク(`blink::WebMediaStreamAudioSink`)は、要素が再生している間、来た音声を48kHzのステレオの16bitにし、音量を掛けて音声ミキサの自分のストリームに書く。WebRTCの相手の音声のトラックは、シンクでなくWebRTCの音声のレンダラで再生する(14.5.2)。最初のフレームが来ると(映像がなければすぐに)、要素は再生できる状態になる。長さは無限、時刻は再生を始めてからの経過である。

ブラウザの中の音源(メディア要素のパイプライン、`MediaStream`のトラック、WebRTCの相手の音声、`AudioContext`、音声合成)は、それぞれがブラウザの音声ミキサ(`port/media/br_mix.cc`)の1つのストリームになり、1つのスレッドがそれらを足し合わせてサウンドデバイスのリングバッファ(`br_snd.cc`)に書く。リングバッファはサウンドデバイスの1つのチャネルで、書かれた順に再生するので、以前のようにそれぞれが直接リングバッファに書くと、2つが同時に鳴ったときに10ミリ秒ずつ交互に詰め込まれ、どちらも半分の速さで途切れて聞こえた(会議のページで、相手の声とページの効果音が重なるときなど)。ストリームは48kHz、16bitのステレオのフレームの列(1秒分まで)で、書き込む側はそれまでと同じく、自分のストリームとリングバッファに残る分(`br_mix_buffered`)を見て書く。ミキサのスレッドは、リングバッファが50ミリ秒分より減るたびに10ミリ秒分を作る。各ストリームの持つ分を足し、何も持たないストリーム(一時停止、終わり、始まる前)は無音とする。10ミリ秒分に満たないストリームは、リングバッファに20ミリ秒分より多く残っている間は待つので、書込みが少し遅れても途切れて聞こえない。どのストリームも何も持たないときは書かず、リングバッファはそれまでどおり空になってチャネルを止める。足した値は16bitの範囲にクリップする。

### 14.6.3 Encrypted Media Extensions

`navigator.requestMediaKeySystemAccess`、`MediaKeys`、`MediaKeySession`、要素の`setMediaKeys`と`encrypted`イベントは、BlinkのEMEの部分(`modules/encryptedmedia`の12のソースと17のバインディング)と、`platform/media`のCDMの部分(`WebEncryptedMediaClientImpl`、`WebContentDecryptionModuleImpl`、`CdmSessionAdapter`、`KeySystemConfigSelector`など)をそのまま使う。BlinkはEMEのエントリポイント(`blink::WebEncryptedMediaClient`)をフレームのクライアントに求めるが、このブラウザのフレームはそれを持たないので、パッチ`encrypted_media.patch`で`port/media/eme_tessronos.cc`のものを返す。それは、キーシステムの一覧(`media::KeySystemsImpl`)、CDMのファクトリ、権限の判定(暗号化されたメディアは許可し、マシンを識別する値は渡さない)を持つ。キーシステムは、組み込みのClear Key(`org.w3.clearkey`)と、利用者がWidevineのCDMを導入したときのWidevine(`com.widevine.alpha`、下記)である。CDMのファクトリは、Clear Keyにはメディアのライブラリの`AesDecryptor`を作り(`media::DefaultCdmFactory`)、WidevineにはライブラリのCDMのインタフェース(`media::CdmAdapterFactory`)で`media::CdmAdapter`を作る。

`KeySystemsImpl`は、メディアのクライアント(`media::MediaClient`)があるときだけ、Clear Key以外のキーシステムを登録のコールバック(`GetSupportedKeySystemsCB`)で問い合わせる。そのため`eme_tessronos.cc`はメディアのクライアント(`BrMediaClient`)を置く。キーシステムを返すほかは、型の可否(`IsDefaultDecoderSupportedVideoType`など)をクライアントがないときと同じように返すので、再生できる型は変わらない。登録のコールバックは保持しておき、WidevineのCDMを導入したときと外したときに、新しい一覧でもう一度呼ぶ。ブラウザを起動し直さなくても、次の`requestMediaKeySystemAccess`から結果が変わる。

要素に`MediaKeys`が付くと、プレーヤはそのCDMのコンテキストをパイプラインに渡し(`SetCdm`)、パイプラインは暗号化されたストリームを復号してからデコーダに渡す。パイプラインが始まる前に付いたときは、始まったところで渡す。ライブラリのCDM(`media::CdmAdapter`、Widevine)はシングルスレッドで、復号を含むすべての呼出しを、作ったスレッド(エンジンのスレッド)で受け、そうでなければ止まる(Chromiumはそれを専用のプロセスに置き、復号の部分はMojoで呼ぶ)。そこでプレーヤは、パイプラインにCDMのコンテキストをそのまま渡さず、復号の呼出しをCDMのスレッドに送り、その結果を呼出し元のスレッドに返すもの(`port/media/media_player.cc`の`BrCdmOnThread`)を渡す。CDMへのプレーヤの参照(`CdmContextRef`)は`BrCdmOnThread`が持つ。CDMは最後の参照を解放したスレッドで、その場で破棄される(`ContentDecryptionModule::DeleteOnCorrectThread`は`delete this`だけで、スレッドを移さない)。ページを離れて`MediaKeys`が回収された後は、プレーヤの参照が最後になる。それをパイプラインのコンポーネントと一緒にパイプラインのスレッドで解放すると、止めたパイプラインのデコーダが破棄されるときにCDMのスレッドへ送った呼出し(`DeinitializeDecoder`、`CancelDecrypt`)が届く前に、CDMがパイプラインのスレッドで破棄される。その呼出しは解放済みのCDMを呼び、レンダラが落ちていた(R1.400までの不具合。動画のページから同じサイトの別のページへ移ると、WidevineのCDMの`CdmAdapter::DeinitializeDecoder`で落ちた)。そこで`BrCdmOnThread`は、破棄されるときに参照をCDMのスレッドへ送り(`cdm_release_on`)、CDMは先に送られた呼出しの後で、自分のスレッドで解放される。パイプラインに渡す前の参照も同じく送る。CENC(`cenc`)のMP4とWebMの暗号化を扱う。

EMEは安全なコンテキスト(`https`のページ)にだけある。YouTubeの普通の動画は暗号化されていないので、Widevineがなくても再生できる。PlayReadyなど、ほかの独自のCDMのキーシステムは持たない。

#### 14.6.3.1 利用者が導入するWidevine

WidevineのCDM(`libwidevinecdm.so`)はGoogleの独自のソフトウェアで、TessronOSにもブラウザにも同梱しない。リポジトリにも入れない。利用者が自分で導入したときだけ使える。CDMは、そのプロセッサ向けにGoogleのCDMを配布している正規の配布元から、その配布形式のまま入手する。

- aarch64では、LinuxのディストリビューションがChromiumに対して行うのと同じく、Raspberry Piの公式リポジトリ(<https://archive.raspberrypi.com/debian>)がRaspberry Pi OS向けに配布しているパッケージ`libwidevinecdm0`(arm64)を使う。
- x64では、LinuxのChromeとChromiumのコンポーネントの更新(`components/component_updater`)がWidevineをインストールするのと同じく、Googleのコンポーネントの更新のサービス(<https://update.googleapis.com/service/update2/json>)が配布するWidevine CDMのコンポーネント(Linux x64のCRX3のファイル)を使う。Firefoxも、LinuxでWidevineをこのサービスから入手する(設定`media.gmp-manager.chromium-update-url`)。バージョンはサービスが決める。このエンジンのバージョンを示して問い合わせ、サービスが返したものを導入する。

CDMの中身は書き換えず、解析もしない。Chromiumの正規のインタフェースで読み込んで使うだけである。導入していなければ、`requestMediaKeySystemAccess('com.widevine.alpha', ...)`は従来どおり`NotSupportedError`で拒否される。

導入はメニューの〈Widevineを導入...〉で行う。ブラウザはまずデータボックスのパネル7「Widevineの導入」(14.8)を出す。パネルは、WidevineがGoogleの独自のソフトウェアでTessronOSに同梱していないこと、入手元、利用者の実身に入れること、利用の条件は導入する前に利用者が確認することを示す。入手元の2行(部品3と4)は、プロセッサに合わせてブラウザが書く(`br_wv_source`。aarch64はRaspberry Piの公式リポジトリ、x64はGoogleのコンポーネントの更新のサービス)。パネルとメニューは両方のプロセッサで同じものである。〈導入〉を選んだときだけ取得し、〈取消〉とEscでは何も取得しない。取得と展開は別のスレッドで行い(`port/media/br_widevine.cc`)、その間はページを使える。aarch64の手順は次のとおりである。

1. リポジトリのインデックス`dists/<配布>/main/binary-arm64/Packages`をHTTPSで取得する。配布は`trixie`、なければ`bookworm`の順に見る。インデックスのうち`Package: libwidevinecdm0`で`Architecture: arm64`のものから、Debianのバージョンの順序(dpkgと同じ比較方法)で最も新しいものを選び、`Filename`、`Size`、`SHA256`を読む。`Filename`は`pool/`で始まり、`..`を含まないものに限る。
2. パッケージ(`.deb`)をHTTPSで取得し、大きさとSHA-256(BoringSSL)をインデックスの値と照合する。合わなければ何も導入しない。リダイレクトでHTTPSでない所へ移されたときも中止する。
3. `.deb`(arのアーカイブ)の`debian-binary`が`2.x`であることを確認し、`data.tar.xz`をLZMA SDKのXZのデコーダ(Chromiumのチェックアウトの`third_party/lzma_sdk`。`tools/browser/build.sh`の`xz_lib`)で展開する。tarのアーカイブ(ustar、GNUの長い名前とpaxのパスを読む)から、`opt/WidevineCdm/`の下の`libwidevinecdm.so`と`opt/WidevineCdm/manifest.json`を取り出す。
4. `manifest.json`を、Chromiumの`components/cdm/common/cdm_manifest.cc`の`IsCdmManifestCompatibleWithChrome`と`ParseCdmManifest`で検証する。このブラウザが対応するCDMのモジュール、インタフェース、ホストのバージョン(`x-cdm-module-versions`、`x-cdm-interface-versions`、`x-cdm-host-versions`)を宣言していないCDMは導入しない。

x64では、1から3の代わりに次を行う。4から後は同じである。

1. コンポーネントの更新のサービスに、その更新の確認のプロトコル(3.1)で、Widevine CDMのコンポーネントのアプリケーションID(`oimompecagnajdejgnnjijobebaeigek`)、OSの`linux`、アーキテクチャの`x64`、このエンジンのバージョン(`prodversion`、153.0.8010.36)をHTTPSのPOSTで問い合わせる。アプリケーションIDは、Chromiumのコンポーネントのインストーラ(`chrome/browser/component_updater/widevine_cdm_component_installer.cc`)が持つ署名の鍵のSHA-256(`kWidevineSha2Hash`)の先頭16バイトを`a`から`p`の文字で書いたものである。応答の`updatecheck`が`ok`のとき、`manifest.version`(CDMのバージョン)、パッケージの`name`(`.crx3`で終わり、英数字と`._-`だけのもの)、`size`、`hash_sha256`と、`urls.url`の`codebase`を読む。
2. `https://`で始まる`codebase`に名前を足したアドレスから、応答の順にファイルを取得し、大きさとSHA-256を応答の値と照合する。合わなければ何も導入しない。リダイレクトでHTTPSでない所へ移されたときも中止する。
3. CRX3のファイルを、Chromiumの`components/crx_file/crx_verifier.cc`と同じく検証する。ヘッダ(`crx3.proto`の`CrxFileHeader`)を読み、ヘッダにZIPの終端のシグネチャ(`PK\x05\x06`、`PK\x06\x07`、`PK\x06\x06`)があれば拒否する。すべてのproof(`sha256_with_rsa`、`sha256_with_ecdsa`)の署名を、`CRX3 SignedData`と0のバイト、署名したヘッダの長さ(32ビットのリトルエンディアン)、署名したヘッダ、アーカイブに対してBoringSSLで検証し、1つでも合わなければ拒否する。proofの鍵に、SHA-256が`kWidevineSha2Hash`のもの(Widevineのコンポーネントの鍵)と、署名したヘッダの`crx_id`が示す鍵の両方がなければ拒否する。そのあと、アーカイブ(ZIP)から`_platform_specific/linux_x64/libwidevinecdm.so`と`manifest.json`を取り出す(無圧縮のままか、zlibで展開する)。マニフェストのバージョンがサービスの応答と違えば拒否する。

両方のプロセッサで、続けて次を行う。

5. 利用者ごとの普通の実身「Widevine CDM」を「ブラウザの保存域」と同じボリュームに作る。所有者はブラウザが代理するユーザ(`ob_get_crd`)、モードは`rw-------`で、レコード0にこの実身が何かを書いた文章、レコード1にライブラリ、レコード2にマニフェスト、レコード3にバージョン、入手元、SHA-256を持つ。新しい箱は作らない。「ブラウザの鍵」(14.4.7.1)と同じく、どの仮身からも指されないので固定(`ob_lnk_obj`)を1つ付ける。
6. エンジンのスレッドで、「ブラウザの保存域」のレコード10のそのユーザの行(`<ユーザのUUID> <実身のUUID>`)を新しい実身に向け、前に導入した実身があれば消す。続けてキーシステムの一覧を作り直す。結果はメッセージ行に出し、導入できなかったときは理由をパネル1で通知する。

メニューの〈Widevineを外す〉(導入していないときは灰色)は、パネル2で確認してから、そのユーザの行と実身「Widevine CDM」を消し、キーシステムの一覧からWidevineを除く。

CDMの読み込みは、最初にページがEMEを使ってキーシステムの一覧を作るときに行う。レコード10からそのユーザの実身を取得し、マニフェストを上と同じく検証してから、ライブラリのレコードをメモリに読み、`port/compat/ts_so.c`でプロセスに読み込む。これはLinuxの動的リンカと同じく、そのプロセッサ(aarch64かx86-64)のELFの共有オブジェクトのセグメントを配置し、再配置(RELR、RELA、PLT。種類はそれぞれのプロセッサのELFのABIのもの)を適用し、GNU Cライブラリの関数のシンボルを`port/compat/ts_glibc.c`の代替の関数にリンクし(要求されるバージョンはGNU Cライブラリのバージョンでなければならないが、動的リンカと同じく、weakとマークされたバージョンの要求は満たせなくてもよく、文字列テーブルの文字列は`DT_STRSZ`の外でもオブジェクトの中にあれば読む)、スレッドローカルストレージをTLSディスクリプタ(x86-64ではGNU2の形式。リゾルバの関数は`%rax`のディスクリプタを受け取り、`%fs:0`からのオフセットを返し、ほかのレジスタを保存する)か`__tls_get_addr`で与え、初期化の関数を呼ぶ。解決できないシンボルは1つずつコンソールに出す。x64のCDMはGCCのアンワインダ(`libgcc_s.so.1`、バージョン`GCC_3.0`など)に依存するので、その`_Unwind_`の関数はプロセスのもの(libgccの`unwind-dw2.c`)にリンクする。プロセスのアンワインダは、登録されたフレームの情報の中からだけFDEを探す(libgccの`unwind-dw2-fde.c`。`dl_iterate_phdr`は使わない)ので、`libgcc_s.so.1`に依存するオブジェクトは、初期化の関数を呼ぶ前に、`PT_GNU_EH_FRAME`(`.eh_frame_hdr`)のテーブルに並ぶFDEを`__register_frame_info_table_bases`で登録し、オブジェクトを解放するときに登録を外す。x86-64のglibc向けに作られたコードは、スタック保護の値を`%fs:0x28`から読むので、TessronOSのx86-64のスレッドの制御ブロックはglibcの`tcbhead_t`の先頭と同じ64バイトにし、0x28にスレッドごとの値(最下位のバイトは0)を、0x30(glibcのポインタ保護)に0を置く(`lib/libpthread/sysdepend/x86_64/pth_cpu.h`)。glibcの代替の関数は、x86-64ではミューテックスを40バイト、スレッドの属性を56バイトとし、`open`の`O_DIRECTORY`、`syscall()`の番号、`uname`の`machine`、`setjmp`と`longjmp`の`jmp_buf`をx86-64のものにする。`/proc/cpuinfo`はCPUIDから作る(ベンダ、ファミリ、モデル、ブランドの文字列、機能のフラグ。AVXとAVX2はXCR0で有効なときだけ)。動的リンカの変数`__libc_stack_end`は、メインスレッドのスタックの上端(`lib/libpthread`のメインスレッドの`pthread_getattr_np`)にする。名前は`libwidevinecdm.so`で、`dlopen`(`port/compat/ts_linux.c`)はこの名前で読み込んだものを返す。そのうえでメディアのライブラリの`media::CdmModule`が`dlopen`で開いてエントリポイント(`InitializeCdmModule_4`、`CreateCdmInstance`、`GetCdmVersion`など)を取得し、初期化する。コンソールには`browser: widevine: CDM <CDMのバージョン> loaded (manifest <バージョン>)`と出る。読み込みは1回の起動に1度で、外した後もプロセスが終わるまでメモリに残る(一覧には出さない)。

キーシステムの記述は、Chromiumの`components/cdm/renderer/widevine_key_system_info.cc`の`cdm::WidevineKeySystemInfo`をそのまま使う。エンジンはWidevineを同梱しない構成(`enable_widevine`が偽)でビルドしているので、このソースはそのままではコンパイルできない。`port/media/widevine_ks_tessronos.cc`がそのコンパイル単位の中だけ`ENABLE_WIDEVINE`を真にして取り込む。コーデックと暗号化の方式はマニフェストのもの(`x-cdm-codecs`)で、ロバストネスはソフトウェアのもの(L3)だけである。音声は`SW_SECURE_CRYPTO`まで、映像は`SW_SECURE_DECODE`までで、ハードウェアで保護するデコーダと`HW_SECURE_*`はない。セッションは一時的なものだけで、永続するライセンスは、ホストの検証とストレージのIDがないので扱わない(LinuxのChromeと同じ)。マシンを識別する値は使わない。

CDMが求めるもの(`media::CdmAuxiliaryHelper`、`port/media/eme_tessronos.cc`の`BrCdmHelper`)のうち、バッファはメディアのライブラリの`SimpleCdmAllocator`で作り、CDMが復号した映像のフレームは`BrCdmVideoFrame`が`media::VideoFrame`にする。CDMは各プレーンを、自分が決めたオフセットとストライドで、要求したバッファの容量(`cdm::Buffer::Capacity`)の中に書く。プレーンは順に並ぶとは限らない。Raspberry Pi 5のWidevineは、960×540のI420で、YとUとVのストライドを1024バイトにし、UとVを同じ行に並べて置く(Uは552960、VはUの512バイト後ろの553472から始まり、容量は830464バイト)。ライブラリの`SimpleCdmAllocator`のフレームは、YとUのプレーンのデータを次のプレーンの始まりで、Vのプレーンのデータをバッファに設定した大きさ(`Size`)で区切る。そのため、Uのプレーンのデータが512バイトになり、表示する行の分(ストライド×(行数−1)+1行のバイト数、275936バイト)より短くなる。そのフレームを描くと`VideoFrame::visible_data`の範囲の検査(`span.h`の`CHECK`)でレンダラが落ちていた(R1.420のテスト版の不具合。Raspberry Pi 5でHuluの960×540のH.264を`DecryptingVideoDecoder`で再生すると、最初のフレームをGPUのラスタ化で描くところ(`BrFrameGenerator::GetYUVAPlanes`)で、終了ステータス0x8600で落ちた)。`BrCdmVideoFrame`は、Chromeの`MojoCdmAllocator`と同じくプレーンをバッファ全体から取る。プレーンの順番や重なりは仮定せず、各プレーンにその始まりからバッファの容量の終わりまでを与え、プレーンごとに表示する行の分が収まるかだけを検査する。収まらないプレーンがあるフレームは作らず、デコーダのエラーとする。フレームの形式はCDMが設定したもの(I420、YV12、10ビットのものなど)にする。プレーンの配置は、最初のフレームと、配置が変わったときに、`browser: media: CDM frame`の行に出す(バッファの容量、CDMが設定した大きさ、各プレーンのオフセットとストライド、各プレーンが届く大きさと与えた大きさ、設定した大きさがプレーンの終わりに足りないバイト数)。テストの`test_blink_cdmframe`は、起動時の行`cdmframe=<パス>`で、544行(1088行)の配置で540行(1080行)の大きさを設定したもの、プレーンを詰めて置いたもの、プレーンがバッファからはみ出すもの、Raspberry Pi 5のWidevineと同じくUとVを並べたものの5つのI420のフレームを作り、各プレーンの表示する部分を求める。CDMのファイル(`cdm::FileIO`)は「ブラウザの保存域」のレコード11に、ユーザ、表示しているページのオリジン、ファイル名の組ごとに置く。名前は英数字と`.`、`_`、`-`からなり`_`では始まらないもので、大きさは512KBまでである。ChromeのCDMの保存と同じく、同じファイルを2つ同時に開くと`kInUse`を返す。応答はすべて後のタスクで返す。出力保護の問合せとプラットフォームの検証には「ない」と応答する。

再生の様子はコンソールに次の行で出る。

| 行 | 意味 |
| --- | --- |
| `browser: media: MediaSource attached` | `MediaSource`が要素に付いた |
| `browser: media: WxH, video, sound` | メタデータが揃った |
| `browser: media: video decoder <名前>`、`browser: media: sound decoder <名前>` | パイプラインが選んだデコーダ(`FFmpegVideoDecoder`、`VpxVideoDecoder`、`FFmpegAudioDecoder`、`OpusAudioDecoder`など)。復号してから渡すときは`(decrypting)`が付く |
| `browser: media: picture WxH` | 映像の大きさが変わった(画質の切替え) |
| `browser: media: encrypted (N bytes of init data)` | メディアが暗号化されている |
| `browser: media: MediaKeys attached` | CDMがパイプラインに付いた |
| `browser: media: waiting for MediaKeys`、`browser: media: waiting for a key` | CDMか鍵を待っている |
| `browser: media: ended at T ms` | 終わりに達した |
| `browser: media: pipeline error N`、`browser: media: status {...}`、`browser: media: message {...}` | パイプラインのエラーと、`MediaLog`のエラーと警告 |
| `browser: snd: N frames given to the sound device` | 終了時。サウンドデバイスが受け取ったフレームの数(48kHz) |

### 14.6.4 WebCodecs

WebCodecs(`VideoDecoder`、`VideoEncoder`、`AudioDecoder`、`AudioEncoder`、`VideoFrame`、`AudioData`、`EncodedVideoChunk`、`EncodedAudioChunk`、`ImageDecoder`)は、Blinkのモジュール(`modules/webcodecs`)をそのままコンパイルし、メディアのライブラリのソフトウェアのエンコーダとデコーダで動かす。

| 種類 | 使うもの |
| --- | --- |
| 映像のデコード | H.264はFFmpeg(IntelのGPUのマシンではそのデコーダ、14.6.7.1)、VP8とVP9はlibvpx、AV1はdav1d(メディア要素と同じ`DefaultDecoderFactory`)。HEVCはRaspberry Pi 5のデコーダ(14.6.7) |
| 映像のエンコード | VP8とVP9はlibvpx、H.264はOpenH264(`exclude_blink.txt`から外して組み込んだ) |
| 音声のデコードとエンコード | Opusはlibopus、AACのデコードはFFmpeg。AACのエンコードはブラウザのプロセスのプラットフォームのエンコーダを使う作りなので扱わず、`isConfigSupported`は対応しないと返す |
| 画像のデコード | `ImageDecoder`は、ページの画像と同じBlinkのデコーダ |

GPUのエンコーダとデコーダ(`MojoDecoderFactory`、ハードウェアを選ぶ`hardwareAcceleration: "prefer-hardware"`)は持たない。エンコーダは、エンコードの状況をブラウザのプロセスのメトリクスの受け手に通知する作りなので、通知を捨てる受け手を`port/media/webcodecs_tessronos.cc`が渡す。

### 14.6.5 WebGPUとDawn

ChromiumのWebGPUは、レンダラのBlinkがDawnのワイヤ(コマンドをシリアライズするレイヤ)のクライアントとしてWebGPUの呼出しをシリアライズし、GPUプロセスのデコーダがワイヤのサーバとしてDawnの実装(`dawn::native`)に渡す。DawnはシェーダのトランスレータTintを使い、OSのグラフィックスAPI(Vulkan、Metal、D3D、OpenGL ES)で描く。このブラウザはGPUプロセスを持たないので、ワイヤのサーバを同じプロセスに置き、クライアントが書いたコマンドをそのままサーバに渡す(`port/webgpu/webgpu_tessronos.cc`)。DawnのバックエンドはOpenGL ESで、WebGLと同じくTessronOSのMesa(softpipeかv3d)の上で描く。`make GL=1`のときだけ動く。

| レイヤ | 内容 |
| --- | --- |
| Blink | `navigator.gpu`、`GPUDevice`などのモジュール(`modules/webgpu`)と、canvasの`webgpu`のコンテキスト。DawnのワイヤのクライアントのC++の型で書かれている |
| `webgpu_tessronos.cc` | Blinkが求める`gpu::webgpu::WebGPUInterface`とコンテキストのプロバイダ。ワイヤのクライアントとサーバ、Dawnのインスタンス、canvasの共有イメージ |
| Dawn(`libdawn.a`) | ワイヤのクライアントとサーバ、`dawn::native`のOpenGL ESのバックエンド、関数表(`dawn_proc`) |
| Tint | WGSLの読込みと検査、GLSLの出力 |
| Mesa(`libtsgl.a`) | EGLとOpenGL ES 3.1 |

- ワイヤのクライアントがフラッシュすると、溜めたコマンドの列をその場でサーバに渡す。サーバの応答は、コンテキストをバインドしたスレッド(ページのスレッドかワーカーのスレッド)のタスクとしてクライアントに渡し、続けてクライアントのイベントを処理する。ページのコールバックが新しいスタックで実行されるようにするためである。後で終わるサーバの処理(バッファのマッピング、キューに投入した処理の完了)は、残っている間2ミリ秒ごとにDawnのイベントを処理して確認する。
- アダプタは、DawnのOpenGL ESのバックエンドに、Mesaの`eglGetProcAddress`を渡して作る(`RequestAdapterOptionsGetGLProc`)。ライブラリを開くことはしない。ディスプレイはEGLの既定のディスプレイ(プラットフォーム`tessronos`)である。DawnのOpenGL ESのバックエンドが出すアダプタは互換モード(compatibility)だけなので、ページが求めたモードによらず、互換モードのアダプタを返す。ChromiumがOpenGL ESの上でWebGPUを動かすときと同じ扱いで、アダプタは`core-features-and-limits`の機能を持たない。OpenGL ES 3.1と`GL_EXT_color_buffer_float`(3.2なら要らない)が要る。
- Dawnにアダプタを求める前に、システムのEGLがOpenGL ES 3.1のコンテキストを作れるかを、プロセスで1度だけ、そのためだけのスレッドで確認する(`SystemHasGles31`)。作れなければDawnには求めず、アダプタはないと応答する(ページの`requestAdapter()`はnullになる)。V3Dのv3dドライバは、カーネルがコンピュートのジョブを受け付けるときだけOpenGL ES 3.1になる(第13章13.3.4)。受け付けないときは3.0で、この場合に当たる。Mesaは、ドライバが持たないバージョンのコンテキストを拒否するとき、作りかけのコンテキストを呼出し元のスレッドで一度カレントにしてから破棄し、前のコンテキストはMesaのGLのレイヤでだけ戻す。以前はDawnがページのスレッドで`eglCreateContext`を呼んで拒否されていた(`eglCreateContext failed with EGL_BAD_MATCH`)。ページのスレッドではANGLEのコンテキストがカレントになっているので、その状態をWebGLの描画の途中で変えないようにした。
- Dawnは、デバイスのコンテキストをロバストアクセス(`EGL_CONTEXT_OPENGL_ROBUST_ACCESS`)付きで作る。softpipeは拡張を出すが、リセットの状態を持たないので、このコンテキストを拒否する。パッチ`dawn_context_robustness.patch`で、拒否されたときはロバストアクセスなしで作り直す。範囲外の参照は、Tintがシェーダに入れる検査で防がれる。DawnはGLのコンテキストを使うときに前のコンテキストを記録して戻すので、同じプロセスのANGLE(WebGL)とSkiaのGPUのコンテキストと衝突しない。
- canvasの`webgpu`のコンテキストは、描くテクスチャを共有イメージとして作る。共有イメージのインタフェースは画素をメモリに持ち、メールボックスをテクスチャに関連付ける(`AssociateMailbox`)と、Dawnのデバイスにその大きさと形式(`bgra8unorm`か`rgba8unorm`)のテクスチャを作り、クライアントが予約した番号でサーバに入れる。合成はCPUで行う(14.6.1)ので、ページを描くとき、canvasは今のテクスチャの画素を求める。パッチ`gpu_canvas_context.patch`がBlinkにそのインタフェース(`g_webgpu_canvas_snapshot`)を足し、`webgpu_tessronos.cc`がテクスチャをCPUの読めるバッファにコピーし、マップして読む。テクスチャの間(フレームが終わって次のテクスチャを求める前)に描くときは、最後に読んだ画素を使う。`drawImage`や`toDataURL`が読むcanvasの画像も同じインタフェースから得る。
- Blinkの側はDawnのワイヤのクライアントの型で書かれ、`ui/gl/buildflags.h`の`USE_DAWN`がそれを選ぶ。エンジンのgnの構成は`use_dawn = false`のまま生成し(Dawnを入れると多くのソースのコマンドが変わり、ビルドし直しになる)、生成したこのヘッダの`USE_DAWN`だけを1にする(`tools/browser/blink.sh gen`)。DawnとTintのソースは、Dawnを入れた別のgnの出力(`out/tessronos_dawn`、`DAWN=1 tools/browser/gngen.sh`)から取る。そこではOpenGL ESのバックエンドとワイヤだけを入れ、TintのSPIR-V、HLSL、MSLの出力、Perfetto、X11を外す。Tintの生成するソースはGoのジェネレータ(`tools/src/cmd/gen`)で作るので、Go 1.25をDawnの`tools/golang`の場所に置く。DawnのOpenGLのローダの生成にはKhronosの`gl.xml`(OpenGL-Registry)とEGLのヘッダ(EGL-Registry)が要るので、DawnのDEPSが指すバージョンを取得する。
- `GL=1`でないブラウザも、Blinkが使うワイヤのクライアントのために`libdawn.a`をリンクする(リンクされるのはクライアントと関数表だけ)。`webgpu_tessronos.cc`はリンクせず(弱いシンボルで参照する)、`requestAdapter()`はnullを返す。
- WebGPUのコンテキストを作る前に、Blinkはブラウザのプロセスに3DのAPIがブロックされていないかを問い合わせる(`GpuDataManager`)。プラットフォームのブローカがこれをブラウザ側のサービス(14.4.5)に渡し、ブロックしていないと応答する。

| ファイル | 内容 |
| --- | --- |
| `application/browser/port/webgpu/webgpu_tessronos.cc` | `WebGPUInterface`、ワイヤのつなぎ、共有イメージ、canvasの読み戻し、コンテキストのプロバイダ |
| `tools/browser/labels_dawn.txt`、`exclude_dawn.txt` | コンパイルするDawnとTintのgnのラベルと、外すソース |
| `tools/browser/patches/gpu_canvas_context.patch`、`dawn_context_robustness.patch` | BlinkとDawnへのパッチ |

ブラウザを起動して最初のアダプタを作ったとき、コンソールに次の行が出る。

| 行 | 意味 |
| --- | --- |
| `browser: webgpu: adapter softpipe (OpenGL version OpenGL ES 3.1 Mesa 26.2.3)` | アダプタができた。V3Dで描くときは`softpipe`の代わりにv3dのレンダラ名になる |
| `browser: webgpu: the system's OpenGL ES makes no context of 3.1` | システムのEGLがOpenGL ES 3.1のコンテキストを作れない(V3Dなど)。Dawnには求めない |
| `browser: webgpu: no adapter on OpenGL ES` | アダプタができなかった(OpenGL ES 3.1がないなど) |
| `browser: webgpu: no WebGPU (it needs Dawn on OpenGL ES: make GL=1)` | `GL=1`でないブラウザでWebGPUが求められた |

WebGLとWebGPUは同じスレッドで同じMesaのEGLを使う(WebGLはANGLEのOpenGL ESのバックエンド、WebGPUはDawnのOpenGL ESのバックエンド)。DawnのOpenGL ESのバックエンドは自分のコンテキストをスレッドにバインドしたまま戻るので、`webgpu_tessronos.cc`はDawnの仕事(ワイヤのサーバ、遅れた仕事、テクスチャの読み戻し、共有イメージのバインド)の前にスレッドにバインドされていたEGLのコンテキストを覚え、後でそれをバインドし直す。これがないと、WebGPUのcanvasをWebGLの`texImage2D`に渡したときに、canvasを読み戻した後のANGLEの呼出しがDawnのコンテキストに行き、`INVALID_OPERATION`になっていた。WebGLのcanvasをWebGPUのテクスチャにコピーする(`copyExternalImageToTexture`)、WebGPUのcanvasをWebGLのテクスチャや2Dのcanvasに描く、のどちらも画素がそのまま渡る(`test_blink_glgpu`)。canvasの読み戻しはRGBAの画像にする。

#### 14.6.5.1 WebGL on WebGPU

WebGLのコンテキストをWebGPUのデバイスの上で作る機能(`WebGLOnWebGPU`、ChromeでもChromiumの試作段階の機能で、既定で無効)は、Chromeと同じく既定では出さず、`make GL=1`のビルドで起動時の行`features=WebGLOnWebGPU`で出す。canvasの`webgl`のコンテキストは`WebGLRenderingContextWebGPU`になり、ANGLEのWebGPUのバックエンド(`libANGLE/renderer/wgpu`、シェーダはWGSLに変換)がDawnのワイヤのクライアントの上でGLの呼出しを行う。Blinkの側はChromiumの`webgl_rendering_context_webgpu_base.cc`のままで、GLとEGLの関数表(`gl::DriverGL`、`gl::DriverEGL`)は`ui/gl`をリンクして作る。Chromiumの試作がまだ扱わない呼出し(`renderbufferStorage`など、Chromiumのソースで`NOTIMPLEMENTED`のもの)は、Chromeと同じく何もしない。

- ANGLEはWebGPUのデバイスを求めると、応答が来るまで`WaitAny`で待つ。Chromeでは応答をGPUプロセスからIOのスレッドで受けるが、このブラウザのワイヤのサーバは同じプロセスにあり、応答をページのスレッドの次のタスクで渡すので、待っているスレッドには届かない。そこで同期で作るWebGPUのコンテキスト(`CreateWebGPUGraphicsContext3DProvider`)は、サーバの応答をフラッシュの中でその場でクライアントに渡し、サーバの遅延した処理(アダプタとデバイスの準備など)もフラッシュの中で終わるまで実行する。
- `gn`の構成の`use_dawn = false`のため、Blinkのこのソースだけ`USE_DAWN`を1にしてコンパイルする(`port/webgl/webgl_webgpu_flags.h`、`flags.txt`)。ANGLEの`FramebufferWgpu.cpp`がlibc++の内部のヘッダ(`<__config>`)をインクルードする箇所はパッチ`angle_framebuffer_wgpu.patch`で除く。

### 14.6.6 PDF

型が`application/pdf`(または`text/pdf`)の応答は、メインフレームでもiframeでも、PDFium(Chromiumのソースツリーの`third_party/pdfium`。ChromeのPDFの表示が描くのに使うライブラリ)でページごとに96 dpiの画像に描き、その画像を灰色の背景に並べたHTMLのページを代わりにコミットする(`port/blink/pdf_tessronos.cc`の`br_pdf_page`)。タイトルはPDFの文書情報の`Title`、なければアドレスのファイル名である。描くのは最初の60ページまでで、それより多いときはページの終わりにそのことを書く。〈印刷...〉はそのページを他のページと同じく印刷する。PDFのJavaScriptとフォームは動かさず、文字の選択と検索は画像なのでできない。PDFiumは`labels_pdfium.txt`のラベルをほかと同じくコンパイルして`libpdfium.a`にする(`build.sh`の`pdfium_lib`)。gccでコンパイルできない箇所はパッチ`pdfium_texteditengine.patch`と、newlibの`_timezone`を使わせる`flags.txt`の行で修正する。

PDFを表示できることをページに示すため、Blinkのプラグインの一覧(`PluginRegistry`)に、プラットフォームのブローカ(`blob_tessronos.cc`の`PlatformBroker`)からサービスのスレッドで応答し、`application/pdf`と`text/pdf`を受け付ける1つのプラグインを返す。設定の`PluginsEnabled`は真にする(Chromeと同じ)。PDFのほかのプラグインは持たないので、`<embed>`と`<object>`のほかの型は代わりの中身を出す。

### 14.6.7 HEVCとH.264のハードウェアのデコーダ

HEVC(H.265)は、Raspberry Pi 5のデコーダ(デバイスの実身「動画の復号器」、6.7)でデコードする。ソフトウェアのHEVCのデコーダは持たないので、デコーダの無いマシン(QEMU、Raspberry Pi 4)ではHEVCを扱えないと返す。H.264は、IntelのGPUの動画エンジンを持つx64のマシンでは、そのデコーダ(同じ名前の実身、13.12)でデコードし、それ以外のマシンと、そのデコーダが扱えないストリームはFFmpegでデコードする(後述)。

2つのデコーダの実身は、名前、サブタイプ`OB_S_VIDEODEC`、レコードの番号と`T_HEVCCONF`・`T_HEVCINFO`が同じで、属性`device.kind`がRaspberry Pi 5のものは`"hevc"`、Intelのものは`"h264"`である。`br_hevc.cc`は最初に問い合わせられたときに`OB_S_VIDEODEC`の実身を4つまで列挙し、それぞれを`OB_OP_ATRRD`で開いて`ob_get_atv`で`device.kind`を読み、種類ごとに最初の1つを使う。属性を読めない実身は、属性で見分ける前からあったHEVCのデコーダとみなす。

| コンポーネント | 内容 |
| --- | --- |
| `H265Decoder` | Chromiumの`media/gpu`のもの(`h265_decoder.cc`、`h265_dpb.cc`、`codec_picture.cc`)をそのままコンパイルする。Annex Bのストリーム(MP4のデマルチプレクサとWebCodecsがこの形式で渡す)を`H265Parser`で解析し、POCと参照ピクチャの一覧、復号済みピクチャのバッファ、出力の順を担当し、1枚ごとにアクセラレータを呼ぶ |
| アクセラレータ(`port/media/hevc_tessronos.cc`) | `SubmitFrameMetadata`でパラメータと参照を、`SubmitSlice`でスライスヘッダの値とバイト列をコピーし、`SubmitDecode`で1つのジョブにして実身に書く。重み付き予測の色差のオフセットは仕様の式で求めて渡す。スライスのバイト列は`SubmitSlice`の中でコピーする(次のピクチャの最初のスライスが来て前のピクチャが終わるときには、元のバッファはもう無い) |
| デコーダ(`HevcDecoder`) | `media::VideoDecoder`の実装。MainとMain Still Pictureの8ビット、Main10の8ビットと10ビット、1920×1088までを引き受ける。`kConfigChange`でピクチャの大きさとビット深度と、ストリームが持つ参照の数に2を足したスロット(17まで)を実身に設定する。出力するピクチャは、8ビットならスロットからNV12で読みlibyuvでI420のフレームに、10ビットならP010で読み`P010ToI010`でYUV420P10(16ビットのサンプルの下位10ビットに値を持つI420。WebCodecsでは`I420P10`)のフレームにする。デコーダは一度に1つのストリームだけが持つ(`br_hevc_acquire`)。2つ目のストリームはソフトウェアのデコーダに回り、HEVCは再生できない |
| 実身とのインタフェース(`port/media/br_hevc.cc`) | システムのヘッダで実身を探して読み書きする。メディアのライブラリの側がシステムのヘッダを読まないように、ジョブの構造体をCの型で書き直した`br_hevc.h`を間に置き、両者の配置が同じことを`static_assert`で確認する |

メディアのパイプラインもWebCodecsも、デコーダを`DefaultDecoderFactory`から得る。パッチ`default_decoder_factory.patch`で、埋め込む側が渡すデコーダ(`g_platform_video_decoders`、14.6.7.1)をソフトウェアのデコーダより先に並べる。起動時にHEVCの実身があれば、その関数を表に入れ、`UpdateDefaultDecoderSupportedVideoProfiles`でHEVCのMain、Main10、Main Still Pictureを扱えるプロファイルに追加する(`br_hevc_media_init`)。これで`canPlayType`、`MediaSource.isTypeSupported`、WebCodecsの`isConfigSupported`がHEVC(`hvc1.1`・`hev1.1`系と、Main10の`hvc1.2`・`hev1.2`系)を受け付ける。実身が無ければどれも何もしない。

10ビットのフレームの表示は、メディアの要素の描画(`port/media/media_player.cc`の`BrFrameGenerator`)で行う。GPUのラスタ化がYUVの各プレーンを受け取るとき、16ビットのテクスチャを扱えれば、サンプルの10ビットを上位に詰めて下位にも繰り返し(1023が1.0になる)、10ビットの変換行列(BT.2020なら10ビットのもの)でRGBにする。16ビットのテクスチャが無ければ、上位8ビットに丸めて8ビットの行列で変換する。プロセッサでのラスタ化はlibyuvの`I010ToARGBMatrix`でRGBにする。VP9のプロファイル2やAV1の10ビットのフレームも同じ経路を通る。

QEMUの`test_blink_codecs`は、HEVCのMainとMain10を3つの問合せのどれも受け付けないこと(`hevc=000 hv10=000`)を確認する。デコードそのものは実機で、カーネルの確認(6.7.1)が同じジョブの形式を確認する。

#### 14.6.7.1 H.264(IntelのGPU)

ジョブの形式は`include/ts/h264dec.h`の`T_H264JOB`で、カーネルがスライスヘッダを自分で解析する(MFXのshort format、13.12)。プログラムが渡すのは、ヘッダに書かれていないもの(SPSとPPSの値、frame_num、POC、IDRと参照のフラグ、参照ピクチャとそのスロット、スケーリングリスト)とスライスのバイト列である。

| コンポーネント | 内容 |
| --- | --- |
| `H264Decoder` | Chromiumの`media/gpu`のもの(`h264_decoder.cc`、`h264_dpb.cc`、`codec_picture.cc`)をそのままコンパイルする。Annex Bのストリームを`H264Parser`(`media/parsers`、Blinkのライブラリに入っている)で解析し、POC、参照のマーキング(スライディングウインドウとMMCO)、frame_numの欠落の補完、復号済みピクチャのバッファ(`H264DPB`)、出力の順を担当する |
| アクセラレータ(`port/media/h264_tessronos.cc`) | `SubmitFrameMetadata`でパラメータと参照を、`SubmitSlice`でスライスのNALユニットをコピーし、`SubmitDecode`で1つのジョブにして実身に書く |
| デコーダ(`H264Dec`) | `media::VideoDecoder`の実装。Baseline(Constrained Baselineを含む)、Main、Highの8ビット、4:2:0、1920×1088まで、暗号化なしを引き受ける。`kConfigChange`で大きさと、DPBの大きさに2を足したスロット(17まで)を実身に設定する。出力するピクチャはスロットからNV12で読み、libyuvでI420のフレームにする。デコーダは一度に1つのストリームだけが持つ(`br_h264_acquire`)。2つ目のストリームは`Initialize`で拒否し、FFmpegに回る |
| 実身とのインタフェース(`port/media/br_hevc.cc`、`br_h264.h`) | HEVCと同じく、ジョブの構造体をCの型で書き直した`br_h264.h`を間に置き、`T_H264JOB`などと大きさとメンバの位置が同じことを`static_assert`で確認する |

ジョブの値とChromiumの値の対応は次のとおりである。

- SPSとPPSは`H264SPS`と`H264PPS`の値をそのまま渡す。`second_chroma_qp_index_offset`は、ストリームに無いときパーサが`chroma_qp_index_offset`と同じ値にしたものである。
- ピクチャの`frame_num`、`top_field_order_cnt`と`bottom_field_order_cnt`(TopFieldOrderCnt、BottomFieldOrderCnt)、`idr`、`nal_ref_idc`が0でないことを渡す。
- 参照は、`H264DPB`のピクチャのうち「参照に使用」のフラグ(`ref`)が付いたもの全部(短期と長期)である。出力待ちで参照に使わないものは渡さない。`frame_idx`は、長期(`long_term`)ならLongTermFrameIdx(`long_term_frame_idx`)、短期ならFrameNum(`frame_num`)で、POCはそのピクチャの`top_field_order_cnt`と`bottom_field_order_cnt`である。フレームだけを扱うので、両方のフィールドを参照に使うフラグ(`H264_DPB_TOP`、`H264_DPB_BOTTOM`)を付ける。frame_numの欠落を補うためにパーサが作ったピクチャ(`nonexisting`)はスロットを持たないので、実在する別の参照のスロットを借り、`H264_DPB_NON_EXISTING`のフラグを付ける。
- スケーリングリストは、パーサがフォールバック規則(SPSは規則A、PPSは規則B)を適用した後の値を、ストリームの順(ジグザグスキャンの順)で持つ。SPSにリストが無ければ全部16になっている。PPSがリストを持てばPPSのものを、持たなければSPSのものを、8.5.6と8.5.7の逆スキャンでラスタ順にして渡す(リストのi番目をラスタの`scan[i]`番目に置く。VA-APIとV4L2のH.264のデリゲートと同じ変換)。4×4は6つ、8×8はイントラとインターのYの2つである。
- スライスは`H264SliceHeader`の`nalu_data`と`nalu_size`で、パーサが見つけたNALユニットのヘッダの1バイト目から終わりまで(スタートコードなし、エミュレーション防止バイトはそのまま)である。`SubmitSlice`の中でコピーする。

IntelのデコーダがH.264を扱えないとき(フィールドのストリーム、扱わないプロファイルや大きさ、カーネルが`E_NOSPT`で拒否したジョブ、`E_IO`で終わらなかったジョブ、空いたスロットが無いとき)も、その動画の再生は止めず、残りをFFmpegでデコードする。Chromiumの`DecoderStream`は、最初のフレームを出した後のデコーダのエラーでは次のデコーダに切り替えないので、`H264Dec`の中で切り替える。

- `H264Dec`は、最後のキーフレームからの圧縮データのバッファ(`DecoderBuffer`)を参照で保持しておく(64MBを超えたら、次のキーフレームまで保持しない)。SPSとPPSのNALユニットは、アクセラレータの`ProcessSPS`と`ProcessPPS`で番号ごとに記録する。
- 失敗したら、`H264Decoder`を破棄して実身を返し(`br_h264_release`)、`media::FFmpegVideoDecoder`を作って同じ構成で`Initialize`する。構成のextra data(avcC)は空にする。デマルチプレクサもWebCodecsも、このデコーダにはAnnex Bに直したバッファを渡すからである(`NeedsBitstreamConversion`が真)。
- 保持しておいたバッファを1つずつ渡し直し、最初のものの前に記録したSPSとPPSを置く。保持していなければ、次のキーフレームから渡す。すでに出したフレームのタイムスタンプ以下のフレームは出さない。保持しておいたバッファを渡し終えてから、失敗したバッファの`Decode`に応答する(ストリームの終わりのときは、FFmpegに終わりを渡して応答する)。以後のバッファはFFmpegに渡し、構成の変更の`Initialize`もFFmpegに渡す。
- 渡し直しの途中の`Reset`では、待っている`Decode`に`kAborted`で応答してからFFmpegをリセットする。

メディアのパイプラインもWebCodecsも、デコーダを`DefaultDecoderFactory`から得る。パッチ`default_decoder_factory.patch`の`g_platform_video_decoders`は4つまでの関数の表で、埋め込む側が入れた順にソフトウェアのデコーダより先に並ぶ。`br_hevc_media_init`はHEVCの実身があるとき、`br_h264_media_init`はH.264の実身があるときだけ、それぞれの関数を表の空きに入れる。両方の実身があるマシンでも、どちらのデコーダも`Initialize`で自分のコーデックでないものを拒否するので、そのまま並べてよい。H.264はもともとFFmpegで扱えるので、`canPlayType`などの対応の応答とプロファイルの表は変えない。MSEの`isTypeSupported`の大きさとフレームレートの制限(14.6.2)は、H.264の実身があるマシンではH.264について1920×1088と毎秒60フレームまで広げる。MediaCapabilitiesの`decodingInfo`も同じ条件で、`smooth`とともに`powerEfficient`を真と返す(`br_media_type_decodes_in_time`の1か所で判定する)。

実機で1回の再生で分かるように、次の行を出す。

| 行 | 意味 |
| --- | --- |
| `browser: media: H.264 on the machine's decoder` | 起動時に実身が見つかった |
| `browser: media: h264 on the machine's decoder, <幅>x<高さ>` | `Initialize`でIntelのデコーダを選んだ |
| `browser: media: h264: <幅>x<高さ>, profile <番号>, not for the machine's decoder`、`... the machine's decoder is taken, ...` | `Initialize`で拒否し、FFmpegに回した |
| `browser: media: h264: <幅>x<高さ>, <n> slots` | スロットを設定した |
| `browser: media: h264: picture <n> (frame_num …, … slices, … references, … bytes): decoded` | 最初の3枚と、失敗したジョブ(結果は`E_NOSPT`などの名前) |
| `browser: media: h264: frame <n> out at <時刻> us` | 最初の3枚のフレームを出した |
| `browser: media: h264: the machine's decoder failed (<理由>), the rest by the processor` | FFmpegに切り替えた。続く行が渡し直すバッファの数と、出したフレームの最後の時刻を示す |

QEMUにはこの実身が無いので、H.264は今までどおりFFmpegでデコードする。Intelのデコーダでのデコードと、FFmpegへの切替えは実機で確認する。

## 14.7 ウインドウと操作

ページはウインドウの作業領域の全体に描く。ウインドウのタイトルは、表示しているページのタイトル(タイトルがなければアドレス)である。状態は、システムのメッセージ行に表示する。

ウインドウを開く位置は、ほかのアプリと同じ決まり(10.4.8)に従う。

- ページの実身がウインドウの位置を記録していれば(メタデータの`window`の`pos`)、記録した画面(`window`の`screen`)の左上からその位置に開く(`wm_place_xywh`、`wm_place_open`)。その画面が接続されていなければ、主画面の左上から同じ位置に開く。閉じるときは、ウインドウの左上の点がある画面とその画面からの位置を書く(`save_place`、`wm_place_keep`)。主画面のときは`screen`を書かない(前に書いた`screen`は空にする)。
- 記録した位置がなければ(初めて開くページの実身、実身を指定しない起動)、ブラウザの既定の位置(左上から120, 80)を、新しいウインドウを開く画面の左上から数える(`wm_new_x`、`wm_new_y`)。その画面は、ポインタがある画面で、決められなければ主画面である。設定ウインドウ(14.7.10)も同じで、既定の位置は180, 90である。
- ページが開くウインドウ(`window.open`、ピクチャインピクチャ)は、ページが位置を指定すればブラウザのウインドウがある画面の左上からの位置に開く。ページに見せる画面はその1つで、`screenX`と`screen.width`もその画面での値である(14.4.12)。指定がなければ、ブラウザのウインドウの右下へずらした位置に開く。
- ページの問合せ、許可、共有の選択、ダウンロードなどのパネルは、ライブラリの`ui_dlg_open`が、呼出し元のウインドウ(ブラウザのウインドウ)の中央がある画面の中央に置く(`ask_place`)。ツールパネルはページのウインドウの上に置くので、同じ画面に出る。
- ページがウインドウを最大化したとき(14.4.14)は、ウインドウがある画面の矩形にする。

2026年11月まで、ブラウザは位置をビューの左上からの値で記録し、そのまま開いていた。主画面の左か上に外部の画面を置くと、主画面の原点が動き、その値は外部の画面を指していた。

アドレスの欄と操作のボタンは、ページのウインドウとは別の小さなウインドウ(ツールパネル、外寸560×48)にあり、ページのウインドウの8ドット上に置く。ツールパネルは、ページのウインドウの従属ウインドウで、ウインドウマネージャが常にページのウインドウのすぐ前に置き、ページのウインドウを移動すると一緒に移動する。ほかのアプリのウインドウが最前面の間は隠れ、ページのウインドウが最前面に戻ると、そのすぐ前に出る(10.4)。

| 部品 | 動作 |
| --- | --- |
| ← | 戻る |
| → | 進む |
| 再 | 再読込 |
| × | 読込みの中止。それまでに届いたものでページを表示する |
| アドレスの欄 | クリックで入力を始める。Ctrl+Aで全体を選び、Enterで開き、Escで元に戻す |

右ボタンは、まずページに渡す(押下と`contextmenu`のイベント)。ページが`preventDefault`でそれを自分で処理したときは何も出さず、そうでなければウインドウのメニュー(「ブラウザのメニュー」)を出す。戻る、進む、再読込、アドレス入力、最初のページ、〈印刷...〉(14.7.5)、〈メディア〉(14.7.7)、〈拡大〉〈縮小〉〈等倍で表示〉〈全画面表示〉、〈編集〉と、ウインドウの共通のメニューがある。押した所にあるものの項目、〈リンクを新しいウインドウで開く〉〈リンクのアドレスをトレーへ複写〉〈画像を保存〉〈画像のアドレスをトレーへ複写〉〈選んだ文字をトレーへ複写〉は、リンク、画像、選んだ文字があるときだけ出る(ほかのときは`MN_HIDE`で隠す)。行き先のない〈戻る〉〈進む〉は灰色にする。エンジンのスレッドが1秒近く戻らないとき(読込み中など)は、ウインドウのスレッドがその場でメニューを出し、ページには渡さない。〈編集〉には〈トレーへ複写〉〈トレーへ移動〉〈トレーから複写〉〈すべて選択〉(Blinkの編集コマンド、`Copy`、`Cut`、`Paste`、`SelectAll`)と〈ホームページに設定〉がある。〈ホームページに設定〉は、表示しているページのアドレスをページの実身の本文に書き(14.8)、メッセージ行に「ホームページに設定しました」と出す。キーでは、Alt+←で戻り、Alt+→で進む。Chromeと同じく、Backspaceでは戻らない(入力の途中でページを失わないため。Chromeはバージョン52でやめた)。Ctrl+L、F5とCtrl+R、Ctrl+N(新しいブラウザ)、Alt+Home(ホームページ)、Ctrl+＋(`=`、`;`、テンキーの`+`のキー)とCtrl+−とCtrl+0(拡大、縮小、等倍)、Ctrl+ホイール(拡大と縮小)、F11(全画面)がある。

戻る履歴と進む履歴(セッション履歴)は、エンジンがページのドキュメントごとの`HistoryItem`の列として持つ(最大50)。ページの`history.pushState`と`replaceState`、フラグメントへの移動(`#`)もエントリになる。メインフレームのドキュメントがコミットされたとき(`DispatchDidCommitLoad`)と、同じドキュメントの中で移動したとき(`DidFinishSameDocumentNavigation`)に、列にエントリを追加するか(通常の移動)、今のエントリを置き換えるか(再読込、`replaceState`、`location.replace()`、ページの読込み中のフラグメントへの移動)、移動先のエントリに移る(戻る、進む)。`history.length`はその数で、ページの`history.back()`、`forward()`、`go()`と`navigation.back()`、`forward()`、`traverseTo()`は、ブラウザの〈戻る〉〈進む〉と同じ処理を通る(`NavigateBackForward`。Navigation APIの移動はブラウザに依頼する形なので、パッチ`navigation_api.patch`でそこへ回す)。移動先が今のドキュメントのエントリ(ドキュメントの番号が同じ)なら、そのドキュメントの中で移動し(`CommitSameDocumentNavigation`、`popstate`、`hashchange`、`navigate`のイベントが出る)、ほかのドキュメントのエントリならアドレスを取得し直して、その`HistoryItem`を渡してコミットする(`kBackForward`)。そうすると、エントリの状態のオブジェクト(`history.state`)とスクロール位置が戻り、`performance`の移動の種類は`back_forward`になる。新しいドキュメントには、同じオリジンの前後のエントリをNavigation APIのエントリ(`navigation.entries()`)として渡す。

iframeの移動も同じ列のエントリになる(結合セッション履歴)。エントリはそれぞれ、メインフレームの`HistoryItem`と、そのエントリでドキュメントの分かっているiframeごとの`HistoryItem`を持つ。iframeのドキュメントのコミットと、iframeの中の移動(フラグメント、`pushState`)は、子フレームのクライアントが通知し(`DispatchDidCommitLoad`、`DidFinishSameDocumentNavigation`)、通常の移動なら、今のエントリをコピーしてそのiframeの分だけ替えたエントリを追加し、置き換えならそのiframeの分を置き換える。iframeの最初のドキュメント(初期の空のドキュメントからの移動)は、Blinkが置き換えとして始めるのでエントリを追加しない。戻ると進むでは、移動先のエントリと今とで`HistoryItem`の違うフレームを、それぞれ移動先に移す。同じドキュメントのエントリならそのドキュメントの中で移動し、ほかのドキュメントのエントリなら、子フレームのクライアントがアドレスを取得し直し、その`HistoryItem`を渡して`kBackForward`でコミットする(`GoToEntry`)。メインフレームがほかのドキュメントに移るときは、iframeはそのドキュメントの中で新しく読み込まれる。iframeがドキュメントから取り除かれると(ドキュメントごと替わるときを除く)、その分をエントリから除き、そのiframeだけが作ったエントリは消える。iframeが利用者の操作のないまま作ったエントリ(広告の枠が自分で移動するものなど)は、ウインドウの〈戻る〉〈進む〉とAlt+←、→では飛ばし、ページの`history.back()`などでは飛ばさない。

iframeのドキュメントのコミットには、移動を始めたときの読込みの種類(通常、置き換え、再読込、戻る・進む)、リファラ、移動を始めた時刻と要求を送った時刻と応答の時刻、たどったリダイレクト(応答とその行き先)を渡す。そのため、iframeのドキュメントの`performance.getEntriesByType('navigation')`の`type`(`reload`、`back_forward`)、`redirectCount`、`fetchStart`、`document.referrer`がChromeと同じになる。

アドレスの欄、ウインドウのタイトル、メッセージ行は、ページ自身が行った変更に追従する。エンジンのスレッドはループが1周するたびに、ドキュメントの今のアドレス(`pushState`やフラグメントで変わったもの、リダイレクトの後のもの)、タイトル(`document.title`)、ポインタの下のリンクのアドレスかツールチップ(`title`属性)を見て、変わっていればアドレスの欄、タイトル、メッセージ行に出す(`follow_doc`)。アドレスの欄を編集している間は欄を書き換えない。ツールパネルの←と→は行き先がないとき、×は読込み中でないとき、操作できない形(`OB_WP_OFF`)で表示する。再読込は、ドキュメントの今のアドレスを読み直し、スクロール位置を保つ。

ページを離れるとき(リンク、アドレスの欄、再読込、戻る、進む)は、ページの`beforeunload`を先に呼び(`FrameLoader::ShouldClose`)、ページが離れないよう求めれば、ブラウザのデータボックスの確認のパネル(14.7.3)で「このサイトから移動しますか?」(再読込では「このサイトを再読み込みしますか?」)と確認する。Ctrlを押しながら、または中ボタンでリンクを押したときは、新しいウインドウ(別のプロセスのブラウザ、14.7.8)で開く。

アドレスの欄に打ったものは、Chromeのアドレスバーと同じく扱う。スキームのあるものはそのまま、点を含む名前(と`localhost`、IPアドレス、ポート付きの名前)はアドレスとし、インターネットのホスト名はHTTPSを先に試して接続できなければHTTPにする。空白を含むもの、点のない語は既定の検索エンジン(Google)で検索する。ページが読めないときは、Chromeのエラーのページと同じ文言と、ネットワークのエラーの名前(`ERR_NAME_NOT_RESOLVED`など、`br_net_fetch_error`)と〈再読み込み〉のボタンを持つページを出す。

ページの中の検索は、Ctrl+Fかメニューの〈ページの中を検索...〉で、検索する文字をブラウザのデータボックスの問合せのパネル(14.7.3)で入力させる。Chromeの検索バーと同じく、メインフレームの表示している文字から大文字と小文字を区別せずに探し(Blinkの`FindBuffer`、1000件まで)、見つけたものすべてに検索のマーカー(`TextMatch`のドキュメントマーカー)を付け、移動先の1つをアクティブなマーカー(Chromeと同じ橙色)にして表示の中ほどに出す。メッセージ行に「「語」 2 / 3 件」と出す。F3とCtrl+Gで次、Shiftを足して前へ移り(端から反対の端へ回る)、Escでマーカーを消す。

拡大と縮小は、Chromeの倍率の段(25%から500%)で、メインフレームの倍率(`LocalFrame::SetLayoutZoomFactor`)を変える。`devicePixelRatio`もそれに従う(110%で1.100000023841858、Chromeと同じ値)。倍率はホストごとに記録し、同じホストのページを開くとその倍率にする(ブラウザが動いている間)。

全画面は、ページの`requestFullscreen`(動画のプレーヤの全画面のボタンなど)とF11、メニューの〈全画面表示〉で入る。ウインドウの配置のレコードに`OB_WP_FULL`を立てて書き、ウインドウマネージャに作業領域を画面全体にさせる(枠、バー、メッセージ行の上に重なる、10.2.2)。ツールパネルは隠す。全画面を終えるときはフラグを下ろして書き、ウインドウマネージャが元の位置と大きさに戻す。ページが求めたときは、そうしてから`Fullscreen::DidResolveEnterFullscreenRequest`でページに通知し、ページの`exitFullscreen`とEsc、F11では、ウインドウを元に戻してから`Fullscreen::DidExitFullscreen`で通知する。ホイールと矢印キーはBlinkの中でスクロールし、ページの中のスクロールできる領域にも作用する。ページ全体のスクロールバーは、Blinkのものではなく、ウインドウの右と下にあるTessronOSのスクロールバーである。ブラウザは表示のたびに、ページの高さと幅、表示している範囲をスクロールバーのレコードに書き、つまみの前後を押したときやつまみをドラッグしたときの`OB_E_SCROLL`(10.4)を受けて、その位置へスクロールする。ドラッグ中の通知は続けて届くので、最後の位置だけを描く。Blinkのスクロールバーはメインフレームのものだけを隠し(設定の`HideScrollbars`を、パッチ`viewport_scrollbars.patch`でメインフレームのビューポートに限る)、ページの中のスクロールできる領域とiframeは自分のスクロールバーを持つ。新しいウインドウで開くリンク(`target="_blank"`)と、クリックから呼ばれた`window.open`は、新しいウインドウで開く(14.7.8)。直前にクリックやキーの操作がない`window.open`(スクリプトが出すポップアップ)は開かない。ページの入力欄に入力するときは、キーを先にかな漢字変換に渡し、確定した文字を欄に入れる。変換中の文字は欄の中に下線付きで表示し、候補はブラウザが文字の位置の下に表示する。

ウインドウのイベントは、ブラウザのウインドウのスレッドが受け取る。Blinkが動くスレッド(エンジンのスレッド)は、ページのサブリソースがそろうのを待つ間(最長30秒)、ページのスクリプトが実行され続ける間、ページの本文の取得を待つ間はイベントを受け取れないので、以前はその間、右ボタンのメニューも、ピクトグラムのダブルクリックやCtrl+Eで閉じる操作も効かず、ウインドウがフリーズしたように見えた。ウインドウのスレッドはイベントを受け取ると、次のものにはその場で対応する。

| イベント | 対応 |
| --- | --- |
| 右ボタン(ページとツールパネル) | メニューを出す。〈閉じる〉はその場で閉じ、ほかの項目は選んだものをエンジンのスレッドに渡す。ページを移る項目(戻る、進む、再読込、最初のページ)は、読込み中のページを止めてから渡す |
| `OB_E_CLOSE`(ピクトグラムのダブルクリック、閉じる)、ウインドウの削除、Ctrl+E | 閉じる |
| ツールパネルの×、読込み中のEsc | 中止。取得の待ちをやめ、エンジンのスレッドが1秒以上戻らなければ、実行中のスクリプトを終了させる(V8の`TerminateExecution`はどのスレッドからでも呼べる) |

ほかのイベントはキューに入れ、エンジンのスレッドが順に処理する。閉じるときは、ウインドウの位置をページの実身に書いてから、ページとツールパネルのウインドウをすぐに消し、エンジンのスレッドに終了するよう伝える(取得の待ちをやめ、スクリプトを終了させる)。3秒たっても終わらなければプロセスを終了する。中止で本文の取得をやめたときは、表示していたページをそのまま残し、メッセージ行に「読み込みを中止しました」と出す。本文の取得(`br_net_fetch_sync`)は0.1秒ずつ待ち、そのたびに中止を確認する。エンジンのスレッドはループが1周するたびに、時刻と何をしているか(ループ、ページの本文の取得、ページの読込み、サブリソースの待ち)を記録する。5秒より長く戻らないとき、ウインドウのスレッドはそれをコンソールに出し(`browser: the engine's thread has not come round`。続けて戻らないときは、10秒、20秒、40秒と間隔を倍にして出す)、V8に割込みを要求して、スクリプトが実行中ならそのスタック(関数、スクリプト、行、桁)をエンジンのスレッド自身に出力させる。あわせて、エンジンのスレッドのタスクの状態(`tk_ref_tsk`の状態、待ち要因、待ち対象)と、条件変数で待っているか、ほかのスレッドが持つロックを待っているかを出す。後者のために、プログラムは`pthread_cond_wait`、`pthread_cond_timedwait`、`pthread_mutex_lock`をラップして(リンカの`--wrap`、`build.sh`)リンクし、エンジンのスレッドが条件変数で待つとき、または取得されているロックを待つときに、フレームポインタをたどって呼出し元の戻りアドレスを12段まで記録しておく。出力されたアドレスは、Linuxの側に残すシンボル付きのプログラム(`$OBJ/tessronos/browser.elf.sym`)を使って`addr2line -f -C`で関数名に変換する。

この報告は止まっていた最中の一瞬を見るだけなので、エンジンのスレッドは、ループの1周の時間の内訳を自分で計測する(`br_blink.cc`の`watch_time`)。1周が1秒以上かかったとき、その周を終えたところで次の行を出す。時間はプロセスがシステムコールなしで読めるプロセッサのカウンタ(`cntvct_el0`)で計測する。

| 行 | 意味 |
| --- | --- |
| `browser: slow: M ms while 何をしていたか: tasks T ms (N), frame callbacks F ms, style, layout and paint L ms, outside these O ms; in all script S ms, collector C ms, WebGL W ms` | 1周の時間Mの内訳。`tasks`はタスク(Blinkとネットワークのレイヤがポストしたもの)を実行した時間と数、`frame callbacks`は`requestAnimationFrame`のコールバック、`style, layout and paint`はライフサイクルの更新、`outside these`はそれ以外(本文の取得の待ち、ビューのラスタ化など)。`script`はV8への外側の呼出し(スクリプトの実行、関数とイベントハンドラの呼出し。V8が自分で実行するPromiseのジョブは入らない)、`collector`はこのスレッドでのV8のガベージコレクション、`WebGL`はANGLEとその下のOpenGL ES(softpipeかV3D)の呼出しの時間、`console`はコンソールへの書出し(下のリングバッファの空きの待ち)の時間で、どれもMの一部である。ページのスクリプトから呼ぶWebGLは`script`の中にも数える |
| `browser: slow:   a task of T ms (script S, collector C, WebGL W, console K) posted from 関数 (ファイル:行)` | その1周の長いタスクの上位3つと、それぞれをポストした場所 |
| `browser: slow: a task of T ms ...` | 0.5秒以上かかったタスク。終わったときに出す |
| `browser: start: M ms from main: 段 T, 段 T, ... ms` | 起動の各段階の時間。`main`からエンジンのスレッドのループが初めて1周する(`starting`)までのMの内訳で、段階はウインドウ(`the window`)、書体(`the face`)、GPUとEGLの準備(`the GPU`)、ICUのロケール、時間帯、スレッドプール(`the locale, the time zone and the threads`)、Blinkの初期化(`Blink`)、機能とスケジューラ(`the features and the scheduler`)、Blinkのコアとバインディング(`Blink's core`)、V8とそのスナップショット(`V8 and its snapshot`)、ネットワークのレイヤ(`the network`)、保存域、IndexedDBとサービス(`the stores and services`)、エンジンの残り(`the engine's rest`)、ツールパネル(`the tool panel`)である。`main`より前の、プログラムを読みプロセスを作る時間は、システムの`elf: a program of N MB read, its process made in T ms`の行が示す |

最初の1周(`starting`)は`main`の初めから数える。2026年10月23日まで、前の周を終えた時刻の初期値が0(起動からの時刻の原点)だったので、最初の1周は「OSの起動からの時間」になっていた。ThinkPad X1 Yoga(第3世代)の実機で出た`browser: slow: 137407 ms while starting: ... outside these 137407 ms`はこれで、同じログで3回起動したときの値(137407、159519、205626ミリ秒)は、それぞれの起動の時刻(ログの末尾は起動から205秒)と一致する(1回目は、Wi-Fiにつないだ121秒の後に起動した)。130秒の待ちがあったのではなく、起動にかかった時間はこの行からは分からない。分かっているのは、プログラム(214MB)を読みプロセスを作るのにかかった6.3秒(`elf:`の行)だけで、残りは`browser: start:`の行で確認する。同じ日にQEMUの`q35`(WHPX、NVMeのディスク、`test_blink_v3dfall`)で起動すると、次のようになった。

```text
elf: a program of 214 MB read, its process made in 1229 ms
browser: start: 1391 ms from main: the window 35, the face 82, the GPU 863, the locale, the time zone and the threads 137, Blink 3, the features and the scheduler 7, Blink's core 10, V8 and its snapshot 37, the network 34, the stores and services 16, the engine's rest 5, the tool panel 162 ms
```

`the GPU`はエミュレートしたIntelのGPUでirisのscreenを作り、確認のフレームを試してsoftpipeに戻るまでの時間である。実機との違いはプログラムの読込みで、実機はUSBのカードリーダーのメモリカード(`uda`)から読むので約34MB/s、QEMUはNVMeのディスクのイメージから約170MB/sで読む。

WebGLをGPUに移して縮むのは`WebGL`の時間だけで、`script`のうちの残り、`style, layout and paint`、`outside these`のラスタ化は変わらない(ページのラスタ化は14.7.2のGanesh)。

プロセスの標準出力と標準エラーは、システムのコンソール(`tm_putstring`)に渡る。コンソールはシリアル回線に1文字ずつ、割込みを禁止して送り終えるまで戻らないので、Raspberry Pi 5の115200ボーでは、どのスレッドが書いても毎秒約11.5KBしか進まない。2026年10月16日の実機で、起動後に最初のページ(`https://www.tron.org`)を開くたびに出た`the engine's thread has not come round for 5.0 s: the page's parts coming`(13時40分29秒と13時45分8秒)では、その前後の約7秒間、コンソールの行が毎秒11.0〜11.7KB(行頭の時刻を除いた文字数)出ており、回線の速度の上限に達していた。出ていたのはサブリソースごとの`net fetch`、`fetched`の行、`console: Mixed Content`の行、名前解決できなかった取得の行などで、停止の報告もエンジンのスレッドが`printf`の中でコンソールのロックを待つところを捉えていた(呼出し元を変換すると`BrURLLoader::Deliver`の`printf`)。このページはWebGLを使わず、ページのラスタ化はこの間に実行されないので、この停止はWebGLをGPUにしても変わらない。10月8日のV3Dが使えていたときの実機でも、同じ`the page's parts coming`の停止が出ていた。

QEMU(TCG、4コア)で同じページを開くと(`KT_SITE=https://www.tron.org NET=user`、2026年10月16日)、QEMUのシリアルは速いのでコンソールの待ちはなく(`console` 2〜10ミリ秒)、サブリソースを待つ1周の7079ミリ秒と9547ミリ秒の内訳は、スクリプトが2221ミリ秒と6091ミリ秒(`googletagmanager.com/gtag/js`のタイマーの1タスクが4041ミリ秒、ページの中のスクリプトが2325ミリ秒など)、スタイルとレイアウトと描画の記録が2881ミリ秒と1177ミリ秒、WebGLは0ミリ秒だった。QEMUのプロセッサは実機よりずっと遅いので、実機でのスクリプトとレイアウトの割合はこの行を実機で出して確認する。

コンソールへの書出しでスレッドが待たないように、ブラウザは`_write`をラップしてリンクし(`build.sh`の`--wrap=_write`)、ディスクリプタ1と2への書込みを512KBのリングバッファに入れて戻る。リングバッファの中身は専用のスレッドが来た順にコンソールへ渡す。書く側が待つのはリングバッファが一杯のときだけで、行を捨てることはない(あふれそうなら書く側が回線の速度に合わせて待つ)。プロセスを終了するとき(最後の`_exit`、閉じた後のタイムアウト、V8の`abort`と`ExitProcess`、`atexit`)はリングバッファが空になるまで最大で数秒待つ。プロセスを終了させるデータアボート(V8のトラップハンドラが処理しないもの)では、`ts_signal.c`がプロセスを止める前に、そのスレッドがリングバッファの残りを自分でコンソールに出す(`br_console_flush_now`)ので、カーネルの`!EL0 fault!`の行より前に出る。データアボート以外の例外(未定義命令など)でカーネルがプロセスを止めたときは、リングバッファに残っていた行は出ない。

### 14.7.1 描画

ブラウザは、ウインドウの作業領域のサーフェス(10.2)を`wm_obj_surf`で受け取り、Skiaのサーフェス(`SkSurfaces::WrapPixels`)としてそこに直接描く。描いたあとは変わった矩形を`wm_obj_put`でウインドウに転送する。以前は、作業領域の大きさのプロセスのサーフェスに毎回ページ全体を描き、上位バイトを落としながら別のバッファにコピーし(1回目のコピー)、`dp_put_argb`でカーネルがウインドウのピクセルに1画素ずつコピーし(2回目)、`wm_obj_flush`で作業領域の全体を合成し直していた。サーフェスを受け取れないときは今もこの方法で描く(起動引数の行`draw=copy`でも選べる)。

変わった部分は、コンポジタがレイヤについて求めるのと同じ方法で、描画の成果物(`PaintArtifact`)から求める。ビューと同じ大きさの1つのレイヤをルートのプロパティで持つものとして`RasterInvalidator`を置き、現れた、消えた、動いた、プロパティが変わった描画のまとまり(チャンク)と、描き直された表示項目について、新旧の範囲をビューの座標で受け取る。これは、ライフサイクルの描画の段階が古い成果物を片付ける前に行わなければならない。合成をしない設定では、Blinkはその時点を外部に通知しないので、パッチ`paint_hook.patch`で`LocalFrameView::PushPaintArtifactToCompositor`の合成をしない側から関数`g_non_composited_paint_hook`を呼ばせる。ブラウザはそこで変わった矩形をため、プロパティの変更のフラグを、コンポジタが更新のたびに行うのと同じように消す。表示するときは、ためた矩形を近いもの同士でまとめて最大16個にし、それぞれの矩形にクリップして描画の記録を再生する(記録は1回の表示につき1回だけ作る)。何も変わっていなければ描かず、転送しない。新しいページ、ウインドウの大きさの変更、`OB_E_REDRAW`、かな漢字変換の候補の表示中とその直後は、ビュー全体を描く。

スクロールでは、スクロールの平行移動の下にあるすべてのチャンクの位置がビューの上で変わるので、ほぼビュー全体が変わった部分になる。画面ではビュー全体が動くので、ウインドウマネージャはどのみち作業領域の全体を合成し直す。ページの描画はQEMUで3ミリ秒あまりで、1フレームの時間の大半は合成の側にあるので、すでに描いた画素をずらして現れた帯だけを描く方法は採っていない。

起動引数の行`check=<パス>`を与えると、変わった部分だけを描いた各表示のあとで、同じ状態のビュー全体を別のサーフェスに描き直して比べ、異なる画素の数を数える。異なる画素があれば、その範囲、最大の差、その表示で描いた矩形をコンソールに出す。比べている間にウインドウが削除されてサーフェスがウインドウのものでなくなったとき(`wm_obj_surf`が応答しなくなる)は、その表示を数えない。テスト`test_blink_frame`はこれで、変わった部分の漏れがないことを確認する。

QEMU(TCG、4コア、1280×800)で、作業領域800×560のウインドウに、40×40の箱の色を100ミリ秒ごとに変えるページ(`test_blink_frame`の`/fr`)を表示して計測した値は次のとおりである。1フレームの時間は、ブラウザの中で計測した描画(ライフサイクルの更新を含む)、コピー、転送、反映の時間の和で、反映と転送には合成を待つ時間を含む。合成した画素は、その間にウインドウマネージャが合成した画素の数である。

| 場面 | 1フレームの時間(前、後) | 1フレームで描いた画素(前、後) | 合成した画素(前、後) |
| --- | --- | --- | --- |
| 箱の色が変わる3秒(33、34フレーム) | 10.8ミリ秒、1.9ミリ秒 | 448,000、1,600 | 15,232千、54千 |
| 入力欄のキャレットの点滅3秒(7フレーム) | 12.2ミリ秒、2.9ミリ秒 | 448,000、19 | 3,136千、1千 |
| ホイールを8目盛り回す(15フレーム、9フレーム) | 16.3ミリ秒、18.3ミリ秒 | 448,000、398,224 | 10,734千、7,598千 |

前の1フレームの内訳は、箱の場面で描画3.3、コピー1.6、`dp_put_argb`3.5、`wm_obj_flush`2.3ミリ秒、後は描画1.4、`wm_obj_put`0.5ミリ秒である。スクロールでは、後の`wm_obj_put`(15.0ミリ秒)が前の`dp_put_argb`と`wm_obj_flush`の和(11.1ミリ秒)より長い。サーフェスからのコピーはウインドウマネージャのロックの中で行うので、合成の最中に呼ばれると、その合成が終わるのを待ってからコピーし、さらに次の合成を待つためである。前のバージョンは1目盛りの入力のあとと次の描画の要求とでビューを2度描くことがあり(後のバージョンでは2度目は変わった部分がないので描かない。15フレームのうち6フレーム)、8目盛りの合計では、ブラウザの時間は245ミリ秒から165ミリ秒に、合成した画素は約3割減った。

### 14.7.2 GPUでのラスタ化

`make GL=1`のブラウザは、GPUのあるマシンで、ページのラスタ化をGPUで行う(`port/gpu/br_gpu.cc`)。Chromiumでは、レンダラが作った描画の記録をGPUプロセスのSkia(Ganesh)がOpenGL ES(ANGLE)の上でタイルにラスタ化し、vizがそれを合成する。このブラウザは`content`もvizも使わないので、ライフサイクルの描画の段階が作った記録(`cc::PaintRecord`、14.7.1と同じもの)を、SkiaのGaneshがTessronOSのOpenGL ES(Mesa、第13章13.6)の上で1枚のレンダーターゲットに再生し、変わった矩形を作業領域のサーフェスに読み戻す。レイヤはビュー全体の1つで、合成の段階はない。変わった部分の求め方、矩形のまとめ方、`wm_obj_put`での出力の仕方は14.7.1のままで、矩形を画素にする部分だけが変わる。

| コンポーネント | 内容 |
| --- | --- |
| GPUのスレッド | 起動時に1つ作る。MesaのEGLの既定のディスプレイ(プラットフォーム`tessronos`)にOpenGL ES 3のコンテキスト(なければ2)を1×1のピクセルバッファで作り、`GrGLMakeAssembledInterface`で関数を`eglGetProcAddress`から集めて、Ganeshの`GrDirectContext`を作る。Ganeshのオブジェクト(コンテキスト、レンダーターゲット、テクスチャ、canvasのサーフェスと画像)はすべてこのスレッドで作り、使い、破棄する。メインスレッドは処理を渡して終わりを待つので、処理が読む記録、画像、canvasの記録はその間変わらない。ほかのスレッドで解放されたもの(canvasのスナップショット)は、破棄の処理としてこのスレッドに渡し、次の処理の前に破棄する |
| ページのラスタ化 | ビューの大きさのレンダーターゲット(N32)に、矩形ごとにクリップして白で消し、記録を再生する。全部を描いてから`flushAndSubmit`でドライバに渡し、矩形ごとに`readPixels`でサーフェスの同じ場所に読み戻す。Ganeshのテクスチャのキャッシュは96MBまでとする |
| 画像 | Ganeshが描くときにテクスチャにする。ジェネレータがYUVのプレーンを返せる画像(動画のフレーム、JPEG)は、プレーンのままテクスチャにしてシェーダでRGBにする(Ganeshの遅延画像の扱い) |
| 動画のフレーム | 14.6.2の`BrFrameGenerator`。GPUで描くときは、デコードしたフレームのY、U、Vのプレーンがそのままテクスチャになり、RGBへの変換はGPUのシェーダが行う。同じフレームを描き直すときは、Ganeshが作ったテクスチャを使い回す |
| canvasの2D | パッチ`canvas_gpu.patch`で、Blinkのcanvasの画素を管理するもの(`Canvas2DBitmapProvider`)が、作られるときにサーフェスをGPUのスレッドに要求する。GPUのスレッドは、メインスレッドのcanvasで、128×128以上の8bitのものにだけサーフェスを作る。サーフェスを得たcanvasは、記録の再生(`RasterRecord`)、`putImageData`の書込み(`WritePixels`)、スナップショットをGPUのスレッドで行う。スナップショットはGPUのテクスチャをバッキングストアに持つ`PaintImage`(`cc::TextureBacking`の`BrGpuBacking`)で、GPUのスレッドでページやほかのcanvasに描くときはそのテクスチャをコピーせずに描く。ほかのスレッドで画素を求められたとき(`getImageData`、`toBlob`、プロセッサで描くcanvas)は読み戻して渡す。ワーカーの`OffscreenCanvas`と小さいcanvasは今までどおりプロセッサで描く |
| WebGL | 14.6.1のとおり、ANGLEのコンテキストがメインスレッドで同じMesaのディスプレイの上で動く。カレントのコンテキストはスレッドごとに持つので、GPUのスレッドのコンテキストとは互いに切り替わらない。描画バッファは描いたフレームを読み戻してビットマップにし、Ganeshがページに描くときにテクスチャにする |

使うかどうかは起動時に一度決める。既定では、マシンがGPUを持ち(`gp_ref_gpu`が応答する)、OpenGL ESのドライバがGPUで描くとき(v3d)だけ使う。softpipeはプロセッサで描くドライバで、SkiaのCPUのラスタのほうが速いので、使わない。起動引数の行`gpu=on`では、softpipeでもGPUの経路を使う(QEMUでのテスト)。`gpu=off`では使わない。`GL=1`でないブラウザには`br_gpu.cc`がリンクされず(そのシンボルは弱いシンボルで参照する)、いつもプロセッサで描く。GPUの経路が失敗した表示(レンダーターゲットが作れないなど)は、その回だけプロセッサで描く。

起動引数の行`check=`(14.7.1)は、GPUの経路ではビュー全体もGPUで描き直して比べる。行`gpucheck=<パス>`は、GPUで描いた各表示のあとで、同じ状態のビュー全体をプロセッサで描き直し、作業領域のサーフェスと比べる。GaneshとSkiaのCPUのラスタは、辺のアンチエイリアス、文字の位置の丸め、グラデーションのディザが少し違うので、違う画素の数、いずれかの色成分で48より大きく違う画素の数、最大の差を数え、終了時にそのパスに書く。かな漢字変換の候補を出している間は比べない。

GPUで描くかどうかと、描く速さは、コンソールに次の行で出る。速さの行は、ビューを描いている間、5秒ごとに出る。

| 行 | 意味 |
| --- | --- |
| `browser: gpu: Skia's GPU raster (Ganesh) on V3D 7.1..., OpenGL ES 3.1 Mesa 26.2.3` | ページをGPUで描く。括弧の後はレンダラ名とバージョン(softpipeで`gpu=on`のときは`softpipe (Mesa)`) |
| `browser: gpu: the machine has no GPU, ...` | GPUのないマシン(QEMU)で、プロセッサで描く |
| `browser: gpu: softpipe (Mesa) draws on the processor, ...` | OpenGL ESがsoftpipeになった(V3Dの確認が通らなかったなど、13.8.3)。プロセッサで描く |
| `browser: gpu: OpenGL ES: the GPU draws (driver v3d; ...)`、`browser: gpu: OpenGL ES: softpipe draws on the processor (step N, ...)` | GLを初期化した直後に1回。EGLのプラットフォームが選んだドライバと、softpipeに戻ったときの段階と理由(第13章13.8.4の表)。GPUのプロセスでは`browser: gpu process: OpenGL ES:`になる |
| `browser: gpu: none (理由), ...`、`browser: gpu: off (gpu=off), ...` | EGLかGaneshが起動しなかった、`gpu=off`。プロセッサで描く |
| `browser: draw 5.0 s (gpu): N views, F/s; a view X ms (at most Y), K kpx` | 5秒間に描いた表示の数と毎秒の数、1表示の時間の平均と最大、1表示で描いた画素(千)。`(cpu)`ならプロセッサで描いた |
| `browser: draw: on the GPU a view: play P us, flush F us, read back R us (K kpx); N canvases rastered, C us each` | GPUで描いた1表示の内訳。記録の再生(Ganeshがコマンドを組み立てる、プロセッサの時間)、ドライバへの引渡し、読み戻し(GPUの処理の完了を待つ時間を含む)と、その間にGPUで描いたcanvasの記録の数と1回の時間 |
| `browser: draw: video N frames, F/s; P as YUV planes made RGB on the GPU, R made RGB on the processor` | 5秒間に表示した動画のフレームの数と毎秒の数、そのうちGPUでプレーンからRGBにしたものと、プロセッサでRGBにしたもの |
| `browser: draw: webgl N frames, F/s` | 5秒間に表示したWebGLのフレーム(描画バッファ全体の読み戻し)の数と毎秒の数 |
| `browser: gpucheck: ...` | `gpucheck=`の比較の結果 |

実機(Raspberry Pi 5、`V3D=2`)では、起動時の行がv3dのレンダラ名を示すこと、`draw`の行の1表示の時間と、その内訳のうち読み戻しの割合、動画の毎秒のフレーム数と`P as YUV planes`が表示したフレームの数に近いこと、WebGLの毎秒のフレーム数を見る。比べるときは`gpu=off`で同じページを開く。

QEMU(TCG、4コア)では、GPUの経路はsoftpipeの上で正しさだけを確認する。2026年10月6日、`test_blink_gpu`のページ(14.10)を`gpu=on`で開いた結果は次のとおりである。softpipeはドライバへの引渡し(`flush`)の中でプロセッサが描くので、時間の大半はそこにあり、実機の速さの目安にはならない。

| 場面 | 1表示 | 内訳(再生、引渡し、読み戻し) |
| --- | --- | --- |
| 最初の5秒(ページ全体、2つのcanvas、シェーダのコンパイルを含む) | 平均363ミリ秒、最大1043ミリ秒 | 16、348、18ミリ秒。canvasの記録は1回283ミリ秒 |
| 動画だけが変わる5秒(128×96の矩形) | 平均74〜82ミリ秒、毎秒8表示 | 2.6、66〜73、3.7〜4.0ミリ秒。動画は毎秒8フレームで、すべてYUVのプレーンのままGPUに渡った |

`gpucheck=`で97の表示(4346万画素)をプロセッサで描き直したものと比べると、1.8%の画素が違い、48より大きく違う画素はなく、最大の差は27であった。

MSEのテストページ(14.10、H.264の640×360と426×240)を`NET=user`と`gpu=on`で開くと、シーク、画質の切替え、`endOfStream`の後の`ended`まで、プロセッサで描くときと同じように進んだ。表示したフレームはすべてYUVのプレーンのままGPUに渡り、画面の映像の色も正しかった。softpipeでは640×360の矩形を含む1表示に1.7〜1.8秒かかるので、表示できたのは毎秒0.5フレームほどで、残りのフレームは映像のレンダラがドロップした。同じく`https://www.youtube.com/watch?v=jNQXAC9IVRw`を`gpu=on`で開くと、ページ(ロゴ、検索の欄、アイコン、サムネイル、プレーヤの角の丸めとコントロール)と動画(VP9の320×240)が画面に正しく出て、動画は19秒の終わりまで進んで次の動画の案内が出た。softpipeではページ全体の1表示に2〜5秒かかった。

### 14.7.3 ページからの問合せとパネル

ページが利用者に問い合わせるもの(`alert`、`confirm`、`prompt`、`<select>`の一覧、許可の確認)は、ブラウザのデータボックス(`SYSDEF_BOX_BROWSER`、14.8)のパネルで、ページのウインドウの中央の上に出す(`lib/libui`の`ui_dlg_open`)。パネルの部品と枠はウインドウマネージャが描き、ブラウザは部品の状態を読み書きするだけである。Enterは強調したボタン(了解、許可)を押し、Escは取消しにする。

| パネル | 番号 | 部品 |
| --- | --- | --- |
| ページからの知らせ(`alert`) | 1 | メッセージ5行、了解 |
| ページからの確認(`confirm`) | 2 | メッセージ5行、取消、了解 |
| ページからの問い合わせ(`prompt`) | 3 | メッセージ4行、入力欄(既定の値を入れて出す)、取消、了解 |
| 選択(`<select>`) | 4 | 一覧(選んである項目に印、`<optgroup>`の名前は【】で囲み、その中の項目は字下げする)、取消、了解 |
| 許可の確認 | 5 | メッセージ4行(「<オリジン>がトレーの内容の読み取りを求めています。」「許可しますか。」)、拒否、許可 |
| ファイルの選択(`<input type="file">`) | 6 | メッセージ2行、一覧(トレーにある仮身と、パネルに入れた仮身の名前)、取消、了解 |

メッセージは半角で52文字(全角は2文字に数える)までの行に折り返し(`br_main.cc`の`dlg_lines`)、入りきらない分は「…」で終える。折る位置は禁則を守る所まで戻す(10.7.1、`kin_break`)。句点や閉じ括弧が行頭に来るときは前の字と一緒に次の行へ送り、開き括弧は行の終わりに置かない。英字の語(オリジンなど)は、1行に収まる限り途中で折らない。`alert`、`confirm`、`prompt`の間は、ページのスクリプトもエンジンのスレッドもパネルの応答を待つ(Chromiumと同じく、ページはその間止まる)。`<select>`の一覧は、押した操作が終わってからのタスクとして出し、選んだ項目を`HTMLSelectElement`に渡して`change`イベントを発生させる。許可の確認はサービスのスレッドとは別のスレッドで問い合わせるので、その間もほかのサービスは応答する。起動引数の`quit=`で終了する時刻になると、出ているパネルは応答のないまま閉じる。

アドレス欄はカーソルを持つ。左右の矢印、Home、Endで動かし、押した位置に置き、文字の入力、Backspace、Deleteはカーソルの位置に作用する。文字を末尾に入力すると、閲覧履歴のうちアドレスがその文字で始まるもの(スキームと`www.`は省いてもよい)を回数の多い順に補完して色を付けて示す。Enterで補完したアドレスに移り、右矢印かEndで確定し、上下の矢印で候補を替える。次の文字、Backspace、Deleteは補完を外す。

### 14.7.4 ファイルの選択と保存

ページに渡すファイルと、ページが保存するファイルは、実身にする。File System Access API(`showOpenFilePicker`、`showSaveFilePicker`、`port/blink/file_system_access_tessronos.cc`)も同じで、開くときはパネル6で選んだ実身をファイルとして渡し、保存するときは名前をパネル3で入力させた空のファイルを渡す。ページが書いたものは、書込みのストリームを閉じたときにブラウザの保存箱の新しい実身になる(開いた実身は書き換えない)。`showDirectoryPicker`は、パネル6で選んだ実身を、ファイル変換がディレクトリとして書き出すもの(仮身を並べた図形、キャビネットのようなもの。並べた仮身の実身がその中身になる)として渡す。書き出したツリーはメモリの中に、オリジンごとのファイルシステムと同じ作りで持ち、ページはその中のファイルとフォルダを読み書きする。ページが変えると、2秒後にツリーの全体を、仮身を並べた図形の新しい実身としてブラウザの保存箱に取り込み、トレーに仮身を置く(選んだ実身は書き換えない)。ディレクトリとして書き出せない実身を選ぶと、選ばなかったものとする(`AbortError`)。オリジンごとのファイルシステム(`navigator.storage.getDirectory()`)は実身ではなく、ブラウザの保存域に持つ(14.4.5)。ファイルと実身の間の変換は、ファイル変換(`lib/libxfu`、`include/ts/xfu.h`)の規則に従う(`port/net/br_objfile.cc`)。ライブラリが読み書きするファイルのツリーは、メモリの中の1つのファイルである。

- `<input type="file">`を押すと、ブラウザはパネル6(14.7.3)を出す。一覧には、トレーで手にある組の仮身が並び、パネルに仮身を入れると一覧の終わりに加わって選ばれる。了解で選んだ実身を、ファイル変換が書き出すときと同じ1つのファイルにする。取り込んだファイルの実身はそのときのファイル、文章の実身はその文字、ほかはxmlTADである。ファイルのバイト列は、ブラウザの中で`/obj/<UUID>/<名前>`というパスで持ち、Blinkにはそのパスのファイルとして渡す。ファイルの大きさと時刻はブラウザ側のサービス(`FileUtilitiesHost`、`port/blink/files_tessronos.cc`)が返し、Blobのバックエンド(14.4.4)がバイト列を読む。取消のときは、ページに`cancel`イベントが届く。フォルダの選択(`webkitdirectory`)では、パネル6で選んだ実身を`showDirectoryPicker`と同じくディレクトリとして書き出し、その中のすべてのファイルを渡す。ファイルのパスはASCIIの文字だけの`/obj/<UUID>/top/<番号>/file<拡張子>`で、名前を持たないので、上のフォルダの名前を選んだ実身の名前から順に`base_subdirs`に入れて渡す。`webkitRelativePath`(`fsadir/sub/b.txt`など)はそれと名前から作られる。BlinkはこれをAndroidの作りでしか使わないので、パッチ`file_input_type.patch`で、`base_subdirs`があれば使うようにする。Blinkの`FileChooser`はブラウザのプロセスの`FileChooser`のサービスに問い合わせる作りで、パネルの結果を渡すインタフェースがないので、パッチ`file_chooser.patch`で、埋め込み側が選んだファイルを渡すインタフェースを追加する。このパッチはヘッダを変えるので、ヘッダを使う`br_blink.cc`だけが、パッチを当てたヘッダのツリーを先に探す(`flags.txt`の`-iquote`)。
- ネットワークから保存するもの(ダウンロードになったナビゲーションの応答、`download`属性のリンク)は、受信しながら実身に書く。設定ウインドウの〈ダウンロード〉で進捗(受け取った量と、`Content-Length`があればその割合)を見られ、〈中止〉で取得を止められる。一覧は利用者ごとのストア`downloads`に持ち、次の起動でも見られる。保存先は下のとおりブラウザの保存箱のままである。以前は受信しながらメモリにため、終わってから実身にしていた。
- ページが保存するもの(`download`属性のリンク、Blobの`blob:`のURL、`data:`のURL)と、応答が`Content-Disposition: attachment`のページは、実身にして実身「システム」の「ブラウザの保存箱」(`SYSDEF_BROWSER_SAVE`、14.8)に入れ、その仮身をトレーに置く。メッセージ行に「「名前」をブラウザの保存箱に入れ、トレーに仮身を置きました」と出す。ファイル変換が取り込むときと同じく、文字のファイルは文章、画像は図形の実身になり、ほかはそのまま持つ。名前は、ページが付けた名前、`Content-Disposition`の`filename`、アドレスの最後の部分の順に取り、拡張子がなければ型から付ける。Blinkはダウンロードをブラウザのプロセスに依頼する作りなので、パッチ`local_frame.patch`で、依頼する内容をブラウザに渡すフック(`g_download_url_hook`)を追加する。バイト列は、`data:`ならフレームが作ったBlobから、`blob:`ならURLのBlobから(ページがURLをすぐに取り消してもトークンで読める)、ほかは取得して得る。保存はそれぞれのスレッドで行うので、同時に終わったものが保存箱に仮身を追加するときに保存箱を同時に開き、`E_BUSY`で失敗した。いまは保存箱に仮身を追加する処理を1つずつ行う(`br_objfile.cc`の`g_save_lock`)。全体を持つもの(`br_obj_save`)は取り込み全体をその中で行い、受信しながら書くもの(`br_obj_save_stream`)は、保存箱のボリュームに、どこからも仮身のない実身として取り込み(`xfu_import`に保存箱を渡さず`near`で場所だけ渡す)、全体を書き終えてからその中で保存箱に仮身を追加する(`xfu_box_link`)。受信は長くかかるので、その間ほかの保存を待たせない。添付のページは表示せず、表示していたページをそのまま残す。レンダラを使うときは、保存とダウンロードの一覧の記録はブラウザのプロセスが行う。ページが作ったバイト列(`blob:`、`data:`)は、レンダラがメモリの実身に入れ、`Host.KeepDownload`(同期)でブラウザに渡し、ブラウザが受け取ってから応答を返す。ネットワークから取得するものは、応答の時点でリクエストごとブラウザに渡す(下)。ブラウザは、添付のサイトのために起動したレンダラを、ページの応答が保存と分かった時点で終了させる。以前はレンダラが自分のスレッドで保存していたので、別のサイトの添付は保存の前にレンダラごと消え、保存箱にも一覧にも残らず、メッセージ行は「保存しています…」のままだった。Chromeでもダウンロードはブラウザのプロセスの処理で、レンダラはファイルを書かない。〈設定〉の〈ダウンロード〉は、ブラウザが記録した行(受信中の行もブラウザが持つ)と、ウインドウのページのレンダラが記録した行(応答の来る前に失敗した`download`属性のリンク)を合わせ、同じ行は1つにして、受信中、新しい順に並べる。ブラウザの受信中の行の〈中止〉は、レンダラに問い合わせずブラウザで止める(`br_uidata.cc`の`br_ui_download_cancel`、キーの`browser:`)。
- ナビゲーションの応答を表示するか保存するかは、Chromeのブラウザがコミットの前に決めるのと同じ判定で、応答のヘッダが来た時点で決める(`br_blink.cc`の`br_nav_judge`。Chromeでは`NavigationURLLoaderImpl::OnReceiveResponse`)。`Content-Disposition: attachment`の応答のほか、型がBlinkのドキュメントにできるもの(`blink::IsSupportedMimeType`。HTML、XML、文字、Blinkが表示する画像と動画と音声)でない応答も保存する。PDFは今までどおりPDFiumで描くページとして表示する。型が中身を示さないもの(型なし、`text/plain`、`application/octet-stream`など`net::ShouldSniffMimeType`が挙げるもの)は、`X-Content-Type-Options: nosniff`がなければ先頭の1024バイトから型をスニッフィングし(`net::SniffMimeType`)、その型で判定して表示する。型なしの文字は文字のドキュメントになり、文字でないバイト列は保存になる。保存になる応答のステータスが成功(2xx)でないときは、保存も表示もせず、Chromeと同じく無効な応答のエラーページ(`ERR_INVALID_RESPONSE`)を出す。ブラウザ自身のページ(`about:`など)で型のないものはHTMLのままである。以前は添付だけを保存し、ほかの応答はすべてドキュメントにしていた。`application/zip`の2.7MBの応答をBlinkがHTMLとして構築し、実機ではレンダラが数分たっても応答せず、ブラウザがフリーズした。QEMUでは1.5MBの文字でないバイト列の解析が1つのタスクで27〜30秒かかり(Oilpanのヒープが100MBになる)、その間レンダラはどの要求にも応答しなかった。`application/octet-stream`の応答が文字として表示されたのも同じ原因である。
- 保存のメッセージ行は、保存する側がブラウザのエンジンのスレッドに依頼して出す(コマンド`said:`)。保存を始めるときに「「名前」を保存しています…」、実身にし終えたときに「「名前」をブラウザの保存箱に入れ、トレーに仮身を置きました」を出す。取り込めなかったとき、接続が切れたとき、1分のあいだ何も届かなかったときは「「名前」を保存できませんでした」、〈中止〉とブラウザを閉じたときは「「名前」の保存を中止しました」を出す。依頼した順に、ナビゲーションの応答の後で出るので、上書きされない。以前はナビゲーションの応答を受けたエンジンのスレッドが「保存しています…」を出していた。保存をブラウザのプロセスへ移してからは保存が応答より先に終わり、完了の文がその後の「保存しています…」で上書きされて残らなかった。`said:`の文は、届いてから3秒は、ポインタの下のリンクのアドレスより優先して出し、その後はリンクのアドレスに戻す(`br_main.cc`の`kSaidMs`)。以前はポインタがリンクの上にある間、完了の文がリンクのアドレスに隠れて読めなかった。Chromeの状態の表示も、ページの状態の文とリンクのアドレスを分けて持ち、リンクのアドレスはポインタがリンクの上にある間だけ出す。
- 応答は、ヘッダと、型のスニッフィングに要る先頭の分(1024バイト。スニッフィングしないときは要らない)が来た時点で判定する(`br_nav_head_fetch`)。ナビゲーションの取得は、ネットワークスタックの、受け取る側のペースに合わせるストリーム(`br_net_fetch_paced`)で行い、応答はナビゲーションのスレッドでなく、ネットワークのスレッド(レンダラではネットワークのプロセスの応答が届くスレッド)で受けて、ナビゲーションのスレッドが待つ。表示するものは、そのまま全体を受け取ってからドキュメントにする(全体で60秒のタイムアウトは今までどおり)。ダウンロードと決まったら、ナビゲーションはそこで保存になったと応答して終わり、表示していたページは残る。本体はリクエストごとダウンロードの受け取る側に渡す。レンダラでは、ネットワークのプロセスからの応答のパイプ(`NetFetchClient`のレシーバ)を自分から外し(`br_net_fetch_take`)、それまでに届いた分と一緒に`Host.Download`(同期)でブラウザに渡す(`br_net_fetch_give`)。パイプでまだ読んでいない分は、そのままレシーバとともにブラウザへ移る。ブラウザはそれを専用のスレッド(downloads)で受け、届いた分を取り込みのスレッドに渡す。レンダラは本体を持たず、ブラウザに渡した後に終了してもよい。1つのプロセスのときは、同じストリームの続きを取り込みのスレッドに渡す。取り込みは1つのダウンロードに1つのスレッドで、ファイル変換の取り込み(`xfu_import`)が読むファイルを、届いたものを順に返すツリーにする(`br_obj_save_stream`)。全体をメモリに持たないよう、ネットワークのプロセスは、渡した分のうち受け取る側が取り終えたと応答していない分(`NetFetchClient.Piece`の応答)が512KB(`kPaceWindow`)になると、それ以上読まない。レンダラのレシーバは届いたときにすぐ応答し、ブラウザのレシーバは取り込みのスレッドが取ったときに応答する。Chromeでネットワークのサービスとダウンロードの間のデータパイプが一杯になると読むのを止めるのと同じ働きである。ダウンロードには全体のタイムアウトを掛けず、1分のあいだ1バイトも届かなければ止まった接続とみなして失敗にする(`kDlIdleMs`)。接続が切れたとき、2xxでないとき、書けなかったとき、〈中止〉のときは、取り込みが途中まで作った実身を消し(ファイル変換が失敗した取り込みの実身を残さないのと同じ)、保存箱にもトレーにも何も追加せず、一覧の行を失敗か中止にする。ネットワークのプロセスのリクエストは、パイプを閉じて止める。ブラウザを閉じるときは、受信中のダウンロードを〈中止〉と同じく止め、途中の実身を消して一覧に中止と記録してから(3秒まで待つ)、レンダラとネットワークのプロセスを終了させる(`br_downloads_close`)。Chromeは受信中のダウンロードがあると閉じる前に確認し、閉じればダウンロードを取り消す(次の起動で続きから受け直せる途中のものとして残す)。TessronOSのブラウザは続きからの受け直し(`Range`)を持たないので、途中のものは残さない。以前は応答の全体を受け取ってから判定していたので、実機で3MBのzipを受ける間エンジンのスレッドが5秒止まり(「has not come round for 5.1 s」)、ナビゲーションの全体のタイムアウトの60秒より長くかかるファイルは取得できず、受け取る間はレンダラとブラウザのメモリにその大きさだけ持っていた。`download`属性のリンク(`save_from_net`)も、ストリームを始めて応答が来たら同じ経路で渡す。Service Workerが応答したナビゲーションと、1つのプロセスのときにページが開いたウインドウのナビゲーションは、今までどおり全体を受け取ってから判定し(`br_nav_answer`)、ページが作ったバイト列と同じく全体を渡す。

### 14.7.5 印刷

ページの印刷(メニューの〈印刷...〉と`window.print()`)は、プロセスのアプリの印刷(11.22.6)と同じく、デスクトップに依頼する。デスクトップが印刷設定のパネルを出し、PDFを書いてデスクトップに置く。

- ブラウザは、ページをBlinkの印刷のモード(`PrintContext`)で、A4から上下左右に57ポイントの余白を除いた印刷域(CSSの画素で641×970、1画素が96dpiの1画素)にレイアウトし、各ページを印刷のときと同じ描き方で描く(`port/blink/print_tessronos.cc`)。`beforeprint`と`afterprint`のイベントを前後に送る。
- 描いた記録は、文字を抜き出すキャンバス(`TextTaker`)に再生する。テキストブロブ(`SkTextBlob`)のうち、拡大と移動だけで描かれ(回転や傾きがない)、1色で塗られ、どのグリフも書体の文字に戻せるもの(`SkFontPriv::GlyphsToUnichars`)は、画素に描かず、図形の文章(`<document>`)にする。文章は、ブロブの位置と大きさ(書体の上端から下端まで)、文字の大きさ、色、書体の名前、太字と斜体、文字を持つ。クリップで見えないブロブは捨てる。そのほかのもの(箱、線、画像、回転した文字など)は画素に描き、PNGの画像にする。
- 印刷のページの実身(`ui_prn_open`)に、ページごとに、A4と余白を持つ図形のレコードを入れ、その画像を余白の内側に置き、文章をその上に重ねる。画像はその実身のリソース(`_<ページ>_1.png`)にし、図形の`<image>`の`href`が指す(`port/net/br_print.cc`)。デスクトップに`DT_RQ_PRINT`で依頼し、応答(書き出した、パネルを閉じた)を待ってから実身を消す。デスクトップはその文章を、その書体の文字としてPDFに書く(11.22)ので、PDFの中の字は検索や複写ができる。
- `window.print()`は、Chromiumと同じく応答が来るまでページを止める。
- 文章の字の並びはデスクトップの組版による。ブラウザの描いた位置と字の幅が少し違うことがあるので、文章の枠はブロブの幅より字の大きさの半分だけ広くし、折り返さないようにしている。

### 14.7.6 ドラッグ&ドロップ

Chromiumでは、ページの中で始まったドラッグはブラウザのプロセスがOSのドラッグとして扱い、ドラッグ先のページへの進入、移動、ドロップをレンダラに伝える。このブラウザでは、エンジンがその両方を担当する。

- ページがドラッグを始めると(`draggable`の要素、選んだ文字、リンク、画像。Blinkが`ChromeClient::StartDragging`を呼ぶ)、ブラウザはドラッグするデータ(`DataObject`)を保持し、それ以降のポインタの移動とボタンを離す操作をドラッグの操作として扱う。移動するたびに`DragController::DragEnteredOrUpdated`で、ポインタの下の場所が受け付ける操作を求め(`dragover`)、離した場所が受け付けるなら`PerformDrop`(`drop`)、受け付けなければ`DragExited`にし、ドラッグ元に終了を伝える(`dragend`)。
- ドラッグ中にポインタがウインドウの外へ出ると(`OB_E_LEAVE`)、ドラッグしているもののアドレスか文字を、デスクトップに文字として運ばせる(`DT_RQ_CARRY`の`DT_F_TEXT`)。文章のウインドウや文字を受け付けるウインドウの上で離すと、そこに文字が入る。ページのドラッグは、受け付けられれば複写、そうでなければ受け付けられなかったとして終わる。
- ほかのウインドウからページの上にドロップされた仮身(`OB_E_DROP`)は、14.7.4と同じくそれぞれの実身を1つのファイルにして、文章のウインドウから運ばれた文字(`OB_E_TEXT`)は`text/plain`にして、ドロップされた場所に外部からのドラッグとしてドロップする。ページが受け付けないときは、ドロップした側に「このページはここでは受け取りません」と応答する。
- ドロップしたファイルには、File System Accessの受け渡しのトークン(`FileSystemAccessDataTransferToken`)を付ける。ページが`DataTransferItem.getAsFileSystemHandle()`を呼ぶと、トークンからそのファイルのバイト列を持つファイルのハンドルを返す(`port/blink/file_system_access_tessronos.cc`)。ドロップしたファイルの隔離されたファイルシステムは持たないので、`webkitGetAsEntry()`はnullを返す。
- ページが受け付けないファイルや文字をドロップしても、そのファイルのページに移ることはしない。

### 14.7.7 メディアセッション

`navigator.mediaSession`は、Blinkのモジュール(`modules/mediasession`)をそのまま使い、ブラウザのプロセスが持つ`MediaSessionService`を`port/blink/services_tessronos.cc`が持つ。フレームごとに、再生しているもの(タイトル、アーティスト)、再生の状態、受け付ける操作(`setActionHandler`)、操作を伝える先を保持する。システムにはメディアのキーがないので、操作はブラウザのメニューの〈メディア〉(〈再生/一時停止〉、〈前の曲〉、〈次の曲〉、〈停止〉)から与える。メニューで選ぶと、再生中のセッション、なければ最後に何かを通知してきたセッションに操作を伝え、メッセージ行に「「題」に伝えました」と出す。〈再生/一時停止〉は、セッションが再生中なら`pause`、そうでなければ`play`である。その操作を受け付けていないときは「このページはその操作を受け付けていません」と出す。

### 14.7.8 複数のウインドウとピクチャインピクチャ

ページが新しいウインドウを求めたとき(`target="_blank"`のリンク、`window.open`、ドキュメントのピクチャインピクチャ`documentPictureInPicture.requestWindow`)は、Chromeと同じ切り分けでウインドウを開く。Chromeでは、開いた側のスクリプトが新しいウインドウのドキュメントに同期してアクセスできるウインドウ(openerを持つウインドウ)は、開いた側と同じレンダラのプロセスに入る。openerを持たないウインドウは、別のプロセスでよい。

- openerを持つウインドウ(`window.open`、ドキュメントのピクチャインピクチャ)は、同じエンジンの別のページにする(`br_blink.cc`の`br_open_window`)。BlinkがChromeClientの`CreateWindowDelegate`で新しいウインドウを求めると、開いた側の閲覧コンテキストのグループとエージェント(`AgentGroupScheduler`、ウインドウのエージェントのファクトリ)を引き継いだ`Page`とメインフレームを作る。フレームはopenerを開いた側にして初期化するので、初期の空のドキュメントは開いた側のオリジンを持ち、開いた側のスクリプトがそのドキュメントを読み書きできる。新しいウインドウの`about:blank`への移動は、初期の空のドキュメントのままにする。ほかのアドレスへの移動は、そのウインドウのページがアプリケーションに依頼し、アプリケーションが取得して読み込む。
- アプリケーション(`br_main.cc`)は、そういうページごとに、ツールパネルを持たない通常のウインドウを作る。ウインドウの大きさは、ページが求めたもの(`window.open`の`width`と`height`、ピクチャインピクチャの`width`と`height`)、なければ開いた側と同じである。ウインドウの通知はブラウザのポートにウインドウごとの番号で届き、キー、ボタン、ポインタの動き、ホイールはそのページに渡る。ページの入力欄にフォーカスがあるときのキーは、ページのウインドウと同じく、かな漢字変換を通る。かな漢字変換は1つで、最後にキーを打ったウインドウのページに、確定した文字と変換中の文字を渡し、候補の一覧もそのウインドウに描く。ほかのウインドウで打ち始めたり押したりすると、変換中の文字はもとのウインドウのページで確定する。ピクチャインピクチャのウインドウ(ドキュメントのものと動画のもの)はフローティングウインドウ(`WM_ATTR_FLOAT`、10章)で、ほかのウインドウを前に出しても、その前に出たままである。ページの描画は、ページのウインドウと同じく、変わった部分だけを作業領域のサーフェスに描いてウインドウに出す。タイトルバーにはページのタイトルを出す。ウインドウを閉じるとページを外し(`Page::WillBeDestroyed`)、開いた側からは`closed`が真に見える。ページのスクリプトが`window.close()`を呼ぶと(スクリプトが開いたウインドウだけ、Blinkが許す)、ウインドウを閉じる。開いた側のページがほかへ移ると、そのピクチャインピクチャのウインドウを閉じる。
- openerを持つウインドウ(`window.open`)にも、通常のウインドウと同じツールパネル(アドレスの欄と戻る、進む、再読込)を付ける。欄は、そのウインドウのドキュメントの実際のアドレスを示し、`pushState`にも従う。`about:blank`はそのまま示す。ピクチャインピクチャのウインドウには付けない。
- openerを持たないウインドウ(`target="_blank"`のリンクは既定でこれ、`window.open`の`noopener`)は、ブラウザのプログラム(`SYSDEF_PROG_BROWSER`)をそのアドレスで起動し、別のプロセスのブラウザのウインドウとして開く。そのブラウザはアドレスの欄、履歴、かな漢字変換を持つ通常のウインドウである。起動する前に、Cookie、ページの保存域、HTTPキャッシュを書き出しておき、新しいブラウザが今の状態から始まるようにする。エンジンのスレッドは変わった分を渡すだけで(`br_store_flush_async`)、書込みはストアのスレッドが行い(14.8.1)、新しいブラウザを起動するスレッドがその終わりを10秒まで待つ(`br_store_wait`)。以前はエンジンのスレッドで全体を書き直しており、実機のSDカードではその間(数秒)ページが止まっていた。プロセスの生成(`ob_cre_obj`の`OB_T_PROCESS`)は、動いているブラウザのプログラムのページを共有するので、ヘッダのほかは読まない(5.1)。共有する前は約190MBのプログラムを新しい空間に読むのでQEMUで約3秒、実機のSDカードでは数十秒かかっていた。QEMUでシステムボリュームをSDカードに置いて計測すると、リンクを押してから新しいブラウザのプロセスができるまでが、共有の前は235秒、共有の後は0.32秒(うちストアの書込みを待つのが0.27秒、プロセスの生成が13ミリ秒)であった。生成は、そのために作るスレッド(`br_main.cc`の`apart_thread`)で行い、エンジンのスレッドはその間もページを動かし続ける。以前はエンジンのスレッドで生成を待ち、約7秒止まっていた。生成の途中でプロセスを終えると生成の呼出しが中断されるので、プロセスの終了(`apart_wait`)とウインドウを閉じたときの強制終了は、生成が終わるのを待つ。リンクを押してから新しいブラウザが最初のページを求めるまでは、QEMUで約5.5秒である(生成が約3.1秒、新しいブラウザの起動が約2.4秒)。プロセスの間で保存したものを同時に書き戻すと後から書いたほうが残るのは、ページの実身を2つ開いたときと同じである。`window.open`はnullを返す。
- 利用者の操作(クリックやキー)の直後でない新しいウインドウの求めは、ポップアップとして開かない。ドキュメントのピクチャインピクチャは、Blinkが利用者の操作を確認する。
- ドキュメントのピクチャインピクチャは、Blinkのモジュール(`modules/document_picture_in_picture`)の`PictureInPictureControllerImpl`をそのまま使い、そのウインドウのページを上のとおりに作る。

動画のピクチャインピクチャ(`HTMLVideoElement.requestPictureInPicture`)では、Blinkが、ブラウザのプロセスの`PictureInPictureService`に、プレーヤの番号とそのサーフェス(GPUで合成するときの名前)を渡してウインドウを求める。このブラウザでは、メディア要素のプレーヤ(14.6.2)が求められたときにサーフェスの名前を作り、`port/blink/pip_tessronos.cc`がサービスを持つ。

- サービスは、動画の大きさ(幅400以下に縮め、比を保つ)のウインドウをアプリケーションに依頼し、その大きさをページに返す。アプリケーションはウインドウを作り、プレーヤが新しいフレームを出すたびに、そのフレームをウインドウに黒の縁で比を保って描く。
- ウインドウを押すと、動画が止まっていれば再生、再生中なら一時停止を、要素のプレーヤのインタフェース(`media.mojom.MediaPlayer`)で要求する。
- `document.exitPictureInPicture`(セッションの`Stop`)ではウインドウを閉じる。利用者がウインドウを閉じると、セッションのオブザーバに止まったことを通知し、ページに`leavepictureinpicture`が届く。ウインドウは一度に1つで、別の動画が求めると前の動画はウインドウを出る。
- `pictureInPictureEnabled`は真である。

### 14.7.9 スペルチェック

入力欄(`<input>`、`<textarea>`、`contenteditable`)に入力した英語の綴りを、hunspellで調べる(`port/spell/spell_tessronos.cc`)。フレームのクライアントがテキストチェックのクライアント(`WebTextCheckClient`)を返し、Blinkは編集している欄の文字をチェックに出す(`RequestCheckingOfText`)。チェックのクライアントは文字を、ラテン文字と`'`が続く語に区切り、数字や日本語を含む語と1文字の語は飛ばす。辞書にない語にマーカーを付け、候補を3つまで添える。誤った綴りの下には赤い波線が出る。Blinkは、利用者が操作した欄だけを調べる(欄をクリックしてフォーカスした、入力して中身が変わった)。スクリプトが欄にフォーカスしただけでは調べない。

辞書は英語(`en_US`、SCOWLの単語の表から作ったもの、`tools/browser/spell`)で、ブラウザにリンクして組み込む(`build.sh`の`spell_dict`)。hunspellはChromiumのソースツリーにあるものを、Chromiumと同じくICUで大文字と小文字を扱う構成(`OPENOFFICEORG`)でコンパイルする。辞書の行のリーダ(`FileMgr`)は移植レイヤのもの(`port/spell/filemgr_ts.hxx`と`filemgr_tessronos.cc`)で、組み込んだバイト列を読む。hunspellのリーダはC++のライブラリの`std::ifstream`を使うが、このプログラムはライブラリのストリームのロケールを用意していないので、使えない。移植レイヤのヘッダを各ソースより先にインクルードさせ(`flags.txt`の`-include`)、hunspellの`filemgr.hxx`のインクルードガードの名前を先に定義して、その代わりにする。辞書は最初に語を調べるときに読む。

### 14.7.10 設定ウインドウ

ウインドウのメニューの〈設定...〉(`settings`)は、ブラウザのデータボックスのパネル8を持つ通常のウインドウを開く(`br_settings.cc`)。システム環境設定と同じ`lib/libui`の作りで、部品と枠はウインドウマネージャが描き、ブラウザは部品の状態を読み書きするだけである。ウインドウは専用のスレッドで動かし、ページの動作とは互いに待たせない。データの変更は、そのデータを持つスレッドに渡して応答を待つ(`port/net/br_data.h`)。もう一度選ぶと、開いているウインドウを前面に出す。

| タブ | 内容 |
| --- | --- |
| パスワード | サイト、利用者名、パスワードの一覧。パスワードは既定で伏せ、スイッチ〈パスワードを表示〉で見える。〈変更...〉はパネル9で新しいパスワードを2回入力させる。〈削除〉は確認してから消す |
| パスキー | 依拠当事者(サイト)と利用者名の一覧と〈削除〉 |
| 閲覧データ | 消す種類のスイッチ(Cookie、キャッシュ、localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル、サイトの許可、閲覧履歴、ダウンロードの一覧)、期間、サイトの一覧と〈消去〉 |
| 履歴 | 日時、タイトル、アドレスの一覧と〈削除〉〈すべて削除〉 |
| ダウンロード | 名前、状態、進捗の一覧(0.5秒ごとに更新)と〈中止〉〈一覧から削除〉 |
| 住所 | 住所の一覧と各項目の入力欄、〈追加〉〈変更〉〈削除〉 |
| 拡張機能 | 読み込んだ拡張機能の名前、バージョン、状態。〈読み込む...〉、〈有効と無効〉、〈削除〉(14.12) |
| カード | カードの一覧(番号は既定で下4桁のほか伏せる)、スイッチ〈カードの番号を表示〉、入力欄、〈追加〉〈変更〉〈削除〉。番号の入力欄は文字を伏せる。セキュリティコードは保存しない |

消去の対象は次のように決める。サイトは登録可能ドメイン(`example.co.jp`など。IPアドレスとlocalhostはそのもの)で比べる。Cookieは作られた時刻、キャッシュは最後に使った時刻、履歴は最後に訪れた時刻、ダウンロードは始めた時刻で期間と比べる。localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル、サイトの許可は、中身のそれぞれの書込み時刻を持たないので、オリジンがストレージを最後に使った時刻(ストア`sites`、1時間単位)で比べる。そのため、期間を指定すると、その期間に使ったオリジンの古いデータも消え、その期間に使っていないオリジンのデータは残る。この形より前のデータは最後に使った時刻を持たないので、次に使うまで期間指定では消えない。全期間なら消える。HSTSの情報とサーバについて知ったこと(HTTP/3の対応、QUICのセッション)は閲覧の痕跡なので、前者は閲覧履歴と、後者はサイトと期間を指定しないキャッシュの消去と一緒に消す。

別々に開いたウインドウのブラウザは別のプロセスで、それぞれストアの内容をメモリに持つ。消去をそのプロセスだけで行うと、ほかのプロセスの次の保存で元に戻るので、消去とパスワードの変更はストア`sites`に通知のエントリとして書く。各プロセスは3秒ごとに通知を調べ、ほかのプロセスの通知を自分でも実行する(`br_data.cc`)。通知は1日で消す。

保存する住所とカードは、パスワードと同じ方法で暗号化して「ブラウザの保存域」のレコード12に置く(14.4.7.1)。ページへの入力は、ログインのフォームでないフォームの欄(`autocomplete`、なければ`name`と`id`で種類を判断する)にフォーカスが移ったときに、ドキュメントごとに1度、パネル4で使うものを選ばせ、選んだときだけ行う。カードの入力は安全なコンテキストのページに限る。パネルの一覧のカードの番号は下4桁のほか伏せる。

## 14.8 実身

| 実身 | UUID | 内容 |
| --- | --- | --- |
| プログラム「ブラウザ」 | `01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57`(`SYSDEF_PROG_BROWSER`) | プログラム箱にある。レコード1が実行ファイル。idは`web-browser` |
| 原紙「ブラウザ」 | `01a0d8c4-5b22-7b13-9d54-8a6f7e8c9d01` | 原紙箱に並ぶ。複製するとページの実身になる。アドレスは`https://www.tron.org` |
| 「ブラウザのメニュー」 | `01a0d889-46bc-7e8b-8a9c-d2e3f4a5b678`(`SYSDEF_MENU_BROWSER`) | メニューの定義。〈Widevineを導入...〉(`wvinstall`)と〈Widevineを外す〉(`wvremove`)を含む(14.6.3.1) |
| 「編集」 | `01a0d889-46d1-7a2b-8c3d-4e5f6a7b8c9d` | 「ブラウザのメニュー」の〈編集〉の下のメニューの定義。〈トレーへ複写〉(`copy`)、〈トレーへ移動〉(`cut`)、〈トレーから複写〉(`paste`)、〈すべて選択〉(`selall`)、〈ホームページに設定〉(`sethome`)を持つ |
| 「メディア」 | `01a0d8c4-5b28-7c31-9d42-a1b2c3d4e528` | 「ブラウザのメニュー」の〈メディア〉の下のメニューの定義。〈再生/一時停止〉(`mplay`)、〈前の曲〉(`mprev`)、〈次の曲〉(`mnext`)、〈停止〉(`mstop`)を持つ |
| 「ブラウザのデータボックス」 | `01a0d8c4-5442-7c94-8a05-1b2c3d4e5442`(`SYSDEF_BOX_BROWSER`) | ページからの問合せとファイルの選択のパネル1〜6の定義(14.7.3)と、Widevineを導入する前に示すパネル7「Widevineの導入」の定義(14.6.3.1) |
| 「ブラウザの保存箱」 | `01a0d8c4-5b26-7f17-8b98-ceabbdc0d145`(`SYSDEF_BROWSER_SAVE`) | 実身「システム」にある。ページが保存したものの実身の仮身を並べる(14.7.4) |
| 「ブラウザの利用者データ」 | ユーザごとに作る(UUIDは固定でない) | そのユーザのCookie、サイトのデータ、キャッシュ、履歴など(下の表) |
| 「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」 | `SYSDEF_BROWSER_COOKIE`、`SYSDEF_BROWSER_STORAGE`、`SYSDEF_BROWSER_CACHE` | 実身「かな漢字変換の学習」にある。レコード1に、この形より前のブラウザが書いた、期限付きのCookie、オリジンごとの`localStorage`、HTTPキャッシュが残っていることがある(下の移行で空になる)。「ブラウザの保存域」はレコード2にオリジンごとのIndexedDBのデータベースを、レコード3にWeb Pushの購読を、レコード4にService Workerの登録を、レコード5に同期とContent Indexを、レコード6にCache Storageを、レコード7に許可の設定を、レコード8にオリジンごとのファイルシステムを、レコード9にパスキーとパスワードを、レコード10にユーザごとに導入したWidevineのCDMの実身を、レコード11にCDMのファイルを、レコード12に暗号化した住所とカードを、レコード13にユーザごとの「ブラウザの利用者データ」の表を、「ブラウザの蓄え」はレコード2にサーバについて知ったこととQUICのセッションチケットを持つ |

ページの実身のアドレスは、レコード0の文章の本文である。ブラウザは本文のうち、アドレスである最初の行(`://`を含む行か、`about:`か`www.`で始まる行。全角の英数字は半角として読み、前後の空白は除く)を開く。そのため、基本文章編集でアドレスを書き換えた実身や、URL仮身と同じように文章にアドレスを書いた実身は、書いたアドレスのページを開く。このアドレスが実身のホームページである。リンクでページを移っても、戻る、進む、再読込をしても、実身は書き換えない。メニューの〈編集〉〈ホームページに設定〉を選んだときだけ、本文を表示しているページのアドレスの1段落に書き直し、メタデータ`webBrowser`にそのページのタイトルを書く。メタデータの`window`には、ウインドウを閉じるときにウインドウの位置と大きさを書く(位置はウインドウがある画面の左上から。主画面以外の画面では`screen`も書く。14.7)。戻る履歴と進む履歴は実身に持たず、開くたびに空から始める(ホームページを開く実身に前回の履歴を持ち越しても、つながりのない履歴になるため)。本文にアドレスがない以前の実身は、メタデータ`webBrowser`の`url`を開く。以前の実身が持っていた`url`、`back`、`forward`は読まず、ホームページを設定するときに取り除く。ページの実身の仮身を開くと、デスクトップがプログラム「ブラウザ」をその実身のUUIDを引数にして起動し、ブラウザは実身のアドレスのページを開く。原紙のアドレスは`https://www.tron.org`なので、原紙から作ったページの実身は最初にトロンフォーラムのページを開く。アドレスを持たない実身は空白のページ(`about:blank`)を開く。実身を指定せずに起動したときは、ブラウザの説明のページ(`about:home`)を開く。同じページの実身をもう一度開くと、表示しているウインドウを前面に出す。

実身「ブラウザの利用者データ」は、ブラウザが代理するユーザ(`ob_get_crd`)ごとに初めて使うときに作る。所有者はそのユーザ、モードは`rw-------`、ACLはなく、固定(`ob_lnk_obj`)で保つ。「ブラウザの保存域」と同じボリュームに置き、どの箱にも仮身を置かない。「ブラウザの保存域」のレコード13(`BRU1`)に、ユーザと実身のUUIDの組を並べる。この表は探す手がかりで、実身の所有者がそのユーザで、グループとほかの人に何も許さず、ACLがないことを確認してから使う。

| レコード | ストア | 内容 |
| --- | --- | --- |
| 1 | `cookie` | 期限付きのCookie |
| 2 | `storage` | `localStorage` |
| 3 | `cache` | HTTPキャッシュ |
| 4 | `indexeddb` | IndexedDB |
| 5 | `quic` | サーバについて知ったこととQUICのセッションチケット |
| 6〜11 | `push`、`sw`、`background`、`caches`、`perms`、`opfs` | Web Push、Service Worker、同期とContent Index、Cache Storage、許可の設定、オリジンごとのファイルシステム |
| 12 | `history` | 閲覧履歴(アドレスをキーに、最後に訪れた時刻、回数、タイトル。5000件まで) |
| 13 | `hsts` | HSTSの情報(14.5) |
| 14 | `downloads` | ダウンロードの一覧 |
| 15 | `sites` | オリジンがストレージを最後に使った時刻、保存域のクォータの行(最後に使った時刻、使用量、永続化、14.4.5.1)、消去と追い出しの通知(14.7.10) |

各ストアのレコードの番号に16を足したものが、そのストアのもう一方のレコードである。

この形より前のブラウザが実身「ブラウザのCookie」「ブラウザの保存域」「ブラウザの蓄え」の共有のレコードに置いたものは、ストアを最初に読むときに、そのユーザのレコードが空ならコピーし、コピーできたら共有のレコードを空にする。どのユーザのものかは記録がないので、最初にブラウザを使ったユーザのものになる。

### 14.8.1 ストアの保存

Cookie、`localStorage`、IndexedDB、HTTPキャッシュ、Cache Storageなど、ブラウザが次の起動に持ち越すもの(ストア)は、実身のレコードに置く(`port/net/br_store.cc`)。ストアはキーとバイト列の組(エントリ)の集まりで、全体を1つとして保存するストアは空のキーの1エントリとして扱う。

レコードはログの形(`BRS2`)である。先頭の32バイトがヘッダで、マジック、世代、有効なログの長さ、ヘッダのCRC-32を持つ。その後ろに、エントリの追加(put)と削除(del)が、種類、世代、キーと値の長さ、全体のCRC-32を持つ20バイトのヘッダとともに並ぶ。

- 変わった分の保存は、ログの末尾にエントリを書き足してから、新しい長さのヘッダを書く。ヘッダを書く前に電源が切れても、前のヘッダの長さまでが前の保存のまま読める。
- ログがエントリの中身の2倍(と1MB)を超えたとき、全体を1つとして保存するストアを保存するとき、前の形のレコードしかないときは、すべてのエントリを、ストアのもう1つのレコード(レコード番号に16を足したもの。「ブラウザの保存域」なら17〜24)に次の世代で書き直し、最後にそのヘッダを書いてから、元のレコードを空にする。読むときは、2つのレコードのうちヘッダの正しい、世代の大きいほうを使う。どの時点で電源が切れても、前の保存か新しい保存のどちらかが読める。
- 以前のブラウザの形のレコード(`BRS1`。マジック、長さ、CRC-32、中身)は空のキーのエントリとして読み、次の保存で新しい形をもう1つのレコードに書く。パスワードとパスキー(レコード9)、Widevineの実身(レコード10)、CDMのファイル(レコード11)は、これまでどおり`BRS1`の形でその場に書く。

保存はストアのスレッドが行う。エンジンのスレッドは保存するものを作って渡すだけである。

Cookieのストアは、Cookieごとに1つのエントリを持つ。ウインドウを別に開いたブラウザはプロセスごとに別のCookie jarを持つので、全体を1つとして上書きすると、後から保存したプロセスが、ほかのプロセスのログインを消す。そこで各プロセスは、自分のCookie jarで変わったエントリだけを、ほかの書込みを排他して読み直した内容に重ねて書く。書くたびにすべてのエントリを書き直すので、消したCookieの値はログに残らない。各プロセスは3秒ごとにストアの世代を確認し、変わっていればほかのプロセスが書いた分を自分のCookie jarに取り込む。Cookieの保存はエンジンのスレッドを待たず、ストアのスレッドが期限を調べる。セッションCookie(期限のないCookie)も、起動の時刻のマークを付けて同じストアに書く。ウインドウの間で共有し、OSを起動し直した後の読込みで捨てる。これはChromeの既定(起動時に新しいタブを開く設定)と同じ動きである。

`localStorage`、IndexedDB、Service Workerの登録、許可の設定、HSTSも共有のストアにした(`br_store_register_shared`)。所有者は今のエントリの全部を渡し、ストアが前に書くか読み取った内容との差を求めて、Cookieと同じように最新の内容に重ねて書く。ほかのプロセスが変えたエントリは所有者に渡す。同じエントリを両方が変えたときは、後から書いた方が残る。エントリの単位は次のとおりである。

| ストア | エントリ | ほかのプロセスの変更を受けたとき | Chromeとの違い |
|---|---|---|---|
| `localStorage` | 項目ごと | 項目を変え、同じオリジンのウインドウに`storage`イベントを出す | イベントが数秒遅れ、`url`はオリジンになる。`clear()`は項目ごとのイベントになる |
| IndexedDB | データベースごと | 接続がなければすぐ取り込む。接続があれば閉じるまで待ち、バージョンが違うか消されたときは`versionchange`を出す | 下に記す |
| Service Workerの登録 | スコープごと | 登録を追加する、または解除する。スクリプトが違い自分のワーカーが動いているときは、このプロセスの間はそのまま | プロセスごとにワーカーが動く |
| 許可の設定 | オリジンと許可ごと | 設定を変える | 数秒遅れる |
| HSTS | ホストごと | 規則を変える | 数秒遅れる |

IndexedDBの書込みは、書込みのトランザクションが実行されていないときだけ行うので、保存されるのは常にコミット済みの状態である。ただし、トランザクションの直列化は1つのプロセスの中だけである。Chromeはすべてのタブのトランザクションを1か所で順番に処理するが、TessronOSのブラウザでは2つのプロセスが同じデータベースを同時に開いて書くと、後から保存した方のデータベース全体が残る。別のプロセスのアップグレードは、開いている接続が閉じるのを待たずに進む(`versionchange`は後から届き、`blocked`は出ない)。古いバージョンの接続が書いた内容は、新しいバージョンの上には書かない。

電源オフと再起動では、デスクトップがプログラムのウインドウに閉じる要求を送り、プログラムの終了を最大20秒待ってから`fs_sync`を呼ぶ。

| ストア | 保存の単位 | 保存するものを作る場所 |
| --- | --- | --- |
| Cache Storage(`caches`) | キャッシュ(キー`c`と番号)と項目(キー`e`と番号)。番号は作った時刻とプロセスから作り、作った順に大きくなるので、16桁の16進数のキーの順に読み戻すと元の順になる。前の保存から作られたもの、本文を読み終えたもの、消えたものだけを書く | サービスのスレッド(`MakeDelta`) |
| HTTPキャッシュ(`cache`) | キャッシュのエントリ(キーはキャッシュのキー、値は`BRH2`と3つのストリーム)。ストリームの大きさと、ヘッダとメタデータのハッシュが前の保存と同じエントリは書かない。走査のほかにも開かれているエントリ(本文を受信中か、読んでいるところ)は書かずに次の保存に回し、ストアにある前の保存のエントリはそのまま残す | ネットワークのスレッド(`CacheDeltaOnNet`) |
| そのほか(`cookie`、`storage`、`indexeddb`、`quic`など) | 全体 | エンジンのスレッド(登録した関数)。前の保存とCRC-32と長さが同じなら書かない |

エントリ単位のストアは、変わってから決めた時間(Cache Storageは2秒、HTTPキャッシュは1分)の後に、エンジンのスレッドの`br_store_tick`が変わった分を求め(`br_store_register_keyed`の関数)、ストアはエントリのあるスレッドで変わった分を作って`br_store_answer`で渡す。エンジンのスレッドは待たない。終了時(`br_store_flush_all`)と別のプロセスのブラウザを起動するとき(`br_store_flush_async`と`br_store_wait`)は、すべてのストアの変わった分を求め、書き終わるのを待つ。このプロセスの中の実身の読み書きは1つのロックで順に行い、別のプロセスのブラウザが書込みのために開いている実身は、開けるまで最長5秒リトライする。

保存のたびに、コンソールに`store <名前> saved`の行で、エントリ単位のストアは変わったエントリの数、書いたバイト数、書き直したかどうか(`afresh`)、かかった時間を出す。テスト`test_blink_cachekeep`は、Cache Storageに512KBの応答を24個入れて保存した後に小さい応答を1つ追加し、次に起動したブラウザが25個を本文ごと読み戻すことを確認する。

HTTPキャッシュのストアは、2026年10月16日まで、本文を受信中のエントリもそのとき届いていた分だけで保存していた。次の起動ではそれが完全な応答として返り、Googleマップでは、途中で切れたスクリプト(`Uncaught SyntaxError: Unexpected end of input`)、WebAssemblyのモジュール(`CompileError: ... extends past end of the module`)、アイコンの書体(`OTS parsing error`)になった。切れたスクリプトはメニューのボタンを使えるようにする部分を含み、ボタンは`disabled`のまま、押しても`pointerdown`と`pointerup`だけが届き(無効なフォームのコントロールには`mousedown`と`click`を送らない)、メニューは開かなかった。いまは、使用中のエントリを保存しない。読み戻すときは、値が`BRH2`で始まらないエントリ(前の形式のもので、途中で切れたものがどれか分からない)と、200の応答で本文の長さが`Content-Length`と違うエントリ(キャッシュ自身が途中で切れたとマークしたものは除く。それは範囲指定のリクエストで残りを求める)を捨て、次の保存でストアからも消す。コンソールの`http cache: N entries read back, M dropped`の行にその数が出る。Cache Storageは、本文をBlobから読み終えた項目だけを本文ごと保存するので、この問題はない。テスト`test_blink_cachecut`がこれを確認する。QEMU(`NET=user KT_SITE=https://www.google.com/maps KT_SITE_CLICK=162,141`)では、修正の後、ボタンは使えるようになり、押すと`pointerdown`から`click`までがChromeと同じ順にボタンに届き、設定のモジュール(`exm=AlIQtb,...`)を読み込んでスタイルを追加する。ただし、Chromeではその直後(3ミリ秒)に追加されるメニューのパネル(`div.Gs6lwf`)が、60秒待っても追加されない。例外やコンソールの出力はない。ページはまだHTTP/3で書体やサブリソースを読み込んでいる最中で(1本あたり毎秒10〜20KB、応答のヘッダを30秒待つリクエストもある)、そのことと関係があるかは分かっていない。その後、パネルが追加されないのは、地図のコンポーネントが置いたWebGL 2のフェンスがいつまでも完了せず、ページの初期化が止まっていたためと分かった(14.6.1)。QEMUで、ページに差し込んだスクリプトから見ると、Chromeでは読み込みから約1.5秒で始まる入力ツールと残りのモジュールの読み込みが、クリックまで始まらず、マップは`ActionFlow error: hung`を報告していた。HTTP/3の遅さとは関係がなかった。修正の後、QEMU(`GL=1`、softpipe)では、入力ツールと残りのモジュールがクリックを待たずに読み込まれ、メニューのボタンを押すとパネル(`div.Gs6lwf`)が追加された(softpipeで描くので遅く、押してから約58秒かかった)。このときマップは`invalid-render-detected timer; glError:1282`も報告していた。1282は`GL_INVALID_OPERATION`である。名前の`timer`はマップが描画の正しさをタイマで調べることを指し、時間の問合せ(`EXT_disjoint_timer_query`)とは関係がなかった。この拡張はページに出ていない(14.6.1)。ANGLEのエラーの説明をコンソールに出すようにして(14.6.1)見ると、エラーを残していたのはWebGL 2の`drawBuffers`で、ANGLEは`GL_INVALID_OPERATION: glDrawBuffersEXT: Command requires an extension.`と記録していた。それを修正すると、次はインスタンス描画が`glDrawElementsInstancedANGLE`で同じエラーを残した。どちらも、`GLES2Interface`の拡張の名前のメソッドを、OpenGL ES 3のコンテキストでも拡張の関数に渡していたためである(14.6.1)。フェンスが完了しなかった間は、地図のコンポーネントが描画の確認まで進まず、このエラーも出なかった。フェンスを修正した後は、マップがこのエラーを見て描画が正しくないと判断し、地図を描くのをやめた。実機では地図が黒くなった(2026年10月17日のイメージ)。コアの関数に渡すように修正した後、QEMU(`GL=1`、softpipe)では`glError`も`invalid-render-detected`も出なくなった。ただしQEMUでは、DawnがsoftpipeのOpenGL ES 3.1でWebGPUのアダプタを作れるので、マップはWebGPUで地図を描こうとし、地図の部分は黒いままである。WebGPUを止める(`features=-WebGPU`)と、WebGLで地図が描かれる。実機のV3DはOpenGL ES 3.0なので、WebGPUのアダプタはなく、マップはWebGLで描く。ところが、`drawBuffers`などを修正したイメージでも、実機では地図が消えた(`glError`も`invalid-render-detected`も出ない)。WindowsのChromeをヘッドレスで動かし、同じ注入スクリプトで調べると、マップは地図をページのスレッドではなくワーカー(`mc0`)で描いていた。地図のcanvasを`transferControlToOffscreen`でワーカーに渡し、ワーカーの`OffscreenCanvas`のWebGL(WebGPUのアダプタがあればWebGPU)で描く。ページのスレッドのWebGLは小さなテスト用のコンテキストだけである。このブラウザでは、こうしてワーカーが描いたフレームが、ページのcanvas(プレースホルダ)に一度も出ていなかった(14.6.1)。フェンスを修正する前は、マップの初期化がフェンスの待ちで止まり、地図を画像のタイルで描いていた。修正した後は、ワーカーのベクタの地図に進んで、そのフレームが出ないので地図が消えた。QEMUではsoftpipeが遅く、マップがタイルのまま描いていたので、地図は出ていた。ただしsoftpipeのOpenGL ES 3.1でWebGPUのアダプタができるときは、マップがワーカーのWebGPUを使い、地図は黒かった。プレースホルダにフレームを渡すようにした後、QEMUでは、ワーカーのWebGLとWebGPUで描いたテストページのcanvasが表示された。

読めないことと空であることは分ける。ストアの読込みが失敗したとき(別のブラウザが30秒より長く実身を持っている、システムのディスクが外れている、ヘッダが読めない)、そのプロセスはメモリでは空から始めるが、ストア全体を置き換える保存は拒否する(`br_store.cc`の`g_unread`)。Cookieは保存の時刻ごとに読み直し、読めたら今のCookieと合わせて保存する。読めなかったCookieがブラウザに戻るのは次の起動からである。利用者の表(レコード13)が読めないときや、自分の行の実身について問い合わせられないときは新しい実身を作らない。行の実身が自分だけのものでないときは、行も実身も残したまま新しい実身を作る。暗号化したストアで鍵が読めないときは、新しい鍵で上書きしない。書込みが失敗した変更は順序を保って64MBまでメモリに残し、15秒ごとに書き直す。起動のたびに`browser: store cookie: N kept (rc)`をコンソールに出す。

## 14.9 未対応のもの

- 移植していないWebのインタフェースは、その機能を持たないブラウザと同じく、グローバルのプロパティが`undefined`になる。それらのインタフェースを返す`navigator`などの属性と操作の扱いは14.4.3にある。
- レイヤの合成(コンポジタとviz)は使わない。GPUでラスタ化したページも、作業領域のサーフェスに読み戻してウインドウシステムに渡す(14.7.2)。WebGLの描画バッファも読み戻してページに描く。WebGL 2は`make GL=1`のときだけ扱う(14.6.1)。WebGLの描画バッファのアンチエイリアスは使わない。
- QUICのコネクションマイグレーション(ネットワークやポートを替えて接続を保つ)は扱わない。ネットワークを替えてのマイグレーションには、ネットワークが切り替わったことの通知と、ネットワークを区別するハンドルが要るが、システムはどちらも持たず、Chromiumがそれを扱えるのもAndroidだけである。経路が悪くなったときのポートマイグレーションは、QEMUのTCGでは応答の遅れが経路の劣化に見え、マイグレーションが無用に起きるので止めている(応答が遅いことでQUICの接続を捨てる判定も止めている)。
- Web Audioの出力先は既定の1つと`{type: "none"}`(どこにも出さない)である(`AudioContext.setSinkId`、`sinkId`)。マイクの`MediaStream`は`createMediaStreamSource`でWeb Audioに入る。
- 専用ワーカーのホストのないインタフェースを呼んだときは、パッチ`dedicated_worker.patch`でスクリプトを取得できなかったときと同じく`error`イベントにする。
- 画像のデコーダのうち、Rustで書かれたPNG、JPEG、BMP、ICOのものは使わず、C++のデコーダを使う。
- ChromeのLinux版が出さない次の3つは、Chromeと同じく既定では出さず、起動時の行`features=`で名前を挙げたときだけ出す。実装はどれも入っている。Content Index(`ContentIndex`、ChromeではAndroidだけ)、CSS Animation Worklet(`AnimationWorklet`、Chromeでは実験的な機能)、`navigator.share`(`WebShare`、ChromeはWindows、macOS、ChromeOSだけ)。`test_blink_face`が、既定でこの3つがページから見えないことを確認する。
- MSEのWebCodecsのチャンクの追加(`appendEncodedChunks`、`addSourceBuffer`の`videoConfig`と`audioConfig`)は、Chromeでも実験的な機能(`MediaSourceExtensionsForWebCodecs`)で、Chromeと同じく既定では出さない。起動時の行`features=MediaSourceExtensionsForWebCodecs`で出すと、Chromiumの`WebSourceBufferImpl::AppendChunks`がChunkDemuxerにチャンクを渡し、再生できる(`test_blink_mchunk`)。EMEのキーシステムはClear Keyと、利用者が導入したときのWidevine(ソフトウェアのL3だけ)である(14.6.2、14.6.3)。
- 画面の共有(14.4.18)で、共有中の`CaptureController`の操作(フォーカス、拡大縮小、ホイールの転送)は扱わないと返す。`suppressLocalAudioPlayback`は効かない。
- 映像は、HEVC(Raspberry Pi 5)とH.264(IntelのGPUのマシン)のハードウェアデコーダ(14.6.7)を除いてCPUでデコードする(V3Dに動画のデコーダはない)。YUVからRGBへの変換は、ページをGPUでラスタ化するときはGPUで、そうでなければCPUで行う。

## 14.10 テスト

テストグループ`v8`(`tests/ktest/ktest_v8.c`)は、`make BROWSER=1 BROWSER_BLINK=1`でビルドしたときにブラウザを動かして確認する。ブラウザが入っていないときは各テストをスキップする。テストのタスクがループバックでHTTPのサーバになり、ブラウザのプロセスが要求したパスと、画面の画素と、最後に表示したページのタイトルを見る。ページの実身はタイトルを持たないので(14.8)、タイトルは、テストがブラウザに与える`title=`の行で、ブラウザが終わるときにファイルに書かせる。

| テスト | 内容 |
| --- | --- |
| `test_script`、`test_skia` | V8でJavaScriptとWebAssemblyを実行する、Skiaで描いた図形と日本語をウインドウに出す |
| `test_jit` | `tests/uprog/v8prog.cc`を引数`bench`で起動し、再帰、型付き配列のふるい、オブジェクト、文字列のループと、WebAssemblyのループを実行させる。値が正しく、再帰とふるいの関数がMaglevかTurbofanの機械語になったこと(`%GetOptimizationStatus`)を確認する。続けて`bench --jitless`で同じループをインタプリタで実行させ、スクリプトが自分で計測したJavaScriptのループの時間(コンソールの`v8prog: result ok <ミリ秒>`)が機械語で実行したときの2倍を超えることを確認する。プログラムとV8の起動はどちらでも同じだけかかり、ループより長いので、起動から終了までの時間は比べずに出力だけする |
| `test_browser`、`test_blink` | リンクとリダイレクトをたどり、ページを移ってもページの実身の本文とメタデータが変わらないこと、`sethome`の行(メニューの〈ホームページに設定〉と同じ処理)で本文がそのページのアドレスになり、次に開くとそのページから始まることを確認する、Blinkでスタイルシートとスクリプトのあるページをレイアウトして描く、画像4種類、Cookie、`localStorage`、canvasの2D |
| `test_blink_caret` | 入力欄、テキストエリア、アドレス欄で矢印、Home、End、Delete、Shift+左が動作し、文字がカーソルの位置に入る。ページの欄は英語と日本語(変換中でないとき)で確認する |
| `test_blink_settings` | メニューの〈設定〉のウインドウにパスワードとパスキーが並び、パスワードは伏せてある。変更と削除が次の起動にも残る |
| `test_blink_clear` | Cookieとサイトのデータ(localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル)を、期間、サイト、全体で消し、次の起動で確認する |
| `test_blink_users` | 共有のレコードに残った以前のデータが最初の利用者に移り、共有のレコードが空になる。別の利用者からは見えない |
| `test_blink_history` | 閲覧履歴が次の起動で並び、1件消せる。アドレス欄の補完で残ったページに移る |
| `test_blink_hsts` | HSTSの情報が次の起動に残り、閲覧履歴の消去で消える。規則はテストの行で追加する(TLSのサーバを使わないので、応答のヘッダからの登録はChromiumの処理に任せ、ここでは試さない) |
| `test_blink_dl` | 受信中の進捗、〈中止〉、完了、一覧の持ち越しと一覧からの削除 |
| `test_blink_dlatt` | `127.0.0.1`のページから別のサイト(`localhost`)の添付へ移ると、そのために起動したレンダラはすぐ終了するが、添付はブラウザの保存箱の実身になり、次の起動の〈ダウンロード〉に完了の行が出る |
| `test_blink_dlmime` | 添付と示さない応答のうち、`application/zip`、`application/octet-stream`、型なしの文字でないバイト列、文字でないバイト列の`text/plain`がダウンロードになり、ページが残り、メッセージ行が完了の文になってそのまま残り、保存箱の仮身が1つ増え、トレーの一番上がその仮身で、次の起動の〈ダウンロード〉に完了の行が出る。型なしの文字は表示する。`text/html`と称する1.5MBのバイト列をレンダラが解析している間に〈中止〉(Esc)が5秒以内に効く |
| `test_blink_dlslow` | 秒を数えてタイトルに出すページ`/dsp`から、64秒かけて届く640000バイトの文字でないバイト列`/dsl.bin`へ移ると、応答の時点でダウンロードになり、10秒以内にメッセージ行に「保存しています…」が出て、ページは残って数え続け(レンダラが本体を待たない)、途中の〈ダウンロード〉に受信中の行が0%と100%の間の割合で出て、60秒より後に完了の文が出て保存箱の仮身が1つ増え、次の起動の〈ダウンロード〉に完了の行が出る |
| `test_blink_autofill` | 住所とカードの登録、変更、削除、番号を伏せること、暗号化、パネルで取消なら入力しないこと、選べば入力すること |
| `test_blink_input` | 入力欄への入力とフォームの送信、ホイールのスクロール、かな漢字変換、`target="_blank"`のリンクを別のプロセスのブラウザが開くこと(押してから新しいブラウザがページを求めるまでの時間をコンソールに出す。待つのは20秒まで)。戻ったフォームの欄は打った文字を保つ(Chromeと同じ) |
| `test_blink_hwheel` | 横のホイール(`HID_WHEEL_H`。`dz`が正で右。タッチスクリーンの二本指を左へ動かした向き)が、右向きの`wheel`イベント(`deltaX`が正)としてページに届き、ページを横にだけ動かすこと。縦のホイールは縦にだけ動かし、左へ回すと左端に戻ること |
| `test_blink_barstep` | ウインドウの右のバーのつまみを8回に分けて下へドラッグし、続けてホイールを150msおきに4目盛り下へ回す。ブラウザが書くバーの位置を10msごとに読み、下へ動かしている間に一度も戻らないこと、操作を終えた後に止まること(2つの位置を往復しないこと) |
| `test_blink_hide` | 上端に固定した6つの箱を、ページが50px過ぎてスクロールしたときに6通り(`display:none`、`visibility:hidden`、`opacity`を0、ドキュメントから外す、`opacity`の遷移、`transform`の遷移でビューの外へ)で消し、クリックで出したポップアップを700ms後に消す(`display:none`と`opacity`の遷移)。その下に、遅延読み込みの画像、読み込んでから浮かび上がる画像、交差の監視で背景を付けて浮かび上がる箱の行を並べる。ホイールを6目盛り下へ回した後とポップアップを消した後に、消した箱の画素が画面に1つも残らないこと、ビューに入った3種類の画像が見えることを、レンダラ(`check=`で、ウインドウが見せたビューをレンダラのビュー全体と比べる)、`gpu=on`のレンダラ、`process=single`の3通りで確認する |
| `test_blink_spa` | シングルページアプリケーションのセッション履歴。`replaceState`、`pushState`、フラグメントのエントリ、Alt+←と→、ページの`navigation.back()`と`history.go()`、`popstate`と`hashchange`、別のドキュメントから戻ったときの状態のオブジェクトとスクロール位置とNavigation APIのエントリ、アドレスの欄が追従すること(`addr=`の行で欄が出したアドレスを書き出す) |
| `test_blink_mp` | ページを別のサイトへ移すとレンダラが替わり、上限で閉じた前のレンダラのメモリが解放されること、〈戻る〉で前のページのエントリが新しいレンダラで状態のオブジェクトとともに戻ること、レンダラを`kill`してもブラウザが動いたままエラーのページを出すこと(14.13.7) |
| `test_blink_mpeme` | Clear Keyで復号する動画を再生中のページを、同じサイトの別のページへ移ることを4回繰り返してもレンダラが落ちないこと、動画の再生中に移動を送った直後にレンダラを終了させても、ブラウザが動いたままエラーのページを出し、〈再読み込み〉(Ctrl+R)で移ろうとしたページを出すこと |
| `test_blink_mpnest` | `requestAnimationFrame`のコールバックが300msずつ`document.cookie`を読み続け、10回に1回は飛ばされるビューの切替えを始める間に、ブラウザが拡大・縮小で`Layout`を呼ぶ。コールバックの中でタイマー、`resize`、`ResizeObserver`が1つも実行されず(`nested=0`)、レンダラが落ちないこと(14.13.2) |
| `test_blink_ui` | リンクの上のアドレスをメッセージ行に出す、右ボタンの`contextmenu`、Ctrl+＝とCtrl+0の拡大(`devicePixelRatio`)、Ctrl+A・Ctrl+C・Ctrl+Vのトレーを通した複写、`requestFullscreen`とEsc、Ctrl+クリックで別のプロセスのブラウザ、`beforeunload`の確認(取消と了解) |
| `test_blink_face` | ページから見えるブラウザの特性(14.4.12の表)をコンソールに書き出す。ChromeのLinux版が出さない`navigator.share`、`CSS.animationWorklet`、Content Indexが既定で見えないこと。`serif`が`Noto Serif JP`と同じ幅で`sans-serif`と違い、`monospace`が`Noto Sans Mono CJK JP`と同じ幅でラテン文字がどれも同じ幅であること |
| `test_blink_api5` | CSS Paint APIのワークレットが箱を緑に描き、Animation Workletのアニメーション(起動時の行`features=AnimationWorklet`で出す)が不透明度0.5に保つ。別のサイトのiframeがStorage Accessのハンドル(`requestStorageAccess({all: true})`)でファーストパーティの`localStorage`、`sessionStorage`、IndexedDB、Web Locks、Cache Storage、OPFS、見積もり、`createObjectURL`、`BroadcastChannel`を使う。加速度センサーはセンサーがないので`NotReadableError`。アプリのバッジ3がウインドウのタイトルの前に出る |
| `test_blink_mpa` | MSEの`audio/mpeg`と`audio/aac`(RustのMPEGとADTSのヘッダのパーサ)、`<audio>`のMP3とFLAC(Symphonia)の長さと再生(FLACはChromiumのメディアのテストデータの`bear.flac`) |
| `test_blink_wam` | Web Audioの出力先(`setSinkId`の`{type: "none"}`と既定、存在しないデバイスは拒否する)と、マイクの音声を`createMediaStreamSource`から`AnalyserNode`で読むこと |
| `test_blink_mchunk` | `appendEncodedChunks`が既定では無いこと、`features=`で有効にすると、VP8にエンコードしたキャンバスの10フレームをMSEに渡して再生できること |
| `test_blink_iwa` | 起動時の行`isolated=`で許したオリジンで、`crossOriginIsolated`が真で`TCPSocket`、`UDPSocket`、`TCPServerSocket`、`maximize`があり、テストサーバにTCPで要求して応答を読み、`TCPServerSocket`にページのTCPで接続して送ったものを受け、バインドしたUDPに別のUDPから送ったものを送信元のアドレスとともに受け、`maximize()`(ウインドウの管理の許可のパネルにEnterで応答する)でウインドウの幅が画面の幅になること |
| `test_blink_speech` | マイクに半秒の無音、英語の文を話した音(`tests/ktest/data/speech_hello.raw`、Fliteで作った16kHzのモノラル)、無音を渡し、認識が`audiostart`、`soundstart`、`soundend`、`audioend`、`end`を順に通知して「hello」を含む確定した結果を返すこと。続けて、声が`ja-JP`(既定)と`en-US`の2つで、日本語と英語の文がそれぞれ鳴る時間をかけて`end`で終わり、話している間に`cancel()`した発話が`interrupted`で終わり、その後に並べた発話が話されないこと。音声出力のデバイスがないマシンではスキップする |
| `test_blink_wgw` | `make GL=1`のとき、起動時の行`features=WebGLOnWebGPU`でWebGLのコンテキストができ、緑で消した色と、全体を覆う赤い三角形を`readPixels`で読み戻せ、`getError`が0であること |
| `test_blink_glgpu` | `make GL=1`のとき、緑で消したWebGLのcanvasを`copyExternalImageToTexture`でWebGPUのテクスチャにコピーして緑が読め、赤で消したWebGPUのcanvasをWebGLの`texImage2D`でテクスチャにしてフレームバッファから赤が読めること |
| `test_blink_v3dfall` | `make GL=1`のとき、`/v3dfall`のページを`gpufake=`で開く。DRMシムがエミュレートしたV3D(x64ではKaby Lake GT2)で、Mesaのプラットフォームがv3dドライバ(x64ではiris)を選び、screenを作り、確認のフレームが合わずにsoftpipeに戻ること(`driver=v3d step=6`、x64では`driver=iris step=6`。irisの要求がブラウザの`ioctl`で拒否されるとscreenができず`step=5`になる)、その後もWebGL 2のcanvasが赤で消えて読め、レンダラ名が`softpipe`で、2Dのcanvasも描けること |
| `test_blink_webnn` | 起動時の行`features=MachineLearningNeuralNetwork`で`navigator.ml`があり、コンテキストの限度にfloat32とint32があり、定数との行列積から3を引いて`relu`を通したもの、行ごとの`softmax`、3×3の画像の`conv2d`と`maxPool2d`、int32の和とfloat32への変換が、計算どおりの値になること |
| `test_blink_aa` | WebGLの白い三角形を黒に描き、`antialias`が真のときは`getContextAttributes().antialias`が真で`SAMPLES`が4、縁に中間の色の画素があり、偽のときは縁が硬いこと。`make GL=1`のビルド(ANGLEがsoftpipeの上)では、softpipeがANGLEにマルチサンプルを出さないので、`antialias`が偽になってもよい |
| `test_blink_ai` | `LanguageDetector`が使え(`available`)、日本語の文を`ja`、英語の文を`en`と確信度0.5より上で判定し、`Translator`、`Summarizer`、`LanguageModel`は`unavailable`で、`Summarizer.create()`が拒否され、`Writer`、`Rewriter`、`Proofreader`が無いこと |
| `test_blink_pad` | テスト用のソフトウェアのUSBコントローラ(6.3.2、`device/usb/vhc.c`)に挿したLogitech Dual Action(DirectInputのモード)が、ボタンを押すまでは見えず、押すと標準の配置で現れ(ボタン16、軸4)、Aの押しと離し、スティック、十字キーが届き、`gamepadconnected`が1回来ること |
| `test_blink_midi` | 同じコントローラに挿した、送られたものを返すUSBのMIDIデバイスが、入力と出力1つずつとしてデバイスの名前で見え、システムエクスクルーシブを許したアクセスで、ノートと`F0 7D 01 02 03 04 05 F7`を送ると同じものが入力に戻ること |
| `test_blink_pdf` | PDF(PDFiumのテストの`hello_world.pdf`)のページが白く灰色の背景に描かれ、タイトルがファイルの名前になること。ページが`pdfViewerEnabled`の真、5つのプラグインと2つのMIMEの型を見て、iframeのPDFが画像の1枚のページになること |
| `test_blink_pw` | 前の形のブラウザが平文で書いたレコード9(テストが書く)を読めること。パスキーと、`PasswordCredential`の`store()`とログインのフォームのパスワードをパネルで許して保存し、起動し直したブラウザで、そのパスキーで署名し、`get({password})`で選び、フォームの欄にフォーカスして入れること。続いて別のサイトの`localhost`の`/pw3`(3つ目のブラウザ)で、`127.0.0.1`のiframe(`/pwf`)のログインのフォームの欄にフォーカスが移っても、パネルが出ず欄が空のままで(`frame=empty`)、`localhost`には何もないこと(`get=null`)。終わった後、レコード9が暗号化の形で部分が1つ、その鍵の実身の所有者が部分のユーザでモードが`rw-------`、ACLがないこと。ボリュームを書き出してから、起動したディスク全体を読み、3つのパスワードがどこにもないこと(パスワードはページのスクリプトが組み立て、タイトルには合ったかどうかだけを出し、テストの中では逆順で持つので、蓄えやタイトルやカーネルのイメージには現れない) |
| `test_blink_av1` | AV1を扱えると返すこと(`canPlayType`、MSE、WebCodecs、MediaCapabilitiesの480pはなめらか、1080pはなめらかでない)と、WebMのAV1の480×360の動画がフレームを出して再生されること(Chromiumのメディアのテストデータの`bear-av1-480x360.webm`)。キャンバスの1フレームを`VideoEncoder`でAV1のキーフレームにエンコードし、MediaRecorderがAV1を受け付けること |
| `test_blink_mseav1` | MSEでAV1を扱えると返すこと(`isTypeSupported`のMP4とWebMの`av01`、MediaCapabilitiesの`media-source`の640×360)と、断片に分けたMP4のAV1(`bear-av1.mp4`、320×240)とWebMのAV1(480×360)をそれぞれ`SourceBuffer`に入れて、フレームを出して再生すること。読めないデータを入れた要素がエラーで終わったあと、別の要素でWebMのVP9が再生されること |
| `test_blink_wv` | 導入していないとき、`/wv`のページの`com.widevine.alpha`が`NotSupportedError`、Clear Keyが使えること。メニューの〈Widevineを導入...〉のパネル7にEscで応答すると何も取得しないこと。`make ... WV_TEST=<ディレクトリ>`で、開発マシンでリポジトリから取得したインデックスとパッケージ(`wv_Packages`、`wv.deb`。x64ではコンポーネントの更新のサービスの応答のアドレスをテストサーバのものにした`wv_update.json`と、CRX3の`wv.crx3`)をテスト用のディスクの`/boot`にだけ載せたときは、起動時の行`wvrepo=`でテストサーバのコピーを入手元とし、パネル7にEnterで応答すると、取得、SHA-256の照合(x64ではCRX3の署名も)、展開を経て、レコード10が指すモード`rw-------`の実身にライブラリが入り、同じページで`SW_SECURE_DECODE`のWidevineが使え、`HW_SECURE_ALL`は拒否されること。`NET=user`では続けて`/wvplay`のページで、Shaka Player(4.12.0)がデモのWidevineのストリーム(Angel One、H.264とAAC、CENC)をデモのライセンスサーバの鍵で3秒より先まで再生すること(インターネットに到達できずShaka Playerを取得できないときは試さない)。起動し直したブラウザが実身からCDMを読み込み(コンソールにバージョンが出る)、〈Widevineを外す〉をパネル2の了解で選ぶと、行と実身が消え、Widevineが拒否されること |
| `test_blink_find` | ページの中の検索。Ctrl+Fのパネルに「apple」、3つの一致のうち次、次、前へ移り、表示がその中ほどへ動くこと、アクティブな一致の橙色 |
| `test_blink_keep` | フォームの`POST`、次に起動したブラウザがCookieと`localStorage`を読み戻す |
| `test_blink_media` | WebGL(三角形の描画と画面の画素、`readPixels`、2×2のテクスチャを最近傍で貼ったフレームバッファオブジェクトの画素、WebGL 2のコンテキストでGLSL ES 3.00の頂点配列オブジェクトと整数のユニフォームで描いた色)、音声の長さ、`crypto`、`preload`のない動画を取得しないこと。`GL=1`でないときはWebGL 2がないことを確認する |
| `test_blink_gpu` | `make GL=1`のときだけ。`/g`のページを`gpu=on`(softpipeでもGPUの経路を使う)と`gpucheck=`の行で開く。ページは、角の丸い箱のグラデーションと影、文字、1色の箱、画像、2Dのcanvas(塗り、円、グラデーション、文字、`putImageData`、別のcanvasへの`drawImage`)、WebGLの三角形、1色のVP9の動画(64×48、`/g.webm`、ループで再生)を持つ。画面の画素がそれぞれの色であること、`getImageData`がGPUのcanvasから読み戻した色(塗り、円、置いた四角、別のcanvasにコピーした塗り)と、動画のフレームが表示されたことをタイトルで確認する。プロセッサで描き直したビューと比べて48より大きく違う画素が0.5%以下であること、動画のフレームがYUVのプレーンとしてGPUに渡ったこと、canvasがGPUでラスタ化されたことを、`gpucheck=`のファイルで確認する |
| `test_blink_wasm` | WebAssemblyのモジュールをスクリプトのバイト列と`/t.wasm`(`instantiateStreaming`)から作り、足し算、テーブル経由の呼出し、メモリの読み書き、10万回のループ、トラップを確認する。1ページのメモリのアドレス0、65532、65536、2147483647の読出しが0、0、トラップ、トラップになる(後の2つはトラップハンドラによる) |
| `test_blink_image` | 画像へのアドレス(16×16のPNGとJPEG)を直接開くと、画像のドキュメントとして等倍で描かれる。2000×1000のBMP(左が赤、右が青)はウインドウに縮めて両方の色が見え、赤の側をクリックすると等倍になって赤だけが見える(テストはデスクトップを起動してクリックをウインドウに渡す) |
| `test_blink_worker` | `blob:`のURLから専用ワーカーを作ってすぐにURLを取り消し、ワーカーが`importScripts`で`/wk.js`を読み、転送された`ArrayBuffer`の和と別のBlobのURLを`fetch`で読んだ中身を返す。ページはBlobの`text()`、`slice`、`blob:`のURLの`fetch`、応答の`blob()`を読む |
| `test_blink_shared` | 同じアドレスと名前の`SharedWorker`を2つ作ると1つのワーカーになり(`/sw.js`を1度だけ求める)、それぞれのポートに送ったメッセージに、接続の番号を付けて応答する |
| `test_blink_file` | トレーに文章の実身の仮身を置き、`/fl`の`<input type="file">`を押してパネル6でEnterで選ぶと、ページがファイルとして文字を読む。続けてページが保存する`download`属性のリンク(`/dl`)、すぐに取り消したBlobの`blob:`のURL、`data:`のURLと、添付(`Content-Disposition: attachment`)で応答する`/att`が、ブラウザの保存箱の4つの実身になり、添付は表示されない |
| `test_blink_codecs` | `/wc`のページが、VP8の`isConfigSupported`、赤いcanvasの3フレームのVP8のエンコードとデコード(最初のフレームの画素が赤)、Opusのエンコードとデコード、canvasで作ったPNGの`ImageDecoder`でのデコードをタイトルに書く |
| `test_blink_print` | `/pr`のページが`window.print()`を呼ぶと、デスクトップの印刷設定のパネルが出て、Enterで1ページのPDFができる(`print=`の行で、印刷したページ数、デスクトップの応答、文字のまま印刷したブロブの数を書かせる。見出しと段落の2つ以上)。`afterprint`がページに届く |
| `test_blink_drag` | `/dd`の赤い箱をポインタで緑の箱までドラッグすると、緑の箱がドラッグした文字を受け取る。続けて文章の実身の仮身(`wm_obj_drop`)と文字(`wm_obj_drop_text`)を緑の箱の上にドロップすると、ページがファイルの文字とドロップされた文字を受け取り、ドロップしたファイルの`getAsFileSystemHandle()`のハンドルからも同じ文字を読む |
| `test_blink_session` | `/ms`のページがメディアセッションのタイトルと操作の受け手を置き、メニューの〈メディア〉〈再生/一時停止〉と〈次の曲〉(`menu=`の行)が`play`と`nexttrack`として届く |
| `test_blink_sw` | `/swr`のページがService Workerを登録し、ワーカーが`clients.claim()`でページを制御する。ページの`/swr-data`の取得と、ページが移るアドレスにワーカーが応答し(サーバは`/swr`とワーカーのスクリプトしか要求されない)、ページとワーカーが`postMessage`でメッセージを交わす |
| `test_blink_swframe` | `/swf`のページがService Workerを登録して制御された後、`/swf-frame`のiframeを置く。ワーカーはその移動の`fetch`イベントに`/swf-inner`を取得して応答し、iframeのドキュメントは制御され、その`/swf-data`の取得にもワーカーが応答する。ワーカーはiframeからのメッセージに、クライアントが2つ(`top-level`と`nested`)と応答する。サーバは`/swf-frame`と`/swf-data`を要求されない |
| `test_blink_swredir` | `/swd`のページがService Workerを登録して制御された後、`/swd-sub`を取得する。ワーカーは`fetch(event.request)`で応答し、サーバの302に従った`/swd-subto`の本文がページに届く。続いて`/swd-a`へ移る。ワーカーは`fetch(event.request)`で応答し、サーバの302は`opaqueredirect`になって移動がそれに従う。`/swd-b`にはワーカーが`Response.redirect()`で`/swd-c`へのリダイレクトを返し、`/swd-c`はネットワークから取得する。`/swd-c`のドキュメントが制御されていることと、ワーカーが見た要求のmodeとredirect modeを確認する。サーバは`/swd-b`を要求されない |
| `test_blink_owl` | 専用ワーカーが`OffscreenCanvas`のWebGLで赤く消した画素を読み戻すと`255,0,0,255`(`make GL=1`)。ページの`requestAnimationFrame`のコールバックとタイマーがそれぞれ120ミリ秒かかり、`long-animation-frame`のエントリ(50ミリ秒以上、スクリプトの記録あり)が届く |
| `test_blink_glw` | `/glw`の2つの専用ワーカーが`OffscreenCanvas`のWebGL 1と2で、ページがWebGLのコンテキストを次々に(4秒の間、少なくとも5つ)作って、数値を含むシェーダをコンパイルしながら描き続け、読み戻した画素が赤(ワーカー)と緑(ページ)で、プログラムのリンクがすべて成功し、ブラウザが落ちない(`GL=1`でないときワーカーのWebGL 2は`none`)。描いた後、WebGL 2のワーカーは画素を`PIXEL_PACK_BUFFER`に読み出してフェンスを置き、`clientWaitSync(sync, 0, 0)`をタイマから3秒まで繰り返して待ってから`getBufferSubData`で読む。どちらのワーカーも、リンクを`KHR_parallel_shader_compile`の`COMPLETION_STATUS_KHR`で同じく待つ。`GL=1`では、フェンスがシグナル状態になって読んだ画素が赤で、リンクが完了し、`getError`が0であること。ページの`@font-face`のフォントは、HTTPキャッシュだけからの読込みが`ERR_CACHE_MISS`になった後、サーバに要求される。ページの実身の本文の`trace=exceptions`の段落で、例外がコンソールに出る |
| `test_blink_csp` | `/csa`(CSPでnonce`aaa`を求める)のスクリプトが`/csb`(nonce`bbb`)へ移り、`/csb`のnonce付きのインラインと外部のスクリプトが動く(前のドキュメントのCSPの制約を受けない)。nonceは属性から隠れ、プロパティには残る。ほかのオリジン(`localhost`)への`redirect: 'manual'`の`fetch`は、CORSのヘッダの付いたリダイレクトで止まって`opaqueredirect`になり、その先(`/cst`)は要求されない。同じオリジンへの`POST`の`fetch`とXMLHttpRequestは`Sec-Fetch-Site: same-origin`と`Origin`を付けて送られ、CORSで拒否されない。続けて`http://localhost:18172/csc`を開くと、ほかのサイト(`127.0.0.1`)へリダイレクトされ、そのリダイレクトの応答が付けた`SameSite`のないCookieが残り、移った先のページの同じオリジンへの要求で送られる |
| `test_blink_hist` | iframeの別のドキュメントへの移動とフラグメントで`history.length`が1つずつ増え、`history.back()`でフラグメント(`hashchange`)、続いて前のドキュメント(`back_forward`)に戻る。iframeの再読込は`reload`、リダイレクトで来たドキュメントは`redirectCount`が1。同期のXMLHttpRequestの`statusText`が`Funky Reason`。箱の`behavior: 'smooth'`のスクロールはすぐには動かず、2.5秒後に移動先にある。専用ワーカーの`requestAnimationFrame`が時刻を受ける。1.5秒空けて2回に分けて送る本文を、`fetch()`が届いた分ずつ読む |
| `test_blink_api2` | `/api2`のページで、テストサーバの`/ws`との`WebSocketStream`の往復、支払いの手段がないことを返す`PaymentRequest`、パスキーを作って署名でログインする(2つのパネルにEnterで応答し、署名を返された公開鍵で`crypto.subtle`が検証する)、テストサーバをIDプロバイダとするFedCM(アカウントをEnterで選び、トークンを受け取る) |
| `test_blink_fsa` | `/fsa`の1つめのボタンで`showSaveFilePicker()`の名前のパネルにEnterで応答し、ブラウザの保存箱に実身ができて仮身がトレーに入る。2つめのボタンの`showOpenFilePicker()`でその仮身をパネル6で選び、書いた文字を読み戻す |
| `test_blink_devices` | `/dev`のページが、電池(充電中で満充電)、画面の消灯の防止、キーボードの配列の表、関連アプリ、2つめの画面、アプリのバッジ、Bluetooth(アダプタなし)、MIDI(許可のパネルにEnterで応答し、ポートなし)、HIDとUSBのデバイス(なし)、ゲームパッド(4つの空き)、画面の向き(`landscape-primary`、固定は`NotSupportedError`)、`navigator.connection`(`4g`)、折り畳み(`continuous`)、仮想キーボード、ウインドウの制御の領域、プロトコルのハンドラの登録、`launchQueue`、加速度センサー(なし)の結果をタイトルに書く。ボタンを押すと、HIDとUSBのデバイスを求め(どちらも選ぶものがない)、`navigator.share()`の文字がトレーに入る |
| `test_blink_cookie` | `/ck`のページが、`document.cookie`で入れたCookieを`cookieStore.get`で読み、`cookieStore.set`で入れたものが`document.cookie`に現れ、`set`の`change`イベントが届き、`getAll`の数と`delete`を確認する。初めに自分のCookieを消し、数えるのも自分のものだけにするので、前に実行したテストが同じホストに残したCookie(Cookieはポートで分かれない)に左右されない |
| `test_blink_serial` | `/ser`のボタンを押すと`requestPort()`のパネルが出て、Enterで最初のポート(コンソールの`sera`)を選ぶ。ページがポートを開いて`webserial-hello`の行を書き(ログに出る)、しばらく読んで、`getPorts()`の数を数えて閉じる |
| `test_blink_share` | パネルの〈取消〉で`NotAllowedError`になる。ブラウザの後ろに隠れた赤いウインドウを選ぶと、`displaySurface`が`window`で、ラベルがウインドウの名前で、フレームが赤い(画面からでなくウインドウ自身のピクセルから読んでいる)。閉じると`ended`。ブラウザ自身のウインドウを音声付きで選ぶと`browser`で、音声のトラックにオシレータの音が入る |
| `test_blink_share2` | メモリの出力「Test-2」(640×480と800×600)を主画面の右に置く回と左に置く回。どちらの回も、ブラウザのウインドウが主画面に開く。パネルに画面ごとの行が出て、画面 2を選ぶと`monitor`の640×480で、その画面に開いた赤いウインドウが写る。左の回は画面 1が主画面の大きさで赤くない。共有中に出力のレイヤで画面を変える(`ts_disp_commit`)。右の回は800×600にして、フレーム、`getSettings()`、ビデオ要素の大きさが800×600になり、主画面の左へ移すと、移した先の青いウインドウが写る。左の回は主画面の上へ移し、青いウインドウが写る。画面を切るとトラックが`ended`になる |
| `test_blink_storebusy` | 利用者の実身をテストが40秒近く持ったままブラウザを起動してCookieを追加する。読めなかった間の保存で前のCookieが消えず、次の起動で古いCookieと新しいCookieの両方がある |
| `test_blink_ext` | 2つの拡張機能を読み込む。権限のパネルの〈取消〉と〈読み込む〉、設定のタブの見た目、CRXとモジュールのワーカー、deflateのZIP。ルールが画像、`fetch`、フレーム、ウインドウのドキュメント、WebSocketに効くこと、本当のリダイレクト(`redirected`が真)、`regexSubstitution`、`transform`、`regexFilter`、動的ルール、ヘッダの両方向の`modifyHeaders`。`scripting`、`alarms`、`i18n`、見るだけの`webRequest`、`action`、`contextMenus`、`tabs.sendMessage`、`tabs.create`、拡張機能のページ。宣言の少ない拡張機能が、名前を持たず、直接呼んでも拒否され、ホストのない`redirect`と`modifyHeaders`が効かず、CORSを越えられないこと。`document_idle`のスクリプトが1回だけ実行されること。次の起動で一覧の仮身から読み、無効にする、削除する |
| `test_blink_ubol` | `make EXT_TEST=<ZIP>`でテスト用のディスクに載せた実在の拡張機能を読み込み、ルールとワーカーとボタンのページをコンソールと画面で確認する。ZIPはリポジトリにもテストの既定にも入れないので、与えないときはスキップする |
| `test_blink_meet` | `/meet`のページで、送信側のコーデックにVP8、VP9、H.264、AV1とOpusがあり、カメラの許可の問合せが`prompt`、`crossOriginIsolated`が偽であることを確認する。VP8、VP9、H.264、AV1のそれぞれについて、2つの`RTCPeerConnection`で640×360、毎秒30フレームのcanvasをそのコーデックだけで送り、エンコードとデコードの毎秒のフレーム数、大きさ、エンコーダの名前を書き出す(QEMUでは実機より遅いので値は記録として出す)。最初の組ではオシレータの音も送り、受信側の`<audio>`が再生して、受信した音声のサンプルの数(`totalSamplesReceived`)が増えること(WebRTCの音声が取り出されていること)を確認する。ボタンを押して`getDisplayMedia`を呼び、パネルにEnterで応答すると、`displaySurface`が`monitor`で音声のトラックのないストリームが来て、そのフレームが読めることを確認する |
| `test_blink_coi` | `Cross-Origin-Opener-Policy: same-origin`と`Cross-Origin-Embedder-Policy: require-corp`で返す`/coi`のページが、`crossOriginIsolated`が真で、`SharedArrayBuffer`と共有の`WebAssembly.Memory`を持ち、`SharedArrayBuffer`を渡した専用ワーカーも分離されていて同じメモリに書けることを確認する。別のオリジン(`localhost`)の画像は、`Cross-Origin-Resource-Policy: cross-origin`のものは読め、何も指定しないものは読めない。同じオリジンのiframeは、COEPを持つドキュメントは入り、持たないドキュメントはブロックされてエラーのドキュメントになる。IndexedDBで、アップグレードの最中に要求した2つ目の`open`がアップグレードの作ったストアで`transaction()`でき、バージョン2へのアップグレードが2つの接続に`versionchange`を通知して閉じさせてから動き、削除が接続の閉じるのを待って終わる |
| `test_blink_rtc` | `/rtc`のページの2つの`RTCPeerConnection`を、オファー、アンサー、候補の受け渡しで接続し(ICEが`connected`になる)、データチャネルを開いて両方向にメッセージを送り、canvasの映像のトラックを送って、受信側が`track`イベントを受け、フレームをデコードしたこと(`getStats`)を確認する |
| `test_blink_api3` | `/api3`のページをクリックすると(`queryLocalFonts`は利用者の操作を要する)、WebXR(没入型なし、インライン)、`navigator.ink`、メディア要素の`remote`、`navigator.login`、CSSのペイントのワークレットのモジュール、`getScreenDetails`(許可のパネルにEnterで応答し、画面が1つ)、`queryLocalFonts`(許可にEnter、フォントがある)を確認する。クリックの前に、ストレージのバケットのデータベースとキャッシュが既定のバケットと分かれることとバケットの削除、古いFile System APIのファイルの書込み、一覧、読み戻しを確認する |
| `test_blink_webgpu` | `make GL=1`でないときは、`/wgpu`のページの`requestAdapter()`がnullを返すことだけを確認する。`make GL=1`では、ページが、互換モードのアダプタとデバイスを得て、コンピュートシェーダ(1、2、3、40を3、5、7、81に)を実行してバッファをマップして読み、緑に消したcanvasの左下半分に赤い三角形を描く。`drawImage`で2Dのcanvasにコピーした色と、画面のcanvasの色を確認する。デバイスのエラーがないこと |
| `test_blink_windows` | `/win`のページの右の4つのボタンを順に押す。`window.open`で開いたウインドウ(`/winc`)のタイトルを開いた側が読み、`postMessage`の往復ができ、そのウインドウの入力欄にローマ字で打った「あい」がかな漢字変換で入り、開いた側が閉じると`closed`が真になる。ドキュメントのピクチャインピクチャのウインドウが求めた200×150で、開いた側の要素を移せる。`noopener`のウインドウは`window.open`がnullを返し、別のプロセスのブラウザが`/wina`を開いてタイトルをファイルに書く。動画のピクチャインピクチャのウインドウに入り(ウインドウは64×60)、出ると`leavepictureinpicture`が届く |
| `test_blink_notify` | `/note`のページの3つのボタンを順に押す。表示の許可をパネルで与え、ウインドウの通知をデスクトップの通知のウインドウの〈開く〉で押すと`click`が届く。Service Workerの通知は`getNotifications`に1件あり、そのボタン〈返信〉を押すとワーカーに`notificationclick`(`action`が`a1`)が届く。3つ目の通知を〈閉じる〉で閉じると`close`が届く |
| `test_blink_push` | テストサーバがautopushと同じプロトコルで動くプッシュサービス(`/push`のWebSocket)を持つ。`/pushp`のページが購読し(通知の許可をパネルで与える)、購読の宛先がサービスのもので、鍵が起動時の行`pushkey=`で与えたもの(RFC 8291の例の受信側)であり、`getSubscription`が見つける。サービスがhttp_eceで暗号化して送るメッセージ(aes128gcmとaesgcmの2つ)が復号され、ワーカーの`push`イベントの文が送ったものになり、受信確認が返る。ブラウザを起動し直して`/pushq`を開くと、前の起動で登録したワーカーがページを制御し、購読が残り、ブラウザが前の`uaid`を示し、サービスが持っていたメッセージがワーカーに届く。最後に購読をやめ、登録を消す |
| `test_blink_api4` | `/api4`のページで、`navigator.plugins`と`mimeTypes`があり、最上位の文書がストレージを持ち、メディア要素の出力先が既定(`""`)で`setSinkId('')`が通り、DeviceOrientationのイベントがあり、`PressureObserver`にプロセッサの状態が届き、`MediaStreamTrackProcessor`がcanvasのストリームの16×8のフレームを読む。ボタンを押して`EyeDropper`を開き、赤い箱を押すとその色(`#c80000`)が返る。`encodingInfo`(`webrtc`の型、VP8)が対応と返す。オリジンごとのファイルシステムで、ファイルを書いて読み戻し、一覧、`resolve`、エントリの削除ができ、ワーカーの同期アクセスのハンドルが書き、読み、大きさを返し、切り詰める。ワーカーの`MediaSource`の`handle`をページの動画の`srcObject`にすると、VP9の動画が64×48で再生される。ブラウザを起動し直して`/api4b`を開くと、2つのファイルが残っていて、消せる |
| `test_blink_dir` | ファイルのレイヤのフォルダ(`a.txt`と、`b.txt`のある`sub`)をファイル変換で実身(仮身を並べた図形)にしてトレーに置く。ページのボタンで`showDirectoryPicker`を開き、パネル6にEnterで応答すると、ページがフォルダとその中のフォルダを一覧し、`a.txt`を読み、`c.txt`を書く。ページが変えたフォルダは新しい実身になり、その仮身がトレーに入る。その前に、`webkitdirectory`の`<input type="file">`を押してパネル6にEnterで応答すると、ページに`fsadir/a.txt`と`fsadir/sub/b.txt`が相対パス付きで渡り、中身を読める |
| `test_blink_mic` | サウンドデバイスの録音に、テストが1kHzの方形波を渡す(`knl_snd_loopback`)。`/mic`のページで、入力デバイスに「マイク」があり、`getUserMedia`(パネルにEnterで応答する)の音声のトラックが48000Hzで、`MediaStreamTrackProcessor`で読んだ音声が聞こえる大きさである |
| `test_blink_cam` | カメラデバイスのカーネル自身のカメラ(`knl_cam_loopback`、YUY2の64×48)に、テストが明るさ200のフレームを渡す。`/cam`のページで、`getUserMedia`(パネルにEnterで応答する)の映像のトラックが64×48で、入力デバイスに「Test camera」があり、`MediaStreamTrackProcessor`で読んだフレームが`I420`の64×48で明るさが200であり、`ImageCapture`の`takePhoto`がJPEGを返す |
| `test_blink_spell` | `/sp`のページの入力欄(`contenteditable`)を押して空白を打つと、入力欄の誤った綴りの下に赤い線が出て、入力欄の外の段落の同じ語には出ない(画面の赤い画素を数える) |
| `test_blink_webusb` | QEMUのUSBシリアル(FTDI。クラスドライバが取らない。入力を`/dev/zero`にする。入力が終わるとQEMUがデバイスを抜くため)をQMPで差し、`/usb`のページのボタンで`requestDevice`を開いてパネルにEnterで応答する。ページがそのデバイスを開き、構成とインタフェース、コントロール転送(速度の設定)、バルクのOUT(5バイト)とIN(状態の2バイト以上)、リセットとその後のコントロール転送、閉じる、`getDevices()`が1台を返すことをタイトルで確認する |
| `test_blink_webusb_iso` | 2台目のQEMUのUSBオーディオを、オーディオのドライバが取らないようにして差す(`knl_uac_leave`、テストのときだけ)。ページが再生のインタフェースの2つ目の設定を選び、アイソクロナスのOUTに8パケットを送り、全部が送れたことを確認する |
| `test_blink_webhid` | 2台目のQEMUのUSBマウスを、HIDのドライバが取らないようにして差す(`knl_hid_leave`)。ページが`navigator.hid.requestDevice`で選び、コレクション(Generic DesktopのMouse)と入力レポートの数、開く、`getDevices()`を確認し、QMPでマウスを動かすと入力レポートが届く |
| `test_blink_zone` | システムのタイムゾーンを`Asia/Tokyo`にしてブラウザを起動し、動いている間に`America/New_York`に替える。ページのタイトルが、起動したときのタイムゾーン(`Intl`の名前と2026年7月1日の差、540分)と、替えた後のもの(-240分、夏時間)を示す |
| `test_blink_cam_mjpeg` | カーネル自身のカメラをMJPEGだけにし、明るさ200の灰色のJPEGを渡す。トラックのフレームが`I420`の64×48で、明るさが映像の範囲の値(16+219×200/255、188)になる |
| `test_blink_sync` | `/syncp`のページで、定期の同期の許可が与えられ、一度の同期がすぐワーカーの`sync`イベントになり(最後の機会ではない)、定期の同期(起動時の行`periodicsync=`で間隔を3秒に)が2回以上起き、Content Indexに記事が入り、`navigator.vibrate`が真を返し、2つのファイルのバックグラウンドの取得がワーカーの`backgroundfetchsuccess`に届いて中身が読め、ページにも成功と伝わる。動画の`requestVideoFrameCallback`が3回呼ばれてフレームの情報が入り、canvasの`captureStream`を`srcObject`にした動画が32×24の緑で、長さが無限になる。キャッシュに応答を入れる。ブラウザを起動し直して`/syncq`を開くと、定期の同期、記事、キャッシュの応答が残り、定期の同期がまた起きる。それらを消すとなくなり、登録が消える |
| `test_blink_capture` | `/cap`のページが、デバイスの一覧、`getUserMedia`(マイクとカメラ)、音声認識の結果をタイトルに書き、canvasの`captureStream()`を`MediaRecorder`でVP8のWebMに録画する。判定は、ブラウザと同じ方法でマシンにマイク(48kHzで録音できる音声デバイス)があるかを調べて選ぶ。マイクがなければ、デバイスの一覧が空で、認識がパネルを出さずに`audio-capture`のエラーになること、あれば(Intel HDAなど)、一覧にマイクが1つあり、認識が`audio-capture`にならないこと(許可のパネルには応答しない)を確認する。カメラはどちらでもないので、`getUserMedia`は`NotFoundError`になる |
| `test_blink_iframe` | `/ifc`のiframe(赤)と`srcdoc`のiframe(青)がそれぞれの場所に描かれ、両方が`postMessage`し、ページが前者の`contentDocument`のタイトルを読む |
| `test_blink_api` | HMACの署名と検証、AES-GCMの暗号化と復号、gzipの圧縮と伸長、BroadcastChannel、Web Locks、Permissionsの問合せ(`prompt`)、`Notification.permission`(`default`)、保存の割当て、`sendBeacon`(サーバが受け取る)、`createImageBitmap`、`OffscreenCanvas`の2D |
| `test_blink_storage` | ページが`localStorage`と`sessionStorage`を変えると、同じオリジンのiframeに`storage`イベントがキー、古い値、新しい値、Storageの種類、ページのアドレスとともに届き、ページ自身には届かない |
| `test_blink_dialog` | `alert`、`confirm`2回、`prompt`のパネルにキー(Enter、Esc、`12`とEnter)で応答し、`<select>`を押して出た一覧のパネルで2番目を押してEnterで選ぶ。タイトルがそれぞれの応答と`change`の値になる |
| `test_blink_clip` | `navigator.clipboard.writeText`で2行の文字列をトレーに入れ、`readText`で読み戻す。読み取りの許可のパネルにEnter(許可)で応答し、その後の問合せが`granted`になる |
| `test_blink_cache` | `caches`のキャッシュに応答を入れて読み戻し、`ignoreSearch`の照合、キャッシュの名前、キー、エントリの削除、キャッシュの削除と`has`を確認する |
| `test_blink_cachekeep` | `caches`のキャッシュに512KBの応答を24個入れ、保存された後に小さい応答を1つ追加する。2度目の保存は追加した1つだけを書く(コンソールの`store caches saved`の行)。次に起動したブラウザが25個を読み戻し、小さい応答の本文と大きい応答のバイト列が元のとおりであることを確認し、キャッシュを消す(14.8.1) |
| `test_blink_cachefull` | プールを16MB(`quotapool=16384`、オリジンの上限は12MB)にして、1MBの本文を16個`put`する。12個目からが`QuotaExceededError`で拒まれ、入った11個を次に起動したブラウザがすべて本文ごと読み戻す |
| `test_blink_csevict` | プールを8MBにして、127.0.0.1とlocalhostの2つのオリジンが交互に`put`する(3MB、3MB、1MB、2MB)。最後の`put`でプールを超え、古い方の127.0.0.1がまるごと追い出され(コンソールの`evicted ... for room`の行)、localhostの`put`はすべて成功する。127.0.0.1のページは自分のキャッシュが空なことを確認する |
| `test_blink_csquota` | プールを8MBにして、localhostが1MB、127.0.0.1が8MBを`put`する。127.0.0.1は6個目から`QuotaExceededError`で拒まれ、`estimate()`は上限6144KBと使用量約5MBを返す。プールには空きがあるので、localhostは追い出されず、1MBを追加して`estimate()`が約2MBを返す |
| `test_blink_cspersist` | プールを8MBにして、導入したアプリとして扱う127.0.0.1(`persist=`)が`persist()`を許されて4MBを、localhostが3MBを`put`する。次に起動したブラウザは、127.0.0.1の使用量、最後に使ってからの秒数、永続化をコンソールに出す。localhostの2MBの`put`は、127.0.0.1を追い出さず、localhostの上限(プールから127.0.0.1の分を引いた4MB)を超えるので拒まれる。127.0.0.1の`persisted()`はtrueで、4つの本文が元のとおりである |
| `test_blink_cachecut` | 1日キャッシュしてよい`/ccut.js`の最初の応答を半分で止め(接続は開いたまま)、その間にブラウザを終了する。次に起動したブラウザが`/ccut.js`をもう一度サーバに要求し、全体を実行する(タイトル`ccut whole`)。半分をキャッシュに残すと、それが応答の全体として返り、構文エラーでタイトルが変わらない(14.8.1) |
| `test_blink_jit` | `/j`のページが、再帰と型付き配列のふるいのループと、WebAssemblyのループにかかった時間をタイトルに書く。ブラウザをそのままと、`v8=--jitless`の行を与えて2回開き、値が正しいこと、機械語にしたJavaScriptの時間がインタプリタの半分より短いこと、`--jitless`ではWebAssemblyがないことを確認する |
| `test_blink_socket` | テストサーバの`/ws`との`WebSocket`でテキスト、Blob、バイナリのメッセージを往復させ(Blobの後に送ったものが追い越さない)、1000で閉じる。`/ev`の`EventSource`で、名前の付いたイベントと2つのメッセージを、ストリームが開いたままのうちに受け取り、閉じると接続が切れる |
| `test_blink_idb` | `indexedDB`でデータベースを作り、キー、件数、インデックス、逆向きのカーソルで読み戻す。Blob、File、大きな文字列を含む値を入れ、Blobをすぐ読み戻す。次に起動したブラウザが、バージョン、レコード、ストアが作ったキー、BlobとFileの中身(Fileの名前と時刻も)、大きな文字列の長さを読み戻す |
| `test_blink_audio` | `AudioContext`のオシレータを鳴らし、コンテキストが48kHzで動き、時刻が進み、アナライザに波形が出ること、サウンドデバイスに渡したフレーム数(`sound=`の行で書かせる)が1.5秒分以上あることを確認する。音声出力のデバイスがないマシンではスキップする |
| `test_blink_menu`、`test_blink_tool` | ウインドウのメニューの再読込、ツールパネルでの移動とボタン、ツールパネルがページのウインドウのすぐ前にあること |
| `test_blink_mem` | 大きなページを読んだ後のメモリの使用量 |
| `test_blink_frame` | `/fr`のページを、ビュー全体を描いてコピーする方法(`draw=copy`)、変わった部分だけをサーフェスに描く方法、それに各表示をビュー全体と比べる方法(`check=`)で3回開き、箱の色の変化、キャレットの点滅、ホイールの3つの場面で、ブラウザの描画の時間(`stat`の行で、F12を押すと出す)とウインドウマネージャが合成した画素の数を出す(14.7.1)。比べた画素が1つも違わないこと、箱とキャレットの場面で合成した画素が前の4分の1より少ないことを確認する |
| `test_desk` | デスクトップからページの実身を開く、同じ実身をもう一度開く |
| `test_blink_stuck` | 応答しないページ(テストサーバが`/hang`に応答しない)と、終わらないスクリプトのページ(`/busy`)を、読込みの終わりを待たずに開く。それぞれで、右ボタンでメニューが出ること、Escの中止でページの表示まで進むこと(`ready=`のファイルができる)を確認し、開き直して、応答しないページではCtrl+Eで、スクリプトのページではピクトグラムのダブルクリックで、8秒以内にプロセスが終わることを確認する |
| `test_blink_home` | メニューを右ボタンで出し、〈編集〉の下の〈ホームページに設定〉をポインタで選ぶと、ページの実身の本文が表示しているページのアドレスになることを確認する |
| `test_https` | `NET=user`のときだけ。外部のサイトを開いて画面のスクリーンショットを撮る。期限切れの証明書のサイトを拒否する |
| `test_http3` | `NET=user`のときだけ。HTTP/3を使えるサイト(`https://cloudflare-quic.com/`、`https://www.google.com/`)を開き、続けて同じサイトのページを2つ開く(`then=`の行)。応答の`Alt-Svc`の直後の要求は、確立途中のQUICの接続と競合して既存のTCPの接続で送られることがあるためである。ブラウザが終わるときに書く、プロトコルごとの応答の数と最初の応答のプロトコル(`proto=`の行)で、HTTP/3(`h3`)で届いた応答があることを確認する。QEMUのユーザモードネットワークがUDPを外部へ通すことも、これで確認する。続けて、ブラウザを開き直し、キャッシュにない各サイトのページを1つ開くと、最初の応答からHTTP/3で届くこと(サーバについて知ったことの持ち越し。HTTPSレコードの効果が入らないように、名前はシステムの名前解決で引く。`dns=system`の行)、知ったことを使わずに(`forget`の行)1つのページを開いても、HTTPSレコードによってHTTP/3で届く応答があることを確認する |
| `test_netswitch` | `NET=user`のときだけ。ネットワークの切替え(14.5.3)。makeに`TEST_NETDEV='-netdev hubport,id=n0,hubid=0 -netdev user,id=u1,ipv4=on,ipv6=on -netdev hubport,id=p1,hubid=0,netdev=u1 -netdev user,id=u2,net=10.1.2.0/24,ipv6=off -netdev hubport,id=p2,hubid=0,netdev=u2'`を与え、カードをQEMUのハブに接続し、その先に2つのユーザモードネットワーク(10.0.2.0/24と10.1.2.0/24)を置く。初めにQMPのヘルパの`set_link`でu2を切り離してu1のアドレスを取得し、ブラウザで`https://example.com/`を開く。表示の後、カードのリンクを下げ、u1を切り離してu2を接続し、リンクを上げて、前のアドレスを持ったままDHCPで問い合わせ直させ(9.3.1)、アドレス、ゲートウェイ、DNSサーバがすべて10.1.2.xになることを確認する。ページのスクリプト(`script=js:`の行)は表示の45秒後に、前から接続のある`https://example.com/`と、まだ名前解決していない`https://www.wikipedia.org/`を並べて`fetch`し(それぞれ60秒まで)、結果をタイトルに書く。両方が届くことを確認する。2つ目のネットワークが無いときはアドレスが変わらず、スキップする。終わるとu1に戻す |

`test_blink_image`、`test_blink_dialog`、`test_blink_clip`、`test_blink_file`、`test_blink_print`、`test_blink_drag`はデスクトップを起動し、デスクトップが前面のウインドウに渡す入力でクリックとキーを与える。テストサーバのポートはテストごとに変え(`HTTP_PORT`に足す数。`test_blink_frame`は20から22を使うので、2026年10月7日に追加したテストは31から使う)、前のテストの閉じた接続が残っていても開けるようにしている。

2026年10月7日、ワーカーとBlob、iframe、14.4.5のサービス、画像のドキュメント、ページからの問合せのパネル、クリップボード、Cache Storageを入れて、`test_blink`、`test_blink_image`、`test_blink_wasm`、`test_blink_worker`、`test_blink_iframe`、`test_blink_api`、`test_blink_storage`、`test_blink_dialog`、`test_blink_clip`、`test_blink_cache`が通った。続けて共有ワーカーを入れて、`test_blink_shared`が通った。さらにファイルの選択と保存、WebCodecs、印刷、ドラッグ&ドロップ、メディアセッションを入れて、`test_blink_file`、`test_blink_codecs`、`test_blink_print`、`test_blink_drag`、`test_blink_session`が通った。

`NET=user`のとき、ホスト(QEMUを動かすWindows)で`tools/browser/ice_tcp_server.py 10.0.2.2`を動かし、`KT_SITE='http://10.0.2.2:8791/ice.html'`で開くと、ページはデータチャネルのオファーを作り、TCPのパッシブの候補(10.0.2.2の8793番)だけを持つICE-liteの相手として自分でアンサーを返す。ブラウザ(controlling側)はTCPのソケット(14.5.2)で相手に接続し、RFC 4571で区切ったSTUNのバインディングで候補ペアを確認する。サーバはそれにMESSAGE-INTEGRITYとFINGERPRINTを付けて応答する。ページは結果をコンソールとタイトルに書く(`ice-tcp connected pair=tcp,active,...`)。サーバの`/go`は、同じディレクトリの`go.txt`に書いたアドレスへリダイレクトする(無ければ`/ice.html`)。`KT_SITE`を`http://10.0.2.2:8791/go`にした1つのイメージで、`go.txt`を書き換えながら、外部のサイト(会議のサイトの入口など)を1回ずつ開ける。

`NET=user`のとき、ホストで`tools/browser/wt_echo_server.py <ホストのアドレス>`を動かし、`KT_SITE='http://<アドレス>:8790/wt.html\nsecure=http://<アドレス>:8790'`で開くと、ページがWebTransportでサーバに接続し、双方向と単方向のストリームの往復、データグラムの往復、閉じることを確認して、結果をコンソールに書く(`wt ready bidi=bidi-hello uni=uni-hello dgram=dgram closed`)。2026年10月8日、WSLで動かしたサーバとQEMUの間でこのとおりに動いた。

2026年10月8日、Service Worker、WebSocketStream、Payment Request、資格情報、File System Access、機器のないもの(14.4.8)、カメラとマイクと音声、MediaRecorder、Web Serial、WebHIDとWebUSBとゲームパッド、`cookieStore`、画面の向き、`navigator.connection`などを入れて、`test_blink_sw`、`test_blink_api2`、`test_blink_fsa`、`test_blink_devices`、`test_blink_capture`、`test_blink_serial`、`test_blink_cookie`が通った。続けてWebRTC(14.5.2)を入れて`test_blink_rtc`が、XR、画面、マシンのフォント、CSS Paint APIなどを入れて`test_blink_api3`が通った。ストレージのバケット、古いFile System API、ドロップしたファイルのハンドルを追加して、`test_blink_api3`と`test_blink_drag`が通った。WebGPU(14.6.5)を入れて、`make GL=1`で`test_blink_webgpu`が通った(softpipe)。ブラウザが複数のウインドウを持てるようにし(14.7.8)、`test_blink_windows`が通った。デスクトップに通知を加え(11.28、14.4.10)、`test_blink_notify`が通った。Web Push(14.4.11)を入れ、Service Workerの登録をブラウザを終えても残すようにして、`test_blink_push`が通った。バックグラウンドの同期と定期の同期、バックグラウンドの取得、Content Index、振動を入れて、`test_blink_sync`が通った(`requestVideoFrameCallback`と、`srcObject`に与えた`MediaStream`も同じテストで確認する)。Cache Storageと許可の設定を残し、WebSocketでBlobを送り、IndexedDBにBlobとFileを入れられるようにして、`test_blink_socket`と`test_blink_idb`が通った。マイクを`getUserMedia`に接続して、`test_blink_mic`が通った。`navigator.plugins`、Storage Access、`setSinkId`、DeviceOrientation、Compute Pressure、`MediaStreamTrackProcessor`、`EyeDropper`のモジュール、オリジンごとのファイルシステム、ワーカーの`MediaSource`、モジュールの型のシリアライズ(`postMessage`やIndexedDBで`CryptoKey`、`VideoFrame`、`MediaSourceHandle`などを送る)を入れて、`test_blink_api4`が通った。メディア要素のコントロールをBlinkの標準のものにし、ピクチャインピクチャのウインドウをフローティングウインドウにして、`test_blink_media`と`test_blink_windows`が通った。カメラ(6.6)を`getUserMedia`と`ImageCapture`に接続して`test_blink_cam`が、WebAssemblyのトラップハンドラを有効にして(5.5.2)`test_blink_wasm`が、印刷で文字を文字のまま残し`webkitdirectory`に対応して`test_blink_print`と`test_blink_dir`が、スペルチェックを入れて`test_blink_spell`が、プログラムのUSBデバイス(6.3.1)をWebUSBに接続して`test_blink_webusb`が通った。続けて、WebHID、WebUSBのリセットとアイソクロナスの転送、カメラのMJPEGを入れて、`test_blink_webhid`、`test_blink_webusb_iso`、`test_blink_cam_mjpeg`が通った。

`make ... KT_V8=test_blink_media,test_blink_tool`で、`v8`のうち指定したテストだけを実行する。`make ... GL=1 KT_BR_GPU=1`では、テストが起動するすべてのブラウザに`gpu=on`の行を追加し、ほかのテストもGPUの経路(QEMUではsoftpipe)で実行する。`make ... NET=user KT_SITE=<URL>`では、そのサイトだけを開き、読み終えて1秒後と3秒後の画面のスクリーンショットを`/boot/SITE.PPM`、`/boot/SITE2.PPM`に保存する。読み終えてからもスクリプトがページを構築するサイトでは、`KT_SITE_WAIT=<ミリ秒>`を加えると、2枚目のスクリーンショットを1枚目からその時間の後に撮り、ブラウザをその7秒後まで動かしておく。`KT_SITE`のアドレスの後には、`\n`で区切ってブラウザの起動引数の行を追加できる。`script=<アドレス>`は、ページが表示されたところでそのアドレスのスクリプトを取得してページの中で実行し(シーク、一時停止、画質の切替えなどの操作をテストから行う)、`secure=<オリジン>`は、そのオリジンのページを安全なコンテキストとして扱う(EMEは安全なコンテキストにだけあるので、テストサーバの`http`のページでEMEを試すときに使う)。`gles=<バージョン>`(`gles=3.0`など)は、Mesaのコンテキストが出すOpenGL ESのバージョンをそのバージョンまでにする(`MESA_GLES_VERSION_OVERRIDE`)。QEMUのsoftpipeを実機のV3Dと同じOpenGL ES 3.0にして、ANGLEとWebGPUのアダプタの扱いを実機と同じ経路で試す。`KT_SITE_CLICK=<x>,<y>[,<x>,<y>…]`を加えると、2枚目のスクリーンショットの後、デスクトップを起動して、画面のその座標へポインタを動かして左のボタンを押して放すことを1点ずつ行い、4秒後の画面を`/boot/SITEC1.PPM`、`SITEC2.PPM`…に保存する(ページのボタンがクリックで動くかを、入力の経路を通して見る)。ブラウザが動いている間はスクリーンショットを1枚書くのに十数秒かかるので(QEMUでGoogleマップを開いたとき)、クリックがあるときは、ブラウザを終了する時刻(`quit=`)を、クリック1つにつき25秒と、さらに30秒延ばす。コンソールの`click N at x,y (T ms after the start)`の行に、押した時刻が出る。`KT_SITE_WAIT`の間は、ブラウザのプロセスが割り当てているメモリの量を10秒ごとにコンソールに出す。

2026年10月6日、GPUでのラスタ化(14.7.2)を入れて、`GL=1`のブラウザで`v8`、`gl`、`gpu`のテストグループを実行し、`NET=user`のときだけの2つを除く38のテストが通った(QEMUにはGPUがないので、`test_blink_gpu`のほかはプロセッサで描く)。`KT_BR_GPU=1`で、`test_blink`、`test_blink_input`、`test_blink_keep`、`test_blink_media`、`test_blink_menu`、`test_blink_tool`、`test_blink_frame`をsoftpipeの上のGPUの経路で実行し、すべて通った。`test_blink_frame`の`check=`では、変わった部分だけをGPUで描いた47の表示が、ビュー全体をGPUで描き直したものと1画素も違わなかった。

2026年10月5日、フレームの処理(14.4.2)と移植していない機能の扱い(14.4.3)を入れて、`NET=user KT_SITE_WAIT=60000`でYouTubeを開いた。Raspberry Pi 5の実機では、それまで、ページは読み込まれるが構築前のスケルトン(灰色の動画とアイコンの枠)のままで、サムネイルの画像を一度も要求していなかった。QEMUの4コアでは、サブリソースがそろうまで約30秒かかり、次のように表示された。

| ページ | 表示 |
| --- | --- |
| `https://www.youtube.com/` | スケルトンが外れ、ロゴ、検索の欄、ガイドのアイコン(SVG)が出る。ログインしていない利用者にはサーバがおすすめを返さないので、動画の一覧の代わりに「まずは検索してみましょう」と出る |
| `https://www.youtube.com/results?search_query=TRON` | 検索結果のサムネイル(`i.ytimg.com`のWebP)、チップ、アイコンが出る。画面の外のサムネイルは、スクロールして見えるまで要求しない |

同じ日、Chromiumのメディアのパイプライン、MSE、EME、MediaCapabilities(14.6.2、14.6.3)を入れて、QEMUの4コアで次のように確認した。テストページとメディアのファイルは、ホストのHTTPのサーバ(QEMUのユーザモードネットワークから`10.0.2.2`で到達できる)に置いた。音声は、ブラウザが終わるときに出す、サウンドデバイスが受け取ったフレームの数で確認した。

| ページ | 確認したこと |
| --- | --- |
| MSEのテストページ(H.264の640×360と426×240、AACの44.1kHzの断片に分けたMP4。映像と音声を別の`SourceBuffer`で渡す) | `isTypeSupported`がH.264、AAC、VP9、Opusに真、AV1と1920×1080に偽を返す。`ChunkDemuxer`が開いて`sourceopen`が来て、`FFmpegVideoDecoder`と`FFmpegAudioDecoder`で再生する。4秒で16.5秒にシークすると、持っている範囲が`[6.08-10.08,16.16-22.08]`(先頭を`remove`し、隙間がある)になり、`seeked`のあと再生を続ける。22秒からの部分を426×240の初期化セグメントと断片で追加すると、22.11秒で大きさが変わる。`endOfStream`のあと30.08秒で`ended`になる。418フレームをデコードして60をドロップし、音声は856,934フレーム(17.9秒分)を渡した |
| Clear Keyのテストページ(上のMP4をCENC(AES-CTR)で暗号化したもの) | `requestMediaKeySystemAccess`がClear Keyに`MediaKeySystemAccess`を返し、Widevineを`NotSupportedError`で拒否する。`MediaKeys`を要素に付け、`keyids`の要求に鍵(JWK)で応答すると鍵が`usable`になり、復号してから渡すデコーダ(`(decrypting)`)で30秒の終わりまで再生する。音声は1,442,238フレーム(30.0秒分)を渡した。MediaCapabilitiesの`decodingInfo`は、H.264の640×360とVP9の854×480に`smooth`、H.264の1920×1080に`smooth`でない、AV1に`supported`でないと返す |
| `<video src>`のテストページ(H.264とAACのMP4、VP9とOpusのWebM) | `FFmpegDemuxer`で読み、MP4は`FFmpegVideoDecoder`、WebMは`VpxVideoDecoder`と`OpusAudioDecoder`で同時に再生する。シーク、一時停止、再開、終わり |
| `https://www.youtube.com/watch?v=dQw4w9WgXcQ`(3分33秒) | YouTubeのプレーヤはVP9の854×480とOpusを選ぶ(`VpxVideoDecoder`、`OpusAudioDecoder`)。`script=`の行で、本編が始まって15秒後に60秒へシーク(プレーヤが画質を640×360に下げる)、6秒の一時停止と再開、画質を`small`にする(426×240の`SourceBuffer`に切り替わる)と、それぞれ再生が続いた。音声は4,582,848フレーム(95.5秒分)を渡した。同じテストをもう一度行っても同じように動いた |
| `https://www.youtube.com/watch?v=jNQXAC9IVRw`(19秒) | 本編(320×240)が終わり、自動で次の動画に移ると、広告が2つ(854×480)流れ、そのあと本編(854×480)が45秒まで再生され、640×360に画質が下がった。広告と本編は、それぞれ新しい`MediaSource`で渡された |

2026年10月8日に`https://www.youtube.com/watch?v=09PB3aqdqHY`を開くと、本編が46秒ほど流れたところで、プレーヤが「エラーが発生しました」を出して止まった。動画のサーバへの要求(SABR)が送る再生の検証のトークン(PO token)が受け入れられず、サーバが保護の状態3(検証が要る)を返して動画のデータを送らなくなるためである。実行時の機能を`base::Feature`に合わせた(14.4.12)後は、検証のスクリプトが最後まで動き、トークンは10バイトの仮のものから77バイトのものになったが、まだ受け入れられない。検証のスクリプトの中身は調べていない。止まる前に、要素がプレーヤを解放した(`Shutdown`)後もプレーヤがパイプラインの通知を要素に渡し、要素が解放したプレーヤに問い合わせてブラウザが落ちることがあった。`Shutdown`で要素への通知を止めるようにした(`port/media/media_player.cc`)。

QEMUのTCGでは、854×480のVP9でも1秒の再生にほぼ1秒で進む(MSEのテストページで14%のフレームをドロップした)。YouTubeを開いた1回の実行で、一時停止から再開した直後にブラウザのプロセスがシステムコールに自分のものでないアドレス(`4239d3e0`)を渡して終了した。同じ操作を繰り返した次の2回の実行(うち1回は、音声出力のスレッドを作る処理と止める処理を排他にした後)では起きず、原因はまだ確認できていない。

2026年10月28日、x64のQEMU(WHPX、4コア)で`https://www.youtube.com/watch?v=aqz-KE-bpKQ`(10分34秒)を再生すると、163秒で止まった(`waiting`。持っている範囲は160.4秒まで)。再生ボタンもシークも効かなかった。プレーヤは159.6秒まで取得すると、次の取得まで90秒ほど要求を出さない。その間に、YouTubeのService Worker(14.4.6)への`fetch`イベントも34秒途切れた。ワーカーはアイドルの遅延を過ぎて停止を要求したが、ホストが応答していなかったため、その後のページの取得(動画の`videoplayback`、`stats/qoe`、`log_event`など)はすべてワーカーのキューで待った(コンソールの`service worker: NNs without an answer`。待ちは21まで増えた)。プレーヤは5〜10秒ごとに要求を中止して出し直したが、どれも応答がなかった。最初のイベントが5分でタイムアウトすると(`the fetch event ended 3`)、キューにあったイベントはまとめてネットワークへ回った。ただし、そのときプレーヤの要求はもう中止されていて、ワーカーは遅延が0になってすぐまた停止を要求したので、再生は戻らなかった。ページを読み直すと直るのは、メインフレームの移動の`fetch`イベントは待たずに始まり、キューにあるイベントも一緒に始めるためである。ホストが停止しないと応答するようにした後は、要求の間が90秒ほど空いても最後(634.6秒)まで再生した。65秒でネットワークを切断し(QEMUの`set_link`)、139.9秒で持っている範囲を使い切って`waiting`になった後で接続し直すと、3.5秒後に再生が続いた。

2026年10月1日、JITを入れて`v8`のテストグループを実行し、`test_https`と`test_http3`(`NET=user`のときだけ)を除く19のテストが通った。QEMUの4コアで計測した機械語とインタプリタの時間は次のとおりである(3回の実行の範囲)。

| 計測したもの | 機械語 | `--jitless` |
| --- | --- | --- |
| `test_jit`のJavaScriptのループ(`v8prog`の中で計測する) | 1033〜1319ミリ秒 | 6231〜7311ミリ秒 |
| `test_jit`のプロセスの起動から終了まで | 1996〜2716ミリ秒 | 7231〜8606ミリ秒 |
| `test_jit`のWebAssemblyのループ(10万回の加算を50回) | 52〜67ミリ秒 | なし |
| `test_blink_jit`のJavaScriptのループ(ブラウザの中) | 377ミリ秒 | 1578ミリ秒 |
| `test_blink_jit`のWebAssemblyのループ | 54ミリ秒 | なし |

`v8prog`の`bench`では、再帰とふるいの関数がMaglevかTurbofanの機械語になり(`opt`)、文字列の関数はSparkplugの機械語になった(`baseline`)。`test_blink_mem`で大きなページを読んだ後のブラウザのプロセスのマッピングは約207MBで、V8のヒープは約10MBであった。同じ日に`NET=user`で`test_https`を実行すると、`https://www.gnu.org/`を除く各サイトを読んだ(サブリソースがそろうまで、`https://www.wikipedia.org/`が3.2秒、`https://ja.wikipedia.org/wiki/BTRON`が3.1秒、`https://news.ycombinator.com/`が0.95秒、`https://html.duckduckgo.com/html/?q=TRON`が0.94秒)。`https://www.gnu.org/`は、QEMUのユーザモードネットワークからの接続が拒否かタイムアウトになり、TLSの接続ができなかった(ホストからは接続できる)。JITとは関係しない。

2026年9月30日、QEMUの4コアで実際のサイトを開いた結果は次のとおりである。

| サイト | サブリソースがそろうまで | 同時に取得した最大数 |
| --- | --- | --- |
| `https://www.tron.org/ja/` | 18.6秒 | 68 |
| `https://www.wikipedia.org/` | 3.7秒 | 3 |
| `https://ja.wikipedia.org/wiki/BTRON` | 3.9秒 | 9 |
| `https://news.ycombinator.com/` | 1.4秒 | - |
| `https://html.duckduckgo.com/html/?q=TRON` | 1.9秒 | 13 |

2026年10月1日、`test_http3`でHTTP/3を確認した結果は次のとおりである。QEMUのユーザモードネットワークは、QUICのUDPをホストのソケットで外部へ通した。QUICのソケットの受信バッファは、NetBSDのスタックが受け付けた128KBになった。

| サイト | HTTP/3で届いた応答 | HTTP/2で届いた応答 |
| --- | --- | --- |
| `https://cloudflare-quic.com/`と、その2つのページ | 1(3つめのページ) | 10 |
| `https://www.google.com/`と、その2つのページ | 13 | 8 |

同じ日、サーバについて知ったことの持ち越し、DNSのHTTPSレコード、ネットワークのスレッドの待ち方を入れて`test_http3`を実行した結果は次のとおりである。開き直したブラウザは、前回書いたサーバの情報(10のサーバ)とQUICのセッションチケット(4つ)を読み戻し、両サイトとも最初の応答がHTTP/3で届いた。HTTPSレコードだけでも(`forget`)、両サイトとも最初の応答がHTTP/3で届いた。初めて開いた`https://cloudflare-quic.com/`の最初の応答がHTTP/2なのは、QUICがこのマシンでまだ動いたことがなく、TCPの接続を遅らせずに競わせて、TCPが先に接続できたためである。サブリソースがそろうまでの時間は、ネットワークのスレッドの待ち方を変える前の同じテストと比べたものである。

| 開き方 | 最初の応答 | HTTP/3で届いた応答 | HTTP/2で届いた応答 | サブリソースがそろうまで(前、後) |
| --- | --- | --- | --- | --- |
| `https://cloudflare-quic.com/`と、その2つのページ(初めて) | HTTP/2 | 3 | 8 | 1.83秒、1.60秒(最初のページ) |
| `https://www.google.com/`と、その2つのページ(初めて) | HTTP/3 | 19 | 2 | 5.48秒、4.29秒(最初のページ) |
| `https://cloudflare-quic.com/?again`(開き直し) | HTTP/3 | 1 | 0 | 1.60秒、0.87秒 |
| `https://www.google.com/?again`(開き直し) | HTTP/3 | 6 | 0 | 5.54秒、4.51秒 |
| `https://cloudflare-quic.com/?dns`(HTTPSレコードだけ) | HTTP/3 | 1 | 0 | 1.83秒、0.95秒 |
| `https://www.google.com/?dns`(HTTPSレコードだけ) | HTTP/3 | 4 | 1 | 5.67秒、5.52秒 |

2026年10月8日、セッション履歴とブラウザの操作(14.7)、ページから見える特性(14.4.12)を入れて、`test_blink_spa`(40)、`test_blink_ui`(31)、`test_blink_face`(22)が通り、`test_blink`、`test_blink_image`、`test_blink_media`、`test_blink_iframe`、`test_blink_dialog`、`test_blink_windows`、`test_blink_frame`も通った。YouTubeの再生では、ページから見える特性をChromeにそろえても、POトークンの検証(`GenerateIT`)の応答の先頭のトークンはnullのままで、保護の状態は約50秒で3(検証が要る)になる。
2026年10月9日、AV1(dav1dのデコード、libaomのエンコード)、PDF(PDFium)、ページの中の検索、パスキーとパスワードの保存、`appendEncodedChunks`(実験的な機能として)を入れ、ブラウザのテストグループ(`make GL=1`のときだけの`test_blink_gpu`を除く66本)をグループに分けて実行し、すべて通った。`test_blink_cookie`は前に実行するテストがCookieを残すと数が変わり、順番によっては失敗する。グループの1つで、テストの切替え時にカーネルのネットワークスタック(NetBSDのrump)のタスクが例外で止まった(ブラウザを起動する前で、ブラウザとは関係しない)。

2026年10月10日、明朝体と等幅の書体、ワーカーの`OffscreenCanvas`のWebGL、長いアニメーションフレーム、iframeのドキュメントのService Worker、Web Platform Testsで見つけた違い(14.4.17)を修正し、新しい`test_blink_swframe`と`test_blink_owl`を追加して、`test_blink`、`test_blink_worker`、`test_blink_iframe`、`test_blink_api`、`test_blink_storage`、`test_blink_cache`、`test_blink_shared`、`test_blink_file`、`test_blink_sw`、`test_blink_swframe`、`test_blink_owl`、`test_blink_face`、`test_blink_idb`、`test_blink_frame`が通った(`make GL=1`)。

2026年11月8日、x64のWidevine(14.6.3.1)を、QEMUのq35とOVMF(`tools/x64/ktest64.sh`、`ACCEL=whpx NET=user`)で`test_blink_wv`により確認した。開発マシンでコンポーネントの更新のサービスから取得した応答とCRX3(Widevine CDM 4.10.3112.0、21,970,001バイト、SHA-256 `1bf1f200…a98f`)をテストサーバから渡すと、パネル7に〈導入〉で応答したときだけ取得し、大きさとSHA-256の照合、CRX3の3つの署名の検証(Widevineのコンポーネントの鍵を含む)を経て、ライブラリ(21,965,296バイト)とマニフェストを実身に入れた。CDMは`ts_so.c`で読み込まれ(81,912KB、TLSあり、532のFDEをアンワインダに登録)、`requestMediaKeySystemAccess('com.widevine.alpha')`は`SW_SECURE_DECODE`で通り、`HW_SECURE_ALL`は拒否された。インターネットに接続する`/wvplay`では、Shaka Player 4.12.0がデモのWidevineのストリームの鍵をデモのライセンスサーバから受け、CDMが復号とVP9のデコードを行う`DecryptingVideoDecoder`で768×576の映像を60秒の終わりまで再生した(1,500フレームでドロップしたものは0)。同じ映像のClear Keyのストリームも、復号の呼出しを`BrCdmOnThread`を通して再生した。起動し直したブラウザは実身からCDMを読み込み、〈Widevineを外す〉で実身と行が消えた。同じイメージで`cxx`のグループと`test_present`、`test_script`、`test_jit`、`test_skia`、`test_blink_media`、`test_blink_face`も通り、スレッドの制御ブロックを64バイトにしたあとも、C++のプログラムとブラウザは今までどおり動く。

同じ日、`test_blink_wv`が最後まで終わらなかった原因を、キーの通知の行き先の記録で確認して修正した。テストはブラウザのプロセスを利用者の操作なしに作るので、ブラウザのウインドウは入力フォーカスを受け取らず(10.4の規則)、キーは最初のウインドウ「BTRON」に残っていた。ブラウザのパネル7は開くときに入力フォーカスを取り、閉じるとそれを「BTRON」に返したので、テストのCtrl+Eは「BTRON」に届き、デスクトップは「OSを終了しますか？」を出して待った。デスクトップのパネルは、ほかのウインドウに入力されたキーも自分への応答として読んでいたので、後でブラウザのパネル2に応答したEnterが、背後のこのパネルの「はい」にもなり、OSの終了が始まってブラウザも閉じた。修正したのは次の3つである。デスクトップのパネル(`dt_pn_run`、`dt_confirm_timed`)は、その下のウインドウに入力されたキーだけで応答する。問合せが出ている間にそのプログラムのウインドウが入力フォーカスを受け取ると、入力フォーカスは問合せに渡り、閉じるとそのウインドウに戻る(`knl_wmobj_focused`)。テストの`run_browser`は、利用者がページを開いたときと同じく、ページのウインドウに入力フォーカスを与えてから操作する。これで、`/wvplay`をCtrl+Eで閉じる元の形のまま、x64の`test_blink_wv`は92件の確認項目がすべて通り、集計が出た。

## 14.11 ライセンス

`tessron-chromium`のファイルはMITライセンスで配布する。`BROWSER=1`でビルドしたイメージには、Chromium(Blink、V8、Skia、`base`、`net`など)とその第三者コンポーネント、BoringSSL、FFmpeg、dav1d、Rustのクレートが含まれる。`GL=1`でビルドしたときは、ANGLE(BSD 3条項)とMesa(大半はMIT)も含まれる。一覧と条件は`THIRD_PARTY_NOTICES.md`にある。

## 14.12 拡張機能

Chrome拡張(Manifest V3)を扱う。Chromiumの`extensions/`はブラウザプロセスとレンダラの分離を前提にしていて、このブラウザにはそのレイヤがないので、使わずに移植レイヤに小規模に実装した(`port/ext/`)。Chrome Web Storeからの導入はしない。

### 14.12.1 置き場所と取り込み

拡張機能は、ZIPまたはCRXをファイル変換で取り込んだ実身である(レコード1がファイルのバイト列。`xf_import`)。新しい形式も、新しい箱も作らない。普通の実身「ブラウザの拡張機能」(`SYSDEF_BROWSER_EXT`)のレコード0に、読み込んだ拡張機能の実身への仮身が並ぶ。仮身は参照数に数えるので、もとの仮身を捨てても実身は残る。レコード1はブラウザが持つ状態のJSONで、拡張機能ごとの有効か無効か、`chrome.storage`の`local`と`sync`、有効なルールセット、動的なルール、無効にした静的ルール、登録したコンテンツスクリプト、アラーム、後から許可された権限である。`storage.session`とセッションルールは持ち越さない。

メニューの〈拡張機能を読み込む〉でファイルの選択のパネル(パネル6。トレーの仮身やパネルに入れた仮身)から選ぶと、中身を読んで`manifest_version`が3であることを確認し、権限のパネル(パネル21)に、名前、バージョン、要求している権限(14.12.3)の一覧を出す。〈読み込む〉で一覧に仮身を追加し、〈取消〉なら何も残さない。次に開くページから有効になり、バックグラウンドのワーカーは次の起動から動く。〈設定〉の「拡張機能」のタブ(14.7.10)に、名前、バージョン、状態が並び、読み込み、有効と無効の切替え、削除ができる。状態の変更は次に開くページと次の起動から効き、別のプロセスのブラウザには次の起動から効く。IDは実身のUUIDのSHA-256から作るChromeと同じ字種の32文字で、実身が同じなら変わらない。CRXの署名は検証しない。利用者が自分で選んだ実身だけを読み込む。

### 14.12.2 動くもの

| 機能 | 実装 |
| --- | --- |
| ファイル | `chrome-extension://<ID>/<パス>`にはローダとネットワークのレイヤが応答する(`br_ext_serve`)。ページからは`web_accessible_resources`にあるものだけで、その`matches`がページのオリジンに合うとき。マニフェストのパスの先頭の`/`は無視する |
| 拡張機能のページ | `action`のポップアップ、`options_page`、`tabs.create`で開く拡張機能のページは、このプロセスの通常のウインドウで開く(420×560)。別のプロセスにすると`storage`とワーカーが別になるからである。メインワールドに`chrome`があり、既定のCSPは`script-src 'self' 'wasm-unsafe-eval'; object-src 'self'`(マニフェストの`content_security_policy.extension_pages`があればそれ) |
| `content_scripts` | 拡張機能ごとの隔離されたワールド(番号は1000から)で実行する。ページのスクリプトからは見えない。`run_at`の3つは`RunScriptsAtDocumentElementAvailable`、`RunScriptsAtDocumentReady`、`RunScriptsAtDocumentIdle`。CSSは`InsertStyleSheet`。`matches`、`exclude_matches`、`all_frames`、`world` |
| `background.service_worker` | 拡張機能のオリジンの本物のService Worker(14.4.6)。ブラウザの起動時に登録する。`type`が`module`ならモジュールのワーカーで、スクリプトの先頭に`import '/__brext_shim.js'`を付けて返す(importは順に評価されるので、`chrome`が他のモジュールより先にできる)。クラシックならスクリプトの先頭に`chrome`オブジェクトを付ける。`onInstalled`は最初の1回、`onStartup`はそれ以後 |
| `chrome.runtime` | `id`、`getURL`、`getManifest`、`sendMessage`、`onMessage`(`sender.tab`、`sender.frameId`付き)、`onInstalled`、`onStartup`、`openOptionsPage`、`getPlatformInfo`。`connect`はない |
| `chrome.storage` | `local`、`sync`(実身のレコード1)、`session`(メモリのみ)。`get`、`getKeys`、`set`、`remove`、`clear`、`onChanged`(同じ拡張機能のすべてのコンテキストに届く)。`managed`はいつも空 |
| `chrome.tabs` | このブラウザにタブはなく、ウインドウが1つのタブ(ID 1)に当たる。`query`、`get`は今のページを返し、`url`と`title`は`tabs`か、そのサイトの権限があるときだけ付く。`create`は拡張機能のページならこのプロセスのウインドウ、ほかは別のプロセスのブラウザを開く。`update`は`url`へ移動、`reload`、`sendMessage`(そのページのすべてのフレームのコンテンツスクリプトに届き、最初の応答が結果になる)、`onUpdated`。`remove`は拒否する |
| `chrome.windows` | `get`、`getCurrent`、`getAll`は1つのウインドウを返し、`create`は`tabs.create`と同じ |
| `action` | ボタンはメニューの〈拡張機能を読み込む〉の下に標準の項目として並ぶ。自前で描くツールバーは作らない。項目は`setTitle`の文字で、`setBadgeText`の文字を括弧で添える。アイコンとバッジの色は見せない(`setIcon`は受け取って何もしない)。選ぶと`activeTab`の許可を与え、`default_popup`があればそのページをウインドウで開き、なければ`onClicked`。`enable`、`disable`、`setPopup`、`openPopup` |
| `scripting` | `executeScript`(`func`と`args`、`files`、`target`の`allFrames`と`frameIds`、`world`の`MAIN`)、`insertCSS`、`removeCSS`(`origin`)、`registerContentScripts`、`updateContentScripts`、`unregisterContentScripts`、`getRegisteredContentScripts`。結果がPromiseならそれを待つ。登録は状態に残す。ページのスレッドで実行するので、ワーカーはタスクをポストして結果を待つ |
| `webRequest` | `onBeforeRequest`、`onBeforeSendHeaders`、`onSendHeaders`、`onHeadersReceived`、`onResponseStarted`、`onBeforeRedirect`、`onCompleted`、`onErrorOccurred`。参照するだけで、リスナの戻り値は要求に影響しない(`blocking`を付けると`addListener`が例外)。MV3の仕様どおりである。ヘッダは`extraInfoSpec`で求めたときだけ渡す。`host_permissions`に合う要求だけが届く |
| `alarms` | `create`(`when`、`delayInMinutes`、`periodInMinutes`)、`get`、`getAll`、`clear`、`clearAll`、`onAlarm`。ブラウザのプロセスが持ち、状態に残す。ワーカーが取りに来るときに時刻を見るので、ワーカーが動いている間に100ミリ秒以内に届く。Chromeの30秒の下限は置かない(利用者が選んだ未パックの拡張機能と同じ扱い) |
| `contextMenus` | `create`、`update`、`remove`、`removeAll`、`onClicked`。ウインドウのメニューに標準の項目として並ぶ。子の項目は「親 > 子」の1段で、`checkbox`は「[x] 」を添える。`contexts`、`documentUrlPatterns`、`targetUrlPatterns`。`%s`は選んだ文字に置き換える |
| `i18n` | `getMessage`(`$1`、名前の付いたプレースホルダ、`@@ui_locale`など)、`getUILanguage`、`getAcceptLanguages`。`_locales/<default_locale>`の上に`ja`、`ja_JP`を重ねる。マニフェストの`name`、`short_name`、`description`、`action.default_title`の`__MSG_名前__`も置き換える。`detectLanguage`はいつも空 |
| `permissions` | `getAll`、`contains`、`request`、`remove`。`request`は`optional_permissions`、`optional_host_permissions`のものだけで、権限のパネルで確認する |
| `declarativeNetRequest` | 静的なルールセット(有効になったときに読む)、`updateDynamicRules`、`updateSessionRules`、`updateEnabledRulesets`、`updateStaticRules`、`isRegexSupported`。`block`、`allow`、`allowAllRequests`、`redirect`(`url`、`extensionPath`、`regexSubstitution`、`transform`)、`upgradeScheme`、`modifyHeaders`。`urlFilter`、`regexFilter`(RE2)、ドメイン、種類、メソッド、`domainType` |

`chrome.*`はJavaScript(`br_ext_shim.inc`)で書き、下は1つの呼出し(`api_call`)である。コンテンツスクリプトと拡張機能のページでは、そのコンテキストだけにある関数を呼ぶ。ワーカーでは自分のオリジンの`/__brext/<操作>`を`fetch`する。ローダはこの経路を、要求の起点がその拡張機能のオリジンであるときだけ通す。メッセージとイベントはブラウザの中のログに入り、受け取る側が100〜150ミリ秒ごとに、前に読んだ番号より後を取りに来る。プロセスごとに拡張機能が読まれるので、メッセージと`storage`のやりとりは同じブラウザのプロセスの中である。別のプロセスのブラウザが同時に書くと、ページの保存域と同じく後から書いたほうが残る。

ルールはネットワークのレイヤで評価する。`URLRequestContext`に`net::NetworkDelegate`を1つ置き(`BrExtDelegate`、`net_fetch_tessronos.cc`)、ページのリソース、ウインドウのドキュメント(`main_frame`)、フレームのドキュメント(`sub_frame`)、ワーカーの要求、プリフライトのすべてがそこを通る。

| 段階 | すること |
| --- | --- |
| `OnBeforeURLRequest` | `block`は`ERR_BLOCKED_BY_CLIENT`。`redirect`と`upgradeScheme`は`new_url`を返す。スタックが内部の307を作るので、要求した側にはサーバのリダイレクトと同じに見える(`redirected`が真、`url`が行き先)。行き先が`chrome-extension://`なら、スタックにそのスキームはないので、その場で拡張機能のファイルを返す |
| `OnBeforeStartTransaction` | `modifyHeaders`の`requestHeaders` |
| `OnHeadersReceived` | `modifyHeaders`の`responseHeaders`。応答のヘッダを差し替えるので、Cookieの保存とCORSの判定にも効く |

要求の種類と起点は、ローダとドキュメントの取得が`X-TessronOS-Resource`の行で渡し、スタックがこれを外して`URLRequest`のユーザデータにする。行のない要求はブラウザ自身のものとして、ルールを当てない。拡張機能のルールは他の拡張機能の要求に当たらない。WebSocketのハンドシェイクはリダイレクトをたどれないので、`websocket_tessronos.cc`がチャネルを開く前にルールを見て、`block`なら失敗にし、`redirect`と`upgradeScheme`なら別のアドレスに接続する。

順序はChromeと同じである。拡張機能ごとに優先度、次に`allow`、`allowAllRequests`、`block`、`upgradeScheme`、`redirect`の順で1つを決め、拡張機能の間では`block`が勝ち、次に後から入れたものの`redirect`か`upgradeScheme`である。ある拡張機能の`allow`は別の拡張機能の`block`を取り消さない。`modifyHeaders`は、その拡張機能で要求を許可するルールより優先度が高いものをすべて、後から入れた拡張機能から優先度の順に適用する。`set`か`remove`した名前はそれより後のルールでは変えない。`allowAllRequests`は、表示中のページに合うルールの優先度までを、そのページの要求について無効にする。

ルールは毎回全部を調べない。`||ホスト`で始まるフィルタと`requestDomains`のあるルールはホストをキーにした表にして、要求のホストとその親のドメインで引く。広告のリストの大部分はこの形である。残りだけを順に調べる。

### 14.12.3 権限

宣言にないものは拒否する。拒否するのは`api_call`のエントリとルールの評価で、JavaScriptで`chrome.*`に名前を出さないのは見かけ上のことである。ワーカーが`/__brext/alarms.getAll`を直接`fetch`しても同じエラーになる。

| 確認するもの | 必要なもの |
| --- | --- |
| `storage`、`alarms`、`scripting`、`webRequest`、`contextMenus`、`tabs`の`url`と`title` | 同じ名前の`permissions`。`tabs`の`url`と`title`は、そのサイトの`host_permissions`でもよい |
| `action` | マニフェストの`action` |
| ルールの全部 | `declarativeNetRequest`か`declarativeNetRequestWithHostAccess` |
| `redirect`、`modifyHeaders` | 要求先と起点の両方が`host_permissions`に合うこと |
| `declarativeNetRequestWithHostAccess`のすべてのルール | 同上 |
| `scripting.executeScript`、`insertCSS` | ページのアドレスが`host_permissions`に合うか、そのページで`action`かメニューの項目を選んで`activeTab`の許可があること。`activeTab`はタブが別のオリジンに移ると消える |
| `webRequest`で見える要求 | 要求先が`host_permissions`に合うこと |
| 拡張機能の`fetch`のCORS | 要求先が`host_permissions`に合うときだけ、サーバのCORSの応答を見ない |
| コンテンツスクリプトで使えるもの | `runtime`のメッセージと`getURL`など、`storage`、`i18n`だけ |

権限のパネルのリストには、`permissions`を日本語にしたもの、`host_permissions`、`content_scripts`の`matches`を出す。このブラウザにない権限(`offscreen`など)は「(このブラウザにはありません)」を添えて出し、読込みは拒否しない。`<all_urls>`と`*://*/*`は「すべてのサイト」と書く。部品はデータボックスの標準のテキスト、リスト、ボタンだけである。

### 14.12.4 まだないもの

- `declarativeNetRequest`の条件の`responseHeaders`、`excludedResponseHeaders`はない。そのルールは受け取らず、数を起動時にコンソールに出す。`getMatchedRules`はいつも空である。
- `allowAllRequests`が見るドキュメントは表示中のページだけで、フレームのドキュメントの下の要求には効かない。
- `runtime.connect`、`offscreen`、`userScripts`、WebTransportの`resourceType`はない。呼ばれない名前は`undefined`である。`commands`は`onCommand`があるだけで、キーでは動かない。
- アイコンは使わない。`action`のボタンはメニューの項目で、ツールバーにもアイコンにも出ない。
- CRXの署名、拡張機能の更新は扱わない。

## 14.13 レンダラのプロセス

Blinkを使うブラウザは、ページを別のプロセス(レンダラ)で動かす。ブラウザのプロセスはウインドウ、ツールパネル、入力、かな漢字変換、メニュー、パネル、セッション履歴を持ち、BlinkとV8は持たない。レンダラは1つのサイトのページを動かし、ページの保存域のストアとGPUのラスタ化を自分で持つ。ネットワークは、ブラウザごとに1つのネットワークのプロセスが持つ(14.13.8)。起動時の行`process=single`を与えると、以前と同じく1つのプロセスでページを動かす。レンダラを起動できなかったときも同じである。

### 14.13.1 起動と通信路

レンダラはブラウザと同じプログラムで、起動引数の1行目が`renderer`、その後にブラウザが受け取った行(`v8=`、`features=`など)が続く。ブラウザはプログラムのプロセスの実身として作り(`ob_cre_obj`、`br_sys_spawn`)、メタデータの`tessronos.process.withParent`で、ブラウザが終わるとレンダラも終わるようにする。同じメタデータの`tessronos.process.role`で役割(レンダラは「レンダラ」、ネットワークのプロセスは「ネットワーク」、GPUのプロセスは「GPU」)を付ける(5.5.8)。プロセスの名前は「ブラウザ(レンダラ)」のようになり、レンダラが異常終了したときの「プログラムの異常終了」のパネルやプロセスの一覧で、ブラウザの本体と見分けられる。

通信路はMojoで、下位のレイヤは段階0のストリームのペア(`socketpair`、5.7.1)である。ブラウザはMojoのブローカで、`PlatformChannel`の片端を`ts_chan_give`でレンダラに渡し、もう片端で招待(invitation)を送る。レンダラは`ts_chan_take`で受け取って招待を受ける。チャネルはMojoのIOスレッド(`MessagePumpEpoll`)が`epoll`で待つ。共有メモリ(ipczのノードのメモリ、ページの画素)は`memfd_create`のメモリの実身で、`SCM_RIGHTS`で運ぶ。

招待には次の5つのインタフェースのうち4つのパイプが付く(`Page`のパイプは`CreatePage`で渡す)。招待の添付の名前は0から3の整数にする。MojoのipczはこれをNodeの最初のポータルの番号とし、それ以外の形の名前は1つだけの添付とみなすからである。定義は`tessron-chromium`の`application/browser/mojom/br_process.mojom`で、`tools/browser/build.sh`がChromiumのチェックアウトのMojoのバインディングジェネレータでC++にする(`$OBJ/_brgen`)。

| インタフェース | 呼ぶ側 | 内容 |
| --- | --- | --- |
| `Renderer` | ブラウザのエンジンのスレッド | ページを作る(`CreatePage`)、エンジンの側のメニューのコマンド、テストが読む値、ストアの書き出し |
| `Page` | ブラウザのエンジンのスレッド | `BrEngine`(br.h)の呼出しのすべて。ナビゲーションの取得と表示(`Navigate`)、描画、入力、セッション履歴 |
| `Control` | ブラウザのウインドウのスレッド | 中止、スクリプトの停止、レンダラの終わり。レンダラのIOスレッドで受けるので、スクリプトがメインスレッドを止めていても届く |
| `Host` | レンダラのどのスレッドからでも | ページからの問合せ、許可、選択、ファイルの選択、スポイト、画面の共有のパネル、読込みの進捗、ブラウザのループへのコマンド、ダウンロードの受け渡し(`KeepDownload`、`Download`、14.7.4) |
| `Data` | ブラウザの専用のスレッド(data calls) | 設定のウインドウと拡張機能のパネルが読み書きする、レンダラの持つもの(パスワード、パスキー、拡張機能、サイトのデータ、ダウンロード)。操作の名前と、JSONの引数と戻り値。ブラウザではエンジンのスレッドを通らないので、ウインドウとエンジンのスレッドが互いを待つことがない。レンダラでも専用のスレッド(data)で受ける。操作の中にはページのスレッドに処理を依頼して待つもの(サイトの一覧など)があり、ページのスレッドで受けると自分を待つことになるからである。依頼した処理は、ブラウザが次に`Pump`を呼んだときにページのスレッドで行われる |

### 14.13.2 呼出しの形

ページのターン(Blinkのタスク、フレームのコールバック、描画)は、レンダラが自分の時点で行う。ブラウザはページを動かすための呼出しをしない。ターンの結果は、ページの状態(タイトル、アドレス、リンクの上の文字、ページが求めた移動、全画面、開いたウインドウ、スクロールの位置など)を`FrameSink`の`State`で、描いたものを`Frame`で、どちらも非同期に送る。`State`は、前に送ったものと変わったときと、移動の求めや開いたウインドウがあったときだけ送る。ブラウザの側のページは`RemotePage`(`br_proc.cc`)という`BrEngine`で、届いた状態を保持しておき、ウインドウのループの問合せにはそのコピーで応答する。`br_main.cc`のループと画面の更新の手順は、1つのプロセスのときと同じである。

入力(`Mouse`、`Wheel`、`SetScrollLeft`、`Compose`、`Commit`)と、応答の要らない要求(`Zoom`、`Edit`、`HistoryCut`、`LoadAs`、`Response`、`FullscreenDone`、`SaveUrl`、`DragAway`、`RunScript`)は非同期に送り、その結果は次の`State`で届く。ウインドウの位置と画面の大きさは、変わったときに`Window`で伝える。応答が要る問合せ(リンクの位置、移動の取得、戻ると進む、レイアウト、状態の問合せなど)も、Mojoの同期の呼出しにはしない(`Page`と`Renderer`に`[Sync]`のメソッドはない)。ブラウザは非同期で送り、エンジンのスレッドのタスクのループで応答を待つ(`br_proc.cc`の`call_wait`、`wait_answer`)。待つ間に〈中止〉を受けると、レンダラのメインスレッドが1秒以上ビジーならスクリプトの停止を伝え、ウインドウが終了までに与える時間がたっても応答がなければレンダラを終了させる。ウインドウを閉じるときは3秒まで待つ。応答が来ないまま応答のコールバックが破棄されたとき(パイプが切れたとき)も、待ちを終える。待つ間に実行されるのは、エンジンのスレッドのMojoのタスク(ほかの呼出しの応答、通知)だけで、メニューのコマンドとウインドウのイベントはループの本体が扱う。待ちの中から別の呼出しの待ちが始まったときは、記録に1行出す(`browser: the call ... is made inside a wait for another`)。レンダラのレシーバは同期のメソッドを持たないので、Mojoはレンダラの同期の呼出しの待ちの中でそれを実行しない。Chromeのレンダラと同じく、ブラウザの呼出しはいつも`IpcRunner`のループで実行される。

移動の取得(`Navigate`)は、レンダラがページを取得してドキュメントにし終えるまで応答しないので、長くかかることがある(遅いサーバ、大きなドキュメント)。ダウンロードになる応答は、応答の時点でリクエストをブラウザに渡して応答するので、本体を待たない(14.7.4)。ブラウザはこれを非同期に送り、応答をエンジンのスレッドのタスクのループで待ち、待つ間も100msごとにウインドウの状態を見る(`br_nav_wait_verdict`)。〈中止〉(×とEsc)を受けると、そのレンダラに読込みの中止(`Control.StopLoad`)を伝え、メインスレッドが1秒以上ビジーならスクリプトの停止も伝える。ブラウザはナビゲーションごとに番号を付け(`Navigation.seq`)、中止はその番号を指定する。`Control`はレンダラのIOスレッドで、`Navigate`はメインスレッドで受けるので、中止が`Navigate`の始まりより先に届くことがある。以前はレンダラが`Navigate`の始めに中止のフラグを消していたので、そのときの中止は失われ、応答しないサーバのページは中止できなかった。いまは、そのナビゲーションを指定した中止は始めに消さずに残す。応答を受け取り終えた後でも、表示や保存の前に中止が来ていれば、表示も保存もせずに中止とする(以前は取得が成功すると中止に関係なく表示し、表示中のページのスクリプトが終わらないと応答しなかった)。中止から、ウインドウが閉じるときにエンジンのスレッドに与える時間(3秒)がたっても応答がなければ、レンダラはBlinkの中の中断できない処理にいるとみなして終了させる。表示中のページのレンダラなら、レンダラが落ちたとき(14.13.5)と同じエラーページを元のアドレスで出し、メッセージ行に「読み込みを中止しました。応答しないページのプロセスを終了しました」と出す。別のサイトのために起動したレンダラなら、表示中のページはそのまま残る。ウインドウを閉じるときは待ちをすぐにやめ、レンダラは閉じる手順で終了させる。待ちをやめた呼出しの応答は捨てる。以前は同期の呼出しで応答を待ち続けたので、Blinkが大きなドキュメントを構築する間は中止も閉じる操作も効かず、閉じると3秒後にプロセスごと強制終了していた。待つ間の右ボタンのメニューは、以前と同じくウインドウのスレッドが出す。

レンダラは`Renderer`と`Page`の呼出しを、Blinkのタスクではなく自分のループ(`br_rproc.cc`の`IpcRunner`)で実行する。ページのターンは自分でBlinkのタスクを回す(`run_tasks`)ので、Blinkのタスクの中で受けると、タスクの中でタスクを回すことになるからである。

Mojoでは、スレッドが同期の呼出しの応答を待つ間に、そのスレッドにバインドしたレシーバへの同期の呼出しが届くと、待ちの中でそれを実行する(待ちの割込み)。`Renderer`と`Page`は同期のメソッドを持っていたので、レンダラのメインスレッドが同期の呼出しを待つ間に届いたブラウザの`Layout`などは、`IpcRunner`のループを通らず、その待ちの中で実行されていた。ページの`document.cookie`は、ネットワークのプロセスへの同期の呼出し(`Network.CookieLine`)である。そのため、`requestAnimationFrame`のコールバックやスクリプトの中、レイアウトの最中にページの再構築(`run_tasks`とライフサイクル)が入れ子で実行された。入れ子の`run_tasks`はRunLoopの1段目なので、V8の入れ子にしないタスク(スタックを走査しない前提のGC)も実行する。実機(Surface Go 2)のYouTubeでは、これでコールバックの後に`PageAnimator::ServiceScriptedAnimations`のスタックだけにあるコントローラの一覧が回収されて0になり、`GetExecutionContext`でNULLを読んで落ちた。また、レイアウトの最中のライフサイクルの巻戻しで`document_lifecycle.cc`の検査に当たって止まった。最初の修正として、`Network`の同期の呼出し(cookie、HSTS、消去、サイト、プロトコル、書き出し)を`[NoInterrupt]`にした(今も残している)。待つスレッドは自分への応答だけを待ち、その間に来た`Renderer`と`Page`の呼出しは、待ちが終わってから実行する。ネットワークのプロセスは、これらの呼出しにレンダラもブラウザも呼ばずに応答するので、この待ちでデッドロックすることはない。実機では、`ServiceScriptedAnimations`で落ちる件はこれで出なくなった。`document_lifecycle.cc`の検査に当たる件は残った。下の記録の行で、残りの入れ子はBlinkの中の同期の呼出し(`PluginRegistry.GetPlugins`、`NotificationService.GetPermissionStatus`、`BlobRegistry.Register`、`GpuDataManager.Are3DAPIsBlockedForUrl`)の待ちに`Page.Layout`が入ったものと分かった。これは上のとおり`Page`と`Renderer`を非同期にしてなくした。

ほかに2つの形を試してやめた。1つは、レンダラのプロセス全体で待ちの割込みを止める形(`mojo::SyncCallRestrictions::DisableSyncCallInterrupts`)である。Chromeでこの関数を呼ぶのはブラウザのプロセスで(`content_main_runner_impl.cc`の`RunBrowser`)、レンダラは呼ばない。Chromeのレンダラでは、ブラウザからメインスレッドへの同期の呼出しがないので入れ子が起きない。この形では`test_blink_api4`が通らなくなった(OPFS、同期アクセスハンドル、PressureObserver、スポイトの結果が来ない)。プロセスの中のサービスへの同期の呼出しのうち、待ちの割込みで応答が来るものが応答を得られなくなるためとみられる。もう1つは、エンジンの処理の間は`IpcRunner`の`RunsTasksInCurrentSequence`を偽にして、待ちの中に届いた`Renderer`と`Page`の呼出しをMojoにループへポストさせる形である。QEMUのテストは通ったが、実機のYouTubeでブラウザがフリーズした。ブラウザのエンジンのスレッドは`Renderer.Query`の応答を160秒以上待ったが、その間もレンダラはスクリプトとタスクを実行し続けていた(スクリプトの停止の問合せに応答し、長いタスクの記録も出た)。互いの同期の待ちではなく、`IpcRunner`のループに戻らなかったか、ポストした呼出しが実行されなかったとみられるが、どちらかはまだ確認していない。本来の形は、ブラウザからレンダラへの呼出しを同期にしないことで、上のとおりにした。

入れ子で届いた呼出しは、記録に1行出す(プロセスで最初の8回まで、`browser: renderer: Page.Layout taken inside the engine's work ... frames ...`)。framesはメインスレッドのスタックフレームの戻りアドレスで、シンボル付きのプログラムで`addr2line`にかけると、どの同期の呼出しの待ちで届いたかが分かる。`Page`と`Renderer`を非同期にした後は出ない。

レンダラのメインスレッドから出る同期の呼出しと、それに応答する側は次のとおりである。

| 呼出し | 呼ぶスレッド | 応答する側 | 待っているスレッドを逆に呼び出すか(`[NoInterrupt]`にできるか) |
|---|---|---|---|
| `Network.CookieLine`、`Cookies`、`CookiePut`、`HstsKnown`、`Clear`、`Sites`、`Protocols`、`Flush` | メイン(`document.cookie`、cookieStoreなど) | ネットワークのプロセスの`network calls` | 呼び出さない。拡張機能のルール(`NetHooks`)はネットワークスタックのスレッドからレンダラの`net hooks`のスレッドへ行き、メインスレッドは通らない |
| `Store`のすべて(`Load`、`LoadEntries`、`Version`、`Save`、`Apply`、パスワード、パスキー、`Ext*`) | `store asks`(メインは条件変数で待つ) | ネットワークのプロセスの`store calls` | 呼び出さない。メインの待ちはMojoの待ちではないので、もともと割り込まれない |
| `Host.AskPage`、`AskPermission`、`Choose`、`ChooseObjects`、`ShareChoose`、`AutofillChoose` | メイン(シーケンスのあるスレッドはそのまま呼ぶ) | ブラウザのパネルのスレッドのパネル(`dlg_work`、自分のチャネルで動く) | 呼び出さない。パネルが出ている間、エンジンのスレッドがそのレンダラに応答を求める呼出しはパネルが閉じるまで応答を待つが、待つのはタスクのループなので、ほかのウインドウとレンダラは止まらない |
| `Host.Progress`、`MainCommand`、`ExtAskLoad` | メイン | パネルのスレッド(メッセージ行、コマンドのキュー) | 呼び出さない |
| `Host.PickColor` | スポイトの専用のスレッド(`std::thread`、`g_caller`経由) | パネルのスレッドが、ブラウザのループの押下を待つ | メインスレッドは待たないので、ブラウザのループが`Page`を呼んでも応答できる |
| `Host.KeepDownload`、`Download` | サービスのスレッド | パネルのスレッド | 呼び出さない |
| `Host.GpuChannel` | メイン(起動時) | ブラウザの`g_gpuq`がGPUのプロセスを待つ(最長10秒) | 呼び出さない |
| GPUのプロセスのチャネル(`GpuChannel`、コマンドバッファの待ち、共有イメージ) | メイン | GPUのプロセス | GPUのプロセスはレンダラを同期で呼ばない |
| Blinkの中の同期の呼出し(`PluginRegistry.GetPlugins`、`NotificationService.GetPermissionStatus`、`BlobRegistry.Register`、`GpuDataManager.Are3DAPIsBlockedForUrl`、ファイルのBlobの大きさなど) | メイン | このプロセスのサービスのスレッド(blob、services) | 読んだ範囲では、メインスレッドにポストして応答を待つものはない。待ちの割込みは残るが、`Page`と`Renderer`は実行されない |

ネットワークのプロセスとGPUのプロセスでは、同じ入れ子は起きない。ネットワークのプロセスの同期の呼出しは`NetHooks`だけで、ネットワークスタックのスレッドから出る。そのスレッドには同期のメソッドを持つレシーバがない。GPUのプロセスは同期の呼出しをしない。テストは`test_blink_mpnest`である。待ちの経路でデッドロックしないことは、`test_blink_dialog`(alert、confirm、prompt、select)、`test_blink_clip`(権限のパネル)、`test_blink_api4`(スポイト)、`test_blink_mp`(読込み中の`Progress`)で見る。

ブラウザのエンジンのスレッドは、すべてのウインドウとレンダラの呼出しを1本で担当するので、1つのレンダラの応答を待つ間は、ほかのウインドウとレンダラの処理も止まる。そのため、ブラウザの側の呼出しを次の3つに分けている。

| 分類 | 呼出し | 扱い |
|---|---|---|
| (a) 利用者の操作の結果としてすぐ応答が要るもの | `Click`、`Key`、`Context`、`Find`、`LinkAt`、`FirstLink`、`Drop`、`Print`、`HistoryGo`、`Caret`、`Load`、`OpenPage`、`Navigate`、`SetCanvas`、`SetGsurf`、`CreatePage`、`Command`(メニュー)、`KeepFile`、`PlayerCanvas`、`PlayerState`(一時停止かどうか、押下のとき)、`PipPress`、`PipClosed`、`Query`(テストの終わりとメニューの問合せ)、`Memory` | 応答を待つ(`call_wait`)。ビジーなら〈中止〉でスクリプトの停止、それでも応答がなければレンダラの終了、閉じるときは3秒まで |
| (b) 定期的なもの、監視、記録のためのもの | 描画の統計の`Query`(`gpustat`、`mediaframes`)、動画のウインドウの`PlayerState`(表示したフレームの数、ループが1周するごとに見る)、`PaintPlayer`(動画のウインドウのフレーム) | 待たない。非同期で依頼し、応答が来たときに受け取る。統計は応答が来たときに前の応答からの差分を記録に出し、来なければその回は出さない。同じ依頼に応答がなければ依頼し直さない(`br_proc_query_async`)。動画のウインドウは、メモリにあるフレームを描いて次のフレームを依頼し、来たら描き直す(`br_proc_player_painted`) |
| 応答を待たずに送るもの | `Layout` | Chromeが大きさを送るだけなのと同じく(`WidgetBase::UpdateVisualProperties`)、幅が変わったときに送り、前の高さを返す。応答とページの状態(`doc_height`)で高さを直し、`pump()`が変更ありを返すのでウインドウがレイアウトし直す。応答の来ない`Layout`は1つだけで、その間に変わった幅は応答の後に送る |
| リスナがあるときだけ時間制限つきで待つもの | `MayLeave`、`Closed` | `MayLeave`は、ページの状態(`beforeunload`)がリスナなしを示せば問い合わせずに移る。リスナがあれば500ミリ秒まで待ち(パネルが利用者に問い合わせる間は数えない)、応答がなければ移る。Chromeの`RenderFrameHostImpl::ShouldDispatchBeforeUnload`と`kUnloadTimeout`に当たる。`Closed`(アンロード)も500ミリ秒まで |
| (c) 別のレンダラの仕事を止めてはいけないもの | 離れたページのレンダラの`FlushStores`(`end_proc`)、表示をやめるページの`HistoryState`(`run_keep`) | `FlushStores`は、メインスレッドが`BR_HELD_MS`より長くビジーなら依頼せず、そうでなければ最長5秒。`HistoryState`は待たずにコピーを使う(コピーがまだないページだけ待つ)。コピーは、ページのアドレスかエントリの中の位置が変わったときに、レンダラがページの状態とともに送り(`hist_given`。Chromeでレンダラがエントリの状態を送る`UpdateState`に当たる)、取り出すたびに非同期で依頼し直す。閉じるときの待ちは3秒まで |

リスナの有無は、レンダラがページの状態を送るたびに、ページとそのiframeのドキュメントに`beforeunload`のリスナがあるかを調べて入れる(`BlinkEngine::beforeunload`)。

エンジンのスレッドは、ブラウザ自身のメニューとパネルでもブロックしない。ページの上の右ボタンは、ページの`contextmenu`を`Context`で問い合わせたあと、メニューをウインドウのスレッドに渡して出させ(`ctx_menu_due`、`window_menu`)、選ばれた項目をコマンド(`g_commands`)として受ける。〈探す〉の入力、Widevineの導入と取り外しの確認、拡張機能の選択と確認と権限の問合せのパネルも、ウインドウのスレッドで出し(`panel_on_window`、`panels_due`)、結果をコマンド(`findword:`、`wvgo`、`wvremovego`、`extloadid:`、`extadd:`、`extnot`、`extpermis:`)で受けて続きを行う。以前は、メニューとパネルに応答があるまでエンジンのスレッドで処理していたので、その間はほかのウインドウとレンダラの状態も描画も止まり、ウォッチドッグが「has not come round … its loop」と出した(実機の記録で最長10.2秒)。ページのスクリプトが出すパネル(`alert`など、`Host`)は、今までどおりパネルのスレッドで出す。ページを表示し始めるときの`SetCanvas`は(a)のままで、表示し始めたページのスクリプトがすぐにビジーになると、その間は応答を待つ。Chromeのように待たずに渡すには、フレームにメモリの世代(どの`SetCanvas`のものか)を付ける必要があり、まだしていない。

何もすることがないとき、ブラウザのエンジンのスレッドとレンダラのメインスレッドは、そのスレッドのタスクのループの中で待つ(`br_loop_wait`)。1つのプロセスのブラウザとレンダラではBlinkのスケジューラ、マルチプロセスのブラウザではMojoのタスクの`SingleThreadTaskExecutor`である。待つのは次に決まった時刻まで(ページが求めたフレーム、再生中のプレーヤを見る10msごとの時刻)で、最長1秒である。その間にそのスレッドのタスク(ページのタイマー、ほかのスレッドが送ったもの、そのスレッドにバインドしたパイプのメッセージ)が1つでも実行されると、残りのタスクを済ませて待ちを終える。ほかのスレッドは、何もしないタスクを送って待ちを終わらせる(`br_loop_wake`)。ブラウザでは、ウインドウのスレッドがイベントを受けたとき、メニューのコマンドやパネルの要求をエンジンのスレッドに渡したとき、IOスレッドがレンダラのフレームと状態を受けたときにウェイクアップさせる。レンダラでは、`IpcRunner`にMojoのタスクが来たときにウェイクアップさせる。レンダラの待ちがタスクで終わったときは、すべてのページがターンを行う。動画の一時停止中のシークのフレームのように、メディアのスレッドからフレームが届いたときもウェイクアップさせる。ウインドウのスレッドは、ポートのイベントを`ob_rea_tmo`で200msまで待つ。以前は、エンジンのループが10msごとに回り(何もしていなくても1秒に約77回)、ウインドウのスレッドは5msごとにポートを調べていた。

`Host`には、ブラウザでは専用のスレッドで応答する。エンジンのスレッドがレンダラを呼んでいる間にページが`alert`を出しても、パネルが出る。スポイトのように、ブラウザのループがページの押下で応答するものも待てる。レンダラの側で、シーケンスを持たないスレッド(移植レイヤのpthread)からの呼出しは、そのための専用のスレッドで行い、終わるまで待つ。Mojoの同期の呼出しは、呼ぶスレッドのシーケンスで待つからである。

ページの画素は、ブラウザが作ったメモリの実身(ビューの大きさ)にレンダラが描き、ブラウザがそこから変わった矩形だけをウインドウのサーフェスにコピーする。GPUでラスタ化するレンダラは、同じメモリに読み戻す。

フレームは、ChromiumのCompositorFrameと同じ考え方で、レンダラが自分の時点で作る。ページのターンがフレームを作ったら、レンダラはそのターンの終わりに、変わった部分をビューの位置で描き、矩形の一覧を`FrameSink`の`Frame`で1回だけ送る。ブラウザはこれをIOスレッドで受けて保持し、次の表示でその矩形だけをメモリからウインドウのサーフェスにコピーし、コピーし終えたら`FrameDone`を送る(どちらも非同期)。次のフレームの時刻は、フレームが`FrameDone`で表示し終えたときに決まる(14.4.2。`frame_shown`)。ターンがフレームを始めても、ページの描画に変わった部分がなくて送らなかったとき、または前のフレームがまだ表示中のときは、そのターンの終わりでそのフレームを終えたものとし、次の時刻を決める(ChromiumのBeginFrameで、何も変わらなかったフレームがそこで終わるのと同じ)。以前はこれがなく、描き直しのないcanvasやWebGLだけを毎フレーム描くページでは、時刻が進まないままターンのたびにフレームが始まり、`requestAnimationFrame`が1秒に600〜740回(実機のthree.jsの例)、QEMUで160〜180回呼ばれた。1つのプロセスのときは、フレームを始めるたびに`frame_shown`を呼んでいる。レンダラは`FrameDone`を受けるまで、そのメモリに描かない。描かれている途中の画素をブラウザがコピーすることはない。ウインドウの全体を描き直すとき、ブラウザがメモリを持っていなければ(フレームを受けていないか、送り返した後)、`Redraw`で全体のフレームを1回求め、それが来るまではウインドウのサーフェスの今の画素を残す。以前は、ブラウザが表示のたびに、変わった部分を問い合わせ(`Damage`)、矩形ごとに`Paint`を同期で呼び、表示したことを伝えていた(`FrameShown`)。QEMUのav1の再生では、レンダラが1回5〜11msで描くのに対し、ブラウザは1回の`Paint`で145〜280ms待っていた。表示は1秒に0.7回、1回366msで、1プロセスのとき(1秒に3.6回、1回12.4ms)より大きく遅かった。

ビューの位置は、前のフレームの後にブラウザが`View`で上端と高さを伝えていれば(バーを動かしたとき、ウインドウの大きさが変わったとき)その位置、伝えていなければページが自分で動いた位置(ホイール、キー、スクリプトのスクロール、滑らかなスクロールの途中)である。ブラウザは次の`State`のスクロールの位置で表示の位置を合わせ、同じ位置のフレームをコピーする。ページが自分で動いた位置をブラウザの古い位置で描くと、描くたびにページが元の位置へ戻され、ホイールでスクロールできない。

スクロールの位置を決めるのはページの側だけである。ブラウザは`View`と`SetScrollLeft`を、自分でビューを動かすとき(バー、ブラウザのキー、ウインドウの大きさ)だけ送り、そのたびに番号(`asked`)を1つ進める。レンダラは、フレームと状態に、受け取った最後の番号と、パイプによらず送るたびに進める通し番号(`n`)を付ける。ブラウザは、最後の求めの番号を持ち、前に位置を取ったものより新しいフレームか状態からだけ位置を取り(`follow`)、そのビューを今のビューとする。そのため、ページに追従しただけでは`View`を送らない。求めを送ってからレンダラがそれを受け取るまでは、求めた位置を表示する。別の位置のフレームを、求めを送らずに捨てたときは、`Redraw`でページの今の位置の全体を求める。R1.400とR1.410では、ブラウザが`State`で受けた位置を、伝えたビューと違うというだけで`View`として送り返していた。`State`はフレームより遅れて届くので、送り返す位置は1つ前のものになる。ページは滑らかなスクロールの途中で前の位置へ戻され、2つの位置の`View`と`State`が行き来し続けると、スクロールバーのつまみが2つの位置を往復した。前に伝えた高さでブラウザが位置を切り詰めて送り返すこともあったので、エンジンが示す位置にビューの高さを足したものを、ページの高さの下限にする。Blinkのエンジンは、今の位置と同じ位置への`scroll_to`ではスクロールしない(滑らかなスクロールを止めたり、1画素未満を切り捨てて戻したりしない)。テストは`test_blink_barstep`。

ウインドウを閉じたページ(`Closed`)は、ドキュメントがなくなるので、それ以降ターンを行わず、状態も送らない。

### 14.13.3 サイトとレンダラ

ナビゲーションの行き先が、表示中のページと違うサイト(スキームと登録可能なドメイン、`net::SchemefulSite`)のときだけ、別のレンダラのページで取得して表示する。同じサイトの中の移動、`about:`、`data:`のページ、再読み込みは同じレンダラで行う。iframeは別のプロセスにしない。ランのサイト(14.13.4)は、リダイレクトの後に表示したページのアドレスで決める。別のサイトへリダイレクトされたページを表示したレンダラは、そのページのサイトのレンダラになる。

別のレンダラでページを表示したら、ブラウザは前のページを閉じる(`run_leave`、`Page.Closed`)。前のドキュメントはアンロードされ(`unload`、`pagehide`)、そのドキュメントの動画と音声の再生、`MediaRecorder`の録画、`getUserMedia`のカメラとマイク、WebRTCはドキュメントとともに終わる。そのプレーヤのピクチャインピクチャのウインドウも閉じる。前のレンダラにほかのページ(そのページが開いたウインドウ)がなければ、レンダラを終了させてメモリを解放する。開いたウインドウがあれば、レンダラはそのウインドウのために残り、最後のウインドウを閉じたときに終了する(`sub_close`)。同じサイトの中の移動は、同じレンダラの中でBlinkが新しいドキュメントをコミットし、前のドキュメントをアンロードする。1つのプロセスのときと同じである。以前は、前のページを閉じずに戻るために残していた。上限(下)に届かないかぎり前のページが動き続け、ほかのサイトへ移っても、前のページの動画と音声、カメラの録画が止まらなかった。前のページを凍結して戻ると進むに使う形(ChromiumのバックフォワードキャッシュでのBlinkの凍結)は、今は持たない。

プログラムを読み込みBlinkを初期化するのに数秒かかるので、ブラウザはレンダラを1つ用意しておく(予備)。用意したレンダラは起動してすぐにBlinkを初期化し、ページを待つ。予備はページに使った後、別のスレッドで次のものを起動する。

レンダラの数(予備を含む)の上限は、マシンのメモリが4GBで3、8GBで6とし、その間とそれより大きい分は比例で決め、2を下回らない。QEMUのテスト機(2GB)では2である。上限は、テスト用の起動時の行`renderers=`で変えられる。表示していないランのページは閉じるので、上限を超えるのは、ページが開いたウインドウのために残ったレンダラがあるときだけである。そのときも、ページが開いたウインドウ(`window.open`)を表示しているレンダラは閉じない。閉じるレンダラには、ストアを書かせてから終わらせ(`FlushStores`、`Control.Closing`)、`waitpid`で回収する。5秒で終わらなければ`kill`する。

### 14.13.4 セッション履歴

セッション履歴はブラウザが持つ。1つのレンダラが続けて表示したもの(1つのサイトのページと、そのエンジンが作ったエントリ)を「ラン」と呼び、ブラウザはランを順に並べる。別のサイトへの移動は、表示中のランの後ろに新しいランを作り、その後ろのランを捨てる(進むのエントリが消えるのと同じ)。ランのエントリは、そのランを表示しなくなるときに、レンダラからエントリごとのページの状態(`blink::PageState`。アドレス、状態のオブジェクト、スクロールの位置、フォームの状態、Navigation APIのキー)とアドレスを受け取って保持する。

戻ると進むは、ランの中ではそのレンダラのエンジンが行う(同じドキュメントのエントリと、同じサイトの別のドキュメント)。ランの最初のエントリより前、最後のエントリより後へは、隣のランへ移る。表示していないランのページは閉じているので、新しいレンダラにランのエントリを渡して再構築し(`history_restore`)、離れたときのエントリを戻る移動として取得する。その後、それまで表示していたランのページを閉じる。状態のオブジェクトとスクロールの位置はそのエントリのものになる。ページのスクリプトの`history.go()`は、ランの中でだけ動く。

### 14.13.5 レンダラが落ちたとき

レンダラが終了すると(異常終了しても、`kill`されても)、そのパイプが切れ、呼出しが失敗する。ブラウザはそのページを捨て、新しいレンダラに、Chromeと同じ「エラーが発生しました」のページを、元のページのアドレスで表示する。〈再読み込み〉で元のページを読み直す。ブラウザは動き続ける。

エンジンの検査(`CHECK`、`base::span`の範囲の検査、`NOTREACHED`、`LOG(FATAL)`)が失敗したときは、ログの`FATAL`の文を、`base::ImmediateCrash`でプロセスが終わる前にコンソールに出す(`port/base/base_tessronos.cc`の`br_crash_reports_own`。どの種類のプロセスでも`base_up`で付ける)。続けて`browser: FATAL: pid <番号>, thread '<スレッドの名前>', <数> callers:`の行と、呼出し元の戻りアドレスを8つずつ並べた`browser:   at 0x...`の行を出す。`std::terminate`でも同じ行を`browser: std::terminate:`で出す。戻りアドレスはフレームポインタ(エンジンは`-fno-omit-frame-pointer`でコンパイルする)をたどって集める。このプロセスにはシグナルから自分のスタックをたどる仕組みがなく、`CollectStackTrace`はアドレスを返さない(glibcの`backtrace`がない)ので、Chromiumの`StackTrace`は何も出さない。プログラムは決まったアドレスにリンクされるので、アドレスは、シンボルを残したプログラム(`build.sh`がLinuxの側に残す`<名前>.sym`。ARMでは`~/tessronos-obj/tessronos/browser.elf.sym`)で`addr2line -f -C -i -e`にかければ関数と行になる。出した後はコンソールのリングバッファをその場で書き出す(`br_console_flush_now`)。リングバッファのスレッドを待つと、書き出す前にプロセスが終わり、理由の文が失われることがある。2026年11月に実機でHuluの動画を再生してレンダラが終わったとき、1回目は理由の文が残らず(`renderer 7 has ended (status 0x8600)`だけが出た)、2回目に`[...:FATAL:span.h:1323] Check failed: size_type{offset} <= size() && size_type{count} <= size() - size_type{offset}.`が出た。終了ステータス0x8600は、`abort`と同じくSIGABRTで終わったことを示す。glibcの代替の関数(`ts_glibc.c`)の`abort`と`__stack_chk_fail`、`__*_chk`は、読み込んだオブジェクト(WidevineのCDM)が呼んだことを`browser: glibc:`の行で出す。それ以外の`abort`の呼出しは、まだ呼出し元を出さない。

移動の途中で落ちたとき(`Navigate`の応答を待つ間にパイプが切れたとき)は、待ちが終わって移動は失敗になり、ウインドウのアドレスは移ろうとしたアドレスになる。ループに戻ったところで、表示中のページのレンダラがないことを見て、新しいレンダラにエラーのページをそのアドレスで出す(`renderer_gone`)。〈再読み込み〉は移ろうとしたページを新しいレンダラで読む。`RemotePage`と`Proc`の記録はレンダラが終わった後も残す(ページの記録がそのレンダラを指し続けるため)ので、後始末の呼出しが解放済みのものを呼ぶことはない。

### 14.13.6 エンジン側の機能のプロセス間の受け渡し

1つのプロセスのときにブラウザのプロセスの中で呼んでいたエンジン側の機能は、次のようにプロセス間で受け渡す。

| 機能 | 受け渡し方 |
| --- | --- |
| 動画のピクチャインピクチャのウインドウ | ウインドウはブラウザが開く。開く、替える、閉じるの要求はページの状態(`PageState.pip`)で届き、プレーヤのフレームは、ブラウザが作ったメモリの実身にレンダラが描く(`PlayerCanvas`、`PaintPlayer`)。閉じたことと押下(再生、一時停止)はレンダラに伝える |
| ページへのリンクのドロップ | 実身の中身をメモリの実身でレンダラに渡し、レンダラがページの読むファイルとして持つ(`KeepFile`) |
| 〈Widevineを導入...〉と〈Widevineを外す〉 | パネルはブラウザが出す。導入中か、導入済みかはページの状態で届く。取得と展開、完了、削除はレンダラのコマンドで、導入の終わりは`wvdone`としてブラウザのループに戻る |
| 〈設定〉のウインドウ | ほかのパネルと同じくブラウザのプロセスで開く。閲覧履歴はブラウザのものをそのまま使う。パスワードとパスキーは、ブラウザのプロセスが自分で読み書きする(14.4.7.1)。拡張機能、サイトのデータ、ダウンロード、HSTSは、表示中のページのレンダラに操作の名前とJSONで問い合わせる(`Data`のパイプ、`br_uidata.cc`)。データの消去、拡張機能の有効と無効と削除、ダウンロードの中止は、どのレンダラも自分の分を持つので、すべてのレンダラに送る。問い合わせる呼出しは、ブラウザのエンジンのスレッドを通らず、専用のスレッド(data calls)から各レンダラの`Data`のパイプへ同期で出す。設定のウインドウはエンジンのスレッドを待たず、エンジンのスレッドも設定のウインドウを待たない。ブラウザを閉じるときは、設定のウインドウに閉じるよう伝えるだけで、閉じ終わりは待たない。閉じている途中にもう一度開くよう求められたら、閉じ終わった後に同じスレッドが開き直す |
| 拡張機能の状態の記録 | 状態の記録(有効と無効、ストレージ、アラームなど)は、ページを動かすプロセス(1つのプロセスのブラウザ、各レンダラ)がそれぞれ持ち、変わったときに丸ごと書く。有効と無効は〈設定〉で切り替えた利用者の指示なので、自分のプロセスで切り替えたもの以外は、書く時点の記録の値を使う。起動時に読んだ値で、ほかのプロセスの切替えを書き戻さない |
| 拡張機能のメッセージの受け方 | ワーカーの`msg.poll`(来たものを問い合わせる)と`msg.answer`(返事を待つ)は、何も来ていなければブラウザの側で問合せを保留し(`br_ext_serve_later`)、来たとき(ログへの追加、`msg.reply`)に応答する。`msg.answer`の待ちを解除する返事は、ほかのコンテキストの`msg.reply`のほかに、`chrome.scripting`のスクリプトがページで出した結果(`br_ext_inject_done`)と、`permissions.request`のパネルの結果もある。どれも同じ関数(`reply_given`)で、待っている問合せがあればその場で応答し、なければ次の`msg.answer`のために取っておく。ページのスレッドが読込みで混んでいると、結果より先にワーカーの`msg.answer`が待ちに入るので、結果が待ちを解除しないと20秒のタイムアウトまで返事が届かない。アラームは待ち合わせのスレッドがその時刻にウェイクアップして出す。20秒たっても何も来なければ、そのまま応答する。ワーカーは応答を受けたらすぐに次を問い合わせる。以前は100msごとに問い合わせていた。コンテンツスクリプトと拡張機能のページの問合せは、フレームのネイティブ関数がPromiseを返し、来たときにブラウザの側がそのフレームのスレッドにタスクを送って解決する。何か来ていればその場で応答する。以前は150msごとに問い合わせていた |
| テストの`gpucheck=`、`check=` | 比べる描画はブラウザが`RemotePage`を通して求める。動画のフレームの数、WebGLのフレームの数、GPUのラスタ化の数は、レンダラに問い合わせる(`Query`) |

ブラウザのプロセスは閲覧履歴とダウンロードの一覧のストアを自分で持ち(`br_data_init`)、エンジンの部分(ネットワーク、ページの保存域、サービスのスレッド)には触れない(`br_data.cc`の`engine_here`)。ほかのプロセスが残したデータの消去の通知(14.8)も、そのとおりに扱う。

### 14.13.7 テスト

`tests/ktest/ktest_v8.c`の`test_blink_mp`が確認する。`127.0.0.1`の`/mp1`を開き、別のサイトの`localhost`の`/mp2`へ移る。2GBのテスト機ではレンダラは2つまでなので、`/mp1`のレンダラが閉じ、そのとき空きのメモリがレンダラ自身の分(テスト機では26MB。プログラムのページはプロセスの間で共有するので戻らない)だけ増えること(10MB以上)、〈戻る〉で`/mp1`のエントリが新しいレンダラで再構築され、状態のオブジェクト(`{n: 7}`)が戻ること、レンダラを`kill`する(起動時の行`menu=killrenderer`)と、ブラウザが動いたままエラーのページを元のアドレスで出すことを、ブラウザが起動時の行`procs=`のファイルに書いたものとタイトルで見る。

`test_blink_crashlog`は、エンジンの検査の失敗でレンダラが終わったときのことを確認する。`/mp1`を開き、起動時の行`menu=checkrenderer`で、ページのレンダラに範囲の外まで`base::span`を切り出させる(レンダラのコマンド`checkfail`、`br_crash_check_test`)。レンダラが終わり(`procs=`に`gone`)、ブラウザが動いたままエラーのページを元のアドレスで出すことを見る。コンソールの`FATAL`の文と`browser: FATAL: pid ...`、`browser:   at 0x...`の行はテストの記録で読み、アドレスを`browser.elf.sym`で関数に変換して、検査を起こした関数から呼出し元へたどれることを確認した。

`test_blink_mpnav`は、ページを離れたときに、そのページの再生と録画が終わることを確認する。`127.0.0.1`の`/mpv1`は動画と音声を繰り返して再生し、再生の0.5秒ごとにテストサーバへ通知する(`timeupdate`)。3回目で同じサイトの`/mpv2`へ移り、`/mpv2`も同じように再生して、別のサイトの`localhost`の`/mpv3`へ移る。`/mpv2`を要求された後に`/mpv1`の通知が1回より多く来ないこと、`/mpv3`を要求された後に`/mpv2`の通知が1回より多く来ないことを見る。続いて`/mpc`はカメラの映像を`MediaRecorder`で0.5秒ごとに録画し、3回目で`/mpv3`へ移る。その後に録画の通知が1回より多く来ないこと、4秒後にカメラデバイスのストリーム(`DN_CAMSTREAM`)が止まっていることを見る。どちらも起動時の行`renderers=4`でレンダラの上限を上げ、上限のためにレンダラが閉じることがないようにする。マルチプロセスでは、前のレンダラが終了し、メモリを10MB以上解放すること(`procs=`の`left pid=`と`freed=`。テスト機で27〜36MB)も見る。修正する前は、別のサイトへ移った後も`/mpv2`の通知が35回、録画の通知が8回来て、カメラのストリームは止まらなかった。

`test_blink_mpeme`は、暗号化された動画のページを離れることと、移動の途中でレンダラが落ちることを確認する。`127.0.0.1`の`/emk1`は、VP9の暗号化されたWebM(16フレーム)をMediaSourceとClear Keyの`MediaKeys`で再生し、0.2秒を過ぎたところでリンクの押下で同じサイトの`/emk2`へ移る。`/emk2`は`/emk1`へ戻り、これを4回繰り返して、タイトルが`emk done s=1111`になること(各回で再生できたこと)と、レンダラが落ちていないこと(`procs=`に`gone`がない)を見る。次に、起動時の行`killnav=2`(ブラウザが送る2つ目の移動の直後にレンダラを`kill`する)で`/emk1`から`/emk2`へ移る。ブラウザが動いたままエラーのページを出し(`gone`)、Ctrl+Rの〈再読み込み〉で`/emk2`を新しいレンダラで出す(タイトル`emk done s=1`、終了コード0)ことを見る。

### 14.13.8 ネットワークのプロセス

マルチプロセスのブラウザは、ネットワークのプロセスを1つ持つ。ブラウザと同じプログラムで、起動引数の1行目が`network`である。ブラウザは最初のレンダラより前に起動し(`br_proc.cc`の`net_start`)、`TS_SPN_WITHPARENT`でブラウザとともに終わる。このプロセスだけが、ネットワークスタック(14.4)を持つ。接続のプール、Cookie jar、HTTPキャッシュ、QUICのセッション、名前解決、HSTSと、それらのストア(`cookie`、`cache`、`quic`、`hsts`)である。ChromiumのNetwork Serviceと同じ分け方で、レンダラはソケットとcookieを持たない。2つのレンダラは同じcookieとログインの状態を見て、片方が開いた接続をもう片方も使う。以前(段階1)は、レンダラごとにスタックとCookie jarを持ち、ストアを通して互いの変更を読み直していた(「store cookie: N taken in」「store cookie not read」の行)。`process=single`では、以前と同じく1つのプロセスの中にスタックを持つ。

インタフェースは`br_process.mojom`にある。

| インタフェース | 呼ぶ側 | 内容 |
| --- | --- | --- |
| `NetworkService` | ブラウザ(専用のスレッドnetwork) | レンダラのパイプを接続する(`Connect`、レンダラの番号とともに)、レンダラのサイト(`SetSite`)、レンダラが読んでよい拡張機能の実身(`ExtAllow`)、ストアの書き出し、終わり |
| `Network` | レンダラ(どのスレッドからでも) | リクエスト(`Fetch`)、cookie(`CookieLine`、`CookieSet`、cookieStoreの読み書きと監視)、HSTS、データの消去とサイトの一覧、プロトコルの数、WebSocket |
| `NetFetchClient` | ネットワークのプロセス | 1つのリクエストの応答。本体を分けて受けるものは`Response`と`Piece`、終わりは`Done`。`Piece`にはレシーバが取り出したときに応答し、ネットワークのプロセスは応答のない分が512KBになると読むのを待つ。レンダラがパイプを閉じると、そのリクエストは破棄される。ダウンロードでは、レンダラがこのレシーバをブラウザに渡し、ブラウザが続きを受ける(14.7.4) |
| `NetHooks` | ネットワークのプロセス(ネットワークのスレッド) | リクエストを出したレンダラの拡張機能のルール(declarativeNetRequest)と、webRequestの通知 |
| `NetSocket`、`NetSocketClient` | レンダラとネットワークのプロセス | ページのWebSocketの送信と、接続からの通知 |
| `Store` | レンダラ(どのスレッドからでも) | ページの保存域のストアの実身の読み書き。ストアの読込み(`Load`、`LoadEntries`)、書いた世代(`Version`)、保存(`Save`、`Apply`)。パスワードとパスキーの操作(`PasswordsFor`、`PasswordKeep`、`PasskeysFor`、`PasskeyMake`、`PasskeySign`)。拡張機能の一覧、パッケージ、状態の記録(`ExtList`、`ExtRead`、`ExtState`、`ExtStateWrite`、`ExtLink`、`ExtUnlink`) |

レンダラのパイプは、招待の5つ目のパイプ(`kNetworkPipe`、`Network`)と6つ目のパイプ(`kStorePipe`、`Store`)で渡す。ブラウザは、その反対の端を`NetworkService.Connect`でネットワークのプロセスに渡す。ネットワークのプロセスは、パイプを専用のスレッド(network calls)で受ける。ネットワークのスレッドで受けないのは、スタックの関数の中に、ネットワークのスレッドに処理を渡して待つものがあるからである。

レンダラでは、スタックの関数(`br_net.h`、`br_cookies.h`)がそのままネットワークのプロセスへの呼出しになる(`net_fetch_tessronos.cc`の各関数の先頭の`br_net_remote()`、実体は`net_service_tessronos.cc`)。応答はレンダラの専用のスレッド(net answers)に届き、1つのプロセスのときと同じ形で、要求した側のスレッドに渡る。`chrome-extension:`のアドレスには、拡張機能を持つレンダラが自分で応答する。

拡張機能はレンダラが持つので、ネットワークのプロセスのネットワークデリゲートは、リクエストを出したレンダラにルールを問い合わせる(`NetHooks`、同期)。レンダラは、ほかに何もしないスレッド(net hooks)で応答する。リクエストには、レンダラがルールやwebRequestのリスナを持つかどうかを付け(`X-TessronOS-Hooks`の行。スタックが外す)、持たないレンダラには問い合わせない。

ネットワークのプロセスが終了すると(異常終了しても、`kill`されても)、ブラウザのnetworkのスレッドがパイプの切断で気づき、起動し直して、すべてのレンダラに新しいパイプを渡す(`Control.NetworkAgain`)。途中だったリクエストは失敗になり(`ERR_FAILED`)、ページは〈再読み込み〉で読み直せる。ブラウザとレンダラは動き続ける。cookieとキャッシュは、新しいプロセスがストアから読み戻す。書く前だった変更(cookieは2秒以内)は失われる。1分に5回起動しても終わるときは、起動し直すのをやめる。ブラウザが終わるときは、レンダラの後に、ネットワークのプロセスにストアを書かせて(`Flush`)から終わらせる。

ストアのうち、ネットワークスタックのもの(`cookie`、`cache`、`quic`、`hsts`)は、このプロセスだけが書き込むようになった。ページの保存域のストア(localStorage、IndexedDB、Cache Storage、Service Worker、権限、OPFSなど)は、メモリの中身と保存の手順をレンダラが持ち、実身を開いて読み書きするのはこのプロセスである。レンダラの`br_store.cc`は、登録、`br_store_dirty`、差分の計算、書込みのスレッド、定期的な確認、読めなかったストアの記録(`g_unread`)を今のまま持ち、その下のレイヤ(`log_load`、`log_save`、`log_version`、暗号化したストアと以前の形式のストアの読み書き)だけを`Store`の呼出しに替える(`br_store_remote()`、`store_service_tessronos.cc`)。渡すのは、読んだバイト列とエントリ、書く変更(エントリの書込みと削除)、エントリの合計の大きさ(ログを書き直すかの判断に使う)で、応答の数はレンダラの中で開いていたときと同じである。そのため、レンダラはストアのために実身を開かない。レンダラで実身を開こうとすると、拒否して記録に1行出す(`browser: store: an object of the stores would be opened in a renderer: refused`)。ネットワークのプロセスは`Store`を専用のスレッド(store calls)で受ける。ストアの実身は、ほかのプロセスが持っていると最大30秒待つので、`network calls`を止めないためである。応答するのはページのストア(`kObjects`のうち、ネットワークの4つと、暗号化した`creds`、`autofill`を除いたもの)だけで、ほかの名前とエントリの書込みと削除以外の変更は拒否する。レンダラは呼出しを専用のスレッド(store asks)で行い、終わるまで待つ。ストアの書込みのスレッドと定期的な確認のスレッドはMojoのシーケンスを持たず、そこからは同期の呼出しを待てないからである。ネットワークのプロセスが落ちた間は呼出しが失敗し(読込みは`-2`、書込みは15秒後にやり直し)、`Control.NetworkAgain`で新しい`Store`のパイプに付け替わった後に、書けなかった変更を書く。ブラウザがレンダラに`Control.Closing`を送ったときのストアの書き出しは、IOスレッドでなく別のスレッドで行う。書込みはネットワークのプロセスへの呼出しで、そのパイプをIOスレッドが扱うからである。別々に起動したブラウザとの間、同じブラウザのレンダラの間の変更の取込みは、今のところ14.8の定期的な確認(3秒ごとの`Version`と読込み)のままで、これも`Store`の呼出しになった。同じプロセスに置くのは、cookieのようにネットワークとストアの両方にまたがるものがあること、書込みのスレッドとロックが1組で済むこと、2GBのマシンでプロセスを増やさないことによる。落ちたときは両方が一緒に起動し直される。

パスワードとパスキー(`creds`)は、バイト列をレンダラに渡さない。レンダラは`Store`の操作を依頼し(14.4.7.1)、ネットワークのプロセスが暗号化したレコードを開いて応答する。パスキーの秘密鍵はネットワークのプロセスから出ず、署名もそこで行う(`PasskeySign`。クライアントのデータの`type`が`webauthn.get`で`origin`が依頼したオリジンであることも確認する)。応答するのは、そのレンダラのサイトのオリジンの分だけである。レンダラのサイトは、レンダラが自己申告するものでなく、ブラウザが決めたものを使う。ブラウザは、レンダラにページを取得させる前(`Page.Navigate`の前)に、行き先のアドレスのサイト(`net::SchemefulSite`)を`NetworkService.SetSite`で伝え、ページを表示したら、リダイレクトの後のアドレスのサイトにしてそこで固定する。一度ページを表示したレンダラのサイトは変えない。そのレンダラのページが開いたウインドウや、レンダラの上限のために同じレンダラで表示した別のサイトのページには、そのページのサイトのパスワードを出さない。ネットワークのプロセスは`Connect`で受けたレンダラの番号ごとにサイトを持つ。サイトのまだないレンダラ(予備のレンダラ、`about:`のページ)の依頼は2秒までサイトを待ち、来なければ拒否する。サイトの違うオリジンの依頼は拒否し、記録に1行出す(`browser: store: renderer N of the site S asked for the passwords of O: refused`)。iframeは同じレンダラで動くので、別のサイトのiframeにはパスワードを出さない(Chromeはブラウザがフレームのオリジンを確認してiframeにも出すが、OOPIFのないTessronOSのブラウザではフレームのオリジンを確認できない)。1つのプロセスのブラウザでも、ページと別のサイトのドキュメントにはパスワードを出さない(`credentials_tessronos.cc`の`PasswordsOf`)。ネットワークのプロセスが起動し直したときは、ブラウザが`Connect`の後に各レンダラのサイトを伝え直す。住所とカード(`autofill`)は、ブラウザが読んでパネルで選ばせ、選んだ1件だけをレンダラに渡す(`Host.AutofillChoose`)。

拡張機能(14.12)の一覧、パッケージ、状態の記録(有効と無効、`chrome.storage`、アラームなど)も、レンダラは「ブラウザの拡張機能」(`SYSDEF_BROWSER_EXT`)を開かず、`Store`で依頼する(`br_ext_os.cc`の各関数の先頭の`br_store_remote()`)。ネットワークのプロセスがパッケージを読むのは、一覧にある実身と、ブラウザが`NetworkService.ExtAllow`で伝えた実身(〈拡張機能を読み込む〉のパネルで利用者が選んだもの、テスト用の起動時の行`extadd=`)だけで、一覧に追加する(`ExtLink`)のも後者だけ、一覧から外す(`ExtUnlink`)のは一覧にあるものだけである。乗っ取られたレンダラが、UUIDを指定してほかの実身を読む経路にはならない。ブラウザの通知はレンダラからの依頼と別のパイプで届くので、ネットワークのプロセスは1秒まで待つ。この形を使うかどうかは`br_ext_os.cc`の`kExtAsked`で決め、今は使っている。

WebTransport、WebRTCのソケット(P2P)、Web Pushの接続、Direct Socketsは、まだレンダラが自分で開く。そのためのコンテキストは何も保存しない(cookieのストアもキャッシュも持たない)。

テストは`test_blink_mpck`である。`127.0.0.1`の`/mck1`でログインし(HttpOnlyのcookie、長く残るcookie、セッションのcookie、スクリプトが付けるcookie)、別のサイトの`localhost`の`/mck2`(別のレンダラ)が自分のcookieを付け、`127.0.0.1`の`/mck3`(さらに別のレンダラ)が、ログインのcookieをすべて持ち、`localhost`のものは持たないこと、その要求にHttpOnlyのcookieが付いていることを見る。続いてネットワークのプロセスを`kill`し(起動時の行`menu=killnetwork`)、ブラウザが新しいプロセスを起動して、〈再読み込み〉で`/mck3`が同じcookieで表示されることを見る。

ほかのプロセスから届いたメッセージの検査が失敗すると、Mojoはパイプを閉じる。そのとき、見つけたプロセスは`browser: mojo: the renderer <pid> found a bad message`の行で、プロセスの種類と、Mojoの説明(インタフェース、メソッドの番号、要求か応答か(`RequestValidator`、`ResponseValidator`)、エラーの種類)を出す(`br_proc.cc`、Mojoの既定のプロセスのエラーの関数。1つのプロセスで4回まで)。ストアのパイプでは、さらに次の2つを出す。レンダラは、接続していたパイプで`Load`、`LoadEntries`、`Apply`が失敗した最初の1回を出す。ネットワークのプロセスは、64KB以上の応答の後に呼出しがないままそのパイプが閉じたとき(応答を拒否したレンダラは、その後の呼出しがパイプの上で失敗するので、もう何も届かない)、その応答の要素の数、メッセージの中のおよその大きさ、キーと値のFNV-1aを出す(`store_service_tessronos.cc`)。どれも、ふだんは何も出さない。

ソケットの数の上限は、システム全体の上限(このビルドで4096個、最後の32個はカーネルのもの)と、プロセスごとの上限(ソフトの上限1024、ハードの上限はこのビルドで2048)の2段である(9.2.2)。すべての接続を1つのプロセスが持つので、このプロセスがChromiumのすべてのプールを合わせた数を持つ。Chromiumの上限とプロセスの上限の関係は次のとおりである。

| 上限 | 値 | 決めている所 |
| --- | --- | --- |
| TCPのプールのソフトの上限 | 256 | Chromiumの既定 |
| TCPのプールの乱数の上乗せ | ソフトの上限と同じ数まで、合わせて最大512 | Chromiumの`TcpSocketPoolLimitRandomization`(既定で有効) |
| WebSocketのプール | 256 | Chromiumの既定 |
| QUIC | セッションごとにUDPのソケット1つ | Chromium |
| DNSのクライアント | 同時に6つの名前、1つの名前で3つまで | `net_fetch_tessronos.cc` |
| プロセスのソフトの上限 | 1024 | システム(`SO_PRC_SOFT_WANT`) |
| `getrlimit(RLIMIT_NOFILE)` | ソフト1024、ハード2048 | `port/compat/ts_linux.c`が`so_getlimit`の値をそのまま返す |

Chromiumの上限を合わせた数は多くて約830で、プロセスのソフトの上限1024より小さい。LinuxのChromiumが起動時にソフトの上限を8192に上げて、Chromium自身の上限が先に効くようにしているのと同じ関係である。プロセスの上限に当たると`EMFILE`に、表が尽きると`ENFILE`になり、Chromiumはどちらも`ERR_INSUFFICIENT_RESOURCES`として扱う。`setrlimit(RLIMIT_NOFILE)`は`so_setlimit`につなぎ、ソフトの上限がハードの上限より大きいと`EINVAL`、管理者でないプロセスがハードの上限を上げようとすると(`RLIM_INFINITY`など、システムの数を越える値を含む)`EPERM`で拒否する。ほかの資源の`getrlimit`は、今までどおり`RLIM_INFINITY`を返す。

表が64個の1段だったときは、乱数の上乗せまで使うとTCPだけで64個に届き、QUICとDNSのUDP、カーネルの名前の解決がソケットを作れなくなった(Surfaceの実機で、多くの広告のホストに`h2`でつなぐページを開いたとき、`browser: net: no datagram socket (5)`が195回、名前が引けないことが35回あった)。そのため乱数の上乗せを切り、上限を32で頭打ちにしていた。2段にした後に、まず上乗せを戻し(`br_blink.cc`)、プロセスの既定のソフトの上限を1024に、システム全体の上限を表の大きさによらないものにした後に、32の頭打ちも外してChromiumの既定(ソフトの上限256)に戻した(`net_fetch_tessronos.cc`)。上乗せの分(最大256)を足しても約830で、プロセスの上限に収まり、システム全体にもほかのプログラムとカーネルの分が残る。乱数の上乗せは、ページがプールを使い切ってほかのサイトの接続の数を知る手口(クロスサイトの漏洩)を難しくするためのもので、切っている間はこの手口への備えがなかった。上乗せの大きさはソフトの上限と同じなので、頭打ちを32にしたままでは上乗せも32までで、ページが試す数の幅が狭く、この備えが弱かった。

テストは`test_blink_mpsock`である。受け付けても応答しない96のサーバに6つずつ要求し(576の接続、乱数の上乗せをしてもプールが持てる数より多い)、どの求めも失敗しないこと(待つだけになること)、プロセスが少なくともプールのソフトの上限の256個を持つこと、システム全体の上限とプロセスの上限の行が出ないこと、ブラウザが「ソケットがない」行を出さないこと、その間もカーネルがソケットを作れることを見る。表が64個の1段で上乗せを切る前は、ネットワークのプロセスが50個を持って表が満杯になり、12のサーバに6つずつの72の求めのうち23が失敗し、カーネルは10回のうち1回もソケットを作れなかった。32の頭打ちを外した後は、QEMU virtで576の求めのうち失敗は0、プロセスが持ったソケットは最大279個(頭打ちがあったときは最大64個)、カーネルは10回のうち10回ソケットを作れ、システム全体の上限とプロセスの上限の行は出なかった。

ページの保存域は`test_blink_mpkeep`が確認する。`127.0.0.1`の`/mpk1`が、その回だけの値(アドレスの`#`の後)をlocalStorageとIndexedDBのレコードに書いて、別のサイトの`localhost`の`/mpk2`(別のレンダラ)へ移る。`/mpk2`は同じ名前に自分の値を書いて、`127.0.0.1`の`/mpk3`(さらに別のレンダラ)へ移る。`/mpk3`と、後から起動したブラウザの`/mpk4`が、どちらの値も`/mpk1`のものであることをタイトルで見る。QEMU virtでは、`/mpk3`も`/mpk4`も最初の読込みで値を見つけた。

ストアが大きいときのレンダラの起動の繰り返しは`test_blink_mpstore`が確認する。`127.0.0.1`と`localhost`の`/mps`を交互に開き、どの訪問も新しいレンダラで動く。各サイトの最初の訪問が、localStorage(4KBの項目を160)、IndexedDB(8KBのレコードを300)、Cache Storage(4KBの応答を300)を作るので、後のレンダラは起動のたびに数MBのストアを読み戻す。その間も前のレンダラが書き、用意しておくレンダラが同時に起動する。最初の起動で24回、ブラウザを起動し直して10回ずつを3回行い、どの回も最後の訪問まで進み、最初の訪問が残したものを見つけることを見る。QEMU virt(`-smp 4`、`KLOCK=3`)では、62のレンダラの起動のうち59がストアを読み戻し、落ちたレンダラはなかった。テスト用ディスクのシステムのボリュームが`SYS_VOL_SIZE=auto`で満杯のときは、ネットワークのプロセスの書込みが`E_NOMEM`で失敗し(`browser: writing the store storage failed (-33, ...)`)、ストアが空のままになるので、`SYS_VOL_SIZE=1G`を付けて作る。

lib/libpthreadは、待ちの規則が破られたとき、コンソールに最初の16行まで出す(`libpthread: task N woken while it still waits`、`had its wait ended by an error`、`put on a list while it is on one`)。ふだんは何も出さない。レンダラがストアを読み戻す箇所でヒープが破損して落ちた件(Surface Go 2で2回)の調査のためのもので、ARMのQEMUのテストでは1行も出ていない。

### 14.13.9 サンドボックス

レンダラは、ページのスクリプトに乗っ取られても利用者の実身、ネットワーク、デバイス、ほかのプロセスにアクセスできないように、制限付きのプロセス(5.9)として動かす。LinuxのChromiumがseccomp-bpfで行うことを、TessronOSではカーネルのSVCゲートが行う。ブラウザは、プロセスの種類ごとに`include/ts/sbx.h`の組を使う。

| プロセス | 組 | 使えないもの |
| --- | --- | --- |
| レンダラ | `TS_SBX_RENDERER` | 実身をUUIDや名前で開くこと、パスでのファイル、ネットワーク、デバイス、GPU、ほかのプロセス、ウインドウ、システムのもの |
| レンダラ(GPUのプロセスができるまで) | `TS_SBX_RENDERER_GPU` | 上からGPUを除いたもの |
| レンダラ(今の形。保存域のストアと音声・カメラなどを持つ間) | `TS_SBX_RENDERER_STORE` | ほかのプロセス、システムのもの。実身を開くこと、デバイス、GPU、ウインドウ、ネットワークは使える(それぞれの処理がレンダラの外に出るまで) |
| GPUのプロセス | `TS_SBX_GPUPROC` | 同じ。GPUの呼出しと、渡されたメモリの実身は使える |
| ネットワークとストアのプロセス | `TS_SBX_NETWORK` | デバイス、GPU、ほかのプロセス、ウインドウ、システムのもの。ソケットとストレージの実身は使える |

制限はプロセスが自分で掛ける(`ts_sandbox_enter`)。起動の初めに要るもの(書体、ICUのデータ、乱数の実身のキー、Mojoの招待と通信路の受け取り)を済ませてから掛け、その後は、持っているキーと、名前のない実身(`ob_cre_key`で作るメモリの実身とストリームのペア)だけを使う。

レンダラが今呼んでいるシステムコールを、QEMU virtの`test_blink_mp`で、カーネルに一時的に入れたカウント処理で調べた(段階3の作業中のブラウザ。ネットワークは別のプロセスにあり、GPUのラスタ化は使っていない)。2つ目のサイトのページを約40秒表示したレンダラで、次のとおりであった。

| 呼出し | 回数 | 時期と中身 | 制限の後 |
| --- | --- | --- | --- |
| `ob_rea_tmo`、`ob_rea_rec`、`ob_lst_rec`、`ob_wri_rec`、`ob_cls_obj`、`ob_ntf_evt`、`ob_map_rec`、`ob_dup_key`(自分へ)、`ob_cre_key` | 5138、275、217、42、92、4、3、3、2 | 通信路の端とポート、メモリの実身 | 使える |
| `ts_get_mono`、`tk_`(ミューテックス、セマフォ、タスク)、`ts_map_mem`、`ts_ctl_mem`、`ts_unm_mem`、`tk_get_utc`、`dt_getsyszone`、`tm_putstring` | 4329、約1200、45、174、24、7、13、15 | 時刻、スレッドとその待ち、メモリ | 使える |
| `ts_rcv_msg`、`ts_get_arg`、`ts_ref_cpu`、`ts_def_flt`、`ts_get_pid` | 1、2、2、1、35 | 起動の初め(通信路の受け取りなど) | 使える |
| `ob_opn_obj` | 90 | 書体の実身3つ(起動から0.8〜3.5秒)、乱数の実身1つ(0.1秒)、ブラウザの保存域11回とブラウザの拡張機能2回(4.3〜4.8秒、ページの用意)、保存域の中の1つの実身を73回(表示している間ずっと) | 書体と乱数は先に開いておく。保存域と拡張機能の実身は、ストアのプロセスかブラウザに問い合わせる形にする |
| `ob_fnd_sys`、`ob_fnd_lnk`、`ob_lst_obj`、`ob_get_prt` | 3、3、1、1 | 起動の初め(保存域を探す) | 同じ |
| `fs_open`、`fs_stat` | 1、18 | `fs_open`は起動から29msの`/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq` | 制限の前に済ませる |
| `so_socket`、`so_bind`、`so_getsockname`、`so_fcntl`、`so_sendto`、`so_recv`、`so_poll` | 1、1、1、1、1、2、333 | ネットワークのスレッドをウェイクアップするためのループバックのUDPのソケット(`br_stream.cc`の`make_wake_socket`)。レンダラはもうネットワークを持たないのに作っている | 作らないようにする |
| `dp_`、`wm_`、`gp_`、`sr_`、`us_` | 0 | GPUのラスタ化を使うと`gp_`を呼ぶ | 使えない(GPUは段階2まで`TS_SBX_RENDERER_GPU`で許す) |

ネットワークのプロセスは、起動の0.1〜0.3秒にシステムの実身(デバイス)を3回開き、その後はストアの実身(Cookie、「ブラウザの蓄え」、保存域の中の実身)を開き続け、ソケットを使う。システムの実身は起動の初めだけなので、`TS_SBX_NETWORK`を掛けるのはその後にする。

今の組み込みは次のとおりである。

- レンダラ(`br_rproc.cc`の`br_renderer_main`)は、Blinkを初期化し(`br_sys_engine_up`)、通信路とdataのスレッドを用意した後、ページの呼出しを待つループに入る前に`TS_SBX_RENDERER_STORE`を掛ける(`br_main.cc`の`br_sandbox_renderer`)。ページの保存域のストア(localStorage、IndexedDB、パスワードなど)をまだレンダラが持ち、表示中も保存域の実身を開くので、実身を開くこと(`TS_SBX_OBJ`)は許す。GPUも、GPUのプロセス(段階2)が済むまで許す。音声、マイク、カメラのデバイスの実身もレンダラが自分で開く(`br_snd.cc`など)ので、デバイス(`TS_SBX_DEV`)も許す。
- ネットワークのプロセス(`net_service_tessronos.cc`の`br_network_main`)は、スタックとストアを読み戻し(`br_net_prepare`)、招待を受け取った後、ループに入る前に`TS_SBX_NETWORK`を掛ける(`br_sandbox_network`)。
- ブラウザのプロセス(ウインドウを持つ側)と、`process=single`のときのプロセスには掛けない。
- 起動時の行`sandbox=`で形を選ぶ。`refuse`(既定)は`TS_SBX_REFUSE`で拒否するだけ、`kill`は拒否された呼出しでプロセスを終了させ、`off`は掛けない。実機で拒否された呼出しの行が出ないことを確認してから、既定を`kill`にする。
- レンダラはネットワークのスレッドを、ウェイクアップ用のループバックのソケット(`br_stream.cc`の`make_wake_socket`)なしで動かす(`br_net_thread_start(0)`)。スタックがネットワークのプロセスにあるときは、ソケットを待たないので、処理を渡すときのセマフォだけで待つ。
- WebTransport、WebRTCのP2Pのソケット、Web Pushの接続、Direct Socketsは、まだレンダラが自分でソケットを開く。レンダラの組では`so_socket`が拒否され(`EPERM`)、それぞれ接続の失敗としてページに返る。ネットワークのプロセスに移すまで、これらは使えない。
- 残っている例外は、実身(`TS_SBX_OBJ`)、GPU(`TS_SBX_GPU`)、音声・マイク・カメラ(`TS_SBX_DEV`)である。それぞれレンダラの外へ出したら、組から外す。すべて外したら`TS_SBX_RENDERER`になる。
- ページの保存域のストアの実身は、ネットワークのプロセスが開く(14.13.8)。起動時の行`sandbox_renderer=noobj`を与えると、レンダラは`TS_SBX_RENDERER_STORE`に`TS_SBX_OBJ`を加えた組に入る。既定は今のところ`TS_SBX_RENDERER_STORE`のままで、実機で確認してから既定を変える。
  - `OB_DENY_LOG`のビルドのQEMU virtで、`test_blink_sbxobj`(4つのレンダラとネットワークのプロセス)の拒否された呼出しは0である。ページの保存域は動き(`/mpk3`がlocalStorageとIndexedDBの値を読めた)、パスワードとパスキー(14.13.8)、拡張機能の一覧と状態とパッケージ(14.13.8)もレンダラは開かない。この0は拡張機能を`Store`で依頼する形(`kExtAsked`、今は`true`)を使ったときのものである。その形を無効にすると、`noobj`では拡張機能を開く呼出しが拒否される。以前に残っていた2つは次のように修正した。
  - `fs_stat`(`/sys/fs/cgroup/cpuset/chrome`と`/sys/fs/cgroup/schedtune/chrome`):baseはスレッドの種類を変えるたびに(`PlatformThreadLinux::SetThreadCgroupsForThreadType`)、cgroupのディレクトリがあるかを調べていた。TessronOSにcgroupはないので、レンダラは起動の初め(スレッドを作る前)に、スレッドの種類の扱いを自分で処理するようにする(`ThreadTypeDelegate`、`port/base/base_tessronos.cc`の`br_thread_types_own`)。リアルタイムの音声のスレッドは`SCHED_RR`を求め、どの種類もniceの値を設定し、パスは見ない。Chromiumのソースツリーは変えていない。
  - `ob_opn_obj`(「ブラウザの拡張機能」を2回、一覧と状態):ネットワークのプロセスに依頼する形を作った(14.13.8)。今は無効にしてある。
  - 書体、乱数の実身、起動の初めの`fs_open`は、制限の前に済んでいる。
  - このテストで通らない経路で、レンダラが今も実身やデバイスを開くもの(修正していない):Widevine(`br_widevine.cc`。CDMの実身をUUIDで開いて読み、導入と削除で`ob_cre_obj`、`ob_lnk_obj`、`ob_unl_obj`、`ob_del_obj`)、カメラ(`br_cam.cc`。カメラデバイスの実身を開く)、音声とマイク(`br_snd.cc`。`ob_lst_obj`で音声デバイスを探して開く)、HEVCとH.264のハードウェアデコード(`br_hevc.cc`。`ob_lst_obj`とデバイスを開く)、通知(`br_notify.cc`。返事の実身を`ob_cre_obj`で作り、デスクトップの受付を`ob_fnd_nam`で探して開く)、印刷(`br_print.cc`の`br_ask_desktop`。通知と同じ)、音声認識(`br_speech_rec.cc`。辞書の入ったプログラムの実身を開く)、IPv6のアドレスとDNS(`br_sysnet.cc`。システムの実身のレコード7。WebRTCのP2Pなど、まだレンダラが自分で使うネットワークの経路で呼ばれる見込み)。ページへのドロップは、ブラウザが中身をメモリの実身で渡す(`KeepFile`)ので開かない。

テストは`test_blink_sbx`である。`127.0.0.1`の`/mp1`から別のサイトの`localhost`の`/mp2`へ移り、その間、ブラウザの子のプロセスの実身の属性`tessronos.process.sandbox`を読む。レンダラが2つ以上`TS_SBX_RENDERER_STORE`で、ネットワークのプロセスが`TS_SBX_NETWORK`で、どれも拒否する形であること、`OB_DENY_LOG`のビルド(`make OACVLOG=1`)のコンソールに`sandbox want=`の行が1つもないことを見る。
`test_blink_sbxobj`は、`sandbox_renderer=noobj`で`test_blink_mpkeep`の`/mpk1`から`/mpk3`までを動かす。レンダラが`TS_SBX_RENDERER_STORE | TS_SBX_OBJ`の組にあること、`/mpk3`が保存域の値を読めること、拒否された呼出しの行がないことを見る。

### 14.13.10 GPUのプロセス

起動時の行`gpuproc=on`を与えると、ブラウザはGPUのプロセスを1つ持ち、レンダラのページとcanvas 2Dのラスタ化をそこで行う(OOP-R)。既定は`gpuproc=off`で、以前と同じくレンダラが自分のGPUのスレッドでラスタ化する(14.7.2)。実機で確認するまでの間の切替えである。`gpuproc=on`では、WebGLとWebGPUのコンテキストもGPUのプロセスに置く(下の「WebGL」「WebGPU」)。動画のフレームは、`gpuproc=on`でもまだレンダラの中で扱う。

GPUのプロセスはブラウザと同じプログラムで、起動引数の1行目が`gpu`である(`br_proc.h`の`BR_GPU_LINE`)。ブラウザはネットワークのプロセスを起動した後、専用のスレッド(gpu)でGPUのプロセスを起動する(`br_proc.cc`の`gpu_start`)。プログラムの読込みに数秒かかるので、最初のレンダラの起動と並行させる。招待には2つのパイプが付く。`GpuService`(`kGpuServicePipe`、ブラウザが呼ぶ)と`GpuHost`(`kGpuHostPipe`、GPUのプロセスが呼ぶ)である。ブラウザは起動の直後に`GpuService.Start`を呼び、GPUのプロセスはそこでGLを初期化する。

| インタフェースと型 | 呼ぶ側 | 内容 |
| --- | --- | --- |
| `GpuService` | ブラウザ(スレッドgpu) | GLを初期化する(`Start`)、レンダラのチャネルを作る(`EstablishChannel`)、閉じる(`CloseChannel`)、テストの値(`Query`)、終了(`Closing`) |
| `GpuHost` | GPUのプロセス | 後から来たGPUの通知(`GpuArrived`) |
| `Host.GpuChannel` | レンダラ | チャネルを求める。ブラウザはスレッドgpuで`EstablishChannel`を呼び、応答を`Host`のスレッドに戻す |
| `GpuChannelInfo` | | チャネルの片端、`gpu::mojom::GpuInfo`、`GpuFeatureInfo`、`SharedImageCapabilities`をシリアライズしたバイト列、ページをGPUでラスタ化するか(`raster`)、`GL_RENDERER`の文字列 |

チャネルの番号(`client_id`)はレンダラのプロセス番号である。同期トークンのコマンドバッファの番号はチャネルの番号から作られるので、レンダラも同じ番号で`GpuChannelHost`を作る。ブラウザは`Host`のパイプにレンダラの番号を対応付けておき(`ReceiverSet`のコンテキスト)、`GpuChannel`を求めたレンダラを知る。レンダラが終わると、ブラウザはそのチャネルを閉じさせる。

GPUのプロセス(`port/gpu/br_gpu_main.cc`)は、ChromiumのコマンドバッファのサービスをそのままGPUのプロセスで動かす。`gpu::GpuChannelManager`、`Scheduler`、`SyncPointManager`、`SharedImageManager`である。`gpu::GpuInit`はChromiumのウインドウのレイヤ(Ozone)とサンドボックスの補助機能に依存するので使わず、同じ順でGLを初期化する。静的バインディング、ANGLEのディスプレイ(`port/gpu/gl_init_tessronos.cc`)、GLの文字列の取得(`CollectGraphicsInfoGL`)、拡張機能の設定、既定のオフスクリーンのサーフェスの順である。デコーダはパススルーだけで、SkiaのGaneshはANGLEの上で動く。GPUの機能(`GpuFeatureInfo`)は、すべての機能を有効にし、ドライバのワークアラウンドの表は使わない。ドライバはMesaのv3d、iris、softpipeに限られ、ワークアラウンドはANGLEが持つからである。共有イメージのファクトリはOzoneのサーフェスに、ネイティブのピクスマップがあるかを問い合わせる。そのため、ウインドウも入力もピクスマップも持たないサーフェスのオブジェクト(`NoWindows`)を置く。共有イメージはGLのテクスチャだけになる。

GPUを使うかどうかは、GPUのプロセスが決める。GLの`GL_RENDERER`がsoftpipeなら、既定ではラスタ化しない(`raster`が偽)。プロセッサのSkiaの方が速いという14.7.2の方針を保つためである。`gpu=on`ではsoftpipeでもラスタ化し、`gpu=off`ではしない。GPUのプロセスはsoftpipeのマシンでも起動する。

レンダラ(`port/gpu/br_gpu_client.cc`)は、Blinkを初期化する前(`br_sys_engine_up`のGPUの段階)にチャネルを求め、最初の表示のときにメインスレッドでラスタのコンテキスト(`viz::ContextProviderCommandBuffer`)を作る。表示ごとに、ページの記録(`cc::PaintRecord`)を`cc::DisplayItemList`にラップし、白で塗るコマンドを先に置く。ビューの大きさの共有イメージ(RGBA8、用途はラスタの書込みと読出し)に、変わった矩形ごとに`RasterCHROMIUM`で再生する。新しい共有イメージは、最初のラスタで消してから描く(`needs_clear`)。サービスは消していない共有イメージには描かないからである。その後、`ReadbackImagePixels`でページのメモリへ読み戻す(ウインドウのGPUのサーフェスを使わないとき。下の「ウインドウのGPUのサーフェス」)。これは同期の呼出しで、GPUの処理の完了を待つ。1表示の往復は1回にする。矩形が2つ以上あり、それらを囲む矩形の面積が矩形の和の2倍以下なら、囲む矩形を1回で読み戻す。そうでなければ矩形ごとに読む。囲む矩形には変わっていない画素も入るが、共有イメージはいつもページのメモリと同じ画像を持つので、同じ値が書かれるだけである。そのために、新しい共有イメージと、失敗した表示(プロセッサで描いた表示)の次の表示では、ビュー全体をラスタ化して読み戻す。画像は`cc::GpuImageDecodeCache`でデコードして、転送キャッシュで渡す。`br_gpu_start`、`br_gpu_raster`、`br_gpu_stat`の名前はそのままなので、`br_blink.cc`の呼出し側は変えていない。`process=single`と`gpuproc=off`では、同じ名前が`br_gpu.cc`の経路(`br_gpu_inproc_*`)を呼ぶ。

canvas 2D(メインスレッドの、128×128以上で8ビットのもの)も、同じラスタのコンテキストの共有イメージ(RGBA8、用途はラスタの書込みと読出し)に描く。Blinkのcanvasの画素を管理するもののフック(`canvas_gpu.patch`の5つ。パッチは変えていない)に、`br_gpu_client.cc`が次の実装を入れる。`make`は共有イメージを作り、透明(不透明のcanvasは黒)で消しておく。`raster`はcanvasの記録を`cc::DisplayItemList`にラップして`RasterCHROMIUM`で再生し、非同期のフラッシュを1つ出す。画像はページと同じく`cc::GpuImageDecodeCache`で渡す。`write`(putImageData)は`WritePixels`である。`snapshot`は共有イメージを持つ`cc::TextureBacking`の画像を返す。ページのラスタはそのメールボックスでシリアライズされ、GPUのプロセスの中でコピーなしに描かれる。画素を求められたとき(getImageData、toBlob、プロセッサのcanvasへのdrawImage)は`ReadbackImagePixels`で読む。これはどのスレッドからでもよく、コンテキストはロックで保護する。スナップショットは共有イメージへの参照を持つので、スナップショットが残っている間にcanvasへ描くときは、先に新しい共有イメージへコピーしてから描く(コピーオンライト)。`free`は参照を解放する。アニメーション画像をcanvasに描いたときのフレームの選び方(Blinkの`CustomDataOp`のコールバック)は、GPUのプロセスの経路では渡らず、最初のフレームになる(未対応)。

WebGL(1と2)のコンテキストは、GPUのプロセスのGLES2のコマンドバッファである(`port/webgl/gl_cmdbuf.cc`)。Blinkがプラットフォームにコンテキストを求めると(`br_blink.cc`の`BrWebGLProvider`)、レンダラにGPUのプロセスへのチャネルがあれば、そのチャネルにWebGLの属性の`viz::ContextProviderCommandBuffer`(`CreateForWebGL`)を作り、`WebGraphicsContext3DProvider`でラップして渡す。Blinkの呼出しは`GLES2Implementation`がコマンドバッファに積み、GPUのプロセスのパススルーのデコーダ(`GLES2DecoderPassthroughImpl`)がANGLEに渡す。ANGLEのコンテキストは、デコーダがWebGLの属性(`EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE`、リソースの0での初期化、ディスプレイのテクスチャ共有のグループ、クライアントの配列なし)で作るので、WebGLの規則での検査とシェーダの変換はGPUのプロセスのANGLEが行う。デコーダが初期化で求めるANGLEの拡張(`GL_ANGLE_robust_client_memory`、`GL_CHROMIUM_bind_generates_resource`、`GL_CHROMIUM_copy_texture`、`GL_ANGLE_client_arrays`、`GL_ANGLE_webgl_compatibility`、`GL_ANGLE_request_extension`、`GL_KHR_debug`)と、コンテキストを作るのに要るEGLの拡張は、GPUのプロセスが起動時の行に出す一覧でそろっている(QEMUのsoftpipe)。WebGL 2は、GPUのプロセスのGLのバージョンがOpenGL ES 3以上のときだけ出す(`GpuChannelInfo.webgl2`)。`gl_angle.cc`が独自に応答していたもの(フェンスの問合せ、リンクの完了の問合せ、共有イメージ、同期トークン、拡張の要求、`getBufferSubData`)は、Chromiumのクライアントとデコーダが応答する。スレッドごとの仮想化のグループの対策(14.6.1)は要らない。デコーダはすべてGPUのプロセスのメインスレッドで動くからである。

- プロバイダは、ワーカーの`OffscreenCanvas`のものもページのスレッドで作られる(Blinkがそこで求める)。使うスレッドで`BindToCurrentSequence`したときに、コマンドバッファの返事とコンテキストの喪失の通知がそのスレッドにバインドされる。
- ページのcanvasのフレームは、ページをGPUのプロセスでラスタ化するとき(`GpuChannelInfo.raster`)、読み戻さずに描く。合成をCPUで行う設定(`IsGpuCompositingDisabled`)では、Blinkはページを描くときにcanvasのスナップショットを求め(`PaintRenderingResultsToSnapshot`)、共有イメージのリソースを作らずに描画バッファを読み戻す。そこでパッチ(`webgl_snapshot.patch`)がその前にフック(`g_webgl_gpu_snapshot`)を置き、`gl_cmdbuf.cc`がこのブラウザのプロバイダのcanvasに応答する。描画バッファのカラーバッファを、ラスタのコンテキストの新しい共有イメージ(RGBA8、上から下の行の順、用途はラスタの書込みと読出し)にGPUのプロセスの中でコピーし(`DrawingBuffer::CopyToPlatformSharedImage`、WebGLのコンテキストの同期トークンを待つ`CopySharedImage`)。カラーバッファはGLの下から上の行の順だが、Skiaは下から上の順の共有イメージには書かないので、コピーのときに上下を入れ替える、その共有イメージを持つ`cc::TextureBacking`の画像を返す(`br_gpu_client.cc`の`br_gpu_frame_image`)。画像は`UnacceleratedStaticBitmapImage`にラップするので、Blinkのページの描画(`HTMLCanvasElement::PaintInternal`の`MakeUnaccelerated`)はそれを読み戻さずに記録に入れる。ページのラスタはその画像をメールボックスでシリアライズし、GPUのプロセスの中でコピーなしに描く。コピーとページのラスタは同じラスタのコンテキストに積まれるので、順序はコマンドバッファの順で保たれる。画像の画素を求められたとき(プロセッサでラスタ化する表示、プロセッサのcanvasへの`drawImage`、`toDataURL`、WebGPUへの`copyExternalImageToTexture`)は、canvas 2Dのスナップショットと同じく`ReadbackImagePixels`で読む。`copyExternalImageToTexture`のプロセッサの経路は、テクスチャの画像を受けない前提(`CHECK`)なので、パッチ(`webgpu_external_image.patch`)で、テクスチャを持つ画像はその経路の手前で画素に読み戻す。
- WebGLとWebGPUのcanvasは、コンポジタのレイヤで表示する前提のため(`IsComposited`)、描いてもページの描画を描き直さない。このブラウザはcanvasをページの記録に描くので(`html_canvas_painter.patch`)、新しいフレームでcanvasのレイアウトオブジェクトに描き直しを求める(WebGLはフックの有無によらず。14.6.1)(`SetShouldDoFullPaintInvalidation`)。WebGLは描画バッファが前のフレームの後で初めて変わったとき(`webgl_snapshot.patch`の`MarkContextChanged`)、WebGPUは新しいテクスチャを渡すとき(`gpu_canvas_context.patch`の`getCurrentTexture`)である。これがない間は、最初の描画の後のフレームが画面に出なかった(200ミリ秒ごとに描くページで、13秒に1枚)。画像の最後の所有者がなくなるときは、ラスタのコンテキストの同期トークンを付けて共有イメージを消す。ページをプロセッサでラスタ化するとき、メインスレッド以外(ワーカーの`OffscreenCanvas`、14.6.1)、フックが応答できないときは、今までどおり描画バッファがカラーバッファを`ReadPixels`(GPUのプロセスへの1往復)でcanvasのビットマップに読む。コピーと、描画バッファ全体を原点から読む`ReadPixels`を、WebGLのフレームとして`browser: draw: webgl`の行(14.7.2)に数える。プロバイダはラスタのインタフェースと画像のデコードのキャッシュを持たない(`RasterContextProvider`はnull)ので、canvasが取り込む画像の扱いは今までの経路のままである。
- ANGLEがエラーを記録したときの説明は、デコーダがコマンドバッファの`OnConsoleMessage`でレンダラに送り、`GLES2Implementation`がBlinkのエラーメッセージのコールバックに渡す。プロバイダはその間に入り、違う説明ごとに最初の1回だけ`browser: webgl: [.WebGL-0x10000183aad0] GL_INVALID_OPERATION: glDrawArrays: Vertex buffer is not big enough for the draw call.`の形の行をシステムのコンソールに出す(64件まで。括弧の中のコンテキストの名前は比べない)。Blinkはページのコンソールに警告として出す。GPUのプロセスのデコーダも、同じ説明をChromiumのログの`ERROR`の行(`gl_utils.cc`)で出す(コンテキストごとに256件まで)。Blinkが自分で検査して出すエラー(有効な属性にバッファがない、など)はANGLEに届かないので、この行は出ない。
- GPUのプロセスが終了すると、チャネルの喪失がコマンドバッファの喪失として`ContextProviderCommandBuffer`のオブザーバに通知され、プロバイダはBlinkの喪失のコールバックを呼ぶ。Blinkはページに`webglcontextlost`を出し、ページが`preventDefault`したときは、時間をおいてプラットフォームに新しいコンテキストを求め直す。レンダラが新しいチャネルを求めている間は、プロバイダを作らずにnullを返すので(`gl_angle.cc`の経路には行かない)、Blinkは求め直しを続け、新しいチャネルができるとそこにコンテキストを作って`webglcontextrestored`を出す。
- チャネルがないとき(`gpuproc=off`、`process=single`、`gpu=off`、3回落ちて「GPUなし」になったとき)は、今までどおりレンダラの中のANGLE(`gl_angle.cc`)、それもなければCPUの実装(WebGL 1)で作る。

| 行 | 意味 |
| --- | --- |
| `browser: webgl: WebGL 1 in the GPU process, ANGLE (Mesa, softpipe, ...)` | WebGL 1の最初のコンテキストをGPUのプロセスに作った。WebGL 2も同じ形 |
| `browser: webgl: WebGL N in the GPU process could not be made (R)` | コマンドバッファのコンテキストが作れなかった(Rは`gpu::ContextResult`) |
| `browser: webgl: no WebGL 2 in the GPU process (its GL has no OpenGL ES 3)` | GPUのプロセスのGLでWebGL 2が出せない。レンダラの中のANGLEに戻る |
| `browser: webgl: a context in the GPU process is lost: ...` | GPUのプロセスのコンテキストが失われ、ページに通知した(最初の1回) |
| `browser: webgl: no context while the GPU process is started again` | 新しいチャネルを求めている間にコンテキストを求められた(最初の1回) |
| `browser: webgl: the frames are shown from shared images of the GPU process, with no readback` | ページのcanvasのフレームを、GPUのプロセスの共有イメージのコピーとして初めて描いた |
| `browser: webgl: a frame of the GPU process was read back to be shown` | GPUのプロセスのWebGLのフレームを、描画バッファ全体の読み戻しで初めて描いた |

GPUのプロセスの起動時の行の最後に、WebGLのバージョンとWebGPUの有無(`WebGL 1 and 2 and WebGPU (Dawn on ANGLE, OpenGL ES 3.1) here`)が付く。`gl_angle.cc`が出していたコンテキストごとの5秒の数、上限値と拡張の一覧の行は、GPUのプロセスのコンテキストでは出さない。エンジンのスレッドのループの1周の内訳の`WebGL`の時間(14.7)も、GPUのプロセスのコンテキストの呼出しは数えない。

WebGPUのコンテキストは、GPUのプロセスのWebGPUのコマンドバッファである(`port/webgpu/webgpu_cmdbuf.cc`)。Blinkがプラットフォームにコンテキストを求めると(`br_blink.cc`の`BrWebGPUProvider`)、レンダラにGPUのプロセスへのチャネルがあれば、そのチャネルにWebGPUの`viz::ContextProviderCommandBuffer`(`CreateForWebGPU`)を作って渡す。Blinkの呼出しは`WebGPUImplementation`(Dawnのワイヤのクライアント)がシリアライズしてコマンドバッファに積み(ワイヤのクライアント、そのシリアライズと転送バッファの扱いの3つのソースは、gnがDawnのあるときにしかエンジンのグラフに入れないので、`WebGPUImplementation`のコマンドでコンパイルする。`build.sh`の`dawn_lib`)、GPUのプロセスの`WebGPUDecoderImpl`がワイヤのサーバとしてDawnの実装に渡す。応答、サーバの遅延した処理(バッファのマッピング、キューに投入した処理の完了)、デバイスの喪失は、コマンドバッファを通って戻る。14.6.5の同じプロセスのワイヤ、応答のタスク、2ミリ秒ごとのポーリングは、このとき使わない。

- DawnのOpenGL ESのバックエンドは、GPUのプロセスのANGLEの上で動く。デコーダは、アダプタを求めるときにANGLEのディスプレイとANGLEの`eglGetProcAddress`(`gl::GetGLProcAddress`)をDawnに渡す(`RequestAdapterOptionsGetGLProc`)。Chromiumのビルドではこのためのスイッチ(`DAWN_ENABLE_BACKEND_OPENGLES`)が無効なので、デコーダとEGLイメージの共有イメージのソースだけ、先にインクルードするヘッダ(`port/gpu/dawn_gles_flags.h`、`flags.txt`)で有効にする。
- `SystemHasGles31`の確認(14.6.5)はGPUのプロセスで行う。GLを初期化した後、ANGLEのディスプレイでOpenGL ES 3.1のコンテキストを1つ作って破棄し(バージョンを下げない)、作れたときだけWebGPUを出す(`GpuChannelInfo.webgpu`)。そのとき`GpuPreferences`で、WebGPUのスタブを許し(`enable_webgpu`)、アダプタをOpenGL ESのものにし(`use_webgpu_adapter`)、モードを互換(compatibility)に固定し(`force_webgpu_compat`。DawnのOpenGL ESのバックエンドは互換モードしか持たない)、Chromiumのアダプタのブロックリストを使わない(`adapter_blocklist`を無効にする。ブロックリストはプロセッサの上のアダプタを除くので、softpipeが外れる)。Dawnの関数表はDawnの実装のものにする(`dawnProcSetProcs`)。
- Dawnのインスタンスは、ライブラリを探す場所としてプログラムのディレクトリを求める(`base::PathService`の`DIR_MODULE`)。プログラムはディレクトリの中のファイルではないので、GPUのプロセスはプログラムとそのディレクトリのパス(`FILE_EXE`、`FILE_MODULE`、`DIR_EXE`、`DIR_MODULE`)を「ない」と返すプロバイダを先に登録する。baseの既定のプロバイダは`/proc`のリンクを読もうとし、読めないとプロセスを止める。Dawnはここではライブラリを読み込まない。
- `GpuChannelManager`に`DawnContextProvider`は渡さない(9章の未確認15)。それはSkiaのGraphiteがDawnで描くときのもので、WebGPUのデコーダは自分のDawnのインスタンスを持つ。
- canvasの`webgpu`のコンテキストのテクスチャは、GPUのプロセスの共有イメージである。ANGLEのOpenGL ESのバックエンドでは、GLのテクスチャの共有イメージ(`GLTextureImageBackingFactory`)はWebGPUの用途を受け付けないので、EGLイメージの共有イメージ(`EGLImageBackingFactory`)になり、DawnはそのEGLイメージを自分のテクスチャとして取り込む(`DawnEGLImageRepresentation`)。ページを描くときのcanvasの画素(`g_webgpu_canvas_snapshot`、14.6.5)は、canvasのデバイスで今のテクスチャをcanvasごとの共有イメージ(用途はWebGPUの書込みとラスタの読出し)にコピーし(内部の用途で`CopyTextureToTexture`)、その同期トークンの後でラスタのコンテキストの`ReadbackImagePixels`で読む。今のテクスチャはcanvasのものである間Dawnが持っているので、直接は読めない。テクスチャがないときは、最後に読んだ画素を使う。パッチ`gpu_canvas_context.patch`は、このコピーのためにフックにデバイスとテクスチャ(クライアントの番号)を渡す。
- WebGL on WebGPU(14.6.5.1)の同期で作るコンテキストは、応答をIOのスレッドで受ける(`SetReplyTaskRunner`)。ANGLEがデバイスを待つ間も応答が届く。QEMUでは、`test_blink_wgw`のページを`gpuproc=on`で開き、消去の色と三角形の色が正しいことを1度確認した(テストそのものは`gpuproc=off`のまま)。
- チャネルがないとき(`gpuproc=off`、`process=single`、`gpu=off`、「GPUなし」)は、今までどおりレンダラの中のDawn(`webgpu_tessronos.cc`)で作る。新しいチャネルを求めている間と、GPUのプロセスがWebGPUを出さないときは、コンテキストを作らない(ページの`requestAdapter()`はnull)。

| 行 | 意味 |
| --- | --- |
| `browser: webgpu: a context in the GPU process` | WebGPUの最初のコンテキストをGPUのプロセスに作った(`(waited on, for WebGL on WebGPU)`は同期で作るもの) |
| `browser: gpu process N: no WebGPU: ANGLE makes no context of OpenGL ES 3.1` | GPUのプロセスのANGLEがOpenGL ES 3.1のコンテキストを作れない。WebGPUは出さない |
| `browser: webgpu: the GPU process runs no WebGPU (...): no adapter` | 上のとき、ページがアダプタを求めた(最初の1回) |
| `browser: webgpu: a canvas's texture was not read back from the GPU process` | canvasの画素を読めなかった(最初の1回) |
| `browser: webgpu: a context in the GPU process is lost: ...` | GPUのプロセスのWebGPUのコンテキストが失われ、ページに通知した(`device.lost`、最初の1回) |

GPUのプロセスは、`GpuService.Start`のときにコンソールへ次を出す。GLの`GL_RENDERER`とバージョン、ANGLEのEGLの拡張の一覧、GLの拡張の一覧である。レンダラは、最初の表示をブラウザに送った後(2つ目の表示の前)に、GPUのプロセスへの1往復の時間を出す(`browser: gpu: a round trip to the GPU process N us`)。計測方法は、何もしない同期の呼出し(`GpuChannel.Flush`)を100回呼んだ平均である。計測する間はレンダラのメインスレッドが止まるので、最初の表示より後にし、回数も抑えている。1表示の時間は、今までの`browser: draw`の行で5秒ごとに出る。`browser: draw: on the GPU a view: play, flush, read back`の3つは、`gpuproc=off`ではレンダラのGPUのスレッドの記録の再生、ドライバへのコマンドの引渡し(`flushAndSubmit`)、読み戻しである。`gpuproc=on`では、同じ3つのレンダラの側(記録をコマンドにする、送る、GPUのプロセスの処理を待つ読み戻し)で、行の後ろに`in the renderer, of the GPU process`と付く。`gpuproc=on`のときだけ、続けて`browser: draw: in the GPU process:`の行が出る。1表示あたりの往復の回数と1往復の時間、GPUのプロセスのラスタの数(ビューとcanvasの合計)とその1回あたりの記録の再生(シリアライズした記録を復元してGaneshに渡す時間)とフラッシュ(GaneshがANGLEにコマンドを出す時間)、読み戻しの数とその1回あたりの描画の完了までの待ち(`glFinish`)とコピーの時間である。GPUのプロセスの中の時間は、ラスタのデコーダに追加したパッチ(`raster_decoder_time.patch`)が数え、ブラウザが`GpuService.Query("rasterstat")`で問い合わせる。読み戻しの前の`glFinish`は、読み戻しがどのみち待つ分を分けて計測するためのものである。GPUのプロセスでラスタ化した表示がない5秒の間は、GPUの行は出さない。`gpuproc=off`と`process=single`では、GPUのプロセスの項目は出ない。ブラウザは、`procs=`のファイルに`gpu pid=N renderer=...`を、`gpucheck=`のファイルの9つ目の値にGPUのプロセスでラスタ化したビューの数を書く。

`gpuproc=on`では、GPUのプロセスが、読み戻しのあった5秒ごとに次の行を出す。

```text
browser: gpu readback: N reads, K kpx; source S, dest D; skia A us (gl B us, convert C us); new transfer buffers T
```

読み戻しの数と画素の数、読込み元の共有イメージと読込み先の色の並び(`RGBA`か`BGRA`、両方あれば`mixed`)、Skiaの`readPixels`全体の時間、その中のANGLEの`glReadPixels`の時間、残り(色の変換とコピー)、その間に新しく登録された転送バッファの数である。数はラスタのデコーダと転送バッファの管理に追加したパッチ(`raster_decoder_time.patch`、`transfer_buffer_count.patch`)が数える。`glReadPixels`の時間は、GLの関数を名前で取得する箇所(`gl_init_tessronos.cc`、`SetGLGetProcAddressProc`)で`glReadPixels`を時間を計測するラッパに替えて数える。コンテキストの関数表もSkiaのGLのインタフェースも、関数をそこから取るからである。ラッパが時間を計測するのは、ラスタのデコーダが読み戻しをしている間(スレッドごとのフラグ)だけである。GPUのジョブの数と待ち、GPUの周波数は、まだ行に入れていない。

ビューとcanvas 2Dの共有イメージは、GPUのプロセスのGLがBGRAのテクスチャを作れるとき(`texture_format_bgra8888`)、ページのメモリ(N32)と同じBGRA8にする。読み戻しで色を並べ替えないためである。作れないときと、読込み先がBGRAでないときはRGBA8にする。どちらにしたかは、最初に`browser: gpu: the shared images of the view and the canvases are ...`の行で出す。ラスタのコンテキストだけが失われたとき(チャネルは残っているとき)も、`browser: gpu: the raster's context in the GPU process is lost (reset status 0x..., the channel ...)`の行を出し、チャネルが失われたときと同じく新しいチャネルを求める。

ウインドウのGPUのサーフェス(10.2.4)。`gpuproc=on`では、ブラウザはページのウインドウを、GPUのプロセスが作った2枚のBOから表示する。レンダラはページのフレームを2枚に交互にラスタ化して読み戻さず、ブラウザもページのメモリから作業領域のサーフェスへコピーしない。ウインドウマネージャはBOを作業領域の画素として読む。起動時の行`gsurf=off`で、ビューをページのメモリへ読み戻す今までの経路に戻せる(実機で比べるため)。

| 段 | 誰が | すること |
| --- | --- | --- |
| BOを作る | GPUのプロセス(`GpuService.MakeGsurf(w, h)`) | 自分のGPUのコンテキスト(`gp_opn_gpu`)で線形のBOを2枚作り(1行は幅×4を256の倍数に切り上げる)、BOの実身(`OB_S_GPUBO`)にする。BOをテクスチャにし、共有イメージ(BGRA8、`kTopLeft`、用途はラスタの書込みと読出し)として`SharedImageManager`に登録する(`br_gpu_main.cc`の`GsBacking`)。BOは0で始まり、テクスチャの中身は決まっているので、クリア済みのフラグを付けておく。応答は、BOのPRIMEのディスクリプタ(`handle<platform>`。`ts_chan.c`がBOの実身のキーにして渡す)と、共有イメージのメールボックス |
| レンダラに渡す | ブラウザ(`Page.SetGsurf`) | メールボックス2つ、大きさ、GPUのプロセスのプロセス番号、ブラウザが付けた組の番号。レンダラはBOのキーを持たない |
| 描く | レンダラ | ラスタのコンテキストで、次の番のBOの共有イメージに`RasterCHROMIUM`で再生する。描く矩形は、このフレームの矩形と、もう一方のBOに描いた前のフレームの矩形の和(一度も描いていないBOには全体)。その後に同期トークンを作り、`SignalSyncToken`で待つ(メインスレッドは止めない)。GPUのプロセスがそこまでのコマンドを処理したと通知されてから、`FrameSink.GsurfFrame`(このフレームの矩形、組、BOの番号)を送る |
| ウインドウに付ける | ブラウザ | 組の最初のフレームを表示するときに、ディスクリプタが持つキー(`ts_gpu_fd_key`)をウインドウのレコード`OB_WR_GSURF`の`OB_GS_SET`に書く。その後もディスクリプタは組の間保持する |
| 表示する | ブラウザ | `OB_GS_SHOW`(BOの番号、通し番号、矩形)を書く。ウインドウマネージャは、そのBOを使うGPUのジョブが終わった後の合成で表示し、`OB_E_CHANGE`を通知する。ブラウザはウインドウのスレッドで通知を受けて番号を記録し、エンジンのスレッドがページを動かすとき(`pump`。メインのループのほか、テストの待ちや移動の待ちの中でも動く)に`FrameDone`を送る。レンダラはそれまで次のフレームを描かない |

GPUのプロセスがBOに描く形は、GLを初期化するときに1回だけ決める(`gs_choose`)。

- GLがGPUの上にあり、ANGLEのEGLが`EGL_EXT_image_dma_buf_import`を出すとき(`drawn`)は、BOをEGLのDMA-BUFとして取り込み(`DRM_FORMAT_XRGB8888`、モディファイアは線形)、そのEGLイメージをsiblingとするテクスチャを共有イメージにする。ラスタはBOに直接描き、プロセッサは何もコピーしない。共有イメージへの書込みの終わりに`glFlush`して、同期トークンが進む前にGPUのジョブがカーネルに入るようにする。決めるときに64×64のBOで確認する。全体と最初の8行を違う色で消して`glFinish`し、BOの最初と最後の画素を読む。取り込めることと、行の並び(テクスチャの最初の行、つまりSkiaが`kTopLeft`のサーフェスに描く上端が、BOの最初の行)を見る。
- GLがプロセッサの上にあるとき(softpipe。`copied`)は、DMA-BUFを取り込めない。テクスチャはGLのものにし、BOをGPUのプロセスにマップして、書込みの終わりにテクスチャ全体をBOへ読む(`GLTextureHolder::ReadbackToMemory`)。GLがプロセッサで描くのでコピーもプロセッサが行い、レンダラとブラウザとカーネルのコピーはなくなる。BOはGPUのアドレス空間に入れない(`GPU_BO_NOGPU`)。QEMUではこの形で確認する。
- どちらでもないとき(GPUの上のGLだがDMA-BUFを取り込めない、確認の色が合わない、ページをラスタ化しない)はサーフェスを作らず(`none`)、今までの読み戻しの経路のままにする。

GPUのプロセスの起動時の行の最後に、`gsurf=drawn`、`gsurf=copied`、`gsurf=none`のどれかが付く。`none`のときは理由の行も出る。

ページのフレームは、ページのメモリへのもの(`Frame`)とサーフェスへのもの(`GsurfFrame`)が混在してよい。レンダラは、サーフェスがページのメモリと同じ大きさで、サーフェスを作ったGPUのプロセスへのチャネルでページをラスタ化しているときだけサーフェスに描き、そうでないときはページのメモリに描く。種類が前のフレームと違うときはビュー全体を描く。ブラウザは、ページのメモリのフレームが来たとき、ウインドウがサーフェスを表示していれば`OB_GS_DROP`を書き(ウインドウマネージャは表示していたBOの画像をウインドウの画素へコピーして残す)、今までどおり作業領域のサーフェスへコピーする。ウインドウがサーフェスを表示している間は、作業領域のサーフェスに何も描かず、`wm_obj_put`もしない(倍率が100でないときなど、カーネルがBOをウインドウの画素へコピーする形を上書きしないため)。

- 大きさが変わったとき、ウインドウのページが別のページ(別のレンダラ)に替わったとき、GPUのプロセスが替わったときは、新しい組を作って渡し、古い組のディスクリプタを閉じてGPUのプロセスに返す(`GpuService.DropGsurf`)。ウインドウは新しい組の最初のフレームまで古いBOの画像を表示し続ける。前のページから組を取り上げるのは、そのレンダラがほかのページのウインドウに描けないようにするためである。
- かな漢字変換の候補の一覧を出している間と、消した後の1回は、サーフェスを取り上げてページのメモリに描かせる(一覧は作業領域のサーフェスに描くため。決めること8)。
- GPUのプロセスが落ちると、レンダラは同期トークンの待ちを失ったものとして終え、次のフレームからページのメモリに描く。ブラウザはそのフレームで`OB_GS_DROP`して最後の画像をウインドウに残し、新しいGPUのプロセスができたら新しい組を作る。サーフェスを作れないと応答したGPUのプロセス、組をウインドウかレンダラが受け取らなかったGPUのプロセスには、そのプロセスの間は依頼し直さない。
- 取り上げた後に届いた、前の組のフレームは表示せずに返す。
- 表示の通知が1秒来ないときは、ウインドウのレコードを1回読んで確認する(通知が失われたときのため。ポーリングではなく、ページを動かすときに見る)。

`gpucheck=`では、レンダラはサーフェスに描いたビューを、比べるときだけ共有イメージから読み戻してプロセッサの画像と比べる。5秒ごとの描画の行に次の行が追加される。`browser: draw: on the GPU a view:`の読み戻しの時間は、読み戻したビューの数で割る。`procs=`のファイルには`gsurf how=copied sets=N shown=M drops=K`の行が付く。

```text
browser: draw: window buffers (drawn): N views rastered into them, taken in by the GPU process A us after each; M shown, each B us after the window was asked; let go K times for the page's memory
```

サーフェスにラスタ化したビューの数、ラスタの終わりからGPUのプロセスが受け取ったと通知されるまでの時間、ウインドウに表示した数と`OB_GS_SHOW`から通知までの時間(`drawn`ではGPUの描画完了の待ちを含む)、ページのメモリのフレームのためにサーフェスを外した回数である。

| 行 | 意味 |
| --- | --- |
| `browser: gpu: the page's window is shown from buffers of the GPU process (copied), 800x560, no view read back` | 最初の組をページに渡した |
| `browser: gpu: the window can have no GPU buffers (...): each view is read back from the GPU process` | GPUのプロセスがサーフェスを作れなかった。そのプロセスの間は今までの経路 |
| `browser: gpu: the window did not take its GPU buffers (N): each view is read back` | `OB_GS_SET`が拒否された |
| `browser: the window has no GPU buffers (N): each view is read back from the GPU process` | ウインドウが`OB_WR_GSURF`の通知を受け付けなかった(古いカーネル) |
| `browser: gpu process N: no window buffers of WxH: ...` | GPUのプロセスでBOか共有イメージを作れなかった |

`drawn`の形、V3DとIntelの合成がBOを読むこと、Pi 5のv3dで線形のBGRAに描けることは、実機で確認していない(未確認)。

`gpucheck=`のプロセッサの画像との比較は、レンダラを使うときはレンダラが行う(`br_sys_gpu_check`)。ブラウザのウインドウの画素はレンダラのページのメモリのコピーで、ブラウザのエンジンもそのメモリから描くので、ブラウザで比べると同じ画像どうしの比較になるからである。レンダラは、GPUで描いた表示をブラウザへ送った後に、同じビューをプロセッサで描いて比べる。ブラウザは終わるときに、ウインドウのページのレンダラから合計を問合せ(`gpucheck`)で受け取り、ファイルに書く。比較はアルファも含む。

GPUのプロセスが自分で終了すると(落ちた、またはテストの`menu=killgpu`)、ブラウザは回収してすぐ起動し直す。60秒の間に3回終了したときは起動し直さず、以後は「GPUなし」とする。レンダラは`GpuChannelHost`の接続の喪失(`GpuChannelLostObserver`、IOスレッド)で知る。メインスレッドで、コンテキストと共有イメージを破棄し、ページをプロセッサで描くようにし、新しいチャネルを待たずに求める(`Host.GpuChannel`を非同期で呼ぶ。ブラウザはGPUのプロセスを起動し直している間、応答を保留する)。応答が来たら、新しいチャネルでレンダラのすべてのページのビュー全体を描き直す(`Page.Redraw`と同じ)。応答が空なら「GPUなし」として、以後はプロセッサで描く。ブラウザがプロセスの終了を検出する前にレンダラが求めたときは、`EstablishChannel`が失敗するので、ブラウザはそこで終了したものとして扱い、起動し直してからもう1度作らせる。GPUのプロセスにあったcanvasの中身は、Chromeと同じく失われる。canvasは次に描かれるときに新しいコンテキストの共有イメージで続き、コンテキストがない間はプロセッサのサーフェスで続く。プロセッサのサーフェスに描いたものは、コンテキストが戻ったときに共有イメージへ書き込む。失われたコンテキストの共有イメージを指すスナップショットはメールボックスを返さず、ページには描かれない。Blinkには喪失を通知しないので、`contextlost`と`contextrestored`のイベントは出ない(Chromeとの違い)。

後から来たGPU(x64の`igt: ready`など)は、GPUのプロセスが検出する。GLを初期化する前に、GPUのデバイスの実身(`gpu0`、`OB_UUID_GPU_INIT`)を開いてレコード`OB_GP_STATE`の`OB_E_CHANGE`を通信の実身に通知するよう依頼し、その後で`gp_ref_gpu`を見る(この順で取りこぼさない)。GPUがまだ登録されていなければ、専用のスレッドで通知を待ち、通知のたびに`gp_ref_gpu`を見直す。登録されたら`GpuHost.GpuArrived`をブラウザに送る。ブラウザはGPUのプロセスを閉じさせて新しいものを起動する(落ちた回数には数えない)。レンダラは落ちたときと同じ経路でチャネルを作り直す。起動のときにGPUが登録されていたプロセスは、待たない。ポーリングはしない。TessronOSのヘッダを読むのは`br_`で始まるファイルだけなので(`flags.txt`)、待ちの処理は`br_main.cc`の`br_sys_gpu_watch`に置いた。`gpufake=`は、`gpuproc=on`ではGPUのプロセスもGLの前に入れ(`br_sys_gpu_fake_begin`)、結果のファイル(`driver=v3d step=6`)はGPUのプロセスが書く。

QEMU virt(softpipe、4コア)で計測した値は次のとおりである(TCGなので、実機の値ではない)。

| 項目 | 値 |
| --- | --- |
| GPUのプロセスの起動(読込みからGLの用意まで) | 1.4〜3.4秒。空きのメモリが約210MB減る(1693MBから1485MB) |
| GPUのプロセスへの1往復(`GpuChannel.Flush`の平均) | 5.7〜7.2ミリ秒(1000回)、6.7ミリ秒(100回) |
| 1表示の往復の回数 | 1回(読み戻し) |

1表示の時間を、レンダラの中のGPUのスレッドでのラスタ化(`gpuproc=off`、14.7.2)と並べると次のとおりである。ページは`test_blink_gpu`の`/g`(800×560)である。「GPUの処理」は、`gpuproc=off`では記録の再生と`flushAndSubmit`(SkiaがMesaにコマンドを出し、softpipeが描く)、`gpuproc=on`ではGPUのプロセスがコマンドを受けて描き終わるまで(調査のために読み戻しの前に`Finish`を置いて計測し、1往復を含む)である。

| 表示 | 経路 | レンダラでの再生 | GPUの処理 | 読み戻し | 合計 |
| --- | --- | --- | --- | --- | --- |
| 最初の表示(448kpx、シェーダのコンパイルを含む) | `gpuproc=off` | 67ミリ秒 | 1002ミリ秒 | 45ミリ秒 | 約1110ミリ秒 |
| 同上 | `gpuproc=on` | 78ミリ秒 | 1484ミリ秒 | 39ミリ秒 | 約1600ミリ秒(1.44倍) |
| その後の表示(12kpx、動画の矩形) | `gpuproc=off` | 3ミリ秒 | 74〜96ミリ秒 | 4〜6ミリ秒 | 約80〜105ミリ秒 |
| 同上 | `gpuproc=on` | 1.5〜2.5ミリ秒 | 108〜172ミリ秒 | 7〜16ミリ秒 | 約145〜165ミリ秒(約1.8倍) |

読み戻しそのものは遅くない。ビュー全体(448kpx)の読み戻しは23ミリ秒で、そのうち約9ミリ秒が往復である。色の型をRGBAにしても、行の幅を詰めても変わらなかった(23.7ミリ秒、22.6ミリ秒)。以前の計測で「読み戻し」が1.6秒だったのは、同期の読み戻しがGPUのプロセスの描画の完了を待つ時間を含んでいたためである。遅延の大部分はGPUのプロセスの中にある。シリアライズされた記録を復元してGaneshで再生する分と、ANGLEを挟む分である。GPUのプロセスの中で計測すると(`browser: draw: in the GPU process:`の行、QEMU)、その後の表示(30〜45kpx)の1回のラスタで、記録の再生が約2ミリ秒、フラッシュ(GaneshがANGLEにコマンドを出し、softpipeが描く)が約210ミリ秒、読み戻しの前の描画の完了の待ちが約6ミリ秒、コピーが約3ミリ秒だった。遅延のほとんどはフラッシュ、つまりANGLEとsoftpipeの側にある。softpipeはコマンドを受けたときに描くので、実機のGPUではフラッシュと描画の完了の待ちの割合が変わる見込みである(未確認)。

以前、`gpuproc=on`で画面の7か所が黒かったのは、レンダラが最初の表示の前に往復の時間を1000回の呼出しで計測していたためである。約5.7秒の間、最初の表示がブラウザに届かず、テストが画面を見たときにウインドウはまだ何も描かれていなかった。読み戻した画素はアルファも色も正しかった(箱の位置でBGRAが`50 a0 00 ff`)。また、そのときの`gpucheck=`の差が0だったのは、ブラウザで同じ画像どうしを比べていたためで、レンダラで比べるようにした後は、48を超える違いが0、違う画素は約1.9%(最大27)である。

テストは`test_blink_gpuproc`と`test_blink_gpulost`である。`test_blink_gpuproc`は、`test_blink_gpu`のページ`/g`を`gpuproc=on`と`gpu=on`で開く。次のことを見る。`procs=`に`gpu pid=`があり、その`renderer=`がANGLEの上のsoftpipeであること。画面の7か所が`test_blink_gpu`と同じ色であること。プロセッサの絵と比べて、48を超えて違う画素が0.5%以下であること。GPUのプロセスでラスタ化したビューと、canvasの記録が1つ以上あること。getImageDataで読んだcanvasの色(橙、円、putImageDataの緑、スナップショットを描いたプロセッサのcanvasの橙)が正しいこと。動画のプレーンはまだ見ない。このテストページも、ウインドウのGPUのサーフェス(`copied`)から表示する。

`test_blink_gsurf`は、同じページ`/g`を`gpuproc=on`と`gpu=on`で開き、途中で`menu=killgpu`でGPUのプロセスを1回終了させる。画面の箱、画像、動画、グラデーションが`test_blink_gpu`と同じ色であること(新しいGPUのプロセスのBOから表示している画像。canvasとWebGLの中身は古いプロセスとともに失われるので見ない)、`procs=`に`gsurf how=copied`があり、組を2つ以上作り(GPUのプロセスごとに1つ)、サーフェスから表示したフレームが1つ以上あること、GPUのプロセスを終了させた後にビューがラスタ化されたことを見る。

`test_blink_gpulost`は、箱と200ミリ秒ごとに描き直すcanvasのページ`/gpl`を`gpuproc=on`と`gpu=on`で開く。QEMUにはGPUがないので、GPUのプロセスはGPUを待つ。テストは、GPUのプロセスが待機を示したら、テスト用のバックエンド(`GPU_KIND_TEST`)を`knl_gpu_register`で登録する。`menu=waitgpu`で、GPUのプロセスが起動し直され、ページが新しいプロセスで描き直されることを見る。続けて`menu=killgpu`を3回行う。2回目までは、新しいGPUのプロセスが起動し、ページが描き直されること。3回目は起動し直されず(「GPUなし」)、レンダラが「GPUのプロセスはない」と出力すること。最後に、箱とcanvasが画面にあり(プロセッサで描かれている)、タイトルにgetImageDataで読んだcanvasの色があること。テスト用のバックエンドは終わりに外す。

`test_blink_glproc`は、GPUのプロセスのWebGLを`gpuproc=on`と`gpu=on`(softpipe)で見る。テストはブラウザが動く間コンソールを読む。1つ目は`test_blink_glw`のページ`/glw`で、ワーカーのWebGL 1と2、ページが作り続ける多くのコンテキスト、WebGL 2のフェンス、`KHR_parallel_shader_compile`でポーリングするリンクが、`test_blink_glw`と同じ結果になること。レンダラがWebGL 1と2をGPUのプロセスに作ったと出力し、自分のANGLEには作っていないこと。2つ目は`test_blink_glgpu`のページ`/glgpu`で、GPUのプロセスのWebGLのcanvasをWebGPUのテクスチャにコピーし、WebGPUのcanvasをWebGLのテクスチャに取り込んで、色が保たれること。3つ目のページ`/glp`は、ANGLEが拒む描画(バッファのない属性を有効にした`drawArrays`)をし、`getError`が`INVALID_OPERATION`を返し、その説明が`browser: webgl: [`の行でコンソールに出ること、WebGL 2で消した色を読めることを見る。続けて`menu=killgpu`でGPUのプロセスを1回終わらせ、ページが`webglcontextlost`と`webglcontextrestored`を1回ずつ受け、新しいGPUのプロセスのコンテキストで緑に消した色を読むことを見る。ANGLEが拒む描画には、Blinkが検査しない頂点バッファの大きさの不足を使う(Blinkが自分で検査するものはANGLEに届かない)。QEMU virt(softpipe)で`test_blink_glproc`と`test_blink_gpuproc`が通った。4つ目のページ`/glo`は、ページのWebGLのcanvas(緑で消し、GLの上半分の行を青で消す)を200ミリ秒ごとに描き、画面の上半分が青、下半分が緑であること(コピーした共有イメージが上下を保って描かれること)、コンソールに`with no readback`の行が出て、`was read back to be shown`の行が出ないことを見る。3つ目のページでも同じ2つの行を見る。`/glgpu`のWebGLとWebGPUの間のコピーは、どちらのコンテキストもGPUのプロセスにある形で通る。

`test_blink_gltex`は、テクスチャとページのWebGLのcanvasを、`gpu=on`(実機と同じくページをGPUでラスタ化する。QEMUではsoftpipe)で、レンダラのANGLE(`gpuproc=off`)とGPUのプロセス(`gpuproc=on`)の2通りで見る。ページ`/gltx`は、WebGL 1と2のコンテキストで、配列(`texImage2D`)、1色のPNGとJPEG(`/i.png`、`/i.jpg`。ミップマップを作る)、2の累乗でない大きさの小さいcanvas、256×256のcanvas(GPUで描くcanvas)、`ImageBitmap`から、WebGL 2ではさらに`texStorage2D`と`texSubImage2D`、R8のテクスチャ、2D配列のテクスチャのレイヤ(`texImage3D`)からテクスチャを作り、フレームバッファに描いて読み戻した画素が元の色であること(15通り)を見る。ページのcanvasは毎フレーム描き、テクスチャを作り終えて1.5秒の間は赤、その後は緑で上半分をPNGの青にする。画面のcanvasの上半分が青、下半分が緑であること(最初の赤のフレームのまま残らないこと)と、それからの2秒の`requestAnimationFrame`が1秒に64回以下であることを見る。修正する前は、`gpuproc=off`で画面が赤のまま、毎秒162回だった。

`test_blink_gpuwgpu`は、`test_blink_webgpu`のページ`/wgpu`を`gpuproc=on`と`gpu=on`で開き、`test_blink_webgpu`と同じこと(互換モードのアダプタ、コンピュートシェーダの結果、canvasの三角形を2Dのcanvasに描いて読んだ色、デバイスのエラーがないこと、画面の三角形と緑)を見る。あわせて、GPUのプロセスがWebGPUを出すと出力し、レンダラがGPUのプロセスにコンテキストを作り、自分のアダプタを作っていないことをコンソールで見る。
