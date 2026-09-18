// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/iopoll.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/sizes.h>
#include "edu_lab.h"

#define REG_ID 0x00
#define REG_LIVE 0x04
#define REG_FACT 0x08
#define REG_STATUS 0x20
#define REG_IRQ_STATUS 0x24
#define REG_IRQ_ACK 0x64
#define STATUS_BUSY 0x01
#define STATUS_IRQ 0x80
#define IRQ_FACT 0x01
#define IRQ_DMA 0x100
#define REG_DMA_SRC 0x80
#define REG_DMA_DST 0x88
#define REG_DMA_COUNT 0x90
#define REG_DMA_CMD 0x98
#define DMA_RUN 0x01
#define DMA_FROM_DEVICE 0x02
#define DMA_IRQ_ENABLE 0x04
#define EDU_BUFFER_BASE 0x40000ULL
#define LAB_TIMEOUT_MS 1000U

static bool test_drop_irq;
module_param(test_drop_irq, bool, 0444);
MODULE_PARM_DESC(test_drop_irq, "TEST ONLY: acknowledge IRQs without completing requests (default off)");

static unsigned int test_fail_probe;
module_param(test_fail_probe, uint, 0444);
MODULE_PARM_DESC(test_fail_probe, "TEST ONLY: inject probe failure at acquired-resource stage 1..6 (0 off)");

static bool inject_probe_failure(struct pci_dev *pdev, unsigned int stage)
{
    if (test_fail_probe != stage)
        return false;
    dev_info(&pdev->dev, "test probe failure stage=%u\n", stage);
    return true;
}

struct lab_device {
    struct pci_dev *pdev;
    void __iomem *bar;
    struct miscdevice misc;
    struct mutex op_mutex;
    struct kref ref;
    spinlock_t irq_lock;
    struct completion done;
    u32 expected_irq;
    u64 interrupts;
    int irq;
    enum edu_lab_state state;
    u64 computations;
    u64 timeouts;
    u64 dma_loopbacks;
    void *dma_cpu;
    dma_addr_t dma_addr;
};

static_assert(sizeof(struct edu_lab_header) == 16);
static_assert(sizeof(struct edu_lab_caps) == 80);
static_assert(sizeof(struct edu_lab_compute) == 32);
static_assert(sizeof(struct edu_lab_dma) == 4128);

static void lab_release_ref(struct kref *ref)
{
    kfree(container_of(ref, struct lab_device, ref));
}

static int lab_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct lab_device *lab = container_of(misc, struct lab_device, misc);

    kref_get(&lab->ref);
    file->private_data = lab;
    return nonseekable_open(inode, file);
}

static int lab_release(struct inode *inode, struct file *file)
{
    struct lab_device *lab = file->private_data;

    kref_put(&lab->ref, lab_release_ref);
    return 0;
}

/* IRQ context owns only expected_irq, completion, and the IRQ counter.
 * Operation state and all register programming are serialized by op_mutex. */
static irqreturn_t lab_irq(int irq, void *opaque)
{
    struct lab_device *lab = opaque;
    u32 pending = readl(lab->bar + REG_IRQ_STATUS);
    unsigned long flags;

    if (!pending)
        return IRQ_NONE;
    writel(pending, lab->bar + REG_IRQ_ACK);
    readl(lab->bar + REG_IRQ_STATUS); /* Flush the posted acknowledgement. */
    spin_lock_irqsave(&lab->irq_lock, flags);
    lab->interrupts++;
    if ((pending & lab->expected_irq) && !test_drop_irq)
        complete(&lab->done);
    spin_unlock_irqrestore(&lab->irq_lock, flags);
    return IRQ_HANDLED;
}

static void arm_completion(struct lab_device *lab, u32 expected)
{
    unsigned long flags;

    writel(~0U, lab->bar + REG_IRQ_ACK);
    readl(lab->bar + REG_IRQ_STATUS);
    spin_lock_irqsave(&lab->irq_lock, flags);
    reinit_completion(&lab->done);
    lab->expected_irq = expected;
    spin_unlock_irqrestore(&lab->irq_lock, flags);
}

static int wait_completion(struct lab_device *lab)
{
    unsigned long flags;
    bool done = wait_for_completion_timeout(&lab->done,
                                            msecs_to_jiffies(LAB_TIMEOUT_MS));

    spin_lock_irqsave(&lab->irq_lock, flags);
    lab->expected_irq = 0;
    spin_unlock_irqrestore(&lab->irq_lock, flags);
    if (!done) {
        lab->state = EDU_LAB_FAILED;
        lab->timeouts++;
        writel(0, lab->bar + REG_STATUS);
        /* No reuse after timeout. Keep resources alive until remove. */
        dev_info(&lab->pdev->dev, "request timeout; device failed closed\n");
        return -ETIMEDOUT;
    }
    return 0;
}

static int validate_header(const struct edu_lab_header *h, u32 size)
{
    if (h->version != EDU_LAB_ABI_VERSION || h->size != size ||
        h->flags || h->reserved)
        return -EINVAL;
    return 0;
}

static int dma_transfer(struct lab_device *lab, u32 offset, u32 length, bool from_device)
{
    int ret;

    lab->state = from_device ? EDU_LAB_DMA_FROM : EDU_LAB_DMA_TO;
    arm_completion(lab, IRQ_DMA);
    writeq(from_device ? EDU_BUFFER_BASE + offset : lab->dma_addr,
           lab->bar + REG_DMA_SRC);
    writeq(from_device ? lab->dma_addr : EDU_BUFFER_BASE + offset,
           lab->bar + REG_DMA_DST);
    writeq(length, lab->bar + REG_DMA_COUNT);
    dma_wmb();
    writeq(DMA_RUN | DMA_IRQ_ENABLE | (from_device ? DMA_FROM_DEVICE : 0),
           lab->bar + REG_DMA_CMD);
    ret = wait_completion(lab);
    if (!ret && (readq(lab->bar + REG_DMA_CMD) & DMA_RUN)) {
        lab->state = EDU_LAB_FAILED;
        ret = -EIO;
    }
    if (!ret)
        dma_rmb();
    return ret;
}

static long dma_loopback(struct lab_device *lab, void __user *user)
{
    struct edu_lab_dma *req;
    int ret;

    req = memdup_user(user, sizeof(*req));
    if (IS_ERR(req))
        return PTR_ERR(req);
    ret = validate_header(&req->header, sizeof(*req));
    /* Subtraction form avoids overflow in offset + length. */
    if (ret || req->reserved[0] || req->reserved[1] || !req->length ||
        req->offset >= EDU_LAB_DMA_BYTES ||
        req->length > EDU_LAB_DMA_BYTES - req->offset) {
        ret = -EINVAL;
        goto out;
    }
    if (lab->state == EDU_LAB_FAILED) {
        ret = -EIO;
        goto out;
    }
    memcpy(lab->dma_cpu, req->data, req->length);
    ret = dma_transfer(lab, req->offset, req->length, false);
    if (ret)
        goto out;
    /* Erase host contents to make a stale-buffer false pass impossible. */
    memset(lab->dma_cpu, 0, req->length);
    ret = dma_transfer(lab, req->offset, req->length, true);
    if (ret)
        goto out;
    memcpy(req->data, lab->dma_cpu, req->length);
    lab->state = EDU_LAB_IDLE;
    lab->dma_loopbacks++;
    ret = copy_to_user(user, req, sizeof(*req)) ? -EFAULT : 0;
out:
    kfree(req);
    return ret;
}

static long lab_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct lab_device *lab = file->private_data;
    void __user *user = (void __user *)arg;
    struct edu_lab_header header;
    struct edu_lab_compute compute;
    struct edu_lab_caps caps;
    unsigned long flags;
    int ret;

    if (cmd != EDU_LAB_GET_CAPS && cmd != EDU_LAB_COMPUTE &&
        cmd != EDU_LAB_DMA_LOOPBACK)
        return -ENOTTY;
    if (copy_from_user(&header, user, sizeof(header)))
        return -EFAULT;
    ret = validate_header(&header, _IOC_SIZE(cmd));
    if (ret)
        return ret;
    if (!mutex_trylock(&lab->op_mutex))
        return -EBUSY;
    if (lab->state == EDU_LAB_REMOVED) {
        ret = -ENODEV;
        goto out;
    }
    if (cmd == EDU_LAB_GET_CAPS) {
        memset(&caps, 0, sizeof(caps));
        caps.header = header;
        caps.device_id = readl(lab->bar + REG_ID);
        caps.max_factorial = EDU_LAB_MAX_FACTORIAL;
        caps.timeout_ms = LAB_TIMEOUT_MS;
        caps.state = lab->state;
        caps.features = EDU_LAB_FEATURE_COMPUTE | EDU_LAB_FEATURE_IRQ | EDU_LAB_FEATURE_DMA;
        caps.dma_bytes = EDU_LAB_DMA_BYTES;
        caps.dma_loopbacks = lab->dma_loopbacks;
        caps.dma_bits = 28;
        spin_lock_irqsave(&lab->irq_lock, flags);
        caps.interrupts = lab->interrupts;
        spin_unlock_irqrestore(&lab->irq_lock, flags);
        caps.computations = lab->computations;
        caps.timeouts = lab->timeouts;
        ret = copy_to_user(user, &caps, sizeof(caps)) ? -EFAULT : 0;
        goto out;
    }
    if (cmd == EDU_LAB_DMA_LOOPBACK) {
        ret = dma_loopback(lab, user);
        goto out;
    }
    if (copy_from_user(&compute, user, sizeof(compute))) {
        ret = -EFAULT;
        goto out;
    }
    ret = validate_header(&compute.header, sizeof(compute));
    if (ret || compute.reserved[0] || compute.reserved[1] ||
        compute.input > EDU_LAB_MAX_FACTORIAL) {
        ret = -EINVAL;
        goto out;
    }
    if (lab->state == EDU_LAB_FAILED) {
        ret = -EIO;
        goto out;
    }
    lab->state = EDU_LAB_COMPUTE;
    arm_completion(lab, IRQ_FACT);
    writel(STATUS_IRQ, lab->bar + REG_STATUS);
    writel(compute.input, lab->bar + REG_FACT);
    ret = wait_completion(lab);
    if (ret)
        goto out;
    if (readl(lab->bar + REG_STATUS) & STATUS_BUSY) {
        lab->state = EDU_LAB_FAILED;
        ret = -EIO;
        goto out;
    }
    compute.result = readl(lab->bar + REG_FACT);
    lab->state = EDU_LAB_IDLE;
    lab->computations++;
    ret = copy_to_user(user, &compute, sizeof(compute)) ? -EFAULT : 0;
out:
    mutex_unlock(&lab->op_mutex);
    return ret;
}

static const struct file_operations lab_fops = {
    .owner = THIS_MODULE,
    .open = lab_open,
    .release = lab_release,
    .unlocked_ioctl = lab_ioctl,
    .llseek = no_llseek,
};

static int lab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct lab_device *lab;
    int ret;

    if (test_fail_probe > 6)
        return -EINVAL;
    lab = kzalloc(sizeof(*lab), GFP_KERNEL);
    if (!lab)
        return -ENOMEM;
    kref_init(&lab->ref);
    mutex_init(&lab->op_mutex);
    spin_lock_init(&lab->irq_lock);
    init_completion(&lab->done);
    lab->pdev = pdev;
    ret = pci_enable_device_mem(pdev);
    if (ret)
        goto free_lab;
    if (inject_probe_failure(pdev, 1)) {
        ret = -EIO;
        goto disable;
    }
    if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
        pci_resource_len(pdev, 0) < SZ_1M) {
        ret = -ENODEV;
        goto disable;
    }
    ret = pci_request_region(pdev, 0, "edu_lab");
    if (ret)
        goto disable;
    if (inject_probe_failure(pdev, 2)) {
        ret = -EIO;
        goto release_region;
    }
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
    if (ret)
        goto release_region;
    lab->bar = pci_iomap(pdev, 0, SZ_1M);
    if (!lab->bar) {
        ret = -ENOMEM;
        goto release_region;
    }
    if (inject_probe_failure(pdev, 3)) {
        ret = -EIO;
        goto unmap;
    }
    /* Rebinding must not enable bus mastering while an old EDU DMA is active. */
    if ((readq(lab->bar + REG_DMA_CMD) & DMA_RUN) ||
        (readl(lab->bar + REG_STATUS) & STATUS_BUSY)) {
        ret = -EBUSY;
        goto unmap;
    }
    writel(0x12345678, lab->bar + REG_LIVE);
    if (readl(lab->bar + REG_ID) != 0x010000ed ||
        readl(lab->bar + REG_LIVE) != (u32)~0x12345678U) {
        ret = -ENODEV;
        goto unmap;
    }
    writel(0, lab->bar + REG_STATUS);
    writel(~0U, lab->bar + REG_IRQ_ACK);
    readl(lab->bar + REG_IRQ_STATUS);
    lab->dma_cpu = dma_alloc_coherent(&pdev->dev, EDU_LAB_DMA_BYTES,
                                      &lab->dma_addr, GFP_KERNEL);
    if (!lab->dma_cpu) {
        ret = -ENOMEM;
        goto unmap;
    }
    if (inject_probe_failure(pdev, 4)) {
        ret = -EIO;
        goto free_dma;
    }
    if (lab->dma_addr > DMA_BIT_MASK(28) - (EDU_LAB_DMA_BYTES - 1)) {
        ret = -EIO;
        goto free_dma;
    }
    pci_set_master(pdev);
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    if (ret < 0)
        goto clear_master;
    if (inject_probe_failure(pdev, 5)) {
        ret = -EIO;
        goto free_vectors;
    }
    lab->irq = pci_irq_vector(pdev, 0);
    ret = request_irq(lab->irq, lab_irq, 0, "edu_lab", lab);
    if (ret)
        goto free_vectors;
    if (inject_probe_failure(pdev, 6)) {
        ret = -EIO;
        goto free_irq;
    }
    lab->misc.minor = MISC_DYNAMIC_MINOR;
    lab->misc.name = "edu-lab";
    lab->misc.fops = &lab_fops;
    lab->misc.parent = &pdev->dev;
    lab->misc.mode = 0600;
    ret = misc_register(&lab->misc);
    if (ret)
        goto free_irq;
    pci_set_drvdata(pdev, lab);
    dev_info(&pdev->dev, "probe OK: BAR0, 28-bit DMA mask, MSI\n");
    return 0;
free_irq:
    free_irq(lab->irq, lab);
free_vectors:
    pci_free_irq_vectors(pdev);
clear_master:
    pci_clear_master(pdev);
free_dma:
    dma_free_coherent(&pdev->dev, EDU_LAB_DMA_BYTES, lab->dma_cpu, lab->dma_addr);
unmap:
    pci_iounmap(pdev, lab->bar);
release_region:
    pci_release_region(pdev, 0);
disable:
    pci_disable_device(pdev);
free_lab:
    kref_put(&lab->ref, lab_release_ref);
    return ret;
}

static void lab_remove(struct pci_dev *pdev)
{
    struct lab_device *lab = pci_get_drvdata(pdev);
    u64 dma_cmd;
    u32 status;
    int dma_ret, fact_ret;

    misc_deregister(&lab->misc);
    mutex_lock(&lab->op_mutex);
    lab->state = EDU_LAB_REMOVED;
    writel(0, lab->bar + REG_STATUS);
    /* EDU has no documented abort/reset. Drain, then revoke bus mastering
     * before releasing the coherent buffer even if the drain times out. */
    dma_ret = readq_poll_timeout(lab->bar + REG_DMA_CMD, dma_cmd,
                                !(dma_cmd & DMA_RUN), 1000, LAB_TIMEOUT_MS * 1000);
    fact_ret = readl_poll_timeout(lab->bar + REG_STATUS, status,
                                 !(status & STATUS_BUSY), 1000, LAB_TIMEOUT_MS * 1000);
    pci_clear_master(pdev);
    if (dma_ret || fact_ret)
        dev_err(&pdev->dev, "remove drain timed out; bus mastering revoked\n");
    writel(~0U, lab->bar + REG_IRQ_ACK);
    readl(lab->bar + REG_IRQ_STATUS);
    free_irq(lab->irq, lab);
    pci_free_irq_vectors(pdev);
    dma_free_coherent(&pdev->dev, EDU_LAB_DMA_BYTES, lab->dma_cpu, lab->dma_addr);
    pci_iounmap(pdev, lab->bar);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);
    mutex_unlock(&lab->op_mutex);
    dev_info(&pdev->dev, "remove OK\n");
    kref_put(&lab->ref, lab_release_ref);
}

static const struct pci_device_id lab_ids[] = {
    { PCI_DEVICE(0x1234, 0x11e8) }, { }
};
MODULE_DEVICE_TABLE(pci, lab_ids);
static struct pci_driver lab_driver = {
    .name = "edu_lab", .id_table = lab_ids,
    .probe = lab_probe, .remove = lab_remove,
};
module_pci_driver(lab_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU EDU personal driver lab");
