/* SPDX-License-Identifier: MIT
 * ClearView for the texture render-target views this driver creates.
 * Rectangles apply to every slice of the view and preserve all other pixels.
 */
#include "tgd3d.h"
#include <cmath>

namespace tgd3d {

static bool normalized_format(DXGI_FORMAT format, bool *signed_value)
{
   *signed_value = false;
   switch (format) {
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R8_SNORM:
      *signed_value = true;
      return true;
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_A8_UNORM:
   case DXGI_FORMAT_B5G6R5_UNORM:
   case DXGI_FORMAT_B5G5R5A1_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_B4G4R4A4_UNORM:
      return true;
   default:
      return false;
   }
}

static unsigned integer_bits(const FormatInfo &info, unsigned channel)
{
   switch (info.family) {
   case 1: case 2: case 4: case 9: return 32;
   case 3: case 8: case 12: return 16;
   case 6: return channel == 3 ? 2 : 10;
   case 7: case 11: case 13: return 8;
   default: return 0;
   }
}

static bool color_value(const View &view, const FLOAT color[4], VkClearValue *out)
{
   const FormatInfo *info = format_info(view.format);
   if (!info || (info->flags & (FMT_TYPELESS | FMT_DEPTH | FMT_STENCIL | FMT_BC | FMT_NO_VIEW)))
      return false;
   bool signed_norm;
   const bool normalized = normalized_format(view.format, &signed_norm);
   for (unsigned c = 0; c < 4; ++c) {
      if (info->flags & (FMT_UINT | FMT_SINT)) {
         const unsigned bits = integer_bits(*info, c);
         if (!bits) return false;
         const double input = std::isnan(color[c]) ? 0.0 : color[c];
         if (info->flags & FMT_UINT) {
            const double maximum = (UINT64_C(1) << bits) - 1;
            out->color.uint32[c] = static_cast<uint32_t>(std::fmin(std::fmax(input, 0.0), maximum));
         } else {
            const double minimum = -(INT64_C(1) << (bits - 1));
            const double maximum = (INT64_C(1) << (bits - 1)) - 1;
            out->color.int32[c] = static_cast<int32_t>(std::fmin(std::fmax(input, minimum), maximum));
         }
      } else if (normalized) {
         const float input = std::isnan(color[c]) ? 0.0f : color[c];
         out->color.float32[c] = std::fmin(std::fmax(input, signed_norm ? -1.0f : 0.0f), 1.0f);
      } else {
         out->color.float32[c] = color[c];
      }
   }
   if (view.formatFlags & FMT_ALPHA_ONE)
      out->color.float32[3] = 1.0f;
   if (view.format == DXGI_FORMAT_A8_UNORM && view.info.format == VK_FORMAT_R8_UNORM)
      out->color.float32[0] = out->color.float32[3];
   return true;
}

static bool clip_rect(const RECT &input, const View &view, VkClearRect *out)
{
   const LONG left = input.left < 0 ? 0 : input.left;
   const LONG top = input.top < 0 ? 0 : input.top;
   const LONG right = input.right > (LONG)view.width ? (LONG)view.width : input.right;
   const LONG bottom = input.bottom > (LONG)view.height ? (LONG)view.height : input.bottom;
   if (left >= right || top >= bottom) return false;
   *out = {{{left, top}, {(UINT)(right - left), (UINT)(bottom - top)}}, 0, view.layers};
   return true;
}

void APIENTRY ClearView11(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, void *handle,
                          const FLOAT color[4], const D3D10_DDI_RECT *rects, UINT count)
{
   Device *dev = D(h);
   if (!dev || FAILED(dev->lost)) return;
   RemovalOnlyErrorScope errors(dev);
   /* The current bridge covers texture-2D RTVs (including cube slices).
    * UAV/video/buffer views need their own implementations before admission;
    * this DDI excludes 3-D textures. Never silently skip an unsupported clear. */
   if (type != D3D10DDI_HT_RENDERTARGETVIEW) { TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return; }
   View *view = static_cast<View *>(handle);
   if (!view || view->dev != dev || !view->res || view->res->dev != dev ||
       view->type != ViewType::RTV || !color) { TG_ERROR(dev, E_INVALIDARG); return; }
   if (view->res->kind != ResKind::Image || view->res->imageType != VK_IMAGE_TYPE_2D ||
       !view->res->storage.image || !view->width || !view->height || !view->layers ||
       view->info.subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
      TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return;
   }
   VkClearValue value = {};
   if (!color_value(*view, color, &value)) { TG_ERROR(dev, DXGI_ERROR_UNSUPPORTED); return; }
   bool any = !rects;
   for (UINT i = 0; rects && i < count; ++i) {
      if (rects[i].left > rects[i].right || rects[i].top > rects[i].bottom) {
         TG_ERROR(dev, E_INVALIDARG); return;
      }
      VkClearRect clipped;
      any |= clip_rect(rects[i], *view, &clipped);
   }
   if (!any) return;
   VkImageView image_view = view_get(view);
   if (!image_view) return;
   VkCommandBuffer cmd = cmd_begin(dev); /* flush older clears and order attachment LOAD */
   if (!cmd) return;
   image_use(dev, view->res->storage, true);
   if (FAILED(dev->lost)) return;
   VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
   attachment.imageView = image_view;
   attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   attachment.loadOp = rects ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
   attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.clearValue = value;
   VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
   rendering.renderArea = {{0, 0}, {view->width, view->height}};
   rendering.layerCount = view->layers;
   rendering.colorAttachmentCount = 1;
   rendering.pColorAttachments = &attachment;
   dev->vk.CmdBeginRendering(cmd, &rendering);
   ++dev->stats.renderPasses;
   /* A bounded batch avoids allocating proportional to the runtime rectangle
    * count. Clears ignore draw predication and do not contribute occlusion. */
   VkClearAttachment clear = {};
   clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   clear.clearValue = value;
   VkClearRect batch[32];
   UINT used = 0;
   for (UINT i = 0; rects && i < count; ++i) {
      if (clip_rect(rects[i], *view, &batch[used]) && ++used == 32) {
         dev->vk.CmdClearAttachments(cmd, 1, &clear, used, batch);
         used = 0;
      }
   }
   if (used) dev->vk.CmdClearAttachments(cmd, 1, &clear, used, batch);
   dev->vk.CmdEndRendering(cmd);
}

} // namespace tgd3d
