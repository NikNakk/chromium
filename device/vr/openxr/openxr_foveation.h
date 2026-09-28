// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_OPENXR_FOVEATION_H_
#define DEVICE_VR_OPENXR_OPENXR_FOVEATION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "device/vr/vr_export.h"
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"

// Experimental OpenXR wire contract shared with Monado. Keep this isolated
// from Chromium's generic policy/backend interface below so a standardized
// extension can replace it without changing graphics backends.
#ifndef XR_MNDX_foveation
#define XR_MNDX_foveation 1
#define XR_MNDX_foveation_SPEC_VERSION 1
#define XR_MNDX_FOVEATION_EXTENSION_NAME "XR_MNDX_foveation"
#define XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT 129
#define XR_TYPE_FOVEATION_PROFILE_MNDX ((XrStructureType)0x7fff5055)
#define XR_TYPE_COMPOSITION_LAYER_FOVEATION_MAP_MNDX \
  ((XrStructureType)0x7fff5056)

typedef enum XrFoveationLevelMNDX {
  XR_FOVEATION_LEVEL_REFERENCE_MNDX = 0,
  XR_FOVEATION_LEVEL_STRONG_MNDX = 1,
  XR_FOVEATION_LEVEL_AGGRESSIVE_MNDX = 2,
  XR_FOVEATION_LEVEL_AGGRESSIVE_PLUS_MNDX = 3,
  XR_FOVEATION_LEVEL_NEAR_EXTREME_MNDX = 4,
  XR_FOVEATION_LEVEL_EXTREME_MNDX = 5,
  XR_FOVEATION_LEVEL_MAX_ENUM_MNDX = 0x7fffffff
} XrFoveationLevelMNDX;

typedef struct XrFoveationProfileMNDX {
  XrStructureType type;
  void* next;
  XrFoveationLevelMNDX level;
  float centerRate;
  float middleRate;
  float peripheralRate;
  float centerHalfExtent;
  float middleHalfExtent;
} XrFoveationProfileMNDX;

typedef struct XrCompositionLayerFoveationMapMNDX {
  XrStructureType type;
  const void* next;
  uint32_t boundaryCount;
  float x[XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT];
  float y[XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT];
} XrCompositionLayerFoveationMapMNDX;

typedef XrResult(XRAPI_PTR* PFN_xrGetFoveationProfileMNDX)(
    XrInstance instance,
    XrSystemId systemId,
    XrFoveationLevelMNDX level,
    XrFoveationProfileMNDX* profile);
#endif

namespace device {

inline constexpr size_t kOpenXrFoveationMapBoundaryCount =
    XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT;

enum class OpenXrFoveationLevel {
  kReference = 0,
  kStrong,
  kAggressive,
  kAggressivePlus,
  kNearExtreme,
  kExtreme,
};

// Runtime policy: graphics API independent.
struct DEVICE_VR_EXPORT OpenXrFoveationPolicy {
  OpenXrFoveationLevel level = OpenXrFoveationLevel::kReference;
  float center_rate = 1.0f;
  float middle_rate = 1.0f;
  float peripheral_rate = 1.0f;
  float center_half_extent = 0.0f;
  float middle_half_extent = 0.0f;
};

// Only needed when a backend compacts logical raster coordinates.
struct DEVICE_VR_EXPORT OpenXrFoveationMapping {
  std::array<float, kOpenXrFoveationMapBoundaryCount> x{};
  std::array<float, kOpenXrFoveationMapBoundaryCount> y{};
};

// Render-target input. A packed stereo target has two normalized centres;
 // a per-eye or mono target has one. Keeping this at target level lets the
 // policy remain independent of whether the graphics backend uses arrays,
 // separate swapchains, or a side-by-side texture.
struct DEVICE_VR_EXPORT OpenXrFoveationTargetConfig {
  gfx::Size logical_size;
  std::vector<gfx::PointF> centers;
  OpenXrFoveationPolicy policy;
};

// Backend result. Full-resolution shading-rate backends may omit mapping.
struct DEVICE_VR_EXPORT OpenXrFoveationTargetState {
  gfx::Size physical_size;
  std::optional<OpenXrFoveationMapping> mapping;
};

// Chromium-facing backend boundary. Policy/gaze selection lives above this;
// native graphics objects live strictly below it.
class DEVICE_VR_EXPORT OpenXrFoveationBackend {
 public:
  virtual ~OpenXrFoveationBackend();
  virtual bool IsSupported() const = 0;
  virtual std::optional<OpenXrFoveationTargetState> ConfigureTarget(
      uint32_t target_index,
      const OpenXrFoveationTargetConfig& config) = 0;
  virtual void ResetTarget(uint32_t target_index) = 0;
  virtual void Reset() = 0;
};

DEVICE_VR_EXPORT std::optional<OpenXrFoveationPolicy>
QueryOpenXrFoveationPolicy(XrInstance instance,
                           XrSystemId system,
                           OpenXrFoveationLevel level,
                           PFN_xrGetFoveationProfileMNDX get_profile);

DEVICE_VR_EXPORT float OpenXrFoveationRateForOffset(
    const OpenXrFoveationPolicy& policy,
    float normalized_offset);

DEVICE_VR_EXPORT bool BuildOpenXrFoveationAxisRates(
    const OpenXrFoveationPolicy& policy,
    uint32_t center_index,
    base::span<float> out_rates);

DEVICE_VR_EXPORT bool BuildOpenXrFoveationAxisRates(
    const OpenXrFoveationPolicy& policy,
    base::span<const float> normalized_centers,
    base::span<float> out_rates);

DEVICE_VR_EXPORT XrCompositionLayerFoveationMapMNDX
MakeOpenXrFoveationCompositionMap(const OpenXrFoveationMapping& mapping);

}  // namespace device

#endif
