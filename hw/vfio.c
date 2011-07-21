/*
 * vfio based device assignment support
 *
 * Copyright Red Hat, Inc. 2011
 *
 * Authors:
 *  Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Based on qemu-kvm device-assignment:
 *  Adapted for KVM by Qumranet.
 *  Copyright (c) 2007, Neocleus, Alex Novik (alex@neocleus.com)
 *  Copyright (c) 2007, Neocleus, Guy Zana (guy@neocleus.com)
 *  Copyright (C) 2008, Qumranet, Amit Shah (amit.shah@qumranet.com)
 *  Copyright (C) 2008, Red Hat, Amit Shah (amit.shah@redhat.com)
 *  Copyright (C) 2008, IBM, Muli Ben-Yehuda (muli@il.ibm.com)
 */

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "event_notifier.h"
#include "hw.h"
#include "kvm.h"
#include "memory.h"
#include "monitor.h"
#include "msi.h"
#include "msix.h"
#include "notify.h"
#include "pc.h"
#include "qemu-error.h"
#include "qemu-timer.h"
#include "range.h"
#include "vfio.h"
#include <pci/header.h>
#include <pci/types.h>
#include <linux/types.h>
#include "linux-vfio.h"

//#define DEBUG_VFIO
#ifdef DEBUG_VFIO
#define DPRINTF(fmt, ...) \
    do { printf("vfio: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

/* TODO: msix.h should define these */
#define MSIX_CAP_LENGTH 12
#define MSIX_PAGE_SIZE 0x1000

static void vfio_disable_interrupts(VFIODevice *vdev);
static uint32_t vfio_pci_read_config(PCIDevice *pdev, uint32_t addr, int len);

static uint8_t vfio_find_cap_offset(PCIDevice *pdev, uint8_t cap)
{
    int max_cap = (PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE) /
                  PCI_CAP_SIZEOF;
    uint8_t id, pos = PCI_CAPABILITY_LIST;

    if (!(pdev->config[PCI_STATUS] & PCI_STATUS_CAP_LIST)) {
        return 0;
    }

    while (max_cap--) {
        pos = pdev->config[pos] & ~3;
        if (pos < PCI_CONFIG_HEADER_SIZE) {
            break;
        }

        id = pdev->config[pos + PCI_CAP_LIST_ID];

        if (id == 0xff) {
            break;
        }
        if (id == cap) {
            return pos;
        }

        pos += PCI_CAP_LIST_NEXT;
    }
    return 0;
}

/*
 * QDev routines
 */
static int parse_hostaddr(DeviceState *qdev, Property *prop, const char *str)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(qdev, prop);
    const char *p = str;
    int n, seg, bus, dev, func;
    char field[5];

    if (sscanf(p, "%4[^:]%n", field, &n) != 1 || p[n] != ':') {
        return -EINVAL;
    }

    seg = strtol(field, NULL, 16);
    p += n + 1;

    if (sscanf(p, "%4[^:]%n", field, &n) != 1) {
        return -EINVAL;
    }

    if (p[n] == ':') {
        bus = strtol(field, NULL, 16);
        p += n + 1;
    } else {
        bus = seg;
        seg = 0;
    }

    if (sscanf(p, "%4[^.]%n", field, &n) != 1 || p[n] != '.') {
        return -EINVAL;
    }

    dev = strtol(field, NULL, 16);
    p += n + 1;

    if (!qemu_isdigit(*p)) {
        return -EINVAL;
    }

    func = *p - '0';

    ptr->seg = seg;
    ptr->bus = bus;
    ptr->dev = dev;
    ptr->func = func;
    return 0;
}

static int print_hostaddr(DeviceState *qdev, Property *prop,
                          char *dest, size_t len)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(qdev, prop);

    return snprintf(dest, len, "%04x:%02x:%02x.%x",
                    ptr->seg, ptr->bus, ptr->dev, ptr->func);
}

/*
 * INTx
 */
static inline void vfio_unmask_intx(VFIODevice *vdev)
{
    ioctl(vdev->vfiofd, VFIO_UNMASK_IRQ);
}

static void vfio_intx_interrupt(void *opaque)
{
    VFIODevice *vdev = opaque;

    if (!event_notifier_test_and_clear(&vdev->intx.interrupt)) {
        return;
    }

    DPRINTF("%s(%04x:%02x:%02x.%x) Pin %c\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func,
            'A' + vdev->intx.pin);

    vdev->intx.pending = true;
    qemu_set_irq(vdev->pdev.irq[vdev->intx.pin], 1);
}

static void vfio_eoi(Notifier *notify)
{
    VFIODevice *vdev = container_of(notify, VFIODevice, intx.eoi);

    if (!vdev->intx.pending) {
        return;
    }

    DPRINTF("%s(%04x:%02x:%02x.%x) EOI\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func);

    vdev->intx.pending = false;
    qemu_set_irq(vdev->pdev.irq[vdev->intx.pin], 0);
    vfio_unmask_intx(vdev);
}

static void vfio_update_irq(Notifier *notify)
{
    VFIODevice *vdev = container_of(notify, VFIODevice, intx.update_irq);
    int irq = pci_get_irq(&vdev->pdev, vdev->intx.pin);

    if (irq == vdev->intx.irq) {
        return;
    }

    DPRINTF("%s(%04x:%02x:%02x.%x) IRQ moved %d -> %d\n", __FUNCTION__,
            vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, vdev->intx.irq, irq);

    ioapic_remove_gsi_eoi_notifier(&vdev->intx.eoi, vdev->intx.irq);

    vdev->intx.irq = irq;

    if (irq < 0) {
        fprintf(stderr, "vfio: Error - INTx moved to IRQ %d\n", irq);
        return;
    }

    ioapic_add_gsi_eoi_notifier(&vdev->intx.eoi, vdev->intx.irq);

    /* Re-enable the interrupt in cased we missed an EOI */
    vfio_eoi(&vdev->intx.eoi);
}

static int vfio_enable_intx(VFIODevice *vdev)
{
    int fd;
    uint8_t pin = vfio_pci_read_config(&vdev->pdev, PCI_INTERRUPT_PIN, 1);

    if (!pin) {
        return 0;
    }

    vfio_disable_interrupts(vdev);

    vdev->intx.pin = pin - 1; /* Pin A (1) -> irq[0] */
    vdev->intx.irq = pci_get_irq(&vdev->pdev, vdev->intx.pin);
    vdev->intx.eoi.notify = vfio_eoi;
    ioapic_add_gsi_eoi_notifier(&vdev->intx.eoi, vdev->intx.irq);

    vdev->intx.update_irq.notify = vfio_update_irq;
    pci_add_irq_update_notifier(&vdev->pdev, &vdev->intx.update_irq);

    if (event_notifier_init(&vdev->intx.interrupt, 0)) {
        fprintf(stderr, "vfio: Error: event_notifier_init failed\n");
        return -1;
    }

    fd = event_notifier_get_fd(&vdev->intx.interrupt);
    qemu_set_fd_handler(fd, vfio_intx_interrupt, NULL, vdev);

    if (ioctl(vdev->vfiofd, VFIO_SET_IRQ_EVENTFD, &fd)) {
        fprintf(stderr, "vfio: Error: Failed to setup INTx fd %s\n",
                strerror(errno));
        return -1;
    }

    vdev->interrupt = INT_INTx;

    vfio_unmask_intx(vdev);

    DPRINTF("%s(%04x:%02x:%02x.%x)\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func);

    return 0;
}

static void vfio_disable_intx(VFIODevice *vdev)
{
    int fd = -1;

    ioctl(vdev->vfiofd, VFIO_SET_IRQ_EVENTFD, &fd);

    pci_remove_irq_update_notifier(&vdev->pdev, &vdev->intx.update_irq);
    ioapic_remove_gsi_eoi_notifier(&vdev->intx.eoi, vdev->intx.irq);

    fd = event_notifier_get_fd(&vdev->intx.interrupt);
    qemu_set_fd_handler(fd, NULL, NULL, vdev);
    event_notifier_cleanup(&vdev->intx.interrupt);

    vdev->interrupt = INT_NONE;

    DPRINTF("%s(%04x:%02x:%02x.%x)\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func);
}

/*
 * MSI/X
 */
static void vfio_msi_interrupt(void *opaque)
{
    MSIVector *vec = opaque;
    VFIODevice *vdev = vec->vdev;

    if (!event_notifier_test_and_clear(&vec->interrupt)) {
        return;
    }

    DPRINTF("%s(%04x:%02x:%02x.%x) vector %d\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func, vec->vector);

    if (vdev->interrupt == INT_MSIX) {
        msix_notify(&vdev->pdev, vec->vector);
    } else if (vdev->interrupt == INT_MSI) {
        msi_notify(&vdev->pdev, vec->vector);
    } else {
        fprintf(stderr, "vfio: MSI interrupt receieved, but not enabled?\n");
    }
}

static void vfio_enable_msi(VFIODevice *vdev, bool msix)
{
    int i, *fds;
    int vfio_ioctl = msix ? VFIO_SET_MSIX_EVENTFDS : VFIO_SET_MSI_EVENTFDS;

    vfio_disable_interrupts(vdev);

    vdev->nr_vectors = msix ? vdev->pdev.msix_entries_nr :
                              msi_nr_vectors_allocated(&vdev->pdev);
    vdev->msi_vectors = qemu_malloc(vdev->nr_vectors * sizeof(MSIVector));

    fds = qemu_malloc((vdev->nr_vectors + 1) * sizeof(int));
    fds[0] = vdev->nr_vectors;

    for (i = 0; i < vdev->nr_vectors; i++) {
        vdev->msi_vectors[i].vdev = vdev;
        vdev->msi_vectors[i].vector = i;

        if (event_notifier_init(&vdev->msi_vectors[i].interrupt, 0)) {
            fprintf(stderr, "vfio: Error: event_notifier_init failed\n");
        }

        fds[i + 1] = event_notifier_get_fd(&vdev->msi_vectors[i].interrupt);
        qemu_set_fd_handler(fds[i + 1], vfio_msi_interrupt, NULL,
                            &vdev->msi_vectors[i]);

        if (msix && msix_vector_use(&vdev->pdev, i) < 0) {
            fprintf(stderr, "vfio: Error msix_vector_use\n");
        }
    }

    if (ioctl(vdev->vfiofd, vfio_ioctl, fds)) {
        fprintf(stderr, "vfio: Error: Failed to setup MSI/X fds %s\n",
                strerror(errno));
        for (i = 0; i < vdev->nr_vectors; i++) {
            if (msix) {
                msix_vector_unuse(&vdev->pdev, i);
            }
            qemu_set_fd_handler(fds[i + 1], NULL, NULL, NULL);
            event_notifier_cleanup(&vdev->msi_vectors[i].interrupt);
        }
        qemu_free(fds);
        qemu_free(vdev->msi_vectors);
        vdev->nr_vectors = 0;
        return;
    }

    vdev->interrupt = msix ? INT_MSIX : INT_MSI;

    qemu_free(fds);

    DPRINTF("%s(%04x:%02x:%02x.%x) Enabled %d vectors\n", __FUNCTION__,
            vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, vdev->nr_vectors);
}

static void vfio_disable_msi(VFIODevice *vdev, bool msix)
{
    int i, vectors = 0;
    int vfio_ioctl = msix ? VFIO_SET_MSIX_EVENTFDS : VFIO_SET_MSI_EVENTFDS;

    ioctl(vdev->vfiofd, vfio_ioctl, &vectors);

    for (i = 0; i < vdev->nr_vectors; i++) {
        int fd = event_notifier_get_fd(&vdev->msi_vectors[i].interrupt);

        if (msix) {
            msix_vector_unuse(&vdev->pdev, i);
        }

        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        event_notifier_cleanup(&vdev->msi_vectors[i].interrupt);
    }

    qemu_free(vdev->msi_vectors);
    vdev->nr_vectors = 0;
    vdev->interrupt = INT_NONE;

    DPRINTF("%s(%04x:%02x:%02x.%x)\n", __FUNCTION__, vdev->host.seg,
            vdev->host.bus, vdev->host.dev, vdev->host.func);

    vfio_enable_intx(vdev);
}

/*
 * IO Port/MMIO
 */
static void vfio_resource_write(PCIResource *res, uint32_t addr,
                                uint32_t val, int len)
{
    size_t offset = vfio_pci_space_to_offset(VFIO_PCI_BAR0_RESOURCE + res->bar);

    if (pwrite(res->vfiofd, &val, len, offset + addr) != len) {
        fprintf(stderr, "%s(,0x%x, 0x%x, %d) failed: %s\n",
                __FUNCTION__, addr, val, len, strerror(errno));
    }
    DPRINTF("%s(BAR%d+0x%x, 0x%x, %d)\n", __FUNCTION__, res->bar,
            addr, val, len);
}

static void vfio_resource_writeb(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    vfio_resource_write(opaque, addr, val, 1);
}

static void vfio_resource_writew(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    vfio_resource_write(opaque, addr, val, 2);
}

static void vfio_resource_writel(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    vfio_resource_write(opaque, addr, val, 4);
}

static CPUWriteMemoryFunc * const vfio_resource_writes[] = {
    &vfio_resource_writeb,
    &vfio_resource_writew,
    &vfio_resource_writel
};

static void vfio_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    PCIResource *res = opaque;
    vfio_resource_write(res, addr - res->e_phys, val, 1);
}

static void vfio_ioport_writew(void *opaque, uint32_t addr, uint32_t val)
{
    PCIResource *res = opaque;
    vfio_resource_write(res, addr - res->e_phys, val, 2);
}

static void vfio_ioport_writel(void *opaque, uint32_t addr, uint32_t val)
{
    PCIResource *res = opaque;
    vfio_resource_write(res, addr - res->e_phys, val, 4);
}

static uint32_t vfio_resource_read(PCIResource *res, uint32_t addr, int len)
{
    size_t offset = vfio_pci_space_to_offset(VFIO_PCI_BAR0_RESOURCE + res->bar);
    uint32_t val;

    if (pread(res->vfiofd, &val, len, offset + addr) != len) {
        fprintf(stderr, "%s(,0x%x, %d) failed: %s\n",
                __FUNCTION__, addr, len, strerror(errno));
        return 0xffffffffU;
    }
    DPRINTF("%s(BAR%d+0x%x, %d) = 0x%x\n", __FUNCTION__, res->bar,
            addr, len, val);
    return val;
}

static uint32_t vfio_resource_readb(void *opaque, target_phys_addr_t addr)
{
    return vfio_resource_read(opaque, addr, 1) & 0xff;
}

static uint32_t vfio_resource_readw(void *opaque, target_phys_addr_t addr)
{
    return vfio_resource_read(opaque, addr, 2) & 0xffff;
}

static uint32_t vfio_resource_readl(void *opaque, target_phys_addr_t addr)
{
    return vfio_resource_read(opaque, addr, 4);
}

static CPUReadMemoryFunc * const vfio_resource_reads[] = {
    &vfio_resource_readb,
    &vfio_resource_readw,
    &vfio_resource_readl
};

static uint32_t vfio_ioport_readb(void *opaque, uint32_t addr)
{
    PCIResource *res = opaque;
    return vfio_resource_read(res, addr - res->e_phys, 1) & 0xff;
}

static uint32_t vfio_ioport_readw(void *opaque, uint32_t addr)
{
    PCIResource *res = opaque;
    return vfio_resource_read(res, addr - res->e_phys, 2) & 0xffff;
}

static uint32_t vfio_ioport_readl(void *opaque, uint32_t addr)
{
    PCIResource *res = opaque;
    return vfio_resource_read(res, addr - res->e_phys, 4);
}

static void vfio_ioport_map(PCIDevice *pdev, int bar,
                            pcibus_t e_phys, pcibus_t e_size, int type)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);
    PCIResource *res = &vdev->resources[bar];

    DPRINTF("%s(%04x:%02x:%02x.%x, %d, 0x%" PRIx64 ", 0x%" PRIx64 ", %d)\n",
            __FUNCTION__, vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, bar, e_phys, e_size, type);

    res->e_phys = e_phys;
    res->e_size = e_size;

    register_ioport_write(e_phys, e_size, 1, vfio_ioport_writeb, res);
    register_ioport_write(e_phys, e_size, 2, vfio_ioport_writew, res);
    register_ioport_write(e_phys, e_size, 4, vfio_ioport_writel, res);
    register_ioport_read(e_phys, e_size, 1, vfio_ioport_readb, res);
    register_ioport_read(e_phys, e_size, 2, vfio_ioport_readw, res);
    register_ioport_read(e_phys, e_size, 4, vfio_ioport_readl, res);
}

static void vfio_iomem_map(PCIDevice *pdev, int bar,
                           pcibus_t e_phys, pcibus_t e_size, int type)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);
    PCIResource *res = &vdev->resources[bar];

    DPRINTF("%s(%04x:%02x:%02x.%x, %d, 0x%" PRIx64 ", 0x%" PRIx64 ", %d)\n",
            __FUNCTION__, vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, bar, e_phys, e_size, type);

    res->e_phys = e_phys;
    res->e_size = e_size;

    if (res->msix) {
        if (res->msix_offset > 0) {
            cpu_register_physical_memory(e_phys, res->msix_offset, res->slow ?
                                         res->io_mem : res->memory_index[0]);
        }

        DPRINTF("Overlaying MSI-X table page\n");
        msix_mmio_map(pdev, bar, e_phys, e_size, type);

        if (e_size > res->msix_offset + MSIX_PAGE_SIZE) {
            uint32_t offset = res->msix_offset + MSIX_PAGE_SIZE;
            e_phys += offset;
            e_size -= offset;
            cpu_register_physical_memory_offset(e_phys, e_size,
                            res->slow ? res->io_mem : res->memory_index[1],
                            res->slow ? offset : 0);
        }
    } else {
        cpu_register_physical_memory(e_phys, e_size, res->slow ?
                                     res->io_mem : res->memory_index[0]);
    }
}

/*
 * PCI config space
 */
static uint32_t vfio_pci_read_config(PCIDevice *pdev, uint32_t addr, int len)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);
    uint32_t val = 0;

    if (ranges_overlap(addr, len, PCI_ROM_ADDRESS, 4) ||
        (pdev->cap_present & QEMU_PCI_CAP_MSIX &&
         ranges_overlap(addr, len, pdev->msix_cap, MSIX_CAP_LENGTH)) ||
        (pdev->cap_present & QEMU_PCI_CAP_MSI &&
         ranges_overlap(addr, len, pdev->msi_cap, vdev->msi_cap_size))) {

        val = pci_default_read_config(pdev, addr, len);
    } else {
        if (pread(vdev->vfiofd, &val, len, VFIO_PCI_CONFIG_OFF + addr) != len) {
            fprintf(stderr, "%s(%04x:%02x:%02x.%x, 0x%x, 0x%x) failed: %s\n",
                    __FUNCTION__, vdev->host.seg, vdev->host.bus,
                    vdev->host.dev, vdev->host.func, addr, len,
                    strerror(errno));
            return -1;
        }
    }
    DPRINTF("%s(%04x:%02x:%02x.%x, 0x%x, 0x%x) %x\n", __FUNCTION__,
            vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, addr, len, val);
    return val;
}

static void vfio_pci_write_config(PCIDevice *pdev, uint32_t addr,
                                  uint32_t val, int len)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);

    DPRINTF("%s(%04x:%02x:%02x.%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__,
            vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, addr, val, len);

    /* Write everything to VFIO, let it filter out what we can't write */
    if (pwrite(vdev->vfiofd, &val, len, VFIO_PCI_CONFIG_OFF + addr) != len) {
        fprintf(stderr, "%s(%04x:%02x:%02x.%x, 0x%x, 0x%x, 0x%x) failed: %s\n",
                __FUNCTION__, vdev->host.seg, vdev->host.bus, vdev->host.dev,
                vdev->host.func, addr, val, len, strerror(errno));
    }

    /* Write standard header bits to emulation */
    if (addr < PCI_CONFIG_HEADER_SIZE) {
        pci_default_write_config(pdev, addr, val, len);
        return;
    }

    /* MSI/MSI-X Enabling/Disabling */
    if (pdev->cap_present & QEMU_PCI_CAP_MSI &&
        ranges_overlap(addr, len, pdev->msi_cap, vdev->msi_cap_size)) {
        int is_enabled, was_enabled = msi_enabled(pdev);

        pci_default_write_config(pdev, addr, val, len);
        msi_write_config(pdev, addr, val, len);

        is_enabled = msi_enabled(pdev);

        if (!was_enabled && is_enabled) {
            vfio_enable_msi(vdev, false);
        } else if (was_enabled && !is_enabled) {
            vfio_disable_msi(vdev, false);
        }
    }

    if (pdev->cap_present & QEMU_PCI_CAP_MSIX &&
        ranges_overlap(addr, len, pdev->msix_cap, MSIX_CAP_LENGTH)) {
        int is_enabled, was_enabled = msix_enabled(pdev);

        pci_default_write_config(pdev, addr, val, len);
        msix_write_config(pdev, addr, val, len);

        is_enabled = msix_enabled(pdev);

        if (!was_enabled && is_enabled) {
            vfio_enable_msi(vdev, true);
        } else if (was_enabled && !is_enabled) {
            vfio_disable_msi(vdev, true);
        }
    }
}

/*
 * DMA
 */
static int vfio_dma_map(VFIODevice *vdev, target_phys_addr_t start_addr,
                        ram_addr_t size, ram_addr_t phys_offset)
{
    struct vfio_dma_map dma_map;

    DPRINTF("%s(%04x:%02x:%02x.%x) 0x%" PRIx64 "[0x%lx] -> 0x%lx\n",
            __FUNCTION__, vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, start_addr, size, phys_offset);

    dma_map.vaddr = (uintptr_t)qemu_get_ram_ptr(phys_offset);
    dma_map.dmaaddr = start_addr;
    dma_map.flags = VFIO_FLAG_WRITE;

    while (size) {
        /* Pass "reasonably sized" chunks to vfio */
        dma_map.size = MIN(size, VFIO_MAX_MAP_SIZE);

        if (ioctl(vdev->vfiofd, VFIO_MAP_DMA, &dma_map)) {
            DPRINTF("VFIO_MAP_DMA: %d\n", errno);
            return -errno;
        }

        size -= dma_map.size;
        dma_map.vaddr += dma_map.size;
        dma_map.dmaaddr += dma_map.size;
    }

    return 0;
}

static int vfio_dma_unmap(VFIODevice *vdev, target_phys_addr_t start_addr,
                          ram_addr_t size, ram_addr_t phys_offset)
{
    struct vfio_dma_map dma_map;

    DPRINTF("%s(%04x:%02x:%02x.%x) 0x%" PRIx64 "[0x%lx] -> 0x%lx\n",
            __FUNCTION__, vdev->host.seg, vdev->host.bus, vdev->host.dev,
            vdev->host.func, start_addr, size, phys_offset);

    dma_map.vaddr = (uintptr_t)qemu_get_ram_ptr(phys_offset);
    dma_map.dmaaddr = start_addr;
    dma_map.flags = VFIO_FLAG_WRITE;
    dma_map.size = size;

    if (ioctl(vdev->vfiofd, VFIO_UNMAP_DMA, &dma_map)) {
        DPRINTF("VFIO_UNMAP_DMA: %d\n", errno);
        return -errno;
    }

    return 0;
}

static void vfio_client_set_memory(struct CPUPhysMemoryClient *client,
                                   target_phys_addr_t start_addr,
                                   ram_addr_t size, ram_addr_t phys_offset,
                                   bool log_dirty)
{
    VFIOUIOMMU *uiommu = container_of(client, VFIOUIOMMU, client);
    VFIODevice *vdev = QLIST_FIRST(&uiommu->vdevs);
    ram_addr_t flags = phys_offset & ~TARGET_PAGE_MASK;
    int ret;

    if (!vdev) {
        fprintf(stderr, "%s: Error, called with no vdevs\n", __FUNCTION__);
        return;
    }

    if ((start_addr | size) & ~TARGET_PAGE_MASK) {
        return;
    }

    if (flags == IO_MEM_RAM) {
        ret = vfio_dma_map(vdev, start_addr, size, phys_offset);
        if (!ret) {
            return;
        }

        if (ret == -EBUSY) {
            /* EBUSY means the target address is already set.  Check if the
             * current mapping has changed.  If it hasn't, do nothing.  If it
             * has, unmap and remap the new phys_offset for each page.  On x86
             * this typically only happens for remapping of areas below 1MB. */
            target_phys_addr_t curr = start_addr;
            target_phys_addr_t end = start_addr + size;
            ram_addr_t curr_phys = phys_offset;

            while (curr < end) {
                ram_addr_t phys = cpu_get_physical_page_desc(curr);

                if (phys != curr_phys) {
                    vfio_dma_unmap(vdev, curr, TARGET_PAGE_SIZE, phys);
                    ret = vfio_dma_map(vdev, curr, TARGET_PAGE_SIZE, curr_phys);
                    if (ret) {
                        break;
                    }
                }
                curr += TARGET_PAGE_SIZE;
                curr_phys += TARGET_PAGE_SIZE;
            }

            if (curr >= end) {
                return;
            }
        }

        vfio_dma_unmap(vdev, start_addr, size, phys_offset);

        fprintf(stderr, "%s: "
                "Failed to map region %llx - %llx for device "
                "%04x:%02x:%02x.%x: %s\n", __FUNCTION__,
                (unsigned long long)start_addr,
                (unsigned long long)(start_addr + size - 1), vdev->host.seg,
                vdev->host.bus, vdev->host.dev, vdev->host.func,
                strerror(-ret));

    } else if (flags == IO_MEM_UNASSIGNED) {
        ret = vfio_dma_unmap(vdev, start_addr, size, phys_offset);
        if (!ret) {
            return;
        }
        fprintf(stderr, "%s: "
                "Failed to unmap region %llx - %llx for device "
                "%04x:%02x:%02x.%x: %s\n", __FUNCTION__,
                (unsigned long long)start_addr,
                (unsigned long long)(start_addr + size - 1), vdev->host.seg,
                vdev->host.bus, vdev->host.dev, vdev->host.func,
                strerror(-ret));
    }
}

static int vfio_client_sync_dirty_bitmap(struct CPUPhysMemoryClient *client,
                                         target_phys_addr_t start_addr,
                                         target_phys_addr_t end_addr)
{
    return 0;
}

static int vfio_client_migration_log(struct CPUPhysMemoryClient *client,
                                     int enable)
{
    return 0;
}

/*
 * Interrupt setup
 */
static void vfio_disable_interrupts(VFIODevice *vdev)
{
    switch (vdev->interrupt) {
    case INT_INTx:
        vfio_disable_intx(vdev);
        break;
    case INT_MSI:
        vfio_disable_msi(vdev, false);
        break;
    case INT_MSIX:
        vfio_disable_msi(vdev, true);
    }
}

static int vfio_setup_msi(VFIODevice *vdev)
{
    int pos;

    if ((pos = vfio_find_cap_offset(&vdev->pdev, PCI_CAP_ID_MSI))) {
        uint16_t ctrl;
        bool msi_64bit, msi_maskbit;
        int entries;

        if (pread(vdev->vfiofd, &ctrl, sizeof(ctrl),
                  VFIO_PCI_CONFIG_OFF + pos + PCI_CAP_FLAGS) != sizeof(ctrl)) {
            return -1;
        }

        msi_64bit = !!(ctrl & PCI_MSI_FLAGS_64BIT);
        msi_maskbit = !!(ctrl & PCI_MSI_FLAGS_MASKBIT);
        entries = 1 << ((ctrl & PCI_MSI_FLAGS_QMASK) >> 1);

        DPRINTF("%04x:%02x:%02x.%x PCI MSI CAP @0x%x\n", vdev->host.seg,
                vdev->host.bus, vdev->host.dev, vdev->host.func, pos);

        if (msi_init(&vdev->pdev, pos, entries, msi_64bit, msi_maskbit) < 0) {
            fprintf(stderr, "vfio: msi_init failed\n");
            return -1;
        }
        vdev->msi_cap_size = 0xa + (msi_maskbit ? 0xa : 0) +
                             (msi_64bit ? 0x4 : 0);
    }

    if ((pos = vfio_find_cap_offset(&vdev->pdev, PCI_CAP_ID_MSIX))) {
        uint16_t ctrl;
        uint32_t table, offset;
        uint64_t len;
        int bar, entries;

        if (pread(vdev->vfiofd, &ctrl, sizeof(ctrl),
                  VFIO_PCI_CONFIG_OFF + pos + PCI_CAP_FLAGS) != sizeof(ctrl)) {
            return -1;
        }

        if (pread(vdev->vfiofd, &table, sizeof(table), VFIO_PCI_CONFIG_OFF +
                  pos + PCI_MSIX_TABLE) != sizeof(table)) {
            return -1;
        }

        ctrl = le16_to_cpu(ctrl);
        table = le32_to_cpu(table);

        bar = table & PCI_MSIX_BIR;
        offset = table & ~PCI_MSIX_BIR;
        entries = (ctrl & PCI_MSIX_TABSIZE) + 1;

        vdev->resources[bar].msix = true;
        vdev->resources[bar].msix_offset = offset;

        DPRINTF("%04x:%02x:%02x.%x PCI MSI-X CAP @0x%x, BAR %d, offset 0x%x\n",
                vdev->host.seg, vdev->host.bus, vdev->host.dev,
                vdev->host.func, pos, bar, offset);

        len = table & PCI_MSIX_BIR;
        if (ioctl(vdev->vfiofd, VFIO_GET_BAR_LEN, &len)) {
            fprintf(stderr, "vfio: VFIO_GET_BAR_LEN failed for MSIX BAR\n");
            return -1;
        }

        if (msix_init(&vdev->pdev, entries, bar, len) < 0) {
            fprintf(stderr, "vfio: msix_init failed\n");
            return -1;
        }
    }
    return 0;
}

static void vfio_teardown_msi(VFIODevice *vdev)
{
    msi_uninit(&vdev->pdev);
    msix_uninit(&vdev->pdev);
}

/*
 * Resource setup
 */
static int vfio_map_resources(VFIODevice *vdev)
{
    int i;

    for (i = 0; i < PCI_ROM_SLOT; i++) {
        PCIResource *res;
        uint64_t len;
        uint32_t bar;
        uint8_t offset;
        int ret, space;

        res = &vdev->resources[i];
        res->vfiofd = vdev->vfiofd;
        res->bar = len = i;

        if (ioctl(vdev->vfiofd, VFIO_GET_BAR_LEN, &len)) {
            fprintf(stderr, "vfio: VFIO_GET_BAR_LEN failed for BAR %d (%s)\n",
                    i, strerror(errno));
            return -1;
        }
        if (!len) {
            continue;
        }

        offset = PCI_BASE_ADDRESS_0 + (4 * i);
        ret = pread(vdev->vfiofd, &bar, sizeof(bar),
                    VFIO_PCI_CONFIG_OFF + offset);
        if (ret != sizeof(bar)) {
            fprintf(stderr, "vfio: Failed to read BAR %d (%s)\n",
                    i, strerror(errno));
            return -1;
        }
        bar = le32_to_cpu(bar);
        space = bar & PCI_BASE_ADDRESS_SPACE;

        if (space == PCI_BASE_ADDRESS_SPACE_MEMORY && !(len & 0xfff)) {
            /* Page aligned MMIO BARs - direct map */
            int off = VFIO_PCI_BAR0_RESOURCE + i;
            int prot = PROT_READ | PROT_WRITE;
            char name[32];

            res->mem = true;
            res->size = len;

            if (vdev->pdev.qdev.info->vmsd) {
                snprintf(name, sizeof(name), "%s.bar%d",
                         vdev->pdev.qdev.info->vmsd->name, i);
            } else {
                snprintf(name, sizeof(name), "%s.bar%d",
                         vdev->pdev.qdev.info->name, i);
            }

            if (res->msix) {
                if (res->msix_offset) {
                    char *c = &name[strlen(name)];

                    res->r_virtbase[0] = mmap(NULL, res->msix_offset, prot,
                                              MAP_SHARED, vdev->vfiofd,
                                              vfio_pci_space_to_offset(off));

                    if (res->r_virtbase[0] == MAP_FAILED) {
                        fprintf(stderr, "vfio: Failed to mmap BAR %d.0 (%s)\n",
                                i, strerror(errno));
                        return -1;
                    }
                    strncat(name, ".0", sizeof(name));
                    res->memory_index[0] =
                        qemu_ram_alloc_from_ptr(&vdev->pdev.qdev,
                                                name, res->msix_offset,
                                                res->r_virtbase[0]);
                    *c = 0;
                }
                if (len > res->msix_offset + MSIX_PAGE_SIZE) {
                    char *c = &name[strlen(name)];

                    res->r_virtbase[1] = mmap(NULL,
                                        len - res->msix_offset - MSIX_PAGE_SIZE,
                                        prot, MAP_SHARED, vdev->vfiofd,
                                        vfio_pci_space_to_offset(off) +
                                        res->msix_offset + MSIX_PAGE_SIZE);

                    if (res->r_virtbase[1] == MAP_FAILED) {
                        fprintf(stderr, "vfio: Failed to mmap BAR %d.1 (%s)\n",
                                i, strerror(errno));
                        return -1;
                    }
                    strncat(name, ".1", sizeof(name));
                    res->memory_index[1] =
                        qemu_ram_alloc_from_ptr(&vdev->pdev.qdev, name,
                                        len - MSIX_PAGE_SIZE - res->msix_offset,
                                        res->r_virtbase[1]);
                    *c = 0;
                }
            } else {
                res->r_virtbase[0] = mmap(NULL, len, prot, MAP_SHARED,
                                          vdev->vfiofd,
                                          vfio_pci_space_to_offset(off));

                if (res->r_virtbase[0] == MAP_FAILED) {
                    fprintf(stderr, "vfio: Failed to mmap BAR %d (%s)\n",
                            i, strerror(errno));
                    return -1;
                }
                res->memory_index[0] =
                    qemu_ram_alloc_from_ptr(&vdev->pdev.qdev,
                                            name, len, res->r_virtbase[0]);
            }

            pci_register_bar(&vdev->pdev, i, res->size,
                             bar & PCI_BASE_ADDRESS_MEM_PREFETCH ?
                             PCI_BASE_ADDRESS_MEM_PREFETCH :
                             PCI_BASE_ADDRESS_SPACE_MEMORY,
                             vfio_iomem_map);

            if (bar & PCI_BASE_ADDRESS_MEM_TYPE_64) {
                i++;
            }
        } else if (space == PCI_BASE_ADDRESS_SPACE_MEMORY) {
            /* Non-page aligned MMIO - slow map */

            /* Note that we could still mmap and do reads/writes from the
             * mmap'd region in qemu.  For now we do pread/pwrite to
             * exercise that path in VFIO. */

            res->mem = true;
            res->size = len;
            res->slow = true;

            DPRINTF("%s(%04x:%02x:%02x.%x) Using slow mapping for BAR %d\n",
                    __FUNCTION__, vdev->host.seg, vdev->host.bus,
                    vdev->host.dev, vdev->host.func, i);

            res->io_mem = cpu_register_io_memory(vfio_resource_reads,
                                                 vfio_resource_writes,
                                                 res, DEVICE_NATIVE_ENDIAN);

            pci_register_bar(&vdev->pdev, i, res->size,
                             bar & PCI_BASE_ADDRESS_MEM_PREFETCH ?
                             PCI_BASE_ADDRESS_MEM_PREFETCH :
                             PCI_BASE_ADDRESS_SPACE_MEMORY,
                             vfio_iomem_map);

            if (bar & PCI_BASE_ADDRESS_MEM_TYPE_64) {
                i++;
            }
        } else if (space == PCI_BASE_ADDRESS_SPACE_IO) {
            res->size = len;
            pci_register_bar(&vdev->pdev, i, res->size,
                             PCI_BASE_ADDRESS_SPACE_IO, vfio_ioport_map);
        }
        res->valid = true;
    }
    return 0;
}

static void vfio_unmap_resources(VFIODevice *vdev)
{
    int i;
    PCIResource *res = vdev->resources;

    for (i = 0; i < PCI_ROM_SLOT; i++, res++) {
        if (res->valid && res->mem) {
            if (res->msix) {
                if (res->msix_offset) {
                    cpu_register_physical_memory(res->e_phys, res->msix_offset,
                                                 IO_MEM_UNASSIGNED);
                    qemu_ram_free_from_ptr(res->memory_index[0]);
                    munmap(res->r_virtbase[0], res->msix_offset);
                }
                if (res->size > res->msix_offset + MSIX_PAGE_SIZE) {
                    cpu_register_physical_memory(res->e_phys + MSIX_PAGE_SIZE +
                                                 res->msix_offset,
                                                 res->e_size - MSIX_PAGE_SIZE -
                                                 res->msix_offset,
                                                 IO_MEM_UNASSIGNED);
                    qemu_ram_free_from_ptr(res->memory_index[1]);
                    munmap(res->r_virtbase[1],
                           res->size - MSIX_PAGE_SIZE - res->msix_offset);
                }
            } else {
                if (!res->slow) {
                    cpu_register_physical_memory(res->e_phys, res->e_size,
                                                 IO_MEM_UNASSIGNED);
                    qemu_ram_free_from_ptr(res->memory_index[0]);
                    munmap(res->r_virtbase[0], res->size);
                } else {
                    cpu_unregister_io_memory(res->io_mem);
                }
            }
        }
    }
}

/*
 * Netlink
 */
static QLIST_HEAD(, VFIODevice) nl_list = QLIST_HEAD_INITIALIZER(nl_list);
static struct nl_handle *vfio_nl_handle;
static int vfio_nl_family;

static void vfio_netlink_event(void *opaque)
{
    nl_recvmsgs_default(vfio_nl_handle);
}

static void vfio_remove_abort(void *opaque)
{
    VFIODevice *vdev = opaque;

    error_report("ERROR: Host requested removal of VFIO device "
                 "%04x:%02x:%02x.%x, guest did not respond.  Abort.\n",
                 vdev->host.seg, vdev->host.bus,
                 vdev->host.dev, vdev->host.func);
    abort();
}

static int vfio_parse_netlink(struct nl_msg *msg, void *arg)
{
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct sockaddr_nl *sockaddr = nlmsg_get_src(msg);
    struct genlmsghdr *genl;
    struct nlattr *attrs[VFIO_NL_ATTR_MAX + 1];
    VFIODevice *vdev = NULL;
    int cmd;
    u16 seg;
    u8 bus, dev, func;

    /* Filter out any messages not from the kernel */
    if (sockaddr->nl_pid != 0) {
        return 0;
    }

    genl = nlmsg_data(nlh);
    cmd = genl->cmd;        

    genlmsg_parse(nlh, 0, attrs, VFIO_NL_ATTR_MAX, NULL);

    if (!attrs[VFIO_ATTR_PCI_DOMAIN] || !attrs[VFIO_ATTR_PCI_BUS] ||
        !attrs[VFIO_ATTR_PCI_SLOT] || !attrs[VFIO_ATTR_PCI_FUNC]) {
        fprintf(stderr, "vfio: Invalid netlink message, no device info\n");
        return -1;
    }

    seg = nla_get_u16(attrs[VFIO_ATTR_PCI_DOMAIN]);
    bus = nla_get_u8(attrs[VFIO_ATTR_PCI_BUS]);
    dev = nla_get_u8(attrs[VFIO_ATTR_PCI_SLOT]);
    func = nla_get_u8(attrs[VFIO_ATTR_PCI_FUNC]);

    DPRINTF("Received command %d from netlink for device %04x:%02x:%02x.%x\n",
            cmd, seg, bus, dev, func);

    QLIST_FOREACH(vdev, &nl_list, nl_next) {
        if (seg == vdev->host.seg && bus == vdev->host.bus &&
            dev == vdev->host.dev && func == vdev->host.func) {
            break;
        }
    }

    if (!vdev) {
        return 0;
    }

    switch (cmd) {
    case VFIO_MSG_REMOVE:
        fprintf(stderr, "vfio: Host requests removal of device "
                "%04x:%02x:%02x.%x, sending unplug request to guest.\n",
                seg, bus, dev, func);

        qdev_unplug(&vdev->pdev.qdev);

        /* This isn't an optional request, give the guest some time to release
         * the device.  If it doesn't, we need to trigger a bigger hammer. */
        vdev->remove_timer = qemu_new_timer_ms(rt_clock,
                                               vfio_remove_abort, vdev);
        qemu_mod_timer(vdev->remove_timer,
                       qemu_get_clock_ms(rt_clock) + 30000);
        break;
    /* TODO: Handle errors & suspend/resume */
    }

    return 0;
}

static int vfio_register_netlink(VFIODevice *vdev)
{
    struct nl_msg *msg;

    if (QLIST_EMPTY(&nl_list)) {
        int fd;

        vfio_nl_handle = nl_handle_alloc();
        if (!vfio_nl_handle) {
            error_report("vfio: Failed nl_handle_alloc\n");
            return -1;
        }

        genl_connect(vfio_nl_handle);
        vfio_nl_family = genl_ctrl_resolve(vfio_nl_handle, "VFIO");
        if (vfio_nl_family < 0) {
            error_report("vfio: Failed to resolve netlink channel\n");
            nl_handle_destroy(vfio_nl_handle);
            return -1;
        }
        nl_disable_sequence_check(vfio_nl_handle);
        if (nl_socket_modify_cb(vfio_nl_handle, NL_CB_VALID, NL_CB_CUSTOM,
                                vfio_parse_netlink, NULL)) {
            error_report("vfio: Failed to modify netlink callback\n");
            nl_handle_destroy(vfio_nl_handle);
            return -1;
        }

        fd = nl_socket_get_fd(vfio_nl_handle);
        qemu_set_fd_handler(fd, vfio_netlink_event, NULL, vdev);
    }

    QLIST_INSERT_HEAD(&nl_list, vdev, nl_next);

    msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, vfio_nl_family, 0,
                NLM_F_REQUEST, VFIO_MSG_REGISTER, 1);
    nla_put_u64(msg, VFIO_ATTR_MSGCAP, 1ULL << VFIO_MSG_REMOVE);
    nla_put_u16(msg, VFIO_ATTR_PCI_DOMAIN, vdev->host.seg);
    nla_put_u8(msg, VFIO_ATTR_PCI_BUS, vdev->host.bus);
    nla_put_u8(msg, VFIO_ATTR_PCI_SLOT, vdev->host.dev);
    nla_put_u8(msg, VFIO_ATTR_PCI_FUNC, vdev->host.func);
    nl_send_auto_complete(vfio_nl_handle, msg);
    nlmsg_free(msg);

    return 0;
}

static void vfio_unregister_netlink(VFIODevice *vdev)
{
    if (qemu_timer_pending(vdev->remove_timer)) {
        qemu_del_timer(vdev->remove_timer);
        qemu_free_timer(vdev->remove_timer);
    }

    QLIST_REMOVE(vdev, nl_next);

    if (QLIST_EMPTY(&nl_list)) {
        int fd;

        fd = nl_socket_get_fd(vfio_nl_handle);
        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        nl_handle_destroy(vfio_nl_handle);
    }
}

/*
 * General setup
 */
static int enable_vfio(VFIODevice *vdev)
{
    if (vdev->vfiofd_name && strlen(vdev->vfiofd_name) > 0) {
        if (qemu_isdigit(vdev->vfiofd_name[0])) {
            vdev->vfiofd = strtol(vdev->vfiofd_name, NULL, 0);
            return 0;
        } else {
            vdev->vfiofd = monitor_get_fd(cur_mon, vdev->vfiofd_name);
            if (vdev->vfiofd < 0) {
                fprintf(stderr, "%s: (%s) unkown\n", __func__,
                        vdev->vfiofd_name);
                return -1;
            }
            return 0;
        }
    } else {
        char vfio_dir[64], vfio_dev[16];
        DIR *dir;
        struct dirent *de;

        sprintf(vfio_dir, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/vfio/",
                vdev->host.seg, vdev->host.bus,
                vdev->host.dev, vdev->host.func);
        dir = opendir(vfio_dir);
        if (!dir) {
            error_report("vfio: error: Driver not attached\n");
            return -1;
        }

        while ((de = readdir(dir))) {
            if (de->d_name[0] == '.')
                continue;
            if (!strncmp(de->d_name, "vfio", 4))
                break;
        }

        if (!de) {
            error_report("vfio: error: Cannot find vfio* in %s\n", vfio_dir);
            return -1;
        }

        sprintf(vfio_dev, "/dev/%s", de->d_name);
        vdev->vfiofd = open(vfio_dev, O_RDWR);
        if (vdev->vfiofd < 0) {
            error_report("pci-assign: vfio: Failed to open %s: %s\n",
                         vfio_dev, strerror(errno));
            return -1;
        }
        return 0;
    }
}

static void disable_vfio(VFIODevice *vdev)
{
    /* If we opened it, close it, otherwise leave it alone */
    if (!(vdev->vfiofd_name && strlen(vdev->vfiofd_name) > 0)) {
        close(vdev->vfiofd);
    }
}

static QLIST_HEAD(, VFIOUIOMMU)
    uiommu_list = QLIST_HEAD_INITIALIZER(uiommu_list);

static int enable_uiommu(VFIODevice *vdev)
{
    int fd;
    VFIOUIOMMU *uiommu;
    bool opened = false;

    if (vdev->uiommufd_name && strlen(vdev->uiommufd_name) > 0) {
        if (qemu_isdigit(vdev->uiommufd_name[0])) {
            fd = strtol(vdev->uiommufd_name, NULL, 0);
        } else {
            fd = monitor_get_fd(cur_mon, vdev->uiommufd_name);
            if (fd < 0) {
                fprintf(stderr, "%s: (%s) unkown\n", __func__,
                        vdev->uiommufd_name);
                return fd;
            }
        }
    } else if (vdev->flags & VFIO_FLAG_UIOMMU_SHARED &&
               !QLIST_EMPTY(&uiommu_list)) {
        fd = QLIST_FIRST(&uiommu_list)->fd;
    } else {
        fd = open("/dev/uiommu", O_RDONLY);
        if (fd < 0) {
            return -errno;
        }
        opened = true;
    }

    if (ioctl(vdev->vfiofd, VFIO_SET_UIOMMU_DOMAIN, &fd)) {
        fprintf(stderr, "%s: Failed VFIO_SET_UIOMMU_DOMAIN: %s\n",
                __FUNCTION__, strerror(errno));
        return -errno;
    }

    QLIST_FOREACH(uiommu, &uiommu_list, next) {
        if (uiommu->fd == fd) {
            break;
        }
    }

    if (!uiommu) {
        uiommu = qemu_mallocz(sizeof(*uiommu));

        uiommu->fd = fd;
        uiommu->opened = opened;
        QLIST_INSERT_HEAD(&uiommu_list, uiommu, next);
        QLIST_INIT(&uiommu->vdevs);
        /* When we register a physical memory client, we'll immediately get
         * a backlog of memory mappings.  Since these are registered via the
         * vfio device, we need to have at least one in the list before doing
         * the registration. */
        QLIST_INSERT_HEAD(&uiommu->vdevs, vdev, iommu_next);
        uiommu->client.set_memory = vfio_client_set_memory;
        uiommu->client.sync_dirty_bitmap = vfio_client_sync_dirty_bitmap;
        uiommu->client.migration_log = vfio_client_migration_log;
        cpu_register_phys_memory_client(&uiommu->client);
    } else {
        QLIST_INSERT_HEAD(&uiommu->vdevs, vdev, iommu_next);
    }

    vdev->uiommu = uiommu;

    return 0;
}

static void disable_uiommu(VFIODevice *vdev)
{
    VFIOUIOMMU *uiommu = vdev->uiommu;
    int fd = -1;
    
    ioctl(vdev->vfiofd, VFIO_SET_UIOMMU_DOMAIN, &fd);

    if (!uiommu)
        return;

    QLIST_REMOVE(vdev, iommu_next);
    vdev->uiommu = NULL;

    if (QLIST_EMPTY(&uiommu->vdevs)) {
        cpu_unregister_phys_memory_client(&uiommu->client);
        QLIST_REMOVE(uiommu, next);
        if (uiommu->opened) {
            close(uiommu->fd);
        }
        qemu_free(uiommu);
    }
}

static int vfio_load_rom(VFIODevice *vdev)
{
    uint64_t len, size = PCI_ROM_SLOT;
    char name[32];
    off_t off = 0, voff = vfio_pci_space_to_offset(VFIO_PCI_ROM_RESOURCE);
    ssize_t bytes;
    void *ptr;

    /* If loading ROM from file, pci handles it */
    if (vdev->pdev.romfile || !vdev->pdev.rom_bar)
        return 0;

    if (ioctl(vdev->vfiofd, VFIO_GET_BAR_LEN, &size)) {
        fprintf(stderr, "vfio: VFIO_GET_BAR_LEN failed for OPTION ROM");
        return -1;
    }

    if (!size)
        return 0;

    len = size;
    snprintf(name, sizeof(name), "%s.rom", vdev->pdev.qdev.info->name);
    vdev->pdev.rom_offset = qemu_ram_alloc(&vdev->pdev.qdev, name, size);
    ptr = qemu_get_ram_ptr(vdev->pdev.rom_offset);
    memset(ptr, 0xff, size);

    while (size) {
        bytes = pread(vdev->vfiofd, ptr + off, size, voff + off);
        if (bytes == 0) {
            break; /* expect that we could get back less than the ROM BAR */
        } else if (bytes > 0) {
            off += bytes;
            size -= bytes;
        } else {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "vfio: Error reading device ROM: %s\n",
                    strerror(errno));
            qemu_ram_free(vdev->pdev.rom_offset);
            vdev->pdev.rom_offset = 0;
            return -1;
        }
    }

    pci_register_bar(&vdev->pdev, PCI_ROM_SLOT, len, 0, pci_map_option_rom);
    return 0;
}

static int vfio_initfn(struct PCIDevice *pdev)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);
    char sys[64];
    struct stat st;
    int ret;

    /* Check that the host device exists */
    sprintf(sys, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/",
            vdev->host.seg, vdev->host.bus, vdev->host.dev, vdev->host.func);
    if (stat(sys, &st) < 0) {
        error_report("vfio: error: no such host device "
                     "%04x:%02x:%02x.%01x", vdev->host.seg, vdev->host.bus,
                     vdev->host.dev, vdev->host.func);
        return -1;
    }

    if (enable_vfio(vdev)) {
        return -1;
    }

    if (vfio_register_netlink(vdev)) {
        goto out_disable_vfiofd;
    }

    if (enable_uiommu(vdev)) {
        goto out_disable_netlink;
    }

    /* Get a copy of config space */
    ret = pread(vdev->vfiofd, vdev->pdev.config,
                pci_config_size(&vdev->pdev), VFIO_PCI_CONFIG_OFF);
    if (ret < pci_config_size(&vdev->pdev)) {
        fprintf(stderr, "vfio: Failed to read device config space\n");
        goto out_disable_uiommu;
    }

    /* Clear host resource mapping info.  If we choose not to register a
     * BAR, such as might be the case with the option ROM, we can get
     * confusing, unwritable, residual addresses from the host here. */
    memset(&vdev->pdev.config[PCI_BASE_ADDRESS_0], 0, 24);
    memset(&vdev->pdev.config[PCI_ROM_ADDRESS], 0, 4);

    vfio_load_rom(vdev);

    if (vfio_setup_msi(vdev))
        goto out_disable_uiommu;

    if (vfio_map_resources(vdev))
        goto out_disable_msi;

    if (vfio_enable_intx(vdev))
        goto out_unmap_resources;

    return 0;

out_unmap_resources:
    vfio_unmap_resources(vdev);
out_disable_msi:
    vfio_teardown_msi(vdev);
out_disable_uiommu:
    disable_uiommu(vdev);
out_disable_netlink:
    vfio_unregister_netlink(vdev);
out_disable_vfiofd:
    disable_vfio(vdev);
    return -1;
}

static int vfio_exitfn(struct PCIDevice *pdev)
{
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);

    vfio_disable_interrupts(vdev);
    vfio_teardown_msi(vdev);
    vfio_unmap_resources(vdev);
    disable_uiommu(vdev);
    vfio_unregister_netlink(vdev);
    disable_vfio(vdev);
    return 0;
}

static void vfio_reset(DeviceState *dev)
{
    PCIDevice *pdev = DO_UPCAST(PCIDevice, qdev, dev);
    VFIODevice *vdev = DO_UPCAST(VFIODevice, pdev, pdev);

    if (ioctl(vdev->vfiofd, VFIO_RESET_FUNCTION)) {
        fprintf(stderr, "vfio: Error unable to reset physical device "
                "(%04x:%02x:%02x.%x): %s\n", vdev->host.seg, vdev->host.bus,
                vdev->host.dev, vdev->host.func, strerror(errno));
    }
}

static PropertyInfo qdev_prop_hostaddr = {
    .name  = "pci-hostaddr",
    .type  = -1,
    .size  = sizeof(PCIHostDevice),
    .parse = parse_hostaddr,
    .print = print_hostaddr,
};

static PCIDeviceInfo vfio_info = {
    .qdev.name    = "vfio",
    .qdev.desc    = "pass through host pci devices to the guest via vfio",
    .qdev.size    = sizeof(VFIODevice),
    .qdev.reset   = vfio_reset,
    .init         = vfio_initfn,
    .exit         = vfio_exitfn,
    .config_read  = vfio_pci_read_config,
    .config_write = vfio_pci_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP("host", VFIODevice, host,
                    qdev_prop_hostaddr, PCIHostDevice),
        DEFINE_PROP_STRING("vfiofd", VFIODevice, vfiofd_name),
        DEFINE_PROP_STRING("uiommufd", VFIODevice, uiommufd_name),
        DEFINE_PROP_BIT("shared_uiommu_domain", VFIODevice, flags,
                        VFIO_FLAG_UIOMMU_SHARED_BIT, true),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void vfio_register_devices(void)
{
    pci_qdev_register(&vfio_info);
}

device_init(vfio_register_devices)
