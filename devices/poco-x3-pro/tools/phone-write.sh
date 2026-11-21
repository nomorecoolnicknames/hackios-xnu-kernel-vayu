# vayu: writing a partition from the recovery (PBRP, adb as root) so that the write is known to be done.
# Sourced by standalone.sh (boot) and darwin-install.sh (darwin); not run by itself.
#
# Why: `adb exec-in "dd of=..." < IMAGE` is no barrier. The client returns once it has sent stdin -- not when dd
# on the phone is done, nor its fsync, and it never gets dd's exit status (AOSP adb client/commandline.cpp,
# exec-in: copy_to_file, then return 0). A read back right after it may race the write; a rollback right after
# it is a second writer on the same partition. Twice boot did not read back so (runs of 18.11);
# adb push + dd from the file on the phone did. reports/w/W3-flash-write.md has the whole story.
#
# So every write here is:
#   1. on the host: the bytes in a file of their own (a chunk), its length and sha256;
#   2. the staging place on the phone (STAGE_DIR, default /tmp: PBRP's tmpfs, in RAM) has room for it and a
#      reserve (PW_RESERVE_MB, default 32), and the name, unique to this run, is not there yet;
#   3. adb push the file there (push does wait: it is acknowledged file by file);
#   4. one adb shell, which waits for its command, and in it, in this order: the staged copy's length and sha256
#      are the host's; no dd is writing the node now (a writer left from before); the caller's GUARD (the node is
#      still what it should be); dd if=COPY of=NODE bs seek count conv=notrunc,fsync, its exit status checked;
#      sync; drop_caches; the range read back and its sha256 compared; the copy removed; and last, one line
#      "PW-END ok" or "PW-END fail WHY". The status is that line, not adb's exit code: an old adbd without the
#      shell protocol returns 0 whatever the command did, and a line the shell did not get to print is no "ok".
#   5. No PW-END line (the transport broke, adb was killed): the state is UNKNOWN -- dd may still be running. Then
#      no other write starts: the phone is asked, once a second for PW_WAIT seconds (default 300), whether a
#      process still has this chunk's name in its command line or a dd still writes the node. Once nothing does,
#      the range is read back; if it never settles (or the phone is not there), pw_put says so and the caller
#      stops without writing anything else (no rollback on top of a writer that may still be running).
#
# Big regions (darwin's root is 5.6 GB, the recovery's tmpfs is RAM) go in chunks: PW_CHUNK_MB (default 64) at
# a time, each through all of the above, each read back by itself; the caller still reads back the whole region
# at the end. A chunk is a whole number of 4096 blocks; only the last bytes of a single-file write (a boot image
# that is not whole blocks) may be a short last block: dd writes exactly the file's length, the bytes after it
# stay as they were.
#
# The phone's side is POSIX sh with toybox (mksh in PBRP): no awk, no local, no pipefail. The read back pipes
# dd into head and sha256sum without their statuses, which is fail-closed: a short or failed read can only give
# another sha256, never the expected one.
#
# The caller sets: A (its adb with -s SERIAL), PW_SAY (the name of its logging function).
# Taken from the other HakiOS port's tools/darwin-write.py (telegdyd/ios-vayu, vayu-dev: push, the copy's sha256,
# guard, dd from the copy, read back) -- the idea only, not its constants.

PW_DIR=${STAGE_DIR:-/tmp}
PW_CHUNK_BLOCKS=${PW_CHUNK_BLOCKS:-$(( ${PW_CHUNK_MB:-64} * 256 ))}
PW_RESERVE_MB=${PW_RESERVE_MB:-32}
PW_WAIT=${PW_WAIT:-300}
PW_ID="hakios-w.$(date +%s).$$"
PW_N=0

pw_say() { "${PW_SAY:-echo}" "$@"; }

# On the phone. busy_name NAME: a process whose command line has NAME (the shell that writes a chunk has its
# staged name in it); call it with NAME put together from two halves, so that the asking shell's own command
# line does not have it. busy_dd NODE: a dd (argv[0] dd or .../dd) with of=NODE among its arguments.
PW_FN='busy_name() { for f in /proc/[0-9]*/cmdline; do c=" $(tr "\000" " " < $f 2>/dev/null)"; case "$c" in *"$1"*) echo "${f%/cmdline}"; return 0;; esac; done; return 1; };
busy_dd() { for f in /proc/[0-9]*/cmdline; do c=" $(tr "\000" " " < $f 2>/dev/null)"; w=${c# }; w=${w%% *}; case "$w" in dd|*/dd) case "$c" in *" of=$1 "*) echo "${f%/cmdline}"; return 0;; esac;; esac; done; return 1; };'

pw_sh() { $A shell "$1" 2>&1 | tr -d '\r'; }

# pw_readback NODE BS SEEK COUNT LEN: sha256 of LEN bytes of NODE from block SEEK, the caches dropped first
pw_readback() {
	pw_sh "sync; echo 3 > /proc/sys/vm/drop_caches; dd if=$1 bs=$2 skip=$3 count=$4 2>/dev/null | head -c $5 | sha256sum" | cut -d' ' -f1 | tail -1
}

# the phone's script for one staged chunk: NODE GUARD BS SEEK COUNT LEN SHA COPY
pw_script() {
	cat <<EOF
$PW_FN
t=$8; end() { rm -f "\$t"; echo "PW-END \$*"; exit 0; }
n=\$(stat -c %s "\$t" 2>&1); [ "\$n" = $6 ] || end fail "the staged copy is \$n bytes, not $6"
[ "\$(sha256sum "\$t" | cut -d' ' -f1)" = $7 ] || end fail "the staged copy's sha256 is not the host's"
p=\$(busy_dd $1) && end fail "a dd is writing $1 already (\$p): not a second writer"
$2 || end fail "the guard: $1 is not what it should be"
dd if="\$t" of=$1 bs=$3 seek=$4 count=$5 conv=notrunc,fsync 2>&1; r=\$?
[ \$r = 0 ] || end fail "dd exited \$r"
sync; echo 3 > /proc/sys/vm/drop_caches
h=\$(dd if=$1 bs=$3 skip=$4 count=$5 2>/dev/null | head -c $6 | sha256sum | cut -d' ' -f1)
[ "\$h" = $7 ] || end fail "blocks $4+$5 read back as \$h, not $7"
end ok
EOF
}

# pw_settle NAME NODE: wait until no process on the phone has NAME, nor a dd writes NODE; 0 when so
pw_settle() {
	local name=$1 node=$2 i=0 a b
	a=${name%%.*}; b=.${name#*.}
	while [ $i -lt "$PW_WAIT" ]; do
		case "$(pw_sh "$PW_FN a=$a; b=$b; busy_name \"\$a\$b\" || busy_dd $node || echo PW-IDLE")" in
		*PW-IDLE*) pw_sh "rm -f $PW_DIR/$name" >/dev/null; return 0 ;;
		esac
		sleep 1; i=$((i + 1))
	done
	return 1
}

# pw_put FILE NODE GUARD BS SEEK: FILE's bytes into NODE from block SEEK (blocks of BS), as above.
#   0: written, read back the same.  1: not done, and nothing of it is writing now (the bytes there may be the
#   old ones, the new ones or partly either -- the caller reads back).  3: UNKNOWN, a writer may still be running.
pw_put() {
	local f=$1 node=$2 guard=$3 bs=$4 seek=$5 len sha count name tmp free out end
	len=$(stat -c %s "$f") || return 1
	sha=$(sha256sum "$f" | cut -d' ' -f1)
	count=$(( (len + bs - 1) / bs ))
	[ "$len" -gt 0 ] || { pw_say "pw_put: $f is empty"; return 1; }
	PW_N=$((PW_N + 1)); name=$PW_ID.$PW_N; tmp=$PW_DIR/$name
	free=$(pw_sh "[ -d $PW_DIR ] && [ ! -e $tmp ] && set -- \$(df -k $PW_DIR | tail -1) && echo \$4" | tail -1)
	case "$free" in
	''|*[!0-9]*) pw_say "pw_put: no staging place on the phone ($PW_DIR missing, $tmp there already, or no df): nothing written"; return 1 ;;
	esac
	[ "$free" -ge $(( (len >> 10) + 1 + PW_RESERVE_MB * 1024 )) ] ||
		{ pw_say "pw_put: $PW_DIR on the phone has $free KiB free, $((len >> 10)) KiB + ${PW_RESERVE_MB} MiB wanted (smaller PW_CHUNK_MB, or STAGE_DIR): nothing written"; return 1; }
	out=$($A push "$f" "$tmp" 2>&1) ||
		{ pw_say "pw_put: adb push $f -> $tmp failed: $(echo "$out" | tr -d '\r' | tail -1); nothing written"; pw_sh "rm -f $tmp" >/dev/null; return 1; }
	out=$(pw_sh "$(pw_script "$node" "$guard" "$bs" "$seek" "$count" "$len" "$sha" "$tmp")")
	end=$(echo "$out" | grep '^PW-END ' | tail -1)
	case "$end" in
	"PW-END ok") return 0 ;;
	"PW-END fail"*)
		pw_say "pw_put: blocks $seek+$count of $node: ${end#PW-END fail }"
		echo "$out" | grep -v '^PW-END ' | tail -5 | sed 's/^/    phone: /'
		return 1 ;;
	esac
	pw_say "pw_put: no end line from the phone for blocks $seek+$count of $node (adb: $(echo "$out" | tail -1)): UNKNOWN, a dd may still be running; waiting up to $PW_WAIT s for it, no other write meanwhile"
	if ! pw_settle "$name" "$node"; then
		pw_say "pw_put: UNKNOWN: in $PW_WAIT s the phone did not say that the writer of $name is gone; nothing else is written"
		return 3
	fi
	if [ "$(pw_readback "$node" "$bs" "$seek" "$count" "$len")" = "$sha" ]; then
		pw_say "pw_put: the writer is gone, blocks $seek+$count of $node read back the same"
		return 0
	fi
	pw_say "pw_put: the writer is gone, blocks $seek+$count of $node do NOT read back the same"
	return 1
}

# pw_put_blocks IMAGE SKIP COUNT NODE GUARD AT: COUNT blocks of 4096 of IMAGE from its block SKIP into NODE at
# its block AT, PW_CHUNK_BLOCKS at a time, each chunk through pw_put. Returns pw_put's 0, 1 or 3.
pw_put_blocks() {
	local img=$1 skip=$2 count=$3 node=$4 guard=$5 at=$6 did=0 c r part
	part=$(mktemp "${TMPDIR:-/tmp}/pw-chunk.XXXXXX") || return 1
	while [ $did -lt "$count" ]; do
		c=$((count - did)); [ $c -gt "$PW_CHUNK_BLOCKS" ] && c=$PW_CHUNK_BLOCKS
		if ! dd if="$img" of="$part" bs=4096 skip=$((skip + did)) count=$c 2>/dev/null || [ "$(stat -c %s "$part")" != $((c * 4096)) ]; then
			rm -f "$part"; pw_say "pw_put_blocks: could not cut blocks $((skip + did))+$c out of $img"; return 1
		fi
		pw_put "$part" "$node" "$guard" 4096 $((at + did)); r=$?
		[ $r = 0 ] || { rm -f "$part"; return $r; }
		did=$((did + c))
		[ "$count" -le "$PW_CHUNK_BLOCKS" ] || echo "    $((did * 100 / count))%: $((did >> 8)) of $((count >> 8)) MiB, each chunk read back"
	done
	rm -f "$part"
	return 0
}
