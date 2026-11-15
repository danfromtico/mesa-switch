/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_WINDOWS_DVFS_H
#define TG_GPU_WINDOWS_DVFS_H
#include "resources.h"
#include <stdint.h>

struct _TG_GPU_PLATFORM;
struct _TG_GPU_HARDWARE;
struct _TG_GPU_WORK_QUEUE;

enum tg_gpu_dvfs_mode {
    TgGpuDvfsFixed = 0,
    TgGpuDvfsHighOpt = 1,
};

typedef struct _TG_GPU_DVFS {
    struct _TG_GPU_PLATFORM *Platform;
    volatile LONG Configured;
    volatile LONG StablePowerRequested;
    UINT32 Mode;
    UINT32 RegMode;
    UINT32 RegMinKhz;
    UINT32 RegMaxKhz;
    UINT32 BootKhz;
    UINT32 BootUv;
    UINT32 MinOpp;
    UINT32 MaxOpp;
    UINT32 SafeOpp;
    UINT32 EmcKhz;
    UINT32 EmcVoteWord;
    UINT32 EmcAckWord;
    UINT32 EmcFailedWord;
    UINT32 EmcFloorUv;
    INT32 TempC;
    UINT32 ThermalRange;
    UINT32 PolicySeq;
    UINT32 BusyStreak;
    UINT32 Transitions;
    UINT32 Rollbacks;
    BOOLEAN PolicyBridgeSupported;
    BOOLEAN EmcVoteSupported;
    BOOLEAN PolicyOverrideActive;
    BOOLEAN TempValid;
    BOOLEAN ThermalThrottled;
    ULONGLONG QpcFreq;
    ULONGLONG LastPollUs;
    ULONGLONG LastBusyUs;
    ULONGLONG LastServiceStartUs;
    ULONGLONG LastReportUs;
    ULONGLONG CooldownUntilUs;
} TG_GPU_DVFS;

/* PASSIVE attach after cold TgGpuStartHardware succeeds. Keeps default fixed
 * 460.8 MHz / 800 mV behavior unless SwitchFan Power Mode or explicit
 * tegragpurender\Parameters\GpuDvfsMode enables T210B01 HighOPT DVFS.
 */
VOID TgGpuDvfsAttach(struct _TG_GPU_HARDWARE *Hardware,
    struct _TG_GPU_PLATFORM *Platform, struct _TG_GPU_WORK_QUEUE *Queue);
PLARGE_INTEGER TgGpuDvfsWaitTimeout(const TG_GPU_DVFS *Dvfs, LARGE_INTEGER *Storage);
VOID TgGpuDvfsNoteServiceBegin(TG_GPU_DVFS *Dvfs);
VOID TgGpuDvfsNoteServiceComplete(TG_GPU_DVFS *Dvfs);
VOID TgGpuDvfsSetStablePower(TG_GPU_DVFS *Dvfs, BOOLEAN Enabled);
/* PASSIVE worker hook. Acquires Queue->DispatchGate and refuses to reprogram
 * clocks unless the queue, engine and channels are completely quiescent.
 */
NTSTATUS TgGpuDvfsStep(struct _TG_GPU_HARDWARE *Hardware,
    struct _TG_GPU_WORK_QUEUE *Queue, BOOLEAN WokeForWork);

#endif
