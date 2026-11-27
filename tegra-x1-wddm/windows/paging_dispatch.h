/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PAGING_DISPATCH_H
#define TG_GPU_PAGING_DISPATCH_H
#include "context.h"
#include "paging.h"
DXGKDDI_BUILDPAGINGBUFFER TgGpuDdiBuildPagingBuffer;
NTSTATUS TgGpuBuildPagingRequest(HANDLE Adapter, DXGKARG_BUILDPAGINGBUFFER *Args,
                                TG_GPU_PAGING_REJECTION *Rejection);
void TgGpuRegisterPagingDdi(DRIVER_INITIALIZATION_DATA *);
#endif
