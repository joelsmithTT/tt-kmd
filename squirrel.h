// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_WORMHOLE_H_INCLUDED
#define TTDRIVER_WORMHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct squirrel_device {
	struct tenstorrent_device tt;
};

#define tt_dev_to_sq_dev(ttdev) \
	container_of((tt_dev), struct squirrel_device, tt)

#endif
