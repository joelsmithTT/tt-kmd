// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_ETH_H_INCLUDED
#define TTDRIVER_ETH_H_INCLUDED

#include <linux/types.h>

#define WH_ETH_CORE_COUNT 16

#define MAX_TOPOLOGY_SIZE_CHIPS 0xffff
#define ETH_HASH_BITS 8

struct wormhole_device;

// eth_addr_t is a unique identifier for a chip in a topology.
struct eth_addr_t {
    u8 rack_x;
    u8 rack_y;
    u8 shelf_x;
    u8 shelf_y;
};

struct connected_eth_core {
    u32 core_num;
    u32 fw_version;

    struct eth_addr_t local;
    struct eth_addr_t remote;

    u32 remote_noc_x;
    u32 remote_noc_y;

    u32 local_noc_x;
    u32 local_noc_y;
};

void wormhole_eth_probe(struct wormhole_device *wh_dev);
bool wormhole_eth_read32(struct wormhole_device *wh_dev, u32 eth_core, u64 sys_addr, u16 rack, u32 *value);
u32 maybe_remote_read32(struct wormhole_device *wh_dev, struct eth_addr_t *eth_addr, u32 noc_x, u32 noc_y, u64 addr);
#endif
