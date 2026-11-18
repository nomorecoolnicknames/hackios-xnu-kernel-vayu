/*
 * vayu voice: the control surface between bsd/dev/vayu_voice.c and userland.
 *
 * The kernel keeps a QMI VOICE client (service 0x09, version 2) of the modem and
 * exposes it as /dev/vayu-voice with a text protocol: one command per line is
 * written, and the lines that answer it -- plus the asynchronous call events --
 * are read back.  Nothing here talks to CommCenter or to IMS; the modem decides
 * whether a call goes over VoLTE (IMS on the modem) or falls back to CSFB, and
 * this client only asks for the call.
 *
 * What the client does by itself (vayu_call=1, the default when the argument is
 * present): it looks the VOICE service up, registers for indications and reports
 * every call it is told about -- an incoming call is an EVENT line and one kernel
 * log line.  It sends no call command at all.
 *
 * Boot-args read by vayu_voice_start() (bsd/dev/vayu_voice.c has their meaning):
 *   vayu_call=0          nothing at all (the default)
 *   vayu_call=1          indications and status only; `dial`, `answer`, `end`,
 *                         `dtmf` are refused
 *   vayu_call=2          the same, and the call commands are allowed from
 *                         /dev/vayu-voice (still subject to vayu_call_dial)
 *   vayu_call_wait=S     how long to wait for the VOICE service (default 30,
 *                         1..120)
 *   vayu_call_dial=1     permit `dial`; without it every dial is refused.  Even
 *                         with it, only the number in vayu_call_allow may be
 *                         dialled.
 *   vayu_call_allow=N    the one number `dial` accepts (digits, at most 32).
 *                         Default: 0500, the operator's free service number the
 *                         owner approved for the outbound test.  There is no way
 *                         to dial anything else from this device.
 *   vayu_call_poll=S     seconds between Get All Call Info sweeps (default 0:
 *                         indications only; 5..3600 otherwise)
 *   vayu_call_ind_all=1  register every indication group, not just the four the
 *                         client needs (default 0)
 *   vayu_call_ind_invert=1  send the Indication Register flags inverted (0 for
 *                         the groups wanted).  A hedge for the one thing that
 *                         cannot be checked offline: whether 1 means "report" or
 *                         "do not report" on this firmware.  Default 0.
 *   vayu_call_dump=1     print the bytes of every indication TLV.  Default 0:
 *                         the log gets the TLV types and lengths only, because a
 *                         value may hold the other party's number.  vayu_call_dump=2
 *                         prints only the call-list TLV 0x01 (id, state, type,
 *                         direction, mode, multipart, als -- never a number), which
 *                         is what says whether the field order is the expected one.
 *   vayu_call_audio=1    the q6voice path of bsd/dev/vayu_audio.c follows the call
 *                         (sys/vayu_audio.h): CONVERSATION asks for it, its end
 *                         takes it down.  The path mode is vayu_voice=2 for an
 *                         LTE/VoIP call and vayu_voice=3 for a CS one; those are
 *                         the *audio* module's flags, not this client's.
 *   vayu_call_auto=1     the client dials by itself, so a run needs no shell on the
 *                         phone: vayu_call_auto_delay seconds (default 30) after the
 *                         Indication Register was accepted it sends Dial Call for
 *                         vayu_call_allow, waits for CONVERSATION, holds it
 *                         vayu_call_auto_end seconds (default 45) and sends End Call.
 *                         Needs vayu_call=2 and vayu_call_dial=1; no other number
 *                         can be dialled.  Every step is logged with the masked tail.
 *   vayu_call_auto_delay=S  seconds after READY before the auto-dial (5..300, 30).
 *   vayu_call_auto_end=S    seconds in CONVERSATION before the auto-end (10..600, 45);
 *                         also the wait for CONVERSATION before a listed call is ended.
 *
 * Privacy: the kernel log never prints a caller's number -- only its last four
 * digits, "...1234" -- and never prints an IMSI, ICCID or IMEI (this client does
 * not ask for any).  The device carries the full number it was told, because the
 * device is what a phone UI reads.  The dialled number is never printed in full
 * in the log either: only the same masked tail.
 *
 * The text protocol (no ioctls: both sides agree on lines, not on struct sizes):
 *
 *   commands (write, '\n'-separated; the reply is queued as 'OK ...'/'ERR ...'):
 *     help                       the command list
 *     status                     one 'OK status ...' line: service, registration,
 *                                counters
 *     calls                      Get All Call Info (0x002f) now; the reply is
 *                                one 'OK calls=N' line followed by N 'CALL ...'
 *                                lines
 *     info <id>                  Get Call Info (0x0023) for that call
 *     answer <id>                Answer Call (0x0022)      [vayu_call=2]
 *     end <id>                   End Call (0x0021)         [vayu_call=2]
 *     dtmf <id> <digits>         Burst DTMF (0x0028)       [vayu_call=2]
 *     dtmfstart <id> <digit>     Start Continuous DTMF (0x0029) [vayu_call=2]
 *     dtmfstop <id>              Stop Continuous DTMF (0x002a)  [vayu_call=2]
 *     flash <id>                 Send Flash (0x0024)       [vayu_call=2]
 *     dial <number>              Dial Call (0x0020)        [vayu_call=2 and
 *                                vayu_call_dial=1, and <number> must equal
 *                                vayu_call_allow]
 *
 *   events (read; they arrive without a command):
 *     EVENT CALL id=1 state=incoming dir=MT type=voice mode=lte number=...
 *     EVENT INCOMING id=1 number=... mode=lte
 *     EVENT SERVICE present|gone
 *     EVENT READY indications=registered
 *
 *   read() returns whole lines only.  With no line queued it waits up to
 *   VAYU_VOICE_READ_WAIT_MS and then returns EAGAIN, so a reader may poll.
 */
#ifndef _SYS_VAYU_VOICE_H_
#define _SYS_VAYU_VOICE_H_

#include <stdint.h>

#define VAYU_VOICE_DEVICE                "vayu-voice"
#define VAYU_VOICE_MAGIC                 0x564f4931u     /* "VOI1" */

#define VAYU_VOICE_NUMBER_MAX            33      /* 32 digits and the zero */
#define VAYU_VOICE_DIGITS_MAX            33      /* one Burst DTMF request's digits */
#define VAYU_VOICE_CALLS                 8       /* calls in one report */
#define VAYU_VOICE_LINE_MAX              192     /* one protocol line, zero included */
#define VAYU_VOICE_LINES                 32      /* lines queued for the reader */
#define VAYU_VOICE_COMMAND_MAX           160     /* one write() of commands */
#define VAYU_VOICE_READ_WAIT_MS          3000    /* then EAGAIN */

/* QmiVoiceCallState (libqmi qmi-enums-voice.h, values confirmed from libqmi 1.32.2). */
enum {
	VAYU_VOICE_STATE_UNKNOWN = 0x00,
	VAYU_VOICE_STATE_ORIGINATION = 0x01,
	VAYU_VOICE_STATE_INCOMING = 0x02,
	VAYU_VOICE_STATE_CONVERSATION = 0x03,
	VAYU_VOICE_STATE_CC_IN_PROGRESS = 0x04,
	VAYU_VOICE_STATE_ALERTING = 0x05,
	VAYU_VOICE_STATE_HOLD = 0x06,
	VAYU_VOICE_STATE_WAITING = 0x07,
	VAYU_VOICE_STATE_DISCONNECTING = 0x08,
	VAYU_VOICE_STATE_END = 0x09,
	VAYU_VOICE_STATE_SETUP = 0x0a,
};

/* QmiVoiceCallType. */
enum {
	VAYU_VOICE_TYPE_VOICE = 0x00,
	VAYU_VOICE_TYPE_VOICE_IP = 0x02,
	VAYU_VOICE_TYPE_OTAPA = 0x06,
	VAYU_VOICE_TYPE_NON_STD_OTASP = 0x08,
	VAYU_VOICE_TYPE_EMERGENCY = 0x09,
	VAYU_VOICE_TYPE_SUPS = 0x0a,
};

/* QmiVoiceCallDirection. */
enum {
	VAYU_VOICE_DIRECTION_UNKNOWN = 0x00,
	VAYU_VOICE_DIRECTION_MO = 0x01,
	VAYU_VOICE_DIRECTION_MT = 0x02,
};

/* QmiVoiceCallMode. */
enum {
	VAYU_VOICE_MODE_UNKNOWN = 0x00,
	VAYU_VOICE_MODE_CDMA = 0x01,
	VAYU_VOICE_MODE_GSM = 0x02,
	VAYU_VOICE_MODE_UMTS = 0x03,
	VAYU_VOICE_MODE_LTE = 0x04,
};

/* What the client is doing (the `status` line's service= field). */
enum {
	VAYU_VOICE_OFF = 0,
	VAYU_VOICE_WAITING = 1,         /* no VOICE service yet */
	VAYU_VOICE_CONFIGURING = 2,     /* the service is there, the register is in flight */
	VAYU_VOICE_READY = 3,           /* indications registered */
	VAYU_VOICE_FAILED = 4,          /* the register was refused */
};

struct vayu_voice_call {
	uint8_t id;
	uint8_t state;
	uint8_t type;
	uint8_t direction;
	uint8_t mode;
	uint8_t multipart;
	uint8_t als;
	uint8_t entry_size;                     /* 7 or 8: the layout that decoded */
	char number[VAYU_VOICE_NUMBER_MAX];     /* digits as the modem sent them; "" if unknown */
};

struct vayu_voice_calls {
	uint32_t count;
	struct vayu_voice_call call[VAYU_VOICE_CALLS];
};

/* A snapshot for `status`; no pointers, so the reader may take it as it is. */
struct vayu_voice_status {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t state;
	uint32_t present;                       /* the VOICE service is there */
	uint32_t registered;                    /* Indication Register was accepted */
	uint32_t indications;                   /* datagrams the modem sent us */
	uint32_t unknown_indications;           /* ... whose shape we did not know */
	uint32_t calls;                         /* calls in the last report */
	uint32_t incoming;                      /* incoming calls seen */
	uint32_t requests;
	uint32_t failures;
	uint32_t dial_allowed;                  /* vayu_call_dial=1 */
	uint32_t commands_allowed;              /* vayu_call=2 */
	uint32_t last_result;
	uint32_t last_error;
	uint32_t last_message;
};

#endif /* _SYS_VAYU_VOICE_H_ */
