// Copyright (c) 2023, DeepLink.

#include "DIPUCachingAllocator.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

#include <c10/core/Device.h>
#include <c10/core/DeviceType.h>

#include "csrc_dipu/base/basedef.h"
#include "csrc_dipu/runtime/core/DIPUEvent.h"
#include "csrc_dipu/runtime/devproxy/deviceproxy.h"
#include "csrc_dipu/utils/env.hpp"

namespace dipu {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex DIPURawDeviceAllocator::mutex_;

namespace {

// using RegisteredAllocator = std::map<c10::DeviceType, std::map<std::string,
// std::tuple<c10::Allocator*, uint8_t>>>; using RegisteredAllocator =
// std::map<c10::DeviceType, std::map<std::string,
// std::tuple<std::array<c10::Allocator*, 16>, uint8_t>>>;

using RegisteredAllocator = std::map<
    c10::DeviceType,
    std::map<std::string,
             std::tuple<std::function<c10::Allocator*(int)>, uint8_t>>>;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<RegisteredAllocator> gDIPURegisteredAllocatorPtr;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex dipu_register_allocator_mutex;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::set<c10::Allocator*> used_allocator;

}  // namespace

constexpr const char* dipu_default_memcaching_algorithm = "BF";

const std::string dipu_device_memcaching_algorithm = []() {
  const char* env = std::getenv("DIPU_DEVICE_MEMCACHING_ALGORITHM");
  return env ? env : dipu_default_memcaching_algorithm;
}();

const std::string dipu_host_memcaching_algorithm = []() {
  const char* env = std::getenv("DIPU_HOST_MEMCACHING_ALGORITHM");
  return env ? env : dipu_default_memcaching_algorithm;
}();

void setAllocator(const std::string& name, c10::DeviceType device_type,
                  const std::function<c10::Allocator*(int)>& allocator_getter,
                  uint8_t priority) {
  std::lock_guard<std::mutex> lock(dipu_register_allocator_mutex);
  if (!gDIPURegisteredAllocatorPtr) {
    gDIPURegisteredAllocatorPtr = std::make_unique<RegisteredAllocator>();
  }
  auto& gDIPURegisteredAllocator = *gDIPURegisteredAllocatorPtr;
  if (gDIPURegisteredAllocator[device_type].count(name) <= 0) {
    gDIPURegisteredAllocator[device_type][name] =
        std::make_tuple(allocator_getter, priority);
  } else {
    if (std::get<1>(gDIPURegisteredAllocator[device_type][name]) < priority) {
      gDIPURegisteredAllocator[device_type][name] =
          std::make_tuple(allocator_getter, priority);
    } else {
      TORCH_CHECK(false,
                  "A higher priority allocator is already registered for the "
                  "same device:",
                  device_type, name, priority);
    }
  }
}

namespace {

int getDeviceIndex(const c10::Device& device, int host_index) {
  if (device.is_cpu()) {
    return host_index;
  }
  if (device.has_index()) {
    return device.index();
  }
  return devproxy::current_device();
}

c10::Allocator* createAllocator(const c10::Device& device) {
  c10::DeviceType device_type = device.type();
  c10::Allocator* result = nullptr;
  auto& gDIPURegisteredAllocator = *gDIPURegisteredAllocatorPtr;
  const std::string algorithm =
      (device_type == dipu::DIPU_DEVICE_TYPE ? dipu_device_memcaching_algorithm
                                             : dipu_host_memcaching_algorithm);
  if (gDIPURegisteredAllocator[device_type].count(algorithm) > 0) {
    auto allocator_geter =
        std::get<0>(gDIPURegisteredAllocator[device_type][algorithm]);
    int device_index = getDeviceIndex(device, 0);

    auto allocator = allocator_geter(device_index);
    if (device_type == dipu::DIPU_DEVICE_TYPE) {
      used_allocator.insert(allocator);
    }
    return allocator;
  }
  TORCH_CHECK(false,
              "No allocator found for the device using the given algorithm:",
              device_type, dipu_device_memcaching_algorithm);
  return nullptr;
}

}  // namespace

c10::Allocator* getAllocator(const c10::Device& device) {
  // allocator_lookup_table[device_index] == device allocator
  // allocator_lookup_table[device_count] == host allocator
  static const int device_count = devproxy::getDeviceCount();
  static const int host_index = device_count;
  static std::vector<c10::Allocator*> allocator_lookup_table(device_count + 1);
  int device_index = getDeviceIndex(device, host_index);
  auto& allocator = allocator_lookup_table[device_index];
  if (allocator == nullptr) {
    allocator = createAllocator(device);
  }
  return allocator;
}

c10::Allocator* getAllocator(c10::DeviceType device_type) {
  return getAllocator(c10::Device(device_type));
}

void emptyCachedMem() {
  auto function_name = __FUNCTION__;
  auto empty_allocator_cache = [&function_name](auto allocator) {
    auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
    DIPU_DEBUG_ALLOCATOR(8, function_name
                                << " allocator:" << allocator
                                << ", cached_allocator:" << cached_allocator);
    if (cached_allocator != nullptr) {
      cached_allocator->empty_cache();
    }
  };
  for (auto& allocator : used_allocator) {
    empty_allocator_cache(allocator);
  }
}

void releaseAllDeviceMem() {
  auto release_allocator_memory = [](auto allocator) {
    auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
    DIPU_DEBUG_ALLOCATOR(8, "release_allocator_memory: allocator:"
                                << allocator
                                << ", cached_allocator:" << cached_allocator);
    if (cached_allocator != nullptr) {
      cached_allocator->release_all_memory();
    }
  };
  for (auto& allocator : used_allocator) {
    release_allocator_memory(allocator);
  }
}

size_t memoryReserved(const c10::Device& device) {
  c10::Allocator* allocator = getAllocator(device);
  auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
  if (cached_allocator != nullptr) {
    return cached_allocator->memory_reserved();
  }
  return 0;
}

size_t memoryAllocated(const c10::Device& device) {
  c10::Allocator* allocator = getAllocator(device);
  auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
  if (cached_allocator != nullptr) {
    return cached_allocator->memory_allocated();
  }
  return 0;
}

size_t maxMemoryReserved(const c10::Device& device) {
  c10::Allocator* allocator = getAllocator(device);
  auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
  if (cached_allocator != nullptr) {
    return cached_allocator->max_memory_reserved();
  }
  return 0;
}

size_t maxMemoryAllocated(const c10::Device& device) {
  c10::Allocator* allocator = getAllocator(device);
  auto cached_allocator = dynamic_cast<CacheAllocator*>(allocator);
  if (cached_allocator != nullptr) {
    return cached_allocator->max_memory_allocated();
  }
  return 0;
}

void recordStream(const c10::DataPtr& ptr, const DIPUStream& stream) {
  using pointer = CacheAllocator::DataPtrContextBase*;
  if (auto ctx = static_cast<pointer>(ptr.get_context())) {
    ctx->streams().insert(stream);
  }
}

void recordStream(const at::Tensor& tensor, const DIPUStream& stream) {
  dipu::recordStream(tensor.storage().data_ptr(), stream);
}

namespace {
class DIPUDeviceCachingProxy : public c10::Allocator {
  c10::DeviceType device_type_;

 public:
  explicit DIPUDeviceCachingProxy(c10::DeviceType device_type)
      : device_type_(device_type) {}

  ~DIPUDeviceCachingProxy() override = default;

  c10::DataPtr allocate(size_t size) const override {
    auto currentStream = getCurrentDIPUStream();
    auto defaultStream = getDefaultDIPUStream();
    DIPUEvent event;
    std::cout << "c10::DataPtr allocate" << std::endl;
    if (currentStream != defaultStream) {
      // When allocating memory to a non-default stream, since record_stream is
      // not performed on the default stream, the non-default stream needs to
      // wait for the operation on the default stream to be completed. After
      // adding non-default stream and other default stream operations here, the
      // upper layer does not need to manually add a wait for the default stream
      // when allocating memory on the non-default stream.
      event.record(defaultStream);
      event.wait(currentStream);
      std::cout << "currentStream wait " << std::endl;
    }
    return getAllocator(device_type_)->allocate(size);
  }

  c10::DeleterFnPtr raw_deleter() const override {
    return getAllocator(device_type_)->raw_deleter();
  }
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DIPUDeviceCachingProxy dipu_default_device_allocator(dipu::DIPU_DEVICE_TYPE);
};  // namespace

void initCachedAllocator() {
  // Make the c10::GetAllocator interface available
  constexpr int kPriority = 255;
  c10::SetAllocator(dipu::DIPU_DEVICE_TYPE, &dipu_default_device_allocator,
                    kPriority);
  c10::SetAllocator(c10::DeviceType::CUDA, &dipu_default_device_allocator,
                    kPriority);
}

}  // namespace dipu
