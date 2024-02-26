// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "topology.h"

static unsigned long eth_addr_hash(const struct eth_addr_t *addr) {
	u64 combined = ((u64)addr->rack_x << 24) | ((u64)addr->rack_y << 16) |
				   ((u64)addr->shelf_x << 8) | (u64)addr->shelf_y;
	return hash_64(combined, TOPOLOGY_HASH_BITS);
}

void topology_init(struct topology_t *topology)
{
	hash_init(topology->table);
}

void topology_destroy(struct topology_t *topology)
{
	struct chip_connections_t *entry;
	struct hlist_node *tmp;
	unsigned bkt;

	hash_for_each_safe(topology->table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

void topology_enumerate(struct topology_t *topology)
{
	struct chip_connections_t *entry;
	unsigned bkt;
	u32 i;

	pr_info("Topology:\n");
	hash_for_each(topology->table, bkt, entry, node) {
		pr_info("%lu\n", entry->num_active_links);
		for (i = 0; i < entry->num_active_links; i++) {
			pr_info("(%d,%d,%d,%d):%d <-> (%d,%d,%d,%d):%d\n",
				entry->local.rack_x, entry->local.rack_y, entry->local.shelf_x, entry->local.shelf_y, entry->local_channels[i],
				entry->peers[i].rack_x, entry->peers[i].rack_y, entry->peers[i].shelf_x, entry->peers[i].shelf_y, entry->remote_channels[i]);
		}
	}
}

struct chip_connections_t *topology_add_chip(struct topology_t *topology, const struct eth_addr_t *chip)
{
	struct chip_connections_t *entry;

	entry = topology_find_chip_connections(topology, chip);
	if (entry)
		return entry;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);

	if (!entry) {
		pr_err("Failed to allocate memory for chip connection entry\n");
		return NULL;
	}

	entry->local = *chip;
	entry->num_active_links = 0;

	hash_add(topology->table, &entry->node, eth_addr_hash(chip));

	return entry;
}

struct chip_connections_t *topology_find_chip_connections(struct topology_t *topology, const struct eth_addr_t *chip)
{
	struct chip_connections_t *entry;

	hash_for_each_possible(topology->table, entry, node, eth_addr_hash(chip)) {
		if (memcmp(&entry->local, chip, sizeof(*chip)) == 0) {
			return entry;
		}
	}

	return NULL;
}

void topology_add_chip_peer(struct topology_t *topology, const struct eth_addr_t *local, const struct eth_addr_t *remote,
	u32 local_core, u32 remote_core)
{
	struct chip_connections_t *entry;

	// If the local chip doesn't exist, add it.
	entry = topology_find_chip_connections(topology, local);
	if (!entry)
		entry = topology_add_chip(topology, local);

	if (!entry) {
		pr_err("Failed to add chip connection entry\n");
		return;
	}

	// A Wormhole can have a max of WH_ETH_CORE_COUNT chips connected to it.
	if (entry->num_active_links >= WH_ETH_CORE_COUNT) {
		pr_err("Too many chips connected to local chip\n");
		return;
	}

	// Track which ETH core on this chip is connected to which ETH core on the remote chip.
	// Note that two chips can have multiple links between them.
	entry->local_channels[entry->num_active_links] = local_core;
	entry->remote_channels[entry->num_active_links] = remote_core;
	entry->peers[entry->num_active_links] = *remote;
	entry->num_active_links++;
}

