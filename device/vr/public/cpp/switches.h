// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_PUBLIC_CPP_SWITCHES_H_
#define DEVICE_VR_PUBLIC_CPP_SWITCHES_H_

#include "base/component_export.h"

namespace device::switches {
COMPONENT_EXPORT(VR_FEATURES)
extern const char kWebXrHandAnonymizationStrategy[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kWebXrHandAnonymizationStrategyNone[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kWebXrHandAnonymizationStrategyRuntime[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kWebXrHandAnonymizationStrategyFallback[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kWebXrMaxFramebufferScale[];

// Diagnostic controls for browser-owned immersive media. These switches do
// not affect page-created WebXR sessions.
COMPONENT_EXPORT(VR_FEATURES)
extern const char kXrFoveationMode[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kXrFoveationModeOff[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kXrFoveationModeFixed[];
COMPONENT_EXPORT(VR_FEATURES)
extern const char kXrFoveationModeDynamic[];
// Standard XR_FB_foveation level: "low", "medium" or "high" (0-2 accepted
// as aliases). Used for both fixed and dynamic modes.
COMPONENT_EXPORT(VR_FEATURES)
extern const char kXrFoveationLevel[];
}  // namespace device::switches

#endif  // DEVICE_VR_PUBLIC_CPP_SWITCHES_H_
