/*
 * QEMU device-model skeleton for proto_pcie.
 *
 * This file is intentionally a starter scaffold: it shows the structure of the
 * BAR, registers, and timer-based completion model that the kernel driver and
 * userspace library expect.
 *
 * To make it build inside QEMU, move it into a QEMU source tree and wire it
 * into Meson + the PCI device registration path.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"

#define TYPE_PROTO_PCIE "proto-pcie"
OBJECT_DECLARE_SIMPLE_TYPE(ProtoPcieState, PROTO_PCIE)

#define REG_CTRL        0x00
#define REG_STATUS      0x04
#define REG_DOORBELL    0x08
#define REG_IRQ_ACK     0x0C
#define REG_DMA_LO      0x10
#define REG_DMA_HI      0x14
#define REG_DMA_LEN     0x18
#define REG_VERSION     0x1C

#define CTRL_RESET      (1u << 0)
#define CTRL_START      (1u << 1)
#define STATUS_READY    (1u << 0)
#define STATUS_DONE     (1u << 1)
#define STATUS_ERROR    (1u << 2)

struct ProtoPcieState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    QEMUTimer *completion_timer;
    uint32_t status;
    uint32_t last_opcode;
    uint32_t last_length;
    uint64_t dma_addr;
    uint32_t dma_len;
};

static void proto_pcie_raise_irq(ProtoPcieState *s)
{
    /* In a real implementation, wire this to MSI or INTx. */
    pci_set_irq(&s->parent_obj, true);
}

static void proto_pcie_lower_irq(ProtoPcieState *s)
{
    pci_set_irq(&s->parent_obj, false);
}

static void proto_pcie_complete(void *opaque)
{
    ProtoPcieState *s = opaque;
    s->status = STATUS_READY | STATUS_DONE;
    proto_pcie_raise_irq(s);
}

static uint64_t proto_pcie_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    ProtoPcieState *s = opaque;

    switch (addr) {
    case REG_STATUS:
        return s->status;
    case REG_VERSION:
        return 0x00010000u;
    default:
        return 0;
    }
}

static void proto_pcie_mmio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ProtoPcieState *s = opaque;

    switch (addr) {
    case REG_CTRL:
        if (value & CTRL_RESET) {
            s->status = STATUS_READY;
            proto_pcie_lower_irq(s);
        }
        if (value & CTRL_START) {
            s->status = STATUS_READY;
            timer_mod(s->completion_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 25);
        }
        break;
    case REG_DOORBELL:
        s->last_opcode = value;
        break;
    case REG_DMA_LEN:
        s->dma_len = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps proto_pcie_mmio_ops = {
    .read = proto_pcie_mmio_read,
    .write = proto_pcie_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void proto_pcie_realize(PCIDevice *pdev, Error **errp)
{
    ProtoPcieState *s = PROTO_PCIE(pdev);

    pci_config_set_vendor_id(pdev->config, 0x1AF4);
    pci_config_set_device_id(pdev->config, 0x1100);
    pci_config_set_class(pdev->config, PCI_CLASS_OTHERS);

    memory_region_init_io(&s->bar0, OBJECT(s), &proto_pcie_mmio_ops, s, TYPE_PROTO_PCIE, 0x1000);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    s->completion_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, proto_pcie_complete, s);
    s->status = STATUS_READY;
}

static void proto_pcie_exit(PCIDevice *pdev)
{
    ProtoPcieState *s = PROTO_PCIE(pdev);
    if (s->completion_timer) {
        timer_free(s->completion_timer);
        s->completion_timer = NULL;
    }
}

static void proto_pcie_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = proto_pcie_realize;
    k->exit = proto_pcie_exit;
    k->vendor_id = 0x1AF4;
    k->device_id = 0x1100;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Prototype PCIe device (skeleton)";
}

static const TypeInfo proto_pcie_info = {
    .name = TYPE_PROTO_PCIE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ProtoPcieState),
    .class_init = proto_pcie_class_init,
};

static void proto_pcie_register_types(void)
{
    type_register_static(&proto_pcie_info);
}

type_init(proto_pcie_register_types)
