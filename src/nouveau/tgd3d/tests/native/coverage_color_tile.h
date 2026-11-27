/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr unsigned color_coverage_shapes[] = {0, 24, 6};
static constexpr uint16_t color_coverage_seed = 0x3126;

// The public FL11_1 WARP reference for the overlapping OR/XOR/OR draws.
// This interior tile includes fully covered, partially covered, and untouched pixels.
static bool check_color_coverage_tile(const void *data, unsigned pitch)
{
   static constexpr uint16_t expected[4][8] = {
      {0xffff, 0xffff, 0xffff, 0xdcdc, 0xced9, 0xced9, 0xced9, 0xced9},
      {0xffff, 0xffff, 0xffff, 0xdcdc, 0xced9, 0xced9, 0xced9, 0xced9},
      {0xffff, 0xffff, 0xffff, 0x9dbd, 0x97bc, 0x97bc, 0x97bc, 0x97bc},
      {0xffff, 0xffff, 0xffff, 0xbf37, 0x3126, 0x3126, 0x3126, 0x3126},
   };
   if (!data || pitch < 64 * sizeof(uint16_t)) return false;
   unsigned matches = 0;
   std::printf("coverage16-color-pixels\t");
   for (unsigned y = 0; y < 4; ++y) {
      const auto *row = static_cast<const uint8_t *>(data) + (32 + y) * pitch;
      for (unsigned x = 0; x < 8; ++x) {
         uint16_t value;
         std::memcpy(&value, row + (32 + x) * sizeof(value), sizeof(value));
         std::printf("%04x", unsigned(value));
         matches += value == expected[y][x];
      }
   }
   std::printf("\ncoverage16-color-matches\t%u\t32\n", matches);
   return matches == 32;
}
