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

	memcpy(expected, pattern, sizeof(pattern));
	memcpy(e->dma_virt, pattern, sizeof(pattern));

	/* RAM -> EDU */
	e->last_irq = 0;
	writel(0, e->bar0 + EDU_REG_STATUS);
	writeq((u64)e->dma_handle, e->bar0 + EDU_REG_DMA_SRC);
	writeq(EDU_DMA_BUF_OFFSET,  e->bar0 + EDU_REG_DMA_DST);
	writeq(EDU_DMA_TEST_LEN,    e->bar0 + EDU_REG_DMA_CNT);
	writel(EDU_DMA_CMD_START | EDU_DMA_CMD_IRQ, e->bar0 + EDU_REG_DMA_CMD);

	ret = edu_wait_irq(e, 1000);
	if (ret)
		goto out;

	/* EDU -> RAM */
	memset(e->dma_virt, 0, EDU_DMA_TEST_LEN);
	e->last_irq = 0;
	writeq(EDU_DMA_BUF_OFFSET,  e->bar0 + EDU_REG_DMA_SRC);
	writeq((u64)e->dma_handle,  e->bar0 + EDU_REG_DMA_DST);
	writeq(EDU_DMA_TEST_LEN,    e->bar0 + EDU_REG_DMA_CNT);
	writel(EDU_DMA_CMD_START | EDU_DMA_CMD_DIR | EDU_DMA_CMD_IRQ,
	       e->bar0 + EDU_REG_DMA_CMD);

	ret = edu_wait_irq(e, 1000);
	if (ret)
		goto out;

	if (memcmp(e->dma_virt, expected, EDU_DMA_TEST_LEN) != 0) {
		atomic64_inc(&e->stat_dma_errors);
		ret = -EIO;
	} else if (atomic_cmpxchg(&e->inject_fault, 1, 0) == 1) {
		/* Fault injection: corrupt the result and return EIO */
		dev_warn(&e->pdev->dev,
			 "fault injection triggered — reporting DMA error\n");
		atomic64_inc(&e->stat_dma_errors);
		ret = -EIO;
	} else {
		e->dma_ok = 1;
	}
out:
	mutex_unlock(&e->lock);
	return ret;
}

/* -------------------------------------------------------
 * New ring submit path
 * ------------------------------------------------------- */
static int edu_ring_submit(struct edu_dev *e, struct edu_submit_req *req)
{
	unsigned long flags;
	u32 slot;

	spin_lock_irqsave(&e->ring_lock, flags);

	if (ring_full(e->sq_tail, e->cq_head)) {
		spin_unlock_irqrestore(&e->ring_lock, flags);
		return -ENOSPC;   /* ring full — caller should retry */
	}

	slot = e->sq_tail & RING_MASK;

	e->sq[slot].tag     = req->tag;
	e->sq[slot].opcode  = req->opcode;
	e->sq[slot].flags   = 0;
	e->sq[slot].operand = req->operand;
	e->sq[slot].rsvd    = 0;

	e->inflight[slot] = req->opcode;
	e->sq_tail = ring_next(e->sq_tail);
	atomic64_inc(&e->stat_sq_submitted);

	spin_unlock_irqrestore(&e->ring_lock, flags);

	/*
	 * Kick the hardware based on opcode.
	 * The IRQ handler will post the CQ entry when done.
	 */
	switch (req->opcode) {
	case EDU_OP_FACTORIAL:
		writel(EDU_STATUS_IRQ_EN, e->bar0 + EDU_REG_STATUS);
		writel(req->operand,      e->bar0 + EDU_REG_FACT);
		break;

	case EDU_OP_DMA_TEST:
		/*
		 * Trigger the same RAM->EDU->RAM round-trip as the legacy
		 * DMA test but via the ring path.
		 */
		writel(0, e->bar0 + EDU_REG_STATUS);
		writeq((u64)e->dma_handle, e->bar0 + EDU_REG_DMA_SRC);
		writeq(EDU_DMA_BUF_OFFSET, e->bar0 + EDU_REG_DMA_DST);
		writeq(EDU_DMA_TEST_LEN,   e->bar0 + EDU_REG_DMA_CNT);
		writel(EDU_DMA_CMD_START | EDU_DMA_CMD_IRQ,
		       e->bar0 + EDU_REG_DMA_CMD);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

/* -------------------------------------------------------
 * New ring poll-completion path
 *
 * Dequeues one entry from the CQ. Returns -EAGAIN if empty.
 * ------------------------------------------------------- */
static int edu_poll_cq(struct edu_dev *e, struct edu_completion_req *out)
{
	unsigned long flags;

	spin_lock_irqsave(&e->ring_lock, flags);

	if (e->cq_head == e->cq_tail) {
		spin_unlock_irqrestore(&e->ring_lock, flags);
		return -EAGAIN;   /* nothing ready yet */
	}

	{
		struct cq_entry *cqe = &e->cq[e->cq_head & RING_MASK];
		out->tag    = cqe->tag;
		out->status = cqe->status;
		out->result = cqe->result;
	}

	e->cq_head = ring_next(e->cq_head);
	atomic64_inc(&e->stat_cq_consumed);

	spin_unlock_irqrestore(&e->ring_lock, flags);
	return 0;
}

/* -------------------------------------------------------
 * file_operations
 * ------------------------------------------------------- */
static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *mdev = file->private_data;
	struct edu_dev    *e    = container_of(mdev, struct edu_dev, miscdev);

	switch (cmd) {

	/* ---- legacy ---- */
	case EDU_IOC_RUN_FACTORIAL: {
		struct edu_fact_req req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		if (edu_run_factorial(e, &req))
			return -ETIMEDOUT;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}

	case EDU_IOC_RUN_DMA_TEST:
		return edu_run_dma_test(e);

	case EDU_IOC_GET_STATE: {
		struct edu_state_req state = {
			.last_irq     = e->last_irq,
			.last_fact_in  = e->last_fact_in,
			.last_fact_out = e->last_fact_out,
			.dma_ok        = e->dma_ok,
		};
		if (copy_to_user((void __user *)arg, &state, sizeof(state)))
			return -EFAULT;
		return 0;
	}

	/* ---- new ring ---- */
	case EDU_IOC_SUBMIT: {
		struct edu_submit_req req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		return edu_ring_submit(e, &req);
	}

	case EDU_IOC_POLL_CQ: {
		struct edu_completion_req out = {};
		int ret;

		ret = edu_poll_cq(e, &out);
		if (ret)
			return ret;   /* -EAGAIN if empty */
		if (copy_to_user((void __user *)arg, &out, sizeof(out)))
			return -EFAULT;
		return 0;
	}

	case EDU_IOC_SET_EVENTFD: {
		int fd;
		struct eventfd_ctx *ctx;

		if (copy_from_user(&fd, (void __user *)arg, sizeof(fd)))
			return -EFAULT;

		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);

		/* Replace any previously registered eventfd */
		if (e->evt_ctx)
			eventfd_ctx_put(e->evt_ctx);
		e->evt_ctx = ctx;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

/*
 * mmap — exposes the entire DMA ring buffer (SQ + CQ) to userspace
 * as a read/write mapping.  Userspace can inspect SQ/CQ directly
 * without any copy; the zero-copy path for Phase 3.
 *
 * Usage: ptr = mmap(NULL, DMA_BUF_TOTAL, PROT_READ|PROT_WRITE,
 *                   MAP_SHARED, fd, 0);
 */
static int edu_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct miscdevice *mdev = file->private_data;
	struct edu_dev    *e    = container_of(mdev, struct edu_dev, miscdev);
	unsigned long size      = vma->vm_end - vma->vm_start;
	unsigned long pfn       = virt_to_phys(e->dma_virt) >> PAGE_SHIFT;

	if (size > DMA_BUF_TOTAL)
		return -EINVAL;

	/*
	 * On x86, dma_alloc_coherent returns regular cached memory.
	 * Use default page protection — no pgprot_noncached needed.
	 */
	return remap_pfn_range(vma, vma->vm_start, pfn, size,
			       vma->vm_page_prot);
}

/*
 * poll — lets userspace epoll/select on the char device.
 * Becomes readable when the CQ has at least one entry.
 */
static __poll_t edu_poll(struct file *file, poll_table *wait)
{
	struct miscdevice *mdev = file->private_data;
	struct edu_dev    *e    = container_of(mdev, struct edu_dev, miscdev);
	__poll_t mask = 0;

	/* No wait_queue wired yet — eventfd is the preferred async path.
	 * This poll() is here for completeness / blocking select() callers. */
	if (e->cq_head != e->cq_tail)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int edu_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct edu_dev    *e    = container_of(mdev, struct edu_dev, miscdev);
	unsigned long flags;

	/* Reset ring indices so each open starts with a clean slate */
	spin_lock_irqsave(&e->ring_lock, flags);
	e->sq_tail = 0;
	e->cq_tail = 0;
	e->cq_head = 0;
	memset(e->inflight, 0, sizeof(e->inflight));
	spin_unlock_irqrestore(&e->ring_lock, flags);

	return 0;
}

static const struct file_operations edu_fops = {
	.owner          = THIS_MODULE,
	.open           = edu_open,
	.unlocked_ioctl = edu_ioctl,
	.compat_ioctl   = edu_ioctl,
	.mmap           = edu_mmap,
	.poll           = edu_poll,
};

/* -------------------------------------------------------
 * sysfs fault injection (Phase 7)
 *
 * Exposes a single knob on the PCI device:
 *   /sys/class/misc/edu_pci/device/inject_fault
 *
 * Write "1" to arm. The next DMA round-trip will be poisoned
 * and the driver will return -EIO, exercising the error path.
 * The flag auto-clears after one fault so normal operation
 * resumes automatically.
 *
 * Example:
 *   echo 1 | sudo tee /sys/class/misc/edu_pci/device/inject_fault
 *   sudo ./edu_test   # will see DMA error then recover
 *   cat /sys/kernel/debug/edu_pci/stats | grep dma_errors
 * ------------------------------------------------------- */

static ssize_t inject_fault_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct edu_dev *e = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&e->inject_fault));
}

static ssize_t inject_fault_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct edu_dev *e = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	if (val > 1)
		return -EINVAL;

	atomic_set(&e->inject_fault, val);
	dev_info(dev, "fault injection %s\n", val ? "armed" : "disarmed");
	return count;
}

static DEVICE_ATTR_RW(inject_fault);

static struct attribute *edu_sysfs_attrs[] = {
	&dev_attr_inject_fault.attr,
	NULL,
};

static const struct attribute_group edu_sysfs_group = {
	.attrs = edu_sysfs_attrs,
};

/* -------------------------------------------------------
 * debugfs — /sys/kernel/debug/edu_pci/stats (Phase 6)
 *
 * Exposes all atomic counters as a human-readable text file.
 * Read with:  cat /sys/kernel/debug/edu_pci/stats
 * Reset with: echo 0 > /sys/kernel/debug/edu_pci/stats
 * ------------------------------------------------------- */

static int edu_dbgfs_stats_show(struct seq_file *m, void *v)
{
	struct edu_dev *e = m->private;
	unsigned long flags;
	u32 sq_tail, cq_tail, cq_head;

	spin_lock_irqsave(&e->ring_lock, flags);
	sq_tail = e->sq_tail;
	cq_tail = e->cq_tail;
	cq_head = e->cq_head;
	spin_unlock_irqrestore(&e->ring_lock, flags);

	seq_printf(m, "irq_total         %lld\n",
		   atomic64_read(&e->stat_irq_total));
	seq_printf(m, "sq_submitted      %lld\n",
		   atomic64_read(&e->stat_sq_submitted));
	seq_printf(m, "cq_posted         %lld\n",
		   atomic64_read(&e->stat_cq_posted));
	seq_printf(m, "cq_consumed       %lld\n",
		   atomic64_read(&e->stat_cq_consumed));
	seq_printf(m, "dma_errors        %lld\n",
		   atomic64_read(&e->stat_dma_errors));
	seq_printf(m, "factorial_ops     %lld\n",
		   atomic64_read(&e->stat_factorial_ops));
	seq_printf(m, "eventfd_signals   %lld\n",
		   atomic64_read(&e->stat_eventfd_signals));
	seq_printf(m, "ring_sq_tail      %u\n",  sq_tail);
	seq_printf(m, "ring_cq_tail      %u\n",  cq_tail);
	seq_printf(m, "ring_cq_head      %u\n",  cq_head);
	seq_printf(m, "ring_inflight     %u\n",
		   (cq_tail - cq_head) & RING_MASK);
	return 0;
}

static ssize_t edu_dbgfs_stats_write(struct file *file,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct edu_dev *e = ((struct seq_file *)file->private_data)->private;

	/* Any write resets all counters */
	atomic64_set(&e->stat_irq_total,       0);
	atomic64_set(&e->stat_sq_submitted,    0);
	atomic64_set(&e->stat_cq_posted,       0);
	atomic64_set(&e->stat_cq_consumed,     0);
	atomic64_set(&e->stat_dma_errors,      0);
	atomic64_set(&e->stat_factorial_ops,   0);
	atomic64_set(&e->stat_eventfd_signals, 0);

	return count;
}

static int edu_dbgfs_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, edu_dbgfs_stats_show, inode->i_private);
}

static const struct file_operations edu_dbgfs_stats_fops = {
	.owner   = THIS_MODULE,
	.open    = edu_dbgfs_stats_open,
	.read    = seq_read,
	.write   = edu_dbgfs_stats_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void edu_debugfs_init(struct edu_dev *e)
{
	e->dbgfs_dir = debugfs_create_dir("edu_pci", NULL);
	if (IS_ERR_OR_NULL(e->dbgfs_dir)) {
		dev_warn(&e->pdev->dev, "debugfs unavailable — skipping\n");
		e->dbgfs_dir = NULL;
		return;
	}
	debugfs_create_file("stats", 0644, e->dbgfs_dir, e,
			    &edu_dbgfs_stats_fops);
}

static void edu_debugfs_remove(struct edu_dev *e)
{
	debugfs_remove_recursive(e->dbgfs_dir);
	e->dbgfs_dir = NULL;
}

/* -------------------------------------------------------
 * PCI probe / remove
 * ------------------------------------------------------- */
static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_dev *e;
