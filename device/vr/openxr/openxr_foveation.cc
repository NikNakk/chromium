// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/openxr_foveation.h"

#include <algorithm>
#include <cmath>

namespace device {

OpenXrFoveationBackend::~OpenXrFoveationBackend() = default;

namespace {
std::optional<XrFoveationLevelMNDX> ToXrFoveationLevel(
    OpenXrFoveationLevel level) {
  switch (level) {
    case OpenXrFoveationLevel::kReference:
      return XR_FOVEATION_LEVEL_REFERENCE_MNDX;
    case OpenXrFoveationLevel::kStrong:
      return XR_FOVEATION_LEVEL_STRONG_MNDX;
    case OpenXrFoveationLevel::kAggressive:
      return XR_FOVEATION_LEVEL_AGGRESSIVE_MNDX;
    case OpenXrFoveationLevel::kAggressivePlus:
      return XR_FOVEATION_LEVEL_AGGRESSIVE_PLUS_MNDX;
    case OpenXrFoveationLevel::kNearExtreme:
      return XR_FOVEATION_LEVEL_NEAR_EXTREME_MNDX;
    case OpenXrFoveationLevel::kExtreme:
      return XR_FOVEATION_LEVEL_EXTREME_MNDX;
  }
  return std::nullopt;
}
}  // namespace

std::optional<OpenXrFoveationPolicy> QueryOpenXrFoveationPolicy(
    XrInstance instance,
    XrSystemId system,
    OpenXrFoveationLevel level,
    PFN_xrGetFoveationProfileMNDX get_profile) {
  if (!get_profile || instance == XR_NULL_HANDLE || system == XR_NULL_SYSTEM_ID) {
    return std::nullopt;
  }
  const auto xr_level = ToXrFoveationLevel(level);
  if (!xr_level) {
    return std::nullopt;
  }
  XrFoveationProfileMNDX xr_profile{};
  xr_profile.type = XR_TYPE_FOVEATION_PROFILE_MNDX;
  if (XR_FAILED(get_profile(instance, system, *xr_level, &xr_profile))) {
    return std::nullopt;
  }

  OpenXrFoveationPolicy policy;
  policy.level = level;
  policy.center_rate = xr_profile.centerRate;
  policy.middle_rate = xr_profile.middleRate;
  policy.peripheral_rate = xr_profile.peripheralRate;
  policy.center_half_extent = xr_profile.centerHalfExtent;
  policy.middle_half_extent = xr_profile.middleHalfExtent;
  return policy;
}

float OpenXrFoveationRateForOffset(const OpenXrFoveationPolicy& policy,
                                   float normalized_offset) {
  normalized_offset = std::abs(normalized_offset);
  if (normalized_offset <= policy.center_half_extent) {
    return policy.center_rate;
  }
  if (normalized_offset <= policy.middle_half_extent) {
    return policy.middle_rate;
  }
  return policy.peripheral_rate;
}

bool BuildOpenXrFoveationAxisRates(const OpenXrFoveationPolicy& policy,
                                   uint32_t center_index,
                                   base::span<float> out_rates) {
  if (out_rates.empty() || center_index >= out_rates.size()) {
    return false;
  }
  const float count = static_cast<float>(out_rates.size());
  for (size_t i = 0; i < out_rates.size(); ++i) {
    const size_t delta =
        i > center_index ? i - center_index : center_index - i;
    out_rates[i] = OpenXrFoveationRateForOffset(
        policy, static_cast<float>(delta) / count);
  }
  return true;
}

bool BuildOpenXrFoveationAxisRates(
    const OpenXrFoveationPolicy& policy,
    base::span<const float> normalized_centers,
    base::span<float> out_rates) {
  const std::vector<float> full_target(normalized_centers.size(), 1.0f);
  return BuildOpenXrFoveationAxisRates(policy, normalized_centers, full_target,
                                       out_rates);
}

bool BuildOpenXrFoveationAxisRates(
    const OpenXrFoveationPolicy& policy,
    base::span<const float> normalized_centers,
    base::span<const float> center_view_extents,
    base::span<float> out_rates) {
  if (out_rates.empty() || normalized_centers.empty() ||
      center_view_extents.size() != normalized_centers.size()) {
    return false;
  }

  std::ranges::fill(out_rates, 0.0f);
  const uint32_t count = static_cast<uint32_t>(out_rates.size());
  for (size_t c = 0; c < normalized_centers.size(); ++c) {
    const float view_extent = center_view_extents[c];
    if (!(view_extent > 0.0f) || view_extent > 1.0f) {
      return false;
    }
    const float clamped = std::clamp(normalized_centers[c], 0.0f, 1.0f);
    const uint32_t center_index =
        std::min(static_cast<uint32_t>(clamped * count), count - 1);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t delta =
          i > center_index ? i - center_index : center_index - i;
      const float target_offset =
          static_cast<float>(delta) / static_cast<float>(count);
      out_rates[i] = std::max(
          out_rates[i],
          OpenXrFoveationRateForOffset(policy, target_offset / view_extent));
    }
  }
  return true;
}

bool GetOpenXrFoveationAxisInputs(const OpenXrFoveationTargetConfig& config,
                                  bool horizontal,
                                  std::vector<float>& out_centers,
                                  std::vector<float>& out_view_extents) {
  if (!config.view_extents.empty() &&
      config.view_extents.size() != config.centers.size()) {
    return false;
  }
  out_centers.clear();
  out_view_extents.clear();
  for (size_t i = 0; i < config.centers.size(); ++i) {
    out_centers.push_back(horizontal ? config.centers[i].x()
                                     : config.centers[i].y());
    if (config.view_extents.empty()) {
      out_view_extents.push_back(1.0f);
    } else {
      out_view_extents.push_back(horizontal
                                     ? config.view_extents[i].width()
                                     : config.view_extents[i].height());
    }
  }
  return !out_centers.empty();
}

XrCompositionLayerFoveationMapMNDX MakeOpenXrFoveationCompositionMap(
    const OpenXrFoveationMapping& mapping) {
  XrCompositionLayerFoveationMapMNDX out{};
  out.type = XR_TYPE_COMPOSITION_LAYER_FOVEATION_MAP_MNDX;
  out.boundaryCount = XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT;
  std::ranges::copy(mapping.x, out.x);
  std::ranges::copy(mapping.y, out.y);
  return out;
}

}  // namespace device
