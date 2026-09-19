// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/mac/openxr_platform_helper_mac.h"

#include <utility>
#include <vector>

#include "base/logging.h"
#include "device/vr/openxr/openxr_api_wrapper.h"
#include "device/vr/openxr/openxr_graphics_binding.h"
#include "device/vr/public/mojom/isolated_xr_service.mojom.h"

namespace device {

// static
void OpenXrPlatformHelper::GetRequiredExtensions(
    std::vector<const char*>& extensions) {
  // macOS has no platform-specific required instance extensions.
}

// static
std::vector<const char*> OpenXrPlatformHelper::GetOptionalExtensions() {
  return {};
}

OpenXrPlatformHelperMac::OpenXrPlatformHelperMac() = default;

OpenXrPlatformHelperMac::~OpenXrPlatformHelperMac() {
  if (xr_instance_ != XR_NULL_HANDLE) {
    DestroyInstance(xr_instance_);
  }
}

bool OpenXrPlatformHelperMac::Initialize() {
  return true;
}

std::unique_ptr<OpenXrGraphicsBinding>
OpenXrPlatformHelperMac::GetGraphicsBinding() {
  // Runtime discovery is useful before the Metal graphics binding is complete.
  // Returning nullptr makes an immersive-session request fail cleanly in the
  // render loop rather than pretending that rendering is available.
  DVLOG(1) << "OpenXR Metal graphics binding is not implemented yet";
  return nullptr;
}

void OpenXrPlatformHelperMac::GetPlatformCreateInfo(
    const device::OpenXrCreateInfo& create_info,
    PlatformCreateInfoReadyCallback result_callback,
    PlatormInitiatedShutdownCallback /*shutdown_callback*/) {
  std::move(result_callback).Run(nullptr);
}

device::mojom::XRDeviceData OpenXrPlatformHelperMac::GetXRDeviceData() {
  device::mojom::XRDeviceData data;
  data.is_ar_blend_mode_supported =
      xr_instance_ != XR_NULL_HANDLE && IsArBlendModeSupported(xr_instance_);
  return data;
}

void OpenXrPlatformHelperMac::PrepareForSessionShutdown(
    base::OnceClosure shutdown_ready_callback) {
  std::move(shutdown_ready_callback).Run();
}

XrResult OpenXrPlatformHelperMac::CreateInstance(XrInstance* instance,
                                                 void* create_info) {
  CHECK(!create_info);
  if (xr_instance_ != XR_NULL_HANDLE) {
    *instance = xr_instance_;
    return XR_SUCCESS;
  }
  return OpenXrPlatformHelper::CreateInstance(instance, create_info);
}

bool OpenXrPlatformHelperMac::EnsurePollingInstance() {
  if (xr_instance_ != XR_NULL_HANDLE) {
    return true;
  }

  XrInstance instance = XR_NULL_HANDLE;
  return XR_SUCCEEDED(OpenXrPlatformHelper::CreateInstance(&instance, nullptr));
}

bool OpenXrPlatformHelperMac::IsApiAvailable() {
  return EnsurePollingInstance();
}

bool OpenXrPlatformHelperMac::IsHardwareAvailable() {
  if (!EnsurePollingInstance()) {
    return false;
  }

  XrSystemId system;
  return XR_SUCCEEDED(OpenXrApiWrapper::GetSystem(xr_instance_, &system));
}

}  // namespace device
