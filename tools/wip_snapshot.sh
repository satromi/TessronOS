#!/bin/bash
# The work in a git worktree kept as a commit, without touching it.
#
#   tools/wip_snapshot.sh WORKTREE [NAME]
#
# Everything in the worktree -- what is changed and what is new, as far
# as .gitignore lets it be tracked -- is written as a commit whose parent
# is the worktree's HEAD, through an index of its own, so the worktree's
# own index and files stay as they are and whoever works there is not
# disturbed. The commit is kept under refs/backup/NAME (default: the
# worktree's directory name), each snapshot on top of the one before, so
# that the work survives the worktree being stopped or taken away.
# Answers the commit made; nothing is made when nothing changed.
set -eu
WT=${1:?worktree}
NAME=${2:-$(basename "$WT")}
REF=refs/backup/$NAME
IDX=$(mktemp)
trap 'rm -f "$IDX"' EXIT

cd "$WT"
export GIT_INDEX_FILE=$IDX
git read-tree HEAD
git add -A
TREE=$(git write-tree)
unset GIT_INDEX_FILE
HEAD=$(git rev-parse HEAD)
PREV=$(git rev-parse -q --verify "$REF" || true)
if [ -n "$PREV" ] && [ "$(git rev-parse "$PREV^{tree}")" = "$TREE" ]; then
	echo "unchanged: $REF $PREV"
	exit 0
fi
if [ "$(git rev-parse "$HEAD^{tree}")" = "$TREE" ] && [ -z "$PREV" ]; then
	echo "nothing beyond HEAD $HEAD"
	exit 0
fi
PARENTS="-p $HEAD"
if [ -n "$PREV" ] && [ "$PREV" != "$HEAD" ]; then
	PARENTS="$PARENTS -p $PREV"
fi
C=$(git commit-tree "$TREE" $PARENTS -m "WIP snapshot of $NAME at $(date '+%Y-%m-%d %H:%M')")
git update-ref "$REF" "$C"
echo "$REF $C"
