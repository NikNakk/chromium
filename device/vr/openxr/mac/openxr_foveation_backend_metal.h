// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_MAC_OPENXR_FOVEATION_BACKEND_METAL_H_
#define DEVICE_VR_OPENXR_MAC_OPENXR_FOVEATION_BACKEND_METAL_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "device/vr/openxr/openxr_foveation.h"
#include "device/vr/vr_export.h"

namespace device {

class DEVICE_VR_EXPORT OpenXrFoveationBackendMetal final
    : public OpenXrFoveationBackend {
 public:
  explicit OpenXrFoveationBackendMetal(void* metal_device);
  ~OpenXrFoveationBackendMetal() override;

  bool IsSupported() const override;
  std::optional<OpenXrFoveationViewState> ConfigureView(
      uint32_t view_index,
      const OpenXrFoveationViewConfig& config) override;
  void ResetView(uint32_t view_index) override;
  void Reset() override;

  // Opaque MTLRasterizationRateMap for the Metal rendering path.
  void* GetRasterizationRateMap(uint32_t view_index) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace device

#endif
