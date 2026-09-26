// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/openxr/openxr_graphics_binding.h"

#include "build/build_config.h"
#include "components/viz/common/gpu/context_provider.h"
#include "device/vr/openxr/openxr_api_wrapper.h"
#include "device/vr/openxr/openxr_extension_helper.h"
#include "device/vr/openxr/openxr_util.h"
#include "device/vr/openxr/openxr_view_configuration.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/openxr/src/include/openxr/openxr.h"
#include "ui/gl/gl_bindings.h"

namespace device {

// static
std::vector<std::string> OpenXrGraphicsBinding::GetOptionalExtensions() {
  return {XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME};
}

OpenXrGraphicsBinding::OpenXrGraphicsBinding(
    const OpenXrExtensionEnumeration* extension_enum)
    : fb_composition_layer_ext_enabled_(extension_enum->ExtensionSupported(
          XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME)) {
  if (fb_composition_layer_ext_enabled_) {
    // This struct is chained through XrCompositionLayerProjection::next.
    // Explicitly zero it so the extension chain terminates at next=nullptr.
    // Some runtimes ignore an unknown tail pointer, but Meta XR Simulator
    // deep-copies the full chain and will dereference an uninitialized next.
    y_flip_layer_layout_ = {};
    y_flip_layer_layout_.type = XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB;
    y_flip_layer_layout_.flags =
        XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  }
}

OpenXrGraphicsBinding::~OpenXrGraphicsBinding() {
  DCHECK(!base_layer_);
  OnSessionDestroyed(nullptr);
}

bool OpenXrGraphicsBinding::ShouldRenderBaseLayer() const {
  return layers_sequence_.empty() || overlay_visible_;
}

bool OpenXrGraphicsBinding::RequiresSharedImages() const {
  return false;
}

void OpenXrGraphicsBinding::OnSessionCreated(XrSpace local_space,
                                             bool is_webgpu) {
  webgpu_session_ = is_webgpu;

  // Not all values will be used for the base layer. The swapchain image size
  // will set by SetProjectionLayerSwapchainImageSize(). But to be safe, we
  // still provide XRCompositionLayerData to the base layer.
  mojom::XRCompositionLayerDataPtr layer_data =
      mojom::XRCompositionLayerData::New();
  auto projection_layer_data = mojom::XRProjectionLayerData::New();
  layer_data->read_only_data = mojom::XRLayerReadOnlyData::New();
  // The base layer should have an invalid layer id.
  layer_data->read_only_data->layer_id = kInvalidLayerId;
  layer_data->read_only_data->layout =
      kNumPrimaryViews > 1 ? mojom::XRLayerLayout::kStereoLeftRight
                           : mojom::XRLayerLayout::kMono;
  layer_data->read_only_data->texture_width = 0;
  layer_data->read_only_data->texture_height = 0;
  layer_data->mutable_data = mojom::XRLayerMutableData::New();
  layer_data->mutable_data->layer_data =
      mojom::XRLayerSpecificData::NewProjection(
          std::move(projection_layer_data));
  layer_data->mutable_data->blend_texture_source_alpha = true;
  layer_data->mutable_data->force_mono_presentation = false;
  layer_data->mutable_data->opacity = 1.f;
  layer_data->mutable_data->native_origin_information =
      mojom::XRNativeOriginInformation::NewReferenceSpaceType(
          mojom::XRReferenceSpaceType::kLocal);

  base_layer_ = std::make_unique<OpenXrCompositionLayer>(
      std::move(layer_data), this, CreateLayerGraphicsBindingData());
}

void OpenXrGraphicsBinding::OnSessionDestroyed(gpu::SharedImageInterface* sii) {
  last_rendered_base_projection_views_.clear();
  last_rendered_layer_projection_views_.clear();
  if (base_layer_) {
    base_layer_->DestroySwapchain(sii);
    base_layer_.reset();
  }
  for (auto& [_, layer] : layers_) {
    layer->DestroySwapchain(sii);
  }
  layers_.clear();
}

bool OpenXrGraphicsBinding::CanSubmitBaseLayer() const {
  return base_layer_ &&
         (base_layer_->is_rendered() ||
          base_layer_->has_last_released_swapchain_image());
}

std::vector<XrCompositionLayerProjectionView>
OpenXrGraphicsBinding::GetBaseLayerProjectionViews(
    const OpenXrViewConfiguration& view_config) const {
  CHECK(base_layer_);

  std::vector<XrCompositionLayerProjectionView> projection_views =
      GetProjectionViews(view_config, *base_layer_);

  // Keep the projection metadata paired with the pixels that are actually
  // submitted to the runtime. When WebXR intentionally skips a render, the
  // base layer keeps presenting the previously released swapchain image; using
  // the new frame's view pose for those old pixels would make runtime
  // reprojection believe the stale image was rendered from the current head
  // pose.
  if (base_layer_->is_rendered()) {
    last_rendered_base_projection_views_[view_config.Type()] =
        projection_views;
    return projection_views;
  }

  auto cached =
      last_rendered_base_projection_views_.find(view_config.Type());
  if (cached != last_rendered_base_projection_views_.end()) {
    return cached->second;
  }

  // Before the first rendered base-layer frame there is nothing valid to reuse.
  // Preserve the existing behaviour until a rendered image establishes a cache.
  return projection_views;
}

std::vector<XrCompositionLayerProjectionView>
OpenXrGraphicsBinding::GetProjectionViews(
    const OpenXrViewConfiguration& view_config,
    OpenXrCompositionLayer& layer) const {
  DCHECK(view_config.Active());
  DCHECK(layer.type() == OpenXrCompositionLayer::Type::kProjection);

  // The following code only works for "mono" and "stereo-left-right".
  CHECK(layer.read_only_data().layout ==
            mojom::XRLayerLayout::kStereoLeftRight ||
        layer.read_only_data().layout == mojom::XRLayerLayout::kMono);

  std::vector<XrCompositionLayerProjectionView> projection_views;
  projection_views.resize(view_config.Views().size());

#if BUILDFLAG(IS_MAC)
  // The macOS direct-Metal path exposes custom projection-layer swapchain
  // textures directly to Blink. Unlike the base layer, a WebXR projection
  // layer may request a scaled texture, so its OpenXR imageRect must match the
  // layer's own texture dimensions rather than the headset-recommended base
  // view dimensions.
  const bool use_layer_viewports =
      RequiresSharedImages() && layer.GetLayerId() != kInvalidLayerId;
#else
  constexpr bool use_layer_viewports = false;
#endif

  uint32_t x_offset = view_config.Viewport().x();
  for (uint32_t view_index = 0; view_index < view_config.Views().size();
       view_index++) {
    const XrView& view = view_config.Views()[view_index];

    // Zero-initialize everything.
    XrCompositionLayerProjectionView projection_view{};

    const OpenXrViewProperties& properties =
        view_config.Properties()[view_index];
    projection_view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    projection_view.pose = view.pose;
    projection_view.fov.angleLeft = view.fov.angleLeft;
    projection_view.fov.angleRight = view.fov.angleRight;
    projection_view.subImage.swapchain = layer.color_swapchain();
    // Since we're in double wide mode, the texture array only has one texture
    // and is always index 0. If secondary views are enabled, those views are
    // also in this same texture array.
    projection_view.subImage.imageArrayIndex = 0;

    if (use_layer_viewports) {
      const XrEyeVisibility eye =
          view_index == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
      const gfx::Rect viewport = layer.GetSubImageViewport(eye);
      projection_view.subImage.imageRect.extent.width = viewport.width();
      projection_view.subImage.imageRect.extent.height = viewport.height();
      projection_view.subImage.imageRect.offset.x = viewport.x();
      projection_view.subImage.imageRect.offset.y =
          layer.GetSwapchainImageSize().height() -
          (viewport.y() + viewport.height());
    } else {
      projection_view.subImage.imageRect.extent.width = properties.Width();
      projection_view.subImage.imageRect.extent.height = properties.Height();
      projection_view.subImage.imageRect.offset.x = x_offset;
      x_offset += properties.Width();
      projection_view.subImage.imageRect.offset.y =
          layer.GetSwapchainImageSize().height() - properties.Height();
    }

    projection_view.fov.angleUp = view.fov.angleUp;
    projection_view.fov.angleDown = view.fov.angleDown;

    // WebGL layers may give us flipped content. We need to instruct OpenXR
    // to flip the content before showing it to the user. Some XR runtimes
    // are able to efficiently do this as part of existing post processing
    // steps. However, if we have the composition layer extension enabled, we
    // will instruct the runtime to invert the image in a different manner.
    if (ShouldFlipSubmittedImage(layer) && !fb_composition_layer_ext_enabled_) {
      projection_view.subImage.imageRect.offset.y = 0;
      projection_view.fov.angleUp = -view.fov.angleUp;
      projection_view.fov.angleDown = -view.fov.angleDown;
    }

    projection_views[view_index] = projection_view;
  }

  return projection_views;
}

const void* OpenXrGraphicsBinding::GetFlipLayerLayout() const {
  if (!base_layer_) {
    return nullptr;
  }
  return GetFlipLayerLayout(*base_layer_);
}

const void* OpenXrGraphicsBinding::GetFlipLayerLayout(
    OpenXrCompositionLayer& layer) const {
  // If we don't need to flip the image, then we have nothing to do here.
  // If we do need to flip the image and `fb_composition_layer_ext_enabled_`
  // is false, we have already flipped the image during
  // `GetProjectionViews`.
  if (!ShouldFlipSubmittedImage(layer) || !fb_composition_layer_ext_enabled_) {
    return nullptr;
  }
  return &y_flip_layer_layout_;
}

bool OpenXrGraphicsBinding::IsUsingSharedImages() const {
  return base_layer_ && base_layer_->IsUsingSharedImages();
}

void OpenXrGraphicsBinding::OnContextProviderLost() {
  // Mark the shared mailboxes as invalid since the underlying GPU process
  // associated with them has gone down.
  if (base_layer_) {
    for (OpenXrSwapchainInfo& info : base_layer_->GetSwapchainImages()) {
      info.Clear();
    }
  }
  for (const auto& [_, layer] : layers_) {
    for (OpenXrSwapchainInfo& info : layer->GetSwapchainImages()) {
      info.Clear();
    }
  }
}

void OpenXrGraphicsBinding::SetOverlayAndWebXrVisibility(bool overlay_visible,
                                                         bool webxr_visible) {
  overlay_visible_ = overlay_visible;
  webxr_visible_ = webxr_visible;
  OnSetOverlayAndWebXrVisibility();
}

std::unique_ptr<OpenXrLayers> OpenXrGraphicsBinding::GetLayersForViewConfig(
    OpenXrApiWrapper* openxr,
    const OpenXrViewConfiguration& view_config) const {
  auto openxr_layers = std::make_unique<OpenXrLayers>();
  for (const auto& layer_id : layers_sequence_) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it == layers_.end()) {
      continue;
    }
    OpenXrCompositionLayer& layer = *layer_it->second;

    // A swapchain can only be referenced by xrEndFrame after at least one
    // image has been released to the runtime. During a newly enabled layer's
    // first sparse frame there may be nothing valid to present yet.
    if (!layer.is_rendered() && !layer.has_last_released_swapchain_image()) {
      continue;
    }

    if (layer.type() == OpenXrCompositionLayer::Type::kProjection) {
      auto projection_views = GetProjectionViews(view_config, layer);
      if (layer.is_rendered()) {
        last_rendered_layer_projection_views_[layer_id][view_config.Type()] =
            projection_views;
      } else {
        auto layer_cache =
            last_rendered_layer_projection_views_.find(layer_id);
        if (layer_cache != last_rendered_layer_projection_views_.end()) {
          auto view_cache = layer_cache->second.find(view_config.Type());
          if (view_cache != layer_cache->second.end()) {
            projection_views = view_cache->second;
          }
        }
      }

      openxr_layers->AddCompositionLayer(openxr, layer,
                                         std::move(projection_views),
                                         GetFlipLayerLayout(layer));
    } else {
      openxr_layers->AddCompositionLayer(openxr, layer, {},
                                         GetFlipLayerLayout(layer));
    }
  }
  if (ShouldRenderBaseLayer() && CanSubmitBaseLayer()) {
    openxr_layers->AddBaseLayer(
        openxr->GetReferenceSpace(mojom::XRReferenceSpaceType::kLocal),
        GetBaseLayerProjectionViews(view_config), GetFlipLayerLayout());
  }
  return openxr_layers;
}

XrResult OpenXrGraphicsBinding::CreateBaseLayerSwapchain(
    XrSession session,
    uint32_t sample_count) {
  CHECK(base_layer_);
  return base_layer_->CreateSwapchain(session, sample_count);
}

void OpenXrGraphicsBinding::DestroyBaseLayerSwapchain(
    gpu::SharedImageInterface* sii) {
  CHECK(base_layer_);
  last_rendered_base_projection_views_.clear();
  base_layer_->DestroySwapchain(sii);
}

void OpenXrGraphicsBinding::CreateBaseLayerSharedImages(
    gpu::SharedImageInterface* sii) {
  CHECK(base_layer_);
  CreateSharedImages(*base_layer_, sii);
}

gfx::Size OpenXrGraphicsBinding::GetProjectionLayerSwapchainImageSize() {
  return base_layer_->GetSwapchainImageSize();
}

void OpenXrGraphicsBinding::SetProjectionLayerSwapchainImageSize(
    const gfx::Size& swapchain_image_size) {
  base_layer_->SetSwapchainImageSize(swapchain_image_size);
#if BUILDFLAG(IS_MAC)
  // Direct Metal projection layers use their WebXR-requested texture size so
  // Blink can render straight into the runtime texture without an intermediate
  // resize/copy. Do not overwrite those custom sizes with the base-layer size.
  if (RequiresSharedImages()) {
    return;
  }
#endif
  // Other bindings use the base projection size and may resize/copy their
  // transfer buffers independently.
  for (auto& [_, layer] : layers_) {
    if (layer->type() == OpenXrCompositionLayer::Type::kProjection) {
      layer->SetSwapchainImageSize(swapchain_image_size);
    }
  }
}

bool OpenXrGraphicsBinding::HasBaseLayerColorSwapchain() const {
  return base_layer_ && base_layer_->HasColorSwapchain() &&
         base_layer_->GetSwapchainImages().size() > 0;
}

void OpenXrGraphicsBinding::SetProjectionLayerTransferSize(
    const gfx::Size& transfer_size) {
  CHECK(base_layer_);
  base_layer_->SetTransferSize(transfer_size);
#if BUILDFLAG(IS_MAC)
  // Custom direct-Metal projection layers own their transfer dimensions.
  if (RequiresSharedImages()) {
    return;
  }
#endif
  for (auto& [_, layer] : layers_) {
    if (layer->type() == OpenXrCompositionLayer::Type::kProjection) {
      layer->SetTransferSize(transfer_size);
    }
  }
}

bool OpenXrGraphicsBinding::WaitOnBaseLayerFence(gfx::GpuFence& gpu_fence) {
  CHECK(base_layer_);
  return WaitOnFence(*base_layer_, gpu_fence);
}

void OpenXrGraphicsBinding::UpdateProjectionLayerActiveSwapchainImageSize(
    gpu::SharedImageInterface* sii) {
  CHECK(base_layer_);
  base_layer_->UpdateActiveSwapchainImageSize(sii);
  // All projection layers should have the same size.
  for (const auto& layer_id : layers_sequence_) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it == layers_.end()) {
      continue;
    }
    if (layer_it->second->type() == OpenXrCompositionLayer::Type::kProjection) {
      layer_it->second->UpdateActiveSwapchainImageSize(sii);
    }
  }
}

XrResult OpenXrGraphicsBinding::AcquireSwapchainImages(
    gpu::SharedImageInterface* sii) {
  if (ShouldRenderBaseLayer()) {
    DCHECK(base_layer_);
    if (base_layer_->swapchain_image_state() ==
        OpenXrCompositionLayer::SwapchainImageState::kReleased) {
      RETURN_IF_XR_FAILED(base_layer_->AcquireSwapchainImage(sii));
    }
  }
  for (const auto& layer_id : layers_sequence_) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it == layers_.end()) {
      continue;
    }

    if (layer_it->second->swapchain_image_state() ==
        OpenXrCompositionLayer::SwapchainImageState::kAcquired) {
      continue;
    }

    // Do not request another swapchain image if layer has an image which was
    // not consumed by the previous frame request.
    if (layer_it->second->has_active_swapchain_image()) {
      continue;
    }

    // OpenXR prevents requesting an image more than once for a static
    // swapchain. Subsequent requests will return an error. That's why we must
    // prevent any extra requests for a swapchain image if the layer has already
    // successfully rendered an image.
    //
    // The only way to update a static layer's image is to destroy the existing
    // swapchain and create a new one.
    if (layer_it->second->read_only_data().is_static &&
        layer_it->second->is_rendered()) {
      continue;
    }
    RETURN_IF_XR_FAILED(layer_it->second->AcquireSwapchainImage(sii));
  }
  return XR_SUCCESS;
}

XrResult OpenXrGraphicsBinding::WaitSwapchainImages(
    gpu::SharedImageInterface* sii) {
  XrResult return_result = XR_SUCCESS;

  if (ShouldRenderBaseLayer()) {
    DCHECK(base_layer_);
    if (base_layer_->swapchain_image_state() ==
        OpenXrCompositionLayer::SwapchainImageState::kAcquired) {
      XrResult result = base_layer_->WaitSwapchainImage(sii);
      RETURN_IF_XR_FAILED(result);
      if (result == XR_TIMEOUT_EXPIRED) {
        return_result = result;
      }
    }
  }

  for (const auto& layer_id : layers_sequence_) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it == layers_.end()) {
      continue;
    }
    if (layer_it->second->swapchain_image_state() ==
        OpenXrCompositionLayer::SwapchainImageState::kAcquired) {
      XrResult result = layer_it->second->WaitSwapchainImage(sii);
      RETURN_IF_XR_FAILED(result);
      if (result == XR_TIMEOUT_EXPIRED) {
        return_result = result;
      }
    }
  }

  return return_result;
}

XrResult OpenXrGraphicsBinding::ReleaseActiveSwapchainImages() {
  // ReleaseActiveSwapchainImage() is no-op if the layer doesn't have
  // an active swapchain image. So it is safe to call it on all layers.
  // Also some layers may have been removed from layers_sequence_ before
  // xrEndFrame, we want to release all.
  for (const auto& [_, layer] : layers_) {
    // Sparse explicit layers follow the same rule as the base layer below.
    // Usually an untouched newly-acquired image can remain held while
    // xrEndFrame keeps presenting the previously released image. If the
    // runtime reacquired that *same* image, however, there is no longer a
    // runtime-owned image to present, so release it unchanged.
    if (!layer->is_rendered()) {
      if (layer->active_swapchain_image_is_last_released()) {
        RETURN_IF_XR_FAILED(layer->ReleaseActiveSwapchainImage());
      }
      continue;
    }
    RETURN_IF_XR_FAILED(layer->ReleaseActiveSwapchainImage());
  }

  // The base layer follows the same rule as explicit layers. If Blink did not
  // submit content for this display frame (for example the WebXR sparse-frames
  // test), keep the currently acquired image held for the next render rather
  // than releasing an untouched image. xrEndFrame can continue presenting the
  // swapchain's previously released image in the meantime.
  if (!base_layer_->is_rendered()) {
    // xrEndFrame references the most recently released image. Usually we can
    // leave an untouched newly-acquired image held while continuing to submit
    // that previous image. If the runtime handed us the *same* image index as
    // the last release, however, that image is application-owned again and no
    // released image remains. Re-release it unchanged: its pixels are still
    // exactly the previously presented content.
    if (base_layer_->active_swapchain_image_is_last_released()) {
      return base_layer_->ReleaseActiveSwapchainImage();
    }
    return XR_SUCCESS;
  }
  return base_layer_->ReleaseActiveSwapchainImage();
}

void OpenXrGraphicsBinding::PopulateSharedImageData(
    mojom::XRFrameData& frame_data) {
  // The blink side only paints to base layer when there is no enabled
  // layers defined.
  if (layers_sequence_.empty()) {
    DCHECK(base_layer_);
    const auto* swapchain_info = base_layer_->GetActiveSwapchainImage();
    if (swapchain_info && swapchain_info->shared_image) {
      frame_data.buffer_shared_image = swapchain_info->shared_image->Export();
      frame_data.buffer_sync_token = swapchain_info->sync_token;
    }
  }

  std::vector<mojom::XRLayerFrameDataPtr> layers;
  layers.reserve(layers_sequence_.size());
  for (const auto& layer_id : layers_sequence_) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it == layers_.end()) {
      continue;
    }
    // If the page didn't request to redraw a static image,
    // we shouldn't send up a shared image for it.
    if (layer_it->second->read_only_data().is_static &&
        !layer_it->second->needs_redraw()) {
      continue;
    }
    const auto* swapchain_info = layer_it->second->GetActiveSwapchainImage();
    if (!swapchain_info || !swapchain_info->shared_image) {
      continue;
    }
#if BUILDFLAG(IS_MAC)
    const gfx::Size expected_size(
        layer_it->second->read_only_data().texture_width,
        layer_it->second->read_only_data().texture_height);
    if (swapchain_info->shared_image->size() != expected_size) {
      LOG(ERROR) << "macOS WebXR composition SharedImage size mismatch: layer="
                 << layer_id << " expected=" << expected_size.ToString()
                 << " shared_image="
                 << swapchain_info->shared_image->size().ToString()
                 << " swapchain="
                 << layer_it->second->GetSwapchainImageSize().ToString();
    }
#endif
    mojom::XRLayerFrameDataPtr layer_data = mojom::XRLayerFrameData::New();
    layer_data->layer_id = layer_id;
    layer_data->buffer_shared_image = swapchain_info->shared_image->Export();
    layer_data->buffer_sync_token = swapchain_info->sync_token;
    layers.push_back(std::move(layer_data));
  }
  frame_data.composition_layers_data = std::move(layers);
}

std::vector<scoped_refptr<gpu::ClientSharedImage>>
OpenXrGraphicsBinding::EndSharedImagesExport(
    const std::vector<device::mojom::XRLayerUpdatePtr>& layers,
    std::vector<gpu::SyncToken>& out_sync_tokens) {
  std::vector<scoped_refptr<gpu::ClientSharedImage>> shared_images;

  if (layers_sequence_.empty()) {
    if (base_layer_) {
      CHECK_EQ(layers.size(), 1u);
      auto* swapchain_image = base_layer_->GetActiveSwapchainImage();
      if (swapchain_image && swapchain_image->shared_image) {
        shared_images.emplace_back(swapchain_image->shared_image);
        out_sync_tokens.emplace_back(shared_images.back()->EndExport(
            std::move(layers[0]->shared_image_export_result)));
      }
    }
  } else {
    for (auto& layer : layers) {
      LayerId layer_id = layer->layer_id;
      auto layer_it = layers_.find(layer_id);
      if (layer_it != layers_.end()) {
        auto* swapchain_image = layer_it->second->GetActiveSwapchainImage();
        if (swapchain_image && swapchain_image->shared_image) {
          auto shared_image = swapchain_image->shared_image;
          out_sync_tokens.emplace_back(shared_image->EndExport(
              std::move(layer->shared_image_export_result)));
          shared_images.push_back(std::move(shared_image));
        }
      }
    }
  }
  return shared_images;
}

bool OpenXrGraphicsBinding::Render(
    const scoped_refptr<viz::ContextProvider>& context_provider,
    const std::vector<LayerId>& updated_layers) {
  CHECK(base_layer_);
  bool rendered_any_layer = false;

  if (ShouldRenderBaseLayer()) {
    rendered_any_layer = RenderLayer(*base_layer_, context_provider);
    if (rendered_any_layer) {
      base_layer_->SetIsRendered();
    }
  }

#if DCHECK_IS_ON()
  // All layers excluding those that are static and have been rendered
  // previously should be updated.
  absl::flat_hash_set<LayerId> layers_set(updated_layers.begin(),
                                          updated_layers.end());
  for (auto layer_id : layers_sequence_) {
    if (layers_set.contains(layer_id)) {
      continue;
    }

    auto layer_it = layers_.find(layer_id);
    DCHECK(layer_it != layers_.end());
    if (layer_it->second->read_only_data().is_static &&
        layer_it->second->is_rendered()) {
      continue;
    }

    DLOG(ERROR) << __func__ << ": Not all layers in render state are updated";
    break;
  }
#endif

  // Static layers are only rendered once. So updated_layers can be a subset
  // of layers_sequence_. The order isn't important here. See
  // GetLayersForViewConfig where the order is important.
  for (LayerId layer_id : updated_layers) {
    auto layer_it = layers_.find(layer_id);
    if (layer_it != layers_.end() &&
        RenderLayer(*layer_it->second, context_provider)) {
      layer_it->second->SetIsRendered();
      rendered_any_layer = true;
    }
  }

  return rendered_any_layer;
}

bool OpenXrGraphicsBinding::CreateCompositionLayer(
    mojom::XRCompositionLayerDataPtr layer_data,
    gpu::SharedImageInterface* sii) {
  if (!SupportsLayers()) {
    return false;
  }

  // We don't support "stereo" layout here. The blink side should wrap it into
  // "stereo-left-right".
  CHECK_NE(layer_data->read_only_data->layout,
           device::mojom::XRLayerLayout::kStereo);

  auto layer_id = layer_data->read_only_data->layer_id;
  CHECK(!layers_.contains(layer_id));

  auto new_layer = std::make_unique<OpenXrCompositionLayer>(
      std::move(layer_data), this, CreateLayerGraphicsBindingData());

  if (new_layer->type() == OpenXrCompositionLayer::Type::kProjection) {
#if BUILDFLAG(IS_MAC)
    if (RequiresSharedImages()) {
      // The direct Metal path has no intermediate resize/blit. Match the
      // OpenXR swapchain itself to the texture Blink exposes for this WebXR
      // projection layer; otherwise color/depth attachments can have different
      // sizes and WebGL resolves fail with GL_INVALID_FRAMEBUFFER_OPERATION.
      const gfx::Size layer_size(new_layer->read_only_data().texture_width,
                                 new_layer->read_only_data().texture_height);
      DVLOG(1) << __func__ << ": direct Metal projection layer size "
               << layer_size.ToString() << " (base "
               << GetProjectionLayerSwapchainImageSize().ToString() << ")";
      new_layer->SetSwapchainImageSize(layer_size);
    } else {
      new_layer->SetSwapchainImageSize(GetProjectionLayerSwapchainImageSize());
    }
#else
    // Other bindings composite/resize projection-layer content into the base
    // runtime swapchain size.
    new_layer->SetSwapchainImageSize(GetProjectionLayerSwapchainImageSize());
#endif
  }

  layers_.emplace(layer_id, std::move(new_layer));
  return true;
}

OpenXrCompositionLayer* OpenXrGraphicsBinding::GetCompositionLayer(
    LayerId layer_id) {
  auto layer_it = layers_.find(layer_id);
  return layer_it != layers_.end() ? layer_it->second.get() : nullptr;
}

void OpenXrGraphicsBinding::DestroyCompositionLayer(
    LayerId layer_id,
    gpu::SharedImageInterface* sii) {
  auto layer_it = layers_.find(layer_id);
  if (layer_it == layers_.end()) {
    return;
  }

  layer_it->second->DestroySwapchain(sii);
  last_rendered_layer_projection_views_.erase(layer_id);
  layers_.erase(layer_it);
}

void OpenXrGraphicsBinding::SetEnabledCompositionLayers(
    const std::vector<LayerId>& layer_ids,
    XrSession session,
    uint32_t swapchain_sample_count,
    gpu::SharedImageInterface* sii) {
  absl::flat_hash_set<LayerId> enabled_layers(layer_ids.begin(),
                                              layer_ids.end());
  has_custom_projection_layer_ = false;
  for (auto& [id, layer] : layers_) {
    if (enabled_layers.contains(id)) {
#if BUILDFLAG(IS_MAC)
      if (RequiresSharedImages() &&
          layer->type() == OpenXrCompositionLayer::Type::kProjection) {
        // WebXR projection layers may use a framebuffer scale factor that is
        // different from the runtime's native/base projection size. The Metal
        // shared-image path exposes this swapchain directly to Blink, so the
        // OpenXR swapchain must match the layer descriptor exactly.
        //
        // Reassert this immediately before swapchain creation rather than only
        // in CreateCompositionLayer(). Base-layer size/transfer updates can
        // occur between layer creation and activation.
        const gfx::Size layer_size(layer->read_only_data().texture_width,
                                   layer->read_only_data().texture_height);
        DVLOG(1) << __func__ << ": macOS projection layer " << id
                 << " descriptor=" << layer_size.ToString()
                 << " swapchain="
                 << layer->GetSwapchainImageSize().ToString()
                 << " has_swapchain=" << layer->HasColorSwapchain();
        if (layer->GetSwapchainImageSize() != layer_size) {
          LOG(WARNING) << __func__ << ": correcting macOS projection layer "
                       << id << " swapchain size from "
                       << layer->GetSwapchainImageSize().ToString() << " to "
                       << layer_size.ToString();
          if (layer->HasColorSwapchain()) {
            layer->DestroySwapchain(sii);
          }
          last_rendered_layer_projection_views_.erase(id);
          layer->SetSwapchainImageSize(layer_size);
        }
      }
#endif
      if (!layer->HasColorSwapchain()) {
        last_rendered_layer_projection_views_.erase(id);
        uint32_t layer_sample_count = swapchain_sample_count;
#if BUILDFLAG(IS_MAC)
        // The direct Metal SharedImage import path only supports
        // single-sample 2D textures. Match the base-layer policy here for
        // explicit WebXR composition layers.
        if (RequiresSharedImages()) {
          layer_sample_count = 1;
        }
#endif
        XrResult result =
            layer->CreateSwapchain(session, layer_sample_count);
        CHECK_EQ(result, XR_SUCCESS);
        CreateSharedImages(*layer, sii);
      }
      if (layer->type() == OpenXrCompositionLayer::Type::kProjection) {
        has_custom_projection_layer_ = true;
      }
    } else if (layer->HasColorSwapchain()) {
      layer->DestroySwapchain(sii);
      last_rendered_layer_projection_views_.erase(id);
    }
  }
  layers_sequence_ = layer_ids;
}

}  // namespace device
