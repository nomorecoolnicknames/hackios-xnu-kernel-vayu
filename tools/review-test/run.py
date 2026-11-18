#!/usr/bin/env python3
"""Host fault injection into the kernel tree's own driver functions (XNU_SRC=<tree>, else xnu/files), taken
out of the sources as they are: SMS job results, RMTFS read errors, FastRPC release, USB ECM control and
notification bytes, the camera session's DMA reservation."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TREE = Path(os.environ.get('XNU_SRC', ROOT / 'xnu/files'))
BUILD = ROOT / 'xnu-deps/build/review-test'
BUILD.mkdir(parents=True, exist_ok=True)
print('review-test: against', TREE, flush=True)


def source(name):
    return (TREE / 'bsd/dev' / name).read_text()


def function(text, name):
    start = text.index('\n' + name + '(') + 1
    start = text.rfind('\n', 0, start - 1) + 1
    return text[start:text.index('\n}', start) + 2] + '\n'


def declaration(text, name):
    start = text.index(name + ' {')
    return text[start:text.index('\n};', start) + 3] + '\n'


sms = source('vayu_sms.c')
rmtfs = source('vayu_rmtfs.c')
frpc = source('vayu_fastrpc.c')
common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static unsigned logs;
static __attribute__((unused)) int say(const char *format, ...) { (void)format; logs++; return 0; }
'''
queue = common + '#include "' + str(TREE / 'bsd/sys/vayu_sms.h') + '"\n'
queue += declaration(sms, 'enum sms_job_kind') + declaration(sms, 'struct sms_job')
queue += r'''
#define PRIBIO 0
static struct {
    bool wms_there, running, stop, job_busy, job_waiting, job_pending, job_done, work;
    unsigned state;
    struct sms_job job;
    int job_result;
    uint32_t job_message_id, job_mr, job_pdu_length;
} sms;
static int vayu_sms_lock;
static bool locked, timeout_job, queue_attempted;
static int interloper_result;
static void lck_mtx_lock(int *lock) { (void)lock; assert(!locked); locked = true; }
static void lck_mtx_unlock(int *lock) { (void)lock; assert(locked); locked = false; }
static void wakeup(void *event) { (void)event; assert(locked); }
static int sms_do_send(const char *number, const char *text, uint32_t length,
    uint32_t *id, uint32_t *mr, uint32_t *pdu)
{
    (void)text; (void)length;
    *id = (uint32_t)number[0]; *mr = *id + 1; *pdu = *id + 2; return 0;
}
static int sms_do_delete(uint32_t seq) { (void)seq; return 0; }
static uint32_t sms_refresh_store(uint8_t storage, bool quiet) { (void)quiet; return storage + 100; }
static int msleep(void *, int *, int, const char *, const struct timespec *);
'''
queue += function(sms, 'sms_run_job') + function(sms, 'sms_submit')
queue += r'''
static int msleep(void *event, int *lock, int priority, const char *name, const struct timespec *ts)
{
    (void)event; (void)priority; (void)ts;
    assert(locked);
    if (strcmp(name, "vayu-sms-queue") == 0 || timeout_job) return EWOULDBLOCK;
    lck_mtx_unlock(lock);
    sms_run_job(&sms.job);
    if (!queue_attempted) {
        struct sms_job interloper = { .kind = SMS_JOB_SEND, .number = "B" };
        queue_attempted = true;
        interloper_result = sms_submit(&interloper);
    }
    lck_mtx_lock(lock);
    return 0;
}
int main(void)
{
    struct sms_job first = { .kind = SMS_JOB_SEND, .number = "A" };
    struct sms_job second = { .kind = SMS_JOB_REFRESH, .storage = 1 };
    sms.wms_there = sms.running = true;
    sms.state = 1;
    assert(sms_submit(&first) == 0);
    assert(interloper_result == EBUSY);
    assert(first.message_id == 'A' && first.mr == 'A' + 1 && first.pdu_length == 'A' + 2);
    assert(sms_submit(&second) == 0 && second.pdu_length == 101);
    assert(first.message_id == 'A' && first.pdu_length == 'A' + 2);
    timeout_job = true;
    assert(sms_submit(&first) == -ETIMEDOUT && sms.job_busy && !sms.job_waiting);
    assert(sms_submit(&second) == EBUSY);
    timeout_job = false;
    sms_run_job(&sms.job);
    assert(sms_submit(&second) == 0 && second.pdu_length == 101);
    puts("review-test: SMS completion interleaving, refresh and timeout PASS");
}
'''
pages = common + declaration(rmtfs, 'struct vayu_rmtfs_part') + r'''
#define PAGE_SIZE 4096
#define VAYU_RMTFS_SECTOR 512
#define Z_WAITOK 1
#define Z_ZERO 2
#define NSEC_PER_MSEC 1000000
static uint64_t rmtfs_drop_warning_ms, now_ms = 100;
static bool fail_read, fail_alloc;
static unsigned reads, freed;
static uint64_t mach_absolute_time(void) { return now_ms * NSEC_PER_MSEC; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static void *kalloc_data(size_t size, int flags) { (void)flags; return fail_alloc ? NULL : calloc(1, size); }
static void kfree_data(void *page, size_t size) { (void)size; freed++; free(page); }
static int hakios_ufs_part_read(uint32_t part, uint64_t at, void *page, uint64_t size)
{
    (void)part; (void)at; reads++;
    memset(page, 0x5a, size);
    return fail_read ? EIO : 0;
}
'''
pages += ''.join(function(rmtfs, name) for name in
                 ['rmtfs_write_warning', 'rmtfs_page', 'rmtfs_read_sector', 'rmtfs_write_sector'])
pages += r'''
int main(void)
{
    uint8_t *table[1] = { NULL }, sector[512];
    struct vayu_rmtfs_part part = { .name = "synthetic", .bytes = PAGE_SIZE,
        .ufs_found = true, .page = table, .page_count = 1 };
    fail_read = true;
    memset(sector, 0xab, sizeof(sector));
    assert(!rmtfs_read_sector(&part, 0, sector) && table[0] == NULL && freed == 1);
    assert(sector[0] == 0xab);
    assert(!rmtfs_read_sector(&part, 0, sector) && reads == 2 && table[0] == NULL);
    fail_read = false;
    assert(rmtfs_read_sector(&part, 0, sector) && reads == 3 && sector[0] == 0x5a);
    assert(rmtfs_read_sector(&part, 1, sector) && reads == 3);
    memset(sector, 0xcd, sizeof(sector));
    assert(rmtfs_write_sector(&part, 1, sector));
    assert(rmtfs_read_sector(&part, 1, sector) && sector[0] == 0xcd);
    assert(!rmtfs_read_sector(&part, 8, sector));
    kfree_data(table[0], PAGE_SIZE); table[0] = NULL;
    fail_alloc = true;
    assert(!rmtfs_read_sector(&part, 0, sector));
    logs = 0;
    for (unsigned i = 0; i < 100; i++) assert(rmtfs_write_sector(&part, 0, sector));
    assert(logs == 1 && table[0] == NULL);
    now_ms += 10000;
    assert(rmtfs_write_sector(&part, 0, sector) && logs == 2);
    part.ufs_found = false;
    assert(!rmtfs_read_sector(&part, 0, sector));
    puts("review-test: RMTFS read failure, retry, allocation, RAM writes and warning limit PASS");
}
'''
release = common + r'''
#define FRPC_SID 1
#define FRPC_SID_MASK 1
#define os_atomic_load(p, order) __atomic_load_n(p, __ATOMIC_ACQUIRE)
static bool frpc_running, frpc_claimed = true;
static int frpc_smmu = 7;
static unsigned sleeps, finish_after, buffer_frees, smmu_releases;
static void IOSleep(unsigned ms) { (void)ms; if (++sleeps == finish_after) frpc_running = false; }
static void frpc_buffers_free(void) { buffer_frees++; }
static void vayu_smmu_release(int handle) { assert(handle == 7); smmu_releases++; }
'''
release += function(frpc, 'vayu_fastrpc_release')
release += r'''
int main(void)
{
    assert(!vayu_fastrpc_release(false) && buffer_frees == 0 && smmu_releases == 0);
    frpc_running = true;
    assert(!vayu_fastrpc_release(true) && sleeps == 100 && buffer_frees == 0 && smmu_releases == 0);
    sleeps = 0; finish_after = 3;
    assert(vayu_fastrpc_release(true) && sleeps == 3 && buffer_frees == 1 && smmu_releases == 1);
    assert(!frpc_claimed && frpc_smmu == -1);
    puts("review-test: FastRPC failed PAS stop, worker timeout and completed worker PASS");
}
'''
usb = source('vayu_usbnet_ecm.c')
usb_header = (TREE / 'bsd/sys/vayu_usbnet.h').read_text()
controls = common + '\n'.join(line for line in usb_header.splitlines() if line.startswith('#define ')) + '\n'
controls += '\n'.join(line for line in usb.splitlines() if line.startswith('static inline ')) + '\n'
controls += r'''
#define USB_EP_NUMBER_IN 2
#define USB_MAXPACKET_NOTIFY 16
typedef uintptr_t vm_offset_t;
static uint8_t notify_data[16];
static uint32_t notify_phase, notify_rsc, notify_length, enabled;
static unsigned ep_config_calls, fail_endpoint;
static bool usbnet_ncm, ep0_configured, out_running, in_running, data_up, link_up, fail_out;
static uint32_t ep0_alt, in_rsc;
static int usbnet_lock;
static void *usbnet_ifp = (void *)1;
static vm_offset_t vayu_usbnet_dma_va(unsigned offset) { (void)offset; return (vm_offset_t)notify_data; }
static uint64_t vayu_usbnet_dma_pa(unsigned offset) { return offset; }
static void vayu_usbnet_clean(vm_offset_t address, size_t length) { (void)address; (void)length; }
static void vayu_usbnet_trb(unsigned slot, uint64_t pa, uint32_t length, uint32_t flags)
{ (void)slot; (void)pa; (void)flags; notify_length = length; }
static int vayu_usbnet_ep_cmd_rsc(unsigned phys, uint32_t cmd, uint32_t p0, uint32_t p1,
    uint32_t p2, uint32_t *rsc)
{ (void)phys; (void)cmd; (void)p0; (void)p1; (void)p2; *rsc = 1; return 0; }
static uint32_t vayu_usbnet_rd(unsigned reg) { (void)reg; return enabled; }
static void vayu_usbnet_wr(unsigned reg, uint32_t bits) { (void)reg; enabled = bits; }
static int vayu_usbnet_ep_config(unsigned phys, uint32_t type, uint32_t packet, uint32_t fifo,
    uint32_t interval, bool progress, uint32_t action)
{
    (void)phys; (void)type; (void)packet; (void)fifo; (void)interval; (void)progress; (void)action;
    return ++ep_config_calls == fail_endpoint ? -1 : 0;
}
static void lck_mtx_lock(int *lock) { (void)lock; }
static void lck_mtx_unlock(int *lock) { (void)lock; }
static void usbnet_data_stop(void) { data_up = out_running = link_up = false; }
static void usbnet_out_fill(void) { out_running = !fail_out; }
static void in_fill(void) {}
static void usbnet_link(bool up) { link_up = up; }
'''
controls += ''.join(function(usb, name) for name in
                    ['notify_send', 'usbnet_configured', 'usbnet_data_start', 'usbnet_alt'])
controls += r'''
int main(void)
{
    static const uint8_t speed[] = { 0xa1, 0x2a, 0, 0, 0, 0, 8, 0, 0, 0x38, 0x9c, 0x1c, 0, 0x38, 0x9c, 0x1c };
    static const uint8_t connected[] = { 0xa1, 0, 1, 0, 0, 0, 0, 0 };
    uint32_t bits = (1u << USB_EP_NOTIFY) | (1u << USB_EP_OUT) | (1u << USB_EP_IN);
    (void)usbnet_ncm;               /* (this line's driver is ECM only: phase 1 connected, phase 2 the speed) */
    for (unsigned mode = 0; mode < 1; mode++) {
        for (unsigned phase = 1; phase <= 2; phase++) {
            bool is_speed = phase == 2u;
            notify_phase = 0;
            memset(notify_data, 0xa5, sizeof(notify_data));
            notify_send(phase);
            assert(notify_length == (is_speed ? 16u : 8u));
            assert(memcmp(notify_data, is_speed ? speed : connected, notify_length) == 0);
        }
        for (unsigned fail = 1; fail <= 3; fail++) {
            enabled = bits; ep_config_calls = 0; fail_endpoint = fail;
            assert(!usbnet_configured() && (enabled & bits) == 0);
            ep_config_calls = 0; ep0_configured = true; notify_phase = 0;
            assert(!usbnet_alt(1) && !data_up && !link_up && !ep0_configured && ep0_alt == 0);
        }
        ep_config_calls = 0; fail_endpoint = 0; ep0_configured = true;
        assert(usbnet_alt(1) && data_up && link_up && ep0_alt == 1);
        assert(usbnet_alt(0) && !data_up && !link_up && ep0_alt == 0);
        /* an OUT ring that does not start at once is not a failed SET_INTERFACE: XFERNOTREADY starts it later */
        fail_out = true;
        assert(usbnet_alt(1) && data_up && link_up && ep0_alt == 1);
        fail_out = false;
    }
    puts("review-test: ECM notification bytes, endpoint failure and link state PASS");
}
'''
cam = source('vayu_camera.c')
camera = common + declaration(cam, 'struct cam_abi_sensor')
camera += declaration(cam, 'struct cam_frame_out') + declaration(cam, 'struct cam_source')
camera += '\n'.join(line for line in cam.splitlines() if line.startswith((
    'enum { CAM_PLACE_', 'enum { CAM_FRAME_', '#define CAM_FAKE_RING ',
    '#define CAM_FAKE_IDLE_MS ', '#define CAM_REAL_POLL_MS ', '#define RAW10_STRIDE('))) + '\n'
camera += r'''
static int cam_fake_lock;
static bool locked, cam_fake_open = true, cam_fake_broken, cam_fake_streaming, cam_fake_wanted;
static bool cam_fake_failed, cam_fake_exposure_said;
static const char *cam_fake_failed_why;
static uint32_t cam_fake_frame_bytes, cam_fake_readers, cam_fake_generation, cam_fake_failures;
static uint32_t cam_fake_switch_to, g_fake_fps = 30;
static uint64_t cam_fake_frames_done, cam_fake_frames_given, cam_fake_reads, cam_fake_given_sequence;
static uint64_t cam_fake_last_read, time_ms;
static int cam_real_next;
static unsigned starts, frames;
static struct { uint8_t state; uint64_t sequence, time; uint32_t exposure_lines, gain, frame_lines; }
    cam_fake_place[CAM_FAKE_RING];
static struct { uint32_t lines, gain, frame_lines; bool pending; } cam_fake_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } cam_fake_at, cam_fake_next;
static void lck_mtx_lock(int *lock) { (void)lock; assert(!locked); locked = true; }
static void lck_mtx_unlock(int *lock) { (void)lock; assert(locked); locked = false; }
static void wakeup(void *event) { (void)event; }
static void IOSleep(unsigned ms) { time_ms += ms; }
static uint64_t cam_fake_ms(void) { return time_ms; }
static uint64_t mach_absolute_time(void) { return time_ms; }
static void cam_fake_limits(const struct cam_abi_sensor *s, uint32_t *l, uint32_t *g, uint32_t *f)
{ (void)s; (void)l; (void)g; (void)f; }
static bool cam_fake_select_locked(uint32_t sensor, uint32_t mode) { (void)sensor; (void)mode; return false; }
static bool start_camera(const struct cam_abi_sensor *s, uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
    (void)s; (void)lines; (void)gain; (void)frame_lines;
    starts++;
    cam_fake_place[0].state = CAM_PLACE_DMA;
    cam_real_next = 0;
    return true;
}
static int frame_camera(const struct cam_abi_sensor *s, int place, struct cam_frame_out *out)
{
    (void)s;
    if (++frames == 1) {
        assert(place != 0);
        cam_fake_place[0].state = CAM_PLACE_WRITING;
        cam_fake_place[place].state = CAM_PLACE_DMA;
        cam_real_next = place;
        out->place = 0;
        return CAM_FRAME_DONE;
    }
    assert(place != cam_real_next && cam_fake_place[cam_real_next].state == CAM_PLACE_DMA);
    cam_fake_place[0].state = CAM_PLACE_READING;
    cam_fake_readers = 1;
    cam_fake_open = false;
    return CAM_FRAME_STOP;
}
static void stop_camera(void) {}
static const struct cam_source cam_fake_real = {
    .step = "mock", .fixed_exposure = true, .start = start_camera, .frame = frame_camera, .stop = stop_camera
};
'''
camera += function(cam, 'cam_session')
camera += r'''
int main(void)
{
    struct cam_abi_sensor sensor = { .sensor = 1, .name = "synthetic", .width = 8, .height = 1 };
    cam_fake_readers = 1; cam_fake_generation = 9; cam_fake_frame_bytes = 13;
    cam_fake_place[0].state = CAM_PLACE_READING;
    cam_fake_place[0].sequence = 77;
    cam_session(&sensor, &cam_fake_real);
    assert(starts == 0 && time_ms == 1000 && cam_fake_generation == 9 && cam_fake_frame_bytes == 13);
    assert(cam_fake_place[0].state == CAM_PLACE_READING && cam_fake_place[0].sequence == 77);
    cam_fake_readers = 0;
    cam_session(&sensor, &cam_fake_real);
    assert(starts == 1 && frames == 2 && cam_fake_generation == 10);
    assert(cam_fake_place[0].state == CAM_PLACE_READING && cam_fake_readers == 1);
    puts("review-test: camera old-reader timeout, queued DMA reservation and reader preservation PASS");
}
'''
for name, text in [('sms-queue', queue), ('rmtfs-pages', pages), ('fastrpc-release', release),
                   ('usb-controls', controls), ('camera-session', camera)]:
    path = BUILD / (name + '.c')
    path.write_text(text)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        str(path), '-o', str(BUILD / name)], check=True)
    subprocess.run([str(BUILD / name)], check=True)
