// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "notify_manager.h"  // NOLINT: include directory

#include "utils.h"  // NOLINT: include directory

#include "notify_manager.tmh"  // NOLINT: include directory

namespace {
constexpr UINT32 kNotifyBlockMsixBaseIndex = 0;
}  // namespace

NotifyManager::~NotifyManager() { Release(); }

bool NotifyManager::Init(NDIS_HANDLE miniport_handle, UINT32 num_msi_vectors,
                         QueueConfig* tx_config, QueueConfig* rx_config) {
  NT_ASSERT(miniport_handle != nullptr);
  // We need at least 3 to hold one for mgt queue, one tx and one rx.
  NT_ASSERT(num_msi_vectors >= 3);
  // Verify init is called for the first time or only after Reset.
  NT_VERIFY(num_notify_blocks_ == 0);

  // Use the last interrupt vector for management.
  mgmt_msix_index_ = num_msi_vectors - 1;
  num_notify_blocks_ = num_msi_vectors - 1;

  tx_config_ = tx_config;
  rx_config_ = rx_config;

  UINT32 expected_notify_blocks =
      tx_config_->num_slices + rx_config_->num_slices;

  if (expected_notify_blocks > num_notify_blocks_) {
    // All Tx/Rx traffic class shares one notify block with same slice number.
    // Code calculates how many msi vectors each traffic class
    // can has and then adjust the max slices number accordingly.
    int vecs_per_queue = num_notify_blocks_ / 2;
    int vecs_left = num_notify_blocks_ % 2;

    // In addition to vecs_per_queue, distribute any vectors left across all rx
    // traffic class.
    UINT32 max_rx_slices_with_msi_vector = vecs_per_queue + vecs_left;
    rx_config_->num_slices =
        min(rx_config_->num_slices, max_rx_slices_with_msi_vector);

    // Give all the vectors left to tx.
    UINT32 max_tx_slices_with_msi_vector = vecs_per_queue;
    tx_config_->num_slices =
        min(tx_config_->num_slices, max_tx_slices_with_msi_vector);

    DEBUGP(
        GVNIC_WARNING,
        "[%s] WARNING: Do not have desired msix %u, only enabled %u, adjusting "
        "tx slices to %u and rx slices to %u",
        __FUNCTION__, expected_notify_blocks, num_notify_blocks_,
        tx_config_->num_slices, rx_config_->num_slices);
  }

  // we allocate one more block to have buffer to do cache line alignment.
  if (!dma_notify_blocks_.Allocate(miniport_handle, num_notify_blocks_ + 1)) {
    return false;
  }

  UINT cacheline_offset = GetCacheAlignOffset(
      dma_notify_blocks_.physical_address().QuadPart, kCacheLineSize);

  // Move the virtual address by offset bytes.
  notify_blocks_ = reinterpret_cast<NotifyBlock*>(
      reinterpret_cast<char*>(dma_notify_blocks_.virtual_address()) +
      cacheline_offset);

  notify_blocks_physical_address_ =
      dma_notify_blocks_.physical_address().QuadPart + cacheline_offset;

  DEBUGP(GVNIC_INFO,
         "[%s] Get allocated notify blocks with physical addr %#llx, "
         "calculated offset %#x, adjusted physical addr %#llx",
         __FUNCTION__, dma_notify_blocks_.physical_address().QuadPart,
         cacheline_offset, notify_blocks_physical_address_);

  for (UINT i = 0; i < num_notify_blocks_; i++) {
    notify_blocks_[i].num_rx_rings = 0;
    notify_blocks_[i].rx_rings =
        AllocateMemory<RxRing*>(miniport_handle, rx_config_->num_traffic_class);
    if (notify_blocks_[i].rx_rings == nullptr) {
      return false;
    }
  }

  return true;
}

void NotifyManager::Release() {
  num_notify_blocks_ = 0;
  mgmt_msix_index_ = 0;
  tx_config_ = nullptr;
  rx_config_ = nullptr;

  for (UINT i = 0; i < num_notify_blocks_; i++) {
    FreeMemory(notify_blocks_[i].rx_rings);
  }

  dma_notify_blocks_.Release();
}

UINT32 NotifyManager::RegisterRxRing(UINT32 slice_num, RxRing* rx_ring) {
  UINT32 notify_id = GetRxNotifyId(slice_num);
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);

  NT_ASSERT(notify_block->num_rx_rings + 1 <= rx_config_->num_traffic_class);

  UINT32 num_rx_ring = notify_block->num_rx_rings;
  notify_block->rx_rings[num_rx_ring] = rx_ring;
  notify_block->num_rx_rings++;

  return notify_id;
}

UINT32 NotifyManager::RegisterTxRing(UINT32 slice_num, TxRing* tx_ring) {
  UINT32 notify_id = GetTxNotifyId(slice_num);
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);

  NT_ASSERT(notify_block->tx_ring == nullptr);

  notify_block->tx_ring = tx_ring;

  return notify_id;
}

void NotifyManager::Reset() {
  for (UINT i = 0; i < num_notify_blocks_; i++) {
    for (UINT j = 0; j < notify_blocks_[i].num_rx_rings; j++) {
      notify_blocks_[i].rx_rings[j] = nullptr;
    }

    notify_blocks_[i].num_rx_rings = 0;
    notify_blocks_[i].tx_ring = nullptr;
  }
}

UINT32 NotifyManager::GetRxRingCount(UINT32 notify_id) const {
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);

  return notify_block->num_rx_rings;
}

RxRing* NotifyManager::GetRxRing(UINT32 notify_id, UINT32 ring_idx) const {
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);

  NT_ASSERT(ring_idx < notify_block->num_rx_rings);
  return notify_block->rx_rings[ring_idx];
}

TxRing* NotifyManager::GetTxRing(UINT32 notify_id) const {
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);
  return notify_block->tx_ring;
}

UINT32 NotifyManager::notify_block_msi_base_index() const {
  return kNotifyBlockMsixBaseIndex;
}

UINT32 NotifyManager::GetInterruptDoorbellIndex(UINT32 notify_id) const {
  NotifyBlock* notify_block = GetNotifyBlock(notify_id);
  return RtlUlongByteSwap(notify_block->irq_db_index);
}

UINT32 NotifyManager::GetTxNotifyId(UINT32 slice_num) {
  NT_ASSERT(num_notify_blocks_);
  return kNotifyBlockMsixBaseIndex + slice_num;
}

UINT32 NotifyManager::GetRxNotifyId(UINT32 slice_num) {
  NT_ASSERT(num_notify_blocks_);
  return kNotifyBlockMsixBaseIndex + tx_config_->num_slices + slice_num;
}

NotifyBlock* NotifyManager::GetNotifyBlock(UINT notify_id) const {
  NT_ASSERT(notify_id < num_notify_blocks_);
  return &notify_blocks_[notify_id];
}
