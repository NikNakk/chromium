// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_SPHERICAL_VIDEO_MESH_PARSER_H_
#define DEVICE_VR_OPENXR_SPHERICAL_VIDEO_MESH_PARSER_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "device/vr/vr_export.h"

namespace device {

struct SphericalVideoMeshVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

struct SphericalVideoMesh {
  std::vector<SphericalVideoMeshVertex> vertices;
  // Normalized triangle-list indices. Spherical Video V2 triangle strips and
  // fans are expanded by the parser so renderers only need one primitive type.
  std::vector<uint32_t> triangle_indices;
};

// Parses the ProjectionPrivate payload for a Spherical Video V2 Mesh
// projection. WebM carries the mshp FullBox payload without the outer box
// size/FourCC. Returns one or two meshes on success.
DEVICE_VR_EXPORT std::optional<std::vector<SphericalVideoMesh>>
ParseSphericalVideoMesh(
    base::span<const uint8_t> projection_data);

}  // namespace device

#endif  // DEVICE_VR_OPENXR_SPHERICAL_VIDEO_MESH_PARSER_H_
