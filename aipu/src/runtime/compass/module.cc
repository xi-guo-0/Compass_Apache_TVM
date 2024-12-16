// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2023-2024 Arm Technology (China) Co. Ltd.
/*!
 * \file aipu/src/runtime/compass/module.cc
 */
#include <aipu/runtime/compass/basic_config.h>
#include <aipu/runtime/compass/driver.h>
#include <math.h>
#include <tvm/runtime/module.h>
#include <tvm/runtime/registry.h>

#include <fstream>
#include <map>
#include <string>

#include "../../src/runtime/file_utils.h"

namespace tvm {
namespace runtime {

class AipuCompassModuleNode : public ModuleNode {
  // Things that will interface with user directly.
 public:
  void Init();
  // Member variables need to be serialized.
  // The AIPU executable in binary format.
  std::string aipu_bin;
  // The name of original function which generate the current runtime module.
  std::string func_name;
  // The AIPU target that the AIPU executable is built to.
  std::string target;
  // The size of the Data Tightly Coupled Memory, used by AIPU simulator.
  std::string umd_dtcm_sz;

  // Internal supporting.
  // Override methods that inherited from ModuleNode.
 public:
  const char* type_key() const final;
  void SaveToBinary(dmlc::Stream* stream) final;
  int GetPropertyMask() const final;

 private:
  // TVM runtime execution mechanism relevant.
  PackedFunc GetFunction(const String& name, const ObjectPtr<Object>& sptr_to_self) final;

  // Things of current class.
  // Get outputs args from AIPU driver.
  void GetOutputs(const std::vector<DLTensor*>& out_tensors);

 private:
  // Meta data of input and output parameters, they are the quantized inputs and
  // outputs generated by AIPU Optimizer.
  std::vector<ParamInfo> in_params_;
  std::vector<ParamInfo> out_params_;
  // Member variables needn't to be serialized.
  AipuDriver aipu_driver_;
  // This function will be called to dump inputs and outputs
  // if dump in Cfg or AIPU_TVM_RUNTIME_DUMP in Env is set to True.
  void DumpTensors(const std::vector<DLTensor*>& tensors, bool is_input);
  // Packed Function implemented in the python end, will be called by DumpTensors to
  // do the concrete work.
  const PackedFunc* dump_func;
};

void AipuCompassModuleNode::Init() {
  std::string work_dir = AipuCompassBasicConfig::Global().GetRuntimeWorkDir(func_name);

  aipu_driver_.Init(aipu_bin, work_dir, target, umd_dtcm_sz, func_name);

  in_params_ = aipu_driver_.GetParamInfo(true);
  out_params_ = aipu_driver_.GetParamInfo(false);

  dump_func = Registry::Get("aipu_compass.dump_tensors");
  return;
}

const char* AipuCompassModuleNode::type_key() const { return "aipu_compass.AipuCompassModuleNode"; }

void AipuCompassModuleNode::SaveToBinary(dmlc::Stream* stream) {
  stream->Write(aipu_bin);
  stream->Write(func_name);
  stream->Write(target);
  stream->Write(umd_dtcm_sz);
}

static inline std::vector<DLTensor*> Convert2DLTensor(TVMArgs args, size_t start_idx,
                                                      size_t count) {
  std::vector<DLTensor*> ret;
  for (size_t i = start_idx; i < (start_idx + count); ++i) {
    TVMArgValue arg = args[i];
    // The argument type from "graph" executor is "kTVMDLTensorHandle", from
    // "vm" executor is "kTVMNDArrayHandle".
    ICHECK((arg.type_code() == kTVMDLTensorHandle) || (arg.type_code() == kTVMNDArrayHandle));
    auto* arg_tensor = arg.operator DLTensor*();
    // Ensure the data is a simple contiguous scalar value array.
    ICHECK(IsContiguous(*arg_tensor));
    ICHECK((arg_tensor->byte_offset == 0) && (arg_tensor->dtype.lanes == 1));
    ret.push_back(arg_tensor);
  }
  return ret;
}

void AipuCompassModuleNode::GetOutputs(const std::vector<DLTensor*>& out_tensors) {
  // Get the output data.
  aipu_driver_.GetOutputs(out_tensors);
  // Dump output tensors as a binary file.
  if (dump_func != nullptr) {
    DumpTensors(out_tensors, false);
  }
  // Dump the profile data if it exist.
  aipu_driver_.DumpProfileData();
}

void AipuCompassModuleNode::DumpTensors(const std::vector<DLTensor*>& tensors, bool is_input) {
  const size_t cnt = tensors.size() + 2;
  auto values = std::vector<TVMValue>(cnt);
  auto type_codes = std::vector<int>(cnt);
  auto arg_setter = TVMArgsSetter(values.data(), type_codes.data());
  // Function name to get the tensor storage path.
  arg_setter(0, func_name);
  // Tensor is input if True, otherwise output.
  arg_setter(1, is_input);
  // Tensors will be saved as bin file.
  for (size_t i = 2; i < cnt; ++i) {
    arg_setter(i, tensors[i - 2]);
  }
  TVMRetValue ret_value;
  dump_func->CallPacked(TVMArgs(values.data(), type_codes.data(), cnt), &ret_value);
}

PackedFunc AipuCompassModuleNode::GetFunction(const String& name,
                                              const ObjectPtr<Object>& sptr_to_self) {
  ICHECK_EQ(sptr_to_self.get(), this);

  // The "sptr_to_self" must be captured in PackedFunc, because if current instance
  // of class "AipuCompassModuleNode" is destroyed before this closure, then this
  // closure will crash when it is called.
  if (name == "compass_set_inputs") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      const size_t& in_cnt = in_params_.size();
      // Ensure the count of arguments match those of parameters.
      ICHECK_EQ(in_cnt, args.size()) << "Input arguments count mismatched.";
      std::vector<DLTensor*> in_args = Convert2DLTensor(args, 0, in_cnt);
      // Ensure the type and size of arguments match those of parameters.
      for (size_t i = 0; i < in_cnt; ++i) {
        ICHECK_EQ(DataType(in_args[i]->dtype), in_params_[i].dtype);
        ICHECK_EQ(GetDataSize(*in_args[i]), in_params_[i].size);
      }

      aipu_driver_.SetInputs(in_args);
    });
  } else if (name == "compass_set_outputs") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      const size_t& out_cnt = out_params_.size();
      // Ensure the count of arguments match those of parameters.
      ICHECK_EQ(out_cnt, args.size()) << "Output arguments count mismatched.";
      std::vector<DLTensor*> out_args = Convert2DLTensor(args, 0, out_cnt);
      // Ensure the type and size of arguments match those of parameters.
      for (size_t i = 0; i < out_cnt; ++i) {
        ICHECK_EQ(DataType(out_args[i]->dtype), out_params_[i].dtype);
        ICHECK_EQ(GetDataSize(*out_args[i]), out_params_[i].size);
      }

      aipu_driver_.SetOutputs(out_args);
    });
  } else if (name == "compass_execute") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { aipu_driver_.Run(); });
  } else if (name == "compass_get_param_info") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      int idx = args[0];
      bool is_input = args[1];
      ICHECK_GE(idx, 0) << "The index mismatched.";
      if (is_input) {
        ICHECK_LT(idx, in_params_.size()) << "The index mismatched.";
        const ParamInfo* info = &in_params_[idx];
        *rv = GetRef<ParamInfoRef>(info);
      } else {
        ICHECK_LT(idx, out_params_.size()) << "The index mismatched.";
        const ParamInfo* info = &out_params_[idx];
        *rv = GetRef<ParamInfoRef>(info);
      }
    });
  } else if (name == "compass_get_outputs") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      const size_t& out_cnt = out_params_.size();
      // Ensure the count of arguments match those of parameters.
      ICHECK_EQ(out_cnt, args.size()) << "Output arguments count mismatched.";
      std::vector<DLTensor*> out_args = Convert2DLTensor(args, 0, out_cnt);
      // Ensure the type and size of arguments match those of parameters.
      for (size_t i = 0; i < out_cnt; ++i) {
        ICHECK_EQ(DataType(out_args[i]->dtype), out_params_[i].dtype);
        ICHECK_EQ(GetDataSize(*out_args[i]), out_params_[i].size);
      }

      GetOutputs(out_args);
    });
  } else if (name == "compass_set_input_shared") {
    // Set the module input from dmabuf or physical addr.
    // so that avoid one copy.
    // If the input dltensor dtype is uint64_t, it means physical address
    // Otherwise if the input dltensor dtype is int32, it means fd;
    // The element num in dltensor should be the same with the input num of model
    // If the value in dltensor is 0 and it is pa, means the corresponding input is not shared.
    // If the value in dltensor <= 0 and it is fd, means the corresponding input is not shared.

    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      std::vector<DLTensor*> in_shared_info = Convert2DLTensor(args, 0, 1);
      bool is_fd = in_shared_info[0]->dtype.code == 0 && in_shared_info[0]->dtype.bits == 32;
      if (is_fd) {
        aipu_driver_.SetInputShared(static_cast<int*>(in_shared_info[0]->data));
      } else {
        aipu_driver_.SetInputShared(static_cast<uint64_t*>(in_shared_info[0]->data));
      }
    });
  } else if (name == "compass_mark_output_shared") {
    // Mart the module output shared for next module used in pipeline.
    // Or put the output on dmabuf so that avoid one copy to get the result.
    // If the dltensor dtype is uint64_t, it means physical address and the call
    // will filled the allocated shared buffer and put the pa on it for pipeline.
    // Otherwise if the dltensor dtype is int32, it means fd;
    // The element num in dltensor should be the same with the input num of model
    // If the value in dltensor is 0xFFFFFFFFFFFFFFFF and it is pa, means the corresponding output
    // is not shared. If the value in dltensor <= 0 and it is fd, means the corresponding input is
    // not shared.
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      std::vector<DLTensor*> out_shared_info = Convert2DLTensor(args, 0, 1);
      bool is_fd = out_shared_info[0]->dtype.code == 0 && out_shared_info[0]->dtype.bits == 32;
      if (is_fd) {
        aipu_driver_.MarkOutputShared(static_cast<int*>(out_shared_info[0]->data));
      } else {
        aipu_driver_.MarkOutputShared(static_cast<uint64_t*>(out_shared_info[0]->data));
      }
    });
  } else if (name == "compass_run" || name == func_name) {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      const size_t& in_cnt = in_params_.size();
      const size_t& out_cnt = out_params_.size();
      // Ensure the count of arguments match those of parameters.
      ICHECK_EQ((in_cnt + out_cnt), args.size()) << "Arguments count mismatched.";
      // Split the input and output arguments away.
      std::vector<DLTensor*> in_args = Convert2DLTensor(args, 0, in_cnt);
      std::vector<DLTensor*> out_args = Convert2DLTensor(args, in_cnt, out_cnt);
      // Ensure the type and size of arguments match those of parameters.
      for (size_t i = 0; i < in_cnt; ++i) {
        ICHECK_EQ(DataType(in_args[i]->dtype), in_params_[i].dtype);
        ICHECK_EQ(GetDataSize(*in_args[i]), in_params_[i].size);
      }
      for (size_t i = 0; i < out_cnt; ++i) {
        ICHECK_EQ(DataType(out_args[i]->dtype), out_params_[i].dtype);
        ICHECK_EQ(GetDataSize(*out_args[i]), out_params_[i].size);
      }
      // Dump input tensors as a binary file.
      if (dump_func != nullptr) {
        DumpTensors(in_args, true);
      }
      aipu_driver_.SetInputs(in_args);
      aipu_driver_.Run();
      GetOutputs(out_args);
    });
  } else if (name == "unrestrict_run") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // unrestrict_run do not check anything.
      // first two params is input num and output num
      int in_cnt = args[0].operator int();
      int out_cnt = args[1].operator int();
      // Ensure the count of arguments match those of parameters.
      ICHECK_EQ((in_cnt + out_cnt), args.size() - 2) << "Arguments count mismatched.";
      // Split the input and output arguments away.
      std::vector<DLTensor*> in_args = Convert2DLTensor(args, 2, in_cnt);
      std::vector<DLTensor*> out_args = Convert2DLTensor(args, 2 + in_cnt, out_cnt);

      aipu_driver_.SetInputs(in_args);
      aipu_driver_.Run();
      GetOutputs(out_args);
    });
  } else if (name == "compass_dynamic_run") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // Check information of arguments match those of parameters.
      const size_t& in_cnt = in_params_.size();
      ICHECK_EQ((in_cnt), args.size()) << "Arguments count mismatched.";
      // Split the input and output arguments away.
      std::vector<DLTensor*> in_args = Convert2DLTensor(args, 0, in_cnt);
      // std::vector<DLTensor*> out_args = Convert2DLTensor(args, in_cnt, out_cnt);
      // Only check datatype, not check size
      for (size_t i = 0; i < in_cnt; ++i) {
        ICHECK_EQ(DataType(in_args[i]->dtype), in_params_[i].dtype);
      }
      aipu_driver_.SetInputsWithDynamicShape(in_args);
      aipu_driver_.Run();
      // update params info as shape changed.
      in_params_ = aipu_driver_.GetParamInfo(true);
      out_params_ = aipu_driver_.GetParamInfo(false);
      Array<NDArray> ret;
      std::vector<DLTensor*> outs;
      Device cpu = {kDLCPU, 0};
      for (uint32_t idx = 0; idx < out_params_.size(); idx++) {
        std::vector<int64_t> shape = aipu_driver_.GetOutputShape(idx);
        NDArray out = NDArray::Empty(ShapeTuple(shape), out_params_[idx].dtype, cpu);
        ret.push_back(out);
        outs.push_back(const_cast<DLTensor*>(out.operator->()));
      }
      GetOutputs(outs);
      if (ret.size() == 1) {
        *rv = ret[0];
      } else {
        *rv = ret;
      }
    });
  }
  return nullptr;
}

int AipuCompassModuleNode::GetPropertyMask() const {
  return ModulePropertyMask::kBinarySerializable | ModulePropertyMask::kRunnable;
}

static Module LoadFromBinary(void* stream) {
  dmlc::Stream* strm = static_cast<dmlc::Stream*>(stream);

  ObjectPtr<AipuCompassModuleNode> obj = make_object<AipuCompassModuleNode>();

  if ((strm->Read(&(obj->aipu_bin)) == false) || (strm->Read(&(obj->func_name)) == false) ||
      (strm->Read(&(obj->target)) == false) || (strm->Read(&(obj->umd_dtcm_sz)) == false)) {
    LOG(FATAL) << "Load aipu_compass.AipuCompassModuleNode from binary failed!";
  }
  obj->Init();
  return Module(obj);
}

TVM_REGISTER_GLOBAL("aipu_compass.AipuCompassModuleNode")
    .set_body_typed([](NDArray aipu_bin, std::string func_name, std::string target,
                       std::string umd_dtcm_sz) {
      ObjectPtr<AipuCompassModuleNode> obj = make_object<AipuCompassModuleNode>();
      const DLTensor* dl_tensor = aipu_bin.operator->();
      obj->aipu_bin.assign(static_cast<const char*>(dl_tensor->data), GetDataSize(*dl_tensor));
      obj->func_name = func_name;
      obj->target = target;
      obj->umd_dtcm_sz = umd_dtcm_sz;
      obj->Init();
      return Module(obj);
    });

TVM_REGISTER_GLOBAL("runtime.module.loadbinary_aipu_compass.AipuCompassModuleNode")
    .set_body_typed(LoadFromBinary);

class AipuBmModuleNode : public ModuleNode {
  // Things that will interface with user directly.
 public:
  void Init();
  // The AIPU executable in binary format.
  std::string aipu_bin;
  // The name of original function which generate the current runtime module.
  std::string func_name;
  // The AIPU target that the AIPU executable is built to.
  std::string target;

  // Internal supporting.
  // Override methods that inherited from ModuleNode.
 public:
  const char* type_key() const final { return "c"; }
  void SaveToFile(const String& file_name, const String& format) final;
  String GetFormat() { return "c"; }
  int GetPropertyMask() const final { return ModulePropertyMask::kDSOExportable; }

 private:
  // TVM runtime execution mechanism relevant.
  PackedFunc GetFunction(const String& name, const ObjectPtr<Object>& sptr_to_self) final;
  void GenerateCode();

  // Things of current class.
 private:
  std::stringstream code_;
};

void AipuBmModuleNode::Init() {
  GenerateCode();
  return;
}

void AipuBmModuleNode::GenerateCode() {
  code_ << "#include \"tvm/runtime/c_runtime_api.h\"\n";
  code_ << "#include \"tvm/runtime/c_backend_api.h\"\n";
  code_ << "#include \"aipu_driver_wrapper.h\"\n";
  code_ << "#ifdef __cplusplus\n";
  code_ << "extern \"C\"\n";
  code_ << "#endif\n";
  auto target_prifx = target.substr(0, 2);

  if (target_prifx != "X2") {
    code_ << "uint8_t gbin[" << aipu_bin.size() << "] = {\n";
    code_ << std::hex << std::setfill('0');
    int cnt = 0;
    for (auto&& c = aipu_bin.begin(); c != aipu_bin.end();) {
      code_ << "0x" << std::setw(2) << (uint(*c) & 0xff);
      if (++c != aipu_bin.end()) {
        code_ << ", ";
      }
      cnt++;
      if (cnt == 16) {
        code_ << "\n";
        cnt = 0;
      }
    }
    code_ << "};\n";
  }

  if (target_prifx == "X2") {
    code_ << "extern void* gbin;\n";
  }

  code_ << "TVM_DLL int32_t " << func_name;
  code_ << "(uint8_t* input_buffer_var, uint8_t* output_buffer_var) {\n";
  code_ << "  struct graph_run_info graph_info = {0};\n";
  code_ << "  aipu_run_result_t aipu_result = AIPU_RUN_ERROR;\n\n";
  code_ << "  graph_info.graph_addr = gbin;\n";
  code_ << "  graph_info.input0_addr = input_buffer_var;\n";
  code_ << "  graph_info.output_addr = output_buffer_var;\n";
  code_ << "  graph_info.run_times = 1;\n";
  code_ << "  graph_info.output_type = NOT_BATCH_OUTPUT;\n\n";
  code_ << "  aipu_result = aipu_start_single_graph(&graph_info);\n\n";
  code_ << "  return aipu_result != AIPU_RUN_RESULT_PASS;\n";
  code_ << "}\n";
}

void AipuBmModuleNode::SaveToFile(const String& file_name, const String& format) {
  std::string fmt = GetFileFormat(file_name, format);
  ICHECK_EQ(fmt, "c") << "Can only save to format=c";

  auto code_str = code_.str();
  ICHECK_NE(code_str.length(), 0);
  SaveBinaryToFile(file_name, code_str);
  auto target_prifx = target.substr(0, 2);
  if (target_prifx == "X2") {
    std::string file_path = file_name;
    int dir_last_str_index = file_path.rfind("/");
    auto dir_path = file_path.substr(0, dir_last_str_index);
    auto aipu_bin_path = dir_path + "/aipu.bin";
    SaveBinaryToFile(aipu_bin_path, aipu_bin);
  }
}

PackedFunc AipuBmModuleNode::GetFunction(const String& name,
                                         const ObjectPtr<Object>& sptr_to_self) {
  ICHECK_EQ(sptr_to_self.get(), this);

  if (name == "get_func_names") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      Array<String> func_names{this->func_name};
      *rv = func_names;
    });
  }

  return nullptr;
}

TVM_REGISTER_GLOBAL("aipu_compass.AipuBmModuleNode")
    .set_body_typed([](NDArray aipu_bin, std::string func_name, std::string target) {
      ObjectPtr<AipuBmModuleNode> obj = make_object<AipuBmModuleNode>();
      const DLTensor* dl_tensor = aipu_bin.operator->();
      obj->aipu_bin.assign(static_cast<const char*>(dl_tensor->data), GetDataSize(*dl_tensor));
      obj->func_name = func_name;
      obj->target = target;
      obj->Init();
      return Module(obj);
    });

class AipuCompassBinaryNode : public ModuleNode {
  // This object would just warp aipu.bin but not initlize aipu driver.
 public:
  // Member are same as AipuCompassModuleNode
  NDArray aipu_bin;
  std::string func_name;
  std::string target;
  std::string umd_dtcm_sz;

 public:
  const char* type_key() const final;
  void SaveToBinary(dmlc::Stream* stream) final;
  int GetPropertyMask() const final;

 private:
  // TVM runtime execution mechanism relevant.
  PackedFunc GetFunction(const String& name, const ObjectPtr<Object>& sptr_to_self) final;
};

const char* AipuCompassBinaryNode::type_key() const { return "aipu_compass.AipuCompassBinaryNode"; }

int AipuCompassBinaryNode::GetPropertyMask() const {
  return ModulePropertyMask::kBinarySerializable | ModulePropertyMask::kRunnable;
}

void AipuCompassBinaryNode::SaveToBinary(dmlc::Stream* stream) {
  aipu_bin.Save(stream);
  stream->Write(func_name);
  stream->Write(target);
  stream->Write(umd_dtcm_sz);
}

PackedFunc AipuCompassBinaryNode::GetFunction(const String& name,
                                              const ObjectPtr<Object>& sptr_to_self) {
  ICHECK_EQ(sptr_to_self.get(), this);

  if (name == "get_compass_module") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      // create compass module and initlize
      static auto compass_fn = Registry::Get("aipu_compass.AipuCompassModuleNode");
      *rv = std::move((*compass_fn)(aipu_bin, func_name, target, umd_dtcm_sz));
    });
  }
  return nullptr;
}

static Module CreateAIPUCompassBinary(NDArray aipu_bin, std::string func_name, std::string target,
                                      std::string umd_dtcm_sz) {
  ObjectPtr<AipuCompassBinaryNode> obj = make_object<AipuCompassBinaryNode>();
  obj->aipu_bin = aipu_bin;
  obj->func_name = func_name;
  obj->target = target;
  obj->umd_dtcm_sz = umd_dtcm_sz;
  return Module(obj);
}

TVM_REGISTER_GLOBAL("aipu_compass.AipuCompassBinaryNode").set_body_typed(CreateAIPUCompassBinary);

static Module BinaryModuleLoadFromBinary(void* stream) {
  dmlc::Stream* strm = static_cast<dmlc::Stream*>(stream);

  ObjectPtr<AipuCompassBinaryNode> obj = make_object<AipuCompassBinaryNode>();

  if ((obj->aipu_bin.Load(strm) == false) || (strm->Read(&(obj->func_name)) == false) ||
      (strm->Read(&(obj->target)) == false) || (strm->Read(&(obj->umd_dtcm_sz)) == false)) {
    LOG(FATAL) << "Load aipu_compass.AipuCompassBinaryNode from binary failed!";
  }
  return Module(obj);
}

TVM_REGISTER_GLOBAL("runtime.module.loadbinary_aipu_compass.AipuCompassBinaryNode")
    .set_body_typed(BinaryModuleLoadFromBinary);

}  // namespace runtime
}  // namespace tvm
