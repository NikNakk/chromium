// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_OPENXR_FOVEATION_H_
#define DEVICE_VR_OPENXR_OPENXR_FOVEATION_H_

#include <cstdint>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "device/vr/vr_export.h"
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gfx/geometry/size.h"

// Experimental graphics transport companion for standard XR_FB_foveation.
// Policy and eye-tracked semantics remain owned by FB/META; this only exposes
// the Metal rendering state needed by clients using XR_KHR_metal_enable.
#ifndef XR_MNDX_foveation_metal
#define XR_MNDX_foveation_metal 1
#define XR_MNDX_foveation_metal_SPEC_VERSION 3
#define XR_MNDX_FOVEATION_METAL_EXTENSION_NAME "XR_MNDX_foveation_metal"
#define XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT 16
#define XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT 129
#define XR_TYPE_FOVEATION_METAL_STATE_MNDX ((XrStructureType)0x7fff5057)
#define XR_TYPE_FOVEATION_METAL_PACKED_STATE_MNDX   ((XrStructureType)0x7fff5058)
#define XR_TYPE_FOVEATION_METAL_IMAGE_LAYOUT_MNDX ((XrStructureType)0x7fff5059)

typedef struct XrFoveationMetalStateMNDX {
  XrStructureType type;
  RAW_PTR_EXCLUSION void* next;
  XrBool32 foveationEnabled;
  RAW_PTR_EXCLUSION void* rasterizationRateMap;
  uint32_t physicalWidth;
  uint32_t physicalHeight;
  uint32_t revision;
} XrFoveationMetalStateMNDX;

typedef struct XrFoveationMetalViewMNDX {
  uint32_t viewIndex;
  XrRect2Di imageRect;
} XrFoveationMetalViewMNDX;

typedef struct XrFoveationMetalPackedStateMNDX {
  XrStructureType type;
  RAW_PTR_EXCLUSION void* next;
  uint32_t viewCount;
  RAW_PTR_EXCLUSION const XrFoveationMetalViewMNDX* views;
  uint32_t horizontalSampleCount;
  uint32_t verticalSampleCount;
  float horizontalSampleRates[XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT];
  float verticalSampleRates[XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT];
  uint32_t boundaryCount;
  float x[XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT];
  float y[XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT];
} XrFoveationMetalPackedStateMNDX;

// Spec version 3, chained to XrFoveationMetalPackedStateMNDX::next: the
// packed views are stored vertically mirrored and submitted with a flip.
typedef struct XrFoveationMetalImageLayoutMNDX {
  XrStructureType type;
  RAW_PTR_EXCLUSION const void* next;
  XrBool32 verticalFlip;
} XrFoveationMetalImageLayoutMNDX;

typedef XrResult(XRAPI_PTR* PFN_xrGetFoveationMetalStateMNDX)(
    XrSwapchain swapchain,
    uint32_t viewIndex,
    uint32_t arrayLayer,
    XrFoveationMetalStateMNDX* state);
#endif

namespace device {

// The runtime's resolved Metal rasterization-rate recipe for one packed render
// target: graphics state only, never gaze.
struct DEVICE_VR_EXPORT OpenXrResolvedFoveationRateMap {
  gfx::Size logical_size;
  gfx::Size physical_size;
  std::vector<float> horizontal_rates;
  std::vector<float> vertical_rates;
};

}  // namespace device

#endif
