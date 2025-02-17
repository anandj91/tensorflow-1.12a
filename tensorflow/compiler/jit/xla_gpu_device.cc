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

// Registers the XLA_GPU device, which is an XlaDevice instantiation that runs
// operators using XLA via the XLA "CUDA" (GPU) backend.

#include "tensorflow/compiler/jit/kernels/xla_ops.h"
#include "tensorflow/compiler/jit/xla_device.h"
#include "tensorflow/compiler/jit/xla_device_ops.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/common_runtime/gpu/gpu_id.h"
#include "tensorflow/core/common_runtime/gpu/gpu_id_manager.h"

namespace tensorflow {

class XlaGpuDeviceFactory : public DeviceFactory {
 public:
  Status CreateDevices(const SessionOptions& options, const string& name_prefix,
                       std::vector<Device*>* devices) override;
 private:
  // PlatformGpuId to <TfGpuId, Allocator>
  Status GetGpuDeviceAllocators(std::vector<Device*> *devices,
                                std::unordered_map<int32,
                                std::unordered_map<int32, Allocator*>> *allocators);
};

Status XlaGpuDeviceFactory::CreateDevices(const SessionOptions& options,
                                          const string& name_prefix,
                                          std::vector<Device*>* devices) {
  XlaOpRegistry::DeviceRegistration registration;
  registration.compilation_device_name = DEVICE_GPU_XLA_JIT;
  registration.requires_compilation = true;
  registration.enable_jit_by_default = false;
  registration.compile_resource_ops = true;

  static XlaDeviceOpRegistrations* registrations =
      RegisterXlaDeviceKernels(DEVICE_XLA_GPU, DEVICE_GPU_XLA_JIT);
  (void)registrations;

  std::unordered_map<int32, std::unordered_map<int32, Allocator*>> gpu_device_allocators;
  auto get_allocator_status = GetGpuDeviceAllocators(devices, &gpu_device_allocators);

  std::unique_ptr<XlaDevice> device;
  Status status =
      XlaDevice::Create("CUDA", DEVICE_XLA_GPU, 0, DEVICE_GPU_XLA_JIT, options,
                        name_prefix, registration,
                        /*transfer_as_literal=*/false,
                        /*use_multiple_streams=*/false,
                        /*shape_representation_fn=*/{},
                        /*padded_shape_fn=*/{}, &device,
                        get_allocator_status.ok() ? &gpu_device_allocators : nullptr);

  if (!status.ok()) {
    // Treat failures as non-fatal; there might not be a GPU in the machine.
    VLOG(1) << "Failed to create XLA_GPU device: " << status;
    return Status::OK();
  }

  // TODO(b/78468222): Uncomment after fixing this bug
  // status = device->UseGpuDeviceInfo();
  // if (!status.ok()) {
  //  errors::AppendToMessage(&status, "while setting up ", DEVICE_GPU_XLA_JIT,
  //                          " device");
  //  return status;
  // }

  devices->push_back(device.release());
  return Status::OK();
}

Status XlaGpuDeviceFactory::GetGpuDeviceAllocators(std::vector<Device*> *devices,
                                                   std::unordered_map<int32,
                                                   std::unordered_map<int32, Allocator*>> *allocators) {
  AllocatorAttributes allocator_attr;
  allocator_attr.set_on_host(false);
  for (auto* device : *devices) {
    if (device->device_type() == "GPU") {
      const auto &parsed_name = device->parsed_name();
      if (!parsed_name.has_id) {
        return errors::Unknown("device name has no id, device.name = ", device->name());
      }
      int32 tf_gpu_id = parsed_name.id;
      PlatformGpuId platform_gpu_id;
      TF_RETURN_IF_ERROR(
          GpuIdManager::TfToPlatformGpuId(TfGpuId(tf_gpu_id), &platform_gpu_id));

      (*allocators)[platform_gpu_id.value()][tf_gpu_id] = device->GetAllocator(allocator_attr);
    }
  }
  return Status::OK();
}


REGISTER_LOCAL_DEVICE_FACTORY(DEVICE_XLA_GPU, XlaGpuDeviceFactory);

// Kernel registrations

constexpr std::array<DataType, 13> kAllXlaGpuTypes = {
    {DT_UINT8, DT_QUINT8, DT_INT8, DT_QINT8, DT_INT32, DT_QINT32, DT_INT64,
     DT_HALF, DT_FLOAT, DT_DOUBLE, DT_COMPLEX64, DT_BOOL, DT_BFLOAT16}};

REGISTER_XLA_LAUNCH_KERNEL(DEVICE_XLA_GPU, XlaLocalLaunchOp, kAllXlaGpuTypes);
REGISTER_XLA_COMPILE_KERNEL(DEVICE_XLA_GPU, XlaCompileOp, kAllXlaGpuTypes);
REGISTER_XLA_RUN_KERNEL(DEVICE_XLA_GPU, XlaRunOp, kAllXlaGpuTypes);

REGISTER_XLA_DEVICE_KERNELS(DEVICE_XLA_GPU, kAllXlaGpuTypes);

}  // namespace tensorflow
