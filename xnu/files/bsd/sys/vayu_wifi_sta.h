/*
 * W23a: open-system AUTH + ASSOC to the test AP (vayu_wifi_ssid) after the W22 scan
 * (boot-arg vayu_wifi_join=1, only with vayu_wifi_scan=1).  No EAPOL, no keys:
 * 3 s of RX counting after the association, then DEAUTH (reason 3) and VDEV_STOP.
 * Sequence and WMI layouts from ios-flame phone/wifi/radio.c (commit 745c4341,
 * w_vdev_start / w_peer / w_mgmt_tx / w_send_auth / w_build_assoc_req /
 * sta_mgmt_rx / sta_poll, lines 1326-1884), itself a port of Linux ath10k
 * wmi-tlv.c (wmi_tlv_mgmt_tx_cmd, wmi_tlv_mgmt_tx_compl_ev).
 *
 * MIT License
 *
 * Copyright (c) 2026 dipa-studio (ios-flame)
 * Copyright (c) 2026 The Redwood Project (W23a adaptation)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SYS_VAYU_WIFI_STA_H_
#define _SYS_VAYU_WIFI_STA_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.sta.h>
#include <sys/vayu_wifi_scan.h>

/* boot-arg vayu_wifi_join: 0 off (W22b behaviour), 1 = auth + assoc to the network of vayu_wifi_ssid
 * (vayu_wifi_bssid_tail=XX:XX: only the AP whose BSSID ends so) after the scan; no SSID, no boot join. */
extern uint32_t vayu_sta_mode;

/* Reads the boot-arg and clears the state; call after vayu_scan_reset(). */
void vayu_sta_reset(int (*log)(const char *format, ...) __printflike(1, 2));
/* One WMI event (payload after the 4-byte id).  Returns at once with vayu_sta_mode == 0. */
void vayu_sta_wmi_rx(uint32_t id, const uint8_t *p, uint32_t len);
/* One HTT T2H message (counted only inside the post-association window). */
void vayu_sta_htt_rx(const uint8_t *p, uint32_t len);
/* After the scan: join the network of vayu_wifi_join (if it was heard), then serve iOS's requests
 * (a new scan when there is no link, a join with the passphrase it sends, a leave) until the CE
 * stage is stopped.  0 = the loop ended normally, -1 = no station. */
int vayu_sta_run(const struct vayu_scan_ops *ops);
/* W25r: what bsd/dev/vayu_wifi_net.c (the station boundary) calls on its thread.  Each waits for
 * the loop above; 0 or an errno. */
int vayu_sta_ask_scan(const struct hakios_sta_scan *what);
int vayu_sta_ask_join(const struct hakios_sta_join *what);
int vayu_sta_ask_leave(void);
bool vayu_sta_serving(void);

#endif /* _SYS_VAYU_WIFI_STA_H_ */
