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
branch. A normal gclient hook warns and skips if ANGLE has rolled, so dependency
updates are not blocked. --check remains strict so maintainers can detect when
the bridge needs rebasing.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

EXPECTED_ANGLE_REVISION = "2bb28bbcef760ede0c084f4cf3341f495887f1f5"
# The IOSurface key name is unchanged; the dictionary carries "version": 2.
MARKER = "org.chromium.openxr.metal-foveation-v1"
# Present only in the current bridge (physical-size check + acknowledgement).
ACK_MARKER = "org.chromium.openxr.metal-foveation-applied"


TEXTURE_HEADER_OLD = r"""    angle::Result bindTexImage(const gl::Context *context, egl::Surface *surface) override;
    angle::Result releaseTexImage(const gl::Context *context) override;
"""

TEXTURE_HEADER_NEW = r"""    angle::Result bindTexImage(const gl::Context *context, egl::Surface *surface) override;
    angle::Result releaseTexImage(const gl::Context *context) override;
    angle::Result onLabelUpdate(const gl::Context *context) override;
"""

TEXTURE_BIND_OLD = r"""    ANGLE_TRY(ensureSamplerStateCreated(context));
    ANGLE_TRY(createViewFromBaseToMaxLevel());

    // Tell context to rebind textures
"""

TEXTURE_BIND_NEW = r"""    ANGLE_TRY(ensureSamplerStateCreated(context));
    ANGLE_TRY(createViewFromBaseToMaxLevel());
    ANGLE_TRY(onLabelUpdate(context));

    // Tell context to rebind textures
"""

TEXTURE_RELEASE_OLD = r"""angle::Result TextureMtl::releaseTexImage(const gl::Context *context)
{
    deallocateNativeStorage(/*keepImages=*/false);
    mBoundSurface = nullptr;
    return angle::Result::Continue;
}

angle::Result TextureMtl::getAttachmentRenderTarget"""

TEXTURE_RELEASE_NEW = r"""angle::Result TextureMtl::releaseTexImage(const gl::Context *context)
{
    deallocateNativeStorage(/*keepImages=*/false);
    mBoundSurface = nullptr;
    return angle::Result::Continue;
}

angle::Result TextureMtl::onLabelUpdate(const gl::Context *context)
{
    if (mNativeTextureStorage)
    {
        const std::string &label = mState.getLabel();
        mNativeTextureStorage->getNativeTexture()->get().label =
            label.empty() ? nil : [NSString stringWithUTF8String:label.c_str()];
    }
    return angle::Result::Continue;
}

angle::Result TextureMtl::getAttachmentRenderTarget"""

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

// CFSTR() is not a constant expression; keep the keys as macros.
#    define ANGLE_CHROMIUM_XR_FOVEATION_KEY CFSTR("org.chromium.openxr.metal-foveation-v1")
#    define ANGLE_CHROMIUM_XR_FOVEATION_APPLIED_KEY \
        CFSTR("org.chromium.openxr.metal-foveation-applied")

// ANGLE attaches mip/slice views of a texture's native storage. Views carry
// neither the label nor the IOSurface, so inspect the storage texture itself.
id<MTLTexture> ChromiumXrRootTexture(id<MTLTexture> texture)
{
    while (texture != nil && texture.parentTexture != nil)
    {
        texture = texture.parentTexture;
    }
    return texture;
}

bool IsChromiumXrFoveatedTexture(id<MTLTexture> texture)
{
    texture = ChromiumXrRootTexture(texture);
    if (texture == nil || texture.label == nil)
    {
        return false;
    }
    return [texture.label hasPrefix:@"ChromiumOpenXrFoveated"];
}

angle::ObjCPtr<NSDictionary>
CopyChromiumXrFoveationMetadata(id<MTLTexture> texture)
{
    texture = ChromiumXrRootTexture(texture);
    if (texture == nil || texture.iosurface == nullptr)
    {
        return {};
    }

    CFTypeRef value = IOSurfaceCopyValue(texture.iosurface, ANGLE_CHROMIUM_XR_FOVEATION_KEY);
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

// The XR process checks this before releasing the image to the OpenXR
// runtime: only an image acknowledged with the serial it published is
// composited foveated.
void AcknowledgeChromiumXrFoveationMetadata(id<MTLTexture> texture, NSDictionary *metadata)
{
    texture = ChromiumXrRootTexture(texture);
    NSNumber *serial = [metadata objectForKey:@"serial"];
    if (texture != nil && texture.iosurface != nullptr &&
        [serial isKindOfClass:[NSNumber class]])
    {
        IOSurfaceSetValue(texture.iosurface, ANGLE_CHROMIUM_XR_FOVEATION_APPLIED_KEY,
                          (__bridge CFTypeRef)serial);
    }
}

// Metadata differs every frame only by its serial; rebuild the Metal map only
// when the recipe itself changes.
bool SameChromiumXrFoveationRecipe(NSDictionary *a, NSDictionary *b)
{
    if (a == nil || b == nil)
    {
        return false;
    }
    // ANGLE's Metal backend is built without ARC: own the copies explicitly.
    angle::ObjCPtr<NSMutableDictionary> recipeA = angle::adoptObjCPtr([a mutableCopy]);
    angle::ObjCPtr<NSMutableDictionary> recipeB = angle::adoptObjCPtr([b mutableCopy]);
    [recipeA.get() removeObjectForKey:@"serial"];
    [recipeB.get() removeObjectForKey:@"serial"];
    return [recipeA.get() isEqualToDictionary:recipeB.get()];
}

// Removing the key tells the XR process, which checks it before releasing
// the image to the OpenXR runtime, that this image was rendered unfoveated.
void RejectChromiumXrFoveationMetadata(id<MTLTexture> texture, const char *reason)
{
    texture = ChromiumXrRootTexture(texture);
    static bool sLogged = false;
    if (!sLogged)
    {
        NSLog(@"Chromium XR foveation: rejecting IOSurface rate-map metadata (%s); "
              @"rendering unfoveated. Logged once.",
              reason);
        sLogged = true;
    }
    if (texture != nil && texture.iosurface != nullptr)
    {
        IOSurfaceRemoveValue(texture.iosurface, ANGLE_CHROMIUM_XR_FOVEATION_KEY);
    }
}

angle::ObjCPtr<id<MTLRasterizationRateMap>>
BuildChromiumXrFoveationRateMap(id<MTLTexture> renderTexture,
                                NSDictionary *metadata,
                                const char **outReason)
{
    *outReason = "invalid metadata";
    if (renderTexture == nil || metadata == nil)
    {
        return {};
    }

    NSNumber *versionValue = [metadata objectForKey:@"version"];
    NSNumber *widthValue = [metadata objectForKey:@"logical_width"];
    NSNumber *heightValue = [metadata objectForKey:@"logical_height"];
    NSNumber *physicalWidthValue = [metadata objectForKey:@"physical_width"];
    NSNumber *physicalHeightValue = [metadata objectForKey:@"physical_height"];
    NSNumber *zoneCountValue = [metadata objectForKey:@"zone_count"];
    NSArray *horizontalValues = [metadata objectForKey:@"horizontal"];
    NSArray *verticalValues = [metadata objectForKey:@"vertical"];
    NSNumber *serialValue = [metadata objectForKey:@"serial"];

    // Version 2 carries the physical size the OpenXR compositor will assume.
    if (![versionValue isKindOfClass:[NSNumber class]] ||
        ![serialValue isKindOfClass:[NSNumber class]] ||
        [versionValue unsignedIntValue] != 2 ||
        ![widthValue isKindOfClass:[NSNumber class]] ||
        ![heightValue isKindOfClass:[NSNumber class]] ||
        ![physicalWidthValue isKindOfClass:[NSNumber class]] ||
        ![physicalHeightValue isKindOfClass:[NSNumber class]] ||
        ![zoneCountValue isKindOfClass:[NSNumber class]] ||
        ![horizontalValues isKindOfClass:[NSArray class]] ||
        ![verticalValues isKindOfClass:[NSArray class]])
    {
        return {};
    }

    const NSUInteger logicalWidth = [widthValue unsignedIntegerValue];
    const NSUInteger logicalHeight = [heightValue unsignedIntegerValue];
    const NSUInteger expectedPhysicalWidth = [physicalWidthValue unsignedIntegerValue];
    const NSUInteger expectedPhysicalHeight = [physicalHeightValue unsignedIntegerValue];
    const NSUInteger zoneCount = [zoneCountValue unsignedIntegerValue];
    if (logicalWidth == 0 || logicalHeight == 0 || zoneCount == 0 || zoneCount > 64 ||
        expectedPhysicalWidth == 0 || expectedPhysicalHeight == 0 ||
        horizontalValues.count != zoneCount || verticalValues.count != zoneCount ||
        renderTexture.width != logicalWidth || renderTexture.height != logicalHeight)
    {
        *outReason = "metadata does not describe this render target";
        return {};
    }

    id<MTLDevice> device = renderTexture.device;
    if (device == nil || ![device supportsRasterizationRateMapWithLayerCount:1])
    {
        *outReason = "device has no rasterization-rate maps";
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
        *outReason = "rate-map layer creation failed";
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
        *outReason = "rate-map creation failed";
        return {};
    }

    // Chromium keeps the OpenXR/IOSurface texture at logical size: Metal
    // writes the compact physical raster into its leading region and the
    // OpenXR compositor samples it with the runtime's logical->physical map.
    // That is only correct if this reconstruction produces exactly the
    // physical extent the runtime computed; never render with a different map.
    const MTLSize physicalSize = [rateMap.get() physicalSizeForLayer:0];
    if (physicalSize.width != expectedPhysicalWidth ||
        physicalSize.height != expectedPhysicalHeight ||
        physicalSize.width > renderTexture.width || physicalSize.height > renderTexture.height)
    {
        *outReason = "reconstructed physical size differs from the runtime's";
        return {};
    }

    *outReason = nullptr;
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
    // The isolated XR service attaches a resolved Metal rate-map recipe to
    // the IOSurface before Blink/ANGLE renders the frame. Consume it at the
    // last graphics-API boundary, immediately before Metal creates the native
    // render encoder. Ordinary WebGL surfaces have no metadata and remain
    // completely unchanged.
    mCachedRenderPassDescObjC.get().rasterizationRateMap = nil;
    if (mRenderPassDesc.numColorAttachments > 0)
    {
        MTLRenderPassColorAttachmentDescriptor *colorAttachment =
            mCachedRenderPassDescObjC.get().colorAttachments[0];
        id<MTLTexture> renderTexture = colorAttachment.texture;

        // Chromium labels only browser-owned XR foveation targets. This
        // prefix test is intentionally the first gate so ordinary WebGL never
        // calls IOSurfaceCopyValue from the render-pass hot path.
        id<MTLTexture> metadataTexture = nil;
        if (IsChromiumXrFoveatedTexture(renderTexture))
        {
            metadataTexture = renderTexture;
        }
        else if (IsChromiumXrFoveatedTexture(colorAttachment.resolveTexture))
        {
            // The default WebGL framebuffer may render through an internal
            // multisample attachment and resolve into the XR IOSurface.
            metadataTexture = colorAttachment.resolveTexture;
        }

        angle::ObjCPtr<NSDictionary> metadata =
            CopyChromiumXrFoveationMetadata(metadataTexture);
        if (metadata && renderTexture != nil)
        {
            const bool metadataChanged =
                !SameChromiumXrFoveationRecipe(mXrFoveationMetadata.get(), metadata.get());
            if (metadataChanged)
            {
                const char *rejectReason = nullptr;
                angle::ObjCPtr<id<MTLRasterizationRateMap>> rateMap =
                    BuildChromiumXrFoveationRateMap(renderTexture, metadata.get(),
                                                    &rejectReason);
                if (rateMap)
                {
                    mXrFoveationMetadata = metadata;
                    mXrFoveationRateMap = std::move(rateMap);
                }
                else
                {
                    mXrFoveationMetadata.reset();
                    mXrFoveationRateMap.reset();
                    RejectChromiumXrFoveationMetadata(metadataTexture, rejectReason);
                }
            }

            if (mXrFoveationRateMap)
            {
                mCachedRenderPassDescObjC.get().rasterizationRateMap =
                    mXrFoveationRateMap.get();
                AcknowledgeChromiumXrFoveationMetadata(metadataTexture, metadata.get());
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


def _replace_count(text: str, old: str, new: str, count: int, label: str) -> str:
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} source matches, found {found}")
    return text.replace(old, new)


def _pristine_source(angle_root: Path, path: Path) -> str:
    relative = path.relative_to(angle_root).as_posix()
    result = subprocess.run(
        ["git", "-C", str(angle_root), "show", f"HEAD:{relative}"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return result.stdout


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


def apply_patch(check_only: bool) -> bool:
    chromium_root = Path(__file__).resolve().parents[4]
    angle_root = chromium_root / "third_party" / "angle"
    header_path = angle_root / "src/libANGLE/renderer/metal/mtl_command_buffer.h"
    impl_path = angle_root / "src/libANGLE/renderer/metal/mtl_command_buffer.mm"
    texture_header_path = angle_root / "src/libANGLE/renderer/metal/TextureMtl.h"
    texture_impl_path = angle_root / "src/libANGLE/renderer/metal/TextureMtl.mm"

    if (
        not header_path.is_file()
        or not impl_path.is_file()
        or not texture_header_path.is_file()
        or not texture_impl_path.is_file()
    ):
        raise RuntimeError(
            "third_party/angle is unavailable; run gclient sync before generating the macOS OpenXR build"
        )

    header = header_path.read_text()
    impl = impl_path.read_text()

    # An older bridge (no physical-size validation or acknowledgement, and in
    # the earliest variant no privileged-target gate) cannot be upgraded in
    # place reliably. Rebuild the two render-pass files from pristine ANGLE
    # sources instead; the bridge is the only intended change to them.
    if MARKER in impl and ACK_MARKER not in impl:
        header = _pristine_source(angle_root, header_path)
        impl = _pristine_source(angle_root, impl_path)
        if not check_only:
            print(
                "Chromium XR ANGLE foveation patch: replacing an older bridge",
                file=sys.stderr,
            )
    texture_header = texture_header_path.read_text()
    texture_impl = texture_impl_path.read_text()

    revision = _angle_revision(angle_root)
    if revision is not None and revision != EXPECTED_ANGLE_REVISION:
        message = (
            "ANGLE revision changed: expected "
            f"{EXPECTED_ANGLE_REVISION}, found {revision}. "
            "Rebase the XR Metal foveation bridge."
        )
        if check_only:
            raise RuntimeError(message)
        print(f"Chromium XR ANGLE foveation patch skipped: {message}", file=sys.stderr)
        return False

    # Apply each piece independently. This keeps the hook idempotent even when
    # the ANGLE checkout already contains one of the required includes or a
    # previous local experiment applied only part of the bridge.
    new_texture_header = texture_header
    if "onLabelUpdate(const gl::Context *context) override;" not in new_texture_header:
        new_texture_header = _replace_once(
            new_texture_header,
            TEXTURE_HEADER_OLD,
            TEXTURE_HEADER_NEW,
            "TextureMtl.h label hook",
        )

    new_texture_impl = texture_impl
    if "ANGLE_TRY(onLabelUpdate(context));" not in new_texture_impl:
        # Both setEGLImageTarget() and bindTexImage() create the native
        # storage Chromium's XR SharedImages use; label both.
        new_texture_impl = _replace_count(
            new_texture_impl,
            TEXTURE_BIND_OLD,
            TEXTURE_BIND_NEW,
            2,
            "TextureMtl.mm bind label propagation",
        )
    if "TextureMtl::onLabelUpdate" not in new_texture_impl:
        new_texture_impl = _replace_once(
            new_texture_impl,
            TEXTURE_RELEASE_OLD,
            TEXTURE_RELEASE_NEW,
            "TextureMtl.mm label hook",
        )

    new_header = header
    if "mXrFoveationRateMap" not in new_header:
        new_header = _replace_once(
            new_header, HEADER_OLD, HEADER_NEW, "mtl_command_buffer.h"
        )

    new_impl = impl
    if "#include <cmath>" not in new_impl or "#include <vector>" not in new_impl:
        new_impl = _replace_once(new_impl, INCLUDES_OLD, INCLUDES_NEW, "C++ includes")

    if "#    import <IOSurface/IOSurface.h>" not in new_impl:
        include_anchor = '#include "libANGLE/renderer/metal/mtl_utils.h"\n'
        iosurface_block = (
            "\n#if TARGET_OS_OSX\n"
            "#    import <IOSurface/IOSurface.h>\n"
            "#endif\n"
        )
        new_impl = _replace_once(
            new_impl,
            include_anchor,
            include_anchor + iosurface_block,
            "IOSurface include anchor",
        )

    if "AcknowledgeChromiumXrFoveationMetadata(id<MTLTexture>" not in new_impl:
        new_impl = _replace_once(
            new_impl, NAMESPACE_OLD, NAMESPACE_NEW, "Metal helpers"
        )

    if "AcknowledgeChromiumXrFoveationMetadata(metadataTexture" not in new_impl:
        new_impl = _replace_once(
            new_impl, RESTART_OLD, RESTART_NEW, "render-pass hook"
        )

    if check_only:
        return True

    # Validate every replacement before touching any file, then write all four.
    texture_header_path.write_text(new_texture_header)
    texture_impl_path.write_text(new_texture_impl)
    header_path.write_text(new_header)
    impl_path.write_text(new_impl)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify that the pinned ANGLE source is patchable without modifying it",
    )
    args = parser.parse_args()

    try:
        applied = apply_patch(args.check)
    except Exception as exc:
        print(f"Chromium XR ANGLE foveation patch failed: {exc}", file=sys.stderr)
        return 1

    print("ok" if applied else "skipped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
