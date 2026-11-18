#!/bin/bash
# The radios' fixed paths of the kernel tree (XNU_SRC=<tree>, else xnu/files) on this machine, the
# functions taken out of the driver sources as they are and run against host stand-ins for the IO:
# BT's kctl H4 ring and vendor-command shim (vayu_bt.c), the NFC bridge's RX loop (vayu_nfc.c), the
# CE TX of the Wi-Fi HTC layer (vayu_wifi_htc_connect.c) and the Ethernet -> 802.11 header
# (vayu_wifi_net.c).
#   CC=... XNU_SRC=/path/to/tree tools/radio-test/run.sh
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${XNU_SRC:-$ROOT/xnu/files}
B=$ROOT/xnu-deps/build/radio-test
rm -rf "$B"
mkdir -p "$B"
cp "$K/bsd/sys/vayu_wifi_htc_connect.h" "$B/"
python3 - "$K" "$B" <<'PY'
from pathlib import Path
import re
import sys
k, out = map(Path, sys.argv[1:])
bt = (k / 'bsd/dev/vayu_bt.c').read_text()
nfc = (k / 'bsd/dev/vayu_nfc.c').read_text()
htc = (k / 'bsd/dev/vayu_wifi_htc_connect.c').read_text()
net = (k / 'bsd/dev/vayu_wifi_net.c').read_text()
def function(text, name):
    match = re.search(r'^static [^\n]+\n' + name + r'\(', text, re.M)
    assert match, name
    end = text.index('\n}', match.start()) + 2
    return text[match.start():end] + '\n'
def section(text, start, end):
    return text[text.index(start):text.index(end)]
(out / 'bt_queue.inc').write_text(
    section(bt, 'struct bt_kctl_tx {', 'static uint32_t g_kctl_rx_enobufs;') +
    ''.join(function(bt, n) for n in ['bt_kctl_tx_put_locked', 'bt_kctl_tx_drain', 'bt_kctl_send']))
(out / 'bt_vsc.inc').write_text(
    section(bt, '#define BT_VSC_PASS ', '#define BT_VSC_LOG_FIRST ') +
    ''.join(function(bt, n) for n in ['bt_vsc_find', 'bt_vsc_answer', 'bt_vsc_tx_filter']))
(out / 'nfc_bridge.inc').write_text(function(nfc, 'bridge_thread'))
(out / 'htc_tx.inc').write_text(
    section(htc, '#define HTCC_SR_WR_IDX ', 'uint32_t vayu_htcc_mode;') +
    ''.join(function(htc, n) for n in ['htcc_put16', 'htcc_barrier', 'htcc_txdump', 'htcc_ce_tx'])
    .replace('__asm__ volatile("dsb sy" ::: "memory");', '__atomic_thread_fence(__ATOMIC_SEQ_CST);'))
(out / 'net_tx.inc').write_text(function(net, 'net_send_eth'))
PY
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I"$B" -o "$B/test" "$HERE/test_radio.c"
echo "radio-test: against $K"
"$B/test"
