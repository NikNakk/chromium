// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_spatial_format.h"

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace media {

namespace {

const char* StereoModeToString(VideoStereoMode mode) {
  switch (mode) {
    case VideoStereoMode::kMono:
      return "mono";
    case VideoStereoMode::kSideBySideLeftFirst:
      return "side-by-side-left-first";
    case VideoStereoMode::kTopBottomLeftFirst:
      return "top-bottom-left-first";
    case VideoStereoMode::kStereoCustom:
      return "stereo-custom";
  }
  NOTREACHED();
}

const char* ProjectionTypeToString(VideoProjectionType projection_type) {
  switch (projection_type) {
    case VideoProjectionType::kNone:
      return "none";
    case VideoProjectionType::kEquirect180:
      return "equirect180";
    case VideoProjectionType::kEquirect360:
      return "equirect360";
    case VideoProjectionType::kMesh:
      return "mesh";
  }
  NOTREACHED();
}

}  // namespace

std::string VideoSpatialFormat::ToString() const {
  return base::StrCat(
      {"projection_type: ", ProjectionTypeToString(projection_type),
       ", stereo_mode: ", StereoModeToString(stereo_mode),
       ", projection_data_bytes: ",
       base::NumberToString(projection_data.size())});
}

}  // namespace media
