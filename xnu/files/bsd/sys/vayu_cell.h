/*
 * Cellular, the first read-only look at the SIM and the network for the POCO X3 Pro (vayu,
 * SM8150-AC, X24): what bsd/dev/vayu_cell.c finds out and the fixed record it leaves for a
 * userland shim to read through /dev/vayu-cell.
 *
 * The client itself is a QMI client on QRTR over the modem's GLINK channel; it asks the modem's
 * UIM and NAS services only (no DMS identifier, no UIM write, no SIM PIN, no data call). The
 * record below is the whole of what leaves the kernel: no IMSI, ICCID or IMEI is in it, only the
 * last digits of an identifier, masked (vayu_cell_mask_tail()).
 *
 * Boot-args (bsd/dev/vayu_cell.c has their meaning):
 *   vayu_cell=0        nothing (the default)
 *   vayu_cell=1        one round: card status, EF_ICCID and EF_IMSI (masked), serving system,
 *                      signal; then the QMI handle is let go and the record stays
 *   vayu_cell=2        one round, and another every vayu_cell_poll seconds
 *   vayu_cell_wait=S   how long to wait for the UIM and NAS services (default 20, at most 120)
 *   vayu_cell_poll=S   seconds between rounds at vayu_cell=2 (default 30, at least 5)
 *   vayu_cell_register=0  1: NAS Initiate Network Register (automatic) may be sent; anything
 *                      else, and it never is (0 is the default: this client asks, it does not act)
 *
 * Record layout: the field offsets are written at their declarations, and `size` is filled with
 * sizeof(struct vayu_cell_record) (152 bytes in the kernel's ABI). Numbers are the machine's
 * (little-endian on arm64); strings are NUL-terminated. `magic` is 'CELL' little-endian and
 * `version` is VAYU_CELL_VERSION; a shim that does not find them should not read the rest.
 */
#ifndef _SYS_VAYU_CELL_H_
#define _SYS_VAYU_CELL_H_

#include <stdint.h>

#define VAYU_CELL_MAGIC          0x43454c4cu     /* "CELL", little-endian: 'C' is the low byte */
#define VAYU_CELL_VERSION        1
#define VAYU_CELL_OPERATOR_MAX   64
#define VAYU_CELL_TAIL_MAX       8               /* "...1234" and the zero */

/* What the SIM is doing, from the card's state and its USIM/SIM application's state (QMI UIM). */
enum {
	VAYU_CELL_SIM_UNKNOWN = 0,      /* no answer from UIM, or no card status TLV */
	VAYU_CELL_SIM_ABSENT = 1,       /* a card slot says absent */
	VAYU_CELL_SIM_PRESENT = 2,      /* a card is there but its application is not ready */
	VAYU_CELL_SIM_PIN_REQUIRED = 3, /* PIN1/UPIN is required; this client never sends one */
	VAYU_CELL_SIM_READY = 4,        /* the application says READY */
	VAYU_CELL_SIM_ERROR = 5,        /* the card says error */
};

/* The network registration, QMI NAS's values (libqmi qmi-enums-nas.h: QmiNasRegistrationState). */
enum {
	VAYU_CELL_REG_NOT_REGISTERED = 0,
	VAYU_CELL_REG_REGISTERED = 1,
	VAYU_CELL_REG_SEARCHING = 2,
	VAYU_CELL_REG_DENIED = 3,
	VAYU_CELL_REG_QMI_UNKNOWN = 4,
	VAYU_CELL_REG_UNKNOWN = 0xff,   /* no serving system TLV */
};

/* What of the record was filled: the modem answered that part. */
#define VAYU_CELL_FLAG_CARD_STATUS      0x00000001u
#define VAYU_CELL_FLAG_SERVING          0x00000002u
#define VAYU_CELL_FLAG_SIGNAL           0x00000004u
#define VAYU_CELL_FLAG_ICCID            0x00000008u
#define VAYU_CELL_FLAG_IMSI             0x00000010u
#define VAYU_CELL_FLAG_OPERATOR         0x00000020u
#define VAYU_CELL_FLAG_LAC_CID          0x00000040u
#define VAYU_CELL_FLAG_DATA_CALL        0x00000200u     /* never set: this client has no data call */

struct vayu_cell_record {
	uint32_t magic;                 /*   0: VAYU_CELL_MAGIC */
	uint32_t version;               /*   4: VAYU_CELL_VERSION */
	uint32_t size;                  /*   8: sizeof(struct vayu_cell_record) */
	uint32_t seq;                   /*  12: raised by one per round; 0: never filled */
	uint64_t updated_ms;            /*  16: uptime of that round, milliseconds */
	uint32_t iccid_length;          /*  24: digits the card has, 0: not read */
	uint32_t imsi_length;           /*  28: digits the card has, 0: not read */
	int32_t  signal_dbm;            /*  32: RSRP (LTE/5G) or RSSI (GSM), dBm; INT32_MIN: none */
	int32_t  rsrq_db;               /*  36: LTE RSRQ, dB; INT32_MIN: none */
	int32_t  snr_tenth_db;          /*  40: LTE/5G SNR in 0.1 dB; INT32_MIN: none */
	uint32_t lac;                   /*  44: 3GPP LAC/TAC; 0xffffffff: none */
	uint32_t cid;                   /*  48: 3GPP cell id; 0xffffffff: none */
	uint16_t mcc;                   /*  52: 0: none */
	uint16_t mnc;                   /*  54: 0: none; the description says how many digits */
	uint8_t  sim_state;             /*  56: VAYU_CELL_SIM_* */
	uint8_t  card_state;            /*  57: QMI UimCardState; 0xff: no answer */
	uint8_t  app_state;             /*  58: QMI UimCardApplicationState; 0xff: none */
	uint8_t  pin_state;             /*  59: QMI UimPinState (UPIN, else PIN1); 0xff: none */
	uint8_t  pin_retries;           /*  60: attempts left, as the card says */
	uint8_t  reg_state;             /*  61: VAYU_CELL_REG_* */
	uint8_t  radio;                 /*  62: QMI NasRadioInterface of the serving system (8: LTE, 0x0c: 5G NR) */
	uint8_t  roam;                  /*  63: 0 home, 1 roaming, 0xff unknown */
	uint32_t flags;                 /*  64: VAYU_CELL_FLAG_* */
	char     operator_name[64];     /*  68: the PLMN description the modem gives, NUL-terminated */
	char     iccid_tail[8];         /* 132: "..." + its last digits, never the whole number */
	char     imsi_tail[8];          /* 140: the same */
	uint32_t reserved;              /* 148: 0 */
};                                      /* 152 bytes: no padding after `reserved` */

/*
 * The last digits of an identifier, with everything before them masked: "...1234". Nothing is
 * shown of an identifier shorter than five digits, nor of one that is not all digits: a tail of
 * a malformed string could be a piece that is not an identifier's end. `out` is VAYU_CELL_TAIL_MAX
 * bytes. Never call this with anything but digits: that is what the callers decode first.
 */
static __inline__ void
vayu_cell_mask_tail(const char *digits, uint32_t count, char out[VAYU_CELL_TAIL_MAX])
{
	uint32_t keep = count >= 8 ? 4 : count >= 5 ? 2 : 0;

	out[0] = '.';
	out[1] = '.';
	out[2] = '.';
	for (uint32_t i = 0; i < keep; i++) {
		out[3 + i] = digits[count - keep + i];
	}
	out[3 + keep] = 0;
}

#ifdef KERNEL
struct vayu_qrtr;

/* Start after the modem's GLINK channel is up and QRTR is attached (vayu_modem.c step_link). */
void vayu_cell_start(struct vayu_qrtr *q);
/* Stop and wait for the client's thread; the record stays readable through /dev/vayu-cell. */
void vayu_cell_stop(void);
#endif

#endif /* _SYS_VAYU_CELL_H_ */
