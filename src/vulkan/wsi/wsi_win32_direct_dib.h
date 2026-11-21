/* SPDX-License-Identifier: MIT */
#ifndef WSI_WIN32_DIRECT_DIB_H
#define WSI_WIN32_DIRECT_DIB_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Direct GDI submission hands the mapped, GPU-filled rows of a swapchain
 * image to StretchDIBits() without the intermediate CPU row copy into the
 * DIB section. The pixel storage stays owned by the swapchain image; it
 * must remain untouched until GdiFlush() has completed this thread's GDI
 * batch, which the present path already guarantees before releasing the
 * image. The GPU staging copy into the host-visible mapping is unchanged
 * and still completes before the CPU reads any pixel. */

/* A 32-bpp BI_RGB DIB stores B, G, R, X bytes per pixel and has tightly
 * packed rows: only a top-down mapped image whose row pitch equals
 * width * 4 can be submitted directly. The caller separately guarantees
 * the byte order (B8G8R8A8_UNORM/SRGB); any other format keeps the generic
 * swizzle path. */
static inline bool
wsi_win32_direct_dib_tight_stride(uint32_t width, uint32_t height,
                                  uint64_t row_pitch)
{
   const uint64_t row_bytes = (uint64_t)width * 4;
   return width > 0 && height > 0 &&
          width <= INT32_MAX && height <= INT32_MAX &&
          row_pitch == row_bytes && row_bytes * height <= UINT32_MAX;
}

/* Direct submission must behave exactly like the reference StretchBlt() of
 * the top-down DIB section: identity mapping, no world transform or
 * mirroring, no color management, and a target rectangle that fits inside
 * the DC's clip box so the transfer cannot be clipped to fewer scan lines.
 * A partial clip is left to the generic path, which handles it naturally. */
static inline bool
wsi_win32_direct_dib_dc_ready(HDC dc, uint32_t width, uint32_t height)
{
   if (GetMapMode(dc) != MM_TEXT || GetGraphicsMode(dc) != GM_COMPATIBLE ||
       GetLayout(dc) != 0 || SetICMMode(dc, ICM_QUERY) != ICM_OFF)
      return false;
   RECT clip;
   if (GetClipBox(dc, &clip) == ERROR)
      return false;
   return (int64_t)clip.left <= 0 && (int64_t)clip.top <= 0 &&
          (int64_t)clip.right >= (int64_t)width &&
          (int64_t)clip.bottom >= (int64_t)height;
}

/* Submit top-down BGRA rows (row 0 = top, tight pitch) straight from the
 * mapped image storage. Returns FALSE when GDI clipped or failed the
 * transfer, in which case the caller falls back to the generic DIB-section
 * blit; SRCCOPY makes a retried blit idempotent. */
static inline bool
wsi_win32_direct_dib_submit(HDC dc, const void *bits,
                            uint32_t width, uint32_t height)
{
   BITMAPINFO info;
   memset(&info, 0, sizeof(info));
   info.bmiHeader.biSize = sizeof(info.bmiHeader);
   info.bmiHeader.biWidth = (LONG)width;
   info.bmiHeader.biHeight = -(LONG)height;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;
   return StretchDIBits(dc, 0, 0, (int)width, (int)height,
                        0, 0, (int)width, (int)height, bits, &info,
                        DIB_RGB_COLORS, SRCCOPY) == (int)height;
}

#endif
