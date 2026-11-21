#!/bin/bash
# ufs-abi-test.sh [LOADER_C]: the UFS mailbox page as this kernel lays it out (bsd/sys/vayu_ufs_mailbox.h) against the
# loader's own definition (xnuloader/loader.c, struct loader_block_part and struct loader_block, default
# xnuloader 2.7): both compiled for the same 64-bit target, every field's offset and size and both structs'
# sizes and alignments printed and compared. Exit 0 when they are the same.
set -eu
here=$(cd "$(dirname "$0")" && pwd); tree=$(cd "$here/../../../.." && pwd)
L=${1:-$tree/../xnuloader12377/xnuloader/loader.c}
CC=${CC:-cc}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# the loader's two structs and the constants they need, as its source has them
python3 - "$L" > "$W/loader_structs.h" <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
out = ['#include <stdint.h>']
for name in ('BLOCK_MAX_PARTS',):
    m = re.search(r'^#define\s+%s\s+\S+.*$' % name, s, re.M)
    out.append(m.group(0))
for struct in ('loader_block_part', 'loader_block'):
    m = re.search(r'^struct %s \{.*?^\};' % struct, s, re.M | re.S)
    if not m:
        sys.exit('no struct %s in %s' % (struct, sys.argv[1]))
    out.append(m.group(0))
print('\n'.join(out))
EOF
fields='magic version size parts buffer_size buffer_pa write_part_plus_one write_caps write_first bcb_part_plus_one
bcb_reserved ask ask.request ask.part ask.lba ask.count ask.op answer answer.answered answer.status answer.served
answer.failed answer.bytes answer.ticks part native native.hc_pa native.area_pa native.area_size native.ucd_offset
native.ucd_size native.slots native.uefi_utrlba native.uefi_utrlbau loader_native loader_native.take_back kernel_native
kernel_native.list_in kernel_native.switched_in kernel_native.switched_out kernel_native.cnr_at_switch'
pfields='name block_size blocks start_lba unit lun flags reserved'
gen() {         # HEADER
	echo "#include <stdio.h>"; echo "#include <stddef.h>"; echo "#include \"$1\""
	echo "#define F(s, f) printf(\"%s.%s %zu %zu\\n\", #s, #f, offsetof(struct s, f), sizeof(((struct s *)0)->f))"
	echo "int main(void) {"
	echo "printf(\"sizeof loader_block %zu align %zu\\n\", sizeof(struct loader_block), _Alignof(struct loader_block));"
	echo "printf(\"sizeof loader_block_part %zu align %zu\\n\", sizeof(struct loader_block_part), _Alignof(struct loader_block_part));"
	for f in $fields; do echo "F(loader_block, $f);"; done
	for f in $pfields; do echo "F(loader_block_part, $f);"; done
	echo "return 0; }"
}
gen "$W/loader_structs.h" > "$W/loader.c"; gen "$tree/bsd/sys/vayu_ufs_mailbox.h" > "$W/kernel.c"
$CC -std=c11 -Wall -Werror -o "$W/loader" "$W/loader.c"; $CC -std=c11 -Wall -Werror -o "$W/kernel" "$W/kernel.c"
"$W/loader" > "$W/loader.txt"; "$W/kernel" > "$W/kernel.txt"
n=$(wc -l < "$W/kernel.txt")
if diff -u "$W/loader.txt" "$W/kernel.txt"; then
	head -2 "$W/kernel.txt"; echo "ufs-abi-test: PASS ($n lines the same: $(basename "$L") and vayu_ufs_mailbox.h)"
else
	echo "ufs-abi-test: FAIL"; exit 1
fi
