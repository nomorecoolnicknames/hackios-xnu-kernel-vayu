/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Origin: the iOS 16.5 line's NBD branch (d0e149d) bsd/sys/vayu_ctl.h (the 8796 line); the mailbox and messages unchanged (the
 * loader's and usbctld's copies must stay the same), the input registration below is new on 12377.
 *
 * usbctl: control of a running iOS over the USB cable the loader is already on.
 *
 * Two channels meet here.
 *
 *   host  --fastboot "oem touch|swipe|key|sh|pull"-->  xnuloader (core 0, UEFI USB)
 *   xnuloader  --this mailbox, in kernel memory-->  vayu-ctl thread (kernel)
 *   vayu-ctl thread  --/dev/vayu-ctl-->  /redwood/bin/usbctld (a launchd job, root)
 *
 * The loader keeps the USB controller while iOS runs (that is how "oem log" and "oem
 * screenshot" work in the middle of a run), so the kernel cannot own the port; the loader
 * puts one request at a time into struct vayu_ctl_mbox and waits for the kernel to mark it
 * done.  Commands that must run inside iOS -- a shell command, a file read -- are handed to
 * usbctld through /dev/vayu-ctl; touches and buttons go to the input driver that registered
 * itself with vayu_ctl_set_input() (below; none: VAYU_CTL_ERR_NOINPUT).
 *
 * This header is shared three ways: the kernel (bsd/dev/vayu_ctl.c), the userland agent
 * (userland/src/usbctld.c) and, field for field, the loader's own copy of the mailbox
 * (xnuloader/loader.c, struct vayu_ctl_mbox).  The build copies it to userland/src and
 * checks that the two are identical.
 */
#ifndef _SYS_VAYU_CTL_H_
#define _SYS_VAYU_CTL_H_

#include <stdint.h>

#define VAYU_CTL_DEVICE         "/dev/vayu-ctl"

/* The mailbox the loader writes into and the kernel answers in (kernel memory, physical). */
#define VAYU_CTL_MBOX_MAGIC     0x314c544355564156ULL  /* "VAVUCTL1" */

/* What the loader asks for. */
#define VAYU_CTL_CMD_NONE       0
#define VAYU_CTL_CMD_TOUCH      1       /* arg[0]=x arg[1]=y, panel 0..65535 */
#define VAYU_CTL_CMD_SWIPE      2       /* arg[0..3]=x0 y0 x1 y1, arg[4]=ms, panel 0..65535 */
#define VAYU_CTL_CMD_KEY        3       /* arg[0]=VAYU_CTL_KEY_* */
#define VAYU_CTL_CMD_SH         4       /* text = shell command */
#define VAYU_CTL_CMD_PULL       5       /* text = path, arg[0]=offset, arg[1]=max bytes */
#define VAYU_CTL_CMD_STATUS     6       /* no arguments: is the agent there? */

/* VAYU_CTL_CMD_KEY. */
#define VAYU_CTL_KEY_HOME       1       /* the home gesture (a swipe up from the bottom edge) */
#define VAYU_CTL_KEY_POWER      2       /* the side button (HAKIOS_BUTTON_LOCK) */
#define VAYU_CTL_KEY_VOLUP      3
#define VAYU_CTL_KEY_VOLDOWN    4
#define VAYU_CTL_KEY_BHOME      5       /* the HID menu usage, "touch button home" */

/* Mailbox states. */
#define VAYU_CTL_ST_IDLE        0
#define VAYU_CTL_ST_REQ         1       /* the loader has a request for the kernel */
#define VAYU_CTL_ST_BUSY        2       /* the kernel has taken it */
#define VAYU_CTL_ST_DONE        3       /* the answer is in, see code/data_len */

/* Result codes (mbox->code).  Positive values are a shell command's exit status. */
#define VAYU_CTL_OK             0
#define VAYU_CTL_ERR_ARGS       (-1)
#define VAYU_CTL_ERR_NOAGENT    (-2)     /* usbctld is not running (or not reading the device) */
#define VAYU_CTL_ERR_AGENT      (-3)     /* the agent could not do it; see mbox->flags/errno */
#define VAYU_CTL_ERR_TIMEOUT    (-4)
#define VAYU_CTL_ERR_BUSY       (-5)
#define VAYU_CTL_ERR_NOWINDOW   (-6)
#define VAYU_CTL_ERR_NOINPUT    (-7)     /* touch/swipe/key: no input driver registered (12377) */

/* mbox->flags */
#define VAYU_CTL_F_TRUNCATED    0x1     /* the output did not fit (shell output: 64 KB) */
#define VAYU_CTL_F_LAST         0x2     /* pull: this is the last chunk */

#define VAYU_CTL_SH_MAX         (64 * 1024)
#define VAYU_CTL_PULL_MAX       (4 * 1024 * 1024)
#define VAYU_CTL_TEXT_MAX       512

/*
 * One request/answer.  Written by the loader (seq, cmd, arg[], text), taken by the kernel
 * (state -> BUSY), answered by the kernel (code, data_pa/data_len, total, flags), read by
 * the loader, then state is set back to IDLE by the loader.  seq is the generation: the
 * kernel only answers the request whose seq it took, so a host that gave up (timeout) and a
 * new request cannot be answered with the old result.
 */
struct vayu_ctl_mbox {
	uint64_t magic;                 /* VAYU_CTL_MBOX_MAGIC */
	volatile uint64_t seq;          /* the loader: ++ for every request */
	volatile uint64_t state;        /* VAYU_CTL_ST_* */
	volatile uint64_t done_seq;     /* the kernel: the seq whose answer is in, and only that one */
	volatile uint64_t cmd;          /* VAYU_CTL_CMD_* */
	volatile uint64_t arg[5];
	volatile uint64_t code;         /* result, see VAYU_CTL_OK/ERR_* */
	volatile uint64_t data_pa;      /* physical address of the answer's bytes */
	volatile uint64_t data_len;     /* how many bytes are valid there */
	volatile uint64_t total;        /* pull: the whole file's size */
	volatile uint64_t flags;        /* VAYU_CTL_F_* */
	char text[VAYU_CTL_TEXT_MAX];   /* the command line or the path */
};

/* ------------------------------------------------------------------------------------ */
/* /dev/vayu-ctl: the kernel's queue to usbctld.  One command in flight at a time.       */

#define VAYU_CTL_MSG_MAGIC      0x31474d5343545641ULL  /* "AVCTMSG1" */

#define VAYU_CTL_MSG_CMD        1       /* kernel -> agent: cmd/arg[]/text, answer by id */
#define VAYU_CTL_MSG_OUT        2       /* agent -> kernel: shell output bytes */
#define VAYU_CTL_MSG_DONE       3       /* agent -> kernel: done; code = exit status */
#define VAYU_CTL_MSG_PULLDATA   4       /* agent -> kernel: file bytes at cmd (absolute offset) */
#define VAYU_CTL_MSG_PULLDONE   5       /* agent -> kernel: done; arg[0] = file size, code = errno */
#define VAYU_CTL_MSG_HELLO      6       /* agent -> kernel: usbctld has the device open */

struct vayu_ctl_msg {
	uint64_t magic;                 /* VAYU_CTL_MSG_MAGIC */
	uint32_t type;                  /* VAYU_CTL_MSG_* */
	uint32_t flags;
	uint64_t id;                    /* the job this answers */
	int64_t  code;                  /* DONE/PULLDONE: exit status or errno (0 ok) */
	uint64_t cmd;                   /* CMD: VAYU_CTL_CMD_*; PULLDATA: absolute file offset */
	uint64_t arg[6];                /* CMD: the mailbox's arg[] */
	uint32_t len;                   /* payload bytes that follow this header */
	uint32_t reserved;
};

#if defined(KERNEL_PRIVATE) || defined(XNU_KERNEL_PRIVATE)
/*
 * The kernel's side only. An input driver (the touch port) gives vayu-ctl its synthetic events: post(type, which,
 * x, y) with the HAKIOS_TOUCH_DOWN/MOVE/UP (1/2/3, x and y 0..65535 of the panel) and HAKIOS_BUTTON_DOWN/UP (4/5,
 * which = HAKIOS_BUTTON_*: 1 lock, 2 volume up, 3 volume down, 4 home) of Scion's sys/hakios.input.h, and the
 * panel's size in pixels. Registered once, from the driver's start.
 */
struct vayu_ctl_input {
	void (*post)(uint32_t type, uint32_t which, uint32_t x, uint32_t y);
	void (*panel_size)(uint32_t *width, uint32_t *height);
};
void vayu_ctl_set_input(const struct vayu_ctl_input *input);
void vayu_ctl_init(void);
#endif

#endif /* _SYS_VAYU_CTL_H_ */
