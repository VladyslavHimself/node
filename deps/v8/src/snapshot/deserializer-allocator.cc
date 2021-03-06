// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/deserializer-allocator.h"

#include "src/heap/heap-inl.h"  // crbug.com/v8/8499
#include "src/heap/memory-chunk.h"
#include "src/roots/roots.h"

namespace v8 {
namespace internal {

void DeserializerAllocator::Initialize(Heap* heap) {
  heap_ = heap;
  roots_ = ReadOnlyRoots(heap);
}

// We know the space requirements before deserialization and can
// pre-allocate that reserved space. During deserialization, all we need
// to do is to bump up the pointer for each space in the reserved
// space. This is also used for fixing back references.
// We may have to split up the pre-allocation into several chunks
// because it would not fit onto a single page. We do not have to keep
// track of when to move to the next chunk. An opcode will signal this.
// Since multiple large objects cannot be folded into one large object
// space allocation, we have to do an actual allocation when deserializing
// each large object. Instead of tracking offset for back references, we
// reference large objects by index.
Address DeserializerAllocator::AllocateRaw(SnapshotSpace space, int size) {
  const int space_number = static_cast<int>(space);
  if (space == SnapshotSpace::kLargeObject) {
    // Note that we currently do not support deserialization of large code
    // objects.
    HeapObject obj;
    AlwaysAllocateScope scope(heap_);
    OldLargeObjectSpace* lo_space = heap_->lo_space();
    AllocationResult result = lo_space->AllocateRaw(size);
    obj = result.ToObjectChecked();
    deserialized_large_objects_.push_back(obj);
    return obj.address();
  } else if (space == SnapshotSpace::kMap) {
    DCHECK_EQ(Map::kSize, size);
    return allocated_maps_[next_map_index_++];
  } else {
    DCHECK(IsPreAllocatedSpace(space));
    Address address = high_water_[space_number];
    DCHECK_NE(address, kNullAddress);
    high_water_[space_number] += size;
#ifdef DEBUG
    // Assert that the current reserved chunk is still big enough.
    const Heap::Reservation& reservation = reservations_[space_number];
    int chunk_index = current_chunk_[space_number];
    DCHECK_LE(high_water_[space_number], reservation[chunk_index].end);
#endif
#ifndef V8_ENABLE_THIRD_PARTY_HEAP
    if (space == SnapshotSpace::kCode)
      MemoryChunk::FromAddress(address)
          ->GetCodeObjectRegistry()
          ->RegisterNewlyAllocatedCodeObject(address);
#endif
    return address;
  }
}

Address DeserializerAllocator::Allocate(SnapshotSpace space, int size) {
#ifdef DEBUG
  if (previous_allocation_start_ != kNullAddress) {
    // Make sure that the previous allocation is initialized sufficiently to
    // be iterated over by the GC.
    Address object_address = previous_allocation_start_;
    Address previous_allocation_end =
        previous_allocation_start_ + previous_allocation_size_;
    while (object_address != previous_allocation_end) {
      int object_size = HeapObject::FromAddress(object_address).Size();
      DCHECK_GT(object_size, 0);
      DCHECK_LE(object_address + object_size, previous_allocation_end);
      object_address += object_size;
    }
  }
#endif

  Address address;
  HeapObject obj;
  // TODO(steveblackburn) Note that the third party heap allocates objects
  // at reservation time, which means alignment must be acted on at
  // reservation time, not here.   Since the current encoding does not
  // inform the reservation of the alignment, it must be conservatively
  // aligned.
  //
  // A more general approach will be to avoid reservation altogether, and
  // instead of chunk index/offset encoding, simply encode backreferences
  // by index (this can be optimized by applying something like register
  // allocation to keep the metadata needed to record the in-flight
  // backreferences minimal).   This has the significant advantage of
  // abstracting away the details of the memory allocator from this code.
  // At each allocation, the regular allocator performs allocation,
  // and a fixed-sized table is used to track and fix all back references.
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) {
    address = AllocateRaw(space, size);
  } else if (next_alignment_ != kWordAligned) {
    const int reserved = size + Heap::GetMaximumFillToAlign(next_alignment_);
    address = AllocateRaw(space, reserved);
    obj = HeapObject::FromAddress(address);
    // If one of the following assertions fails, then we are deserializing an
    // aligned object when the filler maps have not been deserialized yet.
    // We require filler maps as padding to align the object.
    DCHECK(roots_.free_space_map().IsMap());
    DCHECK(roots_.one_pointer_filler_map().IsMap());
    DCHECK(roots_.two_pointer_filler_map().IsMap());
    obj = Heap::AlignWithFiller(roots_, obj, size, reserved, next_alignment_);
    address = obj.address();
    next_alignment_ = kWordAligned;
  } else {
    address = AllocateRaw(space, size);
  }

#ifdef DEBUG
  previous_allocation_start_ = address;
  previous_allocation_size_ = size;
#endif

  return address;
}

void DeserializerAllocator::MoveToNextChunk(SnapshotSpace space) {
  DCHECK(IsPreAllocatedSpace(space));
  const int space_number = static_cast<int>(space);
  uint32_t chunk_index = current_chunk_[space_number];
  const Heap::Reservation& reservation = reservations_[space_number];
  // Make sure the current chunk is indeed exhausted.
  CHECK_EQ(reservation[chunk_index].end, high_water_[space_number]);
  // Move to next reserved chunk.
  chunk_index = ++current_chunk_[space_number];
  CHECK_LT(chunk_index, reservation.size());
  high_water_[space_number] = reservation[chunk_index].start;
}

HeapObject DeserializerAllocator::GetMap(uint32_t index) {
  DCHECK_LT(index, next_map_index_);
  return HeapObject::FromAddress(allocated_maps_[index]);
}

HeapObject DeserializerAllocator::GetLargeObject(uint32_t index) {
  DCHECK_LT(index, deserialized_large_objects_.size());
  return deserialized_large_objects_[index];
}

HeapObject DeserializerAllocator::GetObject(SnapshotSpace space,
                                            uint32_t chunk_index,
                                            uint32_t chunk_offset) {
  DCHECK(IsPreAllocatedSpace(space));
  const int space_number = static_cast<int>(space);
  DCHECK_LE(chunk_index, current_chunk_[space_number]);
  Address address =
      reservations_[space_number][chunk_index].start + chunk_offset;
  if (next_alignment_ != kWordAligned) {
    int padding = Heap::GetFillToAlign(address, next_alignment_);
    next_alignment_ = kWordAligned;
    DCHECK(padding == 0 ||
           HeapObject::FromAddress(address).IsFreeSpaceOrFiller());
    address += padding;
  }
  return HeapObject::FromAddress(address);
}

void DeserializerAllocator::DecodeReservation(
    const std::vector<SerializedData::Reservation>& res) {
  DCHECK_EQ(0, reservations_[0].size());
  int current_space = 0;
  for (auto& r : res) {
    reservations_[current_space].push_back(
        {r.chunk_size(), kNullAddress, kNullAddress});
    if (r.is_last()) current_space++;
  }
  DCHECK_EQ(kNumberOfSpaces, current_space);
  for (int i = 0; i < kNumberOfPreallocatedSpaces; i++) current_chunk_[i] = 0;
}

bool DeserializerAllocator::ReserveSpace() {
#ifdef DEBUG
  for (int i = 0; i < kNumberOfSpaces; ++i) {
    DCHECK_GT(reservations_[i].size(), 0);
  }
#endif  // DEBUG
  DCHECK(allocated_maps_.empty());
  // TODO(v8:7464): Allocate using the off-heap ReadOnlySpace here once
  // implemented.
  if (!heap_->ReserveSpace(reservations_, &allocated_maps_)) {
    return false;
  }
  for (int i = 0; i < kNumberOfPreallocatedSpaces; i++) {
    high_water_[i] = reservations_[i][0].start;
  }
  return true;
}

bool DeserializerAllocator::ReservationsAreFullyUsed() const {
  for (int space = 0; space < kNumberOfPreallocatedSpaces; space++) {
    const uint32_t chunk_index = current_chunk_[space];
    if (reservations_[space].size() != chunk_index + 1) {
      return false;
    }
    if (reservations_[space][chunk_index].end != high_water_[space]) {
      return false;
    }
  }
  return (allocated_maps_.size() == next_map_index_);
}

void DeserializerAllocator::RegisterDeserializedObjectsForBlackAllocation() {
  heap_->RegisterDeserializedObjectsForBlackAllocation(
      reservations_, deserialized_large_objects_, allocated_maps_);
}

}  // namespace internal
}  // namespace v8
