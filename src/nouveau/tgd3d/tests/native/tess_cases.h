/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include "tess_shaders.h"

namespace tess_test {
struct Blob {
   const UINT *code;
   size_t words;
   template<size_t N> constexpr Blob(const UINT (&b)[N]) : code(b), words(N) {}
   size_t bytes() const { return words * sizeof(UINT); }
};
enum Hull { Tri, Wide, One, Max, Ccw, Point, Quad, Line };
enum Domain { Triangle, Quadrilateral, Isoline };
static constexpr Blob hulls[] = {tri_hs, tri_wide_hs, tri_one_hs, tri_32_hs, tri_ccw_hs, tri_point_hs, quad_hs, line_hs};
static constexpr Blob domains[] = {tri_ds, quad_ds, line_ds};
struct Case { const char *name; Hull hull; Domain domain; UINT in, out; float factor; bool gs; };
static constexpr Case scenes[] = {
   {"tri-1", Tri, Triangle, 3, 3, 1, false}, {"tri-2", Tri, Triangle, 3, 3, 2, false},
   {"tri-wide-gs", Wide, Triangle, 3, 3, 2, true}, {"tri-narrow-again", Tri, Triangle, 3, 3, 2, false},
   {"one-input", One, Triangle, 1, 3, 1, false}, {"32-inputs", Max, Triangle, 32, 3, 1, false},
   {"reverse-winding", Ccw, Triangle, 3, 3, 2, false}, {"point-mode", Point, Triangle, 3, 3, 2, false},
   {"quad-even", Quad, Quadrilateral, 4, 4, 2, false}, {"quad-fractional", Quad, Quadrilateral, 4, 4, 2.5f, false},
   {"isoline-odd", Line, Isoline, 2, 2, 3, false}, {"isoline-fractional", Line, Isoline, 2, 2, 2.5f, false},
   {"zero-factor", Tri, Triangle, 3, 3, 0, false},
};
constexpr UINT width = 8, height = 2, firstWord = 4, hullWords = 9, domainWords = 15, guard = 0xdecafbad;
using HullData = std::array<UINT, firstWord + hullWords + 5>;
using DomainData = std::array<UINT, firstWord + domainWords + 3>;
using Pixels = std::array<UINT, width * height>;
static HullData initial_hull()
{
   HullData out; out.fill(guard); out[firstWord + 8] = 0; return out;
}
static DomainData initial_domain()
{
   DomainData out; out.fill(guard);
   std::fill_n(out.begin() + firstWord, domainWords, 0);
   return out;
}
static Pixels initial_pixels() { Pixels out; out.fill(guard); return out; }
static UINT hs_seed(UINT i) { return 101 + 7 * i; }
static UINT ds_seed(UINT i) { return 717 + 11 * i; }
template<size_t N>
static void emit(const char *field, const char *scene, const std::array<UINT, N> &values)
{
   std::printf("tess-%s\t%s", field, scene);
   for (UINT x : values) std::printf("\t%08x", x);
   std::puts("");
}

static bool verify_line_segments(const std::array<UINT, 16 * 5> &vertices, const Case &c, UINT tag)
{
   const float x = c.factor == 3 ? 1.0f / 3 : .5f;
   const float edges[]{-1, -x, x, 1};
   bool seen[2][3]{};
   for (UINT segment = 0; segment < 6; ++segment) {
      float a[4], b[4];
      std::memcpy(a, &vertices[10 * segment], sizeof(a)); std::memcpy(b, &vertices[10 * segment + 5], sizeof(b));
      const UINT row = a[1] == .5f ? 0 : 1;
      if (a[1] != (row ? -.5f : .5f) || b[1] != a[1] || a[2] != 0 || b[2] != 0 || a[3] != 1 || b[3] != 1 ||
          vertices[10 * segment + 4] != tag || vertices[10 * segment + 9] != tag) return false;
      UINT i = 0;
      while (i < 3 && (std::fabs(a[0] - edges[i]) > 1.f/32768 || std::fabs(b[0] - edges[i + 1]) > 1.f/32768)) ++i;
      if (i == 3 || seen[row][i]) return false;
      seen[row][i] = true;
   }
   for (size_t i = 12 * 5; i < vertices.size(); ++i) if (vertices[i] != guard) return false;
   std::printf("tess-verified-segments\t%s\t6\n", c.name);
   return true;
}

static bool verify(UINT index, const HullData &h, const DomainData &d, const Pixels &pixels,
                   UINT64 iaVertices, UINT64 patches, UINT64 evaluations, UINT64 gsPrimitives)
{
   const Case &c = scenes[index];
   emit("hull", c.name, h); emit("domain", c.name, d); emit("pixels", c.name, pixels);
   std::printf("tess-query\t%s\t%llu\t%llu\t%llu\t%llu\n", c.name,
      (unsigned long long)iaVertices, (unsigned long long)patches,
      (unsigned long long)evaluations, (unsigned long long)gsPrimitives);
   HullData expectedH = initial_hull();
   const UINT control0 = hs_seed(index) + 7 + 3 * c.in;
   for (UINT i = 0; i < c.out; ++i) expectedH[firstWord + i] = control0 + 13 * i;
   const UINT patchTag = hs_seed(index) + 7 + 5 * c.in + 73;
   expectedH[firstWord + 8] = patchTag;
   const UINT tag = ds_seed(index) + 17 + patchTag + 2 * control0 + 13 * (c.out - 1);
   if (h != expectedH || iaVertices != c.in || patches != 1 || evaluations != d[firstWord]) return false;
   if (!c.factor) return d == initial_domain() && pixels == initial_pixels() && !gsPrimitives;
   if (d[firstWord + 1] != tag || !evaluations || (c.gs ? !gsPrimitives : gsPrimitives != 0)) return false;
   for (UINT i = 0; i < d.size(); ++i)
      if ((i < firstWord || i >= firstWord + domainWords) && d[i] != guard) return false;
   for (UINT y = 0; y <= 12; ++y) if (d[firstWord + 2 + y] & ~0x1fffu) return false;
   // Every active triangular/quad domain includes the three shared corners.
   if (c.domain != Isoline && ((d[firstWord + 2] & 0x1001) != 0x1001 || !(d[firstWord + 14] & 1))) return false;
   if (c.domain == Quadrilateral && !(d[firstWord + 14] & 0x1000)) return false;
   const bool front = c.hull == Ccw || c.hull == Point || c.domain == Isoline;
   for (UINT i = 0; i < pixels.size(); ++i) {
      const bool hit = c.hull != Point || i == 5 || i == 15;
      const UINT expected = hit ? tag + (c.gs ? 1024 : 0) + i + (front ? 0x10000 : 0) : guard;
      if (pixels[i] != expected) return false;
   }
   return true;
}
}
