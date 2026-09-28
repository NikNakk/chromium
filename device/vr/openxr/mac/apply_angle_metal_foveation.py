#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Apply the small ANGLE/Metal bridge used by Chromium XR foveation on macOS.

ANGLE is a separate DEPS checkout, so this experimental Chromium branch cannot
carry changes to it directly. The patch is intentionally narrow: when ANGLE is
about to create a Metal render pass, it looks for foveation policy attached to
an IOSurface by the isolated XR service and supplies the corresponding
MTLRasterizationRateMap.

The patch is idempotent and pinned to the ANGLE revision in this Chromium
branch. If ANGLE rolls and the surrounding source changes, fail loudly so the
bridge can be rebased instead of silently patching the wrong code.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

EXPECTED_ANGLE_REVISION = "2bb28bbcef760ede0c084f4cf3341f495887f1f5"
MARKER = "org.chromium.openxr.metal-foveation-v1"

HEADER_OLD = r"""    angle::ObjCPtr<MTLRenderPassDescriptor> mCachedRenderPassDescObjC;

    angle::ObjCPtr<NSString> mLabel;
"""

HEADER_NEW = r"""    angle::ObjCPtr<MTLRenderPassDescriptor> mCachedRenderPassDescObjC;

#if TARGET_OS_OSX
    // Chromium XR: cache the rate map derived from the active IOSurface
    // metadata. The cache is per Metal context/encoder and is refreshed only
    // when the foveation metadata changes (normally a quantized gaze move).
    angle::ObjCPtr<NSDictionary> mXrFoveationMetadata;
    angle::ObjCPtr<id<MTLRasterizationRateMap>> mXrFoveationRateMap;
#endif

    angle::ObjCPtr<NSString> mLabel;
"""

INCLUDES_OLD = r"""#include <cassert>
#include <cstdint>
#include <random>
#include <type_traits>
"""

INCLUDES_NEW = r"""#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <type_traits>
#include <vector>
"""

ANGLE_INCLUDES_OLD = r"""#include "libANGLE/renderer/metal/mtl_resources.h"
#include "libANGLE/renderer/metal/mtl_utils.h"

// Use to compare
"""

ANGLE_INCLUDES_NEW = r"""#include "libANGLE/renderer/metal/mtl_resources.h"
#include "libANGLE/renderer/metal/mtl_utils.h"

#if TARGET_OS_OSX
#    import <IOSurface/IOSurface.h>
#endif

// Use to compare
"""

NAMESPACE_OLD = r"""namespace
{

#define ANGLE_MTL_CMD_X"""

NAMESPACE_NEW = r"""namespace
{

#if TARGET_OS_OSX

angle::ObjCPtr<NSDictionary>
CopyChromiumXrFoveationMetadata(id<MTLTexture> texture)
{
    if (texture == nil || texture.iosurface == nullptr)
    {
        return {};
    }

    CFTypeRef value =
        IOSurfaceCopyValue(texture.iosurface, CFSTR("org.chromium.openxr.metal-foveation-v1"));
    if (value == nullptr)
    {
        return {};
    }
    if (CFGetTypeID(value) != CFDictionaryGetTypeID())
    {
        CFRelease(value);
        return {};
    }

    // IOSurfaceCopyValue returns an owned CF object. NSDictionary is
    // toll-free bridged and ObjCPtr will balance that ownership with -release.
    return angle::adoptObjCPtr((__bridge NSDictionary *)value);
}

angle::ObjCPtr<id<MTLRasterizationRateMap>>
BuildChromiumXrFoveationRateMap(id<MTLTexture> renderTexture, NSDictionary *metadata)
{
    if (renderTexture == nil || metadata == nil)
    {
        return {};
    }

    NSNumber *versionValue = [metadata objectForKey:@"version"];
    NSNumber *widthValue = [metadata objectForKey:@"logical_width"];
    NSNumber *heightValue = [metadata objectForKey:@"logical_height"];
    NSNumber *zoneCountValue = [metadata objectForKey:@"zone_count"];
    NSArray *horizontalValues = [metadata objectForKey:@"horizontal"];
    NSArray *verticalValues = [metadata objectForKey:@"vertical"];

    if (![versionValue isKindOfClass:[NSNumber class]] ||
        [versionValue unsignedIntValue] != 1 ||
        ![widthValue isKindOfClass:[NSNumber class]] ||
        ![heightValue isKindOfClass:[NSNumber class]] ||
        ![zoneCountValue isKindOfClass:[NSNumber class]] ||
        ![horizontalValues isKindOfClass:[NSArray class]] ||
        ![verticalValues isKindOfClass:[NSArray class]])
    {
        return {};
    }

    const NSUInteger logicalWidth = [widthValue unsignedIntegerValue];
    const NSUInteger logicalHeight = [heightValue unsignedIntegerValue];
    const NSUInteger zoneCount = [zoneCountValue unsignedIntegerValue];
    if (logicalWidth == 0 || logicalHeight == 0 || zoneCount == 0 || zoneCount > 64 ||
        horizontalValues.count != zoneCount || verticalValues.count != zoneCount ||
        renderTexture.width != logicalWidth || renderTexture.height != logicalHeight)
    {
        return {};
    }

    id<MTLDevice> device = renderTexture.device;
    if (device == nil || ![device supportsRasterizationRateMapWithLayerCount:1])
    {
        return {};
    }

    std::vector<float> horizontal(zoneCount);
    std::vector<float> vertical(zoneCount);
    for (NSUInteger i = 0; i < zoneCount; ++i)
    {
        id horizontalValue = [horizontalValues objectAtIndex:i];
        id verticalValue = [verticalValues objectAtIndex:i];
        if (![horizontalValue isKindOfClass:[NSNumber class]] ||
            ![verticalValue isKindOfClass:[NSNumber class]])
        {
            return {};
        }

        const float h = [(NSNumber *)horizontalValue floatValue];
        const float v = [(NSNumber *)verticalValue floatValue];
        if (!std::isfinite(h) || !std::isfinite(v) || h <= 0.0f || h > 1.0f ||
            v <= 0.0f || v > 1.0f)
        {
            return {};
        }
        horizontal[i] = h;
        vertical[i] = v;
    }

    angle::ObjCPtr<MTLRasterizationRateLayerDescriptor> layer =
        angle::adoptObjCPtr([[MTLRasterizationRateLayerDescriptor alloc]
            initWithSampleCount:MTLSizeMake(zoneCount, zoneCount, 1)
                     horizontal:horizontal.data()
                       vertical:vertical.data()]);
    if (!layer)
    {
        return {};
    }

    angle::ObjCPtr<MTLRasterizationRateMapDescriptor> descriptor =
        angle::adoptObjCPtr([[MTLRasterizationRateMapDescriptor alloc] init]);
    descriptor.get().screenSize = MTLSizeMake(logicalWidth, logicalHeight, 1);
    [descriptor.get() setLayer:layer.get() atIndex:0];

    angle::ObjCPtr<id<MTLRasterizationRateMap>> rateMap =
        angle::adoptObjCPtr([device newRasterizationRateMapWithDescriptor:descriptor.get()]);
    if (!rateMap)
    {
        return {};
    }

    // Chromium's fused path deliberately keeps the OpenXR/IOSurface texture at
    // logical size: Metal writes the compact physical raster into its leading
    // region, and the OpenXR compositor consumes the matching logical->physical
    // mapping supplied by XR_MNDX_foveation.
    const MTLSize physicalSize = [rateMap.get() physicalSizeForLayer:0];
    if (physicalSize.width == 0 || physicalSize.height == 0 ||
        physicalSize.width > renderTexture.width || physicalSize.height > renderTexture.height)
    {
        return {};
    }

    ANGLE_MTL_LOG("Chromium XR foveation logical=%lux%lu physical=%lux%lu zones=%lu",
                  static_cast<unsigned long>(logicalWidth),
                  static_cast<unsigned long>(logicalHeight),
                  static_cast<unsigned long>(physicalSize.width),
                  static_cast<unsigned long>(physicalSize.height),
                  static_cast<unsigned long>(zoneCount));
    return rateMap;
}

#endif  // TARGET_OS_OSX

#define ANGLE_MTL_CMD_X"""

RESTART_OLD = r"""    // Convert to Objective-C descriptor
    mRenderPassDesc.convertToMetalDesc(mCachedRenderPassDescObjC, deviceMaxRenderTargets);

    // The actual Objective-C encoder will be created later in endEncoding(), we do so in order
"""

RESTART_NEW = r"""    // Convert to Objective-C descriptor
    mRenderPassDesc.convertToMetalDesc(mCachedRenderPassDescObjC, deviceMaxRenderTargets);

#if TARGET_OS_OSX
    // The isolated XR service attaches graphics-independent foveation rates to
    // the IOSurface before Blink/ANGLE renders the frame. Consume them at the
    // last graphics-API boundary, immediately before Metal creates the native
    // render encoder. Ordinary WebGL surfaces have no metadata and remain
    // completely unchanged.
    mCachedRenderPassDescObjC.get().rasterizationRateMap = nil;
    if (mRenderPassDesc.numColorAttachments > 0)
    {
        MTLRenderPassColorAttachmentDescriptor *colorAttachment =
            mCachedRenderPassDescObjC.get().colorAttachments[0];
        id<MTLTexture> renderTexture = colorAttachment.texture;

        angle::ObjCPtr<NSDictionary> metadata =
            CopyChromiumXrFoveationMetadata(renderTexture);
        if (!metadata && colorAttachment.resolveTexture != nil)
        {
            // The default WebGL framebuffer may render through an internal
            // multisample attachment and resolve into the XR IOSurface.
            metadata = CopyChromiumXrFoveationMetadata(colorAttachment.resolveTexture);
        }

        if (metadata && renderTexture != nil)
        {
            const bool metadataChanged =
                !mXrFoveationMetadata ||
                ![mXrFoveationMetadata.get() isEqualToDictionary:metadata.get()];
            if (metadataChanged)
            {
                angle::ObjCPtr<id<MTLRasterizationRateMap>> rateMap =
                    BuildChromiumXrFoveationRateMap(renderTexture, metadata.get());
                if (rateMap)
                {
                    mXrFoveationMetadata = metadata;
                    mXrFoveationRateMap = std::move(rateMap);
                }
                else
                {
                    mXrFoveationMetadata.reset();
                    mXrFoveationRateMap.reset();
                }
            }

            if (mXrFoveationRateMap)
            {
                mCachedRenderPassDescObjC.get().rasterizationRateMap =
                    mXrFoveationRateMap.get();
            }
        }
    }
#endif  // TARGET_OS_OSX

    // The actual Objective-C encoder will be created later in endEncoding(), we do so in order
"""


def _replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one source match, found {count}")
    return text.replace(old, new, 1)


def _angle_revision(angle_root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(angle_root), "rev-parse", "HEAD"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def apply_patch(check_only: bool) -> None:
    chromium_root = Path(__file__).resolve().parents[4]
    angle_root = chromium_root / "third_party" / "angle"
    header_path = angle_root / "src/libANGLE/renderer/metal/mtl_command_buffer.h"
    impl_path = angle_root / "src/libANGLE/renderer/metal/mtl_command_buffer.mm"

    if not header_path.is_file() or not impl_path.is_file():
        raise RuntimeError(
            "third_party/angle is unavailable; run gclient sync before generating the macOS OpenXR build"
        )

    header = header_path.read_text()
    impl = impl_path.read_text()

    already_applied = MARKER in impl and "mXrFoveationRateMap" in header
    if already_applied:
        return

    revision = _angle_revision(angle_root)
    if revision is not None and revision != EXPECTED_ANGLE_REVISION:
        raise RuntimeError(
            "ANGLE revision changed: expected "
            f"{EXPECTED_ANGLE_REVISION}, found {revision}. Rebase the XR Metal foveation bridge."
        )

    new_header = _replace_once(header, HEADER_OLD, HEADER_NEW, "mtl_command_buffer.h")
    new_impl = _replace_once(impl, INCLUDES_OLD, INCLUDES_NEW, "C++ includes")
    new_impl = _replace_once(
        new_impl, ANGLE_INCLUDES_OLD, ANGLE_INCLUDES_NEW, "IOSurface include"
    )
    new_impl = _replace_once(new_impl, NAMESPACE_OLD, NAMESPACE_NEW, "Metal helpers")
    new_impl = _replace_once(new_impl, RESTART_OLD, RESTART_NEW, "render-pass hook")

    if check_only:
        return

    # Validate every replacement before touching either file, then write both.
    header_path.write_text(new_header)
    impl_path.write_text(new_impl)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify that the pinned ANGLE source is patchable without modifying it",
    )
    args = parser.parse_args()

    try:
        apply_patch(args.check)
    except Exception as exc:
        print(f"Chromium XR ANGLE foveation patch failed: {exc}", file=sys.stderr)
        return 1

    # GN's exec_script uses this exact value as a generation-time assertion.
    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
