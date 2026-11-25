/* SPDX-License-Identifier: MIT */
#include "dvfs.h"
#include "hardware.h"
#include "platform_io.h"
#include "work_queue.h"

#define TG_DVFS_POLL_INTERVAL_US     250000ULL
#define TG_DVFS_IDLE_HOLDOFF_US      200000ULL
#define TG_DVFS_BURST_WINDOW_US       60000ULL
#define TG_DVFS_HEAVY_SLICE_US         4000ULL
#define TG_DVFS_HEARTBEAT_US        1000000ULL
#define TG_DVFS_ROLLBACK_COOLDOWN_US 2000000ULL
#define TG_DVFS_THROTTLE_ENTER_C         80
#define TG_DVFS_THROTTLE_RELEASE_C       72
#define TG_DVFS_UNKNOWN_EMC_KHZ     3200000u

static ULONGLONG query_now_us(TG_GPU_DVFS *d)
{
    LARGE_INTEGER freq = {0};
    LARGE_INTEGER ticks = KeQueryPerformanceCounter(&freq);
    ULONGLONG f = d->QpcFreq ? d->QpcFreq : (ULONGLONG)freq.QuadPart;
    ULONGLONG t = (ULONGLONG)ticks.QuadPart;
    if (!f) f = 19200000ULL;
    if (!d->QpcFreq) d->QpcFreq = f;
    return (t / f) * 1000000ULL + ((t % f) * 1000000ULL) / f;
}

static UINT32 snap_min_opp(UINT32 khz)
{
    UINT32 i;
    if (!khz) khz = TG_GPU_HIOPT_IDLE_KHZ;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i)
        if (tg_gpu_clock_hiopt_freq_khz(i) >= khz) return i;
    return TG_GPU_HIOPT_OPP_COUNT - 1u;
}

static UINT32 snap_max_opp(UINT32 khz)
{
    UINT32 i, best = 0;
    if (!khz) khz = TG_GPU_HIOPT_SAFE_KHZ;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i)
        if (tg_gpu_clock_hiopt_freq_khz(i) <= khz) best = i;
    return best;
}

static BOOLEAN read_param_dword(HANDLE key, PCWSTR value_name, ULONG *out)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    ULONG result_length = 0;
    USHORT chars = 0;
    while (value_name[chars]) ++chars;
    name.Length = (USHORT)(chars * sizeof(WCHAR));
    name.MaximumLength = (USHORT)((chars + 1u) * sizeof(WCHAR));
    name.Buffer = (PWSTR)value_name;
    if (!NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                                    buffer, sizeof(buffer), &result_length)))
        return FALSE;
    if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG))
        return FALSE;
    RtlCopyMemory(out, info->Data, sizeof(ULONG));
    return TRUE;
}

static VOID load_registry_overrides(TG_GPU_DVFS *d)
{
    static WCHAR params_path[] =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\Parameters";
    UNICODE_STRING path = { sizeof(params_path) - sizeof(WCHAR), sizeof(params_path), params_path };
    OBJECT_ATTRIBUTES attrs;
    HANDLE key = NULL;
    ULONG v = 0;
    d->RegMode = TgGpuDvfsFixed;
    d->RegMinKhz = TG_GPU_HIOPT_IDLE_KHZ;
    d->RegMaxKhz = TG_GPU_HIOPT_SAFE_KHZ;
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_QUERY_VALUE, &attrs)))
        return;
    if (read_param_dword(key, L"GpuDvfsMode", &v))
        d->RegMode = v ? TgGpuDvfsHighOpt : TgGpuDvfsFixed;
    if (read_param_dword(key, L"GpuDvfsMinKhz", &v) && v)
        d->RegMinKhz = v;
    if (read_param_dword(key, L"GpuDvfsMaxKhz", &v) && v)
        d->RegMaxKhz = v;
    (VOID)ZwClose(key);
}

static VOID report_telemetry(TG_GPU_DVFS *d, const TG_GPU_HARDWARE *h)
{
    struct tg_gpu_pmic_request op;
    UINT32 opp, temp_byte, status_word;
    UINT32 rail_uv = 0;
    if (!d->Platform || !d->PolicyBridgeSupported) return;
    op = (struct tg_gpu_pmic_request){0, TG_GPU_PMIC_REPORT_ABI, 1, TG_GPU_LIVE_COUNTER_ABI};
    (VOID)TgGpuPlatformTryRequest(d->Platform, &op);
    if (d->EmcVoteSupported) {
        op = (struct tg_gpu_pmic_request){0, TG_GPU_PMIC_REPORT_EMC_ABI, 1, TG_GPU_EMC_VOTE_ABI};
        (VOID)TgGpuPlatformTryRequest(d->Platform, &op);
    }
    if (h->Power.pmic_address) {
        op = (struct tg_gpu_pmic_request){ h->Power.pmic_address, 0x23u, 0, 0 };
        if (NT_SUCCESS(TgGpuPlatformTryRequest(d->Platform, &op)) && op.value <= 0xffu)
            rail_uv = 250000u + op.value * 5000u;
    }
    opp = tg_gpu_clock_hiopt_index(h->Clock.target_khz);
    if (opp == UINT32_MAX) opp = 0x3fu;
    temp_byte = d->TempValid ? (UINT32)((UINT8)d->TempC) : 0xffu;
    status_word = TG_GPU_DVFS_STATUS_PACK(
        d->Mode == TgGpuDvfsHighOpt && !InterlockedCompareExchange((volatile LONG *)&d->StablePowerRequested, 0, 0),
        d->ThermalThrottled, opp, temp_byte, d->Transitions);
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_REPORT_GPU_KHZ, 1, h->Clock.measured_khz };
    (VOID)TgGpuPlatformTryRequest(d->Platform, &op);
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_REPORT_GPU_UV, 1, rail_uv };
    (VOID)TgGpuPlatformTryRequest(d->Platform, &op);
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_REPORT_GPU_STATUS, 1, status_word };
    (VOID)TgGpuPlatformTryRequest(d->Platform, &op);
}

static VOID report_emc_failure(TG_GPU_DVFS *d)
{
    struct tg_gpu_pmic_request op = {0, TG_GPU_PMIC_REPORT_EMC_FAILURE, 1, d->EmcFailedWord};
    NTSTATUS status = TgGpuPlatformTryVoteRequest(d->Platform, &op, &d->EmcProviderGeneration);
    d->EmcFailureReportPending = !NT_SUCCESS(status) && status != STATUS_NOT_SUPPORTED &&
        status != STATUS_INVALID_DEVICE_REQUEST && status != STATUS_INVALID_PARAMETER;
}

static VOID poll_platform_state(TG_GPU_DVFS *d, ULONGLONG now, BOOLEAN initial)
{
    struct tg_gpu_pmic_request op;
    if (!d->Platform) return;
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_QUERY_DVFS_POLICY, 0, 0 };
    if (NT_SUCCESS(TgGpuPlatformTryRequest(d->Platform, &op))) {
        UINT32 pmode = TG_GPU_DVFS_POLICY_MODE(op.value);
        UINT32 pmin = TG_GPU_DVFS_POLICY_MIN_KHZ(op.value);
        UINT32 pmax = TG_GPU_DVFS_POLICY_MAX_KHZ(op.value);
        UINT32 pseq = TG_GPU_DVFS_POLICY_SEQ(op.value);
        d->PolicyBridgeSupported = TRUE;
        if (pmode != 0 || pseq != 0 || d->PolicyOverrideActive) {
            d->PolicyOverrideActive = TRUE;
            d->PolicySeq = pseq;
            d->Mode = pmode == TgGpuDvfsHighOpt ? TgGpuDvfsHighOpt : TgGpuDvfsFixed;
            if (d->Mode == TgGpuDvfsHighOpt) {
                d->MinOpp = snap_min_opp(pmin ? pmin : TG_GPU_HIOPT_IDLE_KHZ);
                d->MaxOpp = snap_max_opp(pmax ? pmax : TG_GPU_HIOPT_SAFE_KHZ);
                if (d->MinOpp > d->MaxOpp) d->MinOpp = d->MaxOpp;
            }
        }
    }
    if (initial || d->Mode == TgGpuDvfsHighOpt) {
        op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_QUERY_TEMP, 0, 0 };
        if (NT_SUCCESS(TgGpuPlatformTryRequest(d->Platform, &op))) {
            INT32 t = (INT32)op.value;
            if (t >= -40 && t <= 120) {
                d->TempValid = TRUE;
                d->TempC = t;
                d->ThermalRange = tg_gpu_clock_thermal_range(t, d->ThermalRange);
                if (t >= TG_DVFS_THROTTLE_ENTER_C)
                    d->ThermalThrottled = TRUE;
                else if (d->ThermalThrottled && t <= TG_DVFS_THROTTLE_RELEASE_C)
                    d->ThermalThrottled = FALSE;
            } else {
                d->TempValid = FALSE;
                d->ThermalRange = 0;
            }
        } else {
            d->TempValid = FALSE;
            d->ThermalRange = 0;
        }
        op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_QUERY_EMC_KHZ, 0, 0 };
        if (NT_SUCCESS(TgGpuPlatformTryRequest(d->Platform, &op)) &&
            op.value >= 204000u && op.value <= 3200000u)
            d->EmcKhz = op.value;
        /* Retain the last confirmed EMC clock on a transient read failure.
         * Before the first sample, cover the characterized maximum instead
         * of assuming the old 1600 MHz boot profile is still active. */
    }
    op = (struct tg_gpu_pmic_request){0, TG_GPU_PMIC_QUERY_EMC_VOTE, 0, 0};
    ULONG generation;
    if (NT_SUCCESS(TgGpuPlatformTryVoteRequest(d->Platform, &op, &generation))) {
        UINT32 floor = TG_GPU_EMC_VOTE_UV(op.value);
        if (generation != d->EmcProviderGeneration) {
            /* The new provider starts its sequence at one. A higher vote may
             * have reached the previous owner just before its removal. */
            if (d->EmcFloorUv > d->EmcRetainedFloorUv)
                d->EmcRetainedFloorUv = d->EmcFloorUv;
            d->EmcProviderGeneration = generation;
            d->EmcAckWord = d->EmcFailedWord = 0;
            d->EmcFailureReportPending = FALSE;
            d->EmcVoteWord = 0;
            d->EmcFloorUv = d->EmcRetainedFloorUv;
        }
        if (op.value && op.value == d->EmcFailedWord) {
            /* The provider validates the generation under its PMIC lock.
             * A status replay only records the earlier quiescent rejection. */
            if (d->EmcFailureReportPending)
                report_emc_failure(d);
        } else if (!floor || (floor >= 590000u && floor <= 760000u)) {
            d->EmcVoteSupported = TRUE;
            d->EmcFailedWord = 0;
            d->EmcFailureReportPending = FALSE;
            d->EmcVoteWord = op.value;
            /* A zero word is an empty mailbox. Releasing a floor uses a
             * nonzero sequence with a zero voltage code. */
            d->EmcFloorUv = op.value ? floor : d->EmcRetainedFloorUv;
        }
    }
    d->LastPollUs = now;
}

/* DispatchGate held and the engine quiescent. A PMIC read alone cannot
 * replace NAPLL recalibration; call only after a successful clock operation. */
static VOID acknowledge_emc(TG_GPU_DVFS *d, const TG_GPU_HARDWARE *h)
{
    struct tg_gpu_pmic_request op;
    if (!d->EmcVoteSupported || !d->EmcVoteWord || d->EmcVoteWord == d->EmcAckWord ||
        !h->Clock.running || h->Clock.rail_uv < d->EmcFloorUv) return;
    op = (struct tg_gpu_pmic_request){0, TG_GPU_PMIC_REPORT_EMC_ACK, 1, d->EmcVoteWord};
    if (NT_SUCCESS(TgGpuPlatformTryVoteRequest(d->Platform, &op, &d->EmcProviderGeneration))) {
        d->EmcAckWord = d->EmcVoteWord;
        d->EmcRetainedFloorUv = d->EmcFloorUv;
    }
}

static VOID reject_emc(TG_GPU_DVFS *d)
{
    if (d->EmcVoteWord && d->EmcVoteWord != d->EmcAckWord) {
        d->EmcFailedWord=d->EmcVoteWord;
        report_emc_failure(d);
        /* No successful ack authorized an EMC increase. Keep the last
         * established vote and do not retry this failed generation forever. */
        d->EmcVoteWord=d->EmcAckWord;
        d->EmcFloorUv=d->EmcRetainedFloorUv;
    }
}

VOID TgGpuDvfsAttach(TG_GPU_HARDWARE *h, TG_GPU_PLATFORM *p, TG_GPU_WORK_QUEUE *q)
{
    TG_GPU_DVFS *d;
    ULONGLONG now;
    BOOLEAN locked = FALSE;
    PAGED_CODE();
    if (!h || !p || !h->Ready || !h->Clock.owned || !h->Clock.running)
        return;
    if (q && q->Engine) {
        if (KeWaitForSingleObject(&q->DispatchGate, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS)
            locked = TRUE;
    }
    d = &h->Dvfs;
    RtlZeroMemory(d, sizeof(*d));
    d->Platform = p;
    d->BootKhz = h->Clock.target_khz;
    d->BootUv = h->Clock.rail_uv;
    d->SafeOpp = snap_max_opp(TG_GPU_HIOPT_SAFE_KHZ);
    d->EmcKhz = TG_DVFS_UNKNOWN_EMC_KHZ;
    d->ThermalRange = TG_GPU_THERMAL_RANGES;
    load_registry_overrides(d);
    d->Mode = d->RegMode;
    d->MinOpp = snap_min_opp(d->RegMinKhz);
    d->MaxOpp = snap_max_opp(d->RegMaxKhz);
    if (d->MinOpp > d->MaxOpp) d->MinOpp = d->MaxOpp;
    now = query_now_us(d);
    d->LastBusyUs = now;
    poll_platform_state(d, now, TRUE);
    report_telemetry(d, h);
    d->LastReportUs = now;
    (VOID)InterlockedExchange(&d->Configured, 1);
    if (locked) {
        (VOID)KeReleaseMutex(&q->DispatchGate, FALSE);
        (VOID)KeSetEvent(&q->Wake, IO_NO_INCREMENT, FALSE);
    }
}

PLARGE_INTEGER TgGpuDvfsWaitTimeout(const TG_GPU_DVFS *d, LARGE_INTEGER *storage)
{
    if (!d || !storage || !InterlockedCompareExchange((volatile LONG *)&d->Configured, 0, 0))
        return NULL;
    if (d->Mode == TgGpuDvfsHighOpt) {
        /* 200 ms idle holdoff / policy poll while HighOPT DVFS is active. */
        storage->QuadPart = -2000000LL;
        return storage;
    }
    if (d->PolicyBridgeSupported) {
        /* Lightweight 500 ms policy check when SwitchFan supports Power Modes. */
        storage->QuadPart = -5000000LL;
        return storage;
    }
    return NULL;
}

VOID TgGpuDvfsNoteServiceBegin(TG_GPU_DVFS *d)
{
    if (!d || !InterlockedCompareExchange(&d->Configured, 0, 0)) return;
    d->LastServiceStartUs = query_now_us(d);
}

VOID TgGpuDvfsNoteServiceComplete(TG_GPU_DVFS *d)
{
    ULONGLONG now;
    if (!d || !InterlockedCompareExchange(&d->Configured, 0, 0)) return;
    now = query_now_us(d);
    if (d->LastServiceStartUs && now > d->LastServiceStartUs &&
        (now - d->LastServiceStartUs) >= TG_DVFS_HEAVY_SLICE_US && d->BusyStreak < 16u)
        d->BusyStreak += 2u;
    d->LastBusyUs = now;
}

VOID TgGpuDvfsSetStablePower(TG_GPU_DVFS *d, BOOLEAN enabled)
{
    if (!d) return;
    (VOID)InterlockedExchange(&d->StablePowerRequested, enabled ? 1 : 0);
}

NTSTATUS TgGpuDvfsStep(TG_GPU_HARDWARE *h, TG_GPU_WORK_QUEUE *q, BOOLEAN woke_for_work)
{
    TG_GPU_DVFS *d;
    TG_GPU_CONTEXT_ENGINE *e;
    ULONGLONG now;
    KIRQL q_irql, e_irql;
    LIST_ENTRY *link;
    UINT32 pending_depth = 0, want_khz, want_uv;
    BOOLEAN busy_now, stable, quiescent, pending_arrived, unchanged, emc_pending;
    enum tg_gpu_clock_result res;
    NTSTATUS wait;
    PAGED_CODE();
    if (!h || !q) return STATUS_INVALID_PARAMETER;
    d = &h->Dvfs;
    if (!InterlockedCompareExchange(&d->Configured, 0, 0))
        return STATUS_SUCCESS;
    e = q->Engine;
    if (!e || !h->Ready) return STATUS_SUCCESS;

    now = query_now_us(d);
    stable = InterlockedCompareExchange(&d->StablePowerRequested, 0, 0) != 0;
    if (d->Mode == TgGpuDvfsFixed && !stable && d->EmcVoteWord == d->EmcAckWord &&
        h->Clock.target_khz == d->BootKhz && h->Clock.rail_uv == d->BootUv &&
        (!d->PolicyBridgeSupported || (now - d->LastPollUs) < TG_DVFS_POLL_INTERVAL_US))
        return STATUS_SUCCESS;

    if (!d->LastPollUs || (now - d->LastPollUs) >= TG_DVFS_POLL_INTERVAL_US)
        poll_platform_state(d, now, FALSE);
    emc_pending = d->EmcVoteWord != d->EmcAckWord;

    KeAcquireSpinLock(&q->Lock, &q_irql);
    for (link = q->Pending.Flink; link != &q->Pending && pending_depth < 4u; link = link->Flink)
        ++pending_depth;
    busy_now = q->Current.Engine != NULL || pending_depth > 0 || woke_for_work;
    KeReleaseSpinLock(&q->Lock, q_irql);

    if (busy_now) {
        UINT32 add = pending_depth >= 2u ? 2u : 1u;
        if (now >= d->LastBusyUs && (now - d->LastBusyUs) <= TG_DVFS_BURST_WINDOW_US)
            d->BusyStreak = d->BusyStreak + add <= 16u ? d->BusyStreak + add : 16u;
        else
            d->BusyStreak = pending_depth >= 2u ? 3u : 1u;
        d->LastBusyUs = now;
    } else if (now >= d->LastBusyUs && (now - d->LastBusyUs) >= TG_DVFS_IDLE_HOLDOFF_US) {
        d->BusyStreak = 0;
    }

    if (d->Mode == TgGpuDvfsFixed || stable) {
        want_khz = d->BootKhz;
        want_uv = d->BootUv;
    } else {
        UINT32 eff_max = d->MaxOpp;
        UINT32 eff_min, cur_opp, next_opp;
        if (!d->TempValid && eff_max > d->SafeOpp)
            eff_max = d->SafeOpp;
        while (eff_max && tg_gpu_clock_hiopt_uv(h->Clock.gpu_speedo,
               tg_gpu_clock_hiopt_freq_khz(eff_max), d->ThermalRange, d->EmcKhz) == UINT32_MAX)
            --eff_max;
        if (d->ThermalThrottled)
            eff_max = d->MinOpp <= eff_max ? d->MinOpp : eff_max;
        eff_min = d->MinOpp <= eff_max ? d->MinOpp : eff_max;
        cur_opp = tg_gpu_clock_hiopt_index(h->Clock.target_khz);
        if (cur_opp == UINT32_MAX) cur_opp = d->SafeOpp;
        next_opp = cur_opp;
        if (d->ThermalThrottled) {
            next_opp = eff_min;
        } else if (busy_now) {
            UINT32 boost = d->SafeOpp < eff_max ? d->SafeOpp : eff_max;
            if (boost < eff_min) boost = eff_min;
            if (cur_opp < boost)
                next_opp = boost;
            else if (d->BusyStreak >= 3u && cur_opp < eff_max)
                next_opp = cur_opp + 2u <= eff_max ? cur_opp + 2u : eff_max;
        } else if (now >= d->LastBusyUs && (now - d->LastBusyUs) >= TG_DVFS_IDLE_HOLDOFF_US) {
            if (cur_opp > eff_min)
                next_opp = cur_opp > eff_min + 3u ? cur_opp - 3u : eff_min;
        }
        if (next_opp < eff_min) next_opp = eff_min;
        if (next_opp > eff_max) next_opp = eff_max;
        want_khz = tg_gpu_clock_hiopt_freq_khz(next_opp);
        want_uv = tg_gpu_clock_hiopt_uv(h->Clock.gpu_speedo, want_khz, d->ThermalRange, d->EmcKhz);
        if (want_uv == UINT32_MAX) {
            want_khz = d->BootKhz;
            want_uv = d->BootUv;
        }
    }

    if (want_uv < d->EmcFloorUv) want_uv = d->EmcFloorUv;
    unchanged = want_khz == h->Clock.target_khz && want_uv == h->Clock.rail_uv;
    if (unchanged && !emc_pending && (now - d->LastReportUs) < TG_DVFS_HEARTBEAT_US)
        return STATUS_SUCCESS;
    if (!unchanged && !emc_pending && now < d->CooldownUntilUs && !d->ThermalThrottled && d->Mode == TgGpuDvfsHighOpt)
        return STATUS_SUCCESS;

    wait = KeWaitForSingleObject(&q->DispatchGate, Executive, KernelMode, FALSE, NULL);
    if (wait != STATUS_SUCCESS)
        return NT_SUCCESS(wait) ? STATUS_SUCCESS : wait;
    if (!InterlockedCompareExchange(&d->Configured, 0, 0)) {
        (VOID)KeReleaseMutex(&q->DispatchGate, FALSE);
        return STATUS_SUCCESS;
    }

    KeAcquireSpinLock(&q->Lock, &q_irql);
    KeAcquireSpinLock(&e->Lock, &e_irql);
    pending_arrived = !IsListEmpty(&q->Pending);
    quiescent = h->Ready && h->Power.owned && h->Power.ready &&
                h->Clock.owned && h->Clock.running &&
                q->Engine == e && !q->Paused && NT_SUCCESS(q->Failure) &&
                !q->Servicing && !q->Current.Engine &&
                !e->Stopping && !e->RecoveryPending && !e->Resetting &&
                !e->Active && !e->Notification && !e->Channel.referenced;
    KeReleaseSpinLock(&e->Lock, e_irql);
    KeReleaseSpinLock(&q->Lock, q_irql);

    if (!quiescent || (pending_arrived && want_khz < h->Clock.target_khz &&
                       !d->ThermalThrottled && d->Mode == TgGpuDvfsHighOpt)) {
        (VOID)KeReleaseMutex(&q->DispatchGate, FALSE);
        return STATUS_SUCCESS;
    }

    if (unchanged) {
        /* GPCCLK measurement temporarily overrides slowdown. Do it only at
         * the same idle/ownership boundary as rate changes, never in flight.
         * A telemetry failure is unavailable data, not a new clock transition. */
        res = tg_gpu_clock_measure(&h->Io, &h->Clock);
        if (res != TG_GPU_CLOCK_OK) { h->Clock.measured_khz = 0; reject_emc(d); }
        else acknowledge_emc(d, h);
        h->Status.measured_khz = h->Clock.measured_khz;
        (VOID)KeReleaseMutex(&q->DispatchGate, FALSE);
        report_telemetry(d, h);
        d->LastReportUs = now;
        return STATUS_SUCCESS;
    }

    res = tg_gpu_clock_set_rate(&h->Io, &h->Power, want_khz, want_uv, &h->Clock);
    h->Status.target_khz = h->Clock.target_khz;
    h->Status.measured_khz = h->Clock.measured_khz;
    h->Status.rail_uv = h->Clock.rail_uv;
    h->Status.clock_result = res;
    if (res == TG_GPU_CLOCK_OK) {
        ++d->Transitions;
        acknowledge_emc(d, h);
    } else if (h->Clock.running) {
        reject_emc(d);
        ++d->Rollbacks;
        d->CooldownUntilUs = now + TG_DVFS_ROLLBACK_COOLDOWN_US;
    } else reject_emc(d);
    (VOID)KeReleaseMutex(&q->DispatchGate, FALSE);

    report_telemetry(d, h);
    d->LastReportUs = now;
    return h->Clock.running ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}
