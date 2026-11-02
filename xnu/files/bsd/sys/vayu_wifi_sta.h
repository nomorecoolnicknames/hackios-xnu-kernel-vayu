/*
 * W23a: open-system AUTH + ASSOC to the owner's AP "Gunwest" after the W22 scan
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
#include <sys/vayu_wifi_scan.h>

/* boot-arg vayu_wifi_join: 0 off (W22b behaviour), 1 = auth + assoc to "Gunwest" after the scan. */
extern uint32_t vayu_sta_mode;

/* Reads the boot-arg and clears the state; call after vayu_scan_reset(). */
void vayu_sta_reset(int (*log)(const char *format, ...) __printflike(1, 2));
/* One WMI event (payload after the 4-byte id).  Returns at once with vayu_sta_mode == 0. */
void vayu_sta_wmi_rx(uint32_t id, const uint8_t *p, uint32_t len);
/* One HTT T2H message (counted only inside the post-association window). */
void vayu_sta_htt_rx(const uint8_t *p, uint32_t len);
/* After the scan: join, 3 s RX count, deauth, VDEV_STOP.  0 = assoc status 0 reached. */
int vayu_sta_run(const struct vayu_scan_ops *ops);

#endif /* _SYS_VAYU_WIFI_STA_H_ */
