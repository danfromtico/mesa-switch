/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_WINDOWS_EXEC_TRANSPORT_H
#define TG_GPU_WINDOWS_EXEC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "submit_abi.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb197.h"
#include "nouveau/vulkan/nvk_mme_slots.h"

enum tg_win_exec_status {
   TG_WIN_EXEC_LINEAR = 0,
   TG_WIN_EXEC_INDIRECT,
   TG_WIN_EXEC_INVALID_SLICE,
   TG_WIN_EXEC_OVERSIZED,
   TG_WIN_EXEC_ORPHAN_NO_PREFETCH,
   TG_WIN_EXEC_MISSING_INDIRECT_SEGMENT,
   TG_WIN_EXEC_MISSING_CONTINUATION,
   TG_WIN_EXEC_MALFORMED_PREFIX,
   TG_WIN_EXEC_UNSUPPORTED_INDIRECT_MACRO,
};

struct tg_win_exec_slice {
   uint64_t addr;
   uint64_t size_B;
   bool incomplete;
   bool no_prefetch;
};

struct tg_win_exec_packet {
   enum tg_win_exec_status status;
   uint32_t consumed;
   uint32_t command_bytes;
   uint32_t mme_slot;
   struct tg_gpu_submit_indirect indirect;
};

static inline const char *
tg_win_exec_status_str(enum tg_win_exec_status status)
{
   switch (status) {
   case TG_WIN_EXEC_LINEAR:
      return "linear push buffer";
   case TG_WIN_EXEC_INDIRECT:
      return "indirect GPFIFO push sequence";
   case TG_WIN_EXEC_INVALID_SLICE:
      return "empty or unaligned push slice";
   case TG_WIN_EXEC_OVERSIZED:
      return "push slice exceeds 64 KiB Windows KMD capture limit";
   case TG_WIN_EXEC_ORPHAN_NO_PREFETCH:
      return "standalone no-prefetch/GPU-generated command slice is not supported on Windows GM20B";
   case TG_WIN_EXEC_MISSING_INDIRECT_SEGMENT:
      return "incomplete push prefix is not followed by a valid indirect parameter slice";
   case TG_WIN_EXEC_MISSING_CONTINUATION:
      return "indirect dispatch prefix/parameter pair is missing its continuation suffix";
   case TG_WIN_EXEC_MALFORMED_PREFIX:
      return "incomplete push prefix does not end at an open B197 MME macro call";
   case TG_WIN_EXEC_UNSUPPORTED_INDIRECT_MACRO:
      return "unsupported pre-Turing indirect MME macro on Windows GM20B";
   }
   return "unknown Windows NVK exec transport failure";
}

static inline bool
tg_win_exec_slice_valid_bounds(const struct tg_win_exec_slice *slice)
{
   return slice && slice->addr && !(slice->addr & 3u) &&
          slice->size_B && !(slice->size_B & 3u);
}

static inline enum tg_win_exec_status
tg_win_exec_check_head(const struct tg_win_exec_slice *slices,
                       uint32_t index, uint32_t count)
{
   if (!slices || index >= count || !tg_win_exec_slice_valid_bounds(&slices[index]))
      return TG_WIN_EXEC_INVALID_SLICE;

   const struct tg_win_exec_slice *head = &slices[index];
   if (head->size_B > TG_GPU_SUBMIT_MAX_BYTES)
      return TG_WIN_EXEC_OVERSIZED;

   if (head->no_prefetch)
      return TG_WIN_EXEC_ORPHAN_NO_PREFETCH;

   if (!head->incomplete)
      return TG_WIN_EXEC_LINEAR;

   if (index + 1 >= count)
      return TG_WIN_EXEC_MISSING_INDIRECT_SEGMENT;

   const struct tg_win_exec_slice *ind = &slices[index + 1];
   if (!tg_win_exec_slice_valid_bounds(ind) || ind->incomplete || !ind->no_prefetch)
      return TG_WIN_EXEC_MISSING_INDIRECT_SEGMENT;
   if (ind->size_B > TG_GPU_SUBMIT_MAX_BYTES)
      return TG_WIN_EXEC_OVERSIZED;

   return TG_WIN_EXEC_INDIRECT;
}

static inline struct tg_win_exec_packet
tg_win_exec_plan_step(const struct tg_win_exec_slice *slices,
                      uint32_t index, uint32_t count,
                      const uint32_t *prefix_words)
{
   struct tg_win_exec_packet pkt = {
      .status = tg_win_exec_check_head(slices, index, count),
      .mme_slot = NVK_MME_COUNT,
   };
   if (pkt.status == TG_WIN_EXEC_LINEAR) {
      pkt.consumed = 1;
      pkt.command_bytes = (uint32_t)slices[index].size_B;
      return pkt;
   }
   if (pkt.status != TG_WIN_EXEC_INDIRECT)
      return pkt;
   if (!prefix_words) {
      pkt.status = TG_WIN_EXEC_INVALID_SLICE;
      return pkt;
   }

   const struct tg_win_exec_slice *prefix = &slices[index];
   const struct tg_win_exec_slice *ind = &slices[index + 1];
   const size_t word_count = (size_t)(prefix->size_B / sizeof(uint32_t));
   const uint32_t indirect_words = (uint32_t)(ind->size_B / sizeof(uint32_t));

   for (size_t n = 0; n < word_count;) {
      const uint32_t header = prefix_words[n++];
      if (header == NVB06F_DMA_NOP)
         continue;
      if (header & (1u << 12)) {
         pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
         return pkt;
      }
      const uint32_t op = NVVAL_GET(header, NVB06F, DMA_INCR, OPCODE);
      if (op == NVB06F_DMA_IMMD_OPCODE_VALUE)
         continue;
      if (op != NVB06F_DMA_INCR_OPCODE_VALUE &&
          op != NVB06F_DMA_NONINCR_OPCODE_VALUE &&
          op != NVB06F_DMA_ONEINCR_OPCODE_VALUE) {
         pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
         return pkt;
      }
      const uint32_t method_count = NVVAL_GET(header, NVB06F, DMA_INCR, COUNT);
      if (!method_count) {
         pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
         return pkt;
      }
      if (method_count <= word_count - n) {
         n += method_count;
         continue;
      }

      const uint32_t subc = NVVAL_GET(header, NVB06F, DMA_INCR, SUBCHANNEL);
      const uint32_t method = NVVAL_GET(header, NVB06F, DMA_INCR, ADDRESS) * 4u;
      const uint32_t base = NVB197_CALL_MME_MACRO(0);
      if (op != NVB06F_DMA_ONEINCR_OPCODE_VALUE || subc != 0 ||
          method < base || (method - base) % 8u != 0) {
         pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
         return pkt;
      }

      const uint32_t slot = (method - base) / 8u;
      const size_t inline_words = word_count - n;
      pkt.mme_slot = slot;

      switch (slot) {
      case NVK_MME_DRAW_INDIRECT:
      case NVK_MME_DRAW_INDEXED_INDIRECT:
         if (inline_words != 2 || method_count != inline_words + indirect_words) {
            pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
            return pkt;
         }
         pkt.consumed = 2;
         pkt.command_bytes = (uint32_t)prefix->size_B;
         pkt.indirect = (struct tg_gpu_submit_indirect) {
            .address = ind->addr,
            .bytes = (uint32_t)ind->size_B,
            .suffix_offset = 0,
         };
         return pkt;

      case NVK_MME_XFB_DRAW_INDIRECT:
         if (inline_words != 2 || indirect_words != 1 || method_count != 3) {
            pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
            return pkt;
         }
         pkt.consumed = 2;
         pkt.command_bytes = (uint32_t)prefix->size_B;
         pkt.indirect = (struct tg_gpu_submit_indirect) {
            .address = ind->addr,
            .bytes = (uint32_t)ind->size_B,
            .suffix_offset = 0,
         };
         return pkt;

      case NVK_MME_XFB_COUNTER_LOAD:
         if (inline_words != 1 || indirect_words != 1 || method_count != 2) {
            pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
            return pkt;
         }
         pkt.consumed = 2;
         pkt.command_bytes = (uint32_t)prefix->size_B;
         pkt.indirect = (struct tg_gpu_submit_indirect) {
            .address = ind->addr,
            .bytes = (uint32_t)ind->size_B,
            .suffix_offset = 0,
         };
         return pkt;

      case NVK_MME_DISPATCH_INDIRECT: {
         if (inline_words != 0 || indirect_words != 3 || method_count != 8) {
            pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
            return pkt;
         }
         if (index + 2 >= count) {
            pkt.status = TG_WIN_EXEC_MISSING_CONTINUATION;
            return pkt;
         }
         const struct tg_win_exec_slice *suffix = &slices[index + 2];
         if (!tg_win_exec_slice_valid_bounds(suffix) ||
             suffix->incomplete || suffix->no_prefetch) {
            pkt.status = TG_WIN_EXEC_MISSING_CONTINUATION;
            return pkt;
         }
         if (suffix->size_B > TG_GPU_SUBMIT_MAX_BYTES - prefix->size_B) {
            pkt.status = TG_WIN_EXEC_OVERSIZED;
            return pkt;
         }
         pkt.consumed = 3;
         pkt.command_bytes = (uint32_t)(prefix->size_B + suffix->size_B);
         pkt.indirect = (struct tg_gpu_submit_indirect) {
            .address = ind->addr,
            .bytes = (uint32_t)ind->size_B,
            .suffix_offset = (uint32_t)prefix->size_B,
         };
         return pkt;
      }

      default:
         pkt.status = TG_WIN_EXEC_UNSUPPORTED_INDIRECT_MACRO;
         return pkt;
      }
   }

   pkt.status = TG_WIN_EXEC_MALFORMED_PREFIX;
   return pkt;
}

#endif
