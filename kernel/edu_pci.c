// kernel/edu_pci.c
// Phase 1 complete: SQ/CQ ring buffer, eventfd, async submit/poll ioctls,
// mmap for zero-copy userspace ring access. All legacy ioctls preserved.

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/eventfd.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#define DRV_NAME "edu_pci"

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/* --- EDU MMIO register map --- */
#define EDU_REG_ID         0x00
#define EDU_REG_LIVE       0x04
#define EDU_REG_FACT       0x08
#define EDU_REG_STATUS     0x20
#define EDU_REG_IRQ_STAT   0x24
#define EDU_REG_IRQ_RAISE  0x60
#define EDU_REG_IRQ_ACK    0x64
#define EDU_REG_DMA_SRC    0x80
#define EDU_REG_DMA_DST    0x88
#define EDU_REG_DMA_CNT    0x90
#define EDU_REG_DMA_CMD    0x98

#define EDU_STATUS_BUSY    0x01
#define EDU_STATUS_IRQ_EN  0x80

#define EDU_DMA_BUF_OFFSET 0x40000   /* EDU device-internal buffer PA */

#define EDU_DMA_CMD_START  0x01
#define EDU_DMA_CMD_DIR    0x02       /* 0=RAM->EDU, 1=EDU->RAM */
#define EDU_DMA_CMD_IRQ    0x04

/* -------------------------------------------------------
 * Ring layout — single dma_alloc_coherent slab:
 *
 *   [0 .. SQ_SIZE-1]          SQ entries
 *   [SQ_SIZE .. SQ_SIZE+CQ_SIZE-1]   CQ entries
 *
 * RING_SIZE must be a power of 2.
 * ------------------------------------------------------- */
#define RING_SIZE  64
#define RING_MASK  (RING_SIZE - 1)

/* Opcodes */
#define EDU_OP_FACTORIAL  0x01
#define EDU_OP_DMA_TEST   0x02

/* Submission Queue entry – 16 bytes */
struct sq_entry {
	__u16 tag;      /* caller-assigned sequence number */
	__u8  opcode;   /* EDU_OP_*                        */
	__u8  flags;    /* reserved, must be 0             */
	__u32 operand;  /* e.g. factorial input            */
	__u64 rsvd;
} __packed;

/* Completion Queue entry – 16 bytes */
struct cq_entry {
	__u16 tag;      /* echoes sq_entry.tag             */
	__u8  status;   /* 0 = success                     */
	__u8  flags;    /* reserved                        */
	__u32 result;   /* e.g. factorial output           */
	__u64 rsvd;
} __packed;

#define SQ_SIZE        (RING_SIZE * sizeof(struct sq_entry))  /* 1024 B */
#define CQ_SIZE        (RING_SIZE * sizeof(struct cq_entry))  /* 1024 B */
#define DMA_BUF_TOTAL  PAGE_ALIGN(SQ_SIZE + CQ_SIZE)          /* 4096 B — page aligned */

/* Legacy DMA test payload length */
#define EDU_DMA_TEST_LEN  64

/* -------------------------------------------------------
 * IOCTL definitions
 * ------------------------------------------------------- */
#define EDU_IOC_MAGIC 'E'

/* Legacy structs (unchanged) */
struct edu_fact_req {
	__u32 input;
	__u32 output;
	__u32 irq_status;
};

struct edu_state_req {
	__u32 last_irq;
	__u32 last_fact_in;
	__u32 last_fact_out;
	__u32 dma_ok;
};

/* New ring structs */
struct edu_submit_req {
	__u16 tag;      /* caller picks a unique tag */
	__u8  opcode;   /* EDU_OP_*                  */
	__u8  pad;
	__u32 operand;
};

struct edu_completion_req {
	__u16 tag;      /* OUT: completed tag   */
	__u8  status;   /* OUT: 0 = success     */
	__u8  pad;
	__u32 result;   /* OUT: result value    */
};

/* Legacy ioctls */
#define EDU_IOC_RUN_FACTORIAL  _IOWR(EDU_IOC_MAGIC, 1, struct edu_fact_req)
#define EDU_IOC_RUN_DMA_TEST   _IO  (EDU_IOC_MAGIC, 2)
#define EDU_IOC_GET_STATE      _IOR (EDU_IOC_MAGIC, 3, struct edu_state_req)

/* New ring ioctls */
#define EDU_IOC_SUBMIT         _IOW (EDU_IOC_MAGIC, 4, struct edu_submit_req)
#define EDU_IOC_POLL_CQ        _IOR (EDU_IOC_MAGIC, 5, struct edu_completion_req)
#define EDU_IOC_SET_EVENTFD    _IOW (EDU_IOC_MAGIC, 6, int)

/* -------------------------------------------------------
 * Per-device state
 * ------------------------------------------------------- */
struct edu_dev {
	struct pci_dev    *pdev;
	void __iomem      *bar0;

	struct miscdevice  miscdev;

	/* Legacy serialisation + completion (old ioctls) */
	struct mutex       lock;
	struct completion  irq_done;

	/* ---- DMA ring buffer ---- */
	void              *dma_virt;    /* kernel VA of full slab        */
	dma_addr_t         dma_handle;  /* bus address                   */

	struct sq_entry   *sq;          /* = dma_virt                    */
	struct cq_entry   *cq;          /* = dma_virt + SQ_SIZE          */

	/* Ring indices — protected by ring_lock */
	spinlock_t         ring_lock;
	u32                sq_tail;     /* next slot kernel writes SQ    */
	u32                cq_tail;     /* next slot ISR writes CQ       */
	u32                cq_head;     /* next slot userspace reads CQ  */

	/* In-flight opcode per tag slot (for ISR to know what completed) */
	u8                 inflight[RING_SIZE];

	/* eventfd — signalled by ISR when a CQ entry is posted */
	struct eventfd_ctx *evt_ctx;    /* NULL if not registered        */

	/* Legacy state */
	u32  last_irq;
	u32  last_fact_in;
	u32  last_fact_out;
	u32  dma_ok;

	/* ---- Observability counters (Phase 6) ----
	 * Incremented atomically from IRQ and ioctl context.
	 * Read from debugfs without taking any lock.
	 */
	atomic64_t stat_irq_total;        /* total IRQs handled              */
	atomic64_t stat_sq_submitted;     /* total SQ entries submitted      */
	atomic64_t stat_cq_posted;        /* total CQ entries posted by ISR  */
	atomic64_t stat_cq_consumed;      /* total CQ entries read by user   */
	atomic64_t stat_dma_errors;       /* DMA round-trips that failed     */
	atomic64_t stat_factorial_ops;    /* legacy factorial ioctl calls    */
	atomic64_t stat_eventfd_signals;  /* eventfd signals sent            */

	/* debugfs directory for this device */
	struct dentry *dbgfs_dir;

	/* ---- Fault injection (Phase 7) ----
	 * Set via:  echo 1 > /sys/class/misc/edu_pci/device/inject_fault
	 * Clear via: echo 0 > /sys/class/misc/edu_pci/device/inject_fault
	 * When non-zero, the next DMA completion is poisoned and
	 * the driver exercises its error-recovery path.
	 */
	atomic_t inject_fault;   /* 0 = normal, 1 = inject on next DMA */
};

/* -------------------------------------------------------
 * Helpers
 * ------------------------------------------------------- */

static inline u32 ring_next(u32 idx)
{
	return (idx + 1) & RING_MASK;
}

static inline bool ring_full(u32 tail, u32 head)
{
	/* full when tail would lap head */
	return ring_next(tail) == (head & RING_MASK);
}

/* -------------------------------------------------------
 * IRQ handler
 *
 * For legacy ioctls  → complete irq_done (old path unchanged).
 * For ring submissions→ write a CQ entry and signal eventfd.
 *
 * We distinguish by checking inflight[]: if anything is
 * pending in the ring we post a CQ entry; otherwise we fall
 * back to the legacy complete_all() path.
 * ------------------------------------------------------- */
static irqreturn_t edu_irq_handler(int irq, void *data)
{
	struct edu_dev *e = data;
	u32 irq_stat;
	unsigned long flags;
	bool ring_pending = false;

	irq_stat = readl(e->bar0 + EDU_REG_IRQ_STAT);
	if (!irq_stat)
		return IRQ_NONE;

	writel(irq_stat, e->bar0 + EDU_REG_IRQ_ACK);
	e->last_irq = irq_stat;
	atomic64_inc(&e->stat_irq_total);

	spin_lock_irqsave(&e->ring_lock, flags);

	/*
	 * Scan all ring slots for pending in-flight entries.
	 * The EDU device processes one command at a time, so only
	 * one slot should be inflight at any given IRQ, but we scan
	 * all to be safe and handle any ordering edge cases.
	 */
	{
		u32 i;
		for (i = 0; i < RING_SIZE; i++) {
			if (!e->inflight[i])
				continue;

			{
				struct sq_entry *sq  = &e->sq[i];
				struct cq_entry *cqe = &e->cq[e->cq_tail & RING_MASK];
				u32 result = 0;

				if (sq->opcode == EDU_OP_FACTORIAL)
					result = readl(e->bar0 + EDU_REG_FACT);
				else if (sq->opcode == EDU_OP_DMA_TEST)
					result = 0;

				cqe->tag    = sq->tag;
				cqe->status = 0;
				cqe->result = result;
				cqe->flags  = 0;
				cqe->rsvd   = 0;

				e->cq_tail = ring_next(e->cq_tail);
				e->inflight[i] = 0;
				atomic64_inc(&e->stat_cq_posted);
				ring_pending = true;
			}
			/* EDU is single-command — only one can complete per IRQ */
			break;
		}
	}

	spin_unlock_irqrestore(&e->ring_lock, flags);

	if (ring_pending && e->evt_ctx) {
		eventfd_signal(e->evt_ctx);
		atomic64_inc(&e->stat_eventfd_signals);
	}

	/* Always wake legacy waiters — harmless if nobody is waiting */
	complete_all(&e->irq_done);

	return IRQ_HANDLED;
}

static int edu_wait_irq(struct edu_dev *e, unsigned long timeout_ms)
{
	reinit_completion(&e->irq_done);
	if (!wait_for_completion_timeout(&e->irq_done,
					 msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return 0;
}

/* -------------------------------------------------------
 * Legacy ioctls (unchanged behaviour)
 * ------------------------------------------------------- */
static int edu_run_factorial(struct edu_dev *e, struct edu_fact_req *req)
{
	int ret;

	mutex_lock(&e->lock);
	e->last_fact_in = req->input;
	e->last_irq = 0;
	atomic64_inc(&e->stat_factorial_ops);

	writel(EDU_STATUS_IRQ_EN, e->bar0 + EDU_REG_STATUS);
	writel(req->input, e->bar0 + EDU_REG_FACT);

	ret = edu_wait_irq(e, 1000);
	if (ret)
		goto out;

	req->output     = readl(e->bar0 + EDU_REG_FACT);
	req->irq_status = e->last_irq;
	e->last_fact_out = req->output;
out:
	mutex_unlock(&e->lock);
	return ret;
}

static int edu_run_dma_test(struct edu_dev *e)
{
	static const u8 pattern[EDU_DMA_TEST_LEN] = {
		0x45,0x44,0x55,0x20,0x44,0x4d,0x41,0x20,
		0x72,0x6f,0x75,0x6e,0x64,0x74,0x72,0x69,
		0x70,0x20,0x74,0x65,0x73,0x74,0x20,0x70,
		0x61,0x74,0x74,0x65,0x72,0x6e,0x20,0x31,
		0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,
		0x30,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
		0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
		0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
	};
	u8 expected[EDU_DMA_TEST_LEN];
	int ret;

	mutex_lock(&e->lock);

