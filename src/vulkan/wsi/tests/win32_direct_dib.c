/* SPDX-License-Identifier: MIT */
/* GDI equivalence for submitting mapped top-down BGRA rows directly with
 * StretchDIBits(), mirroring the production decision sequence in
 * wsi_common_win32.cpp. Runs against real Windows GDI (or Wine); the
 * pixel storage here stands in for the mapped swapchain image and is never
 * reused before GdiFlush() completes, like the present path requires. */
#include "../wsi_win32_direct_dib.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

struct bitmap {
   HDC dc;
   HBITMAP dib;
   HGDIOBJ previous;
   uint32_t *bits;
};

static struct bitmap create_bitmap(const BITMAPINFO *info)
{
   struct bitmap bitmap = {0};
   void *bits = NULL;
   bitmap.dc = CreateCompatibleDC(NULL);
   if (!bitmap.dc)
      return bitmap;
   bitmap.dib = CreateDIBSection(bitmap.dc, info, DIB_RGB_COLORS, &bits, NULL, 0);
   bitmap.bits = bits;
   if (bitmap.dib)
      bitmap.previous = SelectObject(bitmap.dc, bitmap.dib);
   return bitmap;
}

static void destroy_bitmap(struct bitmap bitmap)
{
   if (bitmap.previous && bitmap.previous != HGDI_ERROR)
      SelectObject(bitmap.dc, bitmap.previous);
   if (bitmap.dib)
      DeleteObject(bitmap.dib);
   if (bitmap.dc)
      DeleteDC(bitmap.dc);
}

static uint32_t pattern_pixel(unsigned x, unsigned y)
{
   /* Distinct R/G/B per coordinate so orientation and channel order are
    * both caught; row 0 differs from row 1 to catch flipped images. */
   return 0xFF000000u | (uint32_t)((x * 11 + y * 9) & 255) << 16 |
          (uint32_t)((x * 3 + y * 19) & 255) << 8 | ((x * 7 + y * 5) & 255);
}

static int check_size(int width, int height)
{
   BITMAPINFO info = {0};
   info.bmiHeader.biSize = sizeof(info.bmiHeader);
   info.bmiHeader.biWidth = width;
   info.bmiHeader.biHeight = -height;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;

   /* Reference chain: CPU rows -> DIB section -> StretchBlt, exactly the
    * generic production path. Direct chain: mapped rows in place. */
   struct bitmap source = create_bitmap(&info), reference = create_bitmap(&info);
   struct bitmap direct = create_bitmap(&info);
   const size_t count = (size_t)width * height;
   uint32_t *mapped = malloc(count * sizeof(*mapped));
   int ret = 1;
   if (!mapped || !source.bits || !reference.bits || !direct.bits)
      goto cleanup;
   for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
         mapped[(size_t)y * width + x] = pattern_pixel((unsigned)x, (unsigned)y);
   memcpy(source.bits, mapped, count * sizeof(*mapped));

   /* Same predicate sequence as wsi_win32_queue_present(): eligibility,
    * DC readiness, direct submit, GdiFlush before any storage reuse. */
   CHECK(wsi_win32_direct_dib_tight_stride((uint32_t)width, (uint32_t)height,
                                           (uint64_t)width * 4));
   CHECK(wsi_win32_direct_dib_dc_ready(direct.dc, (uint32_t)width, (uint32_t)height));
   const BOOL reference_copied =
      StretchBlt(reference.dc, 0, 0, width, height,
                 source.dc, 0, 0, width, height, SRCCOPY);
   const BOOL direct_copied =
      wsi_win32_direct_dib_submit(direct.dc, mapped, (uint32_t)width, (uint32_t)height);
   if (!GdiFlush() || !reference_copied || !direct_copied) {
      fprintf(stderr, "%dx%d reference=%d direct=%d\n",
              width, height, reference_copied, direct_copied);
      goto cleanup;
   }
   for (size_t i = 0; i < count; i++) {
      if (((reference.bits[i] ^ mapped[i]) | (direct.bits[i] ^ mapped[i])) & 0xFFFFFFu) {
         fprintf(stderr, "%dx%d RGB mismatch at %zu (%#x %#x %#x)\n",
                 width, height, i, mapped[i], reference.bits[i], direct.bits[i]);
         goto cleanup;
      }
   }
   printf("%dx%d exact_rgb=%zu\n", width, height, count);
   ret = 0;
cleanup:
   free(mapped);
   destroy_bitmap(source);
   destroy_bitmap(reference);
   destroy_bitmap(direct);
   return ret;
}

static void check_predicates(void)
{
   /* Tight 32-bpp rows only: the production path separately restricts the
    * format to B8G8R8A8_UNORM/SRGB, which share this BGRA byte layout. */
   CHECK(wsi_win32_direct_dib_tight_stride(640, 360, 640u * 4));
   CHECK(wsi_win32_direct_dib_tight_stride(1, 1, 4));
   CHECK(!wsi_win32_direct_dib_tight_stride(0, 360, 0));
   CHECK(!wsi_win32_direct_dib_tight_stride(640, 0, 640u * 4));
   CHECK(!wsi_win32_direct_dib_tight_stride(640, 360, 640u * 4 + 4)); /* padded stride */
   CHECK(!wsi_win32_direct_dib_tight_stride(640, 360, 640u * 4 - 4)); /* truncated */
   CHECK(!wsi_win32_direct_dib_tight_stride(INT32_MAX, 4, (uint64_t)INT32_MAX * 4 + 4));

   /* A DC whose clip box no longer covers the target is not ready; the
    * production path then keeps the generic blit for this frame and never
    * issues the direct submit, so a clipped transfer can only arise from a
    * clip change racing the present, which the submit's line count reports
    * and the caller retries idempotently through the DIB section. */
   HDC dc = CreateCompatibleDC(NULL);
   CHECK(dc != NULL);
   BITMAPINFO info = {0};
   info.bmiHeader.biSize = sizeof(info.bmiHeader);
   info.bmiHeader.biWidth = 8;
   info.bmiHeader.biHeight = -8;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;
   void *bits = NULL;
   HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
   CHECK(dib && bits);
   SelectObject(dc, dib);
   CHECK(wsi_win32_direct_dib_dc_ready(dc, 8, 8));
   ExcludeClipRect(dc, 0, 0, 8, 8);
   CHECK(!wsi_win32_direct_dib_dc_ready(dc, 8, 8));
   DeleteObject(dib);
   DeleteDC(dc);
}

int main(void)
{
   check_predicates();
   int failed = 0;
   failed |= check_size(640, 360);
   failed |= check_size(1, 1);
   failed |= check_size(127, 61);
   failed |= check_size(3, 257);
   if (failed) {
      fprintf(stderr, "FAIL: %d size cases mismatched\n", failed);
      return 1;
   }
   printf("PASS: direct DIB submission matches the reference blit for every size\n");
   return 0;
}
