#define TI_RUNTIME_HOST
#include "amdgpu_context.h"

#include <unordered_map>
#include <mutex>

#include "taichi/util/lang_util.h"
#include "taichi/system/threading.h"
#include "taichi/rhi/amdgpu/amdgpu_driver.h"
#include "taichi/rhi/amdgpu/amdgpu_profiler.h"
#include "taichi/analysis/offline_cache_util.h"
#include "taichi/util/offline_cache.h"

namespace taichi {
namespace lang {

AMDGPUContext::AMDGPUContext()
    : driver_(AMDGPUDriver::get_instance_without_context()) {
  dev_count_ = 0;
  driver_.init(0);
  driver_.device_get_count(&dev_count_);
  driver_.device_get(&device_, 0);

  char name[128];
  driver_.device_get_name(name, 128, device_);

  TI_TRACE("Using AMDGPU device [id=0]: {}", name);

  driver_.context_create(&context_, 0, device_);

  const auto GB = std::pow(1024.0, 3.0);
  TI_TRACE("Total memory {:.2f} GB; free memory {:.2f} GB",
           get_total_memory() / GB, get_free_memory() / GB);

  void *hip_device_prop = std::malloc(HIP_DEVICE_PROPERTIES_STRUCT_SIZE);
  driver_.device_get_prop(hip_device_prop, device_);

  // Obtain compute capability and arch name using hip_device_prop.
  int runtime_version;
  driver_.runtime_get_version(&runtime_version);

  // Future-proof way of getting compute_capability_ and mcpu_.
  //
  // hipGetDeviceProperties has two versions due to an ABI-breaking change in
  // ROCm 6: hipGetDevicePropertiesR0000 and hipGetDevicePropertiesR0600. The
  // former is for ROCm 5 and the latter is for ROCm 6. However, even in ROCm
  // 6, the ABI symbol hipGetDeviceProperties in libamdhip64.so actually maps
  // to hipGetDevicePropertiesR0000, the ROCm 5 version! See this commit for
  // more details:
  // https://github.com/ROCm/clr/commit/3e72b8d1e12347914974d4e7124cb205796f39f6
  //
  // IMO this is a bug, so in case of this behavior getting changed, let's first
  // treat hipGetDeviceProperties as hipGetDevicePropertiesR0000, and if we're
  // not getting a proper mcpu_, then we treat it as
  // hipGetDevicePropertiesR0600.
  //
  // We can do this safely because hipDeviceProp_t is larger in R0600 then
  // R0000, so using the field offset values in ROCm 5 on a ROCm 6 struct can
  // never cause an out-of-bounds access.
  compute_capability_ =
      (*((int *)(hip_device_prop) + int(HIP_DEVICE_MAJOR))) * 100;
  compute_capability_ +=
      (*((int *)(hip_device_prop) + int(HIP_DEVICE_MINOR))) * 10;
  mcpu_ =
      std::string((char *)((int *)hip_device_prop + HIP_DEVICE_GCN_ARCH_NAME));
  // Basic sanity check on mcpu_ to ensure we're calling R0000 instead of R0600
  if (mcpu_.empty() || mcpu_.substr(0, 3) != "gfx") {
    // ROCm 6 starts with 60000000
    if (runtime_version < 60000000) {
      TI_ERROR(
          "hipGetDevicePropertiesR0000 returned an invalid mcpu_ but HIP "
          "version {} is not ROCm 6",
          runtime_version);
    }
    compute_capability_ =
        (*((int *)(hip_device_prop) + int(HIP_DEVICE_MAJOR_6))) * 100;
    compute_capability_ +=
        (*((int *)(hip_device_prop) + int(HIP_DEVICE_MINOR_6))) * 10;
    mcpu_ = std::string(
        (char *)((int *)(hip_device_prop) + int(HIP_DEVICE_GCN_ARCH_NAME_6)));
  }
  // Strip out xnack/ecc from name
  mcpu_ = mcpu_.substr(0, mcpu_.find(":"));
  std::free(hip_device_prop);

  TI_TRACE("Emitting AMDGPU code for {}", mcpu_);
}

std::size_t AMDGPUContext::get_total_memory() {
  std::size_t ret, _;
  driver_.mem_get_info(&_, &ret);
  return ret;
}

std::size_t AMDGPUContext::get_free_memory() {
  std::size_t ret, _;
  driver_.mem_get_info(&ret, &_);
  return ret;
}

std::string AMDGPUContext::get_device_name() {
  constexpr uint32_t kMaxNameStringLength = 128;
  char name[kMaxNameStringLength];
  driver_.device_get_name(name, kMaxNameStringLength /*=128*/, device_);
  std::string str(name);
  return str;
}

int AMDGPUContext::get_args_byte(std::vector<int> arg_sizes) {
  int byte_cnt = 0;
  int naive_add = 0;
  for (auto &size : arg_sizes) {
    naive_add += size;
    if (size < 32) {
      if ((byte_cnt + size) % 32 > (byte_cnt) % 32 ||
          (byte_cnt + size) % 32 == 0)
        byte_cnt += size;
      else
        byte_cnt += 32 - byte_cnt % 32 + size;
    } else {
      if (byte_cnt % 32 != 0)
        byte_cnt += 32 - byte_cnt % 32 + size;
      else
        byte_cnt += size;
    }
  }
  return byte_cnt;
}

void AMDGPUContext::pack_args(std::vector<void *> arg_pointers,
                              std::vector<int> arg_sizes,
                              char *arg_packed) {
  int byte_cnt = 0;
  for (int ii = 0; ii < arg_pointers.size(); ii++) {
    // The parameter is taken as a vec4
    if (arg_sizes[ii] < 32) {
      if ((byte_cnt + arg_sizes[ii]) % 32 > (byte_cnt % 32) ||
          (byte_cnt + arg_sizes[ii]) % 32 == 0) {
        std::memcpy(arg_packed + byte_cnt, arg_pointers[ii], arg_sizes[ii]);
        byte_cnt += arg_sizes[ii];
      } else {
        int padding_size = 32 - byte_cnt % 32;
        byte_cnt += padding_size;
        std::memcpy(arg_packed + byte_cnt, arg_pointers[ii], arg_sizes[ii]);
        byte_cnt += arg_sizes[ii];
      }
    } else {
      if (byte_cnt % 32 != 0) {
        int padding_size = 32 - byte_cnt % 32;
        byte_cnt += padding_size;
        std::memcpy(arg_packed + byte_cnt, arg_pointers[ii], arg_sizes[ii]);
        byte_cnt += arg_sizes[ii];
      } else {
        std::memcpy(arg_packed + byte_cnt, arg_pointers[ii], arg_sizes[ii]);
        byte_cnt += arg_sizes[ii];
      }
    }
  }
}

void AMDGPUContext::launch(void *func,
                           const std::string &task_name,
                           const std::vector<void *> &arg_pointers,
                           const std::vector<int> &arg_sizes,
                           unsigned grid_dim,
                           unsigned block_dim,
                           std::size_t dynamic_shared_mem_bytes) {
  KernelProfilerBase::TaskHandle task_handle;
  // Kernel launch
  if (profiler_) {
    KernelProfilerAMDGPU *profiler_amdgpu =
        dynamic_cast<KernelProfilerAMDGPU *>(profiler_);
    std::string primal_task_name, key;
    bool valid =
        offline_cache::try_demangle_name(task_name, primal_task_name, key);
    profiler_amdgpu->trace(task_handle, valid ? primal_task_name : task_name,
                           func, grid_dim, block_dim, 0);
  }
  auto pack_size = get_args_byte(arg_sizes);
  char *packed_arg = (char *)std::malloc(pack_size);
  pack_args(arg_pointers, arg_sizes, packed_arg);
  if (grid_dim > 0) {
    std::lock_guard<std::mutex> _(lock_);
    void *config[] = {(void *)0x01, (void *)packed_arg, (void *)0x02,
                      (void *)&pack_size, (void *)0x03};
    driver_.launch_kernel(func, grid_dim, 1, 1, block_dim, 1, 1,
                          dynamic_shared_mem_bytes, nullptr, nullptr,
                          reinterpret_cast<void **>(&config));
  }
  std::free(packed_arg);

  if (profiler_)
    profiler_->stop(task_handle);

  if (debug_) {
    driver_.stream_synchronize(nullptr);
  }
}

AMDGPUContext::~AMDGPUContext() {
}

AMDGPUContext &AMDGPUContext::get_instance() {
  static auto context = new AMDGPUContext();
  return *context;
}

}  // namespace lang
}  // namespace taichi
