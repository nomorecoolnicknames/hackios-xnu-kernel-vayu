#!/bin/bash
# The port's other kernel lines, kept as xnu/ keeps the iOS 16.5 one: a patch to the base tree they sit on plus
# the files that are new, in xnu/lines/LINE/ (hakios.patch, files/, and hfs.patch where the line has HFS Plus as the
# iOS 16.5 kernel does). xnu/lines/LINE/line.conf names the base: URL, REF (a tag, or a commit fetched by its id),
# BASE (the commit it must be), HFS (yes: Apple's hfs-627.100.6 through xnu/hfs/hfs.sh with this line's hfs.patch).
#   xnu/line.sh apply LINE [DIR]    the base fetched into DIR (default xnu-src/LINE), the line's work laid over it
#                                   (a git tree on branch "hakios" with the work uncommitted, as xnu/apply.sh makes)
#   xnu/line.sh export LINE DIR     the reverse: hakios.patch, files/ (and hfs.patch) again from DIR, a clone of the
#                                   base with the line's work in its working tree
# env: LINE_URL  another place to fetch the base from (a clone that has REF: file:///path); HFS_URL as xnu/hfs/hfs.sh
# Lines: ios27 (xnu-12377, iOS 27), macos13 (xnu-8796.121.2, macOS 13.4). Their state: xnu/lines/LINE/README.md.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
CMD=$1 LINE=$2
L=$HERE/lines/$LINE
[ -n "$CMD" ] && [ -n "$LINE" ] && [ -f "$L/line.conf" ] || { echo "usage: xnu/line.sh apply|export LINE [DIR] (xnu/lines/*/line.conf)" >&2; exit 1; }
. "$L/line.conf"
DIR=${3:-$ROOT/xnu-src/$LINE}
# never part of the work: build output and what editors, patch and python leave behind
JUNK='^BUILD/|(^|/)__pycache__/|\.(orig|rej|pyc|o|swp)$|\.bak[0-9]*$|~$|(^|/)\.DS_Store$'

case "$CMD" in
apply)
	[ -e "$DIR" ] && { echo "line.sh: $DIR already exists; not touching it" >&2; exit 1; }
	[ -s "$L/hakios.patch" ] && [ -d "$L/files" ] || { echo "line.sh: no hakios.patch and files/ in $L" >&2; exit 1; }
	mkdir -p "$(dirname "$DIR")"
	DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
	git init -q "$DIR"
	trap 'echo "line.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
	cd "$DIR"
	git remote add origin "${LINE_URL:-$URL}"
	case "$REF" in
	refs/*) git fetch -q --depth 1 origin "$REF:$REF"; git checkout -q -b hakios "$REF" ;;
	*)      git fetch -q --depth 1 origin "$REF"; git checkout -q -b hakios FETCH_HEAD ;;
	esac
	[ "$(git rev-parse HEAD)" = "$BASE" ] || { echo "line.sh: $REF is $(git rev-parse HEAD), expected $BASE" >&2; false; }
	echo 'BUILD/' >> .git/info/exclude
	git apply --whitespace=nowarn "$L/hakios.patch"
	cp -a "$L/files/." .
	[ "$HFS" != yes ] || HFS_PATCH="$L/hfs.patch" "$HERE/hfs/hfs.sh" put "$DIR"
	echo "line.sh: $DIR: $LINE, $REF + $(grep -c '^diff --git' "$L/hakios.patch") changed and $(find "$L/files" -type f | wc -l) new files"
	;;
export)
	[ -d "$DIR/.git" ] || { echo "line.sh: export wants DIR, a clone of the base with the work in it" >&2; exit 1; }
	cd "$DIR"
	git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "line.sh: $DIR is not a clone at $BASE" >&2; exit 1; }
	HFSX='^$'
	[ "$HFS" != yes ] || HFSX='^bsd/hfs(_encodings)?/'
	git -c core.abbrev=12 diff --binary --no-renames --no-color --no-ext-diff --diff-filter=MDT "$BASE" -- . > "$L/hakios.patch.new"
	mv "$L/hakios.patch.new" "$L/hakios.patch"
	rm -rf "$L/files.new"; mkdir "$L/files.new"
	{ git diff --no-renames --name-only --diff-filter=A "$BASE" -- .; git ls-files --others --exclude-standard; } |
		sort -u | grep -Ev "$JUNK" | grep -Ev "$HFSX" > "$L/files.list" || true
	[ "$HFS" != yes ] || [ ! -f bsd/hfs/hfs_builtin.c ] || echo bsd/hfs/hfs_builtin.c >> "$L/files.list"
	while IFS= read -r f; do
		[ -f "$f" ] || [ -L "$f" ] || continue
		mkdir -p "$L/files.new/$(dirname "$f")"
		if [ "$(uname)" = Darwin ]; then cp -pP "$f" "$L/files.new/$f"; else cp -P --preserve=mode "$f" "$L/files.new/$f"; fi
	done < "$L/files.list"
	if [ "$HFS" = yes ]; then
		UNLISTED=$( { ls bsd/hfs 2>/dev/null | grep -vxF hfs_builtin.c | grep -vxF -f "$HERE/hfs/core.list";
			ls bsd/hfs_encodings 2>/dev/null | grep -vxF -f "$HERE/hfs/encodings.list"; } || true)
		[ -z "$UNLISTED" ] || { echo "line.sh: bsd/hfs has files xnu/hfs/*.list does not name:" $UNLISTED >&2;
			rm -rf "$L/files.new" "$L/files.list"; exit 1; }
		HFS_PATCH="$L/hfs.patch" "$HERE/hfs/hfs.sh" export "$DIR"
	fi
	rm -rf "$L/files" "$L/files.list"; mv "$L/files.new" "$L/files"
	echo "xnu/lines/$LINE/hakios.patch: $(grep -c '^diff --git' "$L/hakios.patch") files changed, $(wc -c < "$L/hakios.patch") bytes"
	echo "xnu/lines/$LINE/files: $(find "$L/files" -type f | wc -l) new files, $(du -sk "$L/files" | cut -f1) KB"
	;;
*)
	echo "usage: xnu/line.sh apply|export LINE [DIR]" >&2; exit 1 ;;
esac
