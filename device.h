// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_DEVICE_H_INCLUDED
#define TTDRIVER_DEVICE_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/reboot.h>
#include <linux/kref.h>

#include "ioctl.h"
#include "hwmon.h"

// Tensix DMA refers to the ability for Tensix and Ethernet tiles on the SoC to
// access the system bus via NOC transactions to the PCIE tile.  On GS, this
// region corresponds to 0x0000'0000 - 0xFFFD'FFFF in the PCIE tile's address
// space.  On WH, it corresponds to 0x8'0000'0000 - 0x8'FFFD'FFFF.  From the
// perspective of userspace this region is 4GiB with an empty 128KiB at the top.
// However, an extra 256MiB is allocated for alignment purposes.  Experiments
// with the GS hardware indicate that the iATU will mistranslate accesses if the
// target offset is not aligned to the region's size.  Since we need 4GiB, and
// and 4GiB/16 available iATU regions = 256MiB, we allocate an extra 256MiB to
// ensure that the low 28 bits of the target offset are zero.
#define TENSIX_DMA_ALLOC_SIZE 0x110000000ULL
#define TENSIX_DMA_SIZE 0x100000000ULL


#define RESERVED_MEM_SIZE 0x200000000ULL	/* 8G buffer */
#define RESERVED_MEM_BASE 0x280000000ULL	/* 10G in */

struct tenstorrent_device_class;

struct tenstorrent_device {
	struct kref kref;

	struct device dev;
	struct cdev chardev;
	struct pci_dev *pdev;
	const struct tenstorrent_device_class *dev_class;

	unsigned int ordinal;
	bool dma_capable;
	bool interrupt_enabled;

	struct mutex chardev_mutex;
	unsigned int chardev_open_count;

	struct notifier_block reboot_notifier;

	DECLARE_BITMAP(resource_lock, TENSTORRENT_RESOURCE_LOCK_COUNT);

	struct tt_hwmon_context hwmon_context;

	dma_addr_t tensix_dma_addr;
	// void *tensix_dma_virt;
	void __iomem *tensix_dma;

	// dma_addr_t tensix_dma_addr_aligned;
	// void *tensix_dma_virt_aligned;
};

struct tenstorrent_device_class {
	const char *name;
	u32 instance_size;
	bool (*init_device)(struct tenstorrent_device *ttdev);
	bool (*init_hardware)(struct tenstorrent_device *ttdev);
	void (*cleanup_device)(struct tenstorrent_device *ttdev);
	void (*first_open_cb)(struct tenstorrent_device *ttdev);
	void (*last_release_cb)(struct tenstorrent_device *ttdev);
	void (*reboot)(struct tenstorrent_device *ttdev);
};

void tenstorrent_device_put(struct tenstorrent_device *);

#endif
