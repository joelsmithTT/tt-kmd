// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TOPOLOGY_H_INCLUDED
#define TTDRIVER_TOPOLOGY_H_INCLUDED

#include <linux/hashtable.h>
#include <linux/slab.h>
#include "eth.h"

#define TOPOLOGY_HASH_BITS 4

// topology_t represents the connectnedness of chips.
struct topology_t {
    DECLARE_HASHTABLE(table, TOPOLOGY_HASH_BITS);
};

// chip_connections_t represents all the remote chips from a single local chip's perspective.
struct chip_connections_t {
    struct eth_addr_t local;
    struct hlist_node node;     // keyed by `local`

    // On a per-ETH-core basis, track connection information.
    // Arrays are indexed by `num_active_links`.
    size_t num_active_links;
    u32 local_channels[WH_ETH_CORE_COUNT];
    u32 remote_channels[WH_ETH_CORE_COUNT];
    struct eth_addr_t peers[WH_ETH_CORE_COUNT];
};

void topology_init(struct topology_t *topology);
void topology_destroy(struct topology_t *topology);
void topology_enumerate(struct topology_t *topology);
struct chip_connections_t *topology_add_chip(struct topology_t *topology, const struct eth_addr_t *chip);
struct chip_connections_t *topology_find_chip_connections(struct topology_t *topology, const struct eth_addr_t *chip);
void topology_add_chip_peer(struct topology_t *topology, const struct eth_addr_t *local, const struct eth_addr_t *remote,
    u32 local_core, u32 remote_core);


#endif // TTDRIVER_TOPOLOGY_H_INCLUDED