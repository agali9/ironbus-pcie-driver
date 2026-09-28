# QEMU PCIe Device Skeleton

This directory contains a starter device-model sketch for the prototype PCIe driver.

What it covers:
- BAR0 registers for CTRL, STATUS, DOORBELL, IRQ ACK, DMA address/length
- command submission on MMIO write
- timer-based asynchronous completion
- interrupt raise path for MSI/INTx

Status:
- This is a **skeleton** to drop into a QEMU source tree.
- It is not wired into QEMU's Meson build here.
- The kernel simulation mode is the working path in this package.

Next integration step:
1. add the device to `hw/pci/meson.build`
2. register the type in a `type_init`
3. connect the completion timer to `pci_set_irq()` or MSI notification
4. map BAR0 with `memory_region_init_io()`
