/* SPDX-License-Identifier: MIT */
/* Compile with the real Windows SDK, separately from LLVM-MinGW. */
#include <windows.h>
#include <d3dkmthk.h>
#include "kmt_abi.h"
#define SAME_SIZE(t,k) _Static_assert(sizeof(struct t)==sizeof(k), #t " size")
#define SAME_FIELD(t,m,k,n) _Static_assert(offsetof(struct t,m)==offsetof(k,n), #t "." #m)
SAME_SIZE(tg_kmt_luid,LUID);
SAME_FIELD(tg_kmt_luid,low,LUID,LowPart);
SAME_FIELD(tg_kmt_luid,high,LUID,HighPart);
SAME_SIZE(tg_kmt_adapter,D3DKMT_ADAPTERINFO);
SAME_FIELD(tg_kmt_adapter,handle,D3DKMT_ADAPTERINFO,hAdapter);
SAME_FIELD(tg_kmt_adapter,luid,D3DKMT_ADAPTERINFO,AdapterLuid);
SAME_FIELD(tg_kmt_adapter,sources,D3DKMT_ADAPTERINFO,NumOfSources);
SAME_FIELD(tg_kmt_adapter,precise_present_regions,D3DKMT_ADAPTERINFO,bPrecisePresentRegionsPreferred);
SAME_SIZE(tg_kmt_enum,D3DKMT_ENUMADAPTERS3);
SAME_FIELD(tg_kmt_enum,filter,D3DKMT_ENUMADAPTERS3,Filter);
SAME_FIELD(tg_kmt_enum,count,D3DKMT_ENUMADAPTERS3,NumAdapters);
SAME_FIELD(tg_kmt_enum,adapters,D3DKMT_ENUMADAPTERS3,pAdapters);
SAME_SIZE(tg_kmt_query,D3DKMT_QUERYADAPTERINFO);
SAME_FIELD(tg_kmt_query,handle,D3DKMT_QUERYADAPTERINFO,hAdapter);
SAME_FIELD(tg_kmt_query,type,D3DKMT_QUERYADAPTERINFO,Type);
SAME_FIELD(tg_kmt_query,data,D3DKMT_QUERYADAPTERINFO,pPrivateDriverData);
SAME_FIELD(tg_kmt_query,bytes,D3DKMT_QUERYADAPTERINFO,PrivateDriverDataSize);
SAME_SIZE(tg_kmt_close,D3DKMT_CLOSEADAPTER);
SAME_FIELD(tg_kmt_close,handle,D3DKMT_CLOSEADAPTER,hAdapter);
_Static_assert(TG_KMT_UMDRIVERPRIVATE==KMTQAITYPE_UMDRIVERPRIVATE,"KMT query type");
