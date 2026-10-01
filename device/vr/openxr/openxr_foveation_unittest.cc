// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/openxr_foveation.h"

#include <array>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size_f.h"

namespace device {

namespace {

constexpr size_t kZones = 16;

OpenXrFoveationPolicy TestPolicy() {
  OpenXrFoveationPolicy policy;
  policy.center_rate = 1.0f;
  policy.middle_rate = 0.5f;
  policy.peripheral_rate = 0.25f;
  policy.center_half_extent = 0.15f;
  policy.middle_half_extent = 0.35f;
  return policy;
}

}  // namespace

TEST(OpenXrFoveationTest, FullTargetMatchesUnscaledBuilder) {
  const OpenXrFoveationPolicy policy = TestPolicy();
  const std::array<float, 1> centers = {0.4f};
  const std::array<float, 1> extents = {1.0f};
  std::array<float, kZones> unscaled;
  std::array<float, kZones> scaled;
  ASSERT_TRUE(BuildOpenXrFoveationAxisRates(policy, centers, unscaled));
  ASSERT_TRUE(BuildOpenXrFoveationAxisRates(policy, centers, extents, scaled));
  EXPECT_EQ(unscaled, scaled);
}

// In a side-by-side target each view is half the target width, so a target
// distance of 1/16 is 2/16 of the view: extents must not be doubled.
TEST(OpenXrFoveationTest, PackedViewExtentsAreViewLocal) {
  const OpenXrFoveationPolicy policy = TestPolicy();
  const std::array<float, 2> centers = {0.25f, 0.75f};
  const std::array<float, 2> half = {0.5f, 0.5f};
  std::array<float, kZones> rates;
  ASSERT_TRUE(BuildOpenXrFoveationAxisRates(policy, centers, half, rates));

  // Left centre is zone 4. Zone 6 is 2/16 of the target = 0.25 of the view:
  // middle rate, not the centre rate a full-target measure (0.125) gives.
  EXPECT_FLOAT_EQ(rates[4], policy.center_rate);
  EXPECT_FLOAT_EQ(rates[5], policy.center_rate);  // 0.125 of the view
  EXPECT_FLOAT_EQ(rates[6], policy.middle_rate);
  EXPECT_FLOAT_EQ(rates[1], policy.peripheral_rate);  // 0.375 of the view
  // Right centre is zone 12 and is treated identically.
  EXPECT_FLOAT_EQ(rates[12], policy.center_rate);
  EXPECT_FLOAT_EQ(rates[10], policy.middle_rate);
  EXPECT_FLOAT_EQ(rates[15], policy.peripheral_rate);
}

TEST(OpenXrFoveationTest, RejectsInvalidViewExtents) {
  const OpenXrFoveationPolicy policy = TestPolicy();
  const std::array<float, 2> centers = {0.25f, 0.75f};
  const std::array<float, 1> too_few = {0.5f};
  const std::array<float, 2> zero = {0.5f, 0.0f};
  const std::array<float, 2> too_large = {0.5f, 1.5f};
  std::array<float, kZones> rates;
  EXPECT_FALSE(BuildOpenXrFoveationAxisRates(policy, centers, too_few, rates));
  EXPECT_FALSE(BuildOpenXrFoveationAxisRates(policy, centers, zero, rates));
  EXPECT_FALSE(
      BuildOpenXrFoveationAxisRates(policy, centers, too_large, rates));
}

TEST(OpenXrFoveationTest, AxisInputsFollowTargetConfig) {
  OpenXrFoveationTargetConfig config;
  config.centers = {gfx::PointF(0.2f, 0.6f), gfx::PointF(0.7f, 0.4f)};
  std::vector<float> centers;
  std::vector<float> extents;

  // No extents: each centre covers the whole target.
  ASSERT_TRUE(GetOpenXrFoveationAxisInputs(config, /*horizontal=*/true,
                                           centers, extents));
  EXPECT_EQ(centers, (std::vector<float>{0.2f, 0.7f}));
  EXPECT_EQ(extents, (std::vector<float>{1.0f, 1.0f}));

  config.view_extents = {gfx::SizeF(0.6f, 1.0f), gfx::SizeF(0.4f, 0.9f)};
  ASSERT_TRUE(GetOpenXrFoveationAxisInputs(config, /*horizontal=*/false,
                                           centers, extents));
  EXPECT_EQ(centers, (std::vector<float>{0.6f, 0.4f}));
  EXPECT_EQ(extents, (std::vector<float>{1.0f, 0.9f}));

  config.view_extents.pop_back();
  EXPECT_FALSE(GetOpenXrFoveationAxisInputs(config, /*horizontal=*/true,
                                            centers, extents));
}

}  // namespace device
