/*
 * A remote processor of the POCO X3 Pro (vayu, SM8150-AC) loaded through TrustZone's PAS and
 * watched over SMP2P: bsd/dev/vayu_pas.c. One description per processor (the audio DSP's is in
 * vayu_audio.c); the shared memory, the APCS doorbells, the mailbox to the always-on processor
 * and RPMh are the SoC's and are shared by all of them.
 */
#ifndef _SYS_VAYU_PAS_H_
#define _SYS_VAYU_PAS_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stdint.h>
#include <kern/locks.h>
#include <sys/hakios.qrtr.h>

struct vayu_glink;

#define VAYU_PAS_MAX_SEGMENTS   48
#define VAYU_SMEM_HOST_APPS     0
#define VAYU_SMEM_HOST_GLOBAL   0xfffe

struct vayu_rproc {
	/* what the device tree says of it */
	const char *name;               /* for the log, and the always-on processor's "load_state" name */
	const char *firmware;           /* "adsp": adsp.mdt and adsp.bNN from fw/ of the small image or /fw of the root */
	uint32_t pas_id;                /* qcom,pas-id */
	uint32_t memory, memory_size;   /* its reserved memory, which the image must fill from the start */
	uint16_t smem_host;             /* qcom,remote-pid */
	uint32_t smp2p_theirs, smp2p_ours;      /* qcom,smem of its qcom,smp2p-*: <theirs ours> */
	uint32_t crash_item;            /* qcom,smem-id */
	const char *smp2p_extra;        /* a second outgoing SMP2P entry from the DT (the modem: "ipa"); only with vayu_smp2p_extra=1 */
	unsigned apcs_smp2p;            /* the bit of APCS IPC (0x17c0000c) that rings its SMP2P */
	const char *rails[2];           /* RPMh resources asked for while it boots (NULL: none) */
	uint32_t rail_level;
	bool rails_shared;              /* true (cx.lvl, mx.lvl): only ever raised, never given back -- the
	                                 * application processors' one vote for it is what UEFI, the display
	                                 * and UFS stand on; a rail of its own (mss.lvl) may be given back */

	/* what vayu_pas.c keeps of it */
	struct {
		const uint8_t *header;
		uint64_t header_size;
		struct {
			uint32_t at, file_size, memory_size;
			const uint8_t *file;
			uint64_t file_alloc;    /* not 0: the file was read into memory of ours, of this size */
		} segment[VAYU_PAS_MAX_SEGMENTS];
		unsigned segments;
		uint32_t size;
		uint64_t header_alloc;
		bool written;
	} image;
	uint32_t our_item;
	struct vayu_glink *glink;       /* its GLINK edge, if one was prepared: taken down before it is stopped */
	bool started, rails_asked, stopped_by_its_word;
	bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;
};

/* The bits of its SMP2P entry "slave-kernel" (qcom_q6v5.c). */
#define VAYU_RPROC_FATAL        (1u << 0)
#define VAYU_RPROC_READY        (1u << 1)
#define VAYU_RPROC_HANDOVER     (1u << 2)
#define VAYU_RPROC_STOP_ACK     (1u << 3)

/* The SoC's (true if the shared memory and RPMh's command database are where vayu has them). */
bool vayu_soc_check(const char *who);
/* A new item of that size in our partition with that host (zeroed), as Linux's qcom_smem_alloc(); one already there is kept. */
uint32_t vayu_smem_alloc(uint16_t host, uint32_t item, uint32_t size, const char *who);
uint32_t vayu_smem_item(uint16_t host, uint32_t item, uint32_t *size);  /* the item's offset in the shared memory; 0: not there */
bool vayu_smem_peek(uint64_t offset, void *to, uint32_t size);
void vayu_smem_census_all(const char *who);      /* every partition of the table of contents, sizes, first bytes (masked) and text */
void vayu_smem_census(const char *who, uint16_t host);   /* every item of partitions 0+host and global, sizes and text (reads only) */
uint32_t vayu_smem_word(uint32_t offset);
void vayu_smem_poke(uint32_t offset, uint32_t value);
void vayu_apcs_ring(unsigned bit);      /* one bit of APCS IPC: never read-modify-write */
bool vayu_aop_send(const char *text);   /* a message to the always-on processor (QMP) */
bool vayu_rpmh_vote(const char *const names[], unsigned count, uint32_t level); /* level 0: given back */
uint32_t vayu_rpmh_address(const char *name);   /* cmd-db address of a resource id; 0: not there */
bool vayu_rpmh_tcs_lock(void);           /* TCS 0 of the apps RSC, one writer at a time; false: not taken (on the way out) */
void vayu_rpmh_tcs_unlock(bool held);
bool vayu_rpmh_send(const uint32_t addresses[], const uint32_t values[], unsigned count, const char *who); /* raw RPMh active-only writes through TCS 0, waited; true: RPMh said done */
void vayu_pas_set_pause(uint32_t ms);   /* before each first-time write: time for the loader to note the log (default 2200) */
/* TrustZone MEM_PROT_ASSIGN for a contiguous buffer: its pages are given to the modem's VMID 15
 * read/write while HLOS (3) keeps them read/write. 0 on success, or the SMC's error. */
int vayu_scm_assign_shared(uint64_t physical, uint64_t size, uint32_t vmid);
/* Linux ath10k-shaped MSA assignment: src HLOS -> 15, 0x18[, 0x19]; before MSA_READY. */
int vayu_scm_assign_msa(uint64_t physical, uint64_t size, bool secure);

/* One remote processor. */
bool vayu_pas_supported(struct vayu_rproc *rp); /* TrustZone answers as it answers Linux, and says it authenticates rp->pas_id */
void vayu_pas_survey(struct vayu_rproc *rp);    /* reads only: has it run, its last words, GLINK's items */
bool vayu_pas_load(struct vayu_rproc *rp);      /* the image checked, a running one stopped, TrustZone given the header, written */
bool vayu_pas_start(struct vayu_rproc *rp, uint32_t wait_ms);   /* our SMP2P item, rails, AOP, authenticated and out of reset, "up" */
uint32_t vayu_pas_state(struct vayu_rproc *rp); /* its "slave-kernel" bits now; 0: no item */
bool vayu_pas_stop(struct vayu_rproc *rp, const char *why);     /* asked to stop, shut down by TrustZone, AOP and rails off */
void vayu_pas_quiesce(struct vayu_rproc *rp);   /* the same on a panic's way back to the loader: no locks, no sleep */

/*
 * GLINK over the shared memory with one remote processor (Linux's qcom_glink_smem.c and
 * qcom_glink_native.c): items 478 (descriptor) and 479 (our queue) made by us in our partition with
 * it, 480 (its queue) by it; its doorbell one APCS bit. The channels we answer when it opens them,
 * each with the intents we give it and a function for what arrives (called on the edge's thread,
 * never with a lock held). Several edges may be up at once.
 */
#define VAYU_GLINK_CHANNELS     4

struct vayu_glink_packet {
	struct vayu_glink_packet *next;
	uint32_t length;
	uint8_t data[];
};
typedef void (*vayu_glink_input_t)(void *context, const uint8_t *data, uint32_t length);

struct vayu_glink_channel {
	const char *name;
	struct {
		uint32_t size, count;
	} intents[3];
	vayu_glink_input_t input;
	void *context;

	/* kept by vayu_pas.c */
	uint32_t ours, theirs;          /* our number for it (its place + 1); its number, 0 until it opens it */
	bool asked, acknowledged, open, request_answered, request_granted;
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];
	uint32_t our_intent_count;
	struct vayu_glink_packet *first, *last;
};

struct vayu_glink {
	struct vayu_rproc *rp;
	unsigned apcs_glink;            /* dt: mboxes of its glink node (adsp: 8) */
	uint32_t fifo_size;             /* our queue's size (Linux: 16 KB) */
	struct vayu_glink_channel channel[VAYU_GLINK_CHANNELS];
	unsigned channels;
	void (*tick)(void *context);    /* if set: called by the edge's thread once per loop (about every ms) */
	void *tick_context;

	/* kept by vayu_pas.c */
	lck_mtx_t lock;
	bool lock_ready;
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken, agreed;
	uint32_t features, said, heard, trace;
	char others[4][33];             /* channels it opened that nobody here answers (for the log) */
	volatile bool thread_wanted, thread_runs;
};

bool vayu_glink_prepare(struct vayu_glink *g);  /* its items made (before the processor starts) */
bool vayu_glink_up(struct vayu_glink *g, uint32_t wait_ms);     /* versions, the thread; true when channel[0] is open */
int vayu_glink_send(struct vayu_glink *g, struct vayu_glink_channel *channel, const void *data, uint32_t length);
void vayu_glink_down(struct vayu_glink *g);

/* QRTR on one GLINK channel (IPCRTR): hello answered, the services it tells of written down, and those of ours told
 * to it; ports of our own node, which is what the QMI helpers speak through (sys/hakios.qrtr.h). */
struct vayu_qrtr {
	struct vayu_glink *glink;
	struct vayu_glink_channel *channel;
	struct {
		uint32_t service, instance, node, port;
		bool there;
	} servers[64];
	uint32_t peer, in, out, hellos, lookups;
	bool peer_known;
	thread_t link_thread;           /* the GLINK thread that calls vayu_qrtr_input (for close from inside a callback) */
	lck_mtx_t calling;              /* held by the link thread around a port's callback */
	bool lock_ready;
};

void vayu_qrtr_input(void *context, const uint8_t *data, uint32_t length);   /* the channel's input; context: the vayu_qrtr */
unsigned vayu_qrtr_services(const struct vayu_qrtr *q);
bool vayu_qrtr_find(const struct vayu_qrtr *q, uint32_t service, uint32_t *node, uint32_t *port);

/* q becomes one of the routers (up to four) behind the QMI helpers (hakios_qmi_set_transport); ports opened after
 * this belong to it. Our node, its ports and published services are shared by all attached routers; a datagram goes
 * through the router behind its destination node. */
void vayu_qrtr_attach(struct vayu_qrtr *q);
/* q's ports closed (each published service withdrawn with DEL_SERVER); the QMI helpers' transport taken away with the last router. */
void vayu_qrtr_detach(struct vayu_qrtr *q);
/* The four calls (open/send/lookup/close) on the attached router: for servers of our own as well. */
extern const struct hakios_qrtr_ops vayu_qrtr_ops;
/* The service is OURS at that port: NEW_SERVER to the peer now if HELLO was exchanged, and again after each later
 * HELLO; DEL_SERVER when the port is closed. 0 or an errno. At most 8 published services. */
int vayu_qrtr_publish(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance);
uint32_t vayu_qrtr_port_number(const struct hakios_qrtr_port *port);  /* for the log; 0 if none */
#endif

#endif /* _SYS_VAYU_PAS_H_ */
