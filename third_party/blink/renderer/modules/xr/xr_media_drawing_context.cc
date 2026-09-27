// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/xr/xr_media_drawing_context.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"\n#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/client/raster_interface.h"\n#include "media/base/video_frame.h"
#include "media/renderers/paint_canvas_video_renderer.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/modules/xr/xr_composition_layer.h"
#include "third_party/blink/renderer/modules/xr/xr_session.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#include "third_party/blink/renderer/platform/graphics/gpu/xr_raster_frame_transport_delegate.h"
#include "third_party/blink/renderer/platform/graphics/gpu/xr_webgl_drawing_buffer.h"
#include "third_party/blink/renderer/platform/graphics/video_frame_image_util.h"

namespace media {
class VideoFrame;
class VideoFrameSharedImageCache;
}  // namespace media

namespace blink {

XRMediaDrawingContext::XRMediaDrawingContext(
    XRSession* session,
    HTMLVideoElement* video,
    bool needs_eac_reprojection,
    std::vector<uint8_t> media_projection_data,
    base::RepeatingCallback<void(gfx::Size)> size_changed_callback)
    : session_(session),
      video_(video),
      size_changed_callback_(std::move(size_changed_callback)),
      needs_eac_reprojection_(needs_eac_reprojection),
      media_projection_data_(
          base::span<const uint8_t>(media_projection_data)) {
  frame_transport_delegate_ =
      MakeGarbageCollected<XRRasterFrameTransportDelegate>();

  if (auto wrapper = SharedGpuContext::ContextProviderWrapper()) {
    int max_size =
        wrapper->ContextProvider().GetCapabilities().max_texture_size;
    if (max_size > 0) {
      max_texture_size_ = base::checked_cast<uint16_t>(max_size);
    }
  }

  if (video_) {
    source_video_size_ =
        gfx::Size(video_->videoWidth(), video_->videoHeight());

    uint16_t width = video_->videoWidth();
    uint16_t height = video_->videoHeight();
    if (width > max_texture_size_ || height > max_texture_size_) {
      double scale = std::min(static_cast<double>(max_texture_size_) / width,
                              static_cast<double>(max_texture_size_) / height);
      // Scale down while clamping to a minimum of 1 pixel to avoid
      // generating invalid 0-sized textures in extreme cases such as when the
      // width and height of the video are very different.
      width_ =
          std::max<uint16_t>(1, base::checked_cast<uint16_t>(width * scale));
      height_ =
          std::max<uint16_t>(1, base::checked_cast<uint16_t>(height * scale));
    } else {
      width_ = width;
      height_ = height;
    }
  }
}

XRMediaDrawingContext::~XRMediaDrawingContext() = default;

void XRMediaDrawingContext::OnFrameStart() {
  content_changed_ = false;

  if (!layer_ || !video_) {
    return;
  }

  const gfx::Size current_video_size(video_->videoWidth(), video_->videoHeight());
  const int64_t current_pixels =
      static_cast<int64_t>(current_video_size.width()) *
      current_video_size.height();
  const int64_t layer_source_pixels =
      static_cast<int64_t>(source_video_size_.width()) *
      source_video_size_.height();
  if (!current_video_size.IsEmpty() &&
      current_pixels > layer_source_pixels) {
    DVLOG(1) << "XR media decoded resolution increased from "
             << source_video_size_.ToString() << " to "
             << current_video_size.ToString();
    if (size_changed_callback_) {
      size_changed_callback_.Run(current_video_size);
    }

    // The resize callback rebuilds the internal XR layer on a posted task.
    // Do not copy this newly larger decoded frame into the old layer's
    // smaller SharedImage in the meantime: RasterInterface::CopySharedImage
    // would otherwise see mismatched destination dimensions. The next frame
    // will use the rebuilt layer.
    return;
  }

  auto wrapper = SharedGpuContext::ContextProviderWrapper();
  if (!wrapper) {
    return;
  }

  gpu::raster::RasterInterface* raster_interface =
      wrapper->ContextProvider().RasterInterface();
  if (!raster_interface) {
    return;
  }

  if (!layer_->HasSharedImage()) {
    return;
  }

  const auto& dest_shared_image = layer_->SharedImage();
  if (!dest_shared_image.shared_image) {
    return;
  }

  auto* media_player = video_->GetWebMediaPlayer();
  if (!media_player) {
    return;
  }

  scoped_refptr<media::VideoFrame> media_video_frame =
      media_player->GetCurrentFrameThenUpdate();
  if (!media_video_frame) {
    return;
  }

  const uint64_t media_frame_id =
      media_video_frame->unique_id().GetUnsafeValue();
  if (last_media_frame_id_ == media_frame_id) {
    // The video compositor commonly exposes the same decoded frame over
    // multiple XR ticks (e.g. a 30 fps video in a 120 Hz session). Keep
    // submitting the layer so OpenXR receives a frame, but leave the already
    // populated SharedImage alone rather than scheduling another full-size
    // video copy.
    TRACE_EVENT_INSTANT("xr", "XRMediaFrameReused", "frame_id",
                        media_frame_id);
    content_changed_ = true;
    return;
  }

  bool need_scaling =
      width_ != video_->videoWidth() || height_ != video_->videoHeight();

  bool use_copy_to_shared_image =
      media::PaintCanvasVideoRenderer::CanUseCopyVideoFrameToSharedImage(
          *media_video_frame);

  if (need_scaling || !use_copy_to_shared_image) {
    // This is a 2-copy GPU-accelerated fallback because
    // CopyVideoFrameToSharedImage does not support scaling or
    // format/color-space conversion directly.
    //
    // Copy 1 (with scaling): Draw the VideoFrame onto an intermediate, scaled
    // SharedImage (RGBA) via HTMLVideoElement::CreateStaticBitmapImage().
    //
    // Copy 2: Copy from the intermediate SharedImage to the composition layer's
    // destination SharedImage via
    // gpu::raster::RasterInterface::CopySharedImage().
    //
    // The intermediate SharedImage requires RASTER_READ, and the destination
    // SharedImage requires RASTER_WRITE (checked during BeginRasterAccess).
    // A true 1-copy solution would require refactoring the composition layer
    // swapchains to allow wrapping the destination SharedImage in a blink
    // resource provider and painting/scaling the VideoFrame directly onto it.
    scoped_refptr<StaticBitmapImage> image =
        video_->CreateStaticBitmapImage(gfx::Size(width_, height_));
    if (image && image->IsTextureBacked()) {
      auto src_shared_image = image->GetSharedImage();
      if (src_shared_image) {
        std::unique_ptr<gpu::RasterScopedAccess> src_raster_access =
            src_shared_image->BeginRasterAccess(raster_interface,
                                                image->GetSyncToken(),
                                                /*readonly=*/true);
        std::unique_ptr<gpu::RasterScopedAccess> dst_raster_access =
            dest_shared_image.shared_image->BeginRasterAccess(
                raster_interface, dest_shared_image.sync_token,
                /*readonly=*/false);

        raster_interface->CopySharedImage(
            src_shared_image->mailbox(),
            dest_shared_image.shared_image->mailbox(), 0, 0, 0, 0, width_,
            height_);

        sync_token_ =
            gpu::RasterScopedAccess::EndAccess(std::move(dst_raster_access));
        gpu::RasterScopedAccess::EndAccess(std::move(src_raster_access));
      } else {
        LOG(ERROR) << "OnFrameStart: failed to get shared image from "
                      "StaticBitmapImage";
        return;
      }
    } else {
      LOG(ERROR)
          << "OnFrameStart: failed to create accelerated StaticBitmapImage";
      return;
    }
  } else {
    sync_token_ = media::PaintCanvasVideoRenderer::CopyVideoFrameToSharedImage(
        wrapper->ContextProvider().RasterContextProvider(),
        std::move(media_video_frame), dest_shared_image.shared_image,
        dest_shared_image.sync_token, /*use_visible_rect=*/true,
        media_player->GetYUVSharedImageCache());
  }

  last_media_frame_id_ = media_frame_id;
  TRACE_EVENT_INSTANT("xr", "XRMediaFrameCopied", "frame_id", media_frame_id);
  content_changed_ = true;

  // Flush the commands to ensure the GPU executes the copy before OpenXR
  // consumes it.
  raster_interface->Flush();
}

void XRMediaDrawingContext::OnFrameEnd() {}

void XRMediaDrawingContext::SetCompositionLayer(XRCompositionLayer* layer) {
  layer_ = layer;
}

uint16_t XRMediaDrawingContext::TextureWidth() const {
  return width_;
}

uint16_t XRMediaDrawingContext::TextureHeight() const {
  return height_;
}

std::unique_ptr<SharedImageHolder>
XRMediaDrawingContext::TransferToSharedImageHolder() {
  if (!layer_ || !layer_->HasSharedImage()) {
    return nullptr;
  }
  const auto& dest_shared_image = layer_->SharedImage();
  if (!dest_shared_image.shared_image) {
    return nullptr;
  }
  return std::make_unique<SharedImageHolder>(
      dest_shared_image.shared_image, sync_token_,
      blink::BindOnce([](const gpu::SyncToken&, bool) {}));
}

std::unique_ptr<SharedImageHolder>
XRMediaDrawingContext::DoneWithSharedBuffer() {
  return TransferToSharedImageHolder();
}

void XRMediaDrawingContext::Trace(Visitor* visitor) const {
  visitor->Trace(session_);
  visitor->Trace(video_);
  visitor->Trace(layer_);
  visitor->Trace(frame_transport_delegate_);
  XRLayerDrawingContext::Trace(visitor);
}

}  // namespace blink
