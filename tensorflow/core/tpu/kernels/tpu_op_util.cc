/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/tpu/kernels/tpu_op_util.h"

#include <string>

#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/tpu/kernels/tpu_compile_c_api.h"

namespace tensorflow {
namespace tpu {
namespace {
// Return fingerprint_in_metadata if it's not empty; otherwise read input tensor
// data to compute the fingerprint.
std::string GuaranteedConstFingerprint(const string& fingerprint_in_metadata,
                                       const Tensor* guaranteed_constants,
                                       size_t guaranteed_constants_size) {
  if (fingerprint_in_metadata.empty()) {
    uint64_t fingerprint = 0;
    for (size_t i = 0; i < guaranteed_constants_size; ++i) {
      const Tensor& constant = guaranteed_constants[i];
      fingerprint = TpuCompile_CreateGuaranteedConstFingerprint(
          fingerprint, constant.tensor_data().data(),
          constant.tensor_data().size());
    }
    return std::to_string(fingerprint);
  } else {
    return fingerprint_in_metadata;
  }
}

std::string CreateShapePrefix(
    const std::vector<tensorflow::TensorShape>& dynamic_shapes) {
  std::string shapes_prefix;
  for (const TensorShape& shape : dynamic_shapes) {
    for (int64 size : shape.dim_sizes()) {
      absl::StrAppend(&shapes_prefix, size, ",");
    }
    absl::StrAppend(&shapes_prefix, ";");
  }
  return shapes_prefix;
}

// Include compilation configurations of the arguments that are not captured
// by the called graph.
std::string CreateConfigPrefix(const TPUCompileMetadataProto& metadata) {
  std::string config_prefix;
  for (const auto& arg : metadata.args()) {
    if (arg.is_same_data_across_replicas()) {
      absl::StrAppend(&config_prefix, ":s");
      // Same.
    } else {
      // Different.
      absl::StrAppend(&config_prefix, ":");
    }
    if (arg.enable_xla_sharding() ==
        tpu::TPUCompileMetadataProto::Arg::ALLOWED) {
      // Enabled.
      absl::StrAppend(&config_prefix, "e");
    }
    if (arg.unrestricted_layout()) {
      // Unrestricted.
      absl::StrAppend(&config_prefix, ":u");
    }
    absl::StrAppend(&config_prefix, ",type(", arg.dtype(), ")");
    if (arg.has_shape()) {
      absl::StrAppend(&config_prefix, ",shape(");
      for (const auto& dim : arg.shape().dim()) {
        absl::StrAppend(&config_prefix, dim.size(), ",");
      }
      absl::StrAppend(&config_prefix, ")");
    }
  }
  return config_prefix;
}
}  // namespace

// The `guaranteed_constants` must be passed as reference due to the lazy
// evaluation of `guaranteed_const_fingerprint()` callback.
TpuCompilationCacheKey CreateCompilationCacheKey(
    absl::string_view function_name, uint64 function_library_fingerprint,
    absl::string_view mlir_module, const Tensor* guaranteed_constants,
    size_t guaranteed_constants_size,
    const std::vector<TensorShape>& dynamic_shapes,
    const TPUCompileMetadataProto& metadata,
    const TpuMeshStateInterface& mesh_state) {
  VLOG(1) << "FunctionLibraryFingerprint:" << function_library_fingerprint;
  std::string shapes_prefix = CreateShapePrefix(dynamic_shapes);
  VLOG(1) << "shapes_prefix = " << shapes_prefix;
  std::string config_prefix = CreateConfigPrefix(metadata);
  VLOG(1) << "config_prefix = " << config_prefix;
  std::vector<int32_t> flattened_device_ids;
  if (metadata.has_device_assignment()) {
    for (const auto& device :
         metadata.device_assignment().computation_devices()) {
      flattened_device_ids.insert(flattened_device_ids.end(),
                                  device.replica_device_ids().begin(),
                                  device.replica_device_ids().end());
    }
  }
  CompilationCacheKeyResult result =
      TpuCompile_CreateCompilationCacheKey(CompilationCacheKeyProperty{
          config_prefix.data(),
          shapes_prefix.data(),
          function_name.data(),
          mlir_module.data(),
          flattened_device_ids.data(),
          flattened_device_ids.size(),
          guaranteed_constants_size,
          function_library_fingerprint,
          metadata.num_cores_per_replica(),
          metadata.num_replicas(),
          mesh_state.data(),
      });
  auto buffer_cleanup = gtl::MakeCleanup(
      [result]() { TpuCompile_DestroyCompilationCacheKey(result); });
  TpuCompilationCacheKey key;
  key.prefix = result.key;
  key.debug_string = result.debug_string;

  // Guaranteed constants can be different across sessions. Use session_handle
  // and guaranteed_const fingerprint to guarantee no collision.
  if (guaranteed_constants != nullptr && guaranteed_constants_size > 0) {
    key.has_guaranteed_const = true;
    key.session_handle = metadata.session_handle();
    // Both `metadata` and `guaranteed_constants` lifetime are captured by
    // reference based on the assumption that these variables lifetime is
    // managed through the `TPUCompileOpKernelImpl` that outlives the
    // lifetime of the compilation cache lookups.
    string fingerprint;
    key.guaranteed_const_fingerprint = [&metadata, guaranteed_constants,
                                        guaranteed_constants_size,
                                        fingerprint]() mutable {
      if (fingerprint.empty()) {
        fingerprint = GuaranteedConstFingerprint(
            metadata.guaranteed_const_fingerprint(), guaranteed_constants,
            guaranteed_constants_size);
      }
      return fingerprint;
    };
  }
  return key;
}

TpuCompilationCacheKey CreateCompilationCacheKey(
    absl::string_view function_name, uint64 function_library_fingerprint,
    absl::string_view mlir_module, const OpInputList& guaranteed_constants,
    const std::vector<TensorShape>& dynamic_shapes,
    const TPUCompileMetadataProto& metadata,
    const TpuMeshStateInterface& mesh_state) {
  return CreateCompilationCacheKey(
      function_name, function_library_fingerprint, mlir_module,
      (guaranteed_constants.size() > 0 ? &guaranteed_constants[0] : nullptr),
      guaranteed_constants.size(), dynamic_shapes, metadata, mesh_state);
}
}  // namespace tpu
}  // namespace tensorflow
