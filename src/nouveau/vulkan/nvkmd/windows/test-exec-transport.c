/* SPDX-License-Identifier: MIT */
#include "exec_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define INCR(subc, method, count) \
   (0x20000000u | ((subc) << 13) | ((count) << 16) | ((method) >> 2))
#define ONEINCR(subc, method, count) \
   (0xa0000000u | ((subc) << 13) | ((count) << 16) | ((method) >> 2))

int
main(void)
{
   /* 1. Linear slice. */
   const struct tg_win_exec_slice linear = {
      .addr = 0x100000, .size_B = 64, .incomplete = false, .no_prefetch = false,
   };
   struct tg_win_exec_packet pkt = tg_win_exec_plan_step(&linear, 0, 1, NULL);
   assert(pkt.status == TG_WIN_EXEC_LINEAR);
   assert(pkt.consumed == 1 && pkt.command_bytes == 64);

   /* 2. DrawIndirect 2-slice pairing. */
   const uint32_t draw_prefix[] = {
      INCR(0, NVB06F_WFI, 1), NVB06F_WFI_SCOPE_ALL,
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDIRECT), 12),
      2, 1, /* 2 draws * (4 + 1) = 10 indirect words = 40 bytes */
   };
   const struct tg_win_exec_slice draw_slices[2] = {
      { .addr = 0x100000, .size_B = sizeof(draw_prefix), .incomplete = true },
      { .addr = 0x200000, .size_B = 40, .no_prefetch = true },
   };
   pkt = tg_win_exec_plan_step(draw_slices, 0, 2, draw_prefix);
   assert(pkt.status == TG_WIN_EXEC_INDIRECT);
   assert(pkt.consumed == 2 && pkt.mme_slot == NVK_MME_DRAW_INDIRECT);
   assert(pkt.command_bytes == sizeof(draw_prefix));
   assert(pkt.indirect.address == 0x200000 && pkt.indirect.bytes == 40 &&
          pkt.indirect.suffix_offset == 0);

   /* 3. DrawIndexedIndirect 2-slice pairing. */
   const uint32_t idx_prefix[] = {
      INCR(0, NVB06F_WFI, 1), NVB06F_WFI_SCOPE_ALL,
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW_INDEXED_INDIRECT), 7),
      1, 0,
   };
   const struct tg_win_exec_slice idx_slices[2] = {
      { .addr = 0x100000, .size_B = sizeof(idx_prefix), .incomplete = true },
      { .addr = 0x200100, .size_B = 20, .no_prefetch = true },
   };
   pkt = tg_win_exec_plan_step(idx_slices, 0, 2, idx_prefix);
   assert(pkt.status == TG_WIN_EXEC_INDIRECT &&
          pkt.mme_slot == NVK_MME_DRAW_INDEXED_INDIRECT && pkt.consumed == 2);

   /* 4. XfbDrawIndirect and XfbCounterLoad 2-slice pairing. */
   const uint32_t xfb_draw_prefix[] = {
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_XFB_DRAW_INDIRECT), 3), 4, 16,
   };
   const struct tg_win_exec_slice xfb_slices[2] = {
      { .addr = 0x100000, .size_B = sizeof(xfb_draw_prefix), .incomplete = true },
      { .addr = 0x200200, .size_B = 4, .no_prefetch = true },
   };
   pkt = tg_win_exec_plan_step(xfb_slices, 0, 2, xfb_draw_prefix);
   assert(pkt.status == TG_WIN_EXEC_INDIRECT &&
          pkt.mme_slot == NVK_MME_XFB_DRAW_INDIRECT && pkt.consumed == 2);

   const uint32_t xfb_load_prefix[] = {
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_XFB_COUNTER_LOAD), 2), 0,
   };
   const struct tg_win_exec_slice xfb_load_slices[2] = {
      { .addr = 0x100000, .size_B = sizeof(xfb_load_prefix), .incomplete = true },
      { .addr = 0x200200, .size_B = 4, .no_prefetch = true },
   };
   pkt = tg_win_exec_plan_step(xfb_load_slices, 0, 2, xfb_load_prefix);
   assert(pkt.status == TG_WIN_EXEC_INDIRECT &&
          pkt.mme_slot == NVK_MME_XFB_COUNTER_LOAD && pkt.consumed == 2);

   /* 5. DispatchIndirect 3-slice pairing. */
   const uint32_t disp_prefix[] = {
      INCR(0, NVB06F_WFI, 1), NVB06F_WFI_SCOPE_ALL,
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DISPATCH_INDIRECT), 8),
   };
   const struct tg_win_exec_slice disp_slices[3] = {
      { .addr = 0x100000, .size_B = sizeof(disp_prefix), .incomplete = true },
      { .addr = 0x200300, .size_B = 12, .no_prefetch = true },
      { .addr = 0x10000c, .size_B = 296 },
   };
   pkt = tg_win_exec_plan_step(disp_slices, 0, 3, disp_prefix);
   assert(pkt.status == TG_WIN_EXEC_INDIRECT &&
          pkt.mme_slot == NVK_MME_DISPATCH_INDIRECT);
   assert(pkt.consumed == 3);
   assert(pkt.command_bytes == sizeof(disp_prefix) + 296);
   assert(pkt.indirect.address == 0x200300 && pkt.indirect.bytes == 12 &&
          pkt.indirect.suffix_offset == sizeof(disp_prefix));

   /* 6. Reject malformed / unsupported sequences with specific diagnostics. */
   pkt = tg_win_exec_plan_step(disp_slices, 0, 2, disp_prefix);
   assert(pkt.status == TG_WIN_EXEC_MISSING_CONTINUATION);

   pkt = tg_win_exec_plan_step(draw_slices, 0, 1, draw_prefix);
   assert(pkt.status == TG_WIN_EXEC_MISSING_INDIRECT_SEGMENT);

   const struct tg_win_exec_slice orphan = {
      .addr = 0x300000, .size_B = 16, .no_prefetch = true,
   };
   pkt = tg_win_exec_plan_step(&orphan, 0, 1, NULL);
   assert(pkt.status == TG_WIN_EXEC_ORPHAN_NO_PREFETCH);

   const struct tg_win_exec_slice oversized = {
      .addr = 0x300000, .size_B = TG_GPU_SUBMIT_MAX_BYTES + 4,
   };
   pkt = tg_win_exec_plan_step(&oversized, 0, 1, NULL);
   assert(pkt.status == TG_WIN_EXEC_OVERSIZED);

   const uint32_t unsupported_prefix[] = {
      ONEINCR(0, NVB197_CALL_MME_MACRO(NVK_MME_DRAW), 5), 1,
   };
   const struct tg_win_exec_slice unsupported_slices[2] = {
      { .addr = 0x100000, .size_B = sizeof(unsupported_prefix), .incomplete = true },
      { .addr = 0x200000, .size_B = 16, .no_prefetch = true },
   };
   pkt = tg_win_exec_plan_step(unsupported_slices, 0, 2, unsupported_prefix);
   assert(pkt.status == TG_WIN_EXEC_UNSUPPORTED_INDIRECT_MACRO);

   assert(strlen(tg_win_exec_status_str(pkt.status)) > 0);
   puts("NVK Windows indirect exec transport tests passed");
   return 0;
}
