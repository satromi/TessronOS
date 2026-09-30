# source tools/env.sh — TessronOS のビルドに使うツールへ PATH を通す
PREFIX="${TS_XPACK_PREFIX:-$HOME/.local/xPacks}"
for d in "$PREFIX/aarch64-none-elf-gcc/bin" "$PREFIX/qemu-arm/bin"; do
	case ":$PATH:" in
		*":$d:"*) ;;
		*) [ -d "$d" ] && PATH="$d:$PATH" ;;
	esac
done
export PATH
export TS_TOOLCHAIN="$PREFIX/aarch64-none-elf-gcc/bin"
unset PREFIX
