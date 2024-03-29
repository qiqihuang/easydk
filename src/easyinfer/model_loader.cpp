/*************************************************************************
 * Copyright (C) [2019] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/

#include "easyinfer/model_loader.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "cxxutil/log.h"
#include "model_loader_internal.h"

#define ONLY_SUPPORT_FLOAT32(layout)                                                  \
  do {                                                                                \
    if (layout.dtype != DataType::FLOAT32) {                                          \
      THROW_EXCEPTION(Exception::INVALID_ARG, "Only support float32 for cpu layout"); \
    }                                                                                 \
  } while (0)

#define CHECK_CONDITION(cond, msg)               \
  do {                                           \
    if (!(cond)) {                               \
      THROW_EXCEPTION(Exception::INTERNAL, msg); \
    }                                            \
  } while (0)

#define CHECK_CNRT_RET(cnrt_ret, msg) CHECK_CONDITION((cnrt_ret == CNRT_RET_SUCCESS), msg)

namespace edk {

cnrtDataType CastDataType(const DataType& type) {
  switch (type) {
    case DataType::UINT8:
      return CNRT_UINT8;
    case DataType::FLOAT32:
      return CNRT_FLOAT32;
    case DataType::FLOAT16:
      return CNRT_FLOAT16;
    case DataType::INT16:
      return CNRT_INT16;
    case DataType::INT32:
      return CNRT_INT32;
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported data type");
  }
}

DataType CastDataType(const cnrtDataType& type) {
  switch (type) {
    case CNRT_UINT8:
      return DataType::UINT8;
    case CNRT_FLOAT32:
      return DataType::FLOAT32;
    case CNRT_FLOAT16:
      return DataType::FLOAT16;
    case CNRT_INT16:
      return DataType::INT16;
    case CNRT_INT32:
      return DataType::INT32;
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported data type");
  }
}

// not used yet
#if 0
cnrtDimOrder CastDimOrder(const DimOrder& order) {
  switch (order) {
    case DimOrder::NCHW:
      return CNRT_NCHW;
    case DimOrder::NHWC:
      return CNRT_NHWC;
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported dimension order");
  }
}

DimOrder CastDimOrder(const cnrtDimOrder& order) {
  switch (order) {
    case CNRT_NCHW:
      return DimOrder::NCHW;
    case CNRT_NHWC:
      return DimOrder::NHWC;
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported dimension order");
  }
}
#endif

static const char* DataTypeStr(DataType type) {
  switch (type) {
    case DataType::UINT8:
      return "DataType UINT8";
    case DataType::FLOAT32:
      return "DataType FLOAT32";
    case DataType::FLOAT16:
      return "DataType FLOAT16";
    case DataType::INT16:
      return "DataType INT16";
    case DataType::INT32:
      return "DataType INT32";
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported data type");
  }
}

static const char* DimOrderStr(DimOrder order) {
  switch (order) {
    case DimOrder::NCHW:
      return "DimOrder NCHW";
    case DimOrder::NHWC:
      return "DimOrder NHWC";
    default:
      THROW_EXCEPTION(Exception::UNSUPPORTED, "Unsupported dimension order");
  }
}

class ModelLoaderPrivate {
 public:
  explicit ModelLoaderPrivate(ModelLoader* q) : q_ptr_(q) {}
  void LoadFunction(const char* function_name);

  std::vector<int64_t> i_data_sizes_, o_data_sizes_;
  std::vector<DataLayout> i_mlu_layouts_, o_mlu_layouts_;
  uint32_t o_num_;
  uint32_t i_num_;
  int model_parallelism_;
  std::vector<DataLayout> i_cpu_layouts_, o_cpu_layouts_;
  std::vector<Shape> input_shapes_ = {}, output_shapes_ = {};
  std::vector<ShapeEx> input_shapexs_ = {}, output_shapexs_ = {};
  cnrtModel_t model_;
  cnrtFunction_t function_;
  ModelLoader* q_ptr_ = nullptr;
};  // class ModelLoaderPrivate

ModelLoader::ModelLoader(const std::string& model_path, const std::string& function_name)
    : ModelLoader(model_path.c_str(), function_name.c_str()) {}

ModelLoader::ModelLoader(const char* model_path, const char* function_name) : d_ptr_(new ModelLoaderPrivate(this)) {
  if (FILE* file = fopen(model_path, "r")) {
    fclose(file);
  } else {
    THROW_EXCEPTION(Exception::UNAVAILABLE, "Model file not exist. Please check model path");
  }

  LOGD(INFER) << "Load model from file: " << model_path;
  // 1. get cnrtModel and cnrtFunction
  cnrtRet_t error_code = cnrtLoadModel(&d_ptr_->model_, model_path);
  CHECK_CNRT_RET(error_code, "Load model failed, cnrt error code : " + std::to_string(error_code));

  d_ptr_->LoadFunction(function_name);
}

ModelLoader::ModelLoader(void* mem_ptr, const char* function_name) : d_ptr_(new ModelLoaderPrivate(this)) {
  // 1. get cnrtModel and cnrtFunction
  LOGI(INFER) << "Load model from memory, " << mem_ptr;
  cnrtRet_t error_code = cnrtLoadModelFromMem(&d_ptr_->model_, reinterpret_cast<char*>(mem_ptr));
  CHECK_CNRT_RET(error_code, "Load model from memory failed, cnrt error code : " + std::to_string(error_code));

  d_ptr_->LoadFunction(function_name);
}

void ModelLoaderPrivate::LoadFunction(const char* function_name) {
  cnrtRet_t error_code;

  error_code = cnrtCreateFunction(&function_);
  CHECK_CNRT_RET(error_code, "Create function failed, cnrt error code : " + std::to_string(error_code));
  error_code = cnrtExtractFunction(&function_, model_, function_name);
  CHECK_CNRT_RET(error_code, "Extract function failed, cnrt error code : " + std::to_string(error_code));
  error_code = cnrtQueryModelParallelism(model_, &model_parallelism_);
  CHECK_CNRT_RET(error_code, "Query Model Parallelism failed, cnrt error code : " + std::to_string(error_code));

  LOGI(INFER) << "Load function from offline model succeeded";

  // 2. get IO messages
  // 2.1 get io number and data size
  int64_t* input_sizes = nullptr;
  int input_num = 0;
  error_code = cnrtGetInputDataSize(&input_sizes, &input_num, function_);
  CHECK_CNRT_RET(error_code, "Get input data size failed, cnrt error code : " + std::to_string(error_code));
  i_num_ = input_num;
  i_data_sizes_ = std::vector<int64_t>(input_sizes, input_sizes + input_num);

  int64_t* output_sizes = nullptr;
  int output_num = 0;
  error_code = cnrtGetOutputDataSize(&output_sizes, &output_num, function_);
  CHECK_CNRT_RET(error_code, "Get output data size failed, cnrt error code : " + std::to_string(error_code));
  o_num_ = output_num;
  o_data_sizes_ = std::vector<int64_t>(output_sizes, output_sizes + output_num);

  // 2.2 get io shapes
  int* input_dim_values = nullptr;
  int dim_num = 0;
  input_shapes_.clear();
  input_shapexs_.clear();
  input_shapes_.reserve(input_num);
  input_shapexs_.reserve(input_num);
  for (int i = 0; i < input_num; ++i) {
    error_code = cnrtGetInputDataShape(&input_dim_values, &dim_num, i, function_);
    CHECK_CNRT_RET(error_code, "Get input data size failed, cnrt error code : " + std::to_string(error_code));
    // nhwc shape
    input_shapexs_.emplace_back(std::vector<ShapeEx::value_type>(input_dim_values, input_dim_values + dim_num));

    if (dim_num != 4) LOGI(INFER) << "input dimension is not 4, dims in `Shape` is incorrect, use ShapeEx instead";
    std::vector<uint32_t> dim_value(4, 1);
    for (int i = 0; i < dim_num; ++i) {
      dim_value[i] = input_dim_values[i];
    }
    free(input_dim_values);
    input_shapes_.push_back(Shape(dim_value[0], dim_value[1], dim_value[2], dim_value[3]));
  }

  int* output_dim_values = nullptr;
  output_shapes_.clear();
  output_shapexs_.clear();
  output_shapes_.reserve(output_num);
  output_shapexs_.reserve(output_num);
  for (int i = 0; i < output_num; ++i) {
    error_code = cnrtGetOutputDataShape(&output_dim_values, &dim_num, i, function_);
    CHECK_CNRT_RET(error_code, "Get output data shape failed, cnrt error code : " + std::to_string(error_code));
    // nhwc shape
    output_shapexs_.emplace_back(std::vector<ShapeEx::value_type>(output_dim_values, output_dim_values + dim_num));

    if (dim_num != 4) LOGI(INFER) << "output dimension is not 4, dims in `Shape` is incorrect, use ShapeEx instead";
    std::vector<uint32_t> dim_value(4, 1);
    for (int i = 0; i < dim_num; ++i) {
      dim_value[i] = output_dim_values[i];
    }
    free(output_dim_values);
    output_shapes_.push_back(Shape(dim_value[0], dim_value[1], dim_value[2], dim_value[3]));
  }

  // 2.3 get mlu io data type
  cnrtDataType_t* input_dtypes = nullptr;
  error_code = cnrtGetInputDataType(&input_dtypes, &input_num, function_);
  CHECK_CNRT_RET(error_code, "Get input data type failed, cnrt error code : " + std::to_string(error_code));
  CHECK_CONDITION(static_cast<size_t>(input_num) == i_data_sizes_.size(),
                  "Internel error, maybe input number from cnrtGetInputDataType is wrong.");
  i_mlu_layouts_.resize(i_num_);
  for (uint32_t i = 0; i < i_num_; ++i) {
    i_mlu_layouts_[i].dtype = CastDataType(input_dtypes[i]);
    i_mlu_layouts_[i].order = DimOrder::NHWC;  // mlu data order is always NHWC
  }

  cnrtDataType_t* output_dtypes = nullptr;
  error_code = cnrtGetOutputDataType(&output_dtypes, &output_num, function_);
  CHECK_CNRT_RET(error_code, "Get output data type failed, cnrt error code : " + std::to_string(error_code));
  CHECK_CONDITION(static_cast<size_t>(output_num) == o_data_sizes_.size(),
                  "Internel error, maybe output number from cnrtGetOutputDataType is wrong.");
  o_mlu_layouts_.resize(o_num_);
  for (uint32_t i = 0; i < o_num_; ++i) {
    o_mlu_layouts_[i].dtype = CastDataType(output_dtypes[i]);
    o_mlu_layouts_[i].order = DimOrder::NHWC;  // mlu data order is always NHWC
  }

  // set default cpu layouts
  // this decided by network framework(eg. cambricon caffe)
  i_cpu_layouts_.resize(i_num_);
  for (DataLayout& layout : i_cpu_layouts_) {
    layout.dtype = DataType::FLOAT32;
    layout.order = DimOrder::NHWC;
  }
  o_cpu_layouts_.resize(o_num_);
  for (DataLayout& layout : o_cpu_layouts_) {
    layout.dtype = DataType::FLOAT32;
    layout.order = DimOrder::NHWC;
  }
}

int64_t ModelLoaderInternalInterface::InputDataSize(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(model_->InputNum())) return 0;
  return model_->d_ptr_->i_data_sizes_[data_index];
}

int64_t ModelLoaderInternalInterface::OutputDataSize(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(model_->OutputNum())) return 0;
  return model_->d_ptr_->o_data_sizes_[data_index];
}

DataLayout ModelLoaderInternalInterface::GetMluInputLayout(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(model_->InputNum())) return {};
  return model_->d_ptr_->i_mlu_layouts_[data_index];
}

DataLayout ModelLoaderInternalInterface::GetMluOutputLayout(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(model_->OutputNum())) return {};
  return model_->d_ptr_->o_mlu_layouts_[data_index];
}

cnrtFunction_t ModelLoaderInternalInterface::Function() const { return model_->d_ptr_->function_; }

void ModelLoader::SetCpuInputLayout(DataLayout layout, int data_index) {
  if (data_index < 0 || static_cast<uint32_t>(data_index) >= d_ptr_->i_num_) {
    THROW_EXCEPTION(Exception::INVALID_ARG, "SetCpuInputLayout: Data index out of range");
  }
  ONLY_SUPPORT_FLOAT32(layout);

  d_ptr_->i_cpu_layouts_[data_index] = layout;

  LOGD(INFER) << "Set CPU input data layout";
  LOGD(INFER) << DataTypeStr(layout.dtype) << "\t" << DimOrderStr(layout.order);
}

void ModelLoader::SetCpuOutputLayout(DataLayout layout, int data_index) {
  if (data_index < 0 || static_cast<uint32_t>(data_index) >= d_ptr_->o_num_) {
    THROW_EXCEPTION(Exception::INVALID_ARG, "SetCpuOutputLayout: Data index out of range");
  }
  ONLY_SUPPORT_FLOAT32(layout);

  d_ptr_->o_cpu_layouts_[data_index] = layout;

  LOGD(INFER) << "Set CPU output data layout";
  LOGD(INFER) << DataTypeStr(layout.dtype) << "\t" << DimOrderStr(layout.order);
}

DataLayout ModelLoader::GetCpuInputLayout(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(InputNum())) return {};
  return d_ptr_->i_cpu_layouts_[data_index];
}

DataLayout ModelLoader::GetCpuOutputLayout(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(OutputNum())) return {};
  return d_ptr_->o_cpu_layouts_[data_index];
}

bool ModelLoader::AdjustStackMemory() {
  uint64_t stack_size;
  uint32_t current_device_size;

  cnrtRet_t error_code = cnrtQueryModelStackSize(d_ptr_->model_, &stack_size);
  CHECK_CNRT_RET(error_code, "Query model stack size failed. cnrt error_code : " + std::to_string(error_code));
  LOGD(INFER) << "Model stack size is " << stack_size << " MB";

  error_code = cnrtGetStackMem(&current_device_size);
  CHECK_CNRT_RET(error_code, "Get current device stack size failed. cnrt error_code : " + std::to_string(error_code));
  LOGD(INFER) << "Current MLU stack size is " << current_device_size << " MB";

  if (stack_size > current_device_size) {
    error_code = cnrtSetStackMem(stack_size + 50);
    CHECK_CNRT_RET(error_code, "set stack size failed. cnrt error_code : " + std::to_string(error_code));
    LOGI(INFER) << "Adjust stack memory to " << stack_size + 50 << " MB";
    return true;
  }
  return false;
}

uint32_t ModelLoader::OutputNum() const { return d_ptr_->o_num_; }

uint32_t ModelLoader::InputNum() const { return d_ptr_->i_num_; }

const std::vector<Shape>& ModelLoader::InputShapes() const { return d_ptr_->input_shapes_; }

const std::vector<Shape>& ModelLoader::OutputShapes() const { return d_ptr_->output_shapes_; }

const ShapeEx& ModelLoader::InputShape(uint32_t index) const {
  if (index > d_ptr_->i_num_) THROW_EXCEPTION(Exception::INVALID_ARG, "input shape index overflow");
  return d_ptr_->input_shapexs_[index];
}

const ShapeEx& ModelLoader::OutputShape(uint32_t index) const {
  if (index > d_ptr_->o_num_) THROW_EXCEPTION(Exception::INVALID_ARG, "output shape index overflow");
  return d_ptr_->output_shapexs_[index];
}

int ModelLoader::ModelParallelism() const { return d_ptr_->model_parallelism_; }

int64_t ModelLoader::GetInputDataBatchAlignSize(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(InputNum())) return 0;
  int64_t size = 0;
  ModelLoaderInternalInterface model_loader_internal(const_cast<ModelLoader*>(this));
  size = model_loader_internal.InputDataSize(data_index) / d_ptr_->input_shapexs_[data_index][0];
  return size;
}

int64_t ModelLoader::GetOutputDataBatchAlignSize(int data_index) const {
  if (data_index < 0 || data_index >= static_cast<int>(OutputNum())) return 0;
  int64_t size = 0;
  ModelLoaderInternalInterface model_loader_internal(const_cast<ModelLoader*>(this));
  size = model_loader_internal.OutputDataSize(data_index) / d_ptr_->output_shapexs_[data_index][0];
  return size;
}

ModelLoader::~ModelLoader() {
  LOGI(INFER) << "Destroy neural network function";
  cnrtRet_t error_code = cnrtDestroyFunction(d_ptr_->function_);
  if (CNRT_RET_SUCCESS != error_code) {
    LOGW(INFER) << "Destroy function failed. error_code : " << std::to_string(error_code).c_str();
  }
  LOGI(INFER) << "Unload offline model";
  error_code = cnrtUnloadModel(d_ptr_->model_);
  if (CNRT_RET_SUCCESS != error_code) {
    LOGE(INFER) << "Unload model failed. error_code : " << std::to_string(error_code).c_str();
  }
}

}  // namespace edk
