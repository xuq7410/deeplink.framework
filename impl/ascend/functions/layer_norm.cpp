/**
 * @file
 * @author DeepLink
 * @copyright  (c) 2023, DeepLink.
 */

#include "../common/acloprunner.hpp"

namespace impl {
namespace ascend {

diopiSize_t computeStrideFromShape(diopiSize_t &shape, std::vector<int64_t> &strideVec) {
    strideVec.resize(shape.len, 1);
    for (int64_t i = shape.len - 2; i >= 0; --i) {
        strideVec[i] = strideVec[i + 1] * shape.data[i + 1];
    }
    return vectorToDiopiSize(strideVec);
}

diopiTensorHandle_t createTensorIfNullptr(diopiContextHandle_t ctx, diopiConstTensorHandle_t in, diopiSize_t &shape, diopiSize_t &stride, diopiDtype_t dtype,
                                          bool isFillingRequired, double value) {
    diopiTensorHandle_t out;
    if (nullptr == in) {
        diopiRequireTensor(ctx, &out, &shape, &stride, dtype, diopi_device);
        if (isFillingRequired) {
            diopiScalar_t valueScalar = constructDiopiScalarT(diopi_dtype_float64, value);
            diopiFill(ctx, out, &valueScalar);
        }
    } else {
        out = const_cast<diopiTensorHandle_t>(in);
    }
    return out;
}

diopiError_t diopiLayerNorm(diopiContextHandle_t ctx, diopiTensorHandle_t out, diopiTensorHandle_t saveMean, diopiTensorHandle_t saveInvstd,
                            diopiConstTensorHandle_t input, diopiConstTensorHandle_t weight, diopiConstTensorHandle_t bias, diopiSize_t normalizedShape,
                            double eps) {
    AscendTensor inputAt(input);
    if (0 == inputAt.numel()) {
        AclOpRunner<1, 1>("Fills", ctx).addInput(out).setAttr<float>("value", 0).addOutput(out).run();
        return diopiSuccess;
    }

    std::vector<int64_t> normalizedStrideVec;
    diopiSize_t normalizedStride = computeStrideFromShape(normalizedShape, normalizedStrideVec);
    diopiTensorHandle_t weightTemp = createTensorIfNullptr(ctx, weight, normalizedShape, normalizedStride, inputAt.dtype(), true, 1);
    diopiTensorHandle_t biasTemp = createTensorIfNullptr(ctx, bias, normalizedShape, normalizedStride, inputAt.dtype(), true, 0);

    // gen begin normalized dim
    diopiSize_t inShape;
    diopiGetTensorShape(input, &inShape);
    const int axis = inShape.len - normalizedShape.len;
    int64_t beginDim = axis;

    // call acl op
    AclOpRunner<3, 3>("LayerNorm", ctx)
        .addInput(input)
        .addInput(weightTemp)
        .addInput(biasTemp)
        .addOutput(out)
        .addOutput(saveMean)
        .addOutput(saveInvstd)
        .setAttr("begin_norm_axis", beginDim)
        .setAttr("begin_params_axis", beginDim)
        .setAttr<float>("epsilon", eps)
        .run();
    return diopiSuccess;
}

diopiError_t diopiLayerNormBackward(diopiContextHandle_t ctx, diopiTensorHandle_t gradInput, diopiTensorHandle_t gradWeight, diopiTensorHandle_t gradBias,
                                    diopiConstTensorHandle_t gradOutput, diopiConstTensorHandle_t input, diopiConstTensorHandle_t weight,
                                    diopiConstTensorHandle_t bias, diopiConstTensorHandle_t mean, diopiConstTensorHandle_t rstd, diopiSize_t normalizedShape) {
    std::vector<int64_t> normalizedStrideVec;
    diopiSize_t normalizedStride = computeStrideFromShape(normalizedShape, normalizedStrideVec);
    AscendTensor inputAt(input);
    diopiTensorHandle_t weightTemp = createTensorIfNullptr(ctx, weight, normalizedShape, normalizedStride, inputAt.dtype(), true, 1);
    diopiTensorHandle_t gradWeightTemp = createTensorIfNullptr(ctx, gradWeight, normalizedShape, normalizedStride, inputAt.dtype(), false, 0);
    diopiTensorHandle_t gradBiasTemp = createTensorIfNullptr(ctx, gradBias, normalizedShape, normalizedStride, inputAt.dtype(), false, 0);

    // Align the shape of mean and rstd with input
    AscendTensor meanAt(mean), rstdAt(rstd);
    while (meanAt.dim() < inputAt.dim()) {
        meanAt.unsqueeze(meanAt.dim());
        rstdAt.unsqueeze(rstdAt.dim());
    }

    AclOpRunner<5, 3>("LayerNormGrad", ctx)
        .addInput(gradOutput)
        .addInput(input)
        .addInput(rstdAt)
        .addInput(meanAt)
        .addInput(weightTemp)
        .addOutput(gradInput)
        .addOutput(gradWeightTemp)
        .addOutput(gradBiasTemp)
        .run();
    return diopiSuccess;
}

}  // namespace ascend
}  // namespace impl