////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
//
// Copyright (c) 2014-2020, Advanced Micro Devices, Inc. All rights reserved.
//
// Developed by:
//
//                 AMD Research and AMD HSA Software Development
//
//                 Advanced Micro Devices, Inc.
//
//                 www.amd.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include "core/inc/amd_aql_queue.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#include <stdio.h>
#include <string.h>

#include "core/inc/runtime.h"
#include "core/inc/amd_memory_region.h"
#include "core/inc/signal.h"
#include "core/inc/queue.h"
#include "core/util/utils.h"
#include "core/inc/registers.h"
#include "core/inc/interrupt_signal.h"
#include "core/inc/default_signal.h"
#include "core/inc/hsa_ext_amd_impl.h"
#include "core/inc/amd_gpu_pm4.h"

namespace rocr {
namespace AMD {
// Queue::amd_queue_ is cache-aligned for performance.
const uint32_t kAmdQueueAlignBytes = 0x40;

HsaEvent* AqlQueue::queue_event_ = nullptr;
std::atomic<uint32_t> AqlQueue::queue_count_(0);
KernelMutex AqlQueue::queue_lock_;
int AqlQueue::rtti_id_ = 0;

AqlQueue::AqlQueue(GpuAgent* agent, size_t req_size_pkts, HSAuint32 node_id, ScratchInfo& scratch,
                   core::HsaEventCallback callback, void* err_data, bool is_kv)
    : Queue(),
      LocalSignal(0, false),
      DoorbellSignal(signal()),
      ring_buf_(nullptr),
      ring_buf_alloc_bytes_(0),
      queue_id_(HSA_QUEUEID(-1)),
      active_(false),
      agent_(agent),
      queue_scratch_(scratch),
      errors_callback_(callback),
      errors_data_(err_data),
      is_kv_queue_(is_kv),
      pm4_ib_buf_(nullptr),
      pm4_ib_size_b_(0x1000),
      dynamicScratchState(0),
      exceptionState(0),
      suspended_(false),
      priority_(HSA_QUEUE_PRIORITY_NORMAL),
      exception_signal_(nullptr) {
  // When queue_full_workaround_ is set to 1, the ring buffer is internally
  // doubled in size. Virtual addresses in the upper half of the ring allocation
  // are mapped to the same set of pages backing the lower half.
  // Values written to the HW doorbell are modulo the doubled size.
  // This allows the HW to accept (doorbell == last_doorbell + queue_size).
  // This workaround is required for GFXIP 7 and GFXIP 8 ASICs.
  const core::Isa* isa = agent_->isa();
  queue_full_workaround_ =
      (isa->GetMajorVersion() == 7 || isa->GetMajorVersion() == 8)
          ? 1
          : 0;

  // Identify doorbell semantics for this agent.
  doorbell_type_ = agent->properties().Capability.ui32.DoorbellType;

  // Queue size is a function of several restrictions.
  const uint32_t min_pkts = ComputeRingBufferMinPkts();
  const uint32_t max_pkts = ComputeRingBufferMaxPkts();

  // Apply sizing constraints to the ring buffer.
  uint32_t queue_size_pkts = uint32_t(req_size_pkts);
  queue_size_pkts = Min(queue_size_pkts, max_pkts);
  queue_size_pkts = Max(queue_size_pkts, min_pkts);

  uint32_t queue_size_bytes = queue_size_pkts * sizeof(core::AqlPacket);
  if ((queue_size_bytes & (queue_size_bytes - 1)) != 0)
    throw AMD::hsa_exception(HSA_STATUS_ERROR_INVALID_QUEUE_CREATION,
                             "Requested queue with non-power of two packet capacity.\n");

  // Allocate the AQL packet ring buffer.
  AllocRegisteredRingBuffer(queue_size_pkts);
  if (ring_buf_ == nullptr) throw std::bad_alloc();
  MAKE_NAMED_SCOPE_GUARD(RingGuard, [&]() { FreeRegisteredRingBuffer(); });

  // Fill the ring buffer with invalid packet headers.
  // Leave packet content uninitialized to help track errors.
  for (uint32_t pkt_id = 0; pkt_id < queue_size_pkts; ++pkt_id) {
    (((core::AqlPacket*)ring_buf_)[pkt_id]).dispatch.header = HSA_PACKET_TYPE_INVALID;
  }

  // Zero the amd_queue_ structure to clear RPTR/WPTR before queue attach.
  memset(&amd_queue_, 0, sizeof(amd_queue_));

  // Initialize and map a HW AQL queue.
  HsaQueueResource queue_rsrc = {0};
  queue_rsrc.Queue_read_ptr_aql = (uint64_t*)&amd_queue_.read_dispatch_id;

  if (doorbell_type_ == 2) {
    // Hardware write pointer supports AQL semantics.
    queue_rsrc.Queue_write_ptr_aql = (uint64_t*)&amd_queue_.write_dispatch_id;
  } else {
    // Map hardware write pointer to a software proxy.
    queue_rsrc.Queue_write_ptr_aql = (uint64_t*)&amd_queue_.max_legacy_doorbell_dispatch_id_plus_1;
  }

  // Populate amd_queue_ structure.
  amd_queue_.hsa_queue.type = HSA_QUEUE_TYPE_MULTI;
  amd_queue_.hsa_queue.features = HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
  amd_queue_.hsa_queue.base_address = ring_buf_;
  amd_queue_.hsa_queue.doorbell_signal = Signal::Convert(this);
  amd_queue_.hsa_queue.size = queue_size_pkts;
  amd_queue_.hsa_queue.id = INVALID_QUEUEID;
  amd_queue_.read_dispatch_id_field_base_byte_offset = uint32_t(
      uintptr_t(&amd_queue_.read_dispatch_id) - uintptr_t(&amd_queue_));
  // Initialize the doorbell signal structure.
  memset(&signal_, 0, sizeof(signal_));
  signal_.kind = (doorbell_type_ == 2) ? AMD_SIGNAL_KIND_DOORBELL : AMD_SIGNAL_KIND_LEGACY_DOORBELL;
  signal_.legacy_hardware_doorbell_ptr = nullptr;
  signal_.queue_ptr = &amd_queue_;

  const auto& props = agent->properties();
  amd_queue_.max_cu_id = (props.NumFComputeCores / props.NumSIMDPerCU) - 1;
  amd_queue_.max_wave_id = (props.MaxWavesPerSIMD * props.NumSIMDPerCU) - 1;

#ifdef HSA_LARGE_MODEL
  AMD_HSA_BITS_SET(amd_queue_.queue_properties, AMD_QUEUE_PROPERTIES_IS_PTR64,
                   1);
#else
  AMD_HSA_BITS_SET(amd_queue_.queue_properties, AMD_QUEUE_PROPERTIES_IS_PTR64,
                   0);
#endif

  // Set group and private memory apertures in amd_queue_.
  auto& regions = agent->regions();

  for (auto region : regions) {
    const MemoryRegion* amdregion = static_cast<const AMD::MemoryRegion*>(region);
    uint64_t base = amdregion->GetBaseAddress();

    if (amdregion->IsLDS()) {
#ifdef HSA_LARGE_MODEL
      amd_queue_.group_segment_aperture_base_hi =
          uint32_t(uintptr_t(base) >> 32);
#else
      amd_queue_.group_segment_aperture_base_hi = uint32_t(base);
#endif
    }

    if (amdregion->IsScratch()) {
#ifdef HSA_LARGE_MODEL
      amd_queue_.private_segment_aperture_base_hi =
          uint32_t(uintptr_t(base) >> 32);
#else
      amd_queue_.private_segment_aperture_base_hi = uint32_t(base);
#endif
    }
  }

  assert(amd_queue_.group_segment_aperture_base_hi != 0 && "No group region found.");

  if (core::Runtime::runtime_singleton_->flag().check_flat_scratch()) {
    assert(amd_queue_.private_segment_aperture_base_hi != 0 && "No private region found.");
  }

  MAKE_NAMED_SCOPE_GUARD(EventGuard, [&]() {
    ScopedAcquire<KernelMutex> _lock(&queue_lock_);
    queue_count_--;
    if (queue_count_ == 0) {
      core::InterruptSignal::DestroyEvent(queue_event_);
      queue_event_ = nullptr;
    }
  });

  MAKE_NAMED_SCOPE_GUARD(SignalGuard, [&]() {
    if (amd_queue_.queue_inactive_signal.handle != 0)
      HSA::hsa_signal_destroy(amd_queue_.queue_inactive_signal);
    if (exception_signal_ != nullptr) exception_signal_->DestroySignal();
  });

  if (core::g_use_interrupt_wait) {
    ScopedAcquire<KernelMutex> _lock(&queue_lock_);
    queue_count_++;
    if (queue_event_ == nullptr) {
      assert(queue_count_ == 1 && "Inconsistency in queue event reference counting found.\n");

      queue_event_ = core::InterruptSignal::CreateEvent(HSA_EVENTTYPE_SIGNAL, false);
      if (queue_event_ == nullptr)
        throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                                 "Queue event creation failed.\n");
    }
    auto Signal = new core::InterruptSignal(0, queue_event_);
    assert(Signal != nullptr && "Should have thrown!\n");
    amd_queue_.queue_inactive_signal = core::InterruptSignal::Convert(Signal);
    exception_signal_ = new core::InterruptSignal(0, queue_event_);
    assert(exception_signal_ != nullptr && "Should have thrown!\n");
  } else {
    EventGuard.Dismiss();
    auto Signal = new core::DefaultSignal(0);
    assert(Signal != nullptr && "Should have thrown!\n");
    amd_queue_.queue_inactive_signal = core::DefaultSignal::Convert(Signal);
    exception_signal_ = new core::DefaultSignal(0);
    assert(exception_signal_ != nullptr && "Should have thrown!\n");
  }

  // Ensure the amd_queue_ is fully initialized before creating the KFD queue.
  // This ensures that the debugger can access the fields once it detects there
  // is a KFD queue. The debugger may access the aperture addresses, queue
  // scratch base, and queue type.

  HSAKMT_STATUS kmt_status;
  if (core::Runtime::runtime_singleton_->KfdVersion().supports_exception_debugging) {
    queue_rsrc.ErrorReason = &exception_signal_->signal_.value;
    kmt_status = hsaKmtCreateQueue(node_id, HSA_QUEUE_COMPUTE_AQL, 100, priority_, ring_buf_,
                                   ring_buf_alloc_bytes_, queue_event_, &queue_rsrc);
  } else {
    kmt_status = hsaKmtCreateQueue(node_id, HSA_QUEUE_COMPUTE_AQL, 100, priority_, ring_buf_,
                                   ring_buf_alloc_bytes_, NULL, &queue_rsrc);
  }
  if (kmt_status != HSAKMT_STATUS_SUCCESS)
    throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                             "Queue create failed at hsaKmtCreateQueue\n");
  // Complete populating the doorbell signal structure.
  signal_.legacy_hardware_doorbell_ptr = (volatile uint32_t*)queue_rsrc.Queue_DoorBell;

  // Bind Id of Queue such that is unique i.e. it is not re-used by another
  // queue (AQL, HOST) in the same process during its lifetime.
  amd_queue_.hsa_queue.id = this->GetQueueId();

  queue_id_ = queue_rsrc.QueueId;
  MAKE_NAMED_SCOPE_GUARD(QueueGuard, [&]() { hsaKmtDestroyQueue(queue_id_); });

  // Initialize scratch memory related entities
  queue_scratch_.queue_retry = amd_queue_.queue_inactive_signal;
  InitScratchSRD();

  if (core::Runtime::runtime_singleton_->KfdVersion().supports_exception_debugging) {
    if (AMD::hsa_amd_signal_async_handler(amd_queue_.queue_inactive_signal, HSA_SIGNAL_CONDITION_NE,
                                          0, DynamicScratchHandler<false>,
                                          this) != HSA_STATUS_SUCCESS)
      throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                               "Queue event handler failed registration.\n");
    if (AMD::hsa_amd_signal_async_handler(core::Signal::Convert(exception_signal_),
                                          HSA_SIGNAL_CONDITION_NE, 0, ExceptionHandler,
                                          this) != HSA_STATUS_SUCCESS)
      throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                               "Queue event handler failed registration.\n");
  } else {
    if (AMD::hsa_amd_signal_async_handler(amd_queue_.queue_inactive_signal, HSA_SIGNAL_CONDITION_NE,
                                          0, DynamicScratchHandler<true>,
                                          this) != HSA_STATUS_SUCCESS)
      throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                               "Queue event handler failed registration.\n");
    exceptionState = ERROR_HANDLER_DONE;
  }

  // Allocate IB for icache flushes.
  pm4_ib_buf_ =
      agent_->system_allocator()(pm4_ib_size_b_, 0x1000, core::MemoryRegion::AllocateExecutable);
  if (pm4_ib_buf_ == nullptr)
    throw AMD::hsa_exception(HSA_STATUS_ERROR_OUT_OF_RESOURCES, "PM4 IB allocation failed.\n");

  MAKE_NAMED_SCOPE_GUARD(PM4IBGuard, [&]() { agent_->system_deallocator()(pm4_ib_buf_); });

  // Set initial CU mask
  if (!core::Runtime::runtime_singleton_->flag().cu_mask_skip_init()) SetCUMasking(0, nullptr);

  active_ = true;

  PM4IBGuard.Dismiss();
  RingGuard.Dismiss();
  QueueGuard.Dismiss();
  EventGuard.Dismiss();
  SignalGuard.Dismiss();
}

AqlQueue::~AqlQueue() {
  // Remove error handler synchronously.
  // Sequences error handler callbacks with queue destroy.
  dynamicScratchState |= ERROR_HANDLER_TERMINATE;
  while ((dynamicScratchState & ERROR_HANDLER_DONE) != ERROR_HANDLER_DONE) {
    HSA::hsa_signal_store_screlease(amd_queue_.queue_inactive_signal, 0x8000000000000000ull);
    HSA::hsa_signal_wait_relaxed(amd_queue_.queue_inactive_signal, HSA_SIGNAL_CONDITION_NE,
                                 0x8000000000000000ull, -1ull, HSA_WAIT_STATE_BLOCKED);
  }

  // Remove kfd exception handler
  exceptionState |= ERROR_HANDLER_TERMINATE;
  while ((exceptionState & ERROR_HANDLER_DONE) != ERROR_HANDLER_DONE) {
    exception_signal_->StoreRelease(-1ull);
    exception_signal_->WaitRelaxed(HSA_SIGNAL_CONDITION_NE, -1ull, -1ull, HSA_WAIT_STATE_BLOCKED);
  }

  Inactivate();
  agent_->ReleaseQueueScratch(queue_scratch_);
  FreeRegisteredRingBuffer();
  exception_signal_->DestroySignal();
  HSA::hsa_signal_destroy(amd_queue_.queue_inactive_signal);
  if (core::g_use_interrupt_wait) {
    ScopedAcquire<KernelMutex> lock(&queue_lock_);
    queue_count_--;
    if (queue_count_ == 0) {
      core::InterruptSignal::DestroyEvent(queue_event_);
      queue_event_ = nullptr;
    }
  }
  agent_->system_deallocator()(pm4_ib_buf_);
}

void AqlQueue::Destroy() {
  if (amd_queue_.hsa_queue.type == HSA_QUEUE_TYPE_COOPERATIVE) {
    agent_->GWSRelease();
    return;
  }
  delete this;
}

uint64_t AqlQueue::LoadReadIndexAcquire() {
  return atomic::Load(&amd_queue_.read_dispatch_id, std::memory_order_acquire);
}

uint64_t AqlQueue::LoadReadIndexRelaxed() {
  return atomic::Load(&amd_queue_.read_dispatch_id, std::memory_order_relaxed);
}

uint64_t AqlQueue::LoadWriteIndexAcquire() {
  return atomic::Load(&amd_queue_.write_dispatch_id, std::memory_order_acquire);
}

uint64_t AqlQueue::LoadWriteIndexRelaxed() {
  return atomic::Load(&amd_queue_.write_dispatch_id, std::memory_order_relaxed);
}

void AqlQueue::StoreWriteIndexRelaxed(uint64_t value) {
  atomic::Store(&amd_queue_.write_dispatch_id, value,
                std::memory_order_relaxed);
}

void AqlQueue::StoreWriteIndexRelease(uint64_t value) {
  atomic::Store(&amd_queue_.write_dispatch_id, value,
                std::memory_order_release);
}

uint64_t AqlQueue::CasWriteIndexAcqRel(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_acq_rel);
}
uint64_t AqlQueue::CasWriteIndexAcquire(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_acquire);
}
uint64_t AqlQueue::CasWriteIndexRelaxed(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_relaxed);
}
uint64_t AqlQueue::CasWriteIndexRelease(uint64_t expected, uint64_t value) {
  return atomic::Cas(&amd_queue_.write_dispatch_id, value, expected,
                     std::memory_order_release);
}

uint64_t AqlQueue::AddWriteIndexAcqRel(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value,
                     std::memory_order_acq_rel);
}

uint64_t AqlQueue::AddWriteIndexAcquire(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value,
                     std::memory_order_acquire);
}

uint64_t AqlQueue::AddWriteIndexRelaxed(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value,
                     std::memory_order_relaxed);
}

uint64_t AqlQueue::AddWriteIndexRelease(uint64_t value) {
  return atomic::Add(&amd_queue_.write_dispatch_id, value,
                     std::memory_order_release);
}

void AqlQueue::StoreRelaxed(hsa_signal_value_t value) {
  if (doorbell_type_ == 2) {
    // Hardware doorbell supports AQL semantics.
    atomic::Store(signal_.hardware_doorbell_ptr, uint64_t(value), std::memory_order_release);
    return;
  }

  // Acquire spinlock protecting the legacy doorbell.
  while (atomic::Cas(&amd_queue_.legacy_doorbell_lock, 1U, 0U,
                     std::memory_order_acquire) != 0) {
    os::YieldThread();
  }

#ifdef HSA_LARGE_MODEL
  // AMD hardware convention expects the packet index to point beyond
  // the last packet to be processed. Packet indices written to the
  // max_legacy_doorbell_dispatch_id_plus_1 field must conform to this
  // expectation, since this field is used as the HW-visible write index.
  uint64_t legacy_dispatch_id = value + 1;
#else
  // In the small machine model it is difficult to distinguish packet index
  // wrap at 2^32 packets from a backwards doorbell. Instead, ignore the
  // doorbell value and submit the write index instead. It is OK to issue
  // a doorbell for packets in the INVALID or ALWAYS_RESERVED state.
  // The HW will stall on these packets until they enter a valid state.
  uint64_t legacy_dispatch_id = amd_queue_.write_dispatch_id;

  // The write index may extend more than a full queue of packets beyond
  // the read index. The hardware can process at most a full queue of packets
  // at a time. Clamp the write index appropriately. A doorbell for the
  // remaining packets is guaranteed to be sent at a later time.
  legacy_dispatch_id =
      Min(legacy_dispatch_id,
          uint64_t(amd_queue_.read_dispatch_id) + amd_queue_.hsa_queue.size);
#endif

  // Discard backwards and duplicate doorbells.
  if (legacy_dispatch_id > amd_queue_.max_legacy_doorbell_dispatch_id_plus_1) {
    // Record the most recent packet index used in a doorbell submission.
    // This field will be interpreted as a write index upon HW queue connect.
    // Make ring buffer visible to HW before updating write index.
    atomic::Store(&amd_queue_.max_legacy_doorbell_dispatch_id_plus_1,
                  legacy_dispatch_id, std::memory_order_release);

    // Write the dispatch id to the hardware MMIO doorbell.
    // Make write index visible to HW before sending doorbell.
    if (doorbell_type_ == 0) {
      // The legacy GFXIP 7 hardware doorbell expects:
      //   1. Packet index wrapped to a point within the ring buffer
      //   2. Packet index converted to DWORD count
      uint64_t queue_size_mask =
          ((1 + queue_full_workaround_) * amd_queue_.hsa_queue.size) - 1;

      atomic::Store(signal_.legacy_hardware_doorbell_ptr,
                    uint32_t((legacy_dispatch_id & queue_size_mask) *
                             (sizeof(core::AqlPacket) / sizeof(uint32_t))),
                    std::memory_order_release);
    } else if (doorbell_type_ == 1) {
      atomic::Store(signal_.legacy_hardware_doorbell_ptr,
                    uint32_t(legacy_dispatch_id), std::memory_order_release);
    } else {
      assert(false && "Agent has unsupported doorbell semantics");
    }
  }

  // Release spinlock protecting the legacy doorbell.
  // Also ensures timely delivery of (write-combined) doorbell to HW.
  atomic::Store(&amd_queue_.legacy_doorbell_lock, 0U,
                std::memory_order_release);
}

void AqlQueue::StoreRelease(hsa_signal_value_t value) {
  std::atomic_thread_fence(std::memory_order_release);
  StoreRelaxed(value);
}

uint32_t AqlQueue::ComputeRingBufferMinPkts() {
  // From CP_HQD_PQ_CONTROL.QUEUE_SIZE specification:
  //   Size of the primary queue (PQ) will be: 2^(HQD_QUEUE_SIZE+1) DWs.
  //   Min Size is 7 (2^8 = 256 DWs) and max size is 29 (2^30 = 1 G-DW)
  uint32_t min_bytes = 0x400;

  if (queue_full_workaround_ == 1) {
#ifdef __linux__
    // Double mapping requires one page of backing store.
    min_bytes = Max(min_bytes, 0x1000U);
#endif
#ifdef _WIN32
    // Shared memory mapping is at system allocation granularity.
    SYSTEM_INFO sys_info;
    GetNativeSystemInfo(&sys_info);
    min_bytes = Max(min_bytes, uint32_t(sys_info.dwAllocationGranularity));
#endif
  }

  return uint32_t(min_bytes / sizeof(core::AqlPacket));
}

uint32_t AqlQueue::ComputeRingBufferMaxPkts() {
  // From CP_HQD_PQ_CONTROL.QUEUE_SIZE specification:
  //   Size of the primary queue (PQ) will be: 2^(HQD_QUEUE_SIZE+1) DWs.
  //   Min Size is 7 (2^8 = 256 DWs) and max size is 29 (2^30 = 1 G-DW)
  uint64_t max_bytes = 0x100000000;

  if (queue_full_workaround_ == 1) {
    // Double mapping halves maximum size.
    max_bytes /= 2;
  }

  return uint32_t(max_bytes / sizeof(core::AqlPacket));
}

void AqlQueue::AllocRegisteredRingBuffer(uint32_t queue_size_pkts) {
  if ((agent_->profile() == HSA_PROFILE_FULL) && queue_full_workaround_) {
    // Compute the physical and virtual size of the queue.
    uint32_t ring_buf_phys_size_bytes =
        uint32_t(queue_size_pkts * sizeof(core::AqlPacket));
    ring_buf_alloc_bytes_ = 2 * ring_buf_phys_size_bytes;

#ifdef __linux__
    // Create a system-unique shared memory path for this thread.
    char ring_buf_shm_path[16];
    pid_t sys_unique_tid = pid_t(syscall(__NR_gettid));
    sprintf(ring_buf_shm_path, "/%u", sys_unique_tid);

    int ring_buf_shm_fd = -1;
    void* reserve_va = NULL;

    ring_buf_shm_fd = CreateRingBufferFD(ring_buf_shm_path, ring_buf_phys_size_bytes);

    if (ring_buf_shm_fd == -1) {
      return;
    }

    // Reserve a VA range twice the size of the physical backing store.
    reserve_va = mmap(NULL, ring_buf_alloc_bytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(reserve_va != MAP_FAILED && "mmap failed");

    // Remap the lower and upper halves of the VA range.
    // Map both halves to the shared memory backing store.
    // If the GPU device is KV, do not set PROT_EXEC flag.
    void* ring_buf_lower_half = NULL;
    void* ring_buf_upper_half = NULL;
    if (is_kv_queue_) {
      ring_buf_lower_half = mmap(reserve_va, ring_buf_phys_size_bytes, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_FIXED, ring_buf_shm_fd, 0);
      assert(ring_buf_lower_half != MAP_FAILED && "mmap failed");

      ring_buf_upper_half =
          mmap((void*)(uintptr_t(reserve_va) + ring_buf_phys_size_bytes), ring_buf_phys_size_bytes,
               PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, ring_buf_shm_fd, 0);
      assert(ring_buf_upper_half != MAP_FAILED && "mmap failed");
      } else {
        ring_buf_lower_half = mmap(reserve_va, ring_buf_phys_size_bytes,
                                   PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_SHARED | MAP_FIXED, ring_buf_shm_fd, 0);
        assert(ring_buf_lower_half != MAP_FAILED && "mmap failed");

        ring_buf_upper_half =
            mmap((void*)(uintptr_t(reserve_va) + ring_buf_phys_size_bytes),
                 ring_buf_phys_size_bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_SHARED | MAP_FIXED, ring_buf_shm_fd, 0);
        assert(ring_buf_upper_half != MAP_FAILED && "mmap failed");
      }

      // Successfully created mapping.
      ring_buf_ = ring_buf_lower_half;

      // Release explicit reference to shared memory object.
      CloseRingBufferFD(ring_buf_shm_path, ring_buf_shm_fd);
      return;
#endif
#ifdef _WIN32
    HANDLE ring_buf_mapping = INVALID_HANDLE_VALUE;
    void* ring_buf_lower_half = NULL;
    void* ring_buf_upper_half = NULL;

    do {
      // Create a page file mapping to back the ring buffer.
      ring_buf_mapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL,
                                           PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                           0, ring_buf_phys_size_bytes, NULL);
      if (ring_buf_mapping == NULL) {
        break;
      }

      // Retry until obtaining an appropriate virtual address mapping.
      for (int num_attempts = 0; num_attempts < 1000; ++num_attempts) {
        // Find a virtual address range twice the size of the file mapping.
        void* reserve_va =
            VirtualAllocEx(GetCurrentProcess(), NULL, ring_buf_alloc_bytes_,
                           MEM_TOP_DOWN | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (reserve_va == NULL) {
          break;
        }
        VirtualFree(reserve_va, 0, MEM_RELEASE);

        // Map the ring buffer into the free virtual range.
        // This may fail: another thread can allocate in this range.
        ring_buf_lower_half = MapViewOfFileEx(
            ring_buf_mapping, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0,
            ring_buf_phys_size_bytes, reserve_va);

        if (ring_buf_lower_half == NULL) {
          // Virtual range allocated by another thread, try again.
          continue;
        }

        ring_buf_upper_half = MapViewOfFileEx(
            ring_buf_mapping, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0,
            ring_buf_phys_size_bytes,
            (void*)(uintptr_t(reserve_va) + ring_buf_phys_size_bytes));

        if (ring_buf_upper_half == NULL) {
          // Virtual range allocated by another thread, try again.
          UnmapViewOfFile(ring_buf_lower_half);
          continue;
        }

        // Successfully created mapping.
        ring_buf_ = ring_buf_lower_half;
        break;
      }

      if (ring_buf_ == NULL) {
        break;
      }

      // Release file mapping (reference counted by views).
      CloseHandle(ring_buf_mapping);

      // Don't register the memory: causes a failure in the KFD.
      // Instead use implicit registration to access the ring buffer.
      return;
    } while (false);

    // Resource cleanup on failure.
    UnmapViewOfFile(ring_buf_upper_half);
    UnmapViewOfFile(ring_buf_lower_half);
    CloseHandle(ring_buf_mapping);
#endif
  } else {
    // Allocate storage for the ring buffer.
    ring_buf_alloc_bytes_ = AlignUp(
        queue_size_pkts * sizeof(core::AqlPacket), 4096);

    ring_buf_ = agent_->system_allocator()(
        ring_buf_alloc_bytes_, 0x1000,
        core::MemoryRegion::AllocateExecutable |
            (queue_full_workaround_ ? core::MemoryRegion::AllocateDoubleMap : 0));

    assert(ring_buf_ != NULL && "AQL queue memory allocation failure");

    // The virtual ring allocation is twice as large as requested.
    // Each half maps to the same set of physical pages.
    if (queue_full_workaround_) ring_buf_alloc_bytes_ *= 2;
  }
}

void AqlQueue::FreeRegisteredRingBuffer() {
  if ((agent_->profile() == HSA_PROFILE_FULL) && queue_full_workaround_) {
#ifdef __linux__
    munmap(ring_buf_, ring_buf_alloc_bytes_);
#endif
#ifdef _WIN32
    UnmapViewOfFile(ring_buf_);
    UnmapViewOfFile(
        (void*)(uintptr_t(ring_buf_) + (ring_buf_alloc_bytes_ / 2)));
#endif
  } else {
    agent_->system_deallocator()(ring_buf_);
  }

  ring_buf_ = NULL;
  ring_buf_alloc_bytes_ = 0;
}

void AqlQueue::CloseRingBufferFD(const char* ring_buf_shm_path, int fd) const {
#ifdef __linux__
#if !defined(HAVE_MEMFD_CREATE)
  shm_unlink(ring_buf_shm_path);
#endif
  close(fd);
#else
  assert(false && "Function only needed on Linux.");
#endif
}

int AqlQueue::CreateRingBufferFD(const char* ring_buf_shm_path,
                                 uint32_t ring_buf_phys_size_bytes) const {
#ifdef __linux__
  int fd;
#ifdef HAVE_MEMFD_CREATE
  fd = syscall(__NR_memfd_create, ring_buf_shm_path, 0);

  if (fd == -1) return -1;

  if (ftruncate(fd, ring_buf_phys_size_bytes) == -1) {
    CloseRingBufferFD(ring_buf_shm_path, fd);
    return -1;
  }
#else
  fd = shm_open(ring_buf_shm_path, O_CREAT | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR);

  if (fd == -1) return -1;

  if (posix_fallocate(fd, 0, ring_buf_phys_size_bytes) != 0) {
    CloseRingBufferFD(ring_buf_shm_path, fd);
    return -1;
  }
#endif
  return fd;
#else
  assert(false && "Function only needed on Linux.");
  return -1;
#endif
}

void AqlQueue::Suspend() {
  suspended_ = true;
  auto err = hsaKmtUpdateQueue(queue_id_, 0, priority_, ring_buf_, ring_buf_alloc_bytes_, NULL);
  assert(err == HSAKMT_STATUS_SUCCESS && "hsaKmtUpdateQueue failed.");
}

hsa_status_t AqlQueue::Inactivate() {
  bool active = active_.exchange(false, std::memory_order_relaxed);
  if (active) {
    auto err = hsaKmtDestroyQueue(queue_id_);
    assert(err == HSAKMT_STATUS_SUCCESS && "hsaKmtDestroyQueue failed.");
    atomic::Fence(std::memory_order_acquire);
  }
  return HSA_STATUS_SUCCESS;
}

hsa_status_t AqlQueue::SetPriority(HSA_QUEUE_PRIORITY priority) {
  if (suspended_) {
    return HSA_STATUS_ERROR_INVALID_QUEUE;
  }

  priority_ = priority;
  auto err = hsaKmtUpdateQueue(queue_id_, 100, priority_, ring_buf_, ring_buf_alloc_bytes_, NULL);
  return (err == HSAKMT_STATUS_SUCCESS ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR_OUT_OF_RESOURCES);
}

template <bool HandleExceptions>
bool AqlQueue::DynamicScratchHandler(hsa_signal_value_t error_code, void* arg) {
  AqlQueue* queue = (AqlQueue*)arg;
  hsa_status_t errorCode = HSA_STATUS_SUCCESS;
  bool fatal = false;
  bool changeWait = false;
  hsa_signal_value_t waitVal;

  if ((queue->dynamicScratchState & ERROR_HANDLER_SCRATCH_RETRY) == ERROR_HANDLER_SCRATCH_RETRY) {
    queue->dynamicScratchState &= ~ERROR_HANDLER_SCRATCH_RETRY;
    changeWait = true;
    waitVal = 0;
    HSA::hsa_signal_and_relaxed(queue->amd_queue_.queue_inactive_signal, ~0x8000000000000000ull);
    error_code &= ~0x8000000000000000ull;
  }

  // Process errors only if queue is not terminating.
  if ((queue->dynamicScratchState & ERROR_HANDLER_TERMINATE) != ERROR_HANDLER_TERMINATE) {
    if (error_code == 512) {  // Large scratch reclaim
      auto& scratch = queue->queue_scratch_;
      queue->agent_->ReleaseQueueScratch(scratch);
      scratch.queue_base = nullptr;
      scratch.size = 0;
      scratch.size_per_thread = 0;
      scratch.queue_process_offset = 0;
      queue->InitScratchSRD();

      HSA::hsa_signal_store_relaxed(queue->amd_queue_.queue_inactive_signal, 0);
      // Resumes queue processing.
      atomic::Store(&queue->amd_queue_.queue_properties,
                    queue->amd_queue_.queue_properties & (~AMD_QUEUE_PROPERTIES_USE_SCRATCH_ONCE),
                    std::memory_order_release);
      atomic::Fence(std::memory_order_release);
      return true;
    }

    // Process only one queue error.
    if (error_code & 0x401) {  // insufficient scratch, wave64 or wave32
      // Insufficient scratch - recoverable, don't process dynamic scratch if errors are present.
      auto& scratch = queue->queue_scratch_;

      queue->agent_->ReleaseQueueScratch(scratch);

      uint64_t pkt_slot_idx =
          queue->amd_queue_.read_dispatch_id & (queue->amd_queue_.hsa_queue.size - 1);

      core::AqlPacket& pkt =
          ((core::AqlPacket*)queue->amd_queue_.hsa_queue.base_address)[pkt_slot_idx];

      assert(pkt.IsValid() && "Invalid packet in dynamic scratch handler.");
      assert(pkt.type() == HSA_PACKET_TYPE_KERNEL_DISPATCH &&
             "Invalid packet in dynamic scratch handler.");

      uint32_t scratch_request = pkt.dispatch.private_segment_size;

      const uint32_t MaxScratchSlots =
          (queue->amd_queue_.max_cu_id + 1) * queue->agent_->properties().MaxSlotsScratchCU;

      scratch.size_per_thread = scratch_request;
      scratch.lanes_per_wave = (error_code & 0x400) ? 32 : 64;
      // Align whole waves to 1KB.
      scratch.size_per_thread = AlignUp(scratch.size_per_thread, 1024 / scratch.lanes_per_wave);
      scratch.size = scratch.size_per_thread * MaxScratchSlots * scratch.lanes_per_wave;

      uint64_t lanes_per_group =
          (uint64_t(pkt.dispatch.workgroup_size_x) * pkt.dispatch.workgroup_size_y) *
          pkt.dispatch.workgroup_size_z;
      uint64_t waves_per_group =
          (lanes_per_group + scratch.lanes_per_wave - 1) / scratch.lanes_per_wave;
      scratch.waves_per_group = waves_per_group;

      uint64_t groups = ((uint64_t(pkt.dispatch.grid_size_x) + pkt.dispatch.workgroup_size_x - 1) /
                         pkt.dispatch.workgroup_size_x) *
          ((uint64_t(pkt.dispatch.grid_size_y) + pkt.dispatch.workgroup_size_y - 1) /
           pkt.dispatch.workgroup_size_y) *
          ((uint64_t(pkt.dispatch.grid_size_z) + pkt.dispatch.workgroup_size_z - 1) /
           pkt.dispatch.workgroup_size_z);

      // Assign an equal number of groups to each engine, clipping to capacity limits
      const uint32_t engines = queue->agent_->properties().NumShaderBanks;
      groups = ((groups + engines - 1) / engines) * engines;
      scratch.wanted_slots = groups * waves_per_group;
      scratch.wanted_slots = Min(scratch.wanted_slots, uint64_t(MaxScratchSlots));
      scratch.dispatch_size =
          scratch.size_per_thread * scratch.wanted_slots * scratch.lanes_per_wave;

      queue->agent_->AcquireQueueScratch(scratch);

      if (scratch.retry) {
        queue->dynamicScratchState |= ERROR_HANDLER_SCRATCH_RETRY;
        changeWait = true;
        waitVal = error_code;
      } else {
        // Out of scratch - promote error
        if (scratch.queue_base == nullptr) {
          errorCode = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        } else {
          // Mark large scratch allocation for single use.
          if (scratch.large) {
            queue->amd_queue_.queue_properties |= AMD_QUEUE_PROPERTIES_USE_SCRATCH_ONCE;
            // Set system release fence to flush scratch stores with older firmware versions.
            if ((queue->agent_->isa()->GetMajorVersion() == 8) &&
                (queue->agent_->GetMicrocodeVersion() < 729)) {
              pkt.dispatch.header &= ~(((1 << HSA_PACKET_HEADER_WIDTH_SCRELEASE_FENCE_SCOPE) - 1)
                                       << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
              pkt.dispatch.header |=
                  (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
            }
          }
          // Reset scratch memory related entities for the queue
          queue->InitScratchSRD();
          // Restart the queue.
          HSA::hsa_signal_store_screlease(queue->amd_queue_.queue_inactive_signal, 0);
        }
      }

    } else if (HandleExceptions) {
      if ((error_code & 2) == 2) {  // Invalid dim
        errorCode = HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;

      } else if ((error_code & 4) == 4) {  // Invalid group memory
        errorCode = HSA_STATUS_ERROR_INVALID_ALLOCATION;

      } else if ((error_code & 8) == 8) {  // Invalid (or NULL) code
        errorCode = HSA_STATUS_ERROR_INVALID_CODE_OBJECT;

      } else if (((error_code & 32) == 32) ||    // Invalid format: 32 is generic,
                 ((error_code & 256) == 256)) {  // 256 is vendor specific packets
        errorCode = HSA_STATUS_ERROR_INVALID_PACKET_FORMAT;

      } else if ((error_code & 64) == 64) {  // Group is too large
        errorCode = HSA_STATUS_ERROR_INVALID_ARGUMENT;

      } else if ((error_code & 128) == 128) {  // Out of VGPRs
        errorCode = HSA_STATUS_ERROR_INVALID_ISA;

      } else if ((error_code & 0x20000000) == 0x20000000) {  // Memory violation (>48-bit)
        errorCode = hsa_status_t(HSA_STATUS_ERROR_MEMORY_APERTURE_VIOLATION);

      } else if ((error_code & 0x40000000) == 0x40000000) {  // Illegal instruction
        errorCode = hsa_status_t(HSA_STATUS_ERROR_ILLEGAL_INSTRUCTION);

      } else if ((error_code & 0x80000000) == 0x80000000) {  // Debug trap
        errorCode = HSA_STATUS_ERROR_EXCEPTION;
        fatal = true;

      } else {  // Undefined code
        assert(false && "Undefined queue error code");
        errorCode = HSA_STATUS_ERROR;
        fatal = true;
      }
    } else {
      // Not handling exceptions, clear so that ExceptionHandler can run.
      HSA::hsa_signal_store_relaxed(queue->amd_queue_.queue_inactive_signal, 0);
    }

    if (errorCode == HSA_STATUS_SUCCESS) {
      if (changeWait) {
        core::Runtime::runtime_singleton_->SetAsyncSignalHandler(
            queue->amd_queue_.queue_inactive_signal, HSA_SIGNAL_CONDITION_NE, waitVal,
            DynamicScratchHandler<HandleExceptions>, queue);
        return false;
      }
      return true;
    }

    queue->Suspend();
    if (queue->errors_callback_ != nullptr) {
      queue->errors_callback_(errorCode, queue->public_handle(), queue->errors_data_);
    }
    if (fatal) {
      // Temporarilly removed until there is clarity on exactly what debugtrap's semantics are.
      // assert(false && "Fatal queue error");
      // std::abort();
    }
  }
  // Copy here is to protect against queue being released between setting the scratch state and
  // updating the signal value.  The signal itself is safe to use because it is ref counted rather
  // than being released with the queue.
  hsa_signal_t signal = queue->amd_queue_.queue_inactive_signal;
  queue->dynamicScratchState = ERROR_HANDLER_DONE;
  HSA::hsa_signal_store_screlease(signal, -1ull);
  return false;
}

bool AqlQueue::ExceptionHandler(hsa_signal_value_t error_code, void* arg) {
  struct queue_error_t {
    uint32_t code;
    hsa_status_t status;
  };
  static const queue_error_t QueueErrors[] = {
      // EC_QUEUE_WAVE_ABORT
      1, HSA_STATUS_ERROR_EXCEPTION,
      // EC_QUEUE_WAVE_TRAP
      2, HSA_STATUS_ERROR_EXCEPTION,
      // EC_QUEUE_WAVE_MATH_ERROR
      3, HSA_STATUS_ERROR_EXCEPTION,
      // EC_QUEUE_WAVE_ILLEGAL_INSTRUCTION
      4, (hsa_status_t)HSA_STATUS_ERROR_ILLEGAL_INSTRUCTION,
      // EC_QUEUE_WAVE_MEMORY_VIOLATION
      5, (hsa_status_t)HSA_STATUS_ERROR_MEMORY_FAULT,
      // EC_QUEUE_WAVE_APERTURE_VIOLATION
      6, (hsa_status_t)HSA_STATUS_ERROR_MEMORY_APERTURE_VIOLATION,
      // EC_QUEUE_PACKET_DISPATCH_DIM_INVALID
      16, HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS,
      // EC_QUEUE_PACKET_DISPATCH_GROUP_SEGMENT_SIZE_INVALID
      17, HSA_STATUS_ERROR_INVALID_ALLOCATION,
      // EC_QUEUE_PACKET_DISPATCH_CODE_INVALID
      18, HSA_STATUS_ERROR_INVALID_CODE_OBJECT,
      // EC_QUEUE_PACKET_UNSUPPORTED
      20, HSA_STATUS_ERROR_INVALID_PACKET_FORMAT,
      // EC_QUEUE_PACKET_DISPATCH_WORK_GROUP_SIZE_INVALID
      21, HSA_STATUS_ERROR_INVALID_ARGUMENT,
      // EC_QUEUE_PACKET_DISPATCH_REGISTER_SIZE_INVALID
      22, HSA_STATUS_ERROR_INVALID_ISA,
      // EC_QUEUE_PACKET_VENDOR_UNSUPPORTED
      23, HSA_STATUS_ERROR_INVALID_PACKET_FORMAT,
      // EC_QUEUE_PREEMPTION_ERROR
      31, HSA_STATUS_ERROR,
      // EC_DEVICE_MEMORY_VIOLATION
      33, (hsa_status_t)HSA_STATUS_ERROR_MEMORY_APERTURE_VIOLATION,
      // EC_DEVICE_RAS_ERROR
      34, HSA_STATUS_ERROR,
      // EC_DEVICE_FATAL_HALT
      35, HSA_STATUS_ERROR,
      // EC_DEVICE_NEW
      36, HSA_STATUS_ERROR,
      // EC_PROCESS_DEVICE_REMOVE
      50, HSA_STATUS_ERROR};

  AqlQueue* queue = (AqlQueue*)arg;
  hsa_status_t errorCode = HSA_STATUS_ERROR;

  if (queue->exceptionState == ERROR_HANDLER_TERMINATE) {
    Signal* signal = queue->exception_signal_;
    queue->exceptionState = ERROR_HANDLER_DONE;
    signal->StoreRelease(0);
    return false;
  }

  for (auto& error : QueueErrors) {
    if (error_code & (1 << (error.code - 1))) {
      errorCode = error.status;
      break;
    }
  }

  // Undefined or unexpected code
  assert((errorCode != HSA_STATUS_ERROR) && "Undefined or unexpected queue error code");

  queue->Suspend();
  if (queue->errors_callback_ != nullptr) {
    queue->errors_callback_(errorCode, queue->public_handle(), queue->errors_data_);
  }
  Signal* signal = queue->exception_signal_;
  queue->exceptionState = ERROR_HANDLER_DONE;
  signal->StoreRelease(0);
  return false;
}

hsa_status_t AqlQueue::SetCUMasking(uint32_t num_cu_mask_count, const uint32_t* cu_mask) {
  uint32_t cu_count;
  agent_->GetInfo((hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &cu_count);
  size_t mask_dwords = (cu_count + 31) / 32;
  // Mask to trim the last uint32_t in cu_mask to the physical CU count
  uint32_t tail_mask = (1 << (cu_count % 32)) - 1;

  auto global_mask = core::Runtime::runtime_singleton_->flag().cu_mask(agent_->enumeration_index());
  std::vector<uint32_t> mask;

  bool clipped = false;

  // num_cu_mask_count = 0 resets the CU mask.
  if (num_cu_mask_count == 0) {
    for (int i = 0; i < mask_dwords; i++) mask.push_back(-1);
  } else {
    for (int i = 0; i < num_cu_mask_count / 32; i++) mask.push_back(cu_mask[i]);
  }

  // Apply global mask to user mask
  if (!global_mask.empty()) {
    // Limit mask processing to smallest needed dword range
    size_t limit = Min(global_mask.size(), mask.size(), mask_dwords);

    // Check for disabling requested cus.
    for (int i = limit; i < mask.size(); i++) {
      if (mask[i] != 0) {
        clipped = true;
        break;
      }
    }

    mask.resize(limit, 0);
    for (size_t i = 0; i < limit; i++) {
      clipped |= ((mask[i] & (~global_mask[i])) != 0);
      mask[i] &= global_mask[i];
    }
  } else {
    // Limit to physical CU range only
    size_t limit = Min(mask.size(), mask_dwords);
    mask.resize(limit, 0);
  }

  // Clip last dword to physical CU limit if necessary
  if ((mask.size() == mask_dwords) && (tail_mask != 0)) mask[mask_dwords - 1] &= tail_mask;

  // Apply mask if non-default or not queue initialization.
  ScopedAcquire<KernelMutex> lock(&mask_lock_);
  if ((!cu_mask_.empty()) || (num_cu_mask_count != 0) || (!global_mask.empty())) {
    HSAKMT_STATUS ret =
        hsaKmtSetQueueCUMask(queue_id_, mask.size() * 32, reinterpret_cast<HSAuint32*>(&mask[0]));
    if (ret != HSAKMT_STATUS_SUCCESS) return HSA_STATUS_ERROR;
  }

  // update current cu masking tracking.
  cu_mask_ = std::move(mask);
  return clipped ? (hsa_status_t)HSA_STATUS_CU_MASK_REDUCED : HSA_STATUS_SUCCESS;
}

hsa_status_t AqlQueue::GetCUMasking(uint32_t num_cu_mask_count, uint32_t* cu_mask) {
  ScopedAcquire<KernelMutex> lock(&mask_lock_);
  assert(!cu_mask_.empty() && "No current cu_mask!");

  uint32_t user_dword_count = num_cu_mask_count / 32;
  if (user_dword_count > cu_mask_.size()) {
    memset(&cu_mask[cu_mask_.size()], 0, sizeof(uint32_t) * (user_dword_count - cu_mask_.size()));
    user_dword_count = cu_mask_.size();
  }
  memcpy(cu_mask, &cu_mask_[0], sizeof(uint32_t) * user_dword_count);
  return HSA_STATUS_SUCCESS;
}

void AqlQueue::ExecutePM4(uint32_t* cmd_data, size_t cmd_size_b) {
  // pm4_ib_buf_ is a shared resource, so mutually exclude here.
  ScopedAcquire<KernelMutex> lock(&pm4_ib_mutex_);

  // Obtain reference to any container queue.
  core::Queue* queue = core::Queue::Convert(public_handle());

  // Obtain a queue slot for a single AQL packet.
  uint64_t write_idx = queue->AddWriteIndexAcqRel(1);

  while ((write_idx - queue->LoadReadIndexRelaxed()) >= queue->amd_queue_.hsa_queue.size) {
    os::YieldThread();
  }

  uint32_t slot_idx = uint32_t(write_idx % queue->amd_queue_.hsa_queue.size);
  constexpr uint32_t slot_size_b = 0x40;
  uint32_t* queue_slot =
      (uint32_t*)(uintptr_t(queue->amd_queue_.hsa_queue.base_address) + (slot_idx * slot_size_b));

  // Copy client PM4 command into IB.
  assert(cmd_size_b < pm4_ib_size_b_ && "PM4 exceeds IB size");
  memcpy(pm4_ib_buf_, cmd_data, cmd_size_b);

  // Construct a PM4 command to execute the IB.
  constexpr uint32_t ib_jump_size_dw = 4;

  uint32_t ib_jump_cmd[ib_jump_size_dw] = {
      PM4_HDR(PM4_HDR_IT_OPCODE_INDIRECT_BUFFER, ib_jump_size_dw, agent_->isa()->GetMajorVersion()),
      PM4_INDIRECT_BUFFER_DW1_IB_BASE_LO(uint32_t(uintptr_t(pm4_ib_buf_) >> 2)),
      PM4_INDIRECT_BUFFER_DW2_IB_BASE_HI(uint32_t(uintptr_t(pm4_ib_buf_) >> 32)),
      (PM4_INDIRECT_BUFFER_DW3_IB_SIZE(uint32_t(cmd_size_b / sizeof(uint32_t))) |
       PM4_INDIRECT_BUFFER_DW3_IB_VALID(1))};

  // To respect multi-producer semantics, first buffer commands for the queue slot.
  constexpr uint32_t slot_size_dw = uint32_t(slot_size_b / sizeof(uint32_t));
  uint32_t slot_data[slot_size_dw];

  if (agent_->isa()->GetMajorVersion() <= 8) {
    // Construct a set of PM4 to fit inside the AQL packet slot.
    uint32_t slot_dw_idx = 0;

    // Construct a no-op command to pad the queue slot.
    constexpr uint32_t rel_mem_size_dw = 7;
    constexpr uint32_t nop_pad_size_dw = slot_size_dw - (ib_jump_size_dw + rel_mem_size_dw);

    uint32_t* nop_pad = &slot_data[slot_dw_idx];
    slot_dw_idx += nop_pad_size_dw;

    nop_pad[0] = PM4_HDR(PM4_HDR_IT_OPCODE_NOP, nop_pad_size_dw, agent_->isa()->GetMajorVersion());

    for (uint32_t i = 1; i < nop_pad_size_dw; ++i) {
      nop_pad[i] = 0;
    }

    // Copy in command to execute the IB.
    assert(slot_dw_idx + ib_jump_size_dw <= slot_size_dw && "PM4 exceeded queue slot size");
    uint32_t* ib_jump = &slot_data[slot_dw_idx];
    slot_dw_idx += ib_jump_size_dw;

    memcpy(ib_jump, ib_jump_cmd, sizeof(ib_jump_cmd));

    // Construct a command to advance the read index and invalidate the packet
    // header. This must be the last command since this releases the queue slot
    // for writing.
    assert(slot_dw_idx + rel_mem_size_dw <= slot_size_dw && "PM4 exceeded queue slot size");
    uint32_t* rel_mem = &slot_data[slot_dw_idx];

    rel_mem[0] =
        PM4_HDR(PM4_HDR_IT_OPCODE_RELEASE_MEM, rel_mem_size_dw, agent_->isa()->GetMajorVersion());
    rel_mem[1] = PM4_RELEASE_MEM_DW1_EVENT_INDEX(PM4_RELEASE_MEM_EVENT_INDEX_AQL);
    rel_mem[2] = 0;
    rel_mem[3] = 0;
    rel_mem[4] = 0;
    rel_mem[5] = 0;
    rel_mem[6] = 0;
  } else if (agent_->isa()->GetMajorVersion() >= 9) {
    // Construct an AQL packet to jump to the PM4 IB.
    struct amd_aql_pm4_ib {
      uint16_t header;
      uint16_t ven_hdr;
      uint32_t ib_jump_cmd[4];
      uint32_t dw_cnt_remain;
      uint32_t reserved[8];
      hsa_signal_t completion_signal;
    };

    constexpr uint32_t AMD_AQL_FORMAT_PM4_IB = 0x1;

    amd_aql_pm4_ib aql_pm4_ib{};
    aql_pm4_ib.header = HSA_PACKET_TYPE_VENDOR_SPECIFIC << HSA_PACKET_HEADER_TYPE;
    aql_pm4_ib.ven_hdr = AMD_AQL_FORMAT_PM4_IB;
    aql_pm4_ib.ib_jump_cmd[0] = ib_jump_cmd[0];
    aql_pm4_ib.ib_jump_cmd[1] = ib_jump_cmd[1];
    aql_pm4_ib.ib_jump_cmd[2] = ib_jump_cmd[2];
    aql_pm4_ib.ib_jump_cmd[3] = ib_jump_cmd[3];
    aql_pm4_ib.dw_cnt_remain = 0xA;

    memcpy(slot_data, &aql_pm4_ib, sizeof(aql_pm4_ib));
  } else {
    assert(false && "AqlQueue::ExecutePM4 not implemented");
  }

  // Copy buffered commands into the queue slot.
  // Overwrite the AQL invalid header (first dword) last.
  // This prevents the slot from being read until it's fully written.
  memcpy(&queue_slot[1], &slot_data[1], slot_size_b - sizeof(uint32_t));
  atomic::Store(&queue_slot[0], slot_data[0], std::memory_order_release);

  // Submit the packet slot.
  core::Signal* doorbell = core::Signal::Convert(queue->amd_queue_.hsa_queue.doorbell_signal);
  doorbell->StoreRelease(write_idx);

  // Wait for the packet to be consumed.
  // Should be switched to a signal wait when aql_pm4_ib can be used on all
  // supported platforms.
  while (queue->LoadReadIndexRelaxed() <= write_idx) {
    os::YieldThread();
  }
}

// @brief Define the Scratch Buffer Descriptor and related parameters
// that enable kernel access scratch memory
void AqlQueue::InitScratchSRD() {

  // Populate scratch resource descriptor
  SQ_BUF_RSRC_WORD0 srd0;
  SQ_BUF_RSRC_WORD1 srd1;
  SQ_BUF_RSRC_WORD2 srd2;
  uint32_t srd3_u32;

  uint32_t scratch_base_hi = 0;
  uintptr_t scratch_base = uintptr_t(queue_scratch_.queue_base);
  #ifdef HSA_LARGE_MODEL
  scratch_base_hi = uint32_t(scratch_base >> 32);
  #endif
  srd0.bits.BASE_ADDRESS = uint32_t(scratch_base);

  srd1.bits.BASE_ADDRESS_HI = scratch_base_hi;
  srd1.bits.STRIDE = 0;
  srd1.bits.CACHE_SWIZZLE = 0;
  srd1.bits.SWIZZLE_ENABLE = 1;

  srd2.bits.NUM_RECORDS = uint32_t(queue_scratch_.size);

  if (agent_->isa()->GetMajorVersion() < 10) {
    SQ_BUF_RSRC_WORD3 srd3;

    srd3.bits.DST_SEL_X = SQ_SEL_X;
    srd3.bits.DST_SEL_Y = SQ_SEL_Y;
    srd3.bits.DST_SEL_Z = SQ_SEL_Z;
    srd3.bits.DST_SEL_W = SQ_SEL_W;
    srd3.bits.NUM_FORMAT = BUF_NUM_FORMAT_UINT;
    srd3.bits.DATA_FORMAT = BUF_DATA_FORMAT_32;
    srd3.bits.ELEMENT_SIZE = 1;  // 4
    srd3.bits.INDEX_STRIDE = 3;  // 64
    srd3.bits.ADD_TID_ENABLE = 1;
    srd3.bits.ATC__CI__VI = (agent_->profile() == HSA_PROFILE_FULL);
    srd3.bits.HASH_ENABLE = 0;
    srd3.bits.HEAP = 0;
    srd3.bits.MTYPE__CI__VI = 0;
    srd3.bits.TYPE = SQ_RSRC_BUF;

    srd3_u32 = srd3.u32All;
  } else {
    SQ_BUF_RSRC_WORD3_GFX10 srd3;

    srd3.bits.DST_SEL_X = SQ_SEL_X;
    srd3.bits.DST_SEL_Y = SQ_SEL_Y;
    srd3.bits.DST_SEL_Z = SQ_SEL_Z;
    srd3.bits.DST_SEL_W = SQ_SEL_W;
    srd3.bits.FORMAT = BUF_FORMAT_32_UINT;
    srd3.bits.RESERVED1 = 0;
    srd3.bits.INDEX_STRIDE = 0;  // filled in by CP
    srd3.bits.ADD_TID_ENABLE = 1;
    srd3.bits.RESOURCE_LEVEL = 1;
    srd3.bits.RESERVED2 = 0;
    srd3.bits.OOB_SELECT = 2;  // no bounds check in swizzle mode
    srd3.bits.TYPE = SQ_RSRC_BUF;

    srd3_u32 = srd3.u32All;
  }

  // Update Queue's Scratch descriptor's property
  amd_queue_.scratch_resource_descriptor[0] = srd0.u32All;
  amd_queue_.scratch_resource_descriptor[1] = srd1.u32All;
  amd_queue_.scratch_resource_descriptor[2] = srd2.u32All;
  amd_queue_.scratch_resource_descriptor[3] = srd3_u32;

  // Populate flat scratch parameters in amd_queue_.
  amd_queue_.scratch_backing_memory_location =
      queue_scratch_.queue_process_offset;
  amd_queue_.scratch_backing_memory_byte_size = queue_scratch_.size;

  // For backwards compatibility this field records the per-lane scratch
  // for a 64 lane wavefront. If scratch was allocated for 32 lane waves
  // then the effective size for a 64 lane wave is halved.
  amd_queue_.scratch_wave64_lane_byte_size =
      uint32_t((queue_scratch_.size_per_thread * queue_scratch_.lanes_per_wave) / 64);

  // Set concurrent wavefront limits only when scratch is being used.
  COMPUTE_TMPRING_SIZE tmpring_size = {};
  if (queue_scratch_.size == 0) {
    amd_queue_.compute_tmpring_size = tmpring_size.u32All;
    return;
  }

  // Determine the maximum number of waves device can support
  const auto& agent_props = agent_->properties();
  uint32_t num_cus = agent_props.NumFComputeCores / agent_props.NumSIMDPerCU;
  uint32_t max_scratch_waves = num_cus * agent_props.MaxSlotsScratchCU;

  // Scratch is allocated program COMPUTE_TMPRING_SIZE register
  // Scratch Size per Wave is specified in terms of kilobytes
  uint32_t wave_scratch = (((queue_scratch_.lanes_per_wave *
                               queue_scratch_.size_per_thread) + 1023) / 1024);
  tmpring_size.bits.WAVESIZE = wave_scratch;
  assert(wave_scratch == tmpring_size.bits.WAVESIZE && "WAVESIZE Overflow.");
  uint32_t num_waves = queue_scratch_.size / (tmpring_size.bits.WAVESIZE * 1024);
  tmpring_size.bits.WAVES = std::min(num_waves, max_scratch_waves);
  amd_queue_.compute_tmpring_size = tmpring_size.u32All;
  return;
}

hsa_status_t AqlQueue::EnableGWS(int gws_slot_count) {
  uint32_t discard;
  auto status = hsaKmtAllocQueueGWS(queue_id_, gws_slot_count, &discard);
  if (status != HSAKMT_STATUS_SUCCESS) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
  amd_queue_.hsa_queue.type = HSA_QUEUE_TYPE_COOPERATIVE;
  return HSA_STATUS_SUCCESS;
}

}  // namespace amd
}  // namespace rocr
