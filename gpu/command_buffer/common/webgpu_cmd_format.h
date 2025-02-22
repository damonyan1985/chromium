// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_COMMON_WEBGPU_CMD_FORMAT_H_
#define GPU_COMMAND_BUFFER_COMMON_WEBGPU_CMD_FORMAT_H_

#include "gpu/command_buffer/common/gl2_types.h"
#include "gpu/command_buffer/common/webgpu_cmd_enums.h"
#include "gpu/command_buffer/common/webgpu_cmd_ids.h"

namespace gpu {
namespace webgpu {
namespace cmds {

#define GPU_DAWN_RETURN_DATA_ALIGNMENT (8)
struct alignas(GPU_DAWN_RETURN_DATA_ALIGNMENT) DawnReturnDataHeader {
  DawnReturnDataType return_data_type;
};

static_assert(
    sizeof(DawnReturnDataHeader) % GPU_DAWN_RETURN_DATA_ALIGNMENT == 0,
    "DawnReturnDataHeader must align to GPU_DAWN_RETURN_DATA_ALIGNMENT");

struct DawnReturnCommandsInfo {
  DawnReturnDataHeader return_data_header = {DawnReturnDataType::kDawnCommands};
  alignas(GPU_DAWN_RETURN_DATA_ALIGNMENT) char deserialized_buffer[];
};

static_assert(offsetof(DawnReturnCommandsInfo, return_data_header) == 0,
              "The offset of return_data_header must be 0");

struct DawnReturnAdapterInfoHeader {
  DawnReturnDataHeader return_data_header = {
      DawnReturnDataType::kRequestedDawnAdapterProperties};
  uint32_t request_adapter_serial;
  uint32_t adapter_service_id;
};

static_assert(offsetof(DawnReturnAdapterInfoHeader, return_data_header) == 0,
              "The offset of return_data_header must be 0");

struct DawnReturnAdapterInfo {
  DawnReturnAdapterInfoHeader header;
  alignas(GPU_DAWN_RETURN_DATA_ALIGNMENT) char deserialized_buffer[];
};

static_assert(offsetof(DawnReturnAdapterInfo, header) == 0,
              "The offset of header must be 0");

struct DawnReturnRequestDeviceInfo {
  DawnReturnDataHeader return_data_header = {
      DawnReturnDataType::kRequestedDeviceReturnInfo};
  uint32_t request_device_serial;
  bool is_request_device_success;
};

static_assert(offsetof(DawnReturnRequestDeviceInfo, return_data_header) == 0,
              "The offset of return_data_header must be 0");

// Command buffer is GPU_COMMAND_BUFFER_ENTRY_ALIGNMENT byte aligned.
#pragma pack(push, 4)
static_assert(GPU_COMMAND_BUFFER_ENTRY_ALIGNMENT == 4,
              "pragma pack alignment must be equal to "
              "GPU_COMMAND_BUFFER_ENTRY_ALIGNMENT");

#include "gpu/command_buffer/common/webgpu_cmd_format_autogen.h"

#pragma pack(pop)

}  // namespace cmds
}  // namespace webgpu
}  // namespace gpu
#endif  // GPU_COMMAND_BUFFER_COMMON_WEBGPU_CMD_FORMAT_H_
