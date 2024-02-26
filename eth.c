// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "eth.h"
#include "wormhole.h"
#include "tlb.h"
#include "topology.h"

#define ETH_TIMEOUT_MS 250
#define ETH_MIN_FW_VERSION 0x6069000

#define ETH_FW_VERSION_ADDR 0x210
#define ETH_PORT_STATUS_ADDR 0x1200
#define ETH_NODE_INFO_ADDR 0x1100
#define ETH_LOCAL_RACK_SHELF_ADDR 0x1108
#define ETH_LOCAL_NOC0_X_ADDR 0x1110
#define ETH_LOCAL_NOC0_Y_ADDR 0x1118
#define ETH_REMOTE_RACK_ADDR 0x1128
#define ETH_REMOTE_SHELF_ADDR 0x1124
#define ETH_REQ_WR_PTR_ADDR 0x110a0
#define ETH_REQ_RD_PTR_ADDR 0x110b0
#define ETH_REQ_QUEUE_ADDR 0x110c0
#define ETH_RESP_RD_PTR_ADDR 0x11230
#define ETH_RESP_WR_PTR_ADDR 0x11220
#define ETH_RESP_WR_PTR_ADDR 0x11220
#define ETH_RESP_QUEUE_ADDR 0x11240

#define ETH_CMD_WR_REQ (0x1 << 0)
#define ETH_CMD_WR_ACK (0x1 << 1)
#define ETH_CMD_RD_REQ (0x1 << 2)
#define ETH_CMD_RD_DATA (0x1 << 3)

#define ETH_STATUS_UNKNOWN 0
#define ETH_STATUS_NOT_CONNECTED 1

static const u32 WH_ETH_NOC0_X[WH_ETH_CORE_COUNT] = { 9, 1, 8, 2, 7, 3, 6, 4, 9, 1, 8, 2, 7, 3, 6, 4 };
static const u32 WH_ETH_NOC0_Y[WH_ETH_CORE_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 6, 6, 6, 6, 6 };

struct node_info_t {
	uint32_t delimiter;
	uint32_t train_status;
	uint8_t my_rack_x;
	uint8_t my_rack_y;
	uint8_t my_chip_x;
	uint8_t my_chip_y;
	uint8_t root_node;
	uint8_t eth_node;
	uint8_t my_eth_id;
	uint8_t routing_node;
	uint32_t my_noc_x[2];
	uint32_t my_noc_y[2];
	uint64_t remote_eth_sys_addr;
	uint32_t remote_eth_rack;
};

struct eth_cmd_t {
	uint64_t sys_addr;
	uint32_t data;
	uint32_t flags;
	uint16_t rack;
	uint16_t src_resp_buf_index;
	uint32_t local_buf_index;
	uint8_t src_resp_q_id;
	uint8_t host_mem_txn_id;
	uint16_t padding;
	uint32_t src_addr_tag;
};

// TODO: pasta from wormhole.c
#define NOC_ADDR_NODE_ID_BITS     6
#define NOC_ADDR_LOCAL_BITS       36
static u64 encode_sys_addr(u32 chip_x, u32 chip_y, u32 noc_x, u32 noc_y, u64 offset) {
	u64 result = chip_y; // shelf
	u64 noc_addr_local_bits_mask = (1UL << NOC_ADDR_LOCAL_BITS) - 1;
	result <<= 6;
	result |= chip_x;
	result <<= 6;
	result |= noc_y;
	result <<= 6;
	result |= noc_x;
	result <<= NOC_ADDR_LOCAL_BITS;
	result |= (noc_addr_local_bits_mask & offset);
	return result;
}


static u32 noc_coords_to_eth_channel(u32 x, u32 y)
{
	u32 i;
	for (i = 0; i < WH_ETH_CORE_COUNT; i++) {
		if (WH_ETH_NOC0_X[i] == x && WH_ETH_NOC0_Y[i] == y)
			return i;
	}
	return 0;
}

static u32 eth_read_reg(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr)
{
	struct noc_addr_t noc_addr = {
		.addr = addr,
		.x = WH_ETH_NOC0_X[eth_idx],
		.y = WH_ETH_NOC0_Y[eth_idx],
	};
	u32 val;
	wh_tlb_noc_read32(&wh_dev->tt, tlb, &noc_addr, &val);
	return val;
}

static void eth_write_reg(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr, u32 value)
{
	struct noc_addr_t noc_addr = {
		.addr = addr,
		.x = WH_ETH_NOC0_X[eth_idx],
		.y = WH_ETH_NOC0_Y[eth_idx],
	};
	wh_tlb_noc_write32(&wh_dev->tt, tlb, &noc_addr, value);
}

static void eth_write_block(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr, const void *src,
			    size_t size)
{
	struct noc_addr_t noc_addr = {
		.addr = addr,
		.x = WH_ETH_NOC0_X[eth_idx],
		.y = WH_ETH_NOC0_Y[eth_idx],
	};
	wh_noc_write(&wh_dev->tt, tlb, &noc_addr, src, size);
}

static void eth_read_block(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr, void *dst, size_t size)
{
	struct noc_addr_t noc_addr = {
		.addr = addr,
		.x = WH_ETH_NOC0_X[eth_idx],
		.y = WH_ETH_NOC0_Y[eth_idx],
	};
	wh_noc_read(&wh_dev->tt, tlb, &noc_addr, dst, size);
}

static bool eth_queue_full(u32 wr, u32 rd)
{
	// The queues are 4 entries deep; valid pointer values are 0..7 inclusive.
	// The queue is full if the write pointer is 4 ahead of the read pointer.
	return (wr != rd) && ((wr & 3) == (rd & 3));
}

u32 wormhole_remote_read32(struct wormhole_device *wh_dev, u32 local_eth_core,
				struct eth_addr_t *eth_addr, u32 noc_x, u32 noc_y, u64 addr)
{
	u32 value;
	u64 sys_addr = encode_sys_addr(eth_addr->shelf_x, eth_addr->shelf_y, noc_x, noc_y, addr);
	u16 rack = ((u16)eth_addr->rack_y) << 8 | eth_addr->rack_x;
	wormhole_eth_read32(wh_dev, local_eth_core, sys_addr, rack, &value);
	return value;
}

// Read from noc_x, noc_y, addr on the chip specified by eth_addr.
// If the chip specified by eth_addr is the local chip, use wh_noc_read32.
// Otherwise, use wormhole_remote_read32.
u32 maybe_remote_read32(struct wormhole_device *wh_dev, struct eth_addr_t *eth_addr,
				u32 noc_x, u32 noc_y, u64 addr)
{
	// TODO: eth_read_reg ergonomics.
	u32 local_rack_shelf = eth_read_reg(wh_dev, NULL, 0, ETH_LOCAL_RACK_SHELF_ADDR);
	u32 local_rack_x = (local_rack_shelf >> 0) & 0xFF;
	u32 local_rack_y = (local_rack_shelf >> 8) & 0xFF;
	u32 local_shelf_x = (local_rack_shelf >> 16) & 0xFF;
	u32 local_shelf_y = (local_rack_shelf >> 24) & 0xFF;

	if (eth_addr->rack_x == local_rack_x && eth_addr->rack_y == local_rack_y &&
	    eth_addr->shelf_x == local_shelf_x && eth_addr->shelf_y == local_shelf_y)
		return wh_tlb_noc_xy_read32(&wh_dev->tt, noc_x, noc_y, addr, NULL);

	return wormhole_remote_read32(wh_dev, 0, eth_addr, noc_x, noc_y, addr);
}

struct simple_stack {
	size_t stack_size;
};

#define MAX_ETH_ADDRS 0xffff
static void discover_topology(struct wormhole_device *wh_dev)
{
	struct eth_addr_t *stack;
	size_t stack_size;
	struct topology_t topology;
	struct tlb_t *tlb;
	u32 rack_shelf;

	tlb = tlb_alloc(&wh_dev->tlb_pool);
	if (!tlb)
		return;

	stack = kzalloc(MAX_ETH_ADDRS * sizeof(struct eth_addr_t), GFP_KERNEL);

	if (!stack) {
		tlb_free(tlb);
		return;
	}

	// Determine the eth_addr of the local chip, and push it onto the stack.
	rack_shelf = eth_read_reg(wh_dev, tlb, 0, ETH_LOCAL_RACK_SHELF_ADDR);
	tlb_free(tlb);
	stack[0].rack_x = (rack_shelf >> 0) & 0xFF;
	stack[0].rack_y = (rack_shelf >> 8) & 0xFF;
	stack[0].shelf_x = (rack_shelf >> 16) & 0xFF;
	stack[0].shelf_y = (rack_shelf >> 24) & 0xFF;
	stack_size = 1;

	// Start the topology discovery process.
	topology_init(&topology);
	while (stack_size > 0) {
		struct eth_addr_t ea;
		struct eth_addr_t *eth_addr;
		u32 eth_core;

		// Pop the stack to get an eth_addr to explore.
		stack_size--;
		eth_addr = &ea;
		ea = stack[stack_size];
		stack[stack_size] = (struct eth_addr_t){0};

		topology_add_chip(&topology, eth_addr);

		// Look at all 16 neighbors of the chip at eth_addr.
		// Do this by examining the ETH cores for this chip.
		// They will indicate whether there is another chip on the other side.
		for (eth_core = 0; eth_core < WH_ETH_CORE_COUNT; eth_core++) {
			struct eth_addr_t peer;
			u32 noc_x = WH_ETH_NOC0_X[eth_core];
			u32 noc_y = WH_ETH_NOC0_Y[eth_core];
			u32 fw_version = maybe_remote_read32(wh_dev, eth_addr, noc_x, noc_y, ETH_FW_VERSION_ADDR);
			u32 port_status = maybe_remote_read32(wh_dev, eth_addr, noc_x, noc_y, ETH_PORT_STATUS_ADDR + (eth_core * 4));
			u32 remote_rack = maybe_remote_read32(wh_dev, eth_addr, noc_x, noc_y, ETH_REMOTE_RACK_ADDR);
			u32 remote_shelf = maybe_remote_read32(wh_dev, eth_addr, noc_x, noc_y, ETH_REMOTE_SHELF_ADDR);
			u32 remote_noc_x = (remote_shelf >> 4) & 0x3F;
			u32 remote_noc_y = (remote_shelf >> 10) & 0x3F;
			u32 remote_channel = noc_coords_to_eth_channel(remote_noc_x, remote_noc_y);
			if (fw_version < ETH_MIN_FW_VERSION)
				continue;

			if (port_status == ETH_STATUS_UNKNOWN || port_status == ETH_STATUS_NOT_CONNECTED)
				continue;

			// There's something connected on the other end
			// Add it to the array of peers for this eth_addr.
			peer.rack_x = (remote_rack >> 0) & 0xFF;
			peer.rack_y = (remote_rack >> 8) & 0xFF;
			peer.shelf_x = (remote_shelf >> 16) & 0x3F;
			peer.shelf_y = (remote_shelf >> 22) & 0x3F;
			topology_add_chip_peer(&topology, eth_addr, &peer, eth_core, remote_channel);

			// If we haven't explored peer's peers yet, push peer onto the stack
			// so that we explore it in a subsequent iteration.
			if (topology_find_chip_connections(&topology, &peer) == NULL) {
				if (stack_size < MAX_ETH_ADDRS) {
					// Add peer to the topology so we won't push it onto the
					// stack again if this chip has multiple connections to it.
					topology_add_chip(&topology, &peer);
					stack[stack_size] = peer;
					stack_size++;
				} else {
					pr_err("Stack overflow prevented during topology discovery\n");
				}
			}
		}
	}

	kfree(stack);
	topology_destroy(&topology);
}

void wormhole_eth_probe(struct wormhole_device *wh_dev)
{
	struct tlb_t *tlb;
	u32 i;

	wh_dev->num_connected_cores = 0;
	tlb = tlb_alloc(&wh_dev->tlb_pool);

	if (!tlb)
		return;

	for (i = 0; i < WH_ETH_CORE_COUNT; i++) {
		struct connected_eth_core *core_info = &wh_dev->connected_eth_cores[wh_dev->num_connected_cores];
		u32 fw_version = eth_read_reg(wh_dev, tlb, i, ETH_FW_VERSION_ADDR);
		u32 port_status = eth_read_reg(wh_dev, tlb, i, ETH_PORT_STATUS_ADDR + (i * 4));
		u32 remote_rack = eth_read_reg(wh_dev, tlb, i, ETH_REMOTE_RACK_ADDR);
		u32 remote_shelf = eth_read_reg(wh_dev, tlb, i, ETH_REMOTE_SHELF_ADDR);
		u32 local_noc_x = eth_read_reg(wh_dev, tlb, i, ETH_LOCAL_NOC0_X_ADDR);
		u32 local_noc_y = eth_read_reg(wh_dev, tlb, i, ETH_LOCAL_NOC0_Y_ADDR);
		u32 rack_shelf = eth_read_reg(wh_dev, tlb, i, ETH_LOCAL_RACK_SHELF_ADDR);
		u32 local_rack_x = (rack_shelf >> 0) & 0xFF;
		u32 local_rack_y = (rack_shelf >> 8) & 0xFF;
		u32 local_shelf_x = (rack_shelf >> 16) & 0xFF;
		u32 local_shelf_y = (rack_shelf >> 24) & 0xFF;

		if (fw_version < ETH_MIN_FW_VERSION)
			continue;

		if (port_status == ETH_STATUS_UNKNOWN || port_status == ETH_STATUS_NOT_CONNECTED)
			continue;


		core_info->core_num = i;
		core_info->fw_version = fw_version;

		core_info->local.rack_x = local_rack_x;
		core_info->local.rack_y = local_rack_y;
		core_info->local.shelf_x = local_shelf_x;
		core_info->local.shelf_y = local_shelf_y;
		core_info->local_noc_x = local_noc_x;
		core_info->local_noc_y = local_noc_y;

		core_info->remote.rack_x = (remote_rack >> 0) & 0xFF;
		core_info->remote.rack_y = (remote_rack >> 8) & 0xFF;
		core_info->remote.shelf_x = (remote_shelf >> 16) & 0x3F;
		core_info->remote.shelf_y = (remote_shelf >> 22) & 0x3F;
		core_info->remote_noc_x = (remote_shelf >> 4) & 0x3F;
		core_info->remote_noc_y = (remote_shelf >> 10) & 0x3F;

		wh_dev->num_connected_cores++;
	}

	tlb_free(tlb);
	discover_topology(wh_dev);
}

bool wormhole_eth_read32(struct wormhole_device *wh_dev, u32 eth_core, u64 sys_addr, u16 rack, u32 *value)
{
	struct eth_cmd_t cmd;
	struct tlb_t *tlb;
	unsigned long timeout;
	u32 req_rd, req_wr, resp_rd, resp_wr; // queue pointers; 0 <= ptr < 8
	u32 req_slot, resp_slot; // queue indices; 0 <= slot < 4
	u32 req_offset;
	u32 resp_offset;
	u32 resp_flags_offset;
	u32 resp_data_offset;
	u32 fw_version;
	u32 resp_flags = 0;

	tlb = tlb_alloc(&wh_dev->tlb_pool);
	if (!tlb)
		return false;

	fw_version = eth_read_reg(wh_dev, tlb, eth_core, ETH_FW_VERSION_ADDR);
	if (fw_version < ETH_MIN_FW_VERSION) {
		pr_err("ETH FW version: %u is too old.\n", fw_version);
		goto wormhole_eth_read32_done;
	}

	// Read the current position of the read and write pointers for both the
	// request and response queues.
	req_wr = eth_read_reg(wh_dev, tlb, eth_core, ETH_REQ_WR_PTR_ADDR);
	req_rd = eth_read_reg(wh_dev, tlb, eth_core, ETH_REQ_RD_PTR_ADDR);
	resp_wr = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_WR_PTR_ADDR);
	resp_rd = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_RD_PTR_ADDR);

	// Encode the command.
	memset(&cmd, 0, sizeof(struct eth_cmd_t));
	cmd.sys_addr = sys_addr;
	cmd.data = sizeof(u32);
	cmd.rack = rack;
	cmd.flags = ETH_CMD_RD_REQ;

	if (eth_queue_full(req_wr, req_rd)) {
		pr_err("ETH queue %u full\n", eth_core);
		goto wormhole_eth_read32_done;
	}

	// Write the request to the slot in the request queue.
	req_slot = req_wr & 3;
	req_offset = req_slot * sizeof(struct eth_cmd_t);
	eth_write_block(wh_dev, tlb, eth_core, ETH_REQ_QUEUE_ADDR + req_offset, &cmd, sizeof(struct eth_cmd_t));

	// Write the request write pointer.
	req_wr = (req_wr + 1) & 0x7;
	eth_write_reg(wh_dev, tlb, eth_core, ETH_REQ_WR_PTR_ADDR, req_wr);

	// UMD says,
	// 	erisc firmware will:
	// 1. clear response flags
	// 2. start operation
	// 3. advance response wrptr
	// 4. complete operation and write data into response or buffer
	// 5. set response flags

	// Busy wait until the response write pointer changes.
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while (resp_wr == eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_WR_PTR_ADDR)) {
		if (time_after(jiffies, timeout)) {
			pr_err("ETH response timeout\n");
			break;
		}
	}

	// Busy wait until flags are set.
	resp_slot = resp_rd & 3; // 0 <= resp_slot < 4
	resp_offset = resp_slot * sizeof(struct eth_cmd_t);
	resp_flags_offset = resp_offset + offsetof(struct eth_cmd_t, flags);
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while ((resp_flags = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_QUEUE_ADDR + resp_flags_offset)) == 0) {
		if (time_after(jiffies, timeout)) {
			pr_err("ETH response timeout\n");
			break;
		}
	}

	// Read the response.
	resp_data_offset = resp_offset + offsetof(struct eth_cmd_t, data);
	*value = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_QUEUE_ADDR + resp_data_offset);

	// Increment/wrap/update the response read pointer.
	resp_rd = (resp_rd + 1) & 0x7;
	eth_write_reg(wh_dev, tlb, eth_core, ETH_RESP_RD_PTR_ADDR, resp_rd);

wormhole_eth_read32_done:
	tlb_free(tlb);

	// Response is only valid if we return true.
	return resp_flags == ETH_CMD_RD_DATA;
}
