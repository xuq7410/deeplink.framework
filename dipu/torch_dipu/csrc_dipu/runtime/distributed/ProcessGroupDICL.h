// Copyright (c) 2023, DeepLink.
#pragma once

#include <chrono>
#include <unordered_map>
#include <vector>

#include <c10/core/Device.h>
#include <torch/csrc/distributed/c10d/Backend.hpp>
#include <torch/csrc/distributed/c10d/Store.hpp>
#include <torch/csrc/distributed/c10d/Work.hpp>

#include "csrc_dipu/base/basedef.h"
#include "csrc_dipu/runtime/core/DIPUEvent.h"
#include "csrc_dipu/runtime/core/DIPUStream.h"
#include "csrc_dipu/vendor/vendorapi.h"

#include "DICLUtils.hpp"

namespace dipu {

using c10d::AllgatherOptions;
using c10d::AllreduceOptions;
using c10d::Backend;
using c10d::BarrierOptions;
using c10d::BroadcastOptions;
using c10d::GatherOptions;
using c10d::OpType;
using c10d::ReduceOptions;
using c10d::ReduceScatterOptions;
using c10d::Store;
using c10d::Work;

// Environment variable which controls whether or not wait() is blocking or
// non-blocking.
constexpr const char* DICL_BLOCKING_WAIT = "DICL_BLOCKING_WAIT";
constexpr int64_t diclSyncBusyWaitMillis = 30;

/**
 * ProcessGroupDICL implements DICLbindings for c10d.
 *
 * @author fandaoyi
 *
 * All functions of this class are expected to be called in the same order
 * across all processes in the process group. This is the only way that we can
 * guarantee to match up the same calls among all processes.
 *
 * All DICLfunctions provided by this class are asynchronous functions. More
 * specifically, each DICLcall is scheduled on a separate DIPU stream that is
 * different from the current DIPU stream. This is for the purpose of achieving
 * potentially concurrency and better performance. As a result, it is the
 * callers' responsibility to make sure that the DIPU stream their code works on
 * needs to wait for the DICLoperation from this class. This can be done by
 * calling either WorkDICL::wait() or WorkDICL::synchronize(), both achieves the
 * same functionality and are synonyms.
 *
 * @note Every single DICL or DIPU failure will simply raise std::runtime_error.
 * Therefore, WorkDICL::exception() is not supported, and WorkDICL::isSuccess()
 * will always return true if the operation has completed.
 *
 * @warning Not supporting gather and all _coalesced functions now. We will add
 * them in the future if needed.
 *
 * Example on using DICL process group:
 *
 *     ProcessGroupDICL pg(store, rank, size);
 *     std::shared_ptr<WorkDICL> work = pg.allreduce(tensors);
 *     // At this point, DICL kernel has already been queued successfully.
 *     // Now, let current stream wait for DICL to finish (async).
 *     work->wait()
 *     // Now continue on other work in current stream.
 */
class DIPU_API ProcessGroupDICL : public Backend {
 public:
  class WorkDICL : public Work {
   public:
    // Constructor takes a list of dicl comms
    WorkDICL(std::vector<std::shared_ptr<DICLComm>>& comms, bool blockingWait,
             std::chrono::milliseconds opTimeout)
        : diclComms_(comms),
          blockingWait_(blockingWait),
          opTimeout_(opTimeout),
          workStartTime_(std::chrono::steady_clock::now()) {
      workEvents_.resize(diclComms_.size());
    }

    ~WorkDICL() override = default;

    // Checks if request has completed. In this specific case of DICL, it checks
    // if the DICL operation has completed on the DIPU in its own DICL queue.
    // Non-blocking operation.
    bool isCompleted() override;

    bool isSuccess() const override;

    void record();

    // Same as calling synchronize() for DICL work.
    bool wait(std::chrono::milliseconds timeout /* = kBackendDefaultTimeout */)
        override;

    // Let current stream wait on the completing of the DICL work
    // Throws on exceptions
    void synchronize() override;

    std::vector<at::Tensor> result() override;

    c10::intrusive_ptr<c10::ivalue::Future> getFuture() override;

   protected:
    // Store a reference to DICL collective's outputs, used by result and to
    // give a more descriptive message when representing the Work as a string.
    std::shared_ptr<std::vector<at::Tensor>> outputs_;

    // The future returned by getFuture.
    c10::intrusive_ptr<at::ivalue::Future> future_;

    // The DICL communicators used for this work item.
    std::vector<std::shared_ptr<DICLComm>> diclComms_;

    // The DIPU events used to sync DICL work on comm stream
    std::vector<DIPUEvent> workEvents_;

    // Just checks whether DIPU execution has completed, without modifying
    // exception_ptr.
    bool finishedDICLExecutionInternal() const;
    bool barrier_ = false;

    // Clone of blockingWait_ from ProcessGroupDICL.
    bool blockingWait_ = false;

    // Clone of opTimeout_ from ProcessGroupHCCL.
    std::chrono::milliseconds opTimeout_;

    // Time point representing when the work started.
    std::chrono::time_point<std::chrono::steady_clock> workStartTime_;

   private:
    friend class ProcessGroupDICL;
  };

  struct DIPU_API Options : Backend::Options {
    // NOTE: timeout in ProcessGroupNCCL::Options denote the timeout for
    // operations. This is only used when blockingWait_ is enabled.
    explicit Options() : Backend::Options(DICL_BACKEND_NAME) {}
  };

  // If you wish to create multiple process groups, each with a potentially
  // different rank and size, you can do so by passing a new store instance
  // to each one. If you have only a single store object, you can
  // use the `c10d::PrefixStore` to derive scoped instances.
  // This is also what the Python API in torch.distributed does.
  //
  // The process group instance keeps a reference to the store because
  // it may be used long after the constructor runs. In fact, the constructor
  // doesn't create any DICL communicators. A single DICL communicator can
  // only be used on a specific set of devices, and are therefore created
  // on-demand when a collective runs. If another collective is executed later,
  // against a different set of devices, the process group creates another DICL
  // communicator. These DICL communicators are cached and reused if possible.
  ProcessGroupDICL(const c10::intrusive_ptr<Store>& store, int rank, int size);

  ~ProcessGroupDICL() override;

  // NOLINTNEXTLINE(readability-const-return-type) just follow parent class.
  const std::string getBackendName() const override {
    return DICL_BACKEND_NAME;
  }

  c10::intrusive_ptr<Work> broadcast(
      std::vector<at::Tensor>& tensors,
      const BroadcastOptions& opts /* = BroadcastOptions() */) override;

  c10::intrusive_ptr<Work> allreduce(
      std::vector<at::Tensor>& tensors,
      const AllreduceOptions& opts /* = AllreduceOptions() */) override;

  c10::intrusive_ptr<Work> reduce(
      std::vector<at::Tensor>& tensors,
      const ReduceOptions& opts /* = ReduceOptions() */) override;

  c10::intrusive_ptr<Work> gather(
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      const GatherOptions& opts /* = GatherOptions() */) override;

  c10::intrusive_ptr<Work> allgather(
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      const AllgatherOptions& opts /* = AllgatherOptions() */) override;

  c10::intrusive_ptr<Work> _allgather_base(
      at::Tensor& outputs, at::Tensor& inputs,
      const AllgatherOptions& opts /* = AllgatherOptions() */) override;

  c10::intrusive_ptr<Work> reduce_scatter(
      std::vector<at::Tensor>& outputs,
      std::vector<std::vector<at::Tensor>>& inputs,
      const ReduceScatterOptions& opts /* = ReduceScatterOptions() */) override;

  c10::intrusive_ptr<Work> _reduce_scatter_base(
      at::Tensor& output, at::Tensor& input,
      const ReduceScatterOptions& opts /* = ReduceScatterOptions() */) override;

  c10::intrusive_ptr<Work> send(std::vector<at::Tensor>& tensors, int dstRank,
                                int tag) override;

  c10::intrusive_ptr<Work> recv(std::vector<at::Tensor>& tensors, int srcRank,
                                int tag) override;

  c10::intrusive_ptr<Work> barrier(
      const BarrierOptions& opts /* = BarrierOptions() */) override;

  c10::intrusive_ptr<Store> getStore() { return this->store_; }

 protected:
  // different device may need extend this func to do device specific check
  virtual void checkDeviceTensors(const std::vector<at::Tensor>& tensors);

  // Helper that broadcasts DICL clique ID to all ranks through the store
  virtual void broadcastUniqueID(commUniqueId* uniqueId,
                                 const std::string& storeKey, int commRank);

  // Helper that either looks up the cached DICL communicators or creates
  // a new set of DICL communicators as a cache entry
  virtual std::vector<std::shared_ptr<DICLComm>>& getDICLComms(
      const std::string& localCommsKey, const std::vector<at::Device>& devices,
      int commsRank, OpType opType);

  template <typename Fn>
  c10::intrusive_ptr<Work> collective(std::vector<at::Tensor>& input,
                                      std::vector<at::Tensor>& output, Fn fn,
                                      OpType opType);

  template <typename Fn, typename PreProcess, typename PostProcess>
  c10::intrusive_ptr<Work> collective(std::vector<at::Tensor>& inputs,
                                      std::vector<at::Tensor>& outputs, Fn fn,
                                      PreProcess pre, PostProcess post,
                                      OpType opType);

  template <typename Fn, typename PreProcess, typename PostProcess>
  c10::intrusive_ptr<Work> pointToPoint(std::vector<at::Tensor>& inputs,
                                        std::vector<at::Tensor>& outputs,
                                        int peer, Fn fn, PreProcess pre,
                                        PostProcess post, OpType opType);

  template <typename Fn, typename PreProcess, typename PostProcess>
  c10::intrusive_ptr<Work> doComm(
      std::vector<at::Tensor>& inputs, std::vector<at::Tensor>& outputs,
      std::vector<std::shared_ptr<DICLComm>>& diclComms,
      const std::vector<at::Device>& devices, Fn fn, PreProcess pre,
      PostProcess post, OpType opType);

  // The store is used to broadcast the DICL unique ID of rank 0.
  c10::intrusive_ptr<Store> store_;

  // The number of DICL communicators that have been created during
  // the lifetime of this process group. This sequence number is
  // used to scope keys used in the store.
  uint64_t diclCommCounter_{0};

  // The DICL communicator that the process group has cached.
  // The key is a list of DIPU devices that an operation is operating on
  // The DIPU devices are stored in a device sequence and the cache DICL
  // communicator is associated with this DIPU device sequence
  //
  // e.g. If the process group op only uses device 0, then the value of
  // the used device string stored (value of the hashmap) would be "0".
  //
  //      If the process group op uses device 0 - 7 and the each tensor of the
  //      input tensor list is on device, 0, 1, 2, 3, 4, 5, 6, 7 separately,
  //      then the value of the used device string (key) stored would be
  //      "0,1,2,3,4,5,6,7"
  //
  //      If the process group op uses device 0 - 7 and the each tensor of the
  //      input tensor list is on device, 0, 4, 5, 6, 7, 1, 2, 3 separately,
  //      then the value of the used device string stored would be
  //      "0,4,5,6,7,1,2,3"
  //
  //      Note that the order of the device for the tensor list matters.
  std::unordered_map<std::string, std::vector<std::shared_ptr<DICLComm>>>
      devDICLCommsMap_;

  // Mutex to guard devDICLCommMap_.
  std::mutex devDICLCommMapLock_;

  // Device Indexes used for all collectives in this group
  std::set<int> usedDeviceIdxs_;

  // Whether or not wait() and synchronize() are blocking operations that wait
  // for the operation to complete.
  bool blockingWait_ = false;

  std::chrono::milliseconds opTimeout_ = kBackendDefaultTimeout;
};

namespace dicl_hook {

/**
 * @brief Preprocessing function to be executed before the allReduce operation.
 *
 * @param comms A vector containing all DICLComm objects, each representing a
 * communication channel.
 * @param inputs A vector containing all input tensors.
 * @param outputs A vector containing all output tensors.
 *
 * If this function is not defined, no preprocessing will be performed before
 * the allReduce operation.
 */
DIPU_WEAK void allReducePreFn(std::vector<std::shared_ptr<DICLComm>>& comms,
                              std::vector<at::Tensor>& inputs,
                              std::vector<at::Tensor>& outputs);

/**
 * @brief Postprocessing function to be executed after the allReduce operation.
 *
 * @param comms A vector containing all DICLComm objects, each representing a
 * communication channel.
 * @param inputs A vector containing all input tensors.
 * @param outputs A vector containing all output tensors.
 *
 * If this function is not defined, no postprocessing will be performed after
 * the allReduce operation.
 */
DIPU_WEAK void allReducePostFn(std::vector<std::shared_ptr<DICLComm>>& comms,
                               std::vector<at::Tensor>& inputs,
                               std::vector<at::Tensor>& outputs);

/**
 * @brief Preprocessing function to be executed before the reduce operation.
 *
 * @param comms A vector containing all DICLComm objects, each representing a
 * communication channel.
 * @param inputs A vector containing all input tensors.
 * @param outputs A vector containing all output tensors.
 *
 * If this function is not defined, no preprocessing will be performed before
 * the reduce operation.
 */
DIPU_WEAK void reducePreFn(std::vector<std::shared_ptr<DICLComm>>& comms,
                           std::vector<at::Tensor>& inputs,
                           std::vector<at::Tensor>& outputs);

/**
 * @brief Postprocessing function to be executed after the reduce operation.
 *
 * @param comms A vector containing all DICLComm objects, each representing a
 * communication channel.
 * @param inputs A vector containing all input tensors.
 * @param outputs A vector containing all output tensors.
 *
 * If this function is not defined, no postprocessing will be performed after
 * the reduce operation.
 */
DIPU_WEAK void reducePostFn(std::vector<std::shared_ptr<DICLComm>>& comms,
                            std::vector<at::Tensor>& inputs,
                            std::vector<at::Tensor>& outputs);

}  // namespace dicl_hook

c10::intrusive_ptr<ProcessGroupDICL> createProcessGroupDICL(
    const c10::intrusive_ptr<::c10d::Store>& store, int rank, int size,
    const std::chrono::milliseconds& timeout);

}  // namespace dipu
