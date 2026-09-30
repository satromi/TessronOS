#!/usr/bin/env bash
#
# TessronOS 開発ツールの導入(WSL2 Ubuntu / Linux x86_64)
#
# xPack の AArch64 ベアメタル GCC と QEMU を固定バージョンで取得し、
# $PREFIX(既定 ~/.local/xPacks)に展開する。root 権限は不要。
# 導入後は tools/env.sh を source して PATH を通す。
#
#   ./tools/setup_wsl.sh            # 未導入のものだけ入れる
#   ./tools/setup_wsl.sh --force    # 入れ直す
#
set -euo pipefail

GCC_VER="15.2.1-1.1"      # xpack-dev-tools/aarch64-none-elf-gcc-xpack
QEMU_VER="9.2.4-1"        # xpack-dev-tools/qemu-arm-xpack
PREFIX="${TS_XPACK_PREFIX:-$HOME/.local/xPacks}"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

arch="$(uname -m)"
case "$arch" in
	x86_64)  plat="linux-x64" ;;
	aarch64) plat="linux-arm64" ;;
	*) echo "unsupported host arch: $arch" >&2; exit 1 ;;
esac

fetch_xpack() {
	# $1 = リポジトリ名, $2 = パッケージ名, $3 = バージョン, $4 = リンク名
	local repo="$1" pkg="$2" ver="$3" link="$4"
	local dir="$PREFIX/${pkg}-${ver}"
	local tar="${pkg}-${ver}-${plat}.tar.gz"
	local url="https://github.com/xpack-dev-tools/${repo}/releases/download/v${ver}/${tar}"

	if [ -d "$dir/bin" ] && [ "$FORCE" = 0 ]; then
		echo "== $pkg $ver: already installed ($dir)"
	else
		echo "== $pkg $ver: downloading $url"
		mkdir -p "$PREFIX"
		local tmp
		tmp="$(mktemp -d)"
		curl -fSL --retry 3 -o "$tmp/$tar" "$url"
		rm -rf "$dir"
		tar -xzf "$tmp/$tar" -C "$PREFIX"
		rm -rf "$tmp"
		[ -d "$dir/bin" ] || { echo "extract failed: $dir/bin not found" >&2; exit 1; }
	fi
	ln -sfn "$dir" "$PREFIX/$link"
}

fetch_xpack aarch64-none-elf-gcc-xpack xpack-aarch64-none-elf-gcc "$GCC_VER" aarch64-none-elf-gcc
fetch_xpack qemu-arm-xpack             xpack-qemu-arm             "$QEMU_VER" qemu-arm

# apt が対話なしで使えるときだけ補助ツールを入れる(dtc, gdb-multiarch)
if command -v apt-get >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
	sudo apt-get install -y --no-install-recommends device-tree-compiler gdb-multiarch >/dev/null
	echo "== apt: device-tree-compiler gdb-multiarch installed"
else
	echo "== apt: skipped (no passwordless sudo). Install manually if needed:"
	echo "     sudo apt-get install device-tree-compiler gdb-multiarch"
fi

echo
echo "== done. Add to your shell:"
echo "     source $(cd "$(dirname "$0")" && pwd)/env.sh"
"$PREFIX/aarch64-none-elf-gcc/bin/aarch64-none-elf-gcc" --version | head -1
"$PREFIX/qemu-arm/bin/qemu-system-aarch64" --version | head -1
