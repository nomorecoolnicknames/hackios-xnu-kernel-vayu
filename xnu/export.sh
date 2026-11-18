#!/bin/bash
# Save the kernel work as it is in the working tree, in the two forms apply.sh puts back:
#   hakios.patch   the changes to Apple's files (against the tag the work sits on)
#   files/          the files that are not in Apple's tree, at their paths
# usage: xnu/export.sh [KERNEL-TREE]     (default: $XNU_SRC, else xnu-src/xnu-8796.121.2)
# Run it before committing whenever the kernel tree has changed. Committed or not makes no
# difference there: everything is compared with the base commit.
# Apple's HFS Plus (bsd/hfs, bsd/hfs_encodings) is not kept: its files are fetched again by apply.sh
# (xnu/hfs/hfs.sh), and what this port changes in them goes into xnu/hfs/hfs.patch.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a           # tag xnu-8796.121.2
# never part of the work: build output and what editors, patch and python leave behind
JUNK='^BUILD/|(^|/)__pycache__/|\.(orig|rej|pyc|o|swp)$|\.bak[0-9]*$|~$|(^|/)\.DS_Store$'

cd "$SRC"
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "export.sh: $SRC is not a clone of xnu at $BASE" >&2; exit 1; }

# no renames: a moved file is a deletion in the patch and a new file in files/
git -c core.abbrev=12 diff --binary --no-renames --no-color --no-ext-diff --diff-filter=MDT "$BASE" -- . > "$HERE/hakios.patch.new"
mv "$HERE/hakios.patch.new" "$HERE/hakios.patch"

rm -rf "$HERE/files.new"
mkdir "$HERE/files.new"
{ git diff --no-renames --name-only --diff-filter=A "$BASE" -- .; git ls-files --others --exclude-standard; } |
	sort -u | grep -Ev "$JUNK" |
	# Apple's HFS sources are fetched by xnu/hfs/hfs.sh, not kept: only this port's own file of them
	grep -Ev '^bsd/hfs(_encodings)?/' > "$HERE/files.list"
[ -f bsd/hfs/hfs_builtin.c ] && echo bsd/hfs/hfs_builtin.c >> "$HERE/files.list"
while IFS= read -r f; do
	[ -f "$f" ] || [ -L "$f" ] || continue      # added in a commit, deleted since
	mkdir -p "$HERE/files.new/$(dirname "$f")"
	if [ "$(uname)" = Darwin ]; then cp -pP "$f" "$HERE/files.new/$f"; else cp -P --preserve=mode "$f" "$HERE/files.new/$f"; fi
done < "$HERE/files.list"
# Apple's HFS Plus: a file of it that is not in xnu/hfs/*.list would be lost here, so it is refused
UNLISTED=$( { ls bsd/hfs 2>/dev/null | grep -vxF hfs_builtin.c | grep -vxF -f "$HERE/hfs/core.list";
	ls bsd/hfs_encodings 2>/dev/null | grep -vxF -f "$HERE/hfs/encodings.list"; } || true)
if [ -n "$UNLISTED" ]; then
	echo "export.sh: bsd/hfs or bsd/hfs_encodings has files xnu/hfs/*.list does not name:" $UNLISTED >&2
	rm -rf "$HERE/files.new" "$HERE/files.list"; exit 1
fi
[ ! -d bsd/hfs ] || "$HERE/hfs/hfs.sh" export "$SRC"
rm -rf "$HERE/files" "$HERE/files.list"
mv "$HERE/files.new" "$HERE/files"

echo "xnu/hakios.patch: $(grep -c '^diff --git' "$HERE/hakios.patch") files changed, $(wc -c < "$HERE/hakios.patch") bytes"
echo "xnu/files: $(find "$HERE/files" -type f | wc -l) new files, $(du -sk "$HERE/files" | cut -f1) KB"
echo "xnu/hfs/hfs.patch: $(grep -c '^+++ ' "$HERE/hfs/hfs.patch") of Apple's HFS files changed"
