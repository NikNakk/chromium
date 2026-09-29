// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>

#include "base/containers/span.h"
#include "device/vr/openxr/spherical_video_mesh_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  (void)device::ParseSphericalVideoMesh(base::span(data, size));
  return 0;
}
