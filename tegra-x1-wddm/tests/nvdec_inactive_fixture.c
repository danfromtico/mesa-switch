/* SPDX-License-Identifier: MIT */
/* Existing GPU-only controls create no video service. Its active teardown
 * and clock callback ordering have a separate, explicit adapter control. */
#include "../windows/adapter.h"
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(ULONG);
NTSTATUS TgNvdecAdapterStop(TG_GPU_ADAPTER *a)
{
    if (!a || a->Video.Initialized || a->Video.Backend || a->Video.Clock || a->Video.Device)
        ExitProcess(__LINE__);
    return STATUS_SUCCESS;
}
NTSTATUS TgNvdecAdapterShutdown(TG_GPU_ADAPTER *a)
{
    return TgNvdecAdapterStop(a);
}
