#!/bin/bash
# vayu: the data volume of iOS (/private/var) as an HFS Plus image, for darwin-install.sh var to write into
# darwin's var region (darwin-layout.py: VAR_FIRST/VAR_BLOCKS; the loader hands the kernel the same numbers as
# ufs_var_first=/ufs_var_blocks=, and init mounts it with var_hfs=1). Case-sensitive (HFSX, as iOS's own data
# volume is) and journaled, owned by root, 0755. The file is sparse: mkfs writes only the volume's own
# structures, and only those are written to the phone; the rest is free space, which HFS Plus never reads
# before it has written it. (After another HakiOS port's var-volume.sh, MIT: telegdyd/ios-vayu, branch vayu-dev,
# commit 274f9827f52f; the size is the layout's, not a constant.)
#
#   devices/poco-x3-pro/tools/var-volume.sh OUT       (needs mkfs.hfsplus: Debian's hfsprogs)
#
# The volume starts empty; init fills it at its first boot (notes of o2: the skeleton, then with var_import=1
# the newest var_store archive).
set -euo pipefail
OUT=${1:?usage: var-volume.sh OUT}
eval "$(python3 -I "$(dirname "$0")/darwin-layout.py" sh)"
[ ! -e "$OUT" ] || { echo "var-volume: $OUT exists" >&2; exit 1; }
truncate -s $((VAR_BLOCKS * BS)) "$OUT"
mkfs.hfsplus -s -J -b $BS -U 0 -G 0 -M 755 -v Data "$OUT"
echo "var-volume: $OUT: $((VAR_BLOCKS * BS >> 30)) GiB, $(du -k "$OUT" | cut -f1) KB of it written by mkfs"
