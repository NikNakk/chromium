// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/xr/xr_media_controls_drawing_context.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "gpu/command_buffer/client/raster_interface.h"
#include "third_party/blink/renderer/modules/xr/xr_composition_layer.h"
#include "third_party/blink/renderer/modules/xr/xr_session.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#include "third_party/blink/renderer/platform/graphics/gpu/xr_raster_frame_transport_delegate.h"
#include "third_party/blink/renderer/platform/graphics/gpu/xr_webgl_drawing_buffer.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRect.h"

namespace blink {

namespace {

constexpr int kControlCount = 5;
constexpr float kOuterMargin = 18.0f;
constexpr float kButtonGap = 12.0f;
constexpr float kPanelRadius = 28.0f;
constexpr float kButtonRadius = 20.0f;

SkRect ButtonRect(int index, float width, float height) {
  const float usable_width =
      width - 2.0f * kOuterMargin - (kControlCount - 1) * kButtonGap;
  const float button_width = usable_width / kControlCount;
  const float left =
      kOuterMargin + index * (button_width + kButtonGap);
  return SkRect::MakeLTRB(left, kOuterMargin, left + button_width,
                          height - kOuterMargin);
}

void DrawChevron(SkCanvas* canvas,
                 float center_x,
                 float center_y,
                 float size,
                 bool points_right,
                 SkPaint* paint) {
  const float sign = points_right ? 1.0f : -1.0f;
  SkPathBuilder path;
  path.moveTo(center_x - sign * size * 0.45f, center_y - size * 0.65f);
  path.lineTo(center_x + sign * size * 0.45f, center_y);
  path.lineTo(center_x - sign * size * 0.45f, center_y + size * 0.65f);
  path.close();
  canvas->drawPath(path.detach(), *paint);
}

void DrawSkip(SkCanvas* canvas,
              const SkRect& rect,
              bool points_right,
              SkPaint* paint) {
  const float cy = rect.centerY();
  const float size = std::min(rect.width(), rect.height()) * 0.24f;
  const float offset = size * 0.55f;
  DrawChevron(canvas, rect.centerX() - offset, cy, size, points_right, paint);
  DrawChevron(canvas, rect.centerX() + offset, cy, size, points_right, paint);
}

void DrawPlayPause(SkCanvas* canvas,
                   const SkRect& rect,
                   bool paused,
                   SkPaint* paint) {
  const float size = std::min(rect.width(), rect.height()) * 0.30f;
  if (paused) {
    SkPathBuilder path;
    path.moveTo(rect.centerX() - size * 0.55f, rect.centerY() - size);
    path.lineTo(rect.centerX() + size, rect.centerY());
    path.lineTo(rect.centerX() - size * 0.55f, rect.centerY() + size);
    path.close();
    canvas->drawPath(path.detach(), *paint);
    return;
  }

  const float bar_width = size * 0.48f;
  const float gap = size * 0.38f;
  canvas->drawRoundRect(
      SkRect::MakeLTRB(rect.centerX() - gap - bar_width,
                       rect.centerY() - size,
                       rect.centerX() - gap,
                       rect.centerY() + size),
      bar_width * 0.25f, bar_width * 0.25f, *paint);
  canvas->drawRoundRect(
      SkRect::MakeLTRB(rect.centerX() + gap,
                       rect.centerY() - size,
                       rect.centerX() + gap + bar_width,
                       rect.centerY() + size),
      bar_width * 0.25f, bar_width * 0.25f, *paint);
}

void DrawMute(SkCanvas* canvas,
              const SkRect& rect,
              bool muted,
              SkPaint* paint) {
  const float size = std::min(rect.width(), rect.height()) * 0.28f;
  const float cx = rect.centerX();
  const float cy = rect.centerY();

  SkPathBuilder speaker;
  speaker.moveTo(cx - size, cy - size * 0.35f);
  speaker.lineTo(cx - size * 0.45f, cy - size * 0.35f);
  speaker.lineTo(cx + size * 0.15f, cy - size);
  speaker.lineTo(cx + size * 0.15f, cy + size);
  speaker.lineTo(cx - size * 0.45f, cy + size * 0.35f);
  speaker.lineTo(cx - size, cy + size * 0.35f);
  speaker.close();
  canvas->drawPath(speaker.detach(), *paint);

  if (muted) {
    paint->setStyle(SkPaint::kStroke_Style);
    paint->setStrokeWidth(std::max(6.0f, size * 0.14f));
    paint->setStrokeCap(SkPaint::kRound_Cap);
    canvas->drawLine(cx + size * 0.45f, cy - size * 0.55f,
                     cx + size * 1.25f, cy + size * 0.55f, *paint);
    canvas->drawLine(cx + size * 1.25f, cy - size * 0.55f,
                     cx + size * 0.45f, cy + size * 0.55f, *paint);
    paint->setStyle(SkPaint::kFill_Style);
    return;
  }

  paint->setStyle(SkPaint::kStroke_Style);
  paint->setStrokeWidth(std::max(5.0f, size * 0.10f));
  paint->setStrokeCap(SkPaint::kRound_Cap);
  const SkRect wave =
      SkRect::MakeLTRB(cx - size * 0.35f, cy - size,
                       cx + size * 1.45f, cy + size);
  canvas->drawArc(wave, -48.0f, 96.0f, false, *paint);
  paint->setStyle(SkPaint::kFill_Style);
}

void DrawExit(SkCanvas* canvas, const SkRect& rect, SkPaint* paint) {
  const float size = std::min(rect.width(), rect.height()) * 0.27f;
  paint->setStyle(SkPaint::kStroke_Style);
  paint->setStrokeWidth(std::max(8.0f, size * 0.16f));
  paint->setStrokeCap(SkPaint::kRound_Cap);
  canvas->drawLine(rect.centerX() - size, rect.centerY() - size,
                   rect.centerX() + size, rect.centerY() + size, *paint);
  canvas->drawLine(rect.centerX() + size, rect.centerY() - size,
                   rect.centerX() - size, rect.centerY() + size, *paint);
  paint->setStyle(SkPaint::kFill_Style);
}

}  // namespace

XRMediaControlsDrawingContext::XRMediaControlsDrawingContext(XRSession* session)
    : session_(session) {
  frame_transport_delegate_ =
      MakeGarbageCollected<XRRasterFrameTransportDelegate>();
}

XRMediaControlsDrawingContext::~XRMediaControlsDrawingContext() = default;

int XRMediaControlsDrawingContext::HitTest(float normalized_x,
                                           float normalized_y) {
  if (normalized_x < 0.0f || normalized_x > 1.0f ||
      normalized_y < 0.0f || normalized_y > 1.0f) {
    return -1;
  }

  const float x = normalized_x * 1024.0f;
  const float y = normalized_y * 192.0f;
  for (int i = 0; i < kControlCount; ++i) {
    if (ButtonRect(i, 1024.0f, 192.0f).contains(x, y)) {
      return i;
    }
  }
  return -1;
}

void XRMediaControlsDrawingContext::SetState(bool paused,
                                             bool muted,
                                             int hovered_control) {
  hovered_control = std::clamp(hovered_control, -1, kControlCount - 1);
  if (paused_ == paused && muted_ == muted &&
      hovered_control_ == hovered_control) {
    return;
  }

  paused_ = paused;
  muted_ = muted;
  hovered_control_ = hovered_control;
  dirty_ = true;
}

void XRMediaControlsDrawingContext::OnFrameStart() {
  content_changed_ = false;

  if (!dirty_ || !layer_ || !layer_->HasSharedImage()) {
    return;
  }

  const auto& dest_shared_image = layer_->SharedImage();
  if (!dest_shared_image.shared_image) {
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

  SkBitmap bitmap;
  if (!bitmap.tryAllocN32Pixels(width_, height_)) {
    return;
  }
  bitmap.eraseColor(SK_ColorTRANSPARENT);

  SkCanvas canvas(bitmap);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kFill_Style);

  paint.setColor(SkColorSetARGB(220, 18, 18, 20));
  canvas.drawRoundRect(SkRect::MakeWH(width_, height_), kPanelRadius,
                       kPanelRadius, paint);

  for (int i = 0; i < kControlCount; ++i) {
    const SkRect rect = ButtonRect(i, width_, height_);
    paint.setColor(i == hovered_control_
                       ? SkColorSetARGB(245, 92, 92, 100)
                       : SkColorSetARGB(215, 45, 45, 50));
    canvas.drawRoundRect(rect, kButtonRadius, kButtonRadius, paint);

    paint.setColor(SK_ColorWHITE);
    switch (i) {
      case 0:
        DrawSkip(&canvas, rect, /*points_right=*/false, &paint);
        break;
      case 1:
        DrawPlayPause(&canvas, rect, paused_, &paint);
        break;
      case 2:
        DrawSkip(&canvas, rect, /*points_right=*/true, &paint);
        break;
      case 3:
        DrawMute(&canvas, rect, muted_, &paint);
        break;
      case 4:
        DrawExit(&canvas, rect, &paint);
        break;
    }
  }

  SkPixmap pixmap;
  if (!bitmap.peekPixels(&pixmap)) {
    return;
  }

  sync_token_ = raster_interface->WritePixels(
      dest_shared_image.shared_image, dest_shared_image.sync_token,
      /*dst_x_offset=*/0, /*dst_y_offset=*/0, pixmap);
  raster_interface->Flush();

  dirty_ = false;
  content_changed_ = true;
}

void XRMediaControlsDrawingContext::OnFrameEnd() {}

void XRMediaControlsDrawingContext::SetCompositionLayer(
    XRCompositionLayer* layer) {
  layer_ = layer;
}

std::unique_ptr<SharedImageHolder>
XRMediaControlsDrawingContext::TransferToSharedImageHolder() {
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
XRMediaControlsDrawingContext::DoneWithSharedBuffer() {
  return TransferToSharedImageHolder();
}

void XRMediaControlsDrawingContext::Trace(Visitor* visitor) const {
  visitor->Trace(session_);
  visitor->Trace(layer_);
  visitor->Trace(frame_transport_delegate_);
  XRLayerDrawingContext::Trace(visitor);
}

}  // namespace blink
