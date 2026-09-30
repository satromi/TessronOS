#!/usr/bin/env bash
#
# qemu_run.sh — QEMU を時間制限付きで起動し、UART 出力を捕捉して検査する
#
#   tools/qemu_run.sh [-t 秒] [-e 期待文字列] [-f 禁止文字列] [-o 出力ファイル]
#                     [-q QMP 係の台本] [-s "サーバのコマンド行"] -- <qemu の引数...>
#
#   期待文字列が出力に含まれれば 0、含まれなければ 1 を返す。
#   -e を省略したときは時間切れまでの出力を表示して 0 を返す。
#   -q を与えると QEMU に QMP の口を開き、台本(tools/qemu_qmp.py)に
#   その口と出力ファイルを渡して裏で走らせる。試験が機械の外からの操作
#   (USB 機器の抜き差し、鍵の押下)を頼むときに使う。
#   -s を与えると、試験が機械の中から繋ぐホスト側のサーバ(tools/ftpd.py
#   など)をそのコマンド行で QEMU の前に起こし、終わるときに止める。
#   サーバは標準入力を管から読み、終わるときはその管を閉じて止める
#   (ftpd.py --exit-on-eof)。Windows の Python で起こしたサーバも止まる。
#
#   QEMU が Windows のもの(名前が .exe)のときは WSL から起こす: QMP は
#   Windows 側のループバックの TCP で開き、QMP 係は HOST_PY(Windows の
#   Python)で走らせ、出力と QEMU の番号のファイルは作業ディレクトリの下
#   に置いて相対のパスで渡す(どちらからも同じ所を指す)。時間切れのとき
#   は番号で taskkill する。NET=user の試験で、利用者モードの網を持たない
#   xPack の QEMU の代わりに使う(build_make/makefile)。
#
set -uo pipefail

TIMEOUT=5
EXPECT=""
FORBID=""
OUT=""
QMP_HELPER=""
SERVER=""
while [ $# -gt 0 ]; do
	case "$1" in
		-t) TIMEOUT="$2"; shift 2 ;;
		-e) EXPECT="$2"; shift 2 ;;
		-f) FORBID="$2"; shift 2 ;;
		-o) OUT="$2"; shift 2 ;;
		-q) QMP_HELPER="$2"; shift 2 ;;
		-s) SERVER="$2"; shift 2 ;;
		--) shift; break ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done
QEMU="${QEMU:-qemu-system-aarch64}"
HOST_PY="${HOST_PY:-python3}"
WIN=0
case "$QEMU" in *.exe) WIN=1 ;; esac

# 自分で作った一時ファイルとディレクトリ、起こしたサーバは、終わるときに必ず片付ける
OWN_OUT=""
QMP_DIR=""
WIN_DIR=""
SRV_DIR=""
SERVER_PID=""
cleanup() {
	if [ -n "$SERVER_PID" ]; then
		exec 7>&- 2>/dev/null
		for _ in 1 2 3 4 5 6 7 8 9 10; do
			kill -0 "$SERVER_PID" 2>/dev/null || break
			sleep 0.2
		done
		kill "$SERVER_PID" 2>/dev/null
		wait "$SERVER_PID" 2>/dev/null
	fi
	if [ -n "$WIN_DIR" ] && [ -s "$WIN_DIR/qemu.pid" ]; then
		taskkill.exe /F /PID "$(tr -dc 0-9 < "$WIN_DIR/qemu.pid")" >/dev/null 2>&1
	fi
	[ -n "$OWN_OUT" ] && rm -f "$OWN_OUT"
	[ -n "$QMP_DIR" ] && rm -rf "$QMP_DIR"
	[ -n "$WIN_DIR" ] && rm -rf "$WIN_DIR"
	[ -n "$SRV_DIR" ] && rm -rf "$SRV_DIR"
}
trap cleanup EXIT
if [ "$WIN" = 1 ]; then
	WIN_DIR="$(mktemp -d -p . .qemu_run.XXXXXX)"
fi
if [ -z "$OUT" ]; then
	if [ "$WIN" = 1 ]; then
		OUT="$WIN_DIR/out.log"
	else
		OUT="$(mktemp)"
		OWN_OUT="$OUT"
	fi
fi

# サーバは exec でシェルと入れ替わり、標準入力は管。口を開くまで少し待つ:
# 試験は起動してすぐ繋ぎに来ることがある
if [ -n "$SERVER" ]; then
	SRV_DIR="$(mktemp -d)"
	mkfifo "$SRV_DIR/stdin"
	bash -c "exec $SERVER" < "$SRV_DIR/stdin" &
	SERVER_PID=$!
	exec 7> "$SRV_DIR/stdin"
	sleep 2
	if ! kill -0 "$SERVER_PID" 2>/dev/null; then
		echo "qemu_run: the server did not start: $SERVER" >&2
		SERVER_PID=""
		exit 1
	fi
fi

QMP_ARGS=()
HELPER_PID=""
if [ "$WIN" = 1 ]; then
	QMP_ARGS=(-pidfile "$WIN_DIR/qemu.pid")
fi
if [ -n "$QMP_HELPER" ]; then
	: > "$OUT"
	if [ "$WIN" = 1 ]; then
		QMP_ADDR="tcp:127.0.0.1:$(( 20000 + RANDOM % 20000 ))"
		QMP_ARGS+=(-qmp "$QMP_ADDR,server=on,wait=off")
		"$HOST_PY" "$QMP_HELPER" "$QMP_ADDR" "$OUT" &
	else
		QMP_DIR="$(mktemp -d)"
		QMP_SOCK="$QMP_DIR/qmp.sock"
		QMP_ARGS+=(-qmp "unix:$QMP_SOCK,server=on,wait=off")
		python3 "$QMP_HELPER" "$QMP_SOCK" "$OUT" &
	fi
	HELPER_PID=$!
fi

# -serial stdio で UART をそのまま標準出力へ。モニタは使わない。
# tr は行ごとに書き出させる。溜めると QMP 係が試験の依頼を読めない
timeout --foreground -s KILL "$TIMEOUT" "$QEMU" -serial stdio "$@" "${QMP_ARGS[@]}" -monitor none </dev/null 2>&1 | stdbuf -oL tr -d '\r' | tee "$OUT"
status=${PIPESTATUS[0]}

if [ -n "$HELPER_PID" ]; then
	kill "$HELPER_PID" 2>/dev/null
fi

echo
if [ -n "$FORBID" ] && grep -q -- "$FORBID" "$OUT"; then
	echo "qemu_run: FAIL (\"$FORBID\" found, qemu exit $status)"
	exit 1
fi
if [ -n "$EXPECT" ]; then
	if grep -q -- "$EXPECT" "$OUT"; then
		echo "qemu_run: OK (found \"$EXPECT\", qemu exit $status)"
		exit 0
	fi
	echo "qemu_run: FAIL (\"$EXPECT\" not found, qemu exit $status)"
	exit 1
fi
echo "qemu_run: done (qemu exit $status)"
exit 0
