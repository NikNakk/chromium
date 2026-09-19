// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_MAC_OPENXR_PLATFORM_HELPER_MAC_H_
#define DEVICE_VR_OPENXR_MAC_OPENXR_PLATFORM_HELPER_MAC_H_

#include <memory>

#include "base/functional/callback.h"
#include "device/vr/openxr/openxr_platform_helper.h"
#include "device/vr/public/mojom/isolated_xr_service.mojom-forward.h"
#include "device/vr/vr_export.h"

namespace device {

// macOS platform helper for OpenXR. The runtime is discovered through the
// Khronos loader, matching the externally-managed desktop runtime model.
class DEVICE_VR_EXPORT OpenXrPlatformHelperMac : public OpenXrPlatformHelper {
 public:
  OpenXrPlatformHelperMac();

  OpenXrPlatformHelperMac(const OpenXrPlatformHelperMac&) = delete;
  OpenXrPlatformHelperMac& operator=(const OpenXrPlatformHelperMac&) = delete;

  ~OpenXrPlatformHelperMac() override;

  // OpenXrPlatformHelper:
  std::unique_ptr<OpenXrGraphicsBinding> GetGraphicsBinding() override;
  void GetPlatformCreateInfo(
      const device::OpenXrCreateInfo& create_info,
      PlatformCreateInfoReadyCallback result_callback,
      PlatormInitiatedShutdownCallback shutdown_callback) override;
  device::mojom::XRDeviceData GetXRDeviceData() override;
  void PrepareForSessionShutdown(
      base::OnceClosure shutdown_ready_callback) override;

  bool IsApiAvailable();
  bool IsHardwareAvailable();

  using OpenXrPlatformHelper::CreateInstance;
  XrResult CreateInstance(XrInstance* instance, void* create_info) override;

 protected:
  bool Initialize() override;

 private:
  bool EnsurePollingInstance();
};

}  // namespace device

#endif  // DEVICE_VR_OPENXR_MAC_OPENXR_PLATFORM_HELPER_MAC_H_
