# 第14章　ブラウザ

## 14.1 概要

小物「ブラウザ」は、Chromium 153(153.0.8010.36)のレンダリングエンジン一式、つまりBlink、V8 15.3、Skiaを、TessronOSのプロセスとして動くようにしたWebブラウザである。作者がBTRON(超漢字)向けに作ったChromiumの移植の移植層と画面の構成を引き継ぎ、システムの呼出しをTessronOSのもの(POSIXのソケット、実身、POSIXスレッド)に置き換えた。

ソースコードはTessronOSとは別のリポジトリ`tessron-chromium`にあり、`make BROWSER=1 BROWSER_BLINK=1`でイメージを作るときに取り込む(14.3)。取り込まないときは、プログラムの実身のレコード1に、ブラウザが入っていないことをメッセージ行に表示するだけの小さなプログラム(`application/browser/br_stub.c`)が入る。

| 項目 | 内容 |
| --- | --- |
| ページの表示 | HTML、CSS、JavaScriptとWebAssembly(V8。どちらも機械語にコンパイルして実行する)、画像(PNG、JPEG、GIF、WebP、JPEG XL、AVIF、SVG)、Webフォント、canvasの2D、WebGL 1と2(`make GL=1`のとき、ANGLEとMesaで描く。そうでなければWebGL 1だけをCPUで描く)。`make GL=1`のブラウザは、GPUのあるマシンでページとcanvasの2DをSkiaのGaneshでGPUにラスタ化する(14.7.2)。iframe、専用ワーカー(`blob:`のURLのものも)、画像のアドレスを直接開いたときの画像の文書(14.6) |
| ページの機能 | `crypto.subtle`、`CompressionStream`、`BroadcastChannel`、Web Locks、Permissions、`Notification`(表示は許さない)、`navigator.storage`、`storage`イベント、`createImageBitmap`と`OffscreenCanvas`の2D、`navigator.sendBeacon`、`navigator.clipboard`、Cache Storage(14.4.4、14.4.5) |
| 通信 | Chromiumのネットワークスタック。HTTP/1.1、HTTP/2、HTTP/3(QUIC)、TLS 1.2と1.3、Cookie、HTTPキャッシュ、フォームの`POST`、`fetch`と`XMLHttpRequest`、`WebSocket`、`EventSource` |
| 動画と音声 | `<video>`と`<audio>`、Media Source Extensions(`MediaSource`。YouTubeなどの動画のサイトが使う)、Encrypted Media Extensions(Clear Keyだけ)、MediaCapabilities。Chromiumのメディアのパイプラインで再生し、音はサウンドデバイスに出す。Web Audio(`AudioContext`)の音もサウンドデバイスに出す |
| 入力 | キーボード、ポインタ、ホイール、かな漢字変換。ページの`alert`、`confirm`、`prompt`、`<select>`の一覧、許可の確認は、ブラウザのデータボックスのパネルで尋ねる(14.7.3) |
| 保存 | 開いているページのアドレスと履歴はページの実身に、Cookie、`localStorage`、`indexedDB`、HTTPキャッシュはそれぞれの実身にしまう。Cache Storageはブラウザが動く間だけ持つ。ページの複写と貼り込みはシステムのトレーを使う |

実際のサイトとして、`https://www.tron.org/ja/`、`https://www.wikipedia.org/`、`https://ja.wikipedia.org/wiki/BTRON`、`https://news.ycombinator.com/`、`https://html.duckduckgo.com/`、`https://www.gnu.org/`、`https://example.com/`をQEMUの上で読み、表示を確かめた(14.10)。

## 14.2 構成

| 場所 | 内容 |
| --- | --- |
| `application/browser/br_main.cc`、`br_main_blink.cc` | ウインドウ、ツールパネル、入力、ページの実身の読み書き。Blink版は`br_main_blink.cc`が`br_main.cc`を読み込んで作る |
| `application/browser/br_blink.cc`、`br_bindings.cc`、`br_loader.cc` | Blinkの起動、ページの組み立てと描画、V8のバインディングの登録、部品(スタイルシート、画像、スクリプト)の読み込み |
| `application/browser/br_http.cc`、`br_tls.cc`、`br_text.cc` | Blinkを使わない版のHTTP、TLS、文字だけの描画 |
| `application/browser/br_obj.cc` | ページの実身のメタデータとレコード0 |
| `application/browser/port/net/` | ネットワークスタックの下の層(TCPとUDPのソケット、名前解決、ルート証明書、Cookie・保存域・キャッシュの保存)、ページのクリップボードとしてのトレー(`br_tray.cc`) |
| `application/browser/port/base/`、`blink/`、`skia/`、`v8/` | Chromiumの`base`、Blink、Skia、V8がOSに求めるもの(メモリ、スレッド、時刻、`localStorage`、`WebSocket`のチャネル、IndexedDBの格納側、フォントマネージャ、PNGのコーデック)。`blink/`には、Blobの格納側(`blob_tessronos.cc`)、専用ワーカーのホスト(`worker_tessronos.cc`)、ブラウザの側のサービス(`services_tessronos.cc`、`cache_storage_tessronos.cc`)もある |
| `application/browser/port/media/`、`webgl/` | メディア要素のプレーヤ(Chromiumのメディアのパイプラインへのつなぎ、音と映像の出力、EMEのエントリポイント)、Web Audioの出力、WebGLのGLES2(ANGLEに渡すものと、CPUで実行するもの)とGLSLの解釈 |
| `application/browser/port/gpu/` | SkiaのGaneshによるページとcanvasの2DのGPUでのラスタ化(GPUのスレッド、14.7.2) |
| `application/browser/port/shim/`、`compat/` | LinuxのインタフェースのうちChromiumが使うもの(`epoll`、`eventfd`、`signal`など)のヘッダと実体 |
| `tools/browser/` | ChromiumのソースをTessronOSのツールチェーンでコンパイルし直すスクリプト、除外の一覧、Chromiumのソースへのパッチ |
| `build_make/browser.mk` | TessronOSのmakefileが`make BROWSER=1`のときに読む規則 |
| `tests/uprog/v8prog.cc`、`skiaprog.cc` | V8とSkiaの試験プログラム |

以上は`tessron-chromium`のリポジトリの中の場所である。TessronOSの側には、プロセスのC++とPOSIXの実行環境(`lib/libcxxrt`、`lib/libpthread`、`include/tscxx`、`include/tsposix`)、プログラムと保存用の実身とデータボックスの定義(`etc/def`、`etc/xtad`)、試験`tests/ktest/ktest_v8.c`がある。ブラウザはパネルを`lib/libui`で出す。`build_make/cxxrt.mk`はそれを`libtsui.a`にし、`build.sh`がブラウザにリンクする。`libcxxrt.a`はC++のプログラムのすべてに丸ごとリンクするので、`lib/libui`を自分でリンクするCのプログラム(基本表計算)と重ならないように別のライブラリにしている。

## 14.3 ビルド

Chromium本体はどちらのリポジトリにも含めない。WSLのホームにChromiumのチェックアウト(`~/cr6/src`、DEPSで固定した依存を含む)を置き、`tools/browser/translate.py`が、Chromiumのビルドシステム(gn)が出力するコンパイルのコマンドから定義とインクルードと言語の水準を取り出して、TessronOSのツールチェーン(aarch64-none-elfのgccとnewlib)でコンパイルし直す。生成物は`~/tessronos-obj/tessronos`に置き、ソースかコマンドが変わったものだけをコンパイルし直す。

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

`GL=1`を付けると、`build.sh`はANGLE(約310ファイル)をコンパイルして、Mesaの静的ライブラリ(`libtsgl.a`、13.6.5)とともにブラウザにリンクし、WebGLをANGLEで描く(14.6.1)。GPUでのラスタ化(`port/gpu/br_gpu.cc`、14.7.2)も一緒にリンクする。SkiaのGanesh(GLのバックエンドを含む)はBlinkの依存としていつもコンパイルしてあり、`br_gpu.cc`が呼ぶときだけリンクされる。付けなければANGLEもMesaもリンクしない。

- ChromiumのソースはLinux向けとして読ませ(`__linux__`と`__TESSRONOS__`を定義する)、足りないLinuxとglibcのインタフェースは`application/browser/port/shim`とTessronOSの`include/tsposix`で補う。
- Chromiumのソースは書き換えない。変更が要るものは`tools/browser/patches`のパッチを、コンパイルするときにコピーに当てる。ソースのコピーは`~/tessronos-obj/_patched`に置いてそのソースのコンパイルに使い、ヘッダのコピーは`~/tessronos-obj/_patched_inc`に置いて、元のヘッダが見つかるインクルードのディレクトリ(チェックアウトの最上位を除く)の前に、コピーの同じディレクトリを加える。
- gnの引数(`tools/browser/args.gn`)で、V8を機械語のコンパイラ(Sparkplug、Maglev、Turbofan、WebAssemblyのLiftoff)とともに作る(`v8_jitless = false`)。WebAssemblyのインタプリタDrumBrakeは作らない(`v8_enable_drumbrake = false`)。DrumBrakeをgccで作るためのパッチ(`wasm_interpreter.patch`、`wasm_interpreter_h.patch`)は、作り直すときのために残している。パッチを当てたヘッダのコピーのディレクトリはV8のすべてのソースのインクルードのディレクトリに加わるので、パッチを外すとV8のすべてのソースがコンパイルし直しになる。ポインタ圧縮を有効にし、その4GBのケージはプロセスの中で1つを共有する。この設定はホストの`mksnapshot`にも効き、スナップショットに入る組込み関数の一覧がプログラムのものと一致する。設定を変えたときは`build.sh gngen`と`build.sh hostgen`をやり直す。
- 使わない部分(テスト、GPUのバックエンド、プロセス間のサンドボックスなど)は`tools/browser/exclude_blink.txt`で外す。ネットワークスタックはQUIC(`net/quic`、`third_party/quiche`、BoringSSLのQUICの部分)も含めてコンパイルし、システムに尋ねる部分(TCPとUDPのソケット、名前解決など)だけを外して、移植層の`port/net`のものを使う。
- 同じ目標の8ファイルを1つの翻訳単位にまとめてコンパイルする。Blinkの1つの翻訳単位は1GBから3GBのメモリを使うので、並列はメモリ16GBで6までにする。最初から全部をコンパイルすると数時間かかる。WSLが使うメモリはWindowsから見ると確保済みのメモリになり、Windowsのページファイル(C:にある)がそれに合わせて大きくなる。C:の空きが少ないときは、並列を減らし、WSLのページキャッシュをこまめに捨てる。WSLのメモリが12GBのときは4並列にし、`TRANSLATE_TUNE="--param=ggc-min-expand=20 --param=ggc-min-heapsize=262144"`でgccの使うメモリを抑える(7並列ではWSLのメモリとスワップを使い切った)。構造体の詰め方を`#pragma pack(<数>)`で変えて戻さないソース(WebRTCの`wav_header.cc`)は、後ろのソースの構造体の配置を変えてしまうので、単位から外して単独でコンパイルする(単位の区切りはそのまま)。
- リンクするときに解決できないシンボルには、`tools/browser/blink_stubs.py`がスタブを作る。関数は最初に呼ばれたときに名前を一度表示して0を返し、変数は0で埋めた領域になる。移植層に足した関数も、定義を無名の名前空間に置くなどしてリンクされないと気づかないうちにスタブになり、`std::string`のように値を返す関数では呼び出した側のメモリが壊れる。新しいインタフェースを足したら、実行の記録に`browser: stand-in called:`でその名前が出ないことを確かめる。Cの名前は関数か変数かを名前から見分けられないので、移植層の変数がリンクされないと関数のスタブ(`.text`の数バイト)が当てられ、そこに書き込んだときに書き込み禁止の違反で落ちる(2026年10月、`br_net_quic`で起きた)。そのため、移植層の名前(`br_`、`ts_`、`knl_`、`tessronos`で始まる名前と、`tessronos::`の中や`Br`で始まるクラスのC++の名前)が解決できないときは、スタブを作らずに名前の一覧を出してビルドを止める。
- `blink.sh all`はBlinkのcore、`base`、`net`などをコンパイルし、makeはそれをコンパイルし直さない。coreのソースに当てるパッチ(`loader_factory_for_frame.patch`など)を足したり変えたりしたときは、`blink.sh all`をやり直す(変わった翻訳単位だけをコンパイルするので数分で済む)。modulesのうちリンクするもの(`blink_modules_src.txt`)とバインディング(`blink_modules.txt`)は、makeのたびに変わったものをコンパイルする。
- メディアのパイプラインのデコーダのために、gnの引数で`proprietary_codecs = true`、`ffmpeg_branding = "Chrome"`、`enable_hls_demuxer = false`とし、ChromiumのFFmpeg(H.264とAACを含む構成)、libvpx、libopusをBlinkと一緒にコンパイルする。FFmpegのNEONのアセンブリ(16ファイル)はコンパイルのコマンドに含まれないので、`build.sh`が`ffmpeg_internal.ninja`の定義とインクルードのディレクトリでアセンブルする。libvpxのうちドット積、行列積、SVEの命令を使うファイルは、`flags.txt`でその命令を許してコンパイルする。これらは実行時にプロセッサがその命令を持つと答えたときだけ呼ばれ、TessronOSでは呼ばれない。
- Rustで書かれた部分(JPEG XL、AVIF、JSON、XML、Webフォントの形式の判定、Skiaのフォントの読込み)は、Chromiumのビルド規則でaarch64向けにビルドし、1つのオブジェクトファイルにまとめてリンクする。
- パッチ`canvas_gpu.patch`は`canvas_2d_bitmap_provider.cc`だけを変え、ヘッダは変えないので、当てたときにコンパイルし直すのはその翻訳単位だけである(`blink.sh all`)。
- 実行ファイルは約122MB(JITを入れる前は約106MB、`GL=1`でANGLEとMesaをリンクすると約142MB)で、プログラムの実身のレコード1になる。`BROWSER_BLINK=1`のときは、システムボリュームを448MB、試験用ディスクを1GBにする。Raspberry Piのカードのイメージでは、システムボリュームはそれより先に2GBと決めてある(3.3)。

`tessron-chromium`は、TessronOSのリポジトリと同じディレクトリに置く。別の場所に置くときは、makeに`CHROMIUM=<場所>`を、スクリプトを直接実行するときは環境変数`TESSRONOS=<TessronOSのリポジトリ>`を与える。

### 14.3.1 x64

x64のブラウザ(3.10)は、ARMの移植をそのまま土台にし、同じChromiumのチェックアウト、同じパッチ、同じ移植層から作る。上流のLinux版から移植し直してはいない。x64で別に持つのは、gnの引数、ツールチェーン、CPUに依存するシムとNASMのアセンブリだけで、移植層のコードはARMと共通である。違いは`#if defined(__x86_64__)`の分岐にまとめた。

スクリプト(`build.sh`、`translate.py`、`gngen.sh`、`hostgen.sh`、`blink.sh`、`rust.sh`、`speech.sh`)は、環境変数`TS_ARCH=x86_64`でx64のものを作る。付けないときはARMで、コンパイルのコマンドは以前と一字一句同じなので、ARMの目的物は作り直しにならない。`make TARGET=_X64_ BROWSER=1 BROWSER_BLINK=1`は、`cxxrt.mk`の`BROWSER_ARCH_ENV`で`TS_ARCH=x86_64`を渡す。

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

| もの | ARM | x64 |
| --- | --- | --- |
| gnの引数 | `tools/browser/args.gn`(`target_cpu = "arm64"`) | `tools/browser/args_x64.gn`(`target_cpu = "x64"`) |
| gnの出力 | `~/cr6/src/out/tessronos`、`tessronos_dawn` | `~/cr6/src/out/tessronos_x64`、`tessronos_x64_dawn` |
| 目的物 | `~/tessronos-obj/tessronos` | `~/tessronos-obj-x64/tessronos_x64` |
| コンパイラ | `aarch64-none-elf-gcc`、`-march=armv8.2-a` | `x86_64-elf-gcc`、`-march=x86-64-v2` |
| リンカスクリプト、crti・crtn | `etc/linker/user/program_cxx.ld`、ツールチェーンのもの | `etc/linker/x64/program_cxx.ld`、`lib/libcxxrt/x86_64`のもの |
| Rust | `aarch64-unknown-linux-gnu` | `x86_64-unknown-linux-gnu` |
| Mesa | `~/tsmesa/libtsgl.a` | `~/tsmesa-x64/libtsgl.a`(softpipeとiris) |
| 生成物 | `build_make/obj/browser.elf`など | `build_make/obj/x64/browser.elf`など(`BROWSER_OUT`) |

- gnの引数は、ARMのものに対して`target_cpu`と、`host_toolchain`・`v8_snapshot_toolchain`を`//build/toolchain/linux:clang_x64`にしたことだけが違う。ホストもターゲットもx64なので、gnの既定ではターゲットのgccのツールチェーンがホストの道具とmksnapshotも作ることになる。ターゲットのコマンドは`translate.py`が読むだけなので、ホストの道具とmksnapshotはChromiumのclangで作る。mksnapshotはx64のホストでそのまま動き、`embedded.S`はx86-64のアセンブリになる。`hostgen.sh`は、ターゲットのgccのツールチェーンが呼ぶ`gcc`、`g++`などを、出力のファイルを作るだけの偽物に置き換える(ARMの`aarch64-linux-gnu-gcc`と同じ扱い)。
- 命令セットの基準はTessronOSのx64と同じx86-64-v2である。x64では、gnが目標ごとに付ける`-march=`、`-mavx2`などを落とさず、基準の`-march=x86-64-v2`の後ろに残す。Skiaの`SkOpts_hsw`、skcmsの`TransformHsw`と`TransformSkx`、libyuv、dav1dなどのAVX2とAVX-512の部分はその命令でコンパイルされ、実行時にCPUIDとXGETBVで選ばれる。カーネルはAVXまでを有効にするので(3.10、XCR0)、AVX-512の部分は選ばれない。
- dav1d、FFmpeg、libaom、libjpeg-turbo、libvpx、OpenH264のx86のSIMDの部分はNASMのソースで、gnはそれを独自のアクションで組み立てる。compile_commands.jsonに出ないので、`tools/browser/nasm_objs.py`が`toolchain.ninja`のアクションから旗とソースを読み、ツールチェーンのNASM(12.1)で組み立てて`libnasm.a`にする。リンクは呼ばれるものだけを取る。BoringSSLはARMと同じくアセンブリを使わない(`OPENSSL_NO_ASM`)。
- `port/shim_x64`(`sys/ucontext.h`、`sys/syscall.h`)を`port/shim`より前に読む。`sys/ucontext.h`はx86-64のLinuxの`mcontext_t`(`gregs`と`REG_RIP`などの番号)、`sys/syscall.h`はx86-64のLinuxのシステムコールの番号である(x86_64-unknown-linux-gnuのRustのコードも同じ番号を渡すため)。`port/shim`のファイルは変えていない。
- x64だけの旗と外すソースは`tools/browser/flags_x64.txt`と`exclude_x64.txt`に書く。`x86_64-elf`のnewlibに無いLinuxの名前(`PROT_GROWSDOWN`、`SI_KERNEL`、`SYS_rt_sigaction`)を、それを名指すソースにだけ与える。
- `blink.sh gen`は、x64のgnの出力で作れない生成物を、ARMの出力(`out/tessronos/gen`)から写す。BlinkとMojoの生成物はアーキテクチャに依存しない。作れないのは、ホストのclangとシステムのlibstdc++(gcc 11)では通らないcryptoのソース1つに依る生成物と、チェックアウトに無いesbuildが要るDevToolsのinspector overlayに依る`components/optimization_guide/core:features`である。
- Rustのコードは`x86_64-unknown-linux-gnu`向けに作る。RustのTLSはELFのTLSで、`lib/libpthread`のTLSの変種II(FSのベース)が受ける。Cの名前の`open64`、`stat64`などに渡るLinuxの旗と`struct stat`の並びは、x86-64のLinuxのものに直す(`port/compat/ts_libc.c`)。
- 処理の時間を足す所(エンジンの1回り、タスク、ANGLEの呼出し)は、ARMでは汎用タイマのカウンタ、x64ではTSC(`rdtsc`)を読む(`port/base/counter_tessronos.h`)。TSCの1秒の数は、最初に単調時計と20ミリ秒の間で比べて求める(`br_counter_hz`)。
- 音声の合成と認識の部品はx64でも同じものを作る。whisper.cppのggmlは、x86の行(`arch/x86`)をx86-64-v2でコンパイルする。
- zstdのHuffmanの復号はx86-64のアセンブリのループを呼ぶが、gnはそのソースをコンパイルのコマンドに出さないので、Cのループにする(`ZSTD_DISABLE_ASM`)。Web Audioの`OscillatorNode`のSSE2の部分(`cpu/x86/oscillator_kernel_sse2.cc`)を`blink_modules_src.txt`に加えた。
- Widevine(14.6.3.1)の部品はaarch64のglibcの共有オブジェクトなので、x64では読み込まない(`port/compat/ts_so.c`の読込みは、x64では「この処理系の共有オブジェクトは無い」と答える)。`com.widevine.alpha`は`NotSupportedError`で、Clear Keyは使える。
- HEVCのハードウェアのデコーダ(14.6.7)はRaspberry Pi 5のものなので、x64にはデバイスの実身「動画の復号器」が無く、QEMUと同じくHEVCを扱えないと答える。
- x64のイメージでは試験のプログラムがESP(区画1)に載るので、`BROWSER_BLINK=1`のときはESPを1GB、システムのボリュームを768MBにする(`browser.mk`の`SD_BOOT_MB`と`SYS_VOL_SIZE`、指定すれば変えられる)。実行ファイルは約225MB(`GL=1`)である。

GPUは13章のとおりで、ANGLEはMesaのEGLのプラットフォーム`tessronos`の上で動き、IntelのGPUの機械ではiris、そうでなければsoftpipeで描く。WebGPUはDawnのOpenGL ESのバックエンドで、システムのOpenGL ESが3.1を出すときに使う(14.6.5)。実機で1回の起動で分かるように、x64のブラウザは起動のときに`browser: gpu: the machine's GPU is Intel's (kind 3, version …)`か`browser: gpu: the machine has no GPU the system drives (softpipe)`の行を出す。続く`egl:`の行(13.8.3)がirisかsoftpipeかを、WebGLのコンテキストを作ったときの`browser: webgl: WebGL 2 on ANGLE, ANGLE (…)`の括弧がレンダラ名とOpenGL ESのバージョンを、WebGPUを使ったときの`browser: webgpu: adapter …`の行かその前の`browser: webgpu: the system's OpenGL ES makes no context of 3.1`がWebGPUの可否を言う。

x64で確かめたこと(2026-10-21、QEMU q35、Windowsのハイパーバイザ、`GL=1`でsoftpipe):

- 2026年10月30日、会議のサイトに要るもの(14.5.2、14.4.8、14.4.12.1、14.6.2)を入れて、`test_blink_rtc`、`test_blink_meet`、`test_blink_coi`が通った(`KT_V8`、音の装置は`intel-hda`)。`test_blink_meet`では、640×360、毎秒30フレームのcanvasを、VP8、VP9、H.264、AV1のどれでも毎秒28フレームで符号化して復号した(Windowsのハイパーバイザ。実機のm3-8100Yではこれより遅い)。受信の音は約33万サンプル再生され、画面の共有は1280×800の`monitor`で読めた。`tools/browser/ice_tcp_server.py`の相手には、TCPの能動の候補で繋がった(`ice-tcp connected pair=tcp,active`)。`KT_SITE`で、`https://meet.google.com/<コード>`は対応外のブラウザと言わずに会議の画面(存在しないコードなので「ミーティング コードを確認」)まで、`https://app.zoom.us/wc/join`は「ミーティングに参加」の入力の画面まで進んだ。

- `https://www.tron.org/ja/`を読み(`KT_SITE`、`NET=user`)、タイトル「トロンフォーラム」で画像と日本語の本文を描いた。
- 試験グループ`v8`の全体(`make TARGET=_X64_ KTEST=1 KTONLY=v8 DESKTOP=1 BROWSER=1 BROWSER_BLINK=1 GL=1 NET=user sdimg`、`tools/x64/ktest64.sh`に`USBQ=1 NET=user`)は91件(V3Dを模擬する`test_blink_v3dfall`は飛ばしていた。2026年10月23日からはx64ではIntelのGPUを模擬して走らせる)で、2026年10月21日に下の直しを入れて流したときは、`test_blink_gpu`がcanvasの画素の1点だけ違って落ちたほかは成功した(`test_blink_gpu`はその前の2回では成功した)。QEMU virtでは92件中、`test_blink_dialog`のほかは成功し、それを直したあとの`test_blink_dialog`も成功した。以前はx64とQEMU virtの両方で落ちていた試験の原因と直しは次のとおりである。`test_jit`は、プログラムとV8の起動の時間が支配的なので、スクリプトが自分で測ったループの時間で比べるようにした。`test_blink_file`は、ページの3つの保存が同時に終わって保存箱を開き合い、1つが`E_BUSY`で失敗していた(保存を1つずつにした、14.7.4)。`test_blink_csp`は、クッキーを確かめる道`/csk`が、`test_blink_cachekeep`の同じ名前の道に先に取られていた(`/csq`に分けた)。`test_blink_api5`は、保存域の許可のハンドルが作った`blob:`のURLを、登録が届く前に引いていた(表の「保存域の許可のハンドルの`createObjectURL`」)。`test_blink_menu`は、押す行が再読込の1つ上(灰色の「進む」)だった。メニューの区切りの線も1行の高さを持つ。`test_blink_speech`の最初の1件は、前の`test_blink_iwa`で印刷のパネルに答えたEnterがページにも届いてボタンをもう一度押し、2つ目の印刷のパネルが答えのないまま残って、デスクトップが終わらなかった(パネルの間はキーをアプリに渡さないようにした、11章)。QEMU virtだけで落ちていた`test_blink_dialog`は、遅い機械でページがまだ`prompt`の答えを受け取る前に`<select>`を押していた(タイトルとパネルを待つようにした)。`test_blink_capture`は、ブラウザと同じ方法で機械にマイクがあるかを調べて判定を選び、`test_blink_speech`と`test_blink_audio`は、音の出力の装置がない機械では飛ばす。V8のJIT、WebAssemblyとそのトラップ、ワーカー、Service Worker、WebGL(ANGLEとsoftpipe)、WebGPU、WebRTC、メディア、PDF、音声の認識と合成、ゲームパッドとMIDI(ソフトウェアのUSBコントローラ)はx64でも成功した。
- `test_blink_rtc`は最初、送る映像の符号化で`std::bad_alloc`で終わった。WebRTCの`common_audio/wav_header.cc`が`#pragma pack(2)`を戻さずに終わり、8ファイルの翻訳単位の中でその後ろの`generic_frame_info.cc`の構造体が詰めた配置になって、`GenericFrameInfo`のコンストラクタが`part_of_chain`の中に書いていた。`translate.py`は、`#pragma pack(<数>)`があって`pop`の無いソースを、単位から外して単独でコンパイルする(単位の区切りは変えない。ARMとx64の両方)。

## 14.4 エンジンの起動

Chromiumの`content`層は使わない。ブラウザのプロセスの中で、`base`の終了時処理、コマンドライン、機能の一覧、スレッドプール、プロセス内のMojoを用意し、`blink::Platform`の実装(埋め込んだ`blink_resources.pak`のリソース、フォント、既定の言語`ja`、ユーザエージェント)、メインスレッドのスケジューラ、V8を順に起動する。ページは`Page`と`LocalFrame`で作り、描くときは文書のライフサイクルを進めて、描画の記録を、システムがプロセスにマップしたウインドウの作業領域の面にSkiaで再生し、変わった矩形だけをウインドウに置く(14.7.1)。ページは常に活性で入力フォーカスを持つものとし(`FocusController`)、入力欄のキャレットを表示して点滅させる。

V8はJavaScriptをバイトコードのインタプリタ(Ignition)で実行し始め、よく実行される関数をSparkplug、Maglev、Turbofanで機械語にコンパイルする。WebAssemblyはLiftoffで機械語にしてから実行し、よく実行される関数をTurbofanでコンパイルし直す。`WebAssembly.instantiateStreaming`はBlinkの応答の読み込みのインタフェースで動く。

フォントは実身「書体」の実身のレコード1を使う(10.3)。ゴシック体のNoto Sans JPを既定と`sans-serif`に、明朝体のNoto Serif JPを`serif`に、等幅のNoto Sans Mono CJK JPを`monospace`に充てる。日本語のNoto CJKの書体を入れたLinuxのデスクトップのChromeと同じ割り当てである。フォントマネージャは、明朝体と等幅にはそれぞれのファミリー名(`Noto Serif JP`、`Noto Sans Mono CJK JP`)で、ほかのどのファミリー名にもゴシック体を返す。実身「書体」に明朝体や等幅がなければ、そのファミリーもゴシック体になる。アイコン用のフォント(Segoe Fluent Iconsなど)の名前には何も返さず、ページやテーマが自分で描くようにする。文書が文字の符号化を示さないときは、日本語のChromeと同じくShift_JISとして読む。

### 14.4.1 JITとメモリ

V8 15.3は、W^Xを守るためのページの属性の切替え(以前の`--write-protect-code-memory`)を持たない。LinuxのAArch64では、コードの領域(ポインタ圧縮のケージの中の64MBから128MB)を読み書きと実行の両方の属性で求め(`kReadWriteExecute`)、そこに書き込んだコードを`FlushInstructionCache`の後にそのまま実行する。スレッドごとに書込みと実行を切り替える方式は、AppleのMAP_JIT(`pthread_jit_write_protect_np`)とx64のメモリ保護キー(PKU)にしかない。

TessronOSは書込みと実行を同時に許すページを作らない(5.3)ので、移植層の`OS::SetPermissions`(`port/v8/platform-tessronos.cc`)は`kReadWriteExecute`を`TS_MEM_JIT`に対応させる。この範囲のページは、カーネルが例外を受けて書込みと実行を切り替える。V8がコードを書き込むとページは読み書きになり、それを実行すると、カーネルがデータキャッシュの書戻しと命令キャッシュの無効化を行って読出しと実行に切り替え、同じプロセスを実行中の他のプロセッサにコンテキスト同期をさせる。V8から見ると、ページは書き込めて実行できるメモリとして振る舞う。`kReadExecute`は読出しと実行のページに、`kNoAccessWillJitLater`は予約だけに対応させる。V8が自分で行うキャッシュの操作(`FlushInstructionCache`はLinuxとして`__builtin___clear_cache`を呼び、`CTR_EL0`を読んで`dc cvau`と`ic ivau`を実行する)は、EL0に許してある。

ページは触れたときに作るので、コードの領域の全体を一度に求めても、使った分のメモリしか使わない。`DiscardSystemPages`はページを残したまま何もしないので、一度使ったページはプロセスが終わるまで残る。

WebAssemblyのメモリの範囲外の参照は、V8のトラップハンドラがモジュールのトラップに変える(`EnableWebAssemblyTrapHandler`)。V8はモジュールのメモリの前後にアクセスできないページを置き、生成するコードは範囲を確かめる命令を持たない。範囲外のロードとストアはそこでデータアボートになり、カーネルがそれをプロセスのエントリポイントに返す(`ts_def_flt`、5.5.2)。移植層はそれを`SIGSEGV`として`sigaction`のハンドラ、つまりV8のトラップハンドラに渡し(`port/compat/ts_signal.c`)、ハンドラは戻り先をトラップの着地点に書き換える。`siginfo_t`の`si_code`はLinuxの値にしてある(`port/shim/sys/signal.h`)。newlibでは`SI_USER`が1で`SEGV_MAPERR`とぶつかり、V8がカーネルの出したシグナルと認めないためである。WebAssemblyのものでない例外は、ハンドラが外れて同じアクセスがやり直され、これまでどおりプロセスが終わる。

試験(14.10)のために、ブラウザの起動の引数の行`v8=<フラグ>`でV8にフラグを与えられる。`v8=--jitless`では機械語を生成せず、DrumBrakeを作らないのでWebAssemblyは使えない。

### 14.4.2 フレームとアイドル時間

ブラウザはコンポジタを使わないので、コンポジタがフレームの始まりにメインスレッドに行わせる処理をブラウザ自身が行う。ページが新しいフレームを求めると(`ChromeClient::ScheduleAnimation`)、ブラウザは画面の書き換えの周期(60Hz、16.667ミリ秒)の刻みでフレームを始める。フレームの始めには、HTMLのイベントループの描画の更新のうちライフサイクルより前の段、つまりresizeとscrollのイベント、メディアクエリのリスナ、CSSアニメーションとWeb Animationsの更新とそのイベント、`requestAnimationFrame`のコールバックを、ウィジェットと同じ`PageAnimator::ServiceScriptedAnimations`で実行する。続いてライフサイクル(スタイル、レイアウト、描画)を進め、そのあとで`IntersectionObserver`と`ResizeObserver`に通知する。フレームを求めるフラグはフレームを始める前に消すので、コールバックの中で次のフレームを求めれば次のフレームが来る。ページの部品がそろうのを待つ間も、100ミリ秒に1回フレームを始めてライフサイクルを進め、表示だけを部品がそろうまで待つ。そのため、`loading="lazy"`の画像や、スクリプトが`IntersectionObserver`で見えたことを知ってから読み込ませる画像も、待つ間に取得が始まる。

次のフレームは、ブラウザの起動から数えた書き換えの刻みのうち、前のフレームを始めた時刻にそのフレームにかかった時間の2倍(書き換え1回分より短ければ1回分)を足した時刻以後の最初のものにする。フレームにかかった時間は、フレームを始めてから、コールバック、ライフサイクル、描画、ウインドウへの表示を終えるまでのメインスレッドの時間である。そのため、軽いページのアニメーションと動画は毎秒60回まで描き直し、1回の表示に10ミリ秒かかるページは毎秒30回、20ミリ秒かかるページは毎秒20回ほどになって、スクリプトとネットワークの仕事にスレッドの半分以上が残る。アプリケーションのループは、何も届いていないときに眠る時間を次のフレームの時刻までに縮める(最長10ミリ秒)。画面の書き換えの時刻そのもの(垂直帰線の時刻)はプロセスに通知するインタフェースがないので、位相は合わせず、周期だけを合わせる。ウインドウマネージャは書き換え1回に組み立てを1回にまとめるので(10.4)、周期が合っていれば、表示の取りこぼしと二重の組み立ては起きにくい。以前は長くても100ミリ秒に1回だったので、`requestAnimationFrame`と動画は毎秒10回までしか描かれなかった。

滑らかなスクロール(`scroll-behavior: smooth`、スクリプトの`behavior: 'smooth'`、キーやホイールの1段)は、Blinkのスクロールのアニメータ(`SetScrollAnimatorEnabled`)が、このフレームの始めにメインスレッドで少しずつ進める。以前はアニメータを切っていたので、すぐに行き先へ飛んだ。

専用ワーカーの`requestAnimationFrame`は、Chromiumでは、ワーカーがviz(`EmbeddedFrameSinkProvider`が作る`CompositorFrameSink`)からフレームの始まりを受けて呼ぶ。このブラウザにはそれがないので、パッチ`begin_frame_provider.patch`で、ワーカーにフレームの元がないときは埋め込み側に頼むようにし、ブラウザが60Hzの刻みでワーカーのスレッドにフレームを始めさせる(`worker_begin_frame`)。1回頼まれるごとに1フレームで、コールバックの中で次を求めれば次のフレームが来る。以前は`requestAnimationFrame`が`NotSupportedError`を投げた。

以前はライフサイクルだけを進めていたので、`requestAnimationFrame`のコールバック、scrollとresizeのイベント、CSSアニメーションは一度も実行されなかった。`requestAnimationFrame`で中身を組み立てるサイト(YouTubeのカスタム要素など)は、そのため画像もアイコンも描かなかった。

メインスレッドのスケジューラは、コンポジタからフレームの始まりと、しばらくフレームが来ないことを通知され、フレームの間のアイドル時間に`requestIdleCallback`のコールバックなどのアイドルタスクを実行する。ブラウザは起動時に一度、フレームはしばらく来ないとスケジューラに通知する(`BeginFrameNotExpectedSoon`)。スケジューラは、ほかに実行するタスクがないときに長いアイドル時間(1回が最大50ミリ秒で、次の遅延タスクの時刻までのもの)を始めて、アイドルタスクを実行する。

### 14.4.3 移植していない機能の扱い

`blink_stubs.py`のスタブ(14.3)は、属性ならnullを、操作なら0を返す。ページから呼べるインタフェースはスタブにしない。以前は、移植していないモジュールの属性(`navigator.ml`、`navigator.managed`など)をページのスクリプトより先に消すスクリプトを各ウインドウで実行していたが、消した跡が残り、Chromeと違う姿になるのでやめた。それらの多くは実行時の機能で切れていて、Chromeと同じく初めから無い。`navigator.managed`はChromeと同じく常にあり、モジュールをリンクしてある。管理されたマシンではないので、問合せはChromeと同じくサービスにつながらず断られる。

EME(`requestMediaKeySystemAccess`、`setMediaKeys`)とMediaCapabilities(14.6.2、14.6.3)、14.4.4から14.4.9に挙げるものと、画面の向き(固定は`NotSupportedError`)、`navigator.connection`(有線のネットワーク、品質は固定の見積もりの`4g`)、`navigator.devicePosture`(`continuous`)、`navigator.virtualKeyboard`、`navigator.windowControlsOverlay`、`registerProtocolHandler`、`launchQueue`、`navigator.ink`、ストレージのバケットと古いFile System API(14.4.5)、FedCMのログインの状態(`navigator.login`)、CSS Paint API(`CSS.paintWorklet`。コンポジタのスレッドがないので、ワークレットはページのスレッドで描く)、`navigator.plugins`と`mimeTypes`と`pdfViewerEnabled`、Storage Access(`document.hasStorageAccess`、`requestStorageAccess`)、メディア要素の出力先(`sinkId`、`setSinkId`。出力は既定の1つで、`""`と`"default"`のほかは`NotFoundError`。プレーヤのない要素のときにフレームのクライアントに尋ねる所は、パッチ`audio_output_device.patch`で既定の出力を答える)、`DeviceOrientationEvent`と`DeviceMotionEvent`(センサーがないのでイベントは起きない)、`MediaStreamTrackProcessor`と`MediaStreamTrackGenerator`は、モジュールをコンパイルして本来の実装で答える。取り除く一覧は、機能を載せるたびに減らす(14.9)。

### 14.4.4 ワーカー、Blob、iframe

Chromiumでは、Blobの中身とその公開URLはブラウザのプロセスが持ち(`BlobRegistry`、`BlobURLStore`)、専用ワーカーのスクリプトの取得と始動もブラウザのプロセスのホストが行う。このブラウザでは、それらを`port/blink/blob_tessronos.cc`と`worker_tessronos.cc`がプロセスの中に持つ。

- Blobの格納側は、Blobの中身(バイト列、ほかのBlobの範囲、ストリームから作ったもの)を専用のスレッド(Blobのスレッド)で持ち、読み出し、範囲の読み出し、データパイプへの書き出しに答える。`URL.createObjectURL`のURLは`BlobURLStore`に登録し、`fetch`、`XMLHttpRequest`、ワーカーの始動が、そのURLを`blob:`の`URLLoaderFactory`(`Range`の要求も扱う)か、トークン(`BlobURLToken`)に解決する。トークンが生きている間は、`revokeObjectURL`の後でも解決できる。ウインドウの`BlobURLStore`はフレームの関連インタフェースとしてページのスレッドで受け、来た順にBlobのスレッドへ渡す。
- `blob:`のURLの要求と、すでにファクトリを持つ要求は、埋め込み側のローダでなくそのファクトリで読む(パッチ`loader_factory_for_frame.patch`)。この経路のローダは読込みの通知の受け手(`ResourceLoadInfoNotifierWrapper`)を必ず使うので、フレームのクライアントは通知をどこにも渡さない受け手を作って渡す。
- 専用ワーカー(`new Worker`)は、Blinkの`WebDedicatedWorkerHostFactoryClient`をブラウザが実装して始める。スクリプトはワーカー自身がワーカーのスレッドで取得する(`WebWorkerFetchContext`)。`blob:`のURLのワーカーは、URLをトークンに解決してから始めるので、作った直後にURLを`revokeObjectURL`してよい。ワーカーにも14.4.3の置き換えのスクリプトを実行する。ワーカーのブローカは、Blobの格納側と14.4.5のサービスをバインドする。
- 共有ワーカー(`new SharedWorker`)は、ブラウザのプロセスが持つ`SharedWorkerConnector`とワーカーのホストを`port/blink/shared_worker_tessronos.cc`がページのスレッドで持つ。アドレス、名前、ページのオリジンが同じ接続は1つのワーカーにつなぎ、最初の接続でBlinkの`WebSharedWorker`を始める。ワーカーは自分のスレッドでスクリプトを取得し(専用ワーカーと同じ`WebWorkerFetchContext`)、接続ごとにメッセージポート付きの`connect`イベントを受ける。スクリプトが読めなかったこと、例外、接続できたことはつないだページのすべてに通知し、ページがすべて去ったワーカーは終わらせる。ワーカーの方針(リファラの方針とCSP)の入れ物はワーカー自身が持ち、ホストには通知しない。DevToolsは使わないので、DevToolsの受け手は作らない。
- iframeは、同じプロセスの`LocalFrame`として作る(`CreateFrame`)。子のフレームの読込みはブラウザのフレームのクライアント(`BrChildFrameClient`)が取得して確定させ、`about:blank`、`srcdoc`、`data:`も扱う。子のフレームは親のページの中に描き、`postMessage`と同じオリジンの`contentDocument`が使える。

### 14.4.5 ブラウザの側のサービス

ブラウザのプロセスがページに答えるMojoのサービスのうち、次のものを`port/blink/services_tessronos.cc`と`cache_storage_tessronos.cc`がBlobのスレッドで持つ。ウインドウは空のブローカ(binderを与えたもの)で、ワーカーは自分のブローカで求める。BroadcastChannelは、ウインドウではフレームの関連インタフェースとしてページのスレッドで受ける。このブラウザは一度に1つのページしか表示しないので、ロックとチャネルはプロセスの全体で名前ごとに持つ。

| サービス | 答え方 |
| --- | --- |
| Permissions(`navigator.permissions`、ほかの機能が求める許可) | ブラウザが尋ねずに与えるもの(クリップボードへの書込み、自分のフレームのストレージアクセス)は許可。クリップボードの読み取りは、利用者にパネルで尋ね(14.7.3)、答えをオリジンごとに覚える。利用者の答えは「ブラウザの保存域」のレコード7に入れ、ブラウザを終えても残る。ほかは問い合わせに`prompt`と答え、求めには拒否で答える |
| Notification | 許可は利用者にパネルで尋ね、答えをオリジンごとに覚える(Permissionsと同じく残る)。通知はデスクトップの通知(11.28)で見せる(14.4.10) |
| Push(`pushManager`) | プッシュサービスに購読を登録し、届いたメッセージを復号してService Workerに送る(14.4.11) |
| 保存の割当て(`navigator.storage.estimate`) | 使用量0、割当て2GBと答える |
| Web Locks(`navigator.locks`) | 名前ごとのロックを共有か排他で持ち、求められた順に与える。持ち主がハンドルを閉じると次に渡す。`steal`は持っているロックを壊す |
| BroadcastChannel | 同じ名前のほかの接続のすべてに、それぞれのスレッドで渡す |
| クリップボード(`ClipboardHost`。ページの中の複写と貼り込み、`execCommand`、`navigator.clipboard`が使う) | システムのトレー(18.16)を使う(`port/net/br_tray.cc`)。書き込みが確定すると、文字列を1行1段落のxmlTADの断片として、画像をPNGとして、1つの組でトレーに入れる。読むときは、手にある組の最初のxmlTADの記録の文字(タグを除き、段落の終わりと改行を改行に、実体参照を文字にしたもの)と最初の画像を返す。ほかのアプリが入れた組も読める |
| Cache Storage(`caches`) | オリジンごとに、名前の付いたキャッシュを作った順に持つ。キャッシュは要求(アドレス、メソッド、ヘッダ)と応答の組の並びで、応答の本文はモジュールが作ったBlobのまま持つ。照合は仕様のとおり、フラグメントを除いたアドレス(`ignoreSearch`ならクエリも除く)、`ignoreMethod`でなければGETだけ、`ignoreVary`でなければ応答の`Vary`が挙げる要求ヘッダを比べる。`put`は同じ要求の項目を置き換える。返す応答は本文を共有するコピーである。キャッシュはブラウザを終えても残る。`put`された応答の本文はそのBlobから読み取って持ち、キャッシュが変わってから2秒後に、前の保存から作られたキャッシュと項目、消えたキャッシュと項目だけを「ブラウザの保存域」のレコード6に書く(エントリ単位の保存、14.8.1)。起動時に読み戻し、本文をBlobに作り直す。本文は合わせて64MBまでを残す。フレームのブローカが空のブローカのときにモジュールが拒むのは、パッチ`global_cache_storage.patch`で外す |

| `cookieStore` | ネットワークスタックのCookieの入れ物を使う(`port/blink/cookie_store_tessronos.cc`)。`get`と`getAll`はページのアドレスのCookieのうちスクリプトが見られるもの(HttpOnlyでないもの)を返し、`set`と`delete`はスクリプトが与えたものから作ったCookie(削除は期限の切れたもの)を入れる。`change`イベントは入れ物の変更の通知から作るので、応答、`document.cookie`、`cookieStore`のどれで変わっても届く。HttpOnlyのCookieは見せず、通知しない。フレームのブローカが空のブローカのときにモジュールが`cookieStore`をnullにするのは、パッチ`global_cookie_store.patch`で外す |
| Payment Request(`new PaymentRequest`) | システムに支払いのアプリも手段もないので、`show()`は`NotSupportedError`、`canMakePayment()`と`hasEnrolledInstrument()`はfalseと答える。BlinkではPayment Requestは試験中の扱いなので、ブラウザが起動時に有効にする(`WebRuntimeFeatures::EnablePaymentRequest`) |

Protected Audience(`joinAdInterestGroup`、`runAdAuction`など)、Topics(`document.browsingTopics`)、Shared Storage(`sharedStorage`)は、Chromium 153自身がそれぞれ「何もしない」、「取り除いた」(`NotSupportedError`)、「無効」(`OperationError`)と答える実装になっているので、そのモジュールをそのまま入れて同じに答える。

ストレージのバケット(`navigator.storageBuckets`、`port/blink/buckets_tessronos.cc`)は、オリジンの中の名前の付いた保存の場所である。バケットのIndexedDBとCache Storageは、オリジンのキーの後ろにバケットの名前を付けたものとして持つので、既定のバケットやほかのバケットのものと分かれ、データベースはほかのものと同じくブラウザを終えても残る。ロックはオリジンのものを使う。バケットのディレクトリ(`getDirectory`)は、バケットのキーで分けたオリジンごとのファイルシステム(下)である。`persisted`と期限はブラウザが動く間だけ覚える。バケットを消すと、そのデータベースとキャッシュも消える。Chromium 153のバケットには`durability()`がない。

古いFile System API(`webkitRequestFileSystem`、`webkitResolveLocalFileSystemURL`、`port/blink/filesystem_tessronos.cc`)は、オリジンごとの一時(`TEMPORARY`)と永続(`PERSISTENT`)のファイルシステムを、ブラウザが動く間メモリの中に持つディレクトリとファイルのツリーとして扱う。アドレスは`filesystem:<オリジン>/temporary/a/b`の形である。ディレクトリの作成、一覧、移動、複写、削除、ファイルへのBlobの書込みと切り詰め、ファイルを`File`として読むこと(スナップショットは大きさと時刻だけを返し、中身はバイト列のBlobにする)ができる。サービスは専用のスレッドで動く。ページがファイルのBlobを待つ呼出しで求め、Blobの格納のスレッドに登録するためである。どちらの種類も、ブラウザを終えると中身は消える。

オリジンごとのファイルシステム(Origin Private File System、`navigator.storage.getDirectory()`、`port/blink/opfs_tessronos.cc`)は、オリジンごとのディレクトリとファイルのツリーをプロセスの中に持つ。ディレクトリは名前ごとの項目を、ファイルはバイト列と書いた時刻を持つ。ページが持つハンドルはツリーのノードへのパイプで、ツリーから外されたノードは`NotFoundError`と答える。

- `getFileHandle`と`getDirectoryHandle`は項目を探し、`create`なら作る。`entries()`、`removeEntry`、`remove`、`move`(名前、別のディレクトリ、その両方)、`resolve`がツリーを扱う。
- `createWritable`のストリームに書いたものは、閉じたときにファイルになる。
- 専用ワーカーの`createSyncAccessHandle`には、Chromeがシークレットウインドウで使うファイルの代理(`FileSystemAccessFileDelegateHost`の`Read`、`Write`、`GetLength`、`SetLength`)で答え、その呼出しがファイルのバイト列をそのまま扱う。ハンドルを閉じるまでファイルをロックし、その間は書き込みのストリームを作らせない(逆も同じ)。
- ほかのコンテキストに送ったハンドル(`postMessage`)は、ノードを指す転送のトークンとして届く。
- ツリーは、最後に変わってから2秒後に全体を「ブラウザの保存域」のレコード8に書き、起動時に読み戻す。

`localStorage`と`sessionStorage`の変更は、同じページの同じオリジンのほかのウインドウ(iframe)に`storage`イベントで通知する(`port/blink/storage_tessronos.cc`)。変更したウインドウ自身には通知しない。

### 14.4.6 Service Worker

Chromiumでは、Service Workerの登録とその保存、ワーカーの起動、どのページを制御するか、ページの取得をワーカーに回すことは、ブラウザのプロセスとネットワークサービスが受け持つ。このブラウザでは、それらを`port/blink/service_worker_tessronos.cc`がページのスレッドで持つ。

- `navigator.serviceWorker.register()`は、スコープごとの登録をブラウザが動く間持ち、ワーカー(Blinkの`WebEmbeddedWorker`)をこのプロセスの専用のスレッドで起動する。スクリプトはワーカー自身が取得する。スクリプトが動くと`installing`にして`install`イベントを送り、済めば待ちにし、動いているワーカーがなければ(または`skipWaiting()`なら)`activate`イベントを送って動いているワーカーにする。`ready`、`getRegistration`、`getRegistrations`、`unregister`に答える。
- フレームに文書ができるたびに(メインフレームでもiframeでも)、その文書をワーカーのクライアントとして、UUIDを付けて覚える。フレームのクライアント(`BrFrameClientBase`)がそのUUIDを持ち、次の文書ができたときとフレームが外れたときに忘れる。文書のアドレスを含むスコープに動いているワーカーがあれば、その文書はワーカーに制御される。iframeの`about:blank`と`about:srcdoc`の文書は、親の文書を制御するワーカーに制御される。`clients.claim()`は、スコープの中のすべての文書を制御する。新しいワーカーが動き出すと、前のワーカーが制御していた文書はそのワーカーのものになる(`controllerchange`)。
- 制御された文書の取得(`br_loader.cc`)は、フレームのローダが持つ文書のUUIDで、まずその文書を制御するワーカーに`fetch`イベントとして送り(`clientId`はその文書。ワーカーの`ControllerServiceWorker`を複製した受け手に送る)、`respondWith()`の応答(Blobかストリーム)を文書に渡す。答えなければネットワークから取る。
- スコープの中のアドレスへ移るとき、メインフレーム(`br_main.cc`の`go()`)では、まずワーカーに`fetch`イベントとして尋ね、30秒まで待つ。応答があればそれをページとして表示する。iframeのGETの移動(`BrChildFrameClient`)も、入れ子の文書の移動(`destination`が`iframe`、`<frame>`なら`frame`。`frameType`が`nested`)の`fetch`イベントとして尋ね、その`resultingClientId`をできる文書のUUIDにする。待たずに先へ進み、応答が来たらそれを文書にし、答えなければネットワークから取る。
- 文書とワーカーの間の`postMessage`、ワーカーの`clients.matchAll()`(制御する文書。`includeUncontrolled`なら同じオリジンのすべての文書。作られた順)、`clients.get()`、`client.postMessage()`、`focus()`に答える。クライアントの`frameType`は、メインフレームの文書が`top-level`、iframeの文書が`nested`である。ワーカーへのメッセージの送り手は、そのワーカーのオブジェクトを渡した文書である。文書やワーカーに渡す登録とワーカーのオブジェクト(本来はブラウザとの関連インタフェース)は、それぞれ専用のパイプにする。
- 動いているワーカーのある登録は、スコープ、登録の番号、ワーカーのスクリプトのアドレスを「ブラウザの保存域」のレコード4に入れ(変わってから0.5秒後)、次の起動の初めに読み戻す。読み戻した登録のワーカーは、最初に要るとき(そのスコープのページを表示するとき、ワーカーへのイベントやメッセージが来たとき)に起動し、スクリプトを取得し直して動かす。`install`と`activate`のイベントは送らない。`unregister`で保存からも消える。
- 通知のクリックと閉じたこと(14.4.10)、プッシュ(14.4.11)のイベントはワーカーに送る。ナビゲーションのプリロードは扱わない。
- 起動したワーカーは、ブラウザが動く間止めない。ワーカーは、イベントのない時間がアイドルの遅延(30秒)を過ぎるとホストに止めることを求め(`RequestTermination`)、答えが来るまで、その後に来たイベントを始めずにキューに置く。ホストはページのスレッドで止めないと答え、ワーカーのアイドルの遅延を既定の30秒に戻す(イベントが5分の時間切れになると、ワーカーは遅延を0にし、キューが空くたびにすぐまた求めるため)。答えないと、キューに置かれたイベントは、メインフレームの移動のイベントが来るか、それぞれが5分で時間切れになるまで始まらない(下の2026年10月28日の記録)。`test_blink_swidle`が、40秒何もしなかった後のページの取得にワーカーがすぐ答えることを確かめる。

ワーカーがバックグラウンドですることは、`port/blink/background_tessronos.cc`がサービスのスレッドで受け持つ。

- 一度の同期(`registration.sync.register`)は、ページのpromiseが解決したときに、ワーカーに`sync`イベントとして送る。マシンはつねにネットワークにつながっているとみなす。イベントが失敗すると5分後、さらに15分後に送り直し、3回目を最後の機会(`lastChance`)とする。
- 定期の同期(`registration.periodicSync.register`)は、登録してから、または最後に送ってから間隔が過ぎるたびに、ワーカーに`periodicsync`イベントとして送る。間隔はページが求めた`minInterval`と12時間の長いほうである。試験では起動の行`periodicsync=`で12時間を短くする。
- Content Index(`registration.index`)は、サイトがオフラインで読めるとする記事などの説明を登録ごとに持つ。ChromeではAndroidだけの機能なので、Chromeと同じく既定では出さず、起動の行`features=ContentIndex`で出す。説明を見せる場所はないので、アイコンは求めず(大きさの一覧を空で答える)、`contentdelete`イベントは送らない。
- バックグラウンドの取得(`registration.backgroundFetch.fetch`、`port/blink/background_fetch_tessronos.cc`)は、求められたものを専用のスレッドで1つずつ取得し(`br_fetch`。Cookieを送る)、状態、アドレス、種類、本文を取得とともに持つ。見ているページとワーカーには進み具合と、1つ済むごとにその要求と応答を伝える。最後のものが済むと、すべての応答が2xxならワーカーに`backgroundfetchsuccess`、そうでなければ`backgroundfetchfail`を送る。ワーカーは、そのイベントが終わるまで、登録の`matchAll`で応答をBlobとして読める。`abort()`は`backgroundfetchabort`を送る。取得が済んだことはデスクトップの通知で伝え、通知の〈開く〉はワーカーへの`backgroundfetchclick`になる。アイコンを見せる場所はないので、アイコンは求めない。取得はブラウザが動く間のもので、終えたときに済んでいなかったものは次の起動で続けない。
- 許可の問い合わせには、`background-sync`、`periodic-background-sync`、`background-fetch`を許可と答える。
- 待っている一度の同期、定期の同期、Content Indexの説明は、Service Workerの登録の番号ごとに「ブラウザの保存域」のレコード5に入れ、次の起動で読み戻す。待っていた一度の同期は起動の3秒後に送る。登録を`unregister`すると、その登録のものも消える。

### 14.4.7 資格情報

`navigator.credentials`(`port/blink/credentials_tessronos.cc`)は次のように答える。

| 種類 | 答え方 |
| --- | --- |
| パスキー(WebAuthn) | ブラウザ自身がソフトウェアの認証器(プラットフォームの認証器)を持つ。`create()`は、利用者にパネル5で尋ねてから、依拠するサイト(RP ID)ごとにP-256の鍵を作り、`fmt`が`none`の認証の文書と認証器のデータ(RP IDのハッシュ、フラグUP・UV・AT、カウンタ、AAGUID、資格情報のID、COSEの公開鍵)をCBORで作って返す。`get()`は、許された資格情報か、なければそのサイトの資格情報(複数ならパネル4で選ぶ)で、利用者に尋ねてから、認証器のデータとクライアントのデータのハッシュにECDSA(SHA-256)で署名して返す。鍵は「ブラウザの保存域」のレコード9に、パスワードとともに暗号化して書き(PKCS#8の秘密鍵、ID、RP ID、利用者のIDと名前、カウンタ。14.4.7.1)、次に起動したブラウザでも使える。RP IDはページのホストか、それを含むドメインでなければならない |
| FedCM(`identity`) | IDプロバイダの設定(`configURL`)を取得し、その`accounts_endpoint`から利用者のアカウントを(IDプロバイダのCookieとともに)取得する。アカウントをパネル4で選ばせ、`id_assertion_endpoint`に`account_id`、`client_id`、`nonce`を送ってトークンを受け取り、ページに渡す |
| パスワード(`PasswordCredential`) | `store()`は、利用者にパネル5で尋ね、許せばページのオリジンの資格情報として保存する。`get({password: true})`は、そのオリジンの保存したものを、パネル(1つなら確認、複数ならパネル4の一覧で、最後に保存したものを選んだ状態から)で選ばせてから返す。`mediation: 'silent'`では、1つだけあってページが`preventSilentAccess()`を呼んでいないときだけ尋ねずに返す |
| ログインのフォーム | Chromeのパスワードの管理と同じく、パスワードの欄に値のあるフォームを送るとき(`submit`のイベントの前、`DispatchWillSendSubmitEvent`)、その利用者名(パスワードの欄の前の文字・メール・電話の欄)とパスワードを保存するかをパネル5で尋ねる。保存したパスワードのあるオリジンのログインのフォームの欄にフォーカスが移ると(`FocusedElementChanged`、文書ごとに1度)、入れるかをパネルで尋ね、選んだものを欄に入れる(`input`と`change`のイベントが出る) |
| フェデレーテッドの資格情報 | 保存しない |
| WebOTP | SMSを受ける手段がないと答える |

#### 14.4.7.1 パスワードとパスキーの保存

パスワードとパスキーは、平文では実身に置かない。ChromeがLinuxで暗号の鍵をOSの鍵の保管(libsecret、KWallet)に預け、保存するパスワードをAESで暗号化するのと同じ考えで、鍵を利用者ごとの実身に置き、レコード9をAES-256-GCMで暗号化する(`port/net/br_store.cc`の「sealed stores」)。

鍵は、TessronOSの資格情報と実身の保護(8.6、8.7)に預ける。ブラウザは、自分が代理するユーザ(`ob_get_crd`)ごとに、初めて保存するときに実身「ブラウザの鍵」を「ブラウザの保存域」と同じボリュームに作る。所有者はそのユーザ、モードは`rw-------`、ACLはなく、レコード1に乱数(BoringSSLの`RAND_bytes`)の32バイトの鍵を持つ。どの仮身からも指されないので、固定(`ob_lnk_obj`)を1つ付けて屑実身にならないようにする。ログインのパスワードから鍵を導く方法は採らない。ブラウザはログインのパスワードを受け取らず(8.7.2のとおりデスクトップの中だけで確かめる)、ログインしない利用者はパスワードを持たないからである。

レコード9は、12バイトのヘッダ(14.8)の後に`BRK1`、部分の数、ユーザごとの部分を並べる。部分は、ユーザのUUID、鍵の実身のUUID、12バイトのナンス、暗号文と16バイトのタグである。タグは`BRK1`、ユーザ、鍵の実身も覆うので、部分を別のユーザの位置に移すと開けない。ブラウザは自分のユーザの部分だけを開き、書くときは自分の部分だけを新しいナンスで作り直し、ほかのユーザの部分はそのまま残す。鍵を読むときは、鍵の実身の所有者がそのユーザで、グループとほかの人に何も許さず、ACLがないことを確かめ、満たさない鍵は使わない。書くときは、前の中身をその場所で0で書きつぶしてから新しい中身を書く。

この形より前のブラウザが平文で書いたレコード9は、起動したときに読み込み、その場で暗号化して書き直す。前の中身はその場所で0で書きつぶし、TSFSのジャーナルに残る前のコピーも`ob_wip_vol`で0で書きつぶす(7.3)。

守れるものと守れないものは次のとおりである。

| 場面 | 守れるか | 理由 |
| --- | --- | --- |
| 同じマシンのほかのユーザ、ほかのユーザとして動くプログラムが「ブラウザの保存域」を読む | 守れる | 「ブラウザの保存域」は誰でも読めるが、中身は暗号文で、鍵の実身はそのユーザだけが開ける |
| 部分をほかのユーザの部分と入れ替える、書き換える | 守れる | タグが合わず開けない。開けない部分は読まなかったものとして扱う |
| 管理者(管理者のグループの資格で動くもの) | 守れない | 管理者はすべての実身を開ける。LinuxのChromeでもrootは鍵の保管を読める |
| 同じユーザとして動くほかのプログラム | 守れない | 鍵の実身を開ける。Chromeでも、同じユーザのプログラムはlibsecretから鍵を取り出せる |
| ログインしない利用者(`利用者`)どうし | 守れない | マシンの前でログインしない人は、みな同じユーザである |
| ディスクを外して別のマシンで読む、ボリュームのイメージを読む | 守れない | 鍵と暗号文が同じディスクにあり、鍵を包むログインのパスワードやハードウェアの鍵がない。ChromeがLinuxで鍵の保管を使えないときの`basic`と同じ程度である |
| 動いているブラウザのメモリ | 守れない | 開いたパスワードと鍵はプロセスのメモリにある。使い終えたバッファは`OPENSSL_cleanse`で消す |
| 平文の形からの移し替えの前に書かれたブロック | 守れる(このボリュームの中では) | 移し替えでは、同じ場所とジャーナルを0で書きつぶす。前の形のブラウザが書いたものを別のボリュームに複製したり、バックアップに取ったりしていれば、そこには残る |

住所とカード(レコード12)も同じ形式と同じ鍵で暗号化する。

鍵の実身を消すと、そのユーザの保存したパスワードとパスキーは開けなくなる。次に保存するときに新しい鍵を作り、そのユーザの部分を作り直す。

### 14.4.8 機器のないもの

機器に関わるWebのインタフェースは、Blinkのモジュールをそのまま使い、ブラウザのプロセスの側は`port/blink/devices_tessronos.cc`がこのマシンにあるものを答える。

| インタフェース | 答え方 |
| --- | --- |
| 電池(`navigator.getBattery()`) | 電池がないので、電源につながったマシンと同じく、充電中で満充電と答える |
| 画面の消灯の防止(`navigator.wakeLock`) | 許可する。システムはページを表示している間、画面を消さない |
| センサー(`Accelerometer`など) | センサーがないと答える(`NotReadableError`)。センサーの読取りの共有の領域は作らない。種類ごとの最大の頻度はChromiumの表(`sensor_traits.cc`)による |
| アイドルの検出(`IdleDetector`) | 利用者は操作中で、画面はロックされていないと答える |
| 共有(`navigator.share()`) | タイトル、文、アドレスを1組にしてトレーに入れる(コピーと同じ)。ChromeはLinuxでは出さないので、既定では出さず、起動の行`features=WebShare`で出す |
| キーボード(`navigator.keyboard`) | 配列の表はシステムがキーのコードを読むUSの配列。キーのロックは許可する |
| MIDI(`requestMIDIAccess`) | マシンのUSBのMIDIの機器(オーディオクラスのMIDIストリーミングのサブクラス)をつなぐ(`port/blink/midi_tessronos.cc`)。システムのオーディオのドライバはオーディオのストリーミングのインタフェースしか取らないので、MIDIの機器はプログラムの機器(6.3.1)として残り、ブラウザが開いてUSB MIDIをバルクのエンドポイントで話す。エンドポイントのクラス固有の記述子が挙げる埋め込みのジャックがケーブル1本ずつで、OUTのエンドポイントのものがページの出力、INのものがページの入力になる。名前は機器の名前(ケーブルが複数ならその番号を付ける)。入ってくる4バイトのイベントのパケットは、コードインデックスが示すバイト数を取り出し、ケーブルごとに完全なメッセージにまとめて(`midi::MidiMessageQueue`)ページに渡す。ページが送るメッセージはイベントのパケットに分け(システムエクスクルーシブは3バイトずつ)、機器ごとのスレッドで書き、送れたバイト数をページに通知する。機器はセッションを始めるときに探す。許可は14.4.5の許可の扱いによる |
| アプリのバッジ(`navigator.setAppBadge`) | ブラウザのウインドウのタイトルの前に括弧で出す(`(3) 題`、数のないバッジは`(●)`)。関連アプリはないと答える |
| プレゼンテーション(`PresentationRequest`) | 2つめの画面はないと答える |
| Bluetooth(`navigator.bluetooth`) | アダプタがないと答える(`getAvailability()`はfalse) |
| HIDとUSBの機器(`navigator.hid`、`navigator.usb`) | WebUSBは、システムのクラスドライバが取らなかった機器(6.3.1)を扱う(`port/blink/usb_hid_tessronos.cc`、`port/net/br_usb.cc`)。`requestDevice()`はフィルタに合う機器をパネル(`br_choose`)に並べ、選んだ機器をそのオリジンのものにする(ブラウザが動く間)。機器の情報(`UsbDeviceInfo`)は、機器、構成、文字列の記述子から作る。開く、構成を選ぶ(システムが設定した構成だけ)、インタフェースを取る、代替設定、停止を解く、リセット、コントロールの転送、バルクとインタラプトの転送、アイソクロナスの転送(1サービス間隔に1パケット)ができる。待つ呼出しはそれぞれのスレッドで行い、サービスのスレッドで答える。WebHIDは、同じ機器のうちHIDのインタフェースを扱う(キーボードとポインタはシステムのHIDのドライバのもので、一覧に出ない)。報告記述子はインタフェースへの`GET_DESCRIPTOR`で読み、Chromiumのパーサ(`services/device/public/cpp/hid`)でコレクションと報告の大きさにし、mojomの通常の型からBlinkの型へ、直列化したバイト列で移す。開くと、インタラプトのINのエンドポイントを専用のスレッドで読み続け、届いた報告をページに渡す。出力の報告はOUTのエンドポイント(なければ`SET_REPORT`)、機能の報告は`SET_REPORT`と`GET_REPORT`で送受する。Chromeはキーボードとマウスの報告をページに渡さないが、ここに出る機器はシステムが入力として読んでいないので、そうしない |
| XR(`navigator.xr`) | ヘッドセットはなく、没入型のセッションは扱えないと答える(`isSessionSupported`はfalse、`requestSession`は失敗)。インラインのセッションはページの中で動く |
| 画面の共有(`getDisplayMedia()`) | 求められるたびに、パネルで共有するものを選ばせる(答えは覚えない)。選べるものと音は14.4.18にある。取込みは`port/blink/screen_tessronos.cc`の取込みのホストで、カメラの取込みのホストが、その機器のセッションのクライアントを渡す(`camera_tessronos.cc`の`Hosts`)。セッションごとに取込みを1つ持つ。ホストのスレッドは、求められたフレームレート(30まで)で、ウインドウマネージャが最後に組み立てた画面をポインタごと写し(SVC`wm_grab_screen`、10.4.3、`port/media/br_screen.cc`)、libyuvで`I420`にして、求められた大きさより大きければ縦横の比を保って縮める。4つの共有メモリのバッファの扱いはカメラと同じである。共有している間、ブラウザのウインドウのタイトルの前に`[画面を共有中]`と出す(`br_capture_text`)。最後のトラックが止まると取込みを止め、印も消える |
| 画面(`getScreenDetails()`) | 許可をパネルで尋ねてから、システムの1つの画面を答える |
| マシンのフォント(`queryLocalFonts()`) | 許可をパネルで尋ねてから、ブラウザが描くのに使うフォント(Skiaのフォント管理が持つもの)の一覧を答える |
| リモート再生(メディア要素の`remote`) | 再生先の機器はなく、`watchAvailability`は`NotSupportedError`で拒否する |
| ゲームパッド(`navigator.getGamepads()`) | プログラムの機器のHIDのインタフェースのうち、コレクションがジョイスティックかゲームパッド(汎用デスクトップの4か5)のものを、挿されたときに開き(1秒ごとに機器を見る)、1つずつのスレッドで入力のレポートを読む(`port/blink/usb_hid_tessronos.cc`)。レポートの記述子はChromiumのパーサで読み、ボタン(ボタンのページ)、軸(汎用デスクトップのXからWheelまでを-1から1に)、ハットスイッチ(軸2つ)を用途の順に並べる。Chromiumがベンダとプロダクトに標準の配置を持つものはそれを当て、`mapping`を`standard`にする。名前はChromeのLinux版と同じく「製品名 (STANDARD GAMEPAD Vendor: vvvv Product: pppp)」。Chromeと同じく、ボタンが一度押されるまではページに見せない。ページのウインドウができるたびにゲームパッドの補助(`NavigatorGamepad`)を作るので、`getGamepads`の前に付けた`gamepadconnected`のリスナーにも届く。振動は扱わないと答える |
| 振動(`navigator.vibrate`) | 振動する機器はないので、振動はすぐ終わったと答える(`vibrate`はtrueを返す。Chromeと同じく、利用者がページを操作する前はfalse) |
| Compute Pressure(`PressureObserver`) | プロセッサがタスクを走らせた時間の割合(カーネルの`ts_ref_cpu`、`port/net/br_cpu.cc`)を1秒ごとに測り、0.6未満を`nominal`、0.75未満を`fair`、0.9未満を`serious`、それ以上を`critical`と伝える |
| スポイト(`EyeDropper`) | ページのウインドウのメッセージの欄に点を求める案内を出し、利用者が次にページの上で押した点の色を答える(`br_pick_color`)。Escでやめる |
| カメラとマイク(`navigator.mediaDevices`) | マイクは、サウンドデバイスが録音できるとき(`OB_SND_INFO`の48kHzの録音)に、入力の機器「マイク」として一覧に出る。`getUserMedia`は利用者にパネルで尋ね(答えはオリジンごとにブラウザが動く間覚える)、マイクの機器を返す。ストリームの音は、Blinkのローカルの音のソース(`ProcessedLocalAudioSource`)がプラットフォームから得る録音のソース(`br_audio_capturer`、`port/media/br_audio_sink.cc`)が取る。録音のソースは、サウンドデバイスの再生と別の単位(`snda0`など。単位ごとに経路が別になる)を48kHz、16bit、ステレオの録音にし、専用のスレッドで読んで、頼まれた大きさの区切りで渡す(`br_mic_open`、`br_mic_read`、`port/media/br_snd.cc`)。カメラは、カメラデバイスのユニットの実身(6.6)ごとに入力の機器として一覧に出て(名前は製品名)、モードを`I420`の形式の一覧として見せる。`getUserMedia`で映像を求めると、カメラ、マイク、両方のうち、まだ答えのないものを利用者に尋ね、カメラの機器を返す。カメラとマイクの答えはオリジンとそれぞれについてブラウザが動く間覚え、どちらかが断られていれば尋ねずに断る。この答えは`navigator.permissions.query({name: 'camera'})`と`'microphone'`の答えにもなり(尋ねる前は`prompt`、許すと`granted`、断ると`denied`)、ページの状態のオブジェクトの`change`のイベントにも伝える(会議のサイトは、この答えでカメラとマイクを始めるか、許可を求める案内を出すかを決める)。ほかの許可の答えが変わったときも、その状態のオブジェクトに伝える。映像は、Blinkの映像の取込み(`VideoCaptureImpl`、プラットフォームの`GetVideoCaptureImplManager`)が話す取込みのホスト(`VideoCaptureHost`、`port/blink/camera_tessronos.cc`)が流す。ホストは求められた大きさに一番近いモード(同じならMJPEGより無圧縮)でカメラを流し(`br_cam_open`、`port/media/br_cam.cc`)、読むスレッドが新しいフレームを8ミリ秒ごとに見て`I420`にする。YUY2、UYVY、NV12はlibyuvで、MJPEGはSkiaのJPEGのデコーダでデコードしてからlibyuvで変える。サービスのスレッドで、それを4つの共有メモリのバッファの空いたものにコピーし、止めていない相手に通知する。バッファは、渡した相手がみな返すと次に使う。ホストの相手はみな1つの流れを分け合い、最後の相手が止めるとカメラを閉じる。カメラが去ると、トラックは終わる。カメラの機器のIDは、ページのオリジンとユニットの実身のUUID(v7)をつないだもののSHA-256を64桁の小文字の16進にしたものである。既定の機器以外のIDはこの形でなければメッセージの検査で捨てられる。同じカメラは同じオリジンに対して起動をまたいで同じIDになり、オリジンが違えば違うIDになるので、サイトをまたいで利用者を見分ける手がかりにならない。機器の型の直列化(`media/capture`の`mojom`と型)は、Blinkの全体から除いている`media/capture`のうち、それだけを組み込む |
| 音声の合成(`speechSynthesis`)と認識(`SpeechRecognition`) | ブラウザに入れた合成器と認識器で動く(14.4.15)。マイクがないマシンでは、認識はすぐ`audio-capture`のエラーで終わる |

`MediaRecorder`と、canvasとメディア要素の`captureStream()`は、Blinkのモジュールをそのまま使う。機器がなくても、canvasや`AudioContext`から作ったストリームを、メディアライブラリのソフトウェアのエンコーダ(VP8、Opus)でWebMに録る。

### 14.4.9 Web Serial

`navigator.serial`(`port/blink/serial_tessronos.cc`)は、システムのシリアルポートをSVCのクラス11(6.2、`port/net/br_serial.cc`)で使う。

- `requestPort()`は、ポートの一覧(デバイスの名前と説明。コンソールのポートには「(コンソール)」を付ける)をパネル4で選ばせる。選ばれたポートは、ブラウザが動く間、そのオリジンのページが開け、`getPorts()`に現れる。`forget()`で外す。システムのポートはUSBの機器ではないので、ベンダやプロダクトのIDで絞る`filters`には何も当たらない。
- `open()`は、ポートを開いて求められた速度にする。ページが`readable`を読む間、ポートを専用のスレッドで読み(1回の待ちは0.1秒)、届いたバイトをサービスのスレッドでページのデータパイプに入れる。パイプに入りきらないものは64KBまで持ち、それを超えると読むのを待つ。ページが`writable`に書いたものはサービスのスレッドでパイプから取り、専用のスレッドがポートに送る。遅いポートでも、サービスのスレッドとページは待たない。
- システムのポートにはデータの線しかないので、制御線の設定は受け付けるだけで、読むとすべて偽を返す。データの形式は8ビット、パリティなし、ストップビット1である。

### 14.4.10 通知

Web通知(`new Notification`と、Service Workerの`registration.showNotification`)は、デスクトップの通知(11.28)で見せる。ブラウザの側の`NotificationService`(`port/blink/services_tessronos.cc`)が通知に番号を付けて`DT_RQ_NOTIFY`を書き(`port/net/br_notify.cc`)、デスクトップの答えを専用のスレッドが一つの返事のチャネルで読んで、番号で通知に渡す。

- 表示の許可(`Notification.requestPermission`)は、利用者にパネルで尋ねる。出どころにはページのオリジンを出す。
- ウインドウの通知は、表示するとリスナーに`show`を伝え、〈開く〉で`click`、〈閉じる〉で`close`を伝える。`notification.close()`は通知を引っ込めて`close`を伝える。同じタグの通知は前のものと入れ替わる。
- Service Workerの通知は、〈開く〉で`notificationclick`(`action`は空)、通知のボタンで`notificationclick`(`action`はそのボタンの名前)、〈閉じる〉で`notificationclose`をワーカーに送る(`br_sw_dispatch`)。`requireInteraction`の通知は時間で消えない。時間で消えた通知も、ブラウザの通知の一覧に残り、`getNotifications`に答える。

### 14.4.11 プッシュ

Web Push(Service Workerの`registration.pushManager`)は、プッシュサービスにWebSocketで常につなぎ、届いたメッセージをワーカーの`push`イベントにする(`port/net/push_tessronos.cc`、`port/blink/services_tessronos.cc`の`PushMessagingImpl`)。

- プッシュサービスは既定でMozillaのautopush(`wss://push.services.mozilla.com/`)である。autopushは、送り手の鍵(VAPID)を持つどのサイトにも購読を与え、ブラウザはサービスの側にアカウントを要しない。起動の行`push=`で別のサービス(autopushと同じ手順を話すもの)を指定できる。
- 接続はネットワークスタックのスレッドの`WebSocketChannel`で張り、副プロトコルは`push-notification`である。`hello`で利用者のエージェントの識別子(`uaid`)を受け、以後はそれを名乗る。切れると間を延ばしながらつなぎ直し、25分ごとに`ping`を送る。
- `subscribe()`は、`userVisibleOnly`がtrueで送り手の鍵(`applicationServerKey`)があるものだけを受ける。通知の許可がまだなければ利用者にパネルで尋ねる。スコープごとにチャネルの番号を作って`register`を送り、サービスが答えた`pushEndpoint`を購読の宛先にする。購読ごとにP-256の鍵と16バイトの認証の秘密を作り、ページに`p256dh`と`auth`として渡す。サービスが20秒答えなければ`AbortError`で終わる。
- `getSubscription()`はそのスコープの購読を答え、`unsubscribe()`は`unregister`を送って購読を消す。購読(宛先、鍵、秘密、`uaid`)は「ブラウザの保存域」のレコード3に入れ、ブラウザを終えても残る。購読があれば、ブラウザは起動するとサービスにつなぎ、前の`uaid`を名乗る。ブラウザが動いていない間に送られたメッセージは、サービスが持っていて、そのときに届く。
- サービスの`notification`は、まず`ack`で受け取りを返し、本文をRFC 8291の`aes128gcm`として復号する(ECDH P-256、HKDF-SHA-256、AES-128-GCM。BoringSSLを使う)。復号できたものを、そのスコープのワーカーに`push`イベント(`data`は本文)として送る(`br_sw_dispatch_scope`)。ワーカーが動いていなければ、残してある登録(14.4.6)から起動する。そのスコープの登録がなければ、メッセージは捨てる(受け取りは返してある)。
- 本文のない`notification`は、データのない`push`イベントにする。古い符号化の`aesgcm`(RFC 8291の前の草案のもの。塩と送り手の鍵は`Encryption`と`Crypto-Key`のヘッダで来る)も復号する。
- 試験の行`pushkey=`は、購読の秘密鍵と認証の秘密を与える。試験は、それに向けて暗号化したメッセージを送るサービスを使う。

### 14.4.12 実行時の機能、タイムゾーン、画面、ユーザエージェント

Blinkの実行時の機能(`RuntimeEnabledFeatures`)のうち、`base::Feature`に従うものは、Chromeでは描画のプロセスが起動時に`WebRuntimeFeatures::UpdateStatusFromBaseFeatures`で`base::Feature`の状態を移す。このブラウザも、Blinkの初期化の直後に同じ呼出しをする。以前は呼んでいなかったので、Chromeで既定で有効な機能のうち、Blinkの一覧で安定と書かれていないものが無効のままだった。YouTubeの再生の検証のスクリプト(BotGuard)は、その差のために例外で止まり、検証の結果(`GenerateIT`への要求)にエラーの文字列を入れていた。呼ぶようにすると、検証は最後まで動き、サーバから検証のトークン(integrity token)が返る。

スクリプトの日時と`Intl`は、システムのタイムゾーン(ユーザ環境設定で選ぶ地域、9.5)で動く。ブラウザは起動時に`dt_getsyszone`で地域のIANAの名前を読み、ICUの既定のタイムゾーンにする(`br_zone_name`、`follow_zone`)。名前のないときは差から`GMT+09:00`の形の名前を作る。3秒ごとに見直し、変わっていればICUの既定を替えて、Chromeがマシンのタイムゾーンが変わったときにするのと同じく、V8に日時の設定が変わったことを通知する(`DateTimeConfigurationChangeNotification`)。以後に作る日時は新しいタイムゾーンになる。夏時間とその地域の過去の規則はICUのデータが扱う。試験は`test_blink_zone`である。

`screen.width`、`screen.height`、`screen.colorDepth`、画面の大きさのメディアクエリは、`ChromeClient`の`GetScreenInfos`が答える。画面の大きさはウインドウマネージャに尋ね(`wm_screen`、SVC 9-36)、1ピクセルを1 CSSピクセル、色の深さを24ビットとする。以前は基底の`ChromeClient`の空の値のままで、幅と高さと色の深さが0だった。

ユーザエージェントの文字列は、Chrome 153のLinuxのデスクトップの縮小したユーザエージェントと同じ`Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36`である。Chromeは縮小したユーザエージェントでは、Linuxならプロセッサによらず`X11; Linux x86_64`と言い、`navigator.platform`も`Linux x86_64`を返す。以前は`X11; TessronOS aarch64`と言っていたが、`navigator.platform`(`Linux x86_64`)、クライアントヒントのプラットフォームと食い違い、ページから見て一貫しない姿だった。同じことをクライアントヒントでも示す(`br_user_agent_metadata`)。`navigator.userAgentData`のブランドは`Chromium` 153と、Chromiumがどのバージョンでも足す意味のないブランド(バージョン番号から選ぶ`Not_A Brand` 8)で、この順に並ぶ(並びもバージョン番号から決まり、153ではChromiumが先)。詳しいバージョン(`fullVersionList`、`uaFullVersion`、`Sec-CH-UA-Full-Version-List`)は、コンパイルしたソースのバージョン`153.0.8010.36`である。プラットフォームは`Linux`、アーキテクチャは`arm`の64ビットである(プロセッサはクライアントヒントだけが言う)。フレーム、専用ワーカー、共有ワーカー、Service Workerが同じ値を持つ。安全なオリジンへの要求には、ネットワークの取得が`Sec-CH-UA`、`Sec-CH-UA-Mobile`、`Sec-CH-UA-Platform`のヘッダを付ける(ページが自分で付けたものは替えない)。`navigator.platform`は、ChromeがLinuxのどのアーキテクチャでも返す固定の値`Linux x86_64`である(Blinkのユーザエージェントの縮小の値)。

ChromeのLinux版のウインドウにあるインタフェースのうち、このブラウザになかったのは`BeforeInstallPromptEvent`だけだった(ウインドウのすべてのインタフェースの名前とメンバをページから書き出し、ChromiumのIDLと照らし合わせて確かめた。Android、ChromeOS、隔離されたWebアプリだけのもの、端末上の言語モデルを要するAIのAPIを除く)。そのモジュールをコンパイルして加えた。Webアプリのインストールはないので、`beforeinstallprompt`のイベントは起きない。

ページから見えるブラウザの姿は、Chrome 153のLinuxのデスクトップと次の点をそろえている(試験`test_blink_face`が値を書き出す)。

| 項目 | 値 |
| --- | --- |
| `window.chrome` | `loadTimes()`、`csi()`、`app`(`isInstalled`は偽、`getDetails`、`getIsInstalled`、`installState`、`runningState`、`InstallState`、`RunningState`)。関数はChromeと同じくネイティブの関数で、`loadTimes()`と`csi()`の答えはネイティブのデータプロパティを持つ。値はウインドウの性能の計時と応答から作る(`install_chrome`) |
| `navigator.languages`、`Intl`の既定のロケール、`Date`の文字列 | `ja`、`en-US`、`en`。ICUの既定のロケールを`ja`にする(以前は`en-US`で、`navigator.language`と食い違っていた) |
| `(pointer: fine)`、`(hover: hover)` | 真(マウスのあるマシン)。以前は偽で、ページがタッチの画面と見ていた |
| `outerWidth`、`outerHeight`、`screenX`、`screenY` | ブラウザのウインドウの外寸と位置(`ChromeClient::RootWindowRect`)。以前は0だった |
| 文書の応答 | 状態の番号、ヘッダの行、プロトコルを文書に渡す(`BrEngine::response`、`apply_response`)。`Content-Security-Policy`と`Content-Security-Policy-Report-Only`は、`Platform::AppendContentSecurityPolicy`で文書のCSPに入れる。ネットワークから(または`data:`のURLで)メインフレームに確定する文書には、新しい既定のポリシーのコンテナを渡す(`main_policies`)。渡さないとBlinkはウインドウの前の文書のコンテナを引き継ぎ、新しい文書がそれまでの文書のCSPにも縛られる(2026年10月15日まで、Googleのログインの画面から移った`workspace.google.com`のページが、ログインの画面のnonce付きのCSPで自分のスクリプトを止められていた)。`about:blank`の文書は前の文書のものを引き継ぐ。`Permissions-Policy`、`Refresh`、`Link`などもBlinkが応答から読む。`performance.getEntriesByType('navigation')`の`nextHopProtocol`、`responseStatus`、`transferSize`と`chrome.loadTimes()`の`connectionInfo`はこれで実際の値になる。iframeの文書も同じ |

`navigator.plugins`と`navigator.mimeTypes`は、PDFを表示するChromeと同じく5つ(`PDF Viewer`、`Chrome PDF Viewer`、`Chromium PDF Viewer`、`Microsoft Edge PDF Viewer`、`WebKit built-in PDF`)と2つ(`application/pdf`、`text/pdf`)で、`pdfViewerEnabled`は真である(14.6.6)。WebGLの`UNMASKED_RENDERER_WEBGL`は、`make GL=1`ではANGLEの文字列、そうでなければ`TessronOS software GL`である。

試験のための起動の行に次のものがある。`script=js:<スクリプト>`は、ページを表示した後にそのスクリプトをメインワールドで実行する(`script=<URL>`は取得したスクリプトを実行する)。`features=<名前>,-<名前>`は、Blinkの実行時の機能を入れる(`-`を付けると切る。Chromeが試験中として切っているものを試すため)。`netlog=<語>,<語>`は、URLにその語を含む要求について、要求のヘッダと本文、応答の本文(文字は16KBまで、そのほかは2KBまでを16進で)をコンソールに出す。`quic=off`は、HTTP/3(QUIC)を使わず、すべての要求をTCPで送る(HTTP/3の転送の速さと比べるため)。`trace=exceptions`は、ブラウザのAPIがページのスクリプトに投げた例外を、インタフェース、メンバ、例外の文の組ごとに1度コンソールに出す(V8の`SetExceptionPropagationCallback`で、Blinkが文を整えた後に見る)。ページの実身の本文に`trace=exceptions`の段落を書き足しておくと、そのページの実身から開いたブラウザは、これを起動の行として受け取る(コンソールに`browser: trace=exceptions, from the page object`と出る)。実機で利用者がサイトの振る舞いを調べられるようにするためである。ほかの起動の行はページの実身からは受け取らない(`script=`はページで任意のスクリプトを動かし、`netlog=`は送ったパスワードもコンソールに出すため)。

### 14.4.12.1 クロスオリジンの分離

最上位の文書の応答が`Cross-Origin-Opener-Policy: same-origin`と、分離を許す`Cross-Origin-Embedder-Policy`(`require-corp`か`credentialless`)を持ち、安全なコンテキストであるとき、Chromeはその閲覧コンテキストのグループを分離し、ページは`crossOriginIsolated`が真になって`SharedArrayBuffer`と共有の`WebAssembly.Memory`を使える(ZoomのWeb版は、WebAssemblyのスレッドのためにこれを使う)。以前のこのブラウザは分離をせず、`crossOriginIsolated`はいつも偽で、`SharedArrayBuffer`もなかった。

- 文書を確定するとき(`set_agent_cluster`、`br_blink.cc`)、応答のヘッダをChromiumのパーサ(`network::ParseCrossOriginOpenerPolicy`、`ParseCrossOriginEmbedderPolicy`)で読む。最上位の文書が上の条件を満たせば、そのページは最上位の文書のオリジンで分離される。ページのすべての文書(iframeも)は、エージェントクラスタのキーにその分離(`WebCrossOriginIsolationKey`、オリジンは最上位の文書のもの、モードは`kConcrete`)を持つ。Chromiumでも、分離の鍵は最上位の文書のオリジンで、ページのすべての文書に当たる。Blinkはこのキーから`crossOriginIsolated`を決め、iframeは許可のポリシー`cross-origin-isolated`が許すときだけそれを得る。専用ワーカーは作ったコンテキストの分離を受け継ぐ(Blinkの`DedicatedWorker`)。分離したページでは`Origin-Agent-Cluster: ?0`は効かない。
- 文書の`Cross-Origin-Embedder-Policy`は文書のポリシー(`policy_container`)に入れる。フレームのローダはそれを持ち、部品の応答をChromiumの`network::CrossOriginResourcePolicy::IsBlockedByHeaderValue`で調べる(`br_loader.cc`の`corp_allows`)。別のオリジン(サイト)へのno-corsの要求の応答が`Cross-Origin-Resource-Policy`で`same-origin`(`same-site`)と言えば、COEPによらず止める。COEPが`require-corp`なら、何も言わない応答も止める。止めた要求は`net::ERR_BLOCKED_BY_RESPONSE`で失敗し、コンソールに出す。COEPが`credentialless`のときは、別のオリジンへのno-corsの要求を資格情報なしで送り、CORPのない応答も受け取る。以前はCORPを調べていなかった。
- iframeの文書は、親の文書のCOEPが`require-corp`か`credentialless`のとき、自分も分離を許すCOEPを持たなければならず、入れ子のナビゲーションのCORPの検査(`network::CrossOriginResourcePolicy::IsNavigationBlocked`)も通らなければならない(`coep_blocks_frame`)。通らなければ、Chromeと同じく、そのフレームには文書の代わりにブラウザのエラーの文書(`chrome-error://chromewebdata/`、`net::ERR_BLOCKED_BY_RESPONSE`、オリジンは不透明)を確定し、コンソールに出す。親はその中を見られない(`contentDocument`は`null`)。
- 分離していないページでは、`SharedArrayBuffer`はChromeと同じくない(V8の`--enable-sharedarraybuffer-per-context`で、コンテキストごとにBlinkに尋ねる)。

### 14.4.13 組み込みのAI

Chrome 153がLinuxで出す組み込みのAIのAPI(`LanguageDetector`、`Translator`、`Summarizer`、`LanguageModel`)を出す(`port/blink/ai_tessronos.cc`)。`Writer`、`Rewriter`、`Proofreader`はChromeでもオリジントライアルなので出さない。

- `LanguageDetector`は本当に答える。Chromeはブラウザがダウンロードした言語の判定のモデル(TFLite)をファイルで渡すが、このブラウザはChromiumのソースツリーのCLD3(ニューラルネットワークの重みをソースに持つ)で判定する。言語の判定のドライバ(`ContentLanguageDetectionDriver`)はモデルがある(`kReadily`)と答え、Blinkの判定のモデルにはパッチ`language_detection_model.patch`で判定の関数を渡す。CLD3が見つけた言語(3つまで)に、文のうちその言語の部分の割合と確からしさを掛けたものを確信度とし、残りを判定できないもの(`und`)にする。CLD3の古い言語のコード(`iw`、`jw`、`tl`)は今のもの(`he`、`jv`、`fil`)にする。
- 翻訳の言語パック、要約や言語モデルのモデル(Gemini Nano)はマシンになく、取得もできないので、Chromeがモデルを持てないマシンで答えるのと同じく、`availability()`は`unavailable`、`create()`は断る。`LanguageModel.params()`はnullである。

### 14.4.14 隔離されたWebアプリだけのAPI

Chromeは、Direct Sockets、ウインドウの操作(`window.maximize`、`minimize`、`restore`、`setResizable`)、Web Printingなどを、隔離されたWebアプリ(IWA)にだけ出し、普通のページには出さない。このブラウザにはIWAの仕組みがないので、利用者が許したオリジンをIWAの代わりにする。ウインドウのメニューの〈このサイトを隔離されたWebアプリとして扱う〉を選ぶと、そのページのオリジンが許され(印が付く)、ページを読み直す。もう一度選ぶと外れる。許したオリジンは許可の答えとともに「ブラウザの保存域」のレコード7に残る。試験は起動の行`isolated=<オリジン>`でその実行のあいだだけ許す。

- 許したオリジンのウインドウは、Blinkにとって隔離されたコンテキスト(`IsIsolatedContext`)であり、クロスオリジン分離(`crossOriginIsolated`)でもある(パッチ`isolated_context.patch`)。IWAはいつもクロスオリジン分離されているからである。
- Direct Sockets(`TCPSocket`、`UDPSocket`、`TCPServerSocket`)は、システムのソケットで動く(`port/blink/direct_sockets_tessronos.cc`)。TCPは名前を引いてつなぎ、受けたバイトをページの読むデータパイプへ、ページが書いたバイトをソケットへ、それぞれのスレッドで運ぶ。TCPのサーバは受け付けるたびに同じ形の接続を渡す。UDPは相手につないだものと、自分のアドレスにバインドして誰にでも送るものがあり、ページが求める数だけ受けたデータグラムを渡す。マルチキャストへの参加とTLSへの切り替えは扱わない。
- ウインドウの操作は、Chromeの試験中の機能(`DesktopPWAsAdditionalWindowingControls`)を、許したオリジンのウインドウでだけ入れる。ページはアプリの範囲(`Settings::SetWebAppScope`、そのオリジン)にあるものとし、ウインドウの管理の許可をパネルで尋ねてから行う。最大化は外枠を画面の大きさにし、元に戻すと前の位置に戻して前面に出す。ウインドウシステムには最小化の状態がないので、最小化はほかのすべてのウインドウの後ろに回す。`setResizable(false)`の後に利用者が大きさを変えると、元の大きさに戻す。
- Web Printing(`window.printing`)は、デスクトップの印刷を1台のプリンタ「TessronOSの印刷」として見せる(`port/blink/printing_tessronos.cc`)。属性はA4、1部、カラー、縦と横、96dpiである。ChromeがCUPSを持つChromeOSでだけ使う印刷のインタフェースは、パッチ`web_printing_manager.patch`でこのブラウザでも使う。印刷の文書(PDF)は、PDFiumでページごとに用紙の印刷範囲に描き、ページの印刷と同じくデスクトップに渡す(14.7.5)。仕事は処理中、完了(印刷されなかったときは中止)と通知する。
- 裏付けのないもの(Smart Card、Web NFC、Contacts、Digital Goods、SubApps、Web Install)は、ChromeのLinux版と同じく出さない。スマートカードのリーダのドライバ、NFC、アドレス帳、アプリの導入の仕組みがないからである。

### 14.4.15 音声の合成と認識

Web Speech API(`speechSynthesis`、`SpeechRecognition`)は、ブラウザに入れた合成器と認識器で動く(`port/blink/speech_tessronos.cc`、`port/speech/speech_glue.c`)。合成器と認識器は`tools/browser/speech.sh`がコンパイルしたもの(`libspeech.a`)で、使う辞書、音声、モデル(約57MB)は、プログラム「ブラウザ」の実身のレコード2に入れる(`speech.sh`が1つのレコードにまとめ、ビルドがプログラムの横に置く)。プログラムは起動のときに丸ごとメモリに読まれるので、実行ファイルに入れると、ウインドウを別のブラウザで開くたびに57MBを余計に読み、時間とメモリを使う。レコード2は話すか聞くときに初めて読む(`port/speech/br_speech_rec.cc`)。ネットワークのサービスは使わない。

| 用途 | 使うもの |
| --- | --- |
| 日本語の合成 | Open JTalk 1.11(MeCabと辞書naist-jdicで形態素に分け、読みとアクセントを付け、HTSのラベルにする)とhts_engine API 1.10、音声`nitech_jp_atr503_m001`(48kHz) |
| 英語の合成 | Flite 2.2、音声`cmu_us_slt`(16kHz) |
| 認識 | whisper.cpp 1.7.6、Whisperのtinyのモデル(5bitに量子化したもの、約31MB)。CPU(NEON)の4つのスレッドで認識する |

- 声は2つで、`getVoices()`は「Open JTalk 日本語」(`ja-JP`、既定)と「Flite English」(`en-US`)を答える。どちらも`localService`である。発話は、名指しされた声、なければ言語(`ja`で始まるか)、言語もなければ本文に仮名か漢字があるかで声を選ぶ。`rate`は話す速さ、`pitch`は高さ(Open JTalkでは半音の数、Fliteでは基本周波数の平均)、`volume`は大きさになる。
- Blinkは発話を1つずつ渡す。合成は1つのスレッドで順に行い、作った音を48kHz、ステレオにして、メディア要素と同じ音の輪(14.6.2)に空いた分ずつ書く。書き始めるときに`start`を、輪に書いた音が鳴り終わったときに`end`をページに通知する。`pause()`は書くのを止め、`resume()`で続ける。`cancel()`は話している発話を止めて`interrupted`のエラーで終わらせる。まだ渡されていない発話はBlinkが捨てる。
- 日本語の辞書のファイル(`sys.dic`など)は圧縮して持ち、初めて日本語を話すときにレコードから読んで展開する(`br_speech_file`)。展開した辞書(約107MB)はブラウザが終わるまで残す。音声とモデルは圧縮せずにそのまま読む。
- 認識は、マイクがなければすぐ`audio-capture`のエラーで終わる。あればマイクの使用をパネルで尋ね(答えはオリジンごとにブラウザが動く間覚える)、断られれば`not-allowed`で終わる。許されると録音を始め(`br_mic_open`、14.4.8)、48kHzのステレオを16kHzのモノラルにして溜める。音の大きさ(二乗の平均)が閾値を越えると`soundstart`を通知し、音が出た後に1秒の無音が続くか、10秒たつか、ページが`stop()`を呼ぶと録音を終える。溜めた音をwhisper.cppに要求した言語(`lang`の`-`の前、なければ自動)で認識させ、聞き取った文を確定した結果(`isFinal`が真)として1つ渡す。エンコーダは30秒のウインドウの全部でなく録音の長さに少し足した分だけを見る(`audio_ctx`)ので、数秒の音は短い時間で認識できる。`abort()`は`aborted`、音がなければ`no-speech`、何も聞き取れなければ`no-match`で終わる。途中の結果(`interimResults`)と、続けての認識(`continuous`)の区切りは扱わず、どちらも1つの確定した結果になる。
- マシンの上での認識のAPI(`SpeechRecognition.available()`、`install()`)は、Chromeの試験中の機能なので出さない。
- MeCabは設定のファイルを読まない(Open JTalkが値をコードで与える)が、そのために開くだけのファイルのストリームを作る。ツールチェーンのC++のライブラリはスレッドなしで作られていて、そのファイルのストリームの大きさが、スレッドありとしたヘッダ(`include/tscxx`)で並べたものと合わず、作ると落ちる。そこでその1行をコピーから除く(`speech.sh`)。文字列のストリームは大きさが合うのでそのまま使える。

### 14.4.16 WebNN

WebNN(`navigator.ml`、`MLContext`、`MLGraphBuilder`、`MLGraph`、`MLTensor`)は、Chrome 153でも試験中の機能(`MachineLearningNeuralNetwork`、基の機能`WebMachineLearningNeuralNetwork`は既定で切)なので、Chromeと同じく既定では出さず、起動の行`features=MachineLearningNeuralNetwork`で出す。

- ページが作ったグラフの検査、定数の並べ替え、テンソルの管理は、ChromiumのWebNNのサービス(`services/webnn`のコンテキスト、グラフの組立て、テンソル)が行う。ChromeはGPUのプロセスがコンテキストを作れないときにこのサービスをページのプロセスで動かし(`WebNNContextProviderInRenderer`)、LiteRTで計算する。このブラウザもサービスを自分のプロセスで動かし、計算はLiteRTの代わりに移植層(`port/blink/webnn_tessronos.cc`)が行う。そのためのインタフェースをパッチ`webnn_context_provider.patch`で足す。
- 計算は、ページが組み立てた順に演算を1つずつ、テンソル全体について倍精度の数で行い、結果をその被演算子の型(float32、float16、各整数)に丸めてから次の演算に渡す。WebNNの仕様の演算のすべて(要素ごとの演算と比較と論理演算、型の変換、`matmul`、`gemm`、`conv2d`と`convTranspose2d`(グループと拡張を含む)、プーリング、正規化、`softmax`、集約、`gather`と`scatter`の各種、`pad`、`slice`、`split`、`concat`、`transpose`、`tile`、`resample2d`、`triangular`、`where`、量子化と逆量子化、`gru`、`gruCell`、`lstm`、`lstmCell`など)を扱う。
- コンテキストは、ページが求める機器(`cpu`、`gpu`、`npu`)によらずプロセッサで計算する。被演算子の並びはNCHW(`conv2d`の重みはOIHW、`convTranspose2d`はIOHW)で、型は4bitの整数を除くすべて、次元は8までを扱う。テンソルはブラウザのメモリにあり、WebGPUのテンソルとの共有は扱わない。

### 14.4.17 本物のChromeとの照合

ページから見える姿とネットワークへの要求の形を、本物のChrome 153と同じページで比べてそろえた。基準は、WSLの中でXvfbの画面に出したGoogle Chrome 153.0.8010.36(Linux版の配布物)と、Chrome for Testingの同じバージョンである。Chrome for Testingは試験用のビルドで、XSLT、Topics、Protected Audience、Attribution Reportingを切り、WebMCPを入れているので、比べる基準はGoogle Chromeにした。

比べるページは、ウインドウと各インタフェースの静的な名前とプロトタイプの名前(`Reflect.ownKeys`をたどり、各プロパティの記述子、関数の`length`、`name`、`Function.prototype.toString`の形)、`navigator`、`screen`、`window`、`document`の値、`userAgentData.getHighEntropyValues`、メディアクエリ、`CSS.supports`、計算されたスタイル、`Intl`と`Date`、数学関数の値、時計の分解能と進み、エラーのスタックと文、権限の問合せ、WebGLの値と拡張、Web Audioの出力、canvasの描画、フォントの幅、メディアの型、WebCodecs、WebAuthnの能力、Wasmの機能、ワーカー(専用と共有)とiframeの中の同じ値、CORSのふるまい、`httpbin.org`が返す要求のヘッダを集め、JSONでサーバに送る。差分のツールがそれを「Chromeにだけある」「こちらにだけある」「値が違う」「並びが違う」に分け、画面の大きさ、プロセッサの数、GPU、時計の細かい値のような、本物のLinuxのマシンどうしでも違うものは別に分ける。

違っていて直したものは次のとおりである。

| 項目 | 以前 | 直した後(Chromeと同じ) |
| --- | --- | --- |
| CORS | 移植の取得の層(`br_loader.cc`)が検査をせず、ほかのオリジンへの`fetch`とXMLHttpRequestが読め、画像でcanvasが汚れず、ほかのオリジンのスクリプトの例外の文が見えた | 要求のモードとオリジンから、応答をCORSで検査する(`network::cors::CheckAccess`)。安全でないメソッドとヘッダには事前の要求(`OPTIONS`、`Access-Control-Max-Age`の間は覚える)を送る。`no-cors`のほかのオリジンへの応答は不透明(`opaque`)、CORSの応答は`cors`の型にし、Blinkが公開するヘッダを絞る。`same-origin`のモードのほかのオリジンへの要求は送らない |
| 要求の`Origin`、`Sec-Fetch-Site`、`Sec-Fetch-Mode`、`Sec-Fetch-Dest`、`Sec-Fetch-Storage-Access` | 付けていなかった | ブラウザのネットワークのサービスと同じ規則で付ける(CORSの要求とGET、HEAD以外の`Origin`、安全な宛先への`Sec-Fetch-*`) |
| 資格情報 | 常に送った | `credentials`が`omit`、または`same-origin`でほかのオリジンのときは、Cookieと認証を送らない |
| ウインドウの移動のCookie | 移動の要求のCookieの基準のサイト(site for cookies)が最初のアドレスのままで、ほかのサイトへのリダイレクトの先はクロスサイトの要求として扱われた。`SameSite`のない(既定でLax)Cookieを、リダイレクトの応答が付けても保存せず、送りもしなかった。`www.gmail.com`から`accounts.google.com`へ転送されたログインの画面は、最初の応答が付けた`__Host-GAPS`を持たず、画面の要求(`batchexecute`、`browserinfo`)がHTTP 400で断られた(2026年10月15日まで) | ブラウザの移動と同じく、基準のサイトをリダイレクトごとにその先のアドレスにし、メインフレームの移動としてCookieを扱う。移動を求めたページのオリジンを要求の始まり(initiator)にし、利用者が求めた移動は始まりなしにする(`br_navigation_headers`の行`X-TessronOS-Navigation`、`X-TessronOS-Initiator`をネットワークスタックが取り去って使う) |
| 優先度 | 全部が既定の優先度で、HTTP/2とHTTP/3の`priority`が`i`だけだった | Blinkが決めた優先度と`incremental`をネットワークの層に渡し、`u=1, i`などになる |
| `Accept-Encoding` | `gzip, deflate` | `gzip, deflate, br, zstd`(BrotliとZstandardの復号を入れた) |
| 高エントロピーのクライアントヒント | 送らなかった | ウインドウの文書の応答の`Accept-CH`をオリジンごとに覚え、文書をその指示で確定する(`enabled_client_hints`)。Blinkが部品の要求に`Sec-CH-UA-Arch`、`-Full-Version-List`、`Sec-CH-DPR`、`Sec-CH-Viewport-Width`、`Device-Memory`などを付ける。ウインドウの移動の要求にもChromeと同じ`Accept`、`Upgrade-Insecure-Requests`、`Sec-Fetch-*`、覚えたヒント、最高の優先度を付ける(`br_navigation_headers`) |
| ブランドの並びと詳しいバージョン | `Not_A Brand`が先、詳しいバージョンは`153.0.0.0` | `Chromium`が先、`153.0.8010.36` |
| `SharedArrayBuffer` | どのコンテキストにもあった | クロスオリジン分離のコンテキストだけ(V8の`--enable-sharedarraybuffer-per-context`) |
| タッチのイベントのハンドラ(`ontouchstart`など) | あった | タッチの画面のないマシンなので無い |
| Web Bluetooth | あった | ChromeのLinux版と同じく無い |
| 決済のアプリ(`PaymentManager`)、`a.hrefTranslate`、`SerialPort.connected`、`Request.targetAddressSpace`、`navigator.deprecatedURNToURL`など | 無かった | ある |
| `AbstractRange`の`startContainer`と`endContainer` | あった | `OpaqueRange`が入っているときは無い(Chromeと同じく、引数の解決の後に決める) |
| `window.originAgentCluster` | 偽 | 真。文書をオリジンで分けたエージェントの集まりに入れる(`Origin-Agent-Cluster: ?0`のときを除く)。Blinkの既定の設定が許していたファイルのページの全体へのアクセスをChromeの設定と同じく切った |
| `chrome.loadTimes`、`chrome.csi`、`chrome.app` | 関数に`prototype`がなく、`app`のメンバの順が違った | Chromeと同じ |
| WebGLの`VERSION`、`SHADING_LANGUAGE_VERSION` | ANGLEの文字列(`OpenGL ES 2.0 (ANGLE 2.1.1 ...)`) | GPUのコマンドバッファの文字列(`WebGL 1.0 (OpenGL ES 2.0 Chromium)`) |
| 権限の既定 | `payment-handler`、`pointer-lock`、`keyboard-lock`が尋ねる、`periodic-background-sync`、`top-level-storage-access`が許す | 前の3つは許す、`periodic-background-sync`はインストールしたアプリだけ(試験の`periodicsync=`の行のとき)、`top-level-storage-access`は尋ねる |
| `enumerateDevices` | 許しの前から機器の名前とIDが見えた | 許しの前は種類ごとに1つ、ID、グループ、名前を空にする |
| 文書の`PerformanceNavigationTiming.contentType` | 空 | 文書の型 |
| `navigator.cpuPerformance` | 0 | コアの数からの段階(4コアは2) |
| キーボードの配列の一覧 | 47 | 48(`IntlBackslash`) |
| `(transform-3d)`、`(-webkit-transform-3d)` | 偽 | 真(パッチ`media_values_3d.patch`) |
| `getClientCapabilities` | 空 | このマシンの認証器(パスキーの保存、利用者の確認あり)と、条件付きの仲介を持たないことを名前で答える |
| ワーカーのIndexedDB | ファクトリが無く、`indexedDB.open`が答えなかった | ウインドウと同じオリジンのデータベースを、ウインドウのスレッドで扱う |
| 文書の計時 | 移動の開始が確定の時刻で、`performance.timing`の`fetchStart`、`requestStart`、`responseStart`、`connectStart`などが0だった(YouTubeの計時のマークが負の時刻になる) | ウインドウが移動を告げられた時刻、要求を送った時刻、答えの来た時刻を文書の計時と応答の`LoadTiming`に渡す(`br_navigation_timing`、`apply_nav_timing`) |

その後、比べ方を2つ足した。1つ目はビルドの設定の突き合わせである。公式ビルドのgnの引数(`is_official_build`、x64)を別の出力のディレクトリで`gn args --list`に一覧させ、`out/tessronos`の一覧と比べた。ページから見える振る舞いに関わる違いは、`v8_enable_temporal_support`、`enable_hls_demuxer`、`enable_mse_mpeg2ts_stream_parser`、`use_v8_context_snapshot`の4つだった。content層がbase::Featureの状態からBlinkの実行時の機能を決める表(`content/child/runtime_features.cc`)も既定の状態で突き合わせたが、このブラウザの設定と違うものはなかった。2つ目はWeb Platform Testsである。チェックアウトの`third_party/blink/web_tests/external/wpt`をwptserveで出し、試験のページを1つずつiframeで開くページ(runner)が、testharness.jsが親のウインドウに送る完了の通知を集める。同じ試験をChromeとこのブラウザで回し、サブテストごとに結果を比べた。

これらで見つかり、直したものは次のとおりである。

| 項目 | 以前 | 直した後(Chromeと同じ) |
| --- | --- | --- |
| `Temporal` | 無かった | V8を`v8_enable_temporal_support`で作り、temporal_capiとICU4XのRustのクレートをリンクする(ブラウザはRustの部分のオブジェクトファイルごと、V8だけをリンクするプログラムは同じもののアーカイブから要るものだけをリンクする) |
| HLSとMPEG-2 TS | `canPlayType`がHLSに空を答え、MSEが`video/mp2t`を受け付けなかった | メディアライブラリのHLSのデマルチプレクサで再生する(`port/media/hls_tessronos.cc`がプレイリストとセグメントを取得して渡す)。MPEG-2 TSのストリームパーサを入れる |
| ウインドウのグローバルのプロパティの並び | 実行時の機能で入るインタフェースが`console`の直後に逆順で並んだ | コンテキストのスナップショットを使うChromeと同じ並び。スナップショットに入る型(Window、WindowProperties、HTMLDocument、Document、Node、EventTarget)のテンプレートを、すべての実行時の機能を切って作り、実行時の機能のものはコンテキストごとに後から入れる(`V8ContextSnapshotImpl`とパッチ`context_snapshot_emulation.patch`、`local_window_proxy.patch`) |
| `window.chrome`の位置 | コンテキストの準備が済んだ後に入れていた | コンテキストを作った直後(`DidCreateScriptContext`)に入れる |
| `Referer` | ネットワークの層が要求のヘッダの`Referer`を捨て、どの要求にも付かなかった | Blinkが決めたリファラとリファラポリシーを、スタックの要求そのものに渡す(`br_loader.cc`、`net_fetch_tessronos.cc`) |
| iframeの中の`SharedWorker` | ページの最上位のオリジンでworkerを作り、ほかのオリジンのiframeではスクリプトを読めなかった | workerを作った文書のオリジンで作る |
| iframeを`about:blank`に移したとき | 移した文書のオリジンを渡さず、別のオリジン(不透明なオリジン)になった | 移した文書のオリジンを受け継ぐ |
| Paint Timing、Largest Contentful Paint | 項目が出なかった | 描いたフレームを画面に出した時刻を`NotifyPresentationTime`の答えにする。ウィジェットのないフレームでも計時を続ける(パッチ`paint_timing.patch`) |
| `longtask` | 項目が出なかった | ページごとにタスクの長さを監視し(`AnimationFrameTimingMonitor`)、50ミリ秒以上のタスクをページのフレームのタイムラインに入れる |
| `long-animation-frame` | 項目が出なかった | フレームの始め(`requestAnimationFrame`のコールバックの前)、スタイルとレイアウトの始め、描き終えた時刻を、ウィジェットが通知するのと同じく同じモニタに通知する。フレームのタスクの終わりと描き終えた時刻は、ウィジェットのないフレームのためのフック(`g_frame_timing_monitor_hook`、パッチ`local_frame.patch`、`paint_timing.patch`)でモニタに届く。50ミリ秒以上のフレームと、何も描かずに50ミリ秒以上かかったタスクが、画面に出た時刻とともに項目になる |
| `keyboard.getLayoutMap()` | ほかのオリジンのiframeでも答えた | 最上位のフレームか、権限ポリシーが`keyboard-map`を許すフレームだけに答える(パッチ`keyboard_layout.patch`) |
| メモリオブジェクト(`memfd_create`)の数 | 128まで。Blobの読出しやfetchの本文の多いページで、Mojoのデータパイプが作れなくなった | 1024まで(`lib/libcxxrt/ts_mman.c`) |

本物のLinuxのマシンどうしでも違うので、そのままにしたものがある。画面とウインドウの大きさ、プロセッサの数、`deviceMemory`、GPUの名前と限界の値とWebGLの拡張、Web Audioの`sampleRate`、時計の値の端数、再帰の深さ、音声の合成の声、`AudioContext`の初めの状態(ChromeはYouTubeのような利用の多いサイトで自動再生を許し、ほかでは利用者の操作まで`suspended`にする)である。

2026年10月10日に、次のものも直した。

| 項目 | 以前 | 直した後(Chromeと同じ) |
| --- | --- | --- |
| 明朝体と等幅のフォント | 実身「書体」にゴシック体しかなく、`serif`と`monospace`もゴシック体だった | 実身「書体」にNoto Serif JPとNoto Sans Mono CJK JPを入れ、`serif`と`monospace`をそれに充てる(14.4) |
| ワーカーの`OffscreenCanvas`のWebGL | `getContext('webgl')`がnullだった | ANGLEのコンテキストを、それを使うスレッドで現在のものにする(14.6.1) |
| 長いアニメーションフレーム | 項目が出なかった | 上の表の`long-animation-frame` |
| iframeの文書とService Worker | 制御するのはページの文書だけで、iframeの移動もワーカーに尋ねなかった | 文書ごとにクライアントにし、iframeの文書も制御し、iframeの移動も`fetch`イベントにする(14.4.6) |

同じ日に、上の直しを入れたブラウザでWeb Platform Testsを回し直した。比べたのは、idlharnessの158本を含む2つの一覧(4188本、重なりを除く)で、Chromeの結果は回し終えてあったものを使った。QEMUのマシンを2台並べ、試験を1つずつ配るキュー(`runsrv3.py`)から両方が取り、止まったマシンは起こし直す。試験のページは`10.0.2.2`から開くので、起動の行`secure=`でそのオリジンを安全なコンテキストとして扱う(Chromeは`127.0.0.1`から開くので、もとから安全なコンテキストになる)。サブテストの名前にあるホストとポートは、比べる前にそろえる。1回目は4188本のうち280本、1480のサブテストが違った。違いの多い分野から直し、違った試験だけを回し直して、166本、721のサブテストになった(同じ数え方。そのうちChromeだけが通るものが594、こちらだけが通るものが95)。1回目に通らず今は通るサブテストは745ある。直したものは次のとおりである。

| 項目 | 以前 | 直した後(Chromeと同じ) |
| --- | --- | --- |
| Blobを本文にした`fetch`とXMLHttpRequest | 本文が空で送られた(データパイプの部分を読まなかった) | 本文のデータパイプを読み切ってから送る(`br_loader.cc`の`BodyReader`) |
| iframeへのフォームの送信 | ファイルを含む本文が空で、ASCII以外の本文はLatin-1からUTF-8に化けた | ファイル(Blob)の中身を読んでから送り、本文はバイトのまま送る。ページのフォームも同じくバイトのまま |
| `blob:`のURLの応答 | 型が空のBlobに`Content-Type`を付けなかった | 空でも付ける(`Content-Length`、`Content-Range`、`Content-Type`の順) |
| 応答のヘッダの値 | UTF-8として読み、ASCII以外を含む値が消えた | 1バイトを1文字として読む |
| 何度も現れるヘッダ(CORSの検査) | 最初の1つだけを見た | コンマと空白でつないだ1つの値として見る |
| XMLHttpRequestの`upload`のイベント | 起きなかった | 本文を送った後、応答の前に送った量を通知する |
| 進捗のイベントの`lengthComputable` | いつも真 | 応答に`Content-Length`があるときだけ真 |
| `statusText` | 200なら`OK`、ほかは空 | ステータス行の理由句(HTTP/2とHTTP/3では空)。待つ読込み(同期のXMLHttpRequest)も同じ(`br_net_fetch_sync_status_text`) |
| リダイレクト | ネットワークスタックがBlinkに伝えずに辿り、`Response.redirected`、`Response.url`が元のままで、`redirect`の`manual`と`error`が効かなかった | 辿ったリダイレクトをBlinkに1つずつ伝える(`WillFollowRedirect`)。`error`は失敗にし、`manual`は最初のリダイレクトの応答を不透明なリダイレクトにする。`manual`と`error`の要求は、ネットワークスタックが最初のリダイレクトで止まり(読込みの層の行`X-TessronOS-Redirect: stop`)、その先を求めない。CORSの要求は、リダイレクトの応答もCORSで検査する(2026年10月15日まで、`manual`の要求がその先まで辿られ、その先の応答をCORSで検査して断っていた。YouTubeのページが`https://youtube.com/`へ送るリダイレクトを辿らない`fetch`が、その先の`www.youtube.com`の応答のCORSのエラーになった) |
| `fetch()`の`cache`の指定 | スタックに伝えなかった | 読込みのフラグ(検証する、キャッシュを通らない、など)を伝える |
| 読込みの失敗の理由 | どの失敗も`ERR_FAILED`としてBlinkに伝えた。Webフォント(`@font-face`)は最初にHTTPキャッシュだけから読み(キャッシュを見た読込み)、`ERR_CACHE_MISS`ならネットワークから読み直すが、その印が伝わらず、キャッシュにないフォントはどれも読まれなかった(Google マップのボタンのアイコンが、記号の書体が読まれず、四角に×の形で表示された) | ネットワークスタックのエラー(`br_net_fetch_error`)をそのまま伝える(`br_loader.cc`の`Fail`) |
| 許されないポート | 接続しに行った | ネットワークサービスと同じく直ちに断る(`net::IsPortAllowedForScheme`) |
| URLのスキーム | `chrome-extension:`、`isolated-app:`などを特別なスキームとして登録していなかった | Linux版のChromeと同じく登録する(ホストを持ち、オリジンを持つ、安全、アクセスなし、など) |
| `document.createEvent()` | モジュールのイベント(`StorageEvent`、`DeviceMotionEvent`など)を作れなかった | モジュールのイベントのファクトリを登録する |
| 文字の符号化を示さない文書 | windows-1252として読んだ | 日本語のChromeと同じくShift_JISとして読む |
| ローカル時刻(`document.lastModified`) | Cのライブラリが協定世界時のまま、9時間ずれた | システムのタイムゾーンの時差を`TZ`にしてCのライブラリに渡す |
| iframeの`blob:`の文書 | 開かなかった | Blobの中身で文書にする |
| iframeの`about:blank`と`about:srcdoc` | 基底のURLを親から受け継がなかった。新しいiframeの`load`のイベントが後から起きた | 開いた文書(`srcdoc`は親)の基底のURLを受け継ぐ。新しいiframeの最初の`about:blank`はその場で確定し、`load`は差し込んだ時に起きる |
| iframeの`sandbox`と`allow` | 文書に効かなかった | 親の文書とiframeの属性のサンドボックスのフラグを文書の方針(policy container)に入れ、オリジンを切るときは不透明なオリジンにする。`allow`の権限ポリシーも渡す |
| ウインドウの`blob:`のURLの格納 | フレームの関連インタフェースを通したので、ウインドウのスレッドが同期のXMLHttpRequestで待つと、URLの登録が届かずに止まった | 格納のスレッドへの専用のパイプにする(パッチ`public_url_manager.patch`) |
| 保存域の許可のハンドルの`createObjectURL` | 登録はハンドルの格納のパイプで、読込みはフレームの格納のパイプで届くので、すぐに`fetch()`すると登録より先に引かれ、「登録されていない」で失敗した | 解決の時に見つからないURLは、読込みの時に引き直し、さらに10ミリ秒ずつ50回まで登録を待つ(`blob_tessronos.cc`の`UrlLoaderFactory`) |
| `BroadcastChannel`のBlob | 受け手でnullになった | Blobのハンドルもメッセージと一緒に渡す |
| ワーカーのストレージのバケット | ワーカーに与えていなかった | ワーカーのオリジンのものを与える |
| Element Timingの矩形 | 0だった | ウィジェットのないフレームでは見える矩形をそのまま使う(パッチ`element_timing_utils.patch`) |
| 部品のリソースの計時 | 計時がなく、`Timing-Allow-Origin`の判断もなかった | 要求を渡した時刻と応答の時刻、通ったオリジンの判断を応答に付ける。プロトコル(`nextHopProtocol`)も付ける |
| `<frame>`の移動の`fetch`イベント | `destination`が`iframe`だった | `frame` |

続けて、残っていた次のものを直した。

| 項目 | 以前 | 直した後(Chromeと同じ) |
| --- | --- | --- |
| `fetch()`とXMLHttpRequestの本文 | 全部がそろってから渡した。終わらない応答の`fetch()`は答えず、本文を届いた分ずつ読めなかった | 届いた分ずつデータパイプに書く(14.5) |
| 同期のXMLHttpRequestの`statusText` | 空 | ステータス行の理由句 |
| iframeのセッション履歴 | iframeの移動は項目にならず、`history.length`が増えず、`history.back()`でiframeが戻らなかった | 結合セッション履歴(14.7) |
| iframeの文書の計時 | 読込みの種類がいつも`navigate`、`redirectCount`が0、`fetchStart`が0、`document.referrer`が空 | 移動の種類、リダイレクト、時刻、リファラを確定に渡す(14.7) |
| ワーカーの`requestAnimationFrame` | `NotSupportedError` | 60Hzの刻みで呼ぶ(14.4.2) |
| 滑らかなスクロール | すぐに行き先へ飛んだ | フレームごとに進む(14.4.2) |
| ページのスクリプトの続けての`history.go()` | 前の移動が終わる前の位置から数えた | 前の移動が終わるのを待ち、そこから数える |
| 終わらないスクリプトのワーカーの`terminate()` | マシンが止まった | 止まる(カーネルのラウンドロビン) |

これらを入れて、関係する分野(`/fetch/api/basic`、`response`、`abort`、`/xhr`、`/html/browsers/history`、`/navigation-timing`、`/resource-timing`、`/css/cssom-view`のスクロール、ワーカーの`requestAnimationFrame`の881本)を回し直した。カーネルに同じ優先度の間のラウンドロビン(時分割の層の16〜32を20ミリ秒で回す)が入った後に、`Worker-terminate-forever-during-evaluation.html`(7つのサブテスト)も回した。前の回より通らなくなったサブテストはなく、163のサブテストが新たに通った。4188本全体では、Chromeと違うのは129本、563のサブテストになった(Chromeだけが通るものが434、こちらだけが通るものが98)。1回目からは、通らず今は通るサブテストが908ある。

残った違いには、環境によるものがある。Chromeは`127.0.0.1`を安全なオリジンとするので、そこでは`Secure`のCookieが付くが、`10.0.2.2`の試験では付かない。閉じたポートへの接続は、Windowsのループバックではすぐに断られるが、QEMUの利用者モードのネットワークでは待たされる。`www1.`のような別名のホストは、どちらでも引けない。書体が違うので、文字の幅を測る試験の値が違う。QEMUのマシンは遅いので、HTMLのパーサが時間の枠(10ミリ秒)を使い切って途中で譲り、そこでマイクロタスクが回って、`MutationObserver`の記録の分かれ方が違う(`MutationObserver-document.html`など。本文の渡し方によるものではない)。終わらないスクリプトを走らせているワーカーは、以前は止められなかった。同じ優先度のタスクの間にラウンドロビンがなく、空回りするワーカーが4つあると4つのプロセッサがふさがり、`terminate()`を2秒後に強いるページのスレッドのタイマも回らず、QEMUではマシンの出力が止まった。カーネルに同じ優先度の間のラウンドロビンが入って、Blinkの強制の終了(`TerminateExecution`)が効くようになった。

まだ違うもので、直していないものは次のとおりである。Widevine、ブランドの名前(`Google Chrome`はなく、`Chromium`を名乗る)、WebGLのアンチエイリアスと一部の拡張、上に挙げたWeb Platform Testsの残り。

YouTubeの再生の検証(`GenerateIT`)は、2026年10月9日の時点では答えの先頭のトークンがnullだった。上の表の直しを入れた後の2026年10月10日には、答えの先頭にトークンが返り、その後のSABRの答えの`STREAM_PROTECTION_STATUS`は1のまま本編が流れた。`GenerateIT`の要求の本文も、Chromeの1967バイトに対して1978バイトになった。検証のスクリプトの中身は調べておらず、どの直しが効いたかは切り分けていない。

### 14.4.18 画面、ウインドウ、音の共有

`getDisplayMedia`は、呼ばれるたびにデータボックスのパネル20「画面の共有」を出す。パネルには、求めているオリジン、画面全体とウインドウの一覧(手前のウインドウから順。部品11)、スイッチ「音も共有する」(部品12)、〈取消〉と〈共有〉のボタンがある。既定のボタンは安全な側の〈取消〉である。一覧は`wm_share_list`(SVC 9-39)で得る。名前のバーがあって表示されていて、ほかのウインドウに従属していないウインドウが並ぶ。ページの指定は次のように効く。`selfBrowserSurface: 'exclude'`ならページのウインドウを並べず、`monitorTypeSurfaces: 'exclude'`なら画面全体を並べない。`displaySurface`は最初に選ぶ行を決める。

| 選んだもの | `displaySurface` | 映像 | 音 |
| --- | --- | --- | --- |
| 画面全体 | `monitor` | `wm_grab_screen`。組み立てた画面とポインタ | すべてのプログラムの音。`restrictOwnAudio`なら自分のプロセスを除く。`systemAudio: 'exclude'`なら音はない |
| 同じプログラム(ブラウザ)のウインドウ。Chromeのタブに当たる | `browser` | `wm_grab_window`。そのウインドウの作業領域 | そのプロセスの音 |
| ほかのウインドウ | `window` | 同上 | そのプロセスの音。カーネルのタスクが開いたウインドウ(デスクトップなど)には音がない |

ウインドウは、ウインドウマネージャが持つウインドウ自身のピクセルから読む。X11のCompositeの名前付きピクスマップ、PipeWireのウインドウのノードと同じく、ほかのウインドウに隠れていても正しく読める。読むのは作業領域だけで、枠、タイトルバー、ポインタは入らない(`cursor`は`never`)。ウインドウ番号は使い回されるので、番号と開いたときの通し番号の組で指定する。閉じたウインドウには`E_NOEXS`が返り、ページのトラックは終わる(`ended`)。大きさが変わるとフレームの大きさも変わる。コピーの途中で大きさが変わったときは、そのフレームを捨てる。カーネルの権限での確認はない。プログラムは利用者が選んだものだけを読むという決まりで、`wm_grab_screen`と同じである。

音は、サウンドデバイスのモニタ(`DN_SDMONITOR`、レコード`OB_SND_MONITOR`、6.5)で取り込む。sndioの`mon`やPulseAudioのモニタソースと同じく、ミキサが足し合わせた音を録音のチャンネルに渡す。プログラムの音量の後で全体の音量の前を取るので、利用者が自分のスピーカーを消しても共有の音は消えない。プロセスを指定でき(`SND_MON_ONLY`、`SND_MON_EXCEPT`)、これがタブやウインドウの音になる。ハードウェアのストリームが動いているときだけモニタにフレームが来るので、音を鳴らす出力がない間、ブラウザは時計に合わせて無音を渡す(`br_audio_sink.cc`)。ブラウザはプレイヤーのチャンネルをユニット`snda`、マイクを`snda0`で使い、モニタは`snda1`から順に使う。読むのが遅いときは新しいフレームを捨て、録音は止めない。

共有中はウインドウのタイトルの前に「画面を共有中」と出す。`CaptureController`の操作(フォーカス、拡大縮小、ホイールの転送)は扱わないと答える。`suppressLocalAudioPlayback`は効かない。

## 14.5 ネットワーク

ネットワークスタック(`URLRequestContext`)は専用のスレッドで動き、ソケットはノンブロッキングで使う。スレッドは、スタックが読み書きを待たせているすべてのソケットと、自分を起こすためのデータグラムのソケット(ループバックにバインドする)を1回の`poll`で待つ。どれかのソケットの準備ができたとき、ほかのスレッドから仕事を渡されたとき(そのソケットに1バイトが送られる。続けて渡されても、スレッドが受け取るまでは1回だけ送る)、スタックの次のタイマーの時刻が来たときに起き、準備のできたソケットを進めて、時刻の来た仕事を実行する。その回のうちは、ソケットの準備を待ちの結果から答え、システムに尋ね直さない。待ちは長くても0.1秒である。以前は10ミリ秒ごとにすべてのソケットを1つずつ調べていたので、届いたデータグラムを最大10ミリ秒待たせていた。

名前は、スタックのDNSクライアントが、システムのネームサーバにデータグラムのソケットで尋ねる。ネームサーバは、`so_getdns`で得るIPv4のものと、システムの実身のレコード7の`DNS6`の行(設定のもの、次にルータが告げたもの、9.3.4)のIPv6のもので、この順に尋ねる(`port/net/br_sysnet.cc`)。アドレス(AとAAAA)と同時に、`https`のアドレスの名前ではHTTPSレコード(RFC 9460)も尋ねる。ネームサーバがないとき、またはDNSクライアントが答えられないときは、1件ごとのスレッドで`getaddrinfo`を呼び、IPv4とIPv6のアドレスをすべて、システムが返す順に受け取る(9.3.2)。ページの部品は並行して取得し、ページを表示するのは部品がそろってから(最長30秒)である。

- 同時に持つ接続は32までに抑え、それを超える要求はソケットが空くのを待つ。システム全体のソケットの上限は64である(9.2)。
- TLSはChromiumのBoringSSLで、サーバの証明書はChromiumのルートストアの証明書(実行ファイルに埋め込む)で検証する。期限の切れた証明書のサイトは開かない。
- Cookie、`localStorage`、HTTPキャッシュは、実身「かな漢字変換の学習」にある実身「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」のレコード1にしまう(14.8)。Cookieと`localStorage`は変わってから2秒後、キャッシュは新しいものが来てから1分後に保存し、終了時に残りを保存する。HTTPキャッシュはエントリ単位で、新しいエントリと変わったエントリ(ストリームの大きさか、ヘッダとメタデータのハッシュが変わったもの)、消えたエントリだけを書く。書き込みはストアのスレッドが行う(14.8.1)。
- フォームの`POST`は、本文と種類をアプリが受け取り、ネットワークスタックで送る。
- スクリプトの`fetch`と`XMLHttpRequest`の応答の本文は、Mojoのデータパイプで渡す。`fetch()`の`Response`は本文をストリームとしてだけ読むためである。応答は100ミリ秒だけ持っておき、その間に本文が終われば、応答と本文を1つのタスクでまとめて渡す(すぐに終わる応答が、それまでと同じ順でタスクに乗る。長いタスクの後のXMLHttpRequestの`timeout`の判定など)。終わらなければ、そこで応答とパイプを渡し、本文はネットワークスタックから届いた分ずつパイプに書く(`BrURLLoader`の`PipedPart`と`Release`、パイプは512KB)。パイプが一杯のときは、受け取れなかった分を取っておき、パイプが書けるようになったら書く。そのため、ページは本文を届いた分ずつ読め(`ReadableStream`、XMLHttpRequestの`progress`)、動画を`fetch()`で少しずつ受け取るページ(YouTubeのSABR)も、全部が届く前に再生に回せる。終わらない応答も、応答が来た時点で`fetch()`が答える。読み手が本文を手放したとき(`reader.cancel()`)と、ローダが捨てられたとき(`AbortController`)は、スタックに取得をやめさせる。こうした要求は、応答が来た時点でページの部品の待ちから外す。届いた分ごとにページのスレッドの仕事が1つ増えるので、動画を流し続けるページでは仕事の列が空にならない。そこで、エンジンのスレッドが仕事を回すのは列が空になるまでか、長くても100ミリ秒までとし(`run_tasks`)、残りは次に回す。そうしないと、スレッドがフレームと入力とページの部品の待ちに戻らず、YouTubeのページの表示が4分ほど遅れた。データパイプの共有メモリは、`lib/libcxxrt`の`memfd_create`が作るプロセスの中のメモリオブジェクトで(5.7)、パッチ`shared_memory_region.patch`と`shared_memory_tracker.patch`がそれを使わせる。同じ領域のマッピングはどれも同じアドレスになる。
- `indexedDB`は、BlinkのIndexedDBのページ側(要求、トランザクションの進み、値からのキーの取出し、値の包み)をそのまま使い、ブラウザがプロセスの外に持つ格納側(Mojoの`IDBFactory`、`IDBDatabase`、`IDBTransaction`、`IDBCursor`)を`port/blink/indexeddb_tessronos.cc`がプロセスの中に持つ。ページが空のブローカに`IDBFactory`を求めると、そのページのオリジンの格納側が結ばれる。データはメモリにあり、オリジンごとのデータベース、データベースごとのオブジェクトストア、ストアごとのレコードとインデックスである。キーは、並びがIndexedDBのキーの順(数、日付、文字列、バイナリ、配列)になるバイト列にして持つので、レコードとインデックスの並びがそのままカーソルの順になる。書くトランザクションは最初に書く前にデータベースのコピーを取り、中断されるとコピーに戻す。トランザクションは互いを待たず、要求が来た順に答える。データベースを開くことと消すことは、Chromiumの接続の調整役(connection coordinator)と同じく、データベースごとに頼まれた順に1つずつ行う。開く頼みは、そのデータベースのバージョン変更のトランザクションが動いている間は待ち、終わってから、アップグレードが作ったオブジェクトストアを含む姿で答える。アップグレードと削除は、ほかの開いている接続に`versionchange`を伝え、それらが閉じるまで待つ。ページがその接続を閉じずにおくと(`VersionChangeIgnored`)、頼んだ側に`blocked`を伝える。以前は、アップグレードの最中に来た同じバージョンを開く頼みに、まだストアのない姿で答えていた。ページの別々の部品が同じデータベースを続けて開くZoomのWeb版では、2つ目の接続の`transaction()`が`NotFoundError`(One of the specified object stores was not found)で失敗していた。削除も、開いている接続を強制的に閉じて(`ForcedClose`)すぐに消していた。何かを変えたトランザクションが確定すると、2秒後に全体を「ブラウザの保存域」のレコード2に書き、起動時に読み戻す。BlobやFileを含む値(大きな値もBlinkがBlobに包んで渡す)は、値のバイト列の前にその外部オブジェクトの番号を付けて持つ。外部オブジェクトは番号ごとに、ページが渡したBlob(持ち続け、値を返すたびに複製して渡す)、種類、大きさ、Fileなら名前と時刻、Blobの格納側から後で読み取った中身を持つ。データベースを書くときは中身も書き、読み戻した中身は、それを含む値を最初に返すときにBlobに作り直す。File System Accessのハンドルを含む値は受け付けない。
- `WebSocket`は、BlinkのチャネルをMojoでネットワークサービスに繋ぐ代わりに、`port/blink/websocket_tessronos.cc`のチャネル(パッチ`dom_websocket.patch`で`DOMWebSocket`が作る)が、ネットワークスレッドでネットワークスタックの`net::WebSocketChannel`を動かす。開くときのハンドシェイク、フレームの組立てとマスク、閉じるときのハンドシェイクはスタックが行い、TLS、証明書の検証、Cookieは通常の要求と同じものを使う。メッセージはネットワークスレッドでフレームから組み立て、まとめてページのスレッドに渡す。Blobを送るときは、Blobの格納側からその中身を読み取ってからバイナリのメッセージとして送る。読み取る間にページが送ったものは、そのBlobの後ろで待たせ、送った順を保つ。`WebSocketStream`も同じチャネルで動く(パッチ`websocket_stream.patch`)。
- `URL.createObjectURL(blob)`は、ChromiumではBlobの公開URLを`BlobURLStore`に同期のMojo呼び出し(`Register`)で登録する。このブラウザでは受け手をバインドする仕事がページのスレッドの後の仕事として残るので、同期の呼び出しはその仕事を待って永久に戻らず、Googleマップの読込みが止まった(14.7のエンジンのスレッドの待ちの記録で分かった)。パッチ`public_url_manager.patch`で、`Register`を答えを待たない形で送り、URLはそのまま作る。Blobの中身とURLの解決は14.4.4の格納側が持つ。
- 画像の`decode()`は、`ChromeClient`の`RequestDecode`に復号を頼み、答えを待つ。基底の`ChromeClient`はその場で「できない」と答えるが、画像のローダは要求の列を回しながら頼むので、その場の答えが回している列から要求を消し、プロセスが落ちた(Googleマップ)。このブラウザの`ChromeClient`は、画像を描くときに復号するので「できる」と答え、その答えを後の仕事として渡す(`br_blink.cc`)。
- canvasの描画の時間を測る`CanvasPerformanceMonitor`は、描画のあったタスクの終わりで測り終えることを前提にし、次のタスクの始めまで測りが残っていると止まる(`NOTREACHED`)。このブラウザは、フレームの始め(`requestAnimationFrame`のコールバック)と、入力をページに渡すことを、スケジューラのタスクの外で行うので、そこでcanvasに描くページ(Googleマップ)で止まった。パッチ`canvas_performance_monitor.patch`で、タスクの外の描画の測りは捨てて次のタスクを始める。
- `EventSource`の要求(`Accept: text/event-stream`)は、応答の本文を全部そろえてから渡す通常の取得ではなく、応答が来た時点で応答を、そのあと本文を届いた分ずつBlinkに渡す(`br_net_fetch_stream`)。開いたままのストリームがページの表示を待たせないように、応答が来た時点で未完了の要求の数から外す。`EventSource`を閉じると接続を切る。
- IPv6のアドレスにも接続する。ソケット(`port/net/tcp_socket_tessronos.cc`、`br_stream.cc`)は接続先のアドレスのファミリで開き、IPv6のソケットは`IPV6_V6ONLY`にする。スタックが名前のIPv4とIPv6の両方を求めるとき、`getaddrinfo`には`AI_ADDRCONFIG`を付けるので、外へ届くIPv6のアドレス(リンクローカルでもサイトローカルでもないもの)を持たないマシンではAAAAを尋ねず、IPv4だけになる。両方のアドレスがあれば、Chromiumの接続の手順(`TransportConnectJob`)がIPv6のアドレスに先に接続を始め、300ミリ秒たっても繋がらなければIPv4のアドレスにも接続を始めて、先に繋がった方を使う(Happy Eyeballs)。IPv6の接続がすぐに失敗したときは、待たずにIPv4に移る。`http://[::1]:8080/`のようなIPv6のアドレスのURLも開ける。スタックがIPv6に届くかを調べるときは、UDPのソケットを外のIPv6のアドレスに接続して、そのソケットのアドレスを見る。DNSクライアントで尋ねるときも、この調べでIPv6に届かないと分かったマシンではAAAAを尋ねない。
- HTTP/3(QUIC)を使う。サーバがHTTP/3を使えることは、応答の`Alt-Svc`か、名前のHTTPSレコードの`alpn`で知る。HTTPSレコードで知ったときは最初の要求から、`Alt-Svc`で知ったときはそれ以後の要求で、スタックはQUICの接続をTCPの接続と競わせ、先にできた方で送る。TCPの接続は、QUICの名前解決が済んでから、そのサーバの往復時間の1.5倍(知らなければ0.3秒)だけ遅れて始める。QUICがこのマシンのアドレスで動いた記録がなければ遅らせない。QUICの接続ができなければ、そのサーバのHTTP/3はしばらく使えないものとして記録され、要求はTCP(HTTP/2かHTTP/1.1)のままで送る。この切り替えと戻りはChromiumの仕組み(`HttpStreamFactory`の要求の競争と`HttpServerProperties`の記録)がそのまま行う。HTTPSレコードにECHの設定があれば、TLSのClient Helloを暗号化する(ECH)。QUICの接続は開いたソケットを使い続け(ポートの移動をしない)、ハンドシェイクを15秒まで待つ。
- サーバについて知ったこと(HTTP/3を使えるサーバと`Alt-Svc`の期限、サーバごとの往復時間、QUICが最後に動いたときのマシンのアドレス、使えないと記録したQUIC、HTTP/2を使えるサーバ)と、QUICの接続を再開するためのセッションチケット(TLSのセッション、サーバのトランスポートパラメータ、HTTP/3の設定、アドレスの確認のトークン)は、「ブラウザの蓄え」のレコード2にしまい、次に起動したブラウザが読み戻す。前者は、`HttpServerProperties`が書き出す形(JSON)のまま持つ。チケットはQUICのセッションプールのセッションキャッシュにあり、キャッシュはチケットを渡すときに消す(チケットは1回だけ使う)ので、HTTP/3で応答したサーバと読み戻したサーバのチケットを1つずつ取り出してコピーし、すぐに戻す。予約された番号のトランスポートパラメータ(GREASE)は書き出さない。セッションキャッシュは、プールが自分のほかに入ることを許しているクラス(`net::test::QuicSessionPoolPeer`)の名前で取り出すので、Chromiumのソースは変えていない。ネットワークから新しい応答が来てから10秒後と終了時に書き直す。読み戻したブラウザは、最初の要求からQUICの接続を競わせ、チケットで接続を再開して完全なハンドシェイクを省く。
- QUICのUDPのソケット(`port/net/udp_socket_tessronos.cc`)は、Chromiumの`UDPSocketPosix`を、システムのノンブロッキングのデータグラムのソケット(`socket`、`connect`、`recvfrom`、`sendto`)で実装したものである。すぐに読み書きできないときは、要求をソケットの待ちとして残し、ネットワークのスレッドがTCPのソケットと一緒に待つ。QUICの読み手には、待っているデータグラムを1回の読出し(`ReadMultiple`、機能`QuicUseReadMultiple`を有効にする)でまとめて渡す。システムには複数のデータグラムを1回で受け取る呼出し(`recvmmsg`)がないので、移植層が`recvfrom`を続けて呼び、64KBのバッファに詰める。セグメンテーションのオフロード(GSO)、まとめての受信(GRO)と分割禁止のフラグは使わない。ECNは扱う。送るデータグラムのマークはQUICが求めるとおりに`IP_TOS`(IPv6は`IPV6_TCLASS`)で付け、QUICが受け取るマークを求める(`SetRecvTos`)と、ソケットは`IP_RECVTOS`(`IPV6_RECVTCLASS`)を1にして、データグラムを`recvmsg`で読み、付随データのTOSをQUICの読み手に渡す(9.2)。QUICはそれを数えてACKのフレームでサーバに伝える。受信のバッファはQUICが求める1MBをシステムが受け付けなければ、受け付けるまで半分にして求め直す。スタックのバッファが一時的に足りないときの送信の失敗は`ENOBUFS`になり、QUICはしばらく待って送り直す。

### 14.5.1 WebTransport

`new WebTransport(url)`は、ネットワークスタックのHTTP/3(QUIC)の上のWebTransportのクライアント(`net::CreateWebTransportClient`)を使う。Chromiumではネットワークサービスがその接続とページの側(セッション、ストリーム、データグラムのMojoのインタフェース)を繋ぐが、このブラウザでは`port/blink/web_transport_tessronos.cc`がネットワークスレッドで繋ぐ。

- ストリームのバイトは、ページのデータパイプとQUICのストリームの間を、双方が受け取れるだけ渡す。ストリームの終わり(FIN)、リセット、送信の停止の求めは両方向に伝える。ページが作った双方向と単方向のストリーム、サーバが開いたストリームのどちらも扱う。
- データグラムは、セッションが送れるときに送り、処理されたことをページに通知する。
- サーバの証明書は、ページがSHA-256で名指したもの(`serverCertificateHashes`)でもよく、その照合はスタックが行う。
- 応答のヘッダは、Cookieを除いてページに渡す。
- ネットワークスレッドでMojoのメッセージを受けられるように、そのスレッドはシーケンスごとのストレージの表(`SequenceLocalStorageMap`)を持つ。

試験は、ホストで動かすWebTransportのエコーのサーバ(`tools/browser/wt_echo_server.py`、aioquicを使う)と`NET=user`で行う(14.10)。

### 14.5.2 WebRTC

`RTCPeerConnection`はChromiumのWebRTC(`third_party/webrtc`)とBlinkのモジュールをそのまま使う。WebRTCのスレッドはエンジンのスレッドの上で動く(`components/webrtc`の`ThreadWrapper`)。ネットワークへの出口は、Chromiumではネットワークサービスの`P2PSocketManager`であるが、このブラウザでは`port/blink/p2p_tessronos.cc`が受け持つ。

- マシンのインタフェース(`GetNetworkList`、システムの`so_getifaddr`が答えるIPv4のアドレスとマスク)を、ページが求めたときに通知する。ネットワークがなくてもマシンの中の接続ができるように、ループバックの候補も集める(`Platform::AllowsLoopbackInPeerConnection`)。
- ソケットはネットワークスタックのデータグラムのソケットで、求められたアドレスと範囲のポートにバインドし、ネットワークスレッドで読み書きする。届いたものは届いた順にページに渡し、送ったパケットはそれぞれ送ったと通知する。ICE、STUN、TURN、DTLS、SRTP、SCTP(データチャネル)はすべてこのソケットの上をWebRTCが行う。
- TCPのソケット(TCPの上のICE、TCPとTLSの上のTURN、Googleの`ssltcp`の候補が使う擬似TLS)は、Chromiumのネットワークサービスのもの(`services/network/p2p`の`P2PSocketTcp`と`P2PSocketStunTcp`)をそのままコンパイルして使う。パケットをストリームの上でRFC 4571の長さの前置きか、STUNとTURNの区切りで分けるのはそれらである。ページの側の口(Mojoの`P2PSocket`と`P2PSocketClient`)は同じパイプのまま、ネットワークのスレッドでBlinkの型でない`network::mojom`の型に結ぶ。接続はネットワークスタックの接続の手順(`network::ProxyResolvingClientSocketFactory`。プロキシの設定、TCPのソケット、TLSとブラウザの証明書の検証)で作る。UDPが通らない網(社内の網など)では、MeetのようなサイトはTCPかTLSの443番のTURNに切り替えて繋ぐ。
- ページが自分のアドレスを隠す名前(mDNSの`<uuid>.local`、`MdnsResponder`)は、ブラウザがプロセスの全体で覚え、その名前の名前解決はそれで答える。同じマシンの2つの接続は、これで互いを見つける。名前をネットワークに通知することはしない。ほかの名前はネットワークスタックの名前解決で引く。
- このブラウザは`WebLocalFrame`を使わずにBlinkのフレームを動かすので、接続を作るとき、フレームの`LocalFrame`は実行のコンテキストから得て、接続を記録する仕組み(`PeerConnectionTracker`)は使わない(パッチ`rtc_peer_connection_handler.patch`)。フレームがなくても候補はこのソケットで集める(パッチ`peer_connection_dependency_factory.patch`。Chromium 153のこのファイルの宣言と定義の型の食い違いも直す)。
- 映像の符号化と復号はソフトウェアである。WebRTCの内蔵のエンコーダとデコーダで、VP8とVP9はlibvpx、H.264の符号化はOpenH264、復号はFFmpeg、AV1の符号化はlibaom、復号はdav1dである(gnの引数の`proprietary_codecs`から`rtc_use_h264`が、`enable_av1_decoder`からWebRTCのdav1dが有効になる)。音はOpusである。`RTCRtpSender.getCapabilities`はこの4つの映像のコーデックとOpusを答える。
- 受け取った音は、WebRTCに求めたときにだけ復号される。窓のWebRTCの音の装置(`blink::WebRtcAudioDeviceImpl`)から10ミリ秒ずつ引く(`RenderData`)と、WebRTCの混合器がその窓のすべての接続の受信の流れからその分を取り出し、受信のトラックのシンクにも渡す。Chromiumでは、要素が相手のストリームを再生するときに作るWebRTCの音のレンダラ(`blink::WebRtcAudioRenderer`)がオーディオサービスの出力の流れの上で引く。このブラウザは`WebLocalFrame`を使わないのでそれを作れず、引く者がいなかった。そのため、相手の音は聞こえず、受信の流れのジッタのバッファはあふれ、マイクのエコーキャンセラは再生した音の参照を受け取らなかった(装置は引かれた音を再生のシンク、つまりマイクの音の処理にも渡す)。`port/media/rtc_audio_tessronos.cc`が、窓ごとの装置に1つ、レンダラにあたるスレッドを持つ。要素が相手のストリームを`srcObject`にすると、プレーヤは最初の音のトラックが相手のもの(`PeerConnectionRemoteAudioTrack`)ならそのレンダラの受け手になり(Chromiumと同じ選び方)、自分ではその音を流さない。スレッドは、ブラウザの音の混合(14.6.2)の自分の流れとリングに残る分が0.1秒より減るたびに引き、残る分が聞こえるまでの時間を再生の遅れとして渡す(エコーキャンセラはこの遅れで参照の音を合わせる)。要素が一時停止していても引き続け(受信の流れがあふれないように)、どの要素も再生していなければ無音を流す。音量はChromiumと同じく、相手の音源ごとに、それを持って再生している要素の音量の和(10まで、なければ0)を、WebRTCのシグナリングのスレッドで音源に設定し、WebRTCが混合の前に掛ける。

HSTS(`Strict-Transport-Security`)で知ったことは、`BrHstsKeeper`が`TransportSecurityState`の変更を聞き、利用者ごとのストア`hsts`に10秒後に保存し、次の起動で読み戻す。ホスト名はそのSHA-256だけを持つ。

## 14.6 画像、動画、音声、WebGL

画像はlibpng(Skiaのコーデック)、libjpeg-turbo、Wuffs(GIF)、libwebp、JPEG XLとAVIF(Rust、AVIFはdav1dを使う)で復号する。SVGとXHTMLはRustのXMLパーサで読む。

画像のアドレスを直接開いたとき(画像へのリンク)は、Blinkの画像の文書(`ImageDocument`)になり、画像を暗い背景の中央に表示する。ウインドウより大きな画像はウインドウに収まるように縮め、画像をクリックすると等倍にして、クリックした所をウインドウの中央に置く。もう一度クリックすると縮めた表示に戻る。タイトルは「ファイル名 (幅×高さ)」になる。

動画と音声は、Chromiumのメディアのパイプライン(`media/`)で再生する(14.6.2)。デマルチプレクサは、アドレスで指定されたリソースにはFFmpegのもの(`media::FFmpegDemuxer`)、`MediaSource`にはChromiumのもの(`media::ChunkDemuxer`)を使う。デコーダは、H.264、AAC、MP3、Vorbis、FLAC、TheoraなどがChromiumのFFmpeg、VP8とVP9がlibvpx、Opusがlibopus、AV1がdav1dである。映像と音の同期はChromiumの仕組みのままで、音のレンダラが音の出力の遅れから時計を進め、映像のレンダラがその時計に合わせてフレームを選び、間に合わないフレームを落とす。アドレスで指定されたリソースは丸ごと取得してメモリから読むので、要素の`preload`が`auto`(または`autoplay`)のときだけ読み込み時に取得し、そうでなければ再生が始まるまで取得しない。音は48kHz、16bit、ステレオでサウンドデバイスの実身に書く。

Web Audio(`AudioContext`)は、Blinkのノードとグラフをそのまま使い、出力のデバイス(`blink::WebAudioDevice`)を`port/media/web_audio_tessronos.cc`が持つ。デバイスのスレッドが、メディア要素と共用の音のリング(`br_snd.cc`)をサウンドデバイスより約0.1秒先まで満たすように、10ミリ秒(480フレーム)ずつグラフを描かせ、48kHz、16bit、ステレオにして書く。コンテキストのサンプリング周波数はデバイスの48kHzで、ページが別の周波数を求めると、Blinkの出力先がそれに変換する。発振器の波形表(`PeriodicWave`)、畳み込み、アナライザに使うFFTはRustのRustFFTで、`rust.sh`が作る。メディア要素とWeb Audioが同時に鳴ると、同じリングに交互に書かれ、音は混ぜられない。サウンドデバイスがないとき、またはデバイスがサンプルを持ったまま、聞こえた量(`OB_SND_CNT`)が0.4秒進まないとき(ハードウェアがサンプルを取っていない)は、リングを送り出すスレッドが48kHzの時刻どおりにリングを捨てて、音を出さずに進める。リングに残っている量でメディア要素の時計が進むので、こうしないと、出力の止まったデバイスでは動画の時刻と映像も止まる。捨てている間も聞こえた量を見続け、進めばまたデバイスに渡す。止まったこと、戻ったことはコンソールに出す(`browser: snd:`)。マイクと出力先の選択は扱わない(許可を問う先がなく、問合せは答えられないまま残る)。

CPUの実装(`port/webgl/gl_tessronos.cc`、`br_glsl.cc`)は、マルチサンプルのレンダーバッファ(4サンプル、回転した格子の位置)を持つ。三角形はその内側のサンプルを覆い、フラグメントシェーダは画素ごとに1度だけ実行し(画素の中心が外ならば最初に覆ったサンプルの位置で)、深度の試験と合成はサンプルごとに行う。1サンプルのフレームバッファへのブリットと読出しで、サンプルを平均する。描画バッファは`antialias`(既定は真)のとき、この明示の解決(`GL_CHROMIUM_framebuffer_multisample`)で描くので、三角形の縁がなめらかになる。`make GL=1`のとき、Mesaのsoftpipeはマルチサンプルを持つように答えるだけで縁はなめらかにならない。V3Dは本物のマルチサンプルを持つ。WEBGL_debug_renderer_infoの`UNMASKED_VENDOR_WEBGL`と`UNMASKED_RENDERER_WEBGL`は、Chromeが答える形(「Google Inc. (ベンダ)」、「ANGLE (ベンダ, デバイス, API)」)で、CPUの実装は「Google Inc. (TessronOS)」、「ANGLE (TessronOS, TessronOS software GL, OpenGL ES 2.0)」である。

WebGLは、`make GL=1`でビルドしたときはChromiumのANGLEで描き、ANGLEはOpenGL ESのバックエンドでTessronOSのMesa(第13章 13.6)の上に動く(14.6.1)。WebGL 1と2を扱い、MesaはV3Dがあればv3d、なければsoftpipeで描く。`GL=1`でないとき、またはANGLEが起動しなかったときは、以前からのCPUの実装がWebGL 1だけを描く(`port/webgl/gl_tessronos.cc`、`br_glsl.cc`。GLES2をCPUで実行し、GLSL ES 1.00の一部を解釈する)。SwiftShader(CPUで動くVulkanとGLES)は使わない。SwiftShaderは、シェーダと描画の処理を実行時に機械語に変換して実行する(ReactorのLLVMかSubzero)。書込みと実行を交互に行うメモリ(5.3)を使えば動かせるが、LLVMかSubzeroを移植することになるので、使っていない。

### 14.6.1 WebGLとANGLE

ChromiumのWebGLは、普通はGPUプロセスの中で動く。レンダラのBlinkは`gpu::gles2::GLES2Interface`にGLの呼出しを書き、その実装(`GLES2Implementation`)が呼出しをコマンドバッファに積む。GPUプロセスのデコーダ(パススルーのデコーダ)がそれを取り出してANGLEのOpenGL ESに渡す。ANGLEはWebGLのための検査とシェーダの変換を行い、OSのグラフィックスAPIで描く。このブラウザはcontentもGPUプロセスも使わないので、コマンドバッファとデコーダを持ち込まず、Blinkが呼ぶ`GLES2Interface`を、ANGLEを同じプロセスで直接呼ぶものとして実装した(`port/webgl/gl_angle.cc`)。差し込む場所は、BlinkがWebGLのコンテキストを作るときにプラットフォームに求める`WebGraphicsContext3DProvider`で、CPUの実装もここに差し込んでいる。コマンドバッファのクライアント、`ui/gl`、GPUのサービスの層(共有イメージの管理、同期点の管理、スケジューラ)はどれも要らない。

| 層 | 内容 |
| --- | --- |
| Blink | WebGLのコンテキスト、描画バッファ(`DrawingBuffer`)、拡張の扱い。変更しない |
| `gl_angle.cc`の`GLES2Interface` | 呼出しを1つずつANGLEのエントリポイントに渡す。コンテキストを切り替えるときだけ`EGL_MakeCurrent`を呼ぶ |
| ANGLE(`libGLESv2_thin_static`) | `GL_`と`EGL_`で始まる名前のエントリポイント、`libANGLE`、シェーダのトランスレータ(ESSLの出力)、OpenGL ESのバックエンド(`DisplayEGL`) |
| Mesa(`libtsgl.a`) | EGLのプラットフォーム`tessronos`と、OpenGL ES 3.1。softpipeかv3d |

- ANGLEのエントリポイントは`GL_ActiveTexture`、`EGL_Initialize`のように名前の先頭が`GL_`と`EGL_`なので、同じプログラムにリンクするMesaの`glActiveTexture`、`eglInitialize`とぶつからない。`gl`と`egl`の名前を出すANGLEのファイル(`libGLESv2_autogen.cpp`、`libEGL`)はコンパイルしない。
- `GLES2Interface`の約330のメソッドのうち、ANGLEに同じ関数がある約290は、`tools/browser/gen_gl_forward.py`がChromiumのヘッダ(`gles2_interface_autogen.h`)とANGLEのエントリポイントのヘッダから転送のコードを生成する(`gl_angle_forward.inc`、`gl_angle_entry.inc`)。Chromium独自の拡張の名前のメソッドは、ANGLE、EXT、OESなどの接尾辞の同じ関数に渡す。マルチドローの要素の位置は、バイト数の配列からポインタの配列に直す。ただし、渡す先の拡張の関数がOpenGL ES 3.0のコアにもある場合は、OpenGL ES 3のコンテキスト(WebGL 2)ではコアの関数に、OpenGL ES 2のコンテキストでは拡張の関数に渡す。インスタンス描画(`DrawArraysInstancedANGLE`、`DrawElementsInstancedANGLE`、`VertexAttribDivisorANGLE`)、頂点配列オブジェクト(`BindVertexArrayOES`など)、問合せ(`GenQueriesEXT`、`BeginQueryEXT`など)、`TexStorage2DEXT`、`UnmapBufferCHROMIUM`は生成するコードで(`gen_gl_forward.py`の`CORE_IN_ES3`)、`DrawBuffersEXT`、`BlitFramebufferCHROMIUM`、`RenderbufferStorageMultisampleCHROMIUM`と問合せの開始と終了は`gl_angle.cc`の手で書いた部分で切り替える。GPUプロセスのデコーダが使うGLの関数の表も、OpenGL ES 3ではこれらの名前をコアの関数に結び付ける。`RenderbufferStorageMultisampleEXT`は`EXT_multisampled_render_to_texture`の別の関数なので、切り替えない。ANGLEは、拡張を有効にしていないコンテキストで拡張の関数が呼ばれると`GL_INVALID_OPERATION`(`Command requires an extension.`)にする。OpenGL ES 3のコンテキストでは`EXT_draw_buffers`も`ANGLE_instanced_arrays`も有効にならないので、以前はWebGL 2の`drawBuffers`とインスタンス描画が呼ばれるたびにこのエラーが残り、描画もされなかった(14.8.1)。残りのうちコマンドバッファのクライアントが自分で答えるもの(共有イメージ、拡張の要求、フラッシュ、同期トークン、WebGL 2の`getBufferSubData`)は手で書いた。どこにも渡せないもの(コマンドバッファのトレースやGPUフェンスなど)は、最初に呼ばれたときにコンソールに名前を出し、何もしない。
- コマンドバッファ独自の問合せの対象は、ANGLEに渡さずに`gl_angle.cc`が答える。WebGL 2の`fenceSync`は、Blinkの中では`GL_READBACK_SHADOW_COPIES_UPDATED_CHROMIUM`の問合せで、`clientWaitSync`と`getSyncParameter`は、その結果が出たかを尋ねる。`KHR_parallel_shader_compile`の`COMPLETION_STATUS_KHR`も、`linkProgram`を挟んだ`GL_PROGRAM_COMPLETION_QUERY_CHROMIUM`の問合せである。以前はこれらもANGLEに渡していたが、ANGLEはこの対象を知らないので問合せは始まらず、結果はいつまでも出ず、尋ねるたびに`getError`に`INVALID_OPERATION`が残った。ページのフェンスは信号状態にならず、リンクは終わらなかった。Googleマップの地図の部品(`mapcore`)は、描いた画素を`PIXEL_PACK_BUFFER`に読み出してフェンスを置き、`clientWaitSync(sync, 0, 0)`をタイマで繰り返して読み出しの終わりを待つ。その待ちが終わらず、地図のフレームの完了と表示の安定を待つ処理(マップが`ActionFlow error: hung, waiting for done(). Branches: stableViewport flowType frame_complete_event`と報告する)が止まり、続く初期化と、左上のメニューのパネルを出す処理も止まった。いまは、フェンスの問合せは終わるときにANGLEのフェンス(`glFenceSync`)を置き、それが信号状態になったら結果が出たと答える(OpenGL ES 2のコンテキストにはWebGLのフェンスがないので、フラッシュした時点で出たとする)。リンクの問合せは終えた時点で出たとし、結果はそのプログラムのリンクの状態にする(ANGLEはリンクの状態を尋ねるとリンクの終わりを待つ)。
- ANGLEのディスプレイは、OpenGL ESのバックエンド(`EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE`)を、システムのEGLをデバイスとして(`EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE`)使う形で、プロセスに1つ作る。ANGLEはシステムのEGLを`dlopen`で開いて`eglGetProcAddress`を探す。ブラウザの`dlopen`(`port/compat/ts_linux.c`)は、名前に`EGL`を含むものを開くと、リンクしたMesaを指す値を返し、`dlsym`はMesaの`eglGetProcAddress`を返す。以降のEGLとOpenGL ESの関数はすべてそれで得る。MesaのEGLは既定のディスプレイでプラットフォーム`tessronos`になり、ピクセルバッファとサーフェスなしのコンテキストを持つ。Mesaの`dlopen`など(`lib/libgl/gl_os.c`)は弱い名前なので、ブラウザのものが使われる。
- コンテキストはプロバイダ(`WebGraphicsContext3DProvider`)ごとに1つで、WebGL 1はOpenGL ES 2.0、WebGL 2は3.0のコンテキストである。WebGL向けの属性(`EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE`)を付けて作るので、ANGLEがWebGLの規則で呼出しを検査し、シェーダを変換する。初期化されていないリソースは0で埋める(`EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE`)。描画は1×1のピクセルバッファを現在のサーフェスにして行い、描画先はいつもフレームバッファオブジェクトである。コンテキストを作ったとき、コマンドバッファのクライアントが自分のために使う拡張(フレームバッファのブリット、テクスチャのストレージ、テクスチャのコピー、`OES_packed_depth_stencil`など。GPUプロセスのデコーダが要求するものと同じ一覧)のうち、ANGLEが出せるものを要求する。OpenGL ES 3のコンテキストでは`OES_packed_depth_stencil`はコアの機能なので、拡張の文字列にないときは足して答える。BlinkはWebGLのコンテキストを作るときにこの名前を確かめる。
- ANGLEがエラーを記録したときの説明(どの関数か、なぜか)は、コンテキストを作るときに`KHR_debug`のコールバック(`GL_DebugMessageCallbackKHR`)で受け取り、`[.WebGL-<通し番号>]GL_INVALID_OPERATION: glDrawBuffersEXT: Command requires an extension.`の形にしてコンテキストのエラーメッセージのコールバック(`SetErrorMessageCallback`)に渡す。Blinkはそれをページのコンソールに警告として出す(コンテキストごとに256件まで)。同じ説明はシステムのコンソールにも、違う説明ごとに最初の1回だけ`browser: webgl:`の行で出す。ページが`getError`で受け取ったエラーを、どの呼出しが残したかをこれで確かめる。
- `EXT_disjoint_timer_query`と`EXT_disjoint_timer_query_webgl2`はページに出さない。Blinkは、GPUの機能の情報(`GpuFeatureInfo`)で`ENABLE_WEBGL_TIMER_QUERY_EXTENSIONS`が有効なときだけこれを承認済みの拡張とし、このブラウザのプロバイダはそれを有効にしていないので、開発者向けの拡張のまま隠れる。LinuxのChromeはこれを有効にしている(Androidを除く)ので、Chromeでは出る。出すと、時間の問合せはANGLEを通ってMesaの`EXT_disjoint_timer_query`に渡る。softpipeは持つが、v3dでは確かめていないので、実機で確かめてから有効にする。
- コンテキストは、それを描くcanvasのスレッドで使う。ページのcanvasならページのスレッド、ワーカーの`OffscreenCanvas`ならワーカーのスレッドである。ワーカーのコンテキストは、Blinkがページのスレッドに作らせて待つので、作ったスレッドと使うスレッドが違う。EGLのコンテキストは同時に1つのスレッドでしか現在のものにできないので、コンテキストは作る間だけ現在のものにし、作り終えたら手放す。使うスレッドが最初の呼出しで現在のものにする。どのコンテキストが現在のものかはスレッドごとに、コンテキストの通し番号で覚える(アドレスで覚えると、別のスレッドで捨てられたコンテキストのアドレスに新しいコンテキストが作られたとき、取り違える)。以前はプロセスで1つしか覚えず、ワーカーのスレッドはページのスレッドで現在のものになったコンテキストをそのまま使えると見なしていたので、ワーカーの`getContext('webgl')`はnullを返した。
- `transferControlToOffscreen`でワーカーに渡したcanvasのフレームは、ワーカーの`OffscreenCanvas`が描くたびに、コンテキストのスナップショットとしてページに渡す(パッチ`offscreen_canvas_frame.patch`と`html_canvas_placeholder.patch`)。Chromiumでは、`OffscreenCanvas`はフレームをGPUプロセスの共有イメージ(GPUで合成しないときはソフトウェアのコンポジタの共有イメージ)としてコンポジタに渡し、ページのcanvas(プレースホルダ)はそれをレイヤーとして表示する。このブラウザはコンポジタもその共有イメージのインタフェースも持たないので、WebGLはフレームを作れず(`GetResourceForPushFrame`がいつもnull)、作れたWebGPUのフレームもページの描画では描かれなかった(`HTMLCanvasElement::Paint`はプレースホルダを描かない)。いまは、`OffscreenCanvas`のフレームごとに、WebGLは描画バッファの読み戻し、WebGPUは今のテクスチャの画素(14.6.5)、ほかはコンテキストの画像を、ラスタの`SkImage`にしてプレースホルダの番号で控え、ページのスレッドに知らせる。WebGLは、フレームを渡した後でコンポジタに渡したときと同じく合成済みとし(`MarkLayerComposited`)、描画バッファを保たない設定なら次のフレームの描画の前に消す。WebGLの読み戻しは毎回描画バッファから読む(`GetRGBAUnacceleratedStaticBitmapImage`)。WebGLのコンテキストが資源の作り手のないときのために持つスナップショット(`PaintRenderingResultsToSnapshot`の`cached_snapshot_`)は、1度作ると大きさが変わるまで作り直さないので、それを使った最初の版では、2枚目からのフレームがすべて1枚目のままになり、実機のGoogleマップは地図を描いても、ドラッグやズームが画面に出なかった。ページのスレッドはそのcanvasの描き直しを求め、ページを描くときにその画像をcanvasの大きさに描く。Googleマップは地図をこの形で描く(14.8.1)。
- コンテキストごとに、5秒の間の呼出しの数、描画と消去の数、描画バッファから読んだフレームの数、`readPixels`の数(メモリとピクセルのパックのバッファ。メモリなら先頭の4バイト)、`getBufferSubData`の数と先頭の8バイト、フェンスの数と、その問合せに「終わった」「まだ」と答えた数、ページに返したエラーの数、色のバッファの大きさを数え、使われたコンテキストについて5秒ごとに1行コンソールに出す。最初のWebGL 1と2のコンテキストを作ったときは、限界値(テクスチャの大きさ、ユニフォームとバリイングの数、サンプル数、描画バッファの数、ユニフォームブロックの大きさなど)とフラグメントシェーダの`highp`の精度、拡張の一覧を1回ずつ出す。実機のV3DとQEMUのsoftpipeでページが選ぶ描き方が違うとき、どの値が違うかをこの行で比べる。
- ANGLEのOpenGL ESのバックエンドは、仮想化のグループ(`EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE`)が同じコンテキストを、システムのEGLの1つのコンテキストの上で切り替えて動かす。指定しなければディスプレイの全部のコンテキストが1つのグループになる。システムのコンテキストも同時に1つのスレッドでしか現在のものにできないので、ワーカーとページが同時にWebGLを使うと、互いにシステムのコンテキストを取り合い、システムの`eglMakeCurrent`が失敗して、呼出しが別のコンテキストの状態の上で動いた。新しく作ったコンテキストが返す文字列が不正なアドレスになり、ブラウザが落ちた(ワーカーの`OffscreenCanvas`とページのcanvasで同時にWebGLを使う試験のページで再現した。Google マップを`gpu=on`で開いたときも、同じ所で落ちていた)。そこで、ページのスレッドのコンテキストはディスプレイのグループに、ほかのスレッドのコンテキストはスレッドごとのグループに入れる。ワーカーのコンテキストはページのスレッドで作られるので、ワーカーのスレッドで最初に現在のものにするときに、そのスレッドのグループで作り直す(まだ何も描いていないので、要求した拡張を求め直すだけでよい)。システムのコンテキストは、ページのスレッドに1つ、WebGLを使うワーカーのスレッドごとに1つになる。softpipeではシステムのコンテキスト1つが約200MBを使うので、QEMUの2GBのマシンで`gpu=on`(Skiaのコンテキストがもう1つ)のままWebGLを使うワーカーのあるサイトを開くと、メモリが足りなくなることがある(4GBでは足りる)。`EGL_MakeCurrent`が失敗したときは、コンソールに1度`browser: webgl: a context could not be made current`を出し、どのコンテキストも現在のものにしないでおく(呼出しはコンテキストのないものとしてANGLEが答える)。
- 描画バッファの色のバッファは共有イメージとして作られる。共有イメージのインタフェースは作られたときの大きさをメールボックスごとに覚え、`CreateAndTexStorage2DSharedImageCHROMIUM`がその大きさのRGBA8のテクスチャをANGLEに作る(`ColorBuffer`)。共有イメージへのアクセスの開始と終了、同期トークンは、呼出しが返ったときには実行が済んでいるので、何もしない。
- 描いた結果は、合成をCPUで行う設定(`IsGpuCompositingDisabled`)により、描画バッファが`ReadPixels`で読み戻してcanvasのビットマップにし、Skiaがページに描く。ページをGPUでラスタ化するとき(14.7.2)は、Ganeshがそのビットマップをテクスチャにして描く。GPUから読み戻すので、v3dではキャッシュしないマッピングのBOを読むことになる。描画バッファ全体を原点から読む`ReadPixels`は表示のためのフレームとして数え、コンソールの`browser: draw: webgl`の行(14.7.2)に出す。後で、色のバッファをMesaのBOにしてウインドウシステムに渡し、GPUで合成する(13.10)ときは、色のバッファを作る1か所(`ColorBuffer`。BOのEGLイメージをテクスチャにする)と、描画バッファの出力(読み戻しをやめて共有イメージのまま渡す)を変える。転送の部分は変わらない。
- softpipeはマルチサンプリングを持たず、`GL_MAX_SAMPLES`を1としたままOpenGL ES 3.1を出す(Mesaのソフトウェアのマルチサンプリングの扱い)。ANGLEは、OpenGL ES 3.0の必須の形式が4サンプルを持たないとOpenGL ES 2.0に制限するので、そのままではsoftpipeでWebGL 2ができない。パッチ(`angle_es3_samples.patch`)で、システムのドライバ自身がOpenGL ES 3以上を出しているときはこの制限をしないようにした。v3dは4サンプルを持つので、この変更は効かない。softpipeのWebGL 2で`renderbufferStorageMultisample`に2以上のサンプル数を与えるとエラーになる。描画バッファのアンチエイリアスは使わない(`kAntialiasingModeNone`)。
- ANGLEのソースのうち、gccで通らない1か所(`IntermNode.cpp`の配列の大きさ)はパッチ(`angle_intermnode.patch`)で直した。X11、Vulkan、WebGPU、NULLのバックエンド、libpci、HLSL・MSL・WGSL・SPIR-Vの出力はコンパイルせず(`tools/browser/exclude_angle.txt`)、そこを指す名前は`blink_stubs.py`のスタブになる。どれも呼ばれない。ANGLEがGPUの情報を集めるときにPCIのデバイスを探す関数は、`gl_angle.cc`が「PCIのデバイスはない」と答える。ANGLEがDRMの`drmGetVersion`を呼ぶ場所(デバイスファイルを開いて名前を見る)は、ファイルが開けないので通らない。そのヘッダは`port/shim/xf86drm.h`である。
- 古いCPUの実装は残す。`GL=1`でないブラウザ(Mesaをリンクしない)では、WebGL 1をCPUで描くのに使い、WebGL 2はない。`GL=1`でも、ANGLEのディスプレイができなかったとき(システムのEGLが初期化できないときなど)は、WebGL 1をこれで描く。ANGLEのコンテキストのプロバイダは弱い名前で参照し(`br_blink.cc`の`BrWebGLProvider`)、リンクされていないときは呼ばない。

| ファイル | 内容 |
| --- | --- |
| `application/browser/port/webgl/gl_angle.cc` | ANGLEの上の`GLES2Interface`、共有イメージ、コンテキストのプロバイダ |
| `application/browser/port/webgl/gl_angle_forward.inc`、`gl_angle_entry.inc` | 生成した転送のコードと、ANGLEの拡張のエントリポイントの宣言 |
| `tools/browser/gen_gl_forward.py` | 上の2つを生成する |
| `tools/browser/labels_angle.txt`、`exclude_angle.txt` | コンパイルするANGLEのgnのラベルと、外すソース |
| `tools/browser/patches/angle_intermnode.patch`、`angle_es3_samples.patch` | ANGLEへのパッチ |
| `application/browser/port/compat/ts_linux.c` | システムのEGLを返す`dlopen`と`dlsym` |
| `application/browser/port/compat/ts_libc.c` | Cライブラリの浮動小数点数と文字列の変換を一度に1つだけ動かすラッパ(下) |

ワーカーとページのスレッドが同時にWebGLを使うと、両方のスレッドで数値と文字の変換(`printf`の`%g`や`strtod`。ANGLEのシェーダの変換もこれを使う)が同時に動く。プログラムのCライブラリ(newlib)は、スレッドごとの作業域を持たないビルドで、変換に使う大きな整数の空きリストとdtoaの直前の結果をプログラム全体で1つ持つので、2つのスレッドが同時に変換すると空きリストの中身がおかしくなり、`_Balloc`で落ちた。ブラウザは、変換の入口(`_dtoa_r`、`_ldtoa_r`、`_strtod_r`、`_strtod_l`、`strtod`、`strtod_l`、`strtof`、`strtof_l`、`_strtodg_l`)をリンカの`--wrap`で包んでリンクする(`tools/browser/build.sh`)。包んだ関数は1つのロックを取って本物を呼び、dtoaの結果は、ロックを放す前に呼んだスレッドの領域にコピーして返す(`printf`はその後ろに精度の分まで0を書き足すので、その分の大きさを取る)。包まないでリンクするプログラム(`v8prog`など)では、包んだ関数は呼ばれず、`--gc-sections`で捨てられる。

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

ANGLEのディスプレイの前に、Mesaのプラットフォームが`egl:`の行でV3Dを使うかどうかを言う(第13章13.8.3)。`egl: the V3D (v3d), ...`なら、WebGLの行の括弧の中は`Broadcom, V3D 7.1.7.0, OpenGL ES 3.0`になる。V3Dの確認が通らなかったときと、V3Dのないマシンでは、softpipeで描き、ページは同じに表示される。起動の行`gpufake=<パス>`は、DRMシムに実機のV3D(x64ではKaby Lake GT2のIntelのGPU)を模擬させ(ジョブは描かずに終わる)、ローダからv3dドライバ(x64ではiris)の画面までをQEMUで通してから確認の失敗でsoftpipeに戻す。終わりにプラットフォームの選んだドライバと進んだ段階(`driver=v3d step=6`、x64では`driver=iris step=6`)をそのパスに書く(`test_blink_v3dfall`)。irisはi915の要求をCライブラリの`ioctl`で出すので、ブラウザの`ioctl`(`port/compat/ts_linux.c`)はGPUの記述子の要求をDRMシムの`ts_gpu_fd_ioctl`に回す(第13章13.11.9)。

`GLES2Interface`のANGLEへの呼出しは、どれも時間を測り(`gl_angle.cc`の`Timed`)、スレッドごとに足す。エンジンのスレッドの1回りの内訳(14.7)の`WebGL`がこの時間である。

### 14.6.2 メディア要素のプレーヤとMedia Source Extensions

Chromiumでは、メディア要素のプレーヤ(`blink::WebMediaPlayerImpl`)が`content`層の部品(フレームのクライアント、コンポジタ、mojoのサービス)の上でパイプラインを動かす。このブラウザは`content`層を使わないので、要素とパイプラインをつなぐ部分を`port/media`に書いた。パイプラインとその部品はChromiumのものをそのまま使う。

| 部品 | 内容 |
| --- | --- |
| `BrWebMediaPlayer`(`media_player.cc`) | `blink::WebMediaPlayer`の実装。`media::PipelineController`と`media::PipelineImpl`を作り、再生、一時停止、シーク、再生の速さ、音量を渡し、準備の状態、時刻、長さ、大きさ、持っている時刻の範囲、復号と落としたフレームの数を要素に答える |
| メディアのスレッド | パイプライン(デマルチプレクサ、レンダラ、デコーダの呼出し)は、すべてのプレーヤが共有する1つのスレッド(`base::Thread`の`media`)で動く。デマルチプレクサ、レンダラが使う部品、ログは、パイプラインが止まった後にこのスレッドで捨てる |
| レンダラ | `media::RendererImplFactory`が作る`media::RendererImpl`と、`media::DefaultDecoderFactory`のデコーダ。CDMが付くと、暗号化されたストリームの前に`DecryptingDemuxerStream`が入る |
| 音の出力(`br_audio_sink.cc`) | 音のレンダラの出力先(`media::AudioRendererSink`)。48kHz、ステレオのデバイスとして答え、自分のスレッドで、ブラウザの音の混合(`br_mix.cc`)の自分の流れとサウンドデバイスのリング(`br_snd.cc`)に残る分が0.1秒分より減るたびに1回分をレンダラに求め、16bitにして書く。求めるときに、残っている分が聞こえるまでの時間を遅れとして渡し、レンダラはそれで時計を合わせる。音を出せないときはリングが時刻どおりに捨てられるので(14.6)、時計はそのまま進む |
| 映像の出力(`BrVideoSink`) | 映像のレンダラの出力先(`media::VideoRendererSink`)。アプリケーションのループのたびに、その時刻に見せるフレームをレンダラに求め、変わっていれば要素を描き直させる。描くときは、そのフレーム(`media::VideoFrame`)を、YUVの平面を出すジェネレータ(`BrFrameGenerator`)を持つ画像として要素の矩形に描く記録を作る(`PaintFrame`)。ページをGPUでラスタ化するときは、Ganeshが平面をテクスチャにしてシェーダでRGBにし(14.7.2)、プロセッサでラスタ化するときは、ジェネレータがlibyuvでRGBにする。平面を出せるのは8bitと10bitのI420、I422、I444で(10bitの扱いは14.6.7)、回転か反転のあるフレームとほかの形式は、今までどおり`media::PaintCanvasVideoRenderer`がプロセッサでRGBにする。同じフレームを描き直すときは同じ画像を使うので、GPUのテクスチャも使い回す。canvasやWebGLが要素を描くときは、フレームを`PaintCanvasVideoRenderer`に渡す |
| コントロール | `controls`の要素には、Blinkの標準のコントロール(`MediaControlsImpl`、`modules/media_controls`)をシャドウツリーとして付ける。再生と一時停止、シークバー、音量、ピクチャインピクチャなどのボタンは、ページの要素として描かれ、押した操作もページのイベントとして届く |
| デマルチプレクサ(`media_demuxer.cc`) | アドレスで指定されたリソースは、ネットワークスタックで丸ごと取得し、`media::MemoryDataSource`から`media::FFmpegDemuxer`で読む。FFmpegのヘッダに届くメディアのライブラリのフラグでコンパイルするため、別のファイルにした |
| ログ | パイプラインの`media::MediaLog`のエラーと警告をコンソールに出す。プレーヤは作られた順の番号を持ち、パイプラインの開始、再生、一時停止、シーク、レンダラの`BUFFERING_HAVE_ENOUGH`と`BUFFERING_HAVE_NOTHING`、終わりと、再生中または再生を待つ間は5秒ごとに、時刻、要素の準備の状態、パイプラインの状態(止まっている、シーク中、休止、安定)、持っている時刻の範囲、表示しているフレームの時刻、表示したフレームの数、復号したフレームと落としたフレームの数、音のリングの状態(デバイス、デバイスなし、止まっている、リングとデバイスに残る量、デバイスが取った量、捨てた量)を1行でコンソールに出す(`browser: media <番号>:`)。実機で再生が止まったときは、この行で、時刻と範囲、フレーム、音の出力のどこが止まったかを見る |

YouTubeなどの動画のサイトのプレーヤは、動画を数秒ずつの断片に分けて取得し、Media Source Extensions(MSE)の`MediaSource`と`SourceBuffer`で要素に渡す。`MediaSource`が要素に付くと、プレーヤは`media::ChunkDemuxer`を作ってパイプラインを始め、デマルチプレクサが開いた時点で、Chromiumの`blink::WebMediaSourceImpl`を要素に渡す。これで`sourceopen`が来る。BlinkのMSEの部分(`modules/mediasource`の18のソースと11のバインディング)と`platform/media`の`WebMediaSourceImpl`、`WebSourceBufferImpl`は、そのままコンパイルする(`blink_modules_src.txt`、`blink_modules.txt`)。バイト列の解析(MP4とWebMの`StreamParser`)、時刻の範囲ごとのフレームの管理と隙間のあるシーク(`SourceBufferStream`)、`remove`、`abort`、`timestampOffset`、追加のウインドウ、`sequence`のモード、`changeType`、画質の切り替えで足される新しい初期化セグメント、`endOfStream`は`ChunkDemuxer`の仕事で、Chromiumと同じに動く。`URL.createObjectURL(mediaSource)`のレジストリ(`MediaSourceRegistryImpl`)は、modulesの初期化(`ModulesInitializer`)を通らないので、バインディングの準備(`br_bindings.cc`)で作る。

`MediaSource.isTypeSupported`、`canPlayType`、MediaCapabilitiesの`decodingInfo`は、Chromiumのメディアの判断(メディアのクライアントがないときの既定)のままに答える。`encodingInfo`は、`webrtc`の型をWebRTCのエンコーダの判断で答え(なめらかさは、ブラウザの側の`WebrtcVideoPerfHistory`がプロセッサで間に合う大きさと速さか(854×480、毎秒31フレームまで)で答える)、`record`の型はChromeと同じく試験中の機能として受け付けない(`TypeError`)。H.264、AAC、VP8、VP9、Opus、AV1などは受け付け、HEVCはデコーダを持つマシン(Raspberry Pi 5)でだけ受け付ける(14.6.7)。AV1は、LinuxのChromeと同じく、ビルドの記述でデコーダを持つ設定(`enable_av1_decoder = true`。dav1dの`ENABLE_DAV1D_DECODER`は、AVIFの画像にも使う)にする。これで、AV1を扱えると答える所(`media/base/supported_types.cc`、`media/base/mime_util_internal.cc`、`media/filters/stream_parser_factory.cc`)と、AV1を読む所(MSEのMP4のパーサが`av01`のサンプルの記述と`av1C`を読む`media/formats/mp4/box_definitions.cc`、WebMのパーサの`media/formats/webm/webm_video_client.cc`、FFmpegのデマルチプレクサのプロファイルの`media/ffmpeg/ffmpeg_common.cc`)が、同じフラグに従う。扱えると答える所だけをdav1dに従わせると、YouTubeはMSEでAV1のMP4を選び、パーサが`av01`を読めずに追加が失敗して再生が止まる。メディア要素、MSE、WebCodecsの`VideoDecoder`は`DefaultDecoderFactory`のdav1dのデコーダ(`OffloadingDav1dVideoDecoder`)で復号する。なめらかさは下の範囲で答えるので、1080pのAV1は`smooth`が偽になり、YouTubeのようなサイトは大きいものにはほかのコーデックを選ぶ。AV1の符号化は、Chromiumのソースツリーのlibaomをコンパイルしてリンクし、WebCodecsの`VideoEncoder`とMediaRecorder(`video/webm;codecs=av01`)が使う。libaomの内積と行列積の命令の行(`_neon_dotprod.c`、`_neon_i8mm.c`)は、命令を許す`-march`でコンパイルする(`flags.txt`。libaomはプロセッサが持つと答えたときだけ呼び、Cortex-A76は内積の命令を持つ)。SVEの行はコンパイルせず、プロセッサがSVEを持たないので呼ばれない。映像はCPUで復号するので、パッチ`media_source.patch`で、`isTypeSupported`の型の引数に大きさとフレームレート(`width=`、`height=`、`framerate=`)があるとき、854×480の画素数と毎秒31フレームを超えるものを断る(`br_media_source_size_ok`)。パッチ`media_capabilities.patch`で、`decodingInfo`の`smooth`をその範囲で真、`powerEfficient`を常に偽にする。Chromiumは`decodingInfo`の答えをブラウザのプロセスの復号の記録(`VideoDecodePerfHistory`)に尋ねるが、そのサービスはないので、尋ねずに答える。専用ワーカーの中で作った`MediaSource`も扱う(`canConstructInDedicatedWorker`は真)。ページはその`handle`(`MediaSourceHandle`)をウインドウに送って要素の`srcObject`にし、Blinkのスレッドをまたぐ接続(`CrossThreadMediaSourceAttachment`)が、ワーカーの`SourceBuffer`をウインドウの側のプレーヤの`ChunkDemuxer`につなぐ。

Raspberry Pi 5の実機でYouTubeの動画を開くと、広告の1本目の映像が数秒動いて止まり、2本目(実は止まった1本目をプレーヤが新しい`MediaSource`で読み直したもの)は最初のフレームのまま動かなかった。YouTubeが送る再生の統計(`watchtime`の`cmt`)では、要素の時刻は最初から0.277秒で止まり、15秒分のデータを持ったまま待ちになっていた。デバイスがサンプルを取らなくなり、音のリングが満ちたまま、音のレンダラの時計が書いた分の終わりで止まったためである(0.277秒は、デバイスに渡せる7200フレームと出力側が最初に取る4096フレーム、合わせて0.235秒に、リングに残る分を足した量に当たる)。出力のRP1のI2Sは、連結ブロックの有効フラグをコントローラが書き戻しで落とすのを付け直していなかったので、DMAが1周(85ミリ秒)で止まったと考えられる。映像のレンダラは最後に聞こえた時刻から先を推し量って数秒進み、そこで止まった。読み直したプレーヤは、満ちたリングのために音を一度も求められず、時刻0のままだった。直しは、I2Sの出力で済んだブロックに有効フラグを付け直し、動かないDMAを再開させること(状態のレジスタをコンソールに出す)と、ブラウザで出力の止まったデバイスをリングを捨てて扱うこと(14.6)の2つである。QEMUでは音の出力(USBオーディオ)がサンプルを取り続けるので、広告2本と本編が再生されていた。

動画のピクチャインピクチャ(`requestPictureInPicture`)は、ブラウザの別のウインドウに動画のフレームを描いて扱う(14.7.8)。

`requestVideoFrameCallback`は、映像の出力が新しいフレームを要素に渡したときに、待っているページに通知する。コールバックに渡す情報は、そのフレームを出した時刻(見せる時刻はその16ミリ秒後とする)、それまでに出したフレームの数、フレームの大きさとメディアの時刻、フレームの間隔の平均である。

要素の`srcObject`に`MediaStream`(canvasの`captureStream`、WebRTCの相手のストリームなど)を与えると、プレーヤはパイプラインを使わない。最初の映像のトラックにシンク(`blink::MediaStreamVideoSink`)を付け、来たフレームを映像の出力にそのまま渡す。最初の音のトラックのシンク(`blink::WebMediaStreamAudioSink`)は、要素が再生している間、来た音を48kHzのステレオの16bitにし、音量を掛けて音の混合の自分の流れに書く。WebRTCの相手の音のトラックは、シンクでなくWebRTCの音のレンダラで流す(14.5.2)。最初のフレームが来ると(映像がなければすぐに)、要素は再生できる状態になる。長さは無限、時刻は再生を始めてからの経過である。

ブラウザの中の音の出どころ(メディア要素のパイプライン、`MediaStream`のトラック、WebRTCの相手の音、`AudioContext`、音声の合成)は、それぞれがブラウザの音の混合(`port/media/br_mix.cc`)の1つの流れになり、1つのスレッドがそれらを足し合わせてサウンドデバイスのリング(`br_snd.cc`)に書く。リングはサウンドデバイスの1つのチャネルで、書かれた順に再生するので、以前のようにそれぞれが直接リングに書くと、2つが同時に鳴ったときに10ミリ秒ずつ交互に詰め込まれ、どちらも半分の速さで途切れて聞こえた(会議のページで、相手の声とページの効果音が重なるときなど)。流れは48kHz、16bitのステレオのフレームの列(1秒分まで)で、書き手はそれまでと同じく、自分の流れとリングに残る分(`br_mix_buffered`)を見て書く。混合のスレッドは、リングが50ミリ秒分より減るたびに10ミリ秒分を作る。流れの持つ分を足し、何も持たない流れ(一時停止、終わり、始まる前)は無音とする。10ミリ秒分に満たない流れは、リングに20ミリ秒分より多く残っている間は待つので、少し遅れた書き手が途切れとして聞こえない。どの流れも何も持たないときは書かず、リングはそれまでどおり空になってチャネルを止める。足した値は16bitの範囲に切る。

### 14.6.3 Encrypted Media Extensions

`navigator.requestMediaKeySystemAccess`、`MediaKeys`、`MediaKeySession`、要素の`setMediaKeys`と`encrypted`イベントは、BlinkのEMEの部分(`modules/encryptedmedia`の12のソースと17のバインディング)と、`platform/media`のCDMの部分(`WebEncryptedMediaClientImpl`、`WebContentDecryptionModuleImpl`、`CdmSessionAdapter`、`KeySystemConfigSelector`など)をそのまま使う。BlinkはEMEのエントリポイント(`blink::WebEncryptedMediaClient`)をフレームのクライアントに求めるが、このブラウザのフレームはそれを持たないので、パッチ`encrypted_media.patch`で`port/media/eme_tessronos.cc`のものを返す。それは、キーシステムの一覧(`media::KeySystemsImpl`)、CDMのファクトリ、許可(暗号化されたメディアは許し、マシンを識別する値は渡さない)を持つ。キーシステムは、組み込みのClear Key(`org.w3.clearkey`)と、利用者がWidevineのCDMを導入したときのWidevine(`com.widevine.alpha`、下記)である。CDMのファクトリは、Clear Keyにはメディアのライブラリの`AesDecryptor`を作り(`media::DefaultCdmFactory`)、WidevineにはライブラリのCDMのインタフェース(`media::CdmAdapterFactory`)で`media::CdmAdapter`を作る。

`KeySystemsImpl`は、メディアのクライアント(`media::MediaClient`)があるときだけ、Clear Key以外のキーシステムを登録のコールバック(`GetSupportedKeySystemsCB`)で尋ねる。そのため`eme_tessronos.cc`はメディアのクライアント(`BrMediaClient`)を置く。キーシステムを答えるほかは、型の可否(`IsDefaultDecoderSupportedVideoType`など)をクライアントがないときと同じに答えるので、再生できる型は変わらない。登録のコールバックは覚えておき、WidevineのCDMを導入したときと外したときに、新しい一覧でもう一度呼ぶ。ブラウザを起動し直さなくても、次の`requestMediaKeySystemAccess`から答えが変わる。

要素に`MediaKeys`が付くと、プレーヤはそのCDMのコンテキストをパイプラインに渡し(`SetCdm`)、パイプラインは暗号化されたストリームを復号してからデコーダに渡す。パイプラインが始まる前に付いたときは、始まったところで渡す。CENC(`cenc`)のMP4とWebMの暗号化を扱う。

EMEは安全なコンテキスト(`https`のページ)にだけある。YouTubeの普通の動画は暗号化されていないので、Widevineがなくても再生できる。PlayReadyなど、ほかの独自のCDMのキーシステムは持たない。

#### 14.6.3.1 利用者が導入するWidevine

WidevineのCDM(`libwidevinecdm.so`)はGoogleの独自のソフトウェアで、TessronOSにもブラウザにも同梱しない。リポジトリにも入れない。利用者が自分で導入したときだけ使える。LinuxのディストリビューションがChromiumに対して行うのと同じく、Raspberry Piの公式リポジトリ(<https://archive.raspberrypi.com/debian>)がRaspberry Pi OSに配っているパッケージ`libwidevinecdm0`(arm64)を、その配布の形のまま使う。CDMの中身は書き換えず、調べもしない。Chromiumの正規のインタフェースで読み込んで使うだけである。導入していなければ、`requestMediaKeySystemAccess('com.widevine.alpha', ...)`は従来どおり`NotSupportedError`で拒否される。

導入はメニューの〈Widevineを導入...〉で行う。ブラウザはまずデータボックスのパネル7「Widevineの導入」(14.8)を出す。パネルは、WidevineがGoogleの独自のソフトウェアでTessronOSに同梱していないこと、入手元(Raspberry Piの公式リポジトリ)、利用者の実身に入れること、利用の条件は導入する前に利用者が確かめることを示す。〈導入〉を選んだときだけ取りに行き、〈取消〉とEscでは何も取らない。取得と展開は別のスレッドで行い(`port/media/br_widevine.cc`)、その間はページを使える。手順は次のとおりである。

1. リポジトリのインデックス`dists/<配布>/main/binary-arm64/Packages`をHTTPSで取る。配布は`trixie`、なければ`bookworm`の順に見る。インデックスのうち`Package: libwidevinecdm0`で`Architecture: arm64`のものから、Debianのバージョンの順序(dpkgと同じ比べ方)で最も新しいものを選び、`Filename`、`Size`、`SHA256`を読む。`Filename`は`pool/`で始まり、`..`を含まないものに限る。
2. パッケージ(`.deb`)をHTTPSで取り、大きさとSHA-256(BoringSSL)をインデックスの値と照らす。合わなければ何も入れない。転送でHTTPSでない所へ移されたときも取りやめる。
3. `.deb`(arのアーカイブ)の`debian-binary`が`2.x`であることを確かめ、`data.tar.xz`をLZMA SDKのXZのデコーダ(Chromiumのチェックアウトの`third_party/lzma_sdk`。`tools/browser/build.sh`の`xz_lib`)で展開する。tarのアーカイブ(ustar、GNUの長い名前とpaxのパスを読む)から、`opt/WidevineCdm/`の下の`libwidevinecdm.so`と`opt/WidevineCdm/manifest.json`を取り出す。
4. `manifest.json`を、Chromiumの`components/cdm/common/cdm_manifest.cc`の`IsCdmManifestCompatibleWithChrome`と`ParseCdmManifest`で確かめる。このブラウザが話すCDMのモジュール、インタフェース、ホストのバージョン(`x-cdm-module-versions`、`x-cdm-interface-versions`、`x-cdm-host-versions`)を名乗らないCDMは入れない。
5. 利用者ごとの普通の実身「Widevine CDM」を「ブラウザの保存域」と同じボリュームに作る。所有者はブラウザが代理するユーザ(`ob_get_crd`)、モードは`rw-------`で、レコード0にこの実身が何かを書いた文章、レコード1にライブラリ、レコード2にマニフェスト、レコード3にバージョン、入手元、SHA-256を持つ。新しい箱は作らない。「ブラウザの鍵」(14.4.7.1)と同じく、どの仮身からも指されないので固定(`ob_lnk_obj`)を1つ付ける。
6. エンジンのスレッドで、「ブラウザの保存域」のレコード10のそのユーザの行(`<ユーザのUUID> <実身のUUID>`)を新しい実身に向け、前に導入した実身があれば消す。続けてキーシステムの一覧を作り直す。結果はメッセージ行に出し、導入できなかったときは理由をパネル1で通知する。

メニューの〈Widevineを外す〉(導入していないときは灰色)は、パネル2で確かめてから、そのユーザの行と実身「Widevine CDM」を消し、キーシステムの一覧からWidevineを除く。

CDMの読み込みは、最初にページがEMEを使ってキーシステムの一覧を作るときに行う。レコード10からそのユーザの実身を引き、マニフェストを上と同じく確かめてから、ライブラリのレコードをメモリに読み、`port/compat/ts_so.c`でプロセスに読み込む。これはLinuxの動的リンカと同じく、aarch64のELFの共有オブジェクトのセグメントを置き、再配置(RELR、RELA、PLT)を当て、GNU Cライブラリの関数の名前を`port/compat/ts_glibc.c`の肩代わりの関数にリンクし(求めるバージョンはGNU Cライブラリのバージョンでなければならないが、動的リンカと同じく、弱いとマークされたバージョンの要求は満たせなくてもよく、文字列表の文字列は`DT_STRSZ`の外でもオブジェクトの中にあれば読む)、スレッドローカルストレージをTLSの記述子で与え、初期化の関数を呼ぶ。名前は`libwidevinecdm.so`で、`dlopen`(`port/compat/ts_linux.c`)はこの名前で読み込んだものを返す。そのうえでメディアのライブラリの`media::CdmModule`が`dlopen`で開いてエントリポイント(`InitializeCdmModule_4`、`CreateCdmInstance`、`GetCdmVersion`など)を引き、初期化する。コンソールには`browser: widevine: CDM <CDMのバージョン> loaded (manifest <バージョン>)`と出る。読み込みは1回の起動に1度で、外した後もプロセスが終わるまでメモリに残る(一覧には出さない)。

キーシステムの記述は、Chromiumの`components/cdm/renderer/widevine_key_system_info.cc`の`cdm::WidevineKeySystemInfo`をそのまま使う。エンジンはWidevineを同梱しない構成(`enable_widevine`が偽)でビルドしているので、このソースはそのままではコンパイルできない。`port/media/widevine_ks_tessronos.cc`がその翻訳単位の中だけ`ENABLE_WIDEVINE`を真にして取り込む。コーデックと暗号化の方式はマニフェストのもの(`x-cdm-codecs`)で、ロバストネスはソフトウェアのもの(L3)だけである。音声は`SW_SECURE_CRYPTO`まで、映像は`SW_SECURE_DECODE`まで、ハードウェアで守るデコーダと`HW_SECURE_*`はない。セッションは一時的なものだけで、永続するライセンスは、ホストの検証とストレージのIDがないので扱わない(LinuxのChromeと同じ)。マシンを識別する値は使わない。

CDMが求めるもの(`media::CdmAuxiliaryHelper`、`port/media/eme_tessronos.cc`の`BrCdmHelper`)のうち、バッファと映像のフレームはメディアのライブラリの`SimpleCdmAllocator`で作る。CDMのファイル(`cdm::FileIO`)は「ブラウザの保存域」のレコード11に、ユーザ、表示しているページのオリジン、ファイルの名前の組ごとに置く。名前は英数字と`.`、`_`、`-`で、`_`では始まらないもの、大きさは512KBまでで、ChromeのCDMの保存と同じく、同じファイルを2つ同時に開くと`kInUse`を返す。答えはすべて後のタスクで返す。出力の保護の問い合わせとプラットフォームの検証には「ない」と答える。

再生の様子はコンソールに次の行で出る。

| 行 | 意味 |
| --- | --- |
| `browser: media: MediaSource attached` | `MediaSource`が要素に付いた |
| `browser: media: WxH, video, sound` | メタデータが揃った |
| `browser: media: video decoder <名前>`、`browser: media: sound decoder <名前>` | パイプラインが選んだデコーダ(`FFmpegVideoDecoder`、`VpxVideoDecoder`、`FFmpegAudioDecoder`、`OpusAudioDecoder`など)。復号してから渡すときは`(decrypting)`が付く |
| `browser: media: picture WxH` | 映像の大きさが変わった(画質の切り替え) |
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
| 映像の復号 | H.264はFFmpeg、VP8とVP9はlibvpx、AV1はdav1d(メディア要素と同じ`DefaultDecoderFactory`)。HEVCはRaspberry Pi 5のデコーダ(14.6.7) |
| 映像の符号化 | VP8とVP9はlibvpx、H.264はOpenH264(`exclude_blink.txt`から外して入れた) |
| 音の復号と符号化 | Opusはlibopus、AACの復号はFFmpeg。AACの符号化はブラウザのプロセスのプラットフォームのエンコーダを使う作りなので扱わず、`isConfigSupported`は対応しないと答える |
| 画像の復号 | `ImageDecoder`は、ページの画像と同じBlinkのデコーダ |

GPUのエンコーダとデコーダ(`MojoDecoderFactory`、ハードウェアを選ぶ`hardwareAcceleration: "prefer-hardware"`)は持たない。エンコーダは、符号化の具合をブラウザのプロセスの指標の受け手に通知する作りなので、通知されたことを捨てる受け手を`port/media/webcodecs_tessronos.cc`が渡す。

### 14.6.5 WebGPUとDawn

ChromiumのWebGPUは、レンダラのBlinkがDawnのワイヤ(コマンドを直列化する層)のクライアントとしてWebGPUの呼出しを直列化し、GPUプロセスのデコーダがワイヤのサーバとしてDawnの実装(`dawn::native`)に渡す。DawnはシェーダのトランスレータTintを使い、OSのグラフィックスAPI(Vulkan、Metal、D3D、OpenGL ES)で描く。このブラウザはGPUプロセスを持たないので、ワイヤのサーバを同じプロセスに置き、クライアントが書いたコマンドをそのままサーバに渡す(`port/webgpu/webgpu_tessronos.cc`)。DawnのバックエンドはOpenGL ESで、WebGLと同じくTessronOSのMesa(softpipeかv3d)の上で描く。`make GL=1`のときだけ動く。

| 層 | 内容 |
| --- | --- |
| Blink | `navigator.gpu`、`GPUDevice`などのモジュール(`modules/webgpu`)と、canvasの`webgpu`のコンテキスト。DawnのワイヤのクライアントのC++の型で書かれている |
| `webgpu_tessronos.cc` | Blinkが求める`gpu::webgpu::WebGPUInterface`とコンテキストのプロバイダ。ワイヤのクライアントとサーバ、Dawnのインスタンス、canvasの共有イメージ |
| Dawn(`libdawn.a`) | ワイヤのクライアントとサーバ、`dawn::native`のOpenGL ESのバックエンド、関数表(`dawn_proc`) |
| Tint | WGSLの読込みと検査、GLSLの出力 |
| Mesa(`libtsgl.a`) | EGLとOpenGL ES 3.1 |

- ワイヤのクライアントがフラッシュすると、溜めたコマンドの列をその場でサーバに渡す。サーバの答えは、コンテキストをバインドしたスレッド(ページのスレッドかワーカーのスレッド)のタスクとしてクライアントに渡し、続けてクライアントのイベントを処理する。ページのコールバックが新しいスタックで走るようにするためである。後で終わるサーバの仕事(バッファのマッピング、キューの仕事の終わり)は、残っている間2ミリ秒ごとにDawnのイベントを処理して見る。
- アダプタは、DawnのOpenGL ESのバックエンドに、Mesaの`eglGetProcAddress`を渡して作る(`RequestAdapterOptionsGetGLProc`)。ライブラリを開くことはしない。ディスプレイはEGLの既定のディスプレイ(プラットフォーム`tessronos`)である。DawnのOpenGL ESのバックエンドが出すアダプタは互換の水準(compatibility)だけなので、ページが求めた水準によらず、互換の水準のアダプタを返す。ChromiumがOpenGL ESの上でWebGPUを動かすときと同じ扱いで、アダプタは`core-features-and-limits`の機能を持たない。OpenGL ES 3.1と`GL_EXT_color_buffer_float`(3.2なら要らない)が要る。
- Dawnにアダプタを求める前に、システムのEGLがOpenGL ES 3.1のコンテキストを作れるかを、プロセスで1度だけ、そのためだけのスレッドで確かめる(`SystemHasGles31`)。作れなければDawnには求めず、アダプタはないと答える(ページの`requestAdapter()`はnullになる)。V3Dのv3dドライバは、カーネルがコンピュートのジョブを受け付けるときだけOpenGL ES 3.1になる(第13章13.3.4)。受け付けないときは3.0で、この場合になる。Mesaは、ドライバが持たない版のコンテキストを断るとき、作りかけのコンテキストを呼んだスレッドで一度現在のものにしてから壊し、前のコンテキストはMesaのGLの層でだけ戻す。以前はDawnがページのスレッドで`eglCreateContext`を呼んで断られていた(`eglCreateContext failed with EGL_BAD_MATCH`)。ページのスレッドではANGLEのコンテキストが現在のものになっているので、その状態をWebGLの描画の途中で動かさないようにした。
- Dawnは、デバイスのコンテキストをロバストなアクセス(`EGL_CONTEXT_OPENGL_ROBUST_ACCESS`)付きで作る。softpipeは拡張を出すが、リセットの状態を持たないので、このコンテキストを断る。パッチ`dawn_context_robustness.patch`で、断られたときはロバストなアクセスなしで作り直す。範囲外の参照は、Tintがシェーダに入れる検査で防がれる。DawnはGLのコンテキストを使うときに前のコンテキストを覚えて戻すので、同じプロセスのANGLE(WebGL)とSkiaのGPUのコンテキストとぶつからない。
- canvasの`webgpu`のコンテキストは、描くテクスチャを共有イメージとして作る。共有イメージのインタフェースは画素をメモリに持ち、メールボックスをテクスチャに関連付ける(`AssociateMailbox`)と、Dawnのデバイスにその大きさと形式(`bgra8unorm`か`rgba8unorm`)のテクスチャを作り、クライアントが予約した番号でサーバに入れる。合成はCPUで行う(14.6.1)ので、ページを描くとき、canvasは今のテクスチャの画素を求める。パッチ`gpu_canvas_context.patch`がBlinkにそのインタフェース(`g_webgpu_canvas_snapshot`)を足し、`webgpu_tessronos.cc`がテクスチャをCPUの読めるバッファにコピーし、マップして読む。テクスチャの間(フレームが終わって次のテクスチャを求める前)に描くときは、最後に読んだ画素を使う。`drawImage`や`toDataURL`が読むcanvasの画像も同じインタフェースから得る。
- Blinkの側はDawnのワイヤのクライアントの型で書かれ、`ui/gl/buildflags.h`の`USE_DAWN`がそれを選ぶ。エンジンのgnの構成は`use_dawn = false`のまま作り(Dawnを入れると多くのソースのコマンドが変わり、組み直しになる)、生成したこのヘッダの`USE_DAWN`だけを1にする(`tools/browser/blink.sh gen`)。DawnとTintのソースは、Dawnを入れた別のgnの出力(`out/tessronos_dawn`、`DAWN=1 tools/browser/gngen.sh`)から取る。そこではOpenGL ESのバックエンドとワイヤだけを入れ、TintのSPIR-V、HLSL、MSLの出力、Perfetto、X11を外す。Tintの生成するソースはGoのジェネレータ(`tools/src/cmd/gen`)で作るので、Go 1.25をDawnの`tools/golang`の場所に置く。DawnのOpenGLの読込みの生成にはKhronosの`gl.xml`(OpenGL-Registry)とEGLのヘッダ(EGL-Registry)が要るので、DawnのDEPSが指すバージョンを取る。
- `GL=1`でないブラウザも、Blinkが使うワイヤのクライアントのために`libdawn.a`をリンクする(リンクされるのはクライアントと関数表だけ)。`webgpu_tessronos.cc`はリンクせず(弱い名前で参照する)、`requestAdapter()`はnullを返す。
- WebGPUのコンテキストを作る前に、Blinkはブラウザのプロセスに3DのAPIが止められていないかを尋ねる(`GpuDataManager`)。プラットフォームのブローカがこれをブラウザの側のサービス(14.4.5)に渡し、止めていないと答える。

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

WebGLのコンテキストをWebGPUのデバイスの上で作る機能(`WebGLOnWebGPU`、ChromeでもChromiumの試作で既定で切)は、Chromeと同じく既定では出さず、`make GL=1`のビルドで起動の行`features=WebGLOnWebGPU`で出す。canvasの`webgl`のコンテキストは`WebGLRenderingContextWebGPU`になり、ANGLEのWebGPUのバックエンド(`libANGLE/renderer/wgpu`、シェーダはWGSLに変換)がDawnのワイヤのクライアントの上でGLの呼出しを行う。Blinkの側はChromiumの`webgl_rendering_context_webgpu_base.cc`のままで、GLとEGLの関数表(`gl::DriverGL`、`gl::DriverEGL`)は`ui/gl`をリンクして作る。Chromiumの試作がまだ扱わない呼出し(`renderbufferStorage`など、Chromiumのソースで`NOTIMPLEMENTED`のもの)は、Chromeと同じく何もしない。

- ANGLEはWebGPUのデバイスを求めると、答えが来るまで`WaitAny`で待つ。Chromeでは答えをGPUプロセスからIOのスレッドで受けるが、このブラウザのワイヤのサーバは同じプロセスにあり、答えをページのスレッドの次のタスクで渡すので、待っているスレッドには届かない。そこで同期で作るWebGPUのコンテキスト(`CreateWebGPUGraphicsContext3DProvider`)は、サーバの答えをフラッシュの中でその場でクライアントに渡し、サーバの遅れた仕事(アダプタとデバイスの用意など)もフラッシュの中で終わるまで回す。
- `gn`の構成の`use_dawn = false`のため、Blinkのこのソースだけ`USE_DAWN`を1にして読む(`port/webgl/webgl_webgpu_flags.h`、`flags.txt`)。ANGLEの`FramebufferWgpu.cpp`がlibc++の内部のヘッダ(`<__config>`)を読む所はパッチ`angle_framebuffer_wgpu.patch`で除く。

### 14.6.6 PDF

型が`application/pdf`(または`text/pdf`)の応答は、メインフレームでもiframeでも、PDFium(Chromiumのソースツリーの`third_party/pdfium`。ChromeのPDFの表示が描くのに使うライブラリ)でページごとに96 dpiの画像に描き、その画像を灰色の背景に並べたHTMLのページを代わりに確定する(`port/blink/pdf_tessronos.cc`の`br_pdf_page`)。タイトルはPDFの文書の情報の`Title`、なければアドレスのファイルの名前である。描くのは最初の60ページまでで、それより多いときはページの終わりにそう書く。〈印刷...〉はそのページを他のページと同じく印刷する。PDFのJavaScriptとフォームは動かさず、文字の選択と検索は画像なのでできない。PDFiumは`labels_pdfium.txt`のラベルをほかと同じくコンパイルして`libpdfium.a`にする(`build.sh`の`pdfium_lib`)。gccで通らない所はパッチ`pdfium_texteditengine.patch`と、newlibの`_timezone`を使わせる`flags.txt`の行で直す。

PDFを表示できることをページに示すため、Blinkのプラグインの一覧(`PluginRegistry`)を、プラットフォームのブローカ(`blob_tessronos.cc`の`PlatformBroker`)からサービスのスレッドで答え、`application/pdf`と`text/pdf`を受ける1つのプラグインを返す。設定の`PluginsEnabled`は真にする(Chromeと同じ)。PDFのほかのプラグインは持たないので、`<embed>`と`<object>`のほかの型は代わりの中身を出す。

### 14.6.7 HEVCとハードウェアのデコーダ

HEVC(H.265)は、Raspberry Pi 5のデコーダ(デバイスの実身「動画の復号器」、6.7)で復号する。ソフトウェアのHEVCのデコーダは持たないので、デコーダの無いマシン(QEMU、Raspberry Pi 4)ではHEVCを扱えないと答える。

| 部品 | 内容 |
| --- | --- |
| `H265Decoder` | Chromiumの`media/gpu`のもの(`h265_decoder.cc`、`h265_dpb.cc`、`codec_picture.cc`)をそのままコンパイルする。Annex Bのストリーム(MP4のデマルチプレクサとWebCodecsがそうする)を`H265Parser`で解析し、POCと参照ピクチャの一覧、復号済みピクチャのバッファ、出す順を受け持ち、1枚ごとにアクセラレータを呼ぶ |
| アクセラレータ(`port/media/hevc_tessronos.cc`) | `SubmitFrameMetadata`でパラメータと参照を、`SubmitSlice`でスライスヘッダの値とバイト列をコピーし、`SubmitDecode`で1つのジョブにして実身に書く。重み付き予測の色差のオフセットは仕様の式で求めて渡す。スライスのバイト列は`SubmitSlice`のうちにコピーする(次のピクチャの最初のスライスが来て前のピクチャが終わるときには、元のバッファはもう無い) |
| デコーダ(`HevcDecoder`) | `media::VideoDecoder`の実装。MainとMain Still Pictureの8ビット、Main10の8ビットと10ビット、1920×1088までを引き受ける。`kConfigChange`でピクチャの大きさとビット深度と、ストリームが持つ参照の数に2を足したスロット(17まで)を実身に設定する。出すピクチャは、8ビットならスロットからNV12で読みlibyuvでI420のフレームに、10ビットならP010で読み`P010ToI010`でYUV420P10(16ビットのサンプルの下10ビットに値を持つI420。WebCodecsでは`I420P10`)のフレームにする。デコーダは一度に1つのストリームだけが持つ(`br_hevc_acquire`)。2つ目のストリームはソフトウェアのデコーダに回り、HEVCは再生できない |
| 実身との間(`port/media/br_hevc.cc`) | システムのヘッダで実身を探して読み書きする。メディアのライブラリの側がシステムのヘッダを読まないように、ジョブの構造体をCの型で書き直した`br_hevc.h`を間に置き、両者の配置が同じことを`static_assert`で確かめる |

メディアのパイプラインもWebCodecsも、デコーダを`DefaultDecoderFactory`から得る。パッチ`default_decoder_factory.patch`で、埋め込む側が渡すデコーダ(`g_platform_video_decoder`)をソフトウェアのデコーダより先に並べる。起動時に実身があれば、その関数を渡し、`UpdateDefaultDecoderSupportedVideoProfiles`でHEVCのMain、Main10、Main Still Pictureを扱えるプロファイルに足す(`br_hevc_media_init`)。これで`canPlayType`、`MediaSource.isTypeSupported`、WebCodecsの`isConfigSupported`がHEVC(`hvc1.1`・`hev1.1`系と、Main10の`hvc1.2`・`hev1.2`系)を受け付ける。実身が無ければどれも何もしない。

10ビットのフレームの表示は、メディアの要素の描画(`port/media/media_player.cc`の`BrFrameGenerator`)で行う。GPUのラスタ化がYUVの平面を受け取るとき、16ビットのテクスチャを扱えれば、サンプルの10ビットを上に詰めて下にも繰り返し(1023が1.0になる)、10ビットの変換行列(BT.2020なら10ビットのもの)でRGBにする。16ビットのテクスチャが無ければ、上の8ビットに丸めて8ビットの行列で変換する。プロセッサでのラスタ化はlibyuvの`I010ToARGBMatrix`でRGBにする。VP9のプロファイル2やAV1の10ビットのフレームも同じ経路を通る。

QEMUの`test_blink_codecs`は、HEVCのMainとMain10を3つの問いのどれも受け付けないこと(`hevc=000 hv10=000`)を確かめる。復号そのものは実機で、カーネルの確認(6.7.1)が同じジョブの形を確かめる。

## 14.7 ウインドウと操作

ページはウインドウの作業領域の全体に描く。ウインドウのタイトルは、表示しているページのタイトル(タイトルがなければアドレス)である。状態は、システムのメッセージ行に表示する。

アドレスの欄と操作のボタンは、ページのウインドウとは別の小さなウインドウ(ツールパネル、外寸560×48)にあり、ページのウインドウの8ドット上に置く。ツールパネルは、ページのウインドウの従属ウインドウで、ウインドウマネージャが常にページのウインドウのすぐ前に保ち、ページのウインドウを移動すると一緒に移動する。ほかのアプリのウインドウが最前面の間は隠れ、ページのウインドウが最前面に戻ると、そのすぐ前に出る(10.4)。

| 部品 | 動作 |
| --- | --- |
| ← | 戻る |
| → | 進む |
| 再 | 再読込 |
| × | 読込みの中止。それまでに届いたものでページを表示する |
| アドレスの欄 | クリックで入力を始める。Ctrl+Aで全体を選び、Enterで開き、Escで元に戻す |

右ボタンは、まずページに渡す(押下と`contextmenu`のイベント)。ページが`preventDefault`でそれを自分のものにしたときは何も出さず、そうでなければウインドウのメニュー(「ブラウザのメニュー」)を出す。戻る、進む、再読込、アドレス入力、最初のページ、〈印刷...〉(14.7.5)、〈メディア〉(14.7.7)、〈拡大〉〈縮小〉〈等倍で表示〉〈全画面表示〉、〈編集〉と、ウインドウの共通のメニューがある。押した所にあるものの項目、〈リンクを新しいウインドウで開く〉〈リンクのアドレスをトレーへ複写〉〈画像を保存〉〈画像のアドレスをトレーへ複写〉〈選んだ文字をトレーへ複写〉は、リンク、画像、選んだ文字があるときだけ出る(ほかのときは`MN_HIDE`で隠す)。行き先のない〈戻る〉〈進む〉は灰色にする。エンジンのスレッドが1秒近く戻らないとき(読込み中など)は、ウインドウのスレッドがその場でメニューを出し、ページには渡さない。〈編集〉には〈トレーへ複写〉〈トレーへ移動〉〈トレーから複写〉〈すべて選択〉(Blinkの編集の命令、`Copy`、`Cut`、`Paste`、`SelectAll`)と〈ホームページに設定〉がある。〈ホームページに設定〉は、表示しているページのアドレスをページの実身の本文に書き(14.8)、メッセージ行に「ホームページに設定しました」と出す。キーでは、Alt+←で戻り、Alt+→で進む。Chromeと同じく、Backspaceでは戻らない(入力の途中でページを失わないため。Chromeはバージョン52でやめた)。Ctrl+L、F5とCtrl+R、Ctrl+N(新しいブラウザ)、Alt+Home(ホームページ)、Ctrl+＋(`=`、`;`、テンキーの`+`のキー)とCtrl+−とCtrl+0(拡大、縮小、等倍)、Ctrl+ホイール(拡大と縮小)、F11(全画面)がある。

戻る履歴と進む履歴(セッション履歴)は、エンジンがページの文書ごとの`HistoryItem`の列として持つ(最大50)。ページの`history.pushState`と`replaceState`、フラグメントへの移動(`#`)も項目になる。メインフレームの文書が確定したとき(`DispatchDidCommitLoad`)と、同じ文書の中で移ったとき(`DidFinishSameDocumentNavigation`)に、列の項目を足すか(普通の移動)、今の項目を置き換えるか(再読込、`replaceState`、`location.replace()`、ページの読込み中のフラグメントへの移動)、行き先の項目に移る(戻る、進む)。`history.length`はその数で、ページの`history.back()`、`forward()`、`go()`と`navigation.back()`、`forward()`、`traverseTo()`は、ブラウザの〈戻る〉〈進む〉と同じ処理を通る(`NavigateBackForward`。Navigation APIの移動はブラウザに頼む形なので、パッチ`navigation_api.patch`でそこへ回す)。行き先が今の文書の項目(文書の番号が同じ)なら、その文書の中で移り(`CommitSameDocumentNavigation`、`popstate`、`hashchange`、`navigate`のイベントが出る)、ほかの文書の項目ならアドレスを取得し直して、その`HistoryItem`を渡して確定する(`kBackForward`)。そうすると、項目の状態のオブジェクト(`history.state`)とスクロール位置が戻り、`performance`の移動の種類は`back_forward`になる。新しい文書には、同じオリジンの前後の項目をNavigation APIの項目(`navigation.entries()`)として渡す。

iframeの移動も同じ列の項目になる(結合セッション履歴)。項目はそれぞれ、メインフレームの`HistoryItem`と、その項目で文書の分かっているiframeごとの`HistoryItem`を持つ。iframeの文書の確定と、iframeの中の移動(フラグメント、`pushState`)は、子のフレームのクライアントが通知し(`DispatchDidCommitLoad`、`DidFinishSameDocumentNavigation`)、普通の移動なら、今の項目をコピーしてそのiframeの分だけ替えた項目を足し、置き換えならそのiframeの分を置き換える。iframeの最初の文書(初めの空の文書からの移動)は、Blinkが置き換えとして始めるので項目を足さない。戻ると進むでは、行き先の項目と今とで`HistoryItem`の違うフレームを、それぞれ行き先に移す。同じ文書の項目ならその文書の中で移り、ほかの文書の項目なら、子のフレームのクライアントがアドレスを取得し直し、その`HistoryItem`を渡して`kBackForward`で確定する(`GoToEntry`)。メインフレームがほかの文書に移るときは、iframeはその文書の中で新しく読み込まれる。iframeが文書から取り除かれると(文書ごと替わるときを除く)、その分を項目から除き、そのiframeだけが作った項目は消える。iframeが利用者の操作のないまま作った項目(広告の枠が自分で移るものなど)は、ウインドウの〈戻る〉〈進む〉とAlt+←、→では飛ばし、ページの`history.back()`などでは飛ばさない。

iframeの文書の確定には、移動を始めたときの読込みの種類(普通、置き換え、再読込、戻る・進む)、リファラ、移動を始めた時刻と要求を送った時刻と応答の時刻、辿ったリダイレクト(応答とその行き先)を渡す。そのため、iframeの文書の`performance.getEntriesByType('navigation')`の`type`(`reload`、`back_forward`)、`redirectCount`、`fetchStart`、`document.referrer`がChromeと同じになる。

アドレスの欄、ウインドウのタイトル、メッセージ行は、ページが自分でしたことに追いつく。エンジンのスレッドは回るたびに、文書の今のアドレス(`pushState`やフラグメントで変わったもの、転送の後のもの)、タイトル(`document.title`)、ポインタの下のリンクのアドレスかツールチップ(`title`属性)を見て、変わっていればアドレスの欄、タイトル、メッセージ行に出す(`follow_doc`)。アドレスの欄を編集している間は欄を書き換えない。ツールパネルの←と→は行き先がないとき、×は読込み中でないとき、動かせない形(`OB_WP_OFF`)で表示する。再読込は、文書の今のアドレスを読み直し、スクロール位置を保つ。

ページを離れるとき(リンク、アドレスの欄、再読込、戻る、進む)は、ページの`beforeunload`を先に呼び(`FrameLoader::ShouldClose`)、ページが離れないよう求めれば、ブラウザのデータボックスの確認のパネル(14.7.3)で「このサイトから移動しますか?」(再読込では「このサイトを再読み込みしますか?」)と尋ねる。Ctrlを押しながら、または中ボタンでリンクを押したときは、新しいウインドウ(別のプロセスのブラウザ、14.7.8)で開く。

アドレスの欄に打ったものは、Chromeのアドレスバーと同じく扱う。スキームのあるものはそのまま、点を含む名前(と`localhost`、IPアドレス、ポート付きの名前)はアドレスとし、インターネットの名前はHTTPSを先に試して繋がらなければHTTPにする。空白を含むもの、点のない語は既定の検索エンジン(Google)で検索する。ページが読めないときは、Chromeのエラーのページと同じ文言と、ネットワークのエラーの名前(`ERR_NAME_NOT_RESOLVED`など、`br_net_fetch_error`)と〈再読み込み〉のボタンを持つページを出す。

ページの中の検索は、Ctrl+Fかメニューの〈ページの中を検索...〉で、探す文字をブラウザのデータボックスの問い合わせのパネル(14.7.3)で尋ねる。Chromeの検索バーと同じく、メインフレームの表示している文字から大文字と小文字を区別せずに探し(Blinkの`FindBuffer`、1000件まで)、見つけたものすべてに検索のマーカー(`TextMatch`の文書のマーカー)を付け、行った先の一つを動いているマーカー(Chromeと同じ橙色)にして表示の中ほどに出す。メッセージ行に「「語」 2 / 3 件」と出す。F3とCtrl+Gで次、Shiftを足して前へ移り(端から反対の端へ回る)、Escでマーカーを消す。

拡大と縮小は、Chromeの倍率の段(25%から500%)で、メインフレームの倍率(`LocalFrame::SetLayoutZoomFactor`)を変える。`devicePixelRatio`もそれに従う(110%で1.100000023841858、Chromeと同じ値)。倍率はホストごとに覚え、同じホストのページを開くとその倍率にする(ブラウザが動いている間)。

全画面は、ページの`requestFullscreen`(動画のプレーヤの全画面のボタンなど)とF11、メニューの〈全画面表示〉で入る。ウインドウを、作業領域が画面全体になるように動かし(枠とバーは画面の外に出る)、ツールパネルを隠す。ページが求めたときは、そうしてから`Fullscreen::DidResolveEnterFullscreenRequest`でページに通知し、ページの`exitFullscreen`とEsc、F11では、ウインドウを元に戻してから`Fullscreen::DidExitFullscreen`で通知する。ホイールと矢印キーはBlinkの中でスクロールし、ページの中のスクロールできる領域にも効く。ページ全体のスクロールバーは、Blinkのものではなく、ウインドウの右と下にあるTessronOSのスクロールバーである。ブラウザは表示のたびに、ページの高さと幅、表示している範囲をスクロールバーのレコードに書き、つまみの前後を押したときやつまみをドラッグしたときの`OB_E_SCROLL`(10.4)を受けて、その位置へスクロールする。ドラッグ中の通知は続けて届くので、最後の位置だけを描く。Blinkのスクロールバーはメインフレームのものだけを隠し(設定の`HideScrollbars`を、パッチ`viewport_scrollbars.patch`でメインフレームのビューポートに限る)、ページの中のスクロールできる領域とiframeは自分のスクロールバーを持つ。新しいウインドウで開くリンク(`target="_blank"`)と、クリックから呼ばれた`window.open`は、新しいウインドウで開く(14.7.8)。直前にクリックやキーの操作がない`window.open`(スクリプトが出すポップアップ)は開かない。ページの入力欄に入力するときは、キーを先にかな漢字変換に渡し、確定した文字を欄に入れる。変換中の文字は欄の中に下線付きで表示し、候補はブラウザが文字の位置の下に表示する。

ウインドウのイベントは、ブラウザのウインドウのスレッドが受け取る。Blinkが動くスレッド(エンジンのスレッド)は、ページの部品がそろうのを待つ間(長くて30秒)、ページのスクリプトが走り続ける間、ページの本文の取得を待つ間はイベントを受け取れないので、以前はその間、右ボタンのメニューも、ピクトグラムのダブルクリックやCtrl+Eで閉じることも効かず、ウインドウが固まったように見えた。ウインドウのスレッドはイベントを受け取ると、次のものはその場で応じる。

| イベント | 応じ方 |
| --- | --- |
| 右ボタン(ページとツールパネル) | メニューを出す。〈閉じる〉はその場で閉じ、ほかの項目は選んだものをエンジンのスレッドに渡す。ページを移る項目(戻る、進む、再読込、最初のページ)は、読込み中のページを止めてから渡す |
| `OB_E_CLOSE`(ピクトグラムのダブルクリック、閉じる)、ウインドウの削除、Ctrl+E | 閉じる |
| ツールパネルの×、読込み中のEsc | 中止。取得の待ちをやめ、エンジンのスレッドが1秒以上戻らなければ、走っているスクリプトを終わらせる(V8の`TerminateExecution`はどのスレッドからでも呼べる) |

ほかのイベントは列に入れ、エンジンのスレッドが順に処理する。閉じるときは、ウインドウの位置をページの実身に書いてから、ページとツールパネルのウインドウをすぐに消し、エンジンのスレッドに終わるよう伝える(取得の待ちをやめ、スクリプトを終わらせる)。3秒たっても終わらなければプロセスを終える。中止で本文の取得をやめたときは、表示していたページをそのまま残し、メッセージ行に「読み込みを中止しました」と出す。本文の取得(`br_net_fetch_sync`)は0.1秒ずつ待ち、そのたびに中止を確かめる。エンジンのスレッドは回るたびに、時刻と何をしているか(ループ、ページの本文の取得、ページの読込み、部品の待ち)を残す。5秒より長く戻らないとき、ウインドウのスレッドはそれをコンソールに出し(`browser: the engine's thread has not come round`。続けて戻らないときは、10秒、20秒、40秒と間を倍にして出す)、V8に割込みを頼んで、スクリプトが走っていればそのスタック(関数、スクリプト、行、桁)をエンジンのスレッド自身に出させる。あわせて、エンジンのスレッドのタスクの状態(`tk_ref_tsk`の状態、待ちの要因、待ち対象)と、条件変数で待っているか、ほかのスレッドが持つロックを待っているかを出す。後者は、プログラムを`pthread_cond_wait`、`pthread_cond_timedwait`、`pthread_mutex_lock`を包んで(リンクの`--wrap`、`build.sh`)リンクし、エンジンのスレッドが条件で待つとき、または取られているロックを待つときに、フレームポインタをたどって呼出し元の戻りアドレスを12段まで記録しておく。出たアドレスは、Linuxの側に残すシンボル付きのプログラム(`$OBJ/tessronos/browser.elf.sym`)で`addr2line -f -C`に引く。

この報告は止まっていた最中の一瞬を見るだけなので、エンジンのスレッドは、回ってから次に回るまで(1回り)の時間の内訳を自分で数える(`br_blink.cc`の`watch_time`)。1回りが1秒以上かかったとき、回り終えたところで次の行を出す。時間はプロセスがシステムを呼ばずに読めるプロセッサのカウンタ(`cntvct_el0`)で測る。

| 行 | 意味 |
| --- | --- |
| `browser: slow: M ms while 何をしていたか: tasks T ms (N), frame callbacks F ms, style, layout and paint L ms, outside these O ms; in all script S ms, collector C ms, WebGL W ms` | 1回りの時間Mの内訳。`tasks`はタスク(Blinkとネットワークの層が投げたもの)を走らせた時間と数、`frame callbacks`は`requestAnimationFrame`のコールバック、`style, layout and paint`はライフサイクルの更新、`outside these`はそれ以外(本文の取得の待ち、ビューのラスタ化など)。`script`はV8への外側の呼出し(スクリプトの実行、関数とイベントハンドラの呼出し。V8が自分で走らせるPromiseのジョブは入らない)、`collector`はこのスレッドでのV8のごみ集め、`WebGL`はANGLEとその下のOpenGL ES(softpipeかV3D)の呼出しの時間、`console`はコンソールへの書出し(下のリングの空きの待ち)の時間で、どれもMの一部である。ページのスクリプトから呼ぶWebGLは`script`の中にも数える |
| `browser: slow:   a task of T ms (script S, collector C, WebGL W, console K) posted from 関数 (ファイル:行)` | その1回りの長いタスクの上位3つと、それぞれを投げた場所 |
| `browser: slow: a task of T ms ...` | 0.5秒以上かかったタスク。終わったときに出す |
| `browser: start: M ms from main: 段 T, 段 T, ... ms` | 起動の各段の時間。`main`からエンジンのスレッドが初めて回る(`starting`)までのMの内訳で、段はウインドウ(`the window`)、書体(`the face`)、GPUとEGLの準備(`the GPU`)、ICUのロケール、時間帯、スレッドプール(`the locale, the time zone and the threads`)、Blinkの初期化(`Blink`)、機能とスケジューラ(`the features and the scheduler`)、Blinkのコアとバインディング(`Blink's core`)、V8とそのスナップショット(`V8 and its snapshot`)、ネットワークの層(`the network`)、保存域、IndexedDBとサービス(`the stores and services`)、エンジンの残り(`the engine's rest`)、ツールパネル(`the tool panel`)である。`main`より前の、プログラムを読みプロセスを作る時間は、システムの`elf: a program of N MB read, its process made in T ms`の行が言う |

最初の1回り(`starting`)は`main`の初めから数える。2026年10月23日まで、回った時刻の初期値が0(起動からの時刻の原点)だったので、最初の1回りは「OSの起動からの時間」になっていた。ThinkPad X1 Yoga(第3世代)の実機で出た`browser: slow: 137407 ms while starting: ... outside these 137407 ms`はこれで、同じログで3回起動したときの値(137407、159519、205626ミリ秒)は、それぞれの起動の時刻(ログの末尾は起動から205秒)と一致する(1回目は、Wi-Fiにつないだ121秒の後に起動した)。130秒の待ちがあったのではなく、起動にかかった時間はこの行からは分からない。分かっているのは、プログラム(214MB)を読みプロセスを作るのにかかった6.3秒(`elf:`の行)だけで、残りは`browser: start:`の行で確かめる。同じ日にQEMUの`q35`(WHPX、NVMeのディスク、`test_blink_v3dfall`)で起動すると、次のようになった。

```text
elf: a program of 214 MB read, its process made in 1229 ms
browser: start: 1391 ms from main: the window 35, the face 82, the GPU 863, the locale, the time zone and the threads 137, Blink 3, the features and the scheduler 7, Blink's core 10, V8 and its snapshot 37, the network 34, the stores and services 16, the engine's rest 5, the tool panel 162 ms
```

`the GPU`は模擬のIntelのGPUでirisの画面を作り、確認のフレームを試してsoftpipeに戻るまでの時間である。実機との違いはプログラムの読込みで、実機はUSBのカードリーダーのメモリカード(`uda`)から読むので約34MB/s、QEMUはNVMeのディスクのイメージから約170MB/sで読む。

WebGLをGPUに移して縮むのは`WebGL`の時間だけで、`script`のうちの残り、`style, layout and paint`、`outside these`のラスタ化は変わらない(ページのラスタ化は14.7.2のGanesh)。

プロセスの標準出力と標準エラーは、システムのコンソール(`tm_putstring`)に渡る。コンソールはシリアルの線に1文字ずつ、割込みを止めて送り終えるまで戻らないので、Raspberry Pi 5の115200ボーでは、どのスレッドが書いても毎秒約11.5KBしか進まない。2026年10月16日の実機で、起動後に最初のページ(`https://www.tron.org`)を開くたびに出た`the engine's thread has not come round for 5.0 s: the page's parts coming`(13時40分29秒と13時45分8秒)では、その前後の約7秒間、コンソールの行が毎秒11.0〜11.7KB(行頭の時刻を除いた文字数)出ており、線の速さに張り付いていた。出ていたのはページの部品ごとの`net fetch`、`fetched`の行、`console: Mixed Content`の行、名前の引けなかった取得の行などで、止まりの報告もエンジンのスレッドが`printf`の中でコンソールのロックを待つところを捉えていた(呼出し元を引くと`BrURLLoader::Deliver`の`printf`)。このページはWebGLを使わず、ページのラスタ化はこの間に走らないので、この止まりはWebGLをGPUにしても変わらない。10月8日のV3Dが使えていたときの実機でも、同じ`the page's parts coming`の止まりが出ていた。

QEMU(TCG、4コア)で同じページを開くと(`KT_SITE=https://www.tron.org NET=user`、2026年10月16日)、QEMUのシリアルは速いのでコンソールの待ちはなく(`console` 2〜10ミリ秒)、部品を待つ1回りの7079ミリ秒と9547ミリ秒の内訳は、スクリプトが2221ミリ秒と6091ミリ秒(`googletagmanager.com/gtag/js`のタイマーの1タスクが4041ミリ秒、ページの中のスクリプトが2325ミリ秒など)、スタイルとレイアウトと描画の記録が2881ミリ秒と1177ミリ秒、WebGLは0ミリ秒だった。QEMUのプロセッサは実機よりずっと遅いので、実機でのスクリプトとレイアウトの割合はこの行を実機で出して確かめる。

コンソールへの書出しでスレッドが待たないように、ブラウザは`_write`を包んでリンクし(`build.sh`の`--wrap=_write`)、記述子1と2への書込みを512KBのリングに入れて戻る。リングの中身は専用のスレッドが来た順にコンソールへ渡す。書く側が待つのはリングがいっぱいのときだけで、行を捨てることはない(あふれそうなら書く側が線の速さまで待つ)。プロセスを終えるとき(終わりの`_exit`、閉じた後の時間切れ、V8の`abort`と`ExitProcess`、`atexit`)はリングが空になるまで最大で数秒待つ。プロセスを終わらせるデータアボート(V8のトラップハンドラが受けないもの)では、`ts_signal.c`がプロセスを止める前に、そのスレッドがリングの残りを自分でコンソールに出す(`br_console_flush_now`)ので、カーネルの`!EL0 fault!`の行より前に出る。データアボート以外の例外(未定義命令など)でカーネルがプロセスを止めたときは、リングに残っていた行は出ない。

### 14.7.1 描画

ブラウザは、ウインドウの作業領域の面(10.2)を`wm_obj_surf`で受け取り、Skiaの面(`SkSurfaces::WrapPixels`)としてそこに直接描く。描いたあとは変わった矩形を`wm_obj_put`でウインドウに置く。以前は、作業領域の大きさのプロセスの面に毎回ページ全体を描き、上位バイトを落としながら別のバッファにコピーし(1回目のコピー)、`dp_put_argb`でカーネルがウインドウのピクセルに1画素ずつコピーし(2回目)、`wm_obj_flush`で作業領域の全体を組み立て直していた。面を受け取れないときは今もこの方法で描く(起動の引数の行`draw=copy`でも選べる)。

変わった所は、コンポジタがレイヤーについて求めるのと同じ方法で、描画の成果物(`PaintArtifact`)から求める。ビューと同じ大きさの1つのレイヤーをルートの性質で持つものとして`RasterInvalidator`を置き、現れた、消えた、動いた、性質が変わった描画のまとまり(チャンク)と、描き直された表示項目について、新旧の範囲をビューの座標で受け取る。これは、ライフサイクルの描画の段が古い成果物を片付ける前に行わなければならない。合成をしない設定では、Blinkはその時点を外に通知しないので、パッチ`paint_hook.patch`で`LocalFrameView::PushPaintArtifactToCompositor`の合成をしない側から関数`g_non_composited_paint_hook`を呼ばせる。ブラウザはそこで変わった矩形をため、性質の変更のフラグを、コンポジタが更新のたびに行うのと同じように消す。表示するときは、ためた矩形を近いもの同士でまとめて最大16個にし、それぞれの矩形にクリップして描画の記録を再生する(記録は1回の表示につき1回だけ作る)。何も変わっていなければ描かず、置かない。新しいページ、ウインドウの大きさの変更、`OB_E_REDRAW`、かな漢字変換の候補の表示中とその直後は、ビュー全体を描く。

スクロールでは、スクロールの平行移動の下にあるすべてのチャンクの位置がビューの上で変わるので、ほぼビュー全体が変わった所になる。画面ではビュー全体が動くので、ウインドウマネージャはどのみち作業領域の全体を組み立て直す。ページの描画はQEMUで3ミリ秒あまりで、1コマの時間の大半は組み立ての側にあるので、すでに描いた画素をずらして現れた帯だけを描く方法は採っていない。

起動の引数の行`check=<パス>`を与えると、変わった所だけを描いた各表示のあとで、同じ状態のビュー全体を別の面に描き直して比べ、異なる画素の数を数える。異なる画素があれば、その範囲、最大の違い、その表示で描いた矩形をコンソールに出す。比べている間にウインドウが削除されて面がウインドウのものでなくなったとき(`wm_obj_surf`が答えなくなる)は、その表示を数えない。試験`test_blink_frame`はこれで、変わった所の漏れがないことを確かめる。

QEMU(TCG、4コア、1280×800)で、作業領域800×560のウインドウに、40×40の箱の色を100ミリ秒ごとに変えるページ(`test_blink_frame`の`/fr`)を表示して測った値は次のとおりである。1コマの時間は、ブラウザの中で測った描画(ライフサイクルの更新を含む)、コピー、置く、反映の時間の和で、反映と置くには組み立てを待つ時間を含む。組み立てた画素は、その間にウインドウマネージャが組み立てた画素の数である。

| 場面 | 1コマの時間(前、後) | 1コマで描いた画素(前、後) | 組み立てた画素(前、後) |
| --- | --- | --- | --- |
| 箱の色が変わる3秒(33、34コマ) | 10.8ミリ秒、1.9ミリ秒 | 448,000、1,600 | 15,232千、54千 |
| 入力欄のキャレットの点滅3秒(7コマ) | 12.2ミリ秒、2.9ミリ秒 | 448,000、19 | 3,136千、1千 |
| ホイールを8目盛り回す(15コマ、9コマ) | 16.3ミリ秒、18.3ミリ秒 | 448,000、398,224 | 10,734千、7,598千 |

前の1コマの内訳は、箱の場面で描画3.3、コピー1.6、`dp_put_argb`3.5、`wm_obj_flush`2.3ミリ秒、後は描画1.4、`wm_obj_put`0.5ミリ秒である。スクロールでは、後の`wm_obj_put`(15.0ミリ秒)が前の`dp_put_argb`と`wm_obj_flush`の和(11.1ミリ秒)より長い。面からのコピーはウインドウマネージャのロックの中で行うので、組み立ての最中に呼ばれると、その組み立てが終わるのを待ってからコピーし、さらに次の組み立てを待つためである。前のバージョンは1目盛りの入力のあとと次の描画の要求とでビューを2度描くことがあり(後のバージョンでは2度目は変わった所がないので描かない。15コマのうち6コマ)、8目盛りの合計では、ブラウザの時間は245ミリ秒から165ミリ秒に、組み立てた画素は約3割減った。

### 14.7.2 GPUでのラスタ化

`make GL=1`のブラウザは、GPUのあるマシンで、ページのラスタ化をGPUで行う(`port/gpu/br_gpu.cc`)。Chromiumでは、レンダラが作った描画の記録をGPUプロセスのSkia(Ganesh)がOpenGL ES(ANGLE)の上でタイルにラスタ化し、vizがそれを合成する。このブラウザは`content`もvizも使わないので、ライフサイクルの描画の段が作った記録(`cc::PaintRecord`、14.7.1と同じもの)を、SkiaのGaneshがTessronOSのOpenGL ES(Mesa、第13章13.6)の上で1枚のレンダーターゲットに再生し、変わった矩形を作業領域の面に読み戻す。レイヤーはビュー全体の1つで、合成の段はない。変わった所の求め方、矩形のまとめ方、`wm_obj_put`での出し方は14.7.1のままで、矩形を画素にする所だけが変わる。

| 部品 | 内容 |
| --- | --- |
| GPUのスレッド | 起動時に1つ作る。MesaのEGLの既定のディスプレイ(プラットフォーム`tessronos`)にOpenGL ES 3のコンテキスト(なければ2)を1×1のピクセルバッファで作り、`GrGLMakeAssembledInterface`で関数を`eglGetProcAddress`から集めて、Ganeshの`GrDirectContext`を作る。Ganeshのオブジェクト(コンテキスト、レンダーターゲット、テクスチャ、canvasの面と画像)はすべてこのスレッドで作り、使い、捨てる。メインスレッドは仕事を渡して終わりを待つので、仕事が読む記録、画像、canvasの記録はその間変わらない。ほかのスレッドで手放されたもの(canvasのスナップショット)は、捨てる仕事としてこのスレッドに渡し、次の仕事の前に捨てる |
| ページのラスタ化 | ビューの大きさのレンダーターゲット(N32)に、矩形ごとにクリップして白で消し、記録を再生する。全部を描いてから`flushAndSubmit`でドライバに渡し、矩形ごとに`readPixels`で面の同じ場所に読み戻す。Ganeshのテクスチャのキャッシュは96MBまでとする |
| 画像 | Ganeshが描くときにテクスチャにする。ジェネレータがYUVの平面を出せる画像(動画のフレーム、JPEG)は、平面のままテクスチャにしてシェーダでRGBにする(Ganeshの遅延画像の扱い) |
| 動画のフレーム | 14.6.2の`BrFrameGenerator`。GPUで描くときは、復号したフレームのY、U、Vの平面がそのままテクスチャになり、RGBへの変換はGPUのシェーダが行う。同じフレームを描き直すときは、Ganeshが作ったテクスチャを使い回す |
| canvasの2D | パッチ`canvas_gpu.patch`で、Blinkのcanvasの画素の持ち主(`Canvas2DBitmapProvider`)が、作られるときに面をGPUのスレッドに求める。GPUのスレッドは、メインスレッドのcanvasで、128×128以上の8bitのものにだけ面を作る。面を得たcanvasは、記録の再生(`RasterRecord`)、`putImageData`の書込み(`WritePixels`)、スナップショットをGPUのスレッドで行う。スナップショットはGPUのテクスチャをバッキングストアに持つ`PaintImage`(`cc::TextureBacking`の`BrGpuBacking`)で、GPUのスレッドでページやほかのcanvasに描くときはそのテクスチャをコピーせずに描く。ほかのスレッドで画素を求められたとき(`getImageData`、`toBlob`、プロセッサで描くcanvas)は読み戻して渡す。ワーカーの`OffscreenCanvas`と小さいcanvasは今までどおりプロセッサで描く |
| WebGL | 14.6.1のとおり、ANGLEのコンテキストがメインスレッドで同じMesaのディスプレイの上に動く。コンテキストはスレッドごとに現在のものを持つので、GPUのスレッドのコンテキストとは互いに切り替わらない。描画バッファは描いたフレームを読み戻してビットマップにし、Ganeshがページに描くときにテクスチャにする |

使うかどうかは起動時に一度決める。既定では、マシンがGPUを持ち(`gp_ref_gpu`が答える)、OpenGL ESのドライバがGPUで描くとき(v3d)だけ使う。softpipeはプロセッサで描くドライバで、SkiaのCPUのラスタのほうが速いので、使わない。起動の引数の行`gpu=on`では、softpipeでもGPUの経路を使う(QEMUでの試験)。`gpu=off`では使わない。`GL=1`でないブラウザには`br_gpu.cc`がリンクされず(その名前は弱い名前で参照する)、いつもプロセッサで描く。GPUの経路が失敗した表示(レンダーターゲットが作れないなど)は、その回だけプロセッサで描く。

起動の引数の行`check=`(14.7.1)は、GPUの経路ではビュー全体もGPUで描き直して比べる。行`gpucheck=<パス>`は、GPUで描いた各表示のあとで、同じ状態のビュー全体をプロセッサで描き直し、作業領域の面と比べる。GaneshとSkiaのCPUのラスタは、辺のアンチエイリアス、文字の位置の丸め、グラデーションのディザが少し違うので、違う画素の数、ある色で48より大きく違う画素の数、最大の違いを数え、終了時にそのパスに書く。かな漢字変換の候補を出している間は比べない。

GPUで描くかどうかと、描く速さは、コンソールに次の行で出る。速さの行は、ビューを描いている間、5秒ごとに出る。

| 行 | 意味 |
| --- | --- |
| `browser: gpu: Skia's GPU raster (Ganesh) on V3D 7.1..., OpenGL ES 3.1 Mesa 26.2.3` | ページをGPUで描く。括弧の後はレンダラ名とバージョン(softpipeで`gpu=on`のときは`softpipe (Mesa)`) |
| `browser: gpu: the machine has no GPU, ...` | GPUのないマシン(QEMU)で、プロセッサで描く |
| `browser: gpu: softpipe (Mesa) draws on the processor, ...` | OpenGL ESがsoftpipeになった(V3Dの確認が通らなかったなど、13.8.3)。プロセッサで描く |
| `browser: gpu: none (理由), ...`、`browser: gpu: off (gpu=off), ...` | EGLかGaneshが起動しなかった、`gpu=off`。プロセッサで描く |
| `browser: draw 5.0 s (gpu): N views, F/s; a view X ms (at most Y), K kpx` | 5秒間に描いた表示の数と毎秒の数、1表示の時間の平均と最大、1表示で描いた画素(千)。`(cpu)`ならプロセッサで描いた |
| `browser: draw: on the GPU a view: play P us, flush F us, read back R us (K kpx); N canvases rastered, C us each` | GPUで描いた1表示の内訳。記録の再生(Ganeshが命令を組み立てる、プロセッサの時間)、ドライバへの引渡し、読み戻し(GPUの仕事の終わりを待つ時間を含む)と、その間にGPUで描いたcanvasの記録の数と1回の時間 |
| `browser: draw: video N frames, F/s; P as YUV planes made RGB on the GPU, R made RGB on the processor` | 5秒間に表示した動画のフレームの数と毎秒の数、そのうちGPUで平面からRGBにしたものと、プロセッサでRGBにしたもの |
| `browser: draw: webgl N frames, F/s` | 5秒間に表示したWebGLのフレーム(描画バッファ全体の読み戻し)の数と毎秒の数 |
| `browser: gpucheck: ...` | `gpucheck=`の比較の結果 |

実機(Raspberry Pi 5、`V3D=2`)では、起動の行がv3dのレンダラ名を言うこと、`draw`の行の1表示の時間と、その内訳のうち読み戻しの割合、動画の毎秒のフレーム数と`P as YUV planes`が表示したフレームの数に近いこと、WebGLの毎秒のフレーム数を見る。比べるときは`gpu=off`で同じページを開く。

QEMU(TCG、4コア)では、GPUの経路はsoftpipeの上で正しさだけを確かめる。2026年10月6日、`test_blink_gpu`のページ(14.10)を`gpu=on`で開いた結果は次のとおりである。softpipeはドライバへの引渡し(`flush`)の中でプロセッサが描くので、時間の大半はそこにあり、実機の速さの目安にはならない。

| 場面 | 1表示 | 内訳(再生、引渡し、読み戻し) |
| --- | --- | --- |
| 最初の5秒(ページ全体、2つのcanvas、シェーダのコンパイルを含む) | 平均363ミリ秒、最大1043ミリ秒 | 16、348、18ミリ秒。canvasの記録は1回283ミリ秒 |
| 動画だけが変わる5秒(128×96の矩形) | 平均74〜82ミリ秒、毎秒8表示 | 2.6、66〜73、3.7〜4.0ミリ秒。動画は毎秒8フレームで、すべてYUVの平面のままGPUに渡った |

`gpucheck=`で97の表示(4346万画素)をプロセッサで描き直したものと比べると、1.8%の画素が違い、48より大きく違う画素はなく、最大の違いは27であった。

MSEの試験のページ(14.10、H.264の640×360と426×240)を`NET=user`と`gpu=on`で開くと、シーク、画質の切り替え、`endOfStream`の後の`ended`まで、プロセッサで描くときと同じに進んだ。表示したフレームはすべてYUVの平面のままGPUに渡り、画面の映像の色も正しかった。softpipeでは640×360の矩形を含む1表示に1.7〜1.8秒かかるので、表示できたのは毎秒0.5フレームほどで、残りのフレームは映像のレンダラが落とした。同じく`https://www.youtube.com/watch?v=jNQXAC9IVRw`を`gpu=on`で開くと、ページ(ロゴ、検索の欄、アイコン、サムネイル、プレーヤの角の丸めとコントロール)と動画(VP9の320×240)が画面に正しく出て、動画は19秒の終わりまで進んで次の動画の案内が出た。softpipeではページ全体の1表示に2〜5秒かかった。

### 14.7.3 ページの問いとパネル

ページが利用者に尋ねるもの(`alert`、`confirm`、`prompt`、`<select>`の一覧、許可の確認)は、ブラウザのデータボックス(`SYSDEF_BOX_BROWSER`、14.8)のパネルで、ページのウインドウの中央の上に出す(`lib/libui`の`ui_dlg_open`)。パネルの部品と枠はウインドウマネージャが描き、ブラウザは部品の状態を読み書きするだけである。Enterは強調したボタン(了解、許可)を押し、Escは取消しにする。

| パネル | 番号 | 部品 |
| --- | --- | --- |
| ページからの知らせ(`alert`) | 1 | メッセージ5行、了解 |
| ページからの確認(`confirm`) | 2 | メッセージ5行、取消、了解 |
| ページからの問い合わせ(`prompt`) | 3 | メッセージ4行、入力欄(既定の値を入れて出す)、取消、了解 |
| 選択(`<select>`) | 4 | 一覧(選んである項目に印、`<optgroup>`の名前は【】で囲み、その中の項目は字下げする)、取消、了解 |
| 許可の確認 | 5 | メッセージ4行(「<オリジン>がトレーの内容の読み取りを求めています。」「許可しますか。」)、拒否、許可 |
| ファイルの選択(`<input type="file">`) | 6 | メッセージ2行、一覧(トレーにある仮身と、パネルに入れた仮身の名前)、取消、了解 |

メッセージは半角で52文字(全角は2文字に数える)ごとに折り返し、入りきらない分は「…」で終える。`alert`、`confirm`、`prompt`の間は、ページのスクリプトもエンジンのスレッドもパネルの答えを待つ(Chromiumと同じく、ページはその間止まる)。`<select>`の一覧は、押した操作が終わってからの仕事として出し、選んだ項目を`HTMLSelectElement`に渡して`change`イベントを起こす。許可の確認はサービスのスレッドとは別のスレッドで尋ねるので、その間もほかのサービスは答える。起動の引数の`quit=`で終える時刻が来ると、出ているパネルは答えのないまま閉じる。

アドレス欄はカーソルを持つ。左右の矢印、Home、Endで動かし、押した位置に置き、文字の入力、Backspace、Deleteはカーソルの位置に働く。文字を末尾に入力すると、閲覧履歴のうちアドレスがその文字で始まるもの(スキームと`www.`は省いてもよい)を回数の多い順に補完して色を付けて示す。Enterで補完したアドレスに移り、右矢印かEndで確定し、上下の矢印で候補を替える。次の文字、Backspace、Deleteは補完を外す。

### 14.7.4 ファイルの選択と保存

ページに渡すファイルと、ページが保存するファイルは、実身にする。File System Access API(`showOpenFilePicker`、`showSaveFilePicker`、`port/blink/file_system_access_tessronos.cc`)も同じで、開くときはパネル6で選んだ実身をファイルとして渡し、保存するときは名前をパネル3で尋ねた空のファイルを渡す。ページが書いたものは、書き込みのストリームを閉じたときにブラウザの保存箱の新しい実身になる(開いた実身は書き換えない)。`showDirectoryPicker`は、パネル6で選んだ実身を、ファイル変換がディレクトリとして書き出すもの(仮身を並べた図形、キャビネットのようなもの。並べた仮身の実身がその中身になる)として渡す。書き出したツリーはメモリの中に、オリジンごとのファイルシステムと同じ作りで持ち、ページはその中のファイルとフォルダを読み書きする。ページが変えると、2秒後にツリーの全体を、仮身を並べた図形の新しい実身としてブラウザの保存箱に取り込み、トレーに仮身を置く(選んだ実身は書き換えない)。ディレクトリとして書き出せない実身を選ぶと、選ばなかったものとする(`AbortError`)。オリジンごとのファイルシステム(`navigator.storage.getDirectory()`)は実身ではなく、ブラウザの保存域に持つ(14.4.5)。ファイルと実身の間の変換は、ファイル変換(`lib/libxfu`、`include/ts/xfu.h`)の規則に従う(`port/net/br_objfile.cc`)。ライブラリが読み書きするファイルのツリーは、メモリの中の1つのファイルである。

- `<input type="file">`を押すと、ブラウザはパネル6(14.7.3)を出す。一覧には、トレーで手にある組の仮身が並び、パネルに仮身を入れると一覧の終わりに加わって選ばれる。了解で選んだ実身を、ファイル変換が書き出すときと同じ1つのファイルにする。取り込んだファイルの実身はそのときのファイル、文章の実身はその文字、ほかはxmlTADである。ファイルのバイト列は、ブラウザの中で`/obj/<UUID>/<名前>`というパスで持ち、Blinkにはそのパスのファイルとして渡す。ファイルの大きさと時刻はブラウザの側のサービス(`FileUtilitiesHost`、`port/blink/files_tessronos.cc`)が答え、Blobの格納側(14.4.4)がバイト列を読む。取消のときは、ページに`cancel`イベントが届く。フォルダの選択(`webkitdirectory`)では、パネル6で選んだ実身を`showDirectoryPicker`と同じくディレクトリとして書き出し、その中のすべてのファイルを渡す。ファイルのパスはASCIIの文字だけの`/obj/<UUID>/top/<番号>/file<拡張子>`で、名前を持たないので、上のフォルダの名前を選んだ実身の名前から順に`base_subdirs`に入れて渡す。`webkitRelativePath`(`fsadir/sub/b.txt`など)はそれと名前から作られる。BlinkはこれをAndroidの作りでしか使わないので、パッチ`file_input_type.patch`で、`base_subdirs`があれば使うようにする。Blinkの`FileChooser`はブラウザのプロセスの`FileChooser`のサービスに尋ねる作りで、パネルの答えを渡すインタフェースがないので、パッチ`file_chooser.patch`で、埋め込み側が選んだファイルを渡すインタフェースを足す。このパッチはヘッダを変えるので、ヘッダを使う`br_blink.cc`だけが、パッチを当てたヘッダのツリーを先に探す(`flags.txt`の`-iquote`)。
- ネットワークから保存するものは、受信しながらメモリにためて、終わってから実身にする。設定ウインドウの〈ダウンロード〉で進み具合を見られ、〈中止〉で取得を止められる。一覧は利用者ごとのストア`downloads`に持ち、次の起動でも見られる。保存先は下のとおりブラウザの保存箱のままである。
- ページが保存するもの(`download`属性のリンク、Blobの`blob:`のURL、`data:`のURL)と、応答が`Content-Disposition: attachment`のページは、実身にして実身「システム」の「ブラウザの保存箱」(`SYSDEF_BROWSER_SAVE`、14.8)に入れ、その仮身をトレーに置く。メッセージ行に「「名前」をブラウザの保存箱に入れ、トレーに仮身を置きました」と出す。ファイル変換が取り込むときと同じく、文字のファイルは文章、画像は図形の実身になり、ほかはそのまま持つ。名前は、ページが付けた名前、`Content-Disposition`の`filename`、アドレスの最後の部分の順に取り、拡張子がなければ型から付ける。Blinkはダウンロードをブラウザのプロセスに頼む作りなので、パッチ`local_frame.patch`で、頼む内容をブラウザに渡すフック(`g_download_url_hook`)を足す。バイト列は、`data:`ならフレームが作ったBlobから、`blob:`ならURLのBlobから(ページがURLをすぐに取り消してもトークンで読める)、ほかは取得して得る。保存はそれぞれのスレッドで行うので、同時に終わったものが保存箱に仮身を足すときに保存箱を開き合い、`E_BUSY`で失敗した。いまは保存を1つずつ行う(`br_objfile.cc`の`br_obj_save`)。添付のページは表示せず、表示していたページをそのまま残す。

### 14.7.5 印刷

ページの印刷(メニューの〈印刷...〉と`window.print()`)は、プロセスのアプリの印刷(11.22.6)と同じく、デスクトップに頼む。デスクトップが印刷設定のパネルを出し、PDFを書いてデスクトップに置く。

- ブラウザは、ページをBlinkの印刷のモード(`PrintContext`)で、A4から上下左右に57ポイントの余白を除いた印刷域(CSSの画素で641×970、1画素が96dpiの1画素)に組み、各ページを印刷のときと同じ描き方で描く(`port/blink/print_tessronos.cc`)。`beforeprint`と`afterprint`のイベントを前後に送る。
- 描いた記録は、文字を抜き出すキャンバス(`TextTaker`)に再生する。テキストブロブ(`SkTextBlob`)のうち、拡大と移動だけで描かれ(回転や傾きがない)、1色で塗られ、どのグリフも書体の文字に戻せるもの(`SkFontPriv::GlyphsToUnichars`)は、画素に描かず、図形の文章(`<document>`)にする。文章は、ブロブの位置と大きさ(書体の上端から下端まで)、文字の大きさ、色、書体の名前、太字と斜体、文字を持つ。クリップで見えないブロブは捨てる。そのほかのもの(箱、線、画像、回転した文字など)は画素に描き、PNGの画像にする。
- 印刷のページの実身(`ui_prn_open`)に、ページごとに、A4と余白を持つ図形のレコードを入れ、その画像を余白の内側に置き、文章をその上に重ねる。画像はその実身の資源(`_<ページ>_1.png`)にし、図形の`<image>`の`href`が指す(`port/net/br_print.cc`)。デスクトップに`DT_RQ_PRINT`で頼み、答え(書き出した、パネルを閉じた)を待ってから実身を消す。デスクトップはその文章を、その書体の文字としてPDFに書く(11.22)ので、PDFの中の字は検索や複写ができる。
- `window.print()`は、Chromiumと同じく答えが来るまでページを止める。
- 文章の字の並びはデスクトップの組版による。ブラウザの描いた位置と字の幅が少し違うことがあるので、文章の枠はブロブの幅より字の大きさの半分だけ広くし、折り返さないようにしている。

### 14.7.6 ドラッグ&ドロップ

Chromiumでは、ページの中で始まったドラッグはブラウザのプロセスがOSのドラッグとして運び、ドラッグの先のページに入る、動く、落とすをレンダラに伝える。このブラウザでは、エンジンがその両方を受け持つ。

- ページがドラッグを始めると(`draggable`の要素、選んだ文字、リンク、画像。Blinkが`ChromeClient::StartDragging`を呼ぶ)、ブラウザは運ぶもの(`DataObject`)を覚え、それからのポインタの動きと離すことをドラッグのものとする。動くたびに`DragController::DragEnteredOrUpdated`で、ポインタの下の場所が受け取る操作を求め(`dragover`)、離した場所が受け取るなら`PerformDrop`(`drop`)、受け取らなければ`DragExited`にし、運んだ元に終わりを伝える(`dragend`)。
- ドラッグ中にポインタがウインドウの外へ出ると(`OB_E_LEAVE`)、運ぶもののアドレスか文字を、デスクトップに文字として運ばせる(`DT_RQ_CARRY`の`DT_F_TEXT`)。文章のウインドウや文字を受け取るウインドウの上で離すと、そこに文字が入る。ページのドラッグは、受け取られれば複写、そうでなければ受け取られなかったとして終わる。
- ほかのウインドウからページの上に落とされた仮身(`OB_E_DROP`)は、14.7.4と同じくそれぞれの実身を1つのファイルにして、文章のウインドウから運ばれた文字(`OB_E_TEXT`)は`text/plain`にして、落とされた場所に外から運ばれたものとして落とす。ページが受け取らないときは、落とした側に「このページはここでは受け取りません」と答える。
- 落としたファイルには、File System Accessの受け渡しのトークン(`FileSystemAccessDataTransferToken`)を付ける。ページが`DataTransferItem.getAsFileSystemHandle()`を呼ぶと、トークンからそのファイルのバイト列を持つファイルのハンドルを返す(`port/blink/file_system_access_tessronos.cc`)。落としたファイルの隔離されたファイルシステムは持たないので、`webkitGetAsEntry()`はnullを返す。
- ページが受け取らないファイルや文字を落としても、そのファイルのページに移ることはしない。

### 14.7.7 メディアセッション

`navigator.mediaSession`は、Blinkのモジュール(`modules/mediasession`)をそのまま使い、ブラウザのプロセスが持つ`MediaSessionService`を`port/blink/services_tessronos.cc`が持つ。フレームごとに、再生しているもの(タイトル、演奏者)、再生の状態、受け付ける操作(`setActionHandler`)、操作を伝える先を覚える。システムにはメディアのキーがないので、操作はブラウザのメニューの〈メディア〉(〈再生/一時停止〉、〈前の曲〉、〈次の曲〉、〈停止〉)から与える。メニューで選ぶと、再生中のセッション、なければ最後に何かを通知してきたセッションに操作を伝え、メッセージ行に「「題」に伝えました」と出す。〈再生/一時停止〉は、セッションが再生中なら`pause`、そうでなければ`play`である。その操作を受け付けていないときは「このページはその操作を受け付けていません」と出す。

### 14.7.8 複数のウインドウとピクチャインピクチャ

ページが新しいウインドウを求めたとき(`target="_blank"`のリンク、`window.open`、文書のピクチャインピクチャ`documentPictureInPicture.requestWindow`)は、Chromeと同じ切り分けでウインドウを開く。Chromeでは、開いた側のスクリプトが新しいウインドウの文書を同期して触れるウインドウ(openerを持つウインドウ)は、開いた側と同じレンダラのプロセスに入る。openerを持たないウインドウは、別のプロセスでよい。

- openerを持つウインドウ(`window.open`、文書のピクチャインピクチャ)は、同じエンジンの別のページにする(`br_blink.cc`の`br_open_window`)。BlinkがChromeClientの`CreateWindowDelegate`で新しいウインドウを求めると、開いた側の閲覧コンテキストのグループとエージェント(`AgentGroupScheduler`、ウインドウのエージェントのファクトリ)を受け継いだ`Page`とメインフレームを作る。フレームはopenerを開いた側にして初期化するので、最初の空の文書は開いた側のオリジンを持ち、開いた側のスクリプトがその文書を読み書きできる。新しいウインドウの`about:blank`への移動は、最初の空の文書のままにする。ほかのアドレスへの移動は、そのウインドウのページがアプリケーションに頼み、アプリケーションが取得して読み込む。
- アプリケーション(`br_main.cc`)は、そういうページごとに、ツールパネルを持たない普通のウインドウを作る。ウインドウの大きさは、ページが求めたもの(`window.open`の`width`と`height`、ピクチャインピクチャの`width`と`height`)、なければ開いた側と同じである。ウインドウの通知はブラウザのポートにウインドウごとの番号で届き、キー、ボタン、ポインタの動き、ホイールはそのページに渡る。ページの入力欄にフォーカスがあるときのキーは、ページのウインドウと同じく、かな漢字変換を通る。変換は一つで、最後にキーを打ったウインドウのページに、確定した文字と組み立て中の文字を渡し、候補の一覧もそのウインドウに描く。ほかのウインドウで打ち始めたり押したりすると、組み立て中の文字はもとのウインドウのページで確定する。ピクチャインピクチャのウインドウ(文書のものと動画のもの)は浮かぶウインドウ(`WM_ATTR_FLOAT`、10章)で、ほかのウインドウを前に出しても、その前に出たままである。ページの描画は、ページのウインドウと同じく、変わったところだけを作業領域の面に描いてウインドウに出す。タイトルバーにはページのタイトルを出す。ウインドウを閉じるとページを外し(`Page::WillBeDestroyed`)、開いた側からは`closed`が真に見える。ページのスクリプトが`window.close()`を呼ぶと(スクリプトが開いたウインドウだけ、Blinkが許す)、ウインドウを閉じる。開いた側のページがほかへ移ると、そのピクチャインピクチャのウインドウを閉じる。
- openerを持つウインドウ(`window.open`)にも、普通のウインドウと同じツールパネル(アドレスの欄と戻る、進む、再読込)を付ける。欄は、そのウインドウの文書の実際のアドレスを示し、`pushState`にも従う。`about:blank`はそのまま示す。ピクチャインピクチャのウインドウには付けない。
- openerを持たないウインドウ(`target="_blank"`のリンクは既定でこれ、`window.open`の`noopener`)は、ブラウザのプログラム(`SYSDEF_PROG_BROWSER`)をそのアドレスで起動し、別のプロセスのブラウザのウインドウとして開く。そのブラウザはアドレスの欄、履歴、かな漢字変換を持つ普通のウインドウである。起動する前に、Cookie、ページの保存域、HTTPのキャッシュを書き出しておき、新しいブラウザが今の状態から始まるようにする。エンジンのスレッドは変わった分を渡すだけで(`br_store_flush_async`)、書き込みはストアのスレッドが行い(14.8.1)、新しいブラウザを起動するスレッドがその終わりを10秒まで待つ(`br_store_wait`)。以前はエンジンのスレッドで全体を書き直しており、実機のSDカードではその間(数秒)ページが止まっていた。プロセスの生成(`ob_cre_obj`の`OB_T_PROCESS`)は、動いているブラウザのプログラムのページを共有するので、ヘッダのほかは読まない(5.1)。共有する前は約190MBのプログラムを新しい空間に読むのでQEMUで約3秒、実機のSDカードでは数十秒かかっていた。QEMUでシステムボリュームをSDカードに置いて測ると、リンクを押してから新しいブラウザのプロセスができるまでが、共有の前は235秒、共有の後は0.32秒(うちストアの書き込みを待つのが0.27秒、プロセスの生成が13ミリ秒)であった。生成は、そのために作るスレッド(`br_main.cc`の`apart_thread`)で行い、エンジンのスレッドはその間もページを動かし続ける。以前はエンジンのスレッドで生成を待ち、約7秒止まっていた。生成の途中でプロセスを終えると生成の呼出しが中断されるので、プロセスの終了(`apart_wait`)とウインドウを閉じたときの強制終了は、生成が終わるのを待つ。リンクを押してから新しいブラウザが最初のページを求めるまでは、QEMUで約5.5秒である(生成が約3.1秒、新しいブラウザの立ち上がりが約2.4秒)。プロセスの間で保存したものを同時に書き戻すと後から書いたほうが残るのは、ページの実身を2つ開いたときと同じである。`window.open`はnullを返す。
- 利用者の操作(クリックやキー)の直後でない新しいウインドウの求めは、ポップアップとして開かない。文書のピクチャインピクチャは、Blinkが利用者の操作を確かめる。
- 文書のピクチャインピクチャは、Blinkのモジュール(`modules/document_picture_in_picture`)の`PictureInPictureControllerImpl`をそのまま使い、そのウインドウのページを上のとおりに作る。

動画のピクチャインピクチャ(`HTMLVideoElement.requestPictureInPicture`)では、Blinkが、ブラウザのプロセスの`PictureInPictureService`に、プレーヤの番号とその面(GPUで合成するときの名前)を渡してウインドウを求める。このブラウザでは、メディア要素のプレーヤ(14.6.2)が求められたときに面の名前を作り、`port/blink/pip_tessronos.cc`がサービスを持つ。

- サービスは、動画の大きさ(幅400以下に縮め、比を保つ)のウインドウをアプリケーションに頼み、その大きさをページに答える。アプリケーションはウインドウを作り、プレーヤが新しいフレームを出すたびに、そのフレームをウインドウに黒の縁で比を保って描く。
- ウインドウを押すと、動画が止まっていれば再生、再生中なら一時停止を、要素のプレーヤのインタフェース(`media.mojom.MediaPlayer`)で頼む。
- `document.exitPictureInPicture`(セッションの`Stop`)ではウインドウを閉じる。利用者がウインドウを閉じると、セッションのオブザーバに止まったことを通知し、ページに`leavepictureinpicture`が届く。ウインドウは一度に1つで、別の動画が求めると前の動画はウインドウを出る。
- `pictureInPictureEnabled`は真である。

### 14.7.9 綴りの検査

入力欄(`<input>`、`<textarea>`、`contenteditable`)に打った英語の綴りを、hunspellで調べる(`port/spell/spell_tessronos.cc`)。フレームのクライアントが文字の検査のクライアント(`WebTextCheckClient`)を返し、Blinkは編集している欄の文字を検査に出す(`RequestCheckingOfText`)。検査のクライアントは文字を、ラテン文字と`'`の続きの語に切り、数字や日本語を含む語と1文字の語は飛ばす。辞書にない語にマーカーを付け、候補を3つまで添える。誤った綴りの下には赤い波線が出る。Blinkは、利用者が操作した欄だけを調べる(欄に押して入った、打って中身が変わった)。スクリプトが欄に入っただけでは調べない。

辞書は英語(`en_US`、SCOWLの単語の表から作ったもの、`tools/browser/spell`)で、ブラウザにリンクして組み込む(`build.sh`の`spell_dict`)。hunspellはChromiumのソースツリーにあるものを、Chromiumと同じくICUで大文字と小文字を扱う作り(`OPENOFFICEORG`)でコンパイルする。辞書の行の読み手(`FileMgr`)は移植層のもの(`port/spell/filemgr_ts.hxx`と`filemgr_tessronos.cc`)で、組み込んだバイト列を読む。hunspellの読み手はC++のライブラリの`std::ifstream`を使うが、このプログラムはライブラリのストリームのロケールを用意していないので、使えない。移植層のヘッダを各ソースより先に読ませ(`flags.txt`の`-include`)、hunspellの`filemgr.hxx`の二重読み込み防止の名前を先に定義して、その代わりにする。辞書は最初に語を調べるときに読む。

### 14.7.10 設定ウインドウ

ウインドウのメニューの〈設定...〉(`settings`)は、ブラウザのデータボックスのパネル8を持つ普通のウインドウを開く(`br_settings.cc`)。システム環境設定と同じ`lib/libui`の作りで、部品と枠はウインドウマネージャが描き、ブラウザは部品の状態を読み書きするだけである。ウインドウは専用のスレッドで動かし、ページの動作とは互いに待たせない。データの変更は、そのデータを持つスレッドに渡して答えを待つ(`port/net/br_data.h`)。もう一度選ぶと、開いているウインドウを前面に出す。

| タブ | 内容 |
| --- | --- |
| パスワード | サイト、利用者名、パスワードの一覧。パスワードは既定で伏せ、スイッチ〈パスワードを表示〉で見える。〈変更...〉はパネル9で新しいパスワードを2回尋ねる。〈削除〉は確認してから消す |
| パスキー | 依拠するサイトと利用者名の一覧と〈削除〉 |
| 閲覧データ | 消す種類のスイッチ(Cookie、キャッシュ、localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル、サイトの許可、閲覧履歴、ダウンロードの一覧)、期間、サイトの一覧と〈消去〉 |
| 履歴 | 日時、タイトル、アドレスの一覧と〈削除〉〈すべて削除〉 |
| ダウンロード | 名前、状態、進み具合の一覧(0.5秒ごとに更新)と〈中止〉〈一覧から削除〉 |
| 住所 | 住所の一覧と各項目の入力欄、〈追加〉〈変更〉〈削除〉 |
| 拡張機能 | 読み込んだ拡張機能の名前、バージョン、状態。〈読み込む...〉、〈有効と無効〉、〈削除〉(14.12) |
| カード | カードの一覧(番号は既定で下4桁のほか伏せる)、スイッチ〈カードの番号を表示〉、入力欄、〈追加〉〈変更〉〈削除〉。番号の入力欄は文字を伏せる。セキュリティコードは保存しない |

消去の対象は次のように決める。サイトは登録可能ドメイン(`example.co.jp`など。IPアドレスとlocalhostはそのもの)で比べる。Cookieは作られた時刻、キャッシュは最後に使った時刻、履歴は最後に訪れた時刻、ダウンロードは始めた時刻で期間と比べる。localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル、サイトの許可は、中身のそれぞれの書込み時刻を持たないので、オリジンがストレージを最後に使った時刻(ストア`sites`、1時間単位)で比べる。そのため、期間を指定すると、その期間に使ったオリジンの古いデータも消え、その期間に使っていないオリジンのデータは残る。この形より前のデータは最後に使った時刻を持たないので、次に使うまで期間指定では消えない。全期間なら消える。HSTSの情報とサーバについて知ったこと(HTTP/3の対応、QUICのセッション)は閲覧の跡なので、前者は閲覧履歴と、後者はサイトと期間を指定しないキャッシュの消去と一緒に消す。

別々に開いたウインドウのブラウザは別のプロセスで、それぞれストアの内容をメモリに持つ。消去をそのプロセスだけで行うと、ほかのプロセスの次の保存で元に戻るので、消去とパスワードの変更はストア`sites`に通知のエントリとして書く。各プロセスは3秒ごとに通知を調べ、ほかのプロセスの通知を自分でも実行する(`br_data.cc`)。通知は1日で消す。

保存する住所とカードは、パスワードと同じ方法で暗号化して「ブラウザの保存域」のレコード12に置く(14.4.7.1)。ページへの入力は、ログインのフォームでないフォームの欄(`autocomplete`、なければ`name`と`id`で種類を判断する)にフォーカスが移ったときに、文書ごとに1度、パネル4で使うものを尋ね、選んだときだけ行う。カードの入力は安全なコンテキストのページに限る。パネルの一覧のカードの番号は下4桁のほか伏せる。

## 14.8 実身

| 実身 | UUID | 内容 |
| --- | --- | --- |
| プログラム「ブラウザ」 | `01a0d8c4-5b20-7a10-9b31-6c2e8f4d1a57`(`SYSDEF_PROG_BROWSER`) | プログラム箱にある。レコード1が実行ファイル。idは`web-browser` |
| 原紙「ブラウザ」 | `01a0d8c4-5b22-7b13-9d54-8a6f7e8c9d01` | 原紙箱に並ぶ。複製するとページの実身になる。アドレスは`https://www.tron.org` |
| 「ブラウザのメニュー」 | `01a0d889-46bc-7e8b-8a9c-d2e3f4a5b678`(`SYSDEF_MENU_BROWSER`) | メニューの定義。〈Widevineを導入...〉(`wvinstall`)と〈Widevineを外す〉(`wvremove`)を含む(14.6.3.1) |
| 「編集」 | `01a0d889-46d1-7a2b-8c3d-4e5f6a7b8c9d` | 「ブラウザのメニュー」の〈編集〉の下のメニューの定義。〈トレーへ複写〉(`copy`)、〈トレーへ移動〉(`cut`)、〈トレーから複写〉(`paste`)、〈すべて選択〉(`selall`)、〈ホームページに設定〉(`sethome`)を持つ |
| 「メディア」 | `01a0d8c4-5b28-7c31-9d42-a1b2c3d4e528` | 「ブラウザのメニュー」の〈メディア〉の下のメニューの定義。〈再生/一時停止〉(`mplay`)、〈前の曲〉(`mprev`)、〈次の曲〉(`mnext`)、〈停止〉(`mstop`)を持つ |
| 「ブラウザのデータボックス」 | `01a0d8c4-5442-7c94-8a05-1b2c3d4e5442`(`SYSDEF_BOX_BROWSER`) | ページの問いとファイルの選択のパネル1〜6の定義(14.7.3)と、Widevineを導入する前に示すパネル7「Widevineの導入」の定義(14.6.3.1) |
| 「ブラウザの保存箱」 | `01a0d8c4-5b26-7f17-8b98-ceabbdc0d145`(`SYSDEF_BROWSER_SAVE`) | 実身「システム」にある。ページが保存したものの実身の仮身を並べる(14.7.4) |
| 「ブラウザの利用者データ」 | ユーザごとに作る(UUIDは固定でない) | そのユーザのCookie、サイトのデータ、キャッシュ、履歴など(下の表) |
| 「ブラウザのCookie」、「ブラウザの保存域」、「ブラウザの蓄え」 | `SYSDEF_BROWSER_COOKIE`、`SYSDEF_BROWSER_STORAGE`、`SYSDEF_BROWSER_CACHE` | 実身「かな漢字変換の学習」にある。レコード1に、この形より前のブラウザが書いた、期限付きのCookie、オリジンごとの`localStorage`、HTTPキャッシュが残っていることがある(下の移し替えで空になる)。「ブラウザの保存域」はレコード2にオリジンごとのIndexedDBのデータベースを、レコード3にWeb Pushの購読を、レコード4にService Workerの登録を、レコード5に同期とContent Indexを、レコード6にCache Storageを、レコード7に許可の答えを、レコード8にオリジンごとのファイルシステムを、レコード9にパスキーとパスワードを、レコード10にユーザごとに導入したWidevineのCDMの実身を、レコード11にCDMのファイルを、レコード12に暗号化した住所とカードを、レコード13にユーザごとの「ブラウザの利用者データ」の表を、「ブラウザの蓄え」はレコード2にサーバについて知ったこととQUICのセッションチケットを持つ |

ページの実身のアドレスは、レコード0の文章の本文である。ブラウザは本文のうち、アドレスである最初の行(`://`を含む行か、`about:`か`www.`で始まる行。全角の英数字は半角として読み、前後の空白は除く)を開く。そのため、基本文章編集でアドレスを書き換えた実身や、URL仮身と同じように文章にアドレスを書いた実身は、書いたアドレスのページを開く。このアドレスが実身のホームページである。リンクでページを移っても、戻る、進む、再読込をしても、実身は書き換えない。メニューの〈編集〉〈ホームページに設定〉を選んだときだけ、本文を表示しているページのアドレスの1段落に書き直し、メタデータ`webBrowser`にそのページのタイトルを書く。メタデータの`window`には、ウインドウを閉じるときにウインドウの位置と大きさを書く。戻る履歴と進む履歴は実身に持たず、開くたびに空から始める(ホームページを開く実身に前の回の履歴を持ち越しても、つながりのない履歴になるため)。本文にアドレスがない以前の実身は、メタデータ`webBrowser`の`url`を開く。以前の実身が持っていた`url`、`back`、`forward`は読まず、ホームページを設定するときに取り除く。ページの実身の仮身を開くと、デスクトップがプログラム「ブラウザ」をその実身のUUIDを引数にして起動し、ブラウザは実身のアドレスのページを開く。原紙のアドレスは`https://www.tron.org`なので、原紙から作ったページの実身は最初にトロンフォーラムのページを開く。アドレスを持たない実身は空白のページ(`about:blank`)を開く。実身を指定せずに起動したときは、ブラウザの説明のページ(`about:home`)を開く。同じページの実身をもう一度開くと、表示しているウインドウを前面に出す。

実身「ブラウザの利用者データ」は、ブラウザが代理するユーザ(`ob_get_crd`)ごとに初めて使うときに作る。所有者はそのユーザ、モードは`rw-------`、ACLはなく、固定(`ob_lnk_obj`)で保つ。「ブラウザの保存域」と同じボリュームに置き、どの箱にも仮身を置かない。「ブラウザの保存域」のレコード13(`BRU1`)に、ユーザと実身のUUIDの組を並べる。この表は探す手がかりで、実身の所有者がそのユーザで、グループとほかの人に何も許さず、ACLがないことを確かめてから使う。

| レコード | ストア | 内容 |
| --- | --- | --- |
| 1 | `cookie` | 期限付きのCookie |
| 2 | `storage` | `localStorage` |
| 3 | `cache` | HTTPキャッシュ |
| 4 | `indexeddb` | IndexedDB |
| 5 | `quic` | サーバについて知ったこととQUICのセッションチケット |
| 6〜11 | `push`、`sw`、`background`、`caches`、`perms`、`opfs` | Web Push、Service Worker、同期とContent Index、Cache Storage、許可の答え、オリジンごとのファイルシステム |
| 12 | `history` | 閲覧履歴(アドレスをキーに、最後に訪れた時刻、回数、タイトル。5000件まで) |
| 13 | `hsts` | HSTSの情報(14.5) |
| 14 | `downloads` | ダウンロードの一覧 |
| 15 | `sites` | オリジンがストレージを最後に使った時刻と、消去の通知(14.7.10) |

各ストアのレコードの番号に16を足したものが、そのストアのもう一方のレコードである。

この形より前のブラウザが実身「ブラウザのCookie」「ブラウザの保存域」「ブラウザの蓄え」の共有のレコードに置いたものは、ストアを最初に読むときに、そのユーザのレコードが空なら写し、写せたら共有のレコードを空にする。どのユーザのものかは記録がないので、最初にブラウザを使ったユーザのものになる。

### 14.8.1 ストアの保存

Cookie、`localStorage`、IndexedDB、HTTPキャッシュ、Cache Storageなど、ブラウザが次の起動に持ち越すもの(ストア)は、実身のレコードに置く(`port/net/br_store.cc`)。ストアはキーとバイト列の組(エントリ)の集まりで、全体を1つとして保存するストアは空のキーの1エントリとして扱う。

レコードはログの形(`BRS2`)である。先頭の32バイトがヘッダで、マジック、世代、有効なログの長さ、ヘッダのCRC-32を持つ。その後ろに、エントリの追加(put)と削除(del)が、種類、世代、キーと値の長さ、全体のCRC-32を持つ20バイトのヘッダとともに並ぶ。

- 変わった分の保存は、ログの末尾にエントリを書き足してから、新しい長さのヘッダを書く。ヘッダを書く前に電源が切れても、前のヘッダの長さまでが前の保存のまま読める。
- ログがエントリの中身の2倍(と1MB)を超えたとき、全体を1つとして保存するストアを保存するとき、前の形のレコードしかないときは、すべてのエントリを、ストアのもう1つのレコード(レコード番号に16を足したもの。「ブラウザの保存域」なら17〜24)に次の世代で書き直し、最後にそのヘッダを書いてから、元のレコードを空にする。読むときは、2つのレコードのうちヘッダの正しい、世代の大きいほうを使う。どの時点で電源が切れても、前の保存か新しい保存のどちらかが読める。
- 以前のブラウザの形のレコード(`BRS1`。マジック、長さ、CRC-32、中身)は空のキーのエントリとして読み、次の保存で新しい形をもう1つのレコードに書く。パスワードとパスキー(レコード9)、Widevineの実身(レコード10)、CDMのファイル(レコード11)は、これまでどおり`BRS1`の形でその場に書く。

保存はストアのスレッドが行う。エンジンのスレッドは保存するものを作って渡すだけである。

Cookieのストアは、Cookieごとに1つのエントリを持つ。ウインドウを別に開いたブラウザはプロセスごとに別のCookieの入れ物を持つので、全体を1つとして上書きすると、後から保存したプロセスが、ほかのプロセスのログインを消す。そこで各プロセスは、自分の入れ物で変わったエントリだけを、書き手を締め出して読み直した内容に重ねて書く。書くたびにすべてのエントリを書き直すので、消したCookieの値はログに残らない。各プロセスは3秒ごとにストアの世代を確かめ、変わっていればほかのプロセスが書いた分を自分の入れ物に取り込む。Cookieの保存はエンジンのスレッドを待たず、ストアのスレッドが期限を見る。セッションCookie(期限のないCookie)も、起動の時刻の印を付けて同じストアに書く。ウインドウの間で共有し、OSを起動し直した後の読込みで捨てる。これはChromeの既定(起動時に新しいタブを開く設定)と同じ動きである。

`localStorage`、IndexedDB、Service Workerの登録、許可の答え、HSTSも共有のストアにした(`br_store_register_shared`)。持ち主は今のエントリの全部を渡し、ストアが前に書くか読み取った内容との差を求めて、Cookieと同じように最新の内容に重ねて書く。ほかのプロセスが変えたエントリは持ち主に渡す。同じエントリを両方が変えたときは、後から書いた方が残る。エントリの単位は次のとおりである。

| ストア | エントリ | ほかのプロセスの変更を受けたとき | Chromeとの違い |
|---|---|---|---|
| `localStorage` | 項目ごと | 項目を変え、同じオリジンのウインドウに`storage`イベントを出す | イベントが数秒遅れ、`url`はオリジンになる。`clear()`は項目ごとのイベントになる |
| IndexedDB | データベースごと | 接続がなければすぐ取り込む。接続があれば閉じるまで待ち、バージョンが違うか消されたときは`versionchange`を出す | 下に記す |
| Service Workerの登録 | スコープごと | 登録を足す、または解除する。スクリプトが違い自分のワーカーが動いているときは、このプロセスの間はそのまま | プロセスごとにワーカーが動く |
| 許可の答え | オリジンと許可ごと | 答えを変える | 数秒遅れる |
| HSTS | ホストごと | 規則を変える | 数秒遅れる |

IndexedDBの書込みは、書くトランザクションが走っていないときだけ行うので、保存されるのは常にコミット済みの状態である。ただし、トランザクションの直列化は1つのプロセスの中だけである。Chromeはすべてのタブのトランザクションを1か所で順番に処理するが、ここでは2つのプロセスが同じデータベースを同時に開いて書くと、後から保存した方のデータベース全体が残る。別のプロセスのアップグレードは、開いている接続が閉じるのを待たずに進む(`versionchange`は後から届き、`blocked`は出ない)。古いバージョンの接続が書いた内容は、新しいバージョンの上には書かない。

電源オフと再起動では、デスクトップがプログラムのウインドウに閉じる要求を送り、プログラムの終了を最大20秒待ってから`fs_sync`を呼ぶ。

| ストア | 保存の単位 | 保存するものを作る場所 |
| --- | --- | --- |
| Cache Storage(`caches`) | キャッシュ(キー`c`と番号)と項目(キー`e`と番号)。番号は作った順で、16桁の16進数のキーの順に読み戻すと元の順になる。前の保存から作られたもの、本文を読み終えたもの、消えたものだけを書く | サービスのスレッド(`MakeDelta`) |
| HTTPキャッシュ(`cache`) | キャッシュのエントリ(キーはキャッシュのキー、値は`BRH2`と3つのストリーム)。ストリームの大きさと、ヘッダとメタデータのハッシュが前の保存と同じエントリは書かない。走査のほかにも開かれているエントリ(本文を受信中か、読んでいるところ)は書かずに次の保存に回し、ストアにある前の保存のエントリはそのまま残す | ネットワークのスレッド(`CacheDeltaOnNet`) |
| そのほか(`cookie`、`storage`、`indexeddb`、`quic`など) | 全体 | エンジンのスレッド(登録した関数)。前の保存とCRC-32と長さが同じなら書かない |

エントリ単位のストアは、変わってから決めた時間(Cache Storageは2秒、HTTPキャッシュは1分)の後に、エンジンのスレッドの`br_store_tick`が変わった分を求め(`br_store_register_keyed`の関数)、ストアはエントリのあるスレッドで変わった分を作って`br_store_answer`で渡す。エンジンのスレッドは待たない。終了時(`br_store_flush_all`)と別のプロセスのブラウザを起動するとき(`br_store_flush_async`と`br_store_wait`)は、すべてのストアの変わった分を求め、書き終わるのを待つ。このプロセスの中の実身の読み書きは1つのロックで順に行い、別のプロセスのブラウザが書き込みのために開いている実身は、開けるまで5秒まで繰り返す。

保存のたびに、コンソールに`store <名前> saved`の行で、エントリ単位のストアは変わったエントリの数、書いたバイト数、書き直したかどうか(`afresh`)、かかった時間を出す。試験`test_blink_cachekeep`は、Cache Storageに512KBの応答を24個入れて保存した後に小さい応答を1つ足し、次に起動したブラウザが25個を本文ごと読み戻すことを確かめる。

HTTPキャッシュのストアは、2026年10月16日まで、本文を受信中のエントリもそのとき届いていた分だけで保存していた。次の起動ではそれが完全な応答として返り、Googleマップでは、途中で切れたスクリプト(`Uncaught SyntaxError: Unexpected end of input`)、WebAssemblyのモジュール(`CompileError: ... extends past end of the module`)、アイコンの書体(`OTS parsing error`)になった。切れたスクリプトはメニューのボタンを使えるようにする部分を含み、ボタンは`disabled`のまま、押しても`pointerdown`と`pointerup`だけが届き(使えないフォームの部品には`mousedown`と`click`を送らない)、メニューは開かなかった。いまは、使用中のエントリを保存しない。読み戻すときは、値が`BRH2`で始まらないエントリ(前の形式のもので、途中で切れたものがどれか分からない)と、200の応答で本文の長さが`Content-Length`と違うエントリ(キャッシュ自身が途中で切れたと印を付けたものは除く。それは範囲の要求で残りを求める)を捨て、次の保存でストアからも消す。コンソールの`http cache: N entries read back, M dropped`の行にその数が出る。Cache Storageは、本文をBlobから読み終えた項目だけを本文ごと保存するので、この問題はない。試験`test_blink_cachecut`がこれを確かめる。QEMU(`NET=user KT_SITE=https://www.google.com/maps KT_SITE_CLICK=162,141`)では、修正の後、ボタンは使えるようになり、押すと`pointerdown`から`click`までがChromeと同じ順にボタンに届き、設定のモジュール(`exm=AlIQtb,...`)を読み込んでスタイルを足す。ただし、Chromeではその直後(3ミリ秒)に足されるメニューのパネル(`div.Gs6lwf`)が、60秒待っても足されない。例外やコンソールの出力はない。ページはまだHTTP/3で書体や部品を読み込んでいる最中で(1本あたり毎秒10〜20KB、応答のヘッダを30秒待つ要求もある)、そのことと関係があるかは分かっていない。その後、パネルが足されないのは、地図の部品が置いたWebGL 2のフェンスがいつまでも完了せず、ページの初期化が止まっていたためと分かった(14.6.1)。QEMUで、ページに差し込んだスクリプトから見ると、Chromeでは読み込みから約1.5秒で始まる入力ツールと残りのモジュールの読み込みが、クリックまで始まらず、マップは`ActionFlow error: hung`を報告していた。HTTP/3の遅さとは関係がなかった。修正の後、QEMU(`GL=1`、softpipe)では、入力ツールと残りのモジュールがクリックを待たずに読み込まれ、メニューのボタンを押すとパネル(`div.Gs6lwf`)が足された(softpipeで描くので遅く、押してから約58秒かかった)。このときマップは`invalid-render-detected timer; glError:1282`も報告していた。1282は`GL_INVALID_OPERATION`である。名前の`timer`はマップが描画の正しさをタイマで調べることを指し、時間の問合せ(`EXT_disjoint_timer_query`)とは関係がなかった。この拡張はページに出ていない(14.6.1)。ANGLEのエラーの説明をコンソールに出すようにして(14.6.1)見ると、エラーを残していたのはWebGL 2の`drawBuffers`で、ANGLEは`GL_INVALID_OPERATION: glDrawBuffersEXT: Command requires an extension.`と記録していた。それを直すと、次はインスタンス描画が`glDrawElementsInstancedANGLE`で同じエラーを残した。どちらも、`GLES2Interface`の拡張の名前のメソッドを、OpenGL ES 3のコンテキストでも拡張の関数に渡していたためである(14.6.1)。フェンスが完了しなかった間は、地図の部品が描画の確認まで進まず、このエラーも出なかった。フェンスを直した後は、マップがこのエラーを見て描画が壊れていると判断し、地図を描くのをやめた。実機では地図が黒くなった(2026年10月17日のイメージ)。コアの関数に渡すように直した後、QEMU(`GL=1`、softpipe)では`glError`も`invalid-render-detected`も出なくなった。ただしQEMUでは、DawnがsoftpipeのOpenGL ES 3.1でWebGPUのアダプタを作れるので、マップはWebGPUで地図を描こうとし、地図の部分は黒いままである。WebGPUを止める(`features=-WebGPU`)と、WebGLで地図が描かれる。実機のV3DはOpenGL ES 3.0なので、WebGPUのアダプタはなく、マップはWebGLで描く。ところが、`drawBuffers`などを直したイメージでも、実機では地図が消えた(`glError`も`invalid-render-detected`も出ない)。WindowsのChromeをヘッドレスで動かし、同じ注入スクリプトで調べると、マップは地図をページのスレッドではなくワーカー(`mc0`)で描いていた。地図のcanvasを`transferControlToOffscreen`でワーカーに渡し、ワーカーの`OffscreenCanvas`のWebGL(WebGPUのアダプタがあればWebGPU)で描く。ページのスレッドのWebGLは小さな試しのコンテキストだけである。このブラウザでは、こうしてワーカーが描いたフレームが、ページのcanvas(プレースホルダ)に一度も出ていなかった(14.6.1)。フェンスを直す前は、マップの初期化がフェンスの待ちで止まり、地図を画像のタイルで描いていた。直した後は、ワーカーのベクタの地図に進んで、そのフレームが出ないので地図が消えた。QEMUではsoftpipeが遅く、マップがタイルのまま描いていたので、地図は出ていた。ただしsoftpipeのOpenGL ES 3.1でWebGPUのアダプタができるときは、マップがワーカーのWebGPUを使い、地図は黒かった。プレースホルダにフレームを渡すようにした後、QEMUでは、ワーカーのWebGLとWebGPUで描いた試験のページのcanvasが表示された。

読めないことと空であることは分ける。ストアの読込みが失敗したとき(別のブラウザが30秒より長く実身を持っている、システムのディスクが外れている、ヘッダが読めない)、そのプロセスはメモリでは空から始めるが、ストア全体を置き換える保存は断る(`br_store.cc`の`g_unread`)。Cookieは保存の時刻ごとに読み直し、読めたら今のCookieと合わせて保存する。読めなかったCookieがブラウザに戻るのは次の起動からである。利用者の表(レコード13)が読めないときや、自分の行の実身について尋ねられないときは新しい実身を作らない。行の実身が自分だけのものでないときは、行も実身も残したまま新しい実身を作る。暗号化したストアで鍵が読めないときは、新しい鍵で上書きしない。書込みが失敗した変更は順序を保って64MBまでメモリに残し、15秒ごとに書き直す。起動のたびに`browser: store cookie: N kept (rc)`をコンソールに出す。

## 14.9 未対応のもの

- 移植していないWebのインタフェースは、その機能を持たないブラウザと同じく、グローバルのプロパティが`undefined`になる。それらのインタフェースを返す`navigator`などの属性と操作の扱いは14.4.3にある。
- レイヤーの合成(コンポジタとviz)は使わない。GPUでラスタ化したページも、作業領域の面に読み戻してウインドウシステムに渡す(14.7.2)。WebGLの描画バッファも読み戻してページに描く。WebGL 2は`make GL=1`のときだけ扱う(14.6.1)。WebGLの描画バッファのアンチエイリアスは使わない。
- QUICの接続の移動(ネットワークやポートを替えて接続を保つ)は扱わない。ネットワークを替えての移動には、ネットワークが切り替わったことの通知と、ネットワークを区別するハンドルが要るが、システムはどちらも持たず、Chromiumがそれを扱えるのもAndroidだけである。経路が悪くなったときのポートの移動は、QEMUのTCGでは応答の遅れが経路の劣化に見え、移動が無用に起きるので止めている(応答が遅いことでQUICの接続を捨てる判定も止めている)。
- Web Audioの出力先は既定の1つと`{type: "none"}`(どこにも出さない)である(`AudioContext.setSinkId`、`sinkId`)。マイクの`MediaStream`は`createMediaStreamSource`でWeb Audioに入る。
- 専用ワーカーのホストのないインタフェースを呼んだときは、パッチ`dedicated_worker.patch`でスクリプトが取れなかったときと同じく`error`イベントにする。
- 画像のデコーダのうち、Rustで書かれたPNG、BMP、ICOは使わず、C++のデコーダを使う。
- ChromeのLinux版が出さない次の3つは、Chromeと同じく既定では出さず、起動の行`features=`で名前を挙げたときだけ出す。実装はどれも入っている。Content Index(`ContentIndex`、ChromeではAndroidだけ)、CSS Animation Worklet(`AnimationWorklet`、Chromeでは試験中)、`navigator.share`(`WebShare`、ChromeはWindows、macOS、ChromeOSだけ)。`test_blink_face`が、既定でこの3つがページから見えないことを確かめる。
- MSEのWebCodecsのチャンクの追加(`appendEncodedChunks`、`addSourceBuffer`の`videoConfig`と`audioConfig`)は、Chromeでも試験中の機能(`MediaSourceExtensionsForWebCodecs`)で、Chromeと同じく既定では出さない。起動の行`features=MediaSourceExtensionsForWebCodecs`で出すと、Chromiumの`WebSourceBufferImpl::AppendChunks`がChunkDemuxerにチャンクを渡し、再生できる(`test_blink_mchunk`)。EMEのキーシステムはClear Keyと、利用者が導入したときのWidevine(ソフトウェアのL3だけ)である(14.6.2、14.6.3)。
- 画面の共有(14.4.18)で、共有中の`CaptureController`の操作(フォーカス、拡大縮小、ホイールの転送)は扱わないと答える。`suppressLocalAudioPlayback`は効かない。
- 映像はCPUで復号する(V3Dに動画のデコーダはない)。YUVからRGBへの変換は、ページをGPUでラスタ化するときはGPUで、そうでなければCPUで行う。

## 14.10 試験

試験グループ`v8`(`tests/ktest/ktest_v8.c`)は、`make BROWSER=1 BROWSER_BLINK=1`でビルドしたときにブラウザを動かして確かめる。ブラウザが入っていないときは各試験を飛ばす。試験のタスクがループバックでHTTPのサーバになり、ブラウザのプロセスが求めたパスと、画面の画素と、最後に表示したページのタイトルを見る。ページの実身はタイトルを持たないので(14.8)、タイトルは、試験がブラウザに与える`title=`の行で、ブラウザが終わるときにファイルに書かせる。

| 試験 | 内容 |
| --- | --- |
| `test_script`、`test_skia` | V8でJavaScriptとWebAssemblyを実行する、Skiaで描いた図形と日本語をウインドウに出す |
| `test_jit` | `tests/uprog/v8prog.cc`を引数`bench`で起動し、再帰、型付き配列のふるい、オブジェクト、文字列のループと、WebAssemblyのループを実行させる。値が正しく、再帰とふるいの関数がMaglevかTurbofanの機械語になったこと(`%GetOptimizationStatus`)を確かめる。続けて`bench --jitless`で同じループをインタプリタで実行させ、スクリプトが自分で測ったJavaScriptのループの時間(コンソールの`v8prog: result ok <ミリ秒>`)が機械語の版の2倍を超えることを確かめる。プログラムとV8の起動はどちらの版でも同じだけかかり、ループより長いので、起動から終了までの時間は比べずに出力だけする |
| `test_browser`、`test_blink` | リンクと転送をたどり、ページを移ってもページの実身の本文とメタデータが変わらないこと、`sethome`の行(メニューの〈ホームページに設定〉と同じ処理)で本文がそのページのアドレスになり、次に開くとそのページから始まることを確かめる、Blinkでスタイルシートとスクリプトのあるページを組んで描く、画像4種類、Cookie、`localStorage`、canvasの2D |
| `test_blink_caret` | 入力欄、テキストエリア、アドレス欄で矢印、Home、End、Delete、Shift+左が働き、文字がカーソルの位置に入る。ページの欄は英語と日本語(変換中でないとき)で確かめる |
| `test_blink_settings` | メニューの〈設定〉のウインドウにパスワードとパスキーが並び、パスワードは伏せてある。変更と削除が次の起動にも残る |
| `test_blink_clear` | Cookieとサイトのデータ(localStorage、IndexedDB、Cache Storage、Service Worker、サイトのファイル)を、期間、サイト、全体で消し、次の起動で確かめる |
| `test_blink_users` | 共有のレコードに残った以前のデータが最初の利用者に移り、共有のレコードが空になる。別の利用者からは見えない |
| `test_blink_history` | 閲覧履歴が次の起動で並び、1件消せる。アドレス欄の補完で残ったページに移る |
| `test_blink_hsts` | HSTSの情報が次の起動に残り、閲覧履歴の消去で消える。規則は試験の行で足す(TLSのサーバを使わないので、応答のヘッダからの登録はChromiumの処理に任せ、ここでは試さない) |
| `test_blink_dl` | 受信中の進み具合、〈中止〉、完了、一覧の持ち越しと一覧からの削除 |
| `test_blink_autofill` | 住所とカードの登録、変更、削除、番号を伏せること、暗号化、パネルで取消なら入力しないこと、選べば入力すること |
| `test_blink_input` | 入力欄への入力とフォームの送信、ホイールのスクロール、かな漢字変換、`target="_blank"`のリンクを別のプロセスのブラウザが開くこと(押してから新しいブラウザがページを求めるまでの時間をコンソールに出す。待つのは20秒まで)。戻ったフォームの欄は打った文字を保つ(Chromeと同じ) |
| `test_blink_hwheel` | 横のホイール(`HID_WHEEL_H`。`dz`が正で右。タッチスクリーンの二本指を左へ動かした向き)が、右向きの`wheel`イベント(`deltaX`が正)としてページに届き、ページを横にだけ動かすこと。縦のホイールは縦にだけ動かし、左へ回すと左端に戻ること |
| `test_blink_spa` | 単一ページのアプリケーションのセッション履歴。`replaceState`、`pushState`、フラグメントの項目、Alt+←と→、ページの`navigation.back()`と`history.go()`、`popstate`と`hashchange`、別の文書から戻ったときの状態のオブジェクトとスクロール位置とNavigation APIの項目、アドレスの欄が追いつくこと(`addr=`の行で欄が出したアドレスを書き出す) |
| `test_blink_ui` | リンクの上のアドレスをメッセージ行に出す、右ボタンの`contextmenu`、Ctrl+＝とCtrl+0の拡大(`devicePixelRatio`)、Ctrl+A・Ctrl+C・Ctrl+Vのトレーを通した複写、`requestFullscreen`とEsc、Ctrl+クリックで別のプロセスのブラウザ、`beforeunload`の確認(取消と了解) |
| `test_blink_face` | ページから見えるブラウザの姿(14.4.12の表)をコンソールに書き出す。ChromeのLinux版が出さない`navigator.share`、`CSS.animationWorklet`、Content Indexが既定で見えないこと。`serif`が`Noto Serif JP`と同じ幅で`sans-serif`と違い、`monospace`が`Noto Sans Mono CJK JP`と同じ幅でラテン文字がどれも同じ幅であること |
| `test_blink_api5` | CSS Paint APIのワークレットが箱を緑に描き、Animation Workletのアニメーション(起動の行`features=AnimationWorklet`で出す)が不透明度0.5に保つ。別のサイトのiframeがStorage Accessのハンドル(`requestStorageAccess({all: true})`)でファーストパーティの`localStorage`、`sessionStorage`、IndexedDB、Web Locks、Cache Storage、OPFS、見積もり、`createObjectURL`、`BroadcastChannel`を使う。加速度センサーはセンサーがないので`NotReadableError`。アプリのバッジ3がウインドウのタイトルの前に出る |
| `test_blink_mpa` | MSEの`audio/mpeg`と`audio/aac`(RustのMPEGとADTSのヘッダのパーサ)、`<audio>`のMP3とFLAC(Symphonia)の長さと再生(FLACはChromiumのメディアの試験のデータの`bear.flac`) |
| `test_blink_wam` | Web Audioの出力先(`setSinkId`の`{type: "none"}`と既定、ない機器は断る)と、マイクの音を`createMediaStreamSource`から`AnalyserNode`で読むこと |
| `test_blink_mchunk` | `appendEncodedChunks`が既定では無いこと、`features=`で入れると、VP8に符号化したキャンバスの10フレームをMSEに渡して再生できること |
| `test_blink_iwa` | 起動の行`isolated=`で許したオリジンで、`crossOriginIsolated`が真で`TCPSocket`、`UDPSocket`、`TCPServerSocket`、`maximize`があり、試験のサーバにTCPで要求して答えを読み、`TCPServerSocket`にページのTCPでつないで送ったものを受け、バインドしたUDPに別のUDPから送ったものを送り手のアドレスとともに受け、`maximize()`(ウインドウの管理の許可のパネルにEnterで答える)でウインドウの幅が画面の幅になること |
| `test_blink_speech` | マイクに半秒の無音、英語の文を話した音(`tests/ktest/data/speech_hello.raw`、Fliteで作った16kHzのモノラル)、無音を渡し、認識が`audiostart`、`soundstart`、`soundend`、`audioend`、`end`を順に通知して「hello」を含む確定した結果を答えること。続けて、声が`ja-JP`(既定)と`en-US`の2つで、日本語と英語の文がそれぞれ鳴る時間をかけて`end`で終わり、話している間に`cancel()`した発話が`interrupted`で終わり、その後に並べた発話が話されないこと。音の出力の装置がない機械では飛ばす |
| `test_blink_wgw` | `make GL=1`のとき、起動の行`features=WebGLOnWebGPU`でWebGLのコンテキストができ、緑で消した色と、全体を覆う赤い三角形を`readPixels`で読み返せ、`getError`が0であること |
| `test_blink_glgpu` | `make GL=1`のとき、緑で消したWebGLのcanvasを`copyExternalImageToTexture`でWebGPUのテクスチャにコピーして緑が読め、赤で消したWebGPUのcanvasをWebGLの`texImage2D`でテクスチャにしてフレームバッファから赤が読めること |
| `test_blink_v3dfall` | `make GL=1`のとき、`/v3dfall`のページを`gpufake=`で開く。DRMシムが模擬したV3D(x64ではKaby Lake GT2)で、Mesaのプラットフォームがv3dドライバ(x64ではiris)を選び、画面を作り、確認のフレームが合わずにsoftpipeに戻ること(`driver=v3d step=6`、x64では`driver=iris step=6`。irisの要求がブラウザの`ioctl`で断られると画面ができず`step=5`になる)、その後もWebGL 2のcanvasが赤で消えて読め、レンダラ名が`softpipe`で、2Dのcanvasも描けること |
| `test_blink_webnn` | 起動の行`features=MachineLearningNeuralNetwork`で`navigator.ml`があり、コンテキストの限度にfloat32とint32があり、定数との行列積から3を引いて`relu`を通したもの、行ごとの`softmax`、3×3の画像の`conv2d`と`maxPool2d`、int32の和とfloat32への変換が、計算どおりの値になること |
| `test_blink_aa` | WebGLの白い三角形を黒に描き、`antialias`が真のときは`getContextAttributes().antialias`が真で`SAMPLES`が4、縁に中間の色の画素があり、偽のときは縁が硬いこと。`make GL=1`のビルド(ANGLEがsoftpipeの上)では、softpipeがANGLEにマルチサンプルを出さないので、`antialias`が偽になってもよい |
| `test_blink_ai` | `LanguageDetector`が使え(`available`)、日本語の文を`ja`、英語の文を`en`と確信度0.5より上で判定し、`Translator`、`Summarizer`、`LanguageModel`は`unavailable`で、`Summarizer.create()`が断られ、`Writer`、`Rewriter`、`Proofreader`が無いこと |
| `test_blink_pad` | 試験のソフトウェアのUSBコントローラ(6.3.2、`device/usb/vhc.c`)に挿したLogitech Dual Action(DirectInputのモード)が、ボタンを押すまでは見えず、押すと標準の配置で現れ(ボタン16、軸4)、Aの押しと離し、スティック、十字キーが届き、`gamepadconnected`が1回来ること |
| `test_blink_midi` | 同じコントローラに挿した、送られたものを返すUSBのMIDIの機器が、入力と出力1つずつとして機器の名前で見え、システムエクスクルーシブを許したアクセスで、ノートと`F0 7D 01 02 03 04 05 F7`を送ると同じものが入力に戻ること |
| `test_blink_pdf` | PDF(PDFiumの試験の`hello_world.pdf`)のページが白く灰色の背景に描かれ、タイトルがファイルの名前になること。ページが`pdfViewerEnabled`の真、5つのプラグインと2つのMIMEの型を見て、iframeのPDFが画像の1枚のページになること |
| `test_blink_pw` | 前の形のブラウザが平文で書いたレコード9(試験が書く)を読めること。パスキーと、`PasswordCredential`の`store()`とログインのフォームのパスワードをパネルで許して保存し、起動し直したブラウザで、そのパスキーで署名し、`get({password})`で選び、フォームの欄にフォーカスして入れること。終わった後、レコード9が暗号化の形で部分が1つ、その鍵の実身の所有者が部分のユーザでモードが`rw-------`、ACLがないこと。ボリュームを書き出してから、起動したディスク全体を読み、3つのパスワードがどこにもないこと(パスワードはページのスクリプトが組み立て、タイトルには合ったかどうかだけを出し、試験の中では逆さに持つので、蓄えやタイトルやカーネルのイメージには現れない) |
| `test_blink_av1` | AV1を扱えると答えること(`canPlayType`、MSE、WebCodecs、MediaCapabilitiesの480pはなめらか、1080pはなめらかでない)と、WebMのAV1の480×360の動画がフレームを出して再生されること(Chromiumのメディアの試験のデータの`bear-av1-480x360.webm`)。キャンバスの1フレームを`VideoEncoder`でAV1のキーフレームに符号化し、MediaRecorderがAV1を受け付けること |
| `test_blink_mseav1` | MSEでAV1を扱えると答えること(`isTypeSupported`のMP4とWebMの`av01`、MediaCapabilitiesの`media-source`の640×360)と、断片に分けたMP4のAV1(`bear-av1.mp4`、320×240)とWebMのAV1(480×360)をそれぞれ`SourceBuffer`に入れて、フレームを出して再生すること。読めないデータを入れた要素が誤りで終わったあと、別の要素でWebMのVP9が再生されること |
| `test_blink_wv` | 導入していないとき、`/wv`のページの`com.widevine.alpha`が`NotSupportedError`、Clear Keyが使えること。メニューの〈Widevineを導入...〉のパネル7にEscで答えると何も取らないこと。`make ... WV_TEST=<ディレクトリ>`で、開発のマシンでリポジトリから取ったインデックスとパッケージ(`wv_Packages`、`wv.deb`)を試験のディスクの`/boot`にだけ載せたときは、起動の行`wvrepo=`で試験のサーバのコピーをリポジトリとし、パネル7にEnterで答えると、取得、SHA-256の照合、展開を経て、レコード10が指すモード`rw-------`の実身にライブラリが入り、同じページで`SW_SECURE_DECODE`のWidevineが使え、`HW_SECURE_ALL`は断られること。起動し直したブラウザが実身からCDMを読み込み(コンソールにバージョンが出る)、〈Widevineを外す〉をパネル2の了解で選ぶと、行と実身が消え、Widevineが断られること。暗号化された動画の再生と、鍵の要求の生成は試さない |
| `test_blink_find` | ページの中の検索。Ctrl+Fのパネルに「apple」、3つの一致のうち次、次、前へ移り、表示がその中ほどへ動くこと、動いている一致の橙色 |
| `test_blink_keep` | フォームの`POST`、次に起動したブラウザがCookieと`localStorage`を読み戻す |
| `test_blink_media` | WebGL(三角形の描画と画面の画素、`readPixels`、2×2のテクスチャを最近傍で貼ったフレームバッファオブジェクトの画素、WebGL 2のコンテキストでGLSL ES 3.00の頂点配列オブジェクトと整数のユニフォームで描いた色)、音声の長さ、`crypto`、`preload`のない動画を取得しないこと。`GL=1`でないときはWebGL 2がないことを確かめる |
| `test_blink_gpu` | `make GL=1`のときだけ。`/g`のページを`gpu=on`(softpipeでもGPUの経路を使う)と`gpucheck=`の行で開く。ページは、角の丸い箱のグラデーションと影、文字、1色の箱、画像、2Dのcanvas(塗り、円、グラデーション、文字、`putImageData`、別のcanvasへの`drawImage`)、WebGLの三角形、1色のVP9の動画(64×48、`/g.webm`、ループで再生)を持つ。画面の画素がそれぞれの色であること、`getImageData`がGPUのcanvasから読み戻した色(塗り、円、置いた四角、別のcanvasにコピーした塗り)と、動画のフレームが表示されたことをタイトルで確かめる。プロセッサで描き直したビューと比べて48より大きく違う画素が0.5%以下であること、動画のフレームがYUVの平面としてGPUに渡ったこと、canvasがGPUでラスタ化されたことを、`gpucheck=`のファイルで確かめる |
| `test_blink_wasm` | WebAssemblyのモジュールをスクリプトのバイト列と`/t.wasm`(`instantiateStreaming`)から作り、足し算、テーブル経由の呼出し、メモリの読み書き、10万回のループ、トラップを確かめる。1ページのメモリのアドレス0、65532、65536、2147483647の読出しが0、0、トラップ、トラップになる(後の2つはトラップハンドラによる) |
| `test_blink_image` | 画像へのアドレス(16×16のPNGとJPEG)を直接開くと、画像の文書として等倍で描かれる。2000×1000のBMP(左が赤、右が青)はウインドウに縮めて両方の色が見え、赤の側をクリックすると等倍になって赤だけが見える(試験はデスクトップを起こしてクリックをウインドウに渡す) |
| `test_blink_worker` | `blob:`のURLから専用ワーカーを作ってすぐにURLを取り消し、ワーカーが`importScripts`で`/wk.js`を読み、転送された`ArrayBuffer`の和と別のBlobのURLを`fetch`で読んだ中身を返す。ページはBlobの`text()`、`slice`、`blob:`のURLの`fetch`、応答の`blob()`を読む |
| `test_blink_shared` | 同じアドレスと名前の`SharedWorker`を2つ作ると1つのワーカーになり(`/sw.js`を1度だけ求める)、それぞれのポートに送ったメッセージに、接続の番号を付けて答える |
| `test_blink_file` | トレーに文章の実身の仮身を置き、`/fl`の`<input type="file">`を押してパネル6でEnterで選ぶと、ページがファイルとして文字を読む。続けてページが保存する`download`属性のリンク(`/dl`)、すぐに取り消したBlobの`blob:`のURL、`data:`のURLと、添付(`Content-Disposition: attachment`)で答える`/att`が、ブラウザの保存箱の4つの実身になり、添付は表示されない |
| `test_blink_codecs` | `/wc`のページが、VP8の`isConfigSupported`、赤いcanvasの3フレームのVP8の符号化と復号(最初のフレームの画素が赤)、Opusの符号化と復号、canvasで作ったPNGの`ImageDecoder`での復号をタイトルに書く |
| `test_blink_print` | `/pr`のページが`window.print()`を呼ぶと、デスクトップの印刷設定のパネルが出て、Enterで1ページのPDFができる(`print=`の行で、印刷したページ数、デスクトップの答え、文字のまま印刷したブロブの数を書かせる。見出しと段落の2つ以上)。`afterprint`がページに届く |
| `test_blink_drag` | `/dd`の赤い箱をポインタで緑の箱まで運ぶと、緑の箱が運んだ文字を受け取る。続けて文章の実身の仮身(`wm_obj_drop`)と文字(`wm_obj_drop_text`)を緑の箱の上に落とすと、ページがファイルの文字と落とされた文字を受け取り、落としたファイルの`getAsFileSystemHandle()`のハンドルからも同じ文字を読む |
| `test_blink_session` | `/ms`のページがメディアセッションのタイトルと操作の受け手を置き、メニューの〈メディア〉〈再生/一時停止〉と〈次の曲〉(`menu=`の行)が`play`と`nexttrack`として届く |
| `test_blink_sw` | `/swr`のページがService Workerを登録し、ワーカーが`clients.claim()`でページを制御する。ページの`/swr-data`の取得と、ページが移るアドレスにワーカーが答え(サーバは`/swr`とワーカーのスクリプトしか求められない)、ページとワーカーが`postMessage`でメッセージを交わす |
| `test_blink_swframe` | `/swf`のページがService Workerを登録して制御された後、`/swf-frame`のiframeを置く。ワーカーはその移動の`fetch`イベントに`/swf-inner`を取得して答え、iframeの文書は制御され、その`/swf-data`の取得にもワーカーが答える。ワーカーはiframeからのメッセージに、クライアントが2つ(`top-level`と`nested`)と答える。サーバは`/swf-frame`と`/swf-data`を求められない |
| `test_blink_owl` | 専用ワーカーが`OffscreenCanvas`のWebGLで赤く消した画素を読み戻すと`255,0,0,255`(`make GL=1`)。ページの`requestAnimationFrame`のコールバックとタイマーがそれぞれ120ミリ秒かかり、`long-animation-frame`の項目(50ミリ秒以上、スクリプトの記録あり)が届く |
| `test_blink_glw` | `/glw`の2つの専用ワーカーが`OffscreenCanvas`のWebGL 1と2で、ページがWebGLのコンテキストを次々に(4秒の間、少なくとも5つ)作って、数値を含むシェーダをコンパイルしながら描き続け、読み戻した画素が赤(ワーカー)と緑(ページ)で、プログラムのリンクがすべて成功し、ブラウザが落ちない(`GL=1`でないときワーカーのWebGL 2は`none`)。描いた後、WebGL 2のワーカーは画素を`PIXEL_PACK_BUFFER`に読み出してフェンスを置き、`clientWaitSync(sync, 0, 0)`をタイマから3秒まで繰り返して待ってから`getBufferSubData`で読む。どちらのワーカーも、リンクを`KHR_parallel_shader_compile`の`COMPLETION_STATUS_KHR`で同じく待つ。`GL=1`では、フェンスが信号状態になって読んだ画素が赤で、リンクが完了し、`getError`が0であること。ページの`@font-face`のフォントは、HTTPキャッシュだけからの読込みが`ERR_CACHE_MISS`になった後、サーバに求められる。ページの実身の本文の`trace=exceptions`の段落で、例外がコンソールに出る |
| `test_blink_csp` | `/csa`(CSPでnonce`aaa`を求める)のスクリプトが`/csb`(nonce`bbb`)へ移り、`/csb`のnonce付きのインラインと外部のスクリプトが動く(前の文書のCSPに縛られない)。nonceは属性から隠れ、プロパティには残る。ほかのオリジン(`localhost`)への`redirect: 'manual'`の`fetch`は、CORSのヘッダの付いたリダイレクトで止まって`opaqueredirect`になり、その先(`/cst`)は求められない。同じオリジンへの`POST`の`fetch`とXMLHttpRequestは`Sec-Fetch-Site: same-origin`と`Origin`を付けて送られ、CORSで断られない。続けて`http://localhost:18172/csc`を開くと、ほかのサイト(`127.0.0.1`)へリダイレクトされ、そのリダイレクトの応答が付けた`SameSite`のないCookieが残り、移った先のページの同じオリジンへの要求で送られる |
| `test_blink_hist` | iframeの別の文書への移動とフラグメントで`history.length`が1つずつ増え、`history.back()`でフラグメント(`hashchange`)、続いて前の文書(`back_forward`)に戻る。iframeの再読込は`reload`、リダイレクトで来た文書は`redirectCount`が1。同期のXMLHttpRequestの`statusText`が`Funky Reason`。箱の`behavior: 'smooth'`のスクロールはすぐには動かず、2.5秒後に行き先にある。専用ワーカーの`requestAnimationFrame`が時刻を受ける。1.5秒空けて2回に分けて送る本文を、`fetch()`が届いた分ずつ読む |
| `test_blink_api2` | `/api2`のページで、試験のサーバの`/ws`との`WebSocketStream`の往復、支払いの手段がないことを答える`PaymentRequest`、パスキーを作って署名で入る(2つのパネルにEnterで答え、署名を返された公開鍵で`crypto.subtle`が確かめる)、試験のサーバをIDプロバイダとするFedCM(アカウントをEnterで選び、トークンを受け取る) |
| `test_blink_fsa` | `/fsa`の1つめのボタンで`showSaveFilePicker()`の名前のパネルにEnterで答え、ブラウザの保存箱に実身ができて仮身がトレーに入る。2つめのボタンの`showOpenFilePicker()`でその仮身をパネル6で選び、書いた文字を読み戻す |
| `test_blink_devices` | `/dev`のページが、電池(充電中で満充電)、画面の消灯の防止、キーボードの配列の表、関連アプリ、2つめの画面、アプリのバッジ、Bluetooth(アダプタなし)、MIDI(許可のパネルにEnterで答え、ポートなし)、HIDとUSBの機器(なし)、ゲームパッド(4つの空き)、画面の向き(`landscape-primary`、固定は`NotSupportedError`)、`navigator.connection`(`4g`)、折り畳み(`continuous`)、仮想キーボード、ウインドウの制御の領域、プロトコルのハンドラの登録、`launchQueue`、加速度センサー(なし)の答えをタイトルに書く。ボタンを押すと、HIDとUSBの機器を求め(どちらも選ぶものがない)、`navigator.share()`の文字がトレーに入る |
| `test_blink_cookie` | `/ck`のページが、`document.cookie`で入れたCookieを`cookieStore.get`で読み、`cookieStore.set`で入れたものが`document.cookie`に現れ、`set`の`change`イベントが届き、`getAll`の数と`delete`を確かめる。初めに自分のCookieを消し、数えるのも自分のものだけにするので、前に回った試験が同じホストに残したCookie(Cookieはポートで分かれない)に左右されない |
| `test_blink_serial` | `/ser`のボタンを押すと`requestPort()`のパネルが出て、Enterで最初のポート(コンソールの`sera`)を選ぶ。ページがポートを開いて`webserial-hello`の行を書き(ログに出る)、しばらく読んで、`getPorts()`の数を数えて閉じる |
| `test_blink_share` | パネルの〈取消〉で`NotAllowedError`になる。ブラウザの後ろに隠れた赤いウインドウを選ぶと、`displaySurface`が`window`で、ラベルがウインドウの名前で、フレームが赤い(画面からでなくウインドウ自身のピクセルから読んでいる)。閉じると`ended`。ブラウザ自身のウインドウを音も選ぶと`browser`で、音のトラックに発振器の音が入る |
| `test_blink_storebusy` | 利用者の実身をテストが40秒近く持ったままブラウザを起動してCookieを足す。読めなかった間の保存で前のCookieが消えず、次の起動で古いCookieと新しいCookieの両方がある |
| `test_blink_ext` | 2つの拡張機能を読み込む。権限のパネルの〈取消〉と〈読み込む〉、設定のタブの見た目、CRXとモジュールのワーカー、deflateのZIP。ルールが画像、`fetch`、フレーム、ウインドウの文書、WebSocketに効くこと、本当のリダイレクト(`redirected`が真)、`regexSubstitution`、`transform`、`regexFilter`、動的ルール、ヘッダの両方向の`modifyHeaders`。`scripting`、`alarms`、`i18n`、見るだけの`webRequest`、`action`、`contextMenus`、`tabs.sendMessage`、`tabs.create`、拡張機能のページ。宣言の少ない拡張機能が、名前を持たず、直接呼んでも拒否され、ホストのない`redirect`と`modifyHeaders`が効かず、CORSを越えられないこと。`document_idle`のスクリプトが1回だけ走ること。次の起動で一覧の仮身から読み、無効にする、削除する |
| `test_blink_ubol` | `make EXT_TEST=<ZIP>`で試験のディスクに載せた実在の拡張機能を読み込み、ルールとワーカーとボタンのページをコンソールと画面で確かめる。ZIPはリポジトリにも試験の既定にも入れないので、与えないときは飛ばす |
| `test_blink_meet` | `/meet`のページで、送り手のコーデックにVP8、VP9、H.264、AV1とOpusがあり、カメラの許可の問い合わせが`prompt`、`crossOriginIsolated`が偽であることを確かめる。VP8、VP9、H.264、AV1のそれぞれについて、2つの`RTCPeerConnection`で640×360、毎秒30フレームのcanvasをそのコーデックだけで送り、符号化と復号の毎秒のフレーム数、大きさ、エンコーダの名前を書き出す(QEMUでは実機より遅いので値は記録として出す)。最初の組では発振器の音も送り、受け手の`<audio>`が再生して、受信の音のサンプルの数(`totalSamplesReceived`)が増えること(WebRTCの音が引かれていること)を確かめる。ボタンを押して`getDisplayMedia`を呼び、パネルにEnterで答えると、`displaySurface`が`monitor`で音のトラックのないストリームが来て、そのフレームが読めることを確かめる |
| `test_blink_coi` | `Cross-Origin-Opener-Policy: same-origin`と`Cross-Origin-Embedder-Policy: require-corp`で返す`/coi`のページが、`crossOriginIsolated`が真で、`SharedArrayBuffer`と共有の`WebAssembly.Memory`を持ち、`SharedArrayBuffer`を渡した専用ワーカーも分離されていて同じメモリに書けることを確かめる。別のオリジン(`localhost`)の画像は、`Cross-Origin-Resource-Policy: cross-origin`のものは読め、何も言わないものは読めない。同じオリジンのiframeは、COEPを持つ文書は入り、持たない文書は止められてエラーの文書になる。IndexedDBで、アップグレードの最中に頼んだ2つ目の`open`がアップグレードの作ったストアで`transaction()`でき、バージョン2へのアップグレードが2つの接続に`versionchange`を伝えて閉じさせてから動き、削除が接続の閉じるのを待って終わる |
| `test_blink_rtc` | `/rtc`のページの2つの`RTCPeerConnection`を、提案、応答、候補の受け渡しで繋ぎ(ICEが`connected`になる)、データチャネルを開いて両方向にメッセージを送り、canvasの映像のトラックを送って、受け手が`track`イベントを受け、フレームを復号したこと(`getStats`)を確かめる |
| `test_blink_api3` | `/api3`のページをクリックすると(`queryLocalFonts`は利用者の操作を要る)、WebXR(没入型なし、インライン)、`navigator.ink`、メディア要素の`remote`、`navigator.login`、CSSのペイントのワークレットのモジュール、`getScreenDetails`(許可のパネルにEnterで答え、画面が1つ)、`queryLocalFonts`(許可にEnter、フォントがある)を確かめる。クリックの前に、ストレージのバケットのデータベースとキャッシュが既定のバケットと分かれることとバケットの削除、古いFile System APIのファイルの書込み、一覧、読み戻しを確かめる |
| `test_blink_webgpu` | `make GL=1`でないときは、`/wgpu`のページの`requestAdapter()`がnullを返すことだけを確かめる。`make GL=1`では、ページが、互換の水準のアダプタとデバイスを得て、計算シェーダ(1、2、3、40を3、5、7、81に)を走らせてバッファをマップして読み、緑に消したcanvasの左下半分に赤い三角形を描く。`drawImage`で2Dのcanvasにコピーした色と、画面のcanvasの色を確かめる。デバイスのエラーがないこと |
| `test_blink_windows` | `/win`のページの右の4つのボタンを順に押す。`window.open`で開いたウインドウ(`/winc`)のタイトルを開いた側が読み、`postMessage`の往復ができ、そのウインドウの入力欄にローマ字で打った「あい」がかな漢字変換で入り、開いた側が閉じると`closed`が真になる。文書のピクチャインピクチャのウインドウが求めた200×150で、開いた側の要素を移せる。`noopener`のウインドウは`window.open`がnullを返し、別のプロセスのブラウザが`/wina`を開いてタイトルをファイルに書く。動画のピクチャインピクチャのウインドウに入り(ウインドウは64×60)、出ると`leavepictureinpicture`が届く |
| `test_blink_notify` | `/note`のページの3つのボタンを順に押す。表示の許可をパネルで与え、ウインドウの通知をデスクトップの通知のウインドウの〈開く〉で押すと`click`が届く。Service Workerの通知は`getNotifications`に1件あり、そのボタン〈返信〉を押すとワーカーに`notificationclick`(`action`が`a1`)が届く。3つ目の通知を〈閉じる〉で閉じると`close`が届く |
| `test_blink_push` | 試験のサーバがautopushと同じ手順を話すプッシュサービス(`/push`のWebSocket)を持つ。`/pushp`のページが購読し(通知の許可をパネルで与える)、購読の宛先がサービスのもので、鍵が起動の行`pushkey=`で与えたもの(RFC 8291の例の受け手)であり、`getSubscription`が見つける。サービスがhttp_eceで暗号化して送るメッセージ(aes128gcmとaesgcmの2つ)が復号され、ワーカーの`push`イベントの文が送ったものになり、受け取りが返る。ブラウザを起動し直して`/pushq`を開くと、前の起動で登録したワーカーがページを制御し、購読が残り、ブラウザが前の`uaid`を名乗り、サービスが持っていたメッセージがワーカーに届く。最後に購読をやめ、登録を消す |
| `test_blink_api4` | `/api4`のページで、`navigator.plugins`と`mimeTypes`があり、最上位の文書がストレージを持ち、メディア要素の出力先が既定(`""`)で`setSinkId('')`が通り、DeviceOrientationのイベントがあり、`PressureObserver`にプロセッサの状態が届き、`MediaStreamTrackProcessor`がcanvasのストリームの16×8のフレームを読む。ボタンを押して`EyeDropper`を開き、赤い箱を押すとその色(`#c80000`)が返る。`encodingInfo`(`webrtc`の型、VP8)が対応と答える。オリジンごとのファイルシステムで、ファイルを書いて読み戻し、一覧、`resolve`、項目の削除ができ、ワーカーの同期アクセスのハンドルが書き、読み、大きさを答え、切り詰める。ワーカーの`MediaSource`の`handle`をページの動画の`srcObject`にすると、VP9の動画が64×48で再生される。ブラウザを起動し直して`/api4b`を開くと、2つのファイルが残っていて、消せる |
| `test_blink_dir` | ファイルの層のフォルダ(`a.txt`と、`b.txt`のある`sub`)をファイル変換で実身(仮身を並べた図形)にしてトレーに置く。ページのボタンで`showDirectoryPicker`を開き、パネル6にEnterで答えると、ページがフォルダとその中のフォルダを一覧し、`a.txt`を読み、`c.txt`を書く。ページが変えたフォルダは新しい実身になり、その仮身がトレーに入る。その前に、`webkitdirectory`の`<input type="file">`を押してパネル6にEnterで答えると、ページに`fsadir/a.txt`と`fsadir/sub/b.txt`が相対パス付きで渡り、中身を読める |
| `test_blink_mic` | サウンドデバイスの録音に、試験が1kHzの方形波を渡す(`knl_snd_loopback`)。`/mic`のページで、入力の機器に「マイク」があり、`getUserMedia`(パネルにEnterで答える)の音のトラックが48000Hzで、`MediaStreamTrackProcessor`で読んだ音が聞こえる大きさである |
| `test_blink_cam` | カメラデバイスのカーネル自身のカメラ(`knl_cam_loopback`、YUY2の64×48)に、試験が明るさ200のフレームを渡す。`/cam`のページで、`getUserMedia`(パネルにEnterで答える)の映像のトラックが64×48で、入力の機器に「Test camera」があり、`MediaStreamTrackProcessor`で読んだフレームが`I420`の64×48で明るさが200であり、`ImageCapture`の`takePhoto`がJPEGを返す |
| `test_blink_spell` | `/sp`のページの入力欄(`contenteditable`)を押して空白を打つと、入力欄の誤った綴りの下に赤い線が出て、入力欄の外の段落の同じ語には出ない(画面の赤い画素を数える) |
| `test_blink_webusb` | QEMUのUSBシリアル(FTDI。クラスドライバが取らない。入力を`/dev/zero`にする。入力が終わるとQEMUが機器を抜くため)をQMPで差し、`/usb`のページのボタンで`requestDevice`を開いてパネルにEnterで答える。ページがその機器を開き、構成とインタフェース、制御転送(速度の設定)、バルクのOUT(5バイト)とIN(状態の2バイト以上)、リセットとその後の制御転送、閉じる、`getDevices()`が1台を返すことをタイトルで確かめる |
| `test_blink_webusb_iso` | 2台目のQEMUのUSBオーディオを、オーディオのドライバが取らないようにして差す(`knl_uac_leave`、試験のときだけ)。ページが再生のインタフェースの2つ目の設定を選び、アイソクロナスのOUTに8パケットを送り、全部が送れたことを確かめる |
| `test_blink_webhid` | 2台目のQEMUのUSBマウスを、HIDのドライバが取らないようにして差す(`knl_hid_leave`)。ページが`navigator.hid.requestDevice`で選び、コレクション(Generic DesktopのMouse)と入力の報告の数、開く、`getDevices()`を確かめ、QMPでマウスを動かすと入力の報告が届く |
| `test_blink_zone` | システムのタイムゾーンを`Asia/Tokyo`にしてブラウザを起こし、動いている間に`America/New_York`に替える。ページのタイトルが、起動したときのタイムゾーン(`Intl`の名前と2026年7月1日の差、540分)と、替えた後のもの(-240分、夏時間)を示す |
| `test_blink_cam_mjpeg` | カーネル自身のカメラをMJPEGだけにし、明るさ200の灰色のJPEGを渡す。トラックのフレームが`I420`の64×48で、明るさが映像の範囲の値(16+219×200/255、188)になる |
| `test_blink_sync` | `/syncp`のページで、定期の同期の許可が与えられ、一度の同期がすぐワーカーの`sync`イベントになり(最後の機会ではない)、定期の同期(起動の行`periodicsync=`で間隔を3秒に)が2回以上起き、Content Indexに記事が入り、`navigator.vibrate`が真を返し、2つのファイルのバックグラウンドの取得がワーカーの`backgroundfetchsuccess`に届いて中身が読め、ページにも成功と伝わる。動画の`requestVideoFrameCallback`が3回呼ばれてフレームの情報が入り、canvasの`captureStream`を`srcObject`にした動画が32×24の緑で、長さが無限になる。キャッシュに応答を入れる。ブラウザを起動し直して`/syncq`を開くと、定期の同期、記事、キャッシュの応答が残り、定期の同期がまた起きる。それらを消すとなくなり、登録が消える |
| `test_blink_capture` | `/cap`のページが、機器の一覧、`getUserMedia`(マイクとカメラ)、音声認識の結果をタイトルに書き、canvasの`captureStream()`を`MediaRecorder`でVP8のWebMに録る。判定は、ブラウザと同じ方法で機械にマイク(48kHzで録音できる音の装置)があるかを調べて選ぶ。マイクがなければ、機器の一覧が空で、認識がパネルを出さずに`audio-capture`のエラーになること、あれば(Intel HDAなど)、一覧にマイクが1つあり、認識が`audio-capture`にならないこと(許可のパネルには答えない)を確かめる。カメラはどちらでもないので、`getUserMedia`は`NotFoundError`になる |
| `test_blink_iframe` | `/ifc`のiframe(赤)と`srcdoc`のiframe(青)がそれぞれの場所に描かれ、両方が`postMessage`し、ページが前者の`contentDocument`のタイトルを読む |
| `test_blink_api` | HMACの署名と検証、AES-GCMの暗号化と復号、gzipの圧縮と伸長、BroadcastChannel、Web Locks、Permissionsの問い合わせ(`prompt`)、`Notification.permission`(`default`)、保存の割当て、`sendBeacon`(サーバが受け取る)、`createImageBitmap`、`OffscreenCanvas`の2D |
| `test_blink_storage` | ページが`localStorage`と`sessionStorage`を変えると、同じオリジンのiframeに`storage`イベントがキー、古い値、新しい値、Storageの種類、ページのアドレスとともに届き、ページ自身には届かない |
| `test_blink_dialog` | `alert`、`confirm`2回、`prompt`のパネルにキー(Enter、Esc、`12`とEnter)で答え、`<select>`を押して出た一覧のパネルで2番目を押してEnterで選ぶ。タイトルがそれぞれの答えと`change`の値になる |
| `test_blink_clip` | `navigator.clipboard.writeText`で2行の文字列をトレーに入れ、`readText`で読み戻す。読み取りの許可のパネルにEnter(許可)で答え、その後の問い合わせが`granted`になる |
| `test_blink_cache` | `caches`のキャッシュに応答を入れて読み戻し、`ignoreSearch`の照合、キャッシュの名前、キー、項目の削除、キャッシュの削除と`has`を確かめる |
| `test_blink_cachekeep` | `caches`のキャッシュに512KBの応答を24個入れ、保存された後に小さい応答を1つ足す。2度目の保存は足した1つだけを書く(コンソールの`store caches saved`の行)。次に起動したブラウザが25個を読み戻し、小さい応答の本文と大きい応答のバイト列が元のとおりであることを確かめ、キャッシュを消す(14.8.1) |
| `test_blink_cachecut` | 1日キャッシュしてよい`/ccut.js`の最初の応答を半分で止め(接続は開いたまま)、その間にブラウザを終える。次に起動したブラウザが`/ccut.js`をもう一度サーバに求め、全体を実行する(タイトル`ccut whole`)。半分をキャッシュに残すと、それが応答の全体として返り、構文の誤りでタイトルが変わらない(14.8.1) |
| `test_blink_jit` | `/j`のページが、再帰と型付き配列のふるいのループと、WebAssemblyのループにかかった時間をタイトルに書く。ブラウザをそのままと、`v8=--jitless`の行を与えて2回開き、値が正しいこと、機械語にしたJavaScriptの時間がインタプリタの半分より短いこと、`--jitless`ではWebAssemblyがないことを確かめる |
| `test_blink_socket` | 試験のサーバの`/ws`との`WebSocket`でテキスト、Blob、バイナリのメッセージを往復させ(Blobの後に送ったものが追い越さない)、1000で閉じる。`/ev`の`EventSource`で、名前の付いたイベントと2つのメッセージを、ストリームが開いたままのうちに受け取り、閉じると接続が切れる |
| `test_blink_idb` | `indexedDB`でデータベースを作り、キー、件数、インデックス、逆向きのカーソルで読み戻す。Blob、File、大きな文字列を含む値を入れ、Blobをすぐ読み戻す。次に起動したブラウザが、バージョン、レコード、ストアが作ったキー、BlobとFileの中身(Fileの名前と時刻も)、大きな文字列の長さを読み戻す |
| `test_blink_audio` | `AudioContext`の発振器を鳴らし、コンテキストが48kHzで動き、時刻が進み、アナライザに波形が出ること、サウンドデバイスに渡したフレーム数(`sound=`の行で書かせる)が1.5秒分以上あることを確かめる。音の出力の装置がない機械では飛ばす |
| `test_blink_menu`、`test_blink_tool` | ウインドウのメニューの再読込、ツールパネルでの移動とボタン、ツールパネルがページのウインドウのすぐ前にあること |
| `test_blink_mem` | 大きなページを読んだ後のメモリの使用量 |
| `test_blink_frame` | `/fr`のページを、ビュー全体を描いてコピーする方法(`draw=copy`)、変わった所だけを面に描く方法、それに各表示をビュー全体と比べる方法(`check=`)で3回開き、箱の色の変化、キャレットの点滅、ホイールの3つの場面で、ブラウザの描画の時間(`stat`の行で、F12を押すと出す)とウインドウマネージャが組み立てた画素の数を出す(14.7.1)。比べた画素が1つも違わないこと、箱とキャレットの場面で組み立てた画素が前の4分の1より少ないことを確かめる |
| `test_desk` | デスクトップからページの実身を開く、同じ実身をもう一度開く |
| `test_blink_stuck` | 応答しないページ(試験のサーバが`/hang`に答えない)と、終わらないスクリプトのページ(`/busy`)を、読み込みの終わりを待たずに開く。それぞれで、右ボタンでメニューが出ること、Escの中止でページの表示まで進むこと(`ready=`のファイルができる)を確かめ、開き直して、応答しないページではCtrl+Eで、スクリプトのページではピクトグラムのダブルクリックで、8秒以内にプロセスが終わることを確かめる |
| `test_blink_home` | メニューを右ボタンで出し、〈編集〉の下の〈ホームページに設定〉をポインタで選ぶと、ページの実身の本文が表示しているページのアドレスになることを確かめる |
| `test_https` | `NET=user`のときだけ。外部のサイトを開いて画面のスクリーンショットを撮る。期限切れの証明書のサイトを拒否する |
| `test_http3` | `NET=user`のときだけ。HTTP/3を使えるサイト(`https://cloudflare-quic.com/`、`https://www.google.com/`)を開き、続けて同じサイトのページを2つ開く(`then=`の行)。応答の`Alt-Svc`の直後の要求は、できかけのQUICの接続と競って既にあるTCPの接続で送られることがあるためである。ブラウザが終わるときに書く、プロトコルごとの応答の数と最初の応答のプロトコル(`proto=`の行)で、HTTP/3(`h3`)で届いた応答があることを確かめる。QEMUの利用者モードのネットワークがUDPを外へ通すことも、これで確かめる。続けて、ブラウザを開き直し、キャッシュにない各サイトのページを1つ開くと、最初の応答からHTTP/3で届くこと(サーバについて知ったことの持ち越し。HTTPSレコードが助けないように、名前はシステムの名前解決で引く。`dns=system`の行)、知ったことを使わずに(`forget`の行)1つのページを開いても、HTTPSレコードによってHTTP/3で届く応答があることを確かめる |

`test_blink_image`、`test_blink_dialog`、`test_blink_clip`、`test_blink_file`、`test_blink_print`、`test_blink_drag`はデスクトップを起こし、デスクトップが前のウインドウに渡す入力でクリックとキーを与える。試験のサーバのポートは試験ごとに変え(`HTTP_PORT`に足す数。`test_blink_frame`は20から22を使うので、2026年10月7日に足した試験は31から使う)、前の試験の閉じた接続が残っていても開けるようにしている。

2026年10月7日、ワーカーとBlob、iframe、14.4.5のサービス、画像の文書、ページの問いのパネル、クリップボード、Cache Storageを入れて、`test_blink`、`test_blink_image`、`test_blink_wasm`、`test_blink_worker`、`test_blink_iframe`、`test_blink_api`、`test_blink_storage`、`test_blink_dialog`、`test_blink_clip`、`test_blink_cache`が通った。続けて共有ワーカーを入れて、`test_blink_shared`が通った。さらにファイルの選択と保存、WebCodecs、印刷、ドラッグ&ドロップ、メディアセッションを入れて、`test_blink_file`、`test_blink_codecs`、`test_blink_print`、`test_blink_drag`、`test_blink_session`が通った。

`NET=user`のとき、ホスト(QEMUを動かすWindows)で`tools/browser/ice_tcp_server.py 10.0.2.2`を動かし、`KT_SITE='http://10.0.2.2:8791/ice.html'`で開くと、ページはデータチャネルの提案を作り、TCPの受け身の候補(10.0.2.2の8793番)だけを持つICE-liteの相手として自分で応答する。ブラウザ(制御する側)はTCPのソケット(14.5.2)で相手に繋ぎ、RFC 4571で区切ったSTUNのバインディングで組を確かめる。サーバはそれにMESSAGE-INTEGRITYとFINGERPRINTを付けて答える。ページは結果をコンソールとタイトルに書く(`ice-tcp connected pair=tcp,active,...`)。サーバの`/go`は、同じディレクトリの`go.txt`に書いたアドレスへ転送する(無ければ`/ice.html`)。`KT_SITE`を`http://10.0.2.2:8791/go`にした1つのイメージで、`go.txt`を書き換えながら、外部のサイト(会議のサイトの入口など)を1回ずつ開ける。

`NET=user`のとき、ホストで`tools/browser/wt_echo_server.py <ホストのアドレス>`を動かし、`KT_SITE='http://<アドレス>:8790/wt.html\nsecure=http://<アドレス>:8790'`で開くと、ページがWebTransportでサーバに繋ぎ、双方向と単方向のストリームの往復、データグラムの往復、閉じることを確かめて、結果をコンソールに書く(`wt ready bidi=bidi-hello uni=uni-hello dgram=dgram closed`)。2026年10月8日、WSLで動かしたサーバとQEMUの間でこのとおりに動いた。

2026年10月8日、Service Worker、WebSocketStream、Payment Request、資格情報、File System Access、機器のないもの(14.4.8)、カメラとマイクと音声、MediaRecorder、Web Serial、WebHIDとWebUSBとゲームパッド、`cookieStore`、画面の向き、`navigator.connection`などを入れて、`test_blink_sw`、`test_blink_api2`、`test_blink_fsa`、`test_blink_devices`、`test_blink_capture`、`test_blink_serial`、`test_blink_cookie`が通った。続けてWebRTC(14.5.2)を入れて`test_blink_rtc`が、XR、画面、マシンのフォント、CSS Paint APIなどを入れて`test_blink_api3`が通った。ストレージのバケット、古いFile System API、落としたファイルのハンドルを足して、`test_blink_api3`と`test_blink_drag`が通った。WebGPU(14.6.5)を入れて、`make GL=1`で`test_blink_webgpu`が通った(softpipe)。ブラウザが複数のウインドウを持てるようにし(14.7.8)、`test_blink_windows`が通った。デスクトップに通知を加え(11.28、14.4.10)、`test_blink_notify`が通った。Web Push(14.4.11)を入れ、Service Workerの登録をブラウザを終えても残すようにして、`test_blink_push`が通った。バックグラウンドの同期と定期の同期、バックグラウンドの取得、Content Index、振動を入れて、`test_blink_sync`が通った(`requestVideoFrameCallback`と、`srcObject`に与えた`MediaStream`も同じ試験で確かめる)。Cache Storageと許可の答えを残し、WebSocketでBlobを送り、IndexedDBにBlobとFileを入れられるようにして、`test_blink_socket`と`test_blink_idb`が通った。マイクを`getUserMedia`につないで、`test_blink_mic`が通った。`navigator.plugins`、Storage Access、`setSinkId`、DeviceOrientation、Compute Pressure、`MediaStreamTrackProcessor`、`EyeDropper`のモジュール、オリジンごとのファイルシステム、ワーカーの`MediaSource`、モジュールの型の直列化(`postMessage`やIndexedDBで`CryptoKey`、`VideoFrame`、`MediaSourceHandle`などを送る)を入れて、`test_blink_api4`が通った。メディア要素のコントロールをBlinkの標準のものにし、ピクチャインピクチャのウインドウを浮かぶウインドウにして、`test_blink_media`と`test_blink_windows`が通った。カメラ(6.6)を`getUserMedia`と`ImageCapture`につないで`test_blink_cam`が、WebAssemblyのトラップハンドラを有効にして(5.5.2)`test_blink_wasm`が、印刷で文字を文字のまま残し`webkitdirectory`に答えて`test_blink_print`と`test_blink_dir`が、綴りの検査を入れて`test_blink_spell`が、プログラムのUSBデバイス(6.3.1)をWebUSBにつないで`test_blink_webusb`が通った。続けて、WebHID、WebUSBのリセットとアイソクロナスの転送、カメラのMJPEGを入れて、`test_blink_webhid`、`test_blink_webusb_iso`、`test_blink_cam_mjpeg`が通った。

`make ... KT_V8=test_blink_media,test_blink_tool`で、`v8`のうち指定した試験だけを実行する。`make ... GL=1 KT_BR_GPU=1`では、試験が起動するすべてのブラウザに`gpu=on`の行を足し、ほかの試験もGPUの経路(QEMUではsoftpipe)で実行する。`make ... NET=user KT_SITE=<URL>`では、そのサイトだけを開き、読み終えて1秒後と3秒後の画面のスクリーンショットを`/boot/SITE.PPM`、`/boot/SITE2.PPM`に保存する。読み終えてからもスクリプトがページを組み立てるサイトでは、`KT_SITE_WAIT=<ミリ秒>`を加えると、2枚目のスクリーンショットを1枚目からその時間の後に撮り、ブラウザをその7秒後まで動かしておく。`KT_SITE`のアドレスの後には、`\n`で区切ってブラウザの起動の引数の行を足せる。`script=<アドレス>`は、ページが表示されたところでそのアドレスのスクリプトを取得してページの中で実行し(シーク、一時停止、画質の切り替えなどの操作を試験から行う)、`secure=<オリジン>`は、そのオリジンのページを安全なコンテキストとして扱う(EMEは安全なコンテキストにだけあるので、試験のサーバの`http`のページでEMEを試すときに使う)。`gles=<版>`(`gles=3.0`など)は、Mesaのコンテキストが出すOpenGL ESの版をその版までにする(`MESA_GLES_VERSION_OVERRIDE`)。QEMUのsoftpipeを実機のV3Dと同じOpenGL ES 3.0にして、ANGLEとWebGPUのアダプタの扱いを実機と同じ道で試す。`KT_SITE_CLICK=<x>,<y>[,<x>,<y>…]`を加えると、2枚目のスクリーンショットの後、デスクトップを起動して、画面のその座標へポインタを動かして左のボタンを押して放すことを1点ずつ行い、4秒後の画面を`/boot/SITEC1.PPM`、`SITEC2.PPM`…に保存する(ページのボタンがクリックで動くかを、入力の経路を通して見る)。ブラウザが動いている間はスクリーンショットを1枚書くのに十数秒かかるので(QEMUでGoogleマップを開いたとき)、クリックがあるときは、ブラウザを終える時間(`quit=`)を、クリック1つにつき25秒と、さらに30秒延ばす。コンソールの`click N at x,y (T ms after the start)`の行に、押した時刻が出る。`KT_SITE_WAIT`の間は、ブラウザのプロセスが割り付けているメモリの量を10秒ごとにコンソールに出す。

2026年10月6日、GPUでのラスタ化(14.7.2)を入れて、`GL=1`のブラウザで`v8`、`gl`、`gpu`の試験グループを実行し、`NET=user`のときだけの2つを除く38の試験が通った(QEMUにはGPUがないので、`test_blink_gpu`のほかはプロセッサで描く)。`KT_BR_GPU=1`で、`test_blink`、`test_blink_input`、`test_blink_keep`、`test_blink_media`、`test_blink_menu`、`test_blink_tool`、`test_blink_frame`をsoftpipeの上のGPUの経路で実行し、すべて通った。`test_blink_frame`の`check=`では、変わった所だけをGPUで描いた47の表示が、ビュー全体をGPUで描き直したものと1画素も違わなかった。

2026年10月5日、フレームの処理(14.4.2)と移植していない機能の扱い(14.4.3)を入れて、`NET=user KT_SITE_WAIT=60000`でYouTubeを開いた。Raspberry Pi 5の実機では、それまで、ページは読み込まれるが組み立ての前の骨組み(灰色の動画とアイコンの枠)のままで、サムネイルの画像を一度も求めていなかった。QEMUの4コアでは、部品がそろうまで約30秒かかり、次のように表示された。

| ページ | 表示 |
| --- | --- |
| `https://www.youtube.com/` | 骨組みが外れ、ロゴ、検索の欄、ガイドのアイコン(SVG)が出る。ログインしていない利用者にはサーバがおすすめを返さないので、動画の一覧の代わりに「まずは検索してみましょう」と出る |
| `https://www.youtube.com/results?search_query=TRON` | 検索結果のサムネイル(`i.ytimg.com`のWebP)、チップ、アイコンが出る。画面の外のサムネイルは、スクロールして見えるまで求めない |

同じ日、Chromiumのメディアのパイプライン、MSE、EME、MediaCapabilities(14.6.2、14.6.3)を入れて、QEMUの4コアで次のように確かめた。試験のページとメディアのファイルは、ホストのHTTPのサーバ(QEMUの利用者モードのネットワークから`10.0.2.2`で届く)に置いた。音は、ブラウザが終わるときに出す、サウンドデバイスが受け取ったフレームの数で確かめた。

| ページ | 確かめたこと |
| --- | --- |
| MSEの試験のページ(H.264の640×360と426×240、AACの44.1kHzの断片に分けたMP4。映像と音を別の`SourceBuffer`で渡す) | `isTypeSupported`がH.264、AAC、VP9、Opusに真、AV1と1920×1080に偽を返す。`ChunkDemuxer`が開いて`sourceopen`が来て、`FFmpegVideoDecoder`と`FFmpegAudioDecoder`で再生する。4秒で16.5秒にシークすると、持っている範囲が`[6.08-10.08,16.16-22.08]`(先頭を`remove`し、隙間がある)になり、`seeked`のあと再生を続ける。22秒からの部分を426×240の初期化セグメントと断片で足すと、22.11秒で大きさが変わる。`endOfStream`のあと30.08秒で`ended`になる。418フレームを復号して60を落とし、音は856,934フレーム(17.9秒分)を渡した |
| Clear Keyの試験のページ(上のMP4をCENC(AES-CTR)で暗号化したもの) | `requestMediaKeySystemAccess`がClear Keyに`MediaKeySystemAccess`を返し、Widevineを`NotSupportedError`で拒否する。`MediaKeys`を要素に付け、`keyids`の要求に鍵(JWK)で答えると鍵が`usable`になり、復号してから渡すデコーダ(`(decrypting)`)で30秒の終わりまで再生する。音は1,442,238フレーム(30.0秒分)を渡した。MediaCapabilitiesの`decodingInfo`は、H.264の640×360とVP9の854×480に`smooth`、H.264の1920×1080に`smooth`でない、AV1に`supported`でないと答える |
| `<video src>`の試験のページ(H.264とAACのMP4、VP9とOpusのWebM) | `FFmpegDemuxer`で読み、MP4は`FFmpegVideoDecoder`、WebMは`VpxVideoDecoder`と`OpusAudioDecoder`で同時に再生する。シーク、一時停止、再開、終わり |
| `https://www.youtube.com/watch?v=dQw4w9WgXcQ`(3分33秒) | YouTubeのプレーヤはVP9の854×480とOpusを選ぶ(`VpxVideoDecoder`、`OpusAudioDecoder`)。`script=`の行で、本編が始まって15秒後に60秒へシーク(プレーヤが画質を640×360に下げる)、6秒の一時停止と再開、画質を`small`にする(426×240の`SourceBuffer`に切り替わる)と、それぞれ再生が続いた。音は4,582,848フレーム(95.5秒分)を渡した。同じ試験をもう一度行っても同じに動いた |
| `https://www.youtube.com/watch?v=jNQXAC9IVRw`(19秒) | 本編(320×240)が終わり、自動で次の動画に移ると、広告が2つ(854×480)流れ、そのあと本編(854×480)が45秒まで再生され、640×360に画質が下がった。広告と本編は、それぞれ新しい`MediaSource`で渡された |

2026年10月8日に`https://www.youtube.com/watch?v=09PB3aqdqHY`を開くと、本編が46秒ほど流れたところで、プレーヤが「エラーが発生しました」を出して止まった。動画のサーバへの要求(SABR)が送る再生の検証のトークン(PO token)が受け入れられず、サーバが保護の状態3(検証が要る)を返して動画のデータを送らなくなるためである。実行時の機能を`base::Feature`に合わせた(14.4.12)後は、検証のスクリプトが最後まで動き、トークンは10バイトの仮のものから77バイトのものになったが、まだ受け入れられない。検証のスクリプトの中身は調べていない。止まる前に、要素がプレーヤを手放した(`Shutdown`)後もプレーヤがパイプラインの通知を要素に渡し、要素が手放したプレーヤを尋ねてブラウザが落ちることがあった。`Shutdown`で要素への通知を止めるようにした(`port/media/media_player.cc`)。

QEMUのTCGでは、854×480のVP9でも1秒の再生にほぼ1秒で進む(MSEの試験のページで14%のフレームを落とした)。YouTubeを開いた1回の実行で、一時停止から再開した直後にブラウザのプロセスがシステムコールに自分のものでないアドレス(`4239d3e0`)を渡して終わった。同じ操作を繰り返した次の2回の実行(うち1回は、音の出力のスレッドを作る所と止める所を排他にした後)では起きず、原因はまだ確かめられていない。

2026年10月28日、x64のQEMU(WHPX、4コア)で`https://www.youtube.com/watch?v=aqz-KE-bpKQ`(10分34秒)を再生すると、163秒で止まった(`waiting`。持っている範囲は160.4秒まで)。再生ボタンもシークも効かなかった。プレーヤは159.6秒まで取得すると、次の取得まで90秒ほど要求を出さない。その間に、YouTubeのService Worker(14.4.6)への`fetch`イベントも34秒途切れた。ワーカーはアイドルの遅延を過ぎて止めることを求めたが、ホストが答えていなかったため、その後のページの取得(動画の`videoplayback`、`stats/qoe`、`log_event`など)はすべてワーカーのキューで待った(コンソールの`service worker: NNs without an answer`。待ちは21まで増えた)。プレーヤは5〜10秒ごとに要求を中止して出し直したが、どれも答えがなかった。最初のイベントが5分で時間切れになると(`the fetch event ended 3`)、置かれていたイベントはまとめてネットワークへ回った。ただし、そのときプレーヤの要求はもう中止されていて、ワーカーは遅延が0になってすぐまた止めることを求めたので、再生は戻らなかった。ページを読み直すと直るのは、メインフレームの移動の`fetch`イベントは待たずに始まり、キューに置かれたイベントも一緒に始めるためである。ホストが止めないと答えるようにした後は、要求の間が90秒ほど空いても最後(634.6秒)まで再生した。65秒でネットワークを切断し(QEMUの`set_link`)、139.9秒で持っている範囲を使い切って`waiting`になった後でつなぎ直すと、3.5秒後に再生が続いた。

2026年10月1日、JITを入れて`v8`の試験グループを実行し、`test_https`と`test_http3`(`NET=user`のときだけ)を除く19の試験が通った。QEMUの4コアで測った機械語とインタプリタの時間は次のとおりである(3回の実行の範囲)。

| 測ったもの | 機械語 | `--jitless` |
| --- | --- | --- |
| `test_jit`のJavaScriptのループ(`v8prog`の中で測る) | 1033〜1319ミリ秒 | 6231〜7311ミリ秒 |
| `test_jit`のプロセスの起動から終了まで | 1996〜2716ミリ秒 | 7231〜8606ミリ秒 |
| `test_jit`のWebAssemblyのループ(10万回の加算を50回) | 52〜67ミリ秒 | なし |
| `test_blink_jit`のJavaScriptのループ(ブラウザの中) | 377ミリ秒 | 1578ミリ秒 |
| `test_blink_jit`のWebAssemblyのループ | 54ミリ秒 | なし |

`v8prog`の`bench`では、再帰とふるいの関数がMaglevかTurbofanの機械語になり(`opt`)、文字列の関数はSparkplugの機械語になった(`baseline`)。`test_blink_mem`で大きなページを読んだ後のブラウザのプロセスのマッピングは約207MBで、V8のヒープは約10MBであった。同じ日に`NET=user`で`test_https`を実行すると、`https://www.gnu.org/`を除く各サイトを読んだ(部品がそろうまで、`https://www.wikipedia.org/`が3.2秒、`https://ja.wikipedia.org/wiki/BTRON`が3.1秒、`https://news.ycombinator.com/`が0.95秒、`https://html.duckduckgo.com/html/?q=TRON`が0.94秒)。`https://www.gnu.org/`は、QEMUの利用者モードのネットワークからの接続が拒否か時間切れになり、TLSの接続ができなかった(ホストからは接続できる)。JITとは関わらない。

2026年9月30日、QEMUの4コアで実際のサイトを開いた結果は次のとおりである。

| サイト | 部品がそろうまで | 同時に取得した最大数 |
| --- | --- | --- |
| `https://www.tron.org/ja/` | 18.6秒 | 68 |
| `https://www.wikipedia.org/` | 3.7秒 | 3 |
| `https://ja.wikipedia.org/wiki/BTRON` | 3.9秒 | 9 |
| `https://news.ycombinator.com/` | 1.4秒 | - |
| `https://html.duckduckgo.com/html/?q=TRON` | 1.9秒 | 13 |

2026年10月1日、`test_http3`でHTTP/3を確かめた結果は次のとおりである。QEMUの利用者モードのネットワークは、QUICのUDPをホストのソケットで外へ通した。QUICのソケットの受信のバッファは、NetBSDのスタックが受け付けた128KBになった。

| サイト | HTTP/3で届いた応答 | HTTP/2で届いた応答 |
| --- | --- | --- |
| `https://cloudflare-quic.com/`と、その2つのページ | 1(3つめのページ) | 10 |
| `https://www.google.com/`と、その2つのページ | 13 | 8 |

同じ日、サーバについて知ったことの持ち越し、DNSのHTTPSレコード、ネットワークのスレッドの待ち方を入れて`test_http3`を実行した結果は次のとおりである。開き直したブラウザは、前の回が書いたサーバの性質(10のサーバ)とQUICのセッションチケット(4つ)を読み戻し、両サイトとも最初の応答がHTTP/3で届いた。HTTPSレコードだけでも(`forget`)、両サイトとも最初の応答がHTTP/3で届いた。初めて開いた`https://cloudflare-quic.com/`の最初の応答がHTTP/2なのは、QUICがこのマシンでまだ動いたことがなく、TCPの接続を遅らせずに競わせて、TCPが先に繋がったためである。部品がそろうまでの時間は、ネットワークのスレッドの待ち方を変える前の同じ試験と比べたものである。

| 開き方 | 最初の応答 | HTTP/3で届いた応答 | HTTP/2で届いた応答 | 部品がそろうまで(前、後) |
| --- | --- | --- | --- | --- |
| `https://cloudflare-quic.com/`と、その2つのページ(初めて) | HTTP/2 | 3 | 8 | 1.83秒、1.60秒(最初のページ) |
| `https://www.google.com/`と、その2つのページ(初めて) | HTTP/3 | 19 | 2 | 5.48秒、4.29秒(最初のページ) |
| `https://cloudflare-quic.com/?again`(開き直し) | HTTP/3 | 1 | 0 | 1.60秒、0.87秒 |
| `https://www.google.com/?again`(開き直し) | HTTP/3 | 6 | 0 | 5.54秒、4.51秒 |
| `https://cloudflare-quic.com/?dns`(HTTPSレコードだけ) | HTTP/3 | 1 | 0 | 1.83秒、0.95秒 |
| `https://www.google.com/?dns`(HTTPSレコードだけ) | HTTP/3 | 4 | 1 | 5.67秒、5.52秒 |

2026年10月8日、セッション履歴とブラウザの操作(14.7)、ページから見える姿(14.4.12)を入れて、`test_blink_spa`(40)、`test_blink_ui`(31)、`test_blink_face`(22)が通り、`test_blink`、`test_blink_image`、`test_blink_media`、`test_blink_iframe`、`test_blink_dialog`、`test_blink_windows`、`test_blink_frame`も通った。YouTubeの再生では、ページから見える姿をChromeにそろえても、POトークンの検証(`GenerateIT`)の答えの先頭のトークンはnullのままで、保護の状態は約50秒で3(検証が要る)になる。
2026年10月9日、AV1(dav1dの復号、libaomの符号化)、PDF(PDFium)、ページの中の検索、パスキーとパスワードの保存、`appendEncodedChunks`(試験中の機能として)を入れ、ブラウザの試験群(`make GL=1`のときだけの`test_blink_gpu`を除く66本)を組に分けて回し、すべて通った。`test_blink_cookie`は前に回る試験がCookieを残すと数が変わり、順によって落ちる。組の一つで、試験の切り替わりにカーネルのネットワークのスタック(NetBSDのrump)のタスクが例外で止まった(ブラウザを起こす前で、ブラウザとは関係しない)。

2026年10月10日、明朝体と等幅の書体、ワーカーの`OffscreenCanvas`のWebGL、長いアニメーションフレーム、iframeの文書のService Worker、Web Platform Testsで見つけた違い(14.4.17)を直し、新しい`test_blink_swframe`と`test_blink_owl`を足して、`test_blink`、`test_blink_worker`、`test_blink_iframe`、`test_blink_api`、`test_blink_storage`、`test_blink_cache`、`test_blink_shared`、`test_blink_file`、`test_blink_sw`、`test_blink_swframe`、`test_blink_owl`、`test_blink_face`、`test_blink_idb`、`test_blink_frame`が通った(`make GL=1`)。

## 14.11 ライセンス

`tessron-chromium`のファイルはMITライセンスで配布する。`BROWSER=1`でビルドしたイメージには、Chromium(Blink、V8、Skia、`base`、`net`など)とその第三者コンポーネント、BoringSSL、FFmpeg、dav1d、Rustのクレートが含まれる。`GL=1`でビルドしたときは、ANGLE(BSD 3条項)とMesa(大半はMIT)も含まれる。一覧と条件は`THIRD_PARTY_NOTICES.md`にある。

## 14.12 拡張機能

Chrome拡張(Manifest V3)を扱う。Chromiumの`extensions/`はブラウザプロセスとレンダラの分離を前提にしていて、このブラウザにはその層がないので、使わずに移植層に小さく作った(`port/ext/`)。Chrome Web Storeからの導入はしない。

### 14.12.1 置き場所と取り込み

拡張機能は、ZIPまたはCRXをファイル変換で取り込んだ実身である(レコード1がファイルのバイト列。`xf_import`)。新しい形式も、新しい箱も作らない。普通の実身「ブラウザの拡張機能」(`SYSDEF_BROWSER_EXT`)のレコード0に、読み込んだ拡張機能の実身への仮身が並ぶ。仮身は参照数に数えるので、もとの仮身を捨てても実身は残る。レコード1はブラウザが持つ状態のJSONで、拡張機能ごとの有効か無効か、`chrome.storage`の`local`と`sync`、有効なルールセット、動的なルール、無効にした静的ルール、登録したコンテンツスクリプト、アラーム、後から許された権限である。`storage.session`とセッションルールは持ち越さない。

メニューの〈拡張機能を読み込む〉でファイルの選択のパネル(パネル6。トレーの仮身やパネルに入れた仮身)から選ぶと、中身を読んで`manifest_version`が3であることを確かめ、権限のパネル(パネル21)に、名前、バージョン、求めていること(14.12.3)の一覧を出す。〈読み込む〉で一覧に仮身を足し、〈取消〉なら何も残さない。次に開くページから働き、バックグラウンドのワーカーは次の起動から動く。〈設定〉の「拡張機能」のタブ(14.7.10)に、名前、バージョン、状態が並び、読み込み、有効と無効の切替え、削除ができる。状態の変更は次に開くページと次の起動から効き、別のプロセスのブラウザには次の起動から効く。IDは実身のUUIDのSHA-256から作るChromeと同じ字種の32文字で、実身が同じなら変わらない。CRXの署名は確かめない。利用者が自分で選んだ実身だけを読み込む。

### 14.12.2 動くもの

| 機能 | 実装 |
| --- | --- |
| ファイル | `chrome-extension://<ID>/<パス>`をローダとネットワークの層が答える(`br_ext_serve`)。ページからは`web_accessible_resources`にあるものだけで、その`matches`がページのオリジンに合うとき。マニフェストのパスの先頭の`/`は無視する |
| 拡張機能のページ | `action`のポップアップ、`options_page`、`tabs.create`で開く拡張機能のページは、このプロセスの普通のウインドウで開く(420×560)。別のプロセスにすると`storage`とワーカーが別になるからである。メインワールドに`chrome`があり、既定のCSPは`script-src 'self' 'wasm-unsafe-eval'; object-src 'self'`(マニフェストの`content_security_policy.extension_pages`があればそれ) |
| `content_scripts` | 拡張機能ごとの隔離されたワールド(番号は1000から)で実行する。ページのスクリプトからは見えない。`run_at`の3つは`RunScriptsAtDocumentElementAvailable`、`RunScriptsAtDocumentReady`、`RunScriptsAtDocumentIdle`。CSSは`InsertStyleSheet`。`matches`、`exclude_matches`、`all_frames`、`world` |
| `background.service_worker` | 拡張機能のオリジンの本物のService Worker(14.4.6)。ブラウザの起動時に登録する。`type`が`module`ならモジュールのワーカーで、スクリプトの先頭に`import '/__brext_shim.js'`を付けて答える(importは順に評価されるので、`chrome`が他のモジュールより先にできる)。クラシックならスクリプトの先頭に`chrome`オブジェクトを付ける。`onInstalled`は最初の1回、`onStartup`はそれ以後 |
| `chrome.runtime` | `id`、`getURL`、`getManifest`、`sendMessage`、`onMessage`(`sender.tab`、`sender.frameId`付き)、`onInstalled`、`onStartup`、`openOptionsPage`、`getPlatformInfo`。`connect`はない |
| `chrome.storage` | `local`、`sync`(実身のレコード1)、`session`(メモリのみ)。`get`、`getKeys`、`set`、`remove`、`clear`、`onChanged`(同じ拡張機能のすべてのコンテキストに届く)。`managed`はいつも空 |
| `chrome.tabs` | このブラウザにタブはなく、ウインドウが1つのタブ(ID 1)に当たる。`query`、`get`は今のページを答え、`url`と`title`は`tabs`か、そのサイトの権限があるときだけ付く。`create`は拡張機能のページならこのプロセスのウインドウ、ほかは別のプロセスのブラウザを開く。`update`は`url`へ移動、`reload`、`sendMessage`(そのページのすべてのフレームのコンテンツスクリプトに届き、最初の答えが答えになる)、`onUpdated`。`remove`は断る |
| `chrome.windows` | `get`、`getCurrent`、`getAll`は1つのウインドウを答え、`create`は`tabs.create`と同じ |
| `action` | ボタンはメニューの〈拡張機能を読み込む〉の下に標準の項目として並ぶ。自前で描くツールバーは作らない。項目は`setTitle`の文字で、`setBadgeText`の文字を括弧で添える。アイコンとバッジの色は見せない(`setIcon`は受け取って何もしない)。選ぶと`activeTab`の許可を与え、`default_popup`があればそのページをウインドウで開き、なければ`onClicked`。`enable`、`disable`、`setPopup`、`openPopup` |
| `scripting` | `executeScript`(`func`と`args`、`files`、`target`の`allFrames`と`frameIds`、`world`の`MAIN`)、`insertCSS`、`removeCSS`(`origin`)、`registerContentScripts`、`updateContentScripts`、`unregisterContentScripts`、`getRegisteredContentScripts`。結果がプロミスならそれを待つ。登録は状態に残す。ページのスレッドで実行するので、ワーカーはタスクを投げて答えを待つ |
| `webRequest` | `onBeforeRequest`、`onBeforeSendHeaders`、`onSendHeaders`、`onHeadersReceived`、`onResponseStarted`、`onBeforeRedirect`、`onCompleted`、`onErrorOccurred`。見るだけで、答えは要求に影響しない(`blocking`を付けると`addListener`が例外)。MV3の仕様どおりである。ヘッダは`extraInfoSpec`で求めたときだけ渡す。`host_permissions`に合う要求だけが届く |
| `alarms` | `create`(`when`、`delayInMinutes`、`periodInMinutes`)、`get`、`getAll`、`clear`、`clearAll`、`onAlarm`。ブラウザのプロセスが持ち、状態に残す。ワーカーが取りに来るときに時刻を見るので、ワーカーが動いている間に100ミリ秒以内に届く。Chromeの30秒の下限は置かない(利用者が選んだ未パックの拡張機能と同じ扱い) |
| `contextMenus` | `create`、`update`、`remove`、`removeAll`、`onClicked`。ウインドウのメニューに標準の項目として並ぶ。子の項目は「親 > 子」の1段で、`checkbox`は「[x] 」を添える。`contexts`、`documentUrlPatterns`、`targetUrlPatterns`。`%s`は選んだ文字に置き換える |
| `i18n` | `getMessage`(`$1`、名前の付いたプレースホルダ、`@@ui_locale`など)、`getUILanguage`、`getAcceptLanguages`。`_locales/<default_locale>`の上に`ja`、`ja_JP`を重ねる。マニフェストの`name`、`short_name`、`description`、`action.default_title`の`__MSG_名前__`も置き換える。`detectLanguage`はいつも空 |
| `permissions` | `getAll`、`contains`、`request`、`remove`。`request`は`optional_permissions`、`optional_host_permissions`のものだけで、権限のパネルで尋ねる |
| `declarativeNetRequest` | 静的なルールセット(有効になったときに読む)、`updateDynamicRules`、`updateSessionRules`、`updateEnabledRulesets`、`updateStaticRules`、`isRegexSupported`。`block`、`allow`、`allowAllRequests`、`redirect`(`url`、`extensionPath`、`regexSubstitution`、`transform`)、`upgradeScheme`、`modifyHeaders`。`urlFilter`、`regexFilter`(RE2)、ドメイン、種類、メソッド、`domainType` |

`chrome.*`はJavaScript(`br_ext_shim.inc`)で書き、下は1つの呼出し(`api_call`)である。コンテンツスクリプトと拡張機能のページでは、そのコンテキストだけにある関数を呼ぶ。ワーカーでは自分のオリジンの`/__brext/<操作>`を`fetch`する。ローダはこの道を、要求の起点がその拡張機能のオリジンであるときだけ通す。メッセージと事象はブラウザの中のログに入り、相手が100〜150ミリ秒ごとに、前に読んだ番号より後を取りに来る。プロセスごとに拡張機能が読まれるので、メッセージと`storage`のやりとりは同じブラウザのプロセスの中である。別のプロセスのブラウザが同時に書くと、ページの保存域と同じく後から書いたほうが残る。

ルールはネットワークの層で見る。`URLRequestContext`に`net::NetworkDelegate`を1つ置き(`BrExtDelegate`、`net_fetch_tessronos.cc`)、ページのリソース、ウインドウの文書(`main_frame`)、フレームの文書(`sub_frame`)、ワーカーの要求、プリフライトのすべてがそこを通る。

| 段階 | すること |
| --- | --- |
| `OnBeforeURLRequest` | `block`は`ERR_BLOCKED_BY_CLIENT`。`redirect`と`upgradeScheme`は`new_url`を返す。スタックが内部の307を作るので、頼んだ側にはサーバのリダイレクトと同じに見える(`redirected`が真、`url`が行き先)。行き先が`chrome-extension://`なら、スタックにそのスキームはないので、その場で拡張機能のファイルを答える |
| `OnBeforeStartTransaction` | `modifyHeaders`の`requestHeaders` |
| `OnHeadersReceived` | `modifyHeaders`の`responseHeaders`。応答のヘッダを差し替えるので、Cookieの保存とCORSの判定にも効く |

要求の種類と起点は、ローダと文書の取得が`X-TessronOS-Resource`の行で渡し、スタックがこれを外して`URLRequest`のユーザデータにする。行のない要求はブラウザ自身のものとして、ルールを当てない。拡張機能のルールは他の拡張機能の要求に当たらない。WebSocketのハンドシェイクはリダイレクトをたどれないので、`websocket_tessronos.cc`がチャネルを開く前にルールを見て、`block`なら失敗にし、`redirect`と`upgradeScheme`なら別のアドレスに繋ぐ。

順序はChromeと同じである。拡張機能ごとに優先度、次に`allow`、`allowAllRequests`、`block`、`upgradeScheme`、`redirect`の順で1つを決め、拡張機能の間では`block`が勝ち、次に後から入れたものの`redirect`か`upgradeScheme`である。ある拡張機能の`allow`は別の拡張機能の`block`を取り消さない。`modifyHeaders`は、その拡張機能で要求を許すルールより優先度が高いものをすべて、後から入れた拡張機能から優先度の順に当てる。`set`か`remove`した名前はそれより後のルールでは変えない。`allowAllRequests`は、表示中のページに合うルールの優先度までをそのページの要求について止める。

ルールは毎回全部を見ない。`||ホスト`で始まるフィルタと`requestDomains`のあるルールはホストで表にして、要求のホストとその親のドメインで引く。広告のリストの大部分はこの形である。残りだけを順に見る。

### 14.12.3 権限

宣言にないものは拒否する。止めるのは`api_call`の入口とルールの評価で、JavaScriptで`chrome.*`に名前を出さないのは見かけである。ワーカーが`/__brext/alarms.getAll`を直接`fetch`しても同じエラーになる。

| 確かめるもの | 必要なもの |
| --- | --- |
| `storage`、`alarms`、`scripting`、`webRequest`、`contextMenus`、`tabs`の`url`と`title` | 同じ名前の`permissions`。`tabs`の`url`と`title`は、そのサイトの`host_permissions`でもよい |
| `action` | マニフェストの`action` |
| ルールの全部 | `declarativeNetRequest`か`declarativeNetRequestWithHostAccess` |
| `redirect`、`modifyHeaders` | 要求先と起点の両方が`host_permissions`に合うこと |
| `declarativeNetRequestWithHostAccess`のすべてのルール | 同上 |
| `scripting.executeScript`、`insertCSS` | ページのアドレスが`host_permissions`に合うか、そのページで`action`かメニューの項目を選んで`activeTab`の許可があること。`activeTab`はタブが別のオリジンに移ると消える |
| `webRequest`で見える要求 | 要求先が`host_permissions`に合うこと |
| 拡張機能の`fetch`のCORS | 要求先が`host_permissions`に合うときだけ、サーバのCORSの答えを見ない |
| コンテンツスクリプトで使えるもの | `runtime`のメッセージと`getURL`など、`storage`、`i18n`だけ |

権限のパネルのリストには、`permissions`を日本語にしたもの、`host_permissions`、`content_scripts`の`matches`を出す。このブラウザにない権限(`offscreen`など)は「(このブラウザにはありません)」を添えて出し、読み込みは断らない。`<all_urls>`と`*://*/*`は「すべてのサイト」と書く。部品はデータボックスの標準のテキスト、リスト、ボタンだけである。

### 14.12.4 まだないもの

- `declarativeNetRequest`の条件の`responseHeaders`、`excludedResponseHeaders`はない。そのルールは受け取らず、数を起動時にコンソールに出す。`getMatchedRules`はいつも空である。
- `allowAllRequests`が見る文書は表示中のページだけで、フレームの文書の下の要求には効かない。
- `runtime.connect`、`offscreen`、`userScripts`、WebTransportの`resourceType`はない。呼ばれない名前は`undefined`である。`commands`は`onCommand`があるだけで、キーでは動かない。
- アイコンは使わない。`action`のボタンはメニューの項目で、ツールバーにもアイコンにも出ない。
- CRXの署名、拡張機能の更新は扱わない。
