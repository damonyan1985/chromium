// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/service_manager/sandbox/sandbox_type.h"

#include <string>

#include "base/feature_list.h"
#include "services/service_manager/sandbox/features.h"
#include "services/service_manager/sandbox/switches.h"

namespace service_manager {

bool IsUnsandboxedSandboxType(SandboxType sandbox_type) {
  switch (sandbox_type) {
    case SandboxType::kNoSandbox:
      return true;
#if defined(OS_WIN)
    case SandboxType::kNoSandboxAndElevatedPrivileges:
      return true;

    case SandboxType::kXrCompositing:
      return !base::FeatureList::IsEnabled(
          service_manager::features::kXRSandbox);
#endif
    case SandboxType::kAudio:
      return !IsAudioSandboxEnabled();
    case SandboxType::kNetwork:
      return !base::FeatureList::IsEnabled(
          service_manager::features::kNetworkServiceSandbox);
    case SandboxType::kInvalid:
    case SandboxType::kRenderer:
    case SandboxType::kUtility:
    case SandboxType::kGpu:
    case SandboxType::kPpapi:
    case SandboxType::kCdm:
    case SandboxType::kPdfCompositor:
    case SandboxType::kProfiling:
#if defined(OS_FUCHSIA)
    case SandboxType::kWebContext:
#endif
#if defined(OS_MACOSX)
    case SandboxType::kNaClLoader:
#endif
#if defined(OS_CHROMEOS)
    case SandboxType::kIme:
#endif
      return false;
  }
}

void SetCommandLineFlagsForSandboxType(base::CommandLine* command_line,
                                       SandboxType sandbox_type) {
  switch (sandbox_type) {
    case SandboxType::kNoSandbox:
      command_line->AppendSwitch(switches::kNoSandbox);
      break;
#if defined(OS_WIN)
    case SandboxType::kNoSandboxAndElevatedPrivileges:
      command_line->AppendSwitch(switches::kNoSandboxAndElevatedPrivileges);
      break;
#endif
    case SandboxType::kRenderer:
      DCHECK(command_line->GetSwitchValueASCII(switches::kProcessType) ==
             switches::kRendererProcess);
      break;
    case SandboxType::kGpu:
      DCHECK(command_line->GetSwitchValueASCII(switches::kProcessType) ==
             switches::kGpuProcess);
      break;
    case SandboxType::kPpapi:
      if (command_line->GetSwitchValueASCII(switches::kProcessType) ==
          switches::kUtilityProcess) {
        command_line->AppendSwitchASCII(switches::kServiceSandboxType,
                                        switches::kPpapiSandbox);
      } else {
        DCHECK(command_line->GetSwitchValueASCII(switches::kProcessType) ==
               switches::kPpapiPluginProcess);
      }
      break;
    case SandboxType::kUtility:
    case SandboxType::kNetwork:
    case SandboxType::kCdm:
    case SandboxType::kPdfCompositor:
    case SandboxType::kProfiling:
    case SandboxType::kAudio:
#if defined(OS_WIN)
    case SandboxType::kXrCompositing:
#endif  // defined(OS_WIN)
#if defined(OS_CHROMEOS)
    case SandboxType::kIme:
#endif  // defined(OS_CHROMEOS)
      DCHECK(command_line->GetSwitchValueASCII(switches::kProcessType) ==
             switches::kUtilityProcess);
      DCHECK(!command_line->HasSwitch(switches::kServiceSandboxType));
      command_line->AppendSwitchASCII(
          switches::kServiceSandboxType,
          StringFromUtilitySandboxType(sandbox_type));
      break;
#if defined(OS_FUCHSIA)
    case SandboxType::kWebContext:
#endif  // defined(OS_FUCHSIA)
#if defined(OS_MACOSX)
    case SandboxType::kNaClLoader:
#endif  // defined(OS_MACOSX)
    case SandboxType::kInvalid:
      break;
  }
}

SandboxType SandboxTypeFromCommandLine(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(switches::kNoSandbox))
    return SandboxType::kNoSandbox;

#if defined(OS_WIN)
  if (command_line.HasSwitch(switches::kNoSandboxAndElevatedPrivileges))
    return SandboxType::kNoSandboxAndElevatedPrivileges;
#endif

  std::string process_type =
      command_line.GetSwitchValueASCII(switches::kProcessType);
  if (process_type.empty())
    return SandboxType::kNoSandbox;

  if (process_type == switches::kRendererProcess)
    return SandboxType::kRenderer;

  if (process_type == switches::kUtilityProcess) {
    return UtilitySandboxTypeFromString(
        command_line.GetSwitchValueASCII(switches::kServiceSandboxType));
  }
  if (process_type == switches::kGpuProcess) {
    if (command_line.HasSwitch(switches::kDisableGpuSandbox))
      return SandboxType::kNoSandbox;
    return SandboxType::kGpu;
  }
  if (process_type == switches::kPpapiBrokerProcess)
    return SandboxType::kNoSandbox;

  if (process_type == switches::kPpapiPluginProcess)
    return SandboxType::kPpapi;

#if defined(OS_MACOSX)
  if (process_type == switches::kNaClLoaderProcess)
    return SandboxType::kNaClLoader;
#endif

  // This is a process which we don't know about.
  return SandboxType::kInvalid;
}

std::string StringFromUtilitySandboxType(SandboxType sandbox_type) {
  switch (sandbox_type) {
    case SandboxType::kNoSandbox:
      return switches::kNoneSandbox;
    case SandboxType::kNetwork:
      return switches::kNetworkSandbox;
    case SandboxType::kPpapi:
      return switches::kPpapiSandbox;
    case SandboxType::kCdm:
      return switches::kCdmSandbox;
    case SandboxType::kPdfCompositor:
      return switches::kPdfCompositorSandbox;
    case SandboxType::kProfiling:
      return switches::kProfilingSandbox;
    case SandboxType::kUtility:
      return switches::kUtilitySandbox;
    case SandboxType::kAudio:
      return switches::kAudioSandbox;
#if defined(OS_WIN)
    case SandboxType::kXrCompositing:
      return switches::kXrCompositingSandbox;
#endif  // defined(OS_WIN)
#if defined(OS_CHROMEOS)
    case SandboxType::kIme:
      return switches::kImeSandbox;
#endif  // defined(OS_CHROMEOS)
      // The following are not utility processes so should not occur.
    case SandboxType::kRenderer:
    case SandboxType::kGpu:
#if defined(OS_WIN)
    case SandboxType::kNoSandboxAndElevatedPrivileges:
#endif  // defined(OS_WIN)
#if defined(OS_MACOSX)
    case SandboxType::kNaClLoader:
#endif  // defined(OS_MACOSX)
#if defined(OS_FUCHSIA)
    case SandboxType::kWebContext:
#endif  // defined(OS_FUCHSIA)
    case SandboxType::kInvalid:
      NOTREACHED();
      return std::string();
  }
}

SandboxType UtilitySandboxTypeFromString(const std::string& sandbox_string) {
  if (sandbox_string == switches::kNoneSandbox)
    return SandboxType::kNoSandbox;
  if (sandbox_string == switches::kNoneSandboxAndElevatedPrivileges) {
#if defined(OS_WIN)
    return SandboxType::kNoSandboxAndElevatedPrivileges;
#else
    return SandboxType::kNoSandbox;
#endif
  }
  if (sandbox_string == switches::kNetworkSandbox)
    return SandboxType::kNetwork;
  if (sandbox_string == switches::kPpapiSandbox)
    return SandboxType::kPpapi;
  if (sandbox_string == switches::kCdmSandbox)
    return SandboxType::kCdm;
  if (sandbox_string == switches::kPdfCompositorSandbox)
    return SandboxType::kPdfCompositor;
  if (sandbox_string == switches::kProfilingSandbox)
    return SandboxType::kProfiling;
#if defined(OS_WIN)
  if (sandbox_string == switches::kXrCompositingSandbox)
    return SandboxType::kXrCompositing;
#endif
  if (sandbox_string == switches::kAudioSandbox)
    return SandboxType::kAudio;
#if defined(OS_CHROMEOS)
  if (sandbox_string == switches::kImeSandbox)
    return SandboxType::kIme;
#endif  // defined(OS_CHROMEOS)
  return SandboxType::kUtility;
}

void EnableAudioSandbox(bool enable) {
  if (enable) {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        switches::kEnableAudioServiceSandbox);
  }
}

bool IsAudioSandboxEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kEnableAudioServiceSandbox);
}

}  // namespace service_manager
