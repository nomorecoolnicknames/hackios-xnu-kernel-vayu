/*
 * vayu SMS: the control surface between bsd/dev/vayu_sms.c and userland.
 *
 * The kernel keeps a QMI WMS client of the modem (service 0x05) and a small inbox of
 * the messages it has decoded.  /dev/vayu-sms exposes that inbox and the two
 * actions this first stage has: send one short message and delete one stored one.
 * Nothing here talks to CommCenter: the kernel device is the only transport, and a
 * userland tool (work/hackios-vayu/userland/src/sms.c, /redwood/bin/sms) is its
 * first user.
 *
 * Boot-args read by vayu_sms_start():
 *   vayu_sms=0          nothing at all (the default)
 *   vayu_sms=1          the WMS client, /dev/vayu-sms and the inbox
 *   vayu_sms_wait=S     how long to wait for the WMS service (default 30, 1..120)
 *   vayu_sms_routes=1   permit Set Routes 0x0032, and only when Get Routes returns an
 *                       empty table: point-to-point class 0 and 1, UIM storage,
 *                       STORE_AND_NOTIFY.  Default 0: the table is only read.  NV is
 *                       never requested.  An existing table is never rewritten.
 *   vayu_sms_store=N    storage enumerated at startup and by `sms refresh`:
 *                       0 UIM (default), 1 NV
 *   vayu_sms_refresh=S  seconds between automatic List Messages sweeps (default 60,
 *                       0 disables, at least 15 otherwise)
 *
 * Privacy: the kernel log masks a sender number to its last four digits, never
 * prints message text or a PDU byte, and never prints an identifier.  The inbox
 * carries the full number and text because that is what the user asked to see; the
 * device is 0666 on a research image.
 *
 * ioctl commands are plain numbers, not _IOWR(): the userland tool is built without
 * the kernel headers and both sides must agree on one value, not on a structure's
 * size.  The structures below are the ABI; their fields are fixed-width and no
 * pointers are passed.
 */
#ifndef _SYS_VAYU_SMS_H_
#define _SYS_VAYU_SMS_H_

#include <stdint.h>

#define VAYU_SMS_MAGIC          0x534d5331u     /* "SMS1" little-endian */
#define VAYU_SMS_VERSION        1
#define VAYU_SMS_DEVICE         "vayu-sms"

#define VAYU_SMS_NUMBER_MAX     32
#define VAYU_SMS_TEXT_MAX       640
#define VAYU_SMS_TIMESTAMP_MAX  26
#define VAYU_SMS_INBOX          16

/* vayu_sms_message.flags */
#define VAYU_SMS_F_ALPHANUMERIC 0x00000001u     /* the sender is a name, not a number */
#define VAYU_SMS_F_MULTIPART    0x00000002u     /* a UDH concatenation header was present */
#define VAYU_SMS_F_UCS2         0x00000004u     /* DCS said UCS-2 */
#define VAYU_SMS_F_8BIT         0x00000008u     /* DCS said 8-bit data; text is a byte view */

/* vayu_sms_message.alphabet */
#define VAYU_SMS_ALPHABET_GSM7  0
#define VAYU_SMS_ALPHABET_8BIT  1
#define VAYU_SMS_ALPHABET_UCS2  2
#define VAYU_SMS_ALPHABET_NONE  0xff

/* QmiWmsMessageTagType, the two the inbox cares for. */
#define VAYU_SMS_TAG_MT_READ      0
#define VAYU_SMS_TAG_MT_NOT_READ  1

/* The state of the WMS client, as `sms status` prints it. */
#define VAYU_SMS_STATE_OFF          0
#define VAYU_SMS_STATE_WAITING      1
#define VAYU_SMS_STATE_CONFIGURING  2
#define VAYU_SMS_STATE_READY        3
#define VAYU_SMS_STATE_FAILED       4

struct vayu_sms_message {
	uint32_t seq;                   /* inbox sequence, never 0; Delete and dedupe key */
	uint32_t storage;               /* QmiWmsStorageType (0 UIM, 1 NV, 0xff unknown) */
	uint32_t index;                 /* memory index inside that storage */
	uint32_t tag;                   /* QmiWmsMessageTagType (0 MT read, 1 MT not read) */
	uint32_t flags;                 /* VAYU_SMS_F_* */
	uint32_t alphabet;              /* VAYU_SMS_ALPHABET_* */
	uint32_t text_length;           /* bytes in text, UTF-8 (or raw 8-bit view) */
	uint32_t number_length;         /* bytes in number */
	uint64_t received_ms;           /* kernel uptime when it was decoded */
	char     timestamp[VAYU_SMS_TIMESTAMP_MAX];  /* "2026-11-02 06:30:00 +0300" from TP-SCTS */
	char     number[VAYU_SMS_NUMBER_MAX];        /* full number, NUL-terminated */
	char     text[VAYU_SMS_TEXT_MAX];            /* UTF-8, NUL-terminated */
};

/*
 * The inbox is read in two steps: COUNT says how many and which seq the next one
 * will get, then GET fetches one message by its seq.  (A whole inbox in one ioctl
 * would exceed IOCPARM_MAX, 8 KiB.)  The tool walks seq 1 .. next_seq-1 and skips
 * the ENOENT answers: at most VAYU_SMS_INBOX of them are there.
 */
struct vayu_sms_count {
	uint32_t count;                 /* messages in the inbox */
	uint32_t capacity;              /* VAYU_SMS_INBOX */
	uint32_t unread;                /* tag == MT_NOT_READ */
	uint32_t next_seq;              /* the seq the next message will get */
};

struct vayu_sms_get {
	uint32_t seq;                   /* in: the seq to fetch */
	struct vayu_sms_message message;/* out */
};

struct vayu_sms_send {
	char     number[VAYU_SMS_NUMBER_MAX];   /* in: digits, optional leading '+' */
	uint32_t text_length;                   /* in: bytes of text, 1..VAYU_SMS_TEXT_MAX-1 */
	uint32_t message_id;                    /* out: QMI message id */
	uint32_t message_reference;             /* out: TP-MR used */
	uint32_t pdu_length;                    /* out: bytes of the SMS-SUBMIT PDU */
	char     text[VAYU_SMS_TEXT_MAX];       /* in: UTF-8 */
};

struct vayu_sms_delete {
	uint32_t seq;                   /* in: the inbox sequence to delete */
};

struct vayu_sms_status {
	uint32_t magic;                 /* VAYU_SMS_MAGIC */
	uint32_t version;               /* VAYU_SMS_VERSION */
	uint32_t size;                  /* sizeof(struct vayu_sms_status) */
	uint32_t state;                 /* VAYU_SMS_STATE_* */
	uint32_t wms_present;
	uint32_t event_report;
	uint32_t routes_known;
	uint32_t routes_written;
	uint32_t routes_allowed;
	uint32_t route_count;
	uint32_t store;                 /* vayu_sms_store */
	uint32_t inbox_count;
	uint32_t received;
	uint32_t sent;
	uint32_t deleted;
	uint32_t dropped;
	uint32_t requests;
	uint32_t failures;
	uint32_t last_result;
	uint32_t last_error;
	uint32_t last_message;
	uint32_t last_gsm_cause;        /* RP cause of the last Raw Send failure, 0xffff none */
	uint32_t last_failure;          /* local errno of the last failed request */
};

/*
 * ioctl numbers, encoded the way XNU's <sys/ioccom.h> does (_IOWR: 0xc0000000,
 * the structure's size in bits 16..28, group in 8..15, number in 0..7).  The
 * macro is written out here so that a userland program built without the kernel
 * headers gets exactly the same numbers; the structures' layouts are the ABI.
 */
#define VAYU_SMS_IOC_NUMBER(group, number, type) \
	((uint32_t)0xc0000000u | (((uint32_t)sizeof(type) & 0x1fffu) << 16) | \
	 ((uint32_t)(group) << 8) | (uint32_t)(number))
#define VAYU_SMS_IOC_COUNT     VAYU_SMS_IOC_NUMBER('S', 1, struct vayu_sms_count)
#define VAYU_SMS_IOC_GET       VAYU_SMS_IOC_NUMBER('S', 2, struct vayu_sms_get)
#define VAYU_SMS_IOC_SEND      VAYU_SMS_IOC_NUMBER('S', 3, struct vayu_sms_send)
#define VAYU_SMS_IOC_DELETE    VAYU_SMS_IOC_NUMBER('S', 4, struct vayu_sms_delete)
#define VAYU_SMS_IOC_STATUS    VAYU_SMS_IOC_NUMBER('S', 5, struct vayu_sms_status)
#define VAYU_SMS_IOC_REFRESH   VAYU_SMS_IOC_NUMBER('S', 6, uint32_t)

#ifdef KERNEL
struct vayu_qrtr;

/*
 * Start the client's thread after the modem's QRTR transport is registered.  It does
 * nothing unless vayu_sms=1.  Returns 0 when the thread is running (or was already),
 * -EBUSY while an earlier stop is still completing, or a negative errno.
 */
int vayu_sms_start(struct vayu_qrtr *q);
/* Ask the thread to stop and wait a bounded time; /dev/vayu-sms stays readable. */
void vayu_sms_stop(void);
#endif /* KERNEL */

#endif /* _SYS_VAYU_SMS_H_ */
