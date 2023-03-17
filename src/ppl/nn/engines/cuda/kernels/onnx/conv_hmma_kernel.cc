// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "ppl/nn/engines/cuda/kernels/onnx/conv_hmma_kernel.h"
#include "ppl/common/cuda/cuda_types.h"
#include "ppl/common/destructor.h"
#include <cuda_fp16.h>

namespace ppl { namespace nn { namespace cuda {

ConvHmmaKernel::~ConvHmmaKernel() {
    GetCudaDevice()->FreeTmpBuffer(&weight_desc_);
}

ppl::common::RetCode ConvHmmaKernel::BeforeExecute(KernelExecContext* ctx) {
    auto status = Reshape(ctx);
    if (status != ppl::common::RC_SUCCESS) {
        return status;
    }

    for (uint32_t i = 0; i < ctx->GetOutputCount(); ++i) {
        auto tensor = ctx->GetOutput<TensorImpl>(i);
        auto device = GetCudaDevice();
        tensor->SetDevice(device);
        auto concat_edge_id = param_->extra_param.fuse_info.concat_edge_id;
        if (param_->extra_param.fuse_info.channel_offset >= 0) {
            auto edge2buffer = device->GetEdge2Buffer();
            auto ptr = edge2buffer->find(concat_edge_id);
            if (ptr == edge2buffer->end()) {
                BufferDesc buffer;
                auto concat_shape = *tensor->GetShape();
                auto align_size = ppl::common::cuda::GetDataFormatChannelAlignment(concat_shape.GetDataFormat());
                auto channel_size = param_->extra_param.fuse_info.channel_size;
                auto channel_size_pad = (channel_size + align_size - 1) / align_size * align_size;
                concat_shape.SetDim(1, channel_size_pad);
                status = device->Realloc(concat_shape, &buffer);
                if (status != RC_SUCCESS) {
                    LOG(ERROR) << "alloc buffer for constant failed: " << GetRetCodeStr(status);
                    return status;
                }
                tensor->SetBuffer(buffer);
                edge2buffer->emplace(concat_edge_id, std::move(buffer));
            } else {
                tensor->SetBuffer(ptr->second);
            }
        } else {
            status = tensor->ReallocBuffer();
        }
        if (status != ppl::common::RC_SUCCESS) {
            LOG(ERROR) << "ReallocBuffer for tensor[" << tensor->GetName() << "] failed.";
            return status;
        }
    }

    return ppl::common::RC_SUCCESS;
}

ppl::common::RetCode ConvHmmaKernel::UpdateWeight(KernelExecContext* ctx, void* data, bool on_device) {
    const TensorShape& shape_in1 = *ctx->GetInput<TensorImpl>(1)->GetShape();
    TensorShape src_shape = shape_in1;
    src_shape.SetDataType(ppl::common::DATATYPE_FLOAT32);
    src_shape.SetDataFormat(ppl::common::DATAFORMAT_NDARRAY);
    src_shape.CalcPadding();
    int data_size = shape_in1.CalcBytesIncludingPadding();
    auto status = GetCudaDevice()->Realloc(data_size, &weight_desc_);
    if (status != ppl::common::RC_SUCCESS) {
        LOG(ERROR) << "alloc tmp buffer size[" << data_size << "] for kernel[" << GetName()
                   << "] failed: " << ppl::common::GetRetCodeStr(status);
        return status;
    }
    if (on_device) {
        BufferDesc src_desc(data);
        status = GetCudaDevice()->GetDataConverter()->Convert(&weight_desc_, shape_in1, src_desc, src_shape);
    } else {
        status = GetCudaDevice()->GetDataConverter()->ConvertFromHost(&weight_desc_, shape_in1, data, src_shape);
    }
    whether_update_weight_ = true;
    return status;
}

ppl::common::RetCode ConvHmmaKernel::DoExecute(KernelExecContext* ctx) {
    conv_param_t temp_conv_param;
    fuse_param_t temp_fuse_param;

    const TensorShape& shape_in0 = *ctx->GetInput<TensorImpl>(0)->GetShape();
    const TensorShape& shape_in1 = *ctx->GetInput<TensorImpl>(1)->GetShape();
    const TensorShape& shape_out = *ctx->GetOutput<TensorImpl>(0)->GetShape();

    ConvertToForwardConvParam(shape_in0, shape_in1, shape_out, *param_, temp_conv_param);
    ConvertToForwardFuseParam(ctx, GetCudaDevice(), param_->extra_param.fuse_info, temp_fuse_param);

    struct algo_param_t algo_param;
    algo_param = param_->extra_param.algo_info;

    uint64_t size = PPLCUDAConvolutionGetRuntimeBufSize(shape_in0.GetDataType(), temp_conv_param, algo_param.splitk,
                                                        algo_param.splitf, ((uint64_t)8) * 1024 * 1024 * 1024);

    BufferDesc tmp_buffer_desc;
    auto status = GetCudaDevice()->AllocTmpBuffer(size, &tmp_buffer_desc);
    if (status != ppl::common::RC_SUCCESS) {
        LOG(ERROR) << "alloc tmp buffer size[" << size << "] for kernel[" << GetName()
                   << "] failed: " << ppl::common::GetRetCodeStr(status);
        return status;
    }
    ppl::common::Destructor __tmp_buffer_guard([this, &tmp_buffer_desc]() -> void {
        GetCudaDevice()->FreeTmpBuffer(&tmp_buffer_desc);
    });
    auto tmp_buffer = tmp_buffer_desc.addr;
    auto stream = GetStream();

    // convert filter only if the filter tensor is an output of another kernel
    BufferDesc weight_buffer;
    auto newshape = shape_in1;
    if (!param_->extra_param.is_initializer_weight || whether_update_weight_) {
        auto align_size = 8;
        newshape.SetPadding1(0, (newshape.GetDim(0) + align_size - 1) / align_size * align_size - newshape.GetDim(0));

        auto status = GetCudaDevice()->Realloc(newshape, &weight_buffer);
        if (status != ppl::common::RC_SUCCESS) {
            LOG(ERROR) << "alloc buffer for constant failed: " << GetRetCodeStr(status);
            return status;
        }
        auto stream = GetStream();
        conv_param_t temp_conv_param;
        ConvertToForwardConvParam(shape_in0, shape_in1, shape_out, *param_, temp_conv_param);
        void* src_data_ptr = whether_update_weight_ ? weight_buffer.addr : ctx->GetInput<TensorImpl>(1)->GetBufferPtr();
        whether_update_weight_ = false;
        PPLCUDAConvolutionCvtFlt(stream, weight_buffer.addr, src_data_ptr, shape_in0.GetDataType(), temp_conv_param);
    }
    ppl::common::Destructor __tmp_buffer_guard__([this, &weight_buffer]() -> void {
        GetCudaDevice()->Free(&weight_buffer);
    });

#ifdef PPLNN_ENABLE_CUDA_JIT
    CUDAModule* module = static_cast<CUDAModule*>(this->GetCommonParam()->module);
    PPLCUDAConvolutionForwardJitImp(
        GetCudaDevice()->GetDeviceProp(), stream, module->GetKernelFunc(), shape_in0.GetDataType(), (int4*)ctx->GetInput<TensorImpl>(0)->GetBufferPtr(),
        param_->extra_param.is_initializer_weight ? (int4*)ctx->GetInput<TensorImpl>(1)->GetBufferPtr() : (int4*)weight_buffer.addr,
        (int4*)ctx->GetOutput<TensorImpl>(0)->GetBufferPtr(),
        param_->extra_param.bias_term ? (int4*)ctx->GetInput<TensorImpl>(2)->GetBufferPtr() : nullptr,
        (int4*)tmp_buffer, algo_param, temp_conv_param, temp_fuse_param);
#else
    PPLCUDAConvolutionForwardImp(
        GetCudaDevice()->GetDeviceProp(), stream, shape_in0.GetDataType(), (int4*)ctx->GetInput<TensorImpl>(0)->GetBufferPtr(),
        param_->extra_param.is_initializer_weight ? (int4*)ctx->GetInput<TensorImpl>(1)->GetBufferPtr() : (int4*)weight_buffer.addr,
        (int4*)ctx->GetOutput<TensorImpl>(0)->GetBufferPtr(),
        param_->extra_param.bias_term ? (int4*)ctx->GetInput<TensorImpl>(2)->GetBufferPtr() : nullptr,
        (int4*)tmp_buffer, algo_param, temp_conv_param, temp_fuse_param);
#endif
    LOG(DEBUG) << "Excute HMMA conv with kernel id:" << param_->extra_param.algo_info.kid
               << " and temp buffer size: " << size;
    return ppl::common::RC_SUCCESS;
}

}}} // namespace ppl::nn::cuda
