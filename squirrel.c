// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/types.h>

#include "squirrel.h"
#include "pcie.h"
#include "module.h"
#include "hwmon.h"


static bool squirrel_init(struct tenstorrent_device *tt_dev) {
	struct squirrel_device *wh_dev = tt_dev_to_sq_dev(tt_dev);

	return true;
}


static bool squirrel_init_hardware(struct tenstorrent_device *tt_dev) {
	return true;
}

static bool squirrel_post_hardware_init(struct tenstorrent_device *tt_dev) {
	return true;
}

static void squirrel_cleanup_hardware(struct tenstorrent_device *tt_dev) {
}

static void squirrel_cleanup(struct tenstorrent_device *tt_dev) {
	struct squirrel_device *sq_dev = tt_dev_to_sq_dev(tt_dev);
}

struct tenstorrent_device_class squirrel_class = {
	.name = "Squirrel",
	.instance_size = sizeof(struct squirrel_device),
	.init_device = squirrel_init,
	.init_hardware = squirrel_init_hardware,
	.post_hardware_init = squirrel_post_hardware_init,
	.cleanup_hardware = squirrel_cleanup_hardware,
	.cleanup_device = squirrel_cleanup,
	.reboot = squirrel_cleanup_hardware,
};
