# Copyright (c) 2023, DeepLink.
diopi_wrapper_file_template_content = """// autogened file
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

#include <ATen/ExpandUtils.h>
#include <ATen/Functions.h>
#include <ATen/ExpandUtils.h>
#include <ATen/Tensor.h>
#include <ATen/core/ATen_fwd.h>
#include <ATen/core/Generator.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/core/List.h>
#include <ATen/core/TensorBody.h>
#include <ATen/native/ReduceOpsUtils.h>
#include <ATen/ops/empty.h>
#include <ATen/ops/empty_like.h>
#include <ATen/ops/scalar_tensor.h>
#include <ATen/ops/to_native.h>
#include <ATen/ops/zeros.h>
#include <c10/core/MemoryFormat.h>
#include <c10/core/ScalarType.h>
#include <c10/core/SymInt.h>
#include <c10/core/SymIntArrayRef.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Exception.h>
#include <c10/util/Optional.h>
#include <c10/util/SmallVector.h>
#include <c10/util/accumulate.h>
#include <c10/util/string_view.h>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/types.h>
#include <type_traits>

#include <diopi/diopirt.h>
#include <diopi/functions.h>

#include "csrc_dipu/aten/RegisterDIPU.hpp"
#include "csrc_dipu/aten/ops/AutoCompareUtils.hpp"
#include "csrc_dipu/aten/ops/DIPUCopy.hpp"
#include "csrc_dipu/aten/ops/NodispatchUtils.hpp"
#include "csrc_dipu/aten/ops/OpUtils.hpp"
#include "csrc_dipu/base/basedef.h"
#include "csrc_dipu/diopirt/diopirt_impl.h"
#include "csrc_dipu/profiler/profiler.h"
#include "csrc_dipu/runtime/core/DIPUGeneratorImpl.h"
#include "csrc_dipu/runtime/core/DIPUStream.h"

#include "CustomFallbackFunctions.hpp"

$header_include_code

// NOTE: Some kernels (e.g. _foreach_add_.List) have custom codes at the
// beginning ending with early return. This is a workaround intended to skip
// some of the autogened codes (e.g. type cast, calling DIOPI, etc.).
//
// NOLINTBEGIN(readability-redundant-control-flow)

namespace dipu {

namespace native {

using dipu::diopi_helper::toDiopiGeneratorHandle;
using dipu::diopi_helper::toDiopiSize;
using dipu::diopi_helper::toDiopiRoundMode;

$functions_code

}  // namespace native
}  // namespace dipu

// NOLINTEND(readability-redundant-control-flow)

namespace at {

DIPU_LIBRARY_IMPL(aten, DIPU_DEVICE_TYPE_MACRO, m) {
  $op_register_code
}

DIPU_LIBRARY_IMPL(aten, DIPU_AUTOGRAD_DEVICE_TYPE_MACRO, m) {
  $autograd_op_register_code
}

}  // namespace at

"""

diopi_wrapper_function_template_content = """
//  $comment
$cppsignautre {
  dipu::profile::RecordBlockCreator _(__FUNCTION__);
  $custom_code_at_the_beginning

  ::diopiContext context(dipu::getCurrentDIPUStream().rawstream());
  auto ctx = &context;

  //if (dipu::devproxy::current_device() == 0)
  //  printf("%s,stream:%d \\n",  __FUNCTION__,ctx->stream);

  $input_process_code

  $output_process_code

  $attrs_process_code

  $device_check_code

  $custom_code_before_call_diopi

  dipu::profile::RecordBlockCreator dipuRecorder(R"($interface_name)");
  ::diopiError_t ret = $diopi_fun_call_code
  dipuRecorder.end();
  TORCH_CHECK(ret == ::diopiSuccess, __FILE__, ":", __LINE__, R"($diopi_fun_call_code)", " error, error code is ", ret, "error message is ", diopiGetLastErrorString());

  $custom_code_before_return

  synchronizeIfEnable();

  $return_code
}
"""

op_register_template_content = """
DIOPI_ATEN_FUNC("$register_name", $diopi_fun_name, $aten_fun_name);
"""

op_with_custom_fallback_register_template_content = """
DIOPI_ATEN_FUNC_CUSTOM_FALLBACK("$register_name", $diopi_fun_name, $force_fallback /*whether force fallback*/, $aten_fun_name, $fallbackFunc);
"""

custom_autograd_template_content = """
class $autograd_function_name : public torch::autograd::Function<$autograd_function_name> {
public:
  static $return_code forward(torch::autograd::AutogradContext *ctx, $param_list) {
    $forward_process_code

    $save_for_backward_code

    at::AutoDispatchBelowADInplaceOrView g;
    return $call_forward_impl_code;
  }

  static std::vector<at::Tensor> backward(torch::autograd::AutogradContext *ctx, std::vector<at::Tensor> grad_outputs) {
    $load_saved_data_code

    $cal_grad_code

    $call_backward_impl_code

    $backward_return_code
  }
};

$cppsignautre {
  auto result = $autograd_function_name::apply($arg_name_list);
  $wrappter_custom_return
}
"""


autocompare_template_content = """
//  $comment
$cppsignautre {
  std::cout << std::endl << __FUNCTION__ << std::endl;
  $transform_input_to_cpu_code

  $execute_op_on_cpu_code

  $execute_op_on_device_code

  $transform_result_to_cpu_code

  $result_compare_code
}
"""
