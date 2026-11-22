#!/bin/bash
# The lock of the TessronOS tree's build (docs/tessronos/12-development.md 12.2.1).
#
#   tools/mklock.sh build CMD...   run a build (make ...): with the lock shared, so that
#                                  builds of other BUILD_IDs run at the same time.
#                                  When something that the build reads (headers, the
#                                  makefiles, the tools scripts) was changed while it ran,
#                                  CMD is run again (at most TS_MKLOCK_RERUN times, 5 by
#                                  default). Before that, the objects of this BUILD_ID
#                                  written since the run began that depend (by .d) on
#                                  a changed header are removed, since they may have
#                                  read the old contents; the others are made again by
#                                  make for being older than the header. Exit status is not 0 when
#                                  the changes still continue after the last run.
#   tools/mklock.sh edit CMD...    change what every build reads: run CMD at once (no
#                                  waiting for the builds) and record the change
#                                  (a line is appended to GEN) so that the builds
#                                  running now run again.
#
# "flock LOCK CMD" (the lock held alone) waits for the builds and is not recorded in
# GEN, but the modification times are checked by the build all the same.
set -eu
LOCK=${TS_MAKE_LOCK:-/tmp/tessronos-make.lock}
GEN=${TS_SRC_GEN:-/tmp/tessronos-src.gen}
MAXRERUN=${TS_MKLOCK_RERUN:-5}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-}
[ $# -ge 2 ] || { echo "usage: $0 build|edit CMD..." >&2; exit 2; }
shift

gen_now() { [ -f "$GEN" ] && wc -l <"$GEN" || echo 0; }
gen_bump() { date +%s.%N >>"$GEN"; }

case "$mode" in
edit)
	gen_bump
	rc=0
	"$@" || rc=$?
	gen_bump
	exit "$rc"
	;;
build) ;;
*)
	echo "usage: $0 build|edit CMD..." >&2
	exit 2
	;;
esac

exec 9>>"$LOCK"
flock -s 9
STAMP=$(mktemp /tmp/tessronos-mklock.XXXXXX)
trap 'rm -f "$STAMP"' EXIT

# Lists what every build reads that is newer than $STAMP.
newer_files() {
	find "$ROOT" \( -path "$ROOT/.git" -o -path "$ROOT/build_make/obj" -o -path "$ROOT/obj" \) -prune -o \
		-type f \( -name '*.h' -o -path "$ROOT/build_make/*" \( -name makefile -o -name '*.mk' \) \
		-o -path "$ROOT/tools/*" \( -name '*.sh' -o -name '*.py' \) \) \
		-newer "$STAMP" -print 2>/dev/null
}

# The object roots to look in: OBJ_ROOT= on the command line, or the default one, and
# build_make/obj; only the directories of this BUILD_ID are touched.
BID=""; OBJR=""
for a in "$@"; do
	case "$a" in
	BUILD_ID=*) BID=${a#BUILD_ID=} ;;
	OBJ_ROOT=*) OBJR=${a#OBJ_ROOT=} ;;
	esac
done
if [ -z "$OBJR" ]; then
	p=${ROOT#/mnt/?/}; p=${p//\//_}
	OBJR=$HOME/tessronos-obj-ts/$p
fi
case "$OBJR" in /*) ;; *) OBJR=$ROOT/build_make/$OBJR ;; esac

# Removes the objects written since $STAMP that depend on a changed header ($1: the list).
drop_stale_objs() {
	local pats f d o top n=0
	pats=$(mktemp /tmp/tessronos-mklock.XXXXXX)
	printf '%s
' "$1" | grep '\.h$' | sed "s|^$ROOT/||" >"$pats" || true
	if [ -s "$pats" ]; then
		for r in "$OBJR" "$ROOT/build_make/obj"; do
			[ -d "$r" ] || continue
			while IFS= read -r d; do
				top=${d#"$r"/}; top=${top%%/*}
				if [ -n "$BID" ]; then
					case "$top" in user_"$BID" | _*"$BID") ;; *) continue ;; esac
				else
					case "$top" in user | _*[A-Z0-9_]_ | _*_ktest) ;; *) continue ;; esac
				fi
				o=${d%.d}.o
				[ -f "$o" ] && [ "$o" -nt "$STAMP" ] || continue
				if grep -qFf "$pats" "$d"; then rm -f "$o"; n=$((n + 1)); fi
			done < <(find "$r" -name '*.d' 2>/dev/null)
		done
	fi
	rm -f "$pats"
	echo "mklock: removed $n objects that may have read the old headers" >&2
}

run=0
while :; do
	touch "$STAMP"
	g0=$(gen_now)
	rc=0
	"$@" || rc=$?
	g1=$(gen_now)
	files=$(newer_files)
	if [ "$g0" = "$g1" ] && [ -z "$files" ]; then
		exit "$rc"
	fi
	if [ "$run" -ge "$MAXRERUN" ]; then
		echo "mklock: the sources were still changed during the run $((run + 1)); the output may mix old and new headers" >&2
		[ "$rc" -ne 0 ] && exit "$rc"
		exit 1
	fi
	run=$((run + 1))
	drop_stale_objs "$files"
	first=$(printf '%s\n' "$files" | head -3 | sed "s|^$ROOT/||" | tr '\n' ' ')
	echo "mklock: changed during the build (gen $g0->$g1; ${first:-no file newer}); run $((run + 1)) of at most $((MAXRERUN + 1)): $*" >&2
done
