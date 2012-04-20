#ifndef __VFIO_H__
#define __VFIO_H__

#include "qemu-common.h"
#include "qemu-queue.h"
#include "pci.h"
#include "ioapic.h"

typedef struct PCIHostDevice {
    uint16_t seg;
    uint8_t bus;
    uint8_t dev:5;
    uint8_t func:3;
} PCIHostDevice;

typedef struct PCIResource {
    off_t offset;
    int fd;
    MemoryRegion region;
    bool valid;
    bool mem;
    bool slow;
    size_t size;
    void *virtbase;
    uint8_t bar;
} PCIResource;

typedef struct INTx {
    bool pending;
    uint8_t pin;
    int irq;
    EventNotifier interrupt;
    Notifier eoi;
    Notifier update_irq;
} INTx;

struct VFIODevice;

typedef struct MSIVector {
    EventNotifier interrupt;
    struct VFIODevice *vdev;
    int vector;
} MSIVector;

enum {
    INT_NONE = 0,
    INT_INTx = 1,
    INT_MSI  = 2,
    INT_MSIX = 3,
};

struct VFIOGroup;

typedef struct VFIOIOMMU {
    int fd;
    CPUPhysMemoryClient client;
    QLIST_HEAD(, VFIOGroup) group_list;
} VFIOIOMMU;

typedef struct MSIXInfo {
    uint8_t bar;
    uint16_t entries;
    uint32_t offset;
    MemoryRegion region_lo;
    MemoryRegion region_hi;
    void *virtbase;
} MSIXInfo;

typedef struct VFIODevice {
    PCIDevice pdev;
    int fd;
    INTx intx;
    unsigned int config_size;
    off_t config_offset;
    unsigned int rom_size;
    off_t rom_offset;
    int msi_cap_size;
    MSIVector *msi_vectors;
    MSIXInfo *msix;
    int nr_vectors;
    int interrupt;
    PCIResource resources[PCI_NUM_REGIONS - 1]; /* No ROM */
    PCIHostDevice host;
    QLIST_ENTRY(VFIODevice) group_next;
    QEMUTimer *remove_timer;
    uint32_t flags;
    struct VFIOGroup *group;
    bool reset_works;
} VFIODevice;

typedef struct VFIOGroup {
    int fd;
    int groupid;
    VFIOIOMMU *iommu;
    QLIST_HEAD(, VFIODevice) device_list;
    QLIST_ENTRY(VFIOGroup) group_next;
    QLIST_ENTRY(VFIOGroup) iommu_next;
} VFIOGroup;

/* We can either create a domain per device or a domain per guest using
 * the uiommu interface.  By default we set this bit true to share an
 * iommu domain between devices for a guest.  This uses less resources
 * in the host and eliminates extra physical memory clients for us. */
#define VFIO_FLAG_UIOMMU_SHARED_BIT 0
#define VFIO_FLAG_UIOMMU_SHARED (1U << VFIO_FLAG_UIOMMU_SHARED_BIT)

#endif /* __VFIO_H__ */
