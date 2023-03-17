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

#ifndef _ST_HPC_PPL_NN_ENGINES_CUDA_KERNELS_ONNX_CONV_HMMA_KERNEL_H_
#define _ST_HPC_PPL_NN_ENGINES_CUDA_KERNELS_ONNX_CONV_HMMA_KERNEL_H_

#include "ppl/nn/engines/cuda/kernel.h"

#include "ppl/nn/engines/cuda/optimizer/opt_kernel.h"
#include "ppl/nn/engines/cuda/params/conv_extra_param.h"

namespace ppl { namespace nn { namespace cuda {

class ConvHmmaKernel : public CudaKernel {
public:
    ConvHmmaKernel(const ir::Node* node) : CudaKernel(node) {}
    ~ConvHmmaKernel();

    void SetParam(const CudaConvParam* p) {
        param_ = p;
    }

    ppl::common::RetCode UpdateWeight(KernelExecContext* ctx, void* data, bool on_device) override;

private:
    ppl::common::RetCode BeforeExecute(KernelExecContext*) override;
    ppl::common::RetCode DoExecute(KernelExecContext*) override;

private:
    const CudaConvParam* param_ = nullptr;
    BufferDesc weight_desc_;
    bool whether_update_weight_ = false;
    // ConvFuse fuse_params_;
    // BufferDesc cvt_filter_;
    // BufferDesc bias_;
};

}}} // namespace ppl::nn::cuda

#endif
