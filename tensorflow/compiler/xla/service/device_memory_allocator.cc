/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/device_memory_allocator.h"

#include <string>
#include <limits>

#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/lib/core/errors.h"

namespace xla {

StreamExecutorMemoryAllocator::StreamExecutorMemoryAllocator(
    const se::Platform* platform,
    absl::Span<se::StreamExecutor* const> stream_executors)
    : DeviceMemoryAllocator(platform),
      stream_executors_(stream_executors.begin(), stream_executors.end()) {}

StatusOr<OwningDeviceMemory> StreamExecutorMemoryAllocator::Allocate(
    int device_ordinal, uint64 size, bool retry_on_failure) {
  TF_ASSIGN_OR_RETURN(se::StreamExecutor * stream_executor,
                      GetStreamExecutor(device_ordinal));
  se::DeviceMemoryBase result = stream_executor->AllocateArray<uint8>(size);
  if (size > 0 && result == nullptr) {
    return ResourceExhausted(
        "Failed to allocate request for %s (%uB) on device ordinal %d",
        tensorflow::strings::HumanReadableNumBytes(size), size, device_ordinal);
  }
  return OwningDeviceMemory(result, device_ordinal, this);
}

Status StreamExecutorMemoryAllocator::Deallocate(int device_ordinal,
                                                 se::DeviceMemoryBase mem) {
  if (!mem.is_null()) {
    TF_ASSIGN_OR_RETURN(se::StreamExecutor * stream_executor,
                        GetStreamExecutor(device_ordinal));
    stream_executor->Deallocate(&mem);
  }
  return Status::OK();
}

StatusOr<se::StreamExecutor*> StreamExecutorMemoryAllocator::GetStreamExecutor(
    int device_ordinal) {
  if (device_ordinal < 0) {
    return InvalidArgument("device ordinal value (%d) must be non-negative",
                           device_ordinal);
  }
  if (device_ordinal >= stream_executors_.size()) {
    return InvalidArgument(
        "device ordinal value (%d) >= number of devices (%u)", device_ordinal,
        stream_executors_.size());
  }
  if (stream_executors_[device_ordinal] == nullptr) {
    return NotFound("Device %s:%d present but not supported",
                    platform()->Name(), device_ordinal);
  }
  return stream_executors_[device_ordinal];
}

bool StreamExecutorMemoryAllocator::AllowsAsynchronousDeallocation() const {
  return false;
}

AllocatorBackedDeviceMemoryAllocator::AllocatorBackedDeviceMemoryAllocator(
    const se::Platform* platform,
    std::unordered_map<int32,
    std::unordered_map<int32, tensorflow::Allocator*>> *allocator_map):
    DeviceMemoryAllocator(platform) {
  for (auto &virtual_device_allocator_pair : *allocator_map) {
    int32 platform_gpu_id = virtual_device_allocator_pair.first;
    auto &virtual_device_allocator_map = virtual_device_allocator_pair.second;
    int32 min_tf_gpu_id = std::numeric_limits<int>::max();
    tensorflow::Allocator* allocator = nullptr;
    for (auto &virtual_device_allocator : virtual_device_allocator_map) {
      if (virtual_device_allocator.first < min_tf_gpu_id ||
          (virtual_device_allocator.first == min_tf_gpu_id &&
           allocator == nullptr)) {
        min_tf_gpu_id = virtual_device_allocator.first;
        allocator = virtual_device_allocator.second;
      }
    }
    allocator_map_[platform_gpu_id] = allocator;
  }
}
StatusOr<OwningDeviceMemory>
AllocatorBackedDeviceMemoryAllocator::Allocate(int device_ordinal, uint64 size,
                                               bool retry_on_failure) {
  LOG(INFO) << __func__ << " from AllocatorBackedDeviceMemoryAllocator";
  auto allocator_iter = allocator_map_.find(device_ordinal);
  if (allocator_iter == allocator_map_.end()) {
    return tensorflow::errors::NotFound("device_ordinal ", device_ordinal, " not found");
  }

  void *memory = allocator_iter->second->Allocate<uint8>(size);
  if (memory == nullptr) {
    return ResourceExhausted(
        "Failed to allocate request for %s (%uB) on device ordinal %d",
        tensorflow::strings::HumanReadableNumBytes(size), size, device_ordinal);
  }

  auto result = se::DeviceMemory<uint8>::MakeFromByteSize(memory, size);
  return OwningDeviceMemory(result, device_ordinal, this);
}

Status
AllocatorBackedDeviceMemoryAllocator::Deallocate(int device_ordinal,
                                                 se::DeviceMemoryBase mem) {
  auto allocator_iter = allocator_map_.find(device_ordinal);
  if (allocator_iter == allocator_map_.end()) {
    return tensorflow::errors::NotFound("device_ordinal ", device_ordinal, " not found");
  }

  allocator_iter->second->Deallocate<uint8>(reinterpret_cast<uint8*>(mem.opaque()),
                                            mem.size());
  return Status::OK();
}

bool
AllocatorBackedDeviceMemoryAllocator::AllowsAsynchronousDeallocation() const {
  return false;
}


}  // namespace xla
