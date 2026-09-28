// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Metal/Metal.h>

#include "device/vr/openxr/mac/openxr_foveation_backend_metal.h"

#include <algorithm>
#include <array>
#include <map>

namespace device {
namespace {
constexpr uint32_t kZoneCount = 16;

uint32_t ZoneForCoordinate(float coordinate) {
  const float clamped = std::clamp(coordinate, 0.0f, 1.0f);
  return std::min(static_cast<uint32_t>(clamped * kZoneCount), kZoneCount - 1);
}
}  // namespace

class OpenXrFoveationBackendMetal::Impl {
 public:
  explicit Impl(void* d) : device((__bridge id<MTLDevice>)d) {}
  id<MTLDevice> __strong device = nil;
  std::map<uint32_t, id<MTLRasterizationRateMap>> maps;
};

OpenXrFoveationBackendMetal::OpenXrFoveationBackendMetal(void* device)
    : impl_(std::make_unique<Impl>(device)) {}
OpenXrFoveationBackendMetal::~OpenXrFoveationBackendMetal() = default;

bool OpenXrFoveationBackendMetal::IsSupported() const {
  return impl_->device != nil &&
         [impl_->device supportsRasterizationRateMapWithLayerCount:1];
}

std::optional<OpenXrFoveationTargetState>
OpenXrFoveationBackendMetal::ConfigureTarget(
    uint32_t target_index,
    const OpenXrFoveationTargetConfig& config) {
  if (!IsSupported() || config.logical_size.IsEmpty() ||
      config.centers.empty()) {
    return std::nullopt;
  }

  std::vector<float> center_x;
  std::vector<float> center_y;
  center_x.reserve(config.centers.size());
  center_y.reserve(config.centers.size());
  for (const gfx::PointF& center : config.centers) {
    center_x.push_back(center.x());
    center_y.push_back(center.y());
  }

  std::array<float, kZoneCount> horizontal;
  std::array<float, kZoneCount> vertical;
  if (!BuildOpenXrFoveationAxisRates(config.policy, center_x, horizontal) ||
      !BuildOpenXrFoveationAxisRates(config.policy, center_y, vertical)) {
    return std::nullopt;
  }

  MTLRasterizationRateLayerDescriptor* layer =
      [[MTLRasterizationRateLayerDescriptor alloc]
          initWithSampleCount:MTLSizeMake(kZoneCount, kZoneCount, 1)
                   horizontal:horizontal.data()
                     vertical:vertical.data()];
  MTLRasterizationRateMapDescriptor* descriptor =
      [[MTLRasterizationRateMapDescriptor alloc] init];
  descriptor.screenSize =
      MTLSizeMake(config.logical_size.width(), config.logical_size.height(), 1);
  [descriptor setLayer:layer atIndex:0];

  id<MTLRasterizationRateMap> map =
      [impl_->device newRasterizationRateMapWithDescriptor:descriptor];
  if (map == nil) {
    return std::nullopt;
  }

  const MTLSize physical = [map physicalSizeForLayer:0];
  if (physical.width == 0 || physical.height == 0) {
    return std::nullopt;
  }

  OpenXrFoveationTargetState state;
  state.physical_size = gfx::Size(static_cast<int>(physical.width),
                                  static_cast<int>(physical.height));
  state.mapping.emplace();
  for (size_t i = 0; i < kOpenXrFoveationMapBoundaryCount; ++i) {
    const float t = static_cast<float>(i) /
                    static_cast<float>(kOpenXrFoveationMapBoundaryCount - 1);
    const MTLCoordinate2D px =
        [map mapScreenToPhysicalCoordinates:
                 MTLCoordinate2DMake(config.logical_size.width() * t, 0.0)
                                forLayer:0];
    const MTLCoordinate2D py =
        [map mapScreenToPhysicalCoordinates:
                 MTLCoordinate2DMake(0.0, config.logical_size.height() * t)
                                forLayer:0];
    state.mapping->x[i] =
        static_cast<float>(px.x) / config.logical_size.width();
    state.mapping->y[i] =
        static_cast<float>(py.y) / config.logical_size.height();
  }

  impl_->maps[target_index] = map;
  return state;
}

void OpenXrFoveationBackendMetal::ResetTarget(uint32_t target_index) {
  impl_->maps.erase(target_index);
}
void OpenXrFoveationBackendMetal::Reset() {
  impl_->maps.clear();
}
void* OpenXrFoveationBackendMetal::GetRasterizationRateMap(
    uint32_t target_index) const {
  auto it = impl_->maps.find(target_index);
  return it == impl_->maps.end() ? nullptr : (__bridge void*)it->second;
}

}  // namespace device
