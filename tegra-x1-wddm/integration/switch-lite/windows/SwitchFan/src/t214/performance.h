// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef LITE_PERFORMANCE_H
#define LITE_PERFORMANCE_H
#define LITE_PERF_ADDRESS 0xA9FB0000UL
#define LITE_PERF_MAGIC   0x4652504CUL
#define LITE_PERF_VERSION 1
#define LITE_CPU_KHZ      1996800U
#define LITE_RAM_KHZ      2100000U
enum {
  LitePerfDisabled = 0, LitePerfReady = 1, LitePerfBadConfig = 2,
  LitePerfNoBpmp = 3, LitePerfNoEmc = 4, LitePerfNoTiming = 5,
  LitePerfCoreVoltage = 6, LitePerfClockError = 7, LitePerfRateMismatch = 8
};
typedef struct {
  unsigned int Magic, Version, CpuKhz, RamKhz;
  unsigned int RamActualKhz, Status, PmicAddress, CoreMv;
  unsigned int BpmpError, BootPllx, BootPolicy, BpmpChannel;
} LITE_PERFORMANCE;
#endif
