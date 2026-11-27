/* SPDX-License-Identifier: MIT
 *
 * Queries. Results become available when the queue fence passes the
 * submission that recorded the query; polling them never blocks.
 */
#include "tgd3d.h"

#include <string.h>

namespace tgd3d {

static inline Query *Q(D3D10DDI_HQUERY h) { return static_cast<Query *>(h.pDrvPrivate); }

static bool pass_query(const Query *q)
{
   return q->type == D3D10DDI_QUERY_OCCLUSION || q->type == D3D10DDI_QUERY_OCCLUSIONPREDICATE ||
          q->type == D3D10DDI_QUERY_PIPELINESTATS || q->type == D3D11DDI_QUERY_PIPELINESTATS;
}

static const VkQueryPipelineStatisticFlags statistics =
   VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
   VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
   VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

/* Recording/GetData cannot report allocation errors to the runtime. Once a
 * query has lost part of its result, the device must stop; neither retrying
 * Begin nor returning an incomplete sum can repair that command stream. */
static void query_error(Query *q, HRESULT cause)
{
   q->error = cause;
   trace_error(cause, __FILE__, __LINE__);
   TG_ERROR(q->dev, D3DDDIERR_DEVICEREMOVED);
}

static bool query_usable(Query *q)
{
   if (SUCCEEDED(q->dev->lost) && SUCCEEDED(q->error))
      return true;
   TG_ERROR(q->dev, D3DDDIERR_DEVICEREMOVED);
   return false;
}

static HRESULT pool_create(Device *dev, Query *q)
{
   VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
   if (q->type == D3D10DDI_QUERY_TIMESTAMP) {
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = 1;
   } else if (q->type == D3D10DDI_QUERY_PIPELINESTATS || q->type == D3D11DDI_QUERY_PIPELINESTATS) {
      info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      info.queryCount = QUERY_SLOTS;
      info.pipelineStatistics = statistics;
      if (q->type == D3D11DDI_QUERY_PIPELINESTATS) {
         info.pipelineStatistics |= VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
         if (dev->caps->tessellationShader)
            info.pipelineStatistics |= VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
                                       VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
      }
   } else {
      info.queryType = VK_QUERY_TYPE_OCCLUSION;
      info.queryCount = QUERY_SLOTS;
   }
   VkQueryPool pool = VK_NULL_HANDLE;
   VkResult result = dev->vk.CreateQueryPool(dev->vkdev, &info, nullptr, &pool);
   if (result != VK_SUCCESS)
      return resource_result(result);
   dev->vk.ResetQueryPool(dev->vkdev, pool, 0, info.queryCount);
   q->pool = pool;
   return S_OK;
}

void query_pass_begin(Device *dev, VkCommandBuffer cmd, bool compute)
{
   for (Query *q : dev->activeQueries) {
      if (compute && q->type != D3D11DDI_QUERY_PIPELINESTATS)
         continue;
      if (!query_usable(q))
         return;
      if (q->used == QUERY_SLOTS) {
         VkQueryPool previous = q->pool;
         try {
            q->fullPools.push_back(previous);
         } catch (const std::bad_alloc &) {
            query_error(q, E_OUTOFMEMORY);
            return;
         }
         HRESULT hr = pool_create(dev, q);
         if (FAILED(hr)) {
            q->fullPools.pop_back(); /* q->pool still owns the previous pool */
            query_error(q, hr);
            return;
         }
         q->used = 0;
      }
      const VkQueryControlFlags flags = q->type == D3D10DDI_QUERY_OCCLUSION &&
         dev->caps->occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
      dev->vk.CmdBeginQuery(cmd, q->pool, q->used, flags);
      q->inPass = true;
   }
}

void query_pass_end(Device *dev, VkCommandBuffer cmd)
{
   for (Query *q : dev->activeQueries) {
      if (!q->inPass)
         continue;
      dev->vk.CmdEndQuery(cmd, q->pool, q->used++);
      q->inPass = false;
   }
}

static SIZE_T APIENTRY CalcPrivateQuerySize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *)
{
   return sizeof(Query);
}

static void APIENTRY CreateQuery(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEQUERY *args,
                                 D3D10DDI_HQUERY handle, D3D10DDI_HRTQUERY)
{
   Device *dev = D(hDevice);
   if (FAILED(dev->lost)) {
      TG_ERROR(dev, D3DDDIERR_DEVICEREMOVED);
      return;
   }
   Query *q = new (handle.pDrvPrivate) Query();
   q->dev = dev;
   q->type = args->Query;
   q->misc = args->MiscFlags;
   q->state = QueryState::Idle;
   q->pool = VK_NULL_HANDLE;
   q->seq = 0;
   q->used = 0;
   q->inPass = false;
   HRESULT hr = S_OK;
   switch (q->type) {
   case D3D10DDI_QUERY_EVENT:
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
      break;
   case D3D10DDI_QUERY_TIMESTAMP:
      if (!dev->caps->timestampValidBits)
         hr = E_NOTIMPL;
      else
         hr = pool_create(dev, q);
      break;
   case D3D11DDI_QUERY_PIPELINESTATS:
   case D3D10DDI_QUERY_PIPELINESTATS:
      if (!dev->caps->pipelineStatistics) {
         hr = E_NOTIMPL;
         break;
      }
      /* fallthrough */
   case D3D10DDI_QUERY_OCCLUSION:
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
      hr = pool_create(dev, q);
      break;
   default:
      /* Stream-output statistics and performance counters are not offered. */
      hr = E_NOTIMPL;
      break;
   }
   if (FAILED(hr)) {
      /* Runtime invalidates a failed handle and never calls DestroyQuery.
       * Unsupported query/capability paths remain failures, not fake data
       * or a fictitious OOM. Only allocation failure is recoverable here. */
      q->~Query();
      trace_error(hr, __FILE__, __LINE__);
      TG_ERROR(dev, hr == E_OUTOFMEMORY ? hr : D3DDDIERR_DEVICEREMOVED);
   }
}

static void query_deactivate(Device *dev, Query *q)
{
   for (size_t i = 0; i < dev->activeQueries.size(); i++) {
      if (dev->activeQueries[i] == q) {
         end_rendering(dev);     /* closes the slot opened for the current pass */
         dev->activeQueries.erase(dev->activeQueries.begin() + i);
         return;
      }
   }
}

/* Copy group dimensions in GPU execution order. GM20B's older resident
 * indirect macro can execute the dispatch correctly while miscounting shader
 * invocations. No CPU-side argument read or queue drain is needed at dispatch. */
bool capture_indirect_invocations(Device *dev, VkCommandBuffer cmd, Resource *arguments, UINT offset, UINT groupSize)
{
   bool recorded = false;
   for (Query *q : dev->activeQueries) {
      if (q->type != D3D11DDI_QUERY_PIPELINESTATS) continue;
      Slice captured{};
      HRESULT hr = slice_alloc(dev, 3 * sizeof(UINT), 16, &captured);
      if (FAILED(hr)) { query_error(q, hr); return false; }
      try {
         q->indirectInvocations.push_back({captured, groupSize});
      } catch (const std::bad_alloc &) {
         slice_release(dev, captured, 0);
         query_error(q, E_OUTOFMEMORY);
         return false;
      }
      VkBufferCopy copy{arguments->slice.offset + offset, captured.offset, 3 * sizeof(UINT)};
      dev->vk.CmdCopyBuffer(cmd, arguments->slice.buffer(), captured.buffer(), 1, &copy);
      recorded = true;
   }
   if (recorded) {
      // A dispatch can also write its argument buffer through a UAV. Complete
      // the transfer read before the shader starts, even for aliased bindings.
      buffer_written(dev);
      command_barrier(dev);
   }
   return true;
}

static void release_invocations(Device *dev, Query *q)
{
   for (auto &sample : q->indirectInvocations) slice_release(dev, sample.counts, dev->lastSeq());
   q->indirectInvocations.clear();
   q->explicitInvocations = 0;
}

static void APIENTRY DestroyQuery(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   query_deactivate(dev, q);
   release_invocations(dev, q);
   defer(dev, DeferredType::QueryPool, (uint64_t)q->pool, dev->lastSeq());
   for (VkQueryPool pool : q->fullPools)
      defer(dev, DeferredType::QueryPool, (uint64_t)pool, dev->lastSeq());
   q->~Query();
}

static void APIENTRY QueryBegin(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   if (!query_usable(q))
      return;
   if (!pass_query(q)) {
      q->state = QueryState::Active;
      return;
   }
   query_deactivate(dev, q);
   release_invocations(dev, q);
   for (VkQueryPool pool : q->fullPools)
      defer(dev, DeferredType::QueryPool, (uint64_t)pool, dev->lastSeq());
   q->fullPools.clear();
   if (q->used) {
      /* Earlier slots may still be written by queued commands. */
      VkQueryPool previous = q->pool;
      HRESULT hr = pool_create(dev, q);
      if (FAILED(hr)) {
         query_error(q, hr);
         return;
      }
      defer(dev, DeferredType::QueryPool, (uint64_t)previous, dev->lastSeq());
   }
   q->used = 0;
   q->state = QueryState::Active;
   end_rendering(dev);            /* the next pass opens a slot for this query */
   try {
      dev->activeQueries.push_back(q);
   } catch (const std::bad_alloc &) {
      query_error(q, E_OUTOFMEMORY);
   }
}

static void APIENTRY QueryEnd(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   if (!query_usable(q))
      return;
   /* End without Begin is a new empty query, not the previous result. */
   if (pass_query(q) && q->state != QueryState::Active) {
      QueryBegin(hDevice, handle);
      if (FAILED(dev->lost))
         return;
   }
   /* Deferred clears precede this event/timestamp in the D3D command stream. */
   flush_pending_clears(dev, nullptr);
   if (FAILED(dev->lost))
      return;
   if (pass_query(q)) {
      query_deactivate(dev, q);
   } else if (q->type == D3D10DDI_QUERY_TIMESTAMP && q->pool) {
      if (q->seq) {
         VkQueryPool previous = q->pool;
         HRESULT hr = pool_create(dev, q);
         if (FAILED(hr)) {
            query_error(q, hr);
            return;
         }
         defer(dev, DeferredType::QueryPool, (uint64_t)previous, dev->lastSeq());
      }
      VkCommandBuffer cmd = cmd_begin(dev);
      if (!cmd)
         return;
      dev->vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, q->pool, 0);
   }
   /* Ready once everything recorded so far has executed. */
   q->seq = dev->cmd && dev->cmdHasWork ? dev->recordingSeq() : dev->submitted;
   q->state = QueryState::Ended;
}

static void APIENTRY QueryGetData(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY handle, VOID *data,
                                  UINT size, UINT flags)
{
   Device *dev = D(hDevice);
   Query *q = Q(handle);
   if (!query_usable(q))
      return;
   if (q->seq > dev->submitted && !(flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH)) {
      HRESULT hr = flush(dev, false);
      if (FAILED(dev->lost))
         return;
      if (FAILED(hr)) { query_error(q, hr); return; }
   }
   const bool done = q->state == QueryState::Ended && sequence_done(dev, q->seq);
   /* completed() can discover removal. Do not overwrite it with pending. */
   if (FAILED(dev->lost))
      return;
   if (!done) {
      /* Not an error: the runtime turns this into S_FALSE for the caller. */
      if (dev->um.pfnSetErrorCb)
         dev->um.pfnSetErrorCb(dev->hRTCoreLayer, DXGI_DDI_ERR_WASSTILLDRAWING);
      return;
   }
   if (!data)
      return;
   switch (q->type) {
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT: {
      D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {UINT64_C(1000000000), FALSE};
      if (size >= sizeof(disjoint))
         memcpy(data, &disjoint, sizeof(disjoint));
      break;
   }
   case D3D10DDI_QUERY_EVENT:
      if (size >= sizeof(BOOL))
         *static_cast<BOOL *>(data) = TRUE;
      break;
   case D3D10DDI_QUERY_TIMESTAMP: {
      uint64_t ticks = 0;
      VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, q->pool, 0, 1, sizeof(ticks),
                                                    &ticks, sizeof(ticks), VK_QUERY_RESULT_64_BIT);
      if (result != VK_SUCCESS) {
         TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
         return;
      }
      const uint64_t ns = (uint64_t)((double)ticks * dev->caps->timestampPeriod);
      if (size >= sizeof(ns))
         memcpy(data, &ns, sizeof(ns));
      break;
   }
   case D3D10DDI_QUERY_OCCLUSION:
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE: {
      uint64_t total = 0;
      for (size_t p = 0; p <= q->fullPools.size(); p++) {
         const bool full = p < q->fullPools.size();
         const uint32_t count = full ? QUERY_SLOTS : q->used;
         if (!count) continue;
         uint64_t slots[QUERY_SLOTS];
         VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, full ? q->fullPools[p] : q->pool,
            0, count, count * sizeof(slots[0]), slots, sizeof(slots[0]), VK_QUERY_RESULT_64_BIT);
         if (result != VK_SUCCESS) {
            TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
            return;
         }
         for (uint32_t i = 0; i < count; i++) total += slots[i];
      }
      if (q->type == D3D10DDI_QUERY_OCCLUSION) {
         if (size >= sizeof(total))
            memcpy(data, &total, sizeof(total));
      } else if (size >= sizeof(BOOL)) {
         *static_cast<BOOL *>(data) = total != 0;
      }
      break;
   }
   case D3D10DDI_QUERY_PIPELINESTATS:
   case D3D11DDI_QUERY_PIPELINESTATS: {
      const bool modern = q->type == D3D11DDI_QUERY_PIPELINESTATS;
      const bool tess = modern && dev->caps->tessellationShader;
      const UINT counters = tess ? 11 : modern ? 9 : 8;
      D3D11_DDI_QUERY_DATA_PIPELINE_STATISTICS out = {};
      out.CSInvocations = q->explicitInvocations;
      // QueryGetData has checked q->seq against the actual completed fence.
      for (const auto &sample : q->indirectInvocations) {
         HRESULT hr = slice_map(dev, sample.counts);
         if (FAILED(hr)) { query_error(q, hr); return; }
         uint32_t groups[3];
         memcpy(groups, sample.counts.cpu(), sizeof(groups));
         out.CSInvocations += uint64_t(groups[0]) * groups[1] * groups[2] * sample.groupSize;
      }
      static_assert(offsetof(D3D11_DDI_QUERY_DATA_PIPELINE_STATISTICS, HSInvocations) ==
                    sizeof(D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS));
      for (size_t p = 0; p <= q->fullPools.size(); p++) {
         const bool full = p < q->fullPools.size();
         const uint32_t count = full ? QUERY_SLOTS : q->used;
         if (!count) continue;
         uint64_t slots[QUERY_SLOTS * 11];
         VkResult result = dev->vk.GetQueryPoolResults(dev->vkdev, full ? q->fullPools[p] : q->pool,
            0, count, size_t(count) * counters * sizeof(uint64_t), slots, counters * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
         if (result != VK_SUCCESS) {
            TG_ERROR(dev, result == VK_NOT_READY ? DXGI_DDI_ERR_WASSTILLDRAWING : DXGI_ERROR_DEVICE_REMOVED);
            return;
         }
         for (uint32_t i = 0; i < count; i++) {
            /* Vulkan returns the enabled statistics in bit order. */
            const uint64_t *v = &slots[size_t(i) * counters];
            out.IAVertices += v[0];
            out.IAPrimitives += v[1];
            out.VSInvocations += v[2];
            out.GSInvocations += v[3];
            out.GSPrimitives += v[4];
            out.CInvocations += v[5];
            out.CPrimitives += v[6];
            out.PSInvocations += v[7];
            if (tess) {
               out.HSInvocations += v[8];
               out.DSInvocations += v[9];
            }
            if (modern) out.CSInvocations += v[counters - 1];
         }
      }
      const size_t bytes = modern ? sizeof(out) : sizeof(D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS);
      if (size >= bytes) memcpy(data, &out, bytes);
      break;
   }
   default:
      break;
   }
}

static void APIENTRY CheckCounterInfo(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO *info)
{
   memset(info, 0, sizeof(*info));     /* no device-dependent counters */
}

static void APIENTRY CheckCounter(D3D10DDI_HDEVICE hDevice, D3D10DDI_QUERY, D3D10DDI_COUNTER_TYPE *,
                                  UINT *, LPSTR, UINT *, LPSTR, UINT *, LPSTR, UINT *)
{
   TG_ERROR(D(hDevice), DXGI_ERROR_UNSUPPORTED);
}

void query_fill_table(D3D10DDI_DEVICEFUNCS *t)
{
   t->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   t->pfnCreateQuery = CreateQuery;
   t->pfnDestroyQuery = DestroyQuery;
   t->pfnQueryBegin = QueryBegin;
   t->pfnQueryEnd = QueryEnd;
   t->pfnQueryGetData = QueryGetData;
   t->pfnCheckCounterInfo = CheckCounterInfo;
   t->pfnCheckCounter = CheckCounter;
}

}
