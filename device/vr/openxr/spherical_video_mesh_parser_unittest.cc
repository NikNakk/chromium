// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/spherical_video_mesh_parser.h"

#include <array>
#include <cstdint>

#include "testing/gtest/include/gtest/gtest.h"

namespace device {
namespace {

constexpr auto kRawMeshProjection = std::to_array<uint8_t>({
    0x00, 0x00, 0x00, 0x00,  // FullBox version/flags
    0x00, 0x00, 0x00, 0x00,  // CRC (not required by parser)
    0x72, 0x61, 0x77, 0x20,  // 'raw '
    0x00, 0x00, 0x00, 0x20,  // mesh box size
    0x6d, 0x65, 0x73, 0x68,  // 'mesh'
    0x00, 0x00, 0x00, 0x01,  // coordinate count
    0x3f, 0x80, 0x00, 0x00,  // coordinate[0] = 1.0f
    0x00, 0x00, 0x00, 0x01,  // vertex count
    0x00,                    // five zero coordinate deltas + padding
    0x00, 0x00, 0x00, 0x01,  // vertex-list count
    0x00,                    // texture id = video
    0x00,                    // triangle-list primitive
    0x00, 0x00, 0x00, 0x03,  // index count
    0x00,                    // three zero vertex deltas + padding
});

// The same mesh box as above, raw-DEFLATE compressed. This exercises the dfl8
// encoding used by Spherical Video V2 without making the test itself depend on
// a compressor.
constexpr auto kDeflatedMeshProjection = std::to_array<uint8_t>({
    0x00, 0x00, 0x00, 0x00,  // FullBox version/flags
    0x00, 0x00, 0x00, 0x00,  // CRC
    0x64, 0x66, 0x6c, 0x38,  // 'dfl8'
    0x63, 0x60, 0x60, 0x50, 0xc8, 0x4d, 0x2d, 0xce,
    0x60, 0x60, 0x60, 0x60, 0xb4, 0x6f, 0x60, 0x00,
    0x01, 0x46, 0x38, 0xc1, 0xc0, 0xcc, 0x00, 0x00,
});

void ExpectSimpleMesh(base::span<const uint8_t> data) {
  auto meshes = ParseSphericalVideoMesh(data);
  ASSERT_TRUE(meshes);
  ASSERT_EQ(meshes->size(), 1u);

  const SphericalVideoMesh& mesh = meshes->front();
  ASSERT_EQ(mesh.vertices.size(), 1u);
  EXPECT_FLOAT_EQ(mesh.vertices[0].x, 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[0].y, 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[0].z, 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[0].u, 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[0].v, 1.0f);

  ASSERT_EQ(mesh.triangle_indices.size(), 3u);
  EXPECT_EQ(mesh.triangle_indices[0], 0u);
  EXPECT_EQ(mesh.triangle_indices[1], 0u);
  EXPECT_EQ(mesh.triangle_indices[2], 0u);
}

TEST(SphericalVideoMeshParserTest, ParsesRawMesh) {
  ExpectSimpleMesh(kRawMeshProjection);
}

TEST(SphericalVideoMeshParserTest, ParsesRawDeflateMesh) {
  ExpectSimpleMesh(kDeflatedMeshProjection);
}

TEST(SphericalVideoMeshParserTest, RejectsUnknownEncoding) {
  auto data = kRawMeshProjection;
  data[8] = 'b';
  data[9] = 'a';
  data[10] = 'd';
  data[11] = '!';
  EXPECT_FALSE(ParseSphericalVideoMesh(data));
}

TEST(SphericalVideoMeshParserTest, RejectsNonZeroVersion) {
  auto data = kRawMeshProjection;
  data[0] = 1;
  EXPECT_FALSE(ParseSphericalVideoMesh(data));
}

}  // namespace
}  // namespace device
