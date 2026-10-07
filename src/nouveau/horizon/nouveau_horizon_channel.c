/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_gm20b.h"
#include "nouveau_horizon_private.h"

#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#define NOUVEAU_HORIZON_FENCE_POLL_NS UINT64_C(50000000)
#define NOUVEAU_HORIZON_REPORT_CONFIRM_TIMEOUT_NS UINT64_C(10000000000)

_Static_assert(NOUVEAU_HORIZON_REPORT_SLICE_COUNT ==
                  NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT,
               "every inflight ledger slot must own one report slice");
_Static_assert(NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT < INT32_MAX,
               "mapped timeline comparison requires a sub-2^31 window");

static enum nouveau_horizon_status
nouveau_horizon_channel_check_error_locked(
   struct nouveau_horizon_channel *channel);

static enum nouveau_horizon_status
nouveau_horizon_device_scan_channel_errors(
   struct nouveau_horizon_device *device, uint32_t syncpoint_id,
   struct nouveau_horizon_channel *locked_channel);

static void
nouveau_horizon_reset_native_fence(NvFence *fence)
{
   fence->id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   fence->value = 0;
}

static int32_t
nouveau_horizon_timeout_ns_to_us(uint64_t timeout_ns)
{
   if (timeout_ns == UINT64_MAX)
      return -1;

   uint64_t timeout_us = timeout_ns / 1000;
   if (timeout_ns % 1000)
      timeout_us++;
   if (timeout_us > INT32_MAX)
      timeout_us = INT32_MAX;
   return (int32_t)timeout_us;
}

static enum nouveau_horizon_status
nouveau_horizon_fence_wait_impl(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_fence *fence, uint64_t timeout_ns,
   struct nouveau_horizon_channel *locked_channel)
{
   if (device == NULL || fence == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   if (!nouveau_horizon_fence_is_valid(fence))
      return NOUVEAU_HORIZON_SUCCESS;

   NvFence native = nouveau_horizon_native_fence(*fence);
   const uint64_t start_ns = os_time_get_nano();

   for (;;) {
      uint64_t wait_ns = NOUVEAU_HORIZON_FENCE_POLL_NS;
      if (timeout_ns != UINT64_MAX) {
         const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
         const uint64_t remaining_ns = elapsed_ns >= timeout_ns ?
                                          0 : timeout_ns - elapsed_ns;
         wait_ns = MIN2(wait_ns, remaining_ns);
      }

      const Result rc = nvFenceWait(
         &native, nouveau_horizon_timeout_ns_to_us(wait_ns));
      if (R_SUCCEEDED(rc))
         return NOUVEAU_HORIZON_SUCCESS;

      enum nouveau_horizon_status status =
         nouveau_horizon_status_from_result(
            rc, NOUVEAU_HORIZON_ERROR_DEVICE_LOST);
      if (status != NOUVEAU_HORIZON_ERROR_TIMEOUT) {
         nouveau_horizon_device_mark_lost(device);
         return status;
      }

      status = nouveau_horizon_device_scan_channel_errors(
         device, fence->id, locked_channel);
      if (status != NOUVEAU_HORIZON_SUCCESS)
         return status;
      if (nouveau_horizon_device_is_lost(device))
         return NOUVEAU_HORIZON_ERROR_DEVICE_LOST;

      if (timeout_ns != UINT64_MAX &&
          os_time_get_nano() - start_ns >= timeout_ns)
         return NOUVEAU_HORIZON_ERROR_TIMEOUT;
   }
}

static enum nouveau_horizon_status
nouveau_horizon_fence_wait_internal(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_fence *fence, uint64_t timeout_ns,
   struct nouveau_horizon_channel *locked_channel)
{
   const bool timing = device != NULL && device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   const enum nouveau_horizon_status status =
      nouveau_horizon_fence_wait_impl(
         device, fence, timeout_ns, locked_channel);
   if (timing) {
      nouveau_horizon_device_record_fence_wait(
         device, status, os_time_get_nano() - start_ns);
   }
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_fence_wait(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_fence *fence,
   uint64_t timeout_ns)
{
   return nouveau_horizon_fence_wait_internal(
      device, fence, timeout_ns, NULL);
}

static enum nouveau_horizon_status
nouveau_horizon_channel_latch_error(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_status status, Result native_result)
{
   if (channel->error.status == NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_error_set(&channel->error, status, native_result);
   } else if (status == NOUVEAU_HORIZON_ERROR_DEVICE_LOST &&
              channel->error.status != NOUVEAU_HORIZON_ERROR_DEVICE_LOST) {
      channel->error.status = status;
      if (channel->error.native_result == 0)
         channel->error.native_result = R_VALUE(native_result);
   }
   if (status == NOUVEAU_HORIZON_ERROR_DEVICE_LOST) {
      channel->lost = true;
      if (channel->device->total_order_channels)
         nouveau_horizon_device_mark_lost(channel->device);
   }
   return channel->error.status;
}

static const char *
nouveau_horizon_notification_name(uint32_t type)
{
   switch (type) {
   case 8: return "fifo idle timeout";
   case 13: return "graphics engine error (sw notify)";
   case 24: return "graphics semaphore timeout";
   case 25: return "graphics illegal notify";
   case 31: return "mmu fault";
   case 32: return "pbdma error";
   case 43: return "channel reset verification error";
   case 80: return "pushbuffer crc mismatch";
   default: return "unknown";
   }
}

static bool
nouveau_horizon_notification_query_is_empty(Result rc)
{
   /* libnx implements nvGpuChannelGetErrorNotification() as a nonblocking
    * eventWait(..., 0) followed by the notifier ioctl.  An unsignaled event
    * therefore reports KernelError_TimedOut; it is not a channel error.
    */
   return R_VALUE(rc) == R_VALUE(KERNELRESULT(TimedOut));
}

static enum nouveau_horizon_status
nouveau_horizon_channel_check_error_locked(
   struct nouveau_horizon_channel *channel)
{
   NvNotification notification = {0};
   NvError native_error = {0};
   const Result notification_rc = nvGpuChannelGetErrorNotification(
      &channel->gpu_channel, &notification);
   const bool notification_query_empty =
      nouveau_horizon_notification_query_is_empty(notification_rc);
   if (notification_query_empty)
      return channel->error.status;

   /* GetErrorInfo is a follow-up ioctl for a signalled error event.  Calling
    * it on a healthy channel is not a valid poll and retail Horizon reports
    * LibnxNvidiaError_IoctlFailed.
    */
   Result error_rc = 0;
   if (R_SUCCEEDED(notification_rc)) {
      error_rc = nvGpuChannelGetErrorInfo(
         &channel->gpu_channel, &native_error);
   }

   const bool query_failed =
      R_FAILED(notification_rc) || R_FAILED(error_rc);
   const bool has_notification =
      R_SUCCEEDED(notification_rc) && notification.status != 0;
   const bool has_error = R_SUCCEEDED(error_rc) && native_error.type != 0;
   if (!query_failed && !has_notification && !has_error)
      return channel->error.status;

   const bool first_native_report = !channel->native_error_logged;
   const Result query_rc = R_FAILED(notification_rc) ? notification_rc :
                           R_FAILED(error_rc) ? error_rc : 0;
   nouveau_horizon_channel_latch_error(
      channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, query_rc);

   channel->error.notification_timestamp = notification.timestamp;
   channel->error.notification_type = notification.info32;
   channel->error.notification_info = notification.info16;
   channel->error.notification_status = notification.status;
   channel->error.channel_error_type = native_error.type;
   memcpy(channel->error.channel_error_info, native_error.info,
          sizeof(channel->error.channel_error_info));

   if (first_native_report) {
      nouveau_horizon_log(
         channel->device, NOUVEAU_HORIZON_LOG_ERROR,
         "channel %llu lost: notification_rc=0x%x "
         "notification={type=%u (%s) info=%u status=%u} error_rc=0x%x "
         "error={type=%u info=[0x%x,0x%x,0x%x,0x%x]}",
         (unsigned long long)channel->id,
         R_VALUE(notification_rc),
         notification.info32, nouveau_horizon_notification_name(notification.info32),
         notification.info16, notification.status,
         R_VALUE(error_rc),
         native_error.type, native_error.info[0], native_error.info[1],
         native_error.info[2], native_error.info[3]);
      channel->native_error_logged = true;
   }
   channel->lost = true;
   return channel->error.status;
}

static enum nouveau_horizon_status
nouveau_horizon_device_scan_channel_errors(
   struct nouveau_horizon_device *device, uint32_t syncpoint_id,
   struct nouveau_horizon_channel *locked_channel)
{
   if (nouveau_horizon_device_is_lost(device))
      return NOUVEAU_HORIZON_ERROR_DEVICE_LOST;

   enum nouveau_horizon_status status = NOUVEAU_HORIZON_SUCCESS;
   simple_mtx_lock(&device->channel_mutex);
   list_for_each_entry(struct nouveau_horizon_channel, channel,
                       &device->channels, device_link) {
      if (!device->total_order_channels &&
          channel->syncpoint_id != syncpoint_id)
         continue;

      enum nouveau_horizon_status channel_status =
         NOUVEAU_HORIZON_SUCCESS;
      if (channel == locked_channel) {
         channel_status = nouveau_horizon_channel_check_error_locked(channel);
      } else if (mtx_trylock(&channel->mutex) == thrd_success) {
         channel_status = nouveau_horizon_channel_check_error_locked(channel);
         mtx_unlock(&channel->mutex);
      }

      if (channel_status == NOUVEAU_HORIZON_ERROR_DEVICE_LOST) {
         status = NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
         break;
      }
   }
   simple_mtx_unlock(&device->channel_mutex);

   if (nouveau_horizon_device_is_lost(device))
      return NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
   return status;
}

static enum nouveau_horizon_status
nouveau_horizon_fence_wait_locked(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_fence *fence, uint64_t timeout_ns)
{
   if (channel->error.status != NOUVEAU_HORIZON_SUCCESS)
      return channel->error.status;
   if (nouveau_horizon_device_is_lost(channel->device))
      return nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);

   uint32_t report_value = 0;
   bool has_report = false;
   if (channel->mapped_completion_enabled &&
       nouveau_horizon_fence_is_valid(fence) &&
       fence->id == channel->syncpoint_id) {
      for (uint32_t i = 0; i < channel->inflight_count; i++) {
         const uint32_t index =
            (channel->inflight_head + i) %
            NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
         const struct nouveau_horizon_fence *candidate =
            &channel->inflight_submissions[index].fence;
         if (candidate->id == fence->id && candidate->value == fence->value) {
            report_value = channel->inflight_submissions[index].report_value;
            has_report = true;
            break;
         }
      }
   }

   if (!has_report) {
      return nouveau_horizon_fence_wait_internal(
         channel->device, fence, timeout_ns, channel);
   }

   const bool timing = channel->device->enable_timing;
   const uint64_t overall_start_ns = os_time_get_nano();
   uint64_t poll_start_ns = timing ? overall_start_ns : 0;
   uint32_t completed =
      __atomic_load_n(channel->report_cpu, __ATOMIC_ACQUIRE);
   if (timing) {
      channel->stats.mapped_completion_polls++;
      const uint64_t elapsed_ns = os_time_get_nano() - poll_start_ns;
      channel->stats.mapped_completion_poll_ns += elapsed_ns;
      channel->stats.mapped_completion_poll_max_ns =
         MAX2(channel->stats.mapped_completion_poll_max_ns, elapsed_ns);
   }

   /* At most 512 values can be outstanding, so the signed-delta timeline
    * comparison remains unambiguous across uint32_t wrap.
    */
   if ((int32_t)(completed - report_value) >= 0) {
      if (timing)
         channel->stats.mapped_completion_hits++;
      return NOUVEAU_HORIZON_SUCCESS;
   }
   if (timeout_ns == 0)
      return NOUVEAU_HORIZON_ERROR_TIMEOUT;

   if (timing)
      channel->stats.mapped_completion_native_fallbacks++;
   enum nouveau_horizon_status status =
      nouveau_horizon_fence_wait_internal(
         channel->device, fence, timeout_ns, channel);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   poll_start_ns = timing ? os_time_get_nano() : 0;
   completed = __atomic_load_n(channel->report_cpu, __ATOMIC_ACQUIRE);
   if (timing) {
      channel->stats.mapped_completion_polls++;
      const uint64_t elapsed_ns = os_time_get_nano() - poll_start_ns;
      channel->stats.mapped_completion_poll_ns += elapsed_ns;
      channel->stats.mapped_completion_poll_max_ns =
         MAX2(channel->stats.mapped_completion_poll_max_ns, elapsed_ns);
   }
   if ((int32_t)(completed - report_value) >= 0) {
      if (timing)
         channel->stats.mapped_completion_hits++;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   /* The native syncpoint precedes the mapped report. Wait for both before
    * retiring the ledger slot or reusing its command slice.
    */
   if (timing)
      channel->stats.mapped_completion_report_lag_events++;
   const uint64_t lag_start_ns = os_time_get_nano();
   uint64_t last_error_poll_ns = lag_start_ns;
   for (;;) {
      poll_start_ns = timing ? os_time_get_nano() : 0;
      completed = __atomic_load_n(channel->report_cpu, __ATOMIC_ACQUIRE);
      if (timing) {
         channel->stats.mapped_completion_polls++;
         const uint64_t elapsed_ns = os_time_get_nano() - poll_start_ns;
         channel->stats.mapped_completion_poll_ns += elapsed_ns;
         channel->stats.mapped_completion_poll_max_ns =
            MAX2(channel->stats.mapped_completion_poll_max_ns, elapsed_ns);
      }
      if ((int32_t)(completed - report_value) >= 0) {
         if (timing)
            channel->stats.mapped_completion_hits++;
         status = NOUVEAU_HORIZON_SUCCESS;
         break;
      }

      const uint64_t now_ns = os_time_get_nano();
      if (timeout_ns != UINT64_MAX &&
          now_ns - overall_start_ns >= timeout_ns) {
         status = NOUVEAU_HORIZON_ERROR_TIMEOUT;
         break;
      }
      if (now_ns - lag_start_ns >=
          NOUVEAU_HORIZON_REPORT_CONFIRM_TIMEOUT_NS) {
         nouveau_horizon_log(
            channel->device, NOUVEAU_HORIZON_LOG_ERROR,
            "channel %llu native fence %u:%u signalled but mapped report "
            "%u did not arrive within %llu ms",
            (unsigned long long)channel->id, fence->id, fence->value,
            report_value,
            (unsigned long long)
               (NOUVEAU_HORIZON_REPORT_CONFIRM_TIMEOUT_NS / 1000000));
         status = nouveau_horizon_channel_latch_error(
            channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);
         break;
      }

      if (now_ns - last_error_poll_ns >= NOUVEAU_HORIZON_FENCE_POLL_NS) {
         status = nouveau_horizon_channel_check_error_locked(channel);
         if (status != NOUVEAU_HORIZON_SUCCESS)
            break;
         if (nouveau_horizon_device_is_lost(channel->device)) {
            status = nouveau_horizon_channel_latch_error(
               channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);
            break;
         }
         last_error_poll_ns = now_ns;
      }
      svcSleepThread(0);
   }

   if (timing) {
      const uint64_t elapsed_ns = os_time_get_nano() - lag_start_ns;
      channel->stats.mapped_completion_report_lag_wait_ns += elapsed_ns;
      channel->stats.mapped_completion_report_lag_max_wait_ns =
         MAX2(channel->stats.mapped_completion_report_lag_max_wait_ns,
              elapsed_ns);
   }
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_fence_wait(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_fence *fence, uint64_t timeout_ns)
{
   if (channel == NULL || fence == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   const enum nouveau_horizon_status status =
      nouveau_horizon_fence_wait_locked(channel, fence, timeout_ns);
   mtx_unlock(&channel->mutex);
   return status;
}

static void
nouveau_horizon_channel_clear_inflight_locked(
   struct nouveau_horizon_channel *channel)
{
   if (channel->device->enable_timing)
      channel->stats.inflight_retired_submissions += channel->inflight_count;
   channel->inflight_head = 0;
   channel->inflight_count = 0;
   channel->inflight_entries = 0;
   channel->inflight_command_bytes = 0;
   if (channel->device->enable_timing) {
      channel->stats.current_inflight_submissions = 0;
      channel->stats.current_inflight_entries = 0;
      channel->stats.current_inflight_command_bytes = 0;
   }
}

static void
nouveau_horizon_channel_retire_inflight_locked(
   struct nouveau_horizon_channel *channel, uint32_t retire_count)
{
   assert(retire_count <= channel->inflight_count);
   for (uint32_t i = 0; i < retire_count; i++) {
      const uint32_t index =
         (channel->inflight_head + i) %
         NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
      assert(channel->inflight_entries >=
             channel->inflight_submissions[index].entries);
      assert(channel->inflight_command_bytes >=
             channel->inflight_submissions[index].command_bytes);
      channel->inflight_entries -=
         channel->inflight_submissions[index].entries;
      channel->inflight_command_bytes -=
         channel->inflight_submissions[index].command_bytes;
   }

   channel->inflight_head =
      (channel->inflight_head + retire_count) %
      NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
   channel->inflight_count -= retire_count;
   if (channel->device->enable_timing) {
      channel->stats.inflight_retired_submissions += retire_count;
      channel->stats.current_inflight_submissions = channel->inflight_count;
      channel->stats.current_inflight_entries = channel->inflight_entries;
      channel->stats.current_inflight_command_bytes =
         channel->inflight_command_bytes;
   }
}

static void
nouveau_horizon_channel_record_inflight_locked(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_fence *fence,
   uint64_t entries, uint64_t command_bytes,
   uint32_t report_value, uint32_t report_slice)
{
   assert(channel->inflight_count <
          NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT);

   const uint32_t index =
      (channel->inflight_head + channel->inflight_count) %
      NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
   assert(!channel->mapped_completion_enabled || report_slice == index);
   channel->inflight_submissions[index].fence = *fence;
   channel->inflight_submissions[index].entries = entries;
   channel->inflight_submissions[index].command_bytes = command_bytes;
   channel->inflight_submissions[index].report_value = report_value;
   channel->inflight_submissions[index].report_slice = report_slice;
   channel->inflight_count++;
   channel->inflight_entries += entries;
   channel->inflight_command_bytes += command_bytes;
   if (channel->device->enable_timing) {
      channel->stats.peak_inflight_submissions =
         MAX2(channel->stats.peak_inflight_submissions,
              channel->inflight_count);
      channel->stats.peak_inflight_entries =
         MAX2(channel->stats.peak_inflight_entries,
              channel->inflight_entries);
      channel->stats.peak_inflight_command_bytes =
         MAX2(channel->stats.peak_inflight_command_bytes,
              channel->inflight_command_bytes);
      channel->stats.current_inflight_submissions = channel->inflight_count;
      channel->stats.current_inflight_entries = channel->inflight_entries;
      channel->stats.current_inflight_command_bytes =
         channel->inflight_command_bytes;
   }
}

static enum nouveau_horizon_status
nouveau_horizon_channel_poll_inflight_locked(
   struct nouveau_horizon_channel *channel)
{
   assert(channel->mapped_completion_enabled);
   if (channel->error.status != NOUVEAU_HORIZON_SUCCESS)
      return channel->error.status;
   if (nouveau_horizon_device_is_lost(channel->device))
      return nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);

   const bool timing = channel->device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   const uint32_t completed =
      __atomic_load_n(channel->report_cpu, __ATOMIC_ACQUIRE);
   if (timing) {
      channel->stats.inflight_proactive_polls++;
      channel->stats.mapped_completion_polls++;
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      channel->stats.mapped_completion_poll_ns += elapsed_ns;
      channel->stats.mapped_completion_poll_max_ns =
         MAX2(channel->stats.mapped_completion_poll_max_ns, elapsed_ns);
   }

   uint32_t retire_count = 0;
   while (retire_count < channel->inflight_count) {
      const uint32_t index =
         (channel->inflight_head + retire_count) %
         NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
      const uint32_t target =
         channel->inflight_submissions[index].report_value;
      if ((int32_t)(completed - target) < 0)
         break;
      retire_count++;
   }

   if (retire_count > 0) {
      nouveau_horizon_channel_retire_inflight_locked(
         channel, retire_count);
      if (timing) {
         channel->stats.inflight_proactive_retired_submissions +=
            retire_count;
         channel->stats.mapped_completion_hits++;
         channel->stats.mapped_completion_retired_submissions +=
            retire_count;
      }
   }
   return NOUVEAU_HORIZON_SUCCESS;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_poll_native_prefix_locked(
   struct nouveau_horizon_channel *channel, uint32_t retire_count,
   bool *retired_out)
{
   assert(!channel->mapped_completion_enabled);
   assert(retire_count > 0 && retire_count <= channel->inflight_count);
   assert(retired_out != NULL);
   *retired_out = false;

   /* nvFenceWait(0) uses a libnx event/ioctl. Poll only the last fence in
    * the required prefix; channel ordering proves the earlier entries
    * complete.
    */
   const uint32_t wait_index =
      (channel->inflight_head + retire_count - 1) %
      NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
   NvFence native = nouveau_horizon_native_fence(
      channel->inflight_submissions[wait_index].fence);
   const bool timing = channel->device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   if (timing) {
      channel->stats.inflight_proactive_polls++;
      channel->stats.inflight_native_pressure_polls++;
   }
   const Result rc = nvFenceWait(&native, 0);
   if (timing) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      channel->stats.inflight_native_pressure_poll_ns += elapsed_ns;
      channel->stats.inflight_native_pressure_poll_max_ns =
         MAX2(channel->stats.inflight_native_pressure_poll_max_ns,
              elapsed_ns);
   }
   if (R_SUCCEEDED(rc)) {
      nouveau_horizon_channel_retire_inflight_locked(channel, retire_count);
      if (timing) {
         channel->stats.inflight_proactive_retired_submissions +=
            retire_count;
         channel->stats.inflight_native_pressure_retired_submissions +=
            retire_count;
      }
      *retired_out = true;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   const enum nouveau_horizon_status status =
      nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_DEVICE_LOST);
   if (status == NOUVEAU_HORIZON_ERROR_TIMEOUT)
      return NOUVEAU_HORIZON_SUCCESS;

   if (nouveau_horizon_channel_check_error_locked(channel) !=
       NOUVEAU_HORIZON_SUCCESS)
      return channel->error.status;
   nouveau_horizon_device_mark_lost(channel->device);
   return nouveau_horizon_channel_latch_error(channel, status, rc);
}

static bool
nouveau_horizon_credit_exceeds(uint64_t current, uint64_t incoming,
                               uint64_t high)
{
   return high != 0 &&
          (incoming > high || current > high - incoming);
}

static bool
nouveau_horizon_credit_above_target(uint64_t current, uint64_t incoming,
                                    uint64_t target)
{
   return incoming > target || current > target - incoming;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_throttle_inflight_locked(
   struct nouveau_horizon_channel *channel,
   uint64_t incoming_entries, uint64_t incoming_command_bytes)
{
   enum nouveau_horizon_status status = NOUVEAU_HORIZON_SUCCESS;

   /* Mapped completion is a single uncached load, so keep reclaiming its
    * completed prefix proactively.  The native fallback is intentionally
    * deferred until the ledger reaches a real pressure boundary below.
    */
   if (channel->mapped_completion_enabled) {
      status = nouveau_horizon_channel_poll_inflight_locked(channel);
      if (status != NOUVEAU_HORIZON_SUCCESS)
         return status;
   }

   const bool submit_trigger =
      nouveau_horizon_credit_exceeds(
         channel->inflight_count, 1,
         channel->inflight_submit_high_watermark);
   const bool entry_trigger =
      nouveau_horizon_credit_exceeds(
         channel->inflight_entries, incoming_entries,
         channel->inflight_entry_high_watermark);
   const bool byte_trigger =
      nouveau_horizon_credit_exceeds(
         channel->inflight_command_bytes, incoming_command_bytes,
         channel->inflight_command_byte_high_watermark);

   if (!submit_trigger && !entry_trigger && !byte_trigger)
      return NOUVEAU_HORIZON_SUCCESS;

   /* Reclaim the oldest work down to the triggered low watermarks. Allow
    * one oversized batch after draining older work so limits cannot
    * deadlock submission.
    */
   const uint64_t submit_target = submit_trigger ?
      MAX2((uint64_t)channel->inflight_submit_low_watermark,
           UINT64_C(1)) :
      channel->inflight_submit_high_watermark;
   const uint64_t entry_target = entry_trigger ?
      MAX2(channel->inflight_entry_low_watermark, incoming_entries) :
      channel->inflight_entry_high_watermark;
   const uint64_t byte_target = byte_trigger ?
      MAX2(channel->inflight_command_byte_low_watermark,
           incoming_command_bytes) :
      channel->inflight_command_byte_high_watermark;

   const bool timing = channel->device->enable_timing;
   if (timing) {
      channel->stats.inflight_throttle_waits++;
      channel->stats.inflight_credit_waits++;
      if (submit_trigger)
         channel->stats.inflight_submission_watermark_waits++;
      if (entry_trigger)
         channel->stats.inflight_entry_watermark_waits++;
      if (byte_trigger)
         channel->stats.inflight_command_byte_watermark_waits++;
   }

   if (timing && channel->stats.inflight_credit_waits == 1) {
      nouveau_horizon_log(
         channel->device, NOUVEAU_HORIZON_LOG_INFO,
         "channel %llu credit throttle: current={sub=%u entries=%llu "
         "bytes=%llu} incoming={entries=%llu bytes=%llu} "
         "watermarks={sub=%u/%u entries=%llu/%llu bytes=%llu/%llu}",
         (unsigned long long)channel->id, channel->inflight_count,
         (unsigned long long)channel->inflight_entries,
         (unsigned long long)channel->inflight_command_bytes,
         (unsigned long long)incoming_entries,
         (unsigned long long)incoming_command_bytes,
         channel->inflight_submit_low_watermark,
         channel->inflight_submit_high_watermark,
         (unsigned long long)channel->inflight_entry_low_watermark,
         (unsigned long long)channel->inflight_entry_high_watermark,
         (unsigned long long)channel->inflight_command_byte_low_watermark,
         (unsigned long long)channel->inflight_command_byte_high_watermark);
   }

   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   uint32_t retire_count = 0;
   uint64_t projected_submissions = channel->inflight_count;
   uint64_t projected_entries = channel->inflight_entries;
   uint64_t projected_command_bytes = channel->inflight_command_bytes;
   while (retire_count < channel->inflight_count &&
          ((submit_trigger && nouveau_horizon_credit_above_target(
               projected_submissions, 1, submit_target)) ||
           (entry_trigger && nouveau_horizon_credit_above_target(
               projected_entries, incoming_entries, entry_target)) ||
           (byte_trigger && nouveau_horizon_credit_above_target(
               projected_command_bytes, incoming_command_bytes,
               byte_target)))) {
      const uint32_t index =
         (channel->inflight_head + retire_count) %
         NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
      projected_submissions--;
      projected_entries -= channel->inflight_submissions[index].entries;
      projected_command_bytes -=
         channel->inflight_submissions[index].command_bytes;
      retire_count++;
   }

   if (retire_count > 0) {
      bool retired_by_poll = false;
      if (!channel->mapped_completion_enabled) {
         status = nouveau_horizon_channel_poll_native_prefix_locked(
            channel, retire_count, &retired_by_poll);
      }

      /* One wait on the prefix's last fence covers all earlier entries on
       * this channel.
       */
      if (status == NOUVEAU_HORIZON_SUCCESS && !retired_by_poll) {
         const uint32_t wait_index =
            (channel->inflight_head + retire_count - 1) %
            NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT;
         const struct nouveau_horizon_fence fence =
            channel->inflight_submissions[wait_index].fence;
         status = nouveau_horizon_fence_wait_locked(
            channel, &fence,
            NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS);
         if (status == NOUVEAU_HORIZON_SUCCESS)
            nouveau_horizon_channel_retire_inflight_locked(
               channel, retire_count);
      }
   }

   if (timing) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      channel->stats.inflight_throttle_wait_ns += elapsed_ns;
      channel->stats.inflight_throttle_max_wait_ns =
         MAX2(channel->stats.inflight_throttle_max_wait_ns, elapsed_ns);
      channel->stats.inflight_credit_wait_ns += elapsed_ns;
      channel->stats.inflight_credit_max_wait_ns =
         MAX2(channel->stats.inflight_credit_max_wait_ns, elapsed_ns);
      if (submit_trigger)
         channel->stats.inflight_submission_watermark_wait_ns += elapsed_ns;
      if (entry_trigger)
         channel->stats.inflight_entry_watermark_wait_ns += elapsed_ns;
      if (byte_trigger)
         channel->stats.inflight_command_byte_watermark_wait_ns += elapsed_ns;
   }

   return status;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_reserve_entries_locked(
   struct nouveau_horizon_channel *channel, uint32_t entries,
   uint32_t tail_entries)
{
   const uint32_t total_order_entry =
      channel->device->total_order_channels ? 1 : 0;
   const uint32_t completion_entries =
      channel->mapped_completion_enabled ? 2 : 1;
   const uint64_t required =
      (uint64_t)channel->gpu_channel.num_entries + entries + tail_entries +
      completion_entries + total_order_entry;
   if (required > NOUVEAU_HORIZON_GPFIFO_SUBMIT_CAPACITY)
      return NOUVEAU_HORIZON_ERROR_NO_SPACE;
   return NOUVEAU_HORIZON_SUCCESS;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_append_locked(
   struct nouveau_horizon_channel *channel,
   uint64_t addr, uint32_t command_count, uint32_t flags,
   uint32_t tail_entries, bool consume_reserved)
{
   /* Check latched loss per entry. Reserve native error ioctls for
    * failures, waits, and explicit status queries.
    */
   if (channel->error.status != NOUVEAU_HORIZON_SUCCESS)
      return channel->error.status;
   if (nouveau_horizon_device_is_lost(channel->device))
      return nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);

   enum nouveau_horizon_status status;

   if (consume_reserved) {
      status = channel->gpu_channel.num_entries + 1 + tail_entries <=
                  NOUVEAU_HORIZON_GPFIFO_SUBMIT_CAPACITY ?
                  NOUVEAU_HORIZON_SUCCESS :
                  NOUVEAU_HORIZON_ERROR_NO_SPACE;
   } else {
      status = nouveau_horizon_channel_reserve_entries_locked(
         channel, 1, tail_entries);
   }
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   const Result rc = nvGpuChannelAppendEntry(&channel->gpu_channel,
                                              addr, command_count,
                                              flags, 0);
   if (R_FAILED(rc)) {
      if (channel->device->enable_timing)
         channel->stats.submit_failures++;
      nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, rc);
      nouveau_horizon_channel_check_error_locked(channel);
      return channel->error.status;
   }

   channel->pending_command_bytes +=
      (uint64_t)command_count * sizeof(uint32_t);

   return NOUVEAU_HORIZON_SUCCESS;
}

static uint32_t *
nouveau_horizon_channel_wait_slice_cpu(
   struct nouveau_horizon_channel *channel, uint32_t slice)
{
   return channel->builtin_cpu +
          slice * NOUVEAU_HORIZON_WAIT_SLICE_WORDS;
}

static uint64_t
nouveau_horizon_channel_wait_slice_addr(
   struct nouveau_horizon_channel *channel, uint32_t slice)
{
   return channel->builtin_addr +
          (uint64_t)slice * NOUVEAU_HORIZON_WAIT_SLICE_WORDS *
             sizeof(uint32_t);
}

static enum nouveau_horizon_status
nouveau_horizon_channel_reserve_wait_slice_locked(
   struct nouveau_horizon_channel *channel, bool total_order,
   uint32_t *slice_out)
{
   const uint32_t attempts = total_order ?
      1 : NOUVEAU_HORIZON_CLIENT_WAIT_SLICE_COUNT;
   for (uint32_t attempt = 0; attempt < attempts; attempt++) {
      const uint32_t slice = total_order ?
         NOUVEAU_HORIZON_ORDER_WAIT_SLICE : channel->next_wait_slice;
      if (!total_order) {
         channel->next_wait_slice =
            (channel->next_wait_slice + 1) %
            NOUVEAU_HORIZON_CLIENT_WAIT_SLICE_COUNT;
      }

      if (channel->pending_wait_slices & (UINT64_C(1) << slice))
         continue;

      NvFence *reuse_fence = &channel->wait_slice_fences[slice];
      if ((int32_t)reuse_fence->id >= 0) {
         const struct nouveau_horizon_fence public_fence =
            nouveau_horizon_public_fence(*reuse_fence);
         enum nouveau_horizon_status status =
            nouveau_horizon_fence_wait_locked(
               channel, &public_fence, UINT64_MAX);
         if (status != NOUVEAU_HORIZON_SUCCESS)
            return nouveau_horizon_channel_latch_error(channel, status, 0);
         nouveau_horizon_reset_native_fence(reuse_fence);
      }

      *slice_out = slice;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   return NOUVEAU_HORIZON_ERROR_NO_SPACE;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_append_waits_locked(
   struct nouveau_horizon_channel *channel,
   uint32_t wait_count,
   const struct nouveau_horizon_fence *waits,
   bool prepend)
{
   if (wait_count == 0)
      return NOUVEAU_HORIZON_SUCCESS;

   if (wait_count > NOUVEAU_HORIZON_WAIT_SLICE_WORDS /
                    NOUVEAU_HORIZON_GM20B_WAIT_WORDS_PER_FENCE)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   for (uint32_t i = 0; i < wait_count; i++) {
      if (!nouveau_horizon_fence_is_valid(&waits[i]) ||
          waits[i].id > 0x00ffffffu)
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   }

   uint32_t slice = 0;
   enum nouveau_horizon_status status =
      nouveau_horizon_channel_reserve_wait_slice_locked(
         channel, prepend, &slice);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   uint32_t *commands =
      nouveau_horizon_channel_wait_slice_cpu(channel, slice);
   const uint32_t command_count = nouveau_horizon_gm20b_build_waits(
      commands, wait_count, waits);

   const uint32_t original_entries = channel->gpu_channel.num_entries;
   status = nouveau_horizon_channel_append_locked(
      channel, nouveau_horizon_channel_wait_slice_addr(channel, slice),
      command_count,
      GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH,
      prepend ? 1 : 0, prepend);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   if (prepend && original_entries > 0) {
      const nvioctl_gpfifo_entry wait_entry =
         channel->gpu_channel.entries[original_entries];
      memmove(&channel->gpu_channel.entries[1],
              &channel->gpu_channel.entries[0],
              original_entries * sizeof(channel->gpu_channel.entries[0]));
      channel->gpu_channel.entries[0] = wait_entry;
   }

   channel->pending_wait_slices |= UINT64_C(1) << slice;
   channel->pending_work = true;
   if (channel->device->enable_timing)
      channel->stats.waits_enqueued += wait_count;
   return NOUVEAU_HORIZON_SUCCESS;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_emit_cache_acquire_locked(
   struct nouveau_horizon_channel *channel)
{
   if (channel->cache_acquire_emitted)
      return NOUVEAU_HORIZON_SUCCESS;

   enum nouveau_horizon_status status =
      nouveau_horizon_channel_append_locked(
         channel,
         channel->builtin_addr +
            4ull * channel->cache_acquire_offset_words,
         channel->cache_acquire_words,
         GPFIFO_ENTRY_NOT_MAIN, 2, false);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   status = nouveau_horizon_channel_append_locked(
      channel,
      channel->builtin_addr +
         4ull * channel->cache_acquire_sync_offset_words,
      1, GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH, 1, false);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   channel->cache_acquire_emitted = true;
   return NOUVEAU_HORIZON_SUCCESS;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_kickoff_locked(
   struct nouveau_horizon_channel *channel)
{
   const Result timeout =
      MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);
   const Result insufficient_memory =
      MAKERESULT(Module_LibnxNvidia,
                 LibnxNvidiaError_InsufficientMemory);
   const Result busy =
      MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Busy);
   const uint64_t recovery_start_ns = os_time_get_nano();
   uint32_t recovery_attempts = 0;
   bool log_recovery = false;
   Result rc = 0;

   for (;;) {
      const bool timing = channel->device->enable_timing;
      const uint64_t kickoff_start_ns = timing ? os_time_get_nano() : 0;
      rc = nvGpuChannelKickoff(&channel->gpu_channel);
      if (timing) {
         const uint64_t elapsed_ns = os_time_get_nano() - kickoff_start_ns;
         channel->stats.kickoff_calls++;
         channel->stats.kickoff_cpu_ns += elapsed_ns;
         channel->stats.kickoff_max_cpu_ns =
            MAX2(channel->stats.kickoff_max_cpu_ns, elapsed_ns);
      }
      if (R_SUCCEEDED(rc)) {
         if (log_recovery) {
            nouveau_horizon_log(
               channel->device, NOUVEAU_HORIZON_LOG_INFO,
               "channel %llu recovered native submission pressure after "
               "%u retries (%llu ms)",
               (unsigned long long)channel->id, recovery_attempts,
               (unsigned long long)
                  ((os_time_get_nano() - recovery_start_ns) / 1000000));
         }
         return NOUVEAU_HORIZON_SUCCESS;
      }

      if (timing)
         channel->stats.submit_failures++;
      if (nouveau_horizon_channel_check_error_locked(channel) !=
          NOUVEAU_HORIZON_SUCCESS)
         break;

      const bool resource_pressure =
         R_VALUE(rc) == R_VALUE(timeout) ||
         R_VALUE(rc) == R_VALUE(insufficient_memory) ||
         R_VALUE(rc) == R_VALUE(busy);
      if (!resource_pressure)
         break;

      const uint64_t elapsed_ns =
         os_time_get_nano() - recovery_start_ns;
      if (elapsed_ns >= NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS) {
         nouveau_horizon_log(
            channel->device, NOUVEAU_HORIZON_LOG_ERROR,
            "channel %llu native submission pressure did not clear after "
            "%u retries (%llu ms, result=0x%x)",
            (unsigned long long)channel->id, recovery_attempts,
            (unsigned long long)(elapsed_ns / 1000000), R_VALUE(rc));
         return nouveau_horizon_status_from_result(
            rc, NOUVEAU_HORIZON_ERROR_BUSY);
      }

      recovery_attempts++;
      if (timing)
         channel->stats.resource_retries++;
      if (recovery_attempts == 1) {
         log_recovery = true;
         nouveau_horizon_log(
            channel->device, NOUVEAU_HORIZON_LOG_WARNING,
            "channel %llu hit native submission pressure (result=0x%x); "
            "retiring oldest accepted work and retrying "
            "(current={sub=%u entries=%llu bytes=%llu})",
            (unsigned long long)channel->id, R_VALUE(rc),
            channel->inflight_count,
            (unsigned long long)channel->inflight_entries,
            (unsigned long long)channel->inflight_command_bytes);
      }

      /* Failed kickoff retains unsent fence increments, so
       * nvGpuChannelGetFence() is unsuitable for recovery. Reclaim the
       * oldest accepted batch and retry.
       */
      if (channel->inflight_count > 0) {
         for (;;) {
            const uint64_t recovery_elapsed_ns =
               os_time_get_nano() - recovery_start_ns;
            if (recovery_elapsed_ns >=
                NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS) {
               nouveau_horizon_log(
                  channel->device, NOUVEAU_HORIZON_LOG_ERROR,
                  "channel %llu oldest-fence recovery did not clear "
                  "native pressure after %u retries (%llu ms, "
                  "result=0x%x)",
                  (unsigned long long)channel->id, recovery_attempts,
                  (unsigned long long)(recovery_elapsed_ns / 1000000),
                  R_VALUE(rc));
               return nouveau_horizon_status_from_result(
                  rc, NOUVEAU_HORIZON_ERROR_BUSY);
            }

            const uint64_t wait_ns = MIN2(
               NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS -
                  recovery_elapsed_ns,
               channel->resource_wait_slice_ns);
            const struct nouveau_horizon_fence oldest =
               channel->inflight_submissions[
                  channel->inflight_head].fence;
            if (timing)
               channel->stats.resource_recovery_waits++;
            const uint64_t wait_start_ns = timing ? os_time_get_nano() : 0;
            const enum nouveau_horizon_status wait_status =
               nouveau_horizon_fence_wait_locked(
                  channel, &oldest, wait_ns);
            if (timing) {
               const uint64_t wait_elapsed_ns =
                  os_time_get_nano() - wait_start_ns;
               channel->stats.resource_recovery_wait_ns += wait_elapsed_ns;
               channel->stats.resource_recovery_max_wait_ns =
                  MAX2(channel->stats.resource_recovery_max_wait_ns,
                       wait_elapsed_ns);
            }

            if (wait_status == NOUVEAU_HORIZON_SUCCESS) {
               nouveau_horizon_channel_retire_inflight_locked(channel, 1);
               break;
            }
            if (wait_status != NOUVEAU_HORIZON_ERROR_TIMEOUT)
               return nouveau_horizon_channel_latch_error(
                  channel, wait_status, 0);
         }
      } else {
         /* No accepted fence is available to release a credit.  Preserve the
          * retained failed batch and make a bounded, non-spinning retry after
          * allowing the native channel worker to advance.
          */
         const uint64_t sleep_ns = MIN2(
            NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS - elapsed_ns,
            channel->resource_wait_slice_ns);
         if (timing)
            channel->stats.resource_recovery_waits++;
         const uint64_t wait_start_ns = timing ? os_time_get_nano() : 0;
         os_time_sleep(MAX2((int64_t)(sleep_ns / 1000), INT64_C(1)));
         if (timing) {
            const uint64_t wait_elapsed_ns =
               os_time_get_nano() - wait_start_ns;
            channel->stats.resource_recovery_wait_ns += wait_elapsed_ns;
            channel->stats.resource_recovery_max_wait_ns =
               MAX2(channel->stats.resource_recovery_max_wait_ns,
                    wait_elapsed_ns);
         }
         if (nouveau_horizon_channel_check_error_locked(channel) !=
             NOUVEAU_HORIZON_SUCCESS)
            break;
      }
   }

   nouveau_horizon_channel_latch_error(
      channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, rc);
   nouveau_horizon_channel_check_error_locked(channel);
   return channel->error.status;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_submit_locked(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out)
{
   if (fence_out != NULL) {
      fence_out->id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
      fence_out->value = 0;
   }

   if (nouveau_horizon_device_is_lost(channel->device))
      return nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);

   if (channel->error.status != NOUVEAU_HORIZON_SUCCESS)
      return channel->error.status;

   enum nouveau_horizon_status status = NOUVEAU_HORIZON_SUCCESS;

   /* Upgrade a prior lightweight queue fence when a later wait-idle needs a
    * CPU-visible cache-clean completion.
    */
   if (!channel->pending_work && channel->last_fence_valid &&
       completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU &&
       !channel->last_fence_cpu_visible)
      channel->pending_work = true;

   if (!channel->pending_work) {
      if (fence_out != NULL && channel->last_fence_valid)
         *fence_out = channel->last_fence;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   struct nouveau_horizon_device *device = channel->device;
   if (device->total_order_channels) {
      const uint64_t lock_start_ns = device->enable_timing ?
         os_time_get_nano() : 0;
      simple_mtx_lock(&device->submit_mutex);
      if (device->enable_timing) {
         const uint64_t elapsed_ns = os_time_get_nano() - lock_start_ns;
         channel->stats.submit_lock_wait_ns += elapsed_ns;
         channel->stats.submit_lock_max_wait_ns =
            MAX2(channel->stats.submit_lock_max_wait_ns, elapsed_ns);
      }
   }

   if (device->total_order_channels && device->global_fence_valid &&
       device->global_channel_id != channel->id) {
      /* Check producer errors before inheriting a cross-channel fence.
       * Single-channel submissions rely on error polling at kickoff
       * failures, waits, and status queries.
       */
      status = nouveau_horizon_device_scan_channel_errors(
         device, NOUVEAU_HORIZON_INVALID_FENCE_ID, channel);
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         nouveau_horizon_channel_latch_error(channel, status, 0);
         goto done;
      }

      status = nouveau_horizon_channel_append_waits_locked(
         channel, 1, &device->global_fence, true);
      if (status != NOUVEAU_HORIZON_SUCCESS)
         goto done;
   }

   const uint32_t fence_offset =
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU ?
         channel->cpu_fence_offset_words :
         channel->gpu_fence_offset_words;
   const uint32_t fence_words =
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU ?
         channel->cpu_fence_words : channel->gpu_fence_words;

   const bool mapped = channel->mapped_completion_enabled;
   const uint32_t tail_entries = mapped ? 2 : 1;
   const uint64_t incoming_entries =
      (uint64_t)channel->gpu_channel.num_entries + tail_entries;
   const uint64_t incoming_command_bytes =
      channel->pending_command_bytes +
      (uint64_t)fence_words * sizeof(uint32_t) +
      (mapped ?
         (uint64_t)NOUVEAU_HORIZON_GM20B_REPORT_WORDS *
            sizeof(uint32_t) : 0);

   /* Consume the tail space reserved while recording commands. */
   if (incoming_entries > NOUVEAU_HORIZON_GPFIFO_SUBMIT_CAPACITY) {
      status = NOUVEAU_HORIZON_ERROR_NO_SPACE;
      goto done;
   }
   status = nouveau_horizon_channel_throttle_inflight_locked(
      channel, incoming_entries, incoming_command_bytes);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;
   assert(channel->inflight_count <
          NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT);

   uint32_t report_slice = UINT32_MAX;
   uint32_t report_value = 0;
   if (mapped) {
      report_slice =
         (channel->inflight_head + channel->inflight_count) %
         NOUVEAU_HORIZON_REPORT_SLICE_COUNT;
      report_value = channel->next_report_value + 1;
      uint32_t *report_commands =
         channel->builtin_cpu + channel->report_slices_offset_words +
         report_slice * NOUVEAU_HORIZON_GM20B_REPORT_WORDS;
      const uint32_t report_words = nouveau_horizon_gm20b_build_report(
         report_commands, channel->report_addr, report_value);
      assert(report_words == NOUVEAU_HORIZON_GM20B_REPORT_WORDS);
   }

   status = nouveau_horizon_channel_append_locked(
      channel, channel->builtin_addr + 4ull * fence_offset,
      fence_words, GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH,
      mapped ? 1 : 0, true);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   if (mapped) {
      const uint64_t report_commands_addr =
         channel->builtin_addr +
         (uint64_t)(channel->report_slices_offset_words +
                    report_slice * NOUVEAU_HORIZON_GM20B_REPORT_WORDS) *
            sizeof(uint32_t);
      status = nouveau_horizon_channel_append_locked(
         channel, report_commands_addr,
         NOUVEAU_HORIZON_GM20B_REPORT_WORDS,
         GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH,
         0, true);
      if (status != NOUVEAU_HORIZON_SUCCESS)
         goto done;
   }

   nvGpuChannelIncrFence(&channel->gpu_channel);
   if (completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU)
      nvGpuChannelIncrFence(&channel->gpu_channel);

   const uint32_t submitted_entries = channel->gpu_channel.num_entries;
   const uint64_t submitted_command_bytes =
      channel->pending_command_bytes;
   status = nouveau_horizon_channel_kickoff_locked(channel);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   NvFence native_fence;
   nvGpuChannelGetFence(&channel->gpu_channel, &native_fence);
   channel->last_fence = nouveau_horizon_public_fence(native_fence);
   channel->last_fence_valid =
      nouveau_horizon_fence_is_valid(&channel->last_fence);
   channel->last_fence_cpu_visible =
      channel->last_fence_valid &&
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU;
   /* Require a valid native completion for every accepted batch, even
    * without mapped reports, before publishing ownership or recycling
    * command storage.
    */
   if (!channel->last_fence_valid) {
      status = nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);
      goto done;
   }
   if (channel->last_fence_valid)
      nouveau_horizon_channel_record_inflight_locked(
         channel, &channel->last_fence, submitted_entries,
         submitted_command_bytes, report_value, report_slice);
   if (mapped)
      channel->next_report_value = report_value;

   for (uint32_t slice = 0;
        slice < NOUVEAU_HORIZON_WAIT_SLICE_COUNT; slice++) {
      if (!(channel->pending_wait_slices & (UINT64_C(1) << slice)))
         continue;
      channel->wait_slice_fences[slice] = native_fence;
   }
   channel->pending_wait_slices = 0;

   if (device->enable_timing) {
      channel->stats.submissions++;
      channel->stats.submitted_entries += submitted_entries;
      channel->stats.submitted_dwords += channel->pending_dwords;
   }
   channel->pending_dwords = 0;
   channel->pending_command_bytes = 0;
   channel->pending_work = false;
   channel->cache_acquire_emitted = false;

   if (device->total_order_channels && channel->last_fence_valid) {
      device->global_fence = channel->last_fence;
      device->global_fence_valid = true;
      device->global_channel_id = channel->id;
   }

   if (fence_out != NULL && channel->last_fence_valid)
      *fence_out = channel->last_fence;

done:
   if (device->total_order_channels)
      simple_mtx_unlock(&device->submit_mutex);
   return status;
}

static NvChannelPriority
nouveau_horizon_channel_native_priority(
   enum nouveau_horizon_channel_priority priority)
{
   switch (priority) {
   case NOUVEAU_HORIZON_CHANNEL_PRIORITY_LOW:
      return NvChannelPriority_Low;
   case NOUVEAU_HORIZON_CHANNEL_PRIORITY_HIGH:
      return NvChannelPriority_High;
   case NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM:
   default:
      return NvChannelPriority_Medium;
   }
}

static enum nouveau_horizon_status
nouveau_horizon_channel_init_credits(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_channel_create_info *create_info)
{
   const enum nouveau_horizon_mapped_completion_mode mapped_mode =
      create_info != NULL ? create_info->mapped_completion_mode :
                            NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED;
   const uint32_t submit_high =
      create_info != NULL &&
      create_info->inflight_submit_high_watermark != 0 ?
         create_info->inflight_submit_high_watermark :
         NOUVEAU_HORIZON_DEFAULT_SUBMIT_HIGH_WATERMARK;
   const uint32_t submit_low =
      create_info != NULL &&
      create_info->inflight_submit_low_watermark != 0 ?
         create_info->inflight_submit_low_watermark : submit_high / 2;
   const uint64_t entry_high = create_info != NULL ?
      create_info->inflight_entry_high_watermark : 0;
   const uint64_t entry_low = entry_high != 0 ?
      (create_info != NULL &&
       create_info->inflight_entry_low_watermark != 0 ?
          create_info->inflight_entry_low_watermark : entry_high / 2) : 0;
   const uint64_t byte_high = create_info != NULL ?
      create_info->inflight_command_byte_high_watermark : 0;
   const uint64_t byte_low = byte_high != 0 ?
      (create_info != NULL &&
       create_info->inflight_command_byte_low_watermark != 0 ?
          create_info->inflight_command_byte_low_watermark : byte_high / 2) :
      0;
   const uint64_t wait_slice_ns =
      create_info != NULL && create_info->resource_wait_slice_ns != 0 ?
         create_info->resource_wait_slice_ns :
         NOUVEAU_HORIZON_DEFAULT_RESOURCE_WAIT_SLICE_NS;

   if ((mapped_mode != NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED &&
        mapped_mode !=
           NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0) ||
       submit_high == 0 ||
       submit_high > NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT ||
       submit_low >= submit_high ||
       (entry_high == 0 && create_info != NULL &&
        create_info->inflight_entry_low_watermark != 0) ||
       (entry_high != 0 && entry_low >= entry_high) ||
       (byte_high == 0 && create_info != NULL &&
        create_info->inflight_command_byte_low_watermark != 0) ||
       (byte_high != 0 && byte_low >= byte_high) ||
       wait_slice_ns > NOUVEAU_HORIZON_MAX_RESOURCE_WAIT_SLICE_NS)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   channel->inflight_submit_high_watermark = submit_high;
   channel->inflight_submit_low_watermark = submit_low;
   channel->inflight_entry_high_watermark = entry_high;
   channel->inflight_entry_low_watermark = entry_low;
   channel->inflight_command_byte_high_watermark = byte_high;
   channel->inflight_command_byte_low_watermark = byte_low;
   channel->resource_wait_slice_ns = wait_slice_ns;
   channel->mapped_completion_enabled =
      mapped_mode ==
      NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0;
   return NOUVEAU_HORIZON_SUCCESS;
}

enum nouveau_horizon_status
nouveau_horizon_channel_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_channel_create_info *create_info,
   struct nouveau_horizon_channel **channel_out)
{
   if (device == NULL || channel_out == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   *channel_out = NULL;
   struct nouveau_horizon_channel *channel =
      CALLOC_STRUCT(nouveau_horizon_channel);
   if (channel == NULL)
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;

   channel->refcnt = 1;
   channel->device = nouveau_horizon_device_ref(device);
   if (mtx_init(&channel->mutex, mtx_plain) != thrd_success) {
      nouveau_horizon_device_put(channel->device);
      FREE(channel);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }
   list_inithead(&channel->device_link);
   nouveau_horizon_error_reset(&channel->error);
   channel->last_fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   for (uint32_t i = 0; i < NOUVEAU_HORIZON_WAIT_SLICE_COUNT; i++)
      nouveau_horizon_reset_native_fence(&channel->wait_slice_fences[i]);

   enum nouveau_horizon_status status =
      nouveau_horizon_channel_init_credits(channel, create_info);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_channel;

   struct nouveau_horizon_memory_create_info memory_info = {
      .size_B = NOUVEAU_HORIZON_BUILTIN_SIZE_B,
      .align_B = NOUVEAU_HORIZON_BIND_ALIGN_B,
       .backing_kind = NvKind_Pitch,
       .flags = NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE |
                NOUVEAU_HORIZON_MEMORY_ZERO,
       .layout = {
          .valid = true,
          .pte_kind = NvKind_Pitch,
          .tile_mode = 0,
       },
    };
   status = nouveau_horizon_memory_create(
      device, &memory_info, &channel->builtin_memory);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_channel;

   struct nouveau_horizon_va_create_info va_info = {
      .size_B = nouveau_horizon_memory_get_size(channel->builtin_memory),
      .align_B = NOUVEAU_HORIZON_BIND_ALIGN_B,
   };
   status = nouveau_horizon_va_create(device, &va_info,
                                      &channel->builtin_va);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_memory;

   status = nouveau_horizon_va_bind(
      channel->builtin_va, 0, channel->builtin_memory, 0,
      nouveau_horizon_memory_get_size(channel->builtin_memory));
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_va;

   void *builtin_map = NULL;
   status = nouveau_horizon_memory_map(channel->builtin_memory, &builtin_map);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_va;
   channel->builtin_cpu = builtin_map;
   channel->builtin_addr = nouveau_horizon_va_get_addr(channel->builtin_va);

   const enum nouveau_horizon_channel_priority priority =
      create_info != NULL ? create_info->priority :
                            NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM;
   const Result rc = nvGpuChannelCreate(
      &channel->gpu_channel, &device->addr_space,
      nouveau_horizon_channel_native_priority(priority));
   if (R_FAILED(rc)) {
      status = nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
      goto fail_va;
   }
   channel->channel_ready = true;

   uint32_t *commands = channel->builtin_cpu +
      NOUVEAU_HORIZON_WAIT_SLICE_COUNT *
         NOUVEAU_HORIZON_WAIT_SLICE_WORDS;
   channel->cache_acquire_offset_words = commands - channel->builtin_cpu;
   channel->cache_acquire_words =
      nouveau_horizon_gm20b_build_cache_acquire(commands);
   commands += channel->cache_acquire_words;

   channel->cache_acquire_sync_offset_words = commands -
                                               channel->builtin_cpu;
   *commands++ = 0;

   channel->full_barrier_offset_words = commands - channel->builtin_cpu;
   channel->full_barrier_words =
      nouveau_horizon_gm20b_build_full_barrier(commands);
   commands += channel->full_barrier_words;

   channel->full_barrier_acquire_offset_words =
      commands - channel->builtin_cpu;
   channel->full_barrier_acquire_words =
      nouveau_horizon_gm20b_build_full_barrier_acquire(commands);
   commands += channel->full_barrier_acquire_words;

   channel->syncpoint_id =
      nvGpuChannelGetSyncpointId(&channel->gpu_channel);
   channel->gpu_fence_offset_words = commands - channel->builtin_cpu;
   channel->gpu_fence_words = nouveau_horizon_gm20b_build_fence(
      commands, channel->syncpoint_id, NOUVEAU_HORIZON_COMPLETION_GPU);
   commands += channel->gpu_fence_words;

   channel->cpu_fence_offset_words = commands - channel->builtin_cpu;
   channel->cpu_fence_words = nouveau_horizon_gm20b_build_fence(
      commands, channel->syncpoint_id, NOUVEAU_HORIZON_COMPLETION_CPU);
   commands += channel->cpu_fence_words;

   /* Always reserve the report layout so an ABI-constrained adapter can
    * declare its 3D engine immediately after channel construction.  Storage
    * remains inert unless mapped completion is explicitly enabled.
    */
   const uint32_t report_offset_words = ALIGN_POT(
      (uint32_t)(commands - channel->builtin_cpu),
      NOUVEAU_HORIZON_REPORT_ALIGN_WORDS);
   commands = channel->builtin_cpu + report_offset_words;
   channel->report_offset_words = report_offset_words;
   channel->report_cpu = commands;
   channel->report_addr = channel->builtin_addr +
                          (uint64_t)report_offset_words * sizeof(uint32_t);
   *commands++ = 0;

   channel->report_slices_offset_words = ALIGN_POT(
      (uint32_t)(commands - channel->builtin_cpu),
      NOUVEAU_HORIZON_REPORT_ALIGN_WORDS);
   commands = channel->builtin_cpu + channel->report_slices_offset_words;
   commands += NOUVEAU_HORIZON_REPORT_SLICE_COUNT *
               NOUVEAU_HORIZON_GM20B_REPORT_WORDS;

   if ((uint64_t)(commands - channel->builtin_cpu) * sizeof(uint32_t) >
       nouveau_horizon_memory_get_size(channel->builtin_memory)) {
      status = NOUVEAU_HORIZON_ERROR_SYSTEM;
      goto fail_native;
   }

   simple_mtx_lock(&device->submit_mutex);
   channel->id = device->next_channel_id++;
   simple_mtx_unlock(&device->submit_mutex);

   simple_mtx_lock(&device->channel_mutex);
   list_addtail(&channel->device_link, &device->channels);
   channel->registered = true;
   simple_mtx_unlock(&device->channel_mutex);

   nouveau_horizon_log(
      device, NOUVEAU_HORIZON_LOG_INFO,
      "channel %llu credits: submissions=%u/%u entries=%llu/%llu "
       "command-bytes=%llu/%llu pressure-slice=%llu us mapped-3d=%u",
      (unsigned long long)channel->id,
      channel->inflight_submit_low_watermark,
      channel->inflight_submit_high_watermark,
      (unsigned long long)channel->inflight_entry_low_watermark,
      (unsigned long long)channel->inflight_entry_high_watermark,
      (unsigned long long)channel->inflight_command_byte_low_watermark,
      (unsigned long long)channel->inflight_command_byte_high_watermark,
       (unsigned long long)(channel->resource_wait_slice_ns / 1000),
       channel->mapped_completion_enabled);

   *channel_out = channel;
   return NOUVEAU_HORIZON_SUCCESS;

fail_native:
   nvGpuChannelClose(&channel->gpu_channel);
   channel->channel_ready = false;
fail_va:
   nouveau_horizon_va_put(channel->builtin_va);
fail_memory:
   nouveau_horizon_memory_put(channel->builtin_memory);
fail_channel:
   mtx_destroy(&channel->mutex);
   nouveau_horizon_device_put(channel->device);
   FREE(channel);
   return status;
}

struct nouveau_horizon_channel *
nouveau_horizon_channel_ref(struct nouveau_horizon_channel *channel)
{
   if (channel != NULL)
      p_atomic_inc(&channel->refcnt);
   return channel;
}

static void
nouveau_horizon_channel_log_perf_locked(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_status teardown_status)
{
   if (!channel->device->enable_timing)
      return;

   const struct nouveau_horizon_channel_stats *stats = &channel->stats;
   nouveau_horizon_log(
      channel->device, NOUVEAU_HORIZON_LOG_INFO,
      "channel %llu perf: status=%s mapped=%u submissions=%llu "
      "inflight={peak-sub=%llu peak-entries=%llu peak-bytes=%llu "
      "credit-waits=%llu wait-us=%llu max-us=%llu} "
      "mapped-completion={polls=%llu hits=%llu retired=%llu "
      "native-fallbacks=%llu report-lag=%llu poll-us=%llu max-us=%llu} "
      "native-pressure={polls=%llu retired=%llu poll-us=%llu max-us=%llu} "
      "cpu-us={exec=%llu submit=%llu kickoff=%llu lock-wait=%llu}",
      (unsigned long long)channel->id,
      nouveau_horizon_status_string(teardown_status),
      channel->mapped_completion_enabled,
      (unsigned long long)stats->submissions,
      (unsigned long long)stats->peak_inflight_submissions,
      (unsigned long long)stats->peak_inflight_entries,
      (unsigned long long)stats->peak_inflight_command_bytes,
      (unsigned long long)stats->inflight_credit_waits,
      (unsigned long long)(stats->inflight_credit_wait_ns / 1000),
      (unsigned long long)(stats->inflight_credit_max_wait_ns / 1000),
      (unsigned long long)stats->mapped_completion_polls,
      (unsigned long long)stats->mapped_completion_hits,
      (unsigned long long)stats->mapped_completion_retired_submissions,
      (unsigned long long)stats->mapped_completion_native_fallbacks,
      (unsigned long long)stats->mapped_completion_report_lag_events,
      (unsigned long long)(stats->mapped_completion_poll_ns / 1000),
      (unsigned long long)(stats->mapped_completion_poll_max_ns / 1000),
      (unsigned long long)stats->inflight_native_pressure_polls,
      (unsigned long long)
         stats->inflight_native_pressure_retired_submissions,
      (unsigned long long)(stats->inflight_native_pressure_poll_ns / 1000),
      (unsigned long long)
         (stats->inflight_native_pressure_poll_max_ns / 1000),
      (unsigned long long)(stats->exec_cpu_ns / 1000),
      (unsigned long long)(stats->submit_cpu_ns / 1000),
      (unsigned long long)(stats->kickoff_cpu_ns / 1000),
      (unsigned long long)(stats->channel_lock_wait_ns / 1000));
}

enum nouveau_horizon_channel_put_result
nouveau_horizon_channel_put(struct nouveau_horizon_channel *channel)
{
   if (channel == NULL)
      return NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE;
   if (!p_atomic_dec_zero(&channel->refcnt))
      return NOUVEAU_HORIZON_CHANNEL_PUT_RETAINED;

   mtx_lock(&channel->mutex);
   if (channel->channel_ready) {
      struct nouveau_horizon_fence fence;
      enum nouveau_horizon_status status =
         nouveau_horizon_channel_submit_locked(
            channel, NOUVEAU_HORIZON_COMPLETION_CPU, &fence);
      if (status == NOUVEAU_HORIZON_SUCCESS &&
          nouveau_horizon_fence_is_valid(&fence))
         status = nouveau_horizon_fence_wait_locked(
            channel, &fence, UINT64_MAX);
      if (status == NOUVEAU_HORIZON_SUCCESS) {
         nouveau_horizon_channel_clear_inflight_locked(channel);
         status = nouveau_horizon_channel_check_error_locked(channel);
      }

      nouveau_horizon_channel_log_perf_locked(channel, status);

      if (status != NOUVEAU_HORIZON_SUCCESS) {
         channel->quarantined = true;
         nouveau_horizon_device_mark_lost(channel->device);
         nouveau_horizon_log(
            channel->device, NOUVEAU_HORIZON_LOG_ERROR,
            "channel %llu teardown has unknown completion (%s); "
            "quarantining channel, command VA and backing storage",
            (unsigned long long)channel->id,
            nouveau_horizon_status_string(status));
         mtx_unlock(&channel->mutex);
         return NOUVEAU_HORIZON_CHANNEL_PUT_QUARANTINED;
      }

      nvGpuChannelClose(&channel->gpu_channel);
      channel->channel_ready = false;
   }

   if (channel->registered) {
      simple_mtx_lock(&channel->device->channel_mutex);
      list_delinit(&channel->device_link);
      channel->registered = false;
      simple_mtx_unlock(&channel->device->channel_mutex);
   }
   mtx_unlock(&channel->mutex);
   mtx_destroy(&channel->mutex);

   nouveau_horizon_va_put(channel->builtin_va);
   nouveau_horizon_memory_put(channel->builtin_memory);
   nouveau_horizon_device_put(channel->device);
   FREE(channel);
   return NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE;
}

enum nouveau_horizon_status
nouveau_horizon_channel_bind_zcull(
   struct nouveau_horizon_channel *channel, uint64_t addr)
{
   if (channel == NULL || addr == 0)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   if (channel->error.status != NOUVEAU_HORIZON_SUCCESS ||
       nouveau_horizon_device_is_lost(channel->device)) {
      const enum nouveau_horizon_status status =
         channel->error.status != NOUVEAU_HORIZON_SUCCESS ?
            channel->error.status : NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
      mtx_unlock(&channel->mutex);
      return status;
   }
   const Result rc = nvGpuChannelZcullBind(&channel->gpu_channel, addr);
   enum nouveau_horizon_status status =
      nouveau_horizon_status_from_result(rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
   if (R_FAILED(rc))
      status = nouveau_horizon_channel_latch_error(channel, status, rc);
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_enqueue_waits(
   struct nouveau_horizon_channel *channel,
   uint32_t wait_count,
   const struct nouveau_horizon_fence *waits)
{
   if (channel == NULL || (wait_count > 0 && waits == NULL))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_append_waits_locked(
         channel, wait_count, waits, false);
   mtx_unlock(&channel->mutex);
   return status;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_full_barrier_locked(
   struct nouveau_horizon_channel *channel)
{
   /* GM20B host-WFI requires separate GPFIFO entries for SET_REFERENCE and
    * the following 3D no-op, as in deko3D's full barrier.
    */
   enum nouveau_horizon_status status =
      nouveau_horizon_channel_reserve_entries_locked(channel, 3, 0);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   status = nouveau_horizon_channel_append_locked(
      channel,
      channel->builtin_addr +
         4ull * channel->full_barrier_offset_words,
      channel->full_barrier_words, GPFIFO_ENTRY_NOT_MAIN, 2, true);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   status = nouveau_horizon_channel_append_locked(
      channel,
      channel->builtin_addr +
         4ull * channel->full_barrier_acquire_offset_words,
      channel->full_barrier_acquire_words, GPFIFO_ENTRY_NOT_MAIN, 1,
      true);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   status = nouveau_horizon_channel_append_locked(
      channel,
      channel->builtin_addr +
         4ull * channel->cache_acquire_sync_offset_words,
      1, GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH, 0, true);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   channel->pending_dwords +=
      channel->full_barrier_words +
      channel->full_barrier_acquire_words + 1;
   channel->pending_work = true;
   channel->cache_acquire_emitted = true;
   if (channel->device->enable_timing)
      channel->stats.full_barriers_enqueued++;

 done:
   return status;
}

static enum nouveau_horizon_status
nouveau_horizon_channel_exec_locked(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs)
{
   const uint32_t acquire_entries = channel->cache_acquire_emitted ? 0 : 2;
   enum nouveau_horizon_status status =
      nouveau_horizon_channel_reserve_entries_locked(
         channel, exec_count + acquire_entries, 0);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   status = nouveau_horizon_channel_emit_cache_acquire_locked(channel);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto done;

   for (uint32_t i = 0; i < exec_count; i++) {
      uint32_t flags = GPFIFO_ENTRY_NOT_MAIN;
      if (execs[i].no_prefetch)
         flags |= GPFIFO_ENTRY_NO_PREFETCH;

      status = nouveau_horizon_channel_append_locked(
         channel, execs[i].addr, execs[i].size_B / 4,
         flags, execs[i].incomplete ? 1 : 0, false);
      if (status != NOUVEAU_HORIZON_SUCCESS)
         goto done;

      channel->pending_dwords += execs[i].size_B / 4;
      channel->pending_work = true;
   }

 done:
   return status;
}

static bool
nouveau_horizon_channel_execs_valid(
   uint32_t exec_count, const struct nouveau_horizon_exec *execs)
{
   if (exec_count > GPFIFO_QUEUE_SIZE ||
       (exec_count > 0 && execs == NULL))
      return false;

   for (uint32_t i = 0; i < exec_count; i++) {
      if ((execs[i].addr & 3) != 0 || execs[i].size_B == 0 ||
          (execs[i].size_B & 3) != 0 ||
          (execs[i].incomplete && i + 1 == exec_count))
         return false;
   }

   return true;
}

static bool
nouveau_horizon_completion_mode_valid(
   enum nouveau_horizon_completion_mode completion_mode)
{
   return completion_mode == NOUVEAU_HORIZON_COMPLETION_GPU ||
          completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU;
}

enum nouveau_horizon_status
nouveau_horizon_channel_full_barrier(
   struct nouveau_horizon_channel *channel)
{
   if (channel == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_full_barrier_locked(channel);
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_exec(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs)
{
   if (channel == NULL ||
       !nouveau_horizon_channel_execs_valid(exec_count, execs))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   if (exec_count == 0)
      return NOUVEAU_HORIZON_SUCCESS;

   const bool timing = channel->device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   const uint64_t lock_start_ns = start_ns;
   mtx_lock(&channel->mutex);
   const uint64_t locked_ns = timing ? os_time_get_nano() : 0;
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_exec_locked(channel, exec_count, execs);
   if (timing) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      const uint64_t lock_wait_ns = locked_ns - lock_start_ns;
      channel->stats.exec_calls++;
      channel->stats.exec_cpu_ns += elapsed_ns;
      channel->stats.exec_max_cpu_ns =
         MAX2(channel->stats.exec_max_cpu_ns, elapsed_ns);
      channel->stats.channel_lock_wait_ns += lock_wait_ns;
      channel->stats.channel_lock_max_wait_ns =
         MAX2(channel->stats.channel_lock_max_wait_ns, lock_wait_ns);
   }
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_exec_submit(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs,
   bool full_barrier,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out)
{
   if (channel == NULL ||
       !nouveau_horizon_channel_execs_valid(exec_count, execs) ||
       !nouveau_horizon_completion_mode_valid(completion_mode))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   if (fence_out != NULL) {
      fence_out->id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
      fence_out->value = 0;
   }

   const bool timing = channel->device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   const uint64_t lock_start_ns = start_ns;
   mtx_lock(&channel->mutex);
   const uint64_t locked_ns = timing ? os_time_get_nano() : 0;

   enum nouveau_horizon_status status = NOUVEAU_HORIZON_SUCCESS;
   uint64_t exec_done_ns = locked_ns;
   /* Reserve execs and the optional three-entry barrier together.
    * Otherwise capacity failure could leave an accepted prefix without its
    * ordering barrier.
    */
   const uint32_t acquire_entries =
      exec_count > 0 && !channel->cache_acquire_emitted ? 2 : 0;
   const uint32_t barrier_entries = full_barrier ? 3 : 0;
   if (exec_count > 0 || full_barrier) {
      status = nouveau_horizon_channel_reserve_entries_locked(
         channel, exec_count + acquire_entries + barrier_entries, 0);
   }
   if (exec_count > 0) {
      if (status == NOUVEAU_HORIZON_SUCCESS) {
         status = nouveau_horizon_channel_exec_locked(
            channel, exec_count, execs);
      }
      exec_done_ns = timing ? os_time_get_nano() : 0;
   }

   if (status == NOUVEAU_HORIZON_SUCCESS && full_barrier)
      status = nouveau_horizon_channel_full_barrier_locked(channel);

   const bool submit_attempted = status == NOUVEAU_HORIZON_SUCCESS;
   const uint64_t submit_start_ns = timing && submit_attempted ?
      os_time_get_nano() : 0;
   if (submit_attempted) {
      status = nouveau_horizon_channel_submit_locked(
         channel, completion_mode, fence_out);
   }

   if (timing) {
      const uint64_t lock_wait_ns = locked_ns - lock_start_ns;
      if (exec_count > 0) {
         const uint64_t exec_elapsed_ns = exec_done_ns - start_ns;
         channel->stats.exec_calls++;
         channel->stats.exec_cpu_ns += exec_elapsed_ns;
         channel->stats.exec_max_cpu_ns =
            MAX2(channel->stats.exec_max_cpu_ns, exec_elapsed_ns);
      }
      if (submit_attempted) {
         const uint64_t submit_elapsed_ns =
            os_time_get_nano() - submit_start_ns;
         channel->stats.submit_calls++;
         channel->stats.submit_cpu_ns += submit_elapsed_ns;
         channel->stats.submit_max_cpu_ns =
            MAX2(channel->stats.submit_max_cpu_ns, submit_elapsed_ns);
      }
      channel->stats.channel_lock_wait_ns += lock_wait_ns;
      channel->stats.channel_lock_max_wait_ns =
         MAX2(channel->stats.channel_lock_max_wait_ns, lock_wait_ns);
   }

   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_submit(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out)
{
   if (channel == NULL ||
       !nouveau_horizon_completion_mode_valid(completion_mode))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   const bool timing = channel->device->enable_timing;
   const uint64_t start_ns = timing ? os_time_get_nano() : 0;
   const uint64_t lock_start_ns = start_ns;
   mtx_lock(&channel->mutex);
   const uint64_t locked_ns = timing ? os_time_get_nano() : 0;
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_submit_locked(channel, completion_mode,
                                             fence_out);
   if (timing) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      const uint64_t lock_wait_ns = locked_ns - lock_start_ns;
      channel->stats.submit_calls++;
      channel->stats.submit_cpu_ns += elapsed_ns;
      channel->stats.submit_max_cpu_ns =
         MAX2(channel->stats.submit_max_cpu_ns, elapsed_ns);
      channel->stats.channel_lock_wait_ns += lock_wait_ns;
      channel->stats.channel_lock_max_wait_ns =
         MAX2(channel->stats.channel_lock_max_wait_ns, lock_wait_ns);
   }
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_set_mapped_completion(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_mapped_completion_mode mode)
{
   if (channel == NULL ||
       (mode != NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED &&
        mode !=
           NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   enum nouveau_horizon_status status = NOUVEAU_HORIZON_SUCCESS;
   if (channel->pending_work || channel->pending_dwords != 0 ||
       channel->pending_command_bytes != 0 ||
       channel->gpu_channel.num_entries != 0 ||
       channel->pending_wait_slices != 0 ||
       channel->inflight_count != 0 || channel->last_fence_valid) {
      status = NOUVEAU_HORIZON_ERROR_BUSY;
   } else if (channel->error.status != NOUVEAU_HORIZON_SUCCESS) {
      status = channel->error.status;
   } else if (nouveau_horizon_device_is_lost(channel->device)) {
      status = nouveau_horizon_channel_latch_error(
         channel, NOUVEAU_HORIZON_ERROR_DEVICE_LOST, 0);
   } else {
      channel->mapped_completion_enabled =
         mode ==
         NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0;
   }
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_wait_idle(
   struct nouveau_horizon_channel *channel, uint64_t timeout_ns)
{
   if (channel == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   struct nouveau_horizon_fence fence;
   enum nouveau_horizon_status status =
      nouveau_horizon_channel_submit_locked(
         channel, NOUVEAU_HORIZON_COMPLETION_CPU, &fence);
   if (status == NOUVEAU_HORIZON_SUCCESS &&
       nouveau_horizon_fence_is_valid(&fence))
      status = nouveau_horizon_fence_wait_locked(
         channel, &fence, timeout_ns);
   if (status == NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_channel_clear_inflight_locked(channel);
      status = nouveau_horizon_channel_check_error_locked(channel);
   } else if (status != NOUVEAU_HORIZON_ERROR_TIMEOUT) {
      nouveau_horizon_channel_latch_error(channel, status, 0);
   }
   mtx_unlock(&channel->mutex);
   return status;
}

enum nouveau_horizon_status
nouveau_horizon_channel_get_error(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_error *error_out)
{
   if (channel == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   mtx_lock(&channel->mutex);
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_check_error_locked(channel);
   if (error_out != NULL)
      *error_out = channel->error;
   mtx_unlock(&channel->mutex);
   return status;
}

void
nouveau_horizon_channel_get_stats(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_channel_stats *stats_out)
{
   if (channel == NULL || stats_out == NULL)
      return;

   mtx_lock(&channel->mutex);
   *stats_out = channel->stats;
   stats_out->current_inflight_submissions = channel->inflight_count;
   stats_out->current_inflight_entries = channel->inflight_entries;
   stats_out->current_inflight_command_bytes =
      channel->inflight_command_bytes;
   mtx_unlock(&channel->mutex);
}
